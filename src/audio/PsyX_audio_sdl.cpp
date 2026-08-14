#include "PsyX_audio_internal.h"
#include "PsyX_audio_sink.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace psyx::audio
{

class SdlSink final : public AudioSink
{
public:
	~SdlSink() override
	{
		stop();
	}

	PsyXAudioResult start(const SinkStartParams& params) override
	{
		uint32_t outputRate = 0;
		const uint32_t rates[] = {352800u, 176400u, 88200u, 44100u};
		for (uint32_t rate : rates)
			if ((params.config.allowed_rate_mask & rateMaskFor(rate)) != 0 &&
				rate <= params.sourceRate && params.sourceRate % rate == 0)
			{
				outputRate = rate;
				break;
			}
		if (outputRate == 0 ||
			(params.config.allowed_container_mask & PSYX_AUDIO_CONTAINER_S16) == 0)
		{
			params.shared->setFailure(
				PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT,
				0,
				"SDL requires a supported 44.1kHz-family signed 16-bit format");
			return PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT;
		}

		shared_ = params.shared;
		renderCallback_ = params.renderCallback;
		renderCallbackFloat64_ = params.renderCallbackFloat64;
		renderUser_ = params.renderUser;
		sourceRate_ = params.sourceRate;
		dither_ = params.dither;
		outputRate_ = outputRate;

		if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0)
		{
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
			{
				shared_->setFailure(PSYX_AUDIO_ERROR_BACKEND, 0, SDL_GetError());
				return PSYX_AUDIO_ERROR_BACKEND;
			}
			ownsAudioSubsystem_ = true;
		}

		SDL_AudioSpec wanted{};
		wanted.freq = static_cast<int>(outputRate_);
		wanted.format = AUDIO_S16SYS;
		wanted.channels = 2;
		wanted.samples = chooseSamples(outputRate_, params.config.target_latency_ms);
		wanted.callback = &SdlSink::audioCallback;
		wanted.userdata = this;

		const char* deviceName =
			params.config.device_id_utf8 && params.config.device_id_utf8[0]
				? params.config.device_id_utf8
				: nullptr;
		SDL_AudioSpec obtained{};
		device_ = SDL_OpenAudioDevice(deviceName, 0, &wanted, &obtained, 0);
		if (device_ == 0)
		{
			const char* error = SDL_GetError();
			shared_->setFailure(PSYX_AUDIO_ERROR_NO_DEVICE, 0, error);
			releaseSubsystem();
			return PSYX_AUDIO_ERROR_NO_DEVICE;
		}

		if (obtained.freq != static_cast<int>(outputRate_) || obtained.format != AUDIO_S16SYS ||
			obtained.channels != 2)
		{
			shared_->setFailure(
				PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT,
				0,
				"SDL returned a non-canonical output format");
			SDL_CloseAudioDevice(device_);
			device_ = 0;
			releaseSubsystem();
			return PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT;
		}

		const uint32_t effectiveRingFrames = std::max<uint32_t>(
			params.config.ring_capacity_frames,
			static_cast<uint32_t>(obtained.samples) * 2u + 1u);
		pump_ = std::make_unique<RenderPump>(
			effectiveRingFrames,
			renderCallback_,
			renderCallbackFloat64_,
			renderUser_,
			sourceRate_,
			*shared_);
		packer_ = std::make_unique<OutputPacker>(
			*pump_, PackedFormat::S16, outputRate_, dither_);
		targetLeadFrames_ = std::min<uint32_t>(
			effectiveRingFrames / 2,
			std::max<uint32_t>(obtained.samples, 256u));
		stopping_.store(false, std::memory_order_release);

		shared_->setOpened(
			PSYX_AUDIO_BACKEND_SDL,
			PSYX_AUDIO_MODE_SHARED,
			PSYX_AUDIO_FORMAT_S16,
			outputRate_,
			16,
			16,
			renderCallbackFloat64_ ? sourceRate_ / outputRate_ : outputRate_ / sourceRate_,
			false,
			true,
			deviceName ? deviceName : "",
			deviceName ? deviceName : "SDL default output");
		shared_->setState(PSYX_AUDIO_STATE_PRIMING);
		pump_->fillTo(targetLeadFrames_);
		shared_->setState(PSYX_AUDIO_STATE_RUNNING);
		SDL_PauseAudioDevice(device_, 0);
		return PSYX_AUDIO_OK;
	}

	void stop() override
	{
		if (device_ != 0)
		{
			stopping_.store(true, std::memory_order_release);
			SDL_PauseAudioDevice(device_, 1);
			SDL_CloseAudioDevice(device_);
			device_ = 0;
		}
		packer_.reset();
		pump_.reset();
		releaseSubsystem();
	}

private:
	static Uint16 chooseSamples(uint32_t rate, uint32_t latencyMs)
	{
		const uint32_t requested = std::clamp(
			static_cast<uint32_t>((static_cast<uint64_t>(rate) * latencyMs) / 1000ull),
			256u,
			4096u);
		uint32_t samples = 256;
		while (samples < requested)
			samples <<= 1;
		return static_cast<Uint16>(samples);
	}

	static void audioCallback(void* userdata, Uint8* stream, int length)
	{
		static_cast<SdlSink*>(userdata)->render(stream, length);
	}

	void render(Uint8* stream, int length)
	{
		if (stopping_.load(std::memory_order_acquire) || !pump_ || !packer_)
		{
			std::memset(stream, 0, static_cast<size_t>(std::max(length, 0)));
			return;
		}

		const uint32_t frames = static_cast<uint32_t>(length) / 4u;
		const uint64_t target =
			pump_->consumedFrames() +
			packer_->sourceFramesNeeded(frames + targetLeadFrames_);
		pump_->fillTo(target);
		packer_->pack(stream, frames);
		const size_t writtenBytes = static_cast<size_t>(frames) * 4u;
		if (writtenBytes < static_cast<size_t>(length))
			std::memset(stream + writtenBytes, 0, static_cast<size_t>(length) - writtenBytes);
		shared_->deviceFramesSubmitted.fetch_add(frames, std::memory_order_relaxed);
	}

	void releaseSubsystem()
	{
		if (ownsAudioSubsystem_)
		{
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			ownsAudioSubsystem_ = false;
		}
	}

	SDL_AudioDeviceID device_ = 0;
	bool ownsAudioSubsystem_ = false;
	std::atomic<bool> stopping_{false};
	AudioSharedState* shared_ = nullptr;
	PsyXAudioRenderCallback renderCallback_ = nullptr;
	PsyXAudioRenderCallbackFloat64 renderCallbackFloat64_ = nullptr;
	void* renderUser_ = nullptr;
	uint32_t sourceRate_ = 44100;
	uint32_t outputRate_ = 44100;
	PsyXAudioDither dither_ = PSYX_AUDIO_DITHER_NONE;
	uint32_t targetLeadFrames_ = 0;
	std::unique_ptr<RenderPump> pump_;
	std::unique_ptr<OutputPacker> packer_;
};

std::unique_ptr<AudioSink> createSdlSink()
{
	return std::make_unique<SdlSink>();
}

PsyXAudioResult enumerateSdlDevices(std::vector<PsyXAudioDeviceInfo>& devices)
{
	const bool ownsAudioSubsystem = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0;
	if (ownsAudioSubsystem && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return PSYX_AUDIO_ERROR_BACKEND;

	const int count = SDL_GetNumAudioDevices(0);
	if (count < 0)
	{
		if (ownsAudioSubsystem)
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return PSYX_AUDIO_ERROR_BACKEND;
	}

	for (int i = 0; i < count; ++i)
	{
		const char* name = SDL_GetAudioDeviceName(i, 0);
		if (!name)
			continue;
		PsyXAudioDeviceInfo info{};
		info.backend = PSYX_AUDIO_BACKEND_SDL;
		info.is_default = i == 0 ? 1u : 0u;
		info.state = 1;
		copyText(info.id_utf8, sizeof(info.id_utf8), name);
		copyText(info.name_utf8, sizeof(info.name_utf8), name);
		devices.push_back(info);
	}

	if (ownsAudioSubsystem)
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	return devices.empty() ? PSYX_AUDIO_ERROR_NO_DEVICE : PSYX_AUDIO_OK;
}

} // namespace psyx::audio
