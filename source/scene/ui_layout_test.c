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
#include <string.h>   // strcmp, for barKindLabel's four labels (v1.9.1 INTERFACE)

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

// ── The check-count pin ────────────────────────────────────────────────────────────────
//
// Same mechanism, same wording and the same reason as world/furnace_test.c's and
// world/blockstate_test.c's: a check that stops RUNNING does not fail, it silently ceases to
// exist, and the suite's total drops instead of going red. This project has recorded cases of
// exactly that (see tools/run_host_tests.sh's own notes on a whole 2,555-check stanza going
// dark), so the count is pinned rather than watched by eye.
//
// Measured (2026-09-03): this file had NO pin before the v1.8.15 furnace panel landed, so the
// number was not predicted. A placeholder 703 — the count named in ui_layout.h's own vitals
// sabotage note, i.e. the count before the furnace tests were written — was seeded first, and
// the first run with the furnace tests present reported
// "CHECK COUNT: 4821 check(s) were ADDED - expected 703, ran 5524." and exited 1 with
// "ui_layout self-test: FAIL 1/5524" — the pin the only failure, every furnace check green.
//
// It moved once more in the same session, and honestly rather than by re-seeding: the deposit
// arithmetic was pulled out of scene/ui.c into ui_layout.c (uiMoveStackInto) so it could be
// tested at all, and its seven tests reported
// "CHECK COUNT: 766 check(s) were ADDED - expected 5524, ran 6290", again the only failure.
// 6290 is what it actually counts.
//
// The total is far above the 703 this file ran before v1.8.15 because two of the new tests
// SWEEP rather than tabulate — testFurnBarFillIsBoundedAndMonotonic walks three whole progress
// ranges (1200, 200 and 1 ticks) at three checks a step, and testMoveStackIntoConservesUnits
// walks every legal (source, destination) count pair. That is deliberate: a gauge correct at
// eleven table values and wrong at the twelfth, and a stack move correct at every hand-picked
// count and wrong at one in between, are exactly the defects a table cannot see.
//
// v1.9.1 INTERFACE (2026-09-07): 6290 -> 12008, a delta of +5718, ALL of it added. Predicted
// before the first run by counting the new bodies function by function — 87 strip, 2420 tabs,
// 39 close, 153 list rows, 70 kind tables, 70 barBuildKinds, 85 cell-rect identities, 1245 cell
// round trips, 1123 non-overlap pairs, 272 craft list, 125 out-of-range, 29 detail band — and
// checked against what the binary reported rather than adopted from it.
//
// Nothing was removed. SIX existing checks were RETARGETED in place, because the geometry they
// named is retired rather than wrong: three CRAFT_Y probes in testPointsOutsidePanelReturnNoSlot
// became BAR_STRIP_Y / BAR_DETAIL_Y probes (CRAFT_Y no longer exists; the strip band and the
// detail band are what sits at those y values now), and three HOTBAR_H edge probes in
// testBoundaryPixels became a HOTBAR_H / GRID_Y pair (the row below the hotbar is the strip,
// and the grid starts at GRID_Y 72 rather than at 40). Retargeted, not deleted, so the count
// they contribute is unchanged and this delta is purely the new bar work.
//
// Two of the new tests sweep for the same reason the furnace ones do. testBarTabsTileTheStrip
// walks every one of the 294 pixel columns at every tab count, because the truncating split it
// is meant to catch tiles 294 EXACTLY at one, two and three tabs and only leaves a dead column
// at four — a check at the count this version happens to build would have passed against the
// wrong formula. testBarCellsDoNotOverlap walks every cell pair of every category rather than
// the pairs that look suspicious.
//
// Coverage-hole review (2026-09-07): 12008 -> 12013, a delta of +5, all of it
// testHudStatusBandCatchesASmallDrift. A review pass found that the only check touching
// HUD_STATUS_Y0/HUD_STATUS_BOTTOM was testPipsClearTheStatusRows' `HUD_PIPS_Y0 >=
// HUD_STATUS_BOTTOM` (200 >= 168), a >= with 32 px of slack — it does not go red until
// HUD_STATUS_Y0 drifts past 104, so the v1.9.1 rework's own 6 px move (HUD_TOGGLE_H 32 -> 26)
// would have sailed through it, and so would anything smaller. Five literal pins were added:
// HUD_TOGGLE_H == 26, HUD_STATUS_Y0 == 72, HUD_STATUS_BOTTOM == 168, the worst-case hint
// bottom drawHudFont can actually produce == 165, and the gap HUD_PIPS_Y0 - HUD_STATUS_BOTTOM
// == 32. All five are pinned against hand-written numbers rather than against each other's
// macros, specifically so they cannot be tautological restatements of ui_layout.h's own
// formulas (HUD_STATUS_Y0 IS HUD_TOGGLE_Y + HUD_TOGGLE_H + 6 by definition — comparing it back
// to that sum can never fail). Proved red on a mirror copy at HUD_TOGGLE_H 20 and at 32 (see
// testHudStatusBandCatchesASmallDrift's own comment for the relational check that was tried,
// found insensitive to both directions, and cut instead of kept for show).
#define UI_LAYOUT_TEST_EXPECTED_CHECKS 12013

static void checkCountPin(void)
{
	if (s_checks == UI_LAYOUT_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < UI_LAYOUT_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  UI_LAYOUT_TEST_EXPECTED_CHECKS to go green.\n",
		       UI_LAYOUT_TEST_EXPECTED_CHECKS - s_checks,
		       UI_LAYOUT_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         UI_LAYOUT_TEST_EXPECTED_CHECKS - s_checks,
			         UI_LAYOUT_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set UI_LAYOUT_TEST_EXPECTED_CHECKS in\n"
		       "  source/scene/ui_layout_test.c to %d.\n",
		       s_checks - UI_LAYOUT_TEST_EXPECTED_CHECKS,
		       UI_LAYOUT_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - UI_LAYOUT_TEST_EXPECTED_CHECKS,
			         UI_LAYOUT_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

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

	// RETARGETED for v1.9.1's bar. These three were written against CRAFT_Y, the top of the
	// crafting panel that no longer exists; they made the same claim about the band the bar now
	// gives to the tab strip (BAR_STRIP_Y) and the band it gives to the item detail readout
	// (BAR_DETAIL_Y). hitInventorySlot only ever tests the hotbar and main-grid rects, so a
	// point in either band is "no slot" regardless of overlay_open, even though both are inside
	// the 320x240 screen and would land in barStripRect/barListRowRect if those were what was
	// being tested (they are ui.c's own separate dispatch, not hitInventorySlot's job).
	CHECK(hitInventorySlot(10, BAR_STRIP_Y, true) == -1);
	CHECK(hitInventorySlot(10, BAR_DETAIL_Y, true) == -1);
	CHECK(hitInventorySlot(10, BAR_STRIP_Y, false) == -1);
}

// ── Boundary pixels of the first and last slot on each edge ────────────────────────────

static void testBoundaryPixels(void)
{
	// Hotbar slot 0: left/top edge of the whole panel. (0,0) is the first in-bounds pixel;
	// one pixel further out on either axis falls outside everything.
	CHECK(hitInventorySlot(0, 0, false) == 0);
	CHECK(hitInventorySlot(-1, 0, false) == -1);
	CHECK(hitInventorySlot(0, -1, false) == -1);

	// Hotbar slot 0's bottom edge. RETARGETED for v1.9.1's bar: y = HOTBAR_H used to be the
	// first row of the main grid, and these three said so. The grid now starts at GRID_Y (72)
	// with the tab strip in the band between, so the claim becomes "the grid starts at GRID_Y,
	// and the strip row above it is not a slot" — which is the same boundary, moved, and it
	// goes red if GRID_Y is put back where it was.
	CHECK(hitInventorySlot(0, HOTBAR_H - 1, false) == 0);
	CHECK(hitInventorySlot(0, HOTBAR_H,     true)  == -1);   // the strip band, not a slot
	CHECK(hitInventorySlot(0, GRID_Y,       true)  == INV_HOTBAR_SLOTS + 0);

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

// Field-for-field equality, added with v1.9.1's bar: several of its claims are "this rect IS
// that rect" (the strip is the old furnace close bar; a bar cell is the slot rect that already
// shipped), and those are one check each rather than four only because of this.
static bool sameRect(URect a, URect b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
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

// ── v1.8.15 FURNACE: the furnace panel's rects, hit tests and gauge arithmetic ──────────
//
// What these can check that a screenshot cannot, which is the whole reason the split exists:
// that the three furnace cells resolve to the slot they are drawn as and to nothing else,
// that the RELOCATED main grid on this screen and the inventory overlay's grid cannot both
// claim the same point, that the two gauges are readouts and never swallow a tap meant for a
// slot, and that furnBarFill()'s division is guarded on the exact inputs an idle furnace
// produces. A capture proves the panel looks right in the state that happened to be on screen;
// it cannot prove a furnace holding no fuel does not divide by zero, because that furnace
// draws a perfectly ordinary empty bar right up until it crashes.

// gfx/font.h's FONT_GLYPH_H, restated here as a literal rather than included: that header
// pulls in <citro3d.h> and this binary is the one that must link without it (see this file's
// own header comment). 7 is the value, and if it ever moves this constant is wrong in a way
// no compiler will say — which is exactly why the checks below are written against a floor
// well above it rather than against it.
#define TEST_FONT_GLYPH_H 7

// The smallest a touch target on this panel is allowed to be. Deliberately more than double
// TEST_FONT_GLYPH_H: the task's constraint is "comfortably larger than the font", and a
// target the height of its own label is not comfortable on a stylus panel — the existing
// crafting rows are 18 px and ui_layout.h already flags 13 px as the point where a row stops
// having usable clearance.
#define TEST_MIN_TOUCH_H 16

// ── v1.9.1 rework review gap: HUD_STATUS_Y0/_BOTTOM can drift with nothing to catch it ──
//
// testPipsClearTheStatusRows above only checks HUD_PIPS_Y0 >= HUD_STATUS_BOTTOM (200 >= 168),
// a 32 px margin, and it is a >= not an ==. It only goes red once HUD_STATUS_Y0 moves past
// 104 (HUD_STATUS_BOTTOM past 200) — nothing here would have caught the v1.9.1 rework's own
// 6 px move (HUD_TOGGLE_H 32 -> 26, HUD_STATUS_Y0 78 -> 72, HUD_STATUS_BOTTOM 174 -> 168), let
// alone anything smaller. Measured by a review pass, re-verified here against ui_layout.h.
//
// The checks below are literal pins, not restatements of the macros' own definitions.
// HUD_STATUS_Y0 IS (HUD_TOGGLE_Y + HUD_TOGGLE_H + 6) by definition, so
// `CHECK(HUD_STATUS_Y0 == HUD_TOGGLE_Y + HUD_TOGGLE_H + 6)` can never fail no matter what
// HUD_TOGGLE_H is — the preprocessor makes both sides the same expression before the compiler
// ever sees a number. Pinning against a hand-written NUMBER instead (72, 168, 165, 32) is what
// actually exercises the arithmetic: HUD_TOGGLE_H moves the computed value, not the literal it
// is compared to.
//
// A relational check of the shape "HUD_STATUS_BOTTOM clears the worst-case hint bottom" was
// tried and dropped: HUD_STATUS_BOTTOM is HUD_STATUS_Y0 + 96 and the worst-case hint bottom
// (see below) is HUD_STATUS_Y0 + 93, so the two carry the SAME HUD_STATUS_Y0 term and their
// 3 px margin never changes sign no matter what HUD_TOGGLE_H is — it stayed green under both
// the +6 and -6 mirror perturbations this file was checked against, so per this task's own
// rule it was cut rather than kept for show. The two literal pins below (worst-case bottom ==
// 165, HUD_STATUS_BOTTOM == 168) between them say everything that relational check would have,
// and unlike it, they can actually fail.
static void testHudStatusBandCatchesASmallDrift(void)
{
	// The v1.9.1 values themselves, pinned. Any change to HUD_TOGGLE_H — including one made in
	// lockstep with a change elsewhere that happens to leave BAR_STRIP_H tracking it, the way
	// the real v1.9.1 edit did — moves HUD_STATUS_Y0 by the same number of pixels and trips
	// this the instant it lands.
	CHECK(HUD_TOGGLE_H == 26);
	CHECK(HUD_STATUS_Y0 == 72);
	CHECK(HUD_STATUS_BOTTOM == 168);

	// The worst case drawHudFont can actually produce: HUD_STATUS_MAX_ROWS (7) rows at
	// HUD_STATUS_STEP (12) each, then the 2 px gap ui.c's `y += 2` adds after the last row,
	// then one row of glyphs at TEST_FONT_GLYPH_H (7) for the hint line. Pinned to the literal
	// 165 that ui_layout.h's own HUD_STATUS_BOTTOM comment states, so this fails on a
	// row-count/step drift too, not only on a HUD_STATUS_Y0 drift.
	CHECK(HUD_STATUS_Y0 + HUD_STATUS_MAX_ROWS * HUD_STATUS_STEP + 2 + TEST_FONT_GLYPH_H == 165);

	// The gap between the status floor and the vitals strip below it, pinned rather than
	// merely bounded below — this is what makes a drift in EITHER direction fail instead of
	// only a drift large enough to close the 32 px margin entirely.
	CHECK(HUD_PIPS_Y0 - HUD_STATUS_BOTTOM == 32);
}

// The centre of each furnace cell resolves to that cell, and furnSlotRect() agrees with the
// three named accessors for every index.
static void testFurnaceSlotsRoundTrip(void)
{
	static const int all[] = { FURN_HIT_INPUT, FURN_HIT_FUEL, FURN_HIT_OUTPUT };

	for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
		const int which = all[k];
		const URect r = furnSlotRect(which);

		int cx, cy;
		centerOf(r, &cx, &cy);
		CHECK(hitFurnaceSlot(cx, cy) == which);

		// Corners too, not just the centre: the first in-bounds pixel and the last.
		CHECK(hitFurnaceSlot(r.x, r.y) == which);
		CHECK(hitFurnaceSlot(r.x + r.w - 1, r.y + r.h - 1) == which);

		// One pixel outside each edge is NOT this slot. It may be another slot (the cells are
		// 16 px apart, so it is not) or nothing; what it must never be is this one.
		CHECK(hitFurnaceSlot(r.x - 1, cy) != which);
		CHECK(hitFurnaceSlot(r.x + r.w, cy) != which);
		CHECK(hitFurnaceSlot(cx, r.y - 1) != which);
		CHECK(hitFurnaceSlot(cx, r.y + r.h) != which);
	}

	// furnSlotRect() is the same rect as the named accessor, field for field. This is what
	// stops the indexed form and the named form drifting apart — ui.c draws through the
	// indexed one and hit-tests through the named ones.
	const URect in_named = furnInputRect(),  in_idx  = furnSlotRect(FURN_HIT_INPUT);
	const URect fu_named = furnFuelRect(),   fu_idx  = furnSlotRect(FURN_HIT_FUEL);
	const URect ou_named = furnOutputRect(), ou_idx  = furnSlotRect(FURN_HIT_OUTPUT);

	CHECK(in_named.x == in_idx.x && in_named.y == in_idx.y);
	CHECK(in_named.w == in_idx.w && in_named.h == in_idx.h);
	CHECK(fu_named.x == fu_idx.x && fu_named.y == fu_idx.y);
	CHECK(fu_named.w == fu_idx.w && fu_named.h == fu_idx.h);
	CHECK(ou_named.x == ou_idx.x && ou_named.y == ou_idx.y);
	CHECK(ou_named.w == ou_idx.w && ou_named.h == ou_idx.h);
}

// An index outside the three is a zero rect that no point can be inside — the one answer that
// cannot be mistaken for a real slot.
static void testFurnaceSlotRectRejectsOutOfRangeIndex(void)
{
	static const int bad[] = { -1, -100, FURN_HIT_NONE, 3, 4, 99, 1000 };

	for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		const URect r = furnSlotRect(bad[k]);
		CHECK(r.w == 0);
		CHECK(r.h == 0);

		// And nothing lands in it, including its own origin.
		CHECK(!ptInRect(r, r.x, r.y));
		CHECK(!ptInRect(r, 0, 0));
	}
}

// The three cells do not overlap each other, do not overlap the hotbar, and do not overlap the
// relocated main grid. Three separate claims, because the panel is laid out by hand from
// literal x/y constants and a typo in any one of them is invisible in a screenshot of a
// furnace that happens to be empty.
static void testFurnaceSlotsDoNotCollide(void)
{
	static const int all[] = { FURN_HIT_INPUT, FURN_HIT_FUEL, FURN_HIT_OUTPUT };

	for (size_t a = 0; a < sizeof(all) / sizeof(all[0]); a++) {
		const URect ra = furnSlotRect(all[a]);

		for (size_t b = a + 1; b < sizeof(all) / sizeof(all[0]); b++)
			CHECK(!rectsOverlap(ra, furnSlotRect(all[b])));

		for (int s = 0; s < INV_HOTBAR_SLOTS; s++)
			CHECK(!rectsOverlap(ra, hotbarSlotRect(s)));

		for (int i = 0; i < INV_MAIN_SLOTS; i++)
			CHECK(!rectsOverlap(ra, furnGridSlotRect(i)));

		CHECK(!rectsOverlap(ra, furnCloseRect()));
		CHECK(!rectsOverlap(ra, furnBurnRect()));
		CHECK(!rectsOverlap(ra, furnArrowRect()));
	}
}

// The bag, as drawn on the furnace screen: hotbar at its usual rects, main grid at
// FURN_GRID_Y.
static void testFurnaceInvSlotsRoundTrip(void)
{
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++) {
		int cx, cy;
		centerOf(hotbarSlotRect(i), &cx, &cy);
		CHECK(hitFurnaceInvSlot(cx, cy) == i);
	}

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const URect r = furnGridSlotRect(i);
		int cx, cy;
		centerOf(r, &cx, &cy);

		CHECK(hitFurnaceInvSlot(cx, cy) == INV_HOTBAR_SLOTS + i);
		CHECK(hitFurnaceInvSlot(r.x, r.y) == INV_HOTBAR_SLOTS + i);
		CHECK(hitFurnaceInvSlot(r.x + r.w - 1, r.y + r.h - 1) == INV_HOTBAR_SLOTS + i);
	}

	// The grid tiles the bottom of the panel exactly: 2 rows of 40 starting at 160 ends on
	// SCR_H, with no dead strip under it and nothing clipped off it.
	const URect last = furnGridSlotRect(INV_MAIN_SLOTS - 1);
	CHECK(last.y + last.h == SCR_H);
	CHECK(furnGridSlotRect(0).y == FURN_GRID_Y);

	// Points off the panel resolve to nothing here, same contract hitInventorySlot has.
	CHECK(hitFurnaceInvSlot(-1, 0) == -1);
	CHECK(hitFurnaceInvSlot(0, -1) == -1);
	CHECK(hitFurnaceInvSlot(SCR_W, 0) == -1);
	CHECK(hitFurnaceInvSlot(0, SCR_H) == -1);
	CHECK(hitFurnaceInvSlot(SCR_W + 50, SCR_H + 50) == -1);
}

// The two screens' grids are in different places, and neither screen's hit test may resolve a
// point belonging to the other's grid. This is the check that goes red if somebody "unifies"
// the two grids back into one function and forgets that only one of them moved.
static void testTheTwoGridsAreDisjoint(void)
{
	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const URect furn = furnGridSlotRect(i);
		const URect ovl  = gridSlotRect(i);

		// Same column, different band.
		CHECK(furn.x == ovl.x);
		CHECK(furn.y != ovl.y);
		CHECK(!rectsOverlap(furn, ovl));

		int fcx, fcy, ocx, ocy;
		centerOf(furn, &fcx, &fcy);
		centerOf(ovl,  &ocx, &ocy);

		// A point in the furnace screen's grid is not an inventory-overlay slot at all — with
		// the overlay open OR closed. On the overlay screen that band is the crafting panel.
		CHECK(hitInventorySlot(fcx, fcy, true)  == -1);
		CHECK(hitInventorySlot(fcx, fcy, false) == -1);

		// And a point in the overlay's grid is not a furnace-screen bag slot: on the furnace
		// screen that band is the close bar, the three cells and the two gauges.
		CHECK(hitFurnaceInvSlot(ocx, ocy) == -1);
	}
}

// The burn bar and the cook arrow are READOUTS. A tap on either must fall through to nothing,
// so a stylus that lands 2 px low of the fuel cell does not silently do something else.
static void testFurnaceGaugesAreNotTouchTargets(void)
{
	const URect gauges[2] = { furnBurnRect(), furnArrowRect() };

	for (int g = 0; g < 2; g++) {
		const URect r = gauges[g];

		int cx, cy;
		centerOf(r, &cx, &cy);

		CHECK(hitFurnaceSlot(cx, cy) == FURN_HIT_NONE);
		CHECK(hitFurnaceInvSlot(cx, cy) == -1);
		CHECK(!ptInRect(furnCloseRect(), cx, cy));

		// Corners as well as the centre.
		CHECK(hitFurnaceSlot(r.x, r.y) == FURN_HIT_NONE);
		CHECK(hitFurnaceSlot(r.x + r.w - 1, r.y + r.h - 1) == FURN_HIT_NONE);
		CHECK(hitFurnaceInvSlot(r.x, r.y) == -1);
		CHECK(hitFurnaceInvSlot(r.x + r.w - 1, r.y + r.h - 1) == -1);

		// On the panel, and clear of both the hotbar and the bag grid.
		CHECK(r.x >= 0 && r.x + r.w <= SCR_W);
		CHECK(r.y >= 0 && r.y + r.h <= SCR_H);
		for (int s = 0; s < INV_HOTBAR_SLOTS; s++)
			CHECK(!rectsOverlap(r, hotbarSlotRect(s)));
		for (int i = 0; i < INV_MAIN_SLOTS; i++)
			CHECK(!rectsOverlap(r, furnGridSlotRect(i)));
	}

	// The two gauges do not overlap each other either — they are 20 px apart vertically and
	// 100 px apart horizontally, but both of those come from hand-written constants.
	CHECK(!rectsOverlap(furnBurnRect(), furnArrowRect()));
}

// The whole panel fits the 320x240 budget, in the stacking order ui_layout.h's comment claims,
// and every touch target on it clears the font by a comfortable margin.
static void testFurnacePanelFitsAndIsTappable(void)
{
	const URect close = furnCloseRect();

	// Close bar sits directly under the hotbar and above the slot row.
	CHECK(close.y == HOTBAR_Y + HOTBAR_H);
	CHECK(close.y + close.h <= FURN_ROW_Y);
	CHECK(close.x == 0 && close.w == SCR_W);
	CHECK(!rectsOverlap(close, hotbarSlotRect(0)));
	CHECK(!rectsOverlap(close, hotbarSlotRect(INV_HOTBAR_SLOTS - 1)));

	// Every touch target on this screen: the close bar, the three cells, the hotbar and the
	// relocated grid. All on the panel, all comfortably taller than the font.
	CHECK(close.h >= TEST_MIN_TOUCH_H);
	CHECK(close.h > TEST_FONT_GLYPH_H);

	for (int which = FURN_HIT_INPUT; which <= FURN_HIT_OUTPUT; which++) {
		const URect r = furnSlotRect(which);
		CHECK(r.x >= 0);
		CHECK(r.x + r.w <= SCR_W);
		CHECK(r.y >= 0);
		CHECK(r.y + r.h <= SCR_H);
		CHECK(r.h >= TEST_MIN_TOUCH_H);
		CHECK(r.w >= TEST_MIN_TOUCH_H);
		CHECK(r.y == FURN_ROW_Y);
	}

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const URect r = furnGridSlotRect(i);
		CHECK(r.x >= 0);
		CHECK(r.x + r.w <= SCR_W);
		CHECK(r.y >= FURN_GRID_Y);
		CHECK(r.y + r.h <= SCR_H);
		CHECK(r.h >= TEST_MIN_TOUCH_H);
	}

	// The hint text's LAST row must clear the grid. This is the constraint ui_layout.h's
	// FURN_HINT_* comment states, checked rather than reread: it fails if a third hint row is
	// added, if the step grows, or if the grid moves up.
	const int hint_bottom = FURN_HINT_Y + (FURN_HINT_ROWS - 1) * FURN_HINT_STEP
	                        + TEST_FONT_GLYPH_H;
	CHECK(hint_bottom <= FURN_GRID_Y);
	CHECK(FURN_HINT_Y >= FURN_BURN_Y + FURN_BURN_H);

	// The panel's bands are in order, top to bottom, with no band starting above the one
	// before it ends.
	CHECK(FURN_CLOSE_Y >= HOTBAR_Y + HOTBAR_H);
	CHECK(FURN_ROW_Y >= FURN_CLOSE_Y + FURN_CLOSE_H);
	CHECK(FURN_BURN_Y >= FURN_ROW_Y + FURN_SLOT_PX);
	CHECK(FURN_GRID_Y >= FURN_BURN_Y + FURN_BURN_H);
	CHECK(FURN_GRID_Y + INV_MAIN_ROWS * SLOT_PX == SCR_H);
}

// ── furnBarFill: the one piece of this panel that can be arithmetically WRONG ────────────

// A table of expected pixel widths, written out by hand rather than derived, so this is not
// furnBarFill checked against a second copy of furnBarFill.
static void testFurnBarFillScale(void)
{
	static const struct { int total, num, den, want; } cases[] = {
		{ 80,   0, 200,  0 },   // idle: no progress
		{ 80, 200, 200, 80 },   // complete
		{ 80, 100, 200, 40 },   // exactly half
		{ 80,  50, 200, 20 },
		{ 80, 150, 200, 60 },
		{ 80,   1, 200,  0 },   // floors to nothing — one tick in is not one pixel
		{ 80, 199, 200, 79 },   // one tick short of full is NOT full
		{ 40, 600, 1200, 20 },  // the burn bar at half a log
		{ 40, 300, 1200, 10 },  // a full plank on the log scale: a quarter bar. See ui.c's
		                        // note on why the burn gauge is absolute, not relative.
		{ 40, 1200, 1200, 40 },
		{ 40,    0, 1200,  0 },
	};

	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
		CHECK(furnBarFill(cases[k].total, cases[k].num, cases[k].den) == cases[k].want);
}

// The guards. den == 0 is the one that matters most: it is the state of EVERY furnace that has
// no fuel item and no valid input, which is what a freshly placed furnace looks like, and an
// unguarded divide there is a crash on the most ordinary screen in the feature.
static void testFurnBarFillGuardsTheDivision(void)
{
	static const int totals[] = { 0, 1, 12, 40, 80, 320 };

	for (size_t k = 0; k < sizeof(totals) / sizeof(totals[0]); k++) {
		const int t = totals[k];

		CHECK(furnBarFill(t, 0, 0) == 0);        // idle furnace: no numerator, no denominator
		CHECK(furnBarFill(t, 5, 0) == 0);        // progress with no scale to read it against
		CHECK(furnBarFill(t, 5, -1) == 0);
		CHECK(furnBarFill(t, 5, -1000) == 0);
		CHECK(furnBarFill(t, -1, 200) == 0);     // negative progress reads empty, never full
		CHECK(furnBarFill(t, -1000, 200) == 0);
		CHECK(furnBarFill(t, 0, 200) == 0);
	}

	// A non-positive total is a bar with no pixels — never a negative width handed to
	// spriteRect, which would draw a rect extending backwards from its own origin.
	CHECK(furnBarFill(0, 100, 200) == 0);
	CHECK(furnBarFill(-1, 100, 200) == 0);
	CHECK(furnBarFill(-80, 200, 200) == 0);
	CHECK(furnBarFill(0, 0, 0) == 0);
}

// Two properties over the whole real input range, which is what catches an edit that keeps
// every table row above correct and still breaks in between: the result never leaves
// [0, total], and it never goes DOWN as progress goes up.
//
// Swept at the two widths ui.c actually draws (FURN_BURN_W and FURN_ARROW_W) against the two
// denominators it actually passes (a log's burn, and a recipe's 200-tick cook), plus values
// past the denominator so the saturating end is covered too.
static void testFurnBarFillIsBoundedAndMonotonic(void)
{
	static const struct { int total, den; } sweeps[] = {
		{ FURN_BURN_W,  1200 },
		{ FURN_ARROW_W,  200 },
		{ FURN_ARROW_W,    1 },   // a one-tick recipe: every non-zero num saturates
	};

	for (size_t s = 0; s < sizeof(sweeps) / sizeof(sweeps[0]); s++) {
		const int total = sweeps[s].total;
		const int den   = sweeps[s].den;

		int prev = 0;
		for (int num = 0; num <= den + 5; num++) {
			const int got = furnBarFill(total, num, den);

			CHECK(got >= 0);
			CHECK(got <= total);
			CHECK(got >= prev);
			prev = got;
		}

		// The far ends, stated separately so a sweep that silently ran zero iterations could
		// not pass this function.
		CHECK(furnBarFill(total, 0, den) == 0);
		CHECK(furnBarFill(total, den, den) == total);
		CHECK(furnBarFill(total, den + 10000, den) == total);
	}
}

// ── uiMoveStackInto: the deposit arithmetic ─────────────────────────────────────────────
//
// The reason this function is in ui_layout.c at all (see its header comment): an off-by-one in
// the room calculation duplicates or destroys items, and neither shows up in a screenshot of
// a furnace. Every rule its header states gets a check here, and the two slots are inspected
// AFTER every call — not just the return value — because a function that returns the right
// number and writes the wrong fields is the failure this is actually guarding against.
//
// Two arbitrary distinct item ids, written as literals rather than as BLOCK_* names: this
// binary links neither world/block.c nor world/registry.c (see tools/run_host_tests.sh's
// stanza), and uiMoveStackInto treats an ItemId as an opaque integer it only ever compares
// for equality. Using a real block id here would suggest it asks the registry something. It
// does not.
#define TEST_ITEM_A ((ItemId)17)
#define TEST_ITEM_B ((ItemId)29)

static void testMoveStackIntoEmptyDestination(void)
{
	InvSlot  src   = { TEST_ITEM_A, 5 };
	ItemId   dst_i = ITEM_NONE;
	uint8_t  dst_c = 0;

	CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 5);
	CHECK(dst_i == TEST_ITEM_A);
	CHECK(dst_c == 5);
	CHECK(src.item == ITEM_NONE);   // emptied source normalises, never a stale id
	CHECK(src.count == 0);
}

static void testMoveStackIntoMergesWithSameItem(void)
{
	InvSlot  src   = { TEST_ITEM_A, 5 };
	ItemId   dst_i = TEST_ITEM_A;
	uint8_t  dst_c = 3;

	CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 5);
	CHECK(dst_i == TEST_ITEM_A);
	CHECK(dst_c == 8);
	CHECK(src.item == ITEM_NONE);
	CHECK(src.count == 0);
}

// The partial move: the destination tops up to INV_STACK_MAX and the remainder stays exactly
// where it was. Nothing is created and nothing is destroyed — the check that goes red on an
// off-by-one in either direction is the conservation one at the end.
static void testMoveStackIntoPartialLeavesTheRemainder(void)
{
	InvSlot  src   = { TEST_ITEM_A, 10 };
	ItemId   dst_i = TEST_ITEM_A;
	uint8_t  dst_c = INV_STACK_MAX - 4;

	CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 4);
	CHECK(dst_i == TEST_ITEM_A);
	CHECK(dst_c == INV_STACK_MAX);
	CHECK(src.item == TEST_ITEM_A);   // NOT emptied — 6 are still there
	CHECK(src.count == 6);
	CHECK((int)dst_c + (int)src.count == INV_STACK_MAX - 4 + 10);   // conservation
}

// Exactly filling the destination is the boundary between the merge case and the partial case,
// and it is the one an off-by-one lands on.
static void testMoveStackIntoExactFit(void)
{
	InvSlot  src   = { TEST_ITEM_A, 4 };
	ItemId   dst_i = TEST_ITEM_A;
	uint8_t  dst_c = INV_STACK_MAX - 4;

	CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 4);
	CHECK(dst_c == INV_STACK_MAX);
	CHECK(src.item == ITEM_NONE);
	CHECK(src.count == 0);

	// One more unit than fits: the destination still lands exactly on the cap, never past it.
	InvSlot  src2   = { TEST_ITEM_A, 5 };
	ItemId   dst_i2 = TEST_ITEM_A;
	uint8_t  dst_c2 = INV_STACK_MAX - 4;

	CHECK(uiMoveStackInto(&src2, &dst_i2, &dst_c2) == 4);
	CHECK(dst_c2 == INV_STACK_MAX);
	CHECK(src2.item == TEST_ITEM_A);
	CHECK(src2.count == 1);
}

// Every refusal, and the thing that makes a refusal a refusal: BOTH sides come back unchanged.
static void testMoveStackIntoRefusals(void)
{
	// A full destination.
	{
		InvSlot  src   = { TEST_ITEM_A, 5 };
		ItemId   dst_i = TEST_ITEM_A;
		uint8_t  dst_c = INV_STACK_MAX;

		CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 0);
		CHECK(dst_c == INV_STACK_MAX);
		CHECK(src.item == TEST_ITEM_A);
		CHECK(src.count == 5);
	}

	// A destination holding a DIFFERENT item: refused outright, never swapped.
	{
		InvSlot  src   = { TEST_ITEM_A, 5 };
		ItemId   dst_i = TEST_ITEM_B;
		uint8_t  dst_c = 2;

		CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 0);
		CHECK(dst_i == TEST_ITEM_B);
		CHECK(dst_c == 2);
		CHECK(src.item == TEST_ITEM_A);
		CHECK(src.count == 5);
	}

	// An empty source, both spellings of empty.
	{
		InvSlot  src   = { ITEM_NONE, 0 };
		ItemId   dst_i = ITEM_NONE;
		uint8_t  dst_c = 0;

		CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 0);
		CHECK(dst_i == ITEM_NONE);
		CHECK(dst_c == 0);
	}
	{
		// A count of 0 with a live id is a state inventory.h says never happens; refusing it
		// is what stops it becoming a free item if it ever does.
		InvSlot  src   = { TEST_ITEM_A, 0 };
		ItemId   dst_i = ITEM_NONE;
		uint8_t  dst_c = 0;

		CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 0);
		CHECK(dst_i == ITEM_NONE);
		CHECK(dst_c == 0);
		CHECK(src.item == TEST_ITEM_A);
		CHECK(src.count == 0);
	}
}

// The corrupt-payload case the header calls out: a destination reading ITEM_NONE with a
// non-zero count holds NOTHING, so the stale number is overwritten rather than added to. Added
// to, a stale 250 plus 10 would wrap a uint8_t to 4 and quietly delete 256 items.
static void testMoveStackIntoIgnoresAStaleCountOnAnEmptyDestination(void)
{
	InvSlot  src   = { TEST_ITEM_A, 10 };
	ItemId   dst_i = ITEM_NONE;
	uint8_t  dst_c = 250;

	CHECK(uiMoveStackInto(&src, &dst_i, &dst_c) == 10);
	CHECK(dst_i == TEST_ITEM_A);
	CHECK(dst_c == 10);        // overwritten, not 250 + 10
	CHECK(src.item == ITEM_NONE);
	CHECK(src.count == 0);
}

// A sweep over every (source count, destination count) pair a furnace slot can legally be in.
// Three properties hold for all of them, and it is the third that a table of hand-picked cases
// cannot establish: nothing is ever created and nothing is ever destroyed.
static void testMoveStackIntoConservesUnits(void)
{
	for (int have = 1; have <= INV_STACK_MAX; have += 7) {
		for (int held = 0; held <= INV_STACK_MAX; held += 9) {
			InvSlot  src   = { TEST_ITEM_A, (uint8_t)have };
			ItemId   dst_i = (held == 0) ? ITEM_NONE : TEST_ITEM_A;
			uint8_t  dst_c = (uint8_t)held;

			const uint8_t moved = uiMoveStackInto(&src, &dst_i, &dst_c);

			CHECK((int)moved <= have);
			CHECK((int)dst_c <= INV_STACK_MAX);
			CHECK((int)dst_c + (int)src.count == have + held);
			CHECK((src.count == 0) == (src.item == ITEM_NONE));
		}
	}
}

// ── v1.9.1 INTERFACE: the bar's strip, tabs, list rows, kind tables and cell addressing ──
//
// What these can check that a screenshot cannot, which is the same argument the furnace block
// above makes for itself: that the tabs tile their 294 px with no dead column at EVERY tab
// count rather than at the count that happened to be on screen; that the close tab is where a
// stylus expects it to the pixel; that a (row, col) the d-pad can reach is a rect the stylus
// can hit and vice versa, for every cell of every category; and that the tab table can never
// produce two containers or put one anywhere but index 2. A capture proves the bar looked right
// with one chest open; it cannot prove the furnace's three-wide row addresses left-to-right,
// because a swapped pair of columns draws an identical picture.
//
// The cell round trip is the load-bearing one, and it is only load-bearing because
// barCellFromPoint() does not walk barCellRect() — see that function's comment in
// ui_layout.c. Rect side and hit side are two independent paths, so a column swap in one
// shows up as a disagreement rather than as a shared mistake both sides make.

// Vertically inside the strip, wherever the strip is. 53 at BAR_STRIP_Y 40 / _H 26, which is
// the y the interface blueprint's own close-tab arm names.
#define TEST_STRIP_CY (BAR_STRIP_Y + BAR_STRIP_H / 2)

// Tabs are "at least 98x26" in the blueprint's touch-target budget — 98 is the narrowest a tab
// can get at the three categories this version can produce. Written out rather than derived
// from BAR_TABS_W / 3, so it does not follow the code it is checking.
#define TEST_MIN_TAB_W 98

static const BarKind kTestKinds[4] = {
	BAR_KIND_INVENTORY, BAR_KIND_CRAFT, BAR_KIND_CHEST, BAR_KIND_FURNACE
};

// The three kinds whose content is a grid of slots. CRAFT is the odd one out everywhere — it
// is a list, its rows come from its caller, and its cells are the only ones not backed by a
// rect that shipped before the bar existed.
static const BarKind kTestGridKinds[3] = {
	BAR_KIND_INVENTORY, BAR_KIND_CHEST, BAR_KIND_FURNACE
};

// The strip is ONE band, it is the band the three old close bars used, and nothing else on any
// tab reaches into it. The last claim is the one that matters: the bar draws the strip on every
// screen, so a content row that overlapped it would be covered on one tab and visible on the
// next, which is a bug that only shows up on the tab nobody screenshotted.
static void testBarStripIsOneBand(void)
{
	const URect strip = barStripRect();

	CHECK(strip.x == 0);
	CHECK(strip.y == BAR_STRIP_Y);
	CHECK(strip.w == SCR_W);
	CHECK(strip.h == BAR_STRIP_H);
	CHECK(strip.h >= TEST_MIN_TOUCH_H);

	// Directly under the hotbar, and clear of every content band under it.
	CHECK(strip.y == HOTBAR_Y + HOTBAR_H);
	CHECK(strip.y + strip.h <= GRID_Y);
	CHECK(strip.y + strip.h <= CHEST_ROW_Y);
	CHECK(strip.y + strip.h <= FURN_ROW_Y);
	CHECK(strip.y + strip.h <= BAR_LIST_Y0);

	// It IS the furnace and chest close bars, and it IS the HUD's toggle strip — the same
	// rect, not merely the same size. FURN_CLOSE_* and CHEST_CLOSE_* are aliases of
	// BAR_STRIP_* so those two cannot fail; HUD_TOGGLE_* are written as their own literals
	// precisely so the third one can.
	CHECK(sameRect(strip, furnCloseRect()));
	CHECK(sameRect(strip, chestCloseRect()));
	CHECK(sameRect(strip, hudToggleRect()));

	for (int s = 0; s < INV_HOTBAR_SLOTS; s++)
		CHECK(!rectsOverlap(strip, hotbarSlotRect(s)));

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		CHECK(!rectsOverlap(strip, gridSlotRect(i)));
		CHECK(!rectsOverlap(strip, furnGridSlotRect(i)));
		CHECK(!rectsOverlap(strip, chestGridSlotRect(i)));
	}

	for (int c = 0; c < CHEST_SLOTS; c++)
		CHECK(!rectsOverlap(strip, chestSlotRect(c)));

	for (int w = FURN_HIT_INPUT; w <= FURN_HIT_OUTPUT; w++)
		CHECK(!rectsOverlap(strip, furnSlotRect(w)));

	for (int v = 0; v < BAR_LIST_VISIBLE; v++)
		CHECK(!rectsOverlap(strip, barListRowRect(v)));
}

// The tabs tile [0, BAR_TABS_W) exactly at every count from 1 to BAR_MAX_TABS, and every pixel
// column of that span belongs to exactly one of them.
//
// The sweep is not decoration. The truncating split (w = BAR_TABS_W / count) tiles 294 exactly
// at counts 1, 2 and 3 — 294 is 2 x 147 and 3 x 98 — and leaves two dead columns only at
// count 4. Checking the count this version happens to produce would therefore have passed
// against the wrong formula; walking every column at every count is what makes it fail.
static void testBarTabsTileTheStrip(void)
{
	for (int count = 1; count <= BAR_MAX_TABS; count++) {
		int edge = 0;

		for (int i = 0; i < count; i++) {
			const URect r = barTabRect(i, count);

			CHECK(r.x == edge);      // no gap and no overlap with the tab before it
			CHECK(r.w > 0);          // and no tab of zero width, which draws nothing
			CHECK(r.y == BAR_STRIP_Y);
			CHECK(r.h == BAR_STRIP_H);
			CHECK(!rectsOverlap(r, barCloseRect()));

			edge = r.x + r.w;
		}

		CHECK(edge == BAR_TABS_W);   // the tabs tile [0, 294) exactly
	}

	for (int count = 1; count <= BAR_MAX_TABS; count++) {
		for (int x = 0; x < BAR_TABS_W; x++) {
			int owner = BAR_HIT_NONE;
			for (int i = 0; i < count; i++)
				if (ptInRect(barTabRect(i, count), x, TEST_STRIP_CY)) { owner = i; break; }

			CHECK(owner != BAR_HIT_NONE);
			CHECK(hitBarStrip(x, TEST_STRIP_CY, count) == owner);
		}
	}

	// An index outside the count, and a count that is not a count, are zero rects.
	for (int count = 1; count <= BAR_MAX_TABS; count++) {
		const URect over = barTabRect(count, count);
		const URect neg  = barTabRect(-1, count);
		CHECK(over.w == 0 && over.h == 0);
		CHECK(neg.w == 0 && neg.h == 0);
	}

	static const int bad_counts[] = { 0, -1, -100 };
	for (size_t k = 0; k < sizeof(bad_counts) / sizeof(bad_counts[0]); k++) {
		const URect r = barTabRect(0, bad_counts[k]);
		CHECK(r.w == 0);
		CHECK(r.h == 0);
	}
}

// The close tab: the one control in the same place on every screen, and the stylus door out of
// the bar. Its x is checked against BAR_TABS_W and against SCR_W from both ends, because an
// off-by-one here either steals the last tab's final pixel column or leaves a one-pixel gutter
// at the right edge of the panel that closes nothing.
static void testBarCloseTab(void)
{
	const URect c = barCloseRect();

	CHECK(c.x == BAR_TABS_W);
	CHECK(c.y == BAR_STRIP_Y);
	CHECK(c.w == BAR_CLOSE_W);
	CHECK(c.h == BAR_STRIP_H);
	CHECK(c.x + c.w == SCR_W);
	CHECK(c.w >= TEST_MIN_TOUCH_H);
	CHECK(c.h >= TEST_MIN_TOUCH_H);

	for (int count = 1; count <= BAR_MAX_TABS; count++) {
		CHECK(hitBarStrip(c.x, TEST_STRIP_CY, count) == BAR_HIT_CLOSE);
		CHECK(hitBarStrip(c.x + c.w - 1, TEST_STRIP_CY, count) == BAR_HIT_CLOSE);
		CHECK(hitBarStrip(SCR_W, TEST_STRIP_CY, count) == BAR_HIT_NONE);
		CHECK(hitBarStrip(c.x, BAR_STRIP_Y - 1, count) == BAR_HIT_NONE);
		CHECK(hitBarStrip(c.x, BAR_STRIP_Y + BAR_STRIP_H, count) == BAR_HIT_NONE);

		// The two points written as literals rather than derived from the rect under test, so
		// they cannot move with it: the centre of the close tab, and the last pixel column of
		// the tab strip immediately left of it.
		CHECK(hitBarStrip(307, 53, count) == BAR_HIT_CLOSE);
		CHECK(hitBarStrip(293, 53, count) != BAR_HIT_CLOSE);
		CHECK(hitBarStrip(293, 53, count) == count - 1);
	}
}

// The list rows: seven of them between the content band and the bottom of the screen, 22 px
// tall on a 24 px pitch, none of them touching the strip or the hotbar.
//
// BAR_LIST_Y0 is pinned equal to the three other content-band tops rather than only checked to
// be below the strip. Below the strip is the weak claim — 70 would satisfy it — and the design
// claim is that every tab's content starts in the SAME band, so that switching tabs does not
// move the first row two pixels.
static void testBarListRows(void)
{
	CHECK(BAR_LIST_VISIBLE == 7);
	CHECK(BAR_LIST_Y0 >= BAR_STRIP_Y + BAR_STRIP_H);
	CHECK(BAR_LIST_Y0 == GRID_Y);
	CHECK(BAR_LIST_Y0 == CHEST_ROW_Y);
	CHECK(BAR_LIST_Y0 == FURN_ROW_Y);
	CHECK(BAR_LIST_Y0 + BAR_LIST_VISIBLE * BAR_LIST_ROW_H <= SCR_H);

	// Stated across the two kinds as well as across the constants: the first recipe row and the
	// first bag row start at the same y.
	CHECK(barListRowRect(0).y == barCellRect(BAR_KIND_INVENTORY, 1, 0, 0).y);

	for (int i = 0; i < BAR_LIST_VISIBLE; i++) {
		const URect r = barListRowRect(i);

		CHECK(r.x == 4);
		CHECK(r.w == SCR_W - 8);
		CHECK(r.h == BAR_LIST_ROW_H - 2);
		CHECK(r.y == BAR_LIST_Y0 + i * BAR_LIST_ROW_H);
		CHECK(r.y >= BAR_LIST_Y0);
		CHECK(r.y + r.h <= SCR_H);
		CHECK(r.h >= TEST_MIN_TOUCH_H);
		CHECK(!rectsOverlap(r, barStripRect()));
		CHECK(!rectsOverlap(r, barCloseRect()));

		for (int s = 0; s < INV_HOTBAR_SLOTS; s++)
			CHECK(!rectsOverlap(r, hotbarSlotRect(s)));

		if (i > 0) {
			const URect p = barListRowRect(i - 1);
			// Two claims, the same pair the pip strip's own gap check makes: that the
			// arithmetic agrees with the 24/22 pitch, and that adjacent rows are visibly
			// separated at all rather than fused into one 168 px block.
			CHECK(r.y - (p.y + p.h) == 2);
			CHECK(!rectsOverlap(p, r));
		}
	}

	// A row outside the window is a zero rect no point can land in — the same answer
	// furnSlotRect() gives an index outside its three.
	static const int bad[] = { -1, -100, BAR_LIST_VISIBLE, BAR_LIST_VISIBLE + 5, 1000 };
	for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		const URect r = barListRowRect(bad[k]);
		CHECK(r.w == 0);
		CHECK(r.h == 0);
		CHECK(!ptInRect(r, r.x, r.y));
	}
}

// The three per-kind tables the navigation model reads: label, row count, per-row column count,
// and whether left/right belongs to the row or to the tabs. Written out by hand here rather
// than derived, so this is not the tables checked against a second copy of the tables.
static void testBarKindTables(void)
{
	static const struct { BarKind k; const char* label; } labels[] = {
		{ BAR_KIND_INVENTORY, "INVENTORY" },
		{ BAR_KIND_CRAFT,     "CRAFT"     },
		{ BAR_KIND_CHEST,     "CHEST"     },
		{ BAR_KIND_FURNACE,   "FURNACE"   },
	};

	for (size_t k = 0; k < sizeof(labels) / sizeof(labels[0]); k++) {
		CHECK(strcmp(barKindLabel(labels[k].k), labels[k].label) == 0);
		CHECK(barKindHorizontalIsContent(labels[k].k) == (labels[k].k != BAR_KIND_CRAFT));
	}

	// Rows. The grids ignore list_len entirely, which is checked by passing an absurd one.
	CHECK(barKindRows(BAR_KIND_INVENTORY, 0) == 3);
	CHECK(barKindRows(BAR_KIND_INVENTORY, 99) == 3);
	CHECK(barKindRows(BAR_KIND_CHEST, 0) == 4);
	CHECK(barKindRows(BAR_KIND_CHEST, 99) == 4);
	CHECK(barKindRows(BAR_KIND_FURNACE, 0) == 4);
	CHECK(barKindRows(BAR_KIND_FURNACE, 99) == 4);

	for (int n = 0; n <= 16; n++)
		CHECK(barKindRows(BAR_KIND_CRAFT, n) == n);

	// Columns. Every row is eight cells wide except the furnace's own row (three) and a list
	// (one). The chest's row is CHEST_SLOTS wide, which is eight today — stated as its own
	// check so the table below is a claim about the layout and not about a coincidence.
	CHECK(CHEST_SLOTS == INV_HOTBAR_SLOTS);

	for (int row = 0; row < 3; row++)
		CHECK(barKindCols(BAR_KIND_INVENTORY, row) == INV_HOTBAR_SLOTS);

	CHECK(barKindCols(BAR_KIND_CHEST, 0) == INV_HOTBAR_SLOTS);
	CHECK(barKindCols(BAR_KIND_CHEST, 1) == CHEST_SLOTS);
	CHECK(barKindCols(BAR_KIND_CHEST, 2) == INV_HOTBAR_SLOTS);
	CHECK(barKindCols(BAR_KIND_CHEST, 3) == INV_HOTBAR_SLOTS);

	CHECK(barKindCols(BAR_KIND_FURNACE, 0) == INV_HOTBAR_SLOTS);
	CHECK(barKindCols(BAR_KIND_FURNACE, 1) == 3);
	CHECK(barKindCols(BAR_KIND_FURNACE, 2) == INV_HOTBAR_SLOTS);
	CHECK(barKindCols(BAR_KIND_FURNACE, 3) == INV_HOTBAR_SLOTS);

	for (int row = 0; row < 16; row++)
		CHECK(barKindCols(BAR_KIND_CRAFT, row) == 1);

	// A row past the end of a grid has no columns, so barCellRect's one bound check covers it.
	CHECK(barKindCols(BAR_KIND_INVENTORY, 3) == 0);
	CHECK(barKindCols(BAR_KIND_CHEST, 4) == 0);
	CHECK(barKindCols(BAR_KIND_FURNACE, 4) == 0);

	for (size_t k = 0; k < sizeof(kTestKinds) / sizeof(kTestKinds[0]); k++)
		CHECK(barKindCols(kTestKinds[k], -1) == 0);

	// A kind outside the enum answers "nothing", never a plausible-looking default.
	CHECK(barKindLabel((BarKind)99)[0] == '\0');
	CHECK(barKindRows((BarKind)99, 5) == 0);
	CHECK(barKindCols((BarKind)99, 0) == 0);
	CHECK(barKindHorizontalIsContent((BarKind)99) == false);
}

// The tab table: 2 categories with no container open, 3 with one, the container always last,
// and never two containers at once. All four (has_chest, has_furnace) combinations are walked
// including the impossible one — main.c opens one container per PLACE press, so both-true
// cannot happen, and what this checks is that it degrades to one tab rather than to four.
static void testBarBuildKinds(void)
{
	for (int c = 0; c < 2; c++) {
		for (int f = 0; f < 2; f++) {
			BarKind out[BAR_MAX_TABS];
			for (int i = 0; i < BAR_MAX_TABS; i++) out[i] = (BarKind)-1;

			const int n = barBuildKinds(c != 0, f != 0, out);

			CHECK(n >= 2);
			CHECK(n <= BAR_MAX_TABS);
			CHECK(n == ((c || f) ? 3 : 2));
			CHECK(out[0] == BAR_KIND_INVENTORY);
			CHECK(out[1] == BAR_KIND_CRAFT);

			int containers = 0;
			for (int i = 0; i < n; i++)
				if (out[i] == BAR_KIND_CHEST || out[i] == BAR_KIND_FURNACE) containers++;
			CHECK(containers == ((c || f) ? 1 : 0));

			if (n == 3) {
				CHECK(out[2] == (c ? BAR_KIND_CHEST : BAR_KIND_FURNACE));
				CHECK(out[0] != BAR_KIND_CHEST && out[0] != BAR_KIND_FURNACE);
				CHECK(out[1] != BAR_KIND_CHEST && out[1] != BAR_KIND_FURNACE);
			}

			// Whatever it built, the strip tiles for that many tabs, every tab is labelled,
			// and no tab is narrower than the touch budget.
			int edge = 0;
			for (int i = 0; i < n; i++) {
				const URect r = barTabRect(i, n);
				CHECK(barKindLabel(out[i])[0] != '\0');
				CHECK(r.x == edge);
				CHECK(r.w >= TEST_MIN_TAB_W);
				edge = r.x + r.w;
			}
			CHECK(edge == BAR_TABS_W);
		}
	}
}

// Every bar cell is a rect that already shipped, addressed by (row, col) instead of by a flat
// slot index. This is the check that goes red if the row order drifts from what the header
// states — it compares against the named constructors, not against a second copy of the same
// arithmetic.
static void testBarCellRectMatchesTheShippedRects(void)
{
	for (int c = 0; c < INV_HOTBAR_SLOTS; c++) {
		CHECK(sameRect(barCellRect(BAR_KIND_INVENTORY, 0, c, 0), hotbarSlotRect(c)));
		CHECK(sameRect(barCellRect(BAR_KIND_CHEST,     0, c, 0), hotbarSlotRect(c)));
		CHECK(sameRect(barCellRect(BAR_KIND_FURNACE,   0, c, 0), hotbarSlotRect(c)));
	}

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const int col = i % INV_MAIN_COLS;
		CHECK(sameRect(barCellRect(BAR_KIND_INVENTORY, 1 + i / INV_MAIN_COLS, col, 0),
		               gridSlotRect(i)));
	}

	for (int c = 0; c < CHEST_SLOTS; c++)
		CHECK(sameRect(barCellRect(BAR_KIND_CHEST, 1, c, 0), chestSlotRect(c)));

	for (int i = 0; i < INV_MAIN_SLOTS; i++) {
		const int row = 2 + i / INV_MAIN_COLS;
		const int col = i % INV_MAIN_COLS;
		CHECK(sameRect(barCellRect(BAR_KIND_CHEST,   row, col, 0), chestGridSlotRect(i)));
		CHECK(sameRect(barCellRect(BAR_KIND_FURNACE, row, col, 0), furnGridSlotRect(i)));
	}

	CHECK(sameRect(barCellRect(BAR_KIND_FURNACE, 1, 0, 0), furnInputRect()));
	CHECK(sameRect(barCellRect(BAR_KIND_FURNACE, 1, 1, 0), furnFuelRect()));
	CHECK(sameRect(barCellRect(BAR_KIND_FURNACE, 1, 2, 0), furnOutputRect()));

	// And the furnace's three-wide row reads left to right in column order, which is the one
	// property a swapped pair would leave the drawing looking identical for.
	CHECK(barCellRect(BAR_KIND_FURNACE, 1, 0, 0).x < barCellRect(BAR_KIND_FURNACE, 1, 1, 0).x);
	CHECK(barCellRect(BAR_KIND_FURNACE, 1, 1, 0).x < barCellRect(BAR_KIND_FURNACE, 1, 2, 0).x);
}

// The round trip, for every cell of every grid kind: the centre and both far corners of
// barCellRect(row, col) resolve back through barCellFromPoint() to that exact (row, col).
//
// The two sides are independent — see barCellFromPoint()'s comment in ui_layout.c — so this
// fails on a column swap, on a wrong row base, and on a grid drawn at the other screen's y.
static void testBarCellsRoundTrip(void)
{
	for (size_t g = 0; g < sizeof(kTestGridKinds) / sizeof(kTestGridKinds[0]); g++) {
		const BarKind k = kTestGridKinds[g];
		const int rows = barKindRows(k, 0);

		for (int row = 0; row < rows; row++) {
			const int cols = barKindCols(k, row);

			for (int col = 0; col < cols; col++) {
				const URect r = barCellRect(k, row, col, 0);

				CHECK(r.w == SLOT_PX);
				CHECK(r.h == SLOT_PX);
				CHECK(r.x >= 0 && r.x + r.w <= SCR_W);
				CHECK(r.y >= 0 && r.y + r.h <= SCR_H);
				CHECK(r.h >= TEST_MIN_TOUCH_H);
				CHECK(!rectsOverlap(r, barStripRect()));
				CHECK(!rectsOverlap(r, barCloseRect()));

				// scroll belongs to the list categories and must not move a grid cell.
				CHECK(sameRect(r, barCellRect(k, row, col, 7)));

				int cx, cy, gr = -1, gc = -1;
				centerOf(r, &cx, &cy);
				CHECK(barCellFromPoint(k, cx, cy, 0, &gr, &gc));
				CHECK(gr == row);
				CHECK(gc == col);

				gr = gc = -1;
				CHECK(barCellFromPoint(k, r.x, r.y, 0, &gr, &gc));
				CHECK(gr == row && gc == col);

				gr = gc = -1;
				CHECK(barCellFromPoint(k, r.x + r.w - 1, r.y + r.h - 1, 0, &gr, &gc));
				CHECK(gr == row && gc == col);
			}
		}
	}
}

// No two cells of the same category overlap. Separate from the round trip because a round trip
// can pass with two cells on top of each other as long as the hit test resolves the overlap
// consistently — and the cell the player then cannot reach is invisible in both.
static void testBarCellsDoNotOverlap(void)
{
	for (size_t g = 0; g < sizeof(kTestGridKinds) / sizeof(kTestGridKinds[0]); g++) {
		const BarKind k = kTestGridKinds[g];
		const int rows = barKindRows(k, 0);

		for (int r1 = 0; r1 < rows; r1++)
			for (int c1 = 0; c1 < barKindCols(k, r1); c1++)
				for (int r2 = r1; r2 < rows; r2++)
					for (int c2 = (r2 == r1) ? c1 + 1 : 0; c2 < barKindCols(k, r2); c2++)
						CHECK(!rectsOverlap(barCellRect(k, r1, c1, 0),
						                    barCellRect(k, r2, c2, 0)));
	}
}

// CRAFT: the scrolling list. Row r is list row r - scroll, so the SAME screen rect means a
// different recipe at a different scroll, and the round trip has to carry the scroll through.
// Walked at four scrolls including one well past the visible window.
static void testBarCraftListRoundTrip(void)
{
	static const int scrolls[] = { 0, 1, 3, 9 };

	for (size_t s = 0; s < sizeof(scrolls) / sizeof(scrolls[0]); s++) {
		const int scroll = scrolls[s];

		for (int v = 0; v < BAR_LIST_VISIBLE; v++) {
			const int row = scroll + v;
			const URect r = barCellRect(BAR_KIND_CRAFT, row, 0, scroll);

			CHECK(sameRect(r, barListRowRect(v)));
			CHECK(r.h >= TEST_MIN_TOUCH_H);

			int cx, cy, gr = -1, gc = -1;
			centerOf(r, &cx, &cy);
			CHECK(barCellFromPoint(BAR_KIND_CRAFT, cx, cy, scroll, &gr, &gc));
			CHECK(gr == row);
			CHECK(gc == 0);

			gr = gc = -1;
			CHECK(barCellFromPoint(BAR_KIND_CRAFT, r.x, r.y, scroll, &gr, &gc));
			CHECK(gr == row && gc == 0);

			gr = gc = -1;
			CHECK(barCellFromPoint(BAR_KIND_CRAFT, r.x + r.w - 1, r.y + r.h - 1,
			                       scroll, &gr, &gc));
			CHECK(gr == row && gc == 0);
		}

		// Scrolled off either end draws nothing rather than drawing on top of the hotbar.
		CHECK(barCellRect(BAR_KIND_CRAFT, scroll - 1, 0, scroll).w == 0);
		CHECK(barCellRect(BAR_KIND_CRAFT, scroll + BAR_LIST_VISIBLE, 0, scroll).w == 0);
		CHECK(barCellRect(BAR_KIND_CRAFT, scroll + 100, 0, scroll).h == 0);

		// A list is one cell wide, so there is no column 1 and no column -1.
		CHECK(barCellRect(BAR_KIND_CRAFT, scroll, 1, scroll).w == 0);
		CHECK(barCellRect(BAR_KIND_CRAFT, scroll, -1, scroll).w == 0);
	}
}

// Every way of asking for a cell that does not exist gives a zero rect, and every point that is
// not on a cell gives false — including the strip, which is the one band a player's stylus is
// most likely to be on when they are not aiming at a cell at all.
static void testBarCellRectRejectsOutOfRange(void)
{
	static const int bad_rows[] = { -1, -100, 99, 1000 };
	static const int bad_cols[] = { -1, -100, INV_HOTBAR_SLOTS, 99 };

	for (size_t g = 0; g < sizeof(kTestKinds) / sizeof(kTestKinds[0]); g++) {
		const BarKind k = kTestKinds[g];

		for (size_t b = 0; b < sizeof(bad_rows) / sizeof(bad_rows[0]); b++) {
			const URect r = barCellRect(k, bad_rows[b], 0, 0);
			CHECK(r.w == 0);
			CHECK(r.h == 0);
			CHECK(!ptInRect(r, r.x, r.y));
		}

		for (size_t b = 0; b < sizeof(bad_cols) / sizeof(bad_cols[0]); b++) {
			const URect r = barCellRect(k, 0, bad_cols[b], 0);
			CHECK(r.w == 0);
			CHECK(r.h == 0);
			CHECK(!ptInRect(r, r.x, r.y));
		}
	}

	{
		const URect r = barCellRect((BarKind)99, 0, 0, 0);
		CHECK(r.w == 0);
		CHECK(r.h == 0);
		CHECK(!ptInRect(r, r.x, r.y));
	}

	// The furnace row is three wide and not eight, so columns 3..7 exist nowhere on it.
	CHECK(barCellRect(BAR_KIND_FURNACE, 1, 3, 0).w == 0);
	CHECK(barCellRect(BAR_KIND_FURNACE, 1, 7, 0).w == 0);

	// Points that are on no cell of any category: off all four edges, on the strip, and in the
	// 6 px gap between the strip and the top of the content band.
	static const int miss[][2] = {
		{ -1, -1 }, { SCR_W, 0 }, { 0, SCR_H }, { SCR_W + 50, SCR_H + 50 },
		{ 10, BAR_STRIP_Y + 2 }, { 0, BAR_LIST_Y0 - 1 },
	};

	for (size_t g = 0; g < sizeof(kTestKinds) / sizeof(kTestKinds[0]); g++)
		for (size_t m = 0; m < sizeof(miss) / sizeof(miss[0]); m++) {
			int gr = 123, gc = 456;
			CHECK(!barCellFromPoint(kTestKinds[g], miss[m][0], miss[m][1], 0, &gr, &gc));
		}
}

// The INVENTORY tab's detail band. Two claims: the bag clears it, and it is the same band the
// two container tabs give the relocated bag — deliberately equal, not accidentally, because
// they are never on screen together and pinning them equal is what records that.
static void testBarDetailBand(void)
{
	CHECK(BAR_DETAIL_Y >= GRID_Y + GRID_H);
	CHECK(BAR_DETAIL_Y == FURN_GRID_Y);
	CHECK(BAR_DETAIL_Y == CHEST_GRID_Y);
	CHECK(HUD_PIPS_Y0 >= BAR_DETAIL_Y);
	CHECK(BAR_DETAIL_Y < SCR_H);

	for (int row = 0; row < barKindRows(BAR_KIND_INVENTORY, 0); row++)
		for (int col = 0; col < barKindCols(BAR_KIND_INVENTORY, row); col++) {
			const URect r = barCellRect(BAR_KIND_INVENTORY, row, col, 0);
			CHECK(r.y + r.h <= BAR_DETAIL_Y);
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
	testHudStatusBandCatchesASmallDrift();

	testFurnaceSlotsRoundTrip();
	testFurnaceSlotRectRejectsOutOfRangeIndex();
	testFurnaceSlotsDoNotCollide();
	testFurnaceInvSlotsRoundTrip();
	testTheTwoGridsAreDisjoint();
	testFurnaceGaugesAreNotTouchTargets();
	testFurnacePanelFitsAndIsTappable();
	testFurnBarFillScale();
	testFurnBarFillGuardsTheDivision();
	testFurnBarFillIsBoundedAndMonotonic();

	testMoveStackIntoEmptyDestination();
	testMoveStackIntoMergesWithSameItem();
	testMoveStackIntoPartialLeavesTheRemainder();
	testMoveStackIntoExactFit();
	testMoveStackIntoRefusals();
	testMoveStackIntoIgnoresAStaleCountOnAnEmptyDestination();
	testMoveStackIntoConservesUnits();

	testBarStripIsOneBand();
	testBarTabsTileTheStrip();
	testBarCloseTab();
	testBarListRows();
	testBarKindTables();
	testBarBuildKinds();
	testBarCellRectMatchesTheShippedRects();
	testBarCellsRoundTrip();
	testBarCellsDoNotOverlap();
	testBarCraftListRoundTrip();
	testBarCellRectRejectsOutOfRange();
	testBarDetailBand();

	// Last, so it counts everything above it. See its own comment for why the count is pinned
	// at all rather than eyeballed.
	checkCountPin();

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
