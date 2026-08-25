// The block texture atlas.
//
// One 16x1024 RGBA5551 strip: sixty-four 16x16 tile slots stacked vertically with
// no padding between them, and since v1.8.2's task 13b every one of them is
// addressable. It was 16x256 with sixteen slots, fifteen addressable, from v1.6.0
// to v1.8.1. 1024 px is the PICA200's maximum texture dimension, so 64 is a
// permanent ceiling rather than the current size. It is one tile wide so that
// GPU_REPEAT in U has a period of exactly one tile, which is what lets greedy
// meshing merge co-planar faces into a single quad and still have the tile repeat
// across it. world/atlas_uv.h holds the full reasoning and the arithmetic.
//
// It was a 128x128 sheet of 20x20 cells (16px of art inside 2px of edge-extended
// padding) up to v1.5.1, 64x64 before TILE_PLANKS needed a tenth cell, and
// 256x256 before step 9.3. Step 9.3a moved the format from RGBA8888 to
// RGBA5551: the atlas's only non-opaque tile (leaves) has alpha that is 0 or
// 255 and nothing between, which RGBA5551's 1-bit alpha stores losslessly —
// see tools/make_atlas.py for the measurement.
//
// Every tile is drawn by tools/make_atlas.py — original art for this project, not
// borrowed from anything. Re-run that script after editing it, then rebuild.
#pragma once

#include <3ds.h>
#include <citro3d.h>   // C3D_Tex, for atlasTexture() below

// Sheet geometry, the AtlasRect type and the tile-to-UV arithmetic live in
// world/atlas_uv.h, which is <3ds.h>-free so the host-testable mesher shares exactly
// this code rather than a copy of it.
#include "world/atlas_uv.h"

// Tile slots. The list itself lives in gfx/atlas_tiles.h, which has no 3DS dependency, so
// world/block_tiles_check.c can compile the TILE_* names on the HOST and check world/block.h's
// BTEX_* mirror of them on every tools/run_host_tests.sh run. It was in this file until
// v1.8.3, and that put the names behind the <3ds.h> above: a BTEX/TILE mismatch then compiled
// silently on the host, the whole 22-binary host suite stayed green, and only a devkitPro
// console build could catch it. Measured before the split — TILE_TALL_GRASS given the value 12
// while BTEX_TALL_GRASS stayed 11: "SUITE_EXIT=0", zero FAIL lines in the entire run.
//
// Appending a tile has two steps, and gfx/atlas_tiles.h states both. The second one cannot be
// forgotten silently any more; see the assert-count note in world/block_tiles_check.c.
#include "gfx/atlas_tiles.h"

// Uploads the atlas to the GPU and binds it to texture unit 0. Returns false if
// the embedded t3x could not be parsed.
bool atlasInit(void);
void atlasExit(void);

// Re-binds the atlas to texture unit 0. The binding is global GPU state, so any other pass
// that binds a texture takes it away — step 8.3's sprite batch binds the font sheet, and
// before this existed that left the world sampling glyphs from the next frame onward. The
// symptom was not a crash or a warning: the world drew its full triangle count into a screen
// that looked empty, because most of a font sheet is transparent.
void atlasBind(void);

// The atlas texture, to hand to spriteTexture() — the same contract gfx/font.h states for
// fontTexture(), and for the same reason: the sprite batch tracks texture changes itself so
// it can flush exactly once per switch, and calling atlasBind() from inside a batch would
// change the bound texture behind the batch's back, leaving it certain it had not switched.
// Step 8.2's inventory draws block icons out of this sheet, which is what made a read-only
// accessor necessary alongside the bind.
C3D_Tex* atlasTexture(void);

AtlasRect atlasTile(int tile);
