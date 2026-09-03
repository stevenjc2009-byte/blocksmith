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
#include <stddef.h>
#include <stdint.h>

// Where the main thread is. Ordered roughly as a frame runs; the names are what end up in the
// report, so they are the vocabulary this file is actually for.
typedef enum {
	WD_PHASE_APT = 0,    // between frames, inside aptMainLoop() — see below, never reported
	WD_PHASE_INPUT,      // hidScanInput and the touch read
	WD_PHASE_LOAD_GEN,   // loading screen: installing a generated column
	WD_PHASE_LOAD_MESH,  // loading screen: draining the mesh queue
	WD_PHASE_LOAD_DRAW,  // loading screen: the frame itself

	// The three below cover the handoff: the stretch between the loading screen's last drawn
	// frame and the game loop's first one. They exist because of a second hardware report on
	// 2026-08-19 — "a hundred percent, it says ready, but it just sort of freezes ... it
	// freezes the entire console" — and because that window was, until they were added, the
	// one place a hang could not be reported at all: runLoadingScreen leaves the phase at
	// WD_PHASE_APT on its way out, and an APT stall is deliberately ignored (see below), so a
	// main thread that stopped here left no file behind and looked exactly like a console
	// sitting quietly in the HOME menu.
	WD_PHASE_HANDOFF_GPU,     // highlightInit, playerModelInit
	WD_PHASE_HANDOFF_SAVE,    // spawn height, playerInit, inventoryLoad, uiInit
	WD_PHASE_HANDOFF_REPORT,  // worldReportDraw and the queue peak reset

	WD_PHASE_NET,        // netUpdate/networldUpdate
	WD_PHASE_INSTALL,    // in-game: genInstallOne, which locks the worker
	WD_PHASE_SIM,        // player, camera, ring follow, aim, edit
	// Widened v1.8.15 to name the relight queue explicitly. main.c's relightDrain() ran under
	// WD_PHASE_SIM until then, so a stall in it was reported against "player, camera, aim,
	// edit" — the marker moved, and this comment moved with it, because a phase whose declared
	// scope does not match where the marker actually sits is how the next reader gets misled.
	WD_PHASE_MESH,       // in-game: draining the relight, dirty and stream queues
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

// ── v1.8.16 FRZ-FIX: the DEATH SPIRAL the timeout above is blind to ───────────────────────
//
// WD_TIMEOUT_MS only ever fires when the beat STOPS. Any beat at all resets the clock, so a
// game whose frames keep arriving — one every three seconds — never accumulates toward it and
// no hang.txt is ever written. That is exactly what steve reported: "the framerate was slightly
// lagging" first, then a console that would not respond, and NOTHING on the SD card afterwards.
// A permanently parked main thread would have left a file; a spiral does not.
//
// So there is a second, slower accumulator that a beat does NOT clear.
//
// A frame that takes longer than this is not a hitch. Picked above the worst legitimate
// main-thread pause this game has — a region rewrite landing behind a save — so a single slow
// frame never trips it.
#define WD_SLOW_BEAT_MS 2000u

// ...and this much of them, in an unbroken run, is a hang whatever the beat says. Same budget as
// WD_TIMEOUT_MS, so "ten seconds of not playing" means one thing. Five consecutive two-second
// frames trip it; one three-second region rewrite does not; a HOME suspend of any length does
// not, because the WD_PHASE_APT branch zeroes the clock on every poll.
#define WD_SLOW_TOTAL_MS 10000u

// Starts the monitor thread. False if the thread could not be created, in which case
// everything below is a no-op and the game runs exactly as it did before this file existed.
bool watchdogStart(void);

// Joins the monitor thread. Safe to call whether or not watchdogStart succeeded.
void watchdogStop(void);

// One store. Called around anything that could block.
void watchdogPhase(WdPhase p);

// v1.8.16 FRZ-FIX. One load, so a caller can bracket a blocking call and put the phase BACK to
// whatever it actually was rather than to whatever it usually is. main.c's genUnloadColumn needs
// this: it is called both from the in-frame ring follow (WD_PHASE_SIM) and from the startup save
// self-check (WD_PHASE_HANDOFF_SAVE), so a hardcoded restore would be right in one place and a
// lie in the other — and a phase that lies is worse than no phase, which is the whole reason
// WD_PHASE_SAVE is being set around the save at all.
WdPhase watchdogPhaseGet(void);

// Once per frame, from the frame loop. This is the signal the monitor watches: not the phase,
// which a hung frame leaves pointing at the right place, but the count, which only a frame
// that completed can move.
void watchdogBeat(void);

// The world's counters, so the report says what the world was doing as well as where the code
// was. Cheap enough to call every frame — four stores.
void watchdogCounters(int columns, int meshes, int queued, bool worker_busy);

#ifndef BS_DRAW_PROBE
#define BS_DRAW_PROBE 0
#endif

#if BS_DRAW_PROBE
// Draw-bisect build only (see main.c). The main thread samples the two heap figures once per
// frame and hands them over, because the monitor thread must never call linearSpaceFree()
// itself — see the comment on the variables in watchdog.c.
void watchdogProbeState(int arm, uint32_t linear_free, uint32_t vram_free);

// How far into scene/chunk_render.c's chunkRenderDraw the main thread got. Round 1 of the
// bisect proved the freeze is inside that call; this says whereabouts on the first boot that
// freezes, instead of costing another six-boot round to find out.
//
// The distinction the report is built around is CULL versus everything after it: cullFrame
// issues no GPU commands at all, so a freeze recorded there is the CPU spinning, and a freeze
// recorded anywhere later is the CPU waiting on a GPU that never finished.
typedef enum {
	WD_DRAW_IDLE = 0,    // not inside chunkRenderDraw
	WD_DRAW_ENTER,       // entered, before the cull
	WD_DRAW_CULL,        // inside cullFrame — CPU only
	WD_DRAW_BIND,        // pipelineBind and the projection uniform
	WD_DRAW_OPAQUE,      // the opaque pass; k and slot say which chunk
	WD_DRAW_TRANSPARENT, // the transparent pass; k and slot say which chunk
	WD_DRAW_DONE,        // returned to main.c

	// Round 2 answered itself and then stopped short. The 1.1.3 report from real hardware
	// said "finished, back in main.c", which was read at the time as "the CPU finished the
	// whole frame". It does not say that. It says chunkRenderDraw returned — and there are
	// six more GPU submissions after it inside the same WD_PHASE_DRAW window, none of which
	// had a marker, so all six looked identical in the report:
	//
	//   highlightDraw, playerModelDraw, playerModelDrawTags, the bottom screen's own clear
	//   and sprite batch, C3D_FrameEnd, and the next frame's C3D_FrameBegin.
	//
	// These close that gap. Appended rather than inserted in frame order so scene/
	// chunk_render.c's existing numbering is untouched; the names carry the order, not the
	// values.
	WD_DRAW_EYE_SETUP,   // RenderTargetClear + FrameDrawOn, before the world; k is the eye
	WD_DRAW_HIGHLIGHT,   // highlightDraw — the cage around the aimed-at block
	WD_DRAW_PLAYERS,     // playerModelDraw — other people's bodies
	WD_DRAW_TAGS,        // playerModelDrawTags — their name tags, a sprite batch
	WD_DRAW_BOTTOM,      // drawBottomUi — bottom-screen clear, then the UI sprite batch
	WD_DRAW_FRAME_END,   // inside C3D_FrameEnd: command list submitted, transfer queued
	WD_DRAW_FRAME_WAIT,  // v1.1.4's marker for the whole of C3D_FrameBegin(C3D_FRAME_SYNCDRAW)

	// Round 3 stopped one step short in its turn, and for the same reason: a name was trusted
	// instead of read. v1.1.4's hardware report landed on WD_DRAW_FRAME_WAIT and the report's
	// own prose called that "waiting on the GPU". Disassembling citro3d's C3D_FrameBegin shows
	// it is two unrelated waits back to back, and the marker sits before both:
	//
	//   1. if (flags & C3D_FRAME_SYNCDRAW), an inlined C3D_FrameSync() — a
	//      do { gspWaitForAnyEvent(); } while (frameCounter unchanged) loop. This is frame
	//      PACING. It waits for a VBlank tick and never touches the GPU's progress.
	//   2. gxCmdQueueWait(&ctx->gxQueue, -1) — the real one: the GX queue draining, i.e. the
	//      command list, the memory fills and the display transfers all completing.
	//
	// Stuck in 1 means GSP stopped delivering events, or the frame-pacing counter stopped
	// advancing, and the world draw is irrelevant. Stuck in 2 means a GPU command never
	// finished. Different bugs, different fixes, and v1.1.4 could not tell them apart.
	//
	// main.c now calls the two halves separately — C3D_FrameSync() then C3D_FrameBegin(0),
	// which is exactly what C3D_FrameBegin(C3D_FRAME_SYNCDRAW) does internally — so the
	// report names which one. Behaviour is unchanged; only the reporting is finer.
	WD_DRAW_FRAME_VSYNC, // inside C3D_FrameSync — waiting for a vblank tick (frame pacing)
	WD_DRAW_FRAME_QUEUE, // inside C3D_FrameBegin(0) — waiting for the GX queue to drain
	WD_DRAW_STAGE_COUNT
} WdDrawStage;

// k is the loop index within a pass and slot the mesh slot being drawn; pass -1 for both when
// the stage is not inside a loop. Three stores, no lock, same contract as watchdogPhase.
void watchdogDrawStage(int stage, int k, int slot);

// Writes sdmc:/blocksmith/gxprobe.txt once, using the same GX-queue reader the hang report
// uses. Call it from the main thread immediately after C3D_FrameEnd(0), which is the one
// moment the queue is guaranteed to hold commands — that is what makes it a proof that the
// reader works rather than another unfalsifiable line in a report. Every call after the first
// returns immediately. See the comment on watchdogGxSelfTest in watchdog.c.
void watchdogGxSelfTest(void);

// Copies this frame's GPU command list into a two-deep ring, so the hang report can write out
// both the list the GPU never finished (cmdhang.bin) and the one it finished immediately before
// it (cmdprev.bin). Call it from the main thread every frame, immediately after C3D_FrameEnd,
// beside watchdogGxSelfTest — the watchdog thread cannot do this itself, because by the time it
// fires the main thread is already parked inside the next C3D_FrameBegin.
void watchdogCmdCapture(void);

// The most recently captured list, or NULL if no frame has been captured yet. For
// app/gputest.c's post-mortem, which reads the stuck entry straight out of the GX queue and
// needs somewhere to fall back to when that read comes back empty — as it did on the first
// emulator run of the post-mortem, where the queue held no entries at all and steps 2 and 3
// were skipped for want of a list. A captured copy is a strictly worse source than the live
// queue entry, because it cannot show a list that was overwritten after submission, so the
// report says which of the two it used.
const uint8_t* watchdogLastCmdList(uint32_t* len, uint32_t* addr);
#endif

// Hands the report one already-formatted line about the draw guard (scene/chunk_render.c).
// The caller keeps the buffer alive for the rest of the process: this stores the pointer and
// nothing else, because the monitor thread must never allocate or take a lock. Never call it
// and the line is simply left out of the report.
void watchdogGuardLine(const char* line);

// True once a report has been written this session. Only the deliberate-hang self-check reads
// this; nothing in the game does.
bool watchdogFired(void);

// Writes one file under sdmc:/blocksmith/, creating the directory if it is not there, and sizing
// the file to exactly len so a shorter report cannot leave the tail of a longer previous one
// trailing it. Exposed for app/gputest.c, which needs the same raw-FS path the hang report uses
// and for the same reason: it can be called from a frame that is already in trouble, where
// nothing about the C library's buffering is worth relying on.
void watchdogWriteFile(const char* rel_path, const void* data, size_t len);
