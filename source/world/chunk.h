// A chunk: 16 x 16 x 16 blocks, one byte each.
//
// Cubic on purpose. The Phase 7 cave-culling algorithm is defined on cubic chunks
// with six faces and a flood fill inside each one; a column-shaped chunk breaks it,
// and finding that out in Phase 7 would mean rewriting the mesher.
#pragma once

#include <stdbool.h>

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

// True if every block in the chunk is BLOCK_AIR.
//
// The mesher emits a face only for a solid cell *inside* the chunk — neighbours are read
// for occlusion, never as a source of geometry — so an all-air chunk is guaranteed to
// mesh to nothing whatever surrounds it. That makes this a sound early-out for a remesh:
// scene/chunk_render.c skips both the 27-chunk scratch fill and the mesher walk when this
// returns true, which since 2026-08-18 is a case the game reaches routinely, because a
// chunk dug out to nothing now keeps its mesh slot and therefore stays queueable.
//
// Scans bytes and stops at the first non-air one, so a terrain chunk costs a handful of
// reads; only a genuinely empty chunk pays the full 4,096, and that is exactly the chunk
// the early-out then saves ~335 us of work on.
bool    chunkIsAllAir(const Chunk* c);
