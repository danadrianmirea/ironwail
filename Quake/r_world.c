/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_world.c: world model rendering

#include "quakedef.h"

extern cvar_t gl_fullbrights, gl_overbright, r_oldskyleaf, r_showtris; //johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix

extern gltexture_t *lightmap_texture;

extern GLuint gl_bmodel_vbo;
extern size_t gl_bmodel_vbo_size;
extern GLuint gl_bmodel_ibo;
extern size_t gl_bmodel_ibo_size;
extern GLuint gl_bmodel_indirect_buffer;
extern size_t gl_bmodel_indirect_buffer_size;
extern GLuint gl_bmodel_leaf_buffer;
extern GLuint gl_bmodel_surf_buffer;
extern GLuint gl_bmodel_marksurf_buffer;

static GLuint gl_bmodel_compacted_indirect_buffer;

typedef struct gpumark_frame_s {
	vec4_t		frustum[4];
	GLuint		oldskyleaf;
	float		vieworg[3];
	GLuint		framecount;
	GLuint		padding[3];
} gpumark_frame_t;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

#ifdef USE_SSE2
/*
===============
R_CullBoxSIMD

Performs frustum culling for 8 bounding boxes
===============
*/
int R_CullBoxSIMD (soa_aabb_t box, int activelanes)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		mplane_t *p;
		byte signbits;
		int ofs;

		if (activelanes == 0)
			break;

		p = frustum + i;
		signbits = p->signbits;

		__m128 vplane = _mm_loadu_ps(p->normal);

		ofs = signbits & 1 ? 0 : 8; // x min/max
		__m128 px = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(0, 0, 0, 0));
		__m128 v0 = _mm_mul_ps(_mm_loadu_ps(box + ofs), px);
		__m128 v1 = _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), px);

		ofs = signbits & 2 ? 16 : 24; // y min/max
		__m128 py = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(1, 1, 1, 1));
		v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(box + ofs), py));
		v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), py));

		ofs = signbits & 4 ? 32 : 40; // z min/max
		__m128 pz = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(2, 2, 2, 2));
		v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(box + ofs), pz));
		v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), pz));

		__m128 pd = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(3, 3, 3, 3));
		activelanes &= _mm_movemask_ps(_mm_cmplt_ps(pd, v0)) | (_mm_movemask_ps(_mm_cmplt_ps(pd, v1)) << 4);
	}

	return activelanes;
}
#endif // defined(USE_SSE2)

/*
===============
R_MarkVisSurfaces
===============
*/
static void R_MarkVisSurfaces (byte* vis)
{
	int			i;
	GLuint		buf;
	GLbyte*		ofs;
	size_t		vissize = (cl.worldmodel->numleafs + 7) >> 3;
	gpumark_frame_t frame;

	GL_BeginGroup ("Mark surfaces");

	for (i = 0; i < 4; i++)
	{
		frame.frustum[i][0] = frustum[i].normal[0];
		frame.frustum[i][1] = frustum[i].normal[1];
		frame.frustum[i][2] = frustum[i].normal[2];
		frame.frustum[i][3] = frustum[i].dist;
	}
	frame.oldskyleaf = r_oldskyleaf.value != 0.f;
	frame.vieworg[0] = r_refdef.vieworg[0];
	frame.vieworg[1] = r_refdef.vieworg[1];
	frame.vieworg[2] = r_refdef.vieworg[2];
	frame.framecount = r_framecount;

	vissize = (vissize + 3) & ~3; // round up to uint

	GL_UseProgram (glprogs.clear_indirect);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_indirect_buffer, 0, cl.worldmodel->texofs[TEXTYPE_COUNT] * sizeof(bmodel_draw_indirect_t));
	GL_DispatchComputeFunc ((cl.worldmodel->texofs[TEXTYPE_COUNT] + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_UseProgram (glprogs.cull_mark);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_ibo, 0, gl_bmodel_ibo_size);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, vis, vissize, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, buf, (GLintptr)ofs, vissize);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, gl_bmodel_leaf_buffer, 0, cl.worldmodel->numleafs * sizeof(bmodel_gpu_leaf_t));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_marksurf_buffer, 0, cl.worldmodel->nummarksurfaces * sizeof(cl.worldmodel->marksurfaces[0]));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, gl_bmodel_surf_buffer, 0, cl.worldmodel->numsurfaces * sizeof(bmodel_gpu_surf_t));
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &frame, sizeof(frame), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 7, buf, (GLintptr)ofs, sizeof(frame));

	GL_DispatchComputeFunc ((cl.worldmodel->numleafs + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

	GL_EndGroup ();
}

#if defined(USE_SIMD)
/*
===============
R_AddStaticModelsSIMD
===============
*/
void R_AddStaticModelsSIMD (byte *vis)
{
	int			i, j;
	int			numleafs = cl.worldmodel->numleafs;
	soa_aabb_t	*leafbounds = cl.worldmodel->soa_leafbounds;

	for (i = 0; i < numleafs; i += 8)
	{
		mleaf_t *leaf;
		int mask = vis[i>>3];
		if (mask == 0)
			continue;

		mask = R_CullBoxSIMD (leafbounds[i>>3], mask);
		if (mask == 0)
			continue;

		for (j = 0, leaf = &cl.worldmodel->leafs[1 + i]; j < 8 && i + j < numleafs; j++, leaf++)
			if ((mask & (1 << j)) && leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
	}
}
#endif // defined(USE_SIMD)

/*
===============
R_AddStaticModels
===============
*/
void R_AddStaticModels (byte* vis)
{
	int			i;
	mleaf_t		*leaf;

	for (i = 0, leaf = &cl.worldmodel->leafs[1]; i < cl.worldmodel->numleafs; i++, leaf++)
		if (vis[i>>3] & (1<<(i&7)) && !R_CullBox(leaf->minmaxs, leaf->minmaxs + 3) && leaf->efrags)
			R_StoreEfrags (&leaf->efrags);
}

/*
===============
R_MarkSurfaces
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	int			i;
	qboolean	nearwaterportal;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0; i < r_viewleaf->nummarksurfaces; i++)
		if (cl.worldmodel->surfaces[r_viewleaf->firstmarksurface[i]].flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	r_visframecount++;

	R_MarkVisSurfaces (vis);

#if defined(USE_SIMD)
	if (use_simd)
		R_AddStaticModelsSIMD (vis);
	else
#endif
		R_AddStaticModels (vis);
}

/*
================
GL_WaterAlphaForEntityTextureType
 
Returns the water alpha to use for the entity and texture type combination.
================
*/
float GL_WaterAlphaForEntityTextureType (entity_t *ent, textype_t type)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForTextureType(type);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

/*
=============
GLWorld_CreateResources
=============
*/
void GLWorld_CreateResources (void)
{
	GL_GenBuffersFunc (1, &gl_bmodel_compacted_indirect_buffer);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_compacted_indirect_buffer);
	GL_BufferDataFunc (GL_DRAW_INDIRECT_BUFFER, sizeof(bmodel_draw_indirect_t) * MAX_BMODEL_DRAWS, NULL, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, 0);
}

typedef struct bmodel_gpu_instance_s {
	float		world[12];	// world matrix (transposed mat4x3)
	float		alpha;
	float		padding[3];
} bmodel_gpu_instance_t;

typedef struct bmodel_gpu_call_s {
	GLuint64	texture;
	GLuint64	fullbright;
	GLuint		flags;
	GLfloat		alpha;
} bmodel_gpu_call_t;

typedef struct bmodel_gpu_call_remap_s {
	GLuint		src;
	GLuint		inst;
} bmodel_gpu_call_remap_t;

static bmodel_gpu_instance_t	bmodel_instances[MAX_VISEDICTS + 1]; // +1 for worldspawn
static bmodel_gpu_call_t		bmodel_calls[MAX_BMODEL_DRAWS];
static bmodel_gpu_call_remap_t	bmodel_call_remap[MAX_BMODEL_DRAWS];
static int						num_bmodel_calls;
static GLuint					bmodel_batch_program;

/*
=============
R_InitBModelInstance
=============
*/
static void R_InitBModelInstance (bmodel_gpu_instance_t *inst, entity_t *ent)
{
	vec3_t angles;
	float mat[16];

	angles[0] = -ent->angles[0];
	angles[1] =  ent->angles[1];
	angles[2] =  ent->angles[2];
	R_EntityMatrix (mat, ent->origin, angles);

	#define COPY_ROW(row)					\
		inst->world[row*4+0] = mat[row+0],	\
		inst->world[row*4+1] = mat[row+4],	\
		inst->world[row*4+2] = mat[row+8],	\
		inst->world[row*4+3] = mat[row+12]

	COPY_ROW (0);
	COPY_ROW (1);
	COPY_ROW (2);

	#undef COPY_ROW

	inst->alpha = ent->alpha == ENTALPHA_DEFAULT ? -1.f : ENTALPHA_DECODE (ent->alpha);
	memset (&inst->padding, 0, sizeof(inst->padding));
}

/*
=============
R_ResetBModelCalls
=============
*/
static void R_ResetBModelCalls (GLuint program)
{
	bmodel_batch_program = program;
	num_bmodel_calls = 0;
}

/*
=============
R_FlushBModelCalls
=============
*/
static void R_FlushBModelCalls (void)
{
	GLuint	buf;
	GLbyte	*ofs;

	if (!num_bmodel_calls)
		return;

	GL_UseProgram (glprogs.gather_indirect);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_indirect_buffer, 0, gl_bmodel_indirect_buffer_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, gl_bmodel_compacted_indirect_buffer, 0, sizeof(bmodel_draw_indirect_t) * num_bmodel_calls);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_call_remap, sizeof(bmodel_call_remap[0]) * num_bmodel_calls, &buf, &ofs);
	GL_BindBufferRange  (GL_SHADER_STORAGE_BUFFER, 7, buf, (GLintptr)ofs, sizeof(bmodel_call_remap[0]) * num_bmodel_calls);
	GL_DispatchComputeFunc ((num_bmodel_calls + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT);

	GL_UseProgram (bmodel_batch_program);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof (float), NULL);
	GL_VertexAttribPointerFunc (1, 4, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof (float), (void *)(3 * sizeof (float)));
	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls, sizeof(bmodel_calls[0]) * num_bmodel_calls, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(bmodel_calls[0]) * num_bmodel_calls);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_compacted_indirect_buffer);
	GL_MultiDrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, 0, num_bmodel_calls, sizeof (bmodel_draw_indirect_t));

	num_bmodel_calls = 0;
}

/*
=============
R_AddBModelCall
=============
*/
static void R_AddBModelCall (int index, int first_instance, int num_instances, texture_t *t, qboolean zfix)
{
	bmodel_gpu_call_t *call;
	bmodel_gpu_call_remap_t *remap;
	gltexture_t *tx, *fb;

	if (num_bmodel_calls == MAX_BMODEL_DRAWS)
		R_FlushBModelCalls ();

	call = num_bmodel_calls + bmodel_calls;
	remap = num_bmodel_calls + bmodel_call_remap;
	num_bmodel_calls++;

	if (t)
	{
		tx = t->gltexture;
		fb = t->fullbright;
		if (r_lightmap_cheatsafe)
			tx = fb = NULL;
		if (!gl_fullbrights.value)
			fb = NULL;
	}
	else
	{
		tx = fb = whitetexture;
	}

	if (!gl_zfix.value)
		zfix = 0;

	call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
	call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
	call->flags =
		(t != NULL && t->type == TEXTYPE_CUTOUT)
		| (zfix << 1)
		| ((fb != NULL) << 2)
		| (((GLuint) gl_clipcontrol_able) << 31)
	;
	call->alpha = t ? GL_WaterAlphaForTextureType (t->type) : 1.f;

	SDL_assert (num_instances > 0);
	SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
	remap->src = index;
	remap->inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);
}

typedef enum {
	BP_SOLID,
	BP_ALPHATEST,
	BP_SKYLAYERS,
	BP_SKYSTENCIL,
	BP_SHOWTRIS,
} brushpass_t;

/*
=============
R_DrawBrushModels_Real
=============
*/
static void R_DrawBrushModels_Real (entity_t **ents, int count, brushpass_t pass)
{
	int i, j;
	int totalinst, baseinst;
	unsigned state;
	GLuint program;
	GLuint buf;
	GLbyte *ofs;
	textype_t texbegin, texend;

	if (!count)
		return;

	if (count > countof(bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, countof(bmodel_instances));
		count = countof(bmodel_instances);
	}

	switch (pass)
	{
	default:
	case BP_SOLID:
		texbegin = 0;
		texend = TEXTYPE_CUTOUT;
		program = glprogs.world[gl_bindless_able][0];
		break;
	case BP_ALPHATEST:
		texbegin = TEXTYPE_CUTOUT;
		texend = TEXTYPE_CUTOUT + 1;
		program = glprogs.world[gl_bindless_able][1];
		break;
	case BP_SKYLAYERS:
		texbegin = TEXTYPE_SKY;
		texend = TEXTYPE_SKY + 1;
		program = glprogs.skylayers[gl_bindless_able];
		break;
	case BP_SKYSTENCIL:
		texbegin = TEXTYPE_SKY;
		texend = TEXTYPE_SKY + 1;
		program = glprogs.skystencil[gl_bindless_able];
		break;
	case BP_SHOWTRIS:
		texbegin = 0;
		texend = TEXTYPE_COUNT;
		program = glprogs.world[gl_bindless_able][0];
		break;
	}

	// fill instance data
	for (i = 0, totalinst = 0; i < count; i++)
		if (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin] > 0)
			R_InitBModelInstance (&bmodel_instances[totalinst++], ents[i]);

	if (!totalinst)
		return;

	// setup state
	state = GLS_CULL_BACK | GLS_ATTRIBS(2);
	if (ents[0] == cl_entities || ENTALPHA_OPAQUE (ents[0]->alpha))
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	
	R_ResetBModelCalls (program);
	GL_SetState (state);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * count, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * count);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;
		int numtex = model->texofs[texend] - model->texofs[texbegin];

		if (!numtex)
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;

		for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
		{
			texture_t *t = model->textures[model->usedtextures[j]];
			R_AddBModelCall (model->firstcmd + j, baseinst, numinst, pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0, !isworld);
		}

		baseinst += numinst;
	}

	R_FlushBModelCalls ();
}

/*
=============
R_EntHasWater
=============
*/
static qboolean R_EntHasWater (entity_t *ent, qboolean translucent)
{
	int i;
	for (i = TEXTYPE_FIRSTLIQUID; i < TEXTYPE_LASTLIQUID+1; i++)
	{
		int numtex = ent->model->texofs[i+1] - ent->model->texofs[i];
		if (numtex && (GL_WaterAlphaForEntityTextureType (ent, (textype_t)i) < 1.f) == translucent)
			return true;
	}
	return false;
}

/*
=============
R_DrawBrushModels_Water
=============
*/
void R_DrawBrushModels_Water (entity_t **ents, int count, qboolean translucent)
{
	int i, j;
	int totalinst, baseinst;
	unsigned state;
	GLuint buf;
	GLbyte *ofs;

	if (count > countof(bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, countof(bmodel_instances));
		count = countof(bmodel_instances);
	}

	// fill instance data
	for (i = 0, totalinst = 0; i < count; i++)
		if (R_EntHasWater (ents[i], translucent))
			R_InitBModelInstance (&bmodel_instances[totalinst++], ents[i]);

	if (!totalinst)
		return;

	GL_BeginGroup (translucent ? "Water (translucent)" : "Water (opaque)");

	// setup state
	state = GLS_CULL_BACK | GLS_ATTRIBS(2);
	if (translucent)
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;

	R_ResetBModelCalls (glprogs.water[gl_bindless_able]);
	GL_SetState (state);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * count);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;

		if (!R_EntHasWater (e, translucent))
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += R_EntHasWater (ents[i], translucent);

		for (j = model->texofs[TEXTYPE_FIRSTLIQUID]; j < model->texofs[TEXTYPE_LASTLIQUID+1]; j++)
		{
			texture_t *t = model->textures[model->usedtextures[j]];
			R_AddBModelCall (model->firstcmd + j, baseinst, numinst, R_TextureAnimation (t, frame), !isworld);
		}

		baseinst += numinst;
	}

	R_FlushBModelCalls ();

	GL_EndGroup ();
}

/*
=============
R_DrawBrushModels
=============
*/
void R_DrawBrushModels (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SOLID);
	R_DrawBrushModels_Real (ents, count, BP_ALPHATEST);
}

/*
=============
R_DrawBrushModels_SkyLayers
=============
*/
void R_DrawBrushModels_SkyLayers (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKYLAYERS);
}

/*
=============
R_DrawBrushModels_SkyStencil
=============
*/
void R_DrawBrushModels_SkyStencil (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKYSTENCIL);
}

/*
=============
R_DrawBrushModels_ShowTris
=============
*/
void R_DrawBrushModels_ShowTris (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SHOWTRIS);
}
