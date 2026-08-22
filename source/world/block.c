#include "world/block.h"

#include "world/registry.h"

// v1.6.0 Phase A: the master table moved into world/registry.c, which owns both
// the compiled-in core rows and any dynamic rows registered at boot/join.
// blockInfo() stays as the engine-wide read path — same signature, same
// never-NULL contract (an unknown id answers air) — it just delegates now.
const BlockInfo* blockInfo(BlockId id)
{
	return registryView(id);
}

uint8_t blockFaceTex(BlockId id, int face)
{
	const BlockInfo* info = blockInfo(id);
	if (face < 0 || face >= BLOCK_FACES) return info->tex[0];
	return info->tex[face];
}
