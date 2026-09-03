// Host self-test for the v1.8.11 METRICS-VISIBLE readout: proof that source/debug/metrics.c's
// CPU-vs-wait split (metricsCpuAvgMs / metricsWaitAvgMs / metricsFrameAvgMs) is LIVE, and live
// for the RIGHT REASON.
//
// ── Why this file exists ──────────────────────────────────────────────────────────────────
//
// This project has shipped a readout wired to dead code before: main.c's own v1.8.10 note
// (grep metricsSyncBegin in main.c around line 5118) records an overlay figure that looked
// plausible every frame while reading a variable nothing wrote to. A number that LOOKS right
// proves nothing. What proves something is making the frame slower in two DIFFERENT places —
// once outside the GPU-sync bracket, once inside it — and showing that only the matching
// figure moves each time. That is the whole shape of this file.
//
// METRICS-VISIBLE (the lane that added the readout) built and ran exactly this proof, but only
// as a throwaway scratchpad harness: it said so itself when it finished, because
// tools/run_host_tests.sh was owned by another lane at the time and a _test.c dropped into the
// tree with no suite driving it is this project's own worst failure mode (see
// tests/updater_retry_test.c and the "test wired into nothing" class of bug this codebase has
// hit more than once). This file is that harness promoted into the permanent suite, wired into
// tools/run_host_tests.sh's own stanza at the bottom of that script.
//
// ── What is real and what is faked ────────────────────────────────────────────────────────
//
// The unit under test is the REAL source/debug/metrics.c, compiled unmodified from the tree
// and linked into this binary. Nothing about metrics.c's own arithmetic is re-implemented
// here. Only the libctru/citro3d surface it names is stood in for, through
// tests/metrics_timing_stub/3ds.h and tests/metrics_timing_stub/citro3d.h — and the one stub
// symbol that matters, svcGetSystemTick, is defined in THIS file rather than in either stub
// header, because owning the clock is what lets a simulated frame be handed an exact number of
// milliseconds of work and of waiting (see simFrame below). Every other libctru/citro3d symbol
// metrics.c calls (LightEvent_*, threadCreate/threadJoin/threadFree, svcGetThreadPriority,
// linearSpaceFree, C3D_GetProcessingTime/DrawingTime/CmdBufUsage) is a no-op or a constant —
// metrics.c only touches them for the CSV writer thread and the cpu_ms/gpu_ms/cmdbuf columns,
// none of which this suite asserts on.
//
// metrics.c is compiled with BS_BOTTOM_UI left at its header default (1), so the
// `#if !BS_BOTTOM_UI` console-overlay half (metrics.c's metricsDrawOverlay body) is compiled
// OUT here exactly as it is in a shipped CIA — this suite does not need a console stub because
// the code path it exercises never asks for one.
//
// ── The four arms, and why ARM 3 is the one that cannot be simplified away ───────────────────
//
//   1  baseline           2.00 ms work, 14.71 ms wait   -> cpu 2.00  wait 14.71  frame 16.71
//   2  +5 ms of WORK       outside the sync bracket      -> cpu MUST rise by 5.00, wait flat
//   3  +5 ms of WAITING    inside  the sync bracket      -> cpu MUST NOT move, wait rises 5.00
//   4  real wall clock, real busy-wait + real sleep      -> cpu tracks a genuine CPU burn
//
// Arms 2 and 3 land on the IDENTICAL total frame time (21.710 ms) by construction. That
// identity is the point, not a coincidence to tidy away: it is what makes arm 3 a discriminator
// rather than a restatement of arm 2. metrics.h's own header comment gives the arithmetic —
// metricsCpuAvgMs() is the mean of (frame_ms - sync_ms), i.e. work, and metricsWaitAvgMs() is
// the mean of sync_ms, i.e. waiting on the GPU/VBlank inside gpuWaitPrevFrame(). The bug this
// project has actually shipped (see the main.c note cited above) is a "cpu" readout that is
// really just frame_ms with the subtraction dropped. Under that exact bug, arm 2 STILL PASSES:
// injecting 5 ms outside the sync bracket raises frame_ms by 5 ms either way, so a broken
// "cpu = frame_ms" readout rises by 5 ms in arm 2 same as a correct one, and a suite that only
// ran arm 2 would call that a pass. Only arm 3 tells them apart — 5 ms injected INSIDE the sync
// bracket must move wait_ms and must NOT move cpu_ms, and a broken "cpu = frame_ms" readout
// fails exactly that assertion, verbatim confirmed below (see the file-level comment on
// testFrameTimingArms for the recorded red-arm run). An "inject work, watch the number rise"
// test alone would have proved nothing about which half of the split is actually being read.
// Do not delete arm 3 to "simplify" this file to a single positive case — it is the only
// negative case here, and it is the one the suite exists for.
//
// 40 frames is HISTORY_LEN in metrics.c (the sparkline/average window): SETTLE below is double
// that, so the window has to turn over completely and then some between arms, or an arm's
// average still carries the previous arm's frames — which would hide a dead readout behind a
// number that merely looks different rather than one that is provably wrong.
//
// ── What this suite does NOT check ────────────────────────────────────────────────────────
//
// Nothing about metricsDrawOverlay's actual pixels, and nothing about real 3DS/Azahar timing
// behaviour — arm 4 uses the HOST machine's own CLOCK_MONOTONIC and a host busy-wait, which
// proves the arithmetic tracks a genuine CPU burn rather than a genuine sleep, not that the
// numbers a player would see on real hardware match these tolerances. That gap needs a console
// and eyes, same as every other visual/hardware claim in this codebase.
// clock_gettime/CLOCK_MONOTONIC/nanosleep (arm 4's real-clock half) are POSIX.1-2001, not
// ISO C11, so glibc hides them under -std=c11's strict-ANSI mode without this. Every other
// stanza in tools/run_host_tests.sh compiles with plain -std=c11 -Wall -Wextra -Werror, so
// this is fixed here rather than by asking that script to carry a flag no other stanza needs.
#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <3ds.h>

#include "debug/metrics.h"

// ── The clock the unit under test reads ────────────────────────────────────────────────────
static u64  g_tick;        // manual mode (arms 1-3): advanced by this file, in system ticks
static bool g_real_clock;  // arm 4: read the host machine's own monotonic clock instead

u64 svcGetSystemTick(void)
{
	if (!g_real_clock) return g_tick;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double ns = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
	return (u64)(ns * (CPU_TICKS_PER_MSEC / 1.0e6));
}

static void advanceMs(double ms) { g_tick += (u64)(ms * CPU_TICKS_PER_MSEC); }

// ── Everything else metrics.c names but this suite never exercises the behaviour of ────────
// (the CSV writer thread and the three citro3d timing getters). No-ops and constants only --
// see the file header above for why none of these feed a CHECK().
void LightEvent_Init(LightEvent* e, int r) { (void)e; (void)r; }
void LightEvent_Wait(LightEvent* e)        { (void)e; }
void LightEvent_Signal(LightEvent* e)      { (void)e; }
Thread threadCreate(void (*fn)(void*), void* a, size_t s, int p, int c, bool d)
{ (void)fn; (void)a; (void)s; (void)p; (void)c; (void)d; return NULL; }
void threadJoin(Thread t, u64 to) { (void)t; (void)to; }
void threadFree(Thread t)         { (void)t; }
s32  svcGetThreadPriority(s32* o, u32 h) { (void)h; if (o) *o = 0x30; return 0; }
size_t linearSpaceFree(void) { return 0; }
float C3D_GetProcessingTime(void) { return 0.0f; }
float C3D_GetDrawingTime(void)    { return 0.25f; }
float C3D_GetCmdBufUsage(void)    { return 0.0f; }

// ── A frame, shaped exactly like main.c's ───────────────────────────────────────────────────
//
//   metricsFrameBegin()                                       main.c:4792
//   metricsSyncBegin(); gpuWaitPrevFrame(); metricsSyncEnd();  main.c:5124-5126
//   ... work ...
//   metricsFrameEnd()                                         main.c:5856
// Named by symbol rather than by line number in this comment on purpose -- main.c is 6,000+
// lines and shifts under every lane that touches it, so a line reference here would go stale
// within the week. The bracket SHAPE is what this suite depends on, not the line it sits on.
static void simFrame(double wait_ms, double work_ms)
{
	metricsFrameBegin();

	metricsSyncBegin();
	advanceMs(wait_ms);      // gpuWaitPrevFrame(): VBlank + gxCmdQueueWait
	metricsSyncEnd();

	advanceMs(work_ms);      // sim, genFollow, mesh drains, the draw, C3D_FrameEnd
	metricsFrameEnd();
}

// A frame whose work is a GENUINE CPU burn against the machine's own clock (arm 4), so the
// arm cannot be an artefact of this file's own fake tick source.
static volatile double g_sink;
static void burnMs(double ms)
{
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		for (int i = 0; i < 2000; i++) g_sink += (double)i * 1.000001;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		const double el = ((double)t1.tv_sec - (double)t0.tv_sec) * 1e3
		                + ((double)t1.tv_nsec - (double)t0.tv_nsec) / 1e6;
		if (el >= ms) return;
	}
}

static void sleepMs(double ms)
{
	struct timespec ts;
	ts.tv_sec  = (time_t)(ms / 1000.0);
	ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
	nanosleep(&ts, NULL);
}

// ── CHECK / CHECK_MSG: same shape tests/weatherdraw_test.c and source/world/inventory_test.c
// use, so a failure here reads the same way a failure anywhere else in this codebase does.
static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                          \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

#define CHECK_MSG(cond, ...) do {                                                 \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d: ", __LINE__);                                     \
			printf(__VA_ARGS__);                                                   \
			printf("\n");                                                          \
			if (!s_first[0]) {                                                     \
				char msg_[160];                                                    \
				snprintf(msg_, sizeof(msg_), __VA_ARGS__);                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, msg_); \
			}                                                                      \
		}                                                                          \
	} while (0)

// How many CHECK()/CHECK_MSG()s this suite makes on a healthy tree. Unlike
// world/inventory_test.c's INVENTORY_TEST_EXPECTED_CHECKS (see that file's own comment for the
// fuller reasoning this pin exists for), nothing here loops over a production constant that
// could shrink and quietly drop checks -- every arm runs a fixed SETTLE frames and asserts a
// fixed set of columns. The hazard this guards against instead is a check silently deleted (or
// an early return added) during a future edit to this file: the suite would still print PASS,
// just for fewer promises than it used to make. 15 is copied from this suite's own standalone
// run ("metrics timing self-test: PASS  15 checks"), not computed from the arm count by hand.
#define METRICS_TIMING_EXPECTED_CHECKS 15

static void checkCountPin(void)
{
	if (s_checks == METRICS_TIMING_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < METRICS_TIMING_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  METRICS_TIMING_EXPECTED_CHECKS to go green.\n",
		       METRICS_TIMING_EXPECTED_CHECKS - s_checks, METRICS_TIMING_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         METRICS_TIMING_EXPECTED_CHECKS - s_checks,
			         METRICS_TIMING_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If added on purpose, set METRICS_TIMING_EXPECTED_CHECKS in\n"
		       "  tests/metrics_timing_test.c to %d.\n",
		       s_checks - METRICS_TIMING_EXPECTED_CHECKS, METRICS_TIMING_EXPECTED_CHECKS, s_checks,
		       s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - METRICS_TIMING_EXPECTED_CHECKS,
			         METRICS_TIMING_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// 40 frames is metrics.c's HISTORY_LEN; see the file header for why SETTLE is double that.
#define SETTLE 80

static void runArm(const char* name, double wait_ms, double work_ms,
                    double* cpu, double* wait, double* frame)
{
	for (int i = 0; i < SETTLE; i++) simFrame(wait_ms, work_ms);
	*cpu   = (double)metricsCpuAvgMs();
	*wait  = (double)metricsWaitAvgMs();
	*frame = (double)metricsFrameAvgMs();
	printf("%s\n   cpu %.3f   wait %.3f   frame %.3f   (fps %.2f)\n",
	       name, *cpu, *wait, *frame, (*frame > 0.0) ? 1000.0 / *frame : 0.0);
}

// The toggle defaults OFF, which is what keeps the readout a debug option rather than
// something the game boots with (metrics.h's own comment on metricsRowEnabled says the same).
static void testRowToggleDefaultsOff(void)
{
	printf("toggle default\n");
	CHECK_MSG(!metricsRowEnabled(), "metricsRowEnabled() at startup: got %d, want 0 (off)",
	          metricsRowEnabled());
	metricsRowSetEnabled(true);
	CHECK_MSG(metricsRowEnabled(), "metricsRowEnabled() after SetEnabled(true): got %d, want 1",
	          metricsRowEnabled());
	metricsRowSetEnabled(false);
	CHECK_MSG(!metricsRowEnabled(), "metricsRowEnabled() after SetEnabled(false): got %d, want 0",
	          metricsRowEnabled());
	printf("\n");
}

// The four arms described in the file header. Kept as one function rather than split into four
// independent test*() functions because arms 2 and 3 are only meaningful AS DELTAS against
// arm 1's baseline -- splitting them apart would either duplicate the baseline run three times
// (tripling SETTLE's 80-frame cost per arm for no gain) or hide the fact that "cpu rose by
// 5.00" and "cpu did NOT move" are the same comparison against the same reference point.
//
// RED ARM, recorded 2026-09-03: a SCRATCH COPY of source/debug/metrics.c (never the tree file,
// confirmed by md5 before and after -- see this suite's tools/run_host_tests.sh stanza comment)
// sabotaged the same way METRICS-VISIBLE's own scratchpad harness proved this suite can catch --
// `float work_ms = s_now.frame_ms - s_now.sync_ms;` changed to
// `float work_ms = s_now.frame_ms;` (dropping the subtraction, so "cpu" becomes the whole
// frame). Verbatim result against the sabotaged copy:
//
//   ARM 1  baseline: 2.00 ms work, 14.71 ms wait
//      cpu 16.710   wait 14.710   frame 16.710   (fps 59.84)
//     FAIL L318: cpu: got 16.710, want 2.00 +/-0.01
//   ARM 2  +5.00 ms of WORK (outside the sync bracket)
//      cpu 21.710   wait 14.710   frame 21.710   (fps 46.06)
//   ARM 3  RED CONTROL: +5.00 ms of WAITING (inside the bracket)
//      cpu 21.710   wait 19.710   frame 21.710   (fps 46.06)
//     FAIL L334: cpu must NOT move: got delta 5.000
//   ARM 4  real clock, real 6 ms busy-wait + real 8 ms sleep, 80 frames
//      cpu 14.094   wait 8.091   frame 14.094
//     FAIL L360: cpu must track the 6 ms BURN, not the 8 ms sleep: got 14.094
//     FAIL L362: cpu + wait must account for the frame: cpu+wait=22.185 frame=14.094
//   metrics timing self-test: FAIL 4/15  L318 cpu: got 16.710, want 2.00 +/-0.01
//
// (Exact line numbers are as of this run; a later edit to this file can shift them by a line
// or two the way any comment naming a line number can go stale -- the RUN is authoritative
// here, not this comment, same rule world/inventory_test.c's own check-count note states.)
//
// Arm 1's ABSOLUTE check (cpu should be 2.00) catches this sabotage immediately, because a
// broken "cpu = frame_ms" reads 16.71 even at baseline. That is expected and welcome, not a
// substitute for arm 3: it is arm 2's checks that stay entirely green under this sabotage --
// no FAIL line appears under ARM 2 above -- because "cpu rose by the injected 5.00 ms" is true
// whether cpu is computed correctly or is just frame_ms (5 ms landed outside the sync bracket
// raises frame_ms by 5 ms either way). Arm 3 is what catches that a DELTA-only reading of the
// pair would have missed: "cpu must NOT move" fails with delta 5.000, the tell that the 5 ms
// injected INSIDE the sync bracket leaked into the wrong column. A suite that trusted arm 2's
// delta alone -- or that dropped the absolute arm-1 checks and reasoned only in deltas, the way
// a reader skimming "number went up, looks right" would -- goes straight past this bug; arm 3
// is where it cannot hide.
static void testFrameTimingArms(void)
{
	double c1, w1, f1, c2, w2, f2, c3, w3, f3;

	runArm("ARM 1  baseline: 2.00 ms work, 14.71 ms wait", 14.71, 2.00, &c1, &w1, &f1);
	CHECK_MSG(fabs(c1 - 2.00) < 0.01, "cpu: got %.3f, want 2.00 +/-0.01", c1);
	CHECK_MSG(fabs(w1 - 14.71) < 0.01, "wait: got %.3f, want 14.71 +/-0.01", w1);
	CHECK_MSG(fabs(f1 - 16.71) < 0.01, "frame: got %.3f, want 16.71 +/-0.01", f1);
	printf("\n");

	runArm("ARM 2  +5.00 ms of WORK (outside the sync bracket)", 14.71, 7.00, &c2, &w2, &f2);
	CHECK_MSG(fabs((c2 - c1) - 5.00) < 0.02,
	          "cpu must RISE by the injected 5.00 ms: got delta %.3f", c2 - c1);
	CHECK_MSG(fabs(w2 - w1) < 0.02, "wait must NOT move: got delta %.3f", w2 - w1);
	CHECK_MSG(fabs((f2 - f1) - 5.00) < 0.02, "frame must rise by 5.00 ms: got delta %.3f", f2 - f1);
	printf("\n");

	runArm("ARM 3  RED CONTROL: +5.00 ms of WAITING (inside the bracket)", 19.71, 2.00,
	       &c3, &w3, &f3);
	// This is the check that fails if cpu is wired to frame_ms instead of frame_ms - sync_ms --
	// see the file header and the recorded red-arm output above this function.
	CHECK_MSG(fabs(c3 - c1) < 0.02, "cpu must NOT move: got delta %.3f", c3 - c1);
	CHECK_MSG(fabs((w3 - w1) - 5.00) < 0.02,
	          "wait must RISE by exactly 5.00 ms: got delta %.3f", w3 - w1);
	CHECK_MSG(fabs((f3 - f1) - 5.00) < 0.02,
	          "frame must rise by 5.00 ms, same total as arm 2: got delta %.3f", f3 - f1);
	printf("\n");

	// Arm 4: the machine's real clock and a real busy-wait, so the result cannot be an
	// artefact of arms 1-3's fake tick source. Predicted BEFORE running: cpu ~= 6 ms,
	// wait ~= 8 ms, frame ~= 14 ms. Tolerances are wide because a real sleep overshoots and a
	// real busy-wait can be preempted; the claim under test is that cpu tracks the BURN and not
	// the SLEEP -- a 6-vs-8 separation, not a decimal place.
	g_real_clock = true;
	printf("ARM 4  real clock, real 6 ms busy-wait + real 8 ms sleep, %d frames\n", SETTLE);
	for (int i = 0; i < SETTLE; i++) {
		metricsFrameBegin();
		metricsSyncBegin();
		sleepMs(8.0);       // stands in for the block in gpuWaitPrevFrame()
		metricsSyncEnd();
		burnMs(6.0);        // genuine CPU work
		metricsFrameEnd();
	}
	const double c4 = (double)metricsCpuAvgMs();
	const double w4 = (double)metricsWaitAvgMs();
	const double f4 = (double)metricsFrameAvgMs();
	printf("   cpu %.3f   wait %.3f   frame %.3f\n", c4, w4, f4);
	CHECK_MSG(fabs(c4 - 6.0) < 1.5, "cpu must track the 6 ms BURN, not the 8 ms sleep: got %.3f", c4);
	CHECK_MSG(fabs(w4 - 8.0) < 3.0, "wait must track the 8 ms SLEEP: got %.3f", w4);
	CHECK_MSG(fabs((c4 + w4) - f4) < 0.5,
	          "cpu + wait must account for the frame: cpu+wait=%.3f frame=%.3f", c4 + w4, f4);
	printf("\n");
	g_real_clock = false;
}

int main(void)
{
	printf("CPU_TICKS_PER_MSEC = %.3f\n\n", CPU_TICKS_PER_MSEC);

	testRowToggleDefaultsOff();
	testFrameTimingArms();

	checkCountPin();

	if (s_fails == 0)
		printf("metrics timing self-test: PASS  %d checks\n", s_checks);
	else
		printf("metrics timing self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}
