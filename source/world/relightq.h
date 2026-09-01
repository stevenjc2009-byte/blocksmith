// Coalescing worklist of columns awaiting a full relight (v1.8.0).
//
// lightRelightColumn recomputes a WHOLE column — 32768 cells — with a breadth-first flood
// fill: med 0.156 ms on the host at -O1, max 0.287 ms over 240 samples, and dearer on a 268
// MHz ARM11 with no L2 by a factor nobody has measured. main.c's onRemoteEdit
// used to call it once per remote edit, inline, which is fine for one player breaking one
// block and pathological for the case that actually produces edits in bulk: rejoining.
// networldOnColumnLoad (net/networld.c) drains every diff the server has ever stored for a
// column the instant that column installs, and every one of those diffs fires the edit
// hook — so N diffs on one column paid for N identical full relights of that same column
// where one would have done, on the frame a world is already at its busiest.
//
// v1.8.3: those two clauses read "several relaxation passes, 720.1 us ... and milliseconds on a
// 268 MHz ARM11" until now, and all three claims were wrong. The passes and the 720.1 us both
// describe the SWEEPS engine — world/light.h's lightRelightColumnSweeps — which is what
// lightRelightColumn ran until v1.8.0 and has not run since; the ARM11 figure was an
// extrapolation from it, and nothing has run on hardware since v1.2.5 to check either.
// Re-measured here rather than taken on trust, 240 samples per engine over six column profiles,
// gcc -O1: flood fill min 0.010 / med 0.046 / max 0.277 ms against sweeps min 0.329 / med 0.632
// / max 1.595 ms. Every one of the 240 flood-fill samples came in under 720.1 us; 720.1 us
// landed at the 73rd percentile of the sweeps. The two distributions do not meet, so the figure
// could only ever have come from the engine this queue no longer feeds.
//
// The headline numbers above are v1.8.3's own six-profile run (source/main.c's RELIGHT_BUDGET_MS
// carries it in full) rather than the run just quoted, so that the tree states ONE cost for this
// and not a fourth. A stale figure here is not a footnote: the per-frame budget over this very
// queue had to be measured from scratch because the tree held three figures for one relight that
// disagreed by 5x, and this line was the one that disagreed most.
//
// The relight is per-column, idempotent and order-independent (it memsets and recomputes
// from the blocks), so the fix is just to remember WHICH columns need one and do each once.
// That is all this is: a small set with push-time dedup, drained by the main loop.
//
// Bounded by construction rather than by hope: only a column that is already loaded can be
// pushed (net/networld.c's applyOrQueue refuses to apply an edit to a column that is not
// there), and the render distance caps loaded columns at RENDER_DIST_MAX_COLUMNS. The
// capacity below is above that with room to spare. A push that overflows anyway returns
// false and the caller relights inline exactly as it did before — slow, never wrong.
//
// v1.8.5: 64 -> 128, because RENDER_DIST_MAX moved 3 -> 5 and the ring went 49 -> 121 columns.
// 64 no longer cleared the structural bound. This is the ONE capacity in the project that
// caught its own staleness — world_test.c's `CHECK(RELIGHTQ_CAP >= RENDER_DIST_MAX_COLUMNS)`
// went red the moment the ceiling moved, which is exactly what world/jobq.h's JOBQ_CAP did NOT
// do (it had no assert, compiled clean, and would have silently dropped mesh jobs; it now has
// one, in main.c beside genQueueReadyColumns). Cost of the lift: the struct is two int16 arrays,
// so 64 more entries is 256 bytes.
//
// Overflow here is graceful and always was — worth restating, because it is why 128 rather than
// some larger number with a comfort margin is the right size: 121 is a hard structural ceiling,
// not a busy-case estimate, so nothing can ever ask for a 122nd column.
//
// No <3ds.h>: host-testable like the rest of source/world, and the same shape as the
// worklists already here (dirtyq.h, jobq.h, meshq.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RELIGHTQ_CAP 128

typedef struct {
	int16_t cx[RELIGHTQ_CAP];
	int16_t cz[RELIGHTQ_CAP];
	int     count;
	int     coalesced;   // pushes that found the column already listed — the whole point
	int     overflows;   // pushes refused because the set was full; caller relit inline
} RelightQueue;

void relightqInit(RelightQueue* q);

// Records (cx,cz) as needing a relight. True when it is now listed — whether this call put
// it there or a previous one did. False ONLY when the set is full, and the caller must then
// relight immediately rather than dropping the request.
bool relightqPush(RelightQueue* q, int cx, int cz);

// Takes one column off the set. False when it is empty. Order is unspecified on purpose:
// the relights are independent of one another and nothing may come to depend on the order.
bool relightqPop(RelightQueue* q, int* cx, int* cz);

int relightqCount(const RelightQueue* q);
int relightqCoalesced(const RelightQueue* q);
int relightqOverflows(const RelightQueue* q);
