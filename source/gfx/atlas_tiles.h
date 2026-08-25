// The atlas tile slots, and nothing else.
//
// Split out of gfx/atlas.h in v1.8.3 so that this list can be compiled on the host. That
// header includes <3ds.h> and <citro3d.h> for atlasInit/atlasBind/atlasTexture, so any file
// naming TILE_* was a devkitARM-only file — which is what left world/block.h's BTEX_* mirror
// of this enum guarded by nothing but a console build. world/block_tiles_check.c is the file
// that checks the mirror; it now includes THIS header instead of gfx/atlas.h, has no 3DS
// dependency, and is compiled by tools/run_host_tests.sh on every host run.
//
// gfx/atlas.h includes this file, so the console build sees exactly the enum it always saw
// and every existing `#include "gfx/atlas.h"` is unaffected.
//
// The order is the contract with tools/make_atlas.py — append only, never reorder, or every
// mesh built before the change points at the wrong art. Two rules when appending:
//
//   * the new name goes ABOVE TILE_USED_COUNT, and
//   * a matching line goes into world/block_tiles_check.c's BS_BTEX_TILE_PAIRS list.
//
// The second is not a convention to remember: that list's length is _Static_asserted against
// TILE_USED_COUNT, so a tile appended without its assert fails the build rather than sitting
// silently unguarded.
#pragma once

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
	TILE_WATER,      // roadmap task 17 — opaque art; tools/make_atlas.py's tile_water says why
	TILE_TALL_GRASS, // roadmap task 19 — the first tile drawn for a BLOCK_SHAPE_CROSS block

	// How many slots the list above NAMES. Not the sheet's capacity — that is
	// ATLAS_TILE_COUNT (64) in world/atlas_uv.h, and these twelve are 0..11 of it.
	//
	// It exists to be counted against, not to be used as a bound: world/block_tiles_check.c
	// asserts that the number of BTEX/TILE assert lines equals this number, which is what
	// turns "somebody forgot to add the assert" from silence into a build error. Same shape
	// as BLOCK_FACES in world/block.h, which terminates the face enum for the same reason.
	TILE_USED_COUNT,

	// Twelve of ATLAS_TILE_COUNT (64) addressable slots used, 0..11.
	//
	// It was twelve of FIFTEEN until v1.8.2's task 13b. The old ceiling was not the sheet
	// height: MeshVertex.v held an atlas pixel row, so a uint8_t capped the sheet at 256 px
	// = 16 slots and the top slot's top edge (v = 256) did not fit, leaving 15. Task 13b
	// changed that byte's UNITS to a slot-edge index, moved the TILE_PX factor into the
	// shaders' uvScale.y, and the sheet grew to 1024 px — the PICA200's maximum texture
	// dimension — for 64 slots, all addressable. The vertex is still 8 bytes. See the long
	// note in world/atlas_uv.h.
	//
	// The 52 spares (12..62, and the reserved 63) are NOT blank. Since v1.6.0 F7
	// tools/make_atlas.py paints every slot this list does not name with the magenta/black
	// missing-texture marker, and ATLAS_TILE_MISSING (slot 63, world/atlas_uv.h) is reserved
	// as one permanently — it is where atlasRect() clamps an out-of-range tile id. Before
	// that, tex 10..14 drew the sheet's near-black background fill and an out-of-range tex
	// drew grass, so a server shipping a wrong tex byte looked like a rendering bug here.
	// Appending a real tile to the list above simply overwrites the next spare's marker; 63 is
	// the one that cannot be taken, and tools/make_atlas.py refuses rather than letting it be.
};
