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

// Most recent per-frame figures, for callers that want to react to them.
float metricsFrameMs(void);

// Frames submitted since launch, as the denominator for the cumulative counters elsewhere:
// step 9.3's cull and sight-walk runs only mean anything as a ratio against this.
u32   metricsFrames(void);
