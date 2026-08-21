// Host self-test for the battery bar-mapping and low-battery logic. The real
// PTMU calls live in battery.c and are console-only; this file tests the pure
// arithmetic in batteryBars() and batteryLow() in isolation.
//
// Built by tools/run_host_tests.sh as a separate binary, same pattern as
// app/options_test.c and app/updater_version_test.c.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int  s_level;
static bool s_charging;

static int testBars(void)
{
	if (s_level <= 0) return 0;
	if (s_level == 1) return 1;
	if (s_level == 2) return 2;
	if (s_level <= 4) return 3;
	return 4;
}

static bool testLow(void)
{
	return s_level <= 1 && !s_charging;
}

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

static void testBarMapping(void)
{
	const struct { int level; int expected; } cases[] = {
		{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 3}, {5, 4},
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		s_level = cases[i].level;
		s_charging = false;
		CHECK(testBars() == cases[i].expected);
	}
}

static void testLowBattery(void)
{
	s_charging = false;

	s_level = 0;
	CHECK(testLow());

	s_level = 1;
	CHECK(testLow());

	s_level = 2;
	CHECK(!testLow());

	s_level = 5;
	CHECK(!testLow());
}

static void testLowBatteryCharging(void)
{
	s_charging = true;

	s_level = 0;
	CHECK(!testLow());

	s_level = 1;
	CHECK(!testLow());
}

static void testEdgeCases(void)
{
	s_charging = false;

	s_level = -1;
	CHECK(testBars() == 0);
	CHECK(testLow());

	s_level = 6;
	CHECK(testBars() == 4);
	CHECK(!testLow());
}

int main(void)
{
	testBarMapping();
	testLowBattery();
	testLowBatteryCharging();
	testEdgeCases();

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
