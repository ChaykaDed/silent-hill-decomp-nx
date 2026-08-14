#define HAVE_M_PI
#include "../PsyX_main.h"
#include "../audio/PsyX_SPUAL_ext.h"
#include "psx/libapi.h"
#include "psx/libetc.h"

static int s_spu_EVdma = 0;
static int s_inTransfer = 0;
static int s_transferMode = SPU_TRANSFER_BY_DMA;
static SpuTransferCallbackProc s_transferCallback = NULL;

unsigned int SpuWrite(unsigned char* addr, unsigned int size)
{
	unsigned int result = PsyX_SPUAL_Write(addr, size);

	if (s_transferCallback)
		s_transferCallback();
	else
		s_inTransfer = 0;

	return result;
}

unsigned int SpuRead(unsigned char* addr, unsigned int size)
{
	return PsyX_SPUAL_Read(addr, size);
}

int SpuSetTransferMode(int mode)
{
	s_transferMode = PsyX_SPUAL_SetTransferMode(mode);
	return s_transferMode;
}

unsigned int SpuSetTransferStartAddr(unsigned int addr)
{
	return PsyX_SPUAL_SetTransferStartAddr(addr);
}

int SpuIsTransferCompleted(int flag)
{
#if 0
	int event = 0;

	if (s_transferMode == 1 || s_inTransfer == 1)
		return 1;

	event = TestEvent(s_spu_EVdma);
	if (flag == 1)
	{
		if (event != 0)
		{
			s_inTransfer = 1;
			return 1;
		}
		else
		{
			do
			{
				event = TestEvent(s_spu_EVdma);
			} while (event == 0);

			s_inTransfer = 1;
			return 1;
		}
	}

	if (event == 1)
		s_inTransfer = 1;

	return event;
#else
	return 1;
#endif
}

void SpuInit(void)
{
	ResetCallback();
#if 0
	if (s_spu_isCalled == 0)
	{
		s_spu_isCalled = 1;
		EnterCriticalSection();
		_SpuDataCallback(_spu_FiDMA);
		s_spu_EVdma = OpenEvent(HwSPU, EvSpCOMP, EvMdNOINTR, NULL);
		EnableEvent(_spu_EVdma);
		ExitCriticalSection();
	}
#endif
	PsyX_SPUAL_InitSound();
}

void SpuQuit(void)
{
	PsyX_SPUAL_ShutdownSound();
}

void SpuSetVoiceAttr(SpuVoiceAttr *arg)
{
	PsyX_SPUAL_SetVoiceAttr(arg);
}

void SpuGetVoiceAttr(SpuVoiceAttr *arg)
{
	PsyX_SPUAL_GetVoiceAttr(arg);
}

void SpuSetKey(int on_off, unsigned int voice_bit)
{
	PsyX_SPUAL_SetKey(on_off, voice_bit);
}

int SpuGetKeyStatus(unsigned int voice_bit)
{
	return PsyX_SPUAL_GetKeyStatus(voice_bit);
}

void SpuGetAllKeysStatus(char* status)
{
	PsyX_SPUAL_GetAllKeysStatus(status);
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr* attr)
{
	SpuSetVoiceAttr(attr);
	SpuSetKey(SPU_ON, attr->voice);
}

int SpuSetMute(int on_off)
{
	return PsyX_SPUAL_SetMute(on_off);
}

int SpuSetReverb(int on_off)
{
	return PsyX_SPUAL_SetReverb(on_off);
}

int SpuGetReverb(void)
{
	return PsyX_SPUAL_GetReverbState();
}

int SpuSetReverbModeParam(SpuReverbAttr* attr)
{
	/* The game drives reverb through this constantly: per-track depths on BGM
	 * bank loads, the per-tick depth RAMP on sequence (re)start (libsd
	 * replay_reverb_set — the PSX "music fades in with its echo"), and the
	 * boot-time mode set. Returns 0 = success (a nonzero return makes
	 * SdUtSetReverbType report failure to the game). */
	if (attr->mask & SPU_REV_MODE)
		PsyX_SPUAL_SetReverbMode(attr->mode & 0xFF);

	if (attr->mask & (SPU_REV_DEPTHL | SPU_REV_DEPTHR))
		PsyX_SPUAL_SetReverbDepthMasked(
			(attr->mask & SPU_REV_DEPTHL) != 0,
			(attr->mask & SPU_REV_DEPTHR) != 0,
			attr->depth.left, attr->depth.right);

	return 0;
}

void SpuGetReverbModeParam(SpuReverbAttr* attr)
{
	if (!attr)
		return;
	PsyX_SPUAL_GetReverbModeParam(attr);
}

int SpuSetReverbDepth(SpuReverbAttr* attr)
{
	if (attr->mask & (SPU_REV_DEPTHL | SPU_REV_DEPTHR))
		PsyX_SPUAL_SetReverbDepthMasked(
			(attr->mask & SPU_REV_DEPTHL) != 0,
			(attr->mask & SPU_REV_DEPTHR) != 0,
			attr->depth.left, attr->depth.right);
	return 0;
}

int SpuReserveReverbWorkArea(int on_off)
{
	return 1;
}

int SpuIsReverbWorkAreaReserved(int on_off)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned int SpuSetReverbVoice(int on_off, unsigned int voice_bit)
{
	return PsyX_SPUAL_SetReverbVoice(on_off, voice_bit);
}

unsigned int SpuGetReverbVoice(void)
{
	return PsyX_SPUAL_GetReverbVoice();
}

int SpuClearReverbWorkArea(int mode)
{
	(void)mode;
	return PsyX_SPUAL_ClearReverbWorkArea();
}

void SpuSetCommonAttr(SpuCommonAttr* attr)
{
	PsyX_SPUAL_SetCommonAttr(attr);
}

int SpuInitMalloc(int num, char* top)
{
	return PsyX_SPUAL_InitAlloc(num, top);
}

int SpuMalloc(int size)
{
	return PsyX_SPUAL_Alloc(size);
}

int SpuMallocWithStartAddr(unsigned int addr, int size)
{
	return PsyX_SPUAL_AllocAt(addr, size);
}

void SpuFree(unsigned int addr)
{
	PsyX_SPUAL_Free(addr);
}

unsigned int SpuFlush(unsigned int ev)
{
	//PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetCommonMasterVolume(short mvol_left, short mvol_right)// (F)
{
	SpuCommonAttr attr = {0};
	attr.mask = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR |
	            SPU_COMMON_MVOLMODEL | SPU_COMMON_MVOLMODER;
	attr.mvol.left = mvol_left;
	attr.mvol.right = mvol_right;
	attr.mvolmode.left = SPU_VOICE_DIRECT16;
	attr.mvolmode.right = SPU_VOICE_DIRECT16;
	PsyX_SPUAL_SetCommonAttr(&attr);
}

int SpuSetReverbModeType(int mode)
{
	PsyX_SPUAL_SetReverbMode(mode);
	return 0;
}

void SpuSetReverbModeDepth(short depth_left, short depth_right)
{
	PsyX_SPUAL_SetReverbDepthMasked(1, 1, depth_left, depth_right);
}

void SpuGetVoiceVolume(int vNum, short* volL, short* volR)
{
	PsyX_SPUAL_GetVoiceVolume(vNum, volL, volR);
}

void SpuGetVoicePitch(int vNum, unsigned short* pitch)
{
	PsyX_SPUAL_GetVoicePitch(vNum, pitch);
}

#define VOICE_ATTRIB_SETTER_SHORTCUT(flag, field, value) \
	SpuVoiceAttr attr; \
	attr.voice = SPU_VOICECH(vNum); \
	attr.mask = flag; \
	attr.field = value; \
	SpuSetVoiceAttr(&attr)

void SpuSetVoiceVolume(int vNum, short volL, short volR)
{
	SpuVoiceAttr attr;

	attr.mask = SPU_VOICE_VOLL | SPU_VOICE_VOLR;
	attr.voice = SPU_VOICECH(vNum);
	attr.volume.left = volL;
	attr.volume.right = volR;

	SpuSetVoiceAttr(&attr);
}

void SpuSetVoicePitch(int vNum, unsigned short pitch)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_PITCH, pitch, pitch);
}

void SpuSetVoiceStartAddr(int vNum, unsigned int startAddr)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_WDSA, addr, startAddr);
}

void SpuSetVoiceAR(int vNum, unsigned short AR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_AR, ar, AR);
}

extern void SpuSetVoiceDR(int vNum, unsigned short DR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_DR, dr, DR);
}

extern void SpuSetVoiceSR(int vNum, unsigned short SR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_SR, sr, SR);
}

void SpuSetVoiceRR(int vNum, unsigned short RR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_RR, rr, RR);
}

extern void SpuSetVoiceSL(int vNum, unsigned short SL)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_SL, sl, SL);
}

void SpuSetVoiceADSRAttr(int vNum,
	unsigned short AR, unsigned short DR,
	unsigned short SR, unsigned short RR,
	unsigned short SL,
	int ARmode, int SRmode, int RRmode)
{
	SpuVoiceAttr attr;

	attr.mask = SPU_VOICE_ADSR_AR | SPU_VOICE_ADSR_DR | 
				SPU_VOICE_ADSR_SR | SPU_VOICE_ADSR_RR | 
				SPU_VOICE_ADSR_SL |
				SPU_VOICE_ADSR_AMODE | SPU_VOICE_ADSR_SMODE | SPU_VOICE_ADSR_RMODE;

	attr.voice = SPU_VOICECH(vNum);
	attr.ar = AR;
	attr.dr = DR;
	attr.sr = SR;
	attr.rr = RR;
	attr.sl = SL;
	attr.a_mode = ARmode;
	attr.s_mode = SRmode;
	attr.r_mode = RRmode;

	SpuSetVoiceAttr(&attr);
}

SpuTransferCallbackProc SpuSetTransferCallback(SpuTransferCallbackProc func)
{
	SpuTransferCallbackProc oldFn = s_transferCallback;
	s_transferCallback = func;
	return oldFn;
}

int SpuReadDecodedData(SpuDecodedData * d_data, int flag)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int SpuSetIRQ(int on_off)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned int SpuSetIRQAddr(unsigned int x)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

SpuIRQCallbackProc SpuSetIRQCallback(SpuIRQCallbackProc x)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetCommonCDMix(int cd_mix)
{
	SpuCommonAttr attr = {0};
	attr.mask = SPU_COMMON_CDMIX;
	attr.cd.mix = cd_mix;
	PsyX_SPUAL_SetCommonAttr(&attr);
}

void SpuSetCommonCDVolume(short cd_left, short cd_right)
{
	SpuCommonAttr attr = {0};
	attr.mask = SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;
	attr.cd.volume.left = cd_left;
	attr.cd.volume.right = cd_right;
	PsyX_SPUAL_SetCommonAttr(&attr);
}

void SpuSetCommonCDReverb(int cd_reverb)
{
	SpuCommonAttr attr = {0};
	attr.mask = SPU_COMMON_CDREV;
	attr.cd.reverb = cd_reverb;
	PsyX_SPUAL_SetCommonAttr(&attr);
}
