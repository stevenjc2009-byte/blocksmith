// Host tests for the entity foundation (entity/entity.c, v1.8.8) and for the additive AABB
// parameterisation of world/physics.c that it required.
//
// WHAT THIS FILE IS DEFENDING, and why each half of it is here.
//
// The entity store is new code with no shipped behaviour to regress, so the interesting
// risk is not "did it change" but "was it ever right". Five things can be wrong in a fixed
// pool and every one of them is a crash or a leak on a handheld with no MMU:
//
//   1. A slot issued twice, so two entities alias one struct.
//   2. A stale handle resolving to whatever took its slot -- the classic use-after-free
//      that a fixed pool is supposed to make impossible and only does if the id is cleared.
//   3. Occupancy running past the cap, so the count and the scan disagree.
//   4. An entity surviving in a column that has been freed, whose next physics step reads
//      terrain that is gone.
//   5. The tick schedule silently degrading to "everything at full rate", which is not a
//      crash, is invisible, and is the entire CPU saving.
//
// The OTHER half is a regression guard, and it is the more dangerous half. physics.c is
// shipped, tested and load-bearing: scene/player.c and world_test.c's 68 body call sites
// all go through it. Parameterising the AABB touched bodyBlocked, resolveY, resolveX,
// resolveZ, tryStepUp, trySwimUp and wetAt -- seven functions -- and the claim being made
// is that the PLAYER's behaviour is bit-identical afterwards. testPlayerBoxIsUnchanged and
// testDefaultBodyMatchesPlayerConstants exist to make that claim falsifiable rather than
// asserted, and the red arms in the run log show them going red against a deliberately
// wrong default.
//
// Bit-identical, not approximately equal: every position comparison in the player-parity
// tests is `==` on float, never an epsilon. An epsilon there would pass against a box that
// was a millimetre wrong, which is exactly the change the test is supposed to catch.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "entity/entity.h"
#include "world/block.h"
#include "world/physics.h"
#include "world/tick.h"
#include "world/world.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char* what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   %s\n", what);
	}
}

// How many check() calls this suite makes on a healthy tree. A LITERAL, for the reason
// world/tick_test.c states at length: a check that never RUNS is indistinguishable from a
// check that passes, and an early return or a mis-placed continue removes checks silently
// while the suite still reports "0 failed". Editing this by hand is the cost of the guard.
#define ENTITY_TEST_EXPECTED_CHECKS 146

// Deliberately NOT routed through check(), so it cannot perturb the number it is testing.
static void checkCountPin(void)
{
	if (g_checks == ENTITY_TEST_EXPECTED_CHECKS) return;

	g_fails++;
	printf("  FAIL   CHECK COUNT: expected %d checks, ran %d. A check was added, deleted,\n"
	       "         or skipped by an early return -- the pass/fail line below cannot be\n"
	       "         trusted until this agrees.\n",
	       ENTITY_TEST_EXPECTED_CHECKS, g_checks);
}

static World s_world;

// A patch of loaded, EMPTY columns. Nothing is written into them: world.h's coordinate
// convention says anything below the floor reads as WORLD_FLOOR_BLOCK, so a body dropped
// into an empty loaded column falls to exactly y = 0 and rests on the world floor. That is
// a real solid surface as far as bodyBlocked is concerned and it needs no block writes at
// all, which keeps the fixture from depending on worldgen or on the registry's contents.
static void makeGround(World* w, int cx0, int cz0, int cx1, int cz1)
{
	worldInit(w);
	for (int cx = cx0; cx <= cx1; cx++)
		for (int cz = cz0; cz <= cz1; cz++)
			worldColumnCreate(w, cx, cz);
}

// ---------------------------------------------------------------------------------------
// The control. Runs first, so a broken fixture is distinguishable from a broken subject.
// ---------------------------------------------------------------------------------------

static void testFixtureAndFreshStore(void)
{
	makeGround(&s_world, -1, -1, 1, 1);
	check(worldColumn(&s_world, 0, 0) != NULL, "control: the fixture loads column (0, 0)");
	check(worldColumn(&s_world, 9, 9) == NULL, "control: a column nobody made is not loaded");
	check(bodyBlocked(&s_world, 8.0f, -1.0f, 8.0f),
	      "control: the world floor is solid, so the fixture can stop a falling body");
	check(!bodyBlocked(&s_world, 8.0f, 4.0f, 8.0f),
	      "control: open air above the floor is not solid");

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	check(entityCount(&ew) == 0,             "a fresh store holds nothing");
	check(entityCap(&ew) == ENTITY_CAP_OLD,  "a fresh store keeps the cap it was given");
	check(entityGet(&ew, 0) == NULL,         "every slot in a fresh store reads as free");
	check(entityFindById(&ew, 1) == -1,      "no id resolves in a fresh store");

	worldExit(&s_world);
}

static void testConstantsPinned(void)
{
	check(ENTITY_SLOTS   == 48, "ENTITY_SLOTS is 48");
	check(ENTITY_CAP_OLD == 24, "ENTITY_CAP_OLD is 24");
	check(ENTITY_CAP_NEW == 48, "ENTITY_CAP_NEW is 48");
	check(entityCapFor(false) == ENTITY_CAP_OLD, "entityCapFor(Old 3DS) is the Old cap");
	check(entityCapFor(true)  == ENTITY_CAP_NEW, "entityCapFor(New 3DS) is the New cap");

	// The tick period the entity scheduler is built on. Pinned HERE as well as in
	// tick_test.c on purpose: this suite's decimation checks are only meaningful if the
	// period really is 10, and a reader of this file should not have to open another one
	// to find that out.
	check(TICK_FAR_PERIOD == 10, "TICK_FAR_PERIOD is 10, so the far tier is 2 Hz");
	check(TICK_NEAR_BLOCKS == 24, "TICK_NEAR_BLOCKS is 24");
	check(ENTITY_DT > 0.049f && ENTITY_DT < 0.051f, "ENTITY_DT is one 20 Hz tick, 0.05 s");
}

// ---------------------------------------------------------------------------------------
// Storage: spawn, despawn, id stability, exhaustion
// ---------------------------------------------------------------------------------------

static void testSpawnAndDespawn(void)
{
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);

	const int a = entitySpawn(&ew, 1, 10.0f, 5.0f, 20.0f, 0.0f, 0.0f, 0.0f);
	check(a >= 0,                  "a spawn into an empty store succeeds");
	check(entityCount(&ew) == 1,   "the count follows the spawn");

	const Entity* e = entityGet(&ew, a);
	check(e != NULL,               "the spawned slot reads back");
	check(e->kind == 1,            "the kind is stored verbatim");
	check(e->id != 0,              "a live entity never carries the reserved id 0");
	check(e->health == 20,         "a fresh entity starts at full health on the 0..20 scale");
	check(e->flags == 0,           "a fresh entity carries no flags");
	check(e->body.x == 10.0f && e->body.y == 5.0f && e->body.z == 20.0f,
	      "the spawn position lands in the embedded Body, exactly");
	check(e->body.vx == 0.0f && e->body.vy == 0.0f && e->body.vz == 0.0f,
	      "a fresh entity is at rest");

	check(entityDespawn(&ew, a),      "despawning a live slot succeeds");
	check(entityCount(&ew) == 0,      "the count follows the despawn");
	check(entityGet(&ew, a) == NULL,  "the freed slot reads as free");
	check(!entityDespawn(&ew, a),     "despawning an already-free slot is refused, not repeated");

	// The boundary cases a fixed pool gets wrong. Out of range in both directions and the
	// sentinel kind, all refused rather than stored.
	check(entitySpawn(&ew, ENT_NONE, 0, 0, 0, 0, 0, 0) == -1,
	      "spawning kind ENT_NONE is refused -- it would be a slot free the instant it is made");
	check(entityGet(&ew, -1) == NULL,            "a negative slot index reads NULL");
	check(entityGet(&ew, ENTITY_SLOTS) == NULL,  "one past the last slot reads NULL");
	check(entityAt(&ew, ENTITY_SLOTS) == NULL,   "entityAt bounds-checks the same way");
	check(!entityDespawn(&ew, -1),               "a negative slot cannot be despawned");
	check(!entityDespawn(&ew, ENTITY_SLOTS),     "one past the last slot cannot be despawned");
	check(entityCount(&ew) == 0,                 "none of the refusals moved the count");
}

// The use-after-free test, and the reason entityDespawn clears the id.
static void testIdStabilityAcrossDespawn(void)
{
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);

	const int a = entitySpawn(&ew, 1, 0, 0, 0, 0, 0, 0);
	const uint16_t id_a = entityGet(&ew, a)->id;
	check(entityFindById(&ew, id_a) == a, "a live id resolves to its slot");

	entityDespawn(&ew, a);
	check(entityFindById(&ew, id_a) == -1, "a despawned id resolves to nothing");

	// Read off the RAW slot, not through entityFindById. Measured: a red arm that deleted
	// the id clear in entityDespawn left every other check in this file green, because
	// entityFindById skips free slots and so can never see the stale value. A check routed
	// through the accessor is a check that cannot go red for this, which is the same thing
	// as not having one.
	check(ew.e[a].id == 0, "a despawned slot's raw id field is cleared to the reserved 0");

	// The slot is reused -- that is the whole point of a pool -- and the OLD handle must
	// not follow it. This is the check that turns "a slot index is not an identity" from a
	// comment into a property.
	const int b = entitySpawn(&ew, 2, 0, 0, 0, 0, 0, 0);
	check(b == a, "the freed slot is reused, as a pool should");
	const uint16_t id_b = entityGet(&ew, b)->id;
	check(id_b != id_a, "the reused slot carries a NEW id, not the dead one");
	check(entityFindById(&ew, id_a) == -1,
	      "the dead handle STILL resolves to nothing after its slot was reused");
	check(entityFindById(&ew, id_b) == b, "the new handle resolves to the reused slot");

	check(entityFindById(&ew, 0) == -1, "id 0 never resolves -- it is the reserved value");

	// Every id live at one time is distinct. This is what makes entityFindById an answer
	// rather than a guess, and it is the property the issueId collision scan defends.
	EntityWorld full;
	entityWorldInit(&full, ENTITY_CAP_NEW);
	uint16_t ids[ENTITY_SLOTS];
	int n = 0;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const int s = entitySpawn(&full, 3, 0, 0, 0, 0, 0, 0);
		if (s < 0) break;
		ids[n++] = entityGet(&full, s)->id;
	}
	check(n == ENTITY_SLOTS, "a New-3DS-capped store fills every one of its 48 slots");

	int dupes = 0;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (ids[i] == ids[j]) dupes++;
	check(dupes == 0, "no two simultaneously-live entities share an id");

	int found_all = 1;
	for (int i = 0; i < n; i++)
		if (entityFindById(&full, ids[i]) < 0) found_all = 0;
	check(found_all == 1, "every live id resolves to a slot");
}

static void testPoolExhaustion(void)
{
	// The Old 3DS cap in a 48-slot pool: the limit is the CAP, not the array, so 24 of the
	// 48 slots are still free when spawning starts being refused.
	EntityWorld old;
	entityWorldInit(&old, ENTITY_CAP_OLD);
	int spawned = 0;
	for (int i = 0; i < ENTITY_SLOTS + 8; i++)
		if (entitySpawn(&old, 1, 0, 0, 0, 0, 0, 0) >= 0) spawned++;

	check(spawned == ENTITY_CAP_OLD,
	      "an Old-3DS store accepts exactly ENTITY_CAP_OLD spawns and then refuses");
	check(entityCount(&old) == ENTITY_CAP_OLD, "the count stops at the cap");
	check(entitySpawn(&old, 1, 0, 0, 0, 0, 0, 0) == -1, "a capped store refuses further spawns");

	int occupied = 0;
	for (int i = 0; i < ENTITY_SLOTS; i++)
		if (entityGet(&old, i) != NULL) occupied++;
	check(occupied == ENTITY_CAP_OLD,
	      "the SCAN agrees with the count -- no slot was issued twice");

	// Room reappears the moment one is freed, and not before.
	entityDespawn(&old, 0);
	check(entityCount(&old) == ENTITY_CAP_OLD - 1, "freeing one drops the count by one");
	check(entitySpawn(&old, 1, 0, 0, 0, 0, 0, 0) >= 0, "and makes exactly one spawn possible");
	check(entitySpawn(&old, 1, 0, 0, 0, 0, 0, 0) == -1, "and only one");

	// A cap above the array is clamped rather than believed, or the scan and the count
	// would diverge at slot 48.
	EntityWorld silly;
	entityWorldInit(&silly, 1000);
	check(entityCap(&silly) == ENTITY_SLOTS, "a cap above the pool size is clamped to it");
	int silly_n = 0;
	while (entitySpawn(&silly, 1, 0, 0, 0, 0, 0, 0) >= 0) silly_n++;
	check(silly_n == ENTITY_SLOTS, "and the store then holds exactly the pool size");

	EntityWorld none;
	entityWorldInit(&none, -5);
	check(entityCap(&none) == 0, "a negative cap is clamped to zero, not to the pool size");
	check(entitySpawn(&none, 1, 0, 0, 0, 0, 0, 0) == -1, "a zero-cap store spawns nothing");
}

// ---------------------------------------------------------------------------------------
// Physics: the entity moves through bodyMove, with its own box
// ---------------------------------------------------------------------------------------

static void testEntityFallsAndLands(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	const int s = entitySpawn(&ew, 1, 8.0f, 20.0f, 8.0f, 0.6f, 0.9f, 0.8f);
	check(s >= 0, "an entity spawns over the fixture's ground");

	for (uint64_t t = 0; t < 200; t++)
		entityTick(&ew, &s_world, t, 8.0f, 8.0f, NULL, NULL);

	const Entity* e = entityGet(&ew, s);
	check(e != NULL, "the entity survives 200 ticks over loaded ground");
	check(e->body.on_ground, "it ends up on the ground");
	check(e->body.y == 0.0f,
	      "it rests EXACTLY on the world floor at y = 0 -- an analytic snap, not a near miss");
	check(e->body.vy == 0.0f, "and its vertical velocity is zeroed by the landing");

	worldExit(&s_world);
}

static void testEntityIsStoppedByAWall(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	// A wall two blocks tall at x = 12, so it cannot be auto-stepped.
	for (int y = 0; y < 2; y++)
		for (int z = 6; z <= 10; z++)
			worldSet(&s_world, 12, y, z, BLOCK_STONE);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	const int s = entitySpawn(&ew, 1, 8.0f, 2.0f, 8.0f, 0.6f, 0.9f, 0.8f);
	Entity* e = entityAt(&ew, s);
	check(e != NULL, "the walker spawns");

	for (uint64_t t = 0; t < 400; t++) {
		e = entityAt(&ew, s);
		if (!e) break;
		e->body.vx = 4.0f;   // driven straight at the wall, every tick
		entityTick(&ew, &s_world, t, 8.0f, 8.0f, NULL, NULL);
	}

	e = entityAt(&ew, s);
	check(e != NULL, "the walker is still alive after 400 ticks of walking into a wall");
	check(e->body.x < 12.0f,
	      "it never passes through the wall -- bodyMove's substepping caught it");
	check(e->body.x > 11.0f,
	      "and it did reach the wall rather than being stopped by something else");

	worldExit(&s_world);
}

// The reason the AABB was parameterised at all: a SHORT entity fits under a one-block
// overhang that a player-height body does not. If this passes with both boxes, the box is
// not actually being used and the whole change is decoration.
static void testEntityBoxIsActuallyUsed(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	// A ceiling slab at y = 1, leaving exactly one block of headroom above the floor.
	for (int x = 6; x <= 10; x++)
		for (int z = 6; z <= 10; z++)
			worldSet(&s_world, x, 1, z, BLOCK_STONE);

	const float x = 8.5f, y = 0.0f, z = 8.5f;

	check(bodyBlockedBox(&s_world, x, y, z, 0.3f, 1.8f),
	      "a 1.8-tall box does NOT fit in one block of headroom");
	check(!bodyBlockedBox(&s_world, x, y, z, 0.3f, 0.9f),
	      "a 0.9-tall box DOES fit in the same gap -- the height argument is load-bearing");
	check(bodyBlocked(&s_world, x, y, z),
	      "and bodyBlocked, which is the PLAYER box, agrees with the 1.8 case");

	// The same distinction through the entity path rather than the raw predicate: a short
	// entity walks under the slab, a player-boxed one is stopped by it.
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	const int shorty = entitySpawn(&ew, 1, 4.0f, 0.0f, 8.5f, 0.6f, 0.9f, 0.8f);
	const int tall   = entitySpawn(&ew, 2, 4.0f, 0.0f, 8.5f, 0.6f, 1.8f, 0.9f);
	check(shorty >= 0 && tall >= 0, "both a short and a tall entity spawn west of the slab");

	// 60 ticks, not 200: at 3 blocks/s a 200-tick walk covers 30 blocks and leaves the
	// three-by-three patch of loaded columns, at which point entityTick correctly despawns
	// both and the test is measuring its own fixture running out rather than the boxes.
	for (uint64_t t = 0; t < 60; t++) {
		Entity* a = entityAt(&ew, shorty);
		Entity* b = entityAt(&ew, tall);
		if (a) a->body.vx = 3.0f;
		if (b) b->body.vx = 3.0f;
		entityTick(&ew, &s_world, t, 4.0f, 8.5f, NULL, NULL);
	}

	const Entity* a = entityGet(&ew, shorty);
	const Entity* b = entityGet(&ew, tall);
	check(a != NULL && b != NULL, "both survive the walk");
	check(a->body.x > 8.0f, "the SHORT entity walks under the overhang");
	check(b->body.x < 6.0f,
	      "the TALL entity is stopped short by it -- the two boxes really do differ");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// The player is unchanged. This is the regression half.
// ---------------------------------------------------------------------------------------

static void testDefaultBodyMatchesPlayerConstants(void)
{
	Body b;
	bodyInit(&b, 1.0f, 2.0f, 3.0f);

	// `==` on float, deliberately. An epsilon here would accept a box that is a millimetre
	// wrong, which is precisely the change this check exists to refuse.
	check(b.half_w == PLAYER_WIDTH * 0.5f,
	      "bodyInit defaults half_w to exactly half PLAYER_WIDTH");
	check(b.height == PLAYER_HEIGHT, "bodyInit defaults height to exactly PLAYER_HEIGHT");
	check(b.eye    == PLAYER_EYE,    "bodyInit defaults eye to exactly PLAYER_EYE");

	// bodySetBox refuses nonsense rather than storing it, so a partially-filled argument
	// list leaves the player's value in place instead of a zero-size box that collides with
	// nothing.
	bodySetBox(&b, 0.0f, 0.0f, 0.0f);
	check(b.half_w == PLAYER_WIDTH * 0.5f && b.height == PLAYER_HEIGHT && b.eye == PLAYER_EYE,
	      "bodySetBox with all-zero arguments changes nothing");
	bodySetBox(&b, -1.0f, -1.0f, -1.0f);
	check(b.half_w == PLAYER_WIDTH * 0.5f && b.height == PLAYER_HEIGHT && b.eye == PLAYER_EYE,
	      "bodySetBox with negative arguments changes nothing");

	bodySetBox(&b, 0.8f, 1.0f, 0.5f);
	check(b.half_w == 0.4f && b.height == 1.0f && b.eye == 0.5f,
	      "bodySetBox stores width/2, height, and eye as a fraction of height");
	bodySetBox(&b, 0.0f, 2.0f, 0.0f);
	check(b.half_w == 0.4f && b.height == 2.0f,
	      "a zero width leaves the previous width alone while the height still updates");
}

// The parity run. A default-constructed Body is stepped through a long, varied trajectory
// -- fall, land, walk into a wall, auto-step, jump -- and every position is compared
// against a body whose box was explicitly SET to the player's numbers. If any of the seven
// functions that were touched reads the wrong extent, the two diverge.
//
// The point of doing it this way rather than against hard-coded expected positions: a table
// of expected floats would have to be regenerated from the new code, which proves nothing.
// Two bodies that must agree, one of which reaches the box through the default path and one
// through the setter, is a comparison neither side can quietly move.
static void testPlayerBoxIsUnchanged(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	// A one-block step at x = 11 that continues as a plateau to x = 13, so the body is
	// still standing on it when it reaches the wall. A single column at x = 11 would be
	// stepped onto and then walked straight off again, and the "finished on top of the
	// step" check below would be measuring a fall rather than a climb.
	for (int x = 11; x <= 13; x++)
		for (int z = 4; z <= 12; z++)
			worldSet(&s_world, x, 0, z, BLOCK_STONE);
	for (int y = 0; y < 3; y++)
		for (int z = 4; z <= 12; z++)
			worldSet(&s_world, 14, y, z, BLOCK_STONE);       // a wall it cannot climb

	Body def, set;
	bodyInit(&def, 6.0f, 6.0f, 8.0f);
	bodyInit(&set, 6.0f, 6.0f, 8.0f);
	bodySetBox(&set, PLAYER_WIDTH, PLAYER_HEIGHT, PLAYER_EYE / PLAYER_HEIGHT);

	int diverged = 0;
	int landed = 0, stepped_up = 0, blocked_at_wall = 0;
	float peak_x = 6.0f;

	for (int t = 0; t < 600; t++) {
		def.vx = 4.3f; set.vx = 4.3f;
		if (t == 300) { bodyJump(&def, def.wet, true, true, 1.0f / 60.0f);
		                bodyJump(&set, set.wet, true, true, 1.0f / 60.0f); }

		bodyStep(&def, &s_world, 1.0f / 60.0f);
		bodyStep(&set, &s_world, 1.0f / 60.0f);

		if (def.x != set.x || def.y != set.y || def.z != set.z ||
		    def.vx != set.vx || def.vy != set.vy || def.vz != set.vz ||
		    def.on_ground != set.on_ground || def.wet != set.wet)
			diverged++;

		if (def.on_ground) landed = 1;
		if (def.y >= 1.0f && def.on_ground) stepped_up = 1;
		if (def.x > peak_x) peak_x = def.x;
	}
	if (peak_x < 14.0f) blocked_at_wall = 1;

	check(diverged == 0,
	      "600 steps: the default box and an explicitly-set player box stay bit-identical");

	// The trajectory has to actually EXERCISE the touched functions, or "identical" is a
	// statement about two bodies that both did nothing. These four say it did.
	check(landed == 1,        "the parity run really did land (resolveY, falling branch)");
	check(stepped_up == 1,    "the parity run really did auto-step (tryStepUp)");
	check(blocked_at_wall == 1,
	      "the parity run really was stopped by the three-block wall (resolveX)");
	check(def.y >= 1.0f,
	      "and it finished standing on top of the step it climbed, not back at floor level");

	// The wet path (wetAt / b->eye) is the one branch the dry run above never reaches.
	Body wd, ws;
	bodyInit(&wd, 8.0f, 0.0f, 8.0f);
	bodyInit(&ws, 8.0f, 0.0f, 8.0f);
	bodySetBox(&ws, PLAYER_WIDTH, PLAYER_HEIGHT, PLAYER_EYE / PLAYER_HEIGHT);
	check(bodyWetState(&s_world, &wd) == bodyWetState(&s_world, &ws),
	      "the wet probe agrees between the default and the explicit player box");
	check(bodyWetState(&s_world, &wd) == BODY_DRY,
	      "and it says dry, which is the honest answer in a world with no water in it");

	worldExit(&s_world);
}

// bodyBlocked is the four-argument function scene/ and world_test.c both call, and its
// meaning must not have moved. It is now a forward to bodyBlockedBox; this pins that the
// forward passes the player's numbers and not something adjacent.
static void testBodyBlockedStillMeansThePlayerBox(void)
{
	makeGround(&s_world, -1, -1, 1, 1);
	for (int x = 6; x <= 10; x++)
		for (int z = 6; z <= 10; z++)
			worldSet(&s_world, x, 2, z, BLOCK_STONE);

	int mismatches = 0;
	int saw_true = 0, saw_false = 0;
	for (int i = 0; i < 60; i++) {
		const float px = 5.0f + (float)i * 0.12f;
		for (int j = 0; j < 24; j++) {
			const float py = (float)j * 0.15f;
			const bool a = bodyBlocked(&s_world, px, py, 8.3f);
			const bool b = bodyBlockedBox(&s_world, px, py, 8.3f,
			                              PLAYER_WIDTH * 0.5f, PLAYER_HEIGHT);
			if (a != b) mismatches++;
			if (a) saw_true = 1; else saw_false = 1;
		}
	}
	check(mismatches == 0,
	      "over 1,440 positions bodyBlocked and the player-box bodyBlockedBox never disagree");
	check(saw_true == 1 && saw_false == 1,
	      "and that sweep saw both answers, so it is not 1,440 copies of one trivial case");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// The tick schedule
// ---------------------------------------------------------------------------------------

static void testDistanceSchedule(void)
{
	makeGround(&s_world, -2, -2, 6, 6);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	// One well inside TICK_NEAR_BLOCKS (24) of the player at the origin, one well outside.
	const int near_s = entitySpawn(&ew, 1,  5.0f, 0.0f,  0.0f, 0.6f, 0.9f, 0.8f);
	const int far_s  = entitySpawn(&ew, 1, 60.0f, 0.0f,  0.0f, 0.6f, 0.9f, 0.8f);
	check(near_s >= 0 && far_s >= 0, "a near and a far entity both spawn");

	const Entity* n = entityGet(&ew, near_s);
	const Entity* f = entityGet(&ew, far_s);
	check(entityTickPeriod(n, 0.0f, 0.0f) == 1,
	      "an entity 5 blocks away ticks at the full rate (period 1)");
	check(entityTickPeriod(f, 0.0f, 0.0f) == TICK_FAR_PERIOD,
	      "an entity 60 blocks away ticks at the far period (10)");

	// The boundary, from tick.h's own rule: 24 blocks squared is 576 and is INSIDE.
	Entity probe = *n;
	probe.body.x = 23.9f; probe.body.z = 0.0f;
	check(entityTickPeriod(&probe, 0.0f, 0.0f) == 1, "23.9 blocks is still the near tier");
	probe.body.x = 24.1f;
	check(entityTickPeriod(&probe, 0.0f, 0.0f) == TICK_FAR_PERIOD,
	      "24.1 blocks has crossed into the far tier");

	// And the schedule as it actually runs. Counted off entityTick's own stats, not
	// re-derived, so the check is on the code path production uses.
	int near_due = 0, far_due = 0;
	for (uint64_t t = 0; t < 100; t++) {
		near_due += entityTickDue(entityGet(&ew, near_s), t, 0.0f, 0.0f) ? 1 : 0;
		far_due  += entityTickDue(entityGet(&ew, far_s),  t, 0.0f, 0.0f) ? 1 : 0;
	}
	check(near_due == 100, "over 100 ticks the near entity is due every single tick");
	check(far_due == 10,
	      "over 100 ticks the far entity is due exactly 10 times -- a 10x decimation");

	worldExit(&s_world);
}

// The stagger. tick.h's tickDue takes an id specifically so the decimated set does not all
// fire on one tick, and if that stopped working the saving would become a 2 Hz spike --
// which is invisible in every check that only counts totals.
static void testDecimatedSetIsStaggered(void)
{
	makeGround(&s_world, -2, -2, 8, 8);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	// 40 entities, all far away, all at the same distance so the period is identical and
	// the only thing that can spread them is the id.
	for (int i = 0; i < 40; i++)
		entitySpawn(&ew, 1, 100.0f, 0.0f, (float)i * 0.01f, 0.6f, 0.9f, 0.8f);
	check(entityCount(&ew) == 40, "40 far entities are in the store");

	int per_tick[TICK_FAR_PERIOD];
	for (int i = 0; i < TICK_FAR_PERIOD; i++) per_tick[i] = 0;

	// Counted off entityTick's OWN stats, not by re-asking entityTickDue in a loop here.
	// Measured: the first version of this test did the latter, and a red arm that deleted
	// the id stagger from entityTick's call to tickDue left it completely green -- because
	// entityTickDue is a different function and still passed the id. The instrument has to
	// share the code path with the thing it is instrumenting or it is measuring a sibling.
	int total_due = 0;
	for (uint64_t t = 0; t < TICK_FAR_PERIOD; t++) {
		const EntityTickStats st = entityTick(&ew, &s_world, t, 0.0f, 0.0f, NULL, NULL);
		per_tick[t] = st.thought;
		total_due += st.thought;
	}

	check(total_due == 40,
	      "across one full far period every one of the 40 fires exactly once, in total");

	int worst = 0, empty_ticks = 0;
	for (int i = 0; i < TICK_FAR_PERIOD; i++) {
		if (per_tick[i] > worst) worst = per_tick[i];
		if (per_tick[i] == 0) empty_ticks++;
	}
	// The failure this catches is worst == 40 with nine empty ticks: correct totals, and
	// the entire saving spent as one spike.
	check(worst < 40,
	      "the 40 do NOT all fire on the same tick -- the id stagger is doing its job");
	check(worst <= 8,
	      "the worst tick carries at most 8 of the 40, against 4 for a perfect spread");
	check(empty_ticks == 0, "and no tick in the period is idle");

	worldExit(&s_world);
}

// The physics is BATCHED, not scaled: a far entity runs `period` steps of ENTITY_DT rather
// than one step of period * ENTITY_DT. This checks the observable consequence -- a far
// entity's trajectory matches a near one's, rather than being a coarser approximation of it
// -- which is the whole reason for the departure from docs/plan-entities.md §4.3.
static void testFarPhysicsMatchesNearPhysics(void)
{
	makeGround(&s_world, -2, -2, 8, 8);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int a = entitySpawn(&ew, 1,  5.0f, 30.0f, 0.0f, 0.6f, 0.9f, 0.8f);   // near
	const int b = entitySpawn(&ew, 1, 90.0f, 30.0f, 0.0f, 0.6f, 0.9f, 0.8f);   // far

	// One full far period, so the far entity gets exactly one batched turn and the near
	// one gets ten single steps. Both should have fallen the same distance.
	int far_steps = 0, near_steps = 0;
	for (uint64_t t = 0; t < TICK_FAR_PERIOD; t++) {
		const EntityTickStats st = entityTick(&ew, &s_world, t, 0.0f, 0.0f, NULL, NULL);
		near_steps += st.steps;
		(void)far_steps;
	}

	const Entity* ea = entityGet(&ew, a);
	const Entity* eb = entityGet(&ew, b);
	check(ea != NULL && eb != NULL, "both entities survive one far period");
	check(ea->body.y == eb->body.y,
	      "after one far period the far entity has fallen EXACTLY as far as the near one");
	check(ea->body.vy == eb->body.vy, "and carries exactly the same vertical velocity");
	check(near_steps == 2 * TICK_FAR_PERIOD,
	      "the two entities cost 20 bodyStep calls between them over 10 ticks -- the far one "
	      "pays the same total physics as the near one, batched");
	check(ea->body.y < 30.0f, "and the run actually moved them, so the equality is not trivial");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// Lifetime against the world
// ---------------------------------------------------------------------------------------

static void testUnloadedColumnDespawns(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	const int s = entitySpawn(&ew, 1, 8.0f, 4.0f, 8.0f, 0.6f, 0.9f, 0.8f);   // column (0, 0)
	check(s >= 0, "an entity spawns in a loaded column");

	EntityTickStats st = entityTick(&ew, &s_world, 0, 8.0f, 8.0f, NULL, NULL);
	check(st.live == 1 && st.unloaded == 0, "it is live while its column is loaded");
	check(entityCount(&ew) == 1, "and the store still holds it");

	check(worldColumnRemove(&s_world, 0, 0), "the column under it is removed");

	st = entityTick(&ew, &s_world, 1, 8.0f, 8.0f, NULL, NULL);
	check(st.unloaded == 1, "the very next tick reports it despawned for an unloaded column");
	check(st.live == 0,     "and does not count it live");
	check(entityCount(&ew) == 0,     "the store no longer holds it");
	check(entityGet(&ew, s) == NULL, "its slot is free");

	// The negative-coordinate case, which is where a `/ 16` instead of a `>> 4` goes wrong:
	// an entity at x = -0.5 is in column -1, not column 0.
	makeGround(&s_world, -1, -1, 1, 1);
	EntityWorld nw;
	entityWorldInit(&nw, ENTITY_CAP_OLD);
	const int neg = entitySpawn(&nw, 1, -0.5f, 4.0f, -0.5f, 0.6f, 0.9f, 0.8f);
	st = entityTick(&nw, &s_world, 0, 0.0f, 0.0f, NULL, NULL);
	check(st.live == 1 && st.unloaded == 0,
	      "an entity at x = -0.5 is found in column -1, which IS loaded");
	check(worldColumnRemove(&s_world, -1, -1), "column (-1, -1) is removed");
	st = entityTick(&nw, &s_world, 1, 0.0f, 0.0f, NULL, NULL);
	check(st.unloaded == 1, "and the negative-coordinate entity despawns with it");
	check(entityGet(&nw, neg) == NULL, "its slot is free too");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// The seams: the think hook, entityKill, and ENT_F_REMOTE
// ---------------------------------------------------------------------------------------

typedef struct {
	int   calls;
	int   kill_slot;
	float last_dt;
} ThinkProbe;

static void probeThink(EntityWorld* ew, int slot, const World* w, float dt_s, void* user)
{
	ThinkProbe* p = (ThinkProbe*)user;
	(void)w;
	p->calls++;
	p->last_dt = dt_s;
	if (p->kill_slot == slot) entityKill(ew, slot);
}

static void testThinkHookAndKill(void)
{
	makeGround(&s_world, -2, -2, 8, 8);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int near_s = entitySpawn(&ew, 1,  4.0f, 1.0f, 0.0f, 0.6f, 0.9f, 0.8f);
	const int far_s  = entitySpawn(&ew, 1, 80.0f, 1.0f, 0.0f, 0.6f, 0.9f, 0.8f);

	ThinkProbe p = { 0, -1, 0.0f };
	for (uint64_t t = 0; t < TICK_FAR_PERIOD; t++)
		entityTick(&ew, &s_world, t, 0.0f, 0.0f, probeThink, &p);

	check(p.calls == TICK_FAR_PERIOD + 1,
	      "over one far period the think runs 10 times for the near entity and once for the far");

	// The dt the think is handed is the SIMULATED time that entity is about to advance by,
	// which is period * ENTITY_DT -- so an AI does not have to know its own tier to
	// integrate a timer correctly.
	ThinkProbe q = { 0, -1, 0.0f };
	entityTick(&ew, &s_world, 0, 0.0f, 0.0f, probeThink, &q);
	check(q.last_dt == (float)TICK_FAR_PERIOD * ENTITY_DT || q.last_dt == ENTITY_DT,
	      "the think's dt is that entity's own period times ENTITY_DT");

	// entityKill from inside the callback: deferred, so the callback is never standing in a
	// struct that has just been freed, and honoured before the physics runs.
	ThinkProbe k = { 0, near_s, 0.0f };
	entityTick(&ew, &s_world, 0, 0.0f, 0.0f, probeThink, &k);
	check(entityGet(&ew, near_s) == NULL, "an entityKill from inside a think takes effect");
	check(entityGet(&ew, far_s) != NULL,  "and does not touch anything else");

	check(!entityKill(&ew, near_s),   "killing an already-free slot is refused");
	check(!entityKill(&ew, -1),       "killing a negative slot is refused");
	check(!entityKill(&ew, ENTITY_SLOTS), "killing one past the last slot is refused");

	worldExit(&s_world);
}

static void testRemoteEntitiesAreNotSimulated(void)
{
	makeGround(&s_world, -1, -1, 1, 1);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	const int local  = entitySpawn(&ew, 1, 8.0f, 20.0f, 8.0f, 0.6f, 0.9f, 0.8f);
	const int remote = entitySpawn(&ew, 1, 9.0f, 20.0f, 8.0f, 0.6f, 0.9f, 0.8f);
	entityAt(&ew, remote)->flags |= ENT_F_REMOTE;

	ThinkProbe p = { 0, -1, 0.0f };
	for (uint64_t t = 0; t < 40; t++)
		entityTick(&ew, &s_world, t, 8.0f, 8.0f, probeThink, &p);

	const Entity* l = entityGet(&ew, local);
	const Entity* r = entityGet(&ew, remote);
	check(l != NULL && r != NULL, "both a local and a remote entity survive 40 ticks");
	check(l->body.y < 20.0f, "the local one fell -- it is simulated");
	check(r->body.y == 20.0f, "the remote one did NOT move -- its owner drives it");
	check(r->body.vy == 0.0f, "not even gravity was applied to it");
	check(p.calls == 40, "and the think ran only for the local one, 40 times in 40 ticks");

	// It is still stored, still counted, still findable -- so a renderer draws it and a
	// collision test can see it. Only the SIMULATION skips it.
	check(entityCount(&ew) == 2, "a remote entity still occupies a slot and counts");
	check(entityFindById(&ew, r->id) == remote, "and is still reachable by its id");

	worldExit(&s_world);
}

// NULL safety on every entry point. main.c will hold the store as a file static and cannot
// hand these a NULL, but a test can, and a crash on a handheld with no MMU is a hang.
static void testNullSafety(void)
{
	entityWorldInit(NULL, 8);   // must not fault
	check(entityCount(NULL) == 0,  "entityCount(NULL) is 0");
	check(entityCap(NULL) == 0,    "entityCap(NULL) is 0");
	check(entityAt(NULL, 0) == NULL,   "entityAt(NULL) is NULL");
	check(entityGet(NULL, 0) == NULL,  "entityGet(NULL) is NULL");
	check(entityFindById(NULL, 1) == -1, "entityFindById(NULL) is -1");
	check(entitySpawn(NULL, 1, 0, 0, 0, 0, 0, 0) == -1, "entitySpawn(NULL) is -1");
	check(!entityDespawn(NULL, 0), "entityDespawn(NULL) is false");
	check(!entityKill(NULL, 0),    "entityKill(NULL) is false");
	check(entityTickPeriod(NULL, 0, 0) == TICK_FAR_PERIOD,
	      "entityTickPeriod(NULL) answers the CHEAP period, never the full rate");
	check(!entityTickDue(NULL, 0, 0, 0), "entityTickDue(NULL) is false");

	EntityWorld ew;
	entityWorldInit(&ew, 4);
	const EntityTickStats st = entityTick(&ew, NULL, 0, 0, 0, NULL, NULL);
	check(st.live == 0 && st.steps == 0, "entityTick with a NULL world does nothing");
	const EntityTickStats st2 = entityTick(NULL, NULL, 0, 0, 0, NULL, NULL);
	check(st2.live == 0, "entityTick with a NULL store does nothing");

	bodySetBox(NULL, 1.0f, 1.0f, 0.5f);   // must not fault
	check(true, "bodySetBox(NULL) does not fault");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== entity test ==");

	testFixtureAndFreshStore();
	testConstantsPinned();
	testSpawnAndDespawn();
	testIdStabilityAcrossDespawn();
	testPoolExhaustion();
	testEntityFallsAndLands();
	testEntityIsStoppedByAWall();
	testEntityBoxIsActuallyUsed();
	testDefaultBodyMatchesPlayerConstants();
	testPlayerBoxIsUnchanged();
	testBodyBlockedStillMeansThePlayerBox();
	testDistanceSchedule();
	testDecimatedSetIsStaggered();
	testFarPhysicsMatchesNearPhysics();
	testUnloadedColumnDespawns();
	testThinkHookAndKill();
	testRemoteEntitiesAreNotSimulated();
	testNullSafety();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
