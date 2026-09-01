// Host self-test for the battery bar-mapping, low-battery and unknown-state logic in
// source/app/battery.c. The REAL file is linked in by tools/run_host_tests.sh — its
// PTM:U and drawing half is behind an #ifdef __3DS__, exactly so the half this file
// checks is host-compilable.
//
// That is the whole point of this file's v1.6.0 rewrite. It used to be compiled as a
// lone translation unit carrying private testBars()/testLow() copies of the arithmetic,
// so every check passed regardless of what battery.c did: sabotaging batteryBars() to
// return 99 for levels 3-4 still printed "PASS, 16 checks". A test that cannot go red
// when the shipping code breaks is a comment, not cover.
//
// Built as a separate binary with its own main(), same pattern as app/options_test.c
// and app/updater_version_test.c.
#ifndef __3DS__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/battery.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// Drives the module the way batteryPoll() does on the console: one valid reading in,
// then read the derived state back out. batteryApplyReading is the only seam the host
// has, and it is the same call the real poll makes.
static void setReading(int level, bool charging)
{
	batteryApplyReading(level, charging, true);
}

static void testBarMapping(void)
{
	const struct { int level; int expected; } cases[] = {
		{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 3}, {5, 4},
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		setReading(cases[i].level, false);
		CHECK(batteryBars() == cases[i].expected);
	}
}

static void testLowBattery(void)
{
	setReading(0, false);
	CHECK(batteryLow());

	setReading(1, false);
	CHECK(batteryLow());

	setReading(2, false);
	CHECK(!batteryLow());

	setReading(5, false);
	CHECK(!batteryLow());
}

static void testLowBatteryCharging(void)
{
	setReading(0, true);
	CHECK(!batteryLow());

	setReading(1, true);
	CHECK(!batteryLow());
}

static void testEdgeCases(void)
{
	setReading(-1, false);
	CHECK(batteryBars() == 0);
	CHECK(batteryLow());

	setReading(6, false);
	CHECK(batteryBars() == 4);
	CHECK(!batteryLow());
}

// The v1.6.0 unknown state. Previously ptmuInit()'s Result was discarded, so a PTM:U
// that failed to open left the cached level at 0 and the HUD drew a full-confidence
// empty red gauge on a charged console. The contract now is: an invalid reading is
// distinguishable from a flat battery, and it must NOT raise the low-battery warning
// even though BATTERY_LEVEL_UNKNOWN (-1) satisfies the "level <= 1" range test.
static void testUnknownState(void)
{
	batteryApplyReading(0, false, false);
	CHECK(!batteryKnown());
	CHECK(batteryLevel() == BATTERY_LEVEL_UNKNOWN);
	CHECK(batteryBars() == 0);
	CHECK(!batteryLow());

	// An invalid reading must discard the values it was handed, not cache them.
	batteryApplyReading(5, true, false);
	CHECK(batteryLevel() == BATTERY_LEVEL_UNKNOWN);
	CHECK(!batteryCharging());
	CHECK(!batteryKnown());

	// A good reading clears it again, and a later bad one puts it back — the module
	// must not latch either way.
	setReading(4, false);
	CHECK(batteryKnown());
	CHECK(batteryLevel() == 4);
	CHECK(batteryBars() == 3);

	batteryApplyReading(4, false, false);
	CHECK(!batteryKnown());
	CHECK(batteryBars() == 0);
}

// Charge state must round-trip, since batteryLow() and nothing else reads it and a
// stuck-false charging flag would look identical to "not charging" everywhere.
static void testChargingRoundTrip(void)
{
	setReading(3, true);
	CHECK(batteryCharging());
	CHECK(batteryLevel() == 3);

	setReading(3, false);
	CHECK(!batteryCharging());
}

// v1.8.3 — the critical gauge blinks. Asked for as "whenever it gets low on, like, one
// bar, it should start blinking", and the whole cadence lives in batteryBlinkOn() rather
// than in batteryDraw() so that this file can hold it to account; the drawing half is
// behind __3DS__ and nothing here can link it.
static void testBlink(void)
{
	// A healthy battery is lit at every instant of the cycle. Not "mostly lit" — the
	// caller applies this unconditionally, so a single dark millisecond at level 5 would
	// be a gauge that flickers on a full console.
	setReading(5, false);
	for (uint64_t t = 0; t < 2000; t += 7) CHECK(batteryBlinkOn(t));

	setReading(2, false);
	CHECK(batteryBlinkOn(0));
	CHECK(batteryBlinkOn(800));

	// One bar, on battery: on for the first BATTERY_BLINK_ON_MS of each period, off for
	// the rest of it, and the same in the second period as in the first.
	setReading(1, false);
	CHECK(batteryBlinkOn(0));
	CHECK(batteryBlinkOn(BATTERY_BLINK_ON_MS - 1));
	CHECK(!batteryBlinkOn(BATTERY_BLINK_ON_MS));
	CHECK(!batteryBlinkOn(BATTERY_BLINK_PERIOD_MS - 1));
	CHECK(batteryBlinkOn(BATTERY_BLINK_PERIOD_MS));
	CHECK(!batteryBlinkOn(BATTERY_BLINK_PERIOD_MS + BATTERY_BLINK_ON_MS));

	// It really does spend time in both states rather than passing the boundary checks by
	// being stuck near one of them. Over two whole periods the duty must come out at
	// BATTERY_BLINK_ON_MS in BATTERY_BLINK_PERIOD_MS, exactly.
	{
		int on = 0;
		for (uint64_t t = 0; t < 2 * BATTERY_BLINK_PERIOD_MS; t++)
			if (batteryBlinkOn(t)) on++;
		CHECK(on == 2 * BATTERY_BLINK_ON_MS);
	}

	// Empty is critical too, and it is the case the warning matters most in. batteryDraw
	// dims the outline for it, because at zero bars there is nothing else to hide.
	setReading(0, false);
	CHECK(batteryBlinkOn(0));
	CHECK(!batteryBlinkOn(BATTERY_BLINK_ON_MS));

	// On the charger, a low battery is not a warning and must not blink.
	setReading(1, true);
	for (uint64_t t = 0; t < 2000; t += 7) CHECK(batteryBlinkOn(t));

	// And neither must a reading nobody can trust: BATTERY_LEVEL_UNKNOWN satisfies
	// "level <= 1", so a blink that tested the level itself would strobe the gauge on a
	// full console whose PTM:U failed to open. Gating on batteryLow() is what prevents it.
	batteryApplyReading(0, false, false);
	CHECK(!batteryLow());
	for (uint64_t t = 0; t < 2000; t += 7) CHECK(batteryBlinkOn(t));
}

int main(void)
{
	testBarMapping();
	testLowBattery();
	testLowBatteryCharging();
	testEdgeCases();
	testUnknownState();
	testChargingRoundTrip();
	testBlink();

	printf("battery bar mapping: %s\n",
	       s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n",
		       s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif /* !__3DS__ */
