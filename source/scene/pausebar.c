// v1.9.1 INTERFACE, lane D. See scene/pausebar.h for the contract and for why the pause
// panel's model lives in a file of its own rather than inside scene/pausemenu.c.
//
// No <3ds.h> and no gfx/*.h: tests/pausebar_test.c links this module on the host, the same
// carve-out scene/hotbar.c, scene/ui_gesture.c and scene/ui_layout.c already made. The
// console build pulls the real headers in below only to prove the mirrored constants agree.
#include "scene/pausebar.h"

#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#include "gfx/font.h"
#include "scene/ui_layout.h"
_Static_assert(PAUSEBAR_KEY_A == KEY_A && PAUSEBAR_KEY_B == KEY_B &&
               PAUSEBAR_KEY_DRIGHT == KEY_DRIGHT && PAUSEBAR_KEY_DLEFT == KEY_DLEFT &&
               PAUSEBAR_KEY_DUP == KEY_DUP && PAUSEBAR_KEY_DDOWN == KEY_DDOWN &&
               PAUSEBAR_KEY_L == KEY_L && PAUSEBAR_KEY_R == KEY_R,
               "scene/pausebar.h's key bits must mirror libctru's KEY_*");
_Static_assert(PAUSEBAR_FONT_ADVANCE == FONT_ADVANCE &&
               PAUSEBAR_FONT_GLYPH_H == FONT_GLYPH_H,
               "scene/pausebar.h's font metrics must mirror gfx/font.h's");
_Static_assert(PB_SCR_W == SCR_W && PB_SCR_H == SCR_H,
               "scene/pausebar.h's screen size must mirror scene/ui_layout.h's");
#endif

// The size of the state is pinned, and it is pinned in BYTES rather than in members because
// the point of the assertion is the 32 KB stack: main.c holds one of these and a struct that
// silently grows is a struct that could stop being free to pass around.
//
// Every member is one byte wide by construction — bool, int8_t, uint8_t[3] — and there is
// deliberately no enum in it. -fshort-enums is in effect for the ARM build and NOT for the
// host, so a PauseBarAction-style enum member would make this assertion mean two different
// things on the two targets and prove nothing on either. PauseBarAction itself is a pair of
// ints and is never stored, only returned.
_Static_assert(sizeof(PauseBarState) == 6, "PauseBarState must stay 6 bytes on every target");
_Static_assert(_Alignof(PauseBarState) == 1, "PauseBarState must have no padding to hide in");

// The strip has to end where the first row's band begins, or a tap in the seam belongs to
// neither. Checked here rather than left to the two constants being reread correctly.
_Static_assert(PB_STRIP_Y + PB_STRIP_H <= PB_ROW_Y0, "the tab strip must clear the first row");
_Static_assert(PB_STRIP_Y >= PB_PANEL_Y, "the tab strip must sit inside the panel");
// Against PB_SYS_ROWS and not PAUSEBAR_MAX_ROWS: the readout is drawn on SYSTEM only
// (pauseBarTabShowsMemory), and SYSTEM has two rows. OPTIONS' third row reaches y 124 and
// would sit on top of the readout — which is exactly why the readout is not drawn there.
_Static_assert(PB_ROW_Y0 + PB_SYS_ROWS * PB_ROW_H <= PB_MEM_Y0,
               "the memory readout must clear SYSTEM's deepest row");
_Static_assert(PB_MEM_Y0 + (PB_MEM_ROWS - 1) * PB_MEM_STEP + PAUSEBAR_FONT_GLYPH_H < PB_FOOTER_Y,
               "the memory readout must clear the footer");
_Static_assert(PB_ARROW_R_X + PB_ARROW_W <= PB_ROW_X + PB_ROW_W,
               "the right stepper arrow must stay inside its row");
_Static_assert(PB_ARROW_L_X + PB_ARROW_W <= PB_ARROW_R_X,
               "the two stepper arrows must not overlap");

static const PbRect PB_ZERO_RECT = { 0, 0, 0, 0 };

static const PauseBarAction PB_NO_ACTION = { PAUSEBAR_ACT_NONE, 0 };

bool pbPtInRect(PbRect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ── Tables ─────────────────────────────────────────────────────────────────────────────

static bool tabOk(int tab) { return tab >= 0 && tab < PAUSEBAR_TAB_COUNT; }

int pauseBarTabRows(int tab)
{
	switch (tab) {
	case PAUSEBAR_TAB_GAME:    return PB_GAME_ROWS;
	case PAUSEBAR_TAB_OPTIONS: return PB_OPT_ROWS;
	case PAUSEBAR_TAB_SYSTEM:  return PB_SYS_ROWS;
	default:                   return 0;
	}
}

const char* pauseBarTabLabel(int tab)
{
	switch (tab) {
	case PAUSEBAR_TAB_GAME:    return "GAME";
	case PAUSEBAR_TAB_OPTIONS: return "OPTIONS";
	case PAUSEBAR_TAB_SYSTEM:  return "SYSTEM";
	default:                   return "";
	}
}

const char* pauseBarRowLabel(int tab, int row)
{
	if (row < 0 || row >= pauseBarTabRows(tab)) return "";
	switch (tab) {
	case PAUSEBAR_TAB_GAME:
		// "Quit to title", not "Quit": the row leaves the world and lands on the title
		// screen, which is where the app's own quit lives. The wording scene/pausemenu.c
		// already ships, and its reasoning is recorded there.
		return (row == PB_GAME_RESUME) ? "Resume" : "Quit to title";
	case PAUSEBAR_TAB_OPTIONS:
		if (row == PB_OPT_DIST) return "Render dist";
		if (row == PB_OPT_3D)   return "3D";
		return "Sound";
	case PAUSEBAR_TAB_SYSTEM:
		return (row == PB_SYS_CONTROLS) ? "Controls" : "Debug";
	default:
		return "";
	}
}

// Every button this panel answers to, on one line, at the bottom of every tab. L/R is named
// even on GAME and SYSTEM, where left/right do the same thing, because the shoulder buttons
// are the only tab control a player can find without discovering that left/right double up.
const char* pauseBarFooterLabel(void) { return "A SELECT  B RESUME  L/R TAB"; }

bool pauseBarRowIsStepper(int tab, int row)
{
	return tab == PAUSEBAR_TAB_OPTIONS && row >= 0 && row < PB_OPT_ROWS;
}

bool pauseBarHorizontalIsContent(int tab) { return tab == PAUSEBAR_TAB_OPTIONS; }

bool pauseBarTabShowsMemory(int tab) { return tab == PAUSEBAR_TAB_SYSTEM; }

// ── Rects ──────────────────────────────────────────────────────────────────────────────

PbRect pauseBarPanelRect(void)
{
	PbRect r = { PB_PANEL_X, PB_PANEL_Y, PB_PANEL_W, PB_PANEL_H };
	return r;
}

PbRect pauseBarStripRect(void)
{
	PbRect r = { PB_PANEL_X, PB_STRIP_Y, PB_PANEL_W, PB_STRIP_H };
	return r;
}

PbRect pauseBarTabRect(int i)
{
	if (!tabOk(i)) return PB_ZERO_RECT;
	// Each tab's edges are derived from the SAME expression evaluated at i and at i+1, so
	// tab i's right edge is tab i+1's left edge by construction and the three cover
	// [PB_PANEL_X, PB_PANEL_X + PB_PANEL_W) with nothing left over. Writing this as a fixed
	// PB_PANEL_W / 3 would truncate 89.33 to 89 and leave a one-pixel column at the right
	// that the strip fill covers and no tab claims — the blueprint's own lane-B red arm 1.
	const int left  = (i * PB_PANEL_W) / PAUSEBAR_TAB_COUNT;
	const int right = ((i + 1) * PB_PANEL_W) / PAUSEBAR_TAB_COUNT;
	PbRect r = { PB_PANEL_X + left, PB_STRIP_Y, right - left, PB_STRIP_H };
	return r;
}

PbRect pauseBarRowRect(int row)
{
	if (row < 0 || row >= PAUSEBAR_MAX_ROWS) return PB_ZERO_RECT;
	PbRect r = { PB_ROW_X, PB_ROW_Y0 + row * PB_ROW_H, PB_ROW_W, PB_ROW_H - 2 };
	return r;
}

PbRect pauseBarArrowRect(int tab, int row, int dir)
{
	if (!pauseBarRowIsStepper(tab, row)) return PB_ZERO_RECT;
	if (dir != PB_ARROW_LEFT && dir != PB_ARROW_RIGHT) return PB_ZERO_RECT;
	const PbRect band = pauseBarRowRect(row);
	if (band.w == 0) return PB_ZERO_RECT;
	PbRect r = { (dir == PB_ARROW_LEFT) ? PB_ARROW_L_X : PB_ARROW_R_X,
	             band.y, PB_ARROW_W, band.h };
	return r;
}

int pauseBarHitTab(int x, int y)
{
	if (!pbPtInRect(pauseBarStripRect(), x, y)) return PAUSEBAR_HIT_NONE;
	for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++)
		if (pbPtInRect(pauseBarTabRect(i), x, y)) return i;
	// Unreachable while the three tile the strip exactly, and kept anyway: this function's
	// answer must be a real tab or NONE whatever the tiling does, not a fall-through index.
	return PAUSEBAR_HIT_NONE;
}

int pauseBarHitRow(int tab, int x, int y)
{
	const int rows = pauseBarTabRows(tab);
	for (int r = 0; r < rows; r++)
		if (pbPtInRect(pauseBarRowRect(r), x, y)) return r;
	return PAUSEBAR_HIT_NONE;
}

// ── State ──────────────────────────────────────────────────────────────────────────────

void pauseBarReset(PauseBarState* s)
{
	if (!s) return;
	memset(s, 0, sizeof *s);
}

void pauseBarClose(PauseBarState* s)
{
	if (!s) return;
	pauseBarReset(s);
}

void pauseBarToggle(PauseBarState* s)
{
	if (!s) return;
	const bool was_open = s->open;
	pauseBarReset(s);
	s->open = !was_open;
}

bool pauseBarOpen(const PauseBarState* s) { return s && s->open; }

void pauseBarClamp(PauseBarState* s)
{
	if (!s) return;
	if (s->tab < 0 || s->tab >= PAUSEBAR_TAB_COUNT) s->tab = PAUSEBAR_TAB_GAME;

	// Every remembered row is clamped against ITS OWN tab's count, not against the current
	// one: SYSTEM has two rows and OPTIONS three, so a row_mem entry that is legal for one
	// tab can be one past the end of another.
	for (int t = 0; t < PAUSEBAR_TAB_COUNT; t++) {
		const int rows = pauseBarTabRows(t);
		if (rows > 0 && s->row_mem[t] >= (uint8_t)rows) s->row_mem[t] = 0;
	}

	const int rows = pauseBarTabRows(s->tab);
	if (rows <= 0)             s->row = 0;
	else if (s->row < 0)       s->row = 0;
	else if (s->row >= rows)   s->row = 0;
}

// Remembers the row the current tab is being left on, moves to `tab`, and restores the row
// that tab was last left on. `tab` is assumed already in range — every caller clamps first.
static void setTab(PauseBarState* s, int tab)
{
	s->row_mem[s->tab] = (uint8_t)s->row;
	s->tab             = (int8_t)tab;
	s->row             = (int8_t)s->row_mem[tab];
	pauseBarClamp(s);
}

static PauseBarAction action(int kind, int arg)
{
	PauseBarAction a = { kind, arg };
	return a;
}

// What A (or a tap) on this row does. NONE for the two OPTIONS rows that carry a value and
// no command: pressing A on "Render dist" has never done anything and must not start.
static PauseBarAction commitRow(PauseBarState* s, int tab, int row)
{
	if (tab == PAUSEBAR_TAB_GAME) {
		if (row == PB_GAME_RESUME) { pauseBarClose(s); return action(PAUSEBAR_ACT_RESUME, 0); }
		if (row == PB_GAME_QUIT)   { pauseBarClose(s); return action(PAUSEBAR_ACT_QUIT, 0); }
		return PB_NO_ACTION;
	}
	if (tab == PAUSEBAR_TAB_OPTIONS) {
		// 3D is a two-state setting, so left, right and A all mean the same thing: flip it.
		// Accepting A here is behaviour scene/pausemenu.c already ships — a player reaching
		// for the old SELECT-toggles-3D behaviour finds it on the first key they try.
		if (row == PB_OPT_3D) return action(PAUSEBAR_ACT_STEREO_TOGGLE, 0);
		return PB_NO_ACTION;
	}
	if (tab == PAUSEBAR_TAB_SYSTEM) {
		// The panel stays OPEN behind a leaf screen. The caller draws remap/debug over it and
		// closing one lands the player back here, on the row they left from — which is what
		// row_mem and the untouched cursor are for.
		if (row == PB_SYS_CONTROLS) return action(PAUSEBAR_ACT_REMAP, 0);
		if (row == PB_SYS_DEBUG)    return action(PAUSEBAR_ACT_DEBUG, 0);
		return PB_NO_ACTION;
	}
	return PB_NO_ACTION;
}

// Left/right on an OPTIONS row. `dir` is -1 or +1. The 3D row ignores the direction: there
// are two states and either arrow flips between them.
static PauseBarAction stepRow(int row, int dir)
{
	if (row == PB_OPT_DIST)   return action(PAUSEBAR_ACT_DIST_STEP, dir);
	if (row == PB_OPT_3D)     return action(PAUSEBAR_ACT_STEREO_TOGGLE, 0);
	if (row == PB_OPT_VOLUME) return action(PAUSEBAR_ACT_VOLUME_STEP, dir);
	return PB_NO_ACTION;
}

// L/R and the left/right that falls through to a tab switch. Clamped, never wrapping — a
// three-tab strip is a visible row of three, and a cursor that leaps from the last to the
// first reads as a glitch on a control the player can see the ends of.
static PauseBarAction moveTab(PauseBarState* s, int dir)
{
	const int want = s->tab + dir;
	if (want < 0 || want >= PAUSEBAR_TAB_COUNT) return PB_NO_ACTION;
	setTab(s, want);
	return action(PAUSEBAR_ACT_TAB, s->tab);
}

PauseBarAction pauseBarKey(PauseBarState* s, uint32_t keys_down)
{
	if (!s || !s->open) return PB_NO_ACTION;
	pauseBarClamp(s);

	// One action per call, highest first. Several keys on one frame is not a gesture anyone
	// makes on purpose; the winner acts and the rest are dropped rather than queued.
	if (keys_down & PAUSEBAR_KEY_B) {
		pauseBarClose(s);
		return action(PAUSEBAR_ACT_RESUME, 0);
	}

	if (keys_down & PAUSEBAR_KEY_A)
		return commitRow(s, s->tab, s->row);

	if (keys_down & PAUSEBAR_KEY_L) return moveTab(s, -1);
	if (keys_down & PAUSEBAR_KEY_R) return moveTab(s, +1);

	const int rows = pauseBarTabRows(s->tab);
	if (rows > 0 && (keys_down & (PAUSEBAR_KEY_DUP | PAUSEBAR_KEY_DDOWN))) {
		// Wrapping rather than clamping: two or three rows is short enough that running off
		// the bottom to reach the top is a shortcut, not a surprise — the rule
		// scene/pausemenu.c already states for its own cursor.
		const int dir = (keys_down & PAUSEBAR_KEY_DUP) ? -1 : +1;
		s->row = (int8_t)((s->row + rows + dir) % rows);
		return action(PAUSEBAR_ACT_MOVE, s->row);
	}

	if (keys_down & (PAUSEBAR_KEY_DLEFT | PAUSEBAR_KEY_DRIGHT)) {
		const int dir = (keys_down & PAUSEBAR_KEY_DLEFT) ? -1 : +1;
		if (pauseBarHorizontalIsContent(s->tab)) return stepRow(s->row, dir);
		return moveTab(s, dir);
	}

	return PB_NO_ACTION;
}

PauseBarAction pauseBarTouch(PauseBarState* s, bool press, int x, int y)
{
	if (!s || !s->open || !press) return PB_NO_ACTION;
	pauseBarClamp(s);

	const int tab = pauseBarHitTab(x, y);
	if (tab != PAUSEBAR_HIT_NONE) {
		if (tab == s->tab) return PB_NO_ACTION;   // already there; no cue for a no-op
		setTab(s, tab);
		return action(PAUSEBAR_ACT_TAB, s->tab);
	}

	const int row = pauseBarHitRow(s->tab, x, y);
	if (row == PAUSEBAR_HIT_NONE) return PB_NO_ACTION;

	// The cursor follows the stylus BEFORE anything commits, so the highlight is on the row
	// the action came from — and so a controller player who picks the stylus up mid-menu
	// finds the cursor where they last touched rather than where they last d-padded.
	const bool moved = (row != s->row);
	s->row = (int8_t)row;

	if (pauseBarRowIsStepper(s->tab, row)) {
		if (pbPtInRect(pauseBarArrowRect(s->tab, row, PB_ARROW_LEFT), x, y))
			return stepRow(row, PB_ARROW_LEFT);
		if (pbPtInRect(pauseBarArrowRect(s->tab, row, PB_ARROW_RIGHT), x, y))
			return stepRow(row, PB_ARROW_RIGHT);
		// The rest of a stepper row has nothing to commit — see commitRow — so the tap is
		// the move and nothing else.
		return moved ? action(PAUSEBAR_ACT_MOVE, row) : PB_NO_ACTION;
	}

	const PauseBarAction committed = commitRow(s, s->tab, row);
	if (committed.kind != PAUSEBAR_ACT_NONE) return committed;
	return moved ? action(PAUSEBAR_ACT_MOVE, row) : PB_NO_ACTION;
}

int pauseBarTextWidth(const char* text, int scale)
{
	if (!text || scale <= 0) return 0;
	int n = 0;
	while (text[n]) n++;
	return n * PAUSEBAR_FONT_ADVANCE * scale;
}
