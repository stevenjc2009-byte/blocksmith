#include "app/worker.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "world/jobq.h"
#include "world/region.h"

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

// ── Step 8.1: the save ring ───────────────────────────────────────────────────────────
//
// Two slots, not one, so the common case of a couple of edited columns leaving the ring
// together does not make the main thread wait for the card; and not more, because each slot
// is a worst-case column and 32 KB of .bss each is real memory spent on a queue that is
// empty almost all of the time. A third slot would buy the case where three edited columns
// unload in the same frame, which needs the player to have built in three columns and then
// crossed a boundary diagonally — rare enough to pay one SD write of latency for.
#define SAVE_SLOTS 2

typedef struct {
	int32_t  cx, cz;
	uint32_t len;
	uint8_t  bytes[REGION_COL_MAX];
} SaveSlot;

static SaveSlot s_save[SAVE_SLOTS];
static int      s_save_head;        // next slot the main thread fills
static int      s_save_tail;        // next slot the worker writes
static int      s_save_count;       // slots full; guarded by s_lock
static int      s_saved, s_save_failed, s_loaded;

// Empty means "no save file": every column is generated and nothing is written. Sized for a
// path under REGION_ROOT plus a world name.
static char s_world_dir[128];

// Read buffer for the load-before-generate path. Worker-owned, never touched by the main
// thread, and static rather than on the worker's 32 KB stack — which it would exactly fill.
static uint8_t s_load_buf[REGION_COL_MAX];

// Unpack buffer for workerInstall's staging → world copy. Since step 9.2a a Chunk is opaque
// and there is no flat blocks[] to memcpy between the two worlds, so a chunk is unpacked
// here and handed on whole. Main-thread-owned and safe as a static for the same reason the
// rest of workerInstall is: it only runs while s_ready is set and the worker is parked. 4 KB
// in .bss rather than on the stack because workerInstall runs on the *main* thread, whose
// frame is already carrying the render path — this is not worth spending stack on when it is
// live for a few microseconds per installed column.
static BlockId s_install_blocks[CHUNK_BLOCKS];

// Step 8.1. Fills the staging column from the region file, and says whether it managed it.
// False is the ordinary answer, not an error: it means the card has never heard of this
// column, which is true of all of them the first time a world is played.
//
// A failed decode leaves the chunks it had already allocated behind — region.h documents
// that the caller must treat the column as absent — so the half-built column is removed
// before the generator is allowed to run over the top of it. Skipping that would blend a
// corrupt saved column into freshly generated terrain, which is exactly the "neither state"
// outcome the save format is built to make impossible.
static bool workerLoadColumn(int32_t cx, int32_t cz)
{
	if (!s_world_dir[0]) return false;

	const uint32_t n = regionReadColumn(s_world_dir, cx, cz, s_load_buf, sizeof(s_load_buf));
	if (n == 0) return false;

	if (!regionDecodeColumn(&s_staging, cx, cz, s_load_buf, n)) {
		worldColumnRemove(&s_staging, cx, cz);
		return false;
	}

	// A saved column with no allocated chunks encodes to a one-byte mask and decodes without
	// creating anything, which workerInstall would read as a failed generate. Cannot happen
	// today — a column only becomes dirty by an edit, and an edit always leaves a chunk
	// allocated — but the install path assumes the column exists, so make it so rather than
	// leave a one-line assumption between two files.
	if (!worldColumnCreate(&s_staging, cx, cz)) return false;

	s_loaded++;
	return true;
}

// Writes one full save slot. Worker thread only; the slot's bytes are not touched by the
// main thread while it is counted full, so this needs no lock of its own.
static void workerWriteSave(int slot)
{
	const SaveSlot* s = &s_save[slot];
	if (regionWriteColumn(s_world_dir, s->cx, s->cz, s->bytes, s->len)) s_saved++;
	else                                                                s_save_failed++;

	LightLock_Lock(&s_lock);
	s_save_tail = (s_save_tail + 1) % SAVE_SLOTS;
	s_save_count--;
	LightLock_Unlock(&s_lock);
}

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
		int  save_slot = -1;

		LightLock_Lock(&s_lock);
		// Saves come first and are drained even while quitting, so workerStop cannot strand
		// a column the player edited on the way out. workerFlushSaves is still the right
		// thing for the caller to do — it waits for the card — but the ordering here means
		// forgetting it costs nothing.
		if (s_save_count > 0) save_slot = s_save_tail;
		quit = s_quit && save_slot < 0;
		// Nothing is taken while a finished column is still waiting: there is one staging
		// world, so the previous result would be overwritten before it was installed.
		if (!quit && save_slot < 0 && !s_ready)
			got = jobqPop(&s_queue, &job);
		if (got) s_in_flight = true;
		LightLock_Unlock(&s_lock);

		if (quit) break;

		if (save_slot >= 0) {
			const u64 ts = svcGetSystemTick();
			workerWriteSave(save_slot);
			s_busy_ticks += svcGetSystemTick() - ts;
			continue;
		}

		if (!got) {
			LightEvent_Wait(&s_work);
			continue;
		}

		bool ok = false;
		const u64 t0 = svcGetSystemTick();
		switch (job.type) {
		case JOB_GENERATE:
			// Step 8.1. The card first: a column the player changed must come back changed,
			// and regenerating it from the seed would quietly undo their work. Generation is
			// the fallback for everything the card does not have, which on a fresh world is
			// every column.
			ok = workerLoadColumn(job.cx, job.cz);
			if (!ok) ok = worldgenColumn(s_gen, &s_staging, job.cx, job.cz);
			break;
		// JOB_MESH is the main thread's — it writes GPU-visible memory, which belongs to
		// the thread that owns the citro3d context. JOB_LOAD and JOB_SAVE exist as job
		// types but are not routed through the queue: loading is folded into JOB_GENERATE
		// above, and saving has its own ring because it carries bytes, not coordinates.
		// All three are listed so that adding one is a compile error here rather than a
		// job that silently vanishes.
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

void workerSetWorldDir(const char* dir)
{
	if (!dir || !dir[0]) { s_world_dir[0] = '\0'; return; }

	// Truncated rather than refused, and then checked: a path that does not fit is a
	// programming error in the caller, and silently saving into a *different* directory —
	// which is what a truncated path would do — is the one outcome worse than not saving.
	snprintf(s_world_dir, sizeof(s_world_dir), "%s", dir);
	if (strlen(dir) >= sizeof(s_world_dir)) s_world_dir[0] = '\0';
}

bool workerStart(const WorldGen* g)
{
	if (s_started) return true;

	s_gen = g;
	jobqInit(&s_queue);
	worldInit(&s_staging);
	s_quit = s_in_flight = s_ready = s_ready_ok = false;
	s_busy_ticks = s_install_ticks = 0;
	s_save_head = s_save_tail = s_save_count = 0;
	s_saved = s_save_failed = s_loaded = 0;

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

bool workerSubmitSave(const Column* col)
{
	if (!s_started || !s_world_dir[0] || !col) return true;

	// Wait for a slot. Sleeping rather than spinning, because the thread being waited on is
	// one priority step *below* this one: a spin here would keep the worker off the CPU
	// altogether on core 0 and the wait would never end. 1 ms is far below the SD write it
	// is waiting for, so the granularity costs nothing.
	for (;;) {
		LightLock_Lock(&s_lock);
		const bool room = s_save_count < SAVE_SLOTS;
		LightLock_Unlock(&s_lock);
		if (room) break;
		svcSleepThread(1000000ULL);
	}

	SaveSlot* s = &s_save[s_save_head];
	s->cx  = col->cx;
	s->cz  = col->cz;
	s->len = regionEncodeColumn(col, s->bytes, sizeof(s->bytes));
	if (s->len == 0) {
		s_save_failed++;
		return false;
	}

	// Published only once the bytes are in: the worker takes any slot the count says is
	// full, so incrementing first would hand it a half-written buffer.
	LightLock_Lock(&s_lock);
	s_save_head = (s_save_head + 1) % SAVE_SLOTS;
	s_save_count++;
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_work);
	return true;
}

void workerFlushSaves(void)
{
	if (!s_started) return;

	for (;;) {
		LightLock_Lock(&s_lock);
		const int n = s_save_count;
		LightLock_Unlock(&s_lock);
		if (n == 0) return;
		LightEvent_Signal(&s_work);
		svcSleepThread(1000000ULL);
	}
}

int workerLoaded(void)     { return s_loaded; }
int workerSaved(void)      { return s_saved; }
int workerSaveFailed(void) { return s_save_failed; }

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

			// Unpack once, install whole. worldSetChunkAll re-picks the smallest storage
			// form for the destination and settles the budget itself — the same route
			// worldgen and the save loader take — so a freshly installed column is never
			// promoted cell by cell on its own construction. Before step 9.2a this was a
			// memcpy of two flat blocks[] arrays; a Chunk no longer has one.
			chunkDecompressAll(src->chunks[cy], s_install_blocks);
			if (!worldSetChunkAll(w, rx, cy, rz, s_install_blocks)) { all = false; break; }
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
