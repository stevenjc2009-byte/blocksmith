#pragma once

#include <stdbool.h>
#include <stdint.h>

// Returned by batteryLevel() when there is no trustworthy reading — either PTM:U
// failed to open at boot or a PTMU_Get* call failed. It is deliberately NOT 0:
// before v1.6.0 an unopened PTM:U left the cached level at 0, which is
// indistinguishable from a genuinely flat battery, so a service failure drew a
// convincing empty red gauge on a full console with nothing to say it was lying.
#define BATTERY_LEVEL_UNKNOWN (-1)

// ── Cached reading and the pure logic over it ───────────────────────────────────────────
//
// Everything in this section is plain C. It is separated from the PTM:U half below the
// __3DS__ guard so tools/run_host_tests.sh can link the REAL source/app/battery.c into
// tests/battery_test.c — same arrangement as app/debugmenu_ui.c, and for the same reason:
// the directory says where the file belongs in the program, the guard says which half
// the host can actually prove.

// Replaces the cached reading. `valid` false means the reading could not be taken and
// the module goes to BATTERY_LEVEL_UNKNOWN, ignoring `level`/`charging`.
//
// Public rather than static because it is the seam the host suite drives: the polling
// half needs svcGetSystemTick and PTM:U, this half needs nothing, and the bar arithmetic
// under test must be the shipping copy rather than a hand-written twin of it.
void batteryApplyReading(int level, bool charging, bool valid);

// Last polled battery level, 0-5 (0 = empty, 5 = full), or BATTERY_LEVEL_UNKNOWN.
int batteryLevel(void);

// True if the charger is connected. Meaningless while batteryKnown() is false.
bool batteryCharging(void);

// False while the module has no trustworthy reading. Check this before believing
// batteryLevel(), batteryCharging() or the gauge.
bool batteryKnown(void);

// Level mapped to a 0-4 bar count for a 4-bar gauge display.
// BATTERY_LEVEL_UNKNOWN maps to 0 bars, the same as empty — the gauge tells the two
// apart by colour, not by bar count (see batteryDraw).
int batteryBars(void);

// True if battery is critically low (level <= 1 and not charging).
// Always false while batteryKnown() is false: a warning nobody can trust is worse
// than no warning, and BATTERY_LEVEL_UNKNOWN would otherwise satisfy "level <= 1".
bool batteryLow(void);

// ── PTM:U service and rendering — console build only ────────────────────────────────────
#ifdef __3DS__

#include <3ds.h>

// Opens the PTMU service. Call once at boot after screenInit().
// Survives failure: the module reports BATTERY_LEVEL_UNKNOWN instead of a fake 0 and
// batteryPoll() becomes a no-op rather than hammering an invalid handle.
void batteryInit(void);

// Closes the PTMU service. Call at app_shutdown after sleepExit().
void batteryExit(void);

// Reads battery level (0-5) and charge state, cached to at most once/second.
void batteryPoll(void);

// The Result ptmuInit() returned at boot, kept so a failure can be read out of the
// debug menu or a crash dump instead of only showing up as a gauge that never moves.
// 0 on success.
Result batteryInitResult(void);

// Draws a 4-segment battery gauge at (x, y) on the bottom screen.
// Requires an active sprite pass (spriteBegin already called).
// The font texture must already be bound.
void batteryDraw(float x, float y);

#endif
