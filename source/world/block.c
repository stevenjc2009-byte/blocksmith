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
	[BLOCK_WOOD] = {
		.name = "wood",
		.tex  = { BTEX_WOOD_SIDE, BTEX_WOOD_SIDE, BTEX_WOOD_TOP,
		          BTEX_WOOD_TOP,  BTEX_WOOD_SIDE, BTEX_WOOD_SIDE },
		.solid = true, .transparent = false, .liquid = false,
	},
	[BLOCK_LEAVES] = {
		.name = "leaves",
		.tex  = { BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES,
		          BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES },
		// Opaque, not transparent. A cutout canopy needs a sorted second draw pass,
		// which is step 7.5; marking it transparent here would make the mesher emit
		// every interior leaf face for a pass that does not exist yet, and spend
		// triangles in the phase that is meant to be measuring generation.
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
