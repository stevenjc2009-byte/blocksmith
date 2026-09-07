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

// craftCloseRect() and craftRowRect() were here, between gridSlotRect() and hudToggleRect().
// Retired with the fixed crafting split they measured — see scene/ui_layout.h's BAR_LIST_Y0
// block. barListRowRect() below is what replaced them.

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

// ── The bar (v1.9.1 INTERFACE) ──────────────────────────────────────────────────────────
//
// See scene/ui_layout.h's bar blocks for the layout and for why every number is what it is.
// Nothing here re-derives or re-justifies them. The two things this file decides on its own,
// because they are arithmetic rather than layout, are barTabRect()'s running-edge split and
// barCellFromPoint()'s route through the shipped hit tests — both are argued below.

URect barStripRect(void)
{
	URect r = { 0, BAR_STRIP_Y, SCR_W, BAR_STRIP_H };
	return r;
}

URect barTabRect(int i, int count)
{
	if (count < 1 || i < 0 || i >= count) {
		// Zero width and height, so ptInRect() misses it for every point — the same shape
		// furnSlotRect() uses for an index outside its three.
		URect none = { 0, 0, 0, 0 };
		return none;
	}

	// Two running edges, subtracted — NOT i * (BAR_TABS_W / count). The inner division
	// truncates, and the truncation is not cosmetic: at count 4 the even share is 294/4 = 73,
	// four of those tile only 292, and the strip's last two pixel columns then belong to no tab
	// at all. Computing each edge from the full width instead pushes the remainder into
	// whichever tabs the rounding lands in, so widths differ by at most one pixel and the last
	// edge is exactly BAR_TABS_W by construction.
	//
	// It matters at 4 and not at 3 (294 = 3 * 98 exactly), which is why ui_layout_test.c sweeps
	// every pixel column at every count from 1 to BAR_MAX_TABS rather than checking the count
	// this version happens to produce.
	const int x0 = (i * BAR_TABS_W) / count;
	const int x1 = ((i + 1) * BAR_TABS_W) / count;

	URect r = { x0, BAR_STRIP_Y, x1 - x0, BAR_STRIP_H };
	return r;
}

URect barCloseRect(void)
{
	URect r = { BAR_TABS_W, BAR_STRIP_Y, BAR_CLOSE_W, BAR_STRIP_H };
	return r;
}

int hitBarStrip(int x, int y, int count)
{
	// Close first. The two regions are disjoint by construction (the tabs stop at BAR_TABS_W,
	// which is where the close tab starts), so the order cannot change an answer today — it is
	// written this way so that if a future strip ever overlapped them, the door out wins rather
	// than a tab silently swallowing the tap that was meant to leave.
	if (ptInRect(barCloseRect(), x, y)) return BAR_HIT_CLOSE;

	for (int i = 0; i < count; i++)
		if (ptInRect(barTabRect(i, count), x, y)) return i;

	return BAR_HIT_NONE;
}

URect barListRowRect(int visible_row)
{
	if (visible_row < 0 || visible_row >= BAR_LIST_VISIBLE) {
		URect none = { 0, 0, 0, 0 };
		return none;
	}

	// 4 px of margin either side and 2 px shorter than the pitch, written as literals here
	// rather than as two more macros in the header: they are this constructor's own inset, no
	// other rect is placed against them, and the header's pinned list is what four other lanes
	// are coding against.
	URect r = { 4, BAR_LIST_Y0 + visible_row * BAR_LIST_ROW_H,
	            SCR_W - 8, BAR_LIST_ROW_H - 2 };
	return r;
}

int barBuildKinds(bool has_chest, bool has_furnace, BarKind out[BAR_MAX_TABS])
{
	out[0] = BAR_KIND_INVENTORY;
	out[1] = BAR_KIND_CRAFT;

	// has_chest wins if both are set. That state cannot happen — main.c opens one container per
	// PLACE press — so this is not a policy choice between two legitimate inputs, it is a
	// refusal to invent a fourth tab out of a state that means something has already gone wrong
	// upstream. Two container tabs would give the navigation model a category the cursor could
	// sit on while the world holds no such container.
	if (has_chest)   { out[2] = BAR_KIND_CHEST;   return 3; }
	if (has_furnace) { out[2] = BAR_KIND_FURNACE; return 3; }

	return 2;
}

const char* barKindLabel(BarKind k)
{
	switch (k) {
	case BAR_KIND_INVENTORY: return "INVENTORY";
	case BAR_KIND_CRAFT:     return "CRAFT";
	case BAR_KIND_CHEST:     return "CHEST";
	case BAR_KIND_FURNACE:   return "FURNACE";
	}

	// An unlabelled tab, not a "?" — see the header for why a placeholder is worse.
	return "";
}

int barKindRows(BarKind k, int list_len)
{
	switch (k) {
	case BAR_KIND_INVENTORY: return 1 + INV_MAIN_ROWS;       // hotbar + the bag
	case BAR_KIND_CRAFT:     return list_len;                // however many recipes there are
	case BAR_KIND_CHEST:     return 1 + 1 + INV_MAIN_ROWS;   // hotbar + the chest + the bag
	case BAR_KIND_FURNACE:   return 1 + 1 + INV_MAIN_ROWS;   // hotbar + in/fuel/out + the bag
	}

	return 0;
}

int barKindCols(BarKind k, int row)
{
	if (row < 0) return 0;

	// A list is one cell wide at every row, however long it is — checked before the row bound
	// below, because this is the one kind whose row count is not knowable from `k` alone.
	if (k == BAR_KIND_CRAFT) return 1;

	if (row >= barKindRows(k, 0)) return 0;

	switch (k) {
	case BAR_KIND_INVENTORY:
		return INV_HOTBAR_SLOTS;
	case BAR_KIND_CHEST:
		// CHEST_SLOTS rather than INV_HOTBAR_SLOTS for the chest's own row, even though the two
		// are both 8 today: the chest row is world/chest.h's width and the bag rows are
		// world/inventory.h's, and writing whichever one happened to be equal is how a table
		// stops being a statement about the layout.
		return (row == 1) ? CHEST_SLOTS : INV_HOTBAR_SLOTS;
	case BAR_KIND_FURNACE:
		// Input, fuel, output — the one row on the whole bar that is not eight cells wide, and
		// the reason barKindCols() takes a row at all rather than being a per-kind constant.
		return (row == 1) ? 3 : INV_HOTBAR_SLOTS;
	case BAR_KIND_CRAFT:
		break;   // handled above
	}

	return 0;
}

bool barKindHorizontalIsContent(BarKind k)
{
	switch (k) {
	case BAR_KIND_INVENTORY:
	case BAR_KIND_CHEST:
	case BAR_KIND_FURNACE:
		return true;    // left/right is the only way to reach column 7 of a grid row
	case BAR_KIND_CRAFT:
		return false;   // one cell wide, so left/right belongs to the tabs
	}

	// An unknown kind gets the tab behaviour, not the cell behaviour: moving a cursor sideways
	// inside a category that does not exist is the answer that cannot be recovered from.
	return false;
}

URect barCellRect(BarKind k, int row, int col, int scroll)
{
	const URect none = { 0, 0, 0, 0 };

	// One bound check for every kind, asked of the same table the navigation model uses. A cell
	// this rejects is a cell barKindRows()/barKindCols() say does not exist, so the two can
	// never disagree about how big a category is.
	if (row < 0 || col < 0 || col >= barKindCols(k, row)) return none;

	switch (k) {
	case BAR_KIND_INVENTORY:
		if (row == 0) return hotbarSlotRect(col);
		return gridSlotRect((row - 1) * INV_MAIN_COLS + col);

	case BAR_KIND_CRAFT:
		// Off the top or bottom of the window is a zero rect, so a caller that draws every
		// recipe without clipping draws nothing for the scrolled-off ones — see barListRowRect.
		return barListRowRect(row - scroll);

	case BAR_KIND_CHEST:
		if (row == 0) return hotbarSlotRect(col);
		if (row == 1) return chestSlotRect(col);
		return chestGridSlotRect((row - 2) * INV_MAIN_COLS + col);

	case BAR_KIND_FURNACE:
		if (row == 0) return hotbarSlotRect(col);
		if (row == 1) return furnSlotRect(col);   // col is FURN_HIT_INPUT/_FUEL/_OUTPUT
		return furnGridSlotRect((row - 2) * INV_MAIN_COLS + col);
	}

	return none;
}

bool barCellFromPoint(BarKind k, int x, int y, int scroll, int* row, int* col)
{
	// Routed through the hit tests the three panels already shipped and NOT through
	// barCellRect(), so the rect side and the hit side stay two independent paths — see the
	// header for what that buys and what it costs. The conversions below are the only place
	// the (row, col) addressing meets the flat slot indices those hit tests speak.
	switch (k) {
	case BAR_KIND_INVENTORY: {
		const int s = hitInventorySlot(x, y, true);
		if (s < 0) return false;
		if (s < INV_HOTBAR_SLOTS) { *row = 0; *col = s; return true; }

		const int i = s - INV_HOTBAR_SLOTS;
		*row = 1 + i / INV_MAIN_COLS;
		*col = i % INV_MAIN_COLS;
		return true;
	}

	case BAR_KIND_CRAFT:
		// The one kind with no shipped hit test to reuse. Walks the window rather than dividing
		// by the pitch, because the rows have a 2 px gap between them and a division would
		// resolve a point in the gap to the row above it.
		for (int v = 0; v < BAR_LIST_VISIBLE; v++)
			if (ptInRect(barListRowRect(v), x, y)) {
				*row = scroll + v;
				*col = 0;
				return true;
			}
		return false;

	case BAR_KIND_CHEST: {
		const int cs = hitChestSlot(x, y);
		if (cs >= 0) { *row = 1; *col = cs; return true; }

		const int s = hitChestInvSlot(x, y);
		if (s < 0) return false;
		if (s < INV_HOTBAR_SLOTS) { *row = 0; *col = s; return true; }

		const int i = s - INV_HOTBAR_SLOTS;
		*row = 2 + i / INV_MAIN_COLS;
		*col = i % INV_MAIN_COLS;
		return true;
	}

	case BAR_KIND_FURNACE: {
		const int fs = hitFurnaceSlot(x, y);
		if (fs != FURN_HIT_NONE) { *row = 1; *col = fs; return true; }

		const int s = hitFurnaceInvSlot(x, y);
		if (s < 0) return false;
		if (s < INV_HOTBAR_SLOTS) { *row = 0; *col = s; return true; }

		const int i = s - INV_HOTBAR_SLOTS;
		*row = 2 + i / INV_MAIN_COLS;
		*col = i % INV_MAIN_COLS;
		return true;
	}
	}

	return false;
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
