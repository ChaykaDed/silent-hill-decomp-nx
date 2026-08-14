#include "audio/PsyX_SPUAL_ext.h"

#include <cassert>

static int g_legacyInit;
static int g_softwareInit;
static int g_softwareRenderer = -1;
static int g_legacyAdsr = -1;

extern "C"
{

int PsyX_SPULegacy_InitSound() { ++g_legacyInit; return 1; }
int PsyX_SPUSoftware_InitSound() { ++g_softwareInit; return 1; }
void PsyX_SPULegacy_ShutdownSound() {}
void PsyX_SPUSoftware_ShutdownSound() {}

#define BACKEND_PAIR_RETURN(type, name, params, args, value) \
	type PsyX_SPULegacy_##name params { return value; } \
	type PsyX_SPUSoftware_##name params { return value; }
#define BACKEND_PAIR_VOID(name, params, args) \
	void PsyX_SPULegacy_##name params {} \
	void PsyX_SPUSoftware_##name params {}

BACKEND_PAIR_RETURN(int, Alloc, (int), (), 0)
BACKEND_PAIR_RETURN(int, InitAlloc, (int, char*), (), 0)
BACKEND_PAIR_VOID(Free, (u_int), ())
BACKEND_PAIR_RETURN(u_int, Write, (u_char*, u_int), (), 0)
BACKEND_PAIR_RETURN(u_int, Read, (u_char*, u_int), (), 0)
BACKEND_PAIR_RETURN(u_int, SetTransferStartAddr, (u_int), (), 0)
BACKEND_PAIR_VOID(GetVoiceVolume, (int, short*, short*), ())
BACKEND_PAIR_VOID(GetVoicePitch, (int, u_short*), ())
BACKEND_PAIR_VOID(SetVoiceAttr, (SpuVoiceAttr*), ())
BACKEND_PAIR_VOID(GetVoiceAttr, (SpuVoiceAttr*), ())
BACKEND_PAIR_VOID(SetKey, (int, u_int), ())
BACKEND_PAIR_VOID(Update, (), ())
void PsyX_SPULegacy_SetAdsrEnabled(int on) { g_legacyAdsr = on; }
void PsyX_SPUSoftware_SetAdsrEnabled(int) {}
BACKEND_PAIR_RETURN(int, GetAdsrEnabled, (), (), 1)
BACKEND_PAIR_VOID(SetOutputMode, (int), ())
BACKEND_PAIR_RETURN(int, ApplyOutputMode, (int), (), 0)
BACKEND_PAIR_RETURN(int, GetOutputMode, (), (), 1)
BACKEND_PAIR_RETURN(int, GetSurroundActive, (), (), 0)
BACKEND_PAIR_VOID(SetNextKeyOnAzimuth, (int), ())
BACKEND_PAIR_VOID(ClearNextKeyOnAzimuth, (), ())
BACKEND_PAIR_VOID(SetVoiceAzimuth, (int, int), ())
BACKEND_PAIR_RETURN(int, GetKeyStatus, (u_int), (), 0)
BACKEND_PAIR_VOID(GetAllKeysStatus, (char*), ())
BACKEND_PAIR_RETURN(int, SetMute, (int), (), 0)
BACKEND_PAIR_RETURN(int, SetReverb, (int), (), 0)
BACKEND_PAIR_RETURN(int, GetReverbState, (), (), 0)
BACKEND_PAIR_RETURN(u_int, SetReverbVoice, (int, u_int), (), 0)
BACKEND_PAIR_RETURN(u_int, GetReverbVoice, (), (), 0)
BACKEND_PAIR_VOID(SetReverbDepthMasked, (int, int, short, short), ())
BACKEND_PAIR_RETURN(int, SetReverbMode, (int), (), 0)
BACKEND_PAIR_VOID(SetReverbDepthScale, (float), ())
BACKEND_PAIR_RETURN(float, GetReverbDepthScale, (), (), 1.0f)

#undef BACKEND_PAIR_VOID
#undef BACKEND_PAIR_RETURN

int PsyX_SPUSoftware_AllocAt(u_int, int) { return 0; }
int PsyX_SPUSoftware_SetTransferMode(int mode) { return mode; }
void PsyX_SPUSoftware_SetCommonAttr(SpuCommonAttr*) {}
void PsyX_SPUSoftware_GetCommonAttr(SpuCommonAttr*) {}
void PsyX_SPUSoftware_GetReverbModeParam(SpuReverbAttr*) {}
int PsyX_SPUSoftware_ClearReverbWorkArea() { return 0; }
void PsyX_SPUSoftware_ConfigureOutput(int, int, int, int) {}
int PsyX_SPUSoftware_ConfigureRenderer(int renderer, int, int, int)
{
	g_softwareRenderer = renderer;
	return 1;
}
int PsyX_SPUSoftware_PushXaFrames(const short*, u_int, int, int) { return 17; }
void PsyX_SPUSoftware_ResetXa() {}
void PsyX_SPUSoftware_FinishXa() {}
int PsyX_SPUSoftware_IsXaDrained() { return 0; }
void PsyX_SPUSoftware_SetXaMasterGain(double) {}
void PsyX_SPUSoftware_SetXaPaused(int) {}
u_int PsyX_SPUSoftware_GetQueuedXaFrames() { return 23; }

}

int main()
{
	assert(PsyX_SPUAL_InitSound() == 1);
	assert(g_legacyInit == 1 && g_softwareInit == 0);
	assert(PsyX_SPUAL_ConfigureRenderer(1, 0, 0, 0) == 0);
	PsyX_SPUAL_ShutdownSound();

	assert(PsyX_SPUAL_ConfigureRenderer(4, 0, 0, 0) == 0);
	assert(PsyX_SPUAL_ConfigureRenderer(1, 0, 0, 0) == 1);
	assert(g_softwareRenderer == 0);
	assert(PsyX_SPUAL_InitSound() == 1);
	assert(g_softwareInit == 1);
	PsyX_SPUAL_ShutdownSound();

	assert(PsyX_SPUAL_ConfigureRenderer(0, 0, 0, 0) == 1);
	PsyX_SPUAL_SetAdsrEnabled(7);
	assert(g_legacyAdsr == 7);
	short sample = 0;
	assert(PsyX_SPUAL_PushXaFrames(&sample, 1, 37800, 1) == 0);
	assert(PsyX_SPUAL_IsXaDrained() == 1);

	assert(PsyX_SPUAL_ConfigureRenderer(3, 0, 0, 0) == 1);
	assert(g_softwareRenderer == 2);
	assert(PsyX_SPUAL_PushXaFrames(&sample, 1, 37800, 1) == 17);
	assert(PsyX_SPUAL_GetQueuedXaFrames() == 23);
	return 0;
}
