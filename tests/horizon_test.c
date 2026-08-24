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
#define HZN_PI_D  3.14159265358979323846   /* -std=c11 does not give us M_PI */

static bool horizonHiddenExact(int cx, int cy, int cz)
{
	const float col_x = (float)(cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float col_z = (float)(cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float dx     = col_x - s_cam_x, dz = col_z - s_cam_z;
	const float dist   = sqrtf(dx * dx + dz * dz);

	if (dist < (float)CHUNK_DIM * 0.5f) return false;

	const double bearing = atan2((double)dz, (double)dx);
	const float  top_y   = (float)((cy + 1) * CHUNK_DIM);
	const double elev    = atan2((double)top_y - (double)s_cam_y, (double)dist);

	for (int c = 0; c < s_hzn_n; c++) {
		if (s_hzn_cols[c].cx == cx && s_hzn_cols[c].cz == cz) continue;

		const float ncx = (float)(s_hzn_cols[c].cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ncz = (float)(s_hzn_cols[c].cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
		const float ndx = ncx - s_cam_x, ndz = ncz - s_cam_z;
		const float ndist = sqrtf(ndx * ndx + ndz * ndz);

		if (ndist >= dist - 1.0f) continue;

		double dbear = atan2((double)ndz, (double)ndx) - bearing;
		while (dbear >  HZN_PI_D) dbear -= 2.0 * HZN_PI_D;
		while (dbear < -HZN_PI_D) dbear += 2.0 * HZN_PI_D;

		const double half_w = atan2((double)HZN_HALF_W, (double)ndist);
		if (fabs(dbear) > half_w) continue;

		const double nelev = atan2((double)s_hzn_cols[c].top_y - (double)s_cam_y,
		                           (double)ndist);
		if (nelev >= elev) return true;
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

	printf("horizon equivalence: %ld cases | ref-vs-new %ld | new-vs-exact %ld | "
	       "ref-vs-exact %ld | of the ref-vs-new, %ld are the rewrite KEEPING a chunk the "
	       "float code culled | %ld culled by the reference | %ld zero-distance blockers\n",
	       s_cases, s_dis_ref_new, s_dis_new_exact, s_dis_ref_exact, s_dis_ref_kept,
	       s_true_ref, s_zero_ndist);
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
