// v1.8.10 water shimmer — the scroll, and nothing else.
//
// Blocksmith's water is a flat 0.70-alpha colour, and a flat colour reads as a sheet of blue
// plastic no matter how good the tile is: nothing on it moves, so nothing says "liquid". This
// header is the moving half. It is the arithmetic that turns a clock reading into the UV offset
// scene/chunk_render.c uploads to world_dynamic.v.pica's `waterShimmer` uniform, which the
// shader adds to a chunk-local XZ coordinate on texcoord2, which texture unit 2 samples
// gfx/watershimmer.png with. The glints drift; the water stops being plastic.
//
// WHY IT IS A HEADER OF ITS OWN, and not four lines inside chunk_render.c. Same reason
// gfx/fogramp.c is split from gfx/fogtex.c and world/mesher.h holds WATER_ALPHA: scene/
// chunk_render.c includes <3ds.h> and NO host test can link it. Everything here is plain C with
// no libctru, no citro3d and no libm, so world/water_alpha_test.c links this exact file rather
// than a copy of the rule — which is the failure mode tools/run_host_tests.sh records against
// app/battery.c and app/sleep.c, where a suite carrying its own copy of the arithmetic stayed
// green with the real one deleted.
//
// WHY MODULO-THEN-DIVIDE rather than the obvious `fmodf(seconds * speed, 1.0f)`:
//
//   * It needs no libm. The water_alpha_test stanza in tools/run_host_tests.sh does not link
//     -lm, and neither does anything else this would be reachable from.
//   * It cannot drift. `seconds * speed` grows without bound, and a float32 holding a large
//     number has no fractional resolution left: the console's tick counter is milliseconds
//     since the SYSTEM booted, not since the game did, so on a 3DS that has been in sleep mode
//     for a week that product is in the millions before the fractional part is taken. The
//     shimmer would visibly quantise and eventually freeze. An integer remainder taken FIRST
//     never exceeds the period, so the float it is finally divided by is always small.
//   * It wraps exactly. The result is in [0, 1) by construction rather than by a clamp, so the
//     scroll cannot walk off the end of the texture even in principle.
//
// The one seam this leaves is the uint64 rollover at 2^64 ms, which is 585 million years.
//
// WHY TWO DIFFERENT PERIODS. With one period the offset would move along a fixed diagonal and
// the whole sheet would slide as a rigid unit — correct, but the pattern repeats every 9
// seconds and the eye finds that. 9 and 13 are coprime, so the pair of phases does not repeat
// for 9*13 = 117 seconds, which is long enough that it does not read as a loop. Both are slow:
// gfx/watershimmer.png repeats every 1/WATER_SHIMMER_UV_PER_BLOCK = 8 blocks, so a full u wrap
// in 9 s is 8/9 = 0.89 blocks per second of drift. That is the intended look — the highlight
// pattern visibly MOVES. A fast scroll on a sparse high-contrast texture is the documented
// wrong answer (docs/plan-1.8.10-light.md 5.4): it reads as the whole surface strobing.
#pragma once

#include <stdint.h>

// Texture repeats per block, i.e. the sheet covers 1/this = 8 blocks.
//
// It has to divide CHUNK_DIM (16, world/chunk.h) EXACTLY, and that is the only reason this is
// 0.125 rather than any other pleasant number. The shader builds the shimmer coordinate from
// `inpos`, which is CHUNK-LOCAL 0..16 — the chunk's world offset lives in modelView, and the
// vertex format is locked at 8 bytes (world/mesh_vertex.h) so there is nowhere to put a world
// coordinate. 16 * 0.125 = 2.0, a whole number of repeats, so the pattern arriving at a chunk's
// far edge is the pattern leaving its near edge and the sheet is continuous across the chunk
// boundary. At, say, 0.1 per block a chunk would span 1.6 repeats and every chunk seam in the
// ocean would show a hard discontinuity in the glints — a 16-block grid drawn on the water,
// which is a picture, not an error.
#define WATER_SHIMMER_UV_PER_BLOCK  0.125f

// Milliseconds for one full wrap of the sheet in u and in v. Coprime on purpose; see above.
#define WATER_SHIMMER_PERIOD_U_MS   9000u
#define WATER_SHIMMER_PERIOD_V_MS   13000u

// The scroll phase for one axis: where in [0, 1) the sheet has drifted to at `ms`.
static inline float waterShimmerPhase(uint64_t ms, uint32_t period_ms)
{
	// A zero period would be a division by zero rather than a still shimmer, and the constants
	// above are the only callers, so this is a guard against an edit rather than against input.
	if (period_ms == 0u) return 0.0f;
	return (float)(uint32_t)(ms % (uint64_t)period_ms) / (float)period_ms;
}

static inline float waterShimmerOffsetU(uint64_t ms)
{
	return waterShimmerPhase(ms, WATER_SHIMMER_PERIOD_U_MS);
}

static inline float waterShimmerOffsetV(uint64_t ms)
{
	return waterShimmerPhase(ms, WATER_SHIMMER_PERIOD_V_MS);
}
