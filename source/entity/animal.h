// The animal layer: the creature that entity/entity.h deliberately does not contain.
//
// v1.8.14. entity.c owns storage, lifetime, scheduling and physics and knows nothing about
// any creature -- `kind`, `ai_state` and `ai_timer` are carried there and are documented as
// opaque to it. This file is what gives those three fields meaning, and it does so WITHOUT
// modifying entity.c or entity.h at all. That is the design claim: an animal is a `kind`
// value, a row in a table, and a think function matching the hook entity.h already calls.
//
// No <3ds.h>, matching source/world/ and source/entity/. That is not decoration: it is what
// makes every rule below host-testable, and source/entity/animal_test.c exercises all of
// them on the host with no console in the loop. A decision left in a file the host suite
// cannot build is a decision nothing checks -- this project has shipped that defect twice.
//
// <math.h> IS used by animal.c (sinf/cosf/atan2f/sqrtf/fabsf), so any host stanza linking
// this file needs -lm. Every physics-adjacent stanza in tools/run_host_tests.sh already has
// it, so this costs nothing new.
//
// Zero bytes of new per-entity storage. Everything an animal needs -- position, velocity,
// collision box, ground and water state, facing, health, decision timer, behaviour state --
// already exists on Entity. Nothing here allocates.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "entity/entity.h"
#include "world/rng.h"
#include "world/world.h"
#include "world/worldgen.h"

// ---------------------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------------------
//
// ENT_NONE is 0 and means "free slot" -- entity.c's whole free-list story rests on it, so
// animals start at 1.
//
// 5, 6 and 7 are RESERVED for v1.8.16's zombie, skeleton and arrow, which
// docs/plan-1.8.16-monsters.md already assigns. They are left as a hole on purpose: a fifth
// animal taking one of them would force that version to renumber a value that by then is
// baked into save data and (eventually) into a wire record. animalDef() returns NULL for
// them and animal_test.c pins that, so the reservation is enforced rather than commented.
#define ENT_KIND_PIG      1
#define ENT_KIND_COW      2
#define ENT_KIND_CHICKEN  3
#define ENT_KIND_SHEEP    4
/* 5 = zombie, 6 = skeleton, 7 = arrow -- reserved for v1.8.16, do not reuse */
#define ENT_KIND_COUNT    5   /* one past the last ANIMAL kind */

// ---------------------------------------------------------------------------------------
// AI states, stored in Entity.ai_state
// ---------------------------------------------------------------------------------------
//
// IDLE is 0 deliberately: entitySpawn() zeroes ai_state, so a freshly spawned animal is
// already in a valid state and the spawner does not have to remember to write one.
#define ANIMAL_AI_IDLE    0
#define ANIMAL_AI_WANDER  1
#define ANIMAL_AI_FLEE    2

// Damage one bare-handed hit does. There are no tools in this codebase, so this is the only
// damage number the game has. 4 makes a chicken a one-hit kill, a sheep two, a pig or cow
// three -- the pacing the real game trained everyone on.
#define ANIMAL_FIST_DAMAGE 4

// Soft sub-caps on the SHARED entity pool. entity.h's ENTITY_CAP_OLD 24 / ENTITY_CAP_NEW 48
// are shipped, static-asserted and correct, and are NOT touched here. These hold back a
// third of each for v1.8.16's monsters, so that version does not arrive to find every slot
// full of sheep. They allocate nothing -- the cost is one comparison in a spawner that is
// already counting.
#define ANIMAL_CAP_OLD    16
#define ANIMAL_CAP_NEW    32

_Static_assert(ANIMAL_CAP_OLD <= ENTITY_CAP_OLD, "the Old 3DS animal cap is above the pool cap");
_Static_assert(ANIMAL_CAP_NEW <= ENTITY_CAP_NEW, "the New 3DS animal cap is above the pool cap");

// ---------------------------------------------------------------------------------------
// The per-kind table
// ---------------------------------------------------------------------------------------
//
// One row per kind, not four sets of shared constants. The reason is not authenticity:
// v1.8.16 needs a per-kind table for zombies and skeletons whatever this version does, so a
// table now is LESS total work than flat constants plus a later migration.
//
// Fixed-width types and an explicit `pad`, never an enum. The ARM EABI compiles with
// -fshort-enums, which has already made a host sizeof() lie about a console struct on this
// project; a uint8_t is one byte on both machines and cannot. animal_test.c pins the size
// and every field offset, and those pins are true on BOTH ABIs -- unlike sizeof(Entity),
// which is 60 on ARM and 64 on the host because Body embeds a BodyWet enum. Do not write
// that assert.
typedef struct {
	uint8_t health;     // starting hp on the 0..20 scale entity.c already uses
	uint8_t drop_item;  // an ItemId (== BlockId). 0 = drops nothing.
	uint8_t drop_min;   // inclusive
	uint8_t drop_max;   // inclusive
	uint8_t hostile;    // 0 for every animal. The field v1.8.16 fills in.
	uint8_t pad;        // explicit, so the struct is the same size on both ABIs
	float   width;      // full x/z collision extent, blocks -- handed to bodySetBox()
	float   height;     // full box height, feet to head
	float   speed;      // wander speed, blocks/s
} AnimalDef;

// The row for `kind`, or NULL when `kind` is not an animal (ENT_NONE, the reserved monster
// ids 5..7, or anything else). Callers use the NULL as the "is this an animal" predicate --
// the renderer and the raycast both do, so a monster arriving in the same pool in v1.8.16 is
// skipped by both without either of them being edited.
const AnimalDef* animalDef(uint8_t kind);

// The per-console animal sub-cap. Mirrors entityCapFor(bool) rather than inventing a second
// way to ask the same question.
int animalCapFor(bool is_new_3ds);

// Live slots whose kind is an animal. A 48-slot scan, the same shape entityFindById already
// accepts. Not a cached counter: entity.c owns the pool and a second count kept in step with
// it by hand is exactly the state that drifts.
int animalCount(const EntityWorld* ew);

// ---------------------------------------------------------------------------------------
// Thinking
// ---------------------------------------------------------------------------------------

// What animalThink() needs that is not on the Entity. Passed as `user` through entityTick(),
// so nothing here is a file static and a test can own one -- the same rule entity.h:24-25
// sets for the store itself.
//
// `gen` is reserved for a future rule that wants the heightmap or the biome in the think
// (nothing does today, and it is here so that adding one is not a signature change every
// lane has to agree to). `rng` is the decision stream and IS required: see animalThink.
typedef struct {
	const WorldGen* gen;
	Rng*            rng;
} AnimalCtx;

// The creature hook. Signature matches EntityThinkFn (entity.h:187-188) exactly, so wiring
// it is replacing one NULL in main.c's entityTick() call.
//
// Three states and nothing else. Minecraft's passive-mob AI is this simple and simple is
// correct here: no pathfinding, no line of sight, no neighbour scan, no allocation.
//
//   IDLE   -- standing still. On the decision timer expiring: 60% chance of WANDER with a
//             fresh uniform yaw and a 40..119 tick (2..6 s) timer, 40% of staying IDLE for
//             another 30..89 ticks.
//   WANDER -- walking along `yaw` at the row's speed. On expiry -> IDLE for 30..89 ticks.
//   FLEE   -- walking directly away from whatever hit it at 1.5x speed for 60 ticks (3 s).
//             Entered ONLY by animalHurt(). On expiry -> IDLE.
//
// PER-KIND BEHAVIOUR DIFFERENCES: NONE. A chicken is a small fast pig with less health, and
// that is genuinely how the real game's passive mobs work. Every difference lives in the
// AnimalDef row -- health, drop, box, speed -- plus the model. There is deliberately no
// `switch (kind)` anywhere in animal.c's think, because that is the shape that turns into
// four AIs nobody can test.
//
// WALL BOUNCE, for free and with no new API. bodyMove() zeroes vx when it is blocked in x
// and vz when blocked in z (world/physics.c:360, :367), and entityTick() discards the mask
// it returns. So rather than change shipped entity.c, the think reads the SAME condition one
// tick later off the body: a WANDER animal whose horizontal speed has collapsed to zero has
// hit something, and it turns immediately instead of waiting out its timer. An animal
// blocked on only one axis still has speed on the other and keeps sliding along the wall,
// which is the correct behaviour and falls out of the same test.
//
// NEVER touches body.x/y/z. entityTick() runs bodyStep() immediately after this returns
// (entity.c:270-273) and integrating here would double-integrate. animal_test.c asserts
// bit-identical positions across a direct think call.
//
// `w` and `dt_s` are accepted to match the hook and are unused: the decision clock is
// Entity.ai_timer, which entityTick() already counts down in ticks, and mixing a seconds
// clock into it would give two rates that can disagree.
//
// A NULL `user`, or a ctx with a NULL `rng`, makes this a complete no-op -- it cannot roll a
// decision without a stream and a silent fallback stream would be a second, invisible source
// of randomness. Defined, tested, and visible in a playtest as animals that never move.
void animalThink(EntityWorld* ew, int slot, const World* w, float dt_s, void* user);

// ---------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------

// Rolls a herd for one column that has just become part of the LIVE world. Returns the
// number of members actually spawned, 0..4.
//
// THREADING: main-thread only, and it assumes it. It writes `ew` (main-thread state) and
// reads `w` through worldGet(), so it must be called from the same thread that ticks and
// draws. In particular it must NOT be called from worldColumnCreate(), which the terrain
// worker runs against a STAGING world (source/app/worker.c) -- docs/plan-1.8.14-animals.md
// names that site and is wrong about it. The correct site is the main-thread
// "this column is now genuinely part of the live world" moment inside genInstallOne(),
// immediately after networldOnColumnLoad(). Lane D owns that edit; this function does not
// make the call and takes no lock, because on the main thread there is nothing to lock.
//
// The rule, cheapest test first:
//   1. The column must be loaded. An unloaded one spawns nothing and costs one lookup.
//   2. The animal sub-cap must have room (see animalCapFor). Re-checked per member, so the
//      cap holds even if a herd would straddle it.
//   3. ~1 column in 12 gets a herd at all.
//   4. One kind for the whole herd, chosen from the ones its biome allows. BIOME_DESERT
//      allows none and spawns nothing, which is what makes a desert feel like one.
//   5. Herd size 2..4.
//   6. Per member, ONE candidate position and no rejection loop -- an unbounded retry loop
//      is unbounded work on a handheld. A member whose spot is unsuitable is simply not
//      spawned, so a herd of 4 that places 2 is a normal outcome and is self-limiting.
//      Suitable means: standing on grass, dirt or sand, with two blocks of air above.
//
// There is NO light-level gate. world/light.h:14-15 says the light engine is off in the host
// suite and in the dedicated server, so a light-gated rule reads 0 (= dark everywhere) there
// and every host test of this function would exercise the wrong branch. "Top solid block
// with open air above" already means the daylight surface. v1.8.16's monsters genuinely need
// light and can pay for a host seam then.
//
// There is NO per-column "already tried here" flag. Re-entering an area does re-roll, and
// the cap is what actually bounds the population -- correctly, and without either a bit on
// the shipped Column struct (which would have to be persisted to mean anything) or a
// coordinate-keyed side table (new state, new lifetime bug).
int animalSpawnForColumn(EntityWorld* ew, World* w, const WorldGen* g,
                         int cx, int cz, Rng* rng);

// ---------------------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------------------

// The nearest live animal whose box the ray enters within `max_distance`. Returns the SLOT,
// or -1. `out_dist` (may be NULL) receives the distance to the entry point, in blocks.
//
// The box is [x - half_w, x + half_w] x [y, y + height] x [z - half_w, z + half_w]: Body.y
// is FEET, not centre (world/physics.h:199).
//
// The direction is normalised internally, so `out_dist` is in blocks whatever the caller's
// vector length is, and a zero-length direction is refused with -1 rather than dividing by
// zero. A ray starting INSIDE a box hits it at distance 0.
//
// This does NOT extend worldRaycast(). That is a block-grid DDA walker with integer hit
// coordinates and no float box concept; two separate tests compared by distance is both the
// simpler structure and the honest one. main.c compares this result against
// Interact.target.distance and lets the nearer one win.
//
// 48 slots x about a dozen float ops, called once per FRAME rather than per tick.
int animalRaycast(const EntityWorld* ew, float ox, float oy, float oz,
                  float dx, float dy, float dz, float max_distance,
                  float* out_dist);

// Applies `damage` to the animal in `slot`. Returns true if this hit killed it.
//
// Survived: health drops by exactly `damage`, the animal enters FLEE for 60 ticks pointing
// directly away from (from_x, from_z) at 1.5x its row speed, and 0 is written to both out
// parameters.
//
// Killed: health goes to 0 (clamped -- a uint8_t underflow here would be a pig with 253 hp),
// *out_drop_item gets the row's drop_item and *out_drop_count a roll in [drop_min, drop_max]
// inclusive, and the slot is entityKill()ed. entityKill is the DEFERRED one, so this is safe
// even if it is ever called from inside a think.
//
// NO INVENTORY DEPENDENCY, and that is deliberate. net/inv_bridge.h is network-side and
// pulls in <3ds.h>; calling it from here would make the whole damage rule unbuildable on the
// host. Instead this reports the drop through out-parameters and main.c banks it with
// invBridgeAdd(), mirroring exactly how interact.c sets it->broke_id and main.c banks that.
// Both out pointers may be NULL. A drop_item of 0 still writes a count -- main.c is expected
// to check the item id before banking, the same way it checks broke_id != BLOCK_AIR.
//
// THE DROP COUNT IS DERIVED FROM A POSITIONAL HASH, not from a stream, because the frozen
// contract for this function carries no Rng. rngHash3() over the entity's id and its integer
// position gives a different roll for every kill without a stream to thread through main.c's
// combat splice. See animal.c.
bool animalHurt(EntityWorld* ew, int slot, uint8_t damage,
                float from_x, float from_z,
                uint8_t* out_drop_item, uint8_t* out_drop_count);
