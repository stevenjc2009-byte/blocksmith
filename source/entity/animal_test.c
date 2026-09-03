// Host tests for the animal layer (entity/animal.c, v1.8.14).
//
// WHAT THIS FILE IS DEFENDING.
//
// animal.c is new code with no shipped behaviour to regress, so the interesting risk is not
// "did it change" but "was it ever right". The failures that would actually reach a player
// are these, and each has a named test below:
//
//   1. A SIGN ERROR IN THE YAW CONVENTION. The project's forward is (sin yaw, -cos yaw) --
//      read out of scene/player.c:48-51 and scene/interact.c:526-529, quoted in animal.c's
//      header comment. Flip either sign and animals flee TOWARDS whatever hit them, and a
//      test written from the same wrong assumption (asserting an angle equals a number)
//      would agree with the bug. So testFleeRunsAwayAlongX/Z assert a fact about the WORLD:
//      the distance from the threat never drops below where it started and ends several
//      blocks higher, measured over a real entityTick() run with the real bodyStep. Each of
//      the two cases is degenerate on one axis, so between them they pin both signs
//      independently -- a diagonal case would NOT, because mirroring in z still increases
//      the distance from a diagonal threat.
//
//   2. The think INTEGRATING POSITION. entityTick runs bodyStep immediately after the think
//      (entity.c:270-273), so anything the think adds to body.x is applied twice. Asserted
//      bit-identically with ==, never an epsilon: an epsilon would pass against a think that
//      moved an animal by a millimetre a tick, which is exactly the bug.
//
//   3. A uint8_t UNDERFLOW ON DEATH. `health -= damage` on a 2 hp chicken hit for 4 gives
//      254 and an unkillable animal. Asserted on the VALUE, not on "it survived".
//
//   4. THE CAP NOT HOLDING, so animals fill the shared pool and v1.8.16's monsters have
//      nowhere to spawn. Driven over hundreds of column loads, checked every iteration, with
//      a control that proves the cap was actually reached (a cap test on a run that never
//      spawns anything cannot fail).
//
//   5. THE DROP COUNT LEAVING ITS RANGE. Checked over 500 kills, not one, because a rule
//      that is wrong one time in fifty is invisible to a single sample -- this project has a
//      recorded case of a 47/47 pass hiding a 28% failure rate.
//
// #ifndef __3DS__ IS MANDATORY AND IS NOT DECORATION. Makefile:26 globs source/entity into
// the console build, so an unguarded test file drags a second main() into the ELF and breaks
// it. Files under tests/ are exempt; files under source/ are not, and that asymmetry is the
// trap. Copied verbatim from entity_test.c:31.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "entity/animal.h"
#include "entity/entity.h"
#include "world/block.h"
#include "world/genversion.h"
#include "world/physics.h"
#include "world/rng.h"
#include "world/tick.h"
#include "world/world.h"
#include "world/worldgen.h"

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
// entity_test.c:55-58 and world/tick_test.c both state at length: a check that never RUNS is
// indistinguishable from a check that passes, and an early return or a misplaced continue
// removes checks silently while the suite still reports "0 failed". Editing this by hand is
// the cost of the guard.
// [2026-09-03] 223 -> 236 when the meat ids landed: +4 registry-resolution checks in
// testDefTable and +9 in the new testKillDropsThisKindsMeat (3 kinds x 3). The four per-kind
// id checks REPLACED the four `== 0` placeholder checks, so they moved the count by nothing.
#define ANIMAL_TEST_EXPECTED_CHECKS 236

// Deliberately NOT routed through check(), so it cannot perturb the number it is testing.
static void checkCountPin(void)
{
	if (g_checks == ANIMAL_TEST_EXPECTED_CHECKS) return;

	g_fails++;
	printf("  FAIL   CHECK COUNT: expected %d checks, ran %d. A check was added, deleted,\n"
	       "         or skipped by an early return -- the pass/fail line below cannot be\n"
	       "         trusted until this agrees.\n",
	       ANIMAL_TEST_EXPECTED_CHECKS, g_checks);
}

static World    s_world;
static WorldGen s_gen;

// floor(), not a cast. The biome fixture below scans columns from cx = -24, so half the
// positions this suite looks at are negative, and (int) truncates towards zero -- an animal
// standing at x = -19.5 is in block -20, not block -19. Reading the wrong block is how a
// placement check goes red against correct code.
static int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

// ---------------------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------------------

// A patch of loaded, EMPTY columns, exactly entity_test.c's makeGround. world.h's coordinate
// convention makes anything below y = 0 read as WORLD_FLOOR_BLOCK, so a body dropped into an
// empty loaded column rests at exactly y = 0 on a surface bodyBlocked agrees is solid, and
// the fixture needs no block writes and no generator at all.
static void makeGround(World* w, int cx0, int cz0, int cx1, int cz1)
{
	worldInit(w);
	for (int cx = cx0; cx <= cx1; cx++)
		for (int cz = cz0; cz <= cz1; cz++)
			worldColumnCreate(w, cx, cz);
}

// A column the SPAWNER will accept: grass laid at exactly worldgenHeight() - 1 for all 256
// (x, z) in it, with the two blocks above left as the air a fresh column already is. Written
// against the real generator rather than a flat plate, so the spawner's worldgenHeight() call
// and this fixture cannot drift apart.
static void paintGeneratedColumn(World* w, const WorldGen* g, int cx, int cz)
{
	worldColumnCreate(w, cx, cz);
	for (int lx = 0; lx < 16; lx++) {
		for (int lz = 0; lz < 16; lz++) {
			const int x = (cx << 4) + lx;
			const int z = (cz << 4) + lz;
			const int y = worldgenHeight(g, (int32_t)x, (int32_t)z);
			if (y < 1 || y + 1 >= WORLD_HEIGHT) continue;
			worldSet(w, x, y - 1, z, BLOCK_GRASS);
		}
	}
}

// The same column painted deliberately BADLY, so that each half of the spawner's suitability
// test is falsifiable. A fully painted column cannot catch a dropped ground check or a
// dropped headroom check, because every candidate spot in it is good -- which is exactly the
// shape of "a guard that never runs" this project keeps rediscovering.
//
//   * only cells with ((lx + lz) & 1) == 0 get ground at all; the rest are open air down to
//     the floor, so a spawner that skips the ground test drops an animal into a hole
//   * of those, (lx & 3) == 0 gets stone in the FEET cell and (lx & 3) == 2 gets stone in
//     the HEAD cell, so a spawner that skips either air test buries an animal in rock
//
// A quarter of the column's 256 cells are genuinely spawnable, which is plenty for the roll
// to find and few enough that a broken rule places somewhere it should not within a few
// herds.
static void paintPatchyColumn(World* w, const WorldGen* g, int cx, int cz)
{
	worldColumnCreate(w, cx, cz);
	for (int lx = 0; lx < 16; lx++) {
		for (int lz = 0; lz < 16; lz++) {
			if (((lx + lz) & 1) != 0) continue;          // no ground here at all

			const int x = (cx << 4) + lx;
			const int z = (cz << 4) + lz;
			const int y = worldgenHeight(g, (int32_t)x, (int32_t)z);
			if (y < 1 || y + 1 >= WORLD_HEIGHT) continue;

			worldSet(w, x, y - 1, z, BLOCK_GRASS);
			if ((lx & 3) == 0) worldSet(w, x, y,     z, BLOCK_STONE);   // feet cell blocked
			if ((lx & 3) == 2) worldSet(w, x, y + 1, z, BLOCK_STONE);   // head cell blocked
		}
	}
}

// An animal counter written INDEPENDENTLY of animal.c, comparing kind ids by hand rather
// than asking animalDef(). The cap tests measure with this one, because a cap measured with
// the very function under test cannot notice that function being wrong -- a broken
// animalCount() would inflate both the limit and the reading and they would agree.
static int liveAnimalsIndependently(const EntityWorld* ew)
{
	int n = 0;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const Entity* e = entityGet(ew, i);
		if (e && e->kind >= ENT_KIND_PIG && e->kind <= ENT_KIND_SHEEP) n++;
	}
	return n;
}

// Finds `want` columns whose centre is in biome `b`, scanning outwards from the origin.
// Returns how many it found.
static int findColumnsInBiome(const WorldGen* g, BiomeId b, int* cxs, int* czs, int want)
{
	int found = 0;
	for (int cz = -24; cz <= 24 && found < want; cz++) {
		for (int cx = -24; cx <= 24 && found < want; cx++) {
			const int32_t x = (int32_t)((cx << 4) + 8);
			const int32_t z = (int32_t)((cz << 4) + 8);
			if (worldgenBiomeAt(g, x, z) != b) continue;
			cxs[found] = cx;
			czs[found] = cz;
			found++;
		}
	}
	return found;
}

// ---------------------------------------------------------------------------------------
// The control. Runs first, so a broken fixture is distinguishable from a broken subject.
// ---------------------------------------------------------------------------------------

static void testFixtureControl(void)
{
	makeGround(&s_world, -1, -1, 1, 1);
	check(worldColumn(&s_world, 0, 0) != NULL, "control: the fixture loads column (0, 0)");
	check(worldColumn(&s_world, 9, 9) == NULL, "control: a column nobody made is not loaded");
	check(bodyBlocked(&s_world, 8.0f, -1.0f, 8.0f),
	      "control: the world floor is solid, so a falling body has something to rest on");
	worldExit(&s_world);

	check(worldgenInit(&s_gen, 0x5EEDF00Du, GEN_VERSION_NEWEST),
	      "control: the generator initialises at the newest version");

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	check(animalCount(&ew) == 0, "control: a fresh store holds no animals");
}

// ---------------------------------------------------------------------------------------
// Constants, the table, and the shape of AnimalDef
// ---------------------------------------------------------------------------------------

static void testConstantsPinned(void)
{
	check(ENT_KIND_PIG     == 1, "ENT_KIND_PIG is 1");
	check(ENT_KIND_COW     == 2, "ENT_KIND_COW is 2");
	check(ENT_KIND_CHICKEN == 3, "ENT_KIND_CHICKEN is 3");
	check(ENT_KIND_SHEEP   == 4, "ENT_KIND_SHEEP is 4");
	check(ENT_KIND_COUNT   == 5, "ENT_KIND_COUNT is one past the last animal kind");

	check(ANIMAL_AI_IDLE   == 0, "ANIMAL_AI_IDLE is 0, so a zeroed entity is already valid");
	check(ANIMAL_AI_WANDER == 1, "ANIMAL_AI_WANDER is 1");
	check(ANIMAL_AI_FLEE   == 2, "ANIMAL_AI_FLEE is 2");

	check(ANIMAL_FIST_DAMAGE == 4, "ANIMAL_FIST_DAMAGE is 4");
	check(ANIMAL_CAP_OLD     == 16, "ANIMAL_CAP_OLD is 16");
	check(ANIMAL_CAP_NEW     == 32, "ANIMAL_CAP_NEW is 32");
}

static void testDefTable(void)
{
	check(animalDef(ENT_NONE) == NULL, "kind 0 is the free-slot marker, not an animal");
	for (uint8_t k = ENT_KIND_PIG; k < ENT_KIND_COUNT; k++)
		check(animalDef(k) != NULL, "every kind in 1..4 has a def row");

	// The v1.8.16 reservation, pinned so a fifth animal cannot quietly take one of them.
	check(animalDef(5) == NULL, "kind 5 is reserved for the zombie, not an animal");
	check(animalDef(6) == NULL, "kind 6 is reserved for the skeleton, not an animal");
	check(animalDef(7) == NULL, "kind 7 is reserved for the arrow, not an animal");
	check(animalDef(200) == NULL, "a kind far outside the table is not an animal");
	check(animalDef(255) == NULL, "kind 255 does not walk off the end of the table");

	for (uint8_t k = ENT_KIND_PIG; k < ENT_KIND_COUNT; k++) {
		const AnimalDef* d = animalDef(k);
		check(d->health >= 1 && d->health <= 20, "health is on the 0..20 scale and non-zero");
		check(d->drop_min <= d->drop_max,        "drop_min is not above drop_max");
		check(d->drop_min >= 1,                  "a killed animal always drops at least one");
		check(d->width  > 0.0f,                  "the collision box has a positive width");
		check(d->height > 0.0f,                  "the collision box has a positive height");
		check(d->speed  > 0.0f,                  "the wander speed is positive");
		check(d->hostile == 0,                   "no animal is hostile in v1.8.14");
	}

	// The specific rows, so a silent edit to the table is visible here rather than only in a
	// playtest. These are the researched figures, not arbitrary.
	check(animalDef(ENT_KIND_PIG)->health     == 10, "a pig has 10 hp");
	check(animalDef(ENT_KIND_COW)->health     == 10, "a cow has 10 hp");
	check(animalDef(ENT_KIND_CHICKEN)->health ==  4, "a chicken has 4 hp -- a one-hit kill");
	check(animalDef(ENT_KIND_SHEEP)->health   ==  8, "a sheep has 8 hp");
	check(animalDef(ENT_KIND_CHICKEN)->drop_min == 1 &&
	      animalDef(ENT_KIND_CHICKEN)->drop_max == 1, "a chicken drops exactly one");

	// Drop identity, per kind and by NAME. These were `== 0` placeholder checks until the meat
	// ids landed [2026-09-03]. A loop asserting every row holds the SAME value cannot tell a
	// correct table from one whose rows have been swapped -- which is precisely the failure a
	// four-row copy-paste produces. Pinned one at a time, a pig/cow swap turns two of them red.
	check(animalDef(ENT_KIND_PIG)->drop_item     == BLOCK_RAW_PORKCHOP, "a pig drops raw porkchop");
	check(animalDef(ENT_KIND_COW)->drop_item     == BLOCK_RAW_BEEF,     "a cow drops raw beef");
	check(animalDef(ENT_KIND_CHICKEN)->drop_item == BLOCK_RAW_CHICKEN,  "a chicken drops raw chicken");
	check(animalDef(ENT_KIND_SHEEP)->drop_item   == BLOCK_RAW_MUTTON,   "a sheep drops raw mutton");

	// ...and that each id names a block the REGISTRY actually carries. block.h:369-370 states
	// blockInfo() never returns NULL and maps an unknown id onto the AIR view, so an id that
	// block.h defines but registry.c never registered reads back as air and would drop a
	// phantom. The four checks above cannot see that hole -- they only compare numbers to
	// numbers, and both sides of that comparison come from headers rather than the registry.
	for (uint8_t k = ENT_KIND_PIG; k < ENT_KIND_COUNT; k++)
		check(blockInfo(animalDef(k)->drop_item)->name != blockInfo(BLOCK_AIR)->name,
		      "the drop id resolves to a registered block, not the air fallback");
}

static void testStructShapePin(void)
{
	// Pinned because -fshort-enums makes the ARM and the host disagree about any struct with
	// an enum in it. AnimalDef has none, by construction, so these hold on BOTH ABIs -- unlike
	// sizeof(Entity), which is 60 on ARM and 64 on the host and must NOT be asserted here.
	check(sizeof(AnimalDef) == 20, "AnimalDef is 20 bytes on both ABIs");
	check(offsetof(AnimalDef, health)    == 0, "AnimalDef.health is at offset 0");
	check(offsetof(AnimalDef, drop_item) == 1, "AnimalDef.drop_item is at offset 1");
	check(offsetof(AnimalDef, drop_min)  == 2, "AnimalDef.drop_min is at offset 2");
	check(offsetof(AnimalDef, drop_max)  == 3, "AnimalDef.drop_max is at offset 3");
	check(offsetof(AnimalDef, hostile)   == 4, "AnimalDef.hostile is at offset 4");
	check(offsetof(AnimalDef, pad)       == 5, "AnimalDef.pad is at offset 5");
	check(offsetof(AnimalDef, width)     == 8, "AnimalDef.width is 4-byte aligned at 8");
	check(offsetof(AnimalDef, height)    == 12, "AnimalDef.height is at offset 12");
	check(offsetof(AnimalDef, speed)     == 16, "AnimalDef.speed is at offset 16");
	check(sizeof(((AnimalDef*)0)->health) == 1, "AnimalDef.health is exactly one byte");
}

static void testCapFor(void)
{
	check(animalCapFor(false) == ANIMAL_CAP_OLD, "the Old 3DS animal cap is 16");
	check(animalCapFor(true)  == ANIMAL_CAP_NEW, "the New 3DS animal cap is 32");
	check(animalCapFor(false) <  entityCapFor(false),
	      "the Old animal cap leaves room in the pool for v1.8.16's monsters");
	check(animalCapFor(true)  <  entityCapFor(true),
	      "the New animal cap leaves room in the pool for v1.8.16's monsters");
}

static void testAnimalCountCountsOnlyAnimals(void)
{
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	check(entitySpawn(&ew, ENT_KIND_PIG,     4.0f, 0.0f, 4.0f, 0.9f, 0.9f, 0.9f) >= 0, "spawn a pig");
	check(entitySpawn(&ew, ENT_KIND_COW,     6.0f, 0.0f, 4.0f, 0.9f, 1.4f, 0.9f) >= 0, "spawn a cow");
	check(entitySpawn(&ew, ENT_KIND_CHICKEN, 8.0f, 0.0f, 4.0f, 0.4f, 0.7f, 0.9f) >= 0, "spawn a chicken");
	// A reserved monster id in the SAME pool -- the case that separates animalCount() from
	// entityCount(). Counting every non-free slot would answer 4 here.
	check(entitySpawn(&ew, 5,               10.0f, 0.0f, 4.0f, 0.6f, 1.8f, 0.9f) >= 0, "spawn a kind-5 entity");

	check(entityCount(&ew) == 4, "the store holds four entities");
	check(animalCount(&ew) == 3, "only three of them are animals");
	check(animalCount(NULL) == 0, "animalCount survives a NULL store");
}

// ---------------------------------------------------------------------------------------
// The think
// ---------------------------------------------------------------------------------------

static int spawnAt(EntityWorld* ew, uint8_t kind, float x, float y, float z)
{
	const AnimalDef* d = animalDef(kind);
	const int slot = entitySpawn(ew, kind, x, y, z, d->width, d->height, 0.9f);
	Entity* e = entityAt(ew, slot);
	if (e) e->health = d->health;
	return slot;
}

static void testThinkNeverMovesTheBody(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_PIG, 8.0f, 0.0f, 8.0f);

	Rng rng; rngSeed(&rng, 0xA11A0001u);
	AnimalCtx ctx = { &s_gen, &rng };

	// 300 direct think calls with the timer forced to 0 every time, so EVERY call takes the
	// deciding path rather than the early return -- the early return could not move anything
	// anyway, and a test that mostly exercises it would prove nothing.
	Entity* e = entityAt(&ew, slot);
	const float x0 = e->body.x, y0 = e->body.y, z0 = e->body.z;

	bool moved = false;
	for (int i = 0; i < 300; i++) {
		e->ai_timer = 0;
		animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);
		// == on float, never an epsilon: an epsilon here would pass against a think that
		// moved an animal by a millimetre a tick, which is exactly the defect.
		if (e->body.x != x0 || e->body.y != y0 || e->body.z != z0) moved = true;
	}
	check(!moved, "300 think calls leave the body's position bit-identical");

	worldExit(&s_world);
}

static void testThinkMidTimerIsInert(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_COW, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);

	Rng rng; rngSeed(&rng, 0xA11A0002u);
	AnimalCtx ctx = { &s_gen, &rng };

	// An IDLE animal mid-timer. Nothing may change.
	e->ai_state = ANIMAL_AI_IDLE;
	e->ai_timer = 40;
	e->yaw      = 1.25f;
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;
	animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);
	check(e->ai_state == ANIMAL_AI_IDLE, "an IDLE animal mid-timer keeps its state");
	check(e->ai_timer == 40,             "an IDLE animal mid-timer keeps its timer");
	check(e->yaw == 1.25f,               "an IDLE animal mid-timer keeps its yaw");
	check(e->body.vx == 0.0f && e->body.vz == 0.0f,
	      "an IDLE animal mid-timer keeps its velocity");

	// A WANDER animal mid-timer that is still MOVING. Also nothing may change -- the stuck
	// branch must not fire just because the timer has not expired.
	e->ai_state = ANIMAL_AI_WANDER;
	e->ai_timer = 40;
	e->yaw      = 0.5f;
	e->body.vx  = 1.0f;
	e->body.vz  = -1.0f;
	animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);
	check(e->ai_state == ANIMAL_AI_WANDER, "a moving WANDER animal mid-timer keeps its state");
	check(e->ai_timer == 40,               "a moving WANDER animal mid-timer keeps its timer");
	check(e->yaw == 0.5f,                  "a moving WANDER animal mid-timer keeps its yaw");
	check(e->body.vx == 1.0f && e->body.vz == -1.0f,
	      "a moving WANDER animal mid-timer keeps its velocity");

	// A FLEE animal mid-timer, likewise. The stuck branch is WANDER-only on purpose: an
	// animal cornered while fleeing must not spin, it must keep pushing away.
	e->ai_state = ANIMAL_AI_FLEE;
	e->ai_timer = 40;
	e->yaw      = 2.0f;
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;
	animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);
	check(e->ai_state == ANIMAL_AI_FLEE, "a stopped FLEE animal mid-timer keeps its state");
	check(e->yaw == 2.0f,                "a stopped FLEE animal mid-timer keeps its yaw");

	worldExit(&s_world);
}

static void testThinkStuckWanderTurns(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_PIG, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);

	Rng rng; rngSeed(&rng, 0xA11A0003u);
	AnimalCtx ctx = { &s_gen, &rng };

	// The wall-bounce signal: bodyMove zeroed both horizontal axes on the previous step.
	e->ai_state = ANIMAL_AI_WANDER;
	e->ai_timer = 55;
	e->yaw      = 0.0f;
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;
	animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);

	const float speed = animalDef(ENT_KIND_PIG)->speed;
	const float mag   = sqrtf(e->body.vx * e->body.vx + e->body.vz * e->body.vz);
	check(e->ai_state == ANIMAL_AI_WANDER, "a stuck WANDER animal stays in WANDER");
	check(e->ai_timer == 55,               "a stuck WANDER animal keeps its timer");
	check(fabsf(mag - speed) < 1e-4f,      "a stuck WANDER animal is driven again at full speed");
	check(mag > 0.0f,                      "a stuck WANDER animal is actually moving afterwards");

	worldExit(&s_world);
}

static void testThinkWithoutAnRngIsANoop(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_SHEEP, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);

	e->ai_state = ANIMAL_AI_IDLE;
	e->ai_timer = 0;
	e->yaw      = 0.75f;
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;

	animalThink(&ew, slot, &s_world, ENTITY_DT, NULL);
	check(e->ai_state == ANIMAL_AI_IDLE && e->ai_timer == 0 && e->yaw == 0.75f,
	      "a think with a NULL ctx changes nothing at all");

	AnimalCtx no_rng = { &s_gen, NULL };
	animalThink(&ew, slot, &s_world, ENTITY_DT, &no_rng);
	check(e->ai_state == ANIMAL_AI_IDLE && e->ai_timer == 0 && e->yaw == 0.75f,
	      "a think with a ctx that has no Rng changes nothing at all");

	// A monster id sharing the pool must be left completely alone by the ANIMAL think.
	const int mslot = entitySpawn(&ew, 5, 10.0f, 0.0f, 10.0f, 0.6f, 1.8f, 0.9f);
	Entity* m = entityAt(&ew, mslot);
	m->ai_state = 0;
	m->ai_timer = 0;
	m->yaw      = 0.0f;
	Rng rng; rngSeed(&rng, 0xA11A0004u);
	AnimalCtx ctx = { &s_gen, &rng };
	for (int i = 0; i < 50; i++) { m->ai_timer = 0; animalThink(&ew, mslot, &s_world, ENTITY_DT, &ctx); }
	check(m->ai_state == 0 && m->yaw == 0.0f && m->body.vx == 0.0f && m->body.vz == 0.0f,
	      "the animal think leaves a reserved monster kind entirely alone");

	worldExit(&s_world);
}

static void testWanderVelocityMatchesTheYawConvention(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	Rng rng; rngSeed(&rng, 0xA11A0005u);
	AnimalCtx ctx = { &s_gen, &rng };

	const uint8_t kinds[4] = { ENT_KIND_PIG, ENT_KIND_COW, ENT_KIND_CHICKEN, ENT_KIND_SHEEP };
	for (int ki = 0; ki < 4; ki++) {
		const int slot = spawnAt(&ew, kinds[ki], 8.0f + (float)ki, 0.0f, 8.0f);
		Entity* e = entityAt(&ew, slot);
		const AnimalDef* d = animalDef(kinds[ki]);

		// Force the deciding path until it actually picks WANDER. Bounded, not a while(1).
		bool wandered = false;
		for (int i = 0; i < 200 && !wandered; i++) {
			e->ai_timer = 0;
			e->ai_state = ANIMAL_AI_IDLE;
			animalThink(&ew, slot, &s_world, ENTITY_DT, &ctx);
			wandered = (e->ai_state == ANIMAL_AI_WANDER);
		}
		check(wandered, "an IDLE animal eventually decides to wander");

		const float mag = sqrtf(e->body.vx * e->body.vx + e->body.vz * e->body.vz);
		check(fabsf(mag - d->speed) < 1e-4f,
		      "a wandering animal's speed is exactly its row's speed, not its square");
		// The convention itself: forward(yaw) = (sin yaw, -cos yaw), read out of
		// scene/player.c:48-51. Bit-identical, because both sides are the same two calls.
		check(e->body.vx == sinf(e->yaw) * d->speed,
		      "wander vx is +sin(yaw) * speed");
		check(e->body.vz == -cosf(e->yaw) * d->speed,
		      "wander vz is -cos(yaw) * speed");
		check(e->ai_timer >= 40 && e->ai_timer <= 119,
		      "a wander decision lasts 2 to 6 seconds");
	}

	worldExit(&s_world);
}

static void testLongRunStaysFinite(void)
{
	makeGround(&s_world, -4, -4, 4, 4);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	for (int i = 0; i < 12; i++)
		spawnAt(&ew, (uint8_t)(ENT_KIND_PIG + (i & 3)),
		        8.0f + (float)(i % 4) * 3.0f, 2.0f, 8.0f + (float)(i / 4) * 3.0f);

	Rng rng; rngSeed(&rng, 0xA11A0006u);
	AnimalCtx ctx = { &s_gen, &rng };

	const int spawned = animalCount(&ew);
	check(spawned == 12, "twelve animals are alive before the run");

	bool all_finite = true;
	for (uint64_t t = 1; t <= 400; t++) {
		entityTick(&ew, &s_world, t, 8.0f, 8.0f, animalThink, &ctx);
		for (int i = 0; i < ENTITY_SLOTS; i++) {
			const Entity* e = entityGet(&ew, i);
			if (!e) continue;
			if (!isfinite(e->body.x) || !isfinite(e->body.y) || !isfinite(e->body.z) ||
			    !isfinite(e->yaw)    || !isfinite(e->body.vx) || !isfinite(e->body.vz))
				all_finite = false;
		}
	}
	check(all_finite, "400 ticks of AI leave every position, yaw and velocity finite");
	check(animalCount(&ew) > 0, "at least some of the herd is still inside the loaded ring");

	// Every survivor must still be standing in a loaded column -- entityTick's own lifetime
	// rule -- and must not have sunk through the floor.
	bool grounded = true;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const Entity* e = entityGet(&ew, i);
		if (!e) continue;
		if (e->body.y < -0.001f) grounded = false;
	}
	check(grounded, "no animal fell through the world floor over 400 ticks");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// Combat: damage, death, drops
// ---------------------------------------------------------------------------------------

static void testHurtBelowLethal(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_COW, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);

	uint8_t item = 99, count = 99;
	const bool killed = animalHurt(&ew, slot, ANIMAL_FIST_DAMAGE, 4.0f, 8.0f, &item, &count);

	check(!killed,                  "a cow survives one bare-handed hit");
	check(e->health == 10 - ANIMAL_FIST_DAMAGE,
	      "health drops by exactly the damage dealt");
	check(e->ai_state == ANIMAL_AI_FLEE, "a hurt animal flees");
	check(e->ai_timer == 60,             "a flee lasts exactly 60 ticks");
	check(item == 0,                     "a survivor writes no drop item");
	check(count == 0,                    "a survivor writes no drop count");
	check((e->flags & ENT_F_DESPAWN) == 0, "a survivor is not marked for removal");

	const float mag = sqrtf(e->body.vx * e->body.vx + e->body.vz * e->body.vz);
	check(fabsf(mag - animalDef(ENT_KIND_COW)->speed * 1.5f) < 1e-4f,
	      "a fleeing animal runs at 1.5x its wander speed");

	// Both out pointers are documented optional.
	const bool killed2 = animalHurt(&ew, slot, 1, 4.0f, 8.0f, NULL, NULL);
	check(!killed2, "animalHurt accepts NULL out pointers");
	check(e->health == 10 - ANIMAL_FIST_DAMAGE - 1, "the second hit landed too");

	worldExit(&s_world);
}

static void testHurtLethalAndTheDeferredKill(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_CHICKEN, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);
	check(e->health == 4, "a chicken spawns with its row's 4 hp, not entitySpawn's 20");

	uint8_t item = 99, count = 99;
	const bool killed = animalHurt(&ew, slot, ANIMAL_FIST_DAMAGE, 4.0f, 8.0f, &item, &count);

	check(killed, "one bare-handed hit kills a chicken");
	check(e->health == 0, "a killed animal is at 0 hp");
	check(count == 1, "a chicken drops exactly one item");
	// Pinned to the CONSTANT, not to animalDef()->drop_item. Comparing the kill path's output
	// against the same table the kill path read it from is self-referential: a table with the
	// wrong id in it passes that check every time. The constant is the independent side.
	check(item == BLOCK_RAW_CHICKEN,
	      "the killed chicken's drop item is raw chicken");
	check((e->flags & ENT_F_DESPAWN) != 0, "the kill is DEFERRED, not immediate");
	check(animalCount(&ew) == 1, "the slot is still occupied until the tick ends");

	Rng rng; rngSeed(&rng, 0xA11A0007u);
	AnimalCtx ctx = { &s_gen, &rng };
	for (uint64_t t = 1; t <= 4; t++)
		entityTick(&ew, &s_world, t, 8.0f, 8.0f, animalThink, &ctx);
	check(animalCount(&ew) == 0, "the tick reaps the deferred kill");
	check(entityGet(&ew, slot) == NULL, "the slot is free afterwards");

	worldExit(&s_world);
}

// The chicken above proves the kill path CARRIES a drop id. It cannot prove the path carries
// the RIGHT id per kind: a chicken is the one animal whose test would still pass if every kind
// dropped chicken. So walk the other three the same way, each pinned to its own constant. A
// table with two rows swapped now goes red at the far end of the plumbing as well as at the
// table itself, which is what distinguishes "the row is wrong" from "the row is never read".
static void testKillDropsThisKindsMeat(void)
{
	static const struct { uint8_t kind; uint8_t want; const char* msg; } kCases[] = {
		{ ENT_KIND_PIG,   BLOCK_RAW_PORKCHOP, "a killed pig yields raw porkchop through animalHurt" },
		{ ENT_KIND_COW,   BLOCK_RAW_BEEF,     "a killed cow yields raw beef through animalHurt"     },
		{ ENT_KIND_SHEEP, BLOCK_RAW_MUTTON,   "a killed sheep yields raw mutton through animalHurt" },
	};

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		makeGround(&s_world, -2, -2, 2, 2);

		EntityWorld ew;
		entityWorldInit(&ew, ENTITY_CAP_NEW);
		const int slot = spawnAt(&ew, kCases[i].kind, 8.0f, 0.0f, 8.0f);
		const AnimalDef* d = animalDef(kCases[i].kind);

		// Full health as the damage, so one call kills whatever the row's hp is. These three
		// all survive a bare fist, unlike the 4 hp chicken, so ANIMAL_FIST_DAMAGE will not do.
		uint8_t item = 99, count = 99;
		const bool killed = animalHurt(&ew, slot, d->health, 4.0f, 8.0f, &item, &count);

		check(killed, "damage equal to the row's health kills in one call");
		check(item == kCases[i].want, kCases[i].msg);
		check(count >= d->drop_min && count <= d->drop_max,
		      "the drop count lands inside the row's band");

		worldExit(&s_world);
	}
}

static void testHurtCannotUnderflowHealth(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int slot = spawnAt(&ew, ENT_KIND_CHICKEN, 8.0f, 0.0f, 8.0f);
	Entity* e = entityAt(&ew, slot);

	// 2 hp hit for 4. `health -= damage` unclamped gives 254 and an unkillable animal, so the
	// assertion is on the VALUE, not on "it died".
	e->health = 2;
	const bool killed = animalHurt(&ew, slot, 4, 4.0f, 8.0f, NULL, NULL);
	check(killed,          "a 2 hp animal hit for 4 dies");
	check(e->health == 0,  "health is clamped to 0, never wrapped to 254");

	// The exact-boundary case, where >= and > differ.
	const int slot2 = spawnAt(&ew, ENT_KIND_PIG, 12.0f, 0.0f, 8.0f);
	Entity* e2 = entityAt(&ew, slot2);
	e2->health = 4;
	check(animalHurt(&ew, slot2, 4, 4.0f, 8.0f, NULL, NULL), "damage equal to health kills");
	check(e2->health == 0, "an exact-lethal hit leaves 0 hp");

	// A hit at 255 damage, the largest a uint8_t carries.
	const int slot3 = spawnAt(&ew, ENT_KIND_COW, 14.0f, 0.0f, 8.0f);
	Entity* e3 = entityAt(&ew, slot3);
	check(animalHurt(&ew, slot3, 255, 4.0f, 8.0f, NULL, NULL), "a 255-damage hit kills");
	check(e3->health == 0, "a 255-damage hit leaves 0 hp, not a wrapped value");

	worldExit(&s_world);
}

static void testHurtRejectsNonAnimalsAndBadSlots(void)
{
	makeGround(&s_world, -2, -2, 2, 2);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const int mslot = entitySpawn(&ew, 5, 8.0f, 0.0f, 8.0f, 0.6f, 1.8f, 0.9f);

	uint8_t item = 99, count = 99;
	check(!animalHurt(&ew, mslot, 40, 4.0f, 8.0f, &item, &count),
	      "the animal damage rule refuses a reserved monster kind");
	check(item == 0 && count == 0, "a refused hurt still writes a defined zero drop");

	item = 99; count = 99;
	check(!animalHurt(&ew, 47, 4, 0.0f, 0.0f, &item, &count), "an empty slot cannot be hurt");
	check(item == 0 && count == 0, "an empty-slot hurt writes a defined zero drop");
	check(!animalHurt(&ew, -1, 4, 0.0f, 0.0f, NULL, NULL),   "a negative slot is refused");
	check(!animalHurt(&ew, 9999, 4, 0.0f, 0.0f, NULL, NULL), "an out-of-range slot is refused");
	check(!animalHurt(NULL, 0, 4, 0.0f, 0.0f, NULL, NULL),   "a NULL store is refused");

	worldExit(&s_world);
}

static void testDropCountDistribution(void)
{
	makeGround(&s_world, -4, -4, 4, 4);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	Rng rng; rngSeed(&rng, 0xA11A0008u);
	AnimalCtx ctx = { &s_gen, &rng };

	const AnimalDef* pig = animalDef(ENT_KIND_PIG);
	int seen[8];
	memset(seen, 0, sizeof seen);

	int rolls = 0, out_of_range = 0;
	uint64_t tick = 0;

	// 500 kills, not one. A rule that is wrong one roll in fifty is invisible to a single
	// sample -- this project has a recorded case of 47/47 passing while 28% was failing.
	// Each kill is a fresh entity at a fresh position, so the positional hash the drop roll
	// is drawn from is genuinely re-drawn every time.
	for (int i = 0; i < 500; i++) {
		const float x = 8.0f + (float)(i % 20);
		const float z = 8.0f + (float)((i / 20) % 20);
		const int slot = spawnAt(&ew, ENT_KIND_PIG, x, 0.0f, z);
		if (slot < 0) break;

		uint8_t item = 0, count = 0;
		if (animalHurt(&ew, slot, 200, 0.0f, 0.0f, &item, &count)) {
			rolls++;
			if (count < pig->drop_min || count > pig->drop_max) out_of_range++;
			else seen[count]++;
		}
		// Reap, so the next iteration gets a clean pool and a new id.
		entityTick(&ew, &s_world, ++tick, x, z, animalThink, &ctx);
	}

	check(rolls == 500, "all 500 kills were rolled");
	check(out_of_range == 0, "every one of 500 drop counts is inside [drop_min, drop_max]");
	// The control: if the roll always answered drop_min, the range check above could not
	// fail. This asserts the roll genuinely varies across the whole inclusive range.
	check(seen[1] > 0 && seen[2] > 0 && seen[3] > 0,
	      "the drop roll actually produces every value in 1..3, not just one of them");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------
// FLEE direction -- the yaw-convention test, and the reason it is shaped like this
// ---------------------------------------------------------------------------------------
//
// Asserted as a fact about the WORLD, not about the formula: an animal hit from a point must
// never get closer to that point than it started, and must end several blocks further away,
// after a real entityTick() run through the real bodyStep.
//
// Each case is degenerate on one axis on purpose. The z-sign error (atan2f(dx, dz) instead of
// atan2f(dx, -dz)) mirrors the flee direction in z, so it only shows up when the threat is
// displaced in z; the x-sign error only shows up when it is displaced in x. A DIAGONAL threat
// would catch neither reliably, because a z-mirrored escape from a diagonal threat still
// increases the distance -- which is exactly the sort of test that passes against the bug.
static void fleeCase(float from_x, float from_z, const char* what_min, const char* what_end)
{
	makeGround(&s_world, -4, -4, 4, 4);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	const float ax = 32.0f, az = 32.0f;
	const int slot = spawnAt(&ew, ENT_KIND_PIG, ax, 0.0f, az);

	Rng rng; rngSeed(&rng, 0xA11A0009u);
	AnimalCtx ctx = { &s_gen, &rng };

	const float d0 = sqrtf((ax - from_x) * (ax - from_x) + (az - from_z) * (az - from_z));

	check(!animalHurt(&ew, slot, ANIMAL_FIST_DAMAGE, from_x, from_z, NULL, NULL),
	      "the pig survives the hit that starts the flee");

	float d_min = d0, d_end = d0;
	for (uint64_t t = 1; t <= 50; t++) {
		// The player position handed to entityTick is the animal's own, so its tick period is
		// 1 and every tick is a due tick -- this measures the flee, not the decimation.
		const Entity* e = entityGet(&ew, slot);
		if (!e) break;
		entityTick(&ew, &s_world, t, e->body.x, e->body.z, animalThink, &ctx);

		e = entityGet(&ew, slot);
		if (!e) break;
		const float d = sqrtf((e->body.x - from_x) * (e->body.x - from_x) +
		                      (e->body.z - from_z) * (e->body.z - from_z));
		if (d < d_min) d_min = d;
		d_end = d;
	}

	check(d_min >= d0 - 0.001f, what_min);
	check(d_end >= d0 + 3.0f,   what_end);

	worldExit(&s_world);
}

static void testFleeRunsAway(void)
{
	// Threat displaced in -X only. Pins the SIN sign: negating it sends the pig straight at
	// the hitter and the minimum distance collapses to ~0.
	fleeCase(28.0f, 32.0f,
	         "fleeing a threat 4 blocks to -X, the pig never gets closer than it started",
	         "fleeing a threat 4 blocks to -X, the pig ends at least 3 blocks further away");

	// Threat displaced in -Z only. Pins the COS sign, which is the atan2f argument-order bug:
	// atan2f(dx, dz) mirrors the escape in z and runs the pig through the hitter.
	fleeCase(32.0f, 28.0f,
	         "fleeing a threat 4 blocks to -Z, the pig never gets closer than it started",
	         "fleeing a threat 4 blocks to -Z, the pig ends at least 3 blocks further away");

	// And the mirrored pair, so a sign that is wrong only in one direction cannot hide.
	fleeCase(36.0f, 32.0f,
	         "fleeing a threat 4 blocks to +X, the pig never gets closer than it started",
	         "fleeing a threat 4 blocks to +X, the pig ends at least 3 blocks further away");

	fleeCase(32.0f, 36.0f,
	         "fleeing a threat 4 blocks to +Z, the pig never gets closer than it started",
	         "fleeing a threat 4 blocks to +Z, the pig ends at least 3 blocks further away");
}

// ---------------------------------------------------------------------------------------
// The entity raycast
// ---------------------------------------------------------------------------------------

static void testRaycast(void)
{
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);

	// A pig at (8, 0, 8): half_w 0.45, so its near z face is at 7.55 and its box spans
	// y 0..0.9.
	const int pig = spawnAt(&ew, ENT_KIND_PIG, 8.0f, 0.0f, 8.0f);

	float dist = -1.0f;
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == pig,
	      "a ray down the middle hits the pig");
	check(fabsf(dist - 7.55f) < 1e-3f,
	      "the reported distance is to the near face of the box, not to its centre");

	check(animalRaycast(&ew, 10.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == -1,
	      "a ray 2.5 blocks to the side misses");
	check(animalRaycast(&ew, 8.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == -1,
	      "a ray passing well above the box misses");
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, -1.0f, 20.0f, &dist) == -1,
	      "a ray pointing away from the pig misses");

	// Reach. The near face is at 7.55, so 5.0 of reach cannot touch it and 8.0 can.
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 5.0f, &dist) == -1,
	      "an animal beyond max_distance is not hit");
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 8.0f, &dist) == pig,
	      "the same animal inside max_distance is hit");

	// A non-unit direction must give the same answer and the same distance in blocks.
	dist = -1.0f;
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 4.0f, 20.0f, &dist) == pig,
	      "a non-unit direction still hits");
	check(fabsf(dist - 7.55f) < 1e-3f,
	      "the distance is in blocks regardless of the direction vector's length");

	// Nearest of two, and the far one must be reachable on its own so the check cannot pass
	// by the near one simply being the only hit.
	const int cow = spawnAt(&ew, ENT_KIND_COW, 8.0f, 0.0f, 12.0f);
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == pig,
	      "with two animals in line, the NEARER one is returned");
	check(animalRaycast(&ew, 8.0f, 0.5f, 20.0f, 0.0f, 0.0f, -1.0f, 20.0f, &dist) == cow,
	      "from the other side, the cow is the nearer one and is returned");

	// Origin inside a box.
	dist = -1.0f;
	check(animalRaycast(&ew, 8.0f, 0.4f, 8.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == pig,
	      "a ray starting inside a box hits it");
	check(dist == 0.0f, "a hit from inside a box is at distance 0");

	// Degenerate and hostile inputs.
	check(animalRaycast(NULL, 0, 0, 0, 0, 0, 1, 20.0f, &dist) == -1, "a NULL store misses");
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, &dist) == -1,
	      "a zero-length direction is refused rather than dividing by zero");
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, &dist) == -1,
	      "a zero reach hits nothing");
	check(animalRaycast(&ew, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, NULL) == pig,
	      "a NULL out_dist is accepted");

	// A reserved monster kind must be invisible to the ANIMAL raycast, or v1.8.16 gets a
	// zombie that can be punched by the animal damage rule.
	EntityWorld mw;
	entityWorldInit(&mw, ENTITY_CAP_NEW);
	entitySpawn(&mw, 5, 8.0f, 0.0f, 8.0f, 0.6f, 1.8f, 0.9f);
	check(animalRaycast(&mw, 8.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 20.0f, &dist) == -1,
	      "the animal raycast does not see a reserved monster kind");

	EntityWorld empty;
	entityWorldInit(&empty, ENTITY_CAP_NEW);
	check(animalRaycast(&empty, 0, 0, 0, 0, 0, 1, 20.0f, &dist) == -1,
	      "an empty store misses");
}

// ---------------------------------------------------------------------------------------
// The spawner
// ---------------------------------------------------------------------------------------

static void testSpawnerRejectsBadInput(void)
{
	makeGround(&s_world, 0, 0, 0, 0);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	Rng rng; rngSeed(&rng, 0xA11A000Au);

	// An unloaded column is a no-op however many times it is asked. 200 rolls, so the
	// 1-in-12 herd chance cannot make this pass by never firing.
	int total = 0;
	for (int i = 0; i < 200; i++) total += animalSpawnForColumn(&ew, &s_world, &s_gen, 40, 40, &rng);
	check(total == 0, "200 rolls on an unloaded column spawn nothing");
	check(animalCount(&ew) == 0, "and the store is still empty");

	check(animalSpawnForColumn(NULL, &s_world, &s_gen, 0, 0, &rng) == 0, "NULL store spawns nothing");
	check(animalSpawnForColumn(&ew, NULL, &s_gen, 0, 0, &rng) == 0,      "NULL world spawns nothing");
	check(animalSpawnForColumn(&ew, &s_world, NULL, 0, 0, &rng) == 0,    "NULL worldgen spawns nothing");
	check(animalSpawnForColumn(&ew, &s_world, &s_gen, 0, 0, NULL) == 0,  "NULL rng spawns nothing");

	worldExit(&s_world);
}

// Paints `n` columns of biome `b` and returns how many it actually got. Shared by the biome
// and the cap tests so both are looking at the same fixture.
static int paintBiome(BiomeId b, int* cxs, int* czs, int want)
{
	const int got = findColumnsInBiome(&s_gen, b, cxs, czs, want);
	for (int i = 0; i < got; i++) paintGeneratedColumn(&s_world, &s_gen, cxs[i], czs[i]);
	return got;
}

static void testSpawnerBiomeRule(void)
{
	int cxs[6], czs[6];

	// The CONTROL first, so a "desert spawns nothing" result cannot be a broken fixture.
	worldInit(&s_world);
	const int plains = paintBiome(BIOME_PLAINS, cxs, czs, 6);
	check(plains > 0, "control: the seed has plains columns to spawn in");

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	Rng rng; rngSeed(&rng, 0xA11A000Bu);

	int plains_spawned = 0;
	for (int i = 0; i < 400 && plains > 0; i++) {
		const int c = i % plains;
		plains_spawned += animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng);
		entityWorldInit(&ew, ENTITY_CAP_NEW);   // keep the cap out of this measurement
	}
	check(plains_spawned > 0, "control: plains columns DO spawn animals over 400 rolls");
	worldExit(&s_world);

	// The subject.
	worldInit(&s_world);
	const int desert = paintBiome(BIOME_DESERT, cxs, czs, 6);
	check(desert > 0, "control: the seed has desert columns to test against");

	entityWorldInit(&ew, ENTITY_CAP_NEW);
	rngSeed(&rng, 0xA11A000Cu);

	int desert_spawned = 0;
	for (int i = 0; i < 400 && desert > 0; i++) {
		const int c = i % desert;
		desert_spawned += animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng);
	}
	check(desert_spawned == 0, "400 rolls over desert columns spawn nothing at all");
	check(animalCount(&ew) == 0, "and no animal ended up in the store");
	worldExit(&s_world);
}

static void testSpawnerCapAndHerdShape(void)
{
	int cxs[6], czs[6];
	worldInit(&s_world);
	const int n = paintBiome(BIOME_PLAINS, cxs, czs, 6);
	check(n > 0, "control: the cap fixture has plains columns");

	// --- The New 3DS cap, with FOUR RESERVED MONSTER KINDS ALREADY IN THE POOL.
	//
	// The monsters are the point of this arrangement, not decoration. The sub-cap is a limit
	// on ANIMALS, not on pool occupancy, so a store already holding four non-animals must
	// still reach 32 animals. A spawner that gated on entityCount(), or an animalCount() that
	// counted every occupied slot, would stop four short -- and with an all-animal pool the
	// two are indistinguishable, which is exactly the blind spot this fills.
	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	for (int i = 0; i < 4; i++)
		entitySpawn(&ew, 5, 1000.0f + (float)i, 0.0f, 1000.0f, 0.6f, 1.8f, 0.9f);
	check(entityCount(&ew) == 4 && animalCount(&ew) == 0,
	      "control: the pool starts with four monsters and no animals");

	Rng rng; rngSeed(&rng, 0xA11A000Du);

	const int cap_new = animalCapFor(true);
	bool over = false;
	int peak = 0;
	for (int i = 0; i < 900 && n > 0; i++) {
		const int c = i % n;
		animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng);
		const int live = liveAnimalsIndependently(&ew);
		if (live > cap_new) over = true;
		if (live > peak) peak = live;
	}
	check(!over, "900 column loads never push the animal count past the New 3DS cap");
	// The control that makes the check above capable of failing: a cap test on a run that
	// never fills the pool cannot go red.
	check(peak == cap_new, "and the run actually REACHED the cap, so the check could fail");
	check(entityCount(&ew) <= ENTITY_CAP_NEW, "the shared entity cap is respected too");
	check(ENTITY_CAP_NEW - peak >= 16, "16 slots are still free for v1.8.16's monsters");

	// --- The Old 3DS cap, derived from the store's own cap.
	entityWorldInit(&ew, ENTITY_CAP_OLD);
	rngSeed(&rng, 0xA11A000Eu);

	const int cap_old = animalCapFor(false);
	over = false;
	peak = 0;
	for (int i = 0; i < 900 && n > 0; i++) {
		const int c = i % n;
		animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng);
		const int live = liveAnimalsIndependently(&ew);
		if (live > cap_old) over = true;
		if (live > peak) peak = live;
	}
	check(!over, "900 column loads never push the animal count past the Old 3DS cap");
	check(peak == cap_old, "and the Old run actually REACHED its cap");
	check(ENTITY_CAP_OLD - peak >= 8, "8 slots are still free for v1.8.16's monsters");

	// --- Herd shape: size, one kind, and where the members stand.
	rngSeed(&rng, 0xA11A000Fu);
	int herds = 0;
	bool size_ok = true, one_kind = true, placed_ok = true, box_ok = true;

	for (int i = 0; i < 900 && n > 0 && herds < 40; i++) {
		entityWorldInit(&ew, ENTITY_CAP_NEW);
		const int c = i % n;
		const int got = animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng);
		if (got == 0) continue;
		herds++;

		if (got < 2 || got > 4) size_ok = false;
		if (got != animalCount(&ew)) size_ok = false;

		uint8_t kind0 = ENT_NONE;
		for (int s = 0; s < ENTITY_SLOTS; s++) {
			const Entity* e = entityGet(&ew, s);
			if (!e) continue;
			if (kind0 == ENT_NONE) kind0 = e->kind;
			if (e->kind != kind0) one_kind = false;

			const AnimalDef* d = animalDef(e->kind);
			if (!d) { one_kind = false; continue; }

			// Standing on solid ground, with two blocks of air.
			const int bx = floorToInt(e->body.x);
			const int by = floorToInt(e->body.y);
			const int bz = floorToInt(e->body.z);
			const BlockId ground = worldGet(&s_world, bx, by - 1, bz);
			if (ground != BLOCK_GRASS && ground != BLOCK_DIRT && ground != BLOCK_SAND)
				placed_ok = false;
			if (worldGet(&s_world, bx, by,     bz) != BLOCK_AIR) placed_ok = false;
			if (worldGet(&s_world, bx, by + 1, bz) != BLOCK_AIR) placed_ok = false;

			// The row was actually applied, rather than entitySpawn's defaults surviving.
			if (e->health != d->health)                          box_ok = false;
			if (fabsf(e->body.half_w - d->width * 0.5f) > 1e-6f) box_ok = false;
			if (fabsf(e->body.height - d->height)       > 1e-6f) box_ok = false;
			if (e->ai_state != ANIMAL_AI_IDLE)                   box_ok = false;
			if (e->ai_timer >= 60)                               box_ok = false;
		}
	}

	check(herds >= 20, "control: at least 20 herds were actually rolled to look at");
	check(size_ok,   "every herd has 2 to 4 members and the return value matches");
	check(one_kind,  "every herd is a single kind");
	check(placed_ok, "every member stands on grass/dirt/sand with two blocks of air above");
	check(box_ok,    "every member carries its row's health and collision box, not the defaults");

	worldExit(&s_world);
}

static void testSpawnerRefusesBadSpots(void)
{
	int cxs[4], czs[4];
	worldInit(&s_world);
	const int n = findColumnsInBiome(&s_gen, BIOME_PLAINS, cxs, czs, 4);
	check(n > 0, "control: the patchy fixture has plains columns");
	for (int i = 0; i < n; i++) paintPatchyColumn(&s_world, &s_gen, cxs[i], czs[i]);

	EntityWorld ew;
	entityWorldInit(&ew, ENTITY_CAP_NEW);
	Rng rng; rngSeed(&rng, 0xA11A0010u);

	int placed = 0, on_air = 0, in_rock = 0, head_in_rock = 0;

	for (int i = 0; i < 1200 && n > 0; i++) {
		entityWorldInit(&ew, ENTITY_CAP_NEW);
		const int c = i % n;
		if (animalSpawnForColumn(&ew, &s_world, &s_gen, cxs[c], czs[c], &rng) == 0) continue;

		for (int s = 0; s < ENTITY_SLOTS; s++) {
			const Entity* e = entityGet(&ew, s);
			if (!e) continue;
			placed++;

			const int bx = floorToInt(e->body.x);
			const int by = floorToInt(e->body.y);
			const int bz = floorToInt(e->body.z);

			const BlockId ground = worldGet(&s_world, bx, by - 1, bz);
			if (ground != BLOCK_GRASS && ground != BLOCK_DIRT && ground != BLOCK_SAND) on_air++;
			if (worldGet(&s_world, bx, by,     bz) != BLOCK_AIR) in_rock++;
			if (worldGet(&s_world, bx, by + 1, bz) != BLOCK_AIR) head_in_rock++;
		}
	}

	// The control that makes the three checks below capable of failing: a placement test that
	// placed nothing cannot go red.
	check(placed >= 40, "control: at least 40 members were placed in the patchy fixture");
	check(on_air == 0,       "no member was placed over a hole with no ground under it");
	check(in_rock == 0,      "no member was placed with its feet inside a block");
	check(head_in_rock == 0, "no member was placed with its head inside a block");

	worldExit(&s_world);
}

// ---------------------------------------------------------------------------------------

int main(void)
{
	printf("animal_test: the animal layer (entity/animal.c)\n");

	testFixtureControl();
	testConstantsPinned();
	testDefTable();
	testStructShapePin();
	testCapFor();
	testAnimalCountCountsOnlyAnimals();

	testThinkNeverMovesTheBody();
	testThinkMidTimerIsInert();
	testThinkStuckWanderTurns();
	testThinkWithoutAnRngIsANoop();
	testWanderVelocityMatchesTheYawConvention();
	testLongRunStaysFinite();

	testHurtBelowLethal();
	testHurtLethalAndTheDeferredKill();
	testKillDropsThisKindsMeat();
	testHurtCannotUnderflowHealth();
	testHurtRejectsNonAnimalsAndBadSlots();
	testDropCountDistribution();
	testFleeRunsAway();

	testRaycast();

	testSpawnerRejectsBadInput();
	testSpawnerBiomeRule();
	testSpawnerCapAndHerdShape();
	testSpawnerRefusesBadSpots();

	checkCountPin();

	printf("animal_test: %d checks, %d failed\n", g_checks, g_fails);
	return g_fails ? 1 : 0;
}

#endif  // __3DS__
