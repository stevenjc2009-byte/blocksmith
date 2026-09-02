// v1.8.7. world/light.c's file-static state, driven from TWO REAL THREADS.
//
// Every other host binary in this suite is single-threaded, which is exactly why the
// defects this file exists for survived: light.c's statics are only wrong when two
// threads reach them at once, and nothing here had ever done that.
//
// v1.8.9 ARM 3 addendum. The two-thread topology above modelled v1.8.7: one worker
// thread plus the main thread. It stopped being the whole story at v1.8.8, when
// app/worker.c gained a SECOND generator lane (worker.c:577-588, started on core 0
// whenever lane 0 actually landed on core 2 -- see worker.c's WORKER_LANE1_CORE
// comment). A real New 3DS that got its CanAccessCore2/804MHz/L2 grant (cia/
// blocksmith.rsf, app/hw.c) then runs THREE concurrent callers into this file's
// statics: lane 0 (core 2, genuinely parallel), lane 1 (core 0), and the main thread
// (core 0, timesliced with lane 1) -- and nothing in this file had ever driven three
// at once. ARM 3 below adds the third caller without removing ARM 2's two-thread
// case, so a regression that only shows at N=2 is still caught.
//
// WHO THE TWO THREADS ARE IN THE SHIPPING GAME. The worker thread reaches light.c from
// JOB_GENERATE (app/worker.c:286, lightPropagateColumn on the staging world, falling back
// to lightRelightColumn on a refusal at :287). It is NOT parked while it does that — the
// parked-worker handshake app/worker.h describes covers workerInstall only. The main
// thread reaches the same statics from the edit path (scene/interact.c:115 and :309,
// main.c:2958, world/relight_drain.c:26, all lightRelightColumn) and from the streaming
// unload path (world/world.c:148, worldColumnRemove -> lightColumnDetach). On a New 3DS
// the worker sits on core 2 (app/hw.c:8, app/worker.c's core ladder), so the two are
// genuinely simultaneous rather than timesliced.
//
// pthreads here stands in for libctru's threadCreate. The property under test is not
// which threading API is used, it is whether two concurrent callers of the real
// world/light.c entry points can corrupt one another's state — and that is the same
// question on both.
//
// ARM 1 counts. lightColumnAttach/lightColumnDetach did `s_attached++/--`, a non-atomic
// read-modify-write. Lost updates drift lightColumnsAttached() and lightBytesUsed()
// permanently away from the truth.
//
// ARM 2 is the one that changes pixels. Since v1.8.0 lightRelightColumn runs the flood
// fill over a SINGLE file-static queue (light.c's s_edit_queue) instead of the queue-free
// sweeps it used to be. app/worker.c:108's comment still says "Owned by this thread for
// its whole life — the main thread's edit path uses the queue-free sweep engine precisely
// so the two never share one", which was true until v1.8.0 and has not been true since:
// worker.c:287 calls lightRelightColumn too. Two threads then drive one LightQueue at
// once. This arm relights two DIFFERENT columns from two threads and compares each
// column's 32 KiB against the digest the same call produces single-threaded, so a
// disagreement is a wrong light value and nothing else.
//
// The digests ARM 2 prints are also this file's second job: they are a fingerprint of the
// engine's output on a fixed fixture, so any change to light.c that moves a single nibble
// moves them.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/budget.h"
#include "world/light.h"
#include "world/registry.h"
#include "world/world.h"

static int s_checks;
static int s_failed;

// The suite's failure shape: tools/run_host_tests.sh's callers grep for "^FAIL  L[0-9]+".
#define CHECK(cond, ...)                                    \
	do {                                                    \
		s_checks++;                                         \
		if (!(cond)) {                                      \
			s_failed++;                                     \
			printf("FAIL  L%d  ", __LINE__);                \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
			fflush(stdout);                                 \
		}                                                   \
	} while (0)

// ── arm 1: the attach/detach counter ─────────────────────────────────────────

#define ATTACH_ITERS  120000

// A bare Column, not one owned by a World: the counter is the subject here, and going
// through worldColumnCreate would drag the world hash table into an arm that is not
// testing it. lightColumnAttach only ever looks at col->light.
static Column g_col_a;
static Column g_col_b;

static void* attachChurn(void* arg)
{
	Column* col = (Column*)arg;
	for (int i = 0; i < ATTACH_ITERS; i++) {
		lightColumnAttach(col);
		lightColumnDetach(col);
	}
	return NULL;
}

static void testAttachCounterUnderContention(void)
{
	printf("ARM 1 - lightColumnAttach/Detach from two threads, %d iterations each\n",
	       ATTACH_ITERS);
	fflush(stdout);

	const int before = lightColumnsAttached();
	CHECK(before == 0, "baseline lightColumnsAttached() = %d, want 0", before);

	pthread_t ta, tb;
	pthread_create(&ta, NULL, attachChurn, &g_col_a);
	pthread_create(&tb, NULL, attachChurn, &g_col_b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);

	const int after = lightColumnsAttached();
	printf("  attached after: %d (want 0, drift %+d)\n", after, after - before);
	printf("  budget used   : %lu B (want 0)\n", (unsigned long)budgetUsed());
	fflush(stdout);

	// Every attach is paired with a detach on the same column, so the counter must come
	// back to where it started no matter how the two threads interleave.
	CHECK(after == 0, "lightColumnsAttached() = %d after balanced churn, want 0", after);
	CHECK(budgetUsed() == 0, "budgetUsed() = %lu after balanced churn, want 0",
	      (unsigned long)budgetUsed());
}

// ── arm 2: the shared edit queue ─────────────────────────────────────────────

#define RELIGHT_ITERS  60

// v1.8.10: the second fixture column used to be cx=1, directly adjacent to column 0. The
// cross-column block-light handoff added this version (light.c's lightHandoffBorders) walks
// a column's LOADED neighbours from whichever thread relit it, which light.c's own comment
// above lightHandoffBorders documents as safe only because no real caller ever hands it a
// multi-column World from more than one thread at once — this arm is deliberately the one
// exception, so it must not let the two threads' columns be neighbours of each other. This
// fixture's block channel stays all-zero regardless (buildColumn only ever places stone, and
// nothing here registers a luminance), so the handoff does not currently fire for either
// column either way — but that is a property of what buildColumn happens to place, not of
// the coordinates, and a future edit adding an emitter here should not have to rediscover
// this. cx=10 puts column 0's four immediate neighbours (cx=-1,1 / cz=-1,1) nowhere near
// column B, so lightHandoffBorders on either thread only ever reads worldColumn() misses
// (NULL, not loaded) for the other thread's column, never the column itself.
#define ARM2_COLUMN_B_CX  10

static World s_world;

// Two columns with different relief, so the two threads cannot accidentally agree by
// computing the same answer. Terrain only — stone, no torches — so the block channel is
// all zero for both columns regardless of BLOCK_TORCH now being in the core registry
// (v1.8.10); a registered luminance only matters for a block actually placed, and
// buildColumn never places one. The sky channel is what a wrong queue entry shows up in.
// v1.8.9: takes the World explicitly rather than hardcoding &s_world, so ARM 3 below
// can build the same fixture terrain into a lane's own PRIVATE World -- exactly what
// app/worker.c's Lane.staging is -- without dragging that column into the shared
// multi-column s_world ARM 2 already uses.
static void buildColumn(World* w, int cx)
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			const int h = 30 + ((x * 3 + z * 5 + cx * 7) % 11);
			for (int y = 0; y <= h; y++)
				worldSet(w, cx * CHUNK_DIM + x, y, z, BLOCK_STONE);
			// An overhang two above the surface, so sky light has to spread sideways
			// under it instead of falling straight down everywhere.
			if (((x + z + cx) & 3) == 0)
				worldSet(w, cx * CHUNK_DIM + x, h + 3, z, BLOCK_STONE);
		}
	}
}

static uint32_t digestColumn(const Column* col)
{
	const uint8_t* sky = lightChannelSky(col);
	const uint8_t* blk = lightChannelBlock(col);
	if (!sky || !blk) return 0;

	uint32_t h = 2166136261u;   // FNV-1a
	for (int i = 0; i < LIGHT_COL_BYTES; i++) { h ^= sky[i]; h *= 16777619u; }
	for (int i = 0; i < LIGHT_COL_BYTES; i++) { h ^= blk[i]; h *= 16777619u; }
	return h;
}

typedef struct {
	// v1.8.9: which World this caller relights in, explicit rather than always
	// &s_world -- ARM 2's two threads still share one live multi-column World (that
	// IS the scenario being stressed there), but ARM 3's lane-style callers each get
	// their own PRIVATE single-column World, matching app/worker.c's Lane.staging.
	World*   world;
	int      cx;
	uint32_t want;
	int      mismatches;
	int      refusals;
} RelightArg;

static void* relightChurn(void* p)
{
	RelightArg* a = (RelightArg*)p;
	for (int i = 0; i < RELIGHT_ITERS; i++) {
		if (!lightRelightColumn(a->world, a->cx, 0)) a->refusals++;
		if (digestColumn(worldColumn(a->world, a->cx, 0)) != a->want) a->mismatches++;
	}
	return NULL;
}

static void testRelightUnderContention(void)
{
	printf("ARM 2 - lightRelightColumn on two columns from two threads, %d iterations each\n",
	       RELIGHT_ITERS);
	fflush(stdout);

	worldInit(&s_world);
	buildColumn(&s_world, 0);
	buildColumn(&s_world, ARM2_COLUMN_B_CX);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 2 is not testing it");

	// The reference, computed with nothing else running. These two numbers are the
	// engine's output on this fixture: they must not move for any reason other than a
	// deliberate change to what light.c computes.
	CHECK(lightRelightColumn(&s_world, 0, 0), "reference relight of column 0 refused");
	CHECK(lightRelightColumn(&s_world, ARM2_COLUMN_B_CX, 0), "reference relight of column B refused");

	RelightArg a0 = { .world = &s_world, .cx = 0,
	                  .want = digestColumn(worldColumn(&s_world, 0, 0)) };
	RelightArg a1 = { .world = &s_world, .cx = ARM2_COLUMN_B_CX,
	                  .want = digestColumn(worldColumn(&s_world, ARM2_COLUMN_B_CX, 0)) };

	printf("  REFERENCE DIGEST column 0: %08x\n", a0.want);
	printf("  REFERENCE DIGEST column 1: %08x\n", a1.want);
	fflush(stdout);

	CHECK(a0.want != 0, "column 0 has no light channels");
	CHECK(a1.want != 0, "column 1 has no light channels");
	CHECK(a0.want != a1.want, "the two fixture columns light identically; arm 2 is blind");

	// Repeatability with one thread, so a mismatch below can only be the second thread.
	CHECK(lightRelightColumn(&s_world, 0, 0), "repeat relight of column 0 refused");
	CHECK(digestColumn(worldColumn(&s_world, 0, 0)) == a0.want,
	      "column 0 does not relight to the same value twice single-threaded");

	pthread_t t0, t1;
	pthread_create(&t0, NULL, relightChurn, &a0);
	pthread_create(&t1, NULL, relightChurn, &a1);
	pthread_join(t0, NULL);
	pthread_join(t1, NULL);

	printf("  column 0: %d/%d wrong digests, %d refusals\n",
	       a0.mismatches, RELIGHT_ITERS, a0.refusals);
	printf("  column 1: %d/%d wrong digests, %d refusals\n",
	       a1.mismatches, RELIGHT_ITERS, a1.refusals);
	fflush(stdout);

	CHECK(a0.mismatches == 0, "column 0 relit WRONG %d of %d times under contention",
	      a0.mismatches, RELIGHT_ITERS);
	CHECK(a1.mismatches == 0, "column 1 relit WRONG %d of %d times under contention",
	      a1.mismatches, RELIGHT_ITERS);
	CHECK(a0.refusals == 0, "column 0 relight refused %d times under contention",
	      a0.refusals);
	CHECK(a1.refusals == 0, "column 1 relight refused %d times under contention",
	      a1.refusals);

	lightEngineInit(false);
	worldExit(&s_world);
}

// ── arm 3: the real New 3DS topology -- three concurrent callers ────────────────
//
// v1.8.9 diagnosis (reading refs/tags/v1.8.9, the build that froze the owner's real
// New 3DS): the resource grant IS correct end to end -- cia/blocksmith.rsf asks for
// SystemModeExt/CpuSpeed 804MHz/EnableL2Cache/CanAccessCore2, app/hw.c also calls
// osSetSpeedupEnable(true), hwInit() runs first in main(). app/worker.c:518-551 puts
// lane 0 on core 2 when that grant lands, and worker.c:577-588 starts a SECOND
// generator lane on core 0 if and only if lane 0 actually got core 2. So a real New
// 3DS install drives three simultaneous callers into this file's shared statics:
// lane 0 (core 2, genuinely parallel), lane 1 (core 0), and the main thread (core 0,
// timesliced with lane 1). ARM 2 above has only ever driven two.
//
// WHAT LANE 1 ACTUALLY DOES, read out of app/worker.c's workerMain -- there is no
// lane-specific code path, lane 0 and lane 1 run the identical function on their own
// Lane struct, so "lane 1's work" is not a new workload to invent, it is a second
// instance of exactly what lane 0 already does: JOB_GENERATE tries
// lightPropagateColumn(&ln->staging, cx, cz, ln->lightq) -- the lane's OWN staging
// World and OWN queue -- and only calls lightRelightColumn, which CAS-claims this
// file's shared s_edit_queue, as a FALLBACK when that refuses (worker.c:396-397).
//
// Gating this arm's calls behind a propagate attempt the way worker.c does would
// test nothing: propagate's queue is sized to LIGHT_COL_CELLS and, by construction
// (light.h's own comment -- a cell is enqueued only once, on its first strict
// improvement), cannot overflow, and nothing in this fixture places an emitter or
// gets near budget's 12 MB cap -- so it would never refuse and this arm's threads
// would call lightRelightColumn zero times, exercising the CAS not at all. Calling
// lightRelightColumn directly and unconditionally is worker.c:397 verbatim -- same
// function, same arguments, same shared statics -- taken every iteration instead of
// on the rare real refusal. That is the right thing to stress: this arm is asking
// whether the CAS holds when THREE callers are actually inside it together, not how
// often a real session happens to put lane 1 there.
//
// Two PRIVATE, single-column Worlds -- not two more columns in s_world -- because
// that is what app/worker.c's Lane.staging actually is: one World per lane, holding
// exactly one column at a time, emptied by workerInstall between jobs. It is also
// why light.c's cross-column handoff (lightHandoffBorders) is a genuine no-op for
// these two threads' calls rather than a risk that needs managing here: light.c's
// own THREAD SAFETY comment (above LIGHT_HANDOFF_MAX_DEPTH) says a worker's staging
// world never has a second column loaded, so every neighbour lookup it makes reads
// NULL -- true here for the identical reason, not assumed.
static World s_lane0_world;
static World s_lane1_world;

// Coordinates only need to differ so the three printed digests are visually
// distinguishable in the output below; they do NOT need to differ for correctness
// the way ARM 2's two columns in ONE shared World did (that reuse guarded against a
// same-world cross-column mixup masking a wrong answer as a right one by
// coincidence). These three calls land in three separate World structs -- distinct
// heap allocations under distinct Column objects -- so a queue corruption cannot
// silently relocate one thread's bytes into another's and still read as correct.
#define ARM3_LANE0_CX  0
#define ARM3_LANE1_CX  1
#define ARM3_MAIN_CX   20

static void testRelightUnderContentionThreeCallers(void)
{
	printf("ARM 3 - lightRelightColumn from THREE threads (lane 0, lane 1, main), "
	       "%d iterations each\n", RELIGHT_ITERS);
	fflush(stdout);

	worldInit(&s_world);
	worldInit(&s_lane0_world);
	worldInit(&s_lane1_world);
	buildColumn(&s_world,       ARM3_MAIN_CX);
	buildColumn(&s_lane0_world, ARM3_LANE0_CX);
	buildColumn(&s_lane1_world, ARM3_LANE1_CX);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 3 is not testing it");

	CHECK(lightRelightColumn(&s_world,       ARM3_MAIN_CX,  0), "reference relight of main-thread column refused");
	CHECK(lightRelightColumn(&s_lane0_world, ARM3_LANE0_CX, 0), "reference relight of lane 0's column refused");
	CHECK(lightRelightColumn(&s_lane1_world, ARM3_LANE1_CX, 0), "reference relight of lane 1's column refused");

	RelightArg main_arg = { .world = &s_world, .cx = ARM3_MAIN_CX,
	                         .want = digestColumn(worldColumn(&s_world, ARM3_MAIN_CX, 0)) };
	RelightArg lane0_arg = { .world = &s_lane0_world, .cx = ARM3_LANE0_CX,
	                          .want = digestColumn(worldColumn(&s_lane0_world, ARM3_LANE0_CX, 0)) };
	RelightArg lane1_arg = { .world = &s_lane1_world, .cx = ARM3_LANE1_CX,
	                          .want = digestColumn(worldColumn(&s_lane1_world, ARM3_LANE1_CX, 0)) };

	printf("  REFERENCE DIGEST main   : %08x\n", main_arg.want);
	printf("  REFERENCE DIGEST lane 0 : %08x\n", lane0_arg.want);
	printf("  REFERENCE DIGEST lane 1 : %08x\n", lane1_arg.want);
	fflush(stdout);

	CHECK(main_arg.want != 0, "main-thread column has no light channels");
	CHECK(lane0_arg.want != 0, "lane 0's column has no light channels");
	CHECK(lane1_arg.want != 0, "lane 1's column has no light channels");

	pthread_t t_main, t_lane0, t_lane1;
	pthread_create(&t_main,  NULL, relightChurn, &main_arg);
	pthread_create(&t_lane0, NULL, relightChurn, &lane0_arg);
	pthread_create(&t_lane1, NULL, relightChurn, &lane1_arg);
	pthread_join(t_main,  NULL);
	pthread_join(t_lane0, NULL);
	pthread_join(t_lane1, NULL);

	printf("  main   : %d/%d wrong digests, %d refusals\n",
	       main_arg.mismatches, RELIGHT_ITERS, main_arg.refusals);
	printf("  lane 0 : %d/%d wrong digests, %d refusals\n",
	       lane0_arg.mismatches, RELIGHT_ITERS, lane0_arg.refusals);
	printf("  lane 1 : %d/%d wrong digests, %d refusals\n",
	       lane1_arg.mismatches, RELIGHT_ITERS, lane1_arg.refusals);
	fflush(stdout);

	CHECK(main_arg.mismatches == 0, "main-thread column relit WRONG %d of %d times under 3-way contention",
	      main_arg.mismatches, RELIGHT_ITERS);
	CHECK(lane0_arg.mismatches == 0, "lane 0's column relit WRONG %d of %d times under 3-way contention",
	      lane0_arg.mismatches, RELIGHT_ITERS);
	CHECK(lane1_arg.mismatches == 0, "lane 1's column relit WRONG %d of %d times under 3-way contention",
	      lane1_arg.mismatches, RELIGHT_ITERS);
	CHECK(main_arg.refusals == 0, "main-thread column relight refused %d times under 3-way contention",
	      main_arg.refusals);
	CHECK(lane0_arg.refusals == 0, "lane 0's column relight refused %d times under 3-way contention",
	      lane0_arg.refusals);
	CHECK(lane1_arg.refusals == 0, "lane 1's column relight refused %d times under 3-way contention",
	      lane1_arg.refusals);

	lightEngineInit(false);
	worldExit(&s_world);
	worldExit(&s_lane0_world);
	worldExit(&s_lane1_world);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	registryInitCore();
	budgetReset();

	// Arm 1 first and flushed: arm 2 corrupts a shared queue when it is red, and an
	// out-of-range queue entry writes outside the 16 KiB channel. A crash there must not
	// take arm 1's result with it.
	testAttachCounterUnderContention();
	testRelightUnderContention();
	// Arm 3 after arm 2, same reasoning: keeps the N=2 case that already caught a real
	// bug (see this file's header) intact and separately checked, and only then asks
	// the new question -- does the same CAS hold at N=3, the real New 3DS topology.
	testRelightUnderContentionThreeCallers();

	if (s_failed) {
		printf("light race self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("light race self-test: PASS %d checks\n", s_checks);
	return 0;
}
