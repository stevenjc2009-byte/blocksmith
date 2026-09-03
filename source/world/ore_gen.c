#include "world/ore_gen.h"

#include <string.h>

#include "world/rng.h"

// Same discipline world/cave_carve.c documents at its own :7-13: no divide by a runtime
// variable anywhere in this file's per-block hot path (the ARM11 has no integer divide
// instruction). rngBelow() (rng.h) is a 64-bit multiply + shift, not a divide, and is the only
// randomness primitive this file uses -- so there are zero runtime divides anywhere below,
// stricter than cave_carve.c even needs to be (it allows itself one divide per walk for its
// taper angle; this walk has no taper, so it needs none at all).

_Static_assert(ORE_NEIGHBOURHOOD_R ==
               ((ORE_MAX_REACH + CHUNK_DIM + CHUNK_DIM - 1) / CHUNK_DIM),
               "ORE_NEIGHBOURHOOD_R must match R = ceil((ORE_MAX_REACH + CHUNK_DIM) / "
               "CHUNK_DIM), or an attempt could walk past the edge of the scanned "
               "neighbourhood and be missed");

// ── Per-ore table (see ore_gen.h's file comment for the researched Y-bands and provenance) ──

#define ORE_KIND_COUNT 6
// Headroom over the largest `attempts` below (coal, 20): folds (kind, attempt) into one
// combined index for the Rng seed hash without the two ever colliding across kinds.
#define ORE_ATTEMPT_STRIDE 32

typedef struct {
	BlockId block;
	int16_t y_min, y_max;
	bool    peaked;      // true: triangular (avg of two uniform draws), false: uniform
	uint8_t attempts;    // per region
	uint8_t size_min, size_max;   // vein length in blocks (walk step count), inclusive
} OreSpec;

static const OreSpec s_ore_table[ORE_KIND_COUNT] = {
	// block                y_min y_max  peaked attempts size_min size_max
	{ BLOCK_COAL_ORE,     0, 127, false, 20, 8, 17 },
	{ BLOCK_IRON_ORE,     0,  63, false, 20, 5,  9 },
	{ BLOCK_GOLD_ORE,     0,  31, false,  2, 5,  9 },
	{ BLOCK_REDSTONE_ORE, 0,  15, false,  8, 4,  8 },
	{ BLOCK_LAPIS_ORE,    0,  31, true,   1, 4,  7 },
	{ BLOCK_DIAMOND_ORE,  0,  15, false,  1, 4,  8 },
};

// Mask-plane storage encodes "which ore" as a 3-bit id: 0 = none, 1..6 = s_ore_table[id-1].
// Index 0 is an unused sentinel so the table can be indexed directly by the stored id.
static const BlockId s_ore_block[ORE_KIND_COUNT + 1] = {
	BLOCK_AIR,
	BLOCK_COAL_ORE, BLOCK_IRON_ORE, BLOCK_GOLD_ORE,
	BLOCK_REDSTONE_ORE, BLOCK_LAPIS_ORE, BLOCK_DIAMOND_ORE,
};

// ── Mask read/write ───────────────────────────────────────────────────────────────────────

static inline int oreMaskReadId(const WorldGenScratch* s, int32_t lx, int y, int32_t lz)
{
	return (int)(((s->ore_bit0[y][lz] >> lx) & 1u)
	           | (((s->ore_bit1[y][lz] >> lx) & 1u) << 1)
	           | (((s->ore_bit2[y][lz] >> lx) & 1u) << 2));
}

// Stamps one cell (wx, y, wz) in world space as carrying ore `kind` (0-based: 0 == coal, per
// s_ore_table's own order), clipped to column (cx, cz) -- the same clip idiom worldgen.c's
// treePut() uses (its own `(x >> 4) != cx || (z >> 4) != cz` check). First writer wins: a cell
// that already carries a kind (any kind) is left untouched -- see ore_gen.h's file comment for
// why that is the right mechanic, not just a tie-break.
static inline void oreStamp(WorldGenScratch* s, int32_t cx, int32_t cz,
                             int32_t wx, int32_t y, int32_t wz, int kind)
{
	if ((wx >> 4) != cx || (wz >> 4) != cz)
		return;
	if (y < 0 || y >= WORLD_HEIGHT)
		return;

	const int32_t lx = wx - cx * CHUNK_DIM;
	const int32_t lz = wz - cz * CHUNK_DIM;

	if (oreMaskReadId(s, lx, y, lz) != 0)
		return;

	const int id = kind + 1;   // 0 is the "none" sentinel, so stored ids are 1..6
	if (id & 1) s->ore_bit0[y][lz] |= (uint16_t)(1u << lx);
	if (id & 2) s->ore_bit1[y][lz] |= (uint16_t)(1u << lx);
	if (id & 4) s->ore_bit2[y][lz] |= (uint16_t)(1u << lx);
}

BlockId oreGenMaskGet(const WorldGenScratch* s, int32_t x_in_col, int y, int32_t z_in_col)
{
	if (y < 0 || y >= WORLD_HEIGHT)
		return BLOCK_AIR;
	const int id = oreMaskReadId(s, x_in_col, y, z_in_col);
	if (id < 1 || id > ORE_KIND_COUNT)
		return BLOCK_AIR;
	return s_ore_block[id];
}

// ── One attempt: draw an anchor, early-out, walk, stamp ─────────────────────────────────────
//
// Pure function of (seed, rx, rz, kind, attempt) plus the requesting column (cx, cz) -- the
// column only affects WHICH stamped cells survive the clip in oreStamp(), never the walk's own
// path, which is why two different requesting columns that both scan this same region reproduce
// bit-identical geometry for it (ore_gen.h's file comment).
static void oreGenAttempt(uint32_t seed, int32_t rx, int32_t rz, int kind, int attempt,
                           int32_t cx, int32_t cz, WorldGenScratch* s)
{
	const OreSpec* spec = &s_ore_table[kind];
	const int32_t idx = kind * ORE_ATTEMPT_STRIDE + attempt;

	Rng r;
	rngSeed(&r, rngHash3(seed ^ SALT_ORE_WALK, rx, idx, rz));

	const int32_t bx0 = rx * ORE_REGION_DIM;
	const int32_t bz0 = rz * ORE_REGION_DIM;
	const int32_t ax = bx0 + (int32_t)rngBelow(&r, ORE_REGION_DIM);
	const int32_t az = bz0 + (int32_t)rngBelow(&r, ORE_REGION_DIM);

	const uint32_t yrange = (uint32_t)(spec->y_max - spec->y_min + 1);
	int32_t ay;
	if (spec->peaked) {
		// Average of two independent uniform draws -> triangular, peaking at the band's own
		// midpoint -- the standard sum-of-two-uniforms trick (also how vanilla implements
		// lapis's peaked curve). The >>1 is a shift on a compile-time constant, not a runtime
		// divide.
		const int32_t u1 = (int32_t)rngBelow(&r, yrange);
		const int32_t u2 = (int32_t)rngBelow(&r, yrange);
		ay = spec->y_min + ((u1 + u2) >> 1);
	} else {
		ay = spec->y_min + (int32_t)rngBelow(&r, yrange);
	}

	// Early-out (cave_carve.c's own early-out #2, same shape, caveCarveSystem()): the attempt's
	// [anchor +/- ORE_MAX_REACH] box against the target column's bounds. An attempt whose
	// anchor is this far away cannot reach the column no matter which way its walk goes,
	// because ORE_MAX_REACH is the hard clamp the walk enforces on every step below.
	const int32_t col_x0 = cx * CHUNK_DIM, col_x1 = col_x0 + CHUNK_DIM - 1;
	const int32_t col_z0 = cz * CHUNK_DIM, col_z1 = col_z0 + CHUNK_DIM - 1;
	if (ax + ORE_MAX_REACH < col_x0 || ax - ORE_MAX_REACH > col_x1 ||
	    az + ORE_MAX_REACH < col_z0 || az - ORE_MAX_REACH > col_z1)
		return;

	const uint32_t srange = (uint32_t)(spec->size_max - spec->size_min + 1);
	const int steps = spec->size_min + (int)rngBelow(&r, srange);

	int32_t px = ax, py = ay, pz = az;
	for (int i = 0; i < steps; i++) {
		oreStamp(s, cx, cz, px, py, pz, kind);

		// A unit step along one of six axis directions, hard-clamped to the ORE_MAX_REACH box
		// measured from the ATTEMPT'S OWN ANCHOR (ax, ay, az) -- not from the walk's current
		// position -- so a sequence of individually-small steps can't drift the walk arbitrarily
		// far. Same clamp shape cave_carve.c's walk uses (measured from the system's own start),
		// for the same reason: it is what keeps the neighbourhood-radius formula's guarantee
		// true. A step that would breach the clamp is simply skipped (the walk stays put for
		// that step and re-stamps the same cell next iteration, which is harmless -- oreStamp()
		// is idempotent).
		const uint32_t dir = rngBelow(&r, 6);
		int32_t nx = px, ny = py, nz = pz;
		switch (dir) {
		case 0: nx++; break;
		case 1: nx--; break;
		case 2: ny++; break;
		case 3: ny--; break;
		case 4: nz++; break;
		default: nz--; break;
		}
		if (nx - ax > ORE_MAX_REACH || nx - ax < -ORE_MAX_REACH ||
		    ny - ay > ORE_MAX_REACH || ny - ay < -ORE_MAX_REACH ||
		    nz - az > ORE_MAX_REACH || nz - az < -ORE_MAX_REACH)
			continue;
		px = nx; py = ny; pz = nz;
	}
}

void oreGenBuildMaskR(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz,
                      int32_t radius)
{
	memset(s->ore_bit0, 0, sizeof(s->ore_bit0));
	memset(s->ore_bit1, 0, sizeof(s->ore_bit1));
	memset(s->ore_bit2, 0, sizeof(s->ore_bit2));

	for (int32_t rz = cz - radius; rz <= cz + radius; rz++) {
		for (int32_t rx = cx - radius; rx <= cx + radius; rx++) {
			for (int kind = 0; kind < ORE_KIND_COUNT; kind++) {
				const OreSpec* spec = &s_ore_table[kind];
				for (int attempt = 0; attempt < spec->attempts; attempt++)
					oreGenAttempt(g->seed, rx, rz, kind, attempt, cx, cz, s);
			}
		}
	}
}

void oreGenBuildMask(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz)
{
	oreGenBuildMaskR(g, s, cx, cz, ORE_NEIGHBOURHOOD_R);
}
