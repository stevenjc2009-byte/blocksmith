#include "world/noise.h"

#include "world/rng.h"

// Lattice value at an integer corner: the top 16 bits of the positional hash, so the
// result is already a 16.16 fraction in [0, FX_ONE).
static inline fx corner2(uint32_t seed, int32_t ix, int32_t iz)
{
	return (fx)(rngHash2(seed, ix, iz) >> 16);
}

static inline fx corner3(uint32_t seed, int32_t ix, int32_t iy, int32_t iz)
{
	return (fx)(rngHash3(seed, ix, iy, iz) >> 16);
}

// Smoothstep, 3t^2 - 2t^3, on a 16.16 t in [0, FX_ONE].
//
// Computed in 64 bits. t^3 at t = 1.0 is 2^48 before the shifts, which overflows int32
// less than half way through, and the bug that produces is not a crash — it is terrain
// that looks almost right with occasional cliffs, which is exactly the kind of thing that
// survives to Phase 9 and then takes a day to find.
static inline fx smooth(fx t)
{
	const int64_t t2 = ((int64_t)t * t) >> FX_SHIFT;
	const int64_t t3 = (t2 * t) >> FX_SHIFT;
	return (fx)(3 * t2 - 2 * t3);
}

// Linear interpolation, a + (b - a) * t. b - a is at most FX_ONE in magnitude and t is at
// most FX_ONE, so the product fits comfortably in 64 bits with room to spare.
static inline fx lerp(fx a, fx b, fx t)
{
	return a + (fx)((((int64_t)b - a) * t) >> FX_SHIFT);
}

// Positions are carried through the octave loop in 64 bits, and the sampling functions
// take them that way.
//
// Not defensive programming — a fix for a real overflow. The octave loop doubles the
// sample position seven times, so a 16.16 position of even a few thousand blocks passes
// 2^31 before the last octave and signed overflow is undefined behaviour. Widening the
// *position* is the cheap fix; the lattice cell that comes out of it is still an int32,
// and at one cell per unit that only runs out two billion cells from the origin.
static inline void split(int64_t v, int32_t* cell, fx* frac)
{
	// An arithmetic shift floors towards negative infinity, which is what a lattice
	// needs — plain division truncates towards zero and would mirror the noise about
	// the origin, putting a seam through x = 0.
	*cell = (int32_t)(v >> FX_SHIFT);
	*frac = (fx)(v & (FX_ONE - 1));
}

static fx value2At(uint32_t seed, int64_t x, int64_t z)
{
	int32_t ix, iz;
	fx tx, tz;
	split(x, &ix, &tx);
	split(z, &iz, &tz);

	const fx sx = smooth(tx);
	const fx sz = smooth(tz);

	const fx v00 = corner2(seed, ix,     iz);
	const fx v10 = corner2(seed, ix + 1, iz);
	const fx v01 = corner2(seed, ix,     iz + 1);
	const fx v11 = corner2(seed, ix + 1, iz + 1);

	return lerp(lerp(v00, v10, sx), lerp(v01, v11, sx), sz);
}

static fx value3At(uint32_t seed, int64_t x, int64_t y, int64_t z)
{
	int32_t ix, iy, iz;
	fx tx, ty, tz;
	split(x, &ix, &tx);
	split(y, &iy, &ty);
	split(z, &iz, &tz);

	const fx sx = smooth(tx);
	const fx sy = smooth(ty);
	const fx sz = smooth(tz);

	const fx v000 = corner3(seed, ix,     iy,     iz);
	const fx v100 = corner3(seed, ix + 1, iy,     iz);
	const fx v010 = corner3(seed, ix,     iy + 1, iz);
	const fx v110 = corner3(seed, ix + 1, iy + 1, iz);
	const fx v001 = corner3(seed, ix,     iy,     iz + 1);
	const fx v101 = corner3(seed, ix + 1, iy,     iz + 1);
	const fx v011 = corner3(seed, ix,     iy + 1, iz + 1);
	const fx v111 = corner3(seed, ix + 1, iy + 1, iz + 1);

	const fx y0 = lerp(lerp(v000, v100, sx), lerp(v010, v110, sx), sy);
	const fx y1 = lerp(lerp(v001, v101, sx), lerp(v011, v111, sx), sy);
	return lerp(y0, y1, sz);
}

fx noiseValue2(uint32_t seed, fx x, fx z)              { return value2At(seed, x, z); }
fx noiseValue3(uint32_t seed, fx x, fx y, fx z)        { return value3At(seed, x, y, z); }

// Amplitude halves each octave and frequency doubles; the running total is divided by the
// amplitude sum at the end so the output range does not depend on the octave count.
//
// The per-octave seed is derived with rngMix rather than seed+i: adjacent seeds through a
// weak derivation give correlated octaves, which shows up as terrain repeating its own
// shape at two scales.
//
// The one division is per *sample*, not per block-column pair, and the ARM11 has no
// integer divide instruction — so this is a library call in the generator's inner loop.
// Left as a division because it is exact and obvious; Phase 9 is where it earns a
// reciprocal table if a measurement says it matters.
#define FBM_MAX_OCTAVES 8

static int clampOctaves(int octaves)
{
	if (octaves < 1) return 1;
	if (octaves > FBM_MAX_OCTAVES) return FBM_MAX_OCTAVES;
	return octaves;
}

fx noiseFbm2(uint32_t seed, fx x, fx z, int octaves)
{
	octaves = clampOctaves(octaves);

	int64_t  total = 0, norm = 0;
	int32_t  amp = FX_ONE;
	uint32_t s   = seed;
	int64_t  px = x, pz = z;

	for (int i = 0; i < octaves; i++) {
		total += (int64_t)value2At(s, px, pz) * amp;
		norm  += amp;
		amp  >>= 1;
		px    <<= 1;
		pz    <<= 1;
		s      = rngMix(s ^ 0x2545F491U);
	}
	return (fx)(total / norm);
}

fx noiseFbm3(uint32_t seed, fx x, fx y, fx z, int octaves)
{
	octaves = clampOctaves(octaves);

	int64_t  total = 0, norm = 0;
	int32_t  amp = FX_ONE;
	uint32_t s   = seed;
	int64_t  px = x, py = y, pz = z;

	for (int i = 0; i < octaves; i++) {
		total += (int64_t)value3At(s, px, py, pz) * amp;
		norm  += amp;
		amp  >>= 1;
		px    <<= 1;
		py    <<= 1;
		pz    <<= 1;
		s      = rngMix(s ^ 0x2545F491U);
	}
	return (fx)(total / norm);
}
