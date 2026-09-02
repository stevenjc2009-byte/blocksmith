// v1.8.7. The generator's per-lane scratch, spelled out.
//
// **This header exists so that world/worldgen.c and world/worldgen_density.c can hold their
// working buffers somewhere a CALLER owns.** Both files used to keep them in file statics, and
// world/worldgen.h's note on WorldGenScratch has the measurement that made that untenable:
// two threads on disjoint columns produced 24 of 32 columns wrong and 159 of 192 generations
// refused outright. Nothing about the terrain changes here — the same buffers hold the same
// bytes in the same order — only who owns them.
//
// **Why it is not in world/worldgen.h.** The struct is sized by the density generator's grid
// geometry (GEN_D_GRID_XZ, GEN_D_GRID_Y), which lives in world/worldgen_density.h; that header
// includes world/worldgen.h, so the definition cannot go the other way without a cycle.
// world/worldgen.h therefore carries only `typedef struct WorldGenScratch WorldGenScratch;`
// and the prototypes, which is all a caller needs to PASS one. A caller that needs to DECLARE
// one includes this file.
//
// **Who includes it.** world/worldgen.c and world/worldgen_density.c, because they read the
// fields; and every lane owner — app/worker.c, the tools and the tests — because they provide
// the storage. Nothing else should need it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk.h"
#include "world/noise.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_density.h"

// ── The cave field's per-column corner cache ──────────────────────────────────────────
//
// Moved out of world/worldgen.c by the destaticising change and otherwise untouched. That
// file's block comment on caveCacheBuild() is still where the scheme is explained: the cave
// octaves' x and z lattice indices are constant across a 16-block column, so 208 rngHash3
// calls and 96 smooths answer up to 32,768 cave tests.
#define CAVE_SALTS 2

// How many y lattice rows a whole column spans, per octave, plus the row above the top one
// that the interpolation always reads. Octave i samples the lattice at y >> (GEN_CAVE_SHIFT_Y
// - i), so the LAST octave is the finest and needs the most rows; the table is sized for it
// and the coarser octaves use a prefix of it.
#define CAVE_ROWS_AT(o)  ((((WORLD_HEIGHT) - 1) >> (GEN_CAVE_SHIFT_Y - (o))) + 2)
#define CAVE_ROWS        CAVE_ROWS_AT(GEN_CAVE_OCTAVES - 1)

typedef struct {
	bool     valid;
	uint32_t seed;         // which WorldGen's field this holds
	int32_t  bx, bz;       // the column's -x / -z block corner

	// The lattice corners: [salt][octave][y row][dz * 2 + dx]. x and z are the column's own
	// constant indices, so only the two y rows the interpolation straddles have to be looked
	// up per block.
	fx corner[CAVE_SALTS][GEN_CAVE_OCTAVES][CAVE_ROWS][4];

	// smooth() of the lattice fraction, per octave. The x and z tables are indexed by the
	// block's offset INTO THIS COLUMN, the y one by the low bits of the block y — the
	// fraction repeats every 1 << GEN_CAVE_SHIFT_Y blocks and that is at most CHUNK_DIM.
	fx sx[GEN_CAVE_OCTAVES][CHUNK_DIM];
	fx sy[GEN_CAVE_OCTAVES][CHUNK_DIM];
	fx sz[GEN_CAVE_OCTAVES][CHUNK_DIM];
} WorldGenCaveCache;

// ── The lane's scratch ────────────────────────────────────────────────────────────────
//
// Grouped by the file that reads each half, because the two halves are independent and a
// reader tracking one of them should not have to step over the other. Sizes are the ARM ones
// (arm-none-eabi-gcc -O3, armv6k), and they are quoted because the total is the number a
// second lane costs.
//
// None of it is on the linear heap and none of it is claimed against WORLD_BUDGET_BYTES
// (world/budget.h): that budget counts block storage, and nothing here outlives the call that
// fills it. What it IS is 16 KB of a lane owner's static storage — see world/worldgen.h.
struct WorldGenScratch {
	// ── world/worldgen.c ──────────────────────────────────────────────────────────────

	// One chunk's cells for the LEGACY fill, staged and committed in one worldSetChunkAll —
	// never CHUNK_BLOCKS individual worldSet calls, which would promote a freshly generated
	// chunk's storage cell by cell (UNIFORM -> PALETTE4 -> RAW) for a chunk whose final
	// content is already fully known before the first write.        4,096 B
	BlockId gen_flat[CHUNK_BLOCKS];

	// The cave field's corner cache.                                1,488 B
	WorldGenCaveCache cave;

	// ── world/worldgen_density.c ──────────────────────────────────────────────────────

	// The coarse lattice: [y][z][x], 16.16 blocks, positive inside the ground.  1,700 B
	int32_t grid[GEN_D_GRID_Y][GEN_D_GRID_XZ][GEN_D_GRID_XZ];

	// The interpolated result, one bit per block: [y][z] holds a 16-bit mask over x. 4,096
	// bytes for a whole 16 x 16 x 128 column, against 32,768 for a byte per block — and the
	// surface pass wants to scan downwards through a column, which a bitmask makes a shift
	// rather than a strided memory walk.                            4,096 B
	uint16_t solid[WORLD_HEIGHT][CHUNK_DIM];

	// Topmost solid y + 1 per (x, z), i.e. exactly worldgenHeight()'s convention. 0 where the
	// whole column came out empty, which the vertical bias makes impossible but which the fill
	// loop must still not read past the end of.                       512 B
	int16_t top[CHUNK_DIM][CHUNK_DIM];

	// Slope at each (x, z) in blocks-per-block doubled — see GEN_D_CLIFF_SLOPE_X2.   256 B
	uint8_t slope[CHUNK_DIM][CHUNK_DIM];

	// One chunk's cells for the DENSITY fill. Same buffer discipline and the same reason as
	// gen_flat above, kept separate from it only because the two generators must not be able
	// to interfere with each other.                                 4,096 B
	BlockId flat[CHUNK_BLOCKS];

	// Which column `top` currently describes, for wgdColumnTops(). -1/-1 would be a real
	// column coordinate, so validity is its own flag rather than a sentinel pair.      9 B
	int32_t top_cx, top_cz;
	bool    top_valid;
};
