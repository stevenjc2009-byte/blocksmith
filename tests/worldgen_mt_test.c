// tests/worldgen_mt_test.c -- can worldgenColumn() run on TWO threads at once? (v1.8.7)
//
// ── Why this file exists ──────────────────────────────────────────────────────────────
//
// The New 3DS has four cores and this game meaningfully uses two. app/worker.c pins its one
// generator thread to core 2 (app/hw.c's ladder), and the main thread spends ~15.7 ms of every
// 16.71 ms frame blocked on the GPU inside C3D_FrameBegin(C3D_FRAME_SYNCDRAW) -- so core 0 is
// idle for ~94 % of the frame while column generation, measured at 4,669 ms cumulative in the
// `generate` stage of a recorded sdmc:/blocksmith/load.csv, is the dominant cost of a world
// load. A SECOND generator lane on core 0 is therefore the largest single win available on a
// New 3DS, and it is worth exactly nothing if the generator cannot be entered twice at once.
//
// Reading the source said it could not: world/worldgen.c and world/worldgen_density.c held
// their per-column scratch in file statics. Reading is a hypothesis. This file was the
// measurement, and the measurement is recorded below.
//
// v1.8.7 moved that scratch into a caller-owned WorldGenScratch (world/worldgen_scratch.h), so
// this file now answers YES and stays in the suite as the regression guard that says so.
//
// ── What it does ──────────────────────────────────────────────────────────────────────
//
// Three arms over the SAME 32 distinct columns and the same seed, each column digested with an
// FNV-1a over all 32,768 of its blocks read back through worldGet():
//
//   BASELINE     one thread, columns in index order.                 -> the reference digests
//   SERIALISED   two threads, disjoint column sets, one global mutex
//                held across worldgenColumn().                       -> MUST match baseline
//   CONCURRENT   two threads, disjoint column sets, no mutex.        -> the question
//
// The SERIALISED arm is what stops a failure in the CONCURRENT arm from being blamed on the
// harness. It uses the same two threads, the same private Worlds, the same interleaving of
// digest reads and budget traffic, and the same column split -- everything except the lock.
// If SERIALISED matches the baseline and CONCURRENT does not, the difference is concurrency
// inside the generator and nothing else. If SERIALISED were also to fail, this test would be
// measuring itself and its verdict on CONCURRENT would be worthless.
//
// The two threads are never handed the same column: thread 0 takes the even indices and
// thread 1 the odd ones, out of a fixed table. That is deliberate -- the claim under test is
// "two threads on DISTINCT columns", which is the only arrangement a real second lane would
// ever produce, and the weakest claim that still blocks the feature.
//
// ── MEASURED BEFORE THE FIX, 2026-09-02, WSL gcc 15.2.0 -O1, three consecutive runs ───
//
//   SERIALISED   generated  96+ 96  refused  0+ 0  wrong  0/32  missing 0
//   CONCURRENT   generated  44+ 29  refused 52+67  wrong 18/32  missing 2
//   CONCURRENT   generated  23+ 16  refused 73+80  wrong 23/32  missing 9
//   CONCURRENT   generated  17+ 19  refused 79+77  wrong 24/32  missing 7
//
// The control arm was 0/32 wrong in all three runs; the concurrent arm never once came
// close. Note what dominates: 119-159 of every 192 generations were REFUSED OUTRIGHT --
// worldgenColumn() returned false -- which is worse than wrong terrain. That is the
// s_top_cx/s_top_cz/s_top_valid check-then-read in worldgen_density.c: worldgenScatter()
// calls wgdColumnTops() after the fill, the other thread has moved the key by then, and the
// whole column reports failure. In the game a false from worldgenColumn is a permanent hole
// in the world (main.c genInstallOne -> s_genr.columns_failed), not a slow frame.
//
// ── The per-lane scratch, measured on ARM ─────────────────────────────────────────────
//
// arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O3, single TU,
// read with arm-none-eabi-nm -S. Every byte below had to become per-thread before a second
// lane was safe, and every one of them now lives in WorldGenScratch:
//
//   worldgen.c          s_gen_flat  4,096   s_cave  1,488                    =  5,584 B
//   worldgen_density.c  s_grid 1,700  s_solid 4,096  s_top 512
//                       s_slope 256  s_flat 4,096  s_top_cx/cz/valid 12      = 10,672 B
//                                                             per lane, total  16,256 B
//
// Those two figures were the ENTIRE .bss of both TUs (arm-none-eabi-size), so there was no
// other hidden generator state to find -- and after the move both TUs measure .bss 0.
//
// ── THE TEST TODAY ────────────────────────────────────────────────────────────────────
//
// All three arms pass, and it is wired into tools/run_host_tests.sh so that anyone who puts
// generator state back into a file static gets told. To build and run it by hand:
//
//   gcc -std=c11 -Wall -Wextra -Werror -O1 -g -I source
//       source/world/block.c source/world/registry.c source/world/chunk.c
//       source/world/chunk_codec.c source/world/crc32.c
//       source/world/world.c source/world/scratch.c source/world/noise.c
//       source/world/budget.c source/world/worldgen.c source/world/worldgen_density.c
//       source/world/genversion.c tests/net_stub.c tests/worldgen_mt_test.c
//       -pthread -lm -o worldgen_mt_test
//
// (one line; the continuations are omitted here because -Wcomment refuses a backslash at the
// end of a // comment line.)
//
// To prove the guard can still go red, point both arms at the same scratch -- change the two
// `a0.wgs`/`a1.wgs` assignments in runArm() to the same element of g_wgs -- and the CONCURRENT
// arm reports wrong columns and refusals again. That is the defect this file was written for.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/budget.h"
#include "world/genversion.h"
#include "world/registry.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

#define TEST_SEED  0x5EEDCAFEu

// 32 columns, split 16/16 between the two threads. Enough that the arms take long enough to
// overlap properly, and small enough that a whole run is under a second.
#define NCOLS      32

// A race that fires once in a while is still a race, and one round could miss it. Six rounds
// of 32 columns is 192 generations per arm, 96 per thread.
#define ROUNDS     6

static WorldGen g_gen;
static uint32_t g_baseline[NCOLS];
static bool     g_baseline_ok[NCOLS];

static pthread_mutex_t g_serialise = PTHREAD_MUTEX_INITIALIZER;

static int g_checks, g_failed;

#define CHECK(cond) do {                                                     \
        g_checks++;                                                          \
        if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s\n",                \
                                          __FILE__, __LINE__, #cond); }      \
    } while (0)

// A fixed 8 x 4 block of columns around the origin. Fixed rather than random so the baseline
// digests below are reproducible and can be quoted.
static void colCoord(int i, int32_t* cx, int32_t* cz)
{
	*cx = (int32_t)(i % 8) - 4;
	*cz = (int32_t)(i / 8) - 2;
}

// FNV-1a over every block of one column, read through the public API rather than out of the
// Chunk internals -- the storage form a chunk settles on is not part of the claim, the blocks
// are.
static uint32_t digestColumn(const World* w, int32_t cx, int32_t cz)
{
	uint32_t h = 2166136261u;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int lz = 0; lz < CHUNK_DIM; lz++)
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				const BlockId b =
					worldGet(w, cx * CHUNK_DIM + lx, y, cz * CHUNK_DIM + lz);
				h = (h ^ (uint32_t)b) * 16777619u;
			}
	return h;
}

// v1.8.7. One per thread, and the reason the CONCURRENT arm can pass at all: these ARE the
// buffers that used to be file statics in world/worldgen.c and world/worldgen_density.c. Two
// of them, statically allocated, because that is exactly what a second worker lane would own.
//
// Pointing both arms at the same element is this test's red arm -- it reintroduces the defect
// the file was written for, and the CONCURRENT arm must go red when it does.
static WorldGenScratch g_wgs[2];

typedef struct {
	int      parity;        // 0 -> even column indices, 1 -> odd. Disjoint by construction.
	bool     serialise;     // hold g_serialise across worldgenColumn()
	WorldGenScratch* wgs;   // THIS thread's scratch; never shared with the other arm
	uint32_t digest[NCOLS]; // only this thread's indices are written
	bool     got[NCOLS];
	int      generated;
	int      refused;
} Arm;

static void* armRun(void* p)
{
	Arm* a = (Arm*)p;

	// Every thread generates into its OWN World, which is what a second worker lane would do:
	// app/worker.c stages into a world the worker owns exclusively and the main thread copies
	// it across. Sharing one World here would test world.c's hash table instead of the
	// generator, which is a different question.
	World w;
	worldInit(&w);

	for (int r = 0; r < ROUNDS; r++) {
		for (int i = a->parity; i < NCOLS; i += 2) {
			int32_t cx = 0, cz = 0;
			colCoord(i, &cx, &cz);

			if (a->serialise) pthread_mutex_lock(&g_serialise);
			const bool ok = worldgenColumn(&g_gen, a->wgs, &w, cx, cz);
			if (a->serialise) pthread_mutex_unlock(&g_serialise);

			if (!ok) {
				a->refused++;
				worldColumnRemove(&w, cx, cz);
				continue;
			}

			a->generated++;
			// Read back outside the lock on purpose: `w` is private to this thread, so the
			// digest cannot be disturbed by the other one. Only the generation itself is
			// contended, which is the variable under test.
			a->digest[i] = digestColumn(&w, cx, cz);
			a->got[i]    = true;

			// Keeps the budget flat across rounds so a late round cannot be refused for
			// memory the earlier ones never gave back, which would look like a race.
			worldColumnRemove(&w, cx, cz);
		}
	}

	worldExit(&w);
	return NULL;
}

// Runs one two-thread arm and reports how many of the 32 columns disagreed with the baseline.
static int runArm(const char* name, bool serialise)
{
	static Arm a0, a1;   // ~300 bytes each; static only to keep them off a shared stack.
	memset(&a0, 0, sizeof a0);
	memset(&a1, 0, sizeof a1);
	a0.parity = 0; a0.serialise = serialise; a0.wgs = &g_wgs[0];
	a1.parity = 1; a1.serialise = serialise; a1.wgs = &g_wgs[1];
	worldgenScratchInit(a0.wgs);
	worldgenScratchInit(a1.wgs);

	pthread_t t0, t1;
	pthread_create(&t0, NULL, armRun, &a0);
	pthread_create(&t1, NULL, armRun, &a1);
	pthread_join(t0, NULL);
	pthread_join(t1, NULL);

	int wrong = 0, missing = 0;
	for (int i = 0; i < NCOLS; i++) {
		const Arm* a = (i % 2 == 0) ? &a0 : &a1;
		if (!g_baseline_ok[i]) continue;
		if (!a->got[i])                        { missing++; continue; }
		if (a->digest[i] != g_baseline[i])       wrong++;
	}

	printf("  %-11s  generated %3d+%3d  refused %2d+%2d  wrong %2d/%d  missing %d\n",
	       name, a0.generated, a1.generated, a0.refused, a1.refused,
	       wrong, NCOLS, missing);
	return wrong + missing;
}

int main(void)
{
	printf("worldgen two-thread self-test  (seed %08x, %d columns, %d rounds)\n",
	       TEST_SEED, NCOLS, ROUNDS);

	registryInitCore();
	registryFreeze();
	if (!worldgenInit(&g_gen, TEST_SEED, GEN_VERSION_NEWEST)) {
		printf("FATAL: worldgenInit refused GEN_VERSION_NEWEST\n");
		return 2;
	}

	// ── Baseline: one thread, no contention, nothing to race with ─────────────────────
	{
		static World w;
		worldInit(&w);
		int made = 0;
		for (int i = 0; i < NCOLS; i++) {
			int32_t cx = 0, cz = 0;
			colCoord(i, &cx, &cz);
			if (worldgenColumn(&g_gen, &g_wgs[0], &w, cx, cz)) {
				g_baseline[i]    = digestColumn(&w, cx, cz);
				g_baseline_ok[i] = true;
				made++;
			}
			worldColumnRemove(&w, cx, cz);
		}
		worldExit(&w);
		printf("  %-11s  generated %3d/%d\n", "BASELINE", made, NCOLS);
		CHECK(made == NCOLS);
	}

	// Two reference digests, printed so that any change to what the generator COMPUTES is
	// visible here as well as in world_test.c's pinned columns. These must not move for a
	// change that only re-plumbs scratch.
	printf("  REFERENCE   col[0](-4,-2) %08x   col[17](1,0) %08x\n",
	       g_baseline[0], g_baseline[17]);

	// ── The control arm. If this is not clean, the verdict below means nothing. ────────
	const int ser = runArm("SERIALISED", true);
	CHECK(ser == 0);

	// ── The question. ─────────────────────────────────────────────────────────────────
	const int con = runArm("CONCURRENT", false);
	CHECK(con == 0);

	// The budget is already CAS-hardened (world/budget.c) and every column was removed, so a
	// non-zero here would be a leak rather than a race -- checked so a generator change that
	// starts leaking cannot hide behind the digest comparison.
	printf("  budget used %lu B (want 0), refusals %d\n",
	       (unsigned long)budgetUsed(), budgetRefusals());
	CHECK(budgetUsed() == 0);

	printf("worldgen two-thread self-test: %s - %d of %d checks\n",
	       g_failed ? "FAILED" : "PASS",
	       g_failed ? g_failed : g_checks, g_checks);
	return g_failed ? 1 : 0;
}
