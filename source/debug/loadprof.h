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
// region_io, decode, generate and light accumulate on the WORKER threads; everything else on the
// main thread. No stage is written by both sides, and the reader (loadprofEnd, main thread) runs
// after the ring is complete and every worker lane is parked with nothing outstanding. There are
// no locks here on purpose — a lock on a lane's generate path would be measuring the instrument.
//
// **v1.8.8 retracts the sentence that used to follow: "each slot has exactly one writer".** It
// stopped being true when app/worker.c gained a second generator lane on the New 3DS, because
// those four worker stages are now written by both of them. The accumulators in loadprofSince
// are `__atomic_fetch_add` RELAXED instead — see the comment at that line for why a plain `+=`
// on a uint64_t is not merely a lost count on a 32-bit part but a torn value. Still no locks.
//
// The remainder — wall minus the sum of the stages — is real and is reported as
// `unaccounted` (the CSV column is `unaccounted_ms`, renamed from `other_ms`; see below for
// why). On a vsynced loading screen most of it is the wait for VBlank inside the present
// stage's own C3D_FrameBegin, which is why `frames` is recorded beside it: a load that is
// frame-bound rather than work-bound shows up as frames * 16.71 ms accounting for the total,
// and no amount of making the stages faster will move it.
//
// ── Why unaccounted_ms is negative on every row so far, and why that is correct ───────
//
// region_io, decode, generate and light are `+=`'d from up to THREE threads at once — the
// main thread's own stages plus both New 3DS worker lanes — so their ms is CPU time summed
// across those threads, not wall time. On a New 3DS the sum of all eleven stages can
// therefore legitimately exceed wall_ms, which makes `unaccounted_ms = wall_ms - sum`
// negative. That is not a bug in the instrument and not evidence of a double-counted timer:
// it is real thread overlap being reported honestly. Each stage's own total is still correct
// and comparable to the other stages — only comparing the SUM of all eleven against wall
// time is the mistake, and only unaccounted_ms and the "% of wall" figure below (see
// loadprofFormat) are affected. Do not read a negative unaccounted_ms as a broken timer, and
// do not read the four concurrent stages' ms as "time the main thread was blocked" — read
// them as CPU-seconds spent, full stop.
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
//
// ── Why "load2.csv" and not "load.csv" (2026-09) ──────────────────────────────────────
//
// The row this writes grew two columns — `version` (BLOCKSMITH_VERSION, so a row can be
// attributed to a build) and `unix_time_s` (so two rows can be ordered across boots; see
// loadprofWrite) — and loadprofWrite only writes a header when the target file does not
// exist yet (append-only, next paragraph). A build with the wider row appending to an
// EXISTING load.csv would therefore leave a file whose header still names the OLD, narrower
// set of columns while every row appended after it carries two fields the header never
// promised. On steve's card that file is live evidence in an open freeze investigation, so
// that silent mismatch is not acceptable.
//
// Two fixes were rejected. Detecting the old header and rewriting it in place still leaves
// every OLD row under a NEW header — the same mismatch, just moved earlier in the file.
// Detecting it and rotating the old file aside needs rename() on the sdmc devoptab, which
// world/region.h:125-126 already documents as an unverified property of real hardware —
// using it here risks exactly the file this change exists to protect, for no gain over the
// option below.
//
// So the schema version is part of the FILENAME. Old builds only ever open "load.csv"; this
// build and every build after it only ever open "load2.csv". The two paths cannot collide,
// and the guarantee holds even if rename() does not work on this card, because rename() is
// never called: an old load.csv is left exactly as it is, forever, and load2.csv starts
// clean with a header that matches every row under it from the first line. Next time this
// schema changes, bump the number again rather than teaching loadprofWrite to parse a header
// it did not just write.
#define LOADPROF_PATH "sdmc:/blocksmith/load2.csv"

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

// Human-readable multi-line block: one line per stage with total ms, call count, then either
// percentage of wall or, for the four stages a worker lane can also write (region_io, decode,
// generate, light), the literal "cpu" in that column instead of a percentage — see the file
// comment above for why a wall-time percentage would be misleading there (it can read over
// 100%) rather than merely uninteresting. Then per-call microseconds, then `unaccounted` and
// the total. Returns the length written. Never writes more than `cap` bytes including the
// terminator.
int loadprofFormat(char* buf, size_t cap);

// Appends one CSV row to `path`, writing the header first if the file is not there yet. Does
// nothing when the profiler never ran. Errors are ignored on purpose: a card that will not take
// a diagnostic row must not be able to stop a world loading.
//
// The row carries `version` (BLOCKSMITH_VERSION at build time, or "(unset)" — same rule
// app/watchdog.c's hang report already uses) and `unix_time_s` (wall-clock seconds since the
// epoch from time(NULL), or 0 when time() fails, which world/worldseed.c's worldSeedMint
// already treats as the honest signature of a dead or unset RTC). Both exist so a row can be
// attributed to a build and ordered against another row from a different boot — a tick count
// alone cannot do either, because it resets to a small number every power-on and carries no
// build identity.
void loadprofWrite(const char* path);

// Zeroes everything without arming. Only the host harness needs this — the game arms with
// Begin, which resets on its own.
void loadprofReset(void);
