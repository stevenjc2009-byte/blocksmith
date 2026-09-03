#pragma once

// v1.8.16 FRZ-FIX. The wait for a free save slot, extracted from app/worker.c so it can be
// linked into a host suite.
//
// worker.c itself cannot be: it is LightLock, LightEvent, svcSleepThread and threadCreate all
// the way down, which is why tools/run_host_tests.sh recorded the duration of this wait as
// "NOT MEASURED, and not measurable from here". This module is the POLICY half of it — the
// deadline, the pump threshold and the loop — with the console half behind three function
// pointers.
//
// It is the REAL code the console runs, not a model of it: worker.c has no loop of its own
// left, and tests/savewait_guard_test.c asserts that by reading worker.c as text. Without that
// second file this one could be perfect and worker.c could still ship its own `for (;;)`.

#include <stdbool.h>
#include <stdint.h>

// 250 ms is ~15 frames. A healthy save is a few ms, so this cannot fire on one; a quarter
// second of stall is already far past anything a player would call a hitch, so waiting longer
// buys nothing.
#define SAVEWAIT_BUDGET_MS 250u

// One frame. Below this the wait is an ordinary SD write and APT must NOT be re-entered from
// the middle of a frame; above it the console is already visibly stalling and keeping HOME
// alive matters more than the purity of the frame.
#define SAVEWAIT_PUMP_MS 32u

typedef struct {
	bool     (*room)(void* ud);             // is there a free slot / is the ring empty?
	void     (*idle)(void* ud, bool pump);  // sleep ~1 ms; also service APT if pump
	uint32_t (*now_ms)(void* ud);
	void*      ud;
	uint32_t   budget_ms;                   // 0 = no deadline (the flush path)
} SaveWait;

typedef enum {
	SAVEWAIT_ROOM = 0,    // a slot came free
	SAVEWAIT_EXPIRED,     // budget_ms elapsed and there is still no room
} SaveWaitResult;

// Never returns without either room or an expired budget. waited_ms and iters may be NULL.
SaveWaitResult saveWaitForRoom(const SaveWait* w, uint32_t* waited_ms, uint32_t* iters);
