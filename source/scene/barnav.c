// v1.9.1 BAR. See scene/barnav.h for the contract — every rule this file implements is stated
// there, and nothing here is a rule of its own. No <3ds.h> and no gfx/: tests/barnav_test.c
// links this module on the host, the carve-out scene/hotbar.c, scene/ui_gesture.c and
// scene/ringorder.c already made.
#include "scene/barnav.h"

#ifdef __3DS__
#include <3ds.h>
// The whole mirror, not just the two bits this file branches on: a caller hands in a raw
// hidKeysDown() word, so every bit the header names has to be the bit libctru means, including
// the ones only agents C/D/E read. Console-only because the host has no <3ds.h> to compare to;
// scene/ui.c carries the same assert at the seam where the real word arrives (blueprint §6.3).
_Static_assert(BAR_KEY_A == KEY_A && BAR_KEY_B == KEY_B &&
               BAR_KEY_DRIGHT == KEY_DRIGHT && BAR_KEY_DLEFT == KEY_DLEFT &&
               BAR_KEY_DUP == KEY_DUP && BAR_KEY_DDOWN == KEY_DDOWN &&
               BAR_KEY_R == KEY_R && BAR_KEY_L == KEY_L &&
               BAR_KEY_X == KEY_X && BAR_KEY_Y == KEY_Y,
               "scene/barnav.h's key bits must mirror libctru's KEY_*");
#endif

// ── Reading a spec ─────────────────────────────────────────────────────────────────────────

// The category list's usable length. row_mem is BARNAV_MAX_CATS long, so a caller that passes
// more categories than that gets them ignored rather than a write past the end of the array.
static int catLimit(int cat_count)
{
	if (cat_count < 0) return 0;
	if (cat_count > BARNAV_MAX_CATS) return BARNAV_MAX_CATS;
	return cat_count;
}

// How wide row `row` is. A category with uniform_cols set answers that for every row, including
// rows past BARNAV_MAX_ROWS — which is the only way CRAFT's row count is allowed to be the
// recipe count. Otherwise it is cols[row], with a stored 0 (and any row outside the array)
// reading as 1: a row always has the one cell the cursor is on, so no caller ever has to divide
// by, or index with, a width of zero.
static int colsAt(const BarCatSpec* spec, int row)
{
	if (spec->uniform_cols > 0) return spec->uniform_cols;
	if (row < 0 || row >= BARNAV_MAX_ROWS) return 1;
	const int c = (int)spec->cols[row];
	return (c > 0) ? c : 1;
}

// Which of an opposing pair, if either, is asking. Both on one frame is a TIE and answers 0 —
// there is no order to break it with, the same answer scene/hotbar.c gives L and R together.
// The caller treats a tie as "this band said nothing" and moves on to the next one.
static int pairDir(uint32_t keys, uint32_t neg, uint32_t pos)
{
	const uint32_t both = keys & (neg | pos);
	if (both == neg) return -1;
	if (both == pos) return +1;
	return 0;
}

// ── The cursor invariant ───────────────────────────────────────────────────────────────────

void barNavClamp(BarNav* n, const BarCatSpec* cats, int cat_count)
{
	if (!n) return;

	if (n->pref_col < 0) n->pref_col = 0;

	const int count = catLimit(cat_count);
	if (!cats || count <= 0) {
		// No categories at all: there is no cell to be on. Park at the origin rather than
		// leave a stale index that the next call would read a spec through.
		n->cat = 0;
		n->row = 0;
		n->col = 0;
		return;
	}

	// A cat outside the list goes to 0, not to count-1: the tab that vanishes is the container
	// at index 2 (blueprint D2), and the honest place to land when it does is INVENTORY — the
	// one category that is always there — rather than whatever happens to be last.
	if (n->cat < 0 || n->cat >= count) n->cat = 0;

	const BarCatSpec* spec = &cats[n->cat];
	if (spec->rows <= 0) {
		n->row = 0;
		n->col = 0;
		return;
	}

	if (n->row < 0)            n->row = 0;
	if (n->row >= spec->rows)  n->row = spec->rows - 1;

	const int cols = colsAt(spec, n->row);
	if (n->col < 0)      n->col = 0;
	if (n->col >= cols)  n->col = cols - 1;
}

void barNavReset(BarNav* n, int cat, int row, int col)
{
	if (!n) return;

	for (int i = 0; i < BARNAV_MAX_CATS; i++) n->row_mem[i] = 0;
	n->cat      = (cat < 0) ? 0 : cat;
	n->row      = (row < 0) ? 0 : row;
	n->col      = (col < 0) ? 0 : col;
	n->pref_col = n->col;
}

// Remember the row this category is being left on. Stored in a byte, so a row past 255 is
// remembered as 255 — see the header on why that costs nothing real.
static void rememberRow(BarNav* n)
{
	if (n->cat < 0 || n->cat >= BARNAV_MAX_CATS) return;
	const int r = (n->row < 0) ? 0 : (n->row > 255 ? 255 : n->row);
	n->row_mem[n->cat] = (uint8_t)r;
}

// Move the focus to `target`, which the caller has already checked is a real category, carrying
// row_mem across in both directions and restoring the column from pref_col.
static void enterCat(BarNav* n, int target, const BarCatSpec* cats, int count)
{
	rememberRow(n);
	n->cat = target;
	n->row = (int)n->row_mem[target];
	n->col = n->pref_col;
	barNavClamp(n, cats, count);
}

void barNavSetCat(BarNav* n, int cat, const BarCatSpec* cats, int cat_count)
{
	if (!n) return;

	const int count = catLimit(cat_count);
	if (!cats || count <= 0) {
		barNavClamp(n, cats, cat_count);
		return;
	}

	int target = cat;
	if (target < 0)      target = 0;
	if (target >= count) target = count - 1;

	if (target == n->cat) {
		barNavClamp(n, cats, count);   // no move, but the invariant still has to hold
		return;
	}
	enterCat(n, target, cats, count);
}

void barNavSetCell(BarNav* n, int row, int col, const BarCatSpec* cats, int cat_count)
{
	if (!n) return;

	n->row      = (row < 0) ? 0 : row;
	n->col      = (col < 0) ? 0 : col;
	n->pref_col = n->col;
	barNavClamp(n, cats, cat_count);
}

// ── The frame ──────────────────────────────────────────────────────────────────────────────

// L / R, and left/right on a category that does not take them as content. Clamped, no wrap: the
// tabs are a fixed short row and wrapping from the last to the first past a visible edge is the
// motion players read as an accidental double-press (blueprint D6).
static BarEvent stepCat(BarNav* n, int dir, const BarCatSpec* cats, int count)
{
	const int target = n->cat + dir;
	if (target < 0 || target >= count) return BAR_EV_NONE;
	enterCat(n, target, cats, count);
	return BAR_EV_CAT;
}

// Up / down. WRAPS, unlike the tabs: a category's rows are a closed list with no visible edge to
// stop at, and wrapping is how you get from the top row to the bottom one without a full sweep.
static BarEvent moveRow(BarNav* n, int dir, const BarCatSpec* spec)
{
	if (spec->rows <= 1) return BAR_EV_NONE;   // nowhere to go, and rows == 0 has no cursor

	int r = n->row + dir;
	if (r < 0)           r = spec->rows - 1;
	if (r >= spec->rows) r = 0;

	n->row = r;

	// The pref_col restore, NOT a carry of the current column — see the header. min(), so a
	// narrow row clamps without forgetting the wide column that is being walked through.
	const int cols = colsAt(spec, r);
	n->col = (n->pref_col < cols) ? n->pref_col : cols - 1;
	if (n->col < 0) n->col = 0;
	return BAR_EV_MOVE;
}

// Left / right. Three different jobs behind one pair of buttons — the whole of BarCatSpec's
// horizontal_is_content flag plus the row's own width decides which.
static BarEvent moveHorizontal(BarNav* n, int dir, const BarCatSpec* spec,
                               const BarCatSpec* cats, int count)
{
	if (!spec->horizontal_is_content) return stepCat(n, dir, cats, count);
	if (spec->rows <= 0)              return BAR_EV_NONE;   // no cell, nothing to move or step

	const int cols = colsAt(spec, n->row);
	if (cols <= 1) return (dir < 0) ? BAR_EV_STEP_LEFT : BAR_EV_STEP_RIGHT;

	const int c = n->col + dir;
	if (c < 0 || c >= cols) return BAR_EV_NONE;   // clamped at the row's edge: nothing moved

	n->col      = c;
	n->pref_col = c;   // the only place pref_col is set by a press: this IS the deliberate choice
	return BAR_EV_MOVE;
}

BarEvent barNavInput(BarNav* n, uint32_t keys, const BarCatSpec* cats, int cat_count, bool has_lift)
{
	if (!n) return BAR_EV_NONE;

	// Normalise FIRST, unconditionally. The category list may have changed since the last call
	// (a chest closing takes tab 2 with it) and every branch below reads a spec through n->cat.
	barNavClamp(n, cats, cat_count);

	const int count = catLimit(cat_count);
	if (!cats || count <= 0 || keys == 0) return BAR_EV_NONE;

	const BarCatSpec* spec = &cats[n->cat];

	// Priority, one event per call. See the header for the order and why B is first.
	if (keys & BAR_KEY_B) return has_lift ? BAR_EV_CANCEL : BAR_EV_BACK;
	if (keys & BAR_KEY_A) return (spec->rows > 0) ? BAR_EV_COMMIT : BAR_EV_NONE;
	if (keys & BAR_KEY_X) return BAR_EV_ALT;

	// A pressed band consumes the frame even when it could not move (L at the leftmost tab
	// answers NONE and does not fall through to the d-pad). Only a TIE falls through, because a
	// tie is the band declining to say anything at all.
	int dir = pairDir(keys, BAR_KEY_L, BAR_KEY_R);
	if (dir) return stepCat(n, dir, cats, count);

	dir = pairDir(keys, BAR_KEY_DUP, BAR_KEY_DDOWN);
	if (dir) return moveRow(n, dir, spec);

	dir = pairDir(keys, BAR_KEY_DLEFT, BAR_KEY_DRIGHT);
	if (dir) return moveHorizontal(n, dir, spec, cats, count);

	return BAR_EV_NONE;
}

// ── Scrolling ──────────────────────────────────────────────────────────────────────────────

int barNavScrollFor(int cursor_row, int scroll, int visible, int rows)
{
	if (visible <= 0 || rows <= 0) return 0;
	if (rows <= visible)           return 0;   // the list fits; there is nothing to scroll

	const int max_scroll = rows - visible;

	int cur = cursor_row;
	if (cur < 0)     cur = 0;
	if (cur >= rows) cur = rows - 1;

	int s = scroll;
	if (s < 0)          s = 0;
	if (s > max_scroll) s = max_scroll;

	// The SMALLEST change that puts the cursor in the window: follow it off an edge by one row
	// at a time rather than recentring, so a list walked with the d-pad slides under a cursor
	// that stays put instead of jumping half a screen every time it reaches the bottom.
	if (cur < s)                 s = cur;
	else if (cur >= s + visible) s = cur - visible + 1;

	if (s < 0)          s = 0;
	if (s > max_scroll) s = max_scroll;
	return s;
}

// ── The circle pad ─────────────────────────────────────────────────────────────────────────

// One axis of the stick. Returns the key bit for a deflection that STARTED this frame, 0
// otherwise. `*dir` is the armed state and is the only thing carried between frames.
static uint32_t axisEdge(int8_t* dir, int v, uint32_t neg_key, uint32_t pos_key)
{
	// long, not int: negating INT_MIN is undefined, and this takes a raw reading from a caller.
	const long mag = (v < 0) ? -(long)v : (long)v;

	const int was = *dir;
	int want = was;
	if (mag >= BARNAV_STICK_FIRE)       want = (v > 0) ? +1 : -1;
	else if (mag < BARNAV_STICK_REARM)  want = 0;
	// Between the two thresholds it holds `was` — that gap is the hysteresis.

	*dir = (int8_t)want;

	if (want == 0 || want == was) return 0;   // released, held, or still going the same way
	return (want > 0) ? pos_key : neg_key;
}

uint32_t barStickEdge(BarStick* s, int dx, int dy)
{
	if (!s) return 0;

	// dy is libctru's, positive UP, so it maps to DUP; the screen's y axis does not come into
	// it because this module never touches a pixel.
	return axisEdge(&s->dir_x, dx, BAR_KEY_DLEFT, BAR_KEY_DRIGHT) |
	       axisEdge(&s->dir_y, dy, BAR_KEY_DDOWN, BAR_KEY_DUP);
}
