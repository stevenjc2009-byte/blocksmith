// Deterministic hashing and a small PRNG for world generation.
//
// Two different things, deliberately kept apart:
//
//   * `rngHash2` / `rngHash3` are **positional** — a pure function of the seed and a
//     coordinate. Terrain must use these, never a running stream, because chunks are
//     generated in whatever order the player walks into them. A stream would make a
//     chunk's contents depend on how many chunks were generated before it, so walking
//     east then north would build a different world from north then east, and a reload
//     would build a third. The Phase 5.1 criterion — same seed, byte-identical chunks —
//     is only reachable this way.
//   * `Rng` is a sequential xorshift32 stream, for decoration *within* one chunk once a
//     positional hash has fixed its starting point (how many trees, where in the chunk).
//     Seed it from rngHash2 and it stays reproducible.
//
// Nothing here includes <3ds.h>, so the tests run on the PC.
#pragma once

#include <stdint.h>

// The 32-bit mixer from Chris Wellons' prospector search (lowbias32). Chosen over the
// usual Wang hash for its avalanche: neighbouring block coordinates differ in one bit,
// and a weak mixer leaves that visible as a grid pattern in the terrain.
static inline uint32_t rngMix(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return h;
}

// Positional hashes. Signed coordinates convert to uint32_t modulo 2^32, which is
// defined behaviour, and every arithmetic step below is on unsigned types — so this
// cannot trap or wrap into UB at the edges of the world.
static inline uint32_t rngHash2(uint32_t seed, int32_t x, int32_t z)
{
	uint32_t h = seed ^ 0x9E3779B9U;
	h = rngMix(h ^ ((uint32_t)x * 0x9E3779B1U));
	h = rngMix(h ^ ((uint32_t)z * 0x85EBCA77U));
	return h;
}

static inline uint32_t rngHash3(uint32_t seed, int32_t x, int32_t y, int32_t z)
{
	uint32_t h = rngHash2(seed, x, z);
	h = rngMix(h ^ ((uint32_t)y * 0xC2B2AE3DU));
	return h;
}

// **Do not hand-factor the corners of a lattice cell here.** Task 48b tried exactly that —
// an rngHash3Cell() returning all eight corners of one cell by sharing the x and (x,z)
// stages that the eight separate rngHash3 calls appear to recompute, 14 rngMix and 6
// multiplies on paper instead of 24 and 24. It was bit-for-bit correct and it was SLOWER.
//
// The reason is that the sharing is not there to be won: with rngHash2 and rngHash3 both
// `static inline`, GCC already common-subexpression-eliminates the shared stages across the
// eight call sites in value3At. Measured on the ARM11 toolchain the game actually ships
// with (-march=armv6k -mtune=mpcore -O2), world/noise.c compiled to 94 multiplies in 508
// instructions BEFORE the factoring and 101 in 535 after it. On the host, timed against the
// unfactored build in the same process, the factored form ran 0.974 ms against 0.881 ms
// over 24,576 samples — 10.5 % worse, because writing the corners out through an array
// costs more scheduling freedom than the arithmetic it saves.
//
// The optimiser sees this one. Spend the effort on calling the noise fewer times instead.

// xorshift32. Period 2^32-1, and 0 is a fixed point — so seeding is forced away from it
// rather than left as a trap for the caller.
typedef struct {
	uint32_t state;
} Rng;

static inline void rngSeed(Rng* r, uint32_t seed)
{
	r->state = seed ? seed : 0x1D872B41U;
}

static inline uint32_t rngNext(Rng* r)
{
	uint32_t x = r->state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->state = x;
	return x;
}

// Uniform in [0, bound). Uses the high bits via a 64-bit multiply rather than `%`:
// the low bits of xorshift are the weakest, and the ARM11 has no integer divide
// instruction, so a modulo here would be a library call in the generator's inner loop.
static inline uint32_t rngBelow(Rng* r, uint32_t bound)
{
	return (uint32_t)(((uint64_t)rngNext(r) * bound) >> 32);
}
