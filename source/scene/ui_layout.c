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
