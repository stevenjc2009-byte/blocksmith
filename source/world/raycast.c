#include "world/raycast.h"

#include "world/block.h"

// Newton-Raphson square root, because nothing under source/world links <math.h> —
// see block.h's header comment for why: this file has to build and run under the
// host gcc in tests/host_test.c, not just under devkitARM. The only sqrt this file
// needs is for normalising the ray direction, and a handful of iterations converges
// to float precision for any input this ever sees (a squared direction length, so
// always a small positive number). A fixed iteration count rather than a
// converge-and-stop loop is deliberate: it costs a few wasted multiplies once it has
// converged, in exchange for never being able to spin.
static float approxSqrt(float v)
{
	float x = v;
	for (int i = 0; i < 12; i++)
		x = 0.5f * (x + v / x);
	return x;
}

// (int) truncates towards zero, so (int)(-0.5f) is 0 — one cell to the right of the
// cell that actually contains -0.5. World coordinates are floored elsewhere with an
// arithmetic shift (see world.h), and the ray has to agree with it: get this wrong
// and every ray on the negative side of an axis starts, and then walks, in the wrong
// cell.
static int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v)
		i--;
	return i;
}

// Amanatides & Woo grid traversal: instead of sampling points along the ray at some
// fixed spacing, step exactly one cell at a time, always onto whichever axis's cell
// boundary is nearest. Fixed-step sampling can jump clean over the shared edge
// between two blocks that only touch diagonally and land in the gap, hitting
// neither — the DDA walk never skips a cell because it only ever crosses one
// boundary at a time.
RayHit worldRaycast(const World* w, float ox, float oy, float oz,
                    float dx, float dy, float dz, float max_distance)
{
	const RayHit miss = { .hit = false };

	const float len_sq = dx * dx + dy * dy + dz * dz;
	if (len_sq <= 0.0f)
		return miss;   // no direction to march along, and nothing to divide by.

	const float inv_len = 1.0f / approxSqrt(len_sq);
	dx *= inv_len;
	dy *= inv_len;
	dz *= inv_len;

	int x = floorToInt(ox);
	int y = floorToInt(oy);
	int z = floorToInt(oz);

	// Starting inside a wall is a real situation — the camera clipping into terrain —
	// and the honest answer is "this block, no face", not a guessed face that the ray
	// never actually crossed.
	//
	// blockIsSolid and NOT blockIsTargetable, deliberately, and this is the half of
	// v1.6.0 task 13's raycast change that is about what not to do. The rule here is "the
	// camera is stuck inside something", which only a block that fills its cell can do.
	// Widening it to everything targetable would mean that standing in a patch of tall
	// grass — a block the player walks straight through — returns a zero-distance hit on
	// the camera's own cell every frame, with no entry face, and nothing else could be
	// aimed at until they stepped out of it. The cost is that the one cell the camera is
	// already inside cannot be broken from inside it; the walk below starts testing at
	// the first boundary crossing, so every other cell is reachable exactly as before.
	if (blockIsSolid(worldGet(w, x, y, z))) {
		return (RayHit){ .hit = true, .x = x, .y = y, .z = z,
		                  .face = RAY_FACE_NONE, .px = x, .py = y, .pz = z,
		                  .distance = 0.0f };
	}

	const int step_x = (dx > 0.0f) - (dx < 0.0f);
	const int step_y = (dy > 0.0f) - (dy < 0.0f);
	const int step_z = (dz > 0.0f) - (dz < 0.0f);

	// t_max_*: distance along the (now unit-length) direction to the next boundary on
	// that axis, so it doubles as "distance travelled" once an axis is chosen.
	// t_delta_*: how much further that boundary sits each time the axis is crossed
	// again. An axis whose step is zero — the ray is parallel to it — is pinned to a
	// value neither of the other two axes can ever reach, rather than divided by the
	// zero direction component that would otherwise be its t_delta.
	const float kNever = 1e30f;
	float t_max_x = kNever, t_delta_x = kNever;
	float t_max_y = kNever, t_delta_y = kNever;
	float t_max_z = kNever, t_delta_z = kNever;

	if (step_x != 0) {
		t_delta_x = 1.0f / (dx * step_x);
		t_max_x   = ((float)(step_x > 0 ? x + 1 : x) - ox) / dx;
	}
	if (step_y != 0) {
		t_delta_y = 1.0f / (dy * step_y);
		t_max_y   = ((float)(step_y > 0 ? y + 1 : y) - oy) / dy;
	}
	if (step_z != 0) {
		t_delta_z = 1.0f / (dz * step_z);
		t_max_z   = ((float)(step_z > 0 ? z + 1 : z) - oz) / dz;
	}

	for (;;) {
		int axis;
		float t;
		if (t_max_x <= t_max_y && t_max_x <= t_max_z)      { axis = 0; t = t_max_x; }
		else if (t_max_y <= t_max_z)                       { axis = 1; t = t_max_y; }
		else                                                { axis = 2; t = t_max_z; }

		if (t > max_distance)
			return miss;   // ran out of reach before crossing another boundary.

		const int px = x, py = y, pz = z;
		int face;

		// Which face of the *new* cell the ray entered through is the opposite side
		// from the direction it stepped: stepping +X means the ray arrived from the
		// -X side, i.e. through that block's west face. Swap this and every block
		// placement ends up on the wrong side of the one you are looking at.
		if (axis == 0) {
			x += step_x;
			t_max_x += t_delta_x;
			face = step_x > 0 ? FACE_WEST : FACE_EAST;
		} else if (axis == 1) {
			y += step_y;
			t_max_y += t_delta_y;
			face = step_y > 0 ? FACE_BOTTOM : FACE_TOP;
		} else {
			z += step_z;
			t_max_z += t_delta_z;
			face = step_z > 0 ? FACE_NORTH : FACE_SOUTH;
		}

		// blockIsTargetable, not blockIsSolid (v1.6.0 task 13). What stops a ray is not
		// what stops a body: a plant is walked through and still has to be breakable, or
		// it is scenery that can never be removed. Identical to blockIsSolid for every
		// block in the registry as it stands — all of them are solid, none is a liquid —
		// so nothing about aiming at terrain changes.
		//
		// The whole cell is the target, not the two quads inside it. A cross fills its
		// cell's footprint horizontally and reaching only the X itself would mean a DDA
		// that tests geometry instead of cells, which is a much larger change than the
		// thing it buys (aiming through the corner of a plant).
		if (blockIsTargetable(worldGet(w, x, y, z))) {
			return (RayHit){ .hit = true, .x = x, .y = y, .z = z,
			                  .face = face, .px = px, .py = py, .pz = pz,
			                  .distance = t };
		}
	}
}
