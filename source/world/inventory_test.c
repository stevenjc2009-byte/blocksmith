// Host self-test for world/inventory.c and world/crafting.c. Self-contained (its own
// main(), same reasoning as app/options_test.c) rather than plugged into
// tools/run_host_tests.sh's shared harness — that harness and its build-host/ output are
// owned by whoever is wiring step 8.2's other pieces together right now.
//
// The CHECK macro and the PASS/FAIL summary line are copied in the same shape
// app/options_test.c and world/world_test.c use, so a failure here reads the same way a
// failure anywhere else in this codebase does.
//
// The __3DS__ guard below is load-bearing rather than tidy: the Makefile globs every .c
// under source/world into the console build, so without it this file's main() would link
// against source/main.c's and the build would die with "multiple definition of `main'".
// That exact bug already happened once today on this step — see the guard on
// app/options_test.c for the same note.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "world/block.h"
#include "world/crafting.h"
#include "world/inventory.h"
// v1.8.8: testTheBagTakesEveryDefinedBlock() walks the registry directly and registers a
// dynamic row, so this is included by name rather than leant on through inventory.h (which
// pulls it in since the ceiling became registry-driven). world/registry.c joined this binary's
// link line in the same change.
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// How many CHECK()s this suite makes on a healthy tree. A LITERAL on purpose.
//
// This suite is the one in source/world where the hazard is real rather than theoretical:
// its check count is driven by production constants. testAddToEmpty loops to
// INV_SLOT_COUNT, testEveryRecipeCrafts and its insufficient-inputs twin loop to
// RECIPE_COUNT, and the latter carries an `if (r->input_count == 0) continue;`. Shrink any
// of those in the real headers and checks stop RUNNING rather than start FAILING — and a
// suite that only reports failures calls that a pass. Measured elsewhere in this project:
// a shortened production-constant-bounded loop took the net suite from 326 checks to 318
// and it printed "0 failed".
//
// So this number is never derived from INV_SLOT_COUNT, RECIPE_COUNT or anything else the
// code under test can move. A pin computed from the constant it is pinning shrinks with the
// sabotage and guards nothing.
//
// Legitimately adding or removing a CHECK means editing this by hand. The suite going red
// until you do is deliberate friction, not an accident.
//
// 180 -> 212 on 2026-09-02 (v1.8.8), by DELTA rather than by pasting what the run printed.
// testTheBagTakesEveryDefinedBlock() is the only addition and it makes 32 calls: 4 on the
// cactus itself, 13 inside the per-row loop, 2 loop-ran counters, 3 on the defined/undefined
// boundary, 2 on the surviving exclusions (air, liquid), 6 on the dynamic id before and after
// registering plus the re-init, and 2 on the wire span. 180 + 32 = 212.
//
// The 13 is worth spelling out because the delta was first computed as 14 and the run said
// 212, not 213. Fifteen core rows, LESS AIR (the loop starts at id 1) and LESS WATER (a
// liquid). Two subtractions, not one — and the guard is what turned a miscount into a red
// line rather than into a quietly wrong pin.
//
// 212 -> 224 on 2026-09-02, same v1.8.8, later the same day: registry.c grew twelve more core
// rows (ids 15..26, the per-biome timber and flora) on top of the fifteen this file's previous
// entry above was counting against. Nothing in testTheBagTakesEveryDefinedBlock() changed
// shape — the per-row loop at line 217 is still one CHECK() per accepted id, it just now runs
// over twenty-five accepted ids (twenty-seven core rows less air, less water) instead of
// thirteen. Every other tally in the 32-call breakdown above — the 4 on the cactus, the 2
// loop-ran counters, the 3 boundary checks, the 2 exclusions, the 6 dynamic-id calls, the 2
// wire-span calls — is a fixed count untouched by how many rows the registry defines, so the
// whole delta is the loop body: +12. 212 + 12 = 224.
#define INVENTORY_TEST_EXPECTED_CHECKS 224

// Deliberately NOT routed through CHECK(): this must not perturb the number it is testing,
// so it bumps s_fails only. It fills s_first (with both numbers, so the one-line summary is
// self-explanatory) and prints the detail above that summary.
static void checkCountPin(void)
{
	if (s_checks == INVENTORY_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < INVENTORY_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran: a loop bound shrank, an early return or\n"
		       "  a continue fired, or a CHECK was deleted. The checks that did run passing\n"
		       "  tells you nothing about the ones that did not. Find them. Do NOT re-pin\n"
		       "  INVENTORY_TEST_EXPECTED_CHECKS to go green.\n",
		       INVENTORY_TEST_EXPECTED_CHECKS - s_checks,
		       INVENTORY_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         INVENTORY_TEST_EXPECTED_CHECKS - s_checks,
			         INVENTORY_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set INVENTORY_TEST_EXPECTED_CHECKS in\n"
		       "  source/world/inventory_test.c to %d. If you did not, something is running\n"
		       "  checks more times than it should.\n",
		       s_checks - INVENTORY_TEST_EXPECTED_CHECKS,
		       INVENTORY_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - INVENTORY_TEST_EXPECTED_CHECKS,
			         INVENTORY_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Copied
// from app/options_test.c's testMkdir, which copied it from world/world_test.c — same
// "this only runs on the host, and the host might be either" problem, solved the same way
// a third time rather than reinvented.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

#define TEST_DIR "build-host/invtest"

static const char* const s_test_paths[] = {
	TEST_DIR "/roundtrip/inventory.dat",
	TEST_DIR "/roundtrip/inventory.dat.tmp",
	TEST_DIR "/missing/inventory.dat",
	TEST_DIR "/corrupt/inventory.dat",
	TEST_DIR "/truncated/inventory.dat",
	TEST_DIR "/crash_recover/inventory.dat",
	TEST_DIR "/crash_recover/inventory.dat.tmp",
};

static void testCleanup(void)
{
	for (size_t i = 0; i < sizeof(s_test_paths) / sizeof(s_test_paths[0]); i++)
		remove(s_test_paths[i]);
	// Directories are left in place, same as options_test.c leaves TEST_DIR itself: a
	// second run recreates whatever it needs.
}

static bool writeRaw(const char* path, const void* data, size_t len)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

// Field-by-field rather than memcmp: InvSlot is two uint8_t fields and Inventory's only
// other member is a uint8_t, so there is no hidden padding to trip over here the way
// app/options_test.c's Options (bool then float) has — but comparing field-by-field costs
// nothing and means this helper stays correct even if a wider field is ever added.
static bool invEqual(const Inventory* a, const Inventory* b)
{
	if (a->selected_hotbar != b->selected_hotbar) return false;
	for (int i = 0; i < INV_SLOT_COUNT; i++) {
		if (a->slots[i].item  != b->slots[i].item)  return false;
		if (a->slots[i].count != b->slots[i].count) return false;
	}
	return true;
}

// ── Inventory: adding ─────────────────────────────────────────────────────────────────

static void testAddToEmpty(void)
{
	Inventory inv;
	inventoryInit(&inv);

	uint8_t leftover = 123;
	const InvAddResult r = inventoryAdd(&inv, BLOCK_DIRT, 10, &leftover);

	CHECK(r == INV_ADD_OK);
	CHECK(leftover == 0);
	CHECK(inv.slots[0].item  == BLOCK_DIRT);
	CHECK(inv.slots[0].count == 10);
	// Nothing past slot 0 was touched.
	for (int i = 1; i < INV_SLOT_COUNT; i++) {
		CHECK(inv.slots[i].item  == ITEM_NONE);
		CHECK(inv.slots[i].count == 0);
	}
}

// v1.8.8 — the item ceiling, through the API a pickup actually uses.
//
// Every other case in this file adds an id below 8, so the whole suite was green on a build
// where inventoryAdd() refused everything else. That is not hypothetical: it WAS that build,
// for five releases, and the way it surfaced was steve reporting that a cactus could not be
// broken.
//
// The boundary is asserted from BOTH sides and against the registry rather than a constant,
// because the id that must be refused is now "the one with no row", which moves whenever a row
// is added — as v1.8.8's per-biome blocks will.
static void testTheBagTakesEveryDefinedBlock(void)
{
	registryInitCore();

	Inventory inv;
	inventoryInit(&inv);

	// The reported case, end to end: a cactus goes in, and comes back out of the slot as a
	// cactus. Red on any pre-v1.8.8 build.
	uint8_t leftover = 123;
	CHECK(inventoryAdd(&inv, (ItemId)BLOCK_CACTUS, 5, &leftover) == INV_ADD_OK);
	CHECK(leftover == 0);
	CHECK(inv.slots[0].item  == (ItemId)BLOCK_CACTUS);
	CHECK(inv.slots[0].count == 5);

	// The rule, not the instance: every defined, non-liquid core row is accepted. A loop over
	// the id space with a definedness skip, so a row added tomorrow is covered without anyone
	// remembering to come back here.
	Inventory all;
	inventoryInit(&all);
	int accepted = 0;
	for (BlockId id = 1; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined(id)) continue;
		if (registryView(id)->liquid) continue;
		CHECK(inventoryCanHold((ItemId)id));
		accepted++;
	}
	// The loop ran, over the whole table rather than a prefix of it. Without this a
	// definedness query answering false for everything leaves the rule green having asserted
	// nothing at all.
	CHECK(accepted == 25);            // 27 core rows less air and less water
	CHECK(accepted > BLOCK_COUNT);    // and genuinely more than the old ceiling admitted

	// THE BOUNDARY, both sides, derived rather than hard-coded: the last id with a row is
	// accepted, the first id without one is refused.
	CHECK(inventoryCanHold((ItemId)BLOCK_APPLE));                 // 26, the last defined row
	CHECK(!inventoryCanHold((ItemId)(BLOCK_APPLE + 1)));          // 27, no row
	CHECK(!registryIsDefined((BlockId)(BLOCK_APPLE + 1)));        // ...and that is why

	// The two exclusions that survive the widening, each for its own reason. Neither of them
	// is about where the id sits.
	CHECK(!inventoryCanHold(ITEM_NONE));                         // air is not an item
	CHECK(!inventoryCanHold((ItemId)BLOCK_WATER));               // a liquid is not carriable

	// An id in the SERVER range is refused while undefined and accepted once it has a row —
	// the whole change, stated as one before/after pair. Before v1.8.8 the second of these
	// was false no matter what the server sent.
	CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_LO));
	BlockDef def;
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "%s", "bag_probe");
	def.flags = REG_FLAG_SOLID;
	const BlockId dyn = registryRegister(&def);
	CHECK(dyn >= REG_ID_DYN_LO);
	CHECK(inventoryCanHold((ItemId)dyn));
	CHECK(inventoryAdd(&all, (ItemId)dyn, 1, NULL) == INV_ADD_OK);
	CHECK(all.slots[0].item == (ItemId)dyn);

	// Put the table back, so nothing after this case inherits a row it never asked for.
	registryInitCore();
	CHECK(!inventoryCanHold((ItemId)dyn));

	// The WIRE span is a separate and much smaller ceiling, and it did NOT move. Pinned here
	// so the two are never conflated again: the bag holds a cactus, the wire cannot name one.
	CHECK(inventoryItemOnWire((ItemId)BLOCK_PLANKS));
	CHECK(!inventoryItemOnWire((ItemId)BLOCK_CACTUS));
}

static void testAddMergesIntoPartialStack(void)
{
	Inventory inv;
	inventoryInit(&inv);

	CHECK(inventoryAdd(&inv, BLOCK_STONE, 20, NULL) == INV_ADD_OK);
	CHECK(inventoryAdd(&inv, BLOCK_STONE, 15, NULL) == INV_ADD_OK);

	// Both additions land in the same slot — no second stack was opened for an item that
	// already had room in an existing one.
	CHECK(inv.slots[0].item  == BLOCK_STONE);
	CHECK(inv.slots[0].count == 35);
	CHECK(inv.slots[1].item  == ITEM_NONE);
}

static void testAddFillsStackExactly(void)
{
	Inventory inv;
	inventoryInit(&inv);

	CHECK(inventoryAdd(&inv, BLOCK_SAND, INV_STACK_MAX, NULL) == INV_ADD_OK);
	CHECK(inv.slots[0].item  == BLOCK_SAND);
	CHECK(inv.slots[0].count == INV_STACK_MAX);
	CHECK(inv.slots[1].item  == ITEM_NONE);   // exactly one stack, no spill
}

static void testAddPartialMergeThenSpillsToSecondSlot(void)
{
	Inventory inv;
	inventoryInit(&inv);

	// Slot 0 starts nearly full: 97 of INV_STACK_MAX's 99.
	CHECK(inventoryAdd(&inv, BLOCK_WOOD, 97, NULL) == INV_ADD_OK);

	uint8_t leftover = 255;
	const InvAddResult r = inventoryAdd(&inv, BLOCK_WOOD, 5, &leftover);

	// All 5 fit somewhere (2 top up slot 0, 3 open a new stack), so this is a full
	// success even though it spans two slots — the exact partial-merge-then-spill shape
	// the task calls out, not just "an overflow that opens a second slot".
	CHECK(r == INV_ADD_OK);
	CHECK(leftover == 0);
	CHECK(inv.slots[0].item  == BLOCK_WOOD);
	CHECK(inv.slots[0].count == INV_STACK_MAX);   // 97 + 2, topped up, not one over
	CHECK(inv.slots[1].item  == BLOCK_WOOD);
	CHECK(inv.slots[1].count == 3);               // the remainder that did not fit in slot 0
}

static void testAddOverflowsIntoSecondSlot(void)
{
	Inventory inv;
	inventoryInit(&inv);

	// More than one stack's worth in a single call, into an otherwise empty inventory.
	const uint8_t count = (uint8_t)(INV_STACK_MAX + 51);   // e.g. 150 for INV_STACK_MAX=99
	uint8_t leftover = 255;
	const InvAddResult r = inventoryAdd(&inv, BLOCK_LEAVES, count, &leftover);

	CHECK(r == INV_ADD_OK);
	CHECK(leftover == 0);
	CHECK(inv.slots[0].item  == BLOCK_LEAVES);
	CHECK(inv.slots[0].count == INV_STACK_MAX);
	CHECK(inv.slots[1].item  == BLOCK_LEAVES);
	CHECK(inv.slots[1].count == 51);
	CHECK(inv.slots[2].item  == ITEM_NONE);
}

static void testAddThatCannotFitIsRefusedAndInventoryUnchanged(void)
{
	Inventory inv;
	inventoryInit(&inv);

	// Fill every slot to the brim with one item, so no slot has spare room and none is
	// empty — the one state in which nothing at all can be added.
	for (int i = 0; i < INV_SLOT_COUNT; i++) {
		inv.slots[i].item  = BLOCK_STONE;
		inv.slots[i].count = INV_STACK_MAX;
	}
	inv.selected_hotbar = 2;

	const Inventory before = inv;

	uint8_t leftover = 0;
	const InvAddResult r = inventoryAdd(&inv, BLOCK_SAND, 1, &leftover);

	CHECK(r == INV_ADD_REFUSED);
	CHECK(leftover == 1);
	// The whole point of the test: not one byte moved. A weaker check (just "sand count
	// is still 0") would pass even if the implementation had scribbled on an unrelated
	// slot, so this compares the entire struct.
	CHECK(invEqual(&inv, &before));
}

// ── Inventory: removing ───────────────────────────────────────────────────────────────

static void testRemoveMoreThanPresent(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_DIRT, 5, NULL) == INV_ADD_OK);

	const uint8_t removed = inventoryRemove(&inv, BLOCK_DIRT, 10);

	CHECK(removed == 5);                       // only what was actually there
	CHECK(inv.slots[0].item  == ITEM_NONE);     // slot reverts to a clean empty slot
	CHECK(inv.slots[0].count == 0);
	CHECK(inventoryCount(&inv, BLOCK_DIRT) == 0);
}

static void testRemoveNeverTouchesOtherItems(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_GRASS, 8, NULL) == INV_ADD_OK);
	CHECK(inventoryAdd(&inv, BLOCK_STONE, 8, NULL) == INV_ADD_OK);

	CHECK(inventoryRemove(&inv, BLOCK_GRASS, 8) == 8);
	CHECK(inventoryCount(&inv, BLOCK_STONE) == 8);   // untouched by removing a different item
}

// ── Inventory: splitting and swapping ─────────────────────────────────────────────────

static void testSplitStack(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_WOOD, 7, NULL) == INV_ADD_OK);   // lands in slot 0

	CHECK(inventorySplitStack(&inv, 0, 1));

	CHECK(inv.slots[0].item  == BLOCK_WOOD);
	CHECK(inv.slots[0].count == 4);   // origin keeps the ceiling half
	CHECK(inv.slots[1].item  == BLOCK_WOOD);
	CHECK(inv.slots[1].count == 3);   // destination gets the floor half

	// A stack of exactly 1 cannot be split further — there is nothing to divide. Slots 0
	// and 1 are both occupied by now, so this lands in slot 2.
	CHECK(inventoryAdd(&inv, BLOCK_DIRT, 1, NULL) == INV_ADD_OK);
	CHECK(inv.slots[2].item == BLOCK_DIRT && inv.slots[2].count == 1);
	CHECK(!inventorySplitStack(&inv, 2, 3));
	CHECK(inv.slots[2].count == 1);   // unchanged by the refusal
	CHECK(inv.slots[3].item  == ITEM_NONE);

	// A destination that is not empty refuses too.
	CHECK(!inventorySplitStack(&inv, 0, 1));
}

static void testSwapTwoSlots(void)
{
	Inventory inv;
	inventoryInit(&inv);
	inv.slots[0] = (InvSlot){ .item = BLOCK_GRASS, .count = 5 };
	inv.slots[1] = (InvSlot){ .item = BLOCK_SAND,  .count = 3 };

	inventorySwapSlots(&inv, 0, 1);

	CHECK(inv.slots[0].item == BLOCK_SAND  && inv.slots[0].count == 3);
	CHECK(inv.slots[1].item == BLOCK_GRASS && inv.slots[1].count == 5);

	// Swapping an occupied slot with an empty one moves the stack wholesale rather than
	// merging or refusing.
	inventorySwapSlots(&inv, 0, 2);
	CHECK(inv.slots[2].item == BLOCK_SAND && inv.slots[2].count == 3);
	CHECK(inv.slots[0].item == ITEM_NONE  && inv.slots[0].count == 0);
}

static void testMoveUnitsRefusesMismatchedItem(void)
{
	Inventory inv;
	inventoryInit(&inv);
	inv.slots[0] = (InvSlot){ .item = BLOCK_DIRT,  .count = 10 };
	inv.slots[1] = (InvSlot){ .item = BLOCK_STONE, .count = 5 };

	const uint8_t moved = inventoryMoveUnits(&inv, 0, 1, 4);

	CHECK(moved == 0);                          // refused: slot 1 holds a different item
	CHECK(inv.slots[0].count == 10);             // both sides untouched
	CHECK(inv.slots[1].item  == BLOCK_STONE);
	CHECK(inv.slots[1].count == 5);
}

static void testHotbarSelectionAndHeldItem(void)
{
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_WOOD, 4, NULL) == INV_ADD_OK);   // slot 0, in the hotbar

	inventorySelectHotbar(&inv, 0);
	CHECK(inventoryHeldItem(&inv)  == BLOCK_WOOD);
	CHECK(inventoryHeldCount(&inv) == 4);

	// Out-of-range clamps to the last hotbar slot rather than being ignored.
	inventorySelectHotbar(&inv, 200);
	CHECK(inv.selected_hotbar == INV_HOTBAR_SLOTS - 1);
	CHECK(inventoryHeldItem(&inv) == ITEM_NONE);   // that slot is empty
}

// ── Crafting ───────────────────────────────────────────────────────────────────────────

static void testEveryRecipeCrafts(void)
{
	for (int i = 0; i < RECIPE_COUNT; i++) {
		const CraftRecipe* r = &CRAFT_RECIPES[i];

		Inventory inv;
		inventoryInit(&inv);
		CHECK(inventoryAdd(&inv, r->input_item, r->input_count, NULL) == INV_ADD_OK);

		CHECK(craftCanMake(&inv, i));
		CHECK(craftMake(&inv, i));

		CHECK(inventoryCount(&inv, r->input_item)  == 0);                // fully consumed
		CHECK(inventoryCount(&inv, r->output_item) == r->output_count);  // exactly produced
	}
}

static void testRecipeWithInsufficientInputsRefusesAndConsumesNothing(void)
{
	for (int i = 0; i < RECIPE_COUNT; i++) {
		const CraftRecipe* r = &CRAFT_RECIPES[i];
		if (r->input_count == 0) continue;   // not reachable today, but skip cleanly if it ever were

		Inventory inv;
		inventoryInit(&inv);
		CHECK(inventoryAdd(&inv, r->input_item, (uint8_t)(r->input_count - 1), NULL) == INV_ADD_OK);

		const Inventory before = inv;

		CHECK(!craftCanMake(&inv, i));
		CHECK(!craftMake(&inv, i));
		CHECK(invEqual(&inv, &before));   // not one unit of the short ingredient was touched
	}
}

// Exercises craftMake's other refusal path: enough ingredient, but nowhere for the output
// to go. Built by maxing every slot on an unrelated item except the one holding the
// recipe's own ingredient — and giving that slot *more* than the recipe needs, so
// consuming the ingredient does not itself free a slot for the output to land in. Without
// the scratch-copy-then-commit approach in crafting.c, this is exactly the shape of bug
// that would eat the player's last stone for nothing.
static void testCraftRefusesWhenOutputHasNoRoom(void)
{
	const CraftRecipe* r = &CRAFT_RECIPES[RECIPE_STONE_TO_SAND];   // 4 stone -> 1 sand

	Inventory inv;
	inventoryInit(&inv);
	for (int i = 0; i < INV_SLOT_COUNT - 1; i++) {
		inv.slots[i].item  = BLOCK_WOOD;
		inv.slots[i].count = INV_STACK_MAX;
	}
	inv.slots[INV_SLOT_COUNT - 1].item  = r->input_item;
	inv.slots[INV_SLOT_COUNT - 1].count = INV_STACK_MAX;   // far more than the 4 needed

	CHECK(craftCanMake(&inv, RECIPE_STONE_TO_SAND));   // the ingredient is there...

	const Inventory before = inv;
	CHECK(!craftMake(&inv, RECIPE_STONE_TO_SAND));      // ...but there is nowhere for the sand
	CHECK(invEqual(&inv, &before));                     // and nothing was consumed for it
}

// ── Save / load ────────────────────────────────────────────────────────────────────────

static void testSaveLoadRoundTrip(void)
{
	const char* dir = TEST_DIR "/roundtrip";
	testMkdir(dir);

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryAdd(&out, BLOCK_DIRT,  40, NULL) == INV_ADD_OK);
	CHECK(inventoryAdd(&out, BLOCK_STONE, 250, NULL) == INV_ADD_OK);   // spans two slots
	CHECK(inventoryAdd(&out, BLOCK_LEAVES, 3, NULL) == INV_ADD_OK);
	inventorySelectHotbar(&out, 5);

	CHECK(inventorySave(&out, dir));

	Inventory in;
	CHECK(inventoryLoad(&in, dir));
	CHECK(invEqual(&in, &out));

	// The tmp file the save wrote through must not still be there — a leftover .tmp next
	// to a good real file would be mistaken for an interrupted save on the next load.
	FILE* leftover = fopen(TEST_DIR "/roundtrip/inventory.dat.tmp", "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

static void testLoadMissingFileGivesEmptyInventory(void)
{
	const char* dir = TEST_DIR "/missing";
	testMkdir(dir);
	remove(TEST_DIR "/missing/inventory.dat");   // in case a previous failed run left one

	Inventory in;
	CHECK(inventoryLoad(&in, dir));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&in, &empty));
}

static void testLoadCorruptFileGivesEmptyInventory(void)
{
	const char* dir = TEST_DIR "/corrupt";
	testMkdir(dir);

	// A real, previously-good save, flipped in the middle of the payload — same shape a
	// torn write or a bit-rotted card sector would leave. The crc must catch this; a
	// magic/version check alone would not, since both are still intact.
	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryAdd(&out, BLOCK_SAND, 12, NULL) == INV_ADD_OK);
	CHECK(inventorySave(&out, dir));

	const char* path = TEST_DIR "/corrupt/inventory.dat";
	FILE* f = fopen(path, "rb+");
	CHECK(f != NULL);
	if (f) {
		CHECK(fseek(f, 20, SEEK_SET) == 0);   // first byte of slot 0's payload
		const uint8_t flipped = 0xFF;
		CHECK(fwrite(&flipped, 1, 1, f) == 1);
		fclose(f);
	}

	Inventory in;
	CHECK(inventoryLoad(&in, dir));   // must not crash and must still report success

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&in, &empty));     // corrupted payload falls back to empty, not garbage
}

static void testLoadTruncatedFileGivesEmptyInventory(void)
{
	const char* dir = TEST_DIR "/truncated";
	testMkdir(dir);

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryAdd(&out, BLOCK_WOOD, 9, NULL) == INV_ADD_OK);
	CHECK(inventorySave(&out, dir));

	// Truncate the saved file to just its header — the payload never arrived, which is
	// what a write cut off partway through would leave on disk.
	const char* path = TEST_DIR "/truncated/inventory.dat";
	FILE* f = fopen(path, "rb");
	CHECK(f != NULL);
	uint8_t header[16];
	size_t n = f ? fread(header, 1, sizeof(header), f) : 0;
	if (f) fclose(f);
	CHECK(n == sizeof(header));
	CHECK(writeRaw(path, header, n));

	Inventory in;
	CHECK(inventoryLoad(&in, dir));   // reaching this line at all is most of the test

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&in, &empty));
}

static void testCrashRecovery(void)
{
	const char* dir = TEST_DIR "/crash_recover";
	testMkdir(dir);
	const char* path = TEST_DIR "/crash_recover/inventory.dat";
	const char* tmp  = TEST_DIR "/crash_recover/inventory.dat.tmp";
	remove(path);
	remove(tmp);

	Inventory saved;
	inventoryInit(&saved);
	CHECK(inventoryAdd(&saved, BLOCK_GRASS, 6, NULL) == INV_ADD_OK);
	CHECK(inventorySave(&saved, dir));

	// Simulate the cut: the exact on-disk state a power loss between inventorySave's
	// remove() and rename() would leave — the bytes sitting under ".tmp", nothing at the
	// real path.
	FILE* src = fopen(path, "rb");
	CHECK(src != NULL);
	uint8_t buf[4096];
	size_t n = src ? fread(buf, 1, sizeof(buf), src) : 0;
	if (src) fclose(src);
	CHECK(writeRaw(tmp, buf, n));
	remove(path);

	Inventory recovered;
	CHECK(inventoryLoad(&recovered, dir));
	CHECK(inventoryCount(&recovered, BLOCK_GRASS) == 6);

	// Recovery must have promoted the tmp back to the real path, not merely read through
	// it once.
	FILE* real = fopen(path, "rb");
	CHECK(real != NULL);
	if (real) fclose(real);
	FILE* leftover = fopen(tmp, "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testAddToEmpty();
	testTheBagTakesEveryDefinedBlock();   // v1.8.8 — the item ceiling
	testAddMergesIntoPartialStack();
	testAddFillsStackExactly();
	testAddPartialMergeThenSpillsToSecondSlot();
	testAddOverflowsIntoSecondSlot();
	testAddThatCannotFitIsRefusedAndInventoryUnchanged();

	testRemoveMoreThanPresent();
	testRemoveNeverTouchesOtherItems();

	testSplitStack();
	testSwapTwoSlots();
	testMoveUnitsRefusesMismatchedItem();
	testHotbarSelectionAndHeldItem();

	testEveryRecipeCrafts();
	testRecipeWithInsufficientInputsRefusesAndConsumesNothing();
	testCraftRefusesWhenOutputHasNoRoom();

	testSaveLoadRoundTrip();
	testLoadMissingFileGivesEmptyInventory();
	testLoadCorruptFileGivesEmptyInventory();
	testLoadTruncatedFileGivesEmptyInventory();
	testCrashRecovery();

	testCleanup();

	checkCountPin();

	if (s_fails == 0)
		printf("inventory self-test: PASS  %d checks\n", s_checks);
	else
		printf("inventory self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int inventory_test_host_only_t;

#endif   // !__3DS__
