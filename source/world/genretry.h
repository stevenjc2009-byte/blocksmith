// A ledger of columns that installed with a hole in them — main.c's genInstallOne() half that
// can be proven on the host.
//
// ── the bug this exists to fix ──────────────────────────────────────────────────────────
//
// main.c's genInstallOne() marks a column installed (genSlotSet(s_col_in, ...)) whether or not
// worldColumnCreate()/worldChunkCreate() (world.c) actually got every chunk in — and it has to:
// leaving a partial column unmarked would stall its four neighbours' ring-complete check
// forever (genColumnRingComplete walks the 3x3 around a candidate and needs every one of them
// "installed" before it will queue any of them for meshing). That part is correct and this
// file does not touch it.
//
// What main.c did NOT do is ever look at that column again. genRequestArea() only asks the
// worker for a column that is neither in s_col_in nor s_col_asked — and a column that failed
// is in s_col_in permanently, so it is never re-asked. The hole clears only if the player walks
// far enough that the streaming ring drops the column (main.c's genUnloadColumn, which clears
// s_col_in for anything genInArea() now says no to) and later walks back, which re-requests it
// fresh. That is "I am one chunk away and it is unloaded... only when I get close does it load
// in" — the streaming ring is wide enough and nearest-first ordering is already in place
// (main.c:552-556, scene/ringorder.c), so neither of those explains it; a silently permanent
// hole from a budget refusal does.
//
// ── why this is a separate file ─────────────────────────────────────────────────────────
//
// Same reason world/meshq.h gives for existing at all: the bug is in main.c, main.c includes
// <3ds.h> and carries main(), so nothing in it links into the host suite, and this project's
// rule is that a test links the real module rather than a hand-copied duplicate. So the ledger
// — the part that can be exercised without a citro3d context or a running worker thread — gets
// its own file, and main.c is left calling into it instead of keeping its own array.
//
// No <3ds.h>, no floats, no allocation — same rules as the rest of source/world.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Frames before a failed column's first retry, and the ceiling the backoff saturates at.
// Reasoned rather than measured: there is no frame clock on the host to measure against, and
// on the console a frame is main.c's own 16.71 ms, so 30 is roughly half a second and 300 is
// roughly five. Doubling per repeat failure (genRetryMark) is what stops a column that keeps
// failing from being resubmitted every frame — see genRetryTick's own comment for the one
// frame-by-frame exception (a job-ring refusal, not a generation refusal).
#define GEN_RETRY_BASE_FRAMES   30
#define GEN_RETRY_MAX_FRAMES   300

// The backoff shift saturates here: GEN_RETRY_BASE_FRAMES << GEN_RETRY_MAX_ATTEMPTS is already
// past GEN_RETRY_MAX_FRAMES (30 << 4 == 480), so attempts never needs to count past this, and
// capping it explicitly keeps the shift itself from ever running far enough to matter.
#define GEN_RETRY_MAX_ATTEMPTS   4

// Upper bound on how many columns can be tracked at once. Must be >= the caller's area span
// squared — main.c's GEN_AREA_SPAN * GEN_AREA_SPAN — which it checks with a _Static_assert next
// to the ones it already carries for JOBQ_CAP and RING_ORDER_MAX_RADIUS, for the same reason:
// a bound that goes stale under a render-distance change should fail the build, not fail as a
// hole nobody notices. 19x19 covers every area span up to RENDER_DIST_MAX 8 (today's ceiling is
// 5); raise it there if that ceiling ever moves past that.
#define GEN_RETRY_SLOTS   (19 * 19)

typedef struct {
	int32_t cx, cz;
	bool    set;       // an outstanding hole is tracked in this slot
	bool    inflight;  // already resubmitted; waiting on genRetryMark/genRetryClear to hear back
	int16_t cooldown;  // frames left before resubmission — meaningful only if set && !inflight
	uint8_t attempts;  // backoff stage so far, saturates at GEN_RETRY_MAX_ATTEMPTS
} GenRetrySlot;

typedef struct {
	GenRetrySlot slots[GEN_RETRY_SLOTS];
	int span;          // the caller's actual area span, for the coordinate-to-slot wrap
	int outstanding;   // holes currently outstanding — what the debug overlay should show
} GenRetryLedger;

// `span` must match the caller's GEN_AREA_SPAN and be small enough that span*span <=
// GEN_RETRY_SLOTS (checked by genRetryInit itself, which returns false rather than leave the
// ledger half set up if it is not — the caller is expected to treat that as a build-time
// mistake, not a runtime condition to recover from).
bool genRetryInit(GenRetryLedger* h, int span);

// A column installed with ok == false (world.c's worldColumnCreate/worldChunkCreate refused
// part of it). Schedules another attempt with the backoff appropriate to how many times this
// exact column has already failed — a fresh column starts its ledger entry at attempts 0 and
// `outstanding` goes up by one; a column that was already pending just has its backoff pushed
// further out and `outstanding` is unchanged, because it is still the same hole.
void genRetryMark(GenRetryLedger* h, int32_t cx, int32_t cz);

// A column resolved: it installed clean, or the caller no longer wants it (the ring moved
// past it). No-op if nothing was pending for it. Returns true iff it actually cleared a
// pending entry, in which case `outstanding` drops by one.
bool genRetryClear(GenRetryLedger* h, int32_t cx, int32_t cz);

// Ages every pending, non-inflight entry by one frame. An entry further than `radius` (in
// either axis) from (center_cx, center_cz) is dropped outright — the ring moved past it, see
// genRetryClear's comment — rather than counted down. An entry whose cooldown has reached zero
// is handed to `submit(ud, cx, cz)`:
//
//   * `submit` returns true  -> the entry is marked inflight. It stays counted in
//     `outstanding` (the hole is not fixed yet) but is not touched again by this function
//     until genRetryMark or genRetryClear hears back from genInstallOne.
//   * `submit` returns false -> the job ring was full this one frame, which is a one-frame
//     condition and not the kind of failure the backoff exists for, so the entry is left
//     pending with cooldown 0 and tried again next call rather than waiting out a whole
//     backoff period for it.
//
// `submit` is how this file stays ignorant of workerSubmitColumn and s_col_asked, which are
// main.c's — the same separation world/meshq.h keeps from JobQueue's own caller.
void genRetryTick(GenRetryLedger* h, int32_t center_cx, int32_t center_cz, int radius,
                   bool (*submit)(void* ud, int32_t cx, int32_t cz), void* ud);

int genRetryOutstanding(const GenRetryLedger* h);
