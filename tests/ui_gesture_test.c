// Host gate for scene/ui_gesture.c — the inventory overlay's gesture classifier
// (v1.9.0 SPLIT: lift / place / merge / split / quick-move).
//
// WHY THIS FILE LINKS THE REAL MODULE. scene/ui.c cannot be linked outside devkitARM (it
// draws), so the part of it that can be WRONG in a way a playtest would not show — which
// event a frame of touch + button state means, and whether a held button fires once or every
// frame — was moved into scene/ui_gesture.c, which includes nothing the host cannot compile.
// This binary links THAT module and nothing else: the Inventory it reads is filled by hand,
// so world/inventory.c and the registry behind it are not in the link.
//
// WHAT THIS FILE CANNOT PROVE, stated up front: that scene/ui.c feeds uiGestureFeed() every
// frame (the header's contract), that main.c passes hidKeysHeld() through UiInput, that X and
// Y are masked out of the world-edit path while the overlay is up, or that a real stylus on
// a real panel produces the touch_down sequence scripted here. Those are the console's.
//
// Own main(), same CHECK macro and PASS/FAIL line as tests/aimtext_test.c.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene/ui_gesture.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			if (!s_fails) snprintf(s_first, sizeof s_first, "L%d %s", __LINE__, #cond); \
			s_fails++;                                                             \
		}                                                                          \
	} while (0)

// Pinned, so a check that quietly stops being reached reads as a FAIL rather than as a
// shorter green run. Move it only when a check is deliberately added or removed, and say so.
#define UI_GESTURE_EXPECTED_CHECKS 103

// Any two distinct carryable ids; the module compares them, it never looks them up.
#define STONE ((ItemId)1)
#define DIRT  ((ItemId)2)

// The bag every scenario reads. Slot contents chosen so each rule has a slot to hit:
//   0  STONE x10   a stack with a half           4  STONE x1   no half
//   1  STONE x5    same item as 0                8  STONE x20  same item, in the main strip
//   2  DIRT  x3    a different item              3, everything else: empty
static Inventory s_inv;

static void bag(void)
{
	memset(&s_inv, 0, sizeof s_inv);
	s_inv.slots[0].item = STONE; s_inv.slots[0].count = 10;
	s_inv.slots[1].item = STONE; s_inv.slots[1].count = 5;
	s_inv.slots[2].item = DIRT;  s_inv.slots[2].count = 3;
	s_inv.slots[4].item = STONE; s_inv.slots[4].count = 1;
	s_inv.slots[8].item = STONE; s_inv.slots[8].count = 20;
}

// One frame, spelled out. `lift_on` is lift_enabled, `split_on` is split_enabled.
static UiGestureEvent frame(UiGesture* g, bool touch, int under, uint32_t keys, int lifted,
                            bool lift_on, bool split_on, bool chest)
{
	UiGestureInput in;
	in.touch_down    = touch;
	in.slot_under    = under;
	in.keys_held     = keys;
	in.lifted_slot   = lifted;
	in.lift_enabled  = lift_on;
	in.split_enabled = split_on;
	in.chest_open    = chest;
	return uiGestureFeed(g, &in, &s_inv);
}

// The overlay screen: lift on, split on, no chest.
static UiGestureEvent overlay(UiGesture* g, bool touch, int under, uint32_t keys, int lifted)
{
	return frame(g, touch, under, keys, lifted, true, true, false);
}

static bool is(UiGestureEvent e, UiGestureKind k, int src, int dst)
{
	return e.kind == k && e.src == src && e.dst == dst;
}

static bool none(UiGestureEvent e)
{
	return e.kind == UI_GESTURE_NONE && e.src == -1 && e.dst == -1 && !e.to_chest;
}

static void test_idle_and_reset(void)
{
	UiGesture g;
	memset(&g, 0, sizeof g);
	CHECK(none(overlay(&g, false, -1, 0, -1)));            // an idle frame is nothing
	CHECK(none(overlay(&g, false, 5, 0, -1)));             // a slot with no touch is nothing

	// Reset re-arms the edges: the same "already down" state reads as fresh afterwards.
	(void)overlay(&g, true, 0, UI_GESTURE_KEY_Y, -1);      // LIFT, consumes both edges
	CHECK(none(overlay(&g, true, 0, UI_GESTURE_KEY_Y, 0)));   // still down: nothing
	uiGestureReset(&g);
	CHECK(is(overlay(&g, true, 0, UI_GESTURE_KEY_Y, 0), UI_GESTURE_PLACE, 0, 0));   // fresh tap again
}

static void test_lift_fires_on_the_press_edge_only(void)
{
	UiGesture g = { 0 };
	UiGestureEvent e = overlay(&g, true, 0, 0, -1);
	CHECK(is(e, UI_GESTURE_LIFT, 0, -1));
	CHECK(!e.to_chest);

	// Held for five frames on the same slot: no second event.
	for (int i = 0; i < 5; i++) CHECK(none(overlay(&g, true, 0, 0, 0)));
	// Sliding to another slot while still down is not a tap either.
	CHECK(none(overlay(&g, true, 1, 0, 0)));
	// Release: nothing. Press again: a fresh tap.
	CHECK(none(overlay(&g, false, -1, 0, 0)));
	CHECK(is(overlay(&g, true, 0, 0, 0), UI_GESTURE_PLACE, 0, 0));
}

static void test_tap_on_an_empty_slot_lifts_nothing(void)
{
	UiGesture g = { 0 };
	CHECK(none(overlay(&g, true, 3, 0, -1)));
	CHECK(none(overlay(&g, false, -1, 0, -1)));
	CHECK(none(overlay(&g, true, 15, 0, -1)));
}

static void test_place_and_cancel(void)
{
	UiGesture g = { 0 };
	CHECK(is(overlay(&g, true, 0, 0, 0), UI_GESTURE_PLACE, 0, 0));    // tap the lifted slot: cancel
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 3, 0, 0), UI_GESTURE_PLACE, 0, 3));    // onto an empty slot
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 2, 0, 0), UI_GESTURE_PLACE, 0, 2));    // onto a different item: still PLACE (ui.c swaps)
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 4, 0, 2), UI_GESTURE_PLACE, 2, 4));    // dirt lifted onto stone: PLACE, not MERGE
}

static void test_merge_when_the_tapped_slot_holds_the_same_item(void)
{
	UiGesture g = { 0 };
	CHECK(is(overlay(&g, true, 1, 0, 0), UI_GESTURE_MERGE, 0, 1));
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 8, 0, 0), UI_GESTURE_MERGE, 0, 8));    // across the strips too
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 0, 0, 8), UI_GESTURE_MERGE, 8, 0));    // and back the other way
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, true, 4, 0, 0), UI_GESTURE_MERGE, 0, 4));    // a stack of 1 is still a same-item stack
}

static void test_split_on_y(void)
{
	UiGesture g = { 0 };
	const uint32_t Y = UI_GESTURE_KEY_Y;

	// Lifted slot, no stylus: the primary flow.
	UiGestureEvent e = overlay(&g, false, -1, Y, 0);
	CHECK(is(e, UI_GESTURE_SPLIT, 0, -1));
	CHECK(!e.to_chest);
	// Held: once only.
	for (int i = 0; i < 5; i++) CHECK(none(overlay(&g, false, -1, Y, 0)));
	// Release, press again: fires again.
	CHECK(none(overlay(&g, false, -1, 0, 0)));
	CHECK(is(overlay(&g, false, -1, Y, 0), UI_GESTURE_SPLIT, 0, -1));
	(void)overlay(&g, false, -1, 0, 0);

	// Nothing lifted, stylus on a slot: the fallback.
	CHECK(is(overlay(&g, true, 1, Y, -1), UI_GESTURE_SPLIT, 1, -1) == false);   // same frame as the tap: tap wins (LIFT)
	(void)overlay(&g, false, -1, 0, -1);
	(void)overlay(&g, true, 3, 0, -1);                                          // stylus down on an EMPTY slot (no lift)
	CHECK(none(overlay(&g, true, 3, Y, -1)));                                   // Y over an empty slot: nothing
	(void)overlay(&g, false, -1, 0, -1);
	(void)overlay(&g, true, 4, 0, -1);                                          // LIFT of slot 4 would happen in ui.c; script no lift
	CHECK(none(overlay(&g, true, 4, Y, -1)));                                   // a stack of 1 has no half
	(void)overlay(&g, false, -1, 0, -1);

	// The fallback proper: stylus already resting on slot 1 (its tap was a frame ago), then Y.
	(void)overlay(&g, true, 1, 0, -1);
	CHECK(is(overlay(&g, true, 1, Y, -1), UI_GESTURE_SPLIT, 1, -1));
	(void)overlay(&g, false, -1, 0, -1);

	// Lifted slot wins over the stylus slot.
	(void)overlay(&g, true, 1, 0, 0);
	CHECK(is(overlay(&g, true, 1, Y, 0), UI_GESTURE_SPLIT, 0, -1));
	(void)overlay(&g, false, -1, 0, 0);

	// Nothing lifted, no stylus: nothing to split.
	CHECK(none(overlay(&g, false, -1, Y, -1)));
	(void)overlay(&g, false, -1, 0, -1);

	// A lifted slot that has been emptied under us (a snapshot): nothing.
	s_inv.slots[0].item = ITEM_NONE; s_inv.slots[0].count = 0;
	CHECK(none(overlay(&g, false, -1, Y, 0)));
	bag();
}

static void test_quick_move_on_x(void)
{
	UiGesture g = { 0 };
	const uint32_t X = UI_GESTURE_KEY_X;

	// Stylus resting on a slot, nothing lifted.
	(void)overlay(&g, true, 1, 0, -1);
	UiGestureEvent e = overlay(&g, true, 1, X, -1);
	CHECK(is(e, UI_GESTURE_QUICK_MOVE, 1, -1));
	CHECK(!e.to_chest);
	for (int i = 0; i < 5; i++) CHECK(none(overlay(&g, true, 1, X, -1)));    // held: once
	CHECK(none(overlay(&g, true, 1, 0, -1)));                                // released
	CHECK(is(overlay(&g, true, 1, X, -1), UI_GESTURE_QUICK_MOVE, 1, -1));   // pressed again
	(void)overlay(&g, false, -1, 0, -1);

	// Lifted slot, no stylus.
	CHECK(is(overlay(&g, false, -1, X, 0), UI_GESTURE_QUICK_MOVE, 0, -1));
	(void)overlay(&g, false, -1, 0, 0);

	// Both: the stylus slot wins.
	(void)overlay(&g, true, 2, 0, 0);
	CHECK(is(overlay(&g, true, 2, X, 0), UI_GESTURE_QUICK_MOVE, 2, -1));
	(void)overlay(&g, false, -1, 0, 0);

	// Stylus on an empty slot, nothing lifted: nothing to move.
	(void)overlay(&g, true, 3, 0, -1);
	CHECK(none(overlay(&g, true, 3, X, -1)));
	(void)overlay(&g, false, -1, 0, -1);

	// Stylus on an empty slot but something lifted: the stylus slot is empty, so it is not a
	// target — and the lift is NOT used as a fallback either, because the player pointed at a
	// slot. (Documented: `under >= 0` is the stylus on a REAL slot; empty is real.)
	(void)overlay(&g, true, 3, 0, 0);
	CHECK(none(overlay(&g, true, 3, X, 0)));
	(void)overlay(&g, false, -1, 0, 0);

	// Nothing at all: nothing.
	CHECK(none(overlay(&g, false, -1, X, -1)));
}

static void test_chest_screen_gates(void)
{
	UiGesture g = { 0 };
	const uint32_t X = UI_GESTURE_KEY_X;
	const uint32_t Y = UI_GESTURE_KEY_Y;

	// Chest screen, no chest slot lifted: lift on, split OFF, chest open. Bag taps lift as on
	// the overlay.
	CHECK(is(frame(&g, true, 0, 0, -1, true, false, true), UI_GESTURE_LIFT, 0, -1));
	// X on it: a quick-move INTO THE CHEST.
	UiGestureEvent e = frame(&g, true, 0, X, 0, true, false, true);
	CHECK(e.kind == UI_GESTURE_QUICK_MOVE && e.src == 0 && e.to_chest);
	CHECK(none(frame(&g, true, 0, X, 0, true, false, true)));        // held: once
	(void)frame(&g, false, -1, 0, 0, true, false, true);
	// Y with slot 0 lifted: no split on this screen.
	CHECK(none(frame(&g, false, -1, Y, 0, true, false, true)));
	(void)frame(&g, false, -1, 0, 0, true, false, true);

	// Chest screen with a CHEST slot lifted (lift off): a bag-slot tap is ui.c's withdraw,
	// not a lift — but X still quick-moves.
	CHECK(none(frame(&g, true, 0, 0, -1, false, false, true)));
	e = frame(&g, true, 0, X, -1, false, false, true);
	CHECK(e.kind == UI_GESTURE_QUICK_MOVE && e.src == 0 && e.to_chest);
	(void)frame(&g, false, -1, 0, -1, false, false, true);

	// Overlay with the chest CLOSED: to_chest is false.
	(void)overlay(&g, true, 0, 0, -1);
	e = overlay(&g, true, 0, X, -1);
	CHECK(e.kind == UI_GESTURE_QUICK_MOVE && e.src == 0 && !e.to_chest);
	(void)overlay(&g, false, -1, 0, -1);

	// Furnace screen: lift on, split off. A tap lifts; Y with that lift does nothing.
	CHECK(is(frame(&g, true, 1, 0, -1, true, false, false), UI_GESTURE_LIFT, 1, -1));
	CHECK(none(frame(&g, true, 1, Y, 1, true, false, false)));
	(void)frame(&g, false, -1, 0, 1, true, false, false);

	// HUD: everything off, no slot ever. Taps and Y do nothing; X with nothing under the
	// stylus and nothing lifted does nothing.
	CHECK(none(frame(&g, true, -1, 0, -1, false, false, false)));
	CHECK(none(frame(&g, true, -1, Y, -1, false, false, false)));
	(void)frame(&g, false, -1, 0, -1, false, false, false);
	CHECK(none(frame(&g, false, -1, X, -1, false, false, false)));
}

static void test_a_key_held_across_the_overlay_opening_is_not_a_press(void)
{
	UiGesture g = { 0 };
	const uint32_t Y = UI_GESTURE_KEY_Y;
	const uint32_t X = UI_GESTURE_KEY_X;

	// Three HUD frames with Y held (the player is placing a block), fed as the contract says.
	for (int i = 0; i < 3; i++) CHECK(none(frame(&g, false, -1, Y, -1, false, false, false)));
	// The overlay opens with Y still down and slot 0 lifted: not a split.
	CHECK(none(overlay(&g, false, -1, Y, 0)));
	CHECK(none(overlay(&g, false, -1, Y, 0)));
	// Release and press: now it is.
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, false, -1, Y, 0), UI_GESTURE_SPLIT, 0, -1));

	// Same for X: held through the HUD, still held when the overlay opens over a lifted slot.
	(void)overlay(&g, false, -1, 0, 0);
	(void)frame(&g, false, -1, X, -1, false, false, false);
	CHECK(none(overlay(&g, false, -1, X, 0)));
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(is(overlay(&g, false, -1, X, 0), UI_GESTURE_QUICK_MOVE, 0, -1));

	// And a different button pressed while Y is held is not a Y press either.
	(void)overlay(&g, false, -1, Y, 0);      // consumed (SPLIT)
	CHECK(none(overlay(&g, false, -1, Y | UI_GESTURE_KEY_A, 0)));
}

static void test_a_touch_already_down_when_the_overlay_opens_is_not_a_tap(void)
{
	UiGesture g = { 0 };
	// Stylus went down on the HUD (a hotbar select there, not our business), stays down.
	CHECK(none(frame(&g, true, -1, 0, -1, false, false, false)));
	// Overlay opens; the same touch is now over slot 0. Not a tap.
	CHECK(none(overlay(&g, true, 0, 0, -1)));
	// Lift and re-press: a tap.
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(is(overlay(&g, true, 0, 0, -1), UI_GESTURE_LIFT, 0, -1));
}

static void test_tap_and_key_on_one_frame_the_tap_wins_and_the_key_is_dropped(void)
{
	UiGesture g = { 0 };
	const uint32_t Y = UI_GESTURE_KEY_Y;
	const uint32_t X = UI_GESTURE_KEY_X;

	CHECK(is(overlay(&g, true, 1, Y, -1), UI_GESTURE_LIFT, 1, -1));
	CHECK(none(overlay(&g, true, 1, Y, 1)));       // the Y press was consumed, not queued
	(void)overlay(&g, false, -1, 0, 1);

	CHECK(is(overlay(&g, true, 0, X, 1), UI_GESTURE_MERGE, 1, 0));
	CHECK(none(overlay(&g, true, 0, X, 1)));
	(void)overlay(&g, false, -1, 0, 1);

	// Y and X on one frame with no tap: Y is checked first, X is dropped.
	CHECK(is(overlay(&g, false, -1, X | Y, 0), UI_GESTURE_SPLIT, 0, -1));
	CHECK(none(overlay(&g, false, -1, X | Y, 0)));

	// A tap that means nothing (empty slot) still swallows the key on that frame.
	(void)overlay(&g, false, -1, 0, 0);
	CHECK(none(overlay(&g, true, 3, Y, 0)) == false);   // empty slot, lift 0 -> PLACE 0->3, a real event
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(none(overlay(&g, true, 3, Y, -1)));           // empty slot, nothing lifted: NONE, and Y dropped too
	CHECK(none(overlay(&g, true, 3, Y, -1)));
}

static void test_bad_inputs_are_nothing(void)
{
	UiGesture g = { 0 };
	CHECK(none(overlay(&g, true, INV_SLOT_COUNT, 0, -1)));
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(none(overlay(&g, true, 99, 0, -1)));
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(none(overlay(&g, true, -5, 0, -1)));
	(void)overlay(&g, false, -1, 0, -1);
	// A slot_under with no touch is ignored, whatever it says.
	CHECK(none(overlay(&g, false, 0, 0, -1)));
	// A lifted index out of range reads as "nothing lifted".
	CHECK(is(overlay(&g, true, 0, 0, INV_SLOT_COUNT), UI_GESTURE_LIFT, 0, -1));
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(is(overlay(&g, true, 0, 0, -7), UI_GESTURE_LIFT, 0, -1));
	(void)overlay(&g, false, -1, 0, -1);
	// Y with a lifted index out of range and no stylus: nothing (and no read past the array).
	CHECK(none(overlay(&g, false, -1, UI_GESTURE_KEY_Y, INV_SLOT_COUNT)));
	(void)overlay(&g, false, -1, 0, -1);
	CHECK(none(overlay(&g, false, -1, UI_GESTURE_KEY_X, 200)));

	UiGestureInput in = { true, 0, 0, -1, true, true, false };
	CHECK(none(uiGestureFeed(NULL, &in, &s_inv)));
	CHECK(none(uiGestureFeed(&g, NULL, &s_inv)));
	CHECK(none(uiGestureFeed(&g, &in, NULL)));
}

static void test_key_constants_are_libctru_bits(void)
{
	CHECK(UI_GESTURE_KEY_A == 0x1u);
	CHECK(UI_GESTURE_KEY_B == 0x2u);
	CHECK(UI_GESTURE_KEY_X == 0x400u);
	CHECK(UI_GESTURE_KEY_Y == 0x800u);
}

int main(void)
{
	bag();
	test_idle_and_reset();
	test_lift_fires_on_the_press_edge_only();
	test_tap_on_an_empty_slot_lifts_nothing();
	test_place_and_cancel();
	test_merge_when_the_tapped_slot_holds_the_same_item();
	test_split_on_y();
	test_quick_move_on_x();
	test_chest_screen_gates();
	test_a_key_held_across_the_overlay_opening_is_not_a_press();
	test_a_touch_already_down_when_the_overlay_opens_is_not_a_tap();
	test_tap_and_key_on_one_frame_the_tap_wins_and_the_key_is_dropped();
	test_bad_inputs_are_nothing();
	test_key_constants_are_libctru_bits();

	if (s_checks != UI_GESTURE_EXPECTED_CHECKS) {
		printf("FAIL tests/ui_gesture_test.c: %d checks ran, %d expected (pinned)\n",
		       s_checks, UI_GESTURE_EXPECTED_CHECKS);
		return 1;
	}
	if (s_fails) {
		printf("FAIL tests/ui_gesture_test.c: %d of %d checks failed, first %s\n",
		       s_fails, s_checks, s_first);
		return 1;
	}
	printf("PASS tests/ui_gesture_test.c: %d checks\n", s_checks);
	return 0;
}
