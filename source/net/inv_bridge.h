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
// One wrapper per mutating call scene/ui.c and main.c actually make — never one written
// ahead of a caller, because an unused wrapper is a function whose first caller gets to
// discover whether it works. Until v1.9.0 that rule kept inventorySplitStack out of here:
// no split gesture existed.
//
// v1.9.0 SPLIT. The gestures exist now — Y (split), X (quick-move) and the two-tap merge on
// the inventory overlay, decoded by scene/ui_gesture.h and acted on by scene/ui.c — and the
// functions they call are in the "lift" section at the end of this file. None of them is
// inventorySplitStack, and none sends BS_INV_OP_SPLIT, on purpose: that op is
// inventorySplitStack on the server too (deps/blocksmith-server/game/bsgame.c, case
// BS_INV_OP_SPLIT), which REFUSES a non-empty destination, whereas the gesture also drops a
// half onto a same-item stack and lets the cap leave a remainder. Everything that leaves a
// slot here goes out as BS_INV_OP_MOVE carrying the count this console actually realised —
// the same "report what landed" rule as everything else here, and one the two copies cannot
// read differently. BS_INV_OP_SPLIT stays on the wire, unused by this client.

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

// ── v1.9.0 SPLIT: described moves, the lift, and quick-move ─────────────────────────────
//
// Two things the gestures need that the wrappers above do not give them.
//
// THE LIFT. scene/ui.c's two-tap model lifts a slot BY INDEX (UiState.picked_slot) and moves
// the whole stack on the second tap; nothing is ever "in the air". A split cannot work that
// way — half of a stack has to live somewhere while the player chooses where it goes — so a
// split DETACHES the larger half into an InvSlot the UI carries (UiState.lift), and the slot
// keeps the smaller half. The three lift functions below are the only code that reads or
// writes that carried stack. The server is told NOTHING at split time: as far as its copy is
// concerned the whole stack is still in the slot, which is exactly right, because nothing has
// left the bag. It is told at PLACE time, as a BS_INV_OP_MOVE from the slot the lift came from
// to the slot it landed in, carrying the units that landed — and applying that MOVE to a copy
// that never split gives the same result as splitting then placing. Returning the lift to its
// origin (a cancel, a screen change) is therefore also silent. A lift can never be lost to a
// panel closing: invBridgeReturnLift() runs whenever the UI drops a lift.
//
// DESCRIBED MOVES (the multiplayer rule, mirrored from the chest: scene/ui.h's UiChestTransferFn
// sends the transfer and applies nothing, and the server's snapshot rewrites both sides).
// InvBridgeMove is one move as the wire would carry it — exactly the (op, a, b, c) of a
// BS_APP_INV_ACTION — and invBridgePlanQuickMove() describes a quick-move as a list of them
// WITHOUT touching the inventory. A caller under a verified model hands each one to
// networldSendInvAction() and applies nothing; the single-player path (invBridgeQuickMove,
// and scene/ui.c today) applies the same list locally through invBridgeApplyMove(), which is
// the ordinary optimistic apply-then-report every other wrapper in this file does. The plan
// is computed on a private copy of the inventory using the REAL inventoryMoveUnits, so the
// count in each described move is what would land, never what was asked for.

// One move as BS_APP_INV_ACTION carries it. `op` is BS_INV_OP_MOVE (proto/bs_proto.h) for
// every move this file describes today; a = src slot, b = dst slot, c = units.
typedef struct {
	uint8_t op;
	uint8_t a, b, c;
} InvBridgeMove;

// The most moves one quick-move can describe. A quick-move never lands in a slot twice — pass
// 1 tops up each same-item stack once, pass 2 fills each empty slot once — so the bound is the
// slot count of the larger strip, and a plan buffer this size can never truncate.
#define INV_BRIDGE_PLAN_MAX INV_MAIN_SLOTS

// Describes `slot`'s whole stack going to the OTHER strip — a hotbar slot's stack into the
// main grid, a main-grid slot's into the hotbar — without moving anything. The landing order
// is inventoryAdd's own, restated for one strip: every stack of the same item already in that
// strip is topped up first, lowest index first, then whatever is left fills the first empty
// slots. Anything the strip has no room for stays where it is, so a full strip describes zero
// moves and a partially full one describes exactly what fits. Writes up to `cap` moves into
// `out` and returns how many; 0 for an empty slot, a bad index, or a strip with no room.
int invBridgePlanQuickMove(const Inventory* inv, int slot, InvBridgeMove* out, int cap);

// Applies one described move locally and reports it — invBridgeMoveUnits with the struct's
// fields, so the value returned and the count on the wire are what actually moved, which for
// a move invBridgePlanQuickMove described against this same inventory is the described count.
// Returns 0 (and sends nothing) for an op this file does not describe.
uint8_t invBridgeApplyMove(Inventory* inv, const InvBridgeMove* m);

// The single-player quick-move: invBridgePlanQuickMove, then invBridgeApplyMove on every move
// it described, in order. Each piece is its own BS_INV_OP_MOVE packet, so a stack that spills
// across three slots is three packets, each carrying what that piece actually was. Returns the
// total units moved; 0 when the other strip had no room, leaving both strips untouched.
uint8_t invBridgeQuickMove(Inventory* inv, int slot);

// Detaches the larger half of `slot`'s stack into `*out_lift`: the lift gets ceil(count / 2)
// and the slot keeps floor(count / 2) — 7 becomes a lift of 4 over a slot of 3, 2 becomes 1
// over 1. The rounding is the opposite of world/inventory.h's inventorySplitStack (which keeps
// the ceiling in place) and that is deliberate: the half the player is about to carry
// somewhere is the half they are acting on, so it is the one that gets the odd unit. Refuses
// — false, nothing written, nothing changed — for a count under 2 (there is no half of one),
// an empty slot, a bad index or a NULL. Sends nothing; see the section comment.
bool invBridgeSplitStack(Inventory* inv, int slot, InvSlot* out_lift);

// Drops as much of the carried `lift` as slot `dst` will take: an empty `dst` takes it whole,
// a same-item `dst` takes what fits under INV_STACK_MAX and the remainder STAYS IN THE LIFT,
// a different item refuses outright and moves nothing. `origin` is the slot the lift was
// split from, which is what the server is told the units moved out of: a placed amount goes
// out as BS_INV_OP_MOVE(origin, dst, placed). Placing back onto `origin` itself sends nothing
// (nothing moved as far as the server's copy is concerned). Returns the units placed.
uint8_t invBridgePlaceLift(Inventory* inv, int origin, int dst, InvSlot* lift);

// Puts a carried `lift` back into the bag — onto `origin` first, where it always fits in
// single player (the slot still holds the same item and the units came out of it), and
// failing that anywhere inventoryAdd will take it. Nothing is sent either way: the server
// never learned of the detach. Returns true when the lift is empty afterwards. False means
// units the bag would not take back are still in `lift`; in single player that cannot happen,
// and on a server the next snapshot re-asserts the slot the units came from regardless.
bool invBridgeReturnLift(Inventory* inv, int origin, InvSlot* lift);

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
