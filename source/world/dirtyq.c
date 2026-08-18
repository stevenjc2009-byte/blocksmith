#include "world/dirtyq.h"

#include <stddef.h>   // NULL

// Out-of-range indices are refused rather than clamped or asserted. A bad index here can
// only come from a caller iterating a pool that no longer matches the capacity, and in
// that case silently marking slot 0 instead would corrupt a real chunk's state; returning
// false leaves the queue untouched and the caller's "queued" tally visibly short.
static bool inRange(const DirtyQ* q, int i)
{
	return q != NULL && i >= 0 && i < q->capacity;
}

bool dirtyqInit(DirtyQ* q, int capacity)
{
	if (q == NULL) return false;

	for (int i = 0; i < DIRTYQ_MAX; i++)
		q->marked[i] = false;

	q->count = 0;
	q->peak  = 0;

	if (capacity < 0 || capacity > DIRTYQ_MAX) {
		q->capacity = 0;
		return false;
	}

	q->capacity = capacity;
	return true;
}

bool dirtyqMark(DirtyQ* q, int i)
{
	if (!inRange(q, i) || q->marked[i])
		return false;

	q->marked[i] = true;
	q->count++;
	if (q->count > q->peak)
		q->peak = q->count;

	return true;
}

bool dirtyqClear(DirtyQ* q, int i)
{
	if (!inRange(q, i) || !q->marked[i])
		return false;

	q->marked[i] = false;
	q->count--;

	return true;
}

bool dirtyqIsMarked(const DirtyQ* q, int i)
{
	return inRange(q, i) && q->marked[i];
}

int dirtyqCount(const DirtyQ* q) { return q ? q->count : 0; }
int dirtyqPeak(const DirtyQ* q)  { return q ? q->peak  : 0; }

void dirtyqResetPeak(DirtyQ* q)
{
	if (q) q->peak = q->count;
}

bool dirtyqConsistent(const DirtyQ* q)
{
	if (q == NULL) return false;
	if (q->capacity < 0 || q->capacity > DIRTYQ_MAX) return false;

	int set = 0;
	for (int i = 0; i < q->capacity; i++)
		if (q->marked[i]) set++;

	// Anything outside the capacity must be clear too, or a capacity change would hide
	// stale flags that a later widening would resurrect.
	for (int i = q->capacity; i < DIRTYQ_MAX; i++)
		if (q->marked[i]) return false;

	return q->count == set && q->peak >= q->count;
}
