// Host self-test for scene/ui_layout.c — the pixel/hit-test arithmetic pulled out of
// scene/ui.c precisely so it could be checked here instead of on a real touchscreen (see
// ui_layout.h's file comment). Self-contained (its own main()), same shape and same reason
// as world/inventory_test.c and world/worldlist_test.c: this module carries no state of its
// own to fold into tools/run_host_tests.sh's shared world_test binary, and a broken layout
// check must not be able to stop that binary's own suite from running.
//
// The CHECK macro and the PASS/FAIL summary line are copied from world/inventory_test.c,
// which copied them from world/world_test.c — same shape everywhere so a failure here reads
// the same way a failure anywhere else in this project does.
//
// The __3DS__ guard around the *whole file* is load-bearing, not tidy — copied verbatim
// from inventory_test.c's own comment on this: the Makefile globs every .c under source/
// scene into the console build, so without the guard this file's main() links against
// source/main.c's and the build dies with "multiple definition of `main'".
#ifndef __3DS__

#include <stdio.h>

#include "scene/ui_layout.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

static void centerOf(URect r, int* cx, int* cy)
{
	*cx = r.x + r.w / 2;
	*cy = r.y + r.h / 2;
}

// ── Round trips: the centre of every slot must map back to that exact slot ─────────────

// Every hotbar slot round-trips regardless of overlay_open — the hotbar is drawn, and live,
// in both screens (see ui_layout.h's hotbarSlotRect comment in ui.c's original file).
static void testHotbarSlotsRoundTripBothScreens(void)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++) {
		int cx, cy;
		centerOf(hotbarSlotRect(i), &cx, &cy);

		CHECK(hitInventorySlot(cx, cy, false) == i);
		CHECK(hitInventorySlot(cx, cy, true)  == i);
	}
}

// Every main-grid slot round-trips to INV_HOTBAR_SLOTS + i, but only when overlay_open is
// true — this is the whole contract hitInventorySlot's file comment calls out.
static void testGridSlotsRoundTripOnlyWhenOverlayOpen(void)
{
	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		int cx, cy;
		centerOf(gridSlotRect(i), &cx, &cy);

		CHECK(hitInventorySlot(cx, cy, true) == INV_HOTBAR_SLOTS + i);
		CHECK(hitInventorySlot(cx, cy, false) == -1);
	}
}

// ── overlay_open=false exposes ONLY the hotbar ──────────────────────────────────────────

// Restates testGridSlotsRoundTripOnlyWhenOverlayOpen's closed-screen half as its own named
// test (per the task's explicit checklist), plus a handful of non-centre points spread
// across every main-grid row/col so this is not just re-checking the same 16 centres.
static void testOverlayClosedExposesOnlyHotbar(void)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++) {
		int cx, cy;
		centerOf(hotbarSlotRect(i), &cx, &cy);
		CHECK(hitInventorySlot(cx, cy, false) == i);
	}

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const URect r = gridSlotRect(i);
		// Top-left pixel and centre pixel of every grid cell, not just the centre.
		CHECK(hitInventorySlot(r.x, r.y, false) == -1);
		int cx, cy;
		centerOf(r, &cx, &cy);
		CHECK(hitInventorySlot(cx, cy, false) == -1);
	}
}

// ── overlay_open=true exposes hotbar + main grid ────────────────────────────────────────

static void testOverlayOpenExposesHotbarAndGrid(void)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++) {
		int cx, cy;
		centerOf(hotbarSlotRect(i), &cx, &cy);
		CHECK(hitInventorySlot(cx, cy, true) == i);
	}
	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		int cx, cy;
		centerOf(gridSlotRect(i), &cx, &cy);
		CHECK(hitInventorySlot(cx, cy, true) == INV_HOTBAR_SLOTS + i);
	}
}

// ── Points outside the panel entirely ───────────────────────────────────────────────────

static void testPointsOutsidePanelReturnNoSlot(void)
{
	static const int xs[] = { -1, -100, SCR_W, SCR_W + 100 };
	static const int ys[] = { -1, -100, SCR_H, SCR_H + 100 };

	for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
		CHECK(hitInventorySlot(xs[i], 0, false) == -1);
		CHECK(hitInventorySlot(xs[i], 0, true)  == -1);
	}
	for (size_t i = 0; i < sizeof(ys) / sizeof(ys[0]); i++) {
		CHECK(hitInventorySlot(0, ys[i], false) == -1);
		CHECK(hitInventorySlot(0, ys[i], true)  == -1);
	}
	// A point off in both axes at once.
	CHECK(hitInventorySlot(-50, -50, false) == -1);
	CHECK(hitInventorySlot(SCR_W + 50, SCR_H + 50, true) == -1);

	// The crafting panel's own area (y >= CRAFT_Y): hitInventorySlot only ever tests the
	// hotbar and main-grid rects, so a point here is "no slot" regardless of overlay_open,
	// even though it is inside the 320x240 screen and would land in craftCloseRect/
	// craftRowRect if those were what was being tested (they are ui.c's own separate
	// dispatch, not hitInventorySlot's job).
	CHECK(hitInventorySlot(10, CRAFT_Y, true) == -1);
	CHECK(hitInventorySlot(10, CRAFT_Y + 50, true) == -1);
	CHECK(hitInventorySlot(10, CRAFT_Y, false) == -1);
}

// ── Boundary pixels of the first and last slot on each edge ────────────────────────────

static void testBoundaryPixels(void)
{
	// Hotbar slot 0: left/top edge of the whole panel. (0,0) is the first in-bounds pixel;
	// one pixel further out on either axis falls outside everything.
	CHECK(hitInventorySlot(0, 0, false) == 0);
	CHECK(hitInventorySlot(-1, 0, false) == -1);
	CHECK(hitInventorySlot(0, -1, false) == -1);

	// Hotbar slot 0's bottom edge: y = HOTBAR_H - 1 is still the hotbar row; y = HOTBAR_H
	// is the first row of the main grid, which only resolves when overlay_open.
	CHECK(hitInventorySlot(0, HOTBAR_H - 1, false) == 0);
	CHECK(hitInventorySlot(0, HOTBAR_H,     false) == -1);   // grid row, but overlay closed
	CHECK(hitInventorySlot(0, HOTBAR_H,     true)  == INV_HOTBAR_SLOTS + 0);

	// Hotbar slot INV_HOTBAR_SLOTS-1 (last): right edge of the whole 320px panel.
	// SCR_W - 1 is the last in-bounds column; SCR_W itself is one past the screen.
	CHECK(hitInventorySlot(SCR_W - 1, 0, false) == INV_HOTBAR_SLOTS - 1);
	CHECK(hitInventorySlot(SCR_W,     0, false) == -1);

	// The seam between hotbar slot i-1 and slot i: the last pixel of one cell and the first
	// pixel of the next resolve to different slots, with no gap or overlap between them —
	// the cells tile SCR_W exactly (SLOT_PX * INV_HOTBAR_SLOTS == SCR_W), so this is the
	// closest thing to a "gutter" this layout actually has.
	CHECK(hitInventorySlot(SLOT_PX - 1, 0, false) == 0);
	CHECK(hitInventorySlot(SLOT_PX,     0, false) == 1);

	// Main grid, first slot (row 0, col 0): top-left pixel of the grid strip.
	CHECK(hitInventorySlot(0, GRID_Y, true) == INV_HOTBAR_SLOTS + 0);
	CHECK(hitInventorySlot(-1, GRID_Y, true) == -1);

	// Main grid, last slot (row INV_MAIN_ROWS-1, col INV_MAIN_COLS-1): bottom-right pixel
	// of the grid strip. One row further down is the crafting panel's own area, not a grid
	// row, and must miss even with the overlay open.
	const int last_grid_slot = INV_MAIN_SLOTS - 1;
	CHECK(hitInventorySlot(SCR_W - 1, GRID_Y + GRID_H - 1, true) == INV_HOTBAR_SLOTS + last_grid_slot);
	CHECK(hitInventorySlot(SCR_W - 1, GRID_Y + GRID_H,     true) == -1);
	CHECK(hitInventorySlot(SCR_W,     GRID_Y + GRID_H - 1, true) == -1);
}

// ── v1.8.13 SURV-HUD: the health/hunger pip strip ───────────────────────────────────────
//
// These check the two things a screenshot cannot: that the 0..20 -> ten-pip arithmetic is
// right for every interesting value including the odd ones, and that the strip's rects do not
// land on top of anything else the HUD draws. A capture proves the pips are visible and the
// right colour; it cannot prove that health 19 leaves the half pip in slot 9 rather than
// slot 8, and it cannot prove the strip clears the status rows in the worst case (every
// optional row present) rather than only in the configuration that happened to boot.
//
// hudPipFill and hudPipRect are `static inline` in ui_layout.h, so these call the exact
// definitions scene/ui.c compiles — not a copy. See that header's comment on why they are in
// the header at all.

static bool rectsOverlap(URect a, URect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

// A table of expected READINGS, deliberately written out by hand rather than derived, so this
// is not hudPipFill checked against a second copy of hudPipFill. `full` is how many whole pips
// the value should light, `half` whether the pip immediately after them is the half state.
static void testPipFillScale(void)
{
	static const struct { int points, full, half; } scale[] = {
		{  0,  0, 0 },   // empty bar
		{  1,  0, 1 },   // the smallest non-zero reading is a half pip, not a whole one
		{  2,  1, 0 },
		{  3,  1, 1 },
		{  4,  2, 0 },
		{  5,  2, 1 },
		{ 12,  6, 0 },
		{ 13,  6, 1 },   // the odd value the visual check uses
		{ 19,  9, 1 },   // one point short of full: half pip in the LAST slot
		{ 20, 10, 0 },   // full bar
	};

	for (size_t k = 0; k < sizeof(scale) / sizeof(scale[0]); k++) {
		const int pts = scale[k].points;
		for (int i = 0; i < HUD_PIP_COUNT; i++) {
			PipFill want = PIP_EMPTY;
			if (i < scale[k].full)                      want = PIP_FULL;
			else if (i == scale[k].full && scale[k].half) want = PIP_HALF;
			CHECK(hudPipFill(pts, i) == want);
		}
	}
}

// Restated as its own property: an odd value leaves exactly ONE half pip and an even value
// leaves none. This is the half-pip decision itself, and it is the check that goes red if
// somebody later "simplifies" the rule into a round-to-nearest.
static void testOddValuesLeaveExactlyOneHalfPip(void)
{
	for (int pts = 0; pts <= HUD_STAT_MAX; pts++) {
		int halves = 0;
		for (int i = 0; i < HUD_PIP_COUNT; i++)
			if (hudPipFill(pts, i) == PIP_HALF) halves++;

		CHECK(halves == (pts % 2));
	}
}

// A full bar is HUD_STAT_MAX and nothing above it, an empty bar is 0 and nothing below it.
// UiStats.health is filled in by another module, so this is the boundary that stops a bad
// value drawing a bar that runs off its own tenth pip.
//
// hudPipFill has no explicit clamp — see its comment for why one was written and then deleted.
// This check still discriminates: sabotaging the fill rule to `rem >= 2 && rem <= HUD_STAT_MAX`
// (a plausible "tidy the range up" edit) makes every too_high value read PIP_EMPTY and takes
// this function to 50 failures.
static void testPipFillClampsOutOfRangeValues(void)
{
	static const int too_high[] = { HUD_STAT_MAX + 1, 21, 100, 255, 1000 };
	static const int too_low[]  = { -1, -2, -100, -1000 };

	for (size_t k = 0; k < sizeof(too_high) / sizeof(too_high[0]); k++)
		for (int i = 0; i < HUD_PIP_COUNT; i++)
			CHECK(hudPipFill(too_high[k], i) == PIP_FULL);

	for (size_t k = 0; k < sizeof(too_low) / sizeof(too_low[0]); k++)
		for (int i = 0; i < HUD_PIP_COUNT; i++)
			CHECK(hudPipFill(too_low[k], i) == PIP_EMPTY);
}

// An index outside the bar reads empty, never full — the one answer that cannot be mistaken
// for a real reading.
static void testPipFillRejectsOutOfRangeIndex(void)
{
	CHECK(hudPipFill(HUD_STAT_MAX, -1) == PIP_EMPTY);
	CHECK(hudPipFill(HUD_STAT_MAX, -100) == PIP_EMPTY);
	CHECK(hudPipFill(HUD_STAT_MAX, HUD_PIP_COUNT) == PIP_EMPTY);
	CHECK(hudPipFill(HUD_STAT_MAX, HUD_PIP_COUNT + 50) == PIP_EMPTY);
}

// Every pip is on the panel, the pips in a row are evenly spaced and never overlap each other,
// and the hunger row sits exactly one step below the health row.
static void testPipRectsTileTheStrip(void)
{
	for (int row = 0; row < HUD_PIP_ROWS; row++) {
		for (int i = 0; i < HUD_PIP_COUNT; i++) {
			const URect r = hudPipRect(row, i);

			CHECK(r.x >= 0);
			CHECK(r.x + r.w <= SCR_W);
			CHECK(r.y >= 0);
			CHECK(r.y + r.h <= SCR_H);

			if (i > 0) {
				const URect p = hudPipRect(row, i - 1);
				// Two separate claims. The first is that hudPipRect's own arithmetic agrees
				// with HUD_PIP_GAP, which catches a broken formula but NOT a bad value of the
				// constant — it is stated against the constant, so it cannot fail when the
				// constant moves (a sabotage run setting HUD_PIP_GAP to 0 left the whole suite
				// at PASS 703, which is what put the second line here). The second is the claim
				// that actually constrains the value: adjacent pips must be visibly separated,
				// so a gap of 0 — ten pips fused into one 120 px bar — is a failure.
				CHECK(r.x - (p.x + p.w) == HUD_PIP_GAP);
				CHECK(r.x - (p.x + p.w) >= 1);
				CHECK(!rectsOverlap(p, r));
			}
		}
	}

	for (int i = 0; i < HUD_PIP_COUNT; i++) {
		const URect hp = hudPipRect(HUD_PIP_ROW_HEALTH, i);
		const URect fd = hudPipRect(HUD_PIP_ROW_HUNGER, i);

		CHECK(fd.y - hp.y == HUD_PIP_ROW_STEP);
		CHECK(!rectsOverlap(hp, fd));
	}
}

// The collision question this lane exists to answer, as a check rather than as a comment: the
// strip must clear the hotbar band, the OPEN INVENTORY toggle, and the debug/status rows —
// including the optional ones that are absent in most sessions and therefore absent from any
// one screenshot. HUD_STATUS_BOTTOM is ui_layout.h's worst-case floor for those rows.
static void testPipsClearTheStatusRows(void)
{
	CHECK(HUD_PIPS_Y0 >= HUD_STATUS_BOTTOM);
	CHECK(HUD_PIPS_Y0 >= HOTBAR_Y + HOTBAR_H);
	CHECK(HUD_PIPS_Y0 >= HUD_TOGGLE_Y + HUD_TOGGLE_H);

	// The whole strip, last row included, stays on the panel.
	const URect last = hudPipRect(HUD_PIP_ROWS - 1, HUD_PIP_COUNT - 1);
	CHECK(last.y + last.h <= SCR_H);

	for (int row = 0; row < HUD_PIP_ROWS; row++) {
		for (int i = 0; i < HUD_PIP_COUNT; i++) {
			const URect r = hudPipRect(row, i);

			for (int s = 0; s < INV_HOTBAR_SLOTS; s++)
				CHECK(!rectsOverlap(r, hotbarSlotRect(s)));

			CHECK(!rectsOverlap(r, hudToggleRect()));

			// And no pip is inside a hotbar cell as far as the hit test is concerned, so a
			// stylus tap on the strip cannot be read as a hotbar selection.
			int cx, cy;
			centerOf(r, &cx, &cy);
			CHECK(hitInventorySlot(cx, cy, false) == -1);
		}
	}
}

int main(void)
{
	testHotbarSlotsRoundTripBothScreens();
	testGridSlotsRoundTripOnlyWhenOverlayOpen();
	testOverlayClosedExposesOnlyHotbar();
	testOverlayOpenExposesHotbarAndGrid();
	testPointsOutsidePanelReturnNoSlot();
	testBoundaryPixels();

	testPipFillScale();
	testOddValuesLeaveExactlyOneHalfPip();
	testPipFillClampsOutOfRangeValues();
	testPipFillRejectsOutOfRangeIndex();
	testPipRectsTileTheStrip();
	testPipsClearTheStatusRows();

	if (s_fails == 0)
		printf("ui_layout self-test: PASS  %d checks\n", s_checks);
	else
		printf("ui_layout self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int ui_layout_test_host_only_t;

#endif   // !__3DS__
