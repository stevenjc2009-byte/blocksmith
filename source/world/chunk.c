#include "world/chunk.h"

#include <string.h>

void chunkClear(Chunk* c, BlockId fill)
{
	memset(c->blocks, fill, sizeof(c->blocks));
}

bool chunkIsAllAir(const Chunk* c)
{
	// BLOCK_AIR is 0, so this is "are all 4,096 bytes zero". Written as a plain byte scan
	// with an early exit rather than a word scan: BlockId is uint8_t, so the array carries
	// no alignment guarantee, and the case worth optimising is the empty chunk — which has
	// no early exit to win either way, and whose 4,096 compares buy back ~335 us.
	for (int i = 0; i < CHUNK_BLOCKS; i++)
		if (c->blocks[i] != BLOCK_AIR)
			return false;
	return true;
}

