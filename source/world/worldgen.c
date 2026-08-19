#include "world/worldgen.h"

#include "world/noise.h"
#include "world/rng.h"

// Step 9.2a/9.1b. A flat staging buffer for one chunk's worth of cells, built here and then
// committed in one shot via worldSetChunkAll — never CHUNK_BLOCKS individual worldSet calls,
// which would promote a freshly generated chunk's storage cell by cell (UNIFORM ->
// PALETTE4 -> RAW) for a chunk whose final content is already fully known before the first
// write. Static rather than a stack array, following worker.c's s_load_buf convention: this
// runs on whatever thread calls worldgenColumn, and 4 KB is not something to risk on a stack
// that also has to hold the rest of a generation call's frames.
static BlockId s_gen_flat[CHUNK_BLOCKS];

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
#define SALT_CAVE   0x43415645U   // 'CAVE'
#define SALT_CAVE2  0x43415632U   // 'CAV2'

void worldgenInit(WorldGen* g, uint32_t seed)
{
	// The seed is mixed once here rather than used raw, so that the obvious human seeds —
	// 0, 1, 2 — give unrelated worlds instead of three near-identical ones. rngHash2's
	// per-axis mixing hides most of that, but not the first octave's low bits.
	g->seed = rngMix(seed ^ 0x424C4B53U);   // 'BLKS'
}

fx worldgenBiome(const WorldGen* g, int32_t x, int32_t z)
{
	return noiseFbm2(rngMix(g->seed ^ SALT_BIOME), biomeCoord(x), biomeCoord(z),
	                 GEN_BIOME_OCTAVES);
}

bool worldgenIsSandy(const WorldGen* g, int32_t x, int32_t z)
{
	return worldgenBiome(g, x, z) < GEN_SAND_BELOW;
}

static inline bool inCaveBand(fx v)
{
	return v >= GEN_CAVE_CENTRE - GEN_CAVE_HALF && v <= GEN_CAVE_CENTRE + GEN_CAVE_HALF;
}

bool worldgenIsCave(const WorldGen* g, int32_t x, int y, int32_t z)
{
	if (y < GEN_CAVE_FLOOR)
		return false;

	const fx cx = caveCoordXZ(x), cy = caveCoordY(y), cz = caveCoordXZ(z);

	// The first field is evaluated for every underground block and the second only for the
	// ~22 % that survive it. Ordering the test this way is worth about a third of the whole
	// pass, and it is free: the two fields are independent, so either order gives the same
	// answer for the same block.
	if (!inCaveBand(noiseFbm3(rngMix(g->seed ^ SALT_CAVE), cx, cy, cz, GEN_CAVE_OCTAVES)))
		return false;

	return inCaveBand(noiseFbm3(rngMix(g->seed ^ SALT_CAVE2), cx, cy, cz, GEN_CAVE_OCTAVES));
}

int worldgenHeight(const WorldGen* g, int32_t x, int32_t z)
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
	int32_t x, z;    // world block coordinates of the trunk
	int     ground;  // y of the first air above the ground, i.e. the trunk's base
	int     trunk;   // trunk blocks
} Tree;

static Tree treeInCell(const WorldGen* g, int32_t tcx, int32_t tcz)
{
	Tree t = {0};

	const uint32_t h = rngHash2(rngMix(g->seed ^ SALT_TREE), tcx, tcz);
	if ((h & 0xFFu) >= (uint32_t)GEN_TREE_CHANCE)
		return t;

	// Offset inside the cell, kept away from the very edge so that two trees in adjacent
	// cells cannot stand shoulder to shoulder: with GEN_TREE_CELL 8 and the offset in
	// 1..6, the closest two trunks can be is three blocks, which is one more than a
	// canopy's reach on each side.
	t.x = tcx * GEN_TREE_CELL + 1 + (int32_t)((h >> 8) % (GEN_TREE_CELL - 2));
	t.z = tcz * GEN_TREE_CELL + 1 + (int32_t)((h >> 13) % (GEN_TREE_CELL - 2));

	// Sand grows nothing. Asked at the trunk only: a canopy overhanging a dune edge is
	// what a tree on a shoreline actually looks like.
	if (worldgenIsSandy(g, t.x, t.z))
		return t;

	t.ground = worldgenHeight(g, t.x, t.z);
	t.trunk  = GEN_TREE_MIN_H +
	           (int)((h >> 18) % (uint32_t)(GEN_TREE_MAX_H - GEN_TREE_MIN_H + 1));

	// Nothing may be written above the world ceiling. This cannot fire at the current
	// surface band — 68 + 7 + 1 is 76 against a 128-block world — but the band is a tuning
	// constant and the canopy loop below writes without re-checking.
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

bool worldgenDecorate(const WorldGen* g, World* w, int32_t cx, int32_t cz)
{
	// Tree cells that can reach this column. The column covers blocks [cx*16, cx*16+15]
	// and a canopy reaches GEN_TREE_RADIUS further, so the block span is exact; the extra
	// cell at each end covers integer division truncating towards zero on the negative
	// side, which is cheaper than writing a floor-divide for a bound that is scanned once
	// per column and costs one hash per extra cell.
	const int32_t x0 = (cx * CHUNK_DIM - GEN_TREE_RADIUS) / GEN_TREE_CELL - 1;
	const int32_t x1 = (cx * CHUNK_DIM + CHUNK_DIM - 1 + GEN_TREE_RADIUS) / GEN_TREE_CELL + 1;
	const int32_t z0 = (cz * CHUNK_DIM - GEN_TREE_RADIUS) / GEN_TREE_CELL - 1;
	const int32_t z1 = (cz * CHUNK_DIM + CHUNK_DIM - 1 + GEN_TREE_RADIUS) / GEN_TREE_CELL + 1;

	bool ok = true;
	for (int32_t tcz = z0; tcz <= z1; tcz++) {
		for (int32_t tcx = x0; tcx <= x1; tcx++) {
			const Tree t = treeInCell(g, tcx, tcz);
			if (!t.exists)
				continue;

			// Trunk. Written over whatever is there, so a trunk cannot be hollowed out by
			// a canopy block from a neighbouring tree that happened to be placed first.
			for (int i = 0; i < t.trunk; i++)
				ok &= treePut(w, cx, cz, t.x, t.ground + i, t.z, BLOCK_WOOD, false);

			// Canopy. Two wide layers around the top of the trunk and two narrow ones
			// above it, with the corners of the wide layers left out so the silhouette is
			// round-ish rather than a cube on a stick.
			const int top = t.ground + t.trunk;   // first y above the trunk
			for (int dy = -2; dy <= 1; dy++) {
				const int r = (dy <= -1) ? GEN_TREE_RADIUS : 1;
				for (int dz = -r; dz <= r; dz++) {
					for (int dx = -r; dx <= r; dx++) {
						if (r == GEN_TREE_RADIUS && dx * dx + dz * dz > r * r)
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

bool worldgenColumn(const WorldGen* g, World* w, int32_t cx, int32_t cz)
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
			for (int i = 0; i < CHUNK_BLOCKS; i++) s_gen_flat[i] = BLOCK_STONE;
			for (int lz = 0; lz < CHUNK_DIM; lz++)
				for (int lx = 0; lx < CHUNK_DIM; lx++)
					for (int ly = 0; ly < CHUNK_DIM; ly++)
						if (worldgenIsCave(g, cx * CHUNK_DIM + lx, y0 + ly,
						                   cz * CHUNK_DIM + lz))
							s_gen_flat[chunkIndex(lx, ly, lz)] = BLOCK_AIR;
			if (!worldSetChunkAll(w, cx, cy, cz, s_gen_flat))
				return false;
			continue;
		}

		for (int i = 0; i < CHUNK_BLOCKS; i++) s_gen_flat[i] = BLOCK_AIR;
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
					    worldgenIsCave(g, x, y0 + ly, z))
						continue;
					s_gen_flat[chunkIndex(lx, ly, lz)] = blockAtDepth(depth, sandy[lz][lx]);
				}
			}
		}
		if (!worldSetChunkAll(w, cx, cy, cz, s_gen_flat))
			return false;
	}

	return worldgenDecorate(g, w, cx, cz);
}

int worldgenArea(const WorldGen* g, World* w, int32_t cx, int32_t cz, int radius)
{
	int failed = 0;
	for (int dz = -radius; dz <= radius; dz++)
		for (int dx = -radius; dx <= radius; dx++)
			if (!worldgenColumn(g, w, cx + dx, cz + dz))
				failed++;
	return failed;
}
