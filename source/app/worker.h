// Terrain generation on a second thread (step 5.5).
//
// Generating one column costs two 3D fBm evaluations per underground block since step 5.4,
// which is 0.76-0.88 ms per column on the host and more on ARM11. Twenty-five of those in
// front of the first frame is a boot stall; the same twenty-five during play — which is
// what Phase 6 streaming does — would be a dropped frame every time the player crosses a
// column boundary. So generation moves off the main thread and the main thread only ever
// does the cheap half: copying a finished column into the world.
//
// v1.7.0: the figure above is the LEGACY generator's, and it still holds — re-measured at
// 0.757-0.804 ms/column over three seeds at radius 6, twice. The density-field generator
// new worlds now use (world/worldgen_density.c, including the sea-level fill and the tall
// grass scatter) costs 1.055-1.310 ms/column on the same runs, a ratio of 1.37-1.64x.
//
// That is not a budget being missed. The number is here to justify moving generation off the
// main thread at all, and the invariant below is what makes that safe: a slower worker fills
// the streaming ring more slowly, it does not drop a frame. The consequence to watch for on
// hardware is walking fast at render distance 3 out-running generation and showing
// ungenerated ground at the ring edge.
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
// v1.8.4: **none of the table above applies to core 2**, and it must not be read as if it
// did. Every row of it is core 1 -- the system core, rationed by APT_SetAppCpuTimeLimit and
// shared with the OS. Core 2 exists only on a New 3DS, is granted by the exheader's
// CanAccessCore2 rather than by an APT ration, and has nothing else running on it. So on a
// New 3DS the worker now asks for core 2 **by default**, with BS_WORKER_CORE left at 0, and
// falls back to core 0 when the grant is refused. BS_WORKER_CORE still means exactly what
// it meant: may the worker have the system core. It is still off.
//
// What that is NOT: a measurement. It is a hardware request that cannot be measured here --
// no emulator models the New 3DS core layout, and nothing in this project has run on real
// hardware since v1.2.5. What IS verified is that the ladder returns {2,0} for a New 3DS
// with no CPU-time grant (tests/hw_test.c) and that the fallback to core 0 is the same path
// the console has always taken. The win itself is unproven until it runs on a New 3DS.
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

// ── Step 8.1: why the save file is the worker's job too ───────────────────────────────
//
// Every byte that goes to or comes from the SD card is read and written on this thread, and
// on no other. That is not about frame time — though it is worth 5-20 ms a write on a real
// card, which is a dropped frame — it is about the card itself: libctru reaches the SD
// through one FS service session shared by the whole process, and two threads inside it at
// once is not a race this project wants to reason about. One thread, one file at a time,
// and the question never comes up.
//
// The main thread's half is the part that needs the World, which it owns: it encodes a
// column into a save slot and hands the *bytes* over. The worker never looks at a live
// column, and the main thread never touches a file.
//
// Loading works the same way in reverse and needs no new job type: a JOB_GENERATE first
// asks the region file for the column, and only generates when the card has nothing (or has
// something that fails its checksum). So a column the player edited comes back as they left
// it, and a column they never touched costs no read at all beyond the directory.

// Points the worker at a world directory on the SD card, e.g.
// "sdmc:/blocksmith/worlds/default". Must be called before workerStart; the string is
// copied. NULL or "" — the default — means no save file at all: every column is generated
// and none is ever written, which is what the hand-built world and the host tests want.
void workerSetWorldDir(const char* dir);

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

// MAIN THREAD ONLY. Encodes `col` into a free save slot and hands it to the worker to write.
// Returns false only when the column could not be encoded — a corrupt column, which is not a
// condition this game has a way to produce — so a false here is a bug report, not a retry.
//
// **It blocks when every slot is full**, rather than dropping the save or writing the file
// itself. Dropping would silently lose the player's building, and writing it here would put
// a second thread inside the FS session. Blocking costs at most one SD write of latency, and
// only when more dirty columns leave the ring at once than there are slots — which needs the
// player to have edited several columns and then walked far enough for all of them to unload
// in the same frame.
//
// Does nothing and returns true when no world directory is set.
bool workerSubmitSave(const Column* col);

// Blocks until every submitted save has reached the card. Called before the world is torn
// down, so "I quit the game" and "I walked away from it" save the same way.
void workerFlushSaves(void);

int   workerLoaded(void);           // columns read back from the card instead of generated
int   workerSaved(void);            // columns written to the card
int   workerSaveFailed(void);       // columns whose write was refused by the card
int   workerDropped(void);          // submissions refused because the ring was full
int   workerQueued(void);           // jobs still waiting to be picked up
float workerBusyMs(void);           // wall time the worker spent inside the generator
float workerInstallMs(void);        // wall time the main thread spent copying columns

// v1.7.1 task 48b. The save-slot wait inside workerSubmitSave, which blocks the MAIN thread
// on a worker sharing the same core. Since-launch totals, like the two above.
//
// These exist because `save_ms` alone cannot answer the question it was added for: a frame
// that never had a dirty column to save reports 0.0 ms, and so does a frame that submitted
// three and never once had to wait. Only "submits > 0 with waits == 0" says the path was
// exercised and did not block — which is the measured negative worth having. A capture with
// both at zero says nothing at all, and must not be read as a clean bill of health.
int   workerSaveSubmits(void);      // columns handed to the save ring
int   workerSaveWaits(void);        // ...of which this many blocked the main thread
float workerSaveWaitMs(void);       // total main-thread time lost to that wait
float workerSaveWaitMaxMs(void);    // the worst single wait, which is the frame spike

// Which CPU core the worker actually ended up on, not which one it asked for: 1 only if
// APT_SetAppCpuTimeLimit and the thread creation both succeeded, 0 otherwise, -1 before
// workerStart. Reported rather than assumed, because the second core is a request the
// system is allowed to refuse and a silent fallback would make every measurement taken
// afterwards mean something other than what it says.
int workerCore(void);
