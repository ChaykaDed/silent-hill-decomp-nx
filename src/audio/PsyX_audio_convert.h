#ifndef PSYX_AUDIO_CONVERT_H
#define PSYX_AUDIO_CONVERT_H

#include "PsyX_audio_ring.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>

namespace psyx::audio
{

enum class PackedFormat
{
	S16,
	S24,
	S24In32,
	S32,
	F32
};

inline uint32_t bytesPerFrame(PackedFormat format)
{
	switch (format)
	{
	case PackedFormat::S16:
		return 4;
	case PackedFormat::S24:
		return 6;
	case PackedFormat::S24In32:
	case PackedFormat::S32:
	case PackedFormat::F32:
		return 8;
	}
	return 0;
}

inline void writeLe24(uint8_t* output, int32_t sample)
{
	const uint32_t value = static_cast<uint32_t>(sample);
	output[0] = static_cast<uint8_t>(value);
	output[1] = static_cast<uint8_t>(value >> 8);
	output[2] = static_cast<uint8_t>(value >> 16);
}

inline uint32_t ditherRandom(uint64_t& state)
{
	state ^= state >> 12;
	state ^= state << 25;
	state ^= state >> 27;
	return static_cast<uint32_t>((state * 2685821657736338717ull) >> 32);
}

inline double tpdf(uint64_t& state)
{
	constexpr double scale = 1.0 / 4294967296.0;
	return (static_cast<double>(ditherRandom(state)) -
		static_cast<double>(ditherRandom(state))) * scale;
}

inline int64_t quantize(double value, int64_t minimum, int64_t maximum,
	bool dither, uint64_t& ditherState)
{
	if (dither)
		value += tpdf(ditherState);
	if (!std::isfinite(value))
		value = 0.0;
	value = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
	return static_cast<int64_t>(std::llround(value));
}

inline void packFrame(uint8_t* output, PackedFormat format, const StereoFrame& frame,
	bool dither = false, uint64_t* ditherState = nullptr)
{
	uint64_t localState = 0x9E3779B97F4A7C15ull;
	uint64_t& state = ditherState ? *ditherState : localState;
	switch (format)
	{
	case PackedFormat::S16:
	{
		const int16_t samples[2] = {
			static_cast<int16_t>(quantize(frame.left, -32768, 32767, dither, state)),
			static_cast<int16_t>(quantize(frame.right, -32768, 32767, dither, state))
		};
		std::memcpy(output, samples, sizeof(samples));
		break;
	}
	case PackedFormat::S24:
		writeLe24(output, static_cast<int32_t>(quantize(frame.left * 256.0,
			-8388608, 8388607, dither, state)));
		writeLe24(output + 3, static_cast<int32_t>(quantize(frame.right * 256.0,
			-8388608, 8388607, dither, state)));
		break;
	case PackedFormat::S24In32:
	{
		const int32_t samples[2] = {
			static_cast<int32_t>(quantize(frame.left * 256.0,
				-8388608, 8388607, dither, state) * 256),
			static_cast<int32_t>(quantize(frame.right * 256.0,
				-8388608, 8388607, dither, state) * 256)
		};
		std::memcpy(output, samples, sizeof(samples));
		break;
	}
	case PackedFormat::S32:
	{
		const int32_t samples[2] = {
			static_cast<int32_t>(quantize(frame.left * 65536.0,
				std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), dither, state)),
			static_cast<int32_t>(quantize(frame.right * 65536.0,
				std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), dither, state))
		};
		std::memcpy(output, samples, sizeof(samples));
		break;
	}
	case PackedFormat::F32:
	{
		const float samples[2] = {
			static_cast<float>(frame.left / 32768.0),
			static_cast<float>(frame.right / 32768.0)
		};
		std::memcpy(output, samples, sizeof(samples));
		break;
	}
	}
}

} // namespace psyx::audio

#endif
