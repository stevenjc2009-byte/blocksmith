#include "world/jobq.h"

#include <string.h>

void jobqInit(JobQueue* q)
{
	memset(q, 0, sizeof(*q));
}

bool jobqPush(JobQueue* q, Job job)
{
	if (q->count >= JOBQ_CAP) {
		q->dropped++;
		return false;
	}

	q->slots[q->tail] = job;
	q->tail = (q->tail + 1) % JOBQ_CAP;
	q->count++;
	q->pushed++;
	if (q->count > q->peak) q->peak = q->count;
	return true;
}

bool jobqPop(JobQueue* q, Job* out)
{
	if (q->count == 0)
		return false;

	*out = q->slots[q->head];
	q->head = (q->head + 1) % JOBQ_CAP;
	q->count--;
	return true;
}

int jobqCount(const JobQueue* q)   { return q->count; }
int jobqPeak(const JobQueue* q)    { return q->peak; }
int jobqPushed(const JobQueue* q)  { return q->pushed; }
int jobqDropped(const JobQueue* q) { return q->dropped; }

bool jobqConsistent(const JobQueue* q)
{
	if (q->count < 0 || q->count > JOBQ_CAP) return false;
	if (q->head < 0 || q->head >= JOBQ_CAP)  return false;
	if (q->tail < 0 || q->tail >= JOBQ_CAP)  return false;
	if (q->peak < q->count)                  return false;

	// The ring's own invariant: walking `count` slots forward from head lands on tail.
	// This is what catches a push or a pop that moved one index and not the other, which
	// is the ring-buffer version of the dirty queue's double-decrement.
	return (q->head + q->count) % JOBQ_CAP == q->tail;
}
