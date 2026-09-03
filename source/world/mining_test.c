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

	// v1.8.3 Phase 3's five rows, absent from this list until v1.8.8. Their absence was not
	// an oversight in this file so much as a consequence of the item ceiling: none of these
	// ids could be broken, so their hardness byte was a number nothing read, and a test of an
	// unread number would have looked like diligence and proved nothing. With the ceiling
	// widened they are live values, so they are pinned here the same way the ten above are —
	// by name, one line each, typed from the intent rather than read back off the table.
	CHECK(blockHardnessTicks(BLOCK_SNOW)      ==  8, "snow is 8 ticks (0.40 s)");
	CHECK(blockHardnessTicks(BLOCK_ICE)       == 10, "ice is 10 ticks (0.50 s)");
	CHECK(blockHardnessTicks(BLOCK_CACTUS)    ==  9, "cactus is 9 ticks (0.45 s)");
	CHECK(blockHardnessTicks(BLOCK_DEAD_BUSH) ==  1, "dead bush is 1 tick (0.05 s)");
	CHECK(blockHardnessTicks(BLOCK_FERN)      ==  1, "fern is 1 tick (0.05 s)");

	// v1.8.10 "Light". The torch is the first light source and, like every other new core
	// block, must be breakable with its own durability — never instant (0), never unbreakable.
	// 1 is the smallest nonzero value the byte allows, the same floor dead_bush and fern above
	// already sit at: a torch is meant to come up as fast as a Minecraft one, not to be a
	// durability sink.
	CHECK(blockHardnessTicks(BLOCK_TORCH) == 1, "torch is 1 tick (0.05 s)");

	// v1.8.12 "Ores". Six more core rows, each with its own break time — a six-step ladder,
	// not a flat value, for the identical reason the cactus/snow/ice trio below is pinned as
	// three distinct numbers rather than three equal ones. No tool-tier gate in this version:
	// these are the bare-hand tick counts, and there is no faster tier yet to divide them by.
	CHECK(blockHardnessTicks(BLOCK_COAL_ORE)     ==  60, "coal ore is 60 ticks (3.00 s)");
	CHECK(blockHardnessTicks(BLOCK_IRON_ORE)     ==  70, "iron ore is 70 ticks (3.50 s)");
	CHECK(blockHardnessTicks(BLOCK_LAPIS_ORE)    ==  80, "lapis ore is 80 ticks (4.00 s)");
	CHECK(blockHardnessTicks(BLOCK_GOLD_ORE)     ==  85, "gold ore is 85 ticks (4.25 s)");
	CHECK(blockHardnessTicks(BLOCK_REDSTONE_ORE) ==  90, "redstone ore is 90 ticks (4.50 s)");
	CHECK(blockHardnessTicks(BLOCK_DIAMOND_ORE)  == 100, "diamond ore is 100 ticks (5.00 s)");

	// v1.8.14 "Animals". Four more core rows — the raw meats, ids 34..37 — each with its own
	// break time, named one per line and typed from the intent rather than read back off the
	// table, exactly as every block above. A four-step ladder rather than a flat value, for
	// the identical reason the six ores above are six distinct numbers: four equal break
	// times are four blocks a player cannot tell apart while mining. The ladder rises with
	// the size of the animal, every step clears the 1-tick floor dead bush and fern sit at,
	// and every step is far under stone's 45 — raw meat is soft, and these numbers say so.
	CHECK(blockHardnessTicks(BLOCK_RAW_CHICKEN)  ==   3, "raw chicken is 3 ticks (0.15 s)");
	CHECK(blockHardnessTicks(BLOCK_RAW_PORKCHOP) ==   4, "raw porkchop is 4 ticks (0.20 s)");
	CHECK(blockHardnessTicks(BLOCK_RAW_MUTTON)   ==   5, "raw mutton is 5 ticks (0.25 s)");
	CHECK(blockHardnessTicks(BLOCK_RAW_BEEF)     ==   6, "raw beef is 6 ticks (0.30 s)");

	// DISTINCTNESS, and this is the check the roadmap actually asked for: "every single new
	// block breakable with its own durability". The three lines above could all read 9 and
	// each `==` would still be a true statement about a table where the cactus had simply
	// borrowed its neighbours' number. Stated as three inequalities, that cannot happen
	// quietly. The cactus sits deliberately BETWEEN snow and ice rather than outside them —
	// a value picked to be far away would satisfy this while telling the player nothing.
	CHECK(blockHardnessTicks(BLOCK_CACTUS) != blockHardnessTicks(BLOCK_SNOW),
	      "the cactus does not break in the same time as snow");
	CHECK(blockHardnessTicks(BLOCK_CACTUS) != blockHardnessTicks(BLOCK_ICE),
	      "nor in the same time as ice");
	CHECK(blockHardnessTicks(BLOCK_SNOW) < blockHardnessTicks(BLOCK_CACTUS)
	      && blockHardnessTicks(BLOCK_CACTUS) < blockHardnessTicks(BLOCK_ICE),
	      "and it is ordered between them: snow < cactus < ice");

	// THE RULE, not the values: every core row a player can aim at has a break time of its
	// own. This is the check that catches the next block added without a .hardness, because
	// world/registry.c's kCoreDefs is a designated-initialiser table — a row written without
	// one gets 0 from the zero-fill, breakTicksRequired() returns 0, and the block shatters
	// on the press edge with nothing said. A liquid is exempt and only a liquid, because
	// blockIsTargetable() is false for it and no break timer can ever ask.
	//
	// world/registry_test.c's coreHardnessIsDeclared() states the identical rule over the
	// registry directly. Two suites rather than one because they fail at different moments:
	// that one goes red on the table, this one goes red on what mining reads out of it.
	int targetable_rows = 0;
	for (BlockId id = 1; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined(id)) continue;
		if (registryView(id)->liquid) continue;   // View, not Get: Get's liquid is a flags bit
		targetable_rows++;
		CHECK(blockHardnessTicks(id) != 0,
		      "every defined, non-liquid core row declares a hardness of its own");
	}
	// The loop ran, and over the whole table rather than a prefix of it. Without this a
	// `continue` that swallowed everything leaves the rule green having asserted nothing.
	//
	// 26 -> 32 on 2026-09-02/03, v1.8.12 "Ores": six more targetable, non-liquid core rows
	// (the ore blocks, ids 28..33), each declaring its own nonzero hardness — see
	// world/registry_test.c's coreHardnessIsDeclared() for the identical count, read off the
	// table directly rather than through blockHardnessTicks().
	//
	// 32 -> 36 on 2026-09-03, v1.8.14 "Animals": four more targetable, non-liquid core rows
	// (the raw meats, ids 34..37). All four are FULL_CUBE and SOLID like the ores and the
	// apple, so all four are targetable and all four enter this loop; none is a liquid, so
	// none is exempt. Same count from the other side in registry_test.c.
	CHECK(targetable_rows == 36, "and there are 36: 38 core rows less air and less water");

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

	// v1.8.8. The three cubes whose hardness only started being read once the bag would take
	// them. Three adjacent numbers on purpose, so a break time fetched from the wrong ROW —
	// an off-by-one in a lookup, a view copied from the neighbour — lands on a value that is
	// still plausible and is caught here rather than felt in play.
	CHECK(breakTicksRequired(BLOCK_SNOW,   BARE_HANDS) ==  8, "snow takes 8 ticks");
	CHECK(breakTicksRequired(BLOCK_CACTUS, BARE_HANDS) ==  9, "cactus takes 9 ticks: its own");
	CHECK(breakTicksRequired(BLOCK_ICE,    BARE_HANDS) == 10, "ice takes 10 ticks");

	// v1.8.10. hardness 1 at the 1x multiplier this game has today (testSpeedMultiplier below)
	// is ceil(1 * 256 / 256) == 1, not a placeholder copied from dead_bush/fern's line above —
	// measured through the real breakTicksRequired(), the same call every other line here goes
	// through.
	CHECK(breakTicksRequired(BLOCK_TORCH, BARE_HANDS) == 1, "torch takes 1 tick: its own");

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
