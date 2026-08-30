#include "world/relight_drain.h"

#include "world/light.h"

int relightDrain(World* w, RelightQueue* q, int max_columns,
                 uint64_t budget_ticks, RelightClockFn now)
{
	if (!w || !q || !now) return 0;

	// Asked here rather than at the call site so the module is whole: a drain that relit
	// columns with the engine off would attach 32 KiB of light per column to a build that
	// has deliberately compiled lighting out (the host suite and the dedicated server both
	// run with it off).
	if (!lightEnabled()) return 0;

	const uint64_t t0 = now();

	int done = 0;
	int cx, cz;
	while (relightqPop(q, &cx, &cz)) {
		// Popped and relit in the same breath, always. This pairing is the "nothing is
		// dropped" guarantee: there is no path here that removes an entry and does not
		// then act on it, so a column that this call does not reach is still listed for
		// the next one. Dropping it instead would leave the column lit for a world that
		// no longer exists, with nothing left anywhere to say so.
		lightRelightColumn(w, cx, cz);
		done++;

		// Both limits, checked AFTER the relight and BEFORE the next pop, the same way
		// scene/chunk_render.c:1259 chunkRenderDrainDirty checks its two. After, because
		// a budget that is already spent must still let the column above finish or the
		// queue never drains at all. Before the next pop, because the pop is what would
		// take the entry out of the set.
		//
		// Per iteration, not once: checking a clock once at the top bounds nothing — the
		// whole cost is inside the loop, and by the time the first check runs none of it
		// has been paid yet.
		if (done >= max_columns) break;
		if (now() - t0 >= budget_ticks) break;
	}

	return done;
}
