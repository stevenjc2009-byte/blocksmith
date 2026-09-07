// Host gate for scene/hotbar.c — the L/R shoulder-button hotbar cycle (v1.9.0 HOTBAR-LR).
//
// WHY THIS FILE LINKS THE REAL MODULE. Everything that can be WRONG about the control — the
// wrap at each end, the held-to-repeat cadence, what two buttons at once do — lives in
// scene/hotbar.c, which has no <3ds.h> in it precisely so this binary can link THAT module,
// the same carve-out tests/ringorder_test.c and tests/aimtext_test.c made. Nothing here is a
// copy of the code under test; the key bits are the header's own mirrors of libctru's.
//
// WHAT THIS FILE CANNOT PROVE, stated up front: that main.c calls hotbarNavFrame() from the
// frame loop with the real `down`/`held` words, that the answer reaches invBridgeSelectHotbar,
// or that the bottom screen's highlight moves. Those are the console's.
//
// Own main(), same CHECK macro and PASS/FAIL line as tests/ringorder_test.c.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene/hotbar.h"

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
#define HOTBAR_EXPECTED_CHECKS 598

// The bar this is for: world/inventory.h's INV_HOTBAR_SLOTS. Written as a literal on purpose —
// the module takes `count` precisely so it does not depend on that header, and this test
// proves the arithmetic at 8, at 1 and at 3 rather than at one build's constant.
#define SLOTS 8

#define L HOTBAR_KEY_L
#define R HOTBAR_KEY_R

// ── hotbarNavStep ──────────────────────────────────────────────────────────────────────

static void testStepWrapsBothWays(void)
{
	// Every slot, both directions: the wrap at each end is reached by the loop, not just
	// asserted at the ends.
	for (int s = 0; s < SLOTS; s++) {
		int sel = s;
		hotbarNavStep(&sel, +1, SLOTS);
		CHECK(sel == (s + 1) % SLOTS);
		sel = s;
		hotbarNavStep(&sel, -1, SLOTS);
		CHECK(sel == (s + SLOTS - 1) % SLOTS);
		sel = s;
		hotbarNavStep(&sel, 0, SLOTS);
		CHECK(sel == s);                                   // dir 0 is a no-op on every slot
	}
	// The two ends, named, because they are the whole point.
	int sel = SLOTS - 1; hotbarNavStep(&sel, +1, SLOTS); CHECK(sel == 0);
	sel = 0;             hotbarNavStep(&sel, -1, SLOTS); CHECK(sel == SLOTS - 1);
	// A full lap either way lands home.
	sel = 3;
	for (int i = 0; i < SLOTS; i++) hotbarNavStep(&sel, +1, SLOTS);
	CHECK(sel == 3);
	for (int i = 0; i < SLOTS; i++) hotbarNavStep(&sel, -1, SLOTS);
	CHECK(sel == 3);
	// dir is a direction, not a distance.
	sel = 3; hotbarNavStep(&sel, +5, SLOTS); CHECK(sel == 4);
	sel = 3; hotbarNavStep(&sel, -7, SLOTS); CHECK(sel == 2);
	// Out-of-range input is clamped before the step, so the answer is always a real slot.
	sel = 200; hotbarNavStep(&sel, +1, SLOTS); CHECK(sel == 0);
	sel = 200; hotbarNavStep(&sel, -1, SLOTS); CHECK(sel == SLOTS - 2);
	sel = 200; hotbarNavStep(&sel,  0, SLOTS); CHECK(sel == SLOTS - 1);
	sel = -5;  hotbarNavStep(&sel, -1, SLOTS); CHECK(sel == SLOTS - 1);
	sel = -5;  hotbarNavStep(&sel, +1, SLOTS); CHECK(sel == 1);
	// A different count is not a different rule.
	sel = 2; hotbarNavStep(&sel, +1, 3); CHECK(sel == 0);
	sel = 0; hotbarNavStep(&sel, -1, 3); CHECK(sel == 2);
}

static void testStepCountOne(void)
{
	// One slot: there is nowhere to go, in either direction, from anywhere.
	int sel = 0;
	hotbarNavStep(&sel, +1, 1); CHECK(sel == 0);
	hotbarNavStep(&sel, -1, 1); CHECK(sel == 0);
	sel = 7;
	hotbarNavStep(&sel, +1, 1); CHECK(sel == 0);
	// No slots at all, or a NULL target: nothing written, nothing crashed.
	sel = 5;
	hotbarNavStep(&sel, +1, 0);  CHECK(sel == 5);
	hotbarNavStep(&sel, -1, -3); CHECK(sel == 5);
	hotbarNavStep(NULL, +1, SLOTS);
	CHECK(true);   // reached: the NULL was refused, not dereferenced
}

// ── hotbarNavPoll: cadence ─────────────────────────────────────────────────────────────

// Does the poll fire on frame f of a hold that began on frame 0? Read straight off the
// header's constants, so a change to either is a change to this test's expectation too.
static bool expectFire(int f)
{
	if (f == 0) return true;
	if (f < HOTBAR_REPEAT_DELAY) return false;
	return ((f - HOTBAR_REPEAT_DELAY) % HOTBAR_REPEAT_PERIOD) == 0;
}

static void testHeldRepeatCadence(void)
{
	// R pressed on frame 0 and held for 100 frames: the press fires, then nothing for the
	// delay, then exactly one fire every period. Every frame is checked individually, so an
	// off-by-one in either constant, or a repeat that never starts, lands on a named frame.
	HotbarNav nav = {0};
	int fires = 0;
	for (int f = 0; f < 100; f++) {
		const int dir = hotbarNavPoll(&nav, f == 0 ? R : 0u, R);
		CHECK(dir == (expectFire(f) ? +1 : 0));
		if (dir) fires++;
	}
	// 0, 20, 32, 44, 56, 68, 80, 92 at DELAY 20 / PERIOD 12: eight fires in 100 frames.
	CHECK(fires == 1 + (99 - HOTBAR_REPEAT_DELAY) / HOTBAR_REPEAT_PERIOD + 1);
	CHECK(fires == 8);

	// Same for L, and it answers -1.
	HotbarNav navl = {0};
	fires = 0;
	for (int f = 0; f < 100; f++) {
		const int dir = hotbarNavPoll(&navl, f == 0 ? L : 0u, L);
		CHECK(dir == (expectFire(f) ? -1 : 0));
		if (dir) fires++;
	}
	CHECK(fires == 8);

	// The constants themselves, so a "feel" edit is a deliberate edit.
	CHECK(HOTBAR_REPEAT_DELAY == 20);
	CHECK(HOTBAR_REPEAT_PERIOD == 12);
}

static void testReleaseRearms(void)
{
	HotbarNav nav = {0};
	// Press, hold five frames (well short of the delay), release for one frame.
	CHECK(hotbarNavPoll(&nav, R, R) == +1);
	for (int f = 1; f < 5; f++) CHECK(hotbarNavPoll(&nav, 0u, R) == 0);
	CHECK(hotbarNavPoll(&nav, 0u, 0u) == 0);
	// The next press fires at once — the release re-armed it — and the delay starts over:
	// nothing until DELAY frames after THIS press, not after the first.
	CHECK(hotbarNavPoll(&nav, R, R) == +1);
	for (int f = 1; f < HOTBAR_REPEAT_DELAY; f++) CHECK(hotbarNavPoll(&nav, 0u, R) == 0);
	CHECK(hotbarNavPoll(&nav, 0u, R) == +1);   // frame DELAY of the second hold

	// Release mid-repeat, deep into a hold: same rule, nothing carries across the release.
	for (int f = 0; f < 3 * HOTBAR_REPEAT_PERIOD; f++) (void)hotbarNavPoll(&nav, 0u, R);
	CHECK(hotbarNavPoll(&nav, 0u, 0u) == 0);
	for (int f = 0; f < 40; f++) CHECK(hotbarNavPoll(&nav, 0u, 0u) == 0);   // idle stays idle
	CHECK(hotbarNavPoll(&nav, L, L) == -1);
	for (int f = 1; f < HOTBAR_REPEAT_DELAY; f++) CHECK(hotbarNavPoll(&nav, 0u, L) == 0);
	CHECK(hotbarNavPoll(&nav, 0u, L) == -1);

	// A press whose release lands on the same scan (down set, held clear) still fires once
	// and leaves nothing armed.
	HotbarNav tap = {0};
	CHECK(hotbarNavPoll(&tap, R, 0u) == +1);
	CHECK(hotbarNavPoll(&tap, 0u, 0u) == 0);
	CHECK(tap.active == 0);
}

// ── hotbarNavPoll: both buttons ────────────────────────────────────────────────────────

static void testBothButtons(void)
{
	// Both pressed on the SAME frame: a tie, no movement, and holding both moves nothing.
	HotbarNav tie = {0};
	CHECK(hotbarNavPoll(&tie, L | R, L | R) == 0);
	for (int f = 0; f < 60; f++) CHECK(hotbarNavPoll(&tie, 0u, L | R) == 0);
	CHECK(tie.active == 0);

	// LAST PRESSED WINS. L held and repeating; R pressed on top of it switches direction on
	// that very frame and restarts the delay. L's repeat is over even though L is still down.
	HotbarNav nav = {0};
	CHECK(hotbarNavPoll(&nav, L, L) == -1);
	for (int f = 1; f <= HOTBAR_REPEAT_DELAY; f++) (void)hotbarNavPoll(&nav, 0u, L);
	// f == DELAY just fired -1; now on the next frame press R with L still held.
	CHECK(hotbarNavPoll(&nav, R, L | R) == +1);
	for (int f = 1; f < HOTBAR_REPEAT_DELAY; f++) CHECK(hotbarNavPoll(&nav, 0u, L | R) == 0);
	CHECK(hotbarNavPoll(&nav, 0u, L | R) == +1);   // R's repeat, on R's own clock
	for (int f = 1; f < HOTBAR_REPEAT_PERIOD; f++) CHECK(hotbarNavPoll(&nav, 0u, L | R) == 0);
	CHECK(hotbarNavPoll(&nav, 0u, L | R) == +1);

	// Releasing the active button while the other is still down does NOT hand over: L was
	// pressed earlier and already had its say. Nothing moves until a button is pressed again.
	CHECK(hotbarNavPoll(&nav, 0u, L) == 0);
	for (int f = 0; f < 60; f++) CHECK(hotbarNavPoll(&nav, 0u, L) == 0);
	CHECK(nav.active == 0);
	// ...and pressing L again (after letting go of it) fires at once.
	CHECK(hotbarNavPoll(&nav, 0u, 0u) == 0);
	CHECK(hotbarNavPoll(&nav, L, L) == -1);

	// A tie landing on top of a running repeat cancels it.
	HotbarNav run = {0};
	CHECK(hotbarNavPoll(&run, R, R) == +1);
	CHECK(hotbarNavPoll(&run, L | R, L | R) == 0);
	for (int f = 0; f < 40; f++) CHECK(hotbarNavPoll(&run, 0u, L | R) == 0);
}

// ── hotbarNavFrame: the call main.c makes ──────────────────────────────────────────────

static void testFrameMovesSelection(void)
{
	HotbarNav nav = {0};
	int sel = SLOTS - 1;
	// R off the last slot wraps to 0 and reports a change.
	CHECK(hotbarNavFrame(&nav, R, R, &sel, SLOTS) == true);
	CHECK(sel == 0);
	// Held inside the delay: no change reported, selection untouched.
	for (int f = 1; f < HOTBAR_REPEAT_DELAY; f++) {
		CHECK(hotbarNavFrame(&nav, 0u, R, &sel, SLOTS) == false);
		CHECK(sel == 0);
	}
	CHECK(hotbarNavFrame(&nav, 0u, R, &sel, SLOTS) == true);
	CHECK(sel == 1);
	// Release, then L off slot 1 twice via press/release: 0, then wraps to the last slot.
	CHECK(hotbarNavFrame(&nav, 0u, 0u, &sel, SLOTS) == false);
	CHECK(hotbarNavFrame(&nav, L, L, &sel, SLOTS) == true);
	CHECK(sel == 0);
	CHECK(hotbarNavFrame(&nav, 0u, 0u, &sel, SLOTS) == false);
	CHECK(hotbarNavFrame(&nav, L, L, &sel, SLOTS) == true);
	CHECK(sel == SLOTS - 1);
	// Nothing pressed: false, and the selection is not even clamped.
	int odd = 200;
	CHECK(hotbarNavFrame(&nav, 0u, 0u, &odd, SLOTS) == false);
	CHECK(odd == 200);
	// count 1: a press is a real poll (it arms the repeat) but the selection cannot change,
	// so the answer is false — main.c must not send a SELECT for a bar that did not move.
	HotbarNav one = {0};
	int only = 0;
	CHECK(hotbarNavFrame(&one, R, R, &only, 1) == false);
	CHECK(only == 0);
	CHECK(hotbarNavFrame(NULL, R, R, &only, SLOTS) == false);   // NULL nav is refused
	CHECK(hotbarNavFrame(&one, R, R, NULL, SLOTS) == false);    // NULL sel is refused
}

int main(void)
{
	testStepWrapsBothWays();
	testStepCountOne();
	testHeldRepeatCadence();
	testReleaseRearms();
	testBothButtons();
	testFrameMovesSelection();

	CHECK(s_checks + 1 == HOTBAR_EXPECTED_CHECKS);

	if (s_fails) {
		printf("hotbar self-test: FAILED - %d of %d checks, first: %s\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("hotbar self-test: PASS %d checks\n", s_checks);
	return 0;
}
