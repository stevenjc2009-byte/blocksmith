// Adaptive lighting engine (v1.5.0): per-column sky/block light channels.
//
// Two nibble channels per column — sky and block — over the full 16x16x128 cells,
// 16 KiB each, hanging off the owning Column as an opaque pointer. The engine is
// gated behind a global flag that defaults to off; nothing allocates light data,
// runs propagation, or writes anything but pad = 0 until lightEngineInit(true).
//
// v1.8.0 task 24: BOTH console models now turn that gate on at boot. From v1.5.0
// to v1.7.0 only a New 3DS did, so an Old 3DS had baked face shade and AO but no
// light LEVEL — a sealed cave was as bright as open ground. The split was always
// about CPU and memory rather than the GPU, and both were measured before it went:
// 1.53 MB of light at radius 3 (12.8% of WORLD_BUDGET_BYTES) against 295,646 B of
// measured block storage, and an edit-path relight cut from 4.001 ms to 0.141 ms
// by the engine swap below. The gate itself stays, because the host suite and the
// dedicated server both run with it off.
//
// The channels are Minecraft-shaped: sky light is seeded straight down through
// air at 15 and spreads outward with -1 falloff; block light is seeded from
// luminous blocks.
//
// v1.8.2: what a block emits is BlockDef.luminance, read out of the registry — the
// engine's luminance table is a cache of it, refreshed once per column propagate or
// relight. Before that it was a private table nothing ever filled, so a luminance that
// had crossed the wire in a DEFS record (registry.c packs and unpacks byte 24) was
// silently dropped and the block rendered pitch dark. No CORE row declares a luminance,
// so a single-player world's block channel is still always zero and the block-light pass
// is still skipped outright; what changed is that a server-registered glowing block now
// actually glows. Light stops at opaque cells
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
// they agree byte-for-byte:
//
//   * lightPropagateColumn — a breadth-first flood fill. Seeds are assigned up
//     front and a cell is enqueued only when its stored value strictly improves,
//     so the first assignment is final and the queue can never hold more than one
//     entry per cell. Since v1.8.0 this is what BOTH the generation path and the
//     edit path run.
//   * lightRelightColumnSweeps — fixed-order relaxation sweeps, reaching the same
//     fixpoint by pulling where the flood fill pushes. Each pass raises every cell
//     towards its neighbours until a full pass changes nothing. It shares nothing
//     with the flood fill but opaqueAt and the height map, which is what makes
//     diffing the two a real check rather than a restatement.
//
// v1.8.0 note, because the claim above was false for three releases and the test that was
// meant to prove it could not fail: the two DID disagree, on every transparent block sitting
// at the top of its strip — leaves, water, tall grass. heightMapFill records the highest
// non-air cell while opaqueAt is solid && !transparent, so such a cell was below the flood
// fill's sky seeding and still passed light for the sweeps. A generated lake read 0 on all
// 128 of its cells from the flood fill and 13-14 from the sweeps. Fixed by seeding downward
// as well as sideways; see the seeding loop in light.c and testLightTransparentTopStrip.
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
#define LIGHT_COL_BYTES   (LIGHT_COL_CELLS / 2)                    // 16384

// v1.8.0: the two figures above read 32768 and 4096 from v1.5.0 to v1.7.0. The cell
// count was right and the byte count was a quarter of the truth — 32768/2 is 16384, not
// 4096 — and the same 4x understatement was repeated in the prose here, in light.c's
// LightColumn comment and in lightColumnAttach's doc below. The CODE was always correct
// (every allocation, budget claim, release and memcpy uses sizeof(LightColumn)), so
// nothing was mis-allocated; what was wrong was the number anyone reasoning about the
// world budget would have read. Measured on the host: sizeof(LightColumn) is 32768 = 32
// KiB, and at RENDER_DIST_MAX_COLUMNS (49) that is 1,605,632 B = 1.53 MB, 12.8% of
// WORLD_BUDGET_BYTES — against the 0.38 MB the old comments implied.
_Static_assert(LIGHT_COL_BYTES == 16384, "light column channel size drifted from its comment");

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

// Attaches 32 KiB of zeroed light to a column (budget-claimed). Idempotent.
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

// Full recompute of one column from its blocks, no queue needed from the caller. Edit path:
// called after worldSet, before the remesh, so remeshed vertices bake the new light in the same
// frame.
//
// v1.8.0: this runs the flood fill over a queue the engine owns. It used to run the sweeps
// below, which measured 4.064 ms against the flood fill's 0.156 ms on one real generated column
// — 26x, on every block broken or placed, on both consoles. See light.c's s_edit_queue.
bool  lightRelightColumn(World* w, int cx, int cz);

// The sweep engine, direct. Kept as the INDEPENDENT SECOND IMPLEMENTATION rather than as a code
// path anything ships on: it reaches the same fixpoint by pulling where the flood fill pushes,
// with no shared code beyond opaqueAt and the height map, so the host suite diffing the two is a
// real check on both. Nothing outside the tests should call this — lightRelightColumn is the
// edit path, and it only falls back here if the engine could not take its queue.
bool  lightRelightColumnSweeps(World* w, int cx, int cz);

int    lightColumnsAttached(void);
size_t lightBytesUsed(void);   // budget bytes currently held by attached columns

// v1.8.3. Whether lightEngineInit(true) actually got the edit queue it asked for, and how many
// relights have run the 26x slower sweeps because it did not.
//
// lightEngineInit ignored its malloc's result until v1.8.3: a refusal left the engine ENABLED and
// queueless, every lightRelightColumn for the rest of the session silently took the sweep path,
// and nothing recorded it. The fallback stays — sweeps beat darkness — but it now announces
// itself, the same way world/relightq.h's overflows counter and world/dirtyq.h's peak do. See
// light.c's s_edit_queue_refused for why this shape and not a log line.
//
// lightFastEngineReady() is false from the moment the refusal happens, before any relight has run;
// lightSweepFallbacks() is what says the refusal has actually cost frame time. Both are cheap
// enough to read every frame.
bool lightFastEngineReady(void);
int  lightSweepFallbacks(void);

// v1.8.10. How many times the cross-column block-light handoff hit its recursion
// depth cap (light.c's LIGHT_HANDOFF_MAX_DEPTH) rather than stopping on its own
// because a pushed value no longer improved the next column. Expected to stay 0 in
// every real fixture -- see light.c's lightHandoffBorders for the two independent
// arguments that bound it well under the cap -- and exists so a deliberately built
// long chain (tests/light_seam_test.c's testHandoffDepthCap) can prove the cap is
// reachable rather than dead code, the same way lightSweepFallbacks proves the CAS
// fallback above is reachable.
int  lightHandoffCapped(void);
void lightResetHandoffCappedForTest(void);

// Overrides light.c's s_handoff_max_depth so a test can drive the handoff into its cap
// with a small fixture instead of the 30-column chain the real 32-deep bound needs — the
// same idiom lightFailEditQueueForTest below uses for the CAS/malloc-refusal fallback.
// Call lightResetHandoffMaxDepthForTest() when done; nothing else restores it.
void lightSetHandoffMaxDepthForTest(int depth);
void lightResetHandoffMaxDepthForTest(void);

// Test hooks. No block in the CORE registry declares a luminance — nothing emits in a
// single-player world — and these exist so the suite can prove the block channel
// end-to-end without inventing a gameplay feature.
//
// lightSetLuminanceForTest sets an OVERRIDE laid over whatever the registry declares.
// Level 0 clears the override (the block falls back to its registry luminance, which for
// every core row is 0); it does not force a declared emitter dark.
void  lightSetLuminanceForTest(BlockId id, uint8_t level);
void  lightSetSkyForTest(Column* col, int lx, int y, int lz, uint8_t level);

// Forces the NEXT lightEngineInit(true) to behave exactly as a refused 65,544-byte malloc does.
// Call lightEngineInit(false) first: an edit queue already held is not thrown away by this, so
// without the release the next init still has one and the hook appears to do nothing.
void  lightFailEditQueueForTest(bool fail);
void  lightResetSweepFallbacksForTest(void);
