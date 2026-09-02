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
#include <stdlib.h>
#include <string.h>

#include "app/input_map.h"
#include "net/networld.h"
#include "scene/interact.h"
#include "world/block.h"
#include "world/inventory.h"
#include "world/light.h"
// v1.8.8: the cactus case asserts the break landed on the tick breakTicksRequired() says it
// should, rather than on "some tick > 0". world/mining.c is already in this binary's link
// line — scene/interact.c calls it — so this is a header the suite was missing, not a new
// dependency.
#include "world/mining.h"
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

// v1.8.7. Until now this spy always answered true, which is why the whole class of bug the
// send-failure cases below cover was invisible to it: the real networldSendBlockEdit returns
// false whenever the packet did not leave the console, and nothing in this file could ever
// produce that answer. These two knobs are the two independent facts interact.c now has to
// combine, and they are set by the cases that care and left at the honest defaults (the send
// works, there is no server) by every other one.
//
// Knobs, not decisions: neither is read anywhere except in the two stub bodies here, so a
// case that forgets to set one gets the old behaviour rather than a silent pass.
static bool s_send_ok      = true;
static bool s_session_live = false;

bool networldSendBlockEdit(int x, int y, int z, uint8_t block)
{
	// Counted even when it fails. "How many times did interact.c TRY to tell the server" and
	// "how many landed" are different questions, and a spy that only counted successes could
	// not tell a send that was suppressed from one that was refused — which is exactly the
	// distinction the send-failure cases turn on.
	s_edits_sent++;
	s_last_x = x;
	s_last_y = y;
	s_last_z = z;
	s_last_block = block;
	return s_send_ok;
}

// The real one (net/networld.c) reads the transport's state enum. Here it reads a knob, for
// the same reason the send above does: net/networld.c is not in this binary's link.
bool networldSessionActive(void)
{
	return s_session_live;
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
	// v1.8.6: worldExit() BEFORE the reset, not just worldInit(). s_world is one static
	// instance reused by every case in this file, and worldInit() only memsets it — it does
	// not walk the previous incarnation's columns, so nothing released the Column, its
	// Chunk(s), or (now that lightEngineInit(true) runs below in main) its attached 32 KiB
	// LightColumn. Measured on the unpatched line: lightColumnsAttached() read 2 by the
	// second fixture in the file, climbing by one on every case that reaches a relight —
	// once nothing here ever turned lighting on, that leak was silent and free; it stops
	// being free the moment a relight is actually reachable, so it is fixed here rather than
	// carried forward. worldExit() is a no-op on the very first call: a static World starts
	// zeroed, so every slot is already NULL.
	worldExit(&s_world);
	worldInit(&s_world);
	s_edits_sent = 0;
	s_last_block = 0xFF;
	// v1.8.7. Back to "the send works, there is no server, relight inline" for every case, so
	// that the three cases which change any of them cannot leak into the twenty-odd that were
	// written before these knobs existed and assume the old behaviour.
	s_send_ok      = true;
	s_session_live = false;
	interactSetRelightQueue(NULL);

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

// ── driving the v1.8.1 held break ───────────────────────────────────────────────────────
//
// Since task 50 a break is a hold, not a press, so every case below has to drive a LOOP.
// These three helpers are the whole vocabulary: one frame of holding, one frame of not
// holding, and "hold until it goes".
//
// They deliberately pass the break bit in BOTH masks. In the real frame loop `keys_down` is
// a one-frame pulse and `keys_held` stays high, so fresh is set on the first call either
// way; passing it in both here keeps the press-edge accounting (which is what `refused`
// counts against) identical to the console's without the fixtures having to model
// hidKeysDown's pulse shape.

// One frame of holding break, worth `ticks` simulation ticks.
static void holdBreakFrame(Interact* it, const Body* body, int ticks)
{
	interactEdit(it, &s_world, body, breakKey(), breakKey(), ticks);
}

// One frame with nothing held. Ticks still pass — the world's clock does not stop because
// the player let go, and a state machine that only cancels on a zero-tick frame would pass
// a test that never gave it one.
static void releaseFrame(Interact* it, const Body* body, int ticks)
{
	interactEdit(it, &s_world, body, 0, 0, ticks);
}

// Hold break, one tick per frame, until the break lands. Returns the tick it landed on, or
// -1 if it never did within `max_ticks`.
//
// The cap is not decoration: without it, a state machine that banks no progress would hang
// the suite instead of failing it, and a hung suite in CI reads as an infrastructure
// problem rather than as the bug it is.
static int holdBreakUntilDone(Interact* it, const Body* body, int max_ticks)
{
	const int before = it->broke;

	for (int t = 1; t <= max_ticks; t++) {
		holdBreakFrame(it, body, 1);
		if (it->broke > before)
			return t;
	}
	return -1;
}

// Longer than any hardness in the registry (the hardest is stone at 45 ticks), so a case
// that says "this never breaks" has genuinely waited rather than given up early.
#define NEVER_TICKS 200

// ── the ceiling itself ──────────────────────────────────────────────────────────────────

// world/inventory.h's inventoryCanHold() is the single home of the rule; if it ever says
// something different from what the cases below assume, they would all still pass while
// meaning something else. So it is pinned first.
//
// v1.8.8 REWROTE this rule and this test with it. It used to read
//
//     CHECK(inventoryCanHold(BLOCK_PLANKS));            // the last core id
//     CHECK(!inventoryCanHold((ItemId)BLOCK_COUNT));    // one past it
//     CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_LO));  // the first server id
//     CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_HI));
//
// and every one of those four was a statement about the CONSTANT 8, not about the game. Note
// what the old comments called things: planks "the last core id" when it was the last core id
// BELOW THE CEILING — there were fifteen core rows by then and the bag admitted seven of them.
// That is the bug steve reported as "the cactus cannot be broken".
//
// Two of those four lines would still pass verbatim today, which is why they are gone rather
// than left in place: BLOCK_COUNT is 8 and id 8 is water, refused now for being a LIQUID, and
// REG_ID_DYN_HI is refused now for being UNDEFINED. A check that passes for a reason unrelated
// to what it claims is worse than no check.
static void testTheCarryCeilingIsWhereItSays(void)
{
	// Air is not an item, and that has never moved.
	CHECK(!inventoryCanHold(ITEM_NONE));
	CHECK(!inventoryCanHold((ItemId)BLOCK_AIR));      // ITEM_NONE spelled the other way

	// An ordinary block, below the old ceiling. The control: this was true before and must
	// stay true, so a broken predicate that refuses everything cannot pass this suite.
	CHECK(inventoryCanHold(BLOCK_STONE));
	CHECK(inventoryCanHold(BLOCK_PLANKS));

	// THE regression. Every one of these is an id the old `< BLOCK_COUNT` test refused, and
	// every one of them is a real block a player meets in a real world.
	CHECK(inventoryCanHold((ItemId)BLOCK_TALL_GRASS));   // 9
	CHECK(inventoryCanHold((ItemId)BLOCK_SNOW));         // 10
	CHECK(inventoryCanHold((ItemId)BLOCK_ICE));          // 11
	CHECK(inventoryCanHold((ItemId)BLOCK_CACTUS));       // 12 — the reported defect
	CHECK(inventoryCanHold((ItemId)BLOCK_DEAD_BUSH));    // 13
	CHECK(inventoryCanHold((ItemId)BLOCK_FERN));         // 14

	// The BOUNDARY, both sides of it, and it is now the edge of the DEFINED core table
	// rather than a compile-time constant: id 26 is the last defined core row and id 27 is
	// the first undefined one, now that v1.8.8's twelve per-biome rows (ids 15..26) have
	// landed. registryCount() is read here rather than hard-coded so this pair keeps
	// straddling the real edge if another biome rung ever lands on top of this one.
	const ItemId last_defined  = (ItemId)(registryCount() - 1);
	const ItemId first_beyond  = (ItemId)registryCount();
	CHECK(last_defined == (ItemId)BLOCK_APPLE);          // premise: the table is 27 rows
	CHECK(inventoryCanHold(last_defined));
	CHECK(!inventoryCanHold(first_beyond));

	// Liquids stay out, and this is the ONE core row the widened rule still refuses. Not
	// because of where it sits in the id space — it is id 8, right where the old ceiling was,
	// and that coincidence is exactly why the old `!inventoryCanHold(BLOCK_COUNT)` line had
	// to be deleted rather than kept.
	CHECK(!inventoryCanHold((ItemId)BLOCK_WATER));

	// The dynamic range. UNDEFINED ids are refused, defined ones are not — so the answer
	// tracks the registry rather than the address, which is the whole change. Registering
	// here is safe for the cases below: they use ids this never touches.
	CHECK(!inventoryCanHold((ItemId)REG_ID_DYN_HI));     // never registered by this suite
	const BlockId dyn = registerDynBlock("ceiling_probe");
	CHECK(dyn >= REG_ID_DYN_LO);
	CHECK(inventoryCanHold((ItemId)dyn));                // was FALSE before v1.8.8
	CHECK(!inventoryCanHold((ItemId)0xFF));              // reserved, never a row

	// The WIRE span is a SEPARATE and much smaller ceiling, and it did NOT move. This is the
	// gap the report has to name: the bag holds every defined row, but only ids below
	// BLOCK_COUNT can be named in a BS_INV_OP_PICKUP that an unpatched server will accept.
	CHECK(inventoryItemOnWire((ItemId)BLOCK_PLANKS));    // 7, the last id on the wire
	CHECK(!inventoryItemOnWire((ItemId)BLOCK_WATER));    // 8, one past it
	CHECK(!inventoryItemOnWire((ItemId)BLOCK_CACTUS));   // carryable, NOT sendable
	CHECK(!inventoryItemOnWire(ITEM_NONE));
}

// ── the defect ──────────────────────────────────────────────────────────────────────────

// An id the bag genuinely cannot hold, for the cases below that need one. It is an id in the
// server range that this suite NEVER registers, so registryIsDefined() is false for it and
// inventoryCanHold() refuses it.
//
// v1.8.8 had to invent this. Until now these cases used registerDynBlock(), because before
// the fix EVERY dynamic id was over the ceiling by definition — being at 0x80 was the whole
// reason the bag refused it. Now the bag refuses ids it has no ROW for, so a registered
// dynamic block is perfectly carryable and using one here would have quietly turned the two
// refusal cases below into tests that a block breaks. They went red, which is how this was
// found rather than reasoned.
//
// REG_ID_DYN_HI is the choice because registryRegister() hands ids out counting UP from
// REG_ID_DYN_LO, so the top of the range is the last thing this suite could ever collide
// with; the ceiling test above pins that it is undefined before anything else runs.
#define UNCARRYABLE_ID ((BlockId)REG_ID_DYN_HI)

// THE case this file exists for. Before the fix, interactEdit wrote BLOCK_AIR first and the
// carry check happened afterwards in main.c, so the block was deleted from the world and
// then refused by the bag: gone from both, with nothing said. Every check below is red
// against that code.
static void testABreakOnACarryUnholdableBlockChangesNothing(void)
{
	const BlockId dyn = UNCARRYABLE_ID;
	CHECK(!inventoryCanHold((ItemId)dyn));      // the premise, not an assumption
	CHECK(!blockDropsNothing(dyn));             // and it is not a plant taking the other path

	Interact it;
	freshAimedAt(&it, dyn);
	CHECK(worldGet(&s_world, TX, TY, TZ) == dyn);   // the fixture really placed it

	// The return value (chunks newly queued for a remesh) is deliberately NOT asserted
	// anywhere in this file: tests/interact_stub.c's chunkRenderTouch always answers 0, so a
	// check on it could not go red and would prove nothing.
	const Body body = farAwayBody();
	// Held for far longer than any block takes, so this is "it refuses to break", not "it
	// had not finished yet".
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == -1);
	// v1.8.1: and no crack was ever drawn on it either. Letting the animation run on a block
	// that was never going to go would read as the game being broken rather than as the
	// block being unbreakable.
	CHECK(interactBreakStage(&it) == -1);

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
	// The premise, pinned first, and v1.8.8 INVERTED it. This line used to read
	//
	//     CHECK(!inventoryCanHold((ItemId)BLOCK_TALL_GRASS));
	//
	// and the comment under it said "if tall grass ever moves below the ceiling this goes red".
	// It did go red, and correctly: tall grass is id 9 and the bag now holds it. What must not
	// change is the OTHER half — the plant still yields nothing when broken, and it yields
	// nothing because of its SHAPE, never because of its id. Keeping both lines here is what
	// makes that separation visible: carryable and drops-nothing are now plainly independent,
	// where the old pair could be read as one fact stated twice.
	CHECK(inventoryCanHold((ItemId)BLOCK_TALL_GRASS));   // was !, before v1.8.8
	CHECK(blockDropsNothing(BLOCK_TALL_GRASS));
	CHECK(!blockDropsNothing(BLOCK_STONE));          // control: an ordinary block still drops

	Interact it;
	freshAimedAt(&it, BLOCK_TALL_GRASS);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_TALL_GRASS);

	const Body body = farAwayBody();
	// One tick. Tall grass is the softest thing in the registry and comes out on the first
	// tick of the hold — a plant that took two seconds to pull up would be worse than the
	// v1.7.1 bug where it could not be pulled up at all.
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == 1);

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
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

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
	const BlockId dyn = UNCARRYABLE_ID;   // see the note on UNCARRYABLE_ID: v1.8.8 changed this
	CHECK(!inventoryCanHold((ItemId)dyn));

	Interact it;
	freshAimedAt(&it, dyn);
	const Body body = farAwayBody();

	for (int press = 0; press < 3; press++) {
		holdBreakFrame(&it, &body, 1);
		releaseFrame(&it, &body, 1);   // release, so the next call is a fresh edge
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
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

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
//
// v1.8.8 WIDENED the loop, and the old bound is the bug. It read
//
//     for (BlockId id = BLOCK_GRASS; id < BLOCK_COUNT; id++)
//
// which walks ids 1..7 and stops — seven of the fifteen rows the game actually ships. Snow,
// ice, cactus, dead bush and fern were all outside it, so "every core block still breaks" was
// a green check that had never once looked at the block steve reported as unbreakable. That is
// the whole defect in one line: a bound written as a constant walks the constant, not the game.
//
// It now walks the whole CORE id space and skips what has no row, so the row a future biome
// adds is covered the moment it is registered and nobody has to remember to widen anything.
// The bound is REG_ID_CORE_HI rather than registryCount() on purpose: freshAimedAt() calls
// registryInitCore() on every iteration, and a count taken across that would be a bound that
// moves underneath its own loop.
static void testEveryCoreBlockStillBreaks(void)
{
	// Premise: the loop really is looking at more than the old seven. Without this a table
	// that had somehow shrunk back to eight rows would leave the loop exactly as narrow as it
	// was while reading as if it had been fixed.
	CHECK(registryCount() > BLOCK_COUNT);

	int targetable_seen = 0;
	int cross_seen      = 0;

	for (BlockId id = BLOCK_GRASS; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined(id)) continue;
		// Water is the one core row a crosshair cannot land on, so it is skipped rather than
		// asserted about here — testTheCarryCeilingIsWhereItSays owns the liquid rule, and a
		// break case that "passed" on a block no ray can hit would prove nothing.
		if (!blockIsTargetable(id)) continue;
		targetable_seen++;

		Interact it;
		freshAimedAt(&it, id);
		const Body body = farAwayBody();
		CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

		CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
		CHECK(it.refused == 0);
		CHECK(s_edits_sent == 1);

		// What reaches the bag splits by SHAPE, not by id: a cross-quad plant is removed and
		// yields nothing (breakComplete hands main.c BLOCK_AIR), everything else drops itself.
		if (blockDropsNothing(id)) {
			cross_seen++;
			CHECK(it.broke_id == BLOCK_AIR);
		} else {
			CHECK(it.broke_id == id);
		}
	}

	// The loop ran, and ran over both kinds. A `continue` that swallowed everything would
	// otherwise leave this function green having made no assertion at all — the vault's
	// "a check that could not fail proves nothing", written as two counters.
	CHECK(targetable_seen == 25);   // 26 rows past air, less water
	CHECK(cross_seen == 8);         // tall grass, dead bush, fern, tall grass top,
	                                 // poppy, daisy, bluebell, orchid
}

// ── v1.8.8: the reported defect, end to end ─────────────────────────────────────────────

// steve's report, in his words: the cactus cannot be broken. This is that exact sequence —
// aim at a cactus, hold break, and assert all four halves of what "breakable" means.
//
// It is a separate case from the loop above on purpose. The loop proves the RULE; this proves
// the INSTANCE that was reported, so the report has an answer that does not depend on reading
// a bound. Against the pre-v1.8.8 client every check below is red at the first one.
static void testTheCactusBreaksAndDropsItself(void)
{
	// 1. The bag will take it. This is the line that was false, and the only thing that was
	//    ever wrong with the cactus.
	CHECK(inventoryCanHold((ItemId)BLOCK_CACTUS));
	// 2. It is a full cube, so it drops ITSELF rather than taking the plant path.
	CHECK(!blockDropsNothing(BLOCK_CACTUS));
	CHECK(blockIsTargetable(BLOCK_CACTUS));

	Interact it;
	freshAimedAt(&it, BLOCK_CACTUS);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CACTUS);

	const Body body = farAwayBody();
	const int landed = holdBreakUntilDone(&it, &body, NEVER_TICKS);

	// 3. It breaks, and it breaks on ITS OWN clock. 9 ticks is the cactus row's hardness and
	//    nothing else's — snow is 8 and ice is 10, so a break time borrowed from a neighbour
	//    lands on the wrong tick and this goes red. An `== 9` rather than a `> 0` is the
	//    difference between "it broke" and "it broke with its own durability".
	CHECK(landed == 9);
	CHECK(landed == (int)breakTicksRequired(BLOCK_CACTUS, ITEM_NONE));
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);

	// 4. And the cactus itself is what main.c is handed for the bag — not air, not the id of
	//    something else.
	CHECK(it.broke_id == BLOCK_CACTUS);
	CHECK(inventoryCanHold((ItemId)it.broke_id));

	CHECK(s_edits_sent == 1);
	CHECK(s_last_block == BLOCK_AIR);
}

// The guard in scene/interact.c that refuses a break on an id the bag cannot hold. Named in
// that file's comment, because v1.8.8 makes it far easier to delete than to keep.
//
// The honest position, and the reason this test says what it says: through the PRODUCTION
// raycast the branch can no longer fire. world/raycast.c stops on blockIsTargetable(), which
// is now "drawn && !liquid", and inventoryCanHold() is "defined && !air && !liquid" — the same
// set. So no ray can hand breakProgress() an id the bag refuses.
//
// It is kept, and tested, because "no ray can produce it" is not "it cannot happen" and
// certainly not "it will never happen again". A server whose registry sync failed leaves ids
// in the world this client has no row for, and the first ItemId that is not a BlockId — a
// tool, the apple v1.8.8 is being built for — separates the two predicates again on purpose.
// This case reaches the branch the only way anything can: by handing interactEdit a hit on an
// undefined id directly, which is exactly the state a failed registry sync leaves behind.
static void testTheBreakGuardStillRefusesTheUncarryable(void)
{
	// The two predicates agree across every id a raycast can produce. This is the fact that
	// makes the branch unreachable in play, stated as a check so it cannot rot silently: the
	// day it stops being true, this goes red and the branch has a live case again.
	registryInitCore();
	int agreed = 0;
	for (BlockId id = 0; id <= REG_ID_CORE_HI; id++) {
		CHECK(blockIsTargetable(id) == inventoryCanHold((ItemId)id));
		agreed++;
	}
	CHECK(agreed == REG_ID_CORE_HI + 1);   // the loop ran over the whole core id space

	// And the branch still does its job when reached. Same shape as the v1.6.0 defect case:
	// the block stays, nothing is offered to the bag, nothing goes on the wire.
	Interact it;
	freshAimedAt(&it, UNCARRYABLE_ID);
	const Body body = farAwayBody();

	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == -1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == UNCARRYABLE_ID);
	CHECK(it.broke == 0);
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.refused == 1);
	CHECK(s_edits_sent == 0);
}

static void testACoreBlockStillPlaces(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

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
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

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
	holdBreakFrame(&it, &body, 1);

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
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

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

	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

	CHECK(it.refused == 1);
	CHECK(it.placed == 0);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
	CHECK(s_edits_sent == 0);
}

// ── v1.8.1 task 50: a break takes time ──────────────────────────────────────────────────
//
// The hardness values themselves belong to world/registry.c and are pinned there by
// world/mining_test.c. These three are the ones the cases below quote, restated in one
// place so that if the table is ever retuned (task 32 will, when tools arrive) exactly
// three lines move and every case stays honest about what it is claiming.
#define TICKS_STONE   45
#define TICKS_DIRT    12
#define TICKS_LEAVES   4

// THE case task 50 exists for. Every check here is red against the pre-v1.8.1 code, where
// one frame of the button being down took the block away.
static void testABreakDoesNotHappenOnThePressEdge(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	holdBreakFrame(&it, &body, 1);

	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
	CHECK(it.broke == 0);
	// Not a refusal either. A press that started a break did something; counting it as a
	// refusal would make the overlay's `r` figure meaningless the moment anyone mined.
	CHECK(it.refused == 0);
	CHECK(s_edits_sent == 0);
	// It IS in progress, which is what separates "takes time" from "does nothing".
	CHECK(it.breaking);
	CHECK(interactBreakStage(&it) == 0);
}

// Each block takes its OWN time, and takes all of it. Both halves matter: without the
// one-tick-short half, a machine that broke everything on the first tick would pass the
// completion half by accident; without the completion half, a machine that never finished
// would pass the first.
static void testEachBlockTakesItsOwnHardness(void)
{
	const struct { BlockId id; int ticks; } cases[] = {
		{ BLOCK_STONE,  TICKS_STONE  },
		{ BLOCK_DIRT,   TICKS_DIRT   },
		{ BLOCK_LEAVES, TICKS_LEAVES },
	};

	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		Interact it;
		freshAimedAt(&it, cases[i].id);
		const Body body = farAwayBody();

		for (int t = 0; t < cases[i].ticks - 1; t++)
			holdBreakFrame(&it, &body, 1);
		CHECK(worldGet(&s_world, TX, TY, TZ) == cases[i].id);
		CHECK(it.broke == 0);

		holdBreakFrame(&it, &body, 1);
		CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
		CHECK(it.broke == 1);
		CHECK(it.broke_id == cases[i].id);
	}
}

// Letting go throws the progress away. Anything else needs a decay rule, and a decay rule
// is a mechanic nobody asked for.
static void testReleasingTheButtonThrowsAwayProgress(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	for (int t = 0; t < TICKS_STONE - 1; t++)
		holdBreakFrame(&it, &body, 1);
	CHECK(it.breaking);
	CHECK(it.broke == 0);

	releaseFrame(&it, &body, 1);
	CHECK(!it.breaking);
	CHECK(interactBreakStage(&it) == -1);

	// One tick of holding again is the FIRST tick, not the forty-fourth.
	holdBreakFrame(&it, &body, 1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
	CHECK(interactBreakStage(&it) == 0);

	// ...and a whole requirement still has to be paid, less the one tick just banked.
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == TICKS_STONE - 1);
}

// Progress belongs to a block, not to the button. Moving the crosshair mid-hold starts
// again on the new block and abandons the old one.
static void testLookingAtAnotherBlockThrowsAwayProgress(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	worldSet(&s_world, TX + 1, TY, TZ, BLOCK_STONE);   // a second block to look at
	const Body body = farAwayBody();

	for (int t = 0; t < TICKS_STONE - 1; t++)
		holdBreakFrame(&it, &body, 1);

	// The crosshair moves one cell, without the button ever coming up.
	it.target.x = TX + 1;
	holdBreakFrame(&it, &body, 1);

	CHECK(worldGet(&s_world, TX,     TY, TZ) == BLOCK_STONE);
	CHECK(worldGet(&s_world, TX + 1, TY, TZ) == BLOCK_STONE);
	CHECK(it.broke == 0);
	CHECK(it.break_x == TX + 1);
	CHECK(it.break_ticks == 1u);

	// Look back. The original is at zero too: progress does not sit waiting for the
	// crosshair to come home.
	it.target.x = TX;
	holdBreakFrame(&it, &body, 1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
	CHECK(it.break_ticks == 1u);
}

// A remote edit swapping the block under the crosshair must restart the timer. Without the
// id being part of the progress identity, a 44-tick stone break would finish on the very
// next tick against the dirt that replaced it.
static void testTheBlockChangingUnderTheCrosshairRestarts(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	for (int t = 0; t < TICKS_STONE - 1; t++)
		holdBreakFrame(&it, &body, 1);
	CHECK(it.break_ticks == (uint32_t)(TICKS_STONE - 1));

	worldSet(&s_world, TX, TY, TZ, BLOCK_DIRT);

	holdBreakFrame(&it, &body, 1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_DIRT);
	CHECK(it.broke == 0);
	CHECK(it.break_id == BLOCK_DIRT);
	CHECK(it.break_ticks == 1u);

	// And it now takes DIRT's time from that restart, not stone's.
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == TICKS_DIRT - 1);
	CHECK(it.broke_id == BLOCK_DIRT);
}

// The overlay's input. Stage must start at nothing to draw, begin at 0, never go backwards,
// never leave the addressable range, reach the last picture before the block goes, and go
// back to nothing to draw the instant it does.
static void testTheCrackStageTracksProgress(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	CHECK(interactBreakStage(&it) == -1);

	int prev     = 0;
	int in_range = 1;
	int monotone = 1;
	int saw_last = 0;

	for (int t = 1; t < TICKS_STONE; t++) {
		holdBreakFrame(&it, &body, 1);
		const int stage = interactBreakStage(&it);

		// Accumulated into flags rather than CHECKed inside the loop: 44 iterations of four
		// checks would put 176 near-identical lines into the count and drown the summary.
		if (stage < 0 || stage >= INTERACT_BREAK_STAGES) in_range = 0;
		if (stage < prev)                                monotone = 0;
		if (stage == INTERACT_BREAK_STAGES - 1)          saw_last = 1;
		prev = stage;
	}

	CHECK(in_range == 1);
	CHECK(monotone == 1);   // cracks never heal
	CHECK(saw_last == 1);   // the animation does not jump from half-cracked to gone
	CHECK(it.broke == 0);   // ...and none of that finished the break early

	holdBreakFrame(&it, &body, 1);
	CHECK(it.broke == 1);
	CHECK(interactBreakStage(&it) == -1);
}

// tickClockAdvance hands out up to TICK_MAX_CATCHUP_DEFAULT ticks in one frame after a
// chunk-load stall, so the requirement can be stepped straight over rather than landed on.
static void testCatchUpTicksStillLandTheBreak(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	int in_range = 1;
	int frames   = 0;

	for (; frames < 20 && it.broke == 0; frames++) {
		holdBreakFrame(&it, &body, 4);
		if (it.broke)
			break;
		const int stage = interactBreakStage(&it);
		if (stage < 0 || stage >= INTERACT_BREAK_STAGES) in_range = 0;
	}

	CHECK(in_range == 1);
	CHECK(it.broke == 1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	// 4 ticks a frame against 45: the twelfth frame banks 48 and is the one that lands it.
	// Asserted exactly, because ">= sooner or later" would also pass a machine that
	// overshot by a hundred frames.
	CHECK(frames == 11);
}

// At 60 fps the 20 TPS clock produces no tick on two frames out of three. Those frames must
// not cancel the hold, and a clock that appears to run backwards must not rewind it.
static void testZeroAndNegativeTicksKeepTheHold(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	const Body body = farAwayBody();

	for (int f = 0; f < 10; f++)
		holdBreakFrame(&it, &body, 0);
	CHECK(it.breaking);
	CHECK(it.break_ticks == 0u);
	CHECK(interactBreakStage(&it) == 0);

	holdBreakFrame(&it, &body, 5);
	CHECK(it.break_ticks == 5u);
	holdBreakFrame(&it, &body, -100);
	CHECK(it.break_ticks == 5u);

	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == TICKS_STONE - 5);
}

// control: place was deliberately left edge-triggered and must be completely indifferent to
// the clock. Green in every arm of every sabotage of the break machine — if this goes red,
// the sabotage hit something wider than task 50 and the red is not evidence about it.
static void testPlaceIsStillOnePressAndIgnoresTheClock(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;
	const Body body = farAwayBody();

	interactEdit(&it, &s_world, &body, placeKey(), placeKey(), 0);

	CHECK(it.placed == 1);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);

	// Holding it down for a hundred more frames puts down exactly nothing more.
	for (int f = 0; f < 100; f++)
		interactEdit(&it, &s_world, &body, placeKey(), placeKey(), 1);
	CHECK(it.placed == 1);
}

// ── v1.8.6: the light engine is actually on in here now ────────────────────────────────
//
// docs/ROADMAP.md: this file never called lightEngineInit(true), so lightEnabled() was false
// for the whole binary and the two relight calls in scene/interact.c — L114 in the break
// path, L308 in the place path, both guarded by `if (lightEnabled())` — had never once run
// here. Every case above this point still passes with that guard closed; none of them assert
// anything about light, so none of them could have told the difference. Confirmed by
// instrumentation, not by reading: a print dropped inside both guarded bodies, run against
// this file unmodified, never fired once across all 169 checks that existed before this
// version. main() now calls lightEngineInit(true) once, which is also what the shipping
// binary does — both console models turn the gate on unconditionally at boot
// (scene/chunk_render.c) and leave it on for the session, so an always-on gate here matches
// production rather than testing something narrower than it.
//
// The two cases below are what actually exercises the two guarded calls, rather than letting
// them ride along unasserted inside cases written for something else. Both take a real
// baseline reading with lightPropagateColumn (the same engine world_test.c's own light suite
// uses) BEFORE the edit, then read the identical cells again after the edit runs through the
// real interactEdit() path. Nothing else in this file ever writes to a light array, so if
// breakComplete's or the place path's relight call were skipped, the baseline reading would
// still be sitting there unchanged — a check that only asked "did anything crash" could not
// tell a skipped relight from a working one, and now these can.

// TY = 40 is a single stone cell in an otherwise empty column (see freshAimedAt), not a
// sealed floor like world_test.c's lightFloor() — so straight down through the seven other
// open columns of the same layer is unobstructed and leaks in sideways at -1 per step. That
// is why the cell directly under the slab reads 14 rather than 0: one step of falloff from
// its lit neighbours, not the "sealed underneath" shape a full floor would give. All three
// readings below are measured off a real build of this exact fixture, not assumed from the
// falloff rule in the abstract.
static void testABreakRelightsTheColumnItLeftBehind(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) return;
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, TX >> 4, TZ >> 4, q));
	free(q);   // lightRelightColumn owns its own queue below; this one's job is done

	Column* col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	// TX and TZ double as the column-local coordinates here: TX>>4 == TZ>>4 == 0 (see
	// world/world.h's coordinate note), so the world coordinate and the local one are the
	// same number.
	CHECK(lightGetSky(col, TX, TY,     TZ) == 0);    // the slab itself stops light
	CHECK(lightGetSky(col, TX, TY - 1, TZ) == 14);   // one step of sideways leak, not sealed
	CHECK(lightGetSky(col, TX, TY + 1, TZ) == 15);   // open sky above the slab
	CHECK(lightColumnsAttached() == 1);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);

	// Same column, same three cells, read again with nothing else able to have touched
	// them in between. The whole shaft is open air now, and sky light travels straight
	// down through open air at full strength — no per-cell falloff going straight down,
	// only when it spreads sideways (see world/light.h) — so all three read full sun.
	col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, TX, TY,     TZ) == 15);
	CHECK(lightGetSky(col, TX, TY - 1, TZ) == 15);
	CHECK(lightGetSky(col, TX, TY + 1, TZ) == 15);
	CHECK(lightColumnsAttached() == 1);   // still one column — the fix above, not a leak
}

// The place-path twin. The probe cell is the place cell itself: open sky before anything is
// put there, and the placed block's own cell the instant it lands — no falloff arithmetic to
// get right, just "did the write actually get seen by the light arrays or not".
static void testAPlaceRelightsTheColumnItLeftBehind(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) return;
	lightQueueInit(q);

	CHECK(lightPropagateColumn(&s_world, TX >> 4, TZ >> 4, q));
	free(q);

	Column* col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, TX, TY + 1, TZ) == 15);   // the place cell: open sky, nothing there
	CHECK(lightColumnsAttached() == 1);

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(it.placed == 1);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);

	// The cell the plank now occupies is opaque, and an opaque cell stores no light — if
	// the place path's relight call were skipped, this would still read the 15 taken above,
	// which nothing else in this file would ever have overwritten.
	col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, TX, TY + 1, TZ) == 0);
	CHECK(lightColumnsAttached() == 1);
}

// ── v1.8.7 F1: a block edit the server never got must not move the bag ──────────────────
//
// THE DEFECT. Both edit paths called networldSendBlockEdit() and threw the bool away. The
// local worldSet() had already run, the column was already marked dirty (so the edit SAVES),
// and main.c then read broke_id/placed_id and sent BS_INV_OP_PICKUP / BS_INV_OP_CONSUME —
// which deps/blocksmith-server/game/bsgame.c's handle_inv_action takes on trust, under a
// comment that says so. So on a refused send the server's INVENTORY moved and its WORLD did
// not, permanently and silently: the periodic BS_APP_INV_STATE snapshot puts the bag back and
// has nothing to say about the block.
//
// The send really can fail on its own, per packet, which is what makes this reachable rather
// than theoretical: net/bsnet_transport.c's send_app_packet returns false on a closed socket,
// a refused AEAD encrypt, or a short/failed sendto, none of which are session-wide.
//
// WHY A SESSION CHECK AND NOT JUST "the send failed". networldSendBlockEdit is false in
// single player too — there is no transport, so nothing can be sent, and nothing is out of
// step either. A fix that reverted on every false would delete every edit in single player.
// The third case below is the one that fails if anyone ever writes that fix.
static void testAFailedSendInASessionUndoesTheBreak(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	s_session_live = true;    // there IS a server
	s_send_ok      = false;   // and this packet did not reach it

	// ONE break's worth of ticks in one frame, not holdBreakUntilDone: a refused break cancels
	// the hold (breakCancel) without setting `broke`, so that helper would keep re-starting
	// the hold for its whole 200-tick budget and land four or five attempts. That re-try is
	// correct behaviour — it is the module's existing "a veto restarts the timer" rule, which
	// bounds the noise to one refusal per full break time — but it makes "how many times did
	// this try to send" unreadable, and that count is the point of this case.
	const Body body = farAwayBody();
	holdBreakFrame(&it, &body, TICKS_STONE);

	// It was TRIED, exactly once. A fix that suppressed the send instead of reacting to its
	// failure would be a different (and wrong) thing, and this is what tells them apart.
	CHECK(s_edits_sent == 1);

	// The world is where it was. This is the divergence closing: the server still has the
	// stone, and so do we.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);

	// And nothing is offered to the bag, so main.c sends no BS_INV_OP_PICKUP. broke_id is
	// the only channel it reads (see main.c's `if (it.broke_id != BLOCK_AIR)`), so this one
	// check is the whole of "the inventory action was suppressed".
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.broke == 0);
	CHECK(it.refused == 1);

	// And it stays refused rather than slipping through on a later attempt: holding on for a
	// further full break time re-tries and is turned away again, with the block still there.
	holdBreakFrame(&it, &body, TICKS_STONE);
	CHECK(s_edits_sent == 2);
	CHECK(it.refused == 2);
	CHECK(it.broke == 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
}

static void testAFailedSendInASessionUndoesThePlace(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding     = BLOCK_PLANKS;
	s_session_live = true;
	s_send_ok      = false;

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

	CHECK(s_edits_sent == 1);
	// The place cell is empty again — the plank went back into the hand, not into a world
	// the server does not know about.
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
	// So main.c charges nothing: no BS_INV_OP_CONSUME, and the block stays in the hotbar.
	CHECK(it.placed_id == BLOCK_AIR);
	CHECK(it.placed == 0);
	CHECK(it.refused == 1);
	// The block that WAS under the crosshair is untouched — the revert put back the place
	// cell and nothing else.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
}

// The guard on the fix above. In single player every send fails and every break must still
// work; if the revert is ever written without the session test, this is the case that goes
// red instead of a player losing the game.
static void testAFailedSendWithNoSessionStillBreaksAndPlaces(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	s_session_live = false;   // single player
	s_send_ok      = false;   // as it always is with no transport

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke_id == BLOCK_STONE);   // the drop still reaches the bag
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);

	// And the place path likewise.
	freshAimedAt(&it, BLOCK_STONE);
	it.holding     = BLOCK_PLANKS;
	s_session_live = false;
	s_send_ok      = false;
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);
	CHECK(it.placed_id == BLOCK_PLANKS);
	CHECK(it.placed == 1);
	CHECK(it.refused == 0);
}

// ── v1.8.7 F2: the local relight is queued, not run on the frame that caused it ─────────
//
// Every cell of one column, both channels: 32768 cells x 2 = 65536 bytes.
#define SNAP_CELLS  (CHUNK_DIM * CHUNK_DIM * WORLD_HEIGHT)
#define SNAP_BYTES  (SNAP_CELLS * 2)

// Deliberately the WHOLE column and not a sample, and not a checksum. The claim under test is
// "queueing the relight changes nothing about the result", and a sampled compare would pass
// for a relight that got every cell anyone thought to name and none of the others — which is
// precisely the shape of bug a threading change in world/light.c could introduce underneath
// this. A checksum would catch that but could not say which cell moved, and that is the first
// thing anyone asks of a red light test.
static bool lightSnapshot(World* w, int cx, int cz, uint8_t* out)
{
	Column* col = worldColumn(w, cx, cz);
	if (col == NULL) return false;

	size_t i = 0;
	for (int y = 0; y < WORLD_HEIGHT; y++)
		for (int lz = 0; lz < CHUNK_DIM; lz++)
			for (int lx = 0; lx < CHUNK_DIM; lx++) {
				out[i++] = lightGetSky(col, lx, y, lz);
				out[i++] = lightGetBlock(col, lx, y, lz);
			}
	return true;
}

// A real baseline with the target block still in place, the same way the v1.8.6 cases above
// take theirs — so the "before the drain" reading below is a genuine lit column and not the
// zeroed one a fresh World would hand back.
static bool propagateBaseline(void)
{
	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	if (q == NULL) return false;
	lightQueueInit(q);
	const bool ok = lightPropagateColumn(&s_world, TX >> 4, TZ >> 4, q);
	free(q);
	return ok;
}

// Drains the queue the way source/main.c's frame does — relightDrain() at main.c:4656, which
// is AHEAD of chunkRenderDrainDirty() at main.c:4666 and behind interactEdit() at main.c:4545.
// That ordering is the entire reason deferring the relight is safe, so the test walks it in
// the same order rather than in a convenient one.
static int drainRelights(RelightQueue* q)
{
	int cx = 0, cz = 0, n = 0;
	while (relightqPop(q, &cx, &cz)) {
		lightRelightColumn(&s_world, cx, cz);
		n++;
	}
	return n;
}

static void testQueuedAndInlineRelightsAgreeByteForByte(void)
{
	uint8_t* inline_snap = (uint8_t*)malloc(SNAP_BYTES);
	uint8_t* queued_pre  = (uint8_t*)malloc(SNAP_BYTES);
	uint8_t* queued_post = (uint8_t*)malloc(SNAP_BYTES);
	CHECK(inline_snap != NULL && queued_pre != NULL && queued_post != NULL);
	if (inline_snap == NULL || queued_pre == NULL || queued_post == NULL) {
		free(inline_snap); free(queued_pre); free(queued_post);
		return;
	}

	const Body body = farAwayBody();
	Interact it;

	// ARM A — inline, exactly as v1.8.6 shipped. freshAimedAt leaves the queue NULL.
	freshAimedAt(&it, BLOCK_STONE);
	CHECK(propagateBaseline());
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(lightSnapshot(&s_world, TX >> 4, TZ >> 4, inline_snap));

	// ARM B — same fixture, same block, same break, with a queue attached.
	RelightQueue q;
	relightqInit(&q);
	freshAimedAt(&it, BLOCK_STONE);
	interactSetRelightQueue(&q);
	CHECK(propagateBaseline());
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	// The WORLD write is not deferred and must never be — only the light is.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(relightqCount(&q) == 1);
	CHECK(lightSnapshot(&s_world, TX >> 4, TZ >> 4, queued_pre));

	// Before the drain the column still holds the light it had while the stone was there, so
	// it differs from arm A. THIS is the check that stops the whole case from passing for an
	// interact.c that ignores the queue and relights inline anyway — without it, "equal after
	// the drain" is trivially true for a build that never deferred anything.
	CHECK(memcmp(queued_pre, inline_snap, SNAP_BYTES) != 0);

	CHECK(drainRelights(&q) == 1);
	CHECK(lightSnapshot(&s_world, TX >> 4, TZ >> 4, queued_post));

	// The claim: all 32768 cells, both channels, identical either way.
	CHECK(memcmp(queued_post, inline_snap, SNAP_BYTES) == 0);

	// Proof this comparison can actually go red — a single nibble in a single cell, moved by
	// one, in a buffer of 65536 bytes. A memcmp that had been written against the wrong
	// length, or over a buffer nothing filled, would pass the check above and fail here.
	queued_post[2 * (size_t)((TY + 1) * CHUNK_DIM * CHUNK_DIM)] ^= 1u;
	CHECK(memcmp(queued_post, inline_snap, SNAP_BYTES) != 0);

	free(inline_snap);
	free(queued_pre);
	free(queued_post);
	interactSetRelightQueue(NULL);
}

// The place-path twin of the break case above, and the same three-way comparison.
static void testAPlacedBlocksQueuedRelightMatchesTheInlineOne(void)
{
	uint8_t* inline_snap = (uint8_t*)malloc(SNAP_BYTES);
	uint8_t* queued_post = (uint8_t*)malloc(SNAP_BYTES);
	CHECK(inline_snap != NULL && queued_post != NULL);
	if (inline_snap == NULL || queued_post == NULL) {
		free(inline_snap); free(queued_post);
		return;
	}

	const Body body = farAwayBody();
	Interact it;

	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;
	CHECK(propagateBaseline());
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(it.placed == 1);
	CHECK(lightSnapshot(&s_world, TX >> 4, TZ >> 4, inline_snap));

	RelightQueue q;
	relightqInit(&q);
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;
	interactSetRelightQueue(&q);
	CHECK(propagateBaseline());
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(it.placed == 1);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);
	CHECK(relightqCount(&q) == 1);

	CHECK(drainRelights(&q) == 1);
	CHECK(lightSnapshot(&s_world, TX >> 4, TZ >> 4, queued_post));
	CHECK(memcmp(queued_post, inline_snap, SNAP_BYTES) == 0);

	free(inline_snap);
	free(queued_post);
	interactSetRelightQueue(NULL);
}

// A full queue must relight inline rather than drop the request — world/relightq.h's stated
// overflow posture, and the same fallback source/main.c:2957 already relies on. Filled with
// columns this test never edits, so the only way the edited column gets its light is the
// fallback path.
static void testAFullQueueStillRelightsInline(void)
{
	RelightQueue q;
	relightqInit(&q);
	for (int i = 0; i < RELIGHTQ_CAP; i++)
		CHECK(relightqPush(&q, 1000 + i, 2000 + i));
	CHECK(relightqCount(&q) == RELIGHTQ_CAP);

	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	interactSetRelightQueue(&q);
	CHECK(propagateBaseline());

	Column* col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, TX, TY, TZ) == 0);   // the stone blocks the sky

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	// Nothing was queued (there was no room), and the queue counted the refusal.
	CHECK(relightqCount(&q) == RELIGHTQ_CAP);
	CHECK(relightqOverflows(&q) > 0);

	// So the light must already be right, with no drain anywhere.
	col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetSky(col, TX, TY, TZ) == 15);

	interactSetRelightQueue(NULL);
}

// ── apples (v1.8.8: "apples dropping from leaves, pickable and functional") ─────────────
//
// interact.c's breakComplete() rolls a POSITIONAL hash (world/rng.h's rngHash3) rather than
// exposing the roll itself — appleDropRoll and blockIsAppleBearingLeaves are static to that
// file, the same way this file has never reached into any of interact.c's other static
// helpers. What is tested here is the OBSERVABLE behaviour: what it->broke_id reads after a
// real break, driven through the real interactEdit()/breakComplete() path, at coordinates a
// hand-written Python replica of rngHash3 (byte-for-byte, matching the salt 0xA9D1E0 in
// interact.c) already evaluated before this test was written. This file does not reimplement
// the hash; it checks the real one against numbers worked out independently of it.
//
// Deliberately NOT at (TX,TY,TZ) — the fixture every other case in this file shares. Both
// BLOCK_LEAVES and BLOCK_BIRCH_LEAVES were checked against that exact coordinate by the same
// replica before this feature was written, and neither rolls a hit there, which is what keeps
// testEachBlockTakesItsOwnHardness() and testEveryCoreBlockStillBreaks() (both of which
// assert broke_id == the leaf's own id at that fixture) green without touching either case.

// A fresh world and registry, with `target_block` placed at an arbitrary (x,y,z) rather than
// the shared (TX,TY,TZ) fixture. Mirrors freshAimedAt()'s body; kept separate rather than
// parameterising that function, because every other case in this file depends on the fixture
// staying fixed at TX/TY/TZ and a silent extra parameter is how that would stop being true.
static void freshAimedAtCoord(Interact* it, BlockId target_block, int x, int y, int z)
{
	registryInitCore();
	worldExit(&s_world);
	worldInit(&s_world);
	s_edits_sent   = 0;
	s_last_block   = 0xFF;
	s_send_ok      = true;
	s_session_live = false;
	interactSetRelightQueue(NULL);

	worldSet(&s_world, x, y, z, target_block);

	interactInit(it);
	it->target.hit  = true;
	it->target.x    = x;
	it->target.y    = y;
	it->target.z    = z;
	it->target.face = FACE_TOP;
	it->target.px   = x;
	it->target.py   = y + 1;
	it->target.pz   = z;
}

// One break at (x,y,z) in the world the caller already has, re-aiming and re-placing the
// block fresh each call. Used by the grid sweep below, where paying freshAimedAtCoord's full
// registry/world reset for each of 5000 cells would redo the same init work five thousand
// times for no reason: only the one cell being broken needs to start fresh, and this sets
// that itself.
static BlockId breakOneAndReadDrop(Interact* it, int x, int y, int z, BlockId block)
{
	worldSet(&s_world, x, y, z, block);
	interactInit(it);
	it->target.hit  = true;
	it->target.x    = x;
	it->target.y    = y;
	it->target.z    = z;
	it->target.face = FACE_TOP;
	it->target.px   = x;
	it->target.py   = y + 1;
	it->target.pz   = z;

	const Body body = farAwayBody();
	// One oversized-tick frame lands the break in a single call: breakProgress starts the
	// hold and completes it in the same call once break_ticks (now `ticks`) reaches
	// break_need, and nothing between here and there is being timed.
	holdBreakFrame(it, &body, 1000);
	return it->broke_id;
}

#define APPLE_HIT_X   9
#define APPLE_HIT_Y  40
#define APPLE_HIT_Z  36
#define APPLE_MISS_X  0
#define APPLE_MISS_Y 40
#define APPLE_MISS_Z  0

// THE feature. A known hit and a known miss, worked out independently before this test was
// written — this is the real hash being checked against numbers it did not produce.
static void testALeafSometimesDropsAnApple(void)
{
	Interact it;
	const Body body = farAwayBody();

	freshAimedAtCoord(&it, BLOCK_LEAVES, APPLE_HIT_X, APPLE_HIT_Y, APPLE_HIT_Z);
	CHECK(holdBreakUntilDone(&it, &body, 20) > 0);
	CHECK(it.broke_id == (ItemId)BLOCK_APPLE);
	CHECK(worldGet(&s_world, APPLE_HIT_X, APPLE_HIT_Y, APPLE_HIT_Z) == BLOCK_AIR);

	freshAimedAtCoord(&it, BLOCK_LEAVES, APPLE_MISS_X, APPLE_MISS_Y, APPLE_MISS_Z);
	CHECK(holdBreakUntilDone(&it, &body, 20) > 0);
	CHECK(it.broke_id == (ItemId)BLOCK_LEAVES);   // the ordinary case: the leaf drops itself
}

// Spruce is explicitly excluded — registry.c's own row comment says apples "grow in oak and
// birch canopies", never spruce — checked at the SAME coordinate an oak/birch leaf rolls a
// hit at, so this is a real exclusion and not just "spruce was never tried at a hitting cell".
static void testSpruceLeavesNeverDropAnApple(void)
{
	Interact it;
	const Body body = farAwayBody();

	freshAimedAtCoord(&it, BLOCK_SPRUCE_LEAVES, APPLE_HIT_X, APPLE_HIT_Y, APPLE_HIT_Z);
	CHECK(holdBreakUntilDone(&it, &body, 20) > 0);
	CHECK(it.broke_id == (ItemId)BLOCK_SPRUCE_LEAVES);
}

// ── the statistical rate ─────────────────────────────────────────────────────────────────
//
// A 50x50 grid (2500 cells, y fixed at 40) is exactly the sweep the Python replica ran before
// any of this was written, and the two counts pinned below are what it printed. Reproducing
// them here through the REAL interactEdit()/breakComplete() path — not by calling rngHash3
// directly, which this file cannot do; appleDropRoll is static to interact.c — is what proves
// the production code matches the independently-worked-out numbers rather than merely
// matching itself.
static void testAppleDropRateOverAGrid(void)
{
	Interact it;
	int leaves_hits = 0, birch_hits = 0, spruce_hits = 0;

	for (int z = 0; z < 50; z++) {
		for (int x = 0; x < 50; x++) {
			if (breakOneAndReadDrop(&it, x, 40, z, BLOCK_LEAVES) == (ItemId)BLOCK_APPLE)
				leaves_hits++;
			if (breakOneAndReadDrop(&it, x, 40, z, BLOCK_BIRCH_LEAVES) == (ItemId)BLOCK_APPLE)
				birch_hits++;
			// Spruce swept over the SAME 2500 cells as leaves and birch, not just checked at
			// one coordinate. A single-coordinate spruce check was tried first and sabotaging
			// blockIsAppleBearingLeaves() to also accept BLOCK_SPRUCE_LEAVES slipped past it
			// clean — the one coordinate that test used happens not to roll a hit for spruce's
			// id (the id feeds the hash, so leaves and spruce roll independently at the same
			// cell), so a check that could not tell the sabotaged build from the real one was
			// proving nothing. Sweeping the grid and pinning the count at zero is what a
			// leaves-rate-sized sabotage (in this arm, effectively another ~7-in-2500) cannot
			// pass through unnoticed.
			if (breakOneAndReadDrop(&it, x, 40, z, BLOCK_SPRUCE_LEAVES) == (ItemId)BLOCK_APPLE)
				spruce_hits++;
		}
	}

	// Roughly 2500/200 = 12.5 expected of a true 1-in-200 draw; a deterministic positional
	// hash over a finite grid is not required to land near its own expectation, and these two
	// numbers are pinned because they are what the hash actually produces, not because they
	// are close to 12.5.
	CHECK(leaves_hits == 7);
	CHECK(birch_hits  == 9);
	CHECK(spruce_hits == 0);
}

int main(void)
{
	// v1.8.6: see the section comment above testABreakRelightsTheColumnItLeftBehind for why
	// this line exists. Left on for the rest of main(), the same way it stays on for the rest
	// of a real session once scene/chunk_render.c calls it at boot — nothing below needs the
	// gate closed, so there is no matching lightEngineInit(false).
	lightEngineInit(true);

	testTheCarryCeilingIsWhereItSays();
	testABreakOnACarryUnholdableBlockChangesNothing();
	testTallGrassBreaksAndDropsNothing();
	testAnOrdinaryBlockStillDropsItself();
	testRepeatedBreakAttemptsStillChangeNothing();
	testACoreBlockStillBreaks();
	testEveryCoreBlockStillBreaks();

	// v1.8.8 — the item ceiling. The cactus case is the reported defect; the guard case is
	// the branch that used to enforce the old ceiling and now has almost nothing left to
	// refuse.
	testTheCactusBreaksAndDropsItself();
	testTheBreakGuardStillRefusesTheUncarryable();

	testACoreBlockStillPlaces();
	testADynamicBlockStillPlaces();
	testAimingAtNothingIsStillARefusal();
	testPlacingWithAnEmptyHandIsStillARefusal();
	testPlacingIntoTheOwnBodyIsStillARefusal();

	// v1.8.1 task 50.
	testABreakDoesNotHappenOnThePressEdge();
	testEachBlockTakesItsOwnHardness();
	testReleasingTheButtonThrowsAwayProgress();
	testLookingAtAnotherBlockThrowsAwayProgress();
	testTheBlockChangingUnderTheCrosshairRestarts();
	testTheCrackStageTracksProgress();
	testCatchUpTicksStillLandTheBreak();
	testZeroAndNegativeTicksKeepTheHold();
	testPlaceIsStillOnePressAndIgnoresTheClock();

	// v1.8.6.
	testABreakRelightsTheColumnItLeftBehind();
	testAPlaceRelightsTheColumnItLeftBehind();

	// v1.8.7 F1 — the discarded send result.
	testAFailedSendInASessionUndoesTheBreak();
	testAFailedSendInASessionUndoesThePlace();
	testAFailedSendWithNoSessionStillBreaksAndPlaces();

	// v1.8.7 F2 — the relight is queued, not inline.
	testQueuedAndInlineRelightsAgreeByteForByte();
	testAPlacedBlocksQueuedRelightMatchesTheInlineOne();
	testAFullQueueStillRelightsInline();

	// v1.8.8 — apples from leaves.
	testALeafSometimesDropsAnApple();
	testSpruceLeavesNeverDropAnApple();
	testAppleDropRateOverAGrid();

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
