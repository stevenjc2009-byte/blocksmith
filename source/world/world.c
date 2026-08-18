#include "world/world.h"

#include <stdlib.h>
#include <string.h>

#include "world/budget.h"

static uint32_t hashKey(int32_t cx, int32_t cz)
{
	// Two odd multipliers then an xorshift finaliser. Chunk coordinates arrive in
	// scan order, so a weak hash would put every column of a row in adjacent slots
	// and turn lookups into long probe walks.
	uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ (uint32_t)cz * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2545F491u;
	h ^= h >> 13;
	return h;
}

// Returns the slot holding this column, or the empty slot where it would go.
// NULL only when the table is completely full.
static Column** slotFor(const World* w, int32_t cx, int32_t cz)
{
	Column** slots = (Column**)w->slots;
	const uint32_t start = hashKey(cx, cz) & (WORLD_MAP_SLOTS - 1);

	for (int probe = 0; probe < WORLD_MAP_SLOTS; probe++) {
		Column** s = &slots[(start + (uint32_t)probe) & (WORLD_MAP_SLOTS - 1)];
		if (!*s || ((*s)->cx == cx && (*s)->cz == cz)) return s;
	}
	return NULL;
}

void worldInit(World* w)
{
	memset(w, 0, sizeof(*w));
}

void worldExit(World* w)
{
	for (int i = 0; i < WORLD_MAP_SLOTS; i++) {
		Column* col = w->slots[i];
		if (!col) continue;

		for (int y = 0; y < COLUMN_CHUNKS; y++) {
			if (!col->chunks[y]) continue;
			free(col->chunks[y]);
			budgetRelease(sizeof(Chunk));
		}
		free(col);
		budgetRelease(sizeof(Column));
	}
	memset(w, 0, sizeof(*w));
}

Column* worldColumn(const World* w, int cx, int cz)
{
	Column** s = slotFor(w, cx, cz);
	return s ? *s : NULL;
}

Column* worldColumnCreate(World* w, int cx, int cz)
{
	Column** s = slotFor(w, cx, cz);
	if (!s) return NULL;          // table full
	if (*s) return *s;

	if (!budgetClaim(sizeof(Column))) return NULL;

	Column* col = (Column*)calloc(1, sizeof(Column));
	if (!col) {
		budgetRelease(sizeof(Column));
		return NULL;
	}

	col->cx = cx;
	col->cz = cz;
	*s = col;
	w->columns++;
	return col;
}

Chunk* worldChunk(const World* w, int cx, int cy, int cz)
{
	if ((unsigned)cy >= COLUMN_CHUNKS) return NULL;
	Column* col = worldColumn(w, cx, cz);
	return col ? col->chunks[cy] : NULL;
}

Chunk* worldChunkCreate(World* w, int cx, int cy, int cz)
{
	if ((unsigned)cy >= COLUMN_CHUNKS) return NULL;

	Column* col = worldColumnCreate(w, cx, cz);
	if (!col) return NULL;
	if (col->chunks[cy]) return col->chunks[cy];

	if (!budgetClaim(sizeof(Chunk))) return NULL;

	Chunk* c = (Chunk*)malloc(sizeof(Chunk));
	if (!c) {
		budgetRelease(sizeof(Chunk));
		return NULL;
	}

	chunkClear(c, BLOCK_AIR);
	col->chunks[cy] = c;
	w->chunks++;
	return c;
}

bool worldColumnRemove(World* w, int cx, int cz)
{
	Column** s = slotFor(w, cx, cz);
	if (!s || !*s) return false;

	Column* col = *s;
	for (int y = 0; y < COLUMN_CHUNKS; y++) {
		if (!col->chunks[y]) continue;
		free(col->chunks[y]);
		budgetRelease(sizeof(Chunk));
		w->chunks--;
	}
	free(col);
	budgetRelease(sizeof(Column));
	w->columns--;

	// Knuth 6.4 algorithm R. This table has no tombstones — slotFor stops at the first
	// empty slot — so emptying a slot in the middle of a probe chain would cut every
	// column behind it out of the world while its memory is still claimed. Each entry
	// that probed *past* this hole has to be walked back into it.
	//
	// The cheap-looking alternative, a tombstone flag, was rejected: the streaming ring
	// deletes a column every time the player crosses a boundary, forever, so tombstones
	// would accumulate until every lookup walked the whole table.
	uint32_t i = (uint32_t)(s - w->slots);
	w->slots[i] = NULL;

	uint32_t j = i;
	for (;;) {
		j = (j + 1) & (WORLD_MAP_SLOTS - 1);
		Column* c = w->slots[j];
		if (!c) break;

		// Where this entry *wanted* to be. If that home position lies cyclically inside
		// (i, j] it is already at or after the hole and must stay put; otherwise it
		// probed past the hole and moving it back keeps its chain unbroken.
		const uint32_t k = hashKey(c->cx, c->cz) & (WORLD_MAP_SLOTS - 1);
		const bool movable = (i <= j) ? (k <= i || k > j) : (k <= i && k > j);
		if (!movable) continue;

		w->slots[i] = c;
		w->slots[j] = NULL;
		i = j;
	}
	return true;
}

BlockId worldGet(const World* w, int x, int y, int z)
{
	if (y < 0) return WORLD_FLOOR_BLOCK;
	if (y >= WORLD_HEIGHT) return BLOCK_AIR;

	const Chunk* c = worldChunk(w, x >> 4, y >> 4, z >> 4);
	if (!c) return BLOCK_AIR;
	return c->blocks[chunkIndex(x & 15, y & 15, z & 15)];
}

bool worldSet(World* w, int x, int y, int z, BlockId id)
{
	if (y < 0 || y >= WORLD_HEIGHT) return false;

	Chunk* c = worldChunk(w, x >> 4, y >> 4, z >> 4);
	if (!c) {
		// A fresh chunk is all air, so storing air into one that does not exist
		// yet would allocate 4 KB to record nothing.
		if (id == BLOCK_AIR) return true;

		c = worldChunkCreate(w, x >> 4, y >> 4, z >> 4);
		if (!c) return false;
	}

	c->blocks[chunkIndex(x & 15, y & 15, z & 15)] = id;
	return true;
}

size_t worldBytes(const World* w)
{
	return (size_t)w->columns * sizeof(Column) + (size_t)w->chunks * sizeof(Chunk);
}
