// tests/worldgen_density_opt_test.c -- the terrain-identity pin for world/worldgen_density.c
// (v1.8.7).
//
// world/worldgen_density.c carries three optimisations that are only allowed to exist if the
// terrain they produce is UNCHANGED: the density-salt hoist out of densityCore, the
// uniform-cell short circuit in interpolateColumn, and the bitmask top scan in wgdColumn.
// Changing generated terrain is not a performance question on this project -- it needs a new
// GEN_VERSION and a coordinated server release -- so "faster and identical" and "faster" are
// different outcomes and this file is what tells them apart.
//
// ── What it pins ──────────────────────────────────────────────────────────────────────
//
// TERRAIN HASH. FNV-1a over every cell of every allocated chunk of 600 generated columns:
// 12 seeds x 25 column coordinates x both density generator versions. Not four fixtures --
// a fixture set that small is a sample and not a distribution, and this project has been
// caught by that before. The coordinates are deliberately mixed: a contiguous patch (so
// neighbouring columns' shared lattice edges are exercised), negative coordinates (the
// floor-shift path in dCoord and world/noise.c's split), and far-field coordinates out to
// +/-30,000 blocks (so the 64-bit octave position widening is exercised rather than assumed).
//
// The chunk-present flag is hashed alongside the cells on purpose. wgdColumn leaves an
// all-air chunk unallocated, and an "optimisation" that dropped a chunk which should have
// existed would otherwise hash the same as one that correctly skipped it.
//
// POINT-API HASH. wgdHeight and wgdDensityAt over 12 seeds x 400 positions x 19 heights.
// wgdColumn is not the only door into densityCore, and a salt change that the column pass
// happened not to expose would still move this.
//
// TOP-SCAN CROSS-CHECK. wgdColumnTops() against the generated blocks themselves, cell by
// cell, for a sample of columns. The bitmask walk that fills s_top resolves sixteen cells per
// row at once instead of walking the sky sixteen times; this is the check that says it
// resolved the same sixteen answers. The topmost solid block of a column is never carved --
// a cave is at least GEN_CAVE_MIN_DEPTH below it -- and the sea fill only ever writes into
// air, so "the first cell coming down that is not air, water or ice" is exactly s_top - 1.
//
// LERP INTERVAL PROPERTY. The uniform-cell short circuit is exact because lerpSh cannot leave
// the interval its two endpoints span. That is asserted directly, brute-forced over the two
// shift counts the generator actually uses and over corner magnitudes that reach the range
// s_grid really holds. This one duplicates the arithmetic rather than linking it, and is
// therefore a PROPERTY check and not a substitute for the terrain hash above -- it is here to
// say WHY the short circuit is exact, so that a future reader does not have to re-derive it.
//
// ── The measurement this was written for ──────────────────────────────────────────────
//
// Host only -- x86-64 gcc -O2 under WSL, linking this tree's world/noise.c. NOT measured on
// the ARM11, and nothing in this project has run on real 3DS hardware since v1.2.5.
// Alternating both arms in ONE process over 15 rounds (two separate runs of the same binary
// measured 0.4524 and 0.8915 ms/column for identical code, so separate runs prove nothing on
// a machine several agents share):
//
//   wgdColumn, 600 columns/round   ref 0.2767 s   new 0.2557 s   mean -7.6 %
//                                  (two earlier runs: -9.72 %, -9.69 %)
//
// Phase breakdown, same harness, 1800 columns:
//   interpolateColumn   0.0329 -> 0.0043 ms/col   (7.2 % -> 1.0 % of wgdColumn)
//   top scan            0.0071 -> 0.0026 ms/col   (1.6 % -> 0.6 %)
//   buildGrid           0.0612 -> 0.0633 ms/col   (unchanged; the salt hoist is BELOW this
//                                                  harness's noise floor, see below)
//
// The short circuit works because the column is mostly not near its own surface: measured
// 43.2 % of lattice cells all-air, 49.0 % all-solid, only 7.8 % mixed -- so 92.2 % of cells
// skip the 224 lerps entirely.
//
// **The salt hoist did not survive measurement as a timing win and is not quoted as one.**
// Timed in isolation on wgdHeight -- which this change touches in no other way -- 36,000
// queries per round, it read +3.01 % in one run and -1.67 % in another. It removes a counted
// 1,275 redundant rngMix per generated column, which is why it stays, but the honest figure
// is "inside the noise floor", not a percentage.
//
// ── Re-baselining ─────────────────────────────────────────────────────────────────────
//
// The two hashes below are REFERENCE DIGESTS in the sense tests/light_race_test.c uses the
// word: anything that changes what the density generator computes will move them, and that is
// the point. Move them deliberately, alongside a GEN_VERSION bump, or not at all.
#ifndef __3DS__

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "world/block.h"
#include "world/chunk.h"
#include "world/genversion.h"
#include "world/noise.h"
#include "world/registry.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_density.h"
#include "world/worldgen_scratch.h"

// The pinned digests. Measured 2026-09-02 on the tree as it stood BEFORE the three
// optimisations and again after them; both arms produce these.
#define PIN_TERRAIN  0x1a26d43b4b39eee3ULL
#define PIN_POINTAPI 0xc9dce475c6cb6830ULL

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			fprintf(stderr, "FAIL L%d %s\n", __LINE__, #cond);                  \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, #cond);  \
		}                                                                       \
	} while (0)

static World    s_w;
static WorldGen s_g;
static WorldGenScratch s_wgs;   // v1.8.7: the generator's buffers, now the caller's
static BlockId  s_cells[CHUNK_BLOCKS];

static const uint32_t SEEDS[] = {
	1337u, 4242u, 90210u, 7u, 555555u, 31337u,
	2u, 999983u, 123456789u, 40000u, 0xDEADBEEFu, 1u,
};
#define NSEEDS ((int)(sizeof SEEDS / sizeof SEEDS[0]))

typedef struct { int32_t cx, cz; } TestCol;
static const TestCol COLS[] = {
	{  0,  0}, {  1,  0}, {  0,  1}, {  1,  1}, {  2,  1},
	{ -1,  0}, {  0, -1}, { -1, -1}, { -2, -3}, { -1,  2},
	{ 10, 10}, { 11, 10}, { 10, 11}, {-10,-10}, {-11, -9},
	{100,  0}, {  0,100}, {100,100}, {-100,100}, {100,-100},
	{5000, 5000}, {-5000, 5000}, {5001, 5000}, {-30000, 20000}, {77, -4242},
};
#define NCOLS ((int)(sizeof COLS / sizeof COLS[0]))

static const uint32_t VERSIONS[] = { GEN_VERSION_DENSITY, GEN_VERSION_BIOME };
#define NVERS ((int)(sizeof VERSIONS / sizeof VERSIONS[0]))

#define MIX(h, b) do { (h) ^= (uint64_t)(uint8_t)(b); (h) *= 1099511628211ULL; } while (0)

static void hashColumn(uint64_t* h, const World* w, int32_t cx, int32_t cz)
{
	const Column* col = worldColumn(w, cx, cz);
	MIX(*h, col ? 1 : 0);
	if (!col) return;

	MIX(*h, cx); MIX(*h, cx >> 8); MIX(*h, cx >> 16); MIX(*h, cx >> 24);
	MIX(*h, cz); MIX(*h, cz >> 8); MIX(*h, cz >> 16); MIX(*h, cz >> 24);

	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		MIX(*h, cy);
		MIX(*h, col->chunks[cy] ? 1 : 0);
		if (!col->chunks[cy]) continue;
		chunkDecompressAll(col->chunks[cy], s_cells);
		for (int i = 0; i < CHUNK_BLOCKS; i++) MIX(*h, s_cells[i]);
	}
}

// ── The top-scan cross-check ──────────────────────────────────────────────────────────
//
// Run on the column that was JUST generated, because wgdColumnTops() describes the most
// recent one and refuses any other -- which is the contract, not a limitation to work around.
static void checkTops(const World* w, int32_t cx, int32_t cz)
{
	const int16_t* tops = wgdColumnTops(&s_wgs, cx, cz);
	CHECK(tops != NULL);
	if (!tops) return;

	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++) {
			int want = 0;
			for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
				const BlockId b = worldGet(w, cx * CHUNK_DIM + x, y, cz * CHUNK_DIM + z);
				if (b != BLOCK_AIR && b != BLOCK_WATER && b != BLOCK_ICE) { want = y + 1; break; }
			}
			// Counted as ONE check per column rather than 256, so the printed check total
			// stays readable; the coordinate of the first disagreement is what matters and it
			// is reported below.
			if (tops[z * CHUNK_DIM + x] != (int16_t)want) {
				s_fails++;
				fprintf(stderr, "FAIL L%d tops mismatch col(%d,%d) cell(%d,%d) got %d want %d\n",
				        __LINE__, (int)cx, (int)cz, x, z, (int)tops[z * CHUNK_DIM + x], want);
				if (!s_first[0])
					snprintf(s_first, sizeof(s_first), "L%d tops col(%d,%d) cell(%d,%d)",
					         __LINE__, (int)cx, (int)cz, x, z);
				return;
			}
		}
	s_checks++;
}

// ── The property the uniform-cell short circuit rests on ──────────────────────────────
//
// A copy of world/worldgen_density.c's lerpSh, deliberately: this asserts a property of the
// ARITHMETIC, and the terrain hash above is what asserts the file. If the two ever disagree
// about what lerpSh is, the terrain hash is the one that is right.
static int32_t lerpShRef(int32_t a, int32_t b, int t, int sh)
{
	return a + (((b - a) * t) >> sh);
}

static void testLerpInterval(void)
{
	// The two shift counts the generator uses, and corner magnitudes that reach past what
	// s_grid really holds (about +/- 8.4 M in 16.16 blocks).
	static const int SHIFTS[] = { GEN_D_CELL_XZ_SHIFT, GEN_D_CELL_Y_SHIFT };
	static const int32_t MAGS[] = { 1, 2, 3, 7, 8, 15, 100, 65535, 65536, 1000000, 8400000, 16000000 };
	const int nmag = (int)(sizeof MAGS / sizeof MAGS[0]);

	int pos_ok = 0, neg_ok = 0, cases = 0;

	for (int si = 0; si < 2; si++) {
		const int sh = SHIFTS[si];
		for (int ia = 0; ia < nmag; ia++)
			for (int ib = 0; ib < nmag; ib++)
				for (int sa = 0; sa < 2; sa++)
					for (int sb = 0; sb < 2; sb++) {
						const int32_t a = sa ? MAGS[ia] : -MAGS[ia];
						const int32_t b = sb ? MAGS[ib] : -MAGS[ib];
						for (int t = 0; t < (1 << sh); t++) {
							const int32_t v = lerpShRef(a, b, t, sh);
							cases++;
							// The two implications the short circuit uses, and nothing wider:
							// a mixed-sign pair is allowed to land anywhere.
							if (a > 0 && b > 0)   { if (v > 0)  pos_ok++; else goto bad_pos; }
							if (a <= 0 && b <= 0) { if (v <= 0) neg_ok++; else goto bad_neg; }
						}
					}
	}
	CHECK(cases > 0);
	CHECK(pos_ok > 0);
	CHECK(neg_ok > 0);
	return;

bad_pos:
	s_checks++; s_fails++;
	fprintf(stderr, "FAIL L%d lerpSh left the positive interval\n", __LINE__);
	if (!s_first[0]) snprintf(s_first, sizeof(s_first), "L%d lerpSh positive interval", __LINE__);
	return;
bad_neg:
	s_checks++; s_fails++;
	fprintf(stderr, "FAIL L%d lerpSh left the non-positive interval\n", __LINE__);
	if (!s_first[0]) snprintf(s_first, sizeof(s_first), "L%d lerpSh non-positive interval", __LINE__);
}

// ── The two sweeps ────────────────────────────────────────────────────────────────────

static uint64_t sweepTerrain(int* columns)
{
	uint64_t h = 1469598103934665603ULL;
	int      n = 0;

	for (int v = 0; v < NVERS; v++)
		for (int s = 0; s < NSEEDS; s++) {
			CHECK(worldgenInit(&s_g, SEEDS[s], VERSIONS[v]));
			for (int c = 0; c < NCOLS; c++) {
				worldInit(&s_w);
				const bool ok = wgdColumn(&s_g, &s_wgs, &s_w, COLS[c].cx, COLS[c].cz);
				if (!ok) {
					s_checks++; s_fails++;
					fprintf(stderr, "FAIL L%d wgdColumn refused seed %u col(%d,%d)\n",
					        __LINE__, SEEDS[s], (int)COLS[c].cx, (int)COLS[c].cz);
				}
				n++;
				MIX(h, ok ? 1 : 0);
				MIX(h, VERSIONS[v]);
				MIX(h, SEEDS[s]); MIX(h, SEEDS[s] >> 8);
				MIX(h, SEEDS[s] >> 16); MIX(h, SEEDS[s] >> 24);
				hashColumn(&h, &s_w, COLS[c].cx, COLS[c].cz);

				// The tops cross-check on a slice of the sample rather than all of it: it
				// reads 32,768 blocks back through worldGet per column and running it on all
				// 600 would dominate this binary's runtime for no extra coverage.
				if (ok && (c % 5) == 0 && (s % 4) == 0)
					checkTops(&s_w, COLS[c].cx, COLS[c].cz);

				worldExit(&s_w);
			}
		}

	*columns = n;
	return h;
}

static uint64_t sweepPointApi(void)
{
	uint64_t h = 1469598103934665603ULL;

	for (int s = 0; s < NSEEDS; s++) {
		CHECK(worldgenInit(&s_g, SEEDS[s], GEN_VERSION_BIOME));
		for (int i = 0; i < 400; i++) {
			const int32_t x = (int32_t)((i * 8191) % 4001) - 2000;
			const int32_t z = (int32_t)((i * 3571) % 4001) - 2000;
			const int     hgt = wgdHeight(&s_g, x, z);
			MIX(h, hgt); MIX(h, hgt >> 8);
			for (int y = 0; y < 128; y += 7) {
				const int32_t d = wgdDensityAt(&s_g, x, y, z);
				MIX(h, d); MIX(h, d >> 8); MIX(h, d >> 16); MIX(h, d >> 24);
			}
		}
	}
	return h;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== worldgen density identity test ==");

	registryInitCore();
	registryFreeze();

	testLerpInterval();

	int columns = 0;
	const uint64_t terrain = sweepTerrain(&columns);
	const uint64_t pointapi = sweepPointApi();

	printf("REFERENCE DIGEST terrain  %016llx  (%d columns, %d seeds, %d coords, %d versions)\n",
	       (unsigned long long)terrain, columns, NSEEDS, NCOLS, NVERS);
	printf("REFERENCE DIGEST pointapi %016llx  (%d seeds x 400 positions x 19 heights)\n",
	       (unsigned long long)pointapi, NSEEDS);

	CHECK(columns == NSEEDS * NCOLS * NVERS);
	CHECK(terrain  == PIN_TERRAIN);
	CHECK(pointapi == PIN_POINTAPI);

	if (s_fails) {
		printf("worldgen density identity self-test: FAILED - %d of %d checks (first: %s)\n",
		       s_fails, s_checks, s_first);
		return 1;
	}
	printf("worldgen density identity self-test: PASS %d checks\n", s_checks);
	return 0;
}

#endif
