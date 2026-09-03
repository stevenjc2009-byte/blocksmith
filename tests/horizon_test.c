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
//
// 2026-08-25, THE ONE HOLE THE AUDIT LEFT OPEN AND THIS SECOND PASS CLOSES. Everything above
// is about whether horizonHidden() COMPUTES the right answer. Nothing anywhere asserted that
// the renderer still ASKS it. The extraction pulls the function out by its signature and this
// file drives it directly, so deleting the renderer's one call — the chunk cull would simply
// stop rejecting hidden chunks, silently drawing every column behind a hill again — left this
// suite and tests/profile_reset_test.c both fully green: neither of them goes anywhere near
// the caller. testTheRendererStillCallsIt() below is the fix; see its own header for how it
// reads the caller and why it reads it the way it does.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/chunk.h"   // CHUNK_DIM — the real one, not a number retyped here

// v1.8.7. HznCol and hznDerive() — the real ones, for the same reason as CHUNK_DIM above and
// the same reason the function under test is extracted rather than copied. horizonHidden() now
// READS four per-blocker values that horizonBuild() derives, instead of recomputing them in its
// inner loop, so this file has to produce those values the way the renderer produces them. It
// does that by calling the renderer's own hznDerive() (see derivePool below), not by filling
// the fields itself: a hand-filled copy of the derivation would test nothing, which is the
// failure this whole file is built around. Break hznDerive() in the header and this binary
// goes red — demonstrated, not assumed.
#include "scene/horizon_derive.h"

static int  s_checks;
static int  s_fails;
static char s_first[200];

// Prints EVERY failure, not only the first. s_first still carries the first one for the
// summary line, but a red arm has to be readable case by case: a run that reports one line
// out of several cannot be used to tell a check that failed from a check that was quietly
// neutralised, and telling those two apart is the whole job here. Same reporting rule as
// world/water_alpha_test.c's CHECK.
#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d  %s\n", __LINE__, #cond);                           \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── The state horizonHidden() reads ────────────────────────────────────────────────────
//
// Same names as chunk_render.c's, because the extracted function refers to them by name.
// HZN_MAX_COLUMNS is larger here than the renderer's 65 only so the sweeps can push denser
// blocker sets through the loop; the function does not read the bound.
//
// HznCol itself is no longer declared here — it comes from scene/horizon_derive.h, shared with
// the renderer. It used to be a copy carrying the same three fields, which was safe only
// because the fields were pure inputs the test filled itself. v1.8.7 added four DERIVED fields,
// and a copied struct plus a copied derivation is exactly the "second copy that drifts" this
// file exists to prevent.

#define HZN_MAX_COLUMNS 128

static HznCol s_hzn_cols[HZN_MAX_COLUMNS];
static int    s_hzn_n;
static float  s_cam_x, s_cam_y, s_cam_z;

// v1.8.7. What horizonBuild() does to the pool after every write to cx/cz/top_y, and the only
// way this file is allowed to fill the derived fields.
//
// The sweeps below build blocker sets directly rather than through horizonBuild(), because the
// point is to enumerate configurations the renderer's pool would take a very long time to
// reach. That is fine for the inputs. It is NOT fine for the derived half: filling ndx/ndz/
// ndist/blk_h here by hand would hand horizonHidden() values computed by this file and then
// congratulate the renderer on agreeing with itself. So the renderer's own hznDerive() is
// called instead, over the whole pool, immediately before anything reads it — the same
// function the shipped horizonBuild() calls, out of the same header, with no copy in between.
//
// Whole-pool and re-run per candidate rather than incremental: correctness here, not speed, and
// one call site that cannot be forgotten beats four fill sites that each have to remember.
//
// v1.8.8: it also SORTS the pool by ndist ascending, and that is not cosmetic. horizonHidden()
// breaks out of its blocker loop at the first column that is not strictly nearer than the
// candidate, which is only sound on a sorted table. The shipped horizonBuild() insertion-sorts
// after its build loop; this file fills the table by hand, so it has to sort it too, or the
// extracted function is being handed an input the renderer never produces. It is not a
// theoretical hazard: measured on the harness that justified the break, an unsorted table gives
// 3316 wrong answers out of 6880 candidates. Same insertion sort, same comparison, deliberately
// — two ways to order the same table would be two things to keep in step. This is the one call
// site that cannot be forgotten, which is exactly why the sort belongs in it.
static void derivePool(void)
{
	for (int c = 0; c < s_hzn_n; c++) hznDerive(&s_hzn_cols[c], s_cam_x, s_cam_y, s_cam_z);

	for (int i = 1; i < s_hzn_n; i++) {
		const HznCol key = s_hzn_cols[i];
		int j = i - 1;
		while (j >= 0 && s_hzn_cols[j].ndist > key.ndist) {
			s_hzn_cols[j + 1] = s_hzn_cols[j];
			j--;
		}
		s_hzn_cols[j + 1] = key;
	}
}

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
	// v1.8.7: reads the derived fields rather than recomputing them, because the branch this
	// guards is now taken on s_hzn_cols[c].ndist and not on a distance this file worked out.
	// Checking the one the renderer will actually branch on is the point.
	derivePool();

	for (int c = 0; c < s_hzn_n; c++) {
		const float ndx = s_hzn_cols[c].ndx, ndz = s_hzn_cols[c].ndz;
		if (s_hzn_cols[c].ndist == 0.0f) {
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
	// v1.8.7. The pool's derived half, produced by the renderer's hznDerive() — see derivePool.
	// horizonHidden() reads it; horizonHiddenRef() and horizonHiddenExact() still compute their
	// own from cx/cz/top_y, which is what makes this a comparison and not a tautology: the two
	// oracles never see these fields, so a wrong derivation shows up as a disagreement.
	derivePool();

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

// ── Is the renderer still asking? ───────────────────────────────────────────────────────
//
// Everything above this line drives the extracted function directly, so all of it stays green
// if scene/chunk_render.c's cull loop stops consulting the function at all. That is not a
// hypothetical: the loop is four lines inside an #if, and deleting them costs nothing at build
// time — the extraction anchors on the DEFINITION, so the function still compiles, still gets
// tested here, and still has every one of its answers agree with the oracle. It just would not
// be asked any of the questions. Chunks behind a hill would go back to being drawn, at the cost
// this cull was written to avoid, and the two suites that mention the horizon (this one and
// tests/profile_reset_test.c) would both report PASS.
//
// So the caller is asserted over its SOURCE TEXT. chunk_render.c includes <3ds.h> and
// <citro3d.h> and cannot be compiled on the host, which is the same wall the extraction exists
// to get around, and there is no other way to observe the call. The idiom is the one
// world/water_alpha_test.c and world/atlas_uv_shader_test.c already use for the .pica files and
// for the renderer's blend state: whitespace-squashed lines, needles that name a WHOLE
// statement including its operands, an ordering assertion between the anchors, and a parse that
// cannot fail quietly — a file that will not open, or a needle that finds nothing, is a loud
// failure and never a skip. Anyone changing this should read c9df134 first.
//
// TWO THINGS A NAIVE strstr-OVER-SOURCE CHECK GETS WRONG, both of which have already cost this
// project a green run today, and both of which are handled in loadSource() below:
//
//   PROSE.  strstr cannot tell code from a comment. A red arm elsewhere today went green for a
//           reason nobody planned — the sabotaging agent's own comment restated the expression
//           the needle was hunting for, so deleting the code left the needle satisfied by the
//           note about deleting the code. loadSource() therefore CUTS // comments off every
//           line before anything is searched, quote-aware so a literal is not mistaken for one.
//           chunk_render.c contains no /* */ at all (measured: zero occurrences on 2026-08-25),
//           so that is the whole of the comment syntax the file actually uses. Belt and braces,
//           every needle below is also asserted to match EXACTLY ONCE, so a second copy of the
//           text anywhere — prose or code — is itself a failure.
//
//   WRAPS.  a needle can only describe one physical line, so a call reformatted across two
//           lines is half-unchecked by construction and the unchecked half fails silently. That
//           is precisely how the renderer's blending call hid a GPU_ONE, GPU_ZERO for a
//           release. loadSource() joins a statement whose parentheses are still open onto the
//           following lines, so a needle naming a whole call keeps working whatever column the
//           arguments end up in.
//
// It also drops blank and comment-only lines from the array while keeping each entry's real
// physical line number for reporting, so "the next line" means the next line of CODE. The two
// lines after the call are checked that way: the call is only load-bearing if the branch it
// guards both counts the rejection and skips the chunk.
//
// The verbatim needles live here, in a file nothing in the tree greps or parses. Every comment
// about them describes the line instead of restating it, for the PROSE reason above: a note in
// the searched file that spells the needle out is indistinguishable from the code.

#define RENDERER_PATH     "source/scene/chunk_render.c"

#define HZN_DEF_NEEDLE    "static bool horizonHidden(int cx, int cy, int cz)"
#define CULLFRAME_NEEDLE  "static void cullFrame(const C3D_Mtx* view)"
#define DRAW_NEEDLE       "void chunkRenderDraw(const C3D_Mtx* view)"
#define HZN_CALL_NEEDLE   "if (horizonHidden(s->cx, s->cy, s->cz)) {"
#define HZN_COUNT_NEEDLE  "s_horizon_culled++;"
#define HZN_SKIP_NEEDLE   "continue;"

#define SRC_MAX_ENTRIES 6000
#define SRC_MAX_ENTRY   1024
#define SRC_MAX_JOIN    8      // physical lines one open-parens statement may swallow

static char s_src[SRC_MAX_ENTRIES][SRC_MAX_ENTRY];
static int  s_src_at[SRC_MAX_ENTRIES];   // 1-based physical line each entry starts on
static int  s_src_n;

// Truncates `s` at an unquoted //. Character and string literals are tracked so that a path or
// a URL inside quotes survives; chunk_render.c has one such line (an include with a trailing
// note) and it must keep its code half.
static void stripComment(char* s)
{
	bool in_str = false, in_chr = false;
	for (char* p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') in_str = false;
			continue;
		}
		if (in_chr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '\'') in_chr = false;
			continue;
		}
		if (*p == '"') { in_str = true; continue; }
		if (*p == '\'') { in_chr = true; continue; }
		if (*p == '/' && p[1] == '/') { *p = '\0'; return; }
	}
}

// Every run of whitespace becomes one space and the ends are trimmed, so a needle is never
// checking the file's indentation or its column alignment. Same helper, same reason, as
// world/water_alpha_test.c's.
static void squash(const char* in, char* out, size_t cap)
{
	size_t o = 0;
	bool   sp = false;
	for (const char* p = in; *p && o + 1 < cap; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { sp = (o > 0); continue; }
		if (sp) { out[o++] = ' '; sp = false; }
		if (o + 1 < cap) out[o++] = *p;
	}
	out[o] = '\0';
}

// Running parenthesis balance of `s` starting from `depth`, ignoring anything inside a literal.
static int parenDepth(const char* s, int depth)
{
	bool in_str = false, in_chr = false;
	for (const char* p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') in_str = false;
			continue;
		}
		if (in_chr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '\'') in_chr = false;
			continue;
		}
		if (*p == '"') { in_str = true; continue; }
		if (*p == '\'') { in_chr = true; continue; }
		if (*p == '(') depth++;
		else if (*p == ')' && depth > 0) depth--;
	}
	return depth;
}

// Returns the number of code entries read, or -1 if the file could not be opened. -1 is turned
// into a failure by the caller; it is never read as "no lines, therefore nothing to complain
// about". Preprocessor lines never join: an #if whose condition uses a macro call would
// otherwise drag the code after it into one entry.
static int loadSource(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	s_src_n = 0;
	char raw[4096], one[SRC_MAX_ENTRY];
	int  phys = 0, joined = 0;
	bool joining = false;

	while (s_src_n < SRC_MAX_ENTRIES && fgets(raw, sizeof raw, f)) {
		phys++;
		stripComment(raw);
		squash(raw, one, sizeof one);
		if (one[0] == '\0') continue;

		if (joining && joined < SRC_MAX_JOIN) {
			// Appended by hand rather than with snprintf: the entry is a fixed buffer and
			// truncating it silently is the one outcome this file must not have, so the copy
			// stops at the wall and the entry simply ends there.
			char*  dst  = s_src[s_src_n - 1];
			size_t used = strlen(dst);
			if (used + 1 < SRC_MAX_ENTRY) dst[used++] = ' ';
			for (size_t k = 0; one[k] && used + 1 < SRC_MAX_ENTRY; k++) dst[used++] = one[k];
			dst[used] = '\0';
			joined++;
		} else {
			s_src_at[s_src_n] = phys;
			snprintf(s_src[s_src_n], SRC_MAX_ENTRY, "%s", one);
			s_src_n++;
			joined = 0;
		}

		joining = (s_src[s_src_n - 1][0] != '#') &&
		          parenDepth(s_src[s_src_n - 1], 0) > 0 && joined < SRC_MAX_JOIN;
	}

	fclose(f);
	return s_src_n;
}

// How many code entries contain `needle`. `at` gets the physical line the first one starts on,
// `idx` its index in the array, both left at -1 when there is no match.
static int countCode(const char* needle, int* at, int* idx)
{
	int n = 0;
	if (at)  *at  = -1;
	if (idx) *idx = -1;
	for (int i = 0; i < s_src_n; i++) {
		if (!strstr(s_src[i], needle)) continue;
		if (n == 0) {
			if (at)  *at  = s_src_at[i];
			if (idx) *idx = i;
		}
		n++;
	}
	return n;
}

// Does the code entry `idx` lines after the match contain `needle`? Blank and comment-only
// lines are already gone, so this is "the next statement", not "the next physical line".
static bool codeHas(int idx, const char* needle)
{
	if (idx < 0 || idx >= s_src_n) return false;
	return strstr(s_src[idx], needle) != NULL;
}

static void testTheRendererStillCallsIt(void)
{
	const int n = loadSource(RENDERER_PATH);

	int def_at = -1, cf_at = -1, draw_at = -1, call_at = -1;
	int call_idx = -1;

	const int defs  = countCode(HZN_DEF_NEEDLE,   &def_at,  NULL);
	const int cfs   = countCode(CULLFRAME_NEEDLE, &cf_at,   NULL);
	const int draws = countCode(DRAW_NEEDLE,      &draw_at, NULL);
	const int calls = countCode(HZN_CALL_NEEDLE,  &call_at, &call_idx);

	// Deliberately no early return anywhere in here. If the file cannot be opened, or the
	// needles find nothing, every check below must still RUN and fail — a bail-out would delete
	// checks instead, which is the exact failure the count pin at the end of main() exists to
	// catch, and it would be this file causing it.
	CHECK(n > 0);

	// The three anchors. They are the control for this section: none of them has anything to do
	// with whether the call is present, so they stay green while the call check goes red, and a
	// run where they went red too is a broken parse rather than a missing call.
	CHECK(defs == 1);
	CHECK(cfs == 1);
	CHECK(draws == 1);

	// THE CLAIM. The cull loop still asks the predicate about the slot it is looking at, with
	// the slot's own three coordinates. Matching exactly once matters as much as matching: a
	// second copy of the text, in code or in a comment, means the needle is no longer pointing
	// at one known statement.
	CHECK(calls == 1);

	// ...and it is a real call, in the right place. After the definition, so it resolves to the
	// function this file has been testing; after cullFrame's own signature and before the next
	// function's, so it is inside the per-frame cull loop and not in some unused helper.
	CHECK(def_at > 0 && call_at > def_at);
	CHECK(cf_at > 0 && draw_at > 0 && call_at > cf_at && call_at < draw_at);

	// ...and its result is acted on. A call whose branch does nothing is a call that culls
	// nothing: the next statement must count the rejection, and the one after must skip the
	// chunk. Without the skip the counter still climbs and chunkRenderHorizonCulled() still
	// reports work being done, while every chunk it claims to have rejected is drawn anyway.
	CHECK(codeHas(call_idx + 1, HZN_COUNT_NEEDLE));
	CHECK(codeHas(call_idx + 2, HZN_SKIP_NEEDLE));

	printf("horizon call site: %s | %d code lines | definition L%d (x%d) | cullFrame L%d (x%d) "
	       "| call L%d (x%d) | next function L%d (x%d)\n",
	       RENDERER_PATH, n, def_at, defs, cf_at, cfs, call_at, calls, draw_at, draws);
}

// ── The checks ─────────────────────────────────────────────────────────────────────────

int main(void)
{
	// First, because a renderer that no longer asks makes every number below decoration.
	testTheRendererStillCallsIt();

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
	// only thing that notices is a pinned total. 21 counts this line itself, because CHECK
	// increments before it compares. If a new check is added, this number moves with it — that
	// is the point, not a nuisance.
	//
	// 12 -> 21 on 2026-08-25: the nine checks in testTheRendererStillCallsIt(), the only part
	// of this file that reads the CALLER rather than the predicate. Recomputed as 12 + 9, not
	// pasted from what a run printed. Those nine deliberately have no early return, so a
	// chunk_render.c that will not open fails nine checks instead of skipping them.
	CHECK(s_checks == 21);

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
