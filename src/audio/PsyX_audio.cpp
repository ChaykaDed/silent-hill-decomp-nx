#include "PsyX/PsyX_audio.h"
#include "PsyX_audio_internal.h"
#include "PsyX_audio_sink.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace psyx::audio
{

class AudioController
{
public:
	~AudioController()
	{
		stop();
	}

	PsyXAudioResult start(
		const PsyXAudioConfig* requested,
		PsyXAudioRenderCallback renderCallback,
		void* renderUser)
	{
		if (!requested || requested->struct_size < sizeof(PsyXAudioConfig) ||
			requested->api_version != PSYX_AUDIO_API_VERSION || !renderCallback)
			return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		std::lock_guard<std::mutex> lock(mutex_);
		if (sink_)
			return PSYX_AUDIO_ERROR_INVALID_STATE;

		config_ = *requested;
		deviceId_ = requested->device_id_utf8 ? requested->device_id_utf8 : "";
		config_.device_id_utf8 = deviceId_.empty() ? nullptr : deviceId_.c_str();
		renderCallback_ = renderCallback;
		renderCallbackFloat64_ = nullptr;
		renderUser_ = renderUser;
		sourceRate_ = 44100;
		dither_ = PSYX_AUDIO_DITHER_NONE;

		const PsyXAudioResult validation = normalizeConfig(config_);
		if (validation != PSYX_AUDIO_OK)
		{
			shared_.reset(config_);
			shared_.setFailure(validation, 0, "Invalid audio configuration");
			return validation;
		}

		shared_.reset(config_);
		return startConfigured();
	}

	PsyXAudioResult startFloat64(
		const PsyXAudioConfig* requested,
		PsyXAudioRenderCallbackFloat64 renderCallback,
		void* renderUser,
		uint32_t sourceRate,
		PsyXAudioDither dither)
	{
		if (!requested || requested->struct_size < sizeof(PsyXAudioConfig) ||
			requested->api_version != PSYX_AUDIO_API_VERSION || !renderCallback ||
			(sourceRate != 176400u && sourceRate != 352800u) ||
			(dither != PSYX_AUDIO_DITHER_NONE && dither != PSYX_AUDIO_DITHER_TPDF))
			return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		std::lock_guard<std::mutex> lock(mutex_);
		if (sink_)
			return PSYX_AUDIO_ERROR_INVALID_STATE;
		config_ = *requested;
		deviceId_ = requested->device_id_utf8 ? requested->device_id_utf8 : "";
		config_.device_id_utf8 = deviceId_.empty() ? nullptr : deviceId_.c_str();
		renderCallback_ = nullptr;
		renderCallbackFloat64_ = renderCallback;
		renderUser_ = renderUser;
		sourceRate_ = sourceRate;
		dither_ = dither;

		const PsyXAudioResult validation = normalizeConfig(config_);
		if (validation != PSYX_AUDIO_OK)
		{
			shared_.reset(config_);
			shared_.setFailure(validation, 0, "Invalid audio configuration");
			return validation;
		}
		const uint32_t unsupportedHigherRates = sourceRate == 176400u
			? (config_.allowed_rate_mask & PSYX_AUDIO_RATE_352800) : 0u;
		if (unsupportedHigherRates != 0)
		{
			shared_.reset(config_);
			shared_.setFailure(PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT, 0,
				"Device rate exceeds the renderer source rate");
			return PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT;
		}
		shared_.reset(config_);
		return startConfigured();
	}

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopConfigured();
	}

	PsyXAudioResult restart()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!renderCallback_ && !renderCallbackFloat64_)
			return PSYX_AUDIO_ERROR_INVALID_STATE;
		stopConfigured();
		shared_.reset(config_);
		return startConfigured();
	}

	void status(PsyXAudioStatus* output)
	{
		if (!output)
			return;
		shared_.snapshot(*output);
	}

	PsyXAudioResult enumerate(
		PsyXAudioBackend backend,
		PsyXAudioDeviceInfo* devices,
		size_t capacity,
		size_t* deviceCount)
	{
		if (!deviceCount)
			return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<PsyXAudioDeviceInfo> found;
		PsyXAudioResult result = PSYX_AUDIO_ERROR_BACKEND;

#ifdef _WIN32
		if (backend == PSYX_AUDIO_BACKEND_AUTO || backend == PSYX_AUDIO_BACKEND_WASAPI)
			result = enumerateWasapiDevices(found);
		else
#endif
		if (backend == PSYX_AUDIO_BACKEND_AUTO || backend == PSYX_AUDIO_BACKEND_SDL)
			result = enumerateSdlDevices(found);
		else
			result = PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		if (result != PSYX_AUDIO_OK)
		{
			*deviceCount = 0;
			return result;
		}

		*deviceCount = found.size();
		if (!devices)
			return PSYX_AUDIO_OK;
		if (capacity < found.size())
			return PSYX_AUDIO_ERROR_OUT_OF_MEMORY;

		std::copy(found.begin(), found.end(), devices);
		return PSYX_AUDIO_OK;
	}

private:
	static PsyXAudioResult normalizeConfig(PsyXAudioConfig& config)
	{
		if (config.backend < PSYX_AUDIO_BACKEND_AUTO || config.backend > PSYX_AUDIO_BACKEND_SDL ||
			config.mode < PSYX_AUDIO_MODE_AUTO || config.mode > PSYX_AUDIO_MODE_EXCLUSIVE_EVENT)
			return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		config.allowed_rate_mask &=
			PSYX_AUDIO_RATE_44100 | PSYX_AUDIO_RATE_88200 |
			PSYX_AUDIO_RATE_176400 | PSYX_AUDIO_RATE_352800;
		config.allowed_container_mask &=
			PSYX_AUDIO_CONTAINER_S16 | PSYX_AUDIO_CONTAINER_S24 |
			PSYX_AUDIO_CONTAINER_S24_IN_32 | PSYX_AUDIO_CONTAINER_S32 |
			PSYX_AUDIO_CONTAINER_F32;

		if (config.allowed_rate_mask == 0 || config.allowed_container_mask == 0)
			return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;

		if ((config.flags & PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION) == 0)
			config.allowed_rate_mask &= PSYX_AUDIO_RATE_44100;

		if (config.target_latency_ms == 0)
			config.target_latency_ms = 20;
		config.target_latency_ms = std::clamp(config.target_latency_ms, 3u, 500u);

		if (config.ring_capacity_frames == 0)
			config.ring_capacity_frames = 8192;
		config.ring_capacity_frames = std::clamp(config.ring_capacity_frames, 2048u, 65536u);

		if ((config.flags & PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT) != 0)
		{
			if (config.backend == PSYX_AUDIO_BACKEND_SDL ||
				config.mode == PSYX_AUDIO_MODE_SHARED)
				return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;
			config.mode = PSYX_AUDIO_MODE_EXCLUSIVE_EVENT;
			config.allowed_rate_mask &= PSYX_AUDIO_RATE_44100;
			if (config.allowed_rate_mask == 0)
				return PSYX_AUDIO_ERROR_INVALID_ARGUMENT;
		}

		return PSYX_AUDIO_OK;
	}

	PsyXAudioResult trySink(
		std::unique_ptr<AudioSink> candidate,
		const PsyXAudioConfig& attemptConfig)
	{
		if (!candidate)
			return PSYX_AUDIO_ERROR_BACKEND;

		shared_.setState(PSYX_AUDIO_STATE_OPENING);
		SinkStartParams params{
			attemptConfig,
			renderCallback_,
			renderCallbackFloat64_,
			renderUser_,
			sourceRate_,
			dither_,
			&shared_
		};
		const PsyXAudioResult result = candidate->start(params);
		if (result == PSYX_AUDIO_OK)
			sink_ = std::move(candidate);
		return result;
	}

	PsyXAudioResult startConfigured()
	{
		PsyXAudioResult result = PSYX_AUDIO_ERROR_BACKEND;
		PsyXAudioConfig attempt = config_;
		const bool wantsWasapi =
			config_.backend == PSYX_AUDIO_BACKEND_AUTO ||
			config_.backend == PSYX_AUDIO_BACKEND_WASAPI;

#ifdef _WIN32
		if (wantsWasapi)
		{
			if (attempt.mode == PSYX_AUDIO_MODE_AUTO)
				attempt.mode =
					(attempt.flags & PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT)
						? PSYX_AUDIO_MODE_EXCLUSIVE_EVENT
						: PSYX_AUDIO_MODE_SHARED;

			result = trySink(createWasapiSink(), attempt);
			if (result == PSYX_AUDIO_OK)
				return sharedResult();

			if (attempt.mode == PSYX_AUDIO_MODE_EXCLUSIVE_EVENT &&
				(config_.fallback_mask & PSYX_AUDIO_FALLBACK_EXCLUSIVE_TO_SHARED) != 0 &&
				(config_.flags & PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT) == 0)
			{
				shared_.recordFallback(result, "WASAPI exclusive failed; using strict shared mode");
				attempt.mode = PSYX_AUDIO_MODE_SHARED;
				result = trySink(createWasapiSink(), attempt);
				if (result == PSYX_AUDIO_OK)
					return sharedResult();
			}
		}
#else
		(void)wantsWasapi;
		if (config_.backend == PSYX_AUDIO_BACKEND_WASAPI)
			result = PSYX_AUDIO_ERROR_BACKEND;
#endif

		const bool requestedSdl =
			config_.backend == PSYX_AUDIO_BACKEND_SDL
#ifndef _WIN32
			|| config_.backend == PSYX_AUDIO_BACKEND_AUTO
#endif
			;
		const bool mayFallbackSdl =
			(config_.fallback_mask & PSYX_AUDIO_FALLBACK_TO_SDL) != 0 &&
			(config_.flags & PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT) == 0;
		if (requestedSdl || mayFallbackSdl)
		{
			if (!requestedSdl)
				shared_.recordFallback(result, "WASAPI failed; using SDL exact-format shared output");
			attempt = config_;
			attempt.backend = PSYX_AUDIO_BACKEND_SDL;
			attempt.mode = PSYX_AUDIO_MODE_SHARED;
			if (!requestedSdl)
				attempt.device_id_utf8 = nullptr;
			result = trySink(createSdlSink(), attempt);
			if (result == PSYX_AUDIO_OK)
				return sharedResult();
		}

		return result;
	}

	PsyXAudioResult sharedResult()
	{
		PsyXAudioStatus status{};
		shared_.snapshot(status);
		return status.fallback_count ? PSYX_AUDIO_OK_FALLBACK : PSYX_AUDIO_OK;
	}

	void stopConfigured()
	{
		if (!sink_)
			return;
		shared_.setState(PSYX_AUDIO_STATE_STOPPING);
		sink_->stop();
		sink_.reset();
		shared_.setState(PSYX_AUDIO_STATE_STOPPED);
	}

	std::mutex mutex_;
	std::unique_ptr<AudioSink> sink_;
	AudioSharedState shared_;
	PsyXAudioConfig config_{};
	std::string deviceId_;
	PsyXAudioRenderCallback renderCallback_ = nullptr;
	PsyXAudioRenderCallbackFloat64 renderCallbackFloat64_ = nullptr;
	void* renderUser_ = nullptr;
	uint32_t sourceRate_ = 44100;
	PsyXAudioDither dither_ = PSYX_AUDIO_DITHER_NONE;
};

AudioController& controller()
{
	static AudioController instance;
	return instance;
}

} // namespace psyx::audio

extern "C"
{

void PsyX_AudioDefaultConfig(PsyXAudioConfig* config)
{
	if (!config)
		return;
	std::memset(config, 0, sizeof(*config));
	config->struct_size = sizeof(*config);
	config->api_version = PSYX_AUDIO_API_VERSION;
	config->backend = PSYX_AUDIO_BACKEND_AUTO;
	config->mode = PSYX_AUDIO_MODE_SHARED;
	config->fallback_mask = PSYX_AUDIO_FALLBACK_TO_SDL;
	config->flags = PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION;
	config->allowed_rate_mask =
		PSYX_AUDIO_RATE_44100 | PSYX_AUDIO_RATE_88200 | PSYX_AUDIO_RATE_176400;
	config->allowed_container_mask =
		PSYX_AUDIO_CONTAINER_S16 | PSYX_AUDIO_CONTAINER_S24 |
		PSYX_AUDIO_CONTAINER_S24_IN_32 | PSYX_AUDIO_CONTAINER_S32;
	config->target_latency_ms = 20;
	config->ring_capacity_frames = 8192;
}

PsyXAudioResult PsyX_AudioStart(
	const PsyXAudioConfig* config,
	PsyXAudioRenderCallback renderCallback,
	void* renderUser)
{
	return psyx::audio::controller().start(config, renderCallback, renderUser);
}

PsyXAudioResult PsyX_AudioStartFloat64(
	const PsyXAudioConfig* config,
	PsyXAudioRenderCallbackFloat64 renderCallback,
	void* renderUser,
	uint32_t sourceRate,
	PsyXAudioDither dither)
{
	return psyx::audio::controller().startFloat64(
		config, renderCallback, renderUser, sourceRate, dither);
}

void PsyX_AudioStop(void)
{
	psyx::audio::controller().stop();
}

PsyXAudioResult PsyX_AudioRestart(void)
{
	return psyx::audio::controller().restart();
}

void PsyX_AudioGetStatus(PsyXAudioStatus* status)
{
	psyx::audio::controller().status(status);
}

PsyXAudioResult PsyX_AudioEnumerateDevices(
	PsyXAudioBackend backend,
	PsyXAudioDeviceInfo* devices,
	size_t capacity,
	size_t* deviceCount)
{
	return psyx::audio::controller().enumerate(backend, devices, capacity, deviceCount);
}

}
