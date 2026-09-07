#include "scene/title.h"

#include <errno.h>    // v1.9.0 item 6.3: the delete-failed status line names errno
#include <stdio.h>
#include <string.h>

#include "app/hw.h"           // v1.8.5: the render-distance stepper's ceiling is per-console
#include "app/updater.h"
#include "app/updater_retry.h"
#include "app/version_history.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "net/bsnet.h"
#include "net/networld.h"
#include "scene/title_nav.h"
#include "world/region.h"
#include "world/registry.h"

// ── Layout ─────────────────────────────────────────────────────────────────────────────
//
// Every size below is checked against the bottom touch panel's real physical dimensions —
// commonly documented as 53.0 x 39.75 mm for its 320x240 active area, i.e. 6.04 px/mm on
// both axes and evenly so (320/53.0 = 6.038, 240/39.75 = 6.038) — so a pixel count and a
// millimetre figure can be compared honestly instead of guessed at. The design brief for
// this whole file is that the touch UI is *designed* for touch, not adapted to it, so every
// rectangle below is commented with why it is the size it is, not just what the size is.
#define SCR_W 320
#define SCR_H 240

#define TITLE_BAR_H  20   // a label only, never tapped — not held to the finger-size budget below

// ── v1.9.1: the four-tab bar (blueprint D10) ───────────────────────────────────────────
//
// The four stacked buttons this screen opened with (PLAY / MULTIPLAYER / OPTIONS / QUIT) and
// the three screens behind three of them are one surface now: a wordmark, a tab strip, the
// focused tab's content, and a footer hint. Every number below is D10's, and every one of
// them is checked against the same 6.04 px/mm the rest of this file is.
//
//   4..18    wordmark, scale 2 ("BLOCKSMITH" is 10 glyphs * FONT_ADVANCE 6 * 2 = 120 px)
//   22..48   the tab strip
//   52..220  content: six rows of TITLE_ROW_H
//   228..235 footer hint / status line
//
// A tab is SCR_W/4 = 80 px wide exactly, so the four of them tile [0,320) with no gap and no
// overlap and no truncation to reason about — unlike the in-game bar, which has to divide 294
// by three and therefore needs ui_layout.c's per-index formula. 80x26 is 13.2 x 4.3 mm, which
// is over the 22-px minimum this project already ships for a tappable row and comfortably the
// widest class of target on the screen; the longest label, "MULTIPLAYER", is 11 glyphs = 66 px
// at scale 1 and fits inside one with 7 px either side.
//
// 28 px rows (4.6 mm) over the old MAIN_BTN_H's 38: six of them have to fit where four buttons
// used to, because the PLAY tab's rows are the world list and OPTIONS' are five settings plus
// CONTROLS. It is above BIND_ROW_H's 24 px, which this file already ships, and the row is the
// full 300 px wide — area, not height, is what a touch target is scored on (LIST_ROW_H's
// comment makes the same argument for the same reason).
//
//   52 + 6*28 = 220, then 8 px to the footer baseline at 228, whose glyphs end at 235 and
//   leave the same 5 px bottom margin the old layout had.
#define TITLE_WORDMARK_Y   4
#define TITLE_STRIP_Y      22
#define TITLE_STRIP_H      26
#define TITLE_TAB_COUNT    4
#define TITLE_TAB_W        (SCR_W / TITLE_TAB_COUNT)   // 80, and 4*80 == SCR_W exactly
#define TITLE_ROW_Y0       52
#define TITLE_ROW_H        28
#define TITLE_ROWS_VISIBLE 6
#define TITLE_FOOTER_Y     228

// The tabs, in strip order. Also the index into the BarCatSpec array titleBuildCats fills and
// into BarNav.row_mem, so the order is the one thing here that is not free to change.
enum {
	TITLE_CAT_PLAY = 0,
	TITLE_CAT_MULTIPLAYER,
	TITLE_CAT_OPTIONS,
	TITLE_CAT_SYSTEM,
	TITLE_CAT_COUNT,
};

// Rows per tab, where the count is fixed. PLAY's is 1 + world_count and MULTIPLAYER's depends
// on whether there is a session, so those two are computed in titleBuildCats instead.
#define OPT_ROW_COUNT  6   // render dist, 3D depth, invert look, look sens, shading, CONTROLS >
#define SYS_ROW_COUNT  3   // check for update, version history, quit

_Static_assert(TITLE_TAB_W * TITLE_TAB_COUNT == SCR_W,
               "the four title tabs must tile the screen exactly");
_Static_assert(TITLE_ROW_Y0 >= TITLE_STRIP_Y + TITLE_STRIP_H,
               "content rows must not overlap the tab strip");
_Static_assert(TITLE_ROW_Y0 + TITLE_ROWS_VISIBLE * TITLE_ROW_H <= TITLE_FOOTER_Y,
               "the six content rows must clear the footer");

// The shared shape behind world-select and options-general: a handful of content rows,
// then two buttons pinned to the bottom of the screen. Solved once, algebraically, rather
// than eyeballed per screen — see the two screens below for what LIST_VISIBLE_ROWS actually
// holds in each case (worlds, or the four general settings).
//
//   240 = TITLE_BAR_H(20) + gap(4) + rows * LIST_ROW_H + gap(6)
//         + LIST_BTN_H + gap(4) + LIST_BTN_H + margin(6)
//
// With 4 rows and two 36 px buttons that is 40 + 4*LIST_ROW_H + 72 = 240, so
// LIST_ROW_H = 32 (5.3 mm) is not a round number chosen for looks — it is what is left
// after both pinned buttons and every gap are accounted for. 36 px (6.0 mm) for the pinned
// buttons is short of the title screen's 48 px on purpose: those two are reached far less
// often (New World / Controls, and Back) than the title screen's three, so the smaller
// target buys back the vertical space a list needs.
#define LIST_TOP_Y         (TITLE_BAR_H + 4)
#define LIST_ROW_H         32
#define LIST_BTN_H         36
#define LIST_VISIBLE_ROWS  4
#define LIST_BTN1_Y        (LIST_TOP_Y + LIST_VISIBLE_ROWS * LIST_ROW_H + 6)
#define LIST_BTN2_Y         (LIST_BTN1_Y + LIST_BTN_H + 4)

// Bindings is the one screen this file could not make comfortable. ACTION_COUNT (7 today,
// app/options.h says more may be appended later, never removed) content rows plus a single
// pinned Back button have to fit the same 240 px, which leaves:
//
//   BIND_ROW_H = (240 - LIST_TOP_Y(24) - gap(6) - LIST_BTN_H(36) - margin(6)) / ACTION_COUNT
//
// = 168 / 7 = 24 px today (4.0 mm) — under every other target in this file, and the reason
// it is a formula rather than a literal: if a future step appends an eighth verb (see
// app/options.h's own comment on why that enum only ever grows), this recomputes instead
// of silently overlapping rows. It stays a *list* tap (the whole 300-px-wide row is the
// target, same Fitts's-law reasoning LIST_ROW_H's comment makes — area, not height, is
// what a touch target is scored on) rather than a small icon, and reaching any row from a
// controller costs exactly the same D-pad presses it would if the row were twice as tall —
// which is the actual mitigation here: nothing on this screen *requires* the undersized tap.
#define BIND_ROW_Y   LIST_TOP_Y
#define BIND_ROW_H   ((SCR_H - LIST_TOP_Y - 6 - LIST_BTN_H - 6) / ACTION_COUNT)
#define BIND_BACK_Y  (BIND_ROW_Y + ACTION_COUNT * BIND_ROW_H + 6)

// v1.9.1: the options page's own Y budget is gone, and this is the redesign the block that
// used to be here asked for in its own words — "CONTROLS/UPDATE/BACK on their own screen, or
// a scrolling list ... is still the right answer if a SIXTH setting row ever arrives". It
// arrived as CONTROLS >, and the answer taken is the first of the two: the OPTIONS tab is the
// five settings rows plus CONTROLS > on the shared TITLE_ROW_* grid (six rows, exactly the
// window), CHECK FOR UPDATE moved to the SYSTEM tab, and BACK is the tab strip and B.
//
// Nothing squeezes anything any more. The old page solved 240 px for five 26-px rows over
// three 22-px buttons with 0 px spare; the tab solves 168 px for six 28-px rows with 0 px
// spare and no pinned buttons at all, so a seventh settings row costs a scroll rather than a
// shrink — TITLE_ROWS_VISIBLE and barNavScrollFor already handle that for the PLAY tab.

// The update screen's "VERSION HISTORY" button (v1.8.8) - steve's brief in his own words:
// "a small little button ... in the top left". It sits in the same 20 px title-bar strip the
// "UPDATE" label already occupies (y=4..11), to its left, and the label moves right to make
// room rather than being dropped - see drawUpdate. 108 px fits "VERSION HISTORY" (16 chars *
// FONT_ADVANCE(6) = 96 px) at scale 1 with 6 px of padding either side; 16 px tall is the
// smallest this file gives any tappable target (below LIST_ROW_H's 32 and even BIND_ROW_H's
// 24), which is the deliberate reading of "small little button" - it is reached once, to open
// a screen with its own full-size rows, not a target a thumb has to land on repeatedly.
#define VH_ENTRY_BTN_X  6
#define VH_ENTRY_BTN_Y  3
#define VH_ENTRY_BTN_W  108
#define VH_ENTRY_BTN_H  16

// ── Top screen: the release notes (v1.6.0 task 14b) ────────────────────────────────────
//
// The one thing this file draws on the top screen, and the only place in it that is 400 px
// wide rather than 320. Every number below is derived from the panel rather than eyeballed,
// the same way the bottom screen's are.
//
// Text is scale 1 for the same reason every other body of text in this file is: at 1x a
// glyph is exactly its texels (gfx/font.h), and the point of this screen is to fit a
// paragraph of prose, not to shout.
#define TOP_W  400
#define TOP_H  240

#define NOTES_PANEL_X   6
#define NOTES_PANEL_Y   20                                   // under the heading row
#define NOTES_PANEL_W   (TOP_W - 2 * NOTES_PANEL_X)          // 388
#define NOTES_PANEL_H   (TOP_H - NOTES_PANEL_Y - 20)         // 200, leaving a footer hint row

// The scrollbar steve asked for by name — "a slider ... to show that you can actually scroll
// down to see more" — pinned to the right-hand edge inside the panel. 6 px wide is 1.0 mm on
// the top screen's pixel pitch: it is never touched (the top screen is not a touch panel), so
// it only has to be *seen*, which is a different budget from the bottom screen's buttons.
#define NOTES_BAR_W     6
#define NOTES_BAR_X     (NOTES_PANEL_X + NOTES_PANEL_W - NOTES_BAR_W - 4)
#define NOTES_TRACK_Y   (NOTES_PANEL_Y + 4)
#define NOTES_TRACK_H   (NOTES_PANEL_H - 8)                  // 192
#define NOTES_THUMB_MIN 12                                    // never shrinks below visible

// The text column: panel left edge in, and stopping short of the bar with a gap. 366 px at
// FONT_ADVANCE 6 is 61 characters a line, which is what whatsnewBuildLayout is handed — in
// pixels, so moving this moves the wrap with it.
#define NOTES_TEXT_X    (NOTES_PANEL_X + 6)
#define NOTES_TEXT_Y    (NOTES_PANEL_Y + 5)
#define NOTES_TEXT_W    (NOTES_BAR_X - 6 - NOTES_TEXT_X)     // 366
#define NOTES_VISIBLE   ((NOTES_PANEL_H - 10) / FONT_LINE)   // 21 lines

// ── Palette ────────────────────────────────────────────────────────────────────────────

#define COL_BG        SPRITE_RGBA(26, 20, 36, 255)
#define COL_PANEL     SPRITE_RGBA(52, 40, 76, 255)
#define COL_PANEL_HI  SPRITE_RGBA(90, 74, 130, 255)    // focused row/button fill
#define COL_PANEL_LO  SPRITE_RGBA(38, 30, 58, 255)      // the small -/+ chips inside a row
#define COL_TEXT      SPRITE_WHITE
#define COL_TEXT_DIM  SPRITE_RGBA(180, 170, 200, 255)
#define COL_ACCENT    SPRITE_RGBA(255, 226, 150, 255)
#define COL_WARN      SPRITE_RGBA(255, 140, 140, 255)

// ── Small shared helpers ──────────────────────────────────────────────────────────────

typedef struct { float x, y, w, h; } TRect;

static bool ptIn(TRect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Draws one button/row and reports whether it was activated this frame: a tap landing
// inside it this frame, or A while it is the D-pad-focused item. `focused` only changes
// the fill colour and draws the accent-coloured top edge — the hit test itself does not
// depend on it, so a tap always works regardless of where the D-pad cursor happens to be.
static bool uiButton(TRect r, const char* label, bool focused, bool tap, int tx, int ty,
                      bool a_down)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);

	const int scale = ((int)r.h >= 40) ? 2 : 1;
	const int tw = fontTextWidth(label, scale);
	const float lx = r.x + (r.w - (float)tw) * 0.5f;
	const float ly = r.y + (r.h - (float)(FONT_GLYPH_H * scale)) * 0.5f;
	fontDraw(lx, ly, scale, COL_TEXT, label);

	const bool tapped_here = tap && ptIn(r, tx, ty);
	return tapped_here || (focused && a_down);
}

// A filled bar for updaterProgress()'s percentage — the update screen is the only caller,
// so this stays local rather than in gfx/sprite.h; a shared bar belongs there only once a
// second caller needs one. Clamped so a stray reading cannot draw outside `r`.
static void uiProgressBar(TRect r, int pct)
{
	pct = clampInt(pct, 0, 100);
	spriteRect(r.x, r.y, r.w, r.h, COL_PANEL_LO);
	spriteRect(r.x, r.y, r.w * (float)pct / 100.0f, r.h, COL_ACCENT);
}

// A settings row with a numeric value: label left, value and a "-"/"+" chip pair right.
// D-pad Left/Right do the same thing as the chips while the row is focused, and are the
// row's *primary* control from a controller — the chips are genuinely small (22x(h-6), the
// same trade-off BIND_ROW_H's comment makes for the bindings screen) because four of these
// stack in the same budget a single title-screen button gets. Returns -1, 0 or +1: how far
// the caller should step the underlying value this frame.
static int settingRowStepper(TRect r, const char* label, const char* value, bool focused,
                              bool tap, int tx, int ty, bool left, bool right)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);
	fontDraw(r.x + 6, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	const float btn_w = 22.0f, btn_h = r.h - 6.0f;
	const TRect minus_r = {r.x + r.w - 2 * btn_w - 6, r.y + 3, btn_w, btn_h};
	const TRect plus_r  = {r.x + r.w - btn_w - 6,      r.y + 3, btn_w, btn_h};

	spriteRect(minus_r.x, minus_r.y, minus_r.w, minus_r.h, COL_PANEL_LO);
	fontDraw(minus_r.x + (minus_r.w - FONT_ADVANCE) * 0.5f,
	         minus_r.y + (minus_r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "-");
	spriteRect(plus_r.x, plus_r.y, plus_r.w, plus_r.h, COL_PANEL_LO);
	fontDraw(plus_r.x + (plus_r.w - FONT_ADVANCE) * 0.5f,
	         plus_r.y + (plus_r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "+");

	const int vw = fontTextWidth(value, 1);
	fontDraw(minus_r.x - (float)vw - 8, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_ACCENT, value);

	int step = 0;
	if (focused && left)  step = -1;
	if (focused && right) step = 1;
	if (tap && ptIn(minus_r, tx, ty)) step = -1;
	if (tap && ptIn(plus_r,  tx, ty)) step = 1;
	return step;
}

// A settings row with an ON/OFF value. The whole row is the tap target (there is only one
// state to reach, unlike the stepper's two directions, so there is no reason to shrink it
// to a chip) — tapping anywhere on it, or A while it is focused, flips the value.
static bool settingRowToggle(TRect r, const char* label, bool value, bool focused,
                              bool tap, int tx, int ty)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);
	fontDraw(r.x + 6, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	const char* v = value ? "ON" : "OFF";
	const int vw = fontTextWidth(v, 1);
	fontDraw(r.x + r.w - (float)vw - 10, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1,
	         value ? COL_ACCENT : COL_TEXT_DIM, v);

	return tap && ptIn(r, tx, ty);
}

static const char* actionLabel(int action)
{
	switch (action) {
	case ACTION_MOVE_FORWARD: return "MOVE FORWARD";
	case ACTION_MOVE_BACK:    return "MOVE BACK";
	case ACTION_MOVE_LEFT:    return "MOVE LEFT";
	case ACTION_MOVE_RIGHT:   return "MOVE RIGHT";
	case ACTION_JUMP:         return "JUMP";
	case ACTION_BREAK:        return "BREAK";
	case ACTION_PLACE:        return "PLACE";
	case ACTION_EAT:          return "EAT";
	default:                  return "?";
	}
}

// The reverse of options.c's own s_action_keys/OPTIONS_VALID_KEYS pairing, for display:
// a raw OPT_KEY_* bit back to the button name a player recognises. Kept here rather than
// in app/options.* because it is a UI string table, not a setting, and options.h is
// explicit that it wants no display concerns mixed into the format it has to keep
// host-testable.
static const char* keyBitLabel(uint32_t bit)
{
	switch (bit) {
	case OPT_KEY_A:      return "A";
	case OPT_KEY_X:      return "X";
	case OPT_KEY_Y:      return "Y";
	case OPT_KEY_DRIGHT: return "D-PAD RIGHT";
	case OPT_KEY_DLEFT:  return "D-PAD LEFT";
	case OPT_KEY_DUP:    return "D-PAD UP";
	case OPT_KEY_DDOWN:  return "D-PAD DOWN";
	case OPT_KEY_ZL:     return "ZL";
	case OPT_KEY_ZR:     return "ZR";
	default:             return "?";
	}
}

// ── The bar: chrome, geometry and the category table (v1.9.1) ──────────────────────────
//
// Geometry stays here rather than going into scene/ui_layout.h the way the in-game bar's
// does, for the same reason pausemenu.c keeps its own: ui_layout.h is solved for the 320x240
// bottom screen *of a running game*, whose strip sits under a hotbar at y 40 and gives up
// 26 px to a close tab. This bar owns the whole surface, has no hotbar above it and no close
// tab (there is nothing to close a title screen back to), so it shares the strip's visual
// grammar and none of its numbers. Two bars, one chrome — blueprint D1's own words.

static const char* titleCatLabel(int cat)
{
	switch (cat) {
	case TITLE_CAT_PLAY:        return "PLAY";
	case TITLE_CAT_MULTIPLAYER: return "MULTIPLAYER";
	case TITLE_CAT_OPTIONS:     return "OPTIONS";
	case TITLE_CAT_SYSTEM:      return "SYSTEM";
	default:                    return "?";
	}
}

static TRect titleTabRect(int i)
{
	TRect r = {(float)(i * TITLE_TAB_W), TITLE_STRIP_Y, TITLE_TAB_W, TITLE_STRIP_H};
	return r;
}

// `visible_row` is the row's position in the six-row window, not its index in the tab's list —
// the caller adds the scroll offset. Full width less the same 10 px margin every other row in
// this file uses, and 2 px shorter than the pitch so consecutive rows read as separate rows.
static TRect titleRowRect(int visible_row)
{
	TRect r = {10, (float)(TITLE_ROW_Y0 + visible_row * TITLE_ROW_H), SCR_W - 20,
	           TITLE_ROW_H - 2};
	return r;
}

// Which tab a point is in, or -1. The strip and the content rows are disjoint bands (the
// _Static_assert above holds TITLE_ROW_Y0 at or below the strip's bottom edge), so one tap can
// never be both a tab and a row and neither caller has to guard against the other.
static int titleHitStrip(int x, int y)
{
	if (y < TITLE_STRIP_Y || y >= TITLE_STRIP_Y + TITLE_STRIP_H) return -1;
	if (x < 0 || x >= SCR_W) return -1;
	return x / TITLE_TAB_W;
}

// Which of the six visible rows a point is in, or -1. Tests against titleRowRect itself rather
// than dividing by the pitch, so the 2-px gap between rows is a miss here exactly as it looks
// like one on screen.
static int titleRowFromPoint(int x, int y)
{
	for (int v = 0; v < TITLE_ROWS_VISIBLE; v++)
		if (ptIn(titleRowRect(v), x, y)) return v;
	return -1;
}

// The live shape of all four tabs, rebuilt every frame because two of them change size under
// the player: PLAY grows and shrinks with the world list, and MULTIPLAYER loses its invite row
// the moment a session comes up. scene/barnav.c reads this and nothing else to decide where a
// press may move the cursor, so a tab that shrank underneath the cursor is corrected by
// barNavInput's own leading barNavClamp rather than by anything here.
//
// horizontal_is_content is true on OPTIONS alone (blueprint D10: "steppers consume
// left/right"). On the other three there is nothing inside a one-column row for left/right to
// do, so they switch tabs there instead — a list with dead left/right feels broken (D6).
static void titleBuildCats(const TitleState* ts, BarCatSpec cats[TITLE_CAT_COUNT])
{
	memset(cats, 0, sizeof(BarCatSpec) * TITLE_CAT_COUNT);

	// Row 0 is NEW WORLD, then one row per world. NEW WORLD leads rather than trails so the
	// cursor's home position on a fresh card — where there is nothing else — is the one thing
	// the player can actually do.
	cats[TITLE_CAT_PLAY].rows                  = 1 + ts->world_count;
	cats[TITLE_CAT_PLAY].uniform_cols          = 1;
	cats[TITLE_CAT_PLAY].horizontal_is_content = false;

	// CONNECT/DISCONNECT, then "Have an invite code?" while there is no session to protect —
	// the same disappearing item drawMultiplayer has offered since enrolment landed, and for
	// the reason its own comment gives. CONNECT stays row 0 in both shapes.
	cats[TITLE_CAT_MULTIPLAYER].rows                  = (netStatus() == NET_CONNECTED) ? 1 : 2;
	cats[TITLE_CAT_MULTIPLAYER].uniform_cols          = 1;
	cats[TITLE_CAT_MULTIPLAYER].horizontal_is_content = false;

	cats[TITLE_CAT_OPTIONS].rows                  = OPT_ROW_COUNT;
	cats[TITLE_CAT_OPTIONS].uniform_cols          = 1;
	cats[TITLE_CAT_OPTIONS].horizontal_is_content = true;

	cats[TITLE_CAT_SYSTEM].rows                  = SYS_ROW_COUNT;
	cats[TITLE_CAT_SYSTEM].uniform_cols          = 1;
	cats[TITLE_CAT_SYSTEM].horizontal_is_content = false;
}

// Wordmark and tab strip. Focused tab: COL_PANEL_HI fill, a 2-px COL_ACCENT underline on its
// bottom edge and a full-brightness label; the others COL_PANEL and COL_TEXT_DIM. The label
// never changes size with focus — a tab that grows on focus moves its neighbours' touch
// targets, which is the glitch blueprint D4 rules out.
static void drawTitleChrome(const TitleState* ts)
{
	const int wm = fontTextWidth("BLOCKSMITH", 2);
	fontDraw((SCR_W - (float)wm) * 0.5f, TITLE_WORDMARK_Y, 2, COL_ACCENT, "BLOCKSMITH");

	for (int i = 0; i < TITLE_CAT_COUNT; i++) {
		const TRect t       = titleTabRect(i);
		const bool  focused = (ts->nav.cat == i);

		spriteRect(t.x, t.y, t.w, t.h, focused ? COL_PANEL_HI : COL_PANEL);
		if (focused) spriteRect(t.x, t.y + t.h - 2, t.w, 2, COL_ACCENT);
		if (i > 0)   spriteRect(t.x, t.y, 1, t.h, COL_BG);   // 1-px divider

		const char* label = titleCatLabel(i);
		const int   lw    = fontTextWidth(label, 1);
		fontDraw(t.x + (t.w - (float)lw) * 0.5f, t.y + (t.h - FONT_GLYPH_H) * 0.5f, 1,
		         focused ? COL_TEXT : COL_TEXT_DIM, label);
	}
}

// One line at the bottom of the bar. The status line wins it whenever there is one, because
// every message this file writes is something the player has to act on; otherwise the focused
// tab's own hint. There is exactly one such line, so nothing here can push a row off screen.
static void drawTitleFooter(const TitleState* ts, const char* hint)
{
	if (ts->status_ttl > 0) fontDraw(8, TITLE_FOOTER_Y, 1, COL_WARN, ts->status);
	else if (hint)          fontDraw(8, TITLE_FOOTER_Y, 1, COL_TEXT_DIM, hint);
}

// ── World select, now the PLAY tab ─────────────────────────────────────────────────────

// Re-scans REGION_ROOT for the PLAY tab. Called when the player tabs onto PLAY, at titleInit,
// and after a create — never once per frame. A directory listing is a real SD-card cost the
// same way opening a region file is (see world/region.h's own file comment on why
// files-per-column was rejected for exactly this reason); redoing it 60 times a second for a
// list nothing on disk changes while it is on screen would be that mistake again, one layer up.
static void titleEnterPlayTab(TitleState* ts)
{
	ts->world_count = worldlistScan(REGION_ROOT, ts->worlds, WORLDLIST_MAX,
	                                 &ts->world_list_truncated);
	ts->world_scroll = 0;
	ts->status[0] = '\0';
	ts->status_ttl = 0;
	worldlistConfirmReset(&ts->world_confirm);   // a delete armed last visit must not survive it

	if (ts->world_list_truncated) {
		snprintf(ts->status, sizeof(ts->status), "showing first %d worlds", WORLDLIST_MAX);
		ts->status_ttl = 180;   // ~3 s at the project's measured 59.83 fps; see main.c
	}
}

// Every failure branch below is something the player must actually act on — retype the
// name, or notice the SD card write is failing — unlike titleEnterPlayTab's truncation
// notice above, which is only ever informational. That one keeps the 180-frame (~3 s at the
// project's measured 59.83 fps, see main.c) budget; these get roughly double, because a
// message the player has to read *and* respond to before it clears itself needs longer than
// one they can just glance at.
#define WORLD_ERROR_STATUS_TTL 360

// The software keyboard applet, invoked directly from the "New World" button. This blocks
// until the player confirms or cancels — a modal system applet, not a per-frame thing this
// file's "never calls hidScanInput" rule was written about (see title.h's file comment):
// the frame loop is not what is reading input while swkbd owns the screen, libctru is.
static void titleCreateWorldFlow(TitleState* ts, TitleResult* r)
{
	SwkbdState kbd;
	swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, WORLDLIST_NAME_MAX - 1);
	swkbdSetHintText(&kbd, "World name");
	// Backslash is filtered by the keyboard itself; forward slash has no swkbd filter flag,
	// so it — and everything else outside worldlistNameValid's charset — is caught below
	// instead. Two layers on purpose: the filter stops the common case before it is even
	// typed, and worldlistNameValid is the one gate nothing gets past regardless.
	swkbdSetValidation(&kbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_BACKSLASH, 0);

	char name[WORLDLIST_NAME_MAX];
	name[0] = '\0';
	const SwkbdButton btn = swkbdInputText(&kbd, name, sizeof(name));

	// swkbdInit above was called with numButtons=2 and neither button was renamed via
	// swkbdSetButton, so the two buttons libctru actually shows are its defaults for that
	// case: SWKBD_BUTTON_LEFT ("usually Cancel") and SWKBD_BUTTON_RIGHT == SWKBD_BUTTON_CONFIRM
	// ("usually OK") — see the SwkbdButton comments in swkbd.h. SWKBD_BUTTON_MIDDLE cannot
	// fire here; a 2-button dialog has no middle button to press. A genuine Cancel is the one
	// outcome this screen must stay exactly as silent about as it always has; everything else
	// below means the applet did not finish the way the player asked it to.
	if (btn == SWKBD_BUTTON_LEFT) return;   // player cancelled; stay on world select, no message

	if (btn != SWKBD_BUTTON_CONFIRM) {
		// Left as the only silent case above means anything reaching here is
		// SWKBD_BUTTON_NONE in practice — swkbd.h's own doc comment on it says this is what
		// swkbdInputText returns "in special cases" instead of a button at all: out of
		// memory, invalid parameters, or a HOME/soft-reset/power-button interrupt (this
		// screen enables none of the parental-PIN or filter-callback features that add more
		// such cases). swkbdGetResult() is the only way to see which afterwards, so the
		// numeric SwkbdResult goes straight into the status line — a photo of the bottom
		// screen is then enough to look the value up in swkbd.h without reproducing whatever
		// real hardware just hit, which is exactly what this machine cannot do (this flow is
		// verified end-to-end here; the report this is responding to is not reproducible on
		// this machine).
		snprintf(ts->status, sizeof(ts->status), "keyboard error %d - retry",
		         (int)swkbdGetResult(&kbd));
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	if (!worldlistNameValid(name)) {
		// worldlistNameValid's own comment (scene/worldlist.h) is the actual rule: non-empty,
		// no leading/trailing space, under WORLDLIST_NAME_MAX bytes, only [A-Za-z0-9 _-].
		// swkbdSetValidation above already stops a fully empty or all-blank name, and
		// maxTextLength already stops an overlong one, so in practice this fires on a
		// disallowed character — most likely '/', which has no SWKBD_FILTER_* flag of its
		// own (see the comment on swkbdSetValidation above) — or on whitespace at one end of
		// an otherwise non-blank name. Naming the allowed charset here beats making the
		// player guess which of those it was.
		snprintf(ts->status, sizeof(ts->status), "bad name: use A-Z 0-9 _ - only");
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	if (!worldlistCreate(REGION_ROOT, name)) {
		// worldlistCreate's own comment (scene/worldlist.h) says false means one of two
		// things: an invalid name (already ruled out by worldlistNameValid above) or the
		// target directory still not existing once mkdir has been tried. With the name
		// already known good, that leaves exactly one real cause — the SD card write itself
		// failed (card missing, write-protected, full, or REGION_ROOT's parent not mounted)
		// — which is worth saying plainly instead of the old message that gave no hint where
		// to even start looking.
		snprintf(ts->status, sizeof(ts->status), "world create failed (sd write)");
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	// A name that already belonged to a world is not an error — see worldlistCreate's own
	// comment — so this both makes a fresh world and re-opens an existing one typed by name,
	// and either way the player just told this screen exactly what they want to do with it:
	// play it. Making them find it in the list a second time would be busywork.

	// v1.6.0 Phase A: every world gets its registry sidecar at creation — dynamic block
	// defs only, the core rows are implied by the binary. Empty today (nothing on this
	// console registers dynamic rows yet); genStart() loads it back before any column is
	// decoded, and Phase B grows into it. The write's return value is deliberately not
	// an error here: an absent sidecar reads back exactly like an empty one (no dynamic
	// rows), so a failed SD write costs nothing today and nothing later.
	char reg_path[96];
	snprintf(reg_path, sizeof reg_path, "%s/%s/registry.bin", REGION_ROOT, name);
	(void)registrySidecarSave(reg_path);

	r->action = TITLE_START_WORLD;
	snprintf(r->world_name, sizeof(r->world_name), "%s", name);
}

// v1.9.0 item 6.3: the same applet, opened from X on a world row with that world's current
// name already in the field, so a one-letter fix is a one-letter edit. Everything after the
// keyboard closes — the validation, the duplicate check, the disk write, the rescan and where
// the cursor lands — is worldlistRenameAt's (scene/worldlist.h), proven on the host by
// tests/worldlist_ops_test.c; this stays the glue titleCreateWorldFlow above is.
static void titleRenameWorldFlow(TitleState* ts)
{
	if (ts->cursor < 0 || ts->cursor >= ts->world_count) return;   // worldlistUiStep already gates this

	SwkbdState kbd;
	swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, WORLDLIST_NAME_MAX - 1);
	swkbdSetHintText(&kbd, "New world name");
	swkbdSetInitialText(&kbd, ts->worlds[ts->cursor].name);
	// Same two layers as titleCreateWorldFlow: the applet's own filter, then the real gate.
	swkbdSetValidation(&kbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_BACKSLASH, 0);

	char name[WORLDLIST_NAME_MAX];
	name[0] = '\0';
	const SwkbdButton btn = swkbdInputText(&kbd, name, sizeof(name));

	if (btn == SWKBD_BUTTON_LEFT) return;   // player cancelled: as silent as a cancelled create

	if (btn != SWKBD_BUTTON_CONFIRM) {
		// The same "special cases" set titleCreateWorldFlow's identical branch documents.
		snprintf(ts->status, sizeof(ts->status), "keyboard error %d - retry",
		         (int)swkbdGetResult(&kbd));
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	const WorldlistRenameResult res = worldlistRenameAt(REGION_ROOT, ts->worlds, WORLDLIST_MAX,
	                                                    &ts->world_count, &ts->world_list_truncated,
	                                                    &ts->cursor, &ts->world_confirm, name);
	const char* msg = worldlistRenameResultText(res);
	if (msg) {
		snprintf(ts->status, sizeof(ts->status), "%s", msg);
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
	}
}

// The second Y press on the same row. worldlistDeleteAt (scene/worldlist.h) does the delete,
// the rescan and the cursor; the one thing that is this file's is the scroll window, which
// the list can now be shorter than.
static void titleDeleteSelectedWorld(TitleState* ts)
{
	const bool ok = worldlistDeleteAt(REGION_ROOT, ts->worlds, WORLDLIST_MAX, &ts->world_count,
	                                  &ts->world_list_truncated, &ts->cursor, &ts->world_confirm);
	if (!ok) {
		// v1.9.x audit fix. worldlistDeleteAt now rescans on failure too (see its own comment):
		// worldlistRmTree's fail-closed walk can already have removed part of the tree before
		// the failure that stopped it, so what is on screen after this call is the post-attempt
		// truth, not a frozen pre-attempt snapshot papering over a directory that is no longer
		// what it was. "incomplete" rather than "failed" for the same reason — this covers both
		// "nothing was touched" and "some of it is gone", and claiming neither more nor less than
		// that is the whole point of the fix. The errno number is enough to look up in <errno.h>
		// from a photo of the bottom screen, exactly like the keyboard-error line above.
		snprintf(ts->status, sizeof(ts->status), "delete incomplete (errno %d)", errno);
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
	}

	// world_count can move on EITHER outcome now that a failed delete also rescans, so the
	// scroll window is reclamped either way rather than only after a clean delete. v1.9.1:
	// against the bar's six-row window and its 1 + world_count rows (row 0 is NEW WORLD, so
	// the world at ts->cursor is bar row ts->cursor + 1), through the same barNavScrollFor
	// the tab uses every frame rather than a second hand-rolled clamp that could disagree
	// with it about where the window is allowed to sit.
	ts->world_scroll = barNavScrollFor(ts->cursor + 1, ts->world_scroll, TITLE_ROWS_VISIBLE,
	                                    1 + ts->world_count);
}

// The PLAY tab. Row 0 is NEW WORLD, rows 1..world_count are the worlds; six rows are visible
// and ts->world_scroll is the window, which the caller has already moved to follow the cursor.
//
// `commit` is this frame's BAR_EV_COMMIT (A on the focused row), and `ui` is worldlistUiStep's
// answer, both decided by the caller — see drawBar for why X, Y and B are taken out of the word
// barnav sees before it sees it, and why the world verbs run after the cursor has moved.
//
// ts->cursor holds the focused WORLD index (nav.row - 1, so -1 while NEW WORLD is focused)
// throughout this tab: worldlistUiStep, worldlistRenameAt and worldlistDeleteAt all deal in
// world indices, and giving them a bar row that is one out would delete the wrong world.
static TitleResult drawPlayTab(TitleState* ts, const TitleInput* in, bool tap, bool commit,
                               WorldlistUiAction ui)
{
	TitleResult r = {TITLE_STAY, {0}};

	// A frame on which X/Y/B did something does not also load a row: the row under the cursor
	// may no longer be the one the player was looking at when they pressed.
	//
	// v1.9.1: the SAME gate now applies to the touch half. uiButton fires on
	// `tapped_here || (focused && a_down)` and only the second half took the gate before, so a
	// frame carrying both a stylus tap on a world row and a Y press armed the delete AND loaded
	// the world — a rename prompt or a delete confirmation opening on top of a world that is
	// already loading. Rare (it needs a finger and a face button on one frame) but it is the
	// destructive direction, and it is one word to close. The cursor still MOVES on a tap
	// regardless: drawBar's row-tap handler runs off the ungated tap, so nothing about where
	// the highlight goes changes here — only whether the row also activates.
	const bool row_live = ui == WORLDLIST_UI_NONE;
	const bool a        = commit && row_live;
	tap                 = tap && row_live;

	const int rows = 1 + ts->world_count;

	if (ts->world_count == 0)
		fontDraw(10, TITLE_ROW_Y0 + TITLE_ROW_H + 10, 1, COL_TEXT_DIM,
		         "No worlds yet - pick NEW WORLD");

	for (int v = 0; v < TITLE_ROWS_VISIBLE; v++) {
		const int i = ts->world_scroll + v;
		if (i >= rows) break;

		const TRect rr      = titleRowRect(v);
		const bool  focused = (ts->nav.row == i);

		if (i == 0) {
			if (uiButton(rr, "NEW WORLD", focused, tap, in->touch_x, in->touch_y, a))
				titleCreateWorldFlow(ts, &r);
		} else if (uiButton(rr, ts->worlds[i - 1].name, focused, tap, in->touch_x, in->touch_y,
		                     a)) {
			r.action = TITLE_START_WORLD;
			snprintf(r.world_name, sizeof(r.world_name), "%s", ts->worlds[i - 1].name);
		}
	}

	// While a delete is armed for the focused row the prompt takes the whole footer and names
	// the world, and it is drawn off the SAME predicate the next Y press goes through
	// (worldlistConfirmArmedFor), so the text and the behaviour cannot disagree about which
	// world. The footer is 312 px from x=8, 52 chars at FONT_ADVANCE 6; the 24-char prefix
	// leaves 27 for the name, so a 28..31-char name is cut in the PROMPT only — the highlighted
	// row above it still shows it in full. It drops on its own after WORLDLIST_CONFIRM_FRAMES
	// (worldlistConfirmTrack, inside worldlistUiStep), on B, or when the cursor moves.
	if (ts->cursor >= 0 && ts->cursor < ts->world_count &&
	    worldlistConfirmArmedFor(&ts->world_confirm, ts->cursor)) {
		char prompt[24 + WORLDLIST_NAME_MAX];
		snprintf(prompt, sizeof(prompt), "PRESS Y AGAIN TO DELETE %.27s",
		         ts->worlds[ts->cursor].name);
		fontDraw(8, TITLE_FOOTER_Y, 1, COL_WARN, prompt);
	} else {
		drawTitleFooter(ts, (ts->cursor >= 0) ? "A PLAY  X RENAME  Y DELETE  L/R TAB"
		                                      : "A CREATE A WORLD  L/R TAB");
	}

	return r;
}

// The OPTIONS tab: the five settings rows plus CONTROLS >, on the shared six-row grid.
//
// `commit` is BAR_EV_COMMIT and `left`/`right` are BAR_EV_STEP_LEFT/_RIGHT — this is the one
// tab whose horizontal_is_content is true, so barnav turns D-pad left/right into a STEP here
// instead of a tab change (blueprint D10). settingRowStepper's own `focused` test is what
// decides which row a step lands on, and barnav guarantees exactly one row is focused, so the
// two cannot disagree about which value moved.
//
// There is no BACK row and no save button. Leaving the tab is what saves — see drawBar's one
// optionsSave call — so a change made here reaches the card whether the player leaves by L/R,
// by B, by a tab tap or by walking off to PLAY and loading a world.
static void drawOptionsTab(TitleState* ts, Options* opts, const TitleInput* in, bool tap,
                            bool commit, bool left, bool right)
{
	char buf[32];
	int  step;

	snprintf(buf, sizeof(buf), "%d", opts->render_dist);
	step = settingRowStepper(titleRowRect(0), "RENDER DIST", buf, ts->nav.row == 0,
	                          tap, in->touch_x, in->touch_y, left, right);
	// v1.8.5: per-console ceiling. Without this the stepper refuses at 3 on a New 3DS while
	// the pause-menu slider and the ini clamp both allow 5 — the setting would look raisable
	// in one place and silently refuse in another.
	if (step) opts->render_dist = renderDistClampFor(opts->render_dist + step, hwIsNew3ds());

	snprintf(buf, sizeof(buf), "%.1f", (double)opts->slider_3d);
	step = settingRowStepper(titleRowRect(1), "3D DEPTH", buf, ts->nav.row == 1,
	                          tap, in->touch_x, in->touch_y, left, right);
	if (step) opts->slider_3d = clampF(opts->slider_3d + (float)step * 0.1f,
	                                    OPTIONS_SLIDER_MIN, OPTIONS_SLIDER_MAX);

	// v1.9.1: `left || right` as well as `commit`. A two-value setting IS a stepper with two
	// stops, so either direction lands on the other one — there is no direction in which a
	// boolean can be stepped "further". Without this, D-pad Left/Right were dead on this row
	// while the footer two lines below promised "LEFT/RIGHT ADJUST" for the whole tab.
	//
	// The dead direction was NOT barnav's doing: barnav.c:197-198 fires BAR_EV_STEP_LEFT/RIGHT
	// for every 1-column row in a horizontal_is_content category, this row included, and
	// title.c:1772 forwards it. The event arrived and had nowhere to go, because
	// settingRowToggle has no left/right parameter to hand it to. Fixed here at the one call
	// site rather than by giving settingRowToggle two more parameters it would ignore.
	const bool toggled = settingRowToggle(titleRowRect(2), "INVERT LOOK", opts->invert_look,
	                                       ts->nav.row == 2, tap, in->touch_x, in->touch_y);
	if (toggled || (ts->nav.row == 2 && (commit || left || right)))
		opts->invert_look = !opts->invert_look;

	snprintf(buf, sizeof(buf), "%.2f", (double)opts->look_sensitivity);
	step = settingRowStepper(titleRowRect(3), "LOOK SENS", buf, ts->nav.row == 3,
	                          tap, in->touch_x, in->touch_y, left, right);
	if (step) opts->look_sensitivity = clampF(opts->look_sensitivity + (float)step * 0.25f,
	                                           OPTIONS_SENS_MIN, OPTIONS_SENS_MAX);

	// v1.8.10 shaders option: fake directional lighting, off by default (see
	// chunk_render.c's chunkRenderSetFakeShading for what this actually turns on).
	const bool shading_toggled = settingRowToggle(titleRowRect(4), "SHADING", opts->fake_shading,
	                                               ts->nav.row == 4, tap, in->touch_x,
	                                               in->touch_y);
	// v1.9.1: same as INVERT LOOK above — either direction flips a two-stop setting.
	if (shading_toggled || (ts->nav.row == 4 && (commit || left || right)))
		opts->fake_shading = !opts->fake_shading;

	// v1.9.1: RIGHT opens the bindings screen, LEFT deliberately does not. This is the one place
	// this pass did NOT make both directions live, and the reason is the row's own label: the
	// ">" is a direction, and a leftward press opening a screen the label points rightwards to
	// would be a worse surprise than a press that does nothing. A is still the primary way in
	// and the footer still leads with "A SELECT".
	//
	// uiButton ANDs its a_down against `focused` itself (title.c:230), and `focused` here is
	// `ts->nav.row == 5`, so `right` needs no second row test — the same reason `commit` never
	// carried one.
	if (uiButton(titleRowRect(5), "CONTROLS >", ts->nav.row == 5, tap, in->touch_x, in->touch_y,
	             commit || right)) {
		ts->screen = TITLE_SCR_OPTIONS_BINDINGS;
		ts->cursor = 0;
	}

	drawTitleFooter(ts, "A SELECT  LEFT/RIGHT ADJUST  L/R TAB");
}

// The SYSTEM tab: the two update screens and Quit — everything that is about the build rather
// than about a world or a setting. CHECK FOR UPDATE and VERSION HISTORY were both one level
// deeper before (options-general's third pinned button, and a small button on the screen behind
// it); both are now one tab press and one A away, which is the ≤ 2 inputs blueprint 1B.7 asks
// for.
static TitleResult drawSystemTab(TitleState* ts, const TitleInput* in, bool tap, bool commit)
{
	TitleResult r = {TITLE_STAY, {0}};

	// The updater's state survives a trip out of these screens and back, so notes from a
	// previous visit can still be there; the scroll position that went with them must not be.
	// Starting anywhere but the top of a changelog reads as a missing first line.
	if (uiButton(titleRowRect(0), "CHECK FOR UPDATE", ts->nav.row == 0, tap, in->touch_x,
	             in->touch_y, commit)) {
		ts->screen = TITLE_SCR_UPDATE;
		ts->cursor = 0;
		ts->notes_scroll               = 0;
		ts->notes_rep_up.held_frames   = 0;
		ts->notes_rep_down.held_frames = 0;
	}

	if (uiButton(titleRowRect(1), "VERSION HISTORY", ts->nav.row == 1, tap, in->touch_x,
	             in->touch_y, commit)) {
		ts->screen         = TITLE_SCR_VERSION_HISTORY;
		ts->vh_from_update = false;   // B on that leaf comes back HERE, not to a screen never opened
		ts->cursor    = 0;
		ts->vh_cursor = 0;
		ts->vh_scroll = 0;
		ts->notes_scroll               = 0;
		ts->notes_rep_up.held_frames   = 0;
		ts->notes_rep_down.held_frames = 0;
	}

	if (uiButton(titleRowRect(2), "QUIT", ts->nav.row == 2, tap, in->touch_x, in->touch_y,
	             commit))
		r.action = TITLE_QUIT;

	drawTitleFooter(ts, "A SELECT  L/R TAB");
	return r;
}

// ── Options: key bindings ─────────────────────────────────────────────────────────────

static void drawOptionsBindings(TitleState* ts, Options* opts, const TitleInput* in, bool tap)
{
	const bool a = (in->keys_down & KEY_A) != 0;

	if (ts->rebind_action >= 0) {
		// Modal capture: the next key that is one of the seven valid binding bits wins;
		// anything else this frame (nothing yet, or a key this build never binds) is
		// ignored, so brushing the D-pad while aiming the stylus at a row cannot finish the
		// capture by accident. B cancels without changing the binding.
		fontDraw(20, 90, 1, COL_TEXT, "Press a button for:");
		fontDraw(20, 102, 2, COL_ACCENT, actionLabel(ts->rebind_action));
		fontDraw(20, 130, 1, COL_TEXT_DIM, "(B cancels)");

		if (in->keys_down & KEY_B) {
			ts->rebind_action = -1;
		} else if (in->keys_down) {
			for (int k = 0; k < OPTIONS_VALID_KEY_COUNT; k++) {
				if (in->keys_down & OPTIONS_VALID_KEYS[k]) {
					opts->bindings[ts->rebind_action] = OPTIONS_VALID_KEYS[k];
					ts->rebind_action = -1;
					break;
				}
			}
		}
		return;   // nothing else on this screen is live while a capture is in progress
	}

	if (in->keys_down & KEY_DDOWN) ts->cursor = (ts->cursor + 1) % (ACTION_COUNT + 1);
	if (in->keys_down & KEY_DUP)   ts->cursor = (ts->cursor + ACTION_COUNT) % (ACTION_COUNT + 1);

	fontDraw(8, 4, 1, COL_TEXT_DIM, "CONTROLS");

	for (int i = 0; i < ACTION_COUNT; i++) {
		const TRect rr = {10, (float)(BIND_ROW_Y + i * BIND_ROW_H), SCR_W - 20, BIND_ROW_H - 1};
		const bool focused = ts->cursor == i;

		spriteRect(rr.x, rr.y, rr.w, rr.h, focused ? COL_PANEL_HI : COL_PANEL);
		if (focused) spriteRect(rr.x, rr.y, rr.w, 2, COL_ACCENT);
		fontDraw(rr.x + 4, rr.y + (rr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, actionLabel(i));

		const char* keyname = keyBitLabel(opts->bindings[i]);
		const int kw = fontTextWidth(keyname, 1);
		fontDraw(rr.x + rr.w - (float)kw - 6, rr.y + (rr.h - FONT_GLYPH_H) * 0.5f, 1,
		         COL_ACCENT, keyname);

		const bool tapped_here = tap && ptIn(rr, in->touch_x, in->touch_y);
		if (tapped_here || (focused && a)) ts->rebind_action = i;
	}

	const TRect back_r = {10, (float)BIND_BACK_Y, SCR_W - 20, LIST_BTN_H};
	if (uiButton(back_r, "BACK", ts->cursor == ACTION_COUNT, tap, in->touch_x, in->touch_y, a)
	    || (in->keys_down & KEY_B)) {
		// Back to the bar, which is still focused on the OPTIONS tab's CONTROLS > row: this
		// leaf never touched ts->nav, so the cursor is exactly where it was left and nothing
		// here has to name a row index. (Before v1.9.1 this wrote ts->cursor = 4 with a
		// comment claiming that was "CONTROLS >"; on the eight-item options page CONTROLS >
		// was item 5 and item 4 was SHADING, so B out of this screen landed one row high.)
		//
		// Not a save point. Leaving the OPTIONS *tab* is what writes the file (drawBar), and
		// this returns to that tab rather than leaving it — so a binding change survives a
		// battery pull exactly as soon as a general-settings change does, no sooner, which is
		// the rule options-general's own Back button held before.
		ts->screen = TITLE_SCR_MAIN;
	}
}

// ── Multiplayer ────────────────────────────────────────────────────────────────────────
//
// Unlike PLAY/OPTIONS/SYSTEM this tab is not a list of rows on the shared TITLE_ROW_* grid,
// so it does not use titleRowRect: its content (address, status, error, key, hint, player
// list) is prose, and its two actions are pinned buttons under it. Same pinned-button idiom
// it has always had, re-solved for the bar's chrome:
//
//   MP_BTN2_Y = TITLE_FOOTER_Y(228) - gap(6) - MP_BTN_H(36) = 186
//   MP_BTN1_Y = MP_BTN2_Y - gap(4) - MP_BTN_H(36)           = 146
//
// which leaves TITLE_ROW_Y0(52)..MP_BTN1_Y-6 = 52..140 for the body. v1.9.1 (blueprint D10):
// "existing body shifted to y >= 52, BACK button removed (B/tabs)". Both halves of that are
// visible in the arithmetic — every MP_ROW_* below is EXACTLY its old value plus 28, and the
// 40 px the removed BACK slot gives back is exactly what pays for the 28 px of tab strip
// above and the 12 px of footer below. The content band is 94 px either way: it was 24..118
// (above the old third slot) and it is now 52..146, the same height, moved down by 28.
//
// Every row Y below is a literal rather than a formula, the same way drawPlayTab's and
// drawOptionsTab's row content is literal per-row code — only the *count*-driven layouts
// (BIND_ROW_H, TITLE_TAB_W) earn one.
#define MP_BTN_H            36
#define MP_BTN2_Y           (TITLE_FOOTER_Y - 6 - MP_BTN_H)
#define MP_BTN1_Y           (MP_BTN2_Y - 4 - MP_BTN_H)

#define MP_ROW_SERVER        52
#define MP_ROW_STATUS        61
#define MP_ROW_ERROR         70
#define MP_ROW_KEY_LABEL     82
#define MP_ROW_KEY_HEX       91   // + a second line at +FONT_LINE(9) via fontDraw's own '\n'
#define MP_ROW_HINT         112
#define MP_ROW_PLAYERS_HDR  121
#define MP_ROW_PLAYERS_TOP  130
#define MP_ROW_STEP           9

_Static_assert(MP_ROW_SERVER >= TITLE_ROW_Y0, "the multiplayer body must clear the tab strip");

// How many player rows fit under MP_ROW_PLAYERS_TOP: the 5th row's glyphs (130 + 4*9 = 166,
// +FONT_GLYPH_H(7) = 173) clear the only button that can be on screen at the same time —
// DISCONNECT, at MP_BTN2_Y(186) — with 13 px to spare; a 6th (182..189) would not. The upper
// button slot at MP_BTN1_Y(146) does overlap where a 3rd-to-5th player row would draw, and
// that is safe rather than lucky for exactly the reason it always was: that slot is only ever
// occupied by "Have an invite code?" while !connected, and netPlayerCount() is documented in
// bsnet.h as 0 whenever not connected, so there is no state in which a player row and that
// button are both on screen. NET_MAX_PLAYERS (bsnet.h) is 16, so this can genuinely be fewer
// rows than the room holds — see the "+N more" fallback below, the same honesty
// titleEnterPlayTab's own truncation flag applies to a directory listing.
#define MP_PLAYERS_VISIBLE    5

// The software keyboard applet for the "Have an invite code?" item below — same modal-
// applet shape as titleCreateWorldFlow above, and for the same reason: this blocks the
// frame loop, but libctru's swkbd is what is reading input while it owns the screen, not
// this file's own per-frame code.
//
// Unlike a world name, this gets no swkbdSetValidation call at all — not calling it leaves
// swkbd's own default (SWKBD_ANYTHING, applets/swkbd.h), which is exactly what
// CLIENT-ENROLMENT-SPEC.md asks for: the server normalises the code (case, spacing,
// hyphens) before checking it, so any filtering here could only ever reject something the
// server would have accepted.
static void titleInviteCodeFlow(TitleState* ts)
{
	SwkbdState kbd;
	swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, NET_INVITE_MAX - 1);
	swkbdSetHintText(&kbd, "Have an invite code?");

	char code[NET_INVITE_MAX];
	code[0] = '\0';
	const SwkbdButton btn = swkbdInputText(&kbd, code, sizeof(code));

	// Same two default buttons and the same silent-cancel rule as titleCreateWorldFlow
	// above: SWKBD_BUTTON_LEFT is Cancel and stays exactly that silent, and anything other
	// than SWKBD_BUTTON_CONFIRM reaching past it is the applet not finishing the way the
	// player asked (out of memory, bad params, or a HOME/soft-reset/power interrupt — see
	// swkbdGetResult()'s own doc comment) — so the numeric SwkbdResult goes into the status
	// line for the same reason it does there.
	if (btn == SWKBD_BUTTON_LEFT) return;

	if (btn != SWKBD_BUTTON_CONFIRM) {
		snprintf(ts->status, sizeof(ts->status), "keyboard error %d - retry",
		         (int)swkbdGetResult(&kbd));
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	// netConnectWithInvite() runs the same handshake/netUpdate() loop netConnect() already
	// does (net/bsnet.h), so, exactly like the CONNECT button below, this just fires it and
	// leaves the result to the status/error row drawMultiplayer already draws every frame.
	// No local success/failure handling belongs here: failure can take up to ~12 s to land
	// (the server's 10 s enrolment window plus handshake), long after this call returns.
	netConnectWithInvite(code);
}

// The MULTIPLAYER tab. `commit` is BAR_EV_COMMIT and `back_press` is BAR_EV_BACK — see drawBar
// for why the B that produces `back_press` is applied to the tab strip only AFTER this function
// has run, which is what keeps it a live input to titleMpNav rather than a hard-coded false.
//
// *leave_to_play is set when titleMpNav says the player asked to leave, and drawBar is what
// acts on it by focusing the PLAY tab. The decision stays inside titleMpNav rather than being
// re-derived from back_press in the caller for the reason titleMpNav exists at all: leaving and
// the server-entry gate are not independent, and this is the one function that is allowed to
// answer both.
static TitleResult drawMultiplayer(TitleState* ts, const TitleInput* in, bool tap, bool commit,
                                    bool back_press, bool* leave_to_play)
{
	TitleResult r = {TITLE_STAY, {0}};

	const NetStatus st = netStatus();
	const bool connected = (st == NET_CONNECTED);

	// The invite item only makes sense when this console has no session to protect —
	// offering "Have an invite code?" while already connected could only ever be tapped by
	// mistake, so it disappears from the list entirely rather than sitting there disabled,
	// the same convention drawUpdate's action_label/action_idx uses below for a verb that
	// does not apply yet: each slot keeps a fixed Y, but the index and row count shrink to
	// skip whatever is not offered right now. titleBuildCats holds the matching row count
	// (1 connected, 2 not), so barnav cannot leave the cursor on a row this frame does not
	// draw.
	//
	// CONNECT stays row 0 in both layouts. Stacking the new button on top and letting the
	// others slide down would have been the obvious arrangement, but the cursor starts at 0
	// and reads top-to-bottom everywhere in this file, so it would also have moved the
	// default focus off CONNECT — what nearly every visit to this tab is for — and onto a
	// button that matters once in a console's life.
	//
	// v1.9.1: BACK is gone (blueprint D10 — the tab strip and B are the way out), so the
	// disconnected tab reads CONNECT / invite and the connected one is DISCONNECT alone,
	// pinned to the same bottom slot the old screen's bottom button always sat in.
	const int conn_idx   = 0;
	const int invite_idx = connected ? -1 : 1;
	const bool a = commit;

	char addr_line[8 + NET_ADDR_MAX];
	snprintf(addr_line, sizeof(addr_line), "Server: %s", netServerAddress());
	fontDraw(8, MP_ROW_SERVER, 1, COL_TEXT, addr_line);

	fontDraw(8, MP_ROW_STATUS, 1, (st == NET_FAILED) ? COL_WARN : COL_TEXT, netStatusText());

	// netErrorText() and netStatusText() are used verbatim throughout — see title.h's
	// header on why this file never writes its own network wording. This row has three
	// things that can want it and only room for one, so it picks in order of how directly
	// each answers what the player just did: a keyboard error from titleInviteCodeFlow
	// above (ts->status) outranks a stale netErrorText() from a previous attempt, which
	// outranks the one-time "just enrolled" confirmation, which only ever appears once
	// nothing else needs the line.
	const char* err = netErrorText();
	if (ts->status_ttl > 0) {
		fontDraw(8, MP_ROW_ERROR, 1, COL_WARN, ts->status);
	} else if (err[0] != '\0') {
		fontDraw(8, MP_ROW_ERROR, 1, COL_WARN, err);
	} else if (netJustEnrolled()) {
		fontDraw(8, MP_ROW_ERROR, 1, COL_ACCENT, "Enrolled - you're on the list now");
	} else if (connected && networldWorldSeed(NULL) && networldRegistryWaiting()) {
		// v1.6.0 task 8. The seed is in, so the row below no longer describes what is being
		// waited for, but the block table is not settled and the gate at the bottom of this
		// function is holding entry open for it. Bounded — see networld.h on
		// networldRegistryWaiting() — so this is on screen for at most the sync deadline and
		// usually for a single round trip. It says something different from the row below
		// because it IS something different: the world is known, the blocks in it are not.
		fontDraw(8, MP_ROW_ERROR, 1, COL_TEXT_DIM, "Joined - syncing block table...");
	} else if (connected) {
		// Only ever on screen between the handshake completing and the server naming its
		// world, which is one round trip — the seed arrives in BS_APP_WORLD_INFO, the first
		// packet the server sends after JOIN. It stays up indefinitely against a server too
		// old to send one, which is the intended outcome: see the gate at the bottom of this
		// function for why waiting forever beats guessing a seed.
		fontDraw(8, MP_ROW_ERROR, 1, COL_TEXT_DIM, "Joined - waiting for the world...");
	}

	fontDraw(8, MP_ROW_KEY_LABEL, 1, COL_TEXT_DIM, "YOUR KEY:");

	// netLocalKeyHex() is 64 hex characters wide (384 px at scale 1), well past the 300 px
	// a bottom-screen line has to work with, so it is split across two lines here rather
	// than shrinking the font past legibility. The unavailable-key case is much shorter
	// than 64 characters, so it is left on one line rather than force-split at 32.
	const char* key = netLocalKeyHex();
	if (strlen(key) >= 64) {
		char wrapped[2 * 32 + 2];
		snprintf(wrapped, sizeof(wrapped), "%.32s\n%.32s", key, key + 32);
		fontDraw(8, MP_ROW_KEY_HEX, 1, COL_ACCENT, wrapped);
	} else {
		fontDraw(8, MP_ROW_KEY_HEX, 1, COL_ACCENT, key);
	}

	fontDraw(8, MP_ROW_HINT, 1, COL_TEXT_DIM, "Also saved to SD: /blocksmith/client.pub");

	const int player_count = netPlayerCount();
	char hdr[24];
	snprintf(hdr, sizeof(hdr), "PLAYERS (%d)", player_count);
	fontDraw(8, MP_ROW_PLAYERS_HDR, 1, COL_TEXT_DIM, hdr);

	const int shown = (player_count < MP_PLAYERS_VISIBLE) ? player_count : MP_PLAYERS_VISIBLE;
	for (int i = 0; i < shown; i++) {
		const float row_y = (float)(MP_ROW_PLAYERS_TOP + i * MP_ROW_STEP);

		if (i == MP_PLAYERS_VISIBLE - 1 && player_count > MP_PLAYERS_VISIBLE) {
			char more[24];
			snprintf(more, sizeof(more), "+ %d more", player_count - (MP_PLAYERS_VISIBLE - 1));
			fontDraw(8, row_y, 1, COL_TEXT_DIM, more);
			break;
		}

		const char* name = netPlayerName(i);
		if (!name) break;
		fontDraw(8, row_y, 1, netPlayerIsLocal(i) ? COL_ACCENT : COL_TEXT, name);
	}

	// Drawn in the order they are stacked. CONNECT rises into the upper slot only when the
	// invite button is present to fill the one below it, so the connected tab keeps its one
	// button pinned to the bottom the way the old screen's bottom button always was rather
	// than leaving a gap between it and the footer.
	const TRect conn_r = {10, (float)(connected ? MP_BTN2_Y : MP_BTN1_Y), SCR_W - 20, MP_BTN_H};
	if (uiButton(conn_r, connected ? "DISCONNECT" : "CONNECT", ts->nav.row == conn_idx, tap,
	             in->touch_x, in->touch_y, a)) {
		if (connected) netDisconnect(); else netConnect();
	}

	if (!connected) {
		const TRect invite_r = {10, (float)MP_BTN2_Y, SCR_W - 20, MP_BTN_H};
		if (uiButton(invite_r, "Have an invite code?", ts->nav.row == invite_idx, tap,
		             in->touch_x, in->touch_y, a)) {
			titleInviteCodeFlow(ts);
		}
	}

	drawTitleFooter(ts, "A SELECT  B BACK  L/R TAB");

	// v1.9.1: BACK the button is gone, so the only thing that still means "leave" on this tab
	// is B — barnav's BAR_EV_BACK, handed in as back_press. It is still gathered here, and
	// still fed through titleMpNav below, because that is exactly the frame the gate has to be
	// suppressed on; drawBar applies the tab move it causes only after this function returns.
	const bool back = back_press;

	// A server hosts the world, so joining one *is* entering it. This is the only place that
	// happens: there is deliberately no name prompt, no NEW WORLD and no trip through world
	// select on this path, because none of those describe anything about a world this console
	// does not own. World select stays exactly what it was, for single player only.
	//
	// Gated on the seed being known, not on `connected` alone. networldWorldSeed() answers
	// only once BS_APP_WORLD_INFO has arrived — the first packet the server sends after JOIN
	// — so this costs one round trip and buys terrain identical to every other player's. The
	// alternative, entering as soon as the handshake completes, would generate from this
	// client's own BS_WORLD_SEED constant: a world that looks like a successful join and is
	// a different world from the one everybody else is standing in. Waiting visibly (see the
	// "waiting for the world" row above) is the better failure.
	//
	// v1.6.0 task 8 added the second gate, for a near-identical reason one layer down. Entering
	// runs main.c's genStart(), and genStart() calls registryFreeze() before it starts the
	// worker — after which world/registry.c refuses every remote row for the rest of the
	// session. BS_APP_REGISTRY_DEFS are a full round trip behind the BS_APP_WORLD_INFO seed
	// this line already waits for, so without this the freeze always won and a server's
	// dynamic blocks could never be committed by anybody, ever: they arrived microseconds
	// after the only door they could have gone through was bolted. Holding entry here rather
	// than moving the freeze itself is what keeps the worker's no-locks-after-freeze contract
	// (world/registry.h) intact — this loop already draws a frame and pumps netUpdate() and
	// networldUpdate() every pass, so the wait costs nothing but the frames it takes.
	//
	// networldRegistryWaiting() is bounded in every arm (see networld.h), including the
	// pre-v1.6.0-server case where no BS_APP_REGISTRY_INFO is ever coming, so this cannot
	// become a second "waits forever" gate.
	//
	// Both halves of that gate, and BACK above it, now go through scene/title_nav.h rather than
	// sitting as two unrelated `if`s twenty lines apart. They were never independent: BACK set
	// the screen and fell straight through into this line, so pressing B on the one frame the
	// registry settled — or the frame the deadline expired, anywhere in a two-second wait this
	// screen explicitly invites the player to give up on — backed out AND entered the server's
	// world, and main.c acts on the action. See title_nav.h for why leaving wins, and
	// scene/title_nav_test.c for the frames that prove it.
	//
	// The two network reads are gathered here, AFTER this frame's buttons: that ordering is
	// what has always made DISCONNECT safe (netDisconnect() -> networldInit() clears the seed
	// on the same frame), and moving them above the button row would quietly break it.
	const TitleMpNav nav = {
		.back             = back,
		.connected        = connected,
		.have_world_seed  = networldWorldSeed(NULL),
		.registry_waiting = networldRegistryWaiting(),
		// v1.8.3 Phase 4. Gathered here with the other two and for the same reason: it is
		// cleared by netDisconnect() -> networldInit() on the frame DISCONNECT is pressed.
		.gen_waiting      = networldGenWaiting(),
	};
	const TitleMpNavOut nav_out = titleMpNav(nav);

	// v1.9.1: leaving means focusing the PLAY tab — the bar IS the title screen now, so there
	// is no TITLE_SCR_MAIN to go back to and no main-menu row to land the cursor on. drawBar
	// does the move (it owns nav and the one optionsSave that a category leave triggers).
	if (leave_to_play) *leave_to_play = nav_out.leave_to_main;
	if (nav_out.start_server) r.action = TITLE_START_SERVER;

	return r;
}

// ── Update ─────────────────────────────────────────────────────────────────────────────
//
// Content here is a handful of text lines and an optional bar, not rows, but it still ends
// in the same two-pinned-buttons shape world-select and options-general do, so it reuses
// LIST_BTN1_Y/LIST_BTN2_Y rather than earning its own Y budget the way Multiplayer's screen
// above had to (see that screen's own comment for why *its* content did not fit the shape).
static TitleResult drawUpdate(TitleState* ts, const TitleInput* in, bool tap)
{
	TitleResult r = {TITLE_STAY, {0}};
	const bool a = (in->keys_down & KEY_A) != 0;

	const bool available = updaterAvailable();
	const updateState st = updaterState();
	const bool busy = updaterBusy();

	// Which verb (if any) goes on the action button. Nothing is offered at all while busy
	// (CHECKING/DOWNLOADING/INSTALLING) or while the updater never got its services up -
	// updaterBusy()'s own contract is that the player must not be offered a way out then,
	// and updaterStartCheck()/updaterStartInstall() are both no-ops when !updaterAvailable().
	//
	// v1.8.4 moved the decision itself into app/updater_retry.c so it could be host-tested;
	// this is now the four comparisons and nothing else. The state it added: a FAILED that
	// was a failed download now offers RETRY DOWNLOAD and starts the download, where before
	// every FAILED alike offered CHECK NOW and made the player walk the whole check ->
	// available -> install path again to recover from a dropped transfer.
	const updaterAction action =
		updaterActionFor(available, busy, st == UPDATE_AVAILABLE, st == UPDATE_DONE,
		                 st == UPDATE_FAILED, updaterDownloadFailed());
	const char* action_label = updaterActionLabel(action);
	// Back stays offered right up to and including UPDATE_DONE - only the three busy states
	// take it away, per the task's own "BACK whenever !updaterBusy()" rule.
	const bool show_back = !busy;

	const int action_idx  = action_label ? 0 : -1;
	const int back_idx    = show_back ? (action_label ? 1 : 0) : -1;
	const int item_count  = (action_label ? 1 : 0) + (show_back ? 1 : 0);

	// ── Who owns the D-pad on this screen ─────────────────────────────────────────────
	//
	// steve asked for Up/Down to scroll the release notes on the top screen, and Up/Down is
	// also what moved the button cursor here before. Both cannot have it, so ownership is
	// decided once, here, in one place, and the two halves read *different HID words on
	// different bits*: the scroll reads keys_held (Up/Down), the cursor reads keys_down
	// (Left/Right). That is deliberate belt and braces against the bug this project has
	// shipped twice — one hidKeysDown() word consumed by two UI layers in the same loop
	// iteration (tools/run_host_tests.sh records the debugmenu_ui case: the A press that
	// opened the menu also stepped the render-distance slider).
	//
	// Left/Right for a two-item choice costs a controller user nothing — INSTALL and BACK sit
	// one press apart either way — and both buttons stay tappable and stay obvious, so the
	// accept path and the decline path are exactly as reachable as they were.
	//
	// The swap is tied to UPDATE_AVAILABLE rather than to "are the notes long enough to
	// scroll", so the D-pad does not change meaning under the player's thumb depending on how
	// many lines a release happened to write.
	const bool notes_visible = available && st == UPDATE_AVAILABLE;

	// Guarded rather than always taken: item_count is 0 for the whole of CHECKING/
	// DOWNLOADING/INSTALLING, and a modulo by 0 there would crash the console the first time
	// a player pressed D-pad down mid-download.
	if (item_count > 0) {
		const uint32_t next = notes_visible ? KEY_DRIGHT : KEY_DDOWN;
		const uint32_t prev = notes_visible ? KEY_DLEFT  : KEY_DUP;

		if (in->keys_down & next) ts->cursor = (ts->cursor + 1) % item_count;
		if (in->keys_down & prev) ts->cursor = (ts->cursor + item_count - 1) % item_count;
	}

	if (notes_visible) {
		// Repeat rate is app/whatsnew.h's WHATSNEW_REPEAT_DELAY/PERIOD, not a feel: one line
		// on the press, then ~15 a second after a third of a second held. Both directions keep
		// their own counter so holding one does not arm the other.
		ts->notes_scroll += whatsnewRepeatStep(&ts->notes_rep_down,
		                                        (in->keys_held & KEY_DDOWN) != 0);
		ts->notes_scroll -= whatsnewRepeatStep(&ts->notes_rep_up,
		                                        (in->keys_held & KEY_DUP) != 0);
		// The upper bound needs the laid-out line count, which only titleDrawTop has; it
		// clamps both ends there. This end is clamped here as well so the value never goes
		// negative even for the one frame between the two calls.
		if (ts->notes_scroll < 0) ts->notes_scroll = 0;
	} else {
		// Leaving the notes behind resets both counters, so a direction still held when the
		// state changes cannot carry a mid-repeat cadence into the next visit.
		ts->notes_rep_up.held_frames   = 0;
		ts->notes_rep_down.held_frames = 0;
		ts->notes_scroll               = 0;
	}

	// Version history browser's entry point. Deliberately tap-only: it does not join
	// ts->cursor's item_count cycle above (Left/Right when notes_visible, Up/Down otherwise -
	// see "who owns the D-pad on this screen" just above), so adding it cannot disturb either
	// of those two already-balanced mappings. steve's brief asked for the BROWSER itself to be
	// D-pad driven ("using both the top screen and the bottom screen ... with the d pad"),
	// which is what drawVersionHistory below does once this button is tapped - not that every
	// path to reach it also has to be a D-pad path.
	const TRect vh_btn_r = {VH_ENTRY_BTN_X, VH_ENTRY_BTN_Y, VH_ENTRY_BTN_W, VH_ENTRY_BTN_H};
	if (uiButton(vh_btn_r, "VERSION HISTORY", false, tap, in->touch_x, in->touch_y, false)) {
		ts->screen    = TITLE_SCR_VERSION_HISTORY;
		// v1.9.1: version history is also a SYSTEM tab row now, so it has two ways in and B out
		// of it has to come back to whichever one was used. Recorded here rather than inferred
		// afterwards — by the time that leaf runs, nothing distinguishes the two paths.
		ts->vh_from_update = true;
		ts->cursor    = 0;
		ts->vh_cursor = 0;
		ts->vh_scroll = 0;
		ts->notes_scroll               = 0;
		ts->notes_rep_up.held_frames   = 0;
		ts->notes_rep_down.held_frames = 0;
	}

	fontDraw(VH_ENTRY_BTN_X + VH_ENTRY_BTN_W + 6, 4, 1, COL_TEXT_DIM, "UPDATE");

	float y = LIST_TOP_Y;
	char line[64];

	snprintf(line, sizeof(line), "THIS BUILD: v%s",
	         BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "not set");
	fontDraw(10, y, 1, COL_TEXT, line);
	y += FONT_LINE;

	// Empty until a check has reported UPDATE_AVAILABLE or later - see updater.h's own
	// comment on updaterLatestVersion() - so this is skipped rather than drawn blank before
	// that.
	const char* latest = updaterLatestVersion();
	if (latest[0] != '\0') {
		snprintf(line, sizeof(line), "LATEST: %s", latest);
		fontDraw(10, y, 1, COL_TEXT, line);
		y += FONT_LINE;
	}
	y += 4;

	if (!available) {
		fontDraw(10, y, 1, COL_WARN, "UPDATES UNAVAILABLE - NETWORK SERVICES FAILED");
	} else {
		const char* msg = updaterMessage();
		if (msg[0] != '\0') {
			fontDraw(10, y, 1, (st == UPDATE_FAILED) ? COL_WARN : COL_TEXT, msg);
			// v1.8.19: updaterMessage() can now come back multi-line (see updater.c's
			// runCheck()/runInstall() failure strings) -- advance by however many lines
			// fontDraw actually drew, not a flat one, or the widgets below overlap it.
			int lines = 1;
			for (const char* p = msg; *p; p++) if (*p == '\n') lines++;
			y += FONT_LINE * lines;
		}

		const int pct = updaterProgress();
		if (pct >= 0) {
			y += 4;
			const TRect bar_r = {10, y, SCR_W - 20, 14};
			uiProgressBar(bar_r, pct);
			y += 14;
		}

		// The player is looking at the bottom screen when the check lands, so the bottom
		// screen is where they have to be told the changelog exists and how to work it.
		if (notes_visible) {
			y += 6;
			fontDraw(10, y, 1, COL_ACCENT, "WHAT'S NEW IS ON THE TOP SCREEN");
			y += FONT_LINE;
			fontDraw(10, y, 1, COL_TEXT_DIM, "D-PAD UP/DOWN SCROLLS IT");
			y += FONT_LINE;
			fontDraw(10, y, 1, COL_TEXT_DIM, "LEFT/RIGHT PICKS A BUTTON");
		}
	}

	if (action_label) {
		const TRect action_r = {10, (float)LIST_BTN1_Y, SCR_W - 20, LIST_BTN_H};
		if (uiButton(action_r, action_label, ts->cursor == action_idx, tap,
		             in->touch_x, in->touch_y, a)) {
			// Both of the two download verbs run the same call, which is the point: RETRY
			// DOWNLOAD is INSTALL with an honest label on it, not a second code path.
			if (action == UPD_ACT_INSTALL || action == UPD_ACT_RETRY_DOWNLOAD) {
				updaterStartInstall();
			} else if (action == UPD_ACT_RESTART) {
				// updaterRelaunch() only arms the chainloader - the jump itself happens on
				// exit (see updater.h) - so the caller has to fall out of its main loop
				// right after calling it. TITLE_QUIT is exactly that: it is already what
				// makes main() shut every subsystem down and exit cleanly for the Quit
				// button on the main screen, and libctru performs the chainloader jump at
				// that same process exit, so returning it here is the correct way off this
				// screen rather than a reused-for-convenience shortcut.
				updaterRelaunch();
				r.action = TITLE_QUIT;
			} else {
				updaterStartCheck();
			}
		}
	}

	if (show_back) {
		const TRect back_r = {10, (float)LIST_BTN2_Y, SCR_W - 20, LIST_BTN_H};
		if (uiButton(back_r, "BACK", ts->cursor == back_idx, tap, in->touch_x, in->touch_y, a)
		    || (in->keys_down & KEY_B)) {
			// v1.9.1: back to the bar. nav still holds SYSTEM row 0 ("CHECK FOR UPDATE"), the
			// row this screen was entered from, so the focus lands where the player left it
			// without this having to name an index. (Before v1.9.1 this wrote cursor = 5 on the
			// options screen, which was off by one anyway — index 5 there was "CONTROLS >".)
			ts->screen = TITLE_SCR_MAIN;
		}
	}

	return r;
}

// ── Version history browser (v1.8.8) ───────────────────────────────────────────────────
//
// steve's brief, in his own words: "it should be displaying all the past versions up to the
// current latest version. What it added, what it changed, what it fixed, what it removed ...
// So that way players can see ... what they've missed out on." app/version_history.h carries
// the full design writeup (why the data is baked, why whatsnew<version>.txt rather than
// CHANGELOG.md, why nothing before v1.6.0 is here); this half is just the screen.
//
// The list is newest-first, which is the order a player skimming "what did I miss" wants -
// the same order CHANGELOG.md itself is written in. Row 0 is a release the update screen's own
// check has already found but the player has not installed (updaterReleaseNotes() -
// zero extra network requests), when there is one; every row after that is a baked
// app/version_history.c entry, which is stored oldest-first, so the mapping below reads it
// back to front.

// True only once a check has actually found something newer than this build - never for a
// baked version, by construction (app/version_history_data.c can only ever contain versions
// that existed when THIS build was compiled, all of which are <= BLOCKSMITH_VERSION), but
// checked with strcmp anyway rather than assumed, so a rebuilt-but-not-reinstalled dev binary
// can never show the same version as both row 0 and the newest baked row.
static bool vhHasExtraRow(void)
{
	if (!updaterAvailable() || updaterState() != UPDATE_AVAILABLE) return false;

	const char* latest = updaterLatestVersion();
	if (latest[0] == '\0') return false;

	const int n = versionHistoryCount();
	if (n > 0 && strcmp(versionHistoryVersionAt(n - 1), latest) == 0) return false;

	return true;
}

static int vhRowCount(void)
{
	return versionHistoryCount() + (vhHasExtraRow() ? 1 : 0);
}

// Row 0..count-1, newest first. Returns -1 for the not-yet-installed row (row 0, only when
// vhHasExtraRow()), or the app/version_history.c index otherwise.
static int vhBakedIndexFor(int row)
{
	const int extra = vhHasExtraRow() ? 1 : 0;
	if (row < extra) return -1;
	return versionHistoryCount() - 1 - (row - extra);
}

static const char* vhRowVersion(int row)
{
	const int idx = vhBakedIndexFor(row);
	return (idx < 0) ? updaterLatestVersion() : versionHistoryVersionAt(idx);
}

// One WhatsNew's worth of scratch, reused for whichever row is on screen - the same "parse on
// demand into one shared buffer" tradeoff app/version_history.h's file comment already makes
// for versionHistoryNotesAt, so this browser never holds more than ~3 KB of parsed notes live
// no matter how many versions are baked in.
static WhatsNew s_vh_notes;

static const WhatsNew* vhRowNotes(int row)
{
	const int idx = vhBakedIndexFor(row);
	if (idx < 0) return updaterReleaseNotes();
	versionHistoryNotesAt(idx, &s_vh_notes);
	return &s_vh_notes;
}

static TitleResult drawVersionHistory(TitleState* ts, const TitleInput* in, bool tap)
{
	TitleResult r = {TITLE_STAY, {0}};
	const int rows = vhRowCount();

	fontDraw(8, 4, 1, COL_TEXT_DIM, "VERSION HISTORY");

	if (rows <= 0) {
		fontDraw(10, LIST_TOP_Y + 10, 1, COL_TEXT_DIM, "No version history in this build.");
	} else {
		ts->vh_cursor = clampInt(ts->vh_cursor, 0, rows - 1);
		const int prev_cursor = ts->vh_cursor;

		// ── Who owns the D-pad on this screen ─────────────────────────────────────────
		//
		// LEFT/RIGHT step through the timeline one release at a time, the same physical
		// gesture source/debug/blocklist.c's L/R page-through-the-list idiom uses, mapped to
		// the D-pad because TitleInput carries no shoulder buttons (see title.h's own
		// comment on keys_down). UP/DOWN scrolls the selected release's notes on the TOP
		// screen - exactly what UP/DOWN already means on the update screen this was reached
		// from (see drawUpdate's "who owns the D-pad" comment), so a player who has already
		// used that screen needs nothing new here. Each direction owns exactly one thing, so
		// there is nothing here for this project's twice-shipped "one keys_down word read by
		// two UI layers" bug class to land on.
		if (in->keys_down & KEY_DRIGHT) ts->vh_cursor = (ts->vh_cursor + 1) % rows;
		if (in->keys_down & KEY_DLEFT)  ts->vh_cursor = (ts->vh_cursor + rows - 1) % rows;

		if (ts->vh_cursor < ts->vh_scroll) ts->vh_scroll = ts->vh_cursor;
		if (ts->vh_cursor >= ts->vh_scroll + LIST_VISIBLE_ROWS)
			ts->vh_scroll = ts->vh_cursor - LIST_VISIBLE_ROWS + 1;

		for (int row = 0; row < LIST_VISIBLE_ROWS; row++) {
			const int i = ts->vh_scroll + row;
			if (i >= rows) break;

			const int   baked_idx = vhBakedIndexFor(i);
			const char* version   = vhRowVersion(i);

			char label[40];
			if (baked_idx < 0) {
				snprintf(label, sizeof(label), "v%s - AVAILABLE, NOT INSTALLED", version);
			} else if (BLOCKSMITH_VERSION_SET && strcmp(version, BLOCKSMITH_VERSION) == 0) {
				snprintf(label, sizeof(label), "v%s - THIS BUILD", version);
			} else {
				snprintf(label, sizeof(label), "v%s", version);
			}

			const TRect rr = {10, (float)(LIST_TOP_Y + row * LIST_ROW_H), SCR_W - 20,
			                  LIST_ROW_H - 2};
			// A is never read on this screen - only LEFT/RIGHT above move the cursor - so
			// a_down is always false here; a tap still jumps straight to the row (uiButton's
			// own tapped_here || (focused && a_down) contract).
			if (uiButton(rr, label, ts->vh_cursor == i, tap, in->touch_x, in->touch_y, false))
				ts->vh_cursor = i;
		}

		// Moving to a different row starts its notes scrolled to the top, same as entering
		// the update screen already does for its own single version's notes.
		if (ts->vh_cursor != prev_cursor) {
			ts->notes_scroll               = 0;
			ts->notes_rep_up.held_frames   = 0;
			ts->notes_rep_down.held_frames = 0;
		}

		ts->notes_scroll += whatsnewRepeatStep(&ts->notes_rep_down,
		                                        (in->keys_held & KEY_DDOWN) != 0);
		ts->notes_scroll -= whatsnewRepeatStep(&ts->notes_rep_up,
		                                        (in->keys_held & KEY_DUP) != 0);
		if (ts->notes_scroll < 0) ts->notes_scroll = 0;
	}

	const TRect back_r = {10, (float)LIST_BTN1_Y, SCR_W - 20, LIST_BTN_H};
	if (uiButton(back_r, "BACK", false, tap, in->touch_x, in->touch_y, false)
	    || (in->keys_down & KEY_B)) {
		// v1.9.1: two ways in, so B has to go back to whichever one was used — the update
		// screen's small VERSION HISTORY button (vh_from_update) or the SYSTEM tab's own row.
		// Landing an off-the-tab visit on the update screen would put the player somewhere they
		// never went, and B again from there would then dump them on the tab anyway.
		ts->screen = ts->vh_from_update ? TITLE_SCR_UPDATE : TITLE_SCR_MAIN;
		ts->cursor = 0;
		ts->notes_scroll               = 0;
		ts->notes_rep_up.held_frames   = 0;
		ts->notes_rep_down.held_frames = 0;
	}

	return r;
}

// ── Top screen: the release notes ──────────────────────────────────────────────────────

// Re-laid out every frame rather than cached. The wrap is a few kilobytes of memcpy over at
// most 64 lines, which is nothing next to the 16.71 ms frame this menu has entirely to
// itself, and a cache here would need invalidating the moment the worker thread finished a
// fetch — a staleness bug in exchange for time nobody is short of.
static WhatsNewLayout s_notes_layout;

// Shared by the update screen's single-version panel (drawUpdateTop, the only caller before
// v1.8.8) and the version history browser's per-row panel (drawVersionHistoryTop) — same
// layout, same scrollbar, same wrap; the only difference between the two screens is which
// WhatsNew* and which heading text they hand in. Pulled out under its own name in v1.8.8
// rather than duplicated, so drawUpdateTop's quad cost — already shipping, already proven to
// fit — is provably unchanged: it is the same code, just called through one more frame.
static void drawNotesTop(const char* heading, const WhatsNew* notes, int* scroll)
{
	fontDraw(NOTES_PANEL_X + 2, 6, 1, COL_ACCENT, heading);

	// Real pixels and the real font advance, not a guessed character count — see
	// app/whatsnew.h. FONT_ADVANCE is per font pixel, so at scale 1 it is the advance.
	whatsnewBuildLayout(notes, NOTES_TEXT_W, FONT_ADVANCE, &s_notes_layout);

	const int lines = s_notes_layout.count;
	*scroll = whatsnewClampScroll(*scroll, lines, NOTES_VISIBLE);

	spriteRect(NOTES_PANEL_X, NOTES_PANEL_Y, NOTES_PANEL_W, NOTES_PANEL_H, COL_PANEL_LO);

	for (int row = 0; row < NOTES_VISIBLE; row++) {
		const int i = *scroll + row;
		if (i >= lines) break;

		const float ly = (float)(NOTES_TEXT_Y + row * FONT_LINE);

		uint32_t colour;
		switch (s_notes_layout.kind[i]) {
		case WN_LINE_HEADING: colour = COL_ACCENT;   break;
		case WN_LINE_NOTE:    colour = COL_TEXT_DIM; break;
		case WN_LINE_CONT:    colour = COL_TEXT_DIM; break;
		default:              colour = COL_TEXT;     break;
		}

		fontDraw(NOTES_TEXT_X, ly, 1, colour, s_notes_layout.lines[i]);
	}

	// The slider. Track always drawn, so the bar reads as a bar even when there is nothing
	// below the fold; the thumb fills the whole track in that case (whatsnewThumb's own
	// contract) and is drawn in the dimmer colour, which is the entire visual difference
	// between "that is all of it" and "there is more down there".
	const bool more = whatsnewScrollable(lines, NOTES_VISIBLE);

	WhatsNewThumb thumb;
	whatsnewThumb(lines, NOTES_VISIBLE, *scroll, NOTES_TRACK_H, NOTES_THUMB_MIN, &thumb);

	spriteRect(NOTES_BAR_X, NOTES_TRACK_Y, NOTES_BAR_W, NOTES_TRACK_H, COL_PANEL);
	spriteRect(NOTES_BAR_X, (float)(NOTES_TRACK_Y + thumb.y), NOTES_BAR_W, (float)thumb.h,
	           more ? COL_ACCENT : COL_PANEL_HI);

	const char* footer = more ? "D-PAD UP/DOWN TO SCROLL"
	                          : "THAT IS THE WHOLE CHANGELOG";
	fontDraw(NOTES_PANEL_X + 2, (float)(NOTES_PANEL_Y + NOTES_PANEL_H + 5), 1,
	         COL_TEXT_DIM, footer);
}

static void drawUpdateTop(TitleState* ts)
{
	const char* latest = updaterLatestVersion();

	char heading[64];
	if (latest[0] != '\0')
		snprintf(heading, sizeof(heading), "WHAT'S NEW IN %s", latest);
	else
		snprintf(heading, sizeof(heading), "WHAT'S NEW");

	drawNotesTop(heading, updaterReleaseNotes(), &ts->notes_scroll);
}

static void drawVersionHistoryTop(TitleState* ts)
{
	const int rows = vhRowCount();
	if (rows <= 0) {
		spriteRect(NOTES_PANEL_X, NOTES_PANEL_Y, NOTES_PANEL_W, NOTES_PANEL_H, COL_PANEL_LO);
		fontDraw(NOTES_PANEL_X + 2, 6, 1, COL_ACCENT, "VERSION HISTORY");
		return;
	}

	const int row = clampInt(ts->vh_cursor, 0, rows - 1);

	char heading[64];
	snprintf(heading, sizeof(heading), "WHAT'S NEW IN %s", vhRowVersion(row));

	drawNotesTop(heading, vhRowNotes(row), &ts->notes_scroll);
}

void titleDrawTop(TitleState* ts)
{
	spriteBegin(TOP_W, TOP_H);
	spriteTexture(fontTexture());
	spriteRect(0, 0, TOP_W, TOP_H, COL_BG);

	// Exactly the condition drawUpdate() calls `notes_visible`, and it has to stay exactly
	// that: it is what decides whether the D-pad scrolls or moves the button cursor, so a
	// panel on screen with the other input mapping live would be a screen that ignores its
	// own scrollbar. Anything else leaves the top screen as it has always been — cleared,
	// so it never holds uninitialised VRAM, and otherwise empty.
	if (ts->screen == TITLE_SCR_UPDATE && updaterAvailable() &&
	    updaterState() == UPDATE_AVAILABLE)
		drawUpdateTop(ts);
	else if (ts->screen == TITLE_SCR_VERSION_HISTORY)
		drawVersionHistoryTop(ts);

	spriteEnd();
}

// ── The bar itself ─────────────────────────────────────────────────────────────────────
//
// One frame of the four-tab title screen (blueprint D10). Everything that decides WHERE the
// cursor is lives here; the four draw* functions above only read ts->nav and paint.
//
// The order below is load-bearing and is written out in full because three of the steps exist
// solely to keep two consumers of the same input from both eating it — the "one hidKeysDown
// word read by two UI layers" bug this project has shipped twice:
//
//   1. A tab tap is applied BEFORE barNavInput, so the keys read this frame are read against
//      the tab the player just touched, not the one they left.
//   2. On PLAY, X/Y/B are masked out of the word barnav sees, because worldlistUiStep owns all
//      three there (rename, delete, cancel-the-armed-delete). Without the mask a B that
//      disarms a delete would ALSO leave the tab, and a Y that armed one would be an ALT event
//      nothing consumes. The mask is PLAY-only: on the other three tabs B really does mean
//      "leave", and there is no X/Y verb to collide with.
//   3. The world verbs run AFTER barNavInput, on the post-move row. worldlistRenameAt and
//      worldlistDeleteAt take a WORLD index; handing them a row the player had already moved
//      off would rename or delete the wrong world.
//   4. BAR_EV_BACK's tab move is applied AFTER the content dispatch, never before it. On
//      MULTIPLAYER, titleMpNav is the one function allowed to answer both "leave" and "enter
//      the server's world" (scene/title_nav.h) and it needs the B press as an input; moving
//      the tab first would mean drawMultiplayer never ran on the frame that mattered, and
//      titleMpNav's .back would be permanently false.
static TitleResult drawBar(TitleState* ts, Options* opts, const TitleInput* in, bool tap)
{
	TitleResult r = {TITLE_STAY, {0}};

	BarCatSpec cats[TITLE_CAT_COUNT];
	titleBuildCats(ts, cats);

	const int cat_before = ts->nav.cat;

	// Step 1: a tab tap. `tab_tapped` then suppresses this frame's content taps — the strip and
	// the rows are disjoint bands so one point cannot be both, but a tap that switched tabs must
	// not also be offered to the tab it switched TO, which is now drawing rows under a finger
	// that was never aimed at them.
	bool tab_tapped = false;
	if (tap) {
		const int t = titleHitStrip(in->touch_x, in->touch_y);
		if (t >= 0) {
			barNavSetCat(&ts->nav, t, cats, TITLE_CAT_COUNT);
			tab_tapped = true;
		}
	}

	// Step 2: the mask, then the one barNavInput call. has_lift is false: no tab of this bar has
	// a second row band for L/R to lift into (barnav.h) — L/R here always mean "switch tab".
	uint32_t nav_keys = in->keys_down;
	if (ts->nav.cat == TITLE_CAT_PLAY) nav_keys &= ~(uint32_t)(KEY_X | KEY_Y | KEY_B);
	const BarEvent ev = barNavInput(&ts->nav, nav_keys, cats, TITLE_CAT_COUNT, false);

	// Arriving on PLAY re-lists the card. Only on arrival — see titleEnterPlayTab. The world
	// count it just changed is a row count, so cats is stale from here and is rebuilt.
	if (ts->nav.cat == TITLE_CAT_PLAY && cat_before != TITLE_CAT_PLAY) {
		titleEnterPlayTab(ts);
		titleBuildCats(ts, cats);
		barNavClamp(&ts->nav, cats, TITLE_CAT_COUNT);
	}

	// Step 3a: a content-row tap moves the cursor to that row. MULTIPLAYER is excluded on
	// purpose: its content is prose and two pinned buttons, not rows on this grid (see
	// drawMultiplayer), and its taps go through uiButton exactly as they always have.
	if (tap && !tab_tapped && ts->nav.cat != TITLE_CAT_MULTIPLAYER) {
		const int v = titleRowFromPoint(in->touch_x, in->touch_y);
		if (v >= 0) {
			const int scroll = (ts->nav.cat == TITLE_CAT_PLAY) ? ts->world_scroll : 0;
			const int row    = scroll + v;
			// Only a row that is actually DRAWN moves the cursor. titleRowFromPoint tests all six
			// rect slots whether or not the tab has six rows, and barNavSetCell would clamp a tap
			// on the empty space under a two-world list up to the last real row — a cursor jump
			// with nothing under the finger to explain it. A tab with fewer rows than the window
			// simply has dead space below it.
			if (row < cats[ts->nav.cat].rows)
				barNavSetCell(&ts->nav, row, 0, cats, TITLE_CAT_COUNT);
		}
	}

	// Step 3b: PLAY's world verbs, on the post-move row. ts->cursor is the WORLD index — bar row
	// minus the NEW WORLD row — and is -1 while NEW WORLD itself is focused, which is exactly the
	// "no world selected" value worldlistUiStep's own gates are written against.
	WorldlistUiAction ui = WORLDLIST_UI_NONE;
	if (ts->nav.cat == TITLE_CAT_PLAY) {
		ts->cursor = ts->nav.row - 1;
		ui = worldlistUiStep(&ts->world_confirm, ts->cursor, ts->world_count,
		                     (in->keys_down & KEY_X) != 0,
		                     (in->keys_down & KEY_Y) != 0,
		                     (in->keys_down & KEY_B) != 0);
		if (ui == WORLDLIST_UI_RENAME_PROMPT)    titleRenameWorldFlow(ts);
		else if (ui == WORLDLIST_UI_DELETE_FIRE) titleDeleteSelectedWorld(ts);

		// Both of those move ts->cursor themselves (worldlist.h owns where the cursor lands
		// after its own edit) and can change world_count, so the row count and the cursor are
		// both re-derived rather than assumed to have survived.
		if (ui == WORLDLIST_UI_RENAME_PROMPT || ui == WORLDLIST_UI_DELETE_FIRE) {
			titleBuildCats(ts, cats);
			barNavSetCell(&ts->nav, ts->cursor + 1, 0, cats, TITLE_CAT_COUNT);
		}

		// Blueprint §7 red arm 2: the scroll window follows the cursor. Without this the 7th
		// world is unreachable — the cursor moves onto it and the window does not, so the row
		// under focus is off screen.
		ts->world_scroll = barNavScrollFor(ts->nav.row, ts->world_scroll, TITLE_ROWS_VISIBLE,
		                                    1 + ts->world_count);
	}

	drawTitleChrome(ts);

	// Step 4: content. `tap && !tab_tapped` for the reason step 1 gives.
	const bool content_tap = tap && !tab_tapped;
	bool leave_to_play = false;

	switch (ts->nav.cat) {
	case TITLE_CAT_PLAY:
		r = drawPlayTab(ts, in, content_tap, ev == BAR_EV_COMMIT, ui);
		break;
	case TITLE_CAT_MULTIPLAYER:
		r = drawMultiplayer(ts, in, content_tap, ev == BAR_EV_COMMIT, ev == BAR_EV_BACK,
		                    &leave_to_play);
		break;
	case TITLE_CAT_OPTIONS:
		drawOptionsTab(ts, opts, in, content_tap, ev == BAR_EV_COMMIT,
		               ev == BAR_EV_STEP_LEFT, ev == BAR_EV_STEP_RIGHT);
		break;
	case TITLE_CAT_SYSTEM:
		r = drawSystemTab(ts, in, content_tap, ev == BAR_EV_COMMIT);
		break;
	default:
		break;
	}

	// Step 5: B leaves for PLAY, applied last. On MULTIPLAYER the answer is titleMpNav's, not
	// this frame's raw event — see drawMultiplayer. On PLAY, B was masked out above and never
	// reached barnav, so BAR_EV_BACK cannot fire there and "leave PLAY for PLAY" is not a case.
	const bool leaving = (ts->nav.cat == TITLE_CAT_MULTIPLAYER) ? leave_to_play
	                                                            : (ev == BAR_EV_BACK);
	if (leaving && ts->nav.cat != TITLE_CAT_PLAY) {
		barNavSetCat(&ts->nav, TITLE_CAT_PLAY, cats, TITLE_CAT_COUNT);
		titleEnterPlayTab(ts);          // same arrival rescan step 2's L/R path does
		titleBuildCats(ts, cats);
		barNavClamp(&ts->nav, cats, TITLE_CAT_COUNT);
	}

	// Blueprint §7 red arm 1, and D10's "optionsSave on leaving the category": the one disk
	// write. Tested against cat_before rather than against any single way of leaving, so L/R, a
	// tab tap and B all save — the bug the red arm names is a save wired to one of the three.
	// A trip into the bindings leaf is NOT a leave (cat is still OPTIONS while that screen is
	// up), which is why drawOptionsBindings has no save of its own.
	if (cat_before == TITLE_CAT_OPTIONS && ts->nav.cat != TITLE_CAT_OPTIONS)
		optionsSave(opts, TITLE_OPTIONS_PATH);

	return r;
}

// ── Entry points ───────────────────────────────────────────────────────────────────────

void titleInit(TitleState* ts)
{
	memset(ts, 0, sizeof(*ts));
	ts->screen = TITLE_SCR_MAIN;
	ts->rebind_action = -1;
	worldlistConfirmReset(&ts->world_confirm);   // already zero == off; explicit anyway (title.h)

	// v1.9.1: the menu opens ON the PLAY tab and PLAY's rows are the world list, so the scan
	// that used to hang off "the player navigated to world select" has to happen here instead.
	// memset already left nav at cat 0 / row 0 — PLAY, NEW WORLD — which is the state this wants
	// (barnav.h: "Zero is a valid state"), so there is no barNavReset call to go with it.
	titleEnterPlayTab(ts);
}

TitleResult titleUpdateDraw(TitleState* ts, Options* opts, const TitleInput* in)
{
	TitleResult r = {TITLE_STAY, {0}};

	// Rising edge only: a tap fires once, not every frame the finger stays down, matching
	// how KEY_A works via hidKeysDown() rather than hidKeysHeld(). Computed once here so
	// every screen below reads the same `tap` rather than re-deriving it.
	const bool tap = in->touch_down && !ts->touch_prev;
	ts->touch_prev = in->touch_down;

	if (ts->status_ttl > 0) ts->status_ttl--;

	// v1.9.1: three of the TitleScreenId values stopped being screens when their content became
	// a tab, but a caller may still ask for one — main.c writes TITLE_SCR_MULTIPLAYER into a
	// fresh TitleState to put the player back on multiplayer after a server session. Each
	// resolves here, on the frame it is seen, to the bar focused on the matching tab, so the
	// request means exactly what it always meant. See title.h's comment on the enum.
	//
	// The row is deliberately not touched: barNavSetCat restores that tab's remembered row, and
	// on a freshly-inited TitleState that is row 0 anyway.
	if (ts->screen == TITLE_SCR_WORLD_SELECT || ts->screen == TITLE_SCR_OPTIONS_GENERAL ||
	    ts->screen == TITLE_SCR_MULTIPLAYER) {
		const int cat = (ts->screen == TITLE_SCR_WORLD_SELECT)    ? TITLE_CAT_PLAY
		              : (ts->screen == TITLE_SCR_OPTIONS_GENERAL) ? TITLE_CAT_OPTIONS
		                                                          : TITLE_CAT_MULTIPLAYER;
		BarCatSpec cats[TITLE_CAT_COUNT];
		titleBuildCats(ts, cats);
		barNavSetCat(&ts->nav, cat, cats, TITLE_CAT_COUNT);
		ts->screen = TITLE_SCR_MAIN;
	}

	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	switch (ts->screen) {
	case TITLE_SCR_MAIN:             r = drawBar(ts, opts, in, tap);         break;
	case TITLE_SCR_OPTIONS_BINDINGS: drawOptionsBindings(ts, opts, in, tap); break;
	case TITLE_SCR_UPDATE:           r = drawUpdate(ts, in, tap);            break;
	case TITLE_SCR_VERSION_HISTORY:  r = drawVersionHistory(ts, in, tap);    break;
	// Resolved to TITLE_SCR_MAIN above, so unreachable here. Listed rather than defaulted so a
	// future value added to TitleScreenId is a -Wswitch error instead of a silently blank screen.
	case TITLE_SCR_WORLD_SELECT:
	case TITLE_SCR_OPTIONS_GENERAL:
	case TITLE_SCR_MULTIPLAYER:      break;
	}

	spriteEnd();
	return r;
}
