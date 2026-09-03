#pragma once

// FRZ-PROBE emergency diagnostic. v1.8.16 freezes the console instantly, and 100%
// reproducibly, on "create new world" on real hardware. This is the complementary
// instrument to source code review: it does not try to explain the freeze, it just proves
// WHERE it is by writing one flushed-and-closed line to sdmc:/blocksmith/stage.txt at every
// named point below, so that if the console freezes, the LAST line in that file is the last
// stage the main thread (or a worker lane) reached before it stopped.
//
// Designed off by default, enabled only with -DBS_STAGE_PROBE=1 — see the EXTRA_CFLAGS
// convention BS_DRAW_PROBE and BS_GPU_TESTS already use in app/watchdog.h and app/gputest.h.
// With the flag off every macro below compiles to literally nothing: no symbol, no call, no
// .bss. That was verified rather than assumed — `nm` and a string grep over a flag-off ELF
// return zero hits for all three functions and all thirteen marker labels.
//
// ---- 2026-09-03: DEFAULT DELIBERATELY FLIPPED TO 1 FOR v1.8.17 ONLY ----
// v1.8.17 ships four unproven fixes for the instant new-world freeze, and not one of them can
// be confirmed anywhere but on steve's own console. If it freezes again with the probe off,
// the only thing that comes back is another hang.txt saying "phase MESH" — which is what
// v1.8.16 already said, and it was not enough to locate anything. With the probe on, the
// freeze itself names the stage, and one install answers the question instead of costing
// another round trip through him.
//
// The price is thirteen one-shot file appends, all of them during boot and first-world
// creation, none in the steady-state frame loop. There is no per-frame cost and no cost at
// all once the last marker has fired.
//
// This is a diagnostic default, not a permanent one. Put it back to 0 the moment the freeze
// is confirmed cured on hardware — a diagnostic left on forever stops being read.
//
// Every write goes through the SAME raw-FS write/flush/close path app/watchdog.c's fileWrite()
// uses for hang.txt, and for the identical reason: this can run from code that is about to
// wedge the console, so it must not go through stdio/newlib, which could already be sitting
// behind a lock some other stuck call holds. See app/watchdog.c's comment on fileWrite for the
// full argument; stage_probe.c's spAppend is a second, independent copy of that pattern rather
// than a shared helper, because this one must APPEND (never truncate) and watchdog's must
// always truncate to exactly one report — different contracts, kept as different functions on
// purpose so neither has to carry a flag for the other's case.

#ifndef BS_STAGE_PROBE
#define BS_STAGE_PROBE 1
#endif

#if BS_STAGE_PROBE

// Ids for the ONE-SHOT markers: events that either recur many times in ordinary play (a column
// is worldgen'd/lit/meshed repeatedly; C3D_FrameBegin/End and the input poll run every frame)
// or that this tree cannot reach from a single call site (the title screen and the
// world-loading screen each run their own frame loop, so "first C3D_FrameBegin" has two
// possible call sites, not one). bsStageOnce lets every candidate call site try, and only the
// one that actually runs first writes anything, giving a true "first time this happened
// anywhere in the process" marker instead of one that depends on which call site the caller
// picked. Add new ids ABOVE BS_STAGE_ID_COUNT.
typedef enum {
	BS_STAGE_ID_INPUT_POLL = 0,
	BS_STAGE_ID_FRAME_BEGIN,
	BS_STAGE_ID_FRAME_END,
	BS_STAGE_ID_WORLDGEN_COL,
	BS_STAGE_ID_LIGHT_PASS,
	BS_STAGE_ID_MESH_BUILD,
	BS_STAGE_ID_COUNT,
} BsStageId;

// Resets sdmc:/blocksmith/stage.txt to empty (creating sdmc:/blocksmith/ if needed) and clears
// the one-shot latches. Call exactly once, as the very first thing in main() — before ANY
// other marker — so a stage.txt found on the card after a freeze is always THIS boot's
// history, never a previous run's left dangling and misread as "the last thing that happened".
void bsStageInit(void);

// Appends one line — a sequence number, a raw svcGetSystemTick() timestamp, and `label` — and
// returns only once that line has been written, flushed and the file handle closed. See
// stage_probe.c's spAppend for where the flush/close actually happens and why that is the
// entire point of this file. Use this for anything that happens exactly once in the ordinary
// control flow: a service init, GPU/citro3d init, atlas/shader load, save-system init,
// new-world allocation, one call per worker lane at thread start.
void bsStageMark(const char* label);

// Same write, but only the FIRST call for a given `id` — across every call site and every
// thread — actually reaches the card; every later call is a no-op. The de-dup is a plain
// bool check, not a lock: this file's whole reason to exist is writing a marker from code that
// might be seconds from wedging the console, so it must never be able to block waiting for
// another thread. A genuine race between two callers can double-write the same marker once;
// it can never lose one, which is the only property that matters here.
void bsStageOnce(BsStageId id, const char* label);

#define BS_STAGE_INIT()        bsStageInit()
#define BS_STAGE(label)        bsStageMark(label)
#define BS_STAGE_ONCE(id, lbl) bsStageOnce((id), (lbl))

#else

#define BS_STAGE_INIT()        ((void)0)
#define BS_STAGE(label)        ((void)0)
#define BS_STAGE_ONCE(id, lbl) ((void)0)

#endif // BS_STAGE_PROBE
