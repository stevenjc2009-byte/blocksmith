// Host self-test for v1.9.0 "Storage"'s RECIPE_PLANKS_TO_CHEST, world/crafting.c's newest row.
//
// Self-contained (its own main(), same reasoning as world/inventory_test.c's own comment)
// rather than plugged into tools/run_host_tests.sh's shared harness — that script is owned by
// whoever is wiring v1.9.0's other pieces together right now, and this file's job (one new
// recipe) is narrow enough not to need the shared rig.
//
// world/inventory_test.c's testEveryRecipeCrafts()/testRecipeWithInsufficientInputsRefusesAndConsumesNothing()
// already prove every row of CRAFT_RECIPES round-trips through craftMake() generically, by
// looping to RECIPE_COUNT — so the moment this recipe exists, that suite covers its happy path
// too, the same way scene/craft_torch_e2e_test.c's own comment notes for
// RECIPE_COAL_ORE_TO_TORCH. What that generic loop does NOT check is whether THIS SPECIFIC
// recipe is the one that landed, at the item/count the design settled on
// (docs/plan-1.9.0-storage-qol.md §3.6: `{ "Chest", BLOCK_PLANKS, 6, BLOCK_CHEST, 1 }`), or
// whether RECIPE_COUNT moved by exactly the one slot this recipe should cost it — a generic
// loop is blind to both, the same "a check that can't fail proves nothing" / "test order must
// match program order" lessons this project has already paid for once with sabotage-tested
// suites. This file exists to pin those two things specifically.
//
// The __3DS__ guard is load-bearing, not tidy — see world/inventory_test.c's own comment for
// the exact "multiple definition of `main'" failure this avoids: the Makefile globs every .c
// under source/world into the console build.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/crafting.h"
#include "world/inventory.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// RECIPE_COUNT immediately before this recipe existed (v1.8.15 "Furnace" left it at 6 — see
// world/crafting.h's enum: DIRT_TO_GRASS, STONE_TO_SAND, LEAVES_TO_DIRT, WOOD_TO_PLANKS,
// COAL_ORE_TO_TORCH, STONE_TO_FURNACE, six named rows ahead of RECIPE_COUNT). A literal, for
// the identical reason world/inventory_test.c's own count-pin comment gives: a number computed
// from the constant it is pinning shrinks along with any accidental shrink of that constant and
// guards nothing. RECIPE_PLANKS_TO_CHEST must move RECIPE_COUNT from this number to exactly one
// more — never further (a second recipe snuck in with it) and never not at all (dropped from
// the table but left in the enum, or vice versa).
#define RECIPE_COUNT_BEFORE_CHEST 6

static void testRecipeCountMovedByExactlyOne(void)
{
	CHECK(RECIPE_COUNT == RECIPE_COUNT_BEFORE_CHEST + 1);
}

static void testChestRecipeReachableByEnum(void)
{
	// Appended, not inserted — same wire-safety reasoning crafting.c's comment gives for
	// RECIPE_STONE_TO_FURNACE: this must be the LAST slot, or every old client's saved recipe
	// index would silently point at a different recipe than the one it crafted against.
	CHECK(RECIPE_PLANKS_TO_CHEST == RECIPE_COUNT - 1);

	const CraftRecipe* r = &CRAFT_RECIPES[RECIPE_PLANKS_TO_CHEST];
	CHECK(strcmp(r->name, "Planks -> Chest") == 0);
	CHECK(r->input_item   == BLOCK_PLANKS);
	CHECK(r->input_count  == 6);
	CHECK(r->output_item  == BLOCK_CHEST);
	CHECK(r->output_count == 1);
}

static void testChestRecipeCraftsForSixPlanks(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_PLANKS, 6, NULL) == INV_ADD_OK);

	CHECK(craftCanMake(&inv, RECIPE_PLANKS_TO_CHEST));
	CHECK(craftMake(&inv, RECIPE_PLANKS_TO_CHEST));

	CHECK(inventoryCount(&inv, BLOCK_PLANKS) == 0);   // fully consumed
	CHECK(inventoryCount(&inv, BLOCK_CHEST)  == 1);   // exactly one chest produced
}

static void testChestRecipeRefusesFivePlanksAndConsumesNothing(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_PLANKS, 5, NULL) == INV_ADD_OK);   // one short

	const Inventory before = inv;
	CHECK(!craftCanMake(&inv, RECIPE_PLANKS_TO_CHEST));
	CHECK(!craftMake(&inv, RECIPE_PLANKS_TO_CHEST));
	CHECK(inventoryCount(&inv, BLOCK_PLANKS) == 5);   // nothing consumed
	CHECK(inventoryCount(&inv, BLOCK_CHEST)  == 0);   // nothing produced
	CHECK(memcmp(&inv, &before, sizeof(inv)) == 0);   // byte-for-byte untouched
}

// crafting.c:169/177's runtime bound (`recipe_index >= RECIPE_COUNT`) must reject exactly one
// past the NEW end, not the old one — proving this recipe actually widened the accepted range,
// rather than the bound having been left pinned at the pre-chest RECIPE_COUNT some other way
// (e.g. a stray literal instead of the symbol).
static void testBoundsRejectOnePastNewEndButAcceptTheNewLastRecipe(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_PLANKS, 100, NULL) == INV_ADD_OK);   // plenty, so a false
	                                                                     // result below can only
	                                                                     // be the bounds check,
	                                                                     // never a starved input

	CHECK(!craftCanMake(&inv, RECIPE_COUNT));    // one past the new end: must be refused
	CHECK(!craftMake(&inv, RECIPE_COUNT));

	CHECK(craftCanMake(&inv, RECIPE_COUNT - 1));   // the real last recipe (the chest) still works
}

// Same "a check that can't fail proves nothing" lesson world/inventory_test.c's own pin comment
// gives, applied here: this file was orphaned for its whole life (never wired into
// tools/run_host_tests.sh, never run once until the host-test audit found it), so nothing ever
// caught a CHECK silently stopping being reached. Measured, not summed: the first host run of
// this file printed 22 checks total.
#define CRAFTING_TEST_EXPECTED_CHECKS 22

// Deliberately NOT routed through CHECK(): this must not perturb the number it is testing, so
// it bumps s_fails only, the same convention world/inventory_test.c's checkCountPin() uses.
static void checkCountPin(void)
{
	if (s_checks == CRAFTING_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < CRAFTING_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  CRAFTING_TEST_EXPECTED_CHECKS to go green.\n",
		       CRAFTING_TEST_EXPECTED_CHECKS - s_checks,
		       CRAFTING_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         CRAFTING_TEST_EXPECTED_CHECKS - s_checks,
			         CRAFTING_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If added on purpose, set CRAFTING_TEST_EXPECTED_CHECKS in\n"
		       "  source/world/crafting_test.c to %d.\n",
		       s_checks - CRAFTING_TEST_EXPECTED_CHECKS,
		       CRAFTING_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - CRAFTING_TEST_EXPECTED_CHECKS,
			         CRAFTING_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

int main(void)
{
	testRecipeCountMovedByExactlyOne();
	testChestRecipeReachableByEnum();
	testChestRecipeCraftsForSixPlanks();
	testChestRecipeRefusesFivePlanksAndConsumesNothing();
	testBoundsRejectOnePastNewEndButAcceptTheNewLastRecipe();

	checkCountPin();

	if (s_fails == 0)
		printf("crafting chest-recipe self-test: PASS  %d checks\n", s_checks);
	else
		printf("crafting chest-recipe self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#endif // __3DS__
