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
// ── Which core, and the cache review that had to happen first ────────────────────────
//
// Step 6.3 was written to move the worker to **core 1**, the system core, which needs
// APT_SetAppCpuTimeLimit. The code to do that is here, behind BS_WORKER_CORE, and the
// default is **0 — core 0**, because the measurement said so. Frames taken for the world to
// fill from cold, Azahar, everything else identical:
//
//   core 0, no CPU time limit ......  127 frames   (worker wall time 1842 ms)
//   core 1, APT limit 30 ...........  316 frames   (worker wall time 5251 ms)
//   core 1, APT limit 80 ...........  127 frames   (worker wall time 1973 ms)
//
// APT_SetAppCpuTimeLimit is a **ceiling on the application, not a floor**: 30 means the
// worker gets at most 30% of core 1, where on core 0 it already gets the ~94% of every
// frame the main thread spends blocked. That is the whole of the 2.5x. Raising the limit to
// 80 recovers the loss and buys nothing on top of it — the main thread's own CPU time is
// 0.62-0.65 ms of a 16.71 ms frame, so there is no second thread's worth of work to overlap
// with — while taking 80% of the system core away from the OS services that share it, which
// is a hardware risk no emulator can measure. So the request is implemented, measured, and
// off. Set -DBS_WORKER_CORE=1 to turn it back on; workerCore() reports where it landed.
//
// It falls back to core 0 if either the APT call or the thread creation is refused, and
// workerCore() reports where it actually landed — a silent fallback would leave every
// measurement afterwards labelled with a core it never ran on.
//
// Either way it runs one priority step *below* the main thread (higher number = lower
// priority on this console), so on core 0 it can never delay a frame: it only gets the CPU
// when the main thread blocks, which it does for ~15.7 ms of every 16.71 ms frame inside
// C3D_FrameBegin(C3D_FRAME_SYNCDRAW).
//
// The cache review this move needed, done rather than assumed. Two threads on one core
// share an L1 data cache and two threads on two cores do not, so the staging handoff is
// only safe if the primitives around it issue real memory barriers. Disassembling
// devkitPro's libctru.a: LightLock_Unlock opens with `mcr p15, 0, r3, c7, c10, 5`, which
// is the ARM11 Data Memory Barrier, before its ldrex/strex pair; LightLock_Lock,
// LightEvent_Signal and LightEvent_WaitTimeout each contain two of the same instruction.
// Every byte the worker writes into the staging world is published by a LightLock_Unlock
// and read after a LightLock_Lock, so the barriers sit on exactly the edges that matter.
//
// The handshake itself does the rest: the main thread only touches the staging world while
// s_ready is set and the worker is parked in LightEvent_Wait, so the two never write the
// same memory at the same time on either core.
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

// Which CPU core the worker actually ended up on, not which one it asked for: 1 only if
// APT_SetAppCpuTimeLimit and the thread creation both succeeded, 0 otherwise, -1 before
// workerStart. Reported rather than assumed, because the second core is a request the
// system is allowed to refuse and a silent fallback would make every measurement taken
// afterwards mean something other than what it says.
int workerCore(void);
