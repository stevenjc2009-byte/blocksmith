// Terrain generation: turns a seed and a column coordinate into blocks.
//
// The unit of generation is a **column** — all 8 chunks at one (cx, cz) — not a chunk.
// The heightmap is a function of (x, z) only, so generating a column computes 256 heights
// once and then fills straight down through the stack; generating chunk-at-a-time would
// recompute the same 256 heights up to eight times for the same answer. It also matches
// how the world stores things (world.h) and how saving will work in Phase 8, both of which
// are per-(x, z).
//
// Deterministic and order-independent: see world/rng.h for why terrain hashes a position
// instead of drawing from a stream. Two runs with the same seed produce the same world,
// and so does the same run reached by a different route.
//
// <3ds.h>-free, so the tests generate real terrain on the PC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/world.h"

// The band the surface is allowed to occupy. Deliberately narrow and well clear of both
// ends of the 128-block world: below it there must be room for the caves of step 5.4, and
// above it room for trees in 5.3 plus the player's own height, without either having to
// clamp and produce a flat-topped world.
#define GEN_SURFACE_MIN   40
#define GEN_SURFACE_RANGE 28    // so the top block sits at y = 39 .. 67

// Depth of the dirt band under the grass. Three is enough to be visible in a cliff face
// and cheap enough not to matter.
#define GEN_DIRT_DEPTH    3

typedef struct {
	uint32_t seed;
} WorldGen;

void worldgenInit(WorldGen* g, uint32_t seed);

// Surface height at (x, z): the y of the first air block above the ground, so the top
// solid block is at `worldgenHeight() - 1`. Same convention as handbuiltHeight, so
// spawn-finding and the tests do not need to know which world they are looking at.
int worldgenHeight(const WorldGen* g, int32_t x, int32_t z);

// Generates one column (all 8 chunks at cx, cz) into the world. False if the block
// budget or the column table refused an allocation — a real condition the caller must
// report rather than draw a hole for. Generating a column that already exists overwrites
// it, so this is not a way to preserve player edits; that is Phase 8's job.
bool worldgenColumn(const WorldGen* g, World* w, int32_t cx, int32_t cz);

// Generates a square of columns centred on (cx, cz), radius in columns. Returns the
// number of columns that failed, so a caller can report "generated 289, refused 3"
// instead of silently drawing a world with holes in it.
int worldgenArea(const WorldGen* g, World* w, int32_t cx, int32_t cz, int radius);
