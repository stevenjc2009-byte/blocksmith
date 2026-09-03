// The furnace: fuel + input -> output, on a timer. v1.8.15 "Furnace",
// docs/plan-1.8.15-furnace.md.
//
// ── Where the state lives ──────────────────────────────────────────────────────────────
//
// docs/plan-1.8.15-furnace.md proposed a dedicated FurnaceState table keyed by (x, y, z).
// world/blockstate.h landed first and is exactly that table, generalised to any stateful
// block instead of named for furnaces alone — so this file does NOT define a second side
// table. A furnace's live fields are the BLOCKSTATE_PAYLOAD_BYTES (16) opaque bytes
// blockstate.c already stores per position; this file owns what those bytes MEAN and
// nothing about where they are kept.
//
// FurnaceState below is therefore the design doc's struct minus x/y/z (blockstate.c is
// already keyed by position, so repeating it here would be a second copy of the same
// fact) plus one `lit` byte blockstate.h's own header comment anticipated ("a little
// slack ... e.g. a future 'currently lit' byte") and this design needs for real: a later
// rendering lane swaps BTEX_FURNACE_FRONT_LIT in for BTEX_FURNACE_FRONT based on it
// (world/registry.c's furnace row deliberately does not reference the lit tile itself,
// for exactly this reason — see that file's comment on row [42]).
//
// ── Why explicit pack/unpack and not a memcpy of the struct ────────────────────────────
//
// Same reasoning blockstate.h's own header spells out at length: this codebase's ARM EABI
// build compiles with -fshort-enums, which has already made a host sizeof() disagree with
// the console's once (world/chunk.h's Chunk). FurnaceState carries no enum (every field is
// a fixed-width integer), so the struct itself is not at obvious risk of that specific bug
// today — but blockStateGet/Set traffic in `uint8_t[BLOCKSTATE_PAYLOAD_BYTES]`, not a
// FurnaceState*, and a future field addition that also stays fixed-width would still be one
// accidental struct-padding assumption away from the same class of bug if this file ever
// memcpy'd the struct in and out. furnaceStatePack/Unpack write and read each field by name
// at an explicit byte offset, little-endian for the two uint16_t fields, the same
// put32/get32-shaped discipline world/inventory.c already uses for its own sidecar. This is
// belt-and-braces, not a response to a measured failure in THIS struct.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/blockstate.h"
#include "world/inventory.h"   // ItemId

// One furnace's live fields. NOT the wire/storage format — see furnaceStatePack/Unpack for
// that. No x/y/z (blockstate.c already keys by position) and no enum (ARM EABI safety, see
// the file header above).
typedef struct {
	ItemId   input_item;
	uint8_t  input_count;
	ItemId   fuel_item;
	uint8_t  fuel_count;
	uint16_t fuel_ticks_left;   // ticks of burn remaining on the CURRENT fuel item
	uint16_t cook_ticks;        // progress toward the current smelt, 0..recipe->cook_ticks
	ItemId   output_item;
	uint8_t  output_count;
	uint8_t  lit;               // 0/1 — true while fuel_ticks_left > 0; the rendering lane's
	                             // signal to draw BTEX_FURNACE_FRONT_LIT instead of the unlit
	                             // tile. Redundant with "fuel_ticks_left > 0" by construction
	                             // (furnaceTick keeps the two in lockstep — see furnace.c) but
	                             // stored rather than derived so a renderer that only ever
	                             // sees the packed bytes (never calls furnaceTick itself)
	                             // does not need to unpack fuel_ticks_left just to draw a face.
} FurnaceState;

// The struct's IN-MEMORY size is not the same question as the WIRE size below, and this
// assert is deliberately about the former: blockStateGet/Set never see a FurnaceState (they
// only ever see the packed bytes), but furnace.c's own locals are this struct, and nothing
// stops a future caller from being tempted to memcpy it into the 16-byte payload directly.
// Catching that at compile time, on BOTH targets independently (host gcc here, ARM EABI in
// the console build — this header carries no <3ds.h>, so both compile it), is cheaper than
// catching it by a corrupted save.
_Static_assert(sizeof(FurnaceState) <= BLOCKSTATE_PAYLOAD_BYTES,
               "FurnaceState must fit inside blockstate.c's BLOCKSTATE_PAYLOAD_BYTES opaque "
               "payload; it is never stored any other way");

// The packed WIRE/storage layout, independent of the struct's in-memory layout above:
//   [0]     input_item
//   [1]     input_count
//   [2]     fuel_item
//   [3]     fuel_count
//   [4..5]  fuel_ticks_left, little-endian u16
//   [6..7]  cook_ticks,      little-endian u16
//   [8]     output_item
//   [9]     output_count
//   [10]    lit (0 or 1)
//   [11..15] reserved, always written zero
// 11 of the 16 available bytes used — matches blockstate.h's own header arithmetic exactly
// (10 bytes for the design doc's fields, +1 for the lit byte its comment already reserved
// room for).
#define FURNACE_PAYLOAD_USED_BYTES 11
_Static_assert(FURNACE_PAYLOAD_USED_BYTES <= BLOCKSTATE_PAYLOAD_BYTES,
               "furnace's packed wire layout must fit the blockstate payload");

// Zeroes every field: no input, no fuel, no output, unlit, cook progress 0. This is what
// blockStateCreate() already hands back (an all-zero payload) unpacked, so callers that
// place a furnace by calling blockStateCreate() then immediately packing a zeroed
// FurnaceState are being explicit about a fact that was already true, not double-clearing.
void furnaceStateInit(FurnaceState* fs);

// Writes `fs` into the first FURNACE_PAYLOAD_USED_BYTES of `out` (a full
// BLOCKSTATE_PAYLOAD_BYTES buffer, as blockStateSet takes); the remaining bytes are zeroed.
void furnaceStatePack(const FurnaceState* fs, uint8_t out[BLOCKSTATE_PAYLOAD_BYTES]);

// The inverse: reads a FurnaceState back out of a BLOCKSTATE_PAYLOAD_BYTES buffer, as
// produced by furnaceStatePack (or by blockStateCreate's zero-fill, which round-trips to an
// all-zero, furnaceStateInit-equivalent FurnaceState — 0 unpacks to ITEM_NONE/0 for every
// field since ITEM_NONE is BLOCK_AIR is 0).
void furnaceStateUnpack(FurnaceState* fs, const uint8_t in[BLOCKSTATE_PAYLOAD_BYTES]);

// ── Recipes ──────────────────────────────────────────────────────────────────────────────
//
// A parallel table, not an extension of world/crafting.h's CraftRecipe — see this file's
// design doc for why (a furnace recipe needs a fuel dimension and a non-instant timer that
// CraftRecipe's shape deliberately has no notion of). Four rows today: each of
// plan-1.8.14-animals.md's raw meats to its v1.8.15 cooked counterpart. Ore->ingot is
// explicitly out of scope for this version (see the design doc) but this shape needs no
// redesign to grow it later — a future version adds rows, not a new table.
typedef struct {
	const char* name;
	ItemId      input_item;
	uint8_t     input_count;
	ItemId      output_item;
	uint8_t     output_count;
	uint16_t    cook_ticks;   // 200 (10s at TICK_HZ==20) for every row today; a field and not
	                          // a shared constant because a later ore recipe may want a
	                          // different number without widening this struct.
} FurnaceRecipe;

enum {
	FURNACE_RECIPE_PORK = 0,
	FURNACE_RECIPE_BEEF,
	FURNACE_RECIPE_CHICKEN,
	FURNACE_RECIPE_MUTTON,
	FURNACE_RECIPE_COUNT
};

extern const FurnaceRecipe FURNACE_RECIPES[FURNACE_RECIPE_COUNT];

// Finds the recipe whose input_item matches `item`. Returns NULL for anything that is not a
// valid smelting input (including ITEM_NONE) — the caller (furnaceTick) treats NULL as "no
// valid recipe for whatever sits in the input slot right now".
const FurnaceRecipe* furnaceRecipeForInput(ItemId item);

// ── Fuel ─────────────────────────────────────────────────────────────────────────────────
//
// Every existing timber block burns; nothing else does, because nothing else that could
// plausibly burn exists in this codebase yet (no coal/charcoal item — see the design doc's
// "Fuel items accepted" row). Values are docs/plan-1.8.15-furnace.md's own researched
// figures, kept exactly as that document's provenance table records them (including the
// noted-but-not-applied divergence on the log figure — this file matches the document's
// KEPT number, not its recommendation, since changing it is flagged HIS CALL there and this
// lane does not own that call).
#define FURNACE_FUEL_TICKS_PLANKS 300    // 15s: BLOCK_PLANKS, BLOCK_BIRCH_PLANKS, BLOCK_SPRUCE_PLANKS
#define FURNACE_FUEL_TICKS_LOG    1200   // 60s: BLOCK_WOOD, BLOCK_BIRCH_LOG, BLOCK_SPRUCE_LOG

// True if `item` can be burned as fuel at all. `out_ticks` (may be NULL) receives how many
// ticks a fresh unit of it burns for — one of the two constants above. Returns false (and
// leaves `out_ticks` untouched) for anything that is not a recognised fuel item, ITEM_NONE
// included.
bool furnaceIsFuel(ItemId item, uint16_t* out_ticks);

// ── Tick ─────────────────────────────────────────────────────────────────────────────────
//
// Advances one furnace's state by exactly one simulation tick (TICK_HZ, world/tick.h). The
// state machine, in the order it is actually evaluated:
//
//   1. If not currently lit (fuel_ticks_left == 0) and there is a valid recipe for the input
//      AND a unit of fuel is present AND the output slot has room for that recipe's result
//      (empty, or already holding output_item with room under INV_STACK_MAX), ignite: consume
//      one unit of fuel_count, set fuel_ticks_left from furnaceIsFuel(), set lit = 1. A
//      furnace never ignites "for nothing" — see the doc's "burns and does nothing useful"
//      case below for what happens once fuel legitimately is burning and the input later
//      stops being valid.
//   2. If lit, burn down: fuel_ticks_left -= 1; if it reaches 0, lit = 0. This happens
//      UNCONDITIONALLY once lit — a real Java furnace does not "pause" a burning fuel item
//      if the input is pulled partway through (the design doc's own research-given note),
//      and this matches it: ignition is gated on a valid input, continued burning is not.
//   3. If lit AND the input is (still) valid for a recipe AND the output has room, accumulate
//      cook_ticks by 1. If cook_ticks reaches that recipe's cook_ticks, complete the smelt:
//      consume 1 input_count (dropping to ITEM_NONE/0 if it reaches 0), add 1 to the output
//      stack (setting output_item/output_count from 0 if it was empty), and reset cook_ticks
//      to 0. If lit but the input stopped being valid (removed, or no longer matches any
//      recipe) or the output has no room, cook_ticks neither advances nor resets — it PAUSES,
//      exactly as the doc's "furnace with fuel but no valid input just burns fuel and does
//      nothing useful" case describes: the fuel is still spent (step 2 already ran this
//      tick), but no progress is made or lost while stalled. Progress resumes the instant a
//      valid input reappears, because nothing about a paused cook_ticks is discarded.
//
// Returns true if anything about `fs` changed this tick (so a caller doing dirty-tracking or
// mesh invalidation on lit-state flips knows without diffing the struct itself); the caller
// is expected to re-pack and blockStateSet() the result — this function has no knowledge of
// blockstate.c or of a position, only of one FurnaceState's fields.
bool furnaceTick(FurnaceState* fs);
