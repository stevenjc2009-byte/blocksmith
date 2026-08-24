// The pre-task-48b world/noise.c, verbatim. See world/noise_ref.h for why it is still here.
//
// Do not tidy this file, do not fix its comments to match the new one, and do not "keep it
// in sync". Its entire value is that it is the old code — the moment it is edited to look
// like the new code it stops proving anything.
#ifndef __3DS__

#include "world/noise_ref.h"

#include "world/rng.h"

static inline fx corner2(uint32_t seed, int32_t ix, int32_t iz)
{
	return (fx)(rngHash2(seed, ix, iz) >> 16);
}

static inline fx corner3(uint32_t seed, int32_t ix, int32_t iy, int32_t iz)
{
	return (fx)(rngHash3(seed, ix, iy, iz) >> 16);
}

static inline fx smooth(fx t)
{
	const int64_t t2 = ((int64_t)t * t) >> FX_SHIFT;
	const int64_t t3 = (t2 * t) >> FX_SHIFT;
	return (fx)(3 * t2 - 2 * t3);
}

static inline fx lerp(fx a, fx b, fx t)
{
	return a + (fx)((((int64_t)b - a) * t) >> FX_SHIFT);
}

static inline void split(int64_t v, int32_t* cell, fx* frac)
{
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

fx noiseRefValue2(uint32_t seed, fx x, fx z)       { return value2At(seed, x, z); }
fx noiseRefValue3(uint32_t seed, fx x, fx y, fx z) { return value3At(seed, x, y, z); }

#define FBM_MAX_OCTAVES 8

static int clampOctaves(int octaves)
{
	if (octaves < 1) return 1;
	if (octaves > FBM_MAX_OCTAVES) return FBM_MAX_OCTAVES;
	return octaves;
}

fx noiseRefFbm2(uint32_t seed, fx x, fx z, int octaves)
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

fx noiseRefFbm3(uint32_t seed, fx x, fx y, fx z, int octaves)
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

int64_t noiseRefNorm(int octaves)
{
	octaves = clampOctaves(octaves);

	int64_t norm = 0;
	int32_t amp  = FX_ONE;
	for (int i = 0; i < octaves; i++) { norm += amp; amp >>= 1; }
	return norm;
}

#endif /* !__3DS__ */
