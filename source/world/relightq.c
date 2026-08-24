#include "world/relightq.h"

#include <string.h>

void relightqInit(RelightQueue* q)
{
	if (!q) return;
	memset(q, 0, sizeof(*q));
}

bool relightqPush(RelightQueue* q, int cx, int cz)
{
	if (!q) return false;

	// Linear dedup over at most RELIGHTQ_CAP entries. A hash would be faster in theory and
	// the set is 64 wide in practice — the whole scan is a handful of microseconds, against
	// the milliseconds one avoided relight saves, which is the only comparison that matters.
	const int16_t x = (int16_t)cx, z = (int16_t)cz;
	for (int i = 0; i < q->count; i++) {
		if (q->cx[i] == x && q->cz[i] == z) {
			q->coalesced++;
			return true;
		}
	}

	if (q->count >= RELIGHTQ_CAP) { q->overflows++; return false; }

	q->cx[q->count] = x;
	q->cz[q->count] = z;
	q->count++;
	return true;
}

bool relightqPop(RelightQueue* q, int* cx, int* cz)
{
	if (!q || q->count <= 0) return false;

	// Off the back, so removing an entry never shuffles the rest.
	q->count--;
	if (cx) *cx = q->cx[q->count];
	if (cz) *cz = q->cz[q->count];
	return true;
}

int relightqCount(const RelightQueue* q)      { return q ? q->count : 0; }
int relightqCoalesced(const RelightQueue* q)  { return q ? q->coalesced : 0; }
int relightqOverflows(const RelightQueue* q)  { return q ? q->overflows : 0; }
