// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 1999-2000 Paul Brook
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

#ifndef G_CONTROLS_H
#define G_CONTROLS_H

// villsa 01052014 - changed to 420
#define NUMKEYS         420
#define NUMGAMEPADBTNS	60

//
// [styd] gamepad button index space used by the binding system.
//
// The first GAMEPADBTN_SDL_COUNT entries mirror SDL3's SDL_GamepadButton
// enumeration one for one, so an index can be handed straight to
// SDL_GetGamepadButton without a translation table. SDL exposes the two
// triggers as analog axes rather than buttons, so they are given virtual
// indices past the end of that range and turned into digital presses by
// i_sdlinput.c.
//
// NUMGAMEPADBTNS above is the size of the action array and stays larger
// than NUM_GAMEPADBTNS on purpose: SDL has grown its button list before
// and will again, and the spare entries mean a future SDL does not walk
// off the end of GamepadActions[].
//
#define GAMEPADBTN_SDL_COUNT    26      // SDL_GAMEPAD_BUTTON_COUNT (SDL 3.2)
#define GAMEPADBTN_LTRIGGER     (GAMEPADBTN_SDL_COUNT + 0)
#define GAMEPADBTN_RTRIGGER     (GAMEPADBTN_SDL_COUNT + 1)
#define NUM_GAMEPADBTNS         (GAMEPADBTN_SDL_COUNT + 2)

//
// Named indices for the handful of buttons the menu itself has to know
// about. Every other button is anonymous to the engine: it exists only as
// an index into GamepadActions[], which is what makes it rebindable.
//
// These mirror SDL_GamepadButton and are checked against it at compile time
// in i_sdlinput.c, so a renumbering in a future SDL breaks the build rather
// than quietly moving the menu onto the wrong buttons.
//
#define GAMEPADBTN_SOUTH        0
#define GAMEPADBTN_EAST         1
#define GAMEPADBTN_WEST         2
#define GAMEPADBTN_NORTH        3
#define GAMEPADBTN_BACK         4
#define GAMEPADBTN_START        6
#define GAMEPADBTN_LSHOULDER    9
#define GAMEPADBTN_RSHOULDER    10
#define GAMEPADBTN_DPAD_UP      11
#define GAMEPADBTN_DPAD_DOWN    12
#define GAMEPADBTN_DPAD_LEFT    13
#define GAMEPADBTN_DPAD_RIGHT   14

#define PCKF_DOUBLEUSE  0x4000
#define PCKF_UP         0x8000
#define PCKF_COUNTMASK  0x00ff

typedef enum {
	PCKEY_ATTACK,
	PCKEY_USE,
	PCKEY_STRAFE,
	PCKEY_FORWARD,
	PCKEY_BACK,
	PCKEY_LEFT,
	PCKEY_RIGHT,
	PCKEY_STRAFELEFT,
	PCKEY_STRAFERIGHT,
	PCKEY_RUN,
	PCKEY_LOOKUP,
	PCKEY_LOOKDOWN,
	PCKEY_CENTER,
	PCKEY_QUICKSAVE,
	PCKEY_QUICKLOAD,
	PCKEY_SAVE,
	PCKEY_LOAD,
	PCKEY_SCREENSHOT,
	PCKEY_GAMMA,
	NUM_PCKEYS
} pckeys_t;

typedef struct {
	int            mousex;
	int            mousey;

	//
	// [styd] gamepad stick positions, normalised to -1.0 .. 1.0 after
	// dead zone and response curve.
	//
	// Unlike mousex/mousey these are absolute positions, not accumulated
	// deltas: I_GamepadUpdate overwrites them every tic (with zero when no
	// pad is connected or the menu is up), so G_BuildTiccmd reads them and
	// must NOT clear them afterwards.
	//
	// Signs: +movex right, +movey forward, +lookx right, +looky up.
	//
	float		   joymovex;
	float		   joymovey;
	float		   joylookx;
	float		   joylooky;
	int            key[NUM_PCKEYS];
	int            nextweapon;
	int            sdclicktime;
	int            fdclicktime;
	int            flags;
} playercontrols_t;

#define PCF_NEXTWEAPON    0x01
#define PCF_FDCLICK        0x02
#define PCF_FDCLICK2    0x04
#define PCF_SDCLICK        0x08
#define PCF_SDCLICK2    0x10
#define PCF_PREVWEAPON    0x20
#define PCF_GAMEPAD     0x40

extern playercontrols_t    Controls;
extern char* G_GetConfigFileName(void);

#endif
