// The one place the host-testable world code meets the citro3d render code.
//
// world/block.h deliberately duplicates the atlas tile ids so it can stay free of
// <3ds.h> and be unit-tested on the PC. This file is compiled only for the console,
// includes both sides, and fails the build if they ever disagree — which is the only
// thing that makes the duplication safe.
//
// Only the tile ids are checked here. The face order has a single definition now
// (world/block.h), so asserting it against itself could not fail; what actually proves
// it is testMesher() in world_test.c, which meshes one grass block and demands the top
// face carry BTEX_GRASS_TOP. The vertex layout is guarded by the _Static_assert inside
// world/mesh_vertex.h itself.
#include "gfx/atlas.h"
#include "world/block.h"

_Static_assert((int)BTEX_GRASS_TOP  == (int)TILE_GRASS_TOP,  "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_GRASS_SIDE == (int)TILE_GRASS_SIDE, "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_DIRT       == (int)TILE_DIRT,       "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_STONE      == (int)TILE_STONE,      "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_SAND       == (int)TILE_SAND,       "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_SENTINEL   == (int)TILE_SENTINEL,   "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_WOOD_SIDE  == (int)TILE_WOOD_SIDE,  "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_WOOD_TOP   == (int)TILE_WOOD_TOP,   "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_LEAVES     == (int)TILE_LEAVES,     "atlas tile order changed - update world/block.h");
_Static_assert((int)BTEX_PLANKS     == (int)TILE_PLANKS,     "atlas tile order changed - update world/block.h");
