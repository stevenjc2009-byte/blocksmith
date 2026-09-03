// v1.8.12. Ore veins: docs task brief "ORE-GEN" (make the six ore blocks landed in block.h
// actually generate). No plan doc exists yet for this feature; the reasoning that would
// normally live there is written into this file's comments instead.
//
// **Column-independent by construction, the exact idiom cave_carve.h documents for its own
// walker (cave_carve.h:3-11), reused here rather than reinvented.** Every ore "attempt" is a
// pure function of (g->seed, region_x, region_z, ore_kind, attempt_index) -- a region is one
// column's own 16x16 footprint (CHUNK_DIM), same as cave_carve's. To generate column (cx, cz),
// every region in a fixed neighbourhood around it is re-hashed and every ore kind's every
// attempt in that region is re-walked from scratch and clipped to this column's own bounds.
// Two columns that share a region in their neighbourhood independently re-derive bit-identical
// geometry for it -- there is no cache, no order, nothing to disagree about. See cave_carve.h's
// own proof; the argument transfers unchanged because the mechanism is unchanged.
//
// **The one extra property caves didn't need to prove: when two different ore kinds' veins
// both reach the same cell, which one wins is also order-independent.** oreGenBuildMaskR's
// scan is `for (rz = cz-radius..cz+radius) for (rx = cx-radius..cx+radius) for (kind) for
// (attempt))` -- a fixed ABSOLUTE sweep over (rx, rz) (not relative to which column is asking),
// so for any two regions R1 != R2 that are both in range of two different requesting columns,
// the sweep visits them in the same relative order (whichever has the smaller rz, or smaller rx
// on a tie) no matter which column triggered the scan. Combined with "first writer wins" at the
// cell level (oreStamp() in ore_gen.c skips a cell that already carries a kind), this means the
// winning kind at a shared cell is the same regardless of generation order or which column asks
// -- covered by ore_gen_test.c's cross-boundary test, which checks BLOCK IDENTITY, not just "an
// ore is here."
//
// **Why "first writer wins" is also the right MECHANIC, not just a convenient tie-break:**
// once a cell stops being BLOCK_STONE, no later ore's walk can claim it (the write site in
// worldgen_density.c only ever calls oreGenMaskGet() to decide what to draw INTO a cell that
// would otherwise be BLOCK_STONE -- an ore never overwrites another ore, or dirt, or air, or a
// carved cave cell). That is the same shape vanilla Minecraft's classic ore generator uses (a
// vein's placer only ever replaces a full stone block), so two overlapping veins here behave
// the way two overlapping veins behave there: whichever was placed first keeps the cell.
// Kinds are processed in ascending BLOCK_COAL_ORE..BLOCK_DIAMOND_ORE id order (registry.c's own
// ordering) -- a deliberate, defensible tie-break for THIS codebase, not a claim that it
// matches vanilla's own internal ore-generation order, which was not verified here.
//
// **Gated on GEN_VERSION_ORES (genversion.h) and nothing below it.** A world stamped LEGACY,
// DENSITY, BIOME or CAVES never calls oreGenBuildMask() and never reads an ore mask cell --
// wgdColumn() in worldgen_density.c guards both the build call and the write-site lookup behind
// `g->version >= GEN_VERSION_ORES`, the same >= idiom every other version gate in this codebase
// uses (worldgen.h's own house-convention comment: a biome world is a density world with more
// on top, never ==). Because ORES (5) >= CAVES (4), an ore world still gets caves too.
//
// **Two-lane safety.** All working state -- the three ore-mask bit-planes -- lives in the
// caller-owned WorldGenScratch (worldgen_scratch.h), never a file static, for the exact reason
// cave_carve.h documents at its own :17-20 (the pre-v1.8.7 corruption bug). oreGenBuildMask()
// reads only its arguments.
//
// **Stone-only, structurally, not by a runtime "is this ore-eligible" check.** wgdColumn()'s
// fill loop only reaches the write site for a SOLID, UNCARVED cell (cave-carved cells and the
// exposed/surface cells already took a different branch earlier in that loop -- see the call
// site comment in worldgen_density.c). The ore lookup only fires when that cell's provisional
// value is BLOCK_STONE. There is no code path where an ore can land on air, water, dirt, grass,
// sand, or a cell the cave carver already claimed -- the write site's own `if (block ==
// BLOCK_STONE && ...)` guard is the only place ore ever gets written, and it is the same
// guard whether the cell is on a mountain, underwater, or a cave ceiling.
//
// ── Y-band research (pre-1.18 "classic" Minecraft ore distribution, used as REFERENCE ONLY,
//    never literally copied -- see the task brief's own naming trap note: "the legacy
//    distribution" in THIS project's docs means this Y-band shape, not GEN_VERSION_LEGACY) ──
//
// [researched, Minecraft Wiki "Ore/Pre-1.18 distribution" and "Ore/Pre-1.17 distribution",
// cross-checked against the wiki's own prose where its auto-fetched table was internally
// inconsistent -- see the Redstone note below] the classic (Beta-through-1.17) generator placed
// ores as uniform-random blobs across a fixed Y-band per ore, except Lapis Lazuli, which used a
// triangular (peaked) distribution centred in its own band. Approximate per-chunk attempt
// counts and vein ("blob") sizes, vanilla's own numbers:
//   Coal:      Y 0-127 (full height), uniform,   20 attempts/chunk, blob size ~17
//   Iron:      Y 0-63,                uniform,   20 attempts/chunk, blob size ~9
//   Gold:      Y 0-31,                uniform,    2 attempts/chunk, blob size ~9
//   Redstone:  Y 0-15,                uniform,    8 attempts/chunk, blob size ~8
//                 [reasoned: the wiki's auto-fetched table gave a Redstone Y-range that
//                 directly contradicted the SAME page's own prose sentence -- "Redstone has
//                 the same layer and line-size statistics as Diamond, but is generated eight
//                 times per chunk" -- so the table row was treated as a fetch-tool table-
//                 extraction artifact and discarded in favour of the prose, corroborated
//                 against reliable background knowledge of classic Minecraft's well-known
//                 generation constants.]
//   Lapis:     Y 0-31, TRIANGULAR, peak near Y 16, 1 attempt/chunk, blob size ~7
//   Diamond:   Y 0-15,                uniform,    1 attempt/chunk, blob size ~8
//
// This project's WORLD_HEIGHT is 128 (world.h) -- [measured, world.h:24] -- which matches
// Beta 1.7.3's own build height exactly (worldgen_density.c's own file comment already
// documents that this generator models Beta 1.7.3's density shape). So unlike a modern
// Minecraft world (Y -64..320), NO RESCALING of the classic Y-bands above is needed: they are
// used here as absolute Y 0..127 world coordinates, unchanged.
//
// **The Y-bands above are where an attempt's ANCHOR is drawn, not a hard ceiling on every
// stamped cell.** The walk moves in y exactly like it moves in x/z (oreGenAttempt(), ore_gen.c),
// one block per step, so a vein anchored at the very edge of its band can carry up to
// (size_max - 1) cells past that edge before its own step budget runs out -- the same shape
// vanilla's blob generator has (a blob is not hard-clamped to the Y its center was drawn at
// either). [measured, ore_gen_test.c, 2026-09-03]: over a 324-column sweep, iron (band 0-63,
// size_max 9) reached y=67, and gold/redstone/diamond showed similar single-digit overshoots --
// all well inside the size_max-bounded worst case, nowhere close to it. ore_gen_test.c's own
// k_bounds table checks against [band +/- (size_max - 1)] for exactly this reason, not the bare
// band.
//
// This file's own s_ore_table (ore_gen.c) keeps vanilla's attempt counts unchanged as a
// starting point -- [reasoned: per the task brief, measure first, only reduce if the ARM11
// budget is blown] -- but replaces vanilla's true ellipsoid "blob" placer with a simple
// 6-connected unit-step random walk (see oreGenAttempt() in ore_gen.c), because a walk is the
// primitive this codebase already has proven determinism machinery for (cave_carve.c's own
// walker) and is far cheaper on ARM11 (no float ellipsoid-membership test per candidate cell,
// just integer position bookkeeping and one rngBelow() draw per step). Vein SIZE ranges
// (size_min..size_max in the table) are this file's own [reasoned] adaptation to a walk-shaped
// primitive, not a vanilla-exact figure -- vanilla's blob generator doesn't have a natural
// "min size," so a range was chosen to give each vein some size variety while keeping the
// average close to vanilla's quoted blob size.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

// ── Region geometry (identical shape to cave_carve.h's, deliberately) ───────────────────────
#define ORE_REGION_DIM CHUNK_DIM

// Hard clamp, in blocks, on how far any stamped cell of an attempt's walk can be from that
// attempt's own anchor point. Enforced inside the walk (ore_gen.c), not assumed. Sized to
// comfortably clear the largest vein size in the table below (coal, size_max 17) with margin,
// the same conservative-with-headroom approach cave_carve.h's own CAVE_MAX_REACH comment
// documents. [reasoned] -- to be widened later only from a host measurement showing an attempt
// was clipped short by the clamp before finishing its rolled size, never tightened from a
// guess.
#define ORE_MAX_REACH 20

// R = ceil((ORE_MAX_REACH + CHUNK_DIM) / CHUNK_DIM), cave_carve.h's own formula (its :49-50),
// evaluated for the constant above. _Static_assert in ore_gen.c pins this against the formula
// so the two cannot drift.
#define ORE_NEIGHBOURHOOD_R 3

// Salt. New, and checked not to collide with any existing SALT_* in worldgen.c,
// worldgen_density.c, or cave_carve.h (grepped before adding: SALT_BIOME, SALT_TREE,
// SALT_GRASS, SALT_CAVE, SALT_CAVE2, SALT_HUMID, SALT_FLORA, SALT_TSHAPE, SALT_APPLE,
// SALT_DLOW, SALT_DHIGH, SALT_DSEL, SALT_CAVE_SYS, SALT_CAVE_WALK -- none spell 'OREW').
// A single salt covers the whole per-attempt Rng stream (anchor draw AND every walk step),
// the same way SALT_CAVE_WALK covers cave's entire per-system stream (cave_carve.c's own
// caveCarveSystem()) -- no separate "anchor" salt needed.
#define SALT_ORE_WALK 0x4F524557U   // 'OREW'

// ── The pre-pass entry point ─────────────────────────────────────────────────────────────────
//
// Builds the three ore-mask bit-planes in `s` for column (cx, cz): zeroes them, scans the
// (2R+1)x(2R+1) region neighbourhood around (cx, cz), and for every ore kind's every attempt
// in every scanned region, re-walks it and clips the stamp into this column's own bounds. Pure
// function of (g->seed, cx, cz) and nothing else -- safe to call from either lane, any order,
// any number of times, always the same answer.
//
// Caller's responsibility: only call this for g->version >= GEN_VERSION_ORES (worldgen_density.c
// wgdColumn() does; nothing else calls it).
void oreGenBuildMask(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz);

// The general form, with an explicit neighbourhood radius instead of ORE_NEIGHBOURHOOD_R.
// oreGenBuildMask() is a thin wrapper over this with radius == ORE_NEIGHBOURHOOD_R -- same
// production behaviour, byte for byte. Exposed so the host suite can directly test that
// ORE_NEIGHBOURHOOD_R is wide enough: scanning further than it (radius + 1, + 2, ...) must find
// nothing new, the same practical test cave_carve.h's own caveCarveBuildMaskR exposes for
// CAVE_NEIGHBOURHOOD_R.
void oreGenBuildMaskR(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz,
                      int32_t radius);

// Reads a cell already written by oreGenBuildMask()/oreGenBuildMaskR() for this same column.
// x_in_col/z_in_col are 0..CHUNK_DIM-1 (block offset within the column), y is a world height
// 0..WORLD_HEIGHT-1. Returns the ore BlockId stamped there, or BLOCK_AIR (0) if the cell has no
// ore -- BLOCK_AIR is used purely as a "none" sentinel here and never actually means "place
// air"; the caller (worldgen_density.c) only consults this for a cell it already knows is
// BLOCK_STONE, and only overwrites that cell when this returns something other than BLOCK_AIR.
BlockId oreGenMaskGet(const WorldGenScratch* s, int32_t x_in_col, int y, int32_t z_in_col);
