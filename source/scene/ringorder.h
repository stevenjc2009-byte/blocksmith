#ifndef SCENE_RINGORDER_H
#define SCENE_RINGORDER_H

#include <stdint.h>

#include "scene/render_dist.h"

// The order the streaming ring walks its columns in: NEAREST FIRST.
//
// WHY THIS EXISTS. main.c's genRequestArea() and genQueueReadyColumns() both used to walk
// `for dz = -r..r { for dx = -r..r }`, which is row-major from the ring's far corner. The
// worker's job queue is FIFO (app/worker.c's jobqPush, one JOB_GENERATE per column), so
// submission order IS arrival order IS the order terrain appears. Row-major therefore spends
// its first columns on the corner furthest from the player and reaches the player's own
// column about halfway through the pass.
//
// That is the whole of the pop-in complaint. The columns that arrive last are the ones the
// player is most likely to be looking at, when they should be the ones they are least likely
// to be looking at. Loading nearest-first does not make the world load faster — the throughput
// is whatever the worker manages either way — it makes the LATE columns the DISTANT ones, so
// what is still missing is out at the fog line instead of under the player's feet.
//
// WHY A TABLE AND NOT A SHELL WALK. The obvious cheap answer is to walk Chebyshev shells
// (d = 0, 1, 2 ... emitting the perimeter at each). That is nearest-first for a boot, and
// completely useless for the case that actually matters. When the player crosses a column
// boundary, genRecenter() re-requests only the newly entered STRIP — every column of which
// has the same Chebyshev distance r, because they all share |dx| = r. A shell walk leaves that
// strip in its original order and changes nothing at all about walking pop-in.
//
// Sorting by SQUARED EUCLIDEAN distance is what handles both: on a boot it fills outward from
// the player, and within the recentre strip it puts dz = 0 (the column dead ahead) first and
// works out to the strip's ends. dx*dx + dz*dz is exact in int — no sqrt, no float, and no
// ties broken differently on two machines.
//
// The table is built once and covers the LARGEST ring the game can ask for; a caller with a
// smaller radius walks the whole table and skips what falls outside its own bounds (the same
// max-norm test genInArea/genInMesh already apply). That costs at most RING_ORDER_COUNT - (2r+1)^2
// extra loop iterations of two compares each, against a body that submits a worker job or
// pushes a column of mesh jobs.
//
// SIZED OFF render_dist.h RATHER THAN A HAND-WRITTEN 6. world/jobq.h's JOBQ_CAP is the
// cautionary tale — main.c's own comment records it going silently stale under a render
// distance change TWICE, at RENDER_DIST_MAX 2 and again at 5, with a completely clean build
// both times. A table that is one short of the ring it is walked over would not fail to
// compile either: it would simply never emit the outermost columns, so the far edge of the
// world would stop loading. Deriving the radius here means raising RENDER_DIST_MAX cannot
// leave this behind. main.c asserts the cover explicitly against its own GEN_AREA_RADIUS_MAX.
#define RING_ORDER_MAX_RADIUS (RENDER_DIST_MAX + 1)
#define RING_ORDER_SPAN       (2 * RING_ORDER_MAX_RADIUS + 1)
#define RING_ORDER_COUNT      (RING_ORDER_SPAN * RING_ORDER_SPAN)

// int8_t because the largest offset the table can hold is RING_ORDER_MAX_RADIUS, and the
// whole table is then RING_ORDER_COUNT * 2 bytes — 338 at RENDER_DIST_MAX 5. A pair of int32
// would be 1352 bytes of .bss for numbers that never leave [-6, 6].
typedef struct {
	int8_t dx, dz;
} RingOffset;

// Every offset in [-RING_ORDER_MAX_RADIUS, RING_ORDER_MAX_RADIUS] squared, exactly once,
// ordered by increasing dx*dx + dz*dz. RING_ORDER_COUNT entries; entry 0 is always (0, 0).
//
// Built on the first call and never rebuilt. NOT thread-safe, and does not need to be: both
// callers are main.c's streaming path, which runs on the main thread only (app/worker.h's
// handshake is what keeps the worker off it).
const RingOffset* ringOrder(void);

#endif   // SCENE_RINGORDER_H
