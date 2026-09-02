// light_seam_test.c — v1.8.10 cross-column block-light handoff.
//
// Block light was column-local by design (light.h's header banner calls it "the known
// gap"): lightPropagateColumn/lightRelightColumnSweeps only ever read and write the one
// column they were called for, so a torch placed one cell from a column edge produced a
// hard seam — the neighbouring column simply never heard about it. Measured on an ad hoc
// host probe before this file existed: torch at luminance 14 one cell inside column 0's
// +x edge, relit exactly as scene/interact.c's relightEdited does (the edited column
// only), read 14 at column 0's border cell and 0 one cell further into column 1 — a
// vertical discontinuity of 14, not the physically correct 1-level falloff. This file is
// the coverage for the fix: light.c's crossBorderCandidate (the PULL half, read by both
// engines) and lightHandoffBorders (the PUSH half, called once from lightRelightColumn).
//
// Four arms:
//   1  growth      — the seam itself. Also checks the mesher's own light source
//                     (scratch.c's scratchFillLight, which is what cornerLight actually
//                     reads) sees the same falloff rather than a clamp, and that the
//                     independent sweep engine agrees with the BFS engine on the crossed
//                     value.
//   2  retraction  — a torch removed near an edge. The first version of the PUSH half
//                     only fired on a value that STRICTLY IMPROVED a neighbour's current
//                     light, which closes arm 1 and cannot detect a neighbour holding a
//                     now-stale value with no real source left behind it — relighting the
//                     edited column alone pulled the neighbour's stale value back in and
//                     read 12/11 instead of 0/0. See light.c's comment above
//                     relightColumnCoreDiff for the fix and its convergence bound.
//   3  depth cap   — light.c's LIGHT_HANDOFF_MAX_DEPTH backstop is proven reachable (not
//                     dead code) via lightSetHandoffMaxDepthForTest, the same test-hook
//                     idiom lightFailEditQueueForTest already uses for the CAS/malloc
//                     fallback below it — without needing the 30-column chain the real
//                     32-deep bound is sized for.
//   4  seed cost   — the CHUNK_FORM_UNIFORM short-circuit light.c's block-seed loop added
//                     alongside the handoff (a torch existing anywhere makes syncLuminance()
//                     answer true forever, so that loop now runs on every propagate instead
//                     of being skipped outright as it always was pre-v1.8.10). Timed on the
//                     host with clock(), not asserted on: CI hardware jitter would make a
//                     hard threshold either flaky or meaningless, so this prints real numbers
//                     for a human to read rather than a check that can go red on a slow
//                     runner. No verified host-to-268MHz-ARM11 scaling factor exists anywhere
//                     in this codebase (relightq.h says so explicitly), so nothing here
//                     invents one — the numbers below are host-measured and labelled as such.
//
// Own binary and own main(): the world suite is driven by tests/host_test.c and two
// mains cannot share a link, the same reason light_luminance_test.c and
// light_race_test.c each have their own.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/light.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/world.h"

static int s_checks;
static int s_failed;

// The suite's failure shape: tools/run_host_tests.sh's callers grep for "^FAIL  L[0-9]+".
#define CHECK(cond, ...)                                    \
	do {                                                     \
		s_checks++;                                          \
		if (!(cond)) {                                       \
			s_failed++;                                      \
			printf("FAIL  L%d  ", __LINE__);                 \
			printf(__VA_ARGS__);                              \
			printf("\n");                                     \
		}                                                     \
	} while (0)

static void buildFlatColumn(World* w, int cx, int cz, int floor_y)
{
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			for (int y = 0; y <= floor_y; y++)
				worldSet(w, cx * CHUNK_DIM + x, y, cz * CHUNK_DIM + z, BLOCK_STONE);
}

// A torch one cell in from column 0's +x edge, standing on a flat stone floor. "Almost
// anywhere" per the report this shipped with: any column edge, any luminance, one cell
// short of the boundary.
#define FLOOR     9
#define TORCH_LX  15
#define TORCH_Y   (FLOOR + 1)
#define TORCH_LZ  8

static void testSeamGrowth(void)
{
	printf("ARM 1 - seam growth: torch one cell from a column edge\n");

	World w;
	worldInit(&w);
	lightEngineInit(true);
	buildFlatColumn(&w, 0, 0, FLOOR);
	buildFlatColumn(&w, 1, 0, FLOOR);

	const uint8_t lum = registryGet(BLOCK_TORCH)->luminance;
	CHECK(lum == 14, "BLOCK_TORCH luminance = %u, this fixture assumes 14", lum);
	worldSet(&w, TORCH_LX, TORCH_Y, TORCH_LZ, BLOCK_TORCH);

	// Exactly what relightEdited (scene/interact.c) does on a real edit: relight ONLY
	// the column the edit landed in.
	CHECK(lightRelightColumn(&w, 0, 0), "edit-path relight of column 0 refused");

	Column* col0 = worldColumn(&w, 0, 0);
	Column* col1 = worldColumn(&w, 1, 0);
	const uint8_t v0 = lightGetBlock(col0, 15, TORCH_Y, TORCH_LZ);
	const uint8_t v1 = lightGetBlock(col1, 0, TORCH_Y, TORCH_LZ);
	printf("  col0 lx=15 (own border) = %u   col1 lx=0 (crossed) = %u\n", v0, v1);
	CHECK(v0 == 14, "column 0's own border cell = %u, want 14", v0);
	CHECK(v1 == 13, "light crossing into column 1 = %u, want 13 (the reported bug read 0)", v1);

	// What the mesher actually reads: scratchFillLight fills chunk (1,0,0)'s scratch
	// band from the two columns' real channels, sx=0 being column 0's real lx=15 and
	// sx=1 column 1's own lx=0 -- the same array cornerLight() taps with no boundary
	// special-case (mesher.c:405-417, confirmed read-only for this fix).
	MeshScratch scratch;
	scratchFillLight(&scratch, &w, 1, 0, 0);
	const int sy = TORCH_Y + 1, sz = TORCH_LZ + 1;
	const uint8_t border = scratch.light[scratchIndex(0, sy, sz)];
	const uint8_t inside = scratch.light[scratchIndex(1, sy, sz)];
	printf("  scratchFillLight: border blk=%u  inside blk=%u\n", border & 15, inside & 15);
	CHECK((border & 15) == 14, "scratchFillLight border blk = %u, want 14", border & 15);
	CHECK((inside & 15) == 13, "scratchFillLight inside blk = %u, want 13", inside & 15);

	// Engine agreement: the sweep engine, called directly (this wrapper is the only
	// thing that ever runs it outside a BFS refusal), must read the same crossed value
	// through its own copy of the crossBorderCandidate check in its cell loop.
	CHECK(lightRelightColumnSweeps(&w, 1, 0), "sweep relight of column 1 refused");
	const uint8_t sweepv = lightGetBlock(col1, 0, TORCH_Y, TORCH_LZ);
	CHECK(sweepv == 13, "sweeps engine read %u crossing the seam, BFS read 13 -- engines disagree", sweepv);

	CHECK(lightHandoffCapped() == 0,
	      "handoff capped %d time(s) on a single torch; the per-hop bound should hold "
	      "well clear of LIGHT_HANDOFF_MAX_DEPTH", lightHandoffCapped());

	worldExit(&w);
}

static void testRetraction(void)
{
	printf("ARM 2 - retraction: torch removed near a column edge must not leave ghost light\n");

	World w;
	worldInit(&w);
	lightEngineInit(true);
	buildFlatColumn(&w, 0, 0, FLOOR);
	buildFlatColumn(&w, 1, 0, FLOOR);
	worldSet(&w, TORCH_LX, TORCH_Y, TORCH_LZ, BLOCK_TORCH);
	CHECK(lightRelightColumn(&w, 0, 0), "initial relight refused");

	Column* col0 = worldColumn(&w, 0, 0);
	Column* col1 = worldColumn(&w, 1, 0);
	const uint8_t before1 = lightGetBlock(col1, 0, TORCH_Y, TORCH_LZ);
	CHECK(before1 == 13, "setup: neighbour did not receive crossed light before removal (got %u)", before1);

	worldSet(&w, TORCH_LX, TORCH_Y, TORCH_LZ, BLOCK_AIR);
	// Exactly what a real "break torch" edit does: relight ONLY the edited column.
	CHECK(lightRelightColumn(&w, 0, 0), "removal relight refused");

	const uint8_t after0 = lightGetBlock(col0, TORCH_LX, TORCH_Y, TORCH_LZ);
	const uint8_t after1 = lightGetBlock(col1, 0, TORCH_Y, TORCH_LZ);
	printf("  after removal: col0 torch cell = %u   col1 lx=0 = %u\n", after0, after1);
	CHECK(after0 == 0, "column 0 torch cell after removal = %u, want 0 (ghost light pulled back "
	      "from the neighbour's stale border)", after0);
	CHECK(after1 == 0, "column 1 neighbour cell after removal = %u, want 0 (never told the "
	      "torch it was reflecting is gone)", after1);

	worldExit(&w);
}

static void testHandoffDepthCap(void)
{
	printf("ARM 3 - handoff depth cap is reachable, not dead code\n");

	World w;
	worldInit(&w);
	lightEngineInit(true);
	buildFlatColumn(&w, 0, 0, FLOOR);
	buildFlatColumn(&w, 1, 0, FLOOR);
	worldSet(&w, TORCH_LX, TORCH_Y, TORCH_LZ, BLOCK_TORCH);

	lightResetHandoffCappedForTest();
	lightSetHandoffMaxDepthForTest(1);   // real bound is 32; this fixture only needs 2 hops

	CHECK(lightRelightColumn(&w, 0, 0), "relight refused");

	const int capped = lightHandoffCapped();
	printf("  handoff capped %d time(s) with max depth forced to 1\n", capped);
	CHECK(capped > 0, "forcing max depth to 1 never hit the cap; this arm is not exercising it");

	lightResetHandoffMaxDepthForTest();
	lightResetHandoffCappedForTest();

	worldExit(&w);
}

// 200 was not enough for the all-UNIFORM column's clock() delta to clear this host's timer
// resolution -- it read the same 0.0000 ms as an empty loop would. 20000 gives clock() a
// real, non-zero interval to divide even for the ~8-chunkGet fast path.
#define SEED_COST_ITERS  20000

// A column left untouched: chunk.c initialises every chunk CHUNK_FORM_UNIFORM (air) and
// chunkSet never demotes on writing the id a chunk already is, so an all-air column stays
// UNIFORM in every one of its 8 chunks -- exactly the common case (chunk.h: "half of every
// generated column is sky, and every sky chunk is this") the short-circuit exists for.
static void buildAllAirColumn(World* w, int cx, int cz)
{
	(void)w; (void)cx; (void)cz;   // nothing to do -- a fresh column IS this
}

// Forces every one of the 8 chunks away from CHUNK_FORM_UNIFORM by writing two distinct,
// non-luminous ids into each: chunkSet promotes UNIFORM -> PALETTE4 on the second distinct
// id it sees (chunk.c) and never demotes back, so this is a worst case for the seed loop --
// every chunk runs the full CHUNK_BLOCKS cell walk, none of them skip.
static void buildCheckerColumn(World* w, int cx, int cz)
{
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				worldSet(w, cx * CHUNK_DIM + x, y, cz * CHUNK_DIM + z,
				         ((x + z + y) & 1) ? BLOCK_STONE : BLOCK_DIRT);
}

static double timePropagateMs(World* w, int cx, int cz, LightQueue* q, int iters, double* out_total_ms)
{
	const clock_t t0 = clock();
	for (int i = 0; i < iters; i++)
		lightPropagateColumn(w, cx, cz, q);
	const clock_t t1 = clock();
	const double total_ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC;
	if (out_total_ms) *out_total_ms = total_ms;
	return total_ms / iters;
}

static void testSeedScanCost(void)
{
	printf("ARM 4 - emitter seed scan cost: CHUNK_FORM_UNIFORM short-circuit\n");

	World w;
	worldInit(&w);
	lightEngineInit(true);

	// Registers a real emitter so syncLuminance() answers true and the seed loop actually
	// runs on every propagate, matching what a torch existing ANYWHERE in the world does
	// forever after (light.c's own comment above the short-circuit). Torch itself is not
	// placed in either fixture column -- this measures the scan's fixed cost, not a
	// column doing real cross-column work.
	CHECK(registryGet(BLOCK_TORCH)->luminance > 0, "BLOCK_TORCH has no luminance; arm 4 cannot force syncLuminance() true");

	buildAllAirColumn(&w, 0, 0);
	buildCheckerColumn(&w, 1, 0);

	LightQueue q;
	lightQueueInit(&q);

	// Warm-up call outside the timed loop: first touch of freshly budget-claimed light
	// data, page faults and all, so it does not bias the very first timed sample.
	lightPropagateColumn(&w, 0, 0, &q);
	lightPropagateColumn(&w, 1, 0, &q);

	double uniform_total_ms = 0.0, checker_total_ms = 0.0;
	const double uniform_ms  = timePropagateMs(&w, 0, 0, &q, SEED_COST_ITERS, &uniform_total_ms);
	const double checker_ms  = timePropagateMs(&w, 1, 0, &q, SEED_COST_ITERS, &checker_total_ms);

	printf("  measured (host, gcc -O1, %d-iters average): "
	       "all-UNIFORM column %.6f ms (%.3f ms / %d iters total), "
	       "all-checkerboard column %.4f ms (%.3f ms / %d iters total), ratio %.1fx\n",
	       SEED_COST_ITERS,
	       uniform_ms, uniform_total_ms, SEED_COST_ITERS,
	       checker_ms, checker_total_ms, SEED_COST_ITERS,
	       uniform_ms > 0.0 ? checker_ms / uniform_ms : (checker_total_ms > 0.0 ? checker_total_ms / (uniform_total_ms > 0.0 ? uniform_total_ms : 1e-9) : 0.0));
	printf("  no verified host-to-268MHz-ARM11 scaling factor exists in this codebase "
	       "(relightq.h says so explicitly) -- these two numbers are host wall-clock only.\n");

	// Not a hard threshold -- see the file banner on why this arm prints instead of
	// asserting a specific ratio. The one thing worth a CHECK is the direction: the
	// short-circuit exists because skipping a whole non-luminous UNIFORM chunk is cheaper
	// than walking it, so the checkerboard column (which cannot take the short-circuit at
	// all) must not be FASTER than the all-UNIFORM one on the same hardware, same binary,
	// same run.
	CHECK(checker_ms >= uniform_ms,
	      "checkerboard column (%.4f ms) relit faster than the all-UNIFORM column (%.4f ms); "
	      "the short-circuit should never make the worst case cheaper than the best case",
	      checker_ms, uniform_ms);

	worldExit(&w);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	registryInitCore();
	budgetReset();

	testSeamGrowth();
	testRetraction();
	testHandoffDepthCap();
	testSeedScanCost();

	if (s_failed) {
		printf("light seam self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("light seam self-test: PASS %d checks\n", s_checks);
	return 0;
}
