#ifndef PSYX_AUDIO_INTERNAL_H
#define PSYX_AUDIO_INTERNAL_H

#include "PsyX/PsyX_audio.h"
#include "PsyX_audio_convert.h"
#include "PsyX_audio_ring.h"
#include "PsyX_ReferenceResampler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace psyx::audio
{

inline void copyText(char* destination, size_t capacity, const char* source)
{
	if (capacity == 0)
		return;
	if (!source)
	{
		destination[0] = '\0';
		return;
	}
	std::snprintf(destination, capacity, "%s", source);
}

struct AudioSharedState
{
	std::mutex metadataMutex;
	PsyXAudioStatus metadata{};
	std::atomic<uint64_t> canonicalFramesRendered{0};
	std::atomic<uint64_t> deviceFramesSubmitted{0};
	std::atomic<uint64_t> deviceFramesPlayed{0};
	std::atomic<uint64_t> underrunFrames{0};

	void reset(const PsyXAudioConfig& config)
	{
		std::lock_guard<std::mutex> lock(metadataMutex);
		std::memset(&metadata, 0, sizeof(metadata));
		metadata.state = PSYX_AUDIO_STATE_STOPPED;
		metadata.result = PSYX_AUDIO_OK;
		metadata.requested_backend = config.backend;
		metadata.requested_mode = config.mode;
		canonicalFramesRendered.store(0, std::memory_order_relaxed);
		deviceFramesSubmitted.store(0, std::memory_order_relaxed);
		deviceFramesPlayed.store(0, std::memory_order_relaxed);
		underrunFrames.store(0, std::memory_order_relaxed);
	}

	void setState(PsyXAudioState state)
	{
		std::lock_guard<std::mutex> lock(metadataMutex);
		metadata.state = state;
	}

	void setFailure(PsyXAudioResult result, int64_t nativeError, const char* diagnostic)
	{
		std::lock_guard<std::mutex> lock(metadataMutex);
		metadata.state = PSYX_AUDIO_STATE_FAILED;
		metadata.result = result;
		metadata.native_error = nativeError;
		copyText(metadata.diagnostic, sizeof(metadata.diagnostic), diagnostic);
	}

	void recordFallback(PsyXAudioResult reason, const char* diagnostic)
	{
		std::lock_guard<std::mutex> lock(metadataMutex);
		++metadata.fallback_count;
		metadata.fallback_reason = reason;
		metadata.result = PSYX_AUDIO_OK_FALLBACK;
		copyText(metadata.diagnostic, sizeof(metadata.diagnostic), diagnostic);
	}

	void setOpened(
		PsyXAudioBackend backend,
		PsyXAudioMode mode,
		PsyXAudioSampleFormat format,
		uint32_t rate,
		uint16_t containerBits,
		uint16_t validBits,
		uint32_t multiplier,
		bool bitPerfect,
		bool lossless,
		const char* deviceId,
		const char* deviceName)
	{
		std::lock_guard<std::mutex> lock(metadataMutex);
		metadata.active_backend = backend;
		metadata.active_mode = mode;
		metadata.active_format = format;
		metadata.active_rate = rate;
		metadata.container_bits = containerBits;
		metadata.valid_bits = validBits;
		metadata.rate_multiplier = multiplier;
		metadata.bit_perfect = bitPerfect ? 1u : 0u;
		metadata.lossless_conversion = lossless ? 1u : 0u;
		if (metadata.fallback_count == 0)
			metadata.native_error = 0;
		copyText(metadata.device_id_utf8, sizeof(metadata.device_id_utf8), deviceId);
		copyText(metadata.device_name_utf8, sizeof(metadata.device_name_utf8), deviceName);
		if (metadata.fallback_count == 0)
		{
			metadata.result = PSYX_AUDIO_OK;
			metadata.diagnostic[0] = '\0';
		}
	}

	void snapshot(PsyXAudioStatus& output)
	{
		{
			std::lock_guard<std::mutex> lock(metadataMutex);
			output = metadata;
		}
		output.canonical_frames_rendered = canonicalFramesRendered.load(std::memory_order_relaxed);
		output.device_frames_submitted = deviceFramesSubmitted.load(std::memory_order_relaxed);
		output.device_frames_played = deviceFramesPlayed.load(std::memory_order_relaxed);
		output.underrun_frames = underrunFrames.load(std::memory_order_relaxed);
	}
};

class RenderPump
{
public:
	RenderPump(
		uint32_t capacityFrames,
		PsyXAudioRenderCallback callback,
		PsyXAudioRenderCallbackFloat64 callbackFloat64,
		void* user,
		uint32_t sourceRate,
		AudioSharedState& shared)
		: ring_(capacityFrames),
		  callback_(callback),
		  callbackFloat64_(callbackFloat64),
		  user_(user),
		  sourceRate_(sourceRate),
		  shared_(shared),
		  scratchInt_(callback ? static_cast<size_t>(std::min<uint32_t>(capacityFrames, 1024u)) * 2u : 0u),
		  scratchDouble_(callbackFloat64 ? static_cast<size_t>(std::min<uint32_t>(capacityFrames, 256u)) * 2u : 0u),
		  generatedFrames_(0),
		  consumedFrames_(0)
	{
	}

	uint64_t generatedFrames() const { return generatedFrames_; }
	uint64_t consumedFrames() const { return consumedFrames_; }
	size_t bufferedFrames() const { return ring_.size(); }
	uint32_t sourceRate() const { return sourceRate_; }
	bool isFloat64() const { return callbackFloat64_ != nullptr; }

	void fillTo(uint64_t targetFrame)
	{
		while (generatedFrames_ < targetFrame && ring_.freeSpace() != 0)
		{
			const uint64_t remaining = targetFrame - generatedFrames_;
			const size_t scratchFrames = callbackFloat64_
				? scratchDouble_.size() / 2u : scratchInt_.size() / 2u;
			const uint32_t requested = static_cast<uint32_t>(
				std::min<uint64_t>(remaining, std::min<size_t>(scratchFrames, ring_.freeSpace())));
			if (requested == 0)
				break;

			uint32_t rendered = callbackFloat64_
				? callbackFloat64_(user_, scratchDouble_.data(), requested)
				: callback_(user_, scratchInt_.data(), requested);
			rendered = std::min(rendered, requested);
			if (rendered < requested)
			{
				if (callbackFloat64_)
					std::fill(
						scratchDouble_.begin() + static_cast<size_t>(rendered) * 2u,
						scratchDouble_.begin() + static_cast<size_t>(requested) * 2u,
						0.0);
				else
					std::fill(
						scratchInt_.begin() + static_cast<size_t>(rendered) * 2u,
						scratchInt_.begin() + static_cast<size_t>(requested) * 2u,
						0);
				shared_.underrunFrames.fetch_add(requested - rendered, std::memory_order_relaxed);
			}
			if (callbackFloat64_)
				ring_.writeInterleaved(scratchDouble_.data(), requested);
			else
				ring_.writeInterleaved(scratchInt_.data(), requested);
			generatedFrames_ += requested;
			shared_.canonicalFramesRendered.fetch_add(requested, std::memory_order_relaxed);
		}
	}

	StereoFrame consume()
	{
		StereoFrame frame{0, 0};
		if (!ring_.read(frame))
			shared_.underrunFrames.fetch_add(1, std::memory_order_relaxed);
		++consumedFrames_;
		return frame;
	}

private:
	CanonicalRing ring_;
	PsyXAudioRenderCallback callback_;
	PsyXAudioRenderCallbackFloat64 callbackFloat64_;
	void* user_;
	uint32_t sourceRate_;
	AudioSharedState& shared_;
	std::vector<int16_t> scratchInt_;
	std::vector<double> scratchDouble_;
	uint64_t generatedFrames_;
	uint64_t consumedFrames_;
};

class OutputPacker
{
public:
	OutputPacker(RenderPump& pump, PackedFormat format, uint32_t deviceRate, PsyXAudioDither dither)
		: pump_(pump),
		  format_(format),
		  multiplier_(pump.isFloat64() ? 1u : std::max(deviceRate / pump.sourceRate(), 1u)),
		  decimationRatio_(pump.isFloat64() ? pump.sourceRate() / deviceRate : 1u),
		  repeatsLeft_(0),
		  current_{0, 0},
		  dither_(dither == PSYX_AUDIO_DITHER_TPDF),
		  ditherState_(0xD1B54A32D192ED03ull)
	{
		int stages = 0;
		for (uint32_t ratio = decimationRatio_; ratio > 1u; ratio >>= 1u)
			++stages;
		decimator_.Reset(stages);
	}

	uint64_t sourceFramesNeeded(uint32_t outputFrames) const
	{
		if (pump_.isFloat64())
			return static_cast<uint64_t>(outputFrames) * decimationRatio_;
		if (outputFrames <= repeatsLeft_)
			return 0;
		const uint64_t remaining = outputFrames - repeatsLeft_;
		return (remaining + multiplier_ - 1) / multiplier_;
	}

	void pack(uint8_t* output, uint32_t outputFrames)
	{
		const uint32_t stride = bytesPerFrame(format_);
		for (uint32_t i = 0; i < outputFrames; ++i)
		{
			if (pump_.isFloat64())
			{
				bool produced = false;
				for (uint32_t n = 0; n < decimationRatio_; ++n)
				{
					const StereoFrame input = pump_.consume();
					produced = decimator_.Process(
						input.left, input.right, &current_.left, &current_.right);
				}
				if (!produced)
					current_ = StereoFrame{0.0, 0.0};
			}
			else if (repeatsLeft_ == 0)
			{
				current_ = pump_.consume();
				repeatsLeft_ = multiplier_;
			}
			packFrame(output + static_cast<size_t>(i) * stride, format_, current_,
				dither_ && format_ != PackedFormat::F32, &ditherState_);
			if (!pump_.isFloat64())
				--repeatsLeft_;
		}
	}

private:
	RenderPump& pump_;
	PackedFormat format_;
	uint32_t multiplier_;
	uint32_t decimationRatio_;
	uint32_t repeatsLeft_;
	StereoFrame current_;
	bool dither_;
	uint64_t ditherState_;
	PsyX::ReferenceOutputDecimator decimator_;
};

inline uint32_t rateMaskFor(uint32_t rate)
{
	switch (rate)
	{
	case 44100:
		return PSYX_AUDIO_RATE_44100;
	case 88200:
		return PSYX_AUDIO_RATE_88200;
	case 176400:
		return PSYX_AUDIO_RATE_176400;
	case 352800:
		return PSYX_AUDIO_RATE_352800;
	default:
		return 0;
	}
}

inline uint32_t rateMultiplier(uint32_t rate)
{
	return rate / 44100u;
}

} // namespace psyx::audio

#endif
