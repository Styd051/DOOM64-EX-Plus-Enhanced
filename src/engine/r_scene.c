// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
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
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"
#include "gl_main.h"
#include "gl_texture.h"
#include "r_lights.h"
#include "r_sky.h"
#include "r_drawlist.h"
#include "r_main.h"
#include "r_things.h"
#include "i_swap.h"
#include "i_system.h"
#include "dgl.h"
#include "con_cvar.h"
#include "m_fixed.h"
#include "r_dynlights.h"

CVAR_EXTERNAL(r_texturecombiner);
CVAR_EXTERNAL(i_interpolateframes);
CVAR_EXTERNAL(r_fog);
CVAR_EXTERNAL(r_rendersprites);
CVAR_EXTERNAL(st_flashoverlay);
CVAR_EXTERNAL(r_dynlightsprites);

int game_world_shader_scope = 0;

extern void I_SectorCombiner_SetFog(int en, float r, float g, float b, float fac);
extern void I_SectorCombiner_SetFogParams(int mode, float start, float end, float density);

//
// ProcessWalls
//

static boolean ProcessWalls(vtxlist_t* vl, int* drawcount) {
	seg_t* seg = (seg_t*)vl->data;
	sector_t* sec = seg->frontsector;

	bspColor[LIGHT_FLOOR] = R_GetSectorLight(0xff, sec->colors[LIGHT_FLOOR]);
	bspColor[LIGHT_CEILING] = R_GetSectorLight(0xff, sec->colors[LIGHT_CEILING]);
	bspColor[LIGHT_THING] = R_GetSectorLight(0xff, sec->colors[LIGHT_THING]);
	bspColor[LIGHT_UPRWALL] = R_GetSectorLight(0xff, sec->colors[LIGHT_UPRWALL]);
	bspColor[LIGHT_LWRWALL] = R_GetSectorLight(0xff, sec->colors[LIGHT_LWRWALL]);

	if (!vl->callback(seg, &drawVertex[*drawcount])) {
		return false;
	}

	//
	// styd: dynamic projectile lighting.
	// When a light reaches this wall, split the quad into a grid first so
	// the falloff can actually be seen across it, then light the pieces.
	// Otherwise emit the plain quad and light its four corners.
	//
	{
		dlmask_t dlmask = R_DynLightMaskForWall(&drawVertex[*drawcount], sec);

		// Narrow by radius before anything expensive: a sight ray costs a
		// blockmap walk, so it must only ever be paid for a light that could
		// actually light this surface.
		dlmask = R_DynLightsSurfaceMask(&drawVertex[*drawcount], 4, dlmask);

		if (dlmask) {
			// centre of the quad: one sight ray decides the whole surface
			const vtx_t* q = &drawVertex[*drawcount];

			dlmask = R_DynLightMaskLineOfSight(dlmask,
				(q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f,
				(q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f,
				(q[0].z + q[1].z + q[2].z + q[3].z) * 0.25f);
		}

		if (dlmask) {
			vtx_t   srcv[4];
			int     added;

			dmemcpy(srcv, &drawVertex[*drawcount], sizeof(vtx_t) * 4);

			added = R_SubdivideWall(srcv, *drawcount, dlmask);

			if (added > 0) {
				R_ApplyDynLights(&drawVertex[*drawcount], added, dlmask);
				*drawcount += added;
				return true;
			}

			R_ApplyDynLights(&drawVertex[*drawcount], 4, dlmask);
		}
	}

	dglTriangle(*drawcount + 0, *drawcount + 1, *drawcount + 2);
	dglTriangle(*drawcount + 3, *drawcount + 2, *drawcount + 1);

	*drawcount += 4;

	return true;
}

//
// ProcessFlats
//

static boolean ProcessFlats(vtxlist_t* vl, int* drawcount) {
	int j;
	fixed_t tx;
	fixed_t ty;
	leaf_t* leaf;
	subsector_t* ss;
	sector_t* sector;
	int count;

	ss = (subsector_t*)vl->data;
	leaf = &leafs[ss->leaf];
	sector = ss->sector;
	count = *drawcount;

	//
	// styd: the fan used to be emitted here, before the vertices existed.
	// It is now emitted at the bottom of the function, because whether we
	// keep the plain fan or replace it with a subdivided mesh depends on
	// the vertex positions we are about to compute.
	//

	// need to keep texture coords small to avoid
	// floor 'wobble' due to rounding errors on some cards
	// make relative to first vertex, not (0,0)
	// which is arbitary anyway

	tx = (leaf->vertex->x >> 6) & ~(FRACUNIT - 1);
	ty = (leaf->vertex->y >> 6) & ~(FRACUNIT - 1);

	for (j = 0; j < ss->numleafs; j++) {
		int idx;
		vtx_t* v = &drawVertex[count];

		if (vl->flags & DLF_CEILING) {
			leaf = &leafs[(ss->leaf + (ss->numleafs - 1)) - j];
		}
		else {
			leaf = &leafs[ss->leaf + j];
		}

		v->x = F2D3D(leaf->vertex->x);
		v->y = F2D3D(leaf->vertex->y);

		if (vl->flags & DLF_CEILING) {
			if (i_interpolateframes.value) {
				v->z = F2D3D(sector->frame_z2[1]);
			}
			else {
				v->z = F2D3D(sector->ceilingheight);
			}
		}
		else {
			if (i_interpolateframes.value) {
				v->z = F2D3D(sector->frame_z1[1]);
			}
			else {
				v->z = F2D3D(sector->floorheight);
			}
		}

		v->tu = F2D3D((leaf->vertex->x >> 6) - tx);
		v->tv = -F2D3D((leaf->vertex->y >> 6) - ty);

		// set the mapping offsets for scrolling floors/ceilings
		if ((!(vl->flags & DLF_CEILING) && sector->flags & MS_SCROLLFLOOR) ||
			(vl->flags & DLF_CEILING && sector->flags & MS_SCROLLCEILING)) {
			v->tu += F2D3D(sector->xoffset >> 6);
			v->tv += F2D3D(sector->yoffset >> 6);
		}

		v->a = 0xff;

		if (vl->flags & DLF_CEILING) {
			idx = sector->colors[LIGHT_CEILING];
		}
		else {
			idx = sector->colors[LIGHT_FLOOR];
		}

		R_LightToVertex(v, idx, 1);

		//
		// water layer 1
		//
		if (vl->flags & DLF_WATER1) {
			v->tv -= F2D3D(scrollfrac >> 6);
			v->a = 0xA0;
		}

		//
		// water layer 2
		//
		if (vl->flags & DLF_WATER2) {
			v->tu += F2D3D(scrollfrac >> 6);
		}

		count++;
	}

	//
	// styd: dynamic projectile lighting.
	// Doom 64 floors are huge and have very few vertices, so a light either
	// misses every corner or floods the whole plane. Subdivide the fan when
	// a light can reach it.
	//
	if (ss->numleafs <= DL_MAXPOLYVERTS) {
		dlmask_t dlmask = R_DynLightMaskForFlat(&drawVertex[*drawcount],
			(boolean)((vl->flags & DLF_CEILING) != 0), sector);

		dlmask = R_DynLightsSurfaceMask(&drawVertex[*drawcount],
			ss->numleafs, dlmask);

		if (dlmask) {
			// centroid of the leaf fan
			const vtx_t* lv = &drawVertex[*drawcount];
			float   cx = 0.0f;
			float   cy = 0.0f;
			float   inv = 1.0f / (float)ss->numleafs;
			int     k;

			for (k = 0; k < ss->numleafs; k++) {
				cx += lv[k].x;
				cy += lv[k].y;
			}

			dlmask = R_DynLightMaskLineOfSight(dlmask,
				cx * inv, cy * inv, lv[0].z);
		}

		if (dlmask) {
			vtx_t   srcv[DL_MAXPOLYVERTS];
			int     added;

			dmemcpy(srcv, &drawVertex[*drawcount], sizeof(vtx_t) * ss->numleafs);

			added = R_SubdivideFlat(srcv, ss->numleafs, *drawcount, dlmask);

			if (added > 0) {
				R_ApplyDynLights(&drawVertex[*drawcount], added, dlmask);
				*drawcount += added;
				return true;
			}

			R_ApplyDynLights(&drawVertex[*drawcount], ss->numleafs, dlmask);
		}
	}

	// plain fan
	for (j = 0; j < ss->numleafs - 2; j++) {
		dglTriangle(*drawcount, *drawcount + 1 + j, *drawcount + 2 + j);
	}

	*drawcount = count;

	return true;
}

//
// ProcessSprites
//

static boolean ProcessSprites(vtxlist_t* vl, int* drawcount) {
	visspritelist_t* vis;
	mobj_t* mobj;

	vis = (visspritelist_t*)vl->data;
	mobj = vis->spr;

	if (!mobj) {
		return false;
	}

	if (!vl->callback(vis, &drawVertex[*drawcount])) {
		return false;
	}

	// styd: dynamic projectile lighting.
	// full-bright things already draw at maximum brightness (this includes
	// the projectiles that emit the light in the first place), and nightmare
	// things use their own tint from the Nightmare Color option, so both are
	// left alone here.
	if (r_dynlightsprites.value &&
		!(mobj->flags & (MF_NIGHTMARE | MF_RENDERLASER)) &&
		!(mobj->frame & FF_FULLBRIGHT)) {
		dlmask_t dlmask = R_DynLightMaskForThing(mobj->subsector->sector);

		dlmask = R_DynLightsSurfaceMask(&drawVertex[*drawcount], 4, dlmask);

		if (dlmask) {
			dlmask = R_DynLightMaskLineOfSight(dlmask,
				F2D3D(mobj->x), F2D3D(mobj->y),
				F2D3D(mobj->z + (mobj->height >> 1)));

			R_ApplyDynLights(&drawVertex[*drawcount], 4, dlmask);
		}
	}

	GL_SetState(GLSTATE_CULL, !(mobj->flags & MF_RENDERLASER));

	dglTriangle(*drawcount + 0, *drawcount + 1, *drawcount + 2);
	dglTriangle(*drawcount + 3, *drawcount + 2, *drawcount + 1);

	*drawcount += 4;

	return true;
}

//
// SetupFog
//
static void SetupFog(void) {
	if (r_fillmode.value <= 0) {
		I_SectorCombiner_SetFog(0, 0, 0, 0, 0);
		I_SectorCombiner_SetFogParams(0, 0, 0, 0);
		dglDisable(GL_FOG);
		return;
	}

	boolean has_fog = r_fog.value || (sky && sky->fogcolor != 0);

	if (!has_fog) {
		dglDisable(GL_FOG);
		I_SectorCombiner_SetFog(0, 0, 0, 0, 0);
		I_SectorCombiner_SetFogParams(0, 0, 0, 0);
		return;
	}

	rfloat color[4] = { 0,0,0,1 };
	rcolor fogcolor = 0;
	int fognear = sky ? sky->fognear : 985;
	int fogfactor = 1000 - fognear;

	if (fogfactor <= 0) fogfactor = 1;

	dglEnable(GL_FOG);

	if (sky) {
		fogcolor = sky->fogcolor;
	}

	color[0] = ((fogcolor & 0xFF)) / 255.0f;
	color[1] = ((fogcolor >> 8) & 0xFF) / 255.0f; 
	color[2] = ((fogcolor >> 16) & 0xFF) / 255.0f;
	color[3] = 1.0f;

	float position = ((float)fogfactor) / 1000.0f;
	if (position <= 0.0f) position = 0.00001f;

	float start = 5.0f / position;
	float end = 30.0f / position;

	dglFogi(GL_FOG_MODE, GL_LINEAR);
	dglFogf(GL_FOG_START, start);
	dglFogf(GL_FOG_END, end);
	I_SectorCombiner_SetFogParams(1, start, end, 0.0f);

	dglFogfv(GL_FOG_COLOR, color);
	I_SectorCombiner_SetFog(1, color[0], color[1], color[2], (float)fogfactor / 1000.0f);
}

//
// R_SetViewMatrix
//

void R_SetViewMatrix(void) {
	dglMatrixMode(GL_PROJECTION);
	dglLoadIdentity();
	dglViewFrustum(video_width, video_height, r_fov.value, 0.1f);
	dglMatrixMode(GL_MODELVIEW);
	dglLoadIdentity();
	dglRotatef(-TRUEANGLES(viewpitch), 1.0f, 0.0f, 0.0f);
	dglRotatef(-TRUEANGLES(viewangle) + 90.0f, 0.0f, 0.0f, 1.0f);
	dglTranslatef(-fviewx, -fviewy, -fviewz);
}

//
// R_RenderWorld
//

void R_RenderWorld(void) {
	game_world_shader_scope = 1;
	I_ShaderBind();
	SetupFog();

	if (sky && (sky->flags & SKF_VOID)) {
		byte* vb = (byte*)&sky->skycolor[2];
		dglClearColor(vb[0] / 255.0f, vb[1] / 255.0f, vb[2] / 255.0f, 1.0f);
		dglClear(GL_COLOR_BUFFER_BIT);
	}

	dglEnable(GL_DEPTH_TEST);
	DL_BeginDrawList(r_fillmode.value >= 1, r_texturecombiner.value >= 1);

	// setup texture environment for effects
	if (r_texturecombiner.value) {
		if (!nolights) {
			GL_UpdateEnvTexture(WHITE);
			GL_SetTextureUnit(1, true);
			dglTexCombModulate(GL_PREVIOUS, GL_PRIMARY_COLOR);
		}
		if (st_flashoverlay.value <= 0) {
			GL_SetTextureUnit(2, true);
			dglTexCombColor(GL_PREVIOUS, flashcolor, GL_ADD);
		}
		dglTexCombReplaceAlpha(GL_TEXTURE0_ARB);
		GL_SetTextureUnit(0, true);
	}

	dglEnable(GL_ALPHA_TEST);
	dglAlphaFunc(GL_GREATER, 0.5f);
	GL_SetState(GLSTATE_BLEND, 0);
	DL_ProcessDrawList(DLT_FLAT, ProcessFlats);
	DL_ProcessDrawList(DLT_WALL, ProcessWalls);

	dglDisable(GL_ALPHA_TEST);
	dglDepthMask(GL_FALSE);
	GL_SetState(GLSTATE_BLEND, 1);
	dglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	DL_ProcessDrawList(DLT_FLAT, ProcessFlats);
	DL_ProcessDrawList(DLT_WALL, ProcessWalls);

	if (r_rendersprites.value) {
		R_SetupSprites();
		dglBlendFunc(GL_SRC_ALPHA, GL_ONE);
		GL_SetState(GLSTATE_BLEND, 1);
		DL_ProcessDrawList(DLT_SPRITE, ProcessSprites);
	}

	// Restore states
	dglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_SetState(GLSTATE_BLEND, 0);
	dglEnable(GL_ALPHA_TEST);
	dglDepthMask(GL_TRUE);
	dglDisable(GL_FOG);
	dglDisable(GL_DEPTH_TEST);
	GL_SetOrthoScale(1.0f);
	GL_SetDefaultCombiner();
	I_ShaderUnBind();
}
