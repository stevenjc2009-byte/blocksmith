// Step 8.2 (touchscreen UI half). The bottom-screen game screen: hotbar, the inventory
// grid, and the crafting panel, all driven by inventory.h/crafting.h's already-tested data
// model — see this project's world/inventory.h and world/crafting.h for the rules this file
// is a view onto and never re-implements.
//
// Same shape as scene/title.h, which this project already treats as the house style for a
// scene-ish UI module: an opaque-ish state struct, one init, and one *UpdateDraw per frame
// that takes a plain input struct and hands back what happened. Also like title.h, this
// file never touches HID (hidScanInput/touchRead stay in main.c) and never calls
// C3D_FrameDrawOn or C3D_RenderTargetClear — its first move is spriteBegin(320, 240), so
// whatever render target the caller bound before calling in is what it draws onto. In
// practice that is always screenBottom(), and the caller must check screenBottom() is
// non-NULL (a BS_BOTTOM_UI=0 build has no such target) before calling in at all — see
// main.c's existing drawBottomUi for the exact call shape (C3D_RenderTargetClear then
// C3D_FrameDrawOn then the UI draw) this file is meant to slot into unchanged.
//
// ── Touch only, deliberately ─────────────────────────────────────────────────────────────
//
// title.c's screens accept both a D-pad cursor and a tap, because a title screen is reached
// before the player necessarily trusts the touchscreen and because every one of its targets
// is a full-width row or button that a D-pad can usefully focus one at a time. This screen
// is different: it is reached *during* play, its two live gestures are "pick a thing up"
// and "put it down somewhere else", and both are naturally a point in a grid rather than a
// linear list a D-pad cursor can step through one cell at a time without becoming its own
// two-axis input problem. So this file reads only a touch point (see UiInput below) and
// carries no keys_down at all — unlike TitleInput, which genuinely uses one. A field this
// screen would never read is exactly the kind of speculative surface this project's style
// avoids elsewhere. See ui.c's file comment for the pick-up/drop gesture itself, and why it
// was chosen over a continuous drag.
//
// ── The block-icon texture is a parameter, not a global lookup ──────────────────────────
//
// gfx/font.h exposes fontTexture() so a caller can hand the font sheet to spriteTexture()
// itself; gfx/atlas.h has no equivalent for the block atlas — only atlasBind(), which binds
// it directly and bypasses gfx/sprite.h's own texture-change tracking (sprite.c's static
// s_tex, updated only by spriteTexture()). Calling atlasBind() from in here would desync
// that tracking and could leave the wrong texture bound for whatever spriteTexture() call
// runs next, in either direction — a real bug, not a style objection. Closing that gap means
// adding a `C3D_Tex* atlasTexture(void)` to gfx/atlas.h/.c (mirroring fontTexture() in
// gfx/font.h:50 and gfx/font.c:55-58 exactly), which is outside this file's ownership for
// the step 8.2 UI task, so instead this file takes the block atlas's C3D_Tex* as a plain
// parameter to uiUpdateDraw — the same shape fontTexture() already returns. Pass NULL until
// that accessor exists and every slot still draws — background, border, count — just
// without the block art; see ui.c's drawSlotIcon for the fallback (the block's name, in
// text).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <citro3d.h>   // C3D_Tex only — see the file comment above for why

#include "world/inventory.h"

// Which of the two screens this frame is drawing. UI_SCR_HUD is the default: just the
// hotbar (always visible, per inventory.h's own INV_HOTBAR_SLOTS comment) plus the engine
// status text main.c already computes. UI_SCR_INVENTORY adds the main grid and the crafting
// list, covering the rest of the screen — see ui.c's layout block for the exact pixels.
typedef enum {
	UI_SCR_HUD,
	UI_SCR_INVENTORY,
} UiScreen;

typedef struct {
	UiScreen screen;
	bool     touch_prev;    // last frame's touch_down, so a tap fires once — the same trick
	                         // title.c's TitleState uses, for the same reason
	int      picked_slot;   // an inventory.h slot index currently "lifted" for a move,
	                         // mid pick-up/drop; -1 when nothing is lifted
} UiState;

// Zeroes `ui` and puts it on the HUD screen with nothing picked up. Call once, before the
// first uiUpdateDraw.
void uiInit(UiState* ui);

// One frame's touch point. See the file comment for why there is no keys_down here, unlike
// TitleInput.
//
//   touch_down  true while the stylus/finger is on the bottom screen this frame — main.c
//               already derives this from touchRead() for its own reasons (see title.h's
//               TitleInput for the identical note); this file only ever reads it, never
//               calls into libctru itself.
//   touch_x/y   bottom-screen pixels, the 320x240 space spriteBegin(320,240) draws in.
//               Meaningful only when touch_down is true.
typedef struct {
	bool touch_down;
	int  touch_x, touch_y;
} UiInput;

// The world/engine numbers this screen keeps showing in HUD mode — exactly what step 8.3's
// drawBottomUi drew, minus that step's own font-demo content (every printable glyph, the
// tint proof, the scale proof) and its self-referential "ui batch: N draws M quads" line —
// dropped; see ui.c's file comment and the step 8.2 UI task report for why. Plain values
// rather than pointers into main.c's World/ChunkRender state, so this file never has to
// know either type's shape — main.c already computes every one of these for its own status
// line and console overlay and just hands the same numbers over again.
//
// May be passed as NULL to uiUpdateDraw, in which case the metrics/status block — and, since
// v1.8.13, the health/hunger pips below it, which live on the same `stats` — is simply not
// drawn (the hotbar and the open/close button still are) — that is the only condition under
// which any field here is treated as optional; a non-NULL UiStats has every field read, never
// written.
typedef struct {
	int         columns, chunks;
	int         meshes, culled;
	uint32_t    tris;
	uint32_t    bytes, bytes_peak;
	const char* status;   // e.g. "aim -12 8 -10 f2 b9 p9 r9 dq8/8" — main.c already builds
	                       // this exact string. May itself be NULL even when the struct
	                       // isn't; that one line is then left blank.
	const char* net;      // e.g. "net s4 r91 y12 a12 q0 p1" — the multiplayer traffic counters
	                       // from net/networld.h, built by main.c the same way as `status`.
	                       // Same NULL rule: the line is simply omitted.

	// v1.8.11 METRICS-VISIBLE. e.g. "cpu 2.1  wait 14.6  frame 16.7ms  60fps". NULL — and so
	// no row at all — unless the debug menu's "Frame timing" toggle is on, which is how this
	// stays a debug option rather than a permanent HUD line. main.c builds it from
	// debug/metrics.h's averages for the same reason it builds `status`: this header must not
	// grow a dependency on <3ds.h>, and every other number on this panel arrives the same way.
	const char* timing;

	// v1.8.13 SURV-HUD. The player's vitals, drawn as two 10-pip bars at the bottom of the
	// HUD screen (see scene/ui_layout.h's vitals block for where on the panel they fit and
	// why nothing else could move to make room).
	//
	// 0..20, i.e. two points per pip, the same scale world/survival.h keeps them on — so an
	// odd value is half a pip and ui.c draws it as one. Values outside 0..20 are clamped by
	// ui_layout.h's hudPipFill rather than trusted, so a caller that has not wired these up
	// yet gets an empty bar, never a crash and never a bar drawn past its tenth pip.
	//
	// Zero is a legitimate reading (an empty bar), NOT "unset". main.c builds this struct with
	// a designated initialiser, so both fields are already zero-initialised in every build that
	// has not populated them; a build in that state draws two empty bars, which is exactly what
	// health 0 looks like. That is a deliberate contract and not a placeholder: there is no
	// spare sentinel in a uint8_t 0..20 that would mean "no data", and inventing one would put
	// a second meaning on a field ui.c only ever reads.
	uint8_t health, hunger;   // 0..20
} UiStats;

// What this frame's screen was, in case the caller wants to gate something on it — e.g.
// world-interaction input while the player is visibly rearranging storage rather than
// looking at the world. Reflects the screen this frame actually drew, the same "a change
// takes effect next frame" rule title.c's own ts->screen switch follows: a tap that opens
// the overlay this frame is still reported as closed here, because the HUD screen is what
// was drawn this frame.
typedef struct {
	bool inventory_open;
} UiResult;

// One frame: advances `ui`, edits `inv` in place (slot moves, hotbar selection, crafting),
// draws the current screen onto whatever render target the caller bound, and reports what
// the caller might want to know afterwards.
//
// `block_icons` is the block atlas's C3D_Tex*, or NULL — see the file comment. `stats` may
// be NULL — see UiStats above.
UiResult uiUpdateDraw(UiState* ui, Inventory* inv, C3D_Tex* block_icons,
                       const UiStats* stats, const UiInput* in);
