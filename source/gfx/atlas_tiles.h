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

	// v1.8.3 Phase 3's five, claiming slots 12..16 in the order the art was painted in.
	//
	// The ART landed first, in its own commit: tools/make_atlas.py's TILES list already
	// paints these five slots and source/world/atlas_uv_shader_test.c already pins their
	// fingerprints, while nothing named them. That is the safe order for the two halves —
	// an atlas slot no tex byte addresses is simply never sampled, whereas a block whose
	// tile is still an UNPAINTED slot draws the magenta missing-texture marker on a real
	// block face. These names are the second half; the order below is the same order
	// tools/make_atlas.py drew them in, which is what makes each name land on its own art.
	TILE_SNOW,       // 12
	TILE_ICE,        // 13
	TILE_CACTUS,     // 14
	TILE_DEAD_BUSH,  // 15 — BLOCK_SHAPE_CROSS
	TILE_FERN,       // 16 — BLOCK_SHAPE_CROSS

	// v1.8.8's per-biome flora and timber, slots 17..30, in the order
	// tools/make_atlas.py's TILES list paints them. Append-only as always.
	//
	// The woods are here as separate TILES rather than as a tint of oak because the
	// difference between them is DRAWING and not colour: birch's bark carries horizontal
	// lenticel dashes and spruce's carries vertical scaly plates with fissures, and no
	// per-vertex multiply of tile 6 can add a mark that is not in tile 6. The same is true
	// of the planks (birch is a pale near-white board, spruce a dark red-brown one — a
	// multiply can only darken, so oak cannot become birch at all) and of the two leaf
	// canopies (birch is a broadleaf clump, spruce a needled speckle carved on the
	// diagonal). Biome COLOUR is a separate concern and a separate agent's; these are
	// separate MATERIALS.
	TILE_BIRCH_LOG_SIDE,  // 17
	TILE_BIRCH_LOG_TOP,   // 18
	TILE_BIRCH_PLANKS,    // 19
	TILE_BIRCH_LEAVES,    // 20
	TILE_SPRUCE_LOG_SIDE, // 21
	TILE_SPRUCE_LOG_TOP,  // 22
	TILE_SPRUCE_PLANKS,   // 23
	TILE_SPRUCE_LEAVES,   // 24
	TILE_TALL_GRASS_TOP,  // 25 — BLOCK_SHAPE_CROSS, the upper half of the two-block clump
	TILE_POPPY,           // 26 — BLOCK_SHAPE_CROSS
	TILE_DAISY,           // 27 — BLOCK_SHAPE_CROSS
	TILE_BLUEBELL,        // 28 — BLOCK_SHAPE_CROSS
	TILE_ORCHID,          // 29 — BLOCK_SHAPE_CROSS
	TILE_APPLE,           // 30

	// v1.8.10 "Light", slot 31 — the first light source. docs/plan-1.8.10-light.md §2.3.
	TILE_TORCH,           // 31 — BLOCK_SHAPE_CROSS

	// v1.8.12 "Ores", slots 32..37. Same order as tools/make_atlas.py's TILES list and as
	// world/block.h's BTEX_* mirror; world/block_tiles_check.c fails the build if the two
	// enums disagree.
	TILE_COAL_ORE,        // 32
	TILE_IRON_ORE,        // 33
	TILE_GOLD_ORE,        // 34
	TILE_REDSTONE_ORE,    // 35
	TILE_LAPIS_ORE,       // 36
	TILE_DIAMOND_ORE,     // 37

	// How many slots the list above NAMES. Not the sheet's capacity — that is
	// ATLAS_TILE_COUNT (64) in world/atlas_uv.h, and these thirty-eight are 0..37 of it.
	// (This sentence said "thirty-two are 0..31" until 2026-09-03; it had been stale since
	// v1.8.10 added the torch and was a version behind again by v1.8.12. The paragraph below
	// carries the same figure and stayed correct, so the two disagreed in the same file —
	// which is exactly how a reader ends up trusting the wrong one.)
	//
	// It exists to be counted against, not to be used as a bound: world/block_tiles_check.c
	// asserts that the number of BTEX/TILE assert lines equals this number, which is what
	// turns "somebody forgot to add the assert" from silence into a build error. Same shape
	// as BLOCK_FACES in world/block.h, which terminates the face enum for the same reason.
	TILE_USED_COUNT,

	// Thirty-eight of ATLAS_TILE_COUNT (64) addressable slots used, 0..37 — twelve until
	// v1.8.3 Phase 3 claimed 12..16 for snow, ice, cactus, dead bush and fern, seventeen
	// until v1.8.8 claimed 17..30 for the birch and spruce materials, the tall grass top,
	// the four flowers and the apple, thirty-one until v1.8.10 claimed 31 for the torch,
	// and thirty-two until v1.8.12 claimed 32..37 for the six ores.
	//
	// It was twelve of FIFTEEN until v1.8.2's task 13b. The old ceiling was not the sheet
	// height: MeshVertex.v held an atlas pixel row, so a uint8_t capped the sheet at 256 px
	// = 16 slots and the top slot's top edge (v = 256) did not fit, leaving 15. Task 13b
	// changed that byte's UNITS to a slot-edge index, moved the TILE_PX factor into the
	// shaders' uvScale.y, and the sheet grew to 1024 px — the PICA200's maximum texture
	// dimension — for 64 slots, all addressable. The vertex is still 8 bytes. See the long
	// note in world/atlas_uv.h.
	//
	// The 32 spares (32..62, and the reserved 63) are NOT blank. Since v1.6.0 F7
	// tools/make_atlas.py paints every slot this list does not name with the magenta/black
	// missing-texture marker, and ATLAS_TILE_MISSING (slot 63, world/atlas_uv.h) is reserved
	// as one permanently — it is where atlasRect() clamps an out-of-range tile id. Before
	// that, tex 10..14 drew the sheet's near-black background fill and an out-of-range tex
	// drew grass, so a server shipping a wrong tex byte looked like a rendering bug here.
	// Appending a real tile to the list above simply overwrites the next spare's marker; 63 is
	// the one that cannot be taken, and tools/make_atlas.py refuses rather than letting it be.
};
