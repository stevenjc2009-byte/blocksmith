#pragma once

// The state machine behind the "loading world" screen: what stage generation is at, how far
// along it is, and — the part this file exists for — whether it has stopped moving.
//
// Before this screen existed, main() called genWaitSpawnColumn(), which spun on the worker
// *without* calling aptMainLoop(). That is not a slow boot, it is a hung console: with the
// APT loop unserviced the HOME button stops responding and the only way out is holding the
// power button. Reported from real hardware on 2026-08-19 — "it's frozen my three d s".
//
// So the wait is now a real frame loop, and this is the half of it that can be proven on the
// host: no <3ds.h>, no <citro3d.h>, no globals. It is handed a sample of counters once per
// frame and answers with a phase. scene/loading_draw.c turns that answer into pixels, and
// main.c owns the frame loop and the input.
//
// The stall phase is not a nicety. A loading screen that can itself sit at "45%" forever is
// the same bug in a prettier shirt, so this file's contract is that it must eventually leave
// LOADING_GENERATING/LOADING_BUILDING whatever the world does: either to LOADING_READY, or
// to LOADING_STALLED with a reason string the player can photograph.

#include <stdbool.h>

typedef enum {
	LOADING_GENERATING = 0,  // waiting for the column the player will stand in
	LOADING_BUILDING,        // that column and its eight neighbours are in; geometry pending
	LOADING_READY,           // safe to hand over to the game
	LOADING_STALLED,         // nothing has moved for LOADING_STALL_FRAMES, or the worker died
} LoadingPhase;

// ~15 s at the project's measured 59.83 fps. Chosen against the real cost of the thing it is
// waiting for, not picked round: the boot generates one area ring — 25 columns at render
// distance 1, 49 at distance 2 — and the worker's own column job is the unit of progress, so
// a ring that has produced nothing at all for fifteen seconds is not slow, it is stuck.
// Erring long is the safe direction here; a false stall on a slow card would tell the player
// something is wrong when it is not.
#define LOADING_STALL_FRAMES 900

// One frame's worth of counters, all read by main.c from the streaming ring and the mesh
// queue. Everything here is a plain number on purpose — this file must not be able to reach
// into the world, or it could not be tested without one.
typedef struct {
	int  columns_in;      // area-ring columns generated and installed
	int  columns_total;   // how many the ring wants (span * span)
	int  meshes;          // chunk meshes built since the boot began
	int  mesh_queued;     // mesh jobs still waiting
	bool spawn_ready;     // the player's own column is installed
	// Corrected 2026-09-01. This said "its eight neighbours are too", describing a 3x3 gate.
	// The implementation has never done that: main.c sets it from `in >= total`, where total is
	// span*span — the WHOLE ring, 49 columns at radius 3. The difference is not academic. Read
	// as 3x3, the loading screen looks like it should close as soon as the player's immediate
	// surroundings exist, which makes reordering the build queue nearest-first look like a
	// direct fix for pop-in. It is not: loading.c's exit test is a conjunction over the entire
	// ring, so reordering changes which column is built first and cannot close the screen one
	// frame sooner. Measured in v1.8.6's evidence pass; the optimisation was dropped over it.
	bool ring_ready;      // every column the ring wants is installed, so chunks can be meshed
	bool worker_busy;     // the generator thread still has work outstanding
	int  columns_failed;  // installed with holes
	int  submit_failed;   // never made it into the worker's queue at all
} LoadingSample;

typedef struct {
	LoadingPhase phase;
	int  frames;                // frames since the screen opened
	int  frames_since_progress; // frames since `progress` last changed
	long progress;              // the monotone scalar the stall test watches
	int  percent;               // 0..100, high-water mark; read with loadingPercent()
	LoadingSample last;         // the most recent sample, for the counters line
	char reason[80];            // why it stalled; empty in every other phase
} LoadingState;

void loadingInit(LoadingState* st);

// Advances the machine by one frame and returns the new phase. Once it reaches
// LOADING_READY or LOADING_STALLED it stays there — main.c is expected to act on that, and a
// phase that could flip back would make "press A to play anyway" mean something different
// one frame later.
LoadingPhase loadingStep(LoadingState* st, const LoadingSample* s);

// 0..100, monotone. Weighted rather than linear: the two stages cost wildly different
// amounts (a column of terrain is generated on the worker thread; a chunk mesh is built on
// this one inside a per-frame budget), so a single fraction across both would crawl through
// the first stage and then jump. Generation owns 0..80 and meshing 80..100, which is roughly
// how the measured boot actually spends its time.
int loadingPercent(const LoadingState* st);

// Short line under the heading: what is happening right now, in the player's terms.
const char* loadingStageLabel(const LoadingState* st);
