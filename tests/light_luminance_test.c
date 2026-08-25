// light_luminance_test.c — BlockDef.luminance -> the lighting engine, end to end.
//
// v1.8.2. BlockDef.luminance has existed since v1.6.0 and has always crossed the wire
// (world/registry.c packs it at byte 24 of a DEFS record and unpacks it back), but
// nothing on the receiving side ever fed it to world/light.c. A server that registered a
// glowing block at join sent its luminance, the client stored it in the def, and the
// lighting engine read a private table that only a test hook could write — so the block
// rendered pitch dark, with no error and no log line. This file is the coverage for the
// wiring that closes that.
//
// Own binary and own main(), for the reason every other test binary here has one: the
// world suite is driven by tests/host_test.c and two mains cannot share a link.
//
// NOTHING HERE ADDS A LUMINOUS BLOCK TO THE SHIPPED REGISTRY. Every emitter below is
// registered at runtime into the dynamic id space (0x80..0xFD) through the same two calls
// the join path uses, and torn down again with registryInitCore(). world/registry.c's
// kCoreDefs[] is not touched, which is why the core crc16 cannot move.
//
// Four arms:
//   1  registryRegister      — the local dynamic-registration call. Measured falloff.
//   2  registryRemoteApply   — the DEFS-payload call, fed a real 28-byte wire record.
//                              This is the arm that reproduces the actual bug.
//   3  absent                — the same block defined but never placed. All zero.
//   4  un-light              — place, relight, BREAK, relight, re-read. The shipped suite
//                              only ever ADDS light and never re-reads after a break, so
//                              this is a genuine coverage gap. It drives the engine through
//                              lightSetLuminanceForTest, NOT through the registry, which
//                              makes it the named control that must stay green when the
//                              registry wiring is sabotaged.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/block.h"
#include "world/light.h"
#include "world/registry.h"
#include "world/world.h"

static World s_world;
static int   s_fail;
static int   s_checks;

#define CHECK(cond, ...) do {                                      \
	s_checks++;                                                    \
	if (!(cond)) { s_fail++;                                       \
		printf("  FAIL L%d: ", __LINE__); printf(__VA_ARGS__);     \
		printf("\n"); }                                            \
} while (0)

// A sealed stone shell with a 14x5x14 air room inside it, y 40..44. Wider than
// world_test.c's 8x5x8 room on purpose: a luminance of 9 reaches nine cells, and the
// falloff has to be measurable all the way out to zero INSIDE the room rather than
// stopping at a wall. Nothing above y=45 is touched, so the column's own air lets sky
// light fall onto the shell's roof and no further — which is what makes the sky readings
// below a real control rather than a tautology.
static void buildRoom(void)
{
	for (int y = 39; y <= 45; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++) {
				const bool room = (y >= 40 && y <= 44 &&
				                   x >= 1 && x <= 14 && z >= 1 && z <= 14);
				worldSet(&s_world, x, y, z, room ? BLOCK_AIR : BLOCK_STONE);
			}
}

// Fills a def for a dynamic emitter. Solid and luminous, textured as stone: the art is
// irrelevant here, the luminance byte is the whole subject.
static void makeDef(BlockDef* def, const char* name, uint8_t luminance)
{
	memset(def, 0, sizeof *def);
	snprintf(def->name, REGISTRY_NAME_MAX, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) def->tex[f] = BTEX_STONE;
	def->flags     = REG_FLAG_SOLID | REG_FLAG_LUMINOUS;
	def->luminance = luminance;
	def->hardness  = 20;
}

static void dumpRow(const char* label, Column* col, int x0, int n)
{
	printf("  %-26s", label);
	for (int d = 0; d < n; d++)
		printf(" %u", lightGetBlock(col, x0 + d, 42, 2));
	printf("\n");
}

// The two controls every arm re-asserts. They are independent of the registry wiring, so
// they must read the same in a sabotaged build as in a good one; if either ever moves,
// this file is measuring something other than what it claims to.
static void controls(Column* col, const char* arm)
{
	CHECK(lightGetSky(col, 2, 42, 2) == 0,
	      "CONTROL[%s] sky inside the sealed room = %u, want 0",
	      arm, lightGetSky(col, 2, 42, 2));
	CHECK(lightGetSky(col, 2, 46, 2) == 15,
	      "CONTROL[%s] sky on the shell roof = %u, want 15",
	      arm, lightGetSky(col, 2, 46, 2));
}

// Core blocks declare no luminance, and must keep declaring none: this is the check that
// says the wiring did not accidentally light up an existing world. Runs against the
// SHIPPED core table with no dynamic row registered at all.
static void testCoreRegistryEmitsNothing(void)
{
	registryInitCore();
	printf("ARM 0 - shipped core registry, no dynamic rows\n");

	int declared = 0;
	for (int id = 0; id < REGISTRY_MAX; id++)
		if (registryIsDefined((BlockId)id) && registryGet((BlockId)id)->luminance != 0)
			declared++;
	CHECK(declared == 0, "%d core rows declare a non-zero luminance, want 0", declared);

	lightEngineInit(true);
	worldInit(&s_world);
	buildRoom();
	worldSet(&s_world, 2, 42, 2, BLOCK_WOOD);   // a solid, ordinary, non-glowing block
	CHECK(lightRelightColumn(&s_world, 0, 0), "core arm relight refused");

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL, "core arm has no column");
	if (col) {
		dumpRow("block light, +x:", col, 2, 13);
		for (int d = 0; d < 13; d++)
			CHECK(lightGetBlock(col, 2 + d, 42, 2) == 0,
			      "core block lit x=%d to %u, want 0",
			      2 + d, lightGetBlock(col, 2 + d, 42, 2));
		controls(col, "core");
	}

	worldExit(&s_world);
	lightEngineInit(false);
	registryInitCore();
}

// Places `id` at (2,42,2), relights through the real edit path, and asserts a -1-per-cell
// falloff from `lum` out to zero. Returns the column so the caller can read the controls.
static Column* measureFalloff(BlockId id, uint8_t lum, const char* arm)
{
	lightEngineInit(true);
	worldInit(&s_world);
	buildRoom();
	CHECK(worldSet(&s_world, 2, 42, 2, id), "[%s] worldSet(emitter) refused", arm);

	// lightRelightColumn is the production EDIT path — what scene/interact.c calls after
	// a block is placed — not a test-only entry point.
	CHECK(lightRelightColumn(&s_world, 0, 0), "[%s] lightRelightColumn refused", arm);

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL, "[%s] no column", arm);
	if (!col) return NULL;

	dumpRow("block light, +x:", col, 2, 13);

	for (int d = 0; d < 13; d++) {
		const int want = (d < lum) ? (int)lum - d : 0;
		const int got  = lightGetBlock(col, 2 + d, 42, 2);
		CHECK(got == want, "[%s] block light at distance %d = %d, want %d",
		      arm, d, got, want);
	}

	// Outside the sealed shell nothing may glow, in any direction.
	CHECK(lightGetBlock(col, 2, 47, 2) == 0, "[%s] light escaped the roof: %u",
	      arm, lightGetBlock(col, 2, 47, 2));
	CHECK(lightGetBlock(col, 2, 38, 2) == 0, "[%s] light escaped the floor: %u",
	      arm, lightGetBlock(col, 2, 38, 2));

	controls(col, arm);
	return col;
}

// ARM 1: the local dynamic-registration call.
static void testRegistryRegisterLuminance(void)
{
	registryInitCore();

	BlockDef def;
	makeDef(&def, "lum_lamp_a", 9);
	const BlockId lamp = registryRegister(&def);
	printf("ARM 1 - registryRegister(\"lum_lamp_a\", luminance 9) -> id 0x%02X\n", lamp);
	CHECK(lamp >= REG_ID_DYN_LO, "registered id 0x%02X is not in the dyn range", lamp);
	CHECK(registryGet(lamp)->luminance == 9, "def stored luminance %u, want 9",
	      registryGet(lamp)->luminance);

	if (lamp >= REG_ID_DYN_LO) measureFalloff(lamp, 9, "register");

	worldExit(&s_world);
	lightEngineInit(false);
	registryInitCore();
}

// ARM 2: the DEFS payload. This is the arm that reproduces the reported defect — bytes
// arrive from a server, get unpacked into a BlockDef, and have to end up lighting cells.
static void testRegistryRemoteApplyLuminance(void)
{
	registryInitCore();

	BlockDef def;
	makeDef(&def, "lum_lamp_b", 11);

	// A genuine 28-byte wire record, built by the same packer the server uses, so the
	// luminance under test really does travel through byte 24 and back out again.
	uint8_t record[REGISTRY_WIRE_RECORD_BYTES];
	registryDefPack(record, (BlockId)REG_ID_DYN_LO, &def);
	CHECK(record[24] == 11, "wire byte 24 (luminance) = %u, want 11", record[24]);

	const size_t applied = registryRemoteApply((uint8_t)REG_ID_DYN_LO, record, 1);
	printf("ARM 2 - registryRemoteApply(1 DEFS record, luminance 11) -> %u applied\n",
	       (unsigned)applied);
	CHECK(applied == 1, "remote apply returned %u, want 1", (unsigned)applied);

	const BlockId lamp = (BlockId)REG_ID_DYN_LO;
	CHECK(registryIsDefined(lamp), "id 0x%02X was not defined by the DEFS batch", lamp);
	CHECK(registryGet(lamp)->luminance == 11, "unpacked luminance %u, want 11",
	      registryGet(lamp)->luminance);

	Column* col = measureFalloff(lamp, 11, "defs");

	// Both lighting engines have to agree about a registry-fed emitter, not just the
	// flood fill. The sweeps pull where the fill pushes and share nothing but opaqueAt
	// and the height map, so a table filled for one and not the other shows up here.
	if (col) {
		const uint8_t* blk = lightChannelBlock(col);
		CHECK(blk != NULL, "[defs] no block channel");
		if (blk) {
			uint8_t* fill = (uint8_t*)malloc(LIGHT_COL_BYTES);
			CHECK(fill != NULL, "[defs] out of memory");
			if (fill) {
				memcpy(fill, blk, LIGHT_COL_BYTES);
				CHECK(lightRelightColumnSweeps(&s_world, 0, 0),
				      "[defs] sweep engine refused");
				blk = lightChannelBlock(worldColumn(&s_world, 0, 0));
				CHECK(blk && memcmp(fill, blk, LIGHT_COL_BYTES) == 0,
				      "[defs] flood fill and sweeps disagree on the block channel");
				free(fill);
			}
		}
	}

	worldExit(&s_world);
	lightEngineInit(false);
	registryInitCore();
}

// ARM 3: the block is defined, with a luminance, and simply is not in the world. Nothing
// may glow. Without this the falloff arms could pass on a table that lights everything.
static void testLuminousBlockAbsentStaysDark(void)
{
	registryInitCore();

	BlockDef def;
	makeDef(&def, "lum_lamp_c", 15);
	const BlockId lamp = registryRegister(&def);
	printf("ARM 3 - \"lum_lamp_c\" (luminance 15) registered but NEVER placed\n");
	CHECK(lamp >= REG_ID_DYN_LO, "registered id 0x%02X is not in the dyn range", lamp);

	lightEngineInit(true);
	worldInit(&s_world);
	buildRoom();                      // no emitter placed at all
	CHECK(lightRelightColumn(&s_world, 0, 0), "[absent] relight refused");

	Column* col = worldColumn(&s_world, 0, 0);
	CHECK(col != NULL, "[absent] no column");
	if (col) {
		dumpRow("block light, +x:", col, 2, 13);
		for (int d = 0; d < 13; d++)
			CHECK(lightGetBlock(col, 2 + d, 42, 2) == 0,
			      "[absent] x=%d reads %u with no emitter in the world, want 0",
			      2 + d, lightGetBlock(col, 2 + d, 42, 2));
		controls(col, "absent");
	}

	worldExit(&s_world);
	lightEngineInit(false);
	registryInitCore();
}

// ARM 4 — the un-light probe, and the NAMED CONTROL for the sabotage run.
//
// It drives the engine through lightSetLuminanceForTest on a core block, which is the
// path the shipped suite already uses, so it is independent of the registry wiring and
// must stay green whether that wiring is present or broken.
//
// What it adds as coverage: world_test.c's testLightBlockChannel and
// testLightBlockChannelFromDynamicId both place an emitter, assert the lit values and
// tear the world down. Neither ever BREAKS the block and re-reads. Un-lighting is the
// classic failure mode of a light engine, so it is measured rather than assumed.
static void testUnlightAfterBreak(void)
{
	registryInitCore();
	lightEngineInit(true);
	worldInit(&s_world);
	buildRoom();

	// --- placed ---
	worldSet(&s_world, 2, 42, 2, BLOCK_WOOD);
	lightSetLuminanceForTest(BLOCK_WOOD, 7);
	CHECK(lightRelightColumn(&s_world, 0, 0), "[unlight] relight (placed) refused");
	Column* col = worldColumn(&s_world, 0, 0);
	printf("ARM 4 - CONTROL: test-hook emitter at (2,42,2), luminance 7\n");
	CHECK(col != NULL, "[unlight] no column");
	if (!col) goto done;
	dumpRow("placed,  block light +x:", col, 2, 6);
	for (int d = 0; d <= 4; d++)
		CHECK(lightGetBlock(col, 2 + d, 42, 2) == (uint8_t)(7 - d),
		      "CONTROL[unlight] placed x=%d = %u, want %d",
		      2 + d, lightGetBlock(col, 2 + d, 42, 2), 7 - d);
	controls(col, "unlight/placed");

	// --- broken: darkness must propagate back out ---
	worldSet(&s_world, 2, 42, 2, BLOCK_AIR);
	CHECK(lightRelightColumn(&s_world, 0, 0), "[unlight] relight (broken) refused");
	col = worldColumn(&s_world, 0, 0);
	dumpRow("broken,  block light +x:", col, 2, 6);
	for (int d = 0; d <= 4; d++)
		CHECK(lightGetBlock(col, 2 + d, 42, 2) == 0,
		      "CONTROL[unlight] UNLIGHT x=%d still reads %u, want 0",
		      2 + d, lightGetBlock(col, 2 + d, 42, 2));
	CHECK(lightGetBlock(col, 1, 40, 1) == 0, "CONTROL[unlight] far corner = %u, want 0",
	      lightGetBlock(col, 1, 40, 1));
	CHECK(lightGetBlock(col, 14, 44, 14) == 0, "CONTROL[unlight] far corner = %u, want 0",
	      lightGetBlock(col, 14, 44, 14));
	controls(col, "unlight/broken");

	// --- re-placed: proves the dark reading above was not simply a dead engine ---
	worldSet(&s_world, 2, 42, 2, BLOCK_WOOD);
	CHECK(lightRelightColumn(&s_world, 0, 0), "[unlight] relight (re-placed) refused");
	col = worldColumn(&s_world, 0, 0);
	dumpRow("replaced, block light +x:", col, 2, 6);
	CHECK(lightGetBlock(col, 2, 42, 2) == 7, "CONTROL[unlight] re-place = %u, want 7",
	      lightGetBlock(col, 2, 42, 2));

done:
	lightSetLuminanceForTest(BLOCK_WOOD, 0);   // clears the override
	worldExit(&s_world);
	lightEngineInit(false);
	registryInitCore();
}

int main(void)
{
	testCoreRegistryEmitsNothing();
	testRegistryRegisterLuminance();
	testRegistryRemoteApplyLuminance();
	testLuminousBlockAbsentStaysDark();
	testUnlightAfterBreak();

	printf("\nlight luminance self-test: %s - %d of %d checks\n",
	       s_fail ? "FAILED" : "passed", s_checks - s_fail, s_checks);
	return s_fail ? 1 : 0;
}
