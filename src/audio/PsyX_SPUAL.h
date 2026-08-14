#ifndef PSYX_SPUAL_H
#define PSYX_SPUAL_H

#include "psx/types.h"
#include "psx/libspu.h"
#include "PsyX/PsyX_config.h"
#include "PsyX/common/pgxp_defs.h"

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern int PsyX_SPUAL_InitSound();
extern void PsyX_SPUAL_ShutdownSound();

// Private
extern int PsyX_SPUAL_Alloc(int size);
extern int PsyX_SPUAL_InitAlloc(int num, char* top);
extern void PsyX_SPUAL_Free(u_int addr);
extern u_int PsyX_SPUAL_Write(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_Read(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr);

extern void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR);
extern void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch);
extern void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib);
extern void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* psxAttrib);
extern void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit);
extern void PsyX_SPUAL_Update();
extern void PsyX_SPUAL_SetAdsrEnabled(int on);
extern int  PsyX_SPUAL_GetAdsrEnabled(void);

/* Speaker layout (0=auto 1=stereo 2=quad 3=5.1 4=7.1 5=hrtf). Set latches a
 * request for init; Apply renegotiates a live device (alcResetDeviceSOFT).
 * Get returns the ACHIEVED layout, which is what routing must trust. */
extern void PsyX_SPUAL_SetOutputMode(int mode);
extern int  PsyX_SPUAL_ApplyOutputMode(int mode);
extern int  PsyX_SPUAL_GetOutputMode(void);
extern int  PsyX_SPUAL_GetSurroundActive(void);

/* Emitter azimuth side-channel (PSX Q12 angle: 0 = ahead, positive = right).
 * SetNext arms the sound about to key on; SetVoiceAzimuth live-updates an
 * already-identified voice (Sd_SfxAttributesUpdate path). */
extern void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuthQ12);
extern void PsyX_SPUAL_ClearNextKeyOnAzimuth(void);
extern void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12);

extern int PsyX_SPUAL_GetKeyStatus(u_int voice_bit);
extern void PsyX_SPUAL_GetAllKeysStatus(char* status);

extern int PsyX_SPUAL_SetMute(int on_off);
extern int PsyX_SPUAL_SetReverb(int on_off);
extern int PsyX_SPUAL_GetReverbState();
extern u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit);
extern u_int PsyX_SPUAL_GetReverbVoice();
extern void PsyX_SPUAL_SetReverbDepthMasked(int maskL, int maskR, short depthL, short depthR);
extern int PsyX_SPUAL_SetReverbMode(int mode);
extern void PsyX_SPUAL_SetReverbDepthScale(float scale);
extern float PsyX_SPUAL_GetReverbDepthScale(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif // PSYX_SPUAL_H