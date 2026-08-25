// Host tests for the 20 TPS simulation clock (world/tick.c, v1.8.0 task 21).
//
// WHY THIS FILE EXISTS ALONGSIDE world_test.c's testTickClock().
//
// tick.c was NOT uncovered — world_test.c:7176 testTickClock() is a real suite with real
// assertions on real return values, and it already owns: the bank/remainder contract, the 20-
// ticks-per-second rate, the ten-second drift check, the server's sleep-exactly-as-long loop,
// one catch-up clamp event and its recovery, the backwards-clock rule, the max_catchup<=0 clamp,
// NULL safety on all five clock entry points, and tickPeriodForDistSq's 575/576/577 boundary.
// None of that is repeated here. This file covers what a measurement of that suite showed it
// does not reach:
//
//   1. tickDue() AT THE PERIODS PRODUCTION ACTUALLY USES. testTickClock exercises periods 0, 1
//      and TICK_FAR_PERIOD (10) only. The dedicated server calls
//      tickDue(t, BS_POS_BROADCAST_PERIOD, 0) once per tick at
//      deps/blocksmith-server/game/bsgame.c:1306, and BS_POS_BROADCAST_PERIOD is TICK_HZ / 10 =
//      2. Period 2 is the ONLY period tickDue is called with in shipped code and it had no
//      coverage at all. It gates the 10 Hz position broadcast to every client in the room, and
//      this file is MIRRORED into that server, so the rate at which authoritative positions
//      reach clients was resting on an untested branch.
//
//   2. THE STAGGER AS A PATTERN, not as a count. testTickClock proves a decimated thing fires
//      twenty times in two hundred ticks and that ten consecutive ids partition one period. Both
//      of those are still satisfied by a tickDue whose PHASE is wrong -- `(t + id) % p == 1`
//      passes every existing check. The tables below pin WHICH ticks are due, as hand-written
//      literal bit strings, so a phase shift cannot hide inside a correct-looking total.
//
//   3. UNBOUNDED ids and LARGE tick numbers. tick.h promises `id` works "without that value
//      needing to be dense or bounded"; nothing tested an id above 12345, and nothing tested a
//      tick above 200. A truncation of either to 32 bits is invisible to the existing suite.
//
//   4. THE CONSERVATION LAW. Across a stream, count + dropped must equal the whole ticks the
//      real time fed in was worth: time is either simulated or explicitly counted as lost, never
//      quietly evaporated. testTickClock checks dropped once, after one stall. A stream with
//      repeated clamping never appears in it.
//
//   5. NAKED-LITERAL PINS FOR THE CONSTANTS. Every existing assertion about the clamp is written
//      in terms of TICK_MAX_CATCHUP_DEFAULT itself (`== TICK_MAX_CATCHUP_DEFAULT`,
//      `== 20 - TICK_MAX_CATCHUP_DEFAULT`), so that constant can be changed to anything and the
//      whole suite stays green -- measured, see this file's stanza in tools/run_host_tests.sh.
//      That is this project's recorded case-3 trap: a check parameterised by the value under
//      test cannot detect that value changing.
//
// Every expectation below is a hand-computed literal. Nothing here is derived from a constant
// tick.c reads, and nothing restates tick.c's own arithmetic back at it.
//
// The __3DS__ guard is load-bearing, not tidy -- the console Makefile globs every .c under
// source/world, and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/tick.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   %s\n", what);
	} else {
		printf("  ok     %s\n", what);
	}
}

// How many check() calls this suite makes on a healthy tree. A LITERAL on purpose.
//
// Every suite in this project used to end at "0 failed" and nothing else, which means a
// check that never RUNS is indistinguishable from a check that passes. Measured, not
// theorised: a sabotage that shortened a production-constant-bounded loop in the net suite
// took its count from 326 to 318 and the suite reported "0 failed". Eight checks were
// deleted and it called that a pass.
//
// The number below must never be computed from a production constant, a loop bound, or
// anything else the code under test can also move -- a pin that shrinks alongside the thing
// it is pinning is exactly the bug it exists to catch.
//
// Legitimately adding or removing a check means editing this by hand. The suite going red
// until you do is deliberate friction, not an accident.
#define TICK_TEST_EXPECTED_CHECKS 52

// Deliberately NOT routed through check(): this must not perturb the number it is testing,
// so it bumps g_fails only. Reporting shape is check()'s, so a failure here reads the same
// way every other failure in this file does.
static void checkCountPin(void)
{
	if (g_checks == TICK_TEST_EXPECTED_CHECKS)
		return;

	g_fails++;
	if (g_checks < TICK_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran: a loop bound shrank, an early\n"
		       "         return or a continue fired, or a check was deleted. The checks that\n"
		       "         did run passing tells you nothing about the ones that did not.\n"
		       "         Find them. Do NOT re-pin TICK_TEST_EXPECTED_CHECKS to go green.\n",
		       TICK_TEST_EXPECTED_CHECKS - g_checks,
		       TICK_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If you added them on purpose, set TICK_TEST_EXPECTED_CHECKS in\n"
		       "         source/world/tick_test.c to %d. If you did not, something is\n"
		       "         running checks more times than it should.\n",
		       g_checks - TICK_TEST_EXPECTED_CHECKS,
		       TICK_TEST_EXPECTED_CHECKS, g_checks, g_checks);
}

// ---------------------------------------------------------------- the control ---------------
//
// Named control, asserted first and expected GREEN in every sabotage arm this file's stanza in
// tools/run_host_tests.sh records. It touches tickClockInit and both counters and nothing else,
// so a break in tickDue, in the clamp accounting or in the remainder must leave it standing --
// if it ever goes red alongside everything else, the arm is too broad to have localised
// anything.
//
// A control that CANNOT fail is indistinguishable from one that did not fire, so this one was
// pointed at a deliberately wrong expectation once and read back red. That run is quoted in the
// runner stanza.
static void testControlFreshClock(void)
{
	TickClock c;
	tickClockInit(&c, 4);

	check(tickClockCount(&c) == 0 && tickClockDropped(&c) == 0 && c.accum_us == 0,
	      "control: a freshly initialised clock is at tick 0, has dropped nothing, and has "
	      "banked nothing");
}

// ------------------------------------------------- the constants, as naked literals ---------
//
// Nothing else in the tree pins TICK_MAX_CATCHUP_DEFAULT. tick.h's own _Static_asserts protect
// TICK_PERIOD_US, TICK_PERIOD_MS and TICK_FAR_PERIOD only against being INTERNALLY inconsistent
// -- TICK_HZ 20 -> 10 satisfies all three of them -- and world_test.c's tick block spells its
// clamp expectations in terms of TICK_MAX_CATCHUP_DEFAULT itself. These seven lines are the only
// place any of that has to be changed on purpose.
static void testTickConstantsPinned(void)
{
	check(TICK_HZ == 20,                    "TICK_HZ is 20 (the spec's 20 TPS)");
	check(TICK_PERIOD_US == 50000,          "TICK_PERIOD_US is 50000 (50 ms in microseconds)");
	check(TICK_PERIOD_MS == 50,             "TICK_PERIOD_MS is 50");
	check(TICK_NEAR_BLOCKS == 24,           "TICK_NEAR_BLOCKS is 24 (the spec's full-rate radius)");
	check(TICK_NEAR_DIST_SQ == 576,         "TICK_NEAR_DIST_SQ is 576 (24 squared)");
	check(TICK_FAR_PERIOD == 10,            "TICK_FAR_PERIOD is 10 (every 10th tick = 2 Hz)");
	check(TICK_MAX_CATCHUP_DEFAULT == 4,
	      "TICK_MAX_CATCHUP_DEFAULT is 4 -- the death-spiral clamp, pinned nowhere else in the "
	      "tree");
}

// ------------------------------------------------------- tickDue, as an exact pattern -------
//
// Each row says: for this period and this id, these are the ticks that are due, starting at tick
// 0, one character per tick. Hand-computed from tick.h's contract and cross-checked
// arithmetically before being written down; NOT read off tick.c and not generated by running it.
//
// Period 2 is the live one -- see this file's header. Periods 3, 4, 5 and 20 are here because
// tick.h's contract is general and the redstone/crop/smelting mechanics the header names will
// all arrive asking for periods that are neither 1 nor 10.
typedef struct {
	int         period;
	uint32_t    id;
	const char *pattern;
	const char *what;
} DuePattern;

static const DuePattern k_due_patterns[] = {
	{  2, 0u,          "10101010101010101010",
	   "tickDue period 2 id 0 fires on the even ticks (the server's 10 Hz position broadcast)" },
	{  2, 1u,          "01010101010101010101",
	   "tickDue period 2 id 1 fires on the odd ticks -- the stagger inverts it exactly" },
	{  3, 0u,          "100100100100",
	   "tickDue period 3 id 0 fires on ticks 0, 3, 6, 9" },
	{  3, 2u,          "010010010010",
	   "tickDue period 3 id 2 fires on ticks 1, 4, 7, 10" },
	{  4, 5u,          "0001000100010001",
	   "tickDue period 4 id 5 fires on ticks 3, 7, 11, 15 -- an id LARGER than the period still "
	   "staggers" },
	{  5, 3u,          "001000010000100",
	   "tickDue period 5 id 3 fires on ticks 2, 7, 12" },
	{ 10, 0u,          "10000000001000000000",
	   "tickDue period 10 id 0 fires on ticks 0 and 10" },
	{ 10, 7u,          "00010000000001000000",
	   "tickDue period 10 id 7 fires on ticks 3 and 13, NOT on 0 and 10 -- the phase, which a "
	   "firing COUNT cannot see" },
	{ 10, 4294967295u, "00000100000000010000",
	   "tickDue period 10 id UINT32_MAX fires on ticks 5 and 15 -- an unbounded id, which tick.h "
	   "promises and nothing tested" },
	{ 20, 0u,          "1000000000000000000010000000000000000000",
	   "tickDue period 20 id 0 fires on ticks 0 and 20" },
};

#define DUE_PATTERN_COUNT ((int)(sizeof k_due_patterns / sizeof k_due_patterns[0]))

static void testTickDuePatterns(void)
{
	int mixed = 0;

	for (int r = 0; r < DUE_PATTERN_COUNT; r++) {
		const DuePattern *p = &k_due_patterns[r];
		const size_t      n = strlen(p->pattern);

		size_t wrong = 0, ones = 0;
		for (size_t t = 0; t < n; t++) {
			const bool want = (p->pattern[t] == '1');
			if (want) ones++;
			if (tickDue((uint64_t)t, p->period, p->id) != want) wrong++;
		}

		// A pattern of all-1 or all-0 would be satisfied by a tickDue that ignores its
		// arguments and always answers the same way, so count the rows that genuinely ask
		// the question and assert below that every row does.
		if (ones > 0 && ones < n) mixed++;

		check(wrong == 0, p->what);
	}

	// The falsifiability counter. Without it, every row above could be a pattern that cannot
	// discriminate, and ten green checks would mean nothing. 10 is a naked literal: it is the
	// number of rows in the table, and if a row is deleted this goes red rather than shrinking
	// quietly alongside it.
	check(mixed == 10 && DUE_PATTERN_COUNT == 10,
	      "falsifiability: all 10 due-pattern rows contain BOTH a due tick and a not-due tick, "
	      "so none of them can be satisfied by a constant answer");
}

// ---------------------------------------------- tickDue far from the origin ------------------
//
// Nothing tested a tick number above 200. `tick` is a uint64_t and the modulo is done in
// uint64_t; a truncation to 32 bits anywhere on that path would still look perfect for the first
// few thousand ticks and then silently re-phase every decimated entity in the world after
// 2^32 ticks. These use 10^12, which is about 1,585 years of uptime and comfortably past any
// 32-bit boundary.
#define TICK_FAR_FROM_ORIGIN 1000000000000ull

static void testTickDueFarFromTheOrigin(void)
{
	// 10^12 is divisible by 10 and by 2, so the phases are hand-checkable by eye.
	check(tickDue(TICK_FAR_FROM_ORIGIN, 10, 0u),
	      "tickDue is due at tick 10^12 for period 10 id 0 (10^12 divides by 10)");
	check(!tickDue(TICK_FAR_FROM_ORIGIN + 1ull, 10, 0u),
	      "tickDue is NOT due one tick later -- the phase survives 10^12 ticks");
	check(!tickDue(TICK_FAR_FROM_ORIGIN, 2, 1u),
	      "tickDue period 2 id 1 is not due at tick 10^12 (10^12 + 1 is odd)");
	check(tickDue(TICK_FAR_FROM_ORIGIN + 1ull, 2, 1u),
	      "tickDue period 2 id 1 IS due at tick 10^12 + 1");

	// The stagger still partitions a period this far out: over ten consecutive ticks, ten
	// consecutive ids fire exactly once each and exactly one per tick.
	int per_tick_wrong = 0, per_id_wrong = 0;
	for (uint64_t k = 0; k < 10; k++) {
		int due_here = 0;
		for (uint32_t id = 0; id < 10u; id++)
			if (tickDue(TICK_FAR_FROM_ORIGIN + k, 10, id)) due_here++;
		if (due_here != 1) per_tick_wrong++;
	}
	for (uint32_t id = 0; id < 10u; id++) {
		int due_across = 0;
		for (uint64_t k = 0; k < 10; k++)
			if (tickDue(TICK_FAR_FROM_ORIGIN + k, 10, id)) due_across++;
		if (due_across != 1) per_id_wrong++;
	}
	check(per_tick_wrong == 0 && per_id_wrong == 0,
	      "the period-10 stagger still partitions cleanly at tick 10^12 -- one id per tick, one "
	      "tick per id");

	// A period of 1 or less is always due, including at a huge tick and including a NEGATIVE
	// period, which nothing tested. A caller computing a period arithmetically can reach one.
	check(tickDue(TICK_FAR_FROM_ORIGIN, 1, 12345u)
	      && tickDue(TICK_FAR_FROM_ORIGIN, 0, 12345u)
	      && tickDue(TICK_FAR_FROM_ORIGIN, -7, 12345u),
	      "period 1, 0 and -7 are all always due, at tick 10^12 as at tick 0");
}

// --------------------------------------------------- the clock over a real stream ------------
//
// Every number below was worked out by hand from tick.h's contract before the test was run, and
// is written as a literal. max_catchup is passed as a literal 4 rather than as
// TICK_MAX_CATCHUP_DEFAULT so that this stream keeps saying the same thing if that constant is
// ever changed -- the constant is pinned separately, above, and this is not a second copy of
// that pin.
//
// The stream is: a one-second stall, a half-second stall, one clean tick, a zero, a large
// BACKWARDS jump, and then a 74,999 / 25,001 pair that only completes if the sub-tick remainder
// is carried. Fed in: 1,650,000 us of forward time, which is 33 whole ticks. Eleven get run,
// twenty-two get dropped, and 11 + 22 is 33 -- that is the conservation law, and it is the one
// property that says the clamp LOSES time honestly instead of silently.
static void testTickClockStreamWithRepeatedClamping(void)
{
	static const int64_t fed[]  = { 1000000, 500000, 50000, 0, -999999, 74999, 25001 };
	static const int     want[] = {       4,      4,     1, 0,       0,     1,     1 };

	TickClock c;
	tickClockInit(&c, 4);

	int64_t forward_us     = 0;
	int     ret_wrong      = 0;
	int     batch_wrong    = 0;
	int64_t until_at_six   = -1;

	for (int i = 0; i < 7; i++) {
		const uint64_t before = tickClockCount(&c);
		const int      got    = tickClockAdvance(&c, fed[i]);

		if (got != want[i]) ret_wrong++;

		// The server reconstructs the first tick number of each batch as
		// tickClockCount() - n (deps/blocksmith-server/game/bsgame.c:1618). If the return
		// value and the counter ever disagree, every tick number the server stamps on a
		// broadcast is wrong, and it is wrong on the server only -- which is the shape a
		// desync takes. Checked at every step, clamped steps included.
		if (tickClockCount(&c) != before + (uint64_t)got) batch_wrong++;

		if (fed[i] > 0) forward_us += fed[i];
		if (i == 5) until_at_six = tickClockUntilNextUs(&c);
	}

	check(ret_wrong == 0,
	      "the seven advances return 4, 4, 1, 0, 0, 1, 1 -- clamped, clamped, clean, empty, "
	      "backwards, and a split tick");
	check(batch_wrong == 0,
	      "count always advances by exactly the value advance() returned, clamped steps included "
	      "(the server's batch-first-tick arithmetic)");
	check(tickClockCount(&c)   == 11u, "eleven ticks were run across the stream");
	check(tickClockDropped(&c) == 22u, "twenty-two ticks were dropped across the stream");
	check(forward_us == 1650000 && (tickClockCount(&c) + tickClockDropped(&c)) == 33u,
	      "conservation: 1,650,000 us of forward time is 33 whole ticks, and 11 run + 22 dropped "
	      "accounts for all 33");
	check(until_at_six == 25001,
	      "after banking 74,999 us the clock reports 25,001 us to go -- the 24,999 us remainder "
	      "was carried, not discarded");
	check(c.accum_us == 0 && tickClockUntilNextUs(&c) == 50000,
	      "the stream ends on an exact tick boundary with a full period banked to go");
}

// The clamp has to READ max_catchup rather than hold a constant of its own. Two identical
// one-second stalls, run at two different settings: the number of ticks that survive and the
// number dropped must both move with the setting.
static void testTickClockClampFollowsMaxCatchup(void)
{
	TickClock c;

	tickClockInit(&c, 2);
	tickClockAdvance(&c, 1000000);
	tickClockAdvance(&c, 1000000);
	check(tickClockCount(&c)   == 4u,  "max_catchup 2: two one-second stalls run 4 ticks");
	check(tickClockDropped(&c) == 36u, "max_catchup 2: two one-second stalls drop 36 ticks");

	tickClockInit(&c, 4);
	tickClockAdvance(&c, 1000000);
	tickClockAdvance(&c, 1000000);
	check(tickClockCount(&c)   == 8u,  "max_catchup 4: the same two stalls run 8 ticks");
	check(tickClockDropped(&c) == 32u, "max_catchup 4: the same two stalls drop 32 ticks");
}

// A thousand frames at the measured console frame time, with the clamp set far out of the way so
// nothing is dropped and the ONLY thing being measured is accumulated drift. 1000 x 16,714 us is
// 16,714,000 us, which is 334 whole ticks with 14,000 us left over. A clock that rounded or
// truncated per frame instead of banking would land on 333 or on 1000 here, and a clock that
// discarded the remainder would end with nothing banked.
static void testTickClockDriftOverAThousandFrames(void)
{
	TickClock c;
	tickClockInit(&c, 100);

	for (int f = 0; f < 1000; f++) tickClockAdvance(&c, 16714);

	check(tickClockCount(&c)   == 334u, "1000 frames of 16,714 us is exactly 334 ticks");
	check(tickClockDropped(&c) == 0u,   "with max_catchup 100 nothing is dropped over that run");
	check(c.accum_us == 14000,          "14,000 us of the last tick is still banked");
	check(tickClockUntilNextUs(&c) == 36000,
	      "and the clock reports 36,000 us to the next tick, which is 50,000 minus the bank");
}

// The invariants, asserted over a long deterministic stream rather than at three hand-picked
// points. The generator is a plain LCG so the stream is identical on every machine and every
// run; it produces a mix of empty advances, ordinary ones, backwards ones and stalls big enough
// to clamp.
//
// The three falsifiability counters at the end are the point. Without them a green result here
// is satisfied by a stream that never clamped, never returned zero and never went backwards --
// which is precisely the state a carelessly-tuned generator lands in.
static void testTickClockInvariantsOverAMixedStream(void)
{
	TickClock c;
	tickClockInit(&c, 4);

	uint32_t lcg           = 20250825u;
	int64_t  forward_us    = 0;
	int      steps         = 0;
	int      ret_range_bad = 0;
	int      batch_bad     = 0;
	int      accum_bad     = 0;
	int      until_bad     = 0;
	int      clamped_steps = 0;
	int      zero_steps    = 0;
	int      backward_steps= 0;

	for (int i = 0; i < 500; i++) {
		lcg = lcg * 1103515245u + 12345u;

		// -40,000 .. 359,999 us: negative often enough to exercise the backwards rule, and
		// up to seven ticks' worth, which is past a max_catchup of 4.
		const int64_t e = (int64_t)(lcg % 400000u) - 40000;

		const uint64_t before = tickClockCount(&c);
		const uint64_t drop_b = tickClockDropped(&c);
		const int      got    = tickClockAdvance(&c, e);
		steps++;

		if (got < 0 || got > 4)                                    ret_range_bad++;
		if (tickClockCount(&c) != before + (uint64_t)got)           batch_bad++;
		if (c.accum_us < 0 || c.accum_us >= 50000)                  accum_bad++;
		if (tickClockUntilNextUs(&c) != 50000 - c.accum_us)         until_bad++;

		if (tickClockDropped(&c) > drop_b) clamped_steps++;
		if (got == 0)                      zero_steps++;
		if (e <= 0)                        backward_steps++;
		if (e > 0)                         forward_us += e;
	}

	check(steps == 500,        "the mixed stream ran all 500 advances");
	check(ret_range_bad == 0,  "every advance returned between 0 and max_catchup inclusive");
	check(batch_bad == 0,      "count advanced by exactly the returned value at every one of the "
	                           "500 steps");
	check(accum_bad == 0,      "the bank stayed in [0, 50000) after every advance -- never "
	                           "negative, never a whole tick's worth left unspent");
	check(until_bad == 0,      "tickClockUntilNextUs was 50,000 minus the bank at every step");
	check((int64_t)(tickClockCount(&c) + tickClockDropped(&c)) == forward_us / 50000,
	      "conservation across 500 mixed advances: run + dropped equals the whole ticks the "
	      "forward time fed in was worth");

	check(clamped_steps > 20,
	      "falsifiability: the stream clamped on more than 20 steps, so the clamp arithmetic was "
	      "actually exercised rather than dodged");
	check(zero_steps > 20 && backward_steps > 20,
	      "falsifiability: the stream also contained more than 20 empty advances and more than 20 "
	      "backwards ones");
}

// tickPeriodForDistSq across the whole near/far transition in BLOCKS rather than at the three
// squared values world_test.c pins. The expectation is spelled with a naked 24 and a naked 10,
// so it does not move if TICK_NEAR_BLOCKS or TICK_FAR_PERIOD do.
static void testTickPeriodForDistSqSweep(void)
{
	int wrong = 0, near_seen = 0, far_seen = 0;

	for (int32_t d = 0; d <= 40; d++) {
		const int got  = tickPeriodForDistSq(d * d);
		const int want = (d <= 24) ? 1 : 10;
		if (got != want) wrong++;
		if (want == 1)  near_seen++; else far_seen++;
	}

	check(wrong == 0,
	      "tickPeriodForDistSq gives period 1 at 0..24 blocks and period 10 at 25..40, all 41 "
	      "distances");
	check(near_seen == 25 && far_seen == 16,
	      "falsifiability: the sweep straddles the boundary -- 25 near distances and 16 far ones, "
	      "so it is not sitting in a region where only one answer is possible");
	check(tickPeriodForDistSq(576) == 1,
	      "exactly 24 blocks (576 squared units) is still inside the full-rate radius");
	check(tickPeriodForDistSq(577) == 10,
	      "one squared unit further out drops to period 10");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== tick test ==");

	testControlFreshClock();
	testTickConstantsPinned();
	testTickDuePatterns();
	testTickDueFarFromTheOrigin();
	testTickClockStreamWithRepeatedClamping();
	testTickClockClampFollowsMaxCatchup();
	testTickClockDriftOverAThousandFrames();
	testTickClockInvariantsOverAMixedStream();
	testTickPeriodForDistSqSweep();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
