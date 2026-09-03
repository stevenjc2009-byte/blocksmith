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
// Divides the panel evenly among however many recipes crafting.h declares, so adding one
// costs no layout edit here. 30 px a row at RECIPE_COUNT 3, 23 at 4, and 18 at 5.
// The floor is the 7 px font (gfx/font.h FONT_GLYPH_H) plus enough slop to stay a usable
// touch target.
//
// 2026-09-03, v1.8.12's RECIPE_COAL_ORE_TO_TORCH: two claims that were true at 4 recipes are
// no longer true at 5, and both are worth correcting rather than deleting.
//
// "92 divides exactly by 4, leaving no dead strip" — it does not divide by 5. 92/5 = 18 with
// 2 px left over, so there IS now a 2 px dead strip at the bottom of the panel. Two pixels
// below the last row is cosmetic, not a hit-testing bug: craftRowRect() derives every row from
// CRAFT_ROW_Y0 + i * CRAFT_ROW_H, so the rows stay contiguous and correctly sized and the
// leftover simply sits under the last one, inside the panel and outside every row. Nothing
// lands in it and nothing is clipped by it.
//
// "room for two or three more recipes" — that estimate was made against a 23 px row and it
// has now spent one of its own headroom steps. At 6 recipes this is 15 px, at 7 it is 13, and
// 13 px for a 7 px font is a row with 3 px of clearance above and below the glyphs. So the
// real remaining budget is ONE more recipe at a comfortable size and two at a cramped one,
// after which this has to become a scroll list rather than a fixed split. Written as a number
// rather than as "two or three more" because the previous phrasing is what let a fifth recipe
// land without anyone noticing the row shrank by 5 px.
//
// NOT verified on hardware: whether an 18 px row is comfortable to hit with a stylus on a real
// bottom screen, and whether the longest recipe label still fits. Both are playtest questions,
// and neither is settled by the layout suite, which checks that the rects tile correctly — not
// that a finger can hit them.
#define CRAFT_ROW_H   ((CRAFT_H - CRAFT_CLOSE_H) / RECIPE_COUNT)   // (120-28)/5 = 18

#define HUD_TOGGLE_Y  GRID_Y                       // 40, directly under the hotbar
#define HUD_TOGGLE_H  32
#define HUD_STATUS_Y0 (HUD_TOGGLE_Y + HUD_TOGGLE_H + 6)   // 78

// The vertical extent the HUD screen's status text can reach, as a constant, so the vitals
// strip below can be checked against it instead of against a number somebody remembered.
//
// ui.c's drawHudFont draws its rows at HUD_STATUS_Y0 + k * 12 and there are at most seven of
// them (cols/chunks, meshes/tris/cull, blocks/peak, `status`, `net`, biome, and the optional
// `timing` row), after which it adds a 2 px gap and draws the "tap a hotbar slot to select it"
// hint. Worst case — every optional row present — the hint sits at 164 and its glyphs end at
// 171 (FONT_GLYPH_H is 7). HUD_STATUS_BOTTOM budgets one whole extra row on top of that, so it
// is 174: a deliberate over-estimate, because the point of the constant is to be a floor the
// vitals strip stays below, not a tight measurement.
//
// Deliberately NOT wired into drawHudFont's own `y += 12` steps. Those literals are what
// shipped, three separate comments in that function assert the rows above them are unchanged
// byte-for-byte, and rewriting them to derive from here would be a re-layout of a panel this
// change is only appending to. This constant exists to be *checked against* (see
// ui_layout_test.c's testPipsClearTheStatusRows), and it goes red if either side moves.
#define HUD_STATUS_STEP     12
#define HUD_STATUS_MAX_ROWS  7
#define HUD_STATUS_BOTTOM   (HUD_STATUS_Y0 + (HUD_STATUS_MAX_ROWS + 1) * HUD_STATUS_STEP)  // 174

// ── Vitals strip (health + hunger pips) ────────────────────────────────────────────────
//
// Where these can actually go on a 320x240 bottom screen, which is the whole reason they are
// down here at y=200 and not tucked under the hotbar where a first sketch put them:
//
//   HUD screen (overlay closed)      hotbar        y   0..40   (HOTBAR_Y/HOTBAR_H)
//                                    OPEN INVENTORY y  40..72   (HUD_TOGGLE_Y/_H)
//                                    status text    y  78..171  (see HUD_STATUS_BOTTOM)
//                                    FREE           y 172..240
//   INVENTORY screen (overlay open)  hotbar        y   0..40
//                                    main grid      y  40..120  (GRID_Y/GRID_H)
//                                    crafting panel y 120..240  (CRAFT_Y/CRAFT_H)
//                                    FREE           nothing
//
// So the only unoccupied band on the whole panel is the bottom ~68 px of the HUD screen, and
// it is unoccupied *only* on that screen. Anything at y=2/y=10 lands inside the 40 px hotbar
// band; anything between 40 and 72 lands on the inventory toggle; anything from 78 down to
// ~171 lands on the debug/status rows. Nothing moved to make room — HOTBAR_Y, HUD_TOGGLE_Y and
// HUD_STATUS_Y0 are all byte-identical to what shipped — because nothing needed to.
//
// The consequence, and it is a real one rather than an oversight: ui.c draws this strip from
// drawHudFont only, so the vitals are visible on the HUD and not while the inventory overlay
// is open. On the overlay screen every pixel from y=40 down belongs to the grid or the
// crafting panel, and a vitals row there would have to sit on top of a crafting row.
//
// HUD_PIPS_Y0 is written as SCR_H - 40 rather than as 200 so it stays pinned to the bottom of
// the panel if SCR_H ever changes, which is the edge it is actually anchored to. The two rows
// then occupy y 200..208 (health) and 212..220 (hunger), leaving a 20 px margin under them.
#define HUD_PIP_COUNT      10   // pips per bar
#define HUD_STAT_MAX       20   // points a full bar represents, so 2 points per pip
#define HUD_PIP_W          12
#define HUD_PIP_H           8
#define HUD_PIP_GAP         3
#define HUD_PIP_X0         32   // left of this is the 2-char row label, drawn by ui.c at x=6
#define HUD_PIP_ROW_STEP   12
#define HUD_PIP_ROWS        2
#define HUD_PIP_ROW_HEALTH  0
#define HUD_PIP_ROW_HUNGER  1
#define HUD_PIPS_Y0        (SCR_H - 40)   // 200

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

// ── Vitals pip arithmetic ──────────────────────────────────────────────────────────────
//
// `static inline` in the header, and not a pair of functions in ui_layout.c, because
// ui_layout.c is not this change's to edit — see the lane scope this landed under. The
// consequence that matters is that scene/ui.c and scene/ui_layout_test.c compile the *same*
// definitions rather than two hand-kept copies, which is the only property the test needs to
// be worth running: a test that re-implements the rule it is checking proves nothing about
// the code that ships.

// How full pip `i` of a bar is. `points` is the 0..HUD_STAT_MAX value (health or hunger) and
// each pip is worth 2 points, so pip i covers points [2i, 2i+2).
//
// Out-of-range inputs are safe, which matters because UiStats.health is filled in by another
// module and a 0..20 contract written down only in a comment is one off-by-one from drawing
// nonsense. `i` is guarded explicitly — an empty pip is the only answer that cannot be
// mistaken for a real reading — but `points` needs no clamp: the subtraction below already
// saturates, since any value above HUD_STAT_MAX leaves rem >= 2 for all ten pips (a full bar)
// and any value below 0 leaves rem <= 0 for all ten (an empty one).
//
// That is stated as a fact because it was measured, not assumed. An explicit
// `if (points > HUD_STAT_MAX) points = HUD_STAT_MAX;` was written here first and then deleted:
// a sabotage run that removed it left ui_layout_test.c at PASS 703/703, which is the proof
// that the line could never change an answer. Dead code no check can catch is worse than no
// code, so it is gone; the saturating behaviour it was guarding is still checked, by
// testPipFillClampsOutOfRangeValues.
typedef enum {
	PIP_EMPTY = 0,
	PIP_HALF  = 1,   // an odd `points` leaves exactly one pip in this state
	PIP_FULL  = 2,
} PipFill;

static inline PipFill hudPipFill(int points, int i)
{
	if (i < 0 || i >= HUD_PIP_COUNT) return PIP_EMPTY;

	const int rem = points - 2 * i;
	if (rem >= 2) return PIP_FULL;
	if (rem == 1) return PIP_HALF;
	return PIP_EMPTY;
}

// Pixel rect for pip `i` of row `row` (HUD_PIP_ROW_HEALTH / HUD_PIP_ROW_HUNGER).
static inline URect hudPipRect(int row, int i)
{
	URect r = { HUD_PIP_X0 + i * (HUD_PIP_W + HUD_PIP_GAP),
	            HUD_PIPS_Y0 + row * HUD_PIP_ROW_STEP,
	            HUD_PIP_W, HUD_PIP_H };
	return r;
}
