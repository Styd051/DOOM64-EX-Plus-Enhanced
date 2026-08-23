// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// 2026 Styd051
//
// Dynamic projectile lighting
// Ported from the Doom 64 Dreamcast port by jnmartin84
//
//-----------------------------------------------------------------------------

#include <math.h>

#include "doomdef.h"
#include "doomstat.h"
#include "r_dynlights.h"
#include "r_main.h"
#include "p_local.h"
#include "info.h"
#include "con_cvar.h"
#include "tables.h"
#include "r_drawlist.h"
#include "dgl.h"
#include "z_zone.h"
#include "i_swap.h"
#include "m_misc.h"

dynlight_t  dynlights[MAX_DYNLIGHTS];
int         numdynlights = 0;

CVAR(r_dynlights, 0);
CVAR(r_dynlightintensity, 5);
CVAR(r_dynlightsprites, 1);
CVAR(r_dynlightmuzzle, 1);
CVAR(r_dynlightquality, 2);
CVAR(r_dynlightprops, 1);
CVAR(r_dynlightdebug, 0);
CVAR(r_dynlightshadows, 1);
CVAR(r_dynlightweapon, 1);

CVAR_EXTERNAL(i_interpolateframes);

//
// light classes.
// each class has its own cap so that, for example, a wall of lost souls
// cannot starve a rocket of its light slot.
//
typedef enum {
	dl_muzzle,
	dl_rocket,
	dl_plasma,
	dl_bfg,
	dl_laser,
	dl_impball,
	dl_niteball,
	dl_cacoball,
	dl_baronfire,
	dl_knightfire,
	dl_spidershot,
	dl_mancrocket,
	dl_tracer,
	dl_motherfire,
	dl_vilefire,
	dl_skull,
	dl_explosion,
	dl_torchyellow,
	dl_torchblue,
	dl_torchred,
	dl_fire,
	dl_fireyellow,
	dl_fireblue,
	dl_firered,
	dl_candle,
	NUMDLTYPES
} dynlight_type_t;

static const int dl_maxcount[NUMDLTYPES] = {
	1,   // dl_muzzle
	4,   // dl_rocket
	4,   // dl_plasma
	2,   // dl_bfg
	6,   // dl_laser
	4,   // dl_impball
	4,   // dl_niteball
	4,   // dl_cacoball
	3,   // dl_baronfire
	3,   // dl_knightfire
	4,   // dl_spidershot
	3,   // dl_mancrocket
	3,   // dl_tracer
	3,   // dl_motherfire
	4,   // dl_vilefire
	6,   // dl_skull
	4,   // dl_explosion

	// Scenery is static and there can be a lot of it in one room, so these
	// caps are what stop a row of torches from evicting every projectile
	// light on screen. Projectiles keep their own budget either way, since
	// the eviction in R_AddDynLight only ever steals from the same class.
	8,   // dl_torchyellow
	8,   // dl_torchblue
	8,   // dl_torchred
	8,   // dl_fire
	8,   // dl_fireyellow
	8,   // dl_fireblue
	8,   // dl_firered
	8,   // dl_candle
};

static int dl_count[NUMDLTYPES];

// lights that found no slot this frame; surfaced by the debug readout so a
// vanishing light can be told apart from a light that was never created
static int dl_dropped = 0;

//
// muzzle flash state
//
static int         muzzle_tic = -1;
static int         muzzle_weapon = wp_nochange;
static player_t*   muzzle_player = NULL;

//
// local pseudo-random generator.
// deliberately NOT P_Random / M_Random: the flicker is a purely visual
// effect and must not touch the gameplay RNG state (demos, netgames).
//
static unsigned int dl_rndseed = 0x9e3779b9u;

//
// Reseeded from gametic at the start of every collection.
//
// Without this the flicker advances once per *rendered frame*: at 30 fps it
// matches the Dreamcast, but at 144 or 300 fps a torch strobes several times
// faster than the flame animation and reads as electrical buzz rather than
// fire. Seeding from the tic makes every frame inside a tic produce the same
// value, so the flicker runs at a fixed 30 Hz whatever the framerate.
//
static void R_DynLightSeedRandom(void) {
	dl_rndseed = 0x9e3779b9u ^ ((unsigned int)gametic * 2654435761u);
}

static int R_DynLightRandom(void) {
	dl_rndseed = (dl_rndseed * 1103515245u) + 12345u;
	return (int)((dl_rndseed >> 16) & 0xff);
}

//
// How far from the viewer a light may still be created, added to its radius,
// and over how much of the tail of that range it fades to nothing.
//
#define DL_VIEW_RANGE   2048.0f
#define DL_FADE_BAND     512.0f

//
// R_AddDynLight
//
// Mirrors R_AddProjectileLight from the Dreamcast port: distance culling,
// per-class cap, and replacement of the farthest light of the same class
// when the cap is reached.
//

static void R_AddDynLight(fixed_t x, fixed_t y, fixed_t z,
	float radius, int r, int g, int b, int type) {
	float   fx, fy, fz;
	float   dx, dy, dz;
	float   dist;
	float   cullrange;
	int     i;
	int     slot;

	if (radius <= 0.0f) {
		return;
	}

	fx = F2D3D(x);
	fy = F2D3D(y);
	fz = F2D3D(z);

	//
	// Distance culling.
	//
	// The cull is measured from the viewer to the light, but what the player
	// actually sees is the geometry *around* the light, which stays on screen
	// far beyond that. A hard cutoff therefore makes a lit wall snap to dark
	// the moment the viewer steps over the threshold, which is very visible
	// on a static emitter like a torch.
	//
	// So the light fades out over the last stretch of its range instead of
	// being dropped in one frame.
	//
	dx = fx - fviewx;
	dy = fy - fviewy;
	dz = fz - fviewz;

	dist = (float)sqrt((double)(dx * dx + dy * dy + dz * dz));

	cullrange = radius + DL_VIEW_RANGE;

	if (dist > cullrange) {
		return;
	}

	if (dist > (cullrange - DL_FADE_BAND)) {
		float fade = (cullrange - dist) * (1.0f / DL_FADE_BAND);

		r = (int)((float)r * fade);
		g = (int)((float)g * fade);
		b = (int)((float)b * fade);

		// once it has faded to nothing there is no point taking a slot
		if (r <= 0 && g <= 0 && b <= 0) {
			return;
		}
	}

	if (r < 0) { r = 0; } if (r > 255) { r = 255; }
	if (g < 0) { g = 0; } if (g > 255) { g = 255; }
	if (b < 0) { b = 0; } if (b > 255) { b = 255; }

	slot = -1;

	if (numdynlights < MAX_DYNLIGHTS && dl_count[type] < dl_maxcount[type]) {
		slot = numdynlights++;
		dl_count[type]++;
	}
	else {
		// cap reached: steal the slot of the farthest light of the same class
		float farthest = dist;

		for (i = 0; i < numdynlights; i++) {
			if (dynlights[i].type == type && dynlights[i].dist > farthest) {
				farthest = dynlights[i].dist;
				slot = i;
			}
		}

		if (slot < 0) {
			dl_dropped++;
			return;
		}
	}

	dynlights[slot].x = fx;
	dynlights[slot].y = fy;
	dynlights[slot].z = fz;
	dynlights[slot].r = (float)r / 255.0f;
	dynlights[slot].g = (float)g / 255.0f;
	dynlights[slot].b = (float)b / 255.0f;
	dynlights[slot].radius = radius;
	dynlights[slot].rcpradius = 1.0f / radius;
	dynlights[slot].dist = dist;
	dynlights[slot].type = type;

	// remember which sector the light sits in: the reachability flood fill
	// below starts from here
	{
		subsector_t* ss = R_PointInSubsector(x, y);

		dynlights[slot].sector = ss ? ss->sector : NULL;
	}
}


//
// R_AddPropLight
//
// Scenery lights go through here instead of R_AddDynLight directly.
//
// The Dreamcast writes its dynamic light into oargb, the PowerVR's additive
// offset colour, and caps the result at COMPONENT_INTENSITY (96 of 255). The
// light is therefore added on top of the textured surface and can never
// contribute more than about 38% brightness.
//
// Here the light goes into the vertex colour, which *multiplies* the texture,
// and is renormalised to a full 255. That is what gives projectiles their
// punch, and it looks right for something that flies past in half a second.
// A torch, though, never moves: at full strength it does not add a glow, it
// permanently repaints the wall behind it and overrides the mapper's sector
// colouring. Halving the scenery colours puts their ceiling back in line with
// the Dreamcast's, so a torch warms its corner instead of recolouring it.
//
#define DL_PROP_SCALE   0.5f

static void R_AddPropLight(fixed_t x, fixed_t y, fixed_t z,
	float radius, int r, int g, int b, int type) {
	if (!r_dynlightprops.value) {
		return;
	}

	R_AddDynLight(x, y, z, radius,
		(int)((float)r * DL_PROP_SCALE),
		(int)((float)g * DL_PROP_SCALE),
		(int)((float)b * DL_PROP_SCALE),
		type);
}

//
// R_TriggerMuzzleFlash
// Called from the weapon code when a shot is fired.
//

void R_TriggerMuzzleFlash(player_t* player, int weapon) {
	muzzle_tic = gametic;
	muzzle_weapon = weapon;
	muzzle_player = player;
}

//
// R_AddMuzzleFlash
//
// The Dreamcast port fades the muzzle light over a number of rendered
// frames, which makes its duration framerate-dependent. We fade over
// game tics instead so the flash lasts the same time at 30 or 300 fps.
//

#define DL_MUZZLE_BRIGHT    0.70f

static void R_AddMuzzleFlash(player_t* player) {
	int     elapsed;
	int     duration;
	float   scale;
	float   radius;
	float   bright;
	int     r, g, b;

	if (!r_dynlightmuzzle.value || muzzle_tic < 0) {
		return;
	}

	if (!player->mo) {
		return;
	}

	// A_FireCGun and friends run for every player in a netgame, so without
	// this a remote player pulling the trigger would light up our own gun.
	if (muzzle_player != player) {
		return;
	}

	//
	// The light has to match the flash the weapon actually draws. Every
	// flash state is the weapon sprite itself with the full-bright bit set,
	// so the colour comes from the artwork rather than from any field we
	// could read: it has to be written down here, per weapon.
	//
	switch (muzzle_weapon) {
	case wp_pistol:
		duration = 3;
		radius = 288.0f;
		r = 255; g = 236; b = 176;      // warm powder flash
		break;

	case wp_shotgun:
		duration = 4;
		radius = 336.0f;
		r = 255; g = 214; b = 128;
		break;

	case wp_supershotgun:
		duration = 5;
		radius = 384.0f;
		r = 255; g = 214; b = 128;
		break;

	case wp_chaingun:
		duration = 3;
		radius = 288.0f;
		// S_CHAINGLIGHT1/2 are SPR_CHGG frames C and D drawn full bright,
		// and in Doom 64 those frames are blue rather than the usual
		// muzzle yellow. The light it throws has to be blue to match.
		r = 96; g = 160; b = 255;
		break;

	default:
		duration = 3;
		radius = 288.0f;
		r = 255; g = 236; b = 176;
		break;
	}

	elapsed = gametic - muzzle_tic;

	if (elapsed < 0 || elapsed >= duration) {
		muzzle_tic = -1;
		return;
	}

	scale = 1.0f - ((float)elapsed / (float)duration);
	bright = scale * DL_MUZZLE_BRIGHT;

	// place the light slightly in front of the player, at eye height
	{
		angle_t an = player->mo->angle >> ANGLETOFINESHIFT;
		fixed_t lx = player->mo->x + FixedMul(16 * FRACUNIT, finecosine[an]);
		fixed_t ly = player->mo->y + FixedMul(16 * FRACUNIT, finesine[an]);
		fixed_t lz = player->viewz;

		R_AddDynLight(lx, ly, lz, radius * scale,
			(int)((float)r * bright),
			(int)((float)g * bright),
			(int)((float)b * bright), dl_muzzle);
	}
}

static void DL_BuildSectorReach(void);

//
// DL_IsSceneryType
//
// Scenery has to be told apart from gameplay emitters because the two are
// collected in separate passes; see R_CollectDynLights.
//

static boolean DL_IsSceneryType(int type) {
	switch (type) {
	case MT_PROP_TORCHYELLOW:
	case MT_PROP_TORCHBLUE:
	case MT_PROP_TORCHRED:
	case MT_PROP_FIRE:
	case MT_PROP_FIREYELLOW:
	case MT_PROP_FIREBLUE:
	case MT_PROP_FIRERED:
	case MT_PROP_CANDLE:
		return true;

	default:
		return false;
	}
}

//
// R_CollectDynLights
//
// Walks the mobj list once per frame and creates a point light for every
// light-emitting thing. Keying on mobj->type rather than sprite lump means
// the Enhanced engine's added monsters get lights for free.
//

void R_CollectDynLights(void) {
	mobj_t* mo;
	player_t* player;
	boolean interpolate;
	int     flicker;
	int     pass;
	fixed_t x, y, z;

	numdynlights = 0;
	dl_dropped = 0;
	R_DynLightSeedRandom();
	dmemset(dl_count, 0, sizeof(dl_count));

	if (!r_dynlights.value) {
		return;
	}

	player = renderplayer;
	interpolate = (boolean)i_interpolateframes.value;

	if (player) {
		R_AddMuzzleFlash(player);
	}

	//
	// Two passes, gameplay emitters first.
	//
	// The 32 slots run out long before the per-class caps do, so whoever is
	// walked first simply wins them. Scenery is spawned at level load and
	// therefore sits at the head of the mobj list, while projectiles are
	// spawned during play and sit at the tail: with a single pass a room
	// full of torches quietly swallows every slot and rockets stop casting
	// light at all. Collecting gameplay lights first makes the budget
	// predictable no matter how much scenery a map contains.
	//
	for (pass = 0; pass < 2; pass++) {

	for (mo = mobjhead.next; mo != &mobjhead; mo = mo->next) {
		if (DL_IsSceneryType(mo->type) != (pass == 1)) {
			continue;
		}

		// small per-frame flicker, exactly as the Dreamcast port does
		flicker = R_DynLightRandom() % 24;

		x = R_Interpolate(mo->x, mo->frame_x, interpolate);
		y = R_Interpolate(mo->y, mo->frame_y, interpolate);
		z = R_Interpolate(mo->z, mo->frame_z, interpolate);

		switch (mo->type) {
			//
			// player weapons
			//
		case MT_PROJ_ROCKET:
			R_AddDynLight(x, y, z + (8 * FRACUNIT), 304.0f,
				255 - flicker, 127 - flicker, 0, dl_rocket);
			break;

		case MT_PROJ_PLASMA:
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 304.0f,
				48, 48, 255 - flicker, dl_plasma);
			break;

		case MT_PROJ_BFG:
		case MT_BFGSPREAD:
			R_AddDynLight(x, y, z + (32 * FRACUNIT), 320.0f,
				32, 255 - flicker, 32, dl_bfg);
			break;

		case MT_PROJ_LASER:
			// the puff left where the beam lands
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 288.0f,
				255 - flicker, 0, 0, dl_laser);
			break;

		case MT_LASERMARKER:
			//
			// The unmaker's beam is not a projectile.
			//
			// A_FireLaser spawns MT_PROJ_LASER at the *impact point* and
			// builds the beam itself as a chain of laser_t segments hung off
			// this marker's extradata. Lighting MT_PROJ_LASER alone therefore
			// lit only the far wall, which is what it looked like in play.
			//
			// The Dreamcast keys its laser light on MF_RENDERLASER, that is
			// on this marker, and T_LaserThinker advances laser->x1 along the
			// beam every tic - so the light travels with the beam head rather
			// than waiting at the end of it.
			//
			{
				laser_t* beam = (laser_t*)mo->extradata;

				if (beam) {
					R_AddDynLight(beam->x1, beam->y1, beam->z1, 304.0f,
						255 - flicker, 0, 0, dl_laser);
				}
			}
			break;

			//
			// monster projectiles
			//
		case MT_PROJ_IMP1:
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 280.0f,
				255 - flicker, 127 - flicker, 0, dl_impball);
			break;

		case MT_PROJ_IMP2:
		case MT_PROJ_NIGHTMAREHEAD:
			// nightmare imp: violet, matching the Dreamcast colours
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 280.0f,
				0x1a + 0x8a - flicker, 0x1a + 0x2b - flicker,
				0x1a + 0xe2 - flicker, dl_niteball);
			break;

		case MT_PROJ_HEAD:
			R_AddDynLight(x, y, z + (20 * FRACUNIT), 256.0f,
				255 - flicker, 63 - flicker, 0, dl_cacoball);
			break;

			// CAREFUL: the projectile numbering is crossed relative to the
			// monster numbering. P_MissileAttack (p_enemy.c) maps
			//   MT_BRUISER1 (baron of hell) -> MT_PROJ_BRUISER2
			//   MT_BRUISER2 (hell knight)   -> MT_PROJ_BRUISER1
			// The spawn states confirm it: MT_PROJ_BRUISER1 uses S_BGBALL
			// (bruiser green ball, SPR_BAL7) and MT_PROJ_BRUISER2 uses
			// S_BRBALL (bruiser red ball, SPR_BAL8).
		case MT_PROJ_BRUISER1:
			// fired by the hell knight: green ball
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 256.0f,
				0, 255 - flicker, 0, dl_knightfire);
			break;

		case MT_PROJ_BRUISER2:
			// fired by the baron of hell: red ball
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 256.0f,
				255 - flicker, 0, 0, dl_baronfire);
			break;

		case MT_PROJ_BABY:
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 224.0f,
				0x8a - flicker, 0xa3 - flicker, 0xfa - flicker, dl_spidershot);
			break;

		case MT_PROJ_FATSO:
			R_AddDynLight(x, y, z + (26 * FRACUNIT), 256.0f,
				255 - flicker, 127 - flicker, 0, dl_mancrocket);
			break;

		case MT_PROJ_TRACER:
		case MT_PROJ_UNDEAD:
			R_AddDynLight(x, y, z + (20 * FRACUNIT), 256.0f,
				255 - flicker, 127 - flicker, 0, dl_tracer);
			break;

		case MT_PROJ_RECT:
		case MT_PROJ_RECTFIRE:
			R_AddDynLight(x, y, z + (20 * FRACUNIT), 256.0f,
				255 - flicker, 0, 0, dl_motherfire);
			break;

		case MT_FIRE:
			// arch-vile fire
			R_AddDynLight(x, y, z + (24 * FRACUNIT), 224.0f,
				255 - flicker, 160 - flicker, 48, dl_vilefire);
			break;

		case MT_SKULL:
			// lost souls glow while charging
			R_AddDynLight(x, y, z + (40 * FRACUNIT), 224.0f,
				128 - flicker, 63 - flicker, 0, dl_skull);
			break;

			//
			// explosions: brief, bright, wide
			//
		case MT_EXPLOSION1:
		case MT_EXPLOSION2:
			R_AddDynLight(x, y, z + (16 * FRACUNIT), 448.0f,
				255 - flicker, 144 - flicker, 32, dl_explosion);
			break;

			//
			// scenery: torches, flames and candles.
			// Colours and radii follow the Dreamcast values, minus its
			// per-map repositioning hacks, which only exist to work around
			// that port's own placement quirks.
			//
		case MT_PROP_TORCHYELLOW:
			R_AddPropLight(x, y, z + (45 * FRACUNIT), 192.0f,
				192 - flicker, 160 - flicker, 64 - flicker, dl_torchyellow);
			break;

		case MT_PROP_TORCHBLUE:
			R_AddPropLight(x, y, z + (45 * FRACUNIT), 192.0f,
				64 - flicker, 64 - flicker, 255 - flicker, dl_torchblue);
			break;

		case MT_PROP_TORCHRED:
			R_AddPropLight(x, y, z + (45 * FRACUNIT), 192.0f,
				192 - flicker, 32, 32, dl_torchred);
			break;

		case MT_PROP_FIRE:
			R_AddPropLight(x, y, z + (50 * FRACUNIT), 224.0f,
				255 - flicker, 127 - flicker, 39, dl_fire);
			break;

		case MT_PROP_FIREYELLOW:
			R_AddPropLight(x, y, z + (35 * FRACUNIT), 160.0f,
				192 - flicker, 160 - flicker, 64 - flicker, dl_fireyellow);
			break;

		case MT_PROP_FIREBLUE:
			R_AddPropLight(x, y, z + (35 * FRACUNIT), 128.0f,
				64 - flicker, 64 - flicker, 255 - flicker, dl_fireblue);
			break;

		case MT_PROP_FIRERED:
			R_AddPropLight(x, y, z + (35 * FRACUNIT), 128.0f,
				192 - flicker, 32, 32, dl_firered);
			break;

		case MT_PROP_CANDLE:
			R_AddPropLight(x, y, z + (32 * FRACUNIT), 128.0f,
				224 - flicker, 102 - flicker, 32, dl_candle);
			break;

		default:
			break;
		}
	}

	}   // pass

	// work out which sectors each light can actually reach
	DL_BuildSectorReach();
}

//
// R_ApplyDynLights
//
// Adds the contribution of every dynamic light to a run of world-space
// vertices that already carry their static sector colour.
//
// Components are accumulated in floating point and, if the result exceeds
// full brightness, the whole colour is scaled down rather than clamped
// per-component. Clamping each component separately shifts the hue toward
// white; scaling keeps a red light red as it saturates. This is the same
// trick assign_lightcolor() uses in the Dreamcast port.
//

void R_ApplyDynLights(vtx_t* v, int count, dlmask_t mask) {
	int         i;
	int         j;
	int         activecount;
	float       intensity;
	dynlight_t* dl;
	dynlight_t* active[MAX_DYNLIGHTS];

	if (!r_dynlights.value || numdynlights <= 0 || count <= 0 || mask == 0) {
		return;
	}

	intensity = (float)r_dynlightintensity.value / 5.0f;

	if (intensity <= 0.0f) {
		return;
	}

	//
	// Collect the lights named by the mask once, instead of walking all 64
	// slots and testing a bit for every single vertex. A subdivided wall can
	// carry a few hundred vertices, so this inner loop is the hottest code
	// in the whole system.
	//
	{
		int n = 0;

		for (j = 0; j < numdynlights; j++) {
			if (mask & (1ull << j)) {
				active[n++] = &dynlights[j];
			}
		}

		activecount = n;
	}

	if (activecount == 0) {
		return;
	}

	for (i = 0; i < count; i++) {
		float ar = 0.0f;
		float ag = 0.0f;
		float ab = 0.0f;
		float vr, vg, vb;
		float maxc;

		for (j = 0; j < activecount; j++) {
			float dx, dy, dz, d2, dist, scale;

			dl = active[j];

			dx = dl->x - v[i].x;
			dy = dl->y - v[i].y;
			dz = dl->z - v[i].z;
			d2 = (dx * dx) + (dy * dy) + (dz * dz);

			if (d2 >= (dl->radius * dl->radius)) {
				continue;
			}

			dist = (float)sqrt((double)d2);

			// linear attenuation, as on the Dreamcast
			scale = (dl->radius - dist) * dl->rcpradius;

			ar += dl->r * scale;
			ag += dl->g * scale;
			ab += dl->b * scale;
		}

		if (ar <= 0.0f && ag <= 0.0f && ab <= 0.0f) {
			continue;
		}

		vr = ((float)v[i].r / 255.0f) + (ar * intensity);
		vg = ((float)v[i].g / 255.0f) + (ag * intensity);
		vb = ((float)v[i].b / 255.0f) + (ab * intensity);

		maxc = 1.0f;

		if (vr > maxc) { maxc = vr; }
		if (vg > maxc) { maxc = vg; }
		if (vb > maxc) { maxc = vb; }

		maxc = 255.0f / maxc;

		v[i].r = (byte)(vr * maxc);
		v[i].g = (byte)(vg * maxc);
		v[i].b = (byte)(vb * maxc);
	}
}




//=============================================================================
//
// Light reachability
//
// The side tests alone are not enough. A wall plane is infinite, so a light
// standing in the next room can still land on the correct side of it and
// shine through. What actually decides whether light reaches a surface is
// whether there is an opening between the light and that surface.
//
// So each light floods outward from its own sector, crossing only two-sided
// lines that are within reach and actually open, and records every sector it
// arrives in. A surface is then lit only by the lights that reached its
// sector. This is the same idea as marking lights onto BSP leaves in the
// Quake lineage, expressed with the sector graph Doom already maintains.
//
//=============================================================================

static dlmask_t* dl_sectorlights = NULL;
static int           dl_sectorlightcount = 0;

// how many sector hops a light may travel. Radii are around 300 units and
// Doom 64 sectors are large, so anything past a few hops is unreachable in
// practice; the cap just stops pathological recursion on sector soup.
#define DL_MAX_SECTOR_HOPS  4

//
// DL_DistToLineXY
// Shortest distance from a point to a linedef, treated as a segment.
//

static float DL_DistToLineXY(float px, float py, line_t* li) {
	float x1 = F2D3D(li->v1->x);
	float y1 = F2D3D(li->v1->y);
	float x2 = F2D3D(li->v2->x);
	float y2 = F2D3D(li->v2->y);
	float dx = x2 - x1;
	float dy = y2 - y1;
	float len2 = (dx * dx) + (dy * dy);
	float t;
	float cx, cy;

	if (len2 < 0.001f) {
		dx = px - x1;
		dy = py - y1;
		return (float)sqrt((double)((dx * dx) + (dy * dy)));
	}

	t = (((px - x1) * dx) + ((py - y1) * dy)) / len2;

	if (t < 0.0f) { t = 0.0f; }
	if (t > 1.0f) { t = 1.0f; }

	cx = x1 + (dx * t);
	cy = y1 + (dy * t);

	dx = px - cx;
	dy = py - cy;

	return (float)sqrt((double)((dx * dx) + (dy * dy)));
}

//
// DL_MarkSector
//

static void DL_MarkSector(sector_t* sec, dynlight_t* dl, dlmask_t bit, int hops) {
	int i;
	int idx;

	if (!sec) {
		return;
	}

	idx = (int)(sec - sectors);

	if (idx < 0 || idx >= numsectors) {
		return;
	}

	if (dl_sectorlights[idx] & bit) {
		return;         // already reached
	}

	dl_sectorlights[idx] |= bit;

	if (hops <= 0) {
		return;
	}

	for (i = 0; i < sec->linecount; i++) {
		line_t* li = sec->lines[i];
		sector_t* other;
		fixed_t     opentop;
		fixed_t     openbottom;

		if (!li || !li->frontsector || !li->backsector) {
			continue;   // one-sided: solid, light stops here
		}

		other = (li->frontsector == sec) ? li->backsector : li->frontsector;

		if (!other || other == sec) {
			continue;
		}

		// a closed door or a solid step has no opening to pass through
		opentop = (li->frontsector->ceilingheight < li->backsector->ceilingheight) ?
			li->frontsector->ceilingheight : li->backsector->ceilingheight;

		openbottom = (li->frontsector->floorheight > li->backsector->floorheight) ?
			li->frontsector->floorheight : li->backsector->floorheight;

		if (opentop <= openbottom) {
			continue;
		}

		// the light sphere has to overlap that opening vertically
		if ((dl->z + dl->radius) < F2D3D(openbottom)) {
			continue;
		}

		if ((dl->z - dl->radius) > F2D3D(opentop)) {
			continue;
		}

		// and be close enough to shine through it
		if (DL_DistToLineXY(dl->x, dl->y, li) >= dl->radius) {
			continue;
		}

		DL_MarkSector(other, dl, bit, hops - 1);
	}
}

//
// DL_BuildSectorReach
//

static void DL_BuildSectorReach(void) {
	int i;

	if (numsectors <= 0 || !sectors) {
		return;
	}

	if (dl_sectorlightcount != numsectors) {
		if (dl_sectorlights) {
			Z_Free(dl_sectorlights);
		}

		dl_sectorlights = (dlmask_t*)Z_Malloc(
			sizeof(dlmask_t) * numsectors, PU_STATIC, NULL);

		dl_sectorlightcount = numsectors;
	}

	dmemset(dl_sectorlights, 0, sizeof(dlmask_t) * numsectors);

	for (i = 0; i < numdynlights; i++) {
		DL_MarkSector(dynlights[i].sector, &dynlights[i],
			(1ull << i), DL_MAX_SECTOR_HOPS);
	}
}

//
// R_DynLightMaskForSector
//

static dlmask_t R_DynLightMaskForSector(sector_t* sec) {
	int idx;

	if (!dl_sectorlights || !sec) {
		return DL_MASK_ALL;
	}

	idx = (int)(sec - sectors);

	if (idx < 0 || idx >= numsectors) {
		return DL_MASK_ALL;
	}

	return dl_sectorlights[idx];
}

//
// R_DynLightMaskForThing
//
// A sprite is a billboard with no surface plane, so the side tests do not
// apply to it. The sector reach still does: without it a rocket lights up
// monsters standing behind a solid wall.
//

dlmask_t R_DynLightMaskForThing(sector_t* sec) {
	return R_DynLightMaskForSector(sec);
}

//=============================================================================
//
// Occlusion masks
//
// A point light with no visibility data lights whatever is within its radius,
// walls in between included. A full solution would need a visibility or
// shadow pass, which vertex lighting cannot express. These two tests remove
// the cases that actually read as wrong in play: light arriving through the
// face of a wall, and light arriving through a floor or a ceiling.
//
//=============================================================================

//
// R_DynLightMaskForWall
//
// v[0] and v[1] are the two ends of the wall's top edge, so they define the
// wall plane in 2D. A light may only light this wall if it stands on the
// same side of that plane as the camera does. Because the renderer has
// already discarded back-facing walls, "the camera's side" is by definition
// the visible face.
//

dlmask_t R_DynLightMaskForWall(const vtx_t* v, sector_t* sec) {
	dlmask_t mask = 0;
	dlmask_t reach;
	float       dx, dy;
	float       viewside;
	float       len2;
	float       tolerance2;
	int         i;
	dynlight_t* dl;

	if (numdynlights <= 0) {
		return 0;
	}

	reach = R_DynLightMaskForSector(sec);

	if (reach == 0) {
		return 0;
	}

	dx = v[1].x - v[0].x;
	dy = v[1].y - v[0].y;

	// Squared length: only the *sign* of the cross product matters for the
	// side test, and the near-plane tolerance can be compared squared too,
	// so this whole function runs without a single square root or division.
	len2 = (dx * dx) + (dy * dy);

	if (len2 < 0.000001f) {
		return reach;           // degenerate wall, no meaningful side
	}

	tolerance2 = 64.0f * len2;  // (8 units) squared, scaled by the edge length

	viewside = (dx * (fviewy - v[0].y)) - (dy * (fviewx - v[0].x));

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		float lightside;

		if (!(reach & (1ull << i))) {
			continue;           // no opening between the light and this sector
		}

		lightside = (dx * (dl->y - v[0].y)) - (dy * (dl->x - v[0].x));

		// A projectile exploding against the wall sits almost exactly on the
		// plane, where the sign is meaningless, so anything within a few
		// units of the surface is always allowed through.
		if ((lightside * lightside) < tolerance2) {
			mask |= (1ull << i);
			continue;
		}

		if ((lightside < 0.0f) == (viewside < 0.0f)) {
			mask |= (1ull << i);
		}
	}

	return mask;
}

//
// R_DynLightMaskForFlat
//
// Floors and ceilings are horizontal, so the test is simply which side of
// the plane the light is on. This is what stops a rocket in a corridor from
// lighting the floor of the room stacked above it.
//

dlmask_t R_DynLightMaskForFlat(const vtx_t* v, boolean ceiling, sector_t* sec) {
	dlmask_t mask = 0;
	dlmask_t reach;
	float       planez;
	int         i;
	dynlight_t* dl;

	if (numdynlights <= 0) {
		return 0;
	}

	reach = R_DynLightMaskForSector(sec);

	if (reach == 0) {
		return 0;
	}

	planez = v[0].z;

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		if (!(reach & (1ull << i))) {
			continue;           // no opening between the light and this sector
		}

		if (ceiling) {
			if (dl->z <= (planez + 8.0f)) {
				mask |= (1ull << i);
			}
		}
		else {
			if (dl->z >= (planez - 8.0f)) {
				mask |= (1ull << i);
			}
		}
	}

	return mask;
}


//=============================================================================
//
// Line of sight
//
// Sector reachability answers "is there an opening between these two rooms",
// which is not the same question as "can this light see this surface". A
// sector is not convex and need not even be contiguous - the same sector can
// cover two areas separated by a wall - so a light can be marked as reaching
// a sector and still be hidden from most of it.
//
// The only way to settle it is to walk the straight line from the light to
// the surface and see whether a wall gets in the way. That is what this does,
// stepping through the blockmap cells the ray passes over rather than testing
// every linedef in the map.
//
// Two deliberate choices:
//
//   - it does not use validcount. That global is also used by the renderer's
//     BSP walk, and we run in the middle of it; stamping our own array keeps
//     the two from corrupting each other.
//
//   - the test is per surface, taken at its centre. A wall half in shadow is
//     therefore lit or unlit as a whole. Testing per vertex would be exact
//     but would cost hundreds of rays on a subdivided floor.
//
//=============================================================================

static int* dl_linestamp = NULL;
static int  dl_linestampcount = 0;
static int  dl_stamp = 0;

//
// DL_SegmentsCross
// Standard 2D segment intersection, returning the fraction along a->b.
//

static boolean DL_SegmentsCross(float ax, float ay, float bx, float by,
	float cx, float cy, float dx, float dy, float* frac) {
	float rx = bx - ax;
	float ry = by - ay;
	float sx = dx - cx;
	float sy = dy - cy;
	float denom = (rx * sy) - (ry * sx);
	float t, u;

	if (denom > -0.0001f && denom < 0.0001f) {
		return false;           // parallel
	}

	t = (((cx - ax) * sy) - ((cy - ay) * sx)) / denom;
	u = (((cx - ax) * ry) - ((cy - ay) * rx)) / denom;

	if (t <= 0.0f || t >= 1.0f || u <= 0.0f || u >= 1.0f) {
		return false;
	}

	*frac = t;

	return true;
}

//
// DL_LineBlocksSight
//

static boolean DL_LineBlocksSight(line_t* li, float lx, float ly, float lz,
	float tx, float ty, float tz,
	fixed_t rminx, fixed_t rmaxx, fixed_t rminy, fixed_t rmaxy) {
	float   x1;

	//
	// Cheap reject first.
	//
	// p_setup already stores a bounding box on every linedef, so four integer
	// comparisons can throw out the great majority of the lines in a blockmap
	// cell before doing any float work at all. Measurements put this test at
	// roughly seventy percent of the whole lighting cost, so what matters is
	// not how fast one intersection is but how few of them are reached.
	//
	if (li->bbox[BOXRIGHT] < rminx || li->bbox[BOXLEFT] > rmaxx ||
		li->bbox[BOXTOP] < rminy || li->bbox[BOXBOTTOM] > rmaxy) {
		return false;
	}

	x1 = F2D3D(li->v1->x);
	float   y1 = F2D3D(li->v1->y);
	float   x2 = F2D3D(li->v2->x);
	float   y2 = F2D3D(li->v2->y);
	float   frac;
	float   z;
	float   opentop, openbottom;

	if (!DL_SegmentsCross(lx, ly, tx, ty, x1, y1, x2, y2, &frac)) {
		return false;
	}

	if (!li->frontsector || !li->backsector) {
		return true;            // one-sided wall: opaque
	}

	opentop = F2D3D((li->frontsector->ceilingheight < li->backsector->ceilingheight) ?
		li->frontsector->ceilingheight : li->backsector->ceilingheight);

	openbottom = F2D3D((li->frontsector->floorheight > li->backsector->floorheight) ?
		li->frontsector->floorheight : li->backsector->floorheight);

	if (opentop <= openbottom) {
		return true;            // closed door or solid step
	}

	// height the ray has reached where it crosses this line
	z = lz + ((tz - lz) * frac);

	if (z < openbottom || z > opentop) {
		return true;            // passes into the solid part above or below
	}

	return false;
}

//
// DL_SegmentHitsCell
//
// Slab test of the ray against one blockmap cell.
//
// The bounding box of a 304-unit ray spans up to sixteen cells, but the ray
// itself only crosses three or four of them. Iterating the linedefs of a cell
// the ray misses is pure waste, and with twenty-odd lines per cell in a
// detailed map that waste dominates the entire lighting system. Twenty float
// operations here replace four hundred there.
//

static boolean DL_SegmentHitsCell(float px, float py, float qx, float qy,
	float x0, float y0, float x1, float y1) {
	float dx = qx - px;
	float dy = qy - py;
	float tmin = 0.0f;
	float tmax = 1.0f;
	float t1, t2, tmp;

	if (dx > -0.0001f && dx < 0.0001f) {
		if (px < x0 || px > x1) {
			return false;
		}
	}
	else {
		t1 = (x0 - px) / dx;
		t2 = (x1 - px) / dx;

		if (t1 > t2) { tmp = t1; t1 = t2; t2 = tmp; }
		if (t1 > tmin) { tmin = t1; }
		if (t2 < tmax) { tmax = t2; }

		if (tmin > tmax) {
			return false;
		}
	}

	if (dy > -0.0001f && dy < 0.0001f) {
		if (py < y0 || py > y1) {
			return false;
		}
	}
	else {
		t1 = (y0 - py) / dy;
		t2 = (y1 - py) / dy;

		if (t1 > t2) { tmp = t1; t1 = t2; t2 = tmp; }
		if (t1 > tmin) { tmin = t1; }
		if (t2 < tmax) { tmax = t2; }

		if (tmin > tmax) {
			return false;
		}
	}

	return true;
}

//
// DL_SightBlocked
//
// Walks the blockmap cells covered by the ray's bounding box. A light reaches
// at most its radius, so that box spans only a handful of cells.
//

//
// How far the ray is pulled back from each end, in map units. Large enough to
// clear floating point noise on the surface plane, small enough that no real
// wall can hide inside it.
//
#define DL_SIGHT_BIAS   6.0f

static boolean DL_SightBlocked(float lx, float ly, float lz,
	float tx, float ty, float tz) {
	int     bx, by;
	int     bx1, bx2, by1, by2;
	float   minx, maxx, miny, maxy;
	fixed_t rminx, rmaxx, rminy, rmaxy;
	float   forgx = F2D3D(bmaporgx);
	float   forgy = F2D3D(bmaporgy);

	if (!blockmap || !blockmaplump || !dl_linestamp) {
		return false;
	}

	//
	// Pull both ends of the ray in before tracing.
	//
	// The target is the centre of the surface being lit, which lies exactly
	// on that surface's own plane, and the light is often sitting against a
	// wall as well - a plasma bolt striking one, for instance. The endpoint
	// therefore falls on a linedef, and whether the intersection registers
	// comes down to whether the computed centre lands a thousandth of a unit
	// in front of the plane or behind it. Behind it, and the wall reports
	// itself as blocking its own light.
	//
	// Shortening the ray at both ends removes the ambiguity without opening
	// a path through anything thicker than the bias.
	//
	{
		float   ex = tx - lx;
		float   ey = ty - ly;
		float   ez = tz - lz;
		float   elen = (float)sqrt((double)((ex * ex) + (ey * ey) + (ez * ez)));

		if (elen > (DL_SIGHT_BIAS * 3.0f)) {
			float scale = DL_SIGHT_BIAS / elen;

			lx += ex * scale;
			ly += ey * scale;
			lz += ez * scale;

			tx -= ex * scale;
			ty -= ey * scale;
			tz -= ez * scale;
		}
		else {
			// too short for anything to fit in between
			return false;
		}
	}

	minx = (lx < tx) ? lx : tx;
	maxx = (lx < tx) ? tx : lx;
	miny = (ly < ty) ? ly : ty;
	maxy = (ly < ty) ? ty : ly;

	// the ray's own box, in map units, for the per-line reject below
	rminx = (fixed_t)(minx * FRACUNIT);
	rmaxx = (fixed_t)(maxx * FRACUNIT);
	rminy = (fixed_t)(miny * FRACUNIT);
	rmaxy = (fixed_t)(maxy * FRACUNIT);

	bx1 = (int)((minx - forgx) / (float)MAPBLOCKUNITS);
	bx2 = (int)((maxx - forgx) / (float)MAPBLOCKUNITS);
	by1 = (int)((miny - forgy) / (float)MAPBLOCKUNITS);
	by2 = (int)((maxy - forgy) / (float)MAPBLOCKUNITS);

	if (bx1 < 0) { bx1 = 0; }
	if (by1 < 0) { by1 = 0; }
	if (bx2 >= bmapwidth) { bx2 = bmapwidth - 1; }
	if (by2 >= bmapheight) { by2 = bmapheight - 1; }

	if (bx1 > bx2 || by1 > by2) {
		return false;
	}

	dl_stamp++;

	for (by = by1; by <= by2; by++) {
		for (bx = bx1; bx <= bx2; bx++) {
			int     offset;
			short* list;
			float   cx0 = forgx + (float)(bx * MAPBLOCKUNITS);
			float   cy0 = forgy + (float)(by * MAPBLOCKUNITS);

			if (!DL_SegmentHitsCell(lx, ly, tx, ty,
				cx0, cy0, cx0 + MAPBLOCKUNITS, cy0 + MAPBLOCKUNITS)) {
				continue;
			}

			// Same access pattern as P_BlockLinesIterator: the offset is a
			// byte-swapped unsigned short, the list itself is signed and
			// terminated by -1, and there is no leading delimiter to skip.
			// Reading it any other way silently walks the wrong linedefs.
			offset = (by * bmapwidth) + bx;
			offset = (uint16_t)SHORT(*(blockmap + offset));

			for (list = (short*)blockmaplump + offset; *list != -1; list++) {
				int     idx = SHORT(*list);
				line_t* li;

				if (idx < 0 || idx >= numlines) {
					continue;
				}

				if (dl_linestamp[idx] == dl_stamp) {
					continue;   // already tested for this ray
				}

				dl_linestamp[idx] = dl_stamp;

				li = &lines[idx];

				if (DL_LineBlocksSight(li, lx, ly, lz, tx, ty, tz,
					rminx, rmaxx, rminy, rmaxy)) {
					return true;
				}
			}
		}
	}

	return false;
}

//
// R_DynLightMaskLineOfSight
//
// Narrows a mask down to the lights that can actually see the given point.
//

dlmask_t R_DynLightMaskLineOfSight(dlmask_t mask, float x, float y, float z) {
	int         i;
	dlmask_t    out = mask;
	dynlight_t* dl;

	if (mask == 0 || !r_dynlightshadows.value) {
		return mask;
	}

	if (dl_linestampcount != numlines) {
		if (dl_linestamp) {
			Z_Free(dl_linestamp);
		}

		dl_linestamp = (int*)Z_Malloc(sizeof(int) * (numlines > 0 ? numlines : 1),
			PU_STATIC, NULL);

		dmemset(dl_linestamp, 0, sizeof(int) * (numlines > 0 ? numlines : 1));

		dl_linestampcount = numlines;
		dl_stamp = 0;
	}

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		if (!(out & (1ull << i))) {
			continue;
		}

		if (DL_SightBlocked(dl->x, dl->y, dl->z, x, y, z)) {
			out &= ~(1ull << i);
		}
	}

	return out;
}

//=============================================================================
//
// Tessellation of lit surfaces
//
//=============================================================================

//
// Quality presets.
//
// "step" is the target edge length in world units: a surface is split until
// its pieces are roughly that size. "maxdiv" caps the split count so that a
// single enormous polygon cannot eat the whole vertex buffer.
//
static const float dl_quality_step[4] = { 0.0f, 192.0f, 96.0f, 48.0f };
static const int   dl_quality_maxdiv[4] = { 0,     3,      6,     10 };
static const int   dl_quality_maxdepth[4] = { 0,     2,      3,      4 };

//
// Leave headroom in the vertex buffer. DL_ProcessDrawList calls I_Error the
// moment drawcount reaches MAXDLDRAWCOUNT, so we stop subdividing well before
// that and fall back to flat-shaded surfaces rather than killing the game.
//
#define DL_VERTEX_HEADROOM  4096

static int R_DynLightQuality(void) {
	int q = (int)r_dynlightquality.value;

	if (q < 0) { q = 0; }
	if (q > 3) { q = 3; }

	return q;
}

//
// DL_LodStep
//
// Widens the target piece size with distance from the camera. Tessellation
// only exists to make the falloff readable, and a wall fifteen hundred units
// away occupies a few pixels: splitting it as finely as one at arm's length
// spends vertices where nothing can be seen.
//

static float DL_LodStep(float step, const vtx_t* v) {
	float dx = v->x - fviewx;
	float dy = v->y - fviewy;
	float dist = (float)sqrt((double)((dx * dx) + (dy * dy)));

	return step * (1.0f + (dist * (1.0f / 768.0f)));
}

//
// R_DynLightsSurfaceMask
//
// Sphere-vs-AABB test against every active light. Testing the box rather
// than the vertices matters: a rocket flying over the middle of a large
// floor is out of range of all four corners, yet must still light it.
//
// It returns the surviving lights rather than a yes/no, because the caller
// must narrow the set down *before* paying for sight rays. Tracing first and
// range-testing afterwards, which is what this code used to do, fired a ray
// for every surface in the light's sector - including surfaces far outside
// the radius that could never be lit whatever the answer.
//

dlmask_t R_DynLightsSurfaceMask(const vtx_t* v, int count, dlmask_t mask) {
	float       minx, miny, minz;
	float       maxx, maxy, maxz;
	dlmask_t    out = 0;
	int         i;
	dynlight_t* dl;

	if (numdynlights <= 0 || count <= 0 || mask == 0) {
		return 0;
	}

	minx = maxx = v[0].x;
	miny = maxy = v[0].y;
	minz = maxz = v[0].z;

	for (i = 1; i < count; i++) {
		if (v[i].x < minx) { minx = v[i].x; } else if (v[i].x > maxx) { maxx = v[i].x; }
		if (v[i].y < miny) { miny = v[i].y; } else if (v[i].y > maxy) { maxy = v[i].y; }
		if (v[i].z < minz) { minz = v[i].z; } else if (v[i].z > maxz) { maxz = v[i].z; }
	}

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		float cx;

		if (!(mask & (1ull << i))) {
			continue;
		}

		cx = dl->x;
		float cy = dl->y;
		float cz = dl->z;
		float dx, dy, dz;

		// closest point on the box to the light centre
		if (cx < minx) { cx = minx; } else if (cx > maxx) { cx = maxx; }
		if (cy < miny) { cy = miny; } else if (cy > maxy) { cy = maxy; }
		if (cz < minz) { cz = minz; } else if (cz > maxz) { cz = maxz; }

		dx = cx - dl->x;
		dy = cy - dl->y;
		dz = cz - dl->z;

		if (((dx * dx) + (dy * dy) + (dz * dz)) < (dl->radius * dl->radius)) {
			out |= (1ull << i);
		}
	}

	return out;
}

//
// DL_LerpVertex
//

static void DL_LerpVertex(vtx_t* out, const vtx_t* a, const vtx_t* b, float t) {
	float it = 1.0f - t;

	out->x = (a->x * it) + (b->x * t);
	out->y = (a->y * it) + (b->y * t);
	out->z = (a->z * it) + (b->z * t);
	out->tu = (a->tu * it) + (b->tu * t);
	out->tv = (a->tv * it) + (b->tv * t);

	out->r = (byte)(((float)a->r * it) + ((float)b->r * t));
	out->g = (byte)(((float)a->g * it) + ((float)b->g * t));
	out->b = (byte)(((float)a->b * it) + ((float)b->b * t));
	out->a = (byte)(((float)a->a * it) + ((float)b->a * t));
}

//
// DL_Dist2D / DL_Dist3D
//

static float DL_Dist3D(const vtx_t* a, const vtx_t* b) {
	float dx = a->x - b->x;
	float dy = a->y - b->y;
	float dz = a->z - b->z;

	return (float)sqrt((double)((dx * dx) + (dy * dy) + (dz * dz)));
}

//
// R_SubdivideWall
//
// A wall is a quad laid out by R_Generate*SegPlane as
//
//     0 ----- 1      triangles (0,1,2) and (3,2,1)
//     |     / |
//     2 ----- 3
//
// so v0/v1 are the top edge and v2/v3 the bottom. Both edges are split
// bilinearly, which keeps sloped walls (differing floor heights at each
// end) correct.
//

int R_SubdivideWall(const vtx_t* src, int base, dlmask_t mask) {
	int     q;
	int     nu, nw;
	int     iu, iw;
	int     pos;
	float   step;
	float   wu, wv;
	vtx_t   top, bot;

	q = R_DynLightQuality();

	if (q == 0 || mask == 0) {
		return 0;
	}

	step = DL_LodStep(dl_quality_step[q], &src[0]);

	// horizontal extent along the top edge, vertical extent down the left
	wu = DL_Dist3D(&src[0], &src[1]);
	wv = DL_Dist3D(&src[0], &src[2]);

	nu = (int)ceil((double)(wu / step));
	nw = (int)ceil((double)(wv / step));

	if (nu > dl_quality_maxdiv[q]) { nu = dl_quality_maxdiv[q]; }
	if (nw > dl_quality_maxdiv[q]) { nw = dl_quality_maxdiv[q]; }
	if (nu < 1) { nu = 1; }
	if (nw < 1) { nw = 1; }

	if (nu == 1 && nw == 1) {
		return 0;   // nothing to gain
	}

	//
	// Shared grid of (nu+1) x (nw+1) vertices.
	//
	// Emitting four independent vertices per sub-quad was simpler, but every
	// interior vertex then existed four times over, and each copy paid for a
	// full bilinear interpolation and then a full lighting calculation. On a
	// 6x6 grid that is 144 vertices where 49 carry the same information.
	//
	if ((base + ((nu + 1) * (nw + 1))) > (MAXDLDRAWCOUNT - DL_VERTEX_HEADROOM)) {
		return 0;
	}

	pos = base;

	for (iw = 0; iw <= nw; iw++) {
		float w = (float)iw / (float)nw;

		for (iu = 0; iu <= nu; iu++) {
			float u = (float)iu / (float)nu;

			DL_LerpVertex(&top, &src[0], &src[1], u);
			DL_LerpVertex(&bot, &src[2], &src[3], u);
			DL_LerpVertex(&drawVertex[pos], &top, &bot, w);

			pos++;
		}
	}

	// same winding as the original quad: (0,1,2) then (3,2,1)
	for (iw = 0; iw < nw; iw++) {
		for (iu = 0; iu < nu; iu++) {
			int i0 = base + (iw * (nu + 1)) + iu;
			int i1 = i0 + 1;
			int i2 = i0 + (nu + 1);
			int i3 = i2 + 1;

			dglTriangle(i0, i1, i2);
			dglTriangle(i3, i2, i1);
		}
	}

	return pos - base;
}

//
// DL_EmitTriangle
//
// Recursive 1-to-4 midpoint split, the same scheme the Dreamcast port uses
// for its floor planes (s12 / s23 / s31 in r_phase3.c).
//

static boolean DL_TriangleLit(const vtx_t* a, const vtx_t* b, const vtx_t* c,
	dlmask_t mask) {
	float       cx = (a->x + b->x + c->x) * (1.0f / 3.0f);
	float       cy = (a->y + b->y + c->y) * (1.0f / 3.0f);
	float       cz = (a->z + b->z + c->z) * (1.0f / 3.0f);
	float       rad2 = 0.0f;
	const vtx_t* p[3];
	int         i;
	dynlight_t* dl;

	p[0] = a; p[1] = b; p[2] = c;

	// bounding sphere of the triangle
	for (i = 0; i < 3; i++) {
		float dx = p[i]->x - cx;
		float dy = p[i]->y - cy;
		float dz = p[i]->z - cz;
		float d2 = (dx * dx) + (dy * dy) + (dz * dz);

		if (d2 > rad2) {
			rad2 = d2;
		}
	}

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		float dx, dy, dz, sum;

		if (!(mask & (1ull << i))) {
			continue;
		}

		dx = dl->x - cx;
		dy = dl->y - cy;
		dz = dl->z - cz;
		sum = dl->radius + (float)sqrt((double)rad2);

		if (((dx * dx) + (dy * dy) + (dz * dz)) < (sum * sum)) {
			return true;
		}
	}

	return false;
}

static void DL_EmitTriangle(const vtx_t* a, const vtx_t* b, const vtx_t* c,
	int depth, int* pos, int limit, dlmask_t mask) {
	vtx_t   ab, bc, ca;
	vtx_t* out;

	// Splitting a piece no light can reach would only burn vertices, so
	// the budget is spent where the falloff is actually visible. This is
	// what lets the High preset stay affordable on large floors.
	if (depth > 0 && !DL_TriangleLit(a, b, c, mask)) {
		depth = 0;
	}

	if (depth > 0) {
		DL_LerpVertex(&ab, a, b, 0.5f);
		DL_LerpVertex(&bc, b, c, 0.5f);
		DL_LerpVertex(&ca, c, a, 0.5f);

		DL_EmitTriangle(a, &ab, &ca, depth - 1, pos, limit, mask);
		DL_EmitTriangle(&ab, b, &bc, depth - 1, pos, limit, mask);
		DL_EmitTriangle(&ca, &bc, c, depth - 1, pos, limit, mask);
		DL_EmitTriangle(&ab, &bc, &ca, depth - 1, pos, limit, mask);
		return;
	}

	if ((*pos + 3) > limit) {
		return;
	}

	out = &drawVertex[*pos];

	out[0] = *a;
	out[1] = *b;
	out[2] = *c;

	dglTriangle(*pos + 0, *pos + 1, *pos + 2);

	*pos += 3;
}

//
// R_SubdivideFlat
//
// Floors and ceilings arrive as a triangle fan around the first leaf vertex.
// Each fan triangle is split independently.
//

int R_SubdivideFlat(const vtx_t* src, int count, int base, dlmask_t mask) {
	int     q;
	int     j;
	int     pos;
	int     depth;
	int     limit;
	float   step;
	float   longest;

	q = R_DynLightQuality();

	if (q == 0 || count < 3 || mask == 0) {
		return 0;
	}

	step = DL_LodStep(dl_quality_step[q], &src[0]);

	// pick a split depth from the widest fan triangle
	longest = 0.0f;

	for (j = 0; j < count - 2; j++) {
		float d;

		d = DL_Dist3D(&src[0], &src[1 + j]);
		if (d > longest) { longest = d; }

		d = DL_Dist3D(&src[1 + j], &src[2 + j]);
		if (d > longest) { longest = d; }
	}

	depth = 0;

	while (depth < dl_quality_maxdepth[q] && longest > step) {
		longest *= 0.5f;
		depth++;
	}

	if (depth == 0) {
		return 0;
	}

	// 4^depth triangles per fan triangle, three vertices each
	{
		int pertri = 3;
		int k;

		// Worst case is 4^depth triangles per fan triangle. DL_EmitTriangle
		// prunes unlit branches so the real figure is much lower, but the
		// reservation stays pessimistic: overshooting the vertex buffer
		// would call I_Error and take the game down mid-frame.
		for (k = 0; k < depth; k++) {
			pertri *= 4;
		}

		if ((base + ((count - 2) * pertri)) > (MAXDLDRAWCOUNT - DL_VERTEX_HEADROOM)) {
			return 0;
		}
	}

	limit = MAXDLDRAWCOUNT - DL_VERTEX_HEADROOM;
	pos = base;

	// same winding as the original fan: (v0, v[1+j], v[2+j])
	for (j = 0; j < count - 2; j++) {
		DL_EmitTriangle(&src[0], &src[1 + j], &src[2 + j], depth, &pos, limit, mask);
	}

	return pos - base;
}

//
// R_DynLightApplyToWeapon
//
// The weapon in the player's hands is not world geometry: it is a 2D quad in
// ortho space, so it carries no position the lighting loop could use. The
// Dreamcast solves this by sampling the lights at a single point just in front
// of the player and folding the result into the quad's colour, and that is
// what happens here.
//
// The sample sits slightly ahead of and below the eye, roughly where the
// weapon is held. The muzzle flash light lives near that same spot, so firing
// lights up your own gun - which is the point.
//
// The contribution is deliberately capped below full strength. The weapon
// fills a large part of the screen and never moves relative to the camera, so
// at full intensity a passing projectile would wash it out to a flat colour
// rather than glint off it.
//
#define DL_WEAPON_SCALE 0.55f

rcolor R_DynLightApplyToWeapon(rcolor color, player_t* player) {
	float       ar = 0.0f;
	float       ag = 0.0f;
	float       ab = 0.0f;
	float       vr, vg, vb, maxc;
	float       intensity;
	dlmask_t    mask;
	fixed_t     sx, sy, sz;
	float       fx, fy, fz;
	int         i;
	dynlight_t* dl;
	int         cr, cg, cb, ca;

	if (!r_dynlights.value || !r_dynlightweapon.value || numdynlights <= 0) {
		return color;
	}

	if (!player->mo) {
		return color;
	}

	{
		angle_t an = player->mo->angle >> ANGLETOFINESHIFT;

		//
		// 8 units, the same offset the Dreamcast uses, and deliberately
		// well inside the player's 19 unit collision radius. A sample any
		// further forward can land on the far side of a wall the player is
		// standing against: the sight test then reports the point as hidden
		// and the weapon stops catching light exactly when pressed into a
		// corner, which is where the effect is most visible.
		//
		sx = player->mo->x + FixedMul(8 * FRACUNIT, finecosine[an]);
		sy = player->mo->y + FixedMul(8 * FRACUNIT, finesine[an]);
		sz = player->viewz - (8 * FRACUNIT);
	}

	mask = R_DynLightMaskForSector(player->mo->subsector->sector);

	if (mask == 0) {
		return color;
	}

	fx = F2D3D(sx);
	fy = F2D3D(sy);
	fz = F2D3D(sz);

	// a light the player cannot see must not glint off the weapon either
	mask = R_DynLightMaskLineOfSight(mask, fx, fy, fz);

	if (mask == 0) {
		return color;
	}

	dl = dynlights;

	for (i = 0; i < numdynlights; i++, dl++) {
		float dx, dy, dz, d2, dist, scale;

		if (!(mask & (1ull << i))) {
			continue;
		}

		dx = dl->x - fx;
		dy = dl->y - fy;
		dz = dl->z - fz;
		d2 = (dx * dx) + (dy * dy) + (dz * dz);

		if (d2 >= (dl->radius * dl->radius)) {
			continue;
		}

		dist = (float)sqrt((double)d2);
		scale = (dl->radius - dist) * dl->rcpradius;

		ar += dl->r * scale;
		ag += dl->g * scale;
		ab += dl->b * scale;
	}

	if (ar <= 0.0f && ag <= 0.0f && ab <= 0.0f) {
		return color;
	}

	intensity = ((float)r_dynlightintensity.value / 5.0f) * DL_WEAPON_SCALE;

	//
	// rcolor is laid out by D_RGBA, whose byte order flips with the host
	// endianness. Unpacking with hard-coded shifts got red and alpha the
	// wrong way round on a little endian build, so the weapon's own alpha
	// was overwritten with its red level and the sprite faded out.
	//
	// Rebuilding the two reference colours at run time lets the shifts be
	// derived from whichever D_RGBA the build actually compiled.
	//
	{
		const rcolor probe_r = D_RGBA(255, 0, 0, 0);
		const rcolor probe_a = D_RGBA(0, 0, 0, 255);
		int shift_r = 0;
		int shift_g = 0;
		int shift_b = 0;
		int shift_a = 0;

		if (probe_r & 0xff) { shift_r = 0; shift_g = 8; shift_b = 16; }
		else { shift_r = 24; shift_g = 16; shift_b = 8; }

		shift_a = (probe_a & 0xff) ? 0 : 24;

		cr = (int)((color >> shift_r) & 0xff);
		cg = (int)((color >> shift_g) & 0xff);
		cb = (int)((color >> shift_b) & 0xff);
		ca = (int)((color >> shift_a) & 0xff);
	}

	vr = ((float)cr / 255.0f) + (ar * intensity);
	vg = ((float)cg / 255.0f) + (ag * intensity);
	vb = ((float)cb / 255.0f) + (ab * intensity);

	maxc = 1.0f;

	if (vr > maxc) { maxc = vr; }
	if (vg > maxc) { maxc = vg; }
	if (vb > maxc) { maxc = vb; }

	maxc = 255.0f / maxc;

	return D_RGBA((int)(vr * maxc), (int)(vg * maxc), (int)(vb * maxc), ca);
}

//
// R_DynLightDebugCounts
//
// Reports what the light budget is actually doing this frame, so a popping
// light can be traced to the cause instead of guessed at: if the counts move
// at the moment it vanishes it is the budget, if they do not it is one of the
// visibility tests.
//

void R_DynLightDebugCounts(int* total, int* scenery, int* dropped,
	int* nearestfire) {
	int     i;
	float   nearest = -1.0f;

	*total = numdynlights;
	*scenery = 0;
	*dropped = dl_dropped;

	for (i = 0; i < numdynlights; i++) {
		int t = dynlights[i].type;

		if (t >= dl_torchyellow) {
			(*scenery)++;
		}

		if (t >= dl_fire && t <= dl_firered) {
			if (nearest < 0.0f || dynlights[i].dist < nearest) {
				nearest = dynlights[i].dist;
			}
		}
	}

	*nearestfire = (nearest < 0.0f) ? -1 : (int)nearest;
}

//
// R_DynLightsRegisterCvars
//

void R_DynLightsRegisterCvars(void) {
	CON_CvarRegister(&r_dynlights);
	CON_CvarRegister(&r_dynlightintensity);
	CON_CvarRegister(&r_dynlightsprites);
	CON_CvarRegister(&r_dynlightmuzzle);
	CON_CvarRegister(&r_dynlightquality);
	CON_CvarRegister(&r_dynlightprops);
	CON_CvarRegister(&r_dynlightdebug);
	CON_CvarRegister(&r_dynlightshadows);
	CON_CvarRegister(&r_dynlightweapon);
}
