// v1.7.1 task 48b: the world-load instrument.
//
// steve, playing released v1.7.0: "I was just trying to improve loading times of worlds ... as
// soon as I start making some changes, the world takes quite a long time to load, almost
// doubles in the time of creating a world."
//
// There was no number behind that. The figure the project had been quoting — a 20,481 ms
// "frame" in the Azahar CSV — is an artefact and not a measurement: debug/metrics.c's frame_ms
// is wall clock between two IN-WORLD frame boundaries, so the row that straddles a world exit
// and a world entry spans the pause menu, the title screen, the world list, and every sleep and
// PrintWindow the automation harness took in between. The same row reads cpu_ms 3.220,
// mesh_ms 0.004, recenter_ms 0.001. It measures the test script. Every "load stall" figure ever
// reported off it — 57,846 / 23,891 / 58,966 / 23,557 / 54,905 / 20,482 ms — is that artefact.
//
// This file is the replacement, and it is deliberately NOT part of the CSV ring. metrics.c's
// ring is a single-producer/single-consumer frame ring with a writer thread, sized and
// documented around one row per frame; a load is one event with a dozen named stages and two
// producer threads, which is a different shape. Extending the ring to carry it would mean
// widening every frame row for a thing that happens twice a session. So: same directory, same
// style, its own file, and the ring is untouched.
//
// ── What it measures ──────────────────────────────────────────────────────────────────
//
// The wall clock runs from the moment the player's world has been picked (main.c, just above
// saveWorldDir) to the frame runLoadingScreen hands control to the game loop — i.e. to the
// first frame the player can actually move in. Inside that, every stage below is a real
// function on the real path, timed at its own call site:
//
//   worlddir   main.c saveWorldDir()          three mkdirs and a write probe on the card
//   played     main.c worldHasBeenPlayed()    opendir/readdir looking for one *.bsr
//   setup      main.c genStart()              genVersionResolve, worldgenInit, registry
//                                             sidecar, workerStart — everything but the two above
//   region_io  worker.c workerLoadColumn()    regionReadColumnCached, once per column
//   decode     worker.c workerLoadColumn()    regionDecodeColumn, once per SAVED column
//   generate   worker.c JOB_GENERATE          worldgenColumn, once per column NOT on the card
//   light      worker.c JOB_GENERATE          lightPropagateColumn / lightRelightColumn
//   install    worker.c workerInstall()       chunkDecompressAll + worldSetChunkAll per chunk,
//                                             plus lightColumnCopy and the staging worldExit
//   queue      main.c genQueueReadyColumns()  the ring scan that pushes a column's chunks
//   mesh       main.c genDrainMesh()          chunkRenderBuild, once per chunk
//   present    main.c runLoadingScreen()      C3D_FrameBegin(SYNCDRAW) .. C3D_FrameEnd
//
// region_io, decode, generate and light accumulate on the WORKER thread; everything else on the
// main thread. No stage is written by both, which is the whole of the thread-safety argument:
// each slot has exactly one writer, and the reader (loadprofEnd, main thread) runs after the
// ring is complete and the worker is parked with nothing outstanding. There are no locks here
// on purpose — a lock on the worker's generate path would be measuring the instrument.
//
// The remainder — wall minus the sum of the stages — is real and is reported as `other`. On a
// vsynced loading screen most of it is the wait for VBlank inside the present stage's own
// C3D_FrameBegin, which is why `frames` is recorded beside it: a load that is frame-bound
// rather than work-bound shows up as frames * 16.71 ms accounting for the total, and no
// amount of making the stages faster will move it.
//
// ── Cost when nothing is loading ───────────────────────────────────────────────────────
//
// One bool test. loadprofMark() returns 0 the moment the profile is not active and every
// call site is written as mark/since, so an inactive profiler does not read the clock, does
// not touch the accumulators, and cannot be told apart from the code before it. It is armed by
// loadprofBegin and disarmed by loadprofEnd, both of which are called once per world entry.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
	LOAD_STAGE_WORLDDIR = 0,
	LOAD_STAGE_PLAYED,
	LOAD_STAGE_SETUP,
	LOAD_STAGE_REGION_IO,
	LOAD_STAGE_DECODE,
	LOAD_STAGE_GENERATE,
	LOAD_STAGE_LIGHT,
	LOAD_STAGE_INSTALL,
	LOAD_STAGE_QUEUE,
	LOAD_STAGE_MESH,
	LOAD_STAGE_PRESENT,
	LOAD_STAGE_COUNT
} LoadStage;

// Where the console writes the row. Beside frames.csv, in the directory metricsInit already
// makes, so nothing here has to mkdir anything.
#define LOADPROF_PATH "sdmc:/blocksmith/load.csv"

// Appended, never truncated: the interesting comparison is one world entry against the next,
// and a file that is rewritten every boot can only ever hold the last one.
typedef struct {
	uint64_t ticks[LOAD_STAGE_COUNT];   // raw platform ticks; see loadprofTicksPerUs
	uint32_t calls[LOAD_STAGE_COUNT];
	uint64_t wall_ticks;
	uint32_t frames;                    // loading-screen frames presented
	int      reloaded;                  // 1 when the world had a region file before this entry
	char     world[32];
} LoadProfile;

// Ticks per microsecond on this platform. 268.111856 on the console (SYSCLOCK_ARM11), 1000 on
// the host, where the tick is a nanosecond. Exposed so a caller can convert without knowing
// which it is.
double loadprofTicksPerUs(void);

// Arms the profiler and starts the wall clock. `world` may be NULL. `reloaded` is what
// worldHasBeenPlayed() said, recorded rather than derived so the row can be read on its own.
// Calling it twice without an End in between resets everything, which is what a second world
// entry in one session wants.
void loadprofBegin(const char* world, int reloaded);

// Records the answer once it is known. The game arms the profiler BEFORE the card is asked
// whether this world has been played — that question is itself one of the stages, and starting
// the clock after it would leave its cost outside the wall time it belongs to — so `reloaded`
// arrives a moment late. No-op when the profiler is not armed.
void loadprofSetReloaded(int reloaded);

// Stops the wall clock and disarms. Every Mark/Since after this is a no-op until the next
// Begin. Safe when never armed.
void loadprofEnd(void);

int loadprofActive(void);

// Reads the clock, or returns 0 when the profiler is not armed. The 0 is the guard: a tick
// value of 0 is not otherwise reachable (the system tick counts from console boot, and the
// host clock from the epoch or from boot depending on the platform), so loadprofSince can use
// it as "this bracket was opened while disarmed, drop it" without a second flag at every call
// site.
uint64_t loadprofMark(void);

// Adds now - mark to a stage and counts one call. No-op when mark is 0 or the profiler was
// disarmed in between — a bracket that spans loadprofEnd is dropped whole rather than charged
// half.
void loadprofSince(LoadStage s, uint64_t mark);

// One loading-screen frame presented. Counted separately from LOAD_STAGE_PRESENT's call count
// because a build with no bottom target draws nothing and still spends the frame.
void loadprofFrame(void);

const LoadProfile* loadprofGet(void);

// Short lowercase stage name, as it appears in the CSV header and the human block.
const char* loadprofStageName(LoadStage s);

// Human-readable multi-line block: one line per stage with total ms, call count, percentage of
// wall, and per-call microseconds, then `other` and the total. Returns the length written.
// Never writes more than `cap` bytes including the terminator.
int loadprofFormat(char* buf, size_t cap);

// Appends one CSV row to `path`, writing the header first if the file is not there yet. Does
// nothing when the profiler never ran. Errors are ignored on purpose: a card that will not take
// a diagnostic row must not be able to stop a world loading.
void loadprofWrite(const char* path);

// Zeroes everything without arming. Only the host harness needs this — the game arms with
// Begin, which resets on its own.
void loadprofReset(void);
