// Host test runner: compiles the world data code with the PC's gcc and runs the
// same assertions the console runs. Build and run it with tools/run_host_tests.sh.
//
// This exists because the world code has no 3DS dependencies, so a broken invariant
// can be found in a second here instead of a rebuild, an emulator launch and a
// screenshot. It is not a substitute for the on-console run — the console uses a
// different compiler and different alignment rules, and that run is what the phase
// gate reports.
#include <stdio.h>

#include "world/world_test.h"

int main(void)
{
	char summary[96] = "";
	int checks = 0;
	const int fails = worldTestRun(summary, sizeof(summary), &checks);

	printf("world self-test: %s\n", summary);
	if (fails)
		printf("FAILED - %d of %d checks\n", fails, checks);

	return fails ? 1 : 0;
}
