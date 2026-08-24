// The padded meshing scratch: 18 x 18 x 18 blocks.
//
// One chunk (16³) plus a one-block border on every side, filled from **all 26
// neighbouring chunks**, not just the six face-adjacent ones. That is the whole
// point of this file, and it is here in Phase 2 rather than Phase 3 for one
// documented reason: ambient occlusion reads a vertex's diagonal neighbours, so a
// mesher built against six neighbours has to have its border handling rewritten
// the moment AO arrives. Every voxel tutorial that skips this pays for it later.
//
// 18³ is 5,832 bytes — one scratch is reused for every chunk meshed, so the cost
// is a rounding error against the block data itself.
//
// v1.5.0 adds a parallel light band, one packed byte per cell (sky in the high
// nibble, block in the low), filled by scratchFillLight from the per-column
// light store. It is only read when the lighting engine is enabled; when it is
// off the mesher never looks at it and vertices come out exactly as before.
#pragma once

#include "world/world.h"

#define SCRATCH_DIM     (CHUNK_DIM + 2)                                  // 18
#define SCRATCH_BLOCKS  (SCRATCH_DIM * SCRATCH_DIM * SCRATCH_DIM)        // 5832

// v1.8.0 task 22b. How the water band below encodes a cell:
//
//   0     a full cube — a water SOURCE, or a cell that holds no water at all.
//   1..7  a flow level, exactly the 1..7 world/water.h's side map stores.
//
// The mesher turns level L into a top surface L/8 of a block high, so 0 reads as 8/8 and is
// what every block that is not flowing water gets. That is why absence is 0 on both sides:
// world/water.h's map already means "no entry == source == full", so a memset(0) band and an
// unfilled band say the same true thing, and a build with no water simulation in the link
// (every host suite but the water one) renders exactly what it rendered before this task.
#define SCRATCH_WATER_STEPS  8

typedef struct {
	BlockId blocks[SCRATCH_BLOCKS];
	uint8_t light[SCRATCH_BLOCKS];   // sky<<4 | block; see scratchFillLight
	uint8_t water[SCRATCH_BLOCKS];   // 0 = full cube, 1..7 = flow level; see waterFillScratch

	// Whether `water` holds anything at all. Cleared by scratchFill and set only by
	// waterFillScratch, so a caller that never fills the band — the host suites, and the
	// console before the simulation is wired in — costs one branch in the mesher and pays
	// nothing else. It is NOT "there is water in this chunk": a lake of sources leaves it
	// false, because every one of those cells is a full cube already.
	bool    water_any;
} MeshScratch;

// sx/sy/sz are 0..17 — scratch space, where 0 is the border and 1..16 is the chunk.
static inline int scratchIndex(int sx, int sy, int sz)
{
	return (sy * SCRATCH_DIM + sz) * SCRATCH_DIM + sx;
}

// Copies chunk (cx,cy,cz) and its 26 neighbours into the scratch. Absent or
// unloaded neighbours read as air; anything below the world floor reads as
// WORLD_FLOOR_BLOCK, matching worldGet so the two can never disagree.
void scratchFill(MeshScratch* s, const World* w, int cx, int cy, int cz);

// Fills the light band for the same 27-chunk neighbourhood, reading each cell's
// owning column's channels. Call only while the lighting engine is enabled; a
// column with no light yet (still streaming in) reads as full sky, which keeps
// borders at today's brightness until its own propagation lands instead of
// painting black seams at the edge of the loaded ring.
void scratchFillLight(MeshScratch* s, const World* w, int cx, int cy, int cz);

// The water band is filled by waterFillScratch (world/water.h), which lives with the
// simulation because the flow levels do. It is declared there and not here so that scratch.c
// keeps its current link footprint: every host suite links this file, only one links
// world/water.c, and a call from here would drag the simulation into all of them.

// Reads in *chunk-local* coordinates, which run -1..16: the mesher works in the
// chunk's own frame and stepping off the edge is normal, not an error.
static inline BlockId scratchAt(const MeshScratch* s, int lx, int ly, int lz)
{
	return s->blocks[scratchIndex(lx + 1, ly + 1, lz + 1)];
}
