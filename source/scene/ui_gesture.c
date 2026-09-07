#include "scene/ui_gesture.h"

static const UiGestureEvent NO_EVENT = { UI_GESTURE_NONE, -1, -1, false };

static bool slotOk(int slot)
{
	return slot >= 0 && slot < INV_SLOT_COUNT;
}

static bool slotHasItem(const Inventory* inv, int slot)
{
	return slotOk(slot) && inv->slots[slot].item != ITEM_NONE && inv->slots[slot].count > 0;
}

void uiGestureReset(UiGesture* g)
{
	g->touch_prev = false;
	g->keys_prev  = 0;
}

// The tap half: the two-tap model, classified. `slot` is a real slot the stylus just went
// down on. Returns NONE for the one tap that means nothing — a lift of an empty slot.
static UiGestureEvent classifyTap(const Inventory* inv, int lifted, int slot)
{
	UiGestureEvent ev = NO_EVENT;

	if (!slotOk(lifted)) {
		if (!slotHasItem(inv, slot)) return ev;   // nothing to lift
		ev.kind = UI_GESTURE_LIFT;
		ev.src  = slot;
		return ev;
	}

	ev.src = lifted;
	ev.dst = slot;
	if (slot == lifted) {
		ev.kind = UI_GESTURE_PLACE;   // the cancel: dst == src
		return ev;
	}

	// The lifted slot always still holds the lift's item — a whole-stack lift is by index, and
	// a split leaves the smaller half behind — so its item IS the lift's item.
	const ItemId carried = inv->slots[lifted].item;
	const ItemId there   = inv->slots[slot].item;
	ev.kind = (there != ITEM_NONE && there == carried) ? UI_GESTURE_MERGE : UI_GESTURE_PLACE;
	return ev;
}

UiGestureEvent uiGestureFeed(UiGesture* g, const UiGestureInput* in, const Inventory* inv)
{
	UiGestureEvent ev = NO_EVENT;
	if (!g || !in || !inv) return ev;

	// Both edges are taken BEFORE any early return below, so the state tracks reality on every
	// frame regardless of what this frame decided — see the header on why that matters for a
	// button held across the overlay opening.
	const bool     tap     = in->touch_down && !g->touch_prev;
	const uint32_t pressed = in->keys_held & ~g->keys_prev;
	g->touch_prev = in->touch_down;
	g->keys_prev  = in->keys_held;

	// A stylus that is down on a slot. Only meaningful while touch_down (ui.h: a release frame
	// has no meaningful position), and only a real slot.
	const int under = (in->touch_down && slotOk(in->slot_under)) ? in->slot_under : -1;

	// Taps first — see the header on the one-event-a-frame rule.
	if (tap && under >= 0 && in->lift_enabled)
		return classifyTap(inv, in->lifted_slot, under);

	if (pressed & UI_GESTURE_KEY_Y) {
		if (!in->lift_enabled || !in->split_enabled) return ev;   // see the header on split_enabled
		const int target = slotOk(in->lifted_slot) ? in->lifted_slot : under;
		if (!slotHasItem(inv, target) || inv->slots[target].count < 2) return ev;
		ev.kind = UI_GESTURE_SPLIT;
		ev.src  = target;
		return ev;
	}

	if (pressed & UI_GESTURE_KEY_X) {
		const int target = (under >= 0) ? under : in->lifted_slot;
		if (!slotHasItem(inv, target)) return ev;
		ev.kind     = UI_GESTURE_QUICK_MOVE;
		ev.src      = target;
		ev.to_chest = in->chest_open;
		return ev;
	}

	return ev;
}
