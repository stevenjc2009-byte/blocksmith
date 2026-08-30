// Host tests for the per-frame relight budget (world/relight_drain.c) and for the lighting
// engine's now-reported queue refusal (world/light.c), both v1.8.3.
//
// WHAT WAS BROKEN AND WHY EACH HALF IS TESTED HERE.
//
// 1. main.c drained world/relightq.h to EMPTY every frame — `while (relightqPop(...))
//    lightRelightColumn(...)` — under a comment claiming the drain was multiplayer-only and
//    bounded by 49. Both claims were false. Single player feeds it on every water movement
//    (onWorldEdit -> waterNotify -> water.c's change hook -> onWaterChange -> onRemoteEdit ->
//    relightqPush), and the bound is RELIGHTQ_CAP = 64. At the MEASURED host cost of one
//    lightRelightColumn (240 samples, gcc -O1: med 0.156 ms, max 0.287 ms) that is 9.98 to
//    18.4 ms in a 16.71 ms frame.
//
//    The loop moved into world/relight_drain.c to be testable AT ALL: source/main.c carries
//    main() and includes <3ds.h>, so it links into no host binary. world/meshq.c was split out
//    of main.c for exactly this reason and its stanza in tools/run_host_tests.sh states the rule
//    — a test links the real module, never a copy of it. Every check below calls the real
//    relightDrain(), the real relightqPop() and the real lightRelightColumn().
//
// 2. lightEngineInit ignored its malloc's result. A refused 65,544-byte edit queue left the
//    engine ENABLED and queueless, so every relight for the rest of the session ran the 26x
//    slower sweep engine with nothing recording it. light.c now sets s_edit_queue_refused and
//    counts s_sweep_fallbacks; lightFailEditQueueForTest lets that state be entered on purpose
//    here instead of only by exhausting the heap.
//
// WHAT IS DELIBERATELY NOT HERE. RELIGHT_BUDGET_MS and RELIGHT_MAX_COLUMNS themselves live in
// source/main.c and are not reachable from any host binary, so the two literals this project
// ships cannot be pinned here — only the behaviour they are fed into. That gap is stated in the
// report rather than papered over.
//
// The clock is injected (RelightClockFn) precisely so the TIME half of the budget is testable.
// A fake clock makes "stopped after two columns because the budget ran out" a deterministic
// assertion rather than a timing race, and lets testDrainRechecksBudgetEveryIteration
// distinguish a per-iteration check from a check made once at the top — which a wall clock
// never could.
//
// The __3DS__ guard is load-bearing, not tidy: the console Makefile globs every .c under
// source/world and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/light.h"
#include "world/relight_drain.h"
#include "world/relightq.h"
#include "world/world.h"

// Recompute this from the checks added, never paste it off the PASS line. A count that DROPPED
// is checks that stopped running, which a failure count of zero says nothing about.
// Counted off the source before the suite was run, in that order: 5 control + 8 refusals + 6
// count cap + 7 always-one + 7 time budget + 5 per-iteration + 8 nothing-dropped + 6 healthy
// engine + 11 refused engine + 5 degraded drain = 68.
#define RELIGHT_DRAIN_TEST_EXPECTED_CHECKS 68

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char* what)
{
	g_checks++;
	if (cond) return;
	g_fails++;
	printf("  FAIL   %s\n", what);
}

static void checkCountPin(void)
{
	if (g_checks == RELIGHT_DRAIN_TEST_EXPECTED_CHECKS) return;

	g_fails++;
	if (g_checks < RELIGHT_DRAIN_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "         RELIGHT_DRAIN_TEST_EXPECTED_CHECKS to go green.\n",
		       RELIGHT_DRAIN_TEST_EXPECTED_CHECKS - g_checks,
		       RELIGHT_DRAIN_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If deliberate, set RELIGHT_DRAIN_TEST_EXPECTED_CHECKS to %d.\n",
		       g_checks - RELIGHT_DRAIN_TEST_EXPECTED_CHECKS,
		       RELIGHT_DRAIN_TEST_EXPECTED_CHECKS, g_checks, g_checks);
}

// ── the fixture ──────────────────────────────────────────────────────────────────────────────
//
// COLUMNS columns side by side, each a floor of stone with open air above it, so a relight has
// real work to do and its result is observable: sky light reaches 15 in the open air of a column
// that has been relit and the column has no light array at all until it has been.
#define COLUMNS 10
#define FLOOR_Y 7
#define OPEN_Y  100

static World g_world;

static void worldBuild(void)
{
	worldInit(&g_world);
	for (int cx = 0; cx < COLUMNS; cx++)
		for (int lz = 0; lz < CHUNK_DIM; lz++)
			for (int lx = 0; lx < CHUNK_DIM; lx++)
				for (int y = 0; y <= FLOOR_Y; y++)
					worldSet(&g_world, cx * CHUNK_DIM + lx, y, lz, BLOCK_STONE);
}

// True when this column has been relit: the light array only exists once something attached it,
// and the open cell only reads 15 once sky light has actually been propagated down it.
static bool columnIsLit(int cx)
{
	const Column* col = worldColumn(&g_world, cx, 0);
	if (!col) return false;
	if (!lightChannelSky(col)) return false;
	return lightGetSky(col, 0, OPEN_Y, 0) == 15;
}

static int litColumns(void)
{
	int n = 0;
	for (int cx = 0; cx < COLUMNS; cx++) if (columnIsLit(cx)) n++;
	return n;
}

static void queueAll(RelightQueue* q)
{
	relightqInit(q);
	for (int cx = 0; cx < COLUMNS; cx++) relightqPush(q, cx, 0);
}

// ── clocks ───────────────────────────────────────────────────────────────────────────────────

static uint64_t g_tick;
static uint64_t g_step;
static int      g_clock_calls;

static uint64_t clockStepping(void)
{
	g_clock_calls++;
	const uint64_t v = g_tick;
	g_tick += g_step;
	return v;
}

// Frozen: the budget can never expire, so only the count cap can stop a drain.
static uint64_t clockFrozen(void)
{
	g_clock_calls++;
	return 1000;
}

// Zero, zero, then enormous for ever. A drain that re-reads the clock every iteration stops
// after the SECOND column; a drain that read it once before the loop never stops at all.
static uint64_t clockJumpsOnThirdRead(void)
{
	g_clock_calls++;
	return (g_clock_calls <= 2) ? 0 : 1000000;
}

static void clockReset(uint64_t step)
{
	g_tick = 0;
	g_step = step;
	g_clock_calls = 0;
}

// ── the control ──────────────────────────────────────────────────────────────────────────────
//
// Asserted first and expected GREEN in every sabotage arm recorded in this file's stanza in
// tools/run_host_tests.sh. It is not vacuous: it drives the SAME world builder, the SAME engine
// and the SAME observation (columnIsLit) every check below depends on, straight through
// lightRelightColumn with no drain involved. If the fixture stopped building terrain, or the
// engine stopped propagating sky light, or lightGetSky started reading the wrong nibble, this
// goes red and every result below becomes uninterpretable. It stays green under sabotage of
// relightDrain's budget because it never calls relightDrain.
static void testControlEngineAndFixture(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	check(lightEnabled(), "control: the light engine reports itself enabled");
	check(!columnIsLit(0), "control: a column nothing has relit has no light");
	check(lightRelightColumn(&g_world, 0, 0), "control: a direct relight of column 0 succeeds");
	check(columnIsLit(0), "control: column 0 now reads sky 15 in open air");
	check(!columnIsLit(1), "control: relighting column 0 did not light column 1");

	worldExit(&g_world);
}

// ── the drain: refusals ──────────────────────────────────────────────────────────────────────

static void testDrainRefusals(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	check(relightDrain(NULL, &q, 100, 1000, clockFrozen) == 0,
	      "a NULL world drains nothing");
	check(relightDrain(&g_world, NULL, 100, 1000, clockFrozen) == 0,
	      "a NULL queue drains nothing");
	check(relightDrain(&g_world, &q, 100, 1000, NULL) == 0,
	      "a NULL clock drains nothing");
	check(relightqCount(&q) == COLUMNS,
	      "a refused drain left every entry in the set");
	check(litColumns() == 0,
	      "a refused drain relit nothing");

	// The engine off is not a refusal to test lightly: this is the configuration the host
	// suite and the dedicated server both run in, and a drain that relit anyway would attach
	// 32 KiB per column to a build with lighting compiled out.
	lightEngineInit(false);
	check(relightDrain(&g_world, &q, 100, 1000, clockFrozen) == 0,
	      "the engine being off drains nothing");
	check(relightqCount(&q) == COLUMNS,
	      "the engine being off left every entry in the set");

	lightEngineInit(true);
	relightqInit(&q);
	check(relightDrain(&g_world, &q, 100, 1000, clockFrozen) == 0,
	      "an empty set drains nothing");

	worldExit(&g_world);
}

// ── the drain: the count cap ─────────────────────────────────────────────────────────────────

static void testDrainStopsAtTheCountCap(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	// The clock cannot expire here, so the count is the only thing that can stop this.
	check(relightDrain(&g_world, &q, 3, 1000, clockFrozen) == 3,
	      "a cap of 3 over 10 queued columns relights exactly 3");
	check(relightqCount(&q) == COLUMNS - 3,
	      "the 7 it did not reach are still queued");
	check(litColumns() == 3,
	      "exactly 3 columns actually have light: the cap defers, it does not pretend");

	check(relightDrain(&g_world, &q, 3, 1000, clockFrozen) == 3,
	      "the next frame takes the next 3");
	check(relightqCount(&q) == COLUMNS - 6, "4 left after two frames");
	check(litColumns() == 6, "6 columns lit after two frames");

	worldExit(&g_world);
}

// ── the drain: one column always completes ───────────────────────────────────────────────────
//
// The anti-starvation property, and the reason both limits are examined AFTER a relight rather
// than before it. A drain that can be talked out of doing anything never drains, and the columns
// in it stay lit for a world that no longer exists for ever.
static void testDrainAlwaysCompletesOneColumn(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	check(relightDrain(&g_world, &q, 0, 0, clockFrozen) == 1,
	      "a cap of 0 and a budget of 0 still relight one column");
	check(relightqCount(&q) == COLUMNS - 1, "and take exactly one entry out of the set");
	check(litColumns() == 1, "and that column really is lit");

	check(relightDrain(&g_world, &q, -5, 0, clockFrozen) == 1,
	      "a negative cap still relights one column");
	check(relightqCount(&q) == COLUMNS - 2, "and takes exactly one more entry");

	check(relightDrain(&g_world, &q, 100, 0, clockFrozen) == 1,
	      "a zero budget with a large cap still relights one column, and only one");
	check(relightqCount(&q) == COLUMNS - 3, "and takes exactly one more entry");

	worldExit(&g_world);
}

// ── the drain: the time budget ───────────────────────────────────────────────────────────────

static void testDrainStopsAtTheTimeBudget(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);

	// One tick per read. The first read is t0, so elapsed after column k is exactly k.
	clockReset(1);
	check(relightDrain(&g_world, &q, 100, 4, clockStepping) == 4,
	      "a 4-tick budget over a 1-tick-per-column clock relights 4 columns");
	check(relightqCount(&q) == COLUMNS - 4, "the other 6 are still queued");
	check(litColumns() == 4, "4 columns actually lit");

	clockReset(2);
	check(relightDrain(&g_world, &q, 100, 5, clockStepping) == 3,
	      "at 2 ticks per column a 5-tick budget stops after 3 (6 >= 5), not after 2");
	check(relightqCount(&q) == COLUMNS - 7, "3 left");

	// The cap must still win when it is the tighter of the two.
	clockReset(1);
	check(relightDrain(&g_world, &q, 2, 1000000, clockStepping) == 2,
	      "with time to spare the cap is what stops the drain");
	check(relightqCount(&q) == 1, "1 left");

	worldExit(&g_world);
}

// ── the drain: the budget is re-read every iteration ─────────────────────────────────────────
//
// The audit's specific point, and the one check here that a wall clock could not make. The clock
// reads 0 for its first two reads and then jumps far past the budget. A drain that re-reads it
// per iteration stops after the second column; a drain that read it once before the loop, or
// once after the loop, drains all ten.
static void testDrainRechecksBudgetEveryIteration(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	const int drained = relightDrain(&g_world, &q, 100, 10, clockJumpsOnThirdRead);
	check(drained == 2,
	      "a clock that jumps past the budget on its third read stops the drain at 2 columns");
	check(drained != COLUMNS,
	      "it did NOT drain all 10, which is what a budget checked once would have done");
	check(relightqCount(&q) == COLUMNS - 2, "8 columns still queued");
	check(litColumns() == 2, "2 columns lit");
	check(g_clock_calls == 3,
	      "the clock was read once for t0 and once per completed column, 3 reads for 2 columns");

	worldExit(&g_world);
}

// ── the drain: nothing is dropped ────────────────────────────────────────────────────────────
//
// The property the budget must not cost. Deferring a relight is fine because the column comes
// back; DROPPING one leaves the world visibly wrong with nothing anywhere to trigger a repair.
// Driven the way the main loop drives it — the same set, frame after frame, until it empties —
// and checked on the LIGHT rather than on the return values, so a drain that returned plausible
// counts while losing columns still goes red.
static void testNothingIsDroppedAcrossFrames(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	check(litColumns() == 0, "nothing lit before the first frame");

	int frames = 0, total = 0;
	while (relightqCount(&q) > 0 && frames < 100) {
		total += relightDrain(&g_world, &q, 3, 1000, clockFrozen);
		frames++;
	}

	check(frames == 4, "10 columns at 3 per frame take 4 frames");
	check(total == COLUMNS, "every queued column was relit exactly once, none dropped");
	check(relightqCount(&q) == 0, "the set is empty");
	check(litColumns() == COLUMNS, "all 10 columns really do have light");

	// Named individually so a red arm says WHICH column was lost rather than only how many.
	check(columnIsLit(0), "column 0 survived the budgeted drain");
	check(columnIsLit(4), "column 4 survived the budgeted drain");
	check(columnIsLit(9), "column 9, the last one, survived the budgeted drain");

	worldExit(&g_world);
}

// ── light.c: the refused edit queue is now reported ──────────────────────────────────────────

static void testFastEngineReadyOnTheHealthyPath(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldBuild();

	check(lightEnabled(), "engine enabled");
	check(lightFastEngineReady(), "the flood-fill engine reports itself ready");

	lightResetSweepFallbacksForTest();
	check(lightRelightColumn(&g_world, 0, 0), "a relight succeeds");
	check(lightRelightColumn(&g_world, 1, 0), "a second relight succeeds");
	check(lightSweepFallbacks() == 0,
	      "no relight on the healthy path fell back to the sweeps");

	lightEngineInit(false);
	check(!lightFastEngineReady(),
	      "with the engine off the fast engine is not ready either");

	worldExit(&g_world);
}

static void testRefusedEditQueueIsVisibleAndCounted(void)
{
	// lightEngineInit(false) FIRST: the hook makes the next init skip the malloc, and an
	// edit queue already held would otherwise survive and the hook would appear to do nothing.
	lightEngineInit(false);
	lightFailEditQueueForTest(true);
	lightEngineInit(true);
	worldBuild();
	lightResetSweepFallbacksForTest();

	check(lightEnabled(),
	      "a refused queue does NOT disable lighting - the engine is still on");
	check(!lightFastEngineReady(),
	      "and it says so: the fast engine is not ready");
	check(lightSweepFallbacks() == 0,
	      "the refusal has cost nothing yet, before any relight has run");

	check(lightRelightColumn(&g_world, 0, 0),
	      "a relight still succeeds through the sweep fallback");
	check(lightSweepFallbacks() == 1, "and is counted, once");
	check(columnIsLit(0),
	      "the sweeps produced real light: the fallback is slow, not broken");

	check(lightRelightColumn(&g_world, 1, 0), "a second fallback relight succeeds");
	check(lightSweepFallbacks() == 2, "the counter is per relight, not per session");

	// Recovery: taking the queue again clears both, so the state is not a one-way latch that
	// would keep reporting a refusal a later init had already fixed.
	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	check(lightFastEngineReady(), "a later successful init reports ready again");

	lightResetSweepFallbacksForTest();
	check(lightRelightColumn(&g_world, 2, 0), "a relight after recovery succeeds");
	check(lightSweepFallbacks() == 0, "and does not touch the fallback counter");

	worldExit(&g_world);
}

// The two halves together: a budgeted drain running in the degraded state. This is the frame the
// silent fallback was actually dangerous on — every column the drain relights costs the sweep
// price instead of the flood-fill one — and the counter is what would make that visible.
static void testDrainInTheDegradedStateIsCounted(void)
{
	lightEngineInit(false);
	lightFailEditQueueForTest(true);
	lightEngineInit(true);
	worldBuild();
	lightResetSweepFallbacksForTest();

	RelightQueue q;
	queueAll(&q);
	clockReset(0);

	check(!lightFastEngineReady(), "degraded before the drain");
	check(relightDrain(&g_world, &q, 3, 1000, clockFrozen) == 3,
	      "the budget is enforced in the degraded state too");
	check(lightSweepFallbacks() == 3,
	      "and every one of the 3 relights is counted as a sweep fallback");
	check(litColumns() == 3, "3 columns lit by the sweeps");
	check(relightqCount(&q) == COLUMNS - 3, "7 still queued, nothing dropped");

	lightEngineInit(false);
	lightFailEditQueueForTest(false);
	lightEngineInit(true);
	worldExit(&g_world);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== relight drain test ==");

	testControlEngineAndFixture();
	testDrainRefusals();
	testDrainStopsAtTheCountCap();
	testDrainAlwaysCompletesOneColumn();
	testDrainStopsAtTheTimeBudget();
	testDrainRechecksBudgetEveryIteration();
	testNothingIsDroppedAcrossFrames();
	testFastEngineReadyOnTheHealthyPath();
	testRefusedEditQueueIsVisibleAndCounted();
	testDrainInTheDegradedStateIsCounted();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
