// v1.9.1 BAR. The tabbed bar's NAVIGATION MODEL — which tab, which cell, and what the frame's
// buttons are asking for. Agent A's half of docs/blueprint-1.9.1-interface.md; the API below is
// the pinned text of that document's §6.1 and agents C (ui.c), D (pausemenu.c) and E (title.c)
// code against it.
//
// ── What this is, and what it deliberately is not ─────────────────────────────────────────
//
// This is the PURE half, the same carve-out scene/hotbar.h, scene/ui_gesture.h, scene/title_nav.h
// and scene/ringorder.h already made: no <3ds.h>, no <citro3d.h>, no gfx/sprite.h, so
// tests/barnav_test.c links the real scene/barnav.c on the host rather than a copy of it. It
// takes one struct of inputs a frame and answers with one event; it draws nothing, it owns no
// rectangle, and it never touches an Inventory.
//
// It also owns no GEOMETRY. Where a tab is, how wide it is, which rect a cell occupies — all of
// that is scene/ui_layout.h's (Agent B). This file only knows how many rows a category has and
// how wide each row is, because that is the whole of what a cursor needs in order to move. A
// category describes itself to this module in a BarCatSpec and nothing here can name a pixel.
//
// And it owns no LIFT. `has_lift` comes in as an argument to barNavInput for the one rule that
// needs it (B cancels a lift before it closes the bar), for the same reason scene/ui_gesture.h
// takes `lifted_slot` in rather than keeping one: scene/ui.h's UiState.picked_slot is the single
// copy, and a second one in here would be one screen switch away from disagreeing with it.
//
// ── The model ─────────────────────────────────────────────────────────────────────────────
//
// A bar is a row of CATEGORIES (tabs). One is focused. Under it is that category's content,
// which is a grid of ROWS, each row a number of COLUMNS. A list is a grid whose rows are one
// column wide. The cursor is (cat, row, col) and is always on a real cell — that invariant is
// what every caller is allowed to assume, and barNavClamp is what restores it when the set of
// categories changes underneath the cursor (a chest closing takes its tab away).
//
// Two pieces of memory make the bar feel like it remembers where you were:
//
//   pref_col   the column the player last chose ON PURPOSE, i.e. with left/right. Vertical
//              moves restore min(pref_col, cols-1) rather than carrying the clamped column, so
//              walking down a wide inventory row (col 7) through a 3-wide furnace row (col 2)
//              and out the other side lands back on col 7, not on col 2. Without this the
//              cursor drifts left every time it crosses a narrow row, which reads as the bar
//              losing your place.
//   row_mem    the row each category was left on. Switching to CRAFT and back to INVENTORY puts
//              the cursor on the inventory row you were on, not on row 0.
//
// ── Priority: ONE event per call ──────────────────────────────────────────────────────────
//
// barNavInput answers with exactly one BarEvent for the frame, chosen in this order (§6.1):
//
//   B          -> BAR_EV_CANCEL if has_lift, else BAR_EV_BACK.  NEVER BACK while lifting: a
//                 player with a stack in the air who presses B means "put it down", and closing
//                 the bar instead would strand the lift on a screen they can no longer see.
//   A          -> BAR_EV_COMMIT (the caller turns this into a tap at the focused cell's centre)
//   X          -> BAR_EV_ALT (reserved for the stack-split gesture v1.9.0 still owes; no caller
//                 in v1.9.1 acts on it)
//   L / R      -> BAR_EV_CAT, previous / next tab, CLAMPED, no wrap
//   up / down  -> BAR_EV_MOVE, WRAPS within the category's rows
//   left/right -> depends on the category (see BarCatSpec.horizontal_is_content):
//                   content + a row wider than one column -> BAR_EV_MOVE, clamped, sets pref_col
//                   content + a one-column row            -> BAR_EV_STEP_LEFT / _STEP_RIGHT
//                   not content                           -> BAR_EV_CAT, as L / R
//
// BAR_KEY_Y is defined so callers can pass a whole `down` word without masking, and is read by
// nothing here. Anything this module does not claim is simply BAR_EV_NONE.
//
// An OPPOSING PAIR on the same frame (L and R together, up and down, left and right) is a tie:
// that pair moves nothing and the next band down gets its turn, so L|R|DDOWN still moves a row.
// Same rule, for the same reason, as scene/hotbar.c's L|R tie — there is no order to break it
// with, and inventing one would fire a step the player did not ask for.
//
// NOTHING MOVED IS NOT AN EVENT. L at the leftmost tab, right at the rightmost column, up in a
// one-row category: all BAR_EV_NONE, not a CAT or MOVE that changed nothing. The callers play
// SFX_UI_TAP on every event (D13), and a click for a cursor that did not move is the noise a
// menu makes when it is lying to you.
//
// ── Zero is a valid state ─────────────────────────────────────────────────────────────────
//
// A memset-zero BarNav is cat 0, row 0, col 0 and no memory — the correct initial state, which
// is what lets scene/ui.h embed one in UiState and let uiInit's memset do the work (the same
// contract scene/ui_gesture.h's UiGesture has). A memset-zero BarCatSpec is `rows == 0`, a
// category with NO cursor content: up/down/left/right and A all answer BAR_EV_NONE on it, and
// the cursor is pinned at (0,0). Feeding a NULL nav, NULL cats or a cat_count of 0 is inert.
#ifndef SCENE_BARNAV_H
#define SCENE_BARNAV_H

#include <stdbool.h>
#include <stdint.h>

// libctru's key bits (3ds/services/hid.h), mirrored so this header compiles without <3ds.h> —
// the same choice scene/hotbar.h and scene/ui_gesture.h make. barnav.c static-asserts every one
// of them against the real thing when built for the console, and scene/ui.c carries the same
// assert at the point where a real hidKeysDown() word is handed in (blueprint §6.3).
#define BAR_KEY_A      0x0001u
#define BAR_KEY_B      0x0002u
#define BAR_KEY_DRIGHT 0x0010u
#define BAR_KEY_DLEFT  0x0020u
#define BAR_KEY_DUP    0x0040u
#define BAR_KEY_DDOWN  0x0080u
#define BAR_KEY_R      0x0100u
#define BAR_KEY_L      0x0200u
#define BAR_KEY_X      0x0400u
#define BAR_KEY_Y      0x0800u

// The bar with the most tabs in v1.9.1 is the title's four (PLAY/MULTIPLAYER/OPTIONS/SYSTEM);
// the in-game bar has three at most. Six is headroom, and it is also the length of row_mem, so
// a cat_count above it is clamped rather than read past the end of that array.
#define BARNAV_MAX_CATS 6

// Only the length of BarCatSpec.cols[], i.e. the most rows a category with PER-ROW widths may
// have. A category with uniform_cols > 0 (every list, and every grid whose rows are all the
// same width) has no such limit — CRAFT's row count is the recipe count and is free to grow.
#define BARNAV_MAX_ROWS 8

// Circle-pad hysteresis for barStickEdge, in the units hidCircleRead reports (roughly +/-156
// at full deflection). Fire at 40 and re-arm at 24, not at one threshold: a stick resting near
// a single boundary jitters across it and would emit a stream of steps the player never asked
// for. The gap between the two is the whole point of having two numbers.
#define BARNAV_STICK_FIRE  40
#define BARNAV_STICK_REARM 24

// One category's shape, as the category itself describes it to this module.
//
//   rows                   cursor rows in this category. 0 = no cursor content (a text page):
//                          the cursor pins to (0,0) and every move and commit is NONE.
//   uniform_cols           >0: every row is this many columns wide and cols[] is ignored. A
//                          list is 1. This is the escape hatch for a category with more rows
//                          than BARNAV_MAX_ROWS.
//   cols[]                 per-row widths, read only when uniform_cols == 0, and only for
//                          rows < BARNAV_MAX_ROWS. A stored 0 reads as 1: a row always has at
//                          least the one cell the cursor is sitting on.
//   horizontal_is_content  what left/right MEAN here. true on a grid (move the cell) and on a
//                          stepper list (change the value); false on a plain list such as
//                          CRAFT, where left/right switch tabs because there is nothing for
//                          them to do inside a one-column row and a dead direction on a list
//                          feels broken (blueprint D6).
typedef struct {
	int     rows;
	int     uniform_cols;
	uint8_t cols[BARNAV_MAX_ROWS];
	bool    horizontal_is_content;
} BarCatSpec;

// The cursor. Zero-initialise ({0} or a memset) and move it only through the calls below.
//
//   cat, row, col  the focused cell. Always real after any call here: see barNavClamp.
//   pref_col       the column last chosen with left/right — see the header note above.
//   row_mem        the row each category was last left on, one byte per category. uint8_t
//                  because a bar row index is small and this struct is embedded in UiState;
//                  a row above 255 is stored as 255, which costs a remembered position on a
//                  list longer than the screen has ever been and no correctness.
typedef struct {
	int     cat, row, col, pref_col;
	uint8_t row_mem[BARNAV_MAX_CATS];
} BarNav;

// 4 ints + 6 bytes rounded to the int alignment. Pinned because scene/ui.h embeds one in
// UiState and the 3DS stack is 32 KB — a struct that quietly grows is a stack cost nobody
// re-measures. This is safe to assert across the two builds precisely BECAUSE there is no enum
// in it: -fshort-enums makes the console pack an enum into one byte and the host into four, so
// a sizeof over any struct holding a BarEvent would disagree between them. See BarEvent below.
_Static_assert(sizeof(BarNav) == 24, "BarNav must stay 24 bytes - it is embedded in UiState");
_Static_assert(sizeof(BarCatSpec) == 20, "BarCatSpec must stay 20 bytes - callers hold arrays of them");

// The frame's answer. One per barNavInput call.
//
// NEVER written to the wire, to a save file or to a struct whose size is asserted, so
// -fshort-enums (one byte on the console, four on the host) changes nothing a caller can
// observe — the same note scene/ui_gesture.h's UiGestureKind carries, for the same reason.
typedef enum {
	BAR_EV_NONE = 0,
	BAR_EV_CAT,          // the focused tab changed; nav.cat is the new one
	BAR_EV_MOVE,         // the focused cell changed; nav.row / nav.col are the new one
	BAR_EV_COMMIT,       // A on the focused cell
	BAR_EV_ALT,          // X; reserved, no v1.9.1 caller acts on it
	BAR_EV_STEP_LEFT,    // left on a one-column content row: decrease the value
	BAR_EV_STEP_RIGHT,   // right on a one-column content row: increase it
	BAR_EV_CANCEL,       // B with a lift in the air: put it down, do NOT close
	BAR_EV_BACK,         // B with no lift: close the bar / leave the screen
} BarEvent;

// Back to a named cell. Zeroes the whole struct first, so this is also how a caller that did
// not memset one gets a valid state. Negative arguments floor at 0; nothing is clamped against
// a category list, because none is given — follow this with barNavClamp if the caller has one.
// pref_col is set to `col`: opening the bar on the in-hand hotbar slot should make that column
// the one vertical moves return to.
void barNavReset(BarNav* n, int cat, int row, int col);

// One frame. `keys` is the frame's PRESS EDGES (hidKeysDown()), in the BAR_KEY_* bits above,
// optionally OR-ed with barStickEdge's answer so the circle pad drives the same paths as the
// d-pad. `cats` is the live category list, `cat_count` its length, `has_lift` whether a stack
// is in the air right now.
//
// Normalises the cursor (barNavClamp) BEFORE reading the keys, every call, so a caller that
// rebuilt its category list this frame — a chest closing, taking tab 2 with it — cannot get an
// event computed against a cell that no longer exists.
//
// keys == 0 answers BAR_EV_NONE and moves nothing, which is what makes "feed it every frame,
// including while the game is paused with an empty word" the correct way to call it.
BarEvent barNavInput(BarNav* n, uint32_t keys, const BarCatSpec* cats, int cat_count, bool has_lift);

// Puts the cursor back on a real cell against the CURRENT category list. Call after any change
// to that list. A cat at or past `cat_count` (or negative) goes to 0 — the container tab
// disappearing is the case this exists for, and category 0 is INVENTORY, which always exists.
// row and col are clamped into their ranges rather than reset, so a survivable change keeps the
// player's place. pref_col is floored at 0 and otherwise left alone: it is a memory of a wide
// row and is MEANT to exceed a narrow one's width.
void barNavClamp(BarNav* n, const BarCatSpec* cats, int cat_count);

// Focus a category by index, exactly as an L/R press would: the current row is written into
// row_mem, the target's remembered row is restored, and the cursor is clamped. Out-of-range
// clamps into range. This is what scene/ui.c calls when a chest or furnace opens (nav.cat = 2).
void barNavSetCat(BarNav* n, int cat, const BarCatSpec* cats, int cat_count);

// Focus a cell within the current category, clamped, and take `col` as the new pref_col. This
// is the stylus path: a tap lands on a cell, and the cursor follows it so a d-pad press
// afterwards continues from where the player was looking rather than from where it used to be.
void barNavSetCell(BarNav* n, int row, int col, const BarCatSpec* cats, int cat_count);

// The scroll offset a list should use to keep `cursor_row` on screen: the smallest change to
// `scroll` that does it, so the window follows the cursor off each edge one row at a time
// instead of recentring. `visible` is how many rows fit, `rows` how many there are. Answers 0
// whenever the list fits (rows <= visible) or the arguments are nonsense, and never a value
// outside [0, rows - visible]. Pure: it writes nothing.
int barNavScrollFor(int cursor_row, int scroll, int visible, int rows);

// The circle pad's edge state, one per bar. Zero-initialise. dir_x / dir_y are -1, 0 or +1 —
// which way the stick is currently deflected far enough to count — and are the whole of it.
typedef struct { int8_t dir_x, dir_y; } BarStick;

_Static_assert(sizeof(BarStick) == 2, "BarStick must stay 2 bytes");

// Turns a circle-pad reading into d-pad PRESS EDGES: the BAR_KEY_D* bits for the directions the
// stick just deflected into this frame, ready to OR into barNavInput's `keys`. `dx` / `dy` are
// hidCircleRead's, with dx positive right and dy positive UP (libctru's sign, not the screen's).
//
// One press per deflection: a stick held over is one edge, not one a frame. An axis fires when
// its magnitude reaches BARNAV_STICK_FIRE and re-arms when it falls below BARNAV_STICK_REARM;
// in between it holds whatever it decided. Flicking straight from full left to full right
// without passing through the middle is a NEW deflection and fires — the direction changed,
// which is the thing the edge is about. Diagonals fire both axes on the same frame; the caller
// gets DUP|DRIGHT, and barNavInput's one-event-per-call rule then picks the vertical.
uint32_t barStickEdge(BarStick* s, int dx, int dy);

#endif
