#include "app/worker.h"

#include <3ds.h>
#include <string.h>

#include "world/jobq.h"

// 32 KB. worldgenColumn's frame is about 1.3 KB (the 16x16 height and sandy tables) and
// nothing below it recurses, so this is mostly margin: a stack overflow on this console
// corrupts whatever is underneath instead of faulting, and the saving from a tight stack
// would be one page of heap.
#define WORKER_STACK_BYTES (32 * 1024)

// Step 6.3, and the answer is a measurement rather than the one the plan expected. Set to 1
// to ask for the system core; see the table in worker.h for why the default is 0.
#ifndef BS_WORKER_CORE
#define BS_WORKER_CORE 0
#endif

// Percentage of the *system* core the application may use, and only consulted when
// BS_WORKER_CORE is 1. 30 is the number every libctru example asks for and the number the
// plan names; measured, it is also the number that makes the world fill 2.5x slower, because
// it is a ceiling on the worker and not a floor. 80 measures the same as core 0 but takes
// that share from the OS services sharing the core, which cannot be tested on an emulator.
#define WORKER_CPU_TIME_LIMIT 80

static Thread     s_thread;
static bool       s_started;
static int        s_core = -1;   // where the worker actually ended up; see workerCore()

// Everything below is shared between the two threads and only ever touched under s_lock.
// s_work is the worker's wake-up: signalled when a job is submitted, when the staging
// world has been emptied, and when the worker is asked to quit.
static LightLock  s_lock;
static LightEvent s_work;

static JobQueue   s_queue;
static bool       s_quit;
static bool       s_in_flight;      // a job has been popped and is being generated
static bool       s_ready;          // staging holds a finished column waiting to be installed
static bool       s_ready_ok;       // ...and every chunk of it was generated
static int32_t    s_ready_cx, s_ready_cz;

// Worker-owned. The main thread only touches these while s_ready is set and the worker is
// parked, which is the whole handshake — see worker.h.
static World      s_staging;
static const WorldGen* s_gen;

static u64 s_busy_ticks, s_install_ticks;

static void workerMain(void* arg)
{
	(void)arg;

	for (;;) {
		// Cleared *before* the queue is inspected, so a job submitted between the
		// inspection and the wait leaves the event signalled and the wait returns
		// immediately. Clearing after the check is the classic lost-wakeup bug.
		LightEvent_Clear(&s_work);

		Job job = {JOB_NONE, 0, 0, 0};
		bool got = false, quit = false;

		LightLock_Lock(&s_lock);
		quit = s_quit;
		// Nothing is taken while a finished column is still waiting: there is one staging
		// world, so the previous result would be overwritten before it was installed.
		if (!quit && !s_ready)
			got = jobqPop(&s_queue, &job);
		if (got) s_in_flight = true;
		LightLock_Unlock(&s_lock);

		if (quit) break;

		if (!got) {
			LightEvent_Wait(&s_work);
			continue;
		}

		bool ok = false;
		const u64 t0 = svcGetSystemTick();
		switch (job.type) {
		case JOB_GENERATE:
			ok = worldgenColumn(s_gen, &s_staging, job.cx, job.cz);
			break;
		// JOB_MESH is the main thread's — it writes GPU-visible memory, which belongs to
		// the thread that owns the citro3d context. JOB_LOAD and JOB_SAVE are Phase 8 and
		// have no SD-card format to read or write yet. All three are listed so that adding
		// one is a compile error here rather than a job that silently vanishes.
		case JOB_MESH:
		case JOB_LOAD:
		case JOB_SAVE:
		case JOB_NONE:
			break;
		}
		s_busy_ticks += svcGetSystemTick() - t0;

		LightLock_Lock(&s_lock);
		s_ready    = true;
		s_ready_ok = ok;
		s_ready_cx = job.cx;
		s_ready_cz = job.cz;
		s_in_flight = false;
		LightLock_Unlock(&s_lock);
	}

	threadExit(0);
}

bool workerStart(const WorldGen* g)
{
	if (s_started) return true;

	s_gen = g;
	jobqInit(&s_queue);
	worldInit(&s_staging);
	s_quit = s_in_flight = s_ready = s_ready_ok = false;
	s_busy_ticks = s_install_ticks = 0;

	LightLock_Init(&s_lock);
	// Sticky rather than one-shot: the worker clears it itself before each check, so a
	// signal that arrives with nobody waiting must persist until the next wait.
	LightEvent_Init(&s_work, RESET_STICKY);

	// One step below the main thread. Higher numbers are lower priority on this console,
	// so this thread only runs when the main thread is blocked — which it is for most of
	// every frame, waiting on the GPU. Clamped to the 0x18..0x3F userland range.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (prio + 1 <= 0x3F) prio += 1;

	// Step 6.3. The second core is a request, not a guarantee: APT_SetAppCpuTimeLimit can
	// fail (it is refused outright under some launch paths), and threadCreate on core 1
	// fails without it. Both are checked and the result is recorded, because a silent
	// fall back to core 0 would leave every measurement afterwards labelled "core 1" and
	// meaning nothing of the sort.
	//
	// Falling back is the right failure, and — measured — so is not asking. Core 0 at one
	// priority step below the main thread is the step 5.5 arrangement and it works: the
	// worker gets the CPU while the main thread is blocked on the GPU, which is ~15.7 ms of
	// every 16.71 ms frame. Refusing to start would turn "no second core" into "no world".
	s_thread = NULL;
	if (BS_WORKER_CORE && R_SUCCEEDED(APT_SetAppCpuTimeLimit(WORKER_CPU_TIME_LIMIT))) {
		s_thread = threadCreate(workerMain, NULL, WORKER_STACK_BYTES, prio, 1, false);
		if (s_thread) s_core = 1;
	}
	if (!s_thread) {
		s_thread = threadCreate(workerMain, NULL, WORKER_STACK_BYTES, prio, 0, false);
		if (s_thread) s_core = 0;
	}
	if (!s_thread) {
		worldExit(&s_staging);
		return false;
	}

	s_started = true;
	return true;
}

int workerCore(void) { return s_core; }

void workerStop(void)
{
	if (!s_started) return;

	LightLock_Lock(&s_lock);
	s_quit = true;
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_work);

	threadJoin(s_thread, U64_MAX);
	threadFree(s_thread);
	s_thread = NULL;
	s_started = false;

	// Whatever was staged is dropped, not installed: stopping happens on the way out of
	// the game, and a half-installed column in a world about to be freed is worse than no
	// column at all.
	worldExit(&s_staging);
}

bool workerSubmitColumn(int32_t cx, int32_t cz)
{
	if (!s_started) return false;

	const Job job = {JOB_GENERATE, cx, cz, 0};

	LightLock_Lock(&s_lock);
	const bool ok = jobqPush(&s_queue, job);
	LightLock_Unlock(&s_lock);

	if (ok) LightEvent_Signal(&s_work);
	return ok;
}

bool workerInstall(World* w, int32_t* cx, int32_t* cz, bool* ok)
{
	// The lock is only valid once workerStart has run. LightLock_Init writes 1 for
	// "unlocked" and the .bss value is 0, which libctru reads as "held by someone" — so
	// locking an unstarted worker is not a no-op, it is a permanent hang. The hand-built
	// world (BS_WORLD_GEN=0) never starts the worker, so this path is real.
	if (!s_started) return false;

	LightLock_Lock(&s_lock);
	const bool ready = s_ready;
	const int32_t rx = s_ready_cx, rz = s_ready_cz;
	const bool gen_ok = s_ready_ok;
	LightLock_Unlock(&s_lock);

	if (!ready) return false;

	// The worker is parked until s_ready is cleared below, so the staging world is ours
	// for the duration of this function and needs no lock of its own.
	const u64 t0 = svcGetSystemTick();

	bool all = gen_ok;
	const Column* src = worldColumn(&s_staging, rx, rz);
	if (!src) {
		all = false;
	} else {
		for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
			// An absent chunk is sky, not a failure: worldgenColumn leaves everything
			// above the highest ground unallocated on purpose, and allocating it here
			// would undo that.
			if (!src->chunks[cy]) continue;

			Chunk* dst = worldChunkCreate(w, rx, cy, rz);
			if (!dst) { all = false; break; }
			memcpy(dst->blocks, src->chunks[cy]->blocks, sizeof(dst->blocks));
		}
	}

	// Frees the staged column and resets the staging world to empty, which is the state
	// the worker expects to find when it takes the next job.
	worldExit(&s_staging);

	s_install_ticks += svcGetSystemTick() - t0;

	LightLock_Lock(&s_lock);
	s_ready = false;
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_work);

	if (cx) *cx = rx;
	if (cz) *cz = rz;
	if (ok) *ok = all;
	return true;
}

bool workerBusy(void)
{
	if (!s_started) return false;

	LightLock_Lock(&s_lock);
	const bool busy = s_ready || s_in_flight || jobqCount(&s_queue) > 0;
	LightLock_Unlock(&s_lock);
	return busy;
}

int workerQueued(void)
{
	if (!s_started) return 0;

	LightLock_Lock(&s_lock);
	const int n = jobqCount(&s_queue);
	LightLock_Unlock(&s_lock);
	return n;
}

int workerDropped(void)
{
	if (!s_started) return 0;

	LightLock_Lock(&s_lock);
	const int n = jobqDropped(&s_queue);
	LightLock_Unlock(&s_lock);
	return n;
}

float workerBusyMs(void)    { return (float)((double)s_busy_ticks / CPU_TICKS_PER_MSEC); }
float workerInstallMs(void) { return (float)((double)s_install_ticks / CPU_TICKS_PER_MSEC); }
