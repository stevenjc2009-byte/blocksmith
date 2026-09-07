#include "scene/ui_layout.h"

bool ptInRect(URect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

URect hotbarSlotRect(int i)
{
	URect r = { i * SLOT_PX, HOTBAR_Y, SLOT_PX, HOTBAR_H };
	return r;
}

URect gridSlotRect(int i)
{
	const int col = i % INV_MAIN_COLS;
	const int row = i / INV_MAIN_COLS;
	URect r = { col * SLOT_PX, GRID_Y + row * SLOT_PX, SLOT_PX, SLOT_PX };
	return r;
}

URect craftCloseRect(void)
{
	URect r = { 0, CRAFT_Y, SCR_W, CRAFT_CLOSE_H };
	return r;
}

URect craftRowRect(int i)
{
	URect r = { 10, CRAFT_ROW_Y0 + i * CRAFT_ROW_H, SCR_W - 20, CRAFT_ROW_H - 2 };
	return r;
}

URect hudToggleRect(void)
{
	URect r = { 0, HUD_TOGGLE_Y, SCR_W, HUD_TOGGLE_H };
	return r;
}

int hitInventorySlot(int x, int y, bool overlay_open)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		if (ptInRect(hotbarSlotRect(i), x, y)) return i;

	if (overlay_open) {
		for (int i = 0; i < INV_MAIN_SLOTS; i++)
			if (ptInRect(gridSlotRect(i), x, y)) return INV_HOTBAR_SLOTS + i;
	}

	return -1;
}

// ── Furnace panel (v1.8.15 FURNACE) ────────────────────────────────────────────────────
//
// See scene/ui_layout.h's furnace block for the layout itself and for why every one of these
// numbers is what it is. Nothing here re-derives or re-justifies them.

URect furnCloseRect(void)
{
	URect r = { 0, FURN_CLOSE_Y, SCR_W, FURN_CLOSE_H };
	return r;
}

URect furnInputRect(void)
{
	URect r = { FURN_INPUT_X, FURN_ROW_Y, FURN_SLOT_PX, FURN_SLOT_PX };
	return r;
}

URect furnFuelRect(void)
{
	URect r = { FURN_FUEL_X, FURN_ROW_Y, FURN_SLOT_PX, FURN_SLOT_PX };
	return r;
}

URect furnOutputRect(void)
{
	URect r = { FURN_OUTPUT_X, FURN_ROW_Y, FURN_SLOT_PX, FURN_SLOT_PX };
	return r;
}

URect furnSlotRect(int which)
{
	switch (which) {
	case FURN_HIT_INPUT:  return furnInputRect();
	case FURN_HIT_FUEL:   return furnFuelRect();
	case FURN_HIT_OUTPUT: return furnOutputRect();
	default: {
		// Zero width and height, so ptInRect() misses it for every point — see the header.
		URect none = { 0, 0, 0, 0 };
		return none;
	}
	}
}

URect furnBurnRect(void)
{
	URect r = { FURN_BURN_X, FURN_BURN_Y, FURN_BURN_W, FURN_BURN_H };
	return r;
}

URect furnArrowRect(void)
{
	URect r = { FURN_ARROW_X, FURN_ARROW_Y, FURN_ARROW_W, FURN_ARROW_H };
	return r;
}

URect furnGridSlotRect(int i)
{
	const int col = i % INV_MAIN_COLS;
	const int row = i / INV_MAIN_COLS;
	URect r = { col * SLOT_PX, FURN_GRID_Y + row * SLOT_PX, SLOT_PX, SLOT_PX };
	return r;
}

int hitFurnaceSlot(int x, int y)
{
	if (ptInRect(furnInputRect(),  x, y)) return FURN_HIT_INPUT;
	if (ptInRect(furnFuelRect(),   x, y)) return FURN_HIT_FUEL;
	if (ptInRect(furnOutputRect(), x, y)) return FURN_HIT_OUTPUT;
	return FURN_HIT_NONE;
}

int hitFurnaceInvSlot(int x, int y)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		if (ptInRect(hotbarSlotRect(i), x, y)) return i;

	for (int i = 0; i < INV_MAIN_SLOTS; i++)
		if (ptInRect(furnGridSlotRect(i), x, y)) return INV_HOTBAR_SLOTS + i;

	return -1;
}

// ── Chest panel (v1.9.0 CHEST) ──────────────────────────────────────────────────────────
//
// See ui_layout.h's chest panel block for the layout itself and for why every one of these
// numbers is what it is. Nothing here re-derives or re-justifies them.

URect chestCloseRect(void)
{
	URect r = { 0, CHEST_CLOSE_Y, SCR_W, CHEST_CLOSE_H };
	return r;
}

URect chestSlotRect(int i)
{
	URect r = { i * SLOT_PX, CHEST_ROW_Y, SLOT_PX, SLOT_PX };
	return r;
}

URect chestGridSlotRect(int i)
{
	const int col = i % INV_MAIN_COLS;
	const int row = i / INV_MAIN_COLS;
	URect r = { col * SLOT_PX, CHEST_GRID_Y + row * SLOT_PX, SLOT_PX, SLOT_PX };
	return r;
}

int hitChestSlot(int x, int y)
{
	for (int i = 0; i < CHEST_SLOTS; i++)
		if (ptInRect(chestSlotRect(i), x, y)) return i;
	return -1;
}

int hitChestInvSlot(int x, int y)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		if (ptInRect(hotbarSlotRect(i), x, y)) return i;

	for (int i = 0; i < INV_MAIN_SLOTS; i++)
		if (ptInRect(chestGridSlotRect(i), x, y)) return INV_HOTBAR_SLOTS + i;

	return -1;
}

int furnBarFill(int total_px, int num, int den)
{
	// Order matters here and the order is "reject the impossible, then clamp the extremes,
	// then divide" — the division is the last thing that happens, so no guard below it can be
	// skipped by an early return added later.
	//
	// ── What the den guard actually buys, measured rather than assumed ──────────────────
	//
	// It was written to stop a division by zero. It does not, and the difference is worth
	// recording rather than leaving a comment that is wrong in a flattering direction: with
	// this line DELETED, `num >= den` below already catches every den <= 0 that has a positive
	// numerator, and `num <= 0` catches the rest, so the divide is still never reached. A
	// sabotage arm that removed the line entirely did not crash — it returned total_px and
	// failed 15 named checks in testFurnBarFillGuardsTheDivision, first
	// "L692 furnBarFill(t, 5, 0) == 0".
	//
	// So what this line is for is the ANSWER, not the crash: an idle furnace (no fuel item, so
	// no burn duration to measure against; no recipe, so no cook duration) must draw an EMPTY
	// bar, and without this line it draws a FULL one — a furnace with nothing in it reading as
	// a furnace about to finish. That is a worse bug than the crash would have been, because
	// nothing about it looks like a failure.
	if (total_px <= 0) return 0;
	if (den <= 0)      return 0;
	if (num <= 0)      return 0;
	if (num >= den)    return total_px;

	// num and den are both in [1, INT_MAX) here and num < den, so the product cannot exceed
	// total_px * den and the quotient is strictly inside [0, total_px). Written as
	// (total * num) / den rather than total * (num / den) because the latter is integer
	// division first and is therefore always 0 — the classic version of this bug, and one no
	// screenshot would distinguish from "the bar just never fills".
	//
	// int and not long: the real callers pass total_px <= FURN_ARROW_W (80) and num < den <=
	// FURNACE_FUEL_TICKS_LOG (1200), so the widest product this can actually see is 80 * 1199
	// = 95,920. The guards above bound num by den, so even a corrupt uint16_t payload caps the
	// product at 80 * 65535 = 5.2M — three orders of magnitude inside a 32-bit int on both
	// targets. Checked rather than assumed because ARM EABI has already made a host sizeof()
	// lie in this codebase once (world/chunk.h's Chunk), and int is 32 bits on both.
	return (total_px * num) / den;
}

uint8_t uiMoveStackInto(InvSlot* src, ItemId* dst_item, uint8_t* dst_count)
{
	if (src->item == ITEM_NONE || src->count == 0) return 0;

	// `empty` is read off the ITEM, not the count — see the header for the corrupt-payload
	// case this is guarding, and for why refusing a mismatched destination is deliberate
	// rather than the swap an inventory-to-inventory move would do.
	const bool empty = (*dst_item == ITEM_NONE);
	if (!empty && *dst_item != src->item) return 0;

	const int held = empty ? 0 : (int)*dst_count;
	const int room = INV_STACK_MAX - held;
	if (room <= 0) return 0;

	uint8_t n = src->count;
	if ((int)n > room) n = (uint8_t)room;
	if (n == 0) return 0;

	*dst_item  = src->item;
	*dst_count = (uint8_t)(held + n);

	src->count = (uint8_t)(src->count - n);
	if (src->count == 0) src->item = ITEM_NONE;

	return n;
}
