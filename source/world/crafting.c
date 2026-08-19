#include "world/crafting.h"

// ── The recipe table ──────────────────────────────────────────────────────────────────
//
// Three recipes, each justified from the actual block list (world/block.h) and from what
// worldgen.c actually does today — not invented for the sake of having a table. Every
// combination that was considered and rejected is noted below so the absence reads as a
// decision, not a gap.
//
// RECIPE_DIRT_TO_GRASS (1 dirt -> 1 grass)
//   Grass blocks are placed only by worldgen (world/worldgen.c's stoneCap()/decoration
//   pass puts BLOCK_GRASS on the single top solid block of a column) and nothing in
//   worldgen.c or world/handbuilt.c makes a grass block regrow or spread at runtime — a
//   dirt block that used to be grass, or a stone-and-dirt patch dug flat for a house
//   footprint, has no way back to a grass top except mining a fresh, undisturbed column
//   somewhere else. This recipe removes exactly that friction for a builder finishing a
//   lawn or a landscaped area around a build, at a plain 1:1 rate — it re-skins material
//   the player already has, so there is no economy reason to make it lossy.
//
// RECIPE_STONE_TO_SAND (4 stone -> 1 sand)
//   Stone is the most abundant block in the game — every column has it at depth. Sand is
//   geographically confined: worldgen.c's stoneCap() ("sandy replaces the whole
//   grass-and-dirt cap with sand", line ~117) only ever places it on the columns a biome
//   check marked sandy, i.e. beaches/deserts. A player building somewhere else who wants a
//   sand accent has to either travel there or grind surplus stone for it — this recipe is
//   the second option, deliberately lossy (4:1) because it should still be worse than just
//   mining sand where it naturally occurs, or it would erase the reason sand is scarce.
//
// RECIPE_LEAVES_TO_DIRT (4 leaves -> 1 dirt)
//   Leaves are the single most disposable, most renewable material in the game: a tree's
//   whole canopy is leaves (world/worldgen.c's treePut(), the GEN_TREE_RADIUS loop), far
//   outnumbering its trunk, and worldgen.c places a new tree on essentially every forested
//   chunk it generates — there is no scarcity to protect here, unlike wood. Composting
//   surplus canopy into dirt is a physically ordinary process and gives leaves a use once
//   a build is not actively using them, in particular for a player deep in a stone-only
//   cave with no soil underfoot and no easy way back to the surface. Lossy (4:1) for the
//   same reason as the stone recipe: still worse than just digging dirt where it exists.
//
// What is deliberately absent:
//
//   - Wood appears as the input or output of nothing. As an *output* it would let a
//     player synthesize the one raw material this world does not hand out on every
//     column (worldgen only places it inside a tree's trunk), which would erase the
//     scarcity that gives wood any value at all. As an *input*, nothing else in the block
//     list is a plausible product of processing a log — there is no planks or charcoal
//     block to turn it into, and inventing one purely to give wood a recipe would be
//     exactly the "invent a general item system for items that do not exist" this
//     project's block.h explicitly warns against.
//   - Sand -> stone (the reverse of RECIPE_STONE_TO_SAND) was considered — real-world
//     lithification goes that direction too — and rejected as pointless: stone is already
//     the more abundant of the two, so a recipe that turns scarce sand into abundant stone
//     has no player who would ever want it.
//   - Grass -> dirt (the reverse of RECIPE_DIRT_TO_GRASS) was considered and rejected as
//     redundant rather than wrong: dirt sits directly beneath the grass layer on every
//     column, so a player who wants dirt back from a grass block already has it one block
//     down without spending anything on a recipe.
const CraftRecipe CRAFT_RECIPES[RECIPE_COUNT] = {
	[RECIPE_DIRT_TO_GRASS] = {
		.name = "Dirt -> Grass",
		.input_item = BLOCK_DIRT, .input_count = 1,
		.output_item = BLOCK_GRASS, .output_count = 1,
	},
	[RECIPE_STONE_TO_SAND] = {
		.name = "Stone -> Sand",
		.input_item = BLOCK_STONE, .input_count = 4,
		.output_item = BLOCK_SAND, .output_count = 1,
	},
	[RECIPE_LEAVES_TO_DIRT] = {
		.name = "Leaves -> Dirt",
		.input_item = BLOCK_LEAVES, .input_count = 4,
		.output_item = BLOCK_DIRT, .output_count = 1,
	},
};

bool craftCanMake(const Inventory* inv, int recipe_index)
{
	if (!inv || recipe_index < 0 || recipe_index >= RECIPE_COUNT) return false;

	const CraftRecipe* r = &CRAFT_RECIPES[recipe_index];
	return inventoryCount(inv, r->input_item) >= r->input_count;
}

bool craftMake(Inventory* inv, int recipe_index)
{
	if (!inv || recipe_index < 0 || recipe_index >= RECIPE_COUNT) return false;
	if (!craftCanMake(inv, recipe_index)) return false;

	const CraftRecipe* r = &CRAFT_RECIPES[recipe_index];

	// Attempt the whole recipe on a scratch copy — Inventory is a small, fixed-size,
	// stack-allocated struct (no pointers inside it), so this is a plain value copy, not
	// an allocation. Nothing is written back to `inv` unless both the consume and the
	// produce steps succeed, which is what makes a refusal at either step leave `inv`
	// completely untouched without needing a hand-rolled undo.
	Inventory trial = *inv;

	const uint8_t removed = inventoryRemove(&trial, r->input_item, r->input_count);
	if (removed != r->input_count) return false;   // stale read since craftCanMake; bail, inv untouched

	uint8_t leftover = 0;
	const InvAddResult add = inventoryAdd(&trial, r->output_item, r->output_count, &leftover);
	if (add != INV_ADD_OK) return false;   // output has nowhere to go: abort, inv untouched

	*inv = trial;
	return true;
}
