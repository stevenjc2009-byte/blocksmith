// The block texture atlas.
//
// One 256x256 RGBA8 sheet holding 16x16 tiles in 20x20 cells: 16 pixels of art
// with 2 pixels of edge-extended padding on every side. The padding is what lets
// GPU_NEAREST sample tile edges at any distance without pulling in a neighbour,
// and it is why the grid is 12x12 rather than 16x16.
//
// Every tile is drawn by tools/make_atlas.py — original art for this project, not
// borrowed from anything. Re-run that script after editing it, then rebuild.
#pragma once

#include <3ds.h>

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
};

// Uploads the atlas to the GPU and binds it to texture unit 0. Returns false if
// the embedded t3x could not be parsed.
bool atlasInit(void);
void atlasExit(void);

AtlasRect atlasTile(int tile);
