#pragma once

#include <stdbool.h>

// ── What actually happens when the lid closes ──────────────────────────────────────────
//
// Measured, not assumed. Disassembling libctru's apt.o (arm-none-eabi-objdump -d -r on
// apt.o pulled out of C:\devkitPro\libctru\lib\libctru.a) says:
//
//   .text.aptMainLoop  ->  bl aptHandleSleep, bl aptHandleJumpToHome, bl aptShouldClose
//   .text.aptHandleSleep:
//       mov r0, #2            ; APTHOOK_ONSLEEP
//       bl  aptCallHook
//       bl  APT_ReplySleepNotificationComplete
//       bl  LightEvent_Wait   ; <- the console is asleep for the whole of this call
//       bl  aptIsActive
//       mov r0, #3            ; APTHOOK_ONWAKEUP
//       bl  aptCallHook
//
// Three consequences, and every design decision in sleep.c follows from one of them:
//
//  1. Both hooks fire, and the entire sleep elapses, INSIDE aptMainLoop() — before the
//     loop body ever runs. That is why there is no sleepShouldSkip() here any more: the
//     flag it returned was always back to false by the time main.c could read it, so the
//     `if (sleepShouldSkip()) continue;` that used to sit at main.c's frame top could
//     never once have fired. See sleep.c for the full note.
//
//  2. The hooks run on whichever thread called aptMainLoop(), which in this app is the
//     main thread. That is what makes the flush below legal at all: app/worker.h marks
//     workerSubmitSave() MAIN THREAD ONLY, and this is the main thread.
//
//  3. Nothing of ours runs between the two hooks. No keepalive, no heartbeat, no packet
//     of any kind can be sent while the lid is shut, because the process is parked in
//     LightEvent_Wait. sleepNetHeartbeat() — which claimed otherwise — is gone; see the
//     leave hook below for what replaced it.

// How long the lid-close flush may spend writing columns to the card, in milliseconds.
//
// ⚠ This number is a GUESS and is marked as one deliberately. Nobody has ever measured
// how long one region-file write takes on a real 3DS SD card for this project — every
// figure this repo has for the save path came from a host run against a desktop
// filesystem, which is not the same hardware and not the same order of magnitude. 500 ms
// is picked to be long enough to be worth having and short enough that a player who
// closed the lid does not notice the console taking a moment to settle. Replace it with a
// measured value the first time somebody times workerFlushSaves() on hardware.
//
// The budget is checked BETWEEN columns rather than once at the start (see
// sleepFlushBounded), so overshoot is bounded by one column's write, not by the whole
// dirty set.
#define SLEEP_FLUSH_BUDGET_MS 500u

// Milliseconds from some fixed point — only differences are ever used, so the epoch does
// not matter and wraparound is handled by unsigned subtraction. A parameter rather than a
// static hidden inside sleep.c so the host test can drive the budget loop with a fake
// clock and prove the real loop stops where it says it stops.
typedef unsigned (*SleepClockMs)(void);

// Writes at most one dirty column to the card and blocks until it lands.
//
// Returns true if it did work and there may be more to do, false once nothing dirty is
// left. sleep.c calls it repeatedly until it returns false or the budget above runs out;
// it knows nothing about what a column is, which is the point — main.c owns s_world and
// saveDirtyColumns() and this callback is the only way into them.
typedef bool (*SleepFlushStep)(void);

// Called once at ONSLEEP, before the flush. main.c registers a shim that calls
// netDisconnect() (net/bsnet.h) for a server session and nothing at all otherwise.
typedef void (*SleepLeaveHook)(void);

// Registers, or with NULL un-registers, the lid-close flush.
//
// It MUST be NULL whenever no world is loaded — the title screen and the world list —
// because the step it points at walks main.c's slot table and hands columns to a worker
// thread that does not exist yet. main.c registers it after genStart() succeeds and
// clears it before workerStop(). Nothing is registered by default, so a build that never
// calls this simply does not flush.
void sleepSetFlushHook(SleepFlushStep step);

// Registers, or with NULL un-registers, the network leave. Same lifetime rules as the
// flush hook and for the same reason.
void sleepSetLeaveHook(SleepLeaveHook hook);

// Installs the APT hook that drives all of the above. Call once at boot after netInit().
// Console-only: there is no APT on the host, so this symbol does not exist in the host
// build at all — same split as app/battery.c's PTM:U half.
void sleepInit(void);

// Removes the APT hook. Call at app_shutdown after updaterExit(). Console-only.
void sleepExit(void);

// True between APTHOOK_ONSLEEP and APTHOOK_ONWAKEUP.
//
// Read the header note above before reaching for this: that window opens and closes
// inside a single aptMainLoop() call, so a caller in the main-loop BODY can never observe
// it as true. It is the module's own state, and it is honest from inside the hook chain
// and from the host test — it is not a "are we asleep?" question the frame can usefully
// ask, because a frame only ever runs when the answer is no.
bool sleepIsAsleep(void);

// ── The pure half, which is what the host suite links and exercises ────────────────────
//
// sleepOnSleep/sleepOnWake are what the APT hook calls; they carry the whole of the
// module's behaviour and none of libctru. tests/sleep_test.c calls exactly these, so a
// change to what closing the lid does is a change the host suite sees.

// ONSLEEP: raises the asleep flag, runs the leave hook, then spends up to
// SLEEP_FLUSH_BUDGET_MS in the flush hook. `now_ms` is the clock the budget is measured
// with; a NULL clock skips the flush entirely rather than looping without a stop
// condition.
void sleepOnSleep(SleepClockMs now_ms);

// ONWAKEUP: clears the asleep flag. Deliberately does nothing else — see sleep.c.
void sleepOnWake(void);

// Drives `step` until it returns false or `budget_ms` has elapsed on `now_ms`, and
// returns how many times `step` reported doing work. A NULL step or clock returns 0.
// Exposed for the host test; sleepOnSleep is the only production caller.
int sleepFlushBounded(SleepFlushStep step, SleepClockMs now_ms, unsigned budget_ms);
