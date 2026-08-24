// Host self-test for scene/interact.c — the break/place edit path.
//
// It links the REAL scene/interact.c, not a copy of its decision: the defect this file was
// written for is an ORDERING bug (worldSet wrote air before anything asked whether the block
// could be carried), and an ordering bug is invisible to a test that reimplements the order
// it expects. That is what interact.h's __3DS__ split exists for — see the file comment
// there, and app/battery.c / app/debugmenu_ui.c for the same arrangement.
//
// The two symbols the host cannot supply are stubbed in the link, never in interact.c:
// tests/interact_stub.c gives chunkRenderTouch (a citro3d remesh queue), and this file gives
// networldSendBlockEdit — as a *spy*, because "did a block edit go out on the wire" is one of
// the things being asserted. Neither stands in for any logic under test.
//
// The CHECK macro and the PASS/FAIL summary line are copied from scene/title_nav_test.c, so a
// failure here reads the same way a failure anywhere else in this project does. The __3DS__
// guard around the whole file is load-bearing for the same reason it is there: the console
// Makefile globs every .c under source/scene, and without it this main() collides with
// source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/input_map.h"
#include "net/networld.h"
#include "scene/interact.h"
#include "world/block.h"
#include "world/inventory.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			/* Printed per failure, not just the first: this project's own test    \
			 * standard is that every case of a red run gets read, and a summary   \
			 * naming one of four hides the other three. */                        \
			printf("  FAIL  L%d %s\n", __LINE__, #cond);                        \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// ── the wire spy ────────────────────────────────────────────────────────────────────────
//
// net/networld.c is not in this binary's link (it needs the transport, which is not host
// portable — see tests/net_stub.c). This is the real declaration from net/networld.h with a
// recording body: it counts calls and remembers the last one, which is exactly what the
// "do not send an edit the server will drop" requirement has to be checked against. It makes
// no decision, so it cannot make a failing case pass.
static int     s_edits_sent;
static int     s_last_x, s_last_y, s_last_z;
static uint8_t s_last_block;

bool networldSendBlockEdit(int x, int y, int z, uint8_t block)
{
	s_edits_sent++;
	s_last_x = x;
	s_last_y = y;
	s_last_z = z;
	s_last_block = block;
	return true;
}

// ── fixtures ────────────────────────────────────────────────────────────────────────────

// Where every case below edits. Well away from y == 0 and from the column edges, so nothing
// here is testing a boundary by accident.
#define TX 5
#define TY 40
#define TZ 6

static World s_world;

// The dynamic (server-registered) block. Registered through registryRegister() — the same
// call world/registry.c's DEFS path ends in — rather than by poking a table, so what is
// exercised is the way a server-defined block really arrives. Returns 0 on refusal, which
// every caller must check: a missing block would make the break "survive" for the wrong
// reason (id 0 is air, and air is not breakable anyway).
static BlockId registerDynBlock(const char* name)
{
	BlockDef def;
	memset(&def, 0, sizeof def);
	snprintf(def.name, sizeof def.name, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = BTEX_STONE;
	def.flags = REG_FLAG_SOLID;
	return registryRegister(&def);
}

// A fresh registry, a fresh world and a fresh Interact aimed at (TX,TY,TZ), with the place
// cell one block above it. Nothing is aimed by raycast: interactAim needs a Camera and lives
// on the console side of the guard, and the ray is world/raycast.c's business and is tested
// there. What this file is about is what interactEdit does once a target exists.
static void freshAimedAt(Interact* it, BlockId target_block)
{
	registryInitCore();
	worldInit(&s_world);
	s_edits_sent = 0;
	s_last_block = 0xFF;

	if (target_block != BLOCK_AIR)
		worldSet(&s_world, TX, TY, TZ, target_block);

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

// The player, standing far enough away that boxOverlapsCell can never be what refuses a
// placement. Checked explicitly in the place cases below rather than assumed.
static Body farAwayBody(void)
{
	Body b;
	memset(&b, 0, sizeof b);
	b.x = TX + 8.0f;
	b.y = 4.0f;
	b.z = TZ + 8.0f;
	return b;
}

static u32 breakKey(void) { return inputKey(ACTION_BREAK); }
static u32 placeKey(void) { return inputKey(ACTION_PLACE); }

// ── the ceiling itself ──────────────────────────────────────────────────────────────────

// world/inventory.h's inventoryCanHold() is the single home of the rule; if it ever says
// something different from what the cases below assume, they would all still pass while
// meaning something else. So it is pinned first.
static void testTheCarryCeilingIsWhereItSays(void)
{
	CHECK(!inventoryCanHold(ITEM_NONE));
	CHECK(inventoryCanHold(BLOCK_STONE));
	CHECK(inventoryCanHold(BLOCK_PLANKS));            // the last core id
	CHECK(!inventoryCanHold((ItemId)BLOCK_COUNT));    // one past it
	CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_LO));  // the first server id
	CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_HI));
}

// ── the defect ──────────────────────────────────────────────────────────────────────────

// THE case this file exists for. Before the fix, interactEdit wrote BLOCK_AIR first and the
// carry check happened afterwards in main.c, so the block was deleted from the world and
// then refused by the bag: gone from both, with nothing said. Every check below is red
// against that code.
static void testABreakOnACarryUnholdableBlockChangesNothing(void)
{
	const BlockId dyn = registerDynBlock("test_dyn");
	CHECK(dyn >= REG_ID_DYN_LO);
	if (dyn < REG_ID_DYN_LO) return;

	Interact it;
	freshAimedAt(&it, dyn);
	CHECK(worldGet(&s_world, TX, TY, TZ) == dyn);   // the fixture really placed it

	// The return value (chunks newly queued for a remesh) is deliberately NOT asserted
	// anywhere in this file: tests/interact_stub.c's chunkRenderTouch always answers 0, so a
	// check on it could not go red and would prove nothing.
	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, breakKey());

	// The block is still there. This is the whole point: losing it silently is worse than
	// not being able to remove it.
	CHECK(worldGet(&s_world, TX, TY, TZ) == dyn);
	// Nothing to hand main.c, so nothing is offered to the bag and nothing can be dropped
	// on the floor between the two.
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.broke == 0);
	// The player gets the module's existing "that press did nothing" signal.
	CHECK(it.refused == 1);
	// Multiplayer: no BS_APP_BLOCK_EDIT goes out. The server would have honoured the
	// removal and then dropped the matching BS_INV_OP_PICKUP over the same ceiling, so
	// sending it would delete the block for everyone on the server.
	CHECK(s_edits_sent == 0);
}

// v1.7.1 task 47. THE case nobody had: aim at tall grass, press break, assert the cell is air.
//
// Two green suites straddled this without covering it. world_test.c asserted the plant is
// targetable; the ceiling test above asserted ids >= BLOCK_COUNT are refused. Both true, both
// passing, and the bug steve hit sat in the gap between them — BLOCK_TALL_GRASS is 9, past the
// ceiling, so the break was refused and the plant could not be mined at all. The refusal test
// directly above deliberately uses a freshly registered DYNAMIC id, so the one test exercising
// this guard never touched the one block a player actually meets.
static void testTallGrassBreaksAndDropsNothing(void)
{
	// The premise, pinned first: this really is an id the bag cannot hold. If tall grass ever
	// moves below the ceiling this goes red, and the rest stops proving what it claims.
	CHECK(!inventoryCanHold((ItemId)BLOCK_TALL_GRASS));
	CHECK(blockDropsNothing(BLOCK_TALL_GRASS));
	CHECK(!blockDropsNothing(BLOCK_STONE));          // control: an ordinary block still drops

	Interact it;
	freshAimedAt(&it, BLOCK_TALL_GRASS);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_TALL_GRASS);

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, breakKey());

	// It is GONE. This is the check that was red before the fix.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);

	// ...and it gave nothing to carry. broke_id stays BLOCK_AIR, which is how main.c already
	// spells "add nothing to the bag", so no unholdable id is ever offered to the inventory.
	CHECK(it.broke_id == BLOCK_AIR);

	// Multiplayer: the edit DOES go out this time, and it is an ordinary air write — id 0,
	// below every ceiling on both client and server, so nothing can refuse it half way.
	CHECK(s_edits_sent == 1);
	CHECK(s_last_block == BLOCK_AIR);
}

// An ordinary holdable block must be unaffected by the split above: it still breaks AND still
// hands its id to the bag. Without this, writing blockDropsNothing as `return true` would pass
// every check in the test above.
static void testAnOrdinaryBlockStillDropsItself(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, breakKey());

	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);
	CHECK(it.broke_id == BLOCK_STONE);   // the drop survived the change
	CHECK(s_edits_sent == 1);
}

// The refusal must not be a one-shot that quietly gives up: hold the button, press it again,
// and the block is still there. A guard that only worked the first time would still lose the
// block on the second press.
static void testRepeatedBreakAttemptsStillChangeNothing(void)
{
	const BlockId dyn = registerDynBlock("test_dyn2");
	CHECK(dyn >= REG_ID_DYN_LO);
	if (dyn < REG_ID_DYN_LO) return;

	Interact it;
	freshAimedAt(&it, dyn);
	const Body body = farAwayBody();

	for (int press = 0; press < 3; press++) {
		interactEdit(&it, &s_world, &body, breakKey());
		interactEdit(&it, &s_world, &body, 0);   // release, so the next call is a fresh edge
	}

	CHECK(worldGet(&s_world, TX, TY, TZ) == dyn);
	CHECK(it.broke == 0);
	CHECK(it.refused == 3);
	CHECK(s_edits_sent == 0);
}

// ── what must NOT have regressed ────────────────────────────────────────────────────────

static void testACoreBlockStillBreaks(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, breakKey());

	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke_id == BLOCK_STONE);   // this is what main.c puts in the bag
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);
	CHECK(s_edits_sent == 1);
	CHECK(s_last_x == TX && s_last_y == TY && s_last_z == TZ);
	CHECK(s_last_block == BLOCK_AIR);
}

// Every core id, not just stone: the guard is a predicate over ids, so a version of it that
// happened to admit stone and reject something else would pass the case above.
static void testEveryCoreBlockStillBreaks(void)
{
	for (BlockId id = BLOCK_GRASS; id < BLOCK_COUNT; id++) {
		Interact it;
		freshAimedAt(&it, id);
		const Body body = farAwayBody();
		interactEdit(&it, &s_world, &body, breakKey());

		CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
		CHECK(it.broke_id == id);
		CHECK(it.refused == 0);
		CHECK(s_edits_sent == 1);
	}
}

static void testACoreBlockStillPlaces(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey());

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);
	CHECK(it.placed_id == BLOCK_PLANKS);
	CHECK(it.placed == 1);
	CHECK(it.refused == 0);
	CHECK(s_edits_sent == 1);
	CHECK(s_last_block == BLOCK_PLANKS);
}

// Placing a server-registered block works end to end and must keep working — the guard is on
// the break, and only on the break. A blanket "dynamic ids are inert" would pass every case
// above this one and still break the feature.
static void testADynamicBlockStillPlaces(void)
{
	const BlockId dyn = registerDynBlock("test_dyn3");
	CHECK(dyn >= REG_ID_DYN_LO);
	if (dyn < REG_ID_DYN_LO) return;

	Interact it;
	// Registered, then the world is rebuilt: freshAimedAt calls registryInitCore(), so the
	// registration has to survive it. It does not — so register again afterwards, and assert
	// the id is the same one, or this case would be placing air.
	freshAimedAt(&it, BLOCK_STONE);
	const BlockId dyn2 = registerDynBlock("test_dyn3");
	CHECK(dyn2 == dyn);
	it.holding = dyn2;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey());

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == dyn2);
	CHECK(it.placed_id == dyn2);
	CHECK(it.placed == 1);
	CHECK(it.refused == 0);
	CHECK(s_edits_sent == 1);
	CHECK(s_last_block == dyn2);
}

// ── controls: refusals that were already refusals, and must read the same ───────────────
//
// These are green both before and against the fix. They are here so a red run can be told
// apart from a run where the whole binary broke: if a sabotage turns THESE red too, the
// sabotage hit something wider than the guard and the red is not evidence about it.

static void testAimingAtNothingIsStillARefusal(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.target.hit = false;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, breakKey());

	CHECK(it.refused == 1);
	CHECK(it.broke == 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
	CHECK(s_edits_sent == 0);
}

static void testPlacingWithAnEmptyHandIsStillARefusal(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_AIR;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey());

	CHECK(it.refused == 1);
	CHECK(it.placed == 0);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
	CHECK(s_edits_sent == 0);
}

static void testPlacingIntoTheOwnBodyIsStillARefusal(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;

	// Standing exactly in the place cell.
	Body body;
	memset(&body, 0, sizeof body);
	body.x = TX + 0.5f;
	body.y = (float)(TY + 1);
	body.z = TZ + 0.5f;

	interactEdit(&it, &s_world, &body, placeKey());

	CHECK(it.refused == 1);
	CHECK(it.placed == 0);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
	CHECK(s_edits_sent == 0);
}

int main(void)
{
	testTheCarryCeilingIsWhereItSays();
	testABreakOnACarryUnholdableBlockChangesNothing();
	testTallGrassBreaksAndDropsNothing();
	testAnOrdinaryBlockStillDropsItself();
	testRepeatedBreakAttemptsStillChangeNothing();
	testACoreBlockStillBreaks();
	testEveryCoreBlockStillBreaks();
	testACoreBlockStillPlaces();
	testADynamicBlockStillPlaces();
	testAimingAtNothingIsStillARefusal();
	testPlacingWithAnEmptyHandIsStillARefusal();
	testPlacingIntoTheOwnBodyIsStillARefusal();

	if (s_fails == 0)
		printf("interact self-test: PASS  %d checks\n", s_checks);
	else
		printf("interact self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int interact_test_host_only_t;

#endif   // !__3DS__
