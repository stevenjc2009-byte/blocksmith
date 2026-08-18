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

// World block coordinates. worldGet never fails: unloaded is air, below the floor
// is WORLD_FLOOR_BLOCK, above the ceiling is air.
BlockId worldGet(const World* w, int x, int y, int z);

// Writes a block, allocating the chunk if needed. False if y is outside the world
// or the allocation was refused. Writing air into an unloaded chunk is a no-op and
// reports success — there is nothing to store.
bool worldSet(World* w, int x, int y, int z, BlockId id);

// Bytes of block data currently held, for the budget report.
size_t worldBytes(const World* w);
