// world/noise.c exactly as it stood before task 48b, kept compiled so the optimisation that
// replaced it can be checked and timed against it in one process.
//
// **This is a test fixture, not a second noise implementation.** Nothing in the game may
// call it; the whole file is compiled out on the console. It exists because the claim made
// by rngHash2Cell / rngHash3Cell and noiseFbmNormalise is "bit-for-bit the same answer as
// before", and the only way to hold a claim like that honestly is to keep the before.
//
// It lives in its own translation unit on purpose. When the reference sat inside the test
// file it was inlined into the benchmark loop while the real noise.c stayed a cross-module
// call, and the resulting "measurement" said the new code was 69 % slower. Both arms have
// to be compiled the way the game compiles them — separate objects, no LTO — or the number
// is about the compiler, not the code.
#pragma once

#ifndef __3DS__

#include "world/noise.h"

fx noiseRefValue2(uint32_t seed, fx x, fx z);
fx noiseRefValue3(uint32_t seed, fx x, fx y, fx z);
fx noiseRefFbm2(uint32_t seed, fx x, fx z, int octaves);
fx noiseRefFbm3(uint32_t seed, fx x, fx y, fx z, int octaves);

// The amplitude sum the reference divides by, so the normalise checks can name it.
int64_t noiseRefNorm(int octaves);

#endif /* !__3DS__ */
