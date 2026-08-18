#include "scene/render_dist.h"

#include <math.h>

// The near plane at the shortest render distance, and how much each extra column of distance
// buys. 0.10 is what steps 1-7.6 shipped and it is deliberately left exactly alone at radius
// 1, so raising the setting is the only thing that can change how the world looks near the
// camera. Each further column adds 0.05, which moves the first fog knot out by about 6 blocks
// per step, until RENDER_DIST_NEAR_MAX stops it.
#define NEAR_BASE  0.10f
#define NEAR_STEP  0.05f

// The far plane, unchanged across the whole range. It is not the limiter — knots are set by
// `near`, and raising far only pushes the first knot asymptotically toward 128 x near — and
// there is no geometry out there to clip in any case.
#define FAR_PLANE  200.0f

// Shapes the near ramp: 4% fogged at 2.1 blocks, 33% at 4.2, 65% at 6.2 at radius 1. Held
// constant across the range so changing render distance changes where the fade *ends* and
// not how it feels underfoot.
#define FOG_GRADIENT_ALL  2.44f

// Entries in the hardware fog LUT. Knots therefore sit at index 0, 1/128, 2/128 ... 1.
#define LUT_ENTRIES  128

float renderDistLutZ(float index01, float near_plane, float far_plane)
{
	return far_plane * near_plane / (index01 * (far_plane - near_plane) + near_plane);
}

// The inverse: LUT position of a view distance. 0 at the far plane, 1 at the near plane.
static float lutIndex01(float blocks, float near_plane, float far_plane)
{
	if (blocks <= 0.0f) return 1.0f;
	const float i = (far_plane * near_plane / blocks - near_plane) / (far_plane - near_plane);
	return i < 0.0f ? 0.0f : (i > 1.0f ? 1.0f : i);
}

// The exponential FogLut_Exp is asked for: visibility = exp(-(density * z)^gradient).
static float fogCurve(float density, float gradient, float z)
{
	if (density <= 0.0f) return 1.0f;     // -DFOG_DENSITY=0.0f, the check that goes red
	return expf(-powf(density * z, gradient));
}

float renderDistVisibility(const RenderDist* rd, float blocks)
{
	// Where in the LUT this distance lands, in entries.
	const float pos = lutIndex01(blocks, rd->near_plane, rd->far_plane) * (float)LUT_ENTRIES;

	int lo = (int)pos;
	if (lo >= LUT_ENTRIES) lo = LUT_ENTRIES - 1;
	const float frac = pos - (float)lo;

	// The two knots either side, and a straight line between them — which is what the
	// hardware does, and the reason a curve asked to fade out past the first knot is not the
	// curve that gets drawn. Between entry 0 (the far plane) and entry 1 there is nothing at
	// all, so out there this function is a single line however carefully the exponential was
	// chosen.
	const float z_lo = renderDistLutZ((float)lo / (float)LUT_ENTRIES,
	                                  rd->near_plane, rd->far_plane);
	const float z_hi = renderDistLutZ((float)(lo + 1) / (float)LUT_ENTRIES,
	                                  rd->near_plane, rd->far_plane);

	const float v_lo = fogCurve(rd->fog_density, rd->fog_gradient, z_lo);
	const float v_hi = fogCurve(rd->fog_density, rd->fog_gradient, z_hi);

	return v_lo + (v_hi - v_lo) * frac;
}

// Density that makes the *interpolated* curve hit `target` visibility at `blocks`.
//
// Solved by bisection rather than algebraically, and that is the point rather than laziness:
// the algebra of the exponential is easy, but the exponential is not what the hardware draws.
// Inverting the interpolated function means inverting "which knots does this distance fall
// between", which changes with density's effect on nothing at all — the knots move with
// `near`, not with density — but the two-knot blend still has to be solved through, and a
// bisection over the function the player actually sees cannot be wrong about which function
// that is. Visibility is monotone decreasing in density everywhere, so bisection converges.
static float solveDensity(RenderDist* rd, float target, float blocks)
{
	float lo = 0.0001f, hi = 8.0f;
	for (int i = 0; i < 60; i++) {
		const float mid = 0.5f * (lo + hi);
		rd->fog_density = mid;
		if (renderDistVisibility(rd, blocks) > target) lo = mid;  // too clear, fog harder
		else                                           hi = mid;
	}
	return 0.5f * (lo + hi);
}

// Distance at which the interpolated curve reaches `vis`. The answer to "how far can the
// player actually see", which is not the render distance and is the number that says whether
// raising the setting bought anything.
static float solveDistance(const RenderDist* rd, float vis)
{
	float lo = 0.01f, hi = rd->far_plane;
	for (int i = 0; i < 60; i++) {
		const float mid = 0.5f * (lo + hi);
		if (renderDistVisibility(rd, mid) > vis) lo = mid;   // still clear here, go further
		else                                     hi = mid;
	}
	return 0.5f * (lo + hi);
}

RenderDist renderDistFor(int radius)
{
	if (radius < RENDER_DIST_MIN) radius = RENDER_DIST_MIN;
	if (radius > RENDER_DIST_MAX) radius = RENDER_DIST_MAX;

	RenderDist rd;
	rd.radius       = radius;
	rd.area_radius  = radius + 1;
	rd.slots        = (2 * radius + 1) * (2 * radius + 1) * RENDER_DIST_SLOTS_PER_COLUMN;
	rd.far_plane    = FAR_PLANE;
	rd.fog_gradient = FOG_GRADIENT_ALL;

	rd.near_plane = NEAR_BASE + NEAR_STEP * (float)(radius - RENDER_DIST_MIN);
	if (rd.near_plane > RENDER_DIST_NEAR_MAX) rd.near_plane = RENDER_DIST_NEAR_MAX;

	// Worst case, and worst case rather than typical on purpose: the ring is centred on the
	// column the player stands in, so standing on that column's edge puts the far boundary
	// radius x 16 blocks away and the near one at nothing. Fog that only hides the typical
	// case is fog that fails every time the player crosses a chunk edge, which is every few
	// seconds of walking.
	rd.boundary   = (float)(CHUNK_DIM * radius);
	rd.first_knot = renderDistLutZ(1.0f / (float)LUT_ENTRIES, rd.near_plane, rd.far_plane);

	rd.fog_density = solveDensity(&rd, RENDER_DIST_TARGET_VIS, rd.boundary);
	rd.knot_vis    = fogCurve(rd.fog_density, rd.fog_gradient, rd.first_knot);
	rd.half_vis    = solveDistance(&rd, 0.5f);
	rd.fog_hides   = renderDistVisibility(&rd, rd.boundary) <= RENDER_DIST_TARGET_VIS * 1.01f;
	return rd;
}

int renderDistDefault(bool new_3ds)
{
	return new_3ds ? RENDER_DIST_MAX : RENDER_DIST_MIN;
}
