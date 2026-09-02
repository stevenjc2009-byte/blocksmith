#include "app/lanes.h"

// No locking in this file. Every function is called by app/worker.c under the one LightLock it
// already holds for the job queue — see app/lanes.h for why the split is here rather than in
// worker.c, and world/jobq.c for the same arrangement one directory over.

int laneCountFor(bool new_3ds)
{
	return new_3ds ? WORKER_LANES_MAX : 1;
}

// ── The column-claim set ──────────────────────────────────────────────────────────────

void laneClaimsInit(LaneClaims* s)
{
	for (int i = 0; i < WORKER_LANES_MAX; i++) {
		s->held[i] = false;
		s->cx[i]   = 0;
		s->cz[i]   = 0;
	}
	s->collisions = 0;
}

bool laneClaimHeld(const LaneClaims* s, int32_t cx, int32_t cz)
{
	for (int i = 0; i < WORKER_LANES_MAX; i++)
		if (s->held[i] && s->cx[i] == cx && s->cz[i] == cz) return true;
	return false;
}

bool laneClaimBusy(const LaneClaims* s, int lane)
{
	if (lane < 0 || lane >= WORKER_LANES_MAX) return false;
	return s->held[lane];
}

bool laneClaimTake(LaneClaims* s, int lane, int32_t cx, int32_t cz)
{
	if (lane < 0 || lane >= WORKER_LANES_MAX) return false;

	// A lane asking for a second column is a caller bug and not a duplicate, so it is refused
	// without touching `collisions` — that counter has one meaning and it is "two lanes wanted
	// the same square of terrain", which is the number worth reading.
	if (s->held[lane]) return false;

	if (laneClaimHeld(s, cx, cz)) { s->collisions++; return false; }

	s->held[lane] = true;
	s->cx[lane]   = cx;
	s->cz[lane]   = cz;
	return true;
}

void laneClaimDrop(LaneClaims* s, int lane)
{
	if (lane < 0 || lane >= WORKER_LANES_MAX) return;
	s->held[lane] = false;
}

int laneClaimCount(const LaneClaims* s)
{
	int n = 0;
	for (int i = 0; i < WORKER_LANES_MAX; i++) if (s->held[i]) n++;
	return n;
}

int laneClaimCollisions(const LaneClaims* s) { return s->collisions; }

bool laneClaimsDisjoint(const LaneClaims* s)
{
	for (int i = 0; i < WORKER_LANES_MAX; i++) {
		if (!s->held[i]) continue;
		for (int j = i + 1; j < WORKER_LANES_MAX; j++) {
			if (!s->held[j]) continue;
			if (s->cx[i] == s->cx[j] && s->cz[i] == s->cz[j]) return false;
		}
	}
	return true;
}

// ── The ready ring ────────────────────────────────────────────────────────────────────

void laneReadyInit(LaneReadyRing* r)
{
	for (int i = 0; i < LANE_READY_SLOTS; i++) {
		r->lane[i] = -1;
		r->cx[i]   = 0;
		r->cz[i]   = 0;
		r->ok[i]   = false;
	}
	r->head = r->tail = r->count = r->peak = 0;
}

bool laneReadyPush(LaneReadyRing* r, int lane, int32_t cx, int32_t cz, bool ok)
{
	if (r->count >= LANE_READY_SLOTS) return false;

	r->lane[r->tail] = (int8_t)lane;
	r->cx[r->tail]   = cx;
	r->cz[r->tail]   = cz;
	r->ok[r->tail]   = ok;
	r->tail = (r->tail + 1) % LANE_READY_SLOTS;
	r->count++;
	if (r->count > r->peak) r->peak = r->count;
	return true;
}

bool laneReadyPop(LaneReadyRing* r, int* lane, int32_t* cx, int32_t* cz, bool* ok)
{
	if (r->count == 0) return false;

	if (lane) *lane = r->lane[r->head];
	if (cx)   *cx   = r->cx[r->head];
	if (cz)   *cz   = r->cz[r->head];
	if (ok)   *ok   = r->ok[r->head];
	r->head = (r->head + 1) % LANE_READY_SLOTS;
	r->count--;
	return true;
}

int laneReadyCount(const LaneReadyRing* r) { return r->count; }
int laneReadyPeak(const LaneReadyRing* r)  { return r->peak; }

bool laneReadyConsistent(const LaneReadyRing* r)
{
	if (r->count < 0 || r->count > LANE_READY_SLOTS) return false;
	if (r->head < 0 || r->head >= LANE_READY_SLOTS)  return false;
	if (r->tail < 0 || r->tail >= LANE_READY_SLOTS)  return false;
	if (r->peak < r->count || r->peak > LANE_READY_SLOTS) return false;
	return ((r->head + r->count) % LANE_READY_SLOTS) == r->tail;
}
