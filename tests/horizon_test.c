// Host self-test for the horizon cull's predicate — source/scene/chunk_render.c's
// horizonHidden().
//
// v1.8.0 task 49h replaced two software atan2f per candidate, plus three more for every
// blocker column that got past the distance guard, with cross-multiplies. The claim that has
// to be proved is not "faster" — it is "the same cull, chunk for chunk". So this file runs the
// REAL function against a frozen copy of the code it replaced, over an enumerated
// configuration space that includes the degenerate cases nobody hits on purpose, and counts
// disagreements.
//
// The real function's source text is lifted out of chunk_render.c by tools/run_host_tests.sh —
// an awk range from its signature to its closing brace, plus the HZN_HALF_W define — and
// #included below as build-host/.../horizon_extract.inc. That is deliberate and it is the
// whole point of the file: chunk_render.c cannot be compiled on the host, it includes <3ds.h>
// and <citro3d.h>, so the only alternative was to hand-copy the function in here — which is
// exactly the "a test that links nothing tests nothing" failure this project has already paid
// for twice (see tests/battery_test.c's note and run_host_tests.sh's meshq entry). Sabotage
// source/scene/chunk_render.c and this binary goes red; there is no second copy to drift.
//
// 2026-08-25: that last sentence was audited by actually doing it, eleven ways. Eight arms went
// red. Three did not, and each of the three has an entry below saying why and what was done
// about it — one was a real hole in this file (the oracle was written in terms of HZN_HALF_W,
// the constant it existed to watch) and is now fixed; two are production guards that provably
// cannot change an answer on a 16-block chunk grid, so their headroom is measured instead.
//
// The reference below is the OTHER half of that trade and it IS a copy — a frozen verbatim
// transcript of the pre-49h implementation, which by definition no longer exists in the tree.
// A reference oracle is allowed to be a copy; the code under test is not.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/chunk.h"   // CHUNK_DIM — the real one, not a number retyped here

static int  s_checks;
static int  s_fails;
static char s_first[200];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── The state horizonHidden() reads ────────────────────────────────────────────────────
//
// Same names and same types as chunk_render.c's, because the extracted function refers to
// them by name. HZN_MAX_COLUMNS is larger here than the renderer's 65 only so the sweeps can
// push denser blocker sets through the loop; the function does not read the bound.

#define HZN_MAX_COLUMNS 128

typedef struct {
	int   cx, cz;
	float top_y;
} HznCol;

static HznCol s_hzn_cols[HZN_MAX_COLUMNS];
static int    s_hzn_n;
static float  s_cam_x, s_cam_y, s_cam_z;

// The real thing. Generated at build time — see the file comment.
#include "horizon_extract.inc"

#ifndef HZN_HALF_W
#error "horizon_extract.inc did not carry HZN_HALF_W out of chunk_render.c"
#endif

// ── The frozen reference: chunk_render.c's horizonHidden() as it stood before 49h ───────
//
// Verbatim apart from the name and the s_/HZN_ symbols resolving to the ones above. Do not
// "improve" it and do not fold 49h's fixes back into it: its only job is to be the thing the
// console shipped, so that a difference here is a difference in the CULL.

#define HZN_PI  3.14159265f

static bool horizonHiddenRef(int cx, int cy, int cz)
{
	const float col_x = (float)(cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float col_z = (float)(cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float dx     = col_x - s_cam_x, dz = col_z - s_cam_z;
	const float dist   = sqrtf(dx * dx + dz * dz);

	if (dist < (float)CHUNK_DIM * 0.5f) return false;

	const float bearing = atan2f(dz, dx);
	const float top_y   = (float)((cy + 1) * CHUNK_DIM);
	const float elev    = atan2f(top_y - s_cam_y, dist);

	for (int c = 0; c < s_hzn_n; c++) {
		if (s_hzn_cols[c].cx == cx && s_hzn_cols[c].cz == cz) continue;

		const float ncx = (float)(s_hzn_cols[c].cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ncz = (float)(s_hzn_cols[c].cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ndx = ncx - s_cam_x, ndz = ncz - s_cam_z;
		const float ndist = sqrtf(ndx * ndx + ndz * ndz);

		if (ndist >= dist - 1.0f) continue;

		float dbear = atan2f(ndz, ndx) - bearing;
		while (dbear >  HZN_PI) dbear -= 2.0f * HZN_PI;
		while (dbear < -HZN_PI) dbear += 2.0f * HZN_PI;

		const float half_w = atan2f((float)CHUNK_DIM * 0.5f * 0.35f, ndist);
		if (fabsf(dbear) > half_w) continue;

		const float nelev = atan2f(s_hzn_cols[c].top_y - s_cam_y, ndist);
		if (nelev >= elev) return true;
	}
	return false;
}

// ── The oracle ─────────────────────────────────────────────────────────────────────────
//
// The reference above and the rewrite are not the only two answers available: there is a
// third, which is what the predicate MEANS. This is the reference's own algorithm — angles,
// wrap loops and all — evaluated in double instead of float, on bit-identical float inputs
// (dist and ndist stay float, so the distance guard is the same guard). Double gives ~1e-16
// where float gives ~1e-7, which is several orders of magnitude below the closest boundary any
// of the sweeps below constructs, so where the three disagree this one is right.
//
// It exists because a straight ref-vs-new count cannot say WHICH of them moved. With the
// oracle the claim becomes falsifiable in the direction that matters: the rewrite must match
// the intended predicate everywhere, and any place the shipped float code did not is a bug in
// the shipped float code, not a behaviour change introduced here.
//
// 2026-08-25 sabotage audit — WHY THE HALF-WIDTH BELOW IS A FROZEN LITERAL AND NOT HZN_HALF_W.
// This oracle used to read chunk_render.c's own HZN_HALF_W, i.e. it was written in terms of the
// constant it exists to watch. Measured: shrinking that define's 0.35f to 0.05f moved
// 73,490 of 697,968 cases (ref-vs-new went 6 -> 73,490) and this file still printed
// "horizon self-test: PASS  7 checks", because the oracle moved with the sabotage and
// s_dis_new_exact stayed 0. Both remaining claims survived too: ref-vs-new and ref-vs-exact
// stayed equal to each other because new and exact had moved together, and the direction check
// held because a narrower cone only ever KEEPS chunks. Zero checks went red over a 10.5% change
// in what the renderer culls. Same mechanism as the WATER_SPREAD_TICKS case in
// code-vault blocksmith-lesson-sabotage-arm-must-actually-arm.md: a check parameterised by the
// value under test cannot detect that value changing. So the oracle now carries the shipped
// value as a naked literal — the same one horizonHiddenRef above already hardcodes — and the
// production define is pinned separately in main().
#define HZN_PI_D  3.14159265358979323846   /* -std=c11 does not give us M_PI */

// The cone half-extent as the console shipped it: CHUNK_DIM/2 shrunk to 35%. Frozen here on
// purpose. Retuning chunk_render.c's HZN_HALF_W must go red, both here and on main()'s pin.
#define HZN_HALF_W_FROZEN  ((float)CHUNK_DIM * 0.5f * 0.35f)

// ── Redundancy witnesses ───────────────────────────────────────────────────────────────
//
// Two guards inside horizonHidden() cannot change its answer at all on a 16-block chunk grid,
// which the same audit established by sabotaging each of them and getting byte-identical
// output across all 697,968 cases:
//
//   * `ndist >= dist - 1.0f`  (the one-block anti-flicker hysteresis) — changing it to
//     `ndist >= dist` moved nothing. For the margin to decide anything a blocker would have to
//     sit within 1 block of the candidate radially AND inside the bearing cone, which at these
//     distances is 2.8 blocks transversally: a separation under 3 blocks between two DISTINCT
//     column centres, which are 16 apart by construction.
//   * `dist < (float)CHUNK_DIM * 0.5f`  (the near-camera bail) — changing it to `dist < 0.0f`,
//     i.e. never firing, moved nothing. A cull needs ndist < dist - 1 < 7 with dist < 8, so the
//     two centres would be under 15 apart. Again impossible for distinct columns.
//   * (A third, `dot <= 0.0`, is redundant by algebra rather than by geometry: with dot <= 0
//     the cone test's right-hand side is <= 0 and its left-hand side >= 0, so the cone rejects
//     the blocker anyway. Nothing needs to witness that one.)
//
// So no behavioural check in this file can ever redden those two, and pretending otherwise
// would be the "green over a broken feature" failure this project keeps paying for. What is
// asserted instead is the HEADROOM that makes them unreachable, measured rather than argued.
// The oracle is run three ways per case — normally, with the hysteresis dropped, and with the
// near bail dropped — and two numbers come out of the dropped passes:
//
//   * s_margin_slack: the smallest `dist - ndist` over every blocker that got past the BEARING
//     CONE, measured with the hysteresis disabled so the true distribution is visible. The
//     hysteresis rejects a cone-passer exactly when that quantity is <= 1.0, so asserting it
//     stays above 1.0 is a live, quantitative check with the measured margin printed next to
//     it — not a boolean that only moves under an absurd edit.
//   * s_near_slack: the smallest candidate `dist` at which the oracle ever culled anything,
//     measured with the near bail disabled. This one is REPORTED, not asserted — see main().
//     The near bail's actual check is s_dis_near, the count of cases the bail changes.
//
// A first attempt used plain disagreement counters here. The near one was live — widening the
// bail to CHUNK_DIM * 4.0f moved 26,663 cases — but the margin one was NOT: widening the
// hysteresis from 1.0f all the way to 12.0f still moved zero cases, because 12 blocks of radial
// slop plus 2.8 of transverse is still under one 16-block column spacing. A check that needs a
// sixteenfold edit before it can fire is barely a check, which is why the margin side is a
// measured slack now. The near-bail counter is kept because it is already proven live.
static int    s_orc_drop_margin;   // use `ndist >= dist` instead of `ndist >= dist - 1.0f`
static int    s_orc_drop_near;     // skip the near-camera bail entirely
static double s_margin_slack = 1e300;
static double s_near_slack   = 1e300;

static bool horizonHiddenExact(int cx, int cy, int cz)
{
	const float col_x = (float)(cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float col_z = (float)(cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float dx     = col_x - s_cam_x, dz = col_z - s_cam_z;
	const float dist   = sqrtf(dx * dx + dz * dz);

	if (!s_orc_drop_near && dist < (float)CHUNK_DIM * 0.5f) return false;

	const double bearing = atan2((double)dz, (double)dx);
	const float  top_y   = (float)((cy + 1) * CHUNK_DIM);
	const double elev    = atan2((double)top_y - (double)s_cam_y, (double)dist);

	for (int c = 0; c < s_hzn_n; c++) {
		if (s_hzn_cols[c].cx == cx && s_hzn_cols[c].cz == cz) continue;

		const float ncx = (float)(s_hzn_cols[c].cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ncz = (float)(s_hzn_cols[c].cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ndx = ncx - s_cam_x, ndz = ncz - s_cam_z;
		const float ndist = sqrtf(ndx * ndx + ndz * ndz);

		if (ndist >= (s_orc_drop_margin ? dist : dist - 1.0f)) continue;

		double dbear = atan2((double)ndz, (double)ndx) - bearing;
		while (dbear >  HZN_PI_D) dbear -= 2.0 * HZN_PI_D;
		while (dbear < -HZN_PI_D) dbear += 2.0 * HZN_PI_D;

		const double half_w = atan2((double)HZN_HALF_W_FROZEN, (double)ndist);
		if (fabs(dbear) > half_w) continue;

		// Past the cone. With the hysteresis disabled this is the population it would have
		// been judging, so this is where its headroom is measured.
		if (s_orc_drop_margin && (double)(dist - ndist) < s_margin_slack)
			s_margin_slack = (double)(dist - ndist);

		const double nelev = atan2((double)s_hzn_cols[c].top_y - (double)s_cam_y,
		                           (double)ndist);
		if (nelev >= elev) {
			// With the near bail disabled, the closest candidate anything ever culls.
			if (s_orc_drop_near && (double)dist < s_near_slack)
				s_near_slack = (double)dist;
			return true;
		}
	}
	return false;
}

// ── Bookkeeping ────────────────────────────────────────────────────────────────────────

static long s_cases;
static long s_dis_ref_new;      // shipped float angles vs the rewrite
static long s_dis_new_exact;    // the rewrite vs what the predicate means  -- must be 0
static long s_dis_ref_exact;    // shipped float angles vs what it means
static long s_dis_ref_kept;     // of s_dis_ref_new, how many the rewrite KEEPS and ref culled
static long s_true_ref;         // how many of the cases actually culled, in the reference
static long s_dis_near;         // oracle with the near-camera bail dropped    -- must be 0
static char s_dis_first[320];

// The one assumption the rewrite makes that is not algebra: that ndist == 0.0f can only mean
// both components are exactly zero. It is true because a nonzero component at this world's
// coordinate magnitudes is at least a float ULP there, far too large to square to underflow —
// but "far too large" is a reasoning step, so every configuration the sweeps build gets
// checked rather than trusted.
static long s_zero_ndist;

static void checkDegenerateDistances(void)
{
	for (int c = 0; c < s_hzn_n; c++) {
		const float ncx = (float)(s_hzn_cols[c].cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ncz = (float)(s_hzn_cols[c].cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ndx = ncx - s_cam_x, ndz = ncz - s_cam_z;
		if (sqrtf(ndx * ndx + ndz * ndz) == 0.0f) {
			s_zero_ndist++;
			if (ndx != 0.0f || ndz != 0.0f) {
				// A zero distance from nonzero components would mean the branch in the
				// rewrite is answering a case it was not written for.
				CHECK(ndx == 0.0f && ndz == 0.0f);
			}
		}
	}
}

static void compareAt(int cx, int cy, int cz)
{
	const bool ref = horizonHiddenRef(cx, cy, cz);
	const bool now = horizonHidden(cx, cy, cz);
	const bool exa = horizonHiddenExact(cx, cy, cz);

	s_cases++;
	if (ref) s_true_ref++;
	if (now != exa) s_dis_new_exact++;
	if (ref != exa) s_dis_ref_exact++;

	// The two redundancy witnesses — see the comment above horizonHiddenExact. The
	// drop-margin pass is run for its side effect on s_margin_slack, not for a verdict.
	s_orc_drop_margin = 1;
	(void)horizonHiddenExact(cx, cy, cz);
	s_orc_drop_margin = 0;

	s_orc_drop_near = 1;
	if (horizonHiddenExact(cx, cy, cz) != exa) s_dis_near++;
	s_orc_drop_near = 0;

	if (ref != now) {
		s_dis_ref_new++;
		if (ref && !now) s_dis_ref_kept++;
		if (!s_dis_first[0])
			snprintf(s_dis_first, sizeof(s_dis_first),
			         "cam(%.9g,%.9g,%.9g) cand(%d,%d,%d) n=%d ref=%d new=%d exact=%d",
			         (double)s_cam_x, (double)s_cam_y, (double)s_cam_z,
			         cx, cy, cz, s_hzn_n, (int)ref, (int)now, (int)exa);
	}
}

// ── Deterministic RNG ──────────────────────────────────────────────────────────────────
//
// xorshift32 rather than rand(), so the case count and the disagreement count are the same
// numbers on every machine and in every re-run — a fuzz result nobody can reproduce is an
// anecdote.

static uint32_t s_rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static int rndRange(int lo, int hi)   // inclusive
{
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static float rndFloat(float lo, float hi)
{
	return lo + (hi - lo) * ((float)(rnd() >> 8) / (float)(1u << 24));
}

// ── Scenario 1: the shape the renderer actually produces ───────────────────────────────
//
// A full 7x7 ring of blocker columns with terrain-shaped tops around a camera that is walked
// through every interesting sub-position inside its own column: the exact centre (which is
// the one that makes ndist zero), the exact corner, the exact edge, and off-grid fractions.

static void sweepRing(void)
{
	static const float kFrac[] = { 0.0f, 0.5f, 8.0f, 8.5f, 1.0f / 3.0f, 15.9999f, 7.25f };
	static const float kEye[]  = { 0.0f, 16.0f, 64.0f, 70.5f, 128.0f, -8.0f };

	for (size_t fi = 0; fi < sizeof kFrac / sizeof kFrac[0]; fi++) {
		for (size_t ei = 0; ei < sizeof kEye / sizeof kEye[0]; ei++) {
			s_cam_x = kFrac[fi];
			s_cam_z = kFrac[(fi + 3) % (sizeof kFrac / sizeof kFrac[0])];
			s_cam_y = kEye[ei];

			s_hzn_n = 0;
			for (int bz = -3; bz <= 3; bz++) {
				for (int bx = -3; bx <= 3; bx++) {
					s_hzn_cols[s_hzn_n].cx = bx;
					s_hzn_cols[s_hzn_n].cz = bz;
					// 16..128 in chunk steps, the only values horizonBuild can produce.
					s_hzn_cols[s_hzn_n].top_y =
						(float)(CHUNK_DIM * (1 + ((bx * 7 + bz * 13 + 21) & 7)));
					s_hzn_n++;
				}
			}
			checkDegenerateDistances();

			for (int cz = -4; cz <= 4; cz++)
				for (int cx = -4; cx <= 4; cx++)
					for (int cy = 0; cy < 8; cy++)
						compareAt(cx, cy, cz);
		}
	}
}

// ── Scenario 2: exact ties, which is where a rounded angle and an exact ratio part ─────
//
// Three families, all built to land ON a boundary rather than near one:
//   * the blocker exactly collinear with the candidate (equal bearings, dbear == 0),
//   * the blocker's top at exactly the candidate's elevation (nelev == elev),
//   * the blocker exactly at the camera (ndist == 0) with its top exactly at eye level.

static void sweepTies(void)
{
	for (int axis = 0; axis < 4; axis++) {
		for (int step = 1; step <= 6; step++) {
			// Camera exactly at a column centre: every column centre on the axis is then
			// exactly collinear with it, so dbear is exactly 0 for the whole line.
			s_cam_x = 8.0f;
			s_cam_z = 8.0f;
			s_cam_y = (float)(16 * step);

			const int nx = (axis == 0) ? 1 : (axis == 1) ? -1 : 0;
			const int nz = (axis == 2) ? 1 : (axis == 3) ? -1 : 0;

			s_hzn_n = 0;
			for (int k = 1; k <= 6; k++) {
				s_hzn_cols[s_hzn_n].cx = nx * k;
				s_hzn_cols[s_hzn_n].cz = nz * k;
				s_hzn_cols[s_hzn_n].top_y = (float)(16 * step);   // exactly eye level
				s_hzn_n++;
			}
			// And one sitting exactly under the camera.
			s_hzn_cols[s_hzn_n].cx = 0;
			s_hzn_cols[s_hzn_n].cz = 0;
			s_hzn_cols[s_hzn_n].top_y = (float)(16 * step);
			s_hzn_n++;
			checkDegenerateDistances();

			for (int k = -8; k <= 8; k++)
				for (int cy = -2; cy <= 8; cy++)
					compareAt(nx * k, cy, nz * k);

			// Now retune the blockers so nelev == elev holds EXACTLY for a chosen
			// candidate: blk_h / ndist == cand_h / dist, with every quantity a small
			// exact float, so both sides are exact and the two implementations are being
			// asked the same >= on the same tie.
			for (int c = 0; c < s_hzn_n; c++) {
				const int kk = (nx ? s_hzn_cols[c].cx * nx : s_hzn_cols[c].cz * nz);
				if (kk <= 0) continue;
				// candidate at k = 8, top 16*(cy+1); blocker at k -> same ratio.
				s_hzn_cols[c].top_y = s_cam_y + (float)(16 * 4) * (float)kk / 8.0f;
			}
			for (int cy = -2; cy <= 8; cy++)
				compareAt(nx * 8, cy, nz * 8);
		}
	}
}

// ── Scenario 3: the +-pi wrap ──────────────────────────────────────────────────────────
//
// The reference wrapped its bearing difference with two while-loops, so the interesting
// candidates are the ones whose bearing sits within a hair of +-pi — where the loops fire and
// where a rounded atan2f can land on the wrong side of the branch cut. The cross-multiply has
// no cut at all, so this family exists to prove the two still agree across it.

static void sweepWrap(void)
{
	for (int t = 0; t < 400; t++) {
		s_cam_x = rndFloat(-2.0f, 2.0f);
		s_cam_z = 8.0f + rndFloat(-0.001f, 0.001f);   // hugs the z of the -x row's centres
		s_cam_y = rndFloat(-16.0f, 144.0f);

		s_hzn_n = 0;
		for (int k = -6; k <= 6; k++) {
			if (k == 0) continue;
			s_hzn_cols[s_hzn_n].cx = k;
			s_hzn_cols[s_hzn_n].cz = 0;
			s_hzn_cols[s_hzn_n].top_y = rndFloat(-32.0f, 160.0f);
			s_hzn_n++;
		}
		checkDegenerateDistances();

		for (int k = -7; k <= 7; k++)
			for (int cy = -2; cy <= 8; cy++)
				compareAt(k, cy, 0);
	}
}

// ── Scenario 4: fuzz ───────────────────────────────────────────────────────────────────
//
// Everything unconstrained: off-grid cameras above, below and inside the world, blocker tops
// that are not multiples of 16 (which horizonBuild cannot produce, but the predicate must not
// depend on that), negative coordinates on both axes, candidates below the eye, and blocker
// sets from 1 to 49 columns.

static void sweepFuzz(int rounds)
{
	for (int t = 0; t < rounds; t++) {
		s_cam_x = rndFloat(-96.0f, 96.0f);
		s_cam_z = rndFloat(-96.0f, 96.0f);
		s_cam_y = rndFloat(-64.0f, 192.0f);

		// Every fourth round snaps the camera onto exact grid values, which is where the
		// zero and equal-angle degeneracies live.
		if ((t & 3) == 0) {
			s_cam_x = (float)(16 * rndRange(-6, 6) + ((rnd() & 1) ? 8 : 0));
			s_cam_z = (float)(16 * rndRange(-6, 6) + ((rnd() & 1) ? 8 : 0));
			s_cam_y = (float)(16 * rndRange(-2, 9));
		}

		s_hzn_n = rndRange(1, 49);
		for (int c = 0; c < s_hzn_n; c++) {
			s_hzn_cols[c].cx = rndRange(-6, 6);
			s_hzn_cols[c].cz = rndRange(-6, 6);
			s_hzn_cols[c].top_y = ((rnd() & 1)
			                       ? (float)(16 * rndRange(-2, 9))
			                       : rndFloat(-64.0f, 192.0f));
		}
		checkDegenerateDistances();

		for (int r = 0; r < 6; r++)
			compareAt(rndRange(-7, 7), rndRange(-3, 9), rndRange(-7, 7));
	}
}

// ── The checks ─────────────────────────────────────────────────────────────────────────

int main(void)
{
	sweepRing();
	sweepTies();
	sweepWrap();
	sweepFuzz(100000);

	// A sweep that never culls anything would compare "false" against "false" forever and
	// could not tell a working rewrite from a `return false;`. These three are the control:
	// the reference must have said "hidden" a real number of times, and "not hidden" too.
	// They stay green under every sabotage of the rewrite, which is how a real red below is
	// told apart from a broken build.
	CHECK(s_cases > 500000);
	CHECK(s_true_ref > 1000);
	CHECK(s_true_ref < s_cases);

	// The degenerate branch must actually have been exercised, or the ndist == 0 arm of the
	// rewrite is untested and this file is quietly lying about its coverage.
	CHECK(s_zero_ndist > 0);

	// A NAKED PIN on the production constant, written as a bare number on purpose. Every other
	// claim in this file is a comparison between two implementations, and a comparison cannot
	// see a constant that both sides read — which is exactly how a 0.35f -> 0.05f edit to
	// chunk_render.c's HZN_HALF_W passed this suite 7/7 before today (see the note above
	// horizonHiddenExact). 2.8f is CHUNK_DIM/2 * 0.35 at CHUNK_DIM 16, and it is the value the
	// frozen reference and the frozen oracle are both written against, so if this goes red they
	// are stale too and the ref-vs-new counts below mean nothing until it is resolved.
	CHECK(HZN_HALF_W == 2.8f);

	// The redundancy measurements. These do NOT cover horizonHidden's hysteresis and
	// near-camera guards — nothing can, they cannot change an answer on a 16-block grid — they
	// assert the headroom that makes them unreachable, so the day that stops being true this
	// file says so instead of quietly staying green over two dead branches. Read the comment
	// above horizonHiddenExact before touching any of these three.
	CHECK(s_margin_slack < 1e299);      // it was measured at all, not left at its sentinel
	CHECK(s_margin_slack > 1.0);        // ...and no cone-passer came within the 1.0f hysteresis
	CHECK(s_dis_near == 0);             // the near bail never changed an answer

	// s_near_slack is REPORTED and not asserted, on purpose. The obvious assertion —
	// "nothing is ever culled closer than the bail radius CHUNK_DIM * 0.5" — turns out to be
	// scale-invariant and therefore could never fail: the nearest cull is one column away,
	// CHUNK_DIM, which is double the bail radius at every value of CHUNK_DIM. A check that
	// cannot go red proves nothing, so the near bail's coverage is s_dis_near above, which
	// IS live: widening the bail to CHUNK_DIM * 4.0f moved 26,663 cases.

	// THE CLAIM. Not "the rewrite matches the old float code" — it does not, in a handful of
	// cases out of hundreds of thousands, and the reason is in the report line below — but
	// "the rewrite computes the predicate the old float code was TRYING to compute", judged
	// by the double-precision oracle. Every ref-vs-new difference must therefore be a place
	// the old code itself was wrong.
	CHECK(s_dis_new_exact == 0);
	CHECK(s_dis_ref_new == s_dis_ref_exact);

	// And the direction of those few. horizonHidden's contract (see its comment in
	// chunk_render.c) is that it may keep a chunk it did not have to and must never drop one
	// that could be seen — so a place where the shipped float code culled and the exact
	// predicate does not is the shipped code over-culling, and the rewrite is strictly the
	// safer of the two there.
	CHECK(s_dis_ref_kept == s_dis_ref_new);

	// ── The check count, pinned as a bare number, and it must stay LAST in main() ────────
	//
	// No suite in this project asserted its own check count until 2026-08-25, and that absence
	// is what let a self-referential capacity check shrink a bound from 15 to 7, DELETE eight
	// assertions, and still print "0 failed" (326 checks became 318). Any sabotage that shortens
	// a loop bounded by a production constant removes checks instead of failing them, and the
	// only thing that notices is a pinned total. 12 counts this line itself, because CHECK
	// increments before it compares. If a new check is added, this number moves with it — that
	// is the point, not a nuisance.
	CHECK(s_checks == 12);

	printf("horizon equivalence: %ld cases | ref-vs-new %ld | new-vs-exact %ld | "
	       "ref-vs-exact %ld | of the ref-vs-new, %ld are the rewrite KEEPING a chunk the "
	       "float code culled | %ld culled by the reference | %ld zero-distance blockers | "
	       "half-width %.6g | hysteresis headroom %.6g blocks (guard bites at 1) | nearest cull "
	       "%.6g blocks (bail radius %.6g) | near-guard witness %ld\n",
	       s_cases, s_dis_ref_new, s_dis_new_exact, s_dis_ref_exact, s_dis_ref_kept,
	       s_true_ref, s_zero_ndist, (double)HZN_HALF_W, s_margin_slack, s_near_slack,
	       (double)((float)CHUNK_DIM * 0.5f), s_dis_near);
	if (s_dis_first[0])
		printf("horizon first ref-vs-new difference: %s\n", s_dis_first);

	if (s_fails == 0)
		printf("horizon self-test: PASS  %d checks\n", s_checks);
	else
		printf("horizon self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int horizon_test_host_only_t;

#endif   // !__3DS__
