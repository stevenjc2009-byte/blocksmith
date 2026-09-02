// v1.3.0. The seam between world/inventory.h (what the inventory *is*) and net/networld.h
// (what goes on the wire). This is the only file in the tree that includes both, and that is
// deliberate: networld.h's own comment states the boundary it holds — "decodes bytes", never
// "knows what an item is" — which is exactly why it declares its own NetworldInvSlot instead
// of using ItemId. Something still has to convert between the two and decide what a lost
// packet means. Putting that here keeps it out of both modules and out of scene/ui.c, and
// keeps it host-testable (inv_bridge_test.c) because neither half includes <3ds.h>.
//
// ── Why the local op happens first, every time ────────────────────────────────────────────
//
// The transport under this is unreliable UDP with no ack, no retry and no sequence numbers
// (net/bsnet_transport.h). So there are only two possible designs, and only one of them is
// playable:
//
//   * Wait for the server. The player taps a slot, and the stack moves 60-200 ms later, or
//     never if that one datagram is dropped. On a 3DS's touch panel that reads as a screen
//     that ignores taps, which is indistinguishable from a broken game.
//   * Apply locally now, tell the server, and let the server's next BS_APP_INV_STATE
//     snapshot overwrite whatever this console believes. A dropped INV_ACTION means the move
//     visibly un-happens on the next snapshot, which is at least *legible* — the stack
//     springs back — and self-corrects without anything having to detect the loss.
//
// This file is the second one. Every function below performs the ordinary world/inventory.h
// call first, unchanged, then reports what actually happened to the server; the return value
// is the local one, so a call site changes only the function's name. That also means every
// one of these is a plain no-op in single player: networldSendInvAction() sends nothing and
// returns false until an INV_STATE has arrived (it is the capability probe itself, see its
// comment), and no INV_STATE ever arrives when there is no session.
//
// ── What is reported, versus what is requested ───────────────────────────────────────────
//
// The count on the wire is always what *landed locally*, never what the caller asked for. A
// move of 40 units that only had room for 12 sends 12; an inventoryAdd that refused every
// unit sends nothing at all. The server's copy can differ from this console's (that is the
// whole reason it sends snapshots), so telling it "the player asked for 40" would invite it
// to move 40 of its own, and the two copies would diverge by more than the packet that was
// meant to reconcile them. Reporting the realised amount keeps the error bounded by one
// snapshot interval instead of compounding.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "net/networld.h"
#include "world/inventory.h"

// ── Server-reported mutations ────────────────────────────────────────────────────────────
//
// One wrapper per mutating call scene/ui.c and main.c actually make. There is deliberately
// no wrapper for inventorySplitStack: nothing in the client calls it yet (no split gesture
// exists — see scene/ui.c's two-tap file comment), and BS_INV_OP_SPLIT is on the wire ready
// for the day one does. Adding an unused wrapper now would be a function whose first caller
// gets to discover whether it works.

// inventoryMoveUnits, then BS_INV_OP_MOVE with the number of units that actually moved.
// Returns exactly what inventoryMoveUnits returned. Sends nothing when it returned 0 —
// there is no state change to report, and the server applying the same refusal would be a
// no-op on its side too.
uint8_t invBridgeMoveUnits(Inventory* inv, int src, int dst, uint8_t units);

// inventorySwapSlots, then BS_INV_OP_SWAP. Void, like the call it wraps.
void invBridgeSwapSlots(Inventory* inv, int a, int b);

// inventorySelectHotbar, then BS_INV_OP_SELECT — sent unconditionally, including when the
// selection did not change, which is the one place in this file that does not skip a no-op.
// SELECT is the only idempotent op here: applying it twice is applying it once. So if an
// earlier SELECT was dropped, re-sending the same value costs 5 bytes and repairs the
// server's copy, whereas skipping it leaves the server holding a stale selection that its
// *next snapshot will push back onto this console* — a desync that heals in the wrong
// direction. MOVE and SWAP cannot be resent this way (applying either twice is not applying
// it once), which is why they report only real changes and this does not.
void invBridgeSelectHotbar(Inventory* inv, uint8_t hotbar_slot);

// craftMake, then BS_INV_OP_CRAFT if it succeeded. Returns craftMake's own result. The
// caller keeps whatever craftCanMake() guard it already had; this wraps the mutation, not
// the test, so scene/ui.c's re-check comment still describes what that file does.
bool invBridgeCraft(Inventory* inv, int recipe_index);

// inventoryAdd, then BS_INV_OP_PICKUP with (count - leftover) — what the inventory took, not
// what the world handed over. A block broken into a full inventory is refused locally and
// reported to nobody, which matches main.c's existing comment that the block is simply left
// on the floor. `out_leftover` behaves exactly as inventoryAdd's does, NULL included.
InvAddResult invBridgeAdd(Inventory* inv, ItemId item, uint8_t count, uint8_t* out_leftover);

// inventoryRemove, then BS_INV_OP_CONSUME with the number actually removed. Returns that
// same number.
uint8_t invBridgeRemove(Inventory* inv, ItemId item, uint8_t count);

// ── Applying the server's snapshot ───────────────────────────────────────────────────────

// Overwrites `inv` from a decoded BS_APP_INV_STATE. Register this (via a small hook in
// main.c) with networldSetInvHook() and the server becomes authoritative: anything the local
// optimistic path above got wrong is corrected the next time a snapshot lands.
//
// Returns false and leaves `inv` COMPLETELY untouched if any slot names an item this build
// does not have. That test is inventoryCanHold(), which since v1.8.8 asks the registry ("is
// there a row for this id") instead of comparing against BLOCK_COUNT — a more direct answer to
// the question this paragraph was always asking, and a strictly MORE permissive one, so no
// snapshot that used to be accepted is rejected now. networld.c cannot make that check — it must not
// include world/block.h, by the same boundary rule described at the top of this file — so it
// validates only what bs_proto.h promises about the wire (the count-iff-item pairing, the
// stack cap) and this is where the id space is checked. All-or-nothing rather than
// per-slot-skip for the same reason applyInvState() drops a whole malformed packet: half an
// inventory is a state no server ever sent.
//
// The consequence of that false is worth being explicit about, because it is not
// self-healing. An unknown item id means the server is running a build with blocks this
// client does not know (world/block.h ids are append-only, so ids only ever grow), and it
// will keep sending them. So this does not recover on the next snapshot the way a dropped
// packet does — the inventory simply stops tracking the server's, silently, until the
// console is updated. That is still the right failure: the alternative is indexing
// blockInfo() past the end of its table to draw an icon for a block that does not exist in
// this build, which is a crash, not a desync.
//
// selected_hotbar is clamped rather than rejected, because inventorySelectHotbar() already
// clamps an out-of-range index by contract (see its comment) — an out-of-range selection can
// only ever point at a hotbar slot, never outside the array, so it cannot be the unsafe
// thing an unknown item id is.
bool invBridgeApplyState(Inventory* inv, const NetworldInvState* state);
