#pragma once

// Writes down where the main thread was when it stopped, because a stopped main thread cannot
// write that down itself.
//
// This exists for one specific report, from real hardware on 2026-08-19: a world created, the
// HUD still on the bottom screen with its last numbers, no terrain on the top screen, and
// "I can't tap anything, move or use a D pad. Nothing functions. It is frozen ... it's frozen
// my three d s." That is not a crash — app/crash.c would have caught an exception, written
// sdmc:/blocksmith/crash.txt and let ERRF raise the console's own crash screen. It is the other
// failure: the main thread stopped returning to aptMainLoop(), so APT went unserviced and the
// HOME button went with it.
//
// The build that did that cannot be debugged from here. It does not reproduce in Azahar — the
// same .3dsx was driven through world creation on both console profiles and rendered terrain,
// culled partially, and answered input both times — and a shipped build has no console to
// printf to (BS_BOTTOM_UI owns the bottom screen, so consoleInit is not even called). So the
// only way to learn anything from the next occurrence is to have a second thread notice that
// the first one has stopped, and leave a file on the card saying what it was doing.
//
// Cost when nothing is wrong: one thread asleep 250 ms at a time, and one 32-bit store per
// phase marker. Nothing is read back at runtime and no lock is taken, on purpose — a
// diagnostic that can itself block the main thread would be worse than no diagnostic.

#include <stdbool.h>

// Where the main thread is. Ordered roughly as a frame runs; the names are what end up in the
// report, so they are the vocabulary this file is actually for.
typedef enum {
	WD_PHASE_APT = 0,    // between frames, inside aptMainLoop() — see below, never reported
	WD_PHASE_INPUT,      // hidScanInput and the touch read
	WD_PHASE_LOAD_GEN,   // loading screen: installing a generated column
	WD_PHASE_LOAD_MESH,  // loading screen: draining the mesh queue
	WD_PHASE_LOAD_DRAW,  // loading screen: the frame itself
	WD_PHASE_NET,        // netUpdate/networldUpdate
	WD_PHASE_INSTALL,    // in-game: genInstallOne, which locks the worker
	WD_PHASE_SIM,        // player, camera, ring follow, aim, edit
	WD_PHASE_MESH,       // in-game: draining the dirty and stream queues
	WD_PHASE_DRAW,       // C3D_FrameBegin .. C3D_FrameEnd
	WD_PHASE_SAVE,       // the way out: dirty columns, inventory, worker stop
	WD_PHASE_COUNT,
} WdPhase;

// A stall this long with the phase set to anything but WD_PHASE_APT is reported. Ten seconds
// is not a guess at a threshold, it is well past every legitimate pause the main thread has:
// the frame budget is 16.71 ms, the worst measured frame in a profile run was 25 ms, and the
// longest single main-thread operation in the game — flushing dirty columns to the card on the
// way out — is a handful of region writes.
//
// WD_PHASE_APT is excluded because a stall there is normal and expected: aptMainLoop() is where
// the app sits while the HOME menu has it suspended, which can legitimately last hours, and a
// player coming back from lunch must not find a hang report on their card.
#define WD_TIMEOUT_MS 10000

// Starts the monitor thread. False if the thread could not be created, in which case
// everything below is a no-op and the game runs exactly as it did before this file existed.
bool watchdogStart(void);

// Joins the monitor thread. Safe to call whether or not watchdogStart succeeded.
void watchdogStop(void);

// One store. Called around anything that could block.
void watchdogPhase(WdPhase p);

// Once per frame, from the frame loop. This is the signal the monitor watches: not the phase,
// which a hung frame leaves pointing at the right place, but the count, which only a frame
// that completed can move.
void watchdogBeat(void);

// The world's counters, so the report says what the world was doing as well as where the code
// was. Cheap enough to call every frame — four stores.
void watchdogCounters(int columns, int meshes, int queued, bool worker_busy);

// True once a report has been written this session. Only the deliberate-hang self-check reads
// this; nothing in the game does.
bool watchdogFired(void);
