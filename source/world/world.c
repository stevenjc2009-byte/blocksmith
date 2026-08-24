#include "world/world.h"

#include <stdlib.h>
#include <string.h>

#include "net/networld.h"
#include "world/budget.h"

// v1.5.0 adaptive lighting. Included, not linked: tools/run_host_tests.sh names
// its files explicitly and predates world/light.c, so the engine rides inside
// this translation unit and every host binary that links world.o — the world
// suite, networld_test, inv_bridge_test — gets its symbols from here. The
// console Makefile filters light.c out of its wildcard scan for the same reason,
// so there is exactly one definition per link on either platform. Edit the two
// halves of this arrangement together.
#include "world/light.c"

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

// Returns the index of the slot holding this column, or of the empty slot where
// it would go. Negative only when the table is completely full.
//
// An index rather than a Column**: the probe is identical for readers and
// writers, but returning an interior pointer from a `const World*` meant
// casting the const straight off w->slots, which -Wcast-qual rightly objects
// to. An index carries no qualifier, so readers index w->slots[i] through their
// const pointer and writers assign to it through theirs, with no cast either
// side. worldColumnRemove wanted the index back anyway.
static int slotIndexFor(const World* w, int32_t cx, int32_t cz)
{
	const uint32_t start = hashKey(cx, cz) & (WORLD_MAP_SLOTS - 1);

	for (int probe = 0; probe < WORLD_MAP_SLOTS; probe++) {
		const uint32_t i = (start + (uint32_t)probe) & (WORLD_MAP_SLOTS - 1);
		const Column* c = w->slots[i];
		if (!c || (c->cx == cx && c->cz == cz)) return (int)i;
	}
	return -1;
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
			// Release whatever this chunk's CURRENT form actually costs, not a fixed
			// sizeof(Chunk) — since step 9.2a a promoted chunk (PALETTE4 or RAW) claimed
			// more than the UNIFORM bytes it started at, and releasing the wrong amount
			// would leave the budget singleton permanently out of balance.
			budgetRelease(chunkGetBytes(col->chunks[y]));
			chunkFree(col->chunks[y]);
		}
		lightColumnDetach(col);   // no-op unless lighting attached to this column
		free(col);
		budgetRelease(sizeof(Column));
	}
	memset(w, 0, sizeof(*w));
}

Column* worldColumn(const World* w, int cx, int cz)
{
	const int i = slotIndexFor(w, cx, cz);
	return i < 0 ? NULL : w->slots[i];
}

Column* worldColumnCreate(World* w, int cx, int cz)
{
	const int i = slotIndexFor(w, cx, cz);
	if (i < 0) return NULL;          // table full
	if (w->slots[i]) return w->slots[i];

	if (!budgetClaim(sizeof(Column))) return NULL;

	Column* col = (Column*)calloc(1, sizeof(Column));
	if (!col) {
		budgetRelease(sizeof(Column));
		return NULL;
	}

	col->cx = cx;
	col->cz = cz;
	w->slots[i] = col;
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

	// A freshly created chunk always starts UNIFORM (all air) — chunkAlloc's own contract
	// — so this is the one and only form whose bytes a brand new chunk can possibly need.
	const size_t bytes = chunkFormBytes(CHUNK_FORM_UNIFORM);
	if (!budgetClaim(bytes)) return NULL;

	Chunk* c = chunkAlloc(BLOCK_AIR);
	if (!c) {
		budgetRelease(bytes);
		return NULL;
	}

	col->chunks[cy] = c;
	w->chunks++;
	return c;
}

bool worldColumnRemove(World* w, int cx, int cz)
{
	const int slot = slotIndexFor(w, cx, cz);
	if (slot < 0 || !w->slots[slot]) return false;

	Column* col = w->slots[slot];
	for (int y = 0; y < COLUMN_CHUNKS; y++) {
		if (!col->chunks[y]) continue;
		budgetRelease(chunkGetBytes(col->chunks[y]));   // see worldExit for why this is per-chunk
		chunkFree(col->chunks[y]);
		w->chunks--;
	}
	lightColumnDetach(col);   // no-op unless lighting attached to this column
	free(col);
	budgetRelease(sizeof(Column));
	w->columns--;

	// Knuth 6.4 algorithm R. This table has no tombstones — slotIndexFor stops at the first
	// empty slot — so emptying a slot in the middle of a probe chain would cut every
	// column behind it out of the world while its memory is still claimed. Each entry
	// that probed *past* this hole has to be walked back into it.
	//
	// The cheap-looking alternative, a tombstone flag, was rejected: the streaming ring
	// deletes a column every time the player crosses a boundary, forever, so tombstones
	// would accumulate until every lookup walked the whole table.
	uint32_t i = (uint32_t)slot;
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

void worldMarkDirty(World* w, int x, int z)
{
	Column* col = worldColumn(w, x >> 4, z >> 4);
	if (col) col->dirty = true;
}

BlockId worldGet(const World* w, int x, int y, int z)
{
	if (y < 0) return WORLD_FLOOR_BLOCK;
	if (y >= WORLD_HEIGHT) return BLOCK_AIR;

	const Chunk* c = worldChunk(w, x >> 4, y >> 4, z >> 4);
	if (!c) return BLOCK_AIR;
	return chunkGet(c, chunkIndex(x & 15, y & 15, z & 15));
}

int worldStandingY(const World* w, int x, int z, int base)
{
	int y = base;
	if (y < 0) y = 0;

	// Bounded by the world, not by "it cannot take many steps": a column solid all the way to
	// the ceiling — a wall the player built, or a pillar — must end this loop rather than walk
	// off the top. WORLD_HEIGHT - 2 is the highest y at which a 1.8-tall body still fits.
	while (y < WORLD_HEIGHT - 2 &&
	       (blockIsSolid(worldGet(w, x, y,     z)) ||
	        blockIsSolid(worldGet(w, x, y + 1, z))))
		y++;

	return y;
}

// v1.8.0 task 22. See world.h for why there is one of these and why it lives here.
static WorldEditFn s_edit_fn;
static void*       s_edit_ud;

void worldSetEditHook(WorldEditFn fn, void* ud)
{
	s_edit_fn = fn;
	s_edit_ud = ud;
}

bool worldSet(World* w, int x, int y, int z, BlockId id)
{
	if (y < 0 || y >= WORLD_HEIGHT) return false;

	// Read only when somebody is listening. worldSet is on the streaming path via
	// net/blockdiff and on the water simulation's own write path, and an unconditional
	// chunkGet here would put a decompress on both for a value nothing looks at.
	const BlockId prev = s_edit_fn ? worldGet(w, x, y, z) : (BlockId)BLOCK_AIR;

	Chunk* c = worldChunk(w, x >> 4, y >> 4, z >> 4);
	if (!c) {
		// A fresh chunk is all air, so storing air into one that does not exist
		// yet would allocate 4 KB to record nothing.
		if (id == BLOCK_AIR) return true;

		// net/networld.c needs to know the instant a column that did not exist a moment ago
		// becomes real, so any remote edits queued for it (net/blockdiff.h) can land. Checked
		// before the create call, not after — worldChunkCreate() cannot tell a brand new
		// column from one that already held a different vertical chunk.
		const bool new_column = worldColumn(w, x >> 4, z >> 4) == NULL;

		c = worldChunkCreate(w, x >> 4, y >> 4, z >> 4);
		if (!c) return false;

		if (new_column) networldOnColumnLoad(w, x >> 4, z >> 4);
	}

	// Pre-flight, then commit: chunkFormFor is a pure query, so the budget is asked BEFORE
	// anything about c changes. A write that fits c's current form needs zero extra bytes
	// (chunkFormBytes(form_after) - chunkGetBytes(c) is <= 0 whenever form_after == c's
	// current form, since chunkFormFor never reports a smaller form than c already has) and
	// so never touches the budget at all — this is the common case, an edit to already-RAW
	// or already-matching-PALETTE4 terrain, and it stays exactly as cheap as before this
	// step. Only an actual promotion claims anything, and if that claim is refused, chunkSet
	// is never called, so c is left exactly as it was: still whatever form it was in, still
	// holding whatever it held. There is no path here that claims and then fails to commit.
	const ChunkForm form_before = chunkGetForm(c);
	const ChunkForm form_after  = chunkFormFor(c, id);
	const size_t bytes_before = chunkFormBytes(form_before);
	const size_t bytes_after  = chunkFormBytes(form_after);

	if (bytes_after > bytes_before) {
		if (!budgetClaim(bytes_after - bytes_before)) return false;
	}

	if (!chunkSet(c, chunkIndex(x & 15, y & 15, z & 15), id)) {
		// The only way chunkSet can fail is the promotion's own malloc failing, in which
		// case c is untouched — release the claim above, it was never spent.
		if (bytes_after > bytes_before) budgetRelease(bytes_after - bytes_before);
		return false;
	}

	if (s_edit_fn && prev != id) s_edit_fn(s_edit_ud, x, y, z, prev, id);
	return true;
}

bool worldSetChunkAll(World* w, int cx, int cy, int cz, const BlockId in[CHUNK_BLOCKS])
{
	if ((unsigned)cy >= COLUMN_CHUNKS) return false;

	Column* col = worldColumnCreate(w, cx, cz);
	if (!col) return false;

	// v1.7.1 task 49 (install half). Both halves of the pre-flight/commit pair below need
	// `in`'s palette, and both used to derive it from scratch: chunkFormForAll walked the
	// 4096 cells to size the budget claim and chunkLoadAll walked them again to commit.
	// One walk, one plan, both answers — see chunk.h's ChunkPlan for the measurement that
	// made this worth doing. 276 bytes of stack; the deepest caller of this function is
	// worldgen on the 32 KB worker thread.
	ChunkPlan plan;
	const ChunkForm form = chunkPlanAll(in, &plan);

	Chunk* c = col->chunks[cy];
	if (!c) {
		// No existing chunk to size a delta against — this is exactly worldChunkCreate's
		// own claim, done here instead of by calling it, so that the chunk this function
		// commits is never first allocated UNIFORM and then immediately promoted: that
		// would claim the UNIFORM bytes, free them, and claim the real form's bytes right
		// after, for no reason other than code reuse. worldgen.c calls this once per chunk
		// specifically to avoid paying for cell-by-cell promotion; doing it here anyway
		// would defeat the point.
		const size_t bytes = chunkFormBytes(form);
		if (!budgetClaim(bytes)) return false;

		c = chunkAlloc(BLOCK_AIR);
		if (!c) { budgetRelease(bytes); return false; }

		if (!chunkLoadPlanned(c, in, &plan)) {
			// chunkAlloc succeeded (it is UNIFORM, which cannot fail its own claim logic
			// here since chunkLoadPlanned only fails on a promotion's malloc) but
			// chunkLoadPlanned itself failed on its allocation — free the still-UNIFORM
			// shell and give the claim back.
			budgetRelease(bytes);
			chunkFree(c);
			return false;
		}

		col->chunks[cy] = c;
		w->chunks++;
		return true;
	}

	// Replacing an existing chunk's content: the same pre-flight/claim/commit/release shape
	// worldSet uses, except chunkLoadPlanned may also SHRINK c (it is a wholesale replace, not
	// an incremental edit — see chunk.h), so bytes_after can be smaller than bytes_before.
	// That is not refused; it is not claimed either. Nothing here releases the shrink until
	// after the commit actually happens, because chunkGetBytes(c) has to be read again
	// afterward to know how much the form actually changed by — reading it before commit and
	// assuming chunkPlanAll's answer is what will be installed is correct today (nothing
	// else can be running between the query and the commit, single-threaded), but computing
	// the release from the post-commit form rather than the pre-flight query keeps this
	// function honest even if that ever changes.
	const size_t bytes_before = chunkGetBytes(c);
	const size_t bytes_after_query = chunkFormBytes(form);

	if (bytes_after_query > bytes_before) {
		if (!budgetClaim(bytes_after_query - bytes_before)) return false;
	}

	if (!chunkLoadPlanned(c, in, &plan)) {
		if (bytes_after_query > bytes_before)
			budgetRelease(bytes_after_query - bytes_before);
		return false;
	}

	const size_t bytes_after = chunkGetBytes(c);
	if (bytes_after < bytes_before) budgetRelease(bytes_before - bytes_after);
	return true;
}

size_t worldBytes(const World* w)
{
	size_t bytes = (size_t)w->columns * sizeof(Column);
	for (int i = 0; i < WORLD_MAP_SLOTS; i++) {
		const Column* col = w->slots[i];
		if (!col) continue;
		for (int y = 0; y < COLUMN_CHUNKS; y++)
			if (col->chunks[y]) bytes += chunkGetBytes(col->chunks[y]);
	}
	return bytes;
}
