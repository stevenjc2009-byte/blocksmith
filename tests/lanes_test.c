// tests/lanes_test.c -- the TWO-LANE generator, driven end to end on the host (v1.8.8)
//
// ── What this file is for ─────────────────────────────────────────────────────────────
//
// tests/worldgen_mt_test.c answers "can worldgenColumn() be entered twice at once", which was
// v1.8.7's blocking prerequisite. It does NOT answer "does the second lane app/worker.c now
// starts actually work", because app/worker.c includes <3ds.h> on its first line, carries the
// thread bodies, and cannot be compiled anywhere but for the console -- and nothing in this
// project has run on real hardware since v1.2.5.
//
// So the parts of the second lane that can be WRONG were put in a file that has no <3ds.h> in
// it (source/app/lanes.c: the column-claim set, the ready ring and the lane-count policy) and
// this test links THAT FILE, not a retyped copy of it, and drives it from two pthreads over
// the REAL worldgenColumn with the REAL JobQueue -- the same split app/hw.c and app/battery.c
// already use and for the same reason.
//
// ── The four arms ─────────────────────────────────────────────────────────────────────
//
//   UNIT        laneCountFor, LaneClaims and LaneReadyRing exercised directly, including the
//               invariant the claim set exists for: two lanes may never hold one column.
//   BASELINE    one thread, 32 columns in index order -> the reference digests.
//   TWO-LANE    the whole machine: a shared JobQueue under one mutex, two lane threads that
//               pop-and-claim / generate / push-ready, and a main thread that pops the ready
//               ring, digests the staged column and drops the claim. EVERY column is submitted
//               TWICE, which is what main.c can really do (genUnloadColumn clears a column's
//               `asked` mark when it leaves the ring and genRequestArea re-asks if the player
//               walks back), so the duplicate path is exercised rather than assumed.
//   TIMING      the same 64 columns through the same machine with one lane and then with two,
//               wall clock, three repeats. Host figures only -- see the warning below.
//
// The TWO-LANE arm asserts four things, and the accounting one is the load-bearing one:
//
//   * every column's digest equals the BASELINE digest (the terrain is not affected by which
//     lane generated it or by what the other lane was doing);
//   * installs + duplicates-dropped == jobs submitted, EXACTLY. A job either produces a
//     column or is refused as a duplicate; anything else is a job that vanished, which in the
//     game is a permanent hole in the world;
//   * laneClaimsDisjoint() held at every single take, sampled inside the lock;
//   * zero refusals from worldgenColumn and zero budget left over.
//
// ── The RED ARM, because a check that cannot fail proves nothing ──────────────────────
//
// laneClaimTake() in source/app/lanes.c is sabotaged to accept every take (the pre-claim
// behaviour: two lanes may hold the same column). See tools/run_host_tests.sh's stanza for
// this binary for the verbatim output of the red run and the md5 that confirms the restore.
//
// ── What this test does NOT prove ─────────────────────────────────────────────────────
//
// The TIMING arm runs on an x86-64 host with far more than two cores and a completely
// different memory system. It says the two-lane MACHINE scales -- that the claim set, the
// queue and the ready ring do not serialise the generator -- and it says nothing whatever
// about how many milliseconds a New 3DS saves. That number does not exist and cannot be
// produced here.
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app/lanes.h"
#include "world/budget.h"
#include "world/genversion.h"
#include "world/jobq.h"
#include "world/registry.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

#define TEST_SEED   0x5EEDCAFEu

// 32 columns for the correctness arm, each submitted twice = 64 jobs.
#define NCOLS       32

// 64 distinct columns for the timing arm, so one lane has enough work that the thread
// start-up cost is not what is being measured.
#define NTIME_COLS  64

#define TIME_REPEATS 3

static int g_checks, g_failed;

#define CHECK(cond) do {                                                     \
        g_checks++;                                                          \
        if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s\n",                \
                                          __FILE__, __LINE__, #cond); }      \
    } while (0)

static WorldGen g_gen;
static uint32_t g_baseline[NCOLS];
static bool     g_baseline_ok[NCOLS];

// A fixed block of columns around the origin, the same shape worldgen_mt_test.c uses, so a
// digest printed by one file can be compared with the other by eye.
static void colCoord(int i, int32_t* cx, int32_t* cz)
{
	*cx = (int32_t)(i % 8) - 4;
	*cz = (int32_t)(i / 8) - 2;
}

// FNV-1a over every block of one column, read through the public API rather than out of the
// Chunk internals -- the storage form a chunk settles on is not part of the claim.
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

static double nowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// ── Arm 1: the bookkeeping on its own ─────────────────────────────────────────────────

static void unitArm(void)
{
	printf("  UNIT\n");

	// The lane-count policy. Old 3DS one lane, New 3DS two, and never more than the constant
	// says -- a third lane would have no core of its own to run on.
	CHECK(laneCountFor(false) == 1);
	CHECK(laneCountFor(true)  == WORKER_LANES_MAX);
	CHECK(WORKER_LANES_MAX == 2);

	LaneClaims c;
	laneClaimsInit(&c);
	CHECK(laneClaimCount(&c) == 0);
	CHECK(laneClaimsDisjoint(&c));
	CHECK(!laneClaimBusy(&c, 0));

	CHECK(laneClaimTake(&c, 0, 3, -7));
	CHECK(laneClaimBusy(&c, 0));
	CHECK(laneClaimHeld(&c, 3, -7));
	CHECK(!laneClaimHeld(&c, 3, -6));

	// A lane already holding a column may not take a second one. That is a caller bug rather
	// than a duplicate, so it must NOT be counted as a collision.
	CHECK(!laneClaimTake(&c, 0, 9, 9));
	CHECK(laneClaimCollisions(&c) == 0);

	// THE INVARIANT. The other lane asking for the same column is refused, counted, and the
	// set stays disjoint. This is the whole reason app/lanes.c exists.
	CHECK(!laneClaimTake(&c, 1, 3, -7));
	CHECK(laneClaimCollisions(&c) == 1);
	CHECK(laneClaimCount(&c) == 1);
	CHECK(laneClaimsDisjoint(&c));

	// A DIFFERENT column is fine, which is the case the feature is for.
	CHECK(laneClaimTake(&c, 1, 4, -7));
	CHECK(laneClaimCount(&c) == 2);
	CHECK(laneClaimsDisjoint(&c));

	// Dropped, and now the column the other lane wanted is available.
	laneClaimDrop(&c, 0);
	CHECK(!laneClaimBusy(&c, 0));
	CHECK(!laneClaimHeld(&c, 3, -7));
	CHECK(laneClaimTake(&c, 0, 3, -7));

	// Out-of-range lanes are refused rather than writing past the arrays.
	CHECK(!laneClaimTake(&c, -1, 0, 0));
	CHECK(!laneClaimTake(&c, WORKER_LANES_MAX, 0, 0));

	LaneReadyRing r;
	laneReadyInit(&r);
	CHECK(laneReadyConsistent(&r));
	CHECK(laneReadyCount(&r) == 0);
	CHECK(!laneReadyPop(&r, NULL, NULL, NULL, NULL));

	CHECK(laneReadyPush(&r, 0, 1, 2, true));
	CHECK(laneReadyPush(&r, 1, 3, 4, false));
	CHECK(laneReadyCount(&r) == LANE_READY_SLOTS);
	CHECK(laneReadyPeak(&r) == LANE_READY_SLOTS);
	CHECK(laneReadyConsistent(&r));

	// Full. In the real worker this cannot happen -- a lane holds its claim until its result
	// has been installed -- but it must refuse rather than overwrite, because overwriting is
	// a silently lost column.
	CHECK(!laneReadyPush(&r, 0, 5, 6, true));

	// Completion order, not lane order.
	int lane = -1; int32_t cx = 0, cz = 0; bool ok = false;
	CHECK(laneReadyPop(&r, &lane, &cx, &cz, &ok));
	CHECK(lane == 0 && cx == 1 && cz == 2 && ok);
	CHECK(laneReadyPop(&r, &lane, &cx, &cz, &ok));
	CHECK(lane == 1 && cx == 3 && cz == 4 && !ok);
	CHECK(laneReadyCount(&r) == 0);
	CHECK(laneReadyConsistent(&r));

	// And it wraps rather than filling up once and stopping.
	for (int i = 0; i < 7; i++) {
		CHECK(laneReadyPush(&r, i % 2, i, i, true));
		CHECK(laneReadyPop(&r, &lane, &cx, &cz, &ok));
		CHECK(cx == i && cz == i);
		CHECK(laneReadyConsistent(&r));
	}
}

// ── Arms 2-4: the whole machine ───────────────────────────────────────────────────────
//
// This is app/worker.c's structure with LightLock swapped for a pthread mutex and the thread
// bodies rewritten around a poll instead of a LightEvent. The bookkeeping calls -- and their
// ORDER, which is the part that can be wrong -- are the same ones worker.c makes: claim at the
// pop, drop after the staging world has been emptied.

typedef struct {
	pthread_mutex_t lock;
	JobQueue        q;
	LaneClaims      claims;
	LaneReadyRing   ready;
	bool            done_submitting;
	int             dup_dropped;
	int             lost;                  // finished columns the ready ring refused
	int             disjoint_violations;   // sampled under the lock after every take
} Shared;

typedef struct {
	int             index;
	Shared*         sh;
	World           staging;
	WorldGenScratch wgs;
	int             generated;
	int             refused;
	int             popped;
} SimLane;

static void* simLane(void* p)
{
	SimLane* const ln = (SimLane*)p;
	Shared*  const sh = ln->sh;

	worldgenScratchInit(&ln->wgs);
	worldInit(&ln->staging);

	for (;;) {
		Job  job = {JOB_NONE, 0, 0, 0};
		bool got = false, quit = false;

		pthread_mutex_lock(&sh->lock);
		if (!laneClaimBusy(&sh->claims, ln->index)) {
			while (jobqPop(&sh->q, &job)) {
				if (laneClaimTake(&sh->claims, ln->index, job.cx, job.cz)) { got = true; break; }
				sh->dup_dropped++;
			}
			// Sampled here rather than in a checker thread, because here is the only moment
			// the set can have just become non-disjoint. A single violation is the defect.
			if (!laneClaimsDisjoint(&sh->claims)) sh->disjoint_violations++;
			if (!got && sh->done_submitting) quit = true;
		}
		pthread_mutex_unlock(&sh->lock);

		if (quit) break;
		if (!got) {
			// The real lane parks on a LightEvent. A 100 us sleep is the host stand-in and
			// costs nothing: the thing being measured is worldgenColumn, at ~1 ms a column.
			const struct timespec ts = {0, 100000};
			nanosleep(&ts, NULL);
			continue;
		}

		ln->popped++;
		if (worldgenColumn(&g_gen, &ln->wgs, &ln->staging, job.cx, job.cz)) ln->generated++;
		else                                                                ln->refused++;

		pthread_mutex_lock(&sh->lock);
		// The same failure handling app/worker.c's workerMain has, and it is here for the same
		// reason: a lane still holding a claim for a result nobody will ever install never takes
		// another job again. The column is lost either way, which is what `lost` counts.
		if (!laneReadyPush(&sh->ready, ln->index, job.cx, job.cz, true)) {
			laneClaimDrop(&sh->claims, ln->index);
			sh->lost++;
		}
		pthread_mutex_unlock(&sh->lock);
	}

	worldExit(&ln->staging);
	return NULL;
}

// Runs `nlanes` lanes over `njobs` jobs taken from `jobs[]`, installing every finished column
// on this thread exactly as main.c's genInstallOne does. `digest_out` (may be NULL) receives
// the digest of every column indexed by its entry in the column table, and `installs_out` how
// many times each was installed.
static double runMachine(int nlanes, const int* jobs, int njobs, int ncols,
                         uint32_t* digest_out, int* installs_out,
                         int* dup_out, int* violations_out, int* refused_out, int* lost_out)
{
	static SimLane lanes[WORKER_LANES_MAX];
	static Shared  sh;

	memset(&sh, 0, sizeof sh);
	pthread_mutex_init(&sh.lock, NULL);
	jobqInit(&sh.q);
	laneClaimsInit(&sh.claims);
	laneReadyInit(&sh.ready);

	for (int i = 0; i < nlanes; i++) {
		memset(&lanes[i], 0, sizeof lanes[i]);
		lanes[i].index = i;
		lanes[i].sh    = &sh;
	}

	int installed_total = 0;
	if (installs_out) for (int i = 0; i < ncols; i++) installs_out[i] = 0;

	const double t0 = nowMs();

	pthread_t th[WORKER_LANES_MAX];
	for (int i = 0; i < nlanes; i++) pthread_create(&th[i], NULL, simLane, &lanes[i]);

	// Submit everything up front. That is the boot case main.c really produces --
	// genRequestArea pushes the whole ring in one burst -- and it is also the arrangement
	// most likely to hand the two lanes the same column, which is what the claim set is for.
	pthread_mutex_lock(&sh.lock);
	for (int j = 0; j < njobs; j++) {
		int32_t cx = 0, cz = 0;
		colCoord(jobs[j], &cx, &cz);
		const Job job = {JOB_GENERATE, cx, cz, 0};
		jobqPush(&sh.q, job);
	}
	sh.done_submitting = true;
	pthread_mutex_unlock(&sh.lock);

	// The install loop. Runs until every job has become either an install or a duplicate.
	for (;;) {
		int lane = -1; int32_t rx = 0, rz = 0; bool ok = false;

		pthread_mutex_lock(&sh.lock);
		const bool ready = laneReadyPop(&sh.ready, &lane, &rx, &rz, &ok);
		const int  dups  = sh.dup_dropped;
		pthread_mutex_unlock(&sh.lock);

		if (!ready) {
			if (installed_total + dups >= njobs) break;
			// A deadline, so that a SABOTAGED build fails loudly instead of hanging the
			// suite. Every red arm this file documents kills columns on purpose, and a
			// harness that deadlocks under its own red arm cannot be used to prove
			// anything -- the run has to end and the checks have to go red.
			if (nowMs() - t0 > 20000.0) {
				printf("      STALLED after 20 s: installed %d + dup %d of %d jobs\n",
				       installed_total, dups, njobs);
				break;
			}
			const struct timespec ts = {0, 100000};
			nanosleep(&ts, NULL);
			continue;
		}

		// That lane is parked on its claim, so its staging world is ours until the drop
		// below -- the same handshake app/worker.c's workerInstall relies on.
		SimLane* const ln = &lanes[lane];
		if (digest_out) {
			for (int i = 0; i < ncols; i++) {
				int32_t cx = 0, cz = 0;
				colCoord(i, &cx, &cz);
				if (cx == rx && cz == rz) {
					digest_out[i] = digestColumn(&ln->staging, rx, rz);
					if (installs_out) installs_out[i]++;
					break;
				}
			}
		}
		// Stands in for workerInstall's worldExit(&ln->staging): the column leaves the
		// staging world and the budget goes back before the lane is let go.
		worldColumnRemove(&ln->staging, rx, rz);
		installed_total++;

		pthread_mutex_lock(&sh.lock);
		laneClaimDrop(&sh.claims, lane);
		pthread_mutex_unlock(&sh.lock);
	}

	for (int i = 0; i < nlanes; i++) pthread_join(th[i], NULL);
	const double ms = nowMs() - t0;

	int refused = 0;
	for (int i = 0; i < nlanes; i++) refused += lanes[i].refused;

	if (dup_out)        *dup_out        = sh.dup_dropped;
	if (violations_out) *violations_out = sh.disjoint_violations;
	if (refused_out)    *refused_out    = refused;
	if (lost_out)       *lost_out       = sh.lost;

	printf("      %d lane%s: installed %3d  dup %2d  ready-peak %d  popped %3d+%-3d  %7.1f ms\n",
	       nlanes, nlanes == 1 ? " " : "s", installed_total, sh.dup_dropped,
	       laneReadyPeak(&sh.ready), lanes[0].popped,
	       nlanes > 1 ? lanes[1].popped : 0, ms);

	pthread_mutex_destroy(&sh.lock);
	return ms;
}

int main(void)
{
	printf("two-lane generator self-test  (seed %08x, %d columns, lanes max %d)\n",
	       TEST_SEED, NCOLS, WORKER_LANES_MAX);

	registryInitCore();
	registryFreeze();
	if (!worldgenInit(&g_gen, TEST_SEED, GEN_VERSION_NEWEST)) {
		printf("FATAL: worldgenInit refused GEN_VERSION_NEWEST\n");
		return 2;
	}

	unitArm();

	// ── BASELINE: one thread, no lanes, nothing to race with ──────────────────────────
	printf("  BASELINE\n");
	{
		static World w;
		static WorldGenScratch wgs;
		worldgenScratchInit(&wgs);
		worldInit(&w);
		int made = 0;
		for (int i = 0; i < NCOLS; i++) {
			int32_t cx = 0, cz = 0;
			colCoord(i, &cx, &cz);
			if (worldgenColumn(&g_gen, &wgs, &w, cx, cz)) {
				g_baseline[i]    = digestColumn(&w, cx, cz);
				g_baseline_ok[i] = true;
				made++;
			}
			worldColumnRemove(&w, cx, cz);
		}
		worldExit(&w);
		printf("      generated %d/%d   col[0](-4,-2) %08x   col[17](1,0) %08x\n",
		       made, NCOLS, g_baseline[0], g_baseline[17]);
		CHECK(made == NCOLS);
	}

	// ── TWO-LANE: the whole machine, with every column submitted twice ────────────────
	printf("  TWO-LANE  (every column submitted twice: %d jobs)\n", NCOLS * 2);
	{
		// Interleaved -- c0, c0, c1, c1, ... -- so a duplicate is in the queue while its
		// original is still being generated, which is the arrangement that exercises the
		// claim set rather than merely visiting it.
		static int jobs[NCOLS * 2];
		for (int i = 0; i < NCOLS; i++) { jobs[i * 2] = i; jobs[i * 2 + 1] = i; }

		static uint32_t dig[NCOLS];
		static int      installs[NCOLS];
		int dups = 0, violations = 0, refused = 0, lost = 0;

		runMachine(WORKER_LANES_MAX, jobs, NCOLS * 2, NCOLS,
		           dig, installs, &dups, &violations, &refused, &lost);

		int wrong = 0, missing = 0, total_installs = 0;
		for (int i = 0; i < NCOLS; i++) {
			total_installs += installs[i];
			if (!g_baseline_ok[i]) continue;
			if (installs[i] == 0)          { missing++; continue; }
			if (dig[i] != g_baseline[i])     wrong++;
		}
		printf("      wrong %d/%d  missing %d  installs %d  refused %d  violations %d  lost %d\n",
		       wrong, NCOLS, missing, total_installs, refused, violations, lost);

		// The terrain is not affected by which lane made it.
		CHECK(wrong == 0);
		CHECK(missing == 0);

		// THE ACCOUNTING. Every job either produced a column or was refused as a duplicate.
		// A job that did neither is a hole in the world.
		CHECK(total_installs + dups == NCOLS * 2);

		// Nothing was generated and then thrown away because the ready ring had no room. The
		// ring has one slot per lane and a lane holds its claim until its result is installed,
		// so this is structurally impossible — and it is checked because "structurally
		// impossible" is what the last two stale constants in this project were as well.
		CHECK(lost == 0);

		// THE INVARIANT the claim set exists for.
		CHECK(violations == 0);

		// And the duplicate path was actually entered, so the two checks above are not
		// passing because nothing ever contended.
		CHECK(dups > 0);

		CHECK(refused == 0);
	}

	// ── TIMING: does the machine actually scale? ──────────────────────────────────────
	//
	// HOST FIGURES. An x86-64 host has more cores than a New 3DS and a completely different
	// memory system, so this says the claim set / queue / ready ring do not serialise the
	// generator. It does NOT say how much a New 3DS saves; nothing here can.
	printf("  TIMING  (%d distinct columns, %d repeats, HOST -- not a console figure)\n",
	       NTIME_COLS, TIME_REPEATS);
	{
		static int jobs[NTIME_COLS];
		for (int i = 0; i < NTIME_COLS; i++) jobs[i] = i % NCOLS;

		double best1 = 1e30, best2 = 1e30, worst1 = 0, worst2 = 0;
		for (int r = 0; r < TIME_REPEATS; r++) {
			const double m1 = runMachine(1, jobs, NTIME_COLS, NCOLS, NULL, NULL, NULL, NULL, NULL, NULL);
			const double m2 = runMachine(2, jobs, NTIME_COLS, NCOLS, NULL, NULL, NULL, NULL, NULL, NULL);
			if (m1 < best1) best1 = m1;
			if (m2 < best2) best2 = m2;
			if (m1 > worst1) worst1 = m1;
			if (m2 > worst2) worst2 = m2;
		}
		printf("      best 1 lane %7.1f ms (spread %.1f)   best 2 lanes %7.1f ms (spread %.1f)"
		       "   speedup %.2fx\n",
		       best1, worst1 - best1, best2, worst2 - best2, best1 / best2);

		// Deliberately loose. The claim is "two lanes are not SLOWER than one and the
		// bookkeeping does not serialise them", which is what this binary can honestly
		// support on a machine whose scheduler it does not control. A hard 1.8x here would
		// be a check about the build agent's load, not about this code.
		CHECK(best2 < best1);
	}

	// Every column was removed after its install, so a non-zero here is a leak rather than a
	// race -- checked so that a generator change which starts leaking cannot hide behind the
	// digest comparison.
	printf("  budget used %lu B (want 0), refusals %d\n",
	       (unsigned long)budgetUsed(), budgetRefusals());
	CHECK(budgetUsed() == 0);

	printf("two-lane generator self-test: %s - %d of %d checks\n",
	       g_failed ? "FAILED" : "PASS",
	       g_failed ? g_failed : g_checks, g_checks);
	return g_failed ? 1 : 0;
}
