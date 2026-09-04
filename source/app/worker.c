#include "app/worker.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/hw.h"
#include "app/lanes.h"
// v1.8.16 FRZ-FIX. The save-slot wait's policy half, extracted so it can be run on a host —
// see app/savewait.h. watchdog.h is here for watchdogWriteFile(), the raw-FS writer the hang
// report uses; workerSaveStallNote below needs the same one and for the same reason.
#include "app/savewait.h"
#include "app/stage_probe.h"
#include "app/watchdog.h"
#include "debug/loadprof.h"
#include "world/jobq.h"
#include "world/light.h"
#include "world/region.h"
#include "world/registry.h"
#include "world/worldgen_scratch.h"

// 32 KB. worldgenColumn's frame is about 1.3 KB (the 16x16 height and sandy tables) and
// nothing below it recurses, so this is mostly margin: a stack overflow on this console
// corrupts whatever is underneath instead of faulting, and the saving from a tight stack
// would be one page of heap. Charged once per lane — libctru takes a thread stack off the
// application heap — so a second lane costs another 32 KB of heap on a New 3DS and nothing
// at all on an Old one.
#define WORKER_STACK_BYTES (32 * 1024)

// Step 6.3, and the answer is a measurement rather than the one the plan expected. Set to 1
// to ask for the system core; see the table in worker.h for why the default is 0.
//
// v1.8.8: read worker.h's note on cia/blocksmith.rsf's AffinityMask before setting this to 1
// on an installed CIA. The exheader permits core 0 only (AffinityMask : 1), so the request
// below can succeed and the threadCreate that follows it still be refused.
#ifndef BS_WORKER_CORE
#define BS_WORKER_CORE 0
#endif

// Percentage of the *system* core the application may use, and only consulted when
// BS_WORKER_CORE is 1. 30 is the number every libctru example asks for and the number the
// plan names; measured, it is also the number that makes the world fill 2.5x slower, because
// it is a ceiling on the worker and not a floor. 80 measures the same as core 0 but takes
// that share from the OS services sharing the core, which cannot be tested on an emulator.
#define WORKER_CPU_TIME_LIMIT 80

// v1.8.8. Where the SECOND lane goes, and why it is a constant rather than a ladder.
//
// app/hw.c's hwPreferredWorkerCore() is a ladder because every core it offers is a request
// that can be refused for a different reason. Lane 1 has no such choice to make: core 2 has
// already been taken by lane 0 (the second lane is not started at all unless it was — see
// workerStart), core 1 is the system core this game does not take, and core 3 is not offered
// to applications. That leaves core 0, which is always available and is the core the main
// thread is blocked on the GPU for ~15.7 ms of every 16.71 ms frame.
//
// [2026-09-03 CORRECTION, v1.8.17] That ~15.7 ms figure is an Azahar-measured inference, not
// a GPU reading -- Azahar has no GPU cost model. See debug/metrics.h:132.
#define WORKER_LANE1_CORE 0

// ── One lane ──────────────────────────────────────────────────────────────────────────
//
// Everything a generator lane owns exclusively. Lane 0 is a static (its cost is unchanged
// from v1.8.7 and an Old 3DS must not pay a byte for a lane it will never start); lane 1 is
// malloc'd by workerStart and only on a New 3DS that actually got core 2.
typedef struct {
	int        index;      // 0 or 1; the index into s_claims and the tag in the ready ring
	Thread     thread;
	int        core;       // the core threadCreate ACCEPTED for this lane -- see below
	LightEvent work;       // this lane's wake-up; see the note on the event scheme below

	// v1.8.17 N3D-CORE. Where this lane is ACTUALLY running, read by the lane itself with
	// svcGetProcessorID() as the first thing workerMain does. -1 until the thread has been
	// scheduled once.
	//
	// Why both this and `core` exist, when `core` is already documented as "where it landed".
	// `core` is the argument threadCreate accepted, and on the documented kernel behaviour
	// that IS the core the thread got: 3dbrew's Multi-threading page says the kernel's only
	// processor-id restriction checks are the kernel-flags 0x2000 test for core 2 and an
	// outright refusal of core 3, and that a refused core makes CreateThread RETURN
	// 0xD9001BEA rather than quietly placing the thread somewhere else. So `core` should
	// equal `core_run`, and nothing in this tree has ever checked that it does.
	//
	// It has to stay a separate field rather than replacing `core`, because workerStart
	// reads `s_lane0.core` to decide whether to start lane 1 at all, and it reads it
	// immediately after threadCreate returns — before the new thread has necessarily run a
	// single instruction. A measured field would still be -1 at that moment and the second
	// lane would never start on any console.
	// FRZ-AUDIT defect 3 (2026-09-03). Was `volatile s32`. volatile suppresses the compiler's
	// own caching of the value but is not a cross-core barrier on ARM11 -- it emits none. The
	// write in workerMain and the read in workerLaneCoreRunning() now use explicit
	// __ATOMIC_RELEASE / __ATOMIC_ACQUIRE, which do emit one (verified by disassembly: `mcr
	// p15,0,r3,c7,c10,5`, the same ARM11 DMB documented above for LightLock/LightEvent).
	// volatile was not doing anything useful here and is dropped rather than kept alongside.
	s32 core_run;

	// Worker-owned. The main thread only touches this while this lane's claim is held and
	// the lane is parked, which is the whole handshake — see worker.h.
	World      staging;

	// v1.8.7. THIS LANE's generator scratch, 16,256 B — the buffers world/worldgen.c and
	// world/worldgen_density.c used to keep in file statics. It is exactly one lane's worth,
	// which is the point of that move and is what makes v1.8.8's second lane possible at all.
	WorldGenScratch wgs;

	// v1.5.0 adaptive lighting: the BFS worklist lightPropagateColumn fills, allocated only
	// when the engine is enabled — which, since v1.8.0 task 24, is BOTH console models: this
	// stopped being New-3DS-only when chunk_render.c's chunkRenderInit started calling
	// lightEngineInit(true) unconditionally (source/scene/chunk_render.c:844), so an Old 3DS
	// pays for this too now. Owned by its lane for that lane's whole life — the main thread's
	// edit path uses the CAS-claimed shared queue in world/light.c and never this one.
	// ~64 KB of heap per lane against the worker's measured ~94% idle capacity; propagation
	// is what that headroom is for.
	LightQueue* lightq;

	// Per-lane so the two do not have to contend for one counter on the hot path. Read by
	// workerBusyMs(), which sums them.
	//
	// FRZ-AUDIT defect 1 (2026-09-03). Written with a plain `+=` from the lane thread and
	// summed with no lock from the main thread was a data race even though nothing currently
	// branches on the result (main.c:3238's printf is the only reader). Both sides now go
	// through __atomic_fetch_add / __atomic_load_n, __ATOMIC_RELAXED -- see the call sites.
	u64        busy_ticks;
} Lane;

static Lane   s_lane0;                       // always; .bss, unchanged cost on both consoles
static Lane*  s_lane[WORKER_LANES_MAX];      // [0] = &s_lane0, [1] = malloc'd on a New 3DS
static int    s_lanes;                       // how many are actually running: 1 or 2
static bool   s_started;

// ── The event scheme, and why it is one event PER LANE ────────────────────────────────
//
// A single sticky LightEvent with two waiters is a lost-wakeup waiting to happen: each waiter
// has to clear the event before it inspects the queue (clearing afterwards is the classic
// bug), and with two of them one lane's clear can swallow the signal the other lane needed.
// Per-lane events remove the question entirely. The main thread signals every lane on submit;
// each lane clears only its own event and then looks at the shared queue; at most one of them
// gets the job and the other goes back to sleep having lost nothing.
static LightLock  s_lock;

// v1.8.8. The SD card, which is emphatically not per-lane. worker.h's step 8.1 note is the
// reason: libctru reaches the card through one FS service session shared by the whole process,
// and two threads inside it at once is not a race this project wants to reason about. That
// argument was satisfied for free while there was one worker thread; with two it needs a lock.
//
// It covers regionReadColumnCached, regionDecodeColumn, regionWriteColumn and regionMaintain —
// i.e. every entry into world/region.c from this file — because region.c's directory cache
// (s_cache, s_cache_clock), its write hint (s_write_hint) and regionCompact's static repack
// buffer are all file statics shared by whoever calls in. It also lets the two lanes share one
// s_load_buf below instead of paying 32,816 B twice.
//
// It does NOT serialise generation, which is the expensive half and the whole point of the
// second lane: the lock is dropped before worldgenColumn runs. On a fresh world a load is a
// cache hit and a miss return; on a saved world the decodes serialise, which is what the one
// FS session requires anyway.
static LightLock  s_fs_lock;

static JobQueue   s_queue;
static bool       s_quit;

// v1.8.8. Which column each lane is working on, and the finished columns waiting to be
// installed. Both are plain bookkeeping with no locking of their own — see app/lanes.h — and
// every call to them in this file is made under s_lock.
static LaneClaims    s_claims;
static LaneReadyRing s_ready;

// Jobs thrown away because the other lane was already generating that column. Not an error:
// main.c can legitimately submit one column twice (genUnloadColumn clears its `asked` mark
// when it leaves the ring and genRequestArea re-asks if the player walks back), and the copy
// already in flight is the one that will arrive. Counted so that "the world has a hole in it"
// can be told apart from "a duplicate was dropped".
static int        s_dup_dropped;

static const WorldGen* s_gen;

static u64 s_install_ticks;

// ── Step 8.1: the save ring ───────────────────────────────────────────────────────────
//
// Two slots, not one, so the common case of a couple of edited columns leaving the ring
// together does not make the main thread wait for the card; and not more, because each slot
// is a worst-case column and 32 KB of .bss each is real memory spent on a queue that is
// empty almost all of the time. A third slot would buy the case where three edited columns
// unload in the same frame, which needs the player to have built in three columns and then
// crossed a boundary diagonally — rare enough to pay one SD write of latency for.
//
// v1.8.8: drained by LANE 0 ONLY, whatever the lane count. Nothing about a save is
// parallelisable — it is one FS session and one card — so handing it to whichever lane
// happened to be idle would buy no throughput and would put a second thread in the write
// path for the sake of symmetry.
#define SAVE_SLOTS 2

typedef struct {
	int32_t  cx, cz;
	uint32_t len;
	uint8_t  bytes[REGION_COL_MAX];
} SaveSlot;

static SaveSlot s_save[SAVE_SLOTS];
static int      s_save_head;        // next slot the main thread fills
static int      s_save_tail;        // next slot lane 0 writes
static int      s_save_count;       // slots full; guarded by s_lock
static int      s_saved, s_save_failed, s_loaded;

// v1.7.1 task 48b. The save-slot wait, made visible — see workerSubmitSave for why a timing
// alone could not answer the question. Written only by the main thread inside
// workerSubmitSave, read by the debug overlay on the same thread, so they need no lock.
static int      s_save_submits;         // calls that got as far as wanting a slot
static int      s_save_waits;           // ...of which this many had to sleep at least once
static u64      s_save_wait_ticks;      // total slept
static u64      s_save_wait_max_ticks;  // worst single wait

// v1.8.16 FRZ-FIX. The two outcomes the deadline added, counted for the same reason the four
// above are counted: without a number, "the deadline never fires" and "the deadline fires and
// the card is always free" look identical from the debug menu.
static int      s_save_sync;            // written on the MAIN thread after the deadline
static int      s_save_overrun;         // deadline expired AND the card was locked

// Empty means "no save file": every column is generated and nothing is written. Sized for a
// path under REGION_ROOT plus a world name.
static char s_world_dir[128];

// Read buffer for the load-before-generate path. Static rather than on a lane's 32 KB stack —
// which it would not merely fill but overflow by itself: REGION_COL_MAX is 32,816 bytes
// (region.h: COLUMN_CHUNKS(8) * (CHUNK_CODEC_MAX(4098) + 2) + 16) against WORKER_STACK_BYTES's
// 32,768 above — 48 bytes over before any call frame, return address, or other local is
// counted.
//
// v1.8.8: SHARED by both lanes and only ever touched inside s_fs_lock, which is where the
// bytes are read and where they are decoded. Per-lane would cost a second 32,816 B for a
// buffer that can only be in use by one lane at a time anyway.
static uint8_t s_load_buf[REGION_COL_MAX];

// Unpack buffer for workerInstall's staging → world copy. Since step 9.2a a Chunk is opaque
// and there is no flat blocks[] to memcpy between the two worlds, so a chunk is unpacked
// here and handed on whole. Main-thread-owned and safe as a static for the same reason the
// rest of workerInstall is: it only runs while the source lane's claim is held and that lane
// is parked. 4 KB in .bss rather than on the stack because workerInstall runs on the *main*
// thread, whose frame is already carrying the render path — this is not worth spending stack
// on when it is live for a few microseconds per installed column.
static BlockId s_install_blocks[CHUNK_BLOCKS];

static bool workerLightQueue(Lane* ln)
{
	if (!ln->lightq) {
		ln->lightq = (LightQueue*)malloc(sizeof(LightQueue));
		if (!ln->lightq) return false;
		lightQueueInit(ln->lightq);
	}
	return true;
}

// Step 8.1. Fills the lane's staging column from the region file, and says whether it managed
// it. False is the ordinary answer, not an error: it means the card has never heard of this
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
//
// v1.8.8. Everything that touches world/region.c or s_load_buf happens inside s_fs_lock; the
// bookkeeping that follows touches only this lane's own staging world and is outside it.
static bool workerLoadColumn(Lane* ln, int32_t cx, int32_t cz)
{
	if (!s_world_dir[0]) return false;

	LightLock_Lock(&s_fs_lock);

	// v1.7.1 task 48b. The two halves of a load are timed apart because they answer different
	// questions: region_io is the card (an open, a directory parse, a payload read) and decode
	// is the codec (eight chunks unpacked into the staging world). A world with a region file
	// pays region_io for EVERY column of the ring and decode only for the ones that were
	// actually saved, so the pair is what tells "the card is slow" from "there is a lot saved".
	const uint64_t t_io = loadprofMark();
	const uint32_t n = regionReadColumnCached(s_world_dir, cx, cz, s_load_buf, sizeof(s_load_buf));
	loadprofSince(LOAD_STAGE_REGION_IO, t_io);

	bool decoded = false;
	if (n != 0) {
		const uint64_t t_dec = loadprofMark();
		decoded = regionDecodeColumn(&ln->staging, cx, cz, s_load_buf, n);
		loadprofSince(LOAD_STAGE_DECODE, t_dec);
	}

	LightLock_Unlock(&s_fs_lock);

	if (n == 0) return false;
	if (!decoded) {
		worldColumnRemove(&ln->staging, cx, cz);
		return false;
	}

	// A saved column with no allocated chunks encodes to a one-byte mask and decodes without
	// creating anything, which workerInstall would read as a failed generate. Cannot happen
	// today — a column only becomes dirty by an edit, and an edit always leaves a chunk
	// allocated — but the install path assumes the column exists, so make it so rather than
	// leave a one-line assumption between two files.
	if (!worldColumnCreate(&ln->staging, cx, cz)) return false;

	__atomic_fetch_add(&s_loaded, 1, __ATOMIC_RELAXED);
	return true;
}

// Writes one full save slot. Lane 0 only; the slot's bytes are not touched by the main thread
// while it is counted full, so this needs no lock of its own — but the card does, because
// lane 1 may be inside a region read at the same moment.
static void workerWriteSave(int slot)
{
	const SaveSlot* s = &s_save[slot];

	LightLock_Lock(&s_fs_lock);
	const bool written = regionWriteColumn(s_world_dir, s->cx, s->cz, s->bytes, s->len);
	if (written) {
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
		// thread wait in workerSubmitSave. workerSaveWaits() is where that would show. v1.8.8
		// adds a second way for it to show: lane 1 blocking on s_fs_lock behind this rewrite,
		// which costs generation throughput rather than a frame.
		regionMaintain(s_world_dir, regionOf(s->cx), regionOf(s->cz));
	}
	LightLock_Unlock(&s_fs_lock);

	if (written) __atomic_fetch_add(&s_saved, 1, __ATOMIC_RELAXED);
	else         __atomic_fetch_add(&s_save_failed, 1, __ATOMIC_RELAXED);

	LightLock_Lock(&s_lock);
	s_save_tail = (s_save_tail + 1) % SAVE_SLOTS;
	s_save_count--;
	LightLock_Unlock(&s_lock);
}

static void workerMain(void* arg)
{
	Lane* const ln = (Lane*)arg;

	// v1.8.17 N3D-CORE. First thing, before any work: which core did this lane actually get?
	// svcGetProcessorID() is libctru's wrapper for SVC 0x11 GetCurrentProcessorNumber --
	// verified by disassembling libctru.a's svc.o, which is two instructions:
	//     svc 0x00000011
	//     bx  lr
	// It reads the core the CALLING thread is executing on right now, so it can only be
	// answered from inside the thread being asked about. cia/blocksmith.rsf grants SVC 17
	// (SystemCallAccess GetCurrentProcessorNumber), confirmed present in the shipped
	// v1.8.16 exheader's system-call mask.
	// FRZ-AUDIT defect 3. RELEASE, paired with the ACQUIRE load in workerLaneCoreRunning(). This
	// is the first statement of workerMain, so there is nothing else on this thread yet that the
	// barrier needs to publish alongside it -- but a plain store here previously gave a reader on
	// another core (the watchdog, or main.c's debug overlay) no guarantee of ever seeing this
	// value promptly, or at all, without one.
	__atomic_store_n(&ln->core_run, svcGetProcessorID(), __ATOMIC_RELEASE);

	// FRZ-PROBE. One marker per lane, at thread start, naming which lane index this is — so a
	// freeze that happens before ANY worker lane got scheduled at all shows up as "no
	// WORKER_LANE line in stage.txt" rather than as silence. See app/stage_probe.h. The whole
	// block sits behind its own #if, not just the BS_STAGE() call inside it — otherwise the
	// snprintf() and its format-string literal would still be compiled into every shipped
	// build, which is exactly what this flag promises never to do.
#if BS_STAGE_PROBE
	{
		char sp_buf[32];
		snprintf(sp_buf, sizeof(sp_buf), "WORKER_LANE%d core=%ld", ln->index, (long)ln->core_run);
		BS_STAGE(sp_buf);
	}
#endif

	for (;;) {
		// Cleared *before* the queue is inspected, so a job submitted between the
		// inspection and the wait leaves the event signalled and the wait returns
		// immediately. Clearing after the check is the classic lost-wakeup bug. Per-lane,
		// so one lane's clear cannot swallow the other lane's wake-up — see s_lock above.
		LightEvent_Clear(&ln->work);

		Job job = {JOB_NONE, 0, 0, 0};
		bool got = false, quit = false;
		int  save_slot = -1;

		LightLock_Lock(&s_lock);
		// Saves come first and are drained even while quitting, so workerStop cannot strand
		// a column the player edited on the way out. workerFlushSaves is still the right
		// thing for the caller to do — it waits for the card — but the ordering here means
		// forgetting it costs nothing. Lane 0's job alone: see SAVE_SLOTS above.
		if (ln->index == 0 && s_save_count > 0) save_slot = s_save_tail;
		quit = s_quit && save_slot < 0;

		// Nothing is taken while this lane's own finished column is still waiting: a lane has
		// one staging world, so the previous result would be overwritten before it was
		// installed. That is what its claim means — it is held from here until workerInstall
		// has copied the column out and emptied the staging world.
		if (!quit && save_slot < 0 && !laneClaimBusy(&s_claims, ln->index)) {
			while (jobqPop(&s_queue, &job)) {
				if (laneClaimTake(&s_claims, ln->index, job.cx, job.cz)) { got = true; break; }
				// The other lane is already generating this exact column. Dropping the
				// duplicate is right and it is not a hole: the copy in flight will be
				// installed, and anything that genuinely never arrives is re-asked by
				// main.c's genRequestArea. See app/lanes.h.
				s_dup_dropped++;
			}
		}
		LightLock_Unlock(&s_lock);

		if (quit) break;

		if (save_slot >= 0) {
			const u64 ts = svcGetSystemTick();
			workerWriteSave(save_slot);
			// FRZ-AUDIT defect 1. RELAXED: a statistical counter with no ordering relationship
			// to anything else this thread or the reader does -- workerBusyMs() (main thread)
			// only ever sums it for a printf diagnostic, nothing branches on it, and no other
			// memory access needs to be ordered around it. A stronger order would buy nothing.
			__atomic_fetch_add(&ln->busy_ticks, svcGetSystemTick() - ts, __ATOMIC_RELAXED);
			continue;
		}

		if (!got) {
			LightEvent_Wait(&ln->work);
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
			ok = workerLoadColumn(ln, job.cx, job.cz);
			if (!ok) {
				// v1.7.1 task 48b. Timed only when it actually runs, so `generate`'s call
				// count IS the number of columns the card did not have — the other half of
				// the fresh-versus-reloaded question region_io/decode opens above.
				const uint64_t t_gen = loadprofMark();
				ok = worldgenColumn(s_gen, &ln->wgs, &ln->staging, job.cx, job.cz);
				loadprofSince(LOAD_STAGE_GENERATE, t_gen);

				// FRZ-PROBE. First column this process has ever actually generated (as
				// opposed to loaded off the card) — the marker "create new world" needs,
				// since a fresh world has nothing on the card and every column takes this
				// path. Only fires once, whichever lane gets there first.
				if (ok) BS_STAGE_ONCE(BS_STAGE_ID_WORLDGEN_COL, "WORLDGEN_COL_FIRST");
			}

			// v1.5.0 adaptive lighting: light the staged column while it is here, before
			// install can queue it ready — the jobq ordering is what guarantees light
			// exists before the main-thread mesher reads it. A refusal (budget, allocator,
			// queue overflow) leaves the column unlit, which degrades to today's look via
			// scratchFillLight's full-sky default rather than to darkness; a false return
			// from the BFS falls back to the sweep engine so a dropped entry can never
			// ship half-lit terrain. The BFS worklist is this lane's own; the sweep-engine
			// fallback is the one world/light.c CAS-claims (its s_edit_queue_busy), which is
			// what keeps two lanes out of one queue.
			if (ok && lightEnabled() && workerLightQueue(ln)) {
				const uint64_t t_light = loadprofMark();
				if (!lightPropagateColumn(&ln->staging, job.cx, job.cz, ln->lightq))
					lightRelightColumn(&ln->staging, job.cx, job.cz);
				loadprofSince(LOAD_STAGE_LIGHT, t_light);

				// FRZ-PROBE. First light pass this process has run, either engine (BFS or
				// the sweep fallback) — see app/stage_probe.h.
				BS_STAGE_ONCE(BS_STAGE_ID_LIGHT_PASS, "LIGHT_PASS_FIRST");
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
		// FRZ-AUDIT defect 1 -- same reasoning as the save-slot write above.
		__atomic_fetch_add(&ln->busy_ticks, svcGetSystemTick() - t0, __ATOMIC_RELAXED);

		LightLock_Lock(&s_lock);
		// Cannot fail: the ring has one slot per lane and a lane holds its claim until its
		// previous result was installed, so this lane cannot already have one in there. It is
		// checked anyway, and the failure releases the claim rather than keeping it — a lost
		// column is re-asked by main.c's genRequestArea, whereas a lane still holding a claim
		// for a result nobody will ever install never takes another job for the rest of the
		// session. The cheap wrong answer beats the expensive one.
		if (!laneReadyPush(&s_ready, ln->index, job.cx, job.cz, ok))
			laneClaimDrop(&s_claims, ln->index);
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

// Brings one lane up: its staging world, its scratch, its event, and its thread on `core`.
// False leaves nothing behind for the caller to unwind but the World, which it frees itself.
static bool workerLaneStart(Lane* ln, int index, int core, s32 prio)
{
	ln->index      = index;
	ln->core       = -1;
	ln->core_run   = -1;
	ln->thread     = NULL;
	ln->lightq     = NULL;
	ln->busy_ticks = 0;

	worldgenScratchInit(&ln->wgs);
	worldInit(&ln->staging);

	// Sticky rather than one-shot: the lane clears it itself before each check, so a
	// signal that arrives with nobody waiting must persist until the next wait.
	LightEvent_Init(&ln->work, RESET_STICKY);

	ln->thread = threadCreate(workerMain, ln, WORKER_STACK_BYTES, prio, core, false);
	if (!ln->thread) {
		worldExit(&ln->staging);
		return false;
	}
	ln->core = core;
	return true;
}

bool workerStart(const WorldGen* g)
{
	if (s_started) return true;

	s_gen = g;
	jobqInit(&s_queue);
	laneClaimsInit(&s_claims);
	laneReadyInit(&s_ready);
	s_quit = false;
	s_dup_dropped = 0;
	s_install_ticks = 0;
	s_save_head = s_save_tail = s_save_count = 0;
	s_saved = s_save_failed = s_loaded = 0;
	s_lanes = 0;
	// FRZ-AUDIT defect 2 (2026-09-03). s_lane[] is the watchdog thread's only lock-free window
	// onto this file (watchdog.c's hang-report core map, via workerLaneCore()/
	// workerLaneCoreRunning()), and watchdog.h:110 forbids that thread from ever taking a lock --
	// so s_lock cannot guard this array. Every write to s_lane[] in this file is a RELEASE store
	// instead, paired with the ACQUIRE loads in the two functions above. s_lanes is NOT part of
	// this: grepping watchdog.c shows it never reads s_lanes (only workerLaneCore()/
	// workerLaneCoreRunning(), which never touch s_lanes either), so it stays a plain int,
	// touched only by the main thread that also owns workerStart/workerStop, with no reader to
	// race against.
	__atomic_store_n(&s_lane[0], (Lane*)NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&s_lane[1], (Lane*)NULL, __ATOMIC_RELEASE);

	// Cleared with the rest, so the counters describe THIS world rather than every world
	// entered since the app launched. Without this a second world would inherit the first
	// one's waits and the capture would be unattributable.
	s_save_submits = s_save_waits = 0;
	s_save_wait_ticks = s_save_wait_max_ticks = 0;
	s_save_sync = s_save_overrun = 0;

	LightLock_Init(&s_lock);
	LightLock_Init(&s_fs_lock);

	// One step below the main thread. Higher numbers are lower priority on this console,
	// so these threads only run when the main thread is blocked — which it is for most of
	// every frame, waiting on the GPU. Clamped to the 0x18..0x3F userland range.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (prio + 1 <= 0x3F) prio += 1;

	// No floor against libctru's GSP interrupt-relay thread is needed here, and adding one
	// was tried and reverted on 2026-09-04. Both halves of that were measured, so record
	// them rather than let anyone re-derive the first half and stop there:
	//
	//   - gspEventThreadMain (created inside gspInit) runs at 0x1A/26 -- disassembled from
	//     libctru.a's gspgpu.o, whose threadCreate call site loads r3 = #26.
	//   - cia/blocksmith.rsf says `Priority: 16`, but makerom writes RSF + 0x20 into the
	//     exheader. Reading the ARM11 Local Capabilities priority byte out of the packed
	//     exheader of all 25 shipped .cia builds gives 0x30/48, never 16. (Container = the
	//     'NCCH' magic minus 0x100; ExHeader at +0x200; ACI at ExHeader+0x200; priority at
	//     ACI+0x0F. Plaintext because NCCH flags byte 7 is 0x05, NoCrypto.)
	//
	// So the shipped main thread has always run at 48 and this lane at 49 -- both numerically
	// above 26, i.e. correctly LOWER priority than GSP. There is no inversion to guard.
	// The rule this cost an hour to learn: a config value is an INPUT to a tool, not the
	// value that ships. Read the artifact.

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
	//
	// [2026-09-03 CORRECTION, v1.8.17] That ~15.7 ms figure is an Azahar-measured inference,
	// not a GPU reading -- Azahar has no GPU cost model. See debug/metrics.h:132.
	//
	// v1.8.4 adds core 2 in front of that on a New 3DS. Cores 2 and 3 are the New 3DS's extra
	// pair and core 2 is the one an application may be granted; nothing else on the console
	// runs on it, so a worker there stops competing with the system for core 1's slice. It is
	// a request like the others: the exheader has to have asked for it (cia/blocksmith.rsf's
	// CanAccessCore2, added in the same version), and under the Homebrew Launcher the .3dsx
	// inherits the launcher's exheader rather than ours, so this is expected to fail on the
	// .3dsx and succeed on the installed CIA. The ladder handles both without a branch here.
	//
	// The order itself lives in app/hw.c, not in this loop, because it is the part with a
	// decision in it and this file cannot be compiled on the host. tests/hw_test.c links the
	// real hwPreferredWorkerCore() and checks all four input combinations.
	int cores[HW_CORE_LADDER_MAX];

	// Two separate questions, deliberately not sharing a flag.
	//
	// Core 1 is the system core. It is rationed by APT_SetAppCpuTimeLimit, it is shared
	// with the OS services, and the measurement in worker.h says taking it makes the world
	// fill 2.5x slower at the ration every libctru example asks for. That stays behind
	// BS_WORKER_CORE, off by default, exactly as measured.
	//
	// Core 2 is a different thing entirely and the measurement above says nothing about it.
	// It exists only on a New 3DS, it is granted by the exheader's CanAccessCore2 rather
	// than by an APT ration, and nothing else on the console runs on it -- so there is no
	// ration to be starved by and no OS service to steal from. Gating it on BS_WORKER_CORE
	// was a mistake earlier in this same version: the flag defaults to 0, so the ladder
	// returned {0} on every console and the core-2 path was unreachable in a default build.
	//
	// If the exheader did not grant core 2 -- which is the expected answer under the
	// Homebrew Launcher, where the .3dsx inherits the launcher's exheader -- threadCreate
	// refuses and the ladder falls through to core 0, which is where it ran before any of
	// this. workerCore() reports where it actually landed.
	const bool cpu_time_limit_ok =
		BS_WORKER_CORE && R_SUCCEEDED(APT_SetAppCpuTimeLimit(WORKER_CPU_TIME_LIMIT));
	const int ncores = hwPreferredWorkerCore(hwIsNew3ds(), cpu_time_limit_ok, cores);

	bool up = false;
	for (int i = 0; i < ncores && !up; i++)
		up = workerLaneStart(&s_lane0, 0, cores[i], prio);
	if (!up) return false;

	__atomic_store_n(&s_lane[0], &s_lane0, __ATOMIC_RELEASE);  // FRZ-AUDIT defect 2, see above
	s_lanes   = 1;

	// ── v1.8.8: the second lane ───────────────────────────────────────────────────────
	//
	// Started only when there is a SECOND CORE for it to run on, which means both of:
	//
	//   * this is a New 3DS (laneCountFor, app/lanes.c), and
	//   * lane 0 actually landed on core 2 rather than falling back.
	//
	// The second condition is the one worth stating. If core 2 was refused — the expected
	// answer under the Homebrew Launcher, where the .3dsx inherits the launcher's exheader
	// instead of ours — then lane 0 is on core 0, and a second lane there would share one
	// core's idle time with it, buy no throughput at all, and cost ~114 KB of heap plus a
	// scheduler's worth of context switching. So in that case the game runs exactly as
	// v1.8.7 did, with one lane, and workerLanes() says 1 rather than implying otherwise.
	//
	// A refused second lane is not a failure of workerStart. The world still generates; it
	// generates at v1.8.7's speed.
	if (laneCountFor(hwIsNew3ds()) > 1 && s_lane0.core == 2) {
		Lane* const ln = (Lane*)malloc(sizeof(Lane));
		if (ln) {
			memset(ln, 0, sizeof(*ln));
			if (workerLaneStart(ln, 1, WORKER_LANE1_CORE, prio)) {
				__atomic_store_n(&s_lane[1], ln, __ATOMIC_RELEASE);  // FRZ-AUDIT defect 2
				s_lanes   = 2;
			} else {
				free(ln);
			}
		}
	}

	s_started = true;
	return true;
}

int workerCore(void)  { return s_lane[0] ? s_lane[0]->core : -1; }
int workerLanes(void) { return s_started ? s_lanes : 0; }

int workerLaneCore(int lane)
{
	if (lane < 0 || lane >= WORKER_LANES_MAX) return -1;
	// FRZ-AUDIT defect 2. ACQUIRE, paired with the RELEASE stores at every s_lane[] write site
	// above. This is the watchdog thread's read (via the hang report's core map in watchdog.c)
	// as well as main.c's debug overlay's -- only the former is forbidden a lock, so both get
	// the lock-free treatment. NULL is a valid answer at any point in this array's lifetime, not
	// just steady state. ->core needs no atomic access of its own: it is written on the main
	// thread before the lane's pointer is published, so the ACQUIRE below is what makes that
	// write visible here too (safe publication).
	Lane* const ln = (Lane*)__atomic_load_n(&s_lane[lane], __ATOMIC_ACQUIRE);
	return ln ? ln->core : -1;
}

int workerLaneCoreRunning(int lane)
{
	if (lane < 0 || lane >= WORKER_LANES_MAX) return -1;
	// FRZ-AUDIT defect 2, same reasoning as workerLaneCore() above.
	Lane* const ln = (Lane*)__atomic_load_n(&s_lane[lane], __ATOMIC_ACQUIRE);
	if (!ln) return -1;
	// FRZ-AUDIT defect 3. ACQUIRE, paired with the RELEASE store in workerMain.
	return (int)__atomic_load_n(&ln->core_run, __ATOMIC_ACQUIRE);
}

void workerStop(void)
{
	if (!s_started) return;

	LightLock_Lock(&s_lock);
	s_quit = true;
	LightLock_Unlock(&s_lock);
	for (int i = 0; i < s_lanes; i++) LightEvent_Signal(&s_lane[i]->work);

	for (int i = 0; i < s_lanes; i++) {
		threadJoin(s_lane[i]->thread, U64_MAX);
		threadFree(s_lane[i]->thread);
		s_lane[i]->thread = NULL;
	}
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
	for (int i = 0; i < s_lanes; i++) {
		worldExit(&s_lane[i]->staging);
		free(s_lane[i]->lightq);
		s_lane[i]->lightq = NULL;
	}

	// Lane 0 is a static and stays; every lane above it was malloc'd by workerStart and goes
	// back, so an Old 3DS session that never had one is byte-for-byte what it was in v1.8.7.
	//
	// FRZ-AUDIT defect 2. NULL is published (RELEASE, paired with the ACQUIRE loads in
	// workerLaneCore()/workerLaneCoreRunning()) BEFORE the lane is freed below, not after --
	// freeing first would leave a window where a concurrent lock-free reader (the watchdog)
	// could still be holding the old, now-dangling pointer. This narrows that window; it does
	// not close it outright. A load that already returned the old pointer an instant before this
	// store lands can still be preempted before it dereferences it, and closing that residual gap
	// needs either a lock (forbidden -- watchdog.h:110) or a reclamation scheme (hazard pointers,
	// RCU, or simply never freeing lane 1) bigger than an atomics-on-specific-fields pass. Left
	// open on purpose -- see the deliverable note on defect 2. Not reachable today regardless:
	// the only place in this function that can stall long enough to trip WD_TIMEOUT_MS is the
	// threadJoin loop above, which runs to completion before any code below it, this included.
	Lane* freed[WORKER_LANES_MAX] = {0};
	for (int i = 1; i < s_lanes; i++) freed[i] = s_lane[i];
	for (int i = 1; i < WORKER_LANES_MAX; i++) __atomic_store_n(&s_lane[i], (Lane*)NULL, __ATOMIC_RELEASE);
	for (int i = 1; i < WORKER_LANES_MAX; i++) free(freed[i]);
	s_lanes = 0;
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

	// Every lane, because any of them may be the idle one. A lane that is busy clears and
	// re-waits having lost nothing — see the event scheme note above s_lock.
	if (ok) for (int i = 0; i < s_lanes; i++) LightEvent_Signal(&s_lane[i]->work);
	return ok;
}

// ── v1.8.16 FRZ-FIX: the console-side halves of app/savewait.c's loop ─────────────────────
//
// Separate functions and not lambdas-by-macro because tests/savewait_test.c substitutes its
// own three, which is the only way the loop's policy can be run anywhere but on a console.

static bool workerSaveRoom(void* ud)
{
	(void)ud;
	LightLock_Lock(&s_lock);
	const bool room = s_save_count < SAVE_SLOTS;
	LightLock_Unlock(&s_lock);
	return room;
}

static bool workerSaveRingEmpty(void* ud)
{
	(void)ud;
	LightLock_Lock(&s_lock);
	const int n = s_save_count;
	LightLock_Unlock(&s_lock);
	return n == 0;
}

static void workerSaveIdle(void* ud, bool pump)
{
	(void)ud;
	// APT only once the wait is already visible (SAVEWAIT_PUMP_MS). aptMainLoop() here is a
	// re-entry into APT from the middle of a frame — genUnloadColumn runs after
	// gpuWaitPrevFrame and before this frame's C3D_FrameBegin — so it is not something to do on
	// the common path. It IS the thing that keeps HOME working once the alternative is a
	// console the player has to hold the power button to escape, which is the bug being fixed.
	if (pump) (void)aptMainLoop();
	svcSleepThread(1000000ULL);
}

static uint32_t workerNowMs(void* ud)
{
	(void)ud;
	return (uint32_t)((double)svcGetSystemTick() / CPU_TICKS_PER_MSEC);
}

// One line, once per session, straight to the card — because the counters above are only
// readable from a menu the player has to already be in, and the event this is trying to catch
// happens while they are walking. Same raw-FS writer the watchdog uses, for the same reason:
// whatever is wrong, stdio may be locked.
static void workerSaveStallNote(int32_t cx, int32_t cz)
{
	static bool done;
	if (done) return;
	done = true;

	char line[160];
	const int n = snprintf(line, sizeof line,
		"save slot wait exceeded %u ms at column %ld %ld (sync %d, overrun %d)\n",
		(unsigned)SAVEWAIT_BUDGET_MS, (long)cx, (long)cz, s_save_sync, s_save_overrun);
	if (n > 0) watchdogWriteFile("/blocksmith/savestall.txt", line, (size_t)n);
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
		uint32_t iters = 0;

		// ── v1.8.16 FRZ-FIX ────────────────────────────────────────────────────────────────
		//
		// This wait used to be a bare unbounded loop with a 1 ms sleep and nothing else, and it
		// is the mechanism behind steve's "it has froze the entire console" report: it runs on
		// the MAIN thread, from genUnloadColumn (main.c), up to seven times in the one frame a
		// ring step unloads a built edge — and while it ran, aptMainLoop() was not called, so
		// APT went unserviced, HOME stopped responding, and the console read as dead rather
		// than as slow. That is the same defect main.c records for the 2026-08-19 report; it
		// was fixed in the loading loop and left standing here.
		//
		// Three changes, each doing one thing:
		//   * APT is serviced once the wait passes one frame (SAVEWAIT_PUMP_MS), so HOME works.
		//     Below that threshold nothing changed — an ordinary save must not re-enter APT
		//     from the middle of a frame.
		//   * The wait has a deadline (SAVEWAIT_BUDGET_MS).
		//   * On expiry the column is written HERE rather than dropped. Dropping it would
		//     silently lose what the player built, which is the one outcome worse than a stall.
		//
		// The loop itself lives in app/savewait.c so it can be run on a host; this file keeps
		// only the three console-side callbacks above. tests/savewait_guard_test.c asserts that
		// no loop of its own has crept back in here.
		const SaveWait sw = {
			.room      = workerSaveRoom,
			.idle      = workerSaveIdle,
			.now_ms    = workerNowMs,
			.ud        = NULL,
			.budget_ms = SAVEWAIT_BUDGET_MS,
		};
		const SaveWaitResult r = saveWaitForRoom(&sw, NULL, &iters);

		if (iters) {
			const u64 waited = svcGetSystemTick() - t_wait;
			s_save_waits++;
			s_save_wait_ticks += waited;
			if (waited > s_save_wait_max_ticks) s_save_wait_max_ticks = waited;
		}

		if (r == SAVEWAIT_EXPIRED) {
			// The ring never freed. Write it on this thread instead of queueing it — bounded by
			// ONE column write rather than by whatever lane 0 is in the middle of.
			//
			// TryLock and not Lock: lane 0 holds s_fs_lock across regionMaintain, which is a
			// whole-region rewrite, and libctru has no timed lock — so a blocking acquire here
			// would trade an unbounded queue wait for an unbounded lock wait and fix nothing.
			// If the card is busy we fall through and keep waiting with APT alive, which is
			// survivable; losing the column is not. LightLock_TryLock returns zero on success
			// (libctru synchronization.h), checked rather than assumed.
			if (LightLock_TryLock(&s_fs_lock) == 0) {
				// s_load_buf, NOT s_save[s_save_head]. The ring is FULL on this path, which
				// means s_save_head == s_save_tail — the slot lane 0 is at this moment reading
				// out to the card — so encoding into it would corrupt the save already in
				// flight. s_load_buf is the one buffer in this file whose documented rule is
				// "only ever touched inside s_fs_lock", which is exactly where this is, so it
				// costs no extra .bss and cannot collide with lane 1's load.
				const uint32_t n = regionEncodeColumn(col, s_load_buf, sizeof(s_load_buf));
				bool written = false;
				// regionMaintain is deliberately NOT called on this path: it is the whole-region
				// rewrite, and running it on the main thread would reintroduce exactly the
				// unbounded pause this branch exists to escape. The next worker-side save
				// compacts instead.
				if (n) written = regionWriteColumn(s_world_dir, col->cx, col->cz, s_load_buf, n);
				LightLock_Unlock(&s_fs_lock);

				if (written) {
					__atomic_fetch_add(&s_saved, 1, __ATOMIC_RELAXED);
					s_save_sync++;
					workerSaveStallNote(col->cx, col->cz);
					return true;
				}
				__atomic_fetch_add(&s_save_failed, 1, __ATOMIC_RELAXED);
				return false;
			}

			// Card busy. Back to waiting, no deadline, APT pumped every iteration past the
			// threshold. Unbounded in DURATION and bounded in CONSEQUENCE: the player can press
			// HOME and leave, and nothing they built is thrown away.
			s_save_overrun++;
			workerSaveStallNote(col->cx, col->cz);
			const SaveWait sw2 = {
				.room      = workerSaveRoom,
				.idle      = workerSaveIdle,
				.now_ms    = workerNowMs,
				.ud        = NULL,
				.budget_ms = 0,
			};
			(void)saveWaitForRoom(&sw2, NULL, NULL);
		}
	}

	SaveSlot* s = &s_save[s_save_head];
	s->cx  = col->cx;
	s->cz  = col->cz;
	s->len = regionEncodeColumn(col, s->bytes, sizeof(s->bytes));
	if (s->len == 0) {
		__atomic_fetch_add(&s_save_failed, 1, __ATOMIC_RELAXED);
		return false;
	}

	// Published only once the bytes are in: lane 0 takes any slot the count says is full,
	// so incrementing first would hand it a half-written buffer.
	LightLock_Lock(&s_lock);
	s_save_head = (s_save_head + 1) % SAVE_SLOTS;
	s_save_count++;
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_lane[0]->work);
	return true;
}

void workerFlushSaves(void)
{
	if (!s_started) return;

	// v1.8.16 FRZ-FIX. Same treatment as workerSubmitSave and for the same reason, minus the
	// deadline: this is the quit and lid-close path, where a long wait is legitimate — but a
	// long wait with APT unserviced is a console that cannot be closed, and the lid-close caller
	// is by definition a player who has already asked the system for something.
	//
	// The per-iteration LightEvent_Signal is dropped: lane 0 is signalled on entry here and by
	// every workerSubmitSave, and re-signalling a sticky event forty times a second was never
	// load-bearing.
	LightEvent_Signal(&s_lane[0]->work);
	const SaveWait sw = {
		.room      = workerSaveRingEmpty,
		.idle      = workerSaveIdle,
		.now_ms    = workerNowMs,
		.ud        = NULL,
		.budget_ms = 0,
	};
	(void)saveWaitForRoom(&sw, NULL, NULL);
}

int workerLoaded(void)     { return __atomic_load_n(&s_loaded, __ATOMIC_RELAXED); }
int workerSaved(void)      { return __atomic_load_n(&s_saved, __ATOMIC_RELAXED); }
int workerSaveFailed(void) { return __atomic_load_n(&s_save_failed, __ATOMIC_RELAXED); }

bool workerInstall(World* w, int32_t* cx, int32_t* cz, bool* ok)
{
	// The lock is only valid once workerStart has run. LightLock_Init writes 1 for
	// "unlocked" and the .bss value is 0, which libctru reads as "held by someone" — so
	// locking an unstarted worker is not a no-op, it is a permanent hang. The hand-built
	// world (BS_WORLD_GEN=0) never starts the worker, so this path is real.
	if (!s_started) return false;

	int     lane = -1;
	int32_t rx = 0, rz = 0;
	bool    gen_ok = false;

	LightLock_Lock(&s_lock);
	const bool ready = laneReadyPop(&s_ready, &lane, &rx, &rz, &gen_ok);
	LightLock_Unlock(&s_lock);

	if (!ready || lane < 0 || lane >= WORKER_LANES_MAX || !s_lane[lane]) return false;

	Lane* const ln = s_lane[lane];

	// That lane is parked until its claim is dropped below — it may not take another job
	// while it holds one — so its staging world is ours for the duration of this function
	// and needs no lock of its own. This is the v1.8.7 handshake unchanged; all that moved
	// is which of the two staging worlds it applies to on any given call.
	const u64 t0 = svcGetSystemTick();
	// v1.7.1 task 48b. The same bracket s_install_ticks already keeps, reported per world load
	// instead of per session: workerInstallMs() is a since-launch total and cannot say what one
	// world entry cost. Both are kept because they are read in different places — the gen
	// overlay reads the running total, load.csv reads the per-load one.
	const uint64_t t_inst = loadprofMark();

	bool all = gen_ok;
	const Column* src = worldColumn(&ln->staging, rx, rz);
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
		// blocks, under the same parked-lane handshake that makes the chunk copy
		// lock-free. A refusal leaves the live column unlit — scratchFillLight's
		// full-sky default covers it — and the staging copy is freed with the rest of
		// the staging world below either way.
		if (all) lightColumnCopy(src, worldColumn(w, rx, rz));
	}

	// Frees the staged column and resets that lane's staging world to empty, which is the
	// state it expects to find when it takes the next job.
	worldExit(&ln->staging);

	s_install_ticks += svcGetSystemTick() - t0;
	loadprofSince(LOAD_STAGE_INSTALL, t_inst);

	// Only now is the lane let go: the drop is what allows it to pop another job, and it
	// must not be able to do that until the staging world above has been emptied.
	LightLock_Lock(&s_lock);
	laneClaimDrop(&s_claims, lane);
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&ln->work);

	if (cx) *cx = rx;
	if (cz) *cz = rz;
	if (ok) *ok = all;
	return true;
}

bool workerBusy(void)
{
	if (!s_started) return false;

	LightLock_Lock(&s_lock);
	// A held claim covers both "generating" and "finished, waiting to be installed", so the
	// ready ring does not need testing separately — but it is tested anyway, because the two
	// are only equal while the code above is correct and this is the predicate main.c waits
	// on before deciding the world is complete.
	const bool busy = laneClaimCount(&s_claims) > 0 ||
	                  laneReadyCount(&s_ready) > 0 ||
	                  jobqCount(&s_queue) > 0;
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

int workerDupDropped(void)
{
	if (!s_started) return 0;

	LightLock_Lock(&s_lock);
	const int n = s_dup_dropped;
	LightLock_Unlock(&s_lock);
	return n;
}

int workerReadyPeak(void)
{
	if (!s_started) return 0;

	LightLock_Lock(&s_lock);
	const int n = laneReadyPeak(&s_ready);
	LightLock_Unlock(&s_lock);
	return n;
}

float workerBusyMs(void)
{
	u64 t = 0;
	// FRZ-AUDIT defect 1. RELAXED load, paired with the RELAXED fetch_add in workerMain -- see
	// the comment there for why relaxed is enough. s_lanes and s_lane[i] themselves stay plain
	// reads here: this function runs on the main thread only (main.c's debug overlay), the same
	// thread that writes s_lanes and s_lane[] in workerStart/workerStop, so there is no
	// cross-thread hazard on those two -- only busy_ticks crosses threads.
	for (int i = 0; i < s_lanes; i++)
		t += __atomic_load_n(&s_lane[i]->busy_ticks, __ATOMIC_RELAXED);
	return (float)((double)t / CPU_TICKS_PER_MSEC);
}

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

int   workerSaveSync(void)    { return s_save_sync; }
int   workerSaveOverrun(void) { return s_save_overrun; }
