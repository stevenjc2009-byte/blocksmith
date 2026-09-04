// v1.8.18 optlight task A. Proves the s_snapshot_busy CAS claim in light.c's
// relightColumnCoreDiff actually serialises access to the shared 16 KiB scratch buffer, and
// that the LOSING side (the claim not won) is not merely rare but CORRECT: it falls back to
// relightColumnCoreDiffStack, which is byte-for-byte the pre-existing snapshot/relight/diff
// logic, just moved off the common path's stack frame.
//
// Companion to tests/light_race_test.c, which already drives real concurrent threads through
// lightRelightColumn and would eventually hit this CAS by luck. This file does not replace
// that coverage -- it adds two things light_race_test.c cannot give a scheduler-dependent race:
//
//   ARM 1 forces the loss on purpose, single-threaded, via light.h's
//   lightClaimSnapshotScratchForTest()/lightReleaseSnapshotScratchForTest() test hooks (the
//   same CAS the real code claims, claimed from test code instead of a second thread), so the
//   fallback path is proven correct on a specific call rather than merely "probably exercised
//   somewhere in ARM 2's iteration count". light.c's own lightSnapshotFallbacks() counter is
//   read straight out of the real static, not reimplemented, so a build that stops counting
//   (or stops falling back at all) fails this file rather than passing it by accident.
//
//   ARM 2 hammers two DIFFERENT columns from two real threads, same shape and same
//   ARM2_COLUMN_B_CX=10 separation as light_race_test.c's ARM 2 (see that file's comment on
//   the constant for why adjacent columns are unsafe for an unrelated reason -- the
//   cross-column block-light handoff, not this CAS). This file's own probe during development
//   tried the same column from both threads first, to maximise contention on s_snapshot_busy,
//   and found real mismatches -- 2/200 and 2/200 -- but the SAME mismatches at the SAME rate
//   reproduce on the pre-optlight light.c with none of this task's changes applied, so that
//   hazard was relightColumnCore's own column-write path racing against itself when two
//   threads target ONE column concurrently -- pre-existing, and flagged rather than fixed when
//   this file first shipped. Two distinct columns was, at that point, what the project treated
//   as the supported concurrent-relight shape, and was what this arm used to check the
//   snapshot CAS specifically without also exercising that separate hazard.
//
//   ARM 3 is that original same-column design, restored, now that light.c's
//   lightColumnClaimTry/lightColumnClaimRelease (v1.8.18 optlight follow-up) claim a Column's
//   light buffer for the duration of a relight -- see the comment above lightColumnClaimTry in
//   light.c for the full reachability/mechanism/fix writeup. A refusal from the LOSING side is
//   correct behaviour here, unlike ARM 2 -- see relightChurnSameColumn's comment for why its
//   checking policy differs from relightChurn's.
//
// No hand-copied logic: all three arms call the real public API (lightRelightColumn) and the
// real test hooks declared in world/light.h and defined in world/light.c. Nothing here
// reimplements any part of the diffing, CAS or claim logic under test.

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

// Same failure shape as every other host binary in this suite: tools/run_host_tests.sh's
// callers grep for "^FAIL  L[0-9]+".
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

// Deterministic terrain with an overhang, same shape as light_race_test.c's buildColumn, so
// sky light has real spreading to do and the digest is not a trivial all-15/all-0 fixture.
static void buildColumn(World* w, int cx)
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			const int h = 30 + ((x * 3 + z * 5 + cx * 7) % 11);
			for (int y = 0; y <= h; y++)
				worldSet(w, cx * CHUNK_DIM + x, y, z, BLOCK_STONE);
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

// ── arm 1: force the loss, single-threaded, and check the fallback's answer ──────────

static void testForcedFallbackIsCorrect(void)
{
	printf("ARM 1 - force relightColumnCoreDiff's CAS to lose, check the fallback's answer\n");
	fflush(stdout);

	World w;
	worldInit(&w);
	buildColumn(&w, 0);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 1 is not testing it");

	// Reference: claim free, ordinary call, ordinary path. This must not itself have taken
	// the fallback -- if it did, the "forced" case below would prove nothing.
	lightResetSnapshotFallbacksForTest();
	CHECK(lightRelightColumn(&w, 0, 0), "reference relight refused");
	const uint32_t want = digestColumn(worldColumn(&w, 0, 0));
	printf("  REFERENCE DIGEST: %08x\n", want);
	CHECK(want != 0, "fixture column has no light channels");
	CHECK(lightSnapshotFallbacks() == 0,
	      "reference call already took the fallback (%d); arm 1's baseline is not clean",
	      lightSnapshotFallbacks());

	// Force the loss: claim the same static the real code claims, from test code.
	CHECK(lightClaimSnapshotScratchForTest(),
	      "could not claim the snapshot scratch buffer -- is something else already holding it?");

	lightResetSnapshotFallbacksForTest();
	const bool refused = !lightRelightColumn(&w, 0, 0);
	const uint32_t forced = digestColumn(worldColumn(&w, 0, 0));

	printf("  forced-fallback digest: %08x  fallbacks=%d  refused=%d\n",
	       forced, lightSnapshotFallbacks(), refused);
	fflush(stdout);

	CHECK(!refused, "relight was refused while the scratch buffer was held -- should have "
	                 "fallen back, not failed");
	CHECK(lightSnapshotFallbacks() == 1,
	      "lightSnapshotFallbacks() = %d after one forced-contention call, want exactly 1 -- "
	      "either the CAS is not checked or the fallback is not counted",
	      lightSnapshotFallbacks());
	CHECK(forced == want,
	      "fallback path produced digest %08x, want the same reference %08x -- the "
	      "stack-local snapshot fallback computed a WRONG answer under contention",
	      forced, want);

	lightReleaseSnapshotScratchForTest();

	// With the claim released, the next call must win the CAS again -- the counter must not
	// keep climbing once contention stops.
	CHECK(lightRelightColumn(&w, 0, 0), "post-release relight refused");
	CHECK(digestColumn(worldColumn(&w, 0, 0)) == want,
	      "post-release relight did not reproduce the reference digest");
	CHECK(lightSnapshotFallbacks() == 1,
	      "lightSnapshotFallbacks() = %d after the claim was released, want still 1 -- "
	      "an uncontended call took the fallback anyway",
	      lightSnapshotFallbacks());

	lightEngineInit(false);
	worldExit(&w);
}

// ── arm 2: real contention, two distinct columns, from two real threads ────────────

#define RELIGHT_ITERS  200

// Same separation and same reasoning as light_race_test.c's ARM2_COLUMN_B_CX: far enough
// apart that lightHandoffBorders' cross-column read on either thread only ever sees a NULL
// (not loaded) neighbour, never the other thread's column.
#define ARM2_COLUMN_B_CX  10

typedef struct {
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

static void testRealContentionHitsTheFallback(void)
{
	printf("ARM 2 - relight two DIFFERENT columns from two real threads, %d iterations each, "
	       "to force genuine contention on the shared scratch buffer\n", RELIGHT_ITERS);
	fflush(stdout);

	World w;
	worldInit(&w);
	buildColumn(&w, 0);
	buildColumn(&w, ARM2_COLUMN_B_CX);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 2 is not testing it");

	CHECK(lightRelightColumn(&w, 0, 0), "reference relight of column 0 refused");
	CHECK(lightRelightColumn(&w, ARM2_COLUMN_B_CX, 0), "reference relight of column B refused");
	const uint32_t want0 = digestColumn(worldColumn(&w, 0, 0));
	const uint32_t want1 = digestColumn(worldColumn(&w, ARM2_COLUMN_B_CX, 0));
	printf("  REFERENCE DIGEST column 0: %08x\n", want0);
	printf("  REFERENCE DIGEST column 1: %08x\n", want1);
	CHECK(want0 != 0, "fixture column 0 has no light channels");
	CHECK(want1 != 0, "fixture column B has no light channels");
	CHECK(want0 != want1, "the two fixture columns light identically; arm 2 is blind");

	lightResetSnapshotFallbacksForTest();

	RelightArg a0 = { .world = &w, .cx = 0, .want = want0 };
	RelightArg a1 = { .world = &w, .cx = ARM2_COLUMN_B_CX, .want = want1 };

	pthread_t t0, t1;
	pthread_create(&t0, NULL, relightChurn, &a0);
	pthread_create(&t1, NULL, relightChurn, &a1);
	pthread_join(t0, NULL);
	pthread_join(t1, NULL);

	const int fallbacks = lightSnapshotFallbacks();
	printf("  thread 0: %d/%d wrong digests, %d refusals\n", a0.mismatches, RELIGHT_ITERS,
	       a0.refusals);
	printf("  thread 1: %d/%d wrong digests, %d refusals\n", a1.mismatches, RELIGHT_ITERS,
	       a1.refusals);
	printf("  real CAS fallbacks under contention: %d (of %d calls)\n", fallbacks,
	       2 * RELIGHT_ITERS);
	fflush(stdout);

	CHECK(a0.mismatches == 0, "thread 0 relit WRONG %d of %d times under contention",
	      a0.mismatches, RELIGHT_ITERS);
	CHECK(a1.mismatches == 0, "thread 1 relit WRONG %d of %d times under contention",
	      a1.mismatches, RELIGHT_ITERS);
	CHECK(a0.refusals == 0, "thread 0 relight refused %d times under contention", a0.refusals);
	CHECK(a1.refusals == 0, "thread 1 relight refused %d times under contention", a1.refusals);
	// The point of relighting the SAME column from two threads at once: real contention on
	// s_snapshot_busy must actually happen, not just be theoretically possible. If this ever
	// reads 0, either the two threads stopped overlapping (weaken RELIGHT_ITERS' cost, not
	// this check) or the CAS/counter broke.
	CHECK(fallbacks > 0,
	      "lightSnapshotFallbacks() = 0 after %d concurrent calls on one column -- real "
	      "contention never reached the fallback path", 2 * RELIGHT_ITERS);

	lightEngineInit(false);
	worldExit(&w);
}

// ── arm 3: real contention, the SAME column, from two real threads ──────────────────

// Unlike relightChurn above: a refused call is CORRECT here, on purpose -- see
// lightColumnClaimTry's comment in light.c. The losing side of the per-column claim returns
// false WITHOUT touching col->light, precisely so the winning side's write cannot be
// interfered with -- which also means the loser's digest read is meaningless to take right
// then, since the winner may still be mid-write on the very column the loser just refused to
// touch. Only a call that itself won the claim and completed proves anything about
// correctness, so only THAT digest is checked; a refusal is counted, not treated as a
// mismatch.
static void* relightChurnSameColumn(void* p)
{
	RelightArg* a = (RelightArg*)p;
	for (int i = 0; i < RELIGHT_ITERS; i++) {
		if (!lightRelightColumn(a->world, a->cx, 0)) { a->refusals++; continue; }
		if (digestColumn(worldColumn(a->world, a->cx, 0)) != a->want) a->mismatches++;
	}
	return NULL;
}

static void testSameColumnContentionIsCorrect(void)
{
	printf("ARM 3 - relight the SAME column from two real threads, %d iterations each, to "
	       "prove two concurrent relights of ONE column can never corrupt its light buffer\n",
	       RELIGHT_ITERS);
	fflush(stdout);

	World w;
	worldInit(&w);
	buildColumn(&w, 0);

	lightEngineInit(true);
	CHECK(lightFastEngineReady(), "the flood-fill engine is not ready; arm 3 is not testing it");

	CHECK(lightRelightColumn(&w, 0, 0), "reference relight refused");
	const uint32_t want = digestColumn(worldColumn(&w, 0, 0));
	printf("  REFERENCE DIGEST: %08x\n", want);
	CHECK(want != 0, "fixture column has no light channels");

	RelightArg a0 = { .world = &w, .cx = 0, .want = want };
	RelightArg a1 = { .world = &w, .cx = 0, .want = want };

	pthread_t t0, t1;
	pthread_create(&t0, NULL, relightChurnSameColumn, &a0);
	pthread_create(&t1, NULL, relightChurnSameColumn, &a1);
	pthread_join(t0, NULL);
	pthread_join(t1, NULL);

	printf("  thread 0: %d/%d wrong digests, %d refusals\n", a0.mismatches, RELIGHT_ITERS,
	       a0.refusals);
	printf("  thread 1: %d/%d wrong digests, %d refusals\n", a1.mismatches, RELIGHT_ITERS,
	       a1.refusals);
	fflush(stdout);

	// The property that matters: a call that WON the claim and completed must never produce a
	// wrong answer. A refusal is not a failure -- see relightChurnSameColumn's comment -- so
	// unlike ARM 2 there is no refusals == 0 check here; refusing is the fix working.
	CHECK(a0.mismatches == 0, "thread 0 relit WRONG %d of %d times on the SAME column -- the "
	      "per-column claim did not stop the race", a0.mismatches, RELIGHT_ITERS);
	CHECK(a1.mismatches == 0, "thread 1 relit WRONG %d of %d times on the SAME column -- the "
	      "per-column claim did not stop the race", a1.mismatches, RELIGHT_ITERS);
	// Positive control: this arm proves nothing if the two threads never actually contended for
	// the same column's claim. See "An empty grep is not proof of absence" -- the equivalent
	// here is a check that cannot go red, and a refusal count of 0 both times would be exactly
	// that.
	CHECK(a0.refusals + a1.refusals > 0,
	      "0 refusals across %d concurrent same-column calls -- the two threads never actually "
	      "contended for the claim, so this arm did not exercise it", 2 * RELIGHT_ITERS);

	lightEngineInit(false);
	worldExit(&w);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	registryInitCore();
	budgetReset();

	testForcedFallbackIsCorrect();
	testRealContentionHitsTheFallback();
	testSameColumnContentionIsCorrect();

	if (s_failed) {
		printf("light snapshot CAS self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("light snapshot CAS self-test: PASS %d checks\n", s_checks);
	return 0;
}
