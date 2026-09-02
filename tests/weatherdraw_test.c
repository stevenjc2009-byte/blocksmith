// Host probe for v1.8.9's rain/snow renderer (source/gfx/weatherdraw.c) -- the plain-C half
// only: grid placement, UV assignment, and the scroll/drift accumulators. Nothing here can
// reach the GPU half (weatherDrawInit/Exit/Draw, guarded by __3DS__) -- that half is not
// linked into this binary at all, exactly as source/app/battery.c's own split means its
// PTMU polling is untested from the host. What IS checked here is the exact arithmetic that
// ships: this binary links source/gfx/weatherdraw.c directly, not a hand-written twin of it.
//
// Links source/gfx/weatherdraw.c ONLY (plus this file). weatherdraw.c's pure half includes
// world/weather.h for the WeatherKind enum alone -- a header-only need, since nothing here
// calls weatherAt() or any other weather.c function (see weatherdraw.h's own header on why
// that call is deliberately left to the integration lane) -- so no other source/world/*.c
// file needs to appear on this stanza's compile line for this binary to link.
//
// WHAT THIS DOES NOT CHECK, stated rather than left implied: nothing about whether the
// finished quads actually LOOK like falling rain or snow on a screen. That is a visual claim
// and this project's own standing rule is that a visual claim needs a look at the actual
// pixels, which nothing running on this host machine can give. What is checked instead is
// everything that can be checked without eyes: that the geometry this file builds covers the
// grid it claims to, that the UV bands stay inside the tiles they are supposed to select,
// that rain is faster than snow and only snow drifts, and that WEATHER_CLEAR really does mean
// "nothing to draw" everywhere that matters.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gfx/weatherdraw.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                          \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

#define CHECK_MSG(cond, ...) do {                                                 \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d: ", __LINE__);                                     \
			printf(__VA_ARGS__);                                                   \
			printf("\n");                                                          \
			if (!s_first[0]) {                                                     \
				char msg_[160];                                                    \
				snprintf(msg_, sizeof(msg_), __VA_ARGS__);                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, msg_); \
			}                                                                      \
		}                                                                          \
	} while (0)

#define WD_EPS 0.0015f

static bool nearf(float a, float b, float eps) { return fabsf(a - b) <= eps; }

// ── weatherDrawStateInit / SetState / ShouldDraw / FallSpeed / TileForKind / offsets ──────

static void testStateBasics(void)
{
	WeatherDrawState st;

	weatherDrawStateInit(&st);
	CHECK(st.kind == WEATHER_CLEAR);
	CHECK(st.scroll_v == 0.0f);
	CHECK(st.drift_phase == 0.0f);
	CHECK(st.drift_x == 0.0f);
	CHECK(st.grid_x == 0.0f && st.grid_y == 0.0f && st.grid_z == 0.0f);
	CHECK(!weatherDrawShouldDraw(&st));
	CHECK(!weatherDrawShouldDraw(NULL));   // must not crash, must not claim "yes" on a NULL state

	weatherDrawSetState(&st, WEATHER_RAIN);
	CHECK(st.kind == WEATHER_RAIN);
	CHECK(weatherDrawShouldDraw(&st));

	weatherDrawSetState(&st, WEATHER_SNOW);
	CHECK(st.kind == WEATHER_SNOW);
	CHECK(weatherDrawShouldDraw(&st));

	weatherDrawSetState(&st, WEATHER_CLEAR);
	CHECK(st.kind == WEATHER_CLEAR);
	CHECK(!weatherDrawShouldDraw(&st));

	// weatherDrawSetState(NULL, ...) must not crash.
	weatherDrawSetState(NULL, WEATHER_RAIN);

	// The task's own requirement, stated as an inequality rather than as two independent
	// positive-number checks: "rain falls fast... snow falls slower" means RAIN's speed must
	// be strictly greater than SNOW's, not merely that both are nonzero.
	float rain_speed = weatherDrawFallSpeed(WEATHER_RAIN);
	float snow_speed = weatherDrawFallSpeed(WEATHER_SNOW);
	CHECK(rain_speed > 0.0f);
	CHECK(snow_speed > 0.0f);
	CHECK_MSG(rain_speed > snow_speed,
	          "rain fall speed %.4f is not greater than snow's %.4f -- rain must fall "
	          "faster than snow",
	          rain_speed, snow_speed);
	CHECK(weatherDrawFallSpeed(WEATHER_CLEAR) == 0.0f);

	// Tile indices: fixed by the texture layout (tools/make_weathertex.py), not by
	// WeatherKind's own numbering -- see weatherdraw.h's own note on why this is not a cast.
	CHECK(weatherDrawTileForKind(WEATHER_RAIN) == WEATHERDRAW_TILE_RAIN);
	CHECK(weatherDrawTileForKind(WEATHER_SNOW) == WEATHERDRAW_TILE_SNOW);
	CHECK(weatherDrawTileForKind(WEATHER_CLEAR) == -1);
	CHECK(WEATHERDRAW_TILE_RAIN != WEATHERDRAW_TILE_SNOW);

	// First-vertex offsets: each kind's slice is WEATHERDRAW_VERTS_PER_KIND long and the two
	// slices must not overlap.
	CHECK(weatherDrawKindVertexOffset(WEATHER_RAIN) == 0);
	CHECK(weatherDrawKindVertexOffset(WEATHER_SNOW) == WEATHERDRAW_VERTS_PER_KIND);
	CHECK(weatherDrawKindVertexOffset(WEATHER_CLEAR) == -1);
	CHECK(WEATHERDRAW_VERTS_TOTAL == WEATHERDRAW_KINDS * WEATHERDRAW_VERTS_PER_KIND);
}

// ── weatherDrawBuildVertices: buffer-size contract ─────────────────────────────────────────

static void testBuildVerticesBufferContract(void)
{
	static WeatherVertex big[WEATHERDRAW_VERTS_TOTAL + 8];
	static WeatherVertex small[WEATHERDRAW_VERTS_TOTAL - 1];

	CHECK(weatherDrawBuildVertices(NULL, WEATHERDRAW_VERTS_TOTAL) == -1);
	CHECK(weatherDrawBuildVertices(small, WEATHERDRAW_VERTS_TOTAL - 1) == -1);
	CHECK(weatherDrawBuildVertices(big, WEATHERDRAW_VERTS_TOTAL) == WEATHERDRAW_VERTS_TOTAL);
	CHECK(weatherDrawBuildVertices(big, WEATHERDRAW_VERTS_TOTAL + 8) == WEATHERDRAW_VERTS_TOTAL);
}

// ── weatherDrawBuildVertices: a full grid sweep, both kinds ────────────────────────────────
//
// A single-vertex probe would not catch a bug that only shows up on, say, the grid's far
// corner or the second quad of a cross -- this project's own standing rule is that a
// single-coordinate probe is not enough for anything spatial. Every one of
// WEATHERDRAW_VERTS_TOTAL vertices is swept below, not a sample of them.

static void testBuildVerticesGridSweep(void)
{
	static WeatherVertex verts[WEATHERDRAW_VERTS_TOTAL];
	int n = weatherDrawBuildVertices(verts, WEATHERDRAW_VERTS_TOTAL);
	CHECK(n == WEATHERDRAW_VERTS_TOTAL);
	if (n != WEATHERDRAW_VERTS_TOTAL)
		return;   // the sweep below indexes verts[] by construction; do not run it on a short build

	const float half_n  = (float)(WEATHERDRAW_GRID_N - 1) * 0.5f;
	const float half_w  = WEATHERDRAW_COLUMN_WIDTH_BLOCKS * 0.5f;
	const float half_h  = WEATHERDRAW_COLUMN_HEIGHT_BLOCKS * 0.5f;
	const float inv_sqrt2 = 0.70710678f;
	const float v_top = WEATHERDRAW_COLUMN_HEIGHT_BLOCKS / WEATHERDRAW_TEX_WORLD_PER_TILE;

	// The furthest any vertex's local x or z can be from the grid centre: the outermost
	// column's own centre offset, plus the furthest a cross quad's corner can reach off that
	// centre (half_w along a 45-degree diagonal, so up to half_w * inv_sqrt2 on EACH axis).
	const float max_col_offset = half_n * WEATHERDRAW_CELL_BLOCKS;
	const float max_reach = max_col_offset + half_w * inv_sqrt2 + WD_EPS;

	float min_x = 1e9f, max_x = -1e9f, min_z = 1e9f, max_z = -1e9f;
	float min_y = 1e9f, max_y = -1e9f;

	bool saw_min_col_x = false, saw_max_col_x = false;
	bool saw_min_col_z = false, saw_max_col_z = false;

	for (int k = 0; k < WEATHERDRAW_KINDS; k++) {
		WeatherKind kind = (k == 0) ? WEATHER_RAIN : WEATHER_SNOW;
		int off = weatherDrawKindVertexOffset(kind);
		CHECK(off >= 0);

		float want_u0 = (kind == WEATHER_RAIN) ? 0.0f : 0.5f;
		float want_u1 = (kind == WEATHER_RAIN) ? 0.5f : 1.0f;

		for (int i = 0; i < WEATHERDRAW_VERTS_PER_KIND; i++) {
			const WeatherVertex* v = &verts[off + i];

			// Tile band: EVERY vertex of this kind's slice must sample only its own tile's
			// u range -- this is the check that would catch the tile-select axis bleeding
			// into the wrong art, which is exactly the failure mode
			// tools/make_weathertex.py's own header names ("a wrong texture coordinate
			// still renders a texture and never an error").
			CHECK_MSG(v->u >= want_u0 - WD_EPS && v->u <= want_u1 + WD_EPS,
			          "kind=%d vertex %d: u=%.4f is outside its own tile band [%.2f, %.2f]",
			          (int)kind, i, v->u, want_u0, want_u1);

			// Scroll axis: every vertex's BASE v (before any per-frame scroll offset, which
			// this function never applies -- that is weather.v.pica's job) must sit inside
			// [0, v_top]. Going outside that would mean the strip's own geometry already
			// samples past one full repeat before any animation is added.
			CHECK_MSG(v->v >= -WD_EPS && v->v <= v_top + WD_EPS,
			          "kind=%d vertex %d: v=%.4f is outside [0, %.4f]",
			          (int)kind, i, v->v, v_top);

			// Height: every vertex sits exactly at the strip's own top or bottom, never
			// between (these are flat vertical quads, not curved geometry).
			CHECK_MSG(nearf(v->y, -half_h, WD_EPS) || nearf(v->y, half_h, WD_EPS),
			          "kind=%d vertex %d: y=%.4f is neither -half_h (%.4f) nor +half_h (%.4f)",
			          (int)kind, i, v->y, -half_h, half_h);

			CHECK_MSG(fabsf(v->x) <= max_reach,
			          "kind=%d vertex %d: x=%.4f exceeds the grid's own reach (%.4f)",
			          (int)kind, i, v->x, max_reach);
			CHECK_MSG(fabsf(v->z) <= max_reach,
			          "kind=%d vertex %d: z=%.4f exceeds the grid's own reach (%.4f)",
			          (int)kind, i, v->z, max_reach);

			if (v->x < min_x) min_x = v->x;
			if (v->x > max_x) max_x = v->x;
			if (v->z < min_z) min_z = v->z;
			if (v->z > max_z) max_z = v->z;
			if (v->y < min_y) min_y = v->y;
			if (v->y > max_y) max_y = v->y;

			if (nearf(v->x, -max_col_offset - half_w * inv_sqrt2, 0.05f)) saw_min_col_x = true;
			if (nearf(v->x,  max_col_offset + half_w * inv_sqrt2, 0.05f)) saw_max_col_x = true;
			if (nearf(v->z, -max_col_offset - half_w * inv_sqrt2, 0.05f)) saw_min_col_z = true;
			if (nearf(v->z,  max_col_offset + half_w * inv_sqrt2, 0.05f)) saw_max_col_z = true;
		}
	}

	// Coverage: the grid must actually REACH both extremes on both axes and both the top and
	// bottom of a column -- a bug that shrank the grid to a single column, or that only ever
	// built the +X half of it, would still pass a "stays within bounds" check while failing
	// this one.
	CHECK(saw_min_col_x);
	CHECK(saw_max_col_x);
	CHECK(saw_min_col_z);
	CHECK(saw_max_col_z);
	CHECK_MSG(nearf(min_y, -half_h, WD_EPS), "min y across the whole grid is %.4f, want %.4f", min_y, -half_h);
	CHECK_MSG(nearf(max_y,  half_h, WD_EPS), "max y across the whole grid is %.4f, want %.4f", max_y, half_h);

	// Determinism: building the same grid a second time must reproduce the exact same bytes
	// -- weatherDrawInit() on the console calls this exactly once and never rebuilds, so a
	// caller-observable difference between two calls would mean the geometry is not actually
	// the deterministic function of the public constants this file's header claims it is.
	static WeatherVertex verts2[WEATHERDRAW_VERTS_TOTAL];
	int n2 = weatherDrawBuildVertices(verts2, WEATHERDRAW_VERTS_TOTAL);
	CHECK(n2 == n);
	CHECK(memcmp(verts, verts2, sizeof(verts)) == 0);
}

// ── weatherDrawUpdate: scroll / drift / grid-snap, swept over a range of inputs ────────────

static void testUpdateScrollAndDrift(void)
{
	// Rain must scroll faster than snow for the SAME dt, swept over several dt values rather
	// than one -- a single dt could accidentally land on a value where a bug's error term
	// happens to cancel out.
	//
	// Two things are checked per dt, not one, because a raw "rain.scroll_v > snow.scroll_v"
	// comparison is not actually sound on its own: both accumulators WRAP at 1.0 (see
	// weatherDrawUpdate's own header), so a large enough dt makes rain's own faster value
	// lap all the way back past zero while snow's has not, which would make the fast one
	// read as numerically SMALLER despite genuinely covering more distance. (This is not a
	// hypothetical -- the first version of this loop asserted the raw comparison
	// unconditionally and it failed for real at dt=0.5 and dt=1.0, both of which make
	// rain_speed*dt a multiple of 1.0: exactly the lap-around case. Left as two checks
	// instead of one, rather than just picking "safer" dt constants, so the reasoning
	// travels with the code and a future dt added to this list cannot reintroduce the same
	// false failure silently.)
	//
	//   1. The closed-form check, valid for EVERY dt including ones that wrap: after one
	//      Update from a freshly-init'd (scroll_v == 0) state, scroll_v must equal
	//      fmodf(speed * dt, 1.0) to within float noise -- the exact formula
	//      weatherDrawUpdate's own header states it uses. This is what actually pins the
	//      arithmetic down; the ordering check below is a bonus, not a substitute for it.
	//   2. The direct "rain is faster" ordering, gated on this dt being small enough that
	//      NEITHER side has wrapped yet (speed * dt < 1.0) -- the one regime where a raw
	//      greater-than comparison is actually a valid way to state "faster".
	static const float kDts[] = { 1.0f / 60.0f, 1.0f / 30.0f, 0.1f, 0.5f, 1.0f };
	for (size_t i = 0; i < sizeof(kDts) / sizeof(kDts[0]); i++) {
		WeatherDrawState rain, snow;
		weatherDrawStateInit(&rain);
		weatherDrawStateInit(&snow);
		weatherDrawSetState(&rain, WEATHER_RAIN);
		weatherDrawSetState(&snow, WEATHER_SNOW);

		float rain_speed = weatherDrawFallSpeed(WEATHER_RAIN);
		float snow_speed = weatherDrawFallSpeed(WEATHER_SNOW);

		weatherDrawUpdate(&rain, kDts[i], 0.0f, 0.0f, 0.0f);
		weatherDrawUpdate(&snow, kDts[i], 0.0f, 0.0f, 0.0f);

		float want_rain = fmodf(rain_speed * kDts[i], 1.0f);
		float want_snow = fmodf(snow_speed * kDts[i], 1.0f);
		CHECK_MSG(nearf(rain.scroll_v, want_rain, WD_EPS),
		          "dt=%.4f: rain scroll_v %.6f does not match fmodf(speed*dt,1.0)=%.6f",
		          kDts[i], rain.scroll_v, want_rain);
		CHECK_MSG(nearf(snow.scroll_v, want_snow, WD_EPS),
		          "dt=%.4f: snow scroll_v %.6f does not match fmodf(speed*dt,1.0)=%.6f",
		          kDts[i], snow.scroll_v, want_snow);

		if (rain_speed * kDts[i] < 1.0f && snow_speed * kDts[i] < 1.0f) {
			CHECK_MSG(rain.scroll_v > snow.scroll_v,
			          "dt=%.4f (pre-wrap): rain scroll_v %.6f is not greater than snow "
			          "scroll_v %.6f",
			          kDts[i], rain.scroll_v, snow.scroll_v);
		}

		// Rain never drifts; snow's drift must stay within its own stated amplitude.
		CHECK_MSG(rain.drift_x == 0.0f, "dt=%.4f: rain drift_x is %.6f, want exactly 0",
		          kDts[i], rain.drift_x);
	}

	// Sweep enough accumulated time to wrap scroll_v several times over, for both kinds, and
	// check EVERY step stays in range -- not just the final value, which could look fine
	// even if an intermediate step briefly went negative or unbounded.
	WeatherDrawState rain, snow;
	weatherDrawStateInit(&rain);
	weatherDrawStateInit(&snow);
	weatherDrawSetState(&rain, WEATHER_RAIN);
	weatherDrawSetState(&snow, WEATHER_SNOW);

	bool snow_drift_seen_nonzero = false;
	float snow_max_abs_drift = 0.0f;

	for (int step = 0; step < 2000; step++) {
		weatherDrawUpdate(&rain, 0.05f, 0.0f, 0.0f, 0.0f);
		weatherDrawUpdate(&snow, 0.05f, 0.0f, 0.0f, 0.0f);

		CHECK_MSG(rain.scroll_v >= -WD_EPS && rain.scroll_v < 1.0f + WD_EPS,
		          "step %d: rain.scroll_v=%.6f left [0,1)", step, rain.scroll_v);
		CHECK_MSG(snow.scroll_v >= -WD_EPS && snow.scroll_v < 1.0f + WD_EPS,
		          "step %d: snow.scroll_v=%.6f left [0,1)", step, snow.scroll_v);

		if (fabsf(snow.drift_x) > 1e-4f)
			snow_drift_seen_nonzero = true;
		if (fabsf(snow.drift_x) > snow_max_abs_drift)
			snow_max_abs_drift = fabsf(snow.drift_x);

		CHECK_MSG(rain.drift_x == 0.0f, "step %d: rain.drift_x is %.6f, want exactly 0",
		          step, rain.drift_x);
	}

	CHECK(snow_drift_seen_nonzero);
	CHECK_MSG(snow_max_abs_drift <= WEATHERDRAW_DRIFT_AMPLITUDE_BLOCKS + WD_EPS,
	          "snow drift reached %.6f, further than its own stated amplitude %.6f",
	          snow_max_abs_drift, WEATHERDRAW_DRIFT_AMPLITUDE_BLOCKS);

	// A negative dt (a caller's first frame, or a clock glitch) must be clamped to zero, not
	// trusted -- scroll_v and drift_phase must come back byte-identical to what they were.
	WeatherDrawState guard;
	weatherDrawStateInit(&guard);
	weatherDrawSetState(&guard, WEATHER_SNOW);
	weatherDrawUpdate(&guard, 0.3f, 0.0f, 0.0f, 0.0f);
	float scroll_before = guard.scroll_v;
	float phase_before = guard.drift_phase;
	weatherDrawUpdate(&guard, -5.0f, 0.0f, 0.0f, 0.0f);
	CHECK_MSG(guard.scroll_v == scroll_before,
	          "a negative dt changed scroll_v from %.6f to %.6f -- it must be clamped to zero",
	          scroll_before, guard.scroll_v);
	CHECK_MSG(guard.drift_phase == phase_before,
	          "a negative dt changed drift_phase from %.6f to %.6f -- it must be clamped to zero",
	          phase_before, guard.drift_phase);

	// weatherDrawUpdate(NULL, ...) must not crash.
	weatherDrawUpdate(NULL, 0.1f, 0.0f, 0.0f, 0.0f);
}

// ── weatherDrawUpdate: grid snapping, swept over a grid of camera positions ────────────────

static void testUpdateGridSnap(void)
{
	static const float kCoords[] = {
		-97.3f, -40.0f, -12.4f, -3.0f, -0.01f, 0.0f, 0.01f, 3.0f, 12.4f, 40.0f, 97.3f,
	};

	for (size_t xi = 0; xi < sizeof(kCoords) / sizeof(kCoords[0]); xi++) {
		for (size_t zi = 0; zi < sizeof(kCoords) / sizeof(kCoords[0]); zi++) {
			WeatherDrawState st;
			weatherDrawStateInit(&st);
			weatherDrawSetState(&st, WEATHER_RAIN);

			float cx = kCoords[xi];
			float cz = kCoords[zi];
			float cy = cx * 0.37f;   // an arbitrary, independent-looking height per sample

			weatherDrawUpdate(&st, 1.0f / 60.0f, cx, cy, cz);

			// grid_x/grid_z must be exact multiples of WEATHERDRAW_CELL_BLOCKS, and within
			// half a cell of the real camera position -- the "nearest multiple" contract a
			// snap function must hold, not merely "somewhere in the ballpark".
			float qx = st.grid_x / WEATHERDRAW_CELL_BLOCKS;
			float qz = st.grid_z / WEATHERDRAW_CELL_BLOCKS;
			CHECK_MSG(nearf(qx, roundf(qx), WD_EPS),
			          "cam=(%.2f,_,%.2f): grid_x=%.4f is not a multiple of CELL_BLOCKS",
			          cx, cz, st.grid_x);
			CHECK_MSG(nearf(qz, roundf(qz), WD_EPS),
			          "cam=(%.2f,_,%.2f): grid_z=%.4f is not a multiple of CELL_BLOCKS",
			          cx, cz, st.grid_z);
			CHECK_MSG(fabsf(st.grid_x - cx) <= WEATHERDRAW_CELL_BLOCKS * 0.5f + WD_EPS,
			          "cam=(%.2f,_,%.2f): grid_x=%.4f is more than half a cell from the camera",
			          cx, cz, st.grid_x);
			CHECK_MSG(fabsf(st.grid_z - cz) <= WEATHERDRAW_CELL_BLOCKS * 0.5f + WD_EPS,
			          "cam=(_,_,%.2f): grid_z=%.4f is more than half a cell from the camera",
			          cz, st.grid_z);

			// grid_y likewise snaps to WEATHERDRAW_GRID_Y_SNAP_BLOCKS.
			float qy = st.grid_y / WEATHERDRAW_GRID_Y_SNAP_BLOCKS;
			CHECK_MSG(nearf(qy, roundf(qy), WD_EPS),
			          "cam y=%.2f: grid_y=%.4f is not a multiple of the Y snap step",
			          cy, st.grid_y);
			CHECK_MSG(fabsf(st.grid_y - cy) <= WEATHERDRAW_GRID_Y_SNAP_BLOCKS * 0.5f + WD_EPS,
			          "cam y=%.2f: grid_y=%.4f is more than half a step from the camera",
			          cy, st.grid_y);
		}
	}
}

int main(void)
{
	testStateBasics();
	testBuildVerticesBufferContract();
	testBuildVerticesGridSweep();
	testUpdateScrollAndDrift();
	testUpdateGridSnap();

	printf("\n");
	if (s_fails) {
		printf("weatherdraw: FAILED - %d of %d checks (first: %s)\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("weatherdraw: PASS %d checks\n", s_checks);
	return 0;
}
