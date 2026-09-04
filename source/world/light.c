#include "world/light.h"

#include <stdlib.h>
#include <string.h>

#include "world/budget.h"
#include "world/registry.h"

// The adaptive lighting engine's implementation. This file is NOT compiled
// standalone: world.c includes it, because tools/run_host_tests.sh names its
// files explicitly and predates this module, so every host binary that links
// world.o needs the engine's symbols to arrive inside it. The console Makefile
// filters this file out of its own wildcard scan for the same reason — see the
// comment there. Edit either half together.
//
// Both channels of one column, 32 KiB, claimed from the world budget on attach.
// The pointer lives in Column.light and is owned end-to-end by this file: world.c
// only ever calls lightColumnDetach on the way out.
typedef struct {
	uint8_t sky[LIGHT_COL_BYTES];
	uint8_t blk[LIGHT_COL_BYTES];
} LightColumn;

static bool    s_enabled;

// ── Why the counters below are atomic ────────────────────────────────────────
//
// Two threads reach this file, and world/budget.c:5-25 already carries the argument in full
// for exactly this interleaving. The worker generates — app/worker.c:286, JOB_GENERATE ->
// lightPropagateColumn -> lightColumnAttach — while it is NOT parked: the handshake
// app/worker.h describes covers workerInstall only. The main thread attaches from the edit
// path (scene/interact.c:115 and :309, main.c:2958, world/relight_drain.c:26, all
// lightRelightColumn) and detaches from the streaming unload (world/world.c:148,
// worldColumnRemove) at the same time. On a New 3DS the worker sits on core 2 (app/hw.c:8),
// so the two are simultaneous rather than timesliced.
//
// A plain `s_attached++` is a load, an add and a store; two of those interleaved lose one
// column's worth of accounting, permanently.
//
// RELAXED, not the sequentially-consistent CAS budget.c uses for s_used, and the difference
// is deliberate rather than a shortcut. s_used is a GATE — an allocation is refused against
// it — so it needs the ordering. s_attached and s_sweep_fallbacks are STATISTICS, read by
// lightColumnsAttached(), lightBytesUsed() and lightSweepFallbacks() for the host suite and
// the debug overlay and by nothing that decides anything. That is the case budget.c's own
// comment covers — "`s_peak` and `s_refusals` are statistics rather than gates, and are
// relaxed on purpose" — and its s_refusals is incremented with the same
// __atomic_fetch_add(..., __ATOMIC_RELAXED) used here.
//
// MEASURED before this changed, tests/light_race_test.c: two threads, 120,000 balanced
// attach/detach pairs each, lightColumnsAttached() read -3673 instead of 0 — while
// budgetUsed(), driven by those same two threads through those same calls, read 0. The lost
// updates were this file's, not budget.c's.
static int     s_attached;
// The effective luminance both engines read, one entry per id in the WHOLE registry
// id space, core and dynamic alike — not BLOCK_COUNT entries.
//
// This is a CACHE of BlockDef.luminance, rebuilt by syncLuminance() below. Nothing
// else writes it: the registry is the single source of truth for what a block emits.
//
// v1.8.7, and deliberately NOT synchronised — both threads do write it, and it is still safe.
// syncLuminance() is a pure function of s_lum_test[i] and registryGet(i)->luminance, so the
// worker and the main thread do not write DIFFERENT values into an entry, they write the same
// value into it. Each entry is one byte and a byte store is indivisible, so a reader sees the
// old value or the new one and those are equal. Adding a lock or a per-thread copy here would
// buy nothing and would touch the one thing in this file that must not move: what the engines
// read when they seed. Left alone on purpose.
//
// What that argument does NOT cover, and what is a real gap: the registry itself. If
// registryRemoteApply() lands a new luminance on the main thread while the worker is inside
// its seeding loop, the worker can seed some ids from the old table and some from the new. It
// cannot corrupt anything — every entry is a whole byte either way — and the next relight of
// that column is correct. That race lives in world/registry.c, not here.
static uint8_t s_luminance[REGISTRY_MAX];

// Test overrides, laid over the registry by syncLuminance(). Zero means "no override"
// — see lightSetLuminanceForTest, which is the only writer.
static uint8_t s_lum_test[REGISTRY_MAX];

// v1.8.17 perf. opaqueAt's predicate — solid && !transparent — precomputed per id, one
// byte each, filled by syncLuminance() below beside s_luminance.
//
// WHY. opaqueAt is the single hottest thing in this file: it is called for every
// neighbour offer in spread(), for every cell of every sweep pass in
// lightRelightColumnSweeps, for all 2048 cells of each of the four sides in
// seedCrossColumnSide, and once per sky strip in lightPropagateColumn's seed loop. It
// used to end in `blockInfo(id)`, which lives in world/block.c and delegates to
// registryView() in world/registry.c — a CROSS-TU call the console build cannot inline,
// because Makefile:40-70 compiles at -O3 with no LTO. MEASURED on the host harness
// described in this change's report (ld --wrap=blockInfo, so only calls that actually
// cross a translation unit are counted): one lightRelightColumn on a column with real
// relief made 5,164 blockInfo calls, 6,550 on a flat column carrying a torch, and 19,049
// on a torch near a border with four loaded, lit neighbours. The control arm with the
// engine off read 0, so those numbers are this engine's and nothing else's. After this
// table all four arms read 256 — one pass over the registry, which is the syncLuminance()
// walk itself and nothing else. Every light value the same harness dumped (both channels
// of five columns across five handoff scenarios, FNV digests) was unchanged.
//
// WHY IT IS FILLED IN syncLuminance() AND NOT AN INIT FUNCTION. The registry is not
// constant: registryRegister and registryRemoteApply can add or change a row at any
// point in a session (a server registering blocks at join, registrySidecarLoad on a
// world load), and a table built once at boot would go stale and light the world through
// a block's OLD solidity. syncLuminance() already walks every registry entry exactly
// once per propagate/relight, for s_luminance, for precisely this reason — so this rides
// that walk and inherits its freshness guarantee whole. The cost of the extra column in
// that walk is one blockInfo() per id, 256 per relight, against the thousands it deletes.
//
// THREADING. Identical to s_luminance's argument immediately above, and it holds for the
// same reason: this is a pure function of blockInfo(i), so the worker and the main thread
// write the SAME byte into the same entry rather than different ones, and a byte store is
// indivisible — a reader sees the old value or the new one and they are equal.
//
// ORDERING, and the one real hazard. .bss starts this all-zero, which reads as "nothing
// is opaque" — light would pour through stone. Nothing may call opaqueAt before
// syncLuminance() has run at least once in this process. That is why the syncLuminance()
// call in lightPropagateColumn moved ABOVE the sky pass in this change: the sky seed loop
// and spread() both call opaqueAt, and it used to be called after them, purely because
// the block pass was the only thing that needed its return value.
// lightRelightColumnSweeps already called it before its first opaqueAt.
static uint8_t s_opaque[REGISTRY_MAX];

// The six in-column neighbour offsets, shared by both engines.
static const int8_t kDirs[6][3] = {
	{ 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1},
};

// v1.8.0. The edit path's queue, owned here so callers stay queue-free.
//
// lightRelightColumn used to BE the sweep engine, and light.h justified that with "no queue may
// be allocated on the main thread". True, and it cost 26x: measured on the host over one real
// generated column, 200 iterations each, the sweeps take 4.064 ms against the flood fill's
// 0.156 ms. Scaled to a 268 MHz ARM11 with no L2 cache that is the difference between a frame
// and a visible stall on every block broken — on BOTH consoles, since the New 3DS edit path ran
// the sweeps too. Nothing since v1.2.5 has run on hardware, so nobody had felt it.
//
// The premise was right and the conclusion was not: the queue must not be allocated *per edit*,
// which is not the same as never allocating one. It is 65,544 B, taken once when the engine is
// enabled and released when it is disabled — no main-thread allocation at edit time, which is
// the property that mattered. If the allocation is refused the sweeps still run, so the worst
// case is today's speed rather than no light.
static LightQueue* s_edit_queue;

// v1.8.3. Whether that allocation was refused, and how many relights the refusal has cost.
//
// The malloc above used to be fire-and-forget: `if (!s_edit_queue) s_edit_queue = malloc(...)`
// with the result never read. A refusal left s_enabled true and s_edit_queue NULL, so
// lightRelightColumn fell through to lightRelightColumnSweeps for the rest of the session — 26x
// slower, per the measurement above — with no counter, no flag and nowhere for it to show up.
//
// The FALLBACK is not the defect and is not being removed: sweeps at today's speed beat no light
// at all. The defect is that it was silent. The sentence defending it ("the worst case is today's
// speed") was written when a relight ran once per edit; v1.8.0 added main.c's per-frame drain,
// which runs many per frame, and the justification was never revisited against that.
//
// A counter and a predicate rather than a log line, because that is what this layer already does.
// source/world compiles with no <3ds.h> and contains no logging facility of any kind — that is
// deliberate, it is what makes the whole directory host-testable — and every other degraded state
// here is reported the same way: a static counter behind an accessor, read by main.c and the host
// suite. relightqOverflows() and relightqCoalesced() (world/relightq.h), dirtyqPeak()
// (world/dirtyq.h), lightColumnsAttached() and lightBytesUsed() just below are all this shape.
//
// Refusing to claim the engine at all — leaving s_enabled false on a refused malloc — was the
// other candidate and it is worse: it turns a slow relight into no light LEVEL anywhere, which is
// a much larger regression than the one being reported.
static bool s_edit_queue_refused;
static int  s_sweep_fallbacks;

// v1.8.7. Whether a thread is inside the edit queue right now, claimed with a CAS.
//
// app/worker.c:108 says of its own queue: "Owned by this thread for its whole life — the main
// thread's edit path uses the queue-free sweep engine precisely so the two never share one."
// That was true when lightRelightColumn WAS lightRelightColumnSweeps. v1.8.0 rebuilt it on top
// of the flood fill over s_edit_queue and did not revisit the sentence, and worker.c:287 calls
// lightRelightColumn — so on a BFS refusal the worker takes the main thread's queue while the
// main thread's per-frame relight drain (world/relight_drain.c:26) may be inside it. One
// LightQueue, two threads: queuePush and queuePop both read-modify-write head and count, and
// lightQueueInit resets them from under whoever is mid-sweep.
//
// This is not a lost statistic. It is wrong LIGHT, and a popped index from the other thread's
// column can also address past the 16 KiB channel. MEASURED before this existed,
// tests/light_race_test.c: two threads relighting two different columns, 116 of 120 relights
// produced a channel digest that disagreed with the same call's single-threaded answer.
//
// A CAS claim rather than a lock, for the reason world/ has no locks at all: this directory
// compiles with no <3ds.h>, which is what makes it host-testable, so LightLock is not
// reachable from here. The refused case does not wait either — it takes
// lightRelightColumnSweeps, the queue-free engine this file already keeps and that the host
// suite already proves agrees with the flood fill byte for byte. So contention costs the
// loser 26x on that ONE relight and changes not a single light value, and it is counted in
// the place every other degraded state in this file is counted.
//
// ACQUIRE on the claim and RELEASE on the drop, not relaxed: unlike the counters above this
// one really is a gate, and it has to order the queue writes that follow it.
static int  s_edit_queue_busy;

// Test hook, in the same spirit as lightSetLuminanceForTest below: makes the next
// lightEngineInit(true) behave exactly as a refused malloc does, so the degraded path can be
// entered on purpose instead of only by running out of memory. Nothing in production sets it.
static bool s_fail_edit_queue_for_test;

void lightEngineInit(bool enable)
{
	s_enabled = enable;

	if (enable) {
		if (!s_edit_queue && !s_fail_edit_queue_for_test)
			s_edit_queue = (LightQueue*)malloc(sizeof(LightQueue));

		// The result, READ. This assignment is the whole of defect A's fix: everything else
		// here exists to carry it somewhere a person or a test can see it.
		s_edit_queue_refused = (s_edit_queue == NULL);
	} else {
		free(s_edit_queue);
		s_edit_queue        = NULL;
		s_edit_queue_refused = false;
		// v1.8.7. Cleared with the queue it guards. Nothing may be inside it at this point —
		// this runs on the main thread with the worker stopped — but a claim left set after
		// the queue it named has gone would send every later relight down the 26x sweep path
		// for the rest of the session, which is precisely the silent degradation v1.8.3 was
		// written to stop.
		__atomic_store_n(&s_edit_queue_busy, 0, __ATOMIC_RELEASE);
	}
}

bool lightFastEngineReady(void) { return s_enabled && !s_edit_queue_refused; }
int  lightSweepFallbacks(void)  { return __atomic_load_n(&s_sweep_fallbacks, __ATOMIC_RELAXED); }

void lightFailEditQueueForTest(bool fail)   { s_fail_edit_queue_for_test = fail; }
void lightResetSweepFallbacksForTest(void)  { __atomic_store_n(&s_sweep_fallbacks, 0, __ATOMIC_RELAXED); }

bool lightEnabled(void)           { return s_enabled; }

// The relaxed loads cost nothing — on ARM11 a relaxed load of an aligned word is the same
// single LDR a plain read compiles to, with no barrier — and they are what makes reading a
// counter the other core is writing defined rather than merely lucky.
int    lightColumnsAttached(void) { return __atomic_load_n(&s_attached, __ATOMIC_RELAXED); }
size_t lightBytesUsed(void)       { return (size_t)lightColumnsAttached() * sizeof(LightColumn); }

// v1.8.18 optlight follow-up. Guards a single Column's OWN light buffer (col->light) against
// two threads relighting the SAME column at once -- a different hazard from s_edit_queue_busy
// above, which guards the shared WORKLIST the flood fill borrows, not the destination either
// engine writes into. lightPropagateColumn and lightRelightColumnSweeps both do
// `memset(lc, 0, sizeof(*lc))` and then many setNibble() read-modify-writes into lc->sky/blk
// with no synchronisation of their own -- col->light was never given one, because no two
// callers can resolve the same Column* today: every worker lane relights only its own private
// ln->staging (app/worker.c:481-482) and the main thread relights only s_world (main.c,
// scene/interact.c, world/relight_drain.c), and s_attached's comment above is the same argument
// for why those two never collide. That is a property of today's call sites, not a guarantee
// for tomorrow's, so this closes the hole rather than just documenting it.
//
// MEASURED, not assumed: reproduced with tests/light_race_test.c's ORIGINAL design -- same
// World, same cx/cz, two threads racing the SAME column instead of two different ones -- at a
// 2-of-200-digest-mismatch rate, and at the SAME rate against the pre-v1.8.18 light.c (swapped
// in, run, restored), which is what proves this predates this task rather than being introduced
// by it.
//
// Keyed by Column* -- unique per column within a World, stable for a call's whole duration,
// since neither engine detaches its own column mid-call -- against a small FIXED table rather
// than grown per-column, because the real worst case is small and bounded: two worker lanes
// plus the main thread (app/worker.c's WORKER_LANE0/WORKER_LANE1) is three simultaneous
// relights, ever.
//
// EXACT match, not a hash: a hashed single-slot-per-key version of this shipped first and was
// caught by two DIFFERENT tests before it reached anyone else -- tests/light_snapshot_cas_test.c's
// own ARM 2 (two distinct columns, one shared World) and tests/light_race_test.c's pre-existing
// ARM 3 (three distinct columns across three distinct Worlds -- lane 0, lane 1, main) both
// MEASURED nonzero refusals between columns that share nothing, because their Column* addresses
// happened to hash to the same slot -- deterministically, not by chance: lane 1 refused 58 of 60
// calls. A column-identity table must never refuse two columns that are not each other, so the
// slot has to be found by comparing col itself against every entry, not by computing one.
//
// The scan is guarded by s_column_claim_busy, a SEPARATE claim from the one this function
// grants: it protects the four-element table for the handful of loads/stores one claim or
// release touches it with, not the relight itself. Spinning to acquire it is safe in a way
// spinning on a relight would not be -- the critical section it guards is a fixed, tiny loop
// with no call into anything that allocates, blocks, or takes milliseconds, so the longest any
// caller ever waits is one other thread's own same-sized scan. That is a different claim from
// s_edit_queue_busy's (held for a whole relight, hence never retried, per that claim's own
// comment) rather than a departure from it -- the "no lock" constraint below is about LightLock
// (app/*.h, unreachable from a directory with no <3ds.h>), not about a plain atomic word.
#define LIGHT_COLUMN_CLAIM_SLOTS 4
static Column* s_column_claim[LIGHT_COLUMN_CLAIM_SLOTS];
static int     s_column_claim_busy;

static void columnClaimTableLock(void)
{
	int idle = 0;
	while (!__atomic_compare_exchange_n(&s_column_claim_busy, &idle, 1, false,
	                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		idle = 0;
}

static void columnClaimTableUnlock(void)
{
	__atomic_store_n(&s_column_claim_busy, 0, __ATOMIC_RELEASE);
}

// Claims col for the rest of this call. False means another thread already holds col ITSELF --
// never a different column, see the comment above -- and the caller must treat that exactly
// like any other refusal here (budget, queue overflow) and touch nothing in col->light. A full
// table (more concurrent DIFFERENT columns than LIGHT_COLUMN_CLAIM_SLOTS, never observed at
// today's two-lanes-plus-main ceiling) also refuses rather than overrun it. Every successful
// claim must reach lightColumnClaimRelease on every path out, refusal paths included.
static bool lightColumnClaimTry(Column* col)
{
	columnClaimTableLock();

	bool already = false;
	int free_slot = -1;
	for (int i = 0; i < LIGHT_COLUMN_CLAIM_SLOTS; i++) {
		if (s_column_claim[i] == col)              { already = true; break; }
		if (free_slot < 0 && s_column_claim[i] == NULL) free_slot = i;
	}

	const bool claimed = !already && free_slot >= 0;
	if (claimed) s_column_claim[free_slot] = col;

	columnClaimTableUnlock();
	return claimed;
}

static void lightColumnClaimRelease(Column* col)
{
	columnClaimTableLock();

	for (int i = 0; i < LIGHT_COLUMN_CLAIM_SLOTS; i++) {
		if (s_column_claim[i] == col) { s_column_claim[i] = NULL; break; }
	}

	columnClaimTableUnlock();
}

bool lightColumnAttach(Column* col)
{
	if (!col) return false;
	if (col->light) return true;

	if (!budgetClaim(sizeof(LightColumn))) return false;

	LightColumn* lc = (LightColumn*)calloc(1, sizeof(*lc));
	if (!lc) {
		budgetRelease(sizeof(LightColumn));
		return false;
	}

	col->light = lc;
	__atomic_fetch_add(&s_attached, 1, __ATOMIC_RELAXED);
	return true;
}

void lightColumnDetach(Column* col)
{
	if (!col || !col->light) return;

	budgetRelease(sizeof(LightColumn));
	free(col->light);
	col->light = NULL;
	__atomic_fetch_sub(&s_attached, 1, __ATOMIC_RELAXED);
}

bool lightColumnCopy(const Column* src, Column* dst)
{
	if (!src || !dst || !src->light) return false;
	if (!lightColumnAttach(dst)) return false;

	memcpy(dst->light, src->light, sizeof(LightColumn));
	return true;
}

// ── cell access ──────────────────────────────────────────────────────────────

static void setNibble(uint8_t* chan, int ci, uint8_t v)
{
	uint8_t* b = &chan[ci >> 1];
	if (ci & 1) *b = (uint8_t)((*b & 0x0F) | (uint8_t)(v << 4));
	else        *b = (uint8_t)((*b & 0xF0) | v);
}

static const uint8_t* channelSky(const Column* col)
{
	return col && col->light ? ((const LightColumn*)col->light)->sky : NULL;
}

const uint8_t* lightChannelSky(const Column* col)   { return channelSky(col); }

const uint8_t* lightChannelBlock(const Column* col)
{
	return col && col->light ? ((const LightColumn*)col->light)->blk : NULL;
}

// Absent column or out-of-range y reads 0 — "no data" can never read as bright.
// Above the ceiling reads full sky, matching worldGet's air-above-the-world rule
// so a chunk's top border is lit like the open sky it is.
static uint8_t getChan(const Column* col, const uint8_t* chan, int lx, int y, int lz)
{
	if (!chan) return 0;
	if (y >= WORLD_HEIGHT) return (chan == channelSky(col)) ? 15 : 0;
	if (y < 0 || lx < 0 || lx >= CHUNK_DIM || lz < 0 || lz >= CHUNK_DIM) return 0;
	return lightNibble(chan, lightIndex(lx, y, lz));
}

uint8_t lightGetSky(const Column* col, int lx, int y, int lz)
{
	return getChan(col, channelSky(col), lx, y, lz);
}

uint8_t lightGetBlock(const Column* col, int lx, int y, int lz)
{
	return getChan(col, lightChannelBlock(col), lx, y, lz);
}

// ── shared propagation inputs ────────────────────────────────────────────────

// v1.8.18 CHK-FIX. Per-chunk cache of chunkGetForm()/chunkGet(c,0), filled ONCE per
// lightPropagateColumn/lightRelightColumnSweeps call from the chunks[COLUMN_CHUNKS] array
// each already resolves, and read by every opaqueAt() and heightMapFill() call that call
// makes.
//
// WHY. chunkGet (world/chunk.c) is a real cross-translation-unit call -- this build has no
// LTO (Makefile:58) -- and until this change opaqueAt paid one PER CELL it was asked about,
// and heightMapFill paid one per y-level it stepped through, up to 32,768 for one column.
// v1.8.10's luminance loop below already applies this exact idea to its own per-cell walk:
// "a chunk in CHUNK_FORM_UNIFORM form is one id repeated 4096 times... reading it ONCE and
// skipping the whole chunk... turns 4096 chunkGet calls into 1." That early-out was never
// extended to opaqueAt or heightMapFill, which is what this table is for.
//
// chunkGetForm is declared in chunk.h (chunk.h:92) with no inline body -- it is defined in
// chunk.c, so it is exactly as cross-TU as chunkGet itself. Checking it per cell would trade
// one cross-TU call for two and make things worse, not better; this table pays it ONCE per
// chunk (at most COLUMN_CHUNKS = 8 chunkGetForm calls plus 8 chunkGet(c,0) calls) instead.
typedef struct {
	bool    uniform[COLUMN_CHUNKS];
	BlockId id[COLUMN_CHUNKS];   // valid only where uniform[cy] is true
} ChunkUniformCache;

static void chunkUniformCacheFill(ChunkUniformCache* cache, const Chunk* const chunks[COLUMN_CHUNKS])
{
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		const Chunk* c = chunks[cy];
		cache->uniform[cy] = c && chunkGetForm(c) == CHUNK_FORM_UNIFORM;
		if (cache->uniform[cy]) cache->id[cy] = chunkGet(c, 0);
	}
}

// Opaque means "stops light": solid and not transparent, byte-for-byte the same
// predicate the mesher's occlusion table is built from. Air passes; leaves pass.
//
// v1.8.17: the predicate itself is unchanged; it is read out of s_opaque (see the table's
// comment above) instead of recomputed through a cross-TU blockInfo() call per cell.
// BlockId is a uint8_t (world/block.h:15) so the index cannot leave the 256-entry table.
//
// v1.8.18: the id itself now comes from `uni` when the cell's chunk is uniform -- see
// ChunkUniformCache above -- instead of a chunkGet() call every time this is asked about one
// more cell. Byte-identical answer: a uniform chunk holds the same id at every index, so
// uni->id[cy] IS what chunkGet(c, chunkIndex(lx, y&15, lz)) would have returned here.
static bool opaqueAt(const Chunk* const chunks[COLUMN_CHUNKS], const ChunkUniformCache* uni,
                     int lx, int y, int lz)
{
	if (y < 0) return true;   // below the floor reads as solid, like worldGet
	const int cy = y >> 4;
	const Chunk* c = chunks[cy];
	if (!c) return false;     // absent chunk is air
	const BlockId id = uni->uniform[cy] ? uni->id[cy] : chunkGet(c, chunkIndex(lx, y & 15, lz));
	return s_opaque[id] != 0;
}

// Highest non-air cell per (x,z), -1 when the strip is all air. Everything above
// top[x][z] is a straight shot of sunlight at 15; everything at or below it is
// not, whatever it is made of.
typedef struct {
	int8_t top[CHUNK_DIM][CHUNK_DIM];
} HeightMap;

// v1.8.18 CHK-FIX: uni is ChunkUniformCache (see opaqueAt above). A uniform chunk answers the
// same id at every cell, so its whole 16-level span is decided by the ONE cached id instead
// of one chunkGet call per level it steps through -- BLOCK_AIR skips straight past the chunk
// (every real caller enters a chunk at its top row, either from WORLD_HEIGHT-1 or from
// exactly this skip one chunk up, so the jump always lands on the next chunk's own top row);
// non-air breaks on the same y the original per-cell scan would have, since chunkGet would
// have answered the identical id at every cell in a uniform chunk.
static void heightMapFill(HeightMap* hm, const Chunk* const chunks[COLUMN_CHUNKS],
                          const ChunkUniformCache* uni)
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			int y = WORLD_HEIGHT - 1;
			while (y >= 0) {
				const int cy = y >> 4;
				const Chunk* c = chunks[cy];
				if (!c) { y--; continue; }

				if (uni->uniform[cy]) {
					if (uni->id[cy] != BLOCK_AIR) break;
					y = cy * CHUNK_DIM - 1;
					continue;
				}

				if (chunkGet(c, chunkIndex(x, y & 15, z)) != BLOCK_AIR) break;
				y--;
			}
			hm->top[z][x] = (int8_t)y;
		}
	}
}

// v1.6.0: the bound is REGISTRY_MAX, not BLOCK_COUNT. The table has always been the full
// id space wide, but this scan stopped at the 8 compiled-in core rows, so a luminance set
// on a server-registered id (0x80..0xFD) read back as "nothing in this world glows" and
// both engines skipped the block-light pass entirely — the emitter would have been dark.
// Dormant until a dynamic block declares luminance, and free either way. It is called
// exactly ONCE per lightFloodColumn/lightRelightColumn and never inside their cell loops
// (32768 cells per sweep, several sweeps), and neither of those runs per frame — the
// callers are one block edit (main.c, scene/interact.c) and one generated/loaded column on
// the worker thread (app/worker.c). Measured on the host at -O1: the scan costs 79.1 ns at
// 256 entries against 0.3 ns at 8, i.e. 78.7 ns added once, to a lightRelightColumn that
// takes 720.1 us — 0.011% of the call it gates. Scaled by instruction count to a 268 MHz
// ARM11 (248 extra iterations, ~4 instructions each, ~1 IPC) that is roughly 3.7 us per
// edit, against a relight that is milliseconds there; NOT measured on hardware.
//
// v1.8.2: the same walk now also FILLS the table from the registry, which is the whole
// of the fix. BlockDef.luminance has existed since v1.6.0 and crosses the wire in both
// directions (registry.c's registryDefPack/registryDefUnpack, bytes[24]), but no reader
// ever put it into s_luminance — so a server that registered a glowing block at join
// sent its luminance, the client unpacked it into the def, and the lighting engine never
// looked. The block rendered pitch dark with no error and no log line.
//
// Pulling here rather than pushing from registry.c is deliberate, for two reasons.
// First it is ONE choke point instead of three: registryInitCore, registryRegister and
// registryRemoteApply all end up in the same table, and reading that table covers every
// one of them (plus registrySidecarLoad, which goes through RemoteApply) with no way to
// wire two of the three and miss the third. Second, registry.c and registry.h are
// mirrored verbatim into the dedicated server (deps/blocksmith-server/tools/
// sync-world-sources.sh), and light.c is not — a push would have dragged the lighting
// engine into the server build and forced a paired release for a client-only fix.
//
// Cost: the loop was already 256 entries and already ran exactly once per column
// propagate/relight. It now does a registryGet() per entry instead of an array read.
// registryGet on an undefined id answers the air row, whose luminance is 0, so no
// is-defined test is needed.
//
// v1.8.17: the same walk now also fills s_opaque, opaqueAt's predicate table — see that
// table's comment above for the measurement and for why this walk, and not an init
// function, is the only correct place to fill it. It rides here rather than in a second
// loop so the two tables can never be refreshed a different number of times.
static bool syncLuminance(void)
{
	bool any = false;
	for (int i = 0; i < REGISTRY_MAX; i++) {
		const BlockInfo* info = blockInfo((BlockId)i);
		s_opaque[i] = (uint8_t)(info->solid && !info->transparent);

		uint8_t lum = s_lum_test[i];
		if (lum == 0) lum = registryGet((BlockId)i)->luminance;
		s_luminance[i] = lum;
		if (lum) any = true;
	}
	return any;
}

// ── engine 1: breadth-first flood fill ───────────────────────────────────────

void lightQueueInit(LightQueue* q)
{
	q->head = 0;
	q->count = 0;
}

// v1.8.7. `dropped` is the caller's local, not a file static any more. It was
// s_queue_dropped: zeroed at the top of lightPropagateColumn, incremented here, and read at
// the bottom to decide the return value — i.e. state scoped to ONE call, kept in a place two
// threads share. The main thread's `s_queue_dropped = 0` landed in the middle of the worker's
// propagation and erased its refusals, so the worker returned true, took no fallback and
// shipped a half-lit column; the reverse made a main-thread relight report a failure that
// belonged to the worker. An int on the stack has neither problem and costs a register.
static bool queuePush(LightQueue* q, uint16_t ci, int* dropped)
{
	if (q->count >= LIGHT_QUEUE_CAP) { (*dropped)++; return false; }
	q->slots[(q->head + q->count) % LIGHT_QUEUE_CAP] = ci;
	q->count++;
	return true;
}

static uint16_t queuePop(LightQueue* q)
{
	const uint16_t ci = q->slots[q->head];
	q->head = (q->head + 1) % LIGHT_QUEUE_CAP;
	q->count--;
	return ci;
}

// Spreads one channel outward from its seeds with -1 falloff. Seeds are already
// stored in `chan` when this runs, and every entry in the queue names a cell
// whose value was just raised — so a pop re-reads its own level and offers each
// neighbour level-1. A neighbour is written only when that offer strictly beats
// what it holds, which makes its first assignment final (all seeds enter at the
// same level, every edge decrements by exactly one, so later offers can never
// exceed an earlier one) and bounds the queue at one entry per cell.
static void spread(uint8_t* chan, const Chunk* const chunks[COLUMN_CHUNKS],
                   const ChunkUniformCache* uni, LightQueue* q, int* dropped)
{
	while (q->count > 0) {
		const int ci = queuePop(q);
		const int lx = ci & 15;
		const int lz = (ci >> 4) & 15;
		const int y  = ci >> 8;
		const int level = lightNibble(chan, ci);
		if (level <= 1) continue;

		for (int d = 0; d < 6; d++) {
			const int nx = lx + kDirs[d][0];
			const int ny = y  + kDirs[d][1];
			const int nz = lz + kDirs[d][2];

			// Column-local scope: outside the column reads as opaque, so light
			// stops at the border instead of crossing into a column this call
			// does not own.
			if (nx < 0 || nx >= CHUNK_DIM || nz < 0 || nz >= CHUNK_DIM) continue;
			if (ny < 0 || ny >= WORLD_HEIGHT) continue;
			if (opaqueAt(chunks, uni, nx, ny, nz)) continue;

			const int nci = lightIndex(nx, ny, nz);
			if (level - 1 <= lightNibble(chan, nci)) continue;

			setNibble(chan, nci, (uint8_t)(level - 1));
			queuePush(q, (uint16_t)nci, dropped);
		}
	}
}

// ── cross-column block-light handoff (v1.8.10) ──────────────────────────────
//
// The torch is the first block in the game that emits, and propagation above is
// deliberately column-local (light.h's "known gap"): a column-local flood fill
// truncates a torch's light dead at the column edge instead of letting it decay
// one more step into the neighbour, which is wrong -- a torch one cell from an
// edge reads 14 on its own side and 0 one cell across a boundary that should read
// 13. MEASURED (tests/light_seam_test.c, and the scratchpad probe this was first
// caught with): a torch at local x=15 reads col0 lx=15 = 14, col1 lx=0 = 0, a
// 14-level drop across one cell instead of the expected 1. scratchFillLight
// (world/scratch.c) reads the neighbour's REAL stored channel at that border with
// no clamping at all, so the mesher's corner taps show exactly that drop -- this
// is a light-DATA bug, not a mesher bug.
//
// Sky light is NOT extended this way, on purpose: it is near-uniform, so the same
// column-local truncation is invisible there, and touching it would be scope this
// fix was not asked for. See the report this shipped with.
//
// crossBorderCandidate is shared by BOTH engines below (the BFS seeding call and
// the border check inside lightRelightColumnSweeps' cell loop) so the two cannot
// compute two different answers for the same input cell -- the whole reason the
// host suite can diff them byte for byte. It answers "what would this column's
// border cell (lx,y,lz) read if block light crossed the seam", i.e. an already-
// loaded neighbour's own stored value at the mirrored cell, minus one more
// falloff step -- exactly how a real emitter one cell further out would decay
// into this cell. -1 means no loaded neighbour reaches this cell at all (not on
// a border, or that side's column/light is absent), so callers can tell "nothing
// to offer" apart from "offers 0".
static int crossBorderCandidate(const World* w, int cx, int cz, int lx, int y, int lz)
{
	if (lx != 0 && lx != CHUNK_DIM - 1 && lz != 0 && lz != CHUNK_DIM - 1) return -1;

	static const int8_t dcx[4] = { 1, -1, 0, 0 };
	static const int8_t dcz[4] = { 0, 0, 1, -1 };

	int best = -1;
	for (int d = 0; d < 4; d++) {
		const bool along_x = (dcz[d] == 0);
		if (along_x) {
			if (dcx[d] > 0 && lx != CHUNK_DIM - 1) continue;
			if (dcx[d] < 0 && lx != 0)             continue;
		} else {
			if (dcz[d] > 0 && lz != CHUNK_DIM - 1) continue;
			if (dcz[d] < 0 && lz != 0)             continue;
		}

		const Column* ncol = worldColumn(w, cx + dcx[d], cz + dcz[d]);
		const uint8_t* nblk = lightChannelBlock(ncol);
		if (!nblk) continue;   // neighbour not loaded, or has no light: nothing to offer

		const int their_lx = along_x ? (dcx[d] > 0 ? 0 : CHUNK_DIM - 1) : lx;
		const int their_lz = along_x ? lz : (dcz[d] > 0 ? 0 : CHUNK_DIM - 1);
		const int cand = lightNibble(nblk, lightIndex(their_lx, y, their_lz)) - 1;
		if (cand > best) best = cand;
	}
	return best;
}

// One face of the column's border shell (2048 cells: 128 y-levels x 16 across),
// seeding blk from crossBorderCandidate's rule exactly like a local emitter --
// set only when it strictly improves the cell, queued only then, same rule
// spread() itself applies. opaqueAt is checked because a solid cell at the seam
// holds no light of its own to offer inward, matching every other opaque cell in
// this file.
//
// v1.8.11 perf: the neighbour this side reads is the SAME column for every one
// of its 2048 cells -- along_x and edge together pick exactly one of
// crossBorderCandidate's four directions, and that choice never depends on lx/lz
// within one call. The original shape called crossBorderCandidate() per cell,
// which re-derived that one fixed neighbour with a fresh worldColumn() lookup
// every time -- world.c's slotIndexFor is a hashed, linearly-probed slot table,
// not a pointer read -- up to 2048 redundant lookups per side, 8192 per column,
// to answer a question with one answer for the whole call. Resolved once here.
// Now that BLOCK_TORCH is a shipped core block (v1.8.11), syncLuminance() answers
// true in every real game, not just a fixture, so this runs on every propagate
// and every edit relight in every build, not only when a test forces it.
//
// Absent or lightless neighbour (worldColumn NULL, or a loaded column with no
// light channel yet) is now caught ONCE per side instead of being rediscovered
// cell by cell, which additionally skips the entire 2048-cell walk -- opaqueAt
// calls included -- whenever that side has nothing to offer, rather than paying
// the walk and finding out one cell at a time. That is the common case at the
// edge of the loaded ring and during world generation, where a column's
// neighbours are frequently not loaded yet.
//
// MEASURED (scratchpad bench, host, gcc -O1, seed 424242, radius 4 = 81 real
// generated columns, each carrying its own torch one cell from a border -- see
// this change's report for the full harness and both phases): before, two runs,
// torch-removal relight (the retraction path that drives lightHandoffBorders
// hardest) totalled 1836.186 ms / 1658.389 ms for the 81 edits; after,
// 1175.314 ms / 1203.528 ms for the same 81 edits (~30-36% faster). The other
// three phases benchmarked the same way: initial propagate 21.226/16.747 ms
// before vs 11.959/11.611 ms after (~43% faster); torch-placement relight
// (growth) 303.357/307.550 ms before vs 223.339/214.443 ms after (~26-30%
// faster); a light-irrelevant edit 16.731/20.171 ms before vs 12.889/11.125 ms
// after. RAM (budgetUsed/budgetPeak/lightBytesUsed) identical before and
// after in every field -- this is a pure CPU win, no data-structure size
// changed.
//
// Byte-identical to the original at every cell: crossBorderCandidate's per-cell
// answer for THIS side's one live direction is exactly
// lightNibble(nblk, mirrored cell) - 1, which is what this function now computes
// directly instead of through the general four-direction helper. A CORNER cell
// (on two borders at once) loses nothing either: the original's one
// crossBorderCandidate() call there already checked both directions and kept the
// max, but seedCrossColumnBorder visits every corner from BOTH adjoining sides
// regardless (its own comment already says so) -- so the two single-direction
// offers made here converge on the same max the original's one dual-direction
// offer produced, because "only write if it strictly improves" is exactly the
// rule that makes two smaller offers equal one combined one. The one visible
// difference is a corner cell can now be queued twice instead of at most once,
// when both offers strictly improve it in turn; queuePop() re-reads a cell's
// CURRENT stored value rather than a value carried in the queue entry (spread(),
// above), so a repeat visit finds the already-final value and costs one wasted
// neighbour scan, not a wrong answer -- and LIGHT_QUEUE_CAP has always tolerated
// more pushes than the true cell count through the same `dropped` counter every
// other refusal in this file goes through.
static void seedCrossColumnSide(uint8_t* blk, const World* w,
                                const Chunk* const chunks[COLUMN_CHUNKS],
                                const ChunkUniformCache* uni,
                                int cx, int cz, bool along_x, int edge,
                                LightQueue* q, int* dropped)
{
	const int ncx = cx + (along_x ? (edge == 0 ? -1 : 1) : 0);
	const int ncz = cz + (along_x ? 0 : (edge == 0 ? -1 : 1));
	const Column* ncol = worldColumn(w, ncx, ncz);
	const uint8_t* nblk = lightChannelBlock(ncol);
	if (!nblk) return;   // nothing loaded/lit on this side: skip the whole 2048-cell walk

	const int their_edge = (edge == 0) ? CHUNK_DIM - 1 : 0;

	for (int y = 0; y < WORLD_HEIGHT; y++) {
		for (int p = 0; p < CHUNK_DIM; p++) {
			const int lx = along_x ? edge : p;
			const int lz = along_x ? p    : edge;
			if (opaqueAt(chunks, uni, lx, y, lz)) continue;

			const int their_lx = along_x ? their_edge : p;
			const int their_lz = along_x ? p          : their_edge;
			const int cand = lightNibble(nblk, lightIndex(their_lx, y, their_lz)) - 1;
			if (cand <= 0) continue;

			const int ci = lightIndex(lx, y, lz);
			if (cand <= lightNibble(blk, ci)) continue;

			setNibble(blk, ci, (uint8_t)cand);
			queuePush(q, (uint16_t)ci, dropped);
		}
	}
}

// All four sides. Corners are visited twice (once from each adjoining side),
// which is harmless -- the "only write if it strictly improves" rule in
// seedCrossColumnSide makes a repeat visit a no-op.
static void seedCrossColumnBorder(uint8_t* blk, const World* w,
                                  const Chunk* const chunks[COLUMN_CHUNKS],
                                  const ChunkUniformCache* uni,
                                  int cx, int cz, LightQueue* q, int* dropped)
{
	seedCrossColumnSide(blk, w, chunks, uni, cx, cz, true,  0,             q, dropped);
	seedCrossColumnSide(blk, w, chunks, uni, cx, cz, true,  CHUNK_DIM - 1, q, dropped);
	seedCrossColumnSide(blk, w, chunks, uni, cx, cz, false, 0,             q, dropped);
	seedCrossColumnSide(blk, w, chunks, uni, cx, cz, false, CHUNK_DIM - 1, q, dropped);
}

bool lightPropagateColumn(World* w, int cx, int cz, LightQueue* q)
{
	if (!s_enabled || !q) return false;

	Column* col = worldColumn(w, cx, cz);
	if (!col) return false;
	if (!lightColumnAttach(col)) return false;

	// v1.8.18 optlight follow-up: claims col's light buffer for this call's whole duration --
	// see the comment above lightColumnClaimTry for the race this closes. Refusal is the same
	// shape as every other degraded path here: touch nothing, tell the caller no.
	if (!lightColumnClaimTry(col)) return false;

	LightColumn* lc = (LightColumn*)col->light;
	memset(lc, 0, sizeof(*lc));
	int dropped = 0;   // BFS refusals this call; must stay zero (see the CAP note)

	// The column's chunks, resolved once. Gaps are air, matching scratch.c.
	const Chunk* chunks[COLUMN_CHUNKS];
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
		chunks[cy] = worldChunk(w, cx, cy, cz);

	// v1.8.18 CHK-FIX: see ChunkUniformCache's comment above opaqueAt. Filled once here and
	// read by every opaqueAt()/heightMapFill() call this function and the ones it calls make.
	ChunkUniformCache uni;
	chunkUniformCacheFill(&uni, chunks);

	// v1.8.17: HOISTED above the sky pass, and this is load-bearing rather than tidying.
	// opaqueAt now reads s_opaque, which this call fills (see the table's comment), and the
	// sky seed loop and spread() below both call opaqueAt. Called here it is the same pure
	// function of s_lum_test and the registry it was thirty lines lower — neither changes
	// inside this function — so `lum_any` is bit-for-bit the value `if (syncLuminance())`
	// used to test, just computed before the first reader of the table instead of after it.
	const bool lum_any = syncLuminance();

	HeightMap hm;
	heightMapFill(&hm, chunks, &uni);

	// Sky seeds: every air cell above its strip's top holds 15 outright. Only the
	// ones beside a possibly-shadowed column are enqueued — a seed whose four
	// horizontal neighbours are all sunlit too has nothing to teach them, and
	// skipping it keeps the queue proportional to terrain relief rather than to
	// the whole sky.
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			for (int y = hm.top[z][x] + 1; y < WORLD_HEIGHT; y++) {
				setNibble(lc->sky, lightIndex(x, y, z), 15);

				bool border = false;
				if (x > 0          && hm.top[z][x - 1] >= y) border = true;
				if (x < CHUNK_DIM-1 && hm.top[z][x + 1] >= y) border = true;
				if (z > 0          && hm.top[z - 1][x] >= y) border = true;
				if (z < CHUNK_DIM-1 && hm.top[z + 1][x] >= y) border = true;

				// v1.8.0: and DOWNWARD, which the four tests above cannot see.
				// heightMapFill records the highest NON-AIR cell while opaqueAt
				// is solid && !transparent, so a transparent block on top of its
				// strip — leaves, water, tall grass — sits at hm.top (never sky
				// seeded) and still passes light (the sweeps pull 14 into it).
				// Measured before this line existed: an 8x8 lake came out 0 on
				// every one of its 128 cells from here and 13-14 from
				// lightRelightColumn, so a generated lake was black until any
				// edit in that column relit it. Only the lowest seed of a strip
				// can have a non-opaque cell beneath it — everything higher has
				// open sky below and would enqueue the entire sky if tested.
				if (y == hm.top[z][x] + 1 && !opaqueAt(chunks, &uni, x, y - 1, z))
					border = true;

				if (border) queuePush(q, (uint16_t)lightIndex(x, y, z), &dropped);
			}
		}
	}
	spread(lc->sky, chunks, &uni, q, &dropped);

	// Block seeds: luminous cells. syncLuminance() refreshes the table from the
	// registry and answers whether anything in it emits at all; before v1.8.10 no
	// core row declared a luminance, so this whole branch was skipped outright on
	// every single-player world. The torch changes that forever: syncLuminance()
	// now answers true on every call regardless of whether a torch is placed
	// anywhere NEAR this column, so the cell loop below runs on every propagate.
	//
	// v1.8.10: a chunk in CHUNK_FORM_UNIFORM form (chunk.h) is one id repeated
	// 4096 times -- "half of every generated column is sky, and every sky chunk
	// is this" per chunk.h's own header -- and chunkGet on one is already O(1)
	// (chunk.c:207, a struct field read). Reading it ONCE and skipping the whole
	// chunk when that one id does not emit turns the common case (an all-air or
	// all-stone chunk, neither of which is ever luminous) from 4096 chunkGet
	// calls into 1. MEASURED, tests/light_seam_test.c: see its cost-comparison
	// arm for the before/after numbers this comment does not want to go stale.
	//
	// v1.8.17: the syncLuminance() call that used to sit in this `if` is now above the sky
	// pass and its answer is carried here in `lum_any`. Same value, same ONE call per
	// propagate — see the hoist's own comment for why it had to move.
	if (lum_any) {
		for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
			if (!chunks[cy]) continue;
			if (chunkGetForm(chunks[cy]) == CHUNK_FORM_UNIFORM &&
			    !s_luminance[chunkGet(chunks[cy], 0)])
				continue;

			for (int i = 0; i < CHUNK_BLOCKS; i++) {
				const BlockId id = chunkGet(chunks[cy], i);
				const uint8_t lum = s_luminance[id];
				if (!lum) continue;

				// chunkIndex order: x low, then z, then y — same shape the
				// column-wide lightIndex extends over height.
				const int ci = lightIndex(i & 15, (i >> 8) + cy * CHUNK_DIM,
				                          (i >> 4) & 15);
				setNibble(lc->blk, ci, lum);
				queuePush(q, (uint16_t)ci, &dropped);
			}
		}
		// v1.8.10 cross-column handoff -- see the comment above crossBorderCandidate.
		seedCrossColumnBorder(lc->blk, w, chunks, &uni, cx, cz, q, &dropped);
		spread(lc->blk, chunks, &uni, q, &dropped);
	}

	const bool ok = dropped == 0;
	lightColumnClaimRelease(col);
	return ok;
}

// ── engine 2: relaxation sweeps (reference implementation) ───────────────────

// Same fixpoint as the flood fill, reached without a queue: sweep every cell in
// a fixed order, raising each towards the brightest legal offer from its seeds
// and neighbours, until a pass changes nothing. Values only ever rise and are
// bounded by 15, so termination is not a hope; and because the constraint system
// is monotone (max-only updates), the fixpoint does not depend on the order the
// sweeps visit cells in — which is what lets two such different engines agree.
// The edit path's entry point. Same contract as ever — full recompute of one column, no queue
// from the caller — reimplemented in v1.8.0 on top of the flood fill, which is 26x faster (see
// s_edit_queue above for the measurement). lightRelightColumnSweeps is what it used to call and
// is still the independent second implementation the host suite diffs this one against; the two
// are proven to agree byte-for-byte, which is the only reason this swap is behaviour-preserving.
//
// v1.8.10: split into relightColumnCore (this function's old body, unchanged) plus the
// cross-column PUSH below. Everything the old body did — the CAS claim, the fast/sweep
// choice, the fallback counter — is exactly as it was; lightRelightColumn now does that
// and then hands off any border light this column can offer a neighbour.
static bool relightColumnCore(World* w, int cx, int cz)
{
	if (!s_enabled) return false;

	// v1.8.3: counted on the way past. Without this the fallback is indistinguishable from the
	// fast path at every level above it — same signature, same return value, same light, 26x the
	// cost. See s_sweep_fallbacks above for why a counter and not a log line.
	//
	// v1.8.7 adds the second reason to take that same fallback: somebody else is already inside
	// the one shared queue. Claimed with a CAS and never waited on — see s_edit_queue_busy
	// above for why a lock is not available in this directory and why losing the race is
	// harmless. The two reasons share a counter on purpose: what lightSweepFallbacks() has
	// always meant is "a relight paid 26x", and both of these are that.
	// Strong, unlike budget.c's weak CAS: there is no retry loop here to absorb a spurious
	// failure, and a spurious failure would send a relight down the 26x path for no reason.
	int idle = 0;
	if (!s_edit_queue ||
	    !__atomic_compare_exchange_n(&s_edit_queue_busy, &idle, 1, false,
	                                 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(&s_sweep_fallbacks, 1, __ATOMIC_RELAXED);
		return lightRelightColumnSweeps(w, cx, cz);
	}

	lightQueueInit(s_edit_queue);
	const bool ok = lightPropagateColumn(w, cx, cz, s_edit_queue);
	__atomic_store_n(&s_edit_queue_busy, 0, __ATOMIC_RELEASE);
	return ok;
}

// ── cross-column block-light handoff, the PUSH half (v1.8.10) ───────────────────
//
// crossBorderCandidate / seedCrossColumnBorder above are the PULL half: a column
// reads its neighbours' CURRENT block light whenever it is (re)computed, which is
// enough for a column that is edited or loaded NEXT TO an already-lit one. It is
// not enough the other way — an already-loaded, already-lit neighbour does not
// spontaneously notice that ITS neighbour just grew a torch near the shared edge,
// because nothing ever asks it to recompute. This is what makes that happen:
// called once, from lightRelightColumn below, after a column's own light is
// current, so the edit path (a torch placed or broken) ends with every affected
// neighbour correct rather than just the edited column.
//
// v1.8.10, second pass: the first version of this triggered a neighbour only when
// this column's border STRICTLY IMPROVED on what the neighbour already held
// (candidate > neighbour's current value). That is correct for growth — a torch
// placed near an edge — and provably wrong for retraction. Measured on the host
// probe: place a torch one cell from a column edge, relight the edited column
// only (exactly what scene/interact.c's relightEdited does), the neighbour
// correctly reads the crossed light (14 -> 13). Now delete the torch and relight
// the SAME single column again, exactly as the removal path does: the edited
// column reads 12 at the torch cell and 11 one step further, not 0/0. The
// increase-only trigger never told the neighbour anything when it was first lit,
// so the neighbour still holds its old value (13); when the edited column recomputes
// after the removal, crossBorderCandidate PULLS that stale 13 straight back in,
// producing a "ghost" value that has no real emitter behind it anywhere. This is
// exactly the failure mode chunked flood-fill engines solve with a separate
// unset/retract pass (0FPS's "smooth lighting" article and Minecraft's own light
// engine both document it) — a value that is self-consistent with its neighbour
// ("I am one less than what you show me") is not the same thing as a value that
// traces back to a real source, and PUSHING ONLY ON INCREASE can never detect the
// difference once the real source is gone.
//
// The fix does not need separate unset/reflood logic, because relightColumnCore
// already recomputes a column's ENTIRE channel from scratch every time (memset +
// reseed — see the "good news" this file's header banks on) rather than patching
// in place. That means "is this column's light still correct" reduces to "would
// relighting it again change anything" — so relightColumnCoreDiff below relights
// a column and reports whether ANYTHING in its 16 KiB block channel moved, in
// EITHER direction, and lightHandoffBorders chases that report outward instead of
// a one-directional candidate comparison. Growth and retraction fall out of the
// same code path: a grown border changes its neighbour's recompute (now pulls a
// higher candidate) and a retracted one does too (the neighbour's own recompute
// no longer finds the old high value locally-justified once ITS OWN relight also
// stops finding it, and the pair decays together — see the bound below).
//
// BOUNDED — three independent arguments, not one:
//   1. Per-hop, per-direction decay. Consider the worst case this fix exists for:
//      a torch at the max level (14) is removed right at a border, so its
//      neighbour is left holding a stale 13 with the edited column now offering
//      0 locally. Relighting the edited column pulls the stale 13 back (-1 = 12,
//      a "ghost" one step decayed) — a real, if temporary, contamination, exactly
//      what the probe measured. Relighting the NEIGHBOUR next pulls 12 back
//      (-1 = 11). Each hop strictly decreases by exactly 1 relative to two hops
//      before it (the pair is alternately re-deriving from each other's last
//      value, with nothing else feeding either side), so it cannot cycle and
//      cannot fail to reach 0 — it just takes as many hops as the peak level, at
//      most 15 per side, 30 for the pair to both bottom out. Growth converges
//      far faster (a column's OWN local recompute already includes any local
//      emitter, so pulling a neighbour's value only ever raises it to the same
//      fixpoint the old increase-only version reached, typically 1-2 hops).
//      LIGHT_HANDOFF_MAX_DEPTH=32 covers the proven worst case with a small
//      margin, not the depth this is expected to reach in play.
//   2. Column geometry. A column is 16 cells wide and 15 is the brightest level,
//      so a SINGLE emitter's cross-column reach cannot even cross one whole
//      neighbour column (14 - 16 < 0) — only a deliberately built CHAIN of
//      several torches, each within reach of the next column's edge, can push
//      more than one hop of GROWTH. Retraction of one such torch is bounded by
//      argument 1 regardless of chain length, because it only ever involves the
//      one pair straddling the removed emitter's own border.
//   3. The loaded ring. worldColumn(w, ...) reads NULL for anything not currently
//      loaded, so a push never reaches past however many columns are actually
//      loaded (RENDER_DIST_MAX_COLUMNS = 49, scene/render_dist.h, today) no
//      matter how deep a hand-built chain goes.
//   The cap firing is counted in s_handoff_capped (same idiom as s_sweep_fallbacks
//   above) rather than silently truncating, so a fixture that deliberately builds
//   a chain long enough to exceed it (tests/light_seam_test.c's
//   testHandoffDepthCap) can prove the backstop is reachable and not dead code.
//
// COST. relightColumnCoreDiff always pays one relightColumnCore (the file's own
// measured 0.156 ms median) plus one 16 KiB memcmp (negligible next to that) for
// every column it touches. lightRelightColumn only enters the handoff at all when
// the EDITED column's own channel changed — an edit with no light relevance
// (breaking a block far from every emitter and every border) costs exactly the
// one relight it always cost, zero neighbour touches. An edit that does move
// light near a border costs up to 4 neighbour relights per hop it actually
// propagates, which is the real, bounded price of correctness here, not a
// pre-existing cost this pays for edits that were never near a light source.
//
// THREAD SAFETY. This walks live neighbour columns via worldColumn(w, ...), and
// is safe for the same reason crossBorderCandidate's comment gives: every real
// caller that hands this file a MULTI-column world does so from the main thread
// only — scene/interact.c's edit path, main.c's remote-edit path, and
// world/relight_drain.c's drain, which is exactly the set light.c:26-156's
// threading note already names as "the main thread". The worker thread
// (app/worker.c JOB_GENERATE) never reaches this function with a live world: it
// calls lightPropagateColumn/lightRelightColumn directly against its OWN staging
// world (worker.c:396-397), and lightRelightColumn is the only caller of this
// function — so the one time a worker thread's call could reach it, the world in
// hand is a staging world that holds exactly one column for its whole life
// (worker.c:264 creates it, :811-813 empties it), and every worldColumn lookup
// on a neighbour coordinate there reads NULL. This function is a genuine no-op
// on that thread, not a race that happens not to have been hit yet.
//
// tests/light_race_test.c's two concurrently-relit fixture columns were moved
// apart in this version for exactly this reason: that test drives two threads
// against ONE live-style World with adjacent columns, which no real caller does
// (see the paragraph above), and adjacency would make it exercise a cross-column
// read racing a cross-column write — a scenario this file's design does not
// claim to make safe, because nothing in the real game ever produces it.
#define LIGHT_HANDOFF_MAX_DEPTH  32

static int s_handoff_capped;

// v1.8.10: the cap itself is read out of this variable, not the macro directly, so a test
// can prove it is reachable without building the 30-column chain the real bound needs —
// the same idiom lightFailEditQueueForTest above uses to prove the CAS/malloc-refusal
// fallback is reachable without actually exhausting memory. Real callers never touch this;
// it starts at, and non-test code always sees, LIGHT_HANDOFF_MAX_DEPTH.
static int s_handoff_max_depth = LIGHT_HANDOFF_MAX_DEPTH;

int  lightHandoffCapped(void)             { return __atomic_load_n(&s_handoff_capped, __ATOMIC_RELAXED); }
void lightResetHandoffCappedForTest(void) { __atomic_store_n(&s_handoff_capped, 0, __ATOMIC_RELAXED); }

void lightSetHandoffMaxDepthForTest(int depth) { s_handoff_max_depth = depth; }
void lightResetHandoffMaxDepthForTest(void)    { s_handoff_max_depth = LIGHT_HANDOFF_MAX_DEPTH; }

// Relights (cx,cz) and reports, via the return value, whether its stored block
// channel is now different from what it was immediately before this call — in
// EITHER direction. *out_ok, if non-NULL, carries relightColumnCore's own result
// (false only when the engine is off, the column is absent, or light could not
// be attached — the same cases it always returned false for).
//
// v1.8.10, third pass — the first-relight case. The second pass counted a column
// with no light attached yet as "changed" unconditionally, reasoning that it has
// never been through a relight so there is nothing for it to have been consistent
// with. True as far as it goes, and wrong as a HANDOFF trigger, because it makes
// the very first relight of any column fan out to all four of its loaded
// neighbours — each of which has also never been relit, so each also reports
// "changed", and the fan-out walks the entire loaded ring. Every one of those
// columns pays a full relightColumnCore (this file's own measured 0.156 ms
// median). With RENDER_DIST_MAX_COLUMNS = 49 loaded that is up to ~7.6 ms of a
// 16.71 ms frame spent handing off light that does not exist. It is not a
// correctness bug — the result is right — it is a large amount of provably
// pointless work, and world/relight_drain_test.c caught it as one: its control
// ("relighting column 0 did not light column 1") went red in a torch-free world
// of flat stone, where there is no block light anywhere to cross any border.
//
// So the first-relight case now asks the question the handoff actually cares
// about: does this column have any block light AT ALL to tell a neighbour about?
// An all-zero block channel cannot raise a neighbour (it offers 0) and cannot
// lower one either, because a neighbour can only be holding a value pulled from
// this column if this column once had light to be pulled — and this branch is
// reached only when it has never had a channel at all. Both directions of the
// second pass's argument are therefore preserved exactly:
//   GROWTH     — a first relight that DOES find local block light (a column
//                streamed in with a torch already in it) still returns true and
//                still hands off.
//   RETRACTION — removing the last torch happens in a column that WAS lit, so
//                had_before is true, the memcmp below fires, and the ghost-value
//                decay chain the second pass exists for is untouched.
// v1.8.18 STK-FIX. `snapshot` used to be a 16 KiB array ON THIS FUNCTION'S STACK
// (uint8_t snapshot[LIGHT_COL_BYTES]), and that made this the largest stack frame in the
// codebase: MEASURED (arm-none-eabi-objdump -d on the compiled object, before this change)
// at 16,424 bytes, 99.76% of it this one array, sitting on the worker's 32,768-byte
// WORKER_STACK_BYTES (app/worker.c:29) beneath workerMain's own locals -- worst measured
// chain 17,104 B, 52% of the stack. This function runs on the main thread (the edit path,
// via lightRelightColumn) AND on a worker lane (app/worker.c's BFS fallback at :481-482) --
// concurrently, on a New 3DS with two lanes plus the main thread all potentially inside it at
// once.
//
// A bare file-static in place of the stack array would be a data race: two threads relighting
// two different columns at the same moment would clobber the SAME buffer mid-diff, and one of
// them would compare its "before" against the wrong column's stored bytes -- silent light
// corruption, not a crash, and not something the host suite's existing byte-for-byte diff
// between the two engines would ever catch, because both engines would still agree with each
// other while disagreeing with the truth.
//
// So this follows the file's own established idiom instead of inventing a second one -- see
// s_edit_queue_busy above, which CAS-claims a single shared LightQueue and falls back to the
// queue-free sweep engine on contention rather than waiting (no locks: watchdog.h:110). Here:
// ONE static scratch buffer, claimed with the same strong CAS shape. The thread that wins
// uses it for the whole snapshot-relight-diff span (relightColumnCoreDiffInto below) and
// releases it on every exit path, via this wrapper. A thread that LOSES the race does not
// wait and does not skip the diff -- it falls back to exactly what this function did before
// this change, a stack-local snapshot (relightColumnCoreDiffStack below) -- so contention
// costs the loser one 16 KiB frame on that ONE call, not a wrong answer and not a lock. That
// fallback is CORRECT, not merely unlikely: it is byte-for-byte the pre-existing code path,
// unmodified, just moved into its own function so its frame is not paid on the common
// (uncontended) path.
static uint8_t s_snapshot_scratch[LIGHT_COL_BYTES];
static int     s_snapshot_busy;

// How many calls have taken the stack-local fallback below because the shared scratch buffer
// was already claimed -- the same observability idiom as s_sweep_fallbacks and s_handoff_capped
// elsewhere in this file: a fallback that can never be shown to trigger is dead code wearing a
// comment, not a proven-correct path. Exposed read-only via lightSnapshotFallbacks(); a test
// forces it above 0 with lightClaimSnapshotScratchForTest() below rather than by racing real
// threads and hoping for a scheduler-dependent hit.
static int s_snapshot_fallbacks;

// The actual snapshot-relight-diff logic, unchanged from what this function's body used to
// be, now taking its 16 KiB scratch buffer from the caller instead of declaring it on its own
// stack -- see the comment above the two statics for why callers hand it either the shared
// static (the common case) or a stack array of their own (the contended fallback).
//
// noinline (v1.8.18 optlight follow-up, measured, not assumed): -O3 was inlining this body
// into BOTH callers below instead of sharing one out-of-line copy -- 608 B standalone plus
// 688 B and 640 B of duplicate inlined copies in relightColumnCoreDiff and
// relightColumnCoreDiffStack, 1,936 B total for logic that was one 640 B function before this
// task. Forcing a real call recovers most of that; see the coordinator's follow-up in the
// lane report for the measured before/after.
static bool __attribute__((noinline)) relightColumnCoreDiffInto(World* w, int cx, int cz, bool* out_ok, uint8_t* snapshot)
{
	Column* col = worldColumn(w, cx, cz);
	const uint8_t* before = lightChannelBlock(col);
	const bool had_before = (before != NULL);
	if (had_before) memcpy(snapshot, before, LIGHT_COL_BYTES);

	const bool ok = relightColumnCore(w, cx, cz);
	if (out_ok) *out_ok = ok;
	if (!ok) return false;

	col = worldColumn(w, cx, cz);
	const uint8_t* after = lightChannelBlock(col);
	if (!after) return false;   // engine on, but this column still has no light
	if (!had_before) {
		// Never been lit: hand off only if there is actually block light here to
		// hand over. See the third-pass note above.
		for (size_t i = 0; i < LIGHT_COL_BYTES; i++)
			if (after[i]) return true;
		return false;
	}
	return memcmp(snapshot, after, LIGHT_COL_BYTES) != 0;
}

// The contended-fallback path: this function's own 16 KiB local array is ITS frame, not
// relightColumnCoreDiff's -- see the statics' comment above for why the split exists and why
// this is entered only when the shared static scratch buffer is already claimed.
static bool relightColumnCoreDiffStack(World* w, int cx, int cz, bool* out_ok)
{
	uint8_t snapshot[LIGHT_COL_BYTES];
	return relightColumnCoreDiffInto(w, cx, cz, out_ok, snapshot);
}

static bool relightColumnCoreDiff(World* w, int cx, int cz, bool* out_ok)
{
	// Strong CAS, same reasoning as s_edit_queue_busy above: there is no retry loop here to
	// absorb a spurious failure, and this claim's failure path already does something
	// correct, so a spurious failure would only cost one avoidable stack frame, never a
	// wrong answer.
	int idle = 0;
	if (!__atomic_compare_exchange_n(&s_snapshot_busy, &idle, 1, false,
	                                 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(&s_snapshot_fallbacks, 1, __ATOMIC_RELAXED);
		return relightColumnCoreDiffStack(w, cx, cz, out_ok);
	}

	const bool result = relightColumnCoreDiffInto(w, cx, cz, out_ok, s_snapshot_scratch);
	__atomic_store_n(&s_snapshot_busy, 0, __ATOMIC_RELEASE);
	return result;
}

int  lightSnapshotFallbacks(void) { return s_snapshot_fallbacks; }
void lightResetSnapshotFallbacksForTest(void) { s_snapshot_fallbacks = 0; }

// Test hooks that let a single-threaded test force relightColumnCoreDiff's CAS to lose without
// racing a second real thread against the clock. Claiming here from test code and claiming from
// relightColumnCoreDiff itself are the SAME CAS on the SAME static, so a test that holds the
// claim and then calls into the public API (lightRelightColumn) deterministically drives the
// stack-local fallback (relightColumnCoreDiffStack) and can check its answer is still correct --
// proving the fallback is correct on purpose, not merely rare. Mirrors lightFailEditQueueForTest
// and lightSetHandoffMaxDepthForTest above, which force their own file-static state the same way.
bool lightClaimSnapshotScratchForTest(void)
{
	int idle = 0;
	return __atomic_compare_exchange_n(&s_snapshot_busy, &idle, 1, false,
	                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void lightReleaseSnapshotScratchForTest(void)
{
	__atomic_store_n(&s_snapshot_busy, 0, __ATOMIC_RELEASE);
}

static void lightHandoffBorders(World* w, int cx, int cz, int depth)
{
	if (depth >= s_handoff_max_depth) {
		__atomic_fetch_add(&s_handoff_capped, 1, __ATOMIC_RELAXED);
		return;
	}

	static const int8_t dcx[4] = { 1, -1, 0, 0 };
	static const int8_t dcz[4] = { 0, 0, 1, -1 };

	for (int d = 0; d < 4; d++) {
		const int ncx = cx + dcx[d], ncz = cz + dcz[d];
		if (!worldColumn(w, ncx, ncz)) continue;   // that neighbour is not loaded at all

		if (relightColumnCoreDiff(w, ncx, ncz, NULL))
			lightHandoffBorders(w, ncx, ncz, depth + 1);
	}
}

bool lightRelightColumn(World* w, int cx, int cz)
{
	bool ok = false;
	const bool changed = relightColumnCoreDiff(w, cx, cz, &ok);
	if (!ok) return false;
	if (changed) lightHandoffBorders(w, cx, cz, 0);
	return true;
}

bool lightRelightColumnSweeps(World* w, int cx, int cz)
{
	if (!s_enabled) return false;

	Column* col = worldColumn(w, cx, cz);
	if (!col) return false;
	if (!lightColumnAttach(col)) return false;

	// v1.8.18 optlight follow-up: same claim lightPropagateColumn takes, and for the same
	// reason -- see the comment above lightColumnClaimTry. This engine writes the identical
	// shared lc->sky/blk arrays, so it needs the identical guard.
	if (!lightColumnClaimTry(col)) return false;

	LightColumn* lc = (LightColumn*)col->light;
	memset(lc, 0, sizeof(*lc));

	const Chunk* chunks[COLUMN_CHUNKS];
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
		chunks[cy] = worldChunk(w, cx, cy, cz);

	// v1.8.18 CHK-FIX: see ChunkUniformCache's comment above opaqueAt.
	ChunkUniformCache uni;
	chunkUniformCacheFill(&uni, chunks);

	HeightMap hm;
	heightMapFill(&hm, chunks, &uni);

	const bool lum_any = syncLuminance();

	bool changed = true;
	while (changed) {
		changed = false;

		for (int y = 0; y < WORLD_HEIGHT; y++) {
			for (int z = 0; z < CHUNK_DIM; z++) {
				for (int x = 0; x < CHUNK_DIM; x++) {
					const int ci = lightIndex(x, y, z);

					if (opaqueAt(chunks, &uni, x, y, z)) {
						// An opaque cell takes no light in — the BFS never offers
						// one a level, so neither may the sweeps — but a luminous
						// one still emits: store its level so neighbours can read
						// it, exactly like the BFS seeding the emitter directly.
						if (lum_any) {
							const Chunk* c = chunks[y >> 4];
							const uint8_t emit =
							    c ? s_luminance[chunkGet(c, chunkIndex(x, y & 15, z))]
							      : 0;
							if (emit != lightNibble(lc->blk, ci)) {
								setNibble(lc->blk, ci, emit);
								changed = true;
							}
						}
						continue;
					}

					// Seed values, recomputed in place: direct sun above the
					// strip's top, block emission from the table.
					uint8_t sky = (y > hm.top[z][x]) ? 15 : 0;
					uint8_t blk = 0;
					if (lum_any) {
						const Chunk* c = chunks[y >> 4];
						if (c) blk = s_luminance[chunkGet(c, chunkIndex(x, y & 15, z))];
					}

					uint8_t best_s = sky, best_b = blk;
					for (int d = 0; d < 6; d++) {
						const int nx = x + kDirs[d][0];
						const int ny = y + kDirs[d][1];
						const int nz = z + kDirs[d][2];
						if (nx < 0 || nx >= CHUNK_DIM || nz < 0 || nz >= CHUNK_DIM) continue;
						if (ny < 0 || ny >= WORLD_HEIGHT) continue;

						// Opaque neighbours are read, not skipped: they never
						// receive light, so all one holds is its own emission —
						// zero for stone, the luminance for a glowing block. The
						// flood fill pushes straight out of such a block, and the
						// pull has to be able to read that same level back or the
						// two engines part ways at every sealed-in torch.
						const int nci = lightIndex(nx, ny, nz);
						const int ns = lightNibble(lc->sky, nci);
						const int nb = lightNibble(lc->blk, nci);
						if (ns > best_s) best_s = (uint8_t)(ns - 1);
						if (nb > best_b) best_b = (uint8_t)(nb - 1);
					}

					// v1.8.10 cross-column handoff, the sweep engine's half.
					// crossBorderCandidate is the SAME function the BFS engine
					// seeds from (see its comment above lightPropagateColumn) —
					// called with the same inputs it must produce the same
					// answer, which is what keeps the two engines agreeing byte
					// for byte on a column next to a lit neighbour. Gated on
					// x/z first, cheaply, so the function call itself only
					// happens for the border cells, on every one of this loop's
					// (possibly several) passes.
					if (lum_any &&
					    (x == 0 || x == CHUNK_DIM - 1 || z == 0 || z == CHUNK_DIM - 1)) {
						const int cand = crossBorderCandidate(w, cx, cz, x, y, z);
						if (cand > best_b) best_b = (uint8_t)cand;
					}

					if (best_s != lightNibble(lc->sky, ci)) {
						setNibble(lc->sky, ci, best_s);
						changed = true;
					}
					if (best_b != lightNibble(lc->blk, ci)) {
						setNibble(lc->blk, ci, best_b);
						changed = true;
					}
				}
			}
		}
	}

	lightColumnClaimRelease(col);
	return true;
}

// Sets a test override that syncLuminance() lays over whatever the registry says.
// Level 0 REMOVES the override rather than forcing the block dark, which is what the
// suite's two "restore production truth" calls have always meant — and with the
// registry now feeding the table, forcing a zero over a declared luminance would be a
// way to make the test disagree with the game. No core row declares a luminance, so
// those calls still land on 0 either way.
void lightSetLuminanceForTest(BlockId id, uint8_t level)
{
	s_lum_test[id] = level;
}

void lightSetSkyForTest(Column* col, int lx, int y, int lz, uint8_t level)
{
	if (!col || !col->light) return;
	setNibble(((LightColumn*)col->light)->sky, lightIndex(lx, y, lz), level);
}
