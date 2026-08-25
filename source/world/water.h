// Water physics: sources, flow levels, spread and drainage (v1.8.0 task 22).
//
// Nothing here includes <3ds.h>. Same rule as the rest of source/world, and for the same
// reason: every claim below — that a pour terminates, that two tick orders reach the same
// world, that a settled lake costs nothing per tick — is a claim a host test can settle in
// milliseconds and an emulator run cannot settle at all.
//
// ── Where the flow level is kept, and why it is NOT a block id ────────────────────────
//
// The obvious design is one block id per level: BLOCK_WATER stays the source and seven new
// core registry rows carry levels 7..1. It cannot be done in this repo, and the reason is
// worth writing down so nobody spends the afternoon re-deriving it.
//
// deps/blocksmith-server/game/world/registry.c is a BYTE-IDENTICAL vendored copy of
// world/registry.c (md5 e33ec067652cfd4402bd790497078915 on both, checked 2026-08-24), and
// deps/blocksmith-server is a git clone the Makefile pins at commit 05c14fd6 and re-checks
// out on `make deps`. The server packs registryCount() and registryCrc16() from ITS copy
// into BS_APP_REGISTRY_INFO (bsgame.c:519-520), and net/networld.c's registryMatchesInfo()
// (networld.c:160-166) compares both against this client's own table. Adding seven core rows
// moves the count from 10 to 17 and changes the crc, so every joined session would answer
// networldRegistrySynced() == false for ever, spend its whole FETCH retry budget, and fly the
// "!" the debug overlay puts up for an unverified table (main.c:3948). Fixing that means
// editing a tree this client does not own — the exact constraint world/block.h already names
// for BLOCK_COUNT, and the one the roadmap's task 19 entry already refused to cross.
//
// So the level lives HERE, in a sparse side map, and the world's block array keeps saying
// exactly what it says today: BLOCK_WATER or not. That has three consequences, all of them
// wanted:
//
//   * The registry, the chunk storage forms, the region file, chunk_codec's palette/RLE
//     encoding and the wire format are all untouched. A v1.7.0 or v1.7.1 world opens as the
//     world it was, with an empty flow map, which reads as "all of its water is source
//     water" — which is exactly true, because worldgen is the only thing that has ever
//     placed water and water cannot be placed by a player (world/block.h: it is past
//     BLOCK_COUNT, so inventoryCanHold refuses it).
//   * The mesher is untouched. Every level is BLOCK_WATER, so the same-material self-cull at
//     mesher.c's deferred pass still matches on a bare id compare and a lake still meshes to
//     its shell. A design with eight water ids would have had to relax that test.
//   * Nothing needs to go on the wire. A joined session generates GEN_VERSION_LEGACY terrain
//     (world/genversion.h's genVersionForSession), the legacy generator places no water at
//     all (world/worldgen.h's own note: "an existing world has no below-water terrain to
//     flood"), and water is not placeable — so in multiplayer, today, there is no water for
//     anything to disagree about.
//
// ── Source, flow, and the one rule that keeps them apart ──────────────────────────────
//
// A cell holding BLOCK_WATER is a SOURCE when the map has no entry for it, and a FLOW cell of
// level 1..7 when it does. Absence-means-source is not a shortcut, it is what makes an ocean
// free: a hundred thousand generated water cells cost zero bytes and zero work, and only the
// few hundred cells that a player's digging actually set moving are ever stored.
//
// The price is that a flow cell whose map entry is lost would read back as a source, and a
// source spreads. That is why waterDropColumn() exists and why main.c calls it before a
// column is handed to the save worker: flow water is REMOVED from the world when its column
// unloads, so it can never reach the SD card and come back promoted to a source. See
// waterDropColumn's own comment for what that costs and what it does not.
//
// ── The rule ─────────────────────────────────────────────────────────────────────────
//
// Every cell's level is recomputed from its neighbours; nothing is derived incrementally:
//
//     solid cell (anything but air or water)      -> no water
//     water cell with no map entry (a source)     -> level 8, pinned, never recomputed
//     water directly above                        -> level 7  (a waterfall does not weaken)
//     otherwise max over the four side neighbours
//       that are FEEDING                          -> that neighbour's level - 1
//     nothing left                                -> level 0, i.e. the cell drains to air
//
// A neighbour is FEEDING unless it can still go DOWN, in which case all of its water goes down
// and none of it goes sideways. That single rule was arrived at by running the simulation, not
// by reading it: without it a source dropped in mid-air produces a solid block of water fifteen
// wide and as tall as the drop instead of a waterfall.
//
// "Can still go down" means the cell below is air, or the cell below is a FLOW cell — water that
// is itself on its way somewhere. It deliberately does NOT include "the cell below is a source",
// and that distinction is the entire difference between a waterfall and an ocean:
//
//   * A waterfall's source has water under it from the moment the first cell of its column
//     fills. Test only for air, and on the next tick the source starts spreading at its own
//     height, every cell of that spread falls, and the fall becomes a wall.
//   * An ocean's interior cells also have water under them — sources, settled, going nowhere.
//     A wall dug open at mid-depth has to let them out, so they must spread.
//
// The cell where a fall LANDS is outside the rule (rock below), so it spreads, which is exactly
// what turns a fall into a pool.
//
// TERMINATION. Horizontal steps strictly decrease the level and the vertical rule is a
// constant, so within a fixed arrangement of solid blocks there is no zero-sum cycle. An
// unsupported body decays because every cell recomputes to at most (max level in the body) - 1,
// so that maximum falls by at least one per sweep and reaches zero in at most 8. What that
// argument does NOT cover is the feeding rule above, which reads the blocks under a cell while
// the simulation is changing them — so world/water_test.c settles every scenario against a tick
// limit and fails rather than hangs if it ever did not converge, and main.c spends a fixed
// budget per tick so even a hypothetical non-converging patch would cost a constant per frame,
// not a lock-up.
//
// ORDER INDEPENDENCE is tested, not assumed, for the same reason: world/water_test.c hashes the
// finished world after settling the same pour at three different per-tick budgets and after
// disturbing two sources in both orders, and requires all of them to be byte-identical.
//
// ── Cost ─────────────────────────────────────────────────────────────────────────────
//
// Work is queue-driven: a cell is examined only when it, or one of its neighbours, has just
// changed. A settled lake is not in the queue, so it costs literally nothing per tick — which
// is the property that matters most on a 268 MHz ARM11, where a fluid that rescanned its
// surface every tick would cost more than the rest of the game put together.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/block.h"
#include "world/scratch.h"
#include "world/world.h"

// A source is level 8 and is never stored; a flow cell is 1..7 and always is. 7 rather than
// some other number because it is the largest value that fits the "one less per step" rule
// while still reaching zero inside a cell's own byte, and because it puts the reach of a
// disturbance at seven blocks — the distance Beta 1.7.3 uses and the one the roadmap's
// wording ("flow levels") is describing.
#define WATER_LEVEL_SOURCE 8
#define WATER_LEVEL_MAX    7

// Slots for flow cells. Power of two, open addressing with linear probing, same shape as
// world.h's column table. 2048 slots hold roughly 1400 flow cells before probes get long;
// a seven-block spread over flat ground is about 113 cells per layer, and a waterfall with a
// pool under it a few hundred, so this is generous rather than tight. An insert that is
// refused is counted (waterMapFull) and never hidden.
#define WATER_MAP_SLOTS 2048

// The candidate queue: a ring of cells waiting to be recomputed, with a dedup set so a cell
// that six of its neighbours all touch is queued once. Sized under the map for the same
// reason dirtyq/relightq are sized the way they are — the population that can be pending at
// once is the moving fringe of a flow, not the whole flow.
#define WATERQ_CAP       1024
#define WATERQ_SET_SLOTS 2048

// Cells examined per tick. At 20 TPS this is 1280 examinations a second, and an examination
// is six worldGet() calls and a hash probe. Deliberately a budget and not "drain it": the
// catch-up clamp (world/tick.h) can hand four ticks to one frame, and four unbounded drains
// inside one 16.71 ms frame is the death spiral that clamp exists to prevent, re-invented one
// layer up.
#define WATER_TICK_BUDGET 64

// ── How FAST it spreads, which is not the same question as how much it costs ──────────
//
// v1.8.2. The budget above is a CPU cap and it used to be the rate as well, because nothing
// separated one propagation hop from the next: a tick drained 64 entries off a FIFO ring, and
// the entries a popped cell pushed were popped again a few slots later inside that same tick.
// A seven-block pour is about 225 examinations, so it finished in four ticks — 200 ms, with the
// first three blocks of it inside the first 50 ms. That does not read as water flowing, it
// reads as a puddle appearing.
//
// So waterTick now drains at most ONE GENERATION — the entries that were already waiting when
// the tick opened, which is exactly one hop — and holds the next generation for this many ticks.
// Five is Beta 1.7.3's Overworld rate, one block per 0.25 s, the same reference the seven-block
// reach above is taken from.
//
// Measured rather than derived, by world/water_test.c's testSpreadRate: a source dropped on flat
// ground wets distance 1 on tick 1 and distance 7 on tick 31, and the whole pour settles on tick
// 36 — against ticks 1 and 7, settled on 9, before the barrier. At the WATER_TICK_BUDGET of 64
// it is three ticks slower still (distance 7 on tick 34, settled on 41), because the frontier
// from distance 5 outwards is wider than 64 candidates and those generations each need a second
// tick to drain. So a seven-block spread goes from 200 ms to 1.70 s on the console.
//
// The two numbers cannot move each other now. Raising WATER_TICK_BUDGET buys a wide flood more
// throughput and does not make it spread faster; raising this makes it spread slower and does
// not make a saturated tick cheaper. And the barrier can only leave budget UNSPENT, so the
// worst-case tick is the same 64 examinations it always was.
//
// Nothing about the destination changes. The barrier inserts idle ticks into the pop sequence
// and never reorders it, so every cell still comes off the ring in the order it always did —
// which is why world/water_test.c's four-budget order-independence hashes are unmoved.
#define WATER_SPREAD_TICKS 5

// Called once for every cell whose BLOCK actually changed — not for a cell whose flow level
// changed inside an unchanged block of water. main.c passes the same relight-then-remesh pair
// its remote-edit hook uses.
//
// ⚠ The clause that used to follow — "because those two render identically and a remesh for
// one would be pure cost" — was true only while every level drew as a full cube. Since v1.8.0
// task 22b a level IS a height, so a level-only change is a visible change. It still must not
// come out of THIS hook: on_change means "a block moved", and a block that moved needs the
// column relit, which a level change does not. The separate, optional level hook below is what
// carries it, and it is separate precisely so that the relight stays off the cheap path.
typedef void (*WaterChangeFn)(void* ud, int x, int y, int z, BlockId id);

typedef struct {
	// Flow cells. key 0 is "empty"; waterKey() sets a tag bit so a real cell can never
	// pack to zero.
	uint64_t map_key[WATER_MAP_SLOTS];
	uint8_t  map_lvl[WATER_MAP_SLOTS];
	int      map_count;

	uint64_t ring[WATERQ_CAP];
	uint64_t qset[WATERQ_SET_SLOTS];
	int      qhead, qcount;

	// The generation barrier (WATER_SPREAD_TICKS above). `gen_remaining` is how much of the
	// OPEN generation is still unpopped — a generation can outlive its tick when it is bigger
	// than the budget, which is the case the two counters have to be separate for.
	// `gen_cooldown` is how many ticks are left before the next one may open. Both are zeroed
	// by waterInit's memset, so a build that never ticks behaves exactly as it did before.
	int      gen_remaining;
	int      gen_cooldown;

	// Set only while waterDropColumn is running, and the reason is that main.c installs
	// waterNotify on world.c's edit hook: without it, clearing a flooded column would push
	// seven candidates per cleared cell for a column that is about to stop existing, and
	// those candidates would evict real ones from a ring of 1024. Nothing in a dropped
	// column needs re-examining — it is being removed, not changed.
	bool     dropping;

	// Diagnostics, all lifetime totals. Exposed because a bounded queue that silently ate
	// candidates is exactly the failure world/meshq.h exists to have stopped repeating.
	uint32_t map_full;   // flow cells refused a map slot
	uint32_t q_full;     // candidates refused a ring slot
	uint32_t examined;   // cells popped and recomputed
	uint32_t changed;    // of those, cells whose level or block actually moved

	// Ticks that ended with budget left over AND work still queued, i.e. ticks the barrier
	// deliberately held back. It is here for the same reason q_full is: a rate limiter that
	// throttles silently is indistinguishable from a simulation that has stopped, and the one
	// question worth asking when water looks stuck is whether it is waiting or wedged.
	// It is EXPECTED to be large during a spread — four of every five ticks of a pour are a
	// stall by this definition — so what it diagnoses is a value that stops rising while
	// waterPending() stays above zero.
	uint32_t gen_stalls;

	// v1.8.0 task 22b. Fired for a cell whose LEVEL moved inside water that was already there
	// — the case the on_change hook above deliberately stays quiet for. NULL until
	// waterSetLevelHook installs one, which waterInit's memset guarantees, so every existing
	// caller (world/water_test.c, and any build with no renderer) behaves exactly as before.
	WaterChangeFn level_fn;
	void*         level_ud;
} WaterSim;

void waterInit(WaterSim* s);

// v1.8.0 task 22b. Installs the level-change hook described on WaterSim.level_fn. Pass NULL to
// remove it. main.c installs a remesh-only hook here: a flow level now decides how tall the
// cell is drawn (world/mesher.c), so a level that moves without its block moving still has to
// reach the renderer — and must NOT reach the relighter, because no block moved and the light
// in that column has not changed.
void waterSetLevelHook(WaterSim* s, WaterChangeFn fn, void* ud);

// Something changed at (x, y, z): re-examine that cell and its six neighbours. This is the
// only way work ever enters the simulation. main.c installs it on world.c's edit hook, so a
// player break, a placed block and a remote edit all reach it without three call sites.
void waterNotify(WaterSim* s, int x, int y, int z);

// One 20 TPS tick. Examines at most `budget` queued cells — and at most the one generation
// that was already waiting, see WATER_SPREAD_TICKS — and returns how many actually changed.
// `on_change` may be NULL. A tick held back by the barrier returns 0 without examining
// anything and is counted in gen_stalls.
int waterTick(WaterSim* s, World* w, int budget, WaterChangeFn on_change, void* ud);

// Ticks until the queue is empty or `max_ticks` have run. Returns the number of ticks used,
// or -1 if it was still busy at the limit — a caller that wants "did this pour finish"
// gets an answer rather than a hang. The tests are the customer; main.c uses waterTick.
int waterSettle(WaterSim* s, World* w, int max_ticks, WaterChangeFn on_change, void* ud);

// Level at a cell: 0 for no water, 1..7 for a flow cell, WATER_LEVEL_SOURCE for a source.
uint8_t waterLevelAt(const WaterSim* s, const World* w, int x, int y, int z);

// v1.8.0 task 22b. Copies the flow levels covering chunk (cx, cy, cz) and its one-block skirt
// into `ms`'s water band, and sets ms->water_any when it wrote anything. Call it straight after
// scratchFill, which clears the flag.
//
// It is HERE and not in world/scratch.c for a link reason, not a taste one: scratch.c is in
// every host suite and water.c is in one, so a call the other way round would drag the whole
// simulation into six binaries that have no use for it. This direction costs nothing — water.c
// already knows how to unpack a cell key, and scratch.h is a header.
//
// Cost, stated rather than assumed: nothing at all when no cell in the world is flowing
// (map_count == 0, the state a freshly loaded world is in — see the absence-means-source rule
// at the top of this file), and one pass over the 2048 map slots when some cell is. The 5,832
// byte band is cleared only once a slot is found to land inside this scratch, so a chunk
// nowhere near the flow pays the scan and not the memset.
void waterFillScratch(const WaterSim* s, MeshScratch* ms, int cx, int cy, int cz);

int waterPending(const WaterSim* s);    // cells waiting to be examined
int waterFlowCells(const WaterSim* s);  // flow cells currently tracked

uint32_t waterMapFull(const WaterSim* s);
uint32_t waterQueueFull(const WaterSim* s);
uint32_t waterExamined(const WaterSim* s);
uint32_t waterChanged(const WaterSim* s);
uint32_t waterGenStalls(const WaterSim* s);

// Removes every flow cell in column (cx, cz) from the world and from the map, returning how
// many cells it cleared. Sources are untouched.
//
// main.c calls this immediately before a dirty column is handed to the save worker, and the
// column is unloaded straight afterwards. That is what keeps the save format honest: the only
// water that ever reaches the card is the water the generator put there, so a world reloaded
// by this build — or by v1.7.1, or by any build that has never heard of this file — reads
// exactly the same bytes it always did, and no flow cell can come back as a source.
//
// What it costs, stated rather than buried: flowing water does not survive its column leaving
// the render ring. Walk far enough away from a channel you flooded and it is dry when you come
// back, until you disturb it again.
//
// The obvious other half — a waterSeedColumn() that queues every air cell touching water when a
// column loads, so the flow restarts by itself — was written, and then deleted, because it is a
// much bigger change than it looks. Every generated water cell is a source (absence means
// source), and worldgen leaves sealed caves full of air directly against the ocean wall. Seeding
// on load would therefore start the whole coastline of every streaming column draining into
// every cave it touches, with no player having done anything, and 2048 map slots would fill in
// seconds. The same flood is still reachable — break the wall between a cave and the sea and the
// sea comes in, which is the feature — but it happens where the player made it happen, once, and
// not everywhere at once on a streaming path v1.7.1 task 49 is already investigating for frame
// drops. Whether ocean-fed cave flooding should run unprompted is a design decision, not an
// implementation detail, and it is not this task's to make.
int waterDropColumn(WaterSim* s, World* w, int cx, int cz);
