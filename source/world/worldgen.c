#include "world/worldgen.h"

#include "world/genversion.h"
#include "world/noise.h"
#include "world/rng.h"
#include "world/worldgen_density.h"
#include "world/worldgen_scratch.h"

// v1.7.0. Everything below that is not explicitly dispatched is the LEGACY generator, and it
// is deliberately unchanged: a world stamped GEN_VERSION_LEGACY — which is every world that
// existed on 2026-08-24 — must produce the terrain it produced before this rung, byte for
// byte. The suite pins that with a hash of generated columns taken from the tree BEFORE the
// density generator was written. See world/genversion.h for the whole rule.

// Step 9.2a/9.1b. The legacy fill's flat staging buffer for one chunk's worth of cells is
// `s->gen_flat`, and it is built here and then committed in one shot via worldSetChunkAll —
// never CHUNK_BLOCKS individual worldSet calls, which would promote a freshly generated
// chunk's storage cell by cell (UNIFORM -> PALETTE4 -> RAW) for a chunk whose final content is
// already fully known before the first write.
//
// **v1.8.7 moved it, and the cave cache below it, out of this file's .bss and into the
// caller's WorldGenScratch.** It was a file static "following worker.c's s_load_buf
// convention", on the contract that generation runs on one worker thread; that contract is
// exactly what the New 3DS's spare core needs broken. It is still not a stack array — 4 KB is
// not something to risk on a 32 KB worker stack — it is now the lane's, and there can be two
// lanes. See world/worldgen.h's WorldGenScratch note for the measured failure.

// How far apart the terrain's largest features are, as a power of two in blocks. 64 gives
// hills you walk over in a few seconds at 4.31 blocks/s rather than continent-scale
// shapes that read as flat ground from inside the world.
//
// Kept as a shift because the conversion from a block coordinate to a noise coordinate is
// then `x << (FX_SHIFT - GEN_FEATURE_SHIFT)` — one shift, no divide, and the ARM11 has no
// integer divide instruction.
#define GEN_FEATURE_SHIFT 6                       // 64 blocks
#define GEN_OCTAVES       4                       // finest detail every 8 blocks

static inline fx genCoord(int32_t block)
{
	return (fx)((int64_t)block << (FX_SHIFT - GEN_FEATURE_SHIFT));
}

static inline fx biomeCoord(int32_t block)
{
	return (fx)((int64_t)block << (FX_SHIFT - GEN_BIOME_SHIFT));
}

// Caves sample a stretched lattice: wide in x/z, squashed in y. Same shift trick, and the
// squash is what makes tunnels run along the ground instead of standing on end.
static inline fx caveCoordXZ(int32_t block)
{
	return (fx)((int64_t)block << (FX_SHIFT - GEN_CAVE_SHIFT_XZ));
}

static inline fx caveCoordY(int32_t block)
{
	return (fx)((int64_t)block << (FX_SHIFT - GEN_CAVE_SHIFT_Y));
}

// Salts. Each derived field gets its own seed rather than sharing the heightmap's: two
// fBms on the same seed at different scales are visibly the same shape, so a biome map
// sharing the terrain seed would put every desert in the same place as a valley.
#define SALT_BIOME  0x42494F4DU   // 'BIOM'
#define SALT_TREE   0x54524545U   // 'TREE'
#define SALT_GRASS  0x504C4E54U   // 'PLNT'
#define SALT_CAVE   0x43415645U   // 'CAVE'
#define SALT_CAVE2  0x43415632U   // 'CAV2'

// v1.8.3 Phase 2. The humidity field's salt, following the house convention: four ASCII
// characters packed big-endian, applied as rngMix(g->seed ^ SALT_X). Checked against all
// nine existing salts — BIOM TREE PLNT CAVE CAV2 DLOW DHIG DSEL and the BLKS seed mix — and
// it collides with none of them. rngMix is a bijection (lowbias32), so distinct salts always
// give distinct mixed seeds; the check is only that the salt constants themselves differ.
#define SALT_HUMID  0x48554D44U   // 'HUMD'

// v1.8.3 Phase 3. The per-biome flora pass's salt — cactus, dead bush and fern. Same house
// convention as every salt above: four ASCII characters packed big-endian, applied as
// rngMix(g->seed ^ SALT_X). Checked against all ten existing salts — BIOM TREE PLNT CAVE
// CAV2 HUMD DLOW DHIG DSEL and the BLKS seed mix — and it collides with none of them.
//
// It is a NEW salt rather than a second draw off SALT_GRASS, and that is the point: the
// tall-grass pass and this one both ask a question of every eligible surface cell, and
// sharing a hash would tie the answers together — every cell that drew low for grass would
// draw low for fern, so ferns would appear only where tall grass already stood and the two
// scatters would be one field wearing two textures.
#define SALT_FLORA  0x464C4F52U   // 'FLOR'

// v1.8.3 task 52. The per-tree shape draw's salt — canopy width, layer count and whether the
// trunk is 2 x 2. Same house convention, checked against all eleven existing salts — BIOM TREE
// PLNT CAVE CAV2 HUMD FLOR DLOW DHIG DSEL and the BLKS seed mix — and it collides with none.
//
// It has to be its own salt rather than more bits of SALT_TREE's hash, and the reason is
// arithmetic rather than taste: SALT_TREE's 32 bits are fully spent already — 0..7 the spawn
// draw, 8..12 the x offset, 13..17 the z offset, 18..31 the trunk length — so there is no
// unclaimed field left to widen into. Taking bits that another draw already reads would tie
// the two answers together: every cell that spawned a tree at a particular offset would get
// the same canopy, and a forest would repeat on the cell lattice.
#define SALT_TSHAPE 0x54534850U   // 'TSHP'

bool worldgenInit(WorldGen* g, uint32_t seed, uint32_t version)
{
	// Refused rather than coerced. A WorldGen carrying a version this build cannot generate
	// would produce *something* at every call site below, and that something would be the
	// wrong world written into a save that says otherwise — which is the exact failure
	// world/genversion.h exists to make impossible. Version 0 is not a generator, so a
	// refused WorldGen cannot be used by accident either.
	if (!genVersionKnown(version)) {
		g->seed    = 0;
		g->version = 0;
		g->cave_salt[0] = rngMix(g->seed ^ SALT_CAVE);
		g->cave_salt[1] = rngMix(g->seed ^ SALT_CAVE2);
		return false;
	}

	// The seed is mixed once here rather than used raw, so that the obvious human seeds —
	// 0, 1, 2 — give unrelated worlds instead of three near-identical ones. rngHash2's
	// per-axis mixing hides most of that, but not the first octave's low bits.
	//
	// **The mix does not include the version**, deliberately: the same seed on two generators
	// must be recognisably the same world's noise fields, differently shaped, rather than two
	// unrelated worlds. It is also what makes the legacy path byte-identical — folding the
	// version in here would have changed every existing world.
	g->seed    = rngMix(seed ^ 0x424C4B53U);   // 'BLKS'
	g->version = version;

	// v1.8.7. The two cave salts, mixed once here rather than twice per cave test
	// -- about 20,900 rngMix calls per column at the old call site, for two values
	// that cannot change while the WorldGen exists. Output-neutral by construction:
	// rngMix is a pure function of its argument and the argument is g->seed, so this
	// is the identical number the old expression produced every time it ran. Written
	// from g->seed in BOTH arms of this function, so a refused WorldGen's salts are
	// still exactly rngMix(g->seed ^ SALT_X) and the cache below can key on g->seed
	// alone without a refused generator ever aliasing a real one.
	g->cave_salt[0] = rngMix(g->seed ^ SALT_CAVE);
	g->cave_salt[1] = rngMix(g->seed ^ SALT_CAVE2);
	return true;
}

fx worldgenBiome(const WorldGen* g, int32_t x, int32_t z)
{
	return noiseFbm2(rngMix(g->seed ^ SALT_BIOME), biomeCoord(x), biomeCoord(z),
	                 GEN_BIOME_OCTAVES);
}

fx worldgenHumidity(const WorldGen* g, int32_t x, int32_t z)
{
	return noiseFbm2(rngMix(g->seed ^ SALT_HUMID), biomeCoord(x), biomeCoord(z),
	                 GEN_BIOME_OCTAVES);
}

// v1.8.3 Phase 2. The per-biome table, in BiomeId order. Built from the macros in
// world/worldgen.h rather than from bare numbers here, so the derived bound
// GEN_GRASS_CHANCE_MAX cannot drift away from the column it bounds.
//
// The trunk range, the canopy range and the layer pattern are the whole of the silhouette.
//
// **Corrected 2026-09-01, v1.8.3 task 52.** The table this replaces gave jungle and plains the
// identical canopy radius and the identical layer loop, so the "three distinguishable canopies"
// the old comment claimed were two: jungle was a plains tree on a longer trunk. Each biome now
// names a shape, and the two forest biomes draw a width per tree rather than sharing one.
//
// Tundra and desert keep a full row even though their tree_chance is 0 and no tree is ever
// drawn there. A row that cannot be reached is still asserted by the suite's bounds sweep, and
// leaving it at the baseline shape is what makes "trees were switched on for this biome" a
// one-number change rather than a table edit that has to invent a silhouette under time
// pressure.
static const BiomeParams s_biome_params[BIOME_COUNT] = {
	// grass, tree, trunk_min, trunk_max, canopy_min, canopy_max, shape, big_chance
	{GEN_GRASS_TUNDRA, GEN_TREE_TUNDRA, GEN_TREE_MIN_H, GEN_TREE_MAX_H,
	 GEN_TREE_RADIUS, GEN_TREE_RADIUS, TREE_SHAPE_ROUND, 0},
	{GEN_GRASS_TAIGA,  GEN_TREE_TAIGA,  6,              9,
	 1,               2,               TREE_SHAPE_CONIFER, 0},
	{GEN_GRASS_PLAINS, GEN_TREE_PLAINS, GEN_TREE_MIN_H, GEN_TREE_MAX_H,
	 GEN_TREE_RADIUS, GEN_TREE_RADIUS, TREE_SHAPE_ROUND, 0},
	{GEN_GRASS_FOREST, GEN_TREE_FOREST, GEN_TREE_MIN_H, 8,
	 GEN_TREE_RADIUS, GEN_TREE_RADIUS_MAX, TREE_SHAPE_ROUND, 0},
	{GEN_GRASS_DESERT, GEN_TREE_DESERT, GEN_TREE_MIN_H, GEN_TREE_MAX_H,
	 GEN_TREE_RADIUS, GEN_TREE_RADIUS, TREE_SHAPE_ROUND, 0},
	{GEN_GRASS_JUNGLE, GEN_TREE_JUNGLE, 8,              GEN_TREE_TRUNK_MAX,
	 GEN_TREE_RADIUS, GEN_TREE_RADIUS_MAX, TREE_SHAPE_BROAD, 64},
};

const BiomeParams* worldgenBiomeParams(BiomeId b)
{
	// Never NULL, and never an out-of-range read. An id from outside the table is a caller
	// bug, and resolving it to the baseline biome is the one answer that cannot produce a
	// world with a hole in it while the bug is being found.
	if ((unsigned)b >= (unsigned)BIOME_COUNT)
		b = BIOME_PLAINS;
	return &s_biome_params[b];
}

BiomeId worldgenBiomeAt(const WorldGen* g, int32_t x, int32_t z)
{
	// Temperature is the existing field inverted — see GEN_TEMP_HOT in world/worldgen.h for
	// why that direction and why the hot threshold is derived from GEN_SAND_BELOW rather
	// than written as a number of its own.
	const fx temp  = FX_ONE - worldgenBiome(g, x, z);
	const bool wet = worldgenHumidity(g, x, z) >= GEN_HUMID_WET;

	if (temp > GEN_TEMP_HOT)
		return wet ? BIOME_JUNGLE : BIOME_DESERT;
	if (temp < GEN_TEMP_COLD)
		return wet ? BIOME_TAIGA : BIOME_TUNDRA;
	return wet ? BIOME_FOREST : BIOME_PLAINS;
}

bool worldgenIsSandy(const WorldGen* g, int32_t x, int32_t z)
{
	// v1.8.3 Phase 2. **The version gate, and the reason world/worldgen.h documents it at
	// length.** A biome world's sand is the desert cap and nothing else; a legacy or density
	// world keeps the expression it has always had, evaluated on the same field with the same
	// constant, so its beaches and deserts cannot move. The two rules disagree at about a
	// fifth of all columns — every column that is hot AND wet is sand under the old rule and
	// jungle under the new one — so this branch is load-bearing rather than tidy.
	//
	// The gate was `== GEN_VERSION_DENSITY` until 2026-09-01, which put the new rule into
	// every world v1.7.0 through v1.8.2 had already stamped. See genversion.h's note on
	// GEN_VERSION_BIOME for the measurement that caught it.
	if (g->version >= GEN_VERSION_BIOME)
		return worldgenBiomeAt(g, x, z) == BIOME_DESERT;

	return worldgenBiome(g, x, z) < GEN_SAND_BELOW;
}

static inline bool inCaveBand(fx v)
{
	return v >= GEN_CAVE_CENTRE - GEN_CAVE_HALF && v <= GEN_CAVE_CENTRE + GEN_CAVE_HALF;
}

// The cave field, evaluated the long way: two fBms, eight fresh lattice hashes per octave
// per field. This is the v1.8.6 body of worldgenIsCave verbatim, and it is kept for the one
// case the per-column cache below cannot answer — a y above the world ceiling, which is
// outside every caller's loop bounds but not outside this function's signature. Everything
// inside the world goes through the cache.
static bool caveDirect(const WorldGen* g, int32_t x, int y, int32_t z)
{
	const fx cx = caveCoordXZ(x), cy = caveCoordY(y), cz = caveCoordXZ(z);

	if (!inCaveBand(noiseFbm3(g->cave_salt[0], cx, cy, cz, GEN_CAVE_OCTAVES)))
		return false;

	return inCaveBand(noiseFbm3(g->cave_salt[1], cx, cy, cz, GEN_CAVE_OCTAVES));
}

// ── The cave field's per-column corner cache (v1.8.7) ─────────────────────────────────
//
// **This is the "call the noise fewer times" that world/rng.h asks for, and it is NOT the
// hand-factoring that same note forbids.** Task 48b's rng.h note records an rngHash3Cell()
// that returned all eight corners of ONE lattice cell by sharing the hash stages the eight
// rngHash3 calls inside a single value3At() appear to recompute. It was bit-correct and it
// was 10.5 % SLOWER, for a reason specific to it: rngHash2 and rngHash3 are `static inline`,
// so GCC had already common-subexpression-eliminated those stages across the eight call
// sites, and writing the corners out through an array only cost scheduling freedom.
//
// Nothing below touches the inside of a value3At() call. What it shares is work between
// THOUSANDS OF SEPARATE worldgenIsCave() calls — different calls, from a loop in another
// translation unit, whose agreement about a corner is decided by the column coordinate and
// not by anything the optimiser can see in the call. That is the half of the problem the
// optimiser does not see, and it is the half rng.h's closing line points at.
//
// **The redundancy is a property of the shifts, not an accident.** GEN_CAVE_SHIFT_XZ is 5,
// so the first octave's lattice steps 32 blocks in x and z and the second steps 16; a column
// is 16 blocks wide and begins on a multiple of 16, so BOTH octaves' x and z lattice indices
// are constant over the whole 16 x 16 column and only the y index varies. Measured on the
// host for one biome column at seed 1337: 18,571 worldgenIsCave() calls, 41,786 octave
// evaluations, and only 342 distinct lattice cells behind them — a 131x redundancy, and 90 %
// of all the octave work a column does.
//
// So a column needs 2 salts x 2 octaves x a handful of y rows x 4 corners of hashing, paid
// once when the column changes, instead of eight fresh rngHash3 calls per octave per field
// per block.
//
// The table itself is WorldGenCaveCache in world/worldgen_scratch.h, and CAVE_SALTS,
// CAVE_ROWS_AT and CAVE_ROWS are defined beside it: v1.8.7 moved the cache out of this file's
// .bss and into the caller's per-lane scratch, and the type has to be visible to size the
// struct. Nothing about the scheme changed with it.

// The four conditions the whole scheme rests on, asserted rather than described, because
// every one of them is a tuning constant somebody is entitled to change and the failure mode
// of changing one is silently generating a different world rather than a compile error.
_Static_assert(GEN_CAVE_OCTAVES >= 1 && GEN_CAVE_OCTAVES <= GEN_CAVE_SHIFT_Y + 1,
               "a cave octave finer than one lattice row per block has no y index to cache");
_Static_assert((1 << (GEN_CAVE_SHIFT_XZ - (GEN_CAVE_OCTAVES - 1))) >= CHUNK_DIM,
               "the finest cave octave must not step more than once across a column, or the "
               "x and z lattice indices are no longer constant over it");
_Static_assert((1 << GEN_CAVE_SHIFT_Y) <= CHUNK_DIM,
               "the y fraction must repeat within CHUNK_DIM, or sy[] is indexed too narrowly");
_Static_assert(CAVE_ROWS_AT(0) <= CAVE_ROWS, "the row table must be sized for every octave");

// noise.c advances the per-octave seed with rngMix(s ^ this). Restated here rather than
// shared because it is a file-local literal in noise.c and world/noise.h exports no way to
// ask for octave i's seed. If it ever moves, this file computes a different cave field —
// which the suite's twelve pinned legacy terrain fingerprints turn into a red run rather
// than into a quietly reshaped world.
#define CAVE_OCTAVE_ADVANCE 0x2545F491U

// smooth() and lerp() are byte-for-byte world/noise.c's, for the same reason: both are
// `static inline` in that file and neither is exported. The 64-bit intermediate in smooth()
// is not defensive — t^3 at t = 1.0 is 2^48 before the shifts — see the comment there.
static inline fx caveSmooth(fx t)
{
	const int64_t t2 = ((int64_t)t * t) >> FX_SHIFT;
	const int64_t t3 = (t2 * t) >> FX_SHIFT;
	return (fx)(3 * t2 - 2 * t3);
}

static inline fx caveLerp(fx a, fx b, fx t)
{
	return a + (fx)((((int64_t)b - a) * t) >> FX_SHIFT);
}

// Rebuilds the whole table for the column whose -x/-z corner is (bx, bz).
//
// 2 salts x 2 octaves x (9 + 17) rows x 4 corners = 208 rngHash3 calls and 96 smooths for a
// column that then answers up to 32,768 cave tests out of it.
static void caveCacheBuild(WorldGenCaveCache* c, const WorldGen* g, int32_t bx, int32_t bz)
{
	c->valid = false;                       // not usable until every field below is written
	c->seed  = g->seed;
	c->bx    = bx;
	c->bz    = bz;

	for (int o = 0; o < GEN_CAVE_OCTAVES; o++) {
		// The octave's sample position is the block's 16.16 cave coordinate doubled once per
		// octave — exactly what noiseFbm3 does to px/py/pz — and the split into a lattice
		// index and a fraction is value3At's arithmetic shift. The truncation to `fx` before
		// the widening is kept because worldgenIsCave has always done it in that order.
		const int64_t px = (int64_t)caveCoordXZ(bx) << o;
		const int64_t pz = (int64_t)caveCoordXZ(bz) << o;
		const int32_t ix = (int32_t)(px >> FX_SHIFT);
		const int32_t iz = (int32_t)(pz >> FX_SHIFT);

		for (int i = 0; i < CHUNK_DIM; i++) {
			const int64_t qx = (int64_t)caveCoordXZ(bx + i) << o;
			const int64_t qz = (int64_t)caveCoordXZ(bz + i) << o;
			const int64_t qy = (int64_t)caveCoordY(i) << o;
			c->sx[o][i] = caveSmooth((fx)(qx & (FX_ONE - 1)));
			c->sz[o][i] = caveSmooth((fx)(qz & (FX_ONE - 1)));
			c->sy[o][i] = caveSmooth((fx)(qy & (FX_ONE - 1)));
		}

		const int rows = CAVE_ROWS_AT(o);
		for (int s = 0; s < CAVE_SALTS; s++) {
			// Octave 0 uses the salt itself; each later one is the previous seed advanced the
			// way noise.c advances it.
			uint32_t seed = g->cave_salt[s];
			for (int k = 0; k < o; k++)
				seed = rngMix(seed ^ CAVE_OCTAVE_ADVANCE);

			for (int row = 0; row < rows; row++) {
				c->corner[s][o][row][0] = (fx)(rngHash3(seed, ix,     row, iz)     >> 16);
				c->corner[s][o][row][1] = (fx)(rngHash3(seed, ix + 1, row, iz)     >> 16);
				c->corner[s][o][row][2] = (fx)(rngHash3(seed, ix,     row, iz + 1) >> 16);
				c->corner[s][o][row][3] = (fx)(rngHash3(seed, ix + 1, row, iz + 1) >> 16);
			}
		}
	}

	c->valid = true;
}

// One fBm of the cave field, read out of the table. Trilinear interpolation and the octave
// sum in world/noise.c's order and with world/noise.c's normalisation, so the result is the
// same integer noiseFbm3 would have returned — not an approximation of it.
static inline fx caveFieldAt(const WorldGenCaveCache* c, int s, int lx, int y, int lz)
{
	int64_t total = 0;
	int32_t amp   = FX_ONE;

	for (int o = 0; o < GEN_CAVE_OCTAVES; o++) {
		const int row = y >> (GEN_CAVE_SHIFT_Y - o);
		const fx* v0  = c->corner[s][o][row];        // the lattice row at or below y
		const fx* v1  = c->corner[s][o][row + 1];    // and the one above it
		const fx  sx  = c->sx[o][lx];
		const fx  sy  = c->sy[o][y & (CHUNK_DIM - 1)];
		const fx  sz  = c->sz[o][lz];

		const fx y0 = caveLerp(caveLerp(v0[0], v0[1], sx), caveLerp(v1[0], v1[1], sx), sy);
		const fx y1 = caveLerp(caveLerp(v0[2], v0[3], sx), caveLerp(v1[2], v1[3], sx), sy);

		total += (int64_t)caveLerp(y0, y1, sz) * amp;
		amp  >>= 1;
	}

	return noiseFbmNormalise(total, GEN_CAVE_OCTAVES);
}

// v1.8.7's destaticising split what used to be one function in two.
//
// The public worldgenIsCave() is the plain body: no state, no cache, safe from any thread and
// from a caller that has no lane at all. Until the scratch became a parameter it consulted the
// cache too, because the cache lived in a file static it could reach for free; now that the
// cache belongs to a lane, a caller who has one says so.
//
// Bit-identity between the two is not an assumption. It was measured on the host over
// 3,436,800 cells across 4 seeds — every cell of a 5 x 5 column grid, plus 160,000 interleaved
// samples that change column on every call and run y from -8 to WORLD_HEIGHT + 39 — with zero
// mismatches, against a verbatim copy of the pre-cache body. The two fill loops call the
// cached form; nothing else does.
bool worldgenIsCave(const WorldGen* g, int32_t x, int y, int32_t z)
{
	if (y < GEN_CAVE_FLOOR)
		return false;

	return caveDirect(g, x, y, z);
}

bool worldgenIsCaveCached(const WorldGen* g, WorldGenScratch* s, int32_t x, int y, int32_t z)
{
	if (y < GEN_CAVE_FLOOR)
		return false;

	// Above the world. No generator loop reaches here — every one of them stops at
	// WORLD_HEIGHT — but the signature allows it and the table is sized for the world, so it
	// is answered the long way rather than by an out-of-range read.
	if (y >= WORLD_HEIGHT)
		return caveDirect(g, x, y, z);

	// >> 4 then << 4 rather than a divide, so x = -1 lands in column -1 and not column 0 —
	// world.c's rule everywhere, and the reason the cache key is exact on the negative side.
	const int32_t bx = (x >> 4) << 4;
	const int32_t bz = (z >> 4) << 4;

	WorldGenCaveCache* c = &s->cave;
	if (!c->valid || c->seed != g->seed || c->bx != bx || c->bz != bz)
		caveCacheBuild(c, g, bx, bz);

	const int lx = (int)(x - bx), lz = (int)(z - bz);

	// The first field is evaluated for every underground block and the second only for the
	// ~22 % that survive it. Ordering the test this way is worth about a third of the whole
	// pass, and it is free: the two fields are independent, so either order gives the same
	// answer for the same block.
	if (!inCaveBand(caveFieldAt(c, 0, lx, y, lz)))
		return false;

	return inCaveBand(caveFieldAt(c, 1, lx, y, lz));
}

void worldgenScratchInit(WorldGenScratch* s)
{
	s->cave.valid = false;
	s->top_valid  = false;
}

// The legacy heightmap, moved here verbatim from worldgenHeight when the dispatch was added.
// Not one operation of it changed; every constant it reads is still the one it read.
static int legacyHeight(const WorldGen* g, int32_t x, int32_t z)
{
	const fx n = noiseFbm2(g->seed, genCoord(x), genCoord(z), GEN_OCTAVES);
	const fx b = worldgenBiome(g, x, z);

	// The biome sets how much of the band this column is allowed to use: a quarter of it
	// where the biome is at its lowest, all of it where it is at its highest. Written as a
	// deviation from the MIDDLE of the band rather than a scale from the bottom, because
	// scaling from the bottom would put every flat region at the bottom of the world and
	// tie flatness to altitude — flat would always mean low, which is a basin, not a plain.
	const int64_t amp = (int64_t)GEN_SURFACE_RANGE *
	                    (FX_ONE / GEN_FLAT_FRACTION +
	                     (int64_t)b * (GEN_FLAT_FRACTION - 1) / GEN_FLAT_FRACTION);

	// n is [0, FX_ONE], so (n - half) is [-0.5, +0.5] and the product is blocks in
	// 16.16 twice over — hence the double shift. 64-bit throughout: amp alone is already
	// 1.8 million, and this is exactly the expression someone widens the range in later.
	const int off = (int)(((int64_t)(n - FX_ONE / 2) * amp) >> (FX_SHIFT * 2));
	const int h   = GEN_SURFACE_MIN + GEN_SURFACE_RANGE / 2 + off;

	// Clamped even though the arithmetic cannot leave the band, because the cost is one
	// compare per column and the failure it prevents is a write past the end of a chunk.
	if (h < 1) return 1;
	if (h > WORLD_HEIGHT - 1) return WORLD_HEIGHT - 1;
	return h;
}

int worldgenHeight(const WorldGen* g, int32_t x, int32_t z)
{
	// `>=`, not `==`: a biome world is a density world with more on top, and the field itself
	// is shared. genversion.h states the rule — the FIELD is >= GEN_VERSION_DENSITY, biome
	// IDENTITY is >= GEN_VERSION_BIOME.
	return (g->version >= GEN_VERSION_DENSITY) ? wgdHeight(g, x, z)
	                                           : legacyHeight(g, x, z);
}

// The block at a given depth below the surface. Kept separate from the fill loop so the
// layering is one readable rule rather than three nested conditions inside a triple loop.
//
// `sandy` replaces the whole grass-and-dirt cap with sand rather than only the top block:
// a single sand block over dirt reads as a patch of dirt with a lid on it the moment you
// dig one block, and the cliff faces at the edge of a dune would still be brown.
static inline BlockId blockAtDepth(int depth, bool sandy)
{
	if (sandy) return (depth <= GEN_DIRT_DEPTH) ? BLOCK_SAND : BLOCK_STONE;
	if (depth == 0) return BLOCK_GRASS;                  // the top solid block
	if (depth <= GEN_DIRT_DEPTH) return BLOCK_DIRT;
	return BLOCK_STONE;
}

// One tree cell's tree, or nothing. Everything about a tree — whether it exists, where in
// its cell it stands, how tall it is — comes out of one hash of the cell coordinate, so
// two columns generated in either order, or in different sessions, agree about it without
// having to talk to each other.
typedef struct {
	bool    exists;
	int32_t x, z;    // world block coordinates of the trunk, its -x/-z corner when wide
	int     ground;  // y of the first air above the ground, i.e. the trunk's base
	int     trunk;   // trunk blocks
	int     radius;  // canopy half-width of the widest layer — with `shape`, the silhouette
	int     shape;   // a TreeShape
	int     tiers;   // TREE_SHAPE_CONIFER only: how many narrow/wide tier pairs
	bool    wide;    // 2 x 2 trunk rather than one column
} Tree;

static Tree treeInCell(const WorldGen* g, int32_t tcx, int32_t tcz,
                       int32_t col_x0, int32_t col_x1, int32_t col_z0, int32_t col_z1)
{
	Tree t = {0};
	t.radius = GEN_TREE_RADIUS;
	t.shape  = TREE_SHAPE_ROUND;
	t.tiers  = 2;

	const uint32_t h = rngHash2(rngMix(g->seed ^ SALT_TREE), tcx, tcz);

	// Offset inside the cell, kept away from the very edge so that two trees in adjacent
	// cells cannot stand shoulder to shoulder: with GEN_TREE_CELL 8 and the offset in
	// 1..6, the closest two trunks can be is three blocks, which is one more than a
	// canopy's reach on each side.
	//
	// **Computed BEFORE the spawn draw as of v1.8.3, and that reordering is deliberate and
	// legacy-safe.** The offsets are a pure function of `h` with no side effects, so moving
	// them above the draw cannot change what the draw decides; what it buys is a trunk
	// coordinate to resolve the biome at, which is what the per-biome tree density needs.
	// A cell whose draw fails still returns t.exists == false and every caller ignores the
	// rest of the struct. The twelve legacy fingerprints are the check on this.
	t.x = tcx * GEN_TREE_CELL + 1 + (int32_t)((h >> 8) % (GEN_TREE_CELL - 2));
	t.z = tcz * GEN_TREE_CELL + 1 + (int32_t)((h >> 13) % (GEN_TREE_CELL - 2));

	// **v1.8.6 "Speed".** The horizontal-reach reject, and the reason it lives here rather
	// than in worldgenDecorate's caller loop: this is the earliest point at which the
	// tree's actual position is known, and everything below it — the biome resolve (two
	// fBms), the draw, the sandy test and the height query at the bottom of this function —
	// is wasted work for a tree whose canopy cannot possibly land a block in the column
	// being decorated. worldgenDecorate's scan window is deliberately wider than one column
	// (it has to catch a canopy standing in a neighbouring cell), which is exactly what
	// makes most of the cells it visits belong to a tree that treePut() was always going to
	// clip into nothing — see docs/ROADMAP.md's v1.8.6 entry for the measured fraction.
	//
	// The bound is GEN_TREE_REACH_MAX on both sides of both axes, the same conservative
	// constant worldgenDecorate's own scan window is built from (worldgen.h's comment on it
	// states why: it is `GEN_TREE_RADIUS_MAX + 1`, wide enough for the widest canopy this
	// build can draw standing on a 2 x 2 trunk). Symmetric rather than tight — a real
	// canopy's reach is asymmetric, `-radius .. +radius+ext` — because tightening it would
	// need `bp` and the per-tree shape draw, both of which are exactly the work this reject
	// exists to skip. A reject built from a bound that is never smaller than the true reach
	// can only be conservative: it may let an unreachable tree through to the height query
	// on the far side of that slack, but it can never throw away one that could have placed
	// a block, which is what keeps the generated world byte-identical.
	//
	// A pure function of t.x, t.z and the column being decorated, with no side effect on
	// `t` beyond what every other early-return in this function already leaves it at — so a
	// rejected cell returns the same zeroed-out, `exists == false` struct the draw-chance
	// and sandy rejects below return, and every caller already ignores every field but
	// `exists` in that case.
	if (t.x + GEN_TREE_REACH_MAX < col_x0 || t.x - GEN_TREE_REACH_MAX > col_x1 ||
	    t.z + GEN_TREE_REACH_MAX < col_z0 || t.z - GEN_TREE_REACH_MAX > col_z1)
		return t;

	// v1.8.3 Phase 2. Tree density and silhouette per biome on a BIOME world; the single
	// global constants on a legacy or density one, which is what keeps every existing world's
	// forest exactly where it was. The gate read `== GEN_VERSION_DENSITY` until 2026-09-01 —
	// see genversion.h on GEN_VERSION_BIOME.
	int trunk_min = GEN_TREE_MIN_H, trunk_max = GEN_TREE_MAX_H;
	uint32_t chance = (uint32_t)GEN_TREE_CHANCE;
	const BiomeParams* bp = NULL;
	if (g->version >= GEN_VERSION_BIOME) {
		bp        = worldgenBiomeParams(worldgenBiomeAt(g, t.x, t.z));
		chance    = bp->tree_chance;
		trunk_min = bp->trunk_min;
		trunk_max = bp->trunk_max;
		t.shape   = bp->shape;
		t.radius  = bp->canopy_min;
	}

	// The draw uses the same low 8 bits it always has, compared against the biome's chance
	// instead of one global number. Tundra and desert are 0, so the comparison is false for
	// every possible draw and nothing grows there — no separate "does this biome have trees"
	// branch to keep in step with the table.
	if ((h & 0xFFu) >= chance)
		return t;

	// Sand grows nothing. Asked at the trunk only: a canopy overhanging a dune edge is
	// what a tree on a shoreline actually looks like.
	if (worldgenIsSandy(g, t.x, t.z))
		return t;

	t.ground = worldgenHeight(g, t.x, t.z);

	// v1.7.0. Nothing grows at or below the waterline. The legacy generator's whole surface
	// band sits above GEN_SEA_LEVEL by construction (GEN_SURFACE_MIN 40 + half the range puts
	// its lowest ground at 40, but its *measured* floor across seeds is 52 and it has no
	// concept of water at all), so this is gated on the generator rather than applied to both:
	// an unconditional test could not change a legacy world today, but it would be a rule the
	// legacy path had never been measured against, and the point of the versioning work is
	// that nothing new reaches an old world.
	//
	// Task 17 fills the oceans; this is what stops it filling them around tree trunks.
	// `>=`: the waterline exists on every generator from the density field upward, so a biome
	// world must keep this rule too. Only the legacy path, which has no concept of water, is
	// excluded.
	if (g->version >= GEN_VERSION_DENSITY && t.ground <= GEN_SEA_LEVEL)
		return t;

	t.trunk  = trunk_min +
	           (int)((h >> 18) % (uint32_t)(trunk_max - trunk_min + 1));

	// v1.8.3 task 52. The per-tree shape draw, off its own salt for the reason SALT_TSHAPE
	// states. Only trees that actually spawn reach it, so the cost is one hash per tree rather
	// than one per cell scanned.
	if (bp != NULL) {
		const uint32_t hs = rngHash2(rngMix(g->seed ^ SALT_TSHAPE), tcx, tcz);

		t.radius = (int)bp->canopy_min +
		           (int)((hs & 0xFFu) % (uint32_t)(bp->canopy_max - bp->canopy_min + 1));

		// Conifer only: two tiers or three, which is a 4-block canopy or a 6-block one. Drawn
		// from bits the radius did not use.
		t.tiers = 2 + (int)((hs >> 8) & 1u);

		// The 2 x 2 trunk — task 52's "trunk width". Two conditions, and the second is the one
		// that matters: the four columns must share a ground height. A wide trunk is anchored
		// at one column's ground, so on a slope the other three would either hang in the air or
		// start underground, and the canopy would sit crooked on top of it. Refusing rather
		// than levelling keeps every tree a pure function of its cell — no terrain is edited to
		// make a tree fit.
		if (bp->big_chance > 0u && ((hs >> 16) & 0xFFu) < (uint32_t)bp->big_chance
		    && worldgenHeight(g, t.x + 1, t.z)     == t.ground
		    && worldgenHeight(g, t.x,     t.z + 1) == t.ground
		    && worldgenHeight(g, t.x + 1, t.z + 1) == t.ground)
			t.wide = true;
	}

	// Nothing may be written above the world ceiling. A conifer's tip sits one block above the
	// trunk like every other shape, so the trunk plus one still bounds the tallest cell written
	// — 68 + 12 + 1 is 81 against a 128-block world — but the surface band and the trunk
	// ceiling are both tuning constants and the canopy loop below writes without re-checking.
	if (t.ground + t.trunk + 1 >= WORLD_HEIGHT)
		return t;

	t.exists = true;
	return t;
}

// Writes one block of a tree, but only if it lands inside column (cx, cz).
//
// The clip is the whole reason the decoration pass is order-independent. A tree standing
// near a column border reaches into its neighbour, and if it wrote there directly, the
// neighbour's own terrain fill — which runs later, or earlier, or in another session —
// would erase exactly the part that crossed over. Instead every column scans the trees
// around it and writes only its own cells, so each block is decided once, by the column
// that owns it, from data that does not depend on what has been generated yet.
static bool treePut(World* w, int32_t cx, int32_t cz, int32_t x, int y, int32_t z,
                    BlockId id, bool only_into_air)
{
	// >> 4 rather than / CHUNK_DIM: arithmetic shift floors, which is the right behaviour
	// for negative coordinates and what world.c uses everywhere else. Division truncates
	// towards zero and would put block -1 in column 0.
	if ((x >> 4) != cx || (z >> 4) != cz)
		return true;
	if (only_into_air && worldGet(w, x, y, z) != BLOCK_AIR)
		return true;
	return worldSet(w, x, y, z, id);
}

// v1.8.3 task 52. The three silhouettes, as two pure functions of the tree and the layer.
//
// They are functions rather than a table because the conifer's depth varies per tree. They stay
// static: the suite does not call them, it measures the leaves that actually land in a
// generated world, which is the only version of "a taiga tree is narrower than a jungle tree"
// that a shape helper agreeing with itself could not fake.
//
// dy is measured from `top`, the first cell above the trunk. A negative radius means the layer
// is empty; only CONIFER uses it, for the gaps between its tiers.
static int canopyLowest(const Tree* t)
{
	switch (t->shape) {
	case TREE_SHAPE_CONIFER: return -(2 * t->tiers - 1);
	case TREE_SHAPE_BROAD:   return -2;
	default:                 return -2;
	}
}

static int canopyRadiusAt(const Tree* t, int dy)
{
	switch (t->shape) {
	case TREE_SHAPE_CONIFER:
		// A single leaf above the trunk, a narrow collar at the top of it, then tiers
		// alternating wide and narrow all the way down. The alternation is the whole reason a
		// spruce reads as a spruce at 400x240: a straight taper is a cone, and a cone at this
		// block count is indistinguishable from a ball.
		if (dy == 1)  return 0;
		if (dy == 0)  return 1;
		return ((-dy) & 1) ? t->radius : 1;

	case TREE_SHAPE_BROAD:
		// Three layers at full width with a narrow cap. Deep and flat, so from below it is a
		// ceiling and from a distance it is a plateau rather than a lollipop.
		return (dy == 1) ? 1 : t->radius;

	default:
		// ROUND, and byte-for-byte the shape every world before v1.8.3 was drawn with: two
		// wide layers at and below the top, two narrow above.
		return (dy <= -1) ? t->radius : 1;
	}
}

// Distance from a coordinate offset to the trunk's footprint along one axis. `ext` is 1 for a
// 2 x 2 trunk and 0 for a single column, so a wide tree's crown is clipped around a 2-block
// footprint instead of around a point — which is what keeps it centred rather than lopsided.
static int footprintDist(int d, int ext)
{
	if (d < 0)
		return -d;
	if (d > ext)
		return d - ext;
	return 0;
}

bool worldgenDecorate(const WorldGen* g, World* w, int32_t cx, int32_t cz)
{
	// Tree cells that can reach this column. The column covers blocks [cx*16, cx*16+15]
	// and a canopy reaches GEN_TREE_RADIUS further, so the block span is exact; the extra
	// cell at each end covers integer division truncating towards zero on the negative
	// side, which is cheaper than writing a floor-divide for a bound that is scanned once
	// per column and costs one hash per extra cell.
	//
	// v1.8.3 task 52: GEN_TREE_REACH_MAX, not GEN_TREE_RADIUS. A biome world can now draw a
	// canopy of radius 3 on a 2 x 2 trunk, which reaches 4 blocks from the tree's anchor
	// column. Scanning to the old bound would have left the far cells of a wide jungle tree
	// to columns that never looked at it — the block would be leaves or air depending on which
	// column was generated first. Legacy and density worlds never draw more than
	// GEN_TREE_RADIUS, so widening the scan costs them at most one extra hashed cell per side
	// and changes nothing they produce.
	const int32_t x0 = (cx * CHUNK_DIM - GEN_TREE_REACH_MAX) / GEN_TREE_CELL - 1;
	const int32_t x1 = (cx * CHUNK_DIM + CHUNK_DIM - 1 + GEN_TREE_REACH_MAX) / GEN_TREE_CELL + 1;
	const int32_t z0 = (cz * CHUNK_DIM - GEN_TREE_REACH_MAX) / GEN_TREE_CELL - 1;
	const int32_t z1 = (cz * CHUNK_DIM + CHUNK_DIM - 1 + GEN_TREE_REACH_MAX) / GEN_TREE_CELL + 1;

	// v1.8.6. The column's own block bounds, passed down to treeInCell() so its
	// horizontal-reach reject can run before the height query — see the comment there.
	// Computed once here rather than once per cell: cx and cz do not change inside this
	// loop, so the four multiplies belong outside it.
	const int32_t col_x0 = cx * CHUNK_DIM;
	const int32_t col_x1 = col_x0 + CHUNK_DIM - 1;
	const int32_t col_z0 = cz * CHUNK_DIM;
	const int32_t col_z1 = col_z0 + CHUNK_DIM - 1;

	bool ok = true;
	for (int32_t tcz = z0; tcz <= z1; tcz++) {
		for (int32_t tcx = x0; tcx <= x1; tcx++) {
			const Tree t = treeInCell(g, tcx, tcz, col_x0, col_x1, col_z0, col_z1);
			if (!t.exists)
				continue;

			// Trunk. Written over whatever is there, so a trunk cannot be hollowed out by
			// a canopy block from a neighbouring tree that happened to be placed first.
			//
			// v1.8.3 task 52: one column or four. The four share a ground height by
			// construction — treeInCell refuses `wide` otherwise — so one loop covers them.
			const int span = t.wide ? 2 : 1;
			for (int i = 0; i < t.trunk; i++)
				for (int dz = 0; dz < span; dz++)
					for (int dx = 0; dx < span; dx++)
						ok &= treePut(w, cx, cz, t.x + dx, t.ground + i, t.z + dz,
						              BLOCK_WOOD, false);

			// Canopy. v1.8.3 task 52 replaced one loop with three silhouettes, and the
			// layer pattern is chosen per shape rather than shared:
			//
			//   ROUND    two wide layers at and just below the top of the trunk, two narrow
			//            ones above — the shape the game has always drawn.
			//   CONIFER  tiers alternating narrow and wide from the bottom up, finishing in a
			//            single leaf above the trunk. Two tiers or three, drawn per tree.
			//   BROAD    a deep flat crown: three layers at full width and a narrow cap, so a
			//            jungle canopy reads as a ceiling rather than as a ball.
			//
			// The corner clip is what stops any of them being a cube on a stick, and it is
			// written against the tree's own radius rather than a constant so that a radius-1
			// canopy keeps all nine of its cells (a 3 x 3 has no corner to round off) while a
			// radius-3 one is clipped like the radius-2 one always was.
			//
			// `wide` shifts nothing: the loops below run dx over -r..r+1 when the trunk is
			// 2 x 2, and the clip measures distance to the trunk FOOTPRINT rather than to a
			// point, so the crown sits centred over four columns instead of one.
			const int top = t.ground + t.trunk;   // first y above the trunk
			const int ext = t.wide ? 1 : 0;

			for (int dy = canopyLowest(&t); dy <= 1; dy++) {
				const int r = canopyRadiusAt(&t, dy);
				if (r < 0)
					continue;
				for (int dz = -r; dz <= r + ext; dz++) {
					for (int dx = -r; dx <= r + ext; dx++) {
						const int fx = footprintDist(dx, ext);
						const int fz = footprintDist(dz, ext);
						if (r >= 2 && fx * fx + fz * fz > r * r)
							continue;                        // clipped corners
						ok &= treePut(w, cx, cz, t.x + dx, top + dy, t.z + dz,
						              BLOCK_LEAVES, true);
					}
				}
			}
		}
	}

	return ok;
}

// v1.8.3 Phase 3's flora pass, defined below worldgenScatter because it is the second half
// of that pass and reads far better after it. Forward-declared rather than moved above it so
// the tall-grass rule — the one this file has shipped since v1.7.0 — stays the first thing a
// reader meets. `tops` is worldgenScatter's own wgdColumnTops() pointer, handed down rather
// than re-fetched: it is per-column state that the caller has already validated.
static bool worldgenFlora(const WorldGen* g, World* w, int32_t cx, int32_t cz,
                          const int16_t* tops);

// v1.7.0 task 19. Tall grass on the exposed surface.
//
// Gated on the generator by its only caller, exactly as the tree pass's waterline rule is: a
// legacy world has no water block, no plant, and no measurement behind either, and the whole
// point of world/genversion.h is that nothing new reaches an old world.
bool worldgenScatter(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz)
{
	// The surface heights wgdColumn already computed for this column, in THIS LANE's scratch.
	// Refused rather than recomputed if they belong to somewhere else — see wgdColumnTops()
	// for why asking worldgenHeight() 256 times instead is not an option.
	//
	// **This check is where the old file statics failed loudest.** With one shared `s_top` key,
	// the other thread moved it between wgdColumn and here and the whole column reported
	// failure: 159 of 192 generations refused in the measured two-thread run. Keyed on a
	// per-lane scratch it can only fail for the reason it was written for — a caller pairing
	// the scatter with the wrong column.
	const int16_t* tops = wgdColumnTops(s, cx, cz);
	if (!tops)
		return false;

	const uint32_t salt = rngMix(g->seed ^ SALT_GRASS);

	bool ok = true;
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			// tops[] is worldgenHeight's convention, so this y is the first cell above the
			// ground — where the plant would stand — and y - 1 is the surface block.
			const int y = tops[lz * CHUNK_DIM + lx];

			// Above the waterline only. Nothing grows in the sea, and the surface pass has
			// already made everything up to GEN_SEA_LEVEL + GEN_D_BEACH_ABOVE sand anyway —
			// this is the rule stated rather than left to be an accident of the beach band.
			if (y <= GEN_SEA_LEVEL)
				continue;
			// No room under the ceiling. Cannot fire at the current biome table, whose
			// tallest nominal surface is 120 against a 128-block world, but the table is a
			// tuning constant and this writes without re-checking.
			if (y >= WORLD_HEIGHT)
				continue;

			const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;

			// The draw first: it rejects most cells for one hash, which is cheaper than the
			// two world lookups below it and than the noise the biome costs, and makes both
			// the exception rather than the rule.
			//
			// v1.8.3 Phase 2. The draw is compared twice: once against the largest chance
			// any biome has, which needs no noise at all, and then against this cell's own
			// biome. GEN_GRASS_CHANCE_MAX is an upper bound over the whole table, so the
			// first test can only reject cells the second would have rejected anyway — the
			// answer is identical to resolving the biome for all 256 cells, at a fraction of
			// the cost. The suite asserts the bound really does bound the table.
			//
			// Gated on GEN_VERSION_BIOME as of 2026-09-01. A world stamped
			// GEN_VERSION_DENSITY keeps the single global GEN_GRASS_CHANCE 24 it was made
			// with — this pass is reachable from the density path, so leaving the per-biome
			// table ungated changed the tall grass in every v1.7.0-v1.8.2 world. See
			// genversion.h on GEN_VERSION_BIOME.
			const uint32_t draw = rngHash2(salt, x, z) & 0xFFu;
			if (g->version >= GEN_VERSION_BIOME) {
				if (draw >= (uint32_t)GEN_GRASS_CHANCE_MAX)
					continue;
				if (draw >= (uint32_t)worldgenBiomeParams(worldgenBiomeAt(g, x, z))
				                          ->grass_chance)
					continue;
			} else if (draw >= (uint32_t)GEN_GRASS_CHANCE) {
				continue;
			}

			// Grass only — not sand, not the bare stone of a cliff face, not a dirt scar.
			if (worldGet(w, x, y - 1, z) != BLOCK_GRASS)
				continue;

			// **The one rule that makes this pass safe.** Air, and nothing else, is written
			// into. A trunk, a leaf or a block of water occupying this cell is not air, so
			// the plant is silently not placed — see worldgen.h for why that is the right
			// direction for the loss to fall in.
			if (worldGet(w, x, y, z) != BLOCK_AIR)
				continue;

			ok &= worldSet(w, x, y, z, BLOCK_TALL_GRASS);
		}
	}

	// v1.8.3 Phase 3, and BIOME-only: cactus, dead bush and fern are biome identity, and a
	// world stamped GEN_VERSION_DENSITY was made before any of them existed. Running it there
	// would put plants into worlds their players have already walked through.
	if (g->version >= GEN_VERSION_BIOME)
		return worldgenFlora(g, w, cx, cz, tops) && ok;

	return ok;
}

// v1.8.3 Phase 3. Are the `n` cells from (x, y, z) upwards inside the world AND all air?
//
// Asked before ANY of them is written, so a cactus that would not fit is not placed at all
// rather than placed half-height. The ceiling test is part of the same question because the
// only reason a cell can be missing is that it is above the world, and answering the two
// separately is how a loop ends up writing the first block and then discovering the second.
static bool cellsClear(World* w, int32_t x, int y, int32_t z, int n)
{
	if (y + n > WORLD_HEIGHT)
		return false;
	for (int i = 0; i < n; i++)
		if (worldGet(w, x, y + i, z) != BLOCK_AIR)
			return false;
	return true;
}

// v1.8.3 Phase 3. The per-biome flora pass: cactus and dead bush on desert sand, fern on
// taiga and jungle grass.
//
// **Why it is a second loop and not a second draw inside worldgenScatter's.** That loop's
// very first test is `draw >= GEN_GRASS_CHANCE_MAX`, and GEN_GRASS_TUNDRA and
// GEN_GRASS_DESERT are both 0 — so the cells this pass exists for are exactly the ones that
// loop throws away before it ever resolves a biome. Folding this in would mean widening that
// bound to cover both tables, which would make the cheap rejection stop rejecting and cost
// the noise for a biome lookup on cells that want neither plant. Two loops, two bounds, each
// tight.
//
// It runs AFTER the tall-grass loop and keeps that pass's one safety rule unchanged: air,
// and nothing else, is written into. A trunk, a leaf, a block of water or a tall grass
// already standing in the cell is not air, so the flora is silently not placed. Order
// between the two passes is therefore fixed and deliberate — tall grass wins a contested
// cell — but neither depends on which COLUMN was generated first, because every input is a
// hash of the world (x, z) plus this column's own blocks.
static bool worldgenFlora(const WorldGen* g, World* w, int32_t cx, int32_t cz,
                          const int16_t* tops)
{
	const uint32_t salt = rngMix(g->seed ^ SALT_FLORA);

	bool ok = true;
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int y = tops[lz * CHUNK_DIM + lx];

			// The same two gates the tall-grass loop applies, and for the same reasons:
			// nothing grows in the sea, and nothing is written through the ceiling.
			if (y <= GEN_SEA_LEVEL)
				continue;
			if (y >= WORLD_HEIGHT)
				continue;

			const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;

			// One hash, used twice: the low byte is the draw, and bit 8 is the cactus's
			// height. Two independent draws off one hash rather than two hashes, which is
			// what treeInCell() does with its own (offset, chance, trunk, radius) bundle.
			const uint32_t h    = rngHash2(salt, x, z);
			const uint32_t draw = h & 0xFFu;

			// The cheap reject, exactly the shape the tall-grass loop uses.
			// GEN_FLORA_CHANCE_MAX is an upper bound over every branch below, so this can
			// only reject cells those branches would have rejected anyway — the answer is
			// identical to resolving the biome for all 256 cells, at a fraction of the
			// cost. The bound holds BY CONSTRUCTION rather than by assertion: worldgen.h
			// defines it as a MAX over the same constants these branches test against.
			// A branch added below with a NEW chance must be folded into that MAX, or
			// this reject silently clamps it; worldgen.h says so at the definition.
			if (draw >= (uint32_t)GEN_FLORA_CHANCE_MAX)
				continue;

			const BiomeId b = worldgenBiomeAt(g, x, z);
			switch (b) {
			case BIOME_DESERT:
				// Sand only. The desert cap is sand by surfaceBlock(), but a cliff face
				// inside a desert is bare stone and a dune's edge at the waterline is
				// beach — asking the block rather than the biome is what keeps a cactus
				// off a rock face.
				if (worldGet(w, x, y - 1, z) != BLOCK_SAND)
					continue;

				if (draw < (uint32_t)GEN_CACTUS_CHANCE) {
					// One or two blocks. Both must be clear before either is written.
					const int th = GEN_CACTUS_MIN_H +
					               (int)((h >> 8) %
					                     (uint32_t)(GEN_CACTUS_MAX_H - GEN_CACTUS_MIN_H + 1));
					if (!cellsClear(w, x, y, z, th))
						continue;
					for (int i = 0; i < th; i++)
						ok &= worldSet(w, x, y + i, z, BLOCK_CACTUS);
				} else if (draw < (uint32_t)(GEN_CACTUS_CHANCE + GEN_DEAD_BUSH_CHANCE)) {
					if (!cellsClear(w, x, y, z, 1))
						continue;
					ok &= worldSet(w, x, y, z, BLOCK_DEAD_BUSH);
				}
				break;

			case BIOME_TAIGA:
			case BIOME_JUNGLE: {
				const uint32_t chance = (b == BIOME_TAIGA) ? (uint32_t)GEN_FERN_TAIGA
				                                           : (uint32_t)GEN_FERN_JUNGLE;
				if (draw >= chance)
					continue;
				// Grass only — not the bare stone of a cliff face, not a dirt scar, and
				// not the snow or sand of a neighbouring cap that reached in.
				if (worldGet(w, x, y - 1, z) != BLOCK_GRASS)
					continue;
				if (!cellsClear(w, x, y, z, 1))
					continue;
				ok &= worldSet(w, x, y, z, BLOCK_FERN);
				break;
			}

			// Tundra, plains and forest grow none of these three. Written out rather than
			// left to a default so that adding a biome is a compile warning here under
			// -Wswitch rather than a silent nothing.
			case BIOME_TUNDRA:
			case BIOME_PLAINS:
			case BIOME_FOREST:
			case BIOME_COUNT:
				break;
			}
		}
	}

	return ok;
}

static bool legacyColumn(const WorldGen* g, WorldGenScratch* s, World* w,
                         int32_t cx, int32_t cz);

bool worldgenColumn(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz)
{
	// v1.7.0. The two generators share the decoration pass and nothing else — the tree rules
	// are about where a tree may stand, not about how the ground got there, so duplicating
	// them per generator would be two copies of one answer.
	//
	// `>=`: the density FIELD builds every world from version 2 upward. What differs above it
	// is biome identity, and that is decided inside the passes, not here. See genversion.h.
	if (g->version >= GEN_VERSION_DENSITY) {
		if (!wgdColumn(g, s, w, cx, cz))
			return false;
		// Both are run even if the first fails, and both results are reported: a refused
		// allocation in the tree pass is a real condition the caller has to see, and
		// short-circuiting past the scatter would make a half-decorated column look like a
		// fully decorated one on the next visit.
		const bool decorated = worldgenDecorate(g, w, cx, cz);
		return worldgenScatter(g, s, w, cx, cz) && decorated;
	}

	return legacyColumn(g, s, w, cx, cz);
}

static bool legacyColumn(const WorldGen* g, WorldGenScratch* s, World* w,
                         int32_t cx, int32_t cz)
{
	// 256 heights, computed once for the whole 8-chunk stack. Doing this per chunk would
	// evaluate the same fBm up to eight times for the same answer, and the fBm is the
	// expensive part of generation.
	int  height[CHUNK_DIM][CHUNK_DIM];
	bool sandy[CHUNK_DIM][CHUNK_DIM];
	int max_h = 0;
	int min_h = WORLD_HEIGHT;
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
			const int h = worldgenHeight(g, x, z);
			height[lz][lx] = h;
			sandy[lz][lx]  = worldgenIsSandy(g, x, z);
			if (h > max_h) max_h = h;
			if (h < min_h) min_h = h;
		}
	}

	if (!worldColumnCreate(w, cx, cz))
		return false;

	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		const int y0 = cy * CHUNK_DIM;

		// Entirely above the highest ground in this column: leave it unallocated. An
		// absent chunk reads as air and costs a NULL pointer, so a world of mostly sky
		// costs almost nothing — and chunkIsAllAir gives the mesher the same early-out
		// for it as for a dug-out one.
		if (y0 >= max_h)
			continue;

		// Entirely below the lowest ground AND deep enough that every block in it is
		// carvable: solid stone throughout, then the cave pass. The bound is
		// GEN_CAVE_MIN_DEPTH rather than the old GEN_DIRT_DEPTH + 1 so that this branch
		// does not have to think about depth at all — one constant governs both the
		// layering (5 > 3, so it is all stone) and the carve.
		if (y0 + CHUNK_DIM <= min_h - GEN_CAVE_MIN_DEPTH) {
			for (int i = 0; i < CHUNK_BLOCKS; i++) s->gen_flat[i] = BLOCK_STONE;
			for (int lz = 0; lz < CHUNK_DIM; lz++)
				for (int lx = 0; lx < CHUNK_DIM; lx++)
					for (int ly = 0; ly < CHUNK_DIM; ly++)
						if (worldgenIsCaveCached(g, s, cx * CHUNK_DIM + lx, y0 + ly,
						                         cz * CHUNK_DIM + lz))
							s->gen_flat[chunkIndex(lx, ly, lz)] = BLOCK_AIR;
			if (!worldSetChunkAll(w, cx, cy, cz, s->gen_flat))
				return false;
			continue;
		}

		for (int i = 0; i < CHUNK_BLOCKS; i++) s->gen_flat[i] = BLOCK_AIR;
		for (int lz = 0; lz < CHUNK_DIM; lz++) {
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
				const int h = height[lz][lx];
				// Only the part of this chunk that is underground gets written; the loop
				// stops at the surface rather than testing every one of the 16 cells.
				int top = h - 1 - y0;                    // local y of the top solid block
				if (top > CHUNK_DIM - 1) top = CHUNK_DIM - 1;
				for (int ly = 0; ly <= top; ly++) {
					const int depth = h - 1 - (y0 + ly);
					// The buffer starts as air, so a carved block is simply not written.
					// The depth guard comes first because it is a compare against a local
					// and the cave test is two fBms.
					if (depth >= GEN_CAVE_MIN_DEPTH &&
					    worldgenIsCaveCached(g, s, x, y0 + ly, z))
						continue;
					s->gen_flat[chunkIndex(lx, ly, lz)] = blockAtDepth(depth, sandy[lz][lx]);
				}
			}
		}
		if (!worldSetChunkAll(w, cx, cy, cz, s->gen_flat))
			return false;
	}

	return worldgenDecorate(g, w, cx, cz);
}

int worldgenArea(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz,
                 int radius)
{
	int failed = 0;
	for (int dz = -radius; dz <= radius; dz++)
		for (int dx = -radius; dx <= radius; dx++)
			if (!worldgenColumn(g, s, w, cx + dx, cz + dz))
				failed++;
	return failed;
}
