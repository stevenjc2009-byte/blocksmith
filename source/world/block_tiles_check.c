// The one place the host-testable world code meets the citro3d render code.
//
// world/block.h deliberately duplicates the atlas tile ids so it can stay free of
// <3ds.h> and be unit-tested on the PC. This file includes both sides and fails the build
// if they ever disagree — which is the only thing that makes the duplication safe.
//
// It was compiled only for the console until v1.8.3, because it included gfx/atlas.h and
// that header includes <3ds.h>. That made the sentence above true only of a devkitPro
// build: a BTEX/TILE mismatch compiled clean on the host and the whole 22-binary host
// suite stayed green (measured: TILE_TALL_GRASS given 12 while BTEX_TALL_GRASS stayed 11
// printed SUITE_EXIT=0 with zero FAIL lines anywhere in the run). The tile list moved into
// gfx/atlas_tiles.h, which has no 3DS dependency; this file includes that instead, and
// tools/run_host_tests.sh compiles it. The console build is unchanged — gfx/atlas.h
// includes the same header, so every TILE_* name still resolves exactly as before.
//
// Only the tile ids are checked here. The face order has a single definition now
// (world/block.h), so asserting it against itself could not fail; what actually proves
// it is testMesher() in world_test.c, which meshes one grass block and demands the top
// face carry BTEX_GRASS_TOP. The vertex layout is guarded by the _Static_assert inside
// world/mesh_vertex.h itself.
#include "gfx/atlas_tiles.h"
#include "world/block.h"

// The mirror, as data rather than as twelve hand-written assert lines.
//
// The hand-written form had a second defect underneath the console-only one, and it was the
// miss-prone half: one assert per tile means OMITTING a line leaves that tile unguarded and
// nothing complains, on either side. A list nobody can forget to extend is not achievable by
// asking people to remember; what is achievable is making the list COUNTABLE, so the number
// of asserts can be checked against the number of tiles. That is why the pairs live in an
// X-macro list — it is expanded twice below, once into the asserts and once into a count.
//
// Rejected: keeping the twelve asserts and adding `#define BTEX_TILE_ASSERTS_WRITTEN 12`
// beside them. That is a second hand-maintained number guarding the first one, i.e. the same
// defect one level up. Also rejected: generating this file from tools/make_atlas.py's TILES
// list, which would add a codegen step to a build that has none and put the guard in a file
// this task may not edit.
//
// Adding a tile: append to gfx/atlas_tiles.h above TILE_USED_COUNT, append to world/block.h's
// BTEX_* enum, and append one X() line here. Miss the third and the count assert at the bottom
// fails the build. Miss either of the first two and the X() line names an undeclared
// identifier, which is a build error already.
#define BS_BTEX_TILE_PAIRS(X)           \
	X(BTEX_GRASS_TOP,  TILE_GRASS_TOP)  \
	X(BTEX_GRASS_SIDE, TILE_GRASS_SIDE) \
	X(BTEX_DIRT,       TILE_DIRT)       \
	X(BTEX_STONE,      TILE_STONE)      \
	X(BTEX_SAND,       TILE_SAND)       \
	X(BTEX_SENTINEL,   TILE_SENTINEL)   \
	X(BTEX_WOOD_SIDE,  TILE_WOOD_SIDE)  \
	X(BTEX_WOOD_TOP,   TILE_WOOD_TOP)   \
	X(BTEX_LEAVES,     TILE_LEAVES)     \
	X(BTEX_PLANKS,     TILE_PLANKS)     \
	X(BTEX_WATER,      TILE_WATER)      \
	X(BTEX_TALL_GRASS, TILE_TALL_GRASS) \
	X(BTEX_SNOW,       TILE_SNOW)       \
	X(BTEX_ICE,        TILE_ICE)        \
	X(BTEX_CACTUS,     TILE_CACTUS)     \
	X(BTEX_DEAD_BUSH,  TILE_DEAD_BUSH)  \
	X(BTEX_FERN,       TILE_FERN)       \
	X(BTEX_BIRCH_LOG_SIDE,  TILE_BIRCH_LOG_SIDE)  \
	X(BTEX_BIRCH_LOG_TOP,   TILE_BIRCH_LOG_TOP)   \
	X(BTEX_BIRCH_PLANKS,    TILE_BIRCH_PLANKS)    \
	X(BTEX_BIRCH_LEAVES,    TILE_BIRCH_LEAVES)    \
	X(BTEX_SPRUCE_LOG_SIDE, TILE_SPRUCE_LOG_SIDE) \
	X(BTEX_SPRUCE_LOG_TOP,  TILE_SPRUCE_LOG_TOP)  \
	X(BTEX_SPRUCE_PLANKS,   TILE_SPRUCE_PLANKS)   \
	X(BTEX_SPRUCE_LEAVES,   TILE_SPRUCE_LEAVES)   \
	X(BTEX_TALL_GRASS_TOP,  TILE_TALL_GRASS_TOP)  \
	X(BTEX_POPPY,           TILE_POPPY)           \
	X(BTEX_DAISY,           TILE_DAISY)           \
	X(BTEX_BLUEBELL,        TILE_BLUEBELL)        \
	X(BTEX_ORCHID,          TILE_ORCHID)          \
	X(BTEX_APPLE,           TILE_APPLE)           \
	X(BTEX_TORCH,           TILE_TORCH)           \
	X(BTEX_COAL_ORE,        TILE_COAL_ORE)        \
	X(BTEX_IRON_ORE,        TILE_IRON_ORE)        \
	X(BTEX_GOLD_ORE,        TILE_GOLD_ORE)        \
	X(BTEX_REDSTONE_ORE,    TILE_REDSTONE_ORE)    \
	X(BTEX_LAPIS_ORE,       TILE_LAPIS_ORE)       \
	X(BTEX_DIAMOND_ORE,     TILE_DIAMOND_ORE)     \
	X(BTEX_RAW_PORKCHOP,    TILE_RAW_PORKCHOP)    \
	X(BTEX_RAW_BEEF,        TILE_RAW_BEEF)        \
	X(BTEX_RAW_CHICKEN,     TILE_RAW_CHICKEN)     \
	X(BTEX_RAW_MUTTON,      TILE_RAW_MUTTON)      \
	X(BTEX_COOKED_PORKCHOP, TILE_COOKED_PORKCHOP) \
	X(BTEX_COOKED_BEEF,     TILE_COOKED_BEEF)     \
	X(BTEX_COOKED_CHICKEN,  TILE_COOKED_CHICKEN)  \
	X(BTEX_COOKED_MUTTON,   TILE_COOKED_MUTTON)   \
	X(BTEX_FURNACE_FRONT,     TILE_FURNACE_FRONT)     \
	X(BTEX_FURNACE_FRONT_LIT, TILE_FURNACE_FRONT_LIT) \
	X(BTEX_CHEST_TOP,         TILE_CHEST_TOP)

// One _Static_assert per pair, exactly as before. The names are stringified into the message
// because every one of these expands from the same source line, so the message is the only
// thing that says WHICH tile drifted.
#define BS_BTEX_TILE_ASSERT(btex, tile)                                              \
	_Static_assert((int)(btex) == (int)(tile),                                       \
	               "atlas tile order changed - " #btex " and " #tile " disagree; "    \
	               "update world/block.h to match gfx/atlas_tiles.h");
BS_BTEX_TILE_PAIRS(BS_BTEX_TILE_ASSERT)
#undef BS_BTEX_TILE_ASSERT

// The count of the list above, built by expanding it into `+ 1` terms.
#define BS_BTEX_TILE_ONE(btex, tile) + 1
enum { BS_BTEX_TILE_PAIR_COUNT = 0 BS_BTEX_TILE_PAIRS(BS_BTEX_TILE_ONE) };
#undef BS_BTEX_TILE_ONE

// The guard the per-pair asserts cannot provide, because a tile with no assert line is not a
// pair and a pair-wise check can say nothing about it. It fails when a slot is added to
// gfx/atlas_tiles.h and the X() line above is not. Every per-pair assert stays green in that
// case — they all still hold — which is precisely why the old hand-written form could not see
// it, and it is why this assert is not redundant with them.
//
// The count is taken from the ATLAS side on purpose: gfx/atlas_tiles.h's order is the contract
// with tools/make_atlas.py, so it is the list that decides what a tile id means, and
// world/block.h's BTEX_* is the copy of it. A BTEX_USED_COUNT sentinel on the other side would
// close one more case — a BTEX_* appended alone, with neither a TILE_* nor an X() line — but
// world/block.h is vendored into deps/blocksmith-server/game/world/block.h and the Makefile's
// check-world-drift target fails any console build where the two differ, which needs a sync
// and a review in a repository this change does not own. Left undone deliberately, not missed.
_Static_assert((int)BS_BTEX_TILE_PAIR_COUNT == (int)TILE_USED_COUNT,
               "gfx/atlas_tiles.h names a number of tiles that BS_BTEX_TILE_PAIRS above does "
               "not match: a tile was added (or removed) without its assert, so that tile is "
               "unguarded. Add the missing X(BTEX_..., TILE_...) line.");
