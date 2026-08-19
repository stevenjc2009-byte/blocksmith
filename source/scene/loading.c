#include "scene/loading.h"

#include <stdio.h>
#include <string.h>

// The boot is allowed this long before the two "nothing is coming" tests below are believed.
// They read `worker_busy`, and there is a real window at frame one where the generator thread
// has been handed the whole ring but has not yet picked any of it up — workerBusy() is
// s_ready || s_in_flight || queue > 0, all three of which are false for the instant between
// workerStart returning and the first job being taken. One second of grace costs a stalled
// boot one second of extra waiting; not having it would report a healthy boot as broken.
#define LOADING_GRACE_FRAMES 60

// The scalar the stall test watches. Both terms only ever grow during a boot — a column is
// installed once and meshes are built, never unbuilt — so this is monotone, which is what
// makes "unchanged for N frames" mean "stopped" rather than "went backwards". The 4096
// multiplier is just wide enough that no plausible mesh count can carry into the column
// term: the pool holds 150 slots at RENDER_DIST_MAX.
static long progressOf(const LoadingSample* s)
{
	return (long)s->columns_in * 4096L + (long)s->meshes;
}

// Generation owns 0..80 and meshing 80..100 — see loading.h. Held to a high-water mark
// because the meshing fraction is *not* monotone on its own: the queue grows every time
// another column's 3x3 ring completes, so meshes/(meshes+queued) genuinely dips when work
// arrives faster than it is drained. A bar that slides backwards reads as a fault.
static void percentUpdate(LoadingState* st, const LoadingSample* s)
{
	int pct = 0;

	if (s->columns_total > 0) {
		const int in = s->columns_in < s->columns_total ? s->columns_in : s->columns_total;
		pct = in * 80 / s->columns_total;
	}

	const int mesh_total = s->meshes + s->mesh_queued;
	if (mesh_total > 0) pct += s->meshes * 20 / mesh_total;

	if (st->phase == LOADING_READY) pct = 100;
	if (pct < 0)   pct = 0;
	if (pct > 100) pct = 100;
	if (pct > st->percent) st->percent = pct;
}

void loadingInit(LoadingState* st)
{
	memset(st, 0, sizeof(*st));
	st->phase = LOADING_GENERATING;
	// Not 0: progressOf() returns 0 for a boot that has produced nothing yet, and starting
	// equal to it would count the first frame as "no progress since last time" when there has
	// not been a last time. -1 cannot collide with any real sample.
	st->progress = -1;
}

// Both stall reasons carry the counters that produced them, because this string is the only
// thing that leaves the console when a boot goes wrong on hardware nobody here has: a photo
// of the bottom screen has to be enough to tell "the card never answered" from "the mesher
// never drained".
static void stall(LoadingState* st, const LoadingSample* s, const char* what)
{
	st->phase = LOADING_STALLED;
	snprintf(st->reason, sizeof(st->reason), "%s (col %d/%d msh %d q%d f%d s%d)",
	         what, s->columns_in, s->columns_total, s->meshes, s->mesh_queued,
	         s->columns_failed, s->submit_failed);
}

LoadingPhase loadingStep(LoadingState* st, const LoadingSample* s)
{
	st->frames++;
	st->last = *s;

	// Terminal, on purpose — see loading.h. Once the screen has offered "play anyway" it must
	// not withdraw the offer a frame later.
	if (st->phase == LOADING_READY || st->phase == LOADING_STALLED) return st->phase;

	const long p = progressOf(s);
	if (p != st->progress) {
		st->progress = p;
		st->frames_since_progress = 0;
	} else {
		st->frames_since_progress++;
	}

	// The handover test. `mesh_queued == 0` is safe to read as "the geometry is built" only
	// because the queue is filled synchronously: genQueueReadyColumns() runs inside
	// genInstallOne(), so by the time the ring is complete every chunk it wants meshed is
	// already in the queue. An empty queue therefore means drained, never "not asked for yet".
	if (s->spawn_ready && s->ring_ready && s->mesh_queued == 0) {
		st->phase = LOADING_READY;
		percentUpdate(st, s);
		return st->phase;
	}

	if (st->frames > LOADING_GRACE_FRAMES) {
		// Nothing outstanding on the generator and the column the player has to stand in has
		// not arrived. This is the case the old blocking wait handled by simply giving up and
		// dropping the player into a world with no ground under them.
		if (!s->worker_busy && !s->spawn_ready) {
			stall(st, s, "generator stopped");
			return st->phase;
		}

		// Nothing outstanding anywhere, yet the ring is still incomplete: a column was refused
		// by the worker's queue (submit_failed) or came back as a hole, so no further frame can
		// improve on this.
		if (!s->worker_busy && s->mesh_queued == 0) {
			stall(st, s, "ring incomplete");
			return st->phase;
		}
	}

	if (st->frames_since_progress >= LOADING_STALL_FRAMES) {
		stall(st, s, "no progress");
		return st->phase;
	}

	st->phase = s->spawn_ready ? LOADING_BUILDING : LOADING_GENERATING;
	percentUpdate(st, s);
	return st->phase;
}

int loadingPercent(const LoadingState* st)
{
	return st->percent;
}

const char* loadingStageLabel(const LoadingState* st)
{
	switch (st->phase) {
	case LOADING_GENERATING: return "Generating terrain";
	case LOADING_BUILDING:   return "Building geometry";
	case LOADING_READY:      return "Ready";
	case LOADING_STALLED:    return "Stopped";
	default:                 return "";
	}
}
