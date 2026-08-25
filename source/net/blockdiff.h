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
// This DELIBERATELY NO LONGER MATCHES the server's own capacity, and the gap is the design,
// not a drift to be closed. BS_DIFF_MAX in deps/blocksmith-server/game/diffstore.h is 131072
// — exactly twice this. It was raised from 65536 in the server's b95f980 ("feat(proto):
// per-column diff subscription, and a block ceiling past 100000", 2026-08-21), whose message
// states the reason in as many words: the client's inbox having to be as large as the
// server's whole store "is what actually capped BS_DIFF_MAX - not memory here, but memory on
// a 3DS", and with per-column delivery "the console holds edits for loaded columns only and
// the two ceilings come apart". Growing this constant to chase that one would re-couple the
// two ceilings the server paid a protocol change to separate.
//
// An earlier version of this comment claimed the server was 65536 and that "matching the
// server exactly means a join sync can never overflow this store at all". Both halves were
// false from b95f980 onward. Nothing detected that for the same reason nothing detects any
// prose: it was a comment. BLOCKDIFF_SERVER_DIFF_MAX below and `make check-diffcap-drift`
// are what now make the claim checkable instead of merely written down.
//
// What the gap actually costs, MEASURED (blockdiff_test.c, test_server_replay_overflow):
// replaying BS_DIFF_MAX diffs into an empty store — one per column, so nothing collapses via
// latest-write-wins — gives "replayed 131072, accepted 65536, refused 65536". The server
// replays oldest-first, so the 65536 refused are the NEWEST: whatever had just been built.
// Read back by coordinate, the block at x=2097136 (column 131071) drains 0 entries and its
// apply callback never fires. That is the v1.2.x "my house is gone" bug's exact shape, at a
// 256x higher threshold than the 256 slots it first bit at.
//
// It is nonetheless RECOVERABLE, which is the other half of why the cap stays here. Every
// column this client actually loads is subscribed the instant it goes live — main.c's
// genInstallOne() calls networldSubscribeColumn() immediately after networldOnColumnLoad() —
// and the server's handle_chunk_sub() answers every subscription with send_chunk_diffs() for
// that column unconditionally: it does not consult legacy_sync_sent, so a full dump having
// already happened does not suppress it. A diff refused here therefore comes back the moment
// the player is near enough to see the block it targets. Raising this to 131072 would spend
// a second megabyte of static RAM to buy back only edits for columns the player is not in.
//
// What is NOT recovered is the refusal itself becoming visible to anyone: blockdiffRefusals()
// counts every one, networld.c re-exports it as networldPendingRefusals(), and as of this
// writing NOTHING outside the test suites reads either. The store is loud; the client is deaf.
// See this header's note on blockdiffRefusals() below.
//
// Cost: 65536 * sizeof(BlockDiffEntry) = 1048576 bytes (1 MB) of static RAM, plus the
// BLOCKDIFF_BUCKETS index below; the whole BlockDiffStore measures 1064976 bytes (1.02 MB).
// That size is only affordable because record() and drain() no longer scan the whole array —
// see the chaining fields on BlockDiffEntry.
#define BLOCKDIFF_MAX_PENDING   65536

// The server's BS_DIFF_MAX (deps/blocksmith-server/game/diffstore.h) as this client's design
// was reasoned against it. This is NOT a capacity: nothing here is sized from it, and it must
// never be used to size anything. It exists so that the assumption is CHECKABLE — `make
// check-diffcap-drift` reads the real value out of that header on every build and stops if it
// has moved. That guard is exactly what the pair went without when the server doubled from
// 65536 to 131072 and the comment above went on claiming the two numbers matched.
//
// Not a _Static_assert, because one cannot see both values: diffstore.h is a server GAME
// header, not the shared wire contract in proto/bs_proto.h, and it is on no client build's
// include path. Measured, rather than assumed — `gcc -E -dM -I source -I deps/blocksmith-server`
// over a TU including this header reports BS_DIFF_MAX undefined (0 matching lines). bs_proto.h
// does mention BS_DIFF_MAX, but only in prose explaining why CHUNK_SUB exists; it defines
// nothing. So the check has to read the header's text, which is what the Makefile target does.
#define BLOCKDIFF_SERVER_DIFF_MAX 131072u

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
//
// This is the ONLY evidence that edits were dropped, and it is currently evidence nobody
// looks at. networld.c re-exports it as networldPendingRefusals() (networld.h), and the only
// callers of that are networld_test.c's own checks: main.c's debug overlay prints the pending
// COUNT as "q%d" but not this, so a client that has refused 65536 of a join replay looks,
// on screen, exactly like one that refused none. That is the same "silently identical to
// working" failure mode the overlay's registry-CRC "!" marker (main.c, networldRegistrySynced())
// was added to break, and it wants the same treatment: a marker when this is non-zero, so the
// player can rejoin rather than wonder where their house went. Not done here because main.c is
// outside this module — see this task's report for the exact one-line change proposed.
int blockdiffRefusals(const BlockDiffStore* s);

// Drops every pending diff without applying them, and resets the refusal count. For tests,
// and for a hard reset such as leaving a multiplayer session.
void blockdiffClear(BlockDiffStore* s);
