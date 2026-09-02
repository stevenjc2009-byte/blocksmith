// The entity store: a fixed pool of things that are not the player and not blocks.
//
// v1.8.8 foundation. This file deliberately contains NO creature. There is no pig, no
// zombie, no AI, no spawn rule and no drop table here, and that is the point: a later lane
// builds those on top, and the thing that decides whether that lane is easy or miserable is
// whether the foundation made the right four promises. They are:
//
//   1. Storage is a fixed pool. No malloc, no free, no linked list, nothing that can leak
//      or dangle. The whole store is one struct the caller owns and can put in BSS.
//   2. An entity moves through bodyMove(), the SAME collision code the player moves
//      through, with its own box. Not a copy of it. A second sweep drifts from the first
//      and produces bugs that appear for mobs but not the player, which is the hardest
//      shape of bug there is to find.
//   3. Ticking goes through world/tick.h, which was written for this subsystem and was
//      unused by it until now. There is no second scheduler in this project.
//   4. Identity is an id, never a slot index. A slot is reused the moment it is freed; an
//      id is not. Everything that outlives one tick -- a network handle, a "who hit me"
//      reference, a target -- holds the id.
//
// No <3ds.h>, matching source/world/. That is not decoration: it is what makes every rule
// in this file host-testable, and physics.h's own comment states the standard -- a decision
// left in a file the host suite cannot build is a decision nothing checks.
//
// NOT file-static. The store is a struct passed by pointer, so two of them can exist, a
// test can own one, and nothing here is state a worker thread has to reason about.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/physics.h"
#include "world/tick.h"
#include "world/world.h"

// The array size, compiled in, the same on both consoles. One size for both machines for
// the same argument world/budget.h makes about WORLD_BUDGET_BYTES: an OCCUPANCY limit that
// allocates nothing costs the smaller machine nothing, and two array sizes would buy a
// second failure mode for no saving. See entityCapFor() for the per-console limit.
#define ENTITY_SLOTS    48

// Runtime occupancy limits. These are counts of live entities, not bytes, and they are not
// connected to budgetClaim() in either direction: a world with 48 entities and a world with
// 0 consume identical application-heap memory, because the store is not on that heap.
#define ENTITY_CAP_OLD  24
#define ENTITY_CAP_NEW  48

_Static_assert(ENTITY_CAP_NEW <= ENTITY_SLOTS, "the New 3DS cap does not fit the pool");
_Static_assert(ENTITY_CAP_OLD <= ENTITY_CAP_NEW, "the Old 3DS cap is above the New one");

// One tick's worth of seconds, from the shared clock. Not a new constant -- TICK_HZ owns
// the rate and this is the reciprocal of it, so nothing here can disagree with tick.h.
#define ENTITY_DT       (1.0f / (float)TICK_HZ)

// kind == ENT_NONE is what makes a slot free. Zero, so a zeroed EntityWorld is an empty
// one and there is no separate "occupied" bool to keep in sync with anything.
//
// Every other kind value belongs to the creature lane. This file assigns no meaning to any
// of them and never branches on one; the kind is carried, stored and handed back.
#define ENT_NONE        0

// Flag bits. Only two are defined here and both are about the STORE, not about behaviour.
//
// ENT_F_REMOTE is the one that matters for the future, and it is one bit rather than a
// design: an entity carrying it is owned by somebody else on the wire, so entityTick()
// neither thinks for it nor steps it -- its pose arrives instead. See "Networked entities"
// below. Defining it now costs one bit and means the day sync arrives, the store does not
// have to be torn up to distinguish "mine" from "theirs".
#define ENT_F_REMOTE    (1u << 0)

// Set by entityKill() to mark a slot for removal at the end of the tick, so a think
// callback may retire an entity without freeing the struct it is standing in.
#define ENT_F_DESPAWN   (1u << 1)

// The record.
//
// Body is EMBEDDED rather than the position/velocity fields being copied out of it, and
// that is promise 2 above made structural. An entity does not have an x it keeps in step
// with a Body's x; it has a Body. bodyStep() is handed &e->body directly, so there is no
// copy-in/copy-out step to get wrong and no way for the water hysteresis, the on_ground
// flag or the swim window to be silently dropped between ticks.
//
// The cost of embedding is real and is stated in the report: an Entity is bigger than the
// 36 bytes docs/plan-entities.md budgeted for a hand-rolled position/velocity record. It
// buys exact parity with the player's collision, for about twenty bytes at a pool size of
// 48. That is the trade, taken deliberately.
//
// Fixed-width integer types throughout, never an enum, for kind/health/flags/ai_state. The
// ARM EABI compiles with -fshort-enums, which has already made a host sizeof() lie about a
// console struct on this project (Chunk: 8 bytes on ARM against 12 on host x86-64). A
// uint8_t is one byte on both machines and cannot do that.
typedef struct {
	Body     body;      // position, velocity, on_ground, wet state AND the collision box

	float    yaw;       // facing, radians. No pitch: a quadruped does not pitch, and the
	                    // one thing that would want it (a thrown projectile) is not an
	                    // entity in this design.

	uint16_t id;        // stable handle. Never 0 for a live entity, never reused while the
	                    // holder is alive, and what tickDue() staggers the decimated set by.
	uint16_t ai_timer;  // ticks until the creature lane's next decision. Counted down here
	                    // as a convenience (it is the one field every AI wants and none
	                    // would implement differently); never READ here.

	uint8_t  kind;      // ENT_NONE = free slot. Otherwise opaque to this file.
	uint8_t  health;    // 0..20, the scale BS_APP_PLAYER_STATE already uses, so a survival
	                    // system landing later rescales nothing.
	uint8_t  flags;     // ENT_F_*
	uint8_t  ai_state;  // opaque to this file; the creature lane owns its meaning
} Entity;

typedef struct {
	Entity   e[ENTITY_SLOTS];
	int      count;      // occupied slots, maintained by spawn/despawn, never recounted
	int      cap;        // per-console occupancy limit, <= ENTITY_SLOTS
	uint16_t next_id;    // monotonic handle source; wraps past 65535 and skips 0
} EntityWorld;

// ---------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------

// Zeroes the pool and sets the occupancy limit. `cap` is clamped into 0..ENTITY_SLOTS
// rather than rejected: a caller that computes it from a hardware probe should get a
// working store with a sane limit, not a store that silently never spawns.
void entityWorldInit(EntityWorld* ew, int cap);

// The per-console limit. Mirrors scene/render_dist.h's renderDistMaxFor(bool) rather than
// inventing a second way to ask the same question.
int entityCapFor(bool is_new_3ds);

// Claims a free slot. Returns the slot index, or -1 when the pool is full, the cap is
// reached, or `kind` is ENT_NONE (which would be a slot that is free the moment it is
// made -- refused rather than stored, because a caller that passes it has a bug).
//
// `width`, `height` and `eye_frac` are the collision box, handed straight to bodySetBox().
// Pass 0 for any of them to keep the player's value for that dimension.
//
// The new entity starts at rest, at full health (20), with no flags, ai_state 0 and
// ai_timer 0. The creature lane overwrites whatever it cares about through entityAt().
int entitySpawn(EntityWorld* ew, uint8_t kind, float x, float y, float z,
                float width, float height, float eye_frac);

// Frees a slot immediately. False when the slot index is out of range or already free.
// The Entity's memory is not cleared beyond kind = ENT_NONE and id = 0 -- there is nothing
// secret in it and zeroing 60 bytes to free a slot is work for nothing -- but the id IS
// cleared, so a stale handle can never match a reused slot.
bool entityDespawn(EntityWorld* ew, int slot);

// Marks a slot for removal at the end of the current entityTick(). This is what a think
// callback calls: despawning during the iteration would free the struct the callback is
// standing in. Outside a tick it is simply a deferred entityDespawn.
bool entityKill(EntityWorld* ew, int slot);

// ---------------------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------------------

// NULL when the index is out of range or the slot is free. Callers iterate 0..ENTITY_SLOTS
// and skip the NULLs; there is no free list and no compaction, so a slot index is stable
// for exactly as long as the entity in it lives and no longer.
Entity*       entityAt(EntityWorld* ew, int slot);
const Entity* entityGet(const EntityWorld* ew, int slot);

// Slot holding `id`, or -1. Linear over 48 slots, which is 48 halfword compares -- an index
// keyed by id would be faster and would introduce a class of bug (a stale index surviving a
// despawn) that the scan cannot have.
//
// Ids are unique among LIVE entities by construction: entitySpawn refuses to issue an id
// that is currently in use. They are NOT unique over a whole session past 65,536 spawns,
// which is what "never reused while the holder is alive" means and all that anything needs.
int entityFindById(const EntityWorld* ew, uint16_t id);

int entityCount(const EntityWorld* ew);
int entityCap(const EntityWorld* ew);

// ---------------------------------------------------------------------------------------
// Ticking
// ---------------------------------------------------------------------------------------

// The creature lane's hook, and the entire extent of this file's knowledge of behaviour.
// Called once per DUE tick for each entity, before its physics runs, with the seconds of
// simulated time that entity is about to be advanced by. Set vx/vz and yaw in here; do not
// integrate position, that is what the bodyStep below it is for.
//
// A NULL think function is the normal case for this version and costs one branch per due
// entity. Nothing in this file supplies one.
typedef void (*EntityThinkFn)(EntityWorld* ew, int slot, const World* w, float dt_s,
                              void* user);

typedef struct {
	int live;        // occupied slots seen this call
	int thought;     // entities whose think ran (i.e. were due)
	int decimated;   // live, but not due on this tick
	int steps;       // bodyStep() calls made -- the honest physics cost of the call
	int unloaded;    // despawned because their column was gone
} EntityTickStats;

// Advances every entity by one clock tick.
//
// Per entity, in this order and cheapest-first:
//
//   1. The column-unload check. floor(x/16), floor(z/16), worldColumn() -- NULL means the
//      terrain under this entity has been freed, so it is despawned BEFORE any physics
//      touches it. This is the whole of the lifetime story and it is why an Entity holds
//      no pointer into world memory: worldColumnRemove() cannot free entity memory because
//      none is reachable from a Column, so no leak and no dangling pointer is possible, and
//      the only remaining hazard -- a live entity standing on terrain that is gone -- is
//      answered here rather than by a hook into main.c's column dropping that would have to
//      be kept in sync forever. It also gives despawn-by-distance free: the loaded ring is
//      radius + 1, so anything that wanders out of it hits a NULL column, and no distance
//      constant has to exist.
//
//   2. ENT_F_REMOTE -- counted live and then left alone entirely. Its owner drives it.
//
//   3. The schedule, from tick.h: period = tickPeriodForDistSq(dx*dx + dz*dz) -- 1 inside
//      TICK_NEAR_BLOCKS, TICK_FAR_PERIOD beyond -- then tickDue(tick, period, id). The id
//      is what staggers the decimated set so the 2 Hz group does not all fire on the same
//      tick.
//
//   4. Not due: skipped, counted in `decimated`, nothing else happens. Due: ai_timer counts
//      down, `think` runs, then the physics.
//
// THE PHYSICS IS BATCHED, NOT SCALED, and this is the one place this file departs from
// docs/plan-entities.md §4.3 on purpose. That section says a decimated entity "integrates
// once per 10 ticks with a proportionally larger dt". It must not: bodyMove() subdivides a
// move into MAX_SUBSTEP (0.4 block) pieces, so a 10x dt is a 10x sweep is ~10x the substeps
// -- the same total collision work, in one lumpy call instead of ten smooth ones. Worse,
// a merely RESTING entity gains 0.5 s of gravity before it moves, sweeps several blocks
// down, and spends every one of those substeps being snapped back onto the floor it never
// left. Measured on the host: the naive scaled form costs a far entity MORE per second than
// a near one. So a due entity runs `period` bodyStep() calls of exactly ENTITY_DT each,
// which is bit-for-bit the trajectory it would have had at the full rate.
//
// What that means for the saving, stated honestly rather than claimed: distance decimation
// buys a 10x reduction in AI and dispatch, and buys NOTHING on the collision sweep, because
// a body that travels the same distance tests the same cells however the time is chopped
// up. The real numbers are in the report.
//
// `px`, `pz` are the player's world x/z. The distance is computed in float and squared into
// an int32 for tickPeriodForDistSq, which guards its own negative case.
EntityTickStats entityTick(EntityWorld* ew, const World* w, uint64_t tick,
                           float px, float pz, EntityThinkFn think, void* user);

// The schedule, exposed so a caller can ask the same question entityTick asks without
// re-deriving it. Same answer, same source.
int  entityTickPeriod(const Entity* e, float px, float pz);
bool entityTickDue(const Entity* e, uint64_t tick, float px, float pz);

// ---------------------------------------------------------------------------------------
// Rendering seam
// ---------------------------------------------------------------------------------------
//
// Nothing in this file draws, and nothing in this file includes a GPU header. The renderer
// is a sibling of scene/playermodel.c -- one linearAlloc'd VBO built once at init, uniforms
// re-uploaded on every bind because the PICA's float bank is shared hardware, fog on TexEnv
// stage 1, drawn from inside main.c's drawEye() so stereo needs no new code -- and it reads
// this store through exactly three calls and no others:
//
//     for (int i = 0; i < ENTITY_SLOTS; i++) {
//         const Entity* e = entityGet(ew, i);
//         if (!e) continue;
//         /* e->body.x, e->body.y, e->body.z, e->yaw, e->kind, e->flags */
//     }
//
// entityCount() is the early-out, entityGet() is the iterator, and the Entity's own fields
// are the pose. There is no draw list to build and keep in sync, no per-frame copy, and no
// callback from here into scene/. Culling (distance against the fog boundary, then
// frustumTestAABB) belongs on the renderer's side of this line, because the frustum is the
// renderer's and this file must not learn about it.

// ---------------------------------------------------------------------------------------
// Networked entities
// ---------------------------------------------------------------------------------------
//
// Not implemented in this version. What is built here so that it does not have to be torn
// up when it is:
//
//   * `id` is the handle, not the slot. A pose arriving for id 41 finds its slot with
//     entityFindById() and is unaffected by anything that reused slot 3 in the meantime.
//   * ENT_F_REMOTE already makes entityTick() leave an entity entirely alone. A remote
//     entity is stored, drawn and collided against exactly like a local one, and simply is
//     not simulated. That is the whole of the client-side change.
//   * The store is a struct by pointer, so a second one (a "remote" store) is possible
//     without touching this file at all, if that turns out to be cleaner than a flag.
//
// The protocol shape, from deps/blocksmith-server/proto/bs_proto.h: BS_PROTO_VERSION stays
// 1 -- it gates TRANSPORT packet types, and entity messages are BS_APP_* opcodes riding
// inside BS_PKT_DATA, where 0x10..0xFF are free. Direction is what matters:
//
//   BS_APP_ENTITY_SYNC, server -> client. SAFE to ship client-first: an old client hits
//   networldApplyPayload()'s `default: break;` and ignores it.
//
//   BS_APP_ENTITY_SYNC, client -> server, sent by whichever client owns the mobs. NOT safe
//   client-first: an old server's handle_app_payload() default case calls send_kick(). This
//   direction forces a server release shipped FIRST, and the client Makefile's
//   check-proto-drift pins an exact server commit, so that commit must exist before the
//   client can even build.
//
// A 12-byte wire record per entity (id u16, kind u8, flags u8, x/y/z as i16 at 1/16 block,
// yaw u8, pad u8) puts 48 entities in 578 bytes, inside BS_MAX_PAYLOAD (1024) so it never
// fragments. At the existing 10 Hz pose rate that is 5,780 B/s downstream, against the
// 3,750 B/s the current player poses cost -- i.e. it roughly doubles downstream traffic on
// a full server. That is a decision to take with a number attached, not a footnote.
