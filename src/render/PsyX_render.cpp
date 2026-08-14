#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "PsyX/PsyX_public.h"

#include "../platform.h"
#include "../gpu/PsyX_GPU.h"
#include "psx/gtereg.h"

#include "PsyX/PsyX_render.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/util/timer.h"

#include <assert.h>
#include <string.h>
#if defined(__SWITCH__)
#include <SDL2/SDL.h>
extern "C" void svcOutputDebugString(const char* str, size_t len);
#endif

#ifdef _WIN32

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif //def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO					1
#define USE_OFFSCREEN_BLIT		1
#define USE_FRAMEBUFFER_BLIT	1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as framebuffer, so it's disabled
#define USE_PBO					(OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT		(OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT	(OGLES_VERSION == 3)

#endif

extern SDL_Window* g_window;


#define MAX_NUM_VERTEX_BUFFERS		(2)
#define PSX_SCREEN_ASPECT	(240.0f / 320.0f)			// PSX screen is mapped always to this aspect

/* Pixel aspect ratio: 1 horizontal ortho unit / 1 vertical ortho unit
 * ratio to apply on top of the Hor+ widening. PSX renders to a 320×240
 * framebuffer; displayed on a 4:3 CRT (320/240 = 4/3 matches the CRT
 * aspect exactly), each PSX pixel is SQUARE — so PAR=1.0 is the actual
 * pixel aspect for 320×240 PSX content on a 4:3 NTSC CRT. The 1.09375
 * value previously used here was over-correcting and making characters
 * ~9% taller than they appear on a real PSX (visible on Harry's torso
 * relative to a CRT screenshot at the same location).
 *
 * Runtime-settable so the host can override from config. Cull-bound
 * sites (vw_calc.c, Gfx_MeshDraw) read this same global so culling
 * tracks the active ortho. Default 1.0 = matches 320×240 PSX CRT;
 * 1.094 = some emulators' approximation; 1.143 = 8:7 (overscan look). */
extern "C" {
float g_PsxPixelAspect = 1.0f;
/* Vertical FOV scale for the 3D gameplay world (see GR_SetOffscreenState). Our render
 * shows ~14.7%% too much vertical world vs PSX/DuckStation (measured 0.872 vertical /
 * 1.0 horizontal at a fixed 4:3 spot, extra at the bottom). 0.872 crops the world ortho
 * top-anchored to match; 1.0 = no crop (old behavior). Console `vfov <n>`. */
float g_PsxWorldVScale = 0.872f;
/* Vertical view shift (amount, PSX screen-Y units) applied to FIXED-ANGLE camera shots
 * only — gated by g_PsxFixedCamActive, which the game sets when cur_cam_mv_type ==
 * VC_MV_FIX_ANG. Those shots frame the top of the scene clipped vs PSX (e.g. a medkit
 * off the top); the GTE projects geometry up to screen_y ~ -78, so shifting the ortho
 * window up brings it into frame. + = view up. Console `vshift`. Chase/settle/etc. are
 * unaffected. */
float g_PsxWorldVShift = 20.0f;
int   g_PsxFixedCamActive = 0;
/* Set by the game while a cutscene is active. Cutscenes frame themselves with letterbox
 * bars, so the gameplay vertical crop (g_PsxWorldVScale) is skipped while this is set —
 * otherwise it scaled/clipped the bars + subtitles off the bottom of the frame. */
int   g_PsxCutsceneActive = 0;
/* Set by the game around the 2D-UI ordering-table draw (OrderingTable2: map-message
 * subtitles, screen fade, cutscene letterbox bars). When set, GR_SetOffscreenState
 * draws that pass at FULL vertical ortho (vscale 1.0) so it isn't clipped by the Hor+
 * world crop. The 3D world (OrderingTable0) keeps the crop, so 3D framing — including
 * cutscenes — is unchanged. This replaces the old g_PsxCutsceneActive vscale skip
 * (which un-cropped the whole cutscene frame and looked stretched). */
int   g_PsxUIOrthoPass = 0;
/* 3D-world HORIZONTAL ortho scale (Hor+ widescreen only). 1.0 = identity (current
 * behaviour); >1 narrows the ortho around center = wider models, <1 = narrower. Pure
 * tuning/preference knob, default neutral. Console `hfov`; not applied to the UI pass. */
float g_PsxWorldHScale = 1.0f;
}
#define PSX_NTSC_PIXEL_ASPECT (g_PsxPixelAspect)

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousStencilMode = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = { 0,0,0,0 };
/* PC port: nonzero once GR_StoreFrameBuffer has stored at least one frame in
 * g_fbTexture, so GR_UpdateVRAM can re-blit it over the framebuffer pages a
 * full vram[] re-upload just replaced with stale CPU bytes. */
static int g_fbStoreValid = 0;
RECT16 g_PreviousOffscreen = { 0,0,0,0 };

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

// Set to 1 by game code only during states that render a 3D world (InGame, MapEvent).
// When 0, GR_SetOffscreenState uses 4:3 ortho so 2D UI screens don't show VRAM garbage.
int g_PcHorPlusEnabled = 1;

/* PC port: pillarbox 2D screens (menus, load screen) with 4:3 black bars
 * instead of stretching the 4:3 content to fill a widescreen window. Set
 * from g_PcConfig.menuPillarbox in main_pc.c. Default on. */
int g_PcMenuPillarbox = 1;

/* Widescreen presentation mode for 3D gameplay (only used when g_PcHorPlusEnabled=1):
 *   0 = Pillarbox (PSX-faithful): render 4:3 content centered in a 16:9 frame
 *       with black bars on the sides. Characters and scene framing identical to
 *       the original PSX game on a 4:3 CRT, just inside a wider window.
 *   1 = Hor+ widescreen (default): keep vertical FOV, widen horizontal FOV to
 *       fill the 16:9 frame with square pixels (correct character proportions).
 *       Reveals scene content that was cropped on PSX (extra bar counter, walls
 *       beyond the framebuffer edges); per-camera tuning in s_camCorrections[]
 *       compensates per-shot to keep Harry framed like the PSX original.
 *   2 = Stretch (anamorphic): no FOV change, no bars — 4:3 content stretched
 *       to fill 16:9. Characters appear ~33% wider. Not recommended.
 * Default 1 = Hor+ with square pixels. Override from config.cfg via widescreen_mode. */
int g_PcWidescreenMode = 1;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;

/* PC port (Silent Hill): runtime master gate for PGXP. When 0,
 * MakeVertex* writes 0 into a_zw, the shader takes the `a_zw.y > 100.0`
 * else branch (2D-ortho path). Set from main_pc.c after PcConfig_Load runs
 * (config key: use_pgxp). */
int g_PsxUsePgxp = 0;
int g_cfg_bilinearFiltering = 0;
int g_cfg_affineTextures = 0;
/* When non-zero, the GPU_DITHERING macro applies the 4x4 PSX-style ordered
 * dither to every fragment regardless of the per-primitive `a_texcoord.w`
 * dither flag. Lets the PC port mimic the PSX 5-bit framebuffer noise
 * (which masks texture-page seams and gives the authentic look) on
 * primitives that don't request dither at the prim-tag level. */
int g_cfg_psxDither = 1;
int g_PsxDitherSuppressed = 0;
/* PC port: MSAA sample count for the default framebuffer. 0 = off (no
 * multisample requested), 2/4/8 = N-sample MSAA. Read in GR_InitialiseRender
 * BEFORE the GL context is created (SDL_GL_MULTISAMPLE* attributes), so the
 * host must set it from config BEFORE PsyX_Initialise. When > 0, two blits that
 * read/write the (now multisample) default framebuffer with scaling or a
 * single-sample peer become illegal — GR_StoreFrameBuffer resolves first and
 * GR_PresentLastFrame draws a fullscreen quad instead of blitting. */
/* g_cfg_msaaSamples, g_cfg_postProcess defined in main_pc.c */
extern int g_cfg_msaaSamples;
extern int g_cfg_postProcess;
#define POST_PROCESS_MODE_COUNT 8
/* PC port: tone-map operator applied as the final step of the post-process
 * shader. 0=off, 1=Reinhard, 2=ACES, 3=Filmic. F3 cycles it in-game. Defined
 * outside the PSYX_HAS_POSTPROCESS guard so non-GL builds still link. */
int g_cfg_tonemap = 0;

/* PC port: per-pixel (fragment-shader) flashlight cone instead of the PSX
 * per-vertex lighting. 0=off (per-vertex), 1=on. F4 toggles it in-game. */
int g_PsyX_UsePerPixelFlashlight = 0;

/* PC port: per-frame flashlight cone parameters (view space), set by game code.
 * The shader consumes them only when (g_PsyX_UsePerPixelFlashlight &&
 * g_PsyX_FlashlightActive). Defaults make the cone inert (active=0). */
int   g_PsyX_FlashlightActive   = 0;
float g_PsyX_FlashlightPos[3]   = { 0.0f, 0.0f, 0.0f };
float g_PsyX_FlashlightDir[3]   = { 0.0f, 0.0f, 1.0f };
float g_PsyX_FlashlightColor[3] = { 1.0f, 0.95f, 0.85f };  /* warm white; per-fragment N.L + screen-blend keep facing/near surfaces a bright hotspot while angled/far surfaces fall off naturally */
float g_PsyX_FlashlightInnerCos = 0.94f;  /* ~20 deg */
float g_PsyX_FlashlightOuterCos = 0.82f;  /* ~35 deg */
float g_PsyX_FlashlightRange    = 4000.0f;

/* PC port: live per-effect intensity -- [ lowers, ] raises, backslash switches
 * which effect (among the enabled ones); also the FLINT/POSTINT/TMINT console
 * commands. Persisted to config. */
float g_PsyX_FlashlightIntensity = 1.90f; /* cone brightness scale, 0..3 */
float g_cfg_postProcessIntensity = 1.0f; /* post-process effect mix, 0..1 */
float g_cfg_tonemapIntensity     = 1.0f; /* tonemap mix, 0..1 */

/* Defined later in the file (post-process module); called from GR_InitialisePSX
 * and PsyX_EndScene. */
void GR_InitPostProcess(void);
void GR_PostProcess(void);

int vram_need_update = 1;

/* PC port: runtime gate for framebuffer→VRAM feedback. See PsyX_render.h. */
int g_PsxSkipFramebufferStore = 0;

/* PC port: freeze-frame presentation for pause/console/message states.
 * PSX hardware never auto-cleared the framebuffer, so SH1's pause screen
 * simply stopped drawing the world and the last gameplay frame stayed
 * visible under the PAUSED text. PsyCross clears every frame, so the
 * game instead sets g_PsxPresentLastFrame while frozen: every EndScene
 * captures the composed frame into a texture (skipped on frames where
 * the capture was presented, so UI text never bakes in), and BeginScene
 * re-presents it under the new frame's prims. The game sets the flag on
 * freeze ENTRY (same tick) and clears it on exit. */
int g_PsxPresentLastFrame = 0;
static GLuint g_freezeFrameTex = 0;
static GLuint g_freezeFrameFBO = 0;
static int    g_freezeFrameW = 0;
static int    g_freezeFrameH = 0;
static int    g_freezeFrameValid = 0;
static int    g_freezePresentedThisFrame = 0;
int framebuffer_need_update = 0;

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif



#if USE_OPENGL
typedef struct
{
	GLenum fmt;
	GLuint* pbos;
	uint64_t num_pbos;
	uint64_t dx;
	uint64_t num_downloads;

	int width;
	int height;
	int nbytes; /* number of bytes in the pbo buffer. */
	unsigned char* pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO* pbo, GLenum format, int w, int h, int num)
{
	if (pbo->pbos)
	{
		eprinterr("Already initialized. Not necessary to initialize again; or shutdown first.");
		return -1;
	}

	if (0 >= num)
	{
		eprinterr("Invalid number of PBOs: %d", num);
		return -2;
	}

	pbo->fmt = format;
	pbo->width = w;
	pbo->height = h;
	pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
	if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height;
	}
	else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt)
	{
		pbo->nbytes = pbo->width * pbo->height * 3;
	}
	else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height * 4;
	}
	else
	{
		eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
		return -3;
	}

	if (pbo->nbytes == 0)
	{
		eprinterr("Invalid width or height given: %d x %d", pbo->width, pbo->height);
		return -4;
	}

	pbo->pbos = (GLuint*)malloc(sizeof(GLuint) * num);
	pbo->pixels = (u_char*)malloc(pbo->nbytes);

	glGenBuffers(num, pbo->pbos);
	for (int i = 0; i < num; ++i)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
	return 0;
}

void PBO_Destroy(GrPBO* pbo)
{
#if USE_PBO
	if(pbo->pbos)
	{
		glDeleteBuffers(pbo->num_pbos, pbo->pbos);
	
		free(pbo->pbos);
		pbo->num_pbos = 0;
		pbo->pbos = NULL;
	}

#endif
	if (pbo->pixels)
	{
		free(pbo->pixels);
		pbo->pixels = NULL;
	}

	pbo->num_downloads = 0;
	pbo->dx = 0;
	pbo->fmt = 0;
	pbo->nbytes = 0;
}

void PBO_Download(GrPBO* pbo)
{
	unsigned char* ptr;
	
#if USE_PBO
	if (pbo->num_downloads < pbo->num_pbos)
	{
		/*
		   First we need to make sure all our pbos are bound, so glMap/Unmap will
		   read from the oldest bound buffer first.
		*/
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);   /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used as offset into the buffer to read into. */
#endif
	}
	else
	{
		/* Read from the oldest bound pbo */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		ptr = (unsigned char*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (NULL != ptr)
		{
			memcpy(pbo->pixels, ptr, pbo->nbytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
			eprintwarn("Failed to map the buffer\n");

		/* Trigger the next read. */
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
	}

	++pbo->dx;
	pbo->dx = pbo->dx % pbo->num_pbos;

	pbo->num_downloads++;

	if (pbo->num_downloads == UINT64_MAX)
		pbo->num_downloads = pbo->num_pbos;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
	// FIXME: THIS is very slow
	// Do not use at all

	// glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not accidentilly using a PBO. */
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint		g_glVertexArray[2];
GLuint		g_glVertexBuffer[2];
int			g_curVertexBuffer = 0;

GLuint		g_glBlitFramebuffer;
GrPBO		g_glFramebufferPBO;

GLuint		g_glVRAMFramebuffer;

GLuint		g_glOffscreenFramebuffer;
GrPBO		g_glOffscreenPBO;

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char* windowName, int fullscreen)
{
	int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
	windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
	/* 0 = windowed, 1 = exclusive fullscreen, 2 = borderless (fullscreen
	 * desktop: covers the screen at desktop resolution, no mode switch). */
	if (fullscreen == 2)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	else if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	if(g_windowWidth <= 0 || g_windowHeight <= 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);

	/* PC port: if MSAA was requested but the driver can't provide a multisample
	 * pixel format, SDL_CreateWindow fails. Drop MSAA and retry once so the game
	 * still launches (just without antialiasing). */
	if (g_window == NULL && g_cfg_msaaSamples > 0)
	{
		eprintwarn("Window creation with %dx MSAA failed (%s); retrying without MSAA\n", g_cfg_msaaSamples, SDL_GetError());
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		g_cfg_msaaSamples = 0;
		g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);
	}

	if (g_window == NULL)
	{
		eprinterr("Failed to initialise SDL window!\n");
		return 0;
	}
	
#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
	//Override to full screen.
	SDL_DisplayMode displayMode;
	if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0)
	{
		screenWidth = displayMode.w;
		windowWidth = displayMode.w;
		screenHeight = displayMode.h;
		windowHeight = displayMode.h;
	}
#endif

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if(!SDL_GL_CreateContext(g_window))
	{
		eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n", OGLES_VERSION);
		return 0;
	}

#elif defined(RENDERER_OGL)

	int major_version = 3;
	int minor_version = 3;
	int profile = SDL_GL_CONTEXT_PROFILE_CORE;

	// find best OpenGL version
	do
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

		if (SDL_GL_CreateContext(g_window))
			break;
	
		minor_version--;
		
	} while (minor_version >= 0);

	if (minor_version == -1)
	{
		eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.\n");
		return 0;
	}
#endif

	return 1;
}
#endif

int GR_InitialiseGLExt()
{
#if !defined(__SWITCH__)
#if defined(USE_GLAD)
	GLenum err = gladLoadGL();

	if (err == 0)
		return 0;
#endif
#endif
	
	const char* rend = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	eprintf("*Video adapter: %s by %s\n", rend, vendor);

	const char* versionStr = (const char*)glGetString(GL_VERSION);
	eprintf("*OpenGL version: %s\n", versionStr);

	const char* glslVersionStr = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	eprintf("*GLSL version: %s\n", glslVersionStr);

	return 1;
}

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen)
{
	g_windowWidth = width;
	g_windowHeight = height;

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#if USE_OPENGL
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	/* PC port: request MSAA on the default framebuffer when enabled. Must be
	 * set before the window/context is created (GR_InitialiseGLContext). The
	 * driver picks a multisample pixel format; if it can't, window creation is
	 * retried without MSAA inside GR_InitialiseGLContext. */
	if (g_cfg_msaaSamples > 0)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_cfg_msaaSamples);
	}

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (!GR_InitialiseGLContext(windowName, fullscreen))
	{
		eprinterr("Failed to Initialise GL Context!\n");
		return 0;
	}
#endif

	if (!GR_InitialiseGLExt())
	{
		eprinterr("Failed to Intialise GL extensions\n");
		return 0;
	}
#endif
	
	return 1;
}

void GR_Shutdown()
{
#if USE_OPENGL
	glDeleteVertexArrays(2, g_glVertexArray);
	glDeleteBuffers(2, g_glVertexBuffer);

	PBO_Destroy(&g_glFramebufferPBO);
	PBO_Destroy(&g_glOffscreenPBO);

	glDeleteFramebuffers(1, &g_glBlitFramebuffer);
	glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
	glDeleteFramebuffers(1, &g_glVRAMFramebuffer);

	GR_DestroyTexture(g_vramTexturesDouble[0]);
	GR_DestroyTexture(g_vramTexturesDouble[1]);

	GR_DestroyTexture(g_whiteTexture);
	GR_DestroyTexture(g_fbTexture);
	GR_DestroyTexture(g_offscreenRTTexture);
#endif
}

void GR_UpdateSwapIntervalState(int swapInterval)
{
#if defined(RENDERER_OGL)
	SDL_GL_SetSwapInterval(swapInterval);
#endif
}

void GR_BeginScene()
{
	g_lastBoundTexture = 0;

#if USE_OPENGL
#ifdef RENDERER_OGLES
	glClearDepthf(1.0f);
#else
	glClearDepth(1.0f);
#endif
	glClear(GL_DEPTH_BUFFER_BIT);
	glClear(GL_STENCIL_BUFFER_BIT);
#endif

	GR_UpdateVRAM();
	GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

	if (g_dbg_wireframeMode)
	{
		GR_SetWireframe(1);

#if USE_OPENGL
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
}

void GR_EndScene()
{
	framebuffer_need_update = 1;
	
	if (g_dbg_wireframeMode)
		GR_SetWireframe(0);

#if USE_OPENGL
	glBindVertexArray(0);
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];

void GR_ResetDevice()
{
	GR_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

#if USE_OPENGL
	GLint projectionLoc;
	GLint projection3DLoc;
	GLint bilinearFilterLoc;
	GLint ditherForceLoc;
	GLint pixelScaleLoc;
	GLint texelSizeLoc;
	GLint fogColorLoc;
	GLint fogToBlackLoc;
	GLint fogStrengthLoc;
	GLint pgxpEnabledLoc;
	GLint szMaxLoc;
	GLint flashlightOnLoc;
	GLint flLightPosLoc;
	GLint flDirLoc;
	GLint flColorLoc;
	GLint flInnerCosLoc;
	GLint flOuterCosLoc;
	GLint flRangeLoc;
#endif
} GTEShader;

GTEShader g_gte_shader_4;
GTEShader g_gte_shader_8;
GTEShader g_gte_shader_16;
GTEShader g_gte_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_bilinearFilterLoc;
GLint u_ditherForceLoc;
GLint u_pixelScaleLoc;
GLint u_texelSizeLoc;
GLint u_fogColorLoc;
GLint u_fogToBlackLoc;
GLint u_fogStrengthLoc;
GLint u_pgxpEnabledLoc;
GLint u_szMaxLoc;
GLint u_flashlightOnLoc;
GLint u_flLightPosLoc;
GLint u_flDirLoc;
GLint u_flColorLoc;
GLint u_flInnerCosLoc;
GLint u_flOuterCosLoc;
GLint u_flRangeLoc;

float g_PsyX_FogColor[3] = { 0.0f, 0.0f, 0.0f };
/* World fog density multiplier. 1.0 = native PC shader fog; >1 deepens it toward the
 * PSX double-poly fog look the single-pass shader drops. Console `fogstr`; pushed as
 * u_fogStrength and applied against v_fogAmount in the fragment shader. Default 1.1 to
 * match the PSX/DuckStation fog depth (was the value baked in the original PR). */
float g_PsyX_FogStrength = 1.1f;
/* Fog mode for the CURRENT blend: 1 = fade additive/subtractive prims (blood, muzzle
 * flash, etc.) toward black so they fade OUT in fog, instead of blending toward the
 * light fog color — which whitened their edges/faded pixels in daytime. Set by
 * GR_SetBlendMode; pushed to the shader as u_fogToBlack. */
int g_PsxFogToBlack = 0;

#define GPU_PACK_RG\
	"		float color_16 = (color_rg.y * 256.0 + color_rg.x) * 255.0;\n"

#define GPU_DISCARD\
	"		if (color_16 == 0.0) { discard; }\n"

#define GPU_DECODE_RG\
	"		fragColor = fract(floor(color_16 / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"

#define GPU_PACK_RG_FUNC\
	"	const float c_PackRange = 255.001;\n"\
	"	float packRG(vec2 rg) { return (rg.y * 256.0 + rg.x) * c_PackRange;}\n"

#define GPU_DECODE_RG_FUNC\
	" vec4 decodeRG(float rg) {\n"\
	" 	vec4 value = fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"\
	" 	return vec4(value.xyz, rg == 0.0 ? rg : (1.0 - value.w * 16.0));\n"\
	" }\n"
	//"	vec4 decodeRG(float rg) { return fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0); }\n"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

/* PSX 4x4 ordered dither.
 *
 * Native hardware applies a signed offset before quantizing to the 5-bit
 * framebuffer. We match the matrix the original game's GPU uses (values
 * in [-4..+3]) divided by 255 so it lands in 24-bit color space.
 *
 * `v_texcoord.w` carries the per-primitive dither flag from the prim's
 * tpage. `u_ditherForce` is a global override the PC config exposes —
 * when non-zero, dither is applied to every fragment regardless of the
 * per-prim flag. We combine via max() so a prim that requests dither
 * still gets it even when force is 0, and a prim that didn't request
 * dither gets the global override when force is 1.
 *
 * After adding the dither offset we quantize to 5 bits per channel
 * (PSX framebuffer depth) so the noise translates to actual color
 * banding rather than staying as a smooth gradient — that's what
 * produces the visible "film grain" look. */
#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);\n"\
		"		float dStrength = max(v_texcoord.w, u_ditherForce) * v_is3d;\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    fragColor.xyz = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		}\n"

/* Same dither as GPU_DITHERING but skips the `fragColor *= v_color`
 * step — used by the main fragment shader where we now multiply by
 * v_color and apply fog BEFORE dithering, so the dither + quantize is
 * the very last operation on the fragment and the fog mix() can't
 * smooth out the quantization steps that produce the visible noise.
 *
 * 8-pixel cell, native PSX dither strength (1:1 with the original
 * matrix). Earlier resolution-scaling and 1.6x intensity made it
 * look chunkier than PSX; this version sticks closer to the
 * authentic noise level. Gated OFF for additive/subtractive prims (u_fogToBlack==1):
 * the signed dither offset over-brightened the faint low-cyan anti-aliased rim of the
 * subtractive blood decal, so it subtracted ~nothing over a light floor and leaked the
 * bright floor through as white speckled edges. Opaque geometry (u_fogToBlack==0) keeps
 * full dither. */
#	define GPU_DITHERING_NO_VCOLOR\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 8.0) * 4.0);\n"\
		"		float dStrength = max(v_texcoord.w, u_ditherForce) * v_is3d * (1.0 - float(u_fogToBlack));\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    fragColor.xyz = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		}\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { return array[idx]; }"

#else

#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"

/* GLES 2 fallback: no dither (would need ivec2 / mat4 indexing). */
#	define GPU_DITHERING_NO_VCOLOR\
		""

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { if(idx == 0) return array.x; else return array.y; }"

#endif

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC\
    "   // returns 16 bit colour\n"\
    "   float samplePSX(vec2 tc){\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (c_PackRange / 16.0);\n"\
    "       float f = floor(v);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return packRG(VRAM(clut_pos));\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns 16 bit colour\n"\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * c_PackRange * c_VRAMTexel.x;\n"\
	"		vec2 color_rg = VRAM(clut_pos);\n"\
	"		return packRG(VRAM(clut_pos));\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 color_rg = VRAM(uv);\n"\
	"		return packRG(color_rg);\n"\
	"	}\n"


#define GPU_BILINEAR_SAMPLE_FUNC \
	"	float c_textureSize = 1.0;\n"\
	"	float c_onePixel = 1.0;\n"\
	"	vec4 BilinearTextureSample(vec2 P) {\n"\
	"		vec2 frac = fract(P);\n"\
	"		vec2 pixel = floor(P);\n"\
	"		float C11 = samplePSX(pixel);\n"\
	"		float C21 = samplePSX(pixel + vec2(c_onePixel, 0.0));\n"\
	"		float C12 = samplePSX(pixel + vec2(0.0, c_onePixel));\n"\
	"		float C22 = samplePSX(pixel + vec2(c_onePixel, c_onePixel));\n"\
	"		float ax1 = mix(float(C11 > 0.0), float(C21 > 0.0), frac.x);\n"\
	"		float ax2 = mix(float(C12 > 0.0), float(C22 > 0.0), frac.x);\n"\
	"		if(mix(ax1, ax2, frac.y) < 0.5) { discard; }\n"\
	"		vec4 x1 = mix(decodeRG(C11), decodeRG(C21), frac.x);\n"\
	"		vec4 x2 = mix(decodeRG(C12), decodeRG(C22), frac.x);\n"\
	"		return mix(x1, x2, frac.y);\n"\
	"	}\n"

#define GPU_NEAREST_SAMPLE_FUNC \
	"vec4 NearestTextureSample(vec2 P) {\n"\
	"	float color_16 = samplePSX(P);\n"\
	"	if(color_16 == 0.0) {discard;}\n"\
	"	return decodeRG(color_16);\n"\
	"}\n"

/* The VRAM texture stores each 16-bit PSX pixel as two normalised bytes
 * (low/high). Every downstream step — packRG, the 4/8-bit CLUT index math,
 * and the 5-bit channel decode — treats the sampled value as an exact k/255
 * and feeds it to floor(). The Windows GL driver normalises UNSIGNED_BYTE ->
 * float precisely enough that this holds; Mesa (Steam Deck / Proton) rounds
 * slightly differently, so floor() lands one bucket off and colours / palette
 * lookups corrupt. Snap each channel to its exact integer byte right at the
 * source so all downstream math is bit-exact on every driver (a no-op where
 * normalisation is already exact). */
#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return floor(texture2D(s_texture, uv).ra * 255.0 + 0.5) * (1.0 / 255.0); }\n"
#else
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return floor(texture2D(s_texture, uv).rg * 255.0 + 0.5) * (1.0 / 255.0); }\n"
#endif

/* PGXP path (only when u_pgxpEnabled AND this vertex has a precise W>0):
 * build the SAME ortho clip position from the precise float screen X/Y, then
 * scale xyzw by W so the GPU's perspective divide returns the identical NDC
 * position but interpolates varyings (UV/colour) perspective-correctly. Depth
 * (a_zw.x) is left as the FLAT per-prim affine value so Z-ordering matches the
 * painter/submission model this renderer relies on — true per-vertex depth was
 * tried 3x (v1 67a852f / v2 f60aab4 / v3 99e5b18) and ALWAYS dropped/warped
 * coplanar triangles, because depth here is one flat value per prim and ties
 * resolve by OT order; a per-vertex subset is incompatible. Z-fight relief now
 * comes purely from un-quantising that flat depth (PsyX_SetNextPrimSz) when
 * PGXP is on. The else-branch is byte-identical to the legacy affine path. */
#define GTE_PERSPECTIVE_CORRECTION \
		"	if (u_pgxpEnabled > 0 && a_pgxp.z > 0.0) {\n"\
		"		vec4 b = Projection * vec4(a_pgxp.xy, a_zw.x, 1.0);\n"\
		"		float W = a_pgxp.z;\n"\
		"		gl_Position = vec4(b.xyz * W, b.w * W);\n"\
		"	} else {\n"\
		"		gl_Position = Projection * vec4(a_position.xy, a_zw.x, 1.0);\n"\
		"	}\n"

#define GTE_VERTEX_SHADER \
	"	attribute vec4 a_position;\n"\
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"\
	"	attribute vec4 a_color;\n"\
	"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"\
	"	attribute vec4 a_zw;\n"\
	"	attribute vec3 a_pgxp;\n"\
	"	attribute vec3 a_viewpos;\n"\
	"	uniform mat4 Projection;\n"\
	"	uniform mat4 Projection3D;\n"\
	"	uniform int u_pgxpEnabled;\n"\
	"	uniform float u_szMax;\n"\
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"\
	"	void main() {\n"\
	"		v_texcoord = a_texcoord;\n"\
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"\
	"		v_color = a_color;\n"\
	"		v_color.xyz *= a_texcoord.z;\n"\
	"		v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"\
	"		v_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"\
	"		v_page_clut.z = fract(a_position.w / 64.0);\n"\
	"		v_page_clut.w = floor(a_position.w / 64.0) / 512.0;\n"\
	"		v_page_clut.xy += c_UVFudge;\n"\
	"		v_page_clut.zw += c_UVFudge;\n"\
	GTE_PERSPECTIVE_CORRECTION\
	/* v_is3d gates dither + bilinear so 2D logos/UI render sharp.
	 * The `a_zw.y > 100` test only distinguishes 3D from 2D when the
	 * runtime PGXP master gate is on (then a_zw.y is the screen
	 * height ~240 for 3D content, 0 for 2D). With PGXP off at
	 * runtime, ApplyVertexPGXP zeroes a_zw for everything → without
	 * the u_pgxpEnabled override every prim would read v_is3d=0 and
	 * we'd lose dither / bilinear on real 3D geometry too (visibly
	 * blocky tree leaves, etc.). When pgxp off, fall back to legacy
	 * "always treat as 3D" behavior — matches legacy behavior. */	"		v_is3d = (u_pgxpEnabled > 0) ? ((a_pgxp.z > 0.0) ? 1.0 : 0.0) : 1.0;\n"\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"		v_fogAmount = clamp(a_extra.z / 127.0, 0.0, 1.0);\n"\
	"		v_viewpos = a_viewpos;\n"\
	"	}\n"

#define GPU_FRAGMENT_SAMPLE_SHADER(bit) \
	GPU_PACK_RG_FUNC\
	GPU_DECODE_RG_FUNC\
	GPU_FETCH_VRAM_FUNC\
	"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
	GPU_ARRAY_FUNC\
	GPU_SAMPLE_TEXTURE_## bit ##BIT_FUNC\
	GPU_BILINEAR_SAMPLE_FUNC\
	GPU_NEAREST_SAMPLE_FUNC\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec3 u_fogColor;\n"\
	"	uniform int u_fogToBlack;\n"\
	"	uniform float u_fogStrength;\n"\
	"	uniform int u_flashlightOn;\n"\
	"	uniform vec3 u_flLightPos;\n"\
	"	uniform vec3 u_flDir;\n"\
	"	uniform vec3 u_flColor;\n"\
	"	uniform float u_flInnerCos;\n"\
	"	uniform float u_flOuterCos;\n"\
	"	uniform float u_flRange;\n"\
	"	void main() {\n"\
	"		if(bilinearFilter > 0 && v_is3d > 0.5)\n"\
	"			fragColor = BilinearTextureSample(v_texcoord.xy);\n"\
	"		else\n"\
	"			fragColor = NearestTextureSample(v_texcoord.xy);\n"\
	"		vec3 flAlbedo = fragColor.rgb;\n"\
	"		fragColor *= v_color;\n"\
	/* Per-pixel flashlight: spotlight cone * per-fragment Lambert (N.L). GrVertex carries no usable normals, so the surface normal is reconstructed from the view-space position gradient (cross(dFdx,dFdy)) -- v_viewpos is the same proven view-space pos the cone already uses, so this needs no GTE-side normal capture and is exact per triangle face. Derivatives are taken inside the UNIFORM u_flashlightOn branch (never the per-fragment z test) so they stay well-defined. The flashlight term modulates the texture albedo (flAlbedo) and adds to the dimmed base, so lit surfaces keep their texture and N.L shading instead of washing to flat white. */\
	"		if (u_flashlightOn > 0) {\n"\
	"			vec3 flP = v_viewpos;\n"\
	"			vec3 flN = cross(dFdx(flP), dFdy(flP));\n"\
	"			if (flP.z > 0.0) {\n"\
	"				fragColor.rgb *= 0.15; // per-vertex lighting -> dark base so the per-pixel cone is the only flashlight\n"\
	"				vec3 L = u_flLightPos - flP;\n"\
	"				float d = length(L);\n"\
	"				L /= max(d, 0.0001);\n"\
	"				float nlen = length(flN);\n"\
	"				vec3 N = (nlen > 1e-9) ? flN / nlen : vec3(0.0, 0.0, -1.0);\n"\
	"				if (dot(N, flP) > 0.0) N = -N;\n"\
	"				float ndl = 0.15 + 0.85 * max(dot(N, L), 0.0);\n"\
	"				float cone  = smoothstep(u_flOuterCos, u_flInnerCos, dot(-L, normalize(u_flDir)));\n"\
	"				float atten = clamp(1.0 - d / u_flRange, 0.0, 1.0);\n"\
	"				vec3 fl = u_flColor * (cone * atten * ndl);\n"\
	"				fragColor.rgb += flAlbedo * fl;\n"\
	"			}\n"\
	"		}\n"\
	"		float fogAmt = clamp(v_fogAmount * u_fogStrength, 0.0, 1.0);\n"\
	"		if (u_fogToBlack > 0)\n"\
	"			fragColor.rgb *= (1.0 - fogAmt);\n"\
	"		else\n"\
	"			fragColor.rgb = mix(fragColor.rgb, u_fogColor, fogAmt);\n"\
	GPU_DITHERING_NO_VCOLOR\
	"	}\n"

const char* gte_shader_4 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(4)
	"#endif\n";

const char* gte_shader_8 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(8)
	"#endif\n";

const char* gte_shader_16 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(16)
	"#endif\n";

const char* gte_shader_32_rgba =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"varying vec3 v_viewpos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	"	uniform sampler2D s_texture;\n"\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec2 texelSize;\n"\
	"	void main() {\n"\
	"		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"\
	"		fragColor = texture2D(s_texture, tc);\n"\
	GPU_DITHERING\
	"	}\n"
	"#endif\n";

int GR_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024];
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
		return 1;
	
	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024];
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
		return 1;

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

ShaderID GR_Shader_Compile(const char* source)
{
#if defined(ES2_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n";

	const char* GLSL_HEADER_FRAG =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define fragColor gl_FragColor\n";
#elif defined(ES3_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#else
	const char* GLSL_HEADER_VERT =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#endif

	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;

	if (g_cfg_bilinearFiltering)
	{
		strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
	}

	/* Affine (non-perspective-correct) texture mapping — matches PSX GPU behaviour.
	 * Uses noperspective interpolation qualifier (GLSL 1.30+, desktop only). */
	if (g_cfg_affineTextures)
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING noperspective varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING noperspective varying\n");
	}
	else
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING varying\n");
	}

	const char* vs_list[] = { GLSL_HEADER_VERT, extra_vs_defines, source };
	const char* fs_list[] = { GLSL_HEADER_FRAG, extra_fs_defines, source };

	GLuint program = glCreateProgram();

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vertexShader, 3, vs_list, NULL);
		glCompileShader(vertexShader);

		if( GR_Shader_CheckShaderStatus(vertexShader) == 0 )
			eprinterr("Failed to compile Vertex Shader!\n");
	
		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fragmentShader, 3, fs_list, NULL);
		glCompileShader(fragmentShader);

		if(GR_Shader_CheckShaderStatus(fragmentShader) == 0)
			eprinterr("Failed to compile Fragment Shader!\n");
	
		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_zw, "a_zw");
	glBindAttribLocation(program, a_pgxp, "a_pgxp");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");
	glBindAttribLocation(program, a_extra, "a_extra");
	glBindAttribLocation(program, a_normal, "a_normal");
	glBindAttribLocation(program, a_viewpos, "a_viewpos");

	glLinkProgram(program);
	if(GR_Shader_CheckProgramStatus(program) == 0)
		eprinterr("Failed to link Shader!\n");

	GLint sampler = 0;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures()
{
	unsigned int pixelData = 0xFFFFFFFF;

#if USE_OPENGL
	glGenTextures(1, &g_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}
#endif
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/)
{
	TextureID newTexture;
	glGenTextures(1, &newTexture);

	glBindTexture(GL_TEXTURE_2D, newTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	
	// another WebGL stuff. Texture will be black without clamp to edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);

	return newTexture;
}

void GR_CompilePSXShader(GTEShader* sh, const char* source)
{
	sh->shader = GR_Shader_Compile(source);

#if USE_OPENGL
	
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->ditherForceLoc = glGetUniformLocation(sh->shader, "u_ditherForce");
	sh->pixelScaleLoc = glGetUniformLocation(sh->shader, "u_pixelScale");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->fogColorLoc = glGetUniformLocation(sh->shader, "u_fogColor");
	sh->fogToBlackLoc = glGetUniformLocation(sh->shader, "u_fogToBlack");
	sh->fogStrengthLoc = glGetUniformLocation(sh->shader, "u_fogStrength");
	sh->pgxpEnabledLoc = glGetUniformLocation(sh->shader, "u_pgxpEnabled");
	sh->szMaxLoc = glGetUniformLocation(sh->shader, "u_szMax");
	sh->flashlightOnLoc = glGetUniformLocation(sh->shader, "u_flashlightOn");
	sh->flLightPosLoc = glGetUniformLocation(sh->shader, "u_flLightPos");
	sh->flDirLoc = glGetUniformLocation(sh->shader, "u_flDir");
	sh->flColorLoc = glGetUniformLocation(sh->shader, "u_flColor");
	sh->flInnerCosLoc = glGetUniformLocation(sh->shader, "u_flInnerCos");
	sh->flOuterCosLoc = glGetUniformLocation(sh->shader, "u_flOuterCos");
	sh->flRangeLoc = glGetUniformLocation(sh->shader, "u_flRange");
#endif
}

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gte_shader_4, gte_shader_4);
	GR_CompilePSXShader(&g_gte_shader_8, gte_shader_8);
	GR_CompilePSXShader(&g_gte_shader_16, gte_shader_16);
	GR_CompilePSXShader(&g_gte_shader_32_rgba, gte_shader_32_rgba);
}

int GR_InitialisePSX()
{
	SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
	GR_GenerateCommonTextures();
	GR_InitialisePSXShaders();

#if USE_OPENGL
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

	/* PC port: enable MSAA rasterisation when a multisample framebuffer was
	 * obtained. Core profiles default this on, but enable explicitly + report
	 * the sample count the driver actually granted (may differ from requested). */
	if (g_cfg_msaaSamples > 0)
	{
#ifndef RENDERER_OGLES
		glEnable(GL_MULTISAMPLE);
#endif
		int actualSamples = 0;
		SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &actualSamples);
		eprintf("*MSAA: requested %dx, got %dx\n", g_cfg_msaaSamples, actualSamples);
		if (actualSamples <= 1)
			g_cfg_msaaSamples = 0; /* driver gave us a single-sample buffer after all */
	}

	// gen framebuffer
	{
		memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
		PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// make a special texture
		// it will be resized later
		glGenTextures(1, &g_fbTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_fbTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glBlitFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen offscreen RT
	{
		memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
		PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// offscreen texture render target
		glGenTextures(1, &g_offscreenRTTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glOffscreenFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_offscreenRTTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen VRAM textures.
	// double-buffered
	{
		int i;

		glGenTextures(2, g_vramTexturesDouble);

		for(i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// set storage size
			glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
		}

		g_vramTexture = g_vramTexturesDouble[0];

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &g_glVRAMFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(g_glVertexArray[i]);

			glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
		}

		glBindVertexArray(0);
	}
#else
#error
#endif

	GR_InitPostProcess();

	GR_ResetDevice();

	return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

#if USE_OPENGL 
	// -1..1
	float z = (znear + zfar) / (znear - zfar);
#endif

	float ortho[16] = {
		a, 0, 0, 0,
		0, b, 0, 0,
		0, 0, c, 0,
		x, y, z, 1
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	float sinF, cosF;
	sinF = sinf(0.5f * fov);
	cosF = cosf(0.5f * fov);

	float h = cosF / sinF;
	float w = (h * height) / width;

	float persp[16] = {
		w, 0, 0, 0,
		0, h, 0, 0,
		0, 0, (zFar + zNear) / (zFar - zNear), 1,
		0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(	rect->x - activeDispEnv.disp.x > 0 ||
			rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 ||
			rect->h < activeDispEnv.disp.h - 1));

	GR_SetScissorState(scissorOn);

	if (!scissorOn)
		return;

	// Runtime gate: same as the ortho selection — non-PGXP path uses
	// emuScreenAspect=1 (no widescreen aspect remap) so the scissor
	// rectangle stays in pixel-space coordinates that match the legacy
	// 2D ortho. With PGXP at compile time but off at runtime, falling
	// through to the PGXP aspect calc would shrink the scissor and
	// cause clipping inconsistent with the prim ortho.
	const float emuScreenAspect = 1.0f;

	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	// first map to 0..1
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	// then map to screen
	{
		clipRectX -= 0.5f;

		clipRectX *= emuScreenAspect;
		clipRectW *= emuScreenAspect;

		clipRectX += 0.5f;
	}

#if USE_OPENGL
	// adjust scissor rectangle by the backbuffer size (window dimensions)
	const float flipOffset = g_windowHeight - clipRectH * (float)g_windowHeight;
	const float crx = clipRectX * (float)g_windowWidth;
	const float cry = clipRectY * (float)g_windowHeight;
	const float crw = clipRectW * (float)g_windowWidth;
	const float crh = clipRectH * (float)g_windowHeight;

	glScissor(crx, flipOffset - cry, crw, crh);
#endif
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{

	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = activeDispEnv.disp.w;
	rect->h = activeDispEnv.disp.h;
}

void GR_SetShader(const ShaderID shader)
{
	if (g_PreviousShader != shader)
	{
#if USE_OPENGL
		glUseProgram(shader);
#else
#error
#endif

		g_PreviousShader = shader;
	}
}


void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(g_gte_shader_4.shader);
		u_bilinearFilterLoc = g_gte_shader_4.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_4.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_4.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_4.projectionLoc;
		u_projection3DLoc = g_gte_shader_4.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_4.fogColorLoc;
		u_fogToBlackLoc = g_gte_shader_4.fogToBlackLoc;
		u_fogStrengthLoc = g_gte_shader_4.fogStrengthLoc;
		u_pgxpEnabledLoc = g_gte_shader_4.pgxpEnabledLoc;
		u_szMaxLoc = g_gte_shader_4.szMaxLoc;
		u_flashlightOnLoc = g_gte_shader_4.flashlightOnLoc;
		u_flLightPosLoc = g_gte_shader_4.flLightPosLoc;
		u_flDirLoc = g_gte_shader_4.flDirLoc;
		u_flColorLoc = g_gte_shader_4.flColorLoc;
		u_flInnerCosLoc = g_gte_shader_4.flInnerCosLoc;
		u_flOuterCosLoc = g_gte_shader_4.flOuterCosLoc;
		u_flRangeLoc = g_gte_shader_4.flRangeLoc;
		break;
	case TF_8_BIT:
		GR_SetShader(g_gte_shader_8.shader);
		u_bilinearFilterLoc = g_gte_shader_8.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_8.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_8.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_8.projectionLoc;
		u_projection3DLoc = g_gte_shader_8.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_8.fogColorLoc;
		u_fogToBlackLoc = g_gte_shader_8.fogToBlackLoc;
		u_fogStrengthLoc = g_gte_shader_8.fogStrengthLoc;
		u_pgxpEnabledLoc = g_gte_shader_8.pgxpEnabledLoc;
		u_szMaxLoc = g_gte_shader_8.szMaxLoc;
		u_flashlightOnLoc = g_gte_shader_8.flashlightOnLoc;
		u_flLightPosLoc = g_gte_shader_8.flLightPosLoc;
		u_flDirLoc = g_gte_shader_8.flDirLoc;
		u_flColorLoc = g_gte_shader_8.flColorLoc;
		u_flInnerCosLoc = g_gte_shader_8.flInnerCosLoc;
		u_flOuterCosLoc = g_gte_shader_8.flOuterCosLoc;
		u_flRangeLoc = g_gte_shader_8.flRangeLoc;
		break;
	case TF_16_BIT:
		GR_SetShader(g_gte_shader_16.shader);
		u_bilinearFilterLoc = g_gte_shader_16.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_16.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_16.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_16.projectionLoc;
		u_projection3DLoc = g_gte_shader_16.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_16.fogColorLoc;
		u_fogToBlackLoc = g_gte_shader_16.fogToBlackLoc;
		u_fogStrengthLoc = g_gte_shader_16.fogStrengthLoc;
		u_pgxpEnabledLoc = g_gte_shader_16.pgxpEnabledLoc;
		u_szMaxLoc = g_gte_shader_16.szMaxLoc;
		u_flashlightOnLoc = g_gte_shader_16.flashlightOnLoc;
		u_flLightPosLoc = g_gte_shader_16.flLightPosLoc;
		u_flDirLoc = g_gte_shader_16.flDirLoc;
		u_flColorLoc = g_gte_shader_16.flColorLoc;
		u_flInnerCosLoc = g_gte_shader_16.flInnerCosLoc;
		u_flOuterCosLoc = g_gte_shader_16.flOuterCosLoc;
		u_flRangeLoc = g_gte_shader_16.flRangeLoc;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(g_gte_shader_32_rgba.shader);
		u_bilinearFilterLoc = -1;
		u_ditherForceLoc = -1;
		u_pixelScaleLoc = -1;
		u_projectionLoc = g_gte_shader_32_rgba.projectionLoc;
		u_projection3DLoc = g_gte_shader_32_rgba.projection3DLoc;
		u_texelSizeLoc = g_gte_shader_32_rgba.texelSizeLoc;
		u_fogColorLoc = g_gte_shader_32_rgba.fogColorLoc;
		u_fogToBlackLoc = g_gte_shader_32_rgba.fogToBlackLoc;
		u_fogStrengthLoc = g_gte_shader_32_rgba.fogStrengthLoc;
		u_pgxpEnabledLoc = g_gte_shader_32_rgba.pgxpEnabledLoc;
		u_szMaxLoc = g_gte_shader_32_rgba.szMaxLoc;
		u_flashlightOnLoc = g_gte_shader_32_rgba.flashlightOnLoc;
		u_flLightPosLoc = g_gte_shader_32_rgba.flLightPosLoc;
		u_flDirLoc = g_gte_shader_32_rgba.flDirLoc;
		u_flColorLoc = g_gte_shader_32_rgba.flColorLoc;
		u_flInnerCosLoc = g_gte_shader_32_rgba.flInnerCosLoc;
		u_flOuterCosLoc = g_gte_shader_32_rgba.flOuterCosLoc;
		u_flRangeLoc = g_gte_shader_32_rgba.flRangeLoc;
		break;
	}

	/* Push u_pgxpEnabled every shader bind so vertex shader's v_is3d
	 * fallback ((u_pgxpEnabled > 0) ? a_zw.y test : 1.0) correctly
	 * drops the 3D-only gate when PGXP is off at runtime. Without
	 * this fallback, every prim got a_zw.y=0 → v_is3d=0 → no dither
	 * and forced-nearest sampling on actual 3D geometry (visible as
	 * blocky tree leaves). */
	if (u_pgxpEnabledLoc != -1)
		glUniform1i(u_pgxpEnabledLoc, g_PsxUsePgxp);

	/* PGXP depth normalize: prev-frame max SZ. Lets the vertex shader turn each
	 * vertex's unquantized SZ3 into continuous NDC depth (Z-fight fix). */
	if (u_szMaxLoc != -1)
		glUniform1f(u_szMaxLoc, PGXP_GetSzMax());

	if (u_fogColorLoc != -1)
		glUniform3fv(u_fogColorLoc, 1, g_PsyX_FogColor);

	if (u_fogToBlackLoc != -1)
		glUniform1i(u_fogToBlackLoc, g_PsxFogToBlack);

	if (u_fogStrengthLoc != -1)
		glUniform1f(u_fogStrengthLoc, g_PsyX_FogStrength);

	/* Per-pixel flashlight cone. u_flashlightOn is 0 unless BOTH the master
	 * config flag and the per-frame game push are set, so the OFF path never
	 * touches the spotlight branch (and the vsz>0 gate also keeps it off). */
	if (u_flashlightOnLoc != -1)
		glUniform1i(u_flashlightOnLoc,
		            (g_PsyX_UsePerPixelFlashlight && g_PsyX_FlashlightActive) ? 1 : 0);
	if (u_flLightPosLoc != -1)
		glUniform3fv(u_flLightPosLoc, 1, g_PsyX_FlashlightPos);
	if (u_flDirLoc != -1)
		glUniform3fv(u_flDirLoc, 1, g_PsyX_FlashlightDir);
	if (u_flColorLoc != -1) {
		float flCol[3];
		flCol[0] = g_PsyX_FlashlightColor[0] * g_PsyX_FlashlightIntensity;
		flCol[1] = g_PsyX_FlashlightColor[1] * g_PsyX_FlashlightIntensity;
		flCol[2] = g_PsyX_FlashlightColor[2] * g_PsyX_FlashlightIntensity;
		glUniform3fv(u_flColorLoc, 1, flCol);
	}
	if (u_flInnerCosLoc != -1)
		glUniform1f(u_flInnerCosLoc, g_PsyX_FlashlightInnerCos);
	if (u_flOuterCosLoc != -1)
		glUniform1f(u_flOuterCosLoc, g_PsyX_FlashlightOuterCos);
	if (u_flRangeLoc != -1)
		glUniform1f(u_flRangeLoc, g_PsyX_FlashlightRange);

	/* Push the dither-force uniform every shader bind. Cheap (single
	 * float upload) and ensures runtime config changes (if we add a
	 * hotkey toggle later) take effect on the next primitive.
	 * g_PsxDitherSuppressed lets the game disable dither per-frame on
	 * 2D-only states (menus, logos, inventory) without changing the
	 * config flag. */
	if (u_ditherForceLoc != -1)
		glUniform1f(u_ditherForceLoc,
		            (g_cfg_psxDither && !g_PsxDitherSuppressed) ? 1.0f : 0.0f);

	/* Pixel scale = window width / PSX native (320). Scales the dither
	 * cell so each PSX-pixel-equivalent on screen gets its own matrix
	 * lookup, keeping the pattern visually proportional to PSX
	 * regardless of resolution. */
	if (u_pixelScaleLoc != -1) {
		float pixelScale = (g_windowWidth > 0)
			? ((float)g_windowWidth / 320.0f)
			: 1.0f;
		glUniform1f(u_pixelScaleLoc, pixelScale);
	}

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glBindTexture(GL_TEXTURE_2D, texture);
	if(u_bilinearFilterLoc != -1)
		glUniform1i(u_bilinearFilterLoc, g_cfg_bilinearFiltering && !g_PsxDitherSuppressed);

#endif

	g_lastBoundTexture = texture;
}

void GR_SetOverrideTextureSize(int width, int height)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version
	float vec[] = { 1.0f / (float)width, 1.0f / (float)height };
	glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture == -1)
		return;

#if USE_OPENGL
	glDeleteTextures(1, &texture);
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;

	u_short* dst = vram + x + y * VRAM_WIDTH;

	if (x + w > VRAM_WIDTH)
		w = VRAM_WIDTH - x;

	if (y + h > VRAM_HEIGHT)
		h = VRAM_HEIGHT - y;

	// clear VRAM region with given color
	for (int i = 0; i < h; i++)
	{
		u_short* tmp = dst;

		for (int j = 0; j < w; j++)
			*tmp++ = r | (g << 5) | (b << 11);

		dst += VRAM_WIDTH;
	}
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;

#if USE_OPENGL
	/* PC port: when pillarboxing (4:3 content centered in a wider window), keep
	 * the side bars black even when the game clears the framebuffer to a
	 * non-black color. The item-examine ("story item") screen clears to the gray
	 * fog color, which otherwise tints the bars. Clear the whole window black,
	 * then clear only the 4:3 region to the requested color via a scissor.
	 * Skipped when the clear is already black (menus) — those bars are fine. */
	const bool wantPillarbox =
		(g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
		(!g_PcHorPlusEnabled && g_PcMenuPillarbox);
	if (wantPillarbox && g_windowWidth > 0 && g_windowHeight > 0 && (r | g | b) != 0)
	{
		const float psxAspect = 4.0f / 3.0f;
		const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
		if (winAspect > psxAspect)
		{
			const int vpW = (int)(g_windowHeight * psxAspect + 0.5f);
			const int vpX = (g_windowWidth - vpW) / 2;

			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			glEnable(GL_SCISSOR_TEST);
			glScissor(vpX, 0, vpW, g_windowHeight);
			glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			return;
		}
	}

	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

void GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

	FILE* fp = fopen(outputFileName, "wb");
	if (fp == NULL)
		return;

	unsigned char TGAheader[12] = { 0,0,2,0,0,0,0,0,0,0,0,0 };
	unsigned char header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(unsigned char), 12, fp);
	fwrite(header, sizeof(unsigned char), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int* src, int x, int y, int w, int h, int update_vram, int flip_y)
{
	assert(x >= 0);
	assert(y >= 0);
	assert(x + w <= VRAM_WIDTH);
	assert(y + h <= VRAM_WIDTH);

	ushort* fb = (ushort*)malloc(w * h * sizeof(ushort));
	uint* data_src = (uint*)src;
	ushort* data_dst = (ushort*)fb;

	for (int i = 0; i < h; i++)
	{
		for (int j = 0; j < w; j++)
		{
			uint c = *data_src++;

			u_char b = ((c >> 3) & 0x1F);
			u_char g = ((c >> 11) & 0x1F);
			u_char r = ((c >> 19) & 0x1F);
			//u_char a = ((c >> 24) & 0x1F);

			int a = r == g == b == 0 ? 0 : 1;

			*data_dst++ = r | (g << 5) | (b << 10) | (a << 15);
		}
	}

	ushort* ptr = (ushort*)vram + VRAM_WIDTH * y + x;

	for (int fy = 0; fy < h; fy++)
	{
		int py = flip_y ? (h - fy - 1) : fy;
		ushort* fb_ptr = fb + (h * py / h) * w;

		for (int fx = 0; fx < w; fx++)
			ptr[fx] = fb_ptr[w * fx / w];

		ptr += VRAM_WIDTH;
	}

	free(fb);

	if (update_vram)
		vram_need_update = 1;
}

void GR_ReadFramebufferDataToVRAM()
{
	int x, y, w, h;
	if (!framebuffer_need_update)
		return;

	framebuffer_need_update = 0;

	/* PC port: skip readback while a paper-map / TIM-protect screen is active.
	 * The readback writes the framebuffer back into vram[] at (disp.x, disp.y)
	 * which can clobber CLUT/texture rows that live inside the display rect
	 * (paper-map CLUT lives at VRAM (224,15) — inside (0,0)-(320,240)). */
	if (g_PsxSkipFramebufferStore)
		return;

	x = g_PreviousFramebuffer.x;
	y = g_PreviousFramebuffer.y;
	w = g_PreviousFramebuffer.w;
	h = g_PreviousFramebuffer.h;

	// now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
	// read the texture
	if(g_glFramebufferPBO.pixels)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		PBO_Download(&g_glFramebufferPBO);
		glBindTexture(GL_TEXTURE_2D, 0);
		GR_CopyRGBAFramebufferToVRAM((u_int*)g_glFramebufferPBO.pixels, x, y, w, h, 0, 0);
	}
#endif
}

void GR_SetScissorState(int enable)
{
	if (g_PreviousScissorState == enable)
		return;

#if USE_OPENGL
	if (g_PreviousScissorState)
		glDisable(GL_SCISSOR_TEST);
	else
		glEnable(GL_SCISSOR_TEST);
#endif
	g_PreviousScissorState = enable;
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable)
	{
		// setup render target viewport
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
	}
	else
	{
		// setup default viewport

		{
			// Widescreen presentation. Three modes (g_PcWidescreenMode):
			//   0 = Pillarbox (PSX-faithful, default): 4:3 ortho rendered into
			//       a centered 4:3 viewport of the window. Black bars on
			//       sides. Characters and framing match PSX CRT exactly.
			//   1 = Hor+: widen ortho horizontally so GTE-projected geometry
			//       beyond the PSX framebuffer is visible. Reveals scene
			//       content cropped on PSX (extra bar counter, walls etc.).
			//   2 = Stretch (anamorphic): 4:3 content fills 16:9, chars wider.
			//
			// g_PcHorPlusEnabled=0 (2D UI screens) always uses the unwidened
			// 4:3 ortho with full-window viewport — UI was authored stretched.
			//
			// The actual viewport for pillarbox is set in the !enable branch
			// below (search "Pillarbox viewport"); ortho here is matched to
			// whatever viewport that branch picks.
			const float psxW = (float)activeDispEnv.disp.w;  // 320
			float psxH = (float)activeDispEnv.disp.h;   // 224 — aspect/horizontal only
			float orthoTop = 0.0f, orthoBot = psxH;
			if (g_PcHorPlusEnabled) {
				/* 3D gameplay world renders ~14.7% too much vertical world (measured: vertical
				 * scale 0.872 vs DuckStation at a fixed 4:3 spot, horizontal 1.0, top-aligned
				 * with the extra at the BOTTOM = near foreground). Crop the world ortho to
				 * g_PsxWorldVScale of the buffer, top-anchored (keep ceiling, clip foreground),
				 * which also zooms objects ~1/scale taller. psxH (aspect) stays full so the
				 * horizontal + Hor+ logic is unchanged. Console `vfov` tunes it. */
				/* The world (OT0) is ALWAYS cropped — gameplay and cutscenes alike — so
				 * 3D framing matches PSX/DuckStation (the old cutscene vscale-skip un-cropped
				 * the whole frame and read as stretched). The 2D UI pass (g_PsxUIOrthoPass:
				 * OT2 — subtitles, fade, cutscene letterbox bars) instead gets full vertical
				 * ortho so it isn't scaled/clipped off the bottom. vshift is a gameplay
				 * framing aid only (skipped for cutscenes and for the UI pass). */
				const float vscale = g_PsxUIOrthoPass ? 1.0f : g_PsxWorldVScale;
				const float vshift = (g_PsxFixedCamActive && !g_PsxCutsceneActive && !g_PsxUIOrthoPass) ? g_PsxWorldVShift : 0.0f;
				orthoTop = 0.0f          - vshift;   // +shift = show higher content
				orthoBot = psxH * vscale - vshift;
			}
			const float psxAspect = psxW / psxH;
			const float winAspect = (g_windowHeight > 0)
				? ((float)g_windowWidth / (float)g_windowHeight)
				: psxAspect;
			const float horScale = winAspect / psxAspect;
			if (!g_PcHorPlusEnabled || horScale <= 1.0f) {
				/* 2D UI or non-widescreen window: 4:3 ortho, full viewport. */
				GR_Ortho2D(0.0f, psxW, orthoBot, orthoTop, -1.0f, 1.0f);
			} else if (g_PcWidescreenMode == 1) {
				/* Hor+ widescreen: widen ortho, full-window viewport. PSX_NTSC_PIXEL_ASPECT
				 * preserves 1 H px = 1 V px scaling for character proportions. */
				const float effectiveScale = horScale * PSX_NTSC_PIXEL_ASPECT;
				const float margin = psxW * (effectiveScale - 1.0f) * 0.5f;
				/* hfov: scale the horizontal world extent around center (g_PsxWorldHScale).
				 * 1.0 = identity (-margin..psxW+margin); >1 shrinks the ortho width = wider
				 * models. Skipped for the UI pass so 2D UI stays aligned. */
				const float hscale = g_PsxUIOrthoPass ? 1.0f : g_PsxWorldHScale;
				const float cx     = psxW * 0.5f;
				const float halfW  = (psxW * 0.5f + margin) / hscale;
				GR_Ortho2D(cx - halfW, cx + halfW, orthoBot, orthoTop, -1.0f, 1.0f);
			} else {
				/* Pillarbox (mode 0, default) or stretch (mode 2): 4:3 ortho.
				 * The viewport (below) handles pillarbox vs full-window. */
				GR_Ortho2D(0.0f, psxW, orthoBot, orthoTop, -1.0f, 1.0f);
			}

			/* [ASPECT] ground-truth dump of the ACTUAL runtime projection
			 * inputs so the on-screen aspect/FOV can be computed from real
			 * values instead of assumptions. Goes to g_logStream (=SilentHill.log
			 * when debug logging is on) so it lands in the captured log, not
			 * stderr. Logs once per distinct (disp,win,HorPlus,WS) state so the
			 * 2D menu and the 3D gameplay framing are both captured. C2_H is the
			 * GTE geom-screen distance (sets horizontal FOV); C2_OFX/OFY are the
			 * projection center (>>16 = pixels). */
			{
				extern FILE* g_logStream;
				FILE* aspOut = g_logStream ? g_logStream : stderr;
				static unsigned s_aspSeen[8];
				static int s_aspSeenN = 0;
				/* viewport that the block below will pick (recomputed here). */
				int dbgVpW = g_windowWidth, dbgVpX = 0;
				const bool dbgPillar =
					(g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
					(!g_PcHorPlusEnabled && g_PcMenuPillarbox);
				if (dbgPillar && g_windowHeight > 0 && winAspect > psxAspect) {
					dbgVpW = (int)(g_windowHeight * psxAspect + 0.5f);
					dbgVpX = (g_windowWidth - dbgVpW) / 2;
				}
				const float dbgMargin = (g_PcHorPlusEnabled && horScale > 1.0f && g_PcWidescreenMode == 1)
					? psxW * (horScale * PSX_NTSC_PIXEL_ASPECT - 1.0f) * 0.5f : 0.0f;
				unsigned key = ((unsigned)(activeDispEnv.disp.w & 0x3FF) << 22) ^
					((unsigned)(activeDispEnv.disp.h & 0x3FF) << 12) ^
					((unsigned)(g_windowWidth & 0xFFF)) ^
					((unsigned)(g_PcHorPlusEnabled ? 1u : 0u) << 31) ^
					((unsigned)(g_PcWidescreenMode & 3) << 29);
				int seen = 0;
				for (int i = 0; i < s_aspSeenN; i++) if (s_aspSeen[i] == key) { seen = 1; break; }
				if (!seen && s_aspSeenN < 8) {
					s_aspSeen[s_aspSeenN++] = key;
					fprintf(aspOut, "[ASPECT] disp=%dx%d win=%dx%d psxAspect=%.4f winAspect=%.4f horScale=%.4f HorPlus=%d WS=%d PAR=%.4f margin=%.1f vp=%d+%dx%d C2_H=%d OFX=%d OFY=%d\n",
						(int)activeDispEnv.disp.w, (int)activeDispEnv.disp.h,
						g_windowWidth, g_windowHeight, psxAspect, winAspect, horScale,
						g_PcHorPlusEnabled, g_PcWidescreenMode, (float)PSX_NTSC_PIXEL_ASPECT,
						dbgMargin, dbgVpX, dbgVpW, g_windowHeight,
						(int)C2_H, (int)(C2_OFX >> 16), (int)(C2_OFY >> 16));
					fflush(aspOut);
				}
			}
		}

		/* Display viewport — set EVERY call, not just on offscreen-state
		 * change. GR_BeginScene resets the viewport to the full window each
		 * frame, so a stable screen (e.g. a menu that never toggles the
		 * offscreen state) would otherwise lose its pillarbox after frame 1.
		 * Pillarbox the central 4:3 region (black bars) for 3D gameplay in
		 * mode 0, or for 2D screens (menus/load) when g_PcMenuPillarbox is on;
		 * otherwise full window (stretch). */
		{
			int vpX = 0, vpY = 0, vpW = g_windowWidth, vpH = g_windowHeight;
			const bool wantPillarbox =
				(g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
				(!g_PcHorPlusEnabled && g_PcMenuPillarbox);
			if (wantPillarbox && g_windowHeight > 0) {
				const float psxAspect = 4.0f / 3.0f;
				const float winAspect = (float)g_windowWidth / (float)g_windowHeight;
				if (winAspect > psxAspect) {
					vpW = (int)(g_windowHeight * psxAspect + 0.5f);
					vpX = (g_windowWidth - vpW) / 2;
				}
			}
			GR_SetViewPort(vpX, vpY, vpW, vpH);
		}

	}

	if (g_PreviousOffscreenState == enable)
		return;

	g_PreviousOffscreenState = enable;

#if USE_OPENGL
	if (enable)
	{
		// set storage size first
		if (g_PreviousOffscreen.w != offscreenRect->w &&
			g_PreviousOffscreen.h != offscreenRect->h)
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreenRect->w, offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		g_PreviousOffscreen = *offscreenRect;

		GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
		glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

		// clear it out
		glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		/* (Display viewport / pillarbox is set above, before the early-out,
		 * so it applies every call. Here we only do the VRAM writeback.) */

		/* PC port: skip the offscreen-RT → VRAM writeback when a TIM-protect
		 * screen is active. This branch fires whenever a draw split has
		 * dfe=0 (offscreen render target). Both the GL blit and the CPU
		 * GR_CopyRGBAFramebufferToVRAM below write to VRAM at
		 * (g_PreviousOffscreen.x, g_PreviousOffscreen.y, w, h) — for paper-map
		 * pickup screens that rect overlaps the CLUT at (224,15) and texture
		 * origin (320,16), tiling rendered framebuffer content over the
		 * just-uploaded TIM. The viewport / state-tracking above must still
		 * run so subsequent draws use the correct framebuffer binding;
		 * only the writes are suppressed. */
		if (g_PsxSkipFramebufferStore)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
		else
		{
#if USE_OFFSCREEN_BLIT
		// before drawing set source and target
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			// rebind texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

			// setup draw and read framebuffers
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);					// source is backbuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

			glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
								g_PreviousOffscreen.x, g_PreviousOffscreen.y + g_PreviousOffscreen.h, g_PreviousOffscreen.x + g_PreviousOffscreen.w, g_PreviousOffscreen.y,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);

			// done, unbind
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}
#endif

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		// copy rendering results to VRAM texture
		{
			// reat the texture
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			//glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			PBO_Download(&g_glOffscreenPBO);
			glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

			// Don't forcely update VRAM
			GR_CopyRGBAFramebufferToVRAM((u_int*)g_glOffscreenPBO.pixels,
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
				USE_OFFSCREEN_BLIT == 0, 1);
		}
		}

	}
#endif
}

/* ============================================================================
 * PC port: full-screen post-process pass + MSAA-safe fullscreen blit helper.
 *
 * One tiny shader program draws a single full-screen triangle (from gl_VertexID,
 * no vertex buffer) sampling a source texture, applying the look selected by
 * g_cfg_postProcess. The same helper is reused to "present" the freeze-frame
 * when MSAA is on (a single-sample -> multisample glBlitFramebuffer is illegal,
 * but a shader draw into the multisample default FBO is fine).
 * ========================================================================== */
#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)
#define PSYX_HAS_POSTPROCESS 1
#else
#define PSYX_HAS_POSTPROCESS 0
#endif

#if PSYX_HAS_POSTPROCESS

static ShaderID g_postShader = (ShaderID)-1;
static GLint    g_postLoc_mode = -1;
static GLint    g_postLoc_texSize = -1;
static GLint    g_postLoc_time = -1;
static GLint    g_postLoc_tonemap = -1;
static GLint    g_postLoc_postInt = -1;
static GLint    g_postLoc_tmInt = -1;
static GLuint   g_postVAO = 0;
static GLuint   g_postFBO = 0;
static TextureID g_postTex = (TextureID)-1;
static int      g_postW = 0;
static int      g_postH = 0;
static unsigned g_postFrame = 0;

static const char* s_postShaderSrc =
	"varying vec2 v_uv;\n"
	"#ifdef VERTEX\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	v_uv = (p + 1.0) * 0.5;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n"
	"#else\n"
	"uniform sampler2D s_texture;\n"
	"uniform int   u_postMode;\n"
	"uniform vec2  u_texSize;\n"  /* (1/width, 1/height) of the source */
	"uniform float u_time;\n"
	"uniform int   u_tonemap;\n"
	"uniform float u_postIntensity;\n"
	"uniform float u_tmIntensity;\n"
	"float hash(vec2 p) {\n"
	"	p = fract(p * vec2(123.34, 456.21));\n"
	"	p += dot(p, p + 45.32);\n"
	"	return fract(p.x * p.y);\n"
	"}\n"
	"vec3 colorGrade(vec3 c) {\n"
	"	c = (c - 0.5) * 1.12 + 0.5;\n"                       /* contrast */
	"	float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"	c = mix(vec3(l), c, 1.15);\n"                        /* saturation */
	"	c *= vec3(1.06, 1.0, 0.94);\n"                       /* warm tint */
	"	return c;\n"
	"}\n"
	"vec2 curve(vec2 uv) {\n"
	"	uv = uv * 2.0 - 1.0;\n"
	"	vec2 o = abs(uv.yx) / vec2(6.0, 5.0);\n"
	"	uv += uv * o * o;\n"
	"	return uv * 0.5 + 0.5;\n"
	"}\n"
	"vec3 tonemap(vec3 c) {\n"
	"	if (u_tonemap == 1) { return c / (c + vec3(1.0)); }\n"                          /* Reinhard */
	"	if (u_tonemap == 2) {\n"                                                          /* ACES (Narkowicz) */
	"		c *= 0.6;\n"
	"		return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14), 0.0, 1.0);\n"
	"	}\n"
	"	if (u_tonemap == 3) {\n"                                                          /* Filmic (Hejl/Burgess) */
	"		vec3 x = max(vec3(0.0), c - 0.004);\n"
	"		return (x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06);\n"
	"	}\n"
	"	return c;\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = v_uv;\n"
	"	vec3 col;\n"
	"	vec3 origCol = texture2D(s_texture, v_uv).rgb;\n"
	"	if (u_postMode == 1) {\n"                            /* CRT */
	"		uv = curve(uv);\n"
	"		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		col *= 0.75 + 0.25 * abs(sin(uv.y * 240.0 * 3.14159));\n"
	"		int m = int(mod(gl_FragCoord.x, 3.0));\n"
	"		vec3 mask = m == 0 ? vec3(1.0, 0.72, 0.72) : (m == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0));\n"
	"		col *= mask * 1.25;\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 1.1, 0.0, 1.0);\n"
	"	} else if (u_postMode == 2) {\n"                     /* Scanlines */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		col *= 0.7 + 0.3 * abs(sin(uv.y * 240.0 * 3.14159));\n"
	"	} else if (u_postMode == 3) {\n"                     /* Vignette */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 1.3, 0.0, 1.0);\n"
	"	} else if (u_postMode == 4) {\n"                     /* Color grade */
	"		col = colorGrade(texture2D(s_texture, uv).rgb);\n"
	"	} else if (u_postMode == 5) {\n"                     /* Film grain */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"		float n = hash(floor(uv / u_texSize) + u_time);\n"
	"		col += (n - 0.5) * 0.10;\n"
	"	} else if (u_postMode == 6) {\n"                     /* Sharpen */
	"		vec3 c = texture2D(s_texture, uv).rgb;\n"
	"		vec3 b = (texture2D(s_texture, uv + vec2(u_texSize.x, 0.0)).rgb\n"
	"		        + texture2D(s_texture, uv - vec2(u_texSize.x, 0.0)).rgb\n"
	"		        + texture2D(s_texture, uv + vec2(0.0, u_texSize.y)).rgb\n"
	"		        + texture2D(s_texture, uv - vec2(0.0, u_texSize.y)).rgb) * 0.25;\n"
	"		col = c + (c - b) * 0.85;\n"
	"	} else if (u_postMode == 7) {\n"                     /* PSX retro: downsample + dither + 5-bit */
	"		vec2 grid = vec2(320.0, 240.0);\n"
	"		vec2 quv = (floor(uv * grid) + 0.5) / grid;\n"
	"		col = texture2D(s_texture, quv).rgb;\n"
	"		mat4 dith = mat4(-4.0, 0.0, -3.0, 1.0, 2.0, -2.0, 3.0, -1.0, -3.0, 1.0, -4.0, 0.0, 3.0, -1.0, 2.0, -2.0) / 255.0;\n"
	"		ivec2 dc = ivec2(mod(gl_FragCoord.xy, 4.0));\n"
	"		col += vec3(dith[dc.x][dc.y]);\n"
	"		col = floor(col * 32.0 + 0.5) / 32.0;\n"
	"	} else if (u_postMode == 8) {\n"                     /* Cinematic: grade + vignette + grain */
	"		col = colorGrade(texture2D(s_texture, uv).rgb);\n"
	"		vec2 d = uv - 0.5; col *= clamp(1.0 - dot(d, d) * 0.9, 0.0, 1.0);\n"
	"		float n = hash(floor(uv / u_texSize) + u_time);\n"
	"		col += (n - 0.5) * 0.045;\n"
	"	} else {\n"                                          /* passthrough */
	"		col = texture2D(s_texture, uv).rgb;\n"
	"	}\n"
	"	col = mix(origCol, col, u_postIntensity);\n"
	"	col = mix(col, tonemap(col), u_tmIntensity);\n"
	"	fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
	"}\n"
	"#endif\n";

void GR_InitPostProcess(void)
{
	if (g_postShader != (ShaderID)-1)
		return;

	g_postShader = GR_Shader_Compile(s_postShaderSrc);
	g_postLoc_mode    = glGetUniformLocation(g_postShader, "u_postMode");
	g_postLoc_texSize = glGetUniformLocation(g_postShader, "u_texSize");
	g_postLoc_time    = glGetUniformLocation(g_postShader, "u_time");
	g_postLoc_tonemap = glGetUniformLocation(g_postShader, "u_tonemap");
	g_postLoc_postInt = glGetUniformLocation(g_postShader, "u_postIntensity");
	g_postLoc_tmInt   = glGetUniformLocation(g_postShader, "u_tmIntensity");

	glGenVertexArrays(1, &g_postVAO);
}

static void GR_EnsurePostTarget(int w, int h)
{
	if (g_postTex != (TextureID)-1 && g_postW == w && g_postH == h)
		return;

	if (g_postTex == (TextureID)-1)
	{
		glGenTextures(1, &g_postTex);
		glGenFramebuffers(1, &g_postFBO);
	}

	g_postW = w;
	g_postH = h;

	glBindTexture(GL_TEXTURE_2D, g_postTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, g_postFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_postTex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Draw a full-screen triangle sampling `tex` into the currently bound default
 * framebuffer, applying post mode `mode` (0 = straight copy). Disables depth /
 * blend / scissor / stencil for the draw, then invalidates the renderer's
 * cached GL state so the next frame's prims re-establish it. */
static void GR_DrawFullscreenTexture(TextureID tex, int mode)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, g_windowWidth, g_windowHeight);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(g_postShader);
	if (g_postLoc_mode != -1)
		glUniform1i(g_postLoc_mode, mode);
	if (g_postLoc_texSize != -1)
		glUniform2f(g_postLoc_texSize,
		            g_windowWidth  > 0 ? 1.0f / (float)g_windowWidth  : 0.0f,
		            g_windowHeight > 0 ? 1.0f / (float)g_windowHeight : 0.0f);
	if (g_postLoc_time != -1)
		glUniform1f(g_postLoc_time, (float)(g_postFrame & 1023));
	if (g_postLoc_tonemap != -1)
		glUniform1i(g_postLoc_tonemap, g_cfg_tonemap);
	if (g_postLoc_postInt != -1)
		glUniform1f(g_postLoc_postInt, g_cfg_postProcessIntensity);
	if (g_postLoc_tmInt != -1)
		glUniform1f(g_postLoc_tmInt, g_cfg_tonemapIntensity);

	glBindTexture(GL_TEXTURE_2D, tex);
	glBindVertexArray(g_postVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);

	/* The actual GL state now matches: blend off, depth off, scissor off.
	 * Sync the trackers to that so the next set-call doesn't skip a needed
	 * change; force the shader/texture trackers to rebind. */
	g_PreviousShader      = (ShaderID)-1;
	g_lastBoundTexture    = (TextureID)-1;
	g_PreviousBlendMode   = BM_NONE;
	g_PreviousDepthMode   = 0;
	g_PreviousScissorState = 0;
}

/* PC port: post-process the composed backbuffer in place. Resolves the (possibly
 * multisample) default framebuffer into a single-sample texture, then redraws it
 * full-screen through the selected look. No-op when g_cfg_postProcess <= 0. */
void GR_PostProcess(void)
{
	if (g_cfg_postProcess <= 0 && g_cfg_tonemap <= 0)
		return;
	if (g_postShader == (ShaderID)-1)
		GR_InitPostProcess();

	GR_EnsurePostTarget(g_windowWidth, g_windowHeight);

	/* Resolve/copy backbuffer -> single-sample source texture (same size, so
	 * this is a legal multisample resolve when MSAA is on). */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
	glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	g_postFrame++;
	GR_DrawFullscreenTexture(g_postTex, g_cfg_postProcess);
}

#else  /* !PSYX_HAS_POSTPROCESS */
void GR_InitPostProcess(void) {}
void GR_PostProcess(void) {}
#endif

/* See g_PsxPresentLastFrame above. Called from PsyX_EndScene after the
 * frame is fully composed in the backbuffer, before the swap. */
void GR_CaptureLastFrame(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	/* A frame that re-presented the capture must not be re-captured,
	 * or the UI text drawn on top would bake into the frozen image. */
	if (g_freezePresentedThisFrame)
	{
		g_freezePresentedThisFrame = 0;
		return;
	}

	if (!g_freezeFrameTex)
	{
		glGenTextures(1, &g_freezeFrameTex);
		glGenFramebuffers(1, &g_freezeFrameFBO);
	}

	if (g_freezeFrameW != g_windowWidth || g_freezeFrameH != g_windowHeight)
	{
		g_freezeFrameW = g_windowWidth;
		g_freezeFrameH = g_windowHeight;
		glBindTexture(GL_TEXTURE_2D, g_freezeFrameTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_freezeFrameW, g_freezeFrameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
		g_freezeFrameValid = 0;
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_freezeFrameFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_freezeFrameTex, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight,
		0, 0, g_freezeFrameW, g_freezeFrameH,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	g_freezeFrameValid = 1;
#endif
}

/* Called from PsyX_BeginScene right after the frame clear while the game
 * holds g_PsxPresentLastFrame: re-presents the captured frame so this
 * frame's prims (PAUSED text, console, messages) draw on top of it. */
void GR_PresentLastFrame(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (!g_freezeFrameValid)
		return;

#if PSYX_HAS_POSTPROCESS
	/* MSAA: the default framebuffer is multisample, and a single-sample ->
	 * multisample glBlitFramebuffer is illegal. Draw the captured frame as a
	 * full-screen textured triangle instead (writing into the multisample FBO
	 * is fine). */
	if (g_cfg_msaaSamples > 0 && g_postShader != (ShaderID)-1)
	{
		GR_DrawFullscreenTexture(g_freezeFrameTex, 0);
		g_freezePresentedThisFrame = 1;
		return;
	}
#endif

	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_freezeFrameFBO);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glBlitFramebuffer(0, 0, g_freezeFrameW, g_freezeFrameH,
		0, 0, g_windowWidth, g_windowHeight,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	g_freezePresentedThisFrame = 1;
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
	/* PC port: skip the entire framebuffer→VRAM blit when a TIM-protect
	 * screen is active. Without this, every PsyX_EndScene blits the rendered
	 * frame onto g_vramTexture at (disp.x, disp.y, disp.w, disp.h), which
	 * destroys CLUTs/textures that the game just LoadImage'd into that
	 * region (paper-map CLUT at (224,15) is the canonical victim). */
	if (g_PsxSkipFramebufferStore)
		return;

#if USE_OPENGL
	// set storage size first
	if (g_PreviousFramebuffer.w != w ||
		g_PreviousFramebuffer.h != h)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	g_PreviousFramebuffer.x = x;
	g_PreviousFramebuffer.y = y;
	g_PreviousFramebuffer.w = w;
	g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
	glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

	// before drawing set source and target
	{
		GLuint storeReadFBO = 0;	// default: read straight from the backbuffer
#if PSYX_HAS_POSTPROCESS
		/* MSAA: a multisample backbuffer cannot be the source of a *scaled*
		 * blit (this one shrinks window -> w×h and flips Y). Resolve it
		 * same-size into the single-sample post texture first, then scale
		 * from there. */
		if (g_cfg_msaaSamples > 0)
		{
			GR_EnsurePostTarget(g_windowWidth, g_windowHeight);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
			glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			storeReadFBO = g_postFBO;
		}
#endif
		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, storeReadFBO);		// backbuffer, or resolved MSAA copy
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

		/* PC port: destination is the w-by-h g_fbTexture, so the frame must be
		 * stored at its origin. The old (x, y+h)..(x+w, y) rect only landed
		 * inside the texture when disp.y == 0 — for the second display buffer
		 * (disp.y != 0) the whole blit was clipped away and g_fbTexture kept a
		 * stale older frame, which the next blit then stamped into VRAM. */
		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Blit framebuffer to VRAM screen area

		// before drawing set source and target
		glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

		// rebind vram texture
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

		glBlitFramebuffer(0, 0, w, h,
			x, y + h, x + w, y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		
		// done, unbind
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}

	g_fbStoreValid = 1;

	// after drawing
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();
#endif

	GR_ReadFramebufferDataToVRAM();
#endif
}

/* PC port: re-blit the last stored frame (g_fbTexture) over its framebuffer
 * rect in the CURRENT g_vramTexture. GR_UpdateVRAM must call this after every
 * full vram[] re-upload: any LoadImage between frames triggers a texture swap
 * plus whole-texture upload from the CPU vram[] array, whose framebuffer
 * region only ever holds stale lossy PBO-readback bytes. Framebuffer-feedback
 * effects (Screen_BackgroundMotionBlur and the per-map ghosting overlays that
 * sample getTPage(2, ...) display-buffer pages) then read that junk — the
 * accumulating rainbow corruption in TIM-streaming cutscenes. */
static void GR_RestoreStoredFramebufferRegion(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (!g_fbStoreValid || g_PsxSkipFramebufferStore)
		return;

	const int x = g_PreviousFramebuffer.x;
	const int y = g_PreviousFramebuffer.y;
	const int w = g_PreviousFramebuffer.w;
	const int h = g_PreviousFramebuffer.h;

	if (w <= 0 || h <= 0)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

	glBlitFramebuffer(0, 0, w, h,
		x, y + h, x + w, y,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;

	int stride = w;

	if (!src)
	{
		framebuffer_need_update = 1;

		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	/* Clamp the destination to the VRAM rectangle. A well-formed PSX upload never
	 * exceeds VRAM, but a mismatched/wrong-region TIM can report a rect that runs
	 * off the bottom/right edge — without this the row memcpy walks past vram[]
	 * and crashes (seen feeding a PAL disc a US-shaped upload). */
	if (dst_x < 0) { w += dst_x; src -= dst_x; dst_x = 0; }
	if (dst_y < 0) { h += dst_y; src -= dst_y * stride; dst_y = 0; }
	if (dst_x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - dst_x;
	if (dst_y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - dst_y;
	if (w <= 0 || h <= 0)
		return;

#if defined(__SWITCH__)
	{

		char _dbg[200];
		int _n = snprintf(_dbg, sizeof(_dbg), "[VRAM] CopyVRAM: src=(%d,%d) dst=(%d,%d) w=%d h=%d stride=%d",
			x, y, dst_x, dst_y, w, h, stride);
		if (_n > 0) svcOutputDebugString(_dbg, (size_t)(_n < (int)sizeof(_dbg) ? _n : (int)sizeof(_dbg)-1));
	}
#endif

	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++) {
		SDL_memcpy(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++) {
		SDL_memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	if (!vram_need_update)
		return;

	vram_need_update = 0;

#if USE_OPENGL
	g_vramTexture = g_vramTexturesDouble[g_vramTextureIdx];
	g_vramTextureIdx++;
	g_vramTextureIdx &= 1;

	glBindTexture(GL_TEXTURE_2D, g_vramTexture);

#if defined(RENDERER_OGL)
	glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#else
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#endif

	GR_RestoreStoredFramebufferRegion();

#endif
}

/* PC port: directly upload a sub-region of vram[] to BOTH double-buffered VRAM
 * textures, bypassing the vram_need_update / texture-swap dance. Used by the
 * paper-map TIM-protect helper to guarantee the paper-map data is present in
 * whichever texture the renderer is about to sample, even if some unfound
 * code path has stamped framebuffer pixels into the other texture.
 *
 * Why this exists: the gating in GR_StoreFrameBuffer / GR_ReadFramebufferDataToVRAM
 * via g_PsxSkipFramebufferStore is necessary but apparently not sufficient —
 * paper-map pickup screens still show tiled gameplay framebuffer content
 * sampled from the (320, 16+) VRAM region even with all known framebuffer→VRAM
 * paths gated. A direct upload is the nuclear-option fallback that defeats
 * any unfound corruption path: whatever wrote framebuffer pixels into the
 * GPU texture after the last GR_UpdateVRAM gets unconditionally overwritten. */
void GR_DirectUploadVRAMRegion(int x, int y, int w, int h)
{
#if USE_OPENGL
	if (x < 0 || y < 0 || w <= 0 || h <= 0)
		return;
	if (x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	if (w <= 0 || h <= 0)
		return;

	/* Build a contiguous block from the vram[] sub-region (vram is 1024 stride). */
	unsigned short* block = (unsigned short*)malloc((size_t)w * (size_t)h * sizeof(unsigned short));
	if (!block)
		return;

	for (int row = 0; row < h; row++)
	{
		SDL_memcpy(block + (size_t)row * w,
		           vram + (size_t)(y + row) * VRAM_WIDTH + x,
		           (size_t)w * sizeof(unsigned short));
	}

	for (int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, VRAM_FORMAT, GL_UNSIGNED_BYTE, block);
	}
	glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

	free(block);
#endif
}

/* PC port debug: dump the entire 1024x512 16-bit VRAM to a raw file so the
 * loaded textures/CLUTs can be inspected offline (decode 5551). */
void GR_DumpVRAM(const char* path)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(vram, sizeof(unsigned short), VRAM_WIDTH * VRAM_HEIGHT, f);
	fclose(f);
}

void GR_DumpVRAMRegion(const char* path, int x, int y, int w, int h)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	if (x < 0 || y < 0 || w <= 0 || h <= 0) { fclose(f); return; }
	if (x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	if (w <= 0 || h <= 0) { fclose(f); return; }

	fwrite(&w, sizeof(short), 1, f);
	fwrite(&h, sizeof(short), 1, f);
	for (int row = 0; row < h; row++)
		fwrite(vram + (size_t)(y + row) * VRAM_WIDTH + x, sizeof(unsigned short), w, f);
	fclose(f);
}

void GR_SwapWindow()
{
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	SDL_GL_SwapWindow(g_window);
#endif

	//glFinish();
}

void GR_EnableDepth(int enable)
{
	if (g_PreviousDepthMode == enable)
		return;

	g_PreviousDepthMode = enable;

#if USE_OPENGL
	if (enable && g_cfg_pgxpZBuffer)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

void GR_SetStencilMode(int drawPrim)
{
	if (g_PreviousStencilMode == drawPrim)
		return;

	g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
#endif
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (g_PreviousBlendMode == blendMode)
		return;

#if USE_OPENGL
	/* Fog mode for this blend: additive/subtractive prims (blood, muzzle flash) must fade
	 * toward black under fog, not blend toward the light fog color (which whitened their
	 * edges/faded pixels in daytime). Push now — the fog shader is the bound program here —
	 * and again at every shader bind, so it's correct regardless of bind/blend order. */
	g_PsxFogToBlack = (blendMode == BM_ADD || blendMode == BM_SUBTRACT ||
	                   blendMode == BM_ADD_QUATER_SOURCE) ? 1 : 0;
	if (u_fogToBlackLoc != -1)
		glUniform1i(u_fogToBlackLoc, g_PsxFogToBlack);
	if (blendMode == BM_NONE)
	{
		if (g_PreviousBlendMode != BM_NONE)
		{
			glBlendColor(1.f, 1.f, 1.f, 1.f);
			glDisable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(1);
		return;
	}
	else
	{
		if(g_PreviousBlendMode == BM_NONE)
		{
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
			glEnable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(0);
	}

	glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
	switch (blendMode) {
	case BM_AVERAGE:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case BM_ADD:
	case BM_SUBTRACT:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE); 
		break;
	}
#endif

	g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float ofs)
{
#if USE_OPENGL
	if (ofs == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, ofs);
	}
#endif
}

void GR_SetViewPort(int x, int y, int width, int height)
{
#if USE_OPENGL
	glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable)
{
#if defined(RENDERER_OGL)
	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer()
{
#if USE_OPENGL
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);

	glEnableVertexAttribArray(a_position);
	glEnableVertexAttribArray(a_texcoord);
	glEnableVertexAttribArray(a_color);
	glEnableVertexAttribArray(a_extra);

	glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
	glVertexAttribPointer(a_zw, 1, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->z);
	glEnableVertexAttribArray(a_zw);
	glVertexAttribPointer(a_pgxp, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->ppx);
	glEnableVertexAttribArray(a_pgxp);
	glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->u);
	glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex*)NULL)->r);
	glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->tcx);
	glVertexAttribPointer(a_normal, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->nx);
	glEnableVertexAttribArray(a_normal);
	glVertexAttribPointer(a_viewpos, 3, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->vsx);
	glEnableVertexAttribArray(a_viewpos);

	g_curVertexBuffer++;
	g_curVertexBuffer &= 1;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	//assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
	GR_BindVertexBuffer();

#if USE_OPENGL
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
#if USE_OPENGL
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char* label)
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPushDebugGroup)
		return;
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel()
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPopDebugGroup)
		return;
	glPopDebugGroup();
#endif
}