// v1.8.11. The worm-cave carver: docs/plan-1.8.11-caves.md, build order steps 1-7.
//
// **Column-independent by construction, the same idiom worldgenDecorate() already uses for
// trees (worldgen.c:793, comment at :718-725).** Every tunnel/room "system" is a pure function
// of (g->seed, region_x, region_z, system_index) -- a region is one column's own 16x16
// footprint, following treeInCell()'s clip convention exactly: `(x >> 4) == cx && (z >> 4) ==
// cz`. To generate column (cx, cz), every region in a fixed neighbourhood around it is
// re-hashed and, for the ones that roll a system, the system's entire path is re-walked from
// scratch and clipped to this column's own bounds. Two columns that share a region in their
// neighbourhood independently re-derive bit-identical geometry for it -- there is no cache, no
// order, nothing to disagree about. See plan-1.8.11-caves.md 2.1 for the proof.
//
// **Gated on GEN_VERSION_CAVES (genversion.h) and nothing below it** -- every world stamped
// LEGACY, DENSITY or BIOME keeps exactly the noise-field cave test it has today
// (worldgenIsCaveCached(), untouched by this file). Plan 2.6.
//
// **Two-lane safety.** Every byte of state this pass touches -- the per-column carve mask and
// the walker's own step state -- lives in the caller-owned WorldGenScratch (worldgen_scratch.h).
// Nothing here is a file static; that was the exact shape of the pre-v1.8.7 corruption bug
// (worldgen_scratch.h:1-8). caveCarveBuildMask() reads only its arguments.
//
// **Out of scope, deliberately (build order step 8 and later, not implemented here).** Lava,
// ravines, water, and the determinism/two-lane suite coverage that would normally close out
// this feature per the plan's own step 11. Lava needs a new BLOCK_LAVA id, which moves the
// registry CRC and forces a coordinated server release -- see plan 3.2 and this change's own
// task brief. Everything in this file stops one step short of that on purpose.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/noise.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

// ── Region geometry ───────────────────────────────────────────────────────────────────
//
// A region is one column's own footprint: 16x16 blocks, aligned to CHUNK_DIM, so "region
// (rx, rz)" and "column (cx, cz)" are literally the same coordinate. Plan 2.2.
#define CAVE_REGION_DIM CHUNK_DIM

// **max_reach, a hard clamp** (plan 2.2, risk 2): the largest distance in blocks any carved
// cell of a system can be from its own region's footprint. Enforced inside the walk itself,
// not assumed. Started conservative per plan 2.2's table and 8's open-decision note -- 24
// blocks, R = 3, 49 regions scanned per column -- [reasoned], to be widened later only from a
// host measurement showing headroom, never tightened after the fact from a wide guess.
#define CAVE_MAX_REACH 24

// R = ceil((max_reach + CHUNK_DIM) / CHUNK_DIM), plan 2.2's formula, evaluated for the constant
// above. _Static_assert in cave_carve.c pins this against the formula so the two cannot drift.
#define CAVE_NEIGHBOURHOOD_R 3

// ── Per-region draw ───────────────────────────────────────────────────────────────────
//
// Legacy-shaped gate, "roughly 1-in-10" per plan 4.2 (between the sourced 1-in-7 and 1-in-15).
// Out of 256 low bits of the region hash.
#define CAVE_REGION_CHANCE 26

// At most one system per region that rolls "yes". Plan 2.1 step 2 says "zero or more"; this
// build keeps it to at most one, which is the conservative default consistent with 4.2's own
// "1-3 real walks per column" assumption and is the simplest thing that satisfies the plan's
// determinism proof (still a pure function of (seed, rx, rz), just with system_index always 0).
#define CAVE_SYSTEMS_PER_REGION 1

// Of the regions that roll a system, the fraction that are a room instead of a walk (plan 2.4).
// [reasoned], not tuned -- a room is the rarer, larger feature in every sourced lineage
// (caves-legacy-console.md 2, point 6).
#define CAVE_ROOM_CHANCE 32

// Of the regions that roll a walk (not a room), the fraction that also spawn one branch (plan
// 2.4's "called recursively or once-only" -- this build calls it once-only, at the walk's own
// midpoint, off the same deterministic Rng stream so the branch stays a pure function of the
// system's identity with no extra state). [reasoned].
#define CAVE_BRANCH_CHANCE 64

// ── The walk ──────────────────────────────────────────────────────────────────────────

#define CAVE_MIN_STEPS 24
#define CAVE_MAX_STEPS 64
#define CAVE_STEP_LEN  FX_ONE   // one block advanced per step, 16.16

// Tapered radius, smallest at both ends of a walk and largest near the middle (plan 2.3's
// "sine-tapered radius"). Horizontal radius in blocks; vertical is squashed per
// CAVE_VERT_SQUASH_NUM/DEN, matching the Beta/1.7.10 lineage's 0.5 ratio
// (caves-legacy-console.md 2, cited in plan 2.3) -- a design choice for playtesting per that
// document's own note, not a fact being imported exactly.
#define CAVE_MIN_RADIUS 1
#define CAVE_MAX_RADIUS 4
#define CAVE_VERT_SQUASH_NUM 1
#define CAVE_VERT_SQUASH_DEN 2

// A room's radius (plan 2.4: "a larger radius than any single step of an ordinary tunnel
// produces").
#define CAVE_ROOM_RADIUS 6

// A branch is a smaller, shorter copy of the same primitive.
#define CAVE_BRANCH_MIN_STEPS 8
#define CAVE_BRANCH_MAX_STEPS 20
#define CAVE_BRANCH_MAX_RADIUS 3

// Salts. New, and checked not to collide with any existing SALT_* in worldgen.c /
// worldgen_density.c (grepped before adding).
#define SALT_CAVE_SYS  0x43415653U   // 'CAVS' -- per-region system draw
#define SALT_CAVE_WALK 0x43415657U   // 'CAVW' -- the walk's own Rng stream seed

// ── Fixed-point quarter sine (plan 2.3: "nothing in this codebase has a fixed-point sine") ──
//
// angle is a uint16_t where a full 0..65535 turn is 0..2*pi, matching the natural wraparound
// the walker's own yaw already needs -- no modulo required anywhere this is used.
fx caveSin(uint16_t angle);
static inline fx caveCos(uint16_t angle) { return caveSin((uint16_t)(angle + 0x4000u)); }

// ── The pre-pass entry point (plan 3.1) ──────────────────────────────────────────────────
//
// Builds s->carve for column (cx, cz): zeroes it, scans the (2R+1)x(2R+1) region neighbourhood
// around (cx, cz), and for every system a region rolls, re-walks it and clips the carve stamp
// into this column's mask. Pure function of (g->seed, cx, cz) and nothing else -- safe to call
// from either lane, any order, any number of times, always the same answer.
//
// Caller's responsibility: only call this for g->version >= GEN_VERSION_CAVES (worldgen_density.c
// wgdColumn does; the legacy fill loop in worldgen.c is untouched and never calls this).
void caveCarveBuildMask(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz);

// The general form, with an explicit neighbourhood radius instead of CAVE_NEIGHBOURHOOD_R.
// caveCarveBuildMask() is a thin wrapper over this with radius == CAVE_NEIGHBOURHOOD_R -- same
// production behaviour, byte for byte. Exposed so the host suite can directly test that
// CAVE_NEIGHBOURHOOD_R is wide enough: scanning further than it (radius + 1, + 2, ...) must find
// nothing new, which is the practical form of the plan 2.2 guarantee that R is large enough for
// CAVE_MAX_REACH.
void caveCarveBuildMaskR(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz,
                          int32_t radius);

// Reads a bit already written by caveCarveBuildMask() for this same column. x_in_col/z_in_col
// are 0..CHUNK_DIM-1 (block offset within the column), y is a world height 0..WORLD_HEIGHT-1.
static inline bool caveCarveMaskGet(const WorldGenScratch* s, int32_t x_in_col, int y,
                                     int32_t z_in_col)
{
	if (y < 0 || y >= WORLD_HEIGHT)
		return false;
	return (s->carve[y][z_in_col] & (uint16_t)(1u << x_in_col)) != 0;
}
