#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

#include "PsyX_main.h"

#include "PsyX/PsyX_version.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/util/timer.h"

#include "gpu/PsyX_GPU.h"
#include "pad/PsyX_pad.h"

#include "platform.h"
#include "util/crash_handler.h"

#include "psx/libetc.h"
#include "psx/libgte.h"
#include "psx/libgpu.h"
#include "psx/libspu.h"
#include "audio/PsyX_SPUAL.h"

#include <assert.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#include <exception>

#include "PsyX/PsyX_render.h"

#ifdef __EMSCRIPTEN__
int strcasecmp(const char* _l, const char* _r)
{
	const u_char* l = (u_char*)_l, * r = (u_char*)_r;
	for (; *l && *r && (*l == *r || tolower(*l) == tolower(*r)); l++, r++);
	return tolower(*l) - tolower(*r);
}
#elif !defined(_WIN32)
#include <strings.h>
#endif

SDL_Window* g_window = NULL;
int g_swapInterval = 1;
int g_enableSwapInterval = 1;
int g_skipSwapInterval = 0;
timerCtx_t g_vblTimer;

int							g_cfg_swapInterval = 0;
PsyXKeyboardMapping			g_cfg_keyboardMapping;
PsyXKeyboardMapping			g_cfg_keyboardMapping2 = {0};	/* secondary binds; 0 = SDL_SCANCODE_UNKNOWN (unset) */
PsyXControllerMapping		g_cfg_controllerMapping;
PsyXControllerMapping		g_cfg_controllerMapping2;	/* secondary controller binds; all fields BUTTON_INVALID (unset) until configured */
int							g_cfg_allowMouseSecondary = 0;
unsigned short				g_cfg_mouseButtonMask[8] = {0};	/* [SDL button 1..5] -> PSX button bitmask */
GameOnTextInputHandler		g_cfg_gameOnTextInput = NULL;

GameDebugKeysHandlerFunc	g_dbg_gameDebugKeys = NULL;
GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse = NULL;
int							g_dbg_polygonSelected = 0;

enum EPsxCounters
{
	PsxCounter_VBLANK,

	PsxCounter_Num
};

volatile int g_psxSysCounters[PsxCounter_Num];

SDL_Thread* g_intrThread = NULL;
SDL_mutex* g_intrMutex = NULL;
volatile char g_stopIntrThread = 0;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern void(*vsync_callback)(void);
extern int g_rcnt2_timer_active;
extern void PsyX_PumpRCnt2Timer(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

extern int	PsyX_Pad_InitSystem();
extern void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId);
extern void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId);

extern int	GR_InitialisePSX();
extern int	GR_InitialiseRender(char* windowName, int width, int height, int fullscreen);

extern void GR_ResetDevice();
extern void GR_Shutdown();
extern void GR_BeginScene();
extern void GR_EndScene();
extern void GR_UpdateSwapIntervalState(int swapInterval);

int g_vmode = -1;
int g_frameSkip = 0;

#ifdef __EMSCRIPTEN__

int g_emIntrInterval = -1;
int g_intrVMode = MODE_NTSC;
double g_emOldDate = 0;

void emIntrCallback(void* userData)
{
	double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;

	int newVBlank = (Util_GetHPCTime(&g_vblTimer, 0) / timestep) + g_frameSkip;

	int diff = newVBlank - g_psxSysCounters[PsxCounter_VBLANK];

	while (diff--)
	{
		if (vsync_callback)
			vsync_callback();

		g_psxSysCounters[PsxCounter_VBLANK]++;
	}
}

EM_BOOL emIntrCallback2(double time, void* userData)
{
	emIntrCallback(userData);
	return g_stopIntrThread ? EM_FALSE : EM_TRUE;
}

#endif

int PsyX_Sys_SetVMode(int mode)
{
	int old = g_vmode;
	g_vmode = mode;

#ifdef __EMSCRIPTEN__
	if (old != g_vmode)
	{
		//if(g_emIntrInterval != -1)
		//	emscripten_clear_interval(g_emIntrInterval);
		g_stopIntrThread = 1;

		emscripten_sleep(100);

		g_stopIntrThread = 0;
		emscripten_set_timeout_loop(emIntrCallback2, 1.0, NULL);
	}
#endif

	return old;
}

int PsyX_Sys_GetVBlankCount()
{
	if (g_skipSwapInterval)
	{
		// extra speedup.
		// does not affect `vsync_callback` count
		g_psxSysCounters[PsxCounter_VBLANK] += 1;
		g_frameSkip++;
	}
	
	return g_psxSysCounters[PsxCounter_VBLANK];
}

int intrThreadMain(void* data)
{
	timerCtx_t rcnt2Timer;
	Util_InitHPCTimer(&g_vblTimer);
	Util_InitHPCTimer(&rcnt2Timer);

	/* PSX RCnt2 = system clock / 8 = 4233600 Hz.
	 * Target 7328 → fires at 4233600/7328 ≈ 577.8 Hz → ~1.73ms period */
	const double rcnt2Period = 1.0 / 577.8;

	while (!g_stopIntrThread)
	{
		// step counters
		{
			const double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;
			const double vblDelta = Util_GetHPCTime(&g_vblTimer, 0);

			if (vblDelta > timestep)
			{
				SDL_LockMutex(g_intrMutex);

				if (vsync_callback)
					vsync_callback();

				SDL_UnlockMutex(g_intrMutex);

				// do vblank events
				g_psxSysCounters[PsxCounter_VBLANK]++;

				Util_GetHPCTime(&g_vblTimer, 1);
			}

			/* Pump RCnt2 timer for BGM/MIDI sequencer */
			if (g_rcnt2_timer_active)
			{
				const double rcnt2Delta = Util_GetHPCTime(&rcnt2Timer, 0);
				if (rcnt2Delta > rcnt2Period)
				{
					SDL_LockMutex(g_intrMutex);
					PsyX_PumpRCnt2Timer();
					SDL_UnlockMutex(g_intrMutex);
					Util_GetHPCTime(&rcnt2Timer, 1);
				}
			}
		}

		/* Advance SPU ADSR envelopes on the audio-timing thread, NOT from the
		 * render thread (PsyX_EndScene) — that placement deadlocked. Takes only
		 * g_SpuMutex (never nested under g_intrMutex), throttles on the
		 * SDL_GetTicks ms delta, no-op unless `adsr 1`. */
		PsyX_SPUAL_Update();
	}

	return 0;
}

static int PsyX_Sys_InitialiseCore()
{
#ifdef __EMSCRIPTEN__
	Util_InitHPCTimer(&g_vblTimer);
#else

	g_intrThread = SDL_CreateThread(intrThreadMain, "psyX_intr", NULL);

	if (NULL == g_intrThread)
	{
		eprinterr("SDL_CreateThread failed: %s\n", SDL_GetError());
		return 0;
	}
	
	g_intrMutex = SDL_CreateMutex();
	if (NULL == g_intrMutex)
	{
		eprinterr("SDL_CreateMutex failed: %s\n", SDL_GetError());
		return 0;
	}
#endif
	return 1;
}

static void PsyX_Sys_InitialiseInput()
{
	g_cfg_keyboardMapping.kc_square = SDL_SCANCODE_X;
	g_cfg_keyboardMapping.kc_circle = SDL_SCANCODE_V;
	g_cfg_keyboardMapping.kc_triangle = SDL_SCANCODE_Z;
	g_cfg_keyboardMapping.kc_cross = SDL_SCANCODE_C;

	/* Sidestep moved to A/D, aim moved to LSHIFT, view moved to RSHIFT.
	 * RCTRL was unreliable on the user's Win11 setup (never reached
	 * btnsHeld_C even though SDL_SCANCODE_RCTRL is the documented
	 * scancode for the right Control key). LCTRL also retired so the
	 * shift keys mirror cleanly. */
	g_cfg_keyboardMapping.kc_l1 = SDL_SCANCODE_A;            /* sidestep left */
	g_cfg_keyboardMapping.kc_l2 = SDL_SCANCODE_RSHIFT;       /* view */
	g_cfg_keyboardMapping.kc_l3 = SDL_SCANCODE_UNKNOWN  /* [ reserved for effect-intensity control */;

	g_cfg_keyboardMapping.kc_r1 = SDL_SCANCODE_D;            /* sidestep right */
	g_cfg_keyboardMapping.kc_r2 = SDL_SCANCODE_LSHIFT;       /* aim */
	g_cfg_keyboardMapping.kc_r3 = SDL_SCANCODE_UNKNOWN /* ] reserved for effect-intensity control */;

	g_cfg_keyboardMapping.kc_dpad_up = SDL_SCANCODE_UP;
	g_cfg_keyboardMapping.kc_dpad_down = SDL_SCANCODE_DOWN;
	g_cfg_keyboardMapping.kc_dpad_left = SDL_SCANCODE_LEFT;
	g_cfg_keyboardMapping.kc_dpad_right = SDL_SCANCODE_RIGHT;

	g_cfg_keyboardMapping.kc_select = SDL_SCANCODE_SPACE;
	g_cfg_keyboardMapping.kc_start = SDL_SCANCODE_RETURN;

	//----------------
	g_cfg_controllerMapping.gc_square = SDL_CONTROLLER_BUTTON_X;
	g_cfg_controllerMapping.gc_circle = SDL_CONTROLLER_BUTTON_B;
	g_cfg_controllerMapping.gc_triangle = SDL_CONTROLLER_BUTTON_Y;
	g_cfg_controllerMapping.gc_cross = SDL_CONTROLLER_BUTTON_A;

	g_cfg_controllerMapping.gc_l1 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	g_cfg_controllerMapping.gc_l2 = SDL_CONTROLLER_AXIS_TRIGGERLEFT | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_l3 = SDL_CONTROLLER_BUTTON_LEFTSTICK;

	g_cfg_controllerMapping.gc_r1 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	g_cfg_controllerMapping.gc_r2 = SDL_CONTROLLER_AXIS_TRIGGERRIGHT | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_r3 = SDL_CONTROLLER_BUTTON_RIGHTSTICK;

	g_cfg_controllerMapping.gc_dpad_up = SDL_CONTROLLER_BUTTON_DPAD_UP;
	g_cfg_controllerMapping.gc_dpad_down = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	g_cfg_controllerMapping.gc_dpad_left = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	g_cfg_controllerMapping.gc_dpad_right = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;

	g_cfg_controllerMapping.gc_select = SDL_CONTROLLER_BUTTON_BACK;
	g_cfg_controllerMapping.gc_start = SDL_CONTROLLER_BUTTON_START;

	g_cfg_controllerMapping.gc_axis_left_x = SDL_CONTROLLER_AXIS_LEFTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_left_y = SDL_CONTROLLER_AXIS_LEFTY | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_x = SDL_CONTROLLER_AXIS_RIGHTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_y = SDL_CONTROLLER_AXIS_RIGHTY | CONTROLLER_MAP_FLAG_AXIS;

	/* Secondary controller binds default to all-unset (every field
	 * SDL_CONTROLLER_BUTTON_INVALID == -1). The game's Pc_ApplyControlConfig sets
	 * the action buttons it wants; anything left here reads as "not pressed". */
	memset(&g_cfg_controllerMapping2, 0xFF, sizeof(g_cfg_controllerMapping2));

	PsyX_Pad_InitSystem();
}

#ifdef __GNUC__
/* strcasecmp lives in <strings.h>, but in this TU an earlier include locks
 * the glibc feature-test macros before <strings.h> is reached, leaving it
 * undeclared under -std=gnu++17. Declare it directly (POSIX signature). */
extern "C" int strcasecmp(const char* s1, const char* s2);
#define _stricmp(s1, s2) strcasecmp(s1, s2)
#endif

// Keyboard mapping lookup
int PsyX_LookupKeyboardMapping(const char* str, int default_value)
{
	const char* scancodeName;
	int i;

	if (str)
	{
		if (!_stricmp("NONE", str))
			return SDL_SCANCODE_UNKNOWN;

		for (i = 0; i < SDL_NUM_SCANCODES; i++)
		{
			scancodeName = SDL_GetScancodeName((SDL_Scancode)i);

			if (strlen(scancodeName) && !_stricmp(scancodeName, str))
			{
				return i;
			}
		}
	}

	return default_value;
}

// Game controller mapping lookup
// Available controller binds(refer to SDL2 game controller)
//
// Axes:
//	leftx lefty
//	rightx righty
//	lefttrigger righttrigger
//
// NOTE: adding `-` before axis names makes it inverse, so `-leftx` inverse left stick X axis
//
// Buttons:
// 	a, b, x, y
// 	back guide start
// 	leftstick rightstick
// 	leftshoulder rightshoulder
// 	dpup dpdown dpleft dpright

int PsyX_LookupGameControllerMapping(const char* str, int default_value)
{
	const char* axisStr;
	const char* buttonOrAxisName;
	int i, axisFlags;

	if (str)
	{
		axisFlags = CONTROLLER_MAP_FLAG_AXIS;
		axisStr = str;

		if (*axisStr == '-')
		{
			axisFlags |= CONTROLLER_MAP_FLAG_INVERSE;
			axisStr++;
		}

		if (!_stricmp("NONE", str))
			return SDL_CONTROLLER_BUTTON_INVALID;

		// check buttons
		for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
		{
			buttonOrAxisName = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i);

			if (strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, str))
			{
				return i;
			}
		}

		// Check axes
		for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
		{
			buttonOrAxisName = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)i);

			if (strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, axisStr))
			{
				return i | axisFlags;
			}
		}
	}

	return default_value;
}

char* g_appNameStr = NULL;

void PsyX_GetWindowName(char* buffer)
{
#ifdef _DEBUG
	sprintf(buffer, "%s | Debug", g_appNameStr);
#else
	sprintf(buffer, "%s", g_appNameStr);
#endif
}

FILE* g_logStream = NULL;

/* Set by PsyX_Exit() (the normal close path: SDL_QUIT / window-close / Alt+F4).
 * A benign C++ exception escapes during exit()/static-destruction teardown on
 * every normal close (the thrower is not yet isolated), tripping std::terminate
 * AFTER a fully clean shutdown. When this flag is set the terminate handler
 * treats it as the normal exit it is — no misleading FATAL line, clean exit. */
static volatile int g_psyxNormalExit = 0;

/* When the host app routes PsyX logging into its own stream (or NULL to
 * silence it), PsyX must not fopen its own "<app>.log" and must never
 * fclose the host's handle at shutdown — doing so left the host's stdio
 * FILE* dangling for any logging that ran after PsyX_Shutdown. */
static int g_logStreamExternal = 0;

void PsyX_Log_SetStream(FILE* stream)
{
	if (g_logStream && !g_logStreamExternal)
		fclose(g_logStream);

	g_logStream = stream;
	g_logStreamExternal = 1;
}

// intialise logging
void PsyX_Log_Initialise()
{
	char appLogFilename[128];

	if (g_logStreamExternal)
		return;

	sprintf(appLogFilename, "%s.log", g_appNameStr);

	g_logStream = fopen(appLogFilename, "wb");

	if (!g_logStream)
		eprinterr("Error - cannot create log file '%s'\n", appLogFilename);
}

void PsyX_Log_Finalise()
{
	if (g_logStreamExternal)
	{
		if (g_logStream)
			fflush(g_logStream);
		return;
	}

	PsyX_Log_Warning("---- LOG CLOSED ----\n");

	if (g_logStream)
		fclose(g_logStream);

	g_logStream = NULL;
}

void PsyX_Log_Flush()
{
	if (g_logStream)
		fflush(g_logStream);
}

// spew types
typedef enum
{
	SPEW_NORM,
	SPEW_INFO,
	SPEW_WARNING,
	SPEW_ERROR,
	SPEW_SUCCESS,
}SpewType_t;

#ifdef _WIN32
static unsigned short g_InitialColor = 0xFFFF;
static unsigned short g_LastColor = 0xFFFF;
static unsigned short g_BadColor = 0xFFFF;
static WORD g_BackgroundFlags = 0xFFFF;
CRITICAL_SECTION g_SpewCS;
char g_bSpewCSInitted = 0;

static void Spew_GetInitialColors()
{
	// Get the old background attributes.
	CONSOLE_SCREEN_BUFFER_INFO oldInfo;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &oldInfo);
	g_InitialColor = g_LastColor = oldInfo.wAttributes & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	g_BackgroundFlags = oldInfo.wAttributes & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);

	g_BadColor = 0;
	if (g_BackgroundFlags & BACKGROUND_RED)
		g_BadColor |= FOREGROUND_RED;
	if (g_BackgroundFlags & BACKGROUND_GREEN)
		g_BadColor |= FOREGROUND_GREEN;
	if (g_BackgroundFlags & BACKGROUND_BLUE)
		g_BadColor |= FOREGROUND_BLUE;
	if (g_BackgroundFlags & BACKGROUND_INTENSITY)
		g_BadColor |= FOREGROUND_INTENSITY;
}

static WORD Spew_SetConsoleTextColor(int red, int green, int blue, int intensity)
{
	WORD ret = g_LastColor;

	g_LastColor = 0;
	if (red)	g_LastColor |= FOREGROUND_RED;
	if (green) g_LastColor |= FOREGROUND_GREEN;
	if (blue)  g_LastColor |= FOREGROUND_BLUE;
	if (intensity) g_LastColor |= FOREGROUND_INTENSITY;

	// Just use the initial color if there's a match...
	if (g_LastColor == g_BadColor)
		g_LastColor = g_InitialColor;

	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), g_LastColor | g_BackgroundFlags);
	return ret;
}

static void Spew_RestoreConsoleTextColor(WORD color)
{
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color | g_BackgroundFlags);
	g_LastColor = color;
}

void Spew_ConDebugSpew(SpewType_t type, char* text)
{
	// Hopefully two threads won't call this simultaneously right at the start!
	if (!g_bSpewCSInitted)
	{
		Spew_GetInitialColors();
		InitializeCriticalSection(&g_SpewCS);
		g_bSpewCSInitted = 1;
	}

	WORD old;
	EnterCriticalSection(&g_SpewCS);
	{
		if (type == SPEW_NORM)
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 0);
		}
		else if (type == SPEW_WARNING)
		{
			old = Spew_SetConsoleTextColor(1, 1, 0, 1);
		}
		else if (type == SPEW_SUCCESS)
		{
			old = Spew_SetConsoleTextColor(0, 1, 0, 1);
		}
		else if (type == SPEW_ERROR)
		{
			old = Spew_SetConsoleTextColor(1, 0, 0, 1);
		}
		else if (type == SPEW_INFO)
		{
			old = Spew_SetConsoleTextColor(0, 1, 1, 1);
		}
		else
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 1);
		}

		OutputDebugStringA(text);
		printf("%s", text);

		Spew_RestoreConsoleTextColor(old);
	}
	LeaveCriticalSection(&g_SpewCS);
}
#endif

void PrintMessageToOutput(SpewType_t spewtype, char const* pMsgFormat, va_list args)
{
	static char pTempBuffer[4096];
	int len = 0;
	vsprintf(&pTempBuffer[len], pMsgFormat, args);

#ifdef WIN32
	Spew_ConDebugSpew(spewtype, pTempBuffer);
#elif defined(__EMSCRIPTEN__)
	if (spewtype == SPEW_INFO)
	{
		EM_ASM({
			console.info(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_WARNING)
	{
		EM_ASM({
			console.warn(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_ERROR)
	{
		EM_ASM({
			console.error(UTF8ToString($0));
		}, pTempBuffer);
	}
	else
	{
		EM_ASM({
			console.log(UTF8ToString($0));
		}, pTempBuffer);
	}
#else
	printf(pTempBuffer);
#endif

	if(g_logStream)
		fprintf(g_logStream, pTempBuffer);
}

void PsyX_Log(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_NORM, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Info(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_INFO, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Warning(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_WARNING, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Error(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_ERROR, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Success(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_SUCCESS, fmt, argptr);
	va_end(argptr);
}


static void sh_terminate_handler()
{
	/* abort() skips atexit/stdio teardown, so the host's buffered log
	 * (up to 64KB of tail) would be silently lost on every terminate.
	 * Flush it here so the log survives. */
	if (g_logStream)
		fflush(g_logStream);

	if (g_psyxNormalExit)
	{
		/* Normal close already shut everything down cleanly; the terminate is
		 * the benign teardown exception, not a crash. Exit clean, no FATAL
		 * spam. _Exit avoids re-entering the atexit chain we're already in. */
		_Exit(0);
	}

	fprintf(stderr, "[PsyX] FATAL: std::terminate() called!\n");
	fflush(stderr);
	abort();
}

void PsyX_Initialise(char* appName, int width, int height, int fullscreen)
{
	char windowNameStr[128];

	g_appNameStr = appName;

	std::set_terminate(sh_terminate_handler);
	InstallExceptionHandler();

	PsyX_Log_Initialise();
	PsyX_GetWindowName(windowNameStr);

#if defined(_WIN32) && defined(_DEBUG)
	if (AllocConsole())
	{
		freopen("CONOUT$", "w", stdout);
		SetConsoleTitleA(windowNameStr);
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
#endif

	eprintinfo("Initialising Psy-X %d.%d\n", PSYX_MAJOR_VERSION, PSYX_MINOR_VERSION);
	eprintinfo("Build date: %s:%s\n", PSYX_COMPILE_DATE, PSYX_COMPILE_TIME);

#if defined(__EMSCRIPTEN__)
	SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
#endif
	
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		eprinterr("Failed to initialise SDL\n");
		PsyX_Shutdown();
		return;
	}
	
	if (!GR_InitialiseRender(windowNameStr, width, height, fullscreen))
	{
		eprinterr("Failed to Intialise Window\n");
		PsyX_Shutdown();
		return;
	}

	if (!PsyX_Sys_InitialiseCore())
	{
		eprinterr("Failed to Intialise Psy-X Core.\n");
		PsyX_Shutdown();
		return;
	}

	if (!GR_InitialisePSX())
	{
		eprinterr("Failed to Intialise PSX.\n");
		PsyX_Shutdown();
		return;
	}

	PsyX_Sys_InitialiseInput();

	// set shutdown function (PSX apps usualy don't exit)
	atexit(PsyX_Shutdown);

	// disable cursor visibility
	SDL_ShowCursor(0);
}

void PsyX_GetScreenSize(int* screenWidth, int* screenHeight)
{
	SDL_GetWindowSize(g_window, screenWidth, screenHeight);
}

void PsyX_SetCursorPosition(int x, int y)
{
	SDL_WarpMouseInWindow(g_window, x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down); // forward decl
void PsyX_Sys_DoDebugMouseMotion(int x, int y);

void PsyX_Exit();

int g_activeKeyboardControllers = 0x1;
int g_altKeyState = 0;

void PsyX_Sys_DoPollEvent()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
			case SDL_CONTROLLERDEVICEADDED:
				PsyX_Pad_Event_ControllerAdded(event.cdevice.which);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				PsyX_Pad_Event_ControllerRemoved(event.cdevice.which);
				break;
			case SDL_QUIT:
				PsyX_Exit();
				break;
			case SDL_WINDOWEVENT:
				switch (event.window.event)
				{
				case SDL_WINDOWEVENT_RESIZED:
					g_windowWidth = event.window.data1;
					g_windowHeight = event.window.data2;
					GR_ResetDevice();
					break;
				case SDL_WINDOWEVENT_CLOSE:
					PsyX_Exit();
					break;
				}
				break;
			case SDL_MOUSEMOTION:

				PsyX_Sys_DoDebugMouseMotion(event.motion.x, event.motion.y);
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
			{
				int nKey = event.key.keysym.scancode;

				if (nKey == SDL_SCANCODE_RALT)
				{
					g_altKeyState = (event.type == SDL_KEYDOWN);
				}
				else if (nKey == SDL_SCANCODE_RETURN)
				{
					if (g_altKeyState && event.type == SDL_KEYDOWN)
					{
						int fullscreen = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN > 0;

						SDL_SetWindowFullscreen(g_window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);

						SDL_GetWindowSize(g_window, &g_windowWidth, &g_windowHeight);
						GR_ResetDevice();
					}
					break;
				}

				// lshift/right shift
				if (nKey == SDL_SCANCODE_RSHIFT)
					nKey = SDL_SCANCODE_LSHIFT;
				else if (nKey == SDL_SCANCODE_RCTRL)
					nKey = SDL_SCANCODE_LCTRL;
				else if (nKey == SDL_SCANCODE_RALT)
					nKey = SDL_SCANCODE_LALT;

				if (g_cfg_gameOnTextInput && nKey == SDL_SCANCODE_BACKSPACE && event.type == SDL_KEYDOWN)
				{
					(g_cfg_gameOnTextInput)(NULL);
				}

				PsyX_Sys_DoDebugKeys(nKey, (event.type == SDL_KEYUP) ? 0 : 1);
				break;
			}
			case SDL_TEXTINPUT:
			{
				if(g_cfg_gameOnTextInput)
					(g_cfg_gameOnTextInput)(event.text.text);
				break;
			}			
		}
	}
}

char begin_scene_flag = 0;

char PsyX_BeginScene()
{
	PsyX_Sys_DoPollEvent();

	if (begin_scene_flag)
		return 0;

	assert(!begin_scene_flag);

	{
		int swapInterval = (g_cfg_swapInterval && g_enableSwapInterval && !g_skipSwapInterval) ? g_swapInterval : 0;

		// Maximum is (ScreenRefreshRate / 2). 
		// If our screen refresh rate is lower than our PSX vmode refresh rate, 
		// we reducing swap interval to maintain the framerate.
		// Example:
		//		target 60fps, 50hz screen = no interval (tearing)
		//		target 30fps, 50hz screen = 60hz interval (less tearing)
		//		target 30fps, 60hz screen = 30hz interval (no tearing)
		SDL_DisplayMode curMode;
		if (SDL_GetWindowDisplayMode(g_window, &curMode) == 0)
		{
			const int mode_frequency = g_vmode == MODE_NTSC ? VBLANK_FREQUENCY_NTSC : VBLANK_FREQUENCY_PAL;
			if (curMode.refresh_rate < mode_frequency)
				swapInterval--;
		}

		if (swapInterval < 0)
			swapInterval = 0;
		
		GR_UpdateSwapIntervalState(swapInterval);
	}

	GR_BeginScene();

	// Always clear the backbuffer at the start of every frame. The PSX
	// behavior gates this on activeDrawEnv.isbg, but during state
	// transitions (door fades, map pickups, screen wipes) the game can
	// briefly emit prims into the OT for a frame where isbg wasn't yet
	// set — leaving stale "street sign" garbage from the prior frame
	// visible for a split second. Forcing a clear every frame is the
	// PC equivalent of glClear(GL_COLOR_BUFFER_BIT) at frame start.
	{
		const RECT16 clipenv = activeDrawEnv.clip;
		const u_char r = activeDrawEnv.isbg ? activeDrawEnv.r0 : 0;
		const u_char g = activeDrawEnv.isbg ? activeDrawEnv.g0 : 0;
		const u_char b = activeDrawEnv.isbg ? activeDrawEnv.b0 : 0;
		GR_Clear(clipenv.x, clipenv.y, clipenv.w, clipenv.h, r, g, b);
	}

	/* PC port: while the game is frozen (pause / console / map message),
	 * re-present the last captured gameplay frame so this frame's UI prims
	 * draw on top of the frozen world — PSX got this for free because it
	 * never auto-cleared the framebuffer. */
	{
		extern int g_PsxPresentLastFrame;
		extern void GR_PresentLastFrame(void);
		if (g_PsxPresentLastFrame)
			GR_PresentLastFrame();
	}

	begin_scene_flag = 1;

	PsyX_Log_Flush();

	return 1;
}

uint PsyX_CalcFPS();

/* PC port: optional hook the game registers (DbgOverlay_Render) to draw the dev console
 * AFTER the freeze-frame is captured, so the console is never baked into a frozen
 * pause / "no map" image (which would ghost the live console against the frozen copy).
 * NULL = nothing extra drawn. */
extern "C" void (*g_PsyX_PostCaptureHook)(void) = NULL;

void PsyX_EndScene()
{
	if (!begin_scene_flag)
		return;

	assert(begin_scene_flag);
	begin_scene_flag = 0;

	PGXP_CoverageTick();

	GR_EndScene();

#ifndef PSYX_SKIP_FRAMEBUFFER_STORE
	GR_StoreFrameBuffer(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
#endif

	/* PC port: g_PsxSkipFramebufferStore is a per-frame opt-out — the game must
	 * re-set it each tick during a TIM-protect screen (e.g. paper-map pickup). */
	g_PsxSkipFramebufferStore = 0;

	/* PC port: keep a copy of every composed frame for freeze-frame
	 * presentation (skipped internally on frames that re-presented it). */
	{
		extern void GR_CaptureLastFrame(void);
		GR_CaptureLastFrame();
	}

	/* PC port: draw overlays that must NOT be baked into the freeze-frame (the dev
	 * console) — AFTER the capture, BEFORE the swap, so the console is a true live
	 * overlay and never doubles against a frozen copy on pause / "no map" screens. */
	if (g_PsyX_PostCaptureHook)
		g_PsyX_PostCaptureHook();

	/* PC port: apply the selected full-screen post-process look (color grade,
	 * CRT, scanlines, vignette, grain, sharpen, PSX downsample, ...) to the
	 * fully composed frame just before presenting. No-op when off. */
	{
		extern void GR_PostProcess(void);
		GR_PostProcess();
	}

	GR_SwapWindow();
}

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
void PsyX_TakeScreenshot()
{
	u_char* pixels = (u_char*)malloc(g_windowWidth * g_windowHeight * 4);
	
#if defined(RENDERER_OGL)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
#elif defined(RENDERER_OGLES)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);	// FIXME: is that correct format?
#endif

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(pixels, g_windowWidth, g_windowHeight, 8 * 4, g_windowWidth * 4, 0, 0, 0, 0);

	SDL_SaveBMP(surface, "SCREENSHOT.BMP");
	SDL_FreeSurface(surface);

	free(pixels);
}
#endif

void PsyX_Sys_DoDebugMouseMotion(int x, int y)
{
	if (g_dbg_gameDebugMouse)
		g_dbg_gameDebugMouse(x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down)
{
	if (g_dbg_gameDebugKeys)
		g_dbg_gameDebugKeys(nKey, down);

	/* Backspace fast-forward (g_skipSwapInterval) removed — it was unguarded and
	 * trivially hit by accident, silently running the game at uncapped speed. */

	if (!down)
	{
		switch (nKey)
		{
#ifdef _DEBUG
		case SDL_SCANCODE_F1:
			g_dbg_wireframeMode ^= 1;
			eprintwarn("wireframe mode: %d\n", g_dbg_wireframeMode);
			break;

		case SDL_SCANCODE_F2:
			g_dbg_texturelessMode ^= 1;
			eprintwarn("textureless mode: %d\n", g_dbg_texturelessMode);
			break;
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			if (g_dbg_emulatorPaused)
			{
				g_dbg_polygonSelected += (nKey == SDL_SCANCODE_UP) ? 3 : -3;
			}
			break;
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
		case SDL_SCANCODE_F10:
			/* PC port: moved out of #ifdef _DEBUG so the VRAM dump works in normal
			 * (debug-enabled) builds too — it's the key diagnostic for the texture/
			 * VRAM bugs (boss-FX ghost textures etc.). Writes VRAM.TGA to the cwd. */
			eprintwarn("saving VRAM.TGA\n");
			GR_SaveVRAM("VRAM.TGA", 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
			break;
		case SDL_SCANCODE_F12:
			eprintwarn("Saving screenshot...\n");
			PsyX_TakeScreenshot();
			break;
#endif
		/* F3 freed for the game-side tone-map cycle (dbg_overlay.c). The old
		 * bilinear-filtering toggle here was redundant — filtering is set via the
		 * launcher (psx_dither/Filtering option -> main_pc.c). */
		/* F4 keyboard-controller-slot cycle removed — a stray tap moved keyboard +
		 * mouse input off player 1, silently killing fire/aim (read as a gameplay
		 * bug). g_activeKeyboardControllers stays at its 0x1 default. */
		case SDL_SCANCODE_F5:
			g_cfg_pgxpTextureCorrection ^= 1;
			break;
		case SDL_SCANCODE_F6:
			g_cfg_pgxpZBuffer ^= 1;
			break;
		}
	}
}

void PsyX_UpdateInput()
{
	// also poll events here
	PsyX_Sys_DoPollEvent();

	/* Re-assert cursor hidden every frame. SDL_ShowCursor(0) at init isn't
	 * sticky on Windows: alt-tabbing back into fullscreen, or a window-mode
	 * transition triggered by SDL itself, can flip cursor visibility back
	 * on. The user kept seeing the OS arrow pinned at screen center every
	 * gameplay screenshot. SDL_ShowCursor with the same value is a no-op
	 * (just queries internal state), so calling it per-frame is free. */
	if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
		SDL_ShowCursor(SDL_DISABLE);

	if(!g_altKeyState)
		PsyX_Pad_InternalPadUpdates();
}

uint PsyX_CalcFPS()
{
#define FPS_INTERVAL 1.0

	static unsigned int lastTime = 0;
	static unsigned int currentFps = 0;
	static unsigned int passedFrames = 0;

	lastTime = SDL_GetTicks();

	passedFrames++;
	if (lastTime < SDL_GetTicks() - FPS_INTERVAL * 1000)
	{
		lastTime = SDL_GetTicks();
		currentFps = passedFrames;
		passedFrames = 0;
	}

	return currentFps;
}

void PsyX_SetSwapInterval(int interval)
{
	g_swapInterval = interval;
}

void PsyX_EnableSwapInterval(int enable)
{
	g_enableSwapInterval = enable;
}

void PsyX_WaitForTimestep(int count)
{
#if 0 // defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	glFinish(); // best time to complete GPU drawing
#endif

	// wait for vblank
	if (!g_skipSwapInterval)
	{	
		static int swapLastVbl = 0;

		int vbl;
		do
		{
#ifdef __EMSCRIPTEN__
			emscripten_sleep(0);
#endif
			vbl = PsyX_Sys_GetVBlankCount();
		}
		while (vbl - swapLastVbl < count);

		swapLastVbl = PsyX_Sys_GetVBlankCount();
	}
}

void PsyX_Exit()
{
	g_psyxNormalExit = 1;
	fprintf(stderr, "[PsyX] PsyX_Exit() called — normal game exit.\n");
	fflush(stderr);
	exit(0);
}

/* Stage markers: every normal close currently ends in std::terminate (an
 * exception escaping somewhere below, under exit()/atexit). stderr is
 * unbuffered, so the last marker printed before the FATAL line in the log
 * names the throwing step. Remove once the thrower is found and fixed. */
#define SHUTDOWN_STAGE(name) do { fprintf(stderr, "[PsyX] PsyX_Shutdown: %s\n", name); fflush(stderr); } while (0)

void PsyX_Shutdown()
{
	fprintf(stderr, "[PsyX] PsyX_Shutdown() called (g_window=%p).\n", (void*)g_window);
	fflush(stderr);
	/* This is the intentional teardown path (MainLoop returned, incl. Esc warm-
	 * reboot which doesn't go through PsyX_Exit). Mark the exit normal so the
	 * benign teardown exception takes the clean _Exit(0) branch instead of the
	 * FATAL+abort that scared users at the end of ending-map sessions. */
	g_psyxNormalExit = 1;
	if (!g_window)
		return;

	// quit vblank thread
	if (g_intrThread)
	{
		g_stopIntrThread = 1;

		int returnValue;
		SDL_WaitThread(g_intrThread, &returnValue);

		SDL_DestroyMutex(g_intrMutex);
	}
	SHUTDOWN_STAGE("vblank thread joined");

	SDL_DestroyWindow(g_window);
	g_window = NULL;
	SHUTDOWN_STAGE("window destroyed");

	GR_Shutdown();
	SHUTDOWN_STAGE("GR_Shutdown done");

	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

	SDL_Quit();
	SHUTDOWN_STAGE("SDL_Quit done");

	UnInstallExceptionHandler();
	SHUTDOWN_STAGE("exception handler uninstalled");

	PsyX_Log_Finalise();
}
