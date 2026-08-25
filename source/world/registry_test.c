// Host tests for the master block registry (v1.6.0 Phase A).
//
// Own main(), like every other suite binary: a broken registry must not stop the
// world suite from running, and two mains cannot share one link. The registry is
// plain C with no <3ds.h>, so all of it is provable here in seconds.
//
// What each probe guards:
//   testRegistryRoundTrip    - register/find/duplicate/full-range/unknown-id contract
//   testRegistryCoreIdsStable- ids 1..9 byte-identical after InitCore; every saved
//                              world and replay depends on these never moving
//   testRegistryCrcStability - same defs -> same crc16, different defs -> different
//                              crc16, plus a pinned golden for the core-only table
//   testRegistrySidecar      - registry.bin round-trip restores dynamic rows exactly
//
// The __3DS__ guard is load-bearing, not tidy — the console Makefile globs every .c
// under source/world, and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/registry.h"

// ---------------------------------------------------------------------------
// The dynamic block-id range, pinned to hand-written literals.
//
// This is the most important range in the block system: REG_ID_DYN_LO..REG_ID_DYN_HI is
// how many block types a server may add at join time, so it is the ceiling on what the
// game can represent beyond the ten compiled-in core rows.
//
// Before 2026-08-25 this file asserted that range against ITSELF. The two checks in
// testRegistryRoundTrip() read
//
//     check(placed == REG_ID_DYN_HI - REG_ID_DYN_LO + 1 - 5, ...);
//     check(registryCount() == 1 + 9 + (REG_ID_DYN_HI - REG_ID_DYN_LO + 1), ...);
//
// and world/registry.c allocates from those same two constants, so both sides of every
// comparison moved together and neither could ever disagree. Measured, not theorised:
// cutting REG_ID_DYN_HI from 0xFD to 0x90 in source/world/registry.h — the dyn range from
// 126 rows to 17, i.e. a silent 87% cut in what the game can represent — built clean and
// this suite printed "PASS 53 checks, 0 failed", exit 0. The check-count pin below could
// not help: the count does not move, because the fill loop emits one check regardless of
// how many rows it managed to place.
//
// So the expectations are naked literals now. They must NEVER be computed from
// REG_ID_DYN_LO, REG_ID_DYN_HI, REGISTRY_MAX or anything else world/registry.{c,h} can
// also move. Widening or narrowing the range for real means editing these four lines by
// hand, and the suite going red until you do is the entire point.
#define REGISTRY_DYN_LO_PIN     0x80  // first dynamic block id
#define REGISTRY_DYN_HI_PIN     0xFD  // last one; 0xFE/0xFF stay reserved
#define REGISTRY_DYN_ROWS_PIN   126   // 0xFD - 0x80 + 1, WRITTEN OUT, never computed
#define REGISTRY_FULL_COUNT_PIN 136   // 10 core rows (air + nine) + 126 dyn rows

// Compile-time layer. These fire when the host suite builds, which is every
// tools/run_host_tests.sh run; the 3DS build never compiles this file (see the __3DS__
// guard at the top). The runtime layer below survives anyone deleting these.
_Static_assert(REG_ID_DYN_LO == REGISTRY_DYN_LO_PIN,
               "registry.h's REG_ID_DYN_LO is 0x80, the first dynamic block id");
_Static_assert(REG_ID_DYN_HI == REGISTRY_DYN_HI_PIN,
               "registry.h's REG_ID_DYN_HI is 0xFD, the last dynamic block id");
_Static_assert(REG_ID_DYN_HI - REG_ID_DYN_LO + 1 == REGISTRY_DYN_ROWS_PIN,
               "the dynamic block-id range holds 126 rows; if you moved it deliberately, "
               "update REGISTRY_DYN_ROWS_PIN and REGISTRY_FULL_COUNT_PIN in "
               "source/world/registry_test.c by hand");

// The remedy text every dyn-range pin prints when it fails. One string, so a reader who
// trips two of them at once is told the same thing the same way twice.
static const char *const kDynRangeWhy =
	"126 is the SIZE OF THE DYNAMIC BLOCK-ID RANGE (REG_ID_DYN_LO 0x80 ..\n"
	"         REG_ID_DYN_HI 0xFD in source/world/registry.h) — how many block types a\n"
	"         server can add on top of the ten core rows. If you widened or narrowed\n"
	"         that range ON PURPOSE, update REGISTRY_DYN_LO_PIN / REGISTRY_DYN_HI_PIN /\n"
	"         REGISTRY_DYN_ROWS_PIN / REGISTRY_FULL_COUNT_PIN in\n"
	"         source/world/registry_test.c to match, and expect the pinned core crc and\n"
	"         every connected server to need the same change. If you did NOT, the range\n"
	"         has moved behind your back and the game silently represents fewer blocks.\n"
	"         These pins are deliberately NOT derived from REG_ID_DYN_HI/LO: a pin\n"
	"         computed from the constant it is pinning moves with it and guards nothing.";

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   %s\n", what);
	} else {
		printf("  ok     %s\n", what);
	}
}

// check() for a pinned literal. Counts exactly like check() does — it must, or it would
// perturb the check-count pin below — but a failure also prints the measured value, the
// expected one, and WHY the expected one is what it is and what to do about it.
//
// A bare "FAIL   the dyn range accepts exactly its remaining capacity" tells whoever
// widened the range nothing at all: not which number is wrong, not which line to edit,
// not that the literal is hand-written on purpose. The `why` block does.
static void checkPin(bool cond, long got, long want, const char *what, const char *why)
{
	g_checks++;
	if (cond) {
		printf("  ok     %s\n", what);
		return;
	}
	g_fails++;
	printf("  FAIL   %s: expected %ld, got %ld.\n         %s\n", what, want, got, why);
}

// How many check() calls this suite makes on a healthy tree. A LITERAL on purpose.
//
// Every suite in this project used to end at "0 failed" and nothing else, which means a
// check that never RUNS is indistinguishable from a check that passes. Measured, not
// theorised: a sabotage that shortened a production-constant-bounded loop in the net suite
// took its count from 326 to 318 and the suite reported "0 failed". Eight checks were
// deleted and it called that a pass.
//
// The number below must never be computed from a production constant, a loop bound, or
// anything else the code under test can also move — a pin that shrinks alongside the thing
// it is pinning is exactly the bug it exists to catch.
//
// Legitimately adding or removing a check means editing this by hand. The suite going red
// until you do is deliberate friction, not an accident.
//
// 53 -> 54 on 2026-08-25: one check added, "the dynamic block-id range is still
// 0x80..0xFD, 126 rows", the runtime half of the dyn-range pin.
#define REGISTRY_TEST_EXPECTED_CHECKS 54

// Deliberately NOT routed through check(): this must not perturb the number it is testing,
// so it bumps g_fails only. Reporting shape is check()'s, so a failure here reads the same
// way every other failure in this file does.
static void checkCountPin(void)
{
	if (g_checks == REGISTRY_TEST_EXPECTED_CHECKS)
		return;

	g_fails++;
	if (g_checks < REGISTRY_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran: a loop bound shrank, an early\n"
		       "         return or a continue fired, or a check was deleted. The checks that\n"
		       "         did run passing tells you nothing about the ones that did not.\n"
		       "         Find them. Do NOT re-pin REGISTRY_TEST_EXPECTED_CHECKS to go green.\n",
		       REGISTRY_TEST_EXPECTED_CHECKS - g_checks,
		       REGISTRY_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If you added them on purpose, set REGISTRY_TEST_EXPECTED_CHECKS in\n"
		       "         source/world/registry_test.c to %d. If you did not, something is\n"
		       "         running checks more times than it should.\n",
		       g_checks - REGISTRY_TEST_EXPECTED_CHECKS,
		       REGISTRY_TEST_EXPECTED_CHECKS, g_checks, g_checks);
}

// A dyn def with a distinct name and stone faces everywhere, solid. Returns the
// packed def rather than registering it, so tests control when/whether that happens.
static BlockDef makeDef(const char *name)
{
	BlockDef d;
	memset(&d, 0, sizeof d);
	snprintf(d.name, sizeof d.name, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) d.tex[f] = BTEX_STONE;
	d.flags      = REG_FLAG_SOLID;
	d.hardness   = 1;
	d.variant_of = 0;
	return d;
}

static void testRegistryRoundTrip(void)
{
	puts("registry: register / find / duplicate / full range / unknown id");

	registryInitCore();
	// Nine core rows since roadmap tasks 17 and 19: grass..planks are the seven that
	// also carry ITEM ids, plus water (0x08) and tall grass (0x09), which are core
	// blocks and deliberately NOT items — world/block.h records why BLOCK_COUNT
	// stayed at 8 while the registry's row count moved to 10.
	check(registryCount() == 10, "a fresh table defines exactly air + the nine core blocks");
	check(registryFind("grass") == BLOCK_GRASS, "core rows are findable by name");

	// The runtime half of the dyn-range pin. The two _Static_asserts at the top of this
	// file are the primary, compile-time defence; this one is what remains if somebody
	// ever deletes them, and it names the range in the run output on a healthy tree so
	// the number is visible rather than implied.
	checkPin(REG_ID_DYN_LO == REGISTRY_DYN_LO_PIN && REG_ID_DYN_HI == REGISTRY_DYN_HI_PIN,
	         (long)(REG_ID_DYN_HI - REG_ID_DYN_LO + 1), (long)REGISTRY_DYN_ROWS_PIN,
	         "the dynamic block-id range is still 0x80..0xFD, 126 rows", kDynRangeWhy);

	// Five dynamic registrations land on consecutive ids from REG_ID_DYN_LO up.
	BlockId ids[5];
	for (int i = 0; i < 5; i++) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "probe_%d", i);
		BlockDef d = makeDef(name);
		ids[i] = registryRegister(&d);
	}
	// Against the literal 0x80, not REG_ID_DYN_LO: registryRegister() allocates FROM
	// REG_ID_DYN_LO, so comparing to it asks the code whether it agrees with itself.
	bool consecutive = true;
	long first_bad = -1, first_want = -1;
	for (int i = 0; i < 5; i++)
		if (ids[i] != (BlockId)(REGISTRY_DYN_LO_PIN + i)) {
			if (consecutive) { first_bad = ids[i]; first_want = REGISTRY_DYN_LO_PIN + i; }
			consecutive = false;
		}
	checkPin(consecutive, first_bad, first_want,
	         "registrations take the lowest free dyn ids in order, starting at 0x80",
	         kDynRangeWhy);

	for (int i = 0; i < 5; i++) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "probe_%d", i);
		check(registryFind(name) == ids[i], "find-by-name returns the registered id");
	}

	BlockDef dup = makeDef("probe_2");
	check(registryRegister(&dup) == 0, "a duplicate name is refused with 0");

	const BlockDef *got = registryGet(ids[3]);
	check(got != NULL && strcmp(got->name, "probe_3") == 0,
	      "registryGet hands back the def that was registered");
	check(got->flags & REG_FLAG_SOLID, "the registered flags survive the round trip");

	// The never-NULL contract blockInfo() has always had: an unknown id answers
	// air instead of crashing whoever reads it.
	const BlockDef *unknown = registryGet((BlockId)0xC7);
	check(unknown != NULL && strcmp(unknown->name, "air") == 0,
	      "an unregistered dyn id answers the air def, never NULL");
	check(!blockIsSolid(0xC7), "and it is not solid, so meshing/collision treat it as a hole");
	check(strcmp(blockInfo((BlockId)0xFE)->name, "air") == 0,
	      "the reserved 0xFE answers air too");

	// Fill the whole dyn range: exactly 121 more registrations fit (126 rows minus the
	// five placed above), then 0.
	int placed = 0;
	for (;;) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "full_%03d", placed);
		BlockDef d = makeDef(name);
		BlockId id = registryRegister(&d);
		if (id == 0) break;
		placed++;
	}
	checkPin(placed == REGISTRY_DYN_ROWS_PIN - 5, placed, REGISTRY_DYN_ROWS_PIN - 5,
	         "the dyn range accepts exactly its remaining 121 rows, then refuses",
	         kDynRangeWhy);
	checkPin(registryCount() == REGISTRY_FULL_COUNT_PIN,
	         (long)registryCount(), (long)REGISTRY_FULL_COUNT_PIN,
	         "count reflects every defined row once the range is full: 10 core + 126 dyn",
	         kDynRangeWhy);
}

static void testRegistryCoreIdsStable(void)
{
	puts("registry: core ids 1..9 are identical after InitCore");

	registryInitCore();

	// These ten checks ARE the save-format guarantee: a region file written by
	// any earlier build decodes by raw id, so if grass ever stops being 1 every
	// old world silently re-textures. Names first, then the texture rows.
	static const char *const want_names[] = {
		"air", "grass", "dirt", "stone", "sand", "wood", "leaves", "planks",
		"water", "tall_grass",
	};
	for (BlockId id = 0; id < 10; id++) {
		char what[64];
		snprintf(what, sizeof what, "id %u is still \"%s\"", (unsigned)id, want_names[id]);
		check(strcmp(registryGet(id)->name, want_names[id]) == 0, what);
	}

	const BlockDef *grass = registryGet(BLOCK_GRASS);
	check(grass->tex[FACE_TOP] == BTEX_GRASS_TOP && grass->tex[FACE_BOTTOM] == BTEX_DIRT
	      && grass->tex[FACE_EAST] == BTEX_GRASS_SIDE,
	      "grass keeps top/side/bottom tiles exactly as block.c defined them");
	const BlockDef *wood = registryGet(BLOCK_WOOD);
	check(wood->tex[FACE_TOP] == BTEX_WOOD_TOP && wood->tex[FACE_EAST] == BTEX_WOOD_SIDE,
	      "wood keeps end/side tiles");
	check((registryGet(BLOCK_LEAVES)->flags & REG_FLAG_TRANSPARENT) != 0,
	      "leaves stay transparent");
	check((registryGet(BLOCK_STONE)->flags & REG_FLAG_SOLID) != 0,
	      "stone stays solid");
	check(!(registryGet(BLOCK_AIR)->flags & REG_FLAG_SOLID),
	      "air stays non-solid");

	// Water (0x08) and tall grass (0x09), roadmap tasks 17 and 19. Pinned here for
	// the same reason as the rows above: they are core ids now, so a saved world
	// stores them as raw 8 and 9 and they can never move.
	const BlockDef *water = registryGet((BlockId)BLOCK_WATER);
	check(!(water->flags & REG_FLAG_SOLID),
	      "water is not solid: the player walks through it and it occludes nothing");
	check((water->flags & REG_FLAG_TRANSPARENT) != 0,
	      "water is transparent, which is what puts it in the mesher's deferred pass");
	check((water->flags & REG_FLAG_LIQUID) != 0,
	      "water is a liquid, which is what makes the crosshair refuse to target it");
	check(regShapeOf(water->flags) == BLOCK_SHAPE_FULL_CUBE,
	      "water is a full cube, so it culls against its own kind face-for-face");
	check(water->tex[FACE_TOP] == BTEX_WATER && water->tex[FACE_EAST] == BTEX_WATER
	      && water->tex[FACE_BOTTOM] == BTEX_WATER,
	      "water wears the same tile on every face");

	const BlockDef *tg = registryGet((BlockId)BLOCK_TALL_GRASS);
	check(!(tg->flags & REG_FLAG_SOLID),
	      "tall grass is not solid: walked through, and it casts no AO");
	check((tg->flags & REG_FLAG_TRANSPARENT) != 0,
	      "tall grass is transparent, so its cutout texels are alpha-tested away");
	check(!(tg->flags & REG_FLAG_LIQUID),
	      "tall grass is not a liquid, so the crosshair CAN target it");
	check(regShapeOf(tg->flags) == BLOCK_SHAPE_CROSS,
	      "tall grass is the first BLOCK_SHAPE_CROSS block in the game");
	check(tg->tex[FACE_TOP] == BTEX_TALL_GRASS && tg->tex[FACE_EAST] == BTEX_TALL_GRASS,
	      "tall grass wears its own tile; emitCross reads the east rect");
}

static void testRegistryCrcStability(void)
{
	puts("registry: crc16 is deterministic, content-sensitive, and pinned for core");

	registryInitCore();
	uint16_t base = registryCrc16();
	check(base != 0, "the core table produces a non-zero crc");
	check(base == registryCrc16() && base == registryCrc16(),
	      "repeated calls over an unchanged table agree");

	// Pinned golden for the core-only table. If this ever moves WITHOUT a
	// deliberate core-def edit, something redefined history behind the saved
	// worlds' backs. (Computed from this exact kCoreDefs layout.)
	//
	// Moved 0x7E5B -> 0x72A8 on 2026-08-23 by roadmap tasks 17 and 19, which append
	// water (0x08) and tall grass (0x09) to kCoreDefs. Deliberate core-def edit, so
	// the pin moves with it. The cost of moving it is real and is recorded here: a
	// client on this build joining a server still built with eight core rows sees
	// registryMatchesInfo() fail on both count and crc, falls into the bounded
	// REGISTRY_FETCH retry, and finishes the session with s_reg_synced false.
	// Degraded, not fatal — and it clears the moment the server ships the same rows.
	//
	// Moved 0x72A8 -> 0x4066 on 2026-08-24 by roadmap task 50, which gives every core row a
	// non-zero `hardness`. Worth being precise about what moved and what did not: the wire
	// LAYOUT is untouched — `hardness` has been byte 25 of the 28-byte record since the field
	// was added, and registryCount() is still 10. What changed is the VALUES in that byte,
	// from ten zeroes to 12/12/45/10/40/4/40 (and 0 for air and water), and registryCrc16()
	// hashes the records' contents. So this is a deliberate core-def edit like the two above
	// and the pin moves with it, with the identical cost: a client on this build joining a
	// server whose kCoreDefs still has no hardness fails registryMatchesInfo() on crc, falls
	// into the bounded REGISTRY_FETCH retry, and finishes with s_reg_synced false. v1.8.1
	// therefore ships the server in lockstep — deps/blocksmith-server/game/world/registry.c is
	// a byte-identical vendored copy and tools/sync-world-sources.sh is what keeps it so.
	check(base == 0x4066u,
	      "core-only crc matches the pinned golden 0x4066");

	// Content sensitivity: one extra def must move the crc, and re-init must
	// put it back - proving the crc covers table content, not process state.
	BlockDef d = makeDef("crc_probe");
	BlockId id = registryRegister(&d);
	check(id != 0 && registryCrc16() != base, "registering a def changes the crc");

	registryInitCore();
	check(registryCrc16() == base, "re-init restores the core-only crc exactly");
}

static void testRegistrySidecar(void)
{
	puts("registry: sidecar round-trip restores dynamic rows exactly");

	static const char *const path = "build-host/registry_sidecar_probe.bin";
	registryInitCore();

	BlockDef a = makeDef("side_a");
	a.luminance = 5;
	BlockId id_a = registryRegister(&a);
	BlockDef b = makeDef("side_b");
	b.flags |= REG_FLAG_LUMINOUS;
	BlockId id_b = registryRegister(&b);
	// Literals again, for the reason spelled out at the top of this file: registryRegister()
	// hands out ids counting up from REG_ID_DYN_LO, so REG_ID_DYN_LO cannot be the yardstick.
	checkPin(id_a == REGISTRY_DYN_LO_PIN && id_b == REGISTRY_DYN_LO_PIN + 1,
	         (long)id_a, (long)REGISTRY_DYN_LO_PIN,
	         "two defs registered before saving, landing on 0x80 and 0x81", kDynRangeWhy);

	uint16_t before_crc = registryCrc16();
	check(registrySidecarSave(path), "sidecarSave writes the file");

	registryInitCore();
	check(registryFind("side_a") == 0, "after re-init the dyn rows are gone");
	check(registrySidecarLoad(path), "sidecarLoad reads it back");
	check(registryFind("side_a") == id_a && registryFind("side_b") == id_b,
	      "both rows come back under their original ids");
	check(registryGet(id_a)->luminance == 5
	      && (registryGet(id_b)->flags & REG_FLAG_LUMINOUS) != 0,
	      "field-level content survives the round trip");
	check(registryCrc16() == before_crc, "the restored table hashes identically");

	remove(path);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== registry test ==");

	testRegistryRoundTrip();
	testRegistryCoreIdsStable();
	testRegistryCrcStability();
	testRegistrySidecar();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
