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

#ifndef M_KEYS_H
#define M_KEYS_H

#define MAX_KEY_NAME_LENGTH    32

int M_GetKeyName(char* buff, int key);

//
// [styd] name of a gamepad button, for the bindings menu and the config
// file. Returns false and writes a generic "PadBtnNN" name for indices
// that the current SDL does not label.
//
int M_GetGamepadButtonName(char* buff, int btn);

#endif
