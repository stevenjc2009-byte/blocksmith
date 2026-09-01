// Mesher geometry-equality guard. Links the real source/world modules and asserts that
// meshChunk() still emits byte-identical buffers across a wide spread of chunk shapes.
//
// Written during v1.8.6 as a throwaway scratchpad harness, to prove that one specific change
// (packing s_cell_solid/s_cell_occl/s_cell_draw into one s_cell_flags array) altered no
// geometry. Promoted into tests/ and wired into tools/run_host_tests.sh afterwards, because
// the property it checks is permanent rather than specific to that change: the mesher may get
// faster, it may not change what it draws.
//
// Promoted rather than discarded for a reason this project has already paid for once. The
// standing pin in world/world_test.c is FOUR fixtures, and a four-sample gate over a
// distribution only tells you that four draws passed — a green check over a single seeded
// sample has previously hidden a 28% failure rate elsewhere in this codebase. This harness
// covers 408 chunks across 12 seeds and 5 world kinds, so a mesher bug that happens to spare
// those four fixtures still cannot get past it.
//
// What it proves: meshChunk() produces byte-identical MeshOut buffers before and after a change
// to source/world/mesher.c, over several hundred chunks spanning several deterministic "worlds"
// (buried stone, surface terrain with water, a checkerboard worst case, sparse cross-shape
// scatter, and a leaf canopy). Run once against the baseline mesher.c and once against the
// changed one, with the SAME binary logic — only mesher.c differs between the two builds — and
// diff the two logs. Any line that differs is a chunk whose emitted geometry changed.
//
// Also runs a repeated-call timing pass over a fixed subset, gated behind --timeit, so the same
// binary answers both questions without the hash log picking up timing noise.
//
// Four of the "worlds" below (random-core-mix, stone-beside-leaves, all-leaves, all-air) are
// deliberately the exact fixtures world/world_test.c's testMesherFullCubeBytesUnchanged() pins,
// reproduced by hand here (not by including that file) so this harness can assert its own
// meshChunk/meshHash wiring against the four numbers already pinned in the real suite before
// trusting any new number it prints.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "world/block.h"
#include "world/mesh_vertex.h"
#include "world/mesher.h"
#include "world/scratch.h"
#include "world/world.h"

// ---- deterministic PRNG -----------------------------------------------------------------
// Same LCG world_test.c's pinned fixture uses (multiplier/increment from Numerical Recipes),
// so seeding with the same constant reproduces the same sequence for the self-check below.
static uint32_t lcgNext(uint32_t* s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s;
}

// A context-free hash of (seed, wx, wz), for a deterministic "white noise" height field. No
// spatial coherence is fine here — the harness wants varied buried/exposed cell patterns, not
// a good-looking landscape.
static uint32_t cellHash(uint32_t seed, int a, int b)
{
	uint32_t h = seed;
	h ^= (uint32_t)a * 374761393u;
	h ^= (uint32_t)b * 668265263u;
	h  = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return h;
}

// ---- the same FNV-1a mesh hash world_test.c uses, reproduced by hand ---------------------
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

static World       s_world;
static MeshScratch  s_scratch;
static MeshVertex   s_verts[MESH_MAX_VERTS];
static uint16_t     s_indices[MESH_MAX_INDICES];

static long g_meshed = 0;

// ---- self-check: reproduce world_test.c's four pinned fixtures by hand -------------------
// If these don't match, the harness itself is wired wrong (wrong BLOCK_COUNT, wrong hash
// routine, wrong calling convention) and nothing else it prints can be trusted.
static void selfCheckPinnedHashes(void)
{
	MeshOut mo = {0};
	mo.verts     = s_verts;
	mo.indices   = s_indices;
	mo.vert_cap  = MESH_MAX_VERTS;
	mo.index_cap = MESH_MAX_INDICES;

	int fails = 0;

	// Case 1: random-core-mix.
	worldInit(&s_world);
	{
		uint32_t rng = 12345u;
		for (int y = 0; y < CHUNK_DIM; y++)
			for (int z = 0; z < CHUNK_DIM; z++)
				for (int x = 0; x < CHUNK_DIM; x++) {
					rng = lcgNext(&rng);
					const BlockId id = (BlockId)((rng >> 16) % BLOCK_COUNT);
					worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y, CHUNK_DIM + z, id);
				}
	}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&mo, &s_scratch);
	if (mo.vert_count != 23676 || mo.index_count != 35514 || mo.faces != 5919 ||
	    meshHash(&mo) != 0xcf4cf785u) {
		fprintf(stderr, "SELF-CHECK FAIL case1 random-core-mix: verts=%u idx=%u faces=%u hash=%08x\n",
		        mo.vert_count, mo.index_count, mo.faces, meshHash(&mo));
		fails++;
	}
	worldExit(&s_world);

	// Case 2: stone-beside-leaves.
	worldInit(&s_world);
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y, CHUNK_DIM + z, BLOCK_STONE);
				worldSet(&s_world, 2 * CHUNK_DIM + x, CHUNK_DIM + y, CHUNK_DIM + z, BLOCK_LEAVES);
			}
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&mo, &s_scratch);
	if (mo.vert_count != 936 || mo.index_count != 1404 || mo.faces != 234 ||
	    meshHash(&mo) != 0x6a24313eu) {
		fprintf(stderr, "SELF-CHECK FAIL case2 stone-beside-leaves: verts=%u idx=%u faces=%u hash=%08x\n",
		        mo.vert_count, mo.index_count, mo.faces, meshHash(&mo));
		fails++;
	}
	worldExit(&s_world);

	// Case 3: all-leaves.
	worldInit(&s_world);
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				worldSet(&s_world, CHUNK_DIM + x, CHUNK_DIM + y, CHUNK_DIM + z, BLOCK_LEAVES);
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&mo, &s_scratch);
	if (mo.vert_count != 768 || mo.index_count != 1152 || mo.faces != 192 ||
	    meshHash(&mo) != 0xef2164b8u) {
		fprintf(stderr, "SELF-CHECK FAIL case3 all-leaves: verts=%u idx=%u faces=%u hash=%08x\n",
		        mo.vert_count, mo.index_count, mo.faces, meshHash(&mo));
		fails++;
	}
	worldExit(&s_world);

	// Case 4: all-air.
	worldInit(&s_world);
	scratchFill(&s_scratch, &s_world, 1, 1, 1);
	meshChunk(&mo, &s_scratch);
	if (mo.vert_count != 0 || mo.index_count != 0 || mo.faces != 0 || meshHash(&mo) != 0xc655ff85u) {
		fprintf(stderr, "SELF-CHECK FAIL case4 all-air: verts=%u idx=%u faces=%u hash=%08x\n",
		        mo.vert_count, mo.index_count, mo.faces, meshHash(&mo));
		fails++;
	}
	worldExit(&s_world);

	if (fails) {
		fprintf(stderr, "selfCheckPinnedHashes: %d of 4 pinned fixtures did NOT reproduce - "
		                 "harness wiring is suspect, refusing to trust anything else it prints\n",
		        fails);
		exit(2);
	}
	fprintf(stderr, "selfCheckPinnedHashes: OK, all 4 world_test.c fixtures reproduced byte-for-byte\n");
}

// ---- world builders ------------------------------------------------------------------------

// Kind A: deep, fully-buried alternating stone/dirt slabs across a 5x3x5 chunk block, so every
// interior chunk meshed is completely enclosed on all six sides by more of the same. This is
// the exact case the buried-cube skip exists for: no exposed face anywhere, so meshChunk should
// emit nothing at all for any interior chunk.
#define KA_CX 5
#define KA_CY 3
#define KA_CZ 5
static void buildBuriedWorld(uint32_t seed)
{
	worldInit(&s_world);
	const int band = 2 + (int)(seed % 5);   // varies the slab thickness per seed, still fully buried
	for (int y = 0; y < KA_CY * CHUNK_DIM; y++) {
		const BlockId id = (BlockId)((y / band) % 2 ? BLOCK_STONE : BLOCK_DIRT);
		for (int z = 0; z < KA_CZ * CHUNK_DIM; z++)
			for (int x = 0; x < KA_CX * CHUNK_DIM; x++)
				worldSet(&s_world, x, y, z, id);
	}
}

// Kind B: surface terrain with a white-noise height field, grass/dirt/stone layering, sparse
// tall grass on top and shallow water pools in the low columns at varying flow levels. This is
// the kind that exercises AO, the transparent same-material cull, greedy merging AND dropBuild
// all in the same chunk.
#define KB_CX 6
#define KB_CY 3
#define KB_CZ 6
static void buildSurfaceWorld(uint32_t seed)
{
	worldInit(&s_world);
	for (int z = 0; z < KB_CZ * CHUNK_DIM; z++) {
		for (int x = 0; x < KB_CX * CHUNK_DIM; x++) {
			const int h = 10 + (int)(cellHash(seed, x, z) % 13);   // 10..22
			for (int y = 0; y < h - 1 && y < KB_CY * CHUNK_DIM; y++)
				worldSet(&s_world, x, y, z, BLOCK_STONE);
			if (h - 1 >= 0 && h - 1 < KB_CY * CHUNK_DIM)
				worldSet(&s_world, x, h - 1, z, BLOCK_DIRT);
			if (h < KB_CY * CHUNK_DIM)
				worldSet(&s_world, x, h, z, BLOCK_GRASS);

			// Sparse tall grass on top, about 1 in 6 columns.
			if (h + 1 < KB_CY * CHUNK_DIM && cellHash(seed ^ 0x9e3779b9u, x, z) % 6 == 0)
				worldSet(&s_world, x, h + 1, z, BLOCK_TALL_GRASS);

			// Low columns (h < 13) get flooded up to y=12 with water, so the flood has real
			// stone walls on its sides from neighbouring taller columns.
			if (h < 13) {
				for (int y = h; y <= 12 && y < KB_CY * CHUNK_DIM; y++)
					worldSet(&s_world, x, y, z, BLOCK_WATER);
			}
		}
	}
}

// After scratchFill, hand-fills the water band exactly the way world/water.c's waterFillScratch
// would: a level 1..7 for a fraction of the water cells (flow), 0 (source) for the rest. This
// harness has no link to world/water.c (deliberately — see the file header), so it plays the
// same role water_mesh_test.c's msSet() helper does: writes the band directly, which is exactly
// what scratch.h's own contract says a caller may do.
static void applyWaterBand(uint32_t seed)
{
	s_scratch.water_any = false;
	for (int i = 0; i < SCRATCH_BLOCKS; i++) {
		s_scratch.water[i] = 0;
		if (s_scratch.blocks[i] != (BlockId)BLOCK_WATER) continue;
		if (cellHash(seed ^ 0x1234567u, i, 0) % 2 == 0) continue;   // half stay sources
		const uint8_t lvl = (uint8_t)(1 + cellHash(seed ^ 0x7654321u, i, 1) % 7);   // 1..7
		s_scratch.water[i] = lvl;
		s_scratch.water_any = true;
	}
}

// Kind C: an exact 3D checkerboard over a smaller block of chunks — the documented
// MESH_MAX_FACES worst case (mesher.h) and a stress case for the merge logic, since no two
// same-id cells are ever adjacent along any u axis.
#define KC_CX 3
#define KC_CY 3
#define KC_CZ 3
static void buildCheckerboardWorld(uint32_t seed)
{
	worldInit(&s_world);
	const int phase = (int)(seed & 1);   // shifts which parity is solid, still an exact checkerboard
	for (int y = 0; y < KC_CY * CHUNK_DIM; y++)
		for (int z = 0; z < KC_CZ * CHUNK_DIM; z++)
			for (int x = 0; x < KC_CX * CHUNK_DIM; x++)
				if (((x + y + z) & 1) == phase)
					worldSet(&s_world, x, y, z, BLOCK_STONE);
}

// Kind D: a thin solid floor with sparse cross-shaped plants scattered above it — heavy
// exercise of emitCross and the deferred pass, light on the opaque one.
#define KD_CX 4
#define KD_CY 3
#define KD_CZ 4
static void buildCrossScatterWorld(uint32_t seed)
{
	worldInit(&s_world);
	for (int z = 0; z < KD_CZ * CHUNK_DIM; z++) {
		for (int x = 0; x < KD_CX * CHUNK_DIM; x++) {
			worldSet(&s_world, x, 0, z, BLOCK_STONE);
			for (int y = 1; y < KD_CY * CHUNK_DIM; y++) {
				const uint32_t r = cellHash(seed, x, y * 131 + z);
				BlockId id = BLOCK_AIR;
				switch (r % 5) {
				case 0: id = BLOCK_TALL_GRASS; break;
				case 1: id = BLOCK_FERN;       break;
				case 2: id = BLOCK_DEAD_BUSH;  break;
				default: break;   // stays air
				}
				if (id != BLOCK_AIR) worldSet(&s_world, x, y, z, id);
			}
		}
	}
}

// Kind E: a scatter of wood trunks each wrapped in a leaf canopy, floating in open air — the
// deferred same-material self-cull (a canopy hides its own interior faces and the trunk's)
// under randomised trunk placement instead of the single hand-placed pair the pinned fixture
// tests.
#define KE_CX 4
#define KE_CY 3
#define KE_CZ 4
static void buildCanopyWorld(uint32_t seed)
{
	worldInit(&s_world);
	const int W = KE_CX * CHUNK_DIM, D = KE_CZ * CHUNK_DIM;

	// meshInterior samples only cy=1 for a 3-chunk-tall (KE_CY) world, i.e. world y 16..31, and
	// only cx,cz in {1,2} of a 4-chunk-wide world, i.e. world x/z 16..47 - the middle two chunks,
	// one ring in from the edge. Trunk and canopy are placed to land ENTIRELY inside that band,
	// canopy radius included, or the one interior chunk sampled would see nothing and every
	// "canopy" row would silently degenerate to the same all-air fixture "buried" already covers.
	const int y_lo = CHUNK_DIM + 2, y_hi = 2 * CHUNK_DIM - 3;      // 18..29, inside cy=1
	const int xz_lo = CHUNK_DIM + 2, xz_hi = 3 * CHUNK_DIM - 3;    // 18..45, inside cx/cz {1,2}

	for (int i = 0; i < 10; i++) {
		const uint32_t r0 = cellHash(seed, (int)i * 17, 0);
		const uint32_t r1 = cellHash(seed, (int)i * 17, 1);
		const uint32_t r2 = cellHash(seed, (int)i * 17, 2);
		const int tx = xz_lo + (int)(r0 % (uint32_t)(xz_hi - xz_lo));
		const int tz = xz_lo + (int)(r1 % (uint32_t)(xz_hi - xz_lo));
		const int th = y_lo + 3 + (int)(r2 % (uint32_t)(y_hi - y_lo - 3));   // canopy centre
		for (int y = y_lo; y < th; y++)
			worldSet(&s_world, tx, y, tz, BLOCK_WOOD);
		for (int dy = -2; dy <= 2; dy++)
			for (int dz = -2; dz <= 2; dz++)
				for (int dx = -2; dx <= 2; dx++) {
					if (dx * dx + dy * dy + dz * dz > 6) continue;
					const int y = th + dy;
					if (y < y_lo || y > y_hi) continue;
					const int x = tx + dx, z = tz + dz;
					if (x < 0 || x >= W || z < 0 || z >= D) continue;
					if (x == tx && z == tz && y < th) continue;   // don't overwrite trunk
					worldSet(&s_world, x, y, z, BLOCK_LEAVES);
				}
	}
}

// ---- driving the meshing over every interior chunk of a built world ----------------------

static void meshInterior(const char* kind, uint32_t seed, int ncx, int ncy, int ncz, FILE* out)
{
	// Border chunks are skipped: scratchFill reads a chunk's 26 neighbours, and a chunk at the
	// edge of the filled region would read "absent" (air, or WORLD_FLOOR_BLOCK below y=0) on
	// its outer side, which is a real and correct case but not the one each kind is built to
	// exercise. One ring in from every edge keeps every neighbour read landing on real,
	// deliberately-placed content.
	for (int cy = 1; cy < ncy - 1; cy++)
		for (int cz = 1; cz < ncz - 1; cz++)
			for (int cx = 1; cx < ncx - 1; cx++) {
				scratchFill(&s_scratch, &s_world, cx, cy, cz);
				if (strcmp(kind, "surface") == 0) applyWaterBand(seed);
				MeshOut mo = {0};
				mo.verts     = s_verts;
				mo.indices   = s_indices;
				mo.vert_cap  = MESH_MAX_VERTS;
				mo.index_cap = MESH_MAX_INDICES;
				meshChunk(&mo, &s_scratch);
				fprintf(out, "%s seed=%u c=%d,%d,%d verts=%u idx=%u faces=%u opq_idx=%u opq_faces=%u ov=%d hash=%08x\n",
				        kind, seed, cx, cy, cz, mo.vert_count, mo.index_count, mo.faces,
				        mo.opaque_index_count, mo.opaque_faces, (int)mo.overflow, meshHash(&mo));
				g_meshed++;
			}
}

static void runAllWorlds(FILE* out, int nseeds)
{
	for (int s = 0; s < nseeds; s++) {
		const uint32_t seed = 1000u + (uint32_t)s * 7919u;

		buildBuriedWorld(seed);
		meshInterior("buried", seed, KA_CX, KA_CY, KA_CZ, out);
		worldExit(&s_world);

		buildSurfaceWorld(seed);
		meshInterior("surface", seed, KB_CX, KB_CY, KB_CZ, out);
		worldExit(&s_world);

		buildCheckerboardWorld(seed);
		meshInterior("checker", seed, KC_CX, KC_CY, KC_CZ, out);
		worldExit(&s_world);

		buildCrossScatterWorld(seed);
		meshInterior("cross", seed, KD_CX, KD_CY, KD_CZ, out);
		worldExit(&s_world);

		buildCanopyWorld(seed);
		meshInterior("canopy", seed, KE_CX, KE_CY, KE_CZ, out);
		worldExit(&s_world);
	}
}

// ---- timing pass ---------------------------------------------------------------------------
// Builds a fixed, modestly-sized set of scratches once (the "surface" kind, the one that touches
// every mesher code path — AO, merge, transparent cull, dropBuild) and re-meshes each of them
// REPS times, timing only the meshChunk() calls. Printed as total time and per-call average, so
// two runs of this binary (baseline mesher.c, changed mesher.c) can be compared as a ratio.
#define TIME_SEEDS 4
#define REPS       200

static void runTiming(void)
{
	MeshScratch* scratches = NULL;
	int cap = 0, n = 0;

	for (int s = 0; s < TIME_SEEDS; s++) {
		const uint32_t seed = 2000u + (uint32_t)s * 7919u;
		buildSurfaceWorld(seed);
		for (int cy = 1; cy < KB_CY - 1; cy++)
			for (int cz = 1; cz < KB_CZ - 1; cz++)
				for (int cx = 1; cx < KB_CX - 1; cx++) {
					if (n == cap) {
						cap = cap ? cap * 2 : 16;
						scratches = (MeshScratch*)realloc(scratches, sizeof(MeshScratch) * (size_t)cap);
					}
					scratchFill(&scratches[n], &s_world, cx, cy, cz);
					applyWaterBand(seed);
					// applyWaterBand wrote into s_scratch, not scratches[n] - copy the band over.
					memcpy(scratches[n].water, s_scratch.water, sizeof scratches[n].water);
					scratches[n].water_any = s_scratch.water_any;
					n++;
				}
		worldExit(&s_world);
	}

	fprintf(stderr, "timing: %d fixed scratches, %d reps each\n", n, REPS);

	MeshOut mo = {0};
	mo.verts     = s_verts;
	mo.indices   = s_indices;
	mo.vert_cap  = MESH_MAX_VERTS;
	mo.index_cap = MESH_MAX_INDICES;

	// One warm-up pass, untimed, so the tables planBuild() lazily constructs are already built
	// before the clock starts - meshChunk's first-ever call is not representative of steady state.
	for (int i = 0; i < n; i++) meshChunk(&mo, &scratches[i]);

	double best = 1e300;
	for (int rep = 0; rep < 5; rep++) {   // five outer passes, report the fastest as the floor
		const clock_t t0 = clock();
		for (int r = 0; r < REPS; r++)
			for (int i = 0; i < n; i++)
				meshChunk(&mo, &scratches[i]);
		const clock_t t1 = clock();
		const double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
		fprintf(stderr, "  pass %d: %.4f s for %d calls (%.1f ns/call)\n",
		        rep, secs, REPS * n, secs * 1e9 / (REPS * n));
		if (secs < best) best = secs;
	}
	printf("TIMING best_seconds=%.6f calls=%d ns_per_call=%.2f\n",
	       best, REPS * n, best * 1e9 / (REPS * n));

	free(scratches);
}

int main(int argc, char** argv)
{
	selfCheckPinnedHashes();

	if (argc > 1 && strcmp(argv[1], "--timeit") == 0) {
		runTiming();
		return 0;
	}

	const int nseeds = (argc > 1) ? atoi(argv[1]) : 12;
	runAllWorlds(stdout, nseeds);
	fprintf(stderr, "meshed %ld chunks over %d seeds x 5 kinds\n", g_meshed, nseeds);
	return 0;
}
