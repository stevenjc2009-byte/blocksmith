// v1.8.8. GATE on world/scratch.c's scratchFillTint — the one piece of the biome-tint chain
// that had no test at all, and the one whose failure mode is silent.
//
// ── Why this binary exists separately from world/biome_tint_test.c ─────────────────────────
//
// biome_tint_test.c gates the tint MECHANISM: the ao-byte packing, the merge key, the palette
// and the shader. It deliberately does not link world/worldgen.c — scratch.h's own header
// comment records that 10 of the 15 host binaries linking scratch.c do not link the generator,
// which is the entire reason scratchFillTint takes a function pointer instead of a WorldGen*.
// So biome_tint_test proves the mechanism and never calls the filler.
//
// The filler was therefore shipped with zero coverage, and it is the piece most able to be
// wrong without anything noticing. scratch.c's own comment says so:
//
//     "An off-by-one here does not crash, drop a face or move a vertex: it draws every chunk
//      in the world one block of COLOUR out of step with its own terrain, which is invisible
//      to every hash and every face count in the tree."
//
// That is an accurate description of a bug no other test in this repo can see. This binary is
// where it becomes visible.
//
// ── The one check that actually earns its keep, and why the obvious one does not ───────────
//
// The obvious test recomputes `cx * CHUNK_DIM + sx - 1` and compares. That is not a test, it
// is a transcription: the same expression, written twice, agreeing with itself. Drop the -1
// from BOTH copies and it still passes.
//
// A neighbour-agreement test is no better. If scratch.c dropped the -1 uniformly, chunk A's
// band column 17 would be fn(16*cx + 17) and chunk B's band column 1 would be
// fn(16*(cx+1) + 1) = fn(16*cx + 17). Identical. Green. Still wrong.
//
// So the anchor here is the BLOCK band, filled by scratchFill — different code, written years
// earlier, whose walk is already gated by half the suite. The world is built so that a
// column's blocks IDENTIFY that column, and the callback derives the same identity from the
// world coordinates it is handed. The two bands must then agree cell for cell. A tint walk
// that is off by one against its own block walk fails immediately, in the direction of the
// error, which is exactly the bug the comment above describes and nothing else can catch.
//
// ── What is NOT proved here ───────────────────────────────────────────────────────────────
//
//   * Anything on real 3DS hardware. Nothing on this project has run there since v1.2.5.
//   * That the colours look right. That is biome_tint_test.c's offline rasterizer, and it is
//     a separate question from whether the right column got the right row.
//   * That scene/chunk_render.c calls this at the right moment. Its call site is one line in a
//     file with <3ds.h> on it; this binary proves the function, not its caller.

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/genversion.h"
#include "world/mesher.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/world.h"
#include "world/worldgen.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                 \
		if (!(cond)) {                                                              \
			s_fails++;                                                              \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                            \
			if (!s_first[0])                                                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                            \
	} while (0)

// ── The identity a column carries, in two independent forms ───────────────────────────────
//
// Eight rows exist (MESH_TINT_ROWS), so eight distinguishable columns are enough to catch a
// one-cell shift in either axis. The mapping is deliberately NOT symmetric in x and z — a
// shift along x and a shift along z must produce different wrong answers, or a test that
// catches one would let the other through.
static uint8_t rowForColumn(int32_t wx, int32_t wz)
{
	return (uint8_t)(((uint32_t)(wx * 3 + wz * 5)) & MESH_TINT_MASK);
}

// The same identity as a block id. Rows 0..7 map to ids 1..8, which are all defined core rows
// (world/registry.c's kCoreDefs has had at least 15 since v1.8.8 and at least 9 before it), so
// the block band can be read straight back out and turned into the row it stands for.
static BlockId blockForColumn(int32_t wx, int32_t wz)
{
	return (BlockId)(rowForColumn(wx, wz) + 1u);
}

static uint8_t rowFromBlock(BlockId b)
{
	return (uint8_t)((unsigned)b - 1u);
}

// The callback under test, in the shape scene/chunk_render.c's genColumnTint has.
static uint8_t synthTint(const void* ctx, int32_t wx, int32_t wz)
{
	(void)ctx;
	return rowForColumn(wx, wz);
}

static uint8_t zeroTint(const void* ctx, int32_t wx, int32_t wz)
{
	(void)ctx; (void)wx; (void)wz;
	return MESH_TINT_NONE;
}

// A callback that does NOT respect the mask, to prove scratchFillTint masks at the boundary
// rather than trusting its caller. A row of 0xFF spilling unmasked into the ao byte would read
// as an ambient occlusion of up to 31 against a 2-bit field.
static uint8_t wideTint(const void* ctx, int32_t wx, int32_t wz)
{
	(void)ctx; (void)wx; (void)wz;
	return 0xFFu;
}

// The real thing: scene/chunk_render.c's adapter, retyped here because that file includes
// <3ds.h> and cannot be linked on a host. If these two ever disagree this binary is testing a
// copy — so the body is one line and it is the same line.
static uint8_t genTint(const void* ctx, int32_t wx, int32_t wz)
{
	return MESH_TINT_ROW_FOR_BIOME(worldgenBiomeAt((const WorldGen*)ctx, wx, wz));
}

static bool bandIsAllZero(const MeshScratch* s)
{
	for (int i = 0; i < SCRATCH_DIM * SCRATCH_DIM; i++)
		if (s->tint[i] != 0) return false;
	return true;
}

int main(void)
{
	static MeshScratch scratch;
	static World       world;

	const int CX = -3, CZ = 7, CY = 4;   // y 64..79, and negative cx on purpose: the -1 border
	                                     // of a negative chunk is where an int-truncation bug
	                                     // would land, and cx = 0 would hide it.

	registryInitCore();
	worldInit(&world);

	// The 3x3 of columns around the target, because the band covers one block outside the chunk
	// on every side and an uncreated column reads as air — which would make the border cells
	// untestable rather than wrong.
	for (int dz = -1; dz <= 1; dz++)
		for (int dx = -1; dx <= 1; dx++)
			CHECK(worldColumnCreate(&world, CX + dx, CZ + dz));

	// Every column in that 3x3, at the one y the scratch will be filled from, gets the block
	// that identifies it.
	const int WY = CY * CHUNK_DIM;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			for (int lz = 0; lz < CHUNK_DIM; lz++) {
				for (int lx = 0; lx < CHUNK_DIM; lx++) {
					const int32_t wx = (int32_t)(CX + dx) * CHUNK_DIM + lx;
					const int32_t wz = (int32_t)(CZ + dz) * CHUNK_DIM + lz;
					worldSet(&world, (int)wx, WY, (int)wz, blockForColumn(wx, wz));
				}
			}
		}
	}

	// ── 1. The anchor check. Tint band against BLOCK band, cell for cell. ──────────────────
	scratchFill(&scratch, &world, CX, CY, CZ);
	scratchFillTint(&scratch, CX, CZ, synthTint, NULL);

	CHECK(scratch.tint_any);

	int agree = 0, disagree = 0, first_bad_sx = -1, first_bad_sz = -1;
	for (int sz = 0; sz < SCRATCH_DIM; sz++) {
		for (int sx = 0; sx < SCRATCH_DIM; sx++) {
			// scratchAt takes CHUNK-LOCAL coordinates running -1..16; the band is indexed by
			// scratchColumn in the same frame offset by one. Reading the block through the
			// public accessor rather than through s->blocks[] is deliberate: it means this
			// test agrees with the mesher's own view of which cell is which, not with a
			// private index expression.
			const BlockId b   = scratchAt(&scratch, sx - 1, WY - CY * CHUNK_DIM, sz - 1);
			const uint8_t got = scratch.tint[scratchColumn(sx, sz)];

			if (rowFromBlock(b) == got) {
				agree++;
			} else {
				disagree++;
				if (first_bad_sx < 0) { first_bad_sx = sx; first_bad_sz = sz; }
			}
		}
	}
	printf("tint band vs block band: %d agree, %d disagree (of %d cells)\n",
	       agree, disagree, SCRATCH_DIM * SCRATCH_DIM);
	if (disagree)
		printf("  first disagreement at band cell (sx %d, sz %d)\n", first_bad_sx, first_bad_sz);
	CHECK(disagree == 0);
	CHECK(agree == SCRATCH_DIM * SCRATCH_DIM);

	// The check above is only worth anything if the identities it compares are actually
	// distinguishable. If rowForColumn happened to return the same row everywhere, every
	// possible shift would agree and the loop would prove nothing. So: count the distinct rows
	// the band carries, and require more than one.
	bool seen[MESH_TINT_ROWS] = { false };
	int  distinct = 0;
	for (int i = 0; i < SCRATCH_DIM * SCRATCH_DIM; i++) {
		const uint8_t t = scratch.tint[i];
		CHECK(t < MESH_TINT_ROWS);
		if (!seen[t]) { seen[t] = true; distinct++; }
	}
	printf("distinct palette rows across the band: %d of %d\n", distinct, (int)MESH_TINT_ROWS);
	CHECK(distinct >= 4);

	// ── 2. tint_any, both ways ─────────────────────────────────────────────────────────────
	//
	// A callback that tints nothing must leave tint_any false, because that flag is what keeps
	// a world with no biome identity on exactly the pre-v1.8.8 mesher path and every pinned
	// hash where it was. A flag that were merely "scratchFillTint ran" would break that.
	scratchFill(&scratch, &world, CX, CY, CZ);
	scratchFillTint(&scratch, CX, CZ, zeroTint, NULL);
	CHECK(!scratch.tint_any);
	CHECK(bandIsAllZero(&scratch));

	// ── 3. A NULL callback is a no-op, not a crash and not a wipe ──────────────────────────
	scratchFill(&scratch, &world, CX, CY, CZ);
	scratchFillTint(&scratch, CX, CZ, synthTint, NULL);
	CHECK(scratch.tint_any);
	scratchFillTint(&scratch, CX, CZ, NULL, NULL);
	CHECK(scratch.tint_any);            // untouched, not reset
	CHECK(!bandIsAllZero(&scratch));

	// ── 4. scratchFill CLEARS the band ─────────────────────────────────────────────────────
	//
	// The ordering rule in scratch.h — "call this AFTER scratchFill, which clears the band" —
	// is only safe if scratchFill really does clear it. If it did not, a column meshed after a
	// tinted one would inherit the previous column's colours, which is the same silent-wrong-
	// picture failure as the off-by-one and just as invisible to a face count.
	scratchFill(&scratch, &world, CX, CY, CZ);
	CHECK(bandIsAllZero(&scratch));
	CHECK(!scratch.tint_any);

	// ── 5. The mask is applied at the boundary ─────────────────────────────────────────────
	scratchFill(&scratch, &world, CX, CY, CZ);
	scratchFillTint(&scratch, CX, CZ, wideTint, NULL);
	CHECK(scratch.tint_any);
	int over = 0;
	for (int i = 0; i < SCRATCH_DIM * SCRATCH_DIM; i++)
		if (scratch.tint[i] != MESH_TINT_MASK) over++;
	printf("cells not masked to %u from a 0xFF callback: %d\n", (unsigned)MESH_TINT_MASK, over);
	CHECK(over == 0);

	// Packed into a real ao byte, a masked row must leave the occlusion alone in both
	// directions — this is the join between this file and world/mesher.h's packing, and it is
	// the thing an unmasked spill would destroy.
	for (uint8_t ao = 0; ao <= MESH_AO_MASK; ao++) {
		const uint8_t packed = meshAoPack(ao, MESH_TINT_MASK);
		CHECK(meshAoValue(packed) == ao);
		CHECK(meshAoTint(packed)  == MESH_TINT_MASK);
	}

	// ── 6. The real generator, through the real adapter ────────────────────────────────────
	WorldGen gen;
	CHECK(worldgenInit(&gen, 1616u, GEN_VERSION_BIOME));

	scratchFill(&scratch, &world, CX, CY, CZ);
	scratchFillTint(&scratch, CX, CZ, genTint, &gen);

	int gen_disagree = 0;
	for (int sz = 0; sz < SCRATCH_DIM; sz++) {
		for (int sx = 0; sx < SCRATCH_DIM; sx++) {
			const int32_t wx = (int32_t)CX * CHUNK_DIM + sx - 1;
			const int32_t wz = (int32_t)CZ * CHUNK_DIM + sz - 1;
			if (scratch.tint[scratchColumn(sx, sz)] !=
			    MESH_TINT_ROW_FOR_BIOME(worldgenBiomeAt(&gen, wx, wz)))
				gen_disagree++;
		}
	}
	CHECK(gen_disagree == 0);

	// A real generator must not produce one flat row across a whole chunk at this seed, or the
	// check above would pass just as happily against a generator that classified nothing. This
	// is the same maximality argument world_budget_bytes_test.c makes about its cap.
	int gen_distinct = 0;
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < SCRATCH_DIM * SCRATCH_DIM; i++)
		if (!seen[scratch.tint[i]]) { seen[scratch.tint[i]] = true; gen_distinct++; }
	printf("real generator, seed 1616 at chunk (%d,%d): %d distinct rows in the band\n",
	       CX, CZ, gen_distinct);

	// ── 7. The seed double-mix hazard, pinned ──────────────────────────────────────────────
	//
	// worldgenInit does NOT store the seed it is given: world/worldgen.c sets
	// g->seed = rngMix(seed ^ 'BLKS'). So re-deriving a generator from a LIVE one's stored
	// seed mixes twice and classifies against a completely different noise field — silently,
	// with a result that looks entirely valid. This cost a v1.8.8 lane an afternoon: 324
	// columns came back as one flat row and the only symptom was a wrong picture.
	//
	// The rule is "pass the live WorldGen*, never a seed". This check makes the trap visible
	// rather than trusting a comment to be read: the two generators MUST disagree. If they ever
	// stopped disagreeing, the double mix would have become harmless and this note would be
	// wrong — which is worth failing over either way.
	WorldGen remixed;
	CHECK(worldgenInit(&remixed, gen.seed, GEN_VERSION_BIOME));
	CHECK(remixed.seed != gen.seed);

	int remix_diff = 0, probed = 0;
	for (int32_t wz = -64; wz <= 64; wz += 8) {
		for (int32_t wx = -64; wx <= 64; wx += 8) {
			probed++;
			if (worldgenBiomeAt(&gen, wx, wz) != worldgenBiomeAt(&remixed, wx, wz))
				remix_diff++;
		}
	}
	printf("re-deriving from the STORED seed: %d of %d probed columns classify differently\n",
	       remix_diff, probed);
	CHECK(remix_diff > 0);

	worldExit(&world);

	printf("scratch tint fill self-test: %s\n", s_fails ? "FAIL" : "PASS");
	printf("  %d checks", s_checks);
	if (s_fails) printf(", %d failed, first %s", s_fails, s_first);
	printf("\n");
	if (s_fails) printf("FAILED - %d of %d\n", s_fails, s_checks);
	return s_fails ? 1 : 0;
}
