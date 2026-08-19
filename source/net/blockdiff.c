#include "net/blockdiff.h"

#include <string.h>

// Fixed capacity, no heap use — see blockdiff.h for why. Both blockdiffRecord() and
// blockdiffDrain() do a full linear scan of the array; at BLOCKDIFF_MAX_PENDING entries
// that is at most a few hundred integer comparisons, which is why the cap is kept small
// rather than let the scan cost grow with it.

void blockdiffClear(BlockDiffStore* s)
{
	memset(s, 0, sizeof(*s));
}

void blockdiffInit(BlockDiffStore* s)
{
	blockdiffClear(s);
}

void blockdiffFree(BlockDiffStore* s)
{
	blockdiffClear(s);
}

bool blockdiffRecord(BlockDiffStore* s, int x, int y, int z, BlockId id)
{
	if (y < 0 || y >= WORLD_HEIGHT) return false;

	// Latest write wins: a diff already queued for this exact coordinate is overwritten in
	// place. This must happen before the capacity check below — it never grows the store,
	// so it must never be refused for being full.
	for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
		BlockDiffEntry* e = &s->entry[i];
		if (e->used && e->x == x && e->y == y && e->z == z) {
			e->id = id;
			return true;
		}
	}

	// The store is full and this is a genuinely new coordinate: refuse rather than evict.
	//
	// Evicting some other pending diff to make room would silently corrupt a column that
	// nothing is currently wrong with — it would come up missing an edit nobody asked to
	// remove, the moment it loads, with no record that it ever happened. Refusing the new
	// diff instead loses at most this one edit, does so loudly (blockdiffRefusals() counts
	// it, so the caller can log or surface it), and leaves every previously accepted diff
	// exactly as it was. That mirrors how the rest of this codebase treats a full resource
	// table: world/budget.c's budgetClaim() refuses an over-cap allocation and counts the
	// refusal rather than freeing something else to make room, and worldColumnCreate()
	// returns NULL when the column table is full rather than evicting a live column.
	if (s->count >= BLOCKDIFF_MAX_PENDING) {
		s->refusals++;
		return false;
	}

	for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
		BlockDiffEntry* e = &s->entry[i];
		if (e->used) continue;

		e->x = x;
		e->y = y;
		e->z = z;
		e->id = id;
		e->used = true;
		s->count++;
		return true;
	}

	// Unreachable: s->count < BLOCKDIFF_MAX_PENDING guarantees a free slot was found above.
	s->refusals++;
	return false;
}

int blockdiffDrain(BlockDiffStore* s, int cx, int cz, BlockDiffApplyFn apply, void* userdata)
{
	int drained = 0;

	for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
		BlockDiffEntry* e = &s->entry[i];
		if (!e->used) continue;
		if ((e->x >> 4) != cx || (e->z >> 4) != cz) continue;

		if (apply) apply(userdata, e->x, e->y, e->z, e->id);
		e->used = false;
		s->count--;
		drained++;
	}

	return drained;
}

int blockdiffCount(const BlockDiffStore* s)
{
	return s->count;
}

int blockdiffRefusals(const BlockDiffStore* s)
{
	return s->refusals;
}
