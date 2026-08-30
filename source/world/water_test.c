// Host tests for the water simulation (v1.8.0 task 22).
//
// Own main(), like every other suite binary under source/world: water is queue-driven plain C
// with no <3ds.h>, so every claim water.h makes about it is settleable here in milliseconds,
// and a broken fluid must not stop the world suite from running.
//
// What each probe guards, and why it is the check that would go red:
//   testFlatPour        - the exact shape of a pour: a manhattan ball of radius 7 with level
//                         8-d at distance d, 112 flow cells, and nothing at distance 8. An
//                         off-by-one in the "level - 1" step moves that boundary.
//   testWaterfall       - a source in mid-air makes a ONE BLOCK wide fall and a pool where it
//                         lands. Before water.c's two feed exemptions existed this produced a
//                         solid 15-wide block of water as tall as the drop, and the check that
//                         catches it is the count.
//   testDrainage        - remove the source and every flow cell goes, in bounded time.
//   testOrderIndependent- the same pour settled at four different per-tick budgets, and two
//                         sources disturbed in both orders, all hash byte-identically.
//   testSpreadRate      - v1.8.2. WHEN, not where: the pour walks out one block every
//                         WATER_SPREAD_TICKS ticks instead of arriving all at once, and the
//                         ticks it spends waiting are counted in gen_stalls rather than being
//                         a silent throttle.
//   testSpreadRateIsTicksNotBudget
//                       - the two things WATER_TICK_BUDGET used to conflate are apart: the
//                         budget can add ticks to a spread and can never remove any.
//   testSettledIsFree   - a settled body examines zero cells per tick, for ever. This is the
//                         property the whole design exists for on a 268 MHz ARM11.
//   testChangeHook      - on_change fires once per BLOCK change and never for a level-only
//                         change inside water that was already there.
//   testEditHook        - world.c's worldSetEditHook fires on a real change, not on a no-op
//                         write, and not from worldSetChunkAll.
//   testEditHookReportsWorld
//                       - v1.8.3. WHICH world it fired for. One hook, two Worlds, and the
//                         callback must name the right one so a consumer can drop the other.
//                         Nothing checked this before, which is how app/worker.c's staging
//                         world spent a release feeding the live water simulation.
//   testDropColumn      - unloading a column takes its flow water and leaves its sources.
//   testSaveCompat      - a v1.7.1-shaped ocean column encodes to the same bytes before and
//                         after the simulation has run, and a flooded column encodes back to
//                         its original bytes once waterDropColumn has had it.
//   testQueueOverflow   - a refused candidate is COUNTED, not silent, and the world converges
//                         to the reference once the cells are offered again.
//   testNoOrphanedDedupKeys
//                       - v1.8.3. The ring and the dedup set still agree. The only probe here
//                         that reads the two tables against each other rather than asking what
//                         the water did, because a key left in the set with no ring entry makes
//                         qPush refuse that cell FOR EVER while every accessor and every world
//                         hash still reads healthy. Its header carries the sabotage that proves
//                         no other check in this file can see it.
//   testTickCost        - real microseconds per tick, settled and active arms interleaved.
//   testSunkenBasin,
//   testPadAndShaft     - the two fixture experiments. v1.8.3: both build their terrain and
//                         place their source with world.h's edit hook LIVE (resetWorldWired),
//                         because that is what the game does and the results have to transfer.
//                         See wiredEditHook for what these two were measuring before that.
//
// The __3DS__ guard is load-bearing, not tidy: the console Makefile globs every .c under
// source/world and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/chunk_codec.h"
#include "world/water.h"
#include "world/world.h"

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

// ── Fixtures ─────────────────────────────────────────────────────────────────────────

static World    g_world;
static WaterSim g_sim;

static void resetWorld(void)
{
	worldExit(&g_world);
	budgetReset();
	worldInit(&g_world);
	waterInit(&g_sim);
	worldSetEditHook(NULL, NULL);
}

// v1.8.3. The GAME's wiring, in one function, and it is a copy of source/main.c's onWorldEdit
// (main.c:2837, installed once at main.c:3199) down to the discarded arguments: every
// single-block change on the console reaches the water simulation through world.h's edit hook —
// world.c:278, `if (s_edit_fn && prev != id)` — and no edit path anywhere in the game calls
// waterNotify by hand.
//
// Why this exists rather than a hand call: a fixture that builds with the hook NULL and then
// calls waterNotify once is running a DIFFERENT EXPERIMENT from the game, and the difference is
// not cosmetic. buildPad() is 3829 worldSet calls; with the hook dead not one of them queues
// anything, so `waterQueueFull(&g_sim) == 0` afterwards was true by construction and could not
// go red. Measured on 2026-08-25 by instrumenting qPush: with the hook dead the ring never gets
// deeper than 210 of WATERQ_CAP's 1024 during the pad pour, and 50 and 29 during the two basin
// pours. So halving the ring to 512 inside qPush — a real overrun, and that same build's own
// testQueueOverflow reported 1595 candidates genuinely refused — still left this whole file at
// "PASS 149 checks, 0 failed", pad arm included. That is the defect this hook fixes.
//
// v1.8.3. `w` is discarded HERE and only here, and that is not a copy of main.c: main.c compares
// it against s_world because a second World exists there (app/worker.c's staging world). This
// fixture drives exactly one world, g_world, so there is nothing to filter out and a filter would
// be dead code. testEditHookReportsWorld() below is where the parameter is actually exercised.
static void wiredEditHook(void* ud, const World* w, int x, int y, int z, BlockId prev, BlockId now)
{
	(void)ud; (void)w; (void)prev; (void)now;
	waterNotify(&g_sim, x, y, z);
}

// resetWorld() clears the hook and MUST keep clearing it. world.c's s_edit_fn (world.c:217) is a
// file static that outlives every World — worldInit/worldExit never touch it — so a hook left
// armed by the previous probe would fire during the next one's fixture build, against a counter
// or a sim that probe never meant to disturb; and testEditHook's last check, "a NULL hook is
// genuinely uninstalled", has nothing to assert if the reset does not put it back to NULL. So
// the fix for the fidelity defect is to RE-ARM after the reset, never to stop clearing.
static void resetWorldWired(void)
{
	resetWorld();
	worldSetEditHook(wiredEditHook, NULL);
}

static void fillBox(int x0, int x1, int y0, int y1, int z0, int z1, BlockId id)
{
	for (int y = y0; y <= y1; y++)
		for (int z = z0; z <= z1; z++)
			for (int x = x0; x <= x1; x++)
				(void)worldSet(&g_world, x, y, z, id);
}

static int countBlocks(int x0, int x1, int y0, int y1, int z0, int z1, BlockId id)
{
	int n = 0;
	for (int y = y0; y <= y1; y++)
		for (int z = z0; z <= z1; z++)
			for (int x = x0; x <= x1; x++)
				if (worldGet(&g_world, x, y, z) == id) n++;
	return n;
}

// FNV-1a over every block id AND every flow level in the box. Both halves matter: two runs
// that agree on the blocks but disagree on the levels are not the same state, because the
// levels decide what happens on the next disturbance.
static uint64_t hashBox(int x0, int x1, int y0, int y1, int z0, int z1)
{
	uint64_t h = 1469598103934665603ULL;
	for (int y = y0; y <= y1; y++)
		for (int z = z0; z <= z1; z++)
			for (int x = x0; x <= x1; x++) {
				const uint8_t b = (uint8_t)worldGet(&g_world, x, y, z);
				const uint8_t l = waterLevelAt(&g_sim, &g_world, x, y, z);
				h = (h ^ b) * 1099511628211ULL;
				h = (h ^ l) * 1099511628211ULL;
			}
	return h;
}

// A 27x27 stone floor at y = 63, air above it. Big enough that a pour of radius 7 from the
// origin is over solid ground the whole way out to distance 8, so "dry at distance 8" means
// the rule stopped it and not that it ran out of floor.
static void buildFloor(void)
{
	fillBox(-13, 13, 60, 63, -13, 13, BLOCK_STONE);
}

// v1.8.3. Called immediately after a fixture has been laid down with resetWorldWired()'s hook
// live, and it does three things that a fixture built with the hook NULL never had to do.
//
// It MEASURES the build burst and pins it as bare literals, because with the hook live a fixture
// is no longer free: every cell of it fires world.c's edit hook, so buildFloor's 2916 stone cells
// offer candidates to a ring of 1024 with no tick in between to drain it. A build that quietly
// starts saturating the ring is exactly how the outcome checks below stop meaning what they say —
// see wiredEditHook's header for the version of that defect this file has already shipped once.
//
// It CHECKS that the build left no water behind, which is what makes "the pour" below the only
// thing the numbers after it describe.
//
// And it DRAINS, so the source placement that follows starts from an empty ring, exactly as it
// would on a console where 20 ticks a second run while the player is building. Without the drain
// every probe below would be measuring a pour that started life competing with its own fixture.
//
// `want_lost` and `want_pending` are literals on purpose and are edited by hand when they move.
// See main()'s check count for why this file states its numbers twice rather than computing them.
static void wiredBuildDone(const char* what, uint32_t want_lost, int want_pending)
{
	const uint32_t lost    = waterQueueFull(&g_sim);
	const int      pending = waterPending(&g_sim);

	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	const int flow = waterFlowCells(&g_sim);

	printf("         %s build: %u lost, %d pending of %d, then drained to %d pending / %d flow\n",
	       what, lost, pending, (int)WATERQ_CAP, waterPending(&g_sim), flow);

	CHECK(lost == want_lost, "the fixture build loses exactly the candidates it is pinned to lose");
	CHECK(pending == want_pending, "and leaves the ring at exactly the depth it is pinned to");
	CHECK(flow == 0, "control: the fixture build left no water behind, only stone and air");
}

static int manhattan(int x, int z)
{
	return (x < 0 ? -x : x) + (z < 0 ? -z : z);
}

// ── Probes ───────────────────────────────────────────────────────────────────────────

static void testFlatPour(void)
{
	puts("water: a source on flat ground spreads to exactly radius 7");

	// v1.8.3: the game's wiring, not a hand call. See wiredEditHook and wiredBuildDone.
	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	const uint32_t lost_base = waterQueueFull(&g_sim);

	CHECK(worldSet(&g_world, 0, 64, 0, BLOCK_WATER), "the source block is placed");
	CHECK(waterLevelAt(&g_sim, &g_world, 0, 64, 0) == WATER_LEVEL_SOURCE,
	      "water with no map entry reads as a source");

	// No hand call: the edit hook is what queued it, exactly as on the console. This goes red
	// BEFORE any outcome check does if anyone ever unwires the hook again.
	CHECK(waterPending(&g_sim) == 7,
	      "the edit hook alone queued the source and its six neighbours");

	const int ticks = waterSettle(&g_sim, &g_world, 2000, NULL, NULL);
	printf("         settled in %d ticks, %u examined, %u changed\n",
	       ticks, waterExamined(&g_sim), waterChanged(&g_sim));

	CHECK(ticks >= 0, "the pour reaches a stable state inside 2000 ticks");
	CHECK(ticks > 0 && ticks < 200, "and does it in well under a hundred ticks");
	CHECK(waterPending(&g_sim) == 0, "nothing is left queued once it has settled");

	// 4d cells at manhattan distance d, summed over d = 1..7.
	CHECK(waterFlowCells(&g_sim) == 112, "exactly 112 flow cells (4d summed over d = 1..7)");
	CHECK(countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER) == 113,
	      "113 water blocks in the world: the source and its 112 flow cells");

	bool levels_ok = true, edge_dry = true, no_second_layer = true;
	for (int z = -13; z <= 13; z++)
		for (int x = -13; x <= 13; x++) {
			const int d = manhattan(x, z);
			const uint8_t got = waterLevelAt(&g_sim, &g_world, x, 64, z);
			if (d <= 7) {
				if (got != (uint8_t)(8 - d)) levels_ok = false;
			} else if (d == 8) {
				if (got != 0) edge_dry = false;
			}
			if (waterLevelAt(&g_sim, &g_world, x, 65, z) != 0) no_second_layer = false;
		}

	CHECK(levels_ok, "every cell at distance d holds level 8 - d");
	CHECK(edge_dry, "every cell at distance 8 is dry");
	CHECK(no_second_layer, "nothing climbed to y = 65: water never flows upwards");
	// A DELTA against the base taken after the fixture build, not against zero. q_full is
	// cumulative for the life of the sim and this assertion has always been about the POUR;
	// written as a delta it keeps meaning that while the build is judged separately by
	// wiredBuildDone. Written against zero, with the hook live, it would be an assertion about
	// the fixture — and with the hook NULL, which is how it used to be built, it was true by
	// construction and could not go red at all.
	CHECK(waterMapFull(&g_sim) == 0 && waterQueueFull(&g_sim) == lost_base,
	      "the pour fits in the map and the queue with nothing lost");
}

static void testWaterfall(void)
{
	puts("water: a source in mid-air falls one block wide and pools where it lands");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);

	(void)worldSet(&g_world, 0, 80, 0, BLOCK_WATER);
	CHECK(waterPending(&g_sim) == 7,
	      "the edit hook alone queued the source and its six neighbours");
	const int ticks = waterSettle(&g_sim, &g_world, 5000, NULL, NULL);
	printf("         settled in %d ticks, %d flow cells\n", ticks, waterFlowCells(&g_sim));

	CHECK(ticks >= 0, "the fall reaches a stable state inside 5000 ticks");

	// 16 cells of column (y = 64..79) plus a pool of 4d summed over d = 1..6 around the
	// landing cell. The landing cell is level 7 because it is fed from above, so the pool
	// reaches distance 6 and not 7.
	CHECK(waterFlowCells(&g_sim) == 100, "100 flow cells: 16 of column and 84 of pool");
	CHECK(countBlocks(-13, 13, 60, 90, -13, 13, BLOCK_WATER) == 101,
	      "101 water blocks in total, the source included");

	bool column_ok = true;
	for (int y = 64; y <= 79; y++)
		if (waterLevelAt(&g_sim, &g_world, y == 64 ? 0 : 0, y, 0) != WATER_LEVEL_MAX)
			column_ok = false;
	CHECK(column_ok, "every cell of the fall is level 7: a fall does not weaken");

	// The check that would have caught the first draft. Before water.c's feed exemptions,
	// every cell of the column also spread at its own height, and this counted 13.
	int width_at_72 = 0;
	for (int z = -13; z <= 13; z++)
		for (int x = -13; x <= 13; x++)
			if (worldGet(&g_world, x, 72, z) == BLOCK_WATER) width_at_72++;
	CHECK(width_at_72 == 1, "the fall is exactly one block wide halfway down");

	bool pool_ok = true;
	for (int z = -13; z <= 13; z++)
		for (int x = -13; x <= 13; x++) {
			const int d = manhattan(x, z);
			const uint8_t got = waterLevelAt(&g_sim, &g_world, x, 64, z);
			const uint8_t want = (d <= 6) ? (uint8_t)(7 - d) : 0;
			if (got != want) pool_ok = false;
		}
	CHECK(pool_ok, "the pool is a radius-6 ball of level 7-d around the landing cell");
}

static void testDrainage(void)
{
	puts("water: removing the source drains every flow cell");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	CHECK(waterSettle(&g_sim, &g_world, 2000, NULL, NULL) >= 0, "the pour settles first");
	CHECK(waterFlowCells(&g_sim) == 112, "112 flow cells are there to drain");

	// The BREAK goes through worldSet too, which is what scene/interact.c:106 does. Nothing in
	// this probe calls waterNotify by hand any more.
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_AIR);
	CHECK(waterPending(&g_sim) == 7, "the edit hook alone queued the broken cell and its six neighbours");
	const int ticks = waterSettle(&g_sim, &g_world, 2000, NULL, NULL);
	printf("         drained in %d ticks\n", ticks);

	CHECK(ticks >= 0, "the drain reaches a stable state inside 2000 ticks");
	CHECK(waterFlowCells(&g_sim) == 0, "no flow cell is left in the map");
	CHECK(countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER) == 0,
	      "no water block is left in the world");
	CHECK(worldGet(&g_world, 0, 64, 0) == BLOCK_AIR, "including where the source was");
}

// Settles the flat pour with a given per-tick budget and returns the hash of the result.
static uint64_t pourWithBudget(int budget)
{
	resetWorld();
	buildFloor();
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);

	for (int t = 0; t < 100000 && waterPending(&g_sim) > 0; t++)
		(void)waterTick(&g_sim, &g_world, budget, NULL, NULL);

	return hashBox(-13, 13, 60, 70, -13, 13);
}

// Two sources far enough apart that their pours overlap, disturbed in the given order.
static uint64_t twoSources(bool a_first)
{
	resetWorld();
	buildFloor();
	(void)worldSet(&g_world, -4, 64, -2, BLOCK_WATER);
	(void)worldSet(&g_world,  5, 64,  3, BLOCK_WATER);

	if (a_first) {
		waterNotify(&g_sim, -4, 64, -2);
		waterNotify(&g_sim,  5, 64,  3);
	} else {
		waterNotify(&g_sim,  5, 64,  3);
		waterNotify(&g_sim, -4, 64, -2);
	}
	(void)waterSettle(&g_sim, &g_world, 5000, NULL, NULL);
	return hashBox(-13, 13, 60, 70, -13, 13);
}

static void testOrderIndependent(void)
{
	puts("water: the settled world does not depend on the drain order");

	const uint64_t h1    = pourWithBudget(1);
	const uint64_t h7    = pourWithBudget(7);
	const uint64_t h64   = pourWithBudget(WATER_TICK_BUDGET);
	const uint64_t hhuge = pourWithBudget(100000);
	printf("         budget 1 / 7 / 64 / all: %016llx %016llx %016llx %016llx\n",
	       (unsigned long long)h1, (unsigned long long)h7,
	       (unsigned long long)h64, (unsigned long long)hhuge);

	CHECK(h1 == h7 && h7 == h64 && h64 == hhuge,
	      "one cell per tick and the whole pour in one tick settle to identical worlds");

	// A control: the hash must be capable of telling two states apart, or agreeing proves
	// nothing. One extra block of stone has to move it.
	(void)worldSet(&g_world, 3, 64, 3, BLOCK_STONE);
	CHECK(hashBox(-13, 13, 60, 70, -13, 13) != hhuge,
	      "control: the hash changes when one block changes");

	const uint64_t ha = twoSources(true);
	const uint64_t hb = twoSources(false);
	printf("         two sources, order A / order B: %016llx %016llx\n",
	       (unsigned long long)ha, (unsigned long long)hb);
	CHECK(ha == hb, "two overlapping pours settle identically whichever is disturbed first");
	CHECK(ha != hhuge, "control: two sources do not settle to the same world as one");
}

// ── The spread RATE ──────────────────────────────────────────────────────────────────
//
// v1.8.2. Everything above this line is about WHERE the water ends up; these two probes are the
// only ones about WHEN, and they exist because the answer used to be "all of it, more or less at
// once". water.c drained 64 candidates a tick with nothing separating one propagation hop from
// the next, so the 225 examinations of a seven-block pour fitted in four ticks — 200 ms, with
// the first three blocks inside the first 50 ms.
//
// The claim under test is the one WATER_SPREAD_TICKS makes: the ring at manhattan distance d
// goes wet on tick (d-1) * WATER_SPREAD_TICKS and not before, so the pour walks outwards one
// block every five ticks and takes 35 ticks to reach its full reach of seven.
//
// Measured in TICKS and not in milliseconds on purpose. waterTick is only ever called from
// main.c's `ticks_now` loop off world/tick.h's 20 TPS clock, so a tick count is a wall-clock
// duration on the console and a host measurement of seconds here would be a measurement of this
// machine.

// Every cell of the manhattan ring at distance d, in the pour plane, holds water.
static bool ringWet(int d)
{
	for (int z = -d; z <= d; z++) {
		const int x = d - (z < 0 ? -z : z);
		if (waterLevelAt(&g_sim, &g_world, x, 64, z) == 0) return false;
		if (x != 0 && waterLevelAt(&g_sim, &g_world, -x, 64, z) == 0) return false;
	}
	return true;
}

typedef struct {
	int      ticks;          // ticks run before the ring emptied, or `cap`
	int      first_wet[9];   // ticks run when the ring at distance d first went fully wet; 0 = never
	uint32_t stalls;         // waterGenStalls over the run
	uint32_t examined;
} SpreadRun;

// One flat pour, ticked by hand at `budget` so the tick count is the measurement rather than
// something waterSettle decided.
static SpreadRun spreadRun(int budget, int cap)
{
	SpreadRun r;
	memset(&r, 0, sizeof r);

	resetWorld();
	buildFloor();
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);

	for (int t = 0; t < cap; t++) {
		if (waterPending(&g_sim) == 0) break;
		(void)waterTick(&g_sim, &g_world, budget, NULL, NULL);
		r.ticks = t + 1;
		for (int d = 1; d <= 8; d++)
			if (r.first_wet[d] == 0 && ringWet(d)) r.first_wet[d] = t + 1;
	}

	r.stalls   = waterGenStalls(&g_sim);
	r.examined = waterExamined(&g_sim);
	return r;
}

static void printSpreadRun(const char* label, const SpreadRun* r)
{
	printf("         %-14s settled %4d ticks, %5u examined, %5u stalls, first wet at d ="
	       " 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d 8:%d\n",
	       label, r->ticks, r->examined, r->stalls,
	       r->first_wet[1], r->first_wet[2], r->first_wet[3], r->first_wet[4],
	       r->first_wet[5], r->first_wet[6], r->first_wet[7], r->first_wet[8]);
}

static void testSpreadRate(void)
{
	puts("water: a pour walks outwards one block per WATER_SPREAD_TICKS ticks");

	// The law, measured at a budget wide enough that no generation is ever cut in half by it.
	// This arm is the barrier on its own, with the CPU cap taken out of the picture.
	const SpreadRun wide = spreadRun(512, 4000);
	printSpreadRun("budget 512", &wide);

	// Controls first. Neither reads the barrier at all, so both stay green in every sabotage
	// arm below — which is what makes the red ones evidence and not noise.
	CHECK(waterFlowCells(&g_sim) == 112,
	      "control: the settled pour is still 112 flow cells, whatever the rate was");
	CHECK(worldGet(&g_world, 3, 63, 3) == (BlockId)BLOCK_STONE,
	      "control: the floor under the pour is stone");

	// The constant itself, pinned as a literal, and every claim below it stated BOTH ways.
	// Found by sabotage, which is the only way it would have been found: with the checks
	// written only as `first_wet[d] == (d-1) * WATER_SPREAD_TICKS + 1`, setting
	// WATER_SPREAD_TICKS to 1 moved the code and the assertion together and the whole probe
	// stayed green over a fluid that had gone back to spreading a block a tick. The symbolic
	// form still earns its place — it is what goes red if the barrier logic breaks while the
	// constant is untouched — but on its own it cannot see the constant move.
	CHECK(WATER_SPREAD_TICKS == 5,
	      "the shipped rate is one block per 5 ticks: 250 ms, Beta 1.7.3's Overworld water");

	bool rate_ok = true;
	for (int d = 1; d <= 7; d++)
		if (wide.first_wet[d] != (d - 1) * WATER_SPREAD_TICKS + 1) rate_ok = false;
	CHECK(rate_ok, "distance d goes wet on tick (d-1) * WATER_SPREAD_TICKS and not one tick sooner");

	// Spelled out rather than left to the loop, because these are the numbers water.h quotes
	// and the ones a wrong WATER_SPREAD_TICKS moves first and furthest.
	CHECK(wide.first_wet[1] == 1, "distance 1 is wet after the very first tick");
	CHECK(wide.first_wet[2] == 1 + WATER_SPREAD_TICKS,
	      "distance 2 is still dry five ticks in and goes wet on the sixth");
	CHECK(wide.first_wet[7] == 1 + 6 * WATER_SPREAD_TICKS,
	      "distance 7 takes 31 ticks: a full spread is 1.55 s at 20 TPS, not 200 ms");
	CHECK(wide.first_wet[8] == 0,
	      "distance 8 is never wet: the reach is still seven blocks, the barrier did not widen it");

	// The same law again as bare numbers, so that a change to WATER_SPREAD_TICKS has to come
	// through here and be re-pinned deliberately instead of dragging the assertions with it.
	CHECK(wide.first_wet[2] == 6 && wide.first_wet[7] == 31 && wide.ticks == 36,
	      "in numbers: distance 2 on tick 6, distance 7 on tick 31, settled on tick 36");

	// And the arm that is what the console actually runs. It is NOT the clean law above, and
	// the difference is worth pinning rather than rounding off: the generations of a full-reach
	// pour grow with the frontier, and from distance 5 outwards they are wider than
	// WATER_TICK_BUDGET, so each of those three takes two ticks to drain instead of one. The
	// pour reaches distance 7 on tick 34, not 31.
	//
	// That is the CPU cap doing its job, not the barrier failing. It can only ever make the
	// spread slower — see testSpreadRateIsTicksNotBudget — and 34 ticks is 1.70 s against the
	// 1.55 s of the law and the 0.20 s this all replaced. These numbers are MEASURED off a run
	// and re-pinned by measurement if the budget or the rule ever moves; do not compute them.
	const SpreadRun real = spreadRun(WATER_TICK_BUDGET, 4000);
	printSpreadRun("budget 64", &real);

	CHECK(real.first_wet[1] == 1 && real.first_wet[2] == 6 && real.first_wet[3] == 11 &&
	      real.first_wet[4] == 16 && real.first_wet[5] == 22 && real.first_wet[6] == 28 &&
	      real.first_wet[7] == 34,
	      "at the shipping budget of 64 the pour goes wet at ticks 1, 6, 11, 16, 22, 28, 34");
	CHECK(real.ticks == 41, "and the whole thing settles in 41 ticks rather than the old 9");

	// The barrier really is what is holding it, and it says so rather than throttling silently.
	CHECK(real.stalls > 0, "the held ticks are counted in gen_stalls rather than being invisible");
	CHECK(real.stalls * 2 >= (uint32_t)real.ticks,
	      "and most of the run is held ticks: waiting is what the pour now spends its time doing");
	printf("         %u of %d ticks were held by the barrier\n", real.stalls, real.ticks);

	// And it stops holding once there is nothing to hold. A counter that kept climbing over a
	// settled lake would mean the cooldown was ticking for ever, which is the shape of bug
	// testSettledIsFree exists for.
	const uint32_t settled_stalls = waterGenStalls(&g_sim);
	for (int t = 0; t < 1000; t++) (void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);
	CHECK(waterGenStalls(&g_sim) == settled_stalls,
	      "1000 ticks over the settled pour add not one stall: the barrier is idle, not spinning");
}

static void testSpreadRateIsTicksNotBudget(void)
{
	puts("water: the spread rate is ticks, and the budget is only a CPU cap");

	const SpreadRun r64  = spreadRun(WATER_TICK_BUDGET, 4000);
	const SpreadRun r512 = spreadRun(512, 4000);
	const SpreadRun r8   = spreadRun(8, 4000);
	printSpreadRun("budget 64", &r64);
	printSpreadRun("budget 512", &r512);
	printSpreadRun("budget 8", &r8);

	CHECK(r64.examined == r512.examined && r512.examined == r8.examined,
	      "control: all three budgets examine exactly the same cells in the same order");

	// The whole failing of the old code was that the budget WAS the rate: at 512 the pour
	// finished inside one tick and at 8 it crawled, and 64 was simply the number that made
	// "instant" come out as 200 ms. Now the generation decides when the next hop happens and the
	// budget can only ever add ticks to it, never remove them. So the claim is a floor, not an
	// equality — and the floor is the law testSpreadRate pins at budget 512.
	CHECK(r512.first_wet[2] == 1 + WATER_SPREAD_TICKS &&
	      r64.first_wet[2]  == 1 + WATER_SPREAD_TICKS &&
	      r8.first_wet[2]   >= 1 + WATER_SPREAD_TICKS,
	      "no budget gets water to distance 2 before tick 1 + WATER_SPREAD_TICKS");
	CHECK(r64.first_wet[7] >= r512.first_wet[7] && r8.first_wet[7] >= r64.first_wet[7],
	      "a budget too narrow to hold a generation is slower, never faster");
	CHECK(r512.first_wet[7] == 1 + 6 * WATER_SPREAD_TICKS && r512.first_wet[7] == 31,
	      "and 512 is wide enough to hold every generation, so it sits exactly on the law");

	// Sixty-four is NOT wide enough, and the gap is small and real: three ticks over a whole
	// pour. Measured, not derived — the frontier at distance 5 and beyond is wider than 64
	// candidates, so those generations each cost a second tick.
	printf("         distance 7 reached on tick %d at budget 512, %d at 64, %d at 8\n",
	       r512.first_wet[7], r64.first_wet[7], r8.first_wet[7]);
	CHECK(r64.first_wet[7] - r512.first_wet[7] == 3,
	      "the 64-cell cap costs the full pour exactly three ticks against the law");
	// The sensitivity control for the three comparisons above: they are only evidence if a
	// budget can move a tick count at all. Written as a bare inequality on the SETTLE time and
	// not as a ratio on first_wet, because a ratio is a claim about the rate as well as about
	// the budget, and this one has to stay green whatever WATER_SPREAD_TICKS is set to.
	CHECK(r8.ticks > r64.ticks && r64.ticks > r512.ticks,
	      "control: a narrower budget really does cost ticks, so the comparison can see a budget");

	CHECK(r8.first_wet[8] == 0 && r64.first_wet[8] == 0 && r512.first_wet[8] == 0,
	      "control: no budget lets the pour past distance 7");
}

static void testSettledIsFree(void)
{
	puts("water: a settled body costs nothing per tick");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);

	// Snapshotted AFTER the pour, so what follows is a delta and the fixture drain above — which
	// examines cells of its own now that the hook is live — cannot flatter or spoil it.
	const uint32_t examined = waterExamined(&g_sim);
	const uint32_t changed  = waterChanged(&g_sim);

	int did = 0;
	for (int t = 0; t < 5000; t++)
		did += waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);

	CHECK(did == 0, "5000 ticks over a settled 113-block body change nothing");
	CHECK(waterExamined(&g_sim) == examined, "and examine not one single cell");
	CHECK(waterChanged(&g_sim) == changed, "and the change counter does not move");
}

static int g_hook_calls;
static int g_hook_to_water;
static int g_hook_to_air;

static void countingHook(void* ud, int x, int y, int z, BlockId id)
{
	(void)ud; (void)x; (void)y; (void)z;
	g_hook_calls++;
	if (id == BLOCK_WATER)    g_hook_to_water++;
	else if (id == BLOCK_AIR) g_hook_to_air++;
}

static void testChangeHook(void)
{
	puts("water: on_change fires per block change, never per examination");

	resetWorld();
	buildFloor();
	g_hook_calls = g_hook_to_water = g_hook_to_air = 0;

	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 2000, countingHook, NULL);

	printf("         %d hook calls, %u examined, %u changed\n",
	       g_hook_calls, waterExamined(&g_sim), waterChanged(&g_sim));

	CHECK(g_hook_calls == 112, "112 calls: one per cell that became water");
	CHECK(g_hook_to_water == 112 && g_hook_to_air == 0, "all of them report BLOCK_WATER");
	CHECK((uint32_t)g_hook_calls < waterExamined(&g_sim),
	      "far fewer calls than examinations: a look is not a change");

	// A LEVEL-only change, which is the case the hook must stay quiet for. A second source
	// twelve blocks away overlaps the first pour from x = 5 outwards, and (7, 64, 0) — level 1
	// from the first source, level 3 from the second — is raised without its block moving. It
	// renders identically before and after, so a remesh for it would be pure cost.
	//
	// Twelve and not three: at three the cell is already a FLOW cell, and writing BLOCK_WATER
	// over water changes no block id, so nothing happens at all. Placing a source into water
	// that is already flowing is not something the block API can express, and it does not need
	// to be — water is past BLOCK_COUNT, so world/inventory.c refuses to hold it and worldgen
	// is the only thing that has ever placed a source.
	g_hook_calls = 0;
	(void)worldSet(&g_world, 12, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 12, 64, 0);
	const uint32_t before = waterChanged(&g_sim);
	(void)waterSettle(&g_sim, &g_world, 2000, countingHook, NULL);
	const uint32_t moved = waterChanged(&g_sim) - before;
	printf("         second source: %d hook calls for %u changes\n", g_hook_calls, moved);
	CHECK((uint32_t)g_hook_calls < moved,
	      "level-only changes happen and do not fire the hook");

	// Draining, on a fresh pour so the count is the same 112 the fill reported.
	resetWorld();
	buildFloor();
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);

	g_hook_calls = g_hook_to_water = g_hook_to_air = 0;
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_AIR);
	waterNotify(&g_sim, 0, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 2000, countingHook, NULL);
	printf("         drain: %d calls, %d to water, %d to air\n",
	       g_hook_calls, g_hook_to_water, g_hook_to_air);

	// 114 and not 112, and the extra pair is right rather than tolerated: the cell the source
	// was in is now air surrounded by level-7 water, so it refills as a level-6 flow cell
	// before the body around it collapses and takes it with it. One call reporting water, and
	// 113 reporting air — the 112 that were there plus that one.
	CHECK(g_hook_calls == 114, "draining fires 114 calls");
	CHECK(g_hook_to_water == 1, "one of them is the source cell briefly refilling");
	CHECK(g_hook_to_air == 113, "and 113 report BLOCK_AIR, so nothing is left behind");
	CHECK(countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER) == 0, "the world really is dry");
}

// v1.8.0 task 22. The leak that measurement found: replacing flowing water with a solid block
// leaves the map naming a cell that is no longer water, and 2048 slots is only a few hundred
// placed blocks away.
static void testStaleEntry(void)
{
	puts("water: building into flowing water does not leak a map slot");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);
	CHECK(waterFlowCells(&g_sim) == 112, "112 flow cells before the block goes in");
	CHECK(waterLevelAt(&g_sim, &g_world, 7, 64, 0) == 1, "the far cell is a level-1 flow cell");

	// A player places stone into the far end of the flow, exactly as scene/interact.c does —
	// and now literally so: the place goes through worldSet and the hook does the queueing.
	(void)worldSet(&g_world, 7, 64, 0, BLOCK_STONE);
	(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);

	CHECK(waterFlowCells(&g_sim) == 111, "the map drops that cell instead of keeping it");
	CHECK(waterLevelAt(&g_sim, &g_world, 7, 64, 0) == 0, "and the cell reads as no water");
	CHECK(worldGet(&g_world, 7, 64, 0) == BLOCK_STONE, "the placed block is still there");

	// Repeat it enough times that a one-slot-per-placement leak would be unmistakable, and
	// check the map is exactly as big as the water that is actually there.
	for (int i = 0; i < 300; i++) {
		(void)worldSet(&g_world, 7, 64, 0, BLOCK_STONE);
		(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);
		(void)worldSet(&g_world, 7, 64, 0, BLOCK_AIR);
		(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);
	}
	printf("         after 300 place/break cycles: %d flow cells, %u map refusals\n",
	       waterFlowCells(&g_sim), waterMapFull(&g_sim));
	CHECK(waterFlowCells(&g_sim) == 112, "300 place/break cycles later the map is still 112");
	CHECK(waterMapFull(&g_sim) == 0, "and nothing was ever refused a slot");
}

static int g_edits;

static void countingEditHook(void* ud, const World* w, int x, int y, int z, BlockId prev, BlockId now)
{
	(void)ud; (void)w; (void)x; (void)y; (void)z; (void)prev; (void)now;
	g_edits++;
}

static void testEditHook(void)
{
	puts("world: the edit hook fires on a real change and on nothing else");

	resetWorld();
	buildFloor();
	worldSetEditHook(countingEditHook, NULL);

	g_edits = 0;
	CHECK(worldSet(&g_world, 0, 64, 0, BLOCK_WATER) && g_edits == 1,
	      "one call for one block that actually changed");

	g_edits = 0;
	CHECK(worldSet(&g_world, 0, 64, 0, BLOCK_WATER) && g_edits == 0,
	      "no call when the write stores the value that was already there");

	g_edits = 0;
	CHECK(!worldSet(&g_world, 0, -1, 0, BLOCK_STONE) && g_edits == 0,
	      "no call for a write that was refused");

	// A generated or loaded chunk is not an edit. Firing 4096 times per streaming chunk is
	// exactly the stall this hook must never introduce.
	BlockId all[CHUNK_BLOCKS];
	for (int i = 0; i < CHUNK_BLOCKS; i++) all[i] = BLOCK_STONE;
	g_edits = 0;
	CHECK(worldSetChunkAll(&g_world, 40, 2, 40, all) && g_edits == 0,
	      "worldSetChunkAll installs 4096 blocks and fires the hook zero times");

	// And the real wiring: with waterNotify on the hook, placing water queues that cell and
	// its six neighbours without water_test ever calling waterNotify itself.
	resetWorld();
	buildFloor();
	worldSetEditHook(countingEditHook, NULL);
	g_edits = 0;
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);
	CHECK(g_edits == 1 && waterPending(&g_sim) == 7,
	      "an edit puts the cell and its six neighbours on the queue");

	worldSetEditHook(NULL, NULL);
	g_edits = 0;
	(void)worldSet(&g_world, 1, 64, 1, BLOCK_STONE);
	CHECK(g_edits == 0, "a NULL hook is genuinely uninstalled");
}

// ── v1.8.3. WHICH world the hook reports ─────────────────────────────────────────────
//
// testEditHook above checks WHETHER the hook fires. Nothing in this project checked WHICH world
// it fired for, and that gap is the whole reason the defect below survived a release:
//
//   world.c's s_edit_fn is one file static and worldSet fires it for any World* at all. main.c
//   installs onWorldEdit against s_world (main.c:3199), but app/worker.c generates into a SECOND
//   world, s_staging (worker.c:52, worker.c:266), and worldgen's decoration pass writes single
//   blocks into it through worldSet (worldgen.c:339, worldgen.c:458) ON THE WORKER THREAD.
//   Before WorldEditFn carried a World*, every one of those edits reached waterNotify: candidates
//   naming cells in a world the simulation does not simulate, pushed from a thread holding no
//   lock over the ring.
//
// So this probe drives TWO worlds through one hook and asserts the callback can tell them apart,
// with a hook that is a faithful copy of main.c's onWorldEdit filter. It goes red the moment
// world.c stops passing the world through — which is exactly the sabotage it was built against.
static World        g_staging;         // the second world, standing in for app/worker.c's
static const World* g_hook_world;      // the world the last firing named
static int          g_hook_fires;      // firings seen, whatever world
static int          g_hook_admitted;   // firings that passed the &g_world filter

static void filteringEditHook(void* ud, const World* w, int x, int y, int z, BlockId prev, BlockId now)
{
	(void)ud; (void)prev; (void)now;

	g_hook_fires++;
	g_hook_world = w;

	// The line under test. main.c:2837 has this exact compare against &s_world.
	if (w != &g_world) return;

	g_hook_admitted++;
	waterNotify(&g_sim, x, y, z);
}

static void testEditHookReportsWorld(void)
{
	puts("world: the edit hook reports WHICH world the edit went to");

	resetWorld();
	buildFloor();
	worldInit(&g_staging);
	worldSetEditHook(filteringEditHook, NULL);

	// Arm 1 — an edit to the world this simulation owns. It must be admitted.
	g_hook_fires = 0; g_hook_admitted = 0; g_hook_world = NULL;
	const bool live_ok = worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	CHECK(live_ok && g_hook_fires == 1, "an edit to the live world fires the hook once");
	CHECK(g_hook_world == &g_world, "and the callback is told it was the live world");
	CHECK(g_hook_admitted == 1 && waterPending(&g_sim) == 7,
	      "so the filter admits it: the cell and its six neighbours are queued");

	// Arm 2 — the identical edit, into the OTHER world. It must be dropped.
	//
	// y is 64 because it has to be the SAME cell arm 1 wrote and the same cell the control
	// below reads back. worldSet's first line is `if (y < 0 || y >= WORLD_HEIGHT) return false`
	// (world.c:257), so an out-of-range y here returns before the hook can fire — and then
	// "so the filter drops it" passes because nothing was queued by a write that never
	// happened, which is the exact wrong reason the control was written to catch.
	const int pending_before = waterPending(&g_sim);
	g_hook_fires = 0; g_hook_admitted = 0; g_hook_world = NULL;
	const bool staging_ok = worldSet(&g_staging, 0, 64, 0, BLOCK_WATER);
	CHECK(staging_ok && g_hook_fires == 1, "an edit to a second world fires the same hook once");
	CHECK(g_hook_world == &g_staging && g_hook_world != &g_world,
	      "and the callback is told THAT world, not the live one");
	CHECK(g_hook_admitted == 0 && waterPending(&g_sim) == pending_before,
	      "so the filter drops it: nothing is queued for a world this sim does not simulate");

	// Control, and it is the one that keeps arm 2 from passing for the wrong reason. "Nothing was
	// queued" is also what a write that never happened produces, so the write is read back.
	CHECK(worldGet(&g_staging, 0, 64, 0) == BLOCK_WATER &&
	      worldGet(&g_world,   0, 64, 0) == BLOCK_WATER,
	      "control: both writes really landed, so arm 2 is a filter result and not a dead write");

	worldSetEditHook(NULL, NULL);
	worldExit(&g_staging);
}

static void testDropColumn(void)
{
	puts("water: unloading a column takes its flow water and leaves its sources");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);   // column (0, 0)
	(void)waterSettle(&g_sim, &g_world, 2000, NULL, NULL);

	const int before_flow  = waterFlowCells(&g_sim);
	const int before_water = countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER);

	// Flow cells inside column (0, 0) are those with 0 <= x <= 15 and 0 <= z <= 15, minus
	// the source itself, which has no map entry and so is not one.
	int in_column = 0;
	for (int z = 0; z <= 13; z++)
		for (int x = 0; x <= 13; x++)
			if (manhattan(x, z) >= 1 && manhattan(x, z) <= 7) in_column++;

	const int cleared = waterDropColumn(&g_sim, &g_world, 0, 0);
	printf("         cleared %d of %d flow cells\n", cleared, before_flow);

	CHECK(cleared == in_column, "every flow cell of column (0,0) is cleared and no other");
	CHECK(waterFlowCells(&g_sim) == before_flow - cleared, "the map shrinks by exactly that");
	CHECK(worldGet(&g_world, 0, 64, 0) == BLOCK_WATER, "the source is still there");
	CHECK(waterLevelAt(&g_sim, &g_world, 0, 64, 0) == WATER_LEVEL_SOURCE,
	      "and still reads as a source");
	CHECK(countBlocks(0, 13, 64, 64, 0, 13, BLOCK_WATER) == 1,
	      "the only water left in column (0,0) is the source block");
	CHECK(countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER) == before_water - cleared,
	      "water outside the dropped column is untouched");
	// v1.8.3: this check has teeth for the first time. waterDropColumn clears its cells through
	// worldSet, so with the hook live every one of them fires world.c's edit hook and reaches
	// waterNotify — and the only thing stopping seven candidates per cleared cell is
	// WaterSim.dropping (water.h). Built with the hook NULL, as this probe used to be, nothing
	// ever reached waterNotify and the flag was never exercised at all.
	CHECK(waterPending(&g_sim) == 0,
	      "dropping a column queues nothing: it is being removed, not changed");
}

// Encodes every allocated chunk of a column into one buffer, exactly the way the save path
// does, so two calls can be compared byte for byte.
static size_t encodeColumn(int cx, int cz, uint8_t* out, size_t cap)
{
	size_t used = 0;
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		const Chunk* c = worldChunk(&g_world, cx, cy, cz);
		out[used++] = (uint8_t)(c ? 1 : 0);
		if (!c) continue;
		const size_t n = chunkEncode(c, out + used + 2, cap - used - 2);
		out[used++] = (uint8_t)(n & 0xFF);
		out[used++] = (uint8_t)(n >> 8);
		used += n;
	}
	return used;
}

static void testSaveCompat(void)
{
	puts("water: a v1.7.1 world opens unchanged and no flow cell ever reaches the card");

	// v1.8.3, and this is the ONE probe here whose fixture is deliberately built with the hook
	// DOWN — which is an application of the fidelity rule, not an exception to it.
	//
	// Every other fixture in this file stands in for something a player does, and a player's
	// blocks go in one at a time through worldSet, so the hook has to be live. This one stands in
	// for a world ARRIVING FROM THE CARD, and that path fires the hook exactly zero times by
	// design: a loaded or generated column is installed with worldSetChunkAll, which testEditHook
	// pins at zero firings. Arming the hook over this fixture models a world no path in the game
	// can produce.
	//
	// It also destroys the probe, measured rather than argued. With the hook armed over the build:
	// the air cavity is carved AFTER the sea, so carving it notifies the sea wall, the ocean pours
	// into the cavity during the fixture's own drain, and "a freshly opened world" arrives already
	// holding 208 flow cells. The premise this whole probe rests on — that a v1.7.1 world opens
	// with every water cell a source and nothing queued — cannot even be stated over that fixture.
	//
	// So: terrain with the hook down, exactly as the card delivers it; hook armed below for the
	// DISTURBANCE, which is the part a player causes and the part that must run on the game's
	// wiring.
	resetWorld();

	// A column shaped the way worldgen's density generator leaves one: stone up to the
	// seabed and water from there to the waterline, every water cell a source because
	// nothing has a map entry. Column (0,0) is the ocean; column (-1,0) is the dry cavity
	// beside it.
	//
	// Sealed in solid rock on every other side, which is not decoration: the first version of
	// this fixture left the sea open at z = 16, the ocean poured off the edge, fell to the
	// world floor and spread there, and 2048 map slots filled. That is correct behaviour for
	// an open edge and useless for measuring a save.
	fillBox(-20, 20, 55, 71, -4, 20, BLOCK_STONE);
	fillBox(0, 15, 64, 70, 0, 15, BLOCK_WATER);
	fillBox(-16, -1, 64, 70, 0, 15, BLOCK_AIR);

	// The card has delivered the world; from here on every change is one a player causes, so the
	// hook goes on. Nothing above this line fired it, which is the whole point of the note at the
	// top of this probe, and the two checks below say so rather than leaving it to the reader.
	CHECK(waterPending(&g_sim) == 0,
	      "loading a world queues nothing at all: worldSetChunkAll is not an edit");
	worldSetEditHook(wiredEditHook, NULL);

	// Opening it: the map is empty and every one of those 1792 water cells reads as a
	// source, which is exactly what it is.
	CHECK(waterFlowCells(&g_sim) == 0, "a freshly opened world has no flow cells at all");
	bool all_source = true;
	for (int y = 64; y <= 70; y++)
		for (int z = 0; z <= 15; z++)
			for (int x = 0; x <= 15; x++)
				if (waterLevelAt(&g_sim, &g_world, x, y, z) != WATER_LEVEL_SOURCE)
					all_source = false;
	CHECK(all_source, "all 1792 generated water cells read as sources");
	CHECK(waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL) == 0
	      && waterExamined(&g_sim) == 0,
	      "and an untouched world does no work: nothing was ever queued");

	static uint8_t ocean0[262144], ocean1[262144];
	static uint8_t cliff0[262144], cliff1[262144];
	const size_t n_ocean0 = encodeColumn(0, 0, ocean0, sizeof ocean0);
	const size_t n_cliff0 = encodeColumn(-1, 0, cliff0, sizeof cliff0);
	CHECK(n_ocean0 > 0 && n_cliff0 > 0, "both columns encode to save bytes");

	// Now disturb it: the sea spills over the cliff column. Nothing about column (0,0)
	// changes, because it has no air in it for water to move into.
	for (int y = 64; y <= 70; y++)
		waterNotify(&g_sim, 0, y, 8);
	const int ticks = waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	printf("         flood settled in %d ticks, %d flow cells\n", ticks, waterFlowCells(&g_sim));
	CHECK(ticks >= 0, "the flood settles");
	CHECK(waterFlowCells(&g_sim) > 0, "and it really did flood something");

	const size_t n_ocean1 = encodeColumn(0, 0, ocean1, sizeof ocean1);
	CHECK(n_ocean1 == n_ocean0 && memcmp(ocean0, ocean1, n_ocean0) == 0,
	      "the ocean column encodes to byte-identical save data after the flood");

	// And the flooded column goes back to exactly the world the card already holds once it is
	// dropped, which is the whole reason waterDropColumn runs before the save encode in main.c.
	const int cleared = waterDropColumn(&g_sim, &g_world, -1, 0);
	CHECK(cleared > 0, "the flooded column had flow water to clear");

	bool content_same = true;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z <= 15; z++)
			for (int x = -16; x <= -1; x++) {
				const BlockId want = (y >= 64 && y <= 70) ? BLOCK_AIR
				                   : (y >= 55 && y <= 71) ? BLOCK_STONE : BLOCK_AIR;
				if (worldGet(&g_world, x, y, z) != want) content_same = false;
			}
	CHECK(content_same,
	      "every block of the flooded column is back to what it was before the flood");

	// The BYTES are a separate question and the answer is deliberately reported rather than
	// asserted. chunkEncode writes whatever storage form the chunk is in now, and a chunk that
	// was UNIFORM air was promoted when water moved into it and stays promoted afterwards — so
	// identical content can encode to a different length. That is harmless: chunk_codec.c's
	// decoder reads any form, and a column is only written at all when scene/interact.c has
	// marked it dirty, which flowing water never does. It would matter if the save format were
	// content-addressed, and it is not.
	const size_t n_cliff1 = encodeColumn(-1, 0, cliff1, sizeof cliff1);
	printf("         cliff column save bytes: %zu before, %zu after (%s)\n",
	       n_cliff0, n_cliff1,
	       (n_cliff1 == n_cliff0 && memcmp(cliff0, cliff1, n_cliff0) == 0)
	           ? "identical" : "same content, promoted storage form");

	// Control: the content comparison above must be able to fail.
	(void)worldSet(&g_world, -5, 40, 5, BLOCK_STONE);
	bool control_same = true;
	for (int y = 0; y < WORLD_HEIGHT && control_same; y++)
		for (int z = 0; z <= 15 && control_same; z++)
			for (int x = -16; x <= -1; x++) {
				const BlockId want = (y >= 64 && y <= 70) ? BLOCK_AIR
				                   : (y >= 55 && y <= 71) ? BLOCK_STONE : BLOCK_AIR;
				if (worldGet(&g_world, x, y, z) != want) { control_same = false; break; }
			}
	CHECK(!control_same, "control: changing one block in that column is detected");
}

static void testQueueOverflow(void)
{
	puts("water: a refused candidate is counted, and the flow reconverges when re-offered");

	const uint64_t reference = pourWithBudget(WATER_TICK_BUDGET);

	resetWorld();
	buildFloor();

	// Flood the ring with candidates for cells that hold nothing, so the source's own
	// notification is refused. 300 distinct cells x 7 keys each is well past WATERQ_CAP.
	for (int i = 0; i < 300; i++) waterNotify(&g_sim, 1000 + i * 3, 64, 0);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);

	CHECK(waterQueueFull(&g_sim) > 0, "the refusal is counted, not swallowed");
	printf("         %u candidates refused\n", waterQueueFull(&g_sim));

	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	const uint64_t after_loss = hashBox(-13, 13, 60, 70, -13, 13);
	printf("         after the losses: %d flow cells (reference has 112)\n",
	       waterFlowCells(&g_sim));

	// Offer every cell again, a few at a time so the ring is never the limit. This is what
	// any later disturbance near the water does locally; here it is done exhaustively so the
	// claim is about convergence and not about luck.
	for (int y = 63; y <= 66; y++)
		for (int z = -13; z <= 13; z++)
			for (int x = -13; x <= 13; x++) {
				waterNotify(&g_sim, x, y, z);
				(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
			}

	CHECK(hashBox(-13, 13, 60, 70, -13, 13) == reference,
	      "re-offering the cells converges on exactly the reference world");
	CHECK(waterFlowCells(&g_sim) == 112, "112 flow cells again");
	(void)after_loss;
}

// ── The overflow policy, and the claim that used to be made about it ──────────────────
//
// v1.8.3. water.c's qPush carried this comment until today: a candidate refused for want of a ring
// slot "is not a permanent loss ... a candidate that was refused is corrected the next time
// anything beside it moves". Nothing in this file tested it, and it is false. For a source placed
// into a world that is then left alone, nothing beside it ever moves again, so the correction
// never arrives — and because a SOURCE has no map entry (water.h's absence-means-source rule), the
// simulation retains no record that the cell exists, so there is nothing to re-offer either. The
// result is a lone full cube of water sitting on flat ground for ever while waterPending() reads
// 0, waterFlowCells() reads 0 and waterSettle() reports a clean finish in single figures.
//
// That is the exact shape of the open v1.8.0 report — water not spreading, queue saturated — and
// it is reachable in the shipping build: measured on 2026-08-25 against the real density generator
// and the real edit hook, a source placed after terrain streaming failed to spread in 16 of 200
// swept arms (8.0%). qPush now evicts the OLDEST pending candidate instead of refusing the newest,
// which took that to 0 of 200.
//
// This probe is the arm that goes red if that is ever reverted. It is deliberately built the
// game's way — resetWorldWired, no hand waterNotify — because the whole failure is about what
// reaches the ring through world.c's edit hook, and a hand call bypasses the thing under test.
static void testOverflowKeepsTheNewest(void)
{
	puts("water: a full ring loses its OLDEST candidate, never the edit that just happened");

	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);

	// Saturate the ring with candidates for cells a thousand blocks away that hold nothing, so
	// that the source placed below arrives at a ring that is genuinely full. 300 distinct cells
	// at 7 keys each is well past WATERQ_CAP, and none of them is anywhere near the floor.
	for (int i = 0; i < 300; i++) waterNotify(&g_sim, 1000 + i * 3, 64, 0);
	CHECK(waterPending(&g_sim) == (int)WATERQ_CAP, "the ring really is full before the edit");
	const uint32_t lost_before = waterQueueFull(&g_sim);

	// The player places a source, through worldSet, so the edit hook is what queues it.
	CHECK(worldSet(&g_world, 0, 64, 0, BLOCK_WATER), "the source block is placed into a full ring");

	// These two say that SEVEN candidates were lost and that the ring did not grow — and both are
	// true under EITHER policy, which is worth stating here rather than leaving for someone to
	// discover the hard way. Refuse-newest loses the seven new ones; evict-oldest loses seven old
	// ones; either way the counter moves by seven and the depth stays pinned at the cap. Verified
	// by sabotage on 2026-08-25: with qPush reverted to refuse-newest, both of these stayed GREEN.
	// They are here because the loss must remain visible and bounded, not because they discriminate.
	//
	// The checks that DO discriminate are the outcome pair below, and that is not a weakness: the
	// only honest way to ask which candidate was kept is to ask what the water did.
	CHECK(waterQueueFull(&g_sim) - lost_before == 7,
	      "seven candidates are lost, and the loss is counted rather than hidden");
	CHECK(waterPending(&g_sim) == (int)WATERQ_CAP,
	      "the ring is still exactly full: one admitted for one evicted, never more");

	// And the outcome, which is the thing a player would actually see. The world is STATIC from
	// here: nothing but the simulation itself ever touches it again, which is precisely the
	// condition the old comment claimed would heal this and does not.
	const int ticks = waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	printf("         settled in %d ticks, %d flow cells, %d wet in the pour plane\n",
	       ticks, waterFlowCells(&g_sim), countBlocks(-13, 13, 64, 64, -13, 13, BLOCK_WATER));

	CHECK(ticks >= 0, "the pour reaches a stable state");
	CHECK(waterFlowCells(&g_sim) == 112,
	      "the source placed into a FULL ring still spreads to all 112 flow cells");
	CHECK(countBlocks(-13, 13, 60, 70, -13, 13, BLOCK_WATER) == 113,
	      "113 water blocks: the source did not sit alone");

	// The control, and it is the one that says this probe can see the difference at all. Same
	// world, same source cell, but the ring is left EMPTY — so nothing is evicted, nothing is
	// lost, and the answer must be the identical 112. A probe whose full-ring arm passed while
	// its empty-ring arm also passed for some unrelated reason would prove nothing about the
	// ring; this pins that the two arms agree, which is the actual claim.
	resetWorldWired();
	buildFloor();
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	const uint32_t control_lost = waterQueueFull(&g_sim);
	CHECK(waterPending(&g_sim) == 0, "control: this arm starts from an EMPTY ring");
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	CHECK(waterQueueFull(&g_sim) == control_lost,
	      "control: an empty ring loses nothing at all, so the eviction path really did fire above");
	CHECK(waterFlowCells(&g_sim) == 112,
	      "control: the same source into an empty ring reaches the same 112 flow cells");
}

// ── The ring and the dedup set, checked against each other ────────────────────────────
//
// v1.8.3. Every other probe in this file asks what the water DID. This one asks whether the two
// tables that decide what gets examined still agree, and it is the only check here that no
// outcome can stand in for.
//
// water.c:202 short-circuits qPush on qsetFind. So a qset entry with no ring entry behind it is a
// coordinate this simulation refuses to examine FOR EVER, with nothing queued to examine it — and
// nothing about that is visible from outside. Every index is masked, so there is no crash and no
// out-of-bounds read; waterPending(), waterFlowCells() and waterQueueFull() all go on reporting a
// simulation in perfect health while a cell of the player's world sits permanently outside it.
// Silent, permanent and unrecoverable is a worse outcome here than a crash, because a crash gets
// reported.
//
// MEASURED on 2026-08-30, by sabotaging the real water.c: qPop made to skip its qsetEraseAt for
// one pop in 64 — the shape of the preemption race main.c's onWorldEdit describes, where the main
// thread interrupts the worker part-way through qPush — left the 200-arm streaming sweep in
// scratchpad/ws_sweep.c with an orphaned key in 200 of 200 arms (median 9, max 9), while every one
// of those same arms still reported 113 wet cells, 0 candidates lost and a ring depth of 0. A
// 25-source basin pour under that sabotage settled to hash ec667e6cbf27762b, which is byte for
// byte the hash the healthy build produces, with 43 orphans sitting behind it. So neither an
// outcome check nor a world hash nor any accessor in this file could have caught it. This probe
// reads the two tables directly, which is the only thing that can.
//
// The primary claim is the tighter of the two available: qPush refuses any key already in the set,
// so the ring can hold no duplicates, so the number of occupied qset slots must equal
// waterPending() EXACTLY. That single equality rules out an orphaned key and a lost dedup key at
// once. The orphan count is checked as well rather than instead, because it is the half that is
// dangerous and it names the failure in the output when it goes red.
static int qsetLive(const WaterSim* s)
{
	int n = 0;
	for (uint32_t i = 0; i < WATERQ_SET_SLOTS; i++)
		if (s->qset[i] != 0) n++;
	return n;
}

static int qsetOrphans(const WaterSim* s)
{
	int n = 0;
	for (uint32_t i = 0; i < WATERQ_SET_SLOTS; i++) {
		if (s->qset[i] == 0) continue;
		bool in_ring = false;
		for (int k = 0; k < s->qcount && !in_ring; k++)
			if (s->ring[(s->qhead + k) & (WATERQ_CAP - 1)] == s->qset[i]) in_ring = true;
		if (!in_ring) n++;
	}
	return n;
}

static void testNoOrphanedDedupKeys(void)
{
	puts("water: the dedup set never holds a key the ring has stopped holding");

	// Arm 1: an ordinary pour, stopped PART WAY so the ring is genuinely occupied. A probe that
	// only ever looked at a drained simulation would pass on an empty set and prove nothing.
	resetWorldWired();
	buildFloor();
	wiredBuildDone("floor", 8156, 1024);
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	for (int t = 0; t < 12; t++) (void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);

	printf("         mid-pour: %d pending, %d keys in the set, %d orphaned\n",
	       waterPending(&g_sim), qsetLive(&g_sim), qsetOrphans(&g_sim));
	CHECK(waterPending(&g_sim) > 0,
	      "control: the pour was stopped with the ring still occupied, so the set is not empty");
	CHECK(qsetLive(&g_sim) == waterPending(&g_sim),
	      "mid-pour, the set holds exactly one key per queued cell");
	CHECK(qsetOrphans(&g_sim) == 0, "mid-pour, no key in the set has lost its ring entry");

	// Arm 2: the overflow path, which is the one that calls qPop from INSIDE qPush. That is the
	// only place in this file where an entry leaves the ring anywhere but the top of waterTick,
	// so it is where a desynchronisation would be introduced.
	const uint32_t lost_before = waterQueueFull(&g_sim);
	for (int i = 0; i < 300; i++) waterNotify(&g_sim, 1000 + i * 3, 64, 0);
	const uint32_t lost = waterQueueFull(&g_sim) - lost_before;
	printf("         after overflow: %d pending, %d keys in the set, %d orphaned, %u lost\n",
	       waterPending(&g_sim), qsetLive(&g_sim), qsetOrphans(&g_sim), lost);
	CHECK(waterPending(&g_sim) == (int)WATERQ_CAP,
	      "control: the ring really is full, so the eviction path really did run");
	CHECK(lost > 0, "control: and candidates really were lost, so this is not an empty arm");
	CHECK(qsetLive(&g_sim) == waterPending(&g_sim),
	      "after an eviction, the set still holds exactly one key per queued cell");
	CHECK(qsetOrphans(&g_sim) == 0, "an evicted candidate takes its dedup key with it");

	// Arm 3: drained. With nothing queued, nothing may remain in the set — a key left here is the
	// permanent refusal in its purest form, because there is no longer anything to pop that could
	// ever clear it.
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	printf("         drained: %d pending, %d keys in the set\n",
	       waterPending(&g_sim), qsetLive(&g_sim));
	CHECK(waterPending(&g_sim) == 0, "control: the simulation really did drain");
	CHECK(qsetLive(&g_sim) == 0, "an empty ring leaves an empty set: nothing is refused for ever");

	// Arm 4: waterDropColumn, which erases map entries and writes blocks with the edit hook live
	// and `dropping` set. It is the one path that changes the world while deliberately queueing
	// nothing, so it is the one most able to leave the two tables disagreeing.
	(void)worldSet(&g_world, 0, 64, 0, BLOCK_WATER);
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);

	// Disturbed again and stopped mid-flight, so the drop below runs against an OCCUPIED ring.
	// Dropping into an empty one would satisfy both checks with a pair of zeroes and could not
	// have gone red.
	(void)worldSet(&g_world, 0, 65, 0, BLOCK_WATER);
	for (int t = 0; t < 3; t++) (void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);
	const int before_drop = waterPending(&g_sim);
	const int cleared     = waterDropColumn(&g_sim, &g_world, 0, 0);
	printf("         after dropping column (0,0): %d cells cleared, %d pending (was %d), %d keys"
	       " in the set, %d orphaned\n",
	       cleared, waterPending(&g_sim), before_drop, qsetLive(&g_sim), qsetOrphans(&g_sim));
	CHECK(cleared > 0, "control: the drop really did clear flow cells, so this arm is not a no-op");
	CHECK(before_drop > 0,
	      "control: and it ran against an occupied ring, so the two checks below are not two zeroes");
	CHECK(qsetLive(&g_sim) == waterPending(&g_sim),
	      "after a column drop, the set still holds exactly one key per queued cell");
	CHECK(qsetOrphans(&g_sim) == 0, "and the drop leaves no key behind with nothing queued for it");
}

static void testTickCost(void)
{
	puts("water: measured cost per tick, settled and active arms interleaved");

	// A realistic body rather than a toy: a sealed 29x29 basin flooded from a grid of sources
	// six apart, which settles to something like eight hundred flow cells — a lake a player
	// could stand beside, not a two-block puddle. Sealed for the same reason testSaveCompat is:
	// an open edge turns the measurement into a measurement of water falling off the world.
	resetWorld();
	fillBox(-18, 18, 55, 70, -18, 18, BLOCK_STONE);
	fillBox(-14, 14, 64, 66, -14, 14, BLOCK_AIR);
	// One source at a time, each settled before the next goes in. Dropping all 25 at once
	// pushes more candidates than WATERQ_CAP holds, and a measurement taken over a body that
	// was assembled through a queue overflow is a measurement of the wrong thing.
	for (int z = -12; z <= 12; z += 6)
		for (int x = -12; x <= 12; x += 6) {
			(void)worldSet(&g_world, x, 64, z, BLOCK_WATER);
			waterNotify(&g_sim, x, 64, z);
			(void)waterSettle(&g_sim, &g_world, 50000, NULL, NULL);
		}
	const int body = waterFlowCells(&g_sim);
	printf("         body: %d flow cells, %d water blocks, %u refused map, %u refused queue\n",
	       body, countBlocks(-18, 18, 55, 70, -18, 18, BLOCK_WATER),
	       waterMapFull(&g_sim), waterQueueFull(&g_sim));
	CHECK(body > 400, "the measured body really is a large one");
	CHECK(waterMapFull(&g_sim) == 0 && waterQueueFull(&g_sim) == 0,
	      "and it fits, so the measurement is of a working simulation");

	const int ROUNDS = 8, SETTLED_TICKS = 20000;
	double settled_s = 0.0, active_s = 0.0;
	long   active_ticks = 0;

	// Paired and interleaved in one process, alternating arm A and arm B every round, so that
	// a machine that speeds up or slows down partway through the run biases both arms the same
	// way instead of making one of them look good.
	for (int r = 0; r < ROUNDS; r++) {
		// Arm A: the settled lake, which is what the console pays 20 times a second for every
		// second the player is not touching the water.
		clock_t t0 = clock();
		for (int t = 0; t < SETTLED_TICKS; t++)
			(void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);
		settled_s += (double)(clock() - t0) / CLOCKS_PER_SEC;

		// Arm B: the whole lake destroyed and rebuilt. A single broken block is not worth
		// timing — the first version of this measured 8 ticks in total, which clock() cannot
		// resolve — so this pulls every source out, drains 800-odd cells to nothing, puts them
		// all back and refills. Every tick in here spends its full 64-cell budget, so what
		// comes out is the cost of a saturated tick and not of an average one.
		int ticks = 0;
		t0 = clock();
		for (int z = -12; z <= 12; z += 6)
			for (int x = -12; x <= 12; x += 6) {
				(void)worldSet(&g_world, x, 64, z, BLOCK_AIR);
				waterNotify(&g_sim, x, 64, z);
				while (waterPending(&g_sim) > 0 && ticks < 200000) {
					(void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);
					ticks++;
				}
			}
		for (int z = -12; z <= 12; z += 6)
			for (int x = -12; x <= 12; x += 6) {
				(void)worldSet(&g_world, x, 64, z, BLOCK_WATER);
				waterNotify(&g_sim, x, 64, z);
				while (waterPending(&g_sim) > 0 && ticks < 200000) {
					(void)waterTick(&g_sim, &g_world, WATER_TICK_BUDGET, NULL, NULL);
					ticks++;
				}
			}
		active_s += (double)(clock() - t0) / CLOCKS_PER_SEC;
		active_ticks += ticks;
	}

	const double settled_us = settled_s * 1e6 / (double)(ROUNDS * SETTLED_TICKS);
	const double active_us  = active_ticks ? active_s * 1e6 / (double)active_ticks : 0.0;
	printf("         settled: %.4f us/tick over %d ticks\n",
	       settled_us, ROUNDS * SETTLED_TICKS);
	printf("         active : %.4f us/tick over %ld ticks of a full %d-cell budget\n",
	       active_us, active_ticks, WATER_TICK_BUDGET);

	CHECK(active_ticks > 1000, "the active arm ran enough ticks for clock() to resolve");
	CHECK(waterFlowCells(&g_sim) == body,
	      "eight destroy/rebuild rounds later the lake is exactly the lake it started as");
	CHECK(waterMapFull(&g_sim) == 0, "and no map slot was ever refused during the measurement");
	CHECK(settled_us < 0.5, "a settled body costs well under half a microsecond per tick");
	CHECK(active_us > settled_us * 10.0,
	      "control: the active arm really is doing work the settled arm is not");
	CHECK(active_us < 200.0,
	      "a full budget of 64 cells costs under 200 us on this host");
}

// ── The sunken basin: where a source SITS decides whether it spreads ──────────────────
//
// Written on 2026-08-24 to settle an observation off an emulator run, where one source dropped
// into the corner of a 7x7 sunken basin sat there as a lone full cube for twelve seconds while a
// 3x3 of sources in the same basin's CENTRE spread on screen. An emulator cannot tell a
// simulation rule from a rendering one; this can, because both arms are built from one fixture
// and the ONLY thing that differs between them is where the source is put.
//
// The arrangement, in world coordinates relative to a chosen (vcx, vy, vcz):
//   * a stone pad at y = vy over dx,dz in [-8,8], solid stone all the way down under it so
//     there is no escape route the basin did not intend
//   * air at vy+1 .. vy+12 over the same square
//   * a sunken basin: air at y = vy over dx,dz in [-3,3], stone floor at vy-1
//   * a shaft under the basin centre: air at vy-1 .. vy-6 over dx,dz in [-1,1], stone at vy-7
//
// What it establishes, and it is not what the emulator picture suggested: the CORNER source is
// the one that spreads, and the CENTRE source is the one that does not — because the centre sits
// over the shaft mouth, water that CAN STILL GO DOWN does not also go sideways (water.c's
// sideFeed), so the centre source pours straight down instead of filling the basin. Both are the
// documented rule working, not a fault.
#define BASIN_VCX  5
#define BASIN_VY   64
#define BASIN_VCZ  (-3)

static void buildBasin(void)
{
	// Everything from well under the shaft floor up to and including the pad is stone first;
	// the air is then carved out of it. Cheaper to reason about than laying slabs, and it makes
	// "no accidental escape routes below vy-1" true by construction rather than by inspection.
	fillBox(BASIN_VCX - 8, BASIN_VCX + 8, BASIN_VY - 20, BASIN_VY,
	        BASIN_VCZ - 8, BASIN_VCZ + 8, BLOCK_STONE);
	fillBox(BASIN_VCX - 8, BASIN_VCX + 8, BASIN_VY + 1, BASIN_VY + 12,
	        BASIN_VCZ - 8, BASIN_VCZ + 8, BLOCK_AIR);
	fillBox(BASIN_VCX - 3, BASIN_VCX + 3, BASIN_VY, BASIN_VY,
	        BASIN_VCZ - 3, BASIN_VCZ + 3, BLOCK_AIR);
	fillBox(BASIN_VCX - 1, BASIN_VCX + 1, BASIN_VY - 6, BASIN_VY - 1,
	        BASIN_VCZ - 1, BASIN_VCZ + 1, BLOCK_AIR);
	fillBox(BASIN_VCX - 1, BASIN_VCX + 1, BASIN_VY - 7, BASIN_VY - 7,
	        BASIN_VCZ - 1, BASIN_VCZ + 1, BLOCK_STONE);
}

typedef struct {
	int ticks;        // waterSettle's return: ticks used, or -1 if still busy at the limit
	int flow_cells;   // waterFlowCells over the whole world
	int basin_wet;    // cells with a level > 0 in the 7x7 basin plane at y = vy
	int basin_max;    // the highest level found in that plane, source included
	int shaft_wet;    // water blocks in the 3x3 shaft between vy-6 and vy-1
	int pool_cells;   // water blocks on the shaft floor at vy-6
	int pool_max;     // the highest level on that floor
	uint32_t build_q_full;   // candidates the fixture BUILD alone had refused, hook live
	int      build_pending;  // still queued when the build finished, before any tick
	int      build_flow;     // flow cells the build left behind — must be zero, it is all stone
} BasinArm;

// Runs one arm and prints everything an eye on the emulator would have been trying to read.
static BasinArm basinArm(const char* label, int sx, int sz)
{
	BasinArm r;
	memset(&r, 0, sizeof r);

	printf("  -- %s source, one BLOCK_WATER at (%d, %d, %d) --\n", label, sx, BASIN_VY, sz);

	resetWorldWired();
	buildBasin();

	// The fixture goes in one block at a time through worldSet, so with the game's wiring the
	// hook fires on every changed cell of it — buildBasin turns 6069 cells to stone before it
	// carves anything — and not one tick runs in between. That is not a shape the game can
	// produce: worldgen installs terrain through worldSetChunkAll, which fires the hook exactly
	// zero times by design (testEditHook above pins that), and a player laying blocks by hand
	// gets 20 drains a second. So the burst is a property of the FIXTURE, and it is measured
	// here and asserted on by its caller rather than folded silently into the pour below.
	r.build_q_full  = waterQueueFull(&g_sim);
	r.build_pending = waterPending(&g_sim);

	// Then let the simulation catch up, exactly as those 20 TPS would while the player builds.
	// Every queued candidate is a stone or air cell with no water anywhere near it, so this
	// changes nothing — build_flow below is the check that says so — and it leaves the pour as
	// the only thing the numbers after it describe.
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	r.build_flow = waterFlowCells(&g_sim);
	const uint32_t q_full_base = waterQueueFull(&g_sim);

	printf("         build: %u refused, %d pending, then drained to %d pending / %d flow cells\n",
	       r.build_q_full, r.build_pending, waterPending(&g_sim), r.build_flow);

	// Two controls. Neither reads the spread rule at all, so both stay green in the corner arm,
	// in the centre arm, and in a sabotaged build — which is what makes the red ones below
	// evidence rather than noise.
	CHECK(worldGet(&g_world, BASIN_VCX + 8, BASIN_VY, BASIN_VCZ + 8) == (BlockId)BLOCK_STONE,
	      "control: the pad rim outside the basin is stone");
	CHECK(worldGet(&g_world, BASIN_VCX, BASIN_VY - 7, BASIN_VCZ) == (BlockId)BLOCK_STONE,
	      "control: the shaft has a stone floor at vy-7");
	CHECK(r.build_flow == 0, "control: the fixture build left no water behind, only stone and air");

	CHECK(worldSet(&g_world, sx, BASIN_VY, sz, BLOCK_WATER), "the single source block is placed");
	CHECK(waterLevelAt(&g_sim, &g_world, sx, BASIN_VY, sz) == WATER_LEVEL_SOURCE,
	      "control: it reads as a source (level 8)");

	// No hand call. The place went through worldSet, so the edit hook queued the cell and its
	// six neighbours the way it does on the console. This is the check that goes red the instant
	// anyone unwires the hook again, and it goes red BEFORE any outcome check does — which is
	// the whole point: an outcome check cannot see who queued the work, only that it happened.
	CHECK(waterPending(&g_sim) == 7,
	      "the edit hook alone queued the source and its six neighbours");

	r.ticks      = waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	r.flow_cells = waterFlowCells(&g_sim);

	printf("         settled in %d ticks (-1 would mean still busy at 20000), %d pending, "
	       "%d flow cells\n", r.ticks, waterPending(&g_sim), r.flow_cells);

	puts("         basin plane y = vy, level per cell (rows dz = -3..3, cols dx = -3..3):");
	for (int dz = -3; dz <= 3; dz++) {
		printf("          ");
		for (int dx = -3; dx <= 3; dx++) {
			const uint8_t l =
			    waterLevelAt(&g_sim, &g_world, BASIN_VCX + dx, BASIN_VY, BASIN_VCZ + dz);
			printf("%3u", (unsigned)l);
			if (l > 0) r.basin_wet++;
			if ((int)l > r.basin_max) r.basin_max = (int)l;
		}
		printf("\n");
	}

	for (int y = BASIN_VY - 6; y <= BASIN_VY - 1; y++)
		for (int dz = -1; dz <= 1; dz++)
			for (int dx = -1; dx <= 1; dx++)
				if (worldGet(&g_world, BASIN_VCX + dx, y, BASIN_VCZ + dz) ==
				    (BlockId)BLOCK_WATER)
					r.shaft_wet++;

	puts("         shaft floor y = vy-6, level per cell (rows dz = -1..1, cols dx = -1..1):");
	for (int dz = -1; dz <= 1; dz++) {
		printf("          ");
		for (int dx = -1; dx <= 1; dx++) {
			const uint8_t l = waterLevelAt(&g_sim, &g_world, BASIN_VCX + dx,
			                               BASIN_VY - 6, BASIN_VCZ + dz);
			printf("%3u", (unsigned)l);
			if (l > 0) r.pool_cells++;
			if ((int)l > r.pool_max) r.pool_max = (int)l;
		}
		printf("\n");
	}
	printf("         %d water blocks in the shaft (vy-6..vy-1), %d on its floor, max level %d\n",
	       r.shaft_wet, r.pool_cells, r.pool_max);

	CHECK(r.ticks >= 0, "the arm reaches a stable state inside 20000 ticks");
	CHECK(waterPending(&g_sim) == 0, "nothing is left queued once it has settled");
	CHECK(waterMapFull(&g_sim) == 0, "no flow cell was ever refused a map slot");
	// Against the base taken after the build, not against zero: q_full is cumulative for the
	// life of the sim, and this assertion has always been about the POUR. Written as a delta it
	// keeps meaning exactly what it used to mean while the fixture build is judged separately.
	CHECK(waterQueueFull(&g_sim) == q_full_base,
	      "no candidate was refused a ring slot during the pour itself");
	return r;
}

static void testSunkenBasin(void)
{
	puts("water: a sunken basin — a corner source spreads, a source over the shaft does not");

	// ARM 1 — the corner. Stone directly under it, so it cannot go down, so it feeds sideways.
	const BasinArm corner = basinArm("corner", BASIN_VCX + 3, BASIN_VCZ + 3);

	// The build burst, pinned as bare literals so it cannot drift silently. 6069 stone cells laid
	// one worldSet at a time with the hook live and no tick between them: the ring saturates at
	// WATERQ_CAP and 7425 further candidates are lost to it.
	//
	// v1.8.3 CORRECTS TWO CLAIMS THAT USED TO STAND HERE, both of them measured false on
	// 2026-08-25.
	//
	// It said the number was 38771. It was, while qPush refused the newest candidate; qPush now
	// evicts the oldest instead (see water.c), and the same 6069-cell build loses 7425.
	//
	// And it said this burst was "a fixture artefact ... nothing in the game installs terrain
	// through per-block worldSet". That is wrong, and it is the important half. worldgen installs
	// TERRAIN through worldSetChunkAll, which is where the claim came from, but its DECORATION
	// pass places trees and tall grass one block at a time through worldSet (worldgen.c:339,
	// worldgen.c:458) — and world.c's edit hook is a file static that fires for any World, so
	// app/worker.c's generation thread drives this queue while it builds the staging world.
	// Measured against the density generator at seed 12345: 8590 firings over 144 columns, worst
	// single column 221, offering up to 1547 candidates to a ring of 1024. A source placed by the
	// player after streaming failed to spread in 16 of 200 swept arms under the old policy.
	//
	// So the burst is not an artefact, the game reaches this state, and what used to follow from
	// it — "its own notify is refused and the water never moves", a lone unspread source cube from
	// a simulation reporting perfect health — was a live defect and not a hypothetical.
	// testOverflowKeepsTheNewest is the arm that now holds it shut.
	CHECK(corner.build_q_full == 7425, "the basin build loses 7425 candidates with the hook live");
	CHECK(corner.build_pending == 1024, "and leaves the ring saturated at WATERQ_CAP");
	CHECK((int)WATERQ_CAP == 1024, "control: WATERQ_CAP is still the 1024 those two numbers assume");

	CHECK(corner.basin_wet > 1,
	      "the corner source does NOT sit alone: it wets more than its own cell");
	// 31 and not 49: the far side of the basin is in the SHADOW of the shaft mouth. A mouth cell
	// that fills can go down, so it feeds nothing sideways, and the spread stops dead at the
	// mouth instead of crossing it. That shadow is visible in the printed grid as the block of
	// zeros in the dz <= 0, dx <= 0 quadrant, and it is the same rule as the centre arm below.
	CHECK(corner.basin_wet == 31, "31 of the 49 basin cells are wet; the shaft mouth shadows the rest");
	CHECK(corner.flow_cells == 64, "64 flow cells in the world");
	CHECK(corner.basin_max == WATER_LEVEL_SOURCE, "the source itself is still level 8");
	CHECK(corner.shaft_wet == 34, "34 water blocks in the shaft: it reached it and fell");
	CHECK(corner.pool_cells == 9, "and pooled across all 9 cells of the shaft floor");
	CHECK(corner.pool_max == WATER_LEVEL_MAX, "the landing cell of the fall is level 7");

	// ARM 2 — the centre, directly over the shaft mouth. water.c's sideFeed: water that can
	// still go DOWN does not also go sideways, and the cell under the centre is air. So this is
	// the arm that stays a lone cube in the basin plane, and it is the rule working.
	const BasinArm centre = basinArm("centre", BASIN_VCX, BASIN_VCZ);

	CHECK(centre.basin_wet == 1,
	      "the centre source is the one that sits alone: 1 wet cell in the basin plane");
	CHECK(centre.basin_max == WATER_LEVEL_SOURCE, "and that one cell is the source");
	CHECK(centre.shaft_wet == 14, "it went straight down instead: 14 water blocks in the shaft");
	CHECK(centre.pool_cells == 9, "pooling across all 9 cells of the shaft floor");
	CHECK(corner.basin_wet > centre.basin_wet,
	      "the two arms really do differ, and it is the CORNER that spreads");
}

// ── The pad-and-shaft build ──────────────────────────────────────────────────────────
//
// The OTHER arrangement, the one built in the emulator that showed a lone unspread cube after
// twelve seconds with the renderer's triangle count byte-identical before and after. Same
// shaft, but the ground is a flat pad rather than a sunken basin and the source sits ON TOP of
// the pad instead of down in it:
//
//   * a 17x17 stone pad at y = vy, air at vy+1 .. vy+12 above it
//   * a 3x3 shaft carved straight THROUGH the pad: air at vy-0 .. vy-6 (the vy-0 layer is what
//     opens the mouth in the pad itself), stone floor at vy-7
//   * ONE source at (vcx-3, vy+1, vcz-3) — one block ABOVE the pad, with pad stone underneath
#define PAD_VCX  (-6)
#define PAD_VY   64
#define PAD_VCZ  9

static void buildPad(void)
{
	// Step 1 — the flat pad, and the air column over it.
	fillBox(PAD_VCX - 8, PAD_VCX + 8, PAD_VY, PAD_VY,
	        PAD_VCZ - 8, PAD_VCZ + 8, BLOCK_STONE);
	fillBox(PAD_VCX - 8, PAD_VCX + 8, PAD_VY + 1, PAD_VY + 12,
	        PAD_VCZ - 8, PAD_VCZ + 8, BLOCK_AIR);

	// Step 2 — the shaft, carved through the pad. h = 0 is the mouth: it removes pad stone.
	fillBox(PAD_VCX - 1, PAD_VCX + 1, PAD_VY - 6, PAD_VY,
	        PAD_VCZ - 1, PAD_VCZ + 1, BLOCK_AIR);
	fillBox(PAD_VCX - 1, PAD_VCX + 1, PAD_VY - 7, PAD_VY - 7,
	        PAD_VCZ - 1, PAD_VCZ + 1, BLOCK_STONE);
}

typedef struct {
	int ticks;
	int flow_cells;
	int pad_wet;      // cells with a level > 0 in the 17x17 pad plane at y = vy+1
	int pad_max;      // highest level in that plane
	int mouth_wet;    // wet cells of the 3x3 shaft mouth plane at y = vy
	int shaft_wet;    // water blocks in the 3x3 shaft between vy-6 and vy-1
	int pool_cells;   // water blocks on the shaft floor at vy-6
	int pool_max;     // highest level on that floor
	int spill_col;    // water blocks in the column one step OUTSIDE the pad rim, y = 0 .. vy
	uint32_t build_q_full;   // candidates the fixture BUILD alone had refused, hook live
	int      build_pending;  // still queued when the build finished, before any tick
	int      build_flow;     // flow cells the build left behind — must be zero, it is all stone
} PadArm;

static PadArm padArm(const char* label, int sx, int sy, int sz)
{
	PadArm r;
	memset(&r, 0, sizeof r);

	printf("  -- %s source, one BLOCK_WATER at (%d, %d, %d) --\n", label, sx, sy, sz);

	resetWorldWired();
	buildPad();

	// Same as basinArm: the fixture is 3829 worldSet calls with the game's hook live and no tick
	// between any of them, which is a burst no in-game path produces. Measured, asserted on by
	// testPadAndShaft, and then drained before the pour so the pour's own numbers stay the
	// pour's own numbers.
	r.build_q_full  = waterQueueFull(&g_sim);
	r.build_pending = waterPending(&g_sim);
	(void)waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	r.build_flow = waterFlowCells(&g_sim);
	const uint32_t q_full_base = waterQueueFull(&g_sim);

	printf("         build: %u refused, %d pending, then drained to %d pending / %d flow cells\n",
	       r.build_q_full, r.build_pending, waterPending(&g_sim), r.build_flow);

	// Controls. None of them reads the spread rule, so all stay green here and in a sabotaged
	// build — which is what makes the red checks below evidence rather than noise.
	CHECK(worldGet(&g_world, PAD_VCX + 8, PAD_VY, PAD_VCZ + 8) == (BlockId)BLOCK_STONE,
	      "control: the pad rim is stone");
	CHECK(worldGet(&g_world, PAD_VCX, PAD_VY, PAD_VCZ) == (BlockId)BLOCK_AIR,
	      "control: the shaft mouth is open through the pad");
	CHECK(worldGet(&g_world, sx, sy - 1, sz) == (BlockId)BLOCK_STONE,
	      "control: the source has pad stone directly under it");
	CHECK(r.build_flow == 0, "control: the fixture build left no water behind, only stone and air");

	CHECK(worldSet(&g_world, sx, sy, sz, BLOCK_WATER), "the single source block is placed");
	CHECK(waterLevelAt(&g_sim, &g_world, sx, sy, sz) == WATER_LEVEL_SOURCE,
	      "control: it reads as a source (level 8)");

	// No hand call — see basinArm. The hook is what queues the source, exactly as on the console.
	CHECK(waterPending(&g_sim) == 7,
	      "the edit hook alone queued the source and its six neighbours");

	r.ticks      = waterSettle(&g_sim, &g_world, 20000, NULL, NULL);
	r.flow_cells = waterFlowCells(&g_sim);

	printf("         settled in %d ticks, %d pending, %d flow cells\n",
	       r.ticks, waterPending(&g_sim), r.flow_cells);

	puts("         pad plane y = vy+1, level per cell (rows dz = -8..8, cols dx = -8..8):");
	for (int dz = -8; dz <= 8; dz++) {
		printf("          ");
		for (int dx = -8; dx <= 8; dx++) {
			const uint8_t l = waterLevelAt(&g_sim, &g_world,
			                               PAD_VCX + dx, PAD_VY + 1, PAD_VCZ + dz);
			printf("%3u", (unsigned)l);
			if (l > 0) r.pad_wet++;
			if ((int)l > r.pad_max) r.pad_max = (int)l;
		}
		printf("\n");
	}

	puts("         mouth plane y = vy, level per cell (rows dz = -1..1, cols dx = -1..1):");
	for (int dz = -1; dz <= 1; dz++) {
		printf("          ");
		for (int dx = -1; dx <= 1; dx++) {
			const uint8_t l = waterLevelAt(&g_sim, &g_world,
			                               PAD_VCX + dx, PAD_VY, PAD_VCZ + dz);
			printf("%3u", (unsigned)l);
			if (l > 0) r.mouth_wet++;
		}
		printf("\n");
	}

	for (int y = PAD_VY - 6; y <= PAD_VY - 1; y++)
		for (int dz = -1; dz <= 1; dz++)
			for (int dx = -1; dx <= 1; dx++)
				if (worldGet(&g_world, PAD_VCX + dx, y, PAD_VCZ + dz) ==
				    (BlockId)BLOCK_WATER)
					r.shaft_wet++;

	puts("         shaft floor y = vy-6, level per cell (rows dz = -1..1, cols dx = -1..1):");
	for (int dz = -1; dz <= 1; dz++) {
		printf("          ");
		for (int dx = -1; dx <= 1; dx++) {
			const uint8_t l = waterLevelAt(&g_sim, &g_world, PAD_VCX + dx,
			                               PAD_VY - 6, PAD_VCZ + dz);
			printf("%3u", (unsigned)l);
			if (l > 0) r.pool_cells++;
			if ((int)l > r.pool_max) r.pool_max = (int)l;
		}
		printf("\n");
	}
	// The pad is a one-block slab with nothing under it, so the spread runs off all four rims
	// and falls into the void. Measured on the source's own z row, one step past the rim.
	for (int y = 0; y <= PAD_VY; y++)
		if (worldGet(&g_world, PAD_VCX - 9, y, PAD_VCZ - 3) == (BlockId)BLOCK_WATER)
			r.spill_col++;

	printf("         pad_wet %d (max %d), mouth_wet %d, shaft %d, floor %d (max %d), "
	       "rim spill column %d\n",
	       r.pad_wet, r.pad_max, r.mouth_wet, r.shaft_wet, r.pool_cells, r.pool_max,
	       r.spill_col);

	CHECK(r.ticks >= 0, "the arm reaches a stable state inside 20000 ticks");
	CHECK(waterPending(&g_sim) == 0, "nothing is left queued once it has settled");
	CHECK(waterMapFull(&g_sim) == 0, "no flow cell was ever refused a map slot");
	// A delta, for the same reason basinArm's is: cumulative counter, assertion about the pour.
	CHECK(waterQueueFull(&g_sim) == q_full_base,
	      "no candidate was refused a ring slot during the pour itself");
	return r;
}

static void testPadAndShaft(void)
{
	puts("water: a source ON TOP of a flat pad with a shaft cut through it");

	const PadArm pad = padArm("on-pad", PAD_VCX - 3, PAD_VY + 1, PAD_VCZ - 3);

	// The build burst, bare literals, and the number that matters is 974. The pad fixture built
	// with the game's wiring and no tick in between leaves 974 candidates pending against a ring
	// of 1024 — FIFTY slots of headroom, 95.1% full, and nothing refused. Before the hook was
	// wired this whole line was 0 pending and 0 refused, which is why the "no candidate was ever
	// refused" check below used to be true by construction. Pin both halves: the refusal count
	// AND the depth, because a change that only moves the depth is invisible in the count until
	// the day it crosses 1024, and on that day the source's own notify is the one that is refused.
	CHECK(pad.build_q_full == 0, "the pad build refuses nothing: 974 candidates fit in 1024 slots");
	CHECK(pad.build_pending == 974, "and 974 is what it leaves pending, 50 short of WATERQ_CAP");
	CHECK((int)WATERQ_CAP - pad.build_pending == 50, "50 slots of headroom, stated as a number");

	// The headline, and it contradicts what the emulator showed: this source DOES spread. The
	// cell under it is pad stone, so sideFeed hands out its level, and the manhattan ball runs
	// out to radius 7 across the top of the pad exactly as testFlatPour's does on the ground.
	CHECK(pad.pad_wet > 1, "the on-pad source does NOT sit alone: it wets more than its own cell");
	CHECK(pad.pad_wet == 102, "102 cells of the pad plane at vy+1 are wet");
	CHECK(pad.pad_max == WATER_LEVEL_SOURCE, "the source itself is still level 8");
	CHECK(pad.flow_cells == 1558, "1558 flow cells in the world");
	CHECK(pad.ticks > 0 && pad.ticks < 1000, "and it settles quickly rather than churning");

	// The shaft-mouth shadow, the same rule testSunkenBasin's corner arm shows: a pad-plane cell
	// that sits over the open mouth CAN go down, so it feeds nothing sideways, and the spread
	// stops at the near lip instead of crossing the hole.
	CHECK(waterLevelAt(&g_sim, &g_world, PAD_VCX, PAD_VY + 1, PAD_VCZ) == 0,
	      "the pad cell directly over the mouth centre is dry: the mouth shadows it");
	CHECK(waterLevelAt(&g_sim, &g_world, PAD_VCX - 1, PAD_VY + 1, PAD_VCZ) > 0,
	      "but the cell on the near lip of the mouth is wet");

	// It reaches the shaft, falls the full six blocks and pools on the floor.
	CHECK(pad.mouth_wet == 5, "5 of the 9 mouth cells at vy take water");
	CHECK(pad.shaft_wet == 34, "34 water blocks in the shaft (vy-6 .. vy-1): it fell");
	CHECK(pad.pool_cells == 9, "and pooled across all 9 cells of the shaft floor");
	CHECK(pad.pool_max == WATER_LEVEL_MAX, "the landing cell of the fall is level 7");

	// The pad is a slab in mid-air in this fixture, so the ball also runs off all four rims.
	// That, not the shaft, is where most of those 1558 flow cells are.
	CHECK(pad.spill_col > 1,
	      "water runs off the pad rim and falls: the column just outside it is wet");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== water test ==");

	worldInit(&g_world);
	waterInit(&g_sim);

	testFlatPour();
	testWaterfall();
	testDrainage();
	testOrderIndependent();
	testSpreadRate();
	testSpreadRateIsTicksNotBudget();
	testSettledIsFree();
	testChangeHook();
	testStaleEntry();
	testEditHook();
	testEditHookReportsWorld();
	testDropColumn();
	testSaveCompat();
	testQueueOverflow();
	testOverflowKeepsTheNewest();
	testNoOrphanedDedupKeys();
	testSunkenBasin();
	testPadAndShaft();
	testTickCost();

	worldExit(&g_world);

	// v1.8.3. The suite counts itself, and no other suite in this project does — which is exactly
	// how a 326 -> 318 deletion once passed as "0 failed" here. A red arm whose TOTAL dropped is
	// deleting checks, not failing them, and nothing in the PASS line below can tell the two
	// apart on its own. 219 is every check above this one; this check is the 220th.
	//
	// v1.8.3: 203 -> 219. All sixteen are testNoOrphanedDedupKeys, new below
	// testOverflowKeepsTheNewest: four arms — a pour stopped mid-flight, the eviction path, a
	// fully drained simulation and a column drop — each pinning that the dedup set holds exactly
	// one key per queued cell and that no key has lost its ring entry, plus the four controls that
	// stop each arm passing on an empty ring, and wiredBuildDone's own three-check burst pin. It is the only probe in this file that reads the two
	// tables against each other instead of asking what the water did, and it is there because a
	// desynchronisation between them is invisible to every accessor and to every world hash: see
	// the sabotage measurement in its header.
	//
	// v1.8.3: 196 -> 203. All seven are testEditHookReportsWorld, new below testEditHook: three
	// arm-1 checks that an edit to the live world is reported as the live world and admitted,
	// three arm-2 checks that an edit to a second world is reported as that world and dropped,
	// and one control that reads both writes back so arm 2 cannot pass on a write that never
	// landed. That probe is the one that goes red if world.c stops passing the World* through.
	//
	// v1.8.3: 161 -> 196. Twenty-one of the thirty-five are wiredBuildDone's three-check burst pin
	// firing at each of its seven call sites; ten are testOverflowKeepsTheNewest, new below
	// testQueueOverflow; the remaining four are the "the edit hook alone queued the source and its
	// six neighbours" checks added to the four probes that place a source through worldSet, plus
	// testSaveCompat's new "loading a world queues nothing at all".
	//
	// It is deliberately a naked literal and not a symbol: a count expressed in terms of anything
	// the file computes would move with the file, which is the whole failure mode it exists to
	// catch. When a check is legitimately added or removed, this number is edited by hand, on
	// purpose, in the same commit — that edit IS the review.
	const int counted = g_checks;
	CHECK(counted == 219, "the suite ran all 219 of its checks: none was deleted or skipped");

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
