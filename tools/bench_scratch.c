// Host microbenchmark for scratchFill(), the per-chunk neighbourhood gather the mesher runs
// before every chunk build.
//
// Why this exists: step 9.2a made Chunk opaque and routed the gather through
// chunkCopyRun(), which cost a measured 145 -> 292 us per chunk. That figure came off the
// console, where the only way to get it is a build, an emulator boot and a screen read —
// too slow a loop to optimise against. Everything scratchFill touches (world.c, chunk.c,
// worldgen.c, scratch.c) is plain C with no <3ds.h>, exactly the property
// tools/run_host_tests.sh already relies on, so the same code can be timed here in a
// second instead.
//
// This is NOT a test and deliberately does not live under source/: every .c file under
// source/ is globbed into the console build by the Makefile, and a second main() would
// break that link.
//
// Build and run:
//   gcc -std=c11 -O2 -I source tools/bench_scratch.c source/world/{block,chunk,world,
//       scratch,noise,worldgen,handbuilt}.c -lm -o build-host/bench_scratch
//   ./build-host/bench_scratch
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "world/chunk.h"
#include "world/scratch.h"
#include "world/world.h"
#include "world/worldgen.h"

static World       s_world;
static WorldGen    s_gen;
static MeshScratch s_scratch;

// The widest ring the game ever meshes: render_dist.h caps RENDER_DIST_MAX at 2, a 5x5
// column area. Benching the shipped worst case rather than one chunk keeps the form mix
// (UNIFORM deep rock, PALETTE4 surface, RAW where cave noise perforates) representative
// instead of whatever a hand-built chunk happens to be.
#define RADIUS 2

static double nowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
	worldInit(&s_world);
	worldgenInit(&s_gen, 1337u, GEN_VERSION_LEGACY);

	const int cols = worldgenArea(&s_gen, &s_world, 0, 0, RADIUS);
	printf("generated %d columns, %d chunks\n", cols, s_world.chunks);

	// Count the form mix, so a change in timing can be read against a change in what is
	// actually being copied rather than guessed at.
	int n_uniform = 0, n_pal = 0, n_raw = 0, total = 0;
	for (int cx = -RADIUS; cx <= RADIUS; cx++)
		for (int cz = -RADIUS; cz <= RADIUS; cz++)
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
				const Chunk* c = worldChunk(&s_world, cx, cy, cz);
				if (!c) continue;
				total++;
				switch (chunkGetForm(c)) {
				case CHUNK_FORM_UNIFORM:  n_uniform++; break;
				case CHUNK_FORM_PALETTE4: n_pal++;     break;
				case CHUNK_FORM_RAW:      n_raw++;     break;
				}
			}
	printf("forms: uniform %d  palette4 %d  raw %d  (total %d)\n",
	       n_uniform, n_pal, n_raw, total);

	// Warm the caches so the first pass is not the one being reported.
	for (int cx = -RADIUS; cx <= RADIUS; cx++)
		for (int cz = -RADIUS; cz <= RADIUS; cz++)
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
				if (worldChunk(&s_world, cx, cy, cz))
					scratchFill(&s_scratch, &s_world, cx, cy, cz);

	// Three independent passes rather than one: a single number cannot show its own noise
	// floor, and a claimed improvement smaller than the spread between passes is not one.
	const int PASSES = 3;
	const int REPS   = 20;
	for (int pass = 0; pass < PASSES; pass++) {
		const double t0 = nowSeconds();
		long fills = 0;
		for (int r = 0; r < REPS; r++)
			for (int cx = -RADIUS; cx <= RADIUS; cx++)
				for (int cz = -RADIUS; cz <= RADIUS; cz++)
					for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
						if (!worldChunk(&s_world, cx, cy, cz)) continue;
						scratchFill(&s_scratch, &s_world, cx, cy, cz);
						fills++;
					}
		const double us = (nowSeconds() - t0) * 1e6 / (double)fills;
		printf("pass %d: %ld fills, %.3f us per scratchFill\n", pass, fills, us);
	}

	worldExit(&s_world);
	return 0;
}
