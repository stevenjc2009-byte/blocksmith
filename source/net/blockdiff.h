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
// this store's footprint is a separate static on top of it, not carved out of it.
//
// This is deliberately matched to the SERVER's own capacity: BS_DIFF_MAX in
// deps/blocksmith-server/game/diffstore.h is 65536, and the server replays every diff it
// holds to a joining client in one burst (send_world_sync(), game/bsgame.c). Any cap below
// the server's therefore loses edits at join time, and loses the NEWEST ones, because the
// server sends oldest-first and this store refuses rather than evicts once full. Measured
// with a 300-edit join sync against the old cap of 256: "queued 256 of 300, refused 44" —
// the 44 lost were the most recent, i.e. whatever had just been built. Matching the server
// exactly means a join sync can never overflow this store at all.
//
// Cost: 65536 * sizeof(BlockDiffEntry) = 1048576 bytes (1 MB) of static RAM, plus the
// BLOCKDIFF_BUCKETS index below. That size is only affordable because record() and drain()
// no longer scan the whole array — see the chaining fields on BlockDiffEntry.
#define BLOCKDIFF_MAX_PENDING   65536

// Number of per-column chain heads. A power of two so the hash can mask rather than divide,
// and sized so that a store filled with diffs spread across many columns still gives short
// chains (65536 entries over 4096 buckets is 16 deep on average). 4096 * 4 bytes = 16 KB.
#define BLOCKDIFF_BUCKETS       4096

// End-of-chain / no-slot marker. BLOCKDIFF_MAX_PENDING is itself a valid non-index, and a
// uint32_t holds it comfortably alongside every real index 0 .. BLOCKDIFF_MAX_PENDING-1.
#define BLOCKDIFF_NIL           0xFFFFFFFFu

// Exactly 16 bytes: 4 + 4 + 1 + 1 + 2 pad + 4. y is a byte rather than an int32_t because
// blockdiffRecord() rejects anything outside 0 .. WORLD_HEIGHT-1 (128) before storing, so
// the wider field could never hold a value a byte could not. Shrinking it is what pays for
// the `next` link without growing the entry.
typedef struct {
	int32_t  x, z;    // world block coordinate this diff targets (full range)
	uint8_t  y;       // 0 .. WORLD_HEIGHT-1, guaranteed by blockdiffRecord()
	BlockId  id;
	uint16_t _pad;
	uint32_t next;    // next entry in the same bucket chain, or BLOCKDIFF_NIL. A free slot
	                  // uses this same field to chain into the free list.
} BlockDiffEntry;

typedef struct {
	BlockDiffEntry entry[BLOCKDIFF_MAX_PENDING];

	// Chain head per column-hash bucket, or BLOCKDIFF_NIL when that bucket is empty. This is
	// what makes both record() and drain() proportional to one column's pending diffs rather
	// than to BLOCKDIFF_MAX_PENDING: they only ever walk the one chain their coordinate
	// hashes to. With the old full-array scan, a cap of 65536 would have meant roughly 4.3
	// billion comparisons to absorb a full join sync — a hardware freeze, not a slow frame.
	uint32_t bucket[BLOCKDIFF_BUCKETS];

	uint32_t free_head;    // chain of slots returned by drain(), or BLOCKDIFF_NIL
	uint32_t high_water;   // slots [0, high_water) have been handed out at least once; below
	                       // that, allocation is a bump rather than a search. Lets clear()
	                       // reset the store without writing to the 1 MB entry array.
	int      count;        // live entries, across every column
	int      refusals;     // record() calls turned away because the store was full
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
