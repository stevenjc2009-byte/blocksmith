// A chunk: 16 x 16 x 16 blocks, one byte each.
//
// Cubic on purpose. The Phase 7 cave-culling algorithm is defined on cubic chunks
// with six faces and a flood fill inside each one; a column-shaped chunk breaks it,
// and finding that out in Phase 7 would mean rewriting the mesher.
#pragma once

#include "world/block.h"

#define CHUNK_DIM     16
#define CHUNK_BLOCKS  (CHUNK_DIM * CHUNK_DIM * CHUNK_DIM)   // 4096 bytes

// x is the contiguous axis. The mesher and the padded scratch fill both walk x
// innermost, so a run of x is a memcpy rather than a loop.
static inline int chunkIndex(int x, int y, int z)
{
	return (y * CHUNK_DIM + z) * CHUNK_DIM + x;
}

typedef struct {
	BlockId blocks[CHUNK_BLOCKS];
} Chunk;

void    chunkClear(Chunk* c, BlockId fill);
