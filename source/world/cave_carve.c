#include "world/cave_carve.h"

#include <string.h>

#include "world/rng.h"

// This TU has no <3ds.h> and no divide by a runtime variable in any per-block hot path, the
// same discipline world/noise.c and world/worldgen.c already hold to (the ARM11 has no integer
// divide instruction). Two exceptions exist and are each called out where they happen: one
// divide by a runtime variable per WALK (the taper angle increment, steps_total is chosen once
// per walk and only 1-3 walks survive both region-scan early-outs per column, plan 4.2), and a
// divide by the CAVE_VERT_SQUASH_DEN *constant* per carve STEP, which GCC folds into a shift
// because the divisor is a compile-time literal, not a true divide instruction.

_Static_assert(CAVE_NEIGHBOURHOOD_R ==
               ((CAVE_MAX_REACH + CHUNK_DIM + CHUNK_DIM - 1) / CHUNK_DIM),
               "CAVE_NEIGHBOURHOOD_R must match plan 2.2's R = ceil((max_reach + CHUNK_DIM) / "
               "CHUNK_DIM) formula for CAVE_MAX_REACH, or a system could carve past the edge of "
               "the scanned neighbourhood and be missed");

// ── Fixed-point quarter sine ──────────────────────────────────────────────────────────
//
// 65 entries, i = 0..64, table[i] = round(FX_ONE * sin(i * (pi/2) / 64)) -- i.e. one full
// quarter turn at 6-bit (64-step) resolution, computed offline and pinned as literals so the
// host and the ARM11 read the identical bits (the same reason every other noise table in this
// codebase is integer, not computed with libm at startup). table[0] == 0, table[64] == FX_ONE
// exactly.
#define CAVE_SIN_BITS 6
#define CAVE_SIN_N ((1 << CAVE_SIN_BITS) + 1)   // 65

static const fx s_cave_sin_table[CAVE_SIN_N] = {
	0, 1608, 3216, 4821, 6424, 8022, 9616, 11204,
	12785, 14359, 15924, 17479, 19024, 20557, 22078, 23586,
	25080, 26558, 28020, 29466, 30893, 32303, 33692, 35062,
	36410, 37736, 39040, 40320, 41576, 42806, 44011, 45190,
	46341, 47464, 48559, 49624, 50660, 51665, 52639, 53581,
	54491, 55368, 56212, 57022, 57798, 58538, 59244, 59914,
	60547, 61145, 61705, 62228, 62714, 63162, 63572, 63944,
	64277, 64571, 64827, 65043, 65220, 65358, 65457, 65516,
	65536,
};

fx caveSin(uint16_t angle)
{
	// angle: 0..65535 = one full turn. Top 2 bits pick the quadrant; quadrants 1 and 3 mirror
	// the quarter wave (sin(90+x) == sin(90-x)), quadrants 2 and 3 are the negated half.
	const int quadrant = angle >> 14;
	uint16_t rem = (uint16_t)(angle & 0x3FFFu);   // 0..16383, offset within the quadrant
	if (quadrant & 1)
		rem = (uint16_t)(0x4000u - rem);
	// Round to the nearest table step rather than floor, halving the worst-case quantisation
	// error near the steep parts of the curve (measured: ~0.024 floored vs ~0.012 rounded,
	// against libm, over a full-turn sweep -- see the host test's printed max_err). +128 is
	// half of one table step (256 raw angle units per step at 6-bit resolution).
	const uint32_t idx = (((uint32_t)rem) + 128u) >> (14 - CAVE_SIN_BITS);   // 0..64
	const fx v = s_cave_sin_table[idx];
	return (quadrant >= 2) ? -v : v;
}

// 16.16 multiply. Not shared via noise.h -- noise.h is outside this change's file-ownership
// list -- so this is a local copy of the exact inline idiom noise.c already uses at each of its
// own call sites (world/noise.c:31-40).
static inline fx fxMul(fx a, fx b)
{
	return (fx)(((int64_t)a * (int64_t)b) >> FX_SHIFT);
}

// ── The carve stamp (plan 2.3) ────────────────────────────────────────────────────────
//
// Tests every integer cell in a local bounding box around (cxw, cyw, czw) against the
// divide-free ellipsoid inequality dx^2*rv^2 + dy^2*rh^2 + dz^2*rv^2 < rh^2*rv^2 -- the single-
// inequality technique plan 2.3 cites, expanded out of dx^2 + k*dy^2 + dz^2 < r^2 with
// k = (rh/rv)^2 to clear the division rv would otherwise need per cell. A cell outside the
// target column's own bounds never reaches that inequality at all -- treePut()'s clip idiom
// (worldgen.c:726-737), `(coord >> 4) == column`, solved for dz/dx ahead of the loop (Tier-1
// opt, lane OPT-WORLDGEN, 2026-09-02) rather than tested per cell inside it.
static void caveCarveStamp(WorldGenScratch* s, int32_t cx, int32_t cz,
                            int32_t cxw, int32_t cyw, int32_t czw, int32_t rh, int32_t rv)
{
	if (rh < 1) rh = 1;
	if (rv < 1) rv = 1;

	const int32_t col_x0 = cx * CHUNK_DIM;
	const int32_t col_z0 = cz * CHUNK_DIM;

	const int64_t rh2 = (int64_t)rh * rh;
	const int64_t rv2 = (int64_t)rv * rv;
	const int64_t bound = rh2 * rv2;

	// Tier-1 opt (lane OPT-WORLDGEN, 2026-09-02): the per-cell "(coord >> 4) == column" clip
	// above solved for dz/dx ahead of the loop instead of tested inside it -- the same
	// inequality (col_z0 <= czw+dz <= col_z0+15, and the x equivalent), so this reaches exactly
	// the same (dy, dz, dx) triples the old per-iteration clip accepted, just without spending a
	// loop iteration (and, for dz, an inner dx loop entry) on the ones it always rejected. rh is
	// caller-bounded well under CHUNK_DIM (CAVE_ROOM_RADIUS, the largest caller passes, is 6), so
	// dz_lo can exceed dz_hi -- a stamp whose whole box misses this column on one axis -- which
	// the early return below turns into zero wasted iterations instead of (2*rh+1)^2 rejected
	// ones.
	int32_t dz_lo = col_z0 - czw, dz_hi = col_z0 + CHUNK_DIM - 1 - czw;
	if (dz_lo < -rh) dz_lo = -rh;
	if (dz_hi > rh)  dz_hi = rh;

	int32_t dx_lo = col_x0 - cxw, dx_hi = col_x0 + CHUNK_DIM - 1 - cxw;
	if (dx_lo < -rh) dx_lo = -rh;
	if (dx_hi > rh)  dx_hi = rh;

	if (dz_lo > dz_hi || dx_lo > dx_hi)
		return;   // this stamp's box cannot touch the column at all

	for (int32_t dy = -rv; dy <= rv; dy++) {
		const int32_t y = cyw + dy;
		if (y < GEN_CAVE_FLOOR || y >= WORLD_HEIGHT)
			continue;
		const int64_t termY = (int64_t)dy * dy * rh2;
		if (termY >= bound)
			continue;

		for (int32_t dz = dz_lo; dz <= dz_hi; dz++) {
			const int32_t wz = czw + dz;
			const int64_t termYZ = termY + (int64_t)dz * dz * rv2;
			if (termYZ >= bound)
				continue;

			for (int32_t dx = dx_lo; dx <= dx_hi; dx++) {
				const int32_t wx = cxw + dx;
				const int64_t term = termYZ + (int64_t)dx * dx * rv2;
				if (term >= bound)
					continue;

				const int32_t xl = wx - col_x0, zl = wz - col_z0;
				s->carve[y][zl] = (uint16_t)(s->carve[y][zl] | (uint16_t)(1u << xl));
			}
		}
	}
}

// ── The walk (plan 2.3), rooms (2.4) and one branch (2.4's "recursively or once-only") ──
//
// Every byte of state a walk in progress needs lives in this function's own stack frame (px/py/
// pz, yaw/pitch and their velocities, the step counter) or in the caller-supplied Rng* stream --
// nothing is static, nothing outlives this call, so two lanes calling this concurrently for
// disjoint columns cannot see each other's state (plan 2.7). The branch is one recursive call,
// depth-capped at 1 (allow_branch is false on the recursive call), continuing to draw from the
// SAME Rng* stream rather than starting a new one, which is what keeps the branch a pure
// function of the system's own identity (seed, rx, rz, sys) with no extra state to seed it from.
static void caveWalkLoop(Rng* r, WorldGenScratch* s, int32_t cx, int32_t cz,
                          int32_t sx, int32_t sy, int32_t sz,
                          fx px, fx py, fx pz, uint16_t yaw, int32_t pitch,
                          int steps_total, int32_t max_radius, bool allow_branch)
{
	if (steps_total < 2) steps_total = 2;

	// ONE divide by a runtime variable (steps_total), spent once per walk, not once per step
	// and never once per block -- see the file-top comment. 0x8000 is a half turn: caveSin()
	// over step*taper_inc for step in [0, steps_total) sweeps 0 -> FX_ONE -> back towards 0,
	// which is exactly "smallest at both ends, largest near the middle" (plan 2.3).
	const uint16_t taper_inc = (uint16_t)(0x8000u / (uint32_t)steps_total);

	int16_t yaw_vel = 0, pitch_vel = 0;
	const int branch_at = (allow_branch && rngBelow(r, 256) < CAVE_BRANCH_CHANCE)
		? steps_total / 2 : -1;

	for (int step = 0; step < steps_total; step++) {
		// Damped drift: velocity decays toward zero (>>2, a quarter kept back each step) plus
		// a fresh random kick, so the path curves smoothly rather than jittering (plan 2.3,
		// caves-legacy-console.md 2's momentum idiom).
		yaw_vel = (int16_t)(yaw_vel - (yaw_vel >> 2) +
		                     (int16_t)((int32_t)rngBelow(r, 1024) - 512));
		pitch_vel = (int16_t)(pitch_vel - (pitch_vel >> 2) +
		                       (int16_t)((int32_t)rngBelow(r, 512) - 256));

		yaw = (uint16_t)(yaw + yaw_vel);
		pitch += pitch_vel;
		// Clamped away from straight up/down -- an unbounded pitch can wrap through vertical
		// and back, which reads as a shaft rather than the "long flat tunnel systems" plan 2.3
		// (citing caves-legacy-console.md 2) describes this feature chasing. [reasoned], not
		// tuned by playtesting -- see plan 8 on why the final feel is not this document's call.
		if (pitch > 0x3000)  { pitch = 0x3000;  pitch_vel = 0; }
		if (pitch < -0x3000) { pitch = -0x3000; pitch_vel = 0; }

		const fx cp  = caveCos((uint16_t)pitch);
		const fx sdx = fxMul(cp, caveCos(yaw));
		const fx sdz = fxMul(cp, caveSin(yaw));
		const fx sdy = caveSin((uint16_t)pitch);

		const fx nx = px + fxMul(sdx, CAVE_STEP_LEN);
		const fx ny = py + fxMul(sdy, CAVE_STEP_LEN);
		const fx nz = pz + fxMul(sdz, CAVE_STEP_LEN);

		// The hard max_reach clamp (plan 2.2/risk 2), measured from the SYSTEM's own start
		// point -- sx/sy/sz never change across a branch's recursive call, so a branch is
		// bounded by the same budget as its parent, which is what keeps the neighbourhood
		// radius formula's guarantee (plan 2.2) true for the whole system, branches included.
		const int64_t ddx = fxToInt(nx) - sx;
		const int64_t ddy = fxToInt(ny) - sy;
		const int64_t ddz = fxToInt(nz) - sz;
		if (ddx * ddx + ddy * ddy + ddz * ddz > (int64_t)CAVE_MAX_REACH * CAVE_MAX_REACH)
			break;

		px = nx; py = ny; pz = nz;

		const uint16_t t_angle = (uint16_t)((uint32_t)step * taper_inc);
		const fx taper = caveSin(t_angle);   // 0 at both ends of the walk, FX_ONE mid-walk
		int32_t rh = CAVE_MIN_RADIUS +
			fxToInt((fx)((int64_t)(max_radius - CAVE_MIN_RADIUS) * taper));
		if (rh > max_radius)      rh = max_radius;
		if (rh < CAVE_MIN_RADIUS) rh = CAVE_MIN_RADIUS;
		// Divide by the CAVE_VERT_SQUASH_DEN *constant* (2) -- GCC folds a compile-time-
		// constant divisor into a shift; this is not the runtime-variable divide the file-top
		// comment flags, and there is exactly one of those per walk, not per step.
		const int32_t rv = rh / CAVE_VERT_SQUASH_DEN;

		caveCarveStamp(s, cx, cz, fxToInt(px), fxToInt(py), fxToInt(pz), rh, rv);

		if (step == branch_at) {
			const uint16_t byaw   = (uint16_t)rngNext(r);
			const int32_t  bpitch = (int32_t)rngBelow(r, 0x2000u) - 0x1000;
			const int bsteps = CAVE_BRANCH_MIN_STEPS +
				(int)rngBelow(r, CAVE_BRANCH_MAX_STEPS - CAVE_BRANCH_MIN_STEPS + 1);
			caveWalkLoop(r, s, cx, cz, sx, sy, sz, px, py, pz, byaw, bpitch,
			             bsteps, CAVE_BRANCH_MAX_RADIUS, false);
		}
	}
}

// One region's draw: derives the system's start point (a pure function of (seed, rx, rz, sys)),
// rejects it against the target column's bounds (early-out #2, treeInCell()'s own
// horizontal-reach-reject shape, worldgen.c:596-624), and otherwise carves a room or walks a
// tunnel -- both off the SAME Rng stream, seeded once, so every draw this system makes
// (start point, room-or-walk, initial heading, every step's drift and taper, the branch
// decision) is one deterministic replay of that one stream. No caching, no external state.
static void caveCarveSystem(uint32_t seed, int32_t rx, int32_t rz, int sys,
                             int32_t cx, int32_t cz, WorldGenScratch* s)
{
	Rng r;
	rngSeed(&r, rngHash3(seed ^ SALT_CAVE_WALK, rx, sys, rz));

	const int32_t bx0 = rx * CAVE_REGION_DIM;
	const int32_t bz0 = rz * CAVE_REGION_DIM;
	const int32_t sx = bx0 + 1 + (int32_t)rngBelow(&r, CAVE_REGION_DIM - 2);
	const int32_t sz = bz0 + 1 + (int32_t)rngBelow(&r, CAVE_REGION_DIM - 2);

	// Kept away from the world's own floor and ceiling by a margin at least as big as the
	// largest radius this build ever draws, so a room or a mid-taper tunnel cannot itself
	// reach past GEN_CAVE_FLOOR or WORLD_HEIGHT before the per-cell clamp in caveCarveStamp
	// even runs.
	const int32_t y_lo = GEN_CAVE_FLOOR + CAVE_ROOM_RADIUS + 2;
	const int32_t y_hi = WORLD_HEIGHT - CAVE_ROOM_RADIUS - 2;
	const int32_t sy = (y_hi > y_lo) ? y_lo + (int32_t)rngBelow(&r, (uint32_t)(y_hi - y_lo))
	                                 : y_lo;

	// Early-out #2 (plan 2.2/4.2): the system's own [start +/- CAVE_MAX_REACH] box against the
	// target column's bounds. A system whose start is this far away cannot reach the column no
	// matter which way it walks, because CAVE_MAX_REACH is the hard clamp caveWalkLoop enforces
	// on every step.
	const int32_t col_x0 = cx * CHUNK_DIM, col_x1 = col_x0 + CHUNK_DIM - 1;
	const int32_t col_z0 = cz * CHUNK_DIM, col_z1 = col_z0 + CHUNK_DIM - 1;
	if (sx + CAVE_MAX_REACH < col_x0 || sx - CAVE_MAX_REACH > col_x1 ||
	    sz + CAVE_MAX_REACH < col_z0 || sz - CAVE_MAX_REACH > col_z1)
		return;

	if (rngBelow(&r, 256) < CAVE_ROOM_CHANCE) {
		// Plan 2.4: a room is the same stamp primitive with zero steps, one ellipsoid, larger
		// than any single step of an ordinary tunnel.
		caveCarveStamp(s, cx, cz, sx, sy, sz, CAVE_ROOM_RADIUS,
		               CAVE_ROOM_RADIUS / CAVE_VERT_SQUASH_DEN);
		return;
	}

	const uint16_t yaw0 = (uint16_t)rngNext(&r);
	// Initial pitch biased toward horizontal (roughly +/-25% of a quarter turn either way),
	// matching the "long flat tunnel systems" this feature is chasing (plan 2.3).
	const int32_t pitch0 = (int32_t)rngBelow(&r, 0x2000u) - 0x1000;
	const int steps_total = CAVE_MIN_STEPS +
		(int)rngBelow(&r, CAVE_MAX_STEPS - CAVE_MIN_STEPS + 1);

	caveWalkLoop(&r, s, cx, cz, sx, sy, sz,
	             fxFromInt(sx), fxFromInt(sy), fxFromInt(sz),
	             yaw0, pitch0, steps_total, CAVE_MAX_RADIUS, true);
}

void caveCarveBuildMaskR(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz,
                          int32_t radius)
{
	// Tier-1 opt (lane OPT-WORLDGEN, 2026-09-02): same memset conversion as
	// worldgen_density.c's s->solid clear -- s->carve is the identical shape
	// (WORLD_HEIGHT x CHUNK_DIM uint16_t) and an all-zero fill either way.
	memset(s->carve, 0, sizeof(s->carve));

	for (int32_t rz = cz - radius; rz <= cz + radius; rz++) {
		for (int32_t rx = cx - radius; rx <= cx + radius; rx++) {
			// Early-out #1 (plan 2.2/4.2): one rngHash2-shaped call and a compare, per region.
			// Following the house convention every other decorator in worldgen.c uses
			// (rngHash2(rngMix(seed ^ SALT_X), rx, rz) -- worldgen.c:580's SALT_TREE call).
			const uint32_t h = rngHash2(rngMix(g->seed ^ SALT_CAVE_SYS), rx, rz);
			if ((h & 0xFFu) >= CAVE_REGION_CHANCE)
				continue;

			for (int sys = 0; sys < CAVE_SYSTEMS_PER_REGION; sys++)
				caveCarveSystem(g->seed, rx, rz, sys, cx, cz, s);
		}
	}
}

void caveCarveBuildMask(const WorldGen* g, WorldGenScratch* s, int32_t cx, int32_t cz)
{
	caveCarveBuildMaskR(g, s, cx, cz, CAVE_NEIGHBOURHOOD_R);
}
