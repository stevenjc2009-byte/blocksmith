#include "debug/metrics.h"

#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// For BS_BOTTOM_UI, which decides whether this file has a console to print the overlay
// into at all — gfx/screen.c's consoleInit is inside the same switch. See the comment on
// metricsDrawOverlay below.
#include "gfx/screen.h"

// OPT-METRICS, Task B. diagRetire() is main.c's own boot-fence mechanism (see
// app/diag_retire.h for the incident it was built to fix): it moves an existing file aside to
// prev-<name> before this boot can write a fresh one, so evidence from the boot that just
// froze survives the very next launch. Reused here rather than a second mechanism invented
// for frames.csv -- see the metricsInit() comment below for why the fence in main.c
// (diagFenceBoot(), main.c:3657) cannot cover this file itself.
#include "app/diag_retire.h"

#define CSV_PATH        "sdmc:/blocksmith/frames.csv"
#define CSV_PATH_PREV   "sdmc:/blocksmith/prev-frames.csv"
#define CSV_DIR         "sdmc:/blocksmith"
// OPT-METRICS Task A, v1.8.18: dropped from 1024 to 256, verified against the real struct
// size rather than assumed -- sizeof(Sample) is 68 bytes (10 u32/float fields at 4 bytes each
// plus MetricsWork's 28 bytes, no padding: every member up to the trailing two u16 is 4-byte
// aligned and 28 is already a multiple of 4), matching the 68-byte figure the ring comment
// below has always quoted. So s_ring alone goes from 1024*68 = 69,632 bytes of .bss to
// 256*68 = 17,408, a 52,224-byte cut confirmed with arm-none-eabi-size on the compiled
// object (see the OPT-METRICS report), not just this arithmetic.
//
// 256 still leaves 4x headroom over CSV_WAKE_EVERY (64) -- the interval the writer thread
// normally drains at -- and the s_ring comment further down measured a real full drain at
// ~26 ms for 256 rows, far under both one frame's 16.71 ms budget and metricsFlush()'s 250 ms
// bound, so even a completely full ring drains without the writer falling permanently behind.
// It also comfortably covers the case metricsFlush() exists for: hang.txt measured 29 frames
// drawn before a real GPU wedge, well inside 256.
#define CSV_RING        256   // samples the writer thread can fall behind by
// Rows between wake-ups, to keep syscalls off most frames. NOT lowered by the wedge-time
// flush added below (metricsFlush(), called from gpuTestPostMortem()): that call covers the
// "console froze before CSV_WAKE_EVERY frames ever drew" case for free, at zero cost on every
// frame that never wedges, so shrinking this constant would only buy back syscalls on frames
// that were never the problem. See the s_ring comment further down for the 26 ms/256-row
// measurement this value is still weighed against.
#define CSV_WAKE_EVERY  64
#define HISTORY_LEN     40    // sparkline width, in frames
#define OVERLAY_EVERY   15    // frames between console redraws (~4 Hz at 60 fps)

// The 3DS VBlank is 59.83 Hz, not 60 — one vsync is 16.71 ms, so a 16.67 ms
// threshold flags every single healthy frame. Measured on the first Phase 0 run:
// 1979 of 2129 frames "missed", at a steady 59.83 fps.
#define BUDGET_MS       16.72f
// With vsync the frame time quantises to 1, 2, 3... vsyncs, so a real dropped
// frame sits at ~33 ms. Anything past 1.5 vsyncs is a genuine miss.
#define MISS_MS         (BUDGET_MS * 1.5f)

typedef struct {
	u32   frame;
	float frame_ms;
	float cpu_ms;
	float gpu_ms;
	float sync_ms;     // blocked in C3D_FrameBegin, waiting for the last frame's GPU
	float submit_ms;   // inside C3D_FrameEnd, flushing and kicking the GPU
	float cmdbuf;
	u32   linear_free;
	u32   draws;       // draw calls this frame
	u32   tris;        // triangles handed to the GPU this frame
	// v1.7.1 task 49. What the main thread spent the frame on, so a slow row says why and
	// where instead of only how slow. See MetricsWork in the header.
	MetricsWork work;
} Sample;

// How many identical readings in a row mean the console is not timing the GPU at
// all. Azahar returns a bit-identical 0.249 ms forever; real silicon never does.
#define GPU_STUCK_FRAMES 120

static FILE*  s_csv;

// v1.7.1 task 49. This frame's work annotation, filled by main.c through metricsSetWork and
// copied into the sample at metricsFrameEnd. Main thread only, like every other counter here.
static MetricsWork s_work;

// Frame samples are handed to a writer thread through a single-producer,
// single-consumer ring. The frame loop never touches the filesystem.
//
// Why: with the CSV written from the frame loop in batches of 256, every 256th
// frame paid for the fwrite and fflush and came in at ~26 ms against a 16.71 ms
// vsync — a visible hitch roughly every 4.3 seconds. Measured cadence was exact:
// frames 512, 768, 1024 ... 9216, all 25.8-26.0 ms.
static Sample       s_ring[CSV_RING];
static volatile u32 s_head;          // written by the frame loop only
static volatile u32 s_tail;          // written by the writer thread only
static volatile u32 s_lost_rows;     // ring was full: the writer fell behind

// The host suite's own summary line, copied rather than aliased: the caller's buffer is a
// local in main() today, and a dangling pointer here would only show up as garbage on the
// one row nobody would think to distrust.
static char s_selftest[33];

// Step 7.6's three facts, pushed in by main.c once a frame rather than read from here:
// metrics.c has no business knowing about eyes, and screen.c has no business knowing about
// the overlay. Whether the mode is on, where the console's slider is, and what is left of
// VRAM after the right eye's render target.
static bool   s_stereo;
static float  s_slider;
static size_t s_vram_free;
static size_t s_right_eye_bytes;

static Thread       s_writer;
static LightEvent   s_writer_wake;
static volatile bool s_writer_stop;

// The wedge-time flush handshake between metricsFlush() (called from the MAIN thread, at the
// moment gpuTestPostMortem() decides the GPU is wedged) and csvWriterThread (a separate
// thread that is not stopped by a GPU wedge and may be anywhere in its loop when the flush is
// requested). Neither flag is ever touched by csvDrain() itself, so a flush request can never
// become a second consumer of the ring: s_head stays producer-(main-thread)-only and s_tail
// stays writer-thread-only on every path, including this one. See metricsFlush() below for
// the reasoning this exists to satisfy.
static volatile bool s_flush_pending;   // set by metricsFlush(), cleared by the writer thread
static volatile bool s_flush_done;      // set by the writer thread once it has honoured it

static u64    s_last_tick;
static u64    s_sync_tick;
static u64    s_submit_tick;
static u32    s_frame;
static Sample s_now;

static float  s_history[HISTORY_LEN];
static int    s_history_pos;

// v1.8.11 METRICS-VISIBLE. A SECOND ring, deliberately not an extra column bolted onto
// s_history above. Two reasons, both of which cost a bug somewhere else in this tree if
// ignored: s_history is read by metricsSparkline, which only exists under `#if !BS_BOTTOM_UI`,
// so widening it would tie a shipped-build readout to a debug-build-only consumer; and
// s_history is appended on EVERY frame including frame 0, whose frame_ms is measured from
// metricsInit rather than from a previous frame and is therefore not a frame time at all.
// This ring skips frame 0 for the same reason s_worst_ms and s_late already do.
//
// 320 bytes of BSS and, per frame, two float stores plus a subtract and a compare. The means
// are computed lazily in the accessors, so a build with the row switched off — which is every
// build until someone opens the debug menu and turns it on — pays only the stores.
static float  s_cpu_hist[HISTORY_LEN];    // frame_ms - sync_ms: main-thread WORK
static float  s_wait_hist[HISTORY_LEN];   // sync_ms: main-thread WAITING
// The wall clock kept as its OWN ring rather than derived as cpu+wait. The two would agree
// exactly except on a frame where the clamp above fires, and deriving it would then quietly
// report a frame time the console never had — the readout would still add up, which is what
// would make it convincing. Costs 160 bytes and cannot lie.
static float  s_wall_hist[HISTORY_LEN];
static int    s_avg_pos;
static int    s_avg_n;                    // samples in the three rings above, capped at LEN

// The bottom-screen timing row's flag. See metrics.h. Off unless the debug menu turns it on.
static bool   s_row_enabled;

static float  s_worst_ms;      // worst frame since launch, or since metricsWorstReset
static int    s_over_budget;   // frames past MISS_MS — a genuinely dropped frame
// Frames past BUDGET_MS but not past MISS_MS. With vsync these are rare and mean the
// frame's work spilled past one refresh without costing a whole second one, which is
// exactly the question step 4.5 asks about the remesh budget: "no frame over 16.71 ms".
// s_over_budget cannot answer it, because its threshold is 25.08 ms.
static int    s_late;

static u32    s_draws;         // accumulated during the frame by metricsCountDraw
static u32    s_tris;
static int    s_gpu_same;      // consecutive frames with an identical GPU reading

static float msSince(u64 then)
{
	return (float)((double)(svcGetSystemTick() - then) / CPU_TICKS_PER_MSEC);
}

// Formats and writes everything currently in the ring. Runs on the writer thread,
// or on the main thread at shutdown and if the thread could not be created.
static void csvDrain(void)
{
	static char buf[2048];   // static: the writer thread's stack is small
	int n = 0;

	if (!s_csv) {
		s_tail = s_head;   // nowhere to write; do not let the ring wedge
		return;
	}

	u32 tail = s_tail;
	while (tail != s_head) {
		const Sample* s = &s_ring[tail & (CSV_RING - 1)];
		n += snprintf(buf + n, sizeof(buf) - (size_t)n,
		              "%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%lu,%lu,%lu"
		              ",%ld,%ld,%.3f,%.3f,%.3f,%.3f,%u,%u\n",
		              (unsigned long)s->frame, s->frame_ms, s->cpu_ms, s->gpu_ms,
		              s->sync_ms, s->submit_ms, s->cmdbuf,
		              (unsigned long)s->linear_free,
		              (unsigned long)s->draws, (unsigned long)s->tris,
		              (long)s->work.player_cx, (long)s->work.player_cz,
		              s->work.recenter_ms, s->work.relight_ms,
		              s->work.mesh_ms, s->work.save_ms,
		              (unsigned)s->work.built, (unsigned)s->work.meshq);
		tail++;

		// Raised from 160 with the eight task-49 columns: the reserve has to cover the
		// LONGEST row this loop can emit, not a typical one, or the last row before a flush
		// gets truncated mid-number and every column after it in that row shifts left.
		if (n > (int)sizeof(buf) - 260) {
			fwrite(buf, 1, (size_t)n, s_csv);
			n = 0;
		}
	}

	if (n) fwrite(buf, 1, (size_t)n, s_csv);
	fflush(s_csv);

	__dsb();       // the rows are on their way out before the ring is released
	s_tail = tail;
}

static void csvWriterThread(void* arg)
{
	(void)arg;

	for (;;) {
		LightEvent_Wait(&s_writer_wake);   // one-shot: clears itself

		const bool stopping = s_writer_stop;
		csvDrain();

		// Answer a pending metricsFlush() request, if there is one. Checked AFTER csvDrain()
		// so the drain the flush asked for has actually happened by the time the caller sees
		// s_flush_done -- and checked on every wake, not only ones metricsFlush() itself
		// caused, so a flush requested while this thread was already mid-drain from an
		// unrelated signal still gets answered on the very next loop iteration rather than
		// missed. Only this thread ever sets s_flush_done, matching s_tail's rule above.
		if (s_flush_pending) {
			s_flush_pending = false;
			__dsb();          // the drain above is visible before the caller sees s_flush_done
			s_flush_done = true;
		}
		if (stopping) break;
	}
}

// OPT-METRICS, Task B. frames.csv used to be destroyed on the very next boot: fopen(CSV_PATH,
// "w") below truncates unconditionally, and main.c's diagFenceBoot() -- which retires the
// other nine diagnostic files to prev-<name> before anything can write a fresh one -- runs
// too LATE to save this one. Measured, not assumed: metricsInit() is called at main.c:4158,
// and diagFenceBoot() does not run until main.c:4287, well after this function has already
// opened (and truncated) CSV_PATH. Adding "frames.csv" to diagFenceBoot()'s table alone would
// not fix this without also moving one of those two call sites in main.c, which this lane does
// not own -- see the OPT-METRICS report for the exact hunk, offered for main.c's owner rather
// than applied here.
//
// The fix that needs no main.c change at all: retire CSV_PATH here, ourselves, before the
// fopen -- using main.c's own diagRetire() (app/diag_retire.h) rather than inventing a second
// rotation mechanism for one more file. Same probe-then-rename-with-remove-fallback the other
// nine diagnostic files already trust, so this adds no new risk beyond what the project has
// already accepted -- rename() itself is still an UNVERIFIED property of libctru's sdmc
// devoptab on real hardware (world/region.h:125-126), which is exactly why diagRetire() falls
// back to remove() on a failed rename rather than leaving two copies claiming to be current.
void metricsInit(void)
{
	mkdir(CSV_DIR, 0777);   // harmless if it already exists

	diagRetire(CSV_PATH, CSV_PATH_PREV);

	s_csv = fopen(CSV_PATH, "w");
	if (s_csv) {
		fprintf(s_csv, "frame,frame_ms,cpu_ms,gpu_ms,sync_ms,submit_ms,"
		               "cmdbuf_usage,linear_free_bytes,draws,tris,"
		               "player_cx,player_cz,recenter_ms,relight_ms,mesh_ms,save_ms,"
		               "built,meshq\n");
		fflush(s_csv);
	}

	LightEvent_Init(&s_writer_wake, RESET_ONESHOT);

	// One priority step below the frame loop, on the same core: the writer only
	// gets the CPU while the main thread is blocked waiting for VBlank, which is
	// where all the idle time on this console lives.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio += 1;
	// A floor against libctru's GSP event thread (0x1A/26) was added here on 2026-09-04 and
	// reverted the same day -- the main thread ships at 0x30/48, not at cia/blocksmith.rsf's
	// `Priority: 16`, because makerom writes RSF + 0x20. See app/worker.c for the measurement.
	// 32 KB, not the 8 KB a "just formats text" thread looks like it needs: newlib's
	// float formatting plus an FS IPC round trip is deep, and a stack overflow here
	// would corrupt the heap and crash at a random frame instead of failing loudly.
	s_writer = threadCreate(csvWriterThread, NULL, 32 * 1024, prio, 0, false);

	s_last_tick = svcGetSystemTick();
}

void metricsExit(void)
{
	if (s_writer) {
		s_writer_stop = true;
		LightEvent_Signal(&s_writer_wake);
		threadJoin(s_writer, U64_MAX);
		threadFree(s_writer);
		s_writer = NULL;
	}

	csvDrain();   // anything the writer did not reach

	if (s_csv) {
		fclose(s_csv);
		s_csv = NULL;
	}
}

// See metrics.h for the contract. This exists because frames.csv came back as 160 bytes --
// the header and nothing else -- from BOTH of steve's freeze reports: the ring only ever
// drains every CSV_WAKE_EVERY (64) frames or at metricsExit(), and a frozen console reaches
// neither. hang.txt measured 29 frames drawn. 29 < 64, and a frozen console never exits
// cleanly, so not one row ever left RAM.
//
// THE REENTRANCY TRAP THIS AVOIDS, worth stating plainly because getting it wrong turns a
// diagnostic into a crash inside the crash handler:
//
// The writer thread is the sole CONSUMER of an SPSC ring -- s_head is producer-(frame-loop)-
// only, s_tail is writer-thread-only, both documented at their declaration above. A GPU wedge
// does not stop the writer thread: nothing about csvDrain(), fwrite or fflush depends on the
// GPU, so at the instant gpuTestPostMortem() calls this, the writer could be idle in
// LightEvent_Wait, mid-csvDrain(), mid-fwrite, or mid-fflush. Calling csvDrain() directly from
// here, on the main thread, would make the main thread a SECOND consumer of the same ring: two
// threads writing s_tail without a lock (which can make it move backwards or skip entries and
// corrupt the ring, not just lose rows) and two threads potentially calling fwrite/fflush on
// the same FILE* at once -- newlib's own stdio locks are exactly why watchdog.c never touches
// stdio at all (see its fileWrite() comment), and this file has no reason to be safer than
// that one.
//
// So this does not drain the ring itself. It hands the writer thread a request and a
// completion flag and waits, BOUNDED, for the writer to answer on its own -- s_tail is still
// written ONLY by the writer thread, on every path, including this one.
//
// The bound is 250 x 1 ms = 250 ms. The s_ring comment above measured a real drain at ~26 ms
// for 256 rows; frame 29's ring holds at most 28 rows (fewer than CSV_WAKE_EVERY, or this
// path would not be needed), so the honest cost here is a small fraction of that one
// measurement, and 250 ms leaves roughly an order of magnitude of margin over it. If the
// writer thread does not answer inside that bound -- most plausibly because it is stuck
// inside fwrite/fflush against a card that has stopped responding -- this gives up rather
// than blocking the main thread indefinitely: the main thread still has to return from
// gpuTestPostMortem() and keep servicing aptMainLoop() so HOME keeps answering, and a flush
// that failed to land is only the same silence this function exists to close, while a flush
// that hangs the post-mortem would be strictly worse than that silence.
void metricsFlush(void)
{
	if (!s_writer) {
		// No writer thread exists -- threadCreate failed at boot, or metricsExit() already
		// stopped and freed it. metricsFrameEnd() below falls back to draining inline in
		// exactly this situation, and for the same reason it is safe here: with no writer
		// thread there is no second consumer to race, so this thread is already the ring's
		// only consumer.
		csvDrain();
		return;
	}

	s_flush_done = false;
	__dsb();
	s_flush_pending = true;
	LightEvent_Signal(&s_writer_wake);

	for (int i = 0; i < 250 && !s_flush_done; i++)
		svcSleepThread(1000000LL);   // 1 ms
}

void metricsFrameBegin(void)
{
	const u64 now = svcGetSystemTick();
	s_now.frame_ms = (float)((double)(now - s_last_tick) / CPU_TICKS_PER_MSEC);
	s_last_tick = now;

	s_draws = 0;
	s_tris  = 0;

	// v1.7.1 task 49. Cleared here rather than left standing, so a frame that never calls
	// metricsSetWork records zeros. Carrying the last value forward would smear one
	// boundary-crossing recentre across every frame after it, and the CSV would say the
	// spike never ended.
	memset(&s_work, 0, sizeof s_work);
}

void metricsSetWork(const MetricsWork* w)
{
	s_work = *w;
}

void metricsCountDraw(u32 tris)
{
	s_draws++;
	s_tris += tris;
}

// True when the console is clearly not measuring GPU time: the same reading, to the
// bit, for GPU_STUCK_FRAMES frames. Azahar does this (a constant 0.249 ms whether the
// frame draws 12 triangles or 2,412 — measured both ways), so the overlay says so
// instead of printing a number that looks like a measurement.
bool metricsGpuTimed(void)
{
	return s_gpu_same < GPU_STUCK_FRAMES;
}

void metricsSyncBegin(void)   { s_sync_tick = svcGetSystemTick(); }
void metricsSyncEnd(void)     { s_now.sync_ms = msSince(s_sync_tick); }
void metricsSubmitBegin(void) { s_submit_tick = svcGetSystemTick(); }
void metricsSubmitEnd(void)   { s_now.submit_ms = msSince(s_submit_tick); }

void metricsFrameEnd(void)
{
	// These are only meaningful after the frame has been submitted.
	const float gpu_prev = s_now.gpu_ms;

	s_now.frame       = s_frame++;
	s_now.cpu_ms      = C3D_GetProcessingTime();
	s_now.gpu_ms      = C3D_GetDrawingTime();
	s_now.cmdbuf      = C3D_GetCmdBufUsage();
	s_now.linear_free = (u32)linearSpaceFree();
	s_now.draws       = s_draws;
	s_now.tris        = s_tris;
	s_now.work        = s_work;

	if (s_now.frame > 0 && s_now.gpu_ms == gpu_prev) s_gpu_same++;
	else                                             s_gpu_same = 0;

	// Frame 0's delta is measured from metricsInit rather than from a previous
	// frame, so it is not a real frame time — skip it in the aggregates.
	if (s_now.frame > 0) {
		if (s_now.frame_ms > s_worst_ms)  s_worst_ms = s_now.frame_ms;
		if (s_now.frame_ms > MISS_MS)     s_over_budget++;
		else if (s_now.frame_ms > BUDGET_MS) s_late++;

		// v1.8.11 METRICS-VISIBLE. Both halves of the frame, split at the one place the main
		// thread blocks. Clamped at zero rather than allowed negative: sync_ms is timed by a
		// bracket strictly inside the frame_ms interval so the difference cannot legitimately
		// go below zero, and a negative average would read as a bug in the game rather than in
		// the instrument. See metrics.h for why frame - sync is the honest CPU figure here and
		// C3D_GetProcessingTime() (s_now.cpu_ms, three lines up) is not.
		float work_ms = s_now.frame_ms - s_now.sync_ms;
		if (work_ms < 0.0f) work_ms = 0.0f;
		s_cpu_hist[s_avg_pos]  = work_ms;
		s_wait_hist[s_avg_pos] = s_now.sync_ms;
		s_wall_hist[s_avg_pos] = s_now.frame_ms;
		s_avg_pos = (s_avg_pos + 1) % HISTORY_LEN;
		if (s_avg_n < HISTORY_LEN) s_avg_n++;
	}

	s_history[s_history_pos] = s_now.frame_ms;
	s_history_pos = (s_history_pos + 1) % HISTORY_LEN;

	const u32 head = s_head;
	if (head - s_tail < CSV_RING) {
		s_ring[head & (CSV_RING - 1)] = s_now;
		__dsb();          // the sample is complete before the writer can see it
		s_head = head + 1;
	} else {
		s_lost_rows++;    // reported on the overlay rather than silently dropped
	}

	if (((head + 1) % CSV_WAKE_EVERY) == 0) {
		if (s_writer) LightEvent_Signal(&s_writer_wake);
		else          csvDrain();   // no thread: fall back to writing inline
	}
}

// v1.7.1 task 49. Everything from here to the end of metricsDrawOverlay is compiled only
// into a build that actually has a text console to print into, and the default build does
// not have one.
//
// The two are the same switch because they are the same fact. gfx/screen.c calls consoleInit
// exactly once, inside `#if !BS_BOTTOM_UI` (screen.c:21-35); a BS_BOTTOM_UI build never calls
// it, because consoleInit claims the bottom framebuffer and the bottom screen is the game's
// UI in that build. BS_BOTTOM_UI defaults to 1 (gfx/screen.h:36-38), so in every shipped CIA
// these seventeen printf calls — with about ten float conversions among them, on every
// fifteenth frame, forever — formatted a screen of text into a stream with no console behind
// it. newlib still runs the formatting and still makes a write syscall per line for that.
//
// It is gated rather than deleted because the output is not dead in every build: the project's
// probe and measurement builds are built with `-DBS_BOTTOM_UI=0` precisely so this overlay
// comes back (see the ⚠ note in gfx/screen.h), and it is the only way a number gets read back
// out of the emulator. Deleting it would have quietly cost every future probe its instrument.
//
// What this is NOT: a frame-time fix. Bucketing 3,522 real captured frames by `frame % 15`
// gives medians of 1.796, 1.827, 1.840, 1.827, 1.821, 1.791, 1.798, 1.798, 1.798, 1.797,
// 1.826, 1.791, 1.810, 1.845 and 1.835 ms — flat, with the overlay frames indistinguishable
// from the other fourteen. Whatever this costs, Azahar was not charging for it, and no
// speed-up is claimed here. What is claimed is narrower and certain: work that produces
// nothing anyone can read is not done any more in the build the player runs.
#if !BS_BOTTOM_UI

// Oldest-to-newest ASCII bar of recent frame times, scaled so a full-height bar
// is twice the 60 fps budget. '|' marks a frame that missed the budget.
static void metricsSparkline(char* out, int cap)
{
	static const char ramp[] = " .:-=+*#%@";
	const int levels = (int)sizeof(ramp) - 2;
	int n = 0;

	for (int i = 0; i < HISTORY_LEN && n < cap - 1; i++) {
		const float ms = s_history[(s_history_pos + i) % HISTORY_LEN];
		if (ms > MISS_MS) {
			out[n++] = '|';
			continue;
		}
		int level = (int)((ms / (BUDGET_MS * 2.0f)) * (float)levels);
		if (level < 0)       level = 0;
		if (level > levels)  level = levels;
		out[n++] = ramp[level];
	}
	out[n] = '\0';
}

void metricsDrawOverlay(const char* status)
{
	if ((s_frame % OVERLAY_EVERY) != 0) return;

	char spark[HISTORY_LEN + 1];
	metricsSparkline(spark, sizeof(spark));

	const float fps = (s_now.frame_ms > 0.0f) ? (1000.0f / s_now.frame_ms) : 0.0f;

	// \x1b[1;1H homes the cursor instead of clearing, which would flicker.
	printf("\x1b[1;1H");
	printf("BLOCKSMITH  phase 5             \n");
	printf("--------------------------------\n");
	printf("fps    %6.2f  frame %6.2f ms   \n", fps, s_now.frame_ms);
	printf("cpu    %6.2f ms  (C3D process) \n", s_now.cpu_ms);
	if (metricsGpuTimed())
		printf("gpu    %6.2f ms  (C3D drawing) \n", s_now.gpu_ms);
	else
		printf("gpu       n/a  NOT TIMED (emu) \n");
	printf("submit %6.2f ms  sync %6.2f ms \n", s_now.submit_ms, s_now.sync_ms);
	printf("cmdbuf %6.1f %%                 \n", s_now.cmdbuf * 100.0f);
	printf("linear %6lu KB free           \n", (unsigned long)(s_now.linear_free / 1024));
	printf("draws  %6lu   tris %6lu      \n", (unsigned long)s_now.draws,
	       (unsigned long)s_now.tris);
	printf("worst %6.2f late %4d drop %3d\n", s_worst_ms, s_late, s_over_budget);
	printf("frame  %6lu  csvlost %4lu     \n", (unsigned long)s_now.frame,
	       (unsigned long)s_lost_rows);
	// The self-test result, on what used to be a blank spacer row. It is written every
	// frame at a fixed row rather than printed once at boot, because a line printed once
	// is gone by the time anyone can screenshot it: the suite finishes before the
	// emulator window is even paintable (by 4 seconds the game is already past frame
	// 800), and the boot text has long since scrolled away.
	printf("%-32s\n", s_selftest[0] ? s_selftest : "selftest: not run");
	printf("%-32s\n", spark);
	printf("(| = dropped frame, >25 ms)     \n");
	// Step 7.6, on what used to be a blank spacer row. `slider` is the console's own 3D
	// slider, which is not the same question as whether the game is in 3D: at slider 0 the
	// two eyes are identical pictures and the mode still costs two of them, so "on" with a
	// 0.00 slider is the one combination that looks broken and is not. `vram` is what the
	// right eye's render target left behind, and `eye` is what it took — measured across the
	// allocation in screenInit, not arithmetic. On the overlay rather than in a boot printf
	// for the same reason the self-test line is: a line printed once has scrolled away
	// before the emulator window is even paintable.
	//
	// Squeezed labels because the row has to fit 32 columns: the console drops the rest of a
	// longer line silently, so a readable-but-33-column version of this would lose the eye
	// figure off the right edge without saying so.
	printf("3d %s sl%4.2f free%4luK eye%3luK\n", s_stereo ? "on " : "off", s_slider,
	       (unsigned long)(s_vram_free / 1024), (unsigned long)(s_right_eye_bytes / 1024));
	printf("%-32s\n", status ? status : "");
	// Walking-mode controls, which is what a playtest build boots into. v1.9.0 HOTBAR-LR
	// (source/scene/hotbar.h) moved L/R off render distance: they now step the hotbar
	// selection, wrapping, with press-then-hold repeat. Render distance moved to the pause
	// menu's OPT_ROW_DIST row (scene/pausemenu.c) instead. In a BS_FLY build L/R are still
	// the camera's up and down (source/scene/camera.c reads them directly, independent of
	// this build's hotbar/render-dist wiring), which is a developer knob and is why the
	// legend does not mention it.
	printf("pad look  D-pad walk  A jump   \n");
	printf("X break  Y place  SELECT 3D    \n");
	printf("L/R hotbar  START exit         \n");
}

#else   // BS_BOTTOM_UI: there is no console, so there is nothing to draw the overlay on

// Kept as a real function rather than a macro in the header so main.c's call site, its
// rate-limit comment and the `status` string it builds all stay exactly as they are — the
// only thing that changes between the two builds is whether anything is printed.
void metricsDrawOverlay(const char* status)
{
	(void)status;
}

#endif  // !BS_BOTTOM_UI

void metricsWorstReset(void)
{
	s_worst_ms    = 0.0f;
	s_late        = 0;
	s_over_budget = 0;
}

void metricsSetSelfTest(const char* summary)
{
	size_t i = 0;

	if (summary != NULL)
		// Copied by hand rather than with strncpy: the truncation here is deliberate
		// (the console is 32 columns and silently drops the rest of a longer line
		// anyway) and strncpy's is the kind gcc warns about under -Werror.
		for (; i + 1 < sizeof(s_selftest) && summary[i] != '\0'; i++)
			s_selftest[i] = summary[i];

	s_selftest[i] = '\0';
}

void metricsSetStereo(bool on, float slider, size_t vram_free, size_t right_eye_bytes)
{
	s_stereo          = on;
	s_slider          = slider;
	s_vram_free       = vram_free;
	s_right_eye_bytes = right_eye_bytes;
}

float metricsFrameMs(void)  { return s_now.frame_ms; }

// v1.8.11 METRICS-VISIBLE. Lazy rather than accumulated: an incremental running sum would have
// to be maintained on every frame whether or not anyone is looking, and it drifts — repeatedly
// adding and subtracting floats accumulates rounding that a full re-sum does not. Forty adds,
// once per frame, only while the row is on screen.
static float meanOf(const float* h)
{
	if (s_avg_n <= 0) return 0.0f;

	float sum = 0.0f;
	for (int i = 0; i < s_avg_n; i++) sum += h[i];
	return sum / (float)s_avg_n;
}

float metricsCpuAvgMs(void)   { return meanOf(s_cpu_hist);  }
float metricsWaitAvgMs(void)  { return meanOf(s_wait_hist); }
float metricsFrameAvgMs(void) { return meanOf(s_wall_hist); }

bool metricsRowEnabled(void)          { return s_row_enabled; }
void metricsRowSetEnabled(bool on)    { s_row_enabled = on;   }

// Frames submitted since launch. Step 9.3 needs it as a denominator: "the cull ran 4,102
// times" says nothing on its own, and "4,102 culls across 4,102 frames of a 3D build" is the
// entire claim.
u32   metricsFrames(void)   { return s_frame; }
