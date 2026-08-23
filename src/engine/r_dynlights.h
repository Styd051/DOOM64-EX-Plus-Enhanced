// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// 2026 Styd051
//
// Dynamic projectile lighting
// Ported from the Doom 64 Dreamcast port by jnmartin84
// (original concept: R_AddProjectileLight / r_phase1.c, r_lights.c)
//
// The Dreamcast version identifies light emitters by sprite lump number,
// which is specific to its N64-derived data layout. This version keys on
// mobj type instead, so the Enhanced engine's extra monsters and
// projectiles are handled naturally.
//
//-----------------------------------------------------------------------------

#ifndef _R_DYNLIGHTS_H_
#define _R_DYNLIGHTS_H_

#include "doomtype.h"
#include "gl_main.h"
#include "m_fixed.h"
#include "t_bsp.h"

//
// Maximum number of simultaneous point lights per frame.
//
// The Dreamcast port uses 16. 32 turned out to be too few once scenery was
// added: a map with a dozen flames saturates a class and starts evicting
// lights the player is looking straight at. The occlusion masks carry one bit
// per light, so the ceiling is the width of the mask type - hence 64 bits.
//
#define MAX_DYNLIGHTS   64

typedef unsigned long long dlmask_t;

typedef struct {
	float   x;
	float   y;
	float   z;
	float   r;          // 0.0 - 1.0
	float   g;
	float   b;
	float   radius;     // world units
	float   rcpradius;  // 1.0 / radius, precomputed
	float   dist;       // distance to viewer, used when evicting
	sector_t* sector;   // sector the light sits in, start of the flood fill
	int     type;       // dynlight_type_t, used for the per-type cap
} dynlight_t;

extern dynlight_t   dynlights[MAX_DYNLIGHTS];
extern int          numdynlights;

// largest polygon we are willing to subdivide (subsector leaf count)
#define DL_MAXPOLYVERTS 64

// called once per frame, from R_RenderPlayerView
void R_CollectDynLights(void);

//
// Occlusion.
//
// The lights are plain points with no visibility information, so on their
// own they shine straight through geometry. These build a bitmask of the
// lights allowed to touch a given surface; every light beyond the mask is
// ignored. One bit per light, hence the 32 light ceiling.
//
// Walls: a light must sit on the same side of the wall plane as the camera.
// Flats: a light must be above a floor, or below a ceiling.
//
#define DL_MASK_ALL 0xffffffffffffffffull

dlmask_t R_DynLightMaskForWall(const vtx_t* v, sector_t* sec);
dlmask_t R_DynLightMaskForFlat(const vtx_t* v, boolean ceiling, sector_t* sec);

// things have no plane to test against, so only the sector reach applies
dlmask_t R_DynLightMaskForThing(sector_t* sec);

// narrows a mask to the lights that actually have line of sight to a point
dlmask_t R_DynLightMaskLineOfSight(dlmask_t mask, float x, float y, float z);

// applies the accumulated lights to a batch of world-space vertices
void R_ApplyDynLights(vtx_t* v, int count, dlmask_t mask);

//
// Tessellation.
//
// Per-vertex lighting only looks right when the polygon is small relative
// to the light. Doom 64 geometry is very coarse - a whole room floor can be
// a single subsector of four vertices - so a light either misses it
// completely or floods it edge to edge. These helpers split a lit surface
// into smaller pieces first, which is what the Dreamcast port does with its
// precomputed split_verts.
//
// Both return the number of vertices written starting at drawVertex[base],
// with the matching triangles already submitted through dglTriangle, or 0
// if the surface was not subdivided (caller then uses its normal path).
//

// narrows a mask to the lights whose radius reaches the polygon's bounding box
dlmask_t R_DynLightsSurfaceMask(const vtx_t* v, int count, dlmask_t mask);

int R_SubdivideWall(const vtx_t* src, int base, dlmask_t mask);
int R_SubdivideFlat(const vtx_t* src, int count, int base, dlmask_t mask);

// called by the weapon code when a player fires.
// the player is needed so that another player's gun cannot flash ours.
void R_TriggerMuzzleFlash(struct player_s* player, int weapon);

// folds the dynamic lights into the colour of the weapon held on screen
rcolor R_DynLightApplyToWeapon(rcolor color, struct player_s* player);

void R_DynLightDebugCounts(int* total, int* scenery, int* dropped,
	int* nearestfire);

void R_DynLightsRegisterCvars(void);

#endif
