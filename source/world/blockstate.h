// Per-instance block state: a fixed-size side table that lets a handful of block
// *positions* carry private data beyond their id -- a furnace's fuel/input/output and its
// cook progress, and later (unbuilt, unplanned beyond the shape below) a chest's contents
// or a sign's text.
//
// ── The gap this closes ────────────────────────────────────────────────────────────────
//
// Every block in this engine today is *just an id*. world/chunk.h's Chunk stores one
// BlockId per cell and nothing else -- UNIFORM is a single id for the whole chunk,
// PALETTE4 and RAW both bottom out in "one byte per cell". Two furnaces standing next to
// each other are, as far as world/world.c and world/chunk.c are concerned, the same fact
// repeated twice: "there is a furnace block here". Nothing before this file lets one of
// them be half-cooked and the other empty.
//
// ── Why a side table, not a field added to every cell ─────────────────────────────────
//
// A per-cell field costs bytes for every block in every loaded chunk, whether or not that
// block ever needed the field -- exactly the cost world/chunk.h's whole three-form design
// (UNIFORM/PALETTE4/RAW) exists to avoid paying for the common case (a lot of identical
// stone and air). Old 3DS is the binding constraint (world/budget.h caps the world store at
// 12 MB total, radius 5 already spends 89.2% of it), so a design that scales with the
// number of *loaded blocks* is wrong before it is even measured: a furnace is one block in
// tens of thousands. What should scale is the number of blocks that actually carry state,
// which this project's own numbers say will be small for a long time (docs/plan-1.8.15
// -furnace.md estimates well under 64 simultaneously-interesting furnaces per world).
//
// So this is a fixed-size table, entirely separate from Column/Chunk, keyed by absolute
// block position. It costs BLOCKSTATE_SLOTS * sizeof(BlockStateEntry) bytes, period --
// see the arithmetic on BLOCKSTATE_SLOTS below -- whether the table holds zero records or
// is full, and that cost does not move when render distance, chunk count or world size do.
//
// ── Why not source/entity/entity.h's EntityWorld ──────────────────────────────────────
//
// Entity was built for things that move, collide and think (its own file header: "a fixed
// pool of things that are not the player and not blocks", stepped through the same
// bodyMove() collision the player uses). A furnace never moves and has no collision box of
// its own -- every field Entity carries (Body, yaw, ai_state, despawn flags) would be dead
// weight, and ENTITY_CAP_OLD/NEW (24/48, entity.h) is a budget already reasoned about for
// animals and monsters; borrowing from it would make furnace count and mob count compete
// for the same ceiling for no reason. A second, purpose-built table costs one more small
// fixed allocation and keeps the two domains from fighting over one cap.
//
// ── Why the payload is opaque bytes, not a typed struct ────────────────────────────────
//
// A typed payload struct (say, the FurnaceState this project's own furnace design doc
// sketches: input/fuel/output item+count pairs, two uint16 timers) would need its exact
// byte layout to agree between whatever writes it and whatever reads it back -- and this
// codebase has already been bitten twice by assuming a host-compiled struct's layout says
// anything about the ARM EABI build (world/block.h and this project's own session notes:
// the ARM EABI's -fshort-enums makes a host sizeof() lie about a console struct). This
// module never interprets the payload at all: it is BLOCKSTATE_PAYLOAD_BYTES of opaque
// uint8_t, copied in by blockStateSet and back out by blockStateGet. The *owning* module
// (world/furnace.c, when it exists) packs and unpacks its own fields with explicit
// little-endian helpers, the same put32/get32-shaped code world/inventory.c and
// world/daynight.c already use for their own sidecars -- so nothing about this table's
// correctness depends on any other translation unit's struct layout, on either target.
//
// ── Lifetime: independent of chunk residency ───────────────────────────────────────────
//
// A record is keyed by absolute (x, y, z), never by which Column or Chunk currently covers
// it, and it is never stored inside a Column or Chunk. That is what makes chunk eviction a
// non-event for this table: world/world.c's worldColumnRemove frees a column's chunks and
// its light data, and has nothing of this table's to free, copy out, or reattach -- a
// furnace's contents simply keep existing in this table whether or not the chunk the
// furnace physically sits in is currently loaded. The table's own save/load (below) is the
// entirety of its lifecycle; nothing about a chunk loading or unloading touches it.
//
// ── Block destruction: what this file guarantees, and what it does not ────────────────
//
// The primary defence is explicit: whatever handles a block break is expected to call
// blockStateRemove(t, x, y, z) for a block kind that has state, same as any other
// tear-down. This file cannot make that call itself -- it has no hook into a break path,
// by design (see the module comment below) -- so a break handler that forgets to call it
// leaves an ORPHANED record: real bytes, at a real position, that nothing will read again
// until another stateful block is placed at the exact same position. That is a wasted slot,
// not a wrong answer -- see blockStateCreate's contract for why. It is the same shape of
// harmless waste world/chunk.h's own header describes for an orphaned PALETTE4 palette
// slot: "not a rare case worth a scan, never a wrong answer".
//
// The second defence is structural and does not depend on the caller remembering anything:
//   1. blockStateCreate ALWAYS hands back a freshly zeroed payload, even when a record
//      already existed at that position (the orphaned-record case above) -- so a new
//      furnace placed where an old, un-removed one used to stand never inherits its
//      leftover fuel or cook progress. Creation is where staleness gets cleaned up, because
//      it is the one call every stateful block placement is guaranteed to make.
//   2. blockStateGet/blockStateSet both take the caller's own idea of which block owns this
//      position (`expect_block_id`) and refuse (false, payload untouched) if it does not
//      match what is on record. This catches the case where two DIFFERENT block kinds
//      happen to land on the same position across a leaked remove -- a chest's code asking
//      for state at a position that (unbeknownst to it) still holds an orphaned furnace
//      record reads back "no state" rather than a furnace's bytes reinterpreted as a
//      chest's.
//
// What it does NOT catch: the same block kind placed twice at the same position across a
// leaked remove (break a furnace without removing its record, place a second furnace at
// the exact same spot). blockStateGet's id check cannot see that -- both records carry the
// same block_id -- but blockStateCreate's unconditional reset (defence 1) already zeroed
// the payload the moment the second furnace was placed, so there is nothing stale left to
// read by the time this matters. The two defences are deliberately layered to cover each
// other's blind spot.
//
// ── Not thread-safe, and does not need to be ───────────────────────────────────────────
//
// Same reasoning as world/region.c's load-side cache: every call in this file is expected
// to come from the main thread (block placement/removal, panel UI, a tick function walking
// active records) — nothing about furnace/chest interaction runs on app/worker.c's
// generation thread, which only ever touches ordinary column terrain.
//
// ── Not counted against world/budget.h's WORLD_BUDGET_BYTES ───────────────────────────
//
// Same reasoning entity.h's ENTITY_SLOTS gives for EntityWorld: WORLD_BUDGET_BYTES is
// specifically the per-loaded-column terrain/light cost, and this table's size does not
// scale with how much of the world is loaded -- it is a fixed BSS/static cost paid once,
// like EntityWorld, not a claim against the column budget.
//
// ── NOT file-static ─────────────────────────────────────────────────────────────────────
//
// Mirrors entity.h's own rule exactly, for the same reason: "the store is a struct passed
// by pointer, so two of them can exist, a test can own one, and nothing here is state a
// worker thread has to reason about." A caller owns a BlockStateTable (BSS, a field on
// whatever owns the live World, wherever the wiring lane decides -- this file does not
// decide that) and passes it to every call below.
//
// ── What this file deliberately does NOT do ────────────────────────────────────────────
//
// No block-break hook, no block-placement hook, no furnace behaviour, no recipe, no tick
// function, no UI. Those all need to know things this table does not (which block ids are
// "stateful", what a furnace's bytes mean, when a break happens) and belong to whichever
// lane builds the furnace on top of this. This file is the key/value store and nothing
// else -- see the task brief this was built against.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/block.h"

// 64: matches docs/plan-1.8.15-furnace.md's own "concurrent furnace cap" reasoning ("just
// enough for one base," not measured against anything) -- reused rather than re-derived,
// since this table exists to serve exactly the case that number was chosen for. Cheap to
// raise later: the save format below already stores a live COUNT and only that many
// records, so a bigger cap reads an old, smaller save file with no format change (see
// blockStateLoad).
#define BLOCKSTATE_SLOTS  64

// How many opaque bytes each record carries. Sized off the furnace design this table exists
// to unblock: docs/plan-1.8.15-furnace.md's own FurnaceState needs input_item(1) +
// input_count(1) + fuel_item(1) + fuel_count(1) + fuel_ticks_left(2) + cook_ticks(2) +
// output_item(1) + output_count(1) = 10 bytes. 16 rounds that up to a 4-aligned figure with
// a little slack (e.g. a future "currently lit" byte) rather than leaving a format change on
// the table for the first small field furnace.c turns out to want -- not a speculative
// design for a chest or sign, which are not sized here at all and may need more than this
// (a future kind that needs more than 16 bytes is a reason to widen this constant when it is
// actually built, not a reason to over-provision today for a shape nobody has designed yet).
#define BLOCKSTATE_PAYLOAD_BYTES  16

// One record. block_id doubles as the "is this slot in use" flag: BLOCK_AIR (0) can never
// legitimately own a state record (air is never targetable, never placed, never a furnace),
// so a slot reading BLOCK_AIR is free by construction and needs no separate bool to fall out
// of sync -- the same trick source/entity/entity.h's `kind == ENT_NONE` plays for EntityWorld.
//
// Fixed-width integer types throughout and no enum anywhere in this struct, on purpose: this
// project's ARM EABI build compiles with -fshort-enums, which has already made a host
// sizeof() disagree with the console's once (world/chunk.h's Chunk, 8 bytes on ARM against
// 12 on a host x86-64 build). BlockId is already a uint8_t typedef (world/block.h), not an
// enum, so this struct carries nothing that can silently change size or signedness crossing
// that boundary -- verified, not just argued, by blockstate_test.c's _Static_asserts, which
// this header cannot itself run on both targets (see that file for why).
typedef struct {
	int32_t x, y, z;
	BlockId block_id;
	uint8_t data[BLOCKSTATE_PAYLOAD_BYTES];
} BlockStateEntry;

typedef struct {
	BlockStateEntry slots[BLOCKSTATE_SLOTS];
} BlockStateTable;

// Zeroes every slot. Every field of a zeroed BlockStateEntry reads block_id == BLOCK_AIR, so
// this is also what an all-slots-free table looks like -- the same "zero is empty" property
// world/inventory.c's inventoryInit relies on for Inventory.
void blockStateInit(BlockStateTable* t);

// Claims a slot for (x, y, z) owned by `block_id` and hands back a payload of all zero
// bytes. `block_id` must not be BLOCK_AIR (air owns no state; see the free-slot trick above)
// -- passing it is a caller bug and returns false without touching the table.
//
// If a record ALREADY exists at (x, y, z) -- including one left behind by a break that
// never called blockStateRemove, see this file's header -- it is reset in place: its
// block_id is overwritten to the one just given and its payload zeroed, exactly as if the
// old record had been removed and a fresh one created. This is deliberate, not a missed
// "already exists" check: it is the structural half of this file's answer to "state must
// not resurrect when a different block is placed here" (see the header), and it means a
// placement can never be silently refused by a leak it did not cause.
//
// False only when the table has no free slot AND no existing record to reset at this exact
// position -- i.e. BLOCKSTATE_SLOTS live, unrelated records already fill the table. The
// caller must handle this (refuse the placement, or whatever policy it wants), not assume
// it cannot happen; nothing here raises the cap on its own.
bool blockStateCreate(BlockStateTable* t, int x, int y, int z, BlockId block_id);

// Frees the record at (x, y, z), if any -- the primary defence against a stale record
// outliving the block it belonged to; see this file's header for what happens if a caller
// forgets this. Not an error to call with nothing there: a break on a block with no state,
// or a double-remove, is a silent no-op, same as free(NULL).
void blockStateRemove(BlockStateTable* t, int x, int y, int z);

// True and fills `out` with the BLOCKSTATE_PAYLOAD_BYTES stored at (x, y, z) iff a record
// exists there AND its stored block_id equals `expect_block_id`. False (out left untouched)
// for no record, or for a record whose block_id does not match -- see this file's header for
// why the id check exists and what it does and does not catch.
bool blockStateGet(const BlockStateTable* t, int x, int y, int z, BlockId expect_block_id,
                    uint8_t out[BLOCKSTATE_PAYLOAD_BYTES]);

// The write side of blockStateGet: overwrites the payload of the existing record at
// (x, y, z) whose block_id equals `expect_block_id`. False (table untouched) under the same
// two conditions blockStateGet refuses under -- no record there, or a block_id mismatch.
// Never creates a record; see blockStateCreate for that.
bool blockStateSet(BlockStateTable* t, int x, int y, int z, BlockId expect_block_id,
                    const uint8_t data[BLOCKSTATE_PAYLOAD_BYTES]);

// How many of BLOCKSTATE_SLOTS are currently live. For a bottom-screen report and for tests;
// nothing in this file uses it to make a decision.
int blockStateCount(const BlockStateTable* t);

// ── Save / load ─────────────────────────────────────────────────────────────────────────
//
// A dedicated sidecar, "<world_dir>/blockstate.dat", following the exact tmp-write /
// fclose-flush / remove-old / rename-into-place shape world/inventory.c's
// inventorySave/inventoryLoad already use (itself following world/region.c and
// app/options.c) -- so a power cut here degrades the same way an interrupted inventory save
// does: the old file is untouched until the replacement is fully written and closed, and a
// leftover ".tmp" from a cut between the remove and the rename is promoted back into place
// the next time anything loads.
//
// Deliberately NOT folded into world/region.c's per-column format or world/chunk_codec.c's
// per-chunk one: a stateful block's contents are not keyed by which 16-column region file
// its position happens to fall in, and a region file's REGION_VERSION exists to describe
// chunk/column bytes specifically -- bolting an unrelated table onto it would mean every
// region file pays this table's format changes and vice versa, for two things that change
// for unrelated reasons. This mirrors exactly what docs/plan-1.8.15-furnace.md's own design
// pass already concluded for furnace.dat, generalised to any block-state user rather than
// named for furnaces alone.
//
// A missing/corrupt/truncated/wrong-version file all degrade to zero records, same as
// world/inventory.c treats a bad inventory.dat -- there is no player-visible difference
// between "never saved" and "saved, but unreadable" worth reporting separately, and the
// safe answer either way is an empty table, never a crash and never bytes the rest of the
// game has not validated.

// Loads "<world_dir>/blockstate.dat" into `t`. Always leaves `t` fully valid (see
// blockStateInit) and always returns true, except when `t` is NULL or `world_dir` is
// NULL/empty, which is a caller bug rather than a file-format problem.
bool blockStateLoad(BlockStateTable* t, const char* world_dir);

// Saves every live record in `t` to "<world_dir>/blockstate.dat". False on any IO failure
// (could not open the tmp file, a short write, the final rename failing) or a NULL/empty
// `world_dir`, in which case the previous save (if any) is left exactly as it was.
bool blockStateSave(const BlockStateTable* t, const char* world_dir);
