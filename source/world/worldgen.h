// Terrain generation: turns a seed and a column coordinate into blocks.
//
// The unit of generation is a **column** — all 8 chunks at one (cx, cz) — not a chunk.
// The heightmap is a function of (x, z) only, so generating a column computes 256 heights
// once and then fills straight down through the stack; generating chunk-at-a-time would
// recompute the same 256 heights up to eight times for the same answer. It also matches
// how the world stores things (world.h) and how saving will work in Phase 8, both of which
// are per-(x, z).
//
// Deterministic and order-independent: see world/rng.h for why terrain hashes a position
// instead of drawing from a stream. Two runs with the same seed produce the same world,
// and so does the same run reached by a different route.
//
// <3ds.h>-free, so the tests generate real terrain on the PC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/genversion.h"
#include "world/noise.h"
#include "world/world.h"

// ── Sea level ─────────────────────────────────────────────────────────────────────────
//
// **Roadmap task 17's constant, defined here by task 15 because the density generator needs
// it and nothing else had claimed it. Whoever fills the oceans should read it from here and
// not restate it.**
//
// This is the y of the water *surface*, so the topmost water block would sit at
// GEN_SEA_LEVEL - 1 and a column whose ground reaches GEN_SEA_LEVEL or above is dry land.
//
// The generator uses it for exactly two things, neither of which touches a block definition
// or the block registry: the beach rule in the density generator's surface pass (sand rather
// than grass at and just above the waterline) and its tree gate (nothing grows below the
// waterline). **No water is placed anywhere in this build** — there is no water block yet and
// defining one belongs to task 17.
//
// 64 is half of the 128-block world, the same fraction Beta 1.7.3 uses in a world of the same
// height. It is not arbitrary here either: the density generator's biome table
// (world/worldgen_density.h) is placed around it, with the lowland control points below it so
// their terrain becomes seabed and the plains control point above it so most of the walkable
// world is dry. Moving this constant without re-tuning that table would put the shoreline in
// the wrong place. The measured share of surface above it is asserted in the suite.
//
// **The legacy generator does not know this constant exists** and is completely unaffected by
// it: its surface band is GEN_SURFACE_MIN .. +GEN_SURFACE_RANGE, entirely above 64 by
// construction, so an existing world has no below-water terrain to flood.
#define GEN_SEA_LEVEL 64

// The band the surface is allowed to occupy. Well clear of both ends of the 128-block
// world: below it there must be room for the caves of step 5.4, and above it room for
// trees plus the player's own height, without either having to clamp and produce a
// flat-topped world.
//
// **The range is the largest the movement code can walk.** Amplitude was swept against the
// slope distribution: at 28 the worst adjacent step over a 256x256 sample was 1 with 10.7 %
// of pairs stepping at all; at 40 it is still 1 at 15.2 %; at 56 four pairs step **2**,
// and a 2-block rise cannot be climbed by anything in this game (jump 1.29, auto-step 1).
// 40 keeps a real margin under that cliff and was re-checked across 24 seeds — worst step
// 1, zero 2-block pairs.
#define GEN_SURFACE_MIN   40
#define GEN_SURFACE_RANGE 40    // so the top block sits at y = 39 .. 79

// Depth of the dirt band under the grass. Three is enough to be visible in a cliff face
// and cheap enough not to matter.
#define GEN_DIRT_DEPTH    3

// Step 5.3's variation. A second, much larger-featured noise decides how hilly the ground
// is and what the top few blocks are made of, so the world has regions rather than one
// uniform texture of hills everywhere.
//
// 128-block features against the heightmap's 64: the biome has to be a couple of hills
// across or it reads as more hills rather than as somewhere different. Two octaves,
// because a biome field wants smooth boundaries — the detail octaves that make terrain
// interesting would make biome edges ragged and stipple sand into grass one column at a
// time.
//
// **128 was measured, not assumed.** At 256 blocks the biome is wider than anything a
// player can see at once, and across 24 seeds the sand fraction of a 192x192 neighbourhood
// came out 0.00 % on nine of them and over 99 % on five — every world reading as a single
// uniform biome, which is the opposite of a variation pass. At 128 no seed was below 7 %
// or above 92 %: every world has both, within walking distance. At 64 the biome is the
// same size as the hills and the answer collapses to ~45 % everywhere.
#define GEN_BIOME_SHIFT   7
#define GEN_BIOME_OCTAVES 2

// Below this the surface is sand rather than grass, and no trees grow. A hard threshold
// on a smooth field, so the boundary follows a contour of the noise — biomes have edges.
//
// **The threshold is unchanged at 0x7800. The description of what it selects was wrong and
// is corrected below.** This comment used to claim the field "ran 0.075..0.936 with a median
// around 0.68" and that 0x7800 "is the tenth percentile and gives roughly a tenth of the
// world". None of that is true. It is out by roughly thirty percentage points, and anyone
// placing a new threshold against this field on the strength of it would be badly wrong.
//
// **MEASURED — 2026-08-25.** Provenance, stated plainly because the figures it replaces read
// like measurements too: a HOST build only — x86-64 gcc -O2 under WSL, linking this tree's
// world/noise.c and including world/rng.h, sampling worldgenBiome()'s exact expression
// (noiseFbm2 of rngMix(seed ^ SALT_BIOME) at biomeCoord, GEN_BIOME_OCTAVES). **Not measured
// on the ARM11.** Every step of the field is integer, so the console should agree bit for
// bit — that is the whole reason world/noise.h is fixed point — but it was not re-checked on
// hardware for this pass, and "should" is not "did".
//
// 13 seeds: 12345, 0xBEEF, 7919, 1337, 1616, 4242, 7, 99999, 20260818, 90210, 0, 1, 2. Two
// independent windows, each 65,536 columns per seed and 851,968 pooled:
//
//   * 2048 x 2048 blocks at stride 8, centred on the origin — 16 biome features across:
//     min 0.005, max 0.987, **median 0.503**, tenth percentile 0.292.
//     **0x7800 selects 42.6 % of columns** pooled, and 37.9 .. 46.8 % per seed.
//   * the same window moved to (100000, 100000), to check it is not a near-origin artifact:
//     median 0.497, 0x7800 selects 43.7 % pooled, 37.9 .. 50.8 % per seed.
//
// So the field is very nearly symmetric about 0.5 — it is NOT "clustered high" — and 0x7800
// (0.469) sits just below its median, not at its tenth percentile. Sand is about two fifths
// of the world. **If a tenth is ever actually wanted, the value is 0.292 (0x4AA9)**, which
// measured 10.00 % on the wide window and 10.5 % on the far one. Do not reuse 0x7800 for it.
//
// **Where the old figures came from, so the trap is not re-entered.** They came from a single
// 1024x1024 window. That is only eight biome features across, so it measures one seed's local
// terrain rather than the field's distribution. The same probe over a 290 x 290 window
// reproduces the failure exactly: pooled median 0.533 and 37.5 % sand, but a per-seed spread
// of 0.41 % to 64.2 % — at that scale some seeds sit almost entirely inside a single biome.
// Quote the per-seed spread alongside any pooled figure taken from this field, or the next
// reader gets a number with no error bar on it.
//
// The rejection of 0x3000 still stands, but for a smaller margin than claimed: pooled it
// selects 2.0 % of columns on the wide window and 2.6 % on the dense one, not the 0.7 % the
// old text quoted from one seed. Still too rare to be a biome anyone meets; still rejected.
//
// **The suite did not catch this and still would not.** world/world_test.c's
// testWorldgenBiome only asserts the sand fraction lies strictly between 2 % and 98 %, and
// both 10 % and 42 % satisfy that — the band is far too loose to discriminate between the
// claim and the truth. Sharper evidence was already sitting in the same file: the seed
// comment on testWorldgenTrees records seed 1616 splitting its region 47.7 % sand to 52.3 %
// grass, which agrees with 42 % and flatly contradicts 10 %. (world_test.c is deliberately
// not touched by this correction; tightening that band is a separate change.)
#define GEN_SAND_BELOW    0x00007800    // 0.469 of the biome range; MEASURED ~42 % of columns

// How flat the flattest terrain is, as a fraction of GEN_SURFACE_RANGE. Not zero: a biome
// with no relief at all is a billiard table, and the point of the variation pass is that
// somewhere is calmer than somewhere else, not that somewhere is dead.
//
// An eighth rather than a quarter. **The constant is unchanged; its stated reason was wrong.**
// This used to read "for the same reason as the threshold above: with the biome clustered
// high, a quarter-to-full amplitude range came out as roughly three-quarters-to-full
// everywhere". The biome field is NOT clustered high — see GEN_SAND_BELOW above, median 0.503
// measured 2026-08-25 — so that explanation does not hold.
//
// What survives is the observation, not the explanation: a quarter-to-full range was tried
// and the contrast between calm and broken ground was not visible. That was judged by eye
// against the terrain and has NOT been re-measured here, so treat it as a recorded
// observation rather than a number. An eighth is what shipped and what the tests are written
// against; changing it is a terrain change, not a comment fix.
#define GEN_FLAT_FRACTION 8

// Step 5.4's caves. Two independent 3D fBms; a block is hollowed out where BOTH of them
// sit inside a narrow band around their median.
//
// **Why a band and not a threshold.** One field below a threshold gives blobs — a swiss
// cheese of disconnected bubbles the player can never walk through and would never find.
// A narrow band around a value is a thickened iso-SURFACE, a sheet; two independent sheets
// intersect in a curve, and a thickened curve is a tunnel. That is the whole trick, and it
// is why there are two salts rather than one field with a wider band.
//
// **The centre is the measured median, not 0.5.** Over a 1,032,192-sample box the cave-scale
// fBm ran 0.029..0.958 with mean 0.483 and p50 0.469. Those three figures are the original
// 3D measurement and have NOT been re-checked here — only the 2D comparison below was.
//
// The comparison that used to follow them was wrong and is removed: this said the cave field
// was "much more symmetric than the 2D heightmap fBm (0.075..0.936, median 0.68)". It is not
// much more symmetric, because the 2D fields are not skewed either. Measured 2026-08-25 on
// the same host build described under GEN_SAND_BELOW, 13 seeds over 851,968 columns, the
// heightmap fBm (GEN_FEATURE_SHIFT 6, GEN_OCTAVES 4, unsalted) runs 0.019..0.967 with
// **median 0.4996**, and the biome fBm runs 0.005..0.987 with median 0.503. All three fields
// sit close to 0.5.
//
// Measuring the cave field separately was still the right call — a 3D fBm at a different
// octave count is a different distribution and reusing a 2D number would have been an
// assumption — but the justification was the contrast, and the contrast was not real.
//
// **The half-width is the connectivity knee.** Carved fraction and the share of carved
// volume sitting in systems bigger than 100 blocks, 6-connected flood fill over a 96x64x96
// box: 0.03 -> 1.99 % carved / 96.8 % connected; 0.04 -> 3.50 % / 97.9 %; 0.05 -> 5.44 % /
// 99.2 %; 0.06 -> 7.81 % / 99.6 %. Below 0.03 it falls apart (0.02 gives 167 components
// and only 64.9 % in anything walkable). 0.05 is the first width where every seed tried
// (1337, 1616, 4242, 7, 99999, 20260818) put at least 98.9 % of its carved volume into
// walkable systems, at 3.97..8.23 % of the underground — and every extra percent is
// triangles, which is the cost this step was warned about.
#define GEN_CAVE_SHIFT_XZ  5            // 32-block horizontal features
#define GEN_CAVE_SHIFT_Y   4            // 16-block vertical: squashed, so tunnels run flat
#define GEN_CAVE_OCTAVES   2
#define GEN_CAVE_CENTRE    0x00007800   // 0.469, the measured median
#define GEN_CAVE_HALF      0x00000CCD   // 0.05

// No cave may come within this many blocks of the surface, and none may touch y = 0.
//
// **This is a deliberate design decision, not a safety hack.** Five blocks is grass, the
// three dirt blocks, and one of stone, so a cave can never leave a grass block floating on
// nothing, can never open under the player's feet at spawn, and can never break the rule
// the rest of the world depends on — that the first solid block walking down from the sky
// is at worldgenHeight() - 1. The price is that this step generates no cave ENTRANCES: the
// way in is to dig, which is the mechanic the game already has. Surface openings and
// ravines would need spawn-finding to search for solid ground rather than trust the
// heightmap, and that is not in this step.
#define GEN_CAVE_MIN_DEPTH 5
#define GEN_CAVE_FLOOR     1

// Tall grass (roadmap task 19). Out of 256, per surface cell, and applied only where the
// scatter is allowed to look at all: a grass top block, above the waterline, with air over it.
//
// **24 is a measured choice, not a guess at "sparse".** The tree pass next door uses the same
// out-of-256 idiom at 96, but that is per 8 x 8 CELL — one tree per 64 blocks at most — while
// this is asked per BLOCK, so the same number would carpet the world. 24/256 is 9.4 % of
// eligible ground: roughly 24 plants in a 16 x 16 chunk, or one every three or four paces,
// which reads as a meadow with gaps you can see the ground through. Measured coverage of the
// eligible surface and of the whole surface is recorded in the suite.
//
// The draw is one rngHash2 of the world (x, z) — the same positional-hash rule world/rng.h
// states for terrain and the same one treeInCell() follows — so two neighbouring columns
// generated in either order, in different sessions, agree without talking to each other, and
// the same seed regenerates the identical field of plants.
//
// **SUPERSEDED by v1.8.3 Phase 2 and NO LONGER READ BY ANY CODE PATH.** The GEN_GRASS_*
// per-biome chances below replaced it, and the scatter has only ever run for a density world,
// so there is no legacy caller left to keep it alive either. Changing this number now changes
// nothing at all. It is kept rather than deleted because the paragraph above it is the
// measurement the per-biome numbers are calibrated against — 24/256 is what "a meadow with
// gaps you can see the ground through" was measured to look like, and forest still sits
// exactly there.
#define GEN_GRASS_CHANCE  24            // out of 256; superseded, see GEN_GRASS_* below

// Trees. One per 8x8 cell at most, so two trunks can never be closer than a couple of
// blocks and the decoration pass has a bounded neighbourhood to scan.
#define GEN_TREE_CELL     8
#define GEN_TREE_CHANCE   96            // out of 256 cells
#define GEN_TREE_MIN_H    4             // trunk blocks, before the canopy
#define GEN_TREE_MAX_H    7
#define GEN_TREE_RADIUS   2             // canopy half-width, in blocks

// v1.8.3 task 52. The four constants above are the LEGACY and DENSITY shape and none of them
// move; these are the ceilings a BIOME world's per-instance draws are allowed to reach.
//
// GEN_TREE_REACH_MAX is the one that is load-bearing rather than descriptive. worldgenDecorate
// scans the tree cells that can reach a column, and a tree whose canopy reaches further than
// the scan assumes is not a cosmetic bug: the column that owns the far cells would never look
// at that tree, so the same block would be leaves or air depending on which column asked. It
// is `GEN_TREE_RADIUS_MAX + 1` because a wide tree stands on a 2 x 2 trunk whose far column is
// one block further out than its anchor, and the suite asserts every table row against it.
#define GEN_TREE_TRUNK_MAX  12          // tallest trunk any biome may draw (jungle)
#define GEN_TREE_RADIUS_MAX 3           // widest canopy half-width any biome may draw
#define GEN_TREE_REACH_MAX  (GEN_TREE_RADIUS_MAX + 1)

// ── Biomes (v1.8.3 Phase 2) ───────────────────────────────────────────────────────────
//
// **Reached only by a GEN_VERSION_DENSITY world.** Every rule below is behind the version
// dispatch in worldgen.c, for world/genversion.h's reason: a GEN_VERSION_LEGACY world must
// keep generating the terrain it always did, byte for byte, and none of this may touch it.
//
// **Biome is a LABEL painted onto terrain that already exists — it never drives height.**
// wgdBiomeParams() keeps deriving base height and amplitude from the raw biome field exactly
// as it did before. Thresholding a continuous field and then feeding the resulting discrete
// id into the height function is what produces the classic biome-border cliff; not letting
// the id near the height function makes that seam structurally impossible rather than
// something to blend away afterwards. Cost of the fix: zero, by construction.
//
// The set is a 3 x 2 rectangle — three temperature bands crossed with two humidity bands:
//
//            DRY (humid < GEN_HUMID_WET)   WET (humid >= GEN_HUMID_WET)
//   COLD     BIOME_TUNDRA                  BIOME_TAIGA
//   MILD     BIOME_PLAINS                  BIOME_FOREST
//   HOT      BIOME_DESERT                  BIOME_JUNGLE
//
// **Why the hot-wet cell cannot be merged back into desert.** Making the hot band
// humidity-independent is the obvious way back to four biomes, and measured it puts
// sand-capped ground at 26-65 % of the world — worse than the 26.8-56.2 % it is today.
// Shrinking the desert from "half the world" to a place you travel to is the single largest
// visible win of this rung, and merging the cell throws it away. Kept separate, desert
// measures 14.7-32.0 %.
//
// **JUNGLE, not swamp, and the name is not cosmetic.** Swamp was rejected on this project
// for a measured reason recorded at tools/make_atlas.py:414-415 — a teal sea beside a green
// field on a 5-bit-per-channel sheet is two shades of the same thing. And Phase 2 adds NO
// new block ids: a hot, wet region built from grass, dirt, sand, wood and leaves, given the
// densest trees and the thickest undergrowth in the world, is a jungle. Naming an enum
// member for an intention rather than for what it renders is how this same file acquired
// three comments that were false for a year.
typedef enum {
	BIOME_TUNDRA = 0,   // cold, dry — bare dirt cap; see the placeholder note in surfaceBlock
	BIOME_TAIGA,        // cold, wet — sparse tall narrow trees
	BIOME_PLAINS,       // mild, dry — the baseline the others are read against
	BIOME_FOREST,       // mild, wet — dense trees, moderate undergrowth
	BIOME_DESERT,       // hot, dry  — sand cap, nothing grows
	BIOME_JUNGLE,       // hot, wet  — the densest trees and undergrowth in the world
	BIOME_COUNT,
} BiomeId;

// **Temperature is the existing biome field INVERTED**: temp = FX_ONE - worldgenBiome().
// One subtract, no new noise, and it cannot move a block. The direction matters: the biome
// field's high end is the tall broken ground of the density table's upper control points, so
// reading it as "hot" would put deserts on mountain tops and tundra on beaches. Its low end
// is the flat lowland the first two control points describe, and that is where the sand
// already is.
//
// **GEN_TEMP_HOT is DERIVED, not tuned.** `temp > FX_ONE - GEN_SAND_BELOW` is
// `biome < GEN_SAND_BELOW`, character for character the test worldgenIsSandy() has always
// applied. Reusing the constant rather than writing a fresh literal is what keeps the density
// table's second control point and the surface material agreeing: the terrain stops being
// lowland-flat at exactly the coordinate the desert stops. Writing a new number here would
// let the two drift apart silently on the next retune.
#define GEN_TEMP_HOT   (FX_ONE - GEN_SAND_BELOW)   // 0x8800; equals biome < GEN_SAND_BELOW

// ~18th percentile of the temperature field. **The weakest of the three thresholds**, and it
// says so on purpose: 0x3800 / 0x4000 / 0x4800 / 0x5000 were swept over 13 seeds and 0x5000
// is the best of those four — at 0x3800 taiga fell to 0.66 % on seed 1616, which is a biome
// nobody ever walks into — but nothing above 0x5000 was tried. Measured at this value:
// tundra 3.6-12.1 %, taiga 1.8-14.5 %.
#define GEN_TEMP_COLD  0x00005000

// The middle of the humidity field's range, and the only threshold here that genuinely is
// one. The field measured symmetric — pooled median 0.4939, every seed's own median inside
// 0.4712..0.5271 — so 0x8000 splits every world tried no worse than 47/53. No percentile of
// the temperature field manages that, which is why that one had to be measured instead.
#define GEN_HUMID_WET  0x00008000

// Per-biome parameters, as macros so that the derived bounds below are compile-time constants
// and cannot drift from the table that feeds them.
//
// Tall grass is asked per BLOCK of eligible ground, out of 256 — the same idiom and the same
// scale as the single global GEN_GRASS_CHANCE 24 it replaces, which was measured to read as
// "a meadow with gaps you can see the ground through". Jungle at 56 is a little over twice
// that; tundra and desert are zero because nothing grows on snow-country dirt or on sand.
#define GEN_GRASS_TUNDRA   0
#define GEN_GRASS_TAIGA   12
#define GEN_GRASS_PLAINS  40
#define GEN_GRASS_FOREST  24
#define GEN_GRASS_DESERT   0
#define GEN_GRASS_JUNGLE  56

// The largest of the six. worldgenScatter draws its hash FIRST and rejects against this
// before it resolves the biome, so the common case still costs one hash and no noise — the
// ordering the old single-constant code was written for. Asserted against the table in the
// suite, because a new biome with a higher chance would otherwise be silently clamped here.
#define GEN_GRASS_CHANCE_MAX GEN_GRASS_JUNGLE

// Trees, out of 256 per 8 x 8 cell, replacing the single global GEN_TREE_CHANCE 96.
#define GEN_TREE_TUNDRA    0
#define GEN_TREE_TAIGA    64
#define GEN_TREE_PLAINS   24
#define GEN_TREE_FOREST  128
#define GEN_TREE_DESERT    0
#define GEN_TREE_JUNGLE  160

// ── Flora (v1.8.3 Phase 3) ────────────────────────────────────────────────────────────
//
// Cactus and dead bush on desert sand; fern on taiga and jungle grass. All four chances are
// out of 256 per eligible SURFACE CELL — the same idiom and the same scale as the GEN_GRASS_*
// table above, and NOT the per-8x8-cell scale the GEN_TREE_* chances use. Getting those two
// scales confused is what the GEN_GRASS_CHANCE note warns about: the same number means one
// plant per block and one tree per sixty-four.
//
// They are their own constants rather than five more fields in BiomeParams, because they are
// not one quantity per biome. Desert has TWO plants drawn from one shared band and the other
// five biomes have at most one, so a field per plant would carry four zeros for every biome
// that grows none of them, and the derived bound below would be a max over a mostly-zero
// table rather than over the three expressions that actually bound the branches.
//
// **The two desert chances share ONE draw and must be read as a band, not as two independent
// rolls.** world/worldgen.c tests `draw < GEN_CACTUS_CHANCE` first and then
// `draw < GEN_CACTUS_CHANCE + GEN_DEAD_BUSH_CHANCE`, so the second constant is the WIDTH of
// the band above the first: 3 and 8 give 3/256 cactus and 8/256 dead bush, never 11/256 of
// either. Raising GEN_CACTUS_CHANCE alone therefore takes cells away from the dead bush.
//
// Scale, against the one density on this rung that was ever measured. Tall grass at 24/256
// was measured to read as "a meadow with gaps you can see the ground through" (GEN_GRASS_CHANCE
// above). These four are placed against that ruler by eye and are **NOT independently
// measured** — said plainly, because the numbers around them are:
//
//   cactus       3/256   about 3 per 16x16 chunk of desert. Sparse deliberately: a cactus is
//                        a full solid cube here (world/registry.c row [12] says why there is
//                        no narrow shape yet), so a dense field of them is a maze to walk.
//   dead bush    8/256   about 8 per chunk, a third of tall grass, which is what makes a
//                        desert read as picked over rather than planted.
//   fern taiga  20/256   just above taiga's own tall grass at 12, so the cold forest floor
//                        reads fern-first.
//   fern jungle 32/256   under jungle's tall grass at 56, and drawn on a separate salt so the
//                        two are independent fields rather than one wearing two textures. The
//                        pair is the thickest undergrowth in the world, which is what
//                        BIOME_JUNGLE's own comment says that biome is for.
#define GEN_CACTUS_CHANCE     3
#define GEN_DEAD_BUSH_CHANCE  8
#define GEN_FERN_TAIGA       20
#define GEN_FERN_JUNGLE      32

// Cactus height in blocks, inclusive. Two at most. Not a safety bound — the pass checks every
// cell of the column before it writes any of them, so a taller cactus would simply be refused
// more often under a canopy or against a dune — but a three-block stack of a FULL_CUBE reads
// as a pillar rather than as a plant at this tile size.
#define GEN_CACTUS_MIN_H      1
#define GEN_CACTUS_MAX_H      2

// The largest draw any branch of the flora pass can use. worldgenFlora draws its hash FIRST
// and rejects against this before it resolves a biome, so the common case costs one hash and
// no noise — the same ordering, and the same reason, as GEN_GRASS_CHANCE_MAX above.
//
// A MAX over three expressions rather than a single name, because the desert's two plants
// share a band: the widest branch is whichever of (cactus + dead bush), fern-in-taiga and
// fern-in-jungle is largest. Written as nested conditionals so it stays a compile-time
// constant. It bounds all three branches BY CONSTRUCTION -- it is a MAX over the very
// expressions those branches test against -- so there is no way for it to stop bounding them
// and no assertion is written for it. Saying that plainly rather than claiming a check: an
// assert of the form `MAX >= GEN_FERN_JUNGLE` reads like proof and can never go red, which
// is worse than nothing.
//
// What is NOT checked anywhere, and is the real way this rots: a NEW branch added to
// worldgenFlora()'s switch whose chance is not folded into the MAX above. The draw is
// rejected against this bound BEFORE the biome is resolved, so the new chance would be
// silently clamped -- fewer plants, no error, and every placement assertion still green,
// because they all check WHERE a plant stands and never HOW MANY there are. Add a branch,
// add its chance here.
// ── Flowers (v1.8.8) ──────────────────────────────────────────────────────────────────
//
// steve asked for "more grass variety — 2-block tall grass, normal grass, and flowers.
// Flowers should be biome-specific." These are the biome-specific half.
//
// Same scale and the same idiom as everything above: out of 256 per eligible SURFACE cell,
// drawn from worldgenFlora's ONE hash, and read as a BAND stacked on whatever that biome's
// existing flora already claims. So taiga's fern keeps draws 0..19 exactly as it always had
// and the bluebell takes 20..27; jungle's fern keeps 0..31 and the orchid takes 32..43. That
// stacking is the whole reason no existing plant moves: every branch below leaves the band it
// inherited alone and only claims draws above it, which the cheap reject used to throw away.
//
// Which flower goes where, and why these five bands and not others:
//
//   plains   poppy 10 + daisy 14   the open meadow biome; the widest flower band in the
//                                  world, and the daisy is the commoner of the two so that a
//                                  plains field reads white-speckled with red accents.
//   forest   bluebell 12 + poppy 8 a shaded floor. Bluebells lead, poppies are the leftover
//                                  sunlight; the two together (20) stay under plains' 24 so
//                                  the meadow is still the flowery biome.
//   taiga    bluebell 8            sparse and cold, and the same species as the forest's so
//                                  the two shaded biomes share a palette rather than each
//                                  inventing one.
//   jungle   orchid 12             the only biome that grows it. Under the jungle fern's 32,
//                                  so the undergrowth stays fern-first.
//   tundra   none                  nothing flowers on a snow cap; it grows no tall grass
//                                  either (GEN_GRASS_TUNDRA is 0).
//   desert   none                  it has the dead bush, which is what a desert has instead.
//
// Placed by eye against the one measured ruler on this rung — tall grass at 24/256 reads as
// "a meadow with gaps you can see the ground through" — and **NOT independently measured**,
// exactly as the four Phase 3 chances above are not.
#define GEN_POPPY_PLAINS     10
#define GEN_DAISY_PLAINS     14
#define GEN_BLUEBELL_FOREST  12
#define GEN_POPPY_FOREST      8
#define GEN_BLUEBELL_TAIGA    8
#define GEN_ORCHID_JUNGLE    12

// The five per-biome band tops, and the MAX over them. v1.8.8 widened this from three
// expressions to five: every branch of worldgenFlora's switch now has a total, and the bound
// is still a max over the very expressions those branches test against, so it still holds BY
// CONSTRUCTION and still has no assertion for the reason stated above.
//
// It moves from 32 to 44, and that is safe in one direction only, which is the direction it
// moves: raising the bound lets MORE draws reach the switch and can never reject one that used
// to get through. Every draw in the newly admitted 32..43 band falls outside every pre-v1.8.8
// branch — desert stops at 11, taiga's fern at 20, jungle's fern at 32 — so no cactus, dead
// bush or fern changes position. Measured, not argued: see the A/B cell diff in
// world_test.c's testWorldgenWaterAndGrass.
#define GEN_FLORA_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define GEN_FLORA_DESERT_TOTAL (GEN_CACTUS_CHANCE + GEN_DEAD_BUSH_CHANCE)
#define GEN_FLORA_TAIGA_TOTAL  (GEN_FERN_TAIGA + GEN_BLUEBELL_TAIGA)
#define GEN_FLORA_JUNGLE_TOTAL (GEN_FERN_JUNGLE + GEN_ORCHID_JUNGLE)
#define GEN_FLORA_PLAINS_TOTAL (GEN_POPPY_PLAINS + GEN_DAISY_PLAINS)
#define GEN_FLORA_FOREST_TOTAL (GEN_BLUEBELL_FOREST + GEN_POPPY_FOREST)
#define GEN_FLORA_CHANCE_MAX                                                      \
	GEN_FLORA_MAX2(GEN_FLORA_MAX2(GEN_FLORA_DESERT_TOTAL, GEN_FLORA_TAIGA_TOTAL), \
	               GEN_FLORA_MAX2(GEN_FLORA_JUNGLE_TOTAL,                          \
	                              GEN_FLORA_MAX2(GEN_FLORA_PLAINS_TOTAL,           \
	                                             GEN_FLORA_FOREST_TOTAL)))

// ── Two-block tall grass (v1.8.8) ─────────────────────────────────────────────────────
//
// Out of 256, per tall-grass clump worldgenScatter has ALREADY decided to place. Not a share
// of the surface: it is a second question asked of a cell that is getting grass either way, so
// raising it takes nothing away from anything.
//
// Drawn from bits 8..15 of the SAME hash the placement draw uses, and the placement draw is
// still the low byte and is untouched. That is what keeps every existing clump exactly where
// it was — this constant can only turn a one-block clump into a two-block one, never move one
// or delete one. A clump with no room above it (a canopy, a cliff, the world ceiling) falls
// back to one block rather than being skipped.
//
// 96/256, a bit over a third. Chosen so a meadow is visibly mixed-height: steve asked for
// "2-block tall grass, normal grass", so both have to be present. A number near 128 would read
// as "the grass got taller" and a number near 32 as an occasional glitch.
#define GEN_TALL_GRASS_TWO   96

// ── Apples (v1.8.8) ───────────────────────────────────────────────────────────────────
//
// Out of 256, per cell of the ring immediately BELOW a fruiting tree's lowest canopy layer.
// Only oak and birch fruit — BiomeParams.fruit is BLOCK_AIR for every other species — which
// is what keeps the taiga's conifers bare.
//
// 16/256 over a lowest layer of roughly thirteen cells is about 0.8 apples per fruiting tree,
// so most trees carry one and a good many carry none. Deliberately under one: an apple is a
// FULL_CUBE of solid red at 16x16, and a tree wearing six of them reads as a decoration
// rather than as fruit.
#define GEN_APPLE_CHANCE     16

// Silhouette, from trunk length, canopy radius and layer pattern ONLY — no new block ids, no
// new tiles. Canopy shape reads at distances where colour does not, which is what matters on a
// 400x240 screen.
//
// **v1.8.3 task 52 — corrected 2026-09-01.** Until this change the "three shapes" this comment
// claimed were two: jungle and plains both drew canopy radius 2 with the identical layer
// pattern, so jungle differed by trunk length alone. There was also no per-INSTANCE variation
// of anything but trunk length, which is the half of task 52 that had been designed
// (SALT_TREE_SHAPE, in the v1.8.3 design page) and never written. Both are now real:
//
//   * `shape` picks the layer pattern — TREE_SHAPE_ROUND, _CONIFER or _BROAD — and the three
//     are different silhouettes, not three names for one loop.
//   * `canopy_min..canopy_max` is drawn PER TREE, so one forest holds several widths.
//   * `big_chance` is the per-tree draw for a 2 x 2 trunk, which is task 52's "trunk width".
//     A wide tree only stands where its four columns share one ground height; that is checked
//     rather than assumed, because a 2 x 2 trunk anchored off one column's height would float
//     or bury itself on any slope.
//
// **Every trunk length stays inside GEN_TREE_MIN_H..GEN_TREE_TRUNK_MAX, every radius inside
// GEN_TREE_RADIUS_MAX, and radius + (wide ? 1 : 0) inside GEN_TREE_REACH_MAX — correctness
// bounds, not style ones.** worldgenDecorate's scan bounds are written in terms of
// GEN_TREE_REACH_MAX, so a canopy reaching past it would be clipped at a column border
// depending on which column was generated first — an order-dependence bug, not a cosmetic
// one. Asserted in the suite, row by row.
typedef enum {
	TREE_SHAPE_ROUND = 0,   // two wide layers with clipped corners, two narrow above — the oak
	TREE_SHAPE_CONIFER,     // alternating narrow/wide tiers rising to a single tip — the spruce
	TREE_SHAPE_BROAD,       // one deep flat crown carried on a long trunk — the jungle tree
	TREE_SHAPE_COUNT
} TreeShape;

typedef struct {
	uint8_t grass_chance;    // out of 256 eligible surface cells
	uint8_t tree_chance;     // out of 256 tree cells
	uint8_t trunk_min;       // trunk blocks, before the canopy
	uint8_t trunk_max;
	uint8_t canopy_min;      // per-tree canopy half-width is drawn from this range, in blocks
	uint8_t canopy_max;
	uint8_t shape;           // a TreeShape
	uint8_t big_chance;      // out of 256 trees: a 2 x 2 trunk instead of a single column

	// ── v1.8.8: the tree's SPECIES, as three block ids ────────────────────────────────
	//
	// steve asked for "per-biome wood colours, blocks, textures, types, leaves, logs,
	// planks". Silhouette alone could not deliver that — the note above says outright that
	// task 52 gave the biomes three shapes and NO new block ids — so this is the other axis:
	// a taiga tree is a spruce made of spruce, and a forest tree is a birch made of birch.
	//
	// Ids and not a tint. The three barks differ by the MARKS on them (birch's horizontal
	// lenticels, spruce's vertical scaly plates) and the three canopies by their cutout
	// silhouette against the sky; a per-vertex multiply cannot add a mark that is not in the
	// tile, and cannot lighten oak into birch at all. See source/gfx/atlas_tiles.h.
	//
	// `fruit` is BLOCK_AIR for a species that does not bear — which is every conifer. It is
	// hung under the lowest canopy layer at GEN_APPLE_CHANCE, not woven into it, so it is
	// visible from the ground rather than buried in leaves.
	//
	// ⚠ These are read ONLY on the `bp != NULL` path in treeInCell(), i.e. only for a world
	// stamped GEN_VERSION_BIOME. A legacy or density world still gets BLOCK_WOOD and
	// BLOCK_LEAVES from the defaults in the Tree struct, which is what keeps every world made
	// before v1.8.8 byte-for-byte what it was.
	uint8_t log;             // BLOCK_* written for the trunk
	uint8_t leaf;            // BLOCK_* written for the canopy
	uint8_t fruit;           // BLOCK_* hung under the canopy, or BLOCK_AIR for a bare species
} BiomeParams;

typedef struct {
	uint32_t seed;

	// v1.7.0. Which generator this world was made with — see world/genversion.h for the
	// whole rule and for why the field exists at all. Every function in this file dispatches
	// on it, so a world stamped GEN_VERSION_LEGACY produces byte-identical terrain to what
	// it produced before the density generator was written, forever.
	uint32_t version;

	// v1.8.7. rngMix(seed ^ SALT_CAVE) and rngMix(seed ^ SALT_CAVE2), the two mixed
	// seeds worldgenIsCave() draws its pair of 3D fBms from. Derived here rather than
	// at the call site because that call site is asked roughly 20,900 times per
	// generated column and the answer cannot change while the WorldGen lives.
	//
	// Set by worldgenInit() in both arms, always as rngMix(g->seed ^ SALT_X) -- a
	// WorldGen built any other way has never existed in this tree and would carry
	// uninitialised salts.
	uint32_t cave_salt[2];
} WorldGen;

// ── Per-lane scratch (v1.8.7) ─────────────────────────────────────────────────────────
//
// **Every buffer the generator carries from one call to the next lives in here, and nowhere
// else.** Until v1.8.7 that state was a set of file statics split across world/worldgen.c and
// world/worldgen_density.c, which made the whole generator single-entry: two threads
// generating DIFFERENT columns stamped on each other's scratch. Measured rather than argued —
// tests/worldgen_mt_test.c against the old code, two threads over disjoint columns: 24 of 32
// columns wrong and 159 of 192 generations REFUSED, against a serialised control arm that was
// clean in every run. A refused column is a permanent hole in the world (main.c genInstallOne
// -> columns_failed), not a slow frame.
//
// **A WorldGen is shared and read-only; a WorldGenScratch is owned by exactly one thread.**
// That split is the whole design. The seed, the version and the cave salts are the same for
// every lane and must be; the scratch is per lane and must never be. Making it a parameter
// rather than a static puts the rule in the type system instead of in a comment: a second
// generator lane is one more WorldGenScratch, and there is no longer a way to forget which
// half is shared.
//
// **Caller-owned storage, deliberately.** Not allocated here, not a static array indexed by a
// lane number — that would be the same file static wearing a subscript, and the .bss would
// grow rather than move — and not thread-local, because __thread is not something to rely on
// in this toolchain. The lane owns the object; the generator only borrows it.
//
// It is about 16 KB, so it belongs in a caller's static storage and not on a stack: the
// worker thread's is 32 KB (app/worker.c). It is NOT charged against WORLD_BUDGET_BYTES
// (world/budget.h) — that budget counts block storage, and this is paid once per lane rather
// than once per column.
//
// The definition is in world/worldgen_scratch.h, which any caller that needs to DECLARE one
// includes. It lives in its own header because the struct is sized by the density generator's
// grid geometry, which is in world/worldgen_density.h — and that header includes this one, so
// the definition cannot come back the other way.
typedef struct WorldGenScratch WorldGenScratch;

// Puts a WorldGenScratch into the state a freshly zeroed one is already in: nothing cached,
// nothing valid. **Call it once before the scratch's first generation call**, unless the
// object is in static storage, where the C runtime has already done it.
//
// Only the two validity flags are written — the cave cache's and the column-tops key's —
// because they are the only fields ever read before they are written. Every buffer in the
// struct is filled in full by the pass that consumes it, so memsetting 16 KB here would cost
// real time to hide nothing.
void worldgenScratchInit(WorldGenScratch* s);

// `version` is a GEN_VERSION_* from world/genversion.h, and there is deliberately no default:
// the one thing a caller must never do is generate a world without saying which generator it
// belongs to, so the parameter is required at every call site rather than implied. A version
// this build does not know is refused here rather than silently coerced — genVersionKnown()
// is the same predicate genVersionResolve() gates the load on, asked a second time at the one
// place a WorldGen can come into existence.
//
// Returns false and leaves `g` unusable (version 0, which every dispatch treats as unknown)
// when `version` is not one this build can generate.
bool worldgenInit(WorldGen* g, uint32_t seed, uint32_t version);

// Surface height at (x, z): the y of the first air block above the ground, so the top
// solid block is at `worldgenHeight() - 1`. Same convention as handbuiltHeight, so
// spawn-finding and the tests do not need to know which world they are looking at.
//
// Trees are NOT included: this is the ground, which is what spawn-finding and physics
// want. Standing on a canopy is a raycast/collision question, not a heightmap one.
int worldgenHeight(const WorldGen* g, int32_t x, int32_t z);

// The biome field at (x, z), in [0, FX_ONE]. Exposed because the tests need to check that
// the surface block and the tree placement agree with it rather than re-deriving the
// threshold, and because a caller that wants to describe where the player is standing
// should not have to guess.
fx worldgenBiome(const WorldGen* g, int32_t x, int32_t z);

// v1.8.3 Phase 2. The humidity field at (x, z), in [0, FX_ONE]. The second axis of the
// climate square, sampled at the same GEN_BIOME_SHIFT and the same octave count as the biome
// field so that the two vary at the same scale — a humidity field with finer features would
// stipple jungle into desert one column at a time instead of drawing regions.
//
// Its own salt, for the reason worldgen.c's salt block gives: two fBms drawn from the same
// seed are visibly the same shape, and a humidity field sharing the temperature field's seed
// would put every wet region in the same place as every cold one and collapse the 3 x 2
// rectangle to a diagonal.
//
// Exposed for the same reason worldgenBiome() is: the tests check the classifier against the
// two fields rather than re-deriving the thresholds.
fx worldgenHumidity(const WorldGen* g, int32_t x, int32_t z);

// v1.8.3 Phase 2. Which biome (x, z) is in, from the (temperature, humidity) pair.
//
// **Answers for any world, including a legacy one** — it is a pure function of the two noise
// fields and does not consult g->version. What is gated on the version is who ASKS: nothing
// on the legacy path calls this. Keeping it ungated is what lets the suite measure both
// answers side by side for the same seed and assert they differ, which is the check that
// notices the version gate being deleted.
BiomeId worldgenBiomeAt(const WorldGen* g, int32_t x, int32_t z);

// The tall-grass chance, tree chance and tree silhouette for one biome. Never NULL: an id
// outside the table resolves to BIOME_PLAINS, the baseline, rather than walking off the end.
const BiomeParams* worldgenBiomeParams(BiomeId b);

// True where the top of the ground is sand rather than grass and dirt. One rule, one
// place: the column fill and the tree pass both ask this rather than each comparing
// against GEN_SAND_BELOW and drifting apart.
//
// **v1.8.3 Phase 2 splits the answer by generator version, and this is the single most
// safety-critical line of the rung.** A GEN_VERSION_DENSITY world resolves sand through the
// biome classifier — sand is the DESERT cap and nothing else — while a GEN_VERSION_LEGACY
// world keeps the original `worldgenBiome() < GEN_SAND_BELOW` test unchanged. The two rules
// disagree at roughly a fifth of all columns, so deleting the gate would move the beaches and
// deserts of every world on every SD card. testWorldgenLegacySandyWideSweep() exists for
// exactly that sabotage, and it asserts BOTH that the legacy answer still matches the old
// expression everywhere AND that the two rules were capable of disagreeing in the swept
// region — the second half being what stops the first from passing by landing somewhere the
// question is never asked.
bool worldgenIsSandy(const WorldGen* g, int32_t x, int32_t z);

// The raw cave field at a block: true where the two 3D fBms agree that this is hollow.
//
// **It does not know how deep the block is**, and deliberately so — the fill loop already
// has the surface height in a local and would otherwise pay for a second heightmap
// evaluation per block. GEN_CAVE_MIN_DEPTH is applied by the caller. What that means for
// the tests is that the depth rule is checked against the generated world (no air within
// five blocks of the surface) rather than against this function, which is the invariant
// that actually matters anyway.
//
// Always false below GEN_CAVE_FLOOR, so the bottom of the world is solid.
//
// **Stateless as of v1.8.7, and therefore safe to call from any thread.** It evaluates the
// two fBms the long way every time. The per-column corner cache that v1.8.7 added lives in a
// WorldGenScratch and is reached through worldgenIsCaveCached() below; this entry point kept
// the plain body so that a caller which has no lane — the suite's scattered sweeps, a debug
// probe — does not have to invent one to ask a question about the field.
bool worldgenIsCave(const WorldGen* g, int32_t x, int y, int32_t z);

// The same answer as worldgenIsCave(), out of the caller's per-column corner cache.
//
// **Bit-identical to worldgenIsCave() by construction and by measurement**: the cache holds
// the same lattice corners the fBm would have hashed, interpolated in world/noise.c's order
// with world/noise.c's normalisation. Verified over 3,436,800 cells across 4 seeds — every
// cell of a 5 x 5 column grid plus a 160,000-sample interleaved sweep that crosses columns on
// every call and runs y from -8 to WORLD_HEIGHT + 39 — with zero mismatches.
//
// This is the form the two fill loops use, because they ask it up to 32,768 times for one
// column and the cache is 90 % of the octave work a column does. It is on `s` rather than on
// a file static so that two lanes carve their own columns without sharing a key.
bool worldgenIsCaveCached(const WorldGen* g, WorldGenScratch* s, int32_t x, int y, int32_t z);

// Generates one column (all 8 chunks at cx, cz) into the world. False if the block
// budget or the column table refused an allocation — a real condition the caller must
// report rather than draw a hole for. Generating a column that already exists overwrites
// it, so this is not a way to preserve player edits; that is Phase 8's job.
//
// `s` is this thread's scratch and must not be shared with another thread that is inside the
// generator at the same time — see WorldGenScratch above for the measurement that says so.
bool worldgenColumn(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz);

// The scatter pass for one column: tall grass (roadmap task 19). Called by worldgenColumn
// after worldgenDecorate, and only for a GEN_VERSION_DENSITY world.
//
// **The order is the whole safety argument.** It runs LAST and writes only into cells that
// are BLOCK_AIR at the moment it looks, so a trunk, a leaf or a block of water standing where
// a plant would have gone simply is not air and the plant is not placed. That is a structural
// impossibility rather than a probability, and it is the right way round: a plant skipped
// under a canopy costs nothing, while a leaf skipped over a plant would be a hole in a tree.
//
// **v1.8.3 Phase 3 adds a SECOND pass inside this one**, run after the tall grass and under
// the same rule: cactus and dead bush on desert sand, fern on taiga and jungle grass. It is a
// separate loop with a separate salt, not a second draw on the tall-grass hash — see
// GEN_FLORA_CHANCE_MAX above for why the bound could not be shared, and world/worldgen.c for
// why the salt could not be. The ordering between the two is fixed and one-way: tall grass
// goes down first, so a contested cell is tall grass and the fern is the one silently not
// placed. Neither pass depends on which COLUMN was generated first.
//
// Returns false only if the world refused an allocation, or if the density generator's
// per-column surface heights are not the ones for (cx, cz) — see wgdColumnTops().
//
// `s` must be the same scratch the immediately preceding wgdColumn() filled, because that is
// where the surface heights it reads live.
bool worldgenScatter(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz);

// The decoration pass for one column: trees. Called by worldgenColumn after the ground is
// in, and exposed so the tests can run it on its own.
//
// It scans every tree whose canopy could reach this column — including trees whose trunks
// stand in a neighbouring column — and writes only the blocks that land inside (cx, cz).
// That clip is what makes the pass order-independent: a tree that wrote into its neighbour
// directly would be erased when that neighbour's ground was filled, which happens earlier,
// later, or in another session depending on where the player walked.
//
// **No scratch parameter, and that is a fact about the pass rather than an omission.** Every
// input it has is a hash of a tree cell or a heightmap query, both pure functions of the
// WorldGen; it carries nothing between calls. It was already re-entrant before v1.8.7 and
// giving it a scratch it would not read would only suggest otherwise.
bool worldgenDecorate(const WorldGen* g, World* w, int32_t cx, int32_t cz);

// Generates a square of columns centred on (cx, cz), radius in columns. Returns the
// number of columns that failed, so a caller can report "generated 289, refused 3"
// instead of silently drawing a world with holes in it.
int worldgenArea(const WorldGen* g, WorldGenScratch* s, World* w, int32_t cx, int32_t cz,
                 int radius);
