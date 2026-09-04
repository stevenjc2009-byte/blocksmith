// See entity/monster.h for what each rule is and why it is shaped that way. This is the
// arithmetic. Mirrors entity/animal.c's own structure wherever the two genuinely share
// shape (the yaw convention, the IDLE/WANDER decision machinery, the floor helper) and
// departs from it only where docs/plan-1.8.18-monsters.md says a monster is not an animal:
// CHASE, WINDUP/COOLDOWN, the darkness spawn gate and the torch despawn.
#include "entity/monster.h"

#include <math.h>

#include "world/block.h"
#include "world/light.h"
#include "world/raycast.h"
#include "world/tick.h"

// ---------------------------------------------------------------------------------------
// THE YAW CONVENTION. Read out of animal.c, which reads it out of the shipped code -- see
// that file's own header comment for the two independent sites that agree on it. Repeated
// here rather than shared because animal.c's #define is private to that translation unit.
//
//   forward(yaw) = ( sinf(yaw), -cosf(yaw) )   in (x, z)
//   inverse: the yaw that faces along (dx, dz) is atan2f(dx, -dz), NOT atan2f(dx, dz).
#define MONSTER_TWO_PI 6.28318530717958647692f

// Decision timers for IDLE/WANDER, in TICKS -- the SAME numbers animal.c's own
// ANIMAL_IDLE_*/ANIMAL_WANDER_* constants use. [reasoned]: see monster.h's doc comment on
// monsterThink() for why re-using the shipped, already-tuned feel is the smallest change.
#define MONSTER_IDLE_MIN_TICKS    30   /* 1.5 s */
#define MONSTER_IDLE_SPAN_TICKS   60   /* -> 30..89 ticks, 1.5..4.5 s */
#define MONSTER_WANDER_MIN_TICKS  40   /* 2.0 s */
#define MONSTER_WANDER_SPAN_TICKS 80   /* -> 40..119 ticks, 2..6 s   */
#define MONSTER_WANDER_CHANCE     60   /* percent, matches ANIMAL_WANDER_CHANCE */

// Horizontal speed (|vx| + |vz|, blocks/s) below which a WANDER or CHASE monster counts as
// stuck. Identical value and identical reasoning to animal.c's ANIMAL_STUCK_SPEED: bodyMove
// zeroes the blocked axis outright, so the real value after a head-on wall hit is exactly
// 0, and the fastest monster this table has is 2.0, so there is still two orders of
// magnitude of margin.
#define MONSTER_STUCK_SPEED 0.01f

// Ticks of decision-timer jitter given to a freshly spawned monster, matching
// ANIMAL_SPAWN_JITTER_TICKS's own reason: so a batch spawned in the same
// monsterSpawnTick() call does not all decide on the same tick.
#define MONSTER_SPAWN_JITTER_TICKS 60

// ---------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------
//
// Indexed by kind - MONSTER_KIND_MIN, not by kind directly -- unlike animal.c's kDefs,
// which is indexed by kind because ENT_NONE (0) has to occupy a row. Monster kinds start at
// 5 with no ENT_NONE hole to reserve, so a zero-based table wastes 5 empty rows for nothing.
//
// Every number here is either read directly off the plan (health, damage,
// attack_period_ticks where the plan states it exactly) or is a [reasoned] smallest-
// reasonable choice, marked individually -- see monster.h's MonsterDef doc comment for the
// per-field source.
static const MonsterDef kMonsterDefs[MONSTER_KIND_MAX - MONSTER_KIND_MIN + 1] = {
	/* [ENT_KIND_ZOMBIE]   */ { 20, 3, 20,  0, 0,  0.6f, 1.8f, 2.0f },
	/* [ENT_KIND_SKELETON] */ { 20, 4, 60, 20, 0,  0.5f, 1.8f, 1.6f },
};

const MonsterDef* monsterDef(uint8_t kind)
{
	if (kind < MONSTER_KIND_MIN || kind > MONSTER_KIND_MAX) return NULL;
	return &kMonsterDefs[kind - MONSTER_KIND_MIN];
}

int monsterCapFor(bool is_new_3ds)
{
	return is_new_3ds ? MONSTER_CAP_NEW : MONSTER_CAP_OLD;
}

int monsterCount(const EntityWorld* ew)
{
	if (!ew) return 0;

	int n = 0;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const Entity* e = entityGet(ew, i);
		if (e && monsterDef(e->kind)) n++;
	}
	return n;
}

// The monster sub-cap for a given store, mirroring animal.c's animalCapForStore() exactly:
// the store's own cap (set once at entityWorldInit from entityCapFor(hwIsNew3ds())) already
// IS the hardware answer, so reading it back is exact rather than a second hardware probe
// in a file that must not include <3ds.h>.
static int monsterCapForStore(const EntityWorld* ew)
{
	return monsterCapFor(entityCap(ew) >= ENTITY_CAP_NEW);
}

// floor(), the same three lines entity.c, animal.c, physics.c and world.h all carry for the
// same reason: (int) truncates towards zero, which is wrong for negative non-integers.
static inline int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

// ---------------------------------------------------------------------------------------
// Shared IDLE/WANDER machinery -- animal.c's randomYaw/driveAlongYaw/enterIdle/enterWander,
// re-derived here rather than shared because they are static to that translation unit.
// ---------------------------------------------------------------------------------------

static float randomYaw(Rng* rng)
{
	// 16 bits of angle, same reasoning as animal.c's randomYaw: fine enough granularity,
	// and drawing from a power of two avoids a divide.
	return (float)rngBelow(rng, 1u << 16) * (MONSTER_TWO_PI / 65536.0f);
}

static void driveAlongYaw(Entity* e, float speed)
{
	e->body.vx =  sinf(e->yaw) * speed;
	e->body.vz = -cosf(e->yaw) * speed;
}

static void enterIdleMonster(Entity* e, Rng* rng)
{
	e->ai_state = MONSTER_AI_IDLE;
	e->ai_timer = (uint16_t)(MONSTER_IDLE_MIN_TICKS + rngBelow(rng, MONSTER_IDLE_SPAN_TICKS));
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;
}

static void enterWanderMonster(Entity* e, const MonsterDef* def, Rng* rng)
{
	e->ai_state = MONSTER_AI_WANDER;
	e->ai_timer = (uint16_t)(MONSTER_WANDER_MIN_TICKS + rngBelow(rng, MONSTER_WANDER_SPAN_TICKS));
	e->yaw      = randomYaw(rng);
	driveAlongYaw(e, def->speed);
}

// The IDLE/WANDER decision machinery shared by both kinds when neither is actively
// threatening the player -- animalThink()'s exact shape (mid-decision stuck check,
// decision-expiry switch), lifted out to one function so zombieThink and skeletonThink
// both fall into it identically instead of two hand-copies drifting apart.
static void idleWanderStep(Entity* e, const MonsterDef* def, Rng* rng)
{
	if (e->ai_timer > 0) {
		if (e->ai_state == MONSTER_AI_WANDER &&
		    fabsf(e->body.vx) + fabsf(e->body.vz) < MONSTER_STUCK_SPEED) {
			e->yaw = randomYaw(rng);
			driveAlongYaw(e, def->speed);
		}
		return;
	}

	switch (e->ai_state) {
	case MONSTER_AI_WANDER:
		enterIdleMonster(e, rng);
		break;
	default:
		if (rngBelow(rng, 100) < MONSTER_WANDER_CHANCE) enterWanderMonster(e, def, rng);
		else                                            enterIdleMonster(e, rng);
		break;
	}
}

// ---------------------------------------------------------------------------------------
// Combat / detection
// ---------------------------------------------------------------------------------------

// Eye-to-eye visibility test for the skeleton's fire decision. Casts through worldRaycast()
// (block-grid DDA, world/raycast.c) rather than animalRaycast() (a float AABB slab test
// against entity boxes) -- this is a question about TERRAIN in the way, not about which
// entity a ray enters, and worldRaycast is the mechanism this codebase already has for
// exactly that question (scene/interact.c's aim raycast uses the same function).
static bool skeletonHasLineOfSight(const World* w, const Entity* e, const MonsterCtx* ctx)
{
	const float ox = e->body.x;
	const float oy = e->body.y + e->body.eye;
	const float oz = e->body.z;

	const float dx = ctx->player_x - ox;
	const float dy = ctx->player_eye_y - oy;
	const float dz = ctx->player_z - oz;

	const float dist_sq = dx * dx + dy * dy + dz * dz;
	if (dist_sq < 1.0e-6f) return true;   // degenerate: standing on top of the player

	const float dist = sqrtf(dist_sq);

	// worldRaycast normalises the direction internally and walks out to `dist` -- see
	// world/raycast.c. A miss (nothing solid that near) means an unobstructed line; a hit
	// blocks the shot only if the wall is NEARER than the player, so a hit exactly at the
	// player's own cell (the far wall behind them, at distance >= dist) does not count.
	const RayHit hit = worldRaycast(w, ox, oy, oz, dx, dy, dz, dist);
	if (!hit.hit) return true;
	return hit.distance >= dist;
}

static void zombieThink(Entity* e, const MonsterDef* def, MonsterCtx* ctx)
{
	Rng* rng = ctx->rng;

	const float dx = ctx->player_x - e->body.x;
	const float dz = ctx->player_z - e->body.z;
	const float dist_sq = dx * dx + dz * dz;   // horizontal only, matching tick.h's own
	                                            // distance convention -- see monster.h

	if (dist_sq <= MONSTER_DETECT_RADIUS_SQ) {
		if (e->ai_state != MONSTER_AI_CHASE) {
			// Interrupts whatever IDLE/WANDER decision was pending. ai_timer is repurposed
			// here from "ticks until the next wander decision" to "ticks until the next
			// allowed melee swing" (see monster.h's MonsterCtx comment) -- 0 means ready to
			// swing the instant it closes the distance, so a zombie that just noticed the
			// player does not wait out an artificial delay before its first hit.
			e->ai_state = MONSTER_AI_CHASE;
			e->ai_timer = 0;
		}
	} else if (e->ai_state == MONSTER_AI_CHASE) {
		enterIdleMonster(e, rng);   // player broke detection range -- stop chasing
	}

	if (e->ai_state != MONSTER_AI_CHASE) {
		idleWanderStep(e, def, rng);
		return;
	}

	// CHASE. Yaw is recomputed toward the player and driven every call, which -- because
	// MONSTER_DETECT_RADIUS (16) is inside TICK_NEAR_BLOCKS (24) -- means every tick, per
	// monster.h's own argument. Unlike WANDER's random heading, CHASE always has a real
	// target, so there is no separate "stuck -> pick a new direction" branch to reuse from
	// animalThink(): the wall bounce is the SAME free property animal.c's own comment
	// describes -- bodyMove() resolves x and z independently, so an axis that is not
	// blocked keeps its velocity and the body slides along whatever it hit -- and it falls
	// out of driveAlongYaw()/bodyStep() with no extra code here, which is exactly what
	// plan §3.4 means by "will walk into a wall and slide along it."
	if (dx != 0.0f || dz != 0.0f) e->yaw = atan2f(dx, -dz);
	driveAlongYaw(e, def->speed);

	// Contact melee, arithmetic distance only -- no raycast, plan §3.4. `ai_timer` is the
	// cooldown here (see above); entity.c already decrements it once per due tick before
	// this function runs, at the correct real-world rate since a chasing zombie is always
	// on the full 20 Hz tier.
	const float reach = def->width;   // half the body width each side is roughly the box's
	                                  // own extent; using the full width as the reach keeps
	                                  // the check simple and slightly generous rather than
	                                  // exact, which is the right side to be wrong on for a
	                                  // melee hit at 20 Hz -- [reasoned], the plan gives no
	                                  // exact reach number.
	if (dist_sq <= reach * reach && e->ai_timer == 0) {
		const uint16_t sum = (uint16_t)ctx->player_damage + def->damage;
		ctx->player_damage = (uint8_t)(sum > 255 ? 255 : sum);
		e->ai_timer = def->attack_period_ticks;
	}
}

static void skeletonThink(Entity* e, const World* w, const MonsterDef* def, MonsterCtx* ctx)
{
	Rng* rng = ctx->rng;

	const float dx = ctx->player_x - e->body.x;
	const float dz = ctx->player_z - e->body.z;
	const float dist_sq = dx * dx + dz * dz;
	const bool in_range = dist_sq <= MONSTER_DETECT_RADIUS_SQ;

	const bool attacking =
		(e->ai_state == MONSTER_AI_WINDUP || e->ai_state == MONSTER_AI_COOLDOWN);

	if (!in_range) {
		if (attacking) enterIdleMonster(e, rng);
		idleWanderStep(e, def, rng);
		return;
	}

	// In range: stop advancing and face the player every tick, whichever attack phase this
	// is -- plan §3.5/§6.7, no pathfinding, no closing to melee.
	e->body.vx = 0.0f;
	e->body.vz = 0.0f;
	if (dx != 0.0f || dz != 0.0f) e->yaw = atan2f(dx, -dz);

	if (!attacking) {
		e->ai_state = MONSTER_AI_WINDUP;
		e->ai_timer = def->windup_ticks;
		return;   // telegraphing starts THIS tick; nothing else to do yet
	}

	if (e->ai_state == MONSTER_AI_WINDUP) {
		if (e->ai_timer > 0) return;   // still telegraphing

		// Re-checked here, not assumed from when the windup started -- the whole point of a
		// telegraph the player can react to is that both range and line of sight are real
		// again at the moment it resolves, not at the moment it began.
		if (skeletonHasLineOfSight(w, e, ctx)) {
			const uint16_t sum = (uint16_t)ctx->player_damage + def->damage;
			ctx->player_damage = (uint8_t)(sum > 255 ? 255 : sum);
		}
		e->ai_state = MONSTER_AI_COOLDOWN;
		e->ai_timer = (uint16_t)(def->attack_period_ticks - def->windup_ticks);
		return;
	}

	// MONSTER_AI_COOLDOWN
	if (e->ai_timer > 0) return;
	e->ai_state = MONSTER_AI_WINDUP;
	e->ai_timer = def->windup_ticks;
}

void monsterThink(EntityWorld* ew, int slot, const World* w, float dt_s, void* user)
{
	// Accepted to match EntityThinkFn and unused, for the identical reason animalThink()
	// gives: the decision clock is Entity.ai_timer, in ticks, which entityTick() already
	// counts down.
	(void)dt_s;

	Entity* e = entityAt(ew, slot);
	if (!e) return;

	const MonsterDef* def = monsterDef(e->kind);
	if (!def) return;   // not a monster -- an animal or a free slot, leave it alone

	MonsterCtx* ctx = (MonsterCtx*)user;
	if (!ctx || !ctx->rng) return;   // no decision stream -- documented no-op, see monster.h

	// Torch despawn, first, flat 2 Hz regardless of this entity's own near/far tick period
	// -- see monster.h's monsterThink() doc comment for the argument that this is provably
	// exactly 2 Hz either way.
	if (tickDue(ctx->tick, TICK_FAR_PERIOD, (uint32_t)e->id)) {
		const int bx = floorToInt(e->body.x);
		const int by = floorToInt(e->body.y);
		const int bz = floorToInt(e->body.z);
		Column* col = worldColumn(w, bx >> 4, bz >> 4);
		// col == NULL is not reachable here in practice -- entity.c's own column-unload
		// despawn (entity.h's entityTick doc, step 1) runs BEFORE think() on every tick and
		// removes an entity whose column is gone before this function is ever called for
		// it. Guarded anyway rather than assumed, at the cost of one branch.
		if (col && (lightGetSky(col, bx & 15, by, bz & 15) != 0 ||
		           lightGetBlock(col, bx & 15, by, bz & 15) != 0)) {
			entityKill(ew, slot);
			return;
		}
	}

	if (e->kind == ENT_KIND_ZOMBIE) zombieThink(e, def, ctx);
	else                            skeletonThink(e, w, def, ctx);
}

void entityThinkDispatch(EntityWorld* ew, int slot, const World* w, float dt_s, void* user)
{
	const Entity* e = entityGet(ew, slot);
	if (!e) return;

	EntityDispatchCtx* ctx = (EntityDispatchCtx*)user;
	if (!ctx) return;

	if (e->kind >= MONSTER_KIND_MIN && e->kind <= MONSTER_KIND_MAX) {
		monsterThink(ew, slot, w, dt_s, &ctx->monster);
	} else {
		animalThink(ew, slot, w, dt_s, &ctx->animal);
	}
}

// ---------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------

static uint8_t pickMonsterKind(Rng* rng)
{
	return rngBelow(rng, 2) == 0 ? ENT_KIND_ZOMBIE : ENT_KIND_SKELETON;
}

// Can a monster stand at (x, y, z)? One test, no retry -- animal.c's spotIsGood() does the
// same shape of thing for animals, but with a grass/dirt/sand allow-list at y-1; that list
// is dropped here per plan §3.2 ("no allow-list... generic blockIsSolid()"), because a
// monster spawning in a cave stands on stone, not grass.
static bool monsterSpotIsGood(const World* w, int x, int y, int z)
{
	if (!blockIsSolid(worldGet(w, x, y - 1, z))) return false;
	if (!blockIsAir(worldGet(w, x, y,     z))) return false;
	if (!blockIsAir(worldGet(w, x, y + 1, z))) return false;
	return true;
}

int monsterSpawnTick(EntityWorld* ew, World* w, uint64_t tick, float player_x, float player_y,
                     float player_z, Rng* rng)
{
	if (!ew || !w || !rng) return 0;

	// The stateless period gate. Living HERE rather than as a stored countdown or as a
	// `tick % 40` test at the main.c call site -- see monster.h's monsterSpawnTick() doc
	// comment for why: main.c's own convention (confirmed by direct inspection: no
	// repeating-timer pattern exists anywhere in it) is that every periodic subsystem owns
	// its own period internally and is called unconditionally.
	if (tick % MONSTER_SPAWN_PERIOD_TICKS != 0) return 0;

	const int cap = monsterCapForStore(ew);
	if (monsterCount(ew) >= cap) return 0;

	int spawned = 0;
	for (int c = 0; c < MONSTER_SPAWN_CANDIDATES; c++) {
		// Re-checked per candidate, not once above, so a batch cannot straddle the cap --
		// the identical discipline animalSpawnForColumn() applies per herd member.
		if (monsterCount(ew) >= cap) break;

		// Step 1 (distance band): by CONSTRUCTION, not by reject -- see monster.h's
		// MONSTER_SPAWN_MIN_DIST/MAX_DIST comment. randomYaw() is repurposed here as "any
		// uniform angle", not a facing.
		const float angle = randomYaw(rng);
		const uint32_t radius_steps =
			(uint32_t)((MONSTER_SPAWN_MAX_DIST - MONSTER_SPAWN_MIN_DIST) * 4.0f);
		const float radius = MONSTER_SPAWN_MIN_DIST +
			(radius_steps ? (float)rngBelow(rng, radius_steps) * 0.25f : 0.0f);

		const float cand_x = player_x + sinf(angle) * radius;
		const float cand_z = player_z - cosf(angle) * radius;

		const int x = floorToInt(cand_x);
		const int z = floorToInt(cand_z);
		const int y = floorToInt(player_y) +
			(int)rngBelow(rng, (uint32_t)(2 * MONSTER_SPAWN_Y_JITTER + 1)) -
			MONSTER_SPAWN_Y_JITTER;

		if (y < 1 || y + 1 >= WORLD_HEIGHT) continue;

		// Step 2 (worldColumn() != NULL), BEFORE the darkness test -- an unloaded column
		// reads dark on both light channels (world/light.h), so testing darkness first
		// would let an unloaded candidate pass as "dark enough" for the wrong reason.
		Column* col = worldColumn(w, x >> 4, z >> 4);
		if (!col) continue;

		// Step 3 (footprint): generic blockIsSolid(), no grass/dirt/sand allow-list.
		if (!monsterSpotIsGood(w, x, y, z)) continue;

		// Step 4 (darkness): "the one genuinely new predicate" -- plan §3.2.
		const int lx = x & 15;
		const int lz = z & 15;
		if (lightGetSky(col, lx, y, lz) != 0 || lightGetBlock(col, lx, y, lz) != 0) continue;

		// Step 5 (sub-cap) was the loop-top check above, re-evaluated per candidate.

		const uint8_t kind = pickMonsterKind(rng);
		const MonsterDef* def = monsterDef(kind);
		if (!def) continue;   // unreachable given pickMonsterKind()'s range, kept honest

		const int slot = entitySpawn(ew, kind, (float)x + 0.5f, (float)y, (float)z + 0.5f,
		                             def->width, def->height, MONSTER_EYE_FRAC);
		if (slot < 0) break;   // the pool or the entity cap said no; the rest will too

		Entity* e = entityAt(ew, slot);
		if (e) {
			// entitySpawn starts everything at 20 hp, ai_state 0 and ai_timer 0 -- 20 is
			// already this table's health for both kinds, so no health overwrite is
			// strictly needed today, but it is written anyway (from def->health, not the
			// literal) so a future row with a different starting hp does not silently spawn
			// at the wrong one.
			e->health   = def->health;
			e->ai_state = MONSTER_AI_IDLE;
			e->ai_timer = (uint16_t)rngBelow(rng, MONSTER_SPAWN_JITTER_TICKS);
			e->yaw      = randomYaw(rng);
		}
		spawned++;
	}

	return spawned;
}
