#include "world/furnace.h"

#include <string.h>

#include "world/block.h"

// ── pack/unpack ─────────────────────────────────────────────────────────────────────────

void furnaceStateInit(FurnaceState* fs)
{
	memset(fs, 0, sizeof(*fs));
}

void furnaceStatePack(const FurnaceState* fs, uint8_t out[BLOCKSTATE_PAYLOAD_BYTES])
{
	memset(out, 0, BLOCKSTATE_PAYLOAD_BYTES);

	out[0] = (uint8_t)fs->input_item;
	out[1] = fs->input_count;
	out[2] = (uint8_t)fs->fuel_item;
	out[3] = fs->fuel_count;
	out[4] = (uint8_t)(fs->fuel_ticks_left & 0xFF);
	out[5] = (uint8_t)((fs->fuel_ticks_left >> 8) & 0xFF);
	out[6] = (uint8_t)(fs->cook_ticks & 0xFF);
	out[7] = (uint8_t)((fs->cook_ticks >> 8) & 0xFF);
	out[8] = (uint8_t)fs->output_item;
	out[9] = fs->output_count;
	out[10] = fs->lit;
	// [11..15] left zero by the memset above — reserved.
}

void furnaceStateUnpack(FurnaceState* fs, const uint8_t in[BLOCKSTATE_PAYLOAD_BYTES])
{
	fs->input_item      = (ItemId)in[0];
	fs->input_count     = in[1];
	fs->fuel_item       = (ItemId)in[2];
	fs->fuel_count      = in[3];
	fs->fuel_ticks_left = (uint16_t)(in[4] | ((uint16_t)in[5] << 8));
	fs->cook_ticks      = (uint16_t)(in[6] | ((uint16_t)in[7] << 8));
	fs->output_item     = (ItemId)in[8];
	fs->output_count    = in[9];
	fs->lit             = in[10];
}

// ── recipes ─────────────────────────────────────────────────────────────────────────────

// 200 ticks == 10s at TICK_HZ==20 (world/tick.h), for every row today — see the design doc's
// "Numbers" table, "Smelt time" row.
#define FURNACE_SMELT_TICKS 200

const FurnaceRecipe FURNACE_RECIPES[FURNACE_RECIPE_COUNT] = {
	[FURNACE_RECIPE_PORK] = {
		.name         = "Raw Porkchop -> Cooked Porkchop",
		.input_item   = BLOCK_RAW_PORKCHOP,
		.input_count  = 1,
		.output_item  = BLOCK_COOKED_PORKCHOP,
		.output_count = 1,
		.cook_ticks   = FURNACE_SMELT_TICKS,
	},
	[FURNACE_RECIPE_BEEF] = {
		.name         = "Raw Beef -> Cooked Beef",
		.input_item   = BLOCK_RAW_BEEF,
		.input_count  = 1,
		.output_item  = BLOCK_COOKED_BEEF,
		.output_count = 1,
		.cook_ticks   = FURNACE_SMELT_TICKS,
	},
	[FURNACE_RECIPE_CHICKEN] = {
		.name         = "Raw Chicken -> Cooked Chicken",
		.input_item   = BLOCK_RAW_CHICKEN,
		.input_count  = 1,
		.output_item  = BLOCK_COOKED_CHICKEN,
		.output_count = 1,
		.cook_ticks   = FURNACE_SMELT_TICKS,
	},
	[FURNACE_RECIPE_MUTTON] = {
		.name         = "Raw Mutton -> Cooked Mutton",
		.input_item   = BLOCK_RAW_MUTTON,
		.input_count  = 1,
		.output_item  = BLOCK_COOKED_MUTTON,
		.output_count = 1,
		.cook_ticks   = FURNACE_SMELT_TICKS,
	},
};

const FurnaceRecipe* furnaceRecipeForInput(ItemId item)
{
	if (item == ITEM_NONE) return NULL;
	for (int i = 0; i < FURNACE_RECIPE_COUNT; i++)
		if (FURNACE_RECIPES[i].input_item == item) return &FURNACE_RECIPES[i];
	return NULL;
}

// ── fuel ────────────────────────────────────────────────────────────────────────────────

bool furnaceIsFuel(ItemId item, uint16_t* out_ticks)
{
	uint16_t ticks;
	switch (item) {
	case BLOCK_PLANKS:
	case BLOCK_BIRCH_PLANKS:
	case BLOCK_SPRUCE_PLANKS:
		ticks = FURNACE_FUEL_TICKS_PLANKS;
		break;
	case BLOCK_WOOD:
	case BLOCK_BIRCH_LOG:
	case BLOCK_SPRUCE_LOG:
		ticks = FURNACE_FUEL_TICKS_LOG;
		break;
	default:
		return false;
	}
	if (out_ticks) *out_ticks = ticks;
	return true;
}

// ── tick ────────────────────────────────────────────────────────────────────────────────

// Whether `fs`'s output slot has room for one more of `recipe`'s result. Shared by the
// ignition gate (step 1) and the completion gate (step 3) so the two can never disagree
// about what "room" means.
static bool outputHasRoom(const FurnaceState* fs, const FurnaceRecipe* recipe)
{
	if (fs->output_item == ITEM_NONE) return true;
	if (fs->output_item != recipe->output_item) return false;
	return (uint32_t)fs->output_count + recipe->output_count <= INV_STACK_MAX;
}

bool furnaceTick(FurnaceState* fs)
{
	bool changed = false;

	// Step 1: ignition. Only when unlit — a furnace already burning is handled by step 2
	// regardless of what the input looks like right now (see this file's header for why
	// ignition and continued burning are gated differently).
	if (fs->fuel_ticks_left == 0) {
		const FurnaceRecipe* recipe = furnaceRecipeForInput(fs->input_item);
		if (recipe != NULL && fs->input_count > 0 && fs->fuel_count > 0 &&
		    outputHasRoom(fs, recipe)) {
			uint16_t ticks = 0;
			if (furnaceIsFuel(fs->fuel_item, &ticks)) {
				fs->fuel_count -= 1;
				if (fs->fuel_count == 0) fs->fuel_item = ITEM_NONE;
				fs->fuel_ticks_left = ticks;
				fs->lit = 1;
				changed = true;
			}
		}
	}

	// Step 2: burn-down. Unconditional once lit, independent of whether the input is (still)
	// valid — a real furnace does not pause a burning fuel item. See the design doc's
	// research-given note this file's header cites.
	if (fs->fuel_ticks_left > 0) {
		fs->fuel_ticks_left -= 1;
		changed = true;
		if (fs->fuel_ticks_left == 0) {
			fs->lit = 0;
			changed = true;
		}
	}

	// Step 3: cook progress. Only while lit, and only while the input is still valid and the
	// output has room — otherwise cook_ticks pauses (neither advances nor resets), matching
	// "a furnace with fuel but no valid input just burns fuel and does nothing useful".
	if (fs->lit) {
		const FurnaceRecipe* recipe = furnaceRecipeForInput(fs->input_item);
		if (recipe != NULL && fs->input_count > 0 && outputHasRoom(fs, recipe)) {
			fs->cook_ticks += 1;
			changed = true;
			if (fs->cook_ticks >= recipe->cook_ticks) {
				fs->input_count -= 1;
				if (fs->input_count == 0) fs->input_item = ITEM_NONE;

				if (fs->output_item == ITEM_NONE) fs->output_item = recipe->output_item;
				fs->output_count = (uint8_t)(fs->output_count + recipe->output_count);

				fs->cook_ticks = 0;
			}
		}
	}

	return changed;
}
