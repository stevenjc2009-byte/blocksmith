#include "net/inv_bridge.h"

#include "world/block.h"
#include "world/crafting.h"

#include "proto/bs_proto.h"

// invBridgeApplyState() below walks a NetworldInvState's slots straight into an Inventory's,
// so the two array lengths being equal is a correctness requirement of this file, not a
// coincidence of two headers picking the same number. networld.c already asserts its own
// constant against BS_INV_SLOT_COUNT (the wire); this asserts it against INV_SLOT_COUNT (the
// game's own layout), which is the half that assert cannot see — it must not include this
// header. Together the two pin all three to the same value at compile time.
_Static_assert(NETWORLD_INV_SLOT_COUNT == INV_SLOT_COUNT,
               "wire inventory slot count must match world/inventory.h's INV_SLOT_COUNT");

// The wire's op parameters are three bytes (bs_proto.h: "every parameter any of these
// operations takes is a slot index, a hotbar index, a unit count or a recipe index"), while
// world/inventory.h takes slot indices as plain int. Nothing in this client passes an
// out-of-range slot — scene/ui.c's hit-testing (scene/ui_layout.c) only ever produces indices
// inside the grid it laid out — but the cast from int to uint8_t is silent when it is wrong,
// and a slot index that wrapped would be a *valid-looking* op aimed at the wrong slot on the
// server rather than an obvious failure. So the range is checked here, once, at the point
// where the type actually narrows.
//
// A false only suppresses the send; the local call has already happened and stands. That is
// the honest split: if a caller ever does pass a bad index, the bug belongs to that caller and
// to world/inventory.h's own bounds handling, and this file's job is to not turn it into a
// corrupt packet as well.
static bool slotFitsWire(int slot)
{
	return slot >= 0 && slot < INV_SLOT_COUNT;
}

uint8_t invBridgeMoveUnits(Inventory* inv, int src, int dst, uint8_t units)
{
	const uint8_t moved = inventoryMoveUnits(inv, src, dst, units);

	if (moved > 0 && slotFitsWire(src) && slotFitsWire(dst))
		(void)networldSendInvAction(BS_INV_OP_MOVE, (uint8_t)src, (uint8_t)dst, moved);

	return moved;
}

void invBridgeSwapSlots(Inventory* inv, int a, int b)
{
	inventorySwapSlots(inv, a, b);

	// No "did anything change" test, unlike the move above: a swap of two slots holding the
	// same thing is indistinguishable from a swap of two slots holding different things
	// without comparing both before and after, and the only caller (scene/ui.c's
	// handleSlotTap) reaches this branch precisely when the two items differ. Sending it
	// unconditionally costs one packet on a gesture the player made by hand.
	if (slotFitsWire(a) && slotFitsWire(b))
		(void)networldSendInvAction(BS_INV_OP_SWAP, (uint8_t)a, (uint8_t)b, 0);
}

void invBridgeSelectHotbar(Inventory* inv, uint8_t hotbar_slot)
{
	inventorySelectHotbar(inv, hotbar_slot);

	// The value that goes out is what the inventory ended up with, not what the caller asked
	// for. inventorySelectHotbar clamps an out-of-range index to the last hotbar slot (see its
	// comment), so reading it back is what keeps the two copies agreeing on the clamp instead
	// of the server clamping the raw value again and possibly differently.
	//
	// See the header for why this one is sent even when nothing changed.
	(void)networldSendInvAction(BS_INV_OP_SELECT, inv->selected_hotbar, 0, 0);
}

bool invBridgeCraft(Inventory* inv, int recipe_index)
{
	if (!craftMake(inv, recipe_index)) return false;

	// craftMake already refused every out-of-range index before reaching here, so the recipe
	// index is known good by the time it is narrowed — unlike the slot indices above, which
	// world/inventory.h's move/swap do not report a refusal for.
	(void)networldSendInvAction(BS_INV_OP_CRAFT, (uint8_t)recipe_index, 0, 0);
	return true;
}

InvAddResult invBridgeAdd(Inventory* inv, ItemId item, uint8_t count, uint8_t* out_leftover)
{
	uint8_t leftover = 0;
	const InvAddResult r = inventoryAdd(inv, item, count, &leftover);
	if (out_leftover) *out_leftover = leftover;

	// count is what was offered, leftover what would not fit; the difference is what the
	// inventory is now actually holding, and that is the only number the server should be told
	// about. INV_ADD_REFUSED means leftover == count, so `landed` is 0 and nothing goes out.
	//
	// inventoryItemOnWire, not inventoryCanHold (v1.8.8). The two were the same predicate until
	// v1.8.8 widened the BAG to the whole registry; the WIRE item span is still BS_BLOCK_COUNT
	// == 8, in deps/blocksmith-server, a repo this client does not own. The bag really is
	// holding the cactus — this only decides whether the server is told, and an old server's
	// `if (a < BS_BLOCK_COUNT && ...)` has no else, so telling it would be a datagram it
	// discards. See world/inventory.h's inventoryItemOnWire() for what the gap costs a player on
	// a server and for the one change across both repos that closes it.
	const uint8_t landed = (uint8_t)(count - leftover);
	if (landed > 0 && inventoryItemOnWire(item))
		(void)networldSendInvAction(BS_INV_OP_PICKUP, (uint8_t)item, landed, 0);

	return r;
}

uint8_t invBridgeRemove(Inventory* inv, ItemId item, uint8_t count)
{
	const uint8_t removed = inventoryRemove(inv, item, count);

	// inventoryItemOnWire for the same reason as invBridgeAdd above: the bag's ceiling and the
	// wire's parted company in v1.8.8, and this is the wire.
	if (removed > 0 && inventoryItemOnWire(item))
		(void)networldSendInvAction(BS_INV_OP_CONSUME, (uint8_t)item, removed, 0);

	return removed;
}

int invBridgePlanQuickMove(const Inventory* inv, int slot, InvBridgeMove* out, int cap)
{
	if (!inv || !out || cap <= 0 || slot < 0 || slot >= INV_SLOT_COUNT) return 0;

	const ItemId item = inv->slots[slot].item;
	if (item == ITEM_NONE) return 0;

	// The plan is run on a private copy with the REAL inventoryMoveUnits, so every described
	// count is what that primitive would actually move — the cap, the same-item merge and the
	// different-item refusal are all its rules, applied by it, not restated here. An Inventory
	// is INV_SLOT_COUNT two-byte slots plus one byte; the copy is cheap and lives on the stack.
	Inventory sim = *inv;

	// The other strip, as a half-open index range. invSlotIsHotbar is world/inventory.h's own
	// definition of the split, so a change to INV_HOTBAR_SLOTS moves this with it.
	const int lo = invSlotIsHotbar(slot) ? INV_HOTBAR_SLOTS : 0;
	const int hi = invSlotIsHotbar(slot) ? INV_SLOT_COUNT   : INV_HOTBAR_SLOTS;

	// Pass 0: same-item stacks, lowest index first. Pass 1: the first empty slots take whatever
	// is left. Both stop the moment the source empties (inventoryMoveUnits clears its item),
	// which is what `sim.slots[slot].item == item` reads.
	int n = 0;
	for (int pass = 0; pass < 2; pass++) {
		const ItemId want = (pass == 0) ? item : ITEM_NONE;
		for (int i = lo; i < hi && n < cap && sim.slots[slot].item == item; i++) {
			if (sim.slots[i].item != want) continue;
			const uint8_t moved = inventoryMoveUnits(&sim, slot, i, sim.slots[slot].count);
			if (moved == 0) continue;   // a same-item stack already at the cap
			out[n].op = BS_INV_OP_MOVE;
			out[n].a  = (uint8_t)slot;
			out[n].b  = (uint8_t)i;
			out[n].c  = moved;
			n++;
		}
	}
	return n;
}

int invBridgePlanChestDeposit(const Inventory* inv, int slot, const ChestState* cs,
                              InvBridgeChestDeposit* out, int cap)
{
	if (!inv || !cs || !out || cap <= 0 || slot < 0 || slot >= INV_SLOT_COUNT) return 0;

	const ItemId item = inv->slots[slot].item;
	if (item == ITEM_NONE) return 0;

	uint8_t left = inv->slots[slot].count;
	if (left == 0) return 0;

	// No simulated copy of the chest here, unlike invBridgePlanQuickMove's `Inventory sim`.
	// It needs one because inventoryMoveUnits is the primitive that owns the rules and it
	// mutates; a chest has no such primitive on this side of the wire (the server owns the
	// authoritative version), so room is arithmetic and the only thing a copy would buy is
	// re-reading a slot this loop visits at most once. Each pass tests the ORIGINAL cs->item,
	// so pass 0 sees only same-item slots and pass 1 only empty ones — disjoint sets — and
	// within a pass `c` never repeats. That is what makes reading cs->count[c] directly safe.
	int n = 0;
	for (int pass = 0; pass < 2 && left > 0 && n < cap; pass++) {
		for (int c = 0; c < CHEST_SLOTS && left > 0 && n < cap; c++) {
			uint8_t room;
			if (pass == 0) {
				// >= rather than ==: a count above the cap would underflow the subtraction
				// below into a room of ~250 and describe a deposit the server would clamp
				// away. Nothing on this side produces one, but the payload arrives off the
				// wire and this is the cheap end of that argument.
				if (cs->item[c] != item || cs->count[c] >= INV_STACK_MAX) continue;
				room = (uint8_t)(INV_STACK_MAX - cs->count[c]);
			} else {
				if (cs->item[c] != ITEM_NONE) continue;
				room = INV_STACK_MAX;   // untouched by pass 0, which skips empty slots
			}

			const uint8_t units = left < room ? left : room;
			out[n].chest_slot = (uint8_t)c;
			out[n].count      = units;
			n++;
			left = (uint8_t)(left - units);
		}
	}
	return n;
}

uint8_t invBridgeApplyMove(Inventory* inv, const InvBridgeMove* m)
{
	if (!inv || !m || m->op != BS_INV_OP_MOVE) return 0;
	return invBridgeMoveUnits(inv, m->a, m->b, m->c);
}

uint8_t invBridgeQuickMove(Inventory* inv, int slot)
{
	InvBridgeMove plan[INV_BRIDGE_PLAN_MAX];
	const int n = invBridgePlanQuickMove(inv, slot, plan, INV_BRIDGE_PLAN_MAX);

	// A whole stack is at most INV_STACK_MAX units, so the sum cannot leave a uint8_t.
	uint8_t moved = 0;
	for (int i = 0; i < n; i++)
		moved = (uint8_t)(moved + invBridgeApplyMove(inv, &plan[i]));
	return moved;
}

bool invBridgeSplitStack(Inventory* inv, int slot, InvSlot* out_lift)
{
	if (!inv || !out_lift || slot < 0 || slot >= INV_SLOT_COUNT) return false;

	InvSlot* s = &inv->slots[slot];
	if (s->item == ITEM_NONE || s->count < 2) return false;   // there is no half of one

	// ceil(count / 2) to the lift; floor(count / 2), which is at least 1, stays behind — so
	// the slot never empties and never needs its item cleared.
	const uint8_t lift = (uint8_t)(s->count - s->count / 2);
	out_lift->item  = s->item;
	out_lift->count = lift;
	s->count = (uint8_t)(s->count - lift);
	return true;
}

// How many of `lift` slot `d` will take, and the transfer itself: an empty `d` takes up to the
// cap, a same-item `d` takes up to the room under it, a different item takes nothing. The lift
// is normalised to { ITEM_NONE, 0 } when it empties, world/inventory.h's InvSlot invariant. The
// one arithmetic shared by place and return, written once.
static uint8_t liftInto(InvSlot* d, InvSlot* lift)
{
	if (lift->item == ITEM_NONE || lift->count == 0) return 0;
	if (d->item != ITEM_NONE && d->item != lift->item) return 0;

	// `d->count >= INV_STACK_MAX` rather than `== `: a count past the cap can only come from
	// a corrupt snapshot or save, and the subtraction below would wrap on it.
	uint8_t space;
	if (d->item == ITEM_NONE)               space = INV_STACK_MAX;
	else if (d->count >= INV_STACK_MAX)     space = 0;
	else                                    space = (uint8_t)(INV_STACK_MAX - d->count);

	const uint8_t move = (lift->count < space) ? lift->count : space;
	if (move == 0) return 0;

	if (d->item == ITEM_NONE) d->item = lift->item;
	d->count    = (uint8_t)(d->count + move);
	lift->count = (uint8_t)(lift->count - move);
	if (lift->count == 0) lift->item = ITEM_NONE;
	return move;
}

uint8_t invBridgePlaceLift(Inventory* inv, int origin, int dst, InvSlot* lift)
{
	if (!inv || !lift || dst < 0 || dst >= INV_SLOT_COUNT) return 0;

	const uint8_t placed = liftInto(&inv->slots[dst], lift);

	// Reported as a MOVE out of the slot the lift was split from — see the header on why the
	// server's copy, which never split, ends up identical. Back onto the origin is a no-op on
	// that copy, so it is not sent.
	if (placed > 0 && origin != dst && slotFitsWire(origin) && slotFitsWire(dst))
		(void)networldSendInvAction(BS_INV_OP_MOVE, (uint8_t)origin, (uint8_t)dst, placed);

	return placed;
}

bool invBridgeReturnLift(Inventory* inv, int origin, InvSlot* lift)
{
	if (!inv || !lift) return false;
	if (lift->item == ITEM_NONE || lift->count == 0) return true;   // nothing carried

	// Onto the origin first: it still holds the same item (or emptied only if a snapshot
	// rewrote it), and the units came out of it, so in single player this always takes all.
	if (origin >= 0 && origin < INV_SLOT_COUNT)
		(void)liftInto(&inv->slots[origin], lift);

	// The origin no longer takes it — a different item sits there now, which only a server
	// snapshot can have done. inventoryAdd, NOT invBridgeAdd: the server was never told these
	// units left the bag, so a PICKUP report for them would credit the player twice.
	if (lift->item != ITEM_NONE) {
		uint8_t leftover = 0;
		(void)inventoryAdd(inv, lift->item, lift->count, &leftover);
		lift->count = leftover;
		if (lift->count == 0) lift->item = ITEM_NONE;
	}
	return lift->item == ITEM_NONE;
}

bool invBridgeApplyState(Inventory* inv, const NetworldInvState* state)
{
	if (!inv || !state) return false;

	// Validate every slot before writing any of them — see the header on why this is
	// all-or-nothing. networld.c has already guaranteed the count/item pairing and the stack
	// cap; the item id against *this build's* block table is the part it structurally cannot
	// check, so it is the only part re-checked here rather than a duplicate of its work.
	for (uint32_t i = 0; i < NETWORLD_INV_SLOT_COUNT; i++) {
		// ITEM_NONE is a *legal* slot value here (an empty slot), which is the one place
		// inventoryCanHold's "not air" half must not be applied — an empty slot is not an
		// item the bag has to be able to carry. The id ceiling itself still comes from
		// that one predicate rather than a second copy of BLOCK_COUNT.
		const ItemId item = (ItemId)state->slots[i].item;
		if (item != ITEM_NONE && !inventoryCanHold(item)) return false;
	}

	for (uint32_t i = 0; i < NETWORLD_INV_SLOT_COUNT; i++) {
		inv->slots[i].item  = (ItemId)state->slots[i].item;
		inv->slots[i].count = state->slots[i].count;
	}

	// Not a plain assignment: routing through inventorySelectHotbar is what applies the clamp
	// its contract promises, so a server sending a selection outside the hotbar range leaves
	// this console pointing at a real hotbar slot rather than reading past slots[] every frame
	// in inventoryHeldItem().
	inventorySelectHotbar(inv, state->selected_hotbar);
	return true;
}
