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

#include "world/noise.h"
#include "world/world.h"

// The band the surface is allowed to occupy. Well clear of both ends of the 128-block
// world: below it there must be room for the caves of step 5.4, and above it room for
// trees plus the player's own height, without either having to clamp and produce a
// flat-topped world.
//
// **The range is the largest the movement code can walk.** Amplitude was swept against the
// slope distribution: at 28 the worst adjacent step over a 256x256 sample was 1 with 10.7 %
// of pairs stepping at all; at 40 it is still 1 at 15.2 %; at 56 four pairs step **2**,
// and a 2-block rise cannot be climbed by anything in this game (jump 1.29, auto-step 1).
// 40 keeps a real margin under that cliff and was re-checked across 24 seeds — worst step
// 1, zero 2-block pairs.
#define GEN_SURFACE_MIN   40
#define GEN_SURFACE_RANGE 40    // so the top block sits at y = 39 .. 79

// Depth of the dirt band under the grass. Three is enough to be visible in a cliff face
// and cheap enough not to matter.
#define GEN_DIRT_DEPTH    3

// Step 5.3's variation. A second, much larger-featured noise decides how hilly the ground
// is and what the top few blocks are made of, so the world has regions rather than one
// uniform texture of hills everywhere.
//
// 128-block features against the heightmap's 64: the biome has to be a couple of hills
// across or it reads as more hills rather than as somewhere different. Two octaves,
// because a biome field wants smooth boundaries — the detail octaves that make terrain
// interesting would make biome edges ragged and stipple sand into grass one column at a
// time.
//
// **128 was measured, not assumed.** At 256 blocks the biome is wider than anything a
// player can see at once, and across 24 seeds the sand fraction of a 192x192 neighbourhood
// came out 0.00 % on nine of them and over 99 % on five — every world reading as a single
// uniform biome, which is the opposite of a variation pass. At 128 no seed was below 7 %
// or above 92 %: every world has both, within walking distance. At 64 the biome is the
// same size as the hills and the answer collapses to ~45 % everywhere.
#define GEN_BIOME_SHIFT   7
#define GEN_BIOME_OCTAVES 2

// Below this the surface is sand rather than grass, and no trees grow. A hard threshold
// on a smooth field, so the boundary follows a contour of the noise — biomes have edges.
//
// **Chosen from a measured histogram, not by eye.** Value-noise fBm does not fill [0, 1]
// evenly: over a 1024x1024 sample it ran 0.075..0.936 with a median around 0.68, so the
// obvious-looking "a fifth of the range" (0x3000) selected 0.7 % of the world on one seed
// and 0 % of a 400-block neighbourhood on another — a biome nobody would ever see. 0x7800
// is the tenth percentile and gives roughly a tenth of the world.
#define GEN_SAND_BELOW    0x00007800    // 0.469 of the biome range, ~10 % of columns

// How flat the flattest terrain is, as a fraction of GEN_SURFACE_RANGE. Not zero: a biome
// with no relief at all is a billiard table, and the point of the variation pass is that
// somewhere is calmer than somewhere else, not that somewhere is dead.
//
// An eighth rather than a quarter for the same reason as the threshold above: with the
// biome clustered high, a quarter-to-full amplitude range came out as roughly
// three-quarters-to-full everywhere and the contrast was invisible.
#define GEN_FLAT_FRACTION 8

// Trees. One per 8x8 cell at most, so two trunks can never be closer than a couple of
// blocks and the decoration pass has a bounded neighbourhood to scan.
#define GEN_TREE_CELL     8
#define GEN_TREE_CHANCE   96            // out of 256 cells
#define GEN_TREE_MIN_H    4             // trunk blocks, before the canopy
#define GEN_TREE_MAX_H    7
#define GEN_TREE_RADIUS   2             // canopy half-width, in blocks

typedef struct {
	uint32_t seed;
} WorldGen;

void worldgenInit(WorldGen* g, uint32_t seed);

// Surface height at (x, z): the y of the first air block above the ground, so the top
// solid block is at `worldgenHeight() - 1`. Same convention as handbuiltHeight, so
// spawn-finding and the tests do not need to know which world they are looking at.
//
// Trees are NOT included: this is the ground, which is what spawn-finding and physics
// want. Standing on a canopy is a raycast/collision question, not a heightmap one.
int worldgenHeight(const WorldGen* g, int32_t x, int32_t z);

// The biome field at (x, z), in [0, FX_ONE]. Exposed because the tests need to check that
// the surface block and the tree placement agree with it rather than re-deriving the
// threshold, and because a caller that wants to describe where the player is standing
// should not have to guess.
fx worldgenBiome(const WorldGen* g, int32_t x, int32_t z);

// True where the top of the ground is sand rather than grass and dirt. One rule, one
// place: the column fill and the tree pass both ask this rather than each comparing
// against GEN_SAND_BELOW and drifting apart.
bool worldgenIsSandy(const WorldGen* g, int32_t x, int32_t z);

// Generates one column (all 8 chunks at cx, cz) into the world. False if the block
// budget or the column table refused an allocation — a real condition the caller must
// report rather than draw a hole for. Generating a column that already exists overwrites
// it, so this is not a way to preserve player edits; that is Phase 8's job.
bool worldgenColumn(const WorldGen* g, World* w, int32_t cx, int32_t cz);

// The decoration pass for one column: trees. Called by worldgenColumn after the ground is
// in, and exposed so the tests can run it on its own.
//
// It scans every tree whose canopy could reach this column — including trees whose trunks
// stand in a neighbouring column — and writes only the blocks that land inside (cx, cz).
// That clip is what makes the pass order-independent: a tree that wrote into its neighbour
// directly would be erased when that neighbour's ground was filled, which happens earlier,
// later, or in another session depending on where the player walked.
bool worldgenDecorate(const WorldGen* g, World* w, int32_t cx, int32_t cz);

// Generates a square of columns centred on (cx, cz), radius in columns. Returns the
// number of columns that failed, so a caller can report "generated 289, refused 3"
// instead of silently drawing a world with holes in it.
int worldgenArea(const WorldGen* g, World* w, int32_t cx, int32_t cz, int radius);
