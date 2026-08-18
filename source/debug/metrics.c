#include "debug/metrics.h"

#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CSV_PATH        "sdmc:/blocksmith/frames.csv"
#define CSV_DIR         "sdmc:/blocksmith"
#define CSV_RING        1024  // samples the writer thread can fall behind by
#define CSV_WAKE_EVERY  64    // rows between wake-ups, to keep syscalls off most frames
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
} Sample;

// How many identical readings in a row mean the console is not timing the GPU at
// all. Azahar returns a bit-identical 0.249 ms forever; real silicon never does.
#define GPU_STUCK_FRAMES 120

static FILE*  s_csv;

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

static Thread       s_writer;
static LightEvent   s_writer_wake;
static volatile bool s_writer_stop;

static u64    s_last_tick;
static u64    s_sync_tick;
static u64    s_submit_tick;
static u32    s_frame;
static Sample s_now;

static float  s_history[HISTORY_LEN];
static int    s_history_pos;

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
		              "%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%lu,%lu,%lu\n",
		              (unsigned long)s->frame, s->frame_ms, s->cpu_ms, s->gpu_ms,
		              s->sync_ms, s->submit_ms, s->cmdbuf,
		              (unsigned long)s->linear_free,
		              (unsigned long)s->draws, (unsigned long)s->tris);
		tail++;

		if (n > (int)sizeof(buf) - 160) {
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
		if (stopping) break;
	}
}

void metricsInit(void)
{
	mkdir(CSV_DIR, 0777);   // harmless if it already exists

	s_csv = fopen(CSV_PATH, "w");
	if (s_csv) {
		fprintf(s_csv, "frame,frame_ms,cpu_ms,gpu_ms,sync_ms,submit_ms,"
		               "cmdbuf_usage,linear_free_bytes,draws,tris\n");
		fflush(s_csv);
	}

	LightEvent_Init(&s_writer_wake, RESET_ONESHOT);

	// One priority step below the frame loop, on the same core: the writer only
	// gets the CPU while the main thread is blocked waiting for VBlank, which is
	// where all the idle time on this console lives.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// 32 KB, not the 8 KB a "just formats text" thread looks like it needs: newlib's
	// float formatting plus an FS IPC round trip is deep, and a stack overflow here
	// would corrupt the heap and crash at a random frame instead of failing loudly.
	s_writer = threadCreate(csvWriterThread, NULL, 32 * 1024, prio + 1, 0, false);

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

void metricsFrameBegin(void)
{
	const u64 now = svcGetSystemTick();
	s_now.frame_ms = (float)((double)(now - s_last_tick) / CPU_TICKS_PER_MSEC);
	s_last_tick = now;

	s_draws = 0;
	s_tris  = 0;
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

	if (s_now.frame > 0 && s_now.gpu_ms == gpu_prev) s_gpu_same++;
	else                                             s_gpu_same = 0;

	// Frame 0's delta is measured from metricsInit rather than from a previous
	// frame, so it is not a real frame time — skip it in the aggregates.
	if (s_now.frame > 0) {
		if (s_now.frame_ms > s_worst_ms)  s_worst_ms = s_now.frame_ms;
		if (s_now.frame_ms > MISS_MS)     s_over_budget++;
		else if (s_now.frame_ms > BUDGET_MS) s_late++;
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
	printf("                                \n");
	printf("%-32s\n", status ? status : "");
	// Walking-mode controls, which is what a playtest build boots into. The free-fly
	// camera's L/R up-down is a developer knob (main.c, BS_FLY) and is left off the
	// legend rather than listed as something that does nothing.
	printf("pad look  D-pad walk  A jump   \n");
	printf("X break  Y place     START exit\n");
}

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

float metricsFrameMs(void)  { return s_now.frame_ms; }
