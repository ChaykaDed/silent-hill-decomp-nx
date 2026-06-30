#ifndef EMULATOR_PUBLIC_H
#define EMULATOR_PUBLIC_H

#include <stdio.h>	/* FILE — PsyX_Log_SetStream */

#define CONTROLLER_MAP_FLAG_AXIS		0x4000
#define CONTROLLER_MAP_FLAG_INVERSE		0x8000

typedef struct
{
	int id;

	int kc_square, kc_circle, kc_triangle, kc_cross;

	int kc_l1, kc_l2, kc_l3;
	int kc_r1, kc_r2, kc_r3;

	int kc_start, kc_select;

	int kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;
} PsyXKeyboardMapping;

typedef struct
{
	int id;

	// you can bind axis by adding CONTROLLER_MAP_AXIS_FLAG
	int gc_square, gc_circle, gc_triangle, gc_cross;

	int gc_l1, gc_l2, gc_l3;
	int gc_r1, gc_r2, gc_r3;

	int gc_start, gc_select;

	int gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	int gc_axis_left_x, gc_axis_left_y;
	int gc_axis_right_x, gc_axis_right_y;
} PsyXControllerMapping;

typedef void(*GameDebugKeysHandlerFunc)(int nKey, char down);
typedef void(*GameDebugMouseHandlerFunc)(int x, int y);
typedef void(*GameOnTextInputHandler)(const char* buf);

//------------------------------------------------------------------------

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

/* Mapped inputs */
extern PsyXControllerMapping		g_cfg_controllerMapping;
extern PsyXControllerMapping		g_cfg_controllerMapping2;	/* PC: secondary controller binds (second button per action, AND-combined) */
extern PsyXKeyboardMapping			g_cfg_keyboardMapping;
extern PsyXKeyboardMapping			g_cfg_keyboardMapping2;		/* PC: secondary keyboard binds (active w/ allow_mouse_secondary) */
extern int							g_cfg_controllerToSlotMapping[2];
extern int							g_cfg_controllerMovement;	/* PC: 0=analog 1=dpad 2=both */
extern int							g_cfg_allowMouseSecondary;	/* PC: 1 = secondary + mouse-button binds active */
extern unsigned short				g_cfg_mouseButtonMask[8];	/* PC: [SDL button 1..5] -> PSX button bitmask */

/* Game inputs */
extern GameOnTextInputHandler		g_cfg_gameOnTextInput;

/* Graphics configuration */
extern int							g_cfg_swapInterval;
extern int							g_cfg_pgxpZBuffer;
extern int							g_cfg_bilinearFiltering;
extern int							g_cfg_affineTextures;
extern int							g_cfg_psxDither;
extern int							g_cfg_pgxpTextureCorrection;

/* PC port: MSAA sample count for the default framebuffer (0 = off, 2/4/8). Must
 * be set BEFORE PsyX_Initialise — it drives the SDL multisample GL attributes at
 * context-creation time. */
extern int							g_cfg_msaaSamples;

/* PC port: full-screen post-process look (0 = off, 1.. = a built-in filter).
 * Safe to change at runtime (launcher config + F2 in-game cycle). */
extern int							g_cfg_postProcess;

/* PC port: tone-map operator on the final image (0=off,1=Reinhard,2=ACES,
 * 3=Filmic). Runtime-settable (launcher config + F3 in-game cycle). */
extern int							g_cfg_tonemap;

/* PC port: per-pixel (fragment-shader) flashlight cone. Runtime-settable
 * (launcher config + F4 in-game toggle). */
extern int							g_PsyX_UsePerPixelFlashlight;

/* PC port: per-pixel flashlight cone parameters, pushed once per frame by game
 * code (bodyprog world-lighting setup). Position and direction are in VIEW
 * (camera) space — the same space as the per-vertex GrVertex.vsx/vsy/vsz the GTE
 * captures. The shader only consumes these when (g_PsyX_UsePerPixelFlashlight &&
 * g_PsyX_FlashlightActive). g_PsyX_FlashlightActive defaults 0 so nothing is lit
 * until the game pushes a valid light for the frame. */
extern int							g_PsyX_FlashlightActive;     /* 1 = push light this frame */
extern float						g_PsyX_FlashlightPos[3];     /* view-space xyz */
extern float						g_PsyX_FlashlightDir[3];     /* view-space unit dir the cone points along */
extern float						g_PsyX_FlashlightColor[3];   /* additive RGB at full strength */
extern float						g_PsyX_FlashlightInnerCos;   /* cos(inner half-angle) */
extern float						g_PsyX_FlashlightOuterCos;   /* cos(outer half-angle) */
extern float						g_PsyX_FlashlightRange;      /* distance falloff, view-space units */

/* PC port (Silent Hill): runtime master gate for PGXP perspective correction.
 * Set this from game code AFTER PsyX_Initialise. When the binary is built with
 * USE_PGXP=1 but this is 0, the prim emitters write a_zw=0 so the shader takes
 * the 2D-ortho (affine PSX-look) branch. */
extern int							g_PsxUsePgxp;

/* PC port: PGXP shadow-memory model (DuckStation-faithful). The GTE records the
 * precise projection of each stored vertex word keyed by its native address
 * (Shadow_Store, via the gte_stsxy* macros); drawers propagate that shadow when
 * they copy a vertex word into a prim field (Shadow_Copy); the GPU resolves each
 * prim vertex by address at draw. Entirely inside PsyCross; no effect when off. */
#if defined(__cplusplus)
extern "C" {
#endif
void Shadow_Store(void* addr, float x, float y, float w, unsigned value);
void Shadow_Copy(void* dst, const void* src);
void PGXP_FrameReset(void);
void PGXP_CoverageTick(void); /* per-frame; dumps [PGXP] det/miss when on */
float PGXP_GetSzMax(void);    /* prev-frame max SZ for shader depth normalize */
#if defined(__cplusplus)
}
#endif

/* PC port: per-frame override that suppresses dither even when
 * g_cfg_psxDither is enabled. Game code sets this to 1 on 2D-only states
 * (logos, menus, options, save/load, map screen, inventory) so the dither
 * pattern doesn't visibly chew up flat-shaded 2D art. Cleared back to 0
 * during 3D gameplay so the PSX look is preserved. The render path
 * effectively uses (g_cfg_psxDither && !g_PsxDitherSuppressed). */
extern int							g_PsxDitherSuppressed;

/* Debug inputs */
extern GameDebugKeysHandlerFunc		g_dbg_gameDebugKeys;
extern GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse;

/* PC port: route PsyX logging into the host's stdio stream (pass NULL to
 * silence PsyX logging entirely). Call BEFORE PsyX_Initialise so PsyX never
 * creates its own "<appName>.log". PsyX will flush but never fclose an
 * externally-provided stream — the host owns its handle. */
extern void PsyX_Log_SetStream(FILE* stream);

/* Usually called at the beginning of main function */
extern void PsyX_Initialise(char* windowName, int screenWidth, int screenHeight, int fullscreen);

/* Cleans all resources and closes open instances */
extern void PsyX_Shutdown(void);

/* Returns the screen size dimensions */
extern void PsyX_GetScreenSize(int* screenWidth, int* screenHeight);

/* Sets mouse cursor position */
extern void PsyX_SetCursorPosition(int x, int y);

/* Usually called after ClearOTag/ClearOTagR */
extern char PsyX_BeginScene(void);

/* Usually called after DrawOTag/DrawOTagEnv */
extern void PsyX_EndScene(void);

/* Explicitly updates emulator input loop */
extern void PsyX_UpdateInput(void);

/* Returns keyboard mapping index */
extern int PsyX_LookupKeyboardMapping(const char* str, int default_value);

/* Returns controller mapping index */
extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);

/* Screen size of emulated PSX viewport with widescreen offsets */
extern void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect);

/* Waits for timer */
extern void PsyX_WaitForTimestep(int count);

/* Changes swap interval state */
extern void PsyX_EnableSwapInterval(int enable);

/* Changes swap interval interval interval */
extern void PsyX_SetSwapInterval(int interval);

/* 1 while Cross (A) or Start is held on any connected game controller.
 * For blocking loops (FMV playback) that run outside the game's pad
 * polling and otherwise only see the keyboard. */
extern int PsyX_Pad_SkipButtonHeld(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif