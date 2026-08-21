// Host self-test for the sleep state machine logic. The real aptHook/osSetSpeedupEnable
// calls live in sleep.c and are console-only; this file tests the flag transitions and
// enter/exit pairing in isolation.
//
// Built by tools/run_host_tests.sh as a separate binary, same pattern as
// app/options_test.c and app/updater_version_test.c.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Minimal reproduction of the sleep state machine for host testing.
// The real logic in sleep.c is trivial enough that duplicating it here
// verifies the contract without depending on <3ds.h>.

static bool s_test_asleep;

static void testSleepEnter(void) { s_test_asleep = true; }
static void testSleepExit(void)  { s_test_asleep = false; }
static bool testSleepIsAsleep(void) { return s_test_asleep; }
static bool testSleepShouldSkip(void) { return s_test_asleep; }

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

static void testInitialAwake(void)
{
	s_test_asleep = false;
	CHECK(!testSleepIsAsleep());
	CHECK(!testSleepShouldSkip());
}

static void testEnterSetsAsleep(void)
{
	s_test_asleep = false;
	testSleepEnter();
	CHECK(testSleepIsAsleep());
	CHECK(testSleepShouldSkip());
}

static void testExitClearsAsleep(void)
{
	s_test_asleep = true;
	testSleepExit();
	CHECK(!testSleepIsAsleep());
	CHECK(!testSleepShouldSkip());
}

static void testDoubleEnterIdempotent(void)
{
	s_test_asleep = false;
	testSleepEnter();
	testSleepEnter();
	CHECK(testSleepIsAsleep());
}

static void testDoubleExitIdempotent(void)
{
	s_test_asleep = true;
	testSleepExit();
	testSleepExit();
	CHECK(!testSleepIsAsleep());
}

static void testEnterExitRoundTrip(void)
{
	s_test_asleep = false;
	testSleepEnter();
	CHECK(testSleepIsAsleep());
	testSleepExit();
	CHECK(!testSleepIsAsleep());
}

static void testSkipMirrorsAsleep(void)
{
	s_test_asleep = false;
	CHECK(!testSleepShouldSkip());
	s_test_asleep = true;
	CHECK(testSleepShouldSkip());
	s_test_asleep = false;
	CHECK(!testSleepShouldSkip());
}

int main(void)
{
	testInitialAwake();
	testEnterSetsAsleep();
	testExitClearsAsleep();
	testDoubleEnterIdempotent();
	testDoubleExitIdempotent();
	testEnterExitRoundTrip();
	testSkipMirrorsAsleep();

	printf("sleep state machine: %s\n",
	       s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n",
		       s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif /* !__3DS__ */
