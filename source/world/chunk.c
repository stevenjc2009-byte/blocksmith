#include "world/chunk.h"

#include <string.h>

void chunkClear(Chunk* c, BlockId fill)
{
	memset(c->blocks, fill, sizeof(c->blocks));
}

