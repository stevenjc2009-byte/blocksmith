// Host microbenchmark for world GENERATION — worldgenArea() / worldgenColumn() — written for
// lane OPT-WORLDGEN's optimisation pass (2026-09-02).
//
// Why this exists: source/world/worldgen.c, worldgen_density.c, cave_carve.c and noise.c are
// all plain C with no <3ds.h> dependency, exactly the property tools/bench_scratch.c and
// tools/run_host_tests.sh already rely on, so generation speed can be measured here in
// seconds instead of through a devkitARM build and an emulator boot.
//
// This is NOT a test and deliberately does not live under source/: every .c file under
// source/ is globbed into the console build by the Makefile, and a second main() would
// break that link. Same reasoning tools/bench_scratch.c's header states.
//
// Build and run from WSL, repository root, with tools/run_bench_worldgen.sh, which links this
// file against source/world/{block,chunk,world,scratch,noise,worldgen,worldgen_density,
// cave_carve,genversion,registry,budget}.c and tests/net_stub.c.
//
// Method: each of the four generator versions (LEGACY, DENSITY, BIOME, CAVES) generates a
// fresh RADIUS-column ring — the same 7x7 = 49 columns main.c loads around the player at
// RENDER_DIST_MAX — timed over several passes. Each pass targets a DISJOINT area of the same
// World (offset far apart on cx), so every pass is a genuinely cold generation — no column is
// ever regenerated over blocks it already carries — while still sharing one World object and
// therefore one budget/table state across the whole run, which is what the game actually does
// as a player walks. Wall time, peak claimed block-storage bytes (world/budget.h) and
// sizeof(WorldGenScratch) are reported for each version, plus a warm-up pass excluded from the
// timed numbers.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "world/budget.h"
#include "world/chunk.h"
#include "world/genversion.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

// RENDER_DIST_MAX's ring — see tools/bench_scratch.c's own note on why the widest ring the
// game actually meshes is the representative one to bench, not an arbitrarily small one.
#define RADIUS 3
#define PASSES 8
// Columns per pass at RADIUS 3: (2*3+1)^2 = 49.
#define COLS_PER_PASS ((2 * RADIUS + 1) * (2 * RADIUS + 1))

// How far apart (in columns) two passes' areas sit. Wide enough that no tree canopy or the
// worm carver's CAVE_MAX_REACH (24 blocks, cave_carve.c) from one pass's ring can land in the
// next pass's ring and find it already occupied — which would make a later pass's timing
// include neighbour-occupied branches an earlier, fully-cold pass does not take.
#define PASS_GAP_COLUMNS 64

static double nowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Times PASSES fresh-area passes of one generator version and prints the per-column and
// per-pass numbers. Uses its own World so one version's budget usage cannot bleed into
// another's, and one static WorldGenScratch — this bench has one lane, like tools/
// bench_scratch.c and tools/greedy_probe.c both do.
static void benchVersion(const char* name, uint32_t version, uint32_t seed)
{
	static World world;
	static WorldGenScratch wgs;
	WorldGen gen;

	worldInit(&world);
	budgetReset();
	worldgenInit(&gen, seed, version);
	worldgenScratchInit(&wgs);

	printf("── %s (version %u) ──\n", name, version);
	printf("  sizeof(WorldGenScratch) = %zu bytes\n", sizeof(WorldGenScratch));

	// One untimed warm-up pass, at its own disjoint offset, so the first timed pass is not
	// paying for anything a cold process/allocator would only pay once (page faults on the
	// static buffers, etc.) — tools/bench_scratch.c's own reasoning for its warm-up loop.
	worldgenArea(&gen, &wgs, &world, -PASS_GAP_COLUMNS, 0, RADIUS);

	double best_us = 0, worst_us = 0, total_us = 0;
	int failed_total = 0;

	for (int pass = 0; pass < PASSES; pass++) {
		const int32_t cx0 = pass * PASS_GAP_COLUMNS;

		const double t0 = nowSeconds();
		const int failed = worldgenArea(&gen, &wgs, &world, cx0, 0, RADIUS);
		const double us = (nowSeconds() - t0) * 1e6;

		failed_total += failed;
		const double per_col = us / (double)COLS_PER_PASS;
		printf("  pass %d: %.1f us total, %.3f us/column, %d columns, %d refused\n",
		       pass, us, per_col, COLS_PER_PASS, failed);

		total_us += per_col;
		if (pass == 0 || per_col < best_us) best_us = per_col;
		if (pass == 0 || per_col > worst_us) worst_us = per_col;
	}

	printf("  us/column: best %.3f  worst %.3f  mean %.3f  (spread %.1f%%)\n",
	       best_us, worst_us, total_us / PASSES,
	       best_us > 0 ? 100.0 * (worst_us - best_us) / best_us : 0.0);
	printf("  claimed block-storage bytes: %zu  (peak this run)\n", (size_t)budgetPeak());
	printf("  world columns %d  chunks %d  refused-total %d\n\n",
	       world.columns, world.chunks, failed_total);

	worldExit(&world);
}

int main(void)
{
	printf("bench_worldgen: RADIUS=%d, %d columns/pass, %d passes, seed 1337\n\n",
	       RADIUS, COLS_PER_PASS, PASSES);

	// Two full runs of the whole suite (all four versions), so the printed spread also shows
	// the run-to-run noise floor and not just the inter-pass one — CLAUDE.md's "report the
	// noise floor by running it at least twice" rule, satisfied inside one binary invocation
	// rather than by invoking the binary twice.
	for (int run = 0; run < 2; run++) {
		printf("═══ run %d ═══\n", run);
		benchVersion("LEGACY", GEN_VERSION_LEGACY, 1337u);
		benchVersion("DENSITY", GEN_VERSION_DENSITY, 1337u);
		benchVersion("BIOME", GEN_VERSION_BIOME, 1337u);
		benchVersion("CAVES", GEN_VERSION_CAVES, 1337u);
	}

	return 0;
}
