// Adaptive lighting engine (v1.5.0): per-column sky/block light channels.
//
// Two nibble channels per column — sky and block — over the full 16x16x128 cells,
// 4 KiB each, hanging off the owning Column as an opaque pointer. The engine is
// gated behind a global flag that defaults to off: an Old 3DS never allocates
// light data, never runs propagation, and its mesher writes pad = 0 exactly as
// before, so today's baked vertex-colour lighting comes out byte-for-byte
// identical. A New 3DS turns the gate on at boot and every stage below goes live.
//
// The channels are Minecraft-shaped: sky light is seeded straight down through
// air at 15 and spreads outward with -1 falloff; block light is seeded from
// luminous blocks. No luminous block exists in the registry yet, so the block
// channel is always zero in production — the machinery is real and tested, and a
// torch later is one row in the luminance table. Light stops at opaque cells
// (solid and not transparent, the same predicate the mesher's occlusion table
// uses), so air and leaves pass it and stone does not.
//
// Propagation scope is deliberately column-local in v1: a flood fill walks the
// owning column's own cells and treats everything outside as opaque. Cross-
// border spread (a cave mouth lit from the neighbouring column's opening) is the
// known gap; nothing here reads or writes another column's arrays, which is what
// keeps the worker-thread story simple — propagation touches only the staging
// world's own column, and an edit relights only the edited column.
//
// Two independent engines compute the same answer, and the host suite asserts
// they agree byte-for-byte on random fixtures:
//
//   * lightPropagateColumn — a breadth-first flood fill used on the generation
//     path. Seeds are assigned up front and a cell is enqueued only when its
//     stored value strictly improves, so the first assignment is final and the
//     queue can never hold more than one entry per cell.
//   * lightRelightColumn — fixed-order relaxation sweeps used on the edit path,
//     where determinism matters more than speed and no queue may be allocated on
//     the main thread. Each pass raises every cell towards its neighbours until
//     a full pass changes nothing.
//
// No <3ds.h>: this file is host-testable like the rest of source/world.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world/world.h"

// One channel of one column: 16*16*128 nibbles packed two to a byte along x,
// cell index in chunk order extended over height (y major, then z, then x).
#define LIGHT_COL_CELLS   (CHUNK_DIM * CHUNK_DIM * WORLD_HEIGHT)   // 32768
#define LIGHT_COL_BYTES   (LIGHT_COL_CELLS / 2)                    // 4096

static inline int lightIndex(int lx, int y, int lz)
{
	return (y * CHUNK_DIM + lz) * CHUNK_DIM + lx;
}

// Reads one nibble out of a channel array. Even cell indices own the low half of
// their byte, which keeps a contiguous run of x sharing bytes pairwise.
static inline uint8_t lightNibble(const uint8_t* chan, int ci)
{
	const uint8_t b = chan[ci >> 1];
	return (ci & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
}

// Breadth-first worklist for lightPropagateColumn. Caller-owned storage — the
// worker thread keeps one, the host tests keep theirs, nothing is allocated in
// here. LIGHT_QUEUE_CAP entries is the hard bound: a cell is enqueued only when
// its value strictly improves, and with seeds assigned up front that happens at
// most once per cell.
#define LIGHT_QUEUE_CAP  LIGHT_COL_CELLS

typedef struct {
	uint16_t slots[LIGHT_QUEUE_CAP];
	int      head;
	int      count;
} LightQueue;

void  lightQueueInit(LightQueue* q);

// The global gate. Defaults to off and stays off unless lightEngineInit(true)
// runs; every entry point below behaves exactly as it does today while off.
void  lightEngineInit(bool enable);
bool  lightEnabled(void);

// Attaches 8 KiB of zeroed light to a column (budget-claimed). Idempotent.
// False only if the budget refused or the allocator failed — the column then has
// no light and every consumer must treat that as "unlit", never as an error.
bool  lightColumnAttach(Column* col);

// Frees a column's light. Safe on NULL, on a detached column, always.
void  lightColumnDetach(Column* col);

// Copies src's light onto dst, attaching dst as needed. False when src has no
// light or dst could not be attached — the install path then leaves dst unlit,
// which degrades to today's look rather than to darkness.
bool  lightColumnCopy(const Column* src, Column* dst);

// Per-cell reads. An absent channel reads 0, so "no light" can never read as
// bright. Coordinates are column-local, y clamped like worldGet's domain.
uint8_t lightGetSky(const Column* col, int lx, int y, int lz);
uint8_t lightGetBlock(const Column* col, int lx, int y, int lz);

// Raw channel arrays, NULL when the column has no light. Consumers that walk
// whole columns (the mesh scratch fill) use these instead of per-cell getters.
const uint8_t* lightChannelSky(const Column* col);
const uint8_t* lightChannelBlock(const Column* col);

// Full recompute of one column from its blocks, BFS engine. Auto-attaches the
// column's light. False when the engine is off, the column is absent, the light
// could not be attached, or q rejected an entry (counted, never expected).
bool  lightPropagateColumn(World* w, int cx, int cz, LightQueue* q);

// Full recompute of one column from its blocks, sweep engine. Same result as
// lightPropagateColumn by construction-and-test, no queue needed. Edit path:
// called after worldSet, before the remesh, so remeshed vertices bake the new
// light in the same frame.
bool  lightRelightColumn(World* w, int cx, int cz);

int    lightColumnsAttached(void);
size_t lightBytesUsed(void);   // budget bytes currently held by attached columns

// Test hooks. The luminance table is all zeros in production — no block emits —
// and these exist so the suite can prove the block channel end-to-end without
// inventing a gameplay feature.
void  lightSetLuminanceForTest(BlockId id, uint8_t level);
void  lightSetSkyForTest(Column* col, int lx, int y, int lz, uint8_t level);
