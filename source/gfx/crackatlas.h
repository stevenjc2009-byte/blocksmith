// The block-breaking crack overlay sheet — roadmap task 50 part 3.
//
// One 16x128 RGBA5551 strip: eight 16x16 crack stages stacked vertically with no padding
// between them, stage 0 the faintest and stage 7 nearly shattered. 4,096 bytes of VRAM.
//
// Why it is not part of gfx/atlas.h's sheet. That one is FULL for this purpose:
// world/atlas_uv.h makes it 16 slots of which 15 are addressable (MeshVertex.v is a
// uint8_t, so the top slot's v1 = 256 does not fit) and ATLAS_TILE_MISSING (slot 14) is
// permanently reserved for the missing-texture marker, while gfx/atlas.h's TILE_* enum
// already names twelve painted tiles. Two free slots, eight stages needed. Growing the
// shared sheet does not help either — the 15-slot ceiling comes from the uint8_t, not from
// the sheet height (see the long note in world/atlas_uv.h). A separate 4 KB texture is
// cheaper than changing the locked vertex format, and it leaves the world's slot budget
// exactly as it was.
//
// Every stage is drawn by tools/make_crack_atlas.py — original art for this project. That
// script verifies the sheet it wrote before exiting: binary alpha, every stage a superset
// of the one below it, and the V flip applied. Re-run it after editing, then rebuild —
// and note the Makefile needs `crackatlas.t3x: crackatlas.png` for the rebuild to reach
// the console at all (the same dependency trap atlas.png and font.png each carry a line
// for).
#pragma once

#include <3ds.h>
#include <citro3d.h>   // C3D_Tex, for crackAtlasTexture() below

// Sheet geometry. Duplicated in tools/make_crack_atlas.py (CRACK_W_PX/CRACK_H_PX/
// CRACK_STAGES) and used by scene/crackoverlay.c to build its UVs. Unlike the world
// atlas there is no shader-side copy of these numbers: crackoverlay.c bakes normalised
// float UVs straight into its vertex buffer, so source/shaders/crack.v.pica needs no
// uvScale constant and cannot drift out of step with this header.
#define CRACK_W_PX      16
#define CRACK_H_PX      128
#define CRACK_TILE_PX   16
#define CRACK_STAGES    (CRACK_H_PX / CRACK_TILE_PX)   // 8
#define CRACK_STAGE_MAX (CRACK_STAGES - 1)             // 7 — the highest addressable stage

// Uploads the crack sheet to the GPU. Returns false if the embedded t3x could not be
// parsed, in which case crackAtlasBind() and crackAtlasTexture() must not be used —
// scene/crackoverlay.c degrades to drawing nothing rather than binding a dead texture.
//
// Deliberately does NOT bind on success, unlike atlasInit(). Init runs during startup
// while the world's own pipeline is being set up, and leaving the crack sheet on texture
// unit 0 there would have the first world frame sample cracks instead of blocks.
bool crackAtlasInit(void);
void crackAtlasExit(void);

// Binds the crack sheet to texture unit 0, taking the unit from whatever held it — which
// in practice is the world atlas. That is safe only because scene/chunk_render.c calls
// atlasBind() at the top of every frame's draw (see the comment on atlasBind() in
// gfx/atlas.h, which the sprite batch already made necessary). crackOverlayDraw() is the
// only caller and runs after the world pass, so the next frame re-binds the atlas before
// anything samples it.
void crackAtlasBind(void);

C3D_Tex* crackAtlasTexture(void);
