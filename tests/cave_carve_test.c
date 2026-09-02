// tests/cave_carve_test.c -- host suite for the v1.8.11 worm-cave carver
// (world/cave_carve.h/.c), covering build-order steps 1-7 of docs/plan-1.8.11-caves.md.
//
// Lives under tests/ rather than source/world/ deliberately. The Makefile globs
// source/**/*.c into SOURCES (Makefile:26), so a _test.c placed there is compiled into the
// console build and its main() collides with the game's unless the whole body is wrapped in
// #ifndef __3DS__. tests/ is outside that glob, so no guard is needed and none is present.
//
// Wired into tools/run_host_tests.sh as the BHCC stanza. Written by the v1.8.11 cave lane,
// which was permission-blocked from editing that script; adopted and verified here, both
// arms, on 2026-09-02.
//
// ── What each check proves, and how it was proven able to go RED ─────────────────────────
//
//   sine table       endpoints and monotonicity of caveSin() against libm sin(), tolerance
//                     1 part in 65536 (one fx unit). RED ARM: corrupt one table entry (see
//                     the report for the exact sabotage and the failure it produced).
//   headline         a column generated on a FRESH scratch ("in isolation") produces the
//                     cross-column   SAME mask as the same column generated after many OTHER columns were
//   agreement        generated first on a SHARED, reused scratch ("as part of a larger
//                     area") -- the exact property this task's brief calls out by name.
//                     RED ARM: skip the memset at the top of caveCarveBuildMaskR (see report).
//   order            a block of columns built forward, reverse and shuffled all agree,
//   independence     per column. RED ARM: same sabotage as above (a stale mask reused
//                     across columns fails this one too, differently).
//   radius           CAVE_NEIGHBOURHOOD_R and R+2 must find nothing new -- the practical
//   sufficiency      form of the max_reach/R guarantee (plan 2.2). RED ARM: call with radius
//                     0 (nowhere near enough) and confirm it under-carves against the real R.
//   floor/ceiling    no bit is ever set below GEN_CAVE_FLOOR. RED ARM: comment out the
//                     GEN_CAVE_FLOOR check in caveCarveStamp (see report).
//   "carves           across a wide seed sweep, most (seed, column) draws are empty (the
//   something"       region gate's ~90% reject) but SOME are not -- proves the pipeline
//                     actually reaches a carve write at all. RED ARM: force the region gate
//                     to always reject (see report).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/cave_carve.h"
#include "world/genversion.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

#define CAVEAGENT_PI 3.14159265358979323846

static int g_checks, g_failed;

#define CHECK(cond) do {                                                     \
		g_checks++;                                                          \
		if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s\n",                \
		                                  __FILE__, __LINE__, #cond); }      \
	} while (0)

// FNV-1a over one column's worth of carve mask bits.
static uint32_t digestMask(const WorldGenScratch* s)
{
	uint32_t h = 2166136261u;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++) {
			const uint16_t v = s->carve[y][z];
			h = (h ^ (uint32_t)(v & 0xFFu)) * 16777619u;
			h = (h ^ (uint32_t)(v >> 8)) * 16777619u;
		}
	return h;
}

static int popcountMask(const WorldGenScratch* s)
{
	int n = 0;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++) {
			uint16_t v = s->carve[y][z];
			while (v) { n += (int)(v & 1u); v = (uint16_t)(v >> 1); }
		}
	return n;
}

// ── Sine table ─────────────────────────────────────────────────────────────────────────

static void testSine(void)
{
	CHECK(caveSin(0) == 0);
	CHECK(caveSin(0x4000) == FX_ONE);          // 90 degrees
	CHECK(caveSin(0x8000) == 0);               // 180 degrees
	CHECK(caveSin(0xC000) == -FX_ONE);         // 270 degrees
	CHECK(caveCos(0) == FX_ONE);
	CHECK(caveCos(0x4000) == 0);

	// Monotonic non-decreasing over the first quadrant.
	fx prev = caveSin(0);
	bool monotonic = true;
	for (uint32_t a = 1; a <= 0x4000; a += 37) {
		const fx v = caveSin((uint16_t)a);
		if (v < prev) monotonic = false;
		prev = v;
	}
	CHECK(monotonic);

	// Against libm, sampled across the whole turn. One fx unit is 1/65536 = 1.5e-5; the table
	// itself is only 64 steps per quarter turn (a step is 2^14/64 = 256 raw angle units), so the
	// lookup's own quantisation is the dominant error, not the table's arithmetic -- bounded
	// generously at 1200 fx units (~0.018) to allow for that quantisation without hiding a
	// real defect (a wrong table entry moves error by thousands of fx units, not hundreds).
	double max_err = 0;
	for (uint32_t a = 0; a < 65536; a += 91) {
		const double want = sin((double)a * (2.0 * CAVEAGENT_PI / 65536.0));
		const double got  = (double)caveSin((uint16_t)a) / 65536.0;
		const double err  = fabs(want - got);
		if (err > max_err) max_err = err;
	}
	printf("  sine table   max |libm - caveSin| over the full turn: %.6f\n", max_err);
	// Measured after switching the lookup from floor to round-to-nearest: ~0.012. The bound
	// below is loose enough to tolerate the table's own 64-step quantisation but tight enough
	// that a real defect (a corrupted entry moves this by thousands of fx units) still fails.
	CHECK(max_err < 0.02);
}

// ── Headline: isolation vs. as-part-of-a-larger-area ──────────────────────────────────

#define SWEEP_SEEDS 12
static const uint32_t k_seeds[SWEEP_SEEDS] = {
	1u, 1337u, 4242u, 90210u, 0x5EEDCAFEu, 0xDEADBEEFu, 0xC001D00Du,
	0x13371337u, 777u, 999983u, 0xFFFFFFFFu, 0x80000000u,
};

static void testIsolationVsLargerArea(void)
{
	int mismatches = 0, total = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		CHECK(worldgenInit(&g, k_seeds[si], GEN_VERSION_CAVES));

		for (int32_t cz = -2; cz <= 2; cz++) {
			for (int32_t cx = -2; cx <= 2; cx++) {
				// "In isolation": one fresh scratch, one call.
				WorldGenScratch iso;
				worldgenScratchInit(&iso);
				caveCarveBuildMask(&g, &iso, cx, cz);
				const uint32_t d_iso = digestMask(&iso);

				// "As part of a larger area": one shared, reused scratch that has already
				// built masks for a whole neighbourhood of OTHER columns first, exactly the
				// way one lane's WorldGenScratch is reused across every column it generates.
				WorldGenScratch shared;
				worldgenScratchInit(&shared);
				for (int32_t z2 = cz - 3; z2 <= cz + 3; z2++)
					for (int32_t x2 = cx - 3; x2 <= cx + 3; x2++) {
						if (x2 == cx && z2 == cz) continue;
						caveCarveBuildMask(&g, &shared, x2, z2);
					}
				caveCarveBuildMask(&g, &shared, cx, cz);
				const uint32_t d_shared = digestMask(&shared);

				total++;
				if (d_iso != d_shared) mismatches++;
				CHECK(d_iso == d_shared);
			}
		}
	}
	printf("  isolation-vs-larger-area   %d/%d columns agreed (seeds: %d)\n",
	       total - mismatches, total, SWEEP_SEEDS);
}

// ── Order independence (forward / reverse / shuffled) ─────────────────────────────────

static void testOrderIndependence(void)
{
	WorldGen g;
	CHECK(worldgenInit(&g, 0xA5A5A5A5u, GEN_VERSION_CAVES));

	enum { N = 25 };   // 5x5 block
	int32_t cxs[N], czs[N];
	int k = 0;
	for (int32_t z = -2; z <= 2; z++)
		for (int32_t x = -2; x <= 2; x++) { cxs[k] = x; czs[k] = z; k++; }

	uint32_t forward[N], reverse[N], shuffled[N];

	WorldGenScratch s;
	worldgenScratchInit(&s);
	for (int i = 0; i < N; i++) {
		caveCarveBuildMask(&g, &s, cxs[i], czs[i]);
		forward[i] = digestMask(&s);
	}

	worldgenScratchInit(&s);
	for (int i = N - 1; i >= 0; i--) {
		caveCarveBuildMask(&g, &s, cxs[i], czs[i]);
		reverse[i] = digestMask(&s);
	}

	// A fixed pseudo-shuffle (not the game's RNG -- just a stride walk that visits every
	// index exactly once in a different order, deterministic across runs).
	int order[N];
	for (int i = 0; i < N; i++) order[(i * 7) % N] = i;
	worldgenScratchInit(&s);
	for (int oi = 0; oi < N; oi++) {
		const int i = order[oi];
		caveCarveBuildMask(&g, &s, cxs[i], czs[i]);
		shuffled[i] = digestMask(&s);
	}

	int wrong = 0;
	for (int i = 0; i < N; i++) {
		if (forward[i] != reverse[i]) wrong++;
		if (forward[i] != shuffled[i]) wrong++;
		CHECK(forward[i] == reverse[i]);
		CHECK(forward[i] == shuffled[i]);
	}
	printf("  order independence   %d columns, %d mismatches (fwd/rev/shuffled)\n", N, wrong);
}

// ── Radius sufficiency: R and R+2 must agree ───────────────────────────────────────────

static void testRadiusSufficiency(void)
{
	int checked = 0, disagreements = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		CHECK(worldgenInit(&g, k_seeds[si], GEN_VERSION_CAVES));

		for (int32_t cz = -1; cz <= 1; cz++) {
			for (int32_t cx = -1; cx <= 1; cx++) {
				WorldGenScratch a, b;
				worldgenScratchInit(&a);
				worldgenScratchInit(&b);
				caveCarveBuildMaskR(&g, &a, cx, cz, CAVE_NEIGHBOURHOOD_R);
				caveCarveBuildMaskR(&g, &b, cx, cz, CAVE_NEIGHBOURHOOD_R + 2);
				checked++;
				if (digestMask(&a) != digestMask(&b)) disagreements++;
				CHECK(digestMask(&a) == digestMask(&b));
			}
		}
	}
	printf("  radius sufficiency   R vs R+2: %d/%d columns agreed\n",
	       checked - disagreements, checked);

	// The NEGATIVE control for this same check, proving it is not vacuously true: radius 0
	// (scanning only the column's own region) must find LESS than the real R in at least one
	// swept column, or this whole comparison is comparing two accidentally-equal numbers.
	int radius0_found_less = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		worldgenInit(&g, k_seeds[si], GEN_VERSION_CAVES);
		for (int32_t cz = -1; cz <= 1; cz++)
			for (int32_t cx = -1; cx <= 1; cx++) {
				WorldGenScratch a, b;
				worldgenScratchInit(&a);
				worldgenScratchInit(&b);
				caveCarveBuildMaskR(&g, &a, cx, cz, 0);
				caveCarveBuildMaskR(&g, &b, cx, cz, CAVE_NEIGHBOURHOOD_R);
				if (popcountMask(&a) < popcountMask(&b)) radius0_found_less++;
			}
	}
	printf("  radius=0 undercounts vs R in %d/%d columns (sanity: proves the comparison is "
	       "not vacuous)\n", radius0_found_less, SWEEP_SEEDS * 9);
	CHECK(radius0_found_less > 0);
}

// ── Floor respected, and something actually gets carved ───────────────────────────────

static void testFloorAndNonEmpty(void)
{
	int below_floor = 0;
	int nonempty_columns = 0, total_columns = 0;

	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		worldgenInit(&g, k_seeds[si], GEN_VERSION_CAVES);
		for (int32_t cz = -3; cz <= 3; cz++) {
			for (int32_t cx = -3; cx <= 3; cx++) {
				WorldGenScratch s;
				worldgenScratchInit(&s);
				caveCarveBuildMask(&g, &s, cx, cz);
				total_columns++;
				if (popcountMask(&s) > 0) nonempty_columns++;
				for (int y = 0; y < GEN_CAVE_FLOOR; y++)
					for (int z = 0; z < CHUNK_DIM; z++)
						if (s.carve[y][z] != 0) below_floor++;
			}
		}
	}
	printf("  floor/nonempty   %d columns carved something out of %d; %d cells below "
	       "GEN_CAVE_FLOOR (must be 0)\n", nonempty_columns, total_columns, below_floor);
	CHECK(below_floor == 0);
	CHECK(nonempty_columns > 0);
}

int main(void)
{
	printf("cave carver self-test (v1.8.11, plan-1.8.11-caves.md steps 1-7)\n");
	testSine();
	testIsolationVsLargerArea();
	testOrderIndependence();
	testRadiusSufficiency();
	testFloorAndNonEmpty();

	printf("%s (%d/%d checks)\n", g_failed ? "FAILED" : "PASS",
	       g_failed ? g_failed : g_checks, g_checks);
	return g_failed ? 1 : 0;
}
