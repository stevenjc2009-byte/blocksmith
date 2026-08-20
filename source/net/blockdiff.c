#include "net/blockdiff.h"

#include <string.h>

// Fixed capacity, no heap use — see blockdiff.h for why.
//
// Every entry lives in exactly one singly-linked chain, threaded through BlockDiffEntry.next:
// either the chain for the column-hash bucket its coordinate falls in, or the free list. That
// is the whole reason BLOCKDIFF_MAX_PENDING can be 65536 (matching the server) instead of the
// 256 it used to be. The previous implementation scanned all BLOCKDIFF_MAX_PENDING slots twice
// per record() and once per drain(); at 65536 slots, absorbing a full join sync would have been
// on the order of 4.3 billion integer comparisons — the console would simply have stopped.
// With chaining, both calls touch only the diffs already pending for the one column involved.

// Maps a column coordinate to a bucket. Multiplicative hash on both axes then a fold, so that
// the adjacent columns a streaming ring loads together do NOT land in one chain — the obvious
// (cx ^ cz) & mask does exactly that, and would put a whole loaded region on a single chain.
static uint32_t columnBucket(int cx, int cz)
{
	uint32_t h = ((uint32_t)cx * 0x9E3779B1u) ^ ((uint32_t)cz * 0x85EBCA77u);
	h ^= h >> 15;
	return h & (uint32_t)(BLOCKDIFF_BUCKETS - 1);
}

// Takes a slot off the free list, or bumps into never-yet-used territory. Returns BLOCKDIFF_NIL
// only when both are exhausted, which blockdiffRecord()'s count check already rules out.
static uint32_t allocSlot(BlockDiffStore* s)
{
	if (s->free_head != BLOCKDIFF_NIL) {
		const uint32_t i = s->free_head;
		s->free_head = s->entry[i].next;
		return i;
	}
	if (s->high_water < (uint32_t)BLOCKDIFF_MAX_PENDING) return s->high_water++;
	return BLOCKDIFF_NIL;
}

void blockdiffClear(BlockDiffStore* s)
{
	// BLOCKDIFF_NIL is 0xFFFFFFFF, so a byte fill empties every bucket chain. Deliberately
	// does NOT touch s->entry: that is 1 MB, and with every chain head reset and high_water
	// back to zero, nothing in it is reachable. Whatever bytes are left there are re-written
	// before they can be read, by the field-by-field fill in blockdiffRecord().
	memset(s->bucket, 0xFF, sizeof s->bucket);

	s->free_head  = BLOCKDIFF_NIL;
	s->high_water = 0;
	s->count      = 0;
	s->refusals   = 0;
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

	const uint32_t b = columnBucket(x >> 4, z >> 4);

	// Latest write wins: a diff already queued for this exact coordinate is overwritten in
	// place. This must happen before the capacity check below — it never grows the store, so
	// it must never be refused for being full. Only this column's chain is walked; y is
	// compared first because it is the field most likely to differ between two diffs in the
	// same column, so most entries reject on the first comparison.
	for (uint32_t i = s->bucket[b]; i != BLOCKDIFF_NIL; i = s->entry[i].next) {
		BlockDiffEntry* e = &s->entry[i];
		if (e->y == (uint8_t)y && e->x == x && e->z == z) {
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
	//
	// With the cap now matching the server's BS_DIFF_MAX this should be unreachable in
	// practice: the server cannot hold, and so cannot replay, more diffs than fit here.
	if (s->count >= BLOCKDIFF_MAX_PENDING) {
		s->refusals++;
		return false;
	}

	const uint32_t slot = allocSlot(s);
	if (slot == BLOCKDIFF_NIL) {
		// Unreachable: s->count < BLOCKDIFF_MAX_PENDING guarantees a slot was available.
		s->refusals++;
		return false;
	}

	BlockDiffEntry* e = &s->entry[slot];
	e->x    = x;
	e->z    = z;
	e->y    = (uint8_t)y;
	e->id   = id;
	e->_pad = 0;

	e->next      = s->bucket[b];
	s->bucket[b] = slot;
	s->count++;
	return true;
}

int blockdiffDrain(BlockDiffStore* s, int cx, int cz, BlockDiffApplyFn apply, void* userdata)
{
	const uint32_t b = columnBucket(cx, cz);
	int drained = 0;

	// `link` is where the current entry's index is stored — the bucket head for the first
	// entry, the previous entry's next field after that — so an entry can be unlinked without
	// a separate "previous index or none" special case. Entries belonging to a different
	// column that happens to hash to this bucket are skipped, not drained.
	uint32_t* link = &s->bucket[b];
	uint32_t  i    = s->bucket[b];

	while (i != BLOCKDIFF_NIL) {
		BlockDiffEntry* e    = &s->entry[i];
		const uint32_t  next = e->next;

		if ((e->x >> 4) != cx || (e->z >> 4) != cz) {
			link = &e->next;
			i    = next;
			continue;
		}

		if (apply) apply(userdata, e->x, (int)e->y, e->z, e->id);

		*link        = next;          // splice out of the bucket chain
		e->next      = s->free_head;  // and onto the free list
		s->free_head = i;
		s->count--;
		drained++;

		i = next;
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
