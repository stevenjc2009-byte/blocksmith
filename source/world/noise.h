// Value noise in 16.16 fixed point.
//
// **Integer, not float, and that is the whole point.** The Phase 5.1 criterion is that a
// seed produces byte-identical chunks, and the tests that prove it run on x86 while the
// game runs on an ARM11. A float heightmap would agree to within an ulp on the two
// machines, and an ulp either side of a `(int)` threshold is a block of terrain that
// exists on one and not the other — so the host test would be proving something subtly
// different from what ships. Fixed point removes the question: every operation here is
// integer, so the two machines produce the same bits or one of them is broken.
//
// It is also the faster choice on this CPU. The ARM11 has a VFP, but generation runs on
// the worker thread in step 5.5 alongside meshing, and integer work is what that core has
// to spare.
//
// Nothing here includes <3ds.h>.
#pragma once

#include <stdint.h>

// 16.16: FX_ONE is 1.0. Values are kept in [0, FX_ONE] throughout, so a caller can scale
// to a block height with a single multiply and shift.
typedef int32_t fx;
#define FX_ONE    (1 << 16)
#define FX_SHIFT  16

static inline int32_t fxToInt(fx v)          { return v >> FX_SHIFT; }
static inline fx      fxFromInt(int32_t v)   { return v << FX_SHIFT; }

// 2D value noise on the unit lattice, sampled at a 16.16 position. Returns [0, FX_ONE].
//
// Value noise rather than Perlin/simplex on purpose: it needs one hash per lattice corner
// and no gradient table, it is trivially reproducible in integers, and at the scale a
// voxel heightmap is sampled at the visual difference is not worth the extra work. The
// characteristic axis-aligned blockiness is hidden by the octave sum below.
fx noiseValue2(uint32_t seed, fx x, fx z);

// Fractal sum of `octaves` value-noise layers, each at twice the frequency and half the
// amplitude of the last. Returns [0, FX_ONE], normalised by the amplitude sum so the
// range does not depend on the octave count. `octaves` is clamped to 1..8 — past 8 the
// amplitude is below one part in 256 and cannot move a block boundary.
fx noiseFbm2(uint32_t seed, fx x, fx z, int octaves);

// 3D value noise, for caves in step 5.4. Same contract, eight lattice corners.
fx noiseValue3(uint32_t seed, fx x, fx y, fx z);
fx noiseFbm3(uint32_t seed, fx x, fx y, fx z, int octaves);

// The final `total / amplitude-sum` step of the two fBms above, exposed only so the tests
// can brute-force it against the plain division it replaces. Not part of the noise API —
// nothing outside noise.c and the test suite should call it. `octaves` is clamped 1..8 as
// above, and `total` must be a sum of in-range samples, i.e. 0 <= total <= 65535 * norm.
fx noiseFbmNormalise(int64_t total, int octaves);
