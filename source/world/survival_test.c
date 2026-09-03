/* survival_test — host unit test for health, hunger and fall damage (v1.8.13).
 *
 * Own binary rather than more checks in world/world_test.c, for the reason
 * world/playerpose_test.c gives about itself: this is a self-contained contract with its own
 * failure modes, and a suite that is one file per contract says which contract broke without
 * anybody reading a line number.
 *
 * Four kinds of check live here and the split matters:
 *
 *   1. THE CLOCKS. Exact tick counts, not "roughly a minute". A countdown that fires one tick
 *      early is invisible to a player and permanent once shipped, so every timing check here
 *      pins the tick BEFORE the event as well as the event: 1199 must do nothing and 1200
 *      must drain. Half a check would pass for a counter that fired every single tick.
 *
 *   2. THE TWO DEATH RULES, which are deliberately different. Fall damage kills; starvation
 *      floors at 1 and never does. Both directions are pinned, because a suite that only
 *      checked "damage is applied" would be equally happy with a starvation that killed and
 *      with a fall that could not.
 *
 *   3. THE APEX. The jump-then-fall case is the one behavioural check in this file that a
 *      plausible, clean, obviously-correct implementation gets WRONG — capture peak_y once
 *      when on_ground goes false, instead of fmaxf-ing it every frame — and it is wrong by
 *      exactly the jump height, which is under two blocks and therefore under the free-fall
 *      threshold for short falls. It hides. See testJumpThenFallIsMeasuredFromTheApex.
 *
 *   4. THE FILE FORMAT. Round trip and every degradation path returning the same clean
 *      "nothing usable on disk". These run the real world/survival.c against a real directory
 *      on a real filesystem — no doubles, because there is nothing here a double could stand
 *      in for that would not also stand in for the bug.
 *
 * The __3DS__ guard is load-bearing, not tidy — the same one net/blockdiff_test.c and
 * world/playerpose_test.c carry. mc/Makefile:26 globs every .c under source/ into the console
 * build, so without it this file's main() links against source/main.c's and the build dies
 * with "multiple definition of `main'".
 */
#ifndef __3DS__

/* mkdir(2) and the S_IRWXU macros are POSIX, and -std=c11 (not gnu11) makes glibc hide them
 * behind this. Same reason the tree builds strict everywhere else. */
#define _POSIX_C_SOURCE 200809L

#include "world/survival.h"

#include "world/block.h"
#include "world/crc32.h"
#include "world/inventory.h"
#include "world/physics.h"
#include "world/registry.h"
#include "world/world.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) checkAt((cond), #cond, __LINE__)

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL  L%d %s\n", line, what);
	}
}

/* Integers get their VALUES printed, not just the expression text. A red arm that says
 * "health == 18" tells you nothing you did not already know; one that says "got 19, want 18"
 * tells you the fall was measured one block short, which is the whole diagnosis. */
#define CHECK_I(got, want) checkIntAt((long)(got), (long)(want), #got, __LINE__)

static void checkIntAt(long got, long want, const char* what, int line)
{
	g_checks++;
	if (got != want) {
		g_fails++;
		printf("  FAIL  L%d %s: got %ld, want %ld\n", line, what, got, want);
	}
}

/* ── the world the fall checks run against ───────────────────────────────────────────── */

static World s_world;

/* Two columns, far apart, so neither can contaminate the other: the DRY one is plain air
 * (worldGet answers BLOCK_AIR for an absent chunk, so nothing has to be placed) and the WET
 * one is a water column tall enough to cover both cells physics.c's wetAt() probes — the feet
 * cell at floor(y) and the eye cell at floor(y + b->eye). */
#define DRY_X 8
#define DRY_Z 8
#define WET_X 100
#define WET_Z 100

static void fillWaterColumn(void)
{
	for (int y = 0; y < 40; y++)
		if (!worldSet(&s_world, WET_X, y, WET_Z, BLOCK_WATER))
			printf("  (worldSet refused water at %d,%d,%d — the fixture is broken)\n",
			       WET_X, y, WET_Z);
}

/* ── driving a fall ──────────────────────────────────────────────────────────────────────
 *
 * fallDamageUpdate() reads exactly three things off the Body — y, on_ground, and (through
 * bodySubmerged) its position in the world — so a fall is driven by setting them frame by
 * frame rather than by running bodyStep(). That is deliberate: bodyStep would put gravity,
 * substepping and terminal velocity between the test and the rule under test, and the heights
 * this file asserts on would become whatever the integrator happened to produce. The rule
 * being checked is "damage from tracked distance", and this drives exactly that.
 */
static bool runFall(FallTrack* ft, Survival* s, Body* b,
                    const float* air, size_t n_air, float land_y)
{
	b->on_ground = false;
	for (size_t i = 0; i < n_air; i++) {
		b->y = air[i];
		fallDamageUpdate(ft, s, b, &s_world);
	}
	b->y         = land_y;
	b->on_ground = true;
	return fallDamageUpdate(ft, s, b, &s_world);
}

/* A plain drop from `top` to `bottom` with a couple of frames in between, which is what every
 * threshold check below wants. The intermediate heights are inside the interval, so they can
 * never raise the peak — that is the apex test's job, not this one's. */
static uint8_t dropAndGetHealth(float top, float bottom, int bx, int bz)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)bx + 0.5f, top, (float)bz + 0.5f);

	const float mid = (top + bottom) * 0.5f;
	const float air[3] = { top, mid, bottom + 0.25f };
	runFall(&ft, &s, &b, air, 3, bottom);
	return s.health;
}

/* ── 1. fall damage: the threshold ───────────────────────────────────────────────────── */

static void testFallThresholds(void)
{
	/* damage = floor(peak_y - landing_y) - 3, applied only if positive. The three rows either
	 * side of the threshold are all here in one place, because a threshold written with the
	 * wrong comparison or the wrong constant moves exactly one of them. */
	static const struct { float drop; int expect_health; const char* what; } cases[] = {
		{ 1.0f, 20, "1-block fall is free"          },
		{ 2.0f, 20, "2-block fall is free"          },
		{ 3.0f, 20, "3-block fall is EXACTLY free"  },
		{ 4.0f, 19, "4-block fall costs 1"          },
		{ 5.0f, 18, "5-block fall costs 2"          },
		{ 10.0f, 13, "10-block fall costs 7"        },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const float top    = 64.0f;
		const uint8_t got  = dropAndGetHealth(top, top - cases[i].drop, DRY_X, DRY_Z);
		g_checks++;
		if (got != (uint8_t)cases[i].expect_health) {
			g_fails++;
			printf("  FAIL  %s: health got %u, want %d\n",
			       cases[i].what, (unsigned)got, cases[i].expect_health);
		}
	}
}

/* A fall of 3.9 blocks is a fall of 3 for damage purposes — floor(), not round(). A rounding
 * implementation passes every integer case above and fails only this one. */
static void testFractionalDropFloors(void)
{
	CHECK_I(dropAndGetHealth(64.0f, 60.1f, DRY_X, DRY_Z), 20);   /* 3.9 -> floor 3 -> free */
	CHECK_I(dropAndGetHealth(64.0f, 59.9f, DRY_X, DRY_Z), 19);   /* 4.1 -> floor 4 -> 1     */
}

/* Walking is free. Without this, an implementation that scored a "fall" on every grounded
 * frame would still pass every check above, because a fall of zero is under the threshold —
 * but the FallTrack would be resetting constantly and the apex test would be the only thing
 * that noticed. Cheap check, real gap. */
static void testWalkingOnTheGroundIsFree(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)DRY_X + 0.5f, 64.0f, (float)DRY_Z + 0.5f);
	b.on_ground = true;

	for (int i = 0; i < 600; i++) {
		b.y = 64.0f;
		CHECK(!fallDamageUpdate(&ft, &s, &b, &s_world));
	}
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);
	CHECK(!ft.falling);
}

/* ── 2. THE APEX ─────────────────────────────────────────────────────────────────────────
 *
 * The case the module comment calls out. on_ground goes false at the BOTTOM of a jump, so a
 * peak captured at that instant is the take-off height and the rise is missing from the
 * measurement.
 *
 * Numbers chosen so the two implementations differ by a whole point of health rather than by
 * a fraction that floor() would swallow: leave the ground at 64, rise to 65, land at 60.
 *   correct   peak 65, drop 5, damage 5-3 = 2, health 18
 *   captured  peak 64, drop 4, damage 4-3 = 1, health 19
 * A single point, which is why it needs an explicit check and would never be noticed in play.
 */
static void testJumpThenFallIsMeasuredFromTheApex(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)DRY_X + 0.5f, 64.0f, (float)DRY_Z + 0.5f);

	/* on_ground false at 64 (the jump), rising through 64.5 to the apex at 65, then down. */
	const float air[6] = { 64.0f, 64.5f, 65.0f, 64.2f, 62.0f, 60.5f };
	runFall(&ft, &s, &b, air, 6, 60.0f);

	CHECK_I(ft.peak_y, 65);        /* the apex, not the take-off height */
	CHECK_I(s.health, 18);         /* 19 means the rise was not counted */
	CHECK(!ft.falling);            /* landing clears the track */
}

/* The same rule from the other side: a body that only ever descends must NOT have its peak
 * dragged down by the fmaxf. If peak_y were assigned every frame instead of maxed, this reads
 * the last airborne height and scores a fall of almost nothing. */
static void testPeakIsNotDraggedDownByLaterFrames(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)DRY_X + 0.5f, 80.0f, (float)DRY_Z + 0.5f);

	const float air[4] = { 80.0f, 76.0f, 72.0f, 68.0f };
	runFall(&ft, &s, &b, air, 4, 64.0f);

	CHECK_I(ft.peak_y, 80);
	CHECK_I(s.health, 20 - (16 - 3));   /* drop 16, damage 13, health 7 */
}

/* ── 3. water negates the landing ────────────────────────────────────────────────────── */

static void testWaterNegatesFallDamage(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)WET_X + 0.5f, 64.0f, (float)WET_Z + 0.5f);

	/* Land at y=20, inside the water column (0..39), from a height that would otherwise be
	 * comfortably lethal on land. */
	const float air[3] = { 64.0f, 50.0f, 30.0f };
	const bool died = runFall(&ft, &s, &b, air, 3, 20.0f);

	CHECK(!died);
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);
	CHECK(bodySubmerged(&s_world, &b));   /* the fixture really is wet */
	CHECK(!ft.falling);                   /* and the track was still cleared */
}

/* The control that gives the water check its meaning: the SAME fall in the SAME world at the
 * dry column is lethal. Without this, "no damage in water" would also be satisfied by a build
 * with fall damage switched off entirely. */
static void testTheSameFallOnLandIsLethal(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)DRY_X + 0.5f, 64.0f, (float)DRY_Z + 0.5f);

	const float air[3] = { 64.0f, 50.0f, 30.0f };
	const bool died = runFall(&ft, &s, &b, air, 3, 20.0f);

	CHECK(died);                 /* drop 44, damage 41, well past 20 */
	CHECK_I(s.health, 0);
	CHECK(!bodySubmerged(&s_world, &b));
}

/* Fall damage CAN kill, and the "just died" edge fires exactly once. A caller hanging a death
 * screen off this return value must not get it again on the next frame. */
static void testDeathEdgeFiresOnce(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	bodyInit(&b, (float)DRY_X + 0.5f, 64.0f, (float)DRY_Z + 0.5f);

	const float air[2] = { 64.0f, 40.0f };
	CHECK(runFall(&ft, &s, &b, air, 2, 20.0f));
	CHECK_I(s.health, 0);

	/* A second identical fall on an already-dead player reports nothing new. */
	CHECK(!runFall(&ft, &s, &b, air, 2, 20.0f));
	CHECK_I(s.health, 0);
}

/* An exactly-fatal fall lands on 0, not on 1: this is the floor starvation has and fall damage
 * does not, checked from the fall side. Health 20, so a drop of 23 blocks is damage 20. */
static void testFallDamageHasNoFloor(void)
{
	CHECK_I(dropAndGetHealth(64.0f, 41.0f, DRY_X, DRY_Z), 0);    /* drop 23, damage 20 */
	CHECK_I(dropAndGetHealth(64.0f, 42.0f, DRY_X, DRY_Z), 1);    /* drop 22, damage 19 */
}

/* ── 4. the hunger clock ─────────────────────────────────────────────────────────────── */

static void testHungerDrainsOnTheExactTick(void)
{
	Survival s;
	survivalInit(&s);

	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);

	/* One tick short. A counter that is off by one fires here, and only this check sees it. */
	for (int i = 0; i < SURVIVAL_HUNGER_PERIOD - 1; i++) CHECK(!survivalTick(&s));
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);

	CHECK(!survivalTick(&s));                 /* the 1200th */
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER - 1);

	/* And it re-arms for a full period rather than firing every tick from here on. */
	for (int i = 0; i < SURVIVAL_HUNGER_PERIOD - 1; i++) survivalTick(&s);
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER - 1);
	survivalTick(&s);
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER - 2);
}

/* Hunger stops at 0 and does not wrap. uint8_t underflow would put it at 255, which every
 * regen and starvation branch would then read as "well fed". */
static void testHungerFloorsAtZeroWithoutWrapping(void)
{
	Survival s;
	survivalInit(&s);

	/* 20 drains empties it; run 30 periods so it sits at 0 for ten more. */
	for (int i = 0; i < SURVIVAL_HUNGER_PERIOD * 30; i++) survivalTick(&s);
	CHECK_I(s.hunger, 0);
}

/* ── 5. regen ────────────────────────────────────────────────────────────────────────── */

static void testRegenFiresOnlyAtOrAboveEighteen(void)
{
	static const struct { uint8_t hunger; bool expect_regen; } cases[] = {
		{ 20, true  },
		{ 19, true  },
		{ 18, true  },   /* the boundary: a `>` instead of `>=` flips exactly this row */
		{ 17, false },
		{ 10, false },
		{  1, false },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		Survival s;
		survivalInit(&s);
		s.health = 10;
		s.hunger = cases[i].hunger;

		for (int t = 0; t < SURVIVAL_REGEN_PERIOD; t++) CHECK(!survivalTick(&s));

		const uint8_t want = cases[i].expect_regen ? 11 : 10;
		g_checks++;
		if (s.health != want) {
			g_fails++;
			printf("  FAIL  regen at hunger %u: health got %u, want %u\n",
			       (unsigned)cases[i].hunger, (unsigned)s.health, (unsigned)want);
		}
	}
}

static void testRegenTimingAndCap(void)
{
	Survival s;
	survivalInit(&s);
	s.health = 10;

	/* One tick short of the period. */
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD - 1; i++) survivalTick(&s);
	CHECK_I(s.health, 10);
	survivalTick(&s);
	CHECK_I(s.health, 11);

	/* Run long enough to overshoot 20 and confirm the cap holds: health is at 11, nine more
	 * regens reach 20, and this runs twenty.
	 *
	 * The length is bounded from ABOVE as well, and that bound is the whole reason this is 20
	 * and not the 50 first written here. Regen is gated on hunger >= 18, and hunger drains a
	 * point every 1200 ticks — so a run long enough to drain three points switches the thing
	 * under test OFF part way through, and the check then passes for a reason that has nothing
	 * to do with the cap. Measured: at 50 periods (4080 ticks total) hunger ended at 17 and
	 * the premise assertion below went red. 20 periods is 1680 ticks, one drain, hunger 19. */
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD * 20; i++) survivalTick(&s);
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);
	CHECK(s.hunger >= SURVIVAL_REGEN_HUNGER);   /* the premise of the check above */
}

/* Entering the regen band starts a FULL period, rather than inheriting whatever was left on a
 * stale countdown from before hunger dropped out of the band. */
static void testRegenCounterReArmsInTheDeadBand(void)
{
	Survival s;
	survivalInit(&s);
	s.health = 10;
	s.hunger = 20;

	/* 79 ticks: one short of a regen. */
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD - 1; i++) survivalTick(&s);
	CHECK_I(s.health, 10);

	/* Drop out of the band, wait, come back. The one remaining tick must NOT be honoured. */
	s.hunger = 10;
	for (int i = 0; i < 500; i++) survivalTick(&s);
	CHECK_I(s.health, 10);

	s.hunger = 20;
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD - 1; i++) survivalTick(&s);
	CHECK_I(s.health, 10);        /* still short — the counter restarted */
	survivalTick(&s);
	CHECK_I(s.health, 11);
}

/* ── 6. starvation, and the floor that makes it survivable ───────────────────────────── */

static void testStarvationFloorsAtOneAndNeverKills(void)
{
	Survival s;
	survivalInit(&s);
	s.hunger = 0;
	s.health = SURVIVAL_MAX_HEALTH;

	/* One short of the first starvation tick. */
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD - 1; i++) CHECK(!survivalTick(&s));
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);
	CHECK(!survivalTick(&s));
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH - 1);

	/* 19 drops would reach 1; run 200 periods, ten times more than enough to reach 0 if the
	 * floor were missing. survivalTick must never once report a death. */
	bool ever_died = false;
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD * 200; i++)
		if (survivalTick(&s)) ever_died = true;

	CHECK(!ever_died);
	CHECK_I(s.health, 1);
	CHECK_I(s.hunger, 0);
}

/* Starvation must not run while hunger is merely LOW — only at exactly 0. Hunger 1 is the row
 * that a `<=` instead of `==` would break. */
static void testHungerOneDoesNotStarve(void)
{
	Survival s;
	survivalInit(&s);
	s.hunger = 1;
	s.health = 10;

	/* Short of the 1200-tick hunger drain, so hunger stays at 1 for the whole run. */
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD * 10; i++) survivalTick(&s);
	CHECK_I(s.hunger, 1);
	CHECK_I(s.health, 10);
}

/* The last point can only be taken by a fall. Starve to the floor, then drop 4 blocks. */
static void testAFallCanFinishAStarvingPlayer(void)
{
	Survival  s;
	FallTrack ft;
	Body      b;

	survivalInit(&s);
	fallTrackInit(&ft);
	s.hunger = 0;
	for (int i = 0; i < SURVIVAL_REGEN_PERIOD * 200; i++) survivalTick(&s);
	CHECK_I(s.health, 1);

	bodyInit(&b, (float)DRY_X + 0.5f, 64.0f, (float)DRY_Z + 0.5f);
	const float air[2] = { 64.0f, 62.0f };
	CHECK(runFall(&ft, &s, &b, air, 2, 60.0f));   /* drop 4, damage 1 */
	CHECK_I(s.health, 0);
}

/* ── 7. food ─────────────────────────────────────────────────────────────────────────── */

static void testFoodTable(void)
{
	CHECK_I(survivalFoodValue(BLOCK_APPLE), 4);

	/* v1.8.14 "Animals". Four raw meats, ids 34..37, named one per line and typed from the
	 * intent rather than read back off kFoods[]. Two tiers, and the equality is deliberate:
	 * the two big mammals are worth 3 and the two smaller animals 2. See world/survival.c's
	 * table comment for why equal values are right HERE and wrong for hardness next door.
	 *
	 * Every one is strictly below the apple's 4, and that is the claim worth pinning rather
	 * than the individual numbers: it is the headroom v1.8.15's cooked meat needs, since a
	 * cooked cut must restore strictly more than the raw one it came from and
	 * SURVIVAL_MAX_HUNGER is only 20. The three inequalities below go red the day somebody
	 * raises raw meat to the apple's level and quietly spends that headroom. */
	CHECK_I(survivalFoodValue(BLOCK_RAW_PORKCHOP), 3);
	CHECK_I(survivalFoodValue(BLOCK_RAW_BEEF), 3);
	CHECK_I(survivalFoodValue(BLOCK_RAW_CHICKEN), 2);
	CHECK_I(survivalFoodValue(BLOCK_RAW_MUTTON), 2);
	CHECK(survivalFoodValue(BLOCK_RAW_PORKCHOP) < survivalFoodValue(BLOCK_APPLE));
	CHECK(survivalFoodValue(BLOCK_RAW_BEEF) < survivalFoodValue(BLOCK_APPLE));
	CHECK(survivalFoodValue(BLOCK_RAW_CHICKEN) < survivalFoodValue(BLOCK_RAW_PORKCHOP));
	CHECK(survivalFoodValue(BLOCK_RAW_MUTTON) < survivalFoodValue(BLOCK_RAW_BEEF));

	/* v1.8.15 "Furnace". The four cooked cuts, typed from intent like the raw ones above.
	 *
	 * The four RELATIONS below matter more than the four values, and they are the whole
	 * reason the furnace is worth building: each cooked cut must restore strictly more than
	 * the raw one it came from. A furnace that lights, burns fuel and hands back something no
	 * better than what went in is a version that has landed unreachable — it would pass every
	 * test furnace.c owns, because none of those tests can see this table. These four lines
	 * are the only place in the suite where "cooking is worth doing" is actually asserted. */
	CHECK_I(survivalFoodValue(BLOCK_COOKED_PORKCHOP), 8);
	CHECK_I(survivalFoodValue(BLOCK_COOKED_BEEF), 8);
	CHECK_I(survivalFoodValue(BLOCK_COOKED_CHICKEN), 6);
	CHECK_I(survivalFoodValue(BLOCK_COOKED_MUTTON), 6);
	CHECK(survivalFoodValue(BLOCK_COOKED_PORKCHOP) > survivalFoodValue(BLOCK_RAW_PORKCHOP));
	CHECK(survivalFoodValue(BLOCK_COOKED_BEEF)     > survivalFoodValue(BLOCK_RAW_BEEF));
	CHECK(survivalFoodValue(BLOCK_COOKED_CHICKEN)  > survivalFoodValue(BLOCK_RAW_CHICKEN));
	CHECK(survivalFoodValue(BLOCK_COOKED_MUTTON)   > survivalFoodValue(BLOCK_RAW_MUTTON));

	/* And the ceiling, which is the constraint the numbers above were chosen against: no
	 * single item may restore the whole bar. A cooked cut worth SURVIVAL_MAX_HUNGER would
	 * make hunger decorative — eat once, never think about it again — so this goes red on a
	 * generous re-balance rather than letting one land silently. */
	CHECK(survivalFoodValue(BLOCK_COOKED_PORKCHOP) < SURVIVAL_MAX_HUNGER);
	CHECK(survivalFoodValue(BLOCK_COOKED_BEEF) < SURVIVAL_MAX_HUNGER);

	/* Everything else this build ships is not food. Spot-checked across the id space rather
	 * than exhaustively, plus air and an undefined high id. */
	CHECK_I(survivalFoodValue(BLOCK_AIR), 0);
	CHECK_I(survivalFoodValue(BLOCK_STONE), 0);
	CHECK_I(survivalFoodValue(BLOCK_DIRT), 0);
	CHECK_I(survivalFoodValue(BLOCK_WATER), 0);
	CHECK_I(survivalFoodValue((BlockId)0xFE), 0);

	/* And nothing else in the whole 0..255 range is food. A table with a stray row, or a
	 * lookup that fell through to a default, shows up here and nowhere else.
	 *
	 * 1 -> 5 on 2026-09-03, v1.8.14 "Animals": the apple plus the four raw meats. This is the
	 * check that would catch a fifth meat row added by accident, or a cooked row landing a
	 * version early, so it moves by exactly the number of rows added and never by "whatever
	 * makes it pass".
	 *
	 * 5 -> 9 on 2026-09-03, v1.8.15 "Furnace": the four cooked cuts. Exactly four, which is
	 * the point — the check above named them individually, and this one says there is nothing
	 * ELSE now edible. A furnace recipe that accidentally routed some third block into the
	 * food table would be invisible to every named check and visible only here. */
	int food_ids = 0;
	for (int id = 0; id <= 0xFF; id++)
		if (survivalFoodValue((BlockId)id) != 0) food_ids++;
	CHECK_I(food_ids, 9);
}

static void testEatingAnAppleConsumesExactlyOne(void)
{
	Survival  s;
	Inventory inv;

	survivalInit(&s);
	inventoryInit(&inv);
	s.hunger = 10;

	/* Two stacks, so "consumes from the slot it was given" is a real claim and not a
	 * coincidence of there being only one. Slot 0 is the one inventoryRemove() would have
	 * eaten from; slot 5 is the one the caller asked for. */
	CHECK(inventoryAdd(&inv, BLOCK_APPLE, 3, NULL) == INV_ADD_OK);
	inv.slots[5].item  = BLOCK_APPLE;
	inv.slots[5].count = 7;

	CHECK(survivalEat(&s, &inv, 5));

	CHECK_I(s.hunger, 14);                    /* exactly +4 */
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);   /* eating never heals */
	CHECK_I(inv.slots[5].count, 6);           /* exactly one unit */
	CHECK_I(inv.slots[5].item, BLOCK_APPLE);
	CHECK_I(inv.slots[0].count, 3);           /* the other stack is untouched */
	CHECK_I(inv.slots[0].item, BLOCK_APPLE);
}

/* A slot emptied by the last bite goes back to { ITEM_NONE, 0 } — never a stale id with a zero
 * count, which is the invariant inventory.h states. */
static void testLastAppleEmptiesTheSlotCleanly(void)
{
	Survival  s;
	Inventory inv;

	survivalInit(&s);
	inventoryInit(&inv);
	s.hunger = 10;
	inv.slots[2].item  = BLOCK_APPLE;
	inv.slots[2].count = 1;

	CHECK(survivalEat(&s, &inv, 2));
	CHECK_I(inv.slots[2].count, 0);
	CHECK_I(inv.slots[2].item, ITEM_NONE);

	/* And the now-empty slot is not edible. */
	CHECK(!survivalEat(&s, &inv, 2));
	CHECK_I(s.hunger, 14);
}

static void testEatingAtFullHungerRefusesAndChangesNothing(void)
{
	Survival  s;
	Inventory inv;

	survivalInit(&s);
	inventoryInit(&inv);
	s.health = 5;                     /* damaged, so "does not heal instead" is checkable */
	inv.slots[3].item  = BLOCK_APPLE;
	inv.slots[3].count = 9;

	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);
	CHECK(!survivalEat(&s, &inv, 3));

	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);
	CHECK_I(s.health, 5);
	CHECK_I(inv.slots[3].count, 9);   /* nothing consumed */
	CHECK_I(inv.slots[3].item, BLOCK_APPLE);
}

/* Hunger 17 + an apple's 4 is 21, which must land on 20 rather than wrapping or overshooting.
 * The row above refuses at 20; this one is the partial-overflow case that still succeeds. */
static void testEatingCapsHungerAtTwenty(void)
{
	Survival  s;
	Inventory inv;

	survivalInit(&s);
	inventoryInit(&inv);
	s.hunger = 17;
	inv.slots[1].item  = BLOCK_APPLE;
	inv.slots[1].count = 2;

	CHECK(survivalEat(&s, &inv, 1));
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);
	CHECK_I(inv.slots[1].count, 1);
}

static void testEatingANonFoodIsANoOp(void)
{
	Survival  s;
	Inventory inv;

	survivalInit(&s);
	inventoryInit(&inv);
	s.hunger = 10;
	inv.slots[4].item  = BLOCK_STONE;
	inv.slots[4].count = 12;

	CHECK(!survivalEat(&s, &inv, 4));
	CHECK_I(s.hunger, 10);
	CHECK_I(inv.slots[4].count, 12);
	CHECK_I(inv.slots[4].item, BLOCK_STONE);

	/* An empty slot, and an out-of-range slot index, are both refusals rather than reads off
	 * the end of the array. */
	CHECK(!survivalEat(&s, &inv, 6));
	CHECK(!survivalEat(&s, &inv, INV_SLOT_COUNT));
	CHECK(!survivalEat(&s, &inv, 255));
	CHECK_I(s.hunger, 10);
}

/* ── 8. the file format ──────────────────────────────────────────────────────────────── */

#define SURV_DIR "build-host/t-survdir"

static const char* survFile(void) { return SURV_DIR "/survival.dat"; }
static const char* survTmp(void)  { return SURV_DIR "/survival.dat.tmp"; }

static void freshDir(void)
{
	mkdir("build-host", 0777);
	mkdir(SURV_DIR, 0777);
	remove(survFile());
	remove(survTmp());
}

static bool writeBytes(const char* path, const uint8_t* buf, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static size_t readBytes(const char* path, uint8_t* buf, size_t cap)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	const size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

static bool fileExists(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

/* A hand-built 14-byte file, so the corruption cases can be exact. Deliberately NOT built by
 * calling survivalSave and then poking the result: the point of several checks below is that a
 * specific byte is wrong, and a helper that shares the writer's idea of where that byte is
 * would move with it. This encodes the layout from survival.h's comment independently, so a
 * load that starts reading the wrong offset is caught rather than tracked. */
#define SURV_BYTES 14

static void put32le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void buildFile(uint8_t* buf, uint32_t magic, uint32_t version,
                      uint8_t health, uint8_t hunger)
{
	buf[0x0C] = health;
	buf[0x0D] = hunger;
	put32le(buf + 0x00, magic);
	put32le(buf + 0x04, version);
	put32le(buf + 0x08, crc32(buf + 0x0C, SURV_BYTES - 0x0C));
}

/* "BSV1" little-endian, spelled out here rather than taken from the module, so a magic changed
 * in survival.c without meaning to is caught here instead of agreeing with itself. */
#define EXPECT_MAGIC 0x31565342u

/* The sentinel the out-parameter is filled with before every refusal case. "false leaves *out
 * untouched" is half the contract — a caller that keeps its own survivalInit() defaults on
 * false is reading a struct the load may have half-written otherwise. */
static const Survival kUntouched = { 111, 222, 3333, 4444 };

static bool survEqual(const Survival* a, const Survival* b)
{
	return a->health == b->health && a->hunger == b->hunger &&
	       a->hunger_ticks == b->hunger_ticks && a->regen_ticks == b->regen_ticks;
}

static void expectRefused(const char* what)
{
	Survival out = kUntouched;
	const bool ok = survivalLoad(&out, SURV_DIR);
	g_checks++;
	if (ok) { g_fails++; printf("  FAIL  refused: %s (load returned true)\n", what); }
	g_checks++;
	if (!survEqual(&out, &kUntouched)) {
		g_fails++;
		printf("  FAIL  untouched: %s (out was written)\n", what);
	}
}

static void testRoundTrip(void)
{
	freshDir();

	Survival in;
	survivalInit(&in);
	in.health       = 7;
	in.hunger       = 13;
	in.hunger_ticks = 999;    /* deliberately mid-cycle: these must NOT come back */
	in.regen_ticks  = 42;

	CHECK(survivalSave(&in, SURV_DIR));

	Survival out = kUntouched;
	CHECK(survivalLoad(&out, SURV_DIR));
	CHECK_I(out.health, 7);
	CHECK_I(out.hunger, 13);
	CHECK_I(out.hunger_ticks, SURVIVAL_HUNGER_PERIOD);   /* reset, not restored */
	CHECK_I(out.regen_ticks, SURVIVAL_REGEN_PERIOD);

	/* The file the writer actually produced is the layout the header documents. Without this,
	 * a writer and a reader that agree with each other on a wrong offset round-trip perfectly
	 * and nothing notices until a save from a different build is read. */
	uint8_t got[SURV_BYTES + 8];
	const size_t n = readBytes(survFile(), got, sizeof got);
	CHECK_I(n, SURV_BYTES);
	if (n == SURV_BYTES) {
		uint8_t want[SURV_BYTES];
		buildFile(want, EXPECT_MAGIC, 1u, 7, 13);
		CHECK(memcmp(got, want, SURV_BYTES) == 0);
	}

	/* The tmp file is gone once the save has landed — a leftover would be promoted by the
	 * recover pass the next time a real file went missing. */
	CHECK(!fileExists(survTmp()));
}

/* Both ends of the legal range survive a round trip, including a health of 0. Whether a dead
 * player should be persisted as dead is a GAME decision that has not been made; this file
 * stores the documented 0..20 range verbatim and invents no respawn rule of its own. */
static void testRangeEndsRoundTrip(void)
{
	static const struct { uint8_t health, hunger; } cases[] = {
		{ 0, 0 }, { 0, 20 }, { 20, 0 }, { 20, 20 }, { 1, 19 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		freshDir();
		Survival in;
		survivalInit(&in);
		in.health = cases[i].health;
		in.hunger = cases[i].hunger;
		CHECK(survivalSave(&in, SURV_DIR));

		Survival out = kUntouched;
		CHECK(survivalLoad(&out, SURV_DIR));
		CHECK_I(out.health, cases[i].health);
		CHECK_I(out.hunger, cases[i].hunger);
	}
}

static void testMissingFileIsRefused(void)
{
	freshDir();
	expectRefused("no survival.dat at all");
}

static void testCorruptCrcIsRefused(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, 7, 13);
	buf[0x0C] ^= 0x01u;   /* one bit inside health, after the crc was computed */
	CHECK(writeBytes(survFile(), buf, SURV_BYTES));
	expectRefused("one payload bit flipped");
}

/* The crc byte itself corrupted, rather than the payload — the other side of the same check. */
static void testCorruptChecksumFieldIsRefused(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, 7, 13);
	buf[0x08] ^= 0xFFu;
	CHECK(writeBytes(survFile(), buf, SURV_BYTES));
	expectRefused("checksum field corrupted");
}

static void testBadMagicIsRefused(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES];
	buildFile(buf, 0x31505342u /* "BSP1", the pose sidecar's */, 1u, 7, 13);
	CHECK(writeBytes(survFile(), buf, SURV_BYTES));
	expectRefused("magic BSP1");
}

static void testBadVersionIsRefused(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES];
	buildFile(buf, EXPECT_MAGIC, 2u, 7, 13);
	CHECK(writeBytes(survFile(), buf, SURV_BYTES));
	expectRefused("version 2");
}

static void testShortAndLongFilesAreRefused(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES + 1];
	buildFile(buf, EXPECT_MAGIC, 1u, 7, 13);
	CHECK(writeBytes(survFile(), buf, SURV_BYTES - 1));
	expectRefused("13 bytes");

	freshDir();
	buildFile(buf, EXPECT_MAGIC, 1u, 7, 13);
	buf[SURV_BYTES] = 0x00;
	CHECK(writeBytes(survFile(), buf, SURV_BYTES + 1));
	expectRefused("15 bytes");
}

/* The range check the checksum cannot do: a perfectly checksummed file whose health byte is
 * out of range. This is what stands between a save written by a build with different limits
 * (or a deliberately edited file) and a health bar with 200 hearts on it. */
static void testOutOfRangeValuesAreRefused(void)
{
	static const struct { uint8_t health, hunger; const char* what; } cases[] = {
		{ 21,  10, "health 21"  },
		{ 255, 10, "health 255" },
		{ 10,  21, "hunger 21"  },
		{ 10, 255, "hunger 255" },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		freshDir();
		uint8_t buf[SURV_BYTES];
		buildFile(buf, EXPECT_MAGIC, 1u, cases[i].health, cases[i].hunger);
		CHECK(writeBytes(survFile(), buf, SURV_BYTES));
		expectRefused(cases[i].what);
	}
}

static void testNullAndEmptyDirs(void)
{
	freshDir();

	Survival s;
	survivalInit(&s);

	CHECK(!survivalSave(&s, NULL));
	CHECK(!survivalSave(&s, ""));
	CHECK(!survivalSave(NULL, SURV_DIR));
	CHECK(!fileExists(survFile()));
	CHECK(!fileExists(survTmp()));

	Survival out = kUntouched;
	CHECK(!survivalLoad(&out, NULL));
	CHECK(!survivalLoad(&out, ""));
	CHECK(survEqual(&out, &kUntouched));

	/* The NULL-out check goes LAST and against a directory that really does hold a valid file,
	 * which is the difference between a check and a decoration: called against an empty
	 * directory it would return false at the fopen, long before it reached the store. */
	CHECK(survivalSave(&s, SURV_DIR));
	CHECK(!survivalLoad(NULL, SURV_DIR));
}

/* ── 9. the interrupted-save window ──────────────────────────────────────────────────── */

static void testTmpIsPromotedWhenTheRealFileIsGone(void)
{
	freshDir();
	uint8_t buf[SURV_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, 7, 13);
	CHECK(writeBytes(survTmp(), buf, SURV_BYTES));   /* power cut between remove and rename */

	Survival out = kUntouched;
	CHECK(survivalLoad(&out, SURV_DIR));
	CHECK_I(out.health, 7);
	CHECK_I(out.hunger, 13);
	CHECK(fileExists(survFile()));
	CHECK(!fileExists(survTmp()));
}

static void testAGoodRealFileBeatsALeftoverTmp(void)
{
	freshDir();

	Survival real;
	survivalInit(&real);
	real.health = 3;
	real.hunger = 4;
	CHECK(survivalSave(&real, SURV_DIR));

	uint8_t stale[SURV_BYTES];
	buildFile(stale, EXPECT_MAGIC, 1u, 19, 19);
	CHECK(writeBytes(survTmp(), stale, SURV_BYTES));

	Survival out = kUntouched;
	CHECK(survivalLoad(&out, SURV_DIR));
	CHECK_I(out.health, 3);
	CHECK_I(out.hunger, 4);
	CHECK(!fileExists(survTmp()));
}

/* A save over an existing save replaces it — the remove-before-rename path, which is the one
 * Windows' rename() would otherwise fail on. Every quit after the first takes this path. */
static void testResaveReplacesTheOldFile(void)
{
	freshDir();

	Survival a;
	survivalInit(&a);
	a.health = 2;
	a.hunger = 2;
	CHECK(survivalSave(&a, SURV_DIR));

	Survival b;
	survivalInit(&b);
	b.health = 17;
	b.hunger = 6;
	CHECK(survivalSave(&b, SURV_DIR));

	Survival out = kUntouched;
	CHECK(survivalLoad(&out, SURV_DIR));
	CHECK_I(out.health, 17);
	CHECK_I(out.hunger, 6);
	CHECK(!fileExists(survTmp()));
}

/* ── 10. init ────────────────────────────────────────────────────────────────────────── */

static void testInitDefaults(void)
{
	Survival s;
	memset(&s, 0xAB, sizeof s);
	survivalInit(&s);
	CHECK_I(s.health, SURVIVAL_MAX_HEALTH);
	CHECK_I(s.hunger, SURVIVAL_MAX_HUNGER);
	CHECK_I(s.hunger_ticks, SURVIVAL_HUNGER_PERIOD);
	CHECK_I(s.regen_ticks, SURVIVAL_REGEN_PERIOD);

	FallTrack ft;
	memset(&ft, 0xAB, sizeof ft);
	fallTrackInit(&ft);
	CHECK(!ft.falling);

	/* Every entry point survives a NULL rather than dereferencing it. */
	survivalInit(NULL);
	fallTrackInit(NULL);
	CHECK(!survivalTick(NULL));
	CHECK(!survivalEat(NULL, NULL, 0));
	CHECK(!survivalSave(NULL, SURV_DIR));
	CHECK(!survivalLoad(NULL, SURV_DIR));
}

/* A Survival that skipped survivalInit — a zeroed struct, the shape a memset player record
 * arrives in — must not mute the clocks for 65535 ticks by underflowing a uint16_t. */
static void testZeroedStructDoesNotUnderflowTheClocks(void)
{
	Survival s;
	memset(&s, 0, sizeof s);
	s.health = 10;
	s.hunger = 20;

	survivalTick(&s);
	CHECK(s.hunger_ticks > 0 && s.hunger_ticks <= SURVIVAL_HUNGER_PERIOD);
	CHECK(s.regen_ticks  > 0 && s.regen_ticks  <= SURVIVAL_REGEN_PERIOD);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== survival test (v1.8.13) ==");

	registryInitCore();
	worldInit(&s_world);
	fillWaterColumn();

	testInitDefaults();
	testZeroedStructDoesNotUnderflowTheClocks();

	testFallThresholds();
	testFractionalDropFloors();
	testWalkingOnTheGroundIsFree();
	testJumpThenFallIsMeasuredFromTheApex();
	testPeakIsNotDraggedDownByLaterFrames();
	testWaterNegatesFallDamage();
	testTheSameFallOnLandIsLethal();
	testDeathEdgeFiresOnce();
	testFallDamageHasNoFloor();

	testHungerDrainsOnTheExactTick();
	testHungerFloorsAtZeroWithoutWrapping();

	testRegenFiresOnlyAtOrAboveEighteen();
	testRegenTimingAndCap();
	testRegenCounterReArmsInTheDeadBand();

	testStarvationFloorsAtOneAndNeverKills();
	testHungerOneDoesNotStarve();
	testAFallCanFinishAStarvingPlayer();

	testFoodTable();
	testEatingAnAppleConsumesExactlyOne();
	testLastAppleEmptiesTheSlotCleanly();
	testEatingAtFullHungerRefusesAndChangesNothing();
	testEatingCapsHungerAtTwenty();
	testEatingANonFoodIsANoOp();

	testRoundTrip();
	testRangeEndsRoundTrip();
	testMissingFileIsRefused();
	testCorruptCrcIsRefused();
	testCorruptChecksumFieldIsRefused();
	testBadMagicIsRefused();
	testBadVersionIsRefused();
	testShortAndLongFilesAreRefused();
	testOutOfRangeValuesAreRefused();
	testNullAndEmptyDirs();

	testTmpIsPromotedWhenTheRealFileIsGone();
	testAGoodRealFileBeatsALeftoverTmp();
	testResaveReplacesTheOldFile();

	worldExit(&s_world);

	printf("\nsurvival self-test: %s %d checks, %d failed\n",
	       g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
