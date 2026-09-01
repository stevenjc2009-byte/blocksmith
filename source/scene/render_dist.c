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

// Entries in the hardware fog LUT. Knots therefore sit at index 0, 1/128, 2/128 ... 1.
#define LUT_ENTRIES  128

// The first knot the curve is allowed to be fully clear at, and therefore where the ramp
// starts. It cannot be 1: knot 1 is the last knot OUTSIDE the load boundary at every radius in
// range, so its value is pinned by the requirement that the fade be finished by then. 2 is the
// next one out, so it is the earliest the world can be clear again — 10.90 blocks at radius 3,
// 6.21 at radius 1.
//
// Raising it to 3 moves the clear point in to 7.40 blocks (radius 3) and holds knot 2 down at
// the pinned value with knot 1, which makes the world opaque across the whole 10.90-to-20.68
// segment instead of fading across it. Rejected, and the cost is not marginal — measured on the
// host by rebuilding this file with the constant changed and nothing else:
//
//     FOG_CLEAR_KNOT   half_vis r1   half_vis r2   half_vis r3
//     2 (shipped)         8.2035       12.0740       14.3358
//     3                   5.0004        7.4147        8.8402
//     4                   3.5962        5.3501        6.3905
//
// Note what those rows are and are not. They measure this constant, which fogs knot 2 all the
// way DOWN to knot 1's value — the extreme. A gentler intermediate is available and is not
// measured here: holding knot 2 at, say, 0.5 instead of 1.0 would fade from clear at 7.40 to
// half at 10.90 to hidden at 20.68, a three-point fade rather than a two-point one.
//
// It is not offered because it cannot beat this, only soften it. The boundary constraint binds
// between knots 0 and 1 — at radius 3 the load boundary is 48 blocks and knot 1 is 20.68, so
// the blend that has to reach the target uses knot 0 and knot 1 and NOTHING inside them. Knot 2
// is therefore free, and free means the only thing its value trades against is half_vis, which
// falls monotonically as it does. Any near fog at all is a look choice paid for in visible
// world, at a rate this table gives the ends of. Whether some of it should be bought back is
// steve's call and not this file's; the abruptness it leaves is real, see renderDistFogTable.
#define FOG_CLEAR_KNOT  2

// ── What v1.8.5 replaced, kept because it is the measurement, not the code ────────────────
//
// Until v1.8.5 this file solved for a DENSITY feeding a fixed exponential shape,
// visibility = exp(-(density * z)^gradient), with the gradient a constant 2.44 across the whole
// distance range. That gradient was chosen for how the near field felt: 4% fogged at 2.1
// blocks, 33% at 4.2, 65% at 6.2, at radius 1.
//
// That near-field fade is what the new curve gives up, and giving it up is the entire gain. The
// exponential was never a free choice — it had to reach RENDER_DIST_TARGET_VIS at the load
// boundary, and the ONLY knot it can be shaped at outside that boundary is knot 1, so an
// exponential steep enough to be finished by 48 blocks is necessarily most of the way to opaque
// by 6. The fog at 4 blocks was not buying anything; it was the tail of a constraint 40 blocks
// further out. Filling the knots directly separates the two, and the measured cost of the old
// coupling was 34 % of the visible world (9.4950 blocks against 14.3358 at radius 3).
//
// NOT verified, and it cannot be verified here: how the new shape LOOKS. A clear near field
// that starts fading at 10.9 blocks is a different picture from one that is a third fogged at
// 4, and every number in this file is a host measurement of what the LUT will contain, not of
// what a screen shows. No console has rendered it. The thing to look for is the far side of the
// ramp: 10.9 to 20.7 blocks at radius 3 is where the whole fade now lives, so if it reads as a
// wall of sky rather than as distance, that is this change and not the terrain.

// The 11-bit value field and 13-bit signed difference field citro3d packs each LUT entry into
// are both fixed-point with this scale, i.e. 1/2048 per step.
//
// READ OUT OF THE SHIPPED LIBRARY, not from documentation: arm-none-eabi-objdump -d on the
// fog.o extracted from /c/devkitPro/libctru/lib/libcitro3d.a shows FogLut_FromArray multiplying
// both halves of the input by 0x45000000 (2048.0f), converting toward zero, clamping the value
// to 0x7ff and the difference to [-4096, 4095], and packing value << 13 | (difference & 0x1fff).
#define FOG_FIXED_SCALE  2048.0f

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

void renderDistFogTable(const RenderDist* rd, float out[RENDER_DIST_FOG_FLOATS])
{
	// fog_density stopped being a density in v1.8.5 and became the strength of the whole fade:
	// 1 draws the solved curve, 0 leaves every knot at full visibility, which is what
	// -DFOG_DENSITY=0.0f asks for and is still "no fog through this exact code path" rather
	// than through a different one. Clamped rather than trusted because it comes in from a
	// build-time override that nothing else validates.
	float amount = rd->fog_density;
	if (amount < 0.0f) amount = 0.0f;
	if (amount > 1.0f) amount = 1.0f;

	// The knot values. Three regions and no curve at all, which is the point:
	//
	//   knot 0            the far plane, 200 blocks. Fully fogged. Nothing is drawn out there
	//                     — the load boundary is 48 blocks at the widest — and holding it at 0
	//                     is what buys knot 1 the most room. The boundary falls between these
	//                     two knots at every radius in range, so what the hardware shows there
	//                     is (1 - frac) * knot 0 + frac * knot 1; at knot 0 = 0 that collapses
	//                     to target / frac, which is the largest knot 1 the constraint allows.
	//                     (frac is 0.74 at radius 1 and 0.37 at radius 3 — the split moves, the
	//                     argument for zeroing knot 0 does not.)
	//   knot 1            pinned by the boundary constraint. Solved in solveKnotVis.
	//   knot 2 and in     clear. There is no constraint on them at all, and every scrap of fog
	//                     put here comes straight off half_vis.
	for (int i = 0; i < LUT_ENTRIES; i++) {
		const float v = (i == 0)              ? 0.0f
		              : (i < FOG_CLEAR_KNOT)  ? rd->knot_vis
		              :                         1.0f;

		// Blended as v plus the distance back to clear, rather than as the algebraically
		// identical 1 - amount * (1 - v). The two differ in float: at amount 1 — every shipping
		// build — this spelling returns v BIT FOR BIT, where the other returns 1 - (1 - v),
		// which for a knot_vis of 0.0142 comes back wrong in the seventh digit. That is far too
		// small to see and exactly large enough to make "the table's knot 1 IS rd.knot_vis" a
		// claim world_test.c could only check with a tolerance, which is a weaker claim than
		// the code can actually support.
		out[i] = v + (1.0f - amount) * (1.0f - v);
	}

	// The difference half, by literal subtraction of the value half rather than by re-deriving
	// the same numbers a second way. citro3d's FogLut_Exp computes exactly this (objdump: it
	// stores v, then v - previous, at [r5, #504]), and computing it any other way would let the
	// two halves disagree — which the hardware would resolve by drawing the differences, since
	// they are what it interpolates with.
	for (int i = 0; i + 1 < LUT_ENTRIES; i++)
		out[LUT_ENTRIES + i] = out[i + 1] - out[i];

	// The last difference is against a 129th knot at index01 = 1.0, the near plane itself,
	// which has no slot of its own. FogLut_Exp evaluates the curve there; this curve is flat
	// clear from FOG_CLEAR_KNOT inward, so that knot equals knot 127 and the difference is 0.
	out[RENDER_DIST_FOG_FLOATS - 1] = 0.0f;
}

// What the hardware makes of one knot value: 11 bits of 1/2048ths, truncated toward zero, and
// saturating at 2047 — so a value of exactly 1.0 is stored as 2047/2048, not as 1.
static float fogQuantValue(float v)
{
	if (v <= 0.0f) return 0.0f;
	const float s = v * FOG_FIXED_SCALE;
	if (s >= FOG_FIXED_SCALE) return 2047.0f / FOG_FIXED_SCALE;
	return (float)(int)s / FOG_FIXED_SCALE;
}

// And one difference: 13 bits signed, same scale, also truncated toward zero, clamped to
// [-4096, 4095]. The clamp is unreachable for this curve — no step is larger than 1.0 — but it
// is what the library does and a model that only matches on the inputs it was tested with is
// not a model.
static float fogQuantDiff(float d)
{
	if (d == 0.0f) return 0.0f;
	const float s = d * FOG_FIXED_SCALE;
	if (s < -4096.0f) return -4096.0f / FOG_FIXED_SCALE;
	if (s >  4095.0f) return  4095.0f / FOG_FIXED_SCALE;
	return (float)(int)s / FOG_FIXED_SCALE;
}

float renderDistVisibility(const RenderDist* rd, float blocks)
{
	// The real table, built by the real function. This is the whole reason v1.8.5 is safe: the
	// predictor cannot describe a curve the LUT does not contain, because it has no curve of
	// its own to describe. 1 KB of stack per call, and on the console this only ever runs from
	// renderDistFor — boot and a player pressing a shoulder button — so there is nothing to
	// optimise and a shared static would only make it unsafe to call from two threads. The host
	// suite calls it directly and in loops, which is free.
	float lut[RENDER_DIST_FOG_FLOATS];
	renderDistFogTable(rd, lut);

	// Where in the LUT this distance lands, in entries.
	const float pos = lutIndex01(blocks, rd->near_plane, rd->far_plane) * (float)LUT_ENTRIES;

	int lo = (int)pos;
	if (lo >= LUT_ENTRIES) lo = LUT_ENTRIES - 1;
	const float frac = pos - (float)lo;

	// value + frac * difference, which is what the fixed-function stage evaluates, and the
	// reason the LUT stores a difference per entry instead of the caller taking two samples.
	// Between entry 0 (the far plane) and entry 1 there is nothing at all, so out there this is
	// a single straight line however carefully the table was filled — see the header comment in
	// render_dist.h, which is the constraint this whole file exists to work inside.
	//
	// Quantised, because the hardware's copy is 11 and 13 bits and the difference matters here:
	// one step is 1/2048 = 0.000488, against a RENDER_DIST_TARGET_VIS of 0.005. Modelling it in
	// floats would flatter the boundary check by up to 10 % of the thing it is checking.
	//
	// PROVENANCE, since the two halves are not equally solid. The packing is MEASURED — objdump
	// on the shipped libcitro3d.a, quoted at FOG_FIXED_SCALE above. That the hardware then
	// evaluates value + frac * difference is INFERRED: it is the only reading consistent with an
	// encoder that stores 128 values and 128 forward differences, and it is the same linear
	// interpolation this function already assumed before v1.8.5, but no PICA200 has confirmed
	// it here. If it is wrong, it was wrong before this change too.
	return fogQuantValue(lut[lo]) + frac * fogQuantDiff(lut[LUT_ENTRIES + lo]);
}

// The largest knot-1 visibility at which the fade is still complete by `blocks`.
//
// Solved by bisection rather than algebraically, and that is the point rather than laziness.
// The algebra looks trivial — knot 0 is 0, so the visibility at the boundary is just frac times
// knot 1 — but it is only trivial while the boundary happens to fall in the segment between
// knots 0 and 1, which is true at radius 1, 2 and 3 and is not a thing this file should assume
// on behalf of a radius nobody has added yet. A bisection over the function the player actually
// sees cannot be wrong about which function that is. Visibility is monotone increasing in
// knot_vis, so it converges.
//
// It returns `lo`, the last value it PROVED safe, and not the midpoint of the final bracket
// like the density solve it replaced. That is not tidiness: renderDistVisibility quantises to
// 1/2048ths now, so the function being inverted is a staircase, and the midpoint of a bracket
// straddling a step can sit on the wrong tread. Returning `lo` makes "the fade is complete by
// the boundary" true by the loop's own invariant instead of by a rounding argument. lo starts
// at 0 — every knot fogged — which is trivially safe, so the invariant holds from the start.
static float solveKnotVis(RenderDist* rd, float target, float blocks)
{
	float lo = 0.0f, hi = 1.0f;
	for (int i = 0; i < 60; i++) {
		const float mid = 0.5f * (lo + hi);
		rd->knot_vis = mid;
		if (renderDistVisibility(rd, blocks) <= target) lo = mid;  // still hidden, allow more
		else                                            hi = mid;
	}
	rd->knot_vis = lo;
	return lo;
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

	// Full strength. scene/chunk_render.c overrides this field on its own copy when
	// -DFOG_DENSITY is set, so the value here is what an unmodified build draws and nothing
	// upstream of that override ever sees a different one.
	rd.fog_density  = 1.0f;

	rd.near_plane = NEAR_BASE + NEAR_STEP * (float)(radius - RENDER_DIST_MIN);
	if (rd.near_plane > RENDER_DIST_NEAR_MAX) rd.near_plane = RENDER_DIST_NEAR_MAX;

	// Worst case, and worst case rather than typical on purpose: the ring is centred on the
	// column the player stands in, so standing on that column's edge puts the far boundary
	// radius x 16 blocks away and the near one at nothing. Fog that only hides the typical
	// case is fog that fails every time the player crosses a chunk edge, which is every few
	// seconds of walking.
	rd.boundary   = (float)(CHUNK_DIM * radius);
	rd.first_knot = renderDistLutZ(1.0f / (float)LUT_ENTRIES, rd.near_plane, rd.far_plane);

	// knot_vis is now solved rather than read off a curve, and everything downstream reads the
	// table built from it. The ordering matters: solveKnotVis leaves the field set, and
	// solveDistance and fog_hides both go back through renderDistVisibility, which rebuilds the
	// table from it — so there is no window in which the diagnostics describe a different curve
	// from the one chunkRenderSetDistance uploads.
	rd.knot_vis  = solveKnotVis(&rd, RENDER_DIST_TARGET_VIS, rd.boundary);
	rd.half_vis  = solveDistance(&rd, 0.5f);
	rd.fog_hides = renderDistVisibility(&rd, rd.boundary) <= RENDER_DIST_TARGET_VIS * 1.01f;
	return rd;
}

int renderDistMaxFor(bool new_3ds)
{
	const int model = new_3ds ? RENDER_DIST_MAX_NEW : RENDER_DIST_MAX_OLD;

	// The smaller of the two, not the per-model number. See render_dist.h for the full argument;
	// the short form is that RENDER_DIST_MAX_OLD/_NEW say what the console's linear HEAP holds
	// and RENDER_DIST_MAX says what the mesh pool was ALLOCATED for, and while chunkRenderInit
	// still sizes its arenas from the compile-time constant the second one binds on both models.
	//
	// Written as a min rather than as `return RENDER_DIST_MAX;` with a comment promising to come
	// back, so that lifting RENDER_DIST_MAX is the only edit that lift requires and this function
	// starts returning 5 for a New 3DS on its own.
	return model < RENDER_DIST_MAX ? model : RENDER_DIST_MAX;
}

int renderDistClampFor(int radius, bool new_3ds)
{
	// Clamped rather than rejected, for the same reason renderDistFor() clamps: this is a
	// setting a player pokes at, and one that silently did nothing at the ends would read as a
	// broken button. The difference is only which ceiling it stops at.
	const int hi = renderDistMaxFor(new_3ds);

	if (radius < RENDER_DIST_MIN) return RENDER_DIST_MIN;
	if (radius > hi)              return hi;
	return radius;
}

int renderDistDefault(bool new_3ds)
{
	// RENDER_DIST_DEFAULT_NEW, not RENDER_DIST_MAX and not renderDistMaxFor(). See that macro
	// for why the two were separated: written as MAX, this line made every ceiling raise a
	// silent default raise.
	const int want = new_3ds ? RENDER_DIST_DEFAULT_NEW : RENDER_DIST_MIN;

	// Out through the same clamp the setting goes through, so a first boot cannot seed
	// options.ini with a value that the very next load would move. A no-op at today's constants
	// — render_dist.h _Static_asserts that RENDER_DIST_DEFAULT_NEW is inside the narrower
	// console's range — and deliberately so: this line exists to keep the default and the
	// setting governed by ONE range, not to widen either of them.
	return renderDistClampFor(want, new_3ds);
}
