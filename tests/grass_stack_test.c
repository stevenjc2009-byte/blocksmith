// tests/grass_stack_test.c -- GRASS-STACK lane: does worldgenScatter()/worldgenFlora() ever
// place two plant-type blocks in VERTICALLY ADJACENT cells of the SAME column?
//
// steve's bug report, in his own words: "two grasses stacked on top of each other from
// different biomes -- get rid of that". Lives under tests/ rather than source/world/
// deliberately: the Makefile globs source/**/*.c into SOURCES (Makefile:26), so a _test.c
// placed there is compiled into the console build and its main() collides with the game's
// unless wrapped in #ifndef __3DS__. tests/ is outside that glob, so no guard is needed.
//
// ── What this actually checks ─────────────────────────────────────────────────────────────
//
// worldgenScatter() (tall grass, world/worldgen.c:954) and worldgenFlora() (cactus, dead
// bush, fern, and the per-biome flowers, world/worldgen.c:1114) both run once per column,
// in that fixed order, off the SAME tops[] the column's own terrain fill produced. Both
// gate every write on the target cell being BLOCK_AIR (worldgenScatter directly at
// worldgen.c:1031; worldgenFlora through cellsClear(), worldgen.c:1081), and there are two
// sanctioned two-cell writes, each from a SINGLE call site that writes both cells together
// only after cellsClear() has confirmed both are air: the v1.8.8 tall-grass clump
// (BLOCK_TALL_GRASS at y, BLOCK_TALL_GRASS_TOP at y+1) and the 1-2 block desert cactus
// (BLOCK_CACTUS at y and, when the height draw lands on GEN_CACTUS_MAX_H, again at y+1 --
// worldgen.c:1160-1168).
//
// If those guards hold, no column can ever carry two plant blocks in adjacent cells except
// those two sanctioned pairs -- REGARDLESS of which biome(s) contributed to the column, since
// BiomeId is a pure function of (x, z) for a fixed WorldGen and both passes ask it fresh.
// This test does not assume that reasoning; it walks real generated terrain, across a
// region wide enough (21 x 21 chunks, several periods of GEN_BIOME_SHIFT's 128-block field)
// to cross many biome boundaries, and asks the actual blocks.
//
// GREEN here does not mean steve's in-game observation was imagined -- it means this
// specific mechanism (same-column vertical double-placement between scatter and flora) is
// not the cause, and the report says so explicitly rather than papering over it.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "world/block.h"
#include "world/registry.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

static int g_checks, g_failed;

#define CHECK(cond) do {                                                     \
		g_checks++;                                                          \
		if (!(cond)) { g_failed++; printf("FAIL %s:%d  %s\n",                \
		                                  __FILE__, __LINE__, #cond); }      \
	} while (0)

// Every block id worldgenScatter()/worldgenFlora() can place. BLOCK_APPLE is deliberately
// excluded: it is tree fruit hanging under a canopy (worldgenDecorate), not the ground-level
// scatter/flora this report is about, and it is never adjacent to a ground plant by
// construction (it sits several blocks up, under the lowest canopy layer).
static bool isPlant(BlockId id)
{
	switch (id) {
	case BLOCK_TALL_GRASS:
	case BLOCK_TALL_GRASS_TOP:
	case BLOCK_CACTUS:
	case BLOCK_DEAD_BUSH:
	case BLOCK_FERN:
	case BLOCK_POPPY:
	case BLOCK_DAISY:
	case BLOCK_BLUEBELL:
	case BLOCK_ORCHID:
		return true;
	default:
		return false;
	}
}

// Walks every block column in the square of chunks generated around (cx0, cz0) and reports
// how many vertically-adjacent (y, y+1) pairs are BOTH plants and are NOT the sanctioned
// TALL_GRASS / TALL_GRASS_TOP clump. Prints the first few so a red run names real coordinates.
static int scanForStacks(World* w, int32_t cx0, int32_t cz0, int radius, uint32_t seed)
{
	int stacks = 0;
	const int32_t x0 = (cx0 - radius) * CHUNK_DIM;
	const int32_t x1 = (cx0 + radius) * CHUNK_DIM + CHUNK_DIM - 1;
	const int32_t z0 = (cz0 - radius) * CHUNK_DIM;
	const int32_t z1 = (cz0 + radius) * CHUNK_DIM + CHUNK_DIM - 1;

	for (int32_t z = z0; z <= z1; z++) {
		for (int32_t x = x0; x <= x1; x++) {
			// Plants never exist at or below GEN_SEA_LEVEL (both passes gate on it), so the
			// scan starts there rather than at 0 -- a bound stated by the generator itself,
			// not a shortcut this test invents.
			for (int y = GEN_SEA_LEVEL; y < WORLD_HEIGHT - 1; y++) {
				const BlockId a = worldGet(w, x, y, z);
				if (!isPlant(a))
					continue;
				const BlockId b = worldGet(w, x, y + 1, z);
				if (!isPlant(b))
					continue;
				if (a == BLOCK_TALL_GRASS && b == BLOCK_TALL_GRASS_TOP)
					continue;   // the sanctioned tall-grass clump
				if (a == BLOCK_CACTUS && b == BLOCK_CACTUS)
					continue;   // the sanctioned 2-block cactus

				stacks++;
				if (stacks <= 10)
					printf("  STACK seed %08x  (%d,%d,%d)  id %d over id %d\n",
					       seed, x, y, z, (int)b, (int)a);
			}
		}
	}
	return stacks;
}

int main(void)
{
	printf("grass-stack self-test (GRASS-STACK lane, steve's biome-boundary report)\n");

	registryInitCore();
	registryFreeze();

	// Five seeds, not one: a single seed could get unlucky and never place two biomes
	// anywhere near each other in the sampled square.
	static const uint32_t seeds[] = { 1337u, 90210u, 424242u, 7u, 99999u };
	// 21 x 21 chunks = 336 blocks per side, several periods of GEN_BIOME_SHIFT's 128-block
	// field on both the temperature-like and humidity fields, so the square is expected to
	// cross plains/forest/taiga/tundra/desert/jungle boundaries many times over, not once.
	const int radius = 10;

	int total_stacks = 0;
	for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		WorldGen g;
		if (!worldgenInit(&g, seeds[i], GEN_VERSION_NEWEST)) {
			printf("FATAL: worldgenInit refused GEN_VERSION_NEWEST for seed %08x\n", seeds[i]);
			return 2;
		}

		static World w;
		worldInit(&w);
		static WorldGenScratch s;
		worldgenScratchInit(&s);

		const int failed = worldgenArea(&g, &s, &w, 0, 0, radius);
		printf("  seed %08x  radius %d  failed columns %d\n", seeds[i], radius, failed);
		CHECK(failed == 0);

		const int stacks = scanForStacks(&w, 0, 0, radius, seeds[i]);
		printf("  seed %08x  vertically-adjacent unsanctioned plant pairs: %d\n",
		       seeds[i], stacks);
		total_stacks += stacks;

		worldExit(&w);
	}

	CHECK(total_stacks == 0);

	printf("%s (%d/%d checks, %d stacked pairs found across %zu seeds)\n",
	       g_failed ? "FAILED" : "PASS",
	       g_failed ? g_failed : g_checks, g_checks, total_stacks,
	       sizeof(seeds) / sizeof(seeds[0]));
	return g_failed ? 1 : 0;
}
