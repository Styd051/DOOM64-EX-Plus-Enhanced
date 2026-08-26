// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 1993-1997 Id Software, Inc.
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

#ifndef G_SETTINGS_H
#define G_SETTINGS_H

#include "doomtype.h"

void G_LoadSettings(void);
void G_ExecuteFile(char* name);
char* G_GetConfigFileName(void);

//
// [styd] restores every binding to the shipped defaults.
//
// The bindings menu had no way back: an action left unbound, or bound to
// something the player could no longer reach, meant editing config.cfg by
// hand. This runs the same default block that is written into a fresh
// config, which begins with unbindall, so it is a full reset rather than a
// merge.
//
void G_ResetBindings(void);

extern boolean g_in_load_settings;

#endif
