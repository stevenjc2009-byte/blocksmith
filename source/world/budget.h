// The world memory budget, asserted at runtime.
//
// A silent over-allocation on this hardware surfaces later as a random crash in unrelated
// code, so every world allocation asks first and a refusal is counted and reported rather
// than swallowed.
//
// This is a module-level singleton on purpose: there is one world, and the bottom-screen
// report needs the figures without a pointer threaded through everything that allocates.
//
// ── WHICH HEAP THIS IS, because it is the first thing anyone gets wrong ────────────────────
//
// The application heap, via calloc/malloc — NOT the linear heap. world.c:92 callocs a Column,
// chunk.c:53 mallocs a Chunk, light.c mallocs a LightColumn, and every one of those goes
// through the C allocator. The chunk mesh pool is the linearAlloc one (scene/chunk_render.c),
// and app/heapsplit.h is the file that explains why the two are worth keeping straight: on a
// New 3DS the split leaves 64 MB of linear heap and a MEASURED 59,715,584 bytes of application
// heap, so the render distance competes for linear while the world store sits on the other
// side of the fence and does not.
//
// The cap below is therefore not a reservation of anything. budget.c holds three scalars —
// used, peak, refusals — and compares. Setting it to 12 MB does not cost an Old 3DS 12 MB, or
// one byte; it only decides when budgetClaim() says no.
//
// ── WHERE 12 MB COMES FROM ────────────────────────────────────────────────────────────────
//
// This used to read "the 17x17-column worst case is ~9.25 MB", which was 289 columns times
// 8 chunks times 4,096 bytes of block data. That was true in v1.4.x and has been a roughly
// 2x understatement since v1.5.0 added lighting: it counted neither the per-chunk header nor
// the 32 KiB LightColumn that hangs off every loaded column, and since v1.8.0 task 24 the
// lighting engine is on for BOTH console models (scene/chunk_render.c calls lightEngineInit
// (true) unconditionally), so nothing loads a column without it any more.
//
// The real per-column arithmetic ceiling, measured off the compiler for the actual target
// ABI rather than derived on paper (arm-none-eabi-gcc 16.1.0, -march=armv6k, the devkitARM
// flags the console build uses):
//
//     sizeof(Column)                            48 B      (8 chunk pointers, 4 B each)
//     COLUMN_CHUNKS x chunkFormBytes(RAW)   32,832 B      (8 x (8 B header + 4,096 B cells))
//     sizeof(LightColumn)                   32,768 B      (2 x LIGHT_COL_BYTES, sky and block)
//                                          --------
//                                           65,648 B per column, every chunk RAW, light on
//
// The header is 8 bytes and not the 12 a host x86-64 build would suggest, because the ARM
// EABI defaults to -fshort-enums and ChunkForm collapses to one byte. That is exactly the
// kind of thing paper arithmetic gets wrong, which is why tests/world_budget_bytes_test.c
// carries the number and the host suite checks it can still be reached.
//
// Against that, what each console's render-distance ceiling actually needs. The loaded ring
// is one column wider than the drawn one (main.c's s_area_radius = radius + 1), and the
// worker's staging world holds at most one more (app/worker.c installs then empties it):
//
//     radius 3 (RENDER_DIST_MAX_OLD)    9x9  =  81 cols + 1 staging =  5,383,136 B   42.8 %
//     radius 5 (RENDER_DIST_MAX_NEW)   13x13 = 169 cols + 1 staging = 11,160,160 B   88.7 %
//     radius 6                         15x15 = 225 cols + 1 staging = 14,836,448 B  over
//
// So 12 MB is not headroom over the Old 3DS case that happens to survive the New 3DS one: it
// is very nearly exactly the largest radius this constant can hold, and radius 6 is excluded
// by 2,253,536 bytes. tests/world_budget_bytes_test.c asserts BOTH halves — that radius 5
// fits and that radius 6 does not — so the day a field is added to Column, or a chunk form
// grows, or lighting gains a third channel, the ceiling fails the host suite instead of
// failing as a hole in somebody's terrain.
//
// NOT made per-console, deliberately, and this is the v1.8.5 conclusion rather than an
// omission. A per-console cap would save nothing, because the cap allocates nothing; and
// lowering the Old 3DS cap to its own 5,383,136 B ceiling would replace a 2.34x margin with a
// 1.00x fit, on the one failure mode this file exists to prevent. The Old 3DS is charged the
// same number as the New one because that number costs it nothing — see budgetColumnBytes()
// below for the formula both consoles are checked against.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 12 MB. See the derivation above: it holds radius 5 at 88.7 % and refuses radius 6.
#define WORLD_BUDGET_BYTES  (12u * 1024u * 1024u)

// The worker's staging world (app/worker.c's s_staging) holds one generated column at a time
// and is emptied by workerInstall, so it adds exactly one column to whatever the ring costs.
#define BUDGET_STAGING_COLUMNS  1

// ── The sizing, as functions rather than as a paragraph ───────────────────────────────────
//
// Pure arithmetic over values the caller supplies, so this header stays free of world.h,
// chunk.h and light.h — budget.h is included by a dozen translation units and by thirteen
// host-test link lines, and none of them should gain a dependency to hold a formula. The
// sizes themselves are opaque types (struct Chunk and LightColumn are defined only in their
// own .c files), so they cannot be taken here even if the includes were free; the test that
// gates this carries them, measured, with their provenance written down.

// Columns loaded at a render radius. The GENERATED ring, which is one wider than the drawn
// one — main.c sets s_area_radius = radius + 1 and genRecenter/genSetRadius both drop before
// they request, so the loaded set never exceeds this square.
static inline int budgetColumnsForRadius(int radius)
{
	const int area = radius + 1;
	return (2 * area + 1) * (2 * area + 1);
}

// The arithmetic ceiling for one loaded column: the Column struct, every one of its chunks in
// its largest form, and a light column. Deliberately the ceiling and not a measurement — the
// same reasoning scene/render_dist.h's RENDER_DIST_SLOTS_PER_COLUMN applies to the mesh pool,
// where a measured per-column figure went under when the generator changed and cost a release.
static inline size_t budgetColumnBytes(size_t column_struct_bytes,
                                       int    column_chunks,
                                       size_t chunk_max_form_bytes,
                                       size_t light_column_bytes)
{
	return column_struct_bytes
	     + (size_t)column_chunks * chunk_max_form_bytes
	     + light_column_bytes;
}

// What the world store can cost at a radius, worst case, including the staging column.
static inline size_t budgetBytesForRadius(int radius, size_t column_bytes)
{
	return ((size_t)budgetColumnsForRadius(radius) + BUDGET_STAGING_COLUMNS) * column_bytes;
}

// True if the claim fits. On refusal nothing is reserved, the refusal is counted, and a line
// is printed — a budget that fails quietly is worse than no budget.
bool   budgetClaim(size_t bytes);
void   budgetRelease(size_t bytes);

size_t budgetUsed(void);
size_t budgetPeak(void);
size_t budgetCap(void);
int    budgetRefusals(void);

// Test hook: forget everything. Only for the self-test, which needs a clean slate.
void   budgetReset(void);
