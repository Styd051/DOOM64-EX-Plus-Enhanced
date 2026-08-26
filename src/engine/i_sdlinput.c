// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 1993-1997 Id Software, Inc.
// Copyright(C) 2007-2012 Samuel Villarreal
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//    SDL Input
//
//-----------------------------------------------------------------------------

#include <math.h>

#include "i_sdlinput.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "d_main.h"
#include "con_cvar.h"
#include "dgl.h"
#include "g_settings.h"
#include "g_actions.h"

CVAR(v_msensitivityx, 5);
CVAR(v_msensitivityy, 5);
CVAR(v_macceleration, 0);
CVAR(v_mlookinvert, 0);
CVAR(v_yaxismove, 0);
CVAR(v_xaxismove, 0);
CVAR_EXTERNAL(i_interpolateframes);
CVAR_EXTERNAL(m_menumouse);
CVAR_EXTERNAL(p_autoaim);
CVAR_EXTERNAL(v_maxfps);
CVAR_EXTERNAL(v_vsync);

CVAR_CMD(v_mlook, 0) {
	if (cvar->value > 0) {
		if(!g_in_load_settings) {
			I_Printf("WARNING: mouse look: skies will not render properly with high pitch. Do not report.\n");
		}
		return;
	}

	// mlook is disabled

	// center player view, resetting pitch
	if (gamestate == GS_LEVEL) {
		players[0].mo->pitch = 0;
	}

	// force autoaim
	gameflags |= GF_ALLOWAUTOAIM;
};


float mouse_accelfactor;

int	DualMouse;

boolean	DigiJoy;
boolean	MouseMode;
boolean	window_mouse;

gamepad64_t gamepad64;

//
// SDL3 Gamepad
// [styd] 2026 - reworked
//
// The previous implementation synthesised keyboard presses: pushing the
// left stick forward posted SDLK_W, the south face button posted SDLK_E,
// and so on. That had three consequences.
//
//   - The pad was not rebindable. The keys were #defines in this file.
//   - It broke as soon as the player rebound anything. On an AZERTY
//     keyboard, where forward is usually Z rather than W, the stick moved
//     nothing at all.
//   - Look was pushed through the mouse path, so v_msensitivity and
//     I_MouseAccel were applied to it a second time in G_DoCmdMouseMove,
//     and v_mlookinvert was applied twice, which cancelled out.
//
// The pad now reports itself as a pad: button indices travel as
// ev_gamepaddown / ev_gamepadup through GamepadActions[], exactly like keys
// travel through KeyActions[], so every button is bindable from the same
// menu and saved to the same config. The sticks are handed to
// G_SetGamepadAxes as normalised positions and read once, in G_BuildTiccmd.
//

CVAR(v_gamepad, 1);
CVAR(v_gamepadsensx, 5);
CVAR(v_gamepadsensy, 5);
CVAR(v_gamepadinvert, 0);
CVAR(v_gamepaddeadzone, 18);
CVAR(v_gamepadlayout, 0);
CVAR(v_gamepadrumble, 5);

// menu navigation repeat, in tics
#define GAMEPAD_MENU_INITIAL_DELAY_TICS  12
#define GAMEPAD_MENU_REPEAT_TICS         4

// how far the left stick must lean before it counts as a menu direction
#define GAMEPAD_MENU_STICK_THRESH        0.50f

//
// Stick response.
//
// The inner dead zone comes from v_gamepaddeadzone so that a worn stick can
// be compensated without a rebuild. The rest is fixed: an outer dead zone so
// that a stick which cannot quite reach the corners still reads as fully
// deflected, a small anti dead zone so the first movement past the threshold
// is felt rather than lost, and an exponent that gives fine control near the
// centre. The right stick gets a slightly larger dead zone and a steeper
// curve because aiming punishes jitter more than walking does.
//
#define GAMEPAD_OUTER_DZ         0.02f
#define GAMEPAD_ANTI_DZ          0.04f
#define GAMEPAD_EXPO_LEFT        1.20f
#define GAMEPAD_EXPO_RIGHT       1.60f
#define GAMEPAD_RIGHT_DZ_BIAS    0.02f
#define GAMEPAD_MAX_DEADZONE     0.40f

//
// Low pass on the look axes, in seconds. Asymmetric on purpose.
//
// A single time constant smooths the stick coming back to centre as much as
// it smooths it leaving, so the view keeps drifting for a moment after the
// player lets go. Immorpher put it exactly right while testing: "there seems
// to be a delay when the player stops turning".
//
// Smoothing earns its place while a deflection is being held or increased,
// where it takes the tremble out of fine aim. It earns nothing on the way
// back down, so the release constant is short enough to be a single tic, and
// a stick that has returned inside its dead zone snaps to rest with no
// filtering at all.
//
#define GAMEPAD_LOOK_ATTACK_TC   0.060f
#define GAMEPAD_LOOK_RELEASE_TC  0.012f

//
// Triggers are analog on SDL's side and are presented here as two virtual
// buttons. The two thresholds give hysteresis: without it a trigger resting
// near the threshold chatters, and a chattering trigger bound to +fire
// empties a magazine.
//
#define GAMEPAD_TRIGGER_PRESS    0.30f
#define GAMEPAD_TRIGGER_RELEASE  0.22f

#define GAMEPAD_MASK_WORDS       ((NUM_GAMEPADBTNS + 31) / 32)

extern void D_PostEvent(event_t*);
extern boolean menuactive;
extern gamestate_t gamestate;

//
// The button indices in g_controls.h are declared without including SDL, so
// that m_menu.c and g_actions.c do not have to. This is the one place that
// sees both, so this is where they are checked. If a future SDL renumbers
// its buttons or grows the list, the build stops here instead of the pad
// quietly acting on the wrong ones.
//
SDL_COMPILE_TIME_ASSERT(gpbtn_count, GAMEPADBTN_SDL_COUNT == SDL_GAMEPAD_BUTTON_COUNT);
SDL_COMPILE_TIME_ASSERT(gpbtn_south, GAMEPADBTN_SOUTH == SDL_GAMEPAD_BUTTON_SOUTH);
SDL_COMPILE_TIME_ASSERT(gpbtn_east, GAMEPADBTN_EAST == SDL_GAMEPAD_BUTTON_EAST);
SDL_COMPILE_TIME_ASSERT(gpbtn_west, GAMEPADBTN_WEST == SDL_GAMEPAD_BUTTON_WEST);
SDL_COMPILE_TIME_ASSERT(gpbtn_north, GAMEPADBTN_NORTH == SDL_GAMEPAD_BUTTON_NORTH);
SDL_COMPILE_TIME_ASSERT(gpbtn_back, GAMEPADBTN_BACK == SDL_GAMEPAD_BUTTON_BACK);
SDL_COMPILE_TIME_ASSERT(gpbtn_start, GAMEPADBTN_START == SDL_GAMEPAD_BUTTON_START);
SDL_COMPILE_TIME_ASSERT(gpbtn_lb, GAMEPADBTN_LSHOULDER == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(gpbtn_rb, GAMEPADBTN_RSHOULDER == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(gpbtn_up, GAMEPADBTN_DPAD_UP == SDL_GAMEPAD_BUTTON_DPAD_UP);
SDL_COMPILE_TIME_ASSERT(gpbtn_down, GAMEPADBTN_DPAD_DOWN == SDL_GAMEPAD_BUTTON_DPAD_DOWN);
SDL_COMPILE_TIME_ASSERT(gpbtn_left, GAMEPADBTN_DPAD_LEFT == SDL_GAMEPAD_BUTTON_DPAD_LEFT);
SDL_COMPILE_TIME_ASSERT(gpbtn_right, GAMEPADBTN_DPAD_RIGHT == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

//
// menu directions, in the order the repeat timers are stored
//
static const int gamepad_menudir[4] = {
	SDL_GAMEPAD_BUTTON_DPAD_UP,
	SDL_GAMEPAD_BUTTON_DPAD_DOWN,
	SDL_GAMEPAD_BUTTON_DPAD_LEFT,
	SDL_GAMEPAD_BUTTON_DPAD_RIGHT
};

static SDL_INLINE float I_GamepadClamp(float x) { return SDL_clamp(x, 0.f, 1.f); }

static SDL_INLINE void I_GamepadSetBit(unsigned int* mask, int btn) {
	mask[btn >> 5] |= 1u << (btn & 31);
}

static SDL_INLINE bool I_GamepadTestBit(const unsigned int* mask, int btn) {
	return ((mask[btn >> 5] >> (btn & 31)) & 1u) != 0u;
}

//
// I_GamepadDeadZone
//
// v_gamepaddeadzone is a percentage so it reads sensibly on a menu slider.
// Clamped well short of 1.0: a dead zone approaching full deflection would
// make the stick appear broken, and the player has no way back if the menu
// itself needs the stick to navigate.
//
static float I_GamepadDeadZone(void) {
	float dz = (float)v_gamepaddeadzone.value * 0.01f;
	return SDL_clamp(dz, 0.f, GAMEPAD_MAX_DEADZONE);
}

//
// I_GamepadRadialCurve
//
// Radial rather than per axis, so the dead zone is a circle. Per axis dead
// zones leave a cross shaped hole where a diagonal push registers on only
// one axis, which reads as the stick snapping to the cardinals.
//
static void I_GamepadRadialCurve(float x, float y,
	float inner_dz, float outer_dz,
	float expo, float anti_dz,
	float* ox, float* oy)
{
	float r = SDL_sqrtf(x * x + y * y);
	float nx, ny, span, t;

	if (r <= inner_dz) { *ox = 0.f; *oy = 0.f; return; }

	nx = x / (r > 0.f ? r : 1.f);
	ny = y / (r > 0.f ? r : 1.f);
	span = 1.f - inner_dz - outer_dz;
	t = I_GamepadClamp((r - inner_dz) / (span > 0.f ? span : 1.f));

	t = SDL_powf(t, expo);
	t = anti_dz + (1.f - anti_dz) * t;

	*ox = nx * t;
	*oy = ny * t;
}

//
// I_GamepadLookSmoothing
//
// One pole low pass with a separate constant for rising and falling input.
//
// dt is the tic length rather than the frame time on purpose: the pad is
// polled once per tic, so smoothing against the frame rate would make the
// look speed depend on it.
//
static SDL_INLINE float I_GamepadLookSmoothing(float prev, float input, float dt) {
	float tc;
	float alpha;

	//
	// Stick at rest: stop dead. This is the case the player actually feels,
	// because releasing the stick drops it inside the dead zone and the
	// input becomes exactly zero.
	//
	if (input == 0.0f) {
		return 0.0f;
	}

	//
	// The short constant applies when the stick is coming back towards
	// centre, and also when it crosses centre outright. The magnitude test
	// alone misses that second case: flicking from hard left to hard right
	// leaves the magnitude unchanged, so a comparison of magnitudes reads it
	// as a held deflection and damps the reversal.
	//
	if (SDL_fabsf(input) < SDL_fabsf(prev) || (input * prev) < 0.0f) {
		tc = GAMEPAD_LOOK_RELEASE_TC;
	}
	else {
		tc = GAMEPAD_LOOK_ATTACK_TC;
	}

	alpha = (tc > 0.f) ? (dt / (tc + dt)) : 1.f;
	alpha = SDL_clamp(alpha, 0.f, 1.f);

	return prev + alpha * (input - prev);
}

static SDL_INLINE float I_GamepadAxisNorm(Sint16 v) {
	float f = (float)v / (float)SDL_JOYSTICK_AXIS_MAX;
	return SDL_clamp(f, -1.f, 1.f);
}

static SDL_INLINE void I_GamepadPostButton(int btn, bool down) {
	event_t ev;

	ev.type = down ? ev_gamepaddown : ev_gamepadup;
	ev.data1 = btn;
	ev.data2 = ev.data3 = 0.0f;
	ev.data4 = 0;

	D_PostEvent(&ev);
}

//
// I_GamepadAnyHeld
//
// Never test the mask words by hand: NUM_GAMEPADBTNS currently rounds up to
// a single 32 bit word, so held[1] is off the end of the array, and the
// count changes whenever SDL adds a button.
//
static bool I_GamepadAnyHeld(void) {
	int i;

	for (i = 0; i < GAMEPAD_MASK_WORDS; i++) {
		if (gamepad64.held[i]) {
			return true;
		}
	}

	return false;
}

//
// I_GamepadReleaseAll
//
// Posts a release for every button still held and clears the axes.
//
// Needed whenever input stops being observed: unplugging the pad, or losing
// window focus. Without it a button held at that moment stays latched in
// the action system - the classic symptom being the player running forward
// forever after alt-tabbing.
//
static void I_GamepadReleaseAll(void) {
	int i;

	for (i = 0; i < NUM_GAMEPADBTNS; i++) {
		if (I_GamepadTestBit(gamepad64.held, i)) {
			I_GamepadPostButton(i, false);
		}
	}

	SDL_memset(gamepad64.held, 0, sizeof(gamepad64.held));
	SDL_memset(gamepad64.menu_repeat, 0, sizeof(gamepad64.menu_repeat));
	SDL_memset(gamepad64.menu_held, 0, sizeof(gamepad64.menu_held));

	gamepad64.look_fx = gamepad64.look_fy = 0.f;

	// a pad that stops being watched must not keep buzzing
	if (gamepad64.gamepad) {
		SDL_RumbleGamepad(gamepad64.gamepad, 0, 0, 0);
	}

	G_SetGamepadAxes(0.f, 0.f, 0.f, 0.f);
}

//
// I_GamepadLoadMappings
//
// SDL ships a large built in mapping database, so most pads work with no
// help. This picks up gamecontrollerdb.txt if the player dropped one next to
// the executable or in the user directory, which is how an unusual or very
// new pad gets named axes and buttons instead of the raw joystick fallback.
//
static void I_GamepadLoadMappings(void) {
	char* path = I_FindDataFile("gamecontrollerdb.txt"); // must not free
	int added;

	if (!path) {
		return;
	}

	added = SDL_AddGamepadMappingsFromFile(path);

	if (added > 0) {
		I_Printf("I_GamepadLoadMappings: %d mappings from %s\n", added, path);
	}
}

//
// I_GamepadOpen
//
static void I_GamepadOpen(SDL_JoystickID id) {
	if (gamepad64.gamepad || gamepad64.joy) {
		return;
	}

	if (SDL_IsGamepad(id)) {
		gamepad64.gamepad = SDL_OpenGamepad(id);

		if (gamepad64.gamepad) {
			gamepad64.active_id = id;
			I_Printf("I_GamepadOpen: %s\n", I_GamepadName());
			return;
		}
	}

	//
	// Raw joystick fallback, for a device SDL has no mapping for.
	//
	// The old code guessed which physical button was which - button 0 was
	// assumed to be fire, 9 to be start, 10 to 13 to be the d-pad - which
	// is right for almost nothing. Buttons are now passed straight through
	// at their hardware index and the player rebinds them. The names shown
	// in the menu will be the standard ones and will not match the labels
	// on the device, which is honest: without a mapping, nothing here knows
	// what the labels are.
	//
	gamepad64.joy = SDL_OpenJoystick(id);

	if (gamepad64.joy) {
		gamepad64.active_id = id;
		I_Printf("I_GamepadOpen: %s (no mapping, raw joystick)\n",
			I_GamepadName());
	}
}

//
// I_GamepadClose
//
static void I_GamepadClose(void) {
	I_GamepadReleaseAll();

	if (gamepad64.gamepad) {
		SDL_CloseGamepad(gamepad64.gamepad);
		gamepad64.gamepad = NULL;
	}

	if (gamepad64.joy) {
		SDL_CloseJoystick(gamepad64.joy);
		gamepad64.joy = NULL;
	}

	gamepad64.active_id = 0;
}

//
// I_GamepadPickAny
//
// Opens the first suitable device that is currently attached, preferring one
// SDL has a mapping for.
//
// Called at startup and again after the active pad is unplugged: without the
// second call, pulling one pad out of a machine with two attached left the
// player with none, because nothing raises an ADDED event for a device that
// was already there.
//
static void I_GamepadPickAny(void) {
	int n = 0;
	SDL_JoystickID* ids;
	int i;

	if (gamepad64.gamepad || gamepad64.joy) {
		return;
	}

	// SDL_GetJoysticks covers both, since every gamepad is also a joystick
	ids = SDL_GetJoysticks(&n);

	if (!ids) {
		return;
	}

	for (i = 0; i < n && !gamepad64.gamepad; i++) {
		if (SDL_IsGamepad(ids[i])) {
			I_GamepadOpen(ids[i]);
		}
	}

	if (!gamepad64.gamepad && !gamepad64.joy && n > 0) {
		I_GamepadOpen(ids[0]);
	}

	SDL_free(ids);
}

//
// I_GamepadInitOnce
//
static void I_GamepadInitOnce(void) {
	if (gamepad64.init) {
		return;
	}

	//
	// Hints have to be set before the subsystem starts.
	//
	// SDL leaves several HIDAPI drivers off by default on Windows. The PS3
	// one in particular is off everywhere except macOS, which is why a PS3
	// pad is named correctly in the menu and then moves nothing: SDL sees
	// the device but has no driver bound to it. Asking for the driver is
	// free, and on a machine where it cannot be claimed SDL simply carries
	// on as before.
	//
	// This will not rescue every pad. An adapter that misreports itself -
	// the N64 adapter that announces itself as an SNES controller - is
	// wrong before SDL ever sees it, and only an entry in
	// gamecontrollerdb.txt can straighten that out.
	//
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");

	if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
		SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	}

	I_GamepadLoadMappings();

	I_GamepadPickAny();

	gamepad64.init = true;
}

//
// I_GamepadHandleSDLEvent
//
static void I_GamepadHandleSDLEvent(const SDL_Event* e) {
	if (!gamepad64.init) {
		return;
	}

	switch (e->type) {
	case SDL_EVENT_GAMEPAD_ADDED:
		I_GamepadOpen(e->gdevice.which);
		break;

	case SDL_EVENT_JOYSTICK_ADDED:
		// a device with a mapping also raises SDL_EVENT_GAMEPAD_ADDED, so
		// only take it here if it has none
		if (!SDL_IsGamepad(e->jdevice.which)) {
			I_GamepadOpen(e->jdevice.which);
		}
		break;

	case SDL_EVENT_GAMEPAD_REMOVED:
		if (gamepad64.active_id == e->gdevice.which) {
			I_GamepadClose();
			I_GamepadPickAny();
		}
		break;

	case SDL_EVENT_JOYSTICK_REMOVED:
		if (gamepad64.active_id == e->jdevice.which) {
			I_GamepadClose();
			I_GamepadPickAny();
		}
		break;

	default:
		break;
	}
}

//
// I_GamepadPresent
//
boolean I_GamepadPresent(void) {
	if (!(int)v_gamepad.value) {
		return false;
	}

	return (gamepad64.gamepad || gamepad64.joy) ? true : false;
}

//
// I_GamepadName
//
const char* I_GamepadName(void) {
	const char* name = NULL;

	if (gamepad64.gamepad) {
		name = SDL_GetGamepadName(gamepad64.gamepad);
	}
	else if (gamepad64.joy) {
		name = SDL_GetJoystickName(gamepad64.joy);
	}

	return name;
}

//
// I_GamepadRumble
// [styd]
//
// strength is 0.0 to 1.0, duration in milliseconds.
//
// Scaled by v_gamepadrumble, which is a 0 to 10 slider, so 0 turns the
// motors off entirely without the caller having to know. The low frequency
// motor is driven harder than the high frequency one: on an Xbox pad the
// heavy motor reads as impact, the light one as buzz, and a hit should feel
// like the former.
//
// SDL_RumbleGamepad replaces any rumble already playing rather than adding
// to it, so a burst of small hits will not stack into a long vibration.
//
void I_GamepadRumble(float strength, int duration_ms) {
	float scale;
	Uint16 low, high;

	if (!I_GamepadPresent() || !gamepad64.gamepad) {
		return;
	}

	scale = SDL_clamp((float)v_gamepadrumble.value * 0.1f, 0.f, 1.f);

	if (scale <= 0.f || duration_ms <= 0) {
		return;
	}

	strength = SDL_clamp(strength, 0.f, 1.f) * scale;

	low = (Uint16)(strength * 65535.0f);
	high = (Uint16)(strength * 0.45f * 65535.0f);

	SDL_RumbleGamepad(gamepad64.gamepad, low, high, (Uint32)duration_ms);
}

//
// I_GamepadReadAxes
//
// Raw, before dead zone. Returns false if there is nothing to read.
//
static bool I_GamepadReadAxes(float* lx, float* ly, float* rx, float* ry,
	float* lt, float* rt)
{
	*lx = *ly = *rx = *ry = *lt = *rt = 0.f;

	if (gamepad64.gamepad) {
		*lx = I_GamepadAxisNorm(SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_LEFTX));
		*ly = I_GamepadAxisNorm(SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_LEFTY));
		*rx = I_GamepadAxisNorm(SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
		*ry = I_GamepadAxisNorm(SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_RIGHTY));

		//
		// Triggers, unlike the sticks, are reported by SDL already
		// normalised: 0 when released, SDL_JOYSTICK_AXIS_MAX when fully
		// pressed. They never go negative.
		//
		// I previously "fixed" this by remapping -1..1 onto 0..1, on the
		// assumption that a released trigger rested at the axis minimum
		// like a centred stick. It does not. That remap made a released
		// trigger read 0.5, which sits above the press threshold, so both
		// triggers latched as held at startup and never produced another
		// edge - the pad could not fire at all.
		//
		*lt = SDL_clamp((float)SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
			/ (float)SDL_JOYSTICK_AXIS_MAX, 0.f, 1.f);
		*rt = SDL_clamp((float)SDL_GetGamepadAxis(gamepad64.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
			/ (float)SDL_JOYSTICK_AXIS_MAX, 0.f, 1.f);

		return true;
	}

	if (gamepad64.joy) {
		int n = SDL_GetNumJoystickAxes(gamepad64.joy);

		if (n > 0) { *lx = I_GamepadAxisNorm(SDL_GetJoystickAxis(gamepad64.joy, 0)); }
		if (n > 1) { *ly = I_GamepadAxisNorm(SDL_GetJoystickAxis(gamepad64.joy, 1)); }
		if (n > 2) { *rx = I_GamepadAxisNorm(SDL_GetJoystickAxis(gamepad64.joy, 2)); }
		if (n > 3) { *ry = I_GamepadAxisNorm(SDL_GetJoystickAxis(gamepad64.joy, 3)); }

		// an unmapped device has no agreed trigger axes, so leave them idle
		return true;
	}

	return false;
}

//
// I_GamepadPollButtons
//
// Builds the bitmask of what is held right now.
//
static void I_GamepadPollButtons(unsigned int* now, float lt, float rt) {
	int i;
	float thresh;

	SDL_memset(now, 0, sizeof(unsigned int) * GAMEPAD_MASK_WORDS);

	if (gamepad64.gamepad) {
		for (i = 0; i < GAMEPADBTN_SDL_COUNT; i++) {
			if (SDL_GetGamepadButton(gamepad64.gamepad, (SDL_GamepadButton)i)) {
				I_GamepadSetBit(now, i);
			}
		}
	}
	else if (gamepad64.joy) {
		int n = SDL_GetNumJoystickButtons(gamepad64.joy);

		if (n > GAMEPADBTN_SDL_COUNT) {
			n = GAMEPADBTN_SDL_COUNT;
		}

		for (i = 0; i < n; i++) {
			if (SDL_GetJoystickButton(gamepad64.joy, i)) {
				I_GamepadSetBit(now, i);
			}
		}

		// a hat is the d-pad on most unmapped devices, so fold it in
		if (SDL_GetNumJoystickHats(gamepad64.joy) > 0) {
			Uint8 hat = SDL_GetJoystickHat(gamepad64.joy, 0);

			if (hat & SDL_HAT_UP) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_UP); }
			if (hat & SDL_HAT_DOWN) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_DOWN); }
			if (hat & SDL_HAT_LEFT) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_LEFT); }
			if (hat & SDL_HAT_RIGHT) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_RIGHT); }
		}
	}

	// triggers as virtual buttons, with hysteresis against the previous state
	thresh = I_GamepadTestBit(gamepad64.held, GAMEPADBTN_LTRIGGER)
		? GAMEPAD_TRIGGER_RELEASE : GAMEPAD_TRIGGER_PRESS;

	if (lt >= thresh) {
		I_GamepadSetBit(now, GAMEPADBTN_LTRIGGER);
	}

	thresh = I_GamepadTestBit(gamepad64.held, GAMEPADBTN_RTRIGGER)
		? GAMEPAD_TRIGGER_RELEASE : GAMEPAD_TRIGGER_PRESS;

	if (rt >= thresh) {
		I_GamepadSetBit(now, GAMEPADBTN_RTRIGGER);
	}
}

//
// I_GamepadEmitEdges
//
static void I_GamepadEmitEdges(const unsigned int* now) {
	int i;

	for (i = 0; i < NUM_GAMEPADBTNS; i++) {
		bool was = I_GamepadTestBit(gamepad64.held, i);
		bool is = I_GamepadTestBit(now, i);

		if (was != is) {
			I_GamepadPostButton(i, is);
		}
	}

	SDL_memcpy(gamepad64.held, now, sizeof(gamepad64.held));
}

//
// I_GamepadMenuRepeat
//
// The menus are driven by discrete presses, so a held direction has to be
// turned into repeats. These are posted as further ev_gamepaddown events
// rather than as arrow keys: M_Responder already translates a pad direction
// into a menu move, and re-using the same event means a repeat arriving
// while the player is binding a control simply re-binds the same button
// instead of binding an arrow key by accident.
//
static void I_GamepadMenuRepeat(const unsigned int* mask) {
	int i;

	for (i = 0; i < 4; i++) {
		bool down = I_GamepadTestBit(mask, gamepad_menudir[i]);

		if (!down) {
			gamepad64.menu_held[i] = false;
			continue;
		}

		if (!gamepad64.menu_held[i]) {
			// the press itself was already posted by I_GamepadEmitEdges
			gamepad64.menu_held[i] = true;
			gamepad64.menu_repeat[i] = gametic + GAMEPAD_MENU_INITIAL_DELAY_TICS;
			continue;
		}

		if (gametic >= gamepad64.menu_repeat[i]) {
			I_GamepadPostButton(gamepad_menudir[i], true);
			gamepad64.menu_repeat[i] = gametic + GAMEPAD_MENU_REPEAT_TICS;
		}
	}
}

//
// I_GamepadUpdate
//
// Called once per tic from I_StartTic.
//
static void I_GamepadUpdate(void) {
	unsigned int now[GAMEPAD_MASK_WORDS];
	float lx_raw, ly_raw, rx_raw, ry_raw, lt, rt;
	float lx, ly, rx, ry;
	float inner_dz;
	bool in_menu;

	if (!gamepad64.init) {
		return;
	}

	//
	// Disabled, unplugged, or the window is in the background.
	//
	// The axes are cleared unconditionally, not only when a button happens
	// to be held. A stick pushed forward at the moment the window loses
	// focus would otherwise leave its last position in Controls, and the
	// player would keep walking in the background with nothing left to
	// clear it.
	//
	if (!I_GamepadPresent() || !window_focused) {
		if (I_GamepadAnyHeld()) {
			I_GamepadReleaseAll();
		}
		else {
			gamepad64.look_fx = gamepad64.look_fy = 0.f;
			G_SetGamepadAxes(0.f, 0.f, 0.f, 0.f);
		}
		return;
	}

	if (!I_GamepadReadAxes(&lx_raw, &ly_raw, &rx_raw, &ry_raw, &lt, &rt)) {
		//
		// Unreachable as things stand, because I_GamepadPresent above has
		// already established that a device is open. Cleared anyway: this is
		// the one remaining way out of this function, and every other one
		// leaves the axes defined. An exit that quietly keeps the last stick
		// position is how a player ends up walking into a wall forever.
		//
		gamepad64.look_fx = gamepad64.look_fy = 0.f;
		G_SetGamepadAxes(0.f, 0.f, 0.f, 0.f);
		return;
	}

	inner_dz = I_GamepadDeadZone();

	I_GamepadRadialCurve(lx_raw, ly_raw, inner_dz, GAMEPAD_OUTER_DZ,
		GAMEPAD_EXPO_LEFT, GAMEPAD_ANTI_DZ, &lx, &ly);
	I_GamepadRadialCurve(rx_raw, ry_raw, inner_dz + GAMEPAD_RIGHT_DZ_BIAS,
		GAMEPAD_OUTER_DZ, GAMEPAD_EXPO_RIGHT, GAMEPAD_ANTI_DZ, &rx, &ry);

	I_GamepadPollButtons(now, lt, rt);

	in_menu = (menuactive || gamestate != GS_LEVEL);

	if (in_menu) {
		//
		// Outside the level the left stick stands in for the d-pad, so the
		// player can drive a menu, an intermission or the cast with either.
		// In game the stick drives analog movement instead, and the d-pad
		// keeps whatever it is bound to, independently.
		//
		if (-ly > GAMEPAD_MENU_STICK_THRESH) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_UP); }
		if (-ly < -GAMEPAD_MENU_STICK_THRESH) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_DOWN); }
		if (lx < -GAMEPAD_MENU_STICK_THRESH) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_LEFT); }
		if (lx > GAMEPAD_MENU_STICK_THRESH) { I_GamepadSetBit(now, SDL_GAMEPAD_BUTTON_DPAD_RIGHT); }
	}

	//
	// The stick means the d-pad on one side of this boundary and analog
	// movement on the other, so crossing it changes what the mask says
	// without the player having moved a finger.
	//
	// Seeding the held mask absorbs that: opening the menu while walking
	// forward no longer counts as pressing d-pad up and jumping the
	// selection, and closing it no longer sends a phantom release into
	// whatever the d-pad is bound to.
	//
	if (in_menu != gamepad64.was_in_menu) {
		SDL_memcpy(gamepad64.held, now, sizeof(gamepad64.held));
		gamepad64.was_in_menu = in_menu;

		SDL_memset(gamepad64.menu_repeat, 0, sizeof(gamepad64.menu_repeat));
		SDL_memset(gamepad64.menu_held, 0, sizeof(gamepad64.menu_held));
	}

	I_GamepadEmitEdges(now);

	if (in_menu) {
		//
		// Auto-repeat is only for a menu that is actually open.
		//
		// It posts a press with no matching release, which a menu absorbs
		// but the action system does not: on the intermission or the cast,
		// where events fall through to the bindings, every repeat would
		// increment the press count of whatever the d-pad is bound to while
		// only one release ever arrived, latching it on.
		//
		if (menuactive) {
			I_GamepadMenuRepeat(now);
		}

		// nobody walks anywhere from inside a menu
		gamepad64.look_fx = gamepad64.look_fy = 0.f;
		G_SetGamepadAxes(0.f, 0.f, 0.f, 0.f);
		return;
	}

	//
	// Look is smoothed, movement is not: a lagging crosshair reads as
	// weight, a lagging walk reads as a broken control.
	//
	// The tic length is the right dt here because I_StartTic, and so this
	// function, runs once per tic from NetUpdate rather than once per frame.
	// Smoothing against the frame time would make the look speed depend on
	// the frame rate.
	//
	gamepad64.look_fx = I_GamepadLookSmoothing(gamepad64.look_fx, rx,
		1.0f / (float)TICRATE);
	gamepad64.look_fy = I_GamepadLookSmoothing(gamepad64.look_fy, ry,
		1.0f / (float)TICRATE);

	//
	// Signs are normalised here, once: +x right, +y forward or up. SDL
	// reports the vertical axes positive downwards.
	//
	// Sensitivity and inversion are deliberately NOT applied here. They
	// belong to G_BuildTiccmd, which is the single place that turns these
	// positions into a ticcmd. Applying them in both places is what made
	// the invert option a no-op and the sensitivity curve quadratic.
	//
	G_SetGamepadAxes(lx, -ly, gamepad64.look_fx, -gamepad64.look_fy);
}

//
// I_TranslateKey
//

// Modernised, it was really needed!
static int I_TranslateKey(SDL_KeyboardEvent *key_event)
{
	static struct { int sdl; int eng; } map[] = {
		{ SDLK_LEFT,        KEY_LEFTARROW },
		{ SDLK_RIGHT,       KEY_RIGHTARROW },
		{ SDLK_UP,          KEY_UPARROW },
		{ SDLK_DOWN,        KEY_DOWNARROW },
		{ SDLK_ESCAPE,      KEY_ESCAPE },
		{ SDLK_RETURN,      KEY_ENTER },
		{ SDLK_TAB,         KEY_TAB },
		{ SDLK_BACKSPACE,   KEY_BACKSPACE },
		{ SDLK_DELETE,      KEY_DEL },
		{ SDLK_INSERT,      KEY_INSERT },
		{ SDLK_HOME,        KEY_HOME },
		{ SDLK_END,         KEY_END },
		{ SDLK_PAGEUP,      KEY_PAGEUP },
		{ SDLK_PAGEDOWN,    KEY_PAGEDOWN },
		{ SDLK_PAUSE,       KEY_PAUSE },
		{ SDLK_LSHIFT,      KEY_RSHIFT },
		{ SDLK_RSHIFT,      KEY_RSHIFT },
		{ SDLK_LCTRL,       KEY_RCTRL },
		{ SDLK_RCTRL,       KEY_RCTRL },
		{ SDLK_LALT,        KEY_RALT },
		{ SDLK_RALT,        KEY_RALT },
		{ SDLK_EQUALS,      KEY_EQUALS },
		{ SDLK_MINUS,       KEY_MINUS },
		{ SDLK_SPACE,       KEY_SPACEBAR },
		{ SDLK_F1,          KEY_F1 },
		{ SDLK_F2,			KEY_F2  },
		{ SDLK_F3,			KEY_F3  },
		{ SDLK_F4,			KEY_F4  },
		{ SDLK_F5,          KEY_F5 },
		{ SDLK_F6,			KEY_F6  },
		{ SDLK_F7,			KEY_F7  },
		{ SDLK_F8,			KEY_F8  },
		{ SDLK_F9,          KEY_F9 },
		{ SDLK_F10,			KEY_F10 },
		{ SDLK_F11,			KEY_F11 },
		{ SDLK_F12,			KEY_F12 },
		{ SDLK_KP_0,        KEY_KEYPAD0 },
		{ SDLK_KP_1,        KEY_KEYPAD1 },
		{ SDLK_KP_2,        KEY_KEYPAD2 },
		{ SDLK_KP_3,        KEY_KEYPAD3 },
		{ SDLK_KP_4,        KEY_KEYPAD4 },
		{ SDLK_KP_5,        KEY_KEYPAD5 },
		{ SDLK_KP_6,        KEY_KEYPAD6 },
		{ SDLK_KP_7,        KEY_KEYPAD7 },
		{ SDLK_KP_8,        KEY_KEYPAD8 },
		{ SDLK_KP_9,        KEY_KEYPAD9 },
		{ SDLK_KP_ENTER,    KEY_KEYPADENTER },
		{ SDLK_KP_MULTIPLY, KEY_KEYPADMULTIPLY },
		{ SDLK_KP_PLUS,     KEY_KEYPADPLUS },
		{ SDLK_KP_MINUS,    KEY_KEYPADMINUS },
		{ SDLK_KP_DIVIDE,   KEY_KEYPADDIVIDE },
		{ SDLK_KP_PERIOD,   KEY_KEYPADPERIOD },
	};

	const int key = key_event->key;
		
	// key reserved for the console, independent of location on keyboard
	// Located in the top left corner (on both ANSI and ISO keyboards).
	if(key_event->scancode == SDL_SCANCODE_GRAVE) {
		return KEY_CONSOLE;
	}
	
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
		if (key == map[i].sdl) return map[i].eng;
	}
	
	if (key >= 32 && key < 127) {
		return key;
	}
	
	return 0;
}

//
// I_SDLtoDoomMouseState
//

static int I_SDLtoDoomMouseState(Uint8 buttonstate) {
	return 0
		| (buttonstate & SDL_BUTTON_LMASK ? 1 : 0)
		| (buttonstate & SDL_BUTTON_MMASK ? 2 : 0)
		| (buttonstate & SDL_BUTTON_RMASK ? 4 : 0)
		| (buttonstate & SDL_BUTTON_X1MASK ? 8 : 0)
		| (buttonstate & SDL_BUTTON_X2MASK ? 16 : 0);
}

//
// I_ReadMouse
//

void I_ReadMouse(void) {
	float x, y;
	Uint8 btn;
	event_t ev;
	static Uint8 lastmbtn = 0;

	SDL_GetRelativeMouseState(&x, &y);
	btn = SDL_GetMouseState(&mouse_x, &mouse_y);

	if (x != 0 || y != 0 || btn || (lastmbtn != btn)) {
		ev.type = ev_mouse;
		ev.data1 = I_SDLtoDoomMouseState(btn);
		ev.data2 = x * 32.0;
		ev.data3 = -y * 32.0;
		ev.data4 = 0;
		D_PostEvent(&ev);
	}

	lastmbtn = btn;
}

void I_CenterMouse(void) {
	SDL_WarpMouseInWindow(window, (unsigned short)(video_width / 2), (unsigned short)(video_height / 2));
	SDL_PumpEvents();
	SDL_GetRelativeMouseState(NULL, NULL);
}


void I_SetMousePos(float x, float y) {
	SDL_WarpMouseInWindow(window, (unsigned short)x, (unsigned short)y);
	SDL_PumpEvents();
	SDL_GetMouseState(&mouse_x, &mouse_y); // refresh new location
}

//
// I_MouseAccelChange
//

void I_MouseAccelChange(void) {
	mouse_accelfactor = v_macceleration.value / 200.0f + 1.0f;
}

//
// I_MouseAccel
//

float I_MouseAccel(float val) {
	if (!v_macceleration.value) {
		return val;
	}

	if (val < 0) {
		return -I_MouseAccel(-val);
	}

	return (float)(pow((double)val, (double)mouse_accelfactor));
}

//
// I_UpdateGrab
//

boolean I_UpdateGrab(void) {

	static boolean currently_grabbed = false;
	boolean grab;

	grab = !menuactive
		&& (gamestate == GS_LEVEL)
		&& !demoplayback;

	/*
		Don't grab the keyboard (SDL_SetWindowKeyboardGrab) because:
			- we don't need it
			- it mess up Alt-Tab (only works when not grabbed). And Tab switches the automap modes
			- on Linux KDE (Plasma) it prevents global shortcuts to work (brightness, sound, ...)
			- finally, the SDL doc does not recommend it: "Normal games should not use keyboard grab"
	*/

	if (grab && !currently_grabbed) {
		SDL_SetWindowRelativeMouseMode(window, 1); // this grabs the mouse and hide cursor
		SDL_SetWindowMouseGrab(window, 1);
	}

	if (!grab && currently_grabbed) {
		SDL_SetWindowRelativeMouseMode(window, 0);
		SDL_SetWindowMouseGrab(window, 0);
	}

	currently_grabbed = grab;

	return currently_grabbed;
}

//
// I_GetEvent
//

void I_GetEvent(SDL_Event* Event) {
	event_t event;
	unsigned int mwheeluptic = 0, mwheeldowntic = 0;
	unsigned int tic = gametic;

	I_GamepadHandleSDLEvent(Event);
	
	switch (Event->type) {
	case SDL_EVENT_KEY_DOWN:
		if (Event->key.repeat) {
			break;
		}
		event.type = ev_keydown;
		event.data1 = I_TranslateKey(&Event->key);
		if(event.data1) {
			D_PostEvent(&event);
		}
		break;

	case SDL_EVENT_KEY_UP:
		event.type = ev_keyup;
		event.data1 = I_TranslateKey(&Event->key);
		if(event.data1) {
			D_PostEvent(&event);
		}
		break;

	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (!window_focused)
			break;

		event.type = (Event->type == SDL_EVENT_MOUSE_BUTTON_UP) ? ev_mouseup : ev_mousedown;
		event.data1 =
			I_SDLtoDoomMouseState(SDL_GetMouseState(NULL, NULL));
		event.data2 = event.data3 = 0;

		D_PostEvent(&event);
		break;

	case SDL_EVENT_MOUSE_WHEEL:
		if (Event->wheel.y > 0) {
			event.type = ev_keydown;
			event.data1 = KEY_MWHEELUP;
			mwheeluptic = tic;
		}
		else if (Event->wheel.y < 0) {
			event.type = ev_keydown;
			event.data1 = KEY_MWHEELDOWN;
			mwheeldowntic = tic;
		}
		else
			break;

		event.data2 = event.data3 = 0;
		D_PostEvent(&event);
		break;

	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		window_focused = true;
		break;

	case SDL_EVENT_WINDOW_FOCUS_LOST:
		window_focused = false;
		break;

	case SDL_EVENT_WINDOW_MOUSE_ENTER:
		window_mouse = true;
		break;

	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		window_mouse = false;
		break;

	case SDL_EVENT_QUIT:
		I_Quit();
		break;

	default:
		break;
	}

	if (mwheeluptic && mwheeluptic + 1 < tic) {
		event.type = ev_keyup;
		event.data1 = KEY_MWHEELUP;
		D_PostEvent(&event);
		mwheeluptic = 0;
	}

	if (mwheeldowntic && mwheeldowntic + 1 < tic) {
		event.type = ev_keyup;
		event.data1 = KEY_MWHEELDOWN;
		D_PostEvent(&event);
		mwheeldowntic = 0;
	}
}

//
// I_ShutdownWait
//

int I_ShutdownWait(void) {
	static SDL_Event event;

	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT ||
			(event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
			I_ShutdownVideo();
			return 1;
		}
	}

	return 0;
}

//
// I_StartTic
//

void I_StartTic(void) {
	SDL_Event Event;

	while (SDL_PollEvent(&Event)) {
		I_GetEvent(&Event);
	}

	I_InitInputs();
	I_ReadMouse();
	I_GamepadUpdate();
}

static float GetDisplayRefreshRate(void) {
	SDL_DisplayID displayid = SDL_GetDisplayForWindow(window);
	if (displayid) {
		const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayid);
		if (mode && mode->refresh_rate > 0) {
			return mode->refresh_rate;
		}
	}
	return 60.0f; // not happy, return to 60
}

void I_FPSLimit(void) {
	static Uint64 lastFrameTime = 0;
	static float displayRefreshRate = 0.0f;

	static int refreshCheckCounter = 0;
	if (refreshCheckCounter++ % 60 == 0) {
		displayRefreshRate = GetDisplayRefreshRate();
	}

	float targetFPS = (v_maxfps.value > 0) ? v_maxfps.value : displayRefreshRate;
	const Uint64 targetFrameTimeNS = (Uint64)(1000000000.0f / targetFPS);

	Uint64 currentTime = SDL_GetTicksNS();
	if (lastFrameTime > 0) {
		Uint64 elapsed = currentTime - lastFrameTime;
		if (elapsed < targetFrameTimeNS) {
			Uint64 sleepTime = targetFrameTimeNS - elapsed;
			if (sleepTime > 500000) {
				SDL_DelayNS(sleepTime - 100000);
			}
			while (SDL_GetTicksNS() - lastFrameTime < targetFrameTimeNS) {
			}
		}
	}
	lastFrameTime = SDL_GetTicksNS();
}

//
// I_FinishUpdate
//

void I_FinishUpdate(void) {

	static int prev_swap_interval = -1;
	int swap_interval = (int)v_vsync.value;

	I_UpdateGrab();

	if (prev_swap_interval != swap_interval) {
		SDL_GL_SetSwapInterval(swap_interval);
		prev_swap_interval = swap_interval;
	}

	SDL_GL_SwapWindow(window);

	if (swap_interval > 0) {
		glFinish();
	}
	else if (i_interpolateframes.value > 0) {
		I_FPSLimit();
	}

	BusyDisk = false;
}

//
// I_InitInputs
//

void I_InitInputs(void) {
	SDL_PumpEvents();
	I_MouseAccelChange();
	I_GamepadInitOnce();
}

//
// ISDL_RegisterCvars
//

void ISDL_RegisterKeyCvars(void) {
	CON_CvarRegister(&v_msensitivityx);
	CON_CvarRegister(&v_msensitivityy);
	CON_CvarRegister(&v_macceleration);
	CON_CvarRegister(&v_mlook);
	CON_CvarRegister(&v_mlookinvert);
	CON_CvarRegister(&v_yaxismove);
	CON_CvarRegister(&v_xaxismove);

	// [styd] gamepad
	CON_CvarRegister(&v_gamepad);
	CON_CvarRegister(&v_gamepadsensx);
	CON_CvarRegister(&v_gamepadsensy);
	CON_CvarRegister(&v_gamepadinvert);
	CON_CvarRegister(&v_gamepaddeadzone);
	CON_CvarRegister(&v_gamepadlayout);
	CON_CvarRegister(&v_gamepadrumble);
}
