#pragma once

#include <stdbool.h>

// v1.8.4. Which console this is, asked once, and the one place that asks the New 3DS for
// its clock.
//
// Before this file the model question was asked in three places — main.c:2852, main.c:3137
// and app/gputest.c:538 — each calling APT_CheckNew3DS for itself, and the answer was used
// for exactly one thing: picking a render-distance default. Nothing else on the machine was
// different. A New 3DS ran this game as an Old 3DS with a wider ring.
//
// Three separate calls is not a cost worth chasing on its own; what it is, is three places
// that can disagree, and none of them a place where "and therefore we should also ask for
// the faster clock" would naturally be written. So the question moves here, gets asked once
// at boot, and the answers hang off it.
//
// ── Why the clock is asked for HERE and not in app/sleep.c ─────────────────────────────
//
// It used to be in sleep.c, on the APT wake hook, and that was wrong in a specific way worth
// keeping written down: the wake half was the only call to osSetSpeedupEnable anywhere in the
// tree, so the app booted at the clock the exheader asked for and then ran at 804 MHz for the
// rest of the session from the first lid-close onwards. Nothing asked for that. sleep.c's own
// comment concluded "whoever wants speedup should ask for it once, visibly, at boot", and
// this file is that.
//
// ── The timing worry sleep.c raised, and why it does not apply ─────────────────────────
//
// sleep.c also worried that speeding up the CPU would break every timing measurement in the
// tree, because app/battery.c's tickMs() and ~40 other sites divide svcGetSystemTick by
// SYSCLOCK_ARM11. That worry is answered by libctru's own header, which is the authority on
// what the constant means — devkitPro/libctru/include/3ds/os.h:16 reads:
//
//     ///< The clock rate for the Arm11 in CTR mode and in \ref svcGetSystemTick.
//     #define SYSCLOCK_ARM11          (SYSCLOCK_ARM9 * 2)
//
// "and in svcGetSystemTick" is the load-bearing half. The system tick counter runs at
// SYSCLOCK_ARM11 whatever LGR mode the ARM11 is in — SYSCLOCK_ARM11_LGR2 (os.h:21) is the
// *core* clock, not the tick clock. So every existing tick-to-milliseconds division stays
// correct at 804 MHz and none of them were touched. If that ever stops being true the
// symptom is unmistakable and global: every measured millisecond figure in the game divides
// by three at once.
//
// Deliberately callable from the host suite: hwPreferredWorkerCore() is pure policy and is
// the part with a decision in it, so it lives above the __3DS__ guard and tests/hw_test.c
// links this exact file rather than a copy of the rule.

// ── Pure policy. No hardware, no libctru. ──────────────────────────────────────────────

// Cores the terrain worker should ask for, best first, written into `out`. Returns how many
// entries were written; never more than HW_CORE_LADDER_MAX and never zero.
//
// The ladder exists because every core above 0 is a request that can be refused, and the
// refusals have different causes:
//
//   core 2  New 3DS only, and only when the exheader granted it (cia/blocksmith.rsf's
//           CanAccessCore2). Under the Homebrew Launcher the .3dsx inherits the launcher's
//           exheader instead of ours, so this is expected to fail on the .3dsx build and
//           expected to work on the installed CIA. It is the best core there is for this
//           thread: nothing else on the console runs on it, so the worker stops competing
//           with the system for core 1's time slice.
//   core 1  the system core, shared, and only available at all if APT_SetAppCpuTimeLimit
//           succeeded — hence `cpu_time_limit_ok`. This is what worker.c has always used.
//   core 0  the main thread's own core, one priority step below it. Always available. Not a
//           failure: the main thread is blocked on the GPU for ~15.7 ms of every 16.71 ms
//           frame, and the worker gets that time.
//
// `new_3ds` false never offers core 2, because on an Old 3DS it does not exist and asking
// costs a threadCreate that must fail.
#define HW_CORE_LADDER_MAX 3
int hwPreferredWorkerCore(bool new_3ds, bool cpu_time_limit_ok, int out[HW_CORE_LADDER_MAX]);

// ── The cached answer. hwInit() fills it; everything else reads it. ────────────────────

// Asks APT_CheckNew3DS once and, on a New 3DS, asks for the faster clock and the L2 cache.
// Safe to call more than once; only the first call does anything. On the host build this
// sets "Old 3DS" unless hwTestSetNew3ds() said otherwise.
//
// Call it BEFORE anything that branches on the model — in particular before workerStart()
// and before the render-distance default is read.
void hwInit(void);

// True on a New 3DS (or New 2DS XL). False on an Old 3DS, an Old 2DS, and on the host.
// Meaningless before hwInit(); it answers false, which is the safe direction.
bool hwIsNew3ds(void);

// True when hwInit() actually asked for the New 3DS clock and L2 cache. This is NOT a
// promise that the console granted it — libctru's osSetSpeedupEnable returns void and there
// is nothing to check — it is a record that the request was made, so a debug overlay can say
// "asked" rather than implying "measured".
bool hwSpeedupRequested(void);

#ifndef __3DS__
// Host-only seam. Sets what the next hwInit() will report, so the ladder and everything
// downstream of the model flag can be exercised on both models from one test binary.
void hwTestSetNew3ds(bool new_3ds);
// Forgets that hwInit() ran, so a test can re-arm it. Console builds have no reason to.
void hwTestReset(void);
#endif
