// Coalescing worklist of columns awaiting a full relight (v1.8.0).
//
// lightRelightColumn recomputes a WHOLE column — 32768 cells, several relaxation passes,
// 720.1 us on the host at -O1 and milliseconds on a 268 MHz ARM11. main.c's onRemoteEdit
// used to call it once per remote edit, inline, which is fine for one player breaking one
// block and pathological for the case that actually produces edits in bulk: rejoining.
// networldOnColumnLoad (net/networld.c) drains every diff the server has ever stored for a
// column the instant that column installs, and every one of those diffs fires the edit
// hook — so N diffs on one column paid for N identical full relights of that same column
// where one would have done, on the frame a world is already at its busiest.
//
// The relight is per-column, idempotent and order-independent (it memsets and recomputes
// from the blocks), so the fix is just to remember WHICH columns need one and do each once.
// That is all this is: a small set with push-time dedup, drained by the main loop.
//
// Bounded by construction rather than by hope: only a column that is already loaded can be
// pushed (net/networld.c's applyOrQueue refuses to apply an edit to a column that is not
// there), and the render distance caps loaded columns at RENDER_DIST_MAX_COLUMNS = 49. The
// capacity below is above that with room to spare. A push that overflows anyway returns
// false and the caller relights inline exactly as it did before — slow, never wrong.
//
// No <3ds.h>: host-testable like the rest of source/world, and the same shape as the
// worklists already here (dirtyq.h, jobq.h, meshq.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RELIGHTQ_CAP 64

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
