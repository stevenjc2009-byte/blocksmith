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
//
// ── What happens when the ring is full, and why the OLDEST entry is the one that goes ──
//
// v1.8.3. This comment used to say that a refused candidate "is not a permanent loss: every level
// is recomputed from its neighbours, never accumulated, so a candidate that was refused is
// corrected the next time anything beside it moves." That was false, and it was false in the one
// case that matters most — a source the player has just placed into a world where nothing else is
// moving. Nothing beside it ever moves again, so the correction never comes, and the source sits
// there as a lone full cube for ever while waterPending(), waterFlowCells() and the settle count
// all report a simulation in perfect health.
//
// It is not recoverable after the fact either, and the reason is structural rather than an
// oversight: a SOURCE has no map entry (see the absence-means-source rule at the top of water.h),
// so once its notify is dropped the simulation holds no record anywhere that the cell exists.
// There is no bounded set to re-offer, and the only thing that would find it again is a scan of
// the world. That rules out the obvious repair — flag the overflow and rescan the dirty region on
// a later tick — because the region is "wherever the water is", which is the whole world.
//
// So the fix is at the moment of the loss, not after it: when the ring is full, EVICT THE OLDEST
// entry and admit the new one, rather than refusing the new one and keeping 1024 stale ones. The
// count in waterQueueFull() means exactly what it always meant — one candidate lost — but the
// candidate that is lost is now the one furthest from what just happened, instead of the one
// closest to it. That is not self-healing and this comment does not claim it is; it is a choice
// about WHICH loss to take, and it is made on the measurement below.
//
// MEASURED on 2026-08-25, on the real generator and the real edit hook, 200 arms sweeping five
// seeds against 1..40 streamed columns: a source placed through worldSet after streaming failed to
// spread in 16 of 200 arms (8.0%) under refuse-newest, every one of them a lone unspread cube, and
// in 0 of 200 (0.0%) under evict-oldest. A genuine pour that overflows on its own merits reaches a
// BYTE-IDENTICAL final world under both policies (25 basin sources dropped at once: 816 flow
// cells, hash 57907c9cc2a4b240 either way), so the eviction is not buying that at the cost of the
// ordinary case. And nothing that never overflows is touched at all, which is why every pinned
// number in world/water_test.c — the four order-independence hashes included — is unmoved by this
// change except the one that counts refusals during a fixture build.
//
// v1.8.3, later the same day: the flood that produced that 8.0% HAS since been fixed, outside this
// file — world.h's edit hook now carries the World* and main.c's onWorldEdit drops any world that
// is not s_world, so app/worker.c's staging generation no longer feeds this ring. See the block
// above waterNotify() for what that was and what it cost. This eviction is therefore no longer the
// everyday load-bearing fix; it is the safety net for overflows that arise on their own merits, and
// those still exist — handbuiltFill() alone is 51364 firings on the MAIN thread, a large diff drain
// on rejoin is another, a big player-built basin is a third. The ARGUMENT above is untouched by the
// flood going away: it is a structural asymmetry in this file (a source has no map entry, so a
// dropped notify for one is unrecoverable), so refusing-the-newest is the wrong loss to take for
// ANY overflow whatever caused it. And it costs nothing — the byte-identical-final-world result
// above is what says so. Kept deliberately, demoted deliberately, not left behind as a workaround.
//
// gen_remaining is decremented with the eviction because the head of the ring belongs to the
// generation that is currently open (water.h's WATER_SPREAD_TICKS barrier). Leaving it alone would
// leave the barrier counting an entry that is no longer there, and waterTick's own defensive
// branch would then have to mop it up a pop later.
static bool qPop(WaterSim* s, uint64_t* out);

static bool qPush(WaterSim* s, uint64_t key)
{
	if (qsetFind(s, key) >= 0) return true;
	if (s->qcount >= WATERQ_CAP) {
		// Evict the oldest to make room for the newest — see the header comment above for the
		// measurement this is made on. qPop cannot fail here (qcount is WATERQ_CAP, which is
		// positive), but it is checked rather than assumed: a false return would mean the ring
		// is simultaneously full and empty, and admitting an entry on top of that would write
		// past qcount's own accounting.
		uint64_t victim;
		if (!qPop(s, &victim)) return false;
		s->q_full++;
		if (s->gen_remaining > 0) s->gen_remaining--;
	}

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

// ── Why the ring USED to fill up, and where that was closed ──────────────────────────
//
// v1.8.3. FIXED, outside this file. Kept as the historical record of what the leak cost, because
// the measurements are the reason the eviction policy above exists and are not reproducible now
// that the cause is gone.
//
// What it was: world.c's edit hook is a FILE STATIC (world.c:217) and worldSet fires it for ANY
// World* (world.c:278). main.c's onWorldEdit called straight through to waterNotify, because the
// hook's signature carried no world argument for it to check. So app/worker.c's generation thread
// fed this queue: worker.c:266 runs worldgenColumn against its own STAGING world (worker.c:52) and
// worldgen's decoration pass places trees and tall grass one block at a time through worldSet
// (worldgen.c:339 treePut, worldgen.c:458 tall grass), so every decorated cell of every streamed
// column arrived here — naming a coordinate in a world this simulation does not simulate, from a
// thread that does not hold any lock over it.
//
// MEASURED on 2026-08-25, density generator, seed 12345, 144 columns: 8590 firings, worst single
// column 221, and that one column alone offered up to 1547 candidates against a ring of 1024. The
// ring was pinned at its cap for as long as terrain was streaming; the drain is 64 cells every
// WATER_SPREAD_TICKS ticks and could not begin to keep up. The legacy generator is the same shape
// (8287 firings, worst column 193).
//
// Two separate faults, both outside this file, both now closed:
//   * every one of those candidates was WRONG — the cell named did not change in the player's
//     world, so the recompute it asked for was a guaranteed no-op that displaced real work; and
//   * they arrived on the worker thread with no synchronisation against the main thread's
//     waterTick, which is a data race on the ring, the dedup set and the counters. Not an SMP
//     race — worker.c:22's BS_WORKER_CORE is 0, so both threads are on core 0 and never run at
//     the same instant — but a priority-preemption race, which is enough: the main thread
//     interrupts the worker part-way through qPush and runs a whole tick before it resumes.
//     Measured on the host against this exact file with one preemption point injected into
//     qPush: 64 preemptions left 12 keys in the dedup set with no ring entry, 512 left 177, and
//     the count only ever rises. Each orphan is a coordinate qPush refuses for ever (the
//     qsetFind short-circuit at the top of qPush) with nothing queued to examine it.
//
// Where the fix landed: world.h's WorldEditFn now carries a `const World*`, world.c:278 passes the
// world through, and main.c's onWorldEdit returns immediately for any world that is not s_world.
// world/water_test.c's testEditHookReportsWorld is the arm that goes red if that stops being true.
//
// NOT closed by that fix, and the reason the eviction above stays: handbuiltFill() (main.c:3419,
// hook live since main.c:3199) is 51364 firings in a single call on the MAIN thread, so a
// BS_WORLD_GEN=0 build still overflows this ring on its own merits.
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

// v1.8.2. The generation barrier, and the whole of it is the two blocks around the loop.
//
// The ring is FIFO and every push goes to the tail (qPush, above), so the entries waiting at the
// instant a tick opens are exactly one propagation hop and everything they push is the next one.
// Nothing used to separate the two: the loop just ran `budget` times, so a hop pushed by the cell
// popped three slots ago was popped again in the same tick and a pour cascaded as deep as 64
// examinations reached. Snapshotting qcount into gen_remaining is what draws the line, and
// gen_cooldown is what keeps the next line WATER_SPREAD_TICKS away from this one.
//
// What it deliberately does NOT do is change the order cells come off the ring. A held tick pops
// nothing at all, so the pop sequence is the same sequence it was before with idle ticks spliced
// into it — which is why the settled world, the flow-cell count and the four order-independence
// hashes in world/water_test.c are all unmoved, and why only the timing checks needed re-pinning.
int waterTick(WaterSim* s, World* w, int budget, WaterChangeFn on_change, void* ud)
{
	if (s->gen_remaining <= 0) {
		if (s->gen_cooldown > 0) {
			s->gen_cooldown--;
			// Only a stall when there was something to hold back. A cooldown running out over
			// an empty ring is the simulation going idle, and counting that would bury the
			// signal under the four idle ticks that follow every settled pour.
			if (s->qcount > 0) s->gen_stalls++;
			return 0;
		}
		s->gen_remaining = s->qcount;   // open the next generation: the ring as it stands
		if (s->gen_remaining <= 0) return 0;
	}

	int changed = 0;
	int n = 0;
	for (; n < budget && s->gen_remaining > 0; n++) {
		uint64_t key;
		// Defensive, and it has to set the counter rather than just break: qcount can only
		// grow while a generation is open, so an empty ring here is impossible — but if it ever
		// happened, a gen_remaining left above zero would make every later tick skip the
		// cooldown block and the barrier would be gone for the rest of the session.
		if (!qPop(s, &key)) { s->gen_remaining = 0; break; }
		s->gen_remaining--;

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

	// The generation is spent, so the next one is due WATER_SPREAD_TICKS after THIS tick — and
	// this tick is the first of the five, which is why it is minus one. Measured, not reasoned:
	// without the minus one the period is six ticks, and the pour reaches distance 7 on tick 37
	// instead of 31. water_test.c's testSpreadRate pins both numbers as bare literals so the
	// off-by-one cannot come back quietly.
	if (s->gen_remaining <= 0) {
		s->gen_cooldown = WATER_SPREAD_TICKS - 1;
		if (n < budget && s->qcount > 0) s->gen_stalls++;
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
uint32_t waterGenStalls(const WaterSim* s) { return s->gen_stalls; }

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
