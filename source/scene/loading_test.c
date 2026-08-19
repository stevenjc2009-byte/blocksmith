// Host self-test for scene/loading.c — the loading screen's state machine, which exists
// because the thing it replaced (genWaitSpawnColumn) hung a real console hard enough to kill
// the HOME button. A replacement for that cannot be trusted on the strength of reading it:
// the one property that matters is "it always leaves the waiting phases", and that is a claim
// about every input sequence, not about the happy one.
//
// Same shape and same reason as scene/ui_layout_test.c — own main(), whole file behind
// #ifndef __3DS__ so the Makefile's glob of source/scene into the console build does not link
// two main()s. The CHECK macro and the PASS/FAIL line are the project's, copied unchanged so
// a failure here reads like a failure anywhere else.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "scene/loading.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// The boot this project actually performs on an Old 3DS: render distance 1, so an area ring
// of radius 2 — 5x5 = 25 columns — and 40-odd chunk meshes across the 3x3 meshed columns.
#define O3DS_COLUMNS 25

static LoadingSample sampleBooting(int columns_in, int meshes, int queued)
{
	LoadingSample s = {0};
	s.columns_in    = columns_in;
	s.columns_total = O3DS_COLUMNS;
	s.meshes        = meshes;
	s.mesh_queued   = queued;
	s.worker_busy   = true;
	return s;
}

// Drives `frames` frames of the same sample, which is what a stall looks like from inside:
// nothing about the world changes and only the frame counter moves.
static LoadingPhase run(LoadingState* st, LoadingSample s, int frames)
{
	LoadingPhase p = st->phase;
	for (int i = 0; i < frames; i++) p = loadingStep(st, &s);
	return p;
}

// ── The happy boot ─────────────────────────────────────────────────────────────────────

static void testHealthyBootReachesReady(void)
{
	LoadingState st;
	loadingInit(&st);
	CHECK(st.phase == LOADING_GENERATING);

	// Columns arriving one per frame, spawn column not in yet.
	for (int i = 1; i <= 8; i++) {
		LoadingSample s = sampleBooting(i, 0, 0);
		CHECK(loadingStep(&st, &s) == LOADING_GENERATING);
	}

	// Spawn column in, ring not yet complete: still generating, because the mesher cannot
	// start on a column whose eight neighbours are not all present.
	LoadingSample s = sampleBooting(9, 0, 0);
	s.spawn_ready = true;
	CHECK(loadingStep(&st, &s) == LOADING_BUILDING);

	// Ring complete, geometry queued and draining.
	s = sampleBooting(25, 12, 28);
	s.spawn_ready = s.ring_ready = true;
	CHECK(loadingStep(&st, &s) == LOADING_BUILDING);

	s = sampleBooting(25, 40, 0);
	s.spawn_ready = s.ring_ready = true;
	s.worker_busy = false;
	CHECK(loadingStep(&st, &s) == LOADING_READY);
	CHECK(loadingPercent(&st) == 100);
	CHECK(st.reason[0] == '\0');   // a healthy boot never fills a reason string
}

// The handover must not happen while the mesh queue still holds work: that is precisely the
// "I'm in it, but there's no blocks actually loaded in" report — a world handed over before
// its geometry exists.
static void testNotReadyWhileGeometryOutstanding(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample s = sampleBooting(25, 3, 37);
	s.spawn_ready = s.ring_ready = true;
	s.worker_busy = false;
	CHECK(loadingStep(&st, &s) == LOADING_BUILDING);
	CHECK(st.phase != LOADING_READY);
}

// ── The reason this file exists: every wait must end ───────────────────────────────────

// The generator thread never delivered the spawn column and has nothing outstanding. The old
// blocking wait broke out of its loop here and dropped the player in above nothing.
static void testWorkerDiedStalls(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample s = sampleBooting(0, 0, 0);
	s.worker_busy = false;

	// Inside the grace window this is not yet believed — workerBusy() is legitimately false
	// for the instant between workerStart returning and the first job being picked up.
	CHECK(run(&st, s, 30) == LOADING_GENERATING);

	// Past it, it is.
	CHECK(run(&st, s, 60) == LOADING_STALLED);
	CHECK(strstr(st.reason, "generator stopped") != NULL);
	CHECK(strstr(st.reason, "col 0/25") != NULL);   // the counters travel with the reason
}

// Spawn column in, but a column of the ring was refused by the worker's queue, so the ring can
// never complete and nothing is outstanding to change that.
static void testIncompleteRingStalls(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample s = sampleBooting(24, 0, 0);
	s.spawn_ready   = true;
	s.worker_busy   = false;
	s.submit_failed = 1;

	CHECK(run(&st, s, 120) == LOADING_STALLED);
	CHECK(strstr(st.reason, "ring incomplete") != NULL);
	CHECK(strstr(st.reason, "s1") != NULL);         // submit_failed is in the string
}

// The nastiest case, and the one a timeout is for: everything still claims to be working —
// the worker says busy, the queue says there is geometry to build — and yet no number ever
// moves again. A card that stopped answering looks exactly like this.
static void testBusyForeverStalls(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample s = sampleBooting(9, 4, 20);
	s.spawn_ready = true;

	// The first frame establishes the baseline — progress moves from "no sample yet" to a real
	// value — so the run of identical frames starts on frame two and the stall lands on frame
	// LOADING_STALL_FRAMES + 1, not on LOADING_STALL_FRAMES. Measured, not assumed: this test
	// was written the other way round first and failed here.
	CHECK(run(&st, s, LOADING_STALL_FRAMES) == LOADING_BUILDING);
	CHECK(run(&st, s, 1) == LOADING_STALLED);
	CHECK(strstr(st.reason, "no progress") != NULL);
}

// Progress on any front resets the clock, so a slow boot is not a stalled one. Both terms of
// the progress scalar are checked, because a stall test that only watched columns would fire
// during a long mesh drain — 40 chunks at one chunk per frame is 40 frames of the column count
// standing perfectly still.
static void testProgressResetsTheStallClock(void)
{
	LoadingState st;
	loadingInit(&st);

	for (int round = 0; round < 4; round++) {
		LoadingSample s = sampleBooting(9, round, 20);
		s.spawn_ready = true;
		CHECK(run(&st, s, LOADING_STALL_FRAMES - 10) == LOADING_BUILDING);
	}

	LoadingState st2;
	loadingInit(&st2);
	for (int round = 0; round < 4; round++) {
		LoadingSample s = sampleBooting(round + 1, 0, 0);
		CHECK(run(&st2, s, LOADING_STALL_FRAMES - 10) == LOADING_GENERATING);
	}
}

// Both settled phases are terminal. Without this, a stalled screen offering "play anyway"
// could withdraw the offer on the next frame, and a ready world could go back to loading.
static void testSettledPhasesAreTerminal(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample dead = sampleBooting(0, 0, 0);
	dead.worker_busy = false;
	CHECK(run(&st, dead, 120) == LOADING_STALLED);

	LoadingSample healthy = sampleBooting(25, 40, 0);
	healthy.spawn_ready = healthy.ring_ready = true;
	CHECK(run(&st, healthy, 10) == LOADING_STALLED);   // stays stalled

	LoadingState st2;
	loadingInit(&st2);
	CHECK(run(&st2, healthy, 1) == LOADING_READY);
	CHECK(run(&st2, dead, 600) == LOADING_READY);      // stays ready
}

// ── The bar ────────────────────────────────────────────────────────────────────────────

static void testPercentIsMonotoneAndBounded(void)
{
	LoadingState st;
	loadingInit(&st);
	CHECK(loadingPercent(&st) == 0);

	int last = 0;
	// A mesh queue that grows faster than it drains — the case that makes the raw fraction
	// slide backwards — followed by the drain.
	const int meshes[] = {0, 2, 4, 6, 10, 20, 30, 40};
	const int queued[] = {8, 20, 36, 34, 30, 20, 10, 0};
	for (int i = 0; i < 8; i++) {
		LoadingSample s = sampleBooting(20 + i / 4, meshes[i], queued[i]);
		s.spawn_ready = s.ring_ready = true;
		loadingStep(&st, &s);
		const int pct = loadingPercent(&st);
		CHECK(pct >= last);
		CHECK(pct >= 0 && pct <= 100);
		last = pct;
	}
	CHECK(st.phase == LOADING_READY);   // the last sample has an empty queue
	CHECK(loadingPercent(&st) == 100);
}

// A zero column total cannot happen from main.c — the ring is at least 3x3 — but the divide
// is right there in the arithmetic, and a UI module that faults on a zero is a worse outcome
// than one that draws an empty bar.
static void testDegenerateSampleDoesNotDivideByZero(void)
{
	LoadingState st;
	loadingInit(&st);

	LoadingSample s = {0};
	s.worker_busy = true;
	CHECK(loadingStep(&st, &s) == LOADING_GENERATING);
	CHECK(loadingPercent(&st) == 0);
}

static void testStageLabelsAreNeverEmpty(void)
{
	LoadingState st;
	loadingInit(&st);
	const LoadingPhase phases[] = {LOADING_GENERATING, LOADING_BUILDING, LOADING_READY,
	                                LOADING_STALLED};
	for (int i = 0; i < 4; i++) {
		st.phase = phases[i];
		CHECK(loadingStageLabel(&st)[0] != '\0');
	}
}

int main(void)
{
	testHealthyBootReachesReady();
	testNotReadyWhileGeometryOutstanding();
	testWorkerDiedStalls();
	testIncompleteRingStalls();
	testBusyForeverStalls();
	testProgressResetsTheStallClock();
	testSettledPhasesAreTerminal();
	testPercentIsMonotoneAndBounded();
	testDegenerateSampleDoesNotDivideByZero();
	testStageLabelsAreNeverEmpty();

	if (s_fails == 0)
		printf("loading self-test: PASS  %d checks\n", s_checks);
	else
		printf("loading self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int loading_test_host_only_t;

#endif   // !__3DS__
