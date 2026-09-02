#include "app/battery.h"

// ── Cached reading and the pure logic over it ───────────────────────────────────────────
//
// No <3ds.h> above the __3DS__ guard further down, which is the whole point: the host
// suite links THIS file, so the bar arithmetic it checks is the arithmetic that ships.
// Until v1.6.0 tests/battery_test.c was compiled as a lone translation unit carrying its
// own hand-copied testBars()/testLow(), and its 16 checks passed no matter what the code
// below did — deleting this file outright left them green. Same split as app/debugmenu_ui.c.

static int  s_level    = BATTERY_LEVEL_UNKNOWN;
static bool s_charging;
static bool s_known;

void batteryApplyReading(int level, bool charging, bool valid)
{
	if (!valid) {
		// Drop the whole reading rather than half of it. A failed PTMU_Get* leaves its
		// out-parameter untouched, so keeping `charging` from a call that did succeed
		// would pair a real charge state with a level that was never read.
		s_level    = BATTERY_LEVEL_UNKNOWN;
		s_charging = false;
		s_known    = false;
		return;
	}

	s_level    = level;
	s_charging = charging;
	s_known    = true;
}

int  batteryLevel(void)    { return s_level; }
bool batteryCharging(void) { return s_charging; }
bool batteryKnown(void)    { return s_known; }

int batteryBars(void)
{
	// Level 0-5 maps to 0-4 bars. Level 0 = 0 bars, 1 = 1, 2 = 2, 3-4 = 3, 5 = 4.
	// BATTERY_LEVEL_UNKNOWN is negative and falls into the first branch.
	if (s_level <= 0) return 0;
	if (s_level == 1) return 1;
	if (s_level == 2) return 2;
	if (s_level <= 4) return 3;
	return 4;
}

bool batteryLow(void)
{
	// The !s_known term is not redundant with the range check: BATTERY_LEVEL_UNKNOWN is
	// -1, which satisfies `s_level <= 1`, so without it a PTM:U that failed to open would
	// flash a critical-battery warning on a console that is plugged in and full.
	return s_known && s_level <= 1 && !s_charging;
}

bool batteryBlinkOn(uint64_t now_ms)
{
	// Gated on batteryLow() rather than on a second copy of the same rule, so the gauge can
	// never blink on a battery the rest of this module calls healthy, nor sit steady on one
	// it calls critical.
	if (!batteryLow()) return true;
	return (now_ms % BATTERY_BLINK_PERIOD_MS) < BATTERY_BLINK_ON_MS;
}

// ── PTM:U service and rendering — console build only ────────────────────────────────────
#ifdef __3DS__

#include <3ds.h>

#include "gfx/sprite.h"

#define POLL_INTERVAL_MS 1000

static u64    s_last_poll_tick;
static bool   s_ptmu_open;
static Result s_ptmu_rc;

static u64 tickMs(void)
{
	return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

void batteryInit(void)
{
	s_ptmu_rc   = ptmuInit();
	s_ptmu_open = R_SUCCEEDED(s_ptmu_rc);
	s_last_poll_tick = 0;

	// An unopened PTM:U means every PTMU_Get* below runs on an invalid handle and leaves
	// its out-parameter alone, so the old code's `u8 level = 0` sailed through as a real
	// reading of "empty". Start from — and stay at — the explicit unknown state instead;
	// batteryInitResult() keeps the Result itself for anyone who wants the reason.
	batteryApplyReading(0, false, false);
}

void batteryExit(void)
{
	// Only close what actually opened. ptmuExit() on a service that never opened
	// decrements a refcount that was never incremented.
	if (!s_ptmu_open) return;
	ptmuExit();
	s_ptmu_open = false;
}

void batteryPoll(void)
{
#if BS_FAKE_BATTERY
	// Verification instrument, off in every shipping build. The ONLY thing it replaces is
	// where the reading comes from: it feeds a made-up level through batteryApplyReading —
	// the same call the real read below ends in — and then gets out of the way. batteryLow(),
	// the 1000/600 ms cadence and batteryDraw are all reached by their normal path and are
	// not touched, because an override that reimplemented them would prove nothing about the
	// code that ships.
	//
	// It exists because the low-battery state is otherwise unreachable by anybody. Azahar's
	// PTM:U stub (src/core/hle/service/ptm/ptm.cpp) answers GetBatteryLevel with a hardcoded
	// CompletelyFull (5) and GetBatteryChargeState with a battery_is_charging that is
	// initialised true and never assigned, so batteryLow()'s `s_level <= 1` and `!s_charging`
	// each fail on their own, and there is no config key or UI toggle to change either. Real
	// hardware is out under the standing never-run rule. So without this flag, "the gauge
	// blinks at one bar" is a claim nothing on this machine can check.
	//
	// Ramps 5 -> 0, four seconds a level, twenty-four seconds a lap, then repeats. A ramp
	// rather than a fixed level 1 so one boot shows the steady gauge, the transition into the
	// warning, and the warning itself — a fixed value would only ever demonstrate the end
	// state, and could not tell a blink from a gauge that had been blinking all along.
	//
	// Sits above the s_ptmu_open check on purpose: the instrument must not need the service
	// it is standing in for. It reads the clock and writes the cached reading, and nothing
	// else — no file, no archive, no sdmc path, no SD access of any kind. That is deliberate
	// and load-bearing: a previous verification build on this project shared the emulator's
	// SD folder and overwrote a real save, so a build that exists only to be looked at must
	// not be able to write.
	{
		const unsigned step = (unsigned)((tickMs() / 4000u) % 6u);
		batteryApplyReading(5 - (int)step, false, true);
		return;
	}
#endif

	// No handle, nothing to poll. Returning early rather than calling anyway also stops
	// the module from overwriting the unknown state with a stale zero every second.
	if (!s_ptmu_open) return;

	const u64 now = tickMs();
	if (now - s_last_poll_tick < POLL_INTERVAL_MS) return;
	s_last_poll_tick = now;

	u8 level  = 0;
	u8 charge = 0;
	const Result rc_level  = PTMU_GetBatteryLevel(&level);
	const Result rc_charge = PTMU_GetBatteryChargeState(&charge);

	// Both must succeed. A half-read is reported as unknown rather than mixed with the
	// previous second's other half, which would silently pin the gauge at whatever it
	// last managed to read.
	const bool valid = R_SUCCEEDED(rc_level) && R_SUCCEEDED(rc_charge);
	batteryApplyReading((int)level, charge != 0, valid);
}

Result batteryInitResult(void) { return s_ptmu_rc; }

void batteryDraw(float x, float y)
{
	const float bar_w  = 6.0f;
	const float bar_h  = 10.0f;
	const float gap    = 1.0f;
	const float outline = 1.0f;
	const int bars = batteryBars();
	const bool known = batteryKnown();

	// v1.8.3 — the critical blink. Always true unless batteryLow() is, so nothing above a
	// critical battery moves at all. On the off phase the filled bars are not drawn AND the
	// outline drops to the dim grey: at one bar the bar is what pulses, and at zero bars
	// there is no bar left to hide, so the outline has to carry the warning on its own.
	const bool lit = batteryBlinkOn(tickMs());

	// An unknown reading draws the same zero bars as a flat battery, so the outline is
	// what tells them apart: dimmed to the empty-cell grey when there is no reading, so
	// a dead PTM:U looks switched-off rather than looking like a console about to die.
	const uint32_t outline_col = (known && lit)
		? SPRITE_RGBA(200, 200, 200, 255)
		: SPRITE_RGBA(90, 90, 90, 255);
	const uint32_t fill_col    = batteryLow()
		? SPRITE_RGBA(220, 50, 50, 255)
		: SPRITE_RGBA(80, 200, 80, 255);
	const uint32_t empty_col   = SPRITE_RGBA(60, 60, 60, 128);

	// Outline
	spriteRect(x, y, bar_w * 4 + gap * 3 + outline * 2,
	           bar_h + outline * 2, outline_col);

	// Background inside outline
	spriteRect(x + outline, y + outline,
	           bar_w * 4 + gap * 3, bar_h, empty_col);

	// Filled bars — none of them on the blink's off phase.
	const int drawn = lit ? bars : 0;
	for (int i = 0; i < drawn; i++) {
		float bx = x + outline + (float)i * (bar_w + gap);
		spriteRect(bx, y + outline, bar_w, bar_h, fill_col);
	}
}

#endif  // __3DS__
