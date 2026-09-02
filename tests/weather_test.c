// Host tests for world/weather.c / world/weather.h (v1.8.9 task, the weather half of
// "Sky and weather").
//
// WHAT THIS FILE PROVES, IN ORDER.
//
//   1. The constants weather.h documents are what actually ship (naked-literal pins).
//   2. weatherNextSurfaceBlock, the pure accumulation/melt rule, matches a hand-computed
//      table at every boundary this file's own header claims matters (one tick short of a
//      threshold vs exactly at it), and can never propose a second layer of snow or overwrite
//      a block it did not itself place.
//   3. weatherAt is a REPEATABLE, DETERMINISTIC function of its arguments -- called twice with
//      identical inputs it gives the identical answer, and two INDEPENDENTLY initialised
//      WorldGen instances built from the same raw seed agree with each other. That second half
//      is the multiplayer determinism claim weather.h's header makes, tested directly rather
//      than assumed.
//   4. weatherAt decides per LOCATION, not with one global flag: swept across many weather
//      cells at one fixed tick, some are clear and some are not; swept across many ticks at one
//      fixed location, the answer changes as fronts pass.
//   5. weatherAt is biome-aware using REAL biome data from a REAL WorldGen: searched, found
//      locations of every one of the six biomes, and confirmed cold biomes only ever produce
//      WEATHER_SNOW when precipitating and every other biome only ever produces WEATHER_RAIN --
//      never the other kind, at any of many sampled episodes.
//   6. Desert's own, separate, lower precipitation chance is actually being read -- measured
//      over enough episodes that a swapped constant could not hide.
//   7. Nothing truncates at a huge absolute tick (5*10^12, roughly 7,900 years of uptime at
//      TICK_HZ 20) -- the episode-boundary arithmetic is checked against an independently
//      hand-computed value (long division, in a comment) rather than against the code's own
//      expression.
//   8. weatherStepCell, integrated against a real World, is safe against every edit-since-
//      worldgen case weather.h's header names: it will not place snow before the accumulation
//      threshold, will not float snow over ground that has been mined away, will not overwrite
//      a block a player placed, and correctly melts existing snow back to air after the melt
//      threshold -- and never places a second layer once one is already there.
//   9. weatherTickColumn's 4-cells-per-visit sweep genuinely covers a whole column given
//      enough calls, with no gaps and no cell double-counted: 64 calls at tick residues
//      stepping by 4 across the full 0..255 cycle touch and change all 256 of a column's
//      surface cells exactly once.
//
// Nothing here restates weather.c's own arithmetic back at it: every expected value in every
// table below was worked out by hand from weather.h's documented contract before this file was
// run against the real implementation, exactly as world/tick_test.c's own header describes for
// itself.
//
// The __3DS__ guard is load-bearing, not tidy -- the console Makefile globs every .c file, and
// this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/genversion.h"
#include "world/budget.h"
#include "world/weather.h"

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

// How many check() calls this suite makes on a healthy tree. A LITERAL on purpose -- see
// world/tick_test.c's own comment on TICK_TEST_EXPECTED_CHECKS for the full reasoning this
// pin exists for for; not repeated here.
//
// Legitimately adding or removing a check means editing this by hand.
#define WEATHER_TEST_EXPECTED_CHECKS 46

static void checkCountPin(void)
{
	if (g_checks == WEATHER_TEST_EXPECTED_CHECKS)
		return;

	g_fails++;
	if (g_checks < WEATHER_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "         WEATHER_TEST_EXPECTED_CHECKS to go green.\n",
		       WEATHER_TEST_EXPECTED_CHECKS - g_checks, WEATHER_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If on purpose, set WEATHER_TEST_EXPECTED_CHECKS to %d.\n",
		       g_checks - WEATHER_TEST_EXPECTED_CHECKS, WEATHER_TEST_EXPECTED_CHECKS, g_checks,
		       g_checks);
}

// ── shared search helpers ─────────────────────────────────────────────────────────────────
//
// Real terrain, not a fake generator: every helper below is driven by a real WorldGen through
// its public, documented API (worldgenBiomeAt, weatherAt), the same rule weather.h's own header
// insists on -- "ALWAYS pass the live WorldGen*, never a seed".

// First real (x, z) in a stride-spaced square scan (row-major, -half..+half on each axis) whose
// biome is `want`. False if none was found in the scanned square -- callers must check the
// return, never assume the search succeeded.
static bool findBiome(const WorldGen *g, BiomeId want, int32_t half_extent, int32_t stride,
                       int32_t *out_x, int32_t *out_z)
{
	for (int32_t z = -half_extent; z <= half_extent; z += stride) {
		for (int32_t x = -half_extent; x <= half_extent; x += stride) {
			if (worldgenBiomeAt(g, x, z) == want) {
				*out_x = x;
				*out_z = z;
				return true;
			}
		}
	}
	return false;
}

// First episode in [0, max_episode] at which weatherAt(g, episode << WEATHER_EPISODE_SHIFT,
// x, z) equals `want`. False if none was found.
static bool findEpisodeWithKind(const WorldGen *g, int32_t x, int32_t z, WeatherKind want,
                                 uint64_t max_episode, uint64_t *out_episode)
{
	for (uint64_t e = 0; e <= max_episode; e++) {
		if (weatherAt(g, e << WEATHER_EPISODE_SHIFT, x, z) == want) {
			*out_episode = e;
			return true;
		}
	}
	return false;
}

// ── 1: constants, as naked literals ─────────────────────────────────────────────────────────
//
// Nothing else in the tree pins these. weather.h's own _Static_asserts protect
// WEATHER_EPISODE_TICKS and the two threshold-vs-episode-length relationships against being
// INTERNALLY inconsistent, not against moving to a different but still-consistent value.
static void testWeatherConstantsPinned(void)
{
	check(WEATHER_CELL_SHIFT == 7,          "WEATHER_CELL_SHIFT is 7 (128 blocks, the same "
	                                         "scale as worldgen's own biome field)");
	check(WEATHER_EPISODE_SHIFT == 13,      "WEATHER_EPISODE_SHIFT is 13");
	check(WEATHER_EPISODE_TICKS == 8192u,   "WEATHER_EPISODE_TICKS is 8192 (2^13)");
	check(WEATHER_SNOW_ACCUM_TICKS == 2400u,"WEATHER_SNOW_ACCUM_TICKS is 2400 (120 real seconds "
	                                         "at TICK_HZ 20)");
	check(WEATHER_MELT_TICKS == 1200u,      "WEATHER_MELT_TICKS is 1200 (60 real seconds)");
	check(WEATHER_CHANCE_PRECIP == 64u,     "WEATHER_CHANCE_PRECIP is 64 (out of 256, 25%)");
	check(WEATHER_CHANCE_DESERT == 8u,      "WEATHER_CHANCE_DESERT is 8 (out of 256, ~3%)");
	check(WEATHER_CELLS_PER_VISIT == 4,     "WEATHER_CELLS_PER_VISIT is 4");
}

// ── 2: the control ───────────────────────────────────────────────────────────────────────────
//
// Touches worldgenInit and weatherAt's basic repeatability and nothing else, so a break in the
// accumulation rule, the biome-species mapping or the World integration must leave it standing.
static void testControlWeatherAtRepeats(void)
{
	WorldGen g;
	bool ok = worldgenInit(&g, 12345u, GEN_VERSION_BIOME);
	check(ok, "control: worldgenInit succeeds for seed 12345 at GEN_VERSION_BIOME");

	WeatherKind k1 = weatherAt(&g, 0, 0, 0);
	WeatherKind k2 = weatherAt(&g, 0, 0, 0);
	check(k1 == k2, "control: weatherAt(g, 0, 0, 0) gives the same answer called twice");
}

// ── 3: weatherNextSurfaceBlock, table-driven ────────────────────────────────────────────────
//
// Every row hand-computed from weather.h's own stated rule (quoted in weather.h's comment on
// this function) before this file was run. NOT derived from weather.c.
typedef struct {
	WeatherKind kind;
	uint64_t    ticks_into_episode;
	BlockId     current;
	BlockId     want;
	const char *what;
} AccumRow;

static const AccumRow k_accum_rows[] = {
	{ WEATHER_SNOW, 0,                              BLOCK_AIR,   BLOCK_AIR,
	  "snowing, 0 ticks in, air: too soon, stays air" },
	{ WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS - 1,    BLOCK_AIR,   BLOCK_AIR,
	  "snowing, one tick short of the accumulation threshold, air: still air" },
	{ WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS,        BLOCK_AIR,   BLOCK_SNOW,
	  "snowing, exactly at the accumulation threshold, air: snow appears" },
	{ WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS + 1000, BLOCK_AIR,   BLOCK_SNOW,
	  "snowing, well past the threshold, air: snow appears" },
	{ WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS,        BLOCK_SNOW,  BLOCK_SNOW,
	  "snowing, past the threshold, ALREADY snow: stays exactly one block -- no second layer" },
	{ WEATHER_RAIN, WEATHER_MELT_TICKS,               BLOCK_SNOW,  BLOCK_AIR,
	  "raining, exactly at the melt threshold, snow: melts to air" },
	{ WEATHER_CLEAR,WEATHER_MELT_TICKS,               BLOCK_SNOW,  BLOCK_AIR,
	  "clear (not just raining), at the melt threshold, snow: also melts" },
	{ WEATHER_RAIN, WEATHER_MELT_TICKS - 1,           BLOCK_SNOW,  BLOCK_SNOW,
	  "raining, one tick short of the melt threshold, snow: still snow" },
	{ WEATHER_RAIN, 0,                                BLOCK_SNOW,  BLOCK_SNOW,
	  "just started raining, snow: not melted yet" },
	{ WEATHER_SNOW, 0,                                BLOCK_SNOW,  BLOCK_SNOW,
	  "still snowing, already snow: unchanged (melt branch does not apply while it is snowing)" },
	{ WEATHER_RAIN, WEATHER_SNOW_ACCUM_TICKS + 1000,  BLOCK_AIR,   BLOCK_AIR,
	  "raining (not snowing), air, even well past the accumulation tick count: never becomes "
	  "snow" },
	{ WEATHER_CLEAR,0,                                BLOCK_AIR,   BLOCK_AIR,
	  "clear, air: no-op" },
	{ WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS,         BLOCK_STONE, BLOCK_STONE,
	  "snowing, past the threshold, but the cell holds STONE (a player placement): never "
	  "overwritten" },
	{ WEATHER_RAIN, WEATHER_MELT_TICKS,               BLOCK_STONE, BLOCK_STONE,
	  "raining, past the melt threshold, but the cell holds STONE: never overwritten" },
};
#define ACCUM_ROW_COUNT ((int)(sizeof k_accum_rows / sizeof k_accum_rows[0]))

static void testWeatherNextSurfaceBlockTable(void)
{
	int wrong = 0, snow_results = 0, air_results = 0, other_results = 0;

	for (int r = 0; r < ACCUM_ROW_COUNT; r++) {
		const AccumRow *row = &k_accum_rows[r];
		BlockId got = weatherNextSurfaceBlock(row->kind, row->ticks_into_episode, row->current);
		if (got != row->want) wrong++;
		if (got == BLOCK_SNOW) snow_results++;
		else if (got == BLOCK_AIR) air_results++;
		else other_results++;
	}

	check(wrong == 0,
	      "weatherNextSurfaceBlock matches all 14 hand-computed table rows");
	check(snow_results >= 1 && air_results >= 1 && other_results >= 1,
	      "falsifiability: the table's expected results include BLOCK_SNOW, BLOCK_AIR and an "
	      "unchanged non-air/non-snow value -- not one constant answer");

	// Explicit boundary checks, spelled with WEATHER_SNOW_ACCUM_TICKS / WEATHER_MELT_TICKS
	// rather than the naked numbers the table above already used, so a change to either
	// constant is checked against ITSELF rather than only against a copy of its current value.
	check(weatherNextSurfaceBlock(WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS - 1, BLOCK_AIR)
	      == BLOCK_AIR,
	      "one tick short of WEATHER_SNOW_ACCUM_TICKS: still air");
	check(weatherNextSurfaceBlock(WEATHER_SNOW, WEATHER_SNOW_ACCUM_TICKS, BLOCK_AIR)
	      == BLOCK_SNOW,
	      "exactly at WEATHER_SNOW_ACCUM_TICKS: snow appears");
	check(weatherNextSurfaceBlock(WEATHER_RAIN, WEATHER_MELT_TICKS - 1, BLOCK_SNOW)
	      == BLOCK_SNOW,
	      "one tick short of WEATHER_MELT_TICKS: still snow");
	check(weatherNextSurfaceBlock(WEATHER_RAIN, WEATHER_MELT_TICKS, BLOCK_SNOW)
	      == BLOCK_AIR,
	      "exactly at WEATHER_MELT_TICKS: snow melts");

	check(weatherNextSurfaceBlock(WEATHER_SNOW, 999999999ull, BLOCK_SNOW) == BLOCK_SNOW,
	      "an already-snow cell asked again after a huge tick count stays exactly one block of "
	      "snow -- there is no second layer to add");
}

// ── 4: determinism ───────────────────────────────────────────────────────────────────────────
//
// A plain LCG for a repeatable pseudo-random sweep, the same idiom world/tick_test.c's
// testTickClockInvariantsOverAMixedStream uses for its own mixed stream.
static void testWeatherAtDeterministic(void)
{
	WorldGen g1, g2;
	check(worldgenInit(&g1, 987654321u, GEN_VERSION_BIOME)
	      && worldgenInit(&g2, 987654321u, GEN_VERSION_BIOME),
	      "two independent WorldGen instances both init from the same raw seed 987654321");

	uint32_t lcg = 20260901u;
	int repeat_wrong = 0, cross_wrong = 0;
	int clear_seen = 0, nonclear_seen = 0;

	for (int i = 0; i < 200; i++) {
		lcg = lcg * 1103515245u + 12345u;
		int32_t x = (int32_t)(lcg % 20000u) - 10000;
		lcg = lcg * 1103515245u + 12345u;
		int32_t z = (int32_t)(lcg % 20000u) - 10000;
		lcg = lcg * 1103515245u + 12345u;
		uint64_t tick = (uint64_t)(lcg % 2000000u);

		WeatherKind a1 = weatherAt(&g1, tick, x, z);
		WeatherKind a2 = weatherAt(&g1, tick, x, z);
		if (a1 != a2) repeat_wrong++;

		WeatherKind b1 = weatherAt(&g2, tick, x, z);
		if (a1 != b1) cross_wrong++;

		if (a1 == WEATHER_CLEAR) clear_seen++; else nonclear_seen++;
	}

	check(repeat_wrong == 0,
	      "weatherAt gives the identical WeatherKind called twice with identical arguments, "
	      "across 200 pseudo-random (x, z, tick) triples");
	check(cross_wrong == 0,
	      "two independently-initialised WorldGen instances built from the SAME raw seed agree "
	      "on weatherAt at every one of the 200 triples -- the multiplayer determinism claim in "
	      "weather.h, tested directly");
	check(clear_seen > 0 && nonclear_seen > 0,
	      "falsifiability: the 200 triples produced both WEATHER_CLEAR and a precipitating kind, "
	      "so the agreement above is not trivially true of a function that always answers the "
	      "same way");
}

// ── 5: per-location, not global ─────────────────────────────────────────────────────────────
static void testWeatherAtPerLocationNotGlobal(void)
{
	WorldGen g;
	worldgenInit(&g, 555111u, GEN_VERSION_BIOME);

	// Sweep many weather CELLS (spaced well apart, a multiple of the 128-block cell size) at
	// one fixed tick. A single global on/off flag would make every one of these agree; real
	// per-cell fronts should not.
	int clear_cells = 0, nonclear_cells = 0;
	for (int32_t c = -40; c <= 40; c++) {
		WeatherKind k = weatherAt(&g, 4096, c * (1 << WEATHER_CELL_SHIFT), 0);
		if (k == WEATHER_CLEAR) clear_cells++; else nonclear_cells++;
	}
	check(clear_cells > 0 && nonclear_cells > 0,
	      "sweeping 81 weather cells at one fixed tick: some are clear and some are "
	      "precipitating -- weather is decided per cell, not by one flag for the whole world");

	// Sweep many episodes at ONE fixed location. A classification frozen forever would make
	// every one of these agree too.
	int distinct_over_time = 0;
	WeatherKind first = weatherAt(&g, 0, 12345, -6789);
	for (uint64_t e = 1; e < 64; e++) {
		if (weatherAt(&g, e << WEATHER_EPISODE_SHIFT, 12345, -6789) != first) {
			distinct_over_time++;
		}
	}
	check(distinct_over_time > 0,
	      "sweeping 64 episodes at one fixed location: the classification is not frozen at its "
	      "first answer -- fronts pass and the weather at that location changes over time");
}

// ── 6: biome-aware species selection, on real terrain ──────────────────────────────────────
static void testWeatherAtBiomeSpecies(void)
{
	WorldGen g;
	worldgenInit(&g, 20260901u, GEN_VERSION_BIOME);

	static const BiomeId all_biomes[BIOME_COUNT] = {
		BIOME_TUNDRA, BIOME_TAIGA, BIOME_PLAINS, BIOME_FOREST, BIOME_DESERT, BIOME_JUNGLE,
	};
	int32_t bx[BIOME_COUNT], bz[BIOME_COUNT];
	bool found[BIOME_COUNT];
	int found_count = 0;

	for (int i = 0; i < BIOME_COUNT; i++) {
		found[i] = findBiome(&g, all_biomes[i], 4096, 32, &bx[i], &bz[i]);
		if (found[i]) found_count++;
	}
	check(found_count == BIOME_COUNT,
	      "the search locates a real column of every one of the 6 biomes for seed 20260901 "
	      "within a 4096-block, 32-stride square");

	int wrong = 0, snow_seen = 0, rain_seen = 0;
	for (int i = 0; i < BIOME_COUNT; i++) {
		if (!found[i]) continue;
		bool is_cold = (all_biomes[i] == BIOME_TUNDRA || all_biomes[i] == BIOME_TAIGA);
		for (uint64_t e = 0; e < 256; e++) {
			WeatherKind k = weatherAt(&g, e << WEATHER_EPISODE_SHIFT, bx[i], bz[i]);
			if (k == WEATHER_CLEAR) continue;
			if (k == WEATHER_SNOW) snow_seen++;
			if (k == WEATHER_RAIN) rain_seen++;
			bool ok = is_cold ? (k == WEATHER_SNOW) : (k == WEATHER_RAIN);
			if (!ok) wrong++;
		}
	}
	check(wrong == 0,
	      "every found biome location's weatherAt kind matches the documented rule across 256 "
	      "sampled episodes each -- cold biomes (tundra, taiga) never precipitate as rain, and "
	      "every other biome never precipitates as snow");
	check(snow_seen > 0 && rain_seen > 0,
	      "falsifiability: across the whole sweep, both WEATHER_SNOW and WEATHER_RAIN were "
	      "actually observed -- the rule above is not vacuously true of a sweep that only ever "
	      "saw one kind or only ever saw clear");
}

// ── 7: desert's own, lower chance ───────────────────────────────────────────────────────────
static void testWeatherAtDesertRarerThanPlains(void)
{
	WorldGen g;
	worldgenInit(&g, 314159u, GEN_VERSION_BIOME);

	int32_t desert_x, desert_z, plains_x, plains_z;
	bool have_desert = findBiome(&g, BIOME_DESERT, 4096, 32, &desert_x, &desert_z);
	bool have_plains = findBiome(&g, BIOME_PLAINS, 4096, 32, &plains_x, &plains_z);
	check(have_desert && have_plains,
	      "the search locates both a desert and a plains column for seed 314159");

	if (!have_desert || !have_plains) return;

	int desert_precip = 0, plains_precip = 0;
	for (uint64_t e = 0; e < 512; e++) {
		if (weatherAt(&g, e << WEATHER_EPISODE_SHIFT, desert_x, desert_z) != WEATHER_CLEAR)
			desert_precip++;
		if (weatherAt(&g, e << WEATHER_EPISODE_SHIFT, plains_x, plains_z) != WEATHER_CLEAR)
			plains_precip++;
	}

	check(desert_precip < plains_precip / 2,
	      "over 512 episodes, the desert location precipitates less than half as often as the "
	      "plains location -- WEATHER_CHANCE_DESERT (8/256), not WEATHER_CHANCE_PRECIP (64/256), "
	      "is actually being applied to it");
	check(desert_precip > 0 && desert_precip < 512 && plains_precip > 0 && plains_precip < 512,
	      "falsifiability: neither location is always-clear or always-precipitating across the "
	      "512 episodes -- both rolls are genuinely being exercised");
}

// ── 8: a huge absolute tick ─────────────────────────────────────────────────────────────────
//
// 5,000,000,000,000 mod 8192, by long division worked out by hand before this file was run:
// 5,000,000,000,000 = 8192 * 610,351,562 + 4,096. (8192 * 610,351,562 = 4,999,999,995,904;
// 5,000,000,000,000 - 4,999,999,995,904 = 4,096.) This is checked against the mask expression
// this file's own tests below actually use, so a mistake in that expression cannot silently
// validate the wrong residue.
#define WEATHER_TEST_HUGE_TICK 5000000000000ull

static void testWeatherHugeTickArithmetic(void)
{
	check((WEATHER_TEST_HUGE_TICK & (uint64_t)(WEATHER_EPISODE_TICKS - 1)) == 4096ull,
	      "hand-computed by long division: 5*10^12 mod 8192 (WEATHER_EPISODE_TICKS) is 4096");

	check(weatherNextSurfaceBlock(WEATHER_SNOW, 4096ull, BLOCK_AIR) == BLOCK_SNOW,
	      "at that huge tick's own residue within its episode (4096, past the 2400-tick "
	      "accumulation threshold), snow still accumulates -- the huge absolute magnitude of "
	      "the tick does not matter, only its residue");
	check(weatherNextSurfaceBlock(WEATHER_RAIN, 4096ull, BLOCK_SNOW) == BLOCK_AIR,
	      "...and existing snow still melts, by the same residue, past the 1200-tick melt "
	      "threshold");

	WorldGen g;
	worldgenInit(&g, 9001u, GEN_VERSION_BIOME);
	WeatherKind k1 = weatherAt(&g, WEATHER_TEST_HUGE_TICK, 111, 222);
	WeatherKind k2 = weatherAt(&g, WEATHER_TEST_HUGE_TICK, 111, 222);
	check(k1 == k2,
	      "weatherAt is repeatable at a tick around 5*10^12 (roughly 7,900 real-world years of "
	      "uptime at TICK_HZ 20) -- no crash, no UB, no nondeterminism from the huge magnitude");
}

// ── 9: weatherStepCell against a real World ─────────────────────────────────────────────────
static void testWeatherStepCellOnRealWorld(void)
{
	budgetReset();
	World w;
	worldInit(&w);

	WorldGen g;
	worldgenInit(&g, 42424242u, GEN_VERSION_BIOME);

	int32_t x, z;
	bool have_cold = findBiome(&g, BIOME_TUNDRA, 4096, 32, &x, &z)
	               || findBiome(&g, BIOME_TAIGA, 4096, 32, &x, &z);
	uint64_t snow_episode = 0, clear_episode = 0;
	bool have_snow_ep  = have_cold && findEpisodeWithKind(&g, x, z, WEATHER_SNOW, 4095,
	                                                       &snow_episode);
	bool have_clear_ep = have_cold && findEpisodeWithKind(&g, x, z, WEATHER_CLEAR, 4095,
	                                                       &clear_episode);
	check(have_cold && have_snow_ep,
	      "the search locates a real cold-biome column and an episode within it where weatherAt "
	      "classifies WEATHER_SNOW");
	check(have_clear_ep,
	      "...and a second episode at the SAME column where weatherAt classifies WEATHER_CLEAR, "
	      "for the melt half of this test");

	if (!have_cold || !have_snow_ep || !have_clear_ep) {
		worldExit(&w);
		return;
	}

	int32_t air_y = worldgenHeight(&g, x, z);

	// -- accumulation: too soon, then exactly at the threshold, then idempotent --
	worldSet(&w, x, air_y - 1, z, BLOCK_STONE);
	worldSet(&w, x, air_y,     z, BLOCK_AIR);

	bool changed1 = weatherStepCell(&g, &w, snow_episode << WEATHER_EPISODE_SHIFT, x, z);
	check(!changed1 && worldGet(&w, x, air_y, z) == BLOCK_AIR,
	      "before the accumulation threshold, weatherStepCell makes no change and the cell "
	      "stays air");

	bool changed2 = weatherStepCell(&g, &w,
	                                 (snow_episode << WEATHER_EPISODE_SHIFT)
	                                     + WEATHER_SNOW_ACCUM_TICKS,
	                                 x, z);
	check(changed2 && worldGet(&w, x, air_y, z) == BLOCK_SNOW,
	      "at the accumulation threshold, weatherStepCell places snow");

	bool changed3 = weatherStepCell(&g, &w,
	                                 (snow_episode << WEATHER_EPISODE_SHIFT)
	                                     + WEATHER_SNOW_ACCUM_TICKS,
	                                 x, z);
	check(!changed3,
	      "calling weatherStepCell again with snow already there, same episode: no further "
	      "change -- no second layer");

	// -- mined-ground safety --
	worldSet(&w, x, air_y - 1, z, BLOCK_AIR);
	worldSet(&w, x, air_y,     z, BLOCK_AIR);
	bool changed4 = weatherStepCell(&g, &w,
	                                 (snow_episode << WEATHER_EPISODE_SHIFT)
	                                     + WEATHER_SNOW_ACCUM_TICKS,
	                                 x, z);
	check(!changed4 && worldGet(&w, x, air_y, z) == BLOCK_AIR,
	      "if the ground below has been mined away since worldgen, weatherStepCell places "
	      "nothing -- no snow floating over a hole");

	// -- occupied-cell safety --
	worldSet(&w, x, air_y - 1, z, BLOCK_STONE);
	worldSet(&w, x, air_y,     z, BLOCK_STONE);   // pretend a player built here
	bool changed5 = weatherStepCell(&g, &w,
	                                 (snow_episode << WEATHER_EPISODE_SHIFT)
	                                     + WEATHER_SNOW_ACCUM_TICKS,
	                                 x, z);
	check(!changed5 && worldGet(&w, x, air_y, z) == BLOCK_STONE,
	      "a player-placed block occupying the target cell is never overwritten by weather, "
	      "even past the accumulation threshold");

	// -- melt: too soon, then exactly at the threshold --
	worldSet(&w, x, air_y - 1, z, BLOCK_STONE);
	worldSet(&w, x, air_y,     z, BLOCK_SNOW);
	bool changed6 = weatherStepCell(&g, &w, clear_episode << WEATHER_EPISODE_SHIFT, x, z);
	check(!changed6 && worldGet(&w, x, air_y, z) == BLOCK_SNOW,
	      "before the melt threshold, existing snow is not removed");

	bool changed7 = weatherStepCell(&g, &w,
	                                 (clear_episode << WEATHER_EPISODE_SHIFT)
	                                     + WEATHER_MELT_TICKS,
	                                 x, z);
	check(changed7 && worldGet(&w, x, air_y, z) == BLOCK_AIR,
	      "at the melt threshold, existing snow melts back to air");

	worldExit(&w);
}

// ── 10: weatherTickColumn covers a whole column ─────────────────────────────────────────────
static void testWeatherTickColumnCoversTheColumn(void)
{
	budgetReset();
	World w;
	worldInit(&w);

	WorldGen g;
	worldgenInit(&g, 24681357u, GEN_VERSION_BIOME);

	// A column is only 16 blocks across against a 128-block weather cell, so most cold-biome
	// columns should have all four corners agree -- but biome is a per-BLOCK classification and
	// a column sitting near a biome edge can have one corner spill into a different biome, which
	// is exactly the case a naive single-corner search does not defend against. Search directly
	// for a column whose all FOUR corners are cold (tundra or taiga), not just its first corner.
	bool have_cold_column = false;
	int32_t cx = 0, cz = 0;
	for (int32_t czc = -256; czc <= 256 && !have_cold_column; czc++) {
		for (int32_t cxc = -256; cxc <= 256 && !have_cold_column; cxc++) {
			int32_t x0 = cxc * CHUNK_DIM, z0 = czc * CHUNK_DIM;
			int32_t x1 = x0 + CHUNK_DIM - 1, z1 = z0 + CHUNK_DIM - 1;
			BiomeId b00 = worldgenBiomeAt(&g, x0, z0);
			BiomeId b10 = worldgenBiomeAt(&g, x1, z0);
			BiomeId b01 = worldgenBiomeAt(&g, x0, z1);
			BiomeId b11 = worldgenBiomeAt(&g, x1, z1);
			bool c00 = (b00 == BIOME_TUNDRA || b00 == BIOME_TAIGA);
			bool c10 = (b10 == BIOME_TUNDRA || b10 == BIOME_TAIGA);
			bool c01 = (b01 == BIOME_TUNDRA || b01 == BIOME_TAIGA);
			bool c11 = (b11 == BIOME_TUNDRA || b11 == BIOME_TAIGA);
			if (c00 && c10 && c01 && c11) {
				cx = cxc;
				cz = czc;
				have_cold_column = true;
			}
		}
	}

	// Now search for an episode where all four of THAT column's corners classify WEATHER_SNOW
	// together -- checked directly, rather than found at one corner and assumed at the rest.
	uint64_t snow_episode = 0;
	bool have_snow_ep = false;
	if (have_cold_column) {
		int32_t x0 = cx * CHUNK_DIM, z0 = cz * CHUNK_DIM;
		int32_t x1 = x0 + CHUNK_DIM - 1, z1 = z0 + CHUNK_DIM - 1;
		for (uint64_t e = 0; e <= 4095 && !have_snow_ep; e++) {
			uint64_t t = e << WEATHER_EPISODE_SHIFT;
			if (weatherAt(&g, t, x0, z0) == WEATHER_SNOW
			    && weatherAt(&g, t, x1, z0) == WEATHER_SNOW
			    && weatherAt(&g, t, x0, z1) == WEATHER_SNOW
			    && weatherAt(&g, t, x1, z1) == WEATHER_SNOW) {
				snow_episode = e;
				have_snow_ep = true;
			}
		}
	}
	check(have_cold_column && have_snow_ep,
	      "the search locates a real column whose four corners are all cold biome, and an "
	      "episode at which all four corners classify WEATHER_SNOW together");

	if (!have_cold_column || !have_snow_ep) {
		worldExit(&w);
		return;
	}

	// Ground every one of the column's 256 surface cells, using each cell's OWN real
	// worldgenHeight -- not a single shared y.
	for (int32_t lz = 0; lz < CHUNK_DIM; lz++) {
		for (int32_t lx = 0; lx < CHUNK_DIM; lx++) {
			int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
			int32_t ay = worldgenHeight(&g, x, z);
			worldSet(&w, x, ay - 1, z, BLOCK_STONE);
			worldSet(&w, x, ay,     z, BLOCK_AIR);
		}
	}

	// 64 calls, tick residues (tick & 255) stepping by 4 across the full 0..255 cycle exactly
	// once each -- see this file's header for the derivation of why +4*k keeps every one of the
	// 64 calls in the SAME episode (so the SNOW classification found above still holds) while
	// visiting a disjoint quartet of cells each time.
	int sum_changed = 0, calls_not_exactly_four = 0;
	for (int k = 0; k < 64; k++) {
		uint64_t tick = (snow_episode << WEATHER_EPISODE_SHIFT) + WEATHER_SNOW_ACCUM_TICKS
		              + (uint64_t)(4 * k);
		int changed = weatherTickColumn(&g, &w, tick, cx, cz);
		sum_changed += changed;
		if (changed != WEATHER_CELLS_PER_VISIT) calls_not_exactly_four++;
	}
	check(calls_not_exactly_four == 0,
	      "each of the 64 calls changes exactly WEATHER_CELLS_PER_VISIT (4) cells, since every "
	      "cell in the fully-grounded column is eligible");
	check(sum_changed == 256,
	      "64 calls at tick residues stepping by 4 across the full 0..255 cycle together change "
	      "every one of the column's 256 surface cells exactly once -- the sweep has no gaps and "
	      "no cell is double-counted");

	int repeat_changed = weatherTickColumn(&g, &w,
	                                        (snow_episode << WEATHER_EPISODE_SHIFT)
	                                            + WEATHER_SNOW_ACCUM_TICKS,
	                                        cx, cz);
	check(repeat_changed == 0,
	      "calling weatherTickColumn again at the same tick, now that its four cells already "
	      "hold snow, changes nothing further");

	worldExit(&w);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== weather test ==");

	testWeatherConstantsPinned();
	testControlWeatherAtRepeats();
	testWeatherNextSurfaceBlockTable();
	testWeatherAtDeterministic();
	testWeatherAtPerLocationNotGlobal();
	testWeatherAtBiomeSpecies();
	testWeatherAtDesertRarerThanPlains();
	testWeatherHugeTickArithmetic();
	testWeatherStepCellOnRealWorld();
	testWeatherTickColumnCoversTheColumn();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
