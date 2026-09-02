// Host self-test for the NEON BIOME BORDERS debug overlay, source/debug/biomeborder.{h,c}.
//
// It links the REAL world/worldgen.c and asks the REAL worldgenBiomeAt() the same questions
// the module asks, independently, and compares. That is the whole design of it: a test that
// re-derives what it thinks the module should have produced from a copy of the module's own
// arithmetic proves only that two pieces of arithmetic agree, and this project has already
// shipped a compiling, test-passing diff that rendered garbage. So the sweep below is written
// as "here is every edge the generator actually has in this rectangle" and never as "here is
// what biomeBorderBuild would do".
//
// The pinned numbers (75 segments here, 157 and five buckets there) are MEASURED — they came
// out of scratchpad/neonborder_recon.c against input seed 1616 — not chosen to make anything
// pass. They exist so that a change which quietly stops emitting anything, or starts emitting
// everything, fails instead of passing an "every segment I emitted was valid" check that an
// empty list satisfies trivially.
#ifndef __3DS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/biomeborder.h"
#include "world/genversion.h"

static int g_checks;
static int g_fails;
static int g_first_line;

#define CHECK(cond)                                                            \
	do {                                                                       \
		g_checks++;                                                            \
		if (!(cond)) {                                                         \
			if (!g_fails) { g_first_line = __LINE__; }                         \
			g_fails++;                                                         \
			if (g_fails <= 8)                                                  \
				printf("  FAIL  L%d  %s\n", __LINE__, #cond);                  \
		}                                                                      \
	} while (0)

// ── The world under test ─────────────────────────────────────────────────────────────
//
// 1616 is the INPUT seed and it has to be. worldgenInit does NOT store what it is given — it
// stores rngMix(seed ^ 'BLKS'), so g.seed reads back as 675531292, and handing THAT number
// back to worldgenInit would mix an already-mixed seed and classify against a completely
// different noise field. Every number pinned below would then be wrong, and wrong SILENTLY:
// a valid-looking biome map in a world nobody asked for. Measured, not reasoned — the same
// trap scratchpad/biometint_render.c fell into once and documented.
#define TEST_SEED 1616u

// The chunk (-21,-40) seam: the ragged plains/desert staircase the biome-tint visual probe
// photographed in biometint_shot6_realfill_z.png. Block centre of that chunk.
#define SEAM_X (-328)
#define SEAM_Z (-632)

// A column measured to have FIVE different neon buckets in range, so the colour-filtered
// vertex path is exercised with more than one run. Every sample near the seam above has
// exactly one, which would leave the multi-run case uncovered.
#define BUSY_X (-1841)
#define BUSY_Z (-1735)

static BiomeBorderSeg g_segs[BB_MAX_SEGS];
static float          g_verts[BB_MAX_SEGS * BB_FLOATS_PER_SEG];

// Marks for the sweep: [z][x][axis] over the searched rectangle, sized for BB_MAX_RADIUS.
#define SW_DIM (BB_MAX_RADIUS * 2 + 1)
static uint8_t g_seen[SW_DIM][SW_DIM][2];

// ── The sweep ────────────────────────────────────────────────────────────────────────
//
// Independently walks the rectangle biomeborder.h's contract names, counts the edges the
// generator actually has, and then checks the emitted list against it three ways: every
// segment is a real edge, no edge is emitted twice, and no real edge is missing.
static void sweep(const WorldGen* g, int32_t px, int32_t pz, int radius, const char* label)
{
	printf("  sweep %s at (%d,%d) r=%d\n", label, px, pz, radius);

	const int n = biomeBorderBuild(g, px, pz, radius, g_segs, BB_MAX_SEGS);
	CHECK(!biomeBorderTruncated());   // BB_MAX_SEGS must be big enough for this fixture

	memset(g_seen, 0, sizeof g_seen);

	// What the generator actually has.
	int expect = 0;
	for (int32_t z = pz - radius; z <= pz + radius; z++)
		for (int32_t x = px - radius; x <= px + radius; x++) {
			const BiomeId me = worldgenBiomeAt(g, x, z);
			if (me != worldgenBiomeAt(g, x + 1, z)) expect++;
			if (me != worldgenBiomeAt(g, x, z + 1)) expect++;
		}

	CHECK(n == expect);
	printf("    emitted %d, generator has %d\n", n, expect);

	// Every emitted segment is a real edge, in range, with the right two biomes on it, and
	// emitted exactly once.
	for (int i = 0; i < n; i++) {
		const BiomeBorderSeg* s = &g_segs[i];
		const int ix = (int)(s->x - (px - radius));
		const int iz = (int)(s->z - (pz - radius));

		CHECK(ix >= 0 && ix < radius * 2 + 1);
		CHECK(iz >= 0 && iz < radius * 2 + 1);
		CHECK(s->axis == 0 || s->axis == 1);
		if (ix < 0 || ix >= SW_DIM || iz < 0 || iz >= SW_DIM || s->axis > 1) continue;

		CHECK(g_seen[iz][ix][s->axis] == 0);   // no edge emitted twice
		g_seen[iz][ix][s->axis] = 1;

		// The two biomes named on the segment are the two the generator has there — asked
		// again, here, rather than trusted from the struct.
		const BiomeId a = worldgenBiomeAt(g, s->x, s->z);
		const BiomeId b = s->axis == 0 ? worldgenBiomeAt(g, s->x + 1, s->z)
		                               : worldgenBiomeAt(g, s->x, s->z + 1);
		CHECK((BiomeId)s->lo == a);
		CHECK((BiomeId)s->hi == b);
		CHECK(s->lo != s->hi);                 // an edge with no change is not an edge
		CHECK(biomeBorderColourKey(s) == (s->lo < s->hi ? s->lo : s->hi));
	}

	// ...and no real edge is missing. This is the half that catches "emits nothing", which
	// every check above passes trivially.
	int missing = 0;
	for (int32_t z = pz - radius; z <= pz + radius; z++)
		for (int32_t x = px - radius; x <= px + radius; x++) {
			const int ix = (int)(x - (px - radius)), iz = (int)(z - (pz - radius));
			const BiomeId me = worldgenBiomeAt(g, x, z);
			if ((me != worldgenBiomeAt(g, x + 1, z)) != (g_seen[iz][ix][0] != 0)) missing++;
			if ((me != worldgenBiomeAt(g, x, z + 1)) != (g_seen[iz][ix][1] != 0)) missing++;
		}
	CHECK(missing == 0);
	printf("    edges present-but-unemitted or emitted-but-absent: %d\n", missing);
}

int main(void)
{
	// ── A. the toggle ────────────────────────────────────────────────────────────────
	//
	// Checked BEFORE anything else touches it, because "defaults OFF" is the whole of the
	// "this is a debug option, not a game feature" promise and it is a claim about .bss at
	// process start, not about a value someone assigned.
	CHECK(biomeBorderEnabled() == false);
	CHECK(biomeBorderWorldGen() == NULL);

	biomeBorderSetEnabled(true);
	CHECK(biomeBorderEnabled() == true);
	biomeBorderSetEnabled(false);
	CHECK(biomeBorderEnabled() == false);
	biomeBorderSetEnabled(true);
	biomeBorderReset();
	CHECK(biomeBorderEnabled() == false);   // reset puts a debugging session back to shipped
	CHECK(biomeBorderWorldGen() == NULL);

	// ── B. the neon ──────────────────────────────────────────────────────────────────
	//
	// Each colour pinned to its enumerator BY HAND. A shuffle of the table in biomeborder.c
	// is a wrong colour on every seam and not a compile error, so this is the only thing that
	// can catch it.
	// Tundra was (128, 255, 255) — a pale ice cyan — until the "every neon really is neon"
	// loop below rejected it: its darkest channel was 128, so it was a wash rather than an
	// emitted light, and against snow it would have been the one fence you could not see.
	// The colour was changed, not the check.
	CHECK(biomeBorderBiomeColour(BIOME_TUNDRA) == BB_RGBA(  0, 200, 255, 255));
	CHECK(biomeBorderBiomeColour(BIOME_TAIGA)  == BB_RGBA(  0, 255, 160, 255));
	CHECK(biomeBorderBiomeColour(BIOME_PLAINS) == BB_RGBA(200, 255,   0, 255));
	CHECK(biomeBorderBiomeColour(BIOME_FOREST) == BB_RGBA(  0, 255,  64, 255));
	CHECK(biomeBorderBiomeColour(BIOME_DESERT) == BB_RGBA(255, 160,   0, 255));
	CHECK(biomeBorderBiomeColour(BIOME_JUNGLE) == BB_RGBA(255,   0, 220, 255));
	CHECK(biomeBorderBiomeColour(BIOME_COUNT)  == BB_RGBA(255, 255, 255, 255));
	CHECK(biomeBorderBiomeColour((BiomeId)200) == BB_RGBA(255, 255, 255, 255));

	// BYTE ORDER, pulled apart rather than assumed. C3D_TexEnvColor wants R in the LOW byte;
	// getting this backwards would swap red and blue on every fence and still look like a
	// deliberate palette, which is precisely the class of mistake this hardware turns into
	// bad art instead of an error.
	{
		const uint32_t c = biomeBorderBiomeColour(BIOME_DESERT);
		CHECK(( c        & 0xFFu) == 255u);   // R
		CHECK(((c >>  8) & 0xFFu) == 160u);   // G
		CHECK(((c >> 16) & 0xFFu) ==   0u);   // B
		CHECK(((c >> 24) & 0xFFu) == 255u);   // A
	}

	// Every neon really is neon: one channel pinned at 255, one at or below 64.
	for (int b = 0; b < BIOME_COUNT; b++) {
		const uint32_t c = biomeBorderBiomeColour((BiomeId)b);
		const unsigned r = c & 0xFFu, gg = (c >> 8) & 0xFFu, bb = (c >> 16) & 0xFFu;
		CHECK(r == 255u || gg == 255u || bb == 255u);
		CHECK(r <= 64u  || gg <= 64u  || bb <= 64u);
		CHECK(((c >> 24) & 0xFFu) == 255u);
	}

	// The colour key is min(lo, hi) and is the same from either side of the same seam.
	{
		BiomeBorderSeg a = { 0, 0, 0, BIOME_DESERT, BIOME_PLAINS };
		BiomeBorderSeg b = { 0, 0, 0, BIOME_PLAINS, BIOME_DESERT };
		CHECK(biomeBorderColourKey(&a) == BIOME_PLAINS);
		CHECK(biomeBorderColourKey(&b) == BIOME_PLAINS);
		CHECK(biomeBorderColour(&a) == biomeBorderColour(&b));
		CHECK(biomeBorderColour(&a) == biomeBorderBiomeColour(BIOME_PLAINS));
		CHECK(biomeBorderColourKey(NULL) == -1);
		CHECK(biomeBorderColour(NULL) == BB_RGBA(255, 255, 255, 255));
	}

	// ── C. the world ─────────────────────────────────────────────────────────────────
	WorldGen g;
	if (!worldgenInit(&g, TEST_SEED, GEN_VERSION_BIOME)) {
		printf("biomeborder self-test: FAIL  worldgenInit(%u) refused\n", TEST_SEED);
		return 1;
	}
	// The mixing this test's whole fixture depends on, asserted rather than assumed. If
	// worldgenInit ever starts storing the seed it was handed, every pinned number below
	// becomes a number about a different world and this line is what says so.
	CHECK(g.seed != TEST_SEED);
	CHECK(g.seed == 675531292u);

	biomeBorderSetWorldGen(&g);
	CHECK(biomeBorderWorldGen() == &g);

	// Guards. Each must write nothing and return 0 rather than a fence in a guessed place.
	CHECK(biomeBorderBuild(NULL, 0, 0, 8, g_segs, BB_MAX_SEGS) == 0);
	CHECK(biomeBorderBuild(&g, 0, 0, 8, NULL, BB_MAX_SEGS) == 0);
	CHECK(biomeBorderBuild(&g, 0, 0, 8, g_segs, 0) == 0);

	// Radius clamps at both ends. A negative radius is one column, not a walk backwards; an
	// over-large one is BB_MAX_RADIUS, which is what keeps the row buffers in range.
	{
		const int a = biomeBorderBuild(&g, SEAM_X, SEAM_Z, -5, g_segs, BB_MAX_SEGS);
		const int b = biomeBorderBuild(&g, SEAM_X, SEAM_Z, 0, g_segs, BB_MAX_SEGS);
		CHECK(a == b);
		const int c = biomeBorderBuild(&g, SEAM_X, SEAM_Z, 100000, g_segs, BB_MAX_SEGS);
		const int d = biomeBorderBuild(&g, SEAM_X, SEAM_Z, BB_MAX_RADIUS, g_segs, BB_MAX_SEGS);
		CHECK(c == d);
	}

	// ── D. the sweep, against the real generator ─────────────────────────────────────
	sweep(&g, SEAM_X, SEAM_Z, 8, "the plains/desert seam");
	sweep(&g, SEAM_X, SEAM_Z, BB_RADIUS, "the plains/desert seam, full radius");
	sweep(&g, BUSY_X, BUSY_Z, BB_RADIUS, "the five-bucket column");
	sweep(&g, 0, 0, BB_RADIUS, "the origin");

	// ── E. the pinned counts ─────────────────────────────────────────────────────────
	//
	// Measured with scratchpad/neonborder_recon.c. These are what stop the sweep above from
	// being satisfied by a build that emits nothing: "every segment I emitted was a real
	// edge" is trivially true of an empty list, and "no edge is missing" is trivially true of
	// a list containing all of them, so one of these two has to be a number.
	{
		const int n = biomeBorderBuild(&g, SEAM_X, SEAM_Z, BB_RADIUS, g_segs, BB_MAX_SEGS);
		CHECK(n == 75);
		int per[BIOME_COUNT] = {0};
		for (int i = 0; i < n; i++) per[biomeBorderColourKey(&g_segs[i])]++;
		// The seam photographed in biometint_shot6 is plains against desert and nothing else.
		// mesher.h's MESH_TINT_ROW_FOR_BIOME is biome+1, so those are the tint rows 3 and 5
		// that picture shows — the two features agree about the same boundary.
		CHECK(per[BIOME_PLAINS] == 75);
		for (int b = 0; b < BIOME_COUNT; b++)
			if (b != BIOME_PLAINS) CHECK(per[b] == 0);
		CHECK(worldgenBiomeAt(&g, SEAM_X - 8, SEAM_Z) == BIOME_DESERT);
		CHECK(worldgenBiomeAt(&g, SEAM_X + 8, SEAM_Z) == BIOME_PLAINS);
	}
	{
		const int n = biomeBorderBuild(&g, BUSY_X, BUSY_Z, BB_RADIUS, g_segs, BB_MAX_SEGS);
		CHECK(n == 157);
		int per[BIOME_COUNT] = {0}, buckets = 0;
		for (int i = 0; i < n; i++) per[biomeBorderColourKey(&g_segs[i])]++;
		for (int b = 0; b < BIOME_COUNT; b++) if (per[b]) buckets++;
		CHECK(buckets == 5);
		CHECK(per[BIOME_TUNDRA] == 10);
		CHECK(per[BIOME_TAIGA]  == 47);
		CHECK(per[BIOME_PLAINS] == 47);
		CHECK(per[BIOME_FOREST] == 49);
		CHECK(per[BIOME_DESERT] ==  4);
		CHECK(per[BIOME_JUNGLE] ==  0);
	}

	// ── F. truncation ────────────────────────────────────────────────────────────────
	{
		const int n = biomeBorderBuild(&g, SEAM_X, SEAM_Z, BB_RADIUS, g_segs, 1);
		CHECK(n == 1);
		CHECK(biomeBorderTruncated() == true);
		// ...and the flag is per build, not sticky for the life of the process. A stale
		// "truncated" would report a fence as incomplete forever after one small call.
		const int m = biomeBorderBuild(&g, SEAM_X, SEAM_Z, BB_RADIUS, g_segs, BB_MAX_SEGS);
		CHECK(m == 75);
		CHECK(biomeBorderTruncated() == false);
	}

	// ── G. the vertices ──────────────────────────────────────────────────────────────
	{
		const int n = biomeBorderBuild(&g, SEAM_X, SEAM_Z, BB_RADIUS, g_segs, BB_MAX_SEGS);
		CHECK(n == 75);

		// Guards, and the all-or-nothing cap. A partial fence is a hole, and a hole reads as
		// "no seam here" — the one lie this feature must not tell.
		CHECK(biomeBorderVerts(NULL, g_segs, n, -1, g_verts, (int)(sizeof g_verts / sizeof g_verts[0])) == 0);
		CHECK(biomeBorderVerts(&g, NULL, n, -1, g_verts, (int)(sizeof g_verts / sizeof g_verts[0])) == 0);
		CHECK(biomeBorderVerts(&g, g_segs, 0, -1, g_verts, (int)(sizeof g_verts / sizeof g_verts[0])) == 0);
		CHECK(biomeBorderVerts(&g, g_segs, n, -1, g_verts, n * BB_FLOATS_PER_SEG - 1) == 0);

		const int wrote = biomeBorderVerts(&g, g_segs, n, -1, g_verts,
		                                   (int)(sizeof g_verts / sizeof g_verts[0]));
		CHECK(wrote == n * BB_FLOATS_PER_SEG);

		// The buckets partition the segments exactly: no segment in two runs, none in none.
		int sum = 0;
		for (int b = 0; b < BIOME_COUNT; b++) sum += biomeBorderCountFor(g_segs, n, b);
		CHECK(sum == n);
		CHECK(biomeBorderCountFor(g_segs, n, -1) == n);
		CHECK(biomeBorderCountFor(NULL, n, -1) == 0);

		// ...and the filtered vertex counts match the filtered segment counts.
		for (int b = 0; b < BIOME_COUNT; b++) {
			const int c = biomeBorderCountFor(g_segs, n, b);
			const int w = biomeBorderVerts(&g, g_segs, n, b, g_verts,
			                               (int)(sizeof g_verts / sizeof g_verts[0]));
			CHECK(w == c * BB_FLOATS_PER_SEG);
		}

		// The geometry itself, segment by segment: the wall stands in the plane BETWEEN the
		// two columns, spans exactly one column in the other axis, and is planted below the
		// shorter side and above the taller one.
		biomeBorderVerts(&g, g_segs, n, -1, g_verts, (int)(sizeof g_verts / sizeof g_verts[0]));
		for (int i = 0; i < n; i++) {
			const BiomeBorderSeg* s = &g_segs[i];
			const float* v = &g_verts[i * BB_FLOATS_PER_SEG];

			const int hl = worldgenHeight(&g, s->x, s->z);
			const int hr = s->axis == 0 ? worldgenHeight(&g, s->x + 1, s->z)
			                            : worldgenHeight(&g, s->x, s->z + 1);
			const float y0 = (float)((hl < hr ? hl : hr) - BB_WALL_DOWN);
			const float y1 = (float)((hl > hr ? hl : hr) + BB_WALL_UP);
			CHECK(y1 > y0);

			int at_y0 = 0, at_y1 = 0;
			for (int k = 0; k < BB_VERTS_PER_SEG; k++) {
				const float vx = v[k * 3 + 0], vy = v[k * 3 + 1], vz = v[k * 3 + 2];
				if (s->axis == 0) {
					// Plane at world x+1, one column wide in z.
					CHECK(vx == (float)(s->x + 1));
					CHECK(vz == (float)s->z || vz == (float)(s->z + 1));
				} else {
					CHECK(vz == (float)(s->z + 1));
					CHECK(vx == (float)s->x || vx == (float)(s->x + 1));
				}
				CHECK(vy == y0 || vy == y1);
				if (vy == y0) at_y0++; else at_y1++;
			}
			// Two triangles making one quad: three vertices on each rail, not four and two,
			// which is what a transposed corner would give.
			CHECK(at_y0 == 3);
			CHECK(at_y1 == 3);
		}
	}

	if (g_fails) {
		printf("biomeborder self-test: FAIL %d/%d  first: L%d\n",
		       g_fails, g_checks, g_first_line);
		return 1;
	}
	printf("biomeborder self-test: PASS  %d checks\n", g_checks);
	return 0;
}

#endif   // !__3DS__
