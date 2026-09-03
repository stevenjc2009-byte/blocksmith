// See entity/animal.h for what each rule is and why it is shaped that way. This is the
// arithmetic.
#include "entity/animal.h"

#include <math.h>

#include "world/block.h"
#include "world/chunk.h"
#include "world/physics.h"

_Static_assert(CHUNK_DIM == 16, "the column-local spawn roll assumes a 16-wide column");

// ---------------------------------------------------------------------------------------
// THE YAW CONVENTION. Read out of the shipped code, not assumed.
// ---------------------------------------------------------------------------------------
//
// Two independent sites in this tree already turn a yaw into a direction and they agree:
//
//   source/scene/player.c:48-51 (walking)
//       const float s  = sinf(p->cam.yaw);
//       const float c  = cosf(p->cam.yaw);
//       const float fx = s;
//       const float fz = -c;
//   with the comment on :40 -- "Same yaw convention as cameraView -- forward at yaw 0 is -Z"
//
//   source/scene/interact.c:526-529 (the aim raycast)
//       const float cp = cosf(cam->pitch);
//       const float fx = sinf(cam->yaw) * cp;
//       const float fy = -sinf(cam->pitch);
//       const float fz = -cosf(cam->yaw) * cp;
//   with the comment on :521-524 -- "The view matrix is Rx(pitch) * Ry(yaw) * T(-pos), so
//   positive pitch looks down and -Z is forward at zero yaw."
//
// So:  forward(yaw) = ( sinf(yaw), -cosf(yaw) )   in (x, z).
//
// And the inverse -- the yaw that faces along (dx, dz) -- is atan2f(dx, -dz), NOT
// atan2f(dx, dz). Getting that second argument's sign wrong is the bug this whole comment
// exists to prevent: an animal would flee TOWARDS whatever hit it, and a test written from
// the same wrong assumption (asserting an angle equals a number) would agree with it. So
// animal_test.c asserts the DISTANCE from the threat increases over a real simulated run,
// which is a fact about the world rather than about the formula, and it goes red if either
// the sine or the cosine sign is flipped.
#define ANIMAL_TWO_PI 6.28318530717958647692f

// Decision timers, in TICKS (the clock entityTick counts ai_timer down in, 20 Hz).
#define ANIMAL_IDLE_MIN_TICKS    30   /* 1.5 s */
#define ANIMAL_IDLE_SPAN_TICKS   60   /* -> 30..89 ticks, 1.5..4.5 s   */
#define ANIMAL_WANDER_MIN_TICKS  40   /* 2.0 s */
#define ANIMAL_WANDER_SPAN_TICKS 80   /* -> 40..119 ticks, 2..6 s      */
#define ANIMAL_FLEE_TICKS        60   /* 3.0 s, fixed                  */

// Percent chance an IDLE animal starts wandering when its timer expires.
#define ANIMAL_WANDER_CHANCE 60

// A fleeing animal runs at this multiple of its row speed.
#define ANIMAL_FLEE_SPEED_MUL 1.5f

// Horizontal speed (|vx| + |vz|, blocks/s) below which a WANDER animal counts as stuck.
// bodyMove() zeroes the blocked axis outright, so the real value after a head-on wall hit is
// exactly 0; the threshold is a guard against float dust, not a tolerance band. The slowest
// animal moves at 1.5, so there is more than two orders of magnitude of margin.
#define ANIMAL_STUCK_SPEED 0.01f

// Eye height as a fraction of the box, handed to bodySetBox() for the water probe. One value
// for every animal: nothing in this version reads an animal's eye except the wet test, and
// four numbers where one does is four chances to be wrong.
#define ANIMAL_EYE_FRAC 0.9f

// One column in this many gets a herd rolled for it.
#define ANIMAL_HERD_CHANCE_DENOM 12

#define ANIMAL_HERD_MIN  2
#define ANIMAL_HERD_SPAN 3   /* -> 2..4 members */

// Ticks of decision-timer jitter given to a fresh herd member, so a herd does not think in
// lockstep and then all turn on the same tick.
#define ANIMAL_SPAWN_JITTER_TICKS 60

// ---------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------
//
// drop_item was 0 -- "drops nothing" -- in every row until the items-and-registry lane landed
// the four meat blocks. [2026-09-03] it is now the real id in all four rows, written as the
// NAMED constant from world/block.h rather than the literal, so a renumber over there moves
// this table with it instead of silently repointing a pig at whatever ends up at 34.
//
// THE TRAP HERE IS THE NUMBERING SPACE. block.h:217-218 records that a block's id and its
// atlas slot are two different spaces, and for exactly these four rows they differ by four:
// BLOCK_RAW_PORKCHOP is id 34 but tile slot 38, BLOCK_RAW_MUTTON is id 37 but slot 41.
// drop_item is an ID. A nearby 38..41 in atlas code is NOT this number, and the named
// constants are what keep the two apart -- the other reason not to write literals here.
//
// Health and drop counts follow the researched Java figures. Boxes and speeds are chosen
// here: the speeds sit in the 0.35..0.40 band of PLAYER_WALK_SPEED (4.3, physics.h:32), slow
// enough that a herd does not cross the loaded ring in seconds. 0.9 width keeps a pig out of
// the one-block gaps the 0.6-wide player can take; a chicken at 0.4 fits everywhere.
static const AnimalDef kDefs[ENT_KIND_COUNT] = {
	/* [ENT_NONE] -- never returned; the hole is here so the table indexes by kind
	   directly instead of by kind - 1, which is one subtraction nobody can get wrong. */
	{ 0,  0, 0, 0, 0, 0,  0.0f, 0.0f, 0.0f },

	/* [ENT_KIND_PIG]     */ { 10, BLOCK_RAW_PORKCHOP, 1, 3, 0, 0,  0.9f, 0.9f, 1.7f },
	/* [ENT_KIND_COW]     */ { 10, BLOCK_RAW_BEEF,     1, 3, 0, 0,  0.9f, 1.4f, 1.5f },
	/* [ENT_KIND_CHICKEN] */ {  4, BLOCK_RAW_CHICKEN,  1, 1, 0, 0,  0.4f, 0.7f, 1.7f },
	/* [ENT_KIND_SHEEP]   */ {  8, BLOCK_RAW_MUTTON,   1, 2, 0, 0,  0.9f, 1.3f, 1.6f },
};

const AnimalDef* animalDef(uint8_t kind)
{
	// The bounds are the whole point: kinds 5, 6 and 7 are the reserved monster ids and 0 is
	// the free-slot marker, and all four must answer NULL so that every "is this an animal"
	// caller -- the renderer, the raycast, the counter -- skips them without being edited
	// when v1.8.16 puts a zombie in the same pool.
	if (kind == ENT_NONE || kind >= ENT_KIND_COUNT) return NULL;
	return &kDefs[kind];
}

int animalCapFor(bool is_new_3ds)
{
	return is_new_3ds ? ANIMAL_CAP_NEW : ANIMAL_CAP_OLD;
}

int animalCount(const EntityWorld* ew)
{
	if (!ew) return 0;

	int n = 0;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const Entity* e = entityGet(ew, i);
		if (e && animalDef(e->kind)) n++;
	}
	return n;
}

// The animal sub-cap for a given store, derived from the store's own entity cap.
//
// The frozen contract's animalSpawnForColumn() takes no is_new_3ds flag and no cap, so the
// machine has to be inferred. main.c builds the store with
// entityWorldInit(&s_entities, entityCapFor(hwIsNew3ds())), so the store's cap IS the
// hardware answer: 24 on an Old 3DS, 48 on a New one. Reading it back is exact rather than a
// guess, and it keeps the spawner from needing a hardware probe in a file that must not
// include <3ds.h>. animal_test.c pins both directions.
static int animalCapForStore(const EntityWorld* ew)
{
	return animalCapFor(entityCap(ew) >= ENTITY_CAP_NEW);
}

// floor(), the same three lines entity.c, physics.c and world.h all carry for the same
// reason: (int) truncates towards zero, which is wrong for negative non-integers.
static inline int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

// ---------------------------------------------------------------------------------------
// Thinking
// ---------------------------------------------------------------------------------------

static float randomYaw(Rng* rng)
{
	// 16 bits of angle -- 0.0055 degrees of granularity, far finer than anything can see, and
	// it avoids a divide by drawing from a power of two.
	return (float)rngBelow(rng, 1u << 16) * (ANIMAL_TWO_PI / 65536.0f);
}

// Writes the horizontal velocity that walking along `yaw` at `speed` means. vy is left
// alone: gravity, water and the ground are bodyStep's business and this must not touch them.
static void driveAlongYaw(Entity* e, float speed)
{
	e->body.vx =  sinf(e->yaw) * speed;
	e->body.vz = -cosf(e->yaw) * speed;
}

static void enterIdle(Entity* e, Rng* rng)
{
	e->ai_state = ANIMAL_AI_IDLE;
	e->ai_timer = (uint16_t)(ANIMAL_IDLE_MIN_TICKS + rngBelow(rng, ANIMAL_IDLE_SPAN_TICKS));
	e->body.vx  = 0.0f;
	e->body.vz  = 0.0f;
}

static void enterWander(Entity* e, const AnimalDef* def, Rng* rng)
{
	e->ai_state = ANIMAL_AI_WANDER;
	e->ai_timer = (uint16_t)(ANIMAL_WANDER_MIN_TICKS + rngBelow(rng, ANIMAL_WANDER_SPAN_TICKS));
	e->yaw      = randomYaw(rng);
	driveAlongYaw(e, def->speed);
}

void animalThink(EntityWorld* ew, int slot, const World* w, float dt_s, void* user)
{
	// Both accepted to match EntityThinkFn and both genuinely unused -- see animal.h. The
	// decision clock is ai_timer, in ticks, which entityTick already counts down; a second
	// seconds-based clock alongside it would be two rates that can disagree.
	(void)w;
	(void)dt_s;

	Entity* e = entityAt(ew, slot);
	if (!e) return;

	// Not an animal. v1.8.16's monsters will share this pool and this same hook; they get
	// their own think and this one must leave them completely alone.
	const AnimalDef* def = animalDef(e->kind);
	if (!def) return;

	AnimalCtx* ctx = (AnimalCtx*)user;
	if (!ctx || !ctx->rng) return;   // no decision stream -- documented no-op, see animal.h

	Rng* rng = ctx->rng;

	if (e->ai_timer > 0) {
		// Mid-decision. The ONLY thing that interrupts a timer is walking into something:
		// bodyMove zeroed the blocked axis on the previous step, so a WANDER animal with no
		// horizontal speed left has hit a wall head-on and should turn now rather than push
		// against it for the rest of its timer. Blocked on one axis only, it still has speed
		// on the other and keeps sliding, which is correct.
		if (e->ai_state == ANIMAL_AI_WANDER &&
		    fabsf(e->body.vx) + fabsf(e->body.vz) < ANIMAL_STUCK_SPEED) {
			e->yaw = randomYaw(rng);
			driveAlongYaw(e, def->speed);
		}
		return;
	}

	switch (e->ai_state) {
	case ANIMAL_AI_WANDER:
	case ANIMAL_AI_FLEE:
		// Both active states rest afterwards. A fleeing animal that has outrun its 3 seconds
		// stops where it is rather than immediately picking a new direction, which is what
		// makes the flee read as an escape and not as a permanent panic.
		enterIdle(e, rng);
		break;

	case ANIMAL_AI_IDLE:
	default:
		// `default` is not dead code: ai_state is a uint8_t the store carries opaquely, so an
		// unknown value has to resolve to something. Resting is the safe answer.
		if (rngBelow(rng, 100) < ANIMAL_WANDER_CHANCE) enterWander(e, def, rng);
		else                                          enterIdle(e, rng);
		break;
	}
}

// ---------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------

// Which kinds each biome allows, indexed [biome][kind - 1]. Flat allow/deny, no weights: a
// weight table needs a divide or a float and buys nothing four kinds can notice.
//
// BIOME_DESERT is empty on purpose. It costs one row and it is what makes a desert feel like
// a desert instead of like plains with a different colour.
static const uint8_t kBiomeAllows[BIOME_COUNT][ENT_KIND_COUNT - 1] = {
	/*                 pig cow chk shp */
	/* BIOME_TUNDRA */ {  0,  0,  0,  1 },
	/* BIOME_TAIGA  */ {  0,  1,  0,  1 },
	/* BIOME_PLAINS */ {  1,  1,  1,  1 },
	/* BIOME_FOREST */ {  1,  1,  1,  1 },
	/* BIOME_DESERT */ {  0,  0,  0,  0 },
	/* BIOME_JUNGLE */ {  1,  0,  1,  0 },
};

// One kind allowed in `b`, uniformly, or ENT_NONE when the biome allows none.
static uint8_t pickHerdKind(BiomeId b, Rng* rng)
{
	// Both bounds in one unsigned compare, and NOT `(int)b < 0 || (int)b >= BIOME_COUNT`.
	// That spelling builds on the host and BREAKS THE CONSOLE BUILD: every BiomeId
	// enumerator is non-negative, ARM EABI defaults to -fshort-enums so the type is an
	// unsigned char there, and `(int)b < 0` is then provably always false --
	// -Wtype-limits rejects it under the Makefile's -Wall -Wextra -Werror (Makefile:58).
	// The host build cannot catch this, because there the enum is a signed int and the
	// comparison looks live. Casting to unsigned keeps the negative case genuinely
	// covered (a negative value wraps high and still fails the bound) while giving the
	// compiler a comparison that can actually go either way.
	if ((unsigned)b >= (unsigned)BIOME_COUNT) return ENT_NONE;

	uint8_t allowed[ENT_KIND_COUNT - 1];
	int n = 0;
	for (int k = 1; k < ENT_KIND_COUNT; k++)
		if (kBiomeAllows[b][k - 1]) allowed[n++] = (uint8_t)k;

	if (n == 0) return ENT_NONE;
	return allowed[rngBelow(rng, (uint32_t)n)];
}

// Can a member of this kind stand at (x, z)? One test, no retry.
static bool spotIsGood(const World* w, int x, int y, int z)
{
	const BlockId ground = worldGet(w, x, y - 1, z);
	if (ground != BLOCK_GRASS && ground != BLOCK_DIRT && ground != BLOCK_SAND) return false;

	// Two blocks of headroom. The tallest animal is 1.4 blocks, so one block of air would let
	// a cow spawn with its head inside the ceiling and bodyMove would spend every tick after
	// that shoving it out.
	if (worldGet(w, x, y,     z) != BLOCK_AIR) return false;
	if (worldGet(w, x, y + 1, z) != BLOCK_AIR) return false;

	return true;
}

int animalSpawnForColumn(EntityWorld* ew, World* w, const WorldGen* g,
                         int cx, int cz, Rng* rng)
{
	if (!ew || !w || !g || !rng) return 0;

	// The column must genuinely be live. worldGet() answers BLOCK_AIR for an unloaded chunk
	// rather than failing (world.h:93), so without this the ground test below would simply
	// reject every candidate and the caller would pay for the rolls anyway.
	if (worldColumn(w, cx, cz) == NULL) return 0;

	const int cap = animalCapForStore(ew);
	if (animalCount(ew) >= cap) return 0;

	if (rngBelow(rng, ANIMAL_HERD_CHANCE_DENOM) != 0) return 0;

	// The biome is asked once, at the column's centre, so the whole herd is one kind. It is
	// pure noise (worldgen.h:688) -- no chunk load, no allocation, safe to call here.
	const int32_t centre_x = (int32_t)((cx << 4) + 8);
	const int32_t centre_z = (int32_t)((cz << 4) + 8);
	const uint8_t kind = pickHerdKind(worldgenBiomeAt(g, centre_x, centre_z), rng);
	if (kind == ENT_NONE) return 0;

	const AnimalDef* def = animalDef(kind);
	if (!def) return 0;

	const int members = ANIMAL_HERD_MIN + (int)rngBelow(rng, ANIMAL_HERD_SPAN);

	int spawned = 0;
	for (int m = 0; m < members; m++) {
		// Re-checked per member, not once above, so a herd cannot straddle the cap.
		if (animalCount(ew) >= cap) break;

		const int x = (cx << 4) + (int)rngBelow(rng, CHUNK_DIM);
		const int z = (cz << 4) + (int)rngBelow(rng, CHUNK_DIM);
		const int y = worldgenHeight(g, (int32_t)x, (int32_t)z);

		if (y < 1 || y + 1 >= WORLD_HEIGHT) continue;
		if (!spotIsGood(w, x, y, z)) continue;

		const int slot = entitySpawn(ew, kind, (float)x + 0.5f, (float)y, (float)z + 0.5f,
		                             def->width, def->height, ANIMAL_EYE_FRAC);
		if (slot < 0) break;   // the pool or the entity cap said no; the rest will too

		Entity* e = entityAt(ew, slot);
		if (e) {
			// entitySpawn starts everything at 20 hp, ai_state 0 and ai_timer 0. 20 is the
			// player's scale and is not this animal's; ai_state 0 is already IDLE and needs
			// no write; ai_timer gets jitter so the herd does not decide in lockstep.
			e->health   = def->health;
			e->ai_state = ANIMAL_AI_IDLE;
			e->ai_timer = (uint16_t)rngBelow(rng, ANIMAL_SPAWN_JITTER_TICKS);
			e->yaw      = randomYaw(rng);
		}
		spawned++;
	}

	return spawned;
}

// ---------------------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------------------

int animalRaycast(const EntityWorld* ew, float ox, float oy, float oz,
                  float dx, float dy, float dz, float max_distance,
                  float* out_dist)
{
	if (out_dist) *out_dist = 0.0f;
	if (!ew) return -1;
	if (max_distance <= 0.0f) return -1;

	// Normalised here so out_dist is in blocks whatever the caller's vector length is, and so
	// a zero-length direction is refused rather than producing infinities in the slab test.
	const float len = sqrtf(dx * dx + dy * dy + dz * dz);
	if (!(len > 1.0e-6f)) return -1;   // written to also reject a NaN length
	const float inv = 1.0f / len;
	dx *= inv;
	dy *= inv;
	dz *= inv;

	int   best_slot = -1;
	float best_t    = max_distance;

	for (int i = 0; i < ENTITY_SLOTS; i++) {
		const Entity* e = entityGet(ew, i);
		if (!e) continue;

		const AnimalDef* def = animalDef(e->kind);
		if (!def) continue;

		// Body.y is FEET, not centre (physics.h:199), and half_w is the stored half-extent.
		const float min_x = e->body.x - e->body.half_w;
		const float max_x = e->body.x + e->body.half_w;
		const float min_y = e->body.y;
		const float max_y = e->body.y + e->body.height;
		const float min_z = e->body.z - e->body.half_w;
		const float max_z = e->body.z + e->body.half_w;

		// Slab test. tmin starts at 0 so a ray whose origin is already inside the box hits it
		// at distance 0 rather than missing or reporting a negative.
		float tmin = 0.0f;
		float tmax = best_t;   // nothing further than the current best can win anyway
		bool  miss = false;

		const float o[3] = { ox, oy, oz };
		const float d[3] = { dx, dy, dz };
		const float lo[3] = { min_x, min_y, min_z };
		const float hi[3] = { max_x, max_y, max_z };

		for (int a = 0; a < 3; a++) {
			if (fabsf(d[a]) < 1.0e-6f) {
				// Parallel to this slab: either the origin is inside it for the whole ray, or
				// the ray never enters. No division, which is the point of the branch.
				if (o[a] < lo[a] || o[a] > hi[a]) { miss = true; break; }
				continue;
			}
			const float invd = 1.0f / d[a];
			float t1 = (lo[a] - o[a]) * invd;
			float t2 = (hi[a] - o[a]) * invd;
			if (t1 > t2) { const float sw = t1; t1 = t2; t2 = sw; }
			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;
			if (tmin > tmax) { miss = true; break; }
		}
		if (miss) continue;

		// tmin <= tmax <= best_t is already guaranteed by the loop, so this is strictly the
		// nearer hit. `<` not `<=`: an exact tie keeps the lower slot index, which is stable.
		if (tmin < best_t) {
			best_t    = tmin;
			best_slot = i;
		}
	}

	if (best_slot >= 0 && out_dist) *out_dist = best_t;
	return best_slot;
}

bool animalHurt(EntityWorld* ew, int slot, uint8_t damage,
                float from_x, float from_z,
                uint8_t* out_drop_item, uint8_t* out_drop_count)
{
	// Written FIRST and unconditionally, so every early return below leaves the caller with a
	// defined "nothing dropped" rather than with whatever was in its locals.
	if (out_drop_item)  *out_drop_item  = 0;
	if (out_drop_count) *out_drop_count = 0;

	Entity* e = entityAt(ew, slot);
	if (!e) return false;

	const AnimalDef* def = animalDef(e->kind);
	if (!def) return false;

	if (damage >= e->health) {
		e->health = 0;

		// The drop count. rngHash3 over the id and the integer position rather than a stream,
		// because the frozen contract for this function carries no Rng -- see animal.h. The
		// id changes on every spawn, so two kills never share a roll unless they are the same
		// entity in the same block, which cannot happen twice.
		Rng r;
		rngSeed(&r, rngHash3((uint32_t)e->id,
		                     (int32_t)floorToInt(e->body.x),
		                     (int32_t)floorToInt(e->body.y),
		                     (int32_t)floorToInt(e->body.z)));

		uint32_t count = def->drop_min;
		if (def->drop_max > def->drop_min) {
			const uint32_t span = (uint32_t)def->drop_max - (uint32_t)def->drop_min + 1u;
			count = (uint32_t)def->drop_min + rngBelow(&r, span);
		}

		if (out_drop_item)  *out_drop_item  = def->drop_item;
		if (out_drop_count) *out_drop_count = (uint8_t)count;

		// The DEFERRED kill, so this is safe even from inside a think (entity.h:150-152).
		entityKill(ew, slot);
		return true;
	}

	// Clamped by the branch above, so this subtraction cannot underflow the uint8_t. An
	// unclamped `e->health -= damage` on a 2 hp chicken hit for 4 gives 254, and the animal
	// becomes unkillable -- which is why the test for this asserts the value, not just that
	// the animal survived.
	e->health = (uint8_t)(e->health - damage);

	// Face directly AWAY from the hitter. atan2f(dx, -dz) is the inverse of the project's
	// forward(yaw) = (sin yaw, -cos yaw) -- see the convention block at the top of this file.
	const float away_x = e->body.x - from_x;
	const float away_z = e->body.z - from_z;
	if (away_x != 0.0f || away_z != 0.0f) e->yaw = atan2f(away_x, -away_z);

	e->ai_state = ANIMAL_AI_FLEE;
	e->ai_timer = ANIMAL_FLEE_TICKS;
	driveAlongYaw(e, def->speed * ANIMAL_FLEE_SPEED_MUL);

	return false;
}
