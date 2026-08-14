#define PSYX_SPUAL_BACKEND_PREFIX PsyX_SPUSoftware
#include "PsyX_SPUBackendRename.h"
#include "PsyX_SPUAL_ext.h"
#include "PsyX_SPUCore.h"
#include "PsyX_XAStream.h"
#include "PsyX_ReferenceXA.h"
#include "PsyX/PsyX_audio.h"

#include "../PsyX_main.h"

#include <SDL.h>
#include <algorithm>
#include <vector>

#if defined(__GNUC__) && !defined(_WIN32)
#define PSX_API_EXPORT __attribute__((visibility("default")))
#else
#define PSX_API_EXPORT
#endif

namespace
{
/* Constructed on first use, not at static init: SPUCore embeds 512 KiB of SPU
 * RAM plus the Ideal (2 MiB) and Reference (~4.6 MiB) reverb delay lines, all
 * zero-filled by its ctor. As a namespace-scope object that cost was paid by
 * every launch, including the default legacy OpenAL backend which never
 * touches this TU. */
PsyX::SPUCore& g_spu()
{
	static PsyX::SPUCore instance;
	return instance;
}
PsyX_XAStream* g_xa = nullptr;
PsyX_ReferenceXA* g_referenceXa = nullptr;
SDL_mutex* g_spuMutex = nullptr;
bool g_initialized = false;
PsyXAudioConfig g_audioConfig{};
bool g_audioConfigured = false;
bool g_xaPaused = false;
PsyX::RendererMode g_renderer = PsyX::RendererMode::Exact;
PsyX::ClipMode g_clipMode = PsyX::ClipMode::None;
PsyXAudioDither g_dither = PSYX_AUDIO_DITHER_NONE;
bool g_rendererConfigValid = true;
uint32_t g_idealNativePhase = 0;

void EnsureConfig()
{
    if (!g_audioConfigured)
    {
        PsyX_AudioDefaultConfig(&g_audioConfig);
        g_audioConfigured = true;
    }
}

uint32_t RenderAudio(void*, int16_t* output, uint32_t frames)
{
    SDL_LockMutex(g_spuMutex);

    static std::vector<int16_t> xa;
    xa.resize(static_cast<size_t>(frames) * 2u);
    uint32_t xaFrames = g_xa && !g_xaPaused ?
        PsyX_XAStream_Pop44100Stereo(g_xa, xa.data(), frames) : 0;
    for (uint32_t i = 0; i < xaFrames; ++i)
        g_spu().PushCdStereoFrame(xa[i * 2], xa[i * 2 + 1]);

    g_spu().RenderFrames(output, static_cast<int>(frames));
    SDL_UnlockMutex(g_spuMutex);
    return frames;
}

uint32_t RenderAudioFloat64(void*, double* output, uint32_t frames)
{
    SDL_LockMutex(g_spuMutex);
    if (g_renderer == PsyX::RendererMode::Reference)
    {
        static std::vector<double> xa;
        xa.resize(static_cast<size_t>(frames) * 2u);
        const size_t xaFrames = g_referenceXa && !g_xaPaused
            ? PsyX_ReferenceXA_PullStereo(g_referenceXa, xa.data(), frames) : 0;
        for (size_t i = 0; i < xaFrames; ++i)
            g_spu().PushCdStereoFrameDouble(xa[i * 2], xa[i * 2 + 1]);
    }
    else
    {
        uint32_t logicalFrames = 0;
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (g_idealNativePhase == 0)
                ++logicalFrames;
            g_idealNativePhase = (g_idealNativePhase + 1u) & 3u;
        }
        static std::vector<int16_t> xa;
        xa.resize(static_cast<size_t>(logicalFrames) * 2u);
        const uint32_t xaFrames = g_xa && !g_xaPaused
            ? PsyX_XAStream_Pop44100Stereo(g_xa, xa.data(), logicalFrames) : 0;
        for (uint32_t i = 0; i < xaFrames; ++i)
            g_spu().PushCdStereoFrame(xa[i * 2], xa[i * 2 + 1]);
    }
    g_spu().RenderFramesDouble(output, static_cast<int>(frames));
    SDL_UnlockMutex(g_spuMutex);
    return frames;
}

PsyX::TransferMode ToTransferMode(int mode)
{
    return mode == SPU_TRANSFER_BY_IO ? PsyX::TransferMode::ManualWrite : PsyX::TransferMode::DmaWrite;
}
}

extern "C"
{

PSX_API_EXPORT void PsyX_SPUAL_ConfigureOutput(int backend, int mode, int rate, int bitPerfect)
{
    EnsureConfig();
    g_audioConfig.backend = static_cast<PsyXAudioBackend>(backend);
    g_audioConfig.mode = static_cast<PsyXAudioMode>(mode);
    g_audioConfig.allowed_rate_mask =
        rate == 88200 ? PSYX_AUDIO_RATE_88200 :
        rate == 352800 ? PSYX_AUDIO_RATE_352800 :
        rate == 176400 ? PSYX_AUDIO_RATE_176400 : PSYX_AUDIO_RATE_44100;
    g_audioConfig.flags = rate == 44100 ? 0u :
        static_cast<uint32_t>(PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION);
    if (bitPerfect)
        g_audioConfig.flags |= PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT;
}

PSX_API_EXPORT int PsyX_SPUAL_ConfigureRenderer(
    int renderer, int idealClip, int referenceClip, int referenceDither)
{
    if (renderer < 0 || renderer > 2 || idealClip < 0 || idealClip > 2 ||
        referenceClip < 0 || referenceClip > 2 ||
        referenceDither < 0 || referenceDither > 1)
    {
        g_rendererConfigValid = false;
        return 0;
    }
    EnsureConfig();
    if (renderer != 0 && (g_audioConfig.flags & PSYX_AUDIO_FLAG_REQUIRE_BIT_PERFECT) != 0)
    {
        g_rendererConfigValid = false;
        return 0;
    }
    if (renderer == 0 &&
        (g_audioConfig.allowed_rate_mask & PSYX_AUDIO_RATE_352800) != 0)
    {
        g_rendererConfigValid = false;
        return 0;
    }
    if (renderer == 1 &&
        (g_audioConfig.allowed_rate_mask & PSYX_AUDIO_RATE_352800) != 0)
    {
        g_rendererConfigValid = false;
        return 0;
    }
    g_renderer = static_cast<PsyX::RendererMode>(renderer);
    g_clipMode = static_cast<PsyX::ClipMode>(
        renderer == 1 ? idealClip : renderer == 2 ? referenceClip : 1);
    g_dither = referenceDither ? PSYX_AUDIO_DITHER_TPDF : PSYX_AUDIO_DITHER_NONE;
    g_spu().SetRenderer(g_renderer, g_clipMode);
    if (g_renderer != PsyX::RendererMode::Exact)
    {
        g_audioConfig.flags |= PSYX_AUDIO_FLAG_ALLOW_FAMILY_RATE_EXPANSION |
            PSYX_AUDIO_FLAG_ALLOW_SHARED_FLOAT;
        g_audioConfig.allowed_container_mask =
            PSYX_AUDIO_CONTAINER_S16 | PSYX_AUDIO_CONTAINER_S24 |
            PSYX_AUDIO_CONTAINER_S24_IN_32 | PSYX_AUDIO_CONTAINER_S32 |
            PSYX_AUDIO_CONTAINER_F32;
    }
    g_rendererConfigValid = true;
    return 1;
}

int PsyX_SPUAL_InitSound()
{
    if (g_initialized)
        return 1;
    if (!g_rendererConfigValid)
    {
        eprinterr("Software SPU renderer configuration is unsupported\n");
        return 0;
    }

    if (!g_spuMutex)
        g_spuMutex = SDL_CreateMutex();
    if (!g_spuMutex)
        return 0;
    if (!g_xa)
        g_xa = PsyX_XAStream_Create();
    if (!g_referenceXa)
        g_referenceXa = PsyX_ReferenceXA_Create();
    if (!g_xa || !g_referenceXa)
        return 0;

    SDL_LockMutex(g_spuMutex);
    g_spu().Reset();
    g_spu().SetTransferMode(PsyX::TransferMode::DmaWrite);
    PsyX_XAStream_Reset(g_xa);
    PsyX_ReferenceXA_Reset(g_referenceXa);
    g_xaPaused = false;
    g_idealNativePhase = 0;
    SDL_UnlockMutex(g_spuMutex);

    EnsureConfig();
    const uint32_t nativeRate = g_spu().GetNativeSampleRate();
    PsyXAudioResult result = g_renderer == PsyX::RendererMode::Exact
        ? PsyX_AudioStart(&g_audioConfig, RenderAudio, nullptr)
        : PsyX_AudioStartFloat64(
            &g_audioConfig, RenderAudioFloat64, nullptr, nativeRate, g_dither);
    g_initialized = result == PSYX_AUDIO_OK || result == PSYX_AUDIO_OK_FALLBACK;
    if (!g_initialized)
    {
        eprinterr("Software SPU output failed to start (%d)\n", static_cast<int>(result));
    }
    else
    {
        if (g_renderer == PsyX::RendererMode::Ideal)
            eprintinfo("Ideal renderer limitation: XA currently uses the exact zigzag 44.1kHz path\n");
        PsyXAudioStatus status{};
        PsyX_AudioGetStatus(&status);
        eprintinfo("Software SPU output: backend=%d mode=%d rate=%u format=%d bit-perfect=%u\n",
                   static_cast<int>(status.active_backend), static_cast<int>(status.active_mode),
                   status.active_rate, static_cast<int>(status.active_format), status.bit_perfect);
    }
    return g_initialized ? 1 : 0;
}

void PsyX_SPUAL_ShutdownSound()
{
    PsyX_AudioStop();
    if (g_spuMutex)
        SDL_LockMutex(g_spuMutex);
    if (g_xa)
    {
        PsyX_XAStream_Destroy(g_xa);
        g_xa = nullptr;
    }
    if (g_referenceXa)
    {
        PsyX_ReferenceXA_Destroy(g_referenceXa);
        g_referenceXa = nullptr;
    }
    g_initialized = false;
    if (g_spuMutex)
    {
        SDL_UnlockMutex(g_spuMutex);
        SDL_DestroyMutex(g_spuMutex);
        g_spuMutex = nullptr;
    }
}

int PsyX_SPUAL_Alloc(int size)
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().Malloc(static_cast<uint32_t>(size));
    SDL_UnlockMutex(g_spuMutex);
    return static_cast<int>(result);
}

int PsyX_SPUAL_AllocAt(u_int addr, int size)
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().MallocWithStartAddr(addr, static_cast<uint32_t>(size));
    SDL_UnlockMutex(g_spuMutex);
    return static_cast<int>(result);
}

int PsyX_SPUAL_InitAlloc(int num, char*)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().InitMalloc(num, 0x1010);
    SDL_UnlockMutex(g_spuMutex);
    return 0;
}

void PsyX_SPUAL_Free(u_int addr)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().Free(addr);
    SDL_UnlockMutex(g_spuMutex);
}

u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr)
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().SetTransferStartAddr(addr);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

int PsyX_SPUAL_SetTransferMode(int mode)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().SetTransferMode(ToTransferMode(mode));
    SDL_UnlockMutex(g_spuMutex);
    return mode == SPU_TRANSFER_BY_IO ? SPU_TRANSFER_BY_IO : SPU_TRANSFER_BY_DMA;
}

u_int PsyX_SPUAL_Write(u_char* addr, u_int size)
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().Write(addr, size);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

u_int PsyX_SPUAL_Read(u_char* addr, u_int size)
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().Read(addr, size);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* attr)
{
    if (!attr) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().SetVoiceAttr(*attr);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* attr)
{
    if (!attr) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().GetVoiceAttr(*attr);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_SetCommonAttr(SpuCommonAttr* attr)
{
    if (!attr) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().SetCommonAttr(*attr);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_GetCommonAttr(SpuCommonAttr* attr)
{
    if (!attr) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().GetCommonAttr(*attr);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_SetKey(int onOff, u_int voiceBits)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().SetKey(onOff, voiceBits);
    SDL_UnlockMutex(g_spuMutex);
}

int PsyX_SPUAL_GetKeyStatus(u_int voiceBit)
{
    SDL_LockMutex(g_spuMutex);
    int result = g_spu().GetKeyStatus(voiceBit);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

void PsyX_SPUAL_GetAllKeysStatus(char* status)
{
    if (!status) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().GetAllKeysStatus(status);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_GetVoiceVolume(int voice, short* left, short* right)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().GetVoiceVolume(voice, left, right);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_GetVoicePitch(int voice, u_short* pitch)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().GetVoicePitch(voice, pitch);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_Update() {}

PSX_API_EXPORT void PsyX_SPUAL_SetAdsrEnabled(int) {}
PSX_API_EXPORT int PsyX_SPUAL_GetAdsrEnabled(void) { return 1; }

PSX_API_EXPORT void PsyX_SPUAL_SetOutputMode(int) {}
PSX_API_EXPORT int PsyX_SPUAL_ApplyOutputMode(int) { return 0; }
PSX_API_EXPORT int PsyX_SPUAL_GetOutputMode(void) { return 1; }
PSX_API_EXPORT int PsyX_SPUAL_GetSurroundActive(void) { return 0; }

PSX_API_EXPORT void PsyX_SPUAL_SetNextKeyOnAzimuth(int) {}
PSX_API_EXPORT void PsyX_SPUAL_ClearNextKeyOnAzimuth(void) {}
PSX_API_EXPORT void PsyX_SPUAL_SetVoiceAzimuth(int, int) {}

int PsyX_SPUAL_SetMute(int onOff)
{
    SDL_LockMutex(g_spuMutex);
    int result = g_spu().SetMute(onOff);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

int PsyX_SPUAL_SetReverb(int onOff)
{
    SDL_LockMutex(g_spuMutex);
    int previous = g_spu().GetReverbMasterEnable() ? 1 : 0;
    g_spu().SetReverbMasterEnable(onOff != 0);
    SDL_UnlockMutex(g_spuMutex);
    return previous;
}

int PsyX_SPUAL_GetReverbState()
{
    SDL_LockMutex(g_spuMutex);
    int result = g_spu().GetReverbMasterEnable() ? 1 : 0;
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

u_int PsyX_SPUAL_SetReverbVoice(int onOff, u_int voiceBits)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().SetVoiceReverbSend(voiceBits, onOff != 0);
    uint32_t result = g_spu().GetVoiceReverbSendMask();
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

u_int PsyX_SPUAL_GetReverbVoice()
{
    SDL_LockMutex(g_spuMutex);
    uint32_t result = g_spu().GetVoiceReverbSendMask();
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

void PsyX_SPUAL_SetReverbDepthMasked(int maskL, int maskR, short depthL, short depthR)
{
    SpuReverbAttr attr{};
    attr.mask = (maskL ? SPU_REV_DEPTHL : 0) | (maskR ? SPU_REV_DEPTHR : 0);
    attr.depth.left = depthL;
    attr.depth.right = depthR;
    SDL_LockMutex(g_spuMutex);
    g_spu().SetReverbModeParam(attr);
    SDL_UnlockMutex(g_spuMutex);
}

int PsyX_SPUAL_SetReverbMode(int mode)
{
    SpuReverbAttr current{};
    SDL_LockMutex(g_spuMutex);
    g_spu().GetReverbModeParam(current);
    int previous = current.mode;
    SpuReverbAttr attr{};
    attr.mask = SPU_REV_MODE;
    attr.mode = mode;
    g_spu().SetReverbModeParam(attr);
    SDL_UnlockMutex(g_spuMutex);
    return previous;
}

void PsyX_SPUAL_GetReverbModeParam(SpuReverbAttr* attr)
{
    if (!attr) return;
    SDL_LockMutex(g_spuMutex);
    g_spu().GetReverbModeParam(*attr);
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_SetReverbDepthScale(float) {}
float PsyX_SPUAL_GetReverbDepthScale(void) { return 1.0f; }

int PsyX_SPUAL_ClearReverbWorkArea(void)
{
    SDL_LockMutex(g_spuMutex);
    g_spu().ClearReverbWorkArea();
    SDL_UnlockMutex(g_spuMutex);
    return 0;
}

int PsyX_SPUAL_PushXaFrames(const short* samples, u_int frames, int sourceRate, int channels)
{
    if ((!g_xa && !g_referenceXa) || !samples) return 0;
    SDL_LockMutex(g_spuMutex);
    int result = g_renderer == PsyX::RendererMode::Reference
        ? PsyX_ReferenceXA_Push(g_referenceXa, samples, frames, sourceRate, channels)
        : PsyX_XAStream_Push(g_xa, samples, frames, sourceRate, channels);
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

void PsyX_SPUAL_ResetXa(void)
{
    if (!g_xa && !g_referenceXa) return;
    SDL_LockMutex(g_spuMutex);
    PsyX_XAStream_Reset(g_xa);
    PsyX_ReferenceXA_Reset(g_referenceXa);
    g_spu().ClearCdQueue();
    SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_FinishXa(void)
{
    if (!g_xa && !g_referenceXa) return;
    SDL_LockMutex(g_spuMutex);
    if (g_renderer == PsyX::RendererMode::Reference)
        PsyX_ReferenceXA_Finish(g_referenceXa);
    else
        PsyX_XAStream_Drain(g_xa);
    SDL_UnlockMutex(g_spuMutex);
}

int PsyX_SPUAL_IsXaDrained(void)
{
    if (!g_xa && !g_referenceXa) return 1;
    SDL_LockMutex(g_spuMutex);
    const int result = g_renderer == PsyX::RendererMode::Reference
        ? PsyX_ReferenceXA_IsDrained(g_referenceXa)
        : PsyX_XAStream_PeekQueuedOutputFrames(g_xa) == 0;
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

void PsyX_SPUAL_SetXaMasterGain(double gain)
{
    if (g_spuMutex)
        SDL_LockMutex(g_spuMutex);
    g_spu().SetXaMasterGain(gain);
    if (g_spuMutex)
        SDL_UnlockMutex(g_spuMutex);
}

void PsyX_SPUAL_SetXaPaused(int paused)
{
    SDL_LockMutex(g_spuMutex);
    g_xaPaused = paused != 0;
    SDL_UnlockMutex(g_spuMutex);
}

u_int PsyX_SPUAL_GetQueuedXaFrames(void)
{
    if (!g_xa && !g_referenceXa) return 0;
    SDL_LockMutex(g_spuMutex);
    uint32_t result;
    if (g_renderer == PsyX::RendererMode::Reference)
    {
        const size_t highRateFrames =
            PsyX_ReferenceXA_QueuedFrames(g_referenceXa);
        result = static_cast<uint32_t>(std::min<size_t>(
            highRateFrames / 8u, UINT32_MAX));
    }
    else
    {
        result = PsyX_XAStream_PeekQueuedOutputFrames(g_xa);
    }
    SDL_UnlockMutex(g_spuMutex);
    return result;
}

}
