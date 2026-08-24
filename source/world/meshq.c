#include "world/meshq.h"

#include "world/chunk.h"

bool meshqPushColumn(JobQueue* q, const World* w, int32_t cx, int32_t cz, int* refused)
{
	bool all_pushed = true;

	for (int32_t cy = 0; cy < COLUMN_CHUNKS; cy++) {
		const Chunk* c = worldChunk(w, cx, cy, cz);
		if (!c || chunkIsAllAir(c)) continue;

		const Job j = {JOB_MESH, cx, cz, cy};
		if (jobqPush(q, j)) continue;

		// Refused. Keep going rather than breaking out: the remaining pushes will refuse too
		// (the ring only empties on a pop, and nothing pops in here), but counting them is
		// what turns "this column did not fit" into "this column was N chunks short", which
		// is the number main.c reports as mesh_refused.
		if (refused) (*refused)++;
		all_pushed = false;
	}

	// The whole point of this function. False here is what keeps the column unmarked and
	// therefore re-queueable — see the header for the ordering bug this replaces.
	return all_pushed;
}
