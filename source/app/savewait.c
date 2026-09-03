// See savewait.h for why this file exists apart from app/worker.c.
#include "app/savewait.h"

SaveWaitResult saveWaitForRoom(const SaveWait* w, uint32_t* waited_ms, uint32_t* iters)
{
	const uint32_t t0 = w->now_ms(w->ud);
	uint32_t n = 0, waited = 0;

	for (;;) {
		if (w->room(w->ud)) break;

		waited = w->now_ms(w->ud) - t0;

		// The deadline. Compiled out by the red arm (see tests/savewait_test.c), which is the
		// only way "this loop terminates" has ever been shown to be a claim that CAN be false.
#ifndef SAVEWAIT_NO_DEADLINE
		if (w->budget_ms && waited >= w->budget_ms) {
			if (waited_ms) *waited_ms = waited;
			if (iters)     *iters     = n;
			return SAVEWAIT_EXPIRED;
		}
#endif

		// APT only once the wait is already visible. Below SAVEWAIT_PUMP_MS this is an ordinary
		// SD write and the caller's console half must not re-enter APT from mid-frame.
		w->idle(w->ud, waited >= SAVEWAIT_PUMP_MS);
		n++;
	}

	waited = w->now_ms(w->ud) - t0;
	if (waited_ms) *waited_ms = waited;
	if (iters)     *iters     = n;
	return SAVEWAIT_ROOM;
}
