// Host self-test for world/ore_gen.h/.c -- the v1.8.12 ore-vein generator (task brief
// "ORE-GEN": make the six ore blocks landed in block.h actually generate).
//
// Lives under source/world/, not tests/, because the task brief that requested it named this
// path as the example. The Makefile globs source/**/*.c into SOURCES (Makefile:26), so this
// file's main() would collide with source/main.c's on the console build unless the whole body
// is wrapped in #ifndef __3DS__ -- the same load-bearing guard source/world/cavewalk_test.c
// documents at its own top, and the reason files under tests/ (e.g. tests/cave_carve_test.c,
// this file's closest sibling) don't need one.
//
// WIRED into tools/run_host_tests.sh, at :5088-5141 -- the stanza links source/world/ore_gen.c
// (:5134) alongside worldgen_density.c and runs this suite's own binary. It was NOT wired when
// this file was written: the authoring lane (ORE-GEN) was permission-blocked from editing that
// script -- the same restriction tests/cave_carve_test.c's own header notes for its lane -- and
// handed the exact stanza over in its final report instead. The stanza's own comment records
// that handover, and records that the more useful half of it was the eight link-line insertions
// elsewhere in the script, which is what took the suite from dark back to green.
//
// ── What each family proves, and how it was proven able to go RED (see the final report for
//    the actual sabotage/restore transcript and md5s) ──────────────────────────────────────
//
//   testCrossBoundaryDeterminism   A region shared between two columns' scan neighbourhoods
//                                  produces the SAME ore mask regardless of which column asks,
//                                  what order columns are built in, or whether the scratch is
//                                  fresh or reused -- world/ore_gen.h's whole determinism claim,
//                                  the same property tests/cave_carve_test.c's
//                                  testIsolationVsLargerArea / testOrderIndependence pin for
//                                  the cave carver, adapted here.
//
//   testRadiusSufficiency          ORE_NEIGHBOURHOOD_R already finds everything a wider scan
//                                  would; radius 0 provably finds LESS (the non-vacuous
//                                  control). Same shape as cave_carve_test.c's own.
//
//   testStoneOnlyAndCensus         The single strongest check in this file: every cell where an
//                                  ORES-version world differs from the SAME seed's CAVES-version
//                                  world must (a) hold one of the six ore ids on the ORES side
//                                  and (b) hold BLOCK_STONE on the CAVES side. Run through the
//                                  REAL wgdColumn() pipeline (not just the mask builder), over a
//                                  wide sweep of columns, decompressing every chunk of both
//                                  worlds and comparing cell-for-cell -- so this is not "ore
//                                  never appears on air", it is "ore changes NOTHING else about
//                                  the terrain, anywhere, ever". It also tallies the real
//                                  Y-level census the report requires, and checks every ore's
//                                  observed Y range against ore_gen.c's own table (hand-copied
//                                  below, the same duplication tests/worldgen_density_opt_test.c
//                                  accepts for lerpShRef, for the same reason: this is a
//                                  PROPERTY check on the band, not a substitute for reading
//                                  ore_gen.c itself).
//
//   testPerf                       Host-only wall-clock, CAVES vs ORES, same columns, min-of-N
//                                  rounds -- a diagnostic, not a pass/fail gate (the ARM11 is
//                                  the real budget and this binary never runs on it). Printed,
//                                  not asserted, exactly as tests/worldgen_density_opt_test.c
//                                  prints its own timing notes without gating on them.
#ifndef __3DS__

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "world/block.h"
#include "world/chunk.h"
#include "world/genversion.h"
#include "world/ore_gen.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_density.h"
#include "world/worldgen_scratch.h"

static int g_checks, g_failed;

#define CHECK(cond) do {                                                     \
		g_checks++;                                                          \
		if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s\n",                \
		                                  __FILE__, __LINE__, #cond); }      \
	} while (0)

// ── Ore mask digest / popcount (world/ore_gen.h's three bit-planes) ─────────────────────────

static uint32_t digestOreMask(const WorldGenScratch* s)
{
	uint32_t h = 2166136261u;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++) {
			const uint16_t planes[3] = { s->ore_bit0[y][z], s->ore_bit1[y][z], s->ore_bit2[y][z] };
			for (int p = 0; p < 3; p++) {
				h = (h ^ (uint32_t)(planes[p] & 0xFFu)) * 16777619u;
				h = (h ^ (uint32_t)(planes[p] >> 8)) * 16777619u;
			}
		}
	return h;
}

static int popcountOreMask(const WorldGenScratch* s)
{
	int n = 0;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++) {
			uint16_t any = (uint16_t)(s->ore_bit0[y][z] | s->ore_bit1[y][z] | s->ore_bit2[y][z]);
			while (any) { n += (int)(any & 1u); any = (uint16_t)(any >> 1); }
		}
	return n;
}

#define SWEEP_SEEDS 8
static const uint32_t k_seeds[SWEEP_SEEDS] = {
	1u, 1337u, 4242u, 90210u, 0x5EEDCAFEu, 0xDEADBEEFu, 0xC001D00Du, 777u,
};

// ── testCrossBoundaryDeterminism ─────────────────────────────────────────────────────────────

static void testCrossBoundaryDeterminism(void)
{
	puts("\n-- cross-chunk-boundary determinism: isolation vs. as-part-of-a-larger-area --");

	int total = 0, mismatches = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		CHECK(worldgenInit(&g, k_seeds[si], GEN_VERSION_ORES));

		for (int32_t cz = -2; cz <= 2; cz++) {
			for (int32_t cx = -2; cx <= 2; cx++) {
				WorldGenScratch iso;
				worldgenScratchInit(&iso);
				oreGenBuildMask(&g, &iso, cx, cz);
				const uint32_t d_iso = digestOreMask(&iso);

				WorldGenScratch shared;
				worldgenScratchInit(&shared);
				for (int32_t z2 = cz - 3; z2 <= cz + 3; z2++)
					for (int32_t x2 = cx - 3; x2 <= cx + 3; x2++) {
						if (x2 == cx && z2 == cz) continue;
						oreGenBuildMask(&g, &shared, x2, z2);
					}
				oreGenBuildMask(&g, &shared, cx, cz);
				const uint32_t d_shared = digestOreMask(&shared);

				total++;
				if (d_iso != d_shared) mismatches++;
				CHECK(d_iso == d_shared);
			}
		}
	}
	printf("  isolation-vs-larger-area   %d/%d columns agreed (seeds: %d)\n",
	       total - mismatches, total, SWEEP_SEEDS);

	// Order independence: forward, reverse, and a fixed shuffle of the same 25-column block
	// must all agree per column, on one shared reused scratch each time.
	WorldGen g;
	CHECK(worldgenInit(&g, 0xA5A5A5A5u, GEN_VERSION_ORES));

	enum { N = 25 };
	int32_t cxs[N], czs[N];
	int k = 0;
	for (int32_t z = -2; z <= 2; z++)
		for (int32_t x = -2; x <= 2; x++) { cxs[k] = x; czs[k] = z; k++; }

	uint32_t forward[N], reverse[N], shuffled[N];

	WorldGenScratch s;
	worldgenScratchInit(&s);
	for (int i = 0; i < N; i++) {
		oreGenBuildMask(&g, &s, cxs[i], czs[i]);
		forward[i] = digestOreMask(&s);
	}
	worldgenScratchInit(&s);
	for (int i = N - 1; i >= 0; i--) {
		oreGenBuildMask(&g, &s, cxs[i], czs[i]);
		reverse[i] = digestOreMask(&s);
	}
	int order[N];
	for (int i = 0; i < N; i++) order[(i * 7) % N] = i;
	worldgenScratchInit(&s);
	for (int oi = 0; oi < N; oi++) {
		const int i = order[oi];
		oreGenBuildMask(&g, &s, cxs[i], czs[i]);
		shuffled[i] = digestOreMask(&s);
	}

	int wrong = 0;
	for (int i = 0; i < N; i++) {
		if (forward[i] != reverse[i]) wrong++;
		if (forward[i] != shuffled[i]) wrong++;
		CHECK(forward[i] == reverse[i]);
		CHECK(forward[i] == shuffled[i]);
	}
	printf("  order independence   %d columns, %d mismatches (fwd/rev/shuffled)\n", N, wrong);
}

// ── testRadiusSufficiency ────────────────────────────────────────────────────────────────────

static void testRadiusSufficiency(void)
{
	puts("\n-- ORE_NEIGHBOURHOOD_R already finds everything a wider scan would --");

	int checked = 0, disagreements = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		CHECK(worldgenInit(&g, k_seeds[si], GEN_VERSION_ORES));
		for (int32_t cz = -1; cz <= 1; cz++) {
			for (int32_t cx = -1; cx <= 1; cx++) {
				WorldGenScratch a, b;
				worldgenScratchInit(&a);
				worldgenScratchInit(&b);
				oreGenBuildMaskR(&g, &a, cx, cz, ORE_NEIGHBOURHOOD_R);
				oreGenBuildMaskR(&g, &b, cx, cz, ORE_NEIGHBOURHOOD_R + 2);
				checked++;
				if (digestOreMask(&a) != digestOreMask(&b)) disagreements++;
				CHECK(digestOreMask(&a) == digestOreMask(&b));
			}
		}
	}
	printf("  radius sufficiency   R vs R+2: %d/%d columns agreed\n",
	       checked - disagreements, checked);

	// Non-vacuous control: radius 0 must find strictly less than the real R somewhere in the
	// sweep, or the comparison above is comparing two accidentally-equal numbers.
	int radius0_found_less = 0;
	for (int si = 0; si < SWEEP_SEEDS; si++) {
		WorldGen g;
		worldgenInit(&g, k_seeds[si], GEN_VERSION_ORES);
		for (int32_t cz = -1; cz <= 1; cz++)
			for (int32_t cx = -1; cx <= 1; cx++) {
				WorldGenScratch a, b;
				worldgenScratchInit(&a);
				worldgenScratchInit(&b);
				oreGenBuildMaskR(&g, &a, cx, cz, 0);
				oreGenBuildMaskR(&g, &b, cx, cz, ORE_NEIGHBOURHOOD_R);
				if (popcountOreMask(&a) < popcountOreMask(&b)) radius0_found_less++;
			}
	}
	printf("  radius=0 undercounts vs R in %d/%d columns (sanity: proves the comparison above "
	       "is not vacuous)\n", radius0_found_less, SWEEP_SEEDS * 9);
	CHECK(radius0_found_less > 0);
}

// ── testStoneOnlyAndCensus ───────────────────────────────────────────────────────────────────

// Hand-copied from ore_gen.c's s_ore_table -- see this file's header comment for why a copy is
// acceptable here (a PROPERTY check on the band, not a substitute for reading ore_gen.c).
//
// y_min/y_max are the ANCHOR band -- where oreGenAttempt() draws an attempt's starting point --
// not a hard ceiling on every stamped cell. A walk is free to move in y exactly like x/z, one
// block per step, so a vein whose anchor lands at the very edge of its band can still carry up
// to (size_max - 1) cells past that edge before the walk's own step budget runs out -- the same
// way vanilla's own blob generator does not hard-clamp a blob to the Y value its center was
// drawn at. slack[] is that worst case, TAKEN DIRECTLY from ore_gen.c's size_max column, so this
// check still catches a real violation (e.g. the version gate failing open, or a band constant
// drifting by an order of magnitude) while not flagging the walk doing exactly what it is
// documented to do. First measured here 2026-09-03: iron reached y=67 against a stated band of
// 0-63 (a +4 overshoot, size_max 9 allows up to +8) and similar single-digit overshoots on gold,
// redstone and diamond -- all within this bound, none anywhere close to it.
typedef struct { BlockId id; int16_t y_min, y_max; uint8_t slack; const char* name; } OreBound;
static const OreBound k_bounds[6] = {
	{ BLOCK_COAL_ORE,     0, 127, 16, "coal"     },   // size_max 17
	{ BLOCK_IRON_ORE,     0,  63,  8, "iron"     },   // size_max  9
	{ BLOCK_GOLD_ORE,     0,  31,  8, "gold"     },   // size_max  9
	{ BLOCK_REDSTONE_ORE, 0,  15,  7, "redstone" },   // size_max  8
	{ BLOCK_LAPIS_ORE,    0,  31,  6, "lapis"    },   // size_max  7
	{ BLOCK_DIAMOND_ORE,  0,  15,  7, "diamond"  },   // size_max  8
};

static int16_t boundLow(int i)  { const int v = (int)k_bounds[i].y_min - k_bounds[i].slack;
                                   return (int16_t)(v < 0 ? 0 : v); }
static int16_t boundHigh(int i) { const int v = (int)k_bounds[i].y_max + k_bounds[i].slack;
                                   return (int16_t)(v > WORLD_HEIGHT - 1 ? WORLD_HEIGHT - 1 : v); }

static int oreKindIndex(BlockId b)
{
	for (int i = 0; i < 6; i++)
		if (k_bounds[i].id == b) return i;
	return -1;
}

static World s_w_ores;
static World s_w_caves;
static WorldGenScratch s_scratch_ores;
static WorldGenScratch s_scratch_caves;

static void testStoneOnlyAndCensus(void)
{
	puts("\n-- stone-only rule (real pipeline) + Y-level census --");

	long total_cells = 0, diffs = 0, ore_cells = 0;
	long count[6] = { 0 };
	int16_t ymin[6], ymax[6];
	for (int i = 0; i < 6; i++) { ymin[i] = 32767; ymax[i] = -32768; }

	#define CENSUS_SEEDS 4
	static const uint32_t census_seeds[CENSUS_SEEDS] = { 1u, 4242u, 0xDEADBEEFu, 999983u };

	for (int si = 0; si < CENSUS_SEEDS; si++) {
		WorldGen g_ores, g_caves;
		CHECK(worldgenInit(&g_ores,  census_seeds[si], GEN_VERSION_ORES));
		CHECK(worldgenInit(&g_caves, census_seeds[si], GEN_VERSION_CAVES));

		for (int32_t cz = -4; cz <= 4; cz++) {
			for (int32_t cx = -4; cx <= 4; cx++) {
				worldInit(&s_w_ores);
				worldInit(&s_w_caves);
				const bool ok_o = wgdColumn(&g_ores,  &s_scratch_ores,  &s_w_ores,  cx, cz);
				const bool ok_c = wgdColumn(&g_caves, &s_scratch_caves, &s_w_caves, cx, cz);
				CHECK(ok_o); CHECK(ok_c);

				const Column* co = worldColumn(&s_w_ores,  cx, cz);
				const Column* cc = worldColumn(&s_w_caves, cx, cz);

				for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
					BlockId bo[CHUNK_BLOCKS], bc[CHUNK_BLOCKS];
					const Chunk* cho = co ? co->chunks[cy] : NULL;
					const Chunk* chc = cc ? cc->chunks[cy] : NULL;
					if (cho) chunkDecompressAll(cho, bo); else memset(bo, BLOCK_AIR, sizeof bo);
					if (chc) chunkDecompressAll(chc, bc); else memset(bc, BLOCK_AIR, sizeof bc);

					for (int i = 0; i < CHUNK_BLOCKS; i++) {
						total_cells++;
						if (bo[i] == bc[i]) continue;
						diffs++;
						const int kind = oreKindIndex(bo[i]);
						CHECK(kind >= 0);          // any difference must be an ore substitution
						CHECK(bc[i] == BLOCK_STONE);  // and only ever a replacement of STONE
						if (kind < 0) continue;

						ore_cells++;
						count[kind]++;
						const int ly = i / (CHUNK_DIM * CHUNK_DIM);
						const int y  = cy * CHUNK_DIM + ly;
						if ((int16_t)y < ymin[kind]) ymin[kind] = (int16_t)y;
						if ((int16_t)y > ymax[kind]) ymax[kind] = (int16_t)y;
					}
				}

				worldExit(&s_w_ores);
				worldExit(&s_w_caves);
			}
		}
	}

	const int columns = CENSUS_SEEDS * 9 * 9;
	printf("  swept %d columns (%d seeds x 9x9), %ld cells compared, %ld differed, %ld were ore\n",
	       columns, CENSUS_SEEDS, total_cells, diffs, ore_cells);
	CHECK(total_cells > 0);
	CHECK(ore_cells > 0);
	CHECK(ore_cells == diffs);   // every diff was accounted for above; restated as one summary
	                              // check so a future reader sees the headline property in one line

	puts("  -- Y-level census (actual counts, this sweep) --");
	for (int i = 0; i < 6; i++) {
		if (count[i] > 0)
			printf("  %-9s count=%-8ld y=[%d..%d]  anchor band=[%d..%d]  checked=[%d..%d]\n",
			       k_bounds[i].name, count[i], ymin[i], ymax[i],
			       k_bounds[i].y_min, k_bounds[i].y_max, boundLow(i), boundHigh(i));
		else
			printf("  %-9s count=0 (none observed this sweep)\n", k_bounds[i].name);
		CHECK(count[i] > 0);   // all six kinds should show up over 324 columns
		if (count[i] > 0) {
			// Checked against [anchor band +/- slack], not the bare anchor band -- see
			// k_bounds's own comment for why the walk is allowed that much overshoot and no
			// more.
			CHECK(ymin[i] >= boundLow(i));
			CHECK(ymax[i] <= boundHigh(i));
		}
	}

	// coal must be by far the most common and diamond among the rarest, per the design intent
	// in ore_gen.h's file comment ("coal common/high ... diamond very low/rare").
	CHECK(count[0] > count[5]);   // coal > diamond
	CHECK(count[0] > count[4]);   // coal > lapis
}

// ── testVersionGate ───────────────────────────────────────────────────────────────────────────

static int countOreCells(uint32_t version, int32_t cx0, int32_t cx1, int32_t cz0, int32_t cz1,
                          const uint32_t* seeds, int nseeds)
{
	int found = 0;
	for (int si = 0; si < nseeds; si++) {
		WorldGen g;
		worldgenInit(&g, seeds[si], version);
		for (int32_t cz = cz0; cz <= cz1; cz++)
			for (int32_t cx = cx0; cx <= cx1; cx++) {
				World w;
				WorldGenScratch s;
				worldInit(&w);
				worldgenScratchInit(&s);
				wgdColumn(&g, &s, &w, cx, cz);
				const Column* c = worldColumn(&w, cx, cz);
				for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
					if (!c || !c->chunks[cy]) continue;
					BlockId cells[CHUNK_BLOCKS];
					chunkDecompressAll(c->chunks[cy], cells);
					for (int i = 0; i < CHUNK_BLOCKS; i++)
						if (oreKindIndex(cells[i]) >= 0) found++;
				}
				worldExit(&w);
			}
	}
	return found;
}

// Proves the version gate both ways, IN THIS BINARY: a CAVES-and-below world never places an
// ore block anywhere (the negative side), and the identical seeds/columns at GEN_VERSION_ORES
// DO (the positive control, so the negative side above is not vacuously true -- e.g. from a
// build that can never place ore at all). This is the within-process half of the version-gate
// proof; the final report's separate byte-for-byte A/B against a pre-edit baseline is the other
// half (this test alone cannot rule out a change that alters CAVES-version terrain WITHOUT ever
// placing a literal ore id -- unlikely given the write site's own structure, but the A/B is what
// actually rules it out).
static void testVersionGate(void)
{
	puts("\n-- version gate: CAVES-and-below worlds never place ore --");

	static const uint32_t seeds[3] = { 1u, 4242u, 0xDEADBEEFu };

	const int at_caves = countOreCells(GEN_VERSION_CAVES, -1, 1, -1, 1, seeds, 3);
	printf("  27 columns x 3 seeds at GEN_VERSION_CAVES: %d ore cells found\n", at_caves);
	CHECK(at_caves == 0);

	const int at_ores = countOreCells(GEN_VERSION_ORES, -1, 1, -1, 1, seeds, 3);
	printf("  control: the same columns/seeds at GEN_VERSION_ORES: %d ore cells found\n", at_ores);
	CHECK(at_ores > 0);
}

// ── testPerf (diagnostic, not a gate) ───────────────────────────────────────────────────────

static double nowMs(void)
{
	return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void testPerf(void)
{
	puts("\n-- perf: CAVES vs ORES, same columns, min-of-3 rounds (host x86-64, NOT the ARM11) --");

	enum { PCOLS = 20, ROUNDS = 3 };
	int32_t cxs[PCOLS], czs[PCOLS];
	int k = 0;
	for (int32_t z = 0; z < 4 && k < PCOLS; z++)
		for (int32_t x = 0; x < 5 && k < PCOLS; x++) { cxs[k] = x; czs[k] = z; k++; }

	double best_caves = 1e18, best_ores = 1e18;
	for (int r = 0; r < ROUNDS; r++) {
		WorldGen g;
		WorldGenScratch s;
		World w;

		worldgenInit(&g, 12345u, GEN_VERSION_CAVES);
		worldgenScratchInit(&s);
		double t0 = nowMs();
		for (int i = 0; i < PCOLS; i++) {
			worldInit(&w);
			wgdColumn(&g, &s, &w, cxs[i], czs[i]);
			worldExit(&w);
		}
		double t1 = nowMs();
		if (t1 - t0 < best_caves) best_caves = t1 - t0;

		worldgenInit(&g, 12345u, GEN_VERSION_ORES);
		worldgenScratchInit(&s);
		double t2 = nowMs();
		for (int i = 0; i < PCOLS; i++) {
			worldInit(&w);
			wgdColumn(&g, &s, &w, cxs[i], czs[i]);
			worldExit(&w);
		}
		double t3 = nowMs();
		if (t3 - t2 < best_ores) best_ores = t3 - t2;
	}

	const double us_caves = best_caves * 1000.0 / PCOLS;
	const double us_ores  = best_ores  * 1000.0 / PCOLS;
	printf("  CAVES: %.4f ms / %d columns = %.2f us/column (best of %d)\n",
	       best_caves, PCOLS, us_caves, ROUNDS);
	printf("  ORES:  %.4f ms / %d columns = %.2f us/column (best of %d)\n",
	       best_ores, PCOLS, us_ores, ROUNDS);
	if (us_caves > 0.0)
		printf("  delta: %.2f us/column, %.1f%% of the CAVES figure\n",
		       us_ores - us_caves, 100.0 * (us_ores - us_caves) / us_caves);
	puts("  (host x86-64, gcc -O1 -- a ratio, not a console microsecond figure; see the final "
	     "report for how this maps onto the ARM11 budget)");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== ore_gen test (source/world/ore_gen.h/.c) ==");

	testCrossBoundaryDeterminism();
	testRadiusSufficiency();
	testStoneOnlyAndCensus();
	testVersionGate();
	testPerf();

	printf("\n%s %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
