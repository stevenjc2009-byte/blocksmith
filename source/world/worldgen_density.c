#include "world/worldgen_density.h"

#include "world/chunk.h"
#include "world/genversion.h"
#include "world/noise.h"
#include "world/rng.h"

// The grid geometry is derived from the world's, not chosen independently of it. A change to
// CHUNK_DIM or WORLD_HEIGHT that these did not follow would silently sample the wrong lattice
// and produce a seam at every column border, which looks like a noise bug and is not one.
_Static_assert((GEN_D_GRID_XZ - 1) * GEN_D_CELL_XZ == CHUNK_DIM,
               "the xz sample grid must span exactly one column");
_Static_assert((GEN_D_GRID_Y - 1) * GEN_D_CELL_Y == WORLD_HEIGHT,
               "the y sample grid must span exactly the world height");
_Static_assert(1 << GEN_D_CELL_XZ_SHIFT == GEN_D_CELL_XZ, "xz shift must match the cell size");
_Static_assert(1 << GEN_D_CELL_Y_SHIFT == GEN_D_CELL_Y, "y shift must match the cell size");

// Salts. One per field, for world/worldgen.c's reason: two fBms drawn from the same seed at
// different scales are visibly the same shape, so a selector sharing the low field's seed
// would choose "dramatic" in exactly the places the low field already made tall.
#define SALT_DLOW  0x444C4F57U   // 'DLOW'
#define SALT_DHIGH 0x44484947U   // 'DHIG'
#define SALT_DSEL  0x4453454CU   // 'DSEL'

// ── Working buffers ───────────────────────────────────────────────────────────────────
//
// All static, none on the linear heap, none claimed against WORLD_BUDGET_BYTES (world/
// budget.h) — that budget counts block storage, and none of this outlives a call. Static
// rather than automatic for world/worldgen.c's reason: this runs on the worker thread, whose
// stack is 32 KB (app/worker.c), and 6.4 KB of frame is not something to spend there.
//
// Total added .bss for this generator: 1,700 + 4,096 + 512 + 256 = **6,564 bytes**.

// The coarse lattice: [y][z][x], 16.16 blocks, positive inside the ground.
static int32_t s_grid[GEN_D_GRID_Y][GEN_D_GRID_XZ][GEN_D_GRID_XZ];

// The interpolated result, one bit per block: [y][z] holds a 16-bit mask over x. 4,096 bytes
// for a whole 16 x 16 x 128 column, against 32,768 for a byte per block — and the surface
// pass wants to scan downwards through a column, which a bitmask makes a shift rather than a
// strided memory walk.
static uint16_t s_solid[WORLD_HEIGHT][CHUNK_DIM];

// Topmost solid y + 1 per (x, z), i.e. exactly worldgenHeight()'s convention. 0 where the
// whole column came out empty, which the vertical bias makes impossible but which the fill
// loop must still not read past the end of.
static int16_t s_top[CHUNK_DIM][CHUNK_DIM];

// Slope at each (x, z) in blocks-per-block doubled — see GEN_D_CLIFF_SLOPE_X2.
static uint8_t s_slope[CHUNK_DIM][CHUNK_DIM];

// Which column s_top currently describes, for wgdColumnTops(). -1/-1 would be a real column
// coordinate, so validity is its own flag rather than a sentinel pair.
static int32_t s_top_cx, s_top_cz;
static bool    s_top_valid;

// One chunk's cells, staged and committed in one worldSetChunkAll. Same buffer discipline and
// the same reason as world/worldgen.c's s_gen_flat, kept separate from it only because the
// two generators must not be able to interfere with each other.
static BlockId s_flat[CHUNK_BLOCKS];

// ── Coordinates ───────────────────────────────────────────────────────────────────────
//
// Block coordinate to 16.16 noise coordinate. One shift, no divide — world/worldgen.c's
// genCoord and the ARM11's missing divide instruction, same trick.
static inline fx dCoord(int32_t block, int shift)
{
	return (fx)((int64_t)block << (FX_SHIFT - shift));
}

// ── The biome table (task 16) ─────────────────────────────────────────────────────────
//
// Control points on the EXISTING biome field (world/worldgen.h's GEN_BIOME_*), placed against
// its distribution rather than spread over the nominal [0, 1] range it never fills.
//
// **The distribution this used to quote — "0.075 .. 0.936, median near 0.68" — was wrong.**
// MEASURED 2026-08-25 on a host build (x86-64 gcc -O2 under WSL, linking this tree's
// world/noise.c and sampling worldgenBiome() directly; NOT measured on the ARM11), 13 seeds
// over two independent 2048 x 2048-block windows at stride 8, 851,968 pooled columns each:
// the field runs **0.005 .. 0.987 with median 0.503**, i.e. near-symmetric about 0.5 rather
// than clustered high. See world/worldgen.h's GEN_SAND_BELOW for the full figures, the
// per-seed spreads and why the original 1024x1024 window produced the wrong answer.
//
// **The table is UNCHANGED and was not re-derived.** The corrected statistic invalidates the
// argument that was written up for these values, not the values themselves: they were tuned
// by eye against the terrain they produce, and the amplitudes are pinned by the overhang and
// world-ceiling arithmetic below, which is geometry and independent of the histogram. See
// worldgen_density.h. Anyone re-tuning this table should re-derive it against the corrected
// distribution instead of trusting the percentile labels this comment used to carry.
//
// The four points, and why each is where it is. **The percentile labels below are the
// corrected ones**, from the wide-window measurement above; two of the four were badly wrong:
//
// `amp` is the NOMINAL peak-to-trough range, i.e. what the field would span if the fBm
// reached 0 and 1. It does not: measured over 13 x 13 columns on three seeds the realised
// surface spread is 30 to 33 blocks against nominal amplitudes of 14 to 64, so about half.
// The nominal figures below are quoted as nominal, and the measured ones beside them.
//
//   0x1333 (0.075). Labelled "the measured MINIMUM"; it is not, the measured minimum is
//     0.005. It is the 0.05th percentile — effectively the floor, which is what the entry
//     needs to be, so the value stands and only the label was wrong. Lowland. base 52,
//     amp 14 -> nominal 45..59,
//     entirely below GEN_SEA_LEVEL 64. This is the seabed and the shore, and it is the
//     flattest entry on purpose: the one place the player stands at the waterline is the one
//     place a cliff would be most annoying.
//
//   0x7800 (0.469). Labelled "the measured TENTH PERCENTILE"; it is **the 43rd**, and it
//     selects 42.6 % of columns pooled (37.9 .. 46.8 % per seed), not a tenth. The real tenth
//     percentile is 0.292 (0x4AA9). **Reused, not a new number**: it is
//     already GEN_SAND_BELOW, the sand threshold worldgenIsSandy() applies. Putting the
//     lowland-to-plains control point on exactly that value is what keeps the shape and the
//     material agreeing — the terrain stops being beach-flat at the same place it stops being
//     sand. base 64, amp 28 -> nominal 50..78, straddling sea level, which is what a coastal
//     plain is.
//
//   0xAE14 (0.68). Labelled "the measured MEDIAN"; it is **the 85th percentile** — the
//     measured median is 0.503, which lands between this point and 0x7800. So this is not
//     "what most of the world is": about 85 % of columns fall below it, and the terrain most
//     of the world actually gets is the interpolation between 0x7800 and here rather than
//     this pair. Plains and low hills. base 76, amp 48 -> nominal 52..100. Clear of the
//     water, walkable, with enough relief to be worth walking over.
//
//   0xEF9D (0.936). Labelled "the measured MAXIMUM"; it is the 99.95th percentile, the
//     measured maximum being 0.987. The gap matters only in that a handful of columns per
//     world sit above this point and are clamped to it, which is the intended behaviour for
//     the top entry. Mountains. base 88, amp 64 -> nominal 56..120,
//     realised maximum 94 over the sampled area. Two things bound the top:
//       * the tree pass adds a trunk of up to GEN_TREE_MAX_H 7 plus two canopy layers, so a
//         surface at 116 would put leaves at 125 against a 128-block ceiling. treeInCell()'s
//         own ceiling check is the backstop, and it is why this is safe rather than lucky.
//       * densityCore's noise term is (n - 1/2) * amp: |32768 * 64| is 2.1M, well inside
//         int32, but it is the expression that overflows first if this is raised much.
//
//   Why these amplitudes and not smaller ones. **Overhangs are a function of amplitude and
//   nothing else.** The lattice steps 8 blocks vertically and the bias falls one density-block
//   per world block, so density only rises going up — the precondition for an overhang — where
//   amp * (delta noise over 8 blocks) > 8 blocks. Measured on the lattice, 16,384 steps per
//   seed: at the previous amplitudes (10/18/34/58) seeds 4242 and 90210 had a maximum upward
//   step of -2.68 and -0.56 blocks, i.e. **no overhang was geometrically possible anywhere in
//   those worlds**. At these amplitudes the mountainous seed reaches +16.76 and 9.99% of
//   lattice steps rise. Overhangs remain a mountain feature and are absent from plains, which
//   is both what Beta does and what the arithmetic above forces.
//
// Monotonic in both columns by design — higher biome value means higher and more dramatic —
// and asserted as such in the suite, because a table that dipped would make one biome a
// basin inside another for no reason a player could read.
static const GenBiomePoint s_biome_table[GEN_D_BIOME_POINTS] = {
	{0x00001333, 52, 14},
	{0x00007800, 64, 28},
	{0x0000AE14, 76, 48},
	{0x0000EF9D, 88, 64},
};

void wgdBiomeParams(fx biome, int* base_h, int* amp)
{
	const GenBiomePoint* t = s_biome_table;

	if (biome <= t[0].biome) {
		if (base_h) *base_h = t[0].base_h;
		if (amp)    *amp    = t[0].amp;
		return;
	}
	for (int i = 1; i < GEN_D_BIOME_POINTS; i++) {
		if (biome > t[i].biome)
			continue;

		// Linear between the two control points. The span is a runtime value so this is a
		// real divide — the only one in the generator, and it is paid 25 times per column
		// (once per xz grid point), never per block. Putting it here rather than in the
		// interpolation inner loop is the whole reason the biome is resolved on the coarse
		// grid instead of per block.
		const int32_t span = t[i].biome - t[i - 1].biome;
		const int32_t into = biome - t[i - 1].biome;
		if (base_h)
			*base_h = t[i - 1].base_h +
			          (int)(((int64_t)(t[i].base_h - t[i - 1].base_h) * into) / span);
		if (amp)
			*amp = t[i - 1].amp +
			       (int)(((int64_t)(t[i].amp - t[i - 1].amp) * into) / span);
		return;
	}

	if (base_h) *base_h = t[GEN_D_BIOME_POINTS - 1].base_h;
	if (amp)    *amp    = t[GEN_D_BIOME_POINTS - 1].amp;
}

// ── One density sample ────────────────────────────────────────────────────────────────

// The selector, shaped: centred on the field's measured median, amplified so it saturates,
// clamped to [0, FX_ONE]. See GEN_D_SEL_GAIN in the header for why saturation is the point.
static inline fx selectorAt(uint32_t seed, int32_t x, int y, int32_t z)
{
	const fx s = noiseFbm3(seed,
	                       dCoord(x, GEN_D_SEL_SHIFT_XZ),
	                       dCoord(y, GEN_D_SEL_SHIFT_Y),
	                       dCoord(z, GEN_D_SEL_SHIFT_XZ),
	                       GEN_D_SEL_OCTAVES);
	const int32_t t = FX_ONE / 2 + (s - GEN_D_SEL_CENTRE) * GEN_D_SEL_GAIN;
	if (t < 0)       return 0;
	if (t > FX_ONE)  return FX_ONE;
	return (fx)t;
}

// The density at one lattice point, given the biome pair already resolved for that point.
//
// Split out from wgdDensityAt so the column builder can resolve the biome once per xz grid
// point (25 fBm2 evaluations per column) instead of once per lattice point (425).
static inline int32_t densityCore(const WorldGen* g, int32_t x, int y, int32_t z,
                                  int base_h, int amp)
{
	const uint32_t s_low  = rngMix(g->seed ^ SALT_DLOW);
	const uint32_t s_high = rngMix(g->seed ^ SALT_DHIGH);
	const uint32_t s_sel  = rngMix(g->seed ^ SALT_DSEL);

	const fx lo = noiseFbm3(s_low,
	                        dCoord(x, GEN_D_LOW_SHIFT_XZ),
	                        dCoord(y, GEN_D_LOW_SHIFT_Y),
	                        dCoord(z, GEN_D_LOW_SHIFT_XZ),
	                        GEN_D_LOW_OCTAVES);
	const fx hi = noiseFbm3(s_high,
	                        dCoord(x, GEN_D_HIGH_SHIFT_XZ),
	                        dCoord(y, GEN_D_HIGH_SHIFT_Y),
	                        dCoord(z, GEN_D_HIGH_SHIFT_XZ),
	                        GEN_D_HIGH_OCTAVES);
	const fx t  = selectorAt(s_sel, x, y, z);

	// lerp(lo, hi, t). Both are [0, FX_ONE] and t is [0, FX_ONE], so the product is at most
	// 2^32 and must be widened; the shift brings it back to 16.16.
	const fx n = lo + (fx)(((int64_t)(hi - lo) * t) >> FX_SHIFT);

	// (n - 1/2) * amp gives the displacement in blocks, in 16.16: n - 1/2 is +/- 0.5 in fx and
	// amp is a plain integer count of blocks, so the product is already fx-scaled blocks.
	// |32768 * 64| is 2.1M, comfortably inside int32 — but written through int64 anyway
	// because this is exactly the expression someone raises the amplitude in later.
	const int32_t noise_blocks = (int32_t)((int64_t)(n - FX_ONE / 2) * amp);

	// The vertical bias: one density-block per world block. See the header.
	return noise_blocks + (((int32_t)base_h - y) << FX_SHIFT);
}

int32_t wgdDensityAt(const WorldGen* g, int32_t x, int y, int32_t z)
{
	int base_h, amp;
	wgdBiomeParams(worldgenBiome(g, x, z), &base_h, &amp);
	return densityCore(g, x, y, z, base_h, amp);
}

// ── Trilinear interpolation ───────────────────────────────────────────────────────────
//
// Lerps by a fraction over a power-of-two span, so the divide is a shift. `t` is 0..(1<<sh)-1.
//
// Range: the grid holds at most about +/- 128 blocks in 16.16, i.e. +/- 8.4M, so (b - a) is at
// most ~17M and t at most 7 — 117M, well inside int32. Asserted implicitly by the fact that
// nothing in the biome table can produce a density outside +/- (WORLD_HEIGHT + amp/2) blocks.
static inline int32_t lerpSh(int32_t a, int32_t b, int t, int sh)
{
	return a + (((b - a) * t) >> sh);
}

// Fills s_solid for one column from s_grid.
//
// Structured as cell-at-a-time with the y, then z, then x interpolations hoisted out of the
// loops that do not need them — the same nesting Beta uses, and for the same reason: done
// naively this is seven lerps for every one of the 32,768 blocks, and hoisted it is about
// 1.3. On a 268 MHz ARM11 that difference is the whole feasibility of the design.
static void interpolateColumn(void)
{
	for (int cy = 0; cy < GEN_D_GRID_Y - 1; cy++) {
		for (int cz = 0; cz < GEN_D_GRID_XZ - 1; cz++) {
			for (int cx = 0; cx < GEN_D_GRID_XZ - 1; cx++) {
				const int32_t c000 = s_grid[cy][cz][cx];
				const int32_t c001 = s_grid[cy][cz][cx + 1];
				const int32_t c010 = s_grid[cy][cz + 1][cx];
				const int32_t c011 = s_grid[cy][cz + 1][cx + 1];
				const int32_t c100 = s_grid[cy + 1][cz][cx];
				const int32_t c101 = s_grid[cy + 1][cz][cx + 1];
				const int32_t c110 = s_grid[cy + 1][cz + 1][cx];
				const int32_t c111 = s_grid[cy + 1][cz + 1][cx + 1];

				for (int ly = 0; ly < GEN_D_CELL_Y; ly++) {
					// Interpolate the cell's four vertical edges once for this y slice.
					const int32_t e00 = lerpSh(c000, c100, ly, GEN_D_CELL_Y_SHIFT);
					const int32_t e01 = lerpSh(c001, c101, ly, GEN_D_CELL_Y_SHIFT);
					const int32_t e10 = lerpSh(c010, c110, ly, GEN_D_CELL_Y_SHIFT);
					const int32_t e11 = lerpSh(c011, c111, ly, GEN_D_CELL_Y_SHIFT);

					const int y = cy * GEN_D_CELL_Y + ly;

					for (int lz = 0; lz < GEN_D_CELL_XZ; lz++) {
						// ...then the cell's two z edges once for this z row.
						const int32_t r0 = lerpSh(e00, e10, lz, GEN_D_CELL_XZ_SHIFT);
						const int32_t r1 = lerpSh(e01, e11, lz, GEN_D_CELL_XZ_SHIFT);

						const int z = cz * GEN_D_CELL_XZ + lz;
						uint16_t mask = s_solid[y][z];

						for (int lx = 0; lx < GEN_D_CELL_XZ; lx++) {
							// One lerp per block, and nothing else.
							const int32_t d = lerpSh(r0, r1, lx, GEN_D_CELL_XZ_SHIFT);
							const int x = cx * GEN_D_CELL_XZ + lx;
							if (d > 0)
								mask |= (uint16_t)(1u << x);
						}
						s_solid[y][z] = mask;
					}
				}
			}
		}
	}
}

// Evaluates the whole 5 x 5 x 17 lattice for column (cx, cz) into s_grid.
static void buildGrid(const WorldGen* g, int32_t cx, int32_t cz)
{
	// The biome pair is resolved once per xz grid point — 25 fBm2 evaluations per column,
	// against 425 if it were resolved per lattice point. It varies over 128 blocks
	// (GEN_BIOME_SHIFT), so a 4-block sample spacing is already far finer than the field.
	int base_h[GEN_D_GRID_XZ][GEN_D_GRID_XZ];
	int amp[GEN_D_GRID_XZ][GEN_D_GRID_XZ];

	for (int j = 0; j < GEN_D_GRID_XZ; j++) {
		for (int i = 0; i < GEN_D_GRID_XZ; i++) {
			const int32_t x = cx * CHUNK_DIM + i * GEN_D_CELL_XZ;
			const int32_t z = cz * CHUNK_DIM + j * GEN_D_CELL_XZ;
			wgdBiomeParams(worldgenBiome(g, x, z), &base_h[j][i], &amp[j][i]);
		}
	}

	for (int k = 0; k < GEN_D_GRID_Y; k++) {
		const int y = k * GEN_D_CELL_Y;
		for (int j = 0; j < GEN_D_GRID_XZ; j++) {
			for (int i = 0; i < GEN_D_GRID_XZ; i++) {
				const int32_t x = cx * CHUNK_DIM + i * GEN_D_CELL_XZ;
				const int32_t z = cz * CHUNK_DIM + j * GEN_D_CELL_XZ;
				s_grid[k][j][i] = densityCore(g, x, y, z, base_h[j][i], amp[j][i]);
			}
		}
	}
}

// ── Slope (task 20) ───────────────────────────────────────────────────────────────────

static void buildSlope(void)
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			// Central difference in the interior, one-sided on the border ring. Both are real
			// local slopes; see worldgen_density.h for why the border is not fabricated.
			const int xa = (x > 0) ? x - 1 : x;
			const int xb = (x < CHUNK_DIM - 1) ? x + 1 : x;
			const int za = (z > 0) ? z - 1 : z;
			const int zb = (z < CHUNK_DIM - 1) ? z + 1 : z;

			// Doubled units: a central difference already spans two blocks, and a one-sided
			// one is doubled to match rather than halving the central one, which would throw
			// away the odd values on a CPU with no divide.
			int dx = s_top[z][xb] - s_top[z][xa];
			int dz = s_top[zb][x] - s_top[za][x];
			if (xb - xa == 1) dx *= 2;
			if (zb - za == 1) dz *= 2;
			if (dx < 0) dx = -dx;
			if (dz < 0) dz = -dz;

			// The larger of the two axes, not their sum: a slope that is steep along one axis
			// and flat along the other is a cliff, and adding them would call a gentle
			// diagonal one too.
			int s = (dx > dz) ? dx : dz;
			if (s > 255) s = 255;
			s_slope[z][x] = (uint8_t)s;
		}
	}
}

// ── The surface pass (task 20) ────────────────────────────────────────────────────────
//
// Depth and slope, plus the waterline. Applied to the topmost solid run only — every run
// below it is stone — because a surface material is about being open to the sky, and grass
// growing on the floor of a sealed cave is the classic way this pass goes wrong. Beta reaches
// the same result by running its surface pass before it carves caves; doing it by exposure
// costs nothing here and needs no ordering rule.
static inline BlockId surfaceBlock(int depth, int top_y, int slope_x2, bool sandy)
{
	// A cliff face is bare rock all the way to the top. Checked first because it overrides
	// both the beach and the grass rules: sand on a vertical wall is worse than grass on one.
	if (slope_x2 >= GEN_D_CLIFF_SLOPE_X2)
		return BLOCK_STONE;

	// Beach. Everything at or just above the waterline is sand, and so is the whole sandy
	// biome — one rule for both, since a shoreline and a desert want the same material and
	// worldgenIsSandy() is already the game's answer to the second half of it.
	if (sandy || top_y <= GEN_SEA_LEVEL + GEN_D_BEACH_ABOVE)
		return (depth <= GEN_DIRT_DEPTH) ? BLOCK_SAND : BLOCK_STONE;

	if (depth == 0)               return BLOCK_GRASS;
	if (depth <= GEN_DIRT_DEPTH)  return BLOCK_DIRT;
	return BLOCK_STONE;
}

// ── Height ────────────────────────────────────────────────────────────────────────────

int wgdHeight(const WorldGen* g, int32_t x, int32_t z)
{
	// The column this block belongs to, floored: >> then << rather than a divide, so that
	// x = -1 lands in column -1 and not column 0. Same rule as world.c uses everywhere.
	const int32_t bx = (x >> 4) << 4;
	const int32_t bz = (z >> 4) << 4;
	const int lx = (int)(x - bx), lz = (int)(z - bz);

	// The one cell of the lattice that surrounds (x, z) — four columns of 17 samples.
	const int ci = lx >> GEN_D_CELL_XZ_SHIFT;
	const int cj = lz >> GEN_D_CELL_XZ_SHIFT;
	const int fx_ = lx & (GEN_D_CELL_XZ - 1);
	const int fz_ = lz & (GEN_D_CELL_XZ - 1);

	int32_t edge[GEN_D_GRID_Y][2][2];
	for (int di = 0; di < 2; di++) {
		for (int dj = 0; dj < 2; dj++) {
			const int32_t gx = bx + (ci + di) * GEN_D_CELL_XZ;
			const int32_t gz = bz + (cj + dj) * GEN_D_CELL_XZ;
			int base_h, amp;
			wgdBiomeParams(worldgenBiome(g, gx, gz), &base_h, &amp);
			for (int k = 0; k < GEN_D_GRID_Y; k++)
				edge[k][dj][di] = densityCore(g, gx, k * GEN_D_CELL_Y, gz, base_h, amp);
		}
	}

	// Downwards from the ceiling, in exactly the arithmetic interpolateColumn uses, so that a
	// standalone height query and a generated column cannot disagree.
	for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
		const int k  = y >> GEN_D_CELL_Y_SHIFT;
		const int fy = y & (GEN_D_CELL_Y - 1);

		const int32_t e00 = lerpSh(edge[k][0][0], edge[k + 1][0][0], fy, GEN_D_CELL_Y_SHIFT);
		const int32_t e01 = lerpSh(edge[k][0][1], edge[k + 1][0][1], fy, GEN_D_CELL_Y_SHIFT);
		const int32_t e10 = lerpSh(edge[k][1][0], edge[k + 1][1][0], fy, GEN_D_CELL_Y_SHIFT);
		const int32_t e11 = lerpSh(edge[k][1][1], edge[k + 1][1][1], fy, GEN_D_CELL_Y_SHIFT);

		const int32_t r0 = lerpSh(e00, e10, fz_, GEN_D_CELL_XZ_SHIFT);
		const int32_t r1 = lerpSh(e01, e11, fz_, GEN_D_CELL_XZ_SHIFT);
		const int32_t d  = lerpSh(r0, r1, fx_, GEN_D_CELL_XZ_SHIFT);

		if (d > 0)
			return y + 1;
	}

	// Unreachable while the vertical bias holds — at y = 0 the bias alone is +base_h blocks
	// and no amplitude in the table can cancel it — but a height of 1 is the same answer the
	// legacy generator clamps to, so a caller never sees a zero it would divide by.
	return 1;
}

// ── The column ────────────────────────────────────────────────────────────────────────

bool wgdColumn(const WorldGen* g, World* w, int32_t cx, int32_t cz)
{
	buildGrid(g, cx, cz);

	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			s_solid[y][z] = 0;
	interpolateColumn();

	// Topmost solid per (x, z). Scanned down from the ceiling, which is the same convention
	// wgdHeight uses and the same one the rest of the game reads heights in.
	int max_h = 0;
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			int top = 0;
			for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
				if (s_solid[y][z] & (uint16_t)(1u << x)) { top = y + 1; break; }
			}
			s_top[z][x] = (int16_t)top;
			if (top > max_h) max_h = top;
		}
	}
	s_top_cx    = cx;
	s_top_cz    = cz;
	s_top_valid = true;
	buildSlope();

	// The sandy-biome answer per (x, z), from the SAME worldgenIsSandy() the legacy generator
	// and the tree pass ask — one rule, one place, so the material and the biome cannot drift.
	bool sandy[CHUNK_DIM][CHUNK_DIM];
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			sandy[z][x] = worldgenIsSandy(g, cx * CHUNK_DIM + x, cz * CHUNK_DIM + z);

	if (!worldColumnCreate(w, cx, cz))
		return false;

	// Downwards, so that "how deep into this run am I" and "is this run the one open to the
	// sky" are both answered by a counter carried from the block above rather than by a second
	// scan. `run` is the depth into the current solid run, or -1 in air; `exposed` goes false
	// the moment the first run ends and never comes back, which is what confines the surface
	// materials to the top run.
	int8_t run[CHUNK_DIM][CHUNK_DIM];
	bool   exposed[CHUNK_DIM][CHUNK_DIM];

	// ── The sea (task 17) ─────────────────────────────────────────────────────────────
	//
	// **The rule: from GEN_SEA_LEVEL - 1 downwards, air becomes water until the first cell
	// that is not air, and that column is then finished with.** Nothing else. `sea[z][x]`
	// is "this column is still filling", and it can only ever go true -> false.
	//
	// Why this rule and not "every air cell below sea level". The density field carves a real
	// cave system underneath the terrain — measured at 5-8 % of the underground — and every
	// one of those cells is air below y = 64. Filling all air below the line would flood the
	// entire cave system of every world, which is not an ocean, it is a bug that happens to
	// be blue. Stopping at the first solid leaves a sealed cave dry and fills exactly what is
	// open to the sky, which is what an ocean and a lake are.
	//
	// Why it needs no flood fill and no second pass. The commit loop below already walks each
	// column downwards from the ceiling, so "the first solid at or under the waterline" is a
	// single flag carried down that walk — the same trick `run` and `exposed` already use. It
	// costs one bool per (x, z), one compare per y, and no extra pass over anything.
	//
	// It is also why the tree gate in world/worldgen.c refuses to plant at or below the
	// waterline: a trunk standing in an ocean column would be the first non-air cell of that
	// walk and would leave a dry shaft around itself.
	bool sea[CHUNK_DIM][CHUNK_DIM];

	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++) {
			run[z][x] = -1; exposed[z][x] = true; sea[z][x] = true;
		}

	const int32_t wx0 = cx * CHUNK_DIM, wz0 = cz * CHUNK_DIM;

	for (int cy = COLUMN_CHUNKS - 1; cy >= 0; cy--) {
		const int y0 = cy * CHUNK_DIM;

		// Entirely above the highest solid block anywhere in this column: every cell is air,
		// so the chunk is left unallocated. An absent chunk reads as air and costs a NULL
		// pointer — the same saving world/worldgen.c takes, and it matters more here because
		// the density generator's mountains and its seabed can be 60 blocks apart.
		//
		// **The second half of the test is task 17's.** A chunk above the whole column's
		// ground but still under the waterline is an ocean chunk: all air by the density
		// field and all water by the fill. Skipping it on `y0 >= max_h` alone — which is what
		// this line said before water existed — would leave the top half of every ocean empty
		// down to the seabed's own chunk.
		if (y0 >= max_h && y0 >= GEN_SEA_LEVEL)
			continue;

		bool any = false;
		for (int i = 0; i < CHUNK_BLOCKS; i++) s_flat[i] = BLOCK_AIR;

		for (int ly = CHUNK_DIM - 1; ly >= 0; ly--) {
			const int y = y0 + ly;
			// Hoisted out of the 256 cells of this slice: one compare per y, not per block.
			const bool under_line = (y < GEN_SEA_LEVEL);
			for (int z = 0; z < CHUNK_DIM; z++) {
				const uint16_t mask = s_solid[y][z];
				for (int x = 0; x < CHUNK_DIM; x++) {
					if (!(mask & (uint16_t)(1u << x))) {
						// Air ends the run. Only a run that has actually started can close
						// one, so the empty sky above the terrain does not spend the
						// exposure before the ground is reached.
						if (run[z][x] >= 0) exposed[z][x] = false;
						run[z][x] = -1;
						if (under_line && sea[z][x]) {
							s_flat[chunkIndex(x, ly, z)] = BLOCK_WATER;
							any = true;
						}
						continue;
					}

					const int depth = (run[z][x] < 0) ? 0 : run[z][x] + 1;

					// The cave pass. **The existing one** — world/worldgen.c's
					// worldgenIsCave(), the same two fBms with the same salts and the same
					// GEN_CAVE_MIN_DEPTH rule the legacy generator uses. The density field
					// produces overhangs and arches at the surface but it does not produce
					// the long flat tunnel systems that a squashed 3D band field does, and
					// dropping those would have quietly deleted a shipped feature from every
					// new world. Reused rather than reinvented so that a change to the cave
					// shape lands in both generators at once.
					// Depth is measured from the column's own top surface, not from the
					// start of this solid run — exactly as the legacy fill loop measures it.
					// Measuring it from the run would let the first cave reset the counter
					// and forbid another for five blocks under it, which turns a tunnel
					// system into a scatter of isolated pockets. Measured: run-depth gave
					// 10,360 air cells under the surface against 27,312 in legacy.
					const int d_surf = s_top[z][x] - 1 - y;
					if (d_surf >= GEN_CAVE_MIN_DEPTH &&
					    worldgenIsCave(g, wx0 + x, y, wz0 + z)) {
						exposed[z][x] = false;
						run[z][x] = -1;
						// A carved cell is air, so the fill treats it as air. It is almost
						// never reached with sea[z][x] still true — a cave is at least
						// GEN_CAVE_MIN_DEPTH below the column's own top surface, and that
						// top surface is never carved, so the walk has normally already
						// stopped on it. The case that DOES reach here is a cave breaking
						// into the underside of an overhang that stands open to the sea, and
						// water belonging in it is the same answer as for any other air cell
						// the walk can still see.
						if (under_line && sea[z][x]) {
							s_flat[chunkIndex(x, ly, z)] = BLOCK_WATER;
							any = true;
						}
						continue;
					}

					// Saturating, because the counter is only ever compared against
					// GEN_DIRT_DEPTH and a run can be 100 blocks deep in a mountain.
					run[z][x] = (int8_t)((depth > 100) ? 100 : depth);

					// Solid, at or under the waterline: this is the cell the downward walk
					// was looking for, and everything below it in this column is sealed off
					// from the sea whatever it turns out to be.
					if (under_line) sea[z][x] = false;

					s_flat[chunkIndex(x, ly, z)] =
						exposed[z][x]
							? surfaceBlock(depth, s_top[z][x], s_slope[z][x], sandy[z][x])
							: BLOCK_STONE;
					any = true;
				}
			}
		}

		// A chunk that came out entirely air is left unallocated for the same reason the sky
		// chunks above are. This is not the same test as `y0 >= max_h`: max_h is the tallest
		// point in the whole column, so a chunk under a neighbouring peak but above this
		// column's own ground reaches here and is still empty.
		if (!any)
			continue;

		if (!worldSetChunkAll(w, cx, cy, cz, s_flat))
			return false;
	}

	return true;
}

const int16_t* wgdColumnTops(int32_t cx, int32_t cz)
{
	if (!s_top_valid || s_top_cx != cx || s_top_cz != cz)
		return NULL;
	return &s_top[0][0];
}
