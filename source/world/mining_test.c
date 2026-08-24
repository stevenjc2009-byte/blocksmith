// Host tests for the break-time arithmetic (roadmap task 50, part 1).
//
// Own main(), like every other suite binary under source/world: mining.c is plain C with no
// <3ds.h> and no state, so every claim mining.h makes about it is settleable here in
// milliseconds, and a broken break time must not stop the world suite from running.
//
// What each probe guards, and why it is the check that would go red:
//   testCoreHardness      - every core block's hardness byte, BY NAME, read back through the
//                           read-side view. This is the half that would go red if a value in
//                           kCoreDefs moved, and ALSO the half that goes red if refreshView()
//                           stops copying the field — the registry can hold the right byte
//                           and blockInfo() still answer 0.
//   testBreakTicks        - breakTicksRequired for the named blocks at bare hands. At 1x this
//                           must be the hardness itself and nothing else.
//   testNeverInstant      - a block with hardness never breaks in zero ticks, and a block
//                           with none never claims a break time it does not have.
//   testRoundingUp        - miningCeilDiv rounds UP rather than truncating, driven directly:
//                           see the comment on that probe for why the multiplier path cannot
//                           reach a fractional case yet and what is tested instead.
//   testSpeedMultiplier   - the tool table is 1x for everything, which is the STATED state of
//                           the game and not an accident, so it is pinned.
//
// The __3DS__ guard is load-bearing, not tidy: the console Makefile globs every .c under
// source/world and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>

#include "world/block.h"
#include "world/mining.h"
#include "world/registry.h"

static int g_checks = 0;
static int g_fails  = 0;

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   line %d: %s\n", line, what);
	} else {
		printf("  ok     %s\n", what);
	}
}

#define CHECK(cond, what) checkAt((cond), (what), __LINE__)

// The hand. world/inventory.h's ITEM_NONE is BLOCK_AIR, and an empty hand is what every
// break in this game happens with — there are no tools. Named so the call sites below read
// as intent rather than as "air".
#define BARE_HANDS ((ItemId)ITEM_NONE)

// ── Probes ───────────────────────────────────────────────────────────────────────────

static void testCoreHardness(void)
{
	puts("mining: every core block's hardness reads back through blockInfo()");

	// By name, one line each, deliberately NOT a loop over ids against a table: a loop needs
	// a second copy of the ten values to compare against, and a test that carries its own
	// copy of the data it is checking is a test of itself. These are the numbers as written
	// in the roadmap, typed out again here on purpose.
	CHECK(blockHardnessTicks(BLOCK_AIR)        ==  0, "air is 0 ticks");
	CHECK(blockHardnessTicks(BLOCK_GRASS)      == 12, "grass is 12 ticks (0.60 s)");
	CHECK(blockHardnessTicks(BLOCK_DIRT)       == 12, "dirt is 12 ticks (0.60 s)");
	CHECK(blockHardnessTicks(BLOCK_STONE)      == 45, "stone is 45 ticks (2.25 s)");
	CHECK(blockHardnessTicks(BLOCK_SAND)       == 10, "sand is 10 ticks (0.50 s)");
	CHECK(blockHardnessTicks(BLOCK_WOOD)       == 40, "wood is 40 ticks (2.00 s)");
	CHECK(blockHardnessTicks(BLOCK_LEAVES)     ==  4, "leaves are 4 ticks (0.20 s)");
	CHECK(blockHardnessTicks(BLOCK_PLANKS)     == 40, "planks are 40 ticks (2.00 s)");
	CHECK(blockHardnessTicks(BLOCK_WATER)      ==  0, "water is 0 ticks (never targetable)");
	CHECK(blockHardnessTicks(BLOCK_TALL_GRASS) ==  1, "tall grass is 1 tick (0.05 s)");

	// The view is derived from the def, so the two must agree. This is what goes red if
	// refreshView() copies the wrong field, or copies it from the wrong row.
	CHECK(registryGet(BLOCK_STONE)->hardness == blockHardnessTicks(BLOCK_STONE),
	      "the def's hardness byte and the view's agree for stone");
	CHECK(registryGet(BLOCK_LEAVES)->hardness == blockHardnessTicks(BLOCK_LEAVES),
	      "and for leaves, which is a different value: the copy is not a constant");

	// CONTROL. Reads nothing task 50 touched — it is the registry answering at all, over
	// data frozen since v1.6.0. Green in every arm below, and green in a build with every
	// hardness value wrong, which is what makes the red checks above evidence rather than
	// "the registry is broken".
	CHECK(blockIsSolid(BLOCK_STONE) && !blockIsSolid(BLOCK_AIR),
	      "control: the registry still answers about solidity, which task 50 did not touch");

	// A second control, on the value that must NOT move: an unknown id still reads back as
	// air, hardness included. That contract predates this task.
	CHECK(blockHardnessTicks((BlockId)0xFE) == 0,
	      "control: an undefined id reads back as air, so hardness 0");
}

static void testBreakTicks(void)
{
	puts("mining: breakTicksRequired at bare hands is the hardness itself");

	// At a 1x multiplier the arithmetic must be the identity. Anything else — a scale factor
	// left in, a divide by the wrong constant, a fixed-point unit mismatch — moves these.
	CHECK(breakTicksRequired(BLOCK_STONE,      BARE_HANDS) == 45, "stone takes 45 ticks");
	CHECK(breakTicksRequired(BLOCK_DIRT,       BARE_HANDS) == 12, "dirt takes 12 ticks");
	CHECK(breakTicksRequired(BLOCK_LEAVES,     BARE_HANDS) ==  4, "leaves take 4 ticks");
	CHECK(breakTicksRequired(BLOCK_TALL_GRASS, BARE_HANDS) ==  1, "tall grass takes 1 tick");
	CHECK(breakTicksRequired(BLOCK_GRASS,      BARE_HANDS) == 12, "grass takes 12 ticks");
	CHECK(breakTicksRequired(BLOCK_SAND,       BARE_HANDS) == 10, "sand takes 10 ticks");
	CHECK(breakTicksRequired(BLOCK_WOOD,       BARE_HANDS) == 40, "wood takes 40 ticks");
	CHECK(breakTicksRequired(BLOCK_PLANKS,     BARE_HANDS) == 40, "planks take 40 ticks");

	// The blocks with no break time. Neither is targetable, so neither is ever asked in the
	// game — but "never asked" is not "answers anything", and 0 is the answer that says
	// there is nothing to break rather than an instant break.
	CHECK(breakTicksRequired(BLOCK_AIR,   BARE_HANDS) == 0, "air takes 0 ticks: nothing to break");
	CHECK(breakTicksRequired(BLOCK_WATER, BARE_HANDS) == 0, "water takes 0 ticks: same");

	// What is in the hand does not matter today, and that is a claim worth pinning: it is
	// what task 32 will change, and the day it does, this check is the one that says so.
	CHECK(breakTicksRequired(BLOCK_STONE, (ItemId)BLOCK_PLANKS) ==
	      breakTicksRequired(BLOCK_STONE, BARE_HANDS),
	      "holding planks breaks stone exactly as fast as holding nothing: there are no tools");
}

static void testNeverInstant(void)
{
	puts("mining: a block with hardness never breaks in zero ticks");

	// Every core row with a non-zero hardness, checked as a rule rather than by value, so
	// this stays true when task 32 retunes the numbers underneath it.
	const BlockId ids[] = {
		BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE, BLOCK_SAND,
		BLOCK_WOOD, BLOCK_LEAVES, BLOCK_PLANKS, BLOCK_TALL_GRASS,
	};
	bool all_at_least_one = true;
	int  with_hardness    = 0;
	for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
		if (blockHardnessTicks(ids[i]) == 0) continue;
		with_hardness++;
		if (breakTicksRequired(ids[i], BARE_HANDS) < 1) all_at_least_one = false;
	}
	printf("         %d of %zu core blocks carry a hardness\n",
	       with_hardness, sizeof ids / sizeof ids[0]);

	CHECK(with_hardness == 8, "all eight breakable core blocks carry a non-zero hardness");
	CHECK(all_at_least_one, "and every one of them needs at least one tick");

	// The softest block in the registry is the case closest to the floor: one tick of
	// hardness at 1x must come back as one tick and not as zero.
	CHECK(blockHardnessTicks(BLOCK_TALL_GRASS) == 1 &&
	      breakTicksRequired(BLOCK_TALL_GRASS, BARE_HANDS) == 1,
	      "the softest block in the game, hardness 1, still costs a whole tick");

	// The property underneath the rule, driven directly. The `ticks < 1 ? 1 : ticks` clamp
	// in breakTicksRequired cannot fire today — ceil() of any non-zero numerator is at least
	// 1, whatever the divisor — so what is actually testable is that ceil never returns 0
	// for a non-zero numerator. That is what MAKES the never-instant rule hold, and a
	// truncating implementation breaks it at the first divisor bigger than the numerator.
	CHECK(miningCeilDiv(1, 1000000) == 1,
	      "ceil of 1/1000000 is 1, not 0: a truncating divide would make a break instant");
	CHECK(miningCeilDiv(0, 7) == 0, "but nothing to divide is still nothing: 0/7 is 0");
}

static void testRoundingUp(void)
{
	puts("mining: the division rounds UP, and is driven directly to prove it");

	// Why the helper and not breakTicksRequired: there is exactly one multiplier in the game
	// today (MINING_SPEED_ONE, 1x), and at 1x the division is hardness * ONE / ONE, which is
	// exact for every input. No fractional case can be reached through the public entry
	// point until task 32 adds a multiplier that is not 1 — so testing rounding through
	// breakTicksRequired today would be testing nothing and calling it green. The helper is
	// public for this reason (see mining.h), and it is the code the entry point calls.

	// The headline: a 2x tool on stone. 45 * 256 / 512 is 22.5, and a break must not finish
	// early, so it is 23. A truncating divide gives 22 and this goes red.
	CHECK(miningCeilDiv(45u * MINING_SPEED_ONE, 2u * MINING_SPEED_ONE) == 23,
	      "45 ticks at a 2x speed is 23 ticks, not 22: the half tick rounds up");

	// The same shape at other remainders, so the check is about rounding and not about one
	// lucky number.
	CHECK(miningCeilDiv(10, 4) == 3,  "10/4 rounds up to 3");
	CHECK(miningCeilDiv(11, 4) == 3,  "11/4 rounds up to 3");
	CHECK(miningCeilDiv(9,  4) == 3,  "9/4 rounds up to 3");
	CHECK(miningCeilDiv(13, 4) == 4,  "13/4 rounds up to 4");
	CHECK(miningCeilDiv(1,  4) == 1,  "1/4 rounds up to 1");

	// CONTROL. An exact division must NOT be nudged upwards — a rounding rule that adds one
	// unconditionally would pass every check above and be wrong. These are the cases that
	// separate "rounds up" from "always adds one", and they stay green in a truncating build
	// too, which is exactly what a control is for.
	CHECK(miningCeilDiv(8, 4) == 2,   "control: an exact 8/4 is 2, not 3");
	CHECK(miningCeilDiv(12, 4) == 3,  "control: an exact 12/4 is 3, not 4");
	CHECK(miningCeilDiv(45u * MINING_SPEED_ONE, MINING_SPEED_ONE) == 45,
	      "control: an exact 45x at 1x is 45, not 46");

	// And the guards, which have no sane answer and must not crash a break timer.
	CHECK(miningCeilDiv(0, 0) == 0, "0 over a zero divisor is 0");
	CHECK(miningCeilDiv(5, 0) == 5, "a zero divisor is treated as 1 rather than trapping");
}

static void testSpeedMultiplier(void)
{
	puts("mining: the tool table is 1x for everything, because there are no tools");

	// Pinned deliberately. This is the STATE of the game, not an oversight, and the day task
	// 32 changes it these are the lines that say which ids moved.
	bool all_one = true;
	for (int id = 0; id < 256; id++)
		if (miningSpeedMultiplier((ItemId)id) != MINING_SPEED_ONE) all_one = false;
	CHECK(all_one, "every one of the 256 ids gives the bare-hand 1x multiplier");

	CHECK(MINING_SPEED_ONE == 256u,
	      "the fixed-point scale is 256: a power of two, so the multiply is exact");
	CHECK(miningSpeedMultiplier(BARE_HANDS) == MINING_SPEED_ONE,
	      "an empty hand is 1x, stated rather than implied by the default case");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== mining test ==");

	registryInitCore();

	testCoreHardness();
	testBreakTicks();
	testNeverInstant();
	testRoundingUp();
	testSpeedMultiplier();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
