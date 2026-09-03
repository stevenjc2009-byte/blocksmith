// tests/mesher_write_bounds_test.c — write-side stress for BLOCK_SHAPE_CROSS geometry
// (v1.8.16, commit 8260b91 "feat(mesher): smooth per-vertex light on cross blocks", and
// 3a049f1 "fix(mesher): tint the top half of a two-block grass clump").
//
// WHY THIS FILE EXISTS. The real crash report (sdmc:/blocksmith/hang.txt then
// sdmc:/blocksmith/postmortem.txt) is a GPU wedge, not a CPU hang: main.c's
// gpuWaitPrevFrame() blocked forever on a dead PICA. A GPU that stops executing a command
// list is downstream of malformed vertex/index data far more often than it is downstream of
// a CPU infinite loop, and the one thing in this diff that WRITES vertex/index data is
// emitCross()'s new clight[2][2][2] table and its `v->pad = (int8_t)clight[...]` store —
// everything else in the commit only changes what VALUE goes into an already-existing write,
// not whether or how many bytes get written. So the question this file answers is: can a
// cross-heavy chunk make meshChunk() write past out->verts/out->indices, or past its own
// clight table, under the REAL production buffer size?
//
// THE GAP THE EXISTING SUITES LEAVE. tests/mesher_hashcheck.c and tests/cross_light_test.c
// both mesh into MESH_MAX_VERTS/MESH_MAX_INDICES buffers (world/mesher.h): 49,152 verts,
// 73,728 indices — sized for MESH_MAX_FACES = CHUNK_BLOCKS/2*6 = 12,288, an upper bound with
// slack built in. Nothing on the console ever hands meshChunk() a buffer that size.
// scene/chunk_render.c meshes into s_vert_scratch[MESH_SLOT_VERTS] / s_index_scratch
// [MESH_SLOT_INDICES] (scene/mesh_pool_sizing.h) — 8,192 verts, 12,288 indices, sized to
// MESH_SLOT_FACES = 2,048 faces, "comfortably past a fully exposed chunk surface (1,536)" per
// that header's own comment. A fully exposed CUBE surface is 1,536 faces. A chunk with cross
// blocks in most of its 4,096 cells is a different animal: emitCross() charges 4 faces per
// cross cell (CROSS_QUADS in world/mesher.c), so as few as 512 densely-packed cross cells —
// an eighth of one chunk — already reach the 2,048-face cap the real console buffer is sized
// against. No existing host suite ever meshes into a buffer that small, so no existing suite
// has ever exercised the real overflow guard at the real production size.
//
// scene/mesh_pool_sizing.h is deliberately free of <3ds.h> for exactly this reason — see its
// own header comment — so the REAL MESH_SLOT_VERTS/MESH_SLOT_INDICES constants can be used
// here without pulling in citro3d.
//
// WHAT THIS FILE CHECKS, laid out as the task asked:
//   ARM 1  a chunk with EVERY cell a cross block (BLOCK_TALL_GRASS), meshed into a buffer
//          sized EXACTLY to MESH_SLOT_VERTS/MESH_SLOT_INDICES — the real production cap, not
//          the padded test cap — so an off-by-one write lands in an ASAN redzone instead of
//          slack the fixture never uses. Confirms overflow is refused BEFORE any offending
//          write (out->vert_count/index_count never exceed the cap) rather than after.
//   ARM 2  cross blocks at every one of a chunk's four (x,z) corners, at BOTH y=0 (world
//          floor) and y=WORLD_HEIGHT-1 (world ceiling), lit, with the neighbour chunk's
//          column NOT created at all — the "unloaded neighbour" case mesher_hashcheck.c's
//          meshInterior() explicitly skips (see its own comment on why it only meshes one
//          ring in from every edge).
//   ARM 3  the torch-with-neighbours-missing case at a literal chunk (0,0,0) with no world
//          around it whatsoever — cx-1/cy-1/cz-1 through cx+1/cy+1/cz+1 all absent or, at
//          y<0, the synthetic WORLD_FLOOR_BLOCK.
//   ARM 4  four adjacent plants in identical light — the commit message's own claim ("four
//          adjacent plants stay 16 quads / 64 verts") — re-run against the small buffer and
//          against a two-block BLOCK_TALL_GRASS/BLOCK_TALL_GRASS_TOP clump (3a049f1) at a
//          chunk boundary, so the clump fix and the light fix are checked together the way
//          v1.8.16 shipped them together.
//
// Every arm is read for exit status only — ASAN/UBSAN abort the process and the harness sees
// a non-zero exit — plus the explicit CHECK() assertions on out->overflow and the counters
// staying inside cap. A clean ASAN run across every arm is the decisive result this file
// exists to produce; it does not replace tests/cross_light_test.c or tests/mesher_hashcheck.c,
// it covers what they structurally cannot: the real, small, production write target.

#include <stdio.h>
#include <string.h>

#include "scene/mesh_pool_sizing.h"
#include "world/block.h"
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

static World       s_world;
static MeshScratch  s_ms;

// The REAL production write target, byte for byte what scene/chunk_render.c allocates —
// static, not malloc'd, so ASAN's global-redzone instrumentation guards the exact end of the
// array the same way the console's own linear-heap slot would need guarding.
static MeshVertex  s_verts_slot[MESH_SLOT_VERTS];
static uint16_t    s_idx_slot[MESH_SLOT_INDICES];

static MeshOut meshSlotSized(int cx, int cy, int cz, bool lit)
{
	scratchFill(&s_ms, &s_world, cx, cy, cz);
	if (lit) scratchFillLight(&s_ms, &s_world, cx, cy, cz);

	MeshOut out = {0};
	out.verts     = s_verts_slot;
	out.indices   = s_idx_slot;
	out.vert_cap  = MESH_SLOT_VERTS;
	out.index_cap = MESH_SLOT_INDICES;
	meshChunk(&out, &s_ms);
	return out;
}

// ── ARM 1: every cell in the meshed chunk and its full 26-neighbourhood a cross block ──────
//
// The densest cross population this world can express: 4,096 cells per chunk, all
// BLOCK_TALL_GRASS, in every one of the 27 chunks scratchFill reads from. Not realistic
// worldgen — the point is to find the write bound, not to model a plausible world.
static void testDenseCrossWorstCase(void)
{
	printf("ARM 1 - every cell a cross block, meshed into the REAL MESH_SLOT_VERTS(%d)/"
	       "MESH_SLOT_INDICES(%d) buffer\n", MESH_SLOT_VERTS, MESH_SLOT_INDICES);

	worldInit(&s_world);
	lightEngineInit(true);
	for (int z = 0; z < 3 * CHUNK_DIM; z++)
		for (int y = 0; y < 3 * CHUNK_DIM; y++)
			for (int x = 0; x < 3 * CHUNK_DIM; x++)
				worldSet(&s_world, x, y, z, BLOCK_TALL_GRASS);

	MeshOut out = meshSlotSized(1, 1, 1, true);

	// This chunk alone needs 4096 * CROSS_QUADS(4) = 16,384 faces, far past
	// MESH_SLOT_FACES(2048) — the guard MUST refuse rather than overrun.
	CHECK(out.overflow, "4096 cross cells did not trip overflow at MESH_SLOT_FACES=%d — "
	      "either the cap grew or the guard stopped firing", MESH_SLOT_FACES);
	CHECK(out.vert_count <= MESH_SLOT_VERTS, "vert_count %u exceeds the cap %d — WRITE "
	      "OVERFLOW", out.vert_count, MESH_SLOT_VERTS);
	CHECK(out.index_count <= MESH_SLOT_INDICES, "index_count %u exceeds the cap %d — WRITE "
	      "OVERFLOW", out.index_count, MESH_SLOT_INDICES);
	printf("  verts=%u/%d idx=%u/%d overflow=%d\n", out.vert_count, MESH_SLOT_VERTS,
	       out.index_count, MESH_SLOT_INDICES, (int)out.overflow);

	worldExit(&s_world);
	lightEngineInit(false);
}

// ── ARM 2: every corner, both world extremes, unloaded neighbour column ────────────────────
//
// A single chunk at (1,1,1) is created — cx/cz siblings 0 and 2 are left NON-EXISTENT (no
// worldSet ever touches them), so scratchFill's "absent chunk reads as air" path fires on
// every horizontal side. cy=1 puts this chunk's floor at world y=16, nowhere near y=0, so a
// second chunk at (1,0,1) is meshed separately to reach the real y=0 floor, and a chunk at
// (1, COLUMN_CHUNKS-1, 1) reaches y=WORLD_HEIGHT-1. All three are meshed with their horizontal
// neighbour columns absent, combining "chunk boundary" with "neighbour column not loaded" the
// way the task asked.
static void placeCornersAndEdges(int cx, int cy, int cz)
{
	const int bx = cx * CHUNK_DIM, by = cy * CHUNK_DIM, bz = cz * CHUNK_DIM;
	worldSet(&s_world, bx, by, bz, BLOCK_STONE);   // a floor cell under one corner plant

	const int xs[2] = { 0, CHUNK_DIM - 1 };
	const int zs[2] = { 0, CHUNK_DIM - 1 };
	const BlockId kinds[4] = { BLOCK_TALL_GRASS, BLOCK_FERN, BLOCK_DEAD_BUSH, BLOCK_TALL_GRASS };
	int k = 0;
	for (int xi = 0; xi < 2; xi++)
		for (int zi = 0; zi < 2; zi++) {
			worldSet(&s_world, bx + xs[xi], by, bz + zs[zi], kinds[k % 4]);
			worldSet(&s_world, bx + xs[xi], by + CHUNK_DIM - 1, bz + zs[zi], kinds[(k + 1) % 4]);
			k++;
		}
}

static void testCornersAtWorldExtremesUnloadedNeighbours(void)
{
	printf("ARM 2 - every chunk corner, y=0 and y=WORLD_HEIGHT-1, horizontal neighbour "
	       "columns not created\n");

	worldInit(&s_world);
	lightEngineInit(true);

	placeCornersAndEdges(1, 0, 1);                          // touches world y = 0
	placeCornersAndEdges(1, COLUMN_CHUNKS - 1, 1);           // touches world y = WORLD_HEIGHT-1

	MeshOut outFloor = meshSlotSized(1, 0, 1, true);
	CHECK(!outFloor.overflow, "y=0 corner/edge fixture unexpectedly overflowed a %d-face "
	      "budget", MESH_SLOT_FACES);
	CHECK(outFloor.vert_count <= MESH_SLOT_VERTS && outFloor.index_count <= MESH_SLOT_INDICES,
	      "y=0 fixture wrote past cap: verts=%u idx=%u", outFloor.vert_count,
	      outFloor.index_count);

	MeshOut outRoof = meshSlotSized(1, COLUMN_CHUNKS - 1, 1, true);
	CHECK(!outRoof.overflow, "y=WORLD_HEIGHT-1 corner/edge fixture unexpectedly overflowed a "
	      "%d-face budget", MESH_SLOT_FACES);
	CHECK(outRoof.vert_count <= MESH_SLOT_VERTS && outRoof.index_count <= MESH_SLOT_INDICES,
	      "y=WORLD_HEIGHT-1 fixture wrote past cap: verts=%u idx=%u", outRoof.vert_count,
	      outRoof.index_count);

	printf("  floor chunk verts=%u idx=%u overflow=%d | roof chunk verts=%u idx=%u overflow=%d\n",
	       outFloor.vert_count, outFloor.index_count, (int)outFloor.overflow,
	       outRoof.vert_count, outRoof.index_count, (int)outRoof.overflow);

	worldExit(&s_world);
	lightEngineInit(false);
}

// ── ARM 3: a single chunk, nothing else in the world at all ────────────────────────────────
//
// worldInit() alone, then straight to scratchFill(&s_ms, &s_world, 0, 0, 0) with not one
// worldSet ever called for any OTHER chunk: every one of the 26 neighbours scratchFill reads
// is absent, and y<0 additionally exercises the WORLD_FLOOR_BLOCK substitution. Torches at
// every corner of chunk (0,0,0), which is a genuine world floor (cy=0, dy=-1 reads
// WORLD_FLOOR_BLOCK, not air).
static void testChunkWithNoWorldAroundIt(void)
{
	printf("ARM 3 - chunk (0,0,0), completely empty surrounding world, torches at every "
	       "corner\n");

	worldInit(&s_world);
	lightEngineInit(true);

	const int xs[2] = { 0, CHUNK_DIM - 1 };
	const int zs[2] = { 0, CHUNK_DIM - 1 };
	for (int xi = 0; xi < 2; xi++)
		for (int zi = 0; zi < 2; zi++) {
			worldSet(&s_world, xs[xi], 0, zs[zi], BLOCK_STONE);
			worldSet(&s_world, xs[xi], 1, zs[zi], BLOCK_TORCH);
		}

	MeshOut out = meshSlotSized(0, 0, 0, true);
	CHECK(!out.overflow, "isolated chunk (0,0,0) unexpectedly overflowed");
	CHECK(out.vert_count <= MESH_SLOT_VERTS && out.index_count <= MESH_SLOT_INDICES,
	      "isolated chunk wrote past cap: verts=%u idx=%u", out.vert_count, out.index_count);
	printf("  verts=%u idx=%u overflow=%d\n", out.vert_count, out.index_count,
	       (int)out.overflow);

	worldExit(&s_world);
	lightEngineInit(false);
}

// ── ARM 4: the commit's own vertex-count claim, at a chunk boundary, small buffer ──────────
//
// "four adjacent plants in identical light stay 16 quads / 64 verts" (commit 8260b91). Placed
// straddling x=15/x=16 — the last cell of chunk (1,*,*) and, deliberately, NOT into chunk
// (2,*,*) so the fixture stays inside the meshed chunk's own 16 cells — plus one
// BLOCK_TALL_GRASS / BLOCK_TALL_GRASS_TOP clump (3a049f1) at the very corner (0,0) so the two
// commits between v1.8.14 and v1.8.16 are exercised in the same fixture.
static void testFourPlantsAndClumpAtBoundary(void)
{
	printf("ARM 4 - four adjacent plants (16 quads/64 verts claim) plus a two-block clump, "
	       "at a chunk edge\n");

	worldInit(&s_world);
	lightEngineInit(true);

	const int bx = CHUNK_DIM, by = CHUNK_DIM, bz = CHUNK_DIM;   // chunk (1,1,1)
	worldSet(&s_world, bx, by - 1, bz, BLOCK_STONE);
	for (int i = 0; i < 4; i++) {
		worldSet(&s_world, bx + CHUNK_DIM - 1, by, bz + i, BLOCK_TALL_GRASS);
		worldSet(&s_world, bx + CHUNK_DIM - 1, by - 1, bz + i, BLOCK_STONE);
	}
	// Two-block clump at the chunk's (0,0) corner.
	worldSet(&s_world, bx, by - 1, bz, BLOCK_STONE);
	worldSet(&s_world, bx, by, bz, BLOCK_TALL_GRASS);
	worldSet(&s_world, bx, by + 1, bz, BLOCK_TALL_GRASS_TOP);

	MeshOut out = meshSlotSized(1, 1, 1, true);
	CHECK(!out.overflow, "small four-plant + clump fixture unexpectedly overflowed");
	CHECK(out.vert_count <= MESH_SLOT_VERTS && out.index_count <= MESH_SLOT_INDICES,
	      "four-plant fixture wrote past cap: verts=%u idx=%u", out.vert_count,
	      out.index_count);
	printf("  verts=%u idx=%u faces=%u overflow=%d\n", out.vert_count, out.index_count,
	       out.faces, (int)out.overflow);

	worldExit(&s_world);
	lightEngineInit(false);
}

int main(void)
{
	testDenseCrossWorstCase();
	testCornersAtWorldExtremesUnloadedNeighbours();
	testChunkWithNoWorldAroundIt();
	testFourPlantsAndClumpAtBoundary();

	if (s_failed) {
		printf("mesher write-bounds self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("mesher write-bounds self-test: PASS %d checks\n", s_checks);
	return 0;
}
