// v1.7.0 tasks 15, 16 and 20. The Beta 1.7.3-shaped density-field terrain generator.
//
// Reached only from world/worldgen.c's dispatch, and only for a world stamped
// GEN_VERSION_DENSITY (world/genversion.h). A world stamped GEN_VERSION_LEGACY never enters
// this file at all, which is what makes "existing worlds keep their terrain" a structural
// property rather than a promise.
//
// ── What a density field is, and why it is not a heightmap ────────────────────────────
//
// The legacy generator asks one question per (x, z) — how high is the ground — and fills
// straight down. That can only ever produce a single-valued surface: no overhangs, no
// natural arches, no cliff that leans out over you, and caves only as a separate carve
// bolted on afterwards.
//
// A density field asks a question per **block**: how far inside the ground is this cell.
// Solid where the answer is positive, air where it is negative. The surface is wherever the
// field crosses zero, and a field that varies in y can cross zero more than once above the
// same (x, z) — which is exactly what an overhang, an arch and a cave are.
//
// The price is that the question is now asked 32,768 times per column instead of 256, and
// the noise that answers it is 3D. Beta 1.7.3's answer to that cost is the reason for the
// odd-looking grid below, and it is the whole performance design of this file:
//
// ── The 5 x 5 x 17 grid ───────────────────────────────────────────────────────────────
//
// The noise is evaluated on a **coarse lattice** — 5 x 5 x 17 points spanning one column —
// and everything between those points is **trilinearly interpolated**. 425 noise samples
// stand in for 32,768, and the interpolation is adds and shifts.
//
// The dimensions are forced, not chosen. A column is 16 x 16 x 128 blocks, so:
//   * 5 points across 16 blocks = 4 cells of 4 blocks. The 5th point sits at local x = 16,
//     which is the *neighbouring* column's local x = 0 — the same world coordinate, so the
//     same noise value, so the two columns' grids tile with no seam. That is not a happy
//     accident, it is why the grid is 5 and not 4.
//   * 17 points across 128 blocks = 16 cells of 8 blocks, and the 17th point closes the top.
//
// Interpolating this coarsely is also what makes the terrain *readable*. Sampling 3D noise
// per block gives a sponge; sampling it every 4 blocks horizontally and every 8 vertically
// and smoothing between gives broad shapes with the fine detail filtered out, which is what
// Beta terrain actually looks like.
//
// **Nothing here is ported.** The shape — two limit fields, a selector, a vertical bias, a
// coarse grid, trilinear interpolation — is the documented Beta 1.7.3 structure. Every
// constant below was tuned against this game's own 16.16 fixed-point value noise by
// measuring the terrain it produces, because Beta's own constants are floats tuned against a
// different noise function and would mean nothing here. Where a number came from is recorded
// beside it.
//
// Integer throughout, for world/noise.h's reason: the host test and the ARM11 must produce
// the same bits, and a float density field would disagree by an ulp either side of the
// solid/air threshold — which is a block of terrain that exists on one machine and not the
// other.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/noise.h"
#include "world/world.h"
#include "world/worldgen.h"

// ── The grid ──────────────────────────────────────────────────────────────────────────

#define GEN_D_GRID_XZ  5     // sample points across a column, so 4 cells
#define GEN_D_GRID_Y   17    // sample points up a column, so 16 cells
#define GEN_D_CELL_XZ  4     // blocks per cell horizontally  (CHUNK_DIM / 4)
#define GEN_D_CELL_Y   8     // blocks per cell vertically    (WORLD_HEIGHT / 16)

// Shifts rather than divides, because the ARM11 has no integer divide instruction. Asserted
// against the world dimensions in worldgen_density.c so a change to CHUNK_DIM or
// WORLD_HEIGHT cannot leave these silently wrong.
#define GEN_D_CELL_XZ_SHIFT 2
#define GEN_D_CELL_Y_SHIFT  3

// Sea level is GEN_SEA_LEVEL and lives in world/worldgen.h, not here — see the block comment
// beside it there for why, and for what task 17 does with it. wgdColumn fills it.

// ── The two limit fields and the selector ─────────────────────────────────────────────
//
// Beta's shape: two independent noise fields and a third that chooses between them. Where the
// selector saturates one way the terrain is entirely the first field, where it saturates the
// other way entirely the second, and the narrow band in between is the transition. That is
// what produces regions of calm rolling ground next to regions of dramatic broken ground,
// rather than one uniform texture of hills — the thing the legacy generator cannot do.
//
// LOW is the calm field: large features, few octaves, and stretched vertically so it varies
// slowly with height and therefore rarely doubles back on itself. It produces rolling ground.
//
// HIGH is the dramatic one: smaller features and one more octave, and — this is the part that
// matters — a **shorter vertical wavelength**, so within one 8-block grid cell it can move
// enough to beat the vertical bias term and cross zero twice. That is where overhangs, arches
// and open caves come from; they are not carved, they are what a steep 3D field does.
//
// The vertical shifts are the tuning knob that decides how much of that happens. Measured on
// this game's noise, seeds 1337 / 4242 / 90210, 5 x 5 columns each — see the log in
// blocksmith-log.md for the sweep.
#define GEN_D_LOW_SHIFT_XZ   7    // 128-block horizontal features
#define GEN_D_LOW_SHIFT_Y    6    // 64-block vertical: stretched, so it stays single-valued
#define GEN_D_LOW_OCTAVES    2

#define GEN_D_HIGH_SHIFT_XZ  5    // 32-block horizontal features
#define GEN_D_HIGH_SHIFT_Y   4    // 16-block vertical: steep enough to fold back on itself
#define GEN_D_HIGH_OCTAVES   3

#define GEN_D_SEL_SHIFT_XZ   8    // 256-block: the region, not the hill
#define GEN_D_SEL_SHIFT_Y    7
#define GEN_D_SEL_OCTAVES    2

// The selector is centred and amplified before it is used, so that it spends most of its
// range clamped at one end or the other and only a thin band of the world is a blend. A raw
// [0,1] noise used directly as a lerp weight would put the whole world in permanent
// half-and-half and average the two fields into a third, blander one — which is the single
// easiest way to get "noise soup" out of this design.
//
// The centre is the **measured median of this game's 3D fBm**, not 0.5: world/worldgen.h
// records 0.469 over a 1,032,192-sample box for the 2-octave cave field, and the same
// measurement was repeated for these fields rather than assumed — see worldgen_density.c.
#define GEN_D_SEL_CENTRE     0x00007800   // 0.469
#define GEN_D_SEL_GAIN       3

// ── The vertical bias, and what "density" is measured in ──────────────────────────────
//
// A density value here is **16.16 fixed point in blocks**, and it means "how many blocks
// below the local surface is this cell". Solid where it is positive.
//
// That comes from writing the field as
//
//     density(x,y,z) = (limit(x,y,z) - 1/2) * amplitude   +   (base_height - y)
//
// The second term is the vertical bias: a constant slope of one density-block per world
// block, pulling everything below `base_height` solid and everything above it empty. It is
// what stops the world being a uniform sponge from bedrock to sky, and it is also the top and
// bottom taper for free — at y = 0 it is +base_height and no noise excursion can lift it
// negative, at y = 127 it is deeply negative and none can push it solid.
//
// The first term displaces that surface by up to +/- amplitude/2 blocks. So `amplitude` is
// the peak-to-trough height range of the terrain in that biome, directly and in blocks, which
// is what makes the biome table below readable instead of a list of magic numbers.
//
// Choosing a slope of exactly one density-block per world block is what buys that
// readability, and it costs nothing: any other slope would only rescale `amplitude`.

// ── Biomes (task 16) ──────────────────────────────────────────────────────────────────
//
// **This does not add a biome system. It reads the one that already exists.**
// world/worldgen.h's GEN_BIOME_* field — a 2-octave value-noise fBm at 128-block features,
// with its own salt — has been the game's biome since step 5.3, and worldgenIsSandy() is
// already a threshold on it. Adding a second field here would have put two disagreeing
// answers to "where am I" in the same generator, so the density field is fed from that same
// field and worldgenIsSandy() keeps working unchanged and keeps agreeing with the terrain.
//
// What is new is what the field is used FOR. It used to pick a surface material and nothing
// else, so every biome had identical relief. Now it also picks the pair (base height,
// amplitude) that goes into the density field, which is what makes a desert flat, a plain
// gently rolling and a mountain range tall and broken.
//
// **Continuous, not banded.** The pairs are control points and the value between two of them
// is linearly interpolated, so a biome boundary is a gradient rather than a wall. Beta gets
// the same effect by averaging a 5 x 5 neighbourhood of biome cells per column; interpolating
// a continuous field instead gives smooth boundaries for free, with no extra noise
// evaluations and no neighbourhood buffer — which on this CPU is the difference that matters.
//
// **The control points are placed against the field's distribution rather than spread evenly
// over [0,1] — but the distribution this paragraph used to quote was wrong.** It said the
// fBm "runs 0.075..0.936 with a median near 0.68 — it is strongly clustered high" and that
// "0x7800 is the field's measured tenth percentile".
//
// **MEASURED 2026-08-25, and none of that holds.** Host build only — x86-64 gcc -O2 under
// WSL, linking this tree's world/noise.c, sampling worldgenBiome()'s exact expression; NOT
// measured on the ARM11. 13 seeds, two independent 2048 x 2048-block windows at stride 8,
// 65,536 columns per seed and 851,968 pooled each. The field runs 0.005..0.987 with
// **median 0.503** (0.497 on the far-field window) — very nearly symmetric about 0.5, not
// clustered high. 0x7800 is not the tenth percentile; it is roughly the 43rd, and it selects
// **42.6 % of columns** pooled, 37.9 .. 46.8 % per seed. The real tenth percentile is 0.292
// (0x4AA9). world/worldgen.h's GEN_SAND_BELOW comment carries the full figures and the
// per-seed spreads; read them there rather than restating them, which is how this comment
// and that one drifted apart in the first place.
//
// **The table below is UNCHANGED and is not re-derived by this correction.** That needs
// saying, because the false statistic was written up as the table's justification and the
// obvious reading of a corrected statistic is that the table must now be wrong too. It does
// not follow. The control-point values were arrived at by eye against the terrain they
// produce — the amplitudes in particular are pinned by the overhang and world-ceiling
// arithmetic recorded beside them in worldgen_density.c, which is geometry and does not
// depend on the field's histogram at all. What is now known to be false is the *argument*
// offered for them, not the values.
//
// So: the justification does not hold, and anyone re-tuning this table should re-derive it
// from the corrected distribution rather than trusting the reasoning above it. Two of the
// four points are also mislabelled in worldgen_density.c — 0x7800 is called the tenth
// percentile and 0xAE14 the median, and they are actually the 43rd and 85th. The labels are
// corrected there; the numbers they label are not.
//
// Reusing 0x7800 for the lowland control point is still right for the reason it always was,
// which never depended on the histogram: it is already GEN_SAND_BELOW, so putting the
// control point on exactly that value keeps the terrain's shape and its surface material
// changing at the same place instead of at two numbers that drift apart.
#define GEN_D_BIOME_POINTS 4

typedef struct {
	fx  biome;     // the biome-field value this pair is exact at
	int base_h;    // where the surface sits with no noise displacement, in blocks
	int amp;       // peak-to-trough displacement range, in blocks
} GenBiomePoint;

// Resolves the (base height, amplitude) pair for a biome-field value, interpolating between
// control points and clamping outside them. Exposed because the tests check the table's shape
// — monotonic, inside the world, straddling sea level — rather than re-deriving it.
void wgdBiomeParams(fx biome, int* base_h, int* amp);

// ── The surface pass (task 20) ────────────────────────────────────────────────────────
//
// Grass, dirt and sand by depth and by slope, plus stone.
//
// **Slope** is what stops grass growing on a vertical cliff face, which is the single most
// visible difference between a density-field world with a surface pass and one without: the
// legacy generator's ground never rises more than a block at a time, so it never had cliffs
// to get this wrong on. It is measured from the column's own 16 x 16 top-surface heights, by
// central difference in the interior and one-sided difference on the border ring — a
// one-sided difference is a real local slope, just a different estimator, so the border is
// not fabricated data. Two adjacent columns can therefore disagree about a border cell only
// where the slope sits exactly on the threshold, and each column still decides its own cells
// from its own data, which is the order-independence rule worldgenDecorate already lives by.
//
// Measured in blocks of height change per block of horizontal distance, doubled, because the
// central difference spans two blocks and dividing it by two would throw the odd values away
// on a CPU with no divide.
#define GEN_D_CLIFF_SLOPE_X2 5    // >= 2.5 blocks per block reads as a cliff face

// How far above sea level still counts as beach. Two blocks: enough that a shoreline is a
// visible band of sand rather than a single row, and not so much that every lakeside plain
// turns into a desert.
#define GEN_D_BEACH_ABOVE 2

// ── The entry points world/worldgen.c dispatches to ───────────────────────────────────
//
// Each implements exactly the contract world/worldgen.h documents for its public twin — the
// same contract the legacy generator implements, which is the point: nothing outside
// worldgen.c has to know which generator a world is using.

// Surface height at (x, z): the y of the first air above the **topmost** solid block, so the
// top solid block is at wgdHeight() - 1, matching worldgenHeight()'s convention exactly.
//
// With overhangs this is genuinely "the first solid coming down from the sky" and not "the
// ground you would land on if you fell into that hole", which is the same thing the legacy
// generator promised and the only thing spawn-finding needs.
//
// Evaluates only the 2 x 2 x 17 corner sub-grid that surrounds (x, z), not the whole column,
// and interpolates it with the identical arithmetic wgdColumn uses — so a standalone height
// query and the generated blocks agree by construction rather than by coincidence. That
// agreement is asserted for every cell of a generated column in the suite.
int wgdHeight(const WorldGen* g, int32_t x, int32_t z);

// Generates one column into `w`. Same contract as worldgenColumn: false if the block budget
// or the column table refused an allocation. Does not decorate — worldgen.c calls the shared
// tree pass afterwards, exactly as it does for the legacy generator.
//
// **It does place water (task 17).** The fill lives here rather than in a pass of its own
// because it has to go through the same staged worldSetChunkAll the terrain does: an ocean
// written a cell at a time with worldSet would promote every one of its chunks out of the
// UNIFORM form (world/chunk.h) — 2 KB of palette storage each for a chunk whose whole content
// is one repeated id — and an ocean is a great many such chunks. Written into the scratch's
// staging buffer it costs nothing at all.
//
// `s` is the calling thread's own scratch and holds every working buffer this pass uses — see
// world/worldgen.h's WorldGenScratch note. Two threads must never be inside this function on
// the same one.
bool wgdColumn(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz);

// The topmost-solid height of every cell of the column `s` generated MOST RECENTLY, as
// [z * CHUNK_DIM + x], in worldgenHeight()'s convention (the y of the first air above the
// ground, so the top solid block is at that value minus one).
//
// Exists for the tall-grass scatter (task 19), which runs in world/worldgen.c after the tree
// pass and needs the surface height of all 256 cells. Asking worldgenHeight() for them would
// cost 256 evaluations of a 2 x 2 x 17 corner lattice — 204 fBm3 each, more than three times
// the whole column's generation — for numbers wgdColumn has already computed and is holding
// in the scratch.
//
// (cx, cz) is REQUIRED and checked rather than being a comment, because the buffer is reused
// by the next wgdColumn on the same scratch: a caller that asks for the wrong column gets NULL
// and a refusal it must report, not a plausible set of heights belonging to somewhere else.
//
// **v1.8.7: the key is per lane, which is the point of the change.** It used to be a file
// static, so a second thread generating its own column moved the key out from under this one
// and the answer was NULL — measured at 159 refusals in 192 generations, i.e. that many
// permanent holes in the world. Reading it out of the caller's own scratch cannot go wrong
// that way.
const int16_t* wgdColumnTops(const WorldGenScratch* s, int32_t cx, int32_t cz);

// The raw density at one block, in 16.16 blocks, positive inside the ground. Exposed so the
// tests can assert the field's shape directly — that it is deeply positive at bedrock, deeply
// negative at the ceiling, and crosses zero near the biome's base height — rather than only
// observing it through generated blocks, where a sign error and a fill-loop error look alike.
int32_t wgdDensityAt(const WorldGen* g, int32_t x, int y, int32_t z);
