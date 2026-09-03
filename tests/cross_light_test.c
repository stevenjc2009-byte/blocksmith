// tests/cross_light_test.c — smooth per-vertex light on BLOCK_SHAPE_CROSS geometry (v1.8.16).
//
// THE BUG THIS FILE WAS WRITTEN RED AGAINST
//
// Every cube face in the world has carried smooth per-vertex light since v1.5.0:
// world/mesher.c's cornerLight() averages the four cells touching a face corner from outside
// the face and packs the result into MeshVertex.pad (sky<<4 | block), exactly as cornerAO()
// gathers occlusion. Cross-shaped blocks — the torch, tall grass, its top half, the fern —
// were the one thing left out. emitCross() wrote
//
//     v->pad = lit ? s->light[si] : 0;
//
// one flat value for all sixteen vertices, taken from the plant's own cell. So a torch was the
// only object in the game still lit the blocky pre-v1.5.0 way, and the torch is precisely where
// it shows: it is a light source, so the gradient around it is the steepest gradient in the
// world, and its own quads flattened it out.
//
// WHAT THE FIX GATHERS, AND WHY THIS SUITE CAN TELL THE DIFFERENCE
//
// A cross quad is a diagonal plane through the middle of the cell and has no face normal, so
// cornerLight()'s "four cells outside the face" has nothing to be outside OF. What it does have
// is eight vertices sitting on the cell's eight real lattice corners — which are the four
// corners of FACE_BOTTOM and the four of FACE_TOP. The fix walks those two existing FacePlans
// and adds the cell's OWN light as a fifth, always-counted tap (see crossCornerLight in
// world/mesher.c for the full argument). That the own cell is always counted is not a detail:
// a torch stands on a floor, so all four of a bottom corner's outward taps are occluding stone,
// and a gather without the own-cell seed returns cnt = 0 and paints the bottom of every torch
// in the world pitch black. testBottomIsNotBlack below is the check that would catch that, and
// it is the check most likely to go red if this is ever "simplified".
//
// A fixture with uniform light around the plant would pass with the bug fully present and prove
// nothing, so every arm here builds a real GRADIENT out of a real light engine — no hand-written
// light bytes anywhere. The world is a sealed stone cave (sky = 0 under the roof), the sources
// are real BLOCK_TORCHes at luminance 14, the propagation is lightRelightColumn(), and the band
// the mesher reads is filled by the real scratchFillLight(). What is asserted is the shape of
// the gradient the mesher put in the vertices, not numbers copied out of a passing run.
//
//   testFlatWhenUnlit      THE CONTROL. lightEngineInit(false) — every Old 3DS build before
//                          v1.8.0 and every host suite that never turns the engine on — must
//                          still emit pad = 0 on all sixteen cross vertices, and the whole mesh
//                          must hash to what it hashed before this change. If this goes red the
//                          fix leaked into a build that has no light to smooth.
//   testTorchTopVsBottom   THE ASK. A torch on a stone floor in a dim cave: its top vertices and
//                          its bottom vertices must not carry the same light. With the bug all
//                          sixteen carry the cell's own byte and this is a tautology.
//   testBottomIsNotBlack   the failure mode the own-cell seed exists for, above.
//   testHorizontalGradient a tall grass with a torch three cells away along +X. The two ends of
//                          one diagonal are 1.4 blocks apart across the steepest part of that
//                          gradient and must not come out equal. This is the arm that proves the
//                          gather is not merely "own cell, plus something vertical".
//   testTwoHeightsDiffer   two torches in the SAME column at different heights, one standing on
//                          the floor and one floating. Their sixteen-byte vertex runs must not
//                          be identical — with the bug they are byte-for-byte the same, because
//                          both cells hold luminance 14 and nothing else is consulted.
//   testCrossQuadCount     crosses bypass the greedy merge entirely (world/mesher.c's meshPass
//                          returns before the six-face loop that calls mergeRun, so faceFlatKey
//                          is never asked about a cross). Four quads, sixteen vertices, per
//                          plant, before and after. If a future change routes crosses through
//                          the merge key this goes red rather than silently splitting runs.
//   testFacePathUnchanged  the regression guard on the hot path this touches: a lit world with
//                          NO cross blocks in it must hash to exactly what it hashed before the
//                          fix. cornerLight() and avgByCnt() are shared with every cube face in
//                          the game, so "the plants look better" is worth nothing if the walls
//                          moved.
//
// Own binary and own main(), and it lives under tests/ rather than source/world/: the console
// Makefile globs source/**/*.c, so a _test.c there needs a #ifndef __3DS__ wrapper to stay out
// of the CIA, and tests/ is exempt from that glob. Appended as a new stanza in
// tools/run_host_tests.sh, which is the one edit shape to that shared file that cannot drop
// another session's work.

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/light.h"
#include "world/mesher.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/world.h"

static int s_checks;
static int s_failed;

#define CHECK(cond, ...)                                 \
	do {                                                 \
		s_checks++;                                      \
		if (!(cond)) {                                   \
			s_failed++;                                  \
			printf("FAIL  L%d  ", __LINE__);             \
			printf(__VA_ARGS__);                         \
			printf("\n");                                \
		}                                                \
	} while (0)

// Too big for a comfortable stack, like every other mesher suite in this fleet does it.
static World       s_world;
static MeshScratch s_ms;
static MeshOut     s_out;
static MeshVertex  s_verts[MESH_MAX_VERTS];
static uint16_t    s_idx[MESH_MAX_INDICES];

// ── The cave ────────────────────────────────────────────────────────────────────────────
//
// One column, one chunk (0,0,0). Stone from y = 0 to FLOOR and a solid lid at ROOF, so every
// cell between them has sky light 0 and the only light in the fixture is the light this test
// put there. Without the lid the whole cavity reads sky 15 and every gradient below is drowned.
#define FLOOR  4
#define ROOF   13

static void buildCave(void)
{
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++) {
			for (int y = 0; y <= FLOOR; y++)
				worldSet(&s_world, x, y, z, BLOCK_STONE);
			worldSet(&s_world, x, ROOF, z, BLOCK_STONE);
		}
}

static void meshCave(void)
{
	scratchFill(&s_ms, &s_world, 0, 0, 0);
	scratchFillLight(&s_ms, &s_world, 0, 0, 0);

	s_out.verts     = s_verts;
	s_out.indices   = s_idx;
	s_out.vert_cap  = MESH_MAX_VERTS;
	s_out.index_cap = MESH_MAX_INDICES;
	meshChunk(&s_out, &s_ms);
}

// The same FNV-1a over counts, buckets, vertex bytes and indices that world/world_test.c and
// world/water_mesh_test.c pin their anchor worlds with. Deliberately the same function, so the
// suites cannot disagree about what "the same mesh" means.
static uint32_t meshHash(const MeshOut* out)
{
	uint32_t       h = 2166136261u;
	const uint8_t* b;
	size_t         n;

	#define HASH_BYTES(p, len) do {                 \
			b = (const uint8_t*)(p); n = (len);     \
			for (size_t i_ = 0; i_ < n; i_++) {     \
				h ^= b[i_]; h *= 16777619u;         \
			}                                       \
		} while (0)

	HASH_BYTES(&out->vert_count, sizeof out->vert_count);
	HASH_BYTES(&out->index_count, sizeof out->index_count);
	HASH_BYTES(&out->faces, sizeof out->faces);
	HASH_BYTES(&out->opaque_index_count, sizeof out->opaque_index_count);
	HASH_BYTES(&out->opaque_faces, sizeof out->opaque_faces);
	HASH_BYTES(out->face_start, sizeof out->face_start);
	HASH_BYTES(out->verts, sizeof(MeshVertex) * out->vert_count);
	HASH_BYTES(out->indices, sizeof(uint16_t) * out->index_count);
	#undef HASH_BYTES

	return h;
}

// ── Reading the emitted plant back out of the buffer ─────────────────────────────────────
//
// Read back, never predicted: a test that recomputed kCross's corner layout for itself would
// pass against a mesher that had the layout wrong in the same way.
//
// The transparent run's vertices start where its first index points. meshChunk emits the six
// opaque face buckets first and the deferred pass last, and vert_count only ever increases, so
// indices[opaque_index_count] is the first vertex no cube face wrote. Every cross block in this
// file's fixtures is the only deferred geometry in its world, so everything from there on is
// plant.

typedef struct {
	MeshVertex v[4 * 4];   // CROSS_QUADS(4) quads x 4 corners
	int        n;
} CrossVerts;

static void gatherCross(const MeshOut* out, int lx, int ly, int lz, CrossVerts* g)
{
	g->n = 0;
	if (out->opaque_index_count >= out->index_count) return;

	for (uint32_t i = out->indices[out->opaque_index_count]; i < out->vert_count; i++) {
		const MeshVertex* v = &out->verts[i];
		if (v->x < lx || v->x > lx + 1) continue;
		if (v->y < ly || v->y > ly + 1) continue;
		if (v->z < lz || v->z > lz + 1) continue;
		if (g->n < (int)(sizeof g->v / sizeof g->v[0])) g->v[g->n] = *v;
		g->n++;
	}
}

// The light byte a vertex sitting on lattice corner (lx+dx, ly+dy, lz+dz) came out with, or -1
// if the plant emitted no vertex there. Every one of the eight corners is written twice (once
// per winding), and the two must agree — a plant whose two windings disagreed would shimmer as
// the camera crossed the plane, so this returns -2 if they ever do.
static int cornerPad(const CrossVerts* g, int lx, int ly, int lz, int dx, int dy, int dz)
{
	int seen = -1;
	for (int i = 0; i < g->n && i < (int)(sizeof g->v / sizeof g->v[0]); i++) {
		const MeshVertex* v = &g->v[i];
		if (v->x != (int8_t)(lx + dx) || v->y != (int8_t)(ly + dy) || v->z != (int8_t)(lz + dz))
			continue;
		const int pad = (int)(uint8_t)v->pad;
		if (seen >= 0 && seen != pad) return -2;
		seen = pad;
	}
	return seen;
}

static uint8_t scratchLightAt(int lx, int ly, int lz)
{
	return s_ms.light[scratchIndex(lx + 1, ly + 1, lz + 1)];
}

// ── ARM 1: the control ───────────────────────────────────────────────────────────────────

static uint32_t s_unlit_hash;

static void testFlatWhenUnlit(void)
{
	printf("ARM 1 - control: the engine off must still write pad = 0 on every cross vertex\n");

	lightEngineInit(false);
	buildCave();
	worldSet(&s_world, 8, FLOOR + 1, 8, BLOCK_TORCH);
	meshCave();

	CrossVerts g;
	gatherCross(&s_out, 8, FLOOR + 1, 8, &g);
	CHECK(g.n == 16, "unlit torch emitted %d cross vertices, want 16", g.n);

	int nonzero = 0;
	for (int i = 0; i < g.n && i < 16; i++)
		if (g.v[i].pad != 0) nonzero++;
	CHECK(nonzero == 0, "%d of %d cross vertices carry a non-zero pad with the light engine "
	      "OFF; an unlit build must be byte-identical to pre-v1.5.0", nonzero, g.n);

	s_unlit_hash = meshHash(&s_out);
	printf("  unlit torch cave: verts=%u faces=%u hash=0x%08x\n",
	       s_out.vert_count, s_out.faces, s_unlit_hash);
	CHECK(s_unlit_hash == 0x99623c99u,
	      "unlit torch cave hashed 0x%08x, pinned 0x%08x - the engine-off mesh MOVED",
	      s_unlit_hash, 0x99623c99u);

	worldExit(&s_world);
}

// ── ARM 2: the ask ───────────────────────────────────────────────────────────────────────

#define TX  8
#define TY  (FLOOR + 1)
#define TZ  8

static void testTorchTopVsBottom(void)
{
	printf("ARM 2 - the ask: a torch's top and bottom must not be the same brightness\n");

	lightEngineInit(true);
	buildCave();
	worldSet(&s_world, TX, TY, TZ, BLOCK_TORCH);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight of the torch column refused");
	meshCave();

	const uint8_t own = scratchLightAt(TX, TY, TZ);
	CHECK((own & 15) == 14, "fixture: the torch cell's own block light is %u, want 14 - the "
	      "gradient this arm reads does not exist", own & 15);
	CHECK((own >> 4) == 0, "fixture: the torch cell has sky light %u, want 0 - the cave lid is "
	      "not sealing and every gradient below is drowned in daylight", own >> 4);

	CrossVerts g;
	gatherCross(&s_out, TX, TY, TZ, &g);
	CHECK(g.n == 16, "lit torch emitted %d cross vertices, want 16", g.n);

	const int bot = cornerPad(&g, TX, TY, TZ, 0, 0, 0);
	const int top = cornerPad(&g, TX, TY, TZ, 0, 1, 0);
	printf("  torch cell own=%u   bottom corner pad=%d   top corner pad=%d\n", own, bot, top);

	CHECK(bot >= 0, "no vertex at the torch's (0,0,0) lattice corner (got %d)", bot);
	CHECK(top >= 0, "no vertex at the torch's (0,1,0) lattice corner (got %d)", top);
	CHECK(bot != top,
	      "the torch's bottom and top corners both read %d - the cross is still taking one "
	      "flat value from its own cell, which is the blocky lighting this task removes", bot);

	// Not all sixteen the same, said directly: the assertion above names two corners, this one
	// is the whole plant and cannot be satisfied by a fix that only moved the top pair.
	int same = 1;
	for (int i = 1; i < g.n && i < 16; i++)
		if (g.v[i].pad != g.v[0].pad) { same = 0; break; }
	CHECK(!same, "all 16 cross vertices carry pad %u - flat", (uint8_t)g.v[0].pad);

	worldExit(&s_world);
}

static void testBottomIsNotBlack(void)
{
	printf("ARM 3 - a torch standing on stone must not have a black base\n");

	lightEngineInit(true);
	buildCave();
	worldSet(&s_world, TX, TY, TZ, BLOCK_TORCH);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight refused");
	meshCave();

	CrossVerts g;
	gatherCross(&s_out, TX, TY, TZ, &g);

	// All four bottom corners: every outward tap is the stone floor, which is occluding and is
	// skipped, so the only thing left is the cell's own light. A gather without the own-cell
	// seed divides by a count of zero taps and returns 0 here.
	static const int kBot[4][2] = { {0,0}, {1,0}, {0,1}, {1,1} };
	for (int i = 0; i < 4; i++) {
		const int pad = cornerPad(&g, TX, TY, TZ, kBot[i][0], 0, kBot[i][1]);
		printf("  bottom corner (%d,0,%d) pad=%d\n", kBot[i][0], kBot[i][1], pad);
		CHECK(pad > 0, "bottom corner (%d,0,%d) is pad %d: the base of every torch in the "
		      "world is BLACK, because all four outward taps are the floor and the cell's own "
		      "light was not counted", kBot[i][0], kBot[i][1], pad);
	}

	worldExit(&s_world);
}

// ── ARM 4: horizontal gradient ───────────────────────────────────────────────────────────

static void testHorizontalGradient(void)
{
	printf("ARM 4 - a plant across a sideways gradient must vary along its own diagonal\n");

	lightEngineInit(true);
	buildCave();
	worldSet(&s_world, TX,     TY, TZ, BLOCK_TALL_GRASS);
	worldSet(&s_world, TX - 3, TY, TZ, BLOCK_TORCH);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight refused");
	meshCave();

	// The fixture is only worth anything if the cells the gather taps really do differ. Read
	// them out of the band the mesher reads, not out of an expectation.
	const int near_l = scratchLightAt(TX - 1, TY + 1, TZ) & 15;
	const int far_l  = scratchLightAt(TX + 1, TY + 1, TZ) & 15;
	printf("  band above the plant: -x=%d  +x=%d\n", near_l, far_l);
	CHECK(near_l > far_l, "fixture: the band above the plant is %d on the torch side and %d on "
	      "the far side - there is no sideways gradient here to detect", near_l, far_l);

	CrossVerts g;
	gatherCross(&s_out, TX, TY, TZ, &g);
	CHECK(g.n == 16, "lit tall grass emitted %d cross vertices, want 16", g.n);

	// The two ends of plane A's TOP edge: (0,1,0) is the corner nearest the torch, (1,1,1) the
	// corner furthest from it. One quad, two corners, 1.4 blocks apart.
	const int nearp = cornerPad(&g, TX, TY, TZ, 0, 1, 0);
	const int farp  = cornerPad(&g, TX, TY, TZ, 1, 1, 1);
	printf("  plane A top edge: near corner pad=%d  far corner pad=%d\n", nearp, farp);
	CHECK(nearp > farp,
	      "the two ends of one cross quad's top edge read %d and %d; a quad spanning a "
	      "gradient this steep cannot be one flat value", nearp, farp);

	worldExit(&s_world);
}

// ── ARM 5: two heights in one column ─────────────────────────────────────────────────────

static void testTwoHeightsDiffer(void)
{
	printf("ARM 5 - two torches at different heights in one column must not be identical\n");

	lightEngineInit(true);
	buildCave();
	const int lowy  = FLOOR + 1;          // standing on the floor
	const int highy = FLOOR + 5;          // floating, open air below it
	worldSet(&s_world, TX, lowy,  TZ, BLOCK_TORCH);
	worldSet(&s_world, TX, highy, TZ, BLOCK_TORCH);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight refused");
	meshCave();

	CrossVerts lo, hi;
	gatherCross(&s_out, TX, lowy,  TZ, &lo);
	gatherCross(&s_out, TX, highy, TZ, &hi);
	CHECK(lo.n == 16, "low torch emitted %d cross vertices, want 16", lo.n);
	CHECK(hi.n == 16, "high torch emitted %d cross vertices, want 16", hi.n);

	const int lob = cornerPad(&lo, TX, lowy,  TZ, 0, 0, 0);
	const int hib = cornerPad(&hi, TX, highy, TZ, 0, 0, 0);
	printf("  low torch (on stone) bottom pad=%d   high torch (floating) bottom pad=%d\n",
	       lob, hib);

	// Both cells hold luminance 14, so the buggy mesher gave these two plants byte-identical
	// vertex runs. They stand in different places and must not.
	int identical = (lo.n == hi.n);
	for (int i = 0; i < lo.n && i < 16 && identical; i++)
		if (lo.v[i].pad != hi.v[i].pad) identical = 0;
	CHECK(!identical,
	      "the two torches' 16 light bytes are identical (all %u); a torch's light must depend "
	      "on where it is standing, not only on what it is", (uint8_t)lo.v[0].pad);
	CHECK(lob != hib, "both torches' bottom corners read %d", lob);

	worldExit(&s_world);
}

// ── ARM 6: crosses bypass the greedy merge ───────────────────────────────────────────────

static void testCrossQuadCount(void)
{
	printf("ARM 6 - crosses bypass the merge key: four quads each, always\n");

	// Four tall grass in a row along +X, all in identical light. If crosses went through
	// mergeRun/faceFlatKey their keys would match and the run would collapse; they do not,
	// so this is 4 plants x 4 quads x 4 vertices with no merging either way.
	lightEngineInit(true);
	buildCave();
	for (int i = 0; i < 4; i++)
		worldSet(&s_world, 4 + i, TY, TZ, BLOCK_TALL_GRASS);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight refused");
	meshCave();

	const uint32_t deferred_faces = s_out.faces - s_out.opaque_faces;
	const uint32_t deferred_verts = s_out.vert_count - s_out.indices[s_out.opaque_index_count];
	printf("  4 plants in a row: deferred faces=%u verts=%u\n", deferred_faces, deferred_verts);
	CHECK(deferred_faces == 16, "4 crosses emitted %u quads, want 16 (4 each) - a merge would "
	      "collapse this row, since four plants in identical light have identical keys",
	      deferred_faces);
	CHECK(deferred_verts == 64, "4 crosses emitted %u vertices, want 64", deferred_verts);

	// No per-plant gather here on purpose: ADJACENT crosses share lattice corners (the plant at
	// x=4 spans x 4..5 and its neighbour spans 5..6), so a position filter cannot tell one
	// plant's vertices from the next one's, and asking it to would only measure the filter. The
	// aggregate above is the whole claim anyway - 4 plants, 16 quads, nothing merged.

	worldExit(&s_world);
}

// ── ARM 7: the face path did not move ────────────────────────────────────────────────────

static void testFacePathUnchanged(void)
{
	printf("ARM 7 - regression guard: a LIT world with no crosses in it must not have moved\n");

	lightEngineInit(true);
	buildCave();
	// A shaft punched through the lid and a pocket dug into the floor: real cube faces across a
	// real SKY gradient, and no cross block anywhere in the world. cornerLight() and avgByCnt()
	// are shared with every cube face in the game, so this is the arm that says the fix stayed
	// inside emitCross.
	for (int z = 7; z <= 8; z++)
		for (int x = 7; x <= 8; x++)
			worldSet(&s_world, x, ROOF, z, BLOCK_AIR);
	for (int y = FLOOR - 1; y <= FLOOR; y++)
		for (int z = 6; z <= 9; z++)
			for (int x = 6; x <= 9; x++)
				worldSet(&s_world, x, y, z, BLOCK_AIR);
	CHECK(lightRelightColumn(&s_world, 0, 0), "relight refused");
	meshCave();

	// The gradient has to be real or the pin below is pinning a flat mesh. Two cells at the
	// same height, one under the shaft and one well away from it.
	const int under = scratchLightAt(7, FLOOR, 7) >> 4;
	const int away  = scratchLightAt(1, FLOOR + 1, 1) >> 4;
	printf("  sky under the shaft=%d  away from it=%d\n", under, away);
	CHECK(under > away, "fixture: sky light is %d under the shaft and %d away from it - there "
	      "is no gradient here for the face path to smooth", under, away);

	CHECK(s_out.opaque_faces == s_out.faces,
	      "this fixture is meant to hold no deferred geometry, but %u quads are deferred",
	      s_out.faces - s_out.opaque_faces);

	const uint32_t h = meshHash(&s_out);
	printf("  lit cube-only cave: verts=%u faces=%u hash=0x%08x\n",
	       s_out.vert_count, s_out.faces, h);
	CHECK(h == 0x806ce44cu,
	      "lit cube-only cave hashed 0x%08x, pinned 0x%08x - the SHARED face lighting path "
	      "moved, which is a worse regression than the bug this task fixes", h, 0x806ce44cu);

	worldExit(&s_world);
}

int main(void)
{
	printf("cross light self-test\n\n");

	testFlatWhenUnlit();
	testTorchTopVsBottom();
	testBottomIsNotBlack();
	testHorizontalGradient();
	testTwoHeightsDiffer();
	testCrossQuadCount();
	testFacePathUnchanged();

	// The check-count guard this fleet uses everywhere. Work out the DELTA from what you
	// changed and add it; do not paste whatever a failing run printed — that number is the
	// symptom of checks having gone missing, and pasting it throws away the evidence. The pin
	// is the count BEFORE this guard, so it is one less than the PASS line prints.
	const int ran = s_checks;
	if (ran != 32)
		printf("\nCHECK-COUNT GUARD: %d checks ran, %d expected.\n"
		       "  %s\n", ran, 32,
		       ran < 32 ? "Checks went MISSING." : "Extra checks appeared.");
	CHECK(ran == 32, "check-count guard: 32 checks before this line, %d ran", ran);

	printf("\ncross light self-test: %s %d checks, %d failed\n",
	       s_failed == 0 ? "PASS" : "FAILED", s_checks, s_failed);
	if (s_failed) printf("FAILED - %d of %d checks\n", s_failed, s_checks);
	return s_failed == 0 ? 0 : 1;
}
