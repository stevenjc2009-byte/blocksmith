#include "world/worldgen.h"

#include "world/noise.h"
#include "world/rng.h"

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

void worldgenInit(WorldGen* g, uint32_t seed)
{
	// The seed is mixed once here rather than used raw, so that the obvious human seeds —
	// 0, 1, 2 — give unrelated worlds instead of three near-identical ones. rngHash2's
	// per-axis mixing hides most of that, but not the first octave's low bits.
	g->seed = rngMix(seed ^ 0x424C4B53U);   // 'BLKS'
}

int worldgenHeight(const WorldGen* g, int32_t x, int32_t z)
{
	const fx n = noiseFbm2(g->seed, genCoord(x), genCoord(z), GEN_OCTAVES);

	// n is [0, FX_ONE]; scale it into the surface band. The multiply is 64-bit because
	// FX_ONE * GEN_SURFACE_RANGE is 1.8 million — fine in 32 bits today, but this is
	// exactly the expression someone widens the range in later.
	const int h = GEN_SURFACE_MIN + (int)(((int64_t)n * GEN_SURFACE_RANGE) >> FX_SHIFT);

	// Clamped even though the arithmetic cannot exceed the band, because the cost is one
	// compare per column and the failure it prevents is a write past the end of a chunk.
	if (h < 1) return 1;
	if (h > WORLD_HEIGHT - 1) return WORLD_HEIGHT - 1;
	return h;
}

// The block at a given depth below the surface. Kept separate from the fill loop so the
// layering is one readable rule rather than three nested conditions inside a triple loop.
static inline BlockId blockAtDepth(int depth)
{
	if (depth == 0) return BLOCK_GRASS;                  // the top solid block
	if (depth <= GEN_DIRT_DEPTH) return BLOCK_DIRT;
	return BLOCK_STONE;
}

bool worldgenColumn(const WorldGen* g, World* w, int32_t cx, int32_t cz)
{
	// 256 heights, computed once for the whole 8-chunk stack. Doing this per chunk would
	// evaluate the same fBm up to eight times for the same answer, and the fBm is the
	// expensive part of generation.
	int height[CHUNK_DIM][CHUNK_DIM];
	int max_h = 0;
	int min_h = WORLD_HEIGHT;
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int h = worldgenHeight(g, cx * CHUNK_DIM + lx, cz * CHUNK_DIM + lz);
			height[lz][lx] = h;
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

		Chunk* c = worldChunkCreate(w, cx, cy, cz);
		if (!c)
			return false;

		// Entirely below the lowest ground: solid stone, written with one memset instead
		// of 4,096 stores. Worth the special case because at these surface heights it is
		// the common chunk — two or three of every column's eight.
		if (y0 + CHUNK_DIM <= min_h - GEN_DIRT_DEPTH - 1) {
			chunkClear(c, BLOCK_STONE);
			continue;
		}

		chunkClear(c, BLOCK_AIR);
		for (int lz = 0; lz < CHUNK_DIM; lz++) {
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				const int h = height[lz][lx];
				// Only the part of this chunk that is underground gets written; the loop
				// stops at the surface rather than testing every one of the 16 cells.
				int top = h - 1 - y0;                    // local y of the top solid block
				if (top > CHUNK_DIM - 1) top = CHUNK_DIM - 1;
				for (int ly = 0; ly <= top; ly++)
					c->blocks[chunkIndex(lx, ly, lz)] = blockAtDepth(h - 1 - (y0 + ly));
			}
		}
	}

	return true;
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
