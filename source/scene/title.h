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
#include "scene/worldlist.h"

// Where the options file lives. Public so main.c can load it once at boot with the same
// constant this file saves it back to — one path, one place it is spelled out, the same
// reason world/region.h makes REGION_ROOT public for the world list below to share.
#define TITLE_OPTIONS_PATH "sdmc:/blocksmith/options.ini"

typedef enum {
	TITLE_STAY,          // no navigation happened this frame; keep calling every frame
	TITLE_START_WORLD,   // player picked (or just created) a world — see TitleResult below
	TITLE_QUIT,          // player chose Quit on the main screen
} TitleAction;

typedef struct {
	TitleAction action;
	// Valid only when action == TITLE_START_WORLD. A directory name under REGION_ROOT
	// (world/region.h) — exactly what workerSetWorldDir(dir) wants once the caller has
	// joined it onto REGION_ROOT, not a full path itself, since worldlistScan/Create never
	// deal in full paths either.
	char world_name[WORLDLIST_NAME_MAX];
} TitleResult;

typedef enum {
	TITLE_SCR_MAIN,
	TITLE_SCR_WORLD_SELECT,
	TITLE_SCR_OPTIONS_GENERAL,
	TITLE_SCR_OPTIONS_BINDINGS,
	TITLE_SCR_MULTIPLAYER,
	TITLE_SCR_UPDATE,          // Options' "CHECK FOR UPDATE" button - see title.c's drawUpdate
} TitleScreenId;

typedef struct {
	TitleScreenId screen;
	int           cursor;        // focused item index on the current screen
	bool          touch_prev;    // last frame's touch_down, so a tap fires once on the edge

	// World select's cache of worldlistScan's result, filled once on *entering* the screen
	// rather than every frame — a directory listing is a real SD-card cost, the same reason
	// region.h avoids a file per column. See title.c's titleEnterWorldSelect.
	WorldEntry worlds[WORLDLIST_MAX];
	int        world_count;
	bool       world_list_truncated;
	int        world_scroll;     // index of the first visible row in the world list

	// Bindings screen: the action index waiting for its next key press, or -1 for none.
	int rebind_action;

	// A short status line shown at the top of world select — "name already used" and the
	// like. Counts itself down every titleUpdateDraw call so nothing has to remember to
	// clear it once shown.
	char status[48];
	int  status_ttl;
} TitleState;

// Zeroes `ts` and puts it on the main screen. Call once, before the first titleUpdateDraw.
void titleInit(TitleState* ts);

// One frame's worth of input, read by the caller and handed in — see the file comment for
// why this file never reads HID itself.
//
//   keys_down  hidKeysDown() this frame. D-pad Up/Down move the focused item, Left/Right
//              adjust a focused stepper, A activates the focused item, B goes back a screen
//              (in addition to the on-screen Back button every non-main screen also has).
//   touch_down true while the stylus/finger is on the bottom screen this frame. libctru's
//              KEY_TOUCH is never actually set by hidScanInput — hid.h says so on the enum
//              value itself ("Not actually provided by HID") — so the caller has to derive
//              this some other way, typically touchRead() and checking px/py are not both
//              0, which main.c already has to call for its own reasons regardless of this
//              screen existing.
//   touch_x/y  bottom-screen pixels, the same 320x240 space spriteBegin(320,240) draws in.
//              Meaningful only when touch_down is true.
typedef struct {
	uint32_t keys_down;
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
