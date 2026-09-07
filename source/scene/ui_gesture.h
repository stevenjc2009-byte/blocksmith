// v1.9.0 SPLIT. The inventory overlay's gestures — lift, place, merge, split and quick-move —
// classified out of scene/ui.c so the classification can be host-tested, the same split, for
// the same reason, as scene/ui_layout.h (rect arithmetic out of ui.c) and scene/title_nav.h
// (a navigation rule out of title.c). ui.c draws with gfx/sprite.h and includes <citro3d.h>,
// so nothing in it can be linked outside a devkitARM build; this pair needs nothing from it —
// one input struct a frame and a read-only look at the Inventory.
//
// ── What it decides, and what it does not ──────────────────────────────────────────────
//
// This module turns "the stylus went down on slot 5 while slot 2 was lifted and slot 5 holds
// the same item" into UI_GESTURE_MERGE { src 2, dst 5 }. It never moves a unit: every
// mutation is scene/ui.c's, through net/inv_bridge.h, and the event is only a name for what
// the player did. That is what makes it testable without a GPU, and it is also what keeps
// the merge/cap/refuse rules in the ONE place that already owns them (world/inventory.h and
// the bridge) rather than duplicated in a classifier that would then have to agree with them.
//
// It also owns no lift. `lifted_slot` comes IN every frame (scene/ui.h's UiState.picked_slot),
// because ui.c clears that field on every screen change and a second copy in here would be
// one screen switch away from disagreeing with it.
//
// ── The gestures ───────────────────────────────────────────────────────────────────────
//
// Taps are the two-tap model ui.c has always had, on the PRESS edge exactly as before —
// nothing about a plain lift or place is re-timed by this module:
//
//   tap a non-empty slot, nothing lifted         LIFT       { src = that slot }
//   tap the lifted slot again                    PLACE      { src = dst = that slot }  (cancel)
//   tap a slot holding the SAME item as the lift MERGE      { src = lifted, dst = tapped }
//   tap any other slot (empty or different item) PLACE      { src = lifted, dst = tapped }
//
// Keys are libctru's face buttons, taken as a HELD word and edge-detected in here (see
// UiGestureInput.keys_held), so a button held across several frames is one event, and a
// button already down when the overlay opens is no event at all:
//
//   Y                                            SPLIT      { src = the lifted slot, or the
//                                                             slot under the stylus if none
//                                                             is lifted }   count >= 2 only
//   X                                            QUICK_MOVE { src = the slot under the stylus,
//                                                             or the lifted slot if the stylus
//                                                             is not on one; to_chest = the
//                                                             chest panel is up }
//
// Why keys and not a long-press: the first cut of this file was a hold-vs-tap timer, chosen
// because scene/ui.h's UiInput carried a touch point and nothing else and a hold needed no new
// field. Quick-move was then specified on X. Once one gesture needs the keys plumbed through
// UiInput, the second one costs nothing to put on a key too, and a key beats a hold on the two
// things a hold is bad at: it is instant (no 20-frame wait, no "did that register yet"), and it
// cannot be triggered by a stylus that skids or a pressure that wavers — both real on a
// resistive panel, and both things the timer version had to grow a cancel rule for. Taps stay
// on the press edge, so nothing that shipped is re-timed.
//
// Why Y targets the LIFTED slot first: on a 3DS the stylus is in the right hand and so are X
// and Y. Tap to lift (the slot highlights), lift the stylus, thumb the button — that works
// one-handed. "Stylus on the slot AND Y" is still accepted, for a player who wants the
// Minecraft right-click feel, but it is the fallback, not the primary.
//
// Why X targets the STYLUS slot first: quick-move is a fire-and-forget on one slot with no
// second step, so "point at it, press X" is the natural shape and needs no lift at all. The
// lifted slot is the fallback so the one-handed tap-then-thumb flow works here too.
//
// One event a frame, taps before keys. A stylus press and a button press on the same 16 ms
// frame is not a gesture anyone makes on purpose; the tap wins and the key is dropped, not
// queued, so it cannot fire on a later frame against a slot the player has since moved off.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/inventory.h"

// libctru's KEY_A / KEY_B / KEY_X / KEY_Y, restated so this file compiles on the host. The
// same choice app/options.h makes for OPT_KEY_*, and pinned to the real bits the same way:
// main.c carries a _Static_assert of each against <3ds.h>. Only X and Y are read here.
#define UI_GESTURE_KEY_A 0x00000001u
#define UI_GESTURE_KEY_B 0x00000002u
#define UI_GESTURE_KEY_X 0x00000400u
#define UI_GESTURE_KEY_Y 0x00000800u

typedef enum {
	UI_GESTURE_NONE = 0,
	UI_GESTURE_LIFT,        // src = the slot to lift
	UI_GESTURE_PLACE,       // src = the lifted slot, dst = where it goes (dst == src: cancel)
	UI_GESTURE_SPLIT,       // src = the slot to split
	UI_GESTURE_MERGE,       // src = the lifted slot, dst = a slot holding the same item
	UI_GESTURE_QUICK_MOVE,  // src = the slot to send to the other container
} UiGestureKind;

// Never written to the wire or a file, so -fshort-enums (the console packs UiGestureKind into
// one byte, the host into four) changes nothing a caller can observe.
typedef struct {
	UiGestureKind kind;
	int           src;        // -1 when kind is UI_GESTURE_NONE
	int           dst;        // meaningful for PLACE and MERGE only; -1 otherwise
	bool          to_chest;   // QUICK_MOVE only: into the open chest, not the other strip
} UiGestureEvent;

// One frame's inputs.
//
//   touch_down    UiInput.touch_down. Edge-detected in here: a tap is the frame this goes true.
//   slot_under    the bag slot under the stylus THIS frame — scene/ui_layout.h's hit test for
//                 whichever screen is up (hitInventorySlot(x, y, true) on the overlay,
//                 hitChestInvSlot / hitFurnaceInvSlot on the storage screens) — or -1. Ignored
//                 when touch_down is false. -1 on the HUD screen, where a hotbar tap means
//                 "select", never "lift".
//   keys_held     hidKeysHeld(), the LEVEL word, not hidKeysDown(): the press edge is found in
//                 here against last frame's word, so feeding the level is what makes a held
//                 button one event. Feeding hidKeysDown() also works (an edge is its own edge)
//                 but is not what the contract asks for.
//   lifted_slot   UiState.picked_slot: the slot currently lifted, or -1.
//   lift_enabled  a bag-slot tap means lift/place on this screen. True on the inventory
//                 overlay and the furnace screen, and on the chest screen while no CHEST slot
//                 is lifted (with one lifted, a bag-slot tap is scene/ui.c's withdraw, which
//                 the tap chain there handles and this module must not turn into a lift).
//                 False on the HUD. With it false, taps produce nothing; X still works,
//                 because a quick-move needs no lift.
//   split_enabled Y may split here. True on the inventory overlay ONLY: a split leaves half
//                 the stack in the air (scene/ui.h's UiState.lift), and the furnace and chest
//                 deposit paths move the WHOLE lifted slot, so a half in the air on those
//                 screens would deposit the wrong amount. Never true with lift_enabled false.
//   chest_open    the chest panel is up: X sends the stack INTO THE CHEST rather than to the
//                 other strip.
typedef struct {
	bool     touch_down;
	int      slot_under;
	uint32_t keys_held;
	int      lifted_slot;
	bool     lift_enabled;
	bool     split_enabled;
	bool     chest_open;
} UiGestureInput;

// The edge state. Plain struct so scene/ui.h's UiState can embed one; nothing in it is
// meaningful to a caller. All-zero is the right initial state (uiInit memsets UiState).
typedef struct {
	bool     touch_prev;
	uint32_t keys_prev;
} UiGesture;

// Back to the initial state. Only needed by a caller that did not zero the struct.
void uiGestureReset(UiGesture* g);

// One frame. MUST be called every frame the bottom screen is drawn, including on the HUD
// screen with slot_under -1 and lift_enabled false: the key edge state has to track the real
// buttons continuously, or a button held while the overlay opens reads as a fresh press on
// the overlay's first frame. `inv` is read for slot contents only (is it empty; is it the
// same item as the lift; is there a half to split) and never written.
UiGestureEvent uiGestureFeed(UiGesture* g, const UiGestureInput* in, const Inventory* inv);
