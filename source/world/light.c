#include "world/light.h"

#include <stdlib.h>
#include <string.h>

#include "world/budget.h"

// The adaptive lighting engine's implementation. This file is NOT compiled
// standalone: world.c includes it, because tools/run_host_tests.sh names its
// files explicitly and predates this module, so every host binary that links
// world.o needs the engine's symbols to arrive inside it. The console Makefile
// filters this file out of its own wildcard scan for the same reason — see the
// comment there. Edit either half together.
//
// Both channels of one column, 8 KiB, claimed from the world budget on attach.
// The pointer lives in Column.light and is owned end-to-end by this file: world.c
// only ever calls lightColumnDetach on the way out.
typedef struct {
	uint8_t sky[LIGHT_COL_BYTES];
	uint8_t blk[LIGHT_COL_BYTES];
} LightColumn;

static bool    s_enabled;
static int     s_attached;
static int     s_queue_dropped;   // BFS refusals this call; must stay zero (see the CAP note)
static uint8_t s_luminance[256];  // all zeros: no block in the registry emits yet

// The six in-column neighbour offsets, shared by both engines.
static const int8_t kDirs[6][3] = {
	{ 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1},
};

void lightEngineInit(bool enable) { s_enabled = enable; }
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

static bool anyLuminance(void)
{
	for (int i = 0; i < BLOCK_COUNT; i++)
		if (s_luminance[i]) return true;
	return false;
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
				if (border) queuePush(q, (uint16_t)lightIndex(x, y, z));
			}
		}
	}
	spread(lc->sky, chunks, q);

	// Block seeds: luminous cells. No registry block emits today, so the scan is
	// skipped entirely until a test (or a future torch) puts a value in the table.
	if (anyLuminance()) {
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

// ── engine 2: relaxation sweeps ──────────────────────────────────────────────

// Same fixpoint as the flood fill, reached without a queue: sweep every cell in
// a fixed order, raising each towards the brightest legal offer from its seeds
// and neighbours, until a pass changes nothing. Values only ever rise and are
// bounded by 15, so termination is not a hope; and because the constraint system
// is monotone (max-only updates), the fixpoint does not depend on the order the
// sweeps visit cells in — which is what lets two such different engines agree.
bool lightRelightColumn(World* w, int cx, int cz)
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

	const bool lum_any = anyLuminance();

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

void lightSetLuminanceForTest(BlockId id, uint8_t level)
{
	s_luminance[id] = level;
}

void lightSetSkyForTest(Column* col, int lx, int y, int lz, uint8_t level)
{
	if (!col || !col->light) return;
	setNibble(((LightColumn*)col->light)->sky, lightIndex(lx, y, lz), level);
}
