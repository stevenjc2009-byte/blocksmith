// The remesh backlog's bookkeeping, on its own so it can be tested on the host.
//
// This existed as three loose statics inside scene/chunk_render.c until 2026-08-18, and
// it shipped a bug that nothing could have caught there: the flag was cleared in two
// places, so the running count was decremented twice per drained chunk, went negative,
// and the moment a later edit brought it back to exactly 0 the drain's early-out latched
// on and the world silently stopped re-meshing. It could not be tested because
// chunk_render.c includes <3ds.h> and citro3d — a host build cannot link it — so the only
// way to see the fault was to press a button on an emulated console.
//
// Two things follow from that, and they are the whole reason this file exists:
//
//  - The count is never the caller's to adjust. Marking and clearing are the only way it
//    moves, and clearing something already clear is a no-op that returns false rather
//    than a decrement. The double-clear that caused the bug is now arithmetically
//    incapable of causing it.
//  - dirtyqConsistent recomputes the count by scanning and compares. Any future path that
//    corrupts the accounting fails an assertion in a one-second host run instead of
//    presenting as "placing a block does nothing" three hours later.
//
// No <3ds.h>, no floats, no allocation — same rules as the rest of source/world.
#pragma once

#include <stdbool.h>

// One flag per mesh slot. Sized from MESH_SLOTS in scene/chunk_render.h; kept as its own
// constant rather than including that header because this file must stay free of anything that
// drags in <3ds.h>. dirtyqInit rejects a capacity above this, so a pool that outgrows it fails
// at startup rather than writing past the array.
//
// ── v1.7.0: THIS CONSTANT SHIPPED v1.6.0 UNBOOTABLE ───────────────────────────────────────
//
// It said 150 and it stayed 150 when v1.6.0 task 12 raised RENDER_DIST_MAX to 3 and MESH_SLOTS
// to 294. scene/chunk_render.c's chunkRenderInit() opens with
//
//     if (!dirtyqInit(&s_dirty, MESH_SLOTS)) return false;
//
// and dirtyqInit refuses any capacity above DIRTYQ_MAX — so on the released v1.6.0 the very
// first statement of renderer init returned false, main.c:2566 printed "shader/atlas/pool init
// FAILED", and the game quit to the HOME menu without ever drawing a frame. Measured on the
// host from these real headers: dirtyqInit(&q, 294) -> false, capacity = 0.
//
// Nothing caught it. The console build was clean, and every dirtyq test in world_test.c is
// written against DIRTYQ_MAX symbolically (`dirtyqInit(&big, DIRTYQ_MAX) == true`), so the
// suite passes green at ANY value of it — this is the project's own recorded failure mode, a
// test checking its own copy of a hand-copied constant, for the third time.
//
// The number is fixed here, but the number was never the real defect: the duplication was. So
// scene/chunk_render.c now carries a _Static_assert that DIRTYQ_MAX >= MESH_SLOTS, next to the
// dirtyqInit call, in the one translation unit that can see both. That is the same device the
// TIER_* constants in that file already use and for the same stated reason — a _Static_assert
// cannot be checked against a copy and cannot be forgotten to run. Divergence is now a build
// failure instead of a black screen.
//
// Rejected: including scene/render_dist.h here to derive the value outright. It is <3ds.h>-free
// so it would compile, but source/world must not depend on source/scene — render_dist.h already
// includes world/world.h, so it would put a cycle in the directory graph to save one line the
// static assert already protects.
//
// ── v1.8.5: 392 -> 968, ahead of the ceiling rather than behind it ────────────────────────
//
// 392 is the radius-3 ring: 49 columns x COLUMN_CHUNKS. 968 is the radius-5 ring: 121 columns x
// COLUMN_CHUNKS, which is RENDER_DIST_MAX_NEW's ring and the largest pool the New 3DS's linear
// heap can hold (46,948,352 of 67,108,864 bytes — see scene/render_dist.h and the gate in
// tests/mesh_pool_bytes_test.c). It is raised NOW, while RENDER_DIST_MAX is still 3, precisely
// because the failure recorded above is what happens when this constant is raised second: the
// _Static_assert in scene/chunk_render.c's chunkRenderInit fires the moment MESH_SLOTS passes
// this number, so the ceiling lift becomes a build error instead of a boot failure — but only
// if somebody is there to fix it. Moving this first means the lift is a one-line change to
// RENDER_DIST_MAX and nothing else.
//
// What it costs, and it is a real cost paid on BOTH consoles today, including the Old 3DS that
// will never have a pool this wide: `marked` grows from 392 to 968 bytes, so sizeof(DirtyQ)
// goes from 404 to 980 — +576 bytes of .bss, once, for the single static DirtyQ in
// scene/chunk_render.c. That is the same trade the four slot-indexed tables in chunk_render.c
// make (42,336 bytes between them): the arrays are cheap enough to size at the ceiling, and it
// is the 37.7 MB of linearAlloc that had to become a runtime decision instead.
//
// dirtyqInit is still asked for the RUNTIME pool size, not for this number — see the
// dirtyqInit call in chunkRenderInit. The flags past that capacity are never marked, never
// scanned by dirtyqConsistent, and never handed to a drain.
#define DIRTYQ_MAX 968

typedef struct {
	bool marked[DIRTYQ_MAX];
	int  capacity;
	int  count;   // number of set flags in marked[0..capacity), maintained incrementally
	int  peak;    // high-water mark of count since the last dirtyqResetPeak
} DirtyQ;

// Clears every flag and sets the capacity. Returns false (and initialises to a zero
// capacity) if capacity is negative or above DIRTYQ_MAX.
bool dirtyqInit(DirtyQ* q, int capacity);

// Marks slot i. Returns true only if this call is what changed it — the caller uses that
// to count newly queued chunks, and it is why marking the same chunk twenty times in a
// frame is free.
bool dirtyqMark(DirtyQ* q, int i);

// Clears slot i. Returns true only if it had been marked. Calling this on a slot that is
// already clear is deliberately a no-op: see the header comment.
bool dirtyqClear(DirtyQ* q, int i);

bool dirtyqIsMarked(const DirtyQ* q, int i);
int  dirtyqCount(const DirtyQ* q);
int  dirtyqPeak(const DirtyQ* q);

// Resets the peak to the live count, not to zero — the backlog can already be non-empty
// at the moment of reset, and reporting a peak below the number of chunks actually queued
// is a worse lie than not resetting at all.
void dirtyqResetPeak(DirtyQ* q);

// True when the running count matches a fresh scan of the flags, and the peak is at least
// the count. This is the invariant the shipped bug broke.
bool dirtyqConsistent(const DirtyQ* q);
