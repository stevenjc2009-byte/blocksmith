#include "app/worker.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/loadprof.h"
#include "world/jobq.h"
#include "world/light.h"
#include "world/region.h"
#include "world/registry.h"

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

// v1.7.1 task 48b. The save-slot wait, made visible — see workerSubmitSave for why a timing
// alone could not answer the question. Written only by the main thread inside
// workerSubmitSave, read by the debug overlay on the same thread, so they need no lock.
static int      s_save_submits;         // calls that got as far as wanting a slot
static int      s_save_waits;           // ...of which this many had to sleep at least once
static u64      s_save_wait_ticks;      // total slept
static u64      s_save_wait_max_ticks;  // worst single wait

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

// v1.5.0 adaptive lighting: the BFS worklist lightPropagateColumn fills, allocated only
// when the engine is enabled (New 3DS) so an Old 3DS never pays for it, and owned by this
// thread for its whole life — the main thread's edit path uses the queue-free sweep engine
// precisely so the two never share one. ~64 KB of heap against the worker's measured ~94%
// idle capacity; propagation is what that headroom is for.
static LightQueue* s_lightq;

static bool workerLightQueue(void)
{
	if (!s_lightq) {
		s_lightq = (LightQueue*)malloc(sizeof(LightQueue));
		if (!s_lightq) return false;
		lightQueueInit(s_lightq);
	}
	return true;
}

// Step 8.1. Fills the staging column from the region file, and says whether it managed it.
// False is the ordinary answer, not an error: it means the card has never heard of this
// column, which is true of all of them the first time a world is played.
//
// A failed decode leaves the chunks it had already allocated behind — region.h documents
// that the caller must treat the column as absent — so the half-built column is removed
// before the generator is allowed to run over the top of it. Skipping that would blend a
// corrupt saved column into freshly generated terrain, which is exactly the "neither state"
// outcome the save format is built to make impossible.
//
// v1.7.1 task 48. regionReadColumnCached rather than regionReadColumn, because this is the
// one place in the game that reads a column and it is called once per JOB_GENERATE — 81 of
// them for a single genRequestArea at RENDER_DIST_MAX. Uncached, each one re-opened the
// region file and re-parsed both 3088-byte directory copies to answer a question about one
// 12-byte slot, which is the whole reason a world with a region file took roughly twice as
// long to load as a fresh one. Correctness is unchanged: the cache is dropped by every writer
// in region.c, including workerWriteSave's regionWriteColumn below, so a column saved a
// moment ago is never answered from a directory that predates it.
static bool workerLoadColumn(int32_t cx, int32_t cz)
{
	if (!s_world_dir[0]) return false;

	// v1.7.1 task 48b. The two halves of a load are timed apart because they answer different
	// questions: region_io is the card (an open, a directory parse, a payload read) and decode
	// is the codec (eight chunks unpacked into the staging world). A world with a region file
	// pays region_io for EVERY column of the ring and decode only for the ones that were
	// actually saved, so the pair is what tells "the card is slow" from "there is a lot saved".
	const uint64_t t_io = loadprofMark();
	const uint32_t n = regionReadColumnCached(s_world_dir, cx, cz, s_load_buf, sizeof(s_load_buf));
	loadprofSince(LOAD_STAGE_REGION_IO, t_io);
	if (n == 0) return false;

	const uint64_t t_dec = loadprofMark();
	const bool decoded = regionDecodeColumn(&s_staging, cx, cz, s_load_buf, n);
	loadprofSince(LOAD_STAGE_DECODE, t_dec);
	if (!decoded) {
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
	if (regionWriteColumn(s_world_dir, s->cx, s->cz, s->bytes, s->len)) {
		s_saved++;

		// v1.8.2. The region file's payload arena is append-only, so this save just orphaned
		// the previous copy of this column and nothing used to give that space back:
		// regionCompact() existed and had no caller in any shipped build, and a region file
		// measured at 482,885 bytes on disk holding 19,452 bytes of live columns after 960
		// saves — 95.9 % dead and still climbing. regionMaintain declines unless at least half
		// the arena is dead, so this is two file probes and a directory read on twenty-six saves
		// out of twenty-seven and a whole-file rewrite on the other one. That ratio is 960/36 —
		// the measured run compacted 36 times across its 960 saves — and it is written with its
		// derivation so it cannot drift away from the compaction count again: it read "sixteen
		// out of seventeen" (960/56) while that count was the fixture-inflated 56. See region.h.
		//
		// Here rather than at world close on purpose. The worker is the background thread —
		// measured ~94 % idle — so a rewrite costs the frame nothing, and it happens while the
		// player is still playing rather than while they are trying to quit, which is the one
		// moment a lid close or a battery pull is most likely. See region.h.
		//
		// Not verified on hardware: if an SD rewrite of a big region is slow enough, three
		// column saves landing inside one would fill the two-slot ring above and make the main
		// thread wait in workerSubmitSave. workerSaveWaits() is where that would show.
		regionMaintain(s_world_dir, regionOf(s->cx), regionOf(s->cz));
	} else {
		s_save_failed++;
	}

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
			if (!ok) {
				// v1.7.1 task 48b. Timed only when it actually runs, so `generate`'s call
				// count IS the number of columns the card did not have — the other half of
				// the fresh-versus-reloaded question region_io/decode opens above.
				const uint64_t t_gen = loadprofMark();
				ok = worldgenColumn(s_gen, &s_staging, job.cx, job.cz);
				loadprofSince(LOAD_STAGE_GENERATE, t_gen);
			}

			// v1.5.0 adaptive lighting: light the staged column while it is here, before
			// install can queue it ready — the jobq ordering is what guarantees light
			// exists before the main-thread mesher reads it. A refusal (budget, allocator,
			// queue overflow) leaves the column unlit, which degrades to today's look via
			// scratchFillLight's full-sky default rather than to darkness; a false return
			// from the BFS falls back to the sweep engine so a dropped entry can never
			// ship half-lit terrain.
			if (ok && lightEnabled() && workerLightQueue()) {
				const uint64_t t_light = loadprofMark();
				if (!lightPropagateColumn(&s_staging, job.cx, job.cz, s_lightq))
					lightRelightColumn(&s_staging, job.cx, job.cz);
				loadprofSince(LOAD_STAGE_LIGHT, t_light);
			}
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
	// v1.7.1 task 48. The region cache keys on the world directory, so a stale entry could
	// never be *served* to a different world — but it would still hold a FILE* open on the
	// world being left, and an open handle on a FAT card is what stops that world's directory
	// being removed later. Dropped here rather than relying on eviction. Safe at this point
	// for the same reason workerStop's call is: main.c only changes the world directory
	// between sessions, with no worker running.
	regionCacheClose();

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

	// Cleared with the rest, so the counters describe THIS world rather than every world
	// entered since the app launched. Without this a second world would inherit the first
	// one's waits and the capture would be unattributable.
	s_save_submits = s_save_waits = 0;
	s_save_wait_ticks = s_save_wait_max_ticks = 0;

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

	// v1.7.1 task 48. After the join, never before it: the region cache is worker-thread
	// state and closing its handles while that thread could still be reading through them is
	// a use-after-free of a FILE*. The saves have already been drained by the loop above —
	// quitting does not break out until the ring is empty — so nothing is left to write and
	// the entries here are pure read state.
	regionCacheClose();

	// Whatever was staged is dropped, not installed: stopping happens on the way out of
	// the game, and a half-installed column in a world about to be freed is worse than no
	// column at all.
	worldExit(&s_staging);

	free(s_lightq);
	s_lightq = NULL;
}

bool workerSubmitColumn(int32_t cx, int32_t cz)
{
	// v1.6.0 Phase A: the block registry must be frozen before any generation runs —
	// populate + freeze happen before workerStart(), so a submit past this point means a
	// caller is generating against a table that could still change underneath it, which
	// the design forbids (post-freeze tables are read-only, no locks). Fail loudly.
	if (!registryFrozen()) return false;
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
	//
	// ── v1.7.1 task 48b: this wait is COUNTED, not just timed ──────────────────────────
	//
	// main.c already times the call (s_frame_save_ms → the `save_ms` CSV column), but a
	// total of 0.0 ms cannot be told apart from a frame that never reached this branch at
	// all — and the difference is the entire question. The `col->dirty` gate means an
	// unedited world never enters it, so every clean capture ever taken is silent about
	// this path rather than evidence against it.
	//
	// Why it is worth counting. SAVE_SLOTS is 2, and the comment on it says a third slot
	// would only buy "three edited columns unloading in the same frame, which needs the
	// player to have built in three columns and then crossed a boundary DIAGONALLY". That
	// understates the geometry. genInArea is a square window of half-width s_area_radius,
	// so ONE ordinary axis-aligned step of the ring unloads a whole edge — 2*4+1 = 9
	// columns at RENDER_DIST_MAX, and 17 on a diagonal step. Three of those nine being
	// dirty needs a base three columns wide and a walk out of it, not a rarity. Whether
	// the resulting wait is milliseconds or microseconds depends on the SD card, which
	// cannot be measured anywhere but on the console.
	//
	// So: submits, how many of them slept at all, the total slept and the worst single
	// wait. Four counters, read through worker.h. A capture on a world with a base in it
	// now answers the question outright instead of leaving it open.
	s_save_submits++;
	{
		const u64 t_wait = svcGetSystemTick();
		bool slept = false;

		for (;;) {
			LightLock_Lock(&s_lock);
			const bool room = s_save_count < SAVE_SLOTS;
			LightLock_Unlock(&s_lock);
			if (room) break;
			slept = true;
			svcSleepThread(1000000ULL);
		}

		if (slept) {
			const u64 waited = svcGetSystemTick() - t_wait;
			s_save_waits++;
			s_save_wait_ticks += waited;
			if (waited > s_save_wait_max_ticks) s_save_wait_max_ticks = waited;
		}
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
	// v1.7.1 task 48b. The same bracket s_install_ticks already keeps, reported per world load
	// instead of per session: workerInstallMs() is a since-launch total and cannot say what one
	// world entry cost. Both are kept because they are read in different places — the gen
	// overlay reads the running total, load.csv reads the per-load one.
	const uint64_t t_inst = loadprofMark();

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

		// v1.5.0 adaptive lighting: the staged column's channels ride across with its
		// blocks, under the same parked-worker handshake that makes the chunk copy
		// lock-free. A refusal leaves the live column unlit — scratchFillLight's
		// full-sky default covers it — and the staging copy is freed with the rest of
		// the staging world below either way.
		if (all) lightColumnCopy(src, worldColumn(w, rx, rz));
	}

	// Frees the staged column and resets the staging world to empty, which is the state
	// the worker expects to find when it takes the next job.
	worldExit(&s_staging);

	s_install_ticks += svcGetSystemTick() - t0;
	loadprofSince(LOAD_STAGE_INSTALL, t_inst);

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

int   workerSaveSubmits(void) { return s_save_submits; }
int   workerSaveWaits(void)   { return s_save_waits; }
float workerSaveWaitMs(void)
{
	return (float)((double)s_save_wait_ticks / CPU_TICKS_PER_MSEC);
}
float workerSaveWaitMaxMs(void)
{
	return (float)((double)s_save_wait_max_ticks / CPU_TICKS_PER_MSEC);
}
