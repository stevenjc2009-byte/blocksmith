#include "gfx/fogramp.h"

float fogRampCurve(float t)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

uint8_t fogRampTexel(int i)
{
	if (i < 0) i = 0;
	if (i >= FOGRAMP_W) i = FOGRAMP_W - 1;

	// Texel CENTRE, not texel edge. GPU_LINEAR interpolates between centres, so a table built
	// at i/W would be half a texel out everywhere and the fade would finish half a texel early.
	const float t = ((float)i + 0.5f) / (float)FOGRAMP_W;

	// Rounded, not truncated — this has to match what tools/make_fog_ramp.py writes into the
	// PNG, and Python's round() on a value produced by the same arithmetic is the reference.
	// The +0.5 spelling is used rather than roundf() so this file needs no <math.h> and stays
	// linkable into anything.
	const float v = fogRampCurve(t) * 255.0f + 0.5f;
	return (uint8_t)(int)v;
}

FogShape fogShapeFor(float boundary_blocks, float strength)
{
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	if (boundary_blocks < 1.0f) boundary_blocks = 1.0f;

	FogShape s;
	s.end   = boundary_blocks * FOG_END_FRAC;
	s.start = s.end * FOG_START_FRAC;

	// strength divided into the range rather than multiplied into the factor, because the
	// factor is produced per-fragment by a texture fetch that nothing on the CPU can scale.
	// Stretching the range is the only place a strength can be applied at all, and it has the
	// right ends: 1 is the solved fade, and 0 collapses inv_range and bias to exactly 0 so
	// every vertex emits u = 0 and every fragment samples texel 0, which is 0.
	s.inv_range = strength / (s.end - s.start);
	s.bias      = -s.start * s.inv_range;
	return s;
}

float fogRampSample(const uint8_t texels[FOGRAMP_W], float u)
{
	// Texture space -> texel space. Texel i's centre is at u = (i + 0.5)/W, so the continuous
	// texel coordinate is u*W - 0.5 and GPU_LINEAR blends floor and floor+1 of it.
	float x = u * (float)FOGRAMP_W - 0.5f;

	// GPU_CLAMP_TO_EDGE. Outside the outermost centres there is nothing to blend with, so the
	// edge texel's own value is what the hardware returns — including for the u > 1 and u < 0
	// the shader deliberately produces beyond `end` and inside `start`.
	if (x <= 0.0f)                      return (float)texels[0] / 255.0f;
	if (x >= (float)(FOGRAMP_W - 1))    return (float)texels[FOGRAMP_W - 1] / 255.0f;

	const int   lo   = (int)x;
	const float frac = x - (float)lo;
	const float a    = (float)texels[lo]     / 255.0f;
	const float b    = (float)texels[lo + 1] / 255.0f;
	return a + frac * (b - a);
}

float fogVisibility(const FogShape* s, const uint8_t texels[FOGRAMP_W], float blocks)
{
	// Exactly what the vertex shader computes into outtc1 (world_dynamic.v.pica: one mul and
	// one add against fogParams), then exactly what the sampler and TEV stage 1 make of it.
	// TEV stage 1 is GPU_INTERPOLATE(constant, previous, texture1.alpha), i.e.
	// colour = sky*f + terrain*(1-f), so visibility is 1 - f by construction.
	const float u = blocks * s->inv_range + s->bias;
	return 1.0f - fogRampSample(texels, u);
}

float fogHalfVis(const FogShape* s, const uint8_t texels[FOGRAMP_W])
{
	// Monotone decreasing in `blocks`, so bisection converges. The bracket starts at 0.01
	// rather than 0 for the same reason render_dist.c's solveDistance does: a distance of
	// exactly 0 is not a place the player can stand.
	float lo = 0.01f, hi = 4096.0f;
	for (int i = 0; i < 60; i++) {
		const float mid = 0.5f * (lo + hi);
		if (fogVisibility(s, texels, mid) > 0.5f) lo = mid;
		else                                      hi = mid;
	}
	return 0.5f * (lo + hi);
}
