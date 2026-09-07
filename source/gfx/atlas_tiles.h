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
	// v1.8.14 "Animals": four raw meat icons. Same order as world/block.h's BTEX_* mirror
	// and as tools/make_atlas.py's TILES list; world/block_tiles_check.c fails the build if
	// the two enums disagree. ⚠ These are slots 38..41 for BLOCK ids 34..37 — the id and
	// the slot are four apart, exactly as they are for the six ores directly above.
	TILE_RAW_PORKCHOP,    // 38
	TILE_RAW_BEEF,        // 39
	TILE_RAW_CHICKEN,     // 40
	TILE_RAW_MUTTON,      // 41

	// v1.8.15 "Furnace": four cooked-meat icons and the furnace's two front faces. Same
	// order as world/block.h's BTEX_* mirror and as tools/make_atlas.py's TILES list;
	// world/block_tiles_check.c fails the build if the two enums disagree. ⚠ These are
	// slots 42..47 for BLOCK ids 38..42 — the id and the slot are FOUR apart for the four
	// cooked meats, exactly as they are for the raw meats and the ores above, and the
	// furnace (one id, 42) claims TWO slots (46 unlit, 47 lit) because it is the first
	// block in this sheet whose face texture depends on its running state rather than on
	// which block it is.
	TILE_COOKED_PORKCHOP,    // 42
	TILE_COOKED_BEEF,        // 43
	TILE_COOKED_CHICKEN,     // 44
	TILE_COOKED_MUTTON,      // 45
	TILE_FURNACE_FRONT,      // 46 — unlit
	TILE_FURNACE_FRONT_LIT,  // 47 — lit

	// v1.9.0 "Storage"'s chest. NOT auto-numbered from TILE_FURNACE_FRONT_LIT: doing so
	// would give it value 48, which collides with ITEM_ICON_APPLE (source/gfx/item_icons.h)
	// — the sheet has had no free slot directly after 47 since v1.8.16 painted 48..56 with
	// item icons. This is a BLOCK FACE (world/registry.c's chest row points FACE_TOP at it
	// directly, so it is also the inventory icon — no gfx/item_icons.h entry needed), and it
	// claims the first slot free AFTER the icons: 57. tools/make_atlas.py's TILES list
	// mirrors this — "chest_top" is appended last, landing at index 57 by the same append-
	// only discipline as everything above it.
	TILE_CHEST_TOP = 57,

	// How many slots the list above NAMES. Not the sheet's capacity — that is
	// ATLAS_TILE_COUNT (64) in world/atlas_uv.h, and these forty-nine are 0..47 plus 57 of
	// it — no longer a contiguous run, which is why this can no longer be left to auto-
	// increment from TILE_CHEST_TOP (57 + 1 = 58 would be wrong).
	//
	// Nor is it the number of PAINTED slots any more. Since v1.9.0 that is 58, not 49: the
	// sheet also carries nine ITEM ICONS at 48..56 which this enum deliberately does not name.
	// See the note below TILE_USED_COUNT for why, and source/gfx/item_icons.h for the ids.
	//
	// It exists to be counted against, not to be used as a bound: world/block_tiles_check.c
	// asserts that the number of BTEX/TILE assert lines equals this number, which is what
	// turns "somebody forgot to add the assert" from silence into a build error. Same shape
	// as BLOCK_FACES in world/block.h, which terminates the face enum for the same reason.
	TILE_USED_COUNT = 49,

	// Forty-eight of ATLAS_TILE_COUNT (64) addressable slots used as block faces 0..47, plus
	// TILE_CHEST_TOP at 57 — forty-nine named slots total — twelve until v1.8.3 Phase 3
	// claimed 12..16 for snow, ice, cactus, dead bush and fern, seventeen until v1.8.8 claimed
	// 17..30 for the birch and spruce materials, the tall grass top, the four flowers and the
	// apple, thirty-one until v1.8.10 claimed 31 for the torch, thirty-two until v1.8.12
	// claimed 32..37 for the six ores, thirty-eight until v1.8.14 claimed 38..41 for the four
	// raw meats, forty-two until v1.8.15 claimed 42..47 for the four cooked meats and the
	// furnace's unlit/lit front faces, and forty-eight until v1.9.0 claimed slot 57 for the
	// chest — the first free slot after the v1.8.16 icon carve-out, not a contiguous append.
	//
	// v1.8.16 IMP-ICONS PAINTED 48..56 without naming them here, and the distinction is
	// the whole point of the change. Those nine are ITEM ICONS — an apple, four raw cuts, four
	// cooked cuts — with real alpha-0 texels around the item's silhouette, drawn only by
	// scene/ui.c's inventory quad. They must NEVER be a block face: the opaque terrain pass
	// runs with the alpha test off and the blend func at ONE/ZERO (scene/chunk_render.c), so a
	// transparent texel on a cube would write its RGB at full strength over whatever was
	// behind it rather than disappearing. Leaving them out of this enum is what makes that
	// unrepresentable — world/registry.c can only name a BTEX_*, every BTEX_* must have a
	// TILE_* twin (world/block_tiles_check.c asserts the two counts are equal), and neither
	// enum has a name for these. Their ids live in source/gfx/item_icons.h instead, which the
	// block-face path never includes.
	//
	// So TILE_USED_COUNT stays a count of block-face tiles: 48 -> 49 with the chest, and the
	// number that moved separately is ATLAS_PAINTED_SLOTS in world/atlas_uv_shader_test.c,
	// 57 -> 58, because that one counts what tools/make_atlas.py actually paints (block faces
	// plus item icons together).
	//
	// Five free slots remain, 58..62: slot 63 is ATLAS_TILE_MISSING (world/atlas_uv.h)
	// and cannot be claimed.
	//
	// It was twelve of FIFTEEN until v1.8.2's task 13b. The old ceiling was not the sheet
	// height: MeshVertex.v held an atlas pixel row, so a uint8_t capped the sheet at 256 px
	// = 16 slots and the top slot's top edge (v = 256) did not fit, leaving 15. Task 13b
	// changed that byte's UNITS to a slot-edge index, moved the TILE_PX factor into the
	// shaders' uvScale.y, and the sheet grew to 1024 px — the PICA200's maximum texture
	// dimension — for 64 slots, all addressable. The vertex is still 8 bytes. See the long
	// note in world/atlas_uv.h.
	//
	// The spares (58..62 as of v1.9.0, and the reserved 63 — this sentence said "32 spares
	// (32..62)" and had been stale since v1.8.14) are NOT blank. Since v1.6.0 F7
	// tools/make_atlas.py paints every slot its TILES list does not cover with the
	// magenta/black missing-texture marker, and ATLAS_TILE_MISSING (slot 63, world/atlas_uv.h)
	// is reserved
	// as one permanently — it is where atlasRect() clamps an out-of-range tile id. Before
	// that, tex 10..14 drew the sheet's near-black background fill and an out-of-range tex
	// drew grass, so a server shipping a wrong tex byte looked like a rendering bug here.
	// Appending a real tile to the list above simply overwrites the next spare's marker; 63 is
	// the one that cannot be taken, and tools/make_atlas.py refuses rather than letting it be.
};
