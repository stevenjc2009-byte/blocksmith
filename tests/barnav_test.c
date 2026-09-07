// Host gate for scene/barnav.c — the v1.9.1 tabbed bar's navigation model.
//
// WHY THIS FILE LINKS THE REAL MODULE. Everything that can be WRONG about the bar's cursor —
// which tab a shoulder press lands on, whether B closes the bar out from under a lift, whether
// walking down through a 3-wide furnace row loses the column you were on — lives in
// scene/barnav.c, which has no <3ds.h> in it precisely so this binary can link THAT module, the
// same carve-out tests/hotbar_test.c, tests/ui_gesture_test.c and tests/ringorder_test.c made.
// Nothing here is a copy of the code under test; the key bits are the header's own mirrors of
// libctru's, and the specs below are hand-written stand-ins for the shapes scene/ui_layout.h
// (Agent B) will describe, not that header's own values.
//
// WHAT THIS FILE CANNOT PROVE, stated up front: that scene/ui.c calls barNavInput from the
// frame loop with a real hidKeysDown() word, that BAR_EV_COMMIT becomes a synthetic tap at the
// focused cell's centre, that barStickEdge is fed hidCircleRead's dx/dy, or that anything is
// drawn in the right place. Those are the console's and the integrator's (blueprint §6.4 seams
// S1/S2, and playtest 1B.3).
//
// Own main(), same CHECK macro and PASS/FAIL line as tests/hotbar_test.c.
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene/barnav.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			if (!s_fails) snprintf(s_first, sizeof s_first, "L%d %s", __LINE__, #cond); \
			s_fails++;                                                             \
		}                                                                          \
	} while (0)

// Pinned, so a check that quietly stops being reached reads as a FAIL rather than as a shorter
// green run. Move it only when a check is deliberately added or removed, and say so.
//
// One deliberate difference from tests/hotbar_test.c's pin, recorded because it is visible in
// the output: hotbar's line reads `CHECK(s_checks + 1 == HOTBAR_EXPECTED_CHECKS)`, and since
// the macro has already incremented by the time the condition runs, its PASS line prints one
// LESS than its pin. Blueprint §1A.1 requires this suite's printed count to EQUAL
// BARNAV_TEST_EXPECTED_CHECKS, so the pin below is compared without the +1. Same convention —
// a literal that goes red the moment the count drifts — with the off-by-one taken out.
#define BARNAV_TEST_EXPECTED_CHECKS 3306

#define K_A  BAR_KEY_A
#define K_B  BAR_KEY_B
#define K_X  BAR_KEY_X
#define K_Y  BAR_KEY_Y
#define K_L  BAR_KEY_L
#define K_R  BAR_KEY_R
#define K_UP BAR_KEY_DUP
#define K_DN BAR_KEY_DDOWN
#define K_LT BAR_KEY_DLEFT
#define K_RT BAR_KEY_DRIGHT

// ── The category shapes, hand-written ──────────────────────────────────────────────────────
//
// These mirror blueprint §6.2's barKindRows/barKindCols table (INVENTORY 3 rows of 8; CRAFT
// RECIPE_COUNT rows of 1, left/right switch tabs; CHEST 4 rows of 8; FURNACE 4 rows with row 1
// only 3 wide) plus the pause menu's stepper shape (one column, left/right step the value).
// Written out here rather than pulled from ui_layout.h on purpose: barnav takes the shape as an
// argument precisely so it does not depend on that header, and this suite proves the arithmetic
// at several shapes rather than at one build's constants.

#define INV_ROWS   3
#define CRAFT_ROWS 7
#define CHEST_ROWS 4
#define FURN_ROWS  4
#define OPT_ROWS   5

static BarCatSpec specInventory(void)
{
	BarCatSpec s = {0};
	s.rows = INV_ROWS; s.uniform_cols = 8; s.horizontal_is_content = true;
	return s;
}

static BarCatSpec specCraft(void)
{
	BarCatSpec s = {0};
	s.rows = CRAFT_ROWS; s.uniform_cols = 1; s.horizontal_is_content = false;
	return s;
}

static BarCatSpec specChest(void)
{
	BarCatSpec s = {0};
	s.rows = CHEST_ROWS; s.uniform_cols = 8; s.horizontal_is_content = true;
	return s;
}

// The one category with PER-ROW widths: row 1 is input / fuel / output, three cells wide.
static BarCatSpec specFurnace(void)
{
	BarCatSpec s = {0};
	s.rows = FURN_ROWS; s.uniform_cols = 0; s.horizontal_is_content = true;
	s.cols[0] = 8; s.cols[1] = 3; s.cols[2] = 8; s.cols[3] = 8;
	return s;
}

// The pause/title OPTIONS shape: a one-column list whose left/right STEP a value rather than
// switch tabs. The only difference from CRAFT is horizontal_is_content, and that one flag is
// the whole of what separates a stepper from a list.
static BarCatSpec specOptions(void)
{
	BarCatSpec s = {0};
	s.rows = OPT_ROWS; s.uniform_cols = 1; s.horizontal_is_content = true;
	return s;
}

// Rows to the number of columns, computed here rather than asked of the module, so the two
// answers are independent.
static int expectCols(const BarCatSpec* s, int row)
{
	if (s->uniform_cols > 0) return s->uniform_cols;
	if (row < 0 || row >= BARNAV_MAX_ROWS) return 1;
	return (s->cols[row] > 0) ? (int)s->cols[row] : 1;
}

// The invariant blueprint §1A.3 states: after ANY key sequence the cursor is on a real cell.
static void checkInvariant(const BarNav* n, const BarCatSpec* cats, int count)
{
	CHECK(n->cat >= 0 && n->cat < count);
	if (n->cat < 0 || n->cat >= count) return;
	const BarCatSpec* s = &cats[n->cat];
	if (s->rows <= 0) {
		CHECK(n->row == 0);
		CHECK(n->col == 0);
		return;
	}
	CHECK(n->row >= 0 && n->row < s->rows);
	CHECK(n->col >= 0 && n->col < expectCols(s, n->row));
}

// ── Zero state, reset, NULLs ───────────────────────────────────────────────────────────────

static void testZeroStateIsValidAndInert(void)
{
	BarCatSpec cats[2] = { specInventory(), specCraft() };

	// A memset-zero BarNav is the correct initial state — this is what lets scene/ui.h embed
	// one and let uiInit's memset do the work.
	BarNav n;
	memset(&n, 0, sizeof n);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0 && n.pref_col == 0);
	for (int i = 0; i < BARNAV_MAX_CATS; i++) CHECK(n.row_mem[i] == 0);
	checkInvariant(&n, cats, 2);
	barNavClamp(&n, cats, 2);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0);
	CHECK(barNavInput(&n, 0u, cats, 2, false) == BAR_EV_NONE);
	checkInvariant(&n, cats, 2);

	// A memset-zero BarCatSpec is a category with NO cursor content: every move and the commit
	// are NONE, and the cursor pins to the origin. A text page is a real shape (blueprint D10's
	// MULTIPLAYER body), so this has to be inert rather than undefined.
	BarCatSpec empty[1];
	memset(empty, 0, sizeof empty);
	CHECK(empty[0].rows == 0);
	BarNav e;
	memset(&e, 0, sizeof e);
	static const uint32_t moves[] = { K_UP, K_DN, K_LT, K_RT, K_A };
	for (unsigned i = 0; i < sizeof moves / sizeof moves[0]; i++) {
		CHECK(barNavInput(&e, moves[i], empty, 1, false) == BAR_EV_NONE);
		CHECK(e.row == 0 && e.col == 0 && e.cat == 0);
		checkInvariant(&e, empty, 1);
	}
	// B still answers on an empty category — closing the screen is not cursor content.
	CHECK(barNavInput(&e, K_B, empty, 1, false) == BAR_EV_BACK);
	CHECK(barNavInput(&e, K_B, empty, 1, true) == BAR_EV_CANCEL);
	// X too: BAR_EV_ALT is a screen-level gesture, not a cell one.
	CHECK(barNavInput(&e, K_X, empty, 1, false) == BAR_EV_ALT);

	// A memset-zero BarStick is disarmed: no edge until something deflects.
	BarStick st;
	memset(&st, 0, sizeof st);
	CHECK(st.dir_x == 0 && st.dir_y == 0);
	CHECK(barStickEdge(&st, 0, 0) == 0);
}

static void testResetAndNulls(void)
{
	BarNav n;
	memset(&n, 0xAB, sizeof n);          // deliberate garbage, to prove reset zeroes it
	barNavReset(&n, 2, 3, 5);
	CHECK(n.cat == 2 && n.row == 3 && n.col == 5);
	CHECK(n.pref_col == 5);              // reset takes col as the preferred column
	for (int i = 0; i < BARNAV_MAX_CATS; i++) CHECK(n.row_mem[i] == 0);

	// Negative arguments floor at 0 rather than being stored and clamped later.
	barNavReset(&n, -1, -7, -2);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0 && n.pref_col == 0);

	// Every entry point refuses a NULL nav rather than dereferencing it.
	BarCatSpec cats[2] = { specInventory(), specCraft() };
	barNavReset(NULL, 1, 1, 1);
	barNavClamp(NULL, cats, 2);
	barNavSetCat(NULL, 1, cats, 2);
	barNavSetCell(NULL, 1, 1, cats, 2);
	CHECK(barNavInput(NULL, K_A, cats, 2, false) == BAR_EV_NONE);
	CHECK(barStickEdge(NULL, 100, 100) == 0);
	CHECK(true);   // reached: every NULL was refused, not dereferenced

	// A NULL category list, or none at all, is inert and parks the cursor at the origin.
	barNavReset(&n, 3, 2, 4);
	CHECK(barNavInput(&n, K_R, NULL, 2, false) == BAR_EV_NONE);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0);
	barNavReset(&n, 3, 2, 4);
	CHECK(barNavInput(&n, K_R, cats, 0, false) == BAR_EV_NONE);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0);
	barNavReset(&n, 3, 2, 4);
	barNavClamp(&n, cats, -5);
	CHECK(n.cat == 0 && n.row == 0 && n.col == 0);
}

// ── barNavClamp: the cursor invariant, at every category count ─────────────────────────────

static void testClampEveryCatCount(void)
{
	// Six real categories, so a count of 1..6 is a prefix of a live list rather than a
	// contrivance. The shapes repeat on purpose — what is under test is the INDEX arithmetic.
	BarCatSpec cats[BARNAV_MAX_CATS] = {
		specInventory(), specCraft(), specChest(), specFurnace(), specOptions(), specInventory()
	};

	for (int count = 1; count <= BARNAV_MAX_CATS; count++) {
		// Every cat index the caller could hold, valid or not, including well past the array.
		for (int cat = -3; cat < BARNAV_MAX_CATS + 3; cat++) {
			BarNav n;
			barNavReset(&n, 0, 0, 0);
			n.cat = cat;
			n.row = 99;
			n.col = 99;
			barNavClamp(&n, cats, count);
			// The one rule the header names: out of range goes to 0, NOT to count-1. The
			// container tab vanishing is what this is for, and INVENTORY is always there.
			CHECK(n.cat == ((cat >= 0 && cat < count) ? cat : 0));
			checkInvariant(&n, cats, count);
		}
	}

	// The named case from blueprint §7's red arm 1: cat 2 of 2 -> 0.
	BarCatSpec two[2] = { specInventory(), specCraft() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);
	n.cat = 2;
	barNavClamp(&n, two, 2);
	CHECK(n.cat == 0);              // clamp: cat 2 of 2 -> 0
	n.cat = 5;
	barNavClamp(&n, two, 2);
	CHECK(n.cat == 0);
	n.cat = -1;
	barNavClamp(&n, two, 2);
	CHECK(n.cat == 0);

	// A count above BARNAV_MAX_CATS is clamped to the length of row_mem, not read past it.
	barNavReset(&n, 0, 0, 0);
	n.cat = BARNAV_MAX_CATS;        // legal for the caller's array, not for row_mem
	barNavClamp(&n, cats, BARNAV_MAX_CATS + 4);
	CHECK(n.cat == 0);
	n.cat = BARNAV_MAX_CATS - 1;
	barNavClamp(&n, cats, BARNAV_MAX_CATS + 4);
	CHECK(n.cat == BARNAV_MAX_CATS - 1);

	// pref_col is a MEMORY of a wide row and is deliberately NOT clamped down to the current
	// row's width — only floored at 0. Clamping it would be the bug testPrefColMemory guards.
	barNavReset(&n, 3, 1, 0);       // furnace, row 1 (3 wide)
	n.pref_col = 7;
	barNavClamp(&n, cats, BARNAV_MAX_CATS);
	CHECK(n.pref_col == 7);
	CHECK(n.col == 0);
	n.pref_col = -4;
	barNavClamp(&n, cats, BARNAV_MAX_CATS);
	CHECK(n.pref_col == 0);
}

static void testClampRowAndColRanges(void)
{
	BarCatSpec cats[4] = { specInventory(), specCraft(), specChest(), specFurnace() };

	// Every (cat, row, col) a caller could hold, clamped to a real cell — and clamped, not
	// reset: a survivable change keeps the player's place.
	for (int cat = 0; cat < 4; cat++) {
		const BarCatSpec* s = &cats[cat];
		for (int row = -2; row < s->rows + 2; row++) {
			for (int col = -2; col < 10; col++) {
				BarNav n;
				barNavReset(&n, 0, 0, 0);
				n.cat = cat; n.row = row; n.col = col;
				barNavClamp(&n, cats, 4);
				CHECK(n.cat == cat);
				const int want_row = (row < 0) ? 0 : (row >= s->rows ? s->rows - 1 : row);
				CHECK(n.row == want_row);
				const int w = expectCols(s, want_row);
				CHECK(n.col == ((col < 0) ? 0 : (col >= w ? w - 1 : col)));
			}
		}
	}
}

// ── Vertical movement: wrap, at every row of every shape ───────────────────────────────────

static void testRowWrapEveryEdge(void)
{
	BarCatSpec cats[4] = { specInventory(), specCraft(), specChest(), specFurnace() };

	for (int cat = 0; cat < 4; cat++) {
		const int rows = cats[cat].rows;
		for (int start = 0; start < rows; start++) {
			BarNav n;
			barNavReset(&n, cat, start, 0);
			CHECK(barNavInput(&n, K_DN, cats, 4, false) == BAR_EV_MOVE);
			CHECK(n.row == (start + 1) % rows);
			checkInvariant(&n, cats, 4);

			barNavReset(&n, cat, start, 0);
			CHECK(barNavInput(&n, K_UP, cats, 4, false) == BAR_EV_MOVE);
			CHECK(n.row == (start + rows - 1) % rows);
			checkInvariant(&n, cats, 4);
		}
		// A full lap either way lands home, which is what says the wrap is reached by the walk
		// and not only by the two asserted ends.
		BarNav n;
		barNavReset(&n, cat, 1 % rows, 0);
		for (int i = 0; i < rows; i++) (void)barNavInput(&n, K_DN, cats, 4, false);
		CHECK(n.row == 1 % rows);
		for (int i = 0; i < rows; i++) (void)barNavInput(&n, K_UP, cats, 4, false);
		CHECK(n.row == 1 % rows);
	}

	// The two named edges, spelled out.
	BarNav n;
	barNavReset(&n, 0, INV_ROWS - 1, 0);
	CHECK(barNavInput(&n, K_DN, cats, 4, false) == BAR_EV_MOVE);
	CHECK(n.row == 0);                                   // bottom wraps to the top
	barNavReset(&n, 0, 0, 0);
	CHECK(barNavInput(&n, K_UP, cats, 4, false) == BAR_EV_MOVE);
	CHECK(n.row == INV_ROWS - 1);                        // top wraps to the bottom

	// A one-row category has nowhere to go: NONE, not a MOVE that changed nothing.
	BarCatSpec one[1] = { specInventory() };
	one[0].rows = 1;
	BarNav o;
	barNavReset(&o, 0, 0, 3);
	CHECK(barNavInput(&o, K_DN, one, 1, false) == BAR_EV_NONE);
	CHECK(barNavInput(&o, K_UP, one, 1, false) == BAR_EV_NONE);
	CHECK(o.row == 0 && o.col == 3);
}

// ── pref_col: the memory a narrow row must not eat ─────────────────────────────────────────

static void testPrefColAcrossNarrowRow(void)
{
	// The furnace shape is the whole reason pref_col exists: rows 0/2/3 are 8 wide and row 1 is
	// 3 wide, so a cursor walked down the right-hand edge crosses a row that cannot hold it.
	BarCatSpec cats[1] = { specFurnace() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);

	// Walk to column 7 with left/right — the only press that sets pref_col.
	for (int i = 0; i < 7; i++) {
		CHECK(barNavInput(&n, K_RT, cats, 1, false) == BAR_EV_MOVE);
	}
	CHECK(n.col == 7 && n.pref_col == 7);

	// Down into the 3-wide row: clamped to 2, and pref_col is untouched.
	CHECK(barNavInput(&n, K_DN, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 1);
	CHECK(n.col == 2);                 // furnace: col 7 down 2
	CHECK(n.pref_col == 7);

	// Out the other side: back to 7, not left behind at 2.
	CHECK(barNavInput(&n, K_DN, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 2);
	CHECK(n.col == 7);                 // furnace: col 7 down 2 down 7
	CHECK(n.pref_col == 7);

	// And upwards through it, the same.
	CHECK(barNavInput(&n, K_UP, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 1 && n.col == 2 && n.pref_col == 7);
	CHECK(barNavInput(&n, K_UP, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 0 && n.col == 7 && n.pref_col == 7);

	// A deliberate left/right INSIDE the narrow row replaces the memory — the player chose 1,
	// so 1 is what the wide rows get from now on.
	CHECK(barNavInput(&n, K_DN, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 1 && n.col == 2);
	CHECK(barNavInput(&n, K_LT, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.col == 1 && n.pref_col == 1);
	CHECK(barNavInput(&n, K_DN, cats, 1, false) == BAR_EV_MOVE);
	CHECK(n.row == 2 && n.col == 1);

	// Every starting column, walked down through the narrow row and out: the answer is always
	// min(start, 2) in the middle and start again after it.
	for (int start = 0; start < 8; start++) {
		BarNav w;
		barNavReset(&w, 0, 0, 0);
		for (int i = 0; i < start; i++) (void)barNavInput(&w, K_RT, cats, 1, false);
		CHECK(w.col == start);
		(void)barNavInput(&w, K_DN, cats, 1, false);
		CHECK(w.col == (start < 3 ? start : 2));
		(void)barNavInput(&w, K_DN, cats, 1, false);
		CHECK(w.col == start);
	}
}

// ── Categories: L/R, clamping, row_mem ─────────────────────────────────────────────────────

static void testCatStepClampsAndRemembers(void)
{
	BarCatSpec cats[3] = { specInventory(), specCraft(), specChest() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);

	// Clamped, no wrap: L at the leftmost tab is NONE, not a jump to the last one.
	CHECK(barNavInput(&n, K_L, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.cat == 0);
	CHECK(barNavInput(&n, K_R, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1);
	CHECK(barNavInput(&n, K_R, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 2);
	CHECK(barNavInput(&n, K_R, cats, 3, false) == BAR_EV_NONE);   // clamped at the right end
	CHECK(n.cat == 2);
	CHECK(barNavInput(&n, K_L, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1);

	// row_mem: each tab keeps the row it was left on, in both directions.
	barNavReset(&n, 0, 0, 0);
	(void)barNavInput(&n, K_DN, cats, 3, false);
	(void)barNavInput(&n, K_DN, cats, 3, false);
	CHECK(n.row == 2);                              // INVENTORY left on row 2
	CHECK(barNavInput(&n, K_R, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1 && n.row == 0);                // CRAFT has never been visited
	for (int i = 0; i < 5; i++) (void)barNavInput(&n, K_DN, cats, 3, false);
	CHECK(n.row == 5);                              // CRAFT left on row 5
	CHECK(barNavInput(&n, K_L, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 0 && n.row == 2);                // INVENTORY's row came back
	CHECK(barNavInput(&n, K_R, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1 && n.row == 5);                // and so did CRAFT's
	CHECK(n.row_mem[0] == 2);
	CHECK(n.row_mem[1] == 5);

	// A remembered row that no longer fits (the list shrank) is clamped on arrival, not
	// restored blindly.
	BarCatSpec shrunk[3] = { specInventory(), specCraft(), specChest() };
	shrunk[1].rows = 2;
	CHECK(barNavInput(&n, K_L, shrunk, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 0);
	CHECK(barNavInput(&n, K_R, shrunk, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1 && n.row == 1);                // remembered 5, clamped to the last row
	checkInvariant(&n, shrunk, 3);

	// One category: L and R have nowhere to go.
	BarCatSpec solo[1] = { specInventory() };
	BarNav s;
	barNavReset(&s, 0, 1, 4);
	CHECK(barNavInput(&s, K_L, solo, 1, false) == BAR_EV_NONE);
	CHECK(barNavInput(&s, K_R, solo, 1, false) == BAR_EV_NONE);
	CHECK(s.cat == 0 && s.row == 1 && s.col == 4);

	// The column comes back through pref_col on a tab change too, not as 0 and not as whatever
	// the narrow tab clamped it to.
	BarCatSpec pair[2] = { specInventory(), specCraft() };
	BarNav p;
	barNavReset(&p, 0, 0, 0);
	for (int i = 0; i < 6; i++) (void)barNavInput(&p, K_RT, pair, 2, false);
	CHECK(p.col == 6 && p.pref_col == 6);
	CHECK(barNavInput(&p, K_R, pair, 2, false) == BAR_EV_CAT);
	CHECK(p.cat == 1 && p.col == 0);                // CRAFT is one column wide
	CHECK(p.pref_col == 6);
	CHECK(barNavInput(&p, K_L, pair, 2, false) == BAR_EV_CAT);
	CHECK(p.cat == 0 && p.col == 6);                // and back
}

static void testSetCatAndSetCell(void)
{
	BarCatSpec cats[3] = { specInventory(), specCraft(), specFurnace() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);

	// The call scene/ui.c makes when a container opens: focus tab 2 directly.
	(void)barNavInput(&n, K_DN, cats, 3, false);
	CHECK(n.row == 1);
	barNavSetCat(&n, 2, cats, 3);
	CHECK(n.cat == 2 && n.row == 0);
	CHECK(n.row_mem[0] == 1);                       // the row it came from was remembered
	barNavSetCat(&n, 0, cats, 3);
	CHECK(n.cat == 0 && n.row == 1);

	// Out-of-range clamps INTO range here (a caller naming a tab means the nearest real one),
	// which is a different rule from barNavClamp's "a stale index goes home to 0".
	barNavSetCat(&n, 99, cats, 3);
	CHECK(n.cat == 2);
	barNavSetCat(&n, -4, cats, 3);
	CHECK(n.cat == 0);
	barNavSetCat(&n, 2, cats, 2);                   // only two categories live right now
	CHECK(n.cat == 1);
	checkInvariant(&n, cats, 2);

	// Setting the category already focused is a no-op that still leaves a valid cursor.
	barNavReset(&n, 1, 4, 0);
	barNavSetCat(&n, 1, cats, 3);
	CHECK(n.cat == 1 && n.row == 4 && n.col == 0);

	// The stylus path: a tap lands on a cell and the cursor follows it, pref_col included.
	barNavReset(&n, 0, 0, 0);
	barNavSetCell(&n, 2, 5, cats, 3);
	CHECK(n.row == 2 && n.col == 5 && n.pref_col == 5);
	CHECK(barNavInput(&n, K_UP, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.row == 1 && n.col == 5);                // continues from where the stylus was

	// Out-of-range cells clamp rather than being refused.
	barNavSetCell(&n, 99, 99, cats, 3);
	CHECK(n.row == INV_ROWS - 1 && n.col == 7);
	barNavSetCell(&n, -1, -1, cats, 3);
	CHECK(n.row == 0 && n.col == 0 && n.pref_col == 0);
	// On the furnace's narrow row a tap can only land on a real cell, and pref_col follows it.
	barNavReset(&n, 2, 1, 0);
	barNavSetCell(&n, 1, 6, cats, 3);
	CHECK(n.row == 1 && n.col == 2 && n.pref_col == 6);
	checkInvariant(&n, cats, 3);
}

// ── Horizontal: three jobs, one pair of buttons ────────────────────────────────────────────

static void testHorizontalIsContentGrid(void)
{
	BarCatSpec cats[2] = { specInventory(), specCraft() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);

	// Every column of a grid row, walked right then left. Clamped at both ends — a grid's
	// left/right must NOT wrap, or the cursor leaves the row the player is looking at.
	for (int c = 0; c < 7; c++) {
		CHECK(barNavInput(&n, K_RT, cats, 2, false) == BAR_EV_MOVE);
		CHECK(n.col == c + 1);
	}
	CHECK(barNavInput(&n, K_RT, cats, 2, false) == BAR_EV_NONE);
	CHECK(n.col == 7);
	CHECK(n.cat == 0);                              // and it did NOT fall through to a tab change
	for (int c = 7; c > 0; c--) {
		CHECK(barNavInput(&n, K_LT, cats, 2, false) == BAR_EV_MOVE);
		CHECK(n.col == c - 1);
	}
	CHECK(barNavInput(&n, K_LT, cats, 2, false) == BAR_EV_NONE);
	CHECK(n.col == 0);
	CHECK(n.cat == 0);
}

static void testHorizontalIsCatOnAList(void)
{
	// blueprint §7 red arm 2: on a list category DLEFT is a CAT, never a MOVE.
	BarCatSpec cats[3] = { specInventory(), specCraft(), specChest() };
	BarNav n;
	barNavReset(&n, 1, 3, 0);

	CHECK(barNavInput(&n, K_LT, cats, 3, false) == BAR_EV_CAT);   // list: DLEFT is CAT
	CHECK(n.cat == 0);
	barNavReset(&n, 1, 3, 0);
	CHECK(barNavInput(&n, K_RT, cats, 3, false) == BAR_EV_CAT);   // list: DRIGHT is CAT
	CHECK(n.cat == 2);

	// And they clamp exactly as L/R do, at both ends.
	barNavReset(&n, 0, 0, 0);
	CHECK(cats[0].horizontal_is_content == true);
	barNavSetCat(&n, 1, cats, 3);
	CHECK(barNavInput(&n, K_LT, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 0);
	// From the last tab, a list's right is clamped. Put CRAFT last to reach that edge.
	BarCatSpec tail[2] = { specInventory(), specCraft() };
	BarNav t;
	barNavReset(&t, 1, 0, 0);
	CHECK(barNavInput(&t, K_RT, tail, 2, false) == BAR_EV_NONE);
	CHECK(t.cat == 1);
	CHECK(t.row == 0);                               // and the row did not move either
}

static void testHorizontalIsStepOnAStepper(void)
{
	// The pause/title OPTIONS shape: one column, but horizontal IS content, so left/right step
	// the value instead of switching tabs. The cursor must not move — the caller changes a
	// setting, and a row that also jumped would change the wrong one next press.
	BarCatSpec cats[2] = { specOptions(), specCraft() };
	BarNav n;
	barNavReset(&n, 0, 2, 0);

	for (int row = 0; row < OPT_ROWS; row++) {
		barNavReset(&n, 0, row, 0);
		CHECK(barNavInput(&n, K_LT, cats, 2, false) == BAR_EV_STEP_LEFT);
		CHECK(n.row == row && n.col == 0 && n.cat == 0);
		CHECK(barNavInput(&n, K_RT, cats, 2, false) == BAR_EV_STEP_RIGHT);
		CHECK(n.row == row && n.col == 0 && n.cat == 0);
	}

	// A stepper never becomes a tab change, even at a tab edge where a CAT would be clamped.
	barNavReset(&n, 0, 0, 0);
	CHECK(barNavInput(&n, K_LT, cats, 2, false) == BAR_EV_STEP_LEFT);
	CHECK(n.cat == 0);

	// The same rule inside a per-row shape: a row that happens to be one column wide steps,
	// its 8-wide neighbours move. One flag, and the row's own width, decide.
	BarCatSpec mixed[1];
	memset(mixed, 0, sizeof mixed);
	mixed[0].rows = 3;
	mixed[0].uniform_cols = 0;
	mixed[0].horizontal_is_content = true;
	mixed[0].cols[0] = 8; mixed[0].cols[1] = 1; mixed[0].cols[2] = 8;
	BarNav m;
	barNavReset(&m, 0, 1, 0);
	CHECK(barNavInput(&m, K_RT, mixed, 1, false) == BAR_EV_STEP_RIGHT);
	CHECK(m.col == 0 && m.pref_col == 0);
	barNavReset(&m, 0, 0, 0);
	CHECK(barNavInput(&m, K_RT, mixed, 1, false) == BAR_EV_MOVE);
	CHECK(m.col == 1);

	// A cols[] entry of 0 reads as 1 — a row always has the cell the cursor is on — so it
	// steps rather than dividing the caller by nothing.
	BarCatSpec zero[1];
	memset(zero, 0, sizeof zero);
	zero[0].rows = 2;
	zero[0].horizontal_is_content = true;    // cols[] all 0
	BarNav z;
	barNavReset(&z, 0, 0, 0);
	CHECK(barNavInput(&z, K_RT, zero, 1, false) == BAR_EV_STEP_RIGHT);
	CHECK(z.col == 0);
	checkInvariant(&z, zero, 1);

	// A row past the end of cols[] with no uniform_cols also reads as 1 wide.
	BarCatSpec deep[1];
	memset(deep, 0, sizeof deep);
	deep[0].rows = BARNAV_MAX_ROWS + 2;
	deep[0].horizontal_is_content = true;
	for (int i = 0; i < BARNAV_MAX_ROWS; i++) deep[0].cols[i] = 8;
	BarNav d;
	barNavReset(&d, 0, BARNAV_MAX_ROWS + 1, 0);
	barNavClamp(&d, deep, 1);
	CHECK(d.row == BARNAV_MAX_ROWS + 1 && d.col == 0);
	CHECK(barNavInput(&d, K_RT, deep, 1, false) == BAR_EV_STEP_RIGHT);
}

// ── Priority, ties, and repeated presses in one frame ──────────────────────────────────────

static void testPriorityOneEventPerFrame(void)
{
	BarCatSpec cats[3] = { specInventory(), specCraft(), specChest() };
	BarNav n;

	// B is first, and B with a lift is CANCEL and never BACK — blueprint §1A.3 and §7 red arm 4.
	barNavReset(&n, 1, 2, 0);
	CHECK(barNavInput(&n, K_B, cats, 3, true) == BAR_EV_CANCEL);   // B while lifting is CANCEL
	CHECK(barNavInput(&n, K_B, cats, 3, false) == BAR_EV_BACK);
	// ...even with every other button on the same frame.
	const uint32_t all = K_A | K_B | K_X | K_Y | K_L | K_R | K_UP | K_DN | K_LT | K_RT;
	CHECK(barNavInput(&n, all, cats, 3, true) == BAR_EV_CANCEL);
	CHECK(barNavInput(&n, all, cats, 3, false) == BAR_EV_BACK);
	CHECK(n.cat == 1 && n.row == 2);                                // and nothing moved

	// A beats X and everything below it.
	CHECK(barNavInput(&n, K_A, cats, 3, false) == BAR_EV_COMMIT);
	CHECK(barNavInput(&n, K_A | K_X, cats, 3, false) == BAR_EV_COMMIT);
	CHECK(barNavInput(&n, K_A | K_R | K_DN, cats, 3, false) == BAR_EV_COMMIT);
	CHECK(n.cat == 1 && n.row == 2);
	// A is not a move: the same press repeated in the same or the next frame is COMMIT again
	// and the cursor never drifts. The caller de-bounces with hidKeysDown, not this module.
	for (int i = 0; i < 8; i++) {
		CHECK(barNavInput(&n, K_A, cats, 3, false) == BAR_EV_COMMIT);
		CHECK(n.cat == 1 && n.row == 2 && n.col == 0);
	}

	// X beats the d-pad.
	CHECK(barNavInput(&n, K_X, cats, 3, false) == BAR_EV_ALT);
	CHECK(barNavInput(&n, K_X | K_L | K_UP | K_LT, cats, 3, false) == BAR_EV_ALT);
	CHECK(n.cat == 1 && n.row == 2);

	// Y is defined so a caller can pass a whole word, and is claimed by nothing here.
	CHECK(barNavInput(&n, K_Y, cats, 3, false) == BAR_EV_NONE);
	CHECK(barNavInput(&n, K_Y | K_DN, cats, 3, false) == BAR_EV_MOVE);   // Y does not block

	// Shoulders beat the d-pad.
	barNavReset(&n, 0, 0, 0);
	CHECK(barNavInput(&n, K_R | K_DN | K_RT, cats, 3, false) == BAR_EV_CAT);
	CHECK(n.cat == 1 && n.row == 0);
	// Vertical beats horizontal.
	barNavReset(&n, 0, 0, 0);
	CHECK(barNavInput(&n, K_DN | K_RT, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.row == 1 && n.col == 0);
}

static void testOpposingPairsTieAndFallThrough(void)
{
	BarCatSpec cats[3] = { specInventory(), specCraft(), specChest() };
	BarNav n;

	// A tie moves nothing — the same answer scene/hotbar.c gives L and R together.
	barNavReset(&n, 1, 2, 0);
	CHECK(barNavInput(&n, K_L | K_R, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.cat == 1);
	barNavReset(&n, 0, 1, 3);
	CHECK(barNavInput(&n, K_UP | K_DN, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.row == 1);
	CHECK(barNavInput(&n, K_LT | K_RT, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.col == 3);

	// ...and a tie FALLS THROUGH to the next band, because a tie is the band declining to say
	// anything at all. L|R with down still moves a row.
	barNavReset(&n, 0, 0, 3);
	CHECK(barNavInput(&n, K_L | K_R | K_DN, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.row == 1 && n.cat == 0);
	barNavReset(&n, 0, 0, 3);
	CHECK(barNavInput(&n, K_UP | K_DN | K_RT, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.row == 0 && n.col == 4);
	barNavReset(&n, 0, 0, 3);
	CHECK(barNavInput(&n, K_L | K_R | K_UP | K_DN | K_LT, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.col == 2);
	// Every pair tied at once says nothing at all.
	barNavReset(&n, 0, 1, 3);
	CHECK(barNavInput(&n, K_L | K_R | K_UP | K_DN | K_LT | K_RT, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.cat == 0 && n.row == 1 && n.col == 3);

	// A PRESSED band consumes the frame even when it could not move: L at the leftmost tab is
	// NONE and does NOT hand the frame to the d-pad underneath it.
	barNavReset(&n, 0, 0, 0);
	CHECK(barNavInput(&n, K_L | K_DN, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.cat == 0 && n.row == 0);
	// Same at a column edge: right at col 7 does not fall through to a tab change.
	barNavReset(&n, 0, 0, 7);
	CHECK(barNavInput(&n, K_RT, cats, 3, false) == BAR_EV_NONE);
	CHECK(n.cat == 0 && n.col == 7);
}

static void testKeysZeroIsInert(void)
{
	// The paused frame: scene/ui.c hands in an empty word every frame while the pause menu owns
	// the pad, and this module has to be a no-op for as long as that lasts — no drift, no
	// remembered press that fires when the world comes back.
	BarCatSpec cats[3] = { specInventory(), specCraft(), specFurnace() };
	BarNav n;
	barNavReset(&n, 2, 1, 0);
	(void)barNavInput(&n, K_RT, cats, 3, false);
	(void)barNavInput(&n, K_RT, cats, 3, false);
	const BarNav before = n;
	CHECK(before.cat == 2 && before.row == 1 && before.col == 2);

	for (int f = 0; f < 200; f++) {
		CHECK(barNavInput(&n, 0u, cats, 3, (f & 1) != 0) == BAR_EV_NONE);
	}
	CHECK(memcmp(&n, &before, sizeof n) == 0);   // byte-identical after 200 empty frames

	// And the frame the pad comes back is an ordinary frame, with no backlog.
	CHECK(barNavInput(&n, K_UP, cats, 3, false) == BAR_EV_MOVE);
	CHECK(n.row == 0);

	// Undefined bits alone are inert too: a caller passing a whole hidKeysDown() word carries
	// bits this module names nothing for (KEY_START, KEY_SELECT, the touch bit).
	const BarNav mid = n;
	CHECK(barNavInput(&n, 0x08000000u, cats, 3, false) == BAR_EV_NONE);
	CHECK(memcmp(&n, &mid, sizeof n) == 0);
}

// ── The category list changing underneath the cursor ───────────────────────────────────────

static void testContainerTabDisappears(void)
{
	// The case blueprint §7 arm C2 describes on the device: the player is on the CHEST tab and
	// walks away, so the list drops from 3 categories to 2 between one frame and the next. The
	// very next barNavInput must not read a spec through a cat that no longer exists.
	BarCatSpec with_chest[3] = { specInventory(), specCraft(), specChest() };
	BarCatSpec without[2]    = { specInventory(), specCraft() };

	BarNav n;
	barNavReset(&n, 0, 0, 0);
	barNavSetCat(&n, 2, with_chest, 3);
	(void)barNavInput(&n, K_DN, with_chest, 3, false);
	(void)barNavInput(&n, K_RT, with_chest, 3, false);
	CHECK(n.cat == 2 && n.row == 1 && n.col == 1);

	// The chest is gone. The next frame's input normalises before it reads anything.
	CHECK(barNavInput(&n, K_DN, without, 2, false) == BAR_EV_MOVE);
	CHECK(n.cat == 0);
	checkInvariant(&n, without, 2);

	// Even a frame with no keys at all repairs it, so a caller that only draws is safe too.
	BarNav q;
	barNavReset(&q, 0, 0, 0);
	barNavSetCat(&q, 2, with_chest, 3);
	CHECK(q.cat == 2);
	CHECK(barNavInput(&q, 0u, without, 2, false) == BAR_EV_NONE);
	CHECK(q.cat == 0);
	checkInvariant(&q, without, 2);
}

// A long pseudo-random key walk over a changing category list. Nothing here checks a specific
// answer — it checks the ONE thing every caller relies on, that the cursor is on a real cell no
// matter what sequence arrives, which is blueprint §1A.3's "after any key sequence".
static void testFuzzKeepsTheInvariant(void)
{
	BarCatSpec cats[BARNAV_MAX_CATS] = {
		specInventory(), specCraft(), specChest(), specFurnace(), specOptions(), specInventory()
	};
	static const uint32_t bits[] = { K_A, K_B, K_X, K_Y, K_L, K_R, K_UP, K_DN, K_LT, K_RT };

	BarNav n;
	memset(&n, 0, sizeof n);
	uint32_t seed = 0x5EEDCAFEu;
	for (int i = 0; i < 200; i++) {
		seed = seed * 1664525u + 1013904223u;
		uint32_t keys = 0;
		// One or two bits a frame, so ties and priority collisions are both reached.
		keys |= bits[(seed >> 8) % 10];
		if ((seed >> 20) & 1) keys |= bits[(seed >> 12) % 10];
		const int count = 1 + (int)((seed >> 4) % BARNAV_MAX_CATS);
		(void)barNavInput(&n, keys, cats, count, ((seed >> 3) & 1) != 0);
		checkInvariant(&n, cats, count);
	}
}

// ── barNavScrollFor ────────────────────────────────────────────────────────────────────────

static void testScrollFollowsTheCursor(void)
{
	// A list that FITS never scrolls, whatever the caller is holding.
	for (int cur = 0; cur < 7; cur++) {
		CHECK(barNavScrollFor(cur, 0, 7, 7) == 0);
		CHECK(barNavScrollFor(cur, 3, 7, 7) == 0);
		CHECK(barNavScrollFor(cur, 3, 7, 5) == 0);
	}

	// A list that does not fit: 12 rows in a 7-row window, so the offset lives in [0, 5].
	const int rows = 12, vis = 7, maxs = rows - vis;

	// Walking DOWN from the top: the window does not move until the cursor leaves it, then it
	// follows one row at a time. That is the "smallest change" rule — a recentre here would
	// throw away five rows of context on every step.
	int s = 0;
	for (int cur = 0; cur < rows; cur++) {
		s = barNavScrollFor(cur, s, vis, rows);
		CHECK(s == (cur < vis ? 0 : cur - vis + 1));
		CHECK(cur >= s && cur < s + vis);         // the cursor is on screen, every step
		CHECK(s >= 0 && s <= maxs);
	}
	CHECK(s == maxs);

	// ...and back UP: it holds until the cursor leaves the top of the window, then follows.
	for (int cur = rows - 1; cur >= 0; cur--) {
		s = barNavScrollFor(cur, s, vis, rows);
		CHECK(s == (cur >= maxs ? maxs : cur));
		CHECK(cur >= s && cur < s + vis);
		CHECK(s >= 0 && s <= maxs);
	}
	CHECK(s == 0);

	// A jump straight to the far end from anywhere lands inside the range, on the first step.
	for (int from = 0; from <= maxs; from++) {
		CHECK(barNavScrollFor(rows - 1, from, vis, rows) == maxs);
		CHECK(barNavScrollFor(0, from, vis, rows) == 0);
	}

	// Nonsense in, 0 out — never a negative offset and never one past the end.
	CHECK(barNavScrollFor(0, 0, 0, 12) == 0);
	CHECK(barNavScrollFor(0, 0, -3, 12) == 0);
	CHECK(barNavScrollFor(0, 0, 7, 0) == 0);
	CHECK(barNavScrollFor(0, 0, 7, -2) == 0);
	CHECK(barNavScrollFor(-5, 4, vis, rows) == 0);          // a cursor below the list
	CHECK(barNavScrollFor(999, 0, vis, rows) == maxs);      // and above it
	CHECK(barNavScrollFor(3, -9, vis, rows) == 0);          // a scroll below the range
	CHECK(barNavScrollFor(3, 99, vis, rows) == 3);          // and above it, pulled to the cursor
	CHECK(barNavScrollFor(8, 99, vis, rows) == maxs);

	// Every (cursor, scroll) pair at this size: the answer is always in range and always shows
	// the cursor. A sweep, not three named points, because the clamp order is what is under
	// test and an off-by-one hides between named points.
	for (int cur = 0; cur < rows; cur++) {
		for (int from = -2; from <= maxs + 2; from++) {
			const int got = barNavScrollFor(cur, from, vis, rows);
			CHECK(got >= 0 && got <= maxs);
			CHECK(cur >= got && cur < got + vis);
		}
	}

	// A window of one row: the offset IS the cursor.
	for (int cur = 0; cur < rows; cur++) CHECK(barNavScrollFor(cur, 0, 1, rows) == cur);
}

// ── barStickEdge ───────────────────────────────────────────────────────────────────────────

static void testStickFiresOnceAndReArms(void)
{
	BarStick s;
	memset(&s, 0, sizeof s);

	// A deflection fires ONCE, however long it is held. This is the whole point: a stick held
	// over is one press, not one a frame.
	CHECK(barStickEdge(&s, 100, 0) == BAR_KEY_DRIGHT);
	for (int f = 0; f < 60; f++) CHECK(barStickEdge(&s, 100, 0) == 0);
	CHECK(barStickEdge(&s, 156, 0) == 0);              // harder is not a new press

	// The hysteresis gap: back inside the fire threshold but not past the re-arm one, so it
	// stays armed and a return to full deflection fires nothing.
	CHECK(barStickEdge(&s, 30, 0) == 0);
	CHECK(barStickEdge(&s, 100, 0) == 0);
	CHECK(barStickEdge(&s, BARNAV_STICK_REARM, 0) == 0);       // exactly at re-arm: still armed
	CHECK(barStickEdge(&s, 100, 0) == 0);

	// Below the re-arm threshold it disarms, and the next deflection is a fresh press.
	CHECK(barStickEdge(&s, BARNAV_STICK_REARM - 1, 0) == 0);
	CHECK(barStickEdge(&s, 100, 0) == BAR_KEY_DRIGHT);
	CHECK(barStickEdge(&s, 0, 0) == 0);
	CHECK(barStickEdge(&s, 100, 0) == BAR_KEY_DRIGHT);

	// The exact thresholds, so a "feel" edit is a deliberate edit.
	CHECK(BARNAV_STICK_FIRE == 40);
	CHECK(BARNAV_STICK_REARM == 24);
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, BARNAV_STICK_FIRE - 1, 0) == 0);    // one short of firing
	CHECK(barStickEdge(&s, BARNAV_STICK_FIRE, 0) == BAR_KEY_DRIGHT);
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, -(BARNAV_STICK_FIRE - 1), 0) == 0);
	CHECK(barStickEdge(&s, -BARNAV_STICK_FIRE, 0) == BAR_KEY_DLEFT);

	// Both signs on both axes, from a clean state each time. dy is libctru's, positive UP.
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, -100, 0) == BAR_KEY_DLEFT);
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, 0, 100) == BAR_KEY_DUP);
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, 0, -100) == BAR_KEY_DDOWN);

	// A flick straight across without passing through the middle is a NEW deflection: the
	// direction changed, which is the thing the edge is about.
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, 100, 0) == BAR_KEY_DRIGHT);
	CHECK(barStickEdge(&s, -100, 0) == BAR_KEY_DLEFT);
	CHECK(barStickEdge(&s, -100, 0) == 0);
	CHECK(barStickEdge(&s, 100, 0) == BAR_KEY_DRIGHT);

	// Diagonals fire both axes on one frame; the axes are independent state.
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, 100, 100) == (BAR_KEY_DRIGHT | BAR_KEY_DUP));
	CHECK(barStickEdge(&s, 100, 100) == 0);
	CHECK(barStickEdge(&s, 0, 100) == 0);                      // x disarms, y still held
	CHECK(barStickEdge(&s, 100, 100) == BAR_KEY_DRIGHT);       // only x fires again
	CHECK(barStickEdge(&s, 100, -100) == BAR_KEY_DDOWN);       // only y flips
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, -100, -100) == (BAR_KEY_DLEFT | BAR_KEY_DDOWN));

	// Held at the fire threshold across a whole sweep of readings: exactly one edge per
	// crossing of the dead band, at every magnitude the pad can report.
	memset(&s, 0, sizeof s);
	int edges = 0;
	for (int v = 0; v <= 156; v++) if (barStickEdge(&s, v, 0)) edges++;
	CHECK(edges == 1);                                         // one, at v == 40
	for (int v = 156; v >= 0; v--) if (barStickEdge(&s, v, 0)) edges++;
	CHECK(edges == 1);                                         // coming back down fires nothing
	CHECK(s.dir_x == 0);                                       // and it ended disarmed

	// An extreme reading is not a crash: negating INT_MIN would be undefined, so the module
	// must not do it. A caller passing a raw reading is the case this defends.
	memset(&s, 0, sizeof s);
	CHECK(barStickEdge(&s, INT_MIN, INT_MAX) == (BAR_KEY_DLEFT | BAR_KEY_DUP));
	CHECK(barStickEdge(&s, INT_MIN, INT_MAX) == 0);
}

// The join scene/main.c makes at seam S2: the stick's edges are OR-ed into the frame's `down`
// word, so the pad and the d-pad drive the SAME paths. Proved here rather than assumed, because
// a stick that produced bits barNavInput does not read would be silent on the device with every
// host test green.
static void testStickDrivesTheSamePaths(void)
{
	BarCatSpec cats[2] = { specInventory(), specCraft() };
	BarNav n;
	barNavReset(&n, 0, 0, 0);
	BarStick s;
	memset(&s, 0, sizeof s);

	const uint32_t down = barStickEdge(&s, 0, -100);            // stick pushed DOWN
	CHECK(down == BAR_KEY_DDOWN);
	CHECK(barNavInput(&n, 0u | down, cats, 2, false) == BAR_EV_MOVE);
	CHECK(n.row == 1);
	// Held: no second edge, so no second row.
	CHECK(barStickEdge(&s, 0, -100) == 0);
	CHECK(barNavInput(&n, 0u, cats, 2, false) == BAR_EV_NONE);
	CHECK(n.row == 1);
	// Released and pushed again: one more row.
	CHECK(barStickEdge(&s, 0, 0) == 0);
	const uint32_t again = barStickEdge(&s, 0, -100);
	CHECK(barNavInput(&n, again, cats, 2, false) == BAR_EV_MOVE);
	CHECK(n.row == 2);
	// Right: a grid move, exactly as the d-pad's DRIGHT.
	const uint32_t right = barStickEdge(&s, 120, 0);
	CHECK(right == BAR_KEY_DRIGHT);
	CHECK(barNavInput(&n, right, cats, 2, false) == BAR_EV_MOVE);
	CHECK(n.col == 1 && n.pref_col == 1);
}

int main(void)
{
	testZeroStateIsValidAndInert();
	testResetAndNulls();
	testClampEveryCatCount();
	testClampRowAndColRanges();
	testRowWrapEveryEdge();
	testPrefColAcrossNarrowRow();
	testCatStepClampsAndRemembers();
	testSetCatAndSetCell();
	testHorizontalIsContentGrid();
	testHorizontalIsCatOnAList();
	testHorizontalIsStepOnAStepper();
	testPriorityOneEventPerFrame();
	testOpposingPairsTieAndFallThrough();
	testKeysZeroIsInert();
	testContainerTabDisappears();
	testFuzzKeepsTheInvariant();
	testScrollFollowsTheCursor();
	testStickFiresOnceAndReArms();
	testStickDrivesTheSamePaths();

	CHECK(s_checks == BARNAV_TEST_EXPECTED_CHECKS);

	if (s_fails) {
		printf("barnav self-test: FAILED - %d of %d checks, first: %s\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("barnav self-test: PASS  %d checks\n", s_checks);
	return 0;
}
