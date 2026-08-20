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
		// Transparent since step 7.5: the atlas tile now carries real alpha-0 holes and
		// the canopy is drawn in a second, alpha-tested pass after the opaque one.
		//
		// Still `solid`, because the two flags answer different questions. `solid` is
		// "does this fill its cell" — it drives collision, the raycast, ambient occlusion
		// and the mesher's face test, and all four should go on treating a leaf as a
		// block. `transparent` is "does this hide what is behind it", which is now no: the
		// sight walk in world/visgraph.c can see through a canopy, and the renderer knows
		// to defer these faces to the second pass.
		//
		// Leaf-against-leaf faces stay culled, because that test reads `solid`. That is
		// the cheap canopy — sky through the holes, not the inside of a hollow shell.
		.solid = true, .transparent = true, .liquid = false,
	},
	[BLOCK_PLANKS] = {
		.name = "planks",
		// The same tile on all six faces. A real plank block has a grain direction and
		// so arguably wants the top and bottom rotated 90 degrees, but that would need a
		// second tile for one block, and at this texel density and screen size the
		// rotation is not readable — it would be a cell of the atlas spent on something
		// nobody can see. Revisit if a future block genuinely needs a rotated variant.
		.tex  = { BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS,
		          BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS },
		// Fully opaque, unlike leaves: the tile has no alpha-0 pixels, so it belongs in
		// the first (opaque) pass and its faces against other solid blocks cull normally.
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
