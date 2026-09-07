// Host tests for the master block registry (v1.6.0 Phase A).
//
// Own main(), like every other suite binary: a broken registry must not stop the
// world suite from running, and two mains cannot share one link. The registry is
// plain C with no <3ds.h>, so all of it is provable here in seconds.
//
// What each probe guards:
//   testRegistryRoundTrip    - register/find/duplicate/full-range/unknown-id contract
//   testRegistryCoreIdsStable- ids 1..14 byte-identical after InitCore; every saved
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

#if defined(_WIN32)
#include <process.h>   // _getpid — MinGW keeps it here, not in <unistd.h>
#else
#include <unistd.h>    // getpid
#endif

#include "world/block.h"
#include "world/registry.h"

// ---------------------------------------------------------------------------
// The dynamic block-id range, pinned to hand-written literals.
//
// This is the most important range in the block system: REG_ID_DYN_LO..REG_ID_DYN_HI is
// how many block types a server may add at join time, so it is the ceiling on what the
// game can represent beyond the fifteen compiled-in core rows (ten until v1.8.3 Phase 3
// appended snow, ice, cactus, dead_bush and fern at ids 10..14).
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
#define REGISTRY_FULL_COUNT_PIN 170   // 44 core rows (air + forty-three) + 126 dyn rows

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
//
// 54 -> 89 on 2026-08-30 by v1.8.3 Phase 3, counted off the source first and only then
// compared with the run:
//
//    +5   testRegistryCoreIdsStable's name loop runs to 15 instead of 10, and its body is
//         one check() per id — snow, ice, cactus, dead_bush, fern.
//   +30   the kPhase3 table in the same function: five rows x six checks (solid,
//         transparent, not-liquid, shape, tile-on-every-face, past the item ceiling).
//
// 54 + 35 = 89, and the guard printed "expected 54, ran 89" — the same 89, arrived at from
// the other direction. Nothing else in this suite changed count: the crc golden moved VALUE
// (0x4066 -> 0x189B, see testRegistryCrcStability) but it is still exactly one check, and
// REGISTRY_FULL_COUNT_PIN moved value too without adding a call.
//
// 89 -> 106 on 2026-09-02 (v1.8.8), and the arithmetic is written out for the same reason the
// paragraphs above are — so the next person can tell an intended change from a shrunk loop.
// coreHardnessIsDeclared() adds 16 calls: 13 inside its per-row loop, plus 3 fixed ones — the
// loop-ran counter, the cactus-is-distinct check and the undefined-id control. 89 + 16 = 105.
//
// The 13 is worth spelling out because the first attempt at this number was 14 and the run
// said 13. Fifteen core rows, LESS AIR (the loop starts at id 1, since air has no hardness to
// declare) and LESS WATER (a liquid, the one exemption). Two subtractions, not one. The check
// count guard is what caught it, which is the job it was put there to do.
//
// The crc golden moved VALUE again (0x189B -> 0xBDC5) and, as before, that is still exactly
// one check.
//
// 105 -> 117 on 2026-09-02, v1.8.8's per-biome blocks and plants. Twelve core rows are added
// — birch log/planks/leaves, spruce log/planks/leaves, the tall-grass top, four flowers and
// the apple — and the arithmetic is ALL of it, from one loop:
//
//   +12   coreHardnessIsDeclared()'s per-row loop, one check() per targetable core row. It
//         ran over 13 rows and now runs over 25, and 25 - 13 = 12.
//
// Nothing else in this suite gained a call. In particular the two places that look like they
// should have did not: testRegistryCoreIdsStable's name loop and its kPhase3 table are both
// written against a FIXED list of ids, so twelve new registry rows do not enter either one.
// That is why 117 and not something larger, and it was predicted before the run rather than
// pasted off it — the run printed "expected 105, ran 117", the same 12 from the other side.
//
// The crc golden moved VALUE a third time (0xBDC5 -> 0xD236) and, as every time before, that
// is still exactly one check.
//
// 117 -> 118 on 2026-09-02, v1.8.10 "Light"'s torch. One core row is appended (id 27), and
// coreHardnessIsDeclared()'s per-row loop runs one more iteration, so its
// check(v->hardness != 0, v->name) call fires once more: 117 + 1 = 118. Nothing else in this
// suite gained a call, for the same reason nothing else did in the twelve-row move above —
// testRegistryCoreIdsStable and its kPhase3 table are both written against fixed id lists, and
// the crc golden is still exactly one check regardless of what value it holds.
//
// 118 -> 125 on 2026-09-02/03, v1.8.12 "Ores"'s six ore rows (ids 28..33). Counted off the
// source first, same discipline as every entry above:
//
//   +6   coreHardnessIsDeclared()'s per-row loop runs six more iterations, one
//        check(v->hardness != 0, v->name) each for coal/iron/gold/redstone/lapis/diamond.
//   +1   the new ore hardness-ladder pin, the six-ore analogue of the snow/cactus/ice
//        distinctness check just above it.
//
// 118 + 6 + 1 = 125. Nothing else in this suite gained a call: testRegistryCoreIdsStable and
// its kPhase3 table are both written against fixed id lists, and the crc golden is still
// exactly one check regardless of what value it holds (0x165E -> 0xE15E this time).
//
// 125 -> 130 on 2026-09-03, v1.8.14 "Animals"'s four raw meat rows (ids 34..37). Counted off
// the source first, same discipline as every entry above:
//
//   +4   coreHardnessIsDeclared()'s per-row loop runs four more iterations, one
//        check(v->hardness != 0, v->name) each for porkchop/beef/chicken/mutton.
//   +1   the new raw-meat hardness-ladder pin, the four-row analogue of the six-ore ladder
//        check and of the snow/cactus/ice trio above it.
//
// 125 + 4 + 1 = 130. Nothing else in this suite gained a call, for the same reason nothing
// else did in the six-row move above: testRegistryCoreIdsStable and its kPhase3 table are
// both written against FIXED id lists, so four new registry rows do not enter either one, and
// the crc golden is still exactly one check regardless of what value it holds (0xE15E ->
// 0x9610 this time). REGISTRY_FULL_COUNT_PIN moved value too (160 -> 164) without adding a
// call, exactly as it has every previous time.
//
// 130 -> 137 on 2026-09-03, v1.8.15 "Furnace"'s five rows (ids 38..42: the four cooked meats
// and the furnace). Counted off the source first, same discipline as every entry above:
//
//   +5   coreHardnessIsDeclared()'s per-row loop runs five more iterations, one
//        check(v->hardness != 0, v->name) each for cooked_porkchop/cooked_beef/
//        cooked_chicken/cooked_mutton/furnace.
//   +1   the new cooked-meat hardness-ladder pin, the four-row analogue of the raw-meat
//        ladder check just above it (and of the ore ladder and the snow/cactus/ice trio
//        above that).
//   +1   the new furnace hardness pin — not a ladder, one block and one `==`, but pinned by
//        name for the same reason the ladders are: 45 is stone's value on purpose, and a
//        bare nonzero check would not catch it silently drifting off that.
//
// 130 + 5 + 1 + 1 = 137. Nothing else in this suite gained a call, for the same reason nothing
// else did in every move above: testRegistryCoreIdsStable and its kPhase3 table are both
// written against FIXED id lists, so five new registry rows do not enter either one, and the
// crc golden is still exactly one check regardless of what value it holds (0x9610 -> 0xE486
// this time). REGISTRY_FULL_COUNT_PIN moved value too (164 -> 169) without adding a call,
// exactly as it has every previous time.
//
// Predicted (137) before the run, same discipline as the 117 move's comment describes; the
// scratchpad WSL run of this exact suite after all of the above edits confirmed it: see this
// lane's final report for the captured PASS transcript.
//
// 137 -> 139 on 2026-09-05, v1.9.0 "Storage"'s one row (id 43: the chest). Counted off the
// source first, same discipline as every entry above:
//
//   +1   coreHardnessIsDeclared()'s per-row loop runs one more iteration, the
//        check(v->hardness != 0, v->name) for chest.
//   +1   the new chest hardness pin — not a ladder, one block and one `==`, the same shape
//        as the furnace pin just above it (and for the same reason: 40 is planks' value on
//        purpose, and a bare nonzero check would not catch it silently drifting off that).
//
// 137 + 1 + 1 = 139. Nothing else in this suite gained a call, for the same reason nothing
// else did in every move above: testRegistryCoreIdsStable and its kPhase3 table are both
// written against FIXED id lists, so one new registry row does not enter either one, and the
// crc golden is still exactly one check regardless of what value it holds. REGISTRY_FULL_COUNT_PIN
// moved value too (169 -> 170) without adding a call, exactly as it has every previous time.
//
// Predicted (139) before the run; to be confirmed against the real WSL run of this exact
// suite after all of the above edits, same discipline as every prior move.
#define REGISTRY_TEST_EXPECTED_CHECKS 139

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
	// Twenty-seven core rows since v1.8.10: grass..planks are the seven that also carry ITEM
	// ids, plus water (0x08) and tall grass (0x09) from roadmap tasks 17 and 19, plus
	// snow (0x0A), ice (0x0B), cactus (0x0C), dead bush (0x0D) and fern (0x0E), plus the
	// twelve v1.8.8 adds — birch log/planks/leaves (0x0F..0x11), spruce log/planks/leaves
	// (0x12..0x14), the tall-grass top (0x15), poppy/daisy/bluebell/orchid (0x16..0x19) and
	// the apple (0x1A) — plus v1.8.10's torch (0x1B), the first light source, plus v1.8.12's
	// six ores (0x1C..0x21): coal/iron/gold/redstone/lapis/diamond, plus v1.8.14's four raw
	// meats (0x22..0x25): porkchop/beef/chicken/mutton, plus v1.8.15's four cooked meats and
	// the furnace (0x26..0x2A): cooked_porkchop/cooked_beef/cooked_chicken/cooked_mutton/
	// furnace, plus v1.9.0's chest (0x2B). Every one of the last thirty-six is a core block
	// and deliberately NOT an item — world/block.h records why BLOCK_COUNT stayed at 8 while
	// the registry's row count moved to 44.
	check(registryCount() == 44, "a fresh table defines exactly air + the forty-three core blocks");
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
	         "count reflects every defined row once the range is full: 43 core + 126 dyn",
	         kDynRangeWhy);
}

static void testRegistryCoreIdsStable(void)
{
	puts("registry: core ids 1..14 are identical after InitCore");

	registryInitCore();

	// These fifteen checks ARE the save-format guarantee: a region file written by
	// any earlier build decodes by raw id, so if grass ever stops being 1 every
	// old world silently re-textures. Names first, then the texture rows.
	//
	// The last five are v1.8.3 Phase 3's, and they join this list the moment they exist
	// rather than once a server has shipped them: the guarantee is about what a saved byte
	// MEANS, and it starts applying the first time a world is generated with one in it.
	static const char *const want_names[] = {
		"air", "grass", "dirt", "stone", "sand", "wood", "leaves", "planks",
		"water", "tall_grass",
		"snow", "ice", "cactus", "dead_bush", "fern",
	};
	for (BlockId id = 0; id < 15; id++) {
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

	// v1.8.3 Phase 3's five, ids 0x0A..0x0E. Pinned for the same reason as every row above,
	// and per-FLAG rather than only by name: the crc golden already notices any byte moving,
	// but it says nothing about WHICH byte, and these five rows' behaviour is entirely in
	// their flags. A row that quietly lost REG_FLAG_SOLID would keep its name, keep its crc
	// contribution shape, and become a block the player falls through.
	//
	// The tile assertions are the half this project has been bitten by before: a wrong
	// texture constant still renders A texture, so it looks like bad art and never raises
	// anything (world/atlas_uv.h says so). Each of the five is pinned to its own BTEX_*, and
	// world/block_tiles_check.c is what ties those to gfx/atlas_tiles.h.
	static const struct {
		BlockId     id;
		const char *name;
		bool        solid;
		bool        transparent;
		uint8_t     shape;
		uint8_t     tex;
	} kPhase3[] = {
		{ (BlockId)BLOCK_SNOW,      "snow",      true,  false, BLOCK_SHAPE_FULL_CUBE, BTEX_SNOW      },
		{ (BlockId)BLOCK_ICE,       "ice",       true,  false, BLOCK_SHAPE_FULL_CUBE, BTEX_ICE       },
		{ (BlockId)BLOCK_CACTUS,    "cactus",    true,  false, BLOCK_SHAPE_FULL_CUBE, BTEX_CACTUS    },
		{ (BlockId)BLOCK_DEAD_BUSH, "dead_bush", false, true,  BLOCK_SHAPE_CROSS,     BTEX_DEAD_BUSH },
		{ (BlockId)BLOCK_FERN,      "fern",      false, true,  BLOCK_SHAPE_CROSS,     BTEX_FERN      },
	};
	for (size_t i = 0; i < sizeof kPhase3 / sizeof kPhase3[0]; i++) {
		const BlockDef *d = registryGet(kPhase3[i].id);
		char what[96];

		snprintf(what, sizeof what, "%s is solid=%d", kPhase3[i].name, kPhase3[i].solid);
		check(((d->flags & REG_FLAG_SOLID) != 0) == kPhase3[i].solid, what);

		snprintf(what, sizeof what, "%s is transparent=%d", kPhase3[i].name,
		         kPhase3[i].transparent);
		check(((d->flags & REG_FLAG_TRANSPARENT) != 0) == kPhase3[i].transparent, what);

		// Not one of the five is a liquid, and that is load-bearing rather than incidental:
		// blockIsTargetable() is `drawn && !liquid`, so a liquid flag here would make the
		// block un-aimable and un-buildable-against — which for ice, the surface you stand
		// on, would read as the crosshair falling through a frozen lake.
		snprintf(what, sizeof what, "%s is not a liquid, so the crosshair CAN stop on it",
		         kPhase3[i].name);
		check(!(d->flags & REG_FLAG_LIQUID), what);

		snprintf(what, sizeof what, "%s has shape %u", kPhase3[i].name,
		         (unsigned)kPhase3[i].shape);
		check(regShapeOf(d->flags) == kPhase3[i].shape, what);

		snprintf(what, sizeof what, "%s wears tile %u on every face", kPhase3[i].name,
		         (unsigned)kPhase3[i].tex);
		bool all_faces = true;
		for (int f = 0; f < BLOCK_FACES; f++)
			if (d->tex[f] != kPhase3[i].tex) all_faces = false;
		check(all_faces, what);

		// The item ceiling, asserted from the row rather than assumed from the id. All five
		// sit past BLOCK_COUNT, so scene/interact.c's guard refuses the break for the three
		// cubes and allows it (yielding nothing) for the two CROSS plants. This check is what
		// goes red if somebody widens BLOCK_COUNT rather than the predicate — the move
		// world/block.h spends forty lines forbidding.
		snprintf(what, sizeof what, "%s is past the item ceiling and cannot reach the bag",
		         kPhase3[i].name);
		check(kPhase3[i].id >= BLOCK_COUNT, what);
	}
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
	//
	// Moved 0x4066 -> 0x189B on 2026-08-30 by v1.8.3 Phase 3, which appends FIVE core rows —
	// snow (0x0A), ice (0x0B), cactus (0x0C), dead bush (0x0D) and fern (0x0E). This is the
	// same shape of move as tasks 17/19 above and NOT the same shape as task 50's: task 50
	// changed byte values inside existing records and left registryCount() at 10, whereas
	// this adds five whole 28-byte records, so the count moves 10 -> 15 as well and BOTH
	// halves of registryMatchesInfo() disagree with an older server rather than just the crc.
	//
	// **How this was told apart from breakage, rather than re-pinned to whatever ran.** The
	// value came from a probe linking this tree's real world/registry.c and world/block.c
	// (scratchpad p3_crc_probe.c) which prints the row table alongside the crc: it shows
	// rows 0..9 unchanged in every field — name, solid, transparent, liquid, shape, hardness
	// and tex[0] — and five new rows at 10..14 carrying tex 12..16. An appended record can
	// only extend the hash, and the count check above pins the extension to exactly five. If
	// rows 0..9 had moved, that table is where it would have shown, and the pin would have
	// been a bug to find rather than a number to update.
	//
	// The cost is the identical one recorded above and is why the SERVER SHIPS FIRST: a
	// client carrying these rows against a server that does not fails registryMatchesInfo()
	// and finishes with s_reg_synced false, which is degraded but visible. The reverse — a
	// client WITHOUT them against a server that has them — is the one that must never ship,
	// because deps/blocksmith-server/game/bsgame.c accepts the join anyway and every id 10..14
	// in the world resolves through registryGet()'s never-NULL contract to the AIR row: an
	// invisible hole indistinguishable from a cave.
	//
	// Moved 0x189B -> 0xBDC5 on 2026-09-02 by v1.8.8, and this move is a THIRD shape, unlike
	// either above: no record was added (registryCount() stays 15) and no row was renamed or
	// reflagged. Exactly one byte inside one existing record changed value — the cactus row's
	// .hardness, 8 -> 9 — which makes it task 50's shape, not Phase 3's, so registryCount()
	// still agrees with an older server and only the crc half of registryMatchesInfo()
	// disagrees.
	//
	// **How this was told apart from breakage.** Measured by ABLATION, not by subtracting one
	// total from another (scratchpad blockceil_crc_ablate.sh, which builds two arms of the
	// real world/registry.c and asserts they differ on exactly one line):
	//
	//     arm A  ceiling widened + cactus hardness 9  ->  0xBDC5
	//     arm B  ceiling widened + cactus hardness 8  ->  0x189B   (the golden above, exactly)
	//
	// Arm B reproducing the shipped golden byte-for-byte is the finding that matters: v1.8.8's
	// item-ceiling change is entirely CRC-NEUTRAL. It moved inventoryCanHold() from a
	// `< BLOCK_COUNT` comparison to a registry query, and a predicate stores nothing, so it
	// cannot touch the packed table the hash runs over. The WHOLE of this crc move — and
	// therefore the whole of its cross-repo cost — belongs to the cactus durability retune.
	//
	// The cost, stated plainly because it is not free: registryMatchesInfo() failing is a hard
	// join refusal ("Server is a different Blocksmith version - update"), so cactus hardness 9
	// must ship as a LOCKSTEP client+server release, the same procedure this repo ran for
	// 0x72A8 -> 0x4066 and 0x4066 -> 0x189B. BS_PROTO_VERSION stays 1 through it, as it did
	// both previous times; it gates transport packet types and none of those changed.
	// deps/blocksmith-server/game/bsgame_test.c's BS_REGISTRY_CORE_CRC16_GOLDEN must move
	// with this literal or that repo's suite goes red.
	// Moved 0xBDC5 -> 0xD236 on 2026-09-02 by v1.8.8's per-biome blocks and plants, and this
	// move is Phase 3's shape rather than the cactus retune's directly above: TWELVE records
	// were APPENDED and no existing record was touched. registryCount() moves 15 -> 27 with
	// it, so the count half of registryMatchesInfo() disagrees too, not only the crc half.
	//
	// Measured on the live table rather than reasoned about, by a probe linking the real
	// world/registry.c (scratchpad biomeblocks_crc.c): count=27, crc=0xD236, and — the check
	// that matters for "every new block is breakable with its own durability" — 25 targetable
	// core rows with ZERO of them at hardness 0.
	//
	// **Same cost, same order, and it is bigger than the cactus one.** A client carrying these
	// twelve rows against a server without them fails registryMatchesInfo() and refuses the
	// join outright. The reverse is the dangerous direction and is worse here than it was for
	// Phase 3: a client WITHOUT these rows joining a server that HAS them resolves every id
	// 0x0F..0x1A through registryGet()'s never-NULL contract to the AIR row, so every birch
	// tree, every spruce tree and every flower in the world becomes an invisible hole. So the
	// SERVER SHIPS FIRST, exactly as it did for 0x72A8 -> 0x4066, 0x4066 -> 0x189B and
	// 0x189B -> 0xBDC5. BS_PROTO_VERSION stays 1: it gates transport packet types and none of
	// those changed. deps/blocksmith-server/game/bsgame_test.c's BS_REGISTRY_CORE_CRC16_GOLDEN
	// and BS_REGISTRY_CORE_COUNT_GOLDEN must both move with these literals or that repo's
	// suite goes red.
	//
	// MOVED A FOURTH TIME 2026-09-02, 0xD236 -> 0x165E, by v1.8.10 "Light"'s torch (id 27, the
	// first light source). One record appended, Phase 3's shape again: registryCount() moves
	// 27 -> 28. torch is BLOCK_SHAPE_CROSS, non-solid, luminance 14, and .hardness 1 — the
	// smallest nonzero value the byte allows, since 0 means "no break time at all" and is
	// reserved for water; every other targetable core row, torch included, is nonzero for the
	// same reason coreHardnessIsDeclared() below exists to enforce.
	//
	// Measured the same way as every move above: a scratchpad probe (torchreg_crc.c) linking
	// this tree's real world/registry.c and world/block.c, no test file linked so the golden
	// is unreachable from the binary being measured. Printed
	//
	//     count=28 crc=0x165E
	//     targetable-rows-checked, zero-hardness-count=0
	//     torch: name=torch hardness=1 luminance=14 flags=0x2A tex0=31
	//
	// and cross-checked by compiling the identical probe against
	// deps/blocksmith-server/game/world/registry.c after tools/sync-world-sources.sh ran (it
	// reported block.h and registry.c synced, the other nine files unchanged, diff -q
	// confirming both trees byte-identical): count=28 crc16=0x165E rev=1, the same number from
	// the other side.
	//
	// SERVER SHIPS FIRST, same reason as every move above: a v1.8.9 client on a v1.8.10 server
	// cannot name id 27 and would render every torch as an unlit hole.
	// deps/blocksmith-server/game/bsgame_test.c's BS_REGISTRY_CORE_CRC16_GOLDEN and
	// BS_REGISTRY_CORE_COUNT_GOLDEN move with this literal.
	//
	// MOVED A FIFTH TIME 2026-09-02/03, 0x165E -> 0xE15E, by v1.8.12 "Ores"'s six ore rows
	// (ids 28..33: coal/iron/gold/redstone/lapis/diamond). registryCount() moves 28 -> 34.
	// No tool-tier gate in this version — every ore is breakable by hand, each with its own
	// hardness (a six-step ladder, 60/70/80/85/90/100, pinned just below by
	// coreHardnessIsDeclared() so it cannot be flattened to one value later).
	//
	// Measured the same way as every move above: a scratchpad probe
	// (oreblocks_crc_probe.c) linking this tree's real world/registry.c and world/block.c,
	// no test file linked so the golden is unreachable from the binary being measured.
	// Printed:
	//
	//     count=34 crc=0xE15E
	//     id=28 name=coal_ore     hardness= 60 flags=0x01 tex0=32
	//     id=29 name=iron_ore     hardness= 70 flags=0x01 tex0=33
	//     id=30 name=gold_ore     hardness= 85 flags=0x01 tex0=34
	//     id=31 name=redstone_ore hardness= 90 flags=0x01 tex0=35
	//     id=32 name=lapis_ore    hardness= 80 flags=0x01 tex0=36
	//     id=33 name=diamond_ore  hardness=100 flags=0x01 tex0=37
	//     zero-hardness-count=0
	//
	// and cross-checked by compiling the identical probe against
	// deps/blocksmith-server/game/world/registry.c (block.h and registry.c copied byte-for-byte
	// from this tree; `cmp` on all eleven mirrored files confirmed the other nine untouched and
	// these two identical): count=34 crc=0xE15E, the same number from the other side.
	//
	// Corrected 2026-09-03: this parenthesis used to assert "there is no
	// tools/sync-world-sources.sh in this repo despite this comment block's earlier entries
	// assuming one." That was wrong, and the earlier entries were right. The script exists, at
	// deps/blocksmith-server/tools/sync-world-sources.sh — i.e. under the SERVER repo, which is
	// the root every comment citing a bare `tools/sync-world-sources.sh` is written relative to.
	// It does not resolve from the CLIENT repo root, and that failure to resolve got recorded
	// as the script not existing at all. It does exist and it works: run from
	// deps/blocksmith-server it reported all eleven files `unchanged`, "game/world/ was already
	// in sync", exit 0. It is self-locating (resolves off BASH_SOURCE, not cwd), its FILES=()
	// array at line 46 is exactly the eleven mirrored files, and game/Makefile's
	// `check-world-drift` target names it as the fix when the two trees diverge.
	//
	// Worth the space because the wrong version of this sentence is actively harmful: a reader
	// who believes there is no sync tool hand-copies the mirror, which is precisely how eleven
	// files that must stay byte-identical drift apart.
	//
	// SERVER SHIPS FIRST, same reason as every move above: a v1.8.11 client on a v1.8.12
	// server cannot name ids 28..33 and would render every ore as an invisible hole.
	// deps/blocksmith-server/game/bsgame_test.c's BS_REGISTRY_CORE_CRC16_GOLDEN and
	// BS_REGISTRY_CORE_COUNT_GOLDEN move with this literal.
	// MOVED A SIXTH TIME 2026-09-03, 0xE15E -> 0x9610, by v1.8.14 "Animals"'s four raw meat
	// rows (ids 34..37: raw_porkchop/raw_beef/raw_chicken/raw_mutton). registryCount() moves
	// 34 -> 38, the same shape as every move above: four records APPENDED, nothing renumbered,
	// so REGISTRY_REV stays 1.
	//
	// ⚠ 34..37 AND NOT 27..30. docs/plan-1.8.14-animals.md names 27..30 for these rows and it
	// is STALE — written before v1.8.10's torch took 27 and before v1.8.12's six ores took
	// 28..33. The first free id was read off BLOCK_DIAMOND_ORE == 33 in this tree rather than
	// off the plan. Recorded here because a reader who trusts that document over this table
	// renumbers four ids that are already on the wire.
	//
	// All four are FULL_CUBE and SOLID, following the apple rather than the plants: a CROSS
	// row is handed BLOCK_AIR by breakComplete(), i.e. an animal you kill and get nothing
	// from. Each carries its own hardness, a four-step ladder ordered by the size of the
	// animal — chicken 3 < porkchop 4 < mutton 5 < beef 6 — pinned as a ladder by
	// coreHardnessIsDeclared() below so it cannot be flattened to one value later.
	//
	// Measured the same way as every move above: a scratchpad probe (animb_meat_crc_probe.c)
	// linking this tree's real world/registry.c and world/block.c, no test file linked so the
	// golden is unreachable from the binary being measured. Printed:
	//
	//     count=38 crc=0x9610 rev=1
	//     id=34 name=raw_porkchop  hardness=  4 flags=0x01 tex0=38 solid=1 liquid=0
	//     id=35 name=raw_beef      hardness=  6 flags=0x01 tex0=39 solid=1 liquid=0
	//     id=36 name=raw_chicken   hardness=  3 flags=0x01 tex0=40 solid=1 liquid=0
	//     id=37 name=raw_mutton    hardness=  5 flags=0x01 tex0=41 solid=1 liquid=0
	//     targetable-rows=36 zero-hardness-count=0
	//
	// and cross-checked by compiling the identical probe against
	// deps/blocksmith-server/game/world/registry.c after tools/sync-world-sources.sh ran (it
	// reported block.h and registry.c `synced` and the other nine `unchanged`; a following
	// cmp over all eleven reported every one identical): count=38 crc=0x9610, the same number
	// from the other side.
	//
	// SERVER SHIPPED FIRST, and this time that is past tense rather than an instruction: the
	// server release is blocksmith-server v1.9.4, commit a22eea3a, tag v1.9.4, published
	// before this literal moved. Its game/bsgame_test.c carries the mirrored
	// BS_REGISTRY_CORE_CRC16_GOLDEN 0x9610 / BS_REGISTRY_CORE_COUNT_GOLDEN 38, and Makefile's
	// PROTO_COMMIT was bumped to that commit.
	//
	// MOVED A SEVENTH TIME 2026-09-03, 0x9610 -> 0xE486, by v1.8.15 "Furnace"'s five rows:
	// the four cooked meats (ids 38..41: cooked_porkchop/cooked_beef/cooked_chicken/
	// cooked_mutton) and the furnace itself (id 42). registryCount() moves 38 -> 43, the same
	// shape as every move above: five records APPENDED, nothing renumbered, so REGISTRY_REV
	// stays 1.
	//
	// Each cooked meat keeps its raw counterpart's hardness exactly (world/registry.c's own
	// row comment says why: the four-step ladder chicken 3 < porkchop 4 < mutton 5 < beef 6
	// is preserved rather than re-derived). The furnace is FULL_CUBE/SOLID with hardness 45,
	// matching BLOCK_STONE — it is stone-built, and only its FACE_SOUTH ("front") tile
	// differs from plain stone; the other five faces reuse BTEX_STONE. Both facts are pinned
	// as a ladder and a hardness value respectively by coreHardnessIsDeclared() below.
	//
	// Measured the same way as every move above: a scratchpad probe (furnacec_crc_probe.c)
	// linking this tree's real world/registry.c and world/block.c, no test file linked so the
	// golden is unreachable from the binary being measured. Printed:
	//
	//     count=43 crc=0xE486 rev=1
	//     id=38 name=cooked_porkchop  hardness=  4 flags=0x01 tex0=42 solid=1 liquid=0
	//     id=39 name=cooked_beef      hardness=  6 flags=0x01 tex0=43 solid=1 liquid=0
	//     id=40 name=cooked_chicken   hardness=  3 flags=0x01 tex0=44 solid=1 liquid=0
	//     id=41 name=cooked_mutton    hardness=  5 flags=0x01 tex0=45 solid=1 liquid=0
	//     id=42 name=furnace          hardness= 45 flags=0x01 tex0=3  solid=1 liquid=0
	//     targetable-rows=41 zero-hardness-count=0
	//
	// tex0 for the furnace row is 3 (BTEX_STONE), FACE_EAST — expected, since only
	// world/registry.c's furnace row's FACE_SOUTH slot (tex[4]) carries BTEX_FURNACE_FRONT;
	// every other face reuses stone's tile.
	//
	// NOT cross-checked against deps/blocksmith-server this time: this lane (FURNACE-C) is
	// explicitly forbidden from touching deps/blocksmith-server or mc/server/, and
	// tools/sync-world-sources.sh writes into that tree. SERVER SHIPS FIRST still applies —
	// the coordinator or wiring lane must run tools/sync-world-sources.sh from
	// deps/blocksmith-server, confirm world/registry.c and world/block.h report `synced`, and
	// bump BS_REGISTRY_CORE_CRC16_GOLDEN to 0xE486 / BS_REGISTRY_CORE_COUNT_GOLDEN to 43 in
	// deps/blocksmith-server/game/bsgame_test.c (plus PROTO_COMMIT in this repo's Makefile)
	// before this client's furnace rows are safe to run against a live server — an old server
	// refuses a mismatched crc at join time, so this is a safety refusal, not silent
	// corruption, but it does mean single-player only until that ships.
	//
	// MOVED AN EIGHTH TIME 2026-09-05, 0xE486 -> 0x2A61, by v1.9.0 "Storage"'s one row (id 43:
	// the chest, hardness 40 matching BLOCK_PLANKS, tex0=9 == BTEX_PLANKS on every face but
	// FACE_TOP). registryCount() moves 43 -> 44, the same shape as every move above: one
	// record APPENDED, nothing renumbered, so REGISTRY_REV stays 1.
	//
	// Measured the same way as the furnace move: a scratchpad probe (chestc_crc_probe.c)
	// linking this tree's real world/registry.c and world/block.c, no test file linked so the
	// golden pin is unreachable from the binary being measured. Printed:
	//
	//     count=44 crc=0x2A61 rev=1
	//     id=43 name=chest    hardness= 40 flags=0x01 tex0=9  solid=1 liquid=0
	//     targetable-rows=42
	//
	// NOT the patch's reconstructed guess of 0xE8BD — docs/chest-paused-v1.9.0.patch.txt says
	// plainly that its numbers were RECONSTRUCTED, not re-derived, and this is exactly the
	// case that warns about: 0x2A61 is what the real content stream hashes to, measured, not
	// assumed.
	//
	// NOT cross-checked against deps/blocksmith-server this time either, for the identical
	// off-limits reason the furnace move states above: bumping BS_REGISTRY_CORE_CRC16_GOLDEN
	// and BS_REGISTRY_CORE_COUNT_GOLDEN in that tree is a separate lane's job.
	check(base == 0x2A61u,
	      "core-only crc matches the pinned golden 0x2A61");

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

	// Derived from the pid rather than the fixed "build-host/registry_sidecar_probe.bin" it
	// used to be. tools/run_host_tests.sh already gives every binary a pid-scoped build
	// directory, so two concurrent runs get two different EXECUTABLES writing to one shared
	// SIDECAR -- and the remove() at the bottom of this function then lands between another
	// run's save and its load.
	//
	// That is not a hypothetical: it is the measured cause of an earlier "FAIL 89 checks,
	// 5 failed" on a suite that passed on re-run, and a flake that moves with system load is
	// the worst kind to leave in, because the next person to see it spends the time ruling
	// out whatever they had just changed. Same fix and same reasoning as world_test.c's
	// testWorldDir(), which pid-scopes its region directory for exactly this history.
	static char path[64];
	if (path[0] == '\0') {
#if defined(_WIN32)
		const long pid = (long)_getpid();
#else
		const long pid = (long)getpid();
#endif
		snprintf(path, sizeof path, "build-host/registry_sidecar_probe-%ld.bin", pid);
	}
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

// v1.8.8. THE rule the roadmap asked for in steve's words — "make sure every single new block
// you add is breakable with its own durability" — written as a check rather than as a note, so
// forgetting it fails the build instead of shipping.
//
// Why it can fail silently without this. world/registry.c's kCoreDefs is a designated-
// initialiser table: a row that simply omits `.hardness` compiles clean and gets 0 from the
// zero-fill. There is no "unset" to detect and no default worth having, because 0 already
// MEANS something — world/mining.c's breakTicksRequired() returns 0 for it, and 0 ticks is an
// instant break on the press edge. So the missing field does not produce an error, or a slow
// block, or a hard block. It produces a block that shatters the moment the button goes down,
// which reads as a gameplay decision rather than as a mistake.
//
// The exemption is narrow on purpose: a LIQUID, and nothing else. blockIsTargetable() is
// `drawn && !liquid`, so no crosshair ever lands on one and no break timer can ever ask it
// for a number. Any other row with hardness 0 is a bug, and this is where it stops.
//
// This lives in the registry suite because it is a fact about the TABLE. world/mining_test.c
// states the identical rule over blockHardnessTicks(), which is the same fact read through
// the consumer. Two suites because they go red at different moments and a table can be wrong
// in ways mining never asks about.
static void coreHardnessIsDeclared(void)
{
	puts("registry: every targetable core row declares a break time of its own");

	registryInitCore();

	int rows = 0;
	for (BlockId id = 1; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined(id)) continue;

		// registryView(), not registryGet(): Get hands back the packed BlockDef whose
		// liquid-ness lives in a flags BIT, View hands back the unpacked BlockInfo with the
		// bool already broken out. Reading the wrong one does not compile, which is the
		// cheapest possible way for this distinction to be enforced.
		const BlockInfo *v = registryView(id);
		if (v->liquid) continue;   // the only exemption, and it is earned: never targetable

		rows++;
		check(v->hardness != 0, v->name);
	}

	// The loop ran, and over the whole core id space rather than a prefix of it. Without this
	// the rule is green in a build where registryIsDefined() answers false for everything —
	// a check that cannot go red proves nothing, and a `continue` is the easiest way to
	// neutralise one by accident.
	check(rows == 42, "and it ran over 42 rows: 44 core rows less air and less water");

	// A row must have its OWN number, not a neighbour's. The loop above is satisfied by a
	// table where every hardness is 9, which is exactly the failure mode "make sure every
	// block is breakable with its own durability" is guarding against. The cactus is the case
	// that matters here — it is the block v1.8.8 was reported for, and it was given a value
	// deliberately WEDGED between snow's and ice's rather than parked somewhere distant,
	// because a value far from its neighbours would satisfy a distinctness check while
	// telling the player nothing.
	check(registryGet(BLOCK_CACTUS)->hardness == 9
	      && registryGet(BLOCK_SNOW)->hardness == 8
	      && registryGet(BLOCK_ICE)->hardness == 10,
	      "snow 8 < cactus 9 < ice 10: three neighbours, three break times");

	// v1.8.12's six ores, pinned as a LADDER and not merely as six nonzero bytes, for the
	// identical reason as the cactus/snow/ice trio just above. A flat value across all six
	// (the design document's own first draft) is exactly the failure mode this function
	// exists to catch: the per-row loop is satisfied by a table where every ore reads the
	// same number, and six identical hardness bytes tell the player nothing about which ore
	// is which. The ladder rises with depth and every step clears stone's 45.
	check(registryGet(BLOCK_COAL_ORE)->hardness == 60
	      && registryGet(BLOCK_IRON_ORE)->hardness == 70
	      && registryGet(BLOCK_LAPIS_ORE)->hardness == 80
	      && registryGet(BLOCK_GOLD_ORE)->hardness == 85
	      && registryGet(BLOCK_REDSTONE_ORE)->hardness == 90
	      && registryGet(BLOCK_DIAMOND_ORE)->hardness == 100,
	      "ore hardness is a six-step ladder, coal 60 < iron 70 < lapis 80 < gold 85 < "
	      "redstone 90 < diamond 100, every step above stone's 45");

	// v1.8.14's four raw meats, pinned as a LADDER for the identical reason the ore ladder
	// above and the cactus/snow/ice trio above that are: the per-row loop is satisfied by a
	// table where all four read the same number, and four identical break times tell the
	// player nothing about which cut is which. The ladder rises with the size of the animal
	// the meat comes off, every step clears the 1-tick floor the plants sit at, and every
	// step is far under stone's 45 — this is soft material and the numbers say so.
	//
	// Written as inequalities and not as four `==` lines on purpose. Four equalities are all
	// still true about a table where beef had simply borrowed the mutton row's byte; stated
	// as a strict ordering, that cannot happen quietly.
	check(registryGet(BLOCK_RAW_CHICKEN)->hardness == 3
	      && registryGet(BLOCK_RAW_PORKCHOP)->hardness == 4
	      && registryGet(BLOCK_RAW_MUTTON)->hardness == 5
	      && registryGet(BLOCK_RAW_BEEF)->hardness == 6
	      && registryGet(BLOCK_RAW_CHICKEN)->hardness < registryGet(BLOCK_RAW_PORKCHOP)->hardness
	      && registryGet(BLOCK_RAW_PORKCHOP)->hardness < registryGet(BLOCK_RAW_MUTTON)->hardness
	      && registryGet(BLOCK_RAW_MUTTON)->hardness < registryGet(BLOCK_RAW_BEEF)->hardness,
	      "raw meat hardness is a four-step ladder, chicken 3 < porkchop 4 < mutton 5 < "
	      "beef 6, every step above the plants' 1-tick floor and far under stone's 45");

	// v1.8.15's four cooked meats, pinned as the IDENTICAL ladder to the raw meats just above
	// and for the identical reason: world/registry.c's own row comments say each cooked block
	// keeps its raw counterpart's hardness rather than re-deriving one, on the grounds that
	// cooking changes what an item does when eaten, not how hard the placed block is to break.
	// Written the same way as the raw-meat check on purpose — as a strict ordering, not four
	// `==` lines — so the same "beef borrowed mutton's byte" failure mode is caught here too.
	check(registryGet(BLOCK_COOKED_CHICKEN)->hardness == 3
	      && registryGet(BLOCK_COOKED_PORKCHOP)->hardness == 4
	      && registryGet(BLOCK_COOKED_MUTTON)->hardness == 5
	      && registryGet(BLOCK_COOKED_BEEF)->hardness == 6
	      && registryGet(BLOCK_COOKED_CHICKEN)->hardness < registryGet(BLOCK_COOKED_PORKCHOP)->hardness
	      && registryGet(BLOCK_COOKED_PORKCHOP)->hardness < registryGet(BLOCK_COOKED_MUTTON)->hardness
	      && registryGet(BLOCK_COOKED_MUTTON)->hardness < registryGet(BLOCK_COOKED_BEEF)->hardness,
	      "cooked meat hardness matches its raw counterpart's ladder, chicken 3 < porkchop 4 "
	      "< mutton 5 < beef 6 — cooking changes what it feeds, not how hard it breaks");

	// v1.8.15's furnace. Not a ladder — one block, one number — but pinned the same way as
	// every other new-block row above rather than folded into the generic per-row loop's
	// nonzero check, because 45 is not an arbitrary nonzero byte: it is deliberately equal to
	// BLOCK_STONE's hardness. The furnace is a stone block with one re-painted face, and its
	// break time says so.
	check(registryGet(BLOCK_FURNACE)->hardness == 45
	      && registryGet(BLOCK_FURNACE)->hardness == registryGet(BLOCK_STONE)->hardness,
	      "furnace hardness is 45, matching stone: a stone block with one face re-painted");

	// v1.9.0's chest. The same shape as the furnace pin just above and for the identical
	// reason: 40 is not an arbitrary nonzero byte, it is deliberately equal to BLOCK_PLANKS'
	// hardness. The chest is a planks block with one re-painted face (its FACE_TOP, per
	// world/registry.c's chest row), and its break time says so.
	check(registryGet(BLOCK_CHEST)->hardness == 40
	      && registryGet(BLOCK_CHEST)->hardness == registryGet(BLOCK_PLANKS)->hardness,
	      "chest hardness is 40, matching planks: a planks block with one face re-painted");

	// CONTROL. An id with no row still reads back as air, hardness 0, and that must NOT trip
	// the rule above — the rule is about rows that exist. Green in every arm, including one
	// with every hardness in the table zeroed, which is what makes the red checks above
	// evidence rather than "the registry stopped answering".
	check(registryGet((BlockId)(REG_ID_CORE_HI))->hardness == 0,
	      "control: an undefined core id still reads back as air, so hardness 0");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== registry test ==");

	testRegistryRoundTrip();
	testRegistryCoreIdsStable();
	testRegistryCrcStability();
	testRegistrySidecar();
	coreHardnessIsDeclared();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
