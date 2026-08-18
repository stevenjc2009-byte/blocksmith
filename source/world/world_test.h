// The world-data self-test.
//
// One set of assertions, two places to run it: `tests/host_test.c` compiles it with
// host gcc so a broken invariant is visible in a second, and the app calls it on
// the console so the same invariants are checked on ARM with the real compiler and
// the real alignment rules. Sharing the file is the point — two copies would drift.
//
// It resets the memory budget when it starts and again when it finishes, so call it
// **before** building the real world, not after.
#pragma once

#include <stddef.h>

// Returns the number of failed checks. `summary` receives a one-line result, and
// on failure the first failing check's file:line and expression. `checks_out` may
// be NULL; otherwise it receives the number of checks run.
int worldTestRun(char* summary, size_t cap, int* checks_out);
