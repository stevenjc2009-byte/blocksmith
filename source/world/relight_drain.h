// The per-frame budget over world/relightq.h's worklist (v1.8.3).
//
// WHY THIS IS A MODULE AND NOT SIX LINES IN main().
//
// It was six lines in main(), and being there is exactly why it went three releases with a
// comment that asserted the opposite of what it did. source/main.c carries main() and includes
// <3ds.h>, so nothing in it links into any host binary and nothing in it can be tested here.
// world/meshq.c was split out of main.c's genQueueReadyColumns for the same reason and is the
// precedent this follows — see that file's stanza in tools/run_host_tests.sh, which states the
// rule outright: a test links the real module rather than a copy of it.
//
// WHAT WAS WRONG WITH THE DRAIN. main.c ran `while (relightqPop(...)) lightRelightColumn(...)`
// under a comment claiming it was multiplayer-only and bounded by 49. Both were false:
//
//   * Single player feeds it. main.c's world edit hook calls waterNotify(), water.c's change
//     hook is onWaterChange(), that calls onRemoteEdit(), and onRemoteEdit() calls
//     relightqPush(). Digging into a lake runs this drain with no server in sight.
//   * The binding cap is RELIGHTQ_CAP = 64, not 49. RENDER_DIST_MAX_COLUMNS is 49, but the
//     LOADED set is main.c's generation area, GEN_AREA_SPAN^2 = 81 columns, so 49 was never
//     the bound on how many distinct columns could be pushed either.
//
// MEASURED for v1.8.3 rather than quoted, because the tree's three figures for one
// lightRelightColumn disagree by 5x. 240 samples, gcc -O1, six column profiles: med 0.156 ms,
// p95 0.227 ms, max 0.287 ms. A full 64-entry drain is therefore 9.98 ms at the median and
// 18.4 ms at the max against a 16.71 ms frame — a dropped frame on the HOST, and worse on a
// 268 MHz ARM11 with no L2, where nothing has run since v1.2.5. See source/main.c's
// RELIGHT_BUDGET_MS for the whole measurement and the budget derived from it.
//
// THE CONTRACT. One column always completes, however small the budget — a drain that can do
// nothing never drains, and the column is stale until it does. Everything after the first is
// optional and is checked against BOTH limits, after the work and before the next pop, which is
// the shape scene/chunk_render.c's chunkRenderDrainDirty already uses and for its stated reason:
// a clock can only stop the loop once a column has already overrun, so the count is what makes
// the worst case a number that can be written down.
//
// Whatever is not drained STAYS QUEUED. relightqPop is the only thing that removes an entry and
// this never pops one it does not then relight, so nothing is dropped: a column that misses this
// frame is relit on a later one. See the note in main.c on what the deferral costs.
//
// No <3ds.h>: the clock is injected, so main.c passes svcGetSystemTick and the host suite passes
// a counter it controls. That is what makes the time half of the budget testable at all.
#pragma once

#include <stdint.h>

#include "world/relightq.h"
#include "world/world.h"

// Returns a monotonically non-decreasing tick count. Any unit, as long as budget_ticks is in
// the same one.
typedef uint64_t (*RelightClockFn)(void);

// Relights up to max_columns columns from q, stopping early once budget_ticks have elapsed.
// Returns how many were relit.
//
// Zero when the light engine is off, when any argument is NULL, or when q is empty — in all
// three cases q is left exactly as it was found. A max_columns or budget_ticks of zero (or a
// negative max_columns) still relights ONE column if there is one: both limits are examined
// only after a relight has completed.
int relightDrain(World* w, RelightQueue* q, int max_columns,
                 uint64_t budget_ticks, RelightClockFn now);
