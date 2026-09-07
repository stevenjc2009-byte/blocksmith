// Step 8.4 (screens half). Title screen, world select, and the options screen that edits a
// caller-owned Options struct — everything the player sees before playerInit runs.
//
// A small state machine, TITLE -> WORLD_SELECT -> (play) and TITLE -> OPTIONS -> TITLE,
// entirely inside TitleState so main.c owns nothing but one struct and one call per frame.
//
// It never touches HID itself. hidScanInput and touchRead stay in main.c's frame loop,
// where every other subsystem already reads them, and this is handed the results instead —
// see TitleInput below. It never calls C3D_FrameDrawOn or C3D_RenderTargetClear either:
// titleUpdateDraw's first move is spriteBegin(320, 240), so whatever render target the
// caller bound before calling in is what it draws onto. In practice that is always
// screenBottom() — every layout number in title.c assumes exactly 320x240 — and the caller
// must check screenBottom() is non-NULL (a BS_BOTTOM_UI=0 build has no such target at all)
// before calling this file at all.
//
// This is console-only: it draws with gfx/sprite.h and gfx/font.h, both of which need
// <3ds.h> and citro3d, so unlike scene/worldlist.h underneath it there is no host test for
// this half. What can be, and is, tested on the host is everything worldlist.c does — see
// world/worldlist_test.c — and everything options.c does — see app/options_test.c. What is
// left untested by either is purely "did the right rectangle get tapped", which is the one
// category of bug this project's plan puts back on a human looking at the emulator, the
// same way it already does for gfx/font.c's glyph shapes (step 8.3's font_preview.png).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <3ds.h>

#include "app/options.h"
#include "app/version_history.h"
#include "app/whatsnew.h"
#include "scene/barnav.h"      // v1.9.1: the four-tab bar's cursor lives in TitleState below
#include "scene/worldlist.h"

// Where the options file lives. Public so main.c can load it once at boot with the same
// constant this file saves it back to — one path, one place it is spelled out, the same
// reason world/region.h makes REGION_ROOT public for the world list below to share.
#define TITLE_OPTIONS_PATH "sdmc:/blocksmith/options.ini"

typedef enum {
	TITLE_STAY,          // no navigation happened this frame; keep calling every frame
	TITLE_START_WORLD,   // player picked (or just created) a world — see TitleResult below
	// Player joined a server, so the world to play is the server's rather than one of ours.
	// It carries no name and no directory: the server owns both halves of what makes a world
	// a world — the seed its terrain generates from and the block edits made in it — which is
	// why this is its own action instead of a TITLE_START_WORLD carrying some reserved name.
	// The caller must not call its saveWorldDir() equivalent at all on this path (that
	// function mkdirs) and must pass NULL to workerSetWorldDir, which app/worker.h documents
	// as "no save file at all", so a session leaves nothing on the card that could later
	// disagree with the server.
	TITLE_START_SERVER,
	TITLE_QUIT,          // player chose Quit on the main screen
} TitleAction;

typedef struct {
	TitleAction action;
	// Valid only when action == TITLE_START_WORLD — TITLE_START_SERVER leaves it empty,
	// because a server session has no directory of its own at all. A directory name under
	// REGION_ROOT (world/region.h) — exactly what workerSetWorldDir(dir) wants once the
	// caller has joined it onto REGION_ROOT, not a full path itself, since
	// worldlistScan/Create never deal in full paths either.
	char world_name[WORLDLIST_NAME_MAX];
} TitleResult;

// v1.9.1 (blueprint D10). TITLE_SCR_MAIN is now the four-tab BAR — PLAY / MULTIPLAYER /
// OPTIONS / SYSTEM — rather than a list of four buttons, and three of the values below stopped
// being screens the moment their content became a tab of it: WORLD_SELECT, OPTIONS_GENERAL and
// MULTIPLAYER. They are kept, with their original numbering, because a caller may still ASK for
// one — main.c writes TITLE_SCR_MULTIPLAYER into a freshly-inited TitleState to land the player
// back on multiplayer after a server session — and title.c's titleUpdateDraw turns any of the
// three into TITLE_SCR_MAIN focused on the matching tab on the frame it sees it. Writing one of
// them is therefore still exactly the request it always was; it just resolves to a tab.
//
// The remaining three are real screens still, unchanged in layout (blueprint D1: leaf screens
// keep their layouts and are reached from bar rows).
typedef enum {
	TITLE_SCR_MAIN,            // v1.9.1: the four-tab bar
	TITLE_SCR_WORLD_SELECT,    // v1.9.1: resolves to the PLAY tab
	TITLE_SCR_OPTIONS_GENERAL, // v1.9.1: resolves to the OPTIONS tab
	TITLE_SCR_OPTIONS_BINDINGS,
	TITLE_SCR_MULTIPLAYER,     // v1.9.1: resolves to the MULTIPLAYER tab
	TITLE_SCR_UPDATE,          // SYSTEM's "CHECK FOR UPDATE" row - see title.c's drawUpdate
	// The update screen's top-left "VERSION HISTORY" button - see title.c's
	// drawVersionHistory/drawVersionHistoryTop and app/version_history.h.
	TITLE_SCR_VERSION_HISTORY,
} TitleScreenId;

typedef struct {
	TitleScreenId screen;
	int           cursor;        // focused item index on the current screen
	bool          touch_prev;    // last frame's touch_down, so a tap fires once on the edge

	// The PLAY tab's cache of worldlistScan's result, filled once on *entering* the tab rather
	// than every frame — a directory listing is a real SD-card cost, the same reason
	// region.h avoids a file per column. See title.c's titleEnterPlayTab.
	WorldEntry worlds[WORLDLIST_MAX];
	int        world_count;
	bool       world_list_truncated;
	int        world_scroll;     // index of the first visible row in the world list

	// The PLAY tab's "press Y again to delete" latch (v1.9.0 item 6.3) — armed, disarmed,
	// expired and consumed entirely by scene/worldlist.h's worldlistUiStep; this file only
	// draws off it. Its all-zero value is "off" by construction (worldlist.h stores the row
	// PLUS ONE for exactly that reason), so titleInit's memset already disarms it; titleInit
	// and titleEnterPlayTab still call worldlistConfirmReset on it so the disarm is
	// visible at the two places it has to hold rather than an accident of the encoding.
	WorldDeleteConfirm world_confirm;

	// Bindings screen: the action index waiting for its next key press, or -1 for none.
	int rebind_action;

	// A short status line shown at the top of world select — "name already used" and the
	// like. Counts itself down every titleUpdateDraw call so nothing has to remember to
	// clear it once shown.
	char status[48];
	int  status_ttl;

	// Update screen: where the release notes on the TOP screen are scrolled to, and the
	// held-to-repeat state for each D-pad direction that scrolls them (v1.6.0 task 14b).
	// Reset on entering the screen; clamped every frame by titleDrawTop, because the notes
	// can arrive and change length while the player is already looking at the screen.
	int            notes_scroll;
	WhatsNewRepeat notes_rep_up;
	WhatsNewRepeat notes_rep_down;

	// Version history browser (v1.8.8): which row is selected in the combined list, newest
	// first - row 0 is an update the check already found but the player has not installed
	// yet, when there is one (see app/version_history.h's file comment), then every baked
	// app/version_history.c entry. `vh_scroll` is the first visible row of that list on the
	// bottom screen, same auto-follow idea the PLAY tab's world_scroll already uses above.
	// The notes body the selected row shows on the TOP screen reuses notes_scroll/
	// notes_rep_up/notes_rep_down above rather than duplicating them, because this screen and
	// the update screen are never open at the same time.
	int vh_cursor;
	int vh_scroll;

	// v1.9.1 (blueprint D10). Which of the four tabs is focused and which row inside it — the
	// whole of the bar's cursor. A memset-zero BarNav is cat 0 / row 0, i.e. PLAY focused on
	// NEW WORLD, which is exactly the state titleInit wants, so this needs no initialiser of
	// its own (scene/barnav.h, "Zero is a valid state"). `world_scroll` above is the PLAY
	// tab's scroll window as well as world select's; the other three tabs are shorter than the
	// six-row window and never scroll.
	BarNav nav;

	// Version history was opened from the UPDATE screen's own small button (true) rather than
	// straight off the SYSTEM tab (false). B on that leaf goes back to whichever one it was,
	// so a player who never opened the update screen is never dropped onto it on the way out.
	bool vh_from_update;
} TitleState;

// Zeroes `ts` and puts it on the bar with the PLAY tab focused. Call once, before the first
// titleUpdateDraw.
//
// v1.9.1: this also does the one worldlistScan the PLAY tab needs, because PLAY is now the
// tab the menu opens on and its rows are the world list — there is no longer a "navigate to
// world select" moment to hang the scan off. It is still one directory listing per visit to
// the menu, not one a frame; the tab re-scans only when the player tabs back onto it, or
// after a world is created, renamed or deleted (see title.c's titleEnterPlayTab).
void titleInit(TitleState* ts);

// One frame's worth of input, read by the caller and handed in — see the file comment for
// why this file never reads HID itself.
//
//   keys_down  hidKeysDown() this frame. On the bar (v1.9.1, blueprint D6/D10): L/R switch
//              tab, D-pad Up/Down move the focused row, D-pad Left/Right step a focused
//              stepper on OPTIONS and switch tab on every other tab, A activates the focused
//              row, B leaves a tab for PLAY (and, on PLAY, cancels an armed delete first).
//              On the three leaf screens it means exactly what it always did: Up/Down move
//              the focused item, Left/Right adjust a stepper, A activates, B goes back.
//              The caller may OR barStickEdge()'s answer into this word so the circle pad
//              drives the same paths as the D-pad (blueprint seam S8); nothing here can tell
//              the difference, which is the point.
//   touch_down true while the stylus/finger is on the bottom screen this frame. libctru's
//              KEY_TOUCH is never actually set by hidScanInput — hid.h says so on the enum
//              value itself ("Not actually provided by HID") — so the caller has to derive
//              this some other way, typically touchRead() and checking px/py are not both
//              0, which main.c already has to call for its own reasons regardless of this
//              screen existing.
//   touch_x/y  bottom-screen pixels, the same 320x240 space spriteBegin(320,240) draws in.
//              Meaningful only when touch_down is true.
//   keys_held  hidKeysHeld() this frame. Used by exactly one thing: scrolling the update
//              screen's release notes on the top screen, which repeats while the D-pad is
//              held (v1.6.0 task 14b). It is a separate word from keys_down on purpose —
//              app/whatsnew.h's comment on whatsnewRepeatStep records why, and this project
//              has shipped the two-layers-one-keys_down bug twice. Nothing else in this file
//              reads it, and no bit is read out of both words on the same screen.
typedef struct {
	uint32_t keys_down;
	uint32_t keys_held;
	bool     touch_down;
	int      touch_x, touch_y;
} TitleInput;

// One frame: advances `ts`, edits `opts` in place, draws the current screen onto whatever
// render target the caller bound, and reports what the caller should do next.
//
// `opts` must already be loaded (optionsLoad from TITLE_OPTIONS_PATH, at boot, before the
// first call) — this file only ever edits an existing Options in place. Both options
// screens call optionsSave(opts, TITLE_OPTIONS_PATH) themselves on the way back out to the
// main screen, so a battery pull mid-menu loses at most the field the player was touching
// when it happened, never the whole session's changes.
TitleResult titleUpdateDraw(TitleState* ts, Options* opts, const TitleInput* in);

// The menu's TOP-screen half, drawn onto whatever 400x240 target the caller bound — in
// practice screenTop(). Reads no input at all; it paints from `ts` and clamps `ts`'s scroll
// position against the content it just laid out, which is why it takes a mutable pointer.
//
// Today it draws exactly one thing: the release notes for the version the update screen is
// offering (v1.6.0 task 14b). On every other screen it paints the background and nothing
// else, so the top screen is never left holding uninitialised VRAM.
//
// Must be called AFTER titleUpdateDraw in the same frame, with the top target bound. That
// order is what makes a scroll press visible on the frame it happens rather than the next
// one: titleUpdateDraw is where the D-pad is read.
void titleDrawTop(TitleState* ts);
