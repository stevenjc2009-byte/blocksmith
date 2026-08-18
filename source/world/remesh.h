// Which chunks have to be re-meshed when one block changes.
//
// Not just the chunk the block is in. A block on a chunk border is read by its
// neighbour's mesher through the scratch, and because AO reads diagonals, a block on
// a chunk *corner* is read by seven other chunks. Remeshing only the owning chunk is
// the classic voxel seam bug: a wall of shadow that stops dead at the chunk boundary.
#pragma once

#include "world/world.h"

#define REMESH_MAX 8   // a block on a chunk corner touches 2x2x2 chunks

typedef struct {
	int cx, cy, cz;
} ChunkCoord;

// Fills `out` with the chunks whose mesh can change, and returns how many. Chunks
// outside the world's vertical range are left out, so the caller can mesh every entry
// without checking. Returns 0 only if y is outside the world entirely.
int remeshList(int x, int y, int z, ChunkCoord out[REMESH_MAX]);
