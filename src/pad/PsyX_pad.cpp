#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX_pad.h"
#include "PsyX/PsyX_public.h"

#include <string.h>

extern "C"
{
extern int g_padCommEnable;
}

typedef struct
{
	Sint32				deviceId;	// linked device Id
	SDL_GameController* gc;

	u_char*				padData;
	bool				switchingAnalog;
	u_short				hystWord[2]; /* PC: per-mapping Schmitt-trigger latch (analog->digital anti-chatter) */
} PsyXController;

int						g_cfg_controllerToSlotMapping[MAX_CONTROLLERS] = { -1, -1 };

/* PC port: movement source for the controller. 0 = analog stick only,
 * 1 = d-pad only (digital), 2 = both (default). Set from config in main_pc.c.
 * Drives whether the emulated pad sits in analog (0x73) or digital (0x41) mode. */
int						g_cfg_controllerMovement = 2;

PsyXController			g_controllers[MAX_CONTROLLERS];

/* PSX PadSetAct semantics: the game registers a LIVE actuator buffer once
 * and the pad driver transmits its current bytes to the controller every
 * vsync; the game then just mutates the bytes in place (Silent Hill's
 * vibration engine repacks them per frame in func_8009E718). A fire-once
 * PadSetAct loses every later value change, so register here and
 * retransmit from PsyX_Pad_InternalPadUpdates. */
static unsigned char*	g_actBufTable[MAX_CONTROLLERS];
static int				g_actBufLen[MAX_CONTROLLERS];
const u_char*			g_sdlKeyboardState = NULL;

u_short PsyX_Pad_UpdateKeyboardInput();
void	PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad);

// Initializes SDL controllers
int PsyX_Pad_InitSystem()
{
	// do not init second time!
	if (g_sdlKeyboardState != NULL)
		return 1;

	memset(g_controllers, 0, sizeof(g_controllers));

	// init keyboard state
	g_sdlKeyboardState = SDL_GetKeyboardState(NULL);

	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
	{
		eprinterr("Failed to initialise SDL GameController subsystem!\n");
		return 0;
	}

	// Add more controllers from custom file
	SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");

	return 1;
}

// Prints controller list into console
void PsyX_Pad_Debug_ListControllers()
{
	int numJoysticks = SDL_NumJoysticks();
	int numHaptics = SDL_NumHaptics();

	if (numJoysticks)
	{
		eprintf("SDL GameController list:\n");

		for (int i = 0; i < numJoysticks; i++)
		{
			if (SDL_IsGameController(i))
			{
				eprintinfo("  %d '%s'\n", i, SDL_GameControllerNameForIndex(i));
			}
		}
	}
	else
		eprintwarn("No SDL GameControllers found!\n");

	if (numHaptics)
	{
		eprintf("SDL haptic list:\n");

		for (int i = 0; i < numHaptics; i++)
		{
			eprintinfo("  %d '%s'\n", i, SDL_HapticName(i));
		}
	}
	else
		eprintwarn("No SDL haptics found!\n");
}

// Opens specific system controller and assigns to specified slot
void PsyX_Pad_OpenController(Sint32 deviceId, int slot)
{
	PsyXController* controller = &g_controllers[slot];

	if (controller->gc)
	{
		return;
	}

	controller->gc = SDL_GameControllerOpen(deviceId);
	controller->switchingAnalog = false;

	if (controller->gc)
	{
		// assign device id automatically
		if (controller->deviceId == -1)
			controller->deviceId = deviceId;
	}
}

// Closes controller in specific slot
void PsyX_Pad_CloseController(int slot)
{
	PsyXController* controller = &g_controllers[slot];
	SDL_GameControllerClose(controller->gc);

	controller->gc = NULL;
}

// Called from LIBPAD
void PsyX_Pad_InitPad(int slot, u_char* padData)
{
	PsyXController* controller = &g_controllers[slot];

	controller->padData = padData;
	controller->deviceId = g_cfg_controllerToSlotMapping[slot];

	if (padData)
	{
		LPPADRAW pad = (LPPADRAW)padData;
		
		bool wasConnected = (pad->id == 0x41 || pad->id == 0x73);

		if(!wasConnected)
			pad->id = slot == 0 ? 0x41 : 0xFF;	// since keyboard is a main controller - it's always on

		// only reset buttons
		pad->buttons[0] = 0xFF;
		pad->buttons[1] = 0xFF;
		pad->analog[0] = 128;
		pad->analog[1] = 128;
		pad->analog[2] = 128;
		pad->analog[3] = 128;
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	// reinitialize haptics (why we still here?)
	SDL_QuitSubSystem(SDL_INIT_HAPTIC);			// FIXME: this will crash if you already have haptics
	SDL_InitSubSystem(SDL_INIT_HAPTIC);

	PsyX_Pad_Debug_ListControllers();

	// find mapping and open
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == -1 || controller->deviceId == deviceId)
		{
			PsyX_Pad_OpenController(deviceId, i);
			break;
		}
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	PsyX_Pad_Debug_ListControllers();

	// find mapping and close
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == deviceId)
		{
			PsyX_Pad_CloseController(i);
		}
	}
}

void PsyX_Pad_InternalPadUpdates()
{
	PsyXController* controller;
	LPPADRAW pad;
	u_short kbInputs;

	if (g_padCommEnable == 0)
		return;

	kbInputs = PsyX_Pad_UpdateKeyboardInput();

	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->padData)
		{
			pad = (LPPADRAW)controller->padData;

			PsyX_Pad_UpdateGameControllerInput(controller, pad);

			// Retransmit the registered actuator buffer (PSX pad driver
			// behavior) so in-place value changes by the game reach SDL.
			if (g_actBufTable[i] && g_actBufLen[i] > 0 && controller->gc)
				PsyX_Pad_Vibrate(0, i, g_actBufTable[i], g_actBufLen[i]);

			// PC port: analog mode is config-driven (controller_movement) rather
			// than the original Select+Start manual toggle. analog/both -> 0x73
			// (left stick active), dpad -> 0x41 (digital, stick ignored). Only
			// when a real controller is attached; keyboard stays digital below.
			if (controller->gc && SDL_GameControllerGetAttached(controller->gc))
			{
				pad->id = (g_cfg_controllerMovement == 1) ? 0x41 : 0x73;
			}

			// Update keyboard for PAD
			if ((g_activeKeyboardControllers & (1 << i)) && kbInputs != 0xffff)
			{
				pad->status = 0;	// PadStateStable?

				if (pad->id != 0x41)
				{
					if(pad->id != 0x73)
						eprintf("Port %d ANALOG: OFF\n", i + 1);

					pad->id = 0x41; // force disable analog
				}

				*(u_short*)pad->buttons &= kbInputs;
			}
		}
	}

#if defined(__ANDROID__)
	///@TODO SDL_NumJoysticks always reports > 0 for some reason on Android.
#endif
}


int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis); /* defined below */

extern "C" int PsyX_Pad_SkipButtonHeld(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (!gc)
			continue;

		/* Route the FMV/blocking-loop "skip" through the configured Action/Start
		 * binds (primary + alternate) instead of hardcoded A/Start, so a rebound
		 * controller still skips. */
		if (GetControllerButtonState(gc, g_cfg_controllerMapping.gc_cross)  > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping2.gc_cross) > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping.gc_start)  > 16384)
			return 1;
	}

	return 0;
}

int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis)
{
	if (buttonOrAxis & CONTROLLER_MAP_FLAG_AXIS)
	{
		int value = SDL_GameControllerGetAxis(cont, (SDL_GameControllerAxis)(buttonOrAxis & ~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE)));

		if (abs(value) > 500 && (buttonOrAxis & CONTROLLER_MAP_FLAG_INVERSE))
			value *= -1;

		return value;
	}

	return SDL_GameControllerGetButton(cont, (SDL_GameControllerButton)buttonOrAxis) * 32767;
}

/* PC port: is an SDL game-controller button held on ANY attached physical
 * controller? Read straight from SDL, NOT the keyboard-merged PSX pad word, so a
 * keyboard key mapped to the same PSX button cannot trigger a controller-only
 * action (e.g. the Change-Camera pad bind). sdlGameControllerButton < 0 = unbound. */
extern "C" int PsyX_RawControllerButtonHeld(int sdlGameControllerButton)
{
	int i;
	if (sdlGameControllerButton < 0)
		return 0;
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc) &&
		    SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)sdlGameControllerButton))
			return 1;
	}
	return 0;
}

/* PC port: Schmitt-trigger digitization. An analog input (trigger/stick) mapped to a
   button presses only above HIGH and releases only below LOW, so a value wavering near a
   single 50% threshold can't chatter the digital bit -- that chatter double-fired the gun
   on analog triggers. Digital buttons report 0/32767 and clear both thresholds (unaffected).
   prevWord is the previous frame's post-hysteresis word for this mapping. */
static inline bool PadBtnPressed(SDL_GameController* cont, int getter, u_short prevWord, u_short bit)
{
	int v = GetControllerButtonState(cont, getter);
	bool was = (prevWord & bit) == 0; /* active-low: clear bit = was pressed */
	return was ? (v > 8000) : (v > 24000);
}

/* Build the active-low 16-bit PSX button word from one controller mapping (with hysteresis). */
static u_short PsyX_Pad_BuildPadWord(SDL_GameController* cont, const PsyXControllerMapping& mapping, u_short prevWord)
{
	u_short ret = 0xFFFF;
	if (PadBtnPressed(cont, mapping.gc_square,     prevWord, 0x8000)) ret &= ~0x8000; //Square
	if (PadBtnPressed(cont, mapping.gc_circle,     prevWord, 0x2000)) ret &= ~0x2000; //Circle
	if (PadBtnPressed(cont, mapping.gc_triangle,   prevWord, 0x1000)) ret &= ~0x1000; //Triangle
	if (PadBtnPressed(cont, mapping.gc_cross,      prevWord, 0x4000)) ret &= ~0x4000; //Cross
	if (PadBtnPressed(cont, mapping.gc_l1,         prevWord, 0x400))  ret &= ~0x400;  //L1
	if (PadBtnPressed(cont, mapping.gc_r1,         prevWord, 0x800))  ret &= ~0x800;  //R1
	if (PadBtnPressed(cont, mapping.gc_l2,         prevWord, 0x100))  ret &= ~0x100;  //L2
	if (PadBtnPressed(cont, mapping.gc_r2,         prevWord, 0x200))  ret &= ~0x200;  //R2
	if (PadBtnPressed(cont, mapping.gc_dpad_up,    prevWord, 0x10))   ret &= ~0x10;   //UP
	if (PadBtnPressed(cont, mapping.gc_dpad_down,  prevWord, 0x40))   ret &= ~0x40;   //DOWN
	if (PadBtnPressed(cont, mapping.gc_dpad_left,  prevWord, 0x80))   ret &= ~0x80;   //LEFT
	if (PadBtnPressed(cont, mapping.gc_dpad_right, prevWord, 0x20))   ret &= ~0x20;   //RIGHT
	if (PadBtnPressed(cont, mapping.gc_l3,         prevWord, 0x2))    ret &= ~0x2;    //L3
	if (PadBtnPressed(cont, mapping.gc_r3,         prevWord, 0x4))    ret &= ~0x4;    //R3
	if (PadBtnPressed(cont, mapping.gc_select,     prevWord, 0x1))    ret &= ~0x1;    //SELECT
	if (PadBtnPressed(cont, mapping.gc_start,      prevWord, 0x8))    ret &= ~0x8;    //START
	return ret;
}

void PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad)
{
	SDL_GameController* cont = controller->gc;
	short leftX, leftY, rightX, rightY;
	u_short ret;

	if (!cont)
	{
		pad->analog[0] = 127;
		pad->analog[1] = 127;
		pad->analog[2] = 127;
		pad->analog[3] = 127;

		*(u_short*)pad->buttons = 0xFFFF;
		return;
	}

	/* Primary binds AND the secondary (second-button-per-action) binds: active-low,
	 * so an action reads pressed if EITHER mapping clears its bit. Analog sticks come
	 * from the primary mapping's axes only. */
	u_short w1 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping,  controller->hystWord[0]);
	u_short w2 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping2, controller->hystWord[1]);
	controller->hystWord[0] = w1;
	controller->hystWord[1] = w2;
	ret = w1 & w2;

	leftX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_x);
	leftY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_y);

	rightX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_x);
	rightY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_y);

	*(u_short*)pad->buttons = ret;

	// map to range
	pad->analog[0] = (rightX / 256) + 128;
	pad->analog[1] = (rightY / 256) + 128;
	pad->analog[2] = (leftX / 256) + 128;
	pad->analog[3] = (leftY / 256) + 128;
}

static u_short PsyX_Pad_BuildKbWord(const PsyXKeyboardMapping& mapping)
{
	u_short ret = 0xFFFF;

	if (g_sdlKeyboardState[mapping.kc_square])     ret &= ~0x8000;//Square
	if (g_sdlKeyboardState[mapping.kc_circle])     ret &= ~0x2000;//Circle
	if (g_sdlKeyboardState[mapping.kc_triangle])   ret &= ~0x1000;//Triangle
	if (g_sdlKeyboardState[mapping.kc_cross])      ret &= ~0x4000;//Cross
	if (g_sdlKeyboardState[mapping.kc_l1])         ret &= ~0x400; //L1
	if (g_sdlKeyboardState[mapping.kc_l2])         ret &= ~0x100; //L2
	if (g_sdlKeyboardState[mapping.kc_l3])         ret &= ~0x2;   //L3
	if (g_sdlKeyboardState[mapping.kc_r1])         ret &= ~0x800; //R1
	if (g_sdlKeyboardState[mapping.kc_r2])         ret &= ~0x200; //R2
	if (g_sdlKeyboardState[mapping.kc_r3])         ret &= ~0x4;   //R3
	if (g_sdlKeyboardState[mapping.kc_dpad_up])    ret &= ~0x10;  //UP
	if (g_sdlKeyboardState[mapping.kc_dpad_down])  ret &= ~0x40;  //DOWN
	if (g_sdlKeyboardState[mapping.kc_dpad_left])  ret &= ~0x80;  //LEFT
	if (g_sdlKeyboardState[mapping.kc_dpad_right]) ret &= ~0x20;  //RIGHT
	if (g_sdlKeyboardState[mapping.kc_select])     ret &= ~0x1;   //SELECT
	if (g_sdlKeyboardState[mapping.kc_start])      ret &= ~0x8;   //START

	return ret;
}

/* Mouse buttons -> PSX button word (active-low). Each pressed SDL mouse button
 * 1..5 clears whatever PSX bits the config bound it to (g_cfg_mouseButtonMask). */
static u_short PsyX_Pad_BuildMouseWord()
{
	u_short ret = 0xFFFF;
	Uint32  mb  = SDL_GetMouseState(NULL, NULL);
	int     b;

	for (b = 1; b <= 5; b++)
	{
		if ((mb & SDL_BUTTON(b)) && g_cfg_mouseButtonMask[b])
			ret &= ~g_cfg_mouseButtonMask[b];
	}
	return ret;
}

u_short PsyX_Pad_UpdateKeyboardInput()
{
	u_short ret;

	//Not initialised yet
	if (g_sdlKeyboardState == NULL)
		return 0xFFFF;

	SDL_PumpEvents();

	ret = PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping);

	/* Secondary key binds + mouse buttons (gated by allow_mouse_secondary,
	 * forced on in TPS). Active-low: a button is pressed if clear in ANY
	 * source, so the layers combine with AND. */
	if (g_cfg_allowMouseSecondary)
	{
		ret &= PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping2);
		ret &= PsyX_Pad_BuildMouseWord();
	}

	return ret;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
	PsyXController* controller;

	if (slot == 0)
		return 1;	// keyboard always here

	controller = &g_controllers[slot];

	if (controller->gc && SDL_GameControllerGetAttached(controller->gc))
		return 1;

	return 0;
}

void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len)
{
	PsyXController* controller = &g_controllers[slot];

	if (len == 0)
		return;

	Uint16 freq_high	= table[0] * 255;
	Uint16 freq_low		= len > 1 ? table[1] * 255 : 0;

	// apply minimal shake
	if(freq_low != 0 && freq_low < 4096)
		freq_low = 4096;

	if (freq_high != 0 && freq_high < 4096)
		freq_high = 4096;

	SDL_GameControllerRumble(controller->gc, freq_low, freq_high, 200);
}

void PsyX_Pad_SetActBuffer(int slot, unsigned char* table, int len)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	g_actBufTable[slot] = table;
	g_actBufLen[slot]   = (table != NULL) ? len : 0;

	if (g_actBufLen[slot] == 0)
	{
		PsyXController* controller = &g_controllers[slot];
		if (controller->gc)
			SDL_GameControllerRumble(controller->gc, 0, 0, 0);
	}
}