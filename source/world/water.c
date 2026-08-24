// Water physics — implementation. See water.h for the design and for why the flow level is
// not a block id.
#include "world/water.h"

#include <string.h>

// ── Cell keys ────────────────────────────────────────────────────────────────────────
//
// One 64-bit key per cell so the map and the queue-dedup set are both plain open-addressed
// tables over a scalar. 25 bits each of x and z (+-16.7 M, comfortably past the +-60000 that
// net/networld.c's NETWORKD_XZ_LIMIT already refuses) and 7 bits of y (the world is 128 tall).
// The tag bit is what makes 0 usable as "empty": no real cell can pack to zero.
#define WATER_KEY_TAG   (1ULL << 62)
#define WATER_KEY_XZ_M  0x01FFFFFFu
#define WATER_KEY_XZ_SB 0x01000000u

static uint64_t waterKey(int x, int y, int z)
{
	const uint64_t ux = (uint64_t)((uint32_t)x & WATER_KEY_XZ_M);
	const uint64_t uz = (uint64_t)((uint32_t)z & WATER_KEY_XZ_M);
	return WATER_KEY_TAG | (ux << 32) | (uz << 7) | (uint64_t)((uint32_t)y & 0x7Fu);
}

static void waterUnkey(uint64_t k, int* x, int* y, int* z)
{
	uint32_t ux = (uint32_t)((k >> 32) & WATER_KEY_XZ_M);
	uint32_t uz = (uint32_t)((k >> 7)  & WATER_KEY_XZ_M);
	if (ux & WATER_KEY_XZ_SB) ux |= ~WATER_KEY_XZ_M;   // sign-extend the 25-bit field
	if (uz & WATER_KEY_XZ_SB) uz |= ~WATER_KEY_XZ_M;
	*x = (int)(int32_t)ux;
	*z = (int)(int32_t)uz;
	*y = (int)(uint32_t)(k & 0x7Fu);
}

// splitmix64's finaliser. A key is mostly low-entropy in its low bits (y, and z's bottom
// bits) and two neighbouring cells differ by 1, so the table would degenerate into one long
// probe chain under a mask of the raw key.
static uint32_t waterHash(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xFF51AFD7ED558CCDULL;
	k ^= k >> 29;
	k *= 0xC4CEB9FE1A85EC53ULL;
	k ^= k >> 32;
	return (uint32_t)k;
}

// ── The flow-cell map ────────────────────────────────────────────────────────────────

static int mapFind(const WaterSim* s, uint64_t key)
{
	uint32_t i = waterHash(key) & (WATER_MAP_SLOTS - 1u);
	for (int probe = 0; probe < WATER_MAP_SLOTS; probe++) {
		if (s->map_key[i] == 0)   return -1;
		if (s->map_key[i] == key) return (int)i;
		i = (i + 1u) & (WATER_MAP_SLOTS - 1u);
	}
	return -1;
}

// Insert or update. False only when the table is genuinely full, which is counted rather
// than hidden — see waterMapFull().
static bool mapPut(WaterSim* s, uint64_t key, uint8_t level)
{
	uint32_t i = waterHash(key) & (WATER_MAP_SLOTS - 1u);
	for (int probe = 0; probe < WATER_MAP_SLOTS; probe++) {
		if (s->map_key[i] == key) { s->map_lvl[i] = level; return true; }
		if (s->map_key[i] == 0) {
			s->map_key[i] = key;
			s->map_lvl[i] = level;
			s->map_count++;
			return true;
		}
		i = (i + 1u) & (WATER_MAP_SLOTS - 1u);
	}
	s->map_full++;
	return false;
}

// Knuth 6.4 algorithm R, exactly as world.c's worldColumnRemove does it and for the same
// reason: this table has no tombstones, so emptying a slot in the middle of a probe chain
// would cut every entry behind it out of the map while its level is still meaningful.
static void mapEraseAt(WaterSim* s, uint32_t i)
{
	s->map_key[i] = 0;
	s->map_count--;

	uint32_t j = i;
	for (;;) {
		j = (j + 1u) & (WATER_MAP_SLOTS - 1u);
		if (s->map_key[j] == 0) break;

		const uint32_t k = waterHash(s->map_key[j]) & (WATER_MAP_SLOTS - 1u);
		const bool movable = (i <= j) ? (k <= i || k > j) : (k <= i && k > j);
		if (!movable) continue;

		s->map_key[i] = s->map_key[j];
		s->map_lvl[i] = s->map_lvl[j];
		s->map_key[j] = 0;
		i = j;
	}
}

static void mapErase(WaterSim* s, uint64_t key)
{
	const int i = mapFind(s, key);
	if (i >= 0) mapEraseAt(s, (uint32_t)i);
}

// ── The candidate queue ──────────────────────────────────────────────────────────────
//
// A ring plus a dedup set, so the six neighbours a change pushes collapse to one entry when
// they are already waiting. Without the set, a spreading front would push the same cell once
// per neighbour and the ring would fill with duplicates of work already scheduled.

static int qsetFind(const WaterSim* s, uint64_t key)
{
	uint32_t i = waterHash(key) & (WATERQ_SET_SLOTS - 1u);
	for (int probe = 0; probe < WATERQ_SET_SLOTS; probe++) {
		if (s->qset[i] == 0)   return -1;
		if (s->qset[i] == key) return (int)i;
		i = (i + 1u) & (WATERQ_SET_SLOTS - 1u);
	}
	return -1;
}

static void qsetEraseAt(WaterSim* s, uint32_t i)
{
	s->qset[i] = 0;

	uint32_t j = i;
	for (;;) {
		j = (j + 1u) & (WATERQ_SET_SLOTS - 1u);
		if (s->qset[j] == 0) break;

		const uint32_t k = waterHash(s->qset[j]) & (WATERQ_SET_SLOTS - 1u);
		const bool movable = (i <= j) ? (k <= i || k > j) : (k <= i && k > j);
		if (!movable) continue;

		s->qset[i] = s->qset[j];
		s->qset[j] = 0;
		i = j;
	}
}

// True when the cell is now queued — whether this call put it there or a previous one did.
// False only when the ring is full, which is counted (waterQueueFull) and is not a permanent
// loss: every level is recomputed from its neighbours, never accumulated, so a candidate that
// was refused is corrected the next time anything beside it moves. That is the structural
// difference from the mesh-queue drop world/meshq.h was written for, where the refused work
// was the only thing that would ever have built that chunk.
static bool qPush(WaterSim* s, uint64_t key)
{
	if (qsetFind(s, key) >= 0) return true;
	if (s->qcount >= WATERQ_CAP) { s->q_full++; return false; }

	const int slot = (s->qhead + s->qcount) & (WATERQ_CAP - 1);
	s->ring[slot] = key;
	s->qcount++;

	uint32_t i = waterHash(key) & (WATERQ_SET_SLOTS - 1u);
	for (int probe = 0; probe < WATERQ_SET_SLOTS; probe++) {
		if (s->qset[i] == 0) { s->qset[i] = key; return true; }
		i = (i + 1u) & (WATERQ_SET_SLOTS - 1u);
	}
	return true;   // unreachable: the set is twice the ring
}

static bool qPop(WaterSim* s, uint64_t* out)
{
	if (s->qcount <= 0) return false;
	const uint64_t key = s->ring[s->qhead];
	s->qhead = (s->qhead + 1) & (WATERQ_CAP - 1);
	s->qcount--;

	const int i = qsetFind(s, key);
	if (i >= 0) qsetEraseAt(s, (uint32_t)i);

	*out = key;
	return true;
}

// ── The rule ─────────────────────────────────────────────────────────────────────────

static uint8_t levelOfKey(const WaterSim* s, const World* w, int x, int y, int z)
{
	if (worldGet(w, x, y, z) != (BlockId)BLOCK_WATER) return 0;
	const int i = mapFind(s, waterKey(x, y, z));
	return (i >= 0) ? s->map_lvl[i] : (uint8_t)WATER_LEVEL_SOURCE;
}

uint8_t waterLevelAt(const WaterSim* s, const World* w, int x, int y, int z)
{
	return levelOfKey(s, w, x, y, z);
}

// What the water at (x, y, z) contributes to its four side neighbours, or 0 for "nothing".
//
// One rule does all the work, and it was arrived at by running the thing, not by reading it:
// WATER THAT CAN STILL GO DOWN DOES NOT ALSO GO SIDEWAYS. Beta 1.7.3 makes the same choice, and
// it is the only reason a waterfall is one block wide instead of a solid block of water fifteen
// across and as tall as the drop — which is exactly what the first draft produced.
//
// "Can still go down" is not just "the cell below is air". It is:
//
//   * the cell below is air, or
//   * the cell below is a FLOW cell — water that is itself on its way somewhere.
//
// and it is deliberately NOT "the cell below is a source". That distinction is the whole
// difference between a waterfall and an ocean, and both cases matter:
//
//   A waterfall's source has water under it the moment the first cell of the column fills. Test
//   only for air and the source starts spreading at its own height on tick two, each of those
//   cells falls, and the fall becomes a wall.
//
//   An ocean's interior cells also have water under them — sources, settled, going nowhere. A
//   wall dug open at mid-depth has to let them out, so they must spread.
//
// The cell where a fall LANDS is outside the rule (rock below), so it spreads, which is what
// turns a fall into a pool.
static uint8_t sideFeed(const WaterSim* s, const World* w, int x, int y, int z)
{
	if (worldGet(w, x, y, z) != (BlockId)BLOCK_WATER) return 0;

	// y - 1 below the world reads as WORLD_FLOOR_BLOCK, not air, so the bottom layer of the
	// world spreads instead of draining into nothing.
	const BlockId below = worldGet(w, x, y - 1, z);
	if (below == (BlockId)BLOCK_AIR) return 0;
	if (below == (BlockId)BLOCK_WATER && mapFind(s, waterKey(x, y - 1, z)) >= 0) return 0;

	const int idx = mapFind(s, waterKey(x, y, z));
	return (idx >= 0) ? s->map_lvl[idx] : (uint8_t)WATER_LEVEL_SOURCE;
}

// What this cell's level SHOULD be, read only from its neighbours. 0 means "no water here".
static uint8_t desiredLevel(const WaterSim* s, const World* w, int x, int y, int z)
{
	const BlockId b = worldGet(w, x, y, z);

	// Only air is displaced. A plant is left alone deliberately: washing tall grass away is
	// a separate behaviour nobody asked for here, and quietly deleting a block the player can
	// see is the kind of scope creep that has to be asked about first, not shipped.
	if (b != (BlockId)BLOCK_AIR && b != (BlockId)BLOCK_WATER) return 0;

	// A source is pinned and is never recomputed. This is the one place the "no map entry
	// means source" rule is read, and it is what makes an ocean free.
	if (b == (BlockId)BLOCK_WATER && mapFind(s, waterKey(x, y, z)) < 0)
		return (uint8_t)WATER_LEVEL_SOURCE;

	// Fed from directly above at full strength, whatever the level up there: a waterfall does
	// not get weaker as it falls, and this is also what makes vertical fill cost one extra
	// neighbour read instead of a second code path.
	if (y + 1 < WORLD_HEIGHT && worldGet(w, x, y + 1, z) == (BlockId)BLOCK_WATER)
		return (uint8_t)WATER_LEVEL_MAX;

	uint8_t best = 0;
	static const int dx[4] = { 1, -1, 0, 0 };
	static const int dz[4] = { 0, 0, 1, -1 };
	for (int i = 0; i < 4; i++) {
		const uint8_t l = sideFeed(s, w, x + dx[i], y, z + dz[i]);
		if (l > 1 && (uint8_t)(l - 1) > best) best = (uint8_t)(l - 1);
	}
	return best;
}

// Recomputes one cell and writes the result. `*block_moved` is set when the BLOCK id changed,
// as opposed to only the level inside an unchanged block of water.
static bool applyCell(WaterSim* s, World* w, int x, int y, int z, bool* block_moved)
{
	*block_moved = false;
	if (y < 0 || y >= WORLD_HEIGHT) return false;

	const uint64_t key  = waterKey(x, y, z);
	const BlockId  b    = worldGet(w, x, y, z);
	const uint8_t  want = desiredLevel(s, w, x, y, z);

	if (b == (BlockId)BLOCK_WATER) {
		const int idx = mapFind(s, key);
		if (idx < 0) return false;                    // a source: pinned, nothing to do
		if (want == 0) {
			if (!worldSet(w, x, y, z, (BlockId)BLOCK_AIR)) return false;
			mapEraseAt(s, (uint32_t)idx);
			*block_moved = true;
			return true;
		}
		if (want == s->map_lvl[idx]) return false;
		s->map_lvl[idx] = want;
		return true;
	}

	// Something solid is here now, and if it replaced a flow cell there is a map entry naming
	// a cell that is no longer water. Found by measurement, not by reading: placing a block
	// into flowing water leaked one slot every time, and 2048 slots is a few hundred placed
	// blocks. A stale entry is worse than a wasted slot — the cell it names would read back as
	// a flow cell if water ever returned there, at whatever level it happened to be stuck on.
	//
	// Reported as a change so the neighbours get re-examined (they have just lost a feeder),
	// but block_moved stays false: nothing about the world moved, so nothing needs remeshing.
	if (b != (BlockId)BLOCK_AIR) {
		const int idx = mapFind(s, key);
		if (idx < 0) return false;
		mapEraseAt(s, (uint32_t)idx);
		return true;
	}

	if (want == 0) return false;

	// The map entry goes in FIRST. A cell that held BLOCK_WATER with no entry would read as a
	// source for however long the gap lasted, and a source spreads — so the order here is the
	// difference between a flow and a flood.
	if (!mapPut(s, key, want)) return false;
	if (!worldSet(w, x, y, z, (BlockId)BLOCK_WATER)) { mapErase(s, key); return false; }

	*block_moved = true;
	return true;
}

// ── Public API ───────────────────────────────────────────────────────────────────────

void waterInit(WaterSim* s)
{
	memset(s, 0, sizeof *s);
}

void waterSetLevelHook(WaterSim* s, WaterChangeFn fn, void* ud)
{
	s->level_fn = fn;
	s->level_ud = ud;
}

// The two files agree on what a level MEANS, and the agreement is checked here rather than
// commented about: world/scratch.h's band uses 0 for a full cube and 1..7 for a flow level, and
// world/mesher.c turns level L into a surface L/SCRATCH_WATER_STEPS high. If the source level
// and the mesher's denominator ever disagreed, a settled ocean would render a fraction of a
// block short and nothing would fail.
_Static_assert(SCRATCH_WATER_STEPS == WATER_LEVEL_SOURCE,
               "the mesher's height denominator must be the source level");

void waterFillScratch(const WaterSim* s, MeshScratch* ms, int cx, int cy, int cz)
{
	ms->water_any = false;
	if (s->map_count <= 0) return;

	// The scratch's own corner in world coordinates: chunk origin minus the one-block skirt.
	const int x0 = cx * CHUNK_DIM - 1;
	const int y0 = cy * CHUNK_DIM - 1;
	const int z0 = cz * CHUNK_DIM - 1;

	int written = 0;
	for (uint32_t i = 0; i < WATER_MAP_SLOTS; i++) {
		if (s->map_key[i] == 0) continue;

		int x, y, z;
		waterUnkey(s->map_key[i], &x, &y, &z);

		const unsigned sx = (unsigned)(x - x0);
		const unsigned sy = (unsigned)(y - y0);
		const unsigned sz = (unsigned)(z - z0);
		if (sx >= (unsigned)SCRATCH_DIM || sy >= (unsigned)SCRATCH_DIM ||
		    sz >= (unsigned)SCRATCH_DIM)
			continue;

		// Cleared on the first hit, not up front: a chunk with no flow cell anywhere near it —
		// which is nearly all of them, nearly all the time — must not pay a 5,832 byte memset
		// for an answer of "nothing".
		if (!written) memset(ms->water, 0, sizeof ms->water);
		written++;

		ms->water[scratchIndex((int)sx, (int)sy, (int)sz)] = s->map_lvl[i];
	}

	ms->water_any = written > 0;
}

void waterNotify(WaterSim* s, int x, int y, int z)
{
	if (s->dropping) return;   // see WaterSim.dropping in water.h

	qPush(s, waterKey(x, y, z));
	if (y + 1 < WORLD_HEIGHT) qPush(s, waterKey(x, y + 1, z));
	if (y - 1 >= 0)           qPush(s, waterKey(x, y - 1, z));
	qPush(s, waterKey(x + 1, y, z));
	qPush(s, waterKey(x - 1, y, z));
	qPush(s, waterKey(x, y, z + 1));
	qPush(s, waterKey(x, y, z - 1));
}

int waterTick(WaterSim* s, World* w, int budget, WaterChangeFn on_change, void* ud)
{
	int changed = 0;
	for (int n = 0; n < budget; n++) {
		uint64_t key;
		if (!qPop(s, &key)) break;

		int x, y, z;
		waterUnkey(key, &x, &y, &z);
		s->examined++;

		bool block_moved = false;
		if (!applyCell(s, w, x, y, z, &block_moved)) continue;

		s->changed++;
		changed++;

		// A cell that moved can make any of its six neighbours newly eligible — including the
		// one above it, which is how a column of falling water knows to drain when the source
		// under it goes.
		waterNotify(s, x, y, z);

		if (block_moved && on_change) {
			on_change(ud, x, y, z, worldGet(w, x, y, z));
		} else if (!block_moved && s->level_fn) {
			// v1.8.0 task 22b. The level moved inside water that was already there, which is a
			// visible change now that the level is a height. The block test is not paranoia:
			// applyCell also reports a change when it erases a stale map entry for a cell some
			// other code path filled with a solid block, and nothing about that cell is water.
			if (worldGet(w, x, y, z) == (BlockId)BLOCK_WATER)
				s->level_fn(s->level_ud, x, y, z, (BlockId)BLOCK_WATER);
		}
	}
	return changed;
}

int waterSettle(WaterSim* s, World* w, int max_ticks, WaterChangeFn on_change, void* ud)
{
	for (int t = 0; t < max_ticks; t++) {
		if (s->qcount == 0) return t;
		(void)waterTick(s, w, WATER_TICK_BUDGET, on_change, ud);
	}
	return (s->qcount == 0) ? max_ticks : -1;
}

int waterPending(const WaterSim* s)   { return s->qcount; }
int waterFlowCells(const WaterSim* s) { return s->map_count; }

uint32_t waterMapFull(const WaterSim* s)   { return s->map_full; }
uint32_t waterQueueFull(const WaterSim* s) { return s->q_full; }
uint32_t waterExamined(const WaterSim* s)  { return s->examined; }
uint32_t waterChanged(const WaterSim* s)   { return s->changed; }

int waterDropColumn(WaterSim* s, World* w, int cx, int cz)
{
	int cleared = 0;

	s->dropping = true;

	// Walked by slot rather than by cell because the map is the only record of which cells are
	// flow cells; there is no per-column index and building one would cost more than this scan,
	// which runs once per unloading column against a table of 2048 scalars.
	for (uint32_t i = 0; i < WATER_MAP_SLOTS; i++) {
		if (s->map_key[i] == 0) continue;

		int x, y, z;
		waterUnkey(s->map_key[i], &x, &y, &z);
		if ((x >> 4) != cx || (z >> 4) != cz) continue;

		if (worldGet(w, x, y, z) == (BlockId)BLOCK_WATER)
			(void)worldSet(w, x, y, z, (BlockId)BLOCK_AIR);

		mapEraseAt(s, i);
		cleared++;

		// mapEraseAt back-shifts, so the entry that was at some later slot may now be at `i`.
		// Re-examine this slot rather than walking past it.
		i--;
	}

	s->dropping = false;
	return cleared;
}
