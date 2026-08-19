// Pending block-diff store — remote edits waiting for their column to load.
//
// worldSet() (world/world.c:175) refuses to write a non-air block into a column that is
// not currently loaded: there is no chunk there to hold it, and allocating one purely to
// remember a diff would undo the whole point of streaming terrain. worldColumnRemove()
// then frees whatever chunks a column holds the moment it leaves the streaming ring.
// Between those two behaviours, a remote player's edit to a column the *local* player has
// not loaded (or has since unloaded) is simply lost — multiplayer sync goes silently wrong.
//
// This store is the fix: hold the diff here instead of dropping it, keyed implicitly by
// the column its coordinate falls in, and hand it back to the caller with blockdiffDrain()
// the moment that column is loaded. It knows nothing about sockets, packets, or the World
// struct's internals — it takes a coordinate and a block id in, and gives them back out
// through a caller-supplied callback, so whoever owns the World (menu/scene code, not this
// module) decides how and when to call worldSet() with them.
//
// Bounded and array-backed on purpose, with no heap use at all. Remote input drives this
// store — a buggy or hostile peer must not be able to queue diffs forever and eat into the
// 12 MB world budget (world/budget.h) with nothing on screen to show for it. See
// BLOCKDIFF_MAX_PENDING and blockdiffRecord() below for the fixed cap and what happens when
// it is hit.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/world.h"   // BlockId and WORLD_HEIGHT — the only things this module needs
                            // from world/, and both are plain C with no 3DS dependency.

// A slot count, not a byte budget — world/budget.h already owns the real 12 MB figure, and
// this store's total footprint (BLOCKDIFF_MAX_PENDING * sizeof(BlockDiffEntry), a few KB) is
// negligible next to it. 256 is chosen generously above how many remote edits could
// plausibly still be pending for columns the local player hasn't loaded yet at any one
// moment — a streaming column load takes a handful of frames, not minutes — while staying
// small enough that a full linear scan (what blockdiffRecord() and blockdiffDrain() both do)
// stays cheap on Old 3DS hardware.
#define BLOCKDIFF_MAX_PENDING   256

typedef struct {
	int32_t x, y, z;   // world block coordinate this diff targets
	BlockId id;
	bool    used;       // false marks a free slot; not exposed outside blockdiff.c
} BlockDiffEntry;

typedef struct {
	BlockDiffEntry entry[BLOCKDIFF_MAX_PENDING];
	int            count;      // live entries, across every column
	int            refusals;   // record() calls turned away because the store was full
} BlockDiffStore;

void blockdiffInit(BlockDiffStore* s);

// No heap resources today, so this only zeroes the store. Kept as its own call — matching
// worldInit()/worldExit()'s pairing — so a future change to how entries are backed does not
// have to go find every call site.
void blockdiffFree(BlockDiffStore* s);

// Records a remote edit at world block (x, y, z). If a pending diff already targets that
// exact coordinate it is overwritten in place — latest write wins, so a place-then-break
// from the same peer cannot resurrect the earlier block by leaving both diffs queued.
//
// Returns false and stores nothing when:
//   - y is outside 0 .. WORLD_HEIGHT-1 (mirrors worldSet()'s own bound check), or
//   - this is a new coordinate and the store is already at BLOCKDIFF_MAX_PENDING.
//
// The second case is a refusal, not an eviction: the store never drops an already-recorded
// diff to make room for a new one. See blockdiff.c for why. blockdiffRefusals() counts it.
bool blockdiffRecord(BlockDiffStore* s, int x, int y, int z, BlockId id);

// Invoked once per pending diff that targets column (cx, cz) during blockdiffDrain(), with
// `userdata` passed through unchanged. This is a fan-out to the caller's own world, not a
// transaction — there is no way to signal "stop" or "put this one back".
typedef void (*BlockDiffApplyFn)(void* userdata, int x, int y, int z, BlockId id);

// Hands every pending diff for column (cx, cz) to `apply`, one call each, then removes them
// from the store. Safe to call when nothing is pending for that column — apply is simply
// never invoked. Returns the number of diffs drained.
int blockdiffDrain(BlockDiffStore* s, int cx, int cz, BlockDiffApplyFn apply, void* userdata);

// Diffs currently held, across every column. For diagnostics and tests.
int blockdiffCount(const BlockDiffStore* s);

// record() calls refused because the store was full, since init() or the last clear().
// For diagnostics.
int blockdiffRefusals(const BlockDiffStore* s);

// Drops every pending diff without applying them, and resets the refusal count. For tests,
// and for a hard reset such as leaving a multiplayer session.
void blockdiffClear(BlockDiffStore* s);
