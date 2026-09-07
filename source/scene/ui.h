// Step 8.2 (touchscreen UI half). The bottom-screen game screen: hotbar, the inventory
// grid, and the crafting panel, all driven by inventory.h/crafting.h's already-tested data
// model — see this project's world/inventory.h and world/crafting.h for the rules this file
// is a view onto and never re-implements.
//
// v1.8.15 FURNACE added a third screen on the same terms: an open furnace's input, fuel and
// output slots plus two readouts, a view onto world/furnace.h's already-tested state machine.
// This file does not tick a furnace, does not decide what smelts or what burns, and does not
// know where a furnace's state is stored — it moves stacks in and out and draws two gauges.
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
// ── 2026-09-06, v1.9.0 SPLIT: the paragraph above is HISTORY ────────────────────────────
//
// UiInput now carries `keys_held` and this screen reads two bits of it: Y splits a lifted
// stack, X quick-moves one to the other container (scene/ui_gesture.h). The reasoning above
// still holds for the D-pad — nothing here is cursor-driven — and for taps, which stay the
// two-tap lift/place. The field was added the day a gesture needed it, not before.
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

#include "net/networld.h"      // NetworldInvState — v1.9.1, see uiApplyInvSnapshot. The type is
                               // an anonymous-struct typedef, so it cannot be forward-declared.
#include "scene/barnav.h"      // BarNav — v1.9.1 BAR, see UiState.nav
#include "scene/ui_gesture.h"  // UiGesture — v1.9.0 SPLIT, see UiState.gesture
#include "world/chest.h"       // ChestState — see uiUpdateDraw's `chest` parameter
#include "world/furnace.h"     // FurnaceState — see uiUpdateDraw's `furnace` parameter
#include "world/inventory.h"

// ── v1.9.1 BAR: four screens became two ────────────────────────────────────────────────
//
// The separate inventory, furnace and chest screens are GONE, enum values and all. They were
// three screens with one layout each, three close bars in the same band, and three separate
// answers to "where is the bag". They are now three TABS of one screen — UI_SCR_BAR — whose
// focused category is
// `UiState.nav.cat` and whose live category list is scene/ui_layout.h's barBuildKinds():
// INVENTORY, CRAFT, and the container (CHEST or FURNACE, never both, because main.c opens one
// per PLACE press) at index 2 while one is open. See docs/blueprint-1.9.1-interface.md §D1-D8.
//
// UI_SCR_HUD is unchanged and is still the default: the hotbar (always visible, per
// inventory.h's own INV_HOTBAR_SLOTS comment), the engine status text main.c already computes,
// the vitals pips, and the strip under the hotbar that opens the bar.
//
// ⚠ The NULL-pointer contract the two container screens carried is UNCHANGED in substance and
// now reads off the pointers instead of off the screen: uiUpdateDraw builds the category list
// from `chest != NULL` / `furnace != NULL` EVERY frame, so a container tab exists exactly
// while its state is being handed in. A chest closing takes tab 2 away mid-frame, and
// barNavClamp (scene/barnav.h) puts the cursor back on a real cell — which is what replaces
// the old "corrected back to the inventory screen on the spot" same-frame screen fix. Neither
// pointer is ever dereferenced while it is NULL, exactly as before.
//
// The gestures are unchanged, tab for screen: the bag and the chest keep the two-tap
// lift/place (a bag lift in `picked_slot`, a chest lift in `picked_chest`, never both), the
// furnace keeps its one-tap carry-and-resolve, and CRAFT keeps its one-tap row. What is new is
// that a d-pad/A/B cursor can reach every one of them — see UiInput.keys_down.
typedef enum {
	UI_SCR_HUD,
	UI_SCR_BAR,
} UiScreen;

// ── v1.9.0 CHEST-NET: who applies a chest transfer ──────────────────────────────────────
//
// Single player applies a chest transfer right here in ui.c: lift, place, move the stack, and
// main.c re-packs the edited ChestState into world/blockstate.c on the same frame. In a
// session that is the wrong thing to do — the chest's contents are the SERVER's (verified
// model, docs/design-1.9.0-chest-multiplayer.md), two players can be in one chest at once,
// and a client that moved the stack locally and then received the server's snapshot would
// either double the units or watch them vanish. So the multiplayer rule is: do not apply
// optimistically. Send the action, apply nothing, and let the BS_APP_CHEST_STATE /
// BS_APP_INV_STATE snapshots the server answers with rewrite both sides.
//
// This callback is that seam. When one is registered (uiSetChestTransferFn below), the chest
// panel calls it on the PLACE tap of every bag<->chest transfer and applies NOTHING itself,
// whatever it answers; when none is (NULL, the default and single player) the local apply
// runs. The arguments are the wire contract net/networld.h's networldSendChestAction() states,
// restated so the registrant can pass them straight through:
//
//   op       UI_CHEST_OP_DEPOSIT  (0x00) — bag -> chest.  a = the ITEM ID being deposited
//                                          (NOT an inventory slot), b = the chest slot tapped
//            UI_CHEST_OP_WITHDRAW (0x01) — chest -> bag.  a = the chest slot lifted,
//                                          b = the inventory slot tapped
//   x,y,z    the open chest's world position, as handed to uiOpenChest()
//   ud       the registrant's own pointer, passed back verbatim
//
//   count    how many units the transfer moves — the lifted stack's own count, passed in as a
//            parameter. Do NOT re-derive it from ui->picked_slot / ui->picked_chest: after a
//            v1.9.0 stack split the lift holds part of a stack, and the source slot holds the
//            units LEFT BEHIND, which is the wrong number.
//
// The callback runs BEFORE the lift is cleared, so ui->picked_* are still populated if a
// registrant needs anything else about the source; the count itself is already an argument.
//
// Return TRUE for "sent — the server owns it now": the lift is cleared and both sides are
// left exactly as they were, for the snapshot to rewrite. Return FALSE for "refused — not
// sent" (no session, a server without BS_CAP_CHESTS, or anything else): nothing changes AND
// the lift is kept, so the player is still holding what they lifted and can put it back.
// There is deliberately no "apply it locally after all" answer: a registrant that wants the
// local behaviour registers NULL. main.c does exactly that, per frame, off
// networldSessionActive().
//
// A callback for the LINK reason scene/interact.h's interactSetBagFitsFn is one: the send
// lives in source/net, which no host stanza linking scene/ui.c can pull in (net/networld.c's
// transport header is not host-portable — tests/net_stub.c says so), so a direct call here
// would break the very test that proves the transfer semantics (tests/ui_chest_test.c).
//
// Chest -> chest rearrangement (lift a chest slot, place on another chest slot) has no
// opcode on the wire, so with a callback registered it is refused outright — nothing moves
// and the lift is kept; without one it is applied locally like any other place.
#define UI_CHEST_OP_DEPOSIT  0x00
#define UI_CHEST_OP_WITHDRAW 0x01

// `count` is how many items the transfer asks to move: the whole lifted stack, since the lift
// is already the unit the player chose (a split happens before the lift, not during the place).
// It is the last byte of the wire frame — see networldSendChestAction in source/net/networld.h,
// which this callback exists to feed.
typedef bool (*UiChestTransferFn)(void* ud, uint8_t op, int x, int y, int z,
                                  uint8_t a, uint8_t b, uint8_t count);

typedef struct {
	UiScreen screen;
	bool     touch_prev;    // last frame's touch_down, so a tap fires once — the same trick
	                         // title.c's TitleState uses, for the same reason
	int      picked_slot;   // an inventory.h slot index currently "lifted" for a move,
	                         // mid pick-up/drop; -1 when nothing is lifted
	int      picked_chest;  // v1.9.0 CHEST: a chest slot (0..CHEST_SLOTS-1) currently lifted,
	                         // -1 when none. Never >= 0 at the same time as picked_slot: a
	                         // lift only starts when nothing is lifted on either side.
	int      chest_x, chest_y, chest_z;   // the open chest's world position, from uiOpenChest;
	                                       // meaningful only while a chest is being handed in
	UiChestTransferFn chest_fn;   // see the block above; NULL = apply locally
	void*             chest_ud;

	// v1.9.0 SPLIT. `gesture` is the edge state scene/ui_gesture.h keeps between frames (a
	// held X or Y is one event). `lift` is the DETACHED lift: after a Y split, the larger
	// half of picked_slot's stack lives here, not in the bag, until it is placed or returned;
	// lift.item == ITEM_NONE means picked_slot (if any) is the ordinary whole-stack lift by
	// index. `lift_from` is the slot it was split from — the slot net/inv_bridge.h reports the
	// units as moving OUT of on place — and is meaningful only while lift.item != ITEM_NONE.
	// uiUpdateDraw returns the lift to lift_from whenever picked_slot stops naming it (any
	// close, toggle, correction or uiOpen*), so nothing is ever left in the air across a
	// screen change. All-zero (uiInit's memset) is the right initial state.
	UiGesture gesture;
	InvSlot   lift;
	int       lift_from;

	// ── v1.9.1 BAR ─────────────────────────────────────────────────────────────────────
	//
	// `nav` is the cursor: which tab, which cell, and the two pieces of memory that make the
	// bar feel like it remembers where you were (scene/barnav.h). It is the ONLY copy — this
	// file does not keep a second "focused tab" int beside it, for the same reason
	// UiGesture takes `lifted_slot` in rather than keeping one. A memset-zero BarNav is cat 0,
	// row 0, col 0, which is the correct initial state, so uiInit's memset does the work.
	//
	// `list_scroll` is the first visible row of the CRAFT tab's recipe list, recomputed from
	// the cursor every frame by barNavScrollFor(). It is stored rather than derived at each
	// use so the draw pass and the hit test cannot disagree about which rows are on screen —
	// a stylus tap resolving against a different window than the one that was drawn is a tap
	// on the wrong recipe. At RECIPE_COUNT 7 and BAR_LIST_VISIBLE 7 it is always 0 today; the
	// field exists because the list is the one tab whose length is free to grow.
	//
	// `opened_this_frame` is set by uiOpenChest/uiOpenFurnace and by the HUD's own open, and
	// makes the first frame the bar draws ignore that frame's keys_down. Without it the button
	// that opened the bar is still a press edge when the bar first reads the pad: main.c opens
	// a container on ACTION_PLACE, which options.h allows to be bound to A, and A on the bar
	// is COMMIT — so opening a chest would immediately lift or drop on whatever cell the
	// cursor landed on. Cleared by uiUpdateDraw on the frame it is read.
	BarNav nav;
	int    list_scroll;
	bool   opened_this_frame;
} UiState;

// Zeroes `ui` and puts it on the HUD screen with nothing picked up. Call once, before the
// first uiUpdateDraw.
void uiInit(UiState* ui);

// v1.8.15 FURNACE. v1.9.1 BAR: opens the bar on its container tab (screen = UI_SCR_BAR,
// nav.cat = 2 — the index barBuildKinds always puts the container at) and drops any lifted
// stack. The NAME is unchanged on purpose: main.c's call site is "the player interacted with a
// furnace", which is still exactly what this means. This is what the
// caller calls when the player interacts with a BLOCK_FURNACE in the world; from the next
// uiUpdateDraw onward it must pass that furnace's unpacked FurnaceState as the `furnace`
// parameter, and keep passing it until UiResult.furnace_open comes back false.
//
// A one-line function rather than "just set ui->screen yourself", even though UiState is a
// plain struct the caller can write: this is the only screen with a PRECONDITION attached to
// it (a live FurnaceState must accompany it), and a named entry point is where that
// precondition can be stated once instead of in a comment the caller has to find. Setting
// ui->screen directly still works and is still safe — uiUpdateDraw's NULL guard catches the
// case where it was set without a furnace to go with it — it just does not read as a
// contract.
void uiOpenFurnace(UiState* ui);

// v1.9.0 CHEST. v1.9.1 BAR: opens the bar on its container tab (screen = UI_SCR_BAR,
// nav.cat = 2), records the chest's world position (x, y, z — what the transfer callback above
// is handed), and drops any lifted stack on EITHER side — same contract as uiOpenFurnace
// above, with `chest` standing in for `furnace`: from the next uiUpdateDraw onward the caller
// must pass that chest's unpacked ChestState as the `chest` parameter, and keep passing it
// until UiResult.chest_open comes back false. Calling it while a lift is pending, from any
// screen, cancels that lift first: the lifted stack stays exactly where it was.
void uiOpenChest(UiState* ui, int x, int y, int z);

// Registers (or with NULL, clears) the transfer callback described above UiState. Stored on
// the UiState — uiInit() zeroes it, so register AFTER uiInit. main.c re-registers every frame
// off networldSessionActive(): the callback while a session is up, NULL otherwise, which is
// what keeps single player on the local apply with no second code path.
void uiSetChestTransferFn(UiState* ui, UiChestTransferFn fn, void* ud);

// v1.9.1 DUPE FIX. The ONLY way a server BS_APP_INV_STATE snapshot may reach the inventory:
// discards any detached split half, then applies the snapshot via invBridgeApplyState.
//
// Why it has to exist at all. A Y split moves the larger half of a stack OUT of the bag into
// `ui->lift`, which is a field of UiState and NOT part of Inventory, and net/inv_bridge.h says
// plainly that a split is never reported to the server. So while a half is in the air the two
// copies disagree ON PURPOSE: the server still believes the whole undivided stack sits in the
// origin slot. invBridgeApplyState then overwrites every slot wholesale, which is exactly what
// it is for — but it has never known `ui->lift` existed, so the origin slot came back at its
// full pre-split count while the lifted half was STILL held separately. Those units were
// created out of nothing, and the gesture is repeatable, so it was an unbounded duplication.
//
// It is not exotic to reach. The bag screen is not the pause menu, so L/R hotbar cycling and
// the craft rows both still work with a half in the air, and the server answers EVERY accepted
// inventory action with a fresh snapshot (deps/blocksmith-server/game/bsgame.c's
// handle_inv_action ends in an unconditional send_inv_state). Split, nudge L, place: more items
// than you started with.
//
// DISCARDED, not returned. The incoming snapshot already accounts for the lifted units — they
// are part of the count the server is reporting for the origin slot, because it was never told
// they left it. Adding them back on top would be the same duplication by a different route.
// The player sees their half drop back into the stack it came from, which is what the server
// thinks happened anyway; nothing is lost.
//
// Single player never calls this: with no session no snapshot is ever produced, which is why
// the split gesture's local-only design was sound for the case it was written for.
void uiApplyInvSnapshot(UiState* ui, Inventory* inv, const NetworldInvState* state);

// One frame's touch point. See the file comment for why there is no keys_down here, unlike
// TitleInput.
//
//   touch_down  true while the stylus/finger is on the bottom screen this frame — main.c
//               already derives this from touchRead() for its own reasons (see title.h's
//               TitleInput for the identical note); this file only ever reads it, never
//               calls into libctru itself.
//   touch_x/y   bottom-screen pixels, the 320x240 space spriteBegin(320,240) draws in.
//               Meaningful only when touch_down is true.
//   keys_held   v1.9.0 SPLIT: hidKeysHeld(), the LEVEL word. scene/ui_gesture.h finds the
//               press edge itself, so a held X or Y is one gesture, and a button already down
//               when the overlay opens is none. Only KEY_X and KEY_Y are read.
//   keys_down   v1.9.1 BAR: hidKeysDown(), the PRESS-EDGE word, optionally OR-ed with
//               barStickEdge()'s answer so the circle pad drives the same paths as the d-pad
//               (main.c seam S1/S2). Read by scene/barnav.h's barNavInput for B/A/X/L/R and
//               the four d-pad bits, and by nothing else here. `blank` frames (main.c passes a
//               zeroed UiInput while the pause menu is up) leave it 0, which barNavInput
//               answers BAR_EV_NONE for — feeding it every frame is therefore correct.
//
//               Two words, not one, and this is the whole reason: keys_held is a LEVEL and
//               keys_down is an EDGE. Handing barNavInput a level word would step the cursor
//               once per frame for as long as the d-pad was pushed, and handing uiGestureFeed
//               an edge word would make a held Y fire on the frame it was pressed instead of
//               the frame the module decides — each module already documents which it needs.
typedef struct {
	bool     touch_down;
	int      touch_x, touch_y;
	uint32_t keys_held;
	uint32_t keys_down;
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
	// v1.9.1 BAR. True exactly when UI_SCR_BAR is what this frame drew — the rename of what
	// was `inventory_open`, because the bar is no longer "the inventory screen": the bag is
	// one of its tabs. This is the flag main.c gates world input on (blueprint seam S3): with
	// the bar up the player does not walk, jump, break, place, eat or look, and the world
	// keeps ticking regardless.
	bool bar_open;

	// v1.8.15 FURNACE / v1.9.0 CHEST, restated for the bar. True exactly when this frame drew
	// the bar WITH that container's state handed in — i.e. `furnace != NULL` (respectively
	// `chest != NULL`) and screen == UI_SCR_BAR. Read off the POINTER, not off nav.cat, and
	// that distinction is load-bearing: main.c closes its open container when the flag goes
	// false, so deriving it from the focused tab would slam a chest shut the moment the player
	// pressed L to look at the crafting list, and take its tab away underneath them.
	//
	// The two are MUTUALLY EXCLUSIVE, never both true, for the reason barBuildKinds states:
	// main.c opens one container per PLACE press. `bar_open` is true whenever either is.
	bool furnace_open;

	// v1.9.1 BAR adds the field v1.9.0 CHEST deliberately left out (that omission was a scope
	// cut to keep main.c's wiring frozen while it was being written in parallel; the note it
	// carried said adding the field was a one-line change whenever whoever owned the gate
	// wanted it). main.c's seam S6 is that gate: `if (!ures.chest_open) s_chest_open = false;`
	// replaces the old test against the retired chest-screen enum value, which no longer
	// compiles — which is the point: main.c cannot silently keep the stale wiring.
	bool chest_open;
} UiResult;

// One frame: advances `ui`, edits `inv` in place (slot moves, hotbar selection, crafting),
// draws the current screen onto whatever render target the caller bound, and reports what
// the caller might want to know afterwards.
//
// `block_icons` is the block atlas's C3D_Tex*, or NULL — see the file comment. `stats` may
// be NULL — see UiStats above.
//
// ── `furnace` (v1.8.15 FURNACE) ────────────────────────────────────────────────────────
//
// NULL whenever no furnace is open, and a pointer to the caller's UNPACKED live FurnaceState
// whenever one is. v1.9.1 BAR: this pointer is now what DECIDES whether the furnace tab exists
// at all — barBuildKinds is fed `furnace != NULL` every frame — so when it is NULL there is no
// furnace tab to focus, no furnace content is drawn, and this pointer is never dereferenced.
// See UiScreen above for the whole of the rule.
//
// When it is non-NULL, this function EDITS IT IN PLACE: moving a stack into the input or fuel
// slot writes input_item/input_count or fuel_item/fuel_count, and taking the output clears
// output_item/output_count. Nothing here touches fuel_ticks_left, cook_ticks or lit — those
// belong to furnaceTick() and are only ever read here, to draw the two indicators. The caller
// is expected to re-pack the state into world/blockstate.c's payload after the call; this
// file has no knowledge of blockstate.c or of a position, the same boundary world/furnace.h's
// own furnaceTick() comment sets out.
//
// ── `chest` (v1.9.0 CHEST) ─────────────────────────────────────────────────────────────
//
// NULL whenever no chest is open, and a pointer to the caller's UNPACKED live ChestState
// whenever one is — same NULL contract as `furnace` above, restated for CHEST_SLOTS
// (world/chest.h) slots instead of three named ones: when it is NULL there is no chest tab,
// no chest content is drawn, and this pointer is never dereferenced. A cursor left sitting on
// the container tab when it goes away is put back on a real cell by barNavClamp, which is the
// same-frame correction the two container screens used to get by falling back to the bag.
//
// When it is non-NULL, this function EDITS IT IN PLACE, by the two-tap gesture ui.c's file
// comment describes: the first tap on a non-empty slot on EITHER side lifts its whole stack
// (`picked_slot` for a bag slot, `picked_chest` for a chest slot); the second tap places it on
// whatever is under the finger — the same slot again cancels, an empty or same-item slot
// merges up to INV_STACK_MAX and leaves any remainder in the source, a slot holding a
// different item swaps the two stacks. Bag -> bag on this screen is the overlay's own
// handleSlotTap; bag -> chest, chest -> bag and chest -> chest are ui.c's placeStack, the
// same three rules against a destination that is not an inventory slot. Every chest slot
// accepts every item (world/chest.h's own file comment), so unlike `furnace` there is no
// per-slot acceptance rule and no "this would be refused" cue to draw. The caller re-packs
// the state into world/blockstate.c's payload after the call, the same boundary `furnace`
// already sets out; this file knows the chest's position only to hand it to the callback.
//
// v1.9.0 CHEST-NET: with a UiChestTransferFn registered (uiSetChestTransferFn above), NONE
// of the editing this paragraph describes happens for a bag<->chest place — the callback is
// called instead and both `inv` and `chest` are left exactly as they were, so the caller's
// re-pack writes back an unchanged payload. See the callback's own doc for the contract.
UiResult uiUpdateDraw(UiState* ui, Inventory* inv, C3D_Tex* block_icons,
                       const UiStats* stats, const UiInput* in, FurnaceState* furnace,
                       ChestState* chest);
