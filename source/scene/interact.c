#include "scene/interact.h"

#include <math.h>

#include "app/input_map.h"
#include "net/networld.h"
#include "scene/chunk_render.h"
#include "world/block.h"
#include "world/light.h"

// Does the player's box overlap the unit cube at (bx,by,bz)? This is a plain geometric
// test rather than a call into physics.c's bodyBlocked, because the block in question has
// not been written to the world yet — and it must not be, since writing it and then
// reverting would already have queued a remesh and charged the frame 2.1 ms a chunk for
// an edit that never happened.
static bool boxOverlapsCell(const Body* b, int bx, int by, int bz)
{
	const float hw = PLAYER_WIDTH * 0.5f;

	return (b->x - hw) < (float)(bx + 1) && (b->x + hw) > (float)bx &&
	        b->y       < (float)(by + 1) && (b->y + PLAYER_HEIGHT) > (float)by &&
	       (b->z - hw) < (float)(bz + 1) && (b->z + hw) > (float)bz;
}

void interactInit(Interact* it)
{
	it->target    = (RayHit){0};
	it->holding   = BLOCK_STONE;
	it->broke     = 0;
	it->placed    = 0;
	it->refused   = 0;
	it->prev_keys = 0;
	it->broke_id  = BLOCK_AIR;
	it->placed_id = BLOCK_AIR;
}

void interactAim(Interact* it, const World* w, const Camera* cam)
{
	// The same forward vector cameraUpdate walks along, and it has to stay the same one:
	// if the ray and the movement disagree about which way "forward" is, the highlight
	// sits somewhere off to the side of the crosshair and no amount of staring at
	// raycast.c explains why. The view matrix is Rx(pitch) * Ry(yaw) * T(-pos), so
	// positive pitch looks down and -Z is forward at zero yaw.
	const float cp = cosf(cam->pitch);
	const float fx = sinf(cam->yaw) * cp;
	const float fy = -sinf(cam->pitch);
	const float fz = -cosf(cam->yaw) * cp;

	it->target = worldRaycast(w, cam->x, cam->y, cam->z, fx, fy, fz, INTERACT_REACH);
}

int interactEdit(Interact* it, World* w, const Body* body, u32 keys_down)
{
	// Internal edge detection: act only on bits newly set since the last call, no matter
	// what the caller passes. With hidKeysDown() (already a one-frame pulse) this is a
	// no-op — prev_keys cannot already hold a bit that keys_down is just now raising, so
	// fresh == keys_down on the one frame the press happens. With hidKeysHeld()
	// (level-triggered, high for the whole hold) this is what makes it safe to pass:
	// the first frame of a hold still has the bit clear in prev_keys and fires, but every
	// later frame of the same hold has it already set, so the bit drops out of fresh and
	// the edit now happens exactly once per press instead of once per frame held.
	const u32 fresh = keys_down & ~it->prev_keys;
	it->prev_keys = keys_down;

	// Step 8.2. Cleared every call, not accumulated: these two report what THIS call did, so
	// the caller can act on it once. A cumulative counter would leave the caller diffing two
	// integers to work out whether anything happened, and a stale id left over from an
	// earlier frame would put a second copy of the same block into the inventory every frame
	// until the next edit.
	it->broke_id  = BLOCK_AIR;
	it->placed_id = BLOCK_AIR;

	// Step 8.4. The two verbs come from the player's bindings rather than the INTERACT_KEY_*
	// constants directly. app/input_map.c answers with exactly those constants' values until
	// an options.ini says otherwise.
	const u32 key_break = inputKey(ACTION_BREAK);
	const u32 key_place = inputKey(ACTION_PLACE);

	if (!it->target.hit) {
		// A press with nothing to aim at is still a refusal, not a no-op: `refused` is
		// documented (interact.h) as counting edits the world, the body, or a miss
		// rejected, and a press that found no target was rejected same as one the world
		// vetoed. Leaving this uncounted made the overlay's `r` figure understate how
		// many presses actually did nothing.
		if (fresh & (key_break | key_place))
			it->refused++;
		return 0;
	}

	const RayHit* t = &it->target;
	int queued = 0;

	if (fresh & key_break) {
		// Read before the write, obviously, but worth saying why it is read at all: step
		// 8.2's inventory is filled from what the player actually broke, so the id has to be
		// captured here — after worldSet the cell is air and the information is gone.
		const BlockId broken = worldGet(w, t->x, t->y, t->z);

		// worldSet refuses y outside the world, which is exactly what should happen when
		// the ray hit the solid floor below y == 0 — that is not a block anyone owns.
		if (worldSet(w, t->x, t->y, t->z, BLOCK_AIR)) {
			// Step 8.1. Only edits mark a column for saving — see the note on Column.dirty.
			worldMarkDirty(w, t->x, t->z);
			// Adaptive lighting: recompute the edited column's channels before the remesh
			// is queued, so the drained mesh bakes the new light in the same frame. The
			// sweep engine is queue-free and column-local, so this is exactly a full
			// reflood of that one column — no neighbour can go stale behind it.
			if (lightEnabled())
				lightRelightColumn(w, t->x >> 4, t->z >> 4);
			queued += chunkRenderTouch(w, t->x, t->y, t->z);
			// Fire-and-forget to the server: the local write above is already done and is
			// authoritative for this client's own view, so nothing here waits on it.
			networldSendBlockEdit(t->x, t->y, t->z, BLOCK_AIR);
			it->broke++;
			it->broke_id = broken;
		} else {
			it->refused++;
		}
	}

	if (fresh & key_place) {
		// Nothing in hand is a refusal, not a crash and not a silent no-op. Before step 8.2
		// `holding` was a constant BLOCK_STONE and this could not happen; now it comes from
		// the inventory's selected hotbar slot, and an empty slot is the ordinary case.
		// Placing BLOCK_AIR would otherwise read as a second, longer-ranged break.
		if (it->holding == BLOCK_AIR) {
			it->refused++;
			return queued;
		}

		// No entry face means the camera is inside a solid block, so there is no sensible
		// cell to place into — the ray never crossed a boundary to name one.
		if (t->face == RAY_FACE_NONE) {
			it->refused++;
			return queued;
		}

		// Do not overwrite something solid. The place cell is the cell the ray was in
		// before the hit, so it is air in the ordinary case, but a caller could aim at a
		// block through a gap narrower than a block and land here.
		if (blockIsSolid(worldGet(w, t->px, t->py, t->pz))) {
			it->refused++;
			return queued;
		}

		// Refuse to entomb the player. `body` is required (see interact.h) so this check
		// can no longer be silently skipped by a caller passing NULL.
		if (boxOverlapsCell(body, t->px, t->py, t->pz)) {
			it->refused++;
			return queued;
		}

		if (worldSet(w, t->px, t->py, t->pz, it->holding)) {
			worldMarkDirty(w, t->px, t->pz);
			// Same relight-before-remesh ordering as the break path above.
			if (lightEnabled())
				lightRelightColumn(w, t->px >> 4, t->pz >> 4);
			queued += chunkRenderTouch(w, t->px, t->py, t->pz);
			// Fire-and-forget to the server, same as the break path above.
			networldSendBlockEdit(t->px, t->py, t->pz, it->holding);
			it->placed++;
			it->placed_id = it->holding;
		} else {
			it->refused++;
		}
	}

	return queued;
}
