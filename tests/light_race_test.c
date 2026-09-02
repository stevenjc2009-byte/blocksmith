// v1.8.7. world/light.c's file-static state, driven from TWO REAL THREADS.
//
// Every other host binary in this suite is single-threaded, which is exactly why the
// defects this file exists for survived: light.c's statics are only wrong when two
// threads reach them at once, and nothing here had ever done that.
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

static World s_world;

// Two columns with different relief, so the two threads cannot accidentally agree by
// computing the same answer. Terrain only — no emitters, because no core row declares a
// luminance and the block channel is therefore all zero on a single-player world; the sky
// channel is what a wrong queue entry shows up in.
static void buildColumn(int cx)
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			const int h = 30 + ((x * 3 + z * 5 + cx * 7) % 11);
			for (int y = 0; y <= h; y++)
				worldSet(&s_world, cx * CHUNK_DIM + x, y, z, BLOCK_STONE);
			// An overhang two above the surface, so sky light has to spread sideways
			// under it instead of falling straight down everywhere.
			if (((x + z + cx) & 3) == 0)
				worldSet(&s_world, cx * CHUNK_DIM + x, h + 3, z, BLOCK_STONE);
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
	int      cx;
	uint32_t want;
	int      mismatches;
	int      refusals;
} RelightArg;

static void* relightChurn(void* p)
{
	RelightArg* a = (RelightArg*)p;
	for (int i = 0; i < RELIGHT_ITERS; i++) {
		if (!lightRelightColumn(&s_world, a->cx, 0)) a->refusals++;
		if (digestColumn(worldColumn(&s_world, a->cx, 0)) != a->want) a->mismatches++;
	}
	return NULL;
}

static void testRelightUnderContention(void)
{
	printf("ARM 2 - lightRelightColumn on two columns from two threads, %d iterations each\n",
	       RELIGHT_ITERS);
	fflush(stdout);

	worldInit(&s_world);
	buildColumn(0);
	buildColumn(1);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 2 is not testing it");

	// The reference, computed with nothing else running. These two numbers are the
	// engine's output on this fixture: they must not move for any reason other than a
	// deliberate change to what light.c computes.
	CHECK(lightRelightColumn(&s_world, 0, 0), "reference relight of column 0 refused");
	CHECK(lightRelightColumn(&s_world, 1, 0), "reference relight of column 1 refused");

	RelightArg a0 = { 0, digestColumn(worldColumn(&s_world, 0, 0)), 0, 0 };
	RelightArg a1 = { 1, digestColumn(worldColumn(&s_world, 1, 0)), 0, 0 };

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

	if (s_failed) {
		printf("light race self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("light race self-test: PASS %d checks\n", s_checks);
	return 0;
}
