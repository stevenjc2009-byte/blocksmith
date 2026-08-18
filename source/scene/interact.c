#include "scene/interact.h"

#include <math.h>

#include "scene/chunk_render.h"
#include "world/block.h"

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

	if (!it->target.hit) {
		// A press with nothing to aim at is still a refusal, not a no-op: `refused` is
		// documented (interact.h) as counting edits the world, the body, or a miss
		// rejected, and a press that found no target was rejected same as one the world
		// vetoed. Leaving this uncounted made the overlay's `r` figure understate how
		// many presses actually did nothing.
		if (fresh & (INTERACT_KEY_BREAK | INTERACT_KEY_PLACE))
			it->refused++;
		return 0;
	}

	const RayHit* t = &it->target;
	int queued = 0;

	if (fresh & INTERACT_KEY_BREAK) {
		// worldSet refuses y outside the world, which is exactly what should happen when
		// the ray hit the solid floor below y == 0 — that is not a block anyone owns.
		if (worldSet(w, t->x, t->y, t->z, BLOCK_AIR)) {
			queued += chunkRenderTouch(w, t->x, t->y, t->z);
			it->broke++;
		} else {
			it->refused++;
		}
	}

	if (fresh & INTERACT_KEY_PLACE) {
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
			queued += chunkRenderTouch(w, t->px, t->py, t->pz);
			it->placed++;
		} else {
			it->refused++;
		}
	}

	return queued;
}
