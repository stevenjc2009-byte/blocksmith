// v1.8.8. The two-lane generator's bookkeeping, with no <3ds.h> in it.
//
// ── Why this is a file and not three statics in app/worker.c ───────────────────────────
//
// app/worker.c includes <3ds.h> on its first line and carries the thread bodies, so nothing in
// it can be compiled on the host and nothing in it can be tested anywhere but on a console —
// and nothing has run on a console since v1.2.5. The parts of a second lane that can actually
// be WRONG are not the thread bodies; they are two pieces of pure bookkeeping:
//
//   * the column-claim set, which is the only thing standing between "two lanes" and "two
//     lanes generating the same column into two staging worlds"; and
//   * the ready ring, which decides which finished column the main thread installs next.
//
// So they live here, with no locking and no allocation, exactly the split world/jobq.h already
// uses ("This file is the bookkeeping only ... Locking belongs to whoever shares it across
// threads"), and app/worker.c wraps every call below in the same LightLock it already holds.
// tests/lanes_test.c links THIS file — not a retyped copy of the rule — and drives it from two
// pthreads over the real worldgenColumn.
//
// ── Why a claim set is needed at all, since the job queue already hands each job out once ──
//
// A JobQueue pop removes the job, so two lanes cannot pop the same Job. That is not the same
// as two lanes never working on the same COLUMN, because main.c can submit one column twice:
// genUnloadColumn() clears that column's `asked` mark (main.c:976) the moment the column leaves
// the generated ring, and genRequestArea() re-submits it if the player walks back before the
// lane that already has it has finished. With one lane that is harmless — the second job is
// generated after the first — and with two it is a genuine duplicate: two staging worlds
// holding the same column, two installs of it, two networldSubscribeColumn calls, and the
// budget charged twice for one square of terrain.
//
// The claim set makes it impossible instead of unlikely: a lane may only start a column no lane
// currently holds, and it holds it from the moment it pops the job until the main thread has
// installed the result and emptied that lane's staging world. A duplicate is refused at the pop
// and counted (laneClaimCollisions), not queued behind the first copy — the column is already
// being generated and will arrive, and main.c re-asks for anything that does not.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Two, and the number is a hardware fact rather than a tuning knob: the New 3DS has four ARM11
// cores, the application is given core 0 and — with cia/blocksmith.rsf's CanAccessCore2 — core
// 2. Core 1 is the system core and this game does not take it (see app/worker.h). Core 3 is not
// offered to applications at all. So the most generator lanes that can each have a core to
// themselves is two, and a third would only contend with the first two.
#define WORKER_LANES_MAX 2

// How many generator lanes this console should run. One on an Old 3DS — which has two cores,
// one of which the OS keeps — and two on a New 3DS. Pure policy, no hardware: app/worker.c
// still has to get core 2 granted before it starts the second lane, and it reports what it
// actually got through workerLanes().
int laneCountFor(bool new_3ds);

// ── The column-claim set ──────────────────────────────────────────────────────────────
//
// One entry per lane, because a lane works on one column at a time: the claim IS "this lane is
// busy, and this is what with". Held from the pop to the install, so `held[i]` also answers
// "may lane i take a new job" without a second flag that could disagree with this one.
typedef struct {
	bool    held[WORKER_LANES_MAX];
	int32_t cx[WORKER_LANES_MAX];
	int32_t cz[WORKER_LANES_MAX];
	int     collisions;    // lifetime: takes refused because another lane held that column
} LaneClaims;

void laneClaimsInit(LaneClaims* s);

// True if ANY lane currently holds this column.
bool laneClaimHeld(const LaneClaims* s, int32_t cx, int32_t cz);

// Claims (cx, cz) for `lane`. False — and nothing changed — when `lane` already holds a column
// (a caller bug: a lane must not pop twice), when `lane` is out of range, or when another lane
// already holds this exact column, which is the duplicate case and is the only one counted in
// `collisions`. The two are told apart by laneClaimBusy() below rather than by the return.
bool laneClaimTake(LaneClaims* s, int lane, int32_t cx, int32_t cz);

// Releases whatever `lane` held. Safe on a lane holding nothing.
void laneClaimDrop(LaneClaims* s, int lane);

// True when `lane` is holding a column, i.e. is generating or has a result not yet installed.
bool laneClaimBusy(const LaneClaims* s, int lane);

int  laneClaimCount(const LaneClaims* s);
int  laneClaimCollisions(const LaneClaims* s);

// The invariant this structure exists for, checkable: no two lanes hold the same column. False
// here is the defect the whole file is written to prevent.
bool laneClaimsDisjoint(const LaneClaims* s);

// ── The ready ring ────────────────────────────────────────────────────────────────────
//
// Finished columns waiting for the main thread to install them, in completion order. One slot
// per lane and no more: a lane holds its claim until its result has been installed, so it can
// never have two outstanding, and the ring therefore can never be full when a lane wants to
// push. laneReadyPush returning false is a bug in the caller, not back-pressure.
//
// Completion order rather than lane order on purpose. Slot-per-lane with a fixed scan would
// make lane 0's result always install first, so a lane 1 column finished two frames ago would
// wait behind a lane 0 column finished this frame, and lane 1 would sit idle holding its claim.
#define LANE_READY_SLOTS WORKER_LANES_MAX

typedef struct {
	int8_t  lane[LANE_READY_SLOTS];
	int32_t cx[LANE_READY_SLOTS];
	int32_t cz[LANE_READY_SLOTS];
	bool    ok[LANE_READY_SLOTS];
	int     head;      // next to pop
	int     tail;      // next free
	int     count;
	int     peak;      // high-water mark since init: 2 means both lanes really did overlap
} LaneReadyRing;

void laneReadyInit(LaneReadyRing* r);

// False when the ring is full, which the claim set makes impossible — see above.
bool laneReadyPush(LaneReadyRing* r, int lane, int32_t cx, int32_t cz, bool ok);

// False when empty; the out params are left untouched in that case.
bool laneReadyPop(LaneReadyRing* r, int* lane, int32_t* cx, int32_t* cz, bool* ok);

int  laneReadyCount(const LaneReadyRing* r);
int  laneReadyPeak(const LaneReadyRing* r);

// head, tail and count agree — the same check world/jobq.h keeps, for the same reason: the
// dirty queue's shipped bug was a count that drifted from the thing it counted.
bool laneReadyConsistent(const LaneReadyRing* r);
