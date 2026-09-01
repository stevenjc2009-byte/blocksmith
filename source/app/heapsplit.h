// How the process divides its memory between the linear heap and the application heap.
//
// libctru splits it for us, and its split is why the render distance was stuck. From its own
// allocateHeaps.c: the application heap is capped at 24 MB and the linear heap at 32 MB, and
// with a New 3DS's 124 MB region the caps land on linear = 32 MB, app heap = 88.95 MB. Measured
// on a real CXI boot (ExHeader SystemModeExt : 124MB): linear heap 33,554,432 with 22,479,872
// free after boot, app heap 93,270,016 with 258,048 bytes of it ever touched.
//
// That matters because the two pools serve different things and only one of them is short:
//   * the chunk mesh pool is linearAlloc  (scene/chunk_render.c:877, 907)
//   * the world store is calloc behind budgetClaim (world/world.c:90, 123), capped at 12 MB
//     by world/budget.h's WORLD_BUDGET_BYTES
// So the render distance competes for the 32 MB linear heap while ~88 MB of application heap
// sits untouched next to it. The mesh pool needs 46,948,352 bytes at radius 5 — more than the
// entire linear heap, on a console with 124 MB.
//
// Moving the boundary is measured, not assumed: a 64 MB linear heap booted on New 3DS and left
// 56,034,304 bytes free after chunkRenderInit, with the app heap still 59,715,584.
//
// It cannot be a constant. The same 64 MB linear heap on an Old 3DS, whose whole region is
// 64 MB, does not boot at all — libctru's own guard fires svcBreak(USERBREAK_PANIC) because
// the requested heaps exceed what the process was granted. That was run, not reasoned about:
// the boot produced no probe file, having never reached chunkRenderInit.
//
// Hence a policy chosen at runtime from the one number available before any service is up.
#pragma once

#include <stdbool.h>

// The largest application heap libctru will hand out by default, and the floor this policy
// refuses to go below. The world store's 12 MB cap has to fit inside the application heap with
// room for everything else on it, and 24 MB is twice that — the same number libctru itself
// picked for HEAP_SPLIT_SIZE_CAP, kept deliberately rather than invented.
#define HEAPSPLIT_APP_FLOOR_BYTES   (24u * 1024u * 1024u)

// What the linear heap is grown to when there is room. 64 MB fits the radius-5 mesh pool
// (46,948,352 bytes) with 18.3 MB to spare and was the size actually booted and measured.
// Radius 6 (72,114,176) does not fit and is not what this buys.
#define HEAPSPLIT_LINEAR_TARGET_BYTES (64u * 1024u * 1024u)

// Chooses the split for a process granted `remaining` bytes.
//
// Returns true if it grew the linear heap past libctru's default, false if it declined and the
// caller should leave libctru's own policy alone. Declining is the Old 3DS path and is a normal
// outcome, not an error.
//
// On success both out-params are set and are guaranteed to sum to no more than `remaining`,
// which is the invariant whose violation panics the console.
bool heapSplitChoose(unsigned remaining, unsigned* out_linear, unsigned* out_app);
