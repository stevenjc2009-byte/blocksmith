#pragma once

// A battery of GPU tests, for one bug: on real hardware the console freezes a moment after a
// world finishes loading, and it has never once reproduced in an emulator.
//
// Seven builds have narrowed it to a single sentence. v1.1.6's hang report, from steve's own
// console, said:
//
//   gx queue     : cap 32  queued 5  submitted 5  completed 2
//     STUCK ON   : entry 2  type 1  ProcessCommandList - OUR draw commands
//       args     : 14189c00 00002430 00000000 00000000
//
// Both screen clears completed. The GPU then took the frame's draw command list — 0x2430 bytes
// at 0x14189c00 — and never reported finishing it, so the two display transfers queued behind it
// never ran. And gxprobe.txt from the same boot measured a HEALTHY frame's list at 0x2430 bytes
// as well. A citro3d command list's length is fixed by the sequence of calls that built it, so
// the frame that hung issued the same commands as the 334 before it and differs only in the
// values inside them.
//
// So the bug is one wrong value in a ~2300-word list. This file exists to find which one in a
// single boot instead of another six.
//
// The thing that makes that possible is one line of libctru:
//
//   bool gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);   // false if the timeout expired
//
// The game waits forever (citro3d passes -1), which is why the console dies and the only
// evidence is whatever the watchdog thread can scrape afterwards. Waiting with a timeout instead
// turns the freeze into a return value: the main thread survives it, the GPU is still wedged in
// exactly the state that wedged it, and everything below can then be run against it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef BS_GPU_TESTS
#define BS_GPU_TESTS 0
#endif

#if BS_GPU_TESTS

// How long the frame loop waits for the GX queue before declaring the GPU wedged. Two seconds is
// not a guess: the queue drains in well under a millisecond in every profile run taken on this
// project, and the longest legitimate stall in the frame loop is a vblank tick at 16.71 ms. Two
// seconds is 120 frames of slack.
#define BS_GPU_WEDGE_NS (2000000000LL)

// ---------------------------------------------------------------------------------------------
// A. Pre-flight. Runs once at boot, after graphics init and before the title screen, and writes
//    sdmc:/blocksmith/selftest.txt.
//
//    Every test submits real work on a private GX queue and waits with a timeout, so a test that
//    wedges is RECORDED rather than fatal and the battery keeps going. That is the whole point of
//    running them this way: one boot returns every answer, including the answers after the first
//    failure.
//
//    If one of these fails on steve's console the bug is named without the world ever loading.
//    If they all pass, that rules out the GPU primitives themselves and the remaining suspect is
//    the content of the world's own command list — which is what B and C are for.
void gpuTestPreflight(void);

// The command-list half of the pre-flight. Split out because it needs a real citro3d command
// list, and none exists until the world draws its first frame — so this runs once, on that
// frame, and appends to the same file.
//
// It replays a list the GPU has ALREADY executed successfully: once, fifty times, and truncated
// to half with its own tail re-attached. All three must pass. This is what earns the right to
// believe the post-mortem's bisection later, because if replaying a known-good list can wedge,
// or if halving a known-good list can wedge, then every TIMEOUT the bisector reports would be an
// artefact of the method rather than evidence about the bug.
void gpuTestListProbe(const uint8_t* list, uint32_t len_bytes);

// ---------------------------------------------------------------------------------------------
// B. The per-frame validator. Walks the real command list after every C3D_FrameEnd and checks
//    the only things in a list that can point the GPU at memory it cannot read or hand it a
//    number it cannot rasterise:
//
//      - every vertex shader float uniform, for Inf and NaN (PICA float24: exponent all ones)
//      - every attribute buffer offset, against FCRAM and VRAM
//      - the index buffer offset, likewise
//      - every vertex count, for zero and for absurdity
//
//    Cheap enough to run every frame — it is a linear walk of ~2300 words — and it is the test
//    most likely to name the bug outright, because it sees the actual bytes the GPU is about to
//    be given, for every draw in the frame: chunks, the highlight cage, other players, name tags
//    and the whole bottom-screen sprite batch. scene/chunk_render.c's existing draw guard covers
//    only the first of those, and it reported clean on the console that froze.
//
//    Returns true when the list is clean. The first frame that is not is kept verbatim.
bool   gpuTestValidateList(const uint8_t* list, uint32_t len, uint32_t frame);
size_t gpuTestValidatorReport(char* buf, size_t cap, size_t len);

// ---------------------------------------------------------------------------------------------
// C. The post-mortem, run from the main thread the moment the timed wait expires, with the GPU
//    still stuck. In order, because each answer decides whether the next is worth asking:
//
//      1. Submit a trivial known-good list. If it completes, the GPU is healthy and this one
//         list is poison. If it times out too, the GPU is globally wedged and the list may be
//         innocent. That single answer splits the search in half.
//
//      2. If the GPU still works: replay the frozen list itself. If it wedges a second time it
//         is reproducibly poison, and then —
//
//      3. Bisect it. Replay the list truncated to half, a quarter, and so on, each truncation
//         terminated with the real list's own tail so it stays a valid list, until the shortest
//         prefix that still wedges is found. That names the exact command, by word offset, that
//         the GPU cannot execute. It is the difference between handing me 2300 words and handing
//         me one.
//
//    Everything is written to sdmc:/blocksmith/postmortem.txt before any of it is attempted, and
//    rewritten after each step, so a step that kills the console outright still leaves the steps
//    before it on the card.
void gpuTestPostMortem(uint32_t frame);

// The timed wait itself, for main.c to call in place of letting C3D_FrameBegin block forever.
// True if the queue drained. False means wedged, and the caller runs the post-mortem.
bool gpuTestFrameWait(void);

#endif // BS_GPU_TESTS
