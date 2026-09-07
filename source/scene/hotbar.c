// v1.9.0 HOTBAR-LR. See hotbar.h for the contract. No <3ds.h>: this file is linked by
// tests/hotbar_test.c on the host, and the key bits are the header's mirrors of libctru's.
#include "scene/hotbar.h"

#include "world/inventory.h"   // inventoryHotbarStep — see hotbarNavStep below for why

#ifdef __3DS__
#include <3ds.h>
_Static_assert(HOTBAR_KEY_L == KEY_L && HOTBAR_KEY_R == KEY_R,
               "scene/hotbar.h's key bits must mirror libctru's KEY_L / KEY_R");
#endif

// ── One wrap, in one place (v1.9.0 integration) ────────────────────────────────────────
//
// Two lanes wrote this arithmetic independently: world/inventory.h's inventoryHotbarStep()
// and this function. They are now ONE — the step below is inventoryHotbarStep's, delegated,
// so a change to how the hotbar wraps happens once. world/inventory.c is byte-mirrored to the
// server and could not be the caller, so this module is the side that gives way.
//
// The two did not agree on their edges, and this file's behaviour is the one kept (it is what
// main.c and tests/hotbar_test.c already observe), so the delegation is wrapped rather than
// bare:
//
//   * COUNT. inventoryHotbarStep is fixed at INV_HOTBAR_SLOTS and has no count parameter;
//     this function is generic in one. main.c only ever passes INV_HOTBAR_SLOTS, so that is
//     the path that delegates. tests/hotbar_test.c also proves the wrap at count 3 and 1
//     ("rather than at one build's constant"), and inventoryHotbarStep cannot express those,
//     so the generic wrap stays below for them alone.
//   * NEGATIVE sel. This function clamps a negative *sel up to 0 before stepping;
//     inventoryHotbarStep takes a uint8_t and cannot see one (-5 would arrive as 251 and clamp
//     DOWN to the last slot, which is the opposite answer — hotbar_test.c's `sel = -5;
//     hotbarNavStep(&sel, -1, SLOTS); CHECK(sel == SLOTS - 1)` catches exactly that). So the
//     clamp happens HERE, before the hand-off, and only an already-in-range index is passed on.
//
// Note this costs the module the header independence hotbar_test.c's SLOTS comment claims for
// it ("the module takes `count` precisely so it does not depend on that header"): scene/hotbar.c
// now needs world/inventory.h to compile and world/inventory.c (and world/crc32.c behind it)
// to link. That was the integration's call, not this file's.
void hotbarNavStep(int* sel, int dir, int count)
{
	if (!sel || count < 1) return;

	int s = *sel;
	if (s < 0)      s = 0;
	if (s >= count) s = count - 1;

	// `s` is in [0, count-1] here, so at count == INV_HOTBAR_SLOTS it is a real hotbar slot and
	// inventoryHotbarStep's own clamp is a no-op — the two agree exactly from this point on.
	if (count == INV_HOTBAR_SLOTS) {
		*sel = (int)inventoryHotbarStep((uint8_t)s, dir);
		return;
	}

	// Only tests/hotbar_test.c's count 3 and count 1 cases reach this.
	if (dir > 0)      s = (s + 1 == count) ? 0 : s + 1;
	else if (dir < 0) s = (s == 0) ? count - 1 : s - 1;

	*sel = s;
}

// Which of the two buttons, if either, went down THIS frame. Both on one frame is a tie and
// answers 0 — there is no order to break it with.
static uint32_t pressedThisFrame(uint32_t down)
{
	const uint32_t both = down & (HOTBAR_KEY_L | HOTBAR_KEY_R);
	if (both == HOTBAR_KEY_L || both == HOTBAR_KEY_R) return both;
	return 0;
}

int hotbarNavPoll(HotbarNav* nav, uint32_t down, uint32_t held)
{
	if (!nav) return 0;

	// A tie on the same frame moves nothing and cancels whatever was repeating: the player
	// has both buttons down and neither was asked for more recently than the other.
	if ((down & (HOTBAR_KEY_L | HOTBAR_KEY_R)) == (HOTBAR_KEY_L | HOTBAR_KEY_R)) {
		nav->active      = 0;
		nav->held_frames = 0;
		return 0;
	}

	// A fresh press takes over from whatever was active — last pressed wins — and restarts
	// the delay, so the switch fires at once below (held_frames 0) like any other press.
	const uint32_t pressed = pressedThisFrame(down);
	if (pressed) {
		nav->active      = pressed;
		nav->held_frames = 0;
	}

	if (!nav->active) return 0;

	// Release re-arms. Checked AFTER the press, not before: a press whose release lands on the
	// same scan (down set, held clear — libctru cannot report that, but the arithmetic must
	// not care) still fires once. The other button staying down is NOT a hand-over — see the
	// header on why.
	if (nav->held_frames > 0 && !(held & nav->active)) {
		nav->active      = 0;
		nav->held_frames = 0;
		return 0;
	}

	const int f = nav->held_frames++;
	bool fire;
	if (f == 0)                        fire = true;    // the press itself
	else if (f < HOTBAR_REPEAT_DELAY)  fire = false;   // the pause before repeating
	else fire = ((f - HOTBAR_REPEAT_DELAY) % HOTBAR_REPEAT_PERIOD) == 0;

	if (!fire) return 0;
	return (nav->active == HOTBAR_KEY_R) ? +1 : -1;
}

bool hotbarNavFrame(HotbarNav* nav, uint32_t down, uint32_t held, int* sel, int count)
{
	if (!sel) return false;
	const int dir = hotbarNavPoll(nav, down, held);
	if (dir == 0) return false;
	const int before = *sel;
	hotbarNavStep(sel, dir, count);
	return *sel != before;
}
