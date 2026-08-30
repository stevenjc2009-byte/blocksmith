#include "world/world_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/atlas_uv.h"
#include "world/budget.h"
#include "world/chunk_codec.h"
#include "world/dirtyq.h"
#include "world/handbuilt.h"
#include "world/jobq.h"
#include "world/light.h"
#include "world/mesher.h"
#include "world/meshq.h"
#include "world/noise.h"
#include "world/physics.h"
#include "world/rng.h"
#include "world/raycast.h"
#include "world/registry.h"
#include "world/relightq.h"
#include "world/tick.h"
#include "world/remesh.h"
#include "world/scratch.h"
#include "scene/render_dist.h"
#include "world/visgraph.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_density.h"
#include "world/genversion.h"
#include "world/genrefuse.h"

// Big enough to matter on a 32 KB console stack, and only ever one of each.
static World       s_world;
static MeshScratch s_scratch;

static int  s_checks;
static int  s_fails;
static char s_first[96];

// Every failing check, printed as it happens — off unless asked for.
//
// The harness only ever reported s_first plus a count. That is enough to know a run went red,
// and not enough to do the one thing this project keeps having to relearn: in a sabotage arm,
// READ EVERY CASE. A count cannot tell you that an arm you thought broke five things actually
// broke four and neutralised the fifth, and s_first cannot tell you WHICH four. Compile with
// -DWORLD_TEST_VERBOSE to get the list. Nothing about the default build changes.
#ifdef WORLD_TEST_VERBOSE
#define CHECK_REPORT(expr) fprintf(stderr, "FAIL L%d %s\n", __LINE__, (expr))
#else
#define CHECK_REPORT(expr) ((void)0)
#endif

#define CHECK(cond) do {                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			CHECK_REPORT(#cond);                                                \
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
			CHECK_REPORT(#cond);                                                \
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

	// Step 9.2a made Chunk opaque, so sizeof(Chunk) == CHUNK_BLOCKS is no longer even
	// expressible here (Chunk has no complete type outside chunk.c) — the exact byte cost
	// of each storage form is asserted in testChunkFormBytes below instead. What is still
	// this function's business is that the three forms are strictly ordered, since every
	// promotion decision in chunk.c assumes UNIFORM < PALETTE4 < RAW.
	CHECK(chunkFormBytes(CHUNK_FORM_UNIFORM) < chunkFormBytes(CHUNK_FORM_PALETTE4));
	CHECK(chunkFormBytes(CHUNK_FORM_PALETTE4) < chunkFormBytes(CHUNK_FORM_RAW));
}

// ---------------------------------------------------------------------------------------
// Step 9.2a (palette-compressed storage) / 9.1b (uniform-chunk flags). These six tests all
// exercise chunk.c's own API directly with a bare chunkAlloc'd Chunk* -- no World, no budget
// -- because chunk.c has zero dependency on world/budget.h by design (see chunk.h's file
// header): the storage-form logic must be correct on its own terms before anything asks
// whether a promotion can be afforded.

// The exact byte cost of each form, pinned as literal numbers rather than left as "some
// value bigger than the last" (testChunkIndex already checks the ordering). These specific
// numbers are what a promotion's budget delta is computed from in world.c and chunk_codec.c,
// so a silent change here would silently change what every promotion costs.
//   UNIFORM:   sizeof(struct Chunk) alone (form + uniform_id + a payload pointer) — no array.
//   PALETTE4:  + a 16-slot BlockId palette, one pal_n byte, and 2,048 nibble-packed indices
//              (4 bits * 4,096 cells) = 16 + 1 + 2,048 = 2,065 bytes on top of UNIFORM.
//   RAW:       + one BlockId per cell = CHUNK_BLOCKS bytes on top of UNIFORM.
static void testChunkFormBytes(void)
{
	const size_t hdr = chunkFormBytes(CHUNK_FORM_UNIFORM);
	CHECK(chunkFormBytes(CHUNK_FORM_PALETTE4) == hdr + 16 + 1 + 2048);
	CHECK(chunkFormBytes(CHUNK_FORM_RAW)      == hdr + (size_t)CHUNK_BLOCKS);
	CHECK(chunkFormBytes(CHUNK_FORM_PALETTE4) - chunkFormBytes(CHUNK_FORM_UNIFORM)  == 2065);
	CHECK(chunkFormBytes(CHUNK_FORM_RAW)      - chunkFormBytes(CHUNK_FORM_UNIFORM)  == 4096);
	CHECK(chunkFormBytes(CHUNK_FORM_RAW)      - chunkFormBytes(CHUNK_FORM_PALETTE4) == 2031);
}

// A freshly allocated chunk stores no array at all -- its cost is exactly the UNIFORM form's
// bytes, nothing more, and every cell reads back the fill id without ever touching a payload.
static void testChunkUniformNoArray(void)
{
	Chunk* c = chunkAlloc(BLOCK_STONE);
	CHECK(chunkGetForm(c) == CHUNK_FORM_UNIFORM);
	CHECK(chunkGetBytes(c) == chunkFormBytes(CHUNK_FORM_UNIFORM));
	CHECK(chunkIsUniform(c, BLOCK_STONE));
	CHECK(!chunkIsUniform(c, BLOCK_AIR));

	// Sparse sample rather than all 4,096 cells -- CHECK_QUIET exists for exactly this, but
	// a UNIFORM chunk's whole point is that every cell is the same answer, so a handful of
	// widely spaced points already proves it; testChunkRoundTripEveryFormEveryCell below is
	// where the full sweep belongs.
	for (int i = 0; i < CHUNK_BLOCKS; i += 137)
		CHECK_QUIET(chunkGet(c, i) == BLOCK_STONE);
	chunkFree(c);
}

// Writing a second, different id into a UNIFORM chunk promotes it to PALETTE4 -- and nothing
// else about the chunk's content moves: every cell that was not the write target still reads
// back the original uniform id.
static void testChunkPromoteUniformToPalette(void)
{
	Chunk* c = chunkAlloc(BLOCK_STONE);

	// Pre-flight query, checked before the write actually happens: the same id never
	// promotes, a different one always fits (two distinct ids always fit a 16-slot palette).
	CHECK(chunkFormFor(c, BLOCK_STONE) == CHUNK_FORM_UNIFORM);
	CHECK(chunkFormFor(c, BLOCK_DIRT)  == CHUNK_FORM_PALETTE4);

	CHECK(chunkSet(c, 42, BLOCK_DIRT));
	CHECK(chunkGetForm(c)  == CHUNK_FORM_PALETTE4);
	CHECK(chunkGetBytes(c) == chunkFormBytes(CHUNK_FORM_PALETTE4));
	CHECK(chunkGet(c, 42)  == BLOCK_DIRT);
	CHECK(chunkGet(c, 0)   == BLOCK_STONE);      // untouched cells: still the old uniform id
	CHECK(chunkGet(c, CHUNK_BLOCKS - 1) == BLOCK_STONE);
	chunkFree(c);
}

// A PALETTE4 chunk holding exactly 16 distinct ids stays PALETTE4; the 17th distinct id is
// the one write that has to fall back to RAW, and everything written before that moment must
// survive the fallback exactly.
static void testChunkPromotePaletteToRaw(void)
{
	Chunk* c = chunkAlloc((BlockId)1);

	// One id already there (the uniform fill), plus 15 more distinct writes: exactly 16,
	// still fits PALETTE4.
	for (int i = 0; i < 15; i++) CHECK(chunkSet(c, i, (BlockId)(i + 2)));
	CHECK(chunkGetForm(c) == CHUNK_FORM_PALETTE4);

	// The 17th distinct id promotes to RAW.
	CHECK(chunkFormFor(c, (BlockId)200) == CHUNK_FORM_RAW);
	CHECK(chunkSet(c, 4000, (BlockId)200));
	CHECK(chunkGetForm(c)  == CHUNK_FORM_RAW);
	CHECK(chunkGetBytes(c) == chunkFormBytes(CHUNK_FORM_RAW));

	for (int i = 0; i < 15; i++) CHECK(chunkGet(c, i) == (BlockId)(i + 2));
	CHECK(chunkGet(c, 4000) == (BlockId)200);
	CHECK(chunkGet(c, 4001) == (BlockId)1);      // never written: still the original uniform id
	chunkFree(c);
}

// A full 16x16x16 set-then-get sweep for each of the three forms, proving every (x, y, z)
// round-trips through chunkIndex/chunkSet/chunkGet correctly in that form specifically --
// not just a handful of sampled cells the way the other tests above check.
static void testChunkRoundTripEveryFormEveryCell(void)
{
	// UNIFORM: chunkGet alone, no chunkSet needed -- every cell is the fill id by
	// construction.
	{
		Chunk* c = chunkAlloc((BlockId)7);
		for (int i = 0; i < CHUNK_BLOCKS; i++) CHECK_QUIET(chunkGet(c, i) == (BlockId)7);
		chunkFree(c);
	}

	// PALETTE4: sweep ids 1..16 across every cell. The starting uniform id is 1 (one of the
	// 16 swept values), not BLOCK_AIR (0) -- starting from an id outside the swept set would
	// make 17 distinct ids total (the original plus 16 new ones) and silently overflow this
	// into RAW instead of exercising PALETTE4's full 16-slot capacity.
	{
		Chunk* c = chunkAlloc((BlockId)1);
		for (int i = 0; i < CHUNK_BLOCKS; i++)
			CHECK_QUIET(chunkSet(c, i, (BlockId)(1 + (i % 16))));
		CHECK(chunkGetForm(c) == CHUNK_FORM_PALETTE4);
		for (int i = 0; i < CHUNK_BLOCKS; i++)
			CHECK_QUIET(chunkGet(c, i) == (BlockId)(1 + (i % 16)));
		chunkFree(c);
	}

	// RAW: sweep ids 1..32 (32 > 16, so this stays in RAW throughout).
	{
		Chunk* c = chunkAlloc((BlockId)1);
		for (int i = 0; i < CHUNK_BLOCKS; i++)
			CHECK_QUIET(chunkSet(c, i, (BlockId)(1 + (i % 32))));
		CHECK(chunkGetForm(c) == CHUNK_FORM_RAW);
		for (int i = 0; i < CHUNK_BLOCKS; i++)
			CHECK_QUIET(chunkGet(c, i) == (BlockId)(1 + (i % 32)));
		chunkFree(c);
	}
}

// chunkCopyRun/chunkDecompressAll must read back the same bytes no matter which physical form
// a chunk happens to be in, for the same logical content. Three chunks are built holding
// IDENTICAL logical content (every cell BLOCK_STONE) but forced into three DIFFERENT physical
// forms via chunkSet's documented never-demote rule: writing a different id and then writing
// the original id straight back promotes the chunk and leaves it promoted, even though its
// content is once again uniform.
//
// This also pins a real, deliberate asymmetry in chunkIsUniform() between PALETTE4 and RAW
// (see chunk.c): PALETTE4's check is a cheap `pal_n == 1` shortcut that conservatively
// answers false for an orphaned-slot chunk like `pal` below, while RAW has no cheaper
// representation to shortcut through and so actually scans the array, giving the exact
// answer (true) even though the chunk's form never demoted. Getting this backwards here
// once already produced a real, reproducible failure in an earlier draft of this test
// (asserting `!chunkIsUniform(raw, BLOCK_STONE)`, which failed because the scan correctly
// found the RAW array WAS all BLOCK_STONE) -- recorded here so the asymmetry is never
// "fixed" back into a wrong assumption by a future edit.
static void testChunkRunCopyMatchesAcrossForms(void)
{
	Chunk* uni = chunkAlloc(BLOCK_STONE);

	Chunk* pal = chunkAlloc(BLOCK_STONE);
	CHECK(chunkSet(pal, 0, BLOCK_DIRT));         // promotes to PALETTE4
	CHECK(chunkSet(pal, 0, BLOCK_STONE));        // logically stone again; orphaned slot, stays PALETTE4
	CHECK(chunkGetForm(pal) == CHUNK_FORM_PALETTE4);

	Chunk* raw = chunkAlloc(BLOCK_STONE);
	// ids 100+i, well past BLOCK_COUNT (6), specifically so none of the 16 new ids can ever
	// collide with the chunk's own uniform fill id (BLOCK_STONE) no matter what block.h's
	// enum order happens to be. A small literal offset here was tried first (assuming
	// BLOCK_STONE == 2) and was wrong on both counts tried -- BLOCK_STONE is actually 3
	// (block.h's real order is AIR, GRASS, DIRT, STONE, ...) -- and each wrong guess was
	// caught for real by the official host suite as "FAIL ... chunkGetForm(raw) ==
	// CHUNK_FORM_RAW" (a collision wastes one iteration on a same-id no-op write, one short
	// of the 17 distinct ids needed to force RAW). An offset outside BLOCK_COUNT entirely
	// removes the need to know or guess any real block's numeric value.
	for (int i = 0; i < 16; i++) CHECK(chunkSet(raw, i, (BlockId)(100 + i)));   // 17th distinct...
	CHECK(chunkGetForm(raw) == CHUNK_FORM_RAW);                                // ...promotes here
	for (int i = 0; i < 16; i++) CHECK(chunkSet(raw, i, BLOCK_STONE));       // back to logical stone
	CHECK(chunkGetForm(raw) == CHUNK_FORM_RAW);

	CHECK(chunkIsUniform(uni, BLOCK_STONE));
	CHECK(!chunkIsUniform(pal, BLOCK_STONE));    // orphaned slot: the conservative false, by design
	CHECK(chunkIsUniform(raw, BLOCK_STONE));     // RAW has no shortcut: a real scan, the exact answer

	static const struct { int begin, count; } runs[] = {
		{0, 1}, {0, 16}, {2000, 16}, {4080, 16}, {0, CHUNK_BLOCKS},
	};
	static BlockId out_u[CHUNK_BLOCKS], out_p[CHUNK_BLOCKS], out_r[CHUNK_BLOCKS];
	for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
		chunkCopyRun(uni, runs[k].begin, runs[k].count, out_u);
		chunkCopyRun(pal, runs[k].begin, runs[k].count, out_p);
		chunkCopyRun(raw, runs[k].begin, runs[k].count, out_r);
		CHECK(memcmp(out_u, out_p, (size_t)runs[k].count) == 0);
		CHECK(memcmp(out_u, out_r, (size_t)runs[k].count) == 0);
	}

	chunkFree(uni);
	chunkFree(pal);
	chunkFree(raw);
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
	// worldChunkCreate always hands back a fresh UNIFORM(BLOCK_AIR) chunk (see world.c), so
	// going UNIFORM(BLOCK_AIR) -> UNIFORM(id) via chunkClear costs nothing extra in bytes —
	// both are the same form — and needs no budget delta, unlike a real edit through
	// worldSet/chunkSet. Chunk is opaque since step 9.2a, so this can no longer reach in and
	// memset a c->blocks array that does not exist for a UNIFORM chunk anyway.
	chunkClear(c, id);
	return true;
}

// ── v1.6.0 task 11: reading a greedy-merged mesh ─────────────────────────────
//
// The mesher merges runs of co-planar faces along the atlas u axis into one wide quad, so
// out.faces stopped being "how many block faces are visible" and became "how many quads it
// took to cover them". Nearly every count in the tests below was asking the first question
// and reading the second, and every one of them would have gone quietly wrong.
//
// So those counts are restated in BLOCK FACES, which is the number that never moves:
// merging regroups faces, it never creates or destroys one. Anywhere a test used to say
// `out.faces == N` and meant "N block faces are visible", it now says
// `meshFaceCells(&out) == N` and means exactly what it always did.

// How many blocks along u one quad covers. Every emitted quad — merged, unmerged, or a
// cross plane — has its u span running from the tile's u0 to u0 + TILE_PX*width, because a
// merged quad's u1 is stretched and nothing else about the layout changed. A cross quad
// spans one tile, so it counts as the single face it is.
static int quadWidth(const MeshVertex* v)
{
	uint8_t lo = v[0].u, hi = v[0].u;
	for (int i = 1; i < 4; i++) {
		if (v[i].u < lo) lo = v[i].u;
		if (v[i].u > hi) hi = v[i].u;
	}
	return (hi - lo) / TILE_PX;
}

// Block faces covered by the quads in [q_lo, q_hi). Quad q owns vertices q*4 .. q*4+3 —
// every emitter in the mesher writes four vertices per quad, in order, which is the same
// property scene/chunk_render.c's one shared index buffer rests on.
static uint32_t meshFaceCellsRange(const MeshOut* o, uint32_t q_lo, uint32_t q_hi)
{
	uint32_t n = 0;
	for (uint32_t q = q_lo; q < q_hi; q++)
		n += (uint32_t)quadWidth(&o->verts[q * 4]);
	return n;
}

static uint32_t meshFaceCells(const MeshOut* o)
{
	return meshFaceCellsRange(o, 0, o->faces);
}

// The same, for one face direction, plus how many quads it took. Two numbers because the
// whole point of a merge test is the gap between them.
static uint32_t meshFaceCellsNrm(const MeshOut* o, uint8_t nrm, uint32_t* quads_out)
{
	uint32_t cells = 0, quads = 0;
	for (uint32_t q = 0; q < o->faces; q++) {
		if (o->verts[q * 4].nrm != nrm) continue;
		cells += (uint32_t)quadWidth(&o->verts[q * 4]);
		quads++;
	}
	if (quads_out) *quads_out = quads;
	return cells;
}

// The widest run any quad in the mesh covers. ATLAS_MAX_MERGE_BLOCKS is the ceiling, and
// exceeding it wraps MeshVertex.u's uint8_t — see world/atlas_uv.h.
static int meshWidestQuad(const MeshOut* o)
{
	int w = 0;
	for (uint32_t q = 0; q < o->faces; q++) {
		const int qw = quadWidth(&o->verts[q * 4]);
		if (qw > w) w = qw;
	}
	return w;
}

// Vertex positions run 0..CHUNK_DIM inclusive, so the grid is one wider than the chunk.
static bool s_cover[CHUNK_DIM + 1][CHUNK_DIM + 1][CHUNK_DIM + 1][BLOCK_FACES];

// The claim merging has to earn: the merged mesh covers exactly the block faces the
// unmerged one did — no gaps, no overlaps, nothing covered twice.
//
// Every axis-aligned quad is expanded back into the unit block faces it stands for, keyed
// by (min corner, face direction), and a key seen twice is a double-cover. Returns false if
// any quad double-covers or cannot be read as a run at all; `cells_out` is the total, which
// the caller compares against the number of faces that world is known to have — that half
// is what catches a GAP, since a missing face lowers the total and nothing else would.
//
// Cross quads are not expanded and are counted separately: they are diagonal, so they have
// extent on all three axes where a block face has extent on exactly two, and four of them
// share one cell by design. Detected by that geometry rather than by their normal, which
// emitCross deliberately sets to FACE_TOP.
static bool meshCoverageOnce(const MeshOut* o, uint32_t* cells_out, uint32_t* crosses_out)
{
	memset(s_cover, 0, sizeof s_cover);
	uint32_t cells = 0, crosses = 0;
	bool ok = true;

	for (uint32_t q = 0; q < o->faces; q++) {
		const MeshVertex* v = &o->verts[q * 4];
		int mn[3] = { v[0].x, v[0].y, v[0].z };
		int mx[3] = { v[0].x, v[0].y, v[0].z };
		for (int i = 1; i < 4; i++) {
			const int p[3] = { v[i].x, v[i].y, v[i].z };
			for (int a = 0; a < 3; a++) {
				if (p[a] < mn[a]) mn[a] = p[a];
				if (p[a] > mx[a]) mx[a] = p[a];
			}
		}

		if (mn[0] != mx[0] && mn[1] != mx[1] && mn[2] != mx[2]) { crosses++; continue; }

		const int w = quadWidth(v);
		if (w < 1 || v[0].nrm >= BLOCK_FACES) { ok = false; continue; }

		// Which axis the run grew down. For w > 1 exactly one axis spans w blocks: the
		// other in-plane axis is one block and the normal axis is zero. For w == 1 there
		// is nothing to step and the axis is not needed.
		int ax = -1;
		if (w > 1) {
			for (int a = 0; a < 3; a++)
				if (mx[a] - mn[a] == w) ax = a;
			if (ax < 0) { ok = false; continue; }
		}

		for (int k = 0; k < w; k++) {
			int c[3] = { mn[0], mn[1], mn[2] };
			if (ax >= 0) c[ax] += k;
			if (c[0] < 0 || c[0] > CHUNK_DIM || c[1] < 0 || c[1] > CHUNK_DIM ||
			    c[2] < 0 || c[2] > CHUNK_DIM) { ok = false; break; }
			if (s_cover[c[0]][c[1]][c[2]][v[0].nrm]) ok = false;   // covered twice
			s_cover[c[0]][c[1]][c[2]][v[0].nrm] = true;
			cells++;
		}
	}

	if (cells_out)   *cells_out = cells;
	if (crosses_out) *crosses_out = crosses;
	return ok;
}

// Two meshes cover exactly the same block faces, however differently they grouped them
// into quads. Needed wherever two arms of the same world can no longer be compared vertex
// by vertex: a run stops where anything visible about the face changes, so an arm with the
// lighting engine on merges strictly less than one with it off, and the vertex arrays no
// longer line up index for index even though the geometry is the same.
static bool s_cover_b[CHUNK_DIM + 1][CHUNK_DIM + 1][CHUNK_DIM + 1][BLOCK_FACES];

static bool meshCoverageEqual(const MeshOut* a, const MeshOut* b)
{
	uint32_t ca = 0, cb = 0;
	if (!meshCoverageOnce(a, &ca, NULL)) return false;
	memcpy(s_cover_b, s_cover, sizeof s_cover);
	if (!meshCoverageOnce(b, &cb, NULL)) return false;
	return ca == cb && memcmp(s_cover_b, s_cover, sizeof s_cover) == 0;
}

// A merged quad is still one quad: four vertices and six indices, in the winding the one
// shared index buffer in scene/chunk_render.c assumes. Checked over the whole mesh because
// merging changed how many quads there are, not how one is written — if it ever wrote a
// wide quad as two triangles of its own, this is what would notice.
static bool meshIndexPatternOk(const MeshOut* o)
{
	if (o->vert_count != o->faces * 4 || o->index_count != o->faces * 6) return false;
	for (uint32_t q = 0; q < o->faces; q++) {
		const uint16_t  b  = (uint16_t)(q * 4);
		const uint16_t* ix = &o->indices[q * 6];
		if (ix[0] != b || ix[1] != b + 1 || ix[2] != b + 2 ||
		    ix[3] != b || ix[4] != b + 2 || ix[5] != b + 3) return false;
	}
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
	CHECK(meshFaceCells(&out) == 6 * CHUNK_DIM * CHUNK_DIM);
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
	CHECK(meshFaceCells(&out) == 6 * CHUNK_DIM * CHUNK_DIM);
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
	CHECK(leaf.u0 != stone.u0 || leaf.vslot0 != stone.vslot0);   // else the check below proves nothing

	bool all_leaf_tile = true;
	for (uint32_t i = out.opaque_index_count; i < out.index_count; i++) {
		const MeshVertex* v = &out.verts[out.indices[i]];
		const bool in_tile = (v->u >= leaf.u0 && v->u <= leaf.u1 &&
		                      v->v >= leaf.vslot0 && v->v <= leaf.vslot1);
		if (!in_tile) all_leaf_tile = false;
	}
	CHECK(all_leaf_tile);

	// And the opaque run must contain none of it.
	bool any_leaf_in_opaque = false;
	for (uint32_t i = 0; i < out.opaque_index_count; i++) {
		const MeshVertex* v = &out.verts[out.indices[i]];
		if (v->u >= leaf.u0 && v->u <= leaf.u1 && v->v >= leaf.vslot0 && v->v <= leaf.vslot1)
			any_leaf_in_opaque = true;
	}
	CHECK(!any_leaf_in_opaque);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// Step 9.1. An opaque block underneath a see-through one must keep the face between them.
//
// Found by the 2026-08-18 culling audit, not by looking at the screen, and that is the point:
// the neighbour test culled on `solid` alone, and leaves are solid. So the stone under a leaf,
// and — far more often — the trunk inside its own canopy, lost the faces pointing at the
// leaves. Through the alpha-0 holes in the leaf tile that reads as a pinhole of sky in the
// middle of a tree, a few pixels across, which is exactly the kind of thing nobody finds by
// playing and nobody sees in a screenshot.
//
// The rule the mesher now follows, and the reason each half exists:
//
//   cull iff the neighbour is solid AND (it is opaque OR it is the same block id)
//
// The first half is the occlusion that has always been there. The second is what keeps a
// canopy cheap — leaf against leaf is still culled, so the transparent pass draws the shell of
// a tree and not every leaf inside it. Dropping the second half would be correct and would
// also multiply the canopy's face count; dropping the first is what shipped.
static void testMesherOpaqueBehindTransparent(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	// The premise. Leaves are solid (they occupy their cell for collision and AO) and
	// transparent (you can see past them), and those are different questions.
	CHECK(blockIsSolid(BLOCK_LEAVES));
	CHECK(blockInfo(BLOCK_LEAVES)->transparent);
	CHECK(!blockInfo(BLOCK_STONE)->transparent);

	// --- Baseline: a one-block-thick stone floor with nothing above it. 256 top faces.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	// Counted in BLOCK FACES, not in quads: v1.6.0 task 11 merges runs of faces into wide
	// quads, and a leaf placed on the floor below breaks the merge of the top faces around it
	// by darkening their AO. The number of faces is what this test is about and it does not
	// move; the number of quads it takes to draw them does, for reasons that are nothing to
	// do with occlusion.
	const uint32_t bare_floor_faces = meshFaceCellsRange(&out, 0, out.opaque_faces);
	CHECK(bare_floor_faces > 0);

	// --- Now put a leaf directly on one of those stone blocks. The stone's top face is now
	// looking at a leaf instead of at air, and it must still be emitted: the leaf does not
	// fill the pixels it covers. So the opaque count must not move at all.
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 1, CHUNK_DIM + 4, BLOCK_LEAVES));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.opaque_faces) == bare_floor_faces);

	// The leaf itself keeps five faces, not six: the one pointing down at the stone is
	// genuinely hidden, because stone is opaque. The asymmetry is the whole rule.
	CHECK(meshFaceCells(&out) == bare_floor_faces + 5);

	// --- The canopy case, which is the one that actually happens. A wood post with leaves
	// packed around it: every wood face touching a leaf must survive.
	worldExit(&s_world);
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_WOOD));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == 6);        // lone wood block in air

	for (int dy = -1; dy <= 1; dy++)
		for (int dz = -1; dz <= 1; dz++)
			for (int dx = -1; dx <= 1; dx++) {
				if (!dx && !dy && !dz) continue;
				const int lx = CHUNK_DIM + 8 + dx;
				const int ly = CHUNK_DIM + 8 + dy;
				const int lz = CHUNK_DIM + 8 + dz;
				CHECK_QUIET(worldSet(&s_world, lx, ly, lz, BLOCK_LEAVES));
			}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == 6);        // still all six, now that they face leaves

	// And the shell around it is still culled internally: the 26 leaves form a 3x3x3 box with
	// one cell missing, so the transparent run is its outside surface (54 faces) plus the six
	// inward faces around the wood — leaf against wood is leaf against something opaque, so
	// those six are culled too. 54 exactly, and if same-material culling ever broke this
	// number would jump.
	CHECK(meshFaceCellsRange(&out, out.opaque_faces, out.faces) == 54);

	worldExit(&s_world);
	free(out.verts);
	free(out.indices);
}

// Step 9.2. The opaque run is laid out face-major, six contiguous buckets, so the renderer
// can drop the three directions that point away from the camera without touching the mesh.
// That saving is only sound if the layout is exactly what it claims, and nothing on screen
// would show it slipping: a mis-sized bucket draws the wrong triangles, and a bucket holding
// mixed normals makes the whole optimisation silently wrong in a way that only shows as faces
// vanishing at certain angles. So it is pinned here.
static void testMesherFaceBuckets(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	// A 4x4x4 cube of stone floating in air: every one of the six directions is exposed, and
	// each shows exactly a 4x4 slab, so all six buckets must come out the same known size.
	worldInit(&s_world);
	for (int y = 0; y < 4; y++)
		for (int z = 0; z < 4; z++)
			for (int x = 0; x < 4; x++) {
				const int wx = CHUNK_DIM + 6 + x;
				const int wy = CHUNK_DIM + 6 + y;
				const int wz = CHUNK_DIM + 6 + z;
				CHECK_QUIET(worldSet(&s_world, wx, wy, wz, BLOCK_STONE));
			}

	// One leaf well away from it, so there is a transparent run after the opaque one. The
	// buckets must close at the opaque boundary and not run on into it.
	CHECK(worldSet(&s_world, CHUNK_DIM + 2, CHUNK_DIM + 2, CHUNK_DIM + 2, BLOCK_LEAVES));

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);

	CHECK(out.face_start[0] == 0);
	CHECK(out.face_start[BLOCK_FACES] == out.opaque_index_count);
	CHECK(out.opaque_index_count < out.index_count);   // the leaf is there, after the buckets

	// 16 faces a side, and — since every face of a floating convex cube is unoccluded on all
	// four corners, so the whole surface is flat AO 3 — v1.6.0 task 11 merges each 4-block
	// row along u into one quad. Four quads a side, six indices a quad, still covering the
	// same sixteen block faces. Both numbers are checked: the second is the merge, the first
	// is what must survive it.
	for (int f = 0; f < BLOCK_FACES; f++) {
		const uint32_t len = out.face_start[f + 1] - out.face_start[f];
		CHECK_QUIET(len == 4 * 6);
		const uint32_t q_lo = out.face_start[f] / 6, q_hi = out.face_start[f + 1] / 6;
		CHECK_QUIET(meshFaceCellsRange(&out, q_lo, q_hi) == 16);
	}

	// The layout claim itself: every index inside bucket i must land on a vertex whose normal
	// is kFaceOrder[i]. This is the thing the renderer bets on when it skips a bucket. If the
	// mesher ever went back to walking cell-major, bucket 0 would hold all six normals and
	// this would go red; if the mesher and the renderer ever disagreed about the bucket
	// order, the wrong faces would be dropped and this would go red too.
	for (int i = 0; i < BLOCK_FACES; i++)
		for (uint32_t k = out.face_start[i]; k < out.face_start[i + 1]; k++) {
			const uint8_t nrm = out.verts[out.indices[k]].nrm;
			CHECK_QUIET(nrm == kFaceOrder[i]);
		}

	// kFaceOrder must be a permutation of the six faces — a typo that repeated one and
	// dropped another would leave a whole direction unmeshed, and the loop above would
	// still pass.
	int seen[BLOCK_FACES] = {0};
	for (int i = 0; i < BLOCK_FACES; i++) {
		CHECK_QUIET(kFaceOrder[i] < BLOCK_FACES);
		seen[kFaceOrder[i]]++;
	}
	for (int f = 0; f < BLOCK_FACES; f++)
		CHECK_QUIET(seen[f] == 1);

	// No two adjacent buckets may be opposite faces of the same axis — that is the property
	// the renderer's merge loop is built on, and it is what makes this order worth having
	// over the enum order. FACE_* pairs each axis as (2n, 2n+1).
	for (int i = 0; i + 1 < BLOCK_FACES; i++)
		CHECK_QUIET((kFaceOrder[i] >> 1) != (kFaceOrder[i + 1] >> 1));

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
	CHECK(meshFaceCells(&out) == 6 * CHUNK_DIM * CHUNK_DIM);   // 1536, not 24576
	CHECK(out.vert_count == out.faces * 4);   // still four vertices and six indices a quad,
	CHECK(out.index_count == out.faces * 6);  // however many block faces the quad covers
	CHECK(out.overflow == false);

	// v1.6.0 task 11: a chunk of stone in open air is flat AO 3 over its whole surface, so
	// every 16-long row along u merges — into two quads, not one, because
	// ATLAS_MAX_MERGE_BLOCKS is 15 and a row is 16 wide. 16 rows x 2 quads x 6 faces.
	CHECK(out.faces == 6 * CHUNK_DIM * 2);
	CHECK(meshWidestQuad(&out) == ATLAS_MAX_MERGE_BLOCKS);

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
	CHECK(meshFaceCells(&out) == 5 * CHUNK_DIM * CHUNK_DIM);   // 1280

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
		if ((v->u != r.u0 && v->u != r.u1) || (v->v != r.vslot0 && v->v != r.vslot1)) wrong_tile++;
	}
	CHECK(wrong_tile == 0);
	int faces_with_four = 0;
	for (int f = 0; f < BLOCK_FACES; f++) if (per_face[f] == 4) faces_with_four++;
	CHECK(faces_with_four == BLOCK_FACES);

	// Grass is the block that proves per-face tiles work at all: three different
	// tiles on one cube. Top face vertices must carry the grass top tile.
	//
	// Discriminates on v, not u. This read `u == top.u0 || u == top.u1` until v1.6.0's
	// one-tile-wide strip atlas, where EVERY tile's u span is 0..16 — so that condition
	// became true for every vertex in the mesh and the check collapsed into "four vertices
	// have nrm == 2", still green while proving nothing about which tile they carry. On a
	// strip sheet v is the only axis that separates one tile from another.
	const AtlasRect top = atlasRect(BTEX_GRASS_TOP);
	int top_verts = 0;
	for (uint32_t i = 0; i < out.vert_count; i++)
		if (out.verts[i].nrm == 2 && (out.verts[i].v == top.vslot0 || out.verts[i].v == top.vslot1))
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

// ── v1.6.0 task 13: block shapes ─────────────────────────────────────────────

// FNV-1a over a whole mesh: the counts, the bucket table, every vertex byte and every
// index. Used by the anchor below. Endian-independent for the vertex bytes (all u8) and
// little-endian for the u16 indices, which is both hosts this suite runs on.
static uint32_t meshHash(const MeshOut* out)
{
	uint32_t h = 2166136261u;
	const uint8_t* b;
	size_t n;

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

// The no-regression anchor for the shape work.
//
// v1.6.0 task 13 split three questions apart that had been one: what fills a cell
// (collision, AO), what hides the face behind it (occlusion) and what gets drawn at all.
// Every block in the registry is a solid full cube, so all three still answer the same
// for all of them and no existing world may move by a single byte — but "may not move"
// is exactly the sort of claim that is easy to assert and hard to prove, and there is no
// screen here to notice if it did.
//
// So these four numbers are not invented. They were measured by building the SAME probe
// twice, once against the pre-task mesher and once against the post-task one, over these
// four worlds plus a fifth with the lighting engine on, and diffing the two outputs:
//
//   random-core-mix        verts= 24344 idx= 36516 faces= 6086 hash=ceb0a3c6
//   stone-beside-leaves    verts=  6144 idx=  9216 faces= 1536 hash=883aee2f
//   all-leaves             verts=  6144 idx=  9216 faces= 1536 hash=68875887
//   all-air                verts=     0 idx=     0 faces=    0 hash=c655ff85
//   random-core-mix-lit    verts= 24304 idx= 36456 faces= 6076 hash=f0c3f25e
//
// identical on both arms. The four unlit cases are pinned here so the property keeps
// being checked after the probe is gone. A failure here is not "the hash changed", it is
// "the mesh a cube world produces is no longer the one v1.6.0 shipped" — if that is
// intended, re-measure it the same way rather than editing the constant to match.
//
// ── RE-PINNED, v1.6.0 task 11 (greedy meshing) ───────────────────────────────
//
// Merging co-planar faces into wide quads changes every one of those numbers on purpose:
// fewer quads, fewer vertices, different bytes. The anchor was NOT quietly re-baselined —
// the old values are kept above, verbatim, and the new ones measured the same way, by
// running these four worlds against the merged mesher and reading what came out:
//
//   random-core-mix
//     BEFORE verts=24344 idx=36516 faces=6086 hash=ceb0a3c6
//     AFTER  verts=23676 idx=35514 faces=5919 hash=85132e65   cells=6086
//   stone-beside-leaves
//     BEFORE verts= 6144 idx= 9216 faces=1536 hash=883aee2f
//     AFTER  verts=  936 idx= 1404 faces= 234 hash=c62dfbe6   cells=1536
//   all-leaves
//     BEFORE verts= 6144 idx= 9216 faces=1536 hash=68875887
//     AFTER  verts=  768 idx= 1152 faces= 192 hash=953b53b8   cells=1536
//   all-air
//     BEFORE verts=    0 idx=    0 faces=   0 hash=c655ff85
//     AFTER  unchanged — an empty mesh has nothing to merge
//
// A hash is a weak anchor for a change like this: it goes red for any reason at all and
// says nothing about which. So each case now carries an invariant that merging CANNOT
// change, checked alongside it — the number of BLOCK FACES covered, which is exactly the
// pre-merge quad count, together with meshCoverageOnce() proving no face is covered twice.
// If a future change legitimately moves the hashes again, those two must not move with
// them; if they do, geometry has been lost or duplicated and the hash was not the story.
//
// ── RE-PINNED AGAIN, v1.8.2 task 13b (atlas ceiling 15 -> 64 slots) ──────────
//
// The mesher's vertex `v` byte changed UNITS, not width: it held an atlas PIXEL ROW
// (tile * TILE_PX) and now holds a slot-EDGE INDEX (tile, or tile + 1), with the TILE_PX
// factor moved into both shaders' uvScale.y. MeshVertex is still 8 bytes and not one
// vertex moved in space — but every `v` byte in every mesh now holds a different number,
// so the hashes had to move and the geometry had to not.
//
// That is exactly what the paragraph above demands be proved rather than assumed, and it
// is why the counts below are pinned as tightly as the hashes are. Measured by running
// these four worlds against the post-13b mesher and reading the printed values, with the
// pre-13b numbers left standing above, verbatim:
//
//   random-core-mix        verts=23676 idx=35514 faces=5919  hash 85132e65 -> cf4cf785
//   stone-beside-leaves    verts=  936 idx= 1404 faces= 234  hash c62dfbe6 -> 6a24313e
//   all-leaves             verts=  768 idx= 1152 faces= 192  hash 953b53b8 -> ef2164b8
//   all-air                verts=    0 idx=    0 faces=   0  hash c655ff85 -> UNCHANGED
//
// Every count is identical on both arms — not asserted, read off the same run — and the
// suite went from 4821 green to exactly 3 red, one per non-empty world. all-air did not
// move because an empty mesh has no `v` byte to change, which is the fourth case earning
// its keep: a hash that moved there would have meant something else had changed too.
//
// The vert_count/index_count CHECKs on cases 2, 3 and 4 were ADDED here. They were the
// arm that made the claim provable, so they stay: a future units change that also lost
// geometry would otherwise show up as three moved hashes and look identical to this one.
static void testMesherFullCubeBytesUnchanged(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	// Case 1: a pseudo-random mix of every core block, from a fixed LCG so the world is
	// the same one the A/B probe measured. Exercises both passes, all six buckets, AO
	// and the same-material cull over geometry no hand-written case would produce.
	worldInit(&s_world);
	uint32_t rng = 12345u;
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				rng = rng * 1664525u + 1013904223u;
				const BlockId id = (BlockId)((rng >> 16) % BLOCK_COUNT);
				CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y,
				                     CHUNK_DIM + z, id));
			}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.vert_count == 23676);
	CHECK(out.index_count == 35514);
	CHECK(out.faces == 5919);
	CHECK(meshHash(&out) == 0xcf4cf785u);
	// Merging may not invent or lose a single block face: 6086 is what the pre-merge
	// mesher emitted, one quad each, and it has to stay 6086 however they are grouped.
	CHECK(meshFaceCells(&out) == 6086);
	CHECK(meshCoverageOnce(&out, NULL, NULL));
	worldExit(&s_world);

	// Case 2: stone with leaves against its east face — the cross-chunk border, where
	// the scratch's one-block skirt is what the culling reads.
	worldInit(&s_world);
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y,
				                     CHUNK_DIM + z, BLOCK_STONE));
				CHECK_QUIET(worldSet(&s_world, 2 * CHUNK_DIM + x, CHUNK_DIM + y,
				                     CHUNK_DIM + z, BLOCK_LEAVES));
			}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.vert_count == 936);
	CHECK(out.index_count == 1404);
	CHECK(out.faces == 234);
	CHECK(meshHash(&out) == 0x6a24313eu);
	CHECK(meshFaceCells(&out) == 1536);
	CHECK(meshCoverageOnce(&out, NULL, NULL));
	worldExit(&s_world);

	// Case 3: a solid canopy, i.e. the transparent pass on its own.
	worldInit(&s_world);
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y,
				                     CHUNK_DIM + z, BLOCK_LEAVES));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.vert_count == 768);
	CHECK(out.index_count == 1152);
	CHECK(out.faces == 192);
	CHECK(out.opaque_faces == 0);
	CHECK(meshHash(&out) == 0xef2164b8u);
	CHECK(meshFaceCells(&out) == 1536);
	CHECK(meshCoverageOnce(&out, NULL, NULL));
	worldExit(&s_world);

	// Case 4: nothing at all. The empty mesh has to stay empty.
	worldInit(&s_world);
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.vert_count == 0);
	CHECK(out.index_count == 0);
	CHECK(out.faces == 0);
	CHECK(meshHash(&out) == 0xc655ff85u);
	worldExit(&s_world);

	free(out.verts);
	free(out.indices);
}

// Registers the two dynamic rows the shape tests below work with, through the real
// registryRegister() the DEFS decoder ends up in — not a fabricated table — so what is
// exercised is the path a server-defined block actually arrives by. Returns false if
// either registration was refused, which the caller must check: a missing block would
// make every count below pass for the wrong reason.
//
// BTEX_LEAVES for the cross and BTEX_SAND for the control cube, because both have to be
// distinguishable from the BLOCK_STONE floor by UV alone.
// Vertices whose baked ambient occlusion is not full brightness. A whole-mesh count
// rather than a hunt for particular corners: the claim being tested is "this block
// darkened nothing, anywhere", and a number that is zero over a flat floor and non-zero
// once a real cube stands on it separates exactly that.
static uint32_t countDarkVerts(const MeshOut* o)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < o->vert_count; i++)
		if (o->verts[i].ao < 3) n++;
	return n;
}

static bool registerShapeBlocks(BlockId* cross_out, BlockId* cube_out)
{
	BlockDef def;

	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "test_cross");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_LEAVES;
	// Not solid: a plant is walked through. Transparent: the tile has alpha-0 holes.
	def.flags = REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS);
	*cross_out = registryRegister(&def);

	// The control arm. Same dynamic id space, same registration call, everything
	// identical except the shape — so a check that passes for the cross and fails for
	// this one is testing the shape and not "high ids behave differently".
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "test_cube");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_SAND;
	def.flags = REG_FLAG_SOLID;
	*cube_out = registryRegister(&def);

	mesherInvalidateTables();   // the mesher caches its per-id tables; these are new ids
	return *cross_out != 0 && *cube_out != 0;
}

// Puts the registry and the mesher's derived tables back to the core rows. Every test
// after these expects a registry nobody has added to, and a mesher whose cached tables
// agree with it — dropping only one of the two leaves the mesher answering for ids that
// no longer exist.
static void restoreCoreRegistry(void)
{
	registryInitCore();
	mesherInvalidateTables();
}

// The shape a dynamic block declares must survive the registry: the flags byte, the wire
// record, and the derived view the whole engine reads through blockInfo().
//
// The packing is the risky half. The shape rides in the top three bits of BlockDef.flags
// rather than in a field of its own, precisely so REGISTRY_WIRE_RECORD_BYTES and the
// pinned core crc16 do not move — and a packing that quietly collided with a behaviour
// flag would present as a block that is mysteriously solid, not as an error.
static void testBlockShapeRegistry(void)
{
	// Every core row that carries an ITEM id is a full cube, and that is what makes the
	// no-regression anchor above mean anything. BLOCK_COUNT is the item-bearing span
	// (0x00..0x07) and stops short of the two core rows added by roadmap tasks 17 and 19 —
	// water, which is a cube, and tall grass, which is deliberately NOT one and is covered
	// by testMesherCoreWaterAndTallGrass(). See world/block.h for why BLOCK_COUNT stayed 8.
	for (int id = 0; id < BLOCK_COUNT; id++)
		CHECK_QUIET(blockInfo((BlockId)id)->shape == BLOCK_SHAPE_FULL_CUBE);
	CHECK(blockInfo((BlockId)BLOCK_WATER)->shape == BLOCK_SHAPE_FULL_CUBE);
	CHECK(blockInfo((BlockId)BLOCK_TALL_GRASS)->shape == BLOCK_SHAPE_CROSS);
	CHECK(blockIsFullCube(BLOCK_STONE));
	CHECK(blockIsFullCube(BLOCK_AIR));

	// Drawn vs solid, on the blocks that exist today: the two questions still agree, or
	// the anchor above would be measuring a changed world.
	CHECK(!blockIsDrawn(BLOCK_AIR));
	CHECK(blockIsDrawn(BLOCK_STONE) && blockIsDrawn(BLOCK_LEAVES));
	CHECK(!blockIsDrawn((BlockId)0xC7));          // no registry row: still a hole
	CHECK(!blockIsTargetable((BlockId)0xC7));

	BlockId cross = 0, cube = 0;
	CHECK(registerShapeBlocks(&cross, &cube));
	if (!cross || !cube) { restoreCoreRegistry(); return; }

	CHECK(cross >= REG_ID_DYN_LO && cube >= REG_ID_DYN_LO);   // both really are dynamic

	CHECK(blockInfo(cross)->shape == BLOCK_SHAPE_CROSS);
	CHECK(blockInfo(cube)->shape == BLOCK_SHAPE_FULL_CUBE);
	CHECK(!blockIsFullCube(cross));
	CHECK(blockIsFullCube(cube));

	// The shape bits must not have leaked into the behaviour flags or the other way
	// round. The cross declares transparent-and-not-solid; the cube solid-and-opaque.
	CHECK(!blockIsSolid(cross));
	CHECK(blockInfo(cross)->transparent);
	CHECK(blockIsSolid(cube));
	CHECK(!blockInfo(cube)->transparent);

	// Drawn without being solid is the whole point of the split.
	CHECK(blockIsDrawn(cross));
	CHECK(blockIsTargetable(cross));

	// And the wire record carries it. registryDefPack/Unpack is what a DEFS batch and
	// registry.bin both go through, so a shape that did not survive here would arrive at
	// a joining client as a full cube — two players seeing different geometry.
	uint8_t rec[REGISTRY_WIRE_RECORD_BYTES];
	registryDefPack(rec, cross, registryGet(cross));
	BlockId  back_id = 0;
	BlockDef back_def;
	CHECK(registryDefUnpack(&back_id, &back_def, rec));
	CHECK(back_id == cross);
	CHECK(regShapeOf(back_def.flags) == BLOCK_SHAPE_CROSS);
	CHECK((back_def.flags & REG_FLAG_TRANSPARENT) != 0);
	CHECK((back_def.flags & REG_FLAG_SOLID) == 0);

	// The record is still 28 bytes, which is the reason the shape went into the flags
	// byte at all. If this ever moves, every saved registry.bin and every peer on the
	// old protocol is wrong about every block.
	CHECK(REGISTRY_WIRE_RECORD_BYTES == 28);
	CHECK(sizeof(BlockDef) == 27);

	restoreCoreRegistry();
	CHECK(!registryIsDefined(REG_ID_DYN_LO));
}

// The geometry itself: what a BLOCK_SHAPE_CROSS cell meshes into, and what it does to
// its neighbours.
//
// None of this is checkable by looking at the console. A cross that emitted its quads in
// the opaque run would draw as an opaque diagonal sheet; one whose UVs ran past the tile
// rect would draw a stripe of the neighbouring atlas cell; one that still occluded would
// punch a hole in the wall behind it. All three present as bad art, never as an error —
// which is the standing trap in this codebase, and the reason these are counts.
static void testMesherCrossShape(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	BlockId cross = 0, cube = 0;
	CHECK(registerShapeBlocks(&cross, &cube));
	if (!cross || !cube) {
		restoreCoreRegistry();
		free(out.verts); free(out.indices);
		return;
	}

	// --- Baseline: a one-block-thick stone floor across the chunk, nothing on it.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	const uint32_t floor_faces = out.faces;
	CHECK(floor_faces > 0);
	CHECK(out.opaque_faces == floor_faces);      // nothing transparent yet

	// --- One cross block standing on the floor.
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 1, CHUNK_DIM + 4, cross));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);

	// Four quads, not six and not two. Two planes, each emitted twice with opposite
	// winding, because scene/chunk_render.c culls back faces for the whole world pass
	// and never turns it off — a single-sided X would vanish from three of the four
	// horizontal quadrants, and nothing that counts quads would notice.
	CHECK(out.faces == floor_faces + 4);
	CHECK(out.vert_count == floor_faces * 4 + 16);

	// All four are in the transparent run: non-cube geometry has no face buckets, so it
	// can never go in the opaque run, which is emitted face-major in six passes.
	CHECK(out.index_count - out.opaque_index_count == 4 * 6);

	// The stone it stands on keeps its top face. This is the hole-in-the-world check:
	// an X of two quads does not cover the cell, so it must not cull anything.
	CHECK(out.opaque_faces == floor_faces);

	// Exactly the sixteen vertices of the cross, in the order emitCross writes them:
	// plane A forward, plane A backward, plane B forward, plane B backward. Positions
	// are the cell's own integer corners, because MeshVertex holds int8_t block units
	// and there is no fractional inset the format could express.
	const int8_t bx = 4, by = 1, bz = 4;
	static const int8_t kWant[16][3] = {
		{0,0,0}, {1,0,1}, {1,1,1}, {0,1,0},   // plane A, front winding
		{0,1,0}, {1,1,1}, {1,0,1}, {0,0,0},   // plane A, back winding
		{1,0,0}, {0,0,1}, {0,1,1}, {1,1,0},   // plane B, front winding
		{1,1,0}, {0,1,1}, {0,0,1}, {1,0,0},   // plane B, back winding
	};
	const MeshVertex* cv = &out.verts[floor_faces * 4];
	bool pos_ok = true;
	for (int i = 0; i < 16; i++) {
		if (cv[i].x != (int8_t)(bx + kWant[i][0]) ||
		    cv[i].y != (int8_t)(by + kWant[i][1]) ||
		    cv[i].z != (int8_t)(bz + kWant[i][2]))
			pos_ok = false;
	}
	CHECK(pos_ok);

	// The two windings really are opposite: the second quad of each plane is the first
	// one walked backwards. Emitting the same winding twice would be four quads that are
	// still invisible from behind, and the quad count alone cannot tell the difference.
	bool reversed_ok = true;
	for (int i = 0; i < 4; i++) {
		if (cv[i].x != cv[7 - i].x || cv[i].y != cv[7 - i].y || cv[i].z != cv[7 - i].z)
			reversed_ok = false;
		if (cv[8 + i].x != cv[15 - i].x || cv[8 + i].y != cv[15 - i].y ||
		    cv[8 + i].z != cv[15 - i].z)
			reversed_ok = false;
	}
	CHECK(reversed_ok);

	// The two planes are genuinely different planes and not the same one twice: plane A
	// runs corner (0,0)->(1,1) in xz, plane B runs (1,0)->(0,1).
	CHECK(cv[0].x != cv[8].x);

	// UVs stay inside the block's own 16px tile. The atlas is a 6x6 grid of 20px cells
	// holding 16px tiles with a 2px edge-extended border (world/atlas_uv.h), sampled with
	// CLAMP_TO_EDGE — so a UV one pixel past the rect samples the *neighbouring block's
	// art*, and that bug ships as bad art rather than as an error.
	const AtlasRect tile  = atlasRect(blockFaceTex(cross, FACE_EAST));
	const AtlasRect stone = atlasRect(blockFaceTex(BLOCK_STONE, FACE_TOP));
	CHECK(tile.u0 != stone.u0 || tile.vslot0 != stone.vslot0);   // else the check proves nothing

	bool uv_in_tile = true, corners_ok = true, nrm_ok = true, ao_ok = true;
	for (int i = 0; i < 16; i++) {
		if (cv[i].u < tile.u0 || cv[i].u > tile.u1 ||
		    cv[i].v < tile.vslot0 || cv[i].v > tile.vslot1)
			uv_in_tile = false;
		// and not merely inside it — every UV is one of the rect's four corners.
		if ((cv[i].u != tile.u0 && cv[i].u != tile.u1) ||
		    (cv[i].v != tile.vslot0 && cv[i].v != tile.vslot1))
			corners_ok = false;
		// FACE_TOP, so world.v.pica's faceShade gives the plant full brightness rather
		// than shading its two planes differently from each other.
		if (cv[i].nrm != (uint8_t)FACE_TOP) nrm_ok = false;
		if (cv[i].ao != 3) ao_ok = false;   // an X has no crevice corners to darken
	}
	CHECK(uv_in_tile);
	CHECK(corners_ok);
	CHECK(nrm_ok);
	CHECK(ao_ok);

	// --- Two crosses side by side. The same-material cull that keeps a canopy cheap
	// must not fire between them: they do not cover each other, so each still draws all
	// four of its quads.
	CHECK(worldSet(&s_world, CHUNK_DIM + 5, CHUNK_DIM + 1, CHUNK_DIM + 4, cross));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == floor_faces + 8);
	CHECK(out.opaque_faces == floor_faces);
	worldExit(&s_world);

	// --- A cross wedged between two stone blocks must not cull either of their faces.
	// This is the occlusion claim on its own, away from the floor.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 2, CHUNK_DIM + 2, CHUNK_DIM + 2, BLOCK_STONE));
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 2, CHUNK_DIM + 2, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	const uint32_t two_stones = out.opaque_faces;
	CHECK(two_stones == 12);                      // two lone cubes, six faces each

	CHECK(worldSet(&s_world, CHUNK_DIM + 3, CHUNK_DIM + 2, CHUNK_DIM + 2, cross));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == two_stones);        // neither cube lost a face
	CHECK(out.faces == two_stones + 4);

	// The control arm. Swap the cross for the dynamic FULL_CUBE block — same dyn id
	// space, same registration call, only the shape differs — and everything the cross
	// left alone now happens: the two stones lose the faces they point at, and the new
	// block emits four of its own (six, less the two its neighbours cover). 12 - 2 + 4.
	// Without this arm the checks above would also pass for a shape that emitted nothing
	// at all and culled nothing either.
	CHECK(worldSet(&s_world, CHUNK_DIM + 3, CHUNK_DIM + 2, CHUNK_DIM + 2, cube));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == two_stones - 2 + 4);
	CHECK(out.opaque_faces == 14);
	CHECK(out.index_count == out.opaque_index_count); // a solid cube is not deferred
	worldExit(&s_world);

	// --- The mis-declared solid cross. Shape and solidity are separate axes.
	//
	// This arm exists because of a hole in the red run. The two `&& cube` terms in
	// planBuild()'s s_solid and s_occludes tables are what stop a non-cube from darkening
	// and from culling its neighbours — but the plant above is registered non-solid, so
	// `info->solid` short-circuits and neither term is ever reached. Sabotaging them left
	// the suite green, which by this project's own rule proves nothing. A block that
	// declares SOLID and CROSS together does reach them.
	//
	// It is not a block the game will ship. It is the case the tables are defended
	// against: nothing stops a def — including one arriving from a server — setting both
	// bits, and the answer must still be that two diagonal quads cover no face and fill no
	// corner of the cell, however solid the def says it is.
	BlockDef sdef;
	memset(&sdef, 0, sizeof sdef);
	snprintf(sdef.name, sizeof sdef.name, "test_xsolid");
	for (int f = 0; f < BLOCK_FACES; f++) sdef.tex[f] = BTEX_LEAVES;
	sdef.flags = REG_FLAG_SOLID | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS);
	const BlockId xsolid = registryRegister(&sdef);
	mesherInvalidateTables();
	CHECK(xsolid != 0);
	CHECK(blockIsSolid(xsolid));                        // the solid bit really did land
	CHECK(blockInfo(xsolid)->shape == BLOCK_SHAPE_CROSS);

	if (xsolid) {
		// Occlusion, in the same two-stone rig as above.
		worldInit(&s_world);
		CHECK(worldSet(&s_world, CHUNK_DIM + 2, CHUNK_DIM + 2, CHUNK_DIM + 2, BLOCK_STONE));
		CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 2, CHUNK_DIM + 2, BLOCK_STONE));
		CHECK(worldSet(&s_world, CHUNK_DIM + 3, CHUNK_DIM + 2, CHUNK_DIM + 2, xsolid));
		scratchFill(&s_scratch, &s_world, 1, 1, 1);
		meshChunk(&out, &s_scratch);
		CHECK(out.opaque_faces == 12);      // neither cube lost the face it points at
		CHECK(out.faces == 12 + 4);         // and the cross still drew its own four
		worldExit(&s_world);

		// Ambient occlusion. A flat floor has no darkened corner anywhere; standing the
		// solid cross on it must not create one, because an AO tap asks "is that cell
		// filled" and a cross does not fill one.
		worldInit(&s_world);
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
				                     BLOCK_STONE));
		scratchFill(&s_scratch, &s_world, 1, 1, 1);
		meshChunk(&out, &s_scratch);
		CHECK(countDarkVerts(&out) == 0);

		CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 1, CHUNK_DIM + 4, xsolid));
		scratchFill(&s_scratch, &s_world, 1, 1, 1);
		meshChunk(&out, &s_scratch);
		CHECK(countDarkVerts(&out) == 0);

		// The control that proves the counter can move at all. Same cell, real cube:
		// the top faces around it lose corners immediately.
		CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 1, CHUNK_DIM + 4, BLOCK_STONE));
		scratchFill(&s_scratch, &s_world, 1, 1, 1);
		meshChunk(&out, &s_scratch);
		CHECK(countDarkVerts(&out) > 0);
		worldExit(&s_world);
	}

	restoreCoreRegistry();
	free(out.verts);
	free(out.indices);
}

// ── roadmap tasks 17 and 19: the two blocks that actually ship ───────────────
//
// Everything above proves the mesher's cross and transparent paths against blocks this
// test file registers itself. That is the right way to test the PATHS and the wrong way to
// test the BLOCKS: a def built here is a def this file controls, so it would keep passing
// with world/registry.c's real rows deleted, mis-flagged, or never added at all. The two
// checks the brief actually asks for are about BLOCK_TALL_GRASS and BLOCK_WATER as
// registryInitCore() defines them, so those are the ids used below and no def is built.
//
// None of this is visible on the console either, in the standing way: tall grass wired as a
// cube draws as a solid green block, water wired opaque draws as a solid blue one, and water
// that failed to cull against itself draws exactly right while costing thousands of quads a
// frame. All three ship as art or as a frame rate, never as an error.
static void testMesherCoreWaterAndTallGrass(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	restoreCoreRegistry();   // core rows only: no def in this file is in play

	// Short aliases for the worldSet() calls below. CHECK() stringifies its condition into a
	// 96-byte buffer and -Werror=format-truncation turns one byte over into a build failure,
	// so the long spelling does not fit inside a four-argument worldSet.
	const BlockId kWater = (BlockId)BLOCK_WATER;
	const BlockId kGrass = (BlockId)BLOCK_TALL_GRASS;

	// --- What the registry says the two blocks are. The mesher's behaviour below is
	// derived entirely from these, so reading them first says which flag any later failure
	// belongs to instead of leaving "the geometry is wrong" to be bisected by hand.
	CHECK(registryIsDefined(kWater));
	CHECK(registryIsDefined(kGrass));
	CHECK(strcmp(blockInfo(kWater)->name, "water") == 0);
	CHECK(strcmp(blockInfo(kGrass)->name, "tall_grass") == 0);

	// Water: drawn, a full cube, transparent, liquid, not solid.
	CHECK(blockIsDrawn(kWater));
	CHECK(blockIsFullCube(kWater));
	CHECK(blockInfo(kWater)->transparent);
	CHECK(blockInfo(kWater)->liquid);
	CHECK(!blockIsSolid(kWater));
	// Not targetable, which is the whole of "not minable and not placeable today": both
	// break and place in scene/interact.c run off the raycast, and blockIsTargetable() is
	// what the raycast stops on. Water being LIQUID is what makes this false.
	CHECK(!blockIsTargetable(kWater));

	// Tall grass: drawn, a cross, transparent, not liquid, not solid — and targetable,
	// which is the deliberate difference from water. v1.6.0's raycast rule is drawn-and-not
	// -liquid, so a plant can be broken while being walked through.
	CHECK(blockIsDrawn(kGrass));
	CHECK(!blockIsFullCube(kGrass));
	CHECK(blockInfo(kGrass)->shape == BLOCK_SHAPE_CROSS);
	CHECK(blockInfo(kGrass)->transparent);
	CHECK(!blockInfo(kGrass)->liquid);
	CHECK(!blockIsSolid(kGrass));
	CHECK(blockIsTargetable(kGrass));

	// The two do not share a tile. A painter wired to the wrong slot would leave every
	// count below intact and put water's art on the grass.
	{
		const AtlasRect w = atlasRect(blockFaceTex((BlockId)BLOCK_WATER, FACE_TOP));
		const AtlasRect g = atlasRect(blockFaceTex((BlockId)BLOCK_TALL_GRASS, FACE_EAST));
		CHECK(w.vslot0 != g.vslot0);
	}

	// ── Tall grass is a cross ────────────────────────────────────────────────────
	//
	// A stone floor, then one tall grass standing on it. Four quads and sixteen vertices is
	// the cross signature; six and twenty-four would be a cube, and the difference between
	// them is a solid green block on screen.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	const uint32_t floor_faces = out.faces;
	CHECK(floor_faces > 0);
	CHECK(out.opaque_faces == floor_faces);
	CHECK(countDarkVerts(&out) == 0);          // a flat floor has no darkened corner

	CHECK(worldSet(&s_world, CHUNK_DIM + 6, CHUNK_DIM + 1, CHUNK_DIM + 6, kGrass));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);

	CHECK(out.faces == floor_faces + 4);                       // two planes, two windings
	CHECK(out.vert_count == floor_faces * 4 + 16);
	CHECK(out.index_count - out.opaque_index_count == 4 * 6);  // all four are deferred
	CHECK(out.opaque_faces == floor_faces);                    // the floor kept its top face
	CHECK(countDarkVerts(&out) == 0);                          // and it darkened nothing

	// The sixteen vertices themselves: emitCross's normal and AO, and UVs on the corners of
	// tall grass's own tile rather than the stone's. Reading these off the real block is what
	// separates "the cross path works" (proved above, with a def this file wrote) from "tall
	// grass goes down the cross path".
	{
		const AtlasRect tile  = atlasRect(blockFaceTex((BlockId)BLOCK_TALL_GRASS, FACE_EAST));
		const AtlasRect stone = atlasRect(blockFaceTex(BLOCK_STONE, FACE_TOP));
		CHECK(tile.vslot0 != stone.vslot0);   // else the UV check below proves nothing
		const MeshVertex* cv = &out.verts[floor_faces * 4];
		bool nrm_ok = true, ao_ok = true, uv_ok = true;
		for (int i = 0; i < 16; i++) {
			if (cv[i].nrm != (uint8_t)FACE_TOP) nrm_ok = false;
			if (cv[i].ao != 3) ao_ok = false;
			if ((cv[i].u != tile.u0 && cv[i].u != tile.u1) ||
			    (cv[i].v != tile.vslot0 && cv[i].v != tile.vslot1))
				uv_ok = false;
		}
		CHECK(nrm_ok);
		CHECK(ao_ok);
		CHECK(uv_ok);
	}
	worldExit(&s_world);

	// ── Water is a transparent cube ──────────────────────────────────────────────
	//
	// Face CELLS, not quads, throughout: the deferred run is greedy-merged on u like the
	// opaque one, so a quad count would move when merging changed and say nothing about
	// culling. Cells are the number of block faces actually covered, which is the claim.

	// One water block alone in air: six faces, all deferred, nothing opaque. A cube, and in
	// the transparent pass — which is where the same-material cull lives, and therefore the
	// reason water can be made to cull against itself at all.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 4, CHUNK_DIM + 4, kWater));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == 0);
	CHECK(out.opaque_index_count == 0);
	CHECK(meshFaceCellsRange(&out, 0, out.faces) == 6);
	const uint32_t lone_water_cells = meshFaceCellsRange(&out, 0, out.faces);

	// Two water blocks side by side: the pair of faces they point at each other with is
	// gone. 12 - 2 = 10. This is the ocean check — without it a 16x16x16 body of water is
	// 24576 quads instead of the 1536 that face air.
	CHECK(worldSet(&s_world, CHUNK_DIM + 5, CHUNK_DIM + 4, CHUNK_DIM + 4, kWater));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.faces) == 2 * lone_water_cells - 2);
	CHECK(out.opaque_faces == 0);
	worldExit(&s_world);

	// The negative control for that cull, and the reason it has to be a control: the rule in
	// planBuild is same-ID, not "any transparent neighbour". Swap the second water for
	// leaves — also transparent, also a full cube, also deferred — and nothing is culled.
	// Without this arm, a cull that fired between any two transparent blocks would pass
	// above and would hollow out every leaf touching water.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 4, CHUNK_DIM + 4, kWater));
	CHECK(worldSet(&s_world, CHUNK_DIM + 5, CHUNK_DIM + 4, CHUNK_DIM + 4, BLOCK_LEAVES));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.faces) == 12);
	worldExit(&s_world);

	// Water does not occlude, and this is a cost being pinned rather than a win. Water is
	// not solid, so planBuild's s_occludes is false for it, so the stone under an ocean is
	// still meshed and still drawn — behind an opaque blue cube nobody can see through.
	// The only way to make water hide the seabed is to make it solid, which is a wall.
	// Recorded here so that when it is changed it is changed on purpose.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	const uint32_t bare_floor_cells = meshFaceCellsRange(&out, 0, out.opaque_faces);
	CHECK(bare_floor_cells > 0);

	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + 1, CHUNK_DIM + z,
			                     kWater));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.opaque_faces) == bare_floor_cells);
	CHECK(countDarkVerts(&out) == 0);   // and a sheet of water darkens nothing beneath it

	// The control that says the counter above can move at all, one cell at a time so the
	// answer stays arithmetic. A whole SHEET of stone is the WRONG control and was tried
	// first: it takes the floor's 256 top faces away and then contributes its own top,
	// bottom and skirt, so the opaque total goes UP, and the "<" comparison went red at
	// 1/3072 while the culling it was meant to demonstrate was working perfectly.
	//
	// One block instead. Water on a single floor cell changes nothing. Stone on the SAME
	// cell costs the floor the face under it and adds five of its own — the sixth rests on
	// the floor and is culled from both sides. -1 +5.
	worldExit(&s_world);
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 1, CHUNK_DIM + 8, kWater));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.opaque_faces) == bare_floor_cells);

	CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 1, CHUNK_DIM + 8, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCellsRange(&out, 0, out.opaque_faces) == bare_floor_cells - 1 + 5);
	worldExit(&s_world);

	// Water and tall grass in the same cell column, which is what a shoreline is. Different
	// ids, so no cull between them either, and the cross still emits its four quads next to
	// a transparent cube: 6 water faces + 4 cross quads, none of them lost.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM + 4, CHUNK_DIM + 4, kWater));
	CHECK(worldSet(&s_world, CHUNK_DIM + 5, CHUNK_DIM + 4, CHUNK_DIM + 4, kGrass));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.opaque_faces == 0);
	CHECK(out.faces == 6 + 4);
	CHECK(out.vert_count == 6 * 4 + 16);
	worldExit(&s_world);

	free(out.verts);
	free(out.indices);
}

// ── v1.6.0 task 11: greedy meshing, u axis only ──────────────────────────────
//
// What merging is allowed to do, and every place it is deliberately stopped. None of this
// is visible from the console either: a merge that ran one block too far draws a stripe of
// the next block's art, one that crossed differing AO draws a flat-shaded band where there
// should be a gradient, and one that ran past 15 blocks wraps MeshVertex.u's uint8_t and
// draws the tile from the far side of the sheet. All three ship as bad art, never as an
// error, so each is a count here.
//
// The cases are chosen so the answer is arithmetic rather than a measurement copied out of
// a run: a 16-long row of identical faces is 2 quads because the cap is 15, a checkerboard
// is as many quads as faces because no two neighbours match, and so on.
static void testMesherGreedyMerge(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	uint32_t cells = 0, quads = 0, crosses = 0;

	// --- 1. A flat one-block slab of a single material: the best case merging has.
	//
	// 576 block faces — 256 top, 256 bottom, 16 on each of the four sides — collapse to 72
	// quads. That number is arithmetic, not a reading: every direction's run is 16 blocks
	// long and the cap is 15, so each run costs two quads. Top runs along +z (16 rows of x),
	// bottom along +x (16 rows of z), and the four side strips are one 16-run each:
	// 32 + 32 + 4*2 = 72.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCells(&out) == 576);
	CHECK(out.faces == 72);
	CHECK(meshWidestQuad(&out) == ATLAS_MAX_MERGE_BLOCKS);
	CHECK(meshIndexPatternOk(&out));
	CHECK(meshCoverageOnce(&out, &cells, &crosses));
	CHECK(cells == 576 && crosses == 0);
	worldExit(&s_world);

	// --- 2. The same slab, checkerboarded between two materials.
	//
	// The control arm for case 1: identical geometry, identical face count, but no two
	// neighbours share a tile in any direction, so nothing may merge at all and the quad
	// count has to come back equal to the face count. If merging ever ignored the block id
	// this is what would go red, and case 1 alone would not.
	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++) {
			// Hoisted out of the CHECK: the stringified condition is what the suite
			// prints and stores in a fixed 96-byte buffer, and the whole call spelled
			// out is 101 characters, which -Wformat-truncation rejects under -Werror.
			const BlockId id = ((x + z) & 1) ? BLOCK_STONE : BLOCK_SAND;
			const bool    ok = worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM,
			                            CHUNK_DIM + z, id);
			CHECK_QUIET(ok);
		}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(meshFaceCells(&out) == 576);
	CHECK(out.faces == 576);              // one quad per face: nothing merged
	CHECK(meshWidestQuad(&out) == 1);
	CHECK(meshCoverageOnce(&out, &cells, NULL));
	CHECK(cells == 576);
	worldExit(&s_world);

	// --- 3. Two faces that differ only in baked AO must not merge.
	//
	// Two stone blocks side by side along +z, the axis a top face runs down. On their own
	// the two top faces are identical in every way and become one quad. Adding a single
	// block diagonally above at (5,1,4) darkens two corners of one top face and one corner
	// of the other, and nothing else changes — same tile, same direction, still adjacent.
	// The merge must stop, and the top direction must go from 1 quad to 3 (one per face,
	// plus the new block's own top).
	//
	// This is the check the brief cared most about: ~43% of quads in a real world carry
	// non-flat AO, so a merge rule that ignored shading would flatten nearly half the
	// world's contact shadows and no count anywhere else would move.
	worldInit(&s_world);
	CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM, CHUNK_DIM + 4, BLOCK_STONE));
	CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM, CHUNK_DIM + 5, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	cells = meshFaceCellsNrm(&out, FACE_TOP, &quads);
	CHECK(cells == 2 && quads == 1);      // identical: one wide quad

	CHECK(worldSet(&s_world, CHUNK_DIM + 5, CHUNK_DIM + 1, CHUNK_DIM + 4, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	cells = meshFaceCellsNrm(&out, FACE_TOP, &quads);
	CHECK(cells == 3 && quads == 3);      // AO now differs: no merge survives
	CHECK(meshCoverageOnce(&out, NULL, NULL));
	worldExit(&s_world);

	// --- 4. The 15-block cap, from world/atlas_uv.h.
	//
	// The strip atlas is one tile wide and sampled GPU_REPEAT in u, which is what lets a
	// merged quad's u run past the tile edge and still tile correctly. u is a uint8_t, so
	// 16 blocks is 16*16 = 256 px and wraps to 0 — a 16-wide quad would sample nothing at
	// all. 15 blocks is 240 and is the last value that fits.
	//
	// A run of exactly 15 eligible faces must therefore come back as one quad reaching
	// u = 240, and a run of 16 as two quads, not one wrapped one.
	worldInit(&s_world);
	for (int z = 0; z < ATLAS_MAX_MERGE_BLOCKS; z++)
		CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM, CHUNK_DIM + z,
		                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	cells = meshFaceCellsNrm(&out, FACE_TOP, &quads);
	CHECK(cells == ATLAS_MAX_MERGE_BLOCKS && quads == 1);

	uint8_t max_u = 0;
	for (uint32_t i = 0; i < out.vert_count; i++)
		if (out.verts[i].u > max_u) max_u = out.verts[i].u;
	CHECK(max_u == (uint8_t)(TILE_PX * ATLAS_MAX_MERGE_BLOCKS));   // 240, not 0

	// One more block, so 16 are eligible and only 15 may be taken.
	const int zc = CHUNK_DIM + ATLAS_MAX_MERGE_BLOCKS;
	CHECK(worldSet(&s_world, CHUNK_DIM + 4, CHUNK_DIM, zc, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	cells = meshFaceCellsNrm(&out, FACE_TOP, &quads);
	CHECK(cells == ATLAS_MAX_MERGE_BLOCKS + 1 && quads == 2);
	CHECK(meshWidestQuad(&out) == ATLAS_MAX_MERGE_BLOCKS);
	CHECK(meshCoverageOnce(&out, &cells, NULL));
	CHECK(cells == meshFaceCells(&out));
	worldExit(&s_world);

	// --- 5. One block on its own is exactly what it was before the task.
	worldInit(&s_world);
	CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + 7, CHUNK_DIM + 7, CHUNK_DIM + 7,
	                     BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);
	CHECK(meshFaceCells(&out) == 6);
	CHECK(meshWidestQuad(&out) == 1);
	worldExit(&s_world);

	free(out.verts);
	free(out.indices);
}

// Crosses are not faces and must never be merged. emitCross() bypasses the six-face loop
// entirely, so the only thing keeping its quads out of a run is that the run is built
// inside the loop it never enters — which is a structural argument, and structural
// arguments are what this project's own history says to distrust.
//
// A row of crosses along +x, on a floor whose top faces DO merge, is the pair that tells
// the difference: the floor's quad count has to fall and the crosses' must not. Four quads
// per cross, exactly, however many stand in a line.
static void testMesherCrossNeverMerges(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	BlockId cross = 0, cube = 0;
	CHECK(registerShapeBlocks(&cross, &cube));
	if (!cross || !cube) {
		restoreCoreRegistry();
		free(out.verts); free(out.indices);
		return;
	}

	static const int kRow = 8;

	worldInit(&s_world);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM, CHUNK_DIM + z,
			                     BLOCK_STONE));
	for (int x = 0; x < kRow; x++)
		CHECK_QUIET(worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + 1, CHUNK_DIM + 4,
		                     cross));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);

	// The transparent run is the crosses and nothing else — the floor is opaque stone.
	CHECK(out.faces - out.opaque_faces == (uint32_t)(kRow * 4));

	// ...and every one of those quads still spans a single tile. A merged cross would show
	// up here as a width of 2 or more even if the count above somehow held.
	bool cross_widths_ok = true;
	for (uint32_t q = out.opaque_faces; q < out.faces; q++)
		if (quadWidth(&out.verts[q * 4]) != 1) cross_widths_ok = false;
	CHECK(cross_widths_ok);

	// The control: the opaque floor under them did merge, so "nothing merged in this world"
	// cannot be why the two checks above passed.
	CHECK(out.opaque_faces < meshFaceCellsRange(&out, 0, out.opaque_faces));

	uint32_t crosses = 0;
	CHECK(meshCoverageOnce(&out, NULL, &crosses));
	CHECK(crosses == (uint32_t)(kRow * 4));

	worldExit(&s_world);
	restoreCoreRegistry();
	free(out.verts);
	free(out.indices);
}

// v1.7.1 task 49. The emit list no longer carries a cell with nothing exposed — see
// s_emit_face in world/mesher.c for the measurement that made that the shape of the fix.
// The filter is exact by construction; this is what proves it stayed exact.
//
// THE GAP THIS CLOSES. Every mesher test above starts from geometry that is visible: a
// floor, a wall, a tree, a row of plants. Not one of them asks what happens to a block that
// CANNOT be seen, because until this task nothing in the mesher treated such a block
// differently — it walked the same path as every other cell and was rejected six times over
// by the neighbour test. It is now rejected once, earlier, and by a different piece of code,
// and there are exactly two ways for that to be wrong:
//
//   * too greedy — drop a cell that should have emitted. The one shape where "no exposed
//     face" does not mean "emits nothing" is BLOCK_SHAPE_CROSS, which emitCross draws whole
//     regardless of its neighbours. A plant walled in on all six sides is not a contrived
//     case: it is a sapling with a block placed on top of it, or a flower in a hole the
//     player fills in. Nothing above would have noticed one vanishing.
//   * too shy — keep the filter honest but read the exposure from the wrong place. The mask
//     is built from the padded scratch, so a cell on the chunk's own boundary is judged
//     against the NEIGHBOURING CHUNK's block and not against air. Getting that wrong would
//     wall off every chunk seam in the world, or open every seam that should be closed.
//
// Both are checked below, each with a control arm that has to move the other way.
static void testMesherExposureFilter(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	static const int8_t kSix[6][3] = {
		{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
	};

	// --- 1. A single stone block alone in a chunk: six faces, nothing hidden. The control
	// arm for everything below — if this is not 6 the harness itself is wrong and no
	// conclusion drawn from the other cases means anything.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);
	worldExit(&s_world);

	// --- 2. The same block with all six neighbours filled in. Every face is covered, so it
	// emits nothing — and the six neighbours each keep the five faces the middle one does
	// not touch. 6 x 5 = 30 quads, none of them the middle cell's.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_STONE));
	for (int i = 0; i < 6; i++) {
		const int nx = CHUNK_DIM + 8 + kSix[i][0];
		const int ny = CHUNK_DIM + 8 + kSix[i][1];
		const int nz = CHUNK_DIM + 8 + kSix[i][2];
		CHECK_QUIET(worldSet(&s_world, nx, ny, nz, BLOCK_STONE));
	}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 30);

	// ...and no quad sits on the buried cell. A count alone cannot tell "the middle emitted
	// nothing" from "the middle emitted one face and a neighbour lost one", so the positions
	// are read: no quad may have all four corners inside the middle cell's own 1x1x1 box.
	{
		bool none_on_middle = true;
		for (uint32_t q = 0; q < out.faces; q++) {
			const MeshVertex* v = &out.verts[q * 4];
			bool all_inside = true;
			for (int i = 0; i < 4; i++)
				if (v[i].x < 8 || v[i].x > 9 || v[i].y < 8 || v[i].y > 9 ||
				    v[i].z < 8 || v[i].z > 9)
					all_inside = false;
			if (all_inside) none_on_middle = false;
		}
		CHECK(none_on_middle);
	}
	worldExit(&s_world);

	// --- 3. The chunk seam. A stone block at local x = 15 with the cell across the border
	// left as air: the +X face must still be emitted, because the mask is built from the
	// padded scratch and the scratch's border holds the real neighbour.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, CHUNK_DIM + 15, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);

	// The control arm: fill that one cell in the NEXT CHUNK ALONG and the same block drops
	// to five. Without this, the check above would pass just as well for a mesher that
	// ignored the border entirely and assumed air.
	CHECK(worldSet(&s_world, CHUNK_DIM + 16, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 5);
	worldExit(&s_world);

	// --- 4. A cross block walled in on all six sides still draws its four quads.
	//
	// The one case where "nothing exposed" must NOT mean "emits nothing". emitCross runs
	// before the six-face loop and no neighbour can hide an X, so the filter has to exempt
	// every shape that is not a full cube. A filter written without that exemption passes
	// cases 1-3 and every other mesher test in this file, and silently deletes plants.
	BlockId cross = 0, cube = 0;
	CHECK(registerShapeBlocks(&cross, &cube));
	if (cross && cube) {
		worldInit(&s_world);
		CHECK(worldSet(&s_world, CHUNK_DIM + 8, CHUNK_DIM + 8, CHUNK_DIM + 8, cross));
		for (int i = 0; i < 6; i++) {
			const int nx = CHUNK_DIM + 8 + kSix[i][0];
			const int ny = CHUNK_DIM + 8 + kSix[i][1];
			const int nz = CHUNK_DIM + 8 + kSix[i][2];
			CHECK_QUIET(worldSet(&s_world, nx, ny, nz, BLOCK_STONE));
		}
		scratchFill(&s_scratch, &s_world, 1, 1, 1);
		meshChunk(&out, &s_scratch);

		// The four cross quads are the whole transparent run and the stone shell is the
		// opaque one, so reading them apart rather than as a total is what makes this a
		// check on the plant and not on the shell around it.
		CHECK(out.faces - out.opaque_faces == 4);

		// And the shell reads 36, not case 2's 30. A cross occludes nothing, so each of the
		// six stone blocks keeps the inward face that a plain cube in the middle would have
		// covered: 6 x 6. Measured, not assumed — the first draft of this test asserted 30 on
		// the reasoning that only the middle cell had changed, and the run came back 36.
		// Writing the real number down is what makes this a control: it stops the check above
		// from passing because the whole neighbourhood got louder, and it fails just as loudly
		// if a future change lets a cross start occluding.
		CHECK(out.opaque_faces == 36);

		worldExit(&s_world);
	}
	restoreCoreRegistry();

	// --- 5. The buried case at chunk scale, which is what the measurement was actually
	// about: a chunk of solid stone inside solid stone meshes to nothing at all.
	worldInit(&s_world);
	for (int y = 0; y < 3 * CHUNK_DIM; y++)
		for (int z = 0; z < 3 * CHUNK_DIM; z++)
			for (int x = 0; x < 3 * CHUNK_DIM; x++)
				CHECK_QUIET(worldSet(&s_world, x, y, z, BLOCK_STONE));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 0);

	// The control: carve one cell out of the neighbouring chunk, right against the middle
	// chunk's face, and exactly one quad comes back. A mesher that emitted nothing because
	// it had stopped emitting anything at all would fail here.
	CHECK(worldSet(&s_world, 2 * CHUNK_DIM, CHUNK_DIM + 8, CHUNK_DIM + 8, BLOCK_AIR));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 1);
	worldExit(&s_world);

	free(out.verts);
	free(out.indices);
}

// The other two consumers of a block's shape: the player's collision box and the DDA the
// crosshair aims with. They have to disagree about a cross block — walk through it, but
// still be able to break it — and that is exactly the pair a single `solid` test cannot
// express, which is why this task exists.
static void testCrossShapeCollisionAndRaycast(void)
{
	BlockId cross = 0, cube = 0;
	CHECK(registerShapeBlocks(&cross, &cube));
	if (!cross || !cube) { restoreCoreRegistry(); return; }

	// A liquid row too: the raycast's other new term. Registered here rather than in
	// registerShapeBlocks because only this test needs it.
	BlockDef def;
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "test_water");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_SAND;
	def.flags = REG_FLAG_TRANSPARENT | REG_FLAG_LIQUID;   // full cube, drawn, not solid
	const BlockId water = registryRegister(&def);
	mesherInvalidateTables();
	CHECK(water != 0);

	worldInit(&s_world);

	// --- Collision. A floor to stand on, a cross block at (4,5,4) and a stone block at
	// (8,5,4) as the control: identical test, opposite answer.
	for (int z = 0; z < 16; z++)
		for (int x = 0; x < 16; x++)
			CHECK_QUIET(worldSet(&s_world, x, 4, z, BLOCK_STONE));
	CHECK(worldSet(&s_world, 4, 5, 4, cross));
	CHECK(worldSet(&s_world, 8, 5, 4, BLOCK_STONE));

	// The player box centred in each cell. Non-solid means the box is not blocked.
	CHECK(!bodyBlocked(&s_world, 4.5f, 5.0f, 4.5f));   // the cross: walk straight in
	CHECK(bodyBlocked(&s_world, 8.5f, 5.0f, 4.5f));    // the stone: blocked

	// And a body actually walks through it. Starting west of the cross and moving east
	// past it, the body ends up where an unobstructed body would; the same walk into the
	// stone stops short. Both arms, because a move that failed for some other reason
	// would otherwise read as "blocked".
	Body b;
	bodyInit(&b, 2.5f, 5.0f, 4.5f);
	int blocked = bodyMove(&b, &s_world, 3.0f, 0.0f, 0.0f);   // 2.5 -> 5.5, through (4,5,4)
	CHECK((blocked & BLOCKED_X) == 0);
	CHECK(b.x > 5.4f);                                        // came out the far side

	bodyInit(&b, 6.5f, 5.0f, 4.5f);
	blocked = bodyMove(&b, &s_world, 3.0f, 0.0f, 0.0f);       // 6.5 -> 9.5, into (8,5,4)
	CHECK((blocked & BLOCKED_X) != 0);
	CHECK(b.x < 8.0f);                                        // stopped at the stone

	// --- Raycast. The cross must stop the ray, or it is scenery that can never be
	// broken. Fired from a cell centre so there is no argument about the origin cell.
	RayHit h = worldRaycast(&s_world, 0.5f, 5.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit);
	CHECK(h.x == 4 && h.y == 5 && h.z == 4);       // the cross, not the stone past it
	CHECK(h.face == FACE_WEST);
	CHECK(h.px == 3 && h.py == 5 && h.pz == 4);    // and a sane cell to place into

	// Standing inside it must not jam the crosshair. The "camera is inside a wall"
	// short-circuit at the top of worldRaycast tests blockIsSolid, not targetability, so
	// a ray fired from inside the cross still reaches the stone beyond instead of
	// returning a zero-distance hit on the cell the player is standing in.
	h = worldRaycast(&s_world, 4.5f, 5.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit);
	CHECK(h.x == 8 && h.y == 5 && h.z == 4);
	CHECK(h.distance > 0.0f);

	// A liquid is drawn but not targetable: the ray goes straight through it to the
	// stone behind. Without the liquid term this would stop at (4,5,4) exactly like the
	// cross did, so the two cases genuinely separate.
	CHECK(worldSet(&s_world, 4, 5, 4, water));
	CHECK(blockIsDrawn(water));
	CHECK(!blockIsTargetable(water));
	h = worldRaycast(&s_world, 0.5f, 5.5f, 4.5f, 1.0f, 0.0f, 0.0f, 16.0f);
	CHECK(h.hit);
	CHECK(h.x == 8 && h.y == 5 && h.z == 4);

	worldExit(&s_world);
	restoreCoreRegistry();
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

// Auto-step, the refusal half.
//
// testPhysics() above proves the one-block climb works, and that a two-block wall and an
// airborne body are turned away. This covers the rest of the ways the step must decline,
// because a step-up that always succeeded would pass every success check in this file and
// still let the player walk up a cliff — the success cases alone cannot tell a working
// step from one with no refusal logic in it at all.
//
// Every case here is a matched pair wherever one can be built: the refusal, and the SAME
// geometry with the single refusing feature removed. A refusal check on its own cannot
// separate "correctly refused" from "the body never reached the obstruction", and that is
// not hypothetical — the first cost probe written for this work reset the body to x=7.6
// each frame, which at 4.3 blocks/s covers 0.0717 blocks per tick and leaves the box's
// east face at 7.9717, inside cell 7. The step at cell 8 was never touched, tryStepUp was
// never called, and the measurement came back "+0.000 extra lookups" from an arm where
// nothing had happened. The paired arms below all report a visibly different outcome from
// their partner, so neither half can be silently inert.
static void testStepUpRefusals(void)
{
	// --- No headroom over the target.
	//
	// A one-block step that is climbable on its own, plus a solid block at y=13. The
	// destination box stands with its feet at 12 and its head at 13.8, so it reaches
	// into y=13 and the probe refuses.
	//
	// This is a different shape of refusal from testPhysics()'s two-block wall. There
	// the obstruction and the thing filling the headroom are the same column, so a
	// probe that only ever looked at the step's own column would still pass. Here they
	// are separate blocks with a clear cell between them, which is what actually proves
	// the probe tests the whole standing box.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));   // floor top at y=11
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));   // the step, top at y=12
		CHECK(worldSet(&s_world, 8, 13, 5, BLOCK_STONE));   // ceiling over the destination

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;
		// Starts clear, so the stop below is the step's doing and not a body that began
		// the test already embedded in something.
		CHECK(!bodyBlocked(&s_world, 5.0f, 11.0f, 5.0f));

		const int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);

		CHECK(blocked & BLOCKED_X);
		CHECK(body.x > 7.69f && body.x < 7.71f);   // stopped flush at the step's west face
		CHECK(body.y == 11.0f);                    // never rose a millimetre
		CHECK(body.on_ground == true);             // still standing on the original floor

		worldExit(&s_world);
	}

	// --- The control for it: byte-identical world minus the y=13 block. Without this
	// arm the check above cannot tell a working headroom probe from a step-up that
	// never fires in this geometry for some unrelated reason.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));
		// No ceiling this time. That is the only difference.

		Body body;
		bodyInit(&body, 5.0f, 11.0f, 5.0f);
		body.on_ground = true;

		const int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);

		CHECK(!(blocked & BLOCKED_X));   // completed the whole move
		CHECK(body.x > 8.9f);            // and got past the step's column
		CHECK(body.y == 12.0f);          // standing on top of it, exactly

		worldExit(&s_world);
	}

	// --- Falling. A body that is not on the ground must not step, and this pins it with
	// the body placed inside the window where its box genuinely overlaps the step's row
	// (feet at 11.5, so the box spans 11.5..13.3 and the step occupies 11..12) rather
	// than merely near it. The control arm flips on_ground alone and nothing else, so
	// the difference between the two is exactly the guard being tested.
	{
		worldInit(&s_world);
		for (int x = 0; x < 16; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));

		Body body;
		bodyInit(&body, 7.6f, 11.5f, 5.0f);
		body.on_ground = false;
		body.vy = -8.0f;                 // genuinely falling, not merely un-grounded

		const int blocked = bodyMove(&body, &s_world, 0.3f, 0.0f, 0.0f);

		CHECK(blocked & BLOCKED_X);
		CHECK(body.x > 7.69f && body.x < 7.71f);
		CHECK(body.y == 11.5f);            // did not rise
		CHECK(body.on_ground == false);    // and the step did not invent a landing

		// Control: same world, same position, same delta — grounded.
		bodyInit(&body, 7.6f, 11.5f, 5.0f);
		body.on_ground = true;

		const int ok = bodyMove(&body, &s_world, 0.3f, 0.0f, 0.0f);

		CHECK(!(ok & BLOCKED_X));
		CHECK(body.x > 7.89f && body.x < 7.91f);
		CHECK(body.y == 12.5f);            // rose exactly one block

		worldExit(&s_world);
	}

	// --- The same rule under a real fall driven by gravity, rather than by setting
	// on_ground by hand. This is the arm that proves resolveY's own bookkeeping clears
	// on_ground early enough in the substep that the horizontal resolve which follows it
	// cannot step — the hand-set version above would still pass if resolveY forgot.
	//
	// The body starts inside the overlap window on purpose. Dropped from higher up it
	// sails past the step's column before it has fallen far enough to touch it, and
	// blocked_airborne comes back 0: measured, and the reason that counter is asserted
	// non-zero below instead of just being printed.
	//
	// "Still airborne when the tick ended" is the predicate, not "was airborne when it
	// began". A tick that begins airborne and ends grounded is a landing, and a body that
	// lands at the foot of a step and walks up it within that same tick has not stepped
	// in mid-air — on_ground was made true by a genuine resolveY landing in that very
	// substep. Scoring the start state alone reports a spurious 0.907-block "airborne
	// rise" for exactly that legitimate tick; measured, then corrected to this.
	{
		worldInit(&s_world);
		for (int x = 0; x < 32; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		CHECK(worldSet(&s_world, 8, 11, 5, BLOCK_STONE));

		Body body;
		bodyInit(&body, 7.6f, 11.9f, 5.0f);
		body.on_ground = false;
		body.vy = -1.0f;

		int airborne_ticks = 0, blocked_airborne = 0, rose_airborne = 0;

		for (int i = 0; i < 240; i++) {
			body.vx = PLAYER_WALK_SPEED;
			const bool was_air = !body.on_ground;
			const float y0 = body.y;
			const int blk = bodyStep(&body, &s_world, 1.0f / 60.0f);
			if (was_air) {
				airborne_ticks++;
				if (blk & BLOCKED_X) blocked_airborne++;
				if (!body.on_ground && body.y > y0) rose_airborne++;
			}
		}

		CHECK(airborne_ticks > 0);       // it really did spend time off the ground
		CHECK(blocked_airborne > 0);     // and really was pressed into the step while there
		CHECK(rose_airborne == 0);       // and never gained height while off the ground
		CHECK(body.on_ground == true);   // ended up settled, not still falling
		CHECK(body.y == 11.0f);          // on the floor, not on top of the step

		worldExit(&s_world);
	}

	// --- A plant is not a stair.
	//
	// Cross-shaped blocks are registered non-solid, so bodyBlocked ignores them, no
	// horizontal resolve ever reports blocked, and tryStepUp is therefore never even
	// called. The failure this guards against is a step-up keyed on "is something drawn
	// here" rather than on "is something solid here", which would make the player hop up
	// onto every flower they walked into.
	//
	// The solid cube from the same helper is the control: same dynamic id range, same
	// registration call, differing only in shape and solidity — so a difference in
	// outcome is about the block, not about high ids behaving oddly.
	{
		BlockId cross = 0, cube = 0;
		CHECK(registerShapeBlocks(&cross, &cube));
		if (cross && cube) {
			CHECK(!blockIsSolid(cross));
			CHECK(blockIsSolid(cube));

			worldInit(&s_world);
			for (int x = 0; x < 16; x++)
				for (int z = 0; z < 12; z++)
					CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));
			CHECK(worldSet(&s_world, 8, 11, 5, cross));   // a plant standing on the floor

			Body body;
			bodyInit(&body, 5.0f, 11.0f, 5.0f);
			body.on_ground = true;

			int blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);

			CHECK(!(blocked & BLOCKED_X));   // walked straight through it
			CHECK(body.x > 8.9f);
			CHECK(body.y == 11.0f);          // and was never lifted by it

			// The control: swap that one cell for the solid cube and nothing else. Now
			// it IS an obstruction, and now the step-up is allowed to fire.
			CHECK(worldSet(&s_world, 8, 11, 5, cube));
			bodyInit(&body, 5.0f, 11.0f, 5.0f);
			body.on_ground = true;

			blocked = bodyMove(&body, &s_world, 4.0f, 0.0f, 0.0f);

			CHECK(!(blocked & BLOCKED_X));
			CHECK(body.y == 12.0f);          // climbed the cube, having ignored the plant

			worldExit(&s_world);
		}
		restoreCoreRegistry();
	}

	// --- A diagonal squeeze between two blocks.
	//
	// Two two-block walls meeting at a corner, with the far diagonal cell deliberately
	// left empty. The body sits in the inside corner and pushes into both at once. The
	// tempting bug is a step-up that writes both horizontal coordinates at once and so
	// slips the body through the diagonal seam that neither axis alone would open; the
	// real tryStepUp takes the current value for the axis it is not resolving, which is
	// what keeps this shut. Both axes must report blocked and the body must not rise.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));

		CHECK(worldSet(&s_world, 6, 11, 5, BLOCK_STONE));
		CHECK(worldSet(&s_world, 6, 12, 5, BLOCK_STONE));   // +x wall, two tall
		CHECK(worldSet(&s_world, 5, 11, 6, BLOCK_STONE));
		CHECK(worldSet(&s_world, 5, 12, 6, BLOCK_STONE));   // +z wall, two tall
		// (6,_,6) is left empty on purpose: that is the diagonal gap.

		Body body;
		bodyInit(&body, 5.5f, 11.0f, 5.5f);
		body.on_ground = true;

		const int blocked = bodyMove(&body, &s_world, 0.3f, 0.0f, 0.3f);

		CHECK(blocked & BLOCKED_X);
		CHECK(blocked & BLOCKED_Z);
		CHECK(body.x > 5.69f && body.x < 5.71f);   // stopped flush on x
		CHECK(body.z > 5.69f && body.z < 5.71f);   // and flush on z
		CHECK(body.y == 11.0f);                    // and did not climb out over the top

		worldExit(&s_world);
	}

	// --- A body that is already inside a wall must not be lifted out through it.
	//
	// The step-up probes the destination, not the origin, so a body that starts
	// embedded is a case the probe cannot reason about from its own result. What must
	// hold is the modest thing: it does not rise. Here the wall is four blocks tall, so
	// the destination at y+1 is inside it too and the probe refuses on its own terms.
	{
		worldInit(&s_world);
		for (int x = 0; x < 12; x++)
			for (int z = 0; z < 12; z++)
				CHECK_QUIET(worldSet(&s_world, x, 10, z, BLOCK_STONE));
		for (int y = 11; y <= 14; y++)
			CHECK(worldSet(&s_world, 5, y, 5, BLOCK_STONE));

		Body body;
		bodyInit(&body, 5.5f, 11.0f, 5.5f);
		body.on_ground = true;
		CHECK(bodyBlocked(&s_world, 5.5f, 11.0f, 5.5f));   // genuinely embedded to begin with

		const int blocked = bodyMove(&body, &s_world, 0.3f, 0.0f, 0.0f);

		CHECK(blocked & BLOCKED_X);
		CHECK(body.y == 11.0f);   // did not climb the wall it was stuck in

		worldExit(&s_world);
	}

	// --- The regression that matters most: a long walk up a real staircase, checking
	// every tick that the body is never below the surface of the column it is standing
	// over. Falling through the world is the worst failure this module can have, and the
	// flat-floor 18,000-tick check in testPhysics() cannot see it because nothing there
	// ever invokes the step-up. This walks 16 rises — the body climbs from y=11 to y=27
	// — with tryStepUp firing repeatedly, and asserts the floor was never breached.
	//
	// The climb total is asserted too. Without it a step-up that silently stopped working
	// would leave the body stuck against the first riser, never below any surface, and
	// the check would pass while proving nothing.
	{
		worldInit(&s_world);
		bool built = true;
		for (int x = 0; x < 36; x++)
			for (int z = 0; z < 10; z++) {
				const int h = 10 + x / 2;             // one block up every two columns
				for (int y = 0; y <= h; y++)
					if (!worldSet(&s_world, x, y, z, BLOCK_STONE)) built = false;
			}
		CHECK(built);

		Body body;
		bodyInit(&body, 0.5f, 11.0f, 5.5f);
		body.on_ground = true;

		float lowest = 9999.0f;
		int below_surface = 0, ticks = 0;

		for (int i = 0; i < 2000; i++) {
			if (body.x > 32.0f) break;   // stay well inside the built area
			body.vx = PLAYER_WALK_SPEED;
			bodyStep(&body, &s_world, 1.0f / 60.0f);
			ticks++;
			if (body.y < lowest) lowest = body.y;
			// The top surface of the column the body's centre is over.
			const float top = (float)(10 + (int)body.x / 2) + 1.0f;
			if (body.y < top - 0.001f) below_surface++;
		}

		CHECK(ticks > 400);              // it really did run, and for a long time
		CHECK(below_surface == 0);       // never once beneath the surface under it
		CHECK(lowest == 11.0f);          // and never below the height it started at
		CHECK(body.y >= 26.0f);          // climbed at least 15 risers by auto-step alone
		CHECK(body.on_ground == true);   // finished standing on the staircase, not in it

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
	// Both chunks were just created by worldChunkCreate and never written to, so both are
	// still CHUNK_FORM_UNIFORM — chunkFormBytes(CHUNK_FORM_UNIFORM), not the old fixed
	// sizeof(Chunk), is what worldChunkCreate actually claimed for each (see world.c).
	CHECK(used_one == sizeof(Column) + 2 * chunkFormBytes(CHUNK_FORM_UNIFORM));

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

// The refusal path testWorldSetBudgetExhausted covers is worldChunkCreate's -- a brand new
// chunk that cannot even be born. This is the OTHER budget-checked path step 9.2a added to
// worldSet: an existing chunk whose write requires a promotion (UNIFORM -> PALETTE4 here),
// and the budget refuses the delta. The chunk must come back exactly as it went in: same
// form, same bytes, same content, same total budget used -- there must be no path where the
// claim succeeds partially or chunkSet runs anyway after a refused claim.
static void testChunkPromotionBudgetRefused(void)
{
	worldInit(&s_world);

	// worldChunkCreate directly, not worldSet with a real block: worldSet(..., BLOCK_STONE)
	// on a chunk that does not exist yet would create it UNIFORM-AIR and then immediately
	// write a distinct id into it, promoting it to PALETTE4 in the very same call -- there
	// would be no existing UNIFORM chunk left to test a promotion refusal against. Creating
	// the chunk directly, still holding nothing but air, is the only way to get a real,
	// budget-tracked, already-existing UNIFORM chunk to promote out of.
	CHECK(worldChunkCreate(&s_world, 0, 0, 0) != NULL);
	Chunk* c = worldChunk(&s_world, 0, 0, 0);
	CHECK(c != NULL);
	CHECK(chunkGetForm(c) == CHUNK_FORM_UNIFORM);

	const ChunkForm form_before      = chunkGetForm(c);
	const size_t     chunk_bytes_before = chunkGetBytes(c);
	const size_t     world_bytes_before = worldBytes(&s_world);
	const size_t     used_before        = budgetUsed();

	// A UNIFORM -> PALETTE4 promotion costs 2,065 B (testChunkFormBytes pins the exact
	// number); leaving only 100 B in the budget guarantees the claim in worldSet's pre-flight
	// (world.c) is refused, not merely made tight.
	const size_t remaining = budgetCap() - budgetUsed();
	CHECK(remaining > 100);
	CHECK(budgetClaim(remaining - 100) == true);

	// Same cell, a different id -- this is a promotion on an EXISTING chunk, not a creation,
	// so it exercises worldSet's chunkFormFor/chunkSet pre-flight path specifically.
	CHECK(worldSet(&s_world, 4, 4, 4, BLOCK_DIRT) == false);

	CHECK(chunkGetForm(c)       == form_before);
	CHECK(chunkGetBytes(c)      == chunk_bytes_before);
	CHECK(worldBytes(&s_world)  == world_bytes_before);
	CHECK(budgetUsed()          == used_before + (remaining - 100));   // the refused claim took nothing
	CHECK(worldGet(&s_world, 4, 4, 4) == BLOCK_AIR);                   // untouched: still air, never written

	budgetRelease(remaining - 100);
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
// Step 8.1. The dirty flag decides which columns reach the SD card, so the thing worth
// checking is not that it can be set — it is that generating a column does NOT set it. If it
// did, every column in the world would be written on the way out and the save file would grow
// by the whole world instead of by the parts of it the player changed.
static void testColumnDirtyFlag(void)
{
	worldInit(&s_world);

	// A column filled the way the generator fills one: straight through worldSet, which is
	// the shared path and therefore the tempting place to have put the flag.
	for (int y = 0; y < 20; y++) worldSet(&s_world, 3, y, 3, BLOCK_STONE);

	const Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(col && !col->dirty);

	// And the edit path does set it, for the column containing the block.
	worldSet(&s_world, 3, 21, 3, BLOCK_WOOD);
	worldMarkDirty(&s_world, 3, 3);
	CHECK(col && col->dirty);

	// Negative coordinates land in column -1, not column 0: the shift has to floor. Getting
	// this wrong would mark the wrong column and lose the edited one on unload.
	worldSet(&s_world, -1, 21, -1, BLOCK_WOOD);
	worldMarkDirty(&s_world, -1, -1);
	const Column* west = worldColumn(&s_world, -1, -1);
	CHECK(west != NULL);
	CHECK(west && west->dirty);
	CHECK(west != col);

	// A column that is not loaded is not an error and must not fault.
	worldMarkDirty(&s_world, 4096, 4096);

	worldExit(&s_world);
}

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
// v1.8.0 task 21 renamed this from TICK_HZ. It was never a tick rate — it is the measured 3DS
// VBlank rate, and player physics runs on the render frame, not on the simulation tick. The old
// name collided with world/tick.h's real TICK_HZ (20) the moment a tick existed, which is a fair
// summary of why the rest of this rung was needed: "tick" and "frame" had been the same word.
#define FRAME_HZ  59.83f   // measured 3DS VBlank rate, not the nominal 60

static void testControlFeel(void)
{
	const float dt = 1.0f / FRAME_HZ;

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

// v1.8.0 task 25 — buoyancy. The probe first (bodySubmerged), then the two things water
// does to a falling body: a weaker pull, and a much lower terminal speed.
//
// Every case here is written as TWO ARMS over the same fixture — an identical body in air
// and in water, stepped with the same dt — because "a body in water fell 0.13 blocks" is
// not a claim that can go red on its own. The difference between the arms is.
static void testBuoyancy(void)
{
	const float dt = 1.0f / 60.0f;

	// A pool of water from y=4..9 sitting on a stone floor at y=3, and a matching dry
	// column of air beside it. Same floor, same height, so the only difference between
	// the two arms is what fills the cells.
	worldInit(&s_world);
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++)
			CHECK(worldSet(&s_world, x, 3, z, BLOCK_STONE));
	for (int y = 4; y <= 9; y++)
		for (int x = 0; x < 8; x++)
			for (int z = 0; z < 16; z++)
				CHECK_QUIET(worldSet(&s_world, x, y, z, BLOCK_WATER));

	// --- The probe. Feet, eyes, and the two cases that are why the eye cell is asked
	// about at all rather than the feet alone.
	{
		Body wet;  bodyInit(&wet, 4.5f, 6.0f, 4.5f);    // both cells inside the pool
		Body dry;  bodyInit(&dry, 12.5f, 6.0f, 12.5f);  // both cells in open air
		CHECK(bodySubmerged(&s_world, &wet) == true);
		CHECK(bodySubmerged(&s_world, &dry) == false);

		// Feet in the pool's top cell (y=9), eyes at 10.62 which is open air above it.
		Body wading; bodyInit(&wading, 4.5f, 9.0f, 4.5f);
		CHECK(bodySubmerged(&s_world, &wading) == true);

		// The opposite: feet in air at y=3.2 under an overhanging water cell, head at
		// 4.82 inside it. A feet-only probe reads this as dry.
		CHECK(worldSet(&s_world, 12, 4, 12, BLOCK_WATER));
		Body headfirst; bodyInit(&headfirst, 12.5f, 3.2f, 12.5f);
		CHECK(bodySubmerged(&s_world, &headfirst) == true);
		CHECK(worldSet(&s_world, 12, 4, 12, BLOCK_AIR));

		// And a body standing on the floor beside the pool is dry, which is the control
		// that says the probe is not simply answering true.
		Body onfloor; bodyInit(&onfloor, 12.5f, 4.0f, 12.5f);
		CHECK(bodySubmerged(&s_world, &onfloor) == false);

		// v1.8.2 — the same four bodies through bodyWetState, which is now what the
		// boolean is a wrapper over. The wrapper collapses SURFACE and SUBMERGED into
		// one answer; these checks are the only place the difference between them is
		// visible, and the vertical input is the only thing that reads it.
		CHECK(bodyWetState(&s_world, &wet)     == BODY_SUBMERGED);
		CHECK(bodyWetState(&s_world, &dry)     == BODY_DRY);
		CHECK(bodyWetState(&s_world, &wading)  == BODY_SURFACE);   // feet in, eyes out
		CHECK(bodyWetState(&s_world, &onfloor) == BODY_DRY);

		// Head under with air below the feet is SUBMERGED, not SURFACE: the state is
		// named for where the EYES are. A rule written off the feet alone would call
		// this dry and a rule written off "both cells" would have nothing to say.
		CHECK(worldSet(&s_world, 12, 4, 12, BLOCK_WATER));
		Body underlip; bodyInit(&underlip, 12.5f, 3.2f, 12.5f);
		CHECK(bodyWetState(&s_world, &underlip) == BODY_SUBMERGED);
		CHECK(worldSet(&s_world, 12, 4, 12, BLOCK_AIR));
	}

	// --- One tick of gravity, from rest, in each medium. Water must pull less.
	{
		Body wet;  bodyInit(&wet, 4.5f, 7.0f, 4.5f);
		Body dry;  bodyInit(&dry, 12.5f, 7.0f, 12.5f);
		bodyStep(&wet, &s_world, dt);
		bodyStep(&dry, &s_world, dt);

		const float wet_vy = wet.vy < 0.0f ? -wet.vy : wet.vy;
		const float dry_vy = dry.vy < 0.0f ? -dry.vy : dry.vy;
		CHECK(wet_vy < dry_vy);          // the load-bearing one
		CHECK(dry.vy < 0.0f);            // control: the dry arm really is falling
		CHECK(wet.vy < 0.0f);            // and the wet arm sinks, it does not float up

		// The exact numbers, so a change to either constant has to be deliberate.
		// -28 * 1/60 = -0.4667 dry, -8 * 1/60 = -0.1333 wet.
		//
		// This band moved twice inside v1.8.2 and is now back where it started, which is
		// worth recording rather than quietly restoring. Attempt 1 applied
		// PLAYER_WATER_VDRAG to every wet tick, which took 1/11 of the first tick's
		// gravity straight back off and read -0.121212 here; the band was widened to
		// match. Attempt 2 confines the drag to an UNDRIVEN UPWARD coast, so a body
		// sinking from rest is not dragged at all and the tick is a pure -8/60 again.
		// Measured -0.133333.
		CHECK(dry.vy > -0.47f && dry.vy < -0.46f);
		CHECK(wet.vy > -0.14f && wet.vy < -0.13f);
	}

	// --- Terminal speed. Sixty ticks of free fall in each medium; the dry arm is heading
	// for PLAYER_TERMINAL, the wet arm must be sitting exactly on PLAYER_WATER_TERMINAL.
	// Started high enough in each column that neither reaches the floor inside the run.
	//
	// The equality is load-bearing and is the second half of the drag re-scoping above.
	// Attempt 1's whole-tick drag gave a free sink a SECOND terminal of its own, -8/6 =
	// -1.3333, reached before the -1.5 clamp could ever fire — which quietly retired the
	// constant that is supposed to own the sink rate and made sinking 11% slower than
	// v1.8.1 for no reason anybody asked for. Measured -1.328954 then, -1.500000 now.
	// If this ever reads "just short of" the clamp again, a drag has leaked back onto the
	// downward half.
	{
		Body wet;  bodyInit(&wet, 4.5f, 9.0f, 4.5f);
		Body dry;  bodyInit(&dry, 12.5f, 40.0f, 12.5f);
		for (int i = 0; i < 60; i++) {
			bodyStep(&wet, &s_world, dt);
			bodyStep(&dry, &s_world, dt);
		}
		CHECK(wet.vy == PLAYER_WATER_TERMINAL);         // the clamp owns the sink rate
		CHECK(dry.vy < PLAYER_WATER_TERMINAL * 4.0f);   // control: nothing clamped the dry arm
		CHECK(wet.y > 4.0f);                            // still in the pool, not resting on the floor
	}

	// --- Entering water at speed. A body plunging at terminal must be caught on the
	// first submerged tick, not carry its dry momentum to the bottom. This is the case
	// that a gravity-only change (no terminal branch) would not catch.
	{
		Body b; bodyInit(&b, 4.5f, 9.5f, 4.5f);
		b.vy = PLAYER_TERMINAL;                 // falling at -60, about to touch the surface
		CHECK(bodySubmerged(&s_world, &b) == true);
		bodyStep(&b, &s_world, dt);
		CHECK(b.vy == PLAYER_WATER_TERMINAL);
	}

	worldExit(&s_world);
}

// v1.8.0 task 25 — swimming. bodyJump()'s truth table, then the thing it exists for:
// rising through water with nothing under the feet.
static void testSwimming(void)
{
	const float dt = 1.0f / 60.0f;

	worldInit(&s_world);
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++)
			CHECK(worldSet(&s_world, x, 3, z, BLOCK_STONE));
	for (int y = 4; y <= 12; y++)
		for (int x = 0; x < 8; x++)
			for (int z = 0; z < 16; z++)
				CHECK_QUIET(worldSet(&s_world, x, y, z, BLOCK_WATER));

	// --- bodyJump's truth table, with no stepping at all, so each row is about the
	// decision and nothing else.
	{
		// Land, grounded, edge press: the ordinary jump, unchanged by this task.
		Body b; bodyInit(&b, 12.5f, 4.0f, 12.5f);
		b.on_ground = true;
		bodyJump(&b, BODY_DRY, true, true, dt);
		CHECK(b.vy == PLAYER_JUMP_SPEED);

		// Land, grounded, button merely HELD with no fresh press: nothing. This is the
		// half that stops a held button flying, and it must survive the new branch.
		bodyInit(&b, 12.5f, 4.0f, 12.5f);
		b.on_ground = true;
		bodyJump(&b, BODY_DRY, true, false, dt);
		CHECK(b.vy == 0.0f);

		// Land, airborne, edge press: nothing. No double jump.
		bodyInit(&b, 12.5f, 8.0f, 12.5f);
		b.on_ground = false;
		bodyJump(&b, BODY_DRY, true, true, dt);
		CHECK(b.vy == 0.0f);

		// Water, airborne, button held: rises. NOT gated on on_ground — a swimmer has
		// nothing under it, and a land-jump gate copied onto this branch is the whole
		// reason the check above and this one are stated separately.
		//
		// v1.8.2 re-pin, and the single most important line in this test: it used to
		// read `b.vy == PLAYER_SWIM_UP_SPEED` after ONE call. That assignment, fired
		// every frame from whatever vy happened to be, is the bang-bang relay that made
		// the surface judder. It is now an approach: 3.0 * (10*dt / (1 + 10*dt)) with
		// dt = 1/60 is 0.428571, measured. The old exact-equality assertion is what
		// would have to go back for the bug to return, so it is the sabotage arm.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		b.on_ground = false;
		bodyJump(&b, BODY_SUBMERGED, true, false, dt);
		CHECK(b.vy > 0.0f);                                 // it does lift
		CHECK(b.vy < PLAYER_SWIM_UP_SPEED * 0.5f);          // but nowhere near all at once
		CHECK(b.vy > 0.4280f && b.vy < 0.4292f);            // the measured step

		// ...and it converges. bodyJump alone, no gravity and no drag, so this is the
		// approach on its own: 23 calls to get within 3% of the target. That is ~0.38 s
		// at 60 fps, which is the "reached in about a third of a second" the design
		// traded the instant clamp for.
		{
			Body a; bodyInit(&a, 4.5f, 8.0f, 4.5f);
			int n = 0;
			bool monotonic = true;
			float prev = a.vy;
			while (n < 200 && a.vy < 2.9f) {
				bodyJump(&a, BODY_SUBMERGED, true, false, dt);
				if (a.vy <= prev) monotonic = false;
				prev = a.vy;
				n++;
			}
			CHECK(monotonic);                               // never overshoots, never dips
			CHECK(n == 23);
			CHECK(a.vy < PLAYER_SWIM_UP_SPEED);             // approaches from below, never past
		}

		// Water, button released: no rise, and the sink is left alone.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		b.vy = -1.0f;
		bodyJump(&b, BODY_SUBMERGED, false, false, dt);
		CHECK(b.vy == -1.0f);

		// Water, already rising faster than the swim speed (kicked off the bottom):
		// holding the button must not SLOW it down to the cap.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		b.vy = PLAYER_SWIM_UP_SPEED + 4.0f;
		bodyJump(&b, BODY_SUBMERGED, true, true, dt);
		CHECK(b.vy == PLAYER_SWIM_UP_SPEED + 4.0f);

		// v1.8.2 — the surface row, which had no equivalent before because there was no
		// surface state. Same button, same water, head out: the target is
		// PLAYER_SURFACE_RISE (0.0) instead of PLAYER_SWIM_UP_SPEED, so a body already
		// at rest is left exactly at rest rather than being thrown upward. THIS is the
		// line the bug was: at the surface the old code still asserted +3.0.
		//
		// swim_drive is the attempt-2 half and it is asserted here rather than only
		// through behaviour, because it is the flag that tells bodyStep to leave gravity
		// off. A body sitting exactly ON its target must still be DRIVEN — that is the
		// difference between `<=` and `<` in bodyJump, and without it vy = 0 is not a
		// fixed point at all, it is a point the body falls away from at -8 m/s^2.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		bodyJump(&b, BODY_SURFACE, true, false, dt);
		CHECK(b.vy == 0.0f);
		CHECK(b.swim_drive == true);

		// ...and released, it is not driven, so gravity is back on the next bodyStep.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		bodyJump(&b, BODY_SURFACE, false, false, dt);
		CHECK(b.swim_drive == false);

		// Nor on land, at any input: a grounded jump is an impulse, not a drive, and if
		// this ever set the flag the player would hang in mid-air on the way up.
		bodyInit(&b, 12.5f, 4.0f, 12.5f);
		b.on_ground = true;
		bodyJump(&b, BODY_DRY, true, true, dt);
		CHECK(b.vy == PLAYER_JUMP_SPEED);
		CHECK(b.swim_drive == false);

		// And a body sinking at the surface is pulled back UP towards 0, not slammed to
		// it: one frame closes 1/7 of the gap, not the whole gap.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		b.vy = -1.0f;
		bodyJump(&b, BODY_SURFACE, true, false, dt);
		CHECK(b.vy > -1.0f);                                // rising
		CHECK(b.vy < 0.0f);                                 // but still sinking this frame
		CHECK(b.vy > -0.8580f && b.vy < -0.8567f);          // -1 * (1 - 10dt/(1+10dt))

		// Surface, rising fast: the `vy <= target` guard means the surface target never
		// BRAKES a body. A swimmer arriving from below with +3 on him keeps it and loses
		// it to gravity and drag, which is what makes the arrival read as a float rather
		// than as hitting a ceiling. He is also NOT driven while he does — that coast is
		// the only stretch where PLAYER_WATER_VDRAG is allowed to act, and it is what
		// places the resting float about half a block above the waterline.
		bodyInit(&b, 4.5f, 8.0f, 4.5f);
		b.vy = PLAYER_SWIM_UP_SPEED;
		bodyJump(&b, BODY_SURFACE, true, false, dt);
		CHECK(b.vy == PLAYER_SWIM_UP_SPEED);
		CHECK(b.swim_drive == false);
	}

	// --- The hysteresis band, on its own, with no motion involved. PLAYER_WET_HYSTERESIS
	// is one-sided and sits ABOVE the waterline (top face of the y=12 water block, 13.0),
	// so:
	//   entering  is instant — eye under the line at all means BODY_SUBMERGED, because a
	//             body reading BODY_SURFACE with its eyes underwater would hover there
	//             with a target of 0 and never come up, which is the opposite of the point;
	//   leaving   takes a further PLAYER_WET_HYSTERESIS of clearance, which is what stops
	//             a body resting on the boundary from chattering across it, and what puts
	//             the resting float visibly clear of the surface instead of level with it.
	{
		// Eye at 13.10 — a tenth of a block clear of the water. A body that was already
		// submerged stays submerged, because 13.10 - 0.25 is still inside the water.
		Body b; bodyInit(&b, 4.5f, 13.10f - PLAYER_EYE, 4.5f);
		b.wet = BODY_SUBMERGED;
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SUBMERGED);
		CHECK(bodyWetState(&s_world, &b) == BODY_SURFACE);   // the unbiased reading differs
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SUBMERGED); // idempotent at a fixed y

		// The same position approached from the other side stays at the surface: no band
		// resists going up, so nothing drags a floating body back under.
		bodyInit(&b, 4.5f, 13.10f - PLAYER_EYE, 4.5f);
		b.wet = BODY_SURFACE;
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SURFACE);

		// Clear of the band: submerged no longer holds, whatever it came from.
		bodyInit(&b, 4.5f, 13.40f - PLAYER_EYE, 4.5f);
		b.wet = BODY_SUBMERGED;
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SURFACE);

		// Under the line: submerged immediately, with no band to climb through first.
		bodyInit(&b, 4.5f, 12.99f - PLAYER_EYE, 4.5f);
		b.wet = BODY_SURFACE;
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SUBMERGED);
		bodyInit(&b, 4.5f, 12.99f - PLAYER_EYE, 4.5f);
		b.wet = BODY_DRY;
		CHECK(bodyWetUpdate(&s_world, &b) == BODY_SUBMERGED);

		// Control: the band is on the EYE only. Dry land is dry from any history, or a
		// band on the feet probe would let a body stand a quarter-block into a pond
		// without the water noticing.
		Body d; bodyInit(&d, 12.5f, 8.0f, 12.5f);
		d.wet = BODY_SUBMERGED;
		CHECK(bodyWetUpdate(&s_world, &d) == BODY_DRY);
	}

	// --- The approach must never overshoot its target, at any tick length. The body runs
	// on the RENDER frame (scene/player.c hands bodyStep metricsFrameMs(), clamped to its
	// MAX_TICK of 0.05), so dt is 16.7 ms on a good frame and up to 50 ms on a bad one —
	// but bodyStep is a public entry point and main.c's walk-stress probe steps it too,
	// so "the caller will clamp it" is not something this function gets to assume.
	//
	// Deliberately taken well past MAX_TICK: at dt = 1.0 a bare `rate * dt` of 10 would
	// put the body at ten times its own target, and at dt = 10 it would fling it
	// backwards. dampAlpha is bounded in [0, 1) for every non-negative dt, so the worst
	// a long tick can do is arrive.
	//
	// This sweep is the ONLY witness in the suite for dampAlpha's rational form, measured:
	// with `return kdt;` put back in physics.c it goes red three times over, at 0.25, 1.0
	// and 10.0, and nothing else in the surface tests notices. The 0.05 row is green in
	// both arms — 10 * 0.05 is 0.5, which the naive form survives — so it is stated
	// separately below as the control rather than being counted as evidence.
	{
		const float ticks[] = { 0.05f, 0.25f, 1.0f, 10.0f };
		for (int i = 0; i < 4; i++) {
			Body worst; bodyInit(&worst, 4.5f, 6.0f, 4.5f);
			bodyJump(&worst, BODY_SUBMERGED, true, false, ticks[i]);
			CHECK_QUIET(worst.vy > 0.0f);
			CHECK_QUIET(worst.vy <= PLAYER_SWIM_UP_SPEED);
		}
		Body sane; bodyInit(&sane, 4.5f, 6.0f, 4.5f);
		bodyJump(&sane, BODY_SUBMERGED, true, false, 0.05f);
		CHECK(sane.vy > 0.0f && sane.vy < PLAYER_SWIM_UP_SPEED);   // control, spelled loudly
	}

	// --- Swimming up, mid-water, nothing under the feet. Sixty frames of held jump.
	//
	// This bound has now moved twice inside v1.8.2 and BOTH numbers are stated, because
	// the middle one was a regression nobody asked for and the only way to stop it coming
	// back is to leave it written down:
	//
	//     v1.8.1        2.866667 blocks/s steady, 2.866668 blocks over the second
	//     attempt 1     1.215686 blocks/s steady, 1.144175 over the second   <-- 58% SLOWER
	//     attempt 2     2.999712 blocks/s steady, 2.700029 over the second
	//
	// Attempt 1 lost it two ways at once: PLAYER_WATER_VDRAG damped the driven climb as
	// well as the coast, and the approach drooped by WATER_GRAVITY/SWIM_RESPONSE below its
	// own target. Attempt 2 removes both from a DRIVEN frame, so the steady climb is now
	// PLAYER_SWIM_UP_SPEED itself rather than something derived from three constants.
	// The steady rate is 4.6% above v1.8.1's, since v1.8.1 paid one tick of gravity per
	// frame on top of its hard clamp; the first second is 6% short of it, because the
	// approach has to ramp where the clamp did not.
	{
		Body b; bodyInit(&b, 4.5f, 6.0f, 4.5f);
		const float y0 = b.y;
		float prev = b.y;
		bool  monotonic = true;
		for (int i = 0; i < 60; i++) {
			CHECK_QUIET(b.on_ground == false);   // never touched anything to rise off
			bodyJump(&b, bodyWetUpdate(&s_world, &b), true, i == 0, dt);
			bodyStep(&b, &s_world, dt);
			if (b.y <= prev) monotonic = false;
			prev = b.y;
		}
		CHECK(monotonic);
		CHECK(b.y > y0 + 2.6f);            // a real rise, at roughly the v1.8.1 rate
		CHECK(b.y < y0 + 2.75f);           // measured 2.700029 over the second
		CHECK(b.vy > 2.99f && b.vy < 3.001f);  // a STEADY climb at the swim speed, 2.999712
		CHECK(b.vy <= PLAYER_SWIM_UP_SPEED);   // and it never passes it
	}

	// --- The control arm that proves the rise above is the water branch and not the
	// input: the identical loop in the dry column, starting mid-air, must FALL.
	{
		Body b; bodyInit(&b, 12.5f, 20.0f, 12.5f);
		const float y0 = b.y;
		for (int i = 0; i < 60; i++) {
			bodyJump(&b, bodyWetUpdate(&s_world, &b), true, i == 0, dt);
			bodyStep(&b, &s_world, dt);
		}
		CHECK(b.y < y0);
		CHECK(b.y < y0 - 14.2f && b.y > y0 - 14.3f);   // -14.233344, untouched by v1.8.2
	}

	// --- Sinking. Released button in water: the body drifts down, and slowly. Sixty
	// frames at the drag terminal is about 1.1 blocks, against 14-odd dry.
	{
		Body wet; bodyInit(&wet, 4.5f, 11.0f, 4.5f);
		const float y0 = wet.y;
		for (int i = 0; i < 60; i++) {
			bodyJump(&wet, bodyWetUpdate(&s_world, &wet), false, false, dt);
			bodyStep(&wet, &s_world, dt);
		}
		CHECK(wet.y < y0);                 // it does sink
		CHECK(wet.y > y0 - 1.6f);          // but no faster than water allows — unmoved bound
	}

	// --- Horizontal speed. bodyWalkSpeed's own answer, and then the displacement it
	// actually produces, so an ordering mistake at the call site cannot hide behind a
	// correct-looking constant.
	{
		CHECK(bodyWalkSpeed(false) == PLAYER_WALK_SPEED);
		CHECK(bodyWalkSpeed(true) < PLAYER_WALK_SPEED);
		CHECK(bodyWalkSpeed(true) == PLAYER_WALK_SPEED * PLAYER_WATER_SPEED_MUL);

		Body wet;  bodyInit(&wet, 1.5f, 6.0f, 4.5f);
		Body dry;  bodyInit(&dry, 9.5f, 6.0f, 12.5f);
		const float wx0 = wet.x, dx0 = dry.x;
		for (int i = 0; i < 30; i++) {
			wet.vx = bodyWalkSpeed(bodySubmerged(&s_world, &wet));
			dry.vx = bodyWalkSpeed(bodySubmerged(&s_world, &dry));
			bodyStep(&wet, &s_world, dt);
			bodyStep(&dry, &s_world, dt);
		}
		const float wet_dx = wet.x - wx0;
		const float dry_dx = dry.x - dx0;
		CHECK(dry_dx > 2.1f);              // control: the dry arm really did walk
		CHECK(wet_dx > 0.0f);              // control: the wet arm moved at all
		CHECK(wet_dx < dry_dx * 0.6f);     // and got measurably less far
	}

	worldExit(&s_world);
}

// v1.8.2 — the waterline, which is the one region of the water column no test above ever
// entered. testSwimming starts at y=6 in water spanning 4..12 and asserts a rise that never
// reaches y=13; testBuoyancy never crosses either. The bug lived exactly in the gap.
//
// Reported as: "when you swim upwards, once you get to the top, you stop having up and down
// really fast. I don't want that one to be slower, more Minecraft like." Read literally,
// that is two requirements — it must STOP, and the swim itself must not get slower — and
// the run below measures both. All figures are 600 frames of held jump from the bottom of
// the lake at dt = 1/60, sampled from frame 200 once the climb is over:
//
//                        y travel     vy envelope          eye crossings of the waterline
//   v1.8.1               0.223333     +2.866667/-3.200001  0 of 399  (thrown clear, bobbing
//                                                                    in the AIR above it)
//   v1.8.2 attempt 1     0.022027     +0.487818/-0.379595  140 of 399
//   v1.8.2 attempt 2     0.000018     -0.000000/-0.000210  0 of 399
//
// The middle row is why this test is written the way it is. Attempt 1 moved the body ten
// times less and was still wrong: pulling vy towards 0 does not hold a body still, because
// a velocity target cannot cancel a constant acceleration. Gravity re-established about
// -0.5 blocks/s of droop every frame, the body sank out of BODY_SURFACE, the +3 target
// threw it back, and the equilibrium simply relocated to the state boundary — which is the
// EYE LINE. So the camera crossed the water plane ten times a second where the old code
// never crossed it at all. A travel check alone called that a fix. It was not one.
//
// Hence the checks here are on three quantities, not one: the travel, the velocity
// envelope, and the number of times the eye passes through the surface plane. The last is
// the one that would have caught attempt 1, and it is the one closest to what the player
// actually sees, since there is no underwater tint — the only cue that the head went under
// is the water quad flicking past the near plane.
//
// Attempt 2 makes vy = target a genuine fixed point by not applying gravity at all on a
// frame where the swim input is pulling (Body::swim_drive). Nothing restores the error, so
// there is no limit cycle left to damp, and the measured travel is 18 micro-blocks over the
// last 400 frames rather than 2.2 cm.
static void testSurfaceSwim(void)
{
	// Frame-indexed traces. Static rather than automatic: 600 floats twice is 4.8 KB and
	// this file is also built for a 32 KB console stack.
	static float vy[600];
	static float py[600];

	const float dt = 1.0f / 60.0f;

	// The same lake testSwimming uses: stone at y=3, water y=4..12 over x<8, so the water
	// SURFACE is the plane y=13.0 and the top water cell is y=12.
	worldInit(&s_world);
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++)
			CHECK(worldSet(&s_world, x, 3, z, BLOCK_STONE));
	for (int y = 4; y <= 12; y++)
		for (int x = 0; x < 8; x++)
			for (int z = 0; z < 16; z++)
				CHECK_QUIET(worldSet(&s_world, x, y, z, BLOCK_WATER));

	// Started on the LAKE BED with the swim button held, which is the run the report
	// describes — "when you swim upwards, once you get to the top". Attempt 1's version of
	// this test dropped the body in from above instead, which turned out to exercise a
	// different path entirely (the entry brake, not the arrival), and is kept below as a
	// second arm rather than as the main one. 600 frames is ten seconds; the climb from
	// y=4.5 to the surface takes about 150 of them.
	Body b; bodyInit(&b, 4.5f, 4.5f, 4.5f);
	for (int i = 0; i < 600; i++) {
		bodyJump(&b, bodyWetUpdate(&s_world, &b), true, i == 0, dt);
		bodyStep(&b, &s_world, dt);
		vy[i] = b.vy;
		py[i] = b.y;
	}

	// --- The headline check: does the eye ever pass back through the water plane once the
	// body has arrived? This is the one attempt 1 failed, 140 times, and the one a travel
	// threshold on its own is blind to.
	//
	// The surface plane is y = 13.0 — the top face of the y=12 water cell — so the eye is
	// under water exactly when its cell is liquid, which is the same question physics.c
	// asks. Counted as CROSSINGS rather than as a fraction of frames so that a body which
	// settles just under the line (bad, but quiet) cannot be confused with one that settles
	// just over it.
	{
		int under = 0, crossings = 0;
		bool prev = blockInfo(worldGet(&s_world, 4, (int)(py[200] + PLAYER_EYE), 4))->liquid;
		for (int i = 200; i < 600; i++) {
			const bool now = blockInfo(worldGet(&s_world, 4, (int)(py[i] + PLAYER_EYE), 4))->liquid;
			if (now) under++;
			if (now != prev) crossings++;
			prev = now;
		}
		CHECK(crossings == 0);
		CHECK(under == 0);        // and it settled ABOVE the line, not below it
	}

	// --- How far it travels once settled, and the velocity envelope beside it so that a
	// quiet position cannot be bought with a violent velocity.
	//
	// 0.05 blocks is 5 cm. v1.8.1 measured 0.223333 here and attempt 1 measured 0.022027;
	// attempt 2 measures 0.000018, which is three orders of magnitude inside the bound.
	// The tight second bound is what makes this a FIXED POINT check rather than a
	// small-oscillation check: 0.001 blocks is a millimetre, and neither earlier version
	// comes within a factor of twenty of it.
	{
		float ymax = py[200], ymin = py[200], vmax = vy[200], vmin = vy[200];
		for (int i = 200; i < 600; i++) {
			if (py[i] > ymax) ymax = py[i];
			if (py[i] < ymin) ymin = py[i];
			if (vy[i] > vmax) vmax = vy[i];
			if (vy[i] < vmin) vmin = vy[i];
		}

		CHECK(ymax - ymin < 0.05f);      // the bound attempt 1 also passed
		CHECK(ymax - ymin < 0.001f);     // the bound only a fixed point passes

		// v1.8.1: +2.866667 / -3.200001. Attempt 1: +0.487818 / -0.379595.
		// Attempt 2: -0.000000 / -0.000210 — the body is not moving at all.
		CHECK(vmax < 1.0f);
		CHECK(vmin > -1.0f);
		CHECK(vmax < 0.001f && vmin > -0.001f);

		// Control, green in every arm including the sabotaged ones: the run stayed inside
		// the lake and never fell through to the stone floor or flew off the top. This is
		// the check that says the ones above are measuring a floating body and not a
		// fixture that collapsed.
		CHECK(ymin > 4.0f && ymax < 14.0f);
	}

	// --- It reaches that point rather than creeping towards it. Frame 300 against frame
	// 599 is five seconds apart; v1.8.1 drifted 0.010000 over that stretch and attempt 1
	// drifted 0.000859. Attempt 2 does not move at all, so the bound is a hundredth of
	// attempt 1's rather than six times it.
	{
		const float creep = py[599] > py[300] ? py[599] - py[300] : py[300] - py[599];
		CHECK(creep < 0.005f);
		CHECK(creep < 0.00005f);
	}

	// --- Where it settles, and this is the check PLAYER_WET_HYSTERESIS is load-bearing
	// for. The feet must be in water and the EYES must be clearly out of it — Minecraft
	// floats you with your head above the surface, not level with it.
	//
	// The clearance comes from two places and the band is the reliable half: a body leaves
	// BODY_SUBMERGED only once its eye is PLAYER_WET_HYSTERESIS (0.25) above the plane, and
	// then coasts a further ~0.24 on the velocity it arrived with before the surface target
	// takes it. Measured eye height 13.485423, so 0.485 clear. With the band collapsed to
	// zero the coast is all that is left and the eye settles at 13.235, which is why the
	// bound below is 0.40 and not 0.10: it is placed to go red on exactly that change.
	//
	// v1.8.1 for contrast settled with its feet cell in AIR at y 13.124444 — thrown clear
	// of the lake altogether, bobbing above it. That is the "up and down" of the report.
	{
		const int feet = (int)py[599];
		CHECK(blockInfo(worldGet(&s_world, 4, feet, 4))->liquid == true);

		const float eye = py[599] + PLAYER_EYE;
		CHECK(eye > 13.0f);              // head out of the water at all
		CHECK(eye > 13.40f);             // and CLEARLY out, measured 13.485423
		CHECK(eye < 13.60f);             // without being launched clear of the lake
	}

	// --- The same hold at other frame rates. The body runs on the RENDER frame, so this is
	// not hypothetical: a console dropping to 30 or 20 fps must float at the same height,
	// or the water would feel different depending on how much of the world was on screen.
	// Measured settled y: 11.865423 at 60 fps, 11.893859 at 30, 11.870266 at 20 — a spread
	// of 0.028 blocks across a three-fold change in tick length.
	//
	// Stated plainly because the tempting claim is wrong: this is NOT a witness for
	// physics.c's dampAlpha, which was measured to move these heights by less than half a
	// centimetre. The witness for dampAlpha itself is testSwimming's long-tick sweep.
	{
		const float rates[2] = { 1.0f / 30.0f, 1.0f / 20.0f };
		for (int r = 0; r < 2; r++) {
			Body h; bodyInit(&h, 4.5f, 4.5f, 4.5f);
			bool under = false;
			for (int i = 0; i < 600; i++) {
				bodyJump(&h, bodyWetUpdate(&s_world, &h), true, i == 0, rates[r]);
				bodyStep(&h, &s_world, rates[r]);
				if (i >= 200 && blockInfo(worldGet(&s_world, 4, (int)(h.y + PLAYER_EYE), 4))->liquid)
					under = true;
			}
			CHECK_QUIET(under == false);                 // the headline check, at this rate
			const float gap = h.y > py[599] ? h.y - py[599] : py[599] - h.y;
			CHECK_QUIET(gap < 0.05f);
			CHECK_QUIET(blockInfo(worldGet(&s_world, 4, (int)h.y, 4))->liquid == true);
			CHECK_QUIET(h.y + PLAYER_EYE > 13.40f);
		}
		// Spelled loudly once, so a silent sweep cannot be the only evidence.
		Body h30; bodyInit(&h30, 4.5f, 4.5f, 4.5f);
		for (int i = 0; i < 600; i++) {
			bodyJump(&h30, bodyWetUpdate(&s_world, &h30), true, i == 0, 1.0f / 30.0f);
			bodyStep(&h30, &s_world, 1.0f / 30.0f);
		}
		CHECK(h30.y > 11.84f && h30.y < 11.94f);         // measured 11.893859
	}

	// --- Arriving from ABOVE instead of from below: the same button, held through a fall
	// into the lake. Attempt 1's test used this as its only case, so it is kept, but it is
	// a different path — the swim pull brakes the entry rather than ending a climb, and the
	// body stops much higher (settled y 12.767226, eye 1.387 clear of the plane) because a
	// driven frame has no gravity to carry it under. Recorded because it is a visible
	// behaviour change from v1.8.1, where the hard +3 clamp bounced the body straight back
	// out of the water on its first wet frame.
	{
		Body a; bodyInit(&a, 4.5f, 14.0f, 4.5f);
		int crossings = 0;
		bool prev = false;
		for (int i = 0; i < 600; i++) {
			bodyJump(&a, bodyWetUpdate(&s_world, &a), true, i == 0, dt);
			bodyStep(&a, &s_world, dt);
			if (i >= 200) {
				const bool now = blockInfo(worldGet(&s_world, 4, (int)(a.y + PLAYER_EYE), 4))->liquid;
				if (i > 200 && now != prev) crossings++;
				prev = now;
			}
		}
		CHECK(crossings == 0);
		CHECK(a.vy > -0.001f && a.vy < 0.001f);                          // stopped dead
		CHECK(blockInfo(worldGet(&s_world, 4, (int)a.y, 4))->liquid == true);   // feet wet
		CHECK(a.y > 12.7f && a.y < 12.85f);                              // measured 12.767226
	}

	// --- Control arm, green in every sabotage: the identical held-button loop in the dry
	// column beside the lake must fall and land. If this ever goes red the fixture broke,
	// not the surface rule.
	{
		Body d; bodyInit(&d, 12.5f, 14.0f, 12.5f);
		for (int i = 0; i < 600; i++) {
			bodyJump(&d, bodyWetUpdate(&s_world, &d), true, i == 0, dt);
			bodyStep(&d, &s_world, dt);
		}
		CHECK(d.on_ground == true);
		CHECK(d.y > 3.9f && d.y < 4.1f);   // resting on the stone at y=3
	}

	worldExit(&s_world);
}

#ifndef __3DS__
// The half of task 25 that lives in source/scene/player.c, which includes <3ds.h> through
// app/input_map.h and so cannot be linked into this binary at all.
//
// This is not belt-and-braces. Every behavioural check above passes against a game in
// which playerUpdate never asks bodySubmerged() anything — the swimmer would be a body
// with correct water physics that no button can drive. That is precisely the failure
// app/session_test.c was written for ("`sessionBegin();` deleted from main.c's lap ->
// only the two source-text checks go red"), and it uses this same technique on main.c.
//
// Run from the project root, which is where the relative path resolves and where
// tools/run_host_tests.sh puts the working directory.
static void testPlayerWiresSwimming(void)
{
	FILE* f = fopen("source/scene/player.c", "rb");
	CHECK(f != NULL);
	if (!f) return;

	static char src[65536];
	const size_t n = fread(src, 1, sizeof(src) - 1, f);
	fclose(f);
	src[n] = '\0';
	CHECK(n > 0 && n < sizeof(src) - 1);   // read whole, not truncated

	// It must ask the question... and since v1.8.2 it must ask the WIDE one, through the
	// HYSTERETIC entry point. Asking only the boolean wrapper would compile and would swim,
	// and the surface would judder exactly as it did before, because bodyJump cannot tell
	// BODY_SURFACE from BODY_SUBMERGED unless this file hands it the difference. Asking
	// bodyWetState instead of bodyWetUpdate would also compile and would also mostly work,
	// and would quietly cost the float its head clearance — the band lives on the body and
	// only bodyWetUpdate advances it.
	const char* ask = strstr(src, "bodyWetUpdate(");
	CHECK(ask != NULL);
	CHECK(strstr(src, "bodyWetState(") == NULL);

	// ...and hand the answer to both consumers. A build that computes submersion and
	// then ignores it is the exact defect this file cannot otherwise see.
	CHECK(strstr(src, "bodyWalkSpeed(") != NULL);
	CHECK(strstr(src, "bodyJump(") != NULL);

	// v1.8.2 — and it must hand bodyJump the STATE and the frame time, not a bool and
	// nothing. `bodyJump(&p->body, wet, ...)` with `dt` last is the only shape that gives
	// the water branch a target it can pick and a rate it can apply; passing `submerged`
	// there would compile (BodyWet is an enum, a bool converts) and would silently pin
	// every swimmer to BODY_SURFACE's zero target, so a swimmer could never climb.
	const char* jump = strstr(src, "bodyJump(&p->body, wet,");
	CHECK(jump != NULL);
	if (jump) {
		const char* end = strchr(jump, ';');
		CHECK(end != NULL);
		// The last argument on that one call has to be the tick. Searched inside the call
		// only, so bodyStep's own `, dt)` cannot stand in for it.
		if (end) CHECK(strstr(jump, ", dt)") != NULL && strstr(jump, ", dt)") < end);
	}

	// The old inline jump must be GONE, not merely shadowed by a new branch below it:
	// two writers to vy on the same frame is a race decided by line order.
	CHECK(strstr(src, "PLAYER_JUMP_SPEED") == NULL);

	// And the speed must be applied through bodyWalkSpeed: the literal PLAYER_WALK_SPEED
	// that used to scale ix/iz must no longer be there either, or the walk would silently
	// keep its dry speed while bodyWalkSpeed's answer went unused.
	CHECK(strstr(src, "PLAYER_WALK_SPEED") == NULL);

	// Ordering: the submersion answer has to exist before the step that consumes it.
	const char* step = strstr(src, "bodyStep(");
	CHECK(step != NULL);
	if (ask && step) CHECK(ask < step);
}
#endif

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

// The mirror of the test above: a chunk of solid opaque rock, with six neighbours the same,
// also meshes to nothing. Nothing skips that build any more — step 9.4's buried-chunk early-out
// was removed once a census showed the case never occurs in generated terrain, see
// scene/chunk_render.c — but the mesher property itself is worth pinning down, because the two
// halves of it fail in opposite directions and neither is visible on screen: a mesher that
// emitted the enclosed faces would burn the whole budget on geometry inside solid rock, and one
// that dropped a face too many would carve a hole in a cave wall nobody would find except by
// walking into it.
static void testAllOpaqueMeshesToNothing(void)
{
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * out.vert_cap);
	out.indices   = (uint16_t*)malloc(sizeof(uint16_t) * out.index_cap);
	CHECK(out.verts != NULL && out.indices != NULL);
	if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

	worldInit(&s_world);

	// The full 3x3x3, middle included: solid rock with solid rock all around it.
	bool built = true;
	for (int dy = 0; dy <= 2; dy++)
		for (int dz = 0; dz <= 2; dz++)
			for (int dx = 0; dx <= 2; dx++)
				built = fillChunk(dx, dy, dz, BLOCK_STONE) && built;
	CHECK(built);

	const Chunk* middle = worldChunk(&s_world, 1, 1, 1);
	CHECK(middle != NULL);
	CHECK(!chunkIsAllAir(middle));      // solid rock is not the all-air case above

	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 0);
	CHECK(out.index_count == 0);

	// One block of air inside it and exactly the six faces around that hole appear. This is
	// the direction that matters: a mesher that still emitted nothing here would delete a cave.
	CHECK(worldSet(&s_world, 16 + 8, 16 + 8, 16 + 8, BLOCK_AIR));
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&out, &s_scratch);
	CHECK(out.faces == 6);

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
	worldgenInit(&g, 12345u, GEN_VERSION_LEGACY);

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
	worldgenInit(&g, 0xBEEFu, GEN_VERSION_LEGACY);

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
	worldgenInit(&g2, 0xBEEFu + 1, GEN_VERSION_LEGACY);
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
	worldgenInit(&g, 12345u, GEN_VERSION_LEGACY);

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
		worldgenInit(&gs, seeds[s], GEN_VERSION_LEGACY);
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
	worldgenInit(&g, 1616u, GEN_VERSION_LEGACY);

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
	worldgenInit(&g, 1337u, GEN_VERSION_LEGACY);

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
	worldgenInit(&same, 1337u, GEN_VERSION_LEGACY);
	worldgenInit(&other, 1338u, GEN_VERSION_LEGACY);

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

// Step 9.2a/9.1b's memory payoff, reproduced here as a standing regression check rather than
// a one-off scratch measurement someone has to remember to re-run by hand. Radius 8 ->
// (2*8+1)^2 == 289 columns, the render-distance figure the step's design doc measured
// against: a real generated 289-column world came to 2,501,174 B against a 12,582,912 B
// budget cap (WORLD_BUDGET_BYTES), roughly 47 % of what the OLD fixed-4,096-B-per-chunk
// scheme would have cost for the exact same chunk count (5,323,344 B). That exact figure is
// not asserted here — worldgen's noise parameters (tree density, cave carving, biome mix)
// are free to change without this test needing a hand-updated constant every time — but the
// bound below (75 % of the old scheme) is comfortably short of the ~47 % actually measured,
// so a real regression in the palette/uniform win still fails loudly.
static void testWorldgenMemoryProjection(void)
{
	worldInit(&s_world);

	WorldGen g;
	worldgenInit(&g, 20260819u, GEN_VERSION_LEGACY);

	const int radius = 8;
	const int failed = worldgenArea(&g, &s_world, 0, 0, radius);
	CHECK(failed == 0);
	CHECK(s_world.columns == 289);

	const size_t bytes = worldBytes(&s_world);
	CHECK(bytes == budgetUsed());          // worldBytes must always equal what the budget holds

	const size_t old_scheme_bytes = (size_t)s_world.chunks * (size_t)CHUNK_BLOCKS
	                               + (size_t)s_world.columns * sizeof(Column);
	CHECK(bytes < old_scheme_bytes);
	CHECK(bytes < old_scheme_bytes * 3 / 4);

	worldExit(&s_world);
	CHECK(budgetUsed() == 0);
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

	// Wrap-around. 5 in flight at a time, for four full laps of the ring, so head and tail
	// each pass zero several times while the queue is non-empty — the case a push/pop pair
	// that only moves one index gets away with when the ring is drained to zero between
	// operations.
	//
	// The round count is derived from JOBQ_CAP rather than the literal 500 it used to be.
	// v1.6.0 task 12 raised JOBQ_CAP from 128 to 512 and 500 rounds silently stopped being
	// "four laps" — it stopped being even one, so this check would have quietly stopped
	// testing wrap-around at all while staying green.
	const int wrap_rounds = 4 * JOBQ_CAP + 5;
	int wrap_bad = 0;
	for (int i = 0; i < 5; i++) {
		const Job j = {JOB_MESH, i, i, i & 7};
		CHECK_QUIET(jobqPush(&q, j));
	}
	for (int i = 5; i < wrap_rounds; i++) {
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
// v1.6.0 task 12. A refused mesh push has to be RECOVERABLE.
//
// The bug this exists for is an ordering bug, not a capacity bug. main.c's
// genQueueReadyColumns used to mark a column queued and only then push its chunks:
//
//     genSlotSet(s_col_queued, cx, cz);
//     for (cy...) if (!jobqPush(&s_meshq, j)) s_genr.mesh_refused++;
//
// so a push the full ring turned down lost its chunk for the rest of the session — the
// column was already flagged, and the loop's own "if already queued, continue" skipped it
// forever after. The blocks stay in the World and are simply never meshed: a permanent hole
// that walking around does not repair.
//
// world/meshq.c is the real code that decides this, and it is what this test links — not a
// copy of the loop. Its return value IS the contract ("true only if every chunk got in"),
// so the caller's flag can be modelled here in the two lines main.c now spends on it and the
// whole sequence checked end to end. This project has shipped green suites over hand-copied
// logic twice (tests/battery_test.c, tests/sleep_test.c) and the rule since is that the test
// links the module.
//
// The check that has to be able to go red is the LAST one: every chunk of the column is
// eventually queued. Sabotaging meshqPushColumn to `return true` reproduces the old ordering
// exactly, and that is the measurement recorded in tools/run_host_tests.sh.
static void testMeshqRefusalIsRecoverable(void)
{
	// A column with blocks in five separate chunks, and a sixth chunk left as air so the
	// all-air skip is exercised too rather than assumed. Chunk cy holds a block at
	// y = cy * CHUNK_DIM + 1.
	static const int kFilled[] = { 0, 1, 2, 4, 6 };
	const int NFILLED = (int)(sizeof(kFilled) / sizeof(kFilled[0]));

	budgetReset();
	worldInit(&s_world);
	for (int i = 0; i < NFILLED; i++)
		CHECK_QUIET(worldSet(&s_world, 3, kFilled[i] * CHUNK_DIM + 1, 5, BLOCK_STONE));

	// Sanity on the fixture itself: a test whose world is empty would pass every line below
	// while proving nothing at all.
	int present = 0;
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		const Chunk* c = worldChunk(&s_world, 0, cy, 0);
		if (c && !chunkIsAllAir(c)) present++;
	}
	CHECK(present == NFILLED);

	// ── the control arm: a queue with room takes the whole column in one pass ──────────
	JobQueue q;
	jobqInit(&q);
	int refused = 0;
	bool queued = false;

	if (meshqPushColumn(&q, &s_world, 0, 0, &refused)) queued = true;
	CHECK(queued);
	CHECK(refused == 0);
	CHECK(jobqCount(&q) == NFILLED);
	CHECK(jobqConsistent(&q));

	// ── the real arm: only two free slots for five chunks ──────────────────────────────
	//
	// Filled with JOB_GENERATE rather than JOB_MESH so the mesh jobs this test cares about
	// are distinguishable from the padding when the queue is drained below.
	jobqInit(&q);
	const int FREE = 2;
	for (int i = 0; i < JOBQ_CAP - FREE; i++) {
		const Job pad = {JOB_GENERATE, 1000 + i, 0, 0};
		CHECK_QUIET(jobqPush(&q, pad));
	}
	CHECK(jobqCount(&q) == JOBQ_CAP - FREE);

	refused = 0;
	queued  = false;
	if (meshqPushColumn(&q, &s_world, 0, 0, &refused)) queued = true;

	// Three of the five did not fit. The count is the diagnostic main.c reports as
	// mesh_refused, so it has to be the real shortfall and not just a flag.
	CHECK(refused == NFILLED - FREE);
	CHECK(jobqCount(&q) == JOBQ_CAP);

	// THE fix, in one line: the column must NOT be marked queued when anything was refused.
	// This is the check that goes red against the old ordering.
	CHECK(!queued);

	// ── and the recovery: drain, ask again, and the column completes ───────────────────
	//
	// Draining is what a frame of genDrainMesh does. Which cy values arrived across BOTH
	// passes is what matters — a retry re-pushes chunks that already got through, which is
	// harmless (chunkRenderBuild rebuilds a chunk's existing slot in place) and is why this
	// records a set rather than a count.
	int meshed_mask = 0;
	Job j;
	while (jobqPop(&q, &j))
		if (j.type == JOB_MESH) meshed_mask |= 1 << j.cy;
	CHECK(jobqCount(&q) == 0);

	// main.c's guard, verbatim: a column already marked queued is never revisited. If the
	// mark went on despite the refusal, this pass does nothing and the column stays short.
	refused = 0;
	if (!queued && meshqPushColumn(&q, &s_world, 0, 0, &refused)) queued = true;
	CHECK(queued);
	CHECK(refused == 0);

	while (jobqPop(&q, &j))
		if (j.type == JOB_MESH) meshed_mask |= 1 << j.cy;

	// The payoff. Every chunk that had blocks in it got queued for meshing, and no chunk that
	// was all air did. A hole in the world is precisely this mask coming back short.
	int expect_mask = 0;
	for (int i = 0; i < NFILLED; i++) expect_mask |= 1 << kFilled[i];
	CHECK(meshed_mask == expect_mask);

	// ── the capacity half, stated against the ring it now has to serve ────────────────
	//
	// The worst radius-3 ring measured on the host is 242 simultaneous chunk meshes and the
	// structural bound is 49 columns x COLUMN_CHUNKS = 392. JOBQ_CAP was 128, which is under
	// both — and under the 150-slot radius-2 ring it was already serving. See world/jobq.h.
	CHECK(JOBQ_CAP >= 49 * COLUMN_CHUNKS);
	CHECK(JOBQ_CAP >= RENDER_DIST_MAX_SLOTS);

	worldExit(&s_world);
	budgetReset();
}

// ---------------------------------------------------------------------------------------
// Step 7.3, cave culling. Two things are being tested and only one of them is "does it cull":
// the far more important property is that it never culls something it should have kept, and
// most of what follows is aimed at that.

// 35 KB and 18 KB. Static, because the 3DS main thread's stack is 32 KB and a test that only
// runs on the host is still the wrong place to learn that.
static VisWalk    s_walk;
static VisScratch s_vis_scratch;

// Chunk became opaque in step 9.2a, so this can no longer be a plain static struct the way
// s_walk and s_vis_scratch above are — it needs a real chunkAlloc, done once on first use and
// then reused (never freed) for the rest of the test process. That is safe to leave unfreed:
// chunk.c has zero dependency on world/budget.h (see chunk.h's file header for why), so this
// allocation never touches the budget singleton that testBudget and friends assert is exactly
// 0 at the points they check it.
static Chunk* s_vis_chunk;

static Chunk* visChunk(void)
{
	if (!s_vis_chunk) s_vis_chunk = chunkAlloc(BLOCK_AIR);
	return s_vis_chunk;
}

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
	Chunk* c = visChunk();

	// Solid rock joins nothing.
	chunkClear(c, BLOCK_STONE);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);

	// Open sky joins everything — all fifteen pairs, and no bit above them.
	chunkClear(c, BLOCK_AIR);
	const uint16_t open = visChunkConnectivity(c, &s_vis_scratch);
	CHECK(open == 0x7FFF);

	// A straight east-west tunnel through solid rock joins east to west and nothing else.
	// This is the case the whole algorithm exists for: every other face of this chunk is
	// stone, so a camera to the north of it can see nothing beyond it.
	chunkClear(c, BLOCK_STONE);
	for (int x = 0; x < CHUNK_DIM; x++)
		chunkSet(c, chunkIndex(x, 8, 8), BLOCK_AIR);

	const uint16_t tunnel = visChunkConnectivity(c, &s_vis_scratch);
	CHECK(visConnected(tunnel, FACE_EAST, FACE_WEST));
	CHECK(!visConnected(tunnel, FACE_TOP, FACE_BOTTOM));
	CHECK(!visConnected(tunnel, FACE_EAST, FACE_TOP));
	CHECK(!visConnected(tunnel, FACE_NORTH, FACE_SOUTH));
	CHECK(tunnel == (uint16_t)(1u << visPairIndex(FACE_EAST, FACE_WEST)));

	// Add a vertical shaft that does not meet the tunnel. Two separate components, so the
	// mask gains top-bottom and must NOT gain east-top: a fill that leaked between the two
	// would report a sight line that does not exist and stop the cull working at all.
	for (int y = 0; y < CHUNK_DIM; y++)
		chunkSet(c, chunkIndex(2, y, 2), BLOCK_AIR);

	const uint16_t both = visChunkConnectivity(c, &s_vis_scratch);
	CHECK(visConnected(both, FACE_EAST, FACE_WEST));
	CHECK(visConnected(both, FACE_TOP, FACE_BOTTOM));
	CHECK(!visConnected(both, FACE_EAST, FACE_TOP));
	CHECK(!visConnected(both, FACE_WEST, FACE_BOTTOM));

	// Now join them, and the pairs that were absent must appear. Without this the test above
	// would pass just as well against a fill that never connects anything.
	for (int z = 2; z <= 8; z++)
		chunkSet(c, chunkIndex(2, 8, z), BLOCK_AIR);

	const uint16_t joined = visChunkConnectivity(c, &s_vis_scratch);
	CHECK(visConnected(joined, FACE_EAST, FACE_TOP));
	CHECK(visConnected(joined, FACE_WEST, FACE_BOTTOM));

	// Step 7.5 made leaves transparent, and this check flipped with it: it used to assert 0,
	// on the grounds that an opaque canopy blocks sight exactly like stone, and now asserts
	// fully connected because sight passes through one. That the same line could flip by
	// changing one field in the block registry is the point of writing the fill against
	// solid && !transparent rather than against solid — leaves are still solid, and still
	// stop the player walking through them.
	chunkClear(c, BLOCK_LEAVES);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0x7FFF);
	CHECK(blockIsSolid(BLOCK_LEAVES));

	// Step 9.1b's short-circuit: a UNIFORM chunk must answer without ever decompressing into
	// sc->blocks. Solid-and-not-open joins nothing (0), and open joins everything — the same
	// two answers as the general path above, just reached through the fast branch instead of
	// the flood fill. chunkClear always leaves a chunk UNIFORM, so both of the very first two
	// assertions in this function already exercised it; these two are here to say so
	// explicitly and to pin the UNIFORM answer down as its own named case.
	chunkClear(c, BLOCK_STONE);
	CHECK(chunkGetForm(c) == CHUNK_FORM_UNIFORM);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);
	chunkClear(c, BLOCK_AIR);
	CHECK(chunkGetForm(c) == CHUNK_FORM_UNIFORM);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0x7FFF);

	// v1.6.0 task 10. visgraph.c's openTable() filled only ids 0..BLOCK_COUNT-1 from the
	// registry and left everything above them at the air prefill, which was correct exactly
	// while eight was every block there is. A server-registered dynamic block lands at
	// REG_ID_DYN_LO (0x80) and up, so every one of them read back "see-through" no matter
	// what its def said — a chunk of solid dynamic stone reported fully connected, the cave
	// cull believed sight passed straight through it, and everything behind that wall was
	// culled away while the wall itself still drew.
	//
	// A dynamic row registered here rather than a fabricated table: registryRegister() is the
	// same call the DEFS decoder ends up in, so this exercises the real path an id above 0x7F
	// gets into the world by. The two halves are the same pair as BLOCK_STONE/BLOCK_AIR at
	// the top of this function, which is what makes the assertion mean something — the
	// transparent variant must still read open, or a fix that simply hard-coded "high ids
	// are solid" would pass.
	BlockDef dyn;
	memset(&dyn, 0, sizeof dyn);
	snprintf(dyn.name, sizeof dyn.name, "vis_solid");
	for (int f = 0; f < BLOCK_FACES; f++) dyn.tex[f] = BTEX_STONE;
	dyn.flags = REG_FLAG_SOLID;
	const BlockId dyn_solid = registryRegister(&dyn);
	CHECK(dyn_solid == REG_ID_DYN_LO);

	memset(&dyn, 0, sizeof dyn);
	snprintf(dyn.name, sizeof dyn.name, "vis_glass");
	for (int f = 0; f < BLOCK_FACES; f++) dyn.tex[f] = BTEX_LEAVES;
	dyn.flags = REG_FLAG_SOLID | REG_FLAG_TRANSPARENT;
	const BlockId dyn_clear = registryRegister(&dyn);
	CHECK(dyn_clear == REG_ID_DYN_LO + 1);

	CHECK(blockIsSolid(dyn_solid) && !blockInfo(dyn_solid)->transparent);
	chunkClear(c, dyn_solid);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);

	chunkClear(c, dyn_clear);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0x7FFF);

	// Non-uniform too, so the answer comes out of the flood fill rather than the UNIFORM
	// short-circuit: an east-west tunnel bored through the dynamic solid must join exactly
	// the one pair, the same as the stone case above.
	chunkClear(c, dyn_solid);
	for (int x = 0; x < CHUNK_DIM; x++)
		chunkSet(c, chunkIndex(x, 8, 8), BLOCK_AIR);
	CHECK(chunkGetForm(c) != CHUNK_FORM_UNIFORM);
	const uint16_t dyn_tunnel = visChunkConnectivity(c, &s_vis_scratch);
	CHECK(dyn_tunnel == (1u << visPairIndex(FACE_EAST, FACE_WEST)));

	// Put the process-wide table back to the core rows: every test after this one expects a
	// registry nobody has added to.
	registryInitCore();
	CHECK(!registryIsDefined(REG_ID_DYN_LO));
}

// v1.7.1 task 49. visgraph.c stopped rebuilding its 256-entry see-through table on every call
// and started keeping it, which creates a state the old code could never be in: a live table
// that was derived from a registry which has since changed. The failure that produces is
// silent and is the bad direction — a solid block whose cached row still says "see-through"
// makes the cave cull believe sight passes through a wall, and everything behind it is thrown
// away while the wall still draws. That is a hole in the world, not a cosmetic wobble.
//
// The gap this closes, precisely. testVisConnectivity above already registers dynamic rows
// half way through and re-asks, so it catches a cache that never rebuilds at all. What it
// cannot catch is the case where the registry changes without registryCount() changing,
// because every registration it makes strictly increases the count — a cache keyed on the
// count alone sails through every check in it. This is that case, and it is not hypothetical:
// app/session.c calls registryInitCore() when a session is left and net/networld.c applies
// the next server's DEFS when the next one is joined, both in the same process. Two servers
// that define the same NUMBER of blocks put a different block on the same id at the same
// count. Verified red: with visgraph.c's stamp reduced to the count alone, this function is
// the only thing in the suite that fails.
static void testVisTableFollowsRegistry(void)
{
	Chunk* c = visChunk();

	registryInitCore();

	// Warm the table on the core rows, so what follows is a change *under a live cache*
	// rather than a first build.
	chunkClear(c, BLOCK_STONE);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);

	BlockDef def;
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "vis_aba_solid");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_STONE;
	def.flags = REG_FLAG_SOLID;
	const BlockId first = registryRegister(&def);
	CHECK(first == REG_ID_DYN_LO);

	chunkClear(c, first);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);
	const uint8_t count_after_first = registryCount();

	// Leave the session and join another one that defines the same number of blocks. Same
	// id, same count — a see-through block where an opaque one used to be.
	registryInitCore();
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "vis_aba_glass");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_LEAVES;
	def.flags = REG_FLAG_SOLID | REG_FLAG_TRANSPARENT;
	const BlockId second = registryRegister(&def);
	CHECK(second == first);
	CHECK(registryCount() == count_after_first);

	chunkClear(c, second);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0x7FFF);

	// And back the other way on the same id and the same count, so this cannot be passed by
	// a table that has simply decided everything is open.
	registryInitCore();
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "vis_aba_rock");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_STONE;
	def.flags = REG_FLAG_SOLID;
	const BlockId third = registryRegister(&def);
	CHECK(third == first);
	CHECK(registryCount() == count_after_first);

	chunkClear(c, third);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);

	// The same question through the flood fill rather than the UNIFORM short-circuit. The
	// fill indexes the cached table 28,672 times for a chunk this size and would read a
	// stale row just as happily as the one-line shortcut does.
	for (int x = 0; x < CHUNK_DIM; x++)
		chunkSet(c, chunkIndex(x, 8, 8), BLOCK_AIR);
	CHECK(chunkGetForm(c) != CHUNK_FORM_UNIFORM);
	const uint16_t ew = (uint16_t)(1u << visPairIndex(FACE_EAST, FACE_WEST));
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == ew);

	// visgraphInvalidateTables() is the counterpart of mesherInvalidateTables() and is
	// exported for callers that have just changed the registry. Dropping the table must not
	// change any answer — a rebuild from the same registry has to produce the same bits.
	const uint16_t before_drop = visChunkConnectivity(c, &s_vis_scratch);
	visgraphInvalidateTables();
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == before_drop);

	registryInitCore();
	CHECK(!registryIsDefined(REG_ID_DYN_LO));

	// The core rows must still read the way they always did after all of that.
	chunkClear(c, BLOCK_AIR);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0x7FFF);
	chunkClear(c, BLOCK_STONE);
	CHECK(visChunkConnectivity(c, &s_vis_scratch) == 0);
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
	Chunk* c = visChunk();
	chunkClear(c, BLOCK_STONE);
	const uint16_t rock = visChunkConnectivity(c, &s_vis_scratch);
	chunkClear(c, BLOCK_AIR);
	const uint16_t sky = visChunkConnectivity(c, &s_vis_scratch);

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
	CHECK(renderDistDefault(true) == RENDER_DIST_DEFAULT_NEW);

	// v1.6.0 task 12. renderDistDefault used to return RENDER_DIST_MAX verbatim for a New 3DS,
	// so raising the ceiling raised the New 3DS default with it — a behaviour change nobody
	// asked for, hidden inside a constant change. These three lines are the deliberate choice
	// made instead, written down where it can go red: the ceiling moved to 3, the New 3DS
	// default stayed at 2, and the Old 3DS default stayed at 1.
	//
	// The middle one is the load-bearing check. If someone later re-spells renderDistDefault as
	// `new_3ds ? RENDER_DIST_MAX : RENDER_DIST_MIN` it goes red, which is exactly the mistake
	// being guarded against — see scene/render_dist.h's RENDER_DIST_DEFAULT_NEW for why the
	// wider ring is offered on both models and automatic on neither (GPU cost is the binding
	// constraint and cannot be measured anywhere in this project).
	CHECK(renderDistDefault(true) != RENDER_DIST_MAX);
	CHECK(renderDistDefault(true) >= RENDER_DIST_MIN && renderDistDefault(true) <= RENDER_DIST_MAX);
	CHECK(renderDistDefault(false) == 1);
	CHECK(RENDER_DIST_MAX == 3);

	const RenderDist lo = renderDistFor(RENDER_DIST_MIN);
	const RenderDist hi = renderDistFor(RENDER_DIST_MAX);

	// The generated ring is always one column wider than the meshed one. main.c depends on
	// this to keep the mesher from reading an unloaded neighbour as air.
	CHECK(lo.area_radius == lo.radius + 1);
	CHECK(hi.area_radius == hi.radius + 1);

	// Slots: 9 columns and 49 columns at COLUMN_CHUNKS chunks each. The pool is claimed for the
	// maximum at boot, so this number is what decides whether the setting is affordable at all.
	//
	// v1.7.0 took the per-column figure from a measured 6 to the arithmetic ceiling
	// COLUMN_CHUNKS, so the absolute pin below moves 294 -> 392. It stays an absolute number
	// next to the relative one on purpose: the relative check cannot notice
	// RENDER_DIST_SLOTS_PER_COLUMN itself moving, and it moving is what broke the pool.
	CHECK(lo.slots == 9 * RENDER_DIST_SLOTS_PER_COLUMN);
	CHECK(hi.slots == 49 * RENDER_DIST_SLOTS_PER_COLUMN);
	CHECK(hi.slots == 392);
	CHECK(hi.slots > lo.slots);

	// v1.7.0. The pool must cover a ring in which EVERY chunk of every column is meshed, not
	// just the worst ring whatever worldgen happened to be current was measured at. The density
	// generator put the worst measured radius-3 ring at 298 against a pool of 294 — four chunks
	// that would never have been meshed at all, i.e. holes in the terrain — and these two are
	// the host-side statement that it can no longer happen by arithmetic rather than by luck.
	CHECK(hi.slots >= RENDER_DIST_MAX_COLUMNS * COLUMN_CHUNKS);
	CHECK(lo.slots >= 9 * COLUMN_CHUNKS);

	// The slot count the pool is actually cut against, kept in step with the setting's own
	// arithmetic. scene/chunk_render.c splits exactly this many slots across its three tiers.
	CHECK(RENDER_DIST_MAX_COLUMNS == 49);
	CHECK(RENDER_DIST_MAX_SLOTS == hi.slots);

	// Radius 1 keeps the near plane it has always had, so raising the setting is the only
	// thing that can change the view near the camera.
	CHECK(lo.near_plane > 0.0999f && lo.near_plane < 0.1001f);

	// The near plane's corner must stay closer than the player's half-width, or a player
	// pressed to a wall sees through it. 1.5917 is sqrt(1 + tan^2(fovy/2) + tan^2(fovx/2)) at
	// 65 degrees and 400/240. This is the check that stops the near plane being raised further
	// to buy more fog range.
	CHECK(hi.near_plane <= RENDER_DIST_NEAR_MAX);
	CHECK(RENDER_DIST_NEAR_MAX * 1.5917f < 0.3f);

	// v1.6.0 task 12: the near plane SATURATES at the top of the range. NEAR_BASE 0.10 plus
	// NEAR_STEP 0.05 per column reaches 0.20 at radius 3 and the cap holds it at 0.18 — which
	// is the entire reason the ceiling is 3 and not 4. Past here a wider ring cannot move the
	// first fog knot, so it cannot buy visible distance; it only buys geometry the fog hides.
	// If this ever stops being true the argument in render_dist.h no longer holds and the
	// ceiling should be revisited, which is what makes it worth a check rather than a comment.
	//
	// Stated behaviourally — radius 2 is still below the cap, radius 3 is AT it — rather than
	// by re-deriving NEAR_BASE + NEAR_STEP * n here. Those two constants are private to
	// render_dist.c, and a copy of them in this file would be checking the copy.
	CHECK(renderDistFor(2).near_plane < RENDER_DIST_NEAR_MAX);
	CHECK(hi.near_plane >= RENDER_DIST_NEAR_MAX - 1e-6f);
	CHECK(hi.near_plane <= RENDER_DIST_NEAR_MAX + 1e-6f);

	// The load boundary is radius x 16 blocks in the worst case, and the fog must reach the
	// target by then at every setting. This is the criterion step 6.4 established and it has
	// to survive the setting becoming variable.
	CHECK(lo.boundary > 15.9f && lo.boundary < 16.1f);
	CHECK(hi.boundary > 47.9f && hi.boundary < 48.1f);   // radius 3 x 16 blocks
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
	// Past hi.boundary (48 blocks at radius 3) rather than the old 40, which stopped short of
	// the new load boundary — the one distance the fog absolutely has to be monotone at.
	for (float z = 1.0f; z < 56.0f; z += 0.5f) {
		CHECK_QUIET(renderDistVisibility(&lo, z) >= renderDistVisibility(&lo, z + 0.5f) - 1e-6f);
		CHECK_QUIET(renderDistVisibility(&hi, z) >= renderDistVisibility(&hi, z + 0.5f) - 1e-6f);
	}

	// And the payoff, such as it is: a wider ring does let the player see further, because
	// `near` moved with it. Sublinear by a long way — which is the LUT's shape, not a bug, and
	// is why RENDER_DIST_MAX is 3 rather than 5.
	CHECK(hi.half_vis > lo.half_vis * 1.2f);
	CHECK(hi.half_vis < hi.boundary);
	CHECK(lo.half_vis < lo.boundary);

	// v1.6.0 task 12's actual argument for stopping at 3, as a check rather than a paragraph:
	// each extra column buys less than the one before it, and the step from 2 to 3 is the last
	// one worth paying for. Radius 3 is a large gain over radius 2; the near plane saturates
	// there (checked above), so radius 4 and 5 cannot repeat it — which is exactly what makes
	// the ceiling defensible. Both ends measured on the host, not asserted from the comment.
	CHECK(renderDistFor(3).half_vis > renderDistFor(2).half_vis * 1.15f);
	CHECK(renderDistFor(2).half_vis > renderDistFor(1).half_vis);

	// Zero density is "no fog" through this same code path, which is what makes the
	// -DFOG_DENSITY=0.0f check able to go red rather than merely different.
	RenderDist off = lo;
	off.fog_density = 0.0f;
	CHECK(renderDistVisibility(&off, 100.0f) > 0.999f);
}

// ── Step 8.1, the save format ─────────────────────────────────────────────────────────
//
// The codec half runs everywhere. The region-file half is host-only, and deliberately: it
// writes real files, and the one thing this project has already been bitten by is a test
// build sharing a directory with a real save. On the console these tests would run at boot,
// every boot, against the same card the player's world is on.

// Fills a flat CHUNK_BLOCKS buffer with a shape, so each codec case reads as one line. A flat
// buffer rather than a Chunk* directly: since step 9.2a a chunk's in-memory storage form is
// chosen FROM its content by chunkLoadAll, not dictated by whoever is filling it cell by
// cell, so building the shape in the open and handing the whole thing to chunkLoadAll keeps
// that choice entirely inside chunk.c, same as worldgen.c and chunk_codec.c's decoder do.
static void codecFill(BlockId* blocks, int mode)
{
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				const int i = chunkIndex(x, y, z);
				switch (mode) {
				case 0: blocks[i] = BLOCK_AIR; break;                       // uniform
				case 1: blocks[i] = y < 4 ? BLOCK_STONE                     // layered
				                     : y < 7 ? BLOCK_DIRT
				                     : y < 8 ? BLOCK_GRASS : BLOCK_AIR; break;
				case 2: blocks[i] = ((x + y + z) & 1)                       // checkerboard
				                      ? BLOCK_STONE : BLOCK_AIR; break;
				default:
					// mode 3: 32 distinct byte values ((x+y+z) % 32), specifically to push
					// the IN-MEMORY form past PALETTE4's 16-slot ceiling. Mode 2's checkerboard
					// already forces CODEC_RAW on *disk* (RLE would double in size), but it is
					// only 2 distinct ids, so chunkLoadAll would still choose PALETTE4 in
					// memory — mode 2 alone never proves the decoder can rebuild a RAW
					// in-memory chunk, only a RAW-encoded one. This mode does both at once.
					//
					// NOT (x^y^z)&31, which this was first written as: x, y, z are each only
					// 4 bits (0..15), and XOR of three 4-bit values can never set bit 4 or
					// above, so that expression silently produced just 16 distinct ids (0..15)
					// -- exactly PALETTE4's ceiling, not past it. Caught by the official host
					// suite: "FAIL L3268 chunkGetForm(a) == CHUNK_FORM_RAW", because
					// chunkLoadAll correctly chose PALETTE4 for content that this comment
					// claimed needed RAW. (x+y+z) has no such ceiling — x+y+z ranges 0..45 and
					// every integer in 0..31 is reachable before it ever has to wrap, so % 32
					// genuinely visits all 32 remainders across the chunk.
					blocks[i] = (BlockId)((x + y + z) % 32);
					break;
				}
			}
}

// chunkClear() and chunkLoadAll() are chunk.c's plain API and, by design (chunk.h's file
// header), chunk.c has zero dependency on world/budget.h — neither call ever touches the
// budget singleton. chunkDecode() is different: chunk_codec.c claims/releases the byte delta
// around every call (see chunk_codec.h), because region.c calls it directly with no World to
// route through. This test exercises chunkDecode on a bare chunkAlloc'd chunk, outside
// world.c's own tracking, so — exactly like worldExit/worldColumnRemove releasing
// chunkGetBytes(c) before every chunkFree — it is this test's job to keep the singleton
// honest across a direct chunkClear/chunkLoadAll call that discards whatever chunkDecode most
// recently claimed. Skipping this is not cosmetic: it was caught for real by the official
// host suite as a genuine stuck leak ("FAIL L3688 budgetUsed() == 0") the first version of
// this test wrote, from calling chunkClear() to poison a chunk that still held PALETTE4/RAW
// budget a previous chunkDecode had claimed for it.
static void resyncBudget(Chunk* c, size_t bytes_before_direct_call)
{
	budgetRelease(bytes_before_direct_call);
	CHECK(budgetClaim(chunkGetBytes(c)));
}

static void testChunkCodec(void)
{
	Chunk* a = chunkAlloc(BLOCK_AIR);
	Chunk* b = chunkAlloc(BLOCK_AIR);
	CHECK(a && b);

	// 'a' is only ever written through chunkLoadAll/chunkEncode (both budget-free); 'b' is the
	// decode target, so it is the one whose cost this test must own in the budget singleton
	// from the moment it exists — matching what worldChunkCreate() claims for a real chunk
	// before anything is ever decoded into it.
	CHECK(budgetClaim(chunkGetBytes(b)));

	static uint8_t  buf[CHUNK_CODEC_MAX];
	static BlockId  flat_a[CHUNK_BLOCKS];
	static BlockId  flat_b[CHUNK_BLOCKS];

	// Uniform: two bytes, whatever the block is. Half a 128-tall column is sky, so this is
	// the case that decides what a world costs on the card.
	codecFill(flat_a, 0);
	CHECK(chunkLoadAll(a, flat_a));
	size_t n = chunkEncode(a, buf, sizeof(buf));
	CHECK(n == 2);
	CHECK(buf[0] == CODEC_UNIFORM);
	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);                // poison: decode must fully overwrite it
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, buf, n));
	chunkDecompressAll(a, flat_a);
	chunkDecompressAll(b, flat_b);
	CHECK(memcmp(flat_a, flat_b, CHUNK_BLOCKS) == 0);
	CHECK(chunkGetForm(b) == CHUNK_FORM_UNIFORM);   // one id, everywhere -> the cheapest form

	// Layered terrain: the shape RLE exists for. Must beat raw by a long way, not merely
	// beat it — if this ever creeps up towards 4,098 the encoder has stopped working and
	// the only symptom would be a bigger save file.
	codecFill(flat_a, 1);
	CHECK(chunkLoadAll(a, flat_a));
	n = chunkEncode(a, buf, sizeof(buf));
	CHECK(buf[0] == CODEC_RLE);
	CHECK(n < 64);
	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, buf, n));
	chunkDecompressAll(a, flat_a);
	chunkDecompressAll(b, flat_b);
	CHECK(memcmp(flat_a, flat_b, CHUNK_BLOCKS) == 0);

	// Checkerboard: RLE's worst case, twice the size of the blocks it describes. The encoder
	// must notice and fall back, because a player can build this.
	codecFill(flat_a, 2);
	CHECK(chunkLoadAll(a, flat_a));
	n = chunkEncode(a, buf, sizeof(buf));
	CHECK(buf[0] == CODEC_RAW);
	CHECK(n == CHUNK_BLOCKS + 2);
	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, buf, n));
	chunkDecompressAll(a, flat_a);
	chunkDecompressAll(b, flat_b);
	CHECK(memcmp(flat_a, flat_b, CHUNK_BLOCKS) == 0);

	// 32 distinct ids: forces CODEC_RAW on disk (same as the checkerboard) AND
	// CHUNK_FORM_RAW in memory (unlike the checkerboard, which is only 2 ids and stays
	// PALETTE4) — the round trip this step's new in-memory form needs its own coverage for.
	codecFill(flat_a, 3);
	CHECK(chunkLoadAll(a, flat_a));
	CHECK(chunkGetForm(a) == CHUNK_FORM_RAW);
	n = chunkEncode(a, buf, sizeof(buf));
	CHECK(buf[0] == CODEC_RAW);
	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, buf, n));
	CHECK(chunkGetForm(b) == CHUNK_FORM_RAW);
	chunkDecompressAll(a, flat_a);
	chunkDecompressAll(b, flat_b);
	CHECK(memcmp(flat_a, flat_b, CHUNK_BLOCKS) == 0);

	// Malformed input. Every one of these is a shape a power cut can produce, and the
	// contract is that the caller's chunk is left alone rather than half-written — so each
	// case checks the rejection AND that b still holds what it held.
	codecFill(flat_a, 1);
	CHECK(chunkLoadAll(a, flat_a));
	n = chunkEncode(a, buf, sizeof(buf));
	codecFill(flat_b, 0);
	{
		const size_t b_before = chunkGetBytes(b);
		CHECK(chunkLoadAll(b, flat_b));
		resyncBudget(b, b_before);
	}

	CHECK(!chunkDecode(b, buf, 0));
	CHECK(!chunkDecode(b, buf, 1));
	CHECK(!chunkDecode(b, buf, n - 2));           // run table short
	CHECK(!chunkDecode(b, buf, n - 1));           // cut between a run's length and index

	uint8_t bad[CHUNK_CODEC_MAX];
	memcpy(bad, buf, n);
	bad[0] = 99;                                    // unknown tag
	CHECK(!chunkDecode(b, bad, n));

	memcpy(bad, buf, n);
	bad[n - 1] = 200;                               // palette index past the palette
	CHECK(!chunkDecode(b, bad, n));

	// One run too many, so the run table adds up past the end of the chunk. Appending is
	// the way to build this case rather than lengthening the last run: every run in a
	// layered chunk is already the 256 maximum, so there is no run left to grow.
	memcpy(bad, buf, n);
	bad[n]     = 255;                               // a further 256 blocks
	bad[n + 1] = 0;
	CHECK(!chunkDecode(b, bad, n + 2));

	// b must still be the uniform-air chunk it was filled with — both its logical content
	// AND its form: a decode that failed after partially promoting b's storage before
	// rejecting the data would leave this passing on content while lying about cost.
	CHECK(chunkGetForm(b) == CHUNK_FORM_UNIFORM);
	CHECK(chunkIsUniform(b, BLOCK_AIR));
	chunkDecompressAll(b, flat_b);
	for (int i = 0; i < CHUNK_BLOCKS; i++) CHECK_QUIET(flat_b[i] == BLOCK_AIR);

	// A buffer too small to hold even the raw form is a 0, not an overrun.
	codecFill(flat_a, 2);
	CHECK(chunkLoadAll(a, flat_a));
	CHECK(chunkEncode(a, buf, 8) == 0);

	// Give back b's baseline claim from the top of this function — the same
	// budgetRelease(chunkGetBytes(c)) discipline worldExit/worldColumnRemove use right before
	// every chunkFree, needed here because chunk.c's chunkFree itself never touches the
	// budget singleton (see chunk.h). 'a' never touched budget.h (chunkLoadAll/chunkEncode are
	// both budget-free), so it needs no matching release.
	budgetRelease(chunkGetBytes(b));
	chunkFree(a);
	chunkFree(b);
}

// ── Step 9.2d: chunk_codec.c's palette-unify fast paths ──────────────────────────────
//
// testChunkCodec above already runs chunkEncode/chunkDecode over a PALETTE4-form chunk (its
// "layered terrain" case) and a UNIFORM-form chunk (its "uniform" case), so both fast paths
// already have correctness coverage from before this step existed. What this test adds that
// that one does not:
//
//   1. Pins CHUNK_FORM_PALETTE4 explicitly before encoding, on a chunk built the way a
//      player's edits actually build one (chunkSet, not chunkLoadAll from a flat buffer) --
//      and picks the FIRST edit at cell 0 specifically, because chunk.c's chunkSet always
//      puts the OLD uniform value in palette slot 0 and the new write in slot 1, regardless
//      of where the write landed. A first-seen-order scan over the cells (the pre-9.2d
//      algorithm) would instead see the NEW value first at cell 0 and put THAT in slot 0 --
//      the opposite order. If the fast path silently assumed disk order had to match
//      first-seen-cell order, this is the input that would catch it.
//
//   2. Proves backward compatibility: chunkDecode's new direct-to-PALETTE4 commit path must
//      accept a CODEC_RLE buffer whose disk palette is in ANY order, not only the order this
//      build's own chunkEncode happens to produce -- because a save file already on the
//      user's SD card was written by whatever ordering rule was in force when it was
//      written, and this step changed that rule (see chunk_codec.c's chunkEncode comment).
//      The buffer below is built by hand, independently of chunkEncode, in a palette order
//      that provably does not match either algorithm's natural output for this content.
static void testChunkCodecPaletteUnify(void)
{
	Chunk* a = chunkAlloc(BLOCK_AIR);
	Chunk* b = chunkAlloc(BLOCK_AIR);
	CHECK(a && b);
	CHECK(budgetClaim(chunkGetBytes(b)));

	CHECK(chunkSet(a, 0, BLOCK_STONE));      // uniform_id (AIR) -> slot 0, STONE -> slot 1
	CHECK(chunkSet(a, 4000, BLOCK_DIRT));    // 3rd distinct id -> slot 2
	CHECK(chunkGetForm(a) == CHUNK_FORM_PALETTE4);

	static uint8_t buf[CHUNK_CODEC_MAX];
	size_t n = chunkEncode(a, buf, sizeof(buf));
	CHECK(n > 0);
	CHECK(buf[0] == CODEC_RLE);

	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);          // poison: decode must fully overwrite it
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, buf, n));
	CHECK(chunkGetForm(b) == CHUNK_FORM_PALETTE4);
	for (int i = 0; i < CHUNK_BLOCKS; i++) CHECK_QUIET(chunkGet(a, i) == chunkGet(b, i));

	// ── Backward-compat: a hand-built CODEC_RLE buffer, same logical content as `a` above
	// (STONE at cell 0, DIRT at cell 4000, AIR everywhere else) but with its disk palette in
	// [DIRT, AIR, STONE] order -- not chunk.c's in-memory order ([AIR, STONE, DIRT], since
	// AIR is the original uniform_id and gets slot 0), and not first-seen-cell order either
	// ([STONE, AIR, DIRT], since cell 0 is STONE). All three orders are pairwise different
	// for this content, so accepting this one is not accidentally accepting the same order
	// twice under a different name.
	static uint8_t old_style[CHUNK_CODEC_MAX];
	size_t w = 0;
	old_style[w++] = CODEC_RLE;
	old_style[w++] = 2;                        // 3 palette entries, stored minus one
	old_style[w++] = BLOCK_DIRT;                // slot 0
	old_style[w++] = BLOCK_AIR;                 // slot 1
	old_style[w++] = BLOCK_STONE;               // slot 2

	// Each block below appends `length` cells of `slot`, split into RUN_MAX(=256)-sized runs
	// the same way chunk_codec.c's encoder must, so a length above 256 is a legal input here.
	{
		int length = 1; uint8_t slot = 2;      // cell 0: STONE
		while (length > 0) {
			const int run = length > 256 ? 256 : length;
			old_style[w++] = (uint8_t)(run - 1);
			old_style[w++] = slot;
			length -= run;
		}
	}
	{
		int length = 3999; uint8_t slot = 1;   // cells 1..3999: AIR
		while (length > 0) {
			const int run = length > 256 ? 256 : length;
			old_style[w++] = (uint8_t)(run - 1);
			old_style[w++] = slot;
			length -= run;
		}
	}
	{
		int length = 1; uint8_t slot = 0;      // cell 4000: DIRT
		while (length > 0) {
			const int run = length > 256 ? 256 : length;
			old_style[w++] = (uint8_t)(run - 1);
			old_style[w++] = slot;
			length -= run;
		}
	}
	{
		int length = 95; uint8_t slot = 1;     // cells 4001..4095: AIR
		while (length > 0) {
			const int run = length > 256 ? 256 : length;
			old_style[w++] = (uint8_t)(run - 1);
			old_style[w++] = slot;
			length -= run;
		}
	}
	CHECK(w <= CHUNK_CODEC_MAX);

	{
		const size_t b_before = chunkGetBytes(b);
		chunkClear(b, (BlockId)0xEE);
		resyncBudget(b, b_before);
	}
	CHECK(chunkDecode(b, old_style, w));
	CHECK(chunkGetForm(b) == CHUNK_FORM_PALETTE4);
	for (int i = 0; i < CHUNK_BLOCKS; i++) CHECK_QUIET(chunkGet(a, i) == chunkGet(b, i));

	budgetRelease(chunkGetBytes(b));
	chunkFree(a);
	chunkFree(b);
}

// ── v1.5.0 adaptive lighting ─────────────────────────────────────────────────
//
// The engine has two independent propagators — the BFS the worker runs at
// generation and the sweep the edit path runs — and the suite's job is to prove
// they agree byte-for-byte, that the storage nibbles round-trip, and that a
// mesher with the engine off emits exactly today's bytes.

// A stone slab across column (0,0) at one height: the standard sun test floor.
static void lightFloor(int y)
{
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, x, y, z, BLOCK_STONE));
}

// The two engines' answer for one column, compared byte-for-byte.
static void lightEnginesAgree(int cx, int cz)
{
	Column* col = worldColumn(&s_world, cx, cz);
	CHECK(col != NULL && col->light != NULL);

	uint8_t* bfs_sky = (uint8_t*)malloc(LIGHT_COL_BYTES);
	uint8_t* bfs_blk = (uint8_t*)malloc(LIGHT_COL_BYTES);
	CHECK(bfs_sky != NULL && bfs_blk != NULL);
	if (!bfs_sky || !bfs_blk) { free(bfs_sky); free(bfs_blk); return; }

	memcpy(bfs_sky, lightChannelSky(col), LIGHT_COL_BYTES);
	memcpy(bfs_blk, lightChannelBlock(col), LIGHT_COL_BYTES);

	// lightRelightColumnSweeps, NOT lightRelightColumn. Since v1.8.0 the latter IS the flood
	// fill — it was swapped onto it for the 26x — so calling it here would diff the BFS against
	// itself and pass unconditionally, which is precisely the tautology this helper was already
	// caught in once (see testLightIncrementalEqualsFull). The sweeps are the second engine.
	CHECK(lightRelightColumnSweeps(&s_world, cx, cz));

	const int diff = memcmp(bfs_sky, lightChannelSky(col), LIGHT_COL_BYTES) ||
	                 memcmp(bfs_blk, lightChannelBlock(col), LIGHT_COL_BYTES);
	CHECK(diff == 0);

	free(bfs_sky);
	free(bfs_blk);
}

static void testLightStorage(void)
{
	worldInit(&s_world);
	Column* col = worldColumnCreate(&s_world, 0, 0);
	CHECK(col != NULL);

	const int    before_n = lightColumnsAttached();
	const size_t before_b = lightBytesUsed();

	CHECK(lightColumnAttach(col));
	CHECK(lightColumnsAttached() == before_n + 1);
	CHECK(lightBytesUsed() == before_b + 2 * LIGHT_COL_BYTES);

	// Idempotent: attaching twice is one allocation.
	CHECK(lightColumnAttach(col));
	CHECK(lightColumnsAttached() == before_n + 1);

	// Nibble round trip, including two cells sharing one byte along x.
	lightSetSkyForTest(col, 0, 5, 0, 15);
	lightSetSkyForTest(col, 1, 5, 0, 7);
	CHECK(lightGetSky(col, 0, 5, 0) == 15);
	CHECK(lightGetSky(col, 1, 5, 0) == 7);
	CHECK(lightGetSky(col, 1, 6, 0) == 0);    // untouched neighbours stay dark
	CHECK(lightGetBlock(col, 0, 5, 0) == 0);  // the other channel is separate
	CHECK(lightGetSky(col, 0, -1, 0) == 0);   // below the floor reads unlit
	CHECK(lightGetSky(col, 0, WORLD_HEIGHT, 0) == 15);   // above the ceiling: sun

	// Copy carries both channels to a second column.
	Column* dst = worldColumnCreate(&s_world, 1, 0);
	CHECK(dst != NULL);
	CHECK(lightColumnCopy(col, dst));
	CHECK(lightGetSky(dst, 1, 5, 0) == 7);

	lightColumnDetach(col);
	lightColumnDetach(dst);
	CHECK(lightColumnsAttached() == before_n);
	CHECK(lightBytesUsed() == before_b);

	worldExit(&s_world);
}

static void testLightSkySunColumns(void)
{
	lightEngineInit(true);
	worldInit(&s_world);
	lightFloor(40);

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) { worldExit(&s_world); return; }
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, 3, 41, 3) == 15);       // open sky above the slab
	CHECK(lightGetSky(col, 3, 127, 3) == 15);
	CHECK(lightGetSky(col, 3, 40, 3) == 0);        // the slab itself stops light
	CHECK(lightGetSky(col, 3, 39, 3) == 0);        // sealed underneath stays dark
	CHECK(lightGetBlock(col, 3, 41, 3) == 0);      // no luminous blocks yet

	lightEnginesAgree(0, 0);

	worldExit(&s_world);
	free(q);
	lightEngineInit(false);
}

static void testLightSkyOverhang(void)
{
	lightEngineInit(true);
	worldInit(&s_world);
	lightFloor(40);
	// A roof over the east half: cells under it are lit only from the open west.
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 4; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, x, 44, z, BLOCK_STONE));

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) { worldExit(&s_world); return; }
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, 4, 43, 8) == 14);   // one step under the roof edge
	CHECK(lightGetSky(col, 5, 43, 8) == 13);   // the falloff gradient marches east
	CHECK(lightGetSky(col, 15, 43, 8) == 3);   // eleven steps from the lit edge
	CHECK(lightGetSky(col, 10, 45, 8) == 15);  // above the roof is open sky again
	CHECK(lightGetSky(col, 10, 41, 8) == 8);   // every under-roof row fades alike:
	                                           // seven horizontal steps from the
	                                           // lit column, whatever the height
	CHECK(lightGetSky(col, 3, 43, 8) == 15);   // the lit column itself

	lightEnginesAgree(0, 0);

	worldExit(&s_world);
	free(q);
	lightEngineInit(false);
}

static void testLightBlockChannel(void)
{
	lightEngineInit(true);
	worldInit(&s_world);

	// A sealed stone shell with an air room inside, and one luminous block in
	// the middle of it. No registry block emits in production — the table is
	// patched here so the channel is proven end-to-end without inventing a torch.
	for (int y = 39; y <= 45; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				const bool room = (y >= 40 && y <= 44 && x >= 4 && x <= 11 &&
				                   z >= 4 && z <= 11);
				CHECK_QUIET(worldSet(&s_world, x, y, z,
				                     room ? BLOCK_AIR : BLOCK_STONE));
			}
	CHECK(worldSet(&s_world, 7, 42, 7, BLOCK_WOOD));

	lightSetLuminanceForTest(BLOCK_WOOD, 7);

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) { worldExit(&s_world); return; }
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetBlock(col, 7, 42, 7) == 7);   // the emitter holds its own level
	CHECK(lightGetBlock(col, 8, 42, 7) == 6);   // -1 per air cell outward
	CHECK(lightGetBlock(col, 11, 42, 7) == 3);  // four steps east along the room
	CHECK(lightGetBlock(col, 11, 44, 11) == 0); // ten steps to the far corner:
	                                            // past the emitter's reach
	CHECK(lightGetBlock(col, 7, 46, 7) == 0);   // beyond the sealed shell: nothing
	CHECK(lightGetSky(col, 7, 42, 7) == 0);     // indoors: no sky in here

	lightEnginesAgree(0, 0);

	lightSetLuminanceForTest(BLOCK_WOOD, 0);    // restore production truth
	worldExit(&s_world);
	free(q);
	lightEngineInit(false);
}

// The same room, lit by a SERVER-REGISTERED block instead of a core one.
//
// v1.6.0: light.c's anyLuminance() — the gate both engines ask before running the block
// channel at all — looped `i < BLOCK_COUNT` over a table that is the full 256-id space
// wide. So a luminance set on a dynamic id (0x80..0xFD) read back as "nothing in this
// world glows", the block-seed pass was skipped entirely, and the emitter sat dark with no
// error anywhere. Dormant in production (no dynamic block declares luminance yet) and the
// same class of bug as visgraph.c's openTable(), inventory's ceiling and the mesher's
// per-id tables.
//
// Deliberately a near-copy of testLightBlockChannel above rather than a parameterised
// merge: the two differ in exactly one thing — which half of the id space the emitter
// lives in — and that is the whole claim. A shared body would let a change to the core
// arm silently move the dynamic one too.
static void testLightBlockChannelFromDynamicId(void)
{
	// Registered through the real call the DEFS path ends in, so the id really is a
	// dynamic one and not a hand-picked constant.
	BlockDef def;
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "test_lamp");
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_STONE;
	def.flags     = REG_FLAG_SOLID | REG_FLAG_LUMINOUS;
	def.luminance = 7;
	const BlockId lamp = registryRegister(&def);
	mesherInvalidateTables();

	CHECK(lamp >= REG_ID_DYN_LO);          // over the BLOCK_COUNT ceiling, which is the point
	if (lamp < REG_ID_DYN_LO) { restoreCoreRegistry(); return; }

	lightEngineInit(true);
	worldInit(&s_world);

	for (int y = 39; y <= 45; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				const bool room = (y >= 40 && y <= 44 && x >= 4 && x <= 11 &&
				                   z >= 4 && z <= 11);
				CHECK_QUIET(worldSet(&s_world, x, y, z,
				                     room ? BLOCK_AIR : BLOCK_STONE));
			}
	CHECK(worldSet(&s_world, 7, 42, 7, lamp));

	// light.c's table is still filled by hand — wiring it from BlockDef.luminance is a
	// separate piece of work. What is under test here is the SCAN, not the wiring.
	lightSetLuminanceForTest(lamp, 7);

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) { worldExit(&s_world); restoreCoreRegistry(); return; }
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetBlock(col, 7, 42, 7) == 7);   // the emitter holds its own level
	CHECK(lightGetBlock(col, 8, 42, 7) == 6);   // -1 per air cell outward
	CHECK(lightGetBlock(col, 11, 42, 7) == 3);  // four steps east along the room
	CHECK(lightGetBlock(col, 11, 44, 11) == 0); // past the emitter's reach
	CHECK(lightGetBlock(col, 7, 46, 7) == 0);   // beyond the sealed shell: nothing

	// Both engines, same as the core arm: the relaxation sweeps read the same table
	// through the same gate, so a bound that is wrong in one is wrong in both.
	lightEnginesAgree(0, 0);

	lightSetLuminanceForTest(lamp, 0);          // restore production truth
	worldExit(&s_world);
	free(q);
	lightEngineInit(false);
	restoreCoreRegistry();
}

// The design doc's classic drift check: a long sequence of single-block edits,
// each followed by the edit-path relight, must land exactly where a fresh full
// recompute of the final terrain lands. Two independent engines have to agree.
static void testLightIncrementalEqualsFull(void)
{
	lightEngineInit(true);
	worldInit(&s_world);

	// Random relief: stone columns of varying height, some capped with leaves so
	// the transparent-block rule is exercised too.
	Rng rng;
	rngSeed(&rng, 0x5EED1234);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++) {
			const int h = 30 + (int)rngBelow(&rng, 24);
			for (int y = 0; y <= h; y++) {
				const BlockId id =
				    (y == h && rngBelow(&rng, 4) == 0) ? BLOCK_LEAVES : BLOCK_STONE;
				CHECK_QUIET(worldSet(&s_world, x, y, z, id));
			}
		}

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) { worldExit(&s_world); return; }
	lightQueueInit(q);

	// First light, then 200 edits each followed by the edit-path engine.
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	for (int e = 0; e < 200; e++) {
		const int x = (int)rngBelow(&rng, CHUNK_DIM);
		const int z = (int)rngBelow(&rng, CHUNK_DIM);
		const int y = 28 + (int)rngBelow(&rng, 28);
		const BlockId id = worldGet(&s_world, x, y, z) == BLOCK_AIR
		                       ? BLOCK_STONE : BLOCK_AIR;
		CHECK_QUIET(worldSet(&s_world, x, y, z, id));
		CHECK_QUIET(lightRelightColumn(&s_world, 0, 0));
	}

	// The BFS run now must reproduce the edited-and-relit state exactly.
	//
	// v1.8.0: this call used to be the bare lightEnginesAgree() below, and it could
	// not fail. That helper snapshots whatever is in the column, runs the SWEEPS, and
	// diffs — so it only compares the engines when the state it snapshots came from
	// the BFS. Here the state came from the last lightRelightColumn of the loop above,
	// making it sweeps-vs-sweeps: deterministic, memset-first, identical by
	// construction. The header comment above has claimed "two independent engines have
	// to agree" since v1.5.0 and this test never once checked it. Running the flood
	// fill first is what makes the snapshot the BFS's answer.
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));
	lightEnginesAgree(0, 0);

	worldExit(&s_world);
	free(q);
	lightEngineInit(false);
}

// v1.8.0: a transparent block on TOP of its strip — leaves, water, tall grass — is the
// case that split the two engines, and every fixture above misses it because they cap
// their columns with stone. heightMapFill records the highest NON-AIR cell; opaqueAt is
// solid && !transparent. So such a block sits AT hm.top (the BFS's sky seeding starts at
// top+1 and never reaches it) while still passing light (the sweeps read the 15 above it
// and store 14). Measured before the fix: an 8x8x2 lake read 0 on all 128 of its cells
// from lightPropagateColumn and 13-14 from lightRelightColumn. A generated lake was black
// until an edit anywhere in that column relit it and it snapped bright.
//
// The fix is one extra seeding condition in lightPropagateColumn: the LOWEST sky cell of a
// strip is a border seed when the cell beneath it is not opaque. Only the lowest — every
// cell above it has open sky below and testing those would enqueue the whole sky.
static void testLightTransparentTopStrip(void)
{
	lightEngineInit(true);

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) return;

	// A lake: a stone bed with two layers of water on it, no relief anywhere, so the
	// horizontal border tests find nothing and the vertical one is the only seed.
	worldInit(&s_world);
	lightFloor(38);
	for (int z = 4; z <= 11; z++)
		for (int x = 4; x <= 11; x++) {
			CHECK_QUIET(worldSet(&s_world, x, 39, z, BLOCK_WATER));
			CHECK_QUIET(worldSet(&s_world, x, 40, z, BLOCK_WATER));
		}
	// The shore has to be as tall as the water or the lake gets a lit edge to spread
	// from and the vertical seed stops being the thing under test.
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 0; x < CHUNK_DIM; x++)
			if (x < 4 || x > 11 || z < 4 || z > 11) {
				CHECK_QUIET(worldSet(&s_world, x, 39, z, BLOCK_STONE));
				CHECK_QUIET(worldSet(&s_world, x, 40, z, BLOCK_STONE));
			}

	lightQueueInit(q);
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, 8, 41, 8) == 15);   // open sky over the middle of the lake
	CHECK(lightGetSky(col, 8, 40, 8) == 14);   // the surface itself — 0 before the fix
	CHECK(lightGetSky(col, 8, 39, 8) == 13);   // and one deeper
	CHECK(lightGetSky(col, 8, 38, 8) == 0);    // the bed is stone: still dark

	lightEnginesAgree(0, 0);
	worldExit(&s_world);

	// A leaf canopy, where the failure was subtler: the crown's INTERIOR was reachable
	// sideways from the edge, so it came out one short rather than black.
	worldInit(&s_world);
	lightFloor(40);
	for (int z = 6; z <= 10; z++)
		for (int x = 6; x <= 10; x++)
			CHECK_QUIET(worldSet(&s_world, x, 42, z, BLOCK_LEAVES));

	lightQueueInit(q);
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, 8, 42, 8) == 14);   // crown centre — 13 before the fix
	CHECK(lightGetSky(col, 6, 42, 6) == 14);   // and its corner, lit from above alike
	CHECK(lightGetSky(col, 8, 41, 8) == 13);   // the shade under the crown

	lightEnginesAgree(0, 0);
	worldExit(&s_world);

	// Tall grass on a flat plain: one cell tall, so it was already reachable sideways
	// and always agreed. It is here as the control — this case must stay green whether
	// the vertical seed exists or not, which is what makes the two above mean something.
	worldInit(&s_world);
	lightFloor(40);
	for (int z = 0; z < CHUNK_DIM; z += 3)
		for (int x = 0; x < CHUNK_DIM; x += 3)
			CHECK_QUIET(worldSet(&s_world, x, 41, z, BLOCK_TALL_GRASS));

	lightQueueInit(q);
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));
	lightEnginesAgree(0, 0);
	worldExit(&s_world);

	free(q);
	lightEngineInit(false);
}

// v1.8.0 world/relightq.h: the coalescing set that stopped main.c's onRemoteEdit paying for
// one whole-column relight per remote edit. The property that matters is that N edits on one
// column produce exactly ONE entry — that is the entire reason the file exists.
static void testRelightQueue(void)
{
	RelightQueue q;
	relightqInit(&q);
	CHECK(relightqCount(&q) == 0);
	CHECK(relightqCoalesced(&q) == 0);
	CHECK(relightqOverflows(&q) == 0);

	// The rejoin shape: a burst of diffs, all on the same column, as
	// networldOnColumnLoad's drain produces.
	for (int i = 0; i < 500; i++) CHECK_QUIET(relightqPush(&q, 3, -7));
	CHECK(relightqCount(&q) == 1);          // 500 edits, one relight
	CHECK(relightqCoalesced(&q) == 499);

	// Distinct columns each earn their own entry, negatives included — column indices are
	// signed and int16_t has to carry them intact.
	CHECK(relightqPush(&q, -7, 3));         // not the same column as (3,-7)
	CHECK(relightqPush(&q, -300, 20000));
	CHECK(relightqCount(&q) == 3);

	int cx = 0, cz = 0;
	bool saw_far = false;
	int popped = 0;
	while (relightqPop(&q, &cx, &cz)) {
		if (cx == -300 && cz == 20000) saw_far = true;
		popped++;
	}
	CHECK(popped == 3);
	CHECK(saw_far);                          // round-tripped, not truncated
	CHECK(relightqCount(&q) == 0);
	CHECK(!relightqPop(&q, &cx, &cz));       // empty pop reports empty

	// Overflow refuses rather than dropping silently: false is the caller's instruction to
	// relight inline. Only loaded columns are ever pushed and there are at most
	// RENDER_DIST_MAX_COLUMNS of those, so this cannot happen in the client — it is here
	// because "cannot happen" is how the dirtyq bug that stopped v1.6.0 booting was argued.
	relightqInit(&q);
	for (int i = 0; i < RELIGHTQ_CAP; i++) CHECK_QUIET(relightqPush(&q, i, 0));
	CHECK(relightqCount(&q) == RELIGHTQ_CAP);
	CHECK(!relightqPush(&q, 9999, 9999));    // full: refused
	CHECK(relightqOverflows(&q) == 1);
	CHECK(relightqPush(&q, 0, 0));           // but an already-listed column still succeeds
	CHECK(relightqCount(&q) == RELIGHTQ_CAP);

	// The set is wide enough for every column the render distance can load, which is the
	// bound the drain-to-empty in main.c relies on for its frame cost.
	CHECK(RELIGHTQ_CAP >= RENDER_DIST_MAX_COLUMNS);

	// NULL is safe on every entry point — the hook path can be reached before init.
	relightqInit(NULL);
	CHECK(!relightqPush(NULL, 0, 0));
	CHECK(!relightqPop(NULL, &cx, &cz));
	CHECK(relightqCount(NULL) == 0);
	CHECK(relightqCoalesced(NULL) == 0);
	CHECK(relightqOverflows(NULL) == 0);
}

// v1.7.1 task 46. Where the player's feet go on entering a world.
//
// The shipped bug: spawn y came from worldgenHeight() alone, a pure function of the seed that
// cannot know what anyone has built. Reload a world with a block laid on the spawn cell and the
// player was placed inside it, unable to move (the physics resolvers snap back on a blocked
// move) and seeing back-faces — which reads as "the chunk I'm in didn't load".
//
// Every case below is a real thing a player does at spawn, not a synthetic edge. The one that
// would have caught the bug is the second.
static void testSpawnStandingY(void)
{
	const int X = 8, Z = 8, GROUND = 40;   // GROUND = the generator's answer: first air above dirt

	// 1. Untouched terrain — the overwhelmingly common case, and the one a fix must not move.
	//    Air all the way up, so the guess is returned unchanged and a fresh world spawns exactly
	//    where it always did.
	worldInit(&s_world);
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND);
	worldExit(&s_world);

	// 2. THE BUG. One block laid on the spawn cell — a floor, a path, the corner of a house.
	//    The generator still says GROUND, and GROUND is now solid.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, X, GROUND, Z, BLOCK_STONE));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND + 1);
	worldExit(&s_world);

	// 3. A tower. Five blocks stacked on the spawn cell puts the player on top of the fifth,
	//    not part way up it — the loop has to keep stepping, not step once.
	worldInit(&s_world);
	for (int i = 0; i < 5; i++)
		CHECK(worldSet(&s_world, X, GROUND + i, Z, BLOCK_STONE));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND + 5);
	worldExit(&s_world);

	// 4. Head clearance is really two blocks. A single block at GROUND+1 with air at GROUND
	//    leaves a 1-block gap the 1.8-tall body does not fit in, so standing at GROUND would
	//    put the player's head inside it.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, X, GROUND + 1, Z, BLOCK_STONE));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND + 2);
	worldExit(&s_world);

	// 5. A roof with air underneath. The player built a ceiling five blocks up and lived under
	//    it; they must come back where they were, on the ground, NOT on top of the roof. This is
	//    the case that rules out "scan down from the sky for the highest surface".
	worldInit(&s_world);
	CHECK(worldSet(&s_world, X, GROUND + 5, Z, BLOCK_PLANKS));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND);
	worldExit(&s_world);

	// 6. Dug down. The cell below the guess is gone, but the guess itself was already air, so
	//    nothing moves and the player falls the short distance into their own hole — which is
	//    what happened before this function existed and is correct.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, X, GROUND - 1, Z, BLOCK_AIR));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND);
	worldExit(&s_world);

	// 7. Tall grass is not a floor. A plant is drawn and targetable but not solid, so it must
	//    not push the spawn up — otherwise every generated meadow would spawn the player one
	//    block in the air. Shares the predicate with physics, which is why it is one check.
	worldInit(&s_world);
	CHECK(worldSet(&s_world, X, GROUND, Z, (BlockId)BLOCK_TALL_GRASS));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == GROUND);
	worldExit(&s_world);

	// 8. Solid to the ceiling. A pillar the player built all the way up must terminate the loop
	//    at the top of the world instead of walking off it. WORLD_HEIGHT - 2 is the highest y a
	//    1.8-tall body still fits at.
	worldInit(&s_world);
	for (int y = GROUND; y < WORLD_HEIGHT; y++)
		CHECK(worldSet(&s_world, X, y, Z, BLOCK_STONE));
	CHECK(worldStandingY(&s_world, X, Z, GROUND) == WORLD_HEIGHT - 2);
	worldExit(&s_world);

	// 9. A negative or absurd guess is clamped rather than trusted. worldGet answers
	//    WORLD_FLOOR_BLOCK below y 0, so an unclamped start would loop up through solid rock
	//    from wherever it began.
	worldInit(&s_world);
	CHECK(worldStandingY(&s_world, X, Z, -5) == 0);
	worldExit(&s_world);
}

// v1.8.0 task 21. The 20 TPS clock.
//
// The claim being tested is a RATE, so most of this feeds the clock a realistic stream of real
// time and counts what comes out, rather than poking the struct. A clock that returns plausible
// numbers per call and still drifts is exactly the bug that would ship otherwise, and it is
// invisible to any single-call assertion.
static void testTickClock(void)
{
	TickClock c;
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	CHECK(tickClockCount(&c) == 0);
	CHECK(tickClockDropped(&c) == 0);
	CHECK(tickClockUntilNextUs(&c) == TICK_PERIOD_US);

	// Time shorter than a tick banks and produces nothing.
	CHECK(tickClockAdvance(&c, TICK_PERIOD_US - 1) == 0);
	CHECK(tickClockCount(&c) == 0);
	CHECK(tickClockUntilNextUs(&c) == 1);

	// ...and the very next microsecond completes it, which is only true because the remainder
	// was kept.
	CHECK(tickClockAdvance(&c, 1) == 1);
	CHECK(tickClockCount(&c) == 1);
	CHECK(tickClockUntilNextUs(&c) == TICK_PERIOD_US);

	// THE RATE. 60 frames of 16,667 us is one second of a console holding 60 fps, and one
	// second must be 20 ticks — not 19, not 21. The frame time deliberately does not divide the
	// tick period: 50,000 / 16,667 is 2.9999, so a clock that rounded per frame instead of
	// accumulating would land here.
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	for (int f = 0; f < 60; f++) CHECK_QUIET(tickClockAdvance(&c, 16667) >= 0);
	CHECK(tickClockCount(&c) == 20);
	CHECK(tickClockDropped(&c) == 0);

	// The real measured console frame time is 59.83 fps = 16,714 us, and ten seconds of it must
	// still be 200 ticks. This is the drift check: an error of one part in 600 per frame would
	// be invisible above but shows up as a whole missing tick here.
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	for (int f = 0; f < 599; f++) CHECK_QUIET(tickClockAdvance(&c, 16714) >= 0);
	CHECK(tickClockCount(&c) == 200);

	// The server's shape: sleep exactly as long as the clock says, then bank exactly that. Every
	// wake must be worth exactly one tick, and a second must be 20 of them.
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	int64_t slept_us = 0;
	for (int i = 0; i < 20; i++) {
		const int64_t sleep_us = tickClockUntilNextUs(&c);
		slept_us += sleep_us;
		CHECK_QUIET(tickClockAdvance(&c, sleep_us) == 1);
	}
	CHECK(tickClockCount(&c) == 20);
	CHECK(slept_us == 1000000);              // exactly one second of sleeping, no more, no less

	// The death-spiral clamp. A one-second stall asks for 20 ticks and gets max_catchup.
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	CHECK(tickClockAdvance(&c, 1000000) == TICK_MAX_CATCHUP_DEFAULT);
	CHECK(tickClockCount(&c) == (uint64_t)TICK_MAX_CATCHUP_DEFAULT);
	CHECK(tickClockDropped(&c) == (uint64_t)(20 - TICK_MAX_CATCHUP_DEFAULT));

	// ...and the very next ordinary advance is ordinary again. If the clamp left the surplus in
	// the accumulator, this would return max_catchup forever and the game would never recover
	// from one bad frame — which is the whole failure the clamp exists to prevent.
	CHECK(tickClockAdvance(&c, TICK_PERIOD_US) == 1);
	CHECK(tickClockDropped(&c) == (uint64_t)(20 - TICK_MAX_CATCHUP_DEFAULT));   // no new drops

	// A backwards clock rewinds nothing.
	tickClockInit(&c, TICK_MAX_CATCHUP_DEFAULT);
	CHECK(tickClockAdvance(&c, 30000) == 0);
	CHECK(tickClockAdvance(&c, -1000000) == 0);
	CHECK(tickClockUntilNextUs(&c) == TICK_PERIOD_US - 30000);   // the bank is untouched
	CHECK(tickClockAdvance(&c, 20000) == 1);                     // and still completes normally

	// max_catchup can never be zero, whatever it is handed.
	tickClockInit(&c, 0);
	CHECK(tickClockAdvance(&c, 1000000) == 1);
	tickClockInit(&c, -5);
	CHECK(tickClockAdvance(&c, 1000000) == 1);

	// NULL is safe everywhere.
	tickClockInit(NULL, 4);
	CHECK(tickClockAdvance(NULL, 50000) == 0);
	CHECK(tickClockUntilNextUs(NULL) == TICK_PERIOD_US);
	CHECK(tickClockCount(NULL) == 0);
	CHECK(tickClockDropped(NULL) == 0);

	// ---- the spec's distance decimation ----

	CHECK(TICK_NEAR_DIST_SQ == 576);                     // 24 blocks, squared
	CHECK(tickPeriodForDistSq(0) == 1);
	CHECK(tickPeriodForDistSq(575) == 1);
	CHECK(tickPeriodForDistSq(576) == 1);                // exactly 24 blocks is still "within"
	CHECK(tickPeriodForDistSq(577) == TICK_FAR_PERIOD);  // one block-unit further is not
	CHECK(tickPeriodForDistSq(1 << 20) == TICK_FAR_PERIOD);
	CHECK(TICK_HZ / TICK_FAR_PERIOD == 2);               // the far rate really is 2 Hz

	// Period 1 is always due; that is the near case and it must cost nothing.
	for (uint64_t t = 0; t < 40; t++) CHECK_QUIET(tickDue(t, 1, 12345));
	for (uint64_t t = 0; t < 40; t++) CHECK_QUIET(tickDue(t, 0, 12345));

	// A decimated thing fires exactly twice a second and no more: 200 ticks is ten seconds, so
	// 20 firings.
	int fired = 0;
	for (uint64_t t = 0; t < 200; t++)
		if (tickDue(t, TICK_FAR_PERIOD, 7)) fired++;
	CHECK(fired == 20);

	// The stagger, which is the reason `id` is in the signature at all. Ten entities with ten
	// consecutive ids must land on ten DIFFERENT ticks — one apiece per period, never all ten
	// on the same tick. Without this the 2 Hz saving is a 2 Hz spike.
	for (uint64_t t = 0; t < TICK_FAR_PERIOD; t++) {
		int due_here = 0;
		for (uint32_t id = 0; id < (uint32_t)TICK_FAR_PERIOD; id++)
			if (tickDue(t, TICK_FAR_PERIOD, id)) due_here++;
		CHECK_QUIET(due_here == 1);
	}

	// And every one of those ten is due exactly once across the period — the stagger spreads the
	// work, it does not drop any of it.
	for (uint32_t id = 0; id < (uint32_t)TICK_FAR_PERIOD; id++) {
		int due_across = 0;
		for (uint64_t t = 0; t < TICK_FAR_PERIOD; t++)
			if (tickDue(t, TICK_FAR_PERIOD, id)) due_across++;
		CHECK_QUIET(due_across == 1);
	}
}

// Constraint 1 made executable: with the engine off, meshChunk's vertex bytes are
// exactly what the pre-lighting mesher produced — pad zero everywhere, even when
// the scratch's light band holds garbage. With it on, only pad may differ, and
// only where light actually varies.
static void testMesherO3DSParity(void)
{
	MeshOut ref = {0}, lit = {0};
	ref.vert_cap = lit.vert_cap = MESH_MAX_VERTS;
	ref.index_cap = lit.index_cap = MESH_MAX_INDICES;
	ref.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * MESH_MAX_VERTS);
	ref.indices   = (uint16_t*)malloc(sizeof(uint16_t) * MESH_MAX_INDICES);
	lit.verts     = (MeshVertex*)malloc(sizeof(MeshVertex) * MESH_MAX_VERTS);
	lit.indices   = (uint16_t*)malloc(sizeof(uint16_t) * MESH_MAX_INDICES);
	CHECK(ref.verts && ref.indices && lit.verts && lit.indices);
	if (!ref.verts || !ref.indices || !lit.verts || !lit.indices) {
		free(ref.verts); free(ref.indices); free(lit.verts); free(lit.indices);
		return;
	}

	// Floor, an overhang casting real shadow, and a pillar for tile variety.
	worldInit(&s_world);
	lightFloor(40);
	for (int z = 0; z < CHUNK_DIM; z++)
		for (int x = 6; x < CHUNK_DIM; x++)
			CHECK_QUIET(worldSet(&s_world, x, 44, z, BLOCK_STONE));
	for (int y = 41; y <= 43; y++)
		CHECK_QUIET(worldSet(&s_world, 2, y, 2, BLOCK_WOOD));

	// --- Engine off: reference bytes. Garbage in the light band must not leak.
	memset(s_scratch.light, 0xFF, sizeof(s_scratch.light));
	scratchFill(&s_scratch, &s_world, 0, 2, 0);
	meshChunk(&ref, &s_scratch);
	CHECK(ref.faces > 0);

	bool any_pad_set = false;
	for (uint32_t i = 0; i < ref.vert_count; i++)
		if (ref.verts[i].pad != 0) any_pad_set = true;
	CHECK(!any_pad_set);

	// --- Engine on: same geometry, light baked into pad.
	lightEngineInit(true);
	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) {
		lightEngineInit(false);
		worldExit(&s_world);
		free(ref.verts); free(ref.indices); free(lit.verts); free(lit.indices);
		return;
	}
	lightQueueInit(q);
	CHECK(lightPropagateColumn(&s_world, 0, 0, q));

	scratchFill(&s_scratch, &s_world, 0, 2, 0);
	scratchFillLight(&s_scratch, &s_world, 0, 2, 0);
	meshChunk(&lit, &s_scratch);

	// v1.6.0 task 11: the two arms are no longer byte-identical and cannot be. A greedy
	// run stops wherever anything visible about the next face differs, and baked light is
	// one of those things — so the lit arm merges strictly less than the unlit one and the
	// vertex arrays stop lining up index for index. "Nothing but pad moved" is therefore
	// restated as "the same block faces, in the same places, differently grouped", which is
	// the claim that was always meant. Comparing bytes here would only be testing that the
	// merge rule happens to agree between the arms, which is exactly what it must not do.
	CHECK(meshFaceCells(&lit) == meshFaceCells(&ref));
	CHECK(meshCoverageEqual(&ref, &lit));
	CHECK(lit.faces >= ref.faces);        // extra shading detail can only split runs
	CHECK(meshIndexPatternOk(&lit));
	CHECK(meshIndexPatternOk(&ref));

	int pads_set = 0, shadowed = 0;
	for (uint32_t i = 0; i < lit.vert_count; i++) {
		const uint8_t p = (uint8_t)lit.verts[i].pad;
		if (p != 0) pads_set++;
		if ((p >> 4) < 15) shadowed++;
	}
	CHECK(pads_set > 0);            // pad really carries the light
	CHECK(shadowed > 0);            // the overhang's shade reached vertices

	// Fully sunlit corners average four taps of 15 and pack to exactly 0xF0:
	// the value that makes the dynamic shader reproduce the baked brightness.
	// (pad is declared int8_t; the GPU fetches it unsigned, so compare bytes.)
	int full_sun = 0;
	for (uint32_t i = 0; i < lit.vert_count; i++)
		if ((uint8_t)lit.verts[i].pad == 0xF0) full_sun++;
	CHECK(full_sun > 0);

	lightEngineInit(false);
	worldExit(&s_world);
	free(q);
	free(ref.verts); free(ref.indices); free(lit.verts); free(lit.indices);
}

// ── v1.7.0 terrain rework ─────────────────────────────────────────────────────────────

// FNV-1a over every block of one generated column. The whole column, not a sample, because
// the claim being pinned is byte identity and a sampled hash would pass over a change that
// only moved one block.
static uint32_t genTestHashColumn(const World* w, int32_t cx, int32_t cz)
{
	uint32_t h = 2166136261u;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int lz = 0; lz < CHUNK_DIM; lz++)
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				h ^= (uint8_t)worldGet(w, cx * CHUNK_DIM + lx, y, cz * CHUNK_DIM + lz);
				h *= 16777619u;
			}
	return h;
}

// The four materials the ground can be made of. A predicate rather than an inline
// disjunction so CHECK_QUIET's stringified condition fits its 96-byte report buffer.
static bool genTestIsGround(BlockId b)
{
	// BLOCK_SNOW joined this list in v1.8.3 Phase 3 and it is a widening of it, so it is
	// worth saying why it is not a hole. Snow is a SURFACE CAP, not a decoration:
	// worldgen_density.c's surfaceBlock() returns it from the `depth == 0` arm of the
	// tundra branch, in exactly the slot grass occupies in a plains column and sand in a
	// desert one. Under it is dirt and then stone, like everywhere else. A checker that
	// still refused it would be asserting that tundra has no ground.
	return b == BLOCK_STONE || b == BLOCK_DIRT || b == BLOCK_GRASS || b == BLOCK_SAND ||
	       b == BLOCK_SNOW;
}

// Is this block something that STANDS ON the ground rather than being it?
//
// One list, used by genTestTerrainTop() and by testDensityDistribution's own downward walk,
// which had drifted into keeping the same list twice. v1.8.3 Phase 3 is what made that
// expensive: it adds four more ids to step over, and a list updated in one place and not the
// other does not fail loudly — it silently reads the wrong cell, and every surface claim
// built on it quietly becomes a claim about a fern.
//
// SNOW IS DELIBERATELY NOT HERE, for the reason genTestIsGround() gives above. ICE is, and
// that is the other half of the same decision: ice caps the SEA, so the ground under a frozen
// lake is the seabed, not the pane floating on top of it. Cactus is here for the reason wood
// is — a trunk standing on sand is not the sand.
static bool genTestNotGround(BlockId b)
{
	return b == BLOCK_AIR       || b == BLOCK_WATER   || b == BLOCK_TALL_GRASS ||
	       b == BLOCK_WOOD      || b == BLOCK_LEAVES  ||
	       /* v1.8.3 Phase 3 */
	       b == BLOCK_ICE       || b == BLOCK_CACTUS  ||
	       b == BLOCK_DEAD_BUSH || b == BLOCK_FERN;
}

// **The single most important check in the v1.7.0 terrain rework.**
//
// A world stamped GEN_VERSION_LEGACY must generate terrain identical block for block to what
// the generator produced before the density field existed. Not similar, not the same shape —
// identical, because a player's house is placed against specific blocks.
//
// The hashes below were taken from the shipping generator BEFORE any of the v1.7.0 work was
// applied, by a probe linking the same worldgen.c this test links. They are pinned literals
// on purpose: a hash recomputed at test time from the code under test would agree with any
// change at all, which is the classic check that cannot go red.
//
// If this fails, the legacy path has been altered and every existing world on every SD card
// has changed shape. There is no acceptable way to update these numbers other than deciding,
// deliberately and separately, to break existing worlds.
static void testWorldgenLegacyByteIdentity(void)
{
	static const struct { uint32_t seed; int32_t cx, cz; uint32_t hash; } pinned[] = {
		{1337u,   0,  0, 0x036f1cd5u}, {1337u,   1,  0, 0x523065f4u},
		{1337u,  -1, -1, 0x2b5f273du}, {1337u,   7, -3, 0xbc2562b1u},
		{4242u,   0,  0, 0x1845ad62u}, {4242u,   1,  0, 0x8364d627u},
		{4242u,  -1, -1, 0x8d15246cu}, {4242u,   7, -3, 0xe9023721u},
		{90210u,  0,  0, 0x8360fda4u}, {90210u,  1,  0, 0x43d8f303u},
		{90210u, -1, -1, 0x34fd601fu}, {90210u,  7, -3, 0x11cfcfc9u},
	};

	for (size_t i = 0; i < sizeof(pinned) / sizeof(pinned[0]); i++) {
		WorldGen g;
		CHECK(worldgenInit(&g, pinned[i].seed, GEN_VERSION_LEGACY));
		worldInit(&s_world);
		CHECK(worldgenColumn(&g, &s_world, pinned[i].cx, pinned[i].cz));
		CHECK(genTestHashColumn(&s_world, pinned[i].cx, pinned[i].cz) == pinned[i].hash);
		worldExit(&s_world);
	}

	// The other half of the promise: the density generator must actually produce something
	// DIFFERENT. Without this, the checks above would pass for the wrong reason — a dispatch
	// that silently ran legacy for both versions would look perfect here.
	{
		WorldGen legacy, density;
		CHECK(worldgenInit(&legacy,  1337u, GEN_VERSION_LEGACY));
		CHECK(worldgenInit(&density, 1337u, GEN_VERSION_DENSITY));

		worldInit(&s_world);
		CHECK(worldgenColumn(&legacy, &s_world, 0, 0));
		const uint32_t h_legacy = genTestHashColumn(&s_world, 0, 0);
		worldExit(&s_world);

		worldInit(&s_world);
		CHECK(worldgenColumn(&density, &s_world, 0, 0));
		const uint32_t h_density = genTestHashColumn(&s_world, 0, 0);
		worldExit(&s_world);

		CHECK(h_legacy != h_density);
		CHECK(h_legacy == 0x036f1cd5u);
	}
}

// ── v1.8.3 Phase 2: the legacy sand rule, swept where the answer can actually move ────
//
// **This is the gate for the version dispatch inside worldgenIsSandy(), and
// testWorldgenLegacyByteIdentity() above is NOT.** That distinction was measured, not
// assumed, and it is the reason this function exists.
//
// The twelve fingerprints above pin seeds 1337 / 4242 / 90210 crossed with the columns
// (0,0) (1,0) (-1,-1) (7,-3). Under the exact sabotage they are supposed to catch — deleting
// the version gate so a legacy world resolves sand through the biome classifier — **eleven of
// the twelve cannot go red**: measured, only 4242 at (7,-3) moves, and it moves by a single
// cell. The four columns were chosen for tidiness (the origin, its neighbour, a negative pair
// to exercise sign handling, one arbitrary offset) and nobody checked what terrain they land
// on. Seed 1337 at the origin is 98.7 % one biome and 90210 is 100 % another. **A rule that
// decides between two materials cannot be caught changing in a region that only ever produces
// one of them.** The sabotage arms, the hash is computed correctly, the comparison is exact,
// and the answer is the same either way because the input never asked the question.
//
// The fingerprints are KEPT — they still catch wholesale corruption for almost nothing — but
// they are no longer the gate. This is.
//
// **Assertion 2 is the whole point and is the easy one to leave out.** Without it, assertion
// 1 is satisfied by any region where the two rules agree anyway, which is precisely the state
// the twelve pinned columns are in. So the sweep also counts how often the OLD rule and the
// NEW rule give different answers, and requires that count to be large. That makes the test
// arm from BOTH sides: delete the gate and assertion 1 goes red; hard-wire both versions to
// the old rule and assertion 2 goes red. One red direction proves one of the two things is
// load-bearing and says nothing at all about the other.
//
// Measured on this tree, 21,609 positions per seed:
//
//     seed 1337   legacy mismatch 0   disagree 3248 (15.03 %)
//     seed 4242   legacy mismatch 0   disagree 3687 (17.06 %)
//     seed 90210  legacy mismatch 0   disagree 5349 (24.75 %)
//
// so the `> 1000` bound has between three and five times the margin it needs, and it will
// itself go red if someone later narrows the sweep into a uniform region.
static void testWorldgenLegacySandyWideSweep(void)
{
	static const uint32_t seeds[3] = {1337u, 4242u, 90210u};

	long swept = 0;
	for (int s = 0; s < 3; s++) {
		WorldGen legacy, density;
		CHECK(worldgenInit(&legacy,  seeds[s], GEN_VERSION_LEGACY));
		CHECK(worldgenInit(&density, seeds[s], GEN_VERSION_DENSITY));

		long mismatch = 0, disagree = 0, old_sandy = 0, new_sandy = 0, n = 0;
		for (int32_t z = -512; z <= 512; z += 7) {
			for (int32_t x = -512; x <= 512; x += 7) {
				// The rule the legacy generator has always applied, spelled out in full
				// rather than by calling the function under test. A check written in terms
				// of the thing it is testing moves with it and keeps agreeing.
				const bool old_rule = worldgenBiome(&legacy, x, z) < GEN_SAND_BELOW;

				if (worldgenIsSandy(&legacy, x, z) != old_rule) mismatch++;
				const bool new_rule = worldgenIsSandy(&density, x, z);
				if (new_rule != old_rule) disagree++;
				if (old_rule) old_sandy++;
				if (new_rule) new_sandy++;
				n++;
			}
		}
		swept += n;

		// 1. A legacy world's sand is exactly where it always was. Zero exceptions.
		CHECK(mismatch == 0);

		// 2. ...and the two rules were CAPABLE of disagreeing here, so the zero above is a
		// fact about the gate rather than a fact about the coordinates.
		CHECK(disagree > 1000);

		// Non-vacuity for each rule on its own: a region that came out entirely sand, or
		// entirely not, under either rule would satisfy assertion 1 for free.
		CHECK(old_sandy > 0 && old_sandy < n);
		CHECK(new_sandy > 0 && new_sandy < n);

		// The new rule can only ever REMOVE sand, never add it: desert is hot AND dry, and
		// hot alone is character for character the old test. new_sandy above old_sandy would
		// mean GEN_TEMP_HOT had stopped being derived from GEN_SAND_BELOW.
		CHECK(new_sandy < old_sandy);
	}

	// **The control.** It stays green under both sabotage arms above — neither one changes
	// how many positions the loops visit — and it is not vacuous: narrowing the sweep, which
	// is the change that would quietly turn assertion 2 back into decoration, makes it red.
	// 147 values of x and of z per seed, over three seeds.
	CHECK(swept == 147L * 147L * 3L);
}

// ── v1.8.3 Phase 2: the climate fields, the classifier and the per-biome table ─────────
static void testWorldgenBiomes(void)
{
	WorldGen g;
	CHECK(worldgenInit(&g, 1337u, GEN_VERSION_DENSITY));

	// **GEN_TEMP_HOT is an arithmetic identity, not a tuning constant**, and this is the
	// check that says so. `temp > FX_ONE - GEN_SAND_BELOW` must be the same predicate as
	// `biome < GEN_SAND_BELOW` at every position, or the density table's second control
	// point and the surface material have started deciding at two different coordinates.
	CHECK(GEN_TEMP_HOT == FX_ONE - GEN_SAND_BELOW);
	{
		long hot = 0, cold = 0, wet = 0, dry = 0, n = 0, bad_identity = 0, out_of_range = 0;
		for (int32_t z = -400; z <= 400; z += 11) {
			for (int32_t x = -400; x <= 400; x += 11) {
				const fx b = worldgenBiome(&g, x, z);
				const fx h = worldgenHumidity(&g, x, z);
				const fx t = FX_ONE - b;

				if (h < 0 || h > FX_ONE) out_of_range++;
				if ((t > GEN_TEMP_HOT) != (b < GEN_SAND_BELOW)) bad_identity++;

				if (t > GEN_TEMP_HOT)   hot++;
				if (t < GEN_TEMP_COLD)  cold++;
				if (h >= GEN_HUMID_WET) wet++; else dry++;
				n++;
			}
		}
		CHECK(out_of_range == 0);
		CHECK(bad_identity == 0);
		// Every band is reachable in the sampled region, or the identity above was checked
		// where only one answer occurs — the exact defect this whole section exists over.
		CHECK(hot > 0 && hot < n);
		CHECK(cold > 0 && cold < n);
		CHECK(wet > 0 && dry > 0);
	}

	// The humidity field is a SECOND field, not the temperature field wearing a hat. Two
	// fBms drawn from the same seed are visibly the same shape, and if the salt were dropped
	// the climate square would collapse onto its diagonal — only three biomes could occur.
	{
		long same = 0, n = 0;
		for (int32_t z = -400; z <= 400; z += 11)
			for (int32_t x = -400; x <= 400; x += 11) {
				if (worldgenHumidity(&g, x, z) == worldgenBiome(&g, x, z)) same++;
				n++;
			}
		CHECK(same * 100 < n);   // under 1 % coincidental equality
	}

	// The classifier is exactly the 3 x 2 rectangle, re-derived here from the two fields
	// rather than trusted.
	{
		long wrong = 0;
		for (int32_t z = -400; z <= 400; z += 11)
			for (int32_t x = -400; x <= 400; x += 11) {
				const fx t = FX_ONE - worldgenBiome(&g, x, z);
				const bool w = worldgenHumidity(&g, x, z) >= GEN_HUMID_WET;
				BiomeId want;
				if (t > GEN_TEMP_HOT)       want = w ? BIOME_JUNGLE : BIOME_DESERT;
				else if (t < GEN_TEMP_COLD) want = w ? BIOME_TAIGA  : BIOME_TUNDRA;
				else                        want = w ? BIOME_FOREST : BIOME_PLAINS;
				if (worldgenBiomeAt(&g, x, z) != want) wrong++;
			}
		CHECK(wrong == 0);
	}

	// Sand on a density world is the desert cap and nothing else. worldgen_density.c's
	// wgdColumn resolves the biome once per cell and compares against BIOME_DESERT rather
	// than asking worldgenIsSandy() a second time; this is what makes that an identity
	// instead of a second copy of the rule that can drift away from the first.
	{
		long drift = 0, sandy = 0, n = 0;
		for (int32_t z = -400; z <= 400; z += 11)
			for (int32_t x = -400; x <= 400; x += 11) {
				const bool s = worldgenIsSandy(&g, x, z);
				if (s != (worldgenBiomeAt(&g, x, z) == BIOME_DESERT)) drift++;
				if (s) sandy++;
				n++;
			}
		CHECK(drift == 0);
		CHECK(sandy > 0 && sandy < n);   // and both answers occur where it was checked
	}

	// ── The per-biome table ───────────────────────────────────────────────────────────
	//
	// The silhouette bounds are a CORRECTNESS constraint, not a style one: worldgenDecorate
	// derives its tree-cell scan from GEN_TREE_RADIUS, so a canopy wider than that would be
	// clipped at a column border depending on which column was generated first — an
	// order-dependence bug. The trunk bounds are what keeps treeInCell's world-ceiling check
	// honest.
	{
		int max_grass = 0;
		for (int b = 0; b < BIOME_COUNT; b++) {
			const BiomeParams* p = worldgenBiomeParams((BiomeId)b);
			CHECK_QUIET(p != NULL);
			CHECK_QUIET(p->canopy_radius >= 1 && p->canopy_radius <= GEN_TREE_RADIUS);
			CHECK_QUIET(p->trunk_min >= GEN_TREE_MIN_H);
			CHECK_QUIET(p->trunk_max <= GEN_TREE_MAX_H);
			CHECK_QUIET(p->trunk_min <= p->trunk_max);
			if (p->grass_chance > max_grass) max_grass = p->grass_chance;
		}
		// worldgenScatter rejects on GEN_GRASS_CHANCE_MAX before it resolves the biome. If
		// that stopped being an upper bound over the table, the biome with the highest
		// chance would be silently clamped to it and nothing else would notice.
		CHECK(max_grass == GEN_GRASS_CHANCE_MAX);

		// An id outside the table resolves to the baseline rather than reading off the end.
		CHECK(worldgenBiomeParams(BIOME_COUNT) == worldgenBiomeParams(BIOME_PLAINS));
		CHECK(worldgenBiomeParams((BiomeId)255) == worldgenBiomeParams(BIOME_PLAINS));

		// The numbers themselves, as naked literals. A check written in terms of the
		// constants it is testing moves with them and keeps agreeing — this project has a
		// recorded case of exactly that costing three of five reddened checks.
		CHECK(worldgenBiomeParams(BIOME_TUNDRA)->grass_chance == 0);
		CHECK(worldgenBiomeParams(BIOME_TAIGA)->grass_chance  == 12);
		CHECK(worldgenBiomeParams(BIOME_PLAINS)->grass_chance == 40);
		CHECK(worldgenBiomeParams(BIOME_FOREST)->grass_chance == 24);
		CHECK(worldgenBiomeParams(BIOME_DESERT)->grass_chance == 0);
		CHECK(worldgenBiomeParams(BIOME_JUNGLE)->grass_chance == 56);
		CHECK(worldgenBiomeParams(BIOME_TUNDRA)->tree_chance == 0);
		CHECK(worldgenBiomeParams(BIOME_TAIGA)->tree_chance  == 64);
		CHECK(worldgenBiomeParams(BIOME_PLAINS)->tree_chance == 24);
		CHECK(worldgenBiomeParams(BIOME_FOREST)->tree_chance == 128);
		CHECK(worldgenBiomeParams(BIOME_DESERT)->tree_chance == 0);
		CHECK(worldgenBiomeParams(BIOME_JUNGLE)->tree_chance == 160);
		// The three silhouettes: jungle tall and broad, taiga tall and narrow, the other
		// four unchanged. Each of the two differs from the default in one field only.
		CHECK(worldgenBiomeParams(BIOME_JUNGLE)->trunk_min == 6);
		CHECK(worldgenBiomeParams(BIOME_JUNGLE)->canopy_radius == 2);
		CHECK(worldgenBiomeParams(BIOME_TAIGA)->trunk_min == 6);
		CHECK(worldgenBiomeParams(BIOME_TAIGA)->canopy_radius == 1);
		CHECK(worldgenBiomeParams(BIOME_PLAINS)->trunk_min == GEN_TREE_MIN_H);
		CHECK(worldgenBiomeParams(BIOME_PLAINS)->canopy_radius == GEN_TREE_RADIUS);
	}

	// ── Reachability ──────────────────────────────────────────────────────────────────
	//
	// All six biomes have to occur in a world, or one of them is a branch nobody ever walks
	// into and every surface check below is testing five things. Measured here over these
	// three seeds at 1024 x 1024 blocks: worst single share 5.37 % (tundra on 90210). Over
	// the design page's full thirteen seeds the worst is 1.82 % (taiga on 1616), which is
	// where the 1 % bound comes from — set below the worst seed KNOWN rather than below the
	// three that are actually run here.
	{
		static const uint32_t seeds[3] = {1337u, 4242u, 90210u};
		for (int s = 0; s < 3; s++) {
			WorldGen gs;
			CHECK(worldgenInit(&gs, seeds[s], GEN_VERSION_DENSITY));
			long n[BIOME_COUNT] = {0}, tot = 0;
			for (int32_t z = -512; z < 512; z += 8)
				for (int32_t x = -512; x < 512; x += 8) {
					n[worldgenBiomeAt(&gs, x, z)]++;
					tot++;
				}
			for (int b = 0; b < BIOME_COUNT; b++)
				CHECK_QUIET(n[b] * 100 > tot);   // every biome over 1 % of the world
		}
	}
}

// The version contract that needs no filesystem: which values exist, which this build will
// run, and what worldgenInit does with one it does not know.
static void testGenVersionContract(void)
{
	// Append-only and ordered. A renumber would re-point every stamp already on a card.
	CHECK(GEN_VERSION_LEGACY == 1u);
	CHECK(GEN_VERSION_DENSITY == 2u);
	CHECK(GEN_VERSION_NEWEST == GEN_VERSION_DENSITY);
	CHECK(GEN_VERSION_FOR_NEW_WORLDS == GEN_VERSION_NEWEST);

	CHECK(!genVersionKnown(0u));
	CHECK(genVersionKnown(GEN_VERSION_LEGACY));
	CHECK(genVersionKnown(GEN_VERSION_DENSITY));
	CHECK(!genVersionKnown(GEN_VERSION_NEWEST + 1u));
	CHECK(!genVersionKnown(0xFFFFFFFFu));

	// A joined server session generates legacy, because the protocol carries no generator
	// version and two clients on different generators would stand in different worlds.
	CHECK(genVersionForSession() == GEN_VERSION_LEGACY);

	// worldgenInit refuses an unknown version rather than defaulting to one. Refusing is the
	// only safe answer: generating at the wrong version is corruption that looks like terrain.
	WorldGen g;
	CHECK(!worldgenInit(&g, 1337u, 0u));
	CHECK(!worldgenInit(&g, 1337u, GEN_VERSION_NEWEST + 1u));
	CHECK(!worldgenInit(&g, 1337u, 0xFFFFFFFFu));
	CHECK(worldgenInit(&g, 1337u, GEN_VERSION_LEGACY));
	CHECK(g.version == GEN_VERSION_LEGACY);
	CHECK(worldgenInit(&g, 1337u, GEN_VERSION_DENSITY));
	CHECK(g.version == GEN_VERSION_DENSITY);

	// The seed mix must not depend on the version, or stamping an existing world would move
	// its noise even on the legacy path.
	WorldGen a, b;
	CHECK(worldgenInit(&a, 4242u, GEN_VERSION_LEGACY));
	CHECK(worldgenInit(&b, 4242u, GEN_VERSION_DENSITY));
	CHECK(a.seed == b.seed);
}

// The biome table (task 16): shape, bounds, and the fact that it reads the EXISTING biome
// field rather than a second one of its own.
static void testDensityBiomeTable(void)
{
	int prev_h = -1, prev_a = -1;

	// Monotonic in both columns across the whole field range, and inside the world at both
	// extremes of the noise. Stepped finely enough to catch a dip inside one segment, not
	// only at the control points.
	for (int32_t v = 0; v <= FX_ONE; v += FX_ONE / 256) {
		int base_h, amp;
		wgdBiomeParams((fx)v, &base_h, &amp);

		CHECK_QUIET(base_h >= prev_h);
		CHECK_QUIET(amp >= prev_a);
		prev_h = base_h;
		prev_a = amp;

		// The nominal extremes of this biome must stay inside the world. The lower bound
		// matters as much as the upper: a base below amp/2 would put the surface at y < 0
		// and the fill loop would produce an empty column.
		CHECK_QUIET(base_h - amp / 2 > 0);
		CHECK_QUIET(base_h + amp / 2 < WORLD_HEIGHT);
	}
	CHECK(prev_h > 0);       // the loop actually ran

	// Clamped, not extrapolated, outside the control points.
	int lo_h, lo_a, hi_h, hi_a, edge_h, edge_a;
	wgdBiomeParams(0, &lo_h, &lo_a);
	wgdBiomeParams(0x00001333, &edge_h, &edge_a);
	CHECK(lo_h == edge_h);
	CHECK(lo_a == edge_a);
	wgdBiomeParams(FX_ONE, &hi_h, &hi_a);
	wgdBiomeParams(0x0000EF9D, &edge_h, &edge_a);
	CHECK(hi_h == edge_h);
	CHECK(hi_a == edge_a);

	// The table must straddle sea level. The lowest biome is under it *even at its nominal
	// peak*, so there is a real seabed for task 17 to fill; the highest has its BASE above
	// it, so its ground is dry land — but not its whole nominal range, because a mountain
	// biome's low points are valleys and a valley floor at 56 is correct.
	CHECK(lo_h + lo_a / 2 < GEN_SEA_LEVEL);
	CHECK(hi_h > GEN_SEA_LEVEL);

	// It reads the existing field. The sand threshold is a control point, so the shape and
	// the surface material change at exactly the same biome value rather than near it.
	int sand_h, sand_a;
	wgdBiomeParams(GEN_SAND_BELOW, &sand_h, &sand_a);
	CHECK(sand_h > lo_h);
	CHECK(sand_h < hi_h);

	// Both out-parameters are optional, and asking for one must not require the other.
	int only_h = -1, only_a = -1;
	wgdBiomeParams(0x0000AE14, &only_h, NULL);
	wgdBiomeParams(0x0000AE14, NULL, &only_a);
	CHECK(only_h > 0);
	CHECK(only_a > 0);
}

// The density field itself (task 15): sign at the extremes, and that a standalone height
// query agrees with the blocks the column fill actually produced.
static void testDensityField(void)
{
	WorldGen g;
	CHECK(worldgenInit(&g, 1337u, GEN_VERSION_DENSITY));

	// The vertical bias must dominate at both ends, or the world has a hollow floor or a
	// stone sky. Checked over a spread of columns, not one, because the biome varies.
	for (int32_t z = -48; z <= 48; z += 16)
		for (int32_t x = -48; x <= 48; x += 16) {
			CHECK_QUIET(wgdDensityAt(&g, x, 0, z) > 0);
			CHECK_QUIET(wgdDensityAt(&g, x, WORLD_HEIGHT - 1, z) < 0);
		}

	// wgdHeight evaluates a 2x2x17 corner sub-grid; wgdColumn evaluates the whole 5x5x17 one
	// and interpolates it. They must agree for every cell of a column, or spawn-finding and
	// the terrain disagree about where the ground is.
	worldInit(&s_world);
	CHECK(worldgenColumn(&g, &s_world, 3, -2));
	int checked = 0;
	for (int lz = 0; lz < CHUNK_DIM; lz++)
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int32_t x = 3 * CHUNK_DIM + lx, z = -2 * CHUNK_DIM + lz;
			const int h = worldgenHeight(&g, x, z);
			CHECK_QUIET(h > 0 && h <= WORLD_HEIGHT);
			// The block below the reported height is a ground material — the height query
			// and the fill loop cannot be pointing at different y values. Wrapped in a
			// predicate rather than written out, because CHECK_QUIET stringifies its
			// argument into a 96-byte buffer and the four-way test overruns it.
			CHECK_QUIET(genTestIsGround(worldGet(&s_world, x, h - 1, z)));
			checked++;
		}
	CHECK(checked == CHUNK_DIM * CHUNK_DIM);
	worldExit(&s_world);
}

// What the terrain actually looks like, asserted as distributions rather than by eye. A
// terrain claim is partly visual and this does not replace looking at it — but every line
// here goes red for a specific way the generator can be wrong: a flat world, a noise sponge
// and a world whose surface pass never ran all fail different ones.
// Width of a biome bucket in the distribution test below, as a shift of the fx biome value.
// 8 gives 256 buckets across the field: narrow enough that the biome contributes under a
// block of height inside one, wide enough that buckets hold hundreds of columns.
#define GEN_D_TEST_BSHIFT  8
#define GEN_D_TEST_BUCKETS ((FX_ONE >> GEN_D_TEST_BSHIFT) + 2)

static void testDensityDistribution(void)
{
	static const uint32_t seeds[3] = {1337u, 4242u, 90210u};

	for (int si = 0; si < 3; si++) {
		WorldGen g;
		CHECK(worldgenInit(&g, seeds[si], GEN_VERSION_DENSITY));
		worldInit(&s_world);

		// 5 x 5 columns, i.e. 80 x 80 blocks. Not 3 x 3: the biome field varies over 128
		// blocks (GEN_BIOME_SHIFT 7), so a 48-block sample can sit entirely inside one biome
		// and show almost no relief — measured, seed 90210 spans 8 blocks over 5 x 5 and
		// would have failed the "not flat" line below over 3 x 3 for a reason that is not a
		// bug in the generator.
		for (int32_t cz = -2; cz <= 2; cz++)
			for (int32_t cx = -2; cx <= 2; cx++)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

		int hmin = WORLD_HEIGHT, hmax = 0;
		long surface_mat = 0, air_under = 0, solid_total = 0;

		// **How much of the relief is the noise's rather than the biome's.**
		// Each column is bucketed by its biome-field value, narrowly enough (256 wide out of
		// FX_ONE) that the biome's own contribution to height inside one bucket is well under
		// a block. If the noise term were deleted, wgdHeight would collapse to a pure function
		// of the biome value and every column in a bucket would share a height; with the noise
		// wired in they differ by roughly its amplitude. Measured over this same 5 x 5 area:
		// 23 / 9 / 8 blocks for seeds 1337 / 4242 / 90210, against 1 / 1 / 1 with the noise
		// term multiplied by zero.
		//
		// The obvious statistical checks were tried first and are not good enough to keep: the
		// widest height range inside one 16 x 16 column scored 7 healthy against 7 noise-
		// deleted on seed 4242, and adjacent 2-block steps came out at zero either way on two
		// of three seeds even over 128 x 128 blocks. Both would have passed a generator whose
		// noise contributed nothing at all.
		static int bk_lo[GEN_D_TEST_BUCKETS], bk_hi[GEN_D_TEST_BUCKETS];
		for (int i = 0; i < GEN_D_TEST_BUCKETS; i++) { bk_lo[i] = WORLD_HEIGHT; bk_hi[i] = -1; }
		long grass = 0, dirt = 0, sand = 0, stone_top = 0, snow_top = 0;
		static int seen[WORLD_HEIGHT + 1];
		memset(seen, 0, sizeof seen);

		for (int32_t cz = -2; cz <= 2; cz++)
			for (int32_t cx = -2; cx <= 2; cx++)
				for (int lz = 0; lz < CHUNK_DIM; lz++)
					for (int lx = 0; lx < CHUNK_DIM; lx++) {
						const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
						int top = -1;
						for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
							const BlockId b = worldGet(&s_world, x, y, z);
							// v1.7.0 tasks 17 and 19. Water and plants are stepped over for
							// the same reason trees are: this scan is looking for the top of
							// the GROUND, and neither an ocean nor a tuft of grass is it.
							// Without them the "top" of a seabed column is the waterline and
							// every surface-material check below reads the wrong cell.
							//
							// v1.8.3 Phase 3 moved the list into genTestNotGround() rather
							// than adding four more ids to a second copy of it; ice, cactus,
							// dead bush and fern step over here for exactly these reasons.
							// BLOCK_AIR is in that predicate too, so the `!= BLOCK_AIR` arm
							// below now only ever sees ground and the `else` only ever sees
							// air — which is what it always meant.
							if (b != BLOCK_AIR && genTestNotGround(b)) continue;
							if (b != BLOCK_AIR) {
								if (top < 0) top = y;
								solid_total++;
							} else if (top >= 0) {
								air_under++;
							}
						}
						CHECK_QUIET(top >= 0);
						if (top < 0) continue;

						const int h = top + 1;
						if (h < hmin) hmin = h;
						if (h > hmax) hmax = h;
						seen[h] = 1;

						const int bk = (int)(worldgenBiome(&g, x, z) >> GEN_D_TEST_BSHIFT);
						if (bk >= 0 && bk < GEN_D_TEST_BUCKETS) {
							if (h < bk_lo[bk]) bk_lo[bk] = h;
							if (h > bk_hi[bk]) bk_hi[bk] = h;
						}

						switch (worldGet(&s_world, x, top, z)) {
							case BLOCK_GRASS: grass++;     surface_mat++; break;
							case BLOCK_DIRT:  dirt++;      surface_mat++; break;
							case BLOCK_SAND:  sand++;      surface_mat++; break;
							case BLOCK_STONE: stone_top++; surface_mat++; break;
							// v1.8.3 Phase 3's tundra cap. Counted separately rather than
							// folded into dirt: the check below is that EVERY surface cell
							// is one the surface pass could have chosen, and a fifth choice
							// that is invisible in the tally cannot be told from a bug that
							// puts an unexpected block on top.
							case BLOCK_SNOW:  snow_top++;  surface_mat++; break;
							default: break;
						}
					}

		int widest_in_biome = 0;
		for (int i = 0; i < GEN_D_TEST_BUCKETS; i++)
			if (bk_hi[i] >= 0 && bk_hi[i] - bk_lo[i] > widest_in_biome)
				widest_in_biome = bk_hi[i] - bk_lo[i];

		int n_distinct = 0;
		for (int h = 0; h <= WORLD_HEIGHT; h++) n_distinct += seen[h];

		// **Surface materials belong to the topmost solid run and nowhere else.** Everything
		// deeper than the dirt band is stone, so grass never grows on the floor of a sealed
		// cave. This is the check that goes red if the fill loop's `exposed` flag is removed
		// — which is a change that looks harmless in a diff and produces green fields
		// underground.
		long buried_soil = 0;
		for (int32_t cz = -2; cz <= 2; cz++)
			for (int32_t cx = -2; cx <= 2; cx++)
				for (int lz = 0; lz < CHUNK_DIM; lz++)
					for (int lx = 0; lx < CHUNK_DIM; lx++) {
						const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
						const int top = worldgenHeight(&g, x, z) - 1;
						for (int y = top - GEN_DIRT_DEPTH - 1; y >= 0; y--) {
							const BlockId b = worldGet(&s_world, x, y, z);
							if (b == BLOCK_GRASS || b == BLOCK_DIRT || b == BLOCK_SAND)
								buried_soil++;
						}
					}
		CHECK(buried_soil == 0);

		// Not flat. A generator whose noise term was dropped would produce one height and a
		// spread of 0. Measured over these three seeds at 5 x 5 columns: 25, 16 and 8 blocks
		// of spread, 26, 16 and 9 distinct heights. The bound is set below the smallest of
		// those rather than at it, so that ordinary retuning of the biome amplitudes does not
		// have to move the test — but far enough above zero that a flat world cannot pass.
		CHECK(hmax - hmin >= 5);
		CHECK(n_distinct >= 5);
		// And the relief is the noise's, not the biome's. Bound set at 4 against a measured
		// worst case of 8 and a noise-deleted 1, so retuning the amplitudes need not move the
		// test, but a generator that has stopped adding noise to the density field cannot pass.
		CHECK(widest_in_biome >= 4);

		// Not a sponge. Measured 5.1 - 8.1% air under the surface over 13x13 columns; the
		// bound here is loose because a 3x3 sample is noisier, but a noise-soup field would
		// be tens of percent and a generator with no caves at all would be zero.
		CHECK(air_under > 0);
		CHECK(air_under * 100 < solid_total * 30);

		// Every surface block is one of the four the surface pass can choose. Anything else
		// on top means the pass ran on a block it should not have.
		CHECK(surface_mat == 5 * 5 * CHUNK_DIM * CHUNK_DIM);
		CHECK(grass + dirt + sand + stone_top + snow_top == surface_mat);

		worldExit(&s_world);
	}

	// Grass above the waterline, dry land, and bare stone on cliffs — in one world. Seed
	// 1337 measured 96.6% of its surface above sea level over 13x13 columns.
	{
		WorldGen g;
		CHECK(worldgenInit(&g, 1337u, GEN_VERSION_DENSITY));
		worldInit(&s_world);
		for (int32_t cz = -1; cz <= 1; cz++)
			for (int32_t cx = -1; cx <= 1; cx++)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

		long above_sea = 0, cols = 0, grass_high = 0, cliff_stone = 0;
		for (int32_t cz = -1; cz <= 1; cz++)
			for (int32_t cx = -1; cx <= 1; cx++)
				for (int lz = 0; lz < CHUNK_DIM; lz++)
					for (int lx = 0; lx < CHUNK_DIM; lx++) {
						const int32_t x = cx * CHUNK_DIM + lx, z = cz * CHUNK_DIM + lz;
						const int h = worldgenHeight(&g, x, z);
						cols++;
						if (h > GEN_SEA_LEVEL) above_sea++;
						const BlockId top = worldGet(&s_world, x, h - 1, z);
						if (top == BLOCK_GRASS && h > GEN_SEA_LEVEL + GEN_D_BEACH_ABOVE)
							grass_high++;
						if (top == BLOCK_STONE) cliff_stone++;
					}

		// Most of a mountainous seed is dry land. A table whose base heights sat under sea
		// level would drown the world, and this is the line that says so.
		CHECK(above_sea * 2 > cols);
		// Grass only ever appears above the beach band.
		CHECK(grass_high > 0);
		// Bare stone on top happens — the slope rule is finding cliffs.
		CHECK(cliff_stone > 0);
		worldExit(&s_world);
	}
}

// ── Tasks 17 and 19: the sea-level water fill and the tall-grass scatter ──────────────
//
// Both passes ADD blocks to a density world and neither may take one away, so the checks
// below are built on two independent kinds of evidence:
//
//   * a pinned TERRAIN FINGERPRINT — the FNV-1a of a generated column with water and tall
//     grass mapped back to air. The twelve literals were taken from the tree as it stood
//     BEFORE either pass placed a block, by a probe linking this same worldgen.c, exactly
//     the way testWorldgenLegacyByteIdentity's numbers were taken. If water ever overwrites
//     a stone block or a plant ever eats a leaf, the stripped column has an AIR where that
//     block used to be and the hash moves. This is what makes "never overwrites anything" a
//     measured fact about generated blocks rather than an argument about the order two
//     passes run in.
//   * SHAPE invariants read off a generated world — where the waterline sits, that a body of
//     water is a run reaching that line and nothing detached from it, and what a plant is
//     allowed to stand on.
//
// The two do different jobs. The fingerprint cannot see a water block placed in the wrong
// AIR cell (a flooded cave is all air underneath either way); the invariants cannot see a
// terrain block quietly replaced by one. Neither alone is enough.
static uint32_t genTestHashColumnTerrain(const World* w, int32_t cx, int32_t cz)
{
	uint32_t h = 2166136261u;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int lz = 0; lz < CHUNK_DIM; lz++)
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				uint8_t b = (uint8_t)worldGet(w, cx * CHUNK_DIM + lx, y,
				                             cz * CHUNK_DIM + lz);
				if (b == BLOCK_WATER || b == BLOCK_TALL_GRASS) b = BLOCK_AIR;
				h ^= b;
				h *= 16777619u;
			}
	return h;
}

// The topmost TERRAIN block of one (x, z) — worldgenHeight's answer minus one, read off the
// world instead of recomputed. Water, plants and trees are stepped over for the same reason
// worldgenHeight excludes trees: none of them is the ground.
//
// Read from the world rather than by calling worldgenHeight because on the density generator
// that call evaluates a 2 x 2 x 17 corner lattice — 204 fBm3 per cell — and the checks below
// ask the question for all 6,400 cells of a 5 x 5-column area, twice.
static int genTestTerrainTop(const World* w, int32_t x, int32_t z)
{
	for (int y = WORLD_HEIGHT - 1; y >= 0; y--)
		if (!genTestNotGround(worldGet(w, x, y, z)))
			return y;
	return -1;
}

// ── v1.8.3 Phase 2: what a biome actually looks like on the ground ────────────────────
//
// **The area is the whole test, and it was chosen by measurement.** A biome rule can only be
// caught misbehaving somewhere the biome occurs, and the two areas testWorldgenWaterAndGrass
// already surveys cannot do this job: seed 1337 at (-12,12) contains no tundra at all, and
// seed 90210 at (8,-8) is FOREST 4790 / JUNGLE 1610 and nothing else. Checking a six-way rule
// there would pass with four of the six branches never executed — the same defect Case 7 of
// blocksmith-lesson-sabotage-arm-must-actually-arm.md records, one file over.
//
// So a sweep of 8 seeds x 81 column origins was run and scored on the WORST-represented
// biome, and this is the winner: seed 90210, columns (10,-20), radius 3 (7 x 7 = 49 columns,
// 112 x 112 blocks). Usable surface cells — above the beach band, not cliff stone — per
// biome, measured on this tree:
//
//     TUNDRA 1190   TAIGA 1951   PLAINS 282   FOREST 3762   DESERT 331   JUNGLE 1147
//
// Plains is the thin one at 282, which is why nothing below asks plains for a tree: at 24
// tree draws in 256 over roughly four 8 x 8 cells, the expected number of plains trees here
// is under one, and it measured zero. An assertion that plains grows trees would be an
// assertion about this seed's luck, not about the generator.
static void testWorldgenBiomeSurface(void)
{
	WorldGen g;
	CHECK(worldgenInit(&g, 90210u, GEN_VERSION_DENSITY));
	worldInit(&s_world);
	const int32_t ox = 10, oz = -20, r = 3;
	for (int32_t cz = oz - r; cz <= oz + r; cz++)
		for (int32_t cx = ox - r; cx <= ox + r; cx++)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	long usable[BIOME_COUNT] = {0}, wrong_cap[BIOME_COUNT] = {0};
	long eligible[BIOME_COUNT] = {0}, plants[BIOME_COUNT] = {0};
	long trunks[BIOME_COUNT] = {0}, leaves[BIOME_COUNT] = {0};
	int tmin[BIOME_COUNT], tmax[BIOME_COUNT];
	for (int b = 0; b < BIOME_COUNT; b++) { tmin[b] = 999; tmax[b] = -1; }

	for (int32_t z = (oz - r) * CHUNK_DIM; z < (oz + r + 1) * CHUNK_DIM; z++) {
		for (int32_t x = (ox - r) * CHUNK_DIM; x < (ox + r + 1) * CHUNK_DIM; x++) {
			const BiomeId b = worldgenBiomeAt(&g, x, z);
			for (int y = 0; y < WORLD_HEIGHT; y++)
				if (worldGet(&s_world, x, y, z) == BLOCK_LEAVES) leaves[b]++;

			const int top = genTestTerrainTop(&s_world, x, z);
			if (top < 0 || top + 1 >= WORLD_HEIGHT)
				continue;
			// The two overrides that outrank the biome and are NOT being tested here: the
			// beach band, which is sand whatever the climate says, and the cliff rule, which
			// is bare stone on any slope steep enough. Both predate Phase 2 and both still
			// win — that is the point of excluding them rather than special-casing them.
			if (top + 1 <= GEN_SEA_LEVEL + GEN_D_BEACH_ABOVE)
				continue;
			const BlockId cap = worldGet(&s_world, x, top, z);
			if (cap == BLOCK_STONE)
				continue;

			usable[b]++;
			// Desert caps in sand; tundra caps in SNOW as of v1.8.3 Phase 3, which is what
			// the bare-dirt placeholder here was reserved for — see surfaceBlock() in
			// worldgen_density.c, where the tundra branch's `depth == 0` arm now returns
			// BLOCK_SNOW. The other four cap in grass, taiga included: no design call was
			// ever recorded for a snow-capped taiga, so Phase 3 did not invent one. Cold
			// WATER still freezes in taiga, which is a separate rule about the sea.
			const BlockId want = (b == BIOME_DESERT) ? BLOCK_SAND
			                   : (b == BIOME_TUNDRA) ? BLOCK_SNOW
			                                         : BLOCK_GRASS;
			if (cap != want)
				wrong_cap[b]++;

			const BlockId over = worldGet(&s_world, x, top + 1, z);
			if (cap == BLOCK_GRASS && (over == BLOCK_AIR || over == BLOCK_TALL_GRASS)) {
				eligible[b]++;
				if (over == BLOCK_TALL_GRASS)
					plants[b]++;
			}
			if (over == BLOCK_WOOD) {
				trunks[b]++;
				int n = 0;
				for (int y = top + 1;
				     y < WORLD_HEIGHT && worldGet(&s_world, x, y, z) == BLOCK_WOOD; y++)
					n++;
				if (n < tmin[b]) tmin[b] = n;
				if (n > tmax[b]) tmax[b] = n;
			}
		}
	}
	worldExit(&s_world);

	// ── Caps ──────────────────────────────────────────────────────────────────────────
	// Every biome is present with real ground, and every cell of it caps in the material
	// its biome says. The first half is what stops the second half being vacuous.
	for (int b = 0; b < BIOME_COUNT; b++) {
		CHECK_QUIET(usable[b] > 100);
		CHECK_QUIET(wrong_cap[b] == 0);
	}
	// Named individually as well, because a loop of CHECK_QUIET reports a count and these
	// six are the headline claim of the whole phase.
	CHECK(usable[BIOME_TUNDRA] > 100 && wrong_cap[BIOME_TUNDRA] == 0);
	CHECK(usable[BIOME_TAIGA]  > 100 && wrong_cap[BIOME_TAIGA]  == 0);
	CHECK(usable[BIOME_PLAINS] > 100 && wrong_cap[BIOME_PLAINS] == 0);
	CHECK(usable[BIOME_FOREST] > 100 && wrong_cap[BIOME_FOREST] == 0);
	CHECK(usable[BIOME_DESERT] > 100 && wrong_cap[BIOME_DESERT] == 0);
	CHECK(usable[BIOME_JUNGLE] > 100 && wrong_cap[BIOME_JUNGLE] == 0);

	// ── Tall grass ────────────────────────────────────────────────────────────────────
	// The rate, against the table, with a 0.6x .. 1.5x band. Measured here, per 256 eligible
	// cells: taiga 13.04 (nominal 12), plains 41.76 (40), forest 23.88 (24), jungle 58.09
	// (56) — every one inside 9 % of nominal, so the band is loose enough to survive a
	// different area and tight enough that swapping any two rows of the table reddens it.
	{
		static const int nominal[BIOME_COUNT] = {0, 12, 40, 24, 0, 56};
		for (int b = 0; b < BIOME_COUNT; b++) {
			if (nominal[b] == 0)
				continue;
			CHECK_QUIET(eligible[b] > 200);
			CHECK_QUIET(plants[b] * 2560L >= eligible[b] * (long)nominal[b] * 6L);
			CHECK_QUIET(plants[b] * 2560L <= eligible[b] * (long)nominal[b] * 15L);
		}
		const bool grassy = eligible[BIOME_TAIGA] > 200 && eligible[BIOME_PLAINS] > 200 &&
		                    eligible[BIOME_FOREST] > 200 && eligible[BIOME_JUNGLE] > 200;
		CHECK(grassy);
		// Plains grows MORE tall grass than forest — 40 against 24 — which is the one row of
		// the table that reads like a typo and is not. Asserted so that "fixing" it has to be
		// a deliberate act. Cross-multiplied into locals: there is no integer divide on the
		// ARM11, and the CHECK macro stringifies its condition into a 96-byte buffer.
		const long plains_rate = plants[BIOME_PLAINS] * eligible[BIOME_FOREST];
		const long forest_rate = plants[BIOME_FOREST] * eligible[BIOME_PLAINS];
		CHECK(plains_rate > forest_rate);
	}
	// Nothing grows in tundra or desert. Note what this does NOT prove on its own: neither
	// biome has a grass cap for a plant to stand on, so worldgenScatter's own surface test
	// would reject every cell even if the chance table said 56. The load-bearing assertion
	// for those two is the CAP above; this is the consequence, recorded because a future
	// change to the tundra placeholder (Phase 3) will make it load-bearing.
	CHECK(plants[BIOME_TUNDRA] == 0);
	CHECK(plants[BIOME_DESERT] == 0);

	// ── Trees ─────────────────────────────────────────────────────────────────────────
	// Zero where the table says zero. This one is NOT a consequence of anything else: the
	// tree pass has no surface-material rule of its own, so a tundra tree is exactly what a
	// broken chance lookup would produce.
	CHECK(trunks[BIOME_TUNDRA] == 0);
	CHECK(trunks[BIOME_DESERT] == 0);
	CHECK(trunks[BIOME_TAIGA] > 0);
	CHECK(trunks[BIOME_FOREST] > 0);
	CHECK(trunks[BIOME_JUNGLE] > 0);

	// Forest is denser than taiga per unit of ground — 128 against 64 in the table, measured
	// 31 trunks over 3762 cells against 7 over 1951. Cross-multiplied rather than divided:
	// there is no integer divide on the ARM11 and the ratios are small.
	CHECK(trunks[BIOME_TAIGA] * usable[BIOME_FOREST] <
	      trunks[BIOME_FOREST] * usable[BIOME_TAIGA]);

	// ── Silhouette ────────────────────────────────────────────────────────────────────
	// Taiga and jungle trunks start at 6; everything else may still be as short as 4, and
	// forest measured a 4 here, which is what keeps the first pair from being satisfied by
	// some global change to GEN_TREE_MIN_H.
	CHECK(tmin[BIOME_TAIGA] >= 6 && tmax[BIOME_TAIGA] <= GEN_TREE_MAX_H);
	CHECK(tmin[BIOME_JUNGLE] >= 6 && tmax[BIOME_JUNGLE] <= GEN_TREE_MAX_H);
	CHECK(tmin[BIOME_FOREST] < 6);

	// The narrow taiga canopy, the only Phase 2 shape change with no constant to read back:
	// a radius-1 canopy is two 3 x 3 layers where a radius-2 one is two 5 x 5 layers less
	// corners, so leaves per trunk has to be markedly lower. Measured 35.43 for taiga against
	// 43.61 for forest and 60.60 for jungle. Leaves are attributed to the biome under them
	// rather than to the tree that grew them, so a canopy crossing a border is counted on the
	// wrong side — which can only ever blunt this check, never manufacture it.
	CHECK(leaves[BIOME_TAIGA] * trunks[BIOME_FOREST] <
	      leaves[BIOME_FOREST] * trunks[BIOME_TAIGA]);
}

// ── v1.8.3 Phase 3: where the five new blocks actually land ───────────────────────────
//
// The registry rows are proved in world/registry_test.c; this is the other half — that
// worldgen puts each of them where the design says and nowhere else. Every claim below is a
// "== 0" over the whole area paired with a "> 0" that stops it being satisfied by an empty
// world, because a placement rule that never fires satisfies every negative ever written
// about it.
//
// **The area was chosen by measurement, and the table is the reason it is r5 and not r3.**
// Seed 90210 around (10, -20) is already this file's six-biome area (see
// testWorldgenBiomeSurface above for why that seed and that origin). Surveyed on this tree by
// a probe linking the real worldgen.c, cells of each block over the whole area:
//
//     r3 (7x7 columns)    snow 1190  ice  44  cactus 10  dead_bush 35  fern 255
//     r5 (11x11 columns)  snow 4596  ice 280  cactus 16  dead_bush 54  fern 540
//
// Cactus is the thin one either way — GEN_CACTUS_CHANCE is 3 in 256 of the desert cells that
// also have sand directly under them — and ten cells of it is close enough to zero that a
// modest retune of that constant would make the "> 0" below a claim about luck. r5 costs 121
// generated columns against 49 and buys 16. The counts are deterministic for a fixed seed, so
// none of this varies run to run; what it buys is headroom against the constants moving.
//
// Biomes in the r5 area, from the same probe:
//     TUNDRA 4596  TAIGA 2297  PLAINS 1458  FOREST 6546  DESERT 6989  JUNGLE 9090
static void testWorldgenPhase3Flora(void)
{
	const uint32_t seed = 90210u;
	const int32_t  ox = 10, oz = -20;
	const int      r  = 5;

	WorldGen g;
	CHECK(worldgenInit(&g, seed, GEN_VERSION_DENSITY));
	worldInit(&s_world);
	for (int32_t cz = oz - r; cz <= oz + r; cz++)
		for (int32_t cx = ox - r; cx <= ox + r; cx++)
			CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

	long snow = 0, snow_wrong_biome = 0, snow_not_cap = 0;
	long ice  = 0, ice_wrong_biome  = 0, ice_off_line = 0;
	long cactus_cells = 0, cactus_bases = 0, cactus_wrong_biome = 0;
	long cactus_not_on_sand = 0, cactus_bad_height = 0;
	long bush = 0, bush_wrong_biome = 0, bush_not_on_sand = 0, bush_off_ground = 0;
	long fern = 0, fern_wrong_biome = 0, fern_not_on_grass = 0, fern_off_ground = 0;
	long flora_at_or_below_sea = 0, flora_stacked = 0;
	long fern_taiga = 0, fern_jungle = 0, grass_cap_taiga = 0, grass_cap_jungle = 0;

	for (int32_t z = (oz - r) * CHUNK_DIM; z < (oz + r + 1) * CHUNK_DIM; z++) {
		for (int32_t x = (ox - r) * CHUNK_DIM; x < (ox + r + 1) * CHUNK_DIM; x++) {
			const BiomeId b   = worldgenBiomeAt(&g, x, z);
			const int     top = genTestTerrainTop(&s_world, x, z);
			int plants_here = 0;

			for (int y = 0; y < WORLD_HEIGHT; y++) {
				const BlockId here  = worldGet(&s_world, x, y, z);
				const BlockId under = (y > 0) ? worldGet(&s_world, x, y - 1, z) : BLOCK_AIR;

				switch (here) {
					case BLOCK_SNOW:
						// Snow is a CAP, so it is the topmost ground cell and never buried.
						// Snow under dirt would mean surfaceBlock() answered it for a depth
						// other than 0.
						snow++;
						if (b != BIOME_TUNDRA) snow_wrong_biome++;
						if (y != top)          snow_not_cap++;
						break;

					case BLOCK_ICE:
						ice++;
						if (b != BIOME_TUNDRA && b != BIOME_TAIGA) ice_wrong_biome++;
						if (y != GEN_SEA_LEVEL - 1)                ice_off_line++;
						break;

					case BLOCK_CACTUS:
						cactus_cells++;
						if (b != BIOME_DESERT)  cactus_wrong_biome++;
						if (y <= GEN_SEA_LEVEL) flora_at_or_below_sea++;
						if (under != BLOCK_CACTUS) {
							// The base of a stack. Its own rules, and the height of the
							// stack above it, are read here rather than once per cell.
							cactus_bases++;
							if (under != BLOCK_SAND) cactus_not_on_sand++;
							if (y != top + 1)        cactus_not_on_sand++;
							int n = 0;
							for (int yy = y;
							     yy < WORLD_HEIGHT &&
							     worldGet(&s_world, x, yy, z) == BLOCK_CACTUS; yy++)
								n++;
							if (n < GEN_CACTUS_MIN_H || n > GEN_CACTUS_MAX_H)
								cactus_bad_height++;
						}
						break;

					case BLOCK_DEAD_BUSH:
						bush++;
						plants_here++;
						if (b != BIOME_DESERT)   bush_wrong_biome++;
						if (under != BLOCK_SAND) bush_not_on_sand++;
						if (y != top + 1)        bush_off_ground++;
						if (y <= GEN_SEA_LEVEL)  flora_at_or_below_sea++;
						break;

					case BLOCK_FERN:
						fern++;
						plants_here++;
						if (b != BIOME_TAIGA && b != BIOME_JUNGLE) fern_wrong_biome++;
						if (under != BLOCK_GRASS) fern_not_on_grass++;
						if (y != top + 1)         fern_off_ground++;
						if (y <= GEN_SEA_LEVEL)   flora_at_or_below_sea++;
						if (b == BIOME_TAIGA)  fern_taiga++;
						if (b == BIOME_JUNGLE) fern_jungle++;
						break;

					default: break;
				}
			}
			if (plants_here > 1) flora_stacked++;

			// Ground a fern could have stood on, per biome. This is the denominator the rate
			// comparison at the bottom needs; without it that comparison is between two raw
			// counts and says more about how much taiga the area holds than about the table.
			if (top >= 0 && top + 1 > GEN_SEA_LEVEL && top + 1 < WORLD_HEIGHT &&
			    worldGet(&s_world, x, top, z) == BLOCK_GRASS) {
				if (b == BIOME_TAIGA)  grass_cap_taiga++;
				if (b == BIOME_JUNGLE) grass_cap_jungle++;
			}
		}
	}
	worldExit(&s_world);

	// ── The five are where they belong, and nowhere else. ─────────────────────────────
	CHECK(snow_wrong_biome   == 0);
	CHECK(snow_not_cap       == 0);
	CHECK(ice_wrong_biome    == 0);
	CHECK(ice_off_line       == 0);
	CHECK(cactus_wrong_biome == 0);
	CHECK(cactus_not_on_sand == 0);
	CHECK(cactus_bad_height  == 0);
	CHECK(bush_wrong_biome   == 0);
	CHECK(bush_not_on_sand   == 0);
	CHECK(bush_off_ground    == 0);
	CHECK(fern_wrong_biome   == 0);
	CHECK(fern_not_on_grass  == 0);
	CHECK(fern_off_ground    == 0);
	CHECK(flora_at_or_below_sea == 0);
	CHECK(flora_stacked         == 0);

	// ── And every one of them exists, so none of the above is vacuous. ────────────────
	// The bounds are "> 0" rather than the measured numbers so that retuning a chance constant
	// need not move this test — but a placement branch that stopped running altogether cannot
	// pass, and that is the failure this whole function is about.
	CHECK(snow > 0);
	CHECK(ice  > 0);
	CHECK(cactus_cells > 0);
	CHECK(cactus_bases > 0);
	CHECK(bush > 0);
	CHECK(fern > 0);
	// Cacti are stacks, not single blocks: cells must outnumber bases, or nothing in the area
	// reached GEN_CACTUS_MAX_H and the height check above is testing one constant.
	CHECK(cactus_cells > cactus_bases);

	// ── The table is the right way round. ─────────────────────────────────────────────
	// GEN_FERN_JUNGLE is 32 in 256 against GEN_FERN_TAIGA's 20, so jungle must come out denser
	// per cell of eligible ground. Cross-multiplied rather than divided: this is integer C and
	// a ratio would round both sides to 0. Swapping the two rows of the table reddens it, and
	// so does a flora pass that ignores the biome and uses one chance for both.
	CHECK(grass_cap_taiga > 0 && grass_cap_jungle > 0);
	CHECK(fern_jungle * grass_cap_taiga > fern_taiga * grass_cap_jungle);

	// ── None of the five may reach a LEGACY world. ────────────────────────────────────
	// worldgenScatter() — where worldgenFlora() is called from — is reached only from
	// worldgenColumn's GEN_VERSION_DENSITY arm, and surfaceBlock()/seaBlockAt() live in
	// worldgen_density.c, which the legacy path never enters. That is the argument; this is
	// the measurement. Same shape as testWorldgenWaterAndGrass's legacy sweep for water and
	// tall grass, and it exists for the same reason: a saved legacy world must stay
	// byte-identical to what the pre-v1.7.0 generator produced, and five new block ids
	// appearing in one would change every world on every SD card.
	{
		static const uint32_t legacy_seeds[2] = {1337u, 90210u};
		for (int li = 0; li < 2; li++) {
			WorldGen lg;
			CHECK(worldgenInit(&lg, legacy_seeds[li], GEN_VERSION_LEGACY));
			worldInit(&s_world);
			for (int32_t cz = -1; cz <= 1; cz++)
				for (int32_t cx = -1; cx <= 1; cx++)
					CHECK_QUIET(worldgenColumn(&lg, &s_world, cx, cz));

			long intruders = 0, cells = 0;
			for (int32_t z = -CHUNK_DIM; z < 2 * CHUNK_DIM; z++)
				for (int32_t x = -CHUNK_DIM; x < 2 * CHUNK_DIM; x++)
					for (int y = 0; y < WORLD_HEIGHT; y++) {
						const BlockId lb = worldGet(&s_world, x, y, z);
						cells++;
						if (lb == BLOCK_SNOW || lb == BLOCK_ICE || lb == BLOCK_CACTUS ||
						    lb == BLOCK_DEAD_BUSH || lb == BLOCK_FERN)
							intruders++;
					}
			CHECK(intruders == 0);
			// The sweep ran over a real world rather than over nothing.
			CHECK(cells == 3L * CHUNK_DIM * 3L * CHUNK_DIM * WORLD_HEIGHT);
			worldExit(&s_world);
		}
	}
}

static void testWorldgenWaterAndGrass(void)
{
	// ── The terrain fingerprint. ──────────────────────────────────────────────────────
	//
	// **These twelve are the DENSITY generator's fingerprints, and v1.8.3 Phase 2 moved seven
	// of them on purpose.** They are not the legacy-identity pins — those are
	// testWorldgenLegacyByteIdentity()'s, they use genTestHashColumn() rather than
	// genTestHashColumnTerrain(), and they did NOT move: a legacy world is byte-for-byte what
	// it was, which is the hard constraint of the whole generator-version scheme.
	//
	// A density world is a different matter: biomes ARE a change to it, and the design says
	// so. What moved was measured cell by cell over an 81-column ring of seed 1337, this
	// build against the pre-change sources — 7,588 differing cells of 2,654,208 (0.2859 %):
	//
	//     above the surface  5088   tall grass and tree shape (the decorate/scatter pass)
	//     at the surface     2498   every one of them GRASS -> DIRT, the tundra cap
	//     below the surface     2   leaves of a canopy overhanging a cliff; the ground
	//                               under them is STONE in both arms and did not move
	//
	// So no cell of ground moved: no height, no density, no cave. Nothing a player has built
	// on can be left floating or buried by this. And of the cells that did move, ZERO were at
	// or below the surface of a MILD/DRY column — plains keeps the old cap and the old tree
	// chance and only gains tall grass, which stands above the ground by definition.
	//
	// source/debug/loadprof_test.c pins a whole-world content hash of the same generator
	// (GEN_VERSION_NEWEST == GEN_VERSION_DENSITY, genversion.h:75) and moves for the same
	// reason; its own comment says it is supposed to.
	// ── v1.8.3 Phase 3 moved FOUR of the twelve, and only four. ───────────────────────
	//
	// Told apart from breakage by measurement, not by the fact that the suite went green
	// again. An A/B probe built the SAME dump program twice — once against faeaa63 and once
	// against this tree — and diffed all 32,768 cells of each column. Every differing cell in
	// all twelve columns took one of exactly three transitions:
	//
	//     seed 1337  ( 0,  0)  26 cells  air -> fern                    0x4514ba79 -> 0x5d83eac5
	//     seed 1337  ( 1,  0)  18 cells  air -> fern                    0x849f4a0b -> 0xec6b57df
	//     seed 1337  (-1, -1)  21 cells  air -> fern (20), water -> ice (1)
	//                                                                   0x690f990d -> 0x12fc3ace
	//     seed 90210 ( 7, -3)  31 cells  dirt -> snow                   0x8794b8b1 -> 0x30e49729
	//     the other eight columns        IDENTICAL, 32,768 of 32,768 cells
	//
	// That shape is the whole argument. air -> fern is the scatter pass putting a plant in
	// empty space above the ground. dirt -> snow and water -> ice are surface-cap swaps at a
	// cell that was already occupied. NO cell of ground moved: no height, no density, no cave,
	// nothing under a surface. Nothing a player has built on can be left floating or buried.
	//
	// And it is the DENSITY generator's output that moved, which is the one that is allowed
	// to. worldgenScatter() — and so the new worldgenFlora() inside it — is only reached from
	// worldgenColumn's GEN_VERSION_DENSITY arm, and surfaceBlock()/seaBlockAt() live in
	// worldgen_density.c, which the legacy path never enters. testWorldgenLegacyByteIdentity's
	// twelve GEN_VERSION_LEGACY hashes did NOT move, this run or any run of this work, and
	// they are the check that would have caught it if they had.
	//
	// Eight of twelve unchanged is itself evidence rather than luck: the four that moved are
	// exactly the four the cell diff found cells in, and 4242 has no cold, no desert and no
	// taiga/jungle undergrowth in any of its four sampled columns.
	static const struct { uint32_t seed; int32_t cx, cz; uint32_t hash; } pinned[] = {
		{1337u,   0,  0, 0x5d83eac5u}, {1337u,   1,  0, 0xec6b57dfu},
		{1337u,  -1, -1, 0x12fc3aceu}, {1337u,   7, -3, 0x479464fdu},
		{4242u,   0,  0, 0x40c702cbu}, {4242u,   1,  0, 0xa038a451u},
		{4242u,  -1, -1, 0xe4b4d7c1u}, {4242u,   7, -3, 0x7fb8f23fu},
		{90210u,  0,  0, 0x8bb38a6eu}, {90210u,  1,  0, 0xea29565au},
		{90210u, -1, -1, 0xdd9c1f34u}, {90210u,  7, -3, 0x30e49729u},
	};
	for (size_t i = 0; i < sizeof(pinned) / sizeof(pinned[0]); i++) {
		WorldGen g;
		CHECK(worldgenInit(&g, pinned[i].seed, GEN_VERSION_DENSITY));
		worldInit(&s_world);
		CHECK(worldgenColumn(&g, &s_world, pinned[i].cx, pinned[i].cz));
		CHECK_QUIET(genTestHashColumnTerrain(&s_world, pinned[i].cx, pinned[i].cz)
		            == pinned[i].hash);
		worldExit(&s_world);
	}

	// ── Neither block may reach a LEGACY world. ───────────────────────────────────────
	// testWorldgenLegacyByteIdentity's twelve hashes already say this, but they say it as
	// "something changed"; this says which thing, on the two ids that would be changing it.
	//
	// **Both seeds, and this is not belt-and-braces.** Run on 90210 alone, `legacy_plants == 0`
	// was UNFALSIFIABLE. Rerouting the legacy path through the density generator — an arm that
	// plants grass in a legacy world by construction — reddened 11,650 checks including
	// `legacy_water == 0` and all twelve byte-identity hashes, and left `legacy_plants == 0`
	// GREEN: 90210 around the origin is nearly all ocean, so over 3 x 3 columns there was no
	// grass-topped ground above the waterline for a plant to stand on. Exactly the trap this
	// same function documents for its determinism check, one assertion further down. 1337 is
	// the land seed (12,224 eligible cells and 1,166 plants over 7 x 7 columns at the origin),
	// so it is the one that can actually fail, and `land_eligible > 0` below is what stops that
	// claim being a comment instead of a check.
	static const uint32_t legacy_seeds[2] = { 1337u, 90210u };
	for (int li = 0; li < 2; li++) {
		WorldGen g;
		CHECK(worldgenInit(&g, legacy_seeds[li], GEN_VERSION_LEGACY));
		worldInit(&s_world);
		for (int32_t cz = -1; cz <= 1; cz++)
			for (int32_t cx = -1; cx <= 1; cx++)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));
		long legacy_water = 0, legacy_plants = 0, land_eligible = 0;
		for (int32_t z = -CHUNK_DIM; z < 2 * CHUNK_DIM; z++)
			for (int32_t x = -CHUNK_DIM; x < 2 * CHUNK_DIM; x++) {
				for (int y = 0; y < WORLD_HEIGHT; y++) {
					const BlockId b = worldGet(&s_world, x, y, z);
					if (b == BLOCK_WATER)      legacy_water++;
					if (b == BLOCK_TALL_GRASS) legacy_plants++;
				}
				// Ground a plant COULD have stood on, had the scatter reached this world:
				// grass on top, above the waterline, air over it. Without this the two
				// zeroes above are satisfied by a region with nowhere to put a plant.
				const int top = genTestTerrainTop(&s_world, x, z);
				if (top >= 0 && top + 1 > GEN_SEA_LEVEL && top + 1 < WORLD_HEIGHT &&
				    worldGet(&s_world, x, top, z) == BLOCK_GRASS &&
				    worldGet(&s_world, x, top + 1, z) == BLOCK_AIR)
					land_eligible++;
			}
		CHECK(legacy_water == 0);
		CHECK(legacy_plants == 0);
		if (legacy_seeds[li] == 1337u)
			CHECK(land_eligible > 0);
		worldExit(&s_world);
	}

	// ── Shape, over two 5 x 5-column areas, each with a real coastline in it. ─────────
	//
	// **The two areas were chosen by measurement, not by taking the origin twice.** Over 5 x 5
	// columns at (0, 0) seed 1337 has 0 ocean columns and seed 90210 has 0 columns of grass
	// above the waterline — the origin gives an area that is all land on one seed and all sea
	// on the other, so half of what is asserted below would be vacuously true on each.
	// Surveyed over five seeds and three areas, these two hold both at once: 1337 around
	// (-12, 12) is 2,669 ocean columns against 2,942 grass-topped ones with 18 trees standing
	// on them, and 90210 around (8, -8) is 838 against 3,157 with 19 trees.
	static const struct { uint32_t seed; int32_t cx, cz; } areas[2] = {
		{1337u, -12, 12}, {90210u, 8, -8},
	};
	long total_water = 0, total_plants = 0, total_ice = 0;

	for (int si = 0; si < 2; si++) {
		const int32_t ox = areas[si].cx, oz = areas[si].cz;
		WorldGen g;
		CHECK(worldgenInit(&g, areas[si].seed, GEN_VERSION_DENSITY));
		worldInit(&s_world);
		for (int32_t cz = oz - 2; cz <= oz + 2; cz++)
			for (int32_t cx = ox - 2; cx <= ox + 2; cx++)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));

		long water = 0, plants = 0, eligible = 0, sealed_air = 0, trunk_bases = 0;
		long water_above_line = 0, unfilled_ocean = 0, detached_water = 0;
		// v1.8.3 Phase 3's sea ice. `ice` is the non-vacuity counter for the three that
		// follow it; without it "no ice off the waterline" is satisfied by a world with no
		// ice in it, which is exactly the shape of failure this file keeps recording.
		long ice = 0, ice_off_line = 0, ice_wrong_biome = 0, ice_no_water_under = 0;
		long plant_below_line = 0, plant_off_ground = 0, plant_not_on_grass = 0;
		long plant_stacked = 0;

		for (int32_t z = (oz - 2) * CHUNK_DIM; z < (oz + 3) * CHUNK_DIM; z++) {
			for (int32_t x = (ox - 2) * CHUNK_DIM; x < (ox + 3) * CHUNK_DIM; x++) {
				const int top = genTestTerrainTop(&s_world, x, z);

				// **The waterline.** Walking down from GEN_SEA_LEVEL - 1, a column is water
				// until the first cell that is not, and never water again below that. That
				// one sweep is three separate claims at once: the surface sits at
				// GEN_SEA_LEVEL and not a block either side of it, a body of water is
				// connected to that surface, and anything sealed under rock — a cave, an
				// overhang pocket — is left dry.
				bool still_water = true;
				bool hit_solid   = false;
				for (int y = GEN_SEA_LEVEL - 1; y >= 0; y--) {
					const BlockId b = worldGet(&s_world, x, y, z);
					// v1.8.3 Phase 3: the topmost cell of a cold body is ICE, not water, so
					// it is the SURFACE of the body and not a break in it. Treating it as a
					// break was the first symptom this change produced — every water cell in
					// a frozen column counted as detached, four checks red — and "widen the
					// sweep" is the right answer only because ice is placed by
					// seaBlockAt() in the same walk that places the water, at exactly the
					// one y this arm accepts. Anything else is still a break.
					if (b == BLOCK_ICE && y == GEN_SEA_LEVEL - 1 && still_water)
						continue;
					if (b == BLOCK_WATER) {
						water++;
						if (!still_water) detached_water++;
					} else {
						still_water = false;
						// Air below the first solid of the sweep is exactly a pocket the
						// fill must not have reached.
						if (b == BLOCK_AIR) { if (hit_solid) sealed_air++; }
						else                { hit_solid = true; }
					}
				}

				// Nothing above the line, ever.
				for (int y = GEN_SEA_LEVEL; y < WORLD_HEIGHT; y++)
					if (worldGet(&s_world, x, y, z) == BLOCK_WATER) water_above_line++;

				// ── Sea ice (v1.8.3 Phase 3). ─────────────────────────────────────────
				// Every ice cell in the column, wherever it is, so "only at the waterline"
				// is a claim this loop can actually falsify rather than one the loop bound
				// makes true. The biome and the cell underneath are checked at the same
				// time: seaBlockAt() answers BLOCK_ICE only for TUNDRA and TAIGA and only
				// for the top cell of a filled column, so ice anywhere else, ice in a warm
				// biome, or ice with nothing but air under it are three different bugs.
				{
					const BiomeId ib = worldgenBiomeAt(&g, x, z);
					for (int y = 0; y < WORLD_HEIGHT; y++) {
						if (worldGet(&s_world, x, y, z) != BLOCK_ICE) continue;
						ice++;
						if (y != GEN_SEA_LEVEL - 1)                    ice_off_line++;
						if (ib != BIOME_TUNDRA && ib != BIOME_TAIGA)   ice_wrong_biome++;
						if (y > 0 && worldGet(&s_world, x, y - 1, z) == BLOCK_AIR)
							ice_no_water_under++;
					}
				}

				// An ocean column — one whose ground never reaches the line — must be full
				// to the brim. This is the half that catches a fill stopping a block short,
				// which the sweep above would happily call consistent.
				//
				// ICE COUNTS AS FULL, and leaving it out would have been worse than a red
				// check: genTestTerrainTop() now steps over ice, so a frozen ocean column
				// reports its seabed as `top` and DOES enter this branch — where before
				// Phase 3 it reported the ice itself, `top < GEN_SEA_LEVEL - 1` was false,
				// and the column was skipped entirely. The check went vacuous for exactly
				// the columns Phase 3 added, silently, and this arm is what puts it back.
				{
					const BlockId brim = worldGet(&s_world, x, GEN_SEA_LEVEL - 1, z);
					if (top >= 0 && top < GEN_SEA_LEVEL - 1 &&
					    brim != BLOCK_WATER && brim != BLOCK_ICE)
						unfilled_ocean++;
				}

				// ── Plants. ───────────────────────────────────────────────────────────
				int seen_plant = 0;
				for (int y = 0; y < WORLD_HEIGHT; y++) {
					if (worldGet(&s_world, x, y, z) != BLOCK_TALL_GRASS) continue;
					plants++;
					seen_plant++;
					if (y <= GEN_SEA_LEVEL)                             plant_below_line++;
					if (y != top + 1)                                   plant_off_ground++;
					if (worldGet(&s_world, x, y - 1, z) != BLOCK_GRASS) plant_not_on_grass++;
				}
				if (seen_plant > 1) plant_stacked++;

				// Where a plant COULD have gone: grass on top, above the line, room above.
				if (top >= 0 && top + 1 > GEN_SEA_LEVEL && top + 1 < WORLD_HEIGHT &&
				    worldGet(&s_world, x, top, z) == BLOCK_GRASS) {
					const BlockId over = worldGet(&s_world, x, top + 1, z);
					if (over == BLOCK_AIR || over == BLOCK_TALL_GRASS) eligible++;
					if (over == BLOCK_WOOD) trunk_bases++;
				}
			}
		}

		CHECK(water_above_line == 0);
		CHECK(unfilled_ocean == 0);
		CHECK(detached_water == 0);
		CHECK(ice_off_line == 0);
		CHECK(ice_wrong_biome == 0);
		CHECK(ice_no_water_under == 0);
		CHECK(plant_below_line == 0);
		CHECK(plant_off_ground == 0);
		CHECK(plant_not_on_grass == 0);
		CHECK(plant_stacked == 0);

		// The negatives above are only worth having if the world contains the things they
		// are about. A world with no caves under the sea, no tree standing where a plant
		// could have gone and no eligible ground satisfies every one of them by being empty.
		CHECK(sealed_air > 0);
		CHECK(eligible > 0);
		CHECK(trunk_bases > 0);
		CHECK(plants > 0);

		// Scattered, not a lawn, and not a rumour. GEN_GRASS_CHANCE is out of 256 and this
		// band sits well either side of it, so retuning the number by a few need not move
		// the test — while a lawn (every eligible cell) and a silent no-op both fail.
		CHECK(plants * 100 >= eligible * 4);
		CHECK(plants * 100 <= eligible * 20);

		total_water  += water;
		total_plants += plants;
		total_ice    += ice;
		worldExit(&s_world);
	}

	// The ocean has to exist at all. Summed across the two seeds rather than asserted per
	// seed: how much water a given seed has is a property of its biome field, not of the fill.
	CHECK(total_water > 0);
	CHECK(total_plants > 0);
	// And the three ice checks above have to have had something to look at. Summed for the
	// same reason and a stronger one: whether a given area is cold enough to freeze is a
	// property of its biome field, and area 1337/(-12,12) is documented above as containing
	// no tundra at all. Asserting per-seed would be asserting where the climate noise put
	// taiga, which is not what this test is about.
	CHECK(total_ice > 0);

	// ── Regenerating the same seed gives the identical world, water and plants included. ──
	// Forwards and backwards over the same 3 x 3 columns, hashing every block: both passes
	// have to be pure functions of the seed and the coordinate, exactly as the terrain is.
	//
	// **Two seeds, and the counts, because one seed made this vacuous.** Run on 90210 alone
	// it passed with a deliberately order-dependent plant salt (`salt ^= ++call_counter`)
	// still in the module: 90210 around the origin is nearly all ocean — 307 grass-eligible
	// cells and 25 plants over 7 x 7 columns — so over 3 x 3 there was no plant to move and
	// nothing for the hash to see. 1337 at the origin is the land case (12,224 eligible,
	// 1,166 plants over the same 7 x 7). Each seed asserts that the thing it is meant to be
	// proving determinstic is actually present in the hashed region.
	static const struct { uint32_t seed; bool want_plants; bool want_water; } det[2] = {
		{1337u, true, false}, {90210u, false, true},
	};
	for (int di = 0; di < 2; di++) {
		WorldGen g;
		CHECK(worldgenInit(&g, det[di].seed, GEN_VERSION_DENSITY));

		worldInit(&s_world);
		for (int cz = 0; cz < GEN_TEST_COLS; cz++)
			for (int cx = 0; cx < GEN_TEST_COLS; cx++)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));
		const uint32_t forward = genTestHash();

		// Non-vacuity: the hashed region has to contain what this seed is here for.
		long seen_water = 0, seen_plants = 0;
		for (int32_t z = 0; z < GEN_TEST_COLS * CHUNK_DIM; z++)
			for (int32_t x = 0; x < GEN_TEST_COLS * CHUNK_DIM; x++)
				for (int y = 0; y < WORLD_HEIGHT; y++) {
					const BlockId b = worldGet(&s_world, x, y, z);
					if (b == BLOCK_WATER)      seen_water++;
					if (b == BLOCK_TALL_GRASS) seen_plants++;
				}
		if (det[di].want_plants) CHECK(seen_plants > 0);
		if (det[di].want_water)  CHECK(seen_water > 0);
		worldExit(&s_world);

		worldInit(&s_world);
		for (int cz = GEN_TEST_COLS - 1; cz >= 0; cz--)
			for (int cx = GEN_TEST_COLS - 1; cx >= 0; cx--)
				CHECK_QUIET(worldgenColumn(&g, &s_world, cx, cz));
		CHECK(genTestHash() == forward);

		// And the hash can see one block move, or the line above proves nothing.
		CHECK(worldSet(&s_world, 20, GEN_SEA_LEVEL - 2, 20, BLOCK_SAND));
		CHECK(genTestHash() != forward);
		worldExit(&s_world);
	}
}

#ifndef __3DS__

#include <dirent.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>    // _rmdir
#include <process.h>   // _getpid — MinGW keeps it here, not in <unistd.h>
#else
#include <unistd.h>    // rmdir, getpid
#endif

#include "world/region.h"

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Only the
// host tests create directories — the game's save path is made by app/save.c with the
// libctru devoptab, which is POSIX — so this wrapper stays here rather than in the game.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

static void testRmdir(const char* path)
{
#if defined(_WIN32)
	_rmdir(path);
#else
	rmdir(path);
#endif
}

// The directory the region tests below write into. Derived from the pid rather than being
// a fixed literal, because this suite does get run concurrently — two shells in the same
// checkout, or a second session building the same tree — and a shared path means one run
// truncates and rewrites the very region files another run is part-way through asserting
// on. That does not present as a concurrency bug when it bites: it presents as unrelated
// assertions failing at random line numbers on a suite that passes when re-run, which is
// exactly how it showed up, and it cost a full A/B against an innocent change to rule out.
static const char* testWorldDir(void)
{
	static char dir[64];

	if (dir[0] == '\0') {
#if defined(_WIN32)
		const long pid = (long)_getpid();
#else
		const long pid = (long)getpid();
#endif
		snprintf(dir, sizeof(dir), "build-host/testworld-%ld", pid);
	}
	return dir;
}

// Deletes the per-process directory and everything under it, so a run leaves no more
// behind than the fixed-path version did. Recursive because testRegionTornDirectory makes
// a "cutworld" subdirectory: remove() takes files and empty directories on both platforms,
// and the only thing it refuses is a non-empty directory, so recursing on failure and
// letting the tail rmdir finish the job covers every case. Best-effort throughout — a
// leftover file is untidy, not a test failure.
static void testRmTree(const char* path)
{
	DIR* d = opendir(path);
	if (!d) return;

	for (const struct dirent* e = readdir(d); e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

		char child[512];
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (remove(child) != 0) testRmTree(child);
	}

	closedir(d);
	testRmdir(path);
}

static void regionTestDir(void)
{
	testMkdir("build-host");
	testMkdir(testWorldDir());
}

static void regionTestClean(int32_t rx, int32_t rz)
{
	char p[256];
	snprintf(p, sizeof(p), "%s/r.%ld.%ld.bsr", testWorldDir(), (long)rx, (long)rz);
	remove(p);
	snprintf(p, sizeof(p), "%s/r.%ld.%ld.tmp", testWorldDir(), (long)rx, (long)rz);
	remove(p);
}

// Builds a recognisable column at (cx, cz): terrain in the low chunks and one marker block
// whose id depends on `tag`, so a loaded world can be told apart from a stale one.
static void regionTestBuild(World* w, int32_t cx, int32_t cz, BlockId tag)
{
	for (int y = 0; y < 40; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				worldSet(w, cx * CHUNK_DIM + x, y, cz * CHUNK_DIM + z,
				         y < 32 ? BLOCK_STONE : BLOCK_DIRT);

	worldSet(w, cx * CHUNK_DIM + 3, 41, cz * CHUNK_DIM + 5, tag);
}

static void testRegionRoundTrip(void)
{
	static uint8_t buf[REGION_COL_MAX];

	regionTestDir();
	regionTestClean(0, 0);

	worldInit(&s_world);
	regionTestBuild(&s_world, 2, 3, BLOCK_WOOD);

	const Column* col = worldColumn(&s_world, 2, 3);
	CHECK(col != NULL);

	const uint32_t n = regionEncodeColumn(col, buf, sizeof(buf));
	CHECK(n > 0);
	CHECK(regionWriteColumn(testWorldDir(), 2, 3, buf, n));
	worldExit(&s_world);

	// Read it back into a world that has never seen it.
	static uint8_t back[REGION_COL_MAX];
	const uint32_t got = regionReadColumn(testWorldDir(), 2, 3, back, sizeof(back));
	CHECK(got == n);
	CHECK(memcmp(buf, back, n) == 0);

	worldInit(&s_world);
	CHECK(regionDecodeColumn(&s_world, 2, 3, back, got));
	CHECK(worldGet(&s_world, 2 * CHUNK_DIM + 3, 41, 3 * CHUNK_DIM + 5) == BLOCK_WOOD);
	CHECK(worldGet(&s_world, 2 * CHUNK_DIM + 0, 10, 3 * CHUNK_DIM + 0) == BLOCK_STONE);
	CHECK(worldGet(&s_world, 2 * CHUNK_DIM + 0, 35, 3 * CHUNK_DIM + 0) == BLOCK_DIRT);
	worldExit(&s_world);

	// A column that was never saved reads as 0, not as an error and not as somebody else's
	// bytes. Slot arithmetic that masked wrongly would show up right here.
	CHECK(regionReadColumn(testWorldDir(), 5, 7, back, sizeof(back)) == 0);

	// Negative coordinates land in their own region and their own slot. -1 and 15 both mask
	// to slot 15, so they must differ by region file or one would overwrite the other.
	CHECK(regionOf(-1) == -1);
	CHECK(regionOf(-16) == -1);
	CHECK(regionOf(-17) == -2);

	regionTestClean(0, 0);
}

// Copies `src` to `dst`, keeping only the first `keep` bytes. Standing in for the console
// losing power part-way through a write.
static bool truncCopy(const char* src, const char* dst, long keep)
{
	FILE* a = fopen(src, "rb");
	if (!a) return false;
	FILE* b = fopen(dst, "wb");
	if (!b) { fclose(a); return false; }

	for (long i = 0; i < keep; i++) {
		const int ch = fgetc(a);
		if (ch == EOF) break;
		fputc(ch, b);
	}
	fclose(a);
	fclose(b);
	return true;
}

static long fileBytes(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	return n;
}

// **The step 8.1 criterion.** Save a column, save a different version of it, then cut the
// file at every byte offset and load it. Every single cut must produce one of exactly three
// outcomes: the old column, the new column, or nothing at all. Never a mixture, never a
// decode of garbage, never a crash.
//
// That is what the double-buffered directory and the append-only arena are for, and it is
// the only way to check them — an emulator run cannot pull the power at byte 4,073.
static void testRegionPowerCut(void)
{
	static uint8_t old_bytes[REGION_COL_MAX];
	static uint8_t new_bytes[REGION_COL_MAX];
	static uint8_t got[REGION_COL_MAX];

	regionTestDir();
	regionTestClean(0, 0);

	// State A.
	worldInit(&s_world);
	regionTestBuild(&s_world, 1, 1, BLOCK_WOOD);
	const uint32_t old_n = regionEncodeColumn(worldColumn(&s_world, 1, 1),
	                                          old_bytes, sizeof(old_bytes));
	worldExit(&s_world);
	CHECK(old_n > 0);
	CHECK(regionWriteColumn(testWorldDir(), 1, 1, old_bytes, old_n));

	// State B: same column, one block different, so the two encodings are the same length
	// and only the payload contents and the directory tell them apart.
	worldInit(&s_world);
	regionTestBuild(&s_world, 1, 1, BLOCK_SAND);
	const uint32_t new_n = regionEncodeColumn(worldColumn(&s_world, 1, 1),
	                                          new_bytes, sizeof(new_bytes));
	worldExit(&s_world);
	CHECK(new_n > 0);
	CHECK(regionWriteColumn(testWorldDir(), 1, 1, new_bytes, new_n));

	// Built rather than concatenated: the directory is only known at run time now, so
	// string-literal pasting is not available here any more.
	char whole[256], cutdir[256], cut[256];
	snprintf(whole,  sizeof(whole),  "%s/r.0.0.bsr", testWorldDir());
	snprintf(cutdir, sizeof(cutdir), "%s/cutworld", testWorldDir());
	snprintf(cut,    sizeof(cut),    "%s/cutworld/r.0.0.bsr", testWorldDir());
	testMkdir(cutdir);

	const long total = fileBytes(whole);
	CHECK(total > (long)REGION_ARENA_OFF);

	int saw_old = 0, saw_new = 0, saw_none = 0, saw_bad = 0;

	for (long keep = 0; keep <= total; keep++) {
		if (!truncCopy(whole, cut, keep)) { saw_bad++; break; }

		const uint32_t n = regionReadColumn(cutdir, 1, 1, got, sizeof(got));
		if (n == 0) { saw_none++; continue; }

		// Whatever came back must decode cleanly AND be byte-identical to one of the two
		// states. "Decodes but is neither" is the failure this whole design exists to
		// prevent, and it would mean a player's world silently changing under them.
		worldInit(&s_world);
		const bool ok = regionDecodeColumn(&s_world, 1, 1, got, n);
		worldExit(&s_world);

		if (!ok)                                                     saw_bad++;
		else if (n == old_n && memcmp(got, old_bytes, n) == 0)        saw_old++;
		else if (n == new_n && memcmp(got, new_bytes, n) == 0)        saw_new++;
		else                                                         saw_bad++;
	}

	// No cut may produce anything other than one of the two whole states.
	CHECK(saw_bad == 0);

	// And the sweep has to have actually exercised both outcomes, or it proved nothing: a
	// file that read as "nothing" at every offset would pass saw_bad == 0 trivially.
	CHECK(saw_old > 0);
	CHECK(saw_new > 0);
	CHECK(saw_none > 0);

	// ── The other failure mode: a whole file with a scrambled sector ──────────────────
	//
	// Truncation is only half of what a power cut does. An SD card writes in blocks, and an
	// interrupted write can leave a block that was *partly* updated — right length, wrong
	// contents — sitting in the middle of a file that is otherwise complete. The length
	// checks above cannot see that; only the checksums can, which is what makes this sweep
	// the one that proves the CRCs are load-bearing rather than decorative.
	saw_old = saw_new = saw_none = saw_bad = 0;

	for (long at = 0; at < total; at += 64) {
		if (!truncCopy(whole, cut, total)) { saw_bad++; break; }

		FILE* f = fopen(cut, "r+b");
		if (!f) { saw_bad++; break; }
		uint8_t junk[64];
		memset(junk, 0x5A, sizeof(junk));
		fseek(f, at, SEEK_SET);
		fwrite(junk, 1, sizeof(junk), f);
		fclose(f);

		const uint32_t n = regionReadColumn(cutdir, 1, 1, got, sizeof(got));
		if (n == 0) { saw_none++; continue; }

		worldInit(&s_world);
		const bool ok = regionDecodeColumn(&s_world, 1, 1, got, n);
		worldExit(&s_world);

		if (!ok)                                              saw_bad++;
		else if (n == old_n && memcmp(got, old_bytes, n) == 0) saw_old++;
		else if (n == new_n && memcmp(got, new_bytes, n) == 0) saw_new++;
		else                                                  saw_bad++;
	}

	CHECK(saw_bad == 0);
	CHECK(saw_old > 0);      // scrambling the new payload must fall back to the old one
	CHECK(saw_new > 0);      // scrambling dead arena space must change nothing

	remove(cut);
	regionTestClean(0, 0);
}

// The other half of the crash story: not a short file, but a *complete* file with one
// directory copy scribbled over. The loader must fall through to the good copy.
static void testRegionTornDirectory(void)
{
	static uint8_t bytes[REGION_COL_MAX];
	static uint8_t got[REGION_COL_MAX];

	regionTestDir();
	regionTestClean(0, 0);

	worldInit(&s_world);
	regionTestBuild(&s_world, 4, 4, BLOCK_WOOD);
	const uint32_t n = regionEncodeColumn(worldColumn(&s_world, 4, 4), bytes, sizeof(bytes));
	worldExit(&s_world);
	CHECK(regionWriteColumn(testWorldDir(), 4, 4, bytes, n));

	char path[256];
	snprintf(path, sizeof(path), "%s/r.0.0.bsr", testWorldDir());

	// Scribble over directory copy A, which is the live one after a single write.
	FILE* f = fopen(path, "r+b");
	CHECK(f != NULL);
	if (f) {
		uint8_t junk[64];
		memset(junk, 0xA5, sizeof(junk));
		fseek(f, 0, SEEK_SET);
		fwrite(junk, 1, sizeof(junk), f);
		fclose(f);
	}

	// One write means copy B has never been written, so there is no older good copy — the
	// right answer here is "nothing", not a decode of a corrupt directory.
	CHECK(regionReadColumn(testWorldDir(), 4, 4, got, sizeof(got)) == 0);

	// Now the case that matters: two writes, so both copies are live, and destroying the
	// newer one must leave the older one serving the previous state rather than failing.
	regionTestClean(0, 0);
	CHECK(regionWriteColumn(testWorldDir(), 4, 4, bytes, n));    // seq 1 -> copy A
	CHECK(regionWriteColumn(testWorldDir(), 4, 4, bytes, n));    // seq 2 -> copy B

	f = fopen(path, "r+b");
	CHECK(f != NULL);
	if (f) {
		uint8_t junk[64];
		memset(junk, 0xA5, sizeof(junk));
		fseek(f, (long)REGION_DIR_BYTES, SEEK_SET);              // copy B
		fwrite(junk, 1, sizeof(junk), f);
		fclose(f);
	}

	const uint32_t back = regionReadColumn(testWorldDir(), 4, 4, got, sizeof(got));
	CHECK(back == n);
	CHECK(memcmp(got, bytes, n) == 0);

	regionTestClean(0, 0);
}

// v1.7.1 task 48. The load-side region cache (regionReadColumnCached), and specifically the
// one way it can be wrong.
//
// The cache keeps a region file open and its parsed directory in memory between calls, which
// is what stops an 81-column genRequestArea paying 81 opens and 162 directory reads for one
// region's worth of answers. The whole risk in that is staleness: a directory parsed BEFORE a
// column was saved reports that column as never saved, regionReadColumnCached returns 0, and
// app/worker.c's JOB_GENERATE falls through to worldgenColumn and rebuilds it from the seed.
// The player's build is not corrupted, it is deleted — silently, with no error anywhere — and
// that is a far worse outcome than the load time this cache exists to shorten. So every check
// below marked "stale" is written to go red the moment the invalidation in region.c's
// openRegion is weakened.
//
// The crash tests above deliberately do NOT go through this path: they rewrite the region file
// behind region.c's back, truncating it and scribbling sectors into it, and a cache is by
// definition wrong about a file that changed without it being told. regionReadColumn is
// unchanged and still theirs.
static void testRegionCache(void)
{
	static uint8_t a_bytes[REGION_COL_MAX];
	static uint8_t b_bytes[REGION_COL_MAX];
	static uint8_t got[REGION_COL_MAX];
	static uint8_t plain[REGION_COL_MAX];

	regionTestDir();
	regionCacheClose();
	regionTestClean(0, 0);
	regionTestClean(1, 0);
	regionTestClean(2, 0);

	// Two states of one column, differing by a single block, so the two encodings are the
	// same length and only their contents tell them apart — the same trick testRegionPowerCut
	// uses, and for the same reason: a length comparison alone could not see a stale read.
	worldInit(&s_world);
	regionTestBuild(&s_world, 2, 3, BLOCK_WOOD);
	const uint32_t a_n = regionEncodeColumn(worldColumn(&s_world, 2, 3), a_bytes, sizeof(a_bytes));
	worldExit(&s_world);

	worldInit(&s_world);
	regionTestBuild(&s_world, 2, 3, BLOCK_SAND);
	const uint32_t b_n = regionEncodeColumn(worldColumn(&s_world, 2, 3), b_bytes, sizeof(b_bytes));
	worldExit(&s_world);

	CHECK(a_n > 0);
	CHECK(b_n == a_n);
	CHECK(memcmp(a_bytes, b_bytes, a_n) != 0);

	// ── The negative entry ────────────────────────────────────────────────────────────
	//
	// A region with no file is cached as "no file", because a fresh world has none and every
	// column of the first view would otherwise pay two failed opens for nothing. The first
	// save into that region has to make the answer change on the very next call.
	CHECK(regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got)) == 0);
	CHECK(regionWriteColumn(testWorldDir(), 2, 3, a_bytes, a_n));

	uint32_t n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == a_n);                                // stale: a kept "no file" answers 0 here
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	// ── The one that would cost a player their blocks ─────────────────────────────────
	//
	// The read above left this region's directory parsed and in memory. Overwrite the column
	// and read it straight back, which is exactly what the worker does when a player edits a
	// column, walks far enough away to unload it, and walks back.
	CHECK(regionWriteColumn(testWorldDir(), 2, 3, b_bytes, b_n));

	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);          // stale: the old column comes back

	// The repeat read: the cache HIT this whole change exists for, answering a second time
	// without touching the card.
	//
	// This is NOT a control, and it was written as one by mistake — the comment here first
	// claimed it stayed green while the invalidation was sabotaged, and the arm measured
	// otherwise (FAIL L8015/L8016 in arm C). Obvious once seen: a stale entry stays stale, so
	// the second read is stale too. The real controls in this test are the checks that do not
	// depend on invalidation at all — the never-saved column below, and every read through an
	// entry filled after the last write — and those stay green in every arm, which is what
	// says the red above is about staleness and not about the cache simply not working.
	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	// And it answers identically to the uncached function it stands in for. Nothing else in
	// this file compares the two directly, and "faster" is only worth something if it is also
	// "the same".
	const uint32_t pn = regionReadColumn(testWorldDir(), 2, 3, plain, sizeof(plain));
	CHECK(pn == n);
	CHECK(memcmp(plain, got, pn) == 0);

	// A column that shares the cached directory but was never saved still reads as 0. Slot
	// arithmetic done against a cached directory rather than a fresh parse would show here.
	CHECK(regionReadColumnCached(testWorldDir(), 5, 7, got, sizeof(got)) == 0);

	// ── Keyed by region, and by world ─────────────────────────────────────────────────
	//
	// cx 17 is region 1, so this is a different file with a different directory. Serving it
	// from region 0's cached directory would read slot 1 of the wrong file.
	CHECK(regionWriteColumn(testWorldDir(), 17, 3, a_bytes, a_n));
	n = regionReadColumnCached(testWorldDir(), 17, 3, got, sizeof(got));
	CHECK(n == a_n);
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	// Three regions through a two-entry cache, so the least recently used one is evicted and
	// then has to come back from disk *correctly*, not merely come back.
	CHECK(regionWriteColumn(testWorldDir(), 33, 3, b_bytes, b_n));
	n = regionReadColumnCached(testWorldDir(), 33, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	n = regionReadColumnCached(testWorldDir(), 17, 3, got, sizeof(got));
	CHECK(n == a_n);
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	// Same region coordinates, different world directory. Every world has an r.0.0.bsr, so
	// the directory path is part of the key or one world serves another world's blocks.
	char other[256];
	snprintf(other, sizeof(other), "%s/cacheworld2", testWorldDir());
	testMkdir(other);

	CHECK(regionWriteColumn(other, 2, 3, a_bytes, a_n));
	n = regionReadColumnCached(other, 2, 3, got, sizeof(got));
	CHECK(n == a_n);
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	// Closing has to leave the next read working, and has to be safe when there is nothing to
	// close — workerStop and workerSetWorldDir can both run against an empty cache.
	regionCacheClose();
	regionCacheClose();
	n = regionReadColumnCached(testWorldDir(), 2, 3, got, sizeof(got));
	CHECK(n == b_n);
	CHECK(memcmp(got, b_bytes, b_n) == 0);

	// The handles have to be gone before the files are: on Windows an open FILE* makes a file
	// undeletable, and this remove() failing is the visible form of a leaked handle. That is
	// the concrete reason workerSetWorldDir drops the cache instead of leaving entries to be
	// evicted on their own.
	regionCacheClose();

	char p[320];
	snprintf(p, sizeof(p), "%s/r.0.0.bsr", other);
	CHECK(remove(p) == 0);
	testRmdir(other);

	regionTestClean(0, 0);
	regionTestClean(1, 0);
	regionTestClean(2, 0);
}

// ── v1.7.1 task 48c: regionCompact ────────────────────────────────────────────────────
//
// regionCompact() had no caller and no test. region.h's header comment said it "reclaims the
// waste when a world is closed", which was not true of any build — the arena is append-only
// and nothing ever reclaimed it. Before anything could be decided about wiring it up, the
// function had to be proved, and it touches the only bytes on the card the player cannot
// regenerate. Three things are proved here and one defect was found by proving them.
//
// The fixture below is shared by all four tests: eight columns in region (0,0), six of them
// rewritten three more times, so 26 payloads sit in the arena and only 8 are live. That is
// ~69 % dead, comfortably past regionCompact's own "half wasted" gate — a fixture that did
// not clear that gate would make every test below vacuous, because regionCompact declines and
// returns false when there is nothing worth reclaiming.

#define CC_COLS 8

static uint8_t  s_cc_bytes[CC_COLS][REGION_COL_MAX];
static uint32_t s_cc_len[CC_COLS];

// All inside region (0,0) and all in distinct directory slots — 0, 81, 178, 243, 40, 121,
// 158, 223 — so no column can be masked by another and a slot-arithmetic error cannot hide.
static const int32_t s_cc_cx[CC_COLS] = { 0, 1,  2,  3, 8, 9, 14, 15 };
static const int32_t s_cc_cz[CC_COLS] = { 0, 5, 11, 15, 2, 7,  9, 13 };

// Builds that fixture and leaves the LAST written encoding of every column in s_cc_bytes.
static void compactFixture(void)
{
	static const BlockId tags[4] = { BLOCK_WOOD, BLOCK_SAND, BLOCK_DIRT, BLOCK_STONE };

	regionCacheClose();
	regionTestClean(0, 0);

	for (int round = 0; round < 4; round++) {
		// Round 0 saves all eight; later rounds rewrite only the first six, so the arena
		// fills with superseded copies while the live set stays the same size.
		const int n = (round == 0) ? CC_COLS : 6;

		worldInit(&s_world);
		for (int i = 0; i < n; i++)
			regionTestBuild(&s_world, s_cc_cx[i], s_cc_cz[i], tags[(round + i) & 3]);

		for (int i = 0; i < n; i++) {
			const uint32_t len = regionEncodeColumn(worldColumn(&s_world, s_cc_cx[i], s_cc_cz[i]),
			                                        s_cc_bytes[i], sizeof(s_cc_bytes[i]));
			CHECK(len > 0);
			s_cc_len[i] = len;
			CHECK(regionWriteColumn(testWorldDir(), s_cc_cx[i], s_cc_cz[i],
			                        s_cc_bytes[i], len));
		}
		worldExit(&s_world);
	}
}

// Every fixture column read back from `dir`, byte for byte against what was written. Returns
// how many did not match, so a sweep can count instead of asserting thousands of times.
static int compactReadAll(const char* dir)
{
	static uint8_t got[REGION_COL_MAX];
	int bad = 0;

	for (int i = 0; i < CC_COLS; i++) {
		const uint32_t n = regionReadColumn(dir, s_cc_cx[i], s_cc_cz[i], got, sizeof(got));
		if (n != s_cc_len[i] || memcmp(got, s_cc_bytes[i], n) != 0) bad++;
	}
	return bad;
}

// Cut offsets for the sweeps below. Dense over both directory copies and the first bytes of
// the arena, which is where every structural boundary in the format is, and coarse over the
// payload tail, which is thousands of interchangeable bytes — sweeping that at one-byte steps
// costs minutes and finds the same three answers.
static long compactCutStep(long keep, long total)
{
	const bool dense = keep < 64
	                || (keep > (long)REGION_DIR_BYTES - 64 && keep < (long)REGION_DIR_BYTES + 64)
	                || (keep > (long)REGION_ARENA_OFF - 64 && keep < (long)REGION_ARENA_OFF + 64)
	                || keep > total - 64;
	return dense ? 1 : 64;
}

// **The first criterion.** Compacting must reclaim real bytes and lose nothing.
static void testRegionCompactPreservesColumns(void)
{
	static uint8_t got[REGION_COL_MAX];
	static uint8_t post[REGION_COL_MAX];

	regionTestDir();
	compactFixture();

	char path[256];
	snprintf(path, sizeof(path), "%s/r.0.0.bsr", testWorldDir());

	const long before = fileBytes(path);
	CHECK(before > (long)REGION_ARENA_OFF);

	// Readable BEFORE the rewrite, or "still readable after" says nothing about the rewrite.
	CHECK(compactReadAll(testWorldDir()) == 0);

	CHECK(regionCompact(testWorldDir(), 0, 0));

	const long after = fileBytes(path);

	// Without this the whole test would pass for a regionCompact that copied the file
	// unchanged, which is the one implementation that cannot possibly lose a column.
	CHECK(after < before);
	CHECK(after >= (long)REGION_ARENA_OFF);

	// ...and every column survived it byte for byte.
	CHECK(compactReadAll(testWorldDir()) == 0);

	// Same bytes is also the same world: each one still decodes into chunks.
	for (int i = 0; i < CC_COLS; i++) {
		const uint32_t n = regionReadColumn(testWorldDir(), s_cc_cx[i], s_cc_cz[i],
		                                    got, sizeof(got));
		worldInit(&s_world);
		CHECK(regionDecodeColumn(&s_world, s_cc_cx[i], s_cc_cz[i], got, n));
		worldExit(&s_world);
	}

	// A slot nothing ever wrote is still empty, rather than pointing into the repacked arena
	// at whatever now happens to live at its old offset.
	CHECK(regionReadColumn(testWorldDir(), 6, 6, got, sizeof(got)) == 0);

	// Nothing left to reclaim, so the second call declines instead of rewriting again.
	CHECK(!regionCompact(testWorldDir(), 0, 0));

	// And it is still a live region file afterwards: a save appends past the repacked arena,
	// outranks the seq-1 directory compaction wrote, and reads back — while the columns it
	// did not touch stay exactly as compaction left them.
	worldInit(&s_world);
	regionTestBuild(&s_world, s_cc_cx[0], s_cc_cz[0], BLOCK_WOOD);
	const uint32_t pn = regionEncodeColumn(worldColumn(&s_world, s_cc_cx[0], s_cc_cz[0]),
	                                       post, sizeof(post));
	worldExit(&s_world);
	CHECK(pn > 0);
	CHECK(!(pn == s_cc_len[0] && memcmp(post, s_cc_bytes[0], pn) == 0));   // non-vacuity
	CHECK(regionWriteColumn(testWorldDir(), s_cc_cx[0], s_cc_cz[0], post, pn));

	CHECK(regionReadColumn(testWorldDir(), s_cc_cx[0], s_cc_cz[0], got, sizeof(got)) == pn);
	CHECK(memcmp(got, post, pn) == 0);

	int others_bad = 0;
	for (int i = 1; i < CC_COLS; i++) {
		const uint32_t n = regionReadColumn(testWorldDir(), s_cc_cx[i], s_cc_cz[i],
		                                    got, sizeof(got));
		if (n != s_cc_len[i] || memcmp(got, s_cc_bytes[i], n) != 0) others_bad++;
	}
	CHECK(others_bad == 0);

	regionTestClean(0, 0);
}

// **The second criterion.** Interrupting the compaction at each of its stages must leave
// either the complete old region or the complete new one, never a half-written file.
//
// regionCompact's sequence is: write a whole .tmp and close it, remove the .bsr, rename the
// .tmp onto it. regionRecover() runs before every open and resolves what it finds — a .tmp
// beside a .bsr is a compaction that did not finish, so the .tmp is dropped; a .tmp with no
// .bsr is a compaction that finished all but the rename, so the .tmp is promoted.
//
// Compaction preserves every column's bytes, so the old and the new region give the same
// answers and no content check can tell which one served a read. The stages are therefore
// separated by what is left on disk afterwards as well as by what was read.
static void testRegionCompactPowerCut(void)
{
	static uint8_t got[REGION_COL_MAX];

	regionTestDir();
	compactFixture();

	char live[256], pristine[256], repacked[256], tmp[256];
	snprintf(live,     sizeof(live),     "%s/r.0.0.bsr",  testWorldDir());
	snprintf(tmp,      sizeof(tmp),      "%s/r.0.0.tmp",  testWorldDir());
	snprintf(pristine, sizeof(pristine), "%s/pristine.bin", testWorldDir());
	snprintf(repacked, sizeof(repacked), "%s/repacked.bin", testWorldDir());

	const long pre = fileBytes(live);
	CHECK(pre > (long)REGION_ARENA_OFF);
	CHECK(truncCopy(live, pristine, pre));

	CHECK(regionCompact(testWorldDir(), 0, 0));

	const long post = fileBytes(live);
	CHECK(post < pre);
	CHECK(post > (long)REGION_ARENA_OFF);
	CHECK(truncCopy(live, repacked, post));

	// A completed compaction leaves nothing behind for regionRecover to resolve.
	CHECK(fileBytes(tmp) < 0);

	// The two files above are byte-exact copies of what a power cut would find on the card:
	// pristine.bin is the .bsr before the rewrite, repacked.bin is the .tmp the rewrite
	// produces (the rename does not change a byte of it).
	// Wider than cutdir by more than the file names, or -Wformat-truncation refuses the build.
	char cutdir[256], cutbsr[320], cuttmp[320];
	snprintf(cutdir, sizeof(cutdir), "%s/cutcompact", testWorldDir());
	testMkdir(cutdir);
	snprintf(cutbsr, sizeof(cutbsr), "%s/r.0.0.bsr", cutdir);
	snprintf(cuttmp, sizeof(cuttmp), "%s/r.0.0.tmp", cutdir);

	// ── Stage 1: cut while the .tmp is being written. The .bsr is still there ──────────
	//
	// Every cut, including the one where the .tmp is complete, must serve the OLD region in
	// full and must drop the .tmp. This is the stage that goes red if the loader ever
	// prefers a .tmp it cannot know is finished.
	int s1_bad = 0, s1_dropped = 0, s1_cuts = 0;

	for (long keep = 0; keep <= post; keep += compactCutStep(keep, post)) {
		remove(cutbsr);
		remove(cuttmp);
		if (!truncCopy(pristine, cutbsr, pre))    { s1_bad++; break; }
		if (!truncCopy(repacked, cuttmp, keep))   { s1_bad++; break; }

		s1_cuts++;
		if (compactReadAll(cutdir) != 0) s1_bad++;
		if (fileBytes(cuttmp) < 0)       s1_dropped++;
		if (fileBytes(cutbsr) != pre)    s1_bad++;      // the good file was not touched
	}

	CHECK(s1_bad == 0);
	CHECK(s1_cuts > 64);                 // the sweep actually ran
	CHECK(s1_dropped == s1_cuts);        // and every one of them dropped the .tmp

	// ── Stage 2: cut between the remove and the rename. No .bsr, complete .tmp ────────
	//
	// The one window regionCompact leaves open, and the reason regionRecover exists at all.
	remove(cutbsr);
	remove(cuttmp);
	CHECK(truncCopy(repacked, cuttmp, post));
	CHECK(fileBytes(cutbsr) < 0);                       // there really is no .bsr going in
	CHECK(compactReadAll(cutdir) == 0);
	CHECK(fileBytes(cutbsr) == post);                   // promoted
	CHECK(fileBytes(cuttmp) < 0);

	// ── Stage 3: no .bsr and a SHORT .tmp ─────────────────────────────────────────────
	//
	// regionCompact's own sequence cannot reach this on a filesystem that honours the close
	// before the remove, but a card that has not flushed the .tmp's sectors when the power
	// goes can, and that is the state regionRecover promotes blind. Swept anyway: every
	// column must read as itself or as nothing. "Decodes but is a different column" is the
	// outcome the whole format exists to prevent, and it is the one that would silently
	// rearrange a player's world.
	int s3_bad = 0, s3_full = 0, s3_none = 0;

	for (long keep = 0; keep < post; keep += compactCutStep(keep, post)) {
		remove(cutbsr);
		remove(cuttmp);
		if (!truncCopy(repacked, cuttmp, keep)) { s3_bad++; break; }

		for (int i = 0; i < CC_COLS; i++) {
			const uint32_t n = regionReadColumn(cutdir, s_cc_cx[i], s_cc_cz[i],
			                                    got, sizeof(got));
			if (n == 0)                                                      s3_none++;
			else if (n == s_cc_len[i] && memcmp(got, s_cc_bytes[i], n) == 0) s3_full++;
			else                                                             s3_bad++;
		}
	}

	CHECK(s3_bad == 0);
	CHECK(s3_full > 0);      // a nearly-complete .tmp still serves its columns
	CHECK(s3_none > 0);      // and a badly cut one serves nothing rather than guessing

	testRmTree(cutdir);
	remove(pristine);
	remove(repacked);
	regionTestClean(0, 0);
}

// **The third criterion.** regionCompact against the load-side region cache.
//
// regionCompact is the one writer that does not go through openRegion(create=true), so it
// carries its own regionCacheClose() calls — one at the top and one before the remove/rename.
//
// The hard part of testing that is that compaction preserves every column's bytes, so a cache
// entry left pointing at the pre-compaction file returns exactly the right answer and hides
// itself completely. The check that discriminates is at the bottom: the compacted file's
// directory is destroyed behind region.c's back, so a cache that was dropped has to re-read
// the file and finds nothing, while a stale entry answers from the handle it still holds on
// the file compaction replaced.
static void testRegionCompactCache(void)
{
	static uint8_t got[REGION_COL_MAX];
	static uint8_t plain[REGION_COL_MAX];

	regionTestDir();
	compactFixture();

	char path[256];
	snprintf(path, sizeof(path), "%s/r.0.0.bsr", testWorldDir());

	// Warm the cache, so an entry holding this region's pre-compaction directory AND an open
	// handle on the pre-compaction file exists when regionCompact runs.
	regionCacheClose();
	uint32_t n = regionReadColumnCached(testWorldDir(), s_cc_cx[0], s_cc_cz[0],
	                                    got, sizeof(got));
	CHECK(n == s_cc_len[0]);
	CHECK(memcmp(got, s_cc_bytes[0], n) == 0);

	// remove(src) is inside regionCompact. An entry still holding the file open makes that
	// remove fail on Windows and on the console's FAT card, so the return value is itself a
	// check on the invalidation — on POSIX it is not, and this line is green either way there.
	CHECK(regionCompact(testWorldDir(), 0, 0));

	// The cached answer after the rewrite is the uncached answer.
	n = regionReadColumnCached(testWorldDir(), s_cc_cx[1], s_cc_cz[1], got, sizeof(got));
	const uint32_t pn = regionReadColumn(testWorldDir(), s_cc_cx[1], s_cc_cz[1],
	                                     plain, sizeof(plain));
	CHECK(n == s_cc_len[1]);
	CHECK(pn == n);
	CHECK(memcmp(got, plain, n) == 0);

	// The discriminating check. Warm the cache again, compact again, then destroy the file
	// compaction produced. A dropped cache re-reads it and finds nothing; a stale one still
	// answers from the file that was replaced.
	compactFixture();
	regionCacheClose();
	n = regionReadColumnCached(testWorldDir(), s_cc_cx[2], s_cc_cz[2], got, sizeof(got));
	CHECK(n == s_cc_len[2]);
	CHECK(regionCompact(testWorldDir(), 0, 0));

	FILE* f = fopen(path, "r+b");
	CHECK(f != NULL);
	if (f) {
		uint8_t junk[64];
		memset(junk, 0xA5, sizeof(junk));
		fseek(f, 0, SEEK_SET);                    // directory copy A, the only live one
		fwrite(junk, 1, sizeof(junk), f);
		fclose(f);
	}

	// Copy B of a compacted file has never been written, so with A destroyed there is no
	// readable directory left and the honest answer is nothing at all.
	CHECK(regionReadColumn(testWorldDir(), s_cc_cx[2], s_cc_cz[2], plain, sizeof(plain)) == 0);
	CHECK(regionReadColumnCached(testWorldDir(), s_cc_cx[2], s_cc_cz[2], got, sizeof(got)) == 0);

	regionCacheClose();
	regionTestClean(0, 0);
}

// **The defect this exercise found.** regionCompact against a region file that has already
// survived a power cut.
//
// region.c's dirLoadPair makes an explicit promise about that state: a card can flush the
// directory sectors before the payload sectors, so "the newest directory points at a payload
// that is not all there" is reachable, the previous directory still describes a complete
// older world, and regionReadColumn falls back to it — "the difference between a power cut
// costing the last save and a power cut costing the whole column".
//
// regionCompact rewrites the file wholesale, and it read only the newer directory. A column
// in that state was dropped from the repack: unreadable in the newer copy, and the older copy
// that could still have served it thrown away with the old file. Before the fix this test's
// last two checks failed — the column that read correctly one line earlier read as nothing.
// It would have happened at world close, unprompted, on a save the player could still have
// had. The fix gives regionCompact the same two-entry fallback regionReadColumn already has.
static void testRegionCompactKeepsPowerCutFallback(void)
{
	static uint8_t a_bytes[REGION_COL_MAX];
	static uint8_t b_bytes[REGION_COL_MAX];
	static uint8_t got[REGION_COL_MAX];

	regionTestDir();

	// The waste is created FIRST. Every save copies the live directory forward into the copy
	// it writes, so a save after the torn one below would carry the broken entry into both
	// copies and destroy the fallback before compaction ever got the chance.
	compactFixture();

	const int32_t cx = 6, cz = 6;          // slot 102, which compactFixture does not use

	worldInit(&s_world);
	regionTestBuild(&s_world, cx, cz, BLOCK_WOOD);
	const uint32_t a_n = regionEncodeColumn(worldColumn(&s_world, cx, cz),
	                                        a_bytes, sizeof(a_bytes));
	worldExit(&s_world);

	worldInit(&s_world);
	regionTestBuild(&s_world, cx, cz, BLOCK_SAND);
	const uint32_t b_n = regionEncodeColumn(worldColumn(&s_world, cx, cz),
	                                        b_bytes, sizeof(b_bytes));
	worldExit(&s_world);

	CHECK(a_n > 0);
	CHECK(b_n > 0);

	char path[256];
	snprintf(path, sizeof(path), "%s/r.0.0.bsr", testWorldDir());

	// Version A then version B, as the last two writes: the newer directory copy carries B's
	// entry and the older one still carries A's. Payloads are appended, so each one ends at
	// the file's new end.
	CHECK(regionWriteColumn(testWorldDir(), cx, cz, a_bytes, a_n));
	const long a_at = fileBytes(path) - (long)a_n;
	CHECK(regionWriteColumn(testWorldDir(), cx, cz, b_bytes, b_n));
	const long b_at = fileBytes(path) - (long)b_n;
	CHECK(b_at > a_at);

	// The power cut: the directory that points at B is on the card, B's payload sectors are
	// stale. Right length, wrong bytes — only the per-payload CRC can see this.
	FILE* f = fopen(path, "r+b");
	CHECK(f != NULL);
	if (f) {
		uint8_t junk[16];
		memset(junk, 0x5A, sizeof(junk));
		fseek(f, b_at, SEEK_SET);
		fwrite(junk, 1, sizeof(junk), f);
		fclose(f);
	}
	regionCacheClose();

	// The guarantee as it stands before compaction: B is unreadable, so A is served.
	memset(got, 0, sizeof(got));
	uint32_t n = regionReadColumn(testWorldDir(), cx, cz, got, sizeof(got));
	CHECK(n == a_n);
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	// The same read after compaction. Anything else is a column the player could still have
	// had, destroyed by the reclaim pass.
	//
	// The buffer is cleared first for a reason: without it the read above leaves A's bytes
	// sitting in `got`, a failing read leaves them there untouched, and the memcmp below
	// passes on the previous answer instead of this one — a check that could not go red.
	CHECK(regionCompact(testWorldDir(), 0, 0));
	memset(got, 0, sizeof(got));
	n = regionReadColumn(testWorldDir(), cx, cz, got, sizeof(got));
	CHECK(n == a_n);
	CHECK(memcmp(got, a_bytes, a_n) == 0);

	// And the rest of the region is untouched by a repack that had to take a fallback.
	CHECK(compactReadAll(testWorldDir()) == 0);

	regionTestClean(0, 0);
}


// The generator-version sidecar on a real filesystem. Host-only because it makes and deletes
// directories, exactly like the region tests above, and it reuses their per-process directory
// helper for the same reason: two concurrent runs of this suite must not share a path.
static void genverTestDir(char* out, size_t cap, const char* leaf)
{
	testMkdir("build-host");
	testMkdir(testWorldDir());
	snprintf(out, cap, "%s/%s", testWorldDir(), leaf);
	testRmTree(out);
	testMkdir(out);
}

// Answers false, and leaves `out` empty, when the path does not fit. gcc 16 rejects the bare
// snprintf this used to be — "'%s' directive output may be truncated writing 10 bytes into a
// region of size between 0 and 255", an error under -Werror — and it is right to: `dir` is
// unbounded going into a 256-byte `out`, so "genver.bin" has between 0 and 255 bytes to land in.
// gcc 15 says nothing, which is why it sat here; the hazard was identical on both.
//
// Truncation is refused rather than performed, because a truncated path is not a broken path.
// It is a perfectly openable name for a DIFFERENT file, so every caller below would go on to
// poke, size or read that file and report its answer as the stamp's — a suite going green on
// the wrong file, which is the one failure mode a test helper must not have. Widening `out`
// only moves that hazard further out; there is no width at which "it fits" is a property of
// this code rather than of today's callers.
//
// An empty `out` is the refusal because it is the one name no fopen can mistake for a
// neighbour: all four call sites already CHECK the open or the size that follows, so a refusal
// lands as a red check instead of a wrong answer. Same discipline as 1a806b3's saturating
// builders — count what actually fits, never what snprintf wished for — applied to the one
// case where the right amount to saturate at is none.
static bool genverStampPath(char* out, size_t cap, const char* dir)
{
	const int n = snprintf(out, cap, "%s/%s", dir, GEN_VERSION_FILE);
	if (n < 0 || (size_t)n >= cap) {
		if (cap) out[0] = '\0';   // cap 0 has nowhere to put even the NUL: touch nothing.
		return false;
	}
	return true;
}

// Writes `n` bytes over the start of the stamp file, leaving its length alone.
static void genverPoke(const char* dir, long off, const uint8_t* bytes, size_t n)
{
	char p[256];
	genverStampPath(p, sizeof p, dir);
	FILE* f = fopen(p, "r+b");
	if (!f) return;
	fseek(f, off, SEEK_SET);
	fwrite(bytes, 1, n, f);
	fclose(f);
}

static long genverSize(const char* dir)
{
	char p[256];
	genverStampPath(p, sizeof p, dir);
	FILE* f = fopen(p, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	return n;
}

// v1.8.3. Slurps a source file so a test can assert on its TEXT. Needed for exactly one thing —
// checking that source/main.c really routes the refusal — and copied in shape from the same
// helper in source/app/session_test.c, which exists for the same reason: main.c cannot be linked
// into a host binary (it owns main(), pulls in <3ds.h>, and every module below it is stubbed
// here), so the only way to test that a decision is wired into the boot path is to read it.
// Caller frees.
static char* genverReadWholeFile(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	const long n = ftell(f);
	if (n <= 0) { fclose(f); return NULL; }
	rewind(f);
	char* buf = (char*)malloc((size_t)n + 1);
	if (!buf) { fclose(f); return NULL; }
	const size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

// ── genverStampPath's refusal ─────────────────────────────────────────────────────────────
//
// The helper builds "<dir>/genver.bin" into a fixed 256-byte buffer out of a `dir` it does not
// bound, and until this existed nothing asserted what happens when that does not fit — nor
// could it: every `dir` the sidecar test passes is a short build-host path, so the truncating
// branch was UNEXECUTED by the suite rather than merely unchecked. That is the same gap
// 1a806b3 found in loadprofFormat, and it is why gcc 16 was the first thing to notice.
//
// `out` is a window inside a larger '#'-filled array so a write past `cap` is caught as data
// rather than as a crash that may or may not happen, and so the cap-0 case can be shown to
// write nothing at all rather than merely to return false.
static void testGenverStampPath(void)
{
	// "<dir>/genver.bin" needs dirlen + 11 characters plus a NUL, so at cap 256 the longest
	// dir that fits is 244. The pairs straddle that boundary and two smaller ones, because an
	// off-by-one here reads as a working path right up until it silently addresses a
	// neighbouring file.
	static const struct { size_t cap; size_t dirlen; } kCases[] = {
		{ 256,    1 }, { 256,   10 }, { 256,  100 }, { 256,  243 },
		{ 256,  244 },   // exactly fits: 244 + 11 = 255, + NUL = 256
		{ 256,  245 },   // one byte over
		{ 256,  246 }, { 256,  300 }, { 256, 1000 },
		{  16,    4 },   // exactly fits: "dddd/genver.bin" is 15, + NUL = 16
		{  16,   10 },
		{  13,    1 },   // exactly fits: "d/genver.bin" is 12, + NUL = 13
		{  12,    1 },   // one byte short of that
		{   1,    1 },   // room for the NUL and nothing else
		{   0,    1 },   // no room even for the NUL
	};

	static const size_t kPad = 64;
	for (size_t k = 0; k < sizeof kCases / sizeof kCases[0]; k++) {
		const size_t cap    = kCases[k].cap;
		const size_t dirlen = kCases[k].dirlen;

		char dir[1024];
		memset(dir, 'd', dirlen);
		dir[dirlen] = '\0';

		// Deliberately large enough that even the longest dir here would fit, so the content
		// checks below can never themselves read out of bounds whatever the function does.
		// The last byte is a NUL rather than padding for the same reason: with the fix
		// sabotaged the success branch runs on a buffer the helper truncated, and an
		// unterminated array would send strlen off the end — turning the red arm into a crash
		// instead of into the FAIL line that is the whole point of running it.
		char raw[2048];
		memset(raw, '#', sizeof raw);
		raw[sizeof raw - 1] = '\0';
		char* out = raw + kPad;

		const bool ok = genverStampPath(out, cap, dir);

		const size_t want = dirlen + 1 + (sizeof GEN_VERSION_FILE - 1);
		CHECK(ok == (want + 1 <= cap));

		// On success the path is the whole path, not a prefix of it that happens to open. On
		// refusal `out` is empty — except at cap 0, where there is nowhere to put even the NUL
		// and the padding check below is the thing that proves nothing was written.
		//
		// Folded into a named bool rather than written inline, because CHECK stringifies its
		// condition into a 96-byte s_first and gcc 16 rejects the file outright when that
		// overflows: measured at "163 bytes into a region of size 90" for the inline form.
		const bool path_as_documented =
		    ok ? (strlen(out) == want && out[dirlen] == '/' &&
		          memcmp(out, dir, dirlen) == 0 &&
		          strcmp(out + dirlen + 1, GEN_VERSION_FILE) == 0)
		       : (cap == 0 || out[0] == '\0');
		CHECK(path_as_documented);

		bool pad_before_intact = true;
		for (size_t i = 0; i < kPad; i++)
			if (raw[i] != '#') pad_before_intact = false;
		CHECK(pad_before_intact);

		bool pad_after_intact = true;
		for (size_t i = kPad + cap; i + 1 < sizeof raw; i++)   // last byte is the NUL above
			if (raw[i] != '#') pad_after_intact = false;
		CHECK(pad_after_intact);
	}
}

static void testGenVersionSidecar(void)
{
	char dir[256];
	uint32_t v;

	// ── No world directory at all: a joined server session. ───────────────────────────
	v = 0xDEADu;
	CHECK(genVersionResolve(NULL, &v) == GENVER_NO_WORLD_DIR);
	CHECK(v == GEN_VERSION_LEGACY);
	v = 0xDEADu;
	CHECK(genVersionResolve("", &v) == GENVER_NO_WORLD_DIR);
	CHECK(v == GEN_VERSION_LEGACY);

	// ── A brand-new, empty world directory. ───────────────────────────────────────────
	// Nothing has ever been saved here, so this is a world being created now and it gets
	// the newest generator — and gets it written down before anything else can happen.
	genverTestDir(dir, sizeof dir, "gv-new");
	v = 0;
	CHECK(genVersionRead(dir, &v) == GENVER_OK);
	CHECK(v == GEN_VERSION_LEGACY);              // absent reads as legacy...
	CHECK(genverSize(dir) == -1);                // ...and reading does not create one
	v = 0;
	CHECK(genVersionResolve(dir, &v) == GENVER_OK);
	CHECK(v == GEN_VERSION_FOR_NEW_WORLDS);
	CHECK(genverSize(dir) == GEN_VERSION_BYTES); // resolve stamped it
	// Second resolve reads the stamp rather than re-deriving it, and agrees.
	v = 0;
	CHECK(genVersionResolve(dir, &v) == GENVER_OK);
	CHECK(v == GEN_VERSION_FOR_NEW_WORLDS);

	// **The line the whole prerequisite turns on.** Saving now creates a region file. If the
	// stamp had not been written first, this world would read as legacy from here on and its
	// terrain would change under the player on the next boot.
	{
		char bsr[320];
		snprintf(bsr, sizeof bsr, "%s/r.0.0.bsr", dir);
		FILE* f = fopen(bsr, "wb");
		CHECK(f != NULL);
		if (f) { fputc(0, f); fclose(f); }
		v = 0;
		CHECK(genVersionResolve(dir, &v) == GENVER_OK);
		CHECK(v == GEN_VERSION_FOR_NEW_WORLDS);
	}
	testRmTree(dir);

	// ── An unstamped directory that HAS been saved: every world in existence today. ────
	genverTestDir(dir, sizeof dir, "gv-old");
	{
		char bsr[320];
		snprintf(bsr, sizeof bsr, "%s/r.-1.2.bsr", dir);
		FILE* f = fopen(bsr, "wb");
		CHECK(f != NULL);
		if (f) { fputc(0, f); fclose(f); }
	}
	v = 0;
	CHECK(genVersionResolve(dir, &v) == GENVER_OK);
	CHECK(v == GEN_VERSION_LEGACY);
	CHECK(genverSize(dir) == GEN_VERSION_BYTES);
	// And it is now stamped, so it stays legacy for the rest of the world's life.
	v = 0;
	CHECK(genVersionRead(dir, &v) == GENVER_OK);
	CHECK(v == GEN_VERSION_LEGACY);
	testRmTree(dir);

	// ── Round trip of every known version. ────────────────────────────────────────────
	genverTestDir(dir, sizeof dir, "gv-rt");
	for (uint32_t want = GEN_VERSION_LEGACY; want <= GEN_VERSION_NEWEST; want++) {
		CHECK(genVersionWrite(dir, want));
		v = 0;
		CHECK(genVersionRead(dir, &v) == GENVER_OK);
		CHECK(v == want);
		v = 0;
		CHECK(genVersionResolve(dir, &v) == GENVER_OK);   // must not overwrite
		CHECK(v == want);
	}

	// ── A save from a NEWER build. Refused, and the value is reported so the caller can
	//    say what it saw. Not guessed at, not silently run as the newest known.
	CHECK(genVersionWrite(dir, GEN_VERSION_NEWEST + 1u));
	v = 0;
	CHECK(genVersionRead(dir, &v) == GENVER_TOO_NEW);
	CHECK(v == GEN_VERSION_NEWEST + 1u);
	v = 0;
	CHECK(genVersionResolve(dir, &v) == GENVER_TOO_NEW);
	CHECK(v == GEN_VERSION_NEWEST + 1u);
	// And worldgenInit will not run it even if a caller ignored the status.
	{
		WorldGen g;
		CHECK(!worldgenInit(&g, 1337u, GEN_VERSION_NEWEST + 1u));
	}

	CHECK(genVersionWrite(dir, 0xFFFFu));
	v = 0;
	CHECK(genVersionRead(dir, &v) == GENVER_TOO_NEW);
	CHECK(v == 0xFFFFu);

	// ── Damaged, four ways. Each must refuse, and none may fall back to legacy — falling
	//    back to a DIFFERENT generator than the one that shaped this world is the exact
	//    harm the file exists to prevent.
	{
		// Bad magic.
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		const uint8_t bad[1] = {'X'};
		genverPoke(dir, 0, bad, 1);
		CHECK(genVersionRead(dir, NULL) == GENVER_DAMAGED);
		CHECK(genVersionResolve(dir, &v) == GENVER_DAMAGED);
	}
	{
		// Good magic, flipped payload: the CRC catches it.
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		const uint8_t flip[1] = {0x7F};
		genverPoke(dir, 4, flip, 1);
		CHECK(genVersionRead(dir, NULL) == GENVER_DAMAGED);
	}
	{
		// Good record, corrupted CRC field.
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		const uint8_t junk[4] = {0xA5, 0xA5, 0xA5, 0xA5};
		genverPoke(dir, 8, junk, 4);
		CHECK(genVersionRead(dir, NULL) == GENVER_DAMAGED);
	}
	{
		// Truncated — a torn write.
		char p[256];
		genverStampPath(p, sizeof p, dir);
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		uint8_t head[GEN_VERSION_BYTES];
		FILE* f = fopen(p, "rb");
		CHECK(f != NULL);
		if (f) { CHECK(fread(head, 1, sizeof head, f) == sizeof head); fclose(f); }
		f = fopen(p, "wb");
		CHECK(f != NULL);
		if (f) { fwrite(head, 1, 6, f); fclose(f); }
		CHECK(genverSize(dir) == 6);
		CHECK(genVersionRead(dir, NULL) == GENVER_DAMAGED);
	}
	{
		// Longer than the record: something else wrote here.
		char p[256];
		genverStampPath(p, sizeof p, dir);
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		FILE* f = fopen(p, "ab");
		CHECK(f != NULL);
		if (f) { fputc(0x00, f); fclose(f); }
		CHECK(genverSize(dir) == GEN_VERSION_BYTES + 1);
		CHECK(genVersionRead(dir, NULL) == GENVER_DAMAGED);
	}

	testRmTree(dir);

	// ── A card that will not take the stamp, on a world that has never been saved. ────
	//
	// The mint branch's write is the one that has to land — genversion.c's comment above its
	// return says why — so a refused write there has to be reported and not swallowed. Until
	// this arm existed nothing in the suite could make genVersionWrite fail at all: every
	// other call to it is CHECKed for success, so the discarded result had no test that could
	// go red, and the bug shipped.
	//
	// Forcing a real refusal on a host that is not the console took finding. Two obvious
	// routes are dead on this machine, both measured 2026-08-25 before this was written:
	//
	//   * A read-only world directory. chmod does not stick in this checkout — WSL mounts C:
	//     as 9p/drvfs with no metadata, so a chmod 0555 directory came back drwxrwxrwx and
	//     the write went straight through.
	//   * A *directory* named genver.bin, so the write hits EISDIR. It does (errno 21), but
	//     glibc's fopen(<a directory>, "rb") SUCCEEDS on Linux, so genVersionResolve's read
	//     probe would find a "stamp" and answer GENVER_DAMAGED without ever reaching the
	//     branch under test.
	//
	// What is left, and behaves the same on Windows and Linux, is a world_dir that lives
	// under a path component that is a regular FILE: every open below it fails with ENOTDIR.
	// Honest caveat: opendir() fails there too, so hasRegionFile() returns false because the
	// listing failed rather than because it was empty. It is the same branch either way, and
	// the checks below pin down which branch actually answered rather than assuming.
	{
		char blocked[128];
		snprintf(blocked, sizeof blocked, "%s/gv-notadir", testWorldDir());
		remove(blocked);
		FILE* nf = fopen(blocked, "wb");
		CHECK(nf != NULL);
		if (nf) fclose(nf);

		char inside[192];
		snprintf(inside, sizeof inside, "%s/w", blocked);

		// Preconditions, so a pass cannot be a pass for the wrong reason. The path is
		// non-empty, so the NO_WORLD_DIR early return is not what answers; and no stamp is
		// readable there, so the stamp-exists branch is not what answers either.
		v = 0xDEADu;
		CHECK(genVersionRead(inside, &v) == GENVER_OK);
		CHECK(v == GEN_VERSION_LEGACY);
		CHECK(genverSize(inside) == -1);

		// The refusal itself. GENVER_STAMP_FAILED *together with* a derived
		// GEN_VERSION_FOR_NEW_WORLDS can only come out of the mint branch: the legacy branch
		// derives GEN_VERSION_LEGACY and the no-directory return leaves genVersionForSession(),
		// which is also GEN_VERSION_LEGACY.
		v = 0xDEADu;
		CHECK(genVersionResolve(inside, &v) == GENVER_STAMP_FAILED);
		CHECK(v == GEN_VERSION_FOR_NEW_WORLDS);
		CHECK(genverSize(inside) == -1);   // and nothing was stamped anywhere

		remove(blocked);
	}

	// ══ v1.8.3: the REFUSAL, which is what every status above is for ═══════════════════
	//
	// Everything before this point tests what genVersionResolve() decides. None of it tests
	// whether anything ACTS on the decision, and that gap is not hypothetical: 82d4a1e added
	// GENVER_STAMP_FAILED with the red/green arm above proving it is returned, and main.c gated
	// the refusal on `if (gv == GENVER_TOO_NEW || gv == GENVER_DAMAGED)` — an expression that
	// names two statuses by hand. The new one fell straight through it and the world loaded
	// anyway. A returned value with nothing reading it is not a fix, and this suite could not
	// see the difference.
	//
	// So the decision is now a total function over the enum in world/genrefuse.h, and these are
	// its tests. Read that file's comment for why a switch with no default is the mechanism: it
	// turns "a new status nobody routed" from a silent wrong answer into a -Wswitch/-Werror
	// build failure, which is a stronger guarantee than any check below.
	{
		// ── 1. The predicate, over every enumerator that exists. ─────────────────────
		//
		// Written out one enumerator at a time rather than looped, so a red run names WHICH
		// status stopped being routed. The two non-refusals are the CONTROL: they must stay
		// green whatever happens to the three refusals, which is what makes a red run here
		// isolate the routing rather than the file failing to compile or link.
		CHECK(genVersionRefusalText(GENVER_OK) == NULL);
		CHECK(!genVersionRefuses(GENVER_OK));
		CHECK(genVersionRefusalText(GENVER_NO_WORLD_DIR) == NULL);
		CHECK(!genVersionRefuses(GENVER_NO_WORLD_DIR));

		CHECK(genVersionRefuses(GENVER_TOO_NEW));
		CHECK(genVersionRefuses(GENVER_DAMAGED));
		CHECK(genVersionRefuses(GENVER_STAMP_FAILED));

		// ── 2. What the player is actually shown. ────────────────────────────────────
		//
		// Three requirements, and each one is a real failure mode rather than a tidiness
		// check. Non-empty: an empty string is a refusal with no explanation, which is the
		// state this whole task exists to end. Distinct: genversion.h's comment on
		// GENVER_STAMP_FAILED says the separate status exists precisely because the sentence
		// is a different sentence — a card that would not take the write is something the
		// player can fix, an unreadable stamp is not — so three statuses sharing one string
		// would give that reasoning away. Short: the line lands in TitleState.status, a
		// char[48] (scene/title.h), drawn at scale 1 into a 320 px screen, which at
		// gfx/font.h's FONT_ADVANCE of 6 is 53 characters — the buffer is the tighter of the
		// two, so 47 is the real budget.
		const char* const msg[3] = { genVersionRefusalText(GENVER_TOO_NEW),
		                             genVersionRefusalText(GENVER_DAMAGED),
		                             genVersionRefusalText(GENVER_STAMP_FAILED) };
		for (int i = 0; i < 3; i++) {
			CHECK(msg[i] != NULL);
			if (!msg[i]) continue;
			CHECK(msg[i][0] != '\0');
			CHECK(strlen(msg[i]) <= 47);
		}
		if (msg[0] && msg[1] && msg[2]) {
			CHECK(strcmp(msg[0], msg[1]) != 0);
			CHECK(strcmp(msg[0], msg[2]) != 0);
			CHECK(strcmp(msg[1], msg[2]) != 0);
		}
	}

	// ── 3. End to end, through the REAL genVersionResolve on the REAL filesystem. ───────
	//
	// The checks above take a status as an argument, so on their own they prove only that a
	// constant maps to a string. These build the four world directories a console can actually
	// present, resolve each one exactly as main.c's genStart() does, and feed the answer to the
	// same predicate main.c gates on — so the pair under test is (what the card says) ->
	// (whether the player gets in), with nothing hand-fed in the middle.
	{
		// A brand-new world that resolves cleanly. The CONTROL arm of this block: it must stay
		// green in every sabotage of the refusal, because a world that is fine must never stop
		// opening — that failure would be worse than the one being fixed.
		genverTestDir(dir, sizeof dir, "gv-route-ok");
		v = 0;
		const GenVersionStatus st_ok = genVersionResolve(dir, &v);
		CHECK(st_ok == GENVER_OK);
		CHECK(!genVersionRefuses(st_ok));
		CHECK(genVersionRefusalText(st_ok) == NULL);
		testRmTree(dir);

		// Stamped by a build that does not exist yet.
		genverTestDir(dir, sizeof dir, "gv-route-toonew");
		CHECK(genVersionWrite(dir, GEN_VERSION_NEWEST + 1u));
		const GenVersionStatus st_new = genVersionResolve(dir, &v);
		CHECK(st_new == GENVER_TOO_NEW);
		CHECK(genVersionRefuses(st_new));
		testRmTree(dir);

		// Stamped, and the stamp cannot be read.
		genverTestDir(dir, sizeof dir, "gv-route-damaged");
		CHECK(genVersionWrite(dir, GEN_VERSION_DENSITY));
		{
			const uint8_t bad_magic[1] = {'X'};
			genverPoke(dir, 0, bad_magic, 1);
		}
		const GenVersionStatus st_bad = genVersionResolve(dir, &v);
		CHECK(st_bad == GENVER_DAMAGED);
		CHECK(genVersionRefuses(st_bad));
		testRmTree(dir);

		// The one 82d4a1e added and nothing routed: a mint world the card will not stamp. Same
		// ENOTDIR-under-a-regular-file trick the arm above uses, and for the reasons documented
		// there — chmod does not stick on this checkout's mount and a directory named genver.bin
		// is opened successfully by glibc.
		{
			char blocked2[128];
			snprintf(blocked2, sizeof blocked2, "%s/gv-route-notadir", testWorldDir());
			remove(blocked2);
			FILE* nf = fopen(blocked2, "wb");
			CHECK(nf != NULL);
			if (nf) fclose(nf);

			char inside2[192];
			snprintf(inside2, sizeof inside2, "%s/w", blocked2);

			v = 0xDEADu;
			const GenVersionStatus st_sf = genVersionResolve(inside2, &v);
			CHECK(st_sf == GENVER_STAMP_FAILED);
			CHECK(v == GEN_VERSION_FOR_NEW_WORLDS);
			CHECK(genVersionRefuses(st_sf));
			CHECK(genVersionRefusalText(st_sf) != NULL);

			remove(blocked2);
		}
	}

	// ── 4. And that source/main.c actually routes it. ───────────────────────────────────
	//
	// The block above proves the predicate answers correctly. It cannot prove main.c asks it, and
	// "the predicate is right, nothing calls it" is precisely the shape of the bug being fixed —
	// one layer up. main.c cannot be linked here (see genverReadWholeFile), so its text is read,
	// exactly as source/app/session_test.c reads it to prove sessionBegin() is on the lap.
	//
	// Each fact is asserted separately so a red run says which one broke, and the negative one is
	// as load-bearing as the positives: while the old hand-written two-status `if` is still in
	// the file, a third status can still be forgotten, whatever else is also true.
	//
	// Honest about what this is: a text match, so renaming `world_refused` breaks it without
	// breaking the game. That is the accepted cost of the alternative being no coverage at all.
	// Run from the repository root, the same working directory this file's atlas checks use.
	{
		char* src = genverReadWholeFile("source/main.c");
		CHECK(src != NULL);
		if (src) {
			// The refusal is decided by the total function, not by a list of statuses.
			CHECK(strstr(src, "#include \"world/genrefuse.h\"") != NULL);
			CHECK(strstr(src, "genVersionRefusalText(gv)") != NULL);
			// This one matched on the first run — against the COMMENT in main.c that quoted
			// the old line verbatim while explaining why it went. strstr cannot tell code from
			// prose, so main.c's comment now describes the old expression instead of spelling
			// it, and the verbatim text is kept in world/genrefuse.h's file comment where
			// nothing greps for it. Worth recording rather than quietly working around: it is
			// the proof this check reads the real file and can go red.
			CHECK(strstr(src, "gv == GENVER_TOO_NEW || gv == GENVER_DAMAGED") == NULL);

			// genStart's false answer becomes a refusal the rest of the boot path can see...
			CHECK(strstr(src, "const bool world_refused = !worker_ok;") != NULL);

			// ...the loading screen is skipped, so there is no 900-frame stall...
			CHECK(strstr(src, "if (!world_refused &&") != NULL);

			// ...and the way out is the pause menu's existing return to the title screen, which
			// runs the whole teardown, rather than a jump past it or a quit.
			CHECK(strstr(src, "bool quit_to_title = world_refused;") != NULL);

			// A refused world is left exactly as it was found: neither save writes into it.
			CHECK(strstr(src, "if (inv_dir && !world_refused)") != NULL);

			free(src);
		}
	}
}

#endif  // !__3DS__

// v1.7.1 task 49 (install half). chunkPlanAll + chunkLoadPlanned are the one-pass
// replacement for chunkFormForAll + chunkLoadAll, and world.c's worldSetChunkAll — the
// function app/worker.c's workerInstall calls once per chunk of every column the generator
// hands the main thread — now calls the new pair.
//
// The old pair walked the 4096-cell buffer three times: once in chunkFormForAll to size the
// budget claim, once in chunkLoadAll to rebuild the same palette, and once more to pack the
// nibbles, that third walk searching the palette linearly (up to 16 compares) for every
// cell. Measured on the host over a real 7x7-column streaming pass, paired and interleaved
// four rounds each: 4513.7 / 4733.8 / 4121.9 / 4201.0 us per pass before against
// 1049.6 / 971.1 / 1107.4 / 1061.1 us after — 15.05 -> 3.50 us per chunk, 4.2x.
//
// What this test defends is that the speed cost nothing:
//
//  * the form chosen is the same one, for 1, 2, 16 and 17 distinct ids;
//  * every cell reads back the id that was written — the arm that catches the packing loop
//    getting its nibble parity wrong, which is why every buffer here alternates ids so
//    adjacent cells DIFFER (a buffer of runs would survive a swapped pair);
//  * the encoded bytes are identical to chunkLoadAll's, which pins the palette's first-seen
//    ORDER as well as its contents — a reordered palette decodes to the same blocks and
//    would otherwise pass every check above it;
//  * ids at the top of the byte range (0xF0..) work, because slot_of is indexed by BlockId
//    and must be the full 256 entries wide. Sizing it BLOCK_COUNT is the exact bug
//    world/visgraph.c's openTable() and world/light.c's anyLuminance() each shipped with.
static void testChunkPlanAllMatchesLoadAll(void)
{
	static BlockId in[CHUNK_BLOCKS];
	static uint8_t enc_a[CHUNK_BLOCKS * 2 + 64];
	static uint8_t enc_b[CHUNK_BLOCKS * 2 + 64];

	const int     counts[4] = {1, 2, 16, 17};
	const BlockId bases[2]  = {1, 0xF0};   // core-range ids, and the top of the byte range

	for (int bi = 0; bi < 2; bi++)
	for (int ci = 0; ci < 4; ci++) {
		const int n = counts[ci];
		// 0xF0..0xFF is exactly 16 ids, so the high-base family skips the 17-id case
		// rather than aliasing back down onto 0x00 and quietly testing 16 again.
		if ((int)bases[bi] + n - 1 > 0xFF) continue;

		for (int i = 0; i < CHUNK_BLOCKS; i++)
			in[i] = (BlockId)(bases[bi] + (i % n));

		ChunkPlan plan;
		const ChunkForm form = chunkPlanAll(in, &plan);
		CHECK(form == chunkFormForAll(in));
		CHECK(plan.count == (n <= 16 ? n : -1));

		Chunk* a = chunkAlloc(BLOCK_AIR);
		Chunk* b = chunkAlloc(BLOCK_AIR);
		CHECK(a != NULL);
		CHECK(b != NULL);
		if (!a || !b) { chunkFree(a); chunkFree(b); continue; }

		CHECK(chunkLoadPlanned(a, in, &plan));
		CHECK(chunkLoadAll(b, in));
		CHECK(chunkGetForm(a) == form);
		CHECK(chunkGetForm(b) == form);
		CHECK(chunkGetBytes(a) == chunkGetBytes(b));

		bool cells_a = true, cells_b = true;
		for (int i = 0; i < CHUNK_BLOCKS; i++) {
			if (chunkGet(a, i) != in[i]) cells_a = false;
			if (chunkGet(b, i) != in[i]) cells_b = false;
		}
		CHECK(cells_a);
		CHECK(cells_b);

		const size_t la = chunkEncode(a, enc_a, sizeof enc_a);
		const size_t lb = chunkEncode(b, enc_b, sizeof enc_b);
		CHECK(la > 0);
		CHECK(la == lb);
		CHECK(la == lb && memcmp(enc_a, enc_b, la) == 0);

		// First-seen palette order, stated directly rather than only through the encode.
		if (form == CHUNK_FORM_PALETTE4) {
			BlockId pal[16];
			uint8_t pal_n = 0;
			static uint8_t idx[CHUNK_BLOCKS];
			CHECK(chunkGetPalette4(a, pal, &pal_n, idx));
			CHECK(pal_n == (uint8_t)n);
			CHECK(pal[0] == in[0]);
			CHECK(pal[1] == in[1]);
			CHECK(idx[0] == 0);
			CHECK(idx[1] == 1);
		}

		chunkFree(a);
		chunkFree(b);
	}
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
	testChunkFormBytes();
	testChunkUniformNoArray();
	testChunkPromoteUniformToPalette();
	testChunkPromotePaletteToRaw();
	testChunkRoundTripEveryFormEveryCell();
	testChunkRunCopyMatchesAcrossForms();
	testWorldAccess();
	testWorldBytes();
	testColumnTableFull();
	testColumnRemove();
	testWorldSetBudgetExhausted();
	testChunkPromotionBudgetRefused();
	testScratch26();
	testScratchFloor();
	testMesher();
	testMesherTransparentSplit();
	testMesherOpaqueBehindTransparent();
	testMesherFaceBuckets();
	testMesherAO();
	testMesherFullCubeBytesUnchanged();
	testBlockShapeRegistry();
	testMesherCrossShape();
	testMesherCoreWaterAndTallGrass();
	testMesherGreedyMerge();
	testMesherCrossNeverMerges();
	testMesherExposureFilter();
	testCrossShapeCollisionAndRaycast();
	testRemeshList();
	testIncrementalRemesh();
	testDirtyQueue();
	testJobQueue();
	testMeshqRefusalIsRecoverable();
	testHandbuiltHeight();
	testHandbuiltFillShape();
	testHandbuiltWalkable();
	testColumnDirtyFlag();
	testEditPersistence();
	testControlFeel();
	testAllAirMeshesToNothing();
	testAllOpaqueMeshesToNothing();
	testRng();
	testNoiseDeterminism();
	testWorldgenTerrain();
	testWorldgenDeterminism();
	testWorldgenBiome();
	testWorldgenTrees();
	testWorldgenCaves();
	testWorldgenMemoryProjection();
	testWorldgenLegacyByteIdentity();
	testWorldgenLegacySandyWideSweep();
	testWorldgenBiomes();
	testGenVersionContract();
	testDensityBiomeTable();
	testDensityField();
	testDensityDistribution();
	testWorldgenWaterAndGrass();
	testWorldgenBiomeSurface();
	testWorldgenPhase3Flora();
	testRaycast();
	testBodyBlocked();
	testCeilingCollision();
	testPhysics();
	testStepUpRefusals();
	testBuoyancy();
	testSwimming();
	testSurfaceSwim();
#ifndef __3DS__
	testPlayerWiresSwimming();
#endif
	testVisPairIndex();
	testVisConnectivity();
	testVisTableFollowsRegistry();
	testVisWalk();
	testVisWalkNeverHidesOpenSky();
	testRenderDist();
	testLightStorage();
	testLightSkySunColumns();
	testLightSkyOverhang();
	testLightBlockChannel();
	testLightBlockChannelFromDynamicId();
	testLightIncrementalEqualsFull();
	testLightTransparentTopStrip();
	testRelightQueue();
	testTickClock();
	testSpawnStandingY();
	testMesherO3DSParity();
	testChunkCodec();
	testChunkCodecPaletteUnify();
	testChunkPlanAllMatchesLoadAll();
#ifndef __3DS__
	testGenverStampPath();
	testRegionRoundTrip();
	testRegionPowerCut();
	testRegionTornDirectory();
	testRegionCache();
	testRegionCompactPreservesColumns();
	testRegionCompactPowerCut();
	testRegionCompactCache();
	testRegionCompactKeepsPowerCutFallback();
	testGenVersionSidecar();
	testRmTree(testWorldDir());   // the per-process directory those eight worked in
#endif

	// Every allocation the world made must have been given back.
	CHECK(budgetUsed() == 0);

	testBudget();

	// ── Check-count guard ─────────────────────────────────────────────────────────────
	//
	// This suite reports s_checks as well as s_fails, so a shrinking total is at least
	// VISIBLE here — unlike the six suites 0258188 and 1a56469 had to fix, which printed
	// failures alone. Visible is not the same as caught: the number is printed into a
	// harness log nobody diffs, and a change that stops a check from running rather than
	// failing it still leaves an exit code of 0. This makes it red.
	//
	// **Recompute the number, never paste it off the PASS line.** Work out how many checks
	// your change adds or removes and add that delta to the pin. If the printed count and
	// your recomputed one disagree, that disagreement is the bug report — some other check
	// stopped running, and pasting the observed number is what destroys that evidence.
	//
	// The pin is the count BEFORE this guard, so the PASS line prints one higher. Blind
	// pasting therefore lands a red rather than a false green.
	//
	// 5262 was arrived at that way and then confirmed against the run, which is the order
	// that matters: the tree was at 5184 before v1.8.3 Phase 2, and the three tests this
	// phase adds contribute 22 + 35 + 21 = 78 CHECKs. CHECK_QUIET is invisible here — it
	// counts only failures, deliberately — so the 21,609-position sweep in
	// testWorldgenLegacySandyWideSweep costs seven, not sixty-four thousand. 5184 + 78 =
	// 5262, and 5262 is what the run printed.
	//
	// v1.8.3, genverStampPath truncation fix: testGenverStampPath drives that helper over
	// 15 cap/dir-length pairs with 4 CHECKs each, so 5262 + 60 = 5322. Recomputed from the
	// table's length, then confirmed against the run -- that order, not the other one.
	//
	// v1.8.3 Phase 3 (snow, ice, cactus, dead bush, fern): 5322 + 38 = 5360, and the 38 was
	// counted off the source before the suite was run, in the order this comment demands.
	//
	//    +7  testWorldgenWaterAndGrass gains ice_off_line, ice_wrong_biome and
	//        ice_no_water_under inside the loop over its TWO areas (3 x 2 = 6), plus one
	//        CHECK(total_ice > 0) after it.
	//   +31  testWorldgenPhase3Flora, new: 1 worldgenInit + 15 "== 0" placement invariants
	//        + 6 non-vacuity counters + 1 cactus stacking + 2 for the fern rate ordering
	//        + 6 for the legacy sweep (worldgenInit, intruders, cells) over 2 seeds.
	//
	// Predicted 5360 and the run printed 5360, which is the half that matters: an intermediate
	// run of the same work predicted 5269 and printed 5269 as well, so the two agreements are
	// independent. The CHECK_QUIET in the new test's 121-column generation loop and in its
	// legacy 3 x 3 loops contributes nothing while it passes, per the rule stated above.
	//
	// **Host only, and this is a real gap.** Nine tests above are inside `#ifndef __3DS__`
	// and the console total is a different number, which cannot be measured right now: the
	// devkitPro build is red tree-wide for an unrelated reason (source/world/block.h has
	// drifted from its vendored server copy). A console-side pin needs its own measured
	// literal and is not guessable from this one.
#ifndef __3DS__
	{
		const int ran = s_checks;
		if (ran != 5360)
			printf("\nCHECK-COUNT GUARD: %d checks ran, %d expected.\n"
			       "  %s\n"
			       "  This is NOT an ordinary assertion failure.\n"
			       "  Read the comment above this guard in world/world_test.c before"
			       " touching the pinned number.\n",
			       ran, 5360,
			       ran < 5360
			           ? "Checks went MISSING: checks that should have run never ran at all."
			           : "Extra checks appeared: either you added checks and did not update"
			             " the pin, or something is emitting checks it should not.");
		CHECK(ran == 5360);
	}
#endif

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
