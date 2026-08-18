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
#pragma once

#include "world/world.h"

#define SCRATCH_DIM     (CHUNK_DIM + 2)                                  // 18
#define SCRATCH_BLOCKS  (SCRATCH_DIM * SCRATCH_DIM * SCRATCH_DIM)        // 5832

typedef struct {
	BlockId blocks[SCRATCH_BLOCKS];
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

// Reads in *chunk-local* coordinates, which run -1..16: the mesher works in the
// chunk's own frame and stepping off the edge is normal, not an error.
static inline BlockId scratchAt(const MeshScratch* s, int lx, int ly, int lz)
{
	return s->blocks[scratchIndex(lx + 1, ly + 1, lz + 1)];
}
