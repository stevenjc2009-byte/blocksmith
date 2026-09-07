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
// v1.9.0: CHEST_SLOTS, for the chest arm of the contents spy and for the ceiling pin below.
// Header-only — nothing here calls into world/chest.c, so this stanza's link is unchanged.
#include "world/chest.h"
#include "world/inventory.h"
#include "world/light.h"
// v1.8.8: the cactus case asserts the break landed on the tick breakTicksRequired() says it
// should, rather than on "some tick > 0". world/mining.c is already in this binary's link
// line — scene/interact.c calls it — so this is a header the suite was missing, not a new
// dependency.
#include "world/mining.h"
// v1.8.16: the place path now consults itemIsPlaceable(), and the rule is pinned directly as
// well as driven through interactEdit() — see the section above main().
#include "world/placeable.h"
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
	// v1.8.16 F1: back to "no reader registered" for every case, so the cases below that DO
	// register brokeContentsSpy cannot leak into the twenty-odd written before this fix
	// existed — same reasoning as the relight-queue reset immediately above.
	interactSetBrokeContentsFn(NULL);
	// v1.9.0: and back to "no bag predicate registered", which interact.h defines as "yes,
	// always" — the pre-v1.9.0 behaviour every case above this line was written against. Same
	// leak-prevention reasoning as the two resets immediately above it.
	interactSetBagFitsFn(NULL);

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
	// rather than a compile-time constant: id 37 is the last defined core row and id 38 is
	// the first undefined one, now that v1.8.14's four raw meats (ids 34..37) have landed on
	// top of v1.8.12's six ores (ids 28..33), which landed on top of v1.8.10's torch (id 27),
	// which landed on top of v1.8.8's twelve per-biome rows (ids 15..26). registryCount() is
	// read here rather than hard-coded so this pair keeps straddling the real edge if another
	// core row ever lands on top of this one.
	//
	// The NAMED constant on the right is the half that does NOT update itself, and it has now
	// gone stale four times. It is deliberately not replaced by a bare 42: the point of naming the
	// block is that the failure message says WHICH block the table now ends at, which is what
	// tells the next reader whether a row was added on purpose or lost by accident. The price
	// is that this line has to be edited whenever the table grows, and the noisy failure when
	// nobody does is the thing being bought, not a defect in the check.
	//
	// v1.8.15 "Furnace" made it four, moving the edge to BLOCK_FURNACE (id 42) past the four
	// cooked meats (38..41). Worth recording HOW this file was found, because a grep did not
	// find it and could not have: `git diff` reports interact_test.c clean against HEAD, since
	// the furnace work touched interact.c and interact.h but never this file. Its TEXT did not
	// change; its INPUT did. This was the SIXTH file in this one version to go stale that way,
	// after inventory_test.c, session_test.c, atlas_uv_shader_test.c, networld_test.c and the
	// server's own bsgame_test.c — every one of them clean in `git diff`, every one of them
	// found by running the full suite. After appending a core row the instrument is a suite
	// run, not a grep, and "I did not touch that file" reasons about the wrong thing entirely.
	//
	// v1.9.0 "Storage" made it five, moving the edge again to BLOCK_CHEST (id 43). This file
	// went stale in exactly the way the paragraph above describes and was found in exactly the
	// way it prescribes: `git diff` reported interact_test.c clean, and the full suite reported
	// `FAIL L304 last_defined == (ItemId)BLOCK_FURNACE`. The prediction held on the first
	// version to test it, which is why that paragraph is kept rather than trimmed.
	const ItemId last_defined  = (ItemId)(registryCount() - 1);
	const ItemId first_beyond  = (ItemId)registryCount();
	CHECK(last_defined == (ItemId)BLOCK_CHEST);          // premise: the table is 44 rows
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

	// v1.8.10: the WIRE span now tracks the registry too, so it is the SAME ceiling as the
	// bag's rather than a smaller one. Landing it needed the server half first, and that has
	// shipped: deps/blocksmith-server is on v1.9.1, whose bsgame.c guards a PICKUP with
	// inventoryCanHold(a) rather than `a < BS_BLOCK_COUNT`, and whose own suite carries an
	// end-to-end check that "a PICKUP of an item id past the old BS_BLOCK_COUNT ceiling is
	// credited, not dropped". Before that, a broken cactus vanished on a multiplayer rejoin.
	//
	// BLOCK_COUNT itself has NOT moved and must not: it is the frozen core-id span, tied by a
	// _Static_assert in the server's game/validate.c. What moved is the predicate, not the
	// constant.
	CHECK(inventoryItemOnWire((ItemId)BLOCK_PLANKS));    // 7, the last CORE id
	CHECK(inventoryItemOnWire((ItemId)BLOCK_CACTUS));    // past the core span, carryable AND
	                                                     // now sendable — this is the widening
	CHECK(!inventoryItemOnWire((ItemId)BLOCK_WATER));    // 8, past the span but a LIQUID: the
	                                                     // bag will not hold it, so neither will
	                                                     // the wire. Being one past BLOCK_COUNT
	                                                     // is no longer what decides this.
	CHECK(!inventoryItemOnWire(ITEM_NONE));
	// Still refused, and these are the checks that keep the widening honest: the predicate
	// tracks what the REGISTRY defines, so an undefined id and the reserved row stay off the
	// wire. Without these two a `return true;` would pass everything above.
	CHECK(!inventoryItemOnWire((ItemId)REG_ID_DYN_HI));  // never registered by this suite
	CHECK(!inventoryItemOnWire((ItemId)0xFF));           // reserved, never a row
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
	// v1.8.12: 26 -> 32. Thirty-three rows past air, less water. The six ores (ids 28..33) are
	// all FULL_CUBE and all targetable, so they land entirely in this counter and none of them
	// touches cross_seen, which is why that number does NOT move. Both figures below were read
	// off a real run of this binary rather than worked out on paper: the arithmetic and the
	// measurement agreed, but the measurement is what is quoted.
	//
	// v1.8.14: 32 -> 36, same shape of move. The four raw meats (ids 34..37) are FULL_CUBE and
	// SOLID like the ores, so all four land in targetable_seen and cross_seen again does NOT
	// move. Measured, not reasoned: the previous number went red as
	// "interact self-test: FAIL 1/732  L562 targetable_seen == 32", and 36 is what the loop
	// counted on the run after the four rows landed.
	//
	// v1.8.15: 36 -> 41, same shape a third time. The four cooked meats (38..41) and the furnace
	// (42) are all FULL_CUBE and SOLID, so all five land in targetable_seen and cross_seen stays
	// at 9 — no CROSS row was added this version. Measured, not reasoned, exactly as the entry
	// above insists: the stale number went red as "interact self-test: FAIL 2/757  L568
	// targetable_seen == 36", and 41 is what the loop counted afterwards.
	//
	// v1.9.0: 41 -> 42, a fourth time and the smallest move yet. One row, the chest (43), which
	// is FULL_CUBE and SOLID like the furnace before it, so it lands in targetable_seen and
	// cross_seen stays at 9 again. Measured, not reasoned, as every entry above insists: the
	// stale number went red as "interact self-test: FAIL 3/1143  L591 targetable_seen == 41",
	// and 42 is what the loop counted afterwards.
	CHECK(targetable_seen == 42);   // 43 rows past air, less water
	CHECK(cross_seen == 9);         // tall grass, dead bush, fern, tall grass top,
	                                 // poppy, daisy, bluebell, orchid, torch
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

// ── v1.8.10 "Light": the torch, end to end through the real edit path ──────────────────
//
// world/registry.c's [27] row gave BLOCK_TORCH the first non-zero luminance in the game
// (14), and light_luminance_test.c pins that row and light_seam_test.c pins the engine's
// falloff and cross-column handoff — but nothing before this file placed a torch through
// interactEdit() itself, which is the only path a player's press actually takes. The claim
// under test is the player-visible one: put a torch down and the world gets brighter, take
// it back out and the world gets exactly as dark as it was, not just darker than it was
// with the torch there.
//
// Same fixture as the two v1.8.6 relight cases above (TX,TY,TZ stone, place cell one cell
// above it, "otherwise-empty column" — see the comment on testABreakRelightsTheColumnItLeftBehind
// for why open air on every side of the place cell is a premise this file has already
// established rather than a new assumption). Reused rather than a fresh coordinate, so
// nothing here has to re-derive that premise.
static void testPlacingATorchLightsUpTheWorldAndBreakingItDarkensItAgain(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_TORCH;

	// Read from the registry, not hardcoded: a change that silently dropped the torch's
	// brightness must make THIS check wrong, not slip past an assertion that already agreed
	// with the regression.
	const uint8_t lum = registryGet(BLOCK_TORCH)->luminance;
	CHECK(lum > 0);   // premise — nothing below means anything if this block does not emit

	LightQueue* q = (LightQueue*)malloc(sizeof(LightQueue));
	CHECK(q != NULL);
	if (!q) return;
	lightQueueInit(q);
	CHECK(lightPropagateColumn(&s_world, TX >> 4, TZ >> 4, q));
	free(q);

	Column* col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);

	// Step 1: the baseline. Nothing in this fixture emits yet, so block light reads dark at
	// every point the sample cells below will look — pinned rather than assumed, so "brighter
	// after placing" has something real measured to be brighter than.
	const uint8_t base_h1 = lightGetBlock(col, TX + 1, TY + 1, TZ);
	const uint8_t base_h3 = lightGetBlock(col, TX + 3, TY + 1, TZ);
	const uint8_t base_v1 = lightGetBlock(col, TX,     TY + 2, TZ);
	const uint8_t base_v3 = lightGetBlock(col, TX,     TY + 4, TZ);
	CHECK(base_h1 == 0 && base_h3 == 0 && base_v1 == 0 && base_v3 == 0);

	// Step 2: place it, through the real edit path — placeKey(), interactEdit(), the same
	// call testACoreBlockStillPlaces() above drives, so this is the actual press a player
	// makes and not a reimplementation of what the press is supposed to do.
	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(it.placed == 1);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_TORCH);

	// Step 3: brighter, at the torch's own declared luminance, and falling off with distance
	// in both directions sampled — horizontally and vertically — so a fix that only got one
	// axis of the engine right cannot pass this.
	col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	const uint8_t torch_v = lightGetBlock(col, TX, TY + 1, TZ);
	CHECK(torch_v == lum);

	const uint8_t h1 = lightGetBlock(col, TX + 1, TY + 1, TZ);
	const uint8_t h3 = lightGetBlock(col, TX + 3, TY + 1, TZ);
	const uint8_t v1 = lightGetBlock(col, TX,     TY + 2, TZ);
	const uint8_t v3 = lightGetBlock(col, TX,     TY + 4, TZ);

	CHECK(h1 > base_h1 && h3 > base_h3);   // brighter than step 1 in both...
	CHECK(v1 > base_v1 && v3 > base_v3);   // ...directions sampled
	CHECK(h1 > h3);                        // ...and falls off with distance, horizontally
	CHECK(v1 > v3);                        // ...and vertically

	// Pinned exactly, not just "> 0": -1 falloff per cell is the engine's stated rule
	// (light.c's spread(), and the crossed-value checks in tests/light_seam_test.c), and
	// open air on every side of this fixture (the v1.8.6 comment above) means nothing here
	// should fall short of that straight line.
	CHECK(h1 == (uint8_t)(lum - 1));
	CHECK(h3 == (uint8_t)(lum - 3));
	CHECK(v1 == (uint8_t)(lum - 1));
	CHECK(v3 == (uint8_t)(lum - 3));

	// Step 4: break it, through the same real path. Only the crosshair's y needs to move —
	// freshAimedAt() already aimed x/z at the torch's column, and target.px/py/pz already
	// name this exact cell (they were the place cell) — the same minimal-retarget style
	// testLookingAtAnotherBlockThrowsAwayProgress() uses above rather than building a second
	// fixture from scratch.
	it.target.y = TY + 1;

	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == 1);   // hardness 1: gone on tick 1,
	                                                            // same as every other CROSS row
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
	CHECK(it.broke_id == BLOCK_AIR);   // BLOCK_SHAPE_CROSS drops nothing — same family as tall
	                                    // grass; testEveryCoreBlockStillBreaks() already counts
	                                    // the torch into its cross_seen == 9

	// Step 4, the important half: every sampled cell is back to EXACTLY what it read in step
	// 1 — not merely dimmer than it was with the torch there. lightRelightColumn() fully
	// recomputes the column from its seeds rather than patching a delta, so a broken
	// retraction shows up here as a wrong number, not a crash, and asserting with == rather
	// than < is what makes that visible instead of passing on "darker than lit".
	col = worldColumn(&s_world, TX >> 4, TZ >> 4);
	CHECK(col != NULL);
	CHECK(lightGetBlock(col, TX,     TY + 1, TZ) == 0);   // the torch cell itself
	CHECK(lightGetBlock(col, TX + 1, TY + 1, TZ) == base_h1);
	CHECK(lightGetBlock(col, TX + 3, TY + 1, TZ) == base_h3);
	CHECK(lightGetBlock(col, TX,     TY + 2, TZ) == base_v1);
	CHECK(lightGetBlock(col, TX,     TY + 4, TZ) == base_v3);
	CHECK(lightColumnsAttached() == 1);   // still one column — the v1.8.6 leak fix, not a leak
}

// ── v1.8.16: food is an ITEM, not a block ───────────────────────────────────────────────
//
// steve's ask, verbatim: "make the apple an item, not a block, similar to Minecraft."
//
// Every id in world/registry.c is today BOTH a block and an inventory item, so an apple in the
// hotbar could be PLACED as a solid one-metre cube of apple. That is the whole defect. The fix
// is world/placeable.c's itemIsPlaceable(), consulted at the ONE chokepoint every
// player-initiated placement passes through — scene/interact.c's `it->holding == BLOCK_AIR`
// refusal, the first arm of the only `fresh & key_place` branch in interactEdit().
//
// WHY THE REGISTRY ROW IS NOT TOUCHED, since that is the obvious move and it is wrong: the
// apple has to stay SOLID/FULL_CUBE or world/block.h's blockDropsNothing() answers "yes" from
// the SHAPE and breakComplete() hands the bag BLOCK_AIR — which would kill the apple supply
// from leaves outright, the exact regression registry.c:495-500 already warns about in as many
// words. A registry FLAG is equally unavailable: registry.h:37-43 spends bits 0-4 and bits 5-7
// are REG_SHAPE_MASK, so a sixth flag trips the _Static_assert at registry.h:67-69, and
// escaping that grows BlockDef past 27 bytes and changes both the DEFS packet and the
// registry.bin format. A plain list outside the registry is what world/survival.c's
// survivalFoodValue() already does for hunger values, and this is that same precedent.
//
// WHAT THE REFUSAL COSTS THE PLAYER: nothing. Refusing at that arm returns with placed_id left
// at BLOCK_AIR, and placed_id is the ONLY channel source/main.c:6024 reads to decide whether to
// charge the hotbar — so a refused food press consumes no item. That is asserted below rather
// than argued: `placed_id == BLOCK_AIR` in every food case IS the "charged nothing" check at
// this layer.
//
// The list here is written out INDEPENDENTLY of world/placeable.c's own table. Two copies of
// the same array agreeing with each other proves nothing; two hand-written lists agreeing is a
// real cross-check, and the sweep below covers the case where placeable.c grows an entry this
// list never heard of.
static const ItemId kFoodItems[] = {
	BLOCK_APPLE,                                                                  // 26
	BLOCK_RAW_PORKCHOP,    BLOCK_RAW_BEEF,    BLOCK_RAW_CHICKEN,    BLOCK_RAW_MUTTON,     // 34..37
	BLOCK_COOKED_PORKCHOP, BLOCK_COOKED_BEEF, BLOCK_COOKED_CHICKEN, BLOCK_COOKED_MUTTON,  // 38..41
};
#define FOOD_ITEM_COUNT ((int)(sizeof kFoodItems / sizeof kFoodItems[0]))

static bool isFoodItem(BlockId id)
{
	for (int i = 0; i < FOOD_ITEM_COUNT; i++)
		if (kFoodItems[i] == id) return true;
	return false;
}

// Hold `held`, aim at stone, press place once. Every food case below is this same press, so the
// thing that differs between them is the id and nothing else.
static void placeOnce(Interact* it, BlockId held)
{
	freshAimedAt(it, BLOCK_STONE);
	it->holding = held;
	const Body body = farAwayBody();
	interactEdit(it, &s_world, &body, placeKey(), 0, 0);
}

// The rule itself, pinned directly rather than only through a press. The cases below drive the
// real interactEdit() and are what actually prove the feature; this one exists so that a rule
// which broke in a way the place path happened to mask still goes red, and so the two
// deliberate answers at the edges are written down rather than left to be rediscovered.
static void testThePlaceableRuleItself(void)
{
	registryInitCore();

	for (int i = 0; i < FOOD_ITEM_COUNT; i++)
		CHECK(!itemIsPlaceable(kFoodItems[i]));

	// Ordinary blocks, one from each family the game has: plain terrain, a crafted block, a
	// CROSS plant, the liquid, and the newest FULL_CUBE row. None of them is food and every one
	// must stay placeable.
	CHECK(itemIsPlaceable(BLOCK_STONE));
	CHECK(itemIsPlaceable(BLOCK_DIRT));
	CHECK(itemIsPlaceable(BLOCK_PLANKS));
	CHECK(itemIsPlaceable(BLOCK_TORCH));
	CHECK(itemIsPlaceable(BLOCK_WATER));
	CHECK(itemIsPlaceable(BLOCK_CACTUS));
	CHECK(itemIsPlaceable(BLOCK_LEAVES));

	// BLOCK_AIR answers TRUE, deliberately. The rule is about food, and an empty hand is
	// refused one step earlier by interactEdit's own `it->holding == BLOCK_AIR` arm, which is
	// left exactly as it was — folding the empty-hand case in here would move a rule that has
	// nothing to do with food and make testPlacingWithAnEmptyHandIsStillARefusal() pass for a
	// different reason than it claims.
	CHECK(itemIsPlaceable(BLOCK_AIR));

	// A server-registered dynamic id has no local row and must stay placeable, which is what
	// makes the list a DENYlist rather than an allowlist. An allowlist would refuse every modded
	// server's blocks the day it shipped, and testADynamicBlockStillPlaces() above is the
	// end-to-end half of this same claim.
	CHECK(itemIsPlaceable((ItemId)REG_ID_DYN_LO));
	CHECK(itemIsPlaceable((ItemId)REG_ID_DYN_HI));

	// An id with no row at all. Undefined is not the same question as unplaceable, and this
	// pins which answer it gets so a future reader does not have to guess.
	CHECK(itemIsPlaceable((ItemId)(REG_ID_CORE_HI)));
}

// THE reported ask, as its own case. steve named the apple, so the apple gets an answer that
// does not depend on reading a loop bound — the same reason testTheCactusBreaksAndDropsItself()
// sits beside testEveryCoreBlockStillBreaks() rather than inside it.
//
// All six checks are red against the pre-fix client, where this press puts a cube of apple in
// the world.
static void testAnAppleCannotBePlacedAsABlock(void)
{
	Interact it;
	placeOnce(&it, BLOCK_APPLE);

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);   // the world did not change
	CHECK(it.placed == 0);
	CHECK(it.placed_id == BLOCK_AIR);      // main.c:6024's channel: the hotbar is charged nothing
	CHECK(it.placed_valid == false);       // and no position is offered to a side-table cleanup
	CHECK(it.refused == 1);                // counted as a refusal, this module's word for "nothing happened"
	CHECK(s_edits_sent == 0);              // and nothing went to the server either
}

// The apple was never alone. A raw porkchop placeable as a solid cube is the identical
// wrongness, so all nine food ids are covered — one press each, all six assertions each.
static void testEveryFoodItemIsRefusedByThePlacePath(void)
{
	// Premise: the list above is not empty and every id in it really has a registry row, so a
	// typo'd id cannot make this loop pass by refusing something that was never placeable.
	CHECK(FOOD_ITEM_COUNT == 9);

	for (int i = 0; i < FOOD_ITEM_COUNT; i++) {
		const ItemId food = kFoodItems[i];

		Interact it;
		placeOnce(&it, food);

		CHECK(registryIsDefined(food));
		CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
		CHECK(it.placed == 0);
		CHECK(it.placed_id == BLOCK_AIR);
		CHECK(it.placed_valid == false);
		CHECK(it.refused == 1);
		CHECK(s_edits_sent == 0);
	}
}

// Breaking is UNCHANGED, and this is the check that says so. The fix guards the place path and
// only the place path: an apple must still be pickable out of a leaf (v1.8.8's whole feature)
// and a meat must still be breakable back out of the world where worldgen or an older save left
// one. A blanket "food ids are inert" would pass every case above this one and silently delete
// the feature steve asked for in the version before this one.
static void testFoodStillBreaksAndStillReachesTheBag(void)
{
	for (int i = 0; i < FOOD_ITEM_COUNT; i++) {
		const ItemId food = kFoodItems[i];

		Interact it;
		freshAimedAt(&it, food);
		const Body body = farAwayBody();

		CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
		CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
		CHECK(it.broke_id == food);        // it goes in the bag, as itself
		CHECK(it.refused == 0);
		CHECK(s_edits_sent == 1);
		CHECK(inventoryCanHold(food));     // and the bag still admits it
	}
}

// The regression guard, over the WHOLE core id space rather than one hand-picked block, for the
// same reason testEveryCoreBlockStillBreaks() walks the space instead of naming stone: the fix
// is a predicate over ids, and a version of it that happened to refuse the apple and also
// something else would pass every case above.
//
// The two counters are the vault's "a check that could not fail proves nothing", written down:
// a predicate that refused everything, or refused nothing, moves one of them.
static void testEveryNonFoodCoreBlockStillPlaces(void)
{
	CHECK(registryCount() > BLOCK_COUNT);   // premise, same as the break sweep's

	int placed_ok    = 0;
	int food_refused = 0;

	for (BlockId id = BLOCK_GRASS; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined(id)) continue;

		Interact it;
		placeOnce(&it, id);

		if (isFoodItem(id)) {
			food_refused++;
			CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
			CHECK(it.placed == 0);
			CHECK(it.refused == 1);
			CHECK(s_edits_sent == 0);
		} else {
			placed_ok++;
			CHECK(worldGet(&s_world, TX, TY + 1, TZ) == id);
			CHECK(it.placed == 1);
			CHECK(it.placed_id == id);
			CHECK(it.refused == 0);
			CHECK(s_edits_sent == 1);
		}
	}

	// Both numbers read off a real run of this binary, never worked out on paper — the standing
	// rule this file's break sweep states three times over. Seeded as -1 and -1 first, so the
	// first run printed what they actually are.
	// v1.9.0: 33 -> 34. The chest (43) is not a food item, so it takes the placeable arm above.
	// Read off the run that went red as "interact self-test: FAIL 3/1143  L1899 placed_ok == 33",
	// not adjusted on paper.
	CHECK(placed_ok    == 34);
	CHECK(food_refused == 9);
}

// ── v1.8.16 F1: a broken furnace must not silently destroy its contents ────────────────
//
// THE defect: main.c's break-cleanup calls blockStateRemove() on a broken furnace's
// FurnaceState record WITHOUT ever reading it first, so whatever sat in the input, fuel and
// output slots (ore mid-smelt, unburned fuel, a finished ingot) was thrown away with no drop
// and no message. interact.h's InteractBrokeContentsFn is the fix: breakComplete() now
// calls a registered reader BEFORE that removal happens and reports the result through
// broke_extra_item/qty, exactly the way it already reports the single-item broke_id.
//
// This is a SPY, not a stand-in for something the host cannot link — world/blockstate.c and
// world/furnace.c both build cleanly on the host (see tools/run_host_tests.sh's own
// blockstate/furnace stanzas). It exists so this file can prove breakComplete() calls the
// registered reader, with the right arguments, and threads the result into Interact's public
// fields correctly — without dragging blockstate.c/furnace.c into THIS binary's link, which
// is the whole point of the callback shape (see interact.h).
static int     s_spy_calls;
static BlockId s_spy_last_id;
static int     s_spy_last_x, s_spy_last_y, s_spy_last_z;
static int     s_spy_return;      // how many entries to hand back; < 0 exercises the clamp

// Fixed content, standing in for a real furnace's input/fuel/output: a real recipe pair
// (world/furnace.c's FURNACE_RECIPE_PORK) plus a real fuel item, so nothing here is inventing
// an item combination the game could not actually produce.
//
// v1.9.0: sized SPY_FURNACE_SLOTS (3) rather than INTERACT_BROKE_EXTRA_MAX, because those two
// numbers came apart the moment the ceiling went 3 -> 8 for the chest. Left at
// INTERACT_BROKE_EXTRA_MAX these arrays would have gained five zero-filled tail entries, and
// testBrokenFurnaceClampsAnOverclaimingReader below -- whose whole point is that
// breakComplete() clamps to the ceiling -- would have quietly started measuring the zero-qty
// SKIP instead, passing for a reason unrelated to what it claims. A furnace has three slots;
// that is what this constant now says, out loud.
#define SPY_FURNACE_SLOTS 3
static const BlockId kSpyItems[SPY_FURNACE_SLOTS] =
	{ BLOCK_RAW_PORKCHOP, BLOCK_PLANKS, BLOCK_COOKED_PORKCHOP };
static const uint8_t kSpyQtys[SPY_FURNACE_SLOTS] = { 3, 7, 1 };

// v1.9.0: the CHEST arm of the same reader. A chest enforces no per-slot rule (world/chest.h
// says so at length), so this is simply CHEST_SLOTS distinct carryable core blocks with
// non-zero counts. How many of them the reader hands back is s_spy_chest_n, per case.
static const BlockId kChestItems[CHEST_SLOTS] = {
	BLOCK_STONE, BLOCK_DIRT,   BLOCK_PLANKS,    BLOCK_SNOW,
	BLOCK_ICE,   BLOCK_CACTUS, BLOCK_DEAD_BUSH, BLOCK_FERN,
};
static const uint8_t kChestQtys[CHEST_SLOTS] = { 11, 22, 33, 44, 55, 66, 77, 88 };

// How many chest slots the reader reports. 0 for every case written before v1.9.0, so the
// chest arm is inert unless a case asks for it.
static int s_spy_chest_n;

// v1.9.0 F2: the chest arm's own flip, the contents-spy twin of s_fits_flip_after_call above --
// a remote CHEST_STATE landing mid-hold changes what the NEXT read of the chest finds, not what
// an earlier one already returned. s_spy_chest_flip_after_call at 0 (every case above the F2
// guard section) means every call reports s_spy_chest_n, exactly as before this pair existed.
static int s_spy_chest_n_late;
static int s_spy_chest_flip_after_call;

// v1.8.16 F1, sub-case: index 1 (the fuel slot) reports qty 0 -- an empty slot, which a real
// FurnaceState can legitimately have (no fuel loaded) and which breakComplete() must skip
// rather than hand main.c an invBridgeAdd of nothing.
static bool s_spy_zero_middle;

static int brokeContentsSpy(BlockId id, int x, int y, int z,
                             BlockId items_out[INTERACT_BROKE_EXTRA_MAX],
                             uint8_t counts_out[INTERACT_BROKE_EXTRA_MAX])
{
	s_spy_calls++;
	s_spy_last_id = id;
	s_spy_last_x  = x;
	s_spy_last_y  = y;
	s_spy_last_z  = z;

	// v1.9.0: the chest arm, ahead of the furnace one because it is the block this section's
	// new cases are about. Same shape as the furnace arm -- a real reader (main.c's) will gate
	// on the id and then blockStateGet with that id as expect_block_id, so one reader answering
	// for two block kinds is what the real one has to do too.
	if (id == BLOCK_CHEST) {
		// s_spy_calls was just incremented above, so it is already the 1-based number of
		// THIS call -- same numbering bagFitsSpy's flip uses, and it has to be: the two
		// spies are called in lockstep, once each per bagFitsBrokenBlock() invocation.
		const int n = (s_spy_chest_flip_after_call > 0 && s_spy_calls >= s_spy_chest_flip_after_call)
		              ? s_spy_chest_n_late : s_spy_chest_n;
		int write_n = n;
		if (write_n > CHEST_SLOTS) write_n = CHEST_SLOTS;
		for (int i = 0; i < write_n; i++) {
			items_out[i]  = kChestItems[i];
			counts_out[i] = kChestQtys[i];
		}
		return n;
	}

	// Only the furnace carries state today -- a real reader (main.c's) gates the same way,
	// via blockStateGet's own expect_block_id check, so this mirrors that rather than
	// inventing a different rule.
	if (id != BLOCK_FURNACE)
		return 0;

	if (s_spy_return < 0)
		return s_spy_return;   // deliberately out of range; never touches items_out/counts_out

	// Never write past the real buffer even when told to CLAIM more than that -- s_spy_return
	// itself is returned unclamped below, which is what lets testBrokenFurnaceClampsAnOverclaimingReader
	// prove breakComplete() is the one doing the clamping, not this spy.
	//
	// v1.9.0: the source arrays are SPY_FURNACE_SLOTS long and the ceiling is now wider than
	// they are, so the read wraps rather than running off the end. For every count at or under
	// three -- which is every case except the overclaim one -- this is byte-identical to the
	// straight kSpyItems[i] it replaced.
	int write_n = s_spy_return;
	if (write_n > INTERACT_BROKE_EXTRA_MAX) write_n = INTERACT_BROKE_EXTRA_MAX;

	for (int i = 0; i < write_n; i++) {
		items_out[i]  = kSpyItems[i % SPY_FURNACE_SLOTS];
		counts_out[i] = (s_spy_zero_middle && i == 1) ? 0 : kSpyQtys[i % SPY_FURNACE_SLOTS];
	}
	return s_spy_return;
}

static void resetSpy(void)
{
	s_spy_calls       = 0;
	s_spy_last_id     = BLOCK_AIR;
	s_spy_last_x = s_spy_last_y = s_spy_last_z = -1;
	// v1.9.0: SPY_FURNACE_SLOTS, not INTERACT_BROKE_EXTRA_MAX. It was the ceiling only because
	// the ceiling happened to be a furnace's slot count; now that it is a chest's, the default
	// has to say which of the two it meant, and it meant the furnace.
	s_spy_return       = SPY_FURNACE_SLOTS;
	s_spy_zero_middle  = false;
	s_spy_chest_n      = 0;
	// Off by default -- see the comment above s_spy_chest_n_late. Every case that does not
	// explicitly arm this keeps getting s_spy_chest_n on every call, as before this pair existed.
	s_spy_chest_n_late          = 0;
	s_spy_chest_flip_after_call = 0;
}

// THE case this section exists for. Every CHECK from broke_extra_count onward is red against
// the pre-fix code, which has no such field at all (see this file's md5-restore note in the
// section this test was verified against).
static void testBreakingAFurnaceReportsItsContents(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	resetSpy();
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	// The reader was called exactly once, for the right block at the right position -- proof
	// breakComplete() is asking about the cell that actually broke, not a stale one.
	CHECK(s_spy_calls == 1);
	CHECK(s_spy_last_id == BLOCK_FURNACE);
	CHECK(s_spy_last_x == TX && s_spy_last_y == TY && s_spy_last_z == TZ);

	// The furnace itself still drops, exactly as before this fix -- this is an ADDITION to
	// the existing broke_id contract, not a replacement of it.
	CHECK(it.broke_id == BLOCK_FURNACE);

	// And now its CONTENTS are reported too -- the whole point.
	CHECK(it.broke_extra_count == 3);
	CHECK(it.broke_extra_item[0] == BLOCK_RAW_PORKCHOP  && it.broke_extra_qty[0] == 3);
	CHECK(it.broke_extra_item[1] == BLOCK_PLANKS        && it.broke_extra_qty[1] == 7);
	CHECK(it.broke_extra_item[2] == BLOCK_COOKED_PORKCHOP && it.broke_extra_qty[2] == 1);

	interactSetBrokeContentsFn(NULL);
}

// An ordinary block must not gain phantom drops just because a reader happens to be
// registered -- the reader is asked about EVERY break (breakComplete does not pre-filter by
// id), so this is what proves the spy's own furnace-only gate is what the real main.c reader
// mirrors, not a coincidence of this test never breaking anything else.
static void testBreakingAnOrdinaryBlockReportsNoExtraContents(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	resetSpy();
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(s_spy_calls == 1);
	CHECK(s_spy_last_id == BLOCK_STONE);   // asked about, and correctly says no
	CHECK(it.broke_extra_count == 0);
	CHECK(it.broke_id == BLOCK_STONE);     // the ordinary drop is unaffected

	interactSetBrokeContentsFn(NULL);
}

// The default, unwired state -- what every case before this section already got, and what
// the real console gets until main.c registers a reader (see interact.h's setter comment).
// A furnace still breaks and still drops ITSELF; only its contents go unreported, which is
// the honest degrade rather than a crash or a refusal.
static void testNoReaderRegisteredMeansNoExtraContentsForAFurnace(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	interactSetBrokeContentsFn(NULL);   // explicit, though freshAimedAt already did this

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(it.broke_id == BLOCK_FURNACE);
	CHECK(it.broke_extra_count == 0);
}

// A reader that skips an empty slot (a furnace with no fuel loaded, say) must not leave a
// hole in the array -- the two real entries land at indices 0 and 1, not 0 and 2.
static void testAnEmptySlotIsSkippedNotLeftAsAHole(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	resetSpy();
	s_spy_zero_middle = true;
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(it.broke_extra_count == 2);
	CHECK(it.broke_extra_item[0] == BLOCK_RAW_PORKCHOP    && it.broke_extra_qty[0] == 3);
	CHECK(it.broke_extra_item[1] == BLOCK_COOKED_PORKCHOP && it.broke_extra_qty[1] == 1);

	interactSetBrokeContentsFn(NULL);
}

// A reader that claims MORE than INTERACT_BROKE_EXTRA_MAX entries exist must be clamped, not
// trusted -- trusting it would read past the 3-entry items[]/qtys[] stack buffers this file's
// spy is careful never to write past, and past Interact's own 3-entry broke_extra_item/qty
// arrays, which is the buffer that actually matters in production.
static void testBrokenFurnaceClampsAnOverclaimingReader(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	resetSpy();
	// More than INTERACT_BROKE_EXTRA_MAX, expressed relative to it so this case keeps
	// overclaiming if the ceiling moves again. It was the literal 5 while the ceiling was 3;
	// v1.9.0 raised the ceiling to 8 and a literal 5 would have become an UNDER-claim, quietly
	// turning this case into a second copy of the ordinary one.
	s_spy_return = INTERACT_BROKE_EXTRA_MAX + 2;
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(it.broke_extra_count == INTERACT_BROKE_EXTRA_MAX);   // clamped, not 5

	interactSetBrokeContentsFn(NULL);
}

// And a reader that claims a NEGATIVE count (a caller bug, not a real answer) must be treated
// as zero rather than underflowing a loop bound.
static void testBrokenFurnaceClampsANegativeReader(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	resetSpy();
	s_spy_return = -1;
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(it.broke_extra_count == 0);

	interactSetBrokeContentsFn(NULL);
}

// The clear-every-call contract, the same one broke_id/broke_valid already have: a furnace's
// reported contents from one break must not survive into a later call that broke nothing.
static void testBrokeExtraCountClearsOnTheNextCall(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_FURNACE);
	resetSpy();
	interactSetBrokeContentsFn(brokeContentsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
	CHECK(it.broke_extra_count == 3);

	// One more frame, nothing left to break (the cell is air now).
	holdBreakFrame(&it, &body, 1);
	CHECK(it.broke_extra_count == 0);

	interactSetBrokeContentsFn(NULL);
}

// ── v1.9.0: a chest break is REFUSED when the bag cannot take what is inside it ─────────
//
// THE defect this section exists for, stated as the thing a player would see: break a chest
// with eight stacks in it and a bag with no room, and the chest and all eight stacks are gone
// at once, silently. That is not the ordinary "one block lost on a full bag" — breakComplete()
// writes air FIRST (worldSet at the top of it) and only then reads the contents, so by the
// time anything knows what was inside, the cell it was keyed to is already empty.
//
// The existing guard beside the new one asks a TYPE question — inventoryCanHold() is
// "defined, not air, not a liquid", a property of the id — and BLOCK_CHEST passes it
// trivially. There was no capacity check anywhere in this client before this version.
//
// WHERE it had to go, and what these cases actually pin: at the start of the HOLD, next to
// that type guard, not inside breakComplete(). Checking inside breakComplete would be correct
// about the world and wrong about the player — a full crack animation on a block that was
// never going to break, which is the exact regression v1.8.1 removed when it moved the type
// guard forward. So the load-bearing assertion in the refusal case is not `broke == 0`; it is
// that the world still holds BLOCK_CHEST afterwards. A refusal placed too late passes the
// first and fails the second.
//
// The predicate is a callback for a LINK reason, not a stylistic one: inventoryAdd() lives in
// world/inventory.c and two of the three stanzas that link scene/interact.c (this one and
// audio_cue_test) do not link it. See interact.h. That makes the spy below a SPY and not a
// stand-in for something the host cannot supply — it records the exact stack list interact.c
// assembled and answers from a knob, so it decides nothing the cases do not set.

static int     s_fits_calls;
static bool    s_fits_answer;
static int     s_fits_last_n;
static BlockId s_fits_last_items[1 + INTERACT_BROKE_EXTRA_MAX];
static uint8_t s_fits_last_counts[1 + INTERACT_BROKE_EXTRA_MAX];

// v1.9.0 F2 guard cases (below, after testAChestBreakIsRefusedWhenItsContentsWillNotFit): the
// bag's free space is not fixed for the whole two-second hold, which is the entire reason
// breakComplete() asks bagFitsBrokenBlock() a SECOND time (interact.c:363) rather than trusting
// the answer breakProgress() got when the hold started. Every case above this comment gives
// bagFitsSpy one constant answer for the whole hold -- s_fits_capacity stays 0, capacity mode
// stays off, and the ternary below falls through to that same constant s_fits_answer, byte for
// byte the same body this spy always had. A case that wants the LATE call (the one immediately
// before worldSet) to answer differently from the EARLY one (the one at hold-start) sets
// s_fits_capacity_early/late and s_fits_flip_after_call instead; capacity mode judges fit from
// n itself -- the real item count bagFitsBrokenBlock assembled -- rather than from a canned
// bool, so the predicate's answer tracks what interact.c actually offered it on each call.
static int s_fits_capacity_early;   // <= 0: capacity mode off, s_fits_answer decides instead
static int s_fits_capacity_late;
static int s_fits_flip_after_call;  // 1-based call number the LATE capacity applies from; 0 = never

static bool bagFitsSpy(const BlockId* items, const uint8_t* counts, int n)
{
	s_fits_calls++;
	s_fits_last_n = n;
	for (int i = 0; i < n && i < (int)(sizeof s_fits_last_items / sizeof s_fits_last_items[0]); i++) {
		s_fits_last_items[i]  = items[i];
		s_fits_last_counts[i] = counts[i];
	}

	if (s_fits_capacity_early > 0 || s_fits_capacity_late > 0) {
		const int cap = (s_fits_flip_after_call > 0 && s_fits_calls >= s_fits_flip_after_call)
		                ? s_fits_capacity_late : s_fits_capacity_early;
		return n <= cap;
	}
	return s_fits_answer;
}

static void resetFitsSpy(bool answer)
{
	s_fits_calls   = 0;
	s_fits_answer  = answer;
	s_fits_last_n  = -1;
	memset(s_fits_last_items,  0, sizeof s_fits_last_items);
	memset(s_fits_last_counts, 0, sizeof s_fits_last_counts);
	// Capacity mode off by default -- see the comment above bagFitsSpy. Every case that does
	// not explicitly arm it (which is every case above the F2 guard section) gets the same
	// constant-answer spy this file has always had.
	s_fits_capacity_early  = 0;
	s_fits_capacity_late   = 0;
	s_fits_flip_after_call = 0;
}

// The ceiling and the chest's slot count are tied by a _Static_assert in interact.h, which
// fails the BUILD rather than a check. Pinned here as well because a static assert says
// "these two are compatible" and this says what they actually ARE — a reader who sees the
// build stay green after changing one of them learns nothing from the assert alone.
static void testTheExtraContentsCeilingCoversAWholeChest(void)
{
	CHECK(CHEST_SLOTS == 8);
	CHECK(INTERACT_BROKE_EXTRA_MAX == 8);
	CHECK(INTERACT_BROKE_EXTRA_MAX >= CHEST_SLOTS);
}

// The allowed case. A chest with three stacks in it, a bag that can take all of it: the break
// lands exactly as any other block's does, and the predicate was asked ONCE — at the moment
// the hold started, not once per frame of it.
static void testAChestBreaksWhenItsContentsFit(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(true);
	s_spy_chest_n = 3;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke == 1);
	CHECK(it.refused == 0);
	CHECK(it.broke_id == BLOCK_CHEST);
	CHECK(it.broke_extra_count == 3);

	// Asked TWICE, and each time about the block AND its contents in the order the break pays
	// them out: the chest itself first (one of it), then each non-empty slot.
	//
	// Twice, not once, since the v1.9.0 late re-check: breakComplete() asks again immediately
	// before worldSet, because the first ask happened when the break STARTED and a chest's
	// contents can change under a multi-second hold (a remote CHEST_STATE, or the bag filling
	// up meanwhile). Pinned to the exact number rather than left loose so that deleting the
	// late re-check turns this red instead of silently passing.
	CHECK(s_fits_calls == 2);
	CHECK(s_fits_last_n == 4);
	CHECK(s_fits_last_items[0] == BLOCK_CHEST && s_fits_last_counts[0] == 1);
	CHECK(s_fits_last_items[1] == kChestItems[0] && s_fits_last_counts[1] == kChestQtys[0]);
	CHECK(s_fits_last_items[2] == kChestItems[1] && s_fits_last_counts[2] == kChestQtys[1]);
	CHECK(s_fits_last_items[3] == kChestItems[2] && s_fits_last_counts[3] == kChestQtys[2]);

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// A FULL chest, all CHEST_SLOTS of it, still fitting. This is the case the ceiling raise was
// for: at INTERACT_BROKE_EXTRA_MAX 3 the last five slots would never reach the predicate and
// never reach the bag.
static void testAFullChestOffersEveryOneOfItsSlots(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(true);
	s_spy_chest_n = CHEST_SLOTS;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);

	CHECK(s_fits_last_n == 1 + CHEST_SLOTS);
	CHECK(it.broke_extra_count == CHEST_SLOTS);
	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(s_fits_last_items[1 + i]  == kChestItems[i]);
		CHECK(s_fits_last_counts[1 + i] == kChestQtys[i]);
		CHECK(it.broke_extra_item[i] == kChestItems[i]);
		CHECK(it.broke_extra_qty[i]  == kChestQtys[i]);
	}

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// THE case. The bag cannot take it, so the break never happens -- and the assertion that
// matters most is the third one: the chest is STILL THERE. A refusal written inside
// breakComplete() would satisfy `broke == 0` and leave BLOCK_AIR in the cell.
static void testAChestBreakIsRefusedWhenItsContentsWillNotFit(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(false);
	s_spy_chest_n = CHEST_SLOTS;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == -1);   // never lands, however long

	// The world is untouched. This is the one that catches a refusal placed too late.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CHEST);
	CHECK(it.broke == 0);
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.broke_valid == false);
	CHECK(it.broke_extra_count == 0);

	// Counted once per PRESS, not once per frame of a 200-frame hold -- the same accounting
	// the type guard beside it already has, and the reason `pressed` is threaded down there.
	CHECK(it.refused == 1);

	// Nothing on the wire, and no crack animation: the hold was never allowed to start, so
	// there is no progress for the overlay to draw.
	CHECK(s_edits_sent == 0);
	CHECK(it.breaking == false);
	CHECK(interactBreakStage(&it) == -1);

	// And the predicate really was consulted -- a case that refused for some other reason
	// entirely would satisfy every check above this one.
	CHECK(s_fits_calls > 0);
	CHECK(s_fits_last_n == 1 + CHEST_SLOTS);

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// ── v1.9.0 F2: the LATE re-check itself, not just its call count ────────────────────────
//
// A verify agent red-armed this file by deleting breakComplete()'s late re-check (interact.c,
// the block starting `if (broken == BLOCK_CHEST && !bagFitsBrokenBlock(...))` at line 363) and
// re-running the suite. Its verbatim finding: of 1261 checks, the only one that went red was
// `CHECK(s_fits_calls == 2)` in testAChestBreaksWhenItsContentsFit above. Every case that
// exercises breakComplete()'s BEHAVIOUR — the world cell, broke_id, broke_extra_count, refused —
// stayed green, because every one of them gives bagFitsSpy a single CONSTANT answer for the
// whole hold. A constant-true answer makes the late call redundant with the one call a
// single-ask implementation still makes; a constant-false answer (testAChestBreakIsRefusedWhen-
// ItsContentsWillNotFit, above) refuses at breakProgress()'s START-time guard (interact.c:586)
// and never reaches breakComplete()'s line 363 at all. Neither shape can tell "asked twice" from
// "asked once, then trusted".
//
// The two cases below are what was missing: the predicate's answer DIFFERS between the early
// call (breakProgress, hold start) and the late one (breakComplete, immediately before
// worldSet), so a build that only asks once necessarily gets the EARLY (fitting) answer and
// lets the break land — which spills the chest's contents into broke_extra_item/qty and writes
// BLOCK_AIR over a chest whose contents were never actually offered to a full bag. Every
// assertion below is about that world state and those contents, not about how many times
// anything was called.
// The bag fills up DURING the hold: room enough when the two-second break starts, none left by
// the time it finishes. This is interact.c:322-326's own scenario, verbatim -- a joined
// session's onInvState() can write a whole authoritative BS_APP_INV_STATE into the live
// inventory with no "a break is in progress" gate, so the free slot the start-time guard saw
// can be gone before breakComplete() is reached.
static void testAChestBreakIsRefusedWhenTheBagFillsDuringTheHold(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(true);
	s_spy_chest_n = 3;                     // contents constant across the whole hold
	// Capacity mode: call 1 (breakProgress, hold start) sees room for 10, easily fitting the
	// chest-plus-3-stacks n==4 this fixture offers. Call 2 onward (breakComplete, the late
	// re-check) sees room for only 2 -- the bag filled up while the player was still holding
	// the button down.
	s_fits_capacity_early  = 10;
	s_fits_capacity_late   = 2;
	s_fits_flip_after_call = 2;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();

	// Drive the hold for EXACTLY as many ticks as breakTicksRequired() says a chest needs --
	// not holdBreakUntilDone()'s usual NEVER_TICKS. The late re-check refuses on the tick the
	// hold would otherwise complete, and breakCancel() leaves it->breaking false; one frame
	// further and breakProgress()'s START branch re-enters on its own (interact.c's own
	// breakComplete() comment: "the following press ... asks the START-time question ... and is
	// turned away immediately") and calls bagFitsBrokenBlock() again -- correct behaviour on
	// interact.c's part, and not what this case is measuring, so the drive stops the instant the
	// late re-check has had its one chance to fire.
	const uint32_t need = breakTicksRequired(BLOCK_CHEST, it.holding);
	for (uint32_t t = 0; t < need; t++)
		holdBreakFrame(&it, &body, 1);

	// THE assertions a call-count pin cannot make. Delete the late re-check and every one of
	// these goes red together: the chest is gone (BLOCK_AIR), it.broke is 1, broke_id is
	// BLOCK_CHEST, and broke_extra_count is 3 -- the exact silent-destruction defect v1.9.0 F2
	// exists to prevent.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CHEST);   // still standing, unopened
	CHECK(it.broke == 0);
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.broke_valid == false);
	CHECK(it.broke_extra_count == 0);                       // nothing spilled into the bag

	// Conservation: the chest is the only place its three stacks were ever going to come from,
	// and it is still standing with nothing extracted -- so what was inside it before the press
	// is exactly what is inside it now. Nothing went out over the wire either.
	CHECK(s_edits_sent == 0);
	CHECK(it.refused == 1);
	CHECK(it.breaking == false);

	// The tripwire, kept -- still useful (a build that stopped asking entirely at all would
	// fail this even with the checks above coincidentally passing some other way), but no
	// longer the only thing standing guard.
	CHECK(s_fits_calls == 2);

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// The chest's own CONTENTS change during the hold, as a remote CHEST_STATE arriving mid-hold
// would do -- not the bag's free space this time, but what the chest itself reports holding.
// Three stacks at the start (fits easily); a full eight by the time breakComplete() asks again
// (does not). The bag's own capacity never moves in this case, which is the point: it is the
// SOURCE data that changed under the hold, and the late re-check is what catches that too.
static void testAChestBreakIsRefusedWhenItsContentsChangeDuringTheHold(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(true);
	// A fixed capacity of 5 for both calls -- what changes is n, not the ceiling it is judged
	// against.
	s_fits_capacity_early  = 5;
	s_fits_capacity_late   = 5;
	// Call 1 (hold start): 3 stacks, n == 1 + 3 == 4, fits (4 <= 5).
	s_spy_chest_n = 3;
	// Call 2 onward (the late re-check): a remote CHEST_STATE filled every slot, n == 1 + 8 ==
	// 9, no longer fits (9 <= 5 is false).
	s_spy_chest_n_late          = CHEST_SLOTS;
	s_spy_chest_flip_after_call = 2;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	// Same reasoning as the case above for driving exactly `need` frames rather than
	// holdBreakUntilDone()'s NEVER_TICKS -- see the comment there.
	const uint32_t need = breakTicksRequired(BLOCK_CHEST, it.holding);
	for (uint32_t t = 0; t < need; t++)
		holdBreakFrame(&it, &body, 1);

	// Same shape of proof as the case above: the world, not a counter.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CHEST);
	CHECK(it.broke == 0);
	CHECK(it.broke_id == BLOCK_AIR);
	CHECK(it.broke_valid == false);
	CHECK(it.broke_extra_count == 0);

	// Conservation: the now-eight-stack chest is still sitting in the world with everything
	// still in it -- none of the eight stacks the late CHEST_STATE reported were ever added to
	// broke_extra_item/qty, because the break never landed to read them for payout.
	CHECK(s_edits_sent == 0);
	CHECK(it.refused == 1);

	// What the late call was actually asked, confirming the refusal is about the NEW contents
	// and not a stale copy of the old ones.
	CHECK(s_fits_last_n == 1 + CHEST_SLOTS);

	CHECK(s_fits_calls == 2);   // the tripwire, kept, not the only guard

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// An EMPTY chest with a full bag is still refused, because the chest itself is an item too.
// Worth its own case: the obvious wrong implementation checks only the contents, and an empty
// chest has none.
static void testAnEmptyChestIsStillRefusedByAFullBag(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	resetFitsSpy(false);
	s_spy_chest_n = 0;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == -1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CHEST);
	CHECK(it.refused == 1);

	// One entry, the chest itself -- so "the block being broken" is always in the list, not
	// only when it has contents.
	CHECK(s_fits_last_n == 1);
	CHECK(s_fits_last_items[0] == BLOCK_CHEST && s_fits_last_counts[0] == 1);

	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(NULL);
}

// A chest with no reader registered at all -- the state the console is in until main.c wires
// one -- still asks the capacity question about the chest itself. The two callbacks are
// independent; neither being absent may disable the other.
static void testAChestWithNoContentsReaderStillAsksAboutItself(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetFitsSpy(false);
	interactSetBrokeContentsFn(NULL);
	interactSetBagFitsFn(bagFitsSpy);

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) == -1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_CHEST);
	CHECK(s_fits_last_n == 1);
	CHECK(s_fits_last_items[0] == BLOCK_CHEST);

	interactSetBagFitsFn(NULL);
}

// The default, unwired state: no predicate registered means "yes, always", i.e. exactly the
// pre-v1.9.0 behaviour. Every one of the fifty-odd cases above this section depends on that
// being true, so it is asserted rather than left implicit.
static void testNoBagPredicateMeansAChestBreaksAsBefore(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_CHEST);
	resetSpy();
	s_spy_chest_n = CHEST_SLOTS;
	interactSetBrokeContentsFn(brokeContentsSpy);
	interactSetBagFitsFn(NULL);   // explicit, though freshAimedAt already did this

	const Body body = farAwayBody();
	CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	CHECK(it.broke_id == BLOCK_CHEST);
	CHECK(it.broke_extra_count == CHEST_SLOTS);

	interactSetBrokeContentsFn(NULL);
}

// The non-chest path is UNCHANGED, and this is the case that proves the new branch is gated
// rather than merely ordered: with the bag answering "nothing fits" for everything, stone and
// a furnace both still break, and the predicate is never even consulted for them.
//
// The furnace half is deliberate scope, not an oversight -- it has the identical problem and
// is already shipping with it, and docs/design-1.9.0-chest-multiplayer.md's open question 3
// puts furnaces out of scope for this version. If that is ever taken on, this case is the one
// that goes red and says so.
static void testANonChestBreakNeverAsksAboutCapacity(void)
{
	const BlockId ids[] = { BLOCK_STONE, BLOCK_DIRT, BLOCK_FURNACE, BLOCK_TALL_GRASS };

	for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
		Interact it;
		freshAimedAt(&it, ids[i]);
		resetSpy();
		resetFitsSpy(false);   // the bag is full and refuses everything
		interactSetBrokeContentsFn(brokeContentsSpy);
		interactSetBagFitsFn(bagFitsSpy);

		const Body body = farAwayBody();
		CHECK(holdBreakUntilDone(&it, &body, NEVER_TICKS) > 0);
		CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
		CHECK(it.refused == 0);
		CHECK(s_fits_calls == 0);

		interactSetBrokeContentsFn(NULL);
		interactSetBagFitsFn(NULL);
	}
}

// ── v1.9.0 F1: a refused placement must put back what was really there ──────────────────
//
// THE DEFECT, and it is a REFUSAL path, which is the only reason it is worth three cases for
// something a player meets once in a blue moon: v1.9.0 made a placement refuse when
// blockStateCreate() answers false -- the side table holds 256 live records and none of them
// is this cell -- and source/main.c undoes the placement by writing the cell back. It wrote
// BLOCK_AIR, because interactEdit kept the cell's previous contents in a LOCAL (`place_was`)
// and never published them. main.c's own comment said so and called the consequence bounded:
//
//     WHAT THE CELL GOES BACK TO IS BLOCK_AIR, AND THAT IS NOT EXACTLY RIGHT. [...] placing
//     a chest into a tuft of grass with the state table full destroys the grass. Fixing it
//     properly needs a `placed_was` field beside placed_id in scene/interact.h, which this
//     change does not own.
//
// So a refusal whose whole contract is "nothing happened" DELETED A BLOCK. Air is only the
// ordinary case: the place path refuses a solid cell and accepts every non-solid one, and
// tall grass is a non-solid cell a player walks through constantly.
//
// WHY THESE CASES POISON THE FIELD FIRST. `placed_was` is read by the caller AFTER
// interactEdit returns, and the claim under test is that the call PUBLISHES it -- not that it
// happened to hold the right value already. Writing a deliberate wrong value in first is what
// makes that distinguishable, and it is also what makes the red arm DETERMINISTIC: against an
// interact.c that never writes the field there is nothing else to read, and an indeterminate
// stack byte is not evidence about anything. BLOCK_STONE is the poison because it is neither
// the block being placed, nor what the cell held, nor BLOCK_AIR.
//
// The revert these cases perform is main.c's own line, not a paraphrase of it: worldSet on the
// place cell with it.placed_was, guarded by it.placed_valid exactly as main.c's `if` guards it.
// The four other lines of that revert (dirty, relight, remesh, the wire) are main.c's business
// and are not what this file is about -- what IS this file's business is that the id main.c
// puts back is the right one.
static void testAPlacementIntoTallGrassPublishesTheGrass(void)
{
	// The premise, pinned rather than assumed: a placement is only allowed into a NON-solid
	// cell (interact.c refuses `blockIsSolid(place_was)`), so if tall grass ever became solid
	// this whole case would be testing the refusal above instead, and would pass for the wrong
	// reason.
	CHECK(!blockIsSolid(BLOCK_TALL_GRASS));

	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;

	// The place cell is not empty. This is the whole scenario: a tuft of grass where the
	// player is about to put a block.
	worldSet(&s_world, TX, TY + 1, TZ, BLOCK_TALL_GRASS);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_TALL_GRASS);

	it.placed_was = BLOCK_STONE;   // poison — see the section note above

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

	// The placement itself landed, exactly as it always did. If any of these three go red the
	// case is no longer about the revert at all.
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);
	CHECK(it.placed_id == BLOCK_PLANKS);
	CHECK(it.placed_valid == true);

	// THE published field. This is the check that is red without the fix.
	CHECK(it.placed_was == BLOCK_TALL_GRASS);

	// And main.c's refusal revert, run for real over the world this file owns. Without the fix
	// it puts back the poison; before the fix existed at all it put back BLOCK_AIR. Either way
	// the grass is gone, and this is what says so.
	if (it.placed_valid)
		worldSet(&s_world, it.placed_x, it.placed_y, it.placed_z, it.placed_was);

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_TALL_GRASS);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) != BLOCK_AIR);       // the defect, stated directly
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) != BLOCK_PLANKS);    // and the placement is gone

	// The block that was under the crosshair is untouched throughout — the revert names the
	// PLACE cell and nothing else.
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
}

// The ordinary case, which must not move: an empty place cell publishes BLOCK_AIR, so the
// revert writes exactly what main.c's old hardcoded BLOCK_AIR wrote. Without this, a
// `placed_was` that was only ever set for non-air cells would pass the case above and quietly
// change what a normal refused placement does.
static void testAnOrdinaryPlacementIntoAirPublishesAir(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);

	it.placed_was = BLOCK_STONE;   // poison, same reason

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);

	CHECK(it.placed_valid == true);
	CHECK(it.placed_was == BLOCK_AIR);

	if (it.placed_valid)
		worldSet(&s_world, it.placed_x, it.placed_y, it.placed_z, it.placed_was);

	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_AIR);
}

// The STALE case, and it is the one that would turn this fix into a worse bug than the one it
// closes. main.c reads placed_was on a later frame; a value left standing from an EARLIER
// placement would revert this cell to a block that was never in it — a block conjured out of
// somewhere else in the world, which is strictly worse than the air it used to write.
//
// So placed_was is cleared at the top of every interactEdit, on placed_id's own schedule, and
// this is what holds that. The second frame here is a plain release (no keys), which is the
// cheapest call that reaches the top of interactEdit and gets nowhere near the place path.
static void testPlacedWasDoesNotSurviveIntoALaterCall(void)
{
	Interact it;
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_PLANKS;
	worldSet(&s_world, TX, TY + 1, TZ, BLOCK_TALL_GRASS);

	it.placed_was = BLOCK_STONE;   // poison, same reason as the two cases above

	const Body body = farAwayBody();
	interactEdit(&it, &s_world, &body, placeKey(), 0, 0);
	CHECK(it.placed_was == BLOCK_TALL_GRASS);   // the value that must not persist

	releaseFrame(&it, &body, 1);

	// Cleared, and cleared TOGETHER with the flag that gates it. A reader doing main.c's job on
	// this frame finds nothing to revert and, if it looked anyway, would find BLOCK_AIR — the
	// value the field meant before it existed.
	CHECK(it.placed_valid == false);
	CHECK(it.placed_was == BLOCK_AIR);

	// And the world was not touched by the release frame.
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_PLANKS);
}

// ── v1.9.0 F1, the main.c half: the revert really reads placed_was, and the charge waits ──
//
// The three cases above prove interact.c PUBLISHES the field. They cannot prove main.c READS
// it: source/main.c opens with <3ds.h> and carries main(), so it links into no host binary,
// and the revert those cases perform is a copy of main.c's line, not main.c's line. Put
// BLOCK_AIR back into main.c's worldSet -- the exact pre-fix defect -- and every check above
// stays green. Measured, not reasoned: that sabotage was run against this binary before this
// pin existed and the run was "PASS 1252 checks".
//
// So the wiring is pinned the way this suite already pins main.c elsewhere
// (world/framesync_guard_test.c, app/session_test.c, world/playerpose_test.c): read the file
// as text and assert the exact lines are there. Whitespace is collapsed to single spaces
// over the WHOLE file before searching, so a call that spans two lines (the
// networldSendBlockEdit below does) is one searchable string and re-indenting main.c cannot
// turn a pin red. Comments in main.c quote fragments of these lines but never a whole call
// with its arguments, which is why every needle below is a complete statement.
//
// The path is relative to the cwd because the binary runs from the repo root, as
// tools/run_host_tests.sh runs every other main.c pin. A file that cannot be opened is a
// FAILURE, never "no lines, therefore nothing to complain about".
static char* readMainCSquashed(void)
{
	FILE* f = fopen("source/main.c", "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	const long len = ftell(f);
	if (len < 0) { fclose(f); return NULL; }
	rewind(f);

	char* buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }
	const size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);

	// Collapse every run of whitespace to one space, in place.
	size_t o  = 0;
	bool   sp = false;
	for (size_t i = 0; i < got; i++) {
		const char c = buf[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { sp = (o > 0); continue; }
		if (sp) { buf[o++] = ' '; sp = false; }
		buf[o++] = c;
	}
	buf[o] = '\0';
	return buf;
}

static int countIn(const char* hay, const char* needle)
{
	int n = 0;
	for (const char* p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
		n++;
	return n;
}

static void testMainCRevertsTheRefusedPlacementToPlacedWas(void)
{
	char* src = readMainCSquashed();
	CHECK(src != NULL);
	if (!src) return;

	// THE line. Exactly one worldSet names the placed cell, and it writes placed_was -- the
	// second count is what refuses ANY other id there, BLOCK_AIR included.
	CHECK(countIn(src, "worldSet(&s_world, it.placed_x, it.placed_y, it.placed_z, it.placed_was);") == 1);
	CHECK(countIn(src, "worldSet(&s_world, it.placed_x, it.placed_y, it.placed_z,") == 1);

	// The pre-fix defect, stated directly.
	CHECK(countIn(src, "worldSet(&s_world, it.placed_x, it.placed_y, it.placed_z, BLOCK_AIR)") == 0);

	// And the server is told the SAME value, or the revert trades one divergence for another.
	CHECK(countIn(src, "networldSendBlockEdit(it.placed_x, it.placed_y, it.placed_z, (uint8_t)it.placed_was);") == 1);
	CHECK(countIn(src, "networldSendBlockEdit(it.placed_x, it.placed_y, it.placed_z,") == 1);

	// ORDER: the refusal clears placed_id BEFORE the hotbar charge reads it, which is what
	// keeps a refused placement from costing the player the block. Both statements exist
	// exactly once, and the clear comes first in the file.
	const char* clear  = strstr(src, "it.placed_id = BLOCK_AIR;");
	const char* charge = strstr(src, "if (it.placed_id != BLOCK_AIR) invBridgeRemove(&s_inv, it.placed_id, 1);");
	CHECK(countIn(src, "it.placed_id = BLOCK_AIR;") == 1);
	CHECK(countIn(src, "if (it.placed_id != BLOCK_AIR) invBridgeRemove(&s_inv, it.placed_id, 1);") == 1);
	CHECK(clear != NULL && charge != NULL && clear < charge);

	free(src);
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

	// v1.8.10 "Light" — the torch, end to end through interactEdit().
	testPlacingATorchLightsUpTheWorldAndBreakingItDarkensItAgain();

	// v1.8.16 — food is an item, not a block. The reported instance first, then the rule, then
	// the two halves that must NOT move: breaking food still works, and every other core block
	// still places.
	testThePlaceableRuleItself();
	testAnAppleCannotBePlacedAsABlock();
	testEveryFoodItemIsRefusedByThePlacePath();
	testFoodStillBreaksAndStillReachesTheBag();
	testEveryNonFoodCoreBlockStillPlaces();

	// v1.8.16 F1 — a broken furnace must not silently destroy its contents.
	testBreakingAFurnaceReportsItsContents();
	testBreakingAnOrdinaryBlockReportsNoExtraContents();
	testNoReaderRegisteredMeansNoExtraContentsForAFurnace();
	testAnEmptySlotIsSkippedNotLeftAsAHole();
	testBrokenFurnaceClampsAnOverclaimingReader();
	testBrokenFurnaceClampsANegativeReader();
	testBrokeExtraCountClearsOnTheNextCall();

	// v1.9.0 "Storage" — a chest break must not destroy what is inside it. The ceiling pin
	// first, then the allowed cases, then THE refusal, then the two halves that must not move:
	// an unwired predicate behaves as before, and no non-chest block is affected.
	testTheExtraContentsCeilingCoversAWholeChest();
	testAChestBreaksWhenItsContentsFit();
	testAFullChestOffersEveryOneOfItsSlots();
	testAChestBreakIsRefusedWhenItsContentsWillNotFit();
	// v1.9.0 F2 -- the late re-check itself, not just testAChestBreaksWhenItsContentsFit's
	// s_fits_calls == 2 call-count pin. See the section comment above the first of these two.
	testAChestBreakIsRefusedWhenTheBagFillsDuringTheHold();
	testAChestBreakIsRefusedWhenItsContentsChangeDuringTheHold();
	testAnEmptyChestIsStillRefusedByAFullBag();
	testAChestWithNoContentsReaderStillAsksAboutItself();
	testNoBagPredicateMeansAChestBreaksAsBefore();
	testANonChestBreakNeverAsksAboutCapacity();

	// v1.9.0 F1 — a refused placement puts back what was really in the cell. THE case first,
	// then the ordinary air case that must not move, then the stale-value guard.
	testAPlacementIntoTallGrassPublishesTheGrass();
	testAnOrdinaryPlacementIntoAirPublishesAir();
	testPlacedWasDoesNotSurviveIntoALaterCall();
	// v1.9.0 F1, the main.c half -- pinned as text, because main.c links into no host binary
	// and a BLOCK_AIR put back into its revert leaves the three cases above green.
	testMainCRevertsTheRefusedPlacementToPlacedWas();

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
