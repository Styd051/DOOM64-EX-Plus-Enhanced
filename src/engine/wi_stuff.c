// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 1993-1997 Id Software, Inc.
// Copyright(C) 1997 Midway Home Entertainment, Inc
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
//      Intermission screens.
//
//-----------------------------------------------------------------------------

#include <SDL3/SDL.h>

#include "wi_stuff.h"
#include "s_sound.h"
#include "doomstat.h"
#include "sounds.h"
#include "m_password.h"
#include "p_setup.h"
#include "st_stuff.h"
#include "r_wipe.h"
#include "gl_draw.h"
#include "w_wad.h"
#include "i_sdlinput.h"


#define RGB_ALPHA      D_RGBA(0xC0, 0, 0, 0xFF)

static int itempercent[MAXPLAYERS];
static int itemvalue[MAXPLAYERS];

static int killpercent[MAXPLAYERS];
static int killvalue[MAXPLAYERS];

static int secretpercent[MAXPLAYERS];
static int secretvalue[MAXPLAYERS];

static char timevalue[16];

static int wi_stage = 0;
static int wi_counter = 0;
static int wi_advance = 0;

//
// WI_Start
//

void WI_Start(void) {
	int i;
	int minutes = 0;
	int seconds = 0;

	// initialize player stats
	for (i = 0; i < MAXPLAYERS; i++) {
		itemvalue[i] = killvalue[i] = secretvalue[i] = -1;

		if (!totalkills) {
			killpercent[i] = 100;
		}
		else {
			killpercent[i] = (players[i].killcount * 100) / totalkills;
		}

		if (!totalitems) {
			itempercent[i] = 100;
		}
		else {
			itempercent[i] = (players[i].itemcount * 100) / totalitems;
		}

		if (!totalsecret) {
			secretpercent[i] = 100;
		}
		else {
			secretpercent[i] = (players[i].secretcount * 100) / totalsecret;
		}
	}

	// setup level time
	if (leveltime) {
		minutes = (leveltime / TICRATE) / 60;
		seconds = (leveltime / TICRATE) % 60;
	}

	SDL_snprintf(timevalue, sizeof(timevalue), "%2.2d:%2.2d", minutes, seconds);

	// generate password
	if (nextmap < 40) {
		M_EncodePassword();
	}

	// clear variables
	wi_counter = 0;
	wi_stage = 0;
	wi_advance = 0;

	// start music
	S_StartMusic(W_GetNumForName("MUSDONE"));

	allowmenu = true;
}

//
// WI_Stop
//

void WI_Stop(void) {
	wi_counter = 0;
	wi_stage = 0;
	wi_advance = 0;

	allowmenu = false;

	S_StopMusic();
	WIPE_FadeScreen(6);
}

//
// WI_Ticker
//

int WI_Ticker(void) {
	boolean    state = false;
	player_t* player;
	int         i;
	boolean    next = false;

	if (wi_advance <= 3) {
		for (i = 0, player = players; i < MAXPLAYERS; i++, player++) {
			if (playeringame[i]) {
				if ((player->cmd.buttons & BT_ATTACK) && !player->attackdown) {
					S_StartSound(NULL, sfx_explode);
					wi_advance++;
				}
				player->attackdown = (player->cmd.buttons & BT_ATTACK) != 0;

				if ((player->cmd.buttons & BT_USE) && !player->usedown) {
					S_StartSound(NULL, sfx_explode);
					wi_advance++;
				}
				player->usedown = (player->cmd.buttons & BT_USE) != 0;
			}
		}
		//
		// [styd] the hardcoded gamepad polling that used to live here is gone.
		//
		// It read the pad directly, with its own guessed button numbers, and
		// raised wi_advance on its own. But the pad's right trigger is bound
		// to +fire, so a single pull already arrived above as BT_ATTACK: the
		// press counted TWICE and wi_advance jumped straight from 0 to 2.
		//
		// That skipped the wi_advance == 1 branch below, which is the branch
		// that fills killvalue/itemvalue/secretvalue and sets wi_stage to 5.
		// The result was an intermission frozen on the kill counter with the
		// other lines never appearing - while the keyboard, which only ever
		// counted once, worked.
		//
		// The pad now goes through the ticcmd like every other control.
		//
	}

	// accelerate counters
	if (wi_advance == 1) {
		wi_stage = 5;
		wi_counter = 0;
		wi_advance = 2;

		for (i = 0; i < MAXPLAYERS; i++) {
			killvalue[i] = killpercent[i];
			itemvalue[i] = itempercent[i];
			secretvalue[i] = secretpercent[i];
		}
	}

	if (wi_advance == 2) {
		return 0;
	}

	if (wi_advance == 3) {
		//S_StartSound(NULL, sfx_explode);
		wi_advance = 4;
	}

	// fade out, complete world
	if (wi_advance >= 4) {
		clusterdef_t* cluster;
		clusterdef_t* nextcluster;

		cluster = P_GetCluster(gamemap);
		nextcluster = P_GetCluster(nextmap);

		if ((nextcluster && cluster != nextcluster && nextcluster->enteronly) ||
			(cluster && cluster != nextcluster && !cluster->enteronly)) {
			return ga_victory;
		}

		return 1;
	}

	if (wi_counter) {
		if ((gametic - wi_counter) <= 60) {
			return 0;
		}
	}

	next = true;

	// counter ticks
	switch (wi_stage) {
	case 0:
		S_StartSound(NULL, sfx_explode);
		wi_stage = 1;
		state = false;
		break;

	case 1:     // kills
		for (i = 0; i < MAXPLAYERS; i++) {
			killvalue[i] += 4;
			if (killvalue[i] > killpercent[i]) {
				killvalue[i] = killpercent[i];
			}
			else {
				next = false;
			}
		}

		if (next) {
			S_StartSound(NULL, sfx_explode);

			wi_counter = gametic;
			wi_stage = 2;
		}

		state = true;
		break;

	case 2:     // item
		for (i = 0; i < MAXPLAYERS; i++) {
			itemvalue[i] += 4;
			if (itemvalue[i] > itempercent[i]) {
				itemvalue[i] = itempercent[i];
			}
			else {
				next = false;
			}
		}

		if (next) {
			S_StartSound(NULL, sfx_explode);

			wi_counter = gametic;
			wi_stage = 3;
		}

		state = true;
		break;

	case 3:     // secret
		for (i = 0; i < MAXPLAYERS; i++) {
			secretvalue[i] += 4;
			if (secretvalue[i] > secretpercent[i]) {
				secretvalue[i] = secretpercent[i];
			}
			else {
				next = false;
			}
		}

		if (next) {
			S_StartSound(NULL, sfx_explode);

			wi_counter = gametic;
			wi_stage = 4;
		}

		state = true;
		break;

	case 4:
		if (gamemap > 80 && nextmap > 80) {
			S_StartSound(NULL, sfx_explode);
		}

		wi_counter = gametic;
		wi_stage = 5;
		state = false;
		break;
	}

	if (!wi_advance && !state) {
		if (wi_stage == 5) {
			wi_advance = 1;
		}
	}

	// play sound as counter increases
	if (state && !(gametic & 3)) {
		S_StartSound(NULL, sfx_pistol);
	}

	return 0;
}

//
// WI_Drawer
//

void WI_Drawer(void) {
	int currentmap = gamemap;

	GL_ClearView(0xFF000000);

	if (currentmap < 1) {
		currentmap = 0;
	}

	if (currentmap > 80) {
		currentmap = 33;
	}

	if (wi_advance >= 4) {
		return;
	}

	// draw background
	Draw_GfxImageInter(63, 25, "EVIL", WHITE, false);

	// draw 'mapname' Finished text
	Draw_BigText(-1, 20, WHITE, P_GetMapInfo(currentmap)->mapname);
	Draw_BigText(-1, 36, WHITE, "Finished");

	if (!netgame) {
		// draw kills
		Draw_BigText(57, 60, RGB_ALPHA, "Kills");
		Draw_BigText(248, 60, RGB_ALPHA, "%");
		if (wi_stage > 0 && killvalue[0] > -1) {
			Draw_Number(210, 60, killvalue[0], 1, RGB_ALPHA);
		}

		// draw items
		Draw_BigText(57, 78, RGB_ALPHA, "Items");
		Draw_BigText(248, 78, RGB_ALPHA, "%");
		if (wi_stage > 1 && itemvalue[0] > -1) {
			Draw_Number(210, 78, itemvalue[0], 1, RGB_ALPHA);
		}

		// draw secrets
		Draw_BigText(57, 99, RGB_ALPHA, "Secrets");
		Draw_BigText(248, 99, RGB_ALPHA, "%");
		if (wi_stage > 2 && secretvalue[0] > -1) {
			Draw_Number(210, 99, secretvalue[0], 1, RGB_ALPHA);
		}

		// draw time
		if (wi_stage > 3) {
			Draw_BigText(57, 120, RGB_ALPHA, "Time");
			Draw_BigText(210, 120, RGB_ALPHA, timevalue);
		}
	}
	else {
		int i;
		int y = 160;

		GL_SetOrthoScale(0.5f);

		Draw_BigText(180, 140, WHITE, "Kills");
		Draw_BigText(300, 140, WHITE, "Items");
		Draw_BigText(412, 140, WHITE, "Secrets");

		for (i = 0; i < MAXPLAYERS; i++) {
			if (!playeringame[i]) {
				continue;
			}

			Draw_BigText(57, y, RGB_ALPHA, player_names[i]);
			Draw_BigText(232, y, RGB_ALPHA, "%");
			Draw_BigText(352, y, RGB_ALPHA, "%");
			Draw_BigText(464, y, RGB_ALPHA, "%");

			if (wi_stage > 0 && killvalue[i] > -1) {
				Draw_Number(180, y, killvalue[i], 1, RGB_ALPHA);
			}

			if (wi_stage > 1 && itemvalue[i] > -1) {
				Draw_Number(300, y, itemvalue[i], 1, RGB_ALPHA);
			}

			if (wi_stage > 2 && secretvalue[i] > -1) {
				Draw_Number(412, y, secretvalue[i], 1, RGB_ALPHA);
			}

			y += 16;
		}

		// draw time
		if (wi_stage > 3) {
			Draw_BigText(248, y + 32, RGB_ALPHA, "Time");
			Draw_BigText(324, y + 32, RGB_ALPHA, timevalue);
		}

		GL_SetOrthoScale(1.0f);
	}

	// draw password and name of next map
	if (wi_stage > 4 && (P_GetMapInfo(nextmap) != NULL)) {
		char password[20];
		byte* passData;
		int i = 0;
		int y = 145;

		if (netgame) {
			y = 187;
		}

		Draw_BigText(-1, y, WHITE, "Entering");
		Draw_BigText(-1, y + 16, WHITE, P_GetMapInfo(nextmap)->mapname);

		if (netgame) {  // don't bother drawing the password on netgames
			return;
		}

		Draw_BigText(-1, 187, WHITE, "Password");

		dmemset(password, 0, 20);
		passData = passwordData;

		// draw actual password
		do {
			if (i && !((passData - passwordData) & 3)) {
				password[i++] = 0x20;
			}

			if (i >= 20) {
				break;
			}

			password[i++] = passwordChar[*(passData++)];
		} while (i < 20);

		password[19] = 0;

		Draw_BigText(-1, 203, WHITE, password);
	}
}
