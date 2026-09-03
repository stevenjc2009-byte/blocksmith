// Host self-test for the torch acquisition chain end to end: coal ore in the bag, through
// the REAL crafting table (world/crafting.c), into four torches in the bag, through the REAL
// edit path (scene/interact.c's interactEdit, the same function a player's press drives) and
// into a REAL World (world/world.c) — then the placed cell is read back with worldGet() and
// must say BLOCK_TORCH.
//
// Why this file exists rather than folding the check into an existing one: world/
// inventory_test.c already proves every recipe in CRAFT_RECIPES round-trips through
// craftMake() (RECIPE_COAL_ORE_TO_TORCH included, since that suite loops to RECIPE_COUNT
// generically) and scene/interact_test.c already proves a torch placed by hand
// (`it.holding = BLOCK_TORCH`) lights the world through the real interactEdit(). Neither one
// proves the two halves are actually connected — that a torch produced by craftMake() is the
// same value that, handed to interactEdit() via inventoryHeldItem(), a real World accepts and
// stores at the target cell. A change that broke the seam between crafting and placement
// (e.g. craftMake() someday returning an ItemId that does not round-trip through
// inventoryHeldItem(), or a placement gate that started rejecting an item sourced from a
// craft rather than a mined drop) would leave both of those suites green and this one red.
//
// Links the REAL modules end to end, not a reimplementation of any of them: world/inventory.c
// and world/crafting.c for the craft, scene/interact.c and world/world.c for the placement.
// Two symbols the host cannot supply are stubbed here, both spies/no-ops exactly as
// scene/interact_test.c's own file comment explains for the same two symbols: this file is
// not asserting anything about the network, so both are honest no-ops rather than recording
// spies.
//
// The __3DS__ guard around the whole file is load-bearing, not tidy: the Makefile globs every
// .c under source/scene into the console build, so without it this file's main() collides
// with source/main.c's — see world/mining_test.c's own comment on the identical trap.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/input_map.h"
#include "net/networld.h"
#include "scene/interact.h"
#include "world/block.h"
#include "world/crafting.h"
#include "world/inventory.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			printf("  FAIL  L%d %s\n", __LINE__, #cond);                        \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// Both are real declarations from net/networld.h, satisfied here as plain no-ops: this file
// makes no claim about the network, so unlike interact_test.c's recording spy, nothing here
// ever reads what these were called with.
bool networldSendBlockEdit(int x, int y, int z, uint8_t block)
{
	(void)x; (void)y; (void)z; (void)block;
	return true;   // "the send worked" — same default interact_test.c's fixture uses
}

bool networldSessionActive(void)
{
	return false;   // "no server" — same default interact_test.c's fixture uses
}

#define TX 5
#define TY 40
#define TZ 6

static World s_world;

// Same fixture shape as scene/interact_test.c's freshAimedAt(): a fresh world with a solid
// block at (TX,TY,TZ) and an Interact aimed at the empty cell directly above it. Not copied
// blind — this file has exactly one placement case, so there is no need for the target_block
// parameter interact_test.c's twenty-odd cases share.
static void freshWorldAimedAtStone(Interact* it)
{
	registryInitCore();
	worldExit(&s_world);
	worldInit(&s_world);
	interactSetRelightQueue(NULL);

	worldSet(&s_world, TX, TY, TZ, BLOCK_STONE);

	interactInit(it);
	it->target.hit  = true;
	it->target.x    = TX;
	it->target.y    = TY;
	it->target.z    = TZ;
	it->target.face = FACE_TOP;
	it->target.px   = TX;
	it->target.py   = TY + 1;
	it->target.pz   = TZ;
}

// The player, far enough from the place cell that boxOverlapsCell can never be what refuses
// the placement — same distances interact_test.c's farAwayBody() uses.
static Body farAwayBody(void)
{
	Body b;
	memset(&b, 0, sizeof b);
	b.x = TX + 8.0f;
	b.y = 4.0f;
	b.z = TZ + 8.0f;
	return b;
}

static void testCoalOreCraftsIntoTorchesAndATorchPlaces(void)
{
	// ── Step 1: coal ore in the bag ─────────────────────────────────────────────────────
	Inventory inv;
	inventoryInit(&inv);
	CHECK(inventoryAdd(&inv, BLOCK_COAL_ORE, 1, NULL) == INV_ADD_OK);
	CHECK(inventoryCount(&inv, BLOCK_COAL_ORE) == 1);
	CHECK(inventoryCount(&inv, BLOCK_TORCH)    == 0);   // premise: no torch yet, from anywhere

	// ── Step 2: craft, through the real table ───────────────────────────────────────────
	CHECK(craftCanMake(&inv, RECIPE_COAL_ORE_TO_TORCH));
	CHECK(craftMake(&inv, RECIPE_COAL_ORE_TO_TORCH));

	// ── Step 3: four torches in the bag, the coal ore fully spent ───────────────────────
	CHECK(inventoryCount(&inv, BLOCK_COAL_ORE) == 0);
	CHECK(inventoryCount(&inv, BLOCK_TORCH)    == 4);

	// The item a player's press would actually place is whatever the selected hotbar slot
	// holds, read through the same accessor main.c uses — not the BLOCK_TORCH constant
	// directly, so a bug that put the torch in the wrong slot or left the wrong slot
	// selected would show up here rather than being papered over by hardcoding the id.
	const ItemId held = inventoryHeldItem(&inv);
	CHECK(held == BLOCK_TORCH);

	// ── Step 4: place it, through the real edit path ────────────────────────────────────
	Interact it;
	freshWorldAimedAtStone(&it);
	it.holding = held;

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);   // premise: place cell starts empty

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, inputKey(ACTION_PLACE), 0, 0);

	CHECK(it.placed == 1);
	CHECK(it.placed_id == BLOCK_TORCH);

	// ── Step 5: the world itself says so ────────────────────────────────────────────────
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_TORCH);
}

int main(void)
{
	testCoalOreCraftsIntoTorchesAndATorchPlaces();

	if (s_fails == 0)
		printf("craft_torch_e2e self-test: PASS  %d checks\n", s_checks);
	else
		printf("craft_torch_e2e self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int craft_torch_e2e_test_host_only_t;

#endif   // !__3DS__
