// The world: columns of chunks, looked up by chunk coordinate.
//
// A column is a fixed vertical stack of 8 chunks, so the world is 128 blocks tall.
// Columns are the unit of loading because terrain generation and saving are both
// naturally per-(x,z); chunks inside a column are allocated lazily, so a sky chunk
// that is entirely air costs nothing but a NULL pointer.
//
// Coordinate conventions used throughout:
//   * *world* block coordinates are signed and unbounded in x and z, and clamped
//     in y to 0 .. WORLD_HEIGHT-1.
//   * chunk coordinate = block >> 4, local coordinate = block & 15. Arithmetic
//     shift gives the right floor behaviour for negative coordinates, which plain
//     division does not.
//   * below the world floor reads as WORLD_FLOOR_BLOCK and above the ceiling as
//     air, so the mesher never has to special-case the edges of the world and the
//     bottom face of the world is never emitted.
#pragma once

#include <stddef.h>

#include "world/chunk.h"

#define COLUMN_CHUNKS      8
#define WORLD_HEIGHT       (COLUMN_CHUNKS * CHUNK_DIM)   // 128
#define WORLD_FLOOR_BLOCK  BLOCK_STONE

// Power of two, open addressing with linear probing. A 17x17 render distance is
// 289 columns, so 1024 slots keeps the load factor under 0.3 and probes short.
#define WORLD_MAP_SLOTS    1024

typedef struct {
	int32_t cx, cz;
	Chunk*  chunks[COLUMN_CHUNKS];

	// Step 8.1. Set when the *player* has changed a block in this column, and the only
	// thing that decides whether the column is written to the SD card when it unloads.
	//
	// Deliberately not set by worldSet, even though that is the one function every block
	// change goes through: worldgenColumn goes through it too, so marking there would make
	// every column in the world dirty the instant it was generated and turn walking around
	// into a continuous stream of SD writes of terrain that the seed already reproduces
	// exactly. The flag means "the disk does not know about this", so it is raised at the
	// edit site (scene/interact.c) and nowhere else.
	bool dirty;

	// v1.5.0 adaptive lighting: the column's sky/block channels, owned end-to-end by
	// world/light.c (attach/copy/detach) and opaque here. NULL whenever the lighting
	// engine is off, so this field costs one pointer and nothing else on those paths.
	//
	// v1.7.1: that used to read "which is every Old 3DS build and every host test by
	// default", and the Old 3DS half has been wrong since v1.8.0. scene/chunk_render.c
	// now calls lightEngineInit(true) unconditionally (chunk_render.c:750) on both
	// consoles, because v1.8.0 moved the edit path off the relaxation sweeps and onto the
	// flood fill and measured it at 0.141 ms per block against the old 4.064 ms. Host
	// tests still leave it off by default — they never call chunkRenderInit, which owns
	// the shader program's lifetime and is where the gate is published.
	void* light;
} Column;

typedef struct {
	Column* slots[WORLD_MAP_SLOTS];
	int     columns;       // live columns
	int     chunks;        // live (allocated) chunks
} World;

void worldInit(World* w);
void worldExit(World* w);

// Lookup only — NULL when the column or chunk is not loaded.
Column* worldColumn(const World* w, int cx, int cz);
Chunk*  worldChunk(const World* w, int cx, int cy, int cz);

// Allocate on demand. NULL means the budget refused it or the column table is
// full; both are real conditions the caller must handle, not assertions.
Column* worldColumnCreate(World* w, int cx, int cz);
Chunk*  worldChunkCreate(World* w, int cx, int cy, int cz);

// Frees a column and every chunk in it, returning the memory to the budget. False when
// the column was not loaded, which is not an error — the streaming ring asks for columns
// it may already have dropped.
//
// ⚠ Whatever was in the column is gone. There is no save format until step 8.1, so a
// column that unloads and comes back is regenerated from the seed and any blocks the
// player put there are lost. That is the known state of the game, not a bug in here.
bool worldColumnRemove(World* w, int cx, int cz);

// Step 8.1. Flags the column containing world block (x, z) as needing a save. Silently does
// nothing when that column is not loaded, which is the honest answer: there is nothing in
// memory to write, so there is nothing the disk is missing.
void worldMarkDirty(World* w, int x, int z);

// World block coordinates. worldGet never fails: unloaded is air, below the floor
// is WORLD_FLOOR_BLOCK, above the ceiling is air.
BlockId worldGet(const World* w, int x, int y, int z);

// Writes a block, allocating the chunk if needed. False if y is outside the world
// or the allocation was refused. Writing air into an unloaded chunk is a no-op and
// reports success — there is nothing to store.
bool worldSet(World* w, int x, int y, int z, BlockId id);

// v1.8.0 task 22. Called after worldSet has changed a cell's id, with the id that was there
// before. NOT called when the write stored the value the cell already held, and NOT called by
// worldSetChunkAll — a generated or loaded chunk is not an edit, and firing 4096 times per
// streamed chunk is the difference between a hook and a stall.
//
// One hook, installed once by main.c, so that a player break, a player place, a remote edit
// arriving over the wire and the BS_EDIT_STRESS harness all reach the water simulation without
// four call sites having to remember to. scene/interact.c is deliberately not touched: it
// already calls worldSet, which is the single funnel every one of those paths goes through.
typedef void (*WorldEditFn)(void* ud, int x, int y, int z, BlockId prev, BlockId now);

// NULL to remove. There is exactly one slot: a second subscriber would need a list, and the
// only caller is main.c.
void worldSetEditHook(WorldEditFn fn, void* ud);

// Step 9.2a/9.1b. Replaces a whole chunk's content in one shot, choosing whichever of
// chunk.h's three storage forms fits `in` — this is what worldgen.c and (via
// chunk_codec.c) a save load use instead of worldChunkCreate followed by CHUNK_BLOCKS
// individual worldSet calls, so a freshly generated or loaded chunk is never promoted
// cell by cell on its own construction. Allocates the column and chunk if they do not
// exist yet. False if y is outside the world, the column table is full, or the budget
// refused the bytes the chosen form needs; on a refusal nothing already loaded is
// disturbed — a chunk that existed before the call still holds what it held before.
bool worldSetChunkAll(World* w, int cx, int cy, int cz, const BlockId in[CHUNK_BLOCKS]);

// v1.7.1 task 46. Where a player's feet can stand at (x, z), starting from `base` and stepping
// UP while the body would be inside something solid. Returns `base` unchanged when nothing is
// in the way, and never walks past the top of the world.
//
// `base` is the surface the generator predicts — worldgenHeight(), whose convention this shares
// exactly: the y of the first air block, so the top solid block is at y-1. That prediction is a
// pure function of the seed and knows nothing about what a player has built since, which is the
// bug this exists for. Spawn was placed from the prediction alone, so reloading a world with
// anything built at the spawn cell put the player INSIDE their own blocks — unable to move,
// because the physics resolvers snap back on a blocked move, and looking at back-faces, which
// reads exactly like the chunk they are standing in having failed to load.
//
// Stepping up from `base` rather than scanning down from the sky is deliberate — see the note
// at the call site in main.c. Two blocks of clearance, because PLAYER_HEIGHT is 1.8 and a body
// with its feet at integer y occupies y .. y+1.8.
int worldStandingY(const World* w, int x, int z, int base);

// Bytes of block data currently held, for the budget report. Walks every live chunk and
// sums chunkGetBytes(c) rather than assuming a fixed per-chunk cost, because since step
// 9.2a a chunk's size depends on its storage form — this is now an O(chunks) call, not
// O(1), which is fine: the only caller is the bottom-screen report, once a frame.
size_t worldBytes(const World* w);
