#include "world/world_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/atlas_uv.h"
#include "world/budget.h"
#include "world/dirtyq.h"
#include "world/handbuilt.h"
#include "world/jobq.h"
#include "world/mesher.h"
#include "world/noise.h"
#include "world/physics.h"
#include "world/rng.h"
#include "world/raycast.h"
#include "world/remesh.h"
#include "world/scratch.h"
#include "scene/render_dist.h"
#include "world/visgraph.h"
#include "world/world.h"
#include "world/worldgen.h"

// Big enough to matter on a 32 KB console stack, and only ever one of each.
static World       s_world;
static MeshScratch s_scratch;

static int  s_checks;
static int  s_fails;
static char s_first[96];

#define CHECK(cond) do {                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, #cond);  \
		}                                                                       \
	} while (0)

// CHECK for the inside of a long loop. A passing iteration costs nothing; only failures
// are counted and reported. Without this a 4,000-iteration range check would add 4,000 to
// the suite total and turn "2074 checks" into a number that says nothing about coverage.
#define CHECK_QUIET(cond) do {                                                  \
		if (!(cond)) {                                                          \
			s_checks++;                                                         \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, #cond);  \
		}                                                                       \
	} while (0)

static void testBlockRegistry(void)
{
	CHECK(!blockIsSolid(BLOCK_AIR));
	CHECK(blockIsAir(BLOCK_AIR));
	CHECK(blockIsSolid(BLOCK_GRASS));
	CHECK(blockIsSolid(BLOCK_STONE));

	// Face order is east, west, top, bottom, south, north.
	CHECK(blockFaceTex(BLOCK_GRASS, 2) == BTEX_GRASS_TOP);
	CHECK(blockFaceTex(BLOCK_GRASS, 3) == BTEX_DIRT);
	CHECK(blockFaceTex(BLOCK_GRASS, 0) == BTEX_GRASS_SIDE);
	CHECK(blockFaceTex(BLOCK_STONE, 4) == BTEX_STONE);

	// An unknown id must read back as air rather than walk off the table.
	CHECK(blockInfo(200)->solid == false);
	CHECK(strcmp(blockInfo(200)->name, "air") == 0);
}

static void testChunkIndex(void)
{
	static uint8_t seen[CHUNK_BLOCKS];
	memset(seen, 0, sizeof(seen));

	int duplicates = 0;
	int out_of_range = 0;
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				const int i = chunkIndex(x, y, z);
				if (i < 0 || i >= CHUNK_BLOCKS) { out_of_range++; continue; }
				if (seen[i]++) duplicates++;
			}

	CHECK(duplicates == 0);
	CHECK(out_of_range == 0);

	// x must be the contiguous axis — the scratch fill memcpys runs of it.
	CHECK(chunkIndex(1, 0, 0) - chunkIndex(0, 0, 0) == 1);
	CHECK(sizeof(Chunk) == CHUNK_BLOCKS);
}

static void testWorldAccess(void)
{
	worldInit(&s_world);

	CHECK(worldGet(&s_world, 0, 0, 0) == BLOCK_AIR);
	CHECK(worldGet(&s_world, 0, -1, 0) == WORLD_FLOOR_BLOCK);
	CHECK(worldGet(&s_world, 0, WORLD_HEIGHT, 0) == BLOCK_AIR);
	CHECK(worldSet(&s_world, 0, -1, 0, BLOCK_STONE) == false);
	CHECK(worldSet(&s_world, 0, WORLD_HEIGHT, 0, BLOCK_STONE) == false);

	// Storing air into nothing must not allocate.
	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_AIR) == true);
	CHECK(s_world.chunks == 0);

	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_GRASS) == true);
	CHECK(worldGet(&s_world, 4, 4, 4) == BLOCK_GRASS);
	CHECK(s_world.columns == 1);
	CHECK(s_world.chunks == 1);

	// Across a chunk boundary: 15 and 16 are different chunks, same column.
	CHECK(worldSet(&s_world, 16, 4, 4, BLOCK_SAND) == true);
	CHECK(worldGet(&s_world, 16, 4, 4) == BLOCK_SAND);
	CHECK(worldGet(&s_world, 15, 4, 4) == BLOCK_AIR);
	CHECK(s_world.columns == 2);

	// Up a column: same column, next chunk.
	CHECK(worldSet(&s_world, 4, 20, 4, BLOCK_DIRT) == true);
	CHECK(worldGet(&s_world, 4, 20, 4) == BLOCK_DIRT);
	CHECK(s_world.columns == 2);
	CHECK(s_world.chunks == 3);

	// Negative coordinates: block -1 belongs to chunk -1, local 15. Plain division
	// would round toward zero here and put it in chunk 0.
	CHECK(worldSet(&s_world, -1, 5, -1, BLOCK_STONE) == true);
	CHECK(worldGet(&s_world, -1, 5, -1) == BLOCK_STONE);
	CHECK(worldColumn(&s_world, -1, -1) != NULL);
	CHECK(worldChunk(&s_world, -1, 0, -1) != NULL);
	CHECK(worldGet(&s_world, -17, 5, -17) == BLOCK_AIR);

	// Out-of-range vertical chunk requests are refused, not clamped.
	CHECK(worldChunkCreate(&s_world, 0, COLUMN_CHUNKS, 0) == NULL);
	CHECK(worldChunk(&s_world, 0, COLUMN_CHUNKS, 0) == NULL);

	// Same coordinates twice must not double-allocate.
	const int before = s_world.chunks;
	CHECK(worldChunkCreate(&s_world, 0, 0, 0) != NULL);
	CHECK(s_world.chunks == before);

	worldExit(&s_world);
	CHECK(s_world.columns == 0);
	CHECK(s_world.chunks == 0);
}

// The reason the padded scratch exists: every one of the 26 neighbours has to
// arrive in the right border cell, including the eight corner diagonals that
// ambient occlusion reads.
static void testScratch26(void)
{
	worldInit(&s_world);

	// Centre chunk (1,1,1) so no neighbour lands outside the world.
	const int cx = 1, cy = 1, cz = 1;
	const int base = CHUNK_DIM;   // world coordinate of the centre chunk's origin

	// Interior first: a block inside the chunk must land at its local coordinate.
	CHECK(worldSet(&s_world, base + 3, base + 5, base + 7, BLOCK_GRASS) == true);
	scratchFill(&s_scratch, &s_world, cx, cy, cz);
	CHECK(scratchAt(&s_scratch, 3, 5, 7) == BLOCK_GRASS);
	CHECK(scratchAt(&s_scratch, 3, 5, 6) == BLOCK_AIR);

	// Unloaded neighbours read as air, not as garbage.
	CHECK(scratchAt(&s_scratch, -1, -1, -1) == BLOCK_AIR);
	CHECK(scratchAt(&s_scratch, 16, 16, 16) == BLOCK_AIR);

	int wrong = 0;
	int corners = 0;
	for (int dy = -1; dy <= 1; dy++)
		for (int dz = -1; dz <= 1; dz++)
			for (int dx = -1; dx <= 1; dx++) {
				if (!dx && !dy && !dz) continue;

				// The cell immediately outside the centre chunk in this direction:
				// -1 -> local -1, 0 -> local 0, +1 -> local 16.
				const int lx = (dx < 0) ? -1 : (dx > 0 ? CHUNK_DIM : 0);
				const int ly = (dy < 0) ? -1 : (dy > 0 ? CHUNK_DIM : 0);
				const int lz = (dz < 0) ? -1 : (dz > 0 ? CHUNK_DIM : 0);

				if (!worldSet(&s_world, base + lx, base + ly, base + lz, BLOCK_SAND)) {
					wrong++;
					continue;
				}

				scratchFill(&s_scratch, &s_world, cx, cy, cz);
				if (scratchAt(&s_scratch, lx, ly, lz) != BLOCK_SAND) wrong++;
				if (dx && dy && dz) corners++;

				worldSet(&s_world, base + lx, base + ly, base + lz, BLOCK_AIR);
			}

	CHECK(wrong == 0);
	CHECK(corners == 8);   // all eight diagonals were actually exercised

	worldExit(&s_world);
}

static void testScratchFloor(void)
{
	worldInit(&s_world);

	// The chunk sitting on the world floor: the row below it must read solid, or the
	// mesher would emit the underside of the world as a visible face.
	CHECK(worldSet(&s_world, 5, 0, 5, BLOCK_STONE) == true);
	scratchFill(&s_scratch, &s_world, 0, 0, 0);
	CHECK(scratchAt(&s_scratch, 5, -1, 5) == WORLD_FLOOR_BLOCK);
	CHECK(scratchAt(&s_scratch, 5, 0, 5) == BLOCK_STONE);
	CHECK(worldGet(&s_world, 5, -1, 5) == WORLD_FLOOR_BLOCK);   // and worldGet agrees

	// A chunk one up has an ordinary air border below it, not a floor.
	scratchFill(&s_scratch, &s_world, 0, 1, 0);
	CHECK(scratchAt(&s_scratch, 5, -1, 5) == BLOCK_AIR);

	worldExit(&s_world);
}

static void testBudget(void)
{
	budgetReset();

	CHECK(budgetUsed() == 0);
	CHECK(budgetCap() == WORLD_BUDGET_BYTES);
	CHECK(budgetClaim(1024) == true);
	CHECK(budgetUsed() == 1024);
	budgetRelease(1024);
	CHECK(budgetUsed() == 0);

	// Releasing more than was claimed must clamp, not wrap to 4 GB.
	budgetRelease(4096);
	CHECK(budgetUsed() == 0);

	// Over-budget is refused loudly and reserves nothing.
	const int refusals = budgetRefusals();
	CHECK(budgetClaim(WORLD_BUDGET_BYTES + 1) == false);
	CHECK(budgetRefusals() == refusals + 1);
	CHECK(budgetUsed() == 0);

	CHECK(budgetClaim(WORLD_BUDGET_BYTES) == true);
	CHECK(budgetClaim(1) == false);
	budgetRelease(WORLD_BUDGET_BYTES);
	CHECK(budgetPeak() == WORLD_BUDGET_BYTES);

	budgetReset();
}

// Fills a whole chunk with one block id, straight into its storage. Returns false if
// the chunk could not be allocated, which the caller must check — a silently missing
// chunk would make a mesher test pass for the wrong reason.
static bool fillChunk(int cx, int cy, int cz, BlockId id)
{
	Chunk* c = worldChunkCreate(&s_world, cx, cy, cz);
	if (!c) return false;
	memset(c->blocks, id, CHUNK_BLOCKS);
	return true;
}

// Step 7.5's split. The mesher now writes two runs into one index buffer — opaque faces
// first, then the ones the alpha-tested pass draws — and the renderer trusts the boundary
// absolutely: it draws [0, opaque_index_count) with one GPU state and the rest with another.
// A boundary that is off by one quad puts six leaf indices into the opaque pass, where they
// draw as solid green squares, or six stone indices into the alpha pass, where they draw
// fine and mislead every count that follows.
//
// None of that is visible on a screenshot of a forest, which is why it is checked here.
static void testMesherTransparentSplit(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	// The premise the split rests on. If either of these ever stops holding, every count
	// below is measuring something else.
	CHECK(blockIsSolid(BLOCK_LEAVES));
	CHECK(blockInfo(BLOCK_LEAVES)->transparent);
	CHECK(!blockInfo(BLOCK_STONE)->transparent);

	// --- All stone: everything opaque, the transparent run is empty.
	worldInit(&s_world);
	CHECK(fillChunk(1, 1, 1, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6 * CHUNK_DIM * CHUNK_DIM);
	CHECK(out.opaque_faces == out.faces);
	CHECK(out.opaque_index_count == out.index_count);

	// --- All leaves: everything deferred, the opaque run is empty. Same face count as the
	// stone chunk, because leaf-against-leaf is still culled — that is the cheap canopy,
	// and if `solid` were dropped this number would jump to 24,576 instead.
	worldExit(&s_world);
	worldInit(&s_world);
	CHECK(fillChunk(1, 1, 1, BLOCK_LEAVES));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6 * CHUNK_DIM * CHUNK_DIM);
	CHECK(out.opaque_faces == 0);
	CHECK(out.opaque_index_count == 0);

	// --- Mixed: one leaf block sitting alone above a floor of stone. The two runs must
	// account for every face between them, with nothing lost and nothing double-counted.
	worldExit(&s_world);
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z, BLOCK_STONE));
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 6, CHUNK_DIM + 4, BLOCK_LEAVES));

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);

	CHECK(out.opaque_faces + 6 == out.faces);                   // the lone leaf, six faces
	CHECK(out.opaque_index_count + 6 * 6 == out.index_count);
	CHECK(out.opaque_index_count == out.opaque_faces * 6);
	CHECK(out.opaque_faces > 0);                                // the stone floor is there

	// The runs must not overlap and must not leave a gap: every index in the buffer belongs
	// to exactly one of them, and the transparent one starts where the opaque one stops.
	CHECK(out.opaque_index_count <= out.index_count);

	// The deferred run really is the leaf. Its vertices are the last four the mesher wrote,
	// and they must carry the leaf tile rather than stone's.
	const AtlasRect leaf  = atlasRect(blockFaceTex(BLOCK_LEAVES, FACE_TOP));
	const AtlasRect stone = atlasRect(blockFaceTex(BLOCK_STONE, FACE_TOP));
	CHECK(leaf.u0 != stone.u0 || leaf.v0 != stone.v0);   // else the check below proves nothing

	bool all_leaf_tile = true;
	for (uint32_t i = out.opaque_index_count; i < out.index_count; i++) {
		const MeshVertex* v = &out.verts[out.indices[i]];
		const bool in_tile = (v->u >= leaf.u0 && v->u <= leaf.u1 &&
		                      v->v >= leaf.v0 && v->v <= leaf.v1);
		if (!in_tile) all_leaf_tile = false;
	}
	CHECK(all_leaf_tile);

	// And the opaque run must contain none of it.
	bool any_leaf_in_opaque = false;
	for (uint32_t i = 0; i < out.opaque_index_count; i++) {
		const MeshVertex* v = &out.verts[out.indices[i]];
		if (v->u >= leaf.u0 && v->u <= leaf.u1 && v->v >= leaf.v0 && v->v <= leaf.v1)
			any_leaf_in_opaque = true;
	}
	CHECK(!any_leaf_in_opaque);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// The mesher, step 3.1. The counts here are the whole point of the step: a solid
// chunk with solid neighbours must emit *nothing*, and a lone block must emit exactly
// six faces. Both are invisible on screen — a chunk meshed with its interior included
// looks identical and just runs slower — so they are checked here instead.
static void testMesher(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	worldInit(&s_world);

	// --- A solid chunk with solid neighbours on all 26 sides: zero faces.
	bool built = true;
	for (int dy = 0; dy <= 2; dy++)
		for (int dz = 0; dz <= 2; dz++)
			for (int dx = 0; dx <= 2; dx++)
				built = fillChunk(dx, dy, dz, BLOCK_STONE) && built;
	CHECK(built);

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 0);
	CHECK(out.vert_count == 0);
	CHECK(out.index_count == 0);
	CHECK(out.overflow == false);

	// --- The same chunk with air around it: the surface only, 6 x 16 x 16 faces.
	worldExit(&s_world);
	worldInit(&s_world);
	CHECK(fillChunk(1, 1, 1, BLOCK_STONE));

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6 * CHUNK_DIM * CHUNK_DIM);   // 1536, not 24576
	CHECK(out.vert_count == out.faces * 4);
	CHECK(out.index_count == out.faces * 6);
	CHECK(out.overflow == false);

	// Every vertex must sit inside the chunk's own 0..16 box, or the chunk will not
	// line up with its neighbour once a model matrix places it.
	int out_of_box = 0;
	for (uint32_t i = 0; i < out.vert_count; i++) {
		const MeshVertex* v = &out.verts[i];
		if (v->x < 0 || v->x > CHUNK_DIM) out_of_box++;
		if (v->y < 0 || v->y > CHUNK_DIM) out_of_box++;
		if (v->z < 0 || v->z > CHUNK_DIM) out_of_box++;
	}
	CHECK(out_of_box == 0);

	// --- The floor rule: the lowest chunk's bottom faces are culled, because below
	// the world reads as stone. Five sides, not six.
	worldExit(&s_world);
	worldInit(&s_world);
	CHECK(fillChunk(0, 0, 0, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 0, 0, 0);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 5 * CHUNK_DIM * CHUNK_DIM);   // 1280

	// --- One floating block: exactly six faces, four vertices each, and the right
	// tile on each face straight out of the registry.
	worldExit(&s_world);
	worldInit(&s_world);
	CHECK(worldSet(&s_world, 20, 40, 20, BLOCK_GRASS));
	scratchFill(&s_scratch, &s_world, 1, 2, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);
	CHECK(out.vert_count == 24);
	CHECK(out.index_count == 36);

	int per_face[BLOCK_FACES] = {0};
	int wrong_tile = 0;
	for (uint32_t i = 0; i < out.vert_count; i++) {
		const MeshVertex* v = &out.verts[i];
		if (v->nrm < BLOCK_FACES) per_face[v->nrm]++;
		const AtlasRect r = atlasRect(blockFaceTex(BLOCK_GRASS, v->nrm));
		if ((v->u != r.u0 && v->u != r.u1) || (v->v != r.v0 && v->v != r.v1)) wrong_tile++;
	}
	CHECK(wrong_tile == 0);
	int faces_with_four = 0;
	for (int f = 0; f < BLOCK_FACES; f++) if (per_face[f] == 4) faces_with_four++;
	CHECK(faces_with_four == BLOCK_FACES);

	// Grass is the block that proves per-face tiles work at all: three different
	// tiles on one cube. Top face vertices must carry the grass top tile.
	const AtlasRect top = atlasRect(BTEX_GRASS_TOP);
	int top_verts = 0;
	for (uint32_t i = 0; i < out.vert_count; i++)
		if (out.verts[i].nrm == 2 && (out.verts[i].u == top.u0 || out.verts[i].u == top.u1))
			top_verts++;
	CHECK(top_verts == 4);

	// --- Overflow: a buffer too small must stop, flag it, and write nothing past the
	// end. A mesher that overruns here corrupts the linear heap on the console.
	MeshOut small = out;
	small.vert_cap  = 6;    // room for one quad, not two
	small.index_cap = MESH_MAX_INDICES;
	meshChunk(&small, &s_scratch);
	CHECK(small.overflow == true);
	CHECK(small.vert_count == 4);
	CHECK(small.faces == 1);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// Baked AO, step 3.5. The rule is three neighbours per corner, and the two cases that
// matter are the ones no screenshot can measure: an unoccluded face must stay at 3
// everywhere, and a corner with both flanks solid must go to 0 rather than 1.
static void testMesherAO(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	// --- A single floating block is occluded by nothing: every vertex at 3.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, 20, 40, 20, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 2, 1);
	meshChunk(&out, &s_scratch);

	int not_three = 0;
	for (uint32_t i = 0; i < out.vert_count; i++)
		if (out.verts[i].ao != 3) not_three++;
	CHECK(not_three == 0);

	// --- Occlude the top face. A neighbour at the *same* height cannot darken a top
	// face — every block AO reads for a top face sits in the layer above it — so the
	// occluder goes east and one up, at local (5,9,4). The block's own top face is the
	// only nrm==2 quad at y == 9, which is how it is picked out below; the occluder's
	// own top face is at y == 10.
	CHECK(worldSet(&s_world, 21, 41, 20, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 2, 1);
	meshChunk(&out, &s_scratch);

	int top_at_2 = 0, top_at_3 = 0, darker_than_2 = 0;
	for (uint32_t i = 0; i < out.vert_count; i++) {
		const MeshVertex* v = &out.verts[i];
		if (v->nrm != 2 || v->y != 9) continue;      // the first block's own top face
		if (v->ao == 2) top_at_2++;
		if (v->ao == 3) top_at_3++;
		if (v->ao < 2)  darker_than_2++;
	}
	CHECK(top_at_2 == 2);           // the two corners on the occluder's side
	CHECK(top_at_3 == 2);           // the far pair are untouched
	CHECK(darker_than_2 == 0);      // one flank is not a crevice

	// --- An inside corner: a second occluder to the south, also one up, so one corner
	// of the top face now has both flanks solid. That corner must read 0, not the 1 the
	// naive `3 - (s1 + s2 + c)` formula would give.
	CHECK(worldSet(&s_world, 20, 41, 21, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 2, 1);
	meshChunk(&out, &s_scratch);

	int zero_ao = 0, one_ao = 0;
	for (uint32_t i = 0; i < out.vert_count; i++) {
		const MeshVertex* v = &out.verts[i];
		if (v->nrm != 2 || v->y != 9) continue;
		if (v->ao == 0) zero_ao++;
		if (v->ao == 1) one_ao++;
	}
	CHECK(zero_ao == 1);
	CHECK(one_ao == 0);

	// --- AO must not change how many faces exist, only how dark they are. The three
	// blocks only touch diagonally, so nothing is hidden and all 18 faces survive.
	CHECK(out.faces == 3 * 6);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// Which chunks a block edit dirties, step 3.6.
static void testRemeshList(void)
{
	ChunkCoord list[REMESH_MAX];

	// Middle of a chunk: itself only.
	CHECK(remeshList(24, 40, 24, list) == 1);
	CHECK(list[0].cx == 1 && list[0].cy == 2 && list[0].cz == 1);

	// One face border: two chunks.
	CHECK(remeshList(16, 40, 24, list) == 2);
	// An edge: four.
	CHECK(remeshList(16, 40, 32, list) == 4);
	// A corner: all eight, diagonals included — this is the one AO needs.
	CHECK(remeshList(16, 32, 16, list) == 8);

	int diagonal = 0;
	for (int i = 0; i < 8; i++)
		if (list[i].cx == 0 && list[i].cy == 1 && list[i].cz == 0) diagonal++;
	CHECK(diagonal == 1);

	// No duplicates, or a chunk gets meshed twice for nothing.
	int duplicates = 0;
	for (int i = 0; i < 8; i++)
		for (int j = i + 1; j < 8; j++)
			if (list[i].cx == list[j].cx && list[i].cy == list[j].cy &&
			    list[i].cz == list[j].cz) duplicates++;
	CHECK(duplicates == 0);

	// At the world floor the chunk below does not exist, so a corner edit there
	// touches four, not eight.
	CHECK(remeshList(16, 0, 16, list) == 4);
	CHECK(remeshList(16, -1, 16, list) == 0);
	CHECK(remeshList(16, WORLD_HEIGHT, 16, list) == 0);
}

// A mesh's identity, cheap enough to run a few hundred times: every byte the GPU would
// read. Two meshes with the same checksum have the same geometry, tiles and AO.
static uint32_t meshChecksum(const MeshOut* o)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < o->vert_count; i++) {
		const MeshVertex* v = &o->verts[i];
		const uint8_t bytes[7] = { (uint8_t)v->x, (uint8_t)v->y, (uint8_t)v->z,
		                           v->u, v->v, v->nrm, v->ao };
		for (int b = 0; b < 7; b++) { h ^= bytes[b]; h *= 16777619u; }
	}
	return h ^ o->index_count;
}

// Incremental remeshing, step 3.6: after a run of random edits, remeshing only what
// remeshList() nominates must leave every chunk byte-identical to remeshing the lot.
// A missed neighbour shows up here as a mismatch — on screen it would be a seam or a
// band of shadow that stops at a chunk edge, which is exactly what this replaces.
#define EDIT_SPAN_X   2
#define EDIT_SPAN_Z   2
#define EDIT_SPAN_Y   1
#define EDIT_COUNT    32

static void testIncrementalRemesh(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	static uint32_t incremental[EDIT_SPAN_X][EDIT_SPAN_Y][EDIT_SPAN_Z];
	static uint32_t fresh[EDIT_SPAN_X][EDIT_SPAN_Y][EDIT_SPAN_Z];
	static uint32_t before[EDIT_SPAN_X][EDIT_SPAN_Y][EDIT_SPAN_Z];

	worldInit(&s_world);
	CHECK(handbuiltFill(&s_world));

	for (int cx = 0; cx < EDIT_SPAN_X; cx++)
		for (int cy = 0; cy < EDIT_SPAN_Y; cy++)
			for (int cz = 0; cz < EDIT_SPAN_Z; cz++) {
				scratchFill(&s_scratch, &s_world, cx, cy, cz);
				meshChunk(&out, &s_scratch);
				incremental[cx][cy][cz] = meshChecksum(&out);
				before[cx][cy][cz]      = incremental[cx][cy][cz];
			}

	// Deterministic edits, so a failure is reproducible. Chunk borders are where the
	// bug lives, so every fourth edit is aimed straight at x = 16 or z = 16.
	uint32_t rng = 0x1D3B5C7u;
	for (int e = 0; e < EDIT_COUNT; e++) {
		rng = rng * 1664525u + 1013904223u;
		int x = (int)((rng >> 8) % (EDIT_SPAN_X * CHUNK_DIM));
		int z = (int)((rng >> 16) % (EDIT_SPAN_Z * CHUNK_DIM));
		int y = (int)((rng >> 4) % (EDIT_SPAN_Y * CHUNK_DIM));
		const BlockId id = ((rng >> 24) & 1) ? BLOCK_AIR : BLOCK_SAND;

		if ((e & 3) == 0) { x = CHUNK_DIM - ((e >> 2) & 1); z = CHUNK_DIM; }

		CHECK(worldSet(&s_world, x, y, z, id));

		ChunkCoord dirty[REMESH_MAX];
		const int n = remeshList(x, y, z, dirty);
		for (int i = 0; i < n; i++) {
			if (dirty[i].cx < 0 || dirty[i].cx >= EDIT_SPAN_X) continue;
			if (dirty[i].cy < 0 || dirty[i].cy >= EDIT_SPAN_Y) continue;
			if (dirty[i].cz < 0 || dirty[i].cz >= EDIT_SPAN_Z) continue;

			scratchFill(&s_scratch, &s_world, dirty[i].cx, dirty[i].cy, dirty[i].cz);
			meshChunk(&out, &s_scratch);
			incremental[dirty[i].cx][dirty[i].cy][dirty[i].cz] = meshChecksum(&out);
		}
	}

	int mismatches = 0;
	for (int cx = 0; cx < EDIT_SPAN_X; cx++)
		for (int cy = 0; cy < EDIT_SPAN_Y; cy++)
			for (int cz = 0; cz < EDIT_SPAN_Z; cz++) {
				scratchFill(&s_scratch, &s_world, cx, cy, cz);
				meshChunk(&out, &s_scratch);
				fresh[cx][cy][cz] = meshChecksum(&out);
				if (fresh[cx][cy][cz] != incremental[cx][cy][cz]) mismatches++;
			}
	CHECK(mismatches == 0);

	// And the edits must actually have changed the geometry, or the comparison above
	// is two copies of an untouched world agreeing with each other. Every chunk in the
	// span is expected to move: the border edits alone reach all four.
	int changed = 0;
	for (int cx = 0; cx < EDIT_SPAN_X; cx++)
		for (int cy = 0; cy < EDIT_SPAN_Y; cy++)
			for (int cz = 0; cz < EDIT_SPAN_Z; cz++)
				if (fresh[cx][cy][cz] != before[cx][cy][cz]) changed++;
	CHECK(changed == EDIT_SPAN_X * EDIT_SPAN_Y * EDIT_SPAN_Z);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// 4.1 — the DDA raycast. Every case here is one that a naive implementation gets
// wrong, which is the only reason to write a test rather than aim at a block and look.
static void testRaycast(void)
{
	worldInit(&s_world);

	// A single block at (4,4,4), nothing else. Rays are fired from cell centres so
	// there is no argument about which cell the origin is in.
	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_STONE));

	// Straight down the +X axis from x=0.5: enters through the block's WEST face, and
	// the place position is the empty cell it came from.
	RayHit h = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit);
	CHECK(h.x == 4 && h.y == 4 && h.z == 4);
	CHECK(h.face == FACE_WEST);
	CHECK(h.px == 3 && h.py == 4 && h.pz == 4);

	// The opposite direction must give the EAST face, not the same one. Getting the
	// sign of the step backwards is the classic bug and it is invisible until you
	// try to place a block.
	h = worldRaycast(&s_world, 8.5f, 4.5f, 4.5f, -1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit && h.face == FACE_EAST);
	CHECK(h.px == 5 && h.py == 4 && h.pz == 4);

	// From above: TOP face, and the place cell is one higher.
	h = worldRaycast(&s_world, 4.5f, 9.0f, 4.5f, 0.0f, -1.0f, 0.0f, 16.0f);
	CHECK(h.hit && h.face == FACE_TOP);
	CHECK(h.py == 5);

	// From below: BOTTOM face. The world floor is solid, so this ray has to start
	// above it and still reach the block.
	h = worldRaycast(&s_world, 4.5f, 1.5f, 4.5f, 0.0f, 1.0f, 0.0f, 16.0f);
	CHECK(h.hit && h.face == FACE_BOTTOM && h.y == 4);

	// Both Z faces, for completeness — all six get exercised or the face table is
	// only half tested.
	h = worldRaycast(&s_world, 4.5f, 4.5f, 0.5f, 0.0f, 0.0f, 1.0f, 16.0f);
	CHECK(h.hit && h.face == FACE_NORTH);
	h = worldRaycast(&s_world, 4.5f, 4.5f, 8.5f, 0.0f, 0.0f, -1.0f, 16.0f);
	CHECK(h.hit && h.face == FACE_SOUTH);

	// Distance is in blocks and measured to the surface, not to the centre.
	h = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.distance > 3.4f && h.distance < 3.6f);

	// Reach is respected: the same ray with 2 blocks of reach must miss.
	h = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 1.0f, 0.0f, 0.0f, 2.0f);
	CHECK(!h.hit);

	// A miss down an empty row returns hit == false, not a garbage cell.
	h = worldRaycast(&s_world, 0.5f, 40.5f, 40.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(!h.hit);

	// A diagonal ray must not tunnel through the corner between two blocks. This is
	// the case a naive "step along the vector" raycast fails: it samples past the
	// shared edge and hits neither.
	CHECK(worldSet(&s_world, 6, 4, 4, BLOCK_STONE));
	CHECK(worldSet(&s_world, 5, 4, 5, BLOCK_STONE));
	h = worldRaycast(&s_world, 4.5f, 4.5f, 6.5f, 1.0f, 0.0f, -1.0f, 16.0f);
	CHECK(h.hit);

	// The direction is normalised internally, so a long vector must not change the
	// answer — only the distance units.
	const RayHit unit  = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	const RayHit scale = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 9.0f, 0.0f, 0.0f, 16.0f);
	CHECK(scale.hit == unit.hit && scale.x == unit.x && scale.face == unit.face);
	CHECK(scale.distance > unit.distance - 0.01f && scale.distance < unit.distance + 0.01f);

	// A zero-length direction cannot hit anything, and must not divide by zero.
	h = worldRaycast(&s_world, 0.5f, 4.5f, 4.5f, 0.0f, 0.0f, 0.0f, 16.0f);
	CHECK(!h.hit);

	// Negative coordinates: chunk lookup uses an arithmetic shift, and so must the
	// ray's cell arithmetic. Truncation towards zero puts the ray in the wrong cell
	// for exactly half the world.
	CHECK(worldSet(&s_world, -4, 4, -4, BLOCK_STONE));
	h = worldRaycast(&s_world, -8.5f, 4.5f, -3.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit && h.x == -4 && h.y == 4 && h.z == -4 && h.face == FACE_WEST);
	CHECK(h.px == -5);

	// Starting inside a solid block is a real situation (the camera clipping into
	// terrain), and it reports the block with no face rather than lying about one.
	h = worldRaycast(&s_world, 4.5f, 4.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit && h.x == 4 && h.y == 4 && h.z == 4);
	CHECK(h.face == RAY_FACE_NONE);

	// Straight up out of the world: the ceiling is air, so nothing is hit and the
	// loop has to terminate on reach rather than run off the top.
	h = worldRaycast(&s_world, 4.5f, (float)WORLD_HEIGHT - 2.5f, 4.5f,
	                 0.0f, 1.0f, 0.0f, 32.0f);
	CHECK(!h.hit);

	// Straight down out of the world: below the floor is solid, so a ray aimed at it
	// must hit rather than fall through forever. The hit is at y == -1, not y == 0 —
	// worldGet reports WORLD_FLOOR_BLOCK only for y < 0, and nothing was ever placed in
	// this column, so y == 0 is ordinary air. Breaking the floor cell is then refused by
	// worldSet, which is correct, and placing against it lands at y == 0, which is legal.
	h = worldRaycast(&s_world, 40.5f, 1.5f, 40.5f, 0.0f, -1.0f, 0.0f, 8.0f);
	CHECK(h.hit && h.y == -1);
	CHECK(h.face == FACE_TOP && h.py == 0);

	worldExit(&s_world);
}

// 4.4 — the player box against the blocks. Each group resets the world, matching the
// style testMesher() uses, so a stray block from one case cannot leak into the next.
static void testPhysics(void)
{
	// --- Falling onto a floor, landing exactly on it, and not sinking in on the
	// ticks after. The exact-integer landing y is the whole point of resolveY's
	// analytic snap rather than an iterative one: a bisected stop would leave a
	// tiny residual gap or overlap instead of y == 11.0f on the nose.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));   // floor top at y=11

		Body body;
		bodyInit(&body, 5.0f, 20.0f, 5.0f);
		CHECK(body.on_ground == false);   // nothing under a freshly dropped body

		bool landed = false;
		for (int i = 0; i < 400 && !landed; i++) {
			bodyStep(&body, &s_world, 1.0f / 60.0f);
			if (body.on_ground) landed = true;
		}
		CHECK(landed);
		CHECK(body.y == 11.0f);          // exactly on the surface, not into it
		CHECK(body.vy == 0.0f);

		// A few more ticks standing still must not drift the feet at all.
		for (int i = 0; i < 10; i++) bodyStep(&body, &s_world, 1.0f / 60.0f);
		CHECK(body.y == 11.0f);
		CHECK(body.on_ground == true);

		worldExit(&s_world);
	}

	// --- Jumping: rises immediately, leaves on_ground, then comes back down onto
	// the same floor. This is the on_ground transition true -> false -> true.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;   // resting on the floor already, as if it had landed

		body.vy = PLAYER_JUMP_SPEED;
		bodyStep(&body, &s_world, 1.0f / 60.0f);
		CHECK(body.on_ground == false);   // left the ground the instant it jumped
		CHECK(body.y > 11.0f);            // and actually rose

		float peak = body.y;
		bool back = false;
		for (int i = 0; i < 400 && !back; i++) {
			bodyStep(&body, &s_world, 1.0f / 60.0f);
			if (body.y > peak) peak = body.y;
			if (body.on_ground) back = true;
		}
		CHECK(back);
		CHECK(body.y == 11.0f);       // landed back on the same surface, exactly
		CHECK(peak > 11.0f);          // and it did actually arc, not just twitch

		worldExit(&s_world);
	}

	// --- Airborne body walking into a one-block wall: it must stop, and the step-up
	// must NOT fire, because on_ground is false the whole time. This is the case that
	// is easy to get backwards: a step-up bug that ignores on_ground would let a body
	// flying past at head height "step" onto a wall it never touched feet-first.
	{
		worldInit(&s_world);
		CHECK(worldSet(&s_world, 8, 5, 5, BLOCK_STONE));   // a single floating block

		Body body;
		bodyInit(&body, 5.0f, 5.0f, 5.0f);   // on_ground is false from bodyInit
		CHECK(body.on_ground == false);

		int blocked = bodyMove(&body, &s_world, 3.0f, 0.0f, 0.0f);
		CHECK(blocked & BLOCKED_X);
		CHECK(body.x > 7.69f && body.x < 7.71f);   // flush against the wall: 8 - 0.3
		CHECK(body.y == 5.0f);                     // never lifted
		CHECK(body.on_ground == false);            // step-up never touched this

		worldExit(&s_world);
	}

	// --- The same wall, at a delta far bigger than one block, must still stop at the
	// wall face rather than skip clean through it. Without substep subdivision a
	// single 200-block jump would test only the start (clear) and the end (also clear,
	// on the far side of a one-block-thick wall) and never notice the wall between.
	{
		worldInit(&s_world);
		CHECK(worldSet(&s_world, 8, 5, 5, BLOCK_STONE));

		Body body;
		bodyInit(&body, 5.0f, 5.0f, 5.0f);

		int blocked = bodyMove(&body, &s_world, 200.0f, 0.0f, 0.0f);
		CHECK(blocked & BLOCKED_X);
		CHECK(body.x > 7.69f && body.x < 7.71f);   // stopped at the same face as above,
		                                           // not somewhere past x=9

		worldExit(&s_world);
	}

	// --- Stepping up a single block while grounded: the obstruction is exactly one
	// block on top of the same floor the body is already walking on, with clear air
	// above it, so the headroom probe finds nothing and the whole horizontal move
	// completes rather than stopping.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));   // floor, top at y=11
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));           // one block, top at y=12

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;

		int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);
		CHECK(!(blocked & BLOCKED_X));   // completed, not stopped
		CHECK(body.x > 8.9f);            // actually made it past the step's column
		CHECK(body.y == 12.0f);          // standing on top of it, exactly
		CHECK(body.on_ground == true);   // still grounded, just higher up

		worldExit(&s_world);
	}

	// --- A two-block wall on the same kind of floor must refuse the climb. The
	// headroom probe this time finds the second block still occupying the space the
	// body would rise into, so the step is refused and the ordinary blocked-and-
	// stopped result stands. That probe is what enforces "single block only", not a
	// numeric height comparison.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 12, 5, BLOCK_STONE));   // second block, top at y=13

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;

		int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);
		CHECK(blocked & BLOCKED_X);
		CHECK(body.x > 7.69f && body.x < 7.71f);   // stopped flush against it
		CHECK(body.y == 11.0f);                    // never rose at all
		CHECK(body.on_ground == true);             // still on the original floor

		worldExit(&s_world);
	}

	// --- A one-block-wide gap in the floor is not a wall: crossing the column it
	// occupies must not produce any horizontal block. Whether the body falls into the
	// gap depends on where its 0.6-wide box sits over it, which is a placement
	// question, not a collision-code one — this only checks that missing floor is
	// never mistaken for a solid obstruction sideways.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				if (x != 6)   // x == 6 is the gap: no floor block placed there
					CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));

		Body body;
		bodyInit(&body, 3.0f, 11.0f, 5.0f);
		body.on_ground = true;

		int blocked = bodyMove(&body, &s_world, 6.0f, 0.0f, 0.0f);
		CHECK(!(blocked & BLOCKED_X));   // the gap never blocked the crossing
		CHECK(body.x > 8.9f);            // and the body actually reached the far side

		worldExit(&s_world);
	}

	// --- The plan's second stated Phase 4.4 criterion, literally: "cannot fall
	// through the floor after 5 minutes". Five minutes at 60 Hz is 18,000 ticks, and
	// the failure it is looking for is drift — a resting body that loses a fraction of
	// a block per tick sinks out of the world long before the player notices, and no
	// short test can see it. Exact equality is the assertion on purpose: a resting
	// body must not move by even one bit of float.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);

		for (int i = 0; i < 18000; i++)
			bodyStep(&body, &s_world, 1.0f / 60.0f);

		CHECK(body.y == 11.0f);
		CHECK(body.on_ground == true);
		CHECK(worldGet(&s_world, 5, 10, 5) == BLOCK_STONE);   // the floor is still there

		worldExit(&s_world);
	}

	// --- No more than one block of step-up per substep, however many axes are blocked.
	// X and Z each get their own step-up attempt, and succeeding at one does not clear
	// on_ground, so a diagonal move used to be able to fire both and rise two blocks out
	// of a single substep. That lets the body climb a wall the two-block-wall test above
	// proves is unclimbable, just by brushing a one-block step beside it.
	//
	// The configuration matters and was found by probing, not by reasoning: a symmetric
	// one-block corner does NOT trigger it (the X rise lifts the body clear of the Z
	// block, so Z is no longer blocked when it is resolved), and a symmetric two-block
	// corner does not either (both headroom probes refuse). The case that breaks is
	// asymmetric — a one-block step on the axis resolved first, a two-block wall on the
	// second — measured at rise=2.0 before this was fixed.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));   // floor top at y=11

		CHECK(worldSet(&s_world, 6, 11, 5, BLOCK_STONE));   // +x: one block, climbable
		CHECK(worldSet(&s_world, 5, 11, 6, BLOCK_STONE));   // +z: two blocks, must not be
		CHECK(worldSet(&s_world, 5, 12, 6, BLOCK_STONE));

		Body body;
		bodyInit(&body, 5.5f, 11.0f, 5.5f);
		body.on_ground = true;

		// 0.3 on each axis is under MAX_SUBSTEP, so this is one pass through the
		// resolve/step-up pair rather than a sequence of them.
		const int blocked = bodyMove(&body, &s_world, 0.3f, 0.0f, 0.3f);

		CHECK(body.y == 12.0f);          // the climbable step only: exactly one block
		CHECK(blocked & BLOCKED_Z);      // and the two-block wall still stopped it
		CHECK(body.on_ground == true);

		worldExit(&s_world);
	}

	// --- Velocity is zeroed on a blocked axis, and only on that axis. This was an
	// unstated judgement call inside bodyMove; pinning it means changing it later has
	// to be deliberate. Walking into a two-block wall must kill vx and leave vz alone.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 12, 5, BLOCK_STONE));   // two tall: unclimbable

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;
		body.vx = PLAYER_WALK_SPEED;
		body.vz = 1.25f;

		const int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);

		CHECK(blocked & BLOCKED_X);
		CHECK(body.vx == 0.0f);      // the blocked axis stops
		CHECK(body.vz == 1.25f);     // the free axis is untouched

		worldExit(&s_world);
	}

	// --- A step-up that succeeds must NOT zero the velocity it succeeded with: it is a
	// completed move, not a stop. Otherwise stairs stutter to a halt on every tread.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));   // one block: climbable

		// x=7.6 puts the box's right face at 7.9, so a 0.3 move actually reaches the
		// block at x=8. Starting at 7.0 does not touch it at all and the test passes
		// vacuously — which is how it was written first.
		Body body;
		bodyInit(&body, 7.6f, 11.0f, 5.0f);
		body.on_ground = true;
		body.vx = PLAYER_WALK_SPEED;

		bodyMove(&body, &s_world, 0.3f, 0.0f, 0.0f);

		CHECK(body.y == 12.0f);                     // climbed it
		CHECK(body.vx == PLAYER_WALK_SPEED);        // and kept its speed

		worldExit(&s_world);
	}
}

// blockFaceTex()'s documented fallback: an out-of-range face index returns the
// block's first tile rather than reading off the end of the tex[] array.
static void testBlockFaceTexFallback(void)
{
	CHECK(blockFaceTex(BLOCK_GRASS, -1) == blockFaceTex(BLOCK_GRASS, 0));
	CHECK(blockFaceTex(BLOCK_GRASS, BLOCK_FACES) == blockFaceTex(BLOCK_GRASS, 0));
	CHECK(blockFaceTex(BLOCK_GRASS, 200) == blockFaceTex(BLOCK_GRASS, 0));

	// Grass specifically: its first tile (east, the side texture) differs from its
	// top and bottom tiles, so a fallback that silently returned some other tile
	// index would still pass a test written against a block with only one tile.
	CHECK(blockFaceTex(BLOCK_GRASS, -1) == BTEX_GRASS_SIDE);
}

// worldBytes(): the number the bottom-screen budget report is built from. Nothing
// asserted it before this, so a formula bug here would silently misreport memory
// while every other test stayed green.
static void testWorldBytes(void)
{
	worldInit(&s_world);

	CHECK(worldBytes(&s_world) == 0);          // nothing loaded yet
	CHECK(worldBytes(&s_world) == budgetUsed());

	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_STONE));   // allocates a column and a chunk
	CHECK(worldBytes(&s_world) > 0);
	CHECK(worldBytes(&s_world) == budgetUsed());

	// A second chunk in the same column: bytes must grow again, by a chunk's worth,
	// not a column's worth -- the column already exists.
	const size_t before = worldBytes(&s_world);
	CHECK(worldSet(&s_world, 4, 20, 4, BLOCK_STONE));
	CHECK(worldBytes(&s_world) > before);
	CHECK(worldBytes(&s_world) == budgetUsed());

	worldExit(&s_world);
	CHECK(worldBytes(&s_world) == 0);
}

// worldColumnCreate()'s "table full" refusal, world.h's other documented NULL path
// besides the budget one. WORLD_MAP_SLOTS is 1024 and slotFor() probes every slot
// before giving up, so the only honest way to reach this branch is to actually fill
// all 1024 of them.
static void testColumnTableFull(void)
{
	worldInit(&s_world);

	int created = 0;
	for (int i = 0; i < WORLD_MAP_SLOTS; i++)
		if (worldColumnCreate(&s_world, i, 0)) created++;
	CHECK(created == WORLD_MAP_SLOTS);
	CHECK(s_world.columns == WORLD_MAP_SLOTS);

	// One more, guaranteed distinct from all 1024 already in the table: a clean
	// NULL, not a crash.
	CHECK(worldColumnCreate(&s_world, WORLD_MAP_SLOTS, 0) == NULL);
	CHECK(s_world.columns == WORLD_MAP_SLOTS);   // the refusal did not sneak one in

	// The 1024 columns already there must still all be exactly what was put in --
	// a refusal that corrupted a slot on its way out would show up as a lookup
	// miss here.
	int missing = 0;
	for (int i = 0; i < WORLD_MAP_SLOTS; i++)
		if (!worldColumn(&s_world, i, 0)) missing++;
	CHECK(missing == 0);

	worldExit(&s_world);
}

// Unloading a column (step 6.1). The interesting half is not the free() -- it is that
// this table is open-addressed with linear probing and NO tombstones, so emptying a slot
// in the middle of a probe chain cuts every column behind it out of the world while its
// memory is still claimed. That failure is invisible from the outside: the column count
// is right, the budget is right, and a lookup just says "not loaded", which the streaming
// ring answers by generating the column again on top of the one that is still there.
static void testColumnRemove(void)
{
	worldInit(&s_world);
	budgetReset();

	// The plain case, with the accounting checked on both sides.
	CHECK(worldChunkCreate(&s_world, 3, 0, 4) != NULL);
	CHECK(worldChunkCreate(&s_world, 3, 5, 4) != NULL);
	CHECK(s_world.columns == 1 && s_world.chunks == 2);
	const size_t used_one = budgetUsed();
	CHECK(used_one == sizeof(Column) + 2 * sizeof(Chunk));

	CHECK(worldColumnRemove(&s_world, 3, 4));
	CHECK(worldColumn(&s_world, 3, 4) == NULL);
	CHECK(worldChunk(&s_world, 3, 0, 4) == NULL);
	CHECK(s_world.columns == 0 && s_world.chunks == 0);
	CHECK(budgetUsed() == 0);                 // every byte given back, not just the column

	// Removing what is not there is a no-op that reports it, because the ring asks for
	// columns it may already have dropped.
	CHECK(!worldColumnRemove(&s_world, 3, 4));
	CHECK(!worldColumnRemove(&s_world, -99, -99));
	CHECK(s_world.columns == 0);
	CHECK(budgetUsed() == 0);

	// The probe-chain case. 700 columns in a 1024-slot table is a load factor of 0.68,
	// which guarantees long chains; removing every other one and then looking up all 350
	// survivors is what a NULL-the-slot-and-walk-away deletion cannot survive.
	worldExit(&s_world);
	worldInit(&s_world);
	budgetReset();

	int made = 0;
	for (int i = 0; i < 700; i++)
		if (worldColumnCreate(&s_world, i, -i)) made++;
	CHECK(made == 700);

	int removed = 0;
	for (int i = 0; i < 700; i += 2)
		if (worldColumnRemove(&s_world, i, -i)) removed++;
	CHECK(removed == 350);
	CHECK(s_world.columns == 350);

	int lost = 0, ghost = 0, wrong = 0;
	for (int i = 0; i < 700; i++) {
		const Column* c = worldColumn(&s_world, i, -i);
		if (i & 1) {
			if (!c) lost++;
			else if (c->cx != i || c->cz != -i) wrong++;
		} else if (c) {
			ghost++;
		}
	}
	CHECK(lost == 0);      // a survivor stranded behind the hole
	CHECK(ghost == 0);     // a removed column still answering
	CHECK(wrong == 0);     // a slot holding the wrong column after the shuffle

	// And the table is still usable afterwards: the 350 holes take 350 new columns.
	int refilled = 0;
	for (int i = 0; i < 700; i += 2)
		if (worldColumnCreate(&s_world, i, -i)) refilled++;
	CHECK(refilled == 350);
	CHECK(s_world.columns == 700);

	worldExit(&s_world);
	CHECK(budgetUsed() == 0);
	budgetReset();
}

// budgetClaim() refusing in isolation is tested in testBudget(); this is the path
// that actually matters in play -- a real worldSet running into an exhausted budget
// after other allocations already succeeded, and leaving the world exactly as it
// was rather than half-updated.
static void testWorldSetBudgetExhausted(void)
{
	worldInit(&s_world);

	// One real block first, so there is a column and a chunk already on the books
	// to check for damage afterwards.
	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_STONE));
	const int    columns_before = s_world.columns;
	const int    chunks_before  = s_world.chunks;
	const size_t bytes_before   = worldBytes(&s_world);

	// Consume the rest of the budget directly, leaving less than one chunk's worth.
	// The column for the next write already exists, so this reaches worldChunkCreate's
	// own budgetClaim refusal specifically, not the column one testColumnTableFull and
	// testBudget already cover.
	const size_t remaining = budgetCap() - budgetUsed();
	CHECK(budgetClaim(remaining - 1) == true);

	// Same column (cx=0, cz=0) as the block above; a chunk one up that does not
	// exist yet.
	CHECK(worldSet(&s_world, 4, 20, 4, BLOCK_STONE) == false);

	// Nothing moved: no new chunk, no budget quietly taken, and the block placed
	// before the exhaustion is still exactly what it was.
	CHECK(s_world.columns == columns_before);
	CHECK(s_world.chunks == chunks_before);
	CHECK(worldBytes(&s_world) == bytes_before);
	CHECK(worldChunk(&s_world, 0, 1, 0) == NULL);   // the refused chunk was not half-made
	CHECK(worldGet(&s_world, 4, 4, 4) == BLOCK_STONE);

	budgetRelease(remaining - 1);
	worldExit(&s_world);
}

// handbuiltHeight()'s contract, stated as an assertion rather than left implicit:
// it returns the y of the first AIR block above the ground, so the block directly
// below it must be solid -- a body with its feet at that y is standing on the
// surface, not floating over it or sunk into it. Checked at one point from each
// distinct shape handbuilt.c draws: flat ground, a plateau top, inside the pit, and
// partway up the stepped pyramid.
static void testHandbuiltHeight(void)
{
	worldInit(&s_world);
	CHECK(handbuiltFill(&s_world));

	static const int xs[] = { 0, 8, 8, 26 };
	static const int zs[] = { 0, 0, 8, 26 };
	for (int i = 0; i < 4; i++) {
		const int h = handbuiltHeight(xs[i], zs[i]);
		CHECK(blockIsAir(worldGet(&s_world, xs[i], h, zs[i])));
		CHECK(blockIsSolid(worldGet(&s_world, xs[i], h - 1, zs[i])));
	}

	worldExit(&s_world);
}

// handbuiltFill()'s shape, in the constants handbuilt.c is actually built from: the
// checkerboard's period and step, the pyramid's one ring of rise per block, and the
// pit's exact depth. Only handbuiltFill()'s bool return was ever checked before
// this, which proves nothing about whether the world it built looks anything like
// the area handbuilt.h's header comment describes.
//
// These mirror handbuilt.c's own private #defines (GROUND_Y, PLATEAU_STEP,
// PIT_DEPTH, and the pyramid range) -- read from that file, not guessed, and kept
// in one place here so a change to either side shows up as a diff, not a silent
// drift.
#define HB_GROUND_Y      12
#define HB_PLATEAU_STEP  1
#define HB_PYRAMID_X0    20
#define HB_PIT_DEPTH     3
#define HB_PIT_X0        1
#define HB_PIT_X1        7    // exclusive
#define HB_PIT_MID       3    // a column on the pit floor, PIT_DEPTH down
#define HB_PIT_FLAT_Z    16   // flat ground sharing HB_PIT_MID's checker parity

static void testHandbuiltFillShape(void)
{
	worldInit(&s_world);
	CHECK(handbuiltFill(&s_world));

	// The checker: an 8-block period, one PLATEAU_STEP high. (0,0) and (16,0) are a
	// full period apart and land back on the same parity; (8,0) is half a period
	// away, on the other one.
	CHECK(worldGet(&s_world, 0, HB_GROUND_Y - 1, 0) != BLOCK_AIR);
	CHECK(worldGet(&s_world, 0, HB_GROUND_Y, 0) == BLOCK_AIR);
	CHECK(worldGet(&s_world, 8, HB_GROUND_Y + HB_PLATEAU_STEP - 1, 0) != BLOCK_AIR);
	CHECK(worldGet(&s_world, 8, HB_GROUND_Y + HB_PLATEAU_STEP, 0) == BLOCK_AIR);
	CHECK(worldGet(&s_world, 16, HB_GROUND_Y - 1, 0) != BLOCK_AIR);
	CHECK(worldGet(&s_world, 16, HB_GROUND_Y, 0) == BLOCK_AIR);

	// The pyramid: moving one block toward the centre on one axis, with the other
	// axis pinned at the pyramid's own edge, raises the surface by exactly one
	// block -- a ring per block, not some other slope.
	const int base = handbuiltHeight(HB_PYRAMID_X0, 26);
	const int step = handbuiltHeight(HB_PYRAMID_X0 + 1, 26);
	CHECK(step - base == 1);
	CHECK(worldGet(&s_world, HB_PYRAMID_X0 + 1, step - 1, 26) != BLOCK_AIR);
	CHECK(worldGet(&s_world, HB_PYRAMID_X0 + 1, step, 26) == BLOCK_AIR);

	// The pit: exactly PIT_DEPTH below the flat ground it is cut into, not some
	// other depth. (8,24) is flat ground and (8,8) is inside the pit; both share
	// the checker's parity, so the only thing that can account for a height
	// difference between them is the pit.
	const int pit_floor = handbuiltHeight(HB_PIT_MID, HB_PIT_MID);
	CHECK(handbuiltHeight(HB_PIT_MID, HB_PIT_FLAT_Z) - pit_floor == HB_PIT_DEPTH);
	CHECK(worldGet(&s_world, HB_PIT_MID, pit_floor - 1, HB_PIT_MID) != BLOCK_AIR);
	CHECK(worldGet(&s_world, HB_PIT_MID, pit_floor, HB_PIT_MID) == BLOCK_AIR);

	// The pit's walls step one block per ring rather than dropping sheer, so the floor
	// can be walked out of: the rim ring is one block down, not PIT_DEPTH down. Without
	// the intermediate rings these two would be equal.
	CHECK(handbuiltHeight(HB_PIT_X0, HB_PIT_MID) == HB_GROUND_Y - 1);
	CHECK(handbuiltHeight(HB_PIT_X1 - 1, HB_PIT_MID) == HB_GROUND_Y - 1);

	worldExit(&s_world);
}

// The one property that decides whether the hand-built area can actually be walked
// around: no two horizontally adjacent columns may differ in surface height by more
// than one block.
//
// One block is the exact limit the player can manage. Auto-step rises exactly one, and
// a jump reaches PLAYER_JUMP_SPEED^2 / (2 * -PLAYER_GRAVITY) = 8.5^2 / 56 = 1.29
// blocks, so a two-block rise cannot be climbed by any means. Anywhere this invariant
// breaks is a one-way drop: reachable, and then not leavable.
//
// This is written as a sweep of the whole area rather than as spot checks because the
// failures that matter are at the seams between features -- where the checkerboard's
// parity flips inside the pyramid, or where the pit's rim crosses a plateau boundary --
// and those are exactly the places nobody thinks to spot-check. It fails on the
// terrain as it stood before 2026-08-18: PLATEAU_STEP was 2, so every checker edge was
// a two-block wall, and the pit dropped 3 sheer.
static void testHandbuiltWalkable(void)
{
	int worst = 0;
	int worst_x = -1, worst_z = -1;

	for (int z = 0; z < HANDBUILT_BLOCKS_Z; z++) {
		for (int x = 0; x < HANDBUILT_BLOCKS_X; x++) {
			const int h = handbuiltHeight(x, z);

			if (x + 1 < HANDBUILT_BLOCKS_X) {
				const int d = handbuiltHeight(x + 1, z) - h;
				const int a = d < 0 ? -d : d;
				if (a > worst) { worst = a; worst_x = x; worst_z = z; }
			}
			if (z + 1 < HANDBUILT_BLOCKS_Z) {
				const int d = handbuiltHeight(x, z + 1) - h;
				const int a = d < 0 ? -d : d;
				if (a > worst) { worst = a; worst_x = x; worst_z = z; }
			}
		}
	}

	// Reported rather than just asserted: if this ever goes red, the coordinates are
	// what turn "the world is not walkable" into a place to look.
	if (worst > 1)
		printf("  worst adjacent step %d at x=%d z=%d\n", worst, worst_x, worst_z);

	CHECK(worst <= 1);
}

// bodyBlocked() itself, step 4.4's foundation: every resolve*/tryStepUp call in this
// file trusts this one predicate, so a bug in it would be invisible in the physics
// tests above -- they would just all be subtly wrong together. The one thing worth
// singling out on its own is BOX_EPS: a box whose far edge sits exactly on a grid
// line must read as belonging to the cell *below* that line, not the one it is
// only just touching.
static void testBodyBlocked(void)
{
	worldInit(&s_world);
	CHECK(worldSet(&s_world, 6, 5, 5, BLOCK_STONE));

	// Overlapping the block: true.
	CHECK(bodyBlocked(&s_world, 6.5f, 5.0f, 5.5f));

	// Clear air, nowhere near it: false.
	CHECK(!bodyBlocked(&s_world, 40.5f, 40.0f, 40.5f));

	// The box's east edge sits exactly on x=6, the block's near face. Half-open
	// means that edge belongs to the cell below it (x=5), not x=6, so this must
	// read clear even though the box is touching the plane the block starts on.
	CHECK(!bodyBlocked(&s_world, 5.7f, 5.0f, 5.5f));

	// One hundredth of a block further in and the same edge is unambiguously
	// inside x=6: this one must read blocked. The two positions are otherwise
	// identical, so only the epsilon boundary can account for the difference.
	CHECK(bodyBlocked(&s_world, 5.71f, 5.0f, 5.5f));

	worldExit(&s_world);
}

// Ceiling collision: a positive dy is the only path into resolveY's "moving up"
// branch, and nothing above exercises it -- every landing in testPhysics() comes
// from falling. A block placed above the player's head must stop the head at its
// near face exactly, for the same reason a landing stop is exact rather than
// approximate (see the comment above resolveY in physics.c).
static void testCeilingCollision(void)
{
	worldInit(&s_world);
	CHECK(worldSet(&s_world, 5, 8, 5, BLOCK_STONE));   // ceiling, near face at y=8

	Body body;
	bodyInit(&body, 5.5f, 5.0f, 5.5f);
	body.on_ground = true;   // moving up must clear this regardless of where it started
	body.vy = 3.0f;          // nonzero, so a zeroed vy below is provably resolveY's doing

	const int blocked = bodyMove(&body, &s_world, 0.0f, 10.0f, 0.0f);

	CHECK(blocked & BLOCKED_Y);
	CHECK(body.vy == 0.0f);
	CHECK(body.on_ground == false);
	CHECK(body.y == 8.0f - PLAYER_HEIGHT);   // head flush against the ceiling, exactly

	worldExit(&s_world);
}

// The remesh backlog's accounting. Written after it shipped broken: chunk_render.c
// cleared the flag in two places, the count went negative, and once a later edit brought
// it back to exactly 0 the drain latched off and the world stopped re-meshing. None of
// that was testable while the count lived in a file that includes <3ds.h>.
static void testDirtyQueue(void)
{
	DirtyQ q;

	CHECK(dirtyqInit(&q, 8));
	CHECK(dirtyqCount(&q) == 0);
	CHECK(dirtyqPeak(&q) == 0);
	CHECK(dirtyqConsistent(&q));

	// Marking is idempotent and only the first call counts, which is what makes
	// touching the same chunk repeatedly in one frame free.
	CHECK(dirtyqMark(&q, 3) == true);
	CHECK(dirtyqMark(&q, 3) == false);
	CHECK(dirtyqMark(&q, 3) == false);
	CHECK(dirtyqCount(&q) == 1);
	CHECK(dirtyqIsMarked(&q, 3));
	CHECK(!dirtyqIsMarked(&q, 4));
	CHECK(dirtyqConsistent(&q));

	// Out of range is refused, not clamped: marking slot 0 instead would quietly
	// schedule a remesh of a chunk nobody asked about.
	CHECK(dirtyqMark(&q, 8) == false);
	CHECK(dirtyqMark(&q, -1) == false);
	CHECK(dirtyqClear(&q, 8) == false);
	CHECK(dirtyqCount(&q) == 1);
	CHECK(dirtyqConsistent(&q));

	CHECK(dirtyqMark(&q, 0));
	CHECK(dirtyqMark(&q, 7));
	CHECK(dirtyqCount(&q) == 3);
	CHECK(dirtyqPeak(&q) == 3);

	// THE REGRESSION. Every slot cleared twice, exactly as it happened when
	// chunkRenderBuild cleared a flag chunkRenderDrainDirty was about to clear. The
	// second clear must be a no-op, not a decrement.
	for (int i = 0; i < 8; i++) {
		const bool first  = dirtyqClear(&q, i);
		const bool second = dirtyqClear(&q, i);
		CHECK(second == false);
		if (i == 0 || i == 3 || i == 7) CHECK(first == true);
		else                            CHECK(first == false);
	}
	CHECK(dirtyqCount(&q) == 0);      // this read -3 with the shipped bug
	CHECK(dirtyqConsistent(&q));

	// And the consequence that made the bug invisible: after the double-clear the queue
	// still has to accept work and still has to report it. With the old accounting the
	// count came back up to exactly 0 here and the drain's == 0 early-out returned
	// immediately, forever.
	CHECK(dirtyqMark(&q, 1));
	CHECK(dirtyqMark(&q, 2));
	CHECK(dirtyqMark(&q, 5));
	CHECK(dirtyqCount(&q) == 3);
	CHECK(dirtyqCount(&q) > 0);
	CHECK(dirtyqConsistent(&q));

	// The peak survives the queue draining, and resets to the live count rather than to
	// zero — a backlog can be non-empty at the moment of reset.
	CHECK(dirtyqPeak(&q) == 3);
	CHECK(dirtyqClear(&q, 1));
	dirtyqResetPeak(&q);
	CHECK(dirtyqPeak(&q) == 2);
	CHECK(dirtyqCount(&q) == 2);
	CHECK(dirtyqConsistent(&q));

	// A pool bigger than the flag array is refused at init, so it fails at startup
	// instead of writing past the array the first time a high slot is edited.
	DirtyQ big;
	CHECK(dirtyqInit(&big, DIRTYQ_MAX) == true);
	CHECK(dirtyqInit(&big, DIRTYQ_MAX + 1) == false);
	CHECK(dirtyqCount(&big) == 0);
	CHECK(dirtyqMark(&big, 0) == false);   // capacity 0 after a refused init
	CHECK(dirtyqConsistent(&big));

	// A full queue: every slot marked, then drained the way chunkRenderDrainDirty does.
	CHECK(dirtyqInit(&q, DIRTYQ_MAX));
	for (int i = 0; i < DIRTYQ_MAX; i++) CHECK(dirtyqMark(&q, i));
	CHECK(dirtyqCount(&q) == DIRTYQ_MAX);

	int drained = 0;
	for (int i = 0; i < DIRTYQ_MAX; i++) {
		if (!dirtyqIsMarked(&q, i)) continue;
		dirtyqClear(&q, i);
		drained++;
	}
	CHECK(drained == DIRTYQ_MAX);
	CHECK(dirtyqCount(&q) == 0);
	CHECK(dirtyqPeak(&q) == DIRTYQ_MAX);
	CHECK(dirtyqConsistent(&q));
}

// Step 4.3's acceptance criterion, as a permanent test rather than the throwaway probe
// it was: "place a block, travel 200 blocks away and back, it persists".
//
// ⚠ READ THIS BEFORE TRUSTING IT. Nothing evicts chunks yet — streaming is Phase 5 — so
// the literal criterion cannot fail today for the reason it exists, and a version of this
// test that only walked away and back would be a check that could not go red. What it
// tests instead is every part of the mechanism that *is* reachable now and that a
// streaming bug would break:
//
//   * reads far outside the built world must not allocate anything (a read that quietly
//     creates a column is how a walk turns into an out-of-memory later),
//   * an edit in a chunk that did not exist must create it and survive,
//   * edits must survive the chunk table being driven to its 1024-column limit, which is
//     the closest thing to eviction pressure this build can produce.
//
// **Re-run the real criterion at step 5.5**, against actual load/unload. Until then the
// gap is: a bug that loses blocks *on unload* would pass everything below.
static void testEditPersistence(void)
{
	worldInit(&s_world);
	CHECK(handbuiltFill(&s_world));

	// Three edits, deliberately different in kind: mid-chunk, on a chunk boundary, and
	// one in a sky chunk that does not exist yet and has to be created.
	const int mid_y   = handbuiltHeight(18, 18);
	const int edge_y  = handbuiltHeight(16, 16);
	const int sky_y   = 100;

	CHECK(worldGet(&s_world, 18, mid_y, 18) == BLOCK_AIR);
	CHECK(worldChunk(&s_world, 1, sky_y / CHUNK_DIM, 1) == NULL);   // nothing up there yet

	CHECK(worldSet(&s_world, 18, mid_y, 18, BLOCK_STONE));
	CHECK(worldSet(&s_world, 16, edge_y, 16, BLOCK_SAND));
	CHECK(worldSet(&s_world, 18, sky_y, 18, BLOCK_DIRT));

	CHECK(worldChunk(&s_world, 1, sky_y / CHUNK_DIM, 1) != NULL);   // the edit made it

	const int    columns_after_edit = s_world.columns;
	const int    chunks_after_edit  = s_world.chunks;
	const size_t bytes_after_edit   = worldBytes(&s_world);

	// Walk 200 blocks out and 200 back, reading at every step the way the player's
	// raycast and physics do. The built area is only 64x64, so everything past x=63 is
	// unloaded world.
	int reads = 0;
	for (int x = 18; x <= 218; x++) { (void)worldGet(&s_world, x, mid_y, 18); reads++; }
	for (int x = 218; x >= 18; x--) { (void)worldGet(&s_world, x, mid_y, 18); reads++; }
	for (int y = 0; y < WORLD_HEIGHT; y += 8) { (void)worldGet(&s_world, 218, y, 18); reads++; }
	CHECK(reads == 402 + 16);

	// The point of the walk: 418 reads outside the built world allocated NOTHING.
	CHECK(s_world.columns == columns_after_edit);
	CHECK(s_world.chunks  == chunks_after_edit);
	CHECK(worldBytes(&s_world) == bytes_after_edit);

	CHECK(worldGet(&s_world, 18, mid_y, 18) == BLOCK_STONE);
	CHECK(worldGet(&s_world, 16, edge_y, 16) == BLOCK_SAND);
	CHECK(worldGet(&s_world, 18, sky_y, 18) == BLOCK_DIRT);

	// Now the pressure case: fill the column table to its 1024-column limit with far-away
	// columns, so any further allocation is refused. This is as close as this build gets
	// to the memory pressure streaming will really apply.
	int created = 0;
	for (int i = 1; i < WORLD_MAP_SLOTS && s_world.columns < WORLD_MAP_SLOTS; i++)
		if (worldColumnCreate(&s_world, 1000 + i, 1000)) created++;
	CHECK(created > 0);
	CHECK(s_world.columns == WORLD_MAP_SLOTS);
	CHECK(worldColumnCreate(&s_world, 9999, 9999) == NULL);   // genuinely full

	// The three edits are still exactly what was written, and the terrain around them
	// is untouched.
	CHECK(worldGet(&s_world, 18, mid_y, 18) == BLOCK_STONE);
	CHECK(worldGet(&s_world, 16, edge_y, 16) == BLOCK_SAND);
	CHECK(worldGet(&s_world, 18, sky_y, 18) == BLOCK_DIRT);
	CHECK(worldGet(&s_world, 18, mid_y - 1, 18) == BLOCK_GRASS);
	CHECK(worldGet(&s_world, 18, sky_y - 1, 18) == BLOCK_AIR);

	worldExit(&s_world);
}

// The numbers a player actually feels, measured by running the real simulation at the
// real frame rate rather than read off the constants.
//
// This exists because "control feel" was the one Phase 4 item with nothing behind it at
// all: PLAYER_WALK_SPEED is 4.3 and PLAYER_JUMP_SPEED is 8.5, but what reaches the player
// is those constants after gravity, substepping and a 59.83 Hz tick, and none of that was
// ever measured. The point is not to decide whether 4.3 blocks/s feels right — that is
// his to judge — it is that when he says "too slow" or "the jump is floaty" there is a
// number to change instead of a guess, and that a later edit to the physics cannot alter
// how it feels without turning a check red.
//
// Measured 2026-08-18 at the real 59.83 Hz tick. These are the figures the bands below
// are drawn around — not targets, just what the game currently does:
//
//     walk        4.3122 blocks in one second
//     jump apex   1.2197 blocks above the feet (the analytic ceiling is 1.29)
//     airtime     36 ticks, 0.602 s
//     jump reach  2.5873 blocks travelled while airborne at full walk speed
//
// ⚠ Not covered here, because it lives in `source/scene/` behind `<3ds.h>` and cannot be
// linked into a host build: which way the D-pad and circle pad actually point. That is
// verified separately on the emulator.
#define TICK_HZ  59.83f   // measured 3DS VBlank rate, not the nominal 60

static void testControlFeel(void)
{
	const float dt = 1.0f / TICK_HZ;

	worldInit(&s_world);
	for (int x = 0; x < 12; x++)
		for (int z = 0; z < 12; z++)
			CHECK(worldSet(&s_world, x, 10, z, BLOCK_STONE));   // floor top at y=11

	// --- Walking. One second of ticks at full stick, on the flat.
	{
		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;

		const float x0 = body.x;
		for (int i = 0; i < 60; i++) {
			body.vx = PLAYER_WALK_SPEED;   // as playerUpdate sets it every frame
			bodyStep(&body, &s_world, dt);
		}
		const float travelled = body.x - x0;

		// 4.3 blocks/s over 60 ticks of 1/59.83 s is 4.312 blocks. Anything outside
		// this band means the substepping is losing or gaining distance.
		CHECK(travelled > 4.28f && travelled < 4.35f);
	}

	// --- Jumping. Apex above the feet's resting height, and how long it lasts.
	{
		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;
		body.vy = PLAYER_JUMP_SPEED;

		float peak   = body.y;
		int   ticks  = 0;
		bool  landed = false;
		for (int i = 0; i < 400 && !landed; i++) {
			bodyStep(&body, &s_world, dt);
			ticks++;
			if (body.y > peak) peak = body.y;
			if (body.on_ground) landed = true;
		}
		CHECK(landed);

		const float apex = peak - 11.0f;

		// The analytic apex is PLAYER_JUMP_SPEED^2 / (2 * -PLAYER_GRAVITY) = 8.5^2/56
		// = 1.29 blocks. A discretely sampled arc peaks a little under that, and this
		// asserts it stays there: below 1.0 and a one-block hop stops clearing, which
		// is the difference between a walkable world and a set of pens.
		CHECK(apex > 1.20f && apex < 1.25f);
		CHECK(apex > 1.0f);   // stated separately because THIS is the load-bearing one

		// Airtime: about 0.6 s up and down at 28 blocks/s^2. Long enough to be a jump,
		// short enough not to feel like the moon.
		CHECK(ticks >= 33 && ticks <= 39);

		// How far a running jump carries, which is the number that decides whether a
		// one-block gap is crossable: walk speed times airtime.
		const float jump_reach = PLAYER_WALK_SPEED * (float)ticks * dt;
		CHECK(jump_reach > 2.4f && jump_reach < 2.9f);
	}

	worldExit(&s_world);
}

// The invariant scene/chunk_render.c's all-air early-out rests on: a chunk containing no
// solid block meshes to nothing *no matter what surrounds it*. If that were ever false —
// if the mesher grew a rule that emitted geometry for a neighbour's block, a water
// surface say, or a chunk-boundary skirt — the early-out would silently stop drawing it,
// and the symptom would be missing geometry in exactly the chunks nobody looks at.
//
// Deliberately tested against the worst case rather than an empty world: the air chunk is
// surrounded on all 26 sides by solid stone, which is the arrangement that would produce
// the most geometry if the rule ever changed. chunkIsAllAir is checked on the same chunks
// so the predicate and the thing it predicts cannot drift apart.
static void testAllAirMeshesToNothing(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	worldInit(&s_world);

	bool built = true;
	for (int dy = 0; dy <= 2; dy++)
		for (int dz = 0; dz <= 2; dz++)
			for (int dx = 0; dx <= 2; dx++)
				if (dx != 1 || dy != 1 || dz != 1)
					built = fillChunk(dx, dy, dz, BLOCK_STONE) && built;
	CHECK(built);
	CHECK(worldChunkCreate(&s_world, 1, 1, 1) != NULL);   // exists, and is all air

	const Chunk* middle = worldChunk(&s_world, 1, 1, 1);
	CHECK(middle != NULL);
	CHECK(chunkIsAllAir(middle));

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 0);
	CHECK(out.vert_count == 0);
	CHECK(out.index_count == 0);

	// And the predicate is not simply always true: one block turns it off, and that block
	// is exactly what the mesher then draws — six faces against the surrounding air gap...
	// except there is no gap here, so the neighbours occlude nothing that matters. Placed
	// at the chunk's centre, well clear of the borders, so all six of its faces meet air
	// inside this same chunk and the count is unambiguous.
	CHECK(worldSet(&s_world, 16 + 8, 16 + 8, 16 + 8, BLOCK_STONE));
	CHECK(!chunkIsAllAir(worldChunk(&s_world, 1, 1, 1)));

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);

	// A chunk with no storage at all is air too, and takes the same early-out path.
	CHECK(worldChunk(&s_world, 40, 2, 40) == NULL);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// --- Step 5.1: seeded PRNG and value noise -----------------------------------------
//
// The step's stated criterion is "same seed produces byte-identical chunks across runs".
// A single process cannot literally re-run itself, so that is tested as the property the
// criterion is really about: **the noise is a pure function of (seed, position) and of
// nothing else** — not of call order, not of how many samples came before, not of which
// chunk asked. That is the property a walk-east-then-north world needs, and it is what
// would actually be broken by generating from a running stream instead of a hash.
static void testNoiseDeterminism(void)
{
	const uint32_t seed = 0xC0FFEEu;

	// Same input, same output, however many other samples happen in between. The
	// interleaved decoy sample is the point: a generator carrying hidden state passes a
	// naive repeat test and fails this one.
	for (int i = 0; i < 64; i++) {
		const fx x = fxFromInt(i * 7) + (FX_ONE / 3);
		const fx z = fxFromInt(i * 13) - (FX_ONE / 7);

		const fx a = noiseFbm2(seed, x, z, 4);
		(void)noiseFbm2(seed ^ 0x5A5A5A5Au, x + FX_ONE, z - FX_ONE, 6);
		const fx b = noiseFbm2(seed, x, z, 4);
		CHECK(a == b);
	}

	// Sampling a region forwards and then backwards must give the same values in the
	// same places — the order-independence a chunk generator relies on.
	fx forward[32], backward[32];
	for (int i = 0; i < 32; i++)
		forward[i] = noiseFbm2(seed, fxFromInt(i), fxFromInt(100 - i), 5);
	for (int i = 31; i >= 0; i--)
		backward[i] = noiseFbm2(seed, fxFromInt(i), fxFromInt(100 - i), 5);
	int same = 0;
	for (int i = 0; i < 32; i++)
		if (forward[i] == backward[i]) same++;
	CHECK(same == 32);

	// A different seed must actually change the world. Equality on a handful of samples
	// would be chance; equality on all 32 means the seed is being ignored.
	int differing = 0;
	for (int i = 0; i < 32; i++)
		if (noiseFbm2(seed, fxFromInt(i), fxFromInt(i * 3), 5)
		    != noiseFbm2(seed + 1, fxFromInt(i), fxFromInt(i * 3), 5))
			differing++;
	CHECK(differing >= 30);

	// Range. Everything downstream scales this to a block height, so an out-of-range
	// value is terrain outside the world rather than a wrong-looking hill.
	fx lo = FX_ONE * 2, hi = -1;
	for (int i = 0; i < 400; i++) {
		const fx v = noiseFbm2(seed, fxFromInt(i) + (FX_ONE >> 2), fxFromInt(i * 5), 4);
		if (v < lo) lo = v;
		if (v > hi) hi = v;
		CHECK_QUIET(v >= 0 && v <= FX_ONE);
	}
	// And it must use its range: a generator stuck near the middle makes flat terrain.
	CHECK(lo < FX_ONE / 3);
	CHECK(hi > (FX_ONE * 2) / 3);

	// Negative coordinates are a real region of the world, not an edge case: the lattice
	// floor has to keep flooring, or x = 0 gets a mirror seam through it.
	const fx neg = noiseFbm2(seed, fxFromInt(-40), fxFromInt(-40), 4);
	const fx pos = noiseFbm2(seed, fxFromInt(40), fxFromInt(40), 4);
	CHECK(neg != pos);
	for (int i = 1; i <= 16; i++)
		CHECK_QUIET(noiseValue2(seed, fxFromInt(-i), 0) != noiseValue2(seed, fxFromInt(i), 0));

	// The above is not enough on its own, and finding that out is the reason this block
	// exists: at *integer* positions an arithmetic shift and a truncating divide agree
	// exactly, so replacing the lattice floor with `/ FX_ONE` passed the whole suite. It
	// only shows up at fractional negative positions, where truncation collapses cell -1
	// onto cell 0 and mirrors the noise about the origin. So sample there.
	CHECK(noiseValue2(seed, -(FX_ONE / 2), 0) != noiseValue2(seed, FX_ONE / 2, 0));
	CHECK(noiseValue2(seed, 0, -(FX_ONE / 2)) != noiseValue2(seed, 0, FX_ONE / 2));

	// And the crossing itself must be smooth. Under truncation the value jumps as the
	// sample passes zero; under a correct floor it walks continuously through it.
	{
		fx last = noiseValue2(seed, -(FX_ONE / 2), FX_ONE / 4);
		for (int i = -7; i <= 8; i++) {
			const fx v = noiseValue2(seed, (fx)(i * (FX_ONE / 16)), FX_ONE / 4);
			const fx d = v > last ? v - last : last - v;
			CHECK_QUIET(d < FX_ONE / 4);
			last = v;
		}
	}

	// Continuity: neighbouring blocks must not jump. Sampled at 1/16 of a lattice cell,
	// which is the scale a heightmap actually walks, the step must stay small — a value
	// noise cell can cross its whole range in one cell, so the bound is a quarter of the
	// range per sixteenth of a cell, and a discontinuity blows straight through it.
	fx prev = noiseValue2(seed, 0, 0);
	for (int i = 1; i <= 256; i++) {
		const fx v = noiseValue2(seed, (fx)(i * (FX_ONE / 16)), 0);
		const fx d = v > prev ? v - prev : prev - v;
		CHECK_QUIET(d < FX_ONE / 4);
		prev = v;
	}

	// 3D, for the caves in 5.4: pure in three coordinates, and in range.
	for (int i = 0; i < 32; i++) {
		const fx x = fxFromInt(i), y = fxFromInt(i * 2 - 30), z = fxFromInt(-i);
		const fx a = noiseFbm3(seed, x, y, z, 3);
		(void)noiseFbm3(seed, x + 1, y, z, 3);
		CHECK_QUIET(a == noiseFbm3(seed, x, y, z, 3));
		CHECK_QUIET(a >= 0 && a <= FX_ONE);
	}
	// y must be a real axis, not ignored — a cave generator that ignores y digs shafts.
	CHECK(noiseFbm3(seed, 0, 0, 0, 3) != noiseFbm3(seed, 0, fxFromInt(9), 0, 3));

	// Octave count is clamped, not trusted, and the clamp is at the documented bounds.
	CHECK(noiseFbm2(seed, FX_ONE, FX_ONE, 0)   == noiseFbm2(seed, FX_ONE, FX_ONE, 1));
	CHECK(noiseFbm2(seed, FX_ONE, FX_ONE, 999) == noiseFbm2(seed, FX_ONE, FX_ONE, 8));
}

static void testRng(void)
{
	// Positional hashes: pure, and distinct for distinct inputs. 4,096 coordinates into a
	// 32-bit space should collide essentially never; allowing a couple keeps the test
	// about the mixer rather than about luck.
	uint32_t seen[4096];
	int n = 0;
	for (int x = 0; x < 64; x++)
		for (int z = 0; z < 64; z++)
			seen[n++] = rngHash2(0x1234u, x, z);
	int collisions = 0;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (seen[i] == seen[j]) collisions++;
	CHECK(collisions <= 2);

	// Neighbouring coordinates must not give neighbouring hashes — that is the failure
	// that shows up as a visible grid in the terrain rather than as a wrong number.
	int well_mixed = 0;
	for (int x = 0; x < 64; x++) {
		const uint32_t a = rngHash2(7u, x, 0), b = rngHash2(7u, x + 1, 0);
		const uint32_t d = a ^ b;
		int bits = 0;
		for (int i = 0; i < 32; i++) bits += (d >> i) & 1u;
		if (bits >= 8) well_mixed++;   // ~16 expected from a good mixer
	}
	CHECK(well_mixed >= 60);

	// Swapping the coordinates must change the answer, or the world is diagonally
	// symmetric.
	CHECK(rngHash2(7u, 3, 9) != rngHash2(7u, 9, 3));
	CHECK(rngHash3(7u, 1, 2, 3) != rngHash3(7u, 3, 2, 1));
	CHECK(rngHash3(7u, 1, 2, 3) != rngHash3(7u, 1, 5, 3));   // y is not ignored

	// The stream: reproducible from a seed, and never stuck. Zero is a fixed point of
	// xorshift, so seeding with it must be caught rather than silently produce a constant.
	Rng a, b;
	rngSeed(&a, 0);
	rngSeed(&b, 0);
	CHECK(a.state != 0);
	int matched = 0, nonzero = 0;
	for (int i = 0; i < 256; i++) {
		const uint32_t va = rngNext(&a), vb = rngNext(&b);
		if (va == vb) matched++;
		if (va != 0) nonzero++;
	}
	CHECK(matched == 256);
	CHECK(nonzero == 256);

	// rngBelow must stay in range and actually spread across it.
	Rng r;
	rngSeed(&r, 99);
	int buckets[8] = {0};
	for (int i = 0; i < 4000; i++) {
		const uint32_t v = rngBelow(&r, 8);
		CHECK_QUIET(v < 8);
		buckets[v]++;
	}
	for (int i = 0; i < 8; i++)
		CHECK_QUIET(buckets[i] > 350 && buckets[i] < 650);   // 500 expected
}

// --- Step 5.2: heightmap terrain ---------------------------------------------------
//
// The criterion this has to hold to is the one the step inherits from 5.1 — the same seed
// gives the same world — expressed on actual blocks rather than on noise values, plus the
// layering being right everywhere rather than on the one column somebody looked at.
static void testWorldgenTerrain(void)
{
	WorldGen g;
	worldgenInit(&g, 12345u);

	// Heights stay inside the declared band. This is what stops the surface from being
	// clamped flat at the top of the world, and it is checked over a wide area rather
	// than a few points because a band violation is rare by construction.
	int lo = WORLD_HEIGHT, hi = 0;
	for (int z = -200; z <= 200; z += 7) {
		for (int x = -200; x <= 200; x += 7) {
			const int h = worldgenHeight(&g, x, z);
			CHECK_QUIET(h >= GEN_SURFACE_MIN && h <= GEN_SURFACE_MIN + GEN_SURFACE_RANGE);
			if (h < lo) lo = h;
			if (h > hi) hi = h;
		}
	}
	// It must also *use* the band. A generator returning one height everywhere satisfies
	// every bound above and produces a flat plain.
	CHECK(hi - lo >= 15);

	// Neighbouring columns must not step by more than a block or two, or the terrain is
	// a field of pillars and the player's 1-block auto-step cannot walk it. This is the
	// check that ties generation to the measured movement in testControlFeel: a 2-block
	// rise is unclimbable by any means, so terrain that produces them is a design bug.
	int steep = 0;
	for (int z = -60; z <= 60; z++) {
		for (int x = -60; x < 60; x++) {
			const int a = worldgenHeight(&g, x, z);
			const int b = worldgenHeight(&g, x + 1, z);
			if (a - b > 1 || b - a > 1) steep++;
		}
	}
	CHECK(steep == 0);

	worldInit(&s_world);
	CHECK(worldgenColumn(&g, &s_world, 0, 0));
	CHECK(worldgenColumn(&g, &s_world, 1, 0));
	CHECK(worldgenColumn(&g, &s_world, -1, -1));

	// Layering, checked on every one of the 256 columns of chunk (0,0) rather than a
	// sample: the surface cap on top, stone below it, and only air or a tree above.
	//
	// Since step 5.3 the cap is not always grass — a sandy column is sand all the way
	// through the cap, not sand over dirt, because a one-block lid reads as dirt with a
	// skin on it the moment you dig. The expected material comes from worldgenIsSandy
	// rather than from a copy of the threshold, so the test cannot drift from the rule.
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int  h     = worldgenHeight(&g, lx, lz);
			const bool sandy = worldgenIsSandy(&g, lx, lz);
			const BlockId cap = sandy ? BLOCK_SAND : BLOCK_GRASS;
			const BlockId sub = sandy ? BLOCK_SAND : BLOCK_DIRT;

			CHECK_QUIET(worldGet(&s_world, lx, h - 1, lz) == cap);
			for (int d = 1; d <= GEN_DIRT_DEPTH; d++)
				CHECK_QUIET(worldGet(&s_world, lx, h - 1 - d, lz) == sub);
			CHECK_QUIET(worldGet(&s_world, lx, h - 2 - GEN_DIRT_DEPTH, lz) == BLOCK_STONE);
			CHECK_QUIET(worldGet(&s_world, lx, 0, lz) == BLOCK_STONE);

			// Above the ground is air or a tree, never terrain material. This is the check
			// that would catch a decoration pass writing dirt or stone into the sky, and it
			// is deliberately not "must be air" any more — that assertion is what broke
			// when trees landed, and weakening it to nothing would have hidden the next bug.
			const BlockId over = worldGet(&s_world, lx, h, lz);
			CHECK_QUIET(over == BLOCK_AIR || over == BLOCK_WOOD || over == BLOCK_LEAVES);
		}
	}

	// The generated blocks must agree with worldgenHeight, which is what spawn-finding
	// and the tests above rely on. Checked by walking down from the sky to the first
	// solid block, so a fill loop that was off by one is caught rather than assumed.
	// Trees are stepped over: worldgenHeight is the GROUND, and standing on a canopy is a
	// collision question, not a heightmap one.
	for (int lz = 0; lz < CHUNK_DIM; lz += 3) {
		for (int lx = 0; lx < CHUNK_DIM; lx += 3) {
			int y = WORLD_HEIGHT - 1;
			while (y > 0) {
				const BlockId b = worldGet(&s_world, lx, y, lz);
				if (b != BLOCK_AIR && b != BLOCK_WOOD && b != BLOCK_LEAVES) break;
				y--;
			}
			CHECK_QUIET(y + 1 == worldgenHeight(&g, lx, lz));
		}
	}

	// Chunk seams. The column at x=15 and the one at x=16 belong to different chunks and
	// were generated by different calls; the heightmap does not know that, so their
	// heights must agree with a continuous surface. A generator that seeded per chunk
	// would put a cliff on every chunk border here.
	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		const int a = worldgenHeight(&g, 15, lz);
		const int b = worldgenHeight(&g, 16, lz);
		CHECK_QUIET(a - b <= 1 && b - a <= 1);
		const BlockId cap = worldgenIsSandy(&g, 16, lz) ? BLOCK_SAND : BLOCK_GRASS;
		CHECK_QUIET(worldGet(&s_world, 16, b - 1, lz) == cap);
	}

	// Sky chunks are left unallocated rather than filled with air: a world of mostly sky
	// has to cost nothing, and this is the property that makes it so.
	CHECK(worldChunk(&s_world, 0, COLUMN_CHUNKS - 1, 0) == NULL);
	CHECK(worldChunk(&s_world, 0, 0, 0) != NULL);

	worldExit(&s_world);
}

// Same seed, same blocks — the literal step 5.1/5.2 criterion, done on generated chunks
// and across generation orders. Two worlds are built from one seed, one column-by-column
// forwards and one backwards, and every block of both is compared.
#define GEN_TEST_COLS 3
#define GEN_TEST_SPAN (GEN_TEST_COLS * CHUNK_DIM)

// FNV-1a over every block of the test region, in scan order.
static uint32_t genTestHash(void)
{
	uint32_t h = 2166136261u;
	for (int x = 0; x < GEN_TEST_SPAN; x++)
		for (int y = 0; y < WORLD_HEIGHT; y++)
			for (int z = 0; z < GEN_TEST_SPAN; z++) {
				h ^= worldGet(&s_world, x, y, z);
				h *= 16777619u;
			}
	return h;
}

static void testWorldgenDeterminism(void)
{
	WorldGen g;
	worldgenInit(&g, 0xBEEFu);

	// Hashed rather than snapshotted. A 48x128x48 copy would be 295 KB of .bss, and this
	// file is compiled into the 3DS build too — the console runs the same self-test, and a
	// quarter of a megabyte reserved for one comparison is not a reasonable thing to ship.
	// FNV-1a over every block in scan order catches a single wrong block anywhere.
	worldInit(&s_world);
	for (int cz = 0; cz < GEN_TEST_COLS; cz++)
		for (int cx = 0; cx < GEN_TEST_COLS; cx++)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	const uint32_t forward_hash = genTestHash();

	const int    columns_forward = s_world.columns;
	const size_t bytes_forward   = worldBytes(&s_world);
	worldExit(&s_world);

	// Rebuild in the opposite order, which is what a player walking the other way does.
	worldInit(&s_world);
	for (int cz = GEN_TEST_COLS - 1; cz >= 0; cz--)
		for (int cx = GEN_TEST_COLS - 1; cx >= 0; cx--)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	CHECK(genTestHash() == forward_hash);
	CHECK(s_world.columns == columns_forward);
	CHECK(worldBytes(&s_world) == bytes_forward);

	// The hash must be able to see a single wrong block, or the comparison above proves
	// nothing at all — an all-air world would also match itself.
	CHECK(worldSet(&s_world, 20, 30, 20, BLOCK_SAND));
	CHECK(genTestHash() != forward_hash);

	// A different seed must give a different world — otherwise the seed is decoration.
	// Compared on the surface layer, where a heightmap difference actually shows.
	//
	// Over an area rather than a single row, and at 60 % rather than the 83 % this used
	// before step 5.3. Not a weakened test — a corrected one: with the biome pass, a region
	// that lands in a flat biome under both seeds has every column compressed toward the
	// centre of the band, so a lot of them collide on the same integer height without the
	// two worlds being remotely alike. Measured: this seed pair differs on 71.9 % of a
	// 48x48 area (one row of it happened to give 29/48 = 60.4 %, which is why a row is not
	// a big enough sample), and the worst of 24 adjacent seed pairs was 74.6 %.
	WorldGen g2;
	worldgenInit(&g2, 0xBEEFu + 1);
	int height_differs = 0;
	for (int z = 0; z < 48; z++)
		for (int x = 0; x < 48; x++)
			if (worldgenHeight(&g, x, z) != worldgenHeight(&g2, x, z))
				height_differs++;
	CHECK(height_differs >= 48 * 48 * 60 / 100);

	worldExit(&s_world);
}

// --- Step 5.3: biome variation --------------------------------------------------------
//
// The claim being tested is not "there is a second noise" — it is the two things that
// second noise was added to produce: somewhere is flatter than somewhere else, and both
// kinds of ground exist close enough together to walk between.
static void testWorldgenBiome(void)
{
	WorldGen g;
	worldgenInit(&g, 12345u);

	// The field is a normalised noise value. Anything outside [0, FX_ONE] means the octave
	// weights no longer sum to one, and every threshold derived from it silently moves.
	for (int z = -300; z <= 300; z += 11)
		for (int x = -300; x <= 300; x += 11) {
			const fx b = worldgenBiome(&g, x, z);
			CHECK_QUIET(b >= 0 && b <= FX_ONE);
		}

	// Both biomes within walking distance. A 192x192 neighbourhood is about what a player
	// covers in a minute at the measured 4.3122 blocks/s; a world that is all one biome at
	// that scale is a world with no variation in it, whatever the global average says.
	// Three seeds, because one seed proving it proves nothing about the next world.
	const uint32_t seeds[3] = { 12345u, 0xBEEFu, 7919u };
	for (int s = 0; s < 3; s++) {
		WorldGen gs;
		worldgenInit(&gs, seeds[s]);
		int sandy = 0, cols = 0;
		for (int z = -96; z < 96; z += 3)
			for (int x = -96; x < 96; x += 3) {
				cols++;
				if (worldgenIsSandy(&gs, x, z)) sandy++;
			}
		CHECK_QUIET(sandy * 100 > cols * 2);          // at least 2 % sand
		CHECK_QUIET(sandy * 100 < cols * 98);         // at least 2 % not sand
	}

	// Amplitude really is modulated. Every column's height must sit inside the envelope its
	// own biome value allows, so a low-biome column cannot be as tall as a high-biome one.
	// This is the check that goes red if the modulation is dropped and every column reverts
	// to the full range — a flat biome would immediately overshoot its own envelope.
	const int centre = GEN_SURFACE_MIN + GEN_SURFACE_RANGE / 2;
	int flattest = GEN_SURFACE_RANGE;
	for (int z = -200; z <= 200; z += 7) {
		for (int x = -200; x <= 200; x += 7) {
			const fx b = worldgenBiome(&g, x, z);
			const int64_t amp = (int64_t)GEN_SURFACE_RANGE *
			                    (FX_ONE / GEN_FLAT_FRACTION +
			                     (int64_t)b * (GEN_FLAT_FRACTION - 1) / GEN_FLAT_FRACTION);
			const int max_off = (int)((amp / 2) >> FX_SHIFT) + 1;
			const int h = worldgenHeight(&g, x, z);
			CHECK_QUIET(h - centre <= max_off && centre - h <= max_off);
			if (max_off < flattest) flattest = max_off;
		}
	}
	// ...and the envelope has to actually bind somewhere, or the bound above is satisfied
	// by any terrain at all and proves nothing.
	CHECK(flattest <= 6);
}

// --- Step 5.3: trees ------------------------------------------------------------------
//
// FNV-1a over one column, used to compare a column generated on its own against the same
// column generated with its neighbours around it.
static uint32_t colHash(int32_t cx, int32_t cz)
{
	uint32_t h = 2166136261u;
	for (int lx = 0; lx < CHUNK_DIM; lx++)
		for (int y = 0; y < WORLD_HEIGHT; y++)
			for (int lz = 0; lz < CHUNK_DIM; lz++) {
				h ^= worldGet(&s_world, cx * CHUNK_DIM + lx, y, cz * CHUNK_DIM + lz);
				h *= 16777619u;
			}
	return h;
}

static void testWorldgenTrees(void)
{
	// Seed picked by measurement, not by trying one: the 48x48 region below has to contain
	// both biomes or its layering check silently only ever tests one of the two rules. Of
	// 60 candidate seeds, 1616 splits this region 47.7 % sand to 52.3 % grass — the most
	// even of them, so both halves get real coverage and there is still ground for trees.
	WorldGen g;
	worldgenInit(&g, 1616u);

	worldInit(&s_world);
	for (int cz = 0; cz < 3; cz++)
		for (int cx = 0; cx < 3; cx++)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	// There are trees at all. GEN_TREE_CHANCE is 96/256 over 8x8 cells, so a 48x48 area has
	// 36 cells and should carry roughly a dozen trees; a generator that placed none would
	// pass every "no bad blocks" check below.
	int wood = 0, leaves = 0;
	for (int x = 0; x < 48; x++)
		for (int y = 0; y < WORLD_HEIGHT; y++)
			for (int z = 0; z < 48; z++) {
				const BlockId b = worldGet(&s_world, x, y, z);
				if (b == BLOCK_WOOD)   wood++;
				if (b == BLOCK_LEAVES) leaves++;
			}
	CHECK(wood >= 20);
	CHECK(leaves >= 100);

	// Every trunk stands on the ground it was placed against — not floating, not buried —
	// and never on sand, which is the rule worldgenIsSandy is supposed to enforce. Checked
	// by finding the lowest wood block in each (x, z) and comparing with worldgenHeight.
	int trunks = 0;
	for (int x = 0; x < 48; x++) {
		for (int z = 0; z < 48; z++) {
			int lowest = -1;
			for (int y = 0; y < WORLD_HEIGHT; y++)
				if (worldGet(&s_world, x, y, z) == BLOCK_WOOD) { lowest = y; break; }
			if (lowest < 0) continue;
			trunks++;
			CHECK_QUIET(lowest == worldgenHeight(&g, x, z));
			CHECK_QUIET(worldGet(&s_world, x, lowest - 1, z) == BLOCK_GRASS);
			CHECK_QUIET(!worldgenIsSandy(&g, x, z));

			// The trunk is unbroken from the ground to its top...
			int highest = lowest;
			while (worldGet(&s_world, x, highest + 1, z) == BLOCK_WOOD) highest++;
			for (int y = lowest; y <= highest; y++)
				CHECK_QUIET(worldGet(&s_world, x, y, z) == BLOCK_WOOD);
			CHECK_QUIET(highest - lowest + 1 >= GEN_TREE_MIN_H);
			CHECK_QUIET(highest - lowest + 1 <= GEN_TREE_MAX_H);

			// ...and its top is INSIDE the canopy, not below it. The two widest canopy
			// layers sit at the top two blocks of the trunk, so the trunk's own leaves
			// would replace them if they were written over anything but air — the tree
			// would keep its height and lose two blocks off the top of the visible trunk,
			// which no count of wood or leaves notices. Measured at seed 1616: dropping
			// the air guard turns 20 wood blocks into leaves, all of them trunk tops.
			// At the top wood block the canopy has radius 2, so both horizontal
			// neighbours are leaves; if the trunk had been eaten they would be air.
			// Skipped on the outermost columns, where the neighbour is outside the
			// generated region and reads as air for a reason that is not a bug.
			if (x >= 1 && x <= 46) {
				CHECK_QUIET(worldGet(&s_world, x - 1, highest, z) == BLOCK_LEAVES);
				CHECK_QUIET(worldGet(&s_world, x + 1, highest, z) == BLOCK_LEAVES);
			}
		}
	}
	CHECK(trunks >= 4);

	// **Decoration must never touch the ground.** Every one of the 2,304 columns still has
	// its full terrain stack: the right cap block on top and the right material under it.
	// A canopy overhanging a rise can land exactly on a neighbour's top block — the ground
	// two blocks away can be two blocks higher, which is where the lowest canopy layer sits
	// — so the "only into air" guard on leaves is load-bearing, not decorative, and this is
	// the check that notices when it is dropped.
	for (int x = 0; x < 48; x++) {
		for (int z = 0; z < 48; z++) {
			const int  h     = worldgenHeight(&g, x, z);
			const bool sandy = worldgenIsSandy(&g, x, z);
			CHECK_QUIET(worldGet(&s_world, x, h - 1, z) == (sandy ? BLOCK_SAND : BLOCK_GRASS));
			for (int d = 1; d <= GEN_DIRT_DEPTH; d++)
				CHECK_QUIET(worldGet(&s_world, x, h - 1 - d, z) ==
				            (sandy ? BLOCK_SAND : BLOCK_DIRT));
			for (int y = 0; y < h; y++)
				CHECK_QUIET(worldGet(&s_world, x, y, z) != BLOCK_LEAVES);
		}
	}

	// The region must contain both biomes, or the layering check above only ever tested one
	// of the two rules and the seed is doing the test's job badly.
	int sandy_cols = 0;
	for (int x = 0; x < 48; x++)
		for (int z = 0; z < 48; z++)
			if (worldgenIsSandy(&g, x, z)) sandy_cols++;
	CHECK(sandy_cols > 40 && sandy_cols < 48 * 48 - 40);

	const uint32_t centre_with_neighbours = colHash(1, 1);
	worldExit(&s_world);

	// **The cross-column property.** A tree whose trunk stands in one column can reach into
	// the next, and columns are generated in whatever order the player walks. Generating the
	// middle column entirely on its own must produce exactly the blocks it has when its
	// eight neighbours were generated first — every canopy that overhangs it present, and
	// nothing of its own lost. A decoration pass that wrote into its neighbours directly
	// would fail this in both directions at once.
	worldInit(&s_world);
	CHECK(worldgenColumn(&g, &s_world, 1, 1));
	CHECK(colHash(1, 1) == centre_with_neighbours);

	// And it must have touched nothing else. A tree that wrote its far side straight into
	// the neighbouring column would allocate chunks outside the loaded ring — memory spent
	// on ground the player is nowhere near, in a game with a 12 MB block budget — and would
	// have that half of itself erased the moment that column was generated for real, since
	// worldgenColumn clears before it fills.
	CHECK(s_world.columns == 1);

	// The comparison has teeth only if the isolated column actually contains overhang. A
	// column with no tree blocks in it would match trivially.
	int lone_leaves = 0;
	for (int x = 16; x < 32; x++)
		for (int y = 0; y < WORLD_HEIGHT; y++)
			for (int z = 16; z < 32; z++)
				if (worldGet(&s_world, x, y, z) == BLOCK_LEAVES) lone_leaves++;
	CHECK(lone_leaves >= 20);

	// And the hash can see a one-block difference, so the match above is not vacuous.
	CHECK(worldSet(&s_world, 20, 100, 20, BLOCK_LEAVES));
	CHECK(colHash(1, 1) != centre_with_neighbours);

	worldExit(&s_world);
}

// Columns each way for the cave test: 5x5, the same area main.c generates around the
// player. Named rather than written as 2 in four places because the test's measured bounds
// are bounds for THIS area — shrink it and they stop meaning anything (see below).
#define GEN_CAVE_TEST_R 2

static void testWorldgenCaves(void)
{
	// The shipped seed and the shipped generation radius, so this test is about the world
	// the player actually gets. **The area matters as much as the seed here.** The first
	// version of this test used the 3x3 columns the tree test uses, and the mutation that
	// collapses the two cave fields into one PASSED it: over 16 seeds the hollow share of a
	// 3x3 region ranges 2.26 % .. 11.32 %, which overlaps what the broken generator produces
	// (13.34 % .. 28.42 %). Over the 5x5 area the same seeds sit at 4.15 % .. 8.40 % against
	// a mutant 21.91 %, and the two stop overlapping. A small sample did not make the test
	// weaker in an obvious way — it made a real defect invisible.
	WorldGen g;
	worldgenInit(&g, 1337u);

	worldInit(&s_world);
	for (int cz = -GEN_CAVE_TEST_R; cz <= GEN_CAVE_TEST_R; cz++)
		for (int cx = -GEN_CAVE_TEST_R; cx <= GEN_CAVE_TEST_R; cx++)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	long underground = 0, hollow = 0, deep_hollow = 0, lonely = 0;
	int  shallowest = WORLD_HEIGHT;
	for (int32_t x = -GEN_CAVE_TEST_R * CHUNK_DIM;
	     x < (GEN_CAVE_TEST_R + 1) * CHUNK_DIM; x++) {
		for (int32_t z = -GEN_CAVE_TEST_R * CHUNK_DIM;
		     z < (GEN_CAVE_TEST_R + 1) * CHUNK_DIM; z++) {
			const int h = worldgenHeight(&g, x, z);

			// **The cap is never breached.** Grass, the three dirt blocks and one of stone
			// stay solid whatever the cave field says, so no grass block is left floating on
			// nothing, the spawn point cannot open under the player's feet, and the rule the
			// rest of the world relies on — the first solid block walking down from the sky
			// is at worldgenHeight() - 1 — still holds with caves in.
			for (int d = 0; d < GEN_CAVE_MIN_DEPTH; d++)
				CHECK_QUIET(worldGet(&s_world, x, h - 1 - d, z) != BLOCK_AIR);

			// And the bottom of the world is floor, not a hole into nothing.
			CHECK_QUIET(worldGet(&s_world, x, 0, z) != BLOCK_AIR);

			for (int y = 0; y < h; y++) {
				underground++;
				if (worldGet(&s_world, x, y, z) != BLOCK_AIR)
					continue;
				hollow++;
				if (y < 24) deep_hollow++;
				const int depth = h - 1 - y;
				if (depth < shallowest) shallowest = depth;

				// A hollow block with no hollow neighbour is a one-block bubble rather than
				// a passage. Counted in the same pass because the pass is 800,000 blocks and
				// this test runs on the console at boot as well as on the PC.
				const bool touched =
					worldGet(&s_world, x + 1, y, z) == BLOCK_AIR ||
					worldGet(&s_world, x - 1, y, z) == BLOCK_AIR ||
					worldGet(&s_world, x, y + 1, z) == BLOCK_AIR ||
					worldGet(&s_world, x, y - 1, z) == BLOCK_AIR ||
					worldGet(&s_world, x, y, z + 1) == BLOCK_AIR ||
					worldGet(&s_world, x, y, z - 1) == BLOCK_AIR;
				if (!touched) lonely++;
			}
		}
	}

	// **There are caves at all, and not so many that the ground is a sponge.** Measured over
	// six seeds (1337, 1616, 0xBEEF, 4242, 7, 20260818) the hollow share of this area ran
	// 4.15 % .. 8.40 %, and 6.99 % on the shipped one. The bounds sit wide of that on both
	// sides because they are here to catch a generator that stopped carving or started
	// carving everything, not to pin the figure. The upper bound is what notices the two
	// noise fields being collapsed into one: a single band of this width takes 21.91 %.
	CHECK(hollow * 100 > underground * 2);
	CHECK(hollow * 100 < underground * 12);

	// The deep chunks are carved too. They are filled by a different branch of
	// worldgenColumn — one memset of stone, then the cave pass — and it would be entirely
	// possible to carve the surface-adjacent chunks correctly and leave that branch solid.
	CHECK(deep_hollow > 100);

	// The depth guard binds rather than merely being satisfied: the shallowest cave in the
	// area sits exactly at GEN_CAVE_MIN_DEPTH, so the checks above and this one are
	// measuring a rule that is actually doing work.
	CHECK(shallowest == GEN_CAVE_MIN_DEPTH);

	// **Caves are passages, not speckle.** Measured across the same six seeds the lonely
	// share ran 0.03 % .. 0.23 %, so 2 % is a wide bound that a field sampled at one block
	// per lattice cell fails instantly (65.81 % lonely). This stands in for the flood fill
	// that picked the constants — 0.05 puts 98.9 %+ of carved volume into systems bigger
	// than 100 blocks — because an 80x80x128 visited map would be shipped .bss in the
	// console binary, and the fill therefore lives in the probe instead.
	CHECK(lonely * 100 <= hollow * 2);

	worldExit(&s_world);

	// The field itself: same seed same answer, different seed different caves. Two separate
	// WorldGens rather than the same one twice, so this catches a generator that cached
	// state instead of hashing a position — which is the thing that would break the moment
	// Phase 6 regenerates a column in a different order.
	WorldGen same, other;
	worldgenInit(&same, 1337u);
	worldgenInit(&other, 1338u);

	int agree = 0, differs = 0, caves = 0, floor_caves = 0;
	for (int i = 0; i < 4000; i++) {
		const int32_t x = i * 7 - 14000;
		const int32_t z = i * 13 - 26000;
		const int     y = GEN_CAVE_FLOOR + i % 40;
		const bool    a = worldgenIsCave(&g, x, y, z);
		if (a == worldgenIsCave(&same,  x, y, z)) agree++;
		if (a != worldgenIsCave(&other, x, y, z)) differs++;
		if (a) caves++;
		if (worldgenIsCave(&g, x, GEN_CAVE_FLOOR - 1, z)) floor_caves++;
	}
	CHECK(agree == 4000);
	CHECK(caves > 4000 / 50);          // the sample contains caves, so `agree` means something
	CHECK(differs > caves / 2);        // and the seed moves them
	CHECK(floor_caves == 0);           // nothing below the floor, ever
}

// Step 5.5's job ring. Same treatment as the dirty queue: the accounting is checked after
// every operation, because a queue whose count drifts from its contents is the exact bug
// Phase 4 shipped and could not see.
static void testJobQueue(void)
{
	JobQueue q;
	jobqInit(&q);
	CHECK(jobqConsistent(&q));
	CHECK(jobqCount(&q) == 0);
	CHECK(jobqPeak(&q) == 0);
	CHECK(jobqPushed(&q) == 0);
	CHECK(jobqDropped(&q) == 0);

	// Popping an empty queue must not touch the caller's job. The worker loop reads `out`
	// only when the pop succeeded, so a pop that scribbles on failure would be invisible
	// there and would surface as a phantom job the first time that changes.
	Job sentinel = {JOB_SAVE, 111, 222, 3};
	Job out = sentinel;
	CHECK(!jobqPop(&q, &out));
	CHECK(out.type == sentinel.type);
	CHECK(out.cx == sentinel.cx);
	CHECK(out.cz == sentinel.cz);
	CHECK(out.cy == sentinel.cy);

	// FIFO, and the payload survives the trip.
	for (int i = 0; i < 3; i++) {
		const Job j = {JOB_GENERATE, i, -i, 0};
		CHECK(jobqPush(&q, j));
	}
	CHECK(jobqCount(&q) == 3);
	CHECK(jobqPeak(&q) == 3);
	CHECK(jobqConsistent(&q));
	for (int i = 0; i < 3; i++) {
		CHECK(jobqPop(&q, &out));
		CHECK(out.type == JOB_GENERATE && out.cx == i && out.cz == -i);
	}
	CHECK(jobqCount(&q) == 0);
	CHECK(jobqPeak(&q) == 3);            // the peak is a high-water mark, not the live count
	CHECK(jobqConsistent(&q));

	// ...and it has to survive a later, *smaller* burst. Draining to zero and reading the
	// peak above does not prove that, because nothing writes the peak on a pop: only a push
	// taken while the count is below the mark can tell `peak = count` apart from
	// `peak = max`. Written after a mutation run where breaking jobqPush that exact way
	// left the whole suite green. The number matters — step 6.2 sizes the per-frame job
	// budget from the peak, and a peak that tracked the live count would read lowest
	// exactly after a spike had drained.
	const Job small = {JOB_GENERATE, 42, -42, 1};
	CHECK(jobqPush(&q, small));
	CHECK(jobqCount(&q) == 1);
	CHECK(jobqPeak(&q) == 3);
	CHECK(jobqPop(&q, &out));
	CHECK(out.cx == 42 && out.cz == -42);
	CHECK(jobqCount(&q) == 0);
	CHECK(jobqPeak(&q) == 3);
	CHECK(jobqConsistent(&q));

	// Wrap-around. 5 in flight at a time for 500 rounds is four full laps of a 128-slot
	// ring, so head and tail each pass zero several times while the queue is non-empty —
	// the case a push/pop pair that only moves one index gets away with when the ring is
	// drained to zero between operations.
	int wrap_bad = 0;
	for (int i = 0; i < 5; i++) {
		const Job j = {JOB_MESH, i, i, i & 7};
		CHECK_QUIET(jobqPush(&q, j));
	}
	for (int i = 5; i < 500; i++) {
		CHECK_QUIET(jobqPop(&q, &out));
		if (out.cx != i - 5 || out.cy != ((i - 5) & 7) || out.type != JOB_MESH) wrap_bad++;
		const Job j = {JOB_MESH, i, i, i & 7};
		CHECK_QUIET(jobqPush(&q, j));
		CHECK_QUIET(jobqConsistent(&q));
	}
	CHECK(wrap_bad == 0);
	CHECK(jobqCount(&q) == 5);
	CHECK(jobqConsistent(&q));

	// Full. The ring refuses rather than overwriting, and says how many it refused.
	jobqInit(&q);
	int accepted = 0;
	for (int i = 0; i < JOBQ_CAP + 10; i++) {
		const Job j = {JOB_LOAD, i, 0, 0};
		if (jobqPush(&q, j)) accepted++;
	}
	CHECK(accepted == JOBQ_CAP);
	CHECK(jobqCount(&q) == JOBQ_CAP);
	CHECK(jobqDropped(&q) == 10);
	CHECK(jobqPushed(&q) == JOBQ_CAP);
	CHECK(jobqPeak(&q) == JOBQ_CAP);
	CHECK(jobqConsistent(&q));

	// And the ten it refused are the ten that are missing — the survivors are the first
	// JOBQ_CAP pushed, in order, not a scrambled ring.
	int order_bad = 0;
	for (int i = 0; i < JOBQ_CAP; i++) {
		CHECK_QUIET(jobqPop(&q, &out));
		if (out.type != JOB_LOAD || out.cx != i) order_bad++;
	}
	CHECK(order_bad == 0);
	CHECK(jobqCount(&q) == 0);
	CHECK(!jobqPop(&q, &out));
	CHECK(jobqConsistent(&q));
}

// ---------------------------------------------------------------------------------------
// Step 7.3, cave culling. Two things are being tested and only one of them is "does it cull":
// the far more important property is that it never culls something it should have kept, and
// most of what follows is aimed at that.

// 35 KB and 18 KB. Static, because the 3DS main thread's stack is 32 KB and a test that only
// runs on the host is still the wrong place to learn that.
static VisWalk    s_walk;
static VisScratch s_vis_scratch;
static Chunk      s_vis_chunk;

static void testVisPairIndex(void)
{
	// Fifteen distinct indices covering 0..14 exactly once, symmetric, and -1 on the
	// diagonal. visChunkConnectivity ORs bits by this number and visWalkRun reads them back
	// by it, so a collision here would silently join two faces that are not joined.
	int seen[VIS_PAIRS] = {0};
	int diagonal_wrong  = 0;
	int asymmetric      = 0;
	int out_of_range    = 0;

	for (int a = 0; a < BLOCK_FACES; a++) {
		for (int b = 0; b < BLOCK_FACES; b++) {
			const int i = visPairIndex(a, b);
			if (a == b) { if (i != -1) diagonal_wrong++; continue; }
			if (i != visPairIndex(b, a)) asymmetric++;
			if (i < 0 || i >= VIS_PAIRS) { out_of_range++; continue; }
			if (a < b) seen[i]++;
		}
	}
	CHECK(diagonal_wrong == 0);
	CHECK(asymmetric == 0);
	CHECK(out_of_range == 0);

	int covered = 0;
	for (int i = 0; i < VIS_PAIRS; i++)
		if (seen[i] == 1) covered++;
	CHECK(covered == VIS_PAIRS);

	// The walk turns an entry face into the neighbour's entry face with face ^ 1, which is
	// only correct because block.h writes the faces in +/- pairs. That is a contract between
	// two files, so it is asserted rather than assumed.
	CHECK((FACE_EAST ^ 1) == FACE_WEST);
	CHECK((FACE_WEST ^ 1) == FACE_EAST);
	CHECK((FACE_TOP ^ 1) == FACE_BOTTOM);
	CHECK((FACE_BOTTOM ^ 1) == FACE_TOP);
	CHECK((FACE_SOUTH ^ 1) == FACE_NORTH);
	CHECK((FACE_NORTH ^ 1) == FACE_SOUTH);
}

static void testVisConnectivity(void)
{
	// Solid rock joins nothing.
	chunkClear(&s_vis_chunk, BLOCK_STONE);
	CHECK(visChunkConnectivity(&s_vis_chunk, &s_vis_scratch) == 0);

	// Open sky joins everything — all fifteen pairs, and no bit above them.
	chunkClear(&s_vis_chunk, BLOCK_AIR);
	const uint16_t open = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);
	CHECK(open == 0x7FFF);

	// A straight east-west tunnel through solid rock joins east to west and nothing else.
	// This is the case the whole algorithm exists for: every other face of this chunk is
	// stone, so a camera to the north of it can see nothing beyond it.
	chunkClear(&s_vis_chunk, BLOCK_STONE);
	for (int x = 0; x < CHUNK_DIM; x++)
		s_vis_chunk.blocks[chunkIndex(x, 8, 8)] = BLOCK_AIR;

	const uint16_t tunnel = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);
	CHECK(visConnected(tunnel, FACE_EAST, FACE_WEST));
	CHECK(!visConnected(tunnel, FACE_TOP, FACE_BOTTOM));
	CHECK(!visConnected(tunnel, FACE_EAST, FACE_TOP));
	CHECK(!visConnected(tunnel, FACE_NORTH, FACE_SOUTH));
	CHECK(tunnel == (uint16_t)(1u << visPairIndex(FACE_EAST, FACE_WEST)));

	// Add a vertical shaft that does not meet the tunnel. Two separate components, so the
	// mask gains top-bottom and must NOT gain east-top: a fill that leaked between the two
	// would report a sight line that does not exist and stop the cull working at all.
	for (int y = 0; y < CHUNK_DIM; y++)
		s_vis_chunk.blocks[chunkIndex(2, y, 2)] = BLOCK_AIR;

	const uint16_t both = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);
	CHECK(visConnected(both, FACE_EAST, FACE_WEST));
	CHECK(visConnected(both, FACE_TOP, FACE_BOTTOM));
	CHECK(!visConnected(both, FACE_EAST, FACE_TOP));
	CHECK(!visConnected(both, FACE_WEST, FACE_BOTTOM));

	// Now join them, and the pairs that were absent must appear. Without this the test above
	// would pass just as well against a fill that never connects anything.
	for (int z = 2; z <= 8; z++)
		s_vis_chunk.blocks[chunkIndex(2, 8, z)] = BLOCK_AIR;

	const uint16_t joined = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);
	CHECK(visConnected(joined, FACE_EAST, FACE_TOP));
	CHECK(visConnected(joined, FACE_WEST, FACE_BOTTOM));

	// Step 7.5 made leaves transparent, and this check flipped with it: it used to assert 0,
	// on the grounds that an opaque canopy blocks sight exactly like stone, and now asserts
	// fully connected because sight passes through one. That the same line could flip by
	// changing one field in the block registry is the point of writing the fill against
	// solid && !transparent rather than against solid — leaves are still solid, and still
	// stop the player walking through them.
	chunkClear(&s_vis_chunk, BLOCK_LEAVES);
	CHECK(visChunkConnectivity(&s_vis_chunk, &s_vis_scratch) == 0x7FFF);
	CHECK(blockIsSolid(BLOCK_LEAVES));
}

// Fills a walk box of nx x 1 x 1 chunks, every cell drawable, and returns how many the walk
// reached. `middle_mask` is the connectivity of the cell between the camera and the far end.
static int visLineWalk(uint16_t middle_mask, bool set_middle)
{
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 3, 1, 1));
	visWalkSet(&s_walk, 0, 0, 0, VIS_ALL_CONNECTED, true);
	if (set_middle) visWalkSet(&s_walk, 1, 0, 0, middle_mask, true);
	visWalkSet(&s_walk, 2, 0, 0, VIS_ALL_CONNECTED, true);

	// Camera in the middle of chunk 0, looking along +X.
	return visWalkRun(&s_walk, 8.0f, 8.0f, 8.0f);
}

static void testVisWalk(void)
{
	// A wall of rock in the middle: the far chunk is unreachable, so it must not be drawn.
	CHECK(visLineWalk(0, true) == 2);
	CHECK(visWalkVisible(&s_walk, 0, 0, 0));
	CHECK(visWalkVisible(&s_walk, 1, 0, 0));    // the wall itself is on screen
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0));   // what is behind it is not

	// The same box with the wall open. If this did not change, the check above would be
	// proving nothing — it would pass against a walk that culled everything.
	CHECK(visLineWalk(VIS_ALL_CONNECTED, true) == 3);
	CHECK(visWalkVisible(&s_walk, 2, 0, 0));

	// ...and a coordinate that was never inserted at all must behave like open sky, not like
	// a wall. main.c never meshes an all-air chunk, so the sky above the terrain is a hole in
	// the mesh list; a hole that stopped the walk would cull the world from head height up.
	CHECK(visLineWalk(0, false) == 2);          // only two cells are drawable now
	CHECK(visWalkVisible(&s_walk, 2, 0, 0));
	CHECK(!visWalkVisible(&s_walk, 1, 0, 0));   // nothing there to draw

	// A tunnel that runs east-west lets sight through along x and stops it going up. This is
	// the connectivity mask actually steering the walk rather than the walk ignoring it.
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 2, 2, 1));
	visWalkSet(&s_walk, 0, 0, 0, VIS_ALL_CONNECTED, true);
	visWalkSet(&s_walk, 1, 0, 0, (uint16_t)(1u << visPairIndex(FACE_EAST, FACE_WEST)), true);
	visWalkSet(&s_walk, 0, 1, 0, 0, true);
	visWalkSet(&s_walk, 1, 1, 0, VIS_ALL_CONNECTED, true);
	CHECK(visWalkRun(&s_walk, 8.0f, 8.0f, 8.0f) == 3);
	CHECK(visWalkVisible(&s_walk, 1, 0, 0));
	CHECK(visWalkVisible(&s_walk, 0, 1, 0));    // entered straight up out of the camera cell
	CHECK(!visWalkVisible(&s_walk, 1, 1, 0));   // only reachable through two closed faces

	// The camera's own cell is always visible, even when it is solid rock in every direction.
	// Over-culling here would blank the screen from inside a wall.
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 3, 1, 1));
	visWalkSet(&s_walk, 0, 0, 0, 0, true);
	visWalkSet(&s_walk, 1, 0, 0, 0, true);
	visWalkSet(&s_walk, 2, 0, 0, 0, true);
	CHECK(visWalkRun(&s_walk, 8.0f, 8.0f, 8.0f) == 2);   // itself, plus the wall it faces
	CHECK(visWalkVisible(&s_walk, 0, 0, 0));

	// Standing in the middle of an open box, everything is visible: the walk must be able to
	// leave in both directions along an axis when the camera is inside that slab.
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 3, 1, 1));
	for (int i = 0; i < 3; i++) visWalkSet(&s_walk, i, 0, 0, VIS_ALL_CONNECTED, true);
	CHECK(visWalkRun(&s_walk, 24.0f, 8.0f, 8.0f) == 3);

	// A camera outside the box enters at the nearest cell rather than being lost. The player
	// standing at the edge of the loaded area is this case every time the ring recentres.
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 3, 1, 1));
	for (int i = 0; i < 3; i++) visWalkSet(&s_walk, i, 0, 0, VIS_ALL_CONNECTED, true);
	CHECK(visWalkRun(&s_walk, -100.0f, 8.0f, 8.0f) == 3);

	// A negative camera coordinate must floor, not truncate: at x = -1.5 the camera is in
	// chunk -1, and answering chunk 0 would start the walk one cell too far east and cull
	// whatever is behind it.
	CHECK_QUIET(visWalkBegin(&s_walk, -1, 0, 0, 2, 1, 1));
	visWalkSet(&s_walk, -1, 0, 0, VIS_ALL_CONNECTED, true);
	visWalkSet(&s_walk,  0, 0, 0, 0, true);
	CHECK(visWalkRun(&s_walk, -1.5f, 8.0f, 8.0f) == 2);
	CHECK(visWalkVisible(&s_walk, -1, 0, 0));

	// A box bigger than the scratch is refused rather than silently truncated. The caller's
	// only safe response is to draw everything, so this has to be reported and not guessed.
	CHECK(!visWalkBegin(&s_walk, 0, 0, 0, VIS_BOX_MAX_XZ + 1, 1, 1));
	CHECK(!visWalkBegin(&s_walk, 0, 0, 0, 1, VIS_BOX_MAX_Y + 1, 1));
	CHECK(!visWalkBegin(&s_walk, 0, 0, 0, 1, 1, 0));
	CHECK(visWalkBegin(&s_walk, 0, 0, 0, VIS_BOX_MAX_XZ, VIS_BOX_MAX_Y, VIS_BOX_MAX_XZ));

	// And the full-size box does not walk off the end of its queue: every cell open, camera
	// in a corner, which is the most pushes the walk can ever generate.
	for (int y = 0; y < VIS_BOX_MAX_Y; y++)
		for (int z = 0; z < VIS_BOX_MAX_XZ; z++)
			for (int x = 0; x < VIS_BOX_MAX_XZ; x++)
				visWalkSet(&s_walk, x, y, z, VIS_ALL_CONNECTED, true);
	CHECK(visWalkRun(&s_walk, 0.0f, 0.0f, 0.0f) ==
	      VIS_BOX_MAX_XZ * VIS_BOX_MAX_Y * VIS_BOX_MAX_XZ);
}

// The property that matters most: the walk must never cull a chunk that a straight, purely
// axis-aligned open path reaches. Built from real terrain rather than a hand-made box, so it
// exercises the fill and the walk together.
static void testVisWalkNeverHidesOpenSky(void)
{
	CHECK_QUIET(visWalkBegin(&s_walk, 0, 0, 0, 4, VIS_BOX_MAX_Y, 4));

	// Solid ground in the bottom two chunk layers, open air above. Every air chunk above the
	// ground, and the ground layer itself, has to survive.
	chunkClear(&s_vis_chunk, BLOCK_STONE);
	const uint16_t rock = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);
	chunkClear(&s_vis_chunk, BLOCK_AIR);
	const uint16_t sky = visChunkConnectivity(&s_vis_chunk, &s_vis_scratch);

	for (int y = 0; y < VIS_BOX_MAX_Y; y++)
		for (int z = 0; z < 4; z++)
			for (int x = 0; x < 4; x++)
				visWalkSet(&s_walk, x, y, z, (y < 2) ? rock : sky, true);

	// Camera standing on the ground at the corner of the box, eye height inside chunk y = 2.
	const int reached = visWalkRun(&s_walk, 8.0f, 34.0f, 8.0f);

	int missing = 0;
	for (int y = 2; y < VIS_BOX_MAX_Y; y++)
		for (int z = 0; z < 4; z++)
			for (int x = 0; x < 4; x++)
				if (!visWalkVisible(&s_walk, x, y, z)) missing++;
	CHECK(missing == 0);

	// The rock underneath is not all culled either — the layer directly below the camera is
	// entered through its top face — but the layer below *that* is sealed off and must go, or
	// the walk is not culling anything at all.
	CHECK(visWalkVisible(&s_walk, 0, 1, 0));
	CHECK(!visWalkVisible(&s_walk, 0, 0, 0));
	CHECK(reached < 4 * VIS_BOX_MAX_Y * 4);
}

// Step 7.7. The render distance setting is arithmetic — a radius in, a near plane, a fog
// density and a slot count out — and arithmetic is exactly what can be checked here rather
// than by looking at the console and deciding the fog "seems about right".
static void testRenderDist(void)
{
	// The ends of the range hold, and out-of-range clamps instead of returning nonsense: this
	// is a setting a player nudges with a shoulder button, and one that silently produced a
	// radius of 0 at the bottom would blank the world.
	CHECK(renderDistFor(RENDER_DIST_MIN).radius == RENDER_DIST_MIN);
	CHECK(renderDistFor(RENDER_DIST_MAX).radius == RENDER_DIST_MAX);
	CHECK(renderDistFor(0).radius == RENDER_DIST_MIN);
	CHECK(renderDistFor(-5).radius == RENDER_DIST_MIN);
	CHECK(renderDistFor(99).radius == RENDER_DIST_MAX);

	// Defaults differ by console, which is the whole point of the step's second half.
	CHECK(renderDistDefault(false) == RENDER_DIST_MIN);
	CHECK(renderDistDefault(true) == RENDER_DIST_MAX);

	const RenderDist lo = renderDistFor(RENDER_DIST_MIN);
	const RenderDist hi = renderDistFor(RENDER_DIST_MAX);

	// The generated ring is always one column wider than the meshed one. main.c depends on
	// this to keep the mesher from reading an unloaded neighbour as air.
	CHECK(lo.area_radius == lo.radius + 1);
	CHECK(hi.area_radius == hi.radius + 1);

	// Slots: 9 columns and 25 columns at six chunks each. The pool is claimed for the maximum
	// at boot, so this number is what decides whether the setting is affordable at all.
	CHECK(lo.slots == 9 * RENDER_DIST_SLOTS_PER_COLUMN);
	CHECK(hi.slots == 25 * RENDER_DIST_SLOTS_PER_COLUMN);
	CHECK(hi.slots > lo.slots);

	// Radius 1 keeps the near plane it has always had, so raising the setting is the only
	// thing that can change the view near the camera.
	CHECK(lo.near_plane > 0.0999f && lo.near_plane < 0.1001f);

	// The near plane's corner must stay closer than the player's half-width, or a player
	// pressed to a wall sees through it. 1.5917 is sqrt(1 + tan^2(fovy/2) + tan^2(fovx/2)) at
	// 65 degrees and 400/240. This is the check that stops the near plane being raised further
	// to buy more fog range.
	CHECK(hi.near_plane <= RENDER_DIST_NEAR_MAX);
	CHECK(RENDER_DIST_NEAR_MAX * 1.5917f < 0.3f);

	// The load boundary is radius x 16 blocks in the worst case, and the fog must reach the
	// target by then at every setting. This is the criterion step 6.4 established and it has
	// to survive the setting becoming variable.
	CHECK(lo.boundary > 15.9f && lo.boundary < 16.1f);
	CHECK(hi.boundary > 31.9f && hi.boundary < 32.1f);
	CHECK(lo.fog_hides);
	CHECK(hi.fog_hides);
	CHECK(renderDistVisibility(&lo, lo.boundary) <= RENDER_DIST_TARGET_VIS * 1.01f);
	CHECK(renderDistVisibility(&hi, hi.boundary) <= RENDER_DIST_TARGET_VIS * 1.01f);

	// The knot the whole design turns on: with near 0.1 the first LUT sample past the far
	// plane is at 12.03 blocks, and raising near moves it out. If this ever stops being true
	// the fog constants are being solved against a curve the hardware is not drawing.
	CHECK(lo.first_knot > 12.0f && lo.first_knot < 12.1f);
	CHECK(hi.first_knot > lo.first_knot);
	CHECK(renderDistLutZ(0.0f, 0.1f, 200.0f) > 199.9f);        // entry 0 is the far plane
	CHECK(renderDistLutZ(1.0f, 0.1f, 200.0f) < 0.1001f);       // entry 128 is the near plane

	// Fog is monotone: nearer is always clearer, at both settings. A LUT built from a curve
	// that folded back on itself would read as a bright ring at a fixed distance.
	for (float z = 1.0f; z < 40.0f; z += 0.5f) {
		CHECK_QUIET(renderDistVisibility(&lo, z) >= renderDistVisibility(&lo, z + 0.5f) - 1e-6f);
		CHECK_QUIET(renderDistVisibility(&hi, z) >= renderDistVisibility(&hi, z + 0.5f) - 1e-6f);
	}

	// And the payoff, such as it is: a wider ring does let the player see further, because
	// `near` moved with it. Sublinear by a long way — the ring doubles and the half-fade
	// distance goes up by about half — which is the LUT's shape, not a bug, and is why
	// RENDER_DIST_MAX is 2 rather than 4.
	CHECK(hi.half_vis > lo.half_vis * 1.2f);
	CHECK(hi.half_vis < hi.boundary);
	CHECK(lo.half_vis < lo.boundary);

	// Zero density is "no fog" through this same code path, which is what makes the
	// -DFOG_DENSITY=0.0f check able to go red rather than merely different.
	RenderDist off = lo;
	off.fog_density = 0.0f;
	CHECK(renderDistVisibility(&off, 100.0f) > 0.999f);
}

int worldTestRun(char* summary, size_t cap, int* checks_out)
{
	s_checks = 0;
	s_fails = 0;
	s_first[0] = '\0';
	budgetReset();

	testBlockRegistry();
	testBlockFaceTexFallback();
	testChunkIndex();
	testWorldAccess();
	testWorldBytes();
	testColumnTableFull();
	testColumnRemove();
	testWorldSetBudgetExhausted();
	testScratch26();
	testScratchFloor();
	testMesher();
	testMesherTransparentSplit();
	testMesherAO();
	testRemeshList();
	testIncrementalRemesh();
	testDirtyQueue();
	testJobQueue();
	testHandbuiltHeight();
	testHandbuiltFillShape();
	testHandbuiltWalkable();
	testEditPersistence();
	testControlFeel();
	testAllAirMeshesToNothing();
	testRng();
	testNoiseDeterminism();
	testWorldgenTerrain();
	testWorldgenDeterminism();
	testWorldgenBiome();
	testWorldgenTrees();
	testWorldgenCaves();
	testRaycast();
	testBodyBlocked();
	testCeilingCollision();
	testPhysics();
	testVisPairIndex();
	testVisConnectivity();
	testVisWalk();
	testVisWalkNeverHidesOpenSky();
	testRenderDist();

	// Every allocation the world made must have been given back.
	CHECK(budgetUsed() == 0);

	testBudget();

	if (summary && cap) {
		if (s_fails == 0)
			snprintf(summary, cap, "PASS  %d checks", s_checks);
		else
			snprintf(summary, cap, "FAIL %d/%d  %s", s_fails, s_checks, s_first);
	}
	if (checks_out) *checks_out = s_checks;

	budgetReset();
	return s_fails;
}
