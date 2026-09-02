// See entity.h for the four promises this file exists to keep. This is the arithmetic.
//
// No <math.h>, matching source/world/: floorToInt is a one-liner here rather than a libm
// call, so the host suite links against plain gcc and cannot pick up float behaviour the
// ARM11 would not reproduce.
#include "entity/entity.h"

#include "world/chunk.h"

// floor(), the same three lines physics.c and world.h both carry for the same reason:
// (int) truncates towards zero, which is wrong for negative non-integers, and an entity at
// x = -0.5 is in column -1, not column 0.
static inline int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

void entityWorldInit(EntityWorld* ew, int cap)
{
	if (!ew) return;

	for (int i = 0; i < ENTITY_SLOTS; i++) {
		ew->e[i].kind = ENT_NONE;
		ew->e[i].id   = 0;
	}

	ew->count = 0;

	// Clamped, not rejected. `cap` reaches here from a hardware probe, and a store that
	// silently never spawns because someone passed 100 is a worse outcome than one that
	// quietly holds the 48 it actually has room for.
	if (cap < 0) cap = 0;
	if (cap > ENTITY_SLOTS) cap = ENTITY_SLOTS;
	ew->cap = cap;

	// 1, not 0: id 0 is the "no entity" value that entityDespawn writes into a freed slot,
	// so it must never be issued to a live one.
	ew->next_id = 1;
}

int entityCapFor(bool is_new_3ds)
{
	return is_new_3ds ? ENTITY_CAP_NEW : ENTITY_CAP_OLD;
}

// Is this id currently held by a live entity? Ids are issued monotonically and wrap at 16
// bits, so after 65,535 spawns in one session the counter comes back round to a value that
// may still be in use. At most ENTITY_SLOTS of the 65,535 can be live at once, so skipping
// the collisions is bounded and cheap, and it is what lets entity.h promise that a handle
// is unique among live entities rather than merely "probably".
static bool idInUse(const EntityWorld* ew, uint16_t id)
{
	for (int i = 0; i < ENTITY_SLOTS; i++)
		if (ew->e[i].kind != ENT_NONE && ew->e[i].id == id)
			return true;
	return false;
}

static uint16_t issueId(EntityWorld* ew)
{
	// Bounded at ENTITY_SLOTS + 1 attempts rather than looping until it finds one. At most
	// 48 ids are live, so 49 attempts cannot all collide -- but "cannot" is exactly the
	// reasoning that produces an unbounded loop on a handheld when it turns out it can.
	for (int attempt = 0; attempt <= ENTITY_SLOTS; attempt++) {
		const uint16_t id = ew->next_id;

		ew->next_id++;
		if (ew->next_id == 0) ew->next_id = 1;   // skip the reserved value on wrap

		if (!idInUse(ew, id)) return id;
	}
	return 0;
}

int entitySpawn(EntityWorld* ew, uint8_t kind, float x, float y, float z,
                float width, float height, float eye_frac)
{
	if (!ew) return -1;

	// ENT_NONE would be a slot that is free the instant it is claimed -- count would say
	// one thing and the scan another. Refused rather than stored: a caller passing it has
	// a bug, and the honest place to find that bug is the -1 return.
	if (kind == ENT_NONE) return -1;

	if (ew->count >= ew->cap) return -1;

	int slot = -1;
	for (int i = 0; i < ENTITY_SLOTS; i++) {
		if (ew->e[i].kind == ENT_NONE) { slot = i; break; }
	}
	// Reachable only if cap > the number of genuinely free slots, which entityWorldInit's
	// clamp and the count above between them rule out -- but the scan is the authority on
	// what is free and the count is only a cache of it, so the scan gets the last word.
	if (slot < 0) return -1;

	const uint16_t id = issueId(ew);
	if (id == 0) return -1;

	Entity* e = &ew->e[slot];

	// bodyInit first, then bodySetBox: bodyInit installs the PLAYER box, and bodySetBox
	// overrides whichever dimensions were actually asked for. Passing 0 for a dimension
	// therefore means "keep the player's", which is the useful default for a placeholder
	// creature and is what makes a box optional rather than a thing every caller must know.
	bodyInit(&e->body, x, y, z);
	bodySetBox(&e->body, width, height, eye_frac);

	e->yaw      = 0.0f;
	e->id       = id;
	e->ai_timer = 0;
	e->kind     = kind;
	e->health   = 20;      // the 0..20 scale BS_APP_PLAYER_STATE already uses
	e->flags    = 0;
	e->ai_state = 0;

	ew->count++;
	return slot;
}

bool entityDespawn(EntityWorld* ew, int slot)
{
	if (!ew || slot < 0 || slot >= ENTITY_SLOTS) return false;
	if (ew->e[slot].kind == ENT_NONE) return false;

	ew->e[slot].kind  = ENT_NONE;

	// Cleared, and the honest account of why: this is the SECOND line of defence, not the
	// first. entityFindById skips any slot whose kind is ENT_NONE, so a stale id left in a
	// free slot could never have been matched anyway, and entitySpawn overwrites the id
	// before the slot goes live again. Measured: a red arm that deleted this line left all
	// 145 checks green, which is exactly what an unenforced line looks like.
	//
	// It is kept, and a check was added that reads the raw slot rather than going through
	// entityFindById, because the alternative -- deleting it -- would make the id field's
	// value in a free slot undefined, and idInUse() walks every slot including free ones on
	// its way to deciding whether a handle is available. Leaving a live-looking id in a
	// dead slot is a trap for the next person to widen that scan.
	ew->e[slot].id    = 0;
	ew->e[slot].flags = 0;

	ew->count--;
	return true;
}

bool entityKill(EntityWorld* ew, int slot)
{
	if (!ew || slot < 0 || slot >= ENTITY_SLOTS) return false;
	if (ew->e[slot].kind == ENT_NONE) return false;

	ew->e[slot].flags |= ENT_F_DESPAWN;
	return true;
}

Entity* entityAt(EntityWorld* ew, int slot)
{
	if (!ew || slot < 0 || slot >= ENTITY_SLOTS) return NULL;
	return (ew->e[slot].kind != ENT_NONE) ? &ew->e[slot] : NULL;
}

const Entity* entityGet(const EntityWorld* ew, int slot)
{
	if (!ew || slot < 0 || slot >= ENTITY_SLOTS) return NULL;
	return (ew->e[slot].kind != ENT_NONE) ? &ew->e[slot] : NULL;
}

int entityFindById(const EntityWorld* ew, uint16_t id)
{
	if (!ew || id == 0) return -1;

	for (int i = 0; i < ENTITY_SLOTS; i++)
		if (ew->e[i].kind != ENT_NONE && ew->e[i].id == id)
			return i;

	return -1;
}

int entityCount(const EntityWorld* ew) { return ew ? ew->count : 0; }
int entityCap  (const EntityWorld* ew) { return ew ? ew->cap   : 0; }

// The squared horizontal distance from the player, as the int32 tickPeriodForDistSq wants.
//
// Computed in float and converted once, rather than in int, because the entity's position
// IS a float and rounding it to a block first would put an entity at 24.4 blocks in the
// near tier on one axis and the far tier on another. The conversion cannot overflow at any
// distance an entity can reach -- the loaded ring is radius + 1 columns, so the largest
// legitimate value is about 1.5e5 against an int32 ceiling of 2.1e9 -- and if a caller ever
// does hand it something absurd, tickPeriodForDistSq guards its own negative case and
// answers with the CHEAP period, which is the safe reading of "so far away the arithmetic
// wrapped".
static int32_t distSqToPlayer(const Entity* e, float px, float pz)
{
	const float dx = e->body.x - px;
	const float dz = e->body.z - pz;
	const float d2 = dx * dx + dz * dz;

	// Clamped before the cast, because converting a float larger than INT32_MAX to int32 is
	// undefined behaviour in C, not a wrap. 2.0e9 is under the ceiling with room to spare.
	if (d2 >= 2.0e9f) return 2000000000;
	return (int32_t)d2;
}

int entityTickPeriod(const Entity* e, float px, float pz)
{
	if (!e) return TICK_FAR_PERIOD;
	return tickPeriodForDistSq(distSqToPlayer(e, px, pz));
}

bool entityTickDue(const Entity* e, uint64_t tick, float px, float pz)
{
	if (!e) return false;
	return tickDue(tick, entityTickPeriod(e, px, pz), (uint32_t)e->id);
}

EntityTickStats entityTick(EntityWorld* ew, const World* w, uint64_t tick,
                           float px, float pz, EntityThinkFn think, void* user)
{
	EntityTickStats st = { 0, 0, 0, 0, 0 };
	if (!ew || !w) return st;

	for (int i = 0; i < ENTITY_SLOTS; i++) {
		Entity* e = &ew->e[i];
		if (e->kind == ENT_NONE) continue;

		// The lifetime rule, first and before any physics. See entityTick's comment in
		// entity.h for why this is a despawn CONDITION rather than a hook into the column
		// unload path. One hash lookup per entity per tick.
		//
		// >> 4 rather than / CHUNK_DIM: an arithmetic shift floors, which is what a
		// negative coordinate needs and what world.h's own conventions specify.
		const int cx = floorToInt(e->body.x) >> 4;
		const int cz = floorToInt(e->body.z) >> 4;
		if (worldColumn(w, cx, cz) == NULL) {
			entityDespawn(ew, i);
			st.unloaded++;
			continue;
		}

		st.live++;

		// Somebody else's entity. Stored, drawn, collided against, never simulated.
		if (e->flags & ENT_F_REMOTE) continue;

		const int period = tickPeriodForDistSq(distSqToPlayer(e, px, pz));
		if (!tickDue(tick, period, (uint32_t)e->id)) { st.decimated++; continue; }
		st.thought++;   // "was due"; the think below is optional, the schedule is not

		// Counted down here because it is the one piece of state every AI wants and none
		// would implement differently. Never read in this file -- what a zero means is the
		// creature lane's business.
		if (e->ai_timer > 0) e->ai_timer--;

		if (think) {
			think(ew, i, w, (float)period * ENTITY_DT, user);

			// A think that despawned this entity outright (rather than through
			// entityKill) leaves nothing to step. Re-read rather than assumed: the
			// callback holds the store and may do anything to it.
			if (e->kind == ENT_NONE) continue;
		}

		if (e->flags & ENT_F_DESPAWN) { entityDespawn(ew, i); continue; }

		// `period` steps of exactly ENTITY_DT, never one step of period * ENTITY_DT.
		// entity.h's entityTick comment has the whole argument and the measurement behind
		// it; the short version is that bodyMove subdivides by distance, so scaling dt
		// moves the same work into a lumpier call and makes a merely resting entity sweep
		// several blocks of gravity it never actually falls.
		for (int k = 0; k < period; k++) {
			bodyStep(&e->body, w, ENTITY_DT);
			st.steps++;
		}
	}

	return st;
}
