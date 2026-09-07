// v1.9.1 INTERFACE, lane D — the pause bar's MODEL.
//
// docs/blueprint-1.9.1-interface.md D9 replaces the pause menu's two stacked pages
// (main: Resume/Options/Quit, then an options page behind them) with one three-tab bar
// inside the same scrim and the same panel:
//
//     GAME     Resume, Quit to title
//     OPTIONS  Render dist ±, 3D on/off, Sound ±
//     SYSTEM   Controls >, Debug >, and the memory readout under the rows
//
// ── Why this file exists at all ────────────────────────────────────────────────────────
//
// The blueprint's lane D names one pair of files, scene/pausemenu.{h,c}. That pair cannot
// be host-tested: pausemenu.c includes <3ds.h>, gfx/sprite.h, gfx/font.h and audio/audio.h,
// so nothing in it links outside a devkitARM build — the same wall scene/ui.c hits. So the
// pause bar splits exactly the way this project has already split its other two screens:
// scene/ui.c gave its rect arithmetic to scene/ui_layout.c and its gesture classification to
// scene/ui_gesture.c, and scene/title.c gave a navigation rule to scene/title_nav.c. This is
// that same carve-out for the pause panel, and pausemenu.c keeps ONLY the drawing.
//
// What is in here is everything about the pause bar that can be WRONG rather than merely
// ugly: which tab and row the cursor is on after any key or any stylus press, which action
// that produces, where every rect is, and which point is inside which one. What is NOT in
// here is every pixel of colour, the audio call the volume row makes, and the three memory
// figures — those stay in pausemenu.c, which owns the draw and the console services.
//
// ── What it never does ─────────────────────────────────────────────────────────────────
//
// It applies nothing. Render distance, 3D and volume are REPORTED as actions and the caller
// performs them, the same contract scene/pausemenu.h already states for its two out-params
// and for the same reason: genSetRadius() re-meshes the ring, gfxSet3D() is a libctru call,
// audioSetMasterVolume() is the audio system's, and optionsSave() writes the card. Putting
// any of those behind a menu model is what this split exists to prevent.
//
// No <3ds.h>, no gfx/*.h, no world/*.h — <stdbool.h> and <stdint.h> and nothing else, so
// tests/pausebar_test.c links THIS module rather than a copy of it. The key bits below are
// mirrors of libctru's, checked by a _Static_assert in pausebar.c on the console build, the
// carve-out scene/hotbar.h and scene/ui_gesture.h already made.
//
// ── Relationship to scene/barnav.h (lane A) ────────────────────────────────────────────
//
// The blueprint's barnav is the IN-GAME bar's navigation core: six categories, eight rows,
// per-row column widths, a pref_col memory across a three-wide furnace row, and a scroll
// window. The pause panel has three fixed tabs of at most three rows and exactly one column,
// so every one of those mechanisms is inert here and linking them in would buy nothing and
// cost this module its independence. What this file DOES do is obey barnav's documented
// contract to the letter, so the two screens behave identically under the same buttons:
//
//   * one event per call, in barnav's priority order — B, then A, then L/R, then up/down,
//     then left/right (there is no X on this panel: BAR_EV_ALT is unmapped in v1.9.1);
//   * L/R switch tab, CLAMPED, never wrapping, and restore that tab's remembered row;
//   * up/down move the row and DO wrap;
//   * left/right step the value on a category whose horizontal_is_content is true, and
//     switch tab on one where it is false.
//
// pauseBarHorizontalIsContent() and pauseBarTabRows() below are published in exactly the
// shape BarCatSpec wants, so an integrator who would rather drive this panel from a BarNav
// can fill three specs from them and feed the resulting events to pauseBarCommit() /
// pauseBarStep() without this file changing at all.
#ifndef SCENE_PAUSEBAR_H
#define SCENE_PAUSEBAR_H

#include <stdbool.h>
#include <stdint.h>

// libctru's key bits (3ds/services/hid.h), mirrored so this header compiles on the host.
// pausebar.c static-asserts every one of them against <3ds.h> when built for the console.
// Only these six are read: the panel has no X, no Y and no shoulder gestures beyond the tabs.
#define PAUSEBAR_KEY_A      0x0001u
#define PAUSEBAR_KEY_B      0x0002u
#define PAUSEBAR_KEY_DRIGHT 0x0010u
#define PAUSEBAR_KEY_DLEFT  0x0020u
#define PAUSEBAR_KEY_DUP    0x0040u
#define PAUSEBAR_KEY_DDOWN  0x0080u
#define PAUSEBAR_KEY_R      0x0100u
#define PAUSEBAR_KEY_L      0x0200u

// gfx/font.h's FONT_ADVANCE and FONT_GLYPH_H, mirrored for the same reason the key bits are:
// that header includes <citro3d.h>. Every label width in this file is a multiple of the
// advance, and pausebar.c static-asserts both against the real header on the console build.
#define PAUSEBAR_FONT_ADVANCE 6
#define PAUSEBAR_FONT_GLYPH_H 7

// ── Tabs and rows ──────────────────────────────────────────────────────────────────────
//
// Written as plain integer #defines rather than as enums on purpose. -fshort-enums is in
// effect for the console build (see the ARM EABI note this project carries), so an enum is
// one byte there and four on the host; PauseBarState below is size-asserted and must not
// contain a member whose width depends on the target.
#define PAUSEBAR_TAB_GAME    0
#define PAUSEBAR_TAB_OPTIONS 1
#define PAUSEBAR_TAB_SYSTEM  2
#define PAUSEBAR_TAB_COUNT   3

// The most rows any one tab has. The cursor wraps against the CURRENT tab's count
// (pauseBarTabRows), never against this; it exists to size row_mem[] and to bound the
// row-rect table.
#define PAUSEBAR_MAX_ROWS 3

// GAME
#define PB_GAME_RESUME 0
#define PB_GAME_QUIT   1
#define PB_GAME_ROWS   2

// OPTIONS. All three are value rows, which is what makes this the one tab whose
// horizontal_is_content is true — see pauseBarHorizontalIsContent().
#define PB_OPT_DIST   0
#define PB_OPT_3D     1
#define PB_OPT_VOLUME 2
#define PB_OPT_ROWS   3

// SYSTEM. The memory readout is drawn under these two and is NOT a row: the cursor stops at
// PB_SYS_ROWS and never reaches it.
#define PB_SYS_CONTROLS 0
#define PB_SYS_DEBUG    1
#define PB_SYS_ROWS     2

// ── What one input produced ────────────────────────────────────────────────────────────
//
// One action per call, never a queue. PAUSEBAR_ACT_NONE is the overwhelmingly common answer,
// the same thing scene/pausemenu.h says about PAUSE_ACTION_NONE.
//
// TAB and MOVE are reported rather than swallowed so the caller can play SFX_UI_TAP on them
// (blueprint D13: the existing cue on every commit and tab change, no new sound).
#define PAUSEBAR_ACT_NONE            0
#define PAUSEBAR_ACT_RESUME          1   // close the panel and carry on
#define PAUSEBAR_ACT_QUIT            2   // leave the world; the caller decides what that means
#define PAUSEBAR_ACT_REMAP           3   // open the control-remapping screen
#define PAUSEBAR_ACT_DEBUG           4   // open the debug menu
#define PAUSEBAR_ACT_DIST_STEP       5   // arg is -1 or +1 columns of render distance
#define PAUSEBAR_ACT_STEREO_TOGGLE   6   // arg is 0; flip the 3D slider's effect
#define PAUSEBAR_ACT_VOLUME_STEP     7   // arg is -1 or +1 steps of OPTIONS_AUDIO_VOL_STEP
#define PAUSEBAR_ACT_TAB             8   // arg is the tab now focused
#define PAUSEBAR_ACT_MOVE            9   // arg is the row now focused

typedef struct {
	int kind;   // PAUSEBAR_ACT_*
	int arg;    // see each kind above; 0 where none is named
} PauseBarAction;

// ── The state ──────────────────────────────────────────────────────────────────────────
//
// All-zero is a VALID, INERT state: closed, on GAME, row 0, nothing remembered. That is what
// a plain `static PauseBarState s_bar;` gives, and every entry point below refuses to act on
// a closed panel, so a caller that never calls pauseBarReset() is still correct.
//
// Every member is one byte wide on both targets — no enum, no int — so the size assertion in
// pausebar.c means the same thing on the host and on ARM under -fshort-enums.
//
//   open     is the panel up? SELECT flips it through pauseBarToggle().
//   tab      PAUSEBAR_TAB_*, the focused tab.
//   row      the focused row WITHIN that tab, always < pauseBarTabRows(tab).
//   row_mem  the row each tab was left on, restored when the tab is focused again. This is
//            what makes "open Controls, come back, land on Controls again" true rather than
//            landing on the top of SYSTEM — the pause-panel half of the blueprint's own
//            lane-E red arm 3 ("leaf screen B returns to cat 0 instead of remembered").
typedef struct {
	bool    open;
	int8_t  tab;
	int8_t  row;
	uint8_t row_mem[PAUSEBAR_TAB_COUNT];
} PauseBarState;

// ── Geometry ───────────────────────────────────────────────────────────────────────────
//
// The bottom screen is 320x240 (scene/ui_layout.h's SCR_W/SCR_H). Restated as literals here
// rather than by including that header: ui_layout.h pulls in world/chest.h, world/crafting.h
// and world/inventory.h for constants this panel has no use for, and the pause panel's own
// rects are not in that file's vocabulary. pausebar.c static-asserts these two against
// SCR_W/SCR_H on the console build, where including ui_layout.h costs nothing.
#define PB_SCR_W 320
#define PB_SCR_H 240

// The panel, byte-identical to what scene/pausemenu.c ships today (PANEL_X 26, PANEL_Y 24,
// 268x192) — the blueprint's D9 keeps "the same scrim/panel", so these do not move.
#define PB_PANEL_X 26
#define PB_PANEL_Y 24
#define PB_PANEL_W (PB_SCR_W - PB_PANEL_X * 2)   // 268
#define PB_PANEL_H (PB_SCR_H - PB_PANEL_Y * 2)   // 192

// The tab strip, inside the panel. D9: "strip inside the panel at y 30 (h 22, three 89-px
// tabs)". 268 does not divide by three, so the tabs are 89/89/90 and are derived by the
// exact-tiling formula in pauseBarTabRect() rather than by a fixed width — the leftover pixel
// belongs to a tab instead of sitting in a gap the strip's own fill would not cover. This is
// the same rule §1A.2 pins for the in-game strip ("barTabRect(i,n) tiles [0,294) exactly").
#define PB_STRIP_Y 30
#define PB_STRIP_H 22

// Rows. D9: "Rows from y 58, ROW_H 22". The drawn/hit height is ROW_H - 2, leaving a 2 px
// gap between rows — the pitch scene/pausemenu.c's drawRow() already uses.
#define PB_ROW_Y0 58
#define PB_ROW_H  22
#define PB_ROW_X  (PB_PANEL_X + 12)   // 38
#define PB_ROW_W  (PB_PANEL_W - 24)   // 244

// The two stepper arrows on an OPTIONS row, as TOUCH BOXES rather than as glyph positions:
// a 6-px-wide "<" is not a stylus target on a resistive panel, so the box is 22 px and the
// glyph is drawn centred in it. Both boxes sit inside the row and leave a 32 px value field
// between them — five characters at scale 1, enough for "100%" and for a three-digit render
// distance. The 3D row uses the same two boxes; both mean "flip it", which is what left,
// right and A have all meant on that row since it moved off the SELECT button.
#define PB_ARROW_W    22
#define PB_ARROW_L_X  (PB_ROW_X + PB_ROW_W - 80)   // 202
#define PB_ARROW_R_X  (PB_ROW_X + PB_ROW_W - 26)   // 256
#define PB_VALUE_X    (PB_ARROW_L_X + PB_ARROW_W)  // 224 — left edge of the value field

// The memory readout on SYSTEM: three lines under the two rows, with its own heading.
#define PB_MEM_Y0   (PB_ROW_Y0 + PB_SYS_ROWS * PB_ROW_H + 12)   // 114
#define PB_MEM_STEP 11
#define PB_MEM_ROWS 4    // "MEMORY", then World / Linear / VRAM

// The footer hint, at the bottom of the panel — the y scene/pausemenu.c already uses.
#define PB_FOOTER_Y (PB_PANEL_Y + PB_PANEL_H - 16)   // 200

// "no tab / no row here". Distinct from a valid index and never returned as one.
#define PAUSEBAR_HIT_NONE (-1)

// Which stepper arrow. Passed to pauseBarArrowRect and reported by pauseBarTouch's step
// actions as their `arg`, so the two cannot disagree about which direction is which.
#define PB_ARROW_LEFT  (-1)
#define PB_ARROW_RIGHT (+1)

// A pixel rectangle. Deliberately not scene/ui_layout.h's URect — see PB_SCR_W above for why
// this file does not include that header. The two are structurally identical and neither is
// written to the wire or to a file, so nothing has to agree about the layout of either.
typedef struct { int x, y, w, h; } PbRect;

// Is (x, y) inside `r`? Half-open on both axes: the left/top edges are inside, the right and
// bottom edges are not, so two rects that share an edge cannot both claim a point. The same
// convention scene/ui_layout.h's ptInRect uses.
bool pbPtInRect(PbRect r, int x, int y);

// ── Tables ─────────────────────────────────────────────────────────────────────────────
//
// Every one of these refuses an out-of-range index rather than indexing past its table: a
// tab or row number can arrive from a corrupt PauseBarState, and the answer for one that
// cannot exist has to be one no caller can mistake for a real one. Rect getters answer a
// zero rect (pbPtInRect can never match it), label getters answer "" (never NULL, so a
// caller may print it unguarded), and count getters answer 0.

// How many CURSOR rows this tab has. SYSTEM answers 2: the memory readout under its rows is
// a readout, not a row.
int pauseBarTabRows(int tab);

// The tab's label — "GAME", "OPTIONS", "SYSTEM".
const char* pauseBarTabLabel(int tab);

// The row's label — "Resume", "Quit to title", "Render dist", "3D", "Sound", "Controls",
// "Debug".
const char* pauseBarRowLabel(int tab, int row);

// The footer hint, drawn on every tab. One string, so the panel's bottom line never moves.
const char* pauseBarFooterLabel(void);

// Does this row carry a value that left/right change? True for all three OPTIONS rows and
// false everywhere else. A stepper row is also the only kind with arrow touch boxes.
bool pauseBarRowIsStepper(int tab, int row);

// BarCatSpec.horizontal_is_content for this tab (see the barnav note at the top of this
// file). True for OPTIONS only: its rows consume left/right to step their values, so
// left/right there must NOT switch tab. GAME and SYSTEM have nothing to step, so left/right
// switch tab on them, which is what stops a d-pad-only player finding two dead directions.
bool pauseBarHorizontalIsContent(int tab);

// Does this tab draw the memory readout? SYSTEM only. Exposed so the readout is drawn from
// one condition rather than from "the tab happens to be 2" at the draw site — the blueprint's
// own lane-D red arm 2 is a memory readout drawn on every tab.
bool pauseBarTabShowsMemory(int tab);

// ── Rects ──────────────────────────────────────────────────────────────────────────────

PbRect pauseBarPanelRect(void);
PbRect pauseBarStripRect(void);

// Tab `i` of PAUSEBAR_TAB_COUNT. The three tile the panel's full width EXACTLY — no gap
// between two of them and no overlap — which is why the width is derived per tab rather than
// fixed at PB_PANEL_W / 3.
PbRect pauseBarTabRect(int i);

// Row `row`'s rect. The same for every tab (the tabs differ in how many rows they have, not
// in where they sit), so this takes no tab.
PbRect pauseBarRowRect(int row);

// The touch box for one stepper arrow on row `row` of `tab`. A zero rect for a row that is
// not a stepper, or a `dir` that is neither PB_ARROW_LEFT nor PB_ARROW_RIGHT.
PbRect pauseBarArrowRect(int tab, int row, int dir);

// Which tab a point lands on, or PAUSEBAR_HIT_NONE. Only points inside the strip can hit.
int pauseBarHitTab(int x, int y);

// Which row of `tab` a point lands on, or PAUSEBAR_HIT_NONE. Rows this tab does not have
// answer NONE even though their rects exist, so a tap in the empty band under GAME's two
// rows does nothing rather than committing a row the player cannot see.
int pauseBarHitRow(int tab, int x, int y);

// ── State ──────────────────────────────────────────────────────────────────────────────

// Back to the initial state: closed, GAME, row 0, nothing remembered. Only needed by a
// caller that did not zero the struct — an all-zero PauseBarState is already this.
void pauseBarReset(PauseBarState* s);

// SELECT. Opens the panel if it is closed, closes it if it is open, and always returns to
// GAME row 0 on the way in — reopening never lands mid-way down a tab, which is the rule
// scene/pausemenu.h's pauseMenuToggle already states. The per-tab row memory is cleared with
// it: it is a memory of THIS visit, not of the last one.
void pauseBarToggle(PauseBarState* s);

// Closes the panel without choosing anything, for the caller's own paths (leaving a server
// session, say) so it can never be left up over a world that is no longer there.
void pauseBarClose(PauseBarState* s);

// Is the panel up?  A NULL state answers false.
bool pauseBarOpen(const PauseBarState* s);

// Forces a corrupt or hand-built state back into range: tab into [0, PAUSEBAR_TAB_COUNT),
// row into [0, rows-of-that-tab), every row_mem entry into range for its own tab. Called at
// the top of every entry point below, so a caller never has to.
void pauseBarClamp(PauseBarState* s);

// ── Input ──────────────────────────────────────────────────────────────────────────────

// One frame of buttons. `keys_down` is hidKeysDown() — the PRESS EDGE word, not the level:
// nothing in here repeats, and feeding the level would step a value once per frame while a
// direction is held.
//
// Exactly one action per call, in barnav's documented priority (see the note at the top):
//
//   B                       RESUME  — closes the panel, on every tab. There is no page to
//                                     back out to any more; the tabs replaced it.
//   A                       the focused row's commit — RESUME, QUIT, REMAP, DEBUG, or
//                                     STEREO_TOGGLE on the 3D row (left, right and A have
//                                     all meant "flip it" on that row since it moved off
//                                     SELECT). NONE on the two rows with nothing to commit.
//   L / R                   TAB, previous/next, CLAMPED — never wrapping. Restores that
//                                     tab's remembered row. NONE at either end.
//   up / down               MOVE, wrapping within the current tab's rows.
//   left / right            on OPTIONS: DIST_STEP / VOLUME_STEP with arg -1/+1, or
//                                     STEREO_TOGGLE on the 3D row.
//                           on GAME and SYSTEM: the same clamped TAB switch L/R makes.
//
// Several keys on one frame produce ONE action, the highest in that list; the rest are
// dropped, not queued, so a key cannot fire on a later frame against a row the player has
// since moved off — the one-event-a-frame rule scene/ui_gesture.h states for the same reason.
//
// Safe on a closed panel and on a NULL state: consumes nothing, answers NONE.
PauseBarAction pauseBarKey(PauseBarState* s, uint32_t keys_down);

// One stylus press. `press` is the RISING EDGE — main.c's `touch_press`, not touch_down —
// because a stylus held still on a stepper arrow must step once, not once a frame.
//
//   a point on tab i          TAB (and the cursor moves there, restoring its row)
//   a stepper arrow           DIST_STEP / VOLUME_STEP / STEREO_TOGGLE, and the cursor moves
//                             to that row first, so the highlight follows the stylus
//   the rest of a row         MOVE for a stepper row (there is nothing to commit on one),
//                             otherwise that row's commit, exactly as A on it
//   anywhere else             NONE — including the panel's own background and the scrim.
//                             A tap OUTSIDE the panel does not close it: the panel is modal
//                             and a mis-aimed stylus must not drop the player back into the
//                             world.
//
// Safe on a closed panel, on `press` false, and on a NULL state: answers NONE.
PauseBarAction pauseBarTouch(PauseBarState* s, bool press, int x, int y);

// ── Label widths, for the draw site ────────────────────────────────────────────────────

// Width in pixels `text` occupies at `scale`, counting the trailing advance of the last
// character — gfx/font.h's fontTextWidth, restated here so this module can check its own
// labels fit their boxes without including a header that pulls in <citro3d.h>. The two are
// the same arithmetic (length * FONT_ADVANCE * scale); pausebar.c asserts the advance
// against the real header on the console build.
int pauseBarTextWidth(const char* text, int scale);

// The widest label this panel can draw, in pixels at scale 1. The footer, at 180 px — see
// tests/pausebar_test.c's testLabelsFitTheirBoxes, which checks every label against the box
// it is drawn in rather than against this one number.
//
// v1.9.1: 162 -> 180. DERIVED from the footer string, not chosen. The footer gained the word
// SELECT — it now reads "A OK  B/SELECT RESUME  L/R TAB", 30 chars x 6 px advance = 180 —
// because SELECT still toggles this panel (main.c:6039, `if (down & KEY_SELECT)
// pauseMenuToggle();`) and the old wording never said so. The panel's real budget from the
// row inset is 42 columns, so 30 fits with 12 to spare: PB_ROW_X 38 + 180 = 218, against a
// panel right edge of PB_PANEL_X + PB_PANEL_W = 294.
#define PAUSEBAR_WIDEST_LABEL_PX 180

#endif
