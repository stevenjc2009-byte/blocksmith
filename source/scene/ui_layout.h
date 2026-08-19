// Step 8.2 (touchscreen UI half) — the pure layout/hit-test arithmetic pulled out of
// scene/ui.c so it can be host-tested. scene/ui.c includes <citro3d.h> for its C3D_Tex*
// draw parameter, which makes the whole file unlinkable outside a devkitARM build; nothing
// in *this* pair of files needs a texture, a render target, or any libctru/citro3d symbol
// at all — it is exactly the rectangle math ui.c's own file comment already calls out as
// "pure integer arithmetic, no GPU state and no I/O" on hitInventorySlot, generalised to
// every rect this screen lays out, not just that one function.
//
// ui.c is the only caller. It includes this header and calls into these functions instead
// of holding its own copies of the rects or the constants below — see this file's own
// comment on why the numbers have to stay byte-identical to what shipped before this split
// (tools/run_host_tests.sh's new ui_layout_test binary is what proves that, not this file).
//
// No <3ds.h>, no <citro3d.h>, no gfx/*.h — only <stdbool.h>/<stdint.h> and the two data
// headers (world/inventory.h for INV_HOTBAR_SLOTS/INV_MAIN_COLS/INV_MAIN_ROWS, world/
// crafting.h for RECIPE_COUNT, which CRAFT_ROW_H below divides the crafting panel's height
// by). Both of those headers are already pure C themselves — see their own file comments —
// so pulling them in here does not reintroduce the console dependency this split exists to
// remove.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/crafting.h"
#include "world/inventory.h"

// ── Layout constants ──────────────────────────────────────────────────────────────────
//
// Copied verbatim from scene/ui.c's own layout block — see that file for the reasoning
// behind every one of these numbers (the 320x240 panel, the 5.216 px/mm figure, the
// hotbar/grid/crafting vertical budget). This header does not re-derive or re-justify any
// of it; it is the same arithmetic, just made reachable without <citro3d.h>.
#define SCR_W 320
#define SCR_H 240

// One cell, shared by the hotbar and the main grid: SCR_W / INV_HOTBAR_SLOTS = 320/8 = 40 px.
#define SLOT_PX (SCR_W / INV_HOTBAR_SLOTS)

#define HOTBAR_Y 0
#define HOTBAR_H SLOT_PX                          // 40 px

#define GRID_Y   (HOTBAR_Y + HOTBAR_H)             // 40
#define GRID_H   (INV_MAIN_ROWS * SLOT_PX)         // 80 (2 rows of 40)

#define CRAFT_Y  (GRID_Y + GRID_H)                 // 120
#define CRAFT_H  (SCR_H - CRAFT_Y)                 // 120

#define CRAFT_CLOSE_H 28
#define CRAFT_ROW_Y0  (CRAFT_Y + CRAFT_CLOSE_H)              // 148
#define CRAFT_ROW_H   ((CRAFT_H - CRAFT_CLOSE_H) / RECIPE_COUNT)   // (120-28)/3 = 30

#define HUD_TOGGLE_Y  GRID_Y                       // 40, directly under the hotbar
#define HUD_TOGGLE_H  32
#define HUD_STATUS_Y0 (HUD_TOGGLE_Y + HUD_TOGGLE_H + 6)   // 78

// ── Rects ──────────────────────────────────────────────────────────────────────────────

typedef struct { int x, y, w, h; } URect;

bool ptInRect(URect r, int x, int y);

// Pixel rect for hotbar slot `i` (0..INV_HOTBAR_SLOTS-1).
URect hotbarSlotRect(int i);

// Pixel rect for main-grid slot `i` (0..INV_MAIN_SLOTS-1 — an offset from
// INV_HOTBAR_SLOTS, not an inventory.h slot index itself).
URect gridSlotRect(int i);

URect craftCloseRect(void);
URect craftRowRect(int i);
URect hudToggleRect(void);

// The one function that turns a raw touch point into "which inventory.h slot (if any) was
// hit", covering both strips at once. See ui.c's original file comment (now here) for the
// contract: (a) a point inside hotbar cell i always returns i regardless of `overlay_open`,
// (b) a point inside main-grid cell i returns INV_HOTBAR_SLOTS + i only when `overlay_open`
// is true and returns -1 for the same point when it is false, and (c) a point in the gutter
// between cells, or below the grid when the overlay is closed, returns -1.
int hitInventorySlot(int x, int y, bool overlay_open);
