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
static int     s_attached;
static int     s_queue_dropped;   // BFS refusals this call; must stay zero (see the CAP note)
// The effective luminance both engines read, one entry per id in the WHOLE registry
// id space, core and dynamic alike — not BLOCK_COUNT entries.
//
// This is a CACHE of BlockDef.luminance, rebuilt by syncLuminance() below. Nothing
// else writes it: the registry is the single source of truth for what a block emits.
static uint8_t s_luminance[REGISTRY_MAX];

// Test overrides, laid over the registry by syncLuminance(). Zero means "no override"
// — see lightSetLuminanceForTest, which is the only writer.
static uint8_t s_lum_test[REGISTRY_MAX];

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
	}
}

bool lightFastEngineReady(void) { return s_enabled && !s_edit_queue_refused; }
int  lightSweepFallbacks(void)  { return s_sweep_fallbacks; }

void lightFailEditQueueForTest(bool fail)   { s_fail_edit_queue_for_test = fail; }
void lightResetSweepFallbacksForTest(void)  { s_sweep_fallbacks = 0; }

bool lightEnabled(void)           { return s_enabled; }

int    lightColumnsAttached(void) { return s_attached; }
size_t lightBytesUsed(void)       { return (size_t)s_attached * sizeof(LightColumn); }

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
	s_attached++;
	return true;
}

void lightColumnDetach(Column* col)
{
	if (!col || !col->light) return;

	budgetRelease(sizeof(LightColumn));
	free(col->light);
	col->light = NULL;
	s_attached--;
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

// Opaque means "stops light": solid and not transparent, byte-for-byte the same
// predicate the mesher's occlusion table is built from. Air passes; leaves pass.
static bool opaqueAt(const Chunk* const chunks[COLUMN_CHUNKS], int lx, int y, int lz)
{
	if (y < 0) return true;   // below the floor reads as solid, like worldGet
	const Chunk* c = chunks[y >> 4];
	if (!c) return false;     // absent chunk is air
	const BlockId id = chunkGet(c, chunkIndex(lx, y & 15, lz));
	const BlockInfo* info = blockInfo(id);
	return info->solid && !info->transparent;
}

// Highest non-air cell per (x,z), -1 when the strip is all air. Everything above
// top[x][z] is a straight shot of sunlight at 15; everything at or below it is
// not, whatever it is made of.
typedef struct {
	int8_t top[CHUNK_DIM][CHUNK_DIM];
} HeightMap;

static void heightMapFill(HeightMap* hm, const Chunk* const chunks[COLUMN_CHUNKS])
{
	for (int z = 0; z < CHUNK_DIM; z++) {
		for (int x = 0; x < CHUNK_DIM; x++) {
			int y = WORLD_HEIGHT - 1;
			while (y >= 0) {
				const Chunk* c = chunks[y >> 4];
				if (c && chunkGet(c, chunkIndex(x, y & 15, z)) != BLOCK_AIR) break;
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
static bool syncLuminance(void)
{
	bool any = false;
	for (int i = 0; i < REGISTRY_MAX; i++) {
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

static bool queuePush(LightQueue* q, uint16_t ci)
{
	if (q->count >= LIGHT_QUEUE_CAP) { s_queue_dropped++; return false; }
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
                   LightQueue* q)
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
			if (opaqueAt(chunks, nx, ny, nz)) continue;

			const int nci = lightIndex(nx, ny, nz);
			if (level - 1 <= lightNibble(chan, nci)) continue;

			setNibble(chan, nci, (uint8_t)(level - 1));
			queuePush(q, (uint16_t)nci);
		}
	}
}

bool lightPropagateColumn(World* w, int cx, int cz, LightQueue* q)
{
	if (!s_enabled || !q) return false;

	Column* col = worldColumn(w, cx, cz);
	if (!col) return false;
	if (!lightColumnAttach(col)) return false;

	LightColumn* lc = (LightColumn*)col->light;
	memset(lc, 0, sizeof(*lc));
	s_queue_dropped = 0;

	// The column's chunks, resolved once. Gaps are air, matching scratch.c.
	const Chunk* chunks[COLUMN_CHUNKS];
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
		chunks[cy] = worldChunk(w, cx, cy, cz);

	HeightMap hm;
	heightMapFill(&hm, chunks);

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
				if (y == hm.top[z][x] + 1 && !opaqueAt(chunks, x, y - 1, z))
					border = true;

				if (border) queuePush(q, (uint16_t)lightIndex(x, y, z));
			}
		}
	}
	spread(lc->sky, chunks, q);

	// Block seeds: luminous cells. syncLuminance() refreshes the table from the
	// registry and answers whether anything in it emits at all; no core row declares
	// a luminance today, so on a single-player world the scan is skipped entirely.
	if (syncLuminance()) {
		for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
			if (!chunks[cy]) continue;
			for (int i = 0; i < CHUNK_BLOCKS; i++) {
				const BlockId id = chunkGet(chunks[cy], i);
				const uint8_t lum = s_luminance[id];
				if (!lum) continue;

				// chunkIndex order: x low, then z, then y — same shape the
				// column-wide lightIndex extends over height.
				const int ci = lightIndex(i & 15, (i >> 8) + cy * CHUNK_DIM,
				                          (i >> 4) & 15);
				setNibble(lc->blk, ci, lum);
				queuePush(q, (uint16_t)ci);
			}
		}
		spread(lc->blk, chunks, q);
	}

	return s_queue_dropped == 0;
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
bool lightRelightColumn(World* w, int cx, int cz)
{
	if (!s_enabled) return false;

	// v1.8.3: counted on the way past. Without this the fallback is indistinguishable from the
	// fast path at every level above it — same signature, same return value, same light, 26x the
	// cost. See s_sweep_fallbacks above for why a counter and not a log line.
	if (!s_edit_queue) {
		s_sweep_fallbacks++;
		return lightRelightColumnSweeps(w, cx, cz);
	}

	lightQueueInit(s_edit_queue);
	return lightPropagateColumn(w, cx, cz, s_edit_queue);
}

bool lightRelightColumnSweeps(World* w, int cx, int cz)
{
	if (!s_enabled) return false;

	Column* col = worldColumn(w, cx, cz);
	if (!col) return false;
	if (!lightColumnAttach(col)) return false;

	LightColumn* lc = (LightColumn*)col->light;
	memset(lc, 0, sizeof(*lc));

	const Chunk* chunks[COLUMN_CHUNKS];
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
		chunks[cy] = worldChunk(w, cx, cy, cz);

	HeightMap hm;
	heightMapFill(&hm, chunks);

	const bool lum_any = syncLuminance();

	bool changed = true;
	while (changed) {
		changed = false;

		for (int y = 0; y < WORLD_HEIGHT; y++) {
			for (int z = 0; z < CHUNK_DIM; z++) {
				for (int x = 0; x < CHUNK_DIM; x++) {
					const int ci = lightIndex(x, y, z);

					if (opaqueAt(chunks, x, y, z)) {
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
