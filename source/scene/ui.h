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

#include "scene/ui_gesture.h"  // UiGesture — v1.9.0 SPLIT, see UiState.gesture
#include "world/chest.h"       // ChestState — see uiUpdateDraw's `chest` parameter
#include "world/furnace.h"     // FurnaceState — see uiUpdateDraw's `furnace` parameter
#include "world/inventory.h"

// Which of the three screens this frame is drawing. UI_SCR_HUD is the default: just the
// hotbar (always visible, per inventory.h's own INV_HOTBAR_SLOTS comment) plus the engine
// status text main.c already computes. UI_SCR_INVENTORY adds the main grid and the crafting
// list, covering the rest of the screen — see ui.c's layout block for the exact pixels.
//
// v1.8.15 FURNACE. UI_SCR_FURNACE is an open furnace: its input, fuel and output slots, a
// burn-time readout, a cook-progress readout, the hotbar (still, always) and the main grid
// moved down to make room — see scene/ui_layout.h's furnace block for the pixels and for why
// the grid moves on this screen only.
//
// ⚠ UI_SCR_FURNACE is only ever entered or drawn while uiUpdateDraw's `furnace` parameter is
// non-NULL. A UiState left on this screen with no furnace handed in is not an error the
// caller has to avoid — uiUpdateDraw corrects it to UI_SCR_INVENTORY on the spot, drops any
// lifted stack, and draws the inventory overlay instead. That is the ONE place in this file
// where a screen change takes effect on the same frame rather than the next one, and it is
// deliberate: the alternative is a frame that draws a furnace panel out of a pointer it has
// been told not to dereference.
//
// v1.9.0 CHEST. UI_SCR_CHEST is an open chest: its eight slots, the hotbar (still, always)
// and the main grid moved down exactly the way the furnace screen moves it — see
// scene/ui_layout.h's chest panel block for the pixels. It follows the identical NULL-guard
// rule stated above for UI_SCR_FURNACE, with `chest` standing in for `furnace`: entered or
// drawn only while uiUpdateDraw's `chest` parameter is non-NULL, corrected to
// UI_SCR_INVENTORY on the spot otherwise. Its gesture is the SAME two-tap lift/place the
// inventory overlay uses (see ui.c's file comment), extended across the two containers: a
// lifted stack lives in `picked_slot` when it came from the bag and in `picked_chest` when
// it came from the chest, and both are cleared by the close bar, by uiOpenChest/uiOpenFurnace,
// and by the NULL-guard correction above, so nothing is ever left "in the air".
typedef enum {
	UI_SCR_HUD,
	UI_SCR_INVENTORY,
	UI_SCR_FURNACE,
	UI_SCR_CHEST,
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
	                                       // meaningful only while screen == UI_SCR_CHEST
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
} UiState;

// Zeroes `ui` and puts it on the HUD screen with nothing picked up. Call once, before the
// first uiUpdateDraw.
void uiInit(UiState* ui);

// v1.8.15 FURNACE. Switches to UI_SCR_FURNACE and drops any lifted stack. This is what the
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

// v1.9.0 CHEST. Switches to UI_SCR_CHEST, records the chest's world position (x, y, z — what
// the transfer callback above is handed), and drops any lifted stack on EITHER side — same
// contract as uiOpenFurnace above, with `chest` standing in for `furnace`: from the next
// uiUpdateDraw onward the caller must pass that chest's unpacked ChestState as the `chest`
// parameter, and keep passing it until ui->screen has left UI_SCR_CHEST (UiResult has no
// chest_open field — see UiResult below). Calling it while a lift is pending, from any
// screen, cancels that lift first: the lifted stack stays exactly where it was.
void uiOpenChest(UiState* ui, int x, int y, int z);

// Registers (or with NULL, clears) the transfer callback described above UiState. Stored on
// the UiState — uiInit() zeroes it, so register AFTER uiInit. main.c re-registers every frame
// off networldSessionActive(): the callback while a session is up, NULL otherwise, which is
// what keeps single player on the local apply with no second code path.
void uiSetChestTransferFn(UiState* ui, UiChestTransferFn fn, void* ud);

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
typedef struct {
	bool     touch_down;
	int      touch_x, touch_y;
	uint32_t keys_held;
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

	// v1.8.15 FURNACE. True exactly when UI_SCR_FURNACE is what this frame drew — same
	// "reflects the screen this frame actually drew" rule as inventory_open above, including
	// the frame a tap opens or closes the panel on.
	//
	// The two are MUTUALLY EXCLUSIVE, never both true: they name two different screens, and
	// a frame draws one screen. A caller gating world interaction on "is the player visibly
	// managing storage" therefore wants `inventory_open || furnace_open`, not either alone —
	// stated here because inventory_open's own comment describes exactly that use and was
	// written when it was the only answer to it.
	bool furnace_open;

	// v1.9.0 CHEST does NOT add a `chest_open` here, even though UI_SCR_CHEST is a third
	// screen with exactly the same "is the player visibly managing storage" question the
	// comment above already answers for the other two. That omission is deliberate scope,
	// not an oversight: this struct's shape was fixed as part of the task contract this
	// change landed under, specifically so main.c's wiring — done in parallel, against this
	// exact signature — would not need to change; main.c closes its chest off ui->screen
	// instead. The practical gap it leaves: a caller gating on `inventory_open ||
	// furnace_open` reads a frame where UI_SCR_CHEST is what actually drew as neither open,
	// which is wrong for that gate. Flagged rather than fixed; adding the field is a
	// one-line change whenever whoever owns that gate wants it.
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
// whenever one is. When it is NULL, UI_SCR_FURNACE is never entered and never drawn and this
// pointer is never dereferenced — see UiScreen above for what happens if the screen is
// somehow set anyway.
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
// (world/chest.h) slots instead of three named ones: when it is NULL, UI_SCR_CHEST is
// never entered and never drawn and this pointer is never dereferenced (see UiScreen
// above), and a UiState left on UI_SCR_CHEST with no chest handed in is corrected to
// UI_SCR_INVENTORY on the spot, the identical same-frame correction furnace gets.
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
