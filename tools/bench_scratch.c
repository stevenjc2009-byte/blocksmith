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
// Build and run (verified from WSL, repository root, 2026-08-30):
//   gcc -std=c11 -O2 -I source tools/bench_scratch.c tests/net_stub.c \
//       source/world/{block,chunk,world,scratch,noise,worldgen,worldgen_density,handbuilt,
//       registry,budget}.c -lm -o build-host/bench_scratch
//   ./build-host/bench_scratch
//
// The list above is wider than the original comment's, in three ways, each found by running
// the old command and reading the real error rather than guessing at one:
//   - tests/net_stub.c is in the link because world.c's worldSet() calls
//     networldOnColumnLoad() (net/networld.c), which needs a real socket/TLS stack this
//     bench has no business linking. Same stub, same reason, as tools/run_host_tests.sh and
//     source/world/Makefile.playerpose-test use it for.
//   - source/world/registry.c and source/world/budget.c are in the link because block.c and
//     world.c call into both (registryIsDefined/registryView/registryGet,
//     budgetClaim/budgetRelease) — they were never optional, the old command just never
//     tried to link and find out.
//   - source/world/worldgen_density.c is in the link because worldgen.c's column/height/
//     scatter paths (wgdColumn, wgdHeight, wgdColumnTops) live there, not in worldgen.c
//     itself.
// Without tests/net_stub.c, linking the old command fails with "undefined reference to
// `networldOnColumnLoad'"; with only that added, it fails again with a page of undefined
// references into registry.c/budget.c/worldgen_density.c. The old command also predates
// strict -std=c11 making clock_gettime()/CLOCK_MONOTONIC invisible without the
// _POSIX_C_SOURCE guard below — see that guard's own comment.
//
// Must be defined before ANY header is pulled in, same reason and same fix as
// source/net/bsnet_sock.c's file comment: glibc's feature-test macros latch on the first
// system header, not the first one that happens to need them, so clock_gettime()/
// CLOCK_MONOTONIC below are invisible under strict -std=c11 without this.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

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

// render_dist.h caps RENDER_DIST_MAX at 3, a 7x7 = 49 column area — that is the widest
// ring the game ever meshes today. RADIUS below is still 2 (a 5x5 = 25 column area, the
// cap before v1.6.0 task 12 raised it), so this bench no longer covers the shipped worst
// case; whether to raise it to 3 to match is a scope decision for whoever owns this file,
// not taken here. Benching a wide ring rather than one chunk keeps the form mix (UNIFORM
// deep rock, PALETTE4 surface, RAW where cave noise perforates) representative instead of
// whatever a hand-built chunk happens to be.
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
