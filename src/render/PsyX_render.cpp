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
/* Vertical view shift (amount, PSX screen-Y units) for FIXED-ANGLE camera shots, which
 * frame the top of the scene clipped vs PSX (e.g. a medkit off the top). The GAME applies
 * it (MainLoop, game_main.c) by shifting the GTE projection center down (SetGeomOffset)
 * while g_PsxFixedCamActive is set — NOT by shifting the ortho window here: the ortho
 * shift revealed rows above the frame that screen-space overlay prims (authored 0..224)
 * never cover, showing a faded band at the top. + = view up. Console `vshift`. */
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
int g_PreviousDepthFuncAlways = 0; /* 0 = glDepthFunc(GL_LEQUAL) (init default), 1 = GL_ALWAYS */
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
TextureID g_rgLutTexture = -1;
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

/* PC port: Hor+ zoom factor. 1.0 = stock Hor+ (full reveal). >1 zooms the
 * 3D world in uniformly (both axes — no distortion), which makes the
 * character larger and crops the beyond-the-map void that wide angles
 * reveal on maps modeled only for the 4:3 view. Config key
 * `widescreen_zoom` (0.5..2.0). */
float g_PcWidescreenZoom = 1.0f;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;

/* PC port (Silent Hill): runtime master gate for PGXP. When 0,
 * MakeVertex* writes 0 into a_zw, the shader takes the `a_zw.y > 100.0`
 * else branch (2D-ortho path). Set from main_pc.c after PcConfig_Load runs
 * (config key: use_pgxp). */
int g_PsxUsePgxp = 0;
int g_cfg_bilinearFiltering = 0;
/* 1 = bilinear-filter menu / 2D-only frames (those set g_PsxDitherSuppressed),
 * independent of the 3D psx_dither setting. Passed to the sampler as bilinearFilter==2. */
int g_cfg_menuFilter = 0;
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
#if defined(__SWITCH__)
/* g_cfg_msaaSamples, g_cfg_postProcess defined in main_pc.c */
extern int g_cfg_msaaSamples;
extern int g_cfg_postProcess;
#else
int g_cfg_msaaSamples = 0;

/* PC port: full-screen post-process look applied once per frame in
 * GR_PostProcess (PsyX_EndScene, after the freeze capture + console hook, just
 * before swap). 0 = off; 1.. select a built-in look (see the post fragment
 * shader switch). Runtime-settable (launcher config key post_process + the F2
 * in-game cycle). Reads the final composed backbuffer through a resolve
 * texture, so it sees everything (world, UI, console) and is MSAA-safe. */
int g_cfg_postProcess = 0;
#endif
#define POST_PROCESS_MODE_COUNT 8
/* PC port: tone-map operator applied as the final step of the post-process
 * shader. 0=off, 1=Reinhard, 2=ACES, 3=Filmic. F3 cycles it in-game. Defined
 * outside the PSYX_HAS_POSTPROCESS guard so non-GL builds still link. */
int g_cfg_tonemap = 0;

/* PC port: per-pixel (fragment-shader) flashlight cone instead of the PSX
 * per-vertex lighting. 0=off (per-vertex), 1=on. F4 toggles it in-game. */
int g_PsyX_UsePerPixelFlashlight = 0;

/* Per-pixel flashlight STYLE. 0 = MODERN: the stylized spotlight (per-fragment
 * Lambert, hard dark surround, linear falloff, warm color — the pre-PR#7 look).
 * 1 = CLASSIC: PSX-calibrated match of the original flashlight (PR#7 —
 * orientation-independent overlay, func_80057658-derived falloff, room color).
 * Pushed to the shader as u_flStyle; game/config key flashlight_style. */
int g_PsyX_FlashlightStyle = 0;

/* PC port: real flashlight shadow mapping. When on (and the per-pixel flashlight
 * is on + active), the frame's opaque geometry is rendered depth-only from the
 * flashlight's point of view into g_shadowDepthTex, and the cone fragment shader
 * samples it so monsters/props cast dynamic shadows inside the beam. 0 = off
 * (rendered output byte-identical to per-pixel flashlight without shadows). */
int   g_PsyX_UseFlashlightShadows = 0;
/* Depth-compare bias in light-clip [0,1] space; tunable via `shadowbias` console. */
float g_PsyX_FlashlightShadowBias = 0.0018f;
/* Optional shadow-look console tweaks, all DEFAULTING TO NO-OP. They exist for
 * live tuning of the prop-shadow "silhouette" look; see the shader.
 *
 * Receiver offset (`shadownormal`, 0 = off): move the sample toward the light
 * along the light ray by (this * dist-to-light). */
float g_PsyX_FlashlightShadowNormalOffset = 0.0f;
/* How much light a fully-occluded pixel loses (`shadowstrength`): 1.0 = pitch black
 * (default/original), lower = softer half-shadow. */
float g_PsyX_FlashlightShadowStrength = 1.0f;
/* Contact-shadow fade distance in view units (`shadowfade`, 0 = off): when > 0, a
 * receiver this far BEHIND its occluder gets no shadow, so a prop drops a tight
 * fading contact shadow instead of a tall silhouette smeared onto the wall. */
float g_PsyX_FlashlightShadowFadeDist = 0.0f;
/* The shadow frustum's near/far, published by GR_BuildShadowMatrix so the cone
 * shader can linearize shadow-map depth for the contact fade above. */
static float g_shadowZNear = 20.0f;
static float g_shadowZFar  = 5200.0f;

/* PC port: per-frame flashlight cone parameters (view space), set by game code.
 * The shader consumes them only when (g_PsyX_UsePerPixelFlashlight &&
 * g_PsyX_FlashlightActive). Defaults make the cone inert (active=0). */
int   g_PsyX_FlashlightActive   = 0;
/* PC port: shadow depth pre-pass master gate. The game re-arms this to 1 only
 * during settled gameplay (SysState_Gameplay, no screen fade / cutscene) and it
 * is reset to 0 at the top of every frame. The flashlight CONE is fine outside
 * gameplay, but the light-POV depth pre-pass + its GL-state churn corrupt
 * unrelated rendering on menu / room-load / transition frames (white flash on
 * transitions, dropped geometry on the options screen). Shadows are a
 * live-gameplay-only effect, so gate the whole effect on this. */
int   g_PsyX_ShadowsAllowed     = 0;
float g_PsyX_FlashlightPos[3]   = { 0.0f, 0.0f, 0.0f };
/* View-space PHYSICAL flashlight position for the shadow map (Harry's chest/hand
 * light bone). Equals g_PsyX_FlashlightPos in third person, but in FPS the cone
 * is pinned at the eye while THIS stays at the real light — a shadow is a
 * world-space fact of light+occluder, so using the true light position makes FPS
 * shadows land exactly where the third-person camera shows them. */
float g_PsyX_FlashlightShadowPos[3] = { 0.0f, 0.0f, 0.0f };
float g_PsyX_FlashlightDir[3]   = { 0.0f, 0.0f, 1.0f };
float g_PsyX_FlashlightColor[3] = { 1.0f, 1.0f, 1.0f };
float g_PsyX_FlashlightInnerCos = 0.94f;  /* ~20 deg */
float g_PsyX_FlashlightOuterCos = 0.76f;  /* ~41 deg */
float g_PsyX_FlashlightRange    = 4000.0f;
/* PC port: flashlight cone coverage-area multiplier, applied to Inner/OuterCos at
 * push time (1.0 = base cone; 1.5 default = ~1.5x coverage). Live-editable via
 * [ / ] + backslash and persisted as config key flashlight_size. */
float g_PsyX_FlashlightSize     = 3.0f;

/* PC port: live per-effect intensity -- [ lowers, ] raises, backslash switches
 * which effect (among the enabled ones); also the FLINT/POSTINT/TMINT console
 * commands. Persisted to config. */
float g_PsyX_FlashlightIntensity = 1.20f; /* cone brightness scale, 0..3 */
/* FPS-mode flashlight overrides: a head-mounted light wants a tighter, dimmer
 * cone than the third-person one. g_PsyX_FlashlightFpsMode is set by the game
 * each frame (= g_PcFpsCam); when 1 the shader uses these instead of the values
 * above. Separately config/console-tunable (flashlight_*_fps). */
float g_PsyX_FlashlightSizeFps      = 1.30f;
float g_PsyX_FlashlightIntensityFps = 2.10f;
int   g_PsyX_FlashlightFpsMode      = 0;
/* FPS shadow parallax. In first person the game pins the flashlight at the eye
 * (bodyprog_80055028.c) so the cone follows the view — but that makes the SHADOW
 * light coincident with the camera, so the depth map equals the camera's own
 * view and every visible surface reads as lit (no shadows). Pull the shadow
 * light BACK along -viewDir (behind the camera) to restore parallax so occluders
 * throw their shadow forward onto the wall/floor; the cone still originates at
 * the eye. (Earlier this dropped straight DOWN, which sat the light below
 * waist-high props and threw their silhouette up out of the object's top.) TPS
 * is unaffected (FpsMode 0). Tunable via `shadowfpsdrop`. Default 0: the shadow
 * now originates at the real chest/hand light (g_PsyX_FlashlightShadowPos), so no
 * artificial offset is needed; this is just an optional extra-parallax nudge. */
float g_PsyX_FlashlightShadowFpsDrop = 0.0f;
float g_cfg_postProcessIntensity = 1.0f; /* post-process effect mix, 0..1 */
float g_cfg_tonemapIntensity     = 1.0f; /* tonemap mix, 0..1 */

/* Image adjustments applied to the final frame ALWAYS (not gated on the
 * post_process filter). 1.0 = neutral for all three. Set from the Brightness
 * screen. When any is off-neutral, GR_PostProcess runs even with no filter. */
float g_cfg_brightness = 1.0f;
float g_cfg_contrast   = 1.0f;
float g_cfg_saturation = 1.0f;

/* Defined later in the file (post-process module); called from GR_InitialisePSX
 * and PsyX_EndScene. */
void GR_InitPostProcess(void);
void GR_PostProcess(void);

int vram_need_update = 1;

/* PC port: runtime gate for framebuffer→VRAM feedback. See PsyX_render.h. */
int g_PsxSkipFramebufferStore = 0;

/* PC port: framebuffer feedback is LOADING-SCREEN ONLY for now. The generic
 * store also drives the per-map dream/ghosting overlays (map6 otherworld,
 * cutscene ghosts, the rifle scene, ...), which were never made correct on PC
 * and corrupt (striping / ghosted subtitles). Screen_BackgroundMotionBlur — the
 * only loading/transition blur — arms this to 2 each frame it draws (a short
 * trailing window so a 1-frame gap doesn't flip it off). While it is 0,
 * GR_StoreFrameBufferPsx stamps the feedback rects BLACK (word 0 → the samplers
 * discard → draw nothing) instead of leaving a real/stale frame for those
 * overlays to ghost. Re-enable per-scene once their geometry is fixed. */
int g_PsxFeedbackStoreAllowed = 0;

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

#if !defined(__ANDROID__) && !defined(RENDERER_OGLES)
	/* PC port: take the foreground on launch. The launcher spawns the game and
	 * keeps keyboard focus itself, so the FIRST Enter the player presses to skip
	 * the intro lands on the launcher's default (Launch) button and spawns ANOTHER
	 * copy of the game — repeatedly. Windows lets a child of the foreground process
	 * set foreground, so raising the freshly-created window puts intro-skip (and
	 * all) input on the game where it belongs. */
	SDL_ShowWindow(g_window);
	SDL_RaiseWindow(g_window);
#endif

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
#ifdef USE_GLAD
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
	GR_DestroyTexture(g_rgLutTexture);
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
	GLint texOffsetLoc;
	GLint hiresHalfLoc;
	GLint fogColorLoc;
	GLint fogToBlackLoc;
	GLint fogStrengthLoc;
	GLint pgxpEnabledLoc;
	GLint szMaxLoc;
	GLint pgxpFarWLoc;
	GLint worldFarBiasLoc;
	GLint flashlightOnLoc;
	GLint untexturedLoc;
	GLint flStyleLoc;
	GLint flLightPosLoc;
	GLint flDirLoc;
	GLint flColorLoc;
	GLint flInnerCosLoc;
	GLint flOuterCosLoc;
	GLint flRangeLoc;
	GLint shadowOnLoc;
	GLint shadowMatrixLoc;
	GLint shadowBiasLoc;
	GLint shadowTexelLoc;
	GLint shadowNormalOffsetLoc;
	GLint shadowStrengthLoc;
	GLint shadowClipLoc;
	GLint shadowFadeDistLoc;
#endif
} GTEShader;

GTEShader g_gte_shader_4;
GTEShader g_gte_shader_8;
GTEShader g_gte_shader_16;
GTEShader g_gte_shader_32_rgba;
GTEShader g_modern_shader_4, g_modern_shader_8, g_modern_shader_16, g_modern_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_bilinearFilterLoc;
GLint u_ditherForceLoc;
GLint u_pixelScaleLoc;
GLint u_texelSizeLoc;
GLint u_texOffsetLoc;
GLint u_hiresHalfLoc;
GLint u_fogColorLoc;
GLint u_fogToBlackLoc;
GLint u_fogStrengthLoc;
GLint u_pgxpEnabledLoc;
GLint u_szMaxLoc;
GLint u_worldFarBiasLoc;
/* Depth channel Step 4: writer-side world far-push margin M (PsyX_GPU.cpp). */
extern "C" int g_PsxPgxpWorldFarBias;
GLint u_pgxpFarWLoc;
GLint u_flashlightOnLoc;
GLint u_untexturedLoc;
GLint u_flStyleLoc;
GLint u_flLightPosLoc;
GLint u_flDirLoc;
GLint u_flColorLoc;
GLint u_flInnerCosLoc;
GLint u_flOuterCosLoc;
GLint u_flRangeLoc;
GLint u_shadowOnLoc;
GLint u_shadowMatrixLoc;
GLint u_shadowBiasLoc;
GLint u_shadowTexelLoc;
GLint u_shadowNormalOffsetLoc;
GLint u_shadowStrengthLoc;
GLint u_shadowClipLoc;
GLint u_shadowFadeDistLoc;

/* Flashlight shadow map (see g_PsyX_UseFlashlightShadows). Depth-only FBO rendered
 * from the light POV each frame; g_shadowLightMatrix maps view space -> light clip.
 * Column-major, identity until the first shadow pass computes it. */
#define PSYX_SHADOW_MAP_SIZE 1024
static GLuint g_shadowFBO = 0;
static GLuint g_shadowDepthTex = 0;
static ShaderID g_shadowDepthShader = (ShaderID)-1;
static GLint g_shadowDepthMatrixLoc = -1;
static float g_shadowLightMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

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

/* Colour comes from a CPU-baked table (GR_InitRG8LUT) on texture unit 2
 * instead of being reconstructed per pixel with float division + floor():
 * samplePSX now hands back the two raw VRAM bytes and lut() indexes the table
 * with them, so no driver-sensitive arithmetic is left in the decode.
 *
 * Both the index and the sampled colour are recovered with floor(x*255+0.5) —
 * the same snap GPU_FETCH_VRAM_FUNC already uses — so neither depends on how
 * precisely a driver normalises UNSIGNED_BYTE to float. The table bakes each
 * 5-bit channel as v<<3, and v*8/256 == v/32 exactly in binary32, which is
 * what the old fract(floor(rg / K) / 32.0) produced: the decode is bit-exact
 * against the previous one for all 65536 inputs, colour and alpha alike.
 * Alpha must stay exactly 0.0 / 0.5 / 1.0 because BM_AVERAGE feeds it straight
 * to GL_SRC_ALPHA, and it is resolved per texel (not after the bilinear mix)
 * so a colour-0 texel still reads as a hole when its neighbours do not. */
#define GPU_PACK_RG_FUNC\
	"	uniform sampler2D s_rgLut;\n"

#define GPU_DECODE_RG_FUNC\
	"	vec4 lut(vec2 rg) {\n"\
	"		vec2 idx = (floor(rg * 255.0 + 0.5) + 0.5) * (1.0 / 256.0);\n"\
	"		vec4 t = texture2D(s_rgLut, idx);\n"\
	"		vec3 c = floor(t.rgb * 255.0 + 0.5) * (1.0 / 256.0);\n"\
	"		return vec4(c, (rg.x + rg.y == 0.0) ? 0.0 : (t.w > 0.25 ? 0.5 : 1.0));\n"\
	"	}\n"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

/* PSX 4x4 ordered dither.
 *
 * Native hardware applies a signed offset before quantizing to the 5-bit
 * framebuffer. We match the matrix the original game's GPU uses (values
 * in [-4..+3]) divided by 255 so it lands in 24-bit color space.
 *
 * `u_ditherForce` is the master enable the PC config exposes: 1 when the
 * user selects "PSX dither", 0 for both "off" and "bilinear". When 0 we
 * emit NO dither at all — the per-primitive `v_texcoord.w` tpage flag is
 * deliberately NOT OR'd in here, because otherwise prims whose tpage sets
 * the DTD bit would still show a dither pattern with the setting off.
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
		"		float dStrength = u_ditherForce * v_is3d;\n"\
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
		"		float dStrength = u_ditherForce * v_is3d * (1.0 - float(u_fogToBlack));\n"\
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
    "   // returns the two VRAM bytes (lut index)\n"\
    "   vec2 samplePSX(vec2 tc){\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (255.0 / 16.0);\n"\
    "       float f = floor(v + 0.001);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return VRAM(clut_pos);\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns the two VRAM bytes (lut index)\n"\
	"	vec2 samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;\n"\
	"		return VRAM(clut_pos);\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	vec2 samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		return VRAM(uv);\n"\
	"	}\n"


#define GPU_BILINEAR_SAMPLE_FUNC \
	"	float c_textureSize = 1.0;\n"\
	"	float c_onePixel = 1.0;\n"\
	"	vec4 BilinearTextureSample(vec2 P) {\n"\
	"		vec2 frac = fract(P);\n"\
	"		vec2 pixel = floor(P);\n"\
	"		vec2 C11 = samplePSX(pixel);\n"\
	"		vec2 C21 = samplePSX(pixel + vec2(c_onePixel, 0.0));\n"\
	"		vec2 C12 = samplePSX(pixel + vec2(0.0, c_onePixel));\n"\
	"		vec2 C22 = samplePSX(pixel + vec2(c_onePixel, c_onePixel));\n"\
	"		float ax1 = mix(float(C11.x + C11.y > 0.0), float(C21.x + C21.y > 0.0), frac.x);\n"\
	"		float ax2 = mix(float(C12.x + C12.y > 0.0), float(C22.x + C22.y > 0.0), frac.x);\n"\
	"		if(mix(ax1, ax2, frac.y) < 0.5) { discard; }\n"\
	"		vec4 x1 = mix(lut(C11), lut(C21), frac.x);\n"\
	"		vec4 x2 = mix(lut(C12), lut(C22), frac.x);\n"\
	"		return mix(x1, x2, frac.y);\n"\
	"	}\n"

#define GPU_NEAREST_SAMPLE_FUNC \
	"vec4 NearestTextureSample(vec2 P) {\n"\
	"	vec2 rg = samplePSX(P);\n"\
	"	if(rg.x + rg.y == 0.0) {discard;}\n"\
	"	return lut(rg);\n"\
	"}\n"

/* The VRAM texture stores each 16-bit PSX pixel as two normalised bytes
 * (low/high). Every downstream step — the 4/8-bit CLUT index math and the
 * decode-table index — treats the sampled value as an exact k/255 and feeds it
 * to floor(). The Windows GL driver normalises UNSIGNED_BYTE -> float
 * precisely enough that this holds; Mesa (Steam Deck / Proton) rounds
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
		"		if (u_pgxpFarW > 0.0) W = min(W, u_pgxpFarW);\n"\
		/* Depth channel Step 4: marked OPAQUE WORLD verts (a_extra.w, set by
		 * ApplyGtePerVertexDepth for GL_ALWAYS-class prims only) take true
		 * per-vertex depth from the unquantized view W (a_pgxp.z, pre-FarW-
		 * clamp), on the SAME constant linear scale as every flat depth:
		 * ndc = 2*vz/F - 1 — the ortho Projection negates a_zw.x, so this is
		 * exactly what the flat path yields for the same vz. World never
		 * depth-tests itself (ALWAYS painter), so this only makes the depth
		 * field actors LEQUAL against per-pixel accurate — closing the
		 * distant grazing gaps flat-average depth leaves. */\
		"		if (a_extra.w > 0.5) {\n"\
		"			float dvz = min(a_pgxp.z + u_worldFarBias, u_szMax);\n"\
		"			b.z = (2.0 * dvz / u_szMax - 1.0) * b.w;\n"\
		"		}\n"\
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
	"	uniform float u_pgxpFarW;\n"\
	"	uniform float u_worldFarBias;\n"\
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
	 * a_pgxp.z is the PGXP view W: > 0 only on vertices the GTE
	 * actually projected, which is exactly the 3D content. It only
	 * separates 3D from 2D while the runtime PGXP master gate is on —
	 * with PGXP off PgxpFillVertex never runs, so a_pgxp.z is 0 for
	 * everything and without the u_pgxpEnabled override every prim
	 * would read v_is3d=0 and lose dither / bilinear on real 3D
	 * geometry too (visibly blocky tree leaves, etc.). PGXP off falls
	 * back to "always treat as 3D" — the legacy behaviour. Anything
	 * that must hold with PGXP off therefore cannot lean on v_is3d;
	 * use the frame class (g_PsxDitherSuppressed) instead. */	"		v_is3d = (u_pgxpEnabled > 0) ? ((a_pgxp.z > 0.0) ? 1.0 : 0.0) : 1.0;\n"\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"		v_fogAmount = clamp(a_extra.z / 127.0, 0.0, 1.0);\n"\
	"		v_viewpos = a_viewpos;\n"\
	/* The legacy affine screen path has gl_Position.w == 1, so v_viewpos is not
	 * perspective-correct there. Encode receiver position over view Z, adjusted
	 * for whichever clip W this vertex uses, then reconstruct it in the fragment
	 * shader. This also stays coherent for mixed PGXP/fallback triangles. */\
	"		float shadowInvZ = (a_viewpos.z > 0.0) ? (1.0 / a_viewpos.z) : 0.0;\n"\
	"		float shadowClipW = (u_pgxpEnabled > 0 && a_pgxp.z > 0.0) ? ((u_pgxpFarW > 0.0) ? min(a_pgxp.z, u_pgxpFarW) : a_pgxp.z) : 1.0;\n"\
	"		v_shadowViewPos = vec4(a_viewpos * shadowInvZ, shadowInvZ) * shadowClipW;\n"\
	"	}\n"

/* Fog + per-pixel flashlight + shadow uniforms shared by every shader that
 * renders lit world geometry - including the 32-bit RGBA override shader,
 * whose textures (virtual pool slots, hi-res/pack replacements) cover the
 * same world surfaces as the VRAM samplers. */
#define GPU_LIT_UNIFORMS\
	"	uniform vec3 u_fogColor;\n"\
	"	uniform int u_fogToBlack;\n"\
	"	uniform float u_fogStrength;\n"\
	"	uniform int u_flashlightOn;\n"\
	"	uniform int u_untextured;\n"\
	"	uniform int u_flStyle;\n"\
	"	uniform vec3 u_flLightPos;\n"\
	"	uniform vec3 u_flDir;\n"\
	"	uniform vec3 u_flColor;\n"\
	"	uniform float u_flInnerCos;\n"\
	"	uniform float u_flOuterCos;\n"\
	"	uniform float u_flRange;\n"\
	"	uniform int u_shadowOn;\n"\
	"	uniform sampler2D u_shadowTex;\n"\
	"	uniform mat4 u_shadowMatrix;\n"\
	"	uniform float u_shadowBias;\n"\
	"	uniform vec2 u_shadowTexel;\n"\
	"	uniform float u_shadowNormalOffset;\n"\
	"	uniform float u_shadowStrength;\n"\
	"	uniform vec2 u_shadowClip;\n"\
	"	uniform float u_shadowFadeDist;\n"\
	/* Window depth [0,1] -> linear distance along the light's forward axis, using the shadow frustum's near/far (u_shadowClip). Lets us measure how far a receiver sits BEHIND its occluder in world units. */\
	"	float shLinDepth(float zw) {\n"\
	"		float ndc = zw * 2.0 - 1.0;\n"\
	"		return (2.0 * u_shadowClip.x * u_shadowClip.y) / (u_shadowClip.y + u_shadowClip.x - ndc * (u_shadowClip.y - u_shadowClip.x));\n"\
	"	}\n"

/* The lit fragment tail: vertex-color modulate, per-pixel flashlight +
 * shadow, then fog - dither/quantize follows as the very last op. Shared so
 * override-drawn geometry matches the VRAM samplers exactly (missing fog on
 * override surfaces was visible as unfogged distant walls). */
#define GPU_LIT_TAIL\
	"		vec3 flAlbedo = fragColor.rgb;\n"\
	"		fragColor *= v_color;\n"\
	/* Untextured prims sample the white placeholder, so their texture "albedo"
	 * is 1.0 — the beam would add full-white light onto geometry whose real
	 * color is the (often dark) vertex color, and stacked additive layers then
	 * saturate to a white blob (sewer water octagon under PGXP, where the
	 * perspective-correct v_viewpos also resolves grazing pixels much closer
	 * to the light). Use the vertex-lit color as the albedo instead — bounded
	 * by what the surface actually looks like. Textured splits (u_untextured=0)
	 * are byte-identical. */\
	"		if (u_untextured > 0) flAlbedo = fragColor.rgb;\n"\
	/* Two flashlight styles, chosen by the UNIFORM u_flStyle (uniform control flow, so derivative use inside the branch is well-defined). 1 = CLASSIC: PSX-calibrated orientation-independent overlay -- no face normals, func_80057658-derived falloff, eased wide cone, 0.49 base dim. 0 = MODERN: stylized spotlight -- per-fragment Lambert from a dFdx/dFdy-reconstructed face normal, linear falloff, hard 0.15 dark surround. */\
	"		if (u_flashlightOn > 0) {\n"\
	"			vec3 flP = v_viewpos;\n"\
	"			if (flP.z > 0.0) {\n"\
	"				fragColor.rgb *= (u_flStyle > 0) ? 0.49 : 0.15;\n"\
	"				vec3 flDir = normalize(u_flDir);\n"\
	"				vec3 flOrigin = (u_flStyle > 0) ? (u_flLightPos - flDir * 39.0) : u_flLightPos;\n"\
	"				vec3 L = flOrigin - flP;\n"\
	"				float d = length(L);\n"\
	"				L /= max(d, 0.0001);\n"\
	"				float cone  = smoothstep(u_flOuterCos, u_flInnerCos, dot(-L, flDir));\n"\
	"				float ndl = 1.0;\n"\
	"				float atten;\n"\
	"				if (u_flStyle > 0) {\n"\
	"					cone = cone * (2.0 - cone);\n"\
	/* Classic center-beam distance envelope derived from SH1's func_80057658 at full Q12 flashlight strength: its GTE projection reduces to a capped 1/d term plus a thresholded 1/d^2 term, normalized by the room-light cap. */\
	"					float attenD = d * 2.0;\n"\
	"					float invD = 1.0 / max(attenD, 1.0);\n"\
	"					atten = max(0.0, 134217728.0 * invD * invD - 16.0);\n"\
	"					atten += min(48.0, 32768.0 * invD);\n"\
	"					atten = clamp(atten / 255.0, 0.0, 1.0);\n"\
	"					atten *= 1.0 - smoothstep(u_flRange * 0.9, u_flRange, attenD);\n"\
	"				} else {\n"\
	/* Modern: GrVertex carries no usable normals, so the face normal is reconstructed from the view-space position gradient -- exact per triangle face, no GTE-side capture needed. The N.L term gives surfaces 3D shape under the beam. */\
	"					vec3 flN = cross(dFdx(flP), dFdy(flP));\n"\
	"					float nlen = length(flN);\n"\
	"					vec3 N = (nlen > 1e-9) ? flN / nlen : vec3(0.0, 0.0, -1.0);\n"\
	"					if (dot(N, flP) > 0.0) N = -N;\n"\
	"					ndl = 0.15 + 0.85 * max(dot(N, L), 0.0);\n"\
	"					atten = clamp(1.0 - d / u_flRange, 0.0, 1.0);\n"\
	"				}\n"\
	/* Bilinearly interpolated 3x3 PCF avoids kernel jumps as the projected receiver crosses shadow texels. */\
	"				float shadow = 1.0;\n"\
	"				if (u_shadowOn > 0) {\n"\
	/* Perspective-correct shadow receiver (PR#8): correctness for BOTH styles -- same shadow shapes, just stable under motion. */\
	"					vec3 flShadowP = v_shadowViewPos.xyz / max(v_shadowViewPos.w, 1e-9);\n"\
	"					vec3 flPs = flShadowP + L * (u_shadowNormalOffset * d);\n"\
	"					vec4 lp = u_shadowMatrix * vec4(flPs, 1.0);\n"\
	"					if (lp.w > 0.0) {\n"\
	"						vec3 luv = lp.xyz / lp.w * 0.5 + 0.5;\n"\
	"						if (luv.x > 0.0 && luv.x < 1.0 && luv.y > 0.0 && luv.y < 1.0 && luv.z < 1.0) {\n"\
	"							vec3 luvDx = dFdx(luv);\n"\
	"							vec3 luvDy = dFdy(luv);\n"\
	"							float det = luvDx.x * luvDy.y - luvDx.y * luvDy.x;\n"\
	"							float dzdu = 0.0;\n"\
	"							float dzdv = 0.0;\n"\
	"							if (abs(det) > 1e-9) {\n"\
	"								dzdu = (luvDx.z * luvDy.y - luvDy.z * luvDx.y) / det;\n"\
	"								dzdv = (luvDy.z * luvDx.x - luvDx.z * luvDy.x) / det;\n"\
	"							}\n"\
	"							float recvLin = (u_shadowFadeDist > 0.0) ? shLinDepth(luv.z) : 0.0;\n"\
	"							float occ = 0.0;\n"\
	"							vec2 texelPos = luv.xy / u_shadowTexel - vec2(0.5);\n"\
	"							vec2 texelBase = floor(texelPos);\n"\
	"							vec2 texelFrac = fract(texelPos);\n"\
	"							for (int sy = -1; sy <= 2; sy++) {\n"\
	"								float wy = (sy == -1) ? (1.0 - texelFrac.y) : ((sy == 2) ? texelFrac.y : 1.0);\n"\
	"								for (int sx = -1; sx <= 2; sx++) {\n"\
	"									float wx = (sx == -1) ? (1.0 - texelFrac.x) : ((sx == 2) ? texelFrac.x : 1.0);\n"\
	"									float weight = wx * wy;\n"\
	"									vec2 suv = (texelBase + vec2(float(sx), float(sy)) + vec2(0.5)) * u_shadowTexel;\n"\
	"									float sd = texture2D(u_shadowTex, suv).r;\n"\
	"									float receiverDepth = luv.z + dzdu * (suv.x - luv.x) + dzdv * (suv.y - luv.y);\n"\
	"									if (receiverDepth - u_shadowBias > sd) {\n"\
	"										if (u_shadowFadeDist > 0.0) {\n"\
	"											float gap = recvLin - shLinDepth(sd);\n"\
	"											occ += weight * (1.0 - clamp(gap / u_shadowFadeDist, 0.0, 1.0));\n"\
	"										} else {\n"\
	"											occ += weight;\n"\
	"										}\n"\
	"									}\n"\
	"								}\n"\
	"							}\n"\
	"							shadow = 1.0 - u_shadowStrength * (occ / 9.0);\n"\
	"						}\n"\
	"					}\n"\
	"				}\n"\
	"				vec3 fl = u_flColor * (cone * atten * ndl * shadow);\n"\
	"				fragColor.rgb += flAlbedo * fl;\n"\
	"			}\n"\
	"		}\n"\
	"		float fogAmt = clamp(v_fogAmount * u_fogStrength, 0.0, 1.0);\n"\
	"		if (u_fogToBlack > 0)\n"\
	"			fragColor.rgb *= (1.0 - fogAmt);\n"\
	"		else\n"\
	"			fragColor.rgb = mix(fragColor.rgb, u_fogColor, fogAmt);\n"

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
	GPU_LIT_UNIFORMS\
	"	void main() {\n"\
	"		if((bilinearFilter == 1 && v_is3d > 0.5) || bilinearFilter >= 2)\n"\
	"			fragColor = BilinearTextureSample(v_texcoord.xy);\n"\
	"		else\n"\
	"			fragColor = NearestTextureSample(v_texcoord.xy);\n"\
	GPU_LIT_TAIL\
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
	"varying vec4 v_shadowViewPos;\n"
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
	"varying vec4 v_shadowViewPos;\n"
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
	"varying vec4 v_shadowViewPos;\n"
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
	"varying vec4 v_shadowViewPos;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	"	uniform sampler2D s_texture;\n"\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec2 texelSize;\n"\
	"	uniform vec2 u_texOffset;\n"\
	"	uniform vec2 u_hiresHalf;\n"\
	GPU_LIT_UNIFORMS\
	"	void main() {\n"\
	/* u_texOffset: the prim's tpage origin relative to the replaced TIM's
	 * VRAM origin, in native texels. A surface wider than one tpage is drawn
	 * as several prims whose UVs each restart at their own tpage — without
	 * the offset every chunk sampled the override from x=0 (duplicated image).
	 *
	 * u_hiresHalf: half a HIRES texel in native-texel units (0.5*native/hires
	 * per axis). The old "+ 0.5 native texel" shift pushed edge fragments up
	 * to half a texel into the NEIGHBORING atlas cell (white marks hugging
	 * every font glyph / the cursor sprite with texture packs); clamping the
	 * fractional part instead keeps the LINEAR footprint inside the fragment's
	 * own native texel — full hires detail within the cell, zero cross-cell
	 * bleed at cell edges. (0,0) = free linear, no clamp (no-override path). */
	"		vec2 uvn = v_texcoord.xy + u_texOffset;\n"\
	"		vec2 cell = floor(uvn);\n"\
	"		vec2 tc = (cell + clamp(uvn - cell, u_hiresHalf, vec2(1.0) - u_hiresHalf)) * texelSize;\n"\
	/* Filtering off for this prim class (0 = whole 2D/menu frame, 1 = 3D frame but
	 * this prim is 2D): point-sample the replacement at its OWN resolution. The
	 * texture object is LINEAR(+mips) for every upscaled replacement and that is
	 * fixed at upload time, so the snap has to happen here; textureLod pins the base
	 * level, or a minified HD atlas would still blur in through the mip chain. A
	 * native-res replacement already lands on the texel centre (u_hiresHalf == 0.5),
	 * so it comes out bit-identical either way. */
	"		if (bilinearFilter == 0 || (bilinearFilter == 1 && v_is3d < 0.5)) {\n"\
	"			vec2 hiresSize = vec2(textureSize(s_texture, 0));\n"\
	"			fragColor = textureLod(s_texture, (floor(tc * hiresSize) + 0.5) / hiresSize, 0.0);\n"\
	"		} else {\n"\
	"			fragColor = texture2D(s_texture, tc);\n"\
	"		}\n"\
	/* PSX colour-0 transparency for hi-res overrides: alpha 0 texels are
	 * holes on ANY prim (opaque prims ignore blending, so without the
	 * discard they'd render solid). 0.5 cutoff keeps authored soft-alpha
	 * edges blending on semi-transparent prims while opaque cutouts
	 * (foliage/UI) stay clean. */
	"		if (fragColor.a < 0.5) discard;\n"\
	GPU_LIT_TAIL\
	GPU_DITHERING_NO_VCOLOR\
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
	 * Uses noperspective interpolation qualifier (GLSL 1.30+, desktop only).
	 * centroid keeps UV interpolation inside the primitive under MSAA: edge
	 * samples otherwise extrapolate texcoords into the neighboring VRAM-atlas
	 * cell (the "weird lines"/light-texture artifacts, worst at 8x). Identical
	 * to center sampling when MSAA is off, so the non-MSAA image is unchanged. */
#if defined(ES2_SHADERS)
	#define SH_TC_CENTROID ""
#else
	#define SH_TC_CENTROID "centroid "
#endif
	if (g_cfg_affineTextures)
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING noperspective " SH_TC_CENTROID "varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING noperspective " SH_TC_CENTROID "varying\n");
	}
	else
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING " SH_TC_CENTROID "varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING " SH_TC_CENTROID "varying\n");
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
	GLint lutSampler = 2;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	/* Shaders without the decode table return location -1, where this is a no-op. */
	glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &lutSampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

static u_char s_rgLUT[LUT_WIDTH * LUT_HEIGHT * 4];

/* Bake the 16-bit-PSX -> RGBA table once with exact integer bit math, so the
 * shader can decode a texel by indexing it (lut()) instead of dividing and
 * flooring floats per pixel. Channels are stored as v<<3 rather than v so the
 * shader's v*8/256 lands on exactly v/32, matching the old decode bit-for-bit.
 * Ported from OpenDriver2/PsyCross GR_InitRG8LUT. */
void GR_InitRG8LUT()
{
	for (int y = 0; y < LUT_HEIGHT; y++)
	{
		u_char* row = s_rgLUT + y * (LUT_WIDTH * 4);
		for (int x = 0; x < LUT_WIDTH; x++)
		{
			const unsigned short c = (unsigned short)((y << 8) | x);
			u_char* pixel = row + x * 4;
			pixel[0] = (u_char)(((c)       & 31) << 3);
			pixel[1] = (u_char)(((c >>  5) & 31) << 3);
			pixel[2] = (u_char)(((c >> 10) & 31) << 3);
			pixel[3] = (u_char)(((c >> 15) &  1) << 7); // STP
		}
	}
}

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

	/* Unit 0 is the scene texture and unit 1 the shadow map, so the decode
	 * table takes unit 2. It is immutable, uploaded once here, and NEAREST —
	 * lut() indexes texel centres and any filtering would blend palette
	 * entries together. */
	GR_InitRG8LUT();
	glGenTextures(1, &g_rgLutTexture);
	{
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_rgLUT);

		glActiveTexture(GL_TEXTURE0);
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

static void GR_InitialisePSXShader(GTEShader* sh, ShaderID shader)
{
	sh->shader = shader;

#if USE_OPENGL
	
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->ditherForceLoc = glGetUniformLocation(sh->shader, "u_ditherForce");
	sh->pixelScaleLoc = glGetUniformLocation(sh->shader, "u_pixelScale");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->texOffsetLoc = glGetUniformLocation(sh->shader, "u_texOffset");
	sh->hiresHalfLoc = glGetUniformLocation(sh->shader, "u_hiresHalf");
	sh->fogColorLoc = glGetUniformLocation(sh->shader, "u_fogColor");
	sh->fogToBlackLoc = glGetUniformLocation(sh->shader, "u_fogToBlack");
	sh->fogStrengthLoc = glGetUniformLocation(sh->shader, "u_fogStrength");
	sh->pgxpEnabledLoc = glGetUniformLocation(sh->shader, "u_pgxpEnabled");
	sh->szMaxLoc = glGetUniformLocation(sh->shader, "u_szMax");
	sh->worldFarBiasLoc = glGetUniformLocation(sh->shader, "u_worldFarBias");
	sh->pgxpFarWLoc = glGetUniformLocation(sh->shader, "u_pgxpFarW");
	sh->flashlightOnLoc = glGetUniformLocation(sh->shader, "u_flashlightOn");
	sh->untexturedLoc = glGetUniformLocation(sh->shader, "u_untextured");
	sh->flStyleLoc = glGetUniformLocation(sh->shader, "u_flStyle");
	sh->flLightPosLoc = glGetUniformLocation(sh->shader, "u_flLightPos");
	sh->flDirLoc = glGetUniformLocation(sh->shader, "u_flDir");
	sh->flColorLoc = glGetUniformLocation(sh->shader, "u_flColor");
	sh->flInnerCosLoc = glGetUniformLocation(sh->shader, "u_flInnerCos");
	sh->flOuterCosLoc = glGetUniformLocation(sh->shader, "u_flOuterCos");
	sh->flRangeLoc = glGetUniformLocation(sh->shader, "u_flRange");
	sh->shadowOnLoc = glGetUniformLocation(sh->shader, "u_shadowOn");
	sh->shadowMatrixLoc = glGetUniformLocation(sh->shader, "u_shadowMatrix");
	sh->shadowBiasLoc = glGetUniformLocation(sh->shader, "u_shadowBias");
	sh->shadowTexelLoc = glGetUniformLocation(sh->shader, "u_shadowTexel");
	sh->shadowNormalOffsetLoc = glGetUniformLocation(sh->shader, "u_shadowNormalOffset");
	sh->shadowStrengthLoc = glGetUniformLocation(sh->shader, "u_shadowStrength");
	sh->shadowClipLoc = glGetUniformLocation(sh->shader, "u_shadowClip");
	sh->shadowFadeDistLoc = glGetUniformLocation(sh->shader, "u_shadowFadeDist");

	/* Shadow map lives on texture unit 1 (scene textures use unit 0). Bind the
	 * sampler once here; the depth texture is bound to GL_TEXTURE1 each frame. */
	{
		GLint prevProg = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
		GLint sloc = glGetUniformLocation(sh->shader, "u_shadowTex");
		if (sloc != -1)
		{
			glUseProgram(sh->shader);
			glUniform1i(sloc, 1);
			glUseProgram(prevProg);
		}
	}
#endif
}

void GR_CompilePSXShader(GTEShader* sh, const char* source)
{
	GR_InitialisePSXShader(sh, GR_Shader_Compile(source));
}

extern "C" void Pc_ModernShader_Initialise(const char*, const char*, const char*, const char*);
extern "C" ShaderID Pc_ModernShader_Get(TexFormat);

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gte_shader_4, gte_shader_4);
	GR_CompilePSXShader(&g_gte_shader_8, gte_shader_8);
	GR_CompilePSXShader(&g_gte_shader_16, gte_shader_16);
	GR_CompilePSXShader(&g_gte_shader_32_rgba, gte_shader_32_rgba);
	Pc_ModernShader_Initialise(gte_shader_4, gte_shader_8, gte_shader_16, gte_shader_32_rgba);
	GR_InitialisePSXShader(&g_modern_shader_4, Pc_ModernShader_Get(TF_4_BIT));
	GR_InitialisePSXShader(&g_modern_shader_8, Pc_ModernShader_Get(TF_8_BIT));
	GR_InitialisePSXShader(&g_modern_shader_16, Pc_ModernShader_Get(TF_16_BIT));
	GR_InitialisePSXShader(&g_modern_shader_32_rgba, Pc_ModernShader_Get(TF_32_BIT_RGBA));
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


static void GR_SetTextureShader(TextureID texture, TexFormat texFormat, GTEShader* shader)
{
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_8_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_16_BIT:
		GR_SetShader(shader->shader);
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = shader->pixelScaleLoc;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = -1;
		u_texOffsetLoc = -1;
		u_hiresHalfLoc = -1;
		u_fogColorLoc = shader->fogColorLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(shader->shader);
		/* Upstream 5b70144: the 32-bit RGBA (hi-res override) path now honours
		 * menu_filter, so this must be the shader's REAL bilinearFilter location
		 * rather than -1. Read it off the parameterised shader so the modern-mesh
		 * variants get their own location (or -1 if absent, which the guarded
		 * glUniform1i below tolerates). */
		u_bilinearFilterLoc = shader->bilinearFilterLoc;
		u_ditherForceLoc = shader->ditherForceLoc;
		u_pixelScaleLoc = -1;
		u_projectionLoc = shader->projectionLoc;
		u_projection3DLoc = shader->projection3DLoc;
		u_texelSizeLoc = shader->texelSizeLoc;
		u_texOffsetLoc = shader->texOffsetLoc;
		u_hiresHalfLoc = shader->hiresHalfLoc;
		u_fogColorLoc = shader->fogColorLoc;
		u_fogToBlackLoc = shader->fogToBlackLoc;
		u_fogStrengthLoc = shader->fogStrengthLoc;
		u_pgxpEnabledLoc = shader->pgxpEnabledLoc;
		u_szMaxLoc = shader->szMaxLoc;
		u_worldFarBiasLoc = shader->worldFarBiasLoc;
		u_pgxpFarWLoc = shader->pgxpFarWLoc;
		u_flashlightOnLoc = shader->flashlightOnLoc;
		u_untexturedLoc = shader->untexturedLoc;
		u_flStyleLoc = shader->flStyleLoc;
		u_flLightPosLoc = shader->flLightPosLoc;
		u_flDirLoc = shader->flDirLoc;
		u_flColorLoc = shader->flColorLoc;
		u_flInnerCosLoc = shader->flInnerCosLoc;
		u_flOuterCosLoc = shader->flOuterCosLoc;
		u_flRangeLoc = shader->flRangeLoc;
		u_shadowOnLoc = shader->shadowOnLoc;
		u_shadowMatrixLoc = shader->shadowMatrixLoc;
		u_shadowBiasLoc = shader->shadowBiasLoc;
		u_shadowTexelLoc = shader->shadowTexelLoc;
		u_shadowNormalOffsetLoc = shader->shadowNormalOffsetLoc;
		u_shadowStrengthLoc = shader->shadowStrengthLoc;
		u_shadowClipLoc = shader->shadowClipLoc;
		u_shadowFadeDistLoc = shader->shadowFadeDistLoc;
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
	if (u_worldFarBiasLoc != -1)
		glUniform1f(u_worldFarBiasLoc, (float)g_PsxPgxpWorldFarBias);
	{
		extern float g_PgxpFarWClamp;
		if (u_pgxpFarWLoc != -1)
			glUniform1f(u_pgxpFarWLoc, g_PgxpFarWClamp);
	}

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
	/* Untextured splits bind g_whiteTexture; tell the lit tail so the beam's
	 * albedo falls back to the vertex-lit color instead of full white. Keyed on
	 * the caller's texture, not g_lastBoundTexture — this block runs before the
	 * bind early-out, so it stays correct across program switches. */
	if (u_untexturedLoc != -1)
		glUniform1i(u_untexturedLoc, texture == g_whiteTexture ? 1 : 0);
	if (u_flStyleLoc != -1)
		glUniform1i(u_flStyleLoc, g_PsyX_FlashlightStyle ? 1 : 0);
	if (u_flLightPosLoc != -1)
		glUniform3fv(u_flLightPosLoc, 1, g_PsyX_FlashlightPos);
	if (u_flDirLoc != -1)
		glUniform3fv(u_flDirLoc, 1, g_PsyX_FlashlightDir);
	/* FPS mode swaps in its own (tighter/dimmer) cone size + brightness. */
	float flIntensityActive = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightIntensityFps : g_PsyX_FlashlightIntensity;
	float flSizeActive      = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightSizeFps      : g_PsyX_FlashlightSize;
	if (u_flColorLoc != -1) {
		float flCol[3];
		flCol[0] = g_PsyX_FlashlightColor[0] * flIntensityActive;
		flCol[1] = g_PsyX_FlashlightColor[1] * flIntensityActive;
		flCol[2] = g_PsyX_FlashlightColor[2] * flIntensityActive;
		glUniform3fv(u_flColorLoc, 1, flCol);
	}
	/* Scale cone coverage by g_PsyX_FlashlightSize: a cone's solid angle is
	 * ~proportional to (1 - cos(halfAngle)), so scaling that term scales the lit
	 * area ~linearly (size 1.0 = base, 1.5 = ~1.5x). Inner stays the tighter
	 * (higher-cos) angle; the 0.05 floor keeps the half-angle under 90 deg. */
	{
		/* Modern style keeps its pre-calibration ~35 deg base cone; the wider
		 * 0.76 default belongs to the classic (PSX-matched) style. */
		float baseOuterCos = g_PsyX_FlashlightStyle ? g_PsyX_FlashlightOuterCos : 0.82f;
		float flInner = 1.0f - flSizeActive * (1.0f - g_PsyX_FlashlightInnerCos);
		float flOuter = 1.0f - flSizeActive * (1.0f - baseOuterCos);
		if (flInner < 0.05f) flInner = 0.05f;
		if (flOuter < 0.05f) flOuter = 0.05f;
		if (u_flInnerCosLoc != -1)
			glUniform1f(u_flInnerCosLoc, flInner);
		if (u_flOuterCosLoc != -1)
			glUniform1f(u_flOuterCosLoc, flOuter);
	}
	if (u_flRangeLoc != -1)
		glUniform1f(u_flRangeLoc, g_PsyX_FlashlightRange);

	/* Flashlight shadow map: same gate as the depth pre-pass in DrawAllSplits, so the
	 * shader only samples the shadow texture on frames one was actually rendered. */
	{
		int shadowOn = (g_PsyX_UseFlashlightShadows && g_PsyX_UsePerPixelFlashlight &&
		                g_PsyX_FlashlightActive && g_shadowDepthTex != 0 &&
		                g_PsyX_ShadowsAllowed && !g_PsxPresentLastFrame) ? 1 : 0;
		if (u_shadowOnLoc != -1)
			glUniform1i(u_shadowOnLoc, shadowOn);
		if (shadowOn)
		{
			if (u_shadowMatrixLoc != -1)
				glUniformMatrix4fv(u_shadowMatrixLoc, 1, GL_FALSE, g_shadowLightMatrix);
			if (u_shadowBiasLoc != -1)
				glUniform1f(u_shadowBiasLoc, g_PsyX_FlashlightShadowBias);
			if (u_shadowNormalOffsetLoc != -1)
				glUniform1f(u_shadowNormalOffsetLoc, g_PsyX_FlashlightShadowNormalOffset);
			if (u_shadowStrengthLoc != -1)
				glUniform1f(u_shadowStrengthLoc, g_PsyX_FlashlightShadowStrength);
			if (u_shadowClipLoc != -1)
				glUniform2f(u_shadowClipLoc, g_shadowZNear, g_shadowZFar);
			if (u_shadowFadeDistLoc != -1)
				glUniform1f(u_shadowFadeDistLoc, g_PsyX_FlashlightShadowFadeDist);
			if (u_shadowTexelLoc != -1)
				glUniform2f(u_shadowTexelLoc, 1.0f / (float)PSYX_SHADOW_MAP_SIZE, 1.0f / (float)PSYX_SHADOW_MAP_SIZE);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, g_shadowDepthTex);
			glActiveTexture(GL_TEXTURE0);
		}
	}

	/* Re-assert the decode table on unit 2, like the shadow map above: the
	 * upload in GR_GenerateCommonTextures is a one-shot, and a pass that ever
	 * leaves another texture on that unit would otherwise decode every texel
	 * through it. Cheap next to the uniform pushes below, and it restores
	 * unit 0 so nothing downstream inherits a stray active unit. */
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);
	glActiveTexture(GL_TEXTURE0);

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

	/* Filtering is a per-FRAME decision (the frame class flips with
	 * g_PsxDitherSuppressed), so push it on every bind like the two above —
	 * NOT below the g_lastBoundTexture early-out, where it used to sit. A menu
	 * entered from gameplay re-binds the same VRAM texture, so the early-out
	 * skipped the push and the whole screen kept running on the 3D value.
	 *
	 * VRAM samplers: 1 = 3D bilinear (v_is3d-gated), 2 = menu/2D-frame filter
	 * (ignores v_is3d, independent of psx_dither), 0 = point-sample.
	 *
	 * The hi-res override sampler decodes nothing: it reads s_texture through GL
	 * sampler state, which upload_rgba fixes at LINEAR(+mips) for any replacement
	 * larger than native and never revisits — so neither toggle could reach it and
	 * HD menu/map art stayed bilinear with menu_filter off. It gets 1 on 3D frames
	 * unconditionally so world texture packs keep sampling exactly as before,
	 * while its 2D prims follow the same v_is3d gate the VRAM path uses. */
	if (u_bilinearFilterLoc != -1)
		glUniform1i(u_bilinearFilterLoc,
		            g_PsxDitherSuppressed
		                ? (g_cfg_menuFilter ? 2 : 0)
		                : ((texFormat == TF_32_BIT_RGBA || g_cfg_bilinearFiltering) ? 1 : 0));

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glBindTexture(GL_TEXTURE_2D, texture);
#endif

	g_lastBoundTexture = texture;
}

void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	GTEShader* shader;
	if (texFormat == TF_4_BIT) shader = &g_gte_shader_4;
	else if (texFormat == TF_8_BIT) shader = &g_gte_shader_8;
	else if (texFormat == TF_16_BIT) shader = &g_gte_shader_16;
	else shader = &g_gte_shader_32_rgba;
	GR_SetTextureShader(texture, texFormat, shader);
}

void GR_SetOverrideTextureSize(int width, int height, int offsetX, int offsetY,
                               int hiresW, int hiresH)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version.
	// width/height are a pool slot's nativeW/nativeH; a slot whose registration
	// aborted mid-way (an Intel-HD-4600 upload failure) leaves nativeW==0, so
	// 1.0/width == +Inf -> NaN texture coords. NaN sampling is implementation-
	// defined (Intel returns arbitrary texels: the half-screen garbage quad), so
	// clamp the divisor to >=1 to keep coords finite. Valid slots are unaffected.
	float vec[] = { 1.0f / (float)(width  > 0 ? width  : 1),
	                1.0f / (float)(height > 0 ? height : 1) };
	glUniform2fv(u_texelSizeLoc, 1, vec);

	if(u_texOffsetLoc != -1)
	{
		float ofs[] = { (float)offsetX, (float)offsetY };
		glUniform2fv(u_texOffsetLoc, 1, ofs);
	}

	if(u_hiresHalfLoc != -1)
	{
		/* Half a hires texel in native-texel units; the fragment shader clamps
		 * its LINEAR footprint inside each native texel with this. Capped at
		 * 0.5 (native-res replacement = pure texel-center sampling); 0 when the
		 * hires size is unknown (legacy DR_PSYX_TEX path) = no clamp. */
		float hx = (hiresW > 0 && width  > 0) ? 0.5f * (float)width  / (float)hiresW : 0.0f;
		float hy = (hiresH > 0 && height > 0) ? 0.5f * (float)height / (float)hiresH : 0.0f;
		float hh[2];
		hh[0] = (hx > 0.5f) ? 0.5f : hx;
		hh[1] = (hy > 0.5f) ? 0.5f : hy;
		glUniform2fv(u_hiresHalfLoc, 1, hh);
	}
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

	/* The only writer of g_PreviousFramebuffer (GR_StoreFrameBuffer) is
	 * compiled out under PSYX_SKIP_FRAMEBUFFER_STORE, so this rect is 0x0 in
	 * the PC-port build — the 2 MB glGetTexImage + glMapBuffer readback below
	 * ran every frame only to feed a zero-iteration copy. Near-free on modern
	 * drivers (async PBO DMA); a synchronous full-pipeline flush per frame on
	 * the old-Intel GL 3.1 tier — the reported 10fps class. */
	if (w <= 0 || h <= 0)
		return;

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

/* PC port: map a window-pixel point to a [0,1] fraction of the letterboxed 4:3
 * display viewport — the exact pillarbox rect GR_SetOffscreenState installs
 * below. Used by the mouse-cursor feature to convert an OS cursor position into
 * the game's 2D space. Returns 1 if the point is inside the viewport (0 = in the
 * black bars). Mirrors the viewport block near "Display viewport" below; keep in
 * sync if that math changes. */
extern "C" int PsyX_MapWindowToViewport(int mx, int my, float* outFracX, float* outFracY)
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
	const float fx = (vpW > 0) ? (float)(mx - vpX) / (float)vpW : 0.0f;
	const float fy = (vpH > 0) ? (float)(my - vpY) / (float)vpH : 0.0f;
	if (outFracX) *outFracX = fx;
	if (outFracY) *outFracY = fy;
	return (fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f) ? 1 : 0;
}

/* PC port: the window sub-rect the game is actually presented into (pillarbox
 * leaves black bars at the sides). The framebuffer-feedback capture must read
 * THIS, not the whole window: squashing the full window into the 320x224
 * feedback buffer shrinks the image horizontally by vpW/windowWidth, and because
 * the effect re-reads its own output every frame that scale compounds — the
 * picture collapses toward the centre and builds up a vertical line after a few
 * seconds. Recorded from the same block that calls GR_SetViewPort so the two can
 * never disagree. */
static int g_presentVp[4] = { 0, 0, 0, 0 };

/* Window rect (GL coords, x0/y0/x1/y1, origin bottom-left) that the PSX display
 * buffer occupies under the world ortho. Set alongside g_presentVp; this is what
 * the framebuffer-feedback capture reads, so capture and redraw are 1:1. */
static float g_psxAreaVp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static int   g_psxAreaVpValid = 0;

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

		/* The ortho actually installed below, kept in this scope so the viewport
		 * block can work out where the PSX display buffer lands in the window
		 * (g_psxAreaVp — the framebuffer-feedback capture source). */
		float fbOrthoL = 0.0f, fbOrthoR = 0.0f, fbOrthoT = 0.0f, fbOrthoB = 0.0f;
		float fbPsxW = 0.0f, fbPsxH = 0.0f;

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
				 * ortho so it isn't scaled/clipped off the bottom. The FIX_ANG framing shift
				 * (g_PsxWorldVShift) is applied at the GTE projection center by the game, not
				 * here — an ortho-window shift reveals rows overlay prims never cover. */
				const float vscale = g_PsxUIOrthoPass ? 1.0f : g_PsxWorldVScale;
				orthoTop = 0.0f;
				orthoBot = psxH * vscale;
			}
			const float psxAspect = psxW / psxH;
			const float winAspect = (g_windowHeight > 0)
				? ((float)g_windowWidth / (float)g_windowHeight)
				: psxAspect;
			const float horScale = winAspect / psxAspect;
			fbPsxW   = psxW;
			fbPsxH   = (float)activeDispEnv.disp.h;
			fbOrthoT = orthoTop;
			fbOrthoB = orthoBot;
			fbOrthoL = 0.0f;
			fbOrthoR = psxW;
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
				/* widescreen_zoom: scale both axes around center. Keeps the
				 * original (bottom, top) argument order of GR_Ortho2D. */
				const float zoom = (g_PsxUIOrthoPass || g_PcWidescreenZoom <= 0.0f)
					? 1.0f : g_PcWidescreenZoom;
				const float halfH = (orthoBot - orthoTop) * 0.5f / zoom;
				const float cy    = (orthoTop + orthoBot) * 0.5f;
				fbOrthoL = cx - halfW / zoom;
				fbOrthoR = cx + halfW / zoom;
				GR_Ortho2D(cx - halfW / zoom, cx + halfW / zoom,
				          cy + halfH, cy - halfH, -1.0f, 1.0f);
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

			/* Presented sub-rect of the window (pillarbox bars excluded). */
			g_presentVp[0] = vpX; g_presentVp[1] = vpY;
			g_presentVp[2] = vpW; g_presentVp[3] = vpH;

			/* Where the PSX display buffer [0,dispW]x[0,dispH] actually lands in
			 * the window under THIS ortho — the source rect the feedback capture
			 * must read. It is NOT the viewport: Hor+ widens the ortho past the
			 * buffer (the 4:3 core covers only 1/effectiveScale of the window) and
			 * g_PsxWorldVScale crops it vertically, so capturing the viewport
			 * rescales the picture every frame. The blur redraws the captured rect
			 * over exactly these PSX coordinates, so any mismatch compounds — see
			 * GR_StoreFrameBufferPsx. Recorded from the WORLD pass only, because
			 * that is the ortho the blur's OT0 prims are drawn under.
			 * GL window coords, origin bottom-left: ortho y=orthoTop is the TOP. */
			if (!g_PsxUIOrthoPass && fbOrthoR > fbOrthoL && fbOrthoB > fbOrthoT)
			{
				const float sx = (float)vpW / (fbOrthoR - fbOrthoL);
				const float sy = (float)vpH / (fbOrthoB - fbOrthoT);
				g_psxAreaVp[0] = (float)vpX + (0.0f    - fbOrthoL) * sx;
				g_psxAreaVp[2] = (float)vpX + (fbPsxW  - fbOrthoL) * sx;
				g_psxAreaVp[3] = (float)(vpY + vpH) - (0.0f   - fbOrthoT) * sy;
				g_psxAreaVp[1] = (float)(vpY + vpH) - (fbPsxH - fbOrthoT) * sy;
				g_psxAreaVpValid = 1;
			}
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
		else if (g_PreviousOffscreen.w > 0 && g_PreviousOffscreen.h > 0 &&
		         g_PreviousOffscreen.w <= 64 && g_PreviousOffscreen.h <= 64)
		{
			/* Small VRAM-scratch writeback (sewer water caustic 32x32): the
			 * legacy path below is wrong for these — the raw glBlitFramebuffer
			 * writes unpacked RGBA into the packed-RG8 VRAM texture (corrupts
			 * it), and the full-size PBO adds 2 frames of latency. A scratch
			 * this small is a synchronous glReadPixels of a few KB: read the
			 * FBO, pack into vram[] (kept authoritative so any later full
			 * GR_UpdateVRAM upload re-stamps the same bytes), then sub-upload
			 * the packed region into BOTH double-buffered VRAM textures.
			 * vram_need_update is NOT set — no 1MB full re-upload per frame. */
			static u_int  s_scratchRGBA[64 * 64];
			static ushort s_scratchPacked[64 * 64];
			const int sw = g_PreviousOffscreen.w, sh = g_PreviousOffscreen.h;

			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);
			glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, s_scratchRGBA);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			GR_CopyRGBAFramebufferToVRAM(s_scratchRGBA,
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, sw, sh, 0, 1);

			{
				const ushort* src = (const ushort*)vram +
					VRAM_WIDTH * g_PreviousOffscreen.y + g_PreviousOffscreen.x;
				for (int row = 0; row < sh; row++)
					memcpy(&s_scratchPacked[row * sw], src + row * VRAM_WIDTH, sw * sizeof(ushort));

				for (int t = 0; t < 2; t++)
				{
					glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[t]);
					glTexSubImage2D(GL_TEXTURE_2D, 0,
						g_PreviousOffscreen.x, g_PreviousOffscreen.y, sw, sh,
						VRAM_FORMAT, GL_UNSIGNED_BYTE, s_scratchPacked);
				}
				glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);
			}
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
static GLint    g_postLoc_bright = -1;
static GLint    g_postLoc_contrast = -1;
static GLint    g_postLoc_satur = -1;
static GLint    g_postLoc_calib = -1;

/* Set nonzero by the Brightness screen so GR_PostProcess overlays the reference
 * bar; cleared when it exits. */
extern "C" { int g_cfg_calibBar = 0; }
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
	"uniform float u_brightness;\n"     /* image adjust, 1.0 = neutral */
	"uniform float u_contrast;\n"
	"uniform float u_saturation;\n"
	"uniform int   u_calibBar;\n"       /* 1 = overlay the reference bar (Brightness screen) */
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
	/* Reference bar (Brightness screen): a black->white ramp across the top with a
	 * primary-colour strip beneath it, so brightness/contrast/saturation changes
	 * are visible against a known target. Drawn BEFORE the adjustments so the bar
	 * is graded exactly like the game image. */
	"	if (u_calibBar == 1) {\n"
	"		if (uv.y < 0.09) { float g = clamp(uv.x, 0.0, 1.0); col = vec3(g); }\n"
	"		else if (uv.y < 0.15) {\n"
	"			float s = uv.x * 6.0;\n"
	"			if      (s < 1.0) col = vec3(1.0, 0.0, 0.0);\n"
	"			else if (s < 2.0) col = vec3(0.0, 1.0, 0.0);\n"
	"			else if (s < 3.0) col = vec3(0.0, 0.0, 1.0);\n"
	"			else if (s < 4.0) col = vec3(1.0, 1.0, 0.0);\n"
	"			else if (s < 5.0) col = vec3(0.0, 1.0, 1.0);\n"
	"			else              col = vec3(1.0, 0.0, 1.0);\n"
	"		}\n"
	"	}\n"
	/* Image adjustments (always applied; 1.0 = neutral each). Saturation toward
	 * Rec.601 luma, contrast around mid-grey, then brightness scale. */
	"	{\n"
	"		float luma = dot(col, vec3(0.299, 0.587, 0.114));\n"
	"		col = mix(vec3(luma), col, u_saturation);\n"
	"		col = (col - 0.5) * u_contrast + 0.5;\n"
	"		col *= u_brightness;\n"
	"	}\n"
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
	g_postLoc_bright   = glGetUniformLocation(g_postShader, "u_brightness");
	g_postLoc_contrast = glGetUniformLocation(g_postShader, "u_contrast");
	g_postLoc_satur    = glGetUniformLocation(g_postShader, "u_saturation");
	g_postLoc_calib    = glGetUniformLocation(g_postShader, "u_calibBar");

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
	if (g_postLoc_bright != -1)
		glUniform1f(g_postLoc_bright, g_cfg_brightness);
	if (g_postLoc_contrast != -1)
		glUniform1f(g_postLoc_contrast, g_cfg_contrast);
	if (g_postLoc_satur != -1)
		glUniform1f(g_postLoc_satur, g_cfg_saturation);
	if (g_postLoc_calib != -1)
		glUniform1i(g_postLoc_calib, g_cfg_calibBar);

	glBindTexture(GL_TEXTURE_2D, tex);
	glBindVertexArray(g_postVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);

	/* Sentinels, NOT the real state. Recording the truth (BM_NONE/0) is what
	 * strands depth off: GR_EnableDepth is reachable only from inside
	 * GR_SetBlendMode's body, and that early-returns on an unchanged mode, so a
	 * following opaque draw matches BM_NONE, skips the body, and never re-applies
	 * the depth test this function just disabled. Cost the take-screen item its
	 * depth test entirely (antenna through the radio body), because the take
	 * screen composites over the frozen scene through this path while the
	 * inventory does not. Same reasoning as GR_ShadowPassEnd. */
	g_PreviousShader      = (ShaderID)-1;
	g_lastBoundTexture    = (TextureID)-1;
	g_PreviousBlendMode   = -999;
	g_PreviousDepthMode   = -999;
	g_PreviousScissorState = -999;
}

/* PC port: post-process the composed backbuffer in place. Resolves the (possibly
 * multisample) default framebuffer into a single-sample texture, then redraws it
 * full-screen through the selected look. No-op when g_cfg_postProcess <= 0. */
void GR_PostProcess(void)
{
	/* Also run when an image adjustment is off-neutral or the calibration bar is
	 * requested, so brightness/contrast/saturation apply with no filter selected. */
	const int bcsActive = (g_cfg_brightness != 1.0f) || (g_cfg_contrast != 1.0f) ||
	                      (g_cfg_saturation != 1.0f) || (g_cfg_calibBar != 0);
	if (g_cfg_postProcess <= 0 && g_cfg_tonemap <= 0 && !bcsActive)
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

/* ------------------------------------------------------------------------
 * Flashlight shadow mapping (see g_PsyX_UseFlashlightShadows).
 *
 * Everything is done in the engine's CAMERA VIEW space: GrVertex carries
 * a_viewpos (view space) and the flashlight pos/dir are already pushed in
 * view space, so the light matrix is built purely from those with no
 * world-space / camera-inverse step. Each frame the opaque geometry is
 * rendered depth-only from the light POV into g_shadowDepthTex; the cone
 * fragment shader samples it. Feature is a no-op (byte-identical output)
 * when the master flag is off.
 * ---------------------------------------------------------------------- */
#if USE_OPENGL

static const char* s_shadowDepthShaderSrc =
	"#ifdef VERTEX\n"
	"attribute vec3 a_viewpos;\n"
	"attribute vec3 a_normal;\n"
	"uniform mat4 u_shadowMatrix;\n"
	"void main() {\n"
	/* a_normal.y marks a validated view-space entry; a_normal.x suppresses casting. */
	"	if (a_normal.y < 0.5 || a_viewpos.z <= 0.0 || a_normal.x > 0.5) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }\n"
	"	gl_Position = u_shadowMatrix * vec4(a_viewpos, 1.0);\n"
	"}\n"
	"#else\n"
	"void main() { }\n"
	"#endif\n";

static void sh_normalize3(float* v)
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static float sh_dot3(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void sh_cross(const float* a, const float* b, float* r)
{
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
}

/* Column-major (GL) perspective + look-at + multiply. */
static void sh_perspective(float fovy, float aspect, float zn, float zf, float* m)
{
	float f = 1.0f / tanf(fovy * 0.5f);
	for (int i = 0; i < 16; i++) m[i] = 0.0f;
	m[0]  = f / aspect;
	m[5]  = f;
	m[10] = (zf + zn) / (zn - zf);
	m[11] = -1.0f;
	m[14] = (2.0f * zf * zn) / (zn - zf);
}

static void sh_lookat(const float* eye, const float* center, const float* up, float* m)
{
	float f[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
	sh_normalize3(f);
	float s[3]; sh_cross(f, up, s); sh_normalize3(s);
	float u[3]; sh_cross(s, f, u);
	m[0] = s[0];  m[4] = s[1];  m[8]  = s[2];  m[12] = -sh_dot3(s, eye);
	m[1] = u[0];  m[5] = u[1];  m[9]  = u[2];  m[13] = -sh_dot3(u, eye);
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = sh_dot3(f, eye);
	m[3] = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;  m[15] = 1.0f;
}

static void sh_mul(const float* a, const float* b, float* r)  /* r = a * b */
{
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
			                 a[1 * 4 + row] * b[c * 4 + 1] +
			                 a[2 * 4 + row] * b[c * 4 + 2] +
			                 a[3 * 4 + row] * b[c * 4 + 3];
}

static void GR_EnsureShadowTarget(void)
{
	if (g_shadowFBO != 0)
		return;

	glGenTextures(1, &g_shadowDepthTex);
	glBindTexture(GL_TEXTURE_2D, g_shadowDepthTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, PSYX_SHADOW_MAP_SIZE, PSYX_SHADOW_MAP_SIZE,
	             0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	{
		float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  /* outside the light frustum = fully lit */
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &g_shadowFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, g_shadowFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_shadowDepthTex, 0);
#if !defined(RENDERER_OGLES)
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
#endif
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (g_shadowDepthShader == (ShaderID)-1)
	{
		g_shadowDepthShader = GR_Shader_Compile(s_shadowDepthShaderSrc);
		g_shadowDepthMatrixLoc = glGetUniformLocation(g_shadowDepthShader, "u_shadowMatrix");
	}
}

static void GR_BuildShadowMatrix(void)
{
	float sizeActive = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightSizeFps : g_PsyX_FlashlightSize;
	float flOuter = 1.0f - sizeActive * (1.0f - g_PsyX_FlashlightOuterCos);
	if (flOuter < 0.05f)  flOuter = 0.05f;
	if (flOuter > 0.999f) flOuter = 0.999f;

	float fov = acosf(flOuter) * 2.0f * 1.25f;  /* widen past the cone so edge shadows aren't clipped */
	if (fov > 2.9f) fov = 2.9f;
	if (fov < 0.2f) fov = 0.2f;

	float zn = 20.0f;
	float zf = g_PsyX_FlashlightRange * 1.3f;
	if (zf < zn + 1.0f) zf = zn + 1.0f;
	g_shadowZNear = zn;
	g_shadowZFar  = zf;

	/* FPS pins the CONE at the eye (so the beam follows the view), but the shadow
	 * must originate at the REAL light (chest/hand) — else the depth map is the
	 * camera's own view and the shadow either vanishes or floats beside the object.
	 * g_PsyX_FlashlightShadowPos carries that true light position (== FlashlightPos
	 * in TPS). */
	const float* srcPos = g_PsyX_FlashlightFpsMode ? g_PsyX_FlashlightShadowPos : g_PsyX_FlashlightPos;
	float eye[3] = { srcPos[0], srcPos[1], srcPos[2] };
	float dir[3] = { g_PsyX_FlashlightDir[0], g_PsyX_FlashlightDir[1], g_PsyX_FlashlightDir[2] };
	sh_normalize3(dir);
	/* Optional fine-tune: nudge the shadow light back along -viewDir. Default 0
	 * (pure physical light position); `shadowfpsdrop` lets the user dial extra
	 * parallax if a scene wants it. */
	if (g_PsyX_FlashlightFpsMode && g_PsyX_FlashlightShadowFpsDrop != 0.0f)
	{
		eye[0] -= dir[0] * g_PsyX_FlashlightShadowFpsDrop;
		eye[1] -= dir[1] * g_PsyX_FlashlightShadowFpsDrop;
		eye[2] -= dir[2] * g_PsyX_FlashlightShadowFpsDrop;
	}
	float center[3] = { eye[0] + dir[0], eye[1] + dir[1], eye[2] + dir[2] };

	float up[3] = { 0.0f, 1.0f, 0.0f };
	if (fabsf(sh_dot3(dir, up)) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f; }

	float proj[16], view[16];
	sh_perspective(fov, 1.0f, zn, zf, proj);
	sh_lookat(eye, center, up, view);
	sh_mul(proj, view, g_shadowLightMatrix);
}

int GR_FlashlightShadowActive(void)
{
	/* g_PsyX_ShadowsAllowed is re-armed by the game only during settled gameplay
	 * (see its definition). Outside that — menus, room-load fades, cutscenes and
	 * frozen/transition frames (g_PsxPresentLastFrame) — the light-POV depth
	 * pre-pass corrupts unrelated rendering (white flash on room/inventory/map
	 * transitions, Harry's face dropping out on the options screen). Shadows are a
	 * live-gameplay-only effect. */
	return (g_PsyX_UseFlashlightShadows && g_PsyX_UsePerPixelFlashlight &&
	        g_PsyX_FlashlightActive && g_PsyX_ShadowsAllowed &&
	        !g_PsxPresentLastFrame) ? 1 : 0;
}

static GLint s_shadowPrevFBO = 0;
static GLint s_shadowPrevViewport[4] = { 0, 0, 0, 0 };

void GR_ShadowPassBegin(void)
{
	GR_EnsureShadowTarget();
	GR_BuildShadowMatrix();

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_shadowPrevFBO);
	glGetIntegerv(GL_VIEWPORT, s_shadowPrevViewport);

	glBindFramebuffer(GL_FRAMEBUFFER, g_shadowFBO);
	glViewport(0, 0, PSYX_SHADOW_MAP_SIZE, PSYX_SHADOW_MAP_SIZE);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glClear(GL_DEPTH_BUFFER_BIT);

	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.5f, 1.0f);

	glUseProgram(g_shadowDepthShader);
	if (g_shadowDepthMatrixLoc != -1)
		glUniformMatrix4fv(g_shadowDepthMatrixLoc, 1, GL_FALSE, g_shadowLightMatrix);
}

void GR_ShadowPassDraw(int startVertex, int numVerts)
{
	glDrawArrays(GL_TRIANGLES, startVertex, numVerts);
}

void GR_ShadowPassEnd(void)
{
	glDisable(GL_POLYGON_OFFSET_FILL);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s_shadowPrevFBO);
	glViewport(s_shadowPrevViewport[0], s_shadowPrevViewport[1],
	           s_shadowPrevViewport[2], s_shadowPrevViewport[3]);
	glDepthFunc(GL_LEQUAL);  /* restore the renderer default; the pass set GL_LESS */

	/* We changed program/depth/blend/scissor/stencil. Sentinels that never equal a
	 * real mode force the first color split to fully re-establish GL state. */
	glUseProgram(0);
	g_PreviousShader       = (ShaderID)-1;
	g_lastBoundTexture     = (TextureID)-1;
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousDepthFuncAlways = 0; /* this fn just set glDepthFunc(GL_LEQUAL) */
	g_PreviousStencilMode  = -999;
	g_PreviousScissorState = -999;
	glEnable(GL_STENCIL_TEST);
}

#else  /* !USE_OPENGL */
int  GR_FlashlightShadowActive(void) { return 0; }
void GR_ShadowPassBegin(void) {}
void GR_ShadowPassDraw(int startVertex, int numVerts) { (void)startVertex; (void)numVerts; }
void GR_ShadowPassEnd(void) {}
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

/* PC port: some cutscenes (map4_s04 Lisa dream, map3_s02 sibling) redirect the
 * PSX draw-area into an offscreen VRAM scratch rect (e.g. (320,256) 320x224),
 * render the frame THERE, and repaint the visible screen from it via 4
 * blend-layer SPRT strips — the dream-blur effect. GL never rasterizes into
 * VRAM, so on PC those strips sampled whatever TIMs happened to occupy the
 * scratch pages: a rainbow noise band, or a whole character atlas blown up on
 * screen, varying run to run with VRAM contents. ProcessDrawEnv latches the
 * redirect rect when a DR_AREA targets x >= 320 (display buffer columns are
 * always x < 320); while latched, the stored frame is blitted into the scratch
 * rect of g_vramTexture each present (and after every full VRAM re-upload), so
 * the strips composite the previous frame — the effect's actual source on PSX.
 * By PSX construction nothing else can live in the scene's scratch rect while
 * the scene runs, so the blit cannot clobber a live texture. */
static RECT16 g_sceneFbRedirect = { 0, 0, 0, 0 };
static int    g_sceneFbRedirectTtl = 0;
static int    s_sceneFbRedirectArms = 0;

extern "C" void GR_SetSceneFbRedirect(int x, int y, int w, int h)
{
	g_sceneFbRedirect.x = x;
	g_sceneFbRedirect.y = y;
	g_sceneFbRedirect.w = w;
	g_sceneFbRedirect.h = h;
	/* Refreshed every frame the scene submits its DR_AREA; a small TTL lets
	 * the blit die out a couple of presents after the scene stops. */
	/* Log every ARM-after-lapse, not just the first ever. The recurring cutscene
	 * rainbow bar is the strips sampling this rect while the blit is NOT live,
	 * and a once-per-session line cannot tell those runs apart: it proves the
	 * mechanism exists, never that it was active during the scene that
	 * corrupted. Paired with the lapse line below, a user log now says which.
	 * Rate-capped so a scene re-arming every frame cannot flood. */
	if (g_sceneFbRedirectTtl == 0 && s_sceneFbRedirectArms < 32)
	{
		s_sceneFbRedirectArms++;
		eprintinfo("[FBSCRATCH] redirect ARMED (%d,%d %dx%d) - feedback blit live\n", x, y, w, h);
	}

	g_sceneFbRedirectTtl = 3;
}

/* ===================== framebuffer feedback (packed RGB555) =================
 * Silent Hill DOES read rendered pixels back from VRAM: Screen_BackgroundMotionBlur
 * (the Harry-running loading-screen trail, every frame from GameBoot_LoadingScreen)
 * and the per-map ghosting/dream overlays all sample getTPage(2, 0, ...) display
 * pages. Those pages must therefore hold the previous frame.
 *
 * The catch that broke the first attempt at this: VRAM is a GL_RG8 texture whose
 * two channels hold the packed BYTES of a 16-bit PSX pixel — the sampling shader
 * decodes them by indexing s_rgLut with (high byte, low byte), a table baked as
 * bit 0-4 = R, 5-9 = G, 10-14 = B, 15 = mask; R channel = low byte, G = high.
 * A raw glBlitFramebuffer from the RGBA8 frame therefore CANNOT work: GL drops B/A
 * and keeps the R,G *colour* bytes, which the shader then decodes as a bit-packed
 * 555 word — saturated garbage stripes. (The CPU helper GR_CopyRGBAFramebufferToVRAM
 * packs R and B the wrong way round against this shader, so it is no use either.)
 *
 * So the store is a shader pass that packs RGBA8 -> RGB555 into RG, rendered
 * straight into the VRAM texture. Mask bit is left 0, so a pure-black source pixel
 * packs to word 0 and the sampler's zero-texel discard treats it as
 * transparent — exactly PSX texel-0 behaviour, which is what makes the loading
 * screen show a trail of Harry rather than an opaque black rectangle. */
/* Feedback-loop gain, pushed to the pack shader every store. 0.5 is the shipped
 * steady-state value; the door out-fade wants it near identity. Console: FBDAMP. */
extern "C" { float g_PsxFeedbackDamp = 0.5f; }

static ShaderID g_fbPackShader = (ShaderID)-1;
static GLuint   g_fbPackVAO = 0;
static GLuint   g_fbPackTex = 0;   /* captured frame, RGBA8 */
static GLuint   g_fbPackFBO = 0;
static int      g_fbPackW = 0, g_fbPackH = 0;
static int      g_fbPackValid = 0; /* a frame has been captured this session */

/* PSX display-buffer rects recorded from GsDefDispBuff2 (SH: (0,32)/(0,256),
 * 320x224). The PC libgs stub collapses both display envs to (0,0) because there
 * is no VRAM double-buffering here, so activeDispEnv.disp is NOT a usable store
 * target — it would land on the CLUT strip at y<32 (paper map at (224,15)). */
static RECT16 g_psxDispBuf[2] = { {0,0,0,0}, {0,0,0,0} };
static int    g_psxDispBufValid = 0;

/* Frames remaining for which the feedback store stands down because the game
 * uploaded its own data into a display-buffer rect (see GR_CopyVRAM). A few
 * frames of hysteresis so a scene that uploads intermittently doesn't flicker
 * the effect on between uploads. */
static int g_fbFeedbackSuppress = 0;

void GR_NoteVramUploadForFeedback(int x, int y, int w, int h)
{
	int i;

	if (!g_psxDispBufValid)
		return;

	for (i = 0; i < 2; i++)
	{
		const RECT16* b = &g_psxDispBuf[i];
		if (b->w <= 0 || b->h <= 0)
			continue;
		/* rect overlap */
		if (x < b->x + b->w && x + w > b->x &&
		    y < b->y + b->h && y + h > b->y)
		{
			static int s_logged = 0;
			if (!s_logged) {
				s_logged = 1;
				eprintinfo("[FBFEEDBACK] VRAM upload (%d,%d %dx%d) lands in display buffer %d "
					"(%d,%d %dx%d) — feedback store standing down\n",
					x, y, w, h, i, b->x, b->y, b->w, b->h);
			}
			g_fbFeedbackSuppress = 4;
			return;
		}
	}
}

extern "C" void GR_SetPsxDisplayBuffers(int x0, int y0, int x1, int y1, int w, int h)
{
	int gap;
	g_psxDispBuf[0].x = x0; g_psxDispBuf[0].y = y0;
	g_psxDispBuf[1].x = x1; g_psxDispBuf[1].y = y1;

	/* Clamp so buffer 0 can never spill into buffer 1's rows (SH's are exactly
	 * adjacent: 32 + 224 == 256). */
	gap = (y1 > y0) ? (y1 - y0) : h;
	if (gap > 0 && h > gap)
		h = gap;

	g_psxDispBuf[0].w = g_psxDispBuf[1].w = w;
	g_psxDispBuf[0].h = g_psxDispBuf[1].h = h;
	g_psxDispBufValid = (w > 0 && h > 0);
}

static const char* s_fbPackShaderSrc =
	"varying vec2 v_uv;\n"
	"#ifdef VERTEX\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
	"	v_uv = (p + 1.0) * 0.5;\n"
	/* The capture below is an unflipped window->texture blit, so texture row 0 is
	 * the screen BOTTOM. VRAM rows run downward with screen rows (the game samples
	 * buffer 0 from v=32 at the top of the screen), and viewport y=0 is the rect's
	 * first VRAM row — so the first VRAM row must receive the screen TOP. Flip. */
	"	v_uv.y = 1.0 - v_uv.y;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n"
	"#else\n"
	"uniform sampler2D s_texture;\n"
	/* Feedback gain. The game's blur SPRT is OPAQUE (code 0x64, no semi-trans bit)
	 * and modulates at 128/128 = identity, or 127/128 on the odd frame, so on PSX
	 * the loop is stored' = ~0.992*stored with new geometry overwriting on top:
	 * the last frame FREEZES on screen through a door load and Harry's running
	 * pose leaves a slow-decaying trail. Identity here reproduces that exactly.
	 *
	 * This used to be 0.5 to kill "an overexposed pile of ghosts smearing
	 * sideways". That pile was not a gain problem — it was the capture reading a
	 * different rect than the game redraws (see GR_StoreFrameBufferPsx), so each
	 * generation rescaled and piled onto itself. With the capture rect fixed the
	 * loop is stable at identity, and 0.5 would be wrong twice over: it fades a
	 * frame PSX holds, and each halving re-quantizes to 5 bits at an ever lower
	 * level, which is what turned the ghost into saturated colour blotches.
	 *
	 * Identity gain was tried (255/256, cancelling our (mod/255)*2 modulation
	 * against PSX's mod/128) and is WRONG here, because the two problems were
	 * independent: PSX can run at identity only because GsDefDispBuff2 CLEARS its
	 * draw buffer every frame (isbg=1), so "stored" is replaced. We have no VRAM
	 * double-buffer to clear, so stored accumulates and a unity loop diverges —
	 * it converged to a flat mid-grey field over the 4:3 core for a whole door
	 * load. Fixing the capture rect removed the rescale, not the accumulation.
	 *
	 * So keep damping the loop to the same steady state hardware reaches:
	 * stored = k*(0.992*stored + frame) settles at k/(1 - 0.992k), and k = 0.5
	 * gives ~1.0x — one frame's worth of ghost. Lower this if the trail reads too
	 * strong; do NOT raise it toward 1.0 without first clearing the destination. */
	/* Runtime-tunable (g_PsxFeedbackDamp, console FBDAMP) so the value can be
	 * found in-game rather than by rebuilds. The door out-fade wants a gain near
	 * identity so the held frame decays over ~1s like retail, while the
	 * Harry-running loading screen composites new geometry every frame and needs
	 * damping to reach a steady state -- one constant cannot serve both. Raising
	 * it toward 1.0 without first neutralising the blur SPRT's 2x modulation is
	 * what produced the flat mid-grey field recorded above. */
	"	uniform float u_feedbackDamp;\n"
	"void main() {\n"
	"	vec3 c = texture2D(s_texture, v_uv).rgb * u_feedbackDamp;\n"
	/* TRUNCATE, do not round. Retail's decay does not come from the gain -- at
	 * 127/128 over ~60 frames the frame would only reach ~0.61 -- it comes from
	 * this requantize dropping exactly one 5-bit level per pass, 31 passes to
	 * black. Rounding maps a damped level back onto itself, so the loop stalls
	 * and the frame freezes and smears instead of fading (what the Xbox port
	 * does). Inert at the shipped damp of 0.5, where the value halves regardless. */
	"	float r5 = floor(c.r * 31.0 + 0.002);\n"
	"	float g5 = floor(c.g * 31.0 + 0.002);\n"
	"	float b5 = floor(c.b * 31.0 + 0.002);\n"
	"	float w16 = r5 + g5 * 32.0 + b5 * 1024.0;\n"  /* mask bit left 0 */
	"	float hi  = floor(w16 / 256.0);\n"
	"	float lo  = w16 - hi * 256.0;\n"
	"	fragColor = vec4(lo / 255.0, hi / 255.0, 0.0, 1.0);\n"
	"}\n"
	"#endif\n";

static void GR_EnsureFbPackTarget(int w, int h)
{
	if (g_fbPackShader == (ShaderID)-1)
	{
		g_fbPackShader = GR_Shader_Compile(s_fbPackShaderSrc);
		glUseProgram(g_fbPackShader);
		{
			GLint loc = glGetUniformLocation(g_fbPackShader, "s_texture");
			if (loc != -1) glUniform1i(loc, 0);
		}
		glUseProgram(0);
		glGenVertexArrays(1, &g_fbPackVAO);
		glGenTextures(1, &g_fbPackTex);
		glGenFramebuffers(1, &g_fbPackFBO);
	}

	if (g_fbPackW == w && g_fbPackH == h)
		return;

	g_fbPackW = w;
	g_fbPackH = h;

	glBindTexture(GL_TEXTURE_2D, g_fbPackTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, g_fbPackFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbPackTex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Pack the captured frame into one VRAM rect. Saves/restores viewport + FBO and
 * invalidates the renderer's cached GL state, so this is safe to run mid-frame
 * (GR_UpdateVRAM calls it after a full vram[] re-upload). */
static void GR_PackFrameToVramRect(int x, int y, int w, int h)
{
#if USE_OPENGL
	GLint vp[4];

	if (!g_fbPackValid || w <= 0 || h <= 0)
		return;

	glGetIntegerv(GL_VIEWPORT, vp);

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
	glViewport(x, y, w, h);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(g_fbPackShader);
	{
		const GLint dampLoc = glGetUniformLocation(g_fbPackShader, "u_feedbackDamp");
		if (dampLoc != -1)
			glUniform1f(dampLoc, g_PsxFeedbackDamp);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_fbPackTex);
	glBindVertexArray(g_fbPackVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glEnable(GL_STENCIL_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(vp[0], vp[1], vp[2], vp[3]);

	/* Sentinels, not the real state — see GR_DrawFullscreenTexture: recording the
	 * truth lets GR_SetBlendMode early-return and strand depth off. */
	g_PreviousShader       = (ShaderID)-1;
	g_lastBoundTexture     = (TextureID)-1;
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousScissorState = -999;
#endif
}

/* Pack the captured frame into every rect the game may read back: both PSX
 * display buffers, plus a latched scene scratch rect if one is active. */
static void GR_PackFrameToAllFeedbackRects(void)
{
	if (!g_psxDispBufValid)
		return;

	GR_PackFrameToVramRect(g_psxDispBuf[0].x, g_psxDispBuf[0].y,
	                       g_psxDispBuf[0].w, g_psxDispBuf[0].h);
	GR_PackFrameToVramRect(g_psxDispBuf[1].x, g_psxDispBuf[1].y,
	                       g_psxDispBuf[1].w, g_psxDispBuf[1].h);

	if (g_sceneFbRedirectTtl > 0)
	{
		GR_PackFrameToVramRect(g_sceneFbRedirect.x, g_sceneFbRedirect.y,
		                       g_sceneFbRedirect.w, g_sceneFbRedirect.h);
	}
}

/* Stamp a VRAM rect to packed word 0 (RG = 0,0), which the samplers treat as
 * texel-0 = transparent (discard). Used to blank the feedback rects when the
 * loading blur is not active, so the per-map overlay samplers draw nothing. */
static void GR_ClearVramRect(int x, int y, int w, int h)
{
#if USE_OPENGL
	GLfloat cc[4];

	if (w <= 0 || h <= 0)
		return;

	glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);

	glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, y, w, h);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(cc[0], cc[1], cc[2], cc[3]);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	/* Sentinels, not the real state — see GR_DrawFullscreenTexture. */
	g_PreviousBlendMode    = -999;
	g_PreviousDepthMode    = -999;
	g_PreviousScissorState = -999;
#endif
}

static void GR_ClearAllFeedbackRects(void)
{
	if (!g_psxDispBufValid)
		return;

	GR_ClearVramRect(g_psxDispBuf[0].x, g_psxDispBuf[0].y, g_psxDispBuf[0].w, g_psxDispBuf[0].h);
	GR_ClearVramRect(g_psxDispBuf[1].x, g_psxDispBuf[1].y, g_psxDispBuf[1].w, g_psxDispBuf[1].h);

	if (g_sceneFbRedirectTtl > 0)
		GR_ClearVramRect(g_sceneFbRedirect.x, g_sceneFbRedirect.y, g_sceneFbRedirect.w, g_sceneFbRedirect.h);
}

/* Called once per present: capture the composed frame, then pack it into the
 * feedback rects. */
extern "C" void GR_StoreFrameBufferPsx(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	int w, h;
	GLuint readFBO = 0;

	if (!g_psxDispBufValid || g_PsxSkipFramebufferStore)
		return;

	/* The game just wrote its own data into a display-buffer rect — leave it
	 * alone until it stops (see GR_NoteVramUploadForFeedback). */
	if (g_fbFeedbackSuppress > 0)
	{
		g_fbFeedbackSuppress--;
		return;
	}

	/* Loading-screen-only: while the loading/transition blur is not drawing,
	 * blank the feedback rects (word 0 → transparent) so the per-map overlays
	 * this store would otherwise drive read nothing instead of a stale/garbage
	 * frame. See g_PsxFeedbackStoreAllowed. */
	if (g_PsxFeedbackStoreAllowed <= 0)
	{
		GR_ClearAllFeedbackRects();
		return;
	}
	g_PsxFeedbackStoreAllowed--;

	w = g_psxDispBuf[0].w;
	h = g_psxDispBuf[0].h;

	GR_EnsureFbPackTarget(w, h);

#if PSYX_HAS_POSTPROCESS
	/* MSAA: a multisample backbuffer cannot be the source of a scaled blit —
	 * resolve same-size into the post texture first, then downscale from there. */
	if (g_cfg_msaaSamples > 0)
	{
		GR_EnsurePostTarget(g_windowWidth, g_windowHeight);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_postFBO);
		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, 0, 0, g_postW, g_postH,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		readFBO = g_postFBO;
	}
#endif

	/* Unflipped downscale of the window rect the PSX DISPLAY BUFFER occupies into
	 * the 320x224 capture; the pack shader does the one flip needed to land
	 * screen-top on the rect's first VRAM row. LINEAR because this is a large
	 * downsample feeding a blur.
	 *
	 * The source MUST be g_psxAreaVp, not the viewport. The game redraws this
	 * capture over PSX coordinates (0,0)-(dispW,dispH), so whatever window rect
	 * those coordinates cover is the only rect that reads back 1:1. The viewport
	 * is a different rect in two ways: Hor+ widens the ortho so the 4:3 core is
	 * 1/effectiveScale of the window (0.75 at 16:9), and g_PsxWorldVScale crops
	 * the world ortho to 0.872 of the buffer. Capturing the viewport therefore
	 * shrank the picture 0.75x horizontally and stretched it 1.147x vertically
	 * EVERY frame, and because the effect feeds on its own output that compounds:
	 * within a few frames the last gameplay frame collapsed into a narrow,
	 * repeatedly 5-bit-requantized smear — saturated vertical streaks for a
	 * fraction of a second at every room transition (the only time this store is
	 * armed). Rows the ortho crops are left black: the frame simply has no pixels
	 * there, and leaving them black keeps the redraw scale-exact. */
	{
		float ax0 = g_psxAreaVp[0], ay0 = g_psxAreaVp[1];
		float ax1 = g_psxAreaVp[2], ay1 = g_psxAreaVp[3];
		int vx = g_presentVp[0], vy = g_presentVp[1];
		int vw = g_presentVp[2], vh = g_presentVp[3];
		float sx0, sy0, sx1, sy1;
		int dx0, dy0, dx1, dy1;

		if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = g_windowWidth; vh = g_windowHeight; }
		if (!g_psxAreaVpValid || ax1 <= ax0 || ay1 <= ay0)
		{
			ax0 = (float)vx; ay0 = (float)vy;
			ax1 = (float)(vx + vw); ay1 = (float)(vy + vh);
		}

		/* Clip to what was actually rendered. */
		sx0 = ax0 < (float)vx ? (float)vx : ax0;
		sy0 = ay0 < (float)vy ? (float)vy : ay0;
		sx1 = ax1 > (float)(vx + vw) ? (float)(vx + vw) : ax1;
		sy1 = ay1 > (float)(vy + vh) ? (float)(vy + vh) : ay1;

		/* The matching sub-rect of the capture, so the clip does not rescale. */
		dx0 = (int)((sx0 - ax0) / (ax1 - ax0) * (float)w + 0.5f);
		dx1 = (int)((sx1 - ax0) / (ax1 - ax0) * (float)w + 0.5f);
		dy0 = (int)((sy0 - ay0) / (ay1 - ay0) * (float)h + 0.5f);
		dy1 = (int)((sy1 - ay0) / (ay1 - ay0) * (float)h + 0.5f);

		{
			GLfloat cc[4];
			glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbPackFBO);
			glDisable(GL_SCISSOR_TEST);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glClearColor(cc[0], cc[1], cc[2], cc[3]);
		}

		if (sx1 > sx0 && sy1 > sy0 && dx1 > dx0 && dy1 > dy0)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
			glBlitFramebuffer((int)(sx0 + 0.5f), (int)(sy0 + 0.5f),
			                  (int)(sx1 + 0.5f), (int)(sy1 + 0.5f),
			                  dx0, dy0, dx1, dy1,
			                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	g_PreviousScissorState = 0;

	g_fbPackValid = 1;

	GR_PackFrameToAllFeedbackRects();

	if (g_sceneFbRedirectTtl > 0)
		g_sceneFbRedirectTtl--;
#endif
}

/* Re-pack after a full vram[] re-upload has stamped CPU bytes over the feedback
 * rects (GR_UpdateVRAM). */
extern "C" void GR_RepackFrameToVramBuffers(void)
{
	if (g_PsxSkipFramebufferStore)
		return;

	/* Re-blank (not re-pack) when the loading blur is off, so a full vram[]
	 * re-upload that just stamped CPU bytes over the feedback rects can't leave
	 * garbage for the per-map overlay samplers. */
	if (g_PsxFeedbackStoreAllowed <= 0)
	{
		GR_ClearAllFeedbackRects();
		return;
	}

	if (!g_fbPackValid)
		return;

	GR_PackFrameToAllFeedbackRects();
}

/* Legacy raw-blit scene-redirect helper, superseded by the packed path above.
 * Kept as a no-op shim so the old call site in GR_StoreFrameBuffer (which is
 * itself compiled out) does not need to change. */
static void GR_BlitStoredFrameToSceneRedirect(void)
{
#if USE_OPENGL && USE_FRAMEBUFFER_BLIT
	if (g_sceneFbRedirectTtl <= 0)
		return;
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

	/* Scene scratch-redirect feedback (see GR_SetSceneFbRedirect): give the
	 * effect's SPRT strips the frame they expect to find at the redirect rect.
	 * TTL decremented here — once per present. */
	GR_BlitStoredFrameToSceneRedirect();
	if (g_sceneFbRedirectTtl > 0)
	{
		g_sceneFbRedirectTtl--;
		/* Lapse = the scene stopped re-submitting its DR_AREA, so from the next
		 * present its strips sample whatever is really in VRAM at the rect. If
		 * the scene is still on screen when this prints, that IS the bar. */
		if (g_sceneFbRedirectTtl == 0 && s_sceneFbRedirectArms < 32)
		{
			eprintinfo("[FBSCRATCH] redirect LAPSED (%d,%d %dx%d) - strips now sample raw VRAM\n",
			           g_sceneFbRedirect.x, g_sceneFbRedirect.y,
			           g_sceneFbRedirect.w, g_sceneFbRedirect.h);
		}
	}

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

	/* A full vram[] re-upload also stamped TIM bytes over the scene scratch
	 * rect — re-blit the stored frame there too (no TTL decrement here). */
	GR_BlitStoredFrameToSceneRedirect();
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

	/* PC port: self-protect the framebuffer-feedback store. It repacks the frame
	 * into the PSX display-buffer rects every present, which is safe only while
	 * nothing else lives there. Most scenes keep CLUTs above the buffers (y<32)
	 * or below them (y>=480) — but some cutscenes LoadImage CLUTs straight into
	 * the display region as characters appear (map7_s03's endings hand-guard
	 * exactly that with g_PsxSkipFramebufferStore). Rather than rely on finding
	 * every such scene by hand, notice the upload here and stand the store down
	 * for a few frames so the game's own data always wins. Scenes that keep
	 * uploading there simply keep the effect off, which is the correct trade. */
	GR_NoteVramUploadForFeedback(dst_x, dst_y, w, h);

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

	/* PC port: the full vram[] re-upload just stamped CPU bytes over the
	 * framebuffer-feedback pages — re-pack the last captured frame into them so
	 * TIM streaming mid-scene can't blank the motion-blur / dream source. */
	GR_RepackFrameToVramBuffers();

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

/* PC port: force GL depth test ON for the inventory item pass (see
 * PsyX_ForceItemDepthBegin). When set, depth stays on even for the item's
 * semi-transparent faces (GR_SetBlendMode would otherwise GR_EnableDepth(0)),
 * so the model's own front faces occlude its back faces (radio antenna through
 * the body). Scoped by game code to the inventory screen and the pickup take
 * screen, where OT0 holds the item alone — never the live world. */
int g_PsyX_ForceItemDepth = 0;

void GR_EnableDepth(int enable)
{
	/* Track the APPLIED GL state (not the requested `enable`) so toggling
	 * g_PsyX_ForceItemDepth mid-frame re-applies on the next call. */
	int applied = ((enable && g_cfg_pgxpZBuffer) || g_PsyX_ForceItemDepth) ? 1 : 0;

	if (g_PreviousDepthMode == applied)
		return;

	g_PreviousDepthMode = applied;

#if USE_OPENGL
	if (applied)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

/* PGXP coplanar fix: switch the depth comparison between GL_ALWAYS (static-world
 * painter pass — every world face passes so coplanar faces resolve by OT order,
 * not depth test) and GL_LEQUAL (everything else). Only called on the PGXP-on
 * path (DrawSplit gates on g_PsxUsePgxp); when off, glDepthFunc stays at its
 * GL_LEQUAL init default and this never runs. State-cached like the siblings. */
void GR_SetDepthFuncAlways(int enable)
{
	enable = enable ? 1 : 0;
	if (g_PreviousDepthFuncAlways == enable)
		return;
	g_PreviousDepthFuncAlways = enable;
#if USE_OPENGL
	glDepthFunc(enable ? GL_ALWAYS : GL_LEQUAL);
#endif
}

/* [ITEMDEPTH] probe support: report the driver's ACTUAL depth state, not the
 * renderer's cached trackers. g_PreviousDepthMode is handed back alongside so a
 * tracker-vs-driver desync is visible in one line. Read-only — no glEnable /
 * glDepthFunc / glDepthMask here, and the GL error queue is drained so a driver
 * that rejects the attachment query cannot leak an error into the next draw. */
extern "C" void PsyX_ItemProbe_ReadGlDepthState(int* depthTest, int* depthFunc, int* depthMask,
                                                int* depthBits, float* rangeNear, float* rangeFar,
                                                int* fbo, int* cullFace, int* trackerDepthMode)
{
#if USE_OPENGL
	GLint    iv = 0;
	GLboolean bv = GL_FALSE;
	GLfloat  rv[2] = { 0.0f, 1.0f };

	if (depthTest) *depthTest = glIsEnabled(GL_DEPTH_TEST) ? 1 : 0;
	if (cullFace)  *cullFace  = glIsEnabled(GL_CULL_FACE) ? 1 : 0;

	glGetIntegerv(GL_DEPTH_FUNC, &iv);
	if (depthFunc) *depthFunc = (int)iv;

	glGetBooleanv(GL_DEPTH_WRITEMASK, &bv);
	if (depthMask) *depthMask = bv ? 1 : 0;

	glGetFloatv(GL_DEPTH_RANGE, rv);
	if (rangeNear) *rangeNear = rv[0];
	if (rangeFar)  *rangeFar  = rv[1];

	iv = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &iv);
	if (fbo) *fbo = (int)iv;

	{
		GLint bits = -1;
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
			iv ? GL_DEPTH_ATTACHMENT : GL_DEPTH,
			GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
		while (glGetError() != GL_NO_ERROR) { }
		if (depthBits) *depthBits = (int)bits;
	}
#else
	if (depthTest) *depthTest = -1;
	if (depthFunc) *depthFunc = -1;
	if (depthMask) *depthMask = -1;
	if (depthBits) *depthBits = -1;
	if (rangeNear) *rangeNear = 0.0f;
	if (rangeFar)  *rangeFar  = 1.0f;
	if (fbo)       *fbo       = -1;
	if (cullFace)  *cullFace  = -1;
#endif
	if (trackerDepthMode) *trackerDepthMode = g_PreviousDepthMode;
}

/* Bracket the inventory item OT0 draw: clear depth so the item tests only
 * against itself, force depth test+write on for every item face regardless of
 * blend, then restore. Both are no-ops for any other pass (game code only calls
 * them around the GameState_InventoryScreen item draw). */
extern "C" void PsyX_ForceItemDepthBegin(void)
{
#if USE_OPENGL
	g_PsyX_ForceItemDepth = 1;
	g_PreviousDepthMode = -1;   /* force next GR_EnableDepth to re-apply */
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
#endif
}

extern "C" void PsyX_ForceItemDepthEnd(void)
{
#if USE_OPENGL
	g_PsyX_ForceItemDepth = 0;
	g_PreviousDepthMode = -1;   /* force the next prim's GR_EnableDepth to re-apply */
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
		/* g_PreviousBlendMode < 0 is the post-shadow-pass "unknown" sentinel
		 * (-999): the pass glDisable()d blending, so treat it like BM_NONE here
		 * or the first blended split after the pass renders opaque (solid black
		 * where a subtractive/average prim was expected). */
		if(g_PreviousBlendMode == BM_NONE || g_PreviousBlendMode < 0)
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

extern "C" void Pc_ModernVertex_Upload(const GrModernVertex* vertices, int count);
static_assert(a_viewpos + 1 <= 8, "all vertex layouts must fit the GLES2 8-attribute limit");

void GR_UpdateModernVertexBuffer(const GrModernVertex* vertices, int num_vertices)
{
	Pc_ModernVertex_Upload(vertices, num_vertices);
}

struct PcModernDrawBinding { unsigned int textureId; int texFormat, vertexCount; int nativeWidth, nativeHeight; int offsetX, offsetY; int hiresWidth, hiresHeight; };
extern "C" int Pc_ModernMesh_PrepareDraw(unsigned int, PcModernDrawBinding*);

int GR_DrawModernMesh(unsigned int mesh_handle)
{
	PcModernDrawBinding binding; GTEShader* modern;
	if (!Pc_ModernMesh_PrepareDraw(mesh_handle, &binding) || binding.vertexCount < 3)
		return 0;
	TextureID texture = binding.textureId; int format = binding.texFormat, count = binding.vertexCount;
	if (format == TF_4_BIT) modern=&g_modern_shader_4;
	else if (format == TF_8_BIT) modern=&g_modern_shader_8;
	else if (format == TF_16_BIT) modern=&g_modern_shader_16;
	else modern=&g_modern_shader_32_rgba;
	/* Modern item meshes are opaque replacement geometry. Do not inherit the
	 * previous legacy split's blend equation (notably reverse-subtract). */
	GR_SetBlendMode(BM_NONE);
	/* ...nor its depth COMPARISON. The PGXP static-world pass leaves
	 * glDepthFunc(GL_ALWAYS) so coplanar world faces resolve by OT order; every
	 * fragment then wins, and this mesh is a single glDrawArrays with no software
	 * culling, so its back faces paint straight over its front ones — the model
	 * reads as see-through. Per-vertex depth is already correct here
	 * (Pc_ModernDepth_Apply), only the comparison was wrong.
	 *
	 * This has to be issued BEFORE the draw. The state reset at the end of this
	 * function assigns g_PreviousDepthFuncAlways = 0 without ever calling
	 * glDepthFunc, so GL could sit at GL_ALWAYS while the tracker claimed LEQUAL —
	 * a desync that also made the next GR_SetDepthFuncAlways(0) early-out. */
	GR_SetDepthFuncAlways(0);
	GR_SetTextureShader(texture, (TexFormat)format, modern);
	GR_SetOverrideTextureSize(binding.nativeWidth, binding.nativeHeight, binding.offsetX, binding.offsetY, binding.hiresWidth, binding.hiresHeight);
	/* Item OTs are drawn in the ACTIVE display coordinate space, which is not a
	 * constant: the inventory carousel runs a 320x448 double-buffer (vertices for
	 * the lower buffer carry y=224..448) while the world item pickup runs 320x224.
	 * Hardcoding 448 here made the pickup project into an ortho twice as tall as
	 * its real display, halving every y and parking the model near the top of the
	 * frame; the carousel only looked correct because 448 happened to match it.
	 * Derive it the same way the legacy path does (GR_SetOffscreenState reads
	 * activeDispEnv.disp.w/h) so both cases are right for the same reason. */
	GR_Ortho2D(0.0f, (float)activeDispEnv.disp.w, (float)activeDispEnv.disp.h, 0.0f, -1.0f, 1.0f);
	GR_DrawTriangles(0, count / 3);
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer ^ 1]);
	glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[g_curVertexBuffer ^ 1]);
	glUseProgram(0);
	g_PreviousShader = (ShaderID)-1; g_lastBoundTexture = (TextureID)-1;
	g_PreviousBlendMode = g_PreviousDepthMode = g_PreviousStencilMode = -999;
	g_PreviousDepthFuncAlways = 0; g_PreviousScissorState = g_PreviousOffscreenState = -999;
	return 1;
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