// Host self-test for world/scratch.c's scratchFillLight() — the mesh scratch's light band.
//
// v1.8.0 task 49h rewrote that function's walk. It made 11,664 out-of-line
// lightChannelSky/lightChannelBlock calls per chunk build on a tree that does not use -flto,
// of which 11,658 returned what the previous call had returned; it re-asked two loop-invariant
// y-range questions 5,832 times; and it rebuilt lightIndex's multiply chain per cell for an
// index that simply increments along a contiguous run. None of that is a behaviour claim, so
// none of it may CHANGE behaviour — and "the mesher still looks right" is not a check, because
// a wrong light byte comes out as slightly wrong shading on one face of one chunk.
//
// So this file is an identity test. It links the REAL source/world/scratch.c and runs it
// against a frozen verbatim transcript of the pre-49h implementation over a world built to
// reach every branch: lit columns, columns present but with no light attached yet (the
// streaming-in case that must read as full sky, not black), columns absent from the world
// entirely, and chunk y indices above and below the column so the two wy range branches fire.
// Every fill is compared byte for byte and folded into an FNV-1a hash that is printed, so the
// two arms are one number anyone can read.
//
// The reference is a copy on purpose and that is the correct half to copy: the code under test
// is the linked module, and sabotaging source/world/scratch.c turns this red.
#ifndef __3DS__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/light.h"
#include "world/scratch.h"
#include "world/world.h"

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

// Too big for a 32 KB console stack and too big for comfort here — one of each, like
// world_test.c does it.
static World       s_world;
static MeshScratch s_now;
static MeshScratch s_ref;
static LightQueue  s_queue;

// ── The frozen reference: scratch.c's scratchFillLight() as it stood before 49h ─────────

static void scratchFillLightRef(MeshScratch* s, const World* w, int cx, int cy, int cz)
{
	const Column* cols[3][3];
	for (int dz = -1; dz <= 1; dz++)
		for (int dx = -1; dx <= 1; dx++)
			cols[dz + 1][dx + 1] = worldColumn(w, cx + dx, cz + dz);

	for (int sy = 0; sy < SCRATCH_DIM; sy++) {
		const int wy = cy * CHUNK_DIM + sy - 1;

		for (int sz = 0; sz < SCRATCH_DIM; sz++) {
			const int wz = cz * CHUNK_DIM + sz - 1;
			const int ci_z = (wz >> 4) - cz + 1;

			for (int sx = 0; sx < SCRATCH_DIM; sx++) {
				const int wx = cx * CHUNK_DIM + sx - 1;
				const Column* c = cols[ci_z][(wx >> 4) - cx + 1];
				const uint8_t* sky = lightChannelSky(c);
				const uint8_t* blk = lightChannelBlock(c);

				uint8_t v;
				if (!sky) {
					v = 0xF0;
				} else {
					int sv, bv;
					if (wy >= WORLD_HEIGHT) { sv = 15; bv = 0; }
					else if (wy < 0)        { sv = 0;  bv = 0; }
					else {
						const int ci = lightIndex(wx & 15, wy, wz & 15);
						sv = lightNibble(sky, ci);
						bv = blk ? lightNibble(blk, ci) : 0;
					}
					v = (uint8_t)((sv << 4) | bv);
				}

				s->light[scratchIndex(sx, sy, sz)] = v;
			}
		}
	}
}

// ── Identity bookkeeping ───────────────────────────────────────────────────────────────

static uint32_t s_hash_now = 2166136261u;
static uint32_t s_hash_ref = 2166136261u;
static long     s_fills;
static long     s_mismatched_fills;
static long     s_mismatched_bytes;
static char     s_mis_first[200];

static uint32_t fnv(uint32_t h, const uint8_t* p, size_t n)
{
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}

// Every distinct byte value the two arms produced across every fill. A rewrite that wrote one
// constant everywhere would still be byte-identical to a reference that did the same, so the
// coverage claim needs its own evidence: real light data has many levels in it.
static uint8_t s_seen[256];

static void compareFill(int cx, int cy, int cz)
{
	memset(&s_now, 0xAA, sizeof s_now);   // poison, so an unwritten cell is not a lucky zero
	memset(&s_ref, 0x55, sizeof s_ref);

	scratchFillLight(&s_now, &s_world, cx, cy, cz);
	scratchFillLightRef(&s_ref, &s_world, cx, cy, cz);

	s_fills++;
	s_hash_now = fnv(s_hash_now, s_now.light, sizeof s_now.light);
	s_hash_ref = fnv(s_hash_ref, s_ref.light, sizeof s_ref.light);

	int bad = 0;
	for (size_t i = 0; i < sizeof s_now.light; i++) {
		s_seen[s_now.light[i]] = 1;
		if (s_now.light[i] != s_ref.light[i]) {
			bad++;
			if (!s_mis_first[0])
				snprintf(s_mis_first, sizeof(s_mis_first),
				         "chunk(%d,%d,%d) scratch cell %d: new=0x%02X ref=0x%02X",
				         cx, cy, cz, (int)i, s_now.light[i], s_ref.light[i]);
		}
	}
	s_mismatched_bytes += bad;
	if (bad) s_mismatched_fills++;
}

// ── The world the comparison runs over ─────────────────────────────────────────────────
//
// Terrain with a real surface, an overhang and a sealed pocket, so sky light has somewhere to
// fall, something to stop against and somewhere it cannot reach. Flat ground would make every
// lit cell read 15 or 0 and hide a nibble-order or index bug completely.

static void buildTerrain(int cx, int cz)
{
	Column* col = worldColumnCreate(&s_world, cx, cz);
	if (!col) return;

	const int wx0 = cx * CHUNK_DIM, wz0 = cz * CHUNK_DIM;

	for (int lz = 0; lz < CHUNK_DIM; lz++) {
		for (int lx = 0; lx < CHUNK_DIM; lx++) {
			const int wx = wx0 + lx, wz = wz0 + lz;
			const int h  = 34 + ((lx * 3 + lz * 5 + cx * 7 + cz * 11) & 15);

			for (int y = 0; y <= h; y++)
				worldSet(&s_world, wx, y, wz, (y == h) ? BLOCK_GRASS : BLOCK_STONE);

			// An overhang: a solid lid four blocks above the surface over part of the
			// column, so the cells under it are shadowed rather than fully sunlit.
			if (((lx + lz) & 3) != 0)
				worldSet(&s_world, wx, h + 4, wz, BLOCK_STONE);
		}
	}

	// A sealed pocket dug out under the lid, which must come back dark.
	for (int y = 20; y < 24; y++)
		for (int lz = 4; lz < 9; lz++)
			for (int lx = 4; lx < 9; lx++)
				worldSet(&s_world, wx0 + lx, y, wz0 + lz, BLOCK_AIR);
}

int main(void)
{
	lightEngineInit(true);
	lightQueueInit(&s_queue);
	worldInit(&s_world);

	// A 3x3 patch of real columns, plus a fourth row that is created but never lit — the
	// "still streaming in" case scratchFillLight has an explicit branch for — while
	// everything outside stays absent from the world entirely.
	for (int cz = 0; cz <= 2; cz++)
		for (int cx = 0; cx <= 2; cx++)
			buildTerrain(cx, cz);
	buildTerrain(3, 1);   // terrain, deliberately never propagated

	for (int cz = 0; cz <= 2; cz++)
		for (int cx = 0; cx <= 2; cx++)
			CHECK(lightPropagateColumn(&s_world, cx, cz, &s_queue));

	// A block-light emitter, so the low nibble is not identically zero for the whole run.
	// Nothing in the shipped registry glows yet, which is exactly why this hook exists.
	lightSetLuminanceForTest(BLOCK_GRASS, 12);
	CHECK(lightPropagateColumn(&s_world, 1, 1, &s_queue));

	// The unlit column really is unlit, or the streaming-in branch is not being exercised.
	CHECK(lightChannelSky(worldColumn(&s_world, 3, 1)) == NULL);
	CHECK(lightChannelSky(worldColumn(&s_world, 1, 1)) != NULL);
	CHECK(worldColumn(&s_world, 9, 9) == NULL);

	// Every chunk of every column in and around the patch, plus one below the world and one
	// above it, so both wy range branches fire on lit and unlit columns alike.
	for (int cz = -1; cz <= 4; cz++)
		for (int cx = -1; cx <= 4; cx++)
			for (int cy = -1; cy <= COLUMN_CHUNKS; cy++)
				compareFill(cx, cy, cz);

	// ── The claim ──────────────────────────────────────────────────────────────────────
	CHECK(s_mismatched_bytes == 0);
	CHECK(s_mismatched_fills == 0);
	CHECK(s_hash_now == s_hash_ref);

	// ── The controls, green under every sabotage of scratch.c ──────────────────────────
	//
	// Without these, "the two agree" would also be satisfied by two functions that both
	// wrote nothing interesting, and the poison memset above would be the only thing
	// standing between this file and a test that cannot fail.
	CHECK(s_fills == 6 * 6 * (COLUMN_CHUNKS + 2));

	int distinct = 0;
	for (int i = 0; i < 256; i++) distinct += s_seen[i];
	CHECK(distinct >= 12);              // real light levels, not one memset value
	CHECK(s_seen[0xAA] == 0);           // no cell was left holding the poison
	CHECK(s_seen[0xF0] == 1);           // full sky reached (unlit column / above ceiling)
	CHECK(s_seen[0x00] == 1);           // sealed dark reached (pocket / below the floor)

	printf("scratch light identity: %ld fills | %ld mismatched fills | %ld mismatched bytes | "
	       "new hash %08X | ref hash %08X | %d distinct light bytes\n",
	       s_fills, s_mismatched_fills, s_mismatched_bytes, s_hash_now, s_hash_ref, distinct);
	if (s_mis_first[0])
		printf("scratch light first mismatch: %s\n", s_mis_first);

	worldExit(&s_world);

	if (s_fails == 0)
		printf("scratch light self-test: PASS  %d checks\n", s_checks);
	else
		printf("scratch light self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int scratch_light_test_host_only_t;

#endif   // !__3DS__
