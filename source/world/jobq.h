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

// 512 slots = 8 KB.
//
// This was 128 ("= 2 KB"), justified against Phase 5's 25 generate jobs and 3x3 columns of at
// most 72 chunks. That justification stopped being true at RENDER_DIST_MAX 2 without anybody
// noticing: main.c's genQueueReadyColumns pushes one JOB_MESH per non-air chunk of every
// column whose neighbour ring has just completed, and a radius-2 ring is 25 columns —
// which at the pool's own 6 chunks a column is exactly 150, already over 128, and measured at
// 125 simultaneous meshed chunks. The queue was one bad seed from refusing, and "Phase 6
// pushes per frame and drains per frame" only holds once the world is streaming; at boot and
// at every distance change the whole ring queues in a burst.
//
// At RENDER_DIST_MAX 3 the ring is 49 columns. The structural upper bound on one burst is
// therefore 49 * COLUMN_CHUNKS = 392 mesh jobs (every chunk of every column non-air), and the
// measured worst simultaneous occupancy is 242 (see scene/chunk_render.c's TIER_*_SLOTS for
// the measurement). 512 clears the structural bound, not just the measured one, and is the
// next power of two above it — which on ARM11 also keeps the ring's index wrap a mask rather
// than a multiply-and-shift, since the part has no integer divide instruction.
//
// The capacity is only half of what made this dangerous. See main.c's genQueueReadyColumns and
// world/meshq.h for the other half: a refused push used to leave the column marked queued, so
// the chunks it lost were never asked for again.
#define JOBQ_CAP 512

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
