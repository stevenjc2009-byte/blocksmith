// Host tests for BIOME TINT (v1.8.8) — the packing, the merge key, the shaders, the palette.
//
// Biome identity is carried by COLOUR here rather than by a block id per biome: there is one
// grass block and one tall-grass strand in world/registry.c, and the biome multiplies what they
// draw. See world/mesher.h for the encoding and for why it costs no vertex bytes.
//
// ── THE ONE THAT MATTERS: THE SMEAR ──────────────────────────────────────────
//
// world/mesher.c is GREEDY along one axis and has been since v1.6.0 task 11 — mergeRun() joins
// coplanar faces of the same block id into a single quad up to ATLAS_MAX_MERGE_BLOCKS (15)
// cells wide, and it joins them iff faceFlatKey() gives them the same signature. Until this
// task that key was ambient occlusion and light and nothing else. mesher.h's header comment
// still said the mesher was "deliberately *not* greedy", three minor versions after it became
// greedy, which is the sentence that makes this bug easy to ship.
//
// A biome tint is per COLUMN. Two grass tops either side of a biome border are the same block
// id, the same shape, and — on the flat open ground biome borders mostly sit on — the same AO
// and the same light. So without the tint in that key they hash identically, merge into one
// quad, and the quad takes the four vertices of the FIRST cell of the run. Every cell behind it
// is then drawn in the first cell's biome colour, and the border does not fade, it JUMPS: up to
// fifteen blocks from where the biome actually changes, in whichever direction the run happened
// to be walked.
//
// NOTHING ELSE IN THIS TREE CATCHES THAT. The face count is unchanged, the coverage is perfect,
// the AO is right, the light is right, every pinned mesh hash still matches. The geometry is
// correct and only the colour is wrong — which is the exact class of defect this project has
// been bitten by repeatedly, and the reason "it compiled and the tests passed" has never been
// evidence here.
//
// testBiomeBorderIsExact() below walks every emitted quad, works out which COLUMNS it covers
// from its own vertices, and asserts the tint baked into those vertices is the tint every one
// of those columns is entitled to. It is run twice, once with the border along x and once
// along z, because which axis a face merges along comes out of a face table and not out of
// anything this file should be asserting — one orientation alone would be a check that passes
// because nothing merged rather than because nothing smeared. testMergingActuallyHappens()
// guards that directly: it fails if the fixture stops producing multi-block quads at all.
//
// ── WHAT ELSE IS HERE, AND WHAT EACH WOULD CATCH ─────────────────────────────
//
//   THE PACKING    meshAoPack / meshAoValue / meshAoTint round-trip over every one of the 32
//                  legal (ao, tint) pairs, and MESH_TINT_NONE packs to the bare AO byte. The
//                  failure this catches is the silent one: the ao byte is READ UNMASKED by
//                  both vertex shaders, which multiply it by 1/3, so a tint index leaking into
//                  the low two bits is not ignored — it reads as an occlusion of up to 31 and
//                  blows the face to flat white.
//
//   THE ZERO PATH  a scratch nobody filled a tint band into must emit exactly the vertices this
//                  mesher emitted before v1.8.8. That is what keeps the four pinned hashes in
//                  world/world_test.c and the two in world/water_mesh_test.c from moving, and
//                  it is asserted here rather than assumed from them being green.
//
//   THE FACES      the grass block's TOP takes the biome colour and its SIDES do not.
//                  BTEX_GRASS_SIDE is one tile carrying a grass crust over dirt and this
//                  hardware has no fragment shader to mask it with, so tinting that face tints
//                  the dirt half too. The half-grass/half-dirt tile is to be kept exactly as it
//                  is, so this asserts it is left alone — a regression here is somebody
//                  "finishing" the feature and browning the dirt.
//
//   THE DIRT       (v1.8.17) BLOCK_DIRT takes the column's biome colour on ALL SIX faces —
//                  unlike grass it has no shared half-tile to protect, BTEX_DIRT is the same
//                  texture on every face (world/registry.c), so there is nothing for a partial
//                  restriction to guard. This was the one gap left in "per-biome wood, leaves,
//                  grass, flowers, dirt" after v1.8.8; blockFaceTintable excluded BLOCK_DIRT
//                  entirely until this task.
//
//   THE STRANDS    a tall-grass or fern cross takes the tint of the column it stands in, on all
//                  four of its quads, so a strand is the colour of the grass under it from
//                  every angle. That is the half of the ask that is most visible in motion.
//
//   THE SHADERS    BOTH source/shaders/world.v.pica and world_dynamic.v.pica must declare the
//                  palette, split the ao byte before anything reads it, feed the ao curve from
//                  the SPLIT value rather than the raw attribute, and multiply the tint into
//                  outclr.xyz. world_dynamic is the program actually bound on both console
//                  models since the v1.8.0 shader split and world.v.pica is compiled and never
//                  bound, so an edit to only one of them is invisible either on the hardware or
//                  to the next person reading the file. Same drift world/atlas_uv_shader_test.c
//                  guards uvScale against and world/water_alpha_test.c guards the alpha against.
//
//   THE PALETTE    scene/chunk_render.c must fill the bank from meshTintRow() and not from a
//                  copy of the table, must upload MESH_TINT_ROWS of it, and must leave .w at
//                  1.0 — outclr.w is the WATER ALPHA and if the tint owned it every tinted face
//                  in the world would go see-through. Row 0 must be the identity or the "no
//                  tint" path stops being free. Every component must be <= 1.0, because outclr
//                  is clamped to [0,1] before the TexEnv ever sees it and a component above 1
//                  is not a brighter biome, it is a silently clipped one.
//
// Own binary and own main(), appended to tools/run_host_tests.sh rather than merged into the
// mesher stanzas, for the reason every stanza in that file is appended: an append is the one
// edit shape that cannot drop another session's work, and that file is shared.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/block.h"
#include "world/mesher.h"
#include "world/scratch.h"

// ── Harness ──────────────────────────────────────────────────────────────────

static int s_checks;
static int s_fails;

#define CHECK(cond, ...) do {                                     \
		s_checks++;                                                \
		if (!(cond)) {                                             \
			s_fails++;                                             \
			printf("  FAIL L%d  ", __LINE__);                      \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		} else {                                                   \
			printf("  ok     ");                                   \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                          \
	} while (0)

// ── Reading a source file as lines ───────────────────────────────────────────
//
// The same shape world/water_alpha_test.c uses, and for the same reason: the things being
// checked below live in files no host binary can link — two shader assembly files no C compiler
// ever reads, and scene/chunk_render.c, which includes <3ds.h>.

#define MAX_LINES 4096

static char* s_lines[MAX_LINES];
static int   s_nlines;

static void freeLines(void)
{
	for (int i = 0; i < s_nlines; i++) free(s_lines[i]);
	s_nlines = 0;
}

static int loadLines(const char* path)
{
	freeLines();

	FILE* f = fopen(path, "rb");
	if (!f) return 0;

	char buf[2048];
	while (s_nlines < MAX_LINES && fgets(buf, sizeof buf, f)) {
		size_t n = strlen(buf);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
		s_lines[s_nlines] = malloc(n + 1);
		memcpy(s_lines[s_nlines], buf, n + 1);
		s_nlines++;
	}
	fclose(f);
	return s_nlines;
}

// The 1-based line number of the first line containing `needle`, or 0.
static int findLine(const char* needle)
{
	for (int i = 0; i < s_nlines; i++)
		if (strstr(s_lines[i], needle)) return i + 1;
	return 0;
}

// How many lines contain `needle`, ignoring ones whose first non-blank character starts a
// comment — so a rule described in prose is never mistaken for the rule being in force.
// `;` for the .pica files, `//` for C.
static int countCode(const char* needle, const char* comment)
{
	int n = 0;
	for (int i = 0; i < s_nlines; i++) {
		const char* p = s_lines[i];
		while (*p == ' ' || *p == '\t') p++;
		if (comment[0] == ';' && *p == ';') continue;
		if (comment[0] == '/' && p[0] == '/' && p[1] == '/') continue;
		if (strstr(s_lines[i], needle)) n++;
	}
	return n;
}

// ── Mesh fixtures ────────────────────────────────────────────────────────────

static MeshScratch g_ms;
static MeshOut     g_out;
static MeshVertex  g_verts[MESH_MAX_VERTS];
static uint16_t    g_idx[MESH_MAX_INDICES];

static void msReset(void)
{
	memset(&g_ms, 0, sizeof g_ms);
}

// lx/ly/lz are chunk-local and run -1..16: -1 and 16 are the scratch's border, the cells the
// real world/scratch.c fills from the neighbouring chunks.
static void msSet(int lx, int ly, int lz, BlockId id)
{
	g_ms.blocks[scratchIndex(lx + 1, ly + 1, lz + 1)] = id;
}

// The tint of the COLUMN at chunk-local (lx, lz). Same -1..16 range as msSet.
static void msTint(int lx, int lz, uint8_t row)
{
	g_ms.tint[scratchColumn(lx + 1, lz + 1)] = row;
	if (row) g_ms.tint_any = true;
}

static void runMesh(void)
{
	memset(g_verts, 0, sizeof g_verts);
	memset(g_idx, 0, sizeof g_idx);
	g_out.verts     = g_verts;
	g_out.indices   = g_idx;
	g_out.vert_cap  = MESH_MAX_VERTS;
	g_out.index_cap = MESH_MAX_INDICES;
	meshChunk(&g_out, &g_ms);
}

// A flat lid of grass at y = GROUND_Y filling the whole scratch INCLUDING the border, with
// solid dirt beneath it and air above.
//
// The border matters: a plane that stopped at the chunk edge would expose side faces there and
// give the edge columns different AO from the middle, and this fixture's whole value is that
// every top face in it has identical AO and identical light. That is the condition the smear
// needs, and it is the condition real open ground satisfies.
#define GROUND_Y 8

static void buildGrassPlain(void)
{
	msReset();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++) {
			msSet(lx, GROUND_Y, lz, (BlockId)BLOCK_GRASS);
			for (int ly = -1; ly < GROUND_Y; ly++)
				msSet(lx, ly, lz, (BlockId)BLOCK_DIRT);
		}
}

// ── The emitted quads, read back ─────────────────────────────────────────────
//
// Every quad is exactly four consecutive vertices — world/mesher.c's emitFace and emitCross
// both advance vert_count by four per quad and scene/chunk_render.c's single shared index
// buffer rests on that same invariant — so quad k is vertices [4k, 4k+4).

typedef struct {
	int     x0, x1, z0, z1;   // footprint in CELLS, inclusive
	int     y;
	uint8_t face;
	uint8_t tint;
	uint8_t ao;
	bool    flat_tint;        // all four vertices agree on the tint
} Quad;

static int quadCount(void) { return (int)(g_out.vert_count / 4); }

static Quad quadAt(int k)
{
	const MeshVertex* v = &g_verts[k * 4];
	Quad q;

	q.x0 = q.x1 = v[0].x;
	q.z0 = q.z1 = v[0].z;
	q.y  = v[0].y;
	for (int i = 1; i < 4; i++) {
		if (v[i].x < q.x0) q.x0 = v[i].x;
		if (v[i].x > q.x1) q.x1 = v[i].x;
		if (v[i].z < q.z0) q.z0 = v[i].z;
		if (v[i].z > q.z1) q.z1 = v[i].z;
	}

	q.face = meshNrmFace(v[0].nrm);
	q.tint = meshAoTint(v[0].ao);
	q.ao   = meshAoValue(v[0].ao);

	q.flat_tint = true;
	for (int i = 1; i < 4; i++)
		if (meshAoTint(v[i].ao) != q.tint) q.flat_tint = false;

	// A quad's corners sit on CELL BOUNDARIES, so a footprint of one cell spans x0..x0+1 in
	// vertex coordinates. Convert to inclusive cell indices; a degenerate span (a side face,
	// which is flat on one axis) keeps its single cell.
	if (q.x1 > q.x0) q.x1--;
	if (q.z1 > q.z0) q.z1--;
	return q;
}

// ── 1. The packing ───────────────────────────────────────────────────────────

static void testTintPacking(void)
{
	puts("\n-- packing: the ao byte carries both, and neither can see the other --");

	int bad = 0;
	for (unsigned ao = 0; ao <= MESH_AO_MASK; ao++)
		for (unsigned t = 0; t < MESH_TINT_ROWS; t++) {
			const uint8_t b = meshAoPack((uint8_t)ao, (uint8_t)t);
			if (meshAoValue(b) != ao || meshAoTint(b) != t) bad++;
		}
	CHECK(bad == 0, "all %u (ao, tint) pairs round-trip through the ao byte", 4u * MESH_TINT_ROWS);

	int leaks = 0;
	for (unsigned ao = 0; ao <= MESH_AO_MASK; ao++)
		if (meshAoPack((uint8_t)ao, MESH_TINT_NONE) != (uint8_t)ao) leaks++;
	CHECK(leaks == 0, "MESH_TINT_NONE packs to the bare AO byte, so an untinted vertex is "
	                  "byte-identical to a pre-v1.8.8 one");

	// The reason the shader has to mask at all. If a tint index could reach the low two bits
	// the ao curve would read it as occlusion and the face would blow out.
	CHECK(meshAoValue(meshAoPack(3, 7)) == 3,
	      "the largest tint index does not disturb the occlusion under it");
	CHECK(meshAoPack(3, 7) == 31, "ao 3 with tint 7 is byte 31, not 3 (%u)", meshAoPack(3, 7));

	CHECK(MESH_TINT_NONE == 0, "the untinted row is row 0");
	CHECK(MESH_AO_BITS == 2 && MESH_TINT_BITS == 3,
	      "2 occlusion bits and 3 tint bits, leaving bits 5..7 free");
}

// ── 2. The zero path ─────────────────────────────────────────────────────────

static void testUntintedIsUnchanged(void)
{
	puts("\n-- no tint band filled: every vertex is exactly what it always was --");

	buildGrassPlain();
	runMesh();

	CHECK(!g_ms.tint_any, "a scratch nobody tinted reports tint_any false");
	CHECK(g_out.vert_count > 0, "the fixture meshed something (%u vertices)", g_out.vert_count);

	int tinted = 0, out_of_range = 0;
	for (uint32_t i = 0; i < g_out.vert_count; i++) {
		if (meshAoTint(g_verts[i].ao) != 0) tinted++;
		if (g_verts[i].ao > MESH_AO_MASK)   out_of_range++;
	}
	CHECK(tinted == 0, "no vertex carries a tint (%d did)", tinted);
	CHECK(out_of_range == 0,
	      "every ao byte is still 0..3, so the pinned mesh hashes cannot have moved (%d were not)",
	      out_of_range);
}

// ── 3. Which faces the biome may colour ──────────────────────────────────────

static void testGrassTopTintedSidesAreNot(void)
{
	puts("\n-- the grass block: lid tinted, half-grass/half-dirt sides left alone --");

	// One grass block standing alone on dirt, so all six of its faces are exposed and can be
	// inspected. Tinting a lone block also proves the rule is per-face and not per-block.
	msReset();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msSet(lx, -1, lz, (BlockId)BLOCK_DIRT);
	msSet(4, 0, 4, (BlockId)BLOCK_GRASS);
	msTint(4, 4, 5);   // desert
	runMesh();

	int top_tinted = 0, top_seen = 0, side_tinted = 0, side_seen = 0;
	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		if (q.face == FACE_TOP) {
			top_seen++;
			if (q.tint == 5) top_tinted++;
		} else if (q.face != FACE_BOTTOM) {
			side_seen++;
			if (q.tint != 0) side_tinted++;
		}
	}

	CHECK(top_seen >= 1, "the grass block's top face was emitted (%d)", top_seen);
	CHECK(top_tinted == top_seen, "every top face carries the column's biome row (%d of %d)",
	      top_tinted, top_seen);
	CHECK(side_seen >= 4, "its four sides were emitted (%d)", side_seen);
	CHECK(side_tinted == 0,
	      "no side face is tinted — BTEX_GRASS_SIDE is grass OVER DIRT and there is no fragment "
	      "shader to mask the dirt half (%d were)", side_tinted);
}

// v1.8.17: this used to be the fixture for "Dirt is not tintable at all, in any biome" — see the
// log for v1.8.8 through v1.8.16. That was correct for THIS fixture's texture (BTEX_GRASS_SIDE,
// grass over dirt, no fragment shader to mask it) but it was never true of dirt's OWN block:
// world/registry.c gives BLOCK_DIRT plain BTEX_DIRT on all six faces, one uniform texture shared
// with nothing else, so there was never a half-tile to protect there. steve's original ask —
// "per-biome wood colours, blocks, textures, types, leaves, logs, planks, dirt, scenery, flowers,
// grass" — shipped everything but dirt at v1.8.8; world/mesher.c's blockFaceTintable() excluded
// BLOCK_DIRT outright and this is the fixture that proves it, MEASURED red against that code (see
// tools/run_host_tests.sh's biome_tint_test stanza for the exact RED output).
static void testDirtTintedOnAllFaces(void)
{
	puts("\n-- the dirt block: the one gap left after v1.8.8, closed at v1.8.17 --");

	// A lone dirt block with air on every side, so all six faces are exposed and the rule can
	// be checked per face the same way testGrassTopTintedSidesAreNot checks grass. Unlike grass,
	// EVERY face should take the column's row — there is no shared texture to protect.
	msReset();
	msSet(4, 0, 4, (BlockId)BLOCK_DIRT);
	msTint(4, 4, 5);   // desert — 1.00/0.50/0.40, not the identity
	runMesh();

	int seen = 0, tinted = 0;
	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		seen++;
		if (q.tint == 5) tinted++;
	}
	CHECK(seen == 6, "the lone dirt block emits all six faces (%d)", seen);
	CHECK(tinted == seen,
	      "every face carries the column's biome row, top AND sides AND bottom (%d of %d)",
	      tinted, seen);

	// A whole plane of dirt in a tinted column, top and bottom faces alike — the exact fixture
	// that used to assert the opposite.
	msReset();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++) {
			msSet(lx, 0, lz, (BlockId)BLOCK_DIRT);
			msTint(lx, lz, 5);
		}
	runMesh();

	int dirt_seen = 0, dirt_tinted = 0;
	for (uint32_t i = 0; i < g_out.vert_count; i++) {
		dirt_seen++;
		if (meshAoTint(g_verts[i].ao) == 5) dirt_tinted++;
	}
	CHECK(g_out.vert_count > 0 && dirt_tinted == dirt_seen,
	      "a whole plane of dirt in a tinted column is tinted on every vertex, top and bottom "
	      "alike (%d of %u vertices carried row 5)", dirt_tinted, g_out.vert_count);
}

// v1.8.18: leaves take the DIRT restriction, not the grass one -- all six faces, closed for the
// same reason dirt was at v1.8.17. world/registry.c gives all three leaf blocks one uniform
// texture per block (never shared with anything else's crust the way BTEX_GRASS_SIDE is), so
// there is no half-tile for a partial restriction to protect. mesher.c's blockFaceTintable used
// to say "nobody asked for it" about leaves; steve's ask for this task named grass, leaves and
// foliage together, so this closes that gap the same way testDirtTintedOnAllFaces closed dirt's.
static void testLeavesTintedOnAllFaces(void)
{
	puts("\n-- leaves: the dirt-style all-six-faces rule, closed at v1.8.18 --");

	const BlockId leaves[3] = {
		(BlockId)BLOCK_LEAVES, (BlockId)BLOCK_BIRCH_LEAVES, (BlockId)BLOCK_SPRUCE_LEAVES
	};
	const char* names[3] = { "oak leaves", "birch leaves", "spruce leaves" };

	for (int i = 0; i < 3; i++) {
		msReset();
		msSet(4, 0, 4, leaves[i]);
		msTint(4, 4, 6);   // jungle — 0.62/1.00/0.44, not the identity
		runMesh();

		int seen = 0, tinted = 0;
		for (int k = 0; k < quadCount(); k++) {
			const Quad q = quadAt(k);
			seen++;
			if (q.tint == 6) tinted++;
		}
		CHECK(seen == 6, "%s: the lone block emits all six faces (%d)", names[i], seen);
		CHECK(tinted == seen,
		      "%s: every face carries the column's biome row (%d of %d)",
		      names[i], tinted, seen);
	}
}

static void testStrandMatchesGroundBeneath(void)
{
	puts("\n-- a grass strand is the colour of the grass it stands on --");

	const BlockId plants[2] = { (BlockId)BLOCK_TALL_GRASS, (BlockId)BLOCK_FERN };
	const char*   names[2]  = { "tall grass", "fern" };

	for (int p = 0; p < 2; p++) {
		buildGrassPlain();
		for (int lz = -1; lz <= CHUNK_DIM; lz++)
			for (int lx = -1; lx <= CHUNK_DIM; lx++)
				msTint(lx, lz, 6);   // jungle
		msSet(4, GROUND_Y + 1, 4, plants[p]);
		runMesh();

		// A cross emits CROSS_QUADS quads whose four corners span the whole cell on BOTH
		// horizontal axes — that is what makes an X rather than a plane, and it is how a cross
		// quad is told apart from the grass top under it, which spans only one axis when merged
		// but is flat in y. Crosses are the only quads in this fixture with two distinct y.
		int cross_quads = 0, cross_tinted = 0, flat = 0;
		for (int k = 0; k < quadCount(); k++) {
			const MeshVertex* v = &g_verts[k * 4];
			bool two_y = false;
			for (int i = 1; i < 4; i++) if (v[i].y != v[0].y) two_y = true;
			if (!two_y) continue;
			const Quad q = quadAt(k);
			cross_quads++;
			if (q.tint == 6) cross_tinted++;
			if (q.flat_tint) flat++;
		}

		CHECK(cross_quads == 4, "%s emits its four quads (%d)", names[p], cross_quads);
		CHECK(cross_tinted == cross_quads,
		      "%s carries the ground's biome row on every quad (%d of %d)",
		      names[p], cross_tinted, cross_quads);
		CHECK(flat == cross_quads,
		      "%s: all four vertices of each quad agree, so it is one colour from every angle "
		      "(%d of %d)", names[p], flat, cross_quads);

		// And the ground under it agrees, which is the actual ask: the strand must MATCH the
		// block, not merely be tinted.
		int ground = 0, ground_match = 0;
		for (int k = 0; k < quadCount(); k++) {
			const MeshVertex* v = &g_verts[k * 4];
			bool two_y = false;
			for (int i = 1; i < 4; i++) if (v[i].y != v[0].y) two_y = true;
			if (two_y) continue;
			const Quad q = quadAt(k);
			if (q.face != FACE_TOP) continue;
			ground++;
			if (q.tint == 6) ground_match++;
		}
		CHECK(ground > 0 && ground_match == ground,
		      "%s: the grass beneath carries the same row (%d of %d tops)",
		      names[p], ground_match, ground);
	}
}

// The two-block clump is ONE plant drawn as two stacked cross blocks — worldgen.c writes
// BLOCK_TALL_GRASS at y and BLOCK_TALL_GRASS_TOP at y+1 — so the two halves have to come out
// of the mesher the same colour or the plant reads as two different plants stacked on each
// other. steve reported exactly that: "on top of a biome specific type of grass, there is
// another piece of grass that is not for that biome."
//
// The fixture is JUNGLE and that is load-bearing. Row 3 (plains) is {1,1,1} and row 0
// (MESH_TINT_NONE) is also {1,1,1} — byte-identical — so an untinted tip in a plains fixture
// renders correctly by coincidence and this test would pass with the bug fully present. Only a
// biome whose row is not the identity can tell "tinted plains" from "not tinted at all".
static void testTwoBlockClumpIsOneColour(void)
{
	puts("\n-- both halves of a two-block clump take the same biome colour --");

	buildGrassPlain();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msTint(lx, lz, 6);   // jungle — 0.62/1.00/0.44, not the identity

	msSet(4, GROUND_Y + 1, 4, (BlockId)BLOCK_TALL_GRASS);
	msSet(4, GROUND_Y + 2, 4, (BlockId)BLOCK_TALL_GRASS_TOP);
	runMesh();

	// Same cross-quad identification as testStrandMatchesGroundBeneath: a cross spans both
	// horizontal axes, so it is the only thing here with two distinct y on one quad.
	int cross_quads = 0, cross_tinted = 0;
	for (int k = 0; k < quadCount(); k++) {
		const MeshVertex* v = &g_verts[k * 4];
		bool two_y = false;
		for (int i = 1; i < 4; i++) if (v[i].y != v[0].y) two_y = true;
		if (!two_y) continue;
		cross_quads++;
		if (quadAt(k).tint == 6) cross_tinted++;
	}

	CHECK(cross_quads == 8, "the clump emits both halves' quads (%d, want 8)", cross_quads);
	CHECK(cross_tinted == cross_quads,
	      "every quad of the clump carries the jungle row (%d of %d) — a shortfall of 4 is the "
	      "tip falling through to MESH_TINT_NONE", cross_tinted, cross_quads);
}

// ── 4. THE SMEAR ─────────────────────────────────────────────────────────────

// The tint column (lx, lz) is entitled to, for a border split on `axis` at `at`.
static uint8_t wantTint(int axis, int at, int lx, int lz)
{
	const int c = (axis == 0) ? lx : lz;
	return (uint8_t)(c < at ? 2 : 5);   // taiga | desert — two rows that are not each other
}

// v1.8.18: whether (lx, lz) sits in the one-column band world/mesher.c's tintBlendBuild is
// allowed to dither. Its four-orthogonal-neighbour check only ever sees the OTHER side of a
// split from the two columns immediately touching it — column `at-2`'s neighbours are `at-3`
// and `at-1`, both still on its own side, so it never disagrees with any of them and takes the
// fast, undithered path. Only `at-1` and `at` ever have a neighbour one step across the line.
static bool inBlendBand(int axis, int at, int lx, int lz)
{
	const int c = (axis == 0) ? lx : lz;
	return c == at - 1 || c == at;
}

// Meshes a flat grass plain cut in half by a biome border and asserts that EVERY cell under
// EVERY emitted quad is entitled to the colour that quad was given.
//
// Returns the number of cells that were drawn in the wrong biome's colour.
static int borderMismatches(int axis, int at, int* merged_quads, int* widest)
{
	buildGrassPlain();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msTint(lx, lz, wantTint(axis, at, lx, lz));
	runMesh();

	int bad = 0;
	*merged_quads = 0;
	*widest = 1;

	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		if (q.face != FACE_TOP) continue;   // no plants in this fixture, so these are all grass

		const int cells = (q.x1 - q.x0 + 1) * (q.z1 - q.z0 + 1);
		if (cells > 1) (*merged_quads)++;
		if (cells > *widest) *widest = cells;

		for (int z = q.z0; z <= q.z1; z++)
			for (int x = q.x0; x <= q.x1; x++) {
				// v1.8.18: the one-column band tintBlendBuild dithers is allowed to show EITHER
				// of the two biomes either side of the split — testBiomeBorderBlends below
				// asserts the dither is not a no-op. Everywhere else the match must still be
				// exact, which is the actual smear this test exists to catch: the old bug could
				// paint a run up to fifteen cells the wrong colour, far past this one-column
				// band, so it is still caught in full by the `else` branch here.
				if (inBlendBand(axis, at, x, z)) {
					if (q.tint != 2 && q.tint != 5) bad++;   // taiga | desert, this fixture's pair
				} else if (q.tint != wantTint(axis, at, x, z)) {
					bad++;
				}
			}
	}
	return bad;
}

static void testBiomeBorderIsExact(void)
{
	puts("\n-- THE SMEAR: a biome border must be exact, not merged across --");

	// Both orientations. Which axis a top face merges along is decided by mesher.c's face
	// table, not by anything this file should be asserting, so testing one orientation alone
	// would be a check that passes because nothing merged rather than because nothing smeared.
	const char* names[2] = { "border along x", "border along z" };
	for (int axis = 0; axis < 2; axis++) {
		int merged = 0, widest = 1;
		const int bad = borderMismatches(axis, 8, &merged, &widest);
		CHECK(bad == 0,
		      "%s at x/z=8: every cell is drawn in its own biome's colour (%d cells were not)",
		      names[axis], bad);
		printf("         (%s: %d merged quads, widest covers %d cells)\n",
		       names[axis], merged, widest);
	}
}

// ── 4a2. THE BLEND ────────────────────────────────────────────────────────────
//
// v1.8.18: steve asked for the border itself to fade rather than step, Minecraft-style.
// world/mesher.c cannot average real colour (see tintBlendBuild's own comment for why: the
// vertex only ever carries a 3-bit palette INDEX, and averaging two indices is not averaging
// two colours), so it dithers instead — a one-column-wide band on each side of the split mixes
// in the other biome's tint on a fraction of its columns. This test is the one that would catch
// the dither being wired up but doing nothing: it asserts the border strip actually shows BOTH
// colours, not a clean either/or step that merely moved.
static void testBiomeBorderBlends(void)
{
	puts("\n-- THE BLEND: the border strip mixes both biomes instead of stepping --");

	const char* names[2] = { "blend along x", "blend along z" };
	for (int axis = 0; axis < 2; axis++) {
		buildGrassPlain();
		for (int lz = -1; lz <= CHUNK_DIM; lz++)
			for (int lx = -1; lx <= CHUNK_DIM; lx++)
				msTint(lx, lz, wantTint(axis, 8, lx, lz));
		runMesh();

		// Tracked per BAND COLUMN (at-1 and at), not pooled across both. Pooling would pass for
		// a plain hard step too -- column at-1 always taiga, column at always desert is exactly
		// what "no dither at all" looks like, and the two columns combined would still show
		// both colours. Only checking EACH column individually along its own length proves the
		// dither actually mixes rather than just relabelling which side of the step is which.
		bool saw_taiga[2] = { false, false }, saw_desert[2] = { false, false };
		bool band_touched = false;
		int  outside_bad  = 0;

		for (int k = 0; k < quadCount(); k++) {
			const Quad q = quadAt(k);
			if (q.face != FACE_TOP) continue;   // no plants in this fixture, so these are all grass

			for (int z = q.z0; z <= q.z1; z++)
				for (int x = q.x0; x <= q.x1; x++) {
					const int c = (axis == 0) ? x : z;
					if (c == 8 - 1 || c == 8) {
						band_touched = true;
						const int slot = (c == 8) ? 1 : 0;
						if (q.tint == 2) saw_taiga[slot]  = true;
						if (q.tint == 5) saw_desert[slot] = true;
					} else if (q.tint != wantTint(axis, 8, x, z)) {
						outside_bad++;
					}
				}
		}

		CHECK(band_touched, "%s: the border strip was actually meshed", names[axis]);
		for (int slot = 0; slot < 2; slot++) {
			const int col = 8 - 1 + slot;
			CHECK(saw_taiga[slot] && saw_desert[slot],
			      "%s: band column %d shows BOTH biomes along its own length, not a single "
			      "colour (taiga seen=%d, desert seen=%d) — one colour here means this column "
			      "is a relabelled step, not a dither",
			      names[axis], col, (int)saw_taiga[slot], (int)saw_desert[slot]);
		}
		CHECK(outside_bad == 0,
		      "%s: every cell OUTSIDE the one-column blend band still matches its own biome "
		      "exactly (%d did not) — the dither must not leak past the border it softens",
		      names[axis], outside_bad);
	}
}

// ── 4b. THE SMEAR, for a full cube ───────────────────────────────────────────
//
// testBiomeBorderIsExact above proves the merge key holds for grass's single TOP face. Grass's
// cross-shaped strands never reach mergeRun at all — meshPass returns before the six-face loop
// for anything that is not BLOCK_SHAPE_FULL_CUBE — so grass only ever exercises ONE of dirt's six
// tintable faces through that path. Dirt is a full cube on all six, so it is worth proving the
// same key holds for a block whose faces are ALL tintable rather than assuming the shared
// function behaves the same way it did for grass.

// A flat plane of DIRT (full cube) at y = GROUND_Y filling the whole scratch including the
// border, with air above — the same shape buildGrassPlain uses, but the cap itself is dirt so
// the proof below runs dirt through its own mergeRun call rather than reusing grass's.
static void buildDirtPlain(void)
{
	msReset();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msSet(lx, GROUND_Y, lz, (BlockId)BLOCK_DIRT);
}

// Same shape as borderMismatches, over a plane of dirt instead of grass.
static int dirtBorderMismatches(int axis, int at, int* merged_quads, int* widest)
{
	buildDirtPlain();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msTint(lx, lz, wantTint(axis, at, lx, lz));
	runMesh();

	int bad = 0;
	*merged_quads = 0;
	*widest = 1;

	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		if (q.face != FACE_TOP) continue;   // the slab's bottom is never exposed in this fixture

		const int cells = (q.x1 - q.x0 + 1) * (q.z1 - q.z0 + 1);
		if (cells > 1) (*merged_quads)++;
		if (cells > *widest) *widest = cells;

		for (int z = q.z0; z <= q.z1; z++)
			for (int x = q.x0; x <= q.x1; x++) {
				// v1.8.18: same one-column dither tolerance borderMismatches applies above —
				// dirt runs the tint through the identical faceTint()/tintBlendBuild() path, so
				// its border strip is entitled to the same either-biome allowance.
				if (inBlendBand(axis, at, x, z)) {
					if (q.tint != 2 && q.tint != 5) bad++;   // taiga | desert, this fixture's pair
				} else if (q.tint != wantTint(axis, at, x, z)) {
					bad++;
				}
			}
	}
	return bad;
}

static void testDirtBiomeBorderIsExact(void)
{
	puts("\n-- THE SMEAR, for dirt: a full cube's own merge key must not jump the border --");

	const char* names[2] = { "dirt border along x", "dirt border along z" };
	for (int axis = 0; axis < 2; axis++) {
		int merged = 0, widest = 1;
		const int bad = dirtBorderMismatches(axis, 8, &merged, &widest);
		CHECK(bad == 0,
		      "%s at x/z=8: every dirt cell is drawn in its own biome's colour (%d cells were not)",
		      names[axis], bad);
		CHECK(merged > 0,
		      "%s: dirt tops still merge away from the border (%d merged quads)",
		      names[axis], merged);
		printf("         (%s: %d merged quads, widest covers %d cells)\n",
		       names[axis], merged, widest);
	}
}

// The guard that stops the test above from passing for the wrong reason.
//
// If mergeRun ever stops merging in this fixture — a face table change, a merge cap of 1, a
// mergeCandidate that refuses everything — then no quad ever spans the border, the smear
// becomes unreachable, and testBiomeBorderIsExact goes green while proving nothing at all.
static void testMergingActuallyHappens(void)
{
	puts("\n-- the fixture really does merge, so the smear test can fail --");

	int merged_x = 0, widest_x = 1, merged_z = 0, widest_z = 1;
	(void)borderMismatches(0, 8, &merged_x, &widest_x);
	(void)borderMismatches(1, 8, &merged_z, &widest_z);

	CHECK(merged_x + merged_z > 0,
	      "at least one top face was merged across more than one cell (%d + %d quads)",
	      merged_x, merged_z);

	// And with NO border at all the run reaches its cap, which is what proves the merge is
	// being cut short by the tint rather than by geometry.
	int merged_flat = 0, widest_flat = 1;
	buildGrassPlain();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++)
			msTint(lx, lz, 2);
	runMesh();
	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		if (q.face != FACE_TOP) continue;
		const int cells = (q.x1 - q.x0 + 1) * (q.z1 - q.z0 + 1);
		if (cells > 1) merged_flat++;
		if (cells > widest_flat) widest_flat = cells;
	}
	CHECK(widest_flat > 1,
	      "one uniform biome still merges its grass tops into wide quads (widest %d cells)",
	      widest_flat);
	CHECK(widest_flat >= widest_x || widest_flat >= widest_z,
	      "a uniform biome merges at least as wide as a split one (%d vs %d/%d) — the tint "
	      "SPLITS runs, it does not create them", widest_flat, widest_x, widest_z);
}

// A tint must not split runs of blocks that cannot be tinted. Folding the raw column value
// into the merge key for stone and dirt would cost meshing everywhere for no visual difference
// at all, and it is the obvious wrong way to fix the smear.
static void testUntintableRunsStillMerge(void)
{
	puts("\n-- an untintable block still merges across a biome border --");

	msReset();
	for (int lz = -1; lz <= CHUNK_DIM; lz++)
		for (int lx = -1; lx <= CHUNK_DIM; lx++) {
			msSet(lx, 0, lz, (BlockId)BLOCK_STONE);
			msSet(lx, -1, lz, (BlockId)BLOCK_STONE);
			msTint(lx, lz, wantTint(0, 8, lx, lz));
		}
	runMesh();

	int widest = 1;
	for (int k = 0; k < quadCount(); k++) {
		const Quad q = quadAt(k);
		if (q.face != FACE_TOP) continue;
		const int cells = (q.x1 - q.x0 + 1) * (q.z1 - q.z0 + 1);
		if (cells > widest) widest = cells;
	}
	CHECK(widest > 1,
	      "stone tops still merge into wide quads with a biome border through them (widest %d "
	      "cells) — the tint splits only what it can colour", widest);
}

// ── 5. The palette ───────────────────────────────────────────────────────────

static void testPaletteIsSane(void)
{
	puts("\n-- the palette: row 0 identity, nothing above 1.0 --");

	const MeshTint none = meshTintRow(MESH_TINT_NONE);
	CHECK(none.r == 1.0f && none.g == 1.0f && none.b == 1.0f,
	      "row 0 is exactly (1,1,1), so an untinted face multiplies by one "
	      "(%.2f %.2f %.2f)", (double)none.r, (double)none.g, (double)none.b);

	// outclr is clamped to [0,1] before the TexEnv sees it. A component above 1.0 is not a
	// brighter biome, it is a silently clipped one, and the biome comes out looking like
	// whichever channels happened to fit.
	int over = 0, nonpositive = 0;
	for (unsigned i = 0; i < MESH_TINT_ROWS; i++) {
		const MeshTint t = meshTintRow((uint8_t)i);
		if (t.r > 1.0f || t.g > 1.0f || t.b > 1.0f) over++;
		if (t.r <= 0.0f || t.g <= 0.0f || t.b <= 0.0f) nonpositive++;
	}
	CHECK(over == 0, "no palette component exceeds 1.0 — outclr is clamped, so above 1 is "
	                 "clipped and not brighter (%d rows did)", over);
	CHECK(nonpositive == 0, "no palette component is zero or negative (%d rows were)", nonpositive);

	// PLAINS is the biome the art was painted for and the one most of the world is.
	const MeshTint plains = meshTintRow(MESH_TINT_ROW_FOR_BIOME(2));   // BIOME_PLAINS
	CHECK(plains.r == 1.0f && plains.g == 1.0f && plains.b == 1.0f,
	      "plains is the identity, so most of the world renders exactly as it did before");

	// Every biome must land on its own row, and none of them on row 0.
	int on_zero = 0;
	for (unsigned b = 0; b < 6; b++)   // BIOME_COUNT
		if (MESH_TINT_ROW_FOR_BIOME(b) == MESH_TINT_NONE) on_zero++;
	CHECK(on_zero == 0, "no biome maps onto the untinted row (%d did)", on_zero);
	CHECK(MESH_TINT_ROW_FOR_BIOME(5) < MESH_TINT_ROWS,
	      "the last biome fits the palette (row %u of %u)",
	      MESH_TINT_ROW_FOR_BIOME(5), MESH_TINT_ROWS);

	// Two biomes that are meant to look different must actually be different, or the whole
	// feature is a no-op that every check above would still pass.
	const MeshTint taiga  = meshTintRow(2);
	const MeshTint desert = meshTintRow(5);
	CHECK(taiga.r != desert.r || taiga.g != desert.g || taiga.b != desert.b,
	      "taiga and desert are actually different colours");
}

// v1.8.11: the desert row read as "dry olive" instead of sandy tan. The tint LITERAL alone
// (0.94, 0.82, 0.42 — R already bigger than G) looked fine in isolation; the bug only showed up
// in the PRODUCT against grass_top's actual base art, which tools/make_atlas.py's
// tile_grass_top() paints from these four texels (a weighted speckle, plus a highlight this
// test does not need). Base green sits at roughly 1.9x base red, which swallows a tint ratio of
// only 1.15x and comes out green-dominant (measured hue ~96 degrees) regardless of what the
// tint literal alone suggests. That is why this checks the PRODUCT and not just the row.
static float rgbHueDeg(float r, float g, float b)
{
	const float mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
	const float mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
	const float delta = mx - mn;
	if (delta <= 0.00001f) return 0.0f;   // grey: no hue to be wrong about

	float h;
	if (mx == r)      h = fmodf((g - b) / delta, 6.0f);
	else if (mx == g) h = (b - r) / delta + 2.0f;
	else              h = (r - g) / delta + 4.0f;

	h *= 60.0f;
	if (h < 0.0f) h += 360.0f;
	return h;
}

static void testDesertReadsTanNotOlive(void)
{
	puts("\n-- desert: the PRODUCT against grass_top's real base art reads tan, not olive --");

	// tools/make_atlas.py tile_grass_top(): greens = [(58,112,74),(66,126,82),(74,138,90),(50,100,66)]
	const float grass_top_shades[4][3] = {
		{  58.0f / 255.0f, 112.0f / 255.0f,  74.0f / 255.0f },
		{  66.0f / 255.0f, 126.0f / 255.0f,  82.0f / 255.0f },
		{  74.0f / 255.0f, 138.0f / 255.0f,  90.0f / 255.0f },
		{  50.0f / 255.0f, 100.0f / 255.0f,  66.0f / 255.0f },
	};

	const MeshTint desert = meshTintRow(5);   // BIOME_DESERT's row, same lookup the mesher uses

	// 90 degrees is dead-centre green on the hue wheel and the pre-fix product measured ~96;
	// 65 leaves a wide margin into the yellow/orange band a "sandy tan" needs to sit in, on
	// EVERY base shade the real art actually paints, not just their average.
	int bad = 0;
	for (int i = 0; i < 4; i++) {
		const float pr = grass_top_shades[i][0] * desert.r;
		const float pg = grass_top_shades[i][1] * desert.g;
		const float pb = grass_top_shades[i][2] * desert.b;
		const float hue = rgbHueDeg(pr, pg, pb);
		const bool  ok  = hue <= 65.0f;
		if (!ok) bad++;
		CHECK(ok, "grass_top shade %d (%.0f,%.0f,%.0f) tints to hue %.1f (<=65, yellow/tan)",
		      i, (double)(grass_top_shades[i][0] * 255.0f), (double)(grass_top_shades[i][1] * 255.0f),
		      (double)(grass_top_shades[i][2] * 255.0f), (double)hue);
	}
	CHECK(bad == 0, "no grass_top base shade crosses into the green band under the desert tint");

	// The ratio invariant the fix actually rests on: the tint's R:G must overcome grass_top's
	// own G:R bias (~1.9x average), not merely exceed 1 — 0.94/0.82 (ratio 1.15) was already
	// R > G and still produced olive.
	CHECK(desert.r / desert.g >= 1.8f,
	      "desert tint R:G ratio (%.3f) is wide enough to overcome the base art's G:R bias",
	      (double)(desert.r / desert.g));
}

// ── 6. The renderer, by text ─────────────────────────────────────────────────

static void testRendererUploadsPalette(void)
{
	puts("\n-- scene/chunk_render.c fills the bank from the real table --");

	CHECK(loadLines("source/scene/chunk_render.c") > 0, "scene/chunk_render.c was read");

	CHECK(countCode("shaderInstanceGetUniformLocation(s_program.vertexShader, \"tintPalette\")",
	                "//") == 1,
	      "the palette's uniform location is looked up exactly once");

	// The numbers must come from world/mesher.h's meshTintRow and not from a copy here, or the
	// suite above would be proving things about values the GPU never sees.
	CHECK(countCode("meshTintRow(", "//") >= 1,
	      "the upload READS meshTintRow() rather than restating the table");

	const int upload = findLine("s_uloc_tintpalette + (int)i");
	CHECK(upload > 0, "the bank is uploaded row by row at s_uloc_tintpalette + i (L%d)", upload);

	CHECK(countCode("i < MESH_TINT_ROWS", "//") >= 1,
	      "the upload loop runs MESH_TINT_ROWS times, not a literal");

	// outclr.w is the WATER ALPHA (v1.8.2). If the tint owned it, every tinted face in the
	// world would go see-through.
	const int wline = findLine("t.r, t.g, t.b, 1.0f");
	CHECK(wline > 0,
	      "the palette's .w is written 1.0 and does not touch the water alpha in outclr.w (L%d)",
	      wline);
}

// ── 7. The shaders, by text ──────────────────────────────────────────────────

static void checkOneShader(const char* path, const char* label)
{
	printf("   [%s]\n", label);
	CHECK(loadLines(path) > 0, "%s was read", label);

	CHECK(countCode(".fvec tintPalette[8]", ";") == 1,
	      "%s declares the 8-row palette bank", label);

	// The split. Each of these four is asserted on its own: the whole decode being merely
	// PRESENT is not enough, because the operand is what carries the meaning — the exact
	// failure world/water_alpha_test.c recorded when `mova a0.x, inpack.zzzz` was changed to
	// inpack.xxxx and 34 checks stayed green.
	const int div   = findLine("mul r8.x, inv4, inpack.wwww");
	const int flr   = findLine("flr r8.x, r8.xxxx");
	const int neg   = findLine("mul r9.x, neg4, r8.xxxx");
	const int rest  = findLine("add r9.x, r9.xxxx, inpack.wwww");
	const int mova  = findLine("mova a0.x, r8.xxxx");
	const int fetch = findLine("mov r10, tintPalette[a0.x]");

	CHECK(div > 0,  "%s divides the ao byte by 4 to reach the tint index (L%d)", label, div);
	CHECK(flr > div, "%s floors it to a whole row number, after the divide (L%d)", label, flr);
	CHECK(neg > flr, "%s scales the index back up to subtract it (L%d)", label, neg);
	CHECK(rest > neg,
	      "%s recovers the bare occlusion into r9.x (L%d)", label, rest);
	CHECK(mova > flr && fetch > mova,
	      "%s loads the address register from the FLOORED index and then fetches the row "
	      "(L%d, L%d)", label, mova, fetch);

	// The ao curve must read the SPLIT value. This is the check that catches the whole feature
	// being wired up correctly and then quietly bypassed: leave this line reading inpack.wwww
	// and every tinted face reads its tint as occlusion and blows out to white, with the
	// palette still uploaded and every other check here still green.
	CHECK(countCode("mul r4.x, aoScale, r9.xxxx", ";") == 1,
	      "%s feeds the ao curve from the SPLIT occlusion", label);
	CHECK(countCode("mul r4.x, aoScale, inpack.wwww", ";") == 0,
	      "%s no longer feeds the ao curve the raw, unmasked ao byte", label);

	// The multiply into the colour, and that the grey write it replaced is gone.
	CHECK(countCode("mul r11.xyz, ", ";") == 1,
	      "%s multiplies the tint into the vertex colour", label);
	CHECK(countCode("mov outclr.xyz, r11.xyz", ";") == 1,
	      "%s writes the tinted colour to outclr.xyz", label);
	CHECK(countCode("mov outclr.xyz, r6.zzz", ";") == 0 &&
	      countCode("mov outclr.xyz, r5.xxx", ";") == 0,
	      "%s no longer writes a grey straight to outclr.xyz", label);

	// The water alpha must still come from the faceShade row and not from the tint.
	CHECK(countCode("mov outclr.w,   r3.wwww", ";") == 1,
	      "%s still takes outclr.w from the faceShade row — the tint has not eaten the water "
	      "alpha", label);
}

static void testShadersDecodeTint(void)
{
	puts("\n-- both vertex shaders decode the byte and apply the palette --");

	// The BOUND one first, so a red run names it first. world_dynamic is what both console
	// models actually run since the v1.8.0 shader split; world.v.pica is compiled and never
	// bound, so a drift between them is invisible on hardware.
	checkOneShader("source/shaders/world_dynamic.v.pica", "world_dynamic.v.pica (BOUND)");
	checkOneShader("source/shaders/world.v.pica",         "world.v.pica (compiled, not bound)");
}

// ── 8. The stale comment that made this bug easy to ship ─────────────────────

static void testMesherHeaderTellsTheTruth(void)
{
	puts("\n-- world/mesher.h no longer claims the mesher is not greedy --");

	CHECK(loadLines("source/world/mesher.h") > 0, "world/mesher.h was read");
	CHECK(findLine("Deliberately *not* greedy") == 0,
	      "the stale \"deliberately *not* greedy\" claim is gone — it was three minor versions "
	      "out of date and it is what makes packing a field into a vertex look safe");
	CHECK(findLine("mergeRun") > 0,
	      "the header names mergeRun, so the next person packing a field into a vertex is told "
	      "faces are combined");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== biome tint test ==");

	testTintPacking();
	testUntintedIsUnchanged();
	testGrassTopTintedSidesAreNot();
	testDirtTintedOnAllFaces();
	testLeavesTintedOnAllFaces();
	testStrandMatchesGroundBeneath();
	testTwoBlockClumpIsOneColour();
	testBiomeBorderIsExact();
	testBiomeBorderBlends();
	testDirtBiomeBorderIsExact();
	testMergingActuallyHappens();
	testUntintableRunsStillMerge();
	testPaletteIsSane();
	testDesertReadsTanNotOlive();
	testRendererUploadsPalette();
	testShadersDecodeTint();
	testMesherHeaderTellsTheTruth();

	freeLines();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
