#include "world/block.h"

// tex is { east, west, top, bottom, south, north }.
static const BlockInfo kBlocks[BLOCK_COUNT] = {
	[BLOCK_AIR] = {
		.name = "air",
		.tex  = { 0, 0, 0, 0, 0, 0 },
		.solid = false, .transparent = true, .liquid = false,
	},
	[BLOCK_GRASS] = {
		.name = "grass",
		.tex  = { BTEX_GRASS_SIDE, BTEX_GRASS_SIDE, BTEX_GRASS_TOP,
		          BTEX_DIRT,       BTEX_GRASS_SIDE, BTEX_GRASS_SIDE },
		.solid = true, .transparent = false, .liquid = false,
	},
	[BLOCK_DIRT] = {
		.name = "dirt",
		.tex  = { BTEX_DIRT, BTEX_DIRT, BTEX_DIRT, BTEX_DIRT, BTEX_DIRT, BTEX_DIRT },
		.solid = true, .transparent = false, .liquid = false,
	},
	[BLOCK_STONE] = {
		.name = "stone",
		.tex  = { BTEX_STONE, BTEX_STONE, BTEX_STONE, BTEX_STONE, BTEX_STONE, BTEX_STONE },
		.solid = true, .transparent = false, .liquid = false,
	},
	[BLOCK_SAND] = {
		.name = "sand",
		.tex  = { BTEX_SAND, BTEX_SAND, BTEX_SAND, BTEX_SAND, BTEX_SAND, BTEX_SAND },
		.solid = true, .transparent = false, .liquid = false,
	},
};

const BlockInfo* blockInfo(BlockId id)
{
	if (id >= BLOCK_COUNT) return &kBlocks[BLOCK_AIR];
	return &kBlocks[id];
}

uint8_t blockFaceTex(BlockId id, int face)
{
	const BlockInfo* info = blockInfo(id);
	if (face < 0 || face >= BLOCK_FACES) return info->tex[0];
	return info->tex[face];
}
