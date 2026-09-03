// Phase 0 instrument: the numbers every later optimisation decision depends on.
//
// Craftus — the only serious prior block game on this hardware — was measured as
// "almost completely GPU bound" at 45-60 fps on an Old 3DS. That conclusion was
// only reachable because it separated CPU cost from GPU cost at runtime, so this
// exists before the first block does.
//
// CPU  ms : C3D_GetProcessingTime()  — time spent building the command list
// GPU  ms : C3D_GetDrawingTime()     — time the GPU spent executing it
// cmdbuf  : C3D_GetCmdBufUsage()     — fraction of the command buffer used
// linear  : linearSpaceFree()        — free contiguous GPU-visible memory
//
// Two figures are timed by us with svcGetSystemTick rather than read from citro3d,
// because Azahar reports a constant 0.25 ms for C3D_GetDrawingTime():
//
// submit ms : wall time inside C3D_FrameEnd — flushing and kicking the GPU. Under an
//             emulator the emulated GPU work lands here, so this is the figure that
//             actually moves with draw load; on hardware it is submit cost only.
// sync   ms : wall time inside C3D_FrameBegin(C3D_FRAME_SYNCDRAW) — blocked waiting
//             for the previous frame's GPU work to finish. On hardware this is the
//             GPU-bound signal: it grows when the GPU cannot keep up.
#pragma once

#include <3ds.h>

// Opens the CSV on the SD card. Safe to call once, after gfx init.
void metricsInit(void);

// Flushes any buffered CSV rows and closes the file.
void metricsExit(void);

// Call at the very top of the frame, before any work.
void metricsFrameBegin(void);

// Call after C3D_FrameEnd — the GPU/CPU timers are only valid once the frame has
// been submitted.
void metricsFrameEnd(void);

// Call once per draw call, with that call's triangle count. GPU *load* is countable
// even where GPU *time* is not, and it is the number Phase 3 has to keep an eye on.
void metricsCountDraw(u32 tris);

// False once the console has returned a bit-identical GPU time for 120 frames, which
// means it is not timing the GPU at all (Azahar). The overlay prints "NOT TIMED".
bool metricsGpuTimed(void);

// Bracket C3D_FrameBegin with the Sync pair and C3D_FrameEnd with the Submit pair.
void metricsSyncBegin(void);
void metricsSyncEnd(void);
void metricsSubmitBegin(void);
void metricsSubmitEnd(void);

// Draws the overlay to the bottom-screen console. Rate-limited internally so the
// console text rendering does not become the thing being measured.
// `status` is an extra line describing the current stress state, or NULL.
void metricsDrawOverlay(const char* status);

// Clears `worst`, `late` and `drop` back to zero. Startup is not representative: the
// world build and the self-test both happen inside the first frame and leave a ~20 ms
// worst standing for the rest of the run, which masks whatever a later measurement was
// trying to show. Call this at the start of the window being measured.
void metricsWorstReset(void);

// Hands the host suite's summary line ("PASS 1766 checks") to the overlay, which then
// redraws it every frame on a fixed row. Without this the result is printed once during
// boot and is unreadable in practice: the suite finishes inside the first frame, long
// before the emulator window is paintable, and the boot text has scrolled away by the
// time anyone can look. Copies the string; the caller's buffer need not outlive the call.
// Truncated to 32 columns, which is all the console has.
void metricsSetSelfTest(const char* summary);

// Step 7.6's state, for the overlay row. `on` is whether the game is drawing two eyes,
// `slider` is where the console's own 3D slider sits — a different question, since 3D on
// with the slider at 0 still costs two eyes and draws two identical pictures — `vram_free` is
// what is left after the right eye's render target was claimed, and `right_eye_bytes` is what
// that target took (measured across the allocation, see screenRightEyeBytes).
void metricsSetStereo(bool on, float slider, size_t vram_free, size_t right_eye_bytes);

// v1.7.1 task 49. What the main thread actually did this frame, pushed in by main.c once per
// frame and written into the same CSV row as that frame's time.
//
// steve's report is "whenever I'm walking around in certain spots, my game FPS will just drop
// randomly". The frame-time column alone cannot answer that: it says a frame was slow, never
// where the player was or which piece of work made it slow, and a hitch that only happens in
// "certain spots" is exactly the shape of work that is triggered by *position* — a column
// boundary crossed, a column saved, a chunk of dense geometry meshed. Each field below is one
// of those suspects, so one CSV settles it by correlation instead of by argument. This lands
// BEFORE any fix on purpose: §12's "build the check first", and the project's own habit of
// plausible fixes that turn out to have addressed the wrong thing.
//
// Cost on the main thread is one struct copy per frame. Nothing here allocates, formats, or
// touches the filesystem — the writer thread does that, exactly as it already did.
typedef struct {
	// The streaming ring's centre, from the player's feet. This is the "certain spots" axis:
	// if the spikes line up with particular columns, or with the frame a column changes, the
	// answer is in this pair.
	int32_t player_cx, player_cz;

	float recenter_ms;   // inside genRecenter — nonzero ONLY on a boundary-crossing frame
	float relight_ms;    // inside the relight drain, which is deliberately unbudgeted
	float mesh_ms;       // inside both mesh drains, to be read against DRAIN_BUDGET_MS (4.0)
	float save_ms;       // blocked in workerSubmitSave waiting for one of two save slots

	u16 built;   // chunks meshed this frame, against DRAIN_MAX_CHUNKS (3)
	u16 meshq;   // chunks still waiting after the drain — stream queue plus edit queue
} MetricsWork;

// Call once per frame, any time before metricsFrameEnd. metricsFrameBegin clears it, so a
// frame that never calls this records zeros rather than repeating the previous frame's spike
// — which would make one recentre look like a permanent regression.
void metricsSetWork(const MetricsWork* w);

// Most recent per-frame figures, for callers that want to react to them.
float metricsFrameMs(void);

// ── v1.8.11 METRICS-VISIBLE: the CPU-vs-wait split, on a screen the player's build has ──
//
// docs/ROADMAP.md claims the main thread is "GPU-blocked for roughly 15.7 of every 16.71 ms"
// (grep that phrase). That was measured in Azahar, which has no GPU cost model, and it decides
// whether every CPU optimisation shipped this week is visible or invisible. Nothing in a CIA
// could answer it: metricsDrawOverlay is compiled out under BS_BOTTOM_UI (see the block above
// it), and BS_BOTTOM_UI defaults to 1.
//
// COLLECTION was never compiled out — only the drawing was. Everything above the
// `#if !BS_BOTTOM_UI` in metrics.c runs in every build, including the CSV writer thread. So
// these accessors do not re-arm anything; they read numbers the shipped build has been
// keeping all along and had no way to display.
//
// THE ARITHMETIC, and why frame - sync is the honest CPU figure on THIS frame loop:
//
//   metricsFrameBegin()                                        top of the frame
//   metricsSyncBegin(); gpuWaitPrevFrame(); metricsSyncEnd();
//   ... sim, genFollow, both mesh drains, the draw ...
//   metricsSubmitBegin(); C3D_FrameEnd(0); metricsSubmitEnd();
//   metricsFrameEnd()
//
// Named by SYMBOL and not by line number on purpose: main.c is 6,000 lines and every lane that
// touches it shifts the numbers, so a line reference here would be stale within the week and
// this project has already lost an investigation to a comment naming something that had moved.
// Every name above greps.
//
// gpuWaitPrevFrame() is, on the shipped path (its `#else`, i.e. not BS_GPU_TESTS),
// C3D_FrameBegin(C3D_FRAME_SYNCDRAW) — the VBlank wait FOLLOWED by gxCmdQueueWait(-1); its own
// header comment carries the disassembly that establishes this. It is therefore the ONLY place
// in the frame where the main thread blocks on the GPU or on the display, and the later
// C3D_FrameBegin(C3D_FRAME_SYNCDRAW) at the draw finds citro3d's inFrame flag already set and
// early-returns without waiting. C3D_FrameEnd(0) hands the command list to GX and returns
// without blocking.
//
// So: frame_ms - sync_ms is main-thread work, and sync_ms is main-thread waiting. That is the
// split the ROADMAP question needs, and it is NOT what the old overlay's "cpu" row printed:
// that row is C3D_GetProcessingTime(), which only covers citro3d's own FrameBegin..FrameEnd
// window and so misses the simulation, the recentre and both mesh drains entirely.
//
// WHAT sync_ms LUMPS TOGETHER, stated because it changes how the number is read: waiting for
// the VBlank because the frame finished early (healthy 60 fps headroom) and waiting for a GPU
// that has not finished (GPU-bound) are both inside it, and this instrumentation cannot
// separate them — SYNCDRAW does both in one call. The frame RATE separates them instead: a
// large wait at 59.8 fps is headroom, a large wait below 60 fps is the GPU. Splitting them for
// real would mean calling C3D_FrameSync() and C3D_FrameBegin(0) separately inside
// gpuWaitPrevFrame(), which is a change to the hardware-freeze fix and is deliberately not
// made here.
//
// Averaged over HISTORY_LEN frames (~0.67 s) rather than shown raw: a per-frame figure at 60 Hz
// is a blur of digits, and the overlay this replaces rate-limited itself for the same reason.
// Frame 0 is excluded — its delta is measured from metricsInit, not from a previous frame.
float metricsCpuAvgMs(void);     // mean of (frame_ms - sync_ms): work, not waiting
float metricsWaitAvgMs(void);    // mean of sync_ms: blocked on VBlank + GPU queue
float metricsFrameAvgMs(void);   // mean of frame_ms: wall clock

// The bottom-screen timing row's on/off flag, and the ONLY switch for it. Default false: this
// is a debug option reached through pause -> Options -> Debug -> "Frame timing", not a feature
// the game boots with. Nothing persists it, so it is off again after a reboot — same shape and
// same reasoning as debug/biomeborder.h's toggle.
bool metricsRowEnabled(void);
void metricsRowSetEnabled(bool on);

// Frames submitted since launch, as the denominator for the cumulative counters elsewhere:
// step 9.3's cull and sight-walk runs only mean anything as a ratio against this.
u32   metricsFrames(void);
