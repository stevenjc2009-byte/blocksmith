// See physics.h for the three load-bearing ideas. This file is the arithmetic
// behind them.
//
// No <math.h>: source/world is kept free of it so the host test suite can link
// against plain gcc without pulling in libm behaviour that might differ from the
// ARM11's. floorf/fabsf are one-liners here instead.
#include "world/physics.h"

#include "world/block.h"

// A move is cut into substeps no bigger than this, in blocks, before it is
// resolved. It has to stay under 1.0 (a whole block) for the collision snap
// below to hold its "only one new layer of cells" invariant, and it has to stay
// well under that so a fast body still gets several tests against a thin wall
// rather than one lucky-or-unlucky one. 0.4 gives at least two tests per block
// crossed in the worst case.
#define MAX_SUBSTEP  0.4f

// Backoff used when a box edge sits exactly on a grid line. bodyBlocked treats a
// box as the half-open interval [min, max): the max edge must read as belonging
// to the cell *below* it, which floorToInt(max) alone would get wrong whenever
// max lands exactly on an integer (it would round into the next cell up). 1e-4
// is far bigger than float32 rounding error at the coordinate ranges a chunked
// world uses, and far smaller than anything a player could perceive or a wall
// could hide behind.
#define BOX_EPS  1e-4f

// floor(), without pulling in <math.h> for one instruction's worth of function.
// (int) truncates towards zero, which is wrong for negative non-integers; the
// correction below is the whole difference between this and plain truncation,
// and it is exactly the bug the world's own chunk-coordinate shift avoids for
// the same reason (see world.h).
static inline int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

static inline float absf(float v)
{
	return v < 0.0f ? -v : v;
}

void bodyInit(Body* b, float x, float y, float z)
{
	b->x = x;
	b->y = y;
	b->z = z;
	b->vx = 0.0f;
	b->vy = 0.0f;
	b->vz = 0.0f;
	b->on_ground = false;
}

// blockIsSolid, and that is the whole of v1.6.0 task 13's collision story: nothing here
// changed and nothing here should. The task split "what shape is drawn" away from "what
// fills a cell" precisely so that collision could stay a question about `solid` alone —
// a cross-shaped plant is registered non-solid, so the box walks through it here for the
// same reason it walks through air, with no shape test in the hot triple loop below.
//
// Every solid block is still a full 1.0 cube (see tryStepUp), so the AABB test remains
// exact. A shape that is solid AND smaller than its cell — a slab, a stair — is the case
// that would need a real per-shape box here, and it does not exist yet.
bool bodyBlocked(const World* w, float x, float y, float z)
{
	const float half = PLAYER_WIDTH * 0.5f;

	// Min edges are inclusive (the box genuinely starts there), so a plain floor
	// is correct even when the edge sits exactly on a grid line. Max edges are
	// exclusive, which is what the -BOX_EPS is for.
	const int x0 = floorToInt(x - half);
	const int x1 = floorToInt(x + half - BOX_EPS);
	const int y0 = floorToInt(y);
	const int y1 = floorToInt(y + PLAYER_HEIGHT - BOX_EPS);
	const int z0 = floorToInt(z - half);
	const int z1 = floorToInt(z + half - BOX_EPS);

	for (int by = y0; by <= y1; by++)
		for (int bz = z0; bz <= z1; bz++)
			for (int bx = x0; bx <= x1; bx++)
				if (blockIsSolid(worldGet(w, bx, by, bz)))
					return true;

	return false;
}

// Resolves vertical motion in isolation. This is where the "only one new layer"
// invariant matters most, because it is what turns "landed on a floor" into an
// exact answer instead of an approximate one: bodyMove never hands this a delta
// bigger than MAX_SUBSTEP, and the body is never blocked before the call (that is
// the invariant the whole module maintains), so if the moved box is blocked, the
// *only* cell that can have newly become solid is the single layer the leading
// face just entered -- every other cell in the box was already part of the box
// before the move and was already known clear. That means the offending cell's
// index can be read straight off the target position with floorToInt(), and the
// stopping point is exactly that cell's near face: no search, no residual gap,
// no risk of resting a hair's width above or below a surface.
static bool resolveY(Body* b, const World* w, float dy)
{
	if (dy == 0.0f) return false;

	if (dy > 0.0f) b->on_ground = false;   // moving up always leaves the ground

	b->y += dy;

	if (!bodyBlocked(w, b->x, b->y, b->z)) {
		if (dy < 0.0f) b->on_ground = false;   // falling clear, not resting on anything
		return false;
	}

	if (dy > 0.0f) {
		// Leading face is the head, which is bodyBlocked's "max" bound.
		const int cell = floorToInt(b->y + PLAYER_HEIGHT - BOX_EPS);
		b->y = (float)cell - PLAYER_HEIGHT;
	} else {
		// Leading face is the feet, which is bodyBlocked's "min" bound.
		const int cell = floorToInt(b->y);
		b->y = (float)(cell + 1);
		b->on_ground = true;
	}
	b->vy = 0.0f;
	return true;
}

static bool resolveX(Body* b, const World* w, float dx)
{
	if (dx == 0.0f) return false;

	const float half = PLAYER_WIDTH * 0.5f;
	b->x += dx;

	if (!bodyBlocked(w, b->x, b->y, b->z)) return false;

	if (dx > 0.0f) {
		const int cell = floorToInt(b->x + half - BOX_EPS);
		b->x = (float)cell - half;
	} else {
		const int cell = floorToInt(b->x - half);
		b->x = (float)(cell + 1) + half;
	}
	return true;
}

static bool resolveZ(Body* b, const World* w, float dz)
{
	if (dz == 0.0f) return false;

	const float half = PLAYER_WIDTH * 0.5f;
	b->z += dz;

	if (!bodyBlocked(w, b->x, b->y, b->z)) return false;

	if (dz > 0.0f) {
		const int cell = floorToInt(b->z + half - BOX_EPS);
		b->z = (float)cell - half;
	} else {
		const int cell = floorToInt(b->z - half);
		b->z = (float)(cell + 1) + half;
	}
	return true;
}

// Auto-step: when a horizontal move is blocked while grounded, tries planting the
// body exactly one block higher and re-testing the *whole* horizontal target from
// there, rather than the position resolveX/resolveZ already snapped to the wall.
//
// There is deliberately no step-height threshold to compare an obstruction against.
// Every block in this game is a full 1.0 tall cube, so there is nothing shorter than a
// whole block to measure, and lifting by any fraction of one would leave the box's feet
// embedded in the block it is trying to climb. The rise is therefore a flat 1.0.
//
// A two-block wall is refused structurally rather than numerically: the headroom probe
// below runs at y+1, a second stacked block still occupies that headroom, so
// bodyBlocked() at the stepped position comes back true and the step is refused. The
// caller enforces the other half of the rule -- at most one of these may succeed per
// substep -- because otherwise an asymmetric corner rises twice. See bodyMove.
//
// If the registry ever grows a sub-block shape (a slab or a stair) genuinely shorter
// than one block, this is where a real height comparison belongs: the rise would become
// "however tall the obstruction actually is, up to some threshold" instead of a flat 1.0,
// and that threshold is the constant to introduce at that point -- not before.
// There is no swim or sneak state in the game yet. When they arrive, the line below is
// the one place they belong: sneaking suppresses the step entirely (the player is
// deliberately moving carefully, and a sneaking player who auto-climbed would be
// astonished), and swimming replaces it rather than gating it, because a swimmer rises by
// buoyancy through water that is not solid here at all and so never reaches this code.
// Both are one more term on this same condition -- deliberately NOT built ahead of the
// states themselves, which would be a guess about fields that do not exist.
static bool tryStepUp(Body* b, const World* w, float target_x, float target_z)
{
	if (!b->on_ground) return false;   // must never fire mid-air

	const float step_y = b->y + 1.0f;
	if (bodyBlocked(w, target_x, step_y, target_z)) return false;

	b->x = target_x;
	b->y = step_y;
	b->z = target_z;
	return true;
}

int bodyMove(Body* b, const World* w, float dx, float dy, float dz)
{
	// Subdivide so no substep can cross more than one block boundary on any
	// axis -- see MAX_SUBSTEP above for why that bound has to hold, both for
	// tunnelling and for the exact-snap invariant in resolveY/X/Z.
	float amax = absf(dx);
	if (absf(dy) > amax) amax = absf(dy);
	if (absf(dz) > amax) amax = absf(dz);

	int steps = 1;
	if (amax > MAX_SUBSTEP)
		steps = (int)(amax / MAX_SUBSTEP) + 1;

	const float sx = dx / (float)steps;
	const float sy = dy / (float)steps;
	const float sz = dz / (float)steps;

	int blocked = 0;

	for (int i = 0; i < steps; i++) {
		if (resolveY(b, w, sy)) blocked |= BLOCKED_Y;

		// X and Z each get their own step-up attempt, using the position from
		// just before that axis's own resolve -- not the position resolveX/Z
		// already snapped to the wall -- as the intended target. Velocity is
		// only zeroed once a substep is known to be genuinely blocked, i.e.
		// after a step-up attempt has had its chance to clear it; a step that
		// succeeds is a completed move, not a stop.
		// At most one block of rise per substep, no matter how many axes are blocked.
		// Without this, an asymmetric corner -- a one-block step on X, a two-block wall
		// on Z -- rises twice: X's step-up lifts the body a block, and Z is still
		// blocked from the new height, so Z's step-up lifts it again. Measured at
		// rise=2.0, which climbs a wall the headroom probe is supposed to refuse.
		bool stepped = false;

		const float pre_x = b->x;
		bool blocked_x = resolveX(b, w, sx);
		if (blocked_x && tryStepUp(b, w, pre_x + sx, b->z)) {
			blocked_x = false;
			stepped   = true;
		}
		if (blocked_x) { b->vx = 0.0f; blocked |= BLOCKED_X; }

		const float pre_z = b->z;
		bool blocked_z = resolveZ(b, w, sz);
		if (blocked_z && !stepped && tryStepUp(b, w, b->x, pre_z + sz)) blocked_z = false;
		if (blocked_z) { b->vz = 0.0f; blocked |= BLOCKED_Z; }
	}

	return blocked;
}

int bodyStep(Body* b, const World* w, float dt_s)
{
	b->vy += PLAYER_GRAVITY * dt_s;
	if (b->vy < PLAYER_TERMINAL) b->vy = PLAYER_TERMINAL;

	return bodyMove(b, w, b->vx * dt_s, b->vy * dt_s, b->vz * dt_s);
}
