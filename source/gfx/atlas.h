// The block texture atlas.
//
// One 64x64 RGBA5551 sheet holding 16x16 tiles in 20x20 cells: 16 pixels of art
// with 2 pixels of edge-extended padding on every side. The padding is what lets
// GPU_NEAREST sample tile edges at any distance without pulling in a neighbour,
// and it is why the grid is 3x3 rather than 4x4.
//
// Step 9.3c trimmed the sheet from 256x256 (12x12 grid, 144 cells) down to the
// smallest power-of-two sheet that still holds the 9 tiles actually in use
// (3x3 = 9 cells, zero spare). Step 9.3a moved the format from RGBA8888 to
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

// Tile slots. The order is the contract with tools/make_atlas.py — append only,
// never reorder, or every mesh built before the change points at the wrong art.
enum {
	TILE_GRASS_TOP = 0,
	TILE_GRASS_SIDE,
	TILE_DIRT,
	TILE_STONE,
	TILE_SAND,
	TILE_SENTINEL,   // magenta checker — a bleed alarm, never used on real blocks
	TILE_WOOD_SIDE,  // step 5.3's trees, appended after the sentinel rather than
	TILE_WOOD_TOP,   // beside sand, because inserting would re-texture everything
	TILE_LEAVES,     // after the insertion point
	TILE_PLANKS,     // the first crafted block; the tile that forced the sheet to 128
};

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
