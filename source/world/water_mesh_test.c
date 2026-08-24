// Host tests for the SHAPE of flowing water (v1.8.0 task 22b).
//
// Task 22 shipped the simulation with one disclosed defect: every level rendered as a full
// opaque cube, so a level-1 trickle was indistinguishable from an ocean. This suite is the
// proof that it no longer is, and it is geometry only — no GPU, no console, no screen. The
// claim it can settle is "the mesher emits a surface at the height the level says", and the
// claim it cannot is "that looks right", which is a human's job.
//
// Own main(), and own binary, for the two reasons world/water_test.c's header gives: the
// console Makefile globs every .c under source/world, so the __3DS__ guard below is
// load-bearing rather than tidy, and an appended stanza is the one edit shape to the shared
// tools/run_host_tests.sh that cannot drop another session's work.
//
// What each probe guards, and why it is the check that would go red:
//   testSourceUnchanged  - THE CONTROL. A world of sources, and a world of ordinary terrain,
//                          mesh byte-identically with the water band absent and with it
//                          present-but-empty. This is v1.7.1's output. It must stay green in
//                          every sabotage arm, or the arm proved nothing about the arm.
//   testLevelHeights     - a lone flow cell at each level 1..7: the lid comes down 8-L
//                          eighths, the floor does not move, and the four walls carry the
//                          drop at their top corners and zero at their bottom ones.
//   testFallingIsFull    - a cell with water DIRECTLY ABOVE is drawn full height whatever its
//                          level. water.c gives a falling cell level 7, not 8, so without
//                          this rule a waterfall is a stack of slabs with daylight between.
//   testLevelBoundary    - the hard one. Every ordered pair of unequal levels 1..8 side by
//                          side emits EXACTLY ONE quad on the plane between them, spanning
//                          the floor to the TALLER of the two surfaces, so the step in the
//                          surface is walled and no hole is left. Equal levels emit none.
//   testNoMergeAcrossLevels - a greedy run may not swallow a cell of a different height. One
//                          quad cannot be two heights, and a merge that ignored the drop
//                          would flatten a visible step with the face count still perfect.
//   testPinnedGeometry   - a fixed staircase, hashed. Any change to the emitted bytes is a
//                          one-line diff in this file.
//   testFillScratch      - waterFillScratch copies the simulation's levels into the band the
//                          mesher reads, for the right 18x18x18 window, and agrees with
//                          waterLevelAt cell for cell.
//   testLevelHook        - waterSetLevelHook fires for a level-only change (the case the
//                          on_change hook deliberately stays quiet for) and every cell it
//                          names really is water.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/mesher.h"
#include "world/scratch.h"
#include "world/water.h"
#include "world/world.h"

static int g_checks = 0;
static int g_fails  = 0;

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   line %d: %s\n", line, what);
	} else {
		printf("  ok     %s\n", what);
	}
}

#define CHECK(cond, what) checkAt((cond), (what), __LINE__)

// ── Fixtures ─────────────────────────────────────────────────────────────────────────

static MeshScratch g_ms;
static MeshOut     g_out;
static MeshVertex  g_verts[MESH_MAX_VERTS];
static uint16_t    g_idx[MESH_MAX_INDICES];

// Everything air, band empty, water_any false — which is exactly the state every host suite
// that does not link water.c hands the mesher, and exactly v1.7.1's behaviour.
static void msReset(void)
{
	memset(&g_ms, 0, sizeof g_ms);
}

// lx/ly/lz are chunk-local and run -1..16: -1 and 16 are the scratch's border, the cells the
// real mesher fills from the neighbouring chunks.
static void msSet(int lx, int ly, int lz, BlockId id)
{
	g_ms.blocks[scratchIndex(lx + 1, ly + 1, lz + 1)] = id;
}

// A FLOW cell of level 1..7. Level 8 is a source and is spelled by leaving the band at 0 —
// see the encoding note in world/scratch.h, and "absence means source" in world/water.h.
static void msWater(int lx, int ly, int lz, int lvl)
{
	msSet(lx, ly, lz, (BlockId)BLOCK_WATER);
	if (lvl < WATER_LEVEL_SOURCE)
		g_ms.water[scratchIndex(lx + 1, ly + 1, lz + 1)] = (uint8_t)lvl;
	g_ms.water_any = true;
}

static void runMesh(void)
{
	g_out.verts     = g_verts;
	g_out.indices   = g_idx;
	g_out.vert_cap  = MESH_MAX_VERTS;
	g_out.index_cap = MESH_MAX_INDICES;
	meshChunk(&g_out, &g_ms);
}

// The same FNV-1a over counts, buckets, vertex bytes and indices that world/world_test.c
// pins the four anchor worlds with. Deliberately the same function so the two suites cannot
// disagree about what "the same mesh" means.
static uint32_t meshHash(const MeshOut* out)
{
	uint32_t       h = 2166136261u;
	const uint8_t* b;
	size_t         n;

	#define HASH_BYTES(p, len) do {                       \
			b = (const uint8_t*)(p); n = (len);           \
			for (size_t i_ = 0; i_ < n; i_++) {           \
				h ^= b[i_]; h *= 16777619u;               \
			}                                             \
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

// One emitted quad, read back out of the vertex buffer rather than predicted from the face
// tables. Reading it back is the point: a test that recomputed kFaces' corner layout would
// pass against a mesher that had the layout wrong in the same way.
//
// The mesher never shares a vertex between quads (emitFace appends four and six indices every
// time), so quad i is always verts[4i .. 4i+3].
typedef struct {
	int face;       // nrm bits 0..2
	int drop_top;   // the drop carried by the corners at y_hi
	int y_lo, y_hi;
	int x_lo, x_hi;
	int z_lo, z_hi;
	int consistent; // 0 if the four corners disagreed about anything they must not
} Quad;

static Quad g_q[MESH_MAX_FACES];
static int  g_qn;

static void collectQuads(void)
{
	g_qn = 0;
	for (uint32_t i = 0; i + 4 <= g_out.vert_count; i += 4) {
		const MeshVertex* v = &g_out.verts[i];
		Quad              q;

		q.consistent = 1;
		q.face       = meshNrmFace(v[0].nrm);
		q.x_lo = q.x_hi = v[0].x;
		q.y_lo = q.y_hi = v[0].y;
		q.z_lo = q.z_hi = v[0].z;

		for (int k = 1; k < 4; k++) {
			if (meshNrmFace(v[k].nrm) != q.face) q.consistent = 0;
			if (v[k].x < q.x_lo) q.x_lo = v[k].x;
			if (v[k].x > q.x_hi) q.x_hi = v[k].x;
			if (v[k].y < q.y_lo) q.y_lo = v[k].y;
			if (v[k].y > q.y_hi) q.y_hi = v[k].y;
			if (v[k].z < q.z_lo) q.z_lo = v[k].z;
			if (v[k].z > q.z_hi) q.z_hi = v[k].z;
		}

		// The drop belongs to the TOP plane of the cell and to nothing else, so every corner
		// sitting at y_lo (when the quad has two heights at all) must carry zero — a floor
		// that moved would be a leak out of the bottom of the water, not a surface.
		q.drop_top = -1;
		for (int k = 0; k < 4; k++) {
			const int d = meshNrmDrop(v[k].nrm);
			if (v[k].y == q.y_hi) {
				if (q.drop_top < 0)          q.drop_top = d;
				else if (q.drop_top != d)    q.consistent = 0;
			} else if (d != 0) {
				q.consistent = 0;
			}
		}

		g_q[g_qn++] = q;
	}
}

static int quadsConsistent(void)
{
	for (int i = 0; i < g_qn; i++)
		if (!g_q[i].consistent) return 0;
	return 1;
}

static int maxDropAnywhere(void)
{
	int m = 0;
	for (uint32_t i = 0; i < g_out.vert_count; i++) {
		const int d = meshNrmDrop(g_out.verts[i].nrm);
		if (d > m) m = d;
	}
	return m;
}

// ── Probes ───────────────────────────────────────────────────────────────────────────

// THE CONTROL. It must be green in the sabotaged arm of every experiment below, because a
// sabotage that also broke this one would have broken the mesher outright and proved nothing
// about the rule it was aimed at.
static void testSourceUnchanged(void)
{
	puts("water mesh: a source, and ordinary terrain, mesh exactly as they did in v1.7.1");

	msReset();
	msSet(8, 8, 8, (BlockId)BLOCK_WATER);
	runMesh();
	const uint32_t h_nowater = meshHash(&g_out);
	const uint32_t faces_src = g_out.faces;
	collectQuads();

	CHECK(faces_src == 6, "control: a lone water source is six faces");
	CHECK(maxDropAnywhere() == 0, "control: not one vertex of it carries a height drop");

	// The band present but empty. A lake of sources leaves water_any false in the real fill,
	// so this is the stronger statement: even a scratch that SAYS it has water in it produces
	// the identical bytes when nothing in it is a flow cell.
	msReset();
	msSet(8, 8, 8, (BlockId)BLOCK_WATER);
	g_ms.water_any = true;
	runMesh();
	CHECK(meshHash(&g_out) == h_nowater,
	      "control: an empty water band changes not one byte of a source's mesh");

	// Ordinary terrain, with the band on. Nothing in the world but water may move.
	msReset();
	for (int z = 0; z < 16; z++)
		for (int x = 0; x < 16; x++) {
			msSet(x, 4, z, (BlockId)BLOCK_STONE);
			msSet(x, 5, z, (BlockId)BLOCK_DIRT);
			msSet(x, 6, z, (BlockId)BLOCK_GRASS);
		}
	runMesh();
	const uint32_t h_terrain = meshHash(&g_out);
	const uint32_t f_terrain = g_out.faces;

	g_ms.water_any = true;
	runMesh();
	CHECK(g_out.faces == f_terrain && meshHash(&g_out) == h_terrain,
	      "control: a chunk of plain terrain is byte-identical with the band switched on");
}

static void testLevelHeights(void)
{
	puts("water mesh: a lone flow cell is drawn L/8 of a block tall");

	int bad_faces = 0, bad_top = 0, bad_bottom = 0, bad_side = 0, bad_shape = 0;

	for (int lvl = 1; lvl <= 7; lvl++) {
		const int want = WATER_LEVEL_SOURCE - lvl;   // the drop, in eighths

		msReset();
		msWater(8, 8, 8, lvl);
		runMesh();
		collectQuads();

		if (g_out.faces != 6) bad_faces++;
		if (!quadsConsistent()) bad_shape++;

		int tops = 0, bottoms = 0, sides = 0;
		for (int i = 0; i < g_qn; i++) {
			const Quad* q = &g_q[i];
			if (q->y_lo == 9 && q->y_hi == 9) {          // the lid
				tops++;
				if (q->drop_top != want) bad_top++;
			} else if (q->y_lo == 8 && q->y_hi == 8) {   // the floor
				bottoms++;
				if (q->drop_top != 0) bad_bottom++;
			} else {                                     // one of the four walls
				sides++;
				if (q->y_lo != 8 || q->y_hi != 9 || q->drop_top != want) bad_side++;
			}
		}
		if (tops != 1 || bottoms != 1 || sides != 4) bad_faces++;
	}

	CHECK(bad_faces == 0, "every level 1..7 emits one lid, one floor and four walls");
	CHECK(bad_shape == 0, "and no quad's four corners disagree about face or drop");
	CHECK(bad_top == 0, "the lid of a level-L cell comes down by exactly 8-L eighths");
	CHECK(bad_bottom == 0, "the floor never moves: a shortened cell does not leak downwards");
	CHECK(bad_side == 0, "each wall spans the full cell and carries the drop at its top only");

	// The two ends of the range, spelled out, because an off-by-one that only bit at the
	// extremes would hide inside the loop's totals above.
	msReset();
	msWater(8, 8, 8, 1);
	runMesh();
	CHECK(maxDropAnywhere() == 7, "a level-1 trickle is one eighth of a block tall");

	msReset();
	msWater(8, 8, 8, 7);
	runMesh();
	CHECK(maxDropAnywhere() == 1, "a level-7 cell is seven eighths, not a full cube");

	msReset();
	msWater(8, 8, 8, WATER_LEVEL_SOURCE);
	runMesh();
	CHECK(maxDropAnywhere() == 0, "and a source is still a full cube");
}

static void testFallingIsFull(void)
{
	puts("water mesh: a cell with water above it is drawn full height");

	// Two cells of a fall, both level 7 — which is what water.c gives every cell fed from
	// above, deliberately, so that a waterfall does not weaken as it drops.
	msReset();
	msWater(8, 8, 8, WATER_LEVEL_MAX);
	msWater(8, 9, 8, WATER_LEVEL_MAX);
	runMesh();
	collectQuads();

	CHECK(g_out.faces == 10,
	      "the two cells share a plane and neither draws a face on it: 12 - 2 = 10");
	CHECK(quadsConsistent(), "and every quad's corners agree");

	int interface_quads = 0, lower_dropped = 0, upper_flat = 0, lid = 0;
	for (int i = 0; i < g_qn; i++) {
		const Quad* q = &g_q[i];
		if (q->y_lo == 9 && q->y_hi == 9) interface_quads++;
		if (q->y_hi == 9 && q->drop_top != 0) lower_dropped++;   // the lower cell's walls
		if (q->y_hi == 10 && q->drop_top != 1) upper_flat++;      // the upper cell's walls/lid
		if (q->y_lo == 10 && q->y_hi == 10) lid++;
	}

	CHECK(interface_quads == 0, "nothing is drawn on the plane between them");
	CHECK(lower_dropped == 0,
	      "the LOWER cell is full height despite its level, because water sits on it");
	CHECK(upper_flat == 0, "the upper cell, with sky above it, is seven eighths");
	CHECK(lid == 1, "and the column has exactly one lid, on top");

	// The same thing three deep: the middle of a fall must not shrink either.
	msReset();
	for (int y = 6; y <= 9; y++) msWater(8, y, 8, WATER_LEVEL_MAX);
	runMesh();
	collectQuads();

	int dropped_below_top = 0;
	for (int i = 0; i < g_qn; i++)
		if (g_q[i].y_hi < 10 && g_q[i].drop_top != 0) dropped_below_top++;

	CHECK(g_out.faces == 18, "a four-cell fall is 4*6 - 2*3 shared planes = 18 faces");
	CHECK(dropped_below_top == 0, "and only the topmost cell of the column is short");
}

// The part the brief said was most likely to go wrong, and it is checked exhaustively rather
// than by example: all 8x8 ordered pairs of levels, side by side, sharing the plane x = 9.
static void testLevelBoundary(void)
{
	puts("water mesh: a step between two levels is walled, never left open");

	int bad_count = 0, bad_span = 0, bad_equal = 0, bad_shape = 0;
	int first_a = -1, first_b = -1;

	for (int a = 1; a <= WATER_LEVEL_SOURCE; a++) {
		for (int b = 1; b <= WATER_LEVEL_SOURCE; b++) {
			msReset();
			msWater(8, 8, 8, a);
			msWater(9, 8, 8, b);
			runMesh();
			collectQuads();

			if (!quadsConsistent()) bad_shape++;

			// Only a face perpendicular to X can lie in the plane x = 9, and only these two
			// cells have one there: cell 8's +X and cell 9's -X.
			int shared = 0, span_ok = 1;
			for (int i = 0; i < g_qn; i++) {
				const Quad* q = &g_q[i];
				if (q->x_lo != 9 || q->x_hi != 9) continue;
				shared++;

				// The wall must run from the floor of the cells up to the TALLER of the two
				// surfaces. Surface height of level L is 8 + L/8, and a quad's top edge is
				// y_hi - drop/8, so the taller surface means drop == 8 - max(a, b).
				const int taller = a > b ? a : b;
				if (q->y_lo != 8 || q->y_hi != 9) span_ok = 0;
				if (q->drop_top != WATER_LEVEL_SOURCE - taller) span_ok = 0;
			}

			if (a == b) {
				if (shared != 0) { bad_equal++; if (first_a < 0) { first_a = a; first_b = b; } }
			} else {
				if (shared != 1) { bad_count++; if (first_a < 0) { first_a = a; first_b = b; } }
				else if (!span_ok) { bad_span++; if (first_a < 0) { first_a = a; first_b = b; } }
			}
		}
	}

	if (first_a >= 0) printf("  ...first offending pair: levels %d and %d\n", first_a, first_b);

	CHECK(bad_equal == 0,
	      "two cells at the SAME level cull each other, exactly as v1.7.1 did");
	CHECK(bad_count == 0,
	      "every unequal pair draws exactly one quad on the plane between them, never zero");
	CHECK(bad_span == 0,
	      "and that quad runs from the floor to the TALLER surface, so the step has no hole");
	CHECK(bad_shape == 0, "no quad in any of the 64 pairs has inconsistent corners");

	// One pair spelled out end to end, so the failure message names a height and not a total.
	msReset();
	msWater(8, 8, 8, 6);
	msWater(9, 8, 8, 3);
	runMesh();
	collectQuads();

	int wall_drop = -1, walls = 0;
	for (int i = 0; i < g_qn; i++)
		if (g_q[i].x_lo == 9 && g_q[i].x_hi == 9) { walls++; wall_drop = g_q[i].drop_top; }

	CHECK(walls == 1 && wall_drop == 2,
	      "level 6 beside level 3: one wall, topped at 6/8, hiding the 3/8 cell behind it");
}

static void testNoMergeAcrossLevels(void)
{
	puts("water mesh: a greedy run stops where the height changes");

	// A row along Z, because that is the axis a +Y face merges along (see the u-axis note in
	// mesher.c's planBuild). Levels 6,6,3,3: the two pairs may merge with each other and must
	// not merge across the step.
	msReset();
	for (int z = 6; z <= 7; z++) msWater(8, 8, z, 6);
	for (int z = 8; z <= 9; z++) msWater(8, 8, z, 3);
	runMesh();
	collectQuads();

	int tops = 0, top_w2 = 0, top_drops = 0;
	for (int i = 0; i < g_qn; i++) {
		const Quad* q = &g_q[i];
		if (q->y_lo != 9 || q->y_hi != 9) continue;   // the lids
		tops++;
		if (q->z_hi - q->z_lo == 2) top_w2++;
		if (q->drop_top == 2 || q->drop_top == 5) top_drops++;
	}

	CHECK(tops == 2, "four cells at two heights make two lids, not one and not four");
	CHECK(top_w2 == 2, "each lid is exactly two blocks wide: the run stopped at the step");
	CHECK(top_drops == 2, "and the two lids sit at 6/8 and 3/8, the heights they were given");

	// The same four cells at ONE level must still merge into one lid — otherwise the check
	// above would pass just as well against a mesher that had stopped merging altogether.
	msReset();
	for (int z = 6; z <= 9; z++) msWater(8, 8, z, 6);
	runMesh();
	collectQuads();

	int one_tops = 0, one_w4 = 0;
	for (int i = 0; i < g_qn; i++) {
		if (g_q[i].y_lo != 9 || g_q[i].y_hi != 9) continue;
		one_tops++;
		if (g_q[i].z_hi - g_q[i].z_lo == 4) one_w4++;
	}

	CHECK(one_tops == 1 && one_w4 == 1,
	      "control: four cells at the same level still merge into one four-wide lid");
}

// The anchor. A fixed staircase of levels, hashed, so any change to the emitted bytes shows
// up here as a one-line diff instead of as a screenshot argument.
//
// RE-PINNED, v1.8.2 task 13b: 0x6f5d4219 -> 0x11ff64ad. The mesher's vertex `v` byte changed
// UNITS — an atlas pixel row (tile * TILE_PX) became a slot-edge index (tile, or tile + 1),
// with the TILE_PX factor moved into both shaders' uvScale.y — so every `v` byte in every
// mesh holds a different number while no vertex moved and MeshVertex is still 8 bytes.
//
// The re-pin is only legitimate because the three checks around it did NOT move: faces stayed
// 51, all eight drop levels still appear, and every quad's corners are still consistent. The
// printf above line 521 reported `faces=51 verts=204` on the post-change run, matching the
// count this test derives by hand. A hash that moved while any of those moved with it would
// have been a geometry regression wearing a re-pin's clothes.
static void testPinnedGeometry(void)
{
	puts("water mesh: the pinned geometry of a fixed staircase");

	// Water and nothing else, so the face count is arrived at by hand rather than accepted
	// because the program printed it:
	//
	//   A row of 8 along +X at y = 8, z = 8, levels 8 down to 1 — the shape a real pour makes.
	//     8 lids, none merged (a lid merges along +Z and there is one cell per z, and every
	//       neighbour along x is a different height anyway)
	//     8 floors, none merged either. A floor's four corners all sit at py 0 and so carry no
	//       drop at all, but mergeCandidate refuses the run on the CELL's drop rather than on
	//       the face's, so a stack of differently-shortened cells does not pool its undersides.
	//       That is deliberately left alone: the rule is one line and always safe, water's
	//       underside is culled against the ground it sits on in every real world, and a
	//       per-face exception is a second way to be wrong about which faces carry a height.
	//    16 z-facing walls, none merged: adjacent cells differ in height
	//     9 x-facing walls: the two ends, plus ONE on each of the 7 planes between the cells,
	//       emitted by the taller side — 7 planes x 2 faces culled down to 7
	//   = 41
	//
	//   A two-cell fall at x = 12, z = 12, both level 7.
	//    12 faces less the 2 on the plane they share = 10
	//
	//   41 + 10 = 51.
	msReset();
	for (int lvl = WATER_LEVEL_SOURCE; lvl >= 1; lvl--)
		msWater(WATER_LEVEL_SOURCE - lvl, 8, 8, lvl);
	msWater(12, 8, 12, WATER_LEVEL_MAX);
	msWater(12, 9, 12, WATER_LEVEL_MAX);

	runMesh();
	collectQuads();

	printf("  ...faces=%u verts=%u hash=0x%08xu\n",
	       (unsigned)g_out.faces, (unsigned)g_out.vert_count, meshHash(&g_out));

	int seen_drop[WATER_LEVEL_SOURCE] = { 0 };
	for (uint32_t i = 0; i < g_out.vert_count; i++)
		seen_drop[meshNrmDrop(g_out.verts[i].nrm)] = 1;
	int distinct = 0;
	for (int d = 0; d < WATER_LEVEL_SOURCE; d++) distinct += seen_drop[d];

	CHECK(g_out.faces == 51, "the staircase is the 51 quads counted out above");
	CHECK(distinct == WATER_LEVEL_SOURCE, "all eight heights 0/8 .. 7/8 appear in it");
	CHECK(quadsConsistent(), "and every one of the 51 has consistent corners");
	CHECK(meshHash(&g_out) == 0x11ff64adu, "its bytes hash to the pinned value");
	CHECK(!g_out.overflow, "with no overflow");
}

// ── The simulation side: getting the levels into the band at all ─────────────────────

static World    g_world;
static WaterSim g_sim;

static void testFillScratch(void)
{
	puts("water mesh: waterFillScratch hands the mesher the levels the simulation holds");

	worldExit(&g_world);
	budgetReset();
	worldInit(&g_world);
	waterInit(&g_sim);
	worldSetEditHook(NULL, NULL);

	// A floor at y = 63 under a source at y = 64, settled. Chunk (0, 4, 0) covers y 64..79,
	// so the pour lands inside it and its levels are what the band must carry.
	for (int z = -13; z <= 13; z++)
		for (int x = -13; x <= 13; x++)
			(void)worldSet(&g_world, x, 63, z, (BlockId)BLOCK_STONE);
	(void)worldSet(&g_world, 0, 64, 0, (BlockId)BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 4000, NULL, NULL);

	CHECK(waterFlowCells(&g_sim) > 0, "the pour produced flow cells to look at");

	msReset();
	waterFillScratch(&g_sim, &g_ms, 0, 4, 0);

	CHECK(g_ms.water_any, "and the band says it holds something");

	// Cell for cell over the whole 18x18x18 window, including the border ring, against the
	// simulation's own accessor. The band must hold 0 for a source and for anything that is
	// not water, and the map's level for a flow cell.
	int mismatched = 0, flow_seen = 0;
	for (int sy = 0; sy < SCRATCH_DIM; sy++)
		for (int sz = 0; sz < SCRATCH_DIM; sz++)
			for (int sx = 0; sx < SCRATCH_DIM; sx++) {
				const int x = sx - 1;
				const int y = 4 * CHUNK_DIM + sy - 1;
				const int z = sz - 1;

				const uint8_t lvl  = waterLevelAt(&g_sim, &g_world, x, y, z);
				const uint8_t want = (lvl == 0 || lvl == WATER_LEVEL_SOURCE) ? 0 : lvl;
				const uint8_t got  = g_ms.water[scratchIndex(sx, sy, sz)];

				if (got != want) mismatched++;
				if (want) flow_seen++;
			}

	CHECK(mismatched == 0, "every cell of the 18-cube band matches waterLevelAt exactly");
	CHECK(flow_seen > 0, "control: the window really did contain flow cells to get wrong");

	// A window nowhere near the water leaves the band alone and says so, which is what keeps
	// dropBuild's single branch the whole cost for the rest of the world.
	msReset();
	waterFillScratch(&g_sim, &g_ms, 40, 4, 40);
	CHECK(!g_ms.water_any, "a chunk far from any flow cell reports an empty band");

	// And the whole point: mesh the real chunk and see a shortened surface come out.
	msReset();
	scratchFill(&g_ms, &g_world, 0, 4, 0);
	waterFillScratch(&g_sim, &g_ms, 0, 4, 0);
	runMesh();
	CHECK(maxDropAnywhere() > 0,
	      "a real settled pour meshes with real short surfaces in it, not a block of cubes");
}

static int g_level_hits;
static int g_level_bad;

static void onLevel(void* ud, int x, int y, int z, BlockId id)
{
	(void)ud;
	g_level_hits++;
	if (id != (BlockId)BLOCK_WATER) g_level_bad++;
	if (worldGet(&g_world, x, y, z) != (BlockId)BLOCK_WATER) g_level_bad++;
}

// Two sources twelve blocks apart, the second dropped in after the first has settled. That is
// world/water_test.c's testChangeHook scenario, and it is used here for the same reason it was
// chosen there: the overlap raises cells that are ALREADY water to a higher level, so the
// change is a level and not a block. A single pour is no use — every cell of it was air a
// moment before, so every change is a block change and goes out of on_change instead.
static int pourTwoSources(bool arm_hook)
{
	worldExit(&g_world);
	budgetReset();
	worldInit(&g_world);
	waterInit(&g_sim);
	worldSetEditHook(NULL, NULL);

	if (arm_hook) waterSetLevelHook(&g_sim, onLevel, NULL);
	g_level_hits = 0;
	g_level_bad  = 0;

	for (int z = -13; z <= 13; z++)
		for (int x = -13; x <= 13; x++)
			(void)worldSet(&g_world, x, 63, z, (BlockId)BLOCK_STONE);

	(void)worldSet(&g_world, 0, 64, 0, (BlockId)BLOCK_WATER);
	waterNotify(&g_sim, 0, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 4000, NULL, NULL);

	(void)worldSet(&g_world, 12, 64, 0, (BlockId)BLOCK_WATER);
	waterNotify(&g_sim, 12, 64, 0);
	(void)waterSettle(&g_sim, &g_world, 4000, NULL, NULL);

	return g_level_hits;
}

static void testLevelHook(void)
{
	puts("water mesh: the level hook is what asks for the remesh a level change now needs");

	worldExit(&g_world);
	budgetReset();
	worldInit(&g_world);
	waterInit(&g_sim);

	CHECK(g_sim.level_fn == NULL,
	      "waterInit leaves the hook unset, so every caller without one behaves as before");

	const int armed = pourTwoSources(true);
	printf("  ...armed run reported %d level-only changes\n", armed);

	CHECK(armed > 0, "a second source raises settled water, and the hook says so");
	CHECK(g_level_bad == 0, "every cell it names is water, in the hook's id and in the world");

	// The same script with nothing installed. Two arms rather than one, so "0" in the second
	// is a measurement of the hook and not of a scenario that never had anything to report.
	const int unarmed = pourTwoSources(false);
	CHECK(unarmed == 0, "control: the identical pour with no hook installed reports nothing");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== water mesh test ==");

	worldInit(&g_world);

	testSourceUnchanged();
	testLevelHeights();
	testFallingIsFull();
	testLevelBoundary();
	testNoMergeAcrossLevels();
	testPinnedGeometry();
	testFillScratch();
	testLevelHook();

	worldExit(&g_world);

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
