// Host self-test for app/sleep.c — the REAL file, in the link.
//
// It was not always. Until v1.6.0 tools/run_host_tests.sh compiled this file as a lone
// translation unit and it carried private testSleepEnter()/testSleepShouldSkip() copies of
// the flag machine, so its 14 checks passed no matter what sleep.c did — the script's own
// comment admitted it was testing "tests/sleep_test.c's own miniaturised logic". Linking
// source/app/sleep.c is the fix, exactly as the battery_test stanza above it does, and it
// is what lets the checks below be about lid-close behaviour rather than about themselves.
//
// sleep.c splits console-only code behind #ifdef __3DS__ — aptHook, the APT callback and
// svcGetSystemTick live there and nothing above the guard includes <3ds.h> — so the whole
// of what closing the lid actually DOES compiles here with nothing stubbed and nothing
// faked. Same split as app/battery.c and app/debugmenu_ui.c.
//
// Built by tools/run_host_tests.sh as a separate binary, same pattern as
// app/options_test.c and app/updater_version_test.c.
#ifndef __3DS__

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/sleep.h"

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

// ── The doubles the real module is driven with ─────────────────────────────────────────
//
// A clock that advances by a fixed amount on every read, and a flush step that reports
// work a fixed number of times and then reports none left. Between them they let the
// budget loop be asked the only questions worth asking of it: does it stop, where does it
// stop, and does it look at the clock more than once.

static unsigned s_now;
static unsigned s_step_per_read;

static unsigned fakeClock(void)
{
	const unsigned t = s_now;
	s_now += s_step_per_read;
	return t;
}

static int s_columns_left;   // how many the step still has to give
static int s_step_calls;     // how many times sleep.c asked for one

static bool fakeStep(void)
{
	s_step_calls++;
	if (s_columns_left <= 0) return false;
	s_columns_left--;
	return true;
}

// Ordering witness: the leave hook records the flush-step call count it saw when it ran,
// so "the network leave happens before the first column is written" is a number rather
// than a reading of the source.
static int s_leave_calls;
static int s_steps_at_leave;

static void fakeLeave(void)
{
	s_leave_calls++;
	s_steps_at_leave = s_step_calls;
}

static void reset(unsigned now, unsigned step_per_read, int columns)
{
	s_now           = now;
	s_step_per_read = step_per_read;
	s_columns_left  = columns;
	s_step_calls    = 0;
	s_leave_calls   = 0;
	s_steps_at_leave = -1;
	sleepSetFlushHook(fakeStep);
	sleepSetLeaveHook(fakeLeave);
	sleepOnWake();
}

// ── The flag, which is all the old file tested ─────────────────────────────────────────

static void testInitialAwake(void)
{
	reset(0, 0, 0);
	CHECK(!sleepIsAsleep());
}

static void testSleepSetsAsleep(void)
{
	reset(0, 0, 0);
	sleepOnSleep(fakeClock);
	CHECK(sleepIsAsleep());
}

static void testWakeClearsAsleep(void)
{
	reset(0, 0, 0);
	sleepOnSleep(fakeClock);
	sleepOnWake();
	CHECK(!sleepIsAsleep());
}

static void testRepeatedWakeIdempotent(void)
{
	reset(0, 0, 0);
	sleepOnSleep(fakeClock);
	sleepOnWake();
	sleepOnWake();
	CHECK(!sleepIsAsleep());
}

// ── The bounded flush, which is the v1.6.0 behaviour ───────────────────────────────────

static void testFlushDrainsWhenBudgetNeverSpent(void)
{
	// A clock that never moves can never spend a budget, so the loop must run until the
	// step says there is nothing left — 7 columns, 8 calls, the last one reporting empty.
	reset(0, 0, 7);
	const int saved = sleepFlushBounded(fakeStep, fakeClock, 500u);
	CHECK(saved == 7);
	CHECK(s_step_calls == 8);
	CHECK(s_columns_left == 0);
}

static void testFlushStopsOnBudget(void)
{
	// 200 ms a read against a 500 ms budget: after the third column the clock reads 600,
	// which is the first reading at or past the budget, so exactly three are written and
	// the remaining 97 are left for the next lid-close.
	reset(0, 200, 100);
	const int saved = sleepFlushBounded(fakeStep, fakeClock, 500u);
	CHECK(saved == 3);
	CHECK(s_columns_left == 97);
}

static void testBudgetIsCheckedBetweenColumnsNotOnce(void)
{
	// The bug this file exists to catch: a budget read once at the start cannot stop a
	// loop, because at the start nothing has been spent. Here the very first read after
	// the start jumps the clock a whole minute — a check made only at the top would save
	// all 50 columns, and a check made between columns saves exactly one.
	reset(0, 60000u, 50);
	const int saved = sleepFlushBounded(fakeStep, fakeClock, 500u);
	CHECK(saved == 1);
	CHECK(s_columns_left == 49);
}

static void testBudgetIsCheckedAfterTheWriteNotBefore(void)
{
	// The other half of the same claim. A clock already past the budget on its second read
	// must still leave one column written: checking before the step would write none, and
	// "flush nothing at all whenever the card is slow" is not a flush.
	reset(0, 500u, 50);
	const int saved = sleepFlushBounded(fakeStep, fakeClock, 500u);
	CHECK(saved == 1);
}

static void testBudgetSurvivesClockWrap(void)
{
	// Unsigned subtraction, so a clock that rolls over mid-flush measures a difference
	// rather than four billion milliseconds and stopping on the first column. Starts at
	// UINT_MAX-300 and steps 200, so the three readings after the start are UINT_MAX-100
	// (200 elapsed), 99 — wrapped — (400 elapsed) and 299 (600 elapsed). Only the last is
	// at or past the 500 budget, so three columns are written. Signed or naive arithmetic
	// would stop at one.
	reset(UINT_MAX - 300u, 200u, 50);
	const int saved = sleepFlushBounded(fakeStep, fakeClock, 500u);
	CHECK(saved == 3);
}

static void testFlushRefusesNullStep(void)
{
	reset(0, 200, 50);
	CHECK(sleepFlushBounded(NULL, fakeClock, 500u) == 0);
	CHECK(s_step_calls == 0);
}

static void testFlushRefusesNullClock(void)
{
	// A clock-less caller has no stop condition, so the loop must not start at all rather
	// than run the step until the columns happen to run out.
	reset(0, 200, 50);
	CHECK(sleepFlushBounded(fakeStep, NULL, 500u) == 0);
	CHECK(s_step_calls == 0);
	CHECK(s_columns_left == 50);
}

// ── What ONSLEEP wires together ────────────────────────────────────────────────────────

static void testSleepUsesTheHeaderBudget(void)
{
	// sleepOnSleep must spend SLEEP_FLUSH_BUDGET_MS and not some other number of its own.
	// One millisecond a read means the column count IS the budget, so this check fails the
	// moment the two disagree — including if the header constant is changed without the
	// call site following it.
	reset(0, 1u, 100000);
	sleepOnSleep(fakeClock);
	CHECK(s_step_calls == (int)SLEEP_FLUSH_BUDGET_MS);
}

static void testSleepRunsTheLeaveHookExactlyOnce(void)
{
	reset(0, 0, 3);
	sleepOnSleep(fakeClock);
	CHECK(s_leave_calls == 1);
}

static void testLeaveHappensBeforeAnyColumnIsWritten(void)
{
	// The Wi-Fi is being torn down around us and the leave is one datagram; queueing it
	// behind up to SLEEP_FLUSH_BUDGET_MS of card writes would be a good way to never send
	// it. Recorded as "how many columns had been asked for when the leave ran", which is
	// zero or the ordering is wrong.
	reset(0, 0, 5);
	sleepOnSleep(fakeClock);
	CHECK(s_steps_at_leave == 0);
}

static void testSleepWithNoHooksIsSafe(void)
{
	// The title screen and the world list: main.c has un-registered both hooks because
	// there is no world and no worker. Closing the lid there must still track the flag and
	// must not reach for either callback.
	sleepSetFlushHook(NULL);
	sleepSetLeaveHook(NULL);
	s_step_calls  = 0;
	s_leave_calls = 0;
	s_now = 0;
	s_step_per_read = 0;
	sleepOnWake();
	sleepOnSleep(fakeClock);
	CHECK(sleepIsAsleep());
	CHECK(s_step_calls == 0);
	CHECK(s_leave_calls == 0);
	sleepOnWake();
	CHECK(!sleepIsAsleep());
}

static void testSleepWithNoClockStillLeavesAndStillTracks(void)
{
	// A caller with no clock loses the flush and keeps everything else. Nothing in the app
	// does this, but it is the difference between "refuses to loop" and "refuses to run",
	// and the leave is the half that has no budget to blow.
	reset(0, 0, 5);
	sleepOnSleep(NULL);
	CHECK(sleepIsAsleep());
	CHECK(s_leave_calls == 1);
	CHECK(s_step_calls == 0);
}

static void testHooksCanBeUnregistered(void)
{
	// main.c clears both before workerStop(); past that line s_world is about to be freed
	// and the worker joined, so a hook that survived would be a crash on lid-close.
	reset(0, 0, 5);
	sleepSetFlushHook(NULL);
	sleepSetLeaveHook(NULL);
	sleepOnSleep(fakeClock);
	CHECK(s_step_calls == 0);
	CHECK(s_leave_calls == 0);
}

static void testHooksSurviveASleepWakeCycle(void)
{
	// Registered once per world in genStart(), not once per lid-close, so the second sleep
	// has to work exactly like the first.
	reset(0, 0, 5);
	sleepOnSleep(fakeClock);
	sleepOnWake();
	const int after_first = s_step_calls;
	sleepOnSleep(fakeClock);
	sleepOnWake();
	CHECK(after_first == 6);          // 5 columns, then the empty report
	CHECK(s_step_calls == 7);         // second sleep finds nothing and asks once
	CHECK(s_leave_calls == 2);
	CHECK(!sleepIsAsleep());
}

int main(void)
{
	testInitialAwake();
	testSleepSetsAsleep();
	testWakeClearsAsleep();
	testRepeatedWakeIdempotent();

	testFlushDrainsWhenBudgetNeverSpent();
	testFlushStopsOnBudget();
	testBudgetIsCheckedBetweenColumnsNotOnce();
	testBudgetIsCheckedAfterTheWriteNotBefore();
	testBudgetSurvivesClockWrap();
	testFlushRefusesNullStep();
	testFlushRefusesNullClock();

	testSleepUsesTheHeaderBudget();
	testSleepRunsTheLeaveHookExactlyOnce();
	testLeaveHappensBeforeAnyColumnIsWritten();
	testSleepWithNoHooksIsSafe();
	testSleepWithNoClockStillLeavesAndStillTracks();
	testHooksCanBeUnregistered();
	testHooksSurviveASleepWakeCycle();

	printf("sleep lid-close behaviour: %s\n",
	       s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n",
		       s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif /* !__3DS__ */
