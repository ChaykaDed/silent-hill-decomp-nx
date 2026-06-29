#include "../PsyX_main.h"

#include "psx/libspu.h"
#include "psx/libetc.h"
#include "psx/libmath.h"
#include "PsyX_SPUAL.h"

#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#ifndef __EMSCRIPTEN__
#include <AL/efx.h>
#endif

// TODO: implement XA, implement ADSR

static const char* getALCErrorString(int err)
{
	switch (err)
	{
	case ALC_NO_ERROR:
		return "AL_NO_ERROR";
	case ALC_INVALID_DEVICE:
		return "ALC_INVALID_DEVICE";
	case ALC_INVALID_CONTEXT:
		return "ALC_INVALID_CONTEXT";
	case ALC_INVALID_ENUM:
		return "ALC_INVALID_ENUM";
	case ALC_INVALID_VALUE:
		return "ALC_INVALID_VALUE";
	case ALC_OUT_OF_MEMORY:
		return "ALC_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

static const char* getALErrorString(int err)
{
	switch (err)
	{
	case AL_NO_ERROR:
		return "AL_NO_ERROR";
	case AL_INVALID_NAME:
		return "AL_INVALID_NAME";
	case AL_INVALID_ENUM:
		return "AL_INVALID_ENUM";
	case AL_INVALID_VALUE:
		return "AL_INVALID_VALUE";
	case AL_INVALID_OPERATION:
		return "AL_INVALID_OPERATION";
	case AL_OUT_OF_MEMORY:
		return "AL_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

#define SPU_REALMEMSIZE			(512 * 1024)
#define SPU_MEMSIZE				(2048*1024)		// SPU_REALMEMSIZE

typedef struct
{
	u_char samplemem[SPU_MEMSIZE];
	u_char* writeptr;
} SPUMemory;

static SPUMemory s_SpuMemory;
static SDL_mutex* g_SpuMutex = NULL;
static int g_spuInit = 0;
static int s_spuMallocVal = 0;

/* SPU ADSR envelope master gate. Default OFF: the envelope path is the same
 * feature that previously deadlocked when ticked from the render thread, so it
 * ships disabled (audio byte-identical to known-good) and is opt-in via the
 * `adsr 1` console command until validated. When OFF, SetKey/SetVoiceAttr/
 * Update all take their pre-envelope code paths. */
int g_SpuAdsrEnabled = 0;

void PsyX_SPUAL_SetAdsrEnabled(int on) { g_SpuAdsrEnabled = on ? 1 : 0; }
int  PsyX_SPUAL_GetAdsrEnabled(void)   { return g_SpuAdsrEnabled; }

typedef enum
{
	ENV_OFF = 0,
	ENV_ATTACK,
	ENV_DECAY,
	ENV_SUSTAIN,
	ENV_RELEASE,
} EnvPhase;

typedef struct
{
	SpuVoiceAttr attr;	// .voice is Id of this channel

	ALuint alBuffer;
	ALuint alSource;
	ushort sampledirty;
	ushort reverb;

	// PSX SPU ADSR envelope (PC port). Only engaged for looping voices that
	// programmed a real ADSR (adsr1/adsr2 != 0); one-shot SFX/voices keep the
	// plain static-gain path so this never alters the large body of working
	// sounds. Hardware runs the envelope at 44100Hz on a 0..0x7FFF level and
	// multiplies the sample by it; without it a looping sample (e.g. the
	// clock bell) rings forever.
	int      envPhase;     // EnvPhase
	int      envLevel;     // 0..0x7FFF
	int      envCounter;   // accumulated 44100Hz samples toward next step
	float    baseGain;     // volume-derived gain before envelope
	ushort   hasEnvelope;  // adsr programmed + looping
	ushort   looping;      // AL_LOOPING was set for the current sample
} SPUALVoice;

const int s_spuVoiceCount = 24;

SPUALVoice	g_SpuVoices[s_spuVoiceCount];
ALCdevice*	g_ALCdevice = NULL;
ALCcontext* g_ALCcontext = NULL;
int			g_SPUMuted = 0;
ALuint		g_ALEffectSlots[2];
int			g_currEffectSlotIdx = 0;
ALuint		g_nAlReverbEffect = 0;
int			g_enableSPUReverb = 0;
int			g_ALEffectsSupported = 0;

#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)

LPALGENEFFECTS alGenEffects = NULL;
LPALDELETEEFFECTS alDeleteEffects = NULL;
LPALEFFECTI alEffecti = NULL;
LPALEFFECTF alEffectf = NULL;
LPALGENAUXILIARYEFFECTSLOTS alGenAuxiliaryEffectSlots = NULL;
LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots = NULL;
LPALAUXILIARYEFFECTSLOTI alAuxiliaryEffectSloti = NULL;

#endif

static void InitOpenAlEffects()
{
	g_ALEffectsSupported = 0;
#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)
	if (!alcIsExtensionPresent(g_ALCdevice, ALC_EXT_EFX_NAME))
	{
		eprintf("PSX SPU effects are NOT supported!\n");
		return;
	}

	alGenEffects = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
	alDeleteEffects = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
	alEffecti = (LPALEFFECTI)alGetProcAddress("alEffecti");
	alEffectf = (LPALEFFECTF)alGetProcAddress("alEffectf");
	alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
	alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
	alAuxiliaryEffectSloti = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");

	int max_sends = 0;
	alcGetIntegerv(g_ALCdevice, ALC_MAX_AUXILIARY_SENDS, 1, &max_sends);

	// make reverb effect slot
	g_currEffectSlotIdx = 0;
	alGenAuxiliaryEffectSlots(1, g_ALEffectSlots);

	// make reverb effect
	alGenEffects(1, &g_nAlReverbEffect);
	alEffecti(g_nAlReverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);

	// setup defaults of effect
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAIN, 0.45f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAINHF, 0.25f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_TIME, 2.0f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_HFRATIO, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_DELAY, 0.08f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_GAIN, 0.2f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DIFFUSION, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DENSITY, 0.1f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_AIR_ABSORPTION_GAINHF, 0.1f);

	g_ALEffectsSupported = 1;

	eprintf("PSX SPU effects are supported and initialized\n");

	alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
#endif
}

int PsyX_SPUAL_InitSound()
{
	if (!g_SpuMutex)
		g_SpuMutex = SDL_CreateMutex();

	if (!g_spuInit)
		memset(&s_SpuMemory, 0, sizeof(s_SpuMemory));

	g_spuInit = 1;

	int numDevices, alErr, i;
	const char* devices;
	const char* devStrptr;

	// out_channel_formats snd_outputchannels
	static int al_context_params[] =
	{
		ALC_FREQUENCY, 44100,
#ifndef __EMSCRIPTEN__
		ALC_MAX_AUXILIARY_SENDS, 2,
#endif
		0
	};

	if (g_ALCdevice)
		return 1;

	numDevices = 0;

	// Init openAL
	// check devices list

	devStrptr = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
	devices = devStrptr;

	// go through device list (each device terminated with a single NULL, list terminated with double NULL)
	while ((*devStrptr) != '\0')
	{
		eprintinfo("found sound device: %s\n", devStrptr);
		devStrptr += strlen(devStrptr) + 1;
		numDevices++;
	}

	if(numDevices == 0)
		return 0;
	
	g_ALCdevice = alcOpenDevice(NULL);

	alErr = AL_NO_ERROR;

	if (!g_ALCdevice)
	{
		alErr = alcGetError(NULL);
		eprinterr("alcOpenDevice: NULL DEVICE error: %s\n", getALCErrorString(alErr));
		return 0;
	}

#ifndef __EMSCRIPTEN__
	g_ALCcontext = alcCreateContext(g_ALCdevice, al_context_params);
#else
	g_ALCcontext = alcCreateContext(g_ALCdevice, NULL);
#endif

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcCreateContext error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	alcMakeContextCurrent(g_ALCcontext);

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcMakeContextCurrent error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	// Setup defaults
	alListenerf(AL_GAIN, 1.0f);
	alDistanceModel(AL_NONE);

	// create channels
	for (i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		memset(voice, 0, sizeof(SPUALVoice));

		alGenSources(1, &voice->alSource);
		alGenBuffers(1, &voice->alBuffer);
#ifdef AL_SOFT_source_resampler
		alSourcei(voice->alSource, AL_SOURCE_RESAMPLER_SOFT, 2);	// Use cubic resampler
#endif
		alSourcei(voice->alSource, AL_SOURCE_RELATIVE, AL_TRUE);
	}


	InitOpenAlEffects();

	return 1;
}

void PsyX_SPUAL_ShutdownSound()
{
	g_spuInit = 0;

#ifndef __EMSCRIPTEN__
	if (!g_ALCcontext)
		return;

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		alDeleteSources(1, &voice->alSource);
		alDeleteBuffers(1, &voice->alBuffer);
	}

	if (g_ALEffectsSupported)
	{
#ifndef __SWITCH__
		alDeleteEffects(1, &g_nAlReverbEffect);
		alDeleteAuxiliaryEffectSlots(1, g_ALEffectSlots);
#endif
		g_ALEffectsSupported = AL_NONE;
	}

	alcDestroyContext(g_ALCcontext);
	alcCloseDevice(g_ALCdevice);

	g_ALCcontext = NULL;
	g_ALCdevice = NULL;
#endif // __EMSCRIPTEN__
}

//--------------------------------------------------------------------------------

int PsyX_SPUAL_Alloc(int size)
{
	int addr = s_spuMallocVal;
	s_spuMallocVal += size;

	if (s_spuMallocVal > SPU_MEMSIZE)
		return -1;

	return addr;
}

int PsyX_SPUAL_InitAlloc(int num, char* top)
{
	s_spuMallocVal = 0;
	return 0;
}

void PsyX_SPUAL_Free(u_int addr)
{
	s_spuMallocVal = 0;
}

u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr)
{
	s_SpuMemory.writeptr = s_SpuMemory.samplemem + addr;

	if (addr > SPU_MEMSIZE)
		return 0;

	if (addr < 0x1010)
		return 0;

	return 1;
}

u_int PsyX_SPUAL_Write(u_char* addr, u_int size)
{
	//if (0x7EFF0 < size)
	//	size = 0x7EFF0;

	volatile int wptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (wptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuWrite exceeded SPU_REALMEMSIZE by %d bytes!\n", wptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && wptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(s_SpuMemory.writeptr, addr, size);

#if 0 // BANK TEST
	{
		static short waveBuffer[SPU_MEMSIZE];

		ALuint alSource;
		ALuint alBuffer;

		alGenSources(1, &alSource);
		alGenBuffers(1, &alBuffer);

		int loopStart = 0, loopLen = 0;
		int count = decodeSound(addr, size, waveBuffer, &loopStart, &loopLen);

		// update AL buffer
		alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 11000);

		// set the buffer
		alSourcei(alSource, AL_BUFFER, alBuffer);
		alSourcef(alSource, AL_GAIN, 1.0f);// TODO: panning
		alSourcef(alSource, AL_PITCH, 1);

		alSourcePlay(alSource);
		int status;
		do
		{
			alGetSourcei(alSource, AL_SOURCE_STATE, &status);
		} while (status == AL_PLAYING);

		alSourceStop(alSource);

		alDeleteSources(1, &alSource);
		alDeleteBuffers(1, &alBuffer);
	}
#endif

	return size;
}

u_int PsyX_SPUAL_Read(u_char* addr, u_int size)
{
	volatile int rptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (rptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuRead exceeded SPU_REALMEMSIZE by %d bytes!\n", rptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && rptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(addr, s_SpuMemory.writeptr, size);

	return size;
}

// PSX ADPCM coefficients
static const float K0[5] = { 0, 0.9375, 1.796875, 1.53125, 1.90625 };
static const float K1[5] = { 0, 0, -0.8125, -0.859375, -0.9375 };

// PSX ADPCM decoding routine - decodes a single sample
static short vagToPcm(u_char soundParameter, int soundData, float* vagPrev1, float* vagPrev2)
{
	int resultInt = 0;

	float dTmp1 = 0.0;
	float dTmp2 = 0.0;
	float dTmp3 = 0.0;

	if (soundData > 7)
		soundData -= 16;

	dTmp1 = (float)soundData * pow(2, (float)(12 - (soundParameter & 0x0F)));

	dTmp2 = (*vagPrev1) * K0[(soundParameter >> 4) & 0x0F];
	dTmp3 = (*vagPrev2) * K1[(soundParameter >> 4) & 0x0F];

	(*vagPrev2) = (*vagPrev1);
	(*vagPrev1) = dTmp1 + dTmp2 + dTmp3;

	resultInt = (int)round((*vagPrev1));

	if (resultInt > 32767)
		resultInt = 32767;

	if (resultInt < -32768)
		resultInt = -32768;

	return (short)resultInt;
}

typedef enum 
{
	LoopEnd = 1 << 0,		// Jump to repeat address after this block
							// 1 - Copy repeatAddress to currentAddress AFTER this block
							//     set ENDX (TODO: Immediately or after this block?)
							// 0 - Nothing

	Repeat = 1 << 1,		// Takes an effect only with LoopEnd bit set.
							// 1 - Loop normally
							// 0 - Loop and force Release

	LoopStart = 1 << 2,		// Mark current address as the beginning of repeat
							// 1 - Load currentAddress to repeatAddress
							// 0 - Nothing
} ADPCM_FLAGS;


// Main decoding routine - Takes PSX ADPCM formatted audio data and converts it to PCM. It also extracts the looping information if used.
static int decodeSound(u_char* iData, int soundSize, short* oData, int* loopStart, int* loopLength, int breakOnEnd /*= 0*/)
{
	u_char sp;
	u_char flag;
	int sd = 0;
	float vagPrev1 = 0.0;
	float vagPrev2 = 0.0;
	int k = 0;

	int loopStrt = 0, loopEnd = 0;
	int breakOn = -1;

	/* Cap output to prevent buffer overflow — caller passes waveBuffer[SPU_REALMEMSIZE] */
	const int maxOutputSamples = SPU_REALMEMSIZE - 2;

	for (int i = 0; i < soundSize; i++)
	{
		if (i % 16 == 0)
		{
			sp = iData[i];
			flag = iData[i+1];
			i += 2;
		}

		if (k >= maxOutputSamples)
			break;

		sd = (int)iData[i] & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		sd = ((int)iData[i] >> 4) & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		if (breakOnEnd && k == breakOn)
			return k;

		if (breakOn == -1)
		{
			// flags parsed
			if (flag & LoopStart)
			{
				loopStrt = k + 26; // FIXME: is that correct?
			}

			if (flag & LoopEnd)
			{
				loopEnd = k + 26;

				if (flag & Repeat)
				{
					*loopStart = loopStrt;
					*loopLength = loopEnd - loopStrt;
				}

				if (breakOnEnd)
					breakOn = k + 26;
			}
		}
	}

	return k;
}

static void UpdateVoiceSample(SPUALVoice* voice)
{
	static short waveBuffer[SPU_REALMEMSIZE];
	int loopStart, loopLen, count;
	ALuint alSource, alBuffer;

	//if (!voice->sampledirty)
	//	return;

	voice->sampledirty = 0;

	alSource = voice->alSource;
	alBuffer = voice->alBuffer;

	if (alSource == AL_NONE)
		return;

	loopStart = 0;
	loopLen = 0;

	count = decodeSound(s_SpuMemory.samplemem + voice->attr.addr, SPU_MEMSIZE - voice->attr.addr, waveBuffer, &loopStart, &loopLen, 1);

	if (count == 0)
		return;

	alSourcei(alSource, AL_BUFFER, 0);
	alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 44100);

	if (loopLen > 0)
	{
		// On PSX, the SPU hardware tracks loop_addr during playback.
		// On PC, we pre-decode the whole sample, so loop_addr is stale
		// from previous voice assignments. The original adjustment
		// (loopStart += loop_addr - addr) can produce plausible-but-wrong
		// loop points when a voice channel is reused with a different sample.
		//
		// Honor the PSX loop region [loopStart, loopStart+loopLen] instead of
		// looping the whole buffer. Many one-shot SFX (e.g. cutscene grunts,
		// the boss shove) carry a tiny sustain-tail loop region at the end;
		// whole-buffer looping made the ENTIRE sound repeat forever, which is
		// audibly wrong. AL_LOOP_POINTS_SOFT loops only the tail, matching SPU
		// hardware. Genuine full-sample loops (loopStart=0) are unchanged.
		if (loopStart >= 0 && loopStart < count)
		{
			ALint loopEnd = loopStart + loopLen;
			if (loopEnd > count) loopEnd = count;
			ALint loopPoints[2] = { loopStart, loopEnd };
			alGetError();
			alBufferiv(alBuffer, AL_LOOP_POINTS_SOFT, loopPoints);
		}
		alSourcei(alSource, AL_LOOPING, AL_TRUE);
		voice->looping = 1;
	}
	else
	{
		alSourcei(alSource, AL_LOOPING, AL_FALSE);
		voice->looping = 0;
	}

	// set the buffer
	alSourcei(alSource, AL_BUFFER, alBuffer);
}

// ---- PSX SPU ADSR envelope ------------------------------------------------
// Reference: nocash psx-spx "Envelope Operation". adsr1/adsr2 are the raw
// hardware registers (the game programs them via SpuSetVoiceAttr mask
// SPU_VOICE_ADSR_ADSR1/ADSR2). The envelope advances a 0..0x7FFF level at
// 44100Hz; each "step" the level changes by AdsrStep every AdsrCycles samples.

#define ADSR_MAX 0x7FFF

static int adsr_sustain_level(u_short adsr1)
{
	int sl = adsr1 & 0x0F;                 // bits 0-3
	int lvl = (sl + 1) << 11;              // (N+1)*0x800
	return lvl > ADSR_MAX ? ADSR_MAX : lvl;
}

// Decompose a 0..0x7F rate into (cycles, step) at the current level.
static void adsr_step_params(int rate, int increase, int exponential, int level,
	int* outCycles, int* outStep)
{
	int shift = (rate >> 2) & 0x1F;
	int sel   = rate & 3;
	int step  = increase ? (7 - sel) : (-8 + sel);
	int cycles = 1 << (shift > 11 ? shift - 11 : 0);

	if (shift < 11)
		step <<= (11 - shift);

	if (exponential)
	{
		if (increase && level > 0x6000)
			cycles <<= 2;                  // exponential attack slows near the top
		if (!increase)
			step = (step * level) >> 15;   // exponential decrease scales with level
	}

	if (cycles < 1)
		cycles = 1;

	*outCycles = cycles;
	*outStep = step;
}

static void EnvelopeKeyOn(SPUALVoice* voice)
{
	voice->envPhase   = ENV_ATTACK;
	voice->envLevel   = 0;
	voice->envCounter = 0;
}

static void EnvelopeKeyOff(SPUALVoice* voice)
{
	if (voice->envPhase != ENV_OFF)
		voice->envPhase = ENV_RELEASE;
}

// Advance one voice's envelope by `samples` 44100Hz ticks. Returns the
// 0..0x7FFF level. Recomputes step params each step so phase/level changes
// (exponential curves) track correctly.
static int EnvelopeAdvance(SPUALVoice* voice, int samples)
{
	const u_short adsr1 = voice->attr.adsr1;
	const u_short adsr2 = voice->attr.adsr2;

	const int sustainLevel = adsr_sustain_level(adsr1);

	voice->envCounter += samples;

	for (int guard = 0; guard < 200000 && voice->envPhase != ENV_OFF; guard++)
	{
		int rate, increase, exponential;

		switch (voice->envPhase)
		{
		case ENV_ATTACK:
			rate        = (adsr1 >> 8) & 0x7F;
			increase    = 1;
			exponential = (adsr1 >> 15) & 1;
			break;
		case ENV_DECAY:
			rate        = ((adsr1 >> 4) & 0x0F) << 2; // 4-bit, *4
			increase    = 0;
			exponential = 1;                          // decay is always exponential
			break;
		case ENV_SUSTAIN:
			rate        = (adsr2 >> 6) & 0x7F;
			increase    = !((adsr2 >> 14) & 1);       // bit14: 0=increase 1=decrease
			exponential = (adsr2 >> 15) & 1;
			break;
		case ENV_RELEASE:
		default:
			rate        = (adsr2 & 0x1F) << 2;        // 5-bit, *4
			increase    = 0;
			exponential = (adsr2 >> 5) & 1;
			break;
		}

		int cycles, step;
		adsr_step_params(rate, increase, exponential, voice->envLevel, &cycles, &step);

		if (voice->envCounter < cycles)
			break;

		voice->envCounter -= cycles;
		voice->envLevel += step;

		if (voice->envLevel > ADSR_MAX) voice->envLevel = ADSR_MAX;
		if (voice->envLevel < 0)        voice->envLevel = 0;

		switch (voice->envPhase)
		{
		case ENV_ATTACK:
			if (voice->envLevel >= ADSR_MAX)
				voice->envPhase = ENV_DECAY;
			break;
		case ENV_DECAY:
			if (voice->envLevel <= sustainLevel)
			{
				voice->envLevel = sustainLevel;
				voice->envPhase = ENV_SUSTAIN;
			}
			break;
		case ENV_SUSTAIN:
			// holds until key-off; a decreasing sustain naturally rings out
			break;
		case ENV_RELEASE:
			if (voice->envLevel <= 0)
				voice->envPhase = ENV_OFF;
			break;
		}
	}

	return voice->envLevel;
}

void PsyX_SPUAL_Update()
{
	if (!g_spuInit || !g_SpuAdsrEnabled)
		return;

	static u_int s_lastTicks = 0;
	u_int now = SDL_GetTicks();
	if (s_lastTicks == 0)
		s_lastTicks = now;

	int dtMs = (int)(now - s_lastTicks);
	s_lastTicks = now;

	if (dtMs <= 0)
		return;
	if (dtMs > 100)
		dtMs = 100; // clamp hitches so the envelope can't jump seconds at once

	int samples = (dtMs * 44100) / 1000;

	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];

		if (!voice->hasEnvelope || voice->envPhase == ENV_OFF)
			continue;

		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
			continue;

		int level = EnvelopeAdvance(voice, samples);
		float env = (float)level / (float)ADSR_MAX;

		alSourcef(alSource, AL_GAIN, voice->baseGain * env);

		if (voice->envPhase == ENV_OFF)
		{
			// release finished: silence the (looping) source for real
			alSourceStop(alSource);
			voice->hasEnvelope = 0;
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

int PsyX_SPUAL_SetMute(int on_off)
{
	int old_state = g_SPUMuted;
	g_SPUMuted = on_off;
	return old_state;
}

void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR)
{
	if (volL)
		*volL = g_SpuVoices[vNum].attr.volume.left;

	if (volR)
		*volR = g_SpuVoices[vNum].attr.volume.right;
}

void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch)
{
	*pitch = g_SpuVoices[vNum].attr.pitch;
}

void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* psxAttrib)
{
	/* The game uses this to read back the current pitch (and other
	 * attributes) for an already-keyed voice; libsd's
	 * Sd_PlaySfx → Sd_SfxAttributesUpdate path stashes the result in
	 * g_AudioPlayingPitchList[voiceIdx] and re-uses it on every per-frame
	 * SfxAttributesUpdate. If we leave it as a no-op, the stashed pitch
	 * is 0, so the per-frame SetVoiceAttr passes pitch=0 and our handler
	 * pauses the OpenAL source — the voice plays for one frame and goes
	 * silent (manifested as the radio static "playing for a second then
	 * inaudible").
	 *
	 * .voice on input is a single-bit value (the bit corresponding to the
	 * voice index). Find that bit, then copy the cached attr fields into
	 * the caller's struct. */
	if (!g_spuInit || !psxAttrib)
		return;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		/* Preserve caller's .voice and .mask (mask is invalid on Get,
		 * but the call site may inspect or pass it on). Don't overwrite
		 * those — only fill the read-out fields. */
		psxAttrib->volume     = voice->attr.volume;
		psxAttrib->volmode    = voice->attr.volmode;
		psxAttrib->volumex    = voice->attr.volume;  /* current vol = set vol */
		psxAttrib->pitch      = voice->attr.pitch;
		psxAttrib->note       = voice->attr.note;
		psxAttrib->sample_note = voice->attr.sample_note;
		psxAttrib->envx       = 0; /* current envelope unused on PC */
		psxAttrib->addr       = voice->attr.addr;
		psxAttrib->loop_addr  = voice->attr.loop_addr;
		psxAttrib->a_mode     = voice->attr.a_mode;
		psxAttrib->s_mode     = voice->attr.s_mode;
		psxAttrib->r_mode     = voice->attr.r_mode;
		psxAttrib->ar         = voice->attr.ar;
		psxAttrib->dr         = voice->attr.dr;
		psxAttrib->sr         = voice->attr.sr;
		psxAttrib->rr         = voice->attr.rr;
		psxAttrib->sl         = voice->attr.sl;
		psxAttrib->adsr1      = voice->attr.adsr1;
		psxAttrib->adsr2      = voice->attr.adsr2;
		break; /* only one voice per Get call */
	}

	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib)
{
	if (!g_spuInit)
		return;

	const float STEREO_FACTOR = 3.0f;
	
	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			continue;

		// update sample
		if ((psxAttrib->mask & SPU_VOICE_WDSA) || (psxAttrib->mask & SPU_VOICE_LSAX))
		{
			if (psxAttrib->mask & SPU_VOICE_WDSA)
			{
				if (voice->attr.addr != psxAttrib->addr)
					voice->sampledirty++;

				voice->attr.addr = psxAttrib->addr;
			}

			if (psxAttrib->mask & SPU_VOICE_LSAX)
			{
				if(voice->attr.loop_addr != psxAttrib->loop_addr)
					voice->sampledirty++;

				voice->attr.loop_addr = psxAttrib->loop_addr;
			}
		}

		// update volume
		if ((psxAttrib->mask & SPU_VOICE_VOLL) || (psxAttrib->mask & SPU_VOICE_VOLR))
		{
			if (psxAttrib->mask & SPU_VOICE_VOLL)
				voice->attr.volume.left = psxAttrib->volume.left;

			if (psxAttrib->mask & SPU_VOICE_VOLR)
				voice->attr.volume.right = psxAttrib->volume.right;

			float left_gain = (float)(voice->attr.volume.left) / (float)(16384);
			float right_gain = (float)(voice->attr.volume.right) / (float)(16384);

			if(left_gain > 1.0f)
				left_gain = 1.0f;

			if(right_gain > 1.0f)
				right_gain = 1.0f;

			float pan = (acosf(left_gain) + asinf(right_gain)) / ((float)(M_PI)); // average angle in [0,1]
			pan = 2.0f * pan - 1.0f; // convert to [-1, 1]
			pan = pan * 0.5f; // 0.5 = sin(30') for a +/- 30 degree arc
			alSource3f(alSource, AL_POSITION, pan * STEREO_FACTOR, 0, -sqrtf(1.0f - pan * pan));

			voice->baseGain = (left_gain + right_gain) * 0.5f;

			// While an envelope is running it owns the source gain (the tick
			// applies baseGain*level); otherwise apply the volume directly.
			float g = voice->baseGain;
			if (voice->hasEnvelope && voice->envPhase != ENV_OFF)
				g *= (float)voice->envLevel / (float)ADSR_MAX;
			alSourcef(alSource, AL_GAIN, g);
		}

		// Capture the raw ADSR registers so SetKey can run the envelope.
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR1)
			voice->attr.adsr1 = psxAttrib->adsr1;
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR2)
			voice->attr.adsr2 = psxAttrib->adsr2;

		// update pitch
		if (psxAttrib->mask & SPU_VOICE_PITCH)
		{
			ALint state;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);

			if (psxAttrib->pitch == 0 && state == AL_PLAYING)
				alSourcePause(alSource);
			else if (voice->attr.pitch == 0 && state == AL_PAUSED)
				alSourcePlay(alSource);

			voice->attr.pitch = psxAttrib->pitch;

			const float pitch = (float)(voice->attr.pitch) / 4096.0f;
			alSourcef(alSource, AL_PITCH, pitch);
		}
		
		// TODO: ADSR and other stuff
	}
	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return;

	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			continue;

		if (on_off && !g_SPUMuted)
		{
			alSourceStop(alSource);
			UpdateVoiceSample(voice);

			// Engage the ADSR envelope only for looping voices that programmed
			// a real envelope — those are the ones that otherwise ring forever
			// (e.g. the clock bell). One-shot SFX/voices end on their own and
			// keep the plain static-gain path untouched.
			if (g_SpuAdsrEnabled && voice->looping && (voice->attr.adsr1 || voice->attr.adsr2))
			{
				voice->hasEnvelope = 1;
				EnvelopeKeyOn(voice);
				alSourcef(alSource, AL_GAIN, 0.0f); // attack ramps from silence
			}
			else
			{
				voice->hasEnvelope = 0;
				voice->envPhase = ENV_OFF;
			}

			alSourcePlay(alSource);
		}
		else
		{
			// Key-off means "stop sustaining the loop." Clear AL_LOOPING so a
			// Repeat=1 sample can't keep repeating forever — without this, a
			// one-shot SFX whose VAG carries a sustain-tail loop (e.g. a cutscene
			// grunt/boss sound) loops endlessly because the release path below
			// only ramps gain while the buffer keeps looping under it. With it,
			// the voice plays out its current buffer once and stops; the release
			// envelope still fades it naturally. Genuine ambient loops are also
			// keyed off intentionally by the game, so stopping them here is right.
			if (voice->looping)
			{
				alSourcei(alSource, AL_LOOPING, AL_FALSE);
				voice->looping = 0;
			}

			if (g_SpuAdsrEnabled && voice->hasEnvelope && voice->envPhase != ENV_OFF)
			{
				// Let the release phase ring out; the tick stops the source
				// once the envelope reaches zero (or the buffer ends, now that
				// looping is off).
				EnvelopeKeyOff(voice);
			}
			else
			{
				alSourceStop(alSource);
			}
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

// With the ADSR envelope engaged, a keyed-off voice keeps its OpenAL source
// PLAYING while the release tail rings out. The game's libsd "wait until
// SPU_OFF" / "wait for silence" logic (smf_snd/smf_io voice scans, the cutscene
// dialogue gate) would then see the voice as busy for the whole audible
// ringout -> the 30s end-of-dialogue freeze. PSX SpuGetKeyStatus reflects the
// key on/off latch, which goes OFF immediately at key-off (the voice still
// rings out in release but reads SPU_OFF). Mirror that: a keyed-off enveloped
// voice (phase RELEASE/OFF) reads free at once, exactly as the ADSR-disabled
// path does (key-off -> alSourceStop -> STOPPED), while the envelope fades the
// audio out on its own.
static int spual_voice_keyed_on(const SPUALVoice* voice)
{
	int ph = voice->envPhase;
	return ph == ENV_ATTACK || ph == ENV_DECAY || ph == ENV_SUSTAIN;
}

int PsyX_SPUAL_GetKeyStatus(u_int voice_bit)
{
	int playing = 0;
	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if (voice_bit != SPU_VOICECH(i))
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		if (alSource == AL_NONE)
			break; // SpuOff?

		if (g_SpuAdsrEnabled && voice->hasEnvelope)
		{
			playing = spual_voice_keyed_on(voice);
		}
		else
		{
			int state = AL_STOPPED;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);
			playing = (state == AL_PLAYING);
		}
		break;
	}

	SDL_UnlockMutex(g_SpuMutex);

	return playing;
}

void PsyX_SPUAL_GetAllKeysStatus(char* status)
{
	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
		{
			status[i] = 0; // SpuOff?
			continue;
		}

		if (g_SpuAdsrEnabled && voice->hasEnvelope)
		{
			status[i] = spual_voice_keyed_on(voice);
			continue;
		}

		int state;
		alGetSourcei(alSource, AL_SOURCE_STATE, &state);
		status[i] = (state == AL_PLAYING);
	}
	SDL_UnlockMutex(g_SpuMutex);
}

int PsyX_SPUAL_SetReverb(int on_off)
{
	int old_state = g_enableSPUReverb;
	g_enableSPUReverb = on_off;

	if (!g_spuInit)
		return old_state;
#if !defined(__EMSCRIPTEN__) && !defined(__SWITCH__)
	// switch if needed
	if (g_ALEffectsSupported && old_state != g_enableSPUReverb)
	{
		if (g_enableSPUReverb)
		{
			alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
		}
		else
		{
			g_currEffectSlotIdx = 0;
			alAuxiliaryEffectSloti(g_ALEffectSlots[0], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
			alAuxiliaryEffectSloti(g_ALEffectSlots[1], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
		}
	}
#endif
	return old_state;
}

int PsyX_SPUAL_GetReverbState()
{
	return g_enableSPUReverb;
}

u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return 0;

	if (!g_ALEffectsSupported)
		return 0;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
			continue;

		voice->reverb = on_off > 0;
#ifndef __EMSCRIPTEN__
		if (on_off)
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, g_ALEffectSlots[g_currEffectSlotIdx], 0, AL_FILTER_NULL);
		else
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
#endif // __EMSCRIPTEN__
	}

	SDL_UnlockMutex(g_SpuMutex);

	return 0;
}

u_int PsyX_SPUAL_GetReverbVoice()
{
	u_int bits = 0;
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		if (voice->reverb)
			bits |= SPU_KEYCH(i);
	}
	return bits;
}