#ifndef PSYX_AUDIO_H
#define PSYX_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define PSYX_AUDIO_API_VERSION 2u
#define PSYX_AUDIO_DEVICE_ID_MAX 512u
#define PSYX_AUDIO_DEVICE_NAME_MAX 256u
#define PSYX_AUDIO_DIAGNOSTIC_MAX 256u

typedef enum PsyXAudioResult
{
	PSYX_AUDIO_OK = 0,
	PSYX_AUDIO_OK_FALLBACK = 1,
	PSYX_AUDIO_ERROR_INVALID_ARGUMENT = -1,
	PSYX_AUDIO_ERROR_INVALID_STATE = -2,
	PSYX_AUDIO_ERROR_NO_DEVICE = -3,
	PSYX_AUDIO_ERROR_UNSUPPORTED_FORMAT = -4,
	PSYX_AUDIO_ERROR_DEVICE_BUSY = -5,
	PSYX_AUDIO_ERROR_DEVICE_INVALIDATED = -6,
	PSYX_AUDIO_ERROR_BACKEND = -7,
	PSYX_AUDIO_ERROR_THREAD = -8,
	PSYX_AUDIO_ERROR_OUT_OF_MEMORY = -9
} PsyXAudioResult;

typedef enum PsyXAudioBackend
{
	PSYX_AUDIO_BACKEND_AUTO = 0,
	PSYX_AUDIO_BACKEND_WASAPI = 1,
	PSYX_AUDIO_BACKEND_SDL = 2
} PsyXAudioBackend;

typedef enum PsyXAudioMode
{
	PSYX_AUDIO_MODE_AUTO = 0,
	PSYX_AUDIO_MODE_SHARED = 1,
	PSYX_AUDIO_MODE_EXCLUSIVE_EVENT = 2
} PsyXAudioMode;

typedef enum PsyXAudioState
{
	PSYX_AUDIO_STATE_STOPPED = 0,
	PSYX_AUDIO_STATE_OPENING = 1,
	PSYX_AUDIO_STATE_PRIMING = 2,
	PSYX_AUDIO_STATE_RUNNING = 3,
	PSYX_AUDIO_STATE_STOPPING = 4,
	PSYX_AUDIO_STATE_FAILED = 5
} PsyXAudioState;

typedef enum PsyXAudioSampleFormat
{
	PSYX_AUDIO_FORMAT_UNKNOWN = 0,
	PSYX_AUDIO_FORMAT_S16 = 1,
	PSYX_AUDIO_FORMAT_S24 = 2,
	PSYX_AUDIO_FORMAT_S24_IN_32 = 3,
	PSYX_AUDIO_FORMAT_S32 = 4,
	PSYX_AUDIO_FORMAT_F32 = 5
} PsyXAudioSampleFormat;

enum
{
	PSYX_AUDIO_RATE_44100 = 1u << 0,
	PSYX_AUDIO_RATE_88200 = 1u << 1,
	PSYX_AUDIO_RATE_176400 = 1u << 2,
	PSYX_AUDIO_RATE_352800 = 1u << 3
};

enum
{
	PSYX_AUDIO_CONTAINER_S16 = 1u << 0,
	PSYX_AUDIO_CONTAINER_S24 = 1u << 1,
	PSYX_AUDIO_CONTAINER_S24_IN_32 = 1u << 2,
	PSYX_AUDIO_CONTAINER_S32 = 1u << 3,
	PSYX_AUDIO_CONTAINER_F32 = 1u << 4
};

enum
{
	PSYX_AUDIO_FALLBACK_EXCLUSIVE_TO_SHARED = 1u << 0,
	PSYX_AUDIO_FALLBACK_TO_SDL = 1u << 1
};

enum
{
	PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION = 1u << 0,
	PSYX_AUDIO_FLAG_ALLOW_SHARED_FLOAT = 1u << 1,
	PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT = 1u << 2
};

/* The callback advances frame_count frames; any unwritten tail is replaced with silence. */
typedef uint32_t (*PsyXAudioRenderCallback)(
	void* user,
	int16_t* interleaved_stereo,
	uint32_t frame_count);

typedef uint32_t (*PsyXAudioRenderCallbackFloat64)(
	void* user,
	double* interleaved_stereo,
	uint32_t frame_count);

typedef enum PsyXAudioDither
{
	PSYX_AUDIO_DITHER_NONE = 0,
	PSYX_AUDIO_DITHER_TPDF = 1
} PsyXAudioDither;

typedef struct PsyXAudioConfig
{
	uint32_t struct_size;
	uint32_t api_version;
	PsyXAudioBackend backend;
	PsyXAudioMode mode;
	uint32_t fallback_mask;
	uint32_t flags;
	uint32_t allowed_rate_mask;
	uint32_t allowed_container_mask;
	uint32_t target_latency_ms;
	uint32_t ring_capacity_frames;
	const char* device_id_utf8;
} PsyXAudioConfig;

typedef struct PsyXAudioDeviceInfo
{
	PsyXAudioBackend backend;
	uint32_t is_default;
	uint32_t state;
	uint32_t mix_rate;
	uint16_t mix_channels;
	PsyXAudioSampleFormat mix_format;
	uint32_t exclusive_rate_mask;
	uint32_t exclusive_container_mask;
	char id_utf8[PSYX_AUDIO_DEVICE_ID_MAX];
	char name_utf8[PSYX_AUDIO_DEVICE_NAME_MAX];
} PsyXAudioDeviceInfo;

typedef struct PsyXAudioStatus
{
	PsyXAudioState state;
	PsyXAudioResult result;
	PsyXAudioBackend requested_backend;
	PsyXAudioBackend active_backend;
	PsyXAudioMode requested_mode;
	PsyXAudioMode active_mode;
	PsyXAudioSampleFormat active_format;
	uint32_t active_rate;
	uint16_t container_bits;
	uint16_t valid_bits;
	uint32_t rate_multiplier;
	uint32_t bit_perfect;
	uint32_t lossless_conversion;
	uint32_t fallback_count;
	PsyXAudioResult fallback_reason;
	int64_t native_error;
	uint64_t canonical_frames_rendered;
	uint64_t device_frames_submitted;
	uint64_t device_frames_played;
	uint64_t underrun_frames;
	char device_id_utf8[PSYX_AUDIO_DEVICE_ID_MAX];
	char device_name_utf8[PSYX_AUDIO_DEVICE_NAME_MAX];
	char diagnostic[PSYX_AUDIO_DIAGNOSTIC_MAX];
} PsyXAudioStatus;

void PsyX_AudioDefaultConfig(PsyXAudioConfig* config);

PsyXAudioResult PsyX_AudioStart(
	const PsyXAudioConfig* config,
	PsyXAudioRenderCallback render_callback,
	void* render_user);

PsyXAudioResult PsyX_AudioStartFloat64(
	const PsyXAudioConfig* config,
	PsyXAudioRenderCallbackFloat64 render_callback,
	void* render_user,
	uint32_t source_rate,
	PsyXAudioDither dither);

void PsyX_AudioStop(void);
PsyXAudioResult PsyX_AudioRestart(void);
void PsyX_AudioGetStatus(PsyXAudioStatus* status);

PsyXAudioResult PsyX_AudioEnumerateDevices(
	PsyXAudioBackend backend,
	PsyXAudioDeviceInfo* devices,
	size_t capacity,
	size_t* device_count);

int PsyX_AudioPushXaFrames(
	const int16_t* samples,
	uint32_t frames,
	uint32_t source_rate,
	uint32_t channels);
void PsyX_AudioResetXa(void);
void PsyX_AudioFinishXa(void);
int PsyX_AudioIsXaDrained(void);
void PsyX_AudioSetXaMasterGain(double gain);
void PsyX_AudioSetXaPaused(int paused);
uint32_t PsyX_AudioGetQueuedXaFrames(void);

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
