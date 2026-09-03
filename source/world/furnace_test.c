// Host self-test for world/furnace.c. Self-contained (its own main()), same reasoning as
// world/blockstate_test.c and world/inventory_test.c.
//
// The __3DS__ guard below is load-bearing rather than tidy, same reason as every other
// world/*_test.c: the Makefile globs every .c under source/world into the console build, so
// without it this file's main() would link against source/main.c's and the build would die
// with "multiple definition of `main'".
#include "world/furnace.h"

#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"

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

// How many CHECK()s this suite makes on a healthy tree. A LITERAL on purpose, never derived
// from FURNACE_RECIPE_COUNT or anything else the code under test can move -- see
// world/blockstate_test.c's own comment on its equivalent pin for why. Measured from this
// suite's own first clean run (not predicted): see checkCountPin() below and this lane's
// final report for the transcript.
//
// Measured (2026-09-03): seeded at a placeholder 129 before this suite ever ran once; the
// first real run reported "CHECK COUNT: 30 check(s) were ADDED - expected 129, ran 159" and
// exited 1, confirming the pin can go red. 159 is what it actually counted.
#define FURNACE_TEST_EXPECTED_CHECKS 159

static void checkCountPin(void)
{
	if (s_checks == FURNACE_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < FURNACE_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  FURNACE_TEST_EXPECTED_CHECKS to go green.\n",
		       FURNACE_TEST_EXPECTED_CHECKS - s_checks,
		       FURNACE_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         FURNACE_TEST_EXPECTED_CHECKS - s_checks,
			         FURNACE_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set FURNACE_TEST_EXPECTED_CHECKS in\n"
		       "  source/world/furnace_test.c to %d.\n",
		       s_checks - FURNACE_TEST_EXPECTED_CHECKS,
		       FURNACE_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - FURNACE_TEST_EXPECTED_CHECKS,
			         FURNACE_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// ── pack / unpack ──────────────────────────────────────────────────────────────────────

static void testPackUnpackRoundTrip(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item      = BLOCK_RAW_BEEF;
	fs.input_count     = 3;
	fs.fuel_item       = BLOCK_SPRUCE_LOG;
	fs.fuel_count      = 2;
	fs.fuel_ticks_left = 1199;   // exercises both bytes of a u16 (>255)
	fs.cook_ticks      = 199;
	fs.output_item     = BLOCK_COOKED_CHICKEN;
	fs.output_count    = 7;
	fs.lit             = 1;

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	furnaceStatePack(&fs, buf);

	FurnaceState back;
	memset(&back, 0xAA, sizeof(back));   // poison, so unpack must set every field itself
	furnaceStateUnpack(&back, buf);

	CHECK(back.input_item      == fs.input_item);
	CHECK(back.input_count     == fs.input_count);
	CHECK(back.fuel_item       == fs.fuel_item);
	CHECK(back.fuel_count      == fs.fuel_count);
	CHECK(back.fuel_ticks_left == fs.fuel_ticks_left);
	CHECK(back.cook_ticks      == fs.cook_ticks);
	CHECK(back.output_item     == fs.output_item);
	CHECK(back.output_count    == fs.output_count);
	CHECK(back.lit             == fs.lit);
}

static void testPackIsLittleEndianAndZeroesReserved(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.fuel_ticks_left = 0x0102;   // byte 4 must read 0x02, byte 5 must read 0x01
	fs.cook_ticks      = 0x0304;   // byte 6 must read 0x04, byte 7 must read 0x03

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	memset(buf, 0xFF, sizeof(buf));   // poison every byte first
	furnaceStatePack(&fs, buf);

	CHECK(buf[4] == 0x02);
	CHECK(buf[5] == 0x01);
	CHECK(buf[6] == 0x04);
	CHECK(buf[7] == 0x03);

	for (int i = FURNACE_PAYLOAD_USED_BYTES; i < BLOCKSTATE_PAYLOAD_BYTES; i++)
		CHECK(buf[i] == 0);   // reserved tail: pack must clear the poison, not leave it
}

static void testUnpackAllZeroMatchesInit(void)
{
	uint8_t zero[BLOCKSTATE_PAYLOAD_BYTES];
	memset(zero, 0, sizeof(zero));

	FurnaceState fromZero;
	memset(&fromZero, 0xAA, sizeof(fromZero));
	furnaceStateUnpack(&fromZero, zero);

	FurnaceState init;
	furnaceStateInit(&init);

	CHECK(fromZero.input_item      == init.input_item);
	CHECK(fromZero.input_count     == init.input_count);
	CHECK(fromZero.fuel_item       == init.fuel_item);
	CHECK(fromZero.fuel_count      == init.fuel_count);
	CHECK(fromZero.fuel_ticks_left == init.fuel_ticks_left);
	CHECK(fromZero.cook_ticks      == init.cook_ticks);
	CHECK(fromZero.output_item     == init.output_item);
	CHECK(fromZero.output_count    == init.output_count);
	CHECK(fromZero.lit             == init.lit);
	CHECK(init.input_item == ITEM_NONE);   // ITEM_NONE is BLOCK_AIR is 0 -- confirms the claim
	CHECK(init.fuel_item  == ITEM_NONE);   // furnace.h's unpack doc comment makes about this
	CHECK(init.output_item == ITEM_NONE);
}

// ── recipes ────────────────────────────────────────────────────────────────────────────

static void testRecipeLookupFindsAllFourMeats(void)
{
	const FurnaceRecipe* r;

	r = furnaceRecipeForInput(BLOCK_RAW_PORKCHOP);
	CHECK(r != NULL);
	if (r) {
		CHECK(r->input_item   == BLOCK_RAW_PORKCHOP);
		CHECK(r->output_item  == BLOCK_COOKED_PORKCHOP);
		CHECK(r->input_count  == 1);
		CHECK(r->output_count == 1);
		CHECK(r->cook_ticks   == 200);
	}

	r = furnaceRecipeForInput(BLOCK_RAW_BEEF);
	CHECK(r != NULL);
	if (r) CHECK(r->output_item == BLOCK_COOKED_BEEF);

	r = furnaceRecipeForInput(BLOCK_RAW_CHICKEN);
	CHECK(r != NULL);
	if (r) CHECK(r->output_item == BLOCK_COOKED_CHICKEN);

	r = furnaceRecipeForInput(BLOCK_RAW_MUTTON);
	CHECK(r != NULL);
	if (r) CHECK(r->output_item == BLOCK_COOKED_MUTTON);
}

static void testRecipeLookupRefusesNonInputs(void)
{
	CHECK(furnaceRecipeForInput(ITEM_NONE) == NULL);
	CHECK(furnaceRecipeForInput(BLOCK_STONE) == NULL);
	CHECK(furnaceRecipeForInput(BLOCK_COOKED_PORKCHOP) == NULL);   // output is not an input
	CHECK(furnaceRecipeForInput(BLOCK_FURNACE) == NULL);
}

// ── fuel ───────────────────────────────────────────────────────────────────────────────

static void testFuelLookupPlanksFamily(void)
{
	uint16_t ticks;

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_PLANKS, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_PLANKS);

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_BIRCH_PLANKS, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_PLANKS);

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_SPRUCE_PLANKS, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_PLANKS);
}

static void testFuelLookupLogFamily(void)
{
	uint16_t ticks;

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_WOOD, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_LOG);

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_BIRCH_LOG, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_LOG);

	ticks = 0;
	CHECK(furnaceIsFuel(BLOCK_SPRUCE_LOG, &ticks));
	CHECK(ticks == FURNACE_FUEL_TICKS_LOG);
}

static void testFuelLookupRefusesNonFuelAndLeavesOutUntouched(void)
{
	uint16_t ticks = 0xBEEF;   // sentinel: a false return must not touch this
	CHECK(!furnaceIsFuel(BLOCK_STONE, &ticks));
	CHECK(ticks == 0xBEEF);

	CHECK(!furnaceIsFuel(ITEM_NONE, &ticks));
	CHECK(ticks == 0xBEEF);

	CHECK(!furnaceIsFuel(BLOCK_RAW_BEEF, &ticks));   // food is not fuel
	CHECK(ticks == 0xBEEF);

	// NULL out_ticks must not crash on a true result either.
	CHECK(furnaceIsFuel(BLOCK_PLANKS, NULL));
}

// ── tick: idle / ignition gating ──────────────────────────────────────────────────────

static void testTickIdleUnlitFurnaceNeverChanges(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);

	CHECK(!furnaceTick(&fs));
	CHECK(fs.lit == 0);
	CHECK(fs.fuel_ticks_left == 0);
	CHECK(fs.cook_ticks == 0);
	CHECK(fs.input_item == ITEM_NONE);
	CHECK(fs.output_item == ITEM_NONE);
}

static void testTickDoesNotIgniteWithFuelButNoValidInput(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.fuel_item  = BLOCK_PLANKS;
	fs.fuel_count = 1;
	// input_item left ITEM_NONE

	CHECK(!furnaceTick(&fs));
	CHECK(fs.lit == 0);
	CHECK(fs.fuel_count == 1);   // untouched -- nothing was spent
	CHECK(fs.fuel_ticks_left == 0);
}

static void testTickDoesNotIgniteWithInputButNoFuel(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item  = BLOCK_RAW_PORKCHOP;
	fs.input_count = 1;
	// fuel_item left ITEM_NONE

	CHECK(!furnaceTick(&fs));
	CHECK(fs.lit == 0);
	CHECK(fs.input_count == 1);   // untouched
	CHECK(fs.cook_ticks == 0);
}

static void testTickDoesNotIgniteWhenOutputSlotBlocked(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item   = BLOCK_RAW_PORKCHOP;
	fs.input_count  = 1;
	fs.fuel_item    = BLOCK_PLANKS;
	fs.fuel_count   = 1;
	fs.output_item  = BLOCK_COOKED_PORKCHOP;
	fs.output_count = INV_STACK_MAX;   // full: no room for one more

	CHECK(!furnaceTick(&fs));
	CHECK(fs.lit == 0);
	CHECK(fs.fuel_count == 1);   // fuel not spent on a doomed ignition
	CHECK(fs.output_count == INV_STACK_MAX);
}

static void testTickDoesNotIgniteWhenOutputHoldsADifferentItem(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item   = BLOCK_RAW_PORKCHOP;
	fs.input_count  = 1;
	fs.fuel_item    = BLOCK_PLANKS;
	fs.fuel_count   = 1;
	fs.output_item  = BLOCK_COOKED_BEEF;   // wrong item already sitting in the output
	fs.output_count = 1;

	CHECK(!furnaceTick(&fs));
	CHECK(fs.lit == 0);
	CHECK(fs.fuel_count == 1);
}

static void testTickIgnitesWhenOutputHoldsSameItemWithRoom(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item   = BLOCK_RAW_PORKCHOP;
	fs.input_count  = 1;
	fs.fuel_item    = BLOCK_PLANKS;
	fs.fuel_count   = 1;
	fs.output_item  = BLOCK_COOKED_PORKCHOP;
	fs.output_count = 5;   // partial stack, room for more

	CHECK(furnaceTick(&fs));
	CHECK(fs.lit == 1);
	CHECK(fs.output_count == 5);   // ignition alone does not yet add output
}

// ── tick: a normal ignition, exact arithmetic ───────────────────────────────────────────

// Walks the very first tick of a cold furnace with input+fuel present and checks every field
// against furnace.h's own documented step order: ignite (consume 1 fuel, arm
// fuel_ticks_left), THEN burn down by 1 (unconditional once lit), THEN accumulate 1 cook
// tick (input still valid, output has room) -- all three steps fire on the SAME tick.
static void testTickIgnitionArithmeticOnFirstTick(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item  = BLOCK_RAW_MUTTON;
	fs.input_count = 4;
	fs.fuel_item   = BLOCK_SPRUCE_PLANKS;
	fs.fuel_count  = 2;

	CHECK(furnaceTick(&fs));

	CHECK(fs.fuel_count == 1);            // one unit of fuel consumed to ignite
	CHECK(fs.fuel_item == BLOCK_SPRUCE_PLANKS);   // one unit still held in reserve
	CHECK(fs.lit == 1);
	CHECK(fs.fuel_ticks_left == FURNACE_FUEL_TICKS_PLANKS - 1);   // armed, then burned down once
	CHECK(fs.cook_ticks == 1);            // step 3 ran on the same tick
	CHECK(fs.input_item == BLOCK_RAW_MUTTON);   // not consumed yet -- smelt is not complete
	CHECK(fs.input_count == 4);
}

static void testTickIgnitionConsumesLastFuelUnitToItemNone(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.input_item  = BLOCK_RAW_MUTTON;
	fs.input_count = 1;
	fs.fuel_item   = BLOCK_WOOD;
	fs.fuel_count  = 1;   // exactly one unit -- must drop the slot to ITEM_NONE

	CHECK(furnaceTick(&fs));
	CHECK(fs.fuel_count == 0);
	CHECK(fs.fuel_item == ITEM_NONE);
	CHECK(fs.lit == 1);
	CHECK(fs.fuel_ticks_left == FURNACE_FUEL_TICKS_LOG - 1);
}

// ── tick: burn-down is unconditional once lit ───────────────────────────────────────────

static void testTickBurnsDownEvenWithNoValidInput(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 50;
	// input_item left ITEM_NONE -- ignition already happened in the past (this state models
	// "mid-burn"), and the design says a burning fuel item never pauses.

	CHECK(furnaceTick(&fs));
	CHECK(fs.fuel_ticks_left == 49);
	CHECK(fs.lit == 1);        // did not reach 0 yet
	CHECK(fs.cook_ticks == 0);   // no valid input, so no cook progress either
}

static void testTickExtinguishesOnLastFuelTickAndDoesNotCookThatTick(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 1;
	fs.input_item      = BLOCK_RAW_BEEF;
	fs.input_count     = 1;

	CHECK(furnaceTick(&fs));
	CHECK(fs.fuel_ticks_left == 0);
	CHECK(fs.lit == 0);          // extinguished this same tick
	CHECK(fs.cook_ticks == 0);   // step 3 sees the freshly-extinguished state, not the pre-tick one
	CHECK(fs.input_count == 1);  // input untouched
}

// ── tick: cook completion ───────────────────────────────────────────────────────────────

static void testTickCookCompletesConsumesInputAndFillsEmptyOutput(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 50;   // plenty left -- stays lit through and after this tick
	fs.input_item      = BLOCK_RAW_PORKCHOP;
	fs.input_count     = 1;
	fs.cook_ticks      = 199;   // one short of FURNACE_RECIPES[FURNACE_RECIPE_PORK].cook_ticks

	CHECK(furnaceTick(&fs));

	CHECK(fs.cook_ticks == 0);              // reset after completion
	CHECK(fs.input_item == ITEM_NONE);      // last unit consumed
	CHECK(fs.input_count == 0);
	CHECK(fs.output_item == BLOCK_COOKED_PORKCHOP);
	CHECK(fs.output_count == 1);
	CHECK(fs.lit == 1);                     // still burning: fuel_ticks_left was 50, not 1
	CHECK(fs.fuel_ticks_left == 49);
}

static void testTickCookCompletesLeavesRemainingInputAndMergesIntoExistingOutputStack(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 50;
	fs.input_item      = BLOCK_RAW_CHICKEN;
	fs.input_count     = 3;         // more than one unit queued
	fs.cook_ticks      = 199;
	fs.output_item     = BLOCK_COOKED_CHICKEN;
	fs.output_count    = 6;         // an existing partial stack

	CHECK(furnaceTick(&fs));

	CHECK(fs.input_item == BLOCK_RAW_CHICKEN);   // NOT consumed to ITEM_NONE -- 2 left
	CHECK(fs.input_count == 2);
	CHECK(fs.output_item == BLOCK_COOKED_CHICKEN);
	CHECK(fs.output_count == 7);    // merged onto the existing stack
	CHECK(fs.cook_ticks == 0);
}

// ── tick: cook progress pauses, never resets, when it cannot advance ───────────────────

static void testTickCookPausesWhenInputRemovedMidCook(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 10;
	fs.input_item      = ITEM_NONE;   // pulled out mid-cook
	fs.cook_ticks      = 50;          // banked progress from before it was pulled

	CHECK(furnaceTick(&fs));
	CHECK(fs.cook_ticks == 50);   // neither advanced nor reset -- paused
	CHECK(fs.fuel_ticks_left == 9);   // fuel still burns regardless
}

static void testTickCookPausesWhenOutputSlotFillsUpMidCook(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 10;
	fs.input_item      = BLOCK_RAW_PORKCHOP;
	fs.input_count     = 1;
	fs.cook_ticks      = 50;
	fs.output_item     = BLOCK_COOKED_PORKCHOP;
	fs.output_count    = INV_STACK_MAX;   // no room for the next completed unit

	CHECK(furnaceTick(&fs));
	CHECK(fs.cook_ticks == 50);       // paused, not reset -- the banked progress is not lost
	CHECK(fs.input_count == 1);       // not consumed either: the smelt never completed
	CHECK(fs.fuel_ticks_left == 9);
}

// ── tick: fuel runs out mid-cook, then resumes from banked progress on new fuel ────────

static void testTickCookPausesOnFuelExhaustionThenResumesWithReserveFuel(void)
{
	FurnaceState fs;
	furnaceStateInit(&fs);
	fs.lit             = 1;
	fs.fuel_ticks_left = 2;
	fs.fuel_item       = BLOCK_PLANKS;
	fs.fuel_count      = 1;             // one more unit in reserve, not yet burning
	fs.input_item      = BLOCK_RAW_PORKCHOP;
	fs.input_count     = 1;
	fs.cook_ticks      = 100;

	CHECK(furnaceTick(&fs));            // tick 1: burns down 2 -> 1, still lit, cooks +1
	CHECK(fs.fuel_ticks_left == 1);
	CHECK(fs.lit == 1);
	CHECK(fs.cook_ticks == 101);

	CHECK(furnaceTick(&fs));            // tick 2: burns down 1 -> 0, extinguishes this tick,
	CHECK(fs.fuel_ticks_left == 0);     // so step 3 does not run for it
	CHECK(fs.lit == 0);
	CHECK(fs.cook_ticks == 101);        // unchanged from tick 1

	CHECK(furnaceTick(&fs));            // tick 3: re-ignites off the reserve unit, burns down
	CHECK(fs.fuel_count == 0);          // once, cooks once -- all three steps fire again
	CHECK(fs.fuel_item == ITEM_NONE);
	CHECK(fs.lit == 1);
	CHECK(fs.fuel_ticks_left == FURNACE_FUEL_TICKS_PLANKS - 1);
	CHECK(fs.cook_ticks == 102);        // resumed from 101, not reset to 0 or 1
}

// ── blockstate integration: no second side table ────────────────────────────────────────

// Proves the whole point of furnace.h's own header comment: a furnace's live state round-
// trips through world/blockstate.c's real 16-byte opaque payload and nothing else. If this
// ever needed a second table, this is the test that would have to change to prove it.
static void testRoundTripsThroughRealBlockstateTable(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(blockStateCreate(&t, 11, 64, -3, BLOCK_FURNACE));

	uint8_t raw[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 11, 64, -3, BLOCK_FURNACE, raw));

	FurnaceState fs;
	furnaceStateUnpack(&fs, raw);
	CHECK(fs.input_item == ITEM_NONE);   // a freshly-created furnace starts empty

	fs.input_item  = BLOCK_RAW_BEEF;
	fs.input_count = 1;
	fs.fuel_item   = BLOCK_PLANKS;
	fs.fuel_count  = 1;
	CHECK(furnaceTick(&fs));   // ignite + burn-down + one cook tick, via the real function

	uint8_t packed[BLOCKSTATE_PAYLOAD_BYTES];
	furnaceStatePack(&fs, packed);
	CHECK(blockStateSet(&t, 11, 64, -3, BLOCK_FURNACE, packed));

	uint8_t readBack[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 11, 64, -3, BLOCK_FURNACE, readBack));
	CHECK(memcmp(readBack, packed, sizeof(packed)) == 0);

	FurnaceState fromTable;
	furnaceStateUnpack(&fromTable, readBack);
	CHECK(fromTable.lit == 1);
	CHECK(fromTable.cook_ticks == 1);
	CHECK(fromTable.fuel_ticks_left == FURNACE_FUEL_TICKS_PLANKS - 1);
	CHECK(fromTable.input_item == BLOCK_RAW_BEEF);

	// Breaking the furnace removes its record, same rule every other stateful block follows.
	blockStateRemove(&t, 11, 64, -3);
	CHECK(!blockStateGet(&t, 11, 64, -3, BLOCK_FURNACE, readBack));
	CHECK(blockStateCount(&t) == 0);
}

int main(void)
{
	testPackUnpackRoundTrip();
	testPackIsLittleEndianAndZeroesReserved();
	testUnpackAllZeroMatchesInit();

	testRecipeLookupFindsAllFourMeats();
	testRecipeLookupRefusesNonInputs();

	testFuelLookupPlanksFamily();
	testFuelLookupLogFamily();
	testFuelLookupRefusesNonFuelAndLeavesOutUntouched();

	testTickIdleUnlitFurnaceNeverChanges();
	testTickDoesNotIgniteWithFuelButNoValidInput();
	testTickDoesNotIgniteWithInputButNoFuel();
	testTickDoesNotIgniteWhenOutputSlotBlocked();
	testTickDoesNotIgniteWhenOutputHoldsADifferentItem();
	testTickIgnitesWhenOutputHoldsSameItemWithRoom();

	testTickIgnitionArithmeticOnFirstTick();
	testTickIgnitionConsumesLastFuelUnitToItemNone();

	testTickBurnsDownEvenWithNoValidInput();
	testTickExtinguishesOnLastFuelTickAndDoesNotCookThatTick();

	testTickCookCompletesConsumesInputAndFillsEmptyOutput();
	testTickCookCompletesLeavesRemainingInputAndMergesIntoExistingOutputStack();

	testTickCookPausesWhenInputRemovedMidCook();
	testTickCookPausesWhenOutputSlotFillsUpMidCook();

	testTickCookPausesOnFuelExhaustionThenResumesWithReserveFuel();

	testRoundTripsThroughRealBlockstateTable();

	checkCountPin();

	if (s_fails == 0)
		printf("furnace self-test: PASS  %d checks\n", s_checks);
	else
		printf("furnace self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: an empty translation unit is not valid ISO C, so give the compiler one
// declaration to chew on. Same shape as every other world/*_test.c's else-branch.
typedef int furnace_test_host_only_t;

#endif   // !__3DS__
