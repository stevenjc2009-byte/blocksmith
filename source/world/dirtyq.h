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

// One flag per mesh slot. Sized from MESH_SLOTS in scene/chunk_render.h, which is 64;
// kept as its own constant rather than including that header because this file must stay
// free of anything that drags in <3ds.h>. dirtyqInit rejects a capacity above this, so a
// pool that outgrows it fails at startup rather than writing past the array.
#define DIRTYQ_MAX 64

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
