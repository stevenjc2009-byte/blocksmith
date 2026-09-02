// The padded meshing scratch: 18 x 18 x 18 blocks.
//
// One chunk (16³) plus a one-block border on every side, filled from **all 26
// neighbouring chunks**, not just the six face-adjacent ones. That is the whole
// point of this file, and it is here in Phase 2 rather than Phase 3 for one
// documented reason: ambient occlusion reads a vertex's diagonal neighbours, so a
// mesher built against six neighbours has to have its border handling rewritten
// the moment AO arrives. Every voxel tutorial that skips this pays for it later.
//
// 18³ is 5,832 bytes — one scratch is reused for every chunk meshed, so the cost
// is a rounding error against the block data itself.
//
// v1.5.0 adds a parallel light band, one packed byte per cell (sky in the high
// nibble, block in the low), filled by scratchFillLight from the per-column
// light store. It is only read when the lighting engine is enabled; when it is
// off the mesher never looks at it and vertices come out exactly as before.
#pragma once

#include "world/world.h"

#define SCRATCH_DIM     (CHUNK_DIM + 2)                                  // 18
#define SCRATCH_BLOCKS  (SCRATCH_DIM * SCRATCH_DIM * SCRATCH_DIM)        // 5832

// v1.8.0 task 22b. How the water band below encodes a cell:
//
//   0     a full cube — a water SOURCE, or a cell that holds no water at all.
//   1..7  a flow level, exactly the 1..7 world/water.h's side map stores.
//
// The mesher turns level L into a top surface L/8 of a block high, so 0 reads as 8/8 and is
// what every block that is not flowing water gets. That is why absence is 0 on both sides:
// world/water.h's map already means "no entry == source == full", so a memset(0) band and an
// unfilled band say the same true thing, and a build with no water simulation in the link
// (every host suite but the water one) renders exactly what it rendered before this task.
#define SCRATCH_WATER_STEPS  8

// v1.8.2. How far the top of an EXPOSED water surface is pulled below the cell boundary, in
// the same eighths SCRATCH_WATER_STEPS counts. Every water cell with something other than
// water above it is drawn at least this short, source or not, so the sea reads as a surface
// sitting inside its cell rather than as a wall of blue cubes flush with the grass beside it.
//
// 1, i.e. 7/8 = 87.5%. That is Minecraft's own number and it is the nearest value the eighths
// ladder can express to the "about ninety percent" this was asked for. The denominator is NOT
// free to change: world/water.c static-asserts SCRATCH_WATER_STEPS == WATER_LEVEL_SOURCE, so
// re-basing it to tenths would change what a flow LEVEL means to the simulation. Faking an
// exact 90% by writing row 8's offset as -0.10f in scene/chunk_render.c was rejected for a
// different reason: it makes the ladder uneven (90%, 75%, 62.5% — a 15% first step and 12.5%
// after it), so a waterfall's steps would no longer be equal.
//
// A cell with water directly ABOVE it is untouched by this and stays full height — see
// dropBuild in world/mesher.c, which is what keeps a falling column one solid shaft.
#define WATER_SURFACE_DROP   1

typedef struct {
	BlockId blocks[SCRATCH_BLOCKS];
	uint8_t light[SCRATCH_BLOCKS];   // sky<<4 | block; see scratchFillLight
	uint8_t water[SCRATCH_BLOCKS];   // 0 = full cube, 1..7 = flow level; see waterFillScratch

	// Whether `water` holds anything at all. Cleared by scratchFill and set only by
	// waterFillScratch, so a caller that never fills the band — the host suites, and the
	// console before the simulation is wired in — costs one branch in the mesher and pays
	// nothing else. It is NOT "there is water in this chunk": a lake of sources leaves it
	// false, because every one of those cells is a full cube already.
	bool    water_any;

	// Whether any cell of `blocks` is BLOCK_WATER at all — sources included. Set by
	// scratchFill, which is the one place every cell of the scratch is written.
	//
	// water_any above cannot answer this and never could: it means "the band holds a FLOW
	// level", and an ocean of sources holds none, so before v1.8.2 a pure sea never entered
	// dropBuild's loop and could not be shortened. The alternative — deleting dropBuild's
	// early return — would pay a 5,508-cell scan on every chunk in the world, and about 95%
	// of them hold no water whatever.
	bool    has_water;

	// v1.8.8 biome tint. One palette index (0..7, 0 meaning untinted — MESH_TINT_NONE in
	// world/mesher.h) per COLUMN of the scratch, indexed by scratchColumn(sx, sz).
	//
	// Per column and not per cell because a biome is a function of x and z and of nothing
	// else — worldgenBiomeAt(g, x, z) in world/worldgen.h takes no y and never has. A
	// per-cell band would be 5,832 bytes to hold 324 distinct values; this is 324.
	//
	// The band is 18x18, so it covers the one-block border as well as the chunk. That is
	// load-bearing rather than incidental: a merge run can start on the last column of a
	// chunk, and the mesher tints a face from the column the FACE sits in, so a border left
	// at zero would draw an untinted line down every chunk seam inside a biome.
	uint8_t tint[SCRATCH_DIM * SCRATCH_DIM];

	// Whether `tint` holds anything but zeros. Same shape and same purpose as water_any: the
	// band is cleared by scratchFill, and a caller that never fills it (every host suite but
	// the biome-tint one, and the console until a WorldGen reaches the render path) leaves
	// this false, so the mesher skips the lookup entirely and its vertices come out
	// byte-for-byte what they were before v1.8.8.
	bool    tint_any;
} MeshScratch;

// sx/sy/sz are 0..17 — scratch space, where 0 is the border and 1..16 is the chunk.
static inline int scratchIndex(int sx, int sy, int sz)
{
	return (sy * SCRATCH_DIM + sz) * SCRATCH_DIM + sx;
}

// Where column (sx, sz) lands in the tint band. sx/sz are 0..17, the same scratch space
// scratchIndex takes, and the order is the same (z, x) row-major one scratchIndex uses for its
// inner two axes so the two walk memory the same way.
static inline int scratchColumn(int sx, int sz)
{
	return sz * SCRATCH_DIM + sx;
}

// Copies chunk (cx,cy,cz) and its 26 neighbours into the scratch. Absent or
// unloaded neighbours read as air; anything below the world floor reads as
// WORLD_FLOOR_BLOCK, matching worldGet so the two can never disagree.
void scratchFill(MeshScratch* s, const World* w, int cx, int cy, int cz);

// Fills the light band for the same 27-chunk neighbourhood, reading each cell's
// owning column's channels. Call only while the lighting engine is enabled; a
// column with no light yet (still streaming in) reads as full sky, which keeps
// borders at today's brightness until its own propagation lands instead of
// painting black seams at the edge of the loaded ring.
void scratchFillLight(MeshScratch* s, const World* w, int cx, int cy, int cz);

// The water band is filled by waterFillScratch (world/water.h), which lives with the
// simulation because the flow levels do. It is declared there and not here so that scratch.c
// keeps its current link footprint: every host suite links this file, only one links
// world/water.c, and a call from here would drag the simulation into all of them.

// v1.8.8 biome tint. Fills the 18x18 tint band for the chunk column at (cx, cz), asking `fn`
// for the palette row of every column the band covers -- the one-block border included.
//
// **A CALLBACK and not a `const WorldGen*`, and that signature is the whole design decision.**
// The biome lives in world/worldgen.c, and 10 of the 15 host binaries that link THIS file do
// not link that one (measured, not assumed: networld, inv_bridge, interop, scratch_light,
// water_mesh, mesher_hashcheck, light_luminance, relight_drain, light_race, biome_tint).
// Calling worldgenBiomeAt() from here would pull worldgen.c -- and noise.c, and rng.c -- into
// every one of them, which is precisely what the water band's note above refuses to do for
// precisely the same reason. A function pointer keeps this file's link footprint EXACTLY what
// it was, while keeping the part that is actually easy to get wrong -- the scratch-space to
// world-space walk, where sx runs 0..17 and only 1..16 is the chunk -- inside a file that all
// 15 of those binaries already compile, and can therefore test.
//
// `fn` is handed WORLD block coordinates and returns a palette row. Whatever it returns is
// masked to MESH_TINT_MASK here, so a miscounting callback can never spill into the ao byte's
// low two bits and read as an occlusion of up to 31 (see world/mesher.h).
//
// tint_any is set iff at least one column came back non-zero, so a callback that tints nothing
// leaves the mesher on exactly its pre-v1.8.8 path and every pinned hash where it was.
//
// A NULL `fn` is a no-op. Call this AFTER scratchFill, which clears the band -- calling it
// before would have the clear silently undo the fill.
typedef uint8_t (*ScratchTintFn)(const void* ctx, int32_t wx, int32_t wz);

void scratchFillTint(MeshScratch* s, int cx, int cz, ScratchTintFn fn, const void* ctx);

// Reads in *chunk-local* coordinates, which run -1..16: the mesher works in the
// chunk's own frame and stepping off the edge is normal, not an error.
static inline BlockId scratchAt(const MeshScratch* s, int lx, int ly, int lz)
{
	return s->blocks[scratchIndex(lx + 1, ly + 1, lz + 1)];
}
