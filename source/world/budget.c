#include "world/budget.h"

#include <stdio.h>

// ── Why these are atomic ──────────────────────────────────────────────────────────────
//
// Two threads claim from this counter, and the handshake in app/worker.h does NOT keep them
// apart the way its own comment describes. That comment is about the *staging* world, and it
// is right about that: the worker generates only while nothing is waiting to be installed,
// and the main thread copies only while the worker is parked. But the main thread also
// allocates outside that handshake — worldSet -> worldChunkCreate, every time the player
// places a block into a chunk that does not exist yet, which is what happens the moment they
// build upwards into the sky chunks worldgenColumn deliberately leaves unallocated. That call
// can land in the middle of the worker's own claim.
//
// A plain `s_used += bytes` is a load, an add and a store. Two of those interleaved lose one
// allocation's worth of accounting, permanently, in a counter whose whole job is to stop the
// game from running out of the 12 MB it is allowed. The failure is silent and it accumulates:
// every lost claim raises the real high-water mark above the recorded one until an allocation
// the budget thinks is fine returns NULL and a column arrives with a hole in it.
//
// So the claim is a compare-and-swap loop instead: read the counter, work out the new value,
// and publish it only if nobody moved it in between. Release is the same shape. `s_peak` and
// `s_refusals` are statistics rather than gates, and are relaxed on purpose — losing an
// increment there costs a number in a debug overlay, not a block.
static size_t s_used;
static size_t s_peak;
static int    s_refusals;

bool budgetClaim(size_t bytes)
{
	size_t old = __atomic_load_n(&s_used, __ATOMIC_RELAXED);
	size_t next;

	do {
		if (bytes > WORLD_BUDGET_BYTES - old) {
			__atomic_fetch_add(&s_refusals, 1, __ATOMIC_RELAXED);
			printf("BUDGET REFUSED %lu B (used %lu of %lu)\n",
			       (unsigned long)bytes, (unsigned long)old,
			       (unsigned long)WORLD_BUDGET_BYTES);
			return false;
		}
		next = old + bytes;
		// Weak, because the loop already retries: a spurious failure just re-reads `old`,
		// which the strong form would do internally at the same cost.
	} while (!__atomic_compare_exchange_n(&s_used, &old, next, true,
	                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));

	size_t peak = __atomic_load_n(&s_peak, __ATOMIC_RELAXED);
	while (next > peak &&
	       !__atomic_compare_exchange_n(&s_peak, &peak, next, true,
	                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
	return true;
}

void budgetRelease(size_t bytes)
{
	size_t old = __atomic_load_n(&s_used, __ATOMIC_RELAXED);
	size_t next;

	do {
		// Clamped rather than wrapped: an unsigned underflow here would read as a
		// 4 GB world and hide the real bug.
		next = (bytes > old) ? 0 : old - bytes;
	} while (!__atomic_compare_exchange_n(&s_used, &old, next, true,
	                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
}

size_t budgetUsed(void)   { return s_used; }
size_t budgetPeak(void)   { return s_peak; }
size_t budgetCap(void)    { return WORLD_BUDGET_BYTES; }
int    budgetRefusals(void) { return s_refusals; }

void budgetReset(void)
{
	s_used = 0;
	s_peak = 0;
	s_refusals = 0;
}
