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

#ifndef __I_SDLINPUT__
#define __I_SDLINPUT__

#include <SDL3/SDL.h>

#include "doomtype.h"

#include "g_controls.h"

extern int UseMouse[2];
extern float mouse_x;
extern float mouse_y;

float I_MouseAccel(float val);
void I_MouseAccelChange(void);

void ISDL_RegisterKeyCvars(void);

void I_GetEvent(SDL_Event* Event);
void I_ReadMouse(void);
void I_InitInputs(void);

//
// [styd] text entry.
//
// Only switched on while a text field is actually open, because an active
// text input target can raise an IME candidate window or an on screen
// keyboard on some platforms, which has no business appearing during play.
//
void I_StartTextInput(void);
void I_StopTextInput(void);

void I_StartTic(void);
void I_FinishUpdate(void);
int I_ShutdownWait(void);
void I_CenterMouse(void);
void I_SetMousePos(float x, float y);
boolean I_UpdateGrab(void);

//
// [styd] gamepad state.
//
// The old struct carried one bool per hardcoded game action, because the
// pad synthesised keyboard presses (W, A, S, D, Ctrl, E...) instead of
// reporting itself as a gamepad. That made the pad unbindable and broke it
// outright on any layout where the player had rebound movement, which on an
// AZERTY keyboard is everybody.
//
// The pad now reports button indices through ev_gamepaddown/ev_gamepadup
// and goes through the normal binding system, so all that state collapses
// into one bitmask of what was held last tic, used for edge detection.
//
typedef struct {
	SDL_Gamepad* gamepad;
	SDL_Joystick* joy;
	SDL_JoystickID active_id;

	// one bit per button index, as of the previous poll
	unsigned int    held[(NUM_GAMEPADBTNS + 31) / 32];

	// menu repeat timers for the four directions
	int             menu_repeat[4];
	bool            menu_held[4];

	// smoothed look axes, carried between polls by the low pass filter
	float           look_fx;
	float           look_fy;

	// whether the previous poll was in menu context, so that the change of
	// meaning of the stick across that boundary can be absorbed
	bool            was_in_menu;

	bool            init;
} gamepad64_t;

extern gamepad64_t gamepad64;

// true when a pad is connected and enabled
boolean I_GamepadPresent(void);

// name of the connected pad, or NULL. Used by the gamepad options menu.
const char* I_GamepadName(void);

// [styd] strength 0.0 to 1.0, duration in milliseconds. Silent when no pad
// is connected or v_gamepadrumble is 0, so callers need no guard.
void I_GamepadRumble(float strength, int duration_ms);

#endif
