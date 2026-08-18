// A FIFO of typed world jobs.
//
// Phase 5 up to step 5.4 did all of its work before the first frame: generate 25 columns,
// mesh them, then start the loop. That is fine for a fixed 5x5 area and impossible for
// Phase 6, where columns arrive and leave while the player walks. Step 5.5 turns the work
// into jobs so it can be handed to a worker thread and spread over frames.
//
// The four types are the ones the world will ever need, and they are declared together
// even though this step only executes two of them, because the *routing* is the thing
// being built: a job carries where it applies and what to do there, and the code that
// pushes it does not care which thread ends up running it.
//
//   JOB_GENERATE  a column of terrain from the seed        -> worker thread
//   JOB_MESH      one chunk into a GPU buffer              -> main thread (touches the GPU)
//   JOB_LOAD      a column off the SD card                 -> Phase 8
//   JOB_SAVE      a column onto the SD card                -> Phase 8
//
// This file is the bookkeeping only: a fixed-capacity ring with no locking and no
// allocation, so it can be tested on the host in a second. Locking belongs to whoever
// shares it across threads (app/worker.c wraps this one in a LightLock) — the same split
// as world/dirtyq.c, and for the same reason: the queue that shipped a bug in Phase 4 was
// the one that could not be host-tested.
//
// No <3ds.h>, no floats, no allocation — same rules as the rest of source/world.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	JOB_NONE = 0,
	JOB_GENERATE,
	JOB_MESH,
	JOB_LOAD,
	JOB_SAVE,
} JobType;

typedef struct {
	JobType type;
	int32_t cx, cz;   // column coordinate; all four types apply to a column
	int32_t cy;       // chunk within the column, JOB_MESH only; 0 otherwise
} Job;

// 128 slots = 2 KB. The boot submission is 25 generate jobs and the meshed 3x3 columns are
// at most 72 chunks, so nothing in this phase comes close; Phase 6 pushes per frame and
// drains per frame, which keeps the live count small rather than growing it.
#define JOBQ_CAP 128

typedef struct {
	Job slots[JOBQ_CAP];
	int head;      // next to pop
	int tail;      // next free
	int count;
	int peak;      // high-water mark of count since jobqInit
	int pushed;    // lifetime accepted
	int dropped;   // lifetime refused because the ring was full
} JobQueue;

void jobqInit(JobQueue* q);

// False when the ring is full. A refusal is counted rather than silently overwriting the
// oldest job: a dropped generate job is a hole in the world, which is exactly the class of
// bug that has to be visible in a counter instead of discovered by walking into it.
bool jobqPush(JobQueue* q, Job job);

// False when empty; *out is left untouched in that case.
bool jobqPop(JobQueue* q, Job* out);

int  jobqCount(const JobQueue* q);
int  jobqPeak(const JobQueue* q);
int  jobqPushed(const JobQueue* q);
int  jobqDropped(const JobQueue* q);

// True when head, tail and count agree with each other. The dirty queue's shipped bug was
// a count that drifted from the thing it was counting, so this one is checkable the same
// way and is asserted after every operation the self-test performs.
bool jobqConsistent(const JobQueue* q);
