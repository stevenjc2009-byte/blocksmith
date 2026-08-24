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
	const uint8_t landed = (uint8_t)(count - leftover);
	if (landed > 0 && inventoryCanHold(item))
		(void)networldSendInvAction(BS_INV_OP_PICKUP, (uint8_t)item, landed, 0);

	return r;
}

uint8_t invBridgeRemove(Inventory* inv, ItemId item, uint8_t count)
{
	const uint8_t removed = inventoryRemove(inv, item, count);

	if (removed > 0 && inventoryCanHold(item))
		(void)networldSendInvAction(BS_INV_OP_CONSUME, (uint8_t)item, removed, 0);

	return removed;
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
