// Queueing one column's chunks for meshing — the half of main.c's genQueueReadyColumns that
// can be proven on the host.
//
// v1.6.0 task 12 pulled this out of main.c for one reason: the bug it fixes is a bug in the
// ORDER of two statements, and main.c includes <3ds.h> and carries main(), so nothing in it
// can be linked into the host suite. This project's testing rule is that a test links the real
// module and never a hand-copied duplicate (see tools/run_host_tests.sh's own record of
// tests/battery_test.c and tests/sleep_test.c passing 16 and 13 checks with the real module
// deleted), and the only way to obey it here was to give the logic a file of its own. Same
// split as app/battery.c and app/debugmenu_ui.c, for the same recorded reason.
//
// ── the bug ─────────────────────────────────────────────────────────────────────────────
//
// genQueueReadyColumns used to read:
//
//     genSlotSet(s_col_queued, cx, cz);              // mark the column queued
//     for (cy...) if (!jobqPush(&s_meshq, j)) s_genr.mesh_refused++;   // then push
//
// The mark came BEFORE the pushes and did not depend on them. A push refused because the ring
// was full therefore lost its chunk permanently: the column was already flagged as queued, so
// the very next line of genQueueReadyColumns ("if already queued, continue") skipped it for
// the rest of the session. The result is a hole in the world that no amount of walking around
// repairs — the blocks are in the World, they are simply never meshed and never drawn.
//
// It was latent only because a radius-2 ring is 125 measured (150 structural) chunk meshes
// against a JOBQ_CAP that was 128. Raising the render distance to 3 makes the burst 242
// measured (392 structural) and would have made it routine.
//
// The fix is the ordering, not the capacity. Capacity moved too (world/jobq.h), but a queue
// that can be full is a queue that WILL be full eventually — under a slow frame, an edit storm
// draining the shared budget, or a distance change — so a refusal has to be recoverable on its
// own terms. That is what this file's return value is for.
//
// No <3ds.h>, no floats, no allocation — same rules as the rest of source/world.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/jobq.h"
#include "world/world.h"

// Pushes a JOB_MESH for every chunk of column (cx, cz) that has blocks in it.
//
// Returns TRUE only when every chunk that needed queueing was accepted by `q` — which is
// precisely the condition under which the caller may mark the column as queued and stop
// revisiting it. On FALSE the caller must leave the column unmarked so a later pass tries
// again; that is the whole contract, and it is what makes a refusal recoverable.
//
// `refused`, when not NULL, is incremented (not assigned) by the number of pushes the queue
// turned down, so a caller can keep a lifetime counter without a temporary. It counts every
// refusal rather than stopping at the first, because the number is the diagnostic: it says how
// far short the queue actually fell, which "the column did not fit" does not.
//
// Chunks with no Chunk object, and chunks that are all air, are skipped — not left to
// chunkRenderBuild's own all-air early-out, which claims a slot for the empty chunk it finds.
// That is right for a chunk the player dug out (it has to stay queueable) and wrong here, where
// three or four sky chunks per column would eat the pool before the ground was meshed. This
// paragraph is the reasoning that used to sit inside genQueueReadyColumns' cy loop.
//
// A retry re-pushes the chunks that DID get through the first time, because nothing here
// records which those were. That is deliberate: the duplicate is harmless — chunkRenderBuild
// reuses the slot the chunk already owns and rebuilds it in place — and it is bounded by one
// extra pass over one column, whereas the alternative (tracking per-chunk queued state) would
// put a second copy of the pool's occupancy in main.c to drift out of step with the first.
bool meshqPushColumn(JobQueue* q, const World* w, int32_t cx, int32_t cz, int* refused);
