// The monster layer: zombies and skeletons, on top of the same pool animal.h shares.
//
// v1.8.18. docs/plan-1.8.18-monsters.md is the specification; read that document for the
// numbers this file only cites. Exactly like animal.h, this file gives meaning to the three
// fields entity.h carries opaquely (`kind`, `ai_state`, `ai_timer`) for a DIFFERENT range of
// kind values, and it does so without modifying entity.c, entity.h, animal.c or animal.h.
// ENT_KIND_ZOMBIE and ENT_KIND_SKELETON are already reserved and defined in animal.h (5 and
// 6) -- this file reuses them by name and defines no kind values of its own. ENT_KIND_COUNT
// stays exactly as animal.h ships it (5, "one past the last ANIMAL kind"); monsterDef() below
// is a SEPARATE table over a separate id range, not an extension of kDefs[ENT_KIND_COUNT].
//
// No <3ds.h>, matching entity/entity.h, entity/animal.h and every file under world/. Every
// rule below is exercised by tests/monster_test.c on the host, no console in the loop.
//
// Zero bytes of new per-entity storage, for the same reason animal.h gives: position,
// velocity, box, facing, health, decision timer and behaviour state already exist on Entity.
// A zombie's melee cooldown and a skeleton's windup/cooldown both live in Entity.ai_timer,
// reusing its documented meaning ("ticks until the creature lane's next decision") rather
// than asking entity.h for a second timer field -- see monsterThink() below for exactly how.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "entity/animal.h"
#include "entity/entity.h"
#include "world/rng.h"
#include "world/world.h"

// ---------------------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------------------
//
// ENT_KIND_ZOMBIE (5) and ENT_KIND_SKELETON (6) come from animal.h, which already reserves
// and names them for exactly this reason. 7 (arrow) stays unclaimed -- plan §3.5 takes the
// hitscan option (below), so no projectile entity is ever spawned and 7 is not touched here.
#define MONSTER_KIND_MIN ENT_KIND_ZOMBIE     // 5
#define MONSTER_KIND_MAX ENT_KIND_SKELETON   // 6, inclusive

// ---------------------------------------------------------------------------------------
// AI states, stored in Entity.ai_state
// ---------------------------------------------------------------------------------------
//
// A SEPARATE numbering from ANIMAL_AI_* (animal.h): ai_state is opaque per-entity storage,
// read only by the think function matching that entity's own kind, so the two state spaces
// can never collide even though some numbers repeat.
//
// IDLE is 0, matching animal.h's reason: entitySpawn() zeroes ai_state, so a freshly spawned
// monster is already in a valid state.
//
// CHASE is zombie-only. WINDUP and COOLDOWN are skeleton-only. Nothing enforces that at the
// type level -- ai_state is a plain uint8_t -- monsterThink()'s own per-kind branch is what
// keeps a skeleton from ever entering CHASE and a zombie from ever entering WINDUP.
#define MONSTER_AI_IDLE     0
#define MONSTER_AI_WANDER   1
#define MONSTER_AI_CHASE    2   // zombie only
#define MONSTER_AI_WINDUP   3   // skeleton only -- telegraphing before it fires
#define MONSTER_AI_COOLDOWN 4   // skeleton only -- after firing, before the next windup

// Detection / chase radius, blocks. Plan §4, exact: deliberately BELOW TICK_NEAR_BLOCKS (24,
// world/tick.h) so a monster that has noticed the player is always inside the 20 Hz near
// tier and "recomputed every due tick" already means "recomputed every tick" -- no separate
// chase-rate concern to reason about.
#define MONSTER_DETECT_RADIUS    16.0f
#define MONSTER_DETECT_RADIUS_SQ (MONSTER_DETECT_RADIUS * MONSTER_DETECT_RADIUS)

// Soft sub-caps on the SHARED entity pool, mirroring ANIMAL_CAP_OLD/NEW's own comment.
// Plan §4: derived as ENTITY_CAP_* minus ANIMAL_CAP_* (24-16=8, 48-32=16), not a separately
// chosen number -- animals and monsters between them must never exceed the pool's real cap.
#define MONSTER_CAP_OLD   8
#define MONSTER_CAP_NEW  16

_Static_assert(MONSTER_CAP_OLD + ANIMAL_CAP_OLD <= ENTITY_CAP_OLD,
              "the Old 3DS animal+monster caps exceed the pool cap");
_Static_assert(MONSTER_CAP_NEW + ANIMAL_CAP_NEW <= ENTITY_CAP_NEW,
              "the New 3DS animal+monster caps exceed the pool cap");

// ---------------------------------------------------------------------------------------
// The per-kind table
// ---------------------------------------------------------------------------------------
//
// Fixed-width types and an explicit `pad`, never an enum -- the identical reasoning
// AnimalDef's own comment gives: -fshort-enums has already made a host sizeof() lie about a
// console struct on this project, and every field here is pinned by offsetof in
// tests/monster_test.c on both ABIs.
//
// A SIBLING to AnimalDef, not a shared table with it. docs/plan-1.8.18-monsters.md §3.1
// leaves that choice explicitly to this lane ("an implementation call... not a scope call"):
// a shared table would need a `hostile` column every animal row carries and ignores, and
// would put a zombie's attack_period_ticks and windup_ticks on every pig and chicken as dead
// bytes. Two four-field-narrower tables cost less and read cleaner than one wide one.
typedef struct {
	uint8_t  health;                // starting hp, 0..20 scale. Plan §4: 20 for both kinds.
	uint8_t  damage;                // melee (zombie) or ranged (skeleton) per hit.
	                                 // Plan §4, researched: zombie 3, skeleton 4.
	uint16_t attack_period_ticks;   // full attack cycle. Plan §4: zombie ~20 (1s, a "feel"
	                                 // number the plan states outright is not covered by the
	                                 // research); skeleton 60 (3s, exact research match).
	uint16_t windup_ticks;          // skeleton's visible telegraph before it fires, plan §3.5
	                                 // "~20 ticks". 0 for zombie: contact damage is instant
	                                 // once in melee reach, no telegraph to give.
	uint16_t pad;                   // explicit, so width starts on a 4-byte boundary on both
	                                 // ABIs without relying on the compiler's implicit insert
	                                 // -- see AnimalDef's own comment for why this project
	                                 // pins that explicitly rather than trusting it.
	float    width;                 // full x/z collision extent, blocks -- bodySetBox().
	float    height;                // full box height, feet to head.
	float    speed;                 // WANDER speed always; CHASE speed for the zombie only
	                                 // (a windup'd skeleton stops moving entirely, §3.5/§6.7).
} MonsterDef;

// The row for `kind`, or NULL when `kind` is not a monster. Same "NULL is the predicate"
// contract animalDef() documents -- a caller that only wants to know "is this a monster"
// uses the NULL, exactly as the renderer and animalCount()/animalRaycast() already do for
// animalDef() without knowing monsters exist.
const MonsterDef* monsterDef(uint8_t kind);

// The per-console monster sub-cap. Mirrors animalCapFor(bool) / entityCapFor(bool).
int monsterCapFor(bool is_new_3ds);

// Live slots whose kind is a monster. Mirrors animalCount()'s shape and its reasoning
// exactly: not a cached counter, because entity.c owns the pool.
int monsterCount(const EntityWorld* ew);

// ---------------------------------------------------------------------------------------
// Thinking
// ---------------------------------------------------------------------------------------

// What monsterThink() needs that is not on the Entity, mirroring AnimalCtx's shape.
//
// NO Survival* HERE, and that is deliberate, the same argument animal.h's AnimalHurt makes
// for NO inventory dependency: world/survival.h has no <3ds.h> so it WOULD link on the host,
// but pulling it into entity/ would mean this module has to know Survival's shape to do
// anything with player_damage, and the out-parameter is simpler and matches the precedent
// this codebase already set. `player_damage` accumulates every hit landed on the player
// across the WHOLE entityTick() pass (multiple monsters can each add to it on one tick,
// honestly reflecting "no i-frames", plan §3.6/§10.2) and main.c reads it once after
// entityTick() returns and banks it through survivalDamage(), the same way animalHurt()
// reports a drop through out-parameters for main.c to bank with invBridgeAdd().
typedef struct {
	Rng*     rng;             // decision stream, required -- NULL makes every monster a
	                          // complete no-op, the same documented contract AnimalCtx sets.
	uint64_t tick;             // the tick number entityTick() is being called with. Needed
	                          // ONLY for the 2 Hz torch-despawn check below; NOT used as a
	                          // second decision clock (ai_timer, in ticks, is that clock,
	                          // exactly as animal.h argues for animals).
	float    player_x;
	float    player_z;
	float    player_eye_y;    // player's EYE height (body.y + eye), not feet -- skeleton line
	                          // of sight casts eye-to-eye. Zombie chase never reads this: its
	                          // detection is horizontal-only, matching tick.h's own distance
	                          // convention.
	uint8_t  player_damage;   // OUT. Zeroed by main.c before the entityTick() call; every
	                          // landed hit this call adds def->damage to it (clamped at 255,
	                          // which nothing this version can reach: at most
	                          // MONSTER_CAP_NEW=16 monsters times a max damage of 4 is 64).
} MonsterCtx;

// The combined context entityThinkDispatch() unpacks. Exists so entityTick() -- ONE
// EntityThinkFn hook -- can serve two creature families with different ctx shapes without
// widening either AnimalCtx or MonsterCtx to carry fields the other does not need. Plan
// §3.7 is explicit that widening animalThink() itself is not an option; this is the
// alternative it names: "a dispatcher at the call site that switches on kind range."
typedef struct {
	AnimalCtx  animal;
	MonsterCtx monster;
} EntityDispatchCtx;

// The function actually passed to entityTick() in place of animalThink. Matches
// EntityThinkFn exactly. Reads only e->kind to decide which real think function to call;
// touches nothing else itself.
void entityThinkDispatch(EntityWorld* ew, int slot, const World* w, float dt_s, void* user);

// The monster hook. NOT handed to entityTick() directly -- entityThinkDispatch() above is.
// Exists as its own function so tests/monster_test.c can call it directly against a
// synthetic zombie or skeleton without going through the dispatcher or a real animal ctx.
//
//   IDLE/WANDER -- same shape and the same numbers animalThink() uses for its own IDLE/
//                  WANDER (60% chance to wander on decision-expiry, 30..89 / 40..119 tick
//                  timers). [reasoned]: the plan enumerates these two states for monsters
//                  but gives no separate feel numbers for them, and re-using the shipped,
//                  already-tuned ones is the smallest change that gives a monster believable
//                  ambient movement before it notices the player.
//
//   ZOMBIE, on detecting the player (MONSTER_DETECT_RADIUS): immediately interrupts
//   whatever IDLE/WANDER decision was pending and enters CHASE. Every due tick (which, per
//   the radius argument above, means every tick while chasing): recompute yaw toward the
//   player and drive along it at the row's speed, reusing animalThink()'s own "horizontal
//   speed collapsed to ~0 means bodyMove() blocked it" wall-bounce read rather than a new
//   mechanism (plan §3.4, "reuse the stuck check"). In melee reach, deal def->damage through
//   ctx->player_damage on a def->attack_period_ticks cooldown carried in Entity.ai_timer --
//   entity.c already decrements ai_timer once per due tick before calling this, so the
//   cooldown counts down at the correct real-world rate with no extra bookkeeping here. When
//   the player leaves the radius, drops back to IDLE.
//
//   SKELETON, on detecting the player: stops advancing (plan §3.5/§6.7 -- no closing to
//   melee, no pathfinding) and faces the player every tick it is in range. Enters WINDUP for
//   def->windup_ticks; when that expires, RE-CHECKS range and line of sight (via
//   worldRaycast(), eye to eye) rather than trusting the state from when the windup started,
//   fires if both still hold, then COOLDOWN for the remainder of def->attack_period_ticks.
//   Leaving range at any point (including mid-windup or mid-cooldown) drops back to IDLE.
//
//   TORCH DESPAWN, checked FIRST, before any of the above, on a flat 2 Hz stagger
//   (tickDue(ctx->tick, TICK_FAR_PERIOD, e->id), world/tick.h) independent of this entity's
//   own near/far tick period. Plan §3.3: a monster standing in skyLight==0 && blockLight==0
//   despawns via entityKill() (deferred, safe from inside a think). Provably still exactly
//   2 Hz for every monster regardless of its own decimation tier: for a FAR entity (already
//   ticked at TICK_FAR_PERIOD by entity.c's own schedule), this recomputes the IDENTICAL
//   tickDue() call entity.c's own due-check just evaluated true, so it is unconditionally
//   true whenever this function runs at all; for a NEAR entity (ticked every frame), the
//   same fixed-period check independently gates to 1 call in 10 by construction. Either way
//   the real-world rate is 2 Hz, never more, never gated behind whichever tier the entity
//   happens to be in.
//
// `w` is used (worldColumn/lightGet for the despawn check, worldRaycast for skeleton LOS).
// `dt_s` is accepted to match the hook and unused, for the identical reason animalThink()
// gives: the decision clock is Entity.ai_timer, in ticks, and a seconds clock alongside it
// would be a second rate that can disagree.
//
// A NULL `user`, or a ctx with a NULL `rng`, is a complete no-op -- monsters that never move,
// the same documented contract animalThink() sets and animal_test.c pins.
void monsterThink(EntityWorld* ew, int slot, const World* w, float dt_s, void* user);

// ---------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------
//
// All periods below are in TICKS, the same clock tick.h and entity.c both count in.
// [reasoned/plan-silent numbers are named individually below; everything else is read
// directly off plan §3.2-§3.3.]
#define MONSTER_SPAWN_PERIOD_TICKS   40      // 2 s. Plan §4, exact.

// How many independent candidates are rolled per spawn check. [reasoned] -- the plan gives
// the per-candidate check order (§3.2) but not a count. 1 would make an accepted spawn rare
// even in good conditions (every check below has to pass); an unbounded retry loop is
// exactly what animal.h's own spawner refuses to do ("an unbounded retry loop is unbounded
// work on a handheld"). 3 independent, non-retried candidates per 2-second check is the
// smallest number that gives monsters a believable presence without a loop that can spin.
#define MONSTER_SPAWN_CANDIDATES      3

// Horizontal distance band from the player a candidate is rolled inside, blocks. [reasoned]
// -- implements plan §7's clustering mitigation ("cluster candidates near the player rather
// than scattering across the loaded ring... should be written into the code as a comment
// explaining why", which this is). The plan gives no exact band; 8..20 keeps every candidate
// within or just past MONSTER_DETECT_RADIUS (16), so an accepted spawn is never so far away
// it is pointless and never point-blank on top of the player.
//
// Candidates are sampled by angle + radius INSIDE this band, not rolled freely and then
// distance-rejected -- a stricter, cheaper reading of plan §3.2 step 1 ("distance... free
// arithmetic, rejects most candidates before any memory is touched") than a reject-based
// filter: every one of MONSTER_SPAWN_CANDIDATES lands in-band by construction, so none of
// the limited per-tick budget is ever spent on a candidate that step 1 would have thrown
// away anyway. See monsterSpawnTick() in monster.c.
#define MONSTER_SPAWN_MIN_DIST         8.0f   // blocks
#define MONSTER_SPAWN_MAX_DIST        20.0f   // blocks

// Vertical jitter around the PLAYER's own feet-y for a spawn candidate, blocks. [reasoned].
// Monsters spawn in darkness, which near the player usually means a cave at roughly the
// player's own depth -- unlike animalSpawnForColumn(), which samples worldgenHeight() (the
// surface), a monster candidate has no single "top of the world" y to test at all, so it is
// rolled relative to the player instead.
#define MONSTER_SPAWN_Y_JITTER         4

// Eye height fraction handed to bodySetBox(), mirroring ANIMAL_EYE_FRAC's own one-value
// argument: nothing reads a monster's eye except the wet test (and, new here, the
// skeleton's own LOS raycast origin -- see monster.c), and one number is one chance to be
// wrong instead of two. [reasoned] -- the plan does not give this number; animals' own 0.9
// is reused rather than inventing a second one with no stated reason to differ.
#define MONSTER_EYE_FRAC              0.9f

// Rolls up to MONSTER_SPAWN_CANDIDATES spawn attempts around the player. Returns the number
// actually spawned, 0..MONSTER_SPAWN_CANDIDATES.
//
// Called UNCONDITIONALLY, every tick, from main.c -- NOT gated by `tick % 40` at the call
// site. The 2-second period lives inside this function (a static/stored counter is not
// needed: it is gated by `ctx`-free arithmetic on `tick` itself, see monster.c), matching
// the convention main.c's own tick loop already uses for every other subsystem (survivalTick
// et al. gate their own periods internally) rather than introducing the one repeating-timer
// pattern that does not otherwise exist anywhere in main.c.
//
// The check order, cheapest-and-most-rejecting first, is plan §3.2 verbatim:
//   1. Distance from the player inside [MONSTER_SPAWN_MIN_DIST_SQ, MONSTER_SPAWN_MAX_DIST_SQ]
//      -- free arithmetic, rejects most candidates before any memory is touched.
//   2. worldColumn() != NULL -- load-bearing, and ordered BEFORE the darkness test on
//      purpose: an unloaded column reads dark on both light channels (world/light.h), so
//      testing darkness first would let every unloaded candidate silently pass as "dark
//      enough" instead of being rejected for the real reason.
//   3. Footprint: solid ground at y-1, air at y and y+1. Unlike animalSpawnForColumn()'s
//      spotIsGood(), there is NO grass/dirt/sand allow-list here -- blockIsSolid() alone,
//      because a monster spawning in a cave stands on stone, not grass.
//   4. Darkness: lightGetSky(col, lx, y, lz) == 0 && lightGetBlock(col, lx, y, lz) == 0.
//      "Only step 4 is new logic" -- plan §3.2.
//   5. The monster sub-cap (monsterCapFor), re-checked per candidate so a batch cannot
//      straddle it, exactly as animalSpawnForColumn() re-checks its own cap per member.
//
// Kind: a flat 50/50 zombie/skeleton roll per accepted candidate. [reasoned] -- the plan
// names both kinds and gives no selection weighting; nothing in §3-§4 suggests either
// should be rarer, so an even split is the smallest assumption.
int monsterSpawnTick(EntityWorld* ew, World* w, uint64_t tick, float player_x, float player_y,
                     float player_z, Rng* rng);
