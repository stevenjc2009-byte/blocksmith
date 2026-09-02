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
// **The live World is written by the main thread and by nothing else.** Each generator lane
// generates into a staging World it owns exclusively, and the main thread copies the
// result across in workerInstall(). Nothing that renders, raycasts or walks the player
// ever reads a half-generated column, and none of the per-frame world reads need a lock.
//
// The second thing that would otherwise be shared is the memory budget (world/budget.c).
// That used to be argued from the handshake below; it is not any more and has not been since
// v1.8.x — budget.c's own header explains why (the main thread allocates outside the
// handshake every time the player builds into an unallocated sky chunk) and its counters are
// a compare-and-swap loop rather than a plain `+=`. The handshake is about the STAGING
// WORLD, and about that it is exact:
//
//   lane:  wait(work) -> pop job (+claim) -> generate into staging -> push ready -> wait(work)
//   main:  pop ready  -> copy staging into the world -> free staging -> drop claim -> signal
//
// The cost of that choice, stated plainly: a lane produces **at most one column per install**,
// so with one lane and one install per frame the world fills at one column per frame — 25
// columns in 25 frames, about 0.42 s. That is far more than Phase 6 needs (walking at the
// measured 4.31 blocks/s crosses a column boundary every 3.7 s) and it buys a design with
// no locking on any per-frame path.
//
// ── v1.8.8: the SECOND lane, and what "one lane" now means ────────────────────────────
//
// On a New 3DS that actually got core 2, this file runs **two** generator lanes instead of
// one. Everything above still describes each lane; what changed is that there are two of
// them, each with its own staging World, its own 16,256-byte WorldGenScratch
// (world/worldgen_scratch.h) and its own lighting worklist, and they take jobs from the one
// shared queue.
//
// The pieces that make that safe, and where each of them lives:
//
//   * **The generator is re-entrant.** v1.8.7 moved ten file statics out of
//     world/worldgen.c and world/worldgen_density.c into a caller-owned WorldGenScratch.
//     tests/worldgen_mt_test.c is the guard: before that change two threads on COMPLETELY
//     DIFFERENT columns produced 25 of 32 columns wrong and 145 of 192 generations refused
//     outright; after it, "96+96 refused 0+0 wrong 0/32 missing 0".
//   * **No column can be handed to both lanes.** app/lanes.h's claim set. A JobQueue pop
//     already hands each JOB out once, but main.c can legitimately submit the same COLUMN
//     twice — genUnloadColumn clears its `asked` mark the moment it leaves the ring and
//     genRequestArea re-asks if the player walks back — so "each job once" is not "each
//     column once". A duplicate is refused at the pop and counted in workerDupDropped().
//   * **The finished columns queue up.** app/lanes.h's ready ring, one slot per lane, popped
//     in completion order so a lane that finished first is not made to wait behind the other.
//   * **The SD card is still single-threaded.** Every entry into world/region.c from this
//     file is inside one lock (worker.c's s_fs_lock), and saves are drained by lane 0 alone.
//     Generation — the expensive half — runs outside that lock, which is the whole point.
//   * **The lighting worklist is per-lane**, and the shared fallback queue in world/light.c
//     is CAS-claimed (its s_edit_queue_busy, added in v1.8.7 after tests/light_race_test.c
//     measured 116 of 120 relights disagreeing with the single-threaded answer).
//
// **What the second lane costs, and who pays.** Nothing on an Old 3DS: lane 0 is a static
// and lane 1 is malloc'd only when it is actually started, so an Old 3DS session allocates
// and reserves exactly what it did in v1.8.7. On a New 3DS the extra is roughly 114 KB of
// APPLICATION heap — the Lane struct (~49 KB, dominated by the 16,256-byte scratch), a 32 KB
// thread stack, and a 65,544-byte LightQueue allocated on first use — out of the measured
// 59,715,584 bytes app/heapsplit.h leaves. None of that is the linear heap and none of it is
// charged against WORLD_BUDGET_BYTES.
//
// **What the second lane cost world/budget.h, and where that was settled.** Each lane owns
// its own staging world, so two lanes can hold two staged columns at once — which is the
// point of the second lane, not an accident. BUDGET_STAGING_COLUMNS was 1 and understated the
// radius-5 worst case by exactly one column. It is 2 as of v1.8.8, and the derivation there
// now reads "169 cols + 2 staging = 11,225,808 B, 89.2 %" of the 12,582,912-byte cap. Radius
// 5 still fits and radius 6 is still refused, by 2,319,184 B. budgetClaim() never read that
// constant anyway — it compares against WORLD_BUDGET_BYTES directly — so the correction moved
// a comment, a printout and one test bound, and changed nothing a player can select.
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
// **v1.8.8, and read this before you set that flag on an installed CIA: it cannot work
// today.** cia/blocksmith.rsf:112 sets `AffinityMask : 1`. That field is the exheader's
// two-bit core-permission mask — bit 0 core 0, bit 1 core 1 — so 1 permits core 0 and
// forbids core 1 outright, whatever APT_SetAppCpuTimeLimit answers. BS_WORKER_CORE=1 would
// therefore succeed at the APT call, fail at the threadCreate, and fall silently down the
// ladder to core 0, which is where it already runs. Making it work needs `AffinityMask : 3`
// in the RSF and a reinstall, and that is a hardware change nothing here can measure.
//
// Note what this does NOT block: core 2 is granted by the separate New3DS exheader flag
// `CanAccessCore2 : true` (cia/blocksmith.rsf:130) and not by AffinityMask, so lane 0 on
// core 2 and lane 1 on core 0 are both permitted by the exheader exactly as it stands. The
// two-lane split needed no RSF change and got none.
//
// v1.8.4: **none of the core-1 table above applies to core 2**, and it must not be read as if
// it did. Every row of it is core 1 -- the system core, rationed by APT_SetAppCpuTimeLimit and
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
// measurement afterwards labelled with a core it never ran on. v1.8.8 hangs the second lane
// off exactly that answer: if lane 0 did not get core 2 there is no second core for a second
// lane to use, so none is started and workerLanes() says 1.
//
// Either way each lane runs one priority step *below* the main thread (higher number = lower
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
// Every byte a lane writes into its staging world is published by a LightLock_Unlock
// and read after a LightLock_Lock, so the barriers sit on exactly the edges that matter.
// That argument is unchanged by there being two lanes: the ready ring is pushed under the
// same lock the install pops it under.
//
// The handshake itself does the rest: the main thread only touches a lane's staging world
// while that lane's claim is held and the lane is parked in LightEvent_Wait, so the two never
// write the same memory at the same time on either core.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/world.h"
#include "world/worldgen.h"

// ── Step 8.1: why the save file is the worker's job too ───────────────────────────────
//
// Every byte that goes to or comes from the SD card is read and written on a worker lane, and
// on no other thread. That is not about frame time — though it is worth 5-20 ms a write on a
// real card, which is a dropped frame — it is about the card itself: libctru reaches the SD
// through one FS service session shared by the whole process, and two threads inside it at
// once is not a race this project wants to reason about. One file at a time, and the question
// never comes up.
//
// v1.8.8 keeps that promise with two lanes rather than dropping it: writes are lane 0's alone,
// and every read is inside worker.c's s_fs_lock, which the writes also take. What is NOT
// serialised is generation, which touches no file at all.
//
// The main thread's half is the part that needs the World, which it owns: it encodes a
// column into a save slot and hands the *bytes* over. No lane ever looks at a live
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

// Starts the lanes. `g` must outlive them — it is read, never written, and the worker keeps
// the pointer. False only if the FIRST lane or its staging world could not be created; a
// refused second lane is not a failure and leaves workerLanes() reporting 1.
bool workerStart(const WorldGen* g);

// Signals every lane to finish the column it is on, then joins them. Safe to call twice.
void workerStop(void);

// Queues a column for generation. False if the job ring is full, which is counted in
// workerDropped() — a dropped generate job is a hole in the world, not a slow frame.
bool workerSubmitColumn(int32_t cx, int32_t cz);

// MAIN THREAD ONLY. If a generated column is waiting, copies it into `w`, releases the
// staging copy and lets that lane start the next one. Returns true when a column was
// installed, and writes its coordinates and whether every chunk of it made it across —
// false in *ok means the budget or the column table refused part of the column, which is
// a hole the caller should report rather than ignore.
//
// It installs ONE column per call whatever the lane count, so with two lanes a caller that
// wants both results in the same frame has to call it twice. main.c's loading screen and its
// frame loop both do; see the workerLanes() bound at each call site.
bool workerInstall(World* w, int32_t* cx, int32_t* cz, bool* ok);

// True while any column is queued, being generated, or waiting to be installed. Goes false
// exactly once the world is complete, which is what the caller waits on before drawing the
// final report.
bool workerBusy(void);

// MAIN THREAD ONLY. Encodes `col` into a free save slot and hands it to lane 0 to write.
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
float workerBusyMs(void);           // wall time the lanes spent inside the generator, SUMMED
float workerInstallMs(void);        // wall time the main thread spent copying columns

// v1.8.8. Jobs thrown away at the pop because the other lane already held that column.
// **Not a hole in the world and not an error**: the copy in flight is the one that arrives,
// and main.c re-asks for anything that genuinely does not. It is here so that a hole can be
// told apart from a duplicate rather than both being invisible. Zero on an Old 3DS, and zero
// on a New 3DS until the player walks out of a column and back while it is being generated.
int   workerDupDropped(void);

// v1.8.8. High-water mark of the ready ring. **This is the number that says the second lane
// is really doing something**: 1 means the two lanes never once both had a finished column
// waiting, 2 means they did. It cannot exceed the lane count.
int   workerReadyPeak(void);

// v1.7.1 task 48b. The save-slot wait inside workerSubmitSave, which blocks the MAIN thread
// on a lane sharing the same core. Since-launch totals, like the two above.
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

// Which CPU core LANE 0 actually ended up on, not which one it asked for: 2 on a New 3DS
// whose exheader granted core 2, 1 only if APT_SetAppCpuTimeLimit and the thread creation
// both succeeded, 0 otherwise, -1 before workerStart. Reported rather than assumed, because
// every core above 0 is a request the system is allowed to refuse and a silent fallback would
// make every measurement taken afterwards mean something other than what it says.
int workerCore(void);

// v1.8.8. How many generator lanes are actually running: 1 or 2, and 0 before workerStart.
// **Two only on a New 3DS whose lane 0 got core 2** — see the block comment at the top of
// this file for why a second lane on the same core as the first is not worth starting.
int workerLanes(void);

// v1.8.8. Which core lane `lane` landed on, or -1 if that lane is not running. Lane 0 is
// workerCore(); lane 1 is core 0 when it exists at all.
int workerLaneCore(int lane);
