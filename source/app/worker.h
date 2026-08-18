// Terrain generation on a second thread (step 5.5).
//
// Generating one column costs two 3D fBm evaluations per underground block since step 5.4,
// which is 0.76-0.88 ms per column on the host and more on ARM11. Twenty-five of those in
// front of the first frame is a boot stall; the same twenty-five during play — which is
// what Phase 6 streaming does — would be a dropped frame every time the player crosses a
// column boundary. So generation moves off the main thread and the main thread only ever
// does the cheap half: copying a finished column into the world.
//
// ── The one invariant that makes this safe ────────────────────────────────────────────
//
// **The live World is written by the main thread and by nothing else.** The worker
// generates into a staging World it owns exclusively, and the main thread copies the
// result across in workerInstall(). Nothing that renders, raycasts or walks the player
// ever reads a half-generated column, and none of the per-frame world reads need a lock.
//
// The second thing that would otherwise be shared is the memory budget (world/budget.c):
// its counters are three plain statics and a claim from two threads at once would corrupt
// them. Rather than make them atomic, the handshake below makes allocation strictly
// alternate — the worker allocates only while the main thread is waiting for it, and the
// main thread allocates only while the worker is waiting for the main thread:
//
//   worker:  wait(work) -> pop job -> generate into staging -> signal(ready) -> wait(work)
//   main:    see ready  -> copy staging into the world -> free staging -> signal(work)
//
// The cost of that choice, stated plainly: the worker produces **at most one column per
// install**, so with one install per frame the world fills at one column per frame — 25
// columns in 25 frames, about 0.42 s. That is far more than Phase 6 needs (walking at the
// measured 4.31 blocks/s crosses a column boundary every 3.7 s) and it buys a design with
// no locking on any per-frame path.
//
// ── Why core 0 ───────────────────────────────────────────────────────────────────────
//
// The worker runs on the application core at one priority step *below* the main thread, so
// it can never delay a frame: it only gets the CPU when the main thread blocks, which it
// does for ~15.7 ms of every 16.71 ms frame inside C3D_FrameBegin(C3D_FRAME_SYNCDRAW).
// Core 1 is the system core and needs APT_SetAppCpuTimeLimit, which is step 6.3 — and
// moving there is not just a parameter change: two threads on one core share a data cache,
// two threads on two cores do not, so the staging handoff would need a cache review first.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/world.h"
#include "world/worldgen.h"

// Starts the thread. `g` must outlive the worker — it is read, never written, and the
// worker keeps the pointer. False if the thread or its staging world could not be created.
bool workerStart(const WorldGen* g);

// Signals the thread to finish the column it is on, then joins it. Safe to call twice.
void workerStop(void);

// Queues a column for generation. False if the job ring is full, which is counted in
// workerDropped() — a dropped generate job is a hole in the world, not a slow frame.
bool workerSubmitColumn(int32_t cx, int32_t cz);

// MAIN THREAD ONLY. If a generated column is waiting, copies it into `w`, releases the
// staging copy and lets the worker start the next one. Returns true when a column was
// installed, and writes its coordinates and whether every chunk of it made it across —
// false in *ok means the budget or the column table refused part of the column, which is
// a hole the caller should report rather than ignore.
bool workerInstall(World* w, int32_t* cx, int32_t* cz, bool* ok);

// True while any column is queued, being generated, or waiting to be installed. Goes false
// exactly once the world is complete, which is what the caller waits on before drawing the
// final report.
bool workerBusy(void);

int   workerDropped(void);          // submissions refused because the ring was full
int   workerQueued(void);           // jobs still waiting to be picked up
float workerBusyMs(void);           // wall time the worker spent inside the generator
float workerInstallMs(void);        // wall time the main thread spent copying columns
