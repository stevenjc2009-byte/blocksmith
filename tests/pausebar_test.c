// Host gate for scene/pausebar.c — the v1.9.1 three-tab pause bar (blueprint lane D, D9).
//
// WHY THIS FILE LINKS THE REAL MODULE. Everything that can be WRONG about the pause panel —
// which tab and row the cursor lands on after any key or stylus press, which action that
// produces, whether the three tabs tile the panel without a seam, whether a tap on a stepper
// arrow steps the right direction — lives in scene/pausebar.c, which has no <3ds.h> in it
// precisely so this binary can link THAT module. Nothing here is a copy of the code under
// test: the key bits, the font advance and the geometry constants are the header's own.
//
// WHAT THIS FILE CANNOT PROVE, stated up front. It cannot prove scene/pausemenu.c calls
// pauseBarKey()/pauseBarTouch() from the frame loop with the real words, that main.c calls
// pauseMenuTouch() before pauseMenuInput() (the blueprint's seam S7 and its grep in §1A.4),
// that the actions reach genSetRadius/gfxSet3D/audioSetMasterVolume, or that a single pixel
// of the panel is drawn where these rects say it is. Those are the console's, and the draw
// is a VISUAL claim that needs a photograph of a bottom screen.
//
// Own main(), same CHECK macro and PASS/FAIL line as tests/hotbar_test.c.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene/pausebar.h"

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

// Pinned, so a check that quietly stops being reached reads as a FAIL rather than as a
// shorter green run. Move it only when a check is deliberately added or removed, and say so.
// 34083 on the first green run. The bulk of it is three exhaustive sweeps rather than three
// hundred hand-written cases: every pixel of the tab strip and its three-pixel margin
// (testStripTilesThePanelExactly), a coarse grid over the whole panel for the rows, and a
// 7x5-pixel grid over the whole 320x240 bottom screen on each of the three tabs
// (testTouchOnRowsAndArrows). Those sweeps are what turn "the hit tests agree with the
// rects" from a spot check into a proof, and they are cheap: the binary runs in well under
// a second.
#define PAUSEBAR_EXPECTED_CHECKS 34083

#define A  PAUSEBAR_KEY_A
#define B  PAUSEBAR_KEY_B
#define L  PAUSEBAR_KEY_L
#define R  PAUSEBAR_KEY_R
#define UP PAUSEBAR_KEY_DUP
#define DN PAUSEBAR_KEY_DDOWN
#define LT PAUSEBAR_KEY_DLEFT
#define RT PAUSEBAR_KEY_DRIGHT

// An open panel sitting on GAME row 0 — the state every key test starts from.
static PauseBarState opened(void)
{
	PauseBarState s;
	pauseBarReset(&s);
	pauseBarToggle(&s);
	return s;
}

// The centre of a rect, which is the point a synthetic tap should use: an origin lands on the
// half-open left/top edge and a bottom-right corner lands OUTSIDE, so a round trip written
// against either would pass or fail for the wrong reason.
static int midX(PbRect r) { return r.x + r.w / 2; }
static int midY(PbRect r) { return r.y + r.h / 2; }

// ── The zero state ─────────────────────────────────────────────────────────────────────

static void testZeroStateIsValidAndInert(void)
{
	// A memset-zero PauseBarState is the state a plain `static PauseBarState s_bar;` gives,
	// so it has to be legal without anyone having called pauseBarReset().
	PauseBarState s;
	memset(&s, 0, sizeof s);

	CHECK(pauseBarOpen(&s) == false);
	CHECK(s.tab == PAUSEBAR_TAB_GAME);
	CHECK(s.row == 0);

	// Closed: every key and every tap is consumed by nobody and changes nothing.
	static const uint32_t every_bit[] = { A, B, L, R, UP, DN, LT, RT };
	for (unsigned i = 0; i < sizeof every_bit / sizeof every_bit[0]; i++) {
		const PauseBarAction act = pauseBarKey(&s, every_bit[i]);
		CHECK(act.kind == PAUSEBAR_ACT_NONE);
		CHECK(act.arg == 0);
		CHECK(pauseBarOpen(&s) == false);
	}
	CHECK(pauseBarKey(&s, A | B | L | R | UP | DN | LT | RT).kind == PAUSEBAR_ACT_NONE);
	CHECK(pauseBarKey(&s, 0u).kind == PAUSEBAR_ACT_NONE);

	// A tap anywhere on the panel while closed does nothing either — pauseMenuTouch is called
	// unconditionally by the caller, so this is the ordinary case, not a corner.
	for (int y = 0; y < PB_SCR_H; y += 7)
		for (int x = 0; x < PB_SCR_W; x += 11)
			CHECK(pauseBarTouch(&s, true, x, y).kind == PAUSEBAR_ACT_NONE);
	CHECK(pauseBarOpen(&s) == false);

	// Still all-zero afterwards: nothing wrote a field on the way through.
	PauseBarState zero;
	memset(&zero, 0, sizeof zero);
	CHECK(memcmp(&s, &zero, sizeof s) == 0);

	// NULL is refused everywhere rather than dereferenced.
	CHECK(pauseBarOpen(NULL) == false);
	CHECK(pauseBarKey(NULL, A).kind == PAUSEBAR_ACT_NONE);
	CHECK(pauseBarTouch(NULL, true, 100, 100).kind == PAUSEBAR_ACT_NONE);
	pauseBarReset(NULL);
	pauseBarClose(NULL);
	pauseBarToggle(NULL);
	pauseBarClamp(NULL);
	CHECK(true);   // reached: every NULL was refused, not dereferenced

	// The size pin, restated here as well as in pausebar.c, so a member that grows fails a
	// TEST and not only a build. 6 bytes: bool + int8 + int8 + uint8[3], no enum in sight.
	CHECK(sizeof(PauseBarState) == 6);
}

// ── Toggle, close, and what reopening forgets ──────────────────────────────────────────

static void testToggleAndClose(void)
{
	PauseBarState s;
	pauseBarReset(&s);
	CHECK(pauseBarOpen(&s) == false);
	pauseBarToggle(&s);
	CHECK(pauseBarOpen(&s) == true);
	CHECK(s.tab == PAUSEBAR_TAB_GAME);
	CHECK(s.row == 0);
	pauseBarToggle(&s);
	CHECK(pauseBarOpen(&s) == false);

	// Reopening always lands on GAME row 0, however deep the last visit went — the rule
	// scene/pausemenu.h's pauseMenuToggle already states, carried onto the tabs.
	PauseBarState t = opened();
	CHECK(pauseBarKey(&t, R).kind == PAUSEBAR_ACT_TAB);
	CHECK(pauseBarKey(&t, DN).kind == PAUSEBAR_ACT_MOVE);
	CHECK(t.tab == PAUSEBAR_TAB_OPTIONS);
	CHECK(t.row == PB_OPT_3D);
	pauseBarToggle(&t);
	CHECK(pauseBarOpen(&t) == false);
	pauseBarToggle(&t);
	CHECK(pauseBarOpen(&t) == true);
	CHECK(t.tab == PAUSEBAR_TAB_GAME);
	CHECK(t.row == 0);
	// ...and the per-tab memory went with it: it is a memory of THIS visit, not the last one.
	for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++) CHECK(t.row_mem[i] == 0);

	// pauseBarClose leaves the same all-zero state, from any depth.
	PauseBarState c = opened();
	(void)pauseBarKey(&c, R);
	(void)pauseBarKey(&c, R);
	(void)pauseBarKey(&c, DN);
	pauseBarClose(&c);
	PauseBarState zero;
	memset(&zero, 0, sizeof zero);
	CHECK(memcmp(&c, &zero, sizeof c) == 0);
}

// ── Tables, including every out-of-range index ─────────────────────────────────────────

static void testTablesRefuseOutOfRange(void)
{
	CHECK(pauseBarTabRows(PAUSEBAR_TAB_GAME) == PB_GAME_ROWS);
	CHECK(pauseBarTabRows(PAUSEBAR_TAB_OPTIONS) == PB_OPT_ROWS);
	CHECK(pauseBarTabRows(PAUSEBAR_TAB_SYSTEM) == PB_SYS_ROWS);
	CHECK(PB_GAME_ROWS == 2);
	CHECK(PB_OPT_ROWS == 3);
	CHECK(PB_SYS_ROWS == 2);
	CHECK(PAUSEBAR_MAX_ROWS == 3);
	CHECK(PAUSEBAR_TAB_COUNT == 3);

	CHECK(strcmp(pauseBarTabLabel(PAUSEBAR_TAB_GAME), "GAME") == 0);
	CHECK(strcmp(pauseBarTabLabel(PAUSEBAR_TAB_OPTIONS), "OPTIONS") == 0);
	CHECK(strcmp(pauseBarTabLabel(PAUSEBAR_TAB_SYSTEM), "SYSTEM") == 0);

	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_GAME, PB_GAME_RESUME), "Resume") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_GAME, PB_GAME_QUIT), "Quit to title") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_OPTIONS, PB_OPT_DIST), "Render dist") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_OPTIONS, PB_OPT_3D), "3D") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_OPTIONS, PB_OPT_VOLUME), "Sound") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_SYSTEM, PB_SYS_CONTROLS), "Controls") == 0);
	CHECK(strcmp(pauseBarRowLabel(PAUSEBAR_TAB_SYSTEM, PB_SYS_DEBUG), "Debug") == 0);

	// Only OPTIONS steps its rows; only SYSTEM shows the memory readout. Both are checked
	// over EVERY tab, so "return true" and "return false" are each a red arm.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		CHECK(pauseBarHorizontalIsContent(t) == (t == PAUSEBAR_TAB_OPTIONS));
		CHECK(pauseBarTabShowsMemory(t) == (t == PAUSEBAR_TAB_SYSTEM));
		for (int r = 0; r < PAUSEBAR_MAX_ROWS; r++)
			CHECK(pauseBarRowIsStepper(t, r) ==
			      (t == PAUSEBAR_TAB_OPTIONS && r < PB_OPT_ROWS));
	}

	// Out of range, in every direction, including the values a corrupt int8_t can hold.
	static const int bad[] = { -128, -32, -2, -1, 3, 4, 7, 64, 127, 1000 };
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		const int t = bad[i];
		CHECK(pauseBarTabRows(t) == 0);
		CHECK(pauseBarTabLabel(t) != NULL);
		CHECK(pauseBarTabLabel(t)[0] == '\0');
		CHECK(pauseBarHorizontalIsContent(t) == false);
		CHECK(pauseBarTabShowsMemory(t) == false);
		CHECK(pauseBarTabRect(t).w == 0);
		CHECK(pauseBarTabRect(t).h == 0);
		CHECK(pauseBarRowRect(t).w == 0);
		CHECK(pauseBarHitRow(t, 100, PB_ROW_Y0 + 4) == PAUSEBAR_HIT_NONE);
		for (int r = -3; r < 6; r++) {
			CHECK(pauseBarRowLabel(t, r)[0] == '\0');
			CHECK(pauseBarRowIsStepper(t, r) == false);
			CHECK(pauseBarArrowRect(t, r, PB_ARROW_LEFT).w == 0);
			CHECK(pauseBarArrowRect(t, r, PB_ARROW_RIGHT).w == 0);
		}
	}
	// A row past the end of a REAL tab is refused just as firmly as a bad tab is.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		CHECK(pauseBarRowLabel(t, rows)[0] == '\0');
		CHECK(pauseBarRowLabel(t, -1)[0] == '\0');
		CHECK(pauseBarRowLabel(t, 99)[0] == '\0');
	}
	// A direction that is neither arrow gets no box, on a row that really does have two.
	static const int bad_dir[] = { 0, -2, 2, 100 };
	for (unsigned i = 0; i < sizeof bad_dir / sizeof bad_dir[0]; i++)
		CHECK(pauseBarArrowRect(PAUSEBAR_TAB_OPTIONS, PB_OPT_DIST, bad_dir[i]).w == 0);
}

// ── Geometry ───────────────────────────────────────────────────────────────────────────

static void testStripTilesThePanelExactly(void)
{
	const PbRect panel = pauseBarPanelRect();
	const PbRect strip = pauseBarStripRect();
	CHECK(panel.x == 26 && panel.y == 24 && panel.w == 268 && panel.h == 192);
	CHECK(strip.x == panel.x && strip.w == panel.w);
	CHECK(strip.y == PB_STRIP_Y && strip.h == PB_STRIP_H);
	CHECK(strip.y >= panel.y);
	CHECK(strip.y + strip.h <= panel.y + panel.h);

	// The three tabs tile [panel.x, panel.x + panel.w) EXACTLY: tab 0 starts on the panel's
	// left edge, each tab begins where the last ended, and the third ends on the panel's
	// right edge. A fixed PB_PANEL_W / 3 would truncate 89.33 to 89 and leave a one-pixel
	// column at the right that no tab claims.
	int cursor = panel.x;
	for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++) {
		const PbRect t = pauseBarTabRect(i);
		CHECK(t.x == cursor);
		CHECK(t.y == strip.y);
		CHECK(t.h == strip.h);
		CHECK(t.w > 0);
		cursor += t.w;
	}
	CHECK(cursor == panel.x + panel.w);
	// 89 / 89 / 90, named, because the blueprint says "three 89-px tabs" and the leftover
	// pixel has to belong to a tab rather than to a gap.
	CHECK(pauseBarTabRect(0).w == 89);
	CHECK(pauseBarTabRect(1).w == 89);
	CHECK(pauseBarTabRect(2).w == 90);

	// Every pixel of the strip belongs to exactly one tab, and no pixel outside it to any —
	// and pauseBarHitTab agrees with the rects about which one, at every single pixel.
	for (int y = strip.y - 3; y < strip.y + strip.h + 3; y++) {
		for (int x = panel.x - 3; x < panel.x + panel.w + 3; x++) {
			int hits = 0, which = PAUSEBAR_HIT_NONE;
			for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++)
				if (pbPtInRect(pauseBarTabRect(i), x, y)) { hits++; which = i; }
			const bool inside = (y >= strip.y && y < strip.y + strip.h &&
			                     x >= panel.x && x < panel.x + panel.w);
			CHECK(hits == (inside ? 1 : 0));
			CHECK(pauseBarHitTab(x, y) == which);
		}
	}

	// Round trip: the centre of every tab hits that tab and no other.
	for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++) {
		const PbRect t = pauseBarTabRect(i);
		CHECK(pauseBarHitTab(midX(t), midY(t)) == i);
		CHECK(pauseBarHitTab(t.x, t.y) == i);                       // top-left is inside
		CHECK(pauseBarHitTab(t.x + t.w - 1, t.y + t.h - 1) == i);   // bottom-right is inside
		if (i > 0) CHECK(pauseBarHitTab(t.x - 1, midY(t)) == i - 1);   // the seam belongs left
	}
	// One row above and one row below the strip is nobody's.
	for (int x = 0; x < PB_SCR_W; x++) {
		CHECK(pauseBarHitTab(x, strip.y - 1) == PAUSEBAR_HIT_NONE);
		CHECK(pauseBarHitTab(x, strip.y + strip.h) == PAUSEBAR_HIT_NONE);
	}
}

static void testRowsAreDisjointAndInsideThePanel(void)
{
	const PbRect panel = pauseBarPanelRect();
	const PbRect strip = pauseBarStripRect();

	for (int r = 0; r < PAUSEBAR_MAX_ROWS; r++) {
		const PbRect row = pauseBarRowRect(r);
		CHECK(row.x == PB_ROW_X);
		CHECK(row.w == PB_ROW_W);
		CHECK(row.h == PB_ROW_H - 2);
		CHECK(row.y == PB_ROW_Y0 + r * PB_ROW_H);
		// Inside the panel, and clear of the strip above and the footer below.
		CHECK(row.x >= panel.x);
		CHECK(row.x + row.w <= panel.x + panel.w);
		CHECK(row.y >= strip.y + strip.h);
		CHECK(row.y + row.h <= PB_FOOTER_Y);
		// Disjoint from every other row, with the 2 px gap between them.
		for (int q = 0; q < PAUSEBAR_MAX_ROWS; q++) {
			if (q == r) continue;
			const PbRect other = pauseBarRowRect(q);
			const bool overlap = !(row.y + row.h <= other.y || other.y + other.h <= row.y);
			CHECK(overlap == false);
		}
		if (r > 0) CHECK(row.y - (pauseBarRowRect(r - 1).y + pauseBarRowRect(r - 1).h) == 2);
	}
	// The memory readout's band clears SYSTEM's two rows and the footer, which is the whole
	// reason it is drawn on SYSTEM and nowhere else.
	CHECK(PB_MEM_Y0 >= pauseBarRowRect(PB_SYS_ROWS - 1).y + pauseBarRowRect(PB_SYS_ROWS - 1).h);
	CHECK(PB_MEM_Y0 + (PB_MEM_ROWS - 1) * PB_MEM_STEP + PAUSEBAR_FONT_GLYPH_H < PB_FOOTER_Y);
	CHECK(PB_FOOTER_Y + PAUSEBAR_FONT_GLYPH_H <= panel.y + panel.h);

	// hitRow answers a row only where that tab HAS one: a tap in GAME's empty third band
	// must do nothing rather than commit a row the player cannot see.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		for (int r = 0; r < PAUSEBAR_MAX_ROWS; r++) {
			const PbRect row = pauseBarRowRect(r);
			const int hit = pauseBarHitRow(t, midX(row), midY(row));
			CHECK(hit == ((r < rows) ? r : PAUSEBAR_HIT_NONE));
		}
		// The gap between two rows belongs to neither.
		for (int r = 1; r < rows; r++) {
			const PbRect row = pauseBarRowRect(r);
			CHECK(pauseBarHitRow(t, midX(row), row.y - 1) == PAUSEBAR_HIT_NONE);
		}
		// Left and right of the row band, and above the first row.
		CHECK(pauseBarHitRow(t, PB_ROW_X - 1, PB_ROW_Y0 + 4) == PAUSEBAR_HIT_NONE);
		CHECK(pauseBarHitRow(t, PB_ROW_X + PB_ROW_W, PB_ROW_Y0 + 4) == PAUSEBAR_HIT_NONE);
		CHECK(pauseBarHitRow(t, midX(pauseBarRowRect(0)), PB_ROW_Y0 - 1) == PAUSEBAR_HIT_NONE);
	}

	// A full sweep of the panel: every point answers at most one row, and a point that
	// answers a row is inside that row's rect.
	for (int y = panel.y; y < panel.y + panel.h; y += 3) {
		for (int x = panel.x; x < panel.x + panel.w; x += 9) {
			const int hit = pauseBarHitRow(PAUSEBAR_TAB_OPTIONS, x, y);
			if (hit == PAUSEBAR_HIT_NONE) continue;
			CHECK(hit >= 0 && hit < PB_OPT_ROWS);
			CHECK(pbPtInRect(pauseBarRowRect(hit), x, y));
		}
	}
}

static void testArrowBoxesSitInsideTheirRow(void)
{
	for (int r = 0; r < PB_OPT_ROWS; r++) {
		const PbRect row = pauseBarRowRect(r);
		const PbRect lft = pauseBarArrowRect(PAUSEBAR_TAB_OPTIONS, r, PB_ARROW_LEFT);
		const PbRect rgt = pauseBarArrowRect(PAUSEBAR_TAB_OPTIONS, r, PB_ARROW_RIGHT);
		CHECK(lft.w == PB_ARROW_W && rgt.w == PB_ARROW_W);
		CHECK(lft.h == row.h && rgt.h == row.h);
		CHECK(lft.y == row.y && rgt.y == row.y);
		CHECK(lft.x == PB_ARROW_L_X && rgt.x == PB_ARROW_R_X);
		// Inside the row, in order, and not touching each other.
		CHECK(lft.x >= row.x);
		CHECK(lft.x + lft.w <= rgt.x);
		CHECK(rgt.x + rgt.w <= row.x + row.w);
		// The value field between them is at least five characters wide at scale 1, so
		// "100%" and a three-digit render distance both fit without moving the arrows.
		CHECK(rgt.x - (lft.x + lft.w) >= 5 * PAUSEBAR_FONT_ADVANCE);
		CHECK(PB_VALUE_X == lft.x + lft.w);
		// Both boxes are real stylus targets, not glyph-sized.
		CHECK(lft.w >= 20 && lft.h >= 18);
	}
	// A non-stepper row has neither box, on either tab that has one.
	for (int r = 0; r < PAUSEBAR_MAX_ROWS; r++) {
		CHECK(pauseBarArrowRect(PAUSEBAR_TAB_GAME, r, PB_ARROW_LEFT).w == 0);
		CHECK(pauseBarArrowRect(PAUSEBAR_TAB_SYSTEM, r, PB_ARROW_RIGHT).w == 0);
	}
}

static void testLabelsFitTheirBoxes(void)
{
	// The font is 5x7 at integer scales only, advance 6 — so a label's width is a fact, not
	// an estimate, and every one of them is checked against the box it is drawn in.
	CHECK(pauseBarTextWidth("", 1) == 0);
	CHECK(pauseBarTextWidth("A", 1) == PAUSEBAR_FONT_ADVANCE);
	CHECK(pauseBarTextWidth("A", 2) == 2 * PAUSEBAR_FONT_ADVANCE);
	CHECK(pauseBarTextWidth(NULL, 1) == 0);
	CHECK(pauseBarTextWidth("A", 0) == 0);
	CHECK(pauseBarTextWidth("A", -1) == 0);

	// Tab labels, at scale 1, inside their own tab with room for centring.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int w = pauseBarTextWidth(pauseBarTabLabel(t), 1);
		CHECK(w <= pauseBarTabRect(t).w);
		CHECK(w > 0);
	}
	CHECK(pauseBarTextWidth("OPTIONS", 1) == 42);   // the widest tab label
	// Scale 2 is what the blueprint's D4 rules OUT for a focused tab, and this is the number
	// that rules it out: 84 px would still fit an 89 px tab, but "INVENTORY" at 108 does not
	// fit the in-game strip's 98, and a tab that changes width on focus moves its
	// neighbours' touch targets. Recorded so the decision is not re-opened from memory.
	CHECK(pauseBarTextWidth("OPTIONS", 2) == 84);

	// Row labels fit the value field's left edge, so no label can run under an arrow.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		for (int r = 0; r < rows; r++) {
			const int w = pauseBarTextWidth(pauseBarRowLabel(t, r), 1);
			CHECK(w > 0);
			CHECK(PB_ROW_X + 6 + w <= PB_ARROW_L_X);
		}
	}
	CHECK(pauseBarTextWidth("Quit to title", 1) == 78);   // the widest row label

	// The footer is the widest thing on the panel, and it fits the panel's width from the
	// row inset. 27 characters at advance 6.
	CHECK(strcmp(pauseBarFooterLabel(), "A SELECT  B RESUME  L/R TAB") == 0);
	CHECK(pauseBarTextWidth(pauseBarFooterLabel(), 1) == PAUSEBAR_WIDEST_LABEL_PX);
	CHECK(PAUSEBAR_WIDEST_LABEL_PX == 162);
	CHECK(PB_ROW_X + PAUSEBAR_WIDEST_LABEL_PX <= PB_PANEL_X + PB_PANEL_W);
	// Nothing else this module names is wider than the footer.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		CHECK(pauseBarTextWidth(pauseBarTabLabel(t), 1) <= PAUSEBAR_WIDEST_LABEL_PX);
		for (int r = 0; r < PAUSEBAR_MAX_ROWS; r++)
			CHECK(pauseBarTextWidth(pauseBarRowLabel(t, r), 1) <= PAUSEBAR_WIDEST_LABEL_PX);
	}
	// The character budget: at the row inset, the panel's remaining width is 42 characters
	// at scale 1. The footer spends 27 of them.
	CHECK((PB_PANEL_X + PB_PANEL_W - PB_ROW_X) / PAUSEBAR_FONT_ADVANCE == 42);
}

// ── Rows: every wrap edge ──────────────────────────────────────────────────────────────

static void testRowWrapsOnEveryTab(void)
{
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		for (int start = 0; start < rows; start++) {
			// Down from every row, including the last (which wraps to 0).
			PauseBarState s = opened();
			s.tab = (int8_t)t;
			s.row = (int8_t)start;
			PauseBarAction act = pauseBarKey(&s, DN);
			CHECK(act.kind == PAUSEBAR_ACT_MOVE);
			CHECK(s.row == (start + 1) % rows);
			CHECK(act.arg == s.row);
			CHECK(s.tab == t);          // a row move never changes the tab

			// Up from every row, including row 0 (which wraps to the last).
			s.row = (int8_t)start;
			act = pauseBarKey(&s, UP);
			CHECK(act.kind == PAUSEBAR_ACT_MOVE);
			CHECK(s.row == (start + rows - 1) % rows);
			CHECK(act.arg == s.row);
			CHECK(s.tab == t);
		}
		// A full lap either way lands home, on every tab.
		PauseBarState s = opened();
		s.tab = (int8_t)t;
		s.row = 0;
		for (int i = 0; i < rows; i++) (void)pauseBarKey(&s, DN);
		CHECK(s.row == 0);
		for (int i = 0; i < rows; i++) (void)pauseBarKey(&s, UP);
		CHECK(s.row == 0);
		// Two full laps, to catch a wrap that only works once.
		for (int i = 0; i < 2 * rows; i++) (void)pauseBarKey(&s, DN);
		CHECK(s.row == 0);
	}
}

// ── Tabs: clamped, never wrapping, and remembering their row ───────────────────────────

static void testTabsClampAndNeverWrap(void)
{
	PauseBarState s = opened();
	CHECK(s.tab == PAUSEBAR_TAB_GAME);

	// L on the first tab does nothing at all — no action, no move. This is the edge that
	// separates "clamped" from "wrapping": a wrap would land on SYSTEM.
	PauseBarAction act = pauseBarKey(&s, L);
	CHECK(act.kind == PAUSEBAR_ACT_NONE);
	CHECK(s.tab == PAUSEBAR_TAB_GAME);
	for (int i = 0; i < 5; i++) {
		CHECK(pauseBarKey(&s, L).kind == PAUSEBAR_ACT_NONE);
		CHECK(s.tab == PAUSEBAR_TAB_GAME);
	}

	// R walks to the end and then stops there.
	act = pauseBarKey(&s, R);
	CHECK(act.kind == PAUSEBAR_ACT_TAB);
	CHECK(act.arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(s.tab == PAUSEBAR_TAB_OPTIONS);
	act = pauseBarKey(&s, R);
	CHECK(act.kind == PAUSEBAR_ACT_TAB);
	CHECK(act.arg == PAUSEBAR_TAB_SYSTEM);
	CHECK(s.tab == PAUSEBAR_TAB_SYSTEM);
	for (int i = 0; i < 5; i++) {
		CHECK(pauseBarKey(&s, R).kind == PAUSEBAR_ACT_NONE);
		CHECK(s.tab == PAUSEBAR_TAB_SYSTEM);
	}
	// ...and back down the other way.
	CHECK(pauseBarKey(&s, L).arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(pauseBarKey(&s, L).arg == PAUSEBAR_TAB_GAME);
	CHECK(pauseBarKey(&s, L).kind == PAUSEBAR_ACT_NONE);
}

static void testTabsRememberTheirRow(void)
{
	PauseBarState s = opened();
	// GAME: leave it on Quit.
	CHECK(pauseBarKey(&s, DN).kind == PAUSEBAR_ACT_MOVE);
	CHECK(s.row == PB_GAME_QUIT);
	// OPTIONS: leave it on Sound.
	CHECK(pauseBarKey(&s, R).kind == PAUSEBAR_ACT_TAB);
	CHECK(s.row == 0);                       // a tab visited for the first time starts at 0
	(void)pauseBarKey(&s, DN);
	(void)pauseBarKey(&s, DN);
	CHECK(s.row == PB_OPT_VOLUME);
	// SYSTEM: leave it on Debug.
	CHECK(pauseBarKey(&s, R).kind == PAUSEBAR_ACT_TAB);
	CHECK(s.row == 0);
	(void)pauseBarKey(&s, DN);
	CHECK(s.row == PB_SYS_DEBUG);

	// Walking back restores each one.
	CHECK(pauseBarKey(&s, L).arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(s.row == PB_OPT_VOLUME);
	CHECK(pauseBarKey(&s, L).arg == PAUSEBAR_TAB_GAME);
	CHECK(s.row == PB_GAME_QUIT);
	// ...and walking forward again still has them.
	CHECK(pauseBarKey(&s, R).arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(s.row == PB_OPT_VOLUME);
	CHECK(pauseBarKey(&s, R).arg == PAUSEBAR_TAB_SYSTEM);
	CHECK(s.row == PB_SYS_DEBUG);

	// A remembered row too deep for the tab it is restored into is clamped, not carried:
	// OPTIONS' row 2 has no counterpart on SYSTEM's two rows.
	PauseBarState c = opened();
	c.tab     = PAUSEBAR_TAB_OPTIONS;
	c.row     = PB_OPT_VOLUME;
	c.row_mem[PAUSEBAR_TAB_SYSTEM] = 2;      // out of range for SYSTEM
	CHECK(pauseBarKey(&c, R).arg == PAUSEBAR_TAB_SYSTEM);
	CHECK(c.row == 0);
	CHECK(c.row < pauseBarTabRows(c.tab));
}

// ── Commits ────────────────────────────────────────────────────────────────────────────

static void testCommitsOnEveryRow(void)
{
	// GAME / Resume closes the panel and says RESUME.
	PauseBarState s = opened();
	PauseBarAction act = pauseBarKey(&s, A);
	CHECK(act.kind == PAUSEBAR_ACT_RESUME);
	CHECK(pauseBarOpen(&s) == false);

	// GAME / Quit closes it and says QUIT.
	s = opened();
	(void)pauseBarKey(&s, DN);
	act = pauseBarKey(&s, A);
	CHECK(act.kind == PAUSEBAR_ACT_QUIT);
	CHECK(pauseBarOpen(&s) == false);

	// OPTIONS: A does nothing on the two value rows and flips 3D on the third — the
	// behaviour scene/pausemenu.c already ships for that row.
	s = opened();
	(void)pauseBarKey(&s, R);
	CHECK(s.tab == PAUSEBAR_TAB_OPTIONS && s.row == PB_OPT_DIST);
	CHECK(pauseBarKey(&s, A).kind == PAUSEBAR_ACT_NONE);
	CHECK(pauseBarOpen(&s) == true);
	(void)pauseBarKey(&s, DN);
	CHECK(s.row == PB_OPT_3D);
	act = pauseBarKey(&s, A);
	CHECK(act.kind == PAUSEBAR_ACT_STEREO_TOGGLE);
	CHECK(act.arg == 0);
	CHECK(pauseBarOpen(&s) == true);
	(void)pauseBarKey(&s, DN);
	CHECK(s.row == PB_OPT_VOLUME);
	CHECK(pauseBarKey(&s, A).kind == PAUSEBAR_ACT_NONE);

	// SYSTEM: both rows report a leaf screen and LEAVE THE PANEL OPEN, so closing the leaf
	// lands back on the row it was opened from.
	s = opened();
	(void)pauseBarKey(&s, R);
	(void)pauseBarKey(&s, R);
	CHECK(s.tab == PAUSEBAR_TAB_SYSTEM && s.row == PB_SYS_CONTROLS);
	act = pauseBarKey(&s, A);
	CHECK(act.kind == PAUSEBAR_ACT_REMAP);
	CHECK(pauseBarOpen(&s) == true);
	CHECK(s.tab == PAUSEBAR_TAB_SYSTEM);
	CHECK(s.row == PB_SYS_CONTROLS);
	(void)pauseBarKey(&s, DN);
	act = pauseBarKey(&s, A);
	CHECK(act.kind == PAUSEBAR_ACT_DEBUG);
	CHECK(pauseBarOpen(&s) == true);
	CHECK(s.row == PB_SYS_DEBUG);

	// This is the "unpause restores the prior screen" case: nothing about opening and
	// closing a leaf screen touches the cursor, so the panel the player comes back to is
	// the one they left. Simulated by doing nothing at all between the two reads, which is
	// exactly what main.c does while remap/debug is up.
	const int8_t tab_before = s.tab, row_before = s.row;
	for (int frame = 0; frame < 30; frame++)
		CHECK(pauseBarKey(&s, 0u).kind == PAUSEBAR_ACT_NONE);
	CHECK(s.tab == tab_before);
	CHECK(s.row == row_before);
	CHECK(pauseBarOpen(&s) == true);
}

static void testBResumesFromEveryTabAndRow(void)
{
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		for (int r = 0; r < rows; r++) {
			PauseBarState s = opened();
			s.tab = (int8_t)t;
			s.row = (int8_t)r;
			const PauseBarAction act = pauseBarKey(&s, B);
			// B is RESUME everywhere. There is no page to back out to any more — the tabs
			// replaced the second page, so B never means "up one level" on this panel.
			CHECK(act.kind == PAUSEBAR_ACT_RESUME);
			CHECK(act.arg == 0);
			CHECK(pauseBarOpen(&s) == false);
		}
	}
}

// ── Steppers ───────────────────────────────────────────────────────────────────────────

static void testLeftRightStepOnOptionsAndSwitchTabsElsewhere(void)
{
	// OPTIONS: left/right step the row's value and NEVER change tab. This is the blueprint's
	// own lane-D red arm 3 ("OPTIONS horizontal_is_content false -> left/right switch tabs").
	static const int expect_kind[PB_OPT_ROWS] = {
		PAUSEBAR_ACT_DIST_STEP, PAUSEBAR_ACT_STEREO_TOGGLE, PAUSEBAR_ACT_VOLUME_STEP
	};
	for (int r = 0; r < PB_OPT_ROWS; r++) {
		PauseBarState s = opened();
		s.tab = PAUSEBAR_TAB_OPTIONS;
		s.row = (int8_t)r;
		PauseBarAction act = pauseBarKey(&s, LT);
		CHECK(act.kind == expect_kind[r]);
		CHECK(act.arg == (r == PB_OPT_3D ? 0 : -1));
		CHECK(s.tab == PAUSEBAR_TAB_OPTIONS);
		CHECK(s.row == r);
		act = pauseBarKey(&s, RT);
		CHECK(act.kind == expect_kind[r]);
		CHECK(act.arg == (r == PB_OPT_3D ? 0 : +1));
		CHECK(s.tab == PAUSEBAR_TAB_OPTIONS);
		CHECK(s.row == r);
	}
	// Even at the ends of the tab strip, OPTIONS' left/right stay on the row: they are not a
	// clamped tab move that happens to do nothing.
	PauseBarState s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	s.row = PB_OPT_DIST;
	for (int i = 0; i < 10; i++) {
		CHECK(pauseBarKey(&s, LT).kind == PAUSEBAR_ACT_DIST_STEP);
		CHECK(s.tab == PAUSEBAR_TAB_OPTIONS);
	}

	// GAME and SYSTEM have nothing to step, so left/right are the clamped tab switch.
	PauseBarState g = opened();
	CHECK(g.tab == PAUSEBAR_TAB_GAME);
	CHECK(pauseBarKey(&g, LT).kind == PAUSEBAR_ACT_NONE);   // clamped at the first tab
	CHECK(g.tab == PAUSEBAR_TAB_GAME);
	CHECK(pauseBarKey(&g, RT).arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(g.tab == PAUSEBAR_TAB_OPTIONS);

	PauseBarState y = opened();
	y.tab = PAUSEBAR_TAB_SYSTEM;
	CHECK(pauseBarKey(&y, RT).kind == PAUSEBAR_ACT_NONE);   // clamped at the last tab
	CHECK(y.tab == PAUSEBAR_TAB_SYSTEM);
	CHECK(pauseBarKey(&y, LT).arg == PAUSEBAR_TAB_OPTIONS);
	CHECK(y.tab == PAUSEBAR_TAB_OPTIONS);
}

// ── Several keys on one frame ──────────────────────────────────────────────────────────

static void testRepeatedPressesInOneFrame(void)
{
	// The priority, checked as pairs: the higher key acts and the lower is DROPPED, not
	// queued — so the next frame with no keys does nothing at all.
	//
	// B beats A: a frame with both closes the panel rather than committing the row.
	PauseBarState s = opened();
	(void)pauseBarKey(&s, DN);           // sit on Quit, so a stray A would be visible
	CHECK(s.row == PB_GAME_QUIT);
	PauseBarAction act = pauseBarKey(&s, A | B);
	CHECK(act.kind == PAUSEBAR_ACT_RESUME);
	CHECK(pauseBarOpen(&s) == false);

	// A beats L/R and the d-pad: the row commits and the cursor does not move.
	s = opened();
	act = pauseBarKey(&s, A | L | R | UP | DN | LT | RT);
	CHECK(act.kind == PAUSEBAR_ACT_RESUME);   // GAME row 0
	CHECK(pauseBarOpen(&s) == false);

	// L beats R, and both beat the d-pad. From OPTIONS so a move in either direction shows.
	s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	act = pauseBarKey(&s, L | R);
	CHECK(act.kind == PAUSEBAR_ACT_TAB);
	CHECK(act.arg == PAUSEBAR_TAB_GAME);
	CHECK(s.tab == PAUSEBAR_TAB_GAME);

	s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	act = pauseBarKey(&s, R | UP | DN | LT | RT);
	CHECK(act.kind == PAUSEBAR_ACT_TAB);
	CHECK(act.arg == PAUSEBAR_TAB_SYSTEM);
	CHECK(s.row == 0);

	// Up beats down: one row moves, not two, and not zero.
	s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	s.row = PB_OPT_3D;
	act = pauseBarKey(&s, UP | DN);
	CHECK(act.kind == PAUSEBAR_ACT_MOVE);
	CHECK(s.row == PB_OPT_DIST);

	// Up/down beat left/right: the row moves and the value is NOT stepped.
	s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	s.row = PB_OPT_DIST;
	act = pauseBarKey(&s, DN | LT | RT);
	CHECK(act.kind == PAUSEBAR_ACT_MOVE);
	CHECK(s.row == PB_OPT_3D);

	// Left beats right.
	s = opened();
	s.tab = PAUSEBAR_TAB_OPTIONS;
	s.row = PB_OPT_VOLUME;
	act = pauseBarKey(&s, LT | RT);
	CHECK(act.kind == PAUSEBAR_ACT_VOLUME_STEP);
	CHECK(act.arg == -1);

	// Every key at once, from every tab and row: exactly one action, always B's RESUME,
	// and the panel is closed afterwards.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		for (int r = 0; r < pauseBarTabRows(t); r++) {
			PauseBarState e = opened();
			e.tab = (int8_t)t;
			e.row = (int8_t)r;
			act = pauseBarKey(&e, A | B | L | R | UP | DN | LT | RT);
			CHECK(act.kind == PAUSEBAR_ACT_RESUME);
			CHECK(pauseBarOpen(&e) == false);
			// The dropped keys are gone, not queued for the next frame.
			CHECK(pauseBarKey(&e, 0u).kind == PAUSEBAR_ACT_NONE);
		}
	}

	// A frame with no keys, repeatedly, on an open panel: nothing moves and nothing fires.
	PauseBarState idle = opened();
	for (int f = 0; f < 60; f++) {
		CHECK(pauseBarKey(&idle, 0u).kind == PAUSEBAR_ACT_NONE);
		CHECK(idle.tab == PAUSEBAR_TAB_GAME);
		CHECK(idle.row == 0);
		CHECK(pauseBarOpen(&idle) == true);
	}

	// Bits this panel does not use (X, Y, START, SELECT, ZL, ZR) are ignored rather than
	// falling through to some other branch.
	static const uint32_t unused_bits[] = { 0x0004u, 0x0008u, 0x0400u, 0x0800u,
	                                        0x4000u, 0x8000u, 0x00100000u, 0x80000000u };
	for (unsigned i = 0; i < sizeof unused_bits / sizeof unused_bits[0]; i++) {
		PauseBarState u = opened();
		CHECK(pauseBarKey(&u, unused_bits[i]).kind == PAUSEBAR_ACT_NONE);
		CHECK(u.tab == PAUSEBAR_TAB_GAME);
		CHECK(u.row == 0);
		CHECK(pauseBarOpen(&u) == true);
	}
}

// ── Input arriving while already paused ────────────────────────────────────────────────

static void testInputWhileAlreadyPaused(void)
{
	// The panel is already up and the player keeps pressing things. Nothing re-opens it,
	// nothing double-fires, and the state stays in range after a long random-ish sequence.
	static const uint32_t seq[] = { DN, DN, R, UP, LT, RT, R, DN, L, A, UP, RT, L, DN,
	                                R, R, UP, LT, DN, L, RT, UP, R, DN, LT, L, UP, RT };
	PauseBarState s = opened();
	for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
		const PauseBarAction act = pauseBarKey(&s, seq[i]);
		if (!pauseBarOpen(&s)) {         // an A landed on Resume/Quit; reopen and carry on
			CHECK(act.kind == PAUSEBAR_ACT_RESUME || act.kind == PAUSEBAR_ACT_QUIT);
			pauseBarToggle(&s);
		}
		// The invariant, every single frame: the cursor is a cell that exists.
		CHECK(s.tab >= 0 && s.tab < PAUSEBAR_TAB_COUNT);
		CHECK(s.row >= 0 && s.row < pauseBarTabRows(s.tab));
		for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++)
			CHECK(s.row_mem[t] < (uint8_t)pauseBarTabRows(t));
	}

	// SELECT pressed while already paused is pauseBarToggle, and it CLOSES — it does not
	// re-enter and reset the cursor while leaving the panel up.
	PauseBarState t2 = opened();
	(void)pauseBarKey(&t2, R);
	(void)pauseBarKey(&t2, DN);
	pauseBarToggle(&t2);
	CHECK(pauseBarOpen(&t2) == false);

	// A stylus press arriving on the very frame the panel opened is a real press, not a
	// leftover: this module holds no touch edge state of its own — the caller's rising edge
	// is the contract (see pauseBarTouch's header note).
	PauseBarState t3 = opened();
	const PbRect tab2 = pauseBarTabRect(PAUSEBAR_TAB_SYSTEM);
	CHECK(pauseBarTouch(&t3, true, midX(tab2), midY(tab2)).kind == PAUSEBAR_ACT_TAB);
	CHECK(t3.tab == PAUSEBAR_TAB_SYSTEM);
}

// ── Corrupt state ──────────────────────────────────────────────────────────────────────

static void testClampRepairsACorruptCursor(void)
{
	static const int8_t bad_tab[] = { -128, -9, -1, 3, 5, 100, 127 };
	static const int8_t bad_row[] = { -128, -4, -1, 3, 8, 100, 127 };

	for (unsigned i = 0; i < sizeof bad_tab / sizeof bad_tab[0]; i++) {
		for (unsigned j = 0; j < sizeof bad_row / sizeof bad_row[0]; j++) {
			PauseBarState s = opened();
			s.tab = bad_tab[i];
			s.row = bad_row[j];
			s.row_mem[0] = 200;
			s.row_mem[1] = 9;
			s.row_mem[2] = 2;
			pauseBarClamp(&s);
			CHECK(s.tab >= 0 && s.tab < PAUSEBAR_TAB_COUNT);
			CHECK(s.row >= 0 && s.row < pauseBarTabRows(s.tab));
			// Each remembered row is clamped against ITS OWN tab's count: 2 is legal for
			// OPTIONS and one past the end of SYSTEM.
			for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++)
				CHECK(s.row_mem[t] < (uint8_t)pauseBarTabRows(t));

			// And the entry points clamp for the caller, so a corrupt state cannot reach
			// the tables at all.
			PauseBarState k = opened();
			k.tab = bad_tab[i];
			k.row = bad_row[j];
			(void)pauseBarKey(&k, DN);
			CHECK(k.tab >= 0 && k.tab < PAUSEBAR_TAB_COUNT);
			CHECK(k.row >= 0 && k.row < pauseBarTabRows(k.tab));

			PauseBarState p = opened();
			p.tab = bad_tab[i];
			p.row = bad_row[j];
			const PbRect row0 = pauseBarRowRect(0);
			(void)pauseBarTouch(&p, true, midX(row0), midY(row0));
			CHECK(p.tab >= 0 && p.tab < PAUSEBAR_TAB_COUNT);
			CHECK(p.row >= 0 && p.row < pauseBarTabRows(p.tab));
		}
	}
	// A valid row for a wide tab is not clamped when the tab is already valid.
	PauseBarState ok = opened();
	ok.tab = PAUSEBAR_TAB_OPTIONS;
	ok.row = PB_OPT_VOLUME;
	pauseBarClamp(&ok);
	CHECK(ok.tab == PAUSEBAR_TAB_OPTIONS);
	CHECK(ok.row == PB_OPT_VOLUME);
}

// ── Touch ──────────────────────────────────────────────────────────────────────────────

static void testTouchOnTheStrip(void)
{
	// The centre of every tab, from every other tab: the cursor lands there and the action
	// names it. Tapping the tab already focused is a no-op with no cue.
	for (int from = 0; from < PAUSEBAR_TAB_COUNT; from++) {
		for (int to = 0; to < PAUSEBAR_TAB_COUNT; to++) {
			PauseBarState s = opened();
			s.tab = (int8_t)from;
			const PbRect t = pauseBarTabRect(to);
			const PauseBarAction act = pauseBarTouch(&s, true, midX(t), midY(t));
			if (from == to) {
				CHECK(act.kind == PAUSEBAR_ACT_NONE);
			} else {
				CHECK(act.kind == PAUSEBAR_ACT_TAB);
				CHECK(act.arg == to);
			}
			CHECK(s.tab == to);
		}
	}
	// A stylus tap switching tabs remembers rows the same way L/R does.
	PauseBarState s = opened();
	(void)pauseBarKey(&s, DN);
	CHECK(s.row == PB_GAME_QUIT);
	const PbRect sys = pauseBarTabRect(PAUSEBAR_TAB_SYSTEM);
	CHECK(pauseBarTouch(&s, true, midX(sys), midY(sys)).kind == PAUSEBAR_ACT_TAB);
	CHECK(s.row == 0);
	const PbRect game = pauseBarTabRect(PAUSEBAR_TAB_GAME);
	CHECK(pauseBarTouch(&s, true, midX(game), midY(game)).kind == PAUSEBAR_ACT_TAB);
	CHECK(s.row == PB_GAME_QUIT);

	// `press` false is not a press: a stylus held on a tab does not re-fire.
	PauseBarState h = opened();
	CHECK(pauseBarTouch(&h, false, midX(sys), midY(sys)).kind == PAUSEBAR_ACT_NONE);
	CHECK(h.tab == PAUSEBAR_TAB_GAME);
}

static void testTouchOnRowsAndArrows(void)
{
	// A tap on a plain row commits it, exactly as A on it does — the "synthetic tap and the
	// controller cannot diverge" rule the blueprint states for the in-game bar (D8), applied
	// here by having ONE commitRow behind both paths.
	PauseBarState s = opened();
	PbRect row = pauseBarRowRect(PB_GAME_QUIT);
	PauseBarAction act = pauseBarTouch(&s, true, midX(row), midY(row));
	CHECK(act.kind == PAUSEBAR_ACT_QUIT);
	CHECK(pauseBarOpen(&s) == false);

	s = opened();
	row = pauseBarRowRect(PB_GAME_RESUME);
	CHECK(pauseBarTouch(&s, true, midX(row), midY(row)).kind == PAUSEBAR_ACT_RESUME);
	CHECK(pauseBarOpen(&s) == false);

	// SYSTEM's two rows, by stylus, leaving the panel open.
	s = opened();
	s.tab = PAUSEBAR_TAB_SYSTEM;
	row = pauseBarRowRect(PB_SYS_CONTROLS);
	CHECK(pauseBarTouch(&s, true, midX(row), midY(row)).kind == PAUSEBAR_ACT_REMAP);
	CHECK(pauseBarOpen(&s) == true);
	CHECK(s.row == PB_SYS_CONTROLS);
	row = pauseBarRowRect(PB_SYS_DEBUG);
	CHECK(pauseBarTouch(&s, true, midX(row), midY(row)).kind == PAUSEBAR_ACT_DEBUG);
	CHECK(s.row == PB_SYS_DEBUG);

	// OPTIONS: the arrows step, and the direction is the box's own. Every row, both arrows,
	// and the centre of the box AND both of its inside corners, because an off-by-one on
	// PB_ARROW_L_X is the arm this catches.
	static const int expect_kind[PB_OPT_ROWS] = {
		PAUSEBAR_ACT_DIST_STEP, PAUSEBAR_ACT_STEREO_TOGGLE, PAUSEBAR_ACT_VOLUME_STEP
	};
	for (int r = 0; r < PB_OPT_ROWS; r++) {
		static const int dirs[2] = { PB_ARROW_LEFT, PB_ARROW_RIGHT };
		for (int d = 0; d < 2; d++) {
			const PbRect box = pauseBarArrowRect(PAUSEBAR_TAB_OPTIONS, r, dirs[d]);
			const int pts[3][2] = { { midX(box), midY(box) },
			                        { box.x, box.y },
			                        { box.x + box.w - 1, box.y + box.h - 1 } };
			for (int p = 0; p < 3; p++) {
				PauseBarState o = opened();
				o.tab = PAUSEBAR_TAB_OPTIONS;
				o.row = 0;
				act = pauseBarTouch(&o, true, pts[p][0], pts[p][1]);
				CHECK(act.kind == expect_kind[r]);
				CHECK(act.arg == (r == PB_OPT_3D ? 0 : dirs[d]));
				CHECK(o.row == r);                 // the cursor followed the stylus
				CHECK(o.tab == PAUSEBAR_TAB_OPTIONS);
				CHECK(pauseBarOpen(&o) == true);
			}
			// One pixel to the left of the box is NOT the arrow: for the left arrow that
			// point is the label band, for the right arrow it is the value field. Either
			// way the answer is a plain no-op (the cursor is already on this row), and an
			// off-by-one on PB_ARROW_L_X / PB_ARROW_R_X turns it into a step.
			PauseBarState m = opened();
			m.tab = PAUSEBAR_TAB_OPTIONS;
			m.row = (int8_t)r;
			act = pauseBarTouch(&m, true, box.x - 1, midY(box));
			CHECK(act.kind == PAUSEBAR_ACT_NONE);
			CHECK(m.row == r);
		}
		// The label half of a stepper row moves the cursor and steps NOTHING.
		PauseBarState o = opened();
		o.tab = PAUSEBAR_TAB_OPTIONS;
		o.row = 0;
		const PbRect band = pauseBarRowRect(r);
		act = pauseBarTouch(&o, true, band.x + 4, midY(band));
		CHECK(act.kind == (r == 0 ? PAUSEBAR_ACT_NONE : PAUSEBAR_ACT_MOVE));
		CHECK(o.row == r);
	}

	// A tap on a row this tab does NOT have does nothing: GAME's third band is empty.
	PauseBarState g = opened();
	const PbRect ghost = pauseBarRowRect(2);
	CHECK(pauseBarTouch(&g, true, midX(ghost), midY(ghost)).kind == PAUSEBAR_ACT_NONE);
	CHECK(g.row == 0);
	CHECK(pauseBarOpen(&g) == true);

	// A tap outside the panel does NOT close it — the panel is modal, and a mis-aimed
	// stylus must not drop the player back into the world.
	static const int outside[][2] = { { 0, 0 }, { 319, 239 }, { 160, 4 }, { 5, 120 },
	                                  { 314, 120 }, { 160, 236 } };
	for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
		PauseBarState p = opened();
		CHECK(pauseBarTouch(&p, true, outside[i][0], outside[i][1]).kind == PAUSEBAR_ACT_NONE);
		CHECK(pauseBarOpen(&p) == true);
		CHECK(p.tab == PAUSEBAR_TAB_GAME);
		CHECK(p.row == 0);
	}

	// A full sweep of the whole bottom screen on every tab: an action only ever comes from
	// a point inside the strip or inside a row this tab has, and the cursor never leaves
	// range. This is what catches a hit test that answers for a point it does not own.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		for (int y = 0; y < PB_SCR_H; y += 5) {
			for (int x = 0; x < PB_SCR_W; x += 7) {
				PauseBarState p = opened();
				p.tab = (int8_t)t;
				act = pauseBarTouch(&p, true, x, y);
				if (act.kind != PAUSEBAR_ACT_NONE) {
					const bool on_strip = pauseBarHitTab(x, y) != PAUSEBAR_HIT_NONE;
					const bool on_row   = pauseBarHitRow(t, x, y) != PAUSEBAR_HIT_NONE;
					CHECK(on_strip || on_row);
				}
				if (pauseBarOpen(&p)) {
					CHECK(p.tab >= 0 && p.tab < PAUSEBAR_TAB_COUNT);
					CHECK(p.row >= 0 && p.row < pauseBarTabRows(p.tab));
				}
			}
		}
	}
}

int main(void)
{
	testZeroStateIsValidAndInert();
	testToggleAndClose();
	testTablesRefuseOutOfRange();
	testStripTilesThePanelExactly();
	testRowsAreDisjointAndInsideThePanel();
	testArrowBoxesSitInsideTheirRow();
	testLabelsFitTheirBoxes();
	testRowWrapsOnEveryTab();
	testTabsClampAndNeverWrap();
	testTabsRememberTheirRow();
	testCommitsOnEveryRow();
	testBResumesFromEveryTabAndRow();
	testLeftRightStepOnOptionsAndSwitchTabsElsewhere();
	testRepeatedPressesInOneFrame();
	testInputWhileAlreadyPaused();
	testClampRepairsACorruptCursor();
	testTouchOnTheStrip();
	testTouchOnRowsAndArrows();

	CHECK(s_checks + 1 == PAUSEBAR_EXPECTED_CHECKS);

	if (s_fails) {
		printf("pausebar self-test: FAILED - %d of %d checks, first: %s\n",
		       s_fails, s_checks, s_first);
		return 1;
	}
	printf("pausebar self-test: PASS %d checks\n", s_checks);
	return 0;
}
