#include "world/scratch.h"

#include <string.h>

#include "world/light.h"
#include "world/mesher.h"   // MESH_TINT_MASK / MESH_TINT_NONE only -- no link edge added

// Where one neighbour offset lands in the scratch, per axis.
//
// A neighbour at offset -1 contributes exactly one plane — the scratch's border at
// 0, taken from the neighbour's last row (local 15). Offset 0 contributes the
// sixteen middle planes, and +1 contributes the far border at 17 from the
// neighbour's first row (local 0). Same shape on all three axes, which is why this
// is one helper rather than a nest of special cases.
typedef struct {
	int scratch_begin;
	int count;
	int local_begin;
} AxisSpan;

static AxisSpan axisSpan(int d)
{
	if (d < 0)  return (AxisSpan){ 0,             1,         CHUNK_DIM - 1 };
	if (d == 0) return (AxisSpan){ 1,             CHUNK_DIM, 0 };
	return             (AxisSpan){ CHUNK_DIM + 1, 1,         0 };
}

void scratchFill(MeshScratch* s, const World* w, int cx, int cy, int cz)
{
	// v1.8.0 task 22b. The water band starts empty on every fill, and is NOT memset here:
	// waterFillScratch clears it only when it has something to write, so a chunk with no flow
	// cells near it costs one byte store instead of 5,832. A caller that never fills the band
	// therefore meshes exactly what it meshed before this task — see SCRATCH_WATER_STEPS.
	s->water_any = false;

	// v1.8.2. Whether this neighbourhood holds ANY water, sources included — the gate that
	// lets world/mesher.c's dropBuild shorten an ocean surface without scanning 5,832 cells
	// on every waterless chunk in the world. See has_water in world/scratch.h for why
	// water_any cannot answer it.
	//
	// Answered here because this is the one function that writes every cell of `blocks`, and
	// it is answered per RUN rather than per cell: x is the contiguous axis, so a run is a
	// flat byte range and memchr is one call for up to sixteen cells. The `!s->has_water`
	// guard makes the whole sweep stop the moment the first water cell is seen, so the common
	// waterless chunk pays 972 memchr calls over 5,832 bytes and a water chunk usually pays
	// far fewer.
	s->has_water = false;

	// v1.8.8 biome tint. Unlike the water band this one IS cleared here, and the difference is
	// size: 324 bytes against water's 5,832, so the memset costs less than the bookkeeping
	// needed to avoid it. Clearing it makes "nobody filled the tint" and "everything is
	// untinted" the same state, which is what keeps every caller that predates this task
	// emitting exactly the vertices it emitted before.
	memset(s->tint, 0, sizeof s->tint);
	s->tint_any = false;

	for (int dy = -1; dy <= 1; dy++) {
		const AxisSpan ay = axisSpan(dy);
		const int ncy = cy + dy;

		// Outside the column reads as the same thing worldGet would return, so a
		// mesher can trust either source. Below the floor is solid, which is what
		// stops the bottom of the world being meshed as an exposed face.
		BlockId absent = BLOCK_AIR;
		if (ncy < 0) absent = WORLD_FLOOR_BLOCK;

		for (int dz = -1; dz <= 1; dz++) {
			const AxisSpan az = axisSpan(dz);

			for (int dx = -1; dx <= 1; dx++) {
				const AxisSpan ax = axisSpan(dx);
				const Chunk* c = worldChunk(w, cx + dx, ncy, cz + dz);

				for (int i = 0; i < ay.count; i++) {
					for (int j = 0; j < az.count; j++) {
						BlockId* dst = &s->blocks[scratchIndex(ax.scratch_begin,
						                                       ay.scratch_begin + i,
						                                       az.scratch_begin + j)];
						if (!c) {
							memset(dst, absent, (size_t)ax.count);
						} else {
							// x is the contiguous axis in both layouts, so a whole run is
							// still one call — chunkCopyRun is the form-aware bulk primitive
							// (memset for UNIFORM, an unpack loop for PALETTE4, memcpy for
							// RAW) that replaced the direct &c->blocks[...] read this loop
							// used before step 9.2a made Chunk opaque.
							chunkCopyRun(c, chunkIndex(ax.local_begin,
							                           ay.local_begin + i,
							                           az.local_begin + j),
							             ax.count, dst);
						}

						// See has_water above. After the write, never before it, and over
						// exactly the bytes that were just written.
						if (!s->has_water &&
						    memchr(dst, (BlockId)BLOCK_WATER, (size_t)ax.count))
							s->has_water = true;
					}
				}
			}
		}
	}
}

// Fills the light band for the same neighbourhood scratchFill walks. Light lives
// per column, so this is not a per-chunk copy: the 3x3 columns around (cx,cz)
// are resolved once and every cell then reads straight out of its owning
// column's two nibble channels. y stays inside those columns whatever cy does,
// which is why no chunk-level walk is needed here.
//
// v1.8.0 task 49h rewrote the walk without changing a byte of what it produces.
// The old shape asked every one of the 5,832 cells the same three questions the
// cell before it had already answered:
//
//   * lightChannelSky/lightChannelBlock are declared in world/light.h and defined
//     in world/light.c, and this project does not build with -flto, so they are
//     real out-of-line calls the compiler cannot hoist across the TU boundary —
//     11,664 of them per chunk build, of which 11,658 returned what the previous
//     call returned, because the owning column only changes at three values of sx.
//     They are now resolved 18 times, once per (dz,dx) neighbour, before the walk.
//   * `wy >= WORLD_HEIGHT` and `wy < 0` are constant across a whole sy plane, so
//     they are answered once per plane instead of 324 times inside it.
//   * lightIndex() rebuilt a multiply chain per cell for an index that simply
//     increments across the middle 16 cells of an sx run — x is the contiguous
//     axis in BOTH the scratch and the light channel, which is what makes the run
//     a run. Same shape as the emitCollect fix in world/mesher.c.
//
// This became live on an Old 3DS in v1.8.0 task 24, when the lighting gate went on
// for both models (see world/light.h) — so it is new cost on the streaming drain,
// on the hardware that has the least to spend.
// v1.8.8 biome tint. See world/scratch.h for why this takes a function pointer instead of the
// WorldGen it obviously wants -- short version: 10 of the 15 host binaries that link this file
// do not link world/worldgen.c, and this file's whole job is to stay linkable by all of them.
void scratchFillTint(MeshScratch* s, int cx, int cz, ScratchTintFn fn, const void* ctx)
{
	// A caller with no generator (every host suite that predates this, and the console before
	// main.c hands one over) leaves the band exactly as scratchFill cleared it: all zeros,
	// tint_any false, and the mesher never looks at it.
	if (!fn) return;

	bool any = false;

	// sz outer and sx inner because sx is the CONTIGUOUS axis of the band -- scratchColumn puts
	// it in the low term -- so one row is 18 sequential bytes. Same direction of travel as
	// scratchFill and scratchFillLight, for the same reason.
	for (int sz = 0; sz < SCRATCH_DIM; sz++) {
		const int32_t wz = (int32_t)cz * CHUNK_DIM + sz - 1;
		uint8_t* row = &s->tint[scratchColumn(0, sz)];

		for (int sx = 0; sx < SCRATCH_DIM; sx++) {
			// The -1 is the whole correctness question in this function. Scratch column 0 is
			// the BORDER -- one block before the chunk begins -- and 1..16 is the chunk. An
			// off-by-one here does not crash, drop a face or move a vertex: it draws every
			// chunk in the world one block of COLOUR out of step with its own terrain, which
			// is invisible to every hash and every face count in the tree.
			const int32_t wx = (int32_t)cx * CHUNK_DIM + sx - 1;

			// Masked at the boundary rather than trusted: the ao byte's low two bits are the
			// ambient occlusion, and both .pica files read that byte with no mask of their own.
			const uint8_t t = (uint8_t)(fn(ctx, wx, wz) & MESH_TINT_MASK);
			row[sx] = t;
			if (t != MESH_TINT_NONE) any = true;
		}
	}

	// Set from what was actually written, never from "a generator exists". A world sitting
	// entirely inside one untinted region must still report false here.
	s->tint_any = any;
}

void scratchFillLight(MeshScratch* s, const World* w, int cx, int cy, int cz)
{
	// One resolve per neighbour column, not one per cell. Indexed [dz+1][dx+1],
	// which is exactly the ci_z / ci_x pair the old per-cell arithmetic derived.
	const uint8_t* skych[3][3];
	const uint8_t* blkch[3][3];
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Column* c = worldColumn(w, cx + dx, cz + dz);
			skych[dz + 1][dx + 1] = lightChannelSky(c);
			blkch[dz + 1][dx + 1] = lightChannelBlock(c);
		}
	}

	// The three x runs of an 18-wide row, in scratch order: the low border cell
	// (owned by the -1 neighbour, whose local x is 15), the sixteen middle cells
	// (this column, local x 0..15), and the high border cell (the +1 neighbour,
	// local x 0). Derived once here rather than re-derived per cell.
	static const int kRunSx[3]    = { 0, 1, CHUNK_DIM + 1 };
	static const int kRunCount[3] = { 1, CHUNK_DIM, 1 };
	static const int kRunLx[3]    = { CHUNK_DIM - 1, 0, 0 };

	for (int sy = 0; sy < SCRATCH_DIM; sy++) {
		const int wy = cy * CHUNK_DIM + sy - 1;

		// Invariant across the whole plane, so it is decided here and not 324 times.
		const bool above = (wy >= WORLD_HEIGHT);
		const bool below = (wy < 0);

		for (int sz = 0; sz < SCRATCH_DIM; sz++) {
			const int wz   = cz * CHUNK_DIM + sz - 1;
			const int ci_z = (wz >> 4) - cz + 1;
			const int lz   = wz & (CHUNK_DIM - 1);

			uint8_t* row = &s->light[scratchIndex(0, sy, sz)];

			for (int r = 0; r < 3; r++) {
				uint8_t* dst        = row + kRunSx[r];
				const int count     = kRunCount[r];
				const uint8_t* sky  = skych[ci_z][r];

				// No light data yet: full sky, today's brightness — a column still
				// streaming in must not paint black seams at the ring's edge while
				// its own propagation is still in flight. Asked before the y range,
				// exactly as it was before.
				if (!sky || above) { memset(dst, 0xF0, (size_t)count); continue; }
				if (below)         { memset(dst, 0x00, (size_t)count); continue; }

				const uint8_t* blk = blkch[ci_z][r];
				int ci = lightIndex(kRunLx[r], wy, lz);

				for (int k = 0; k < count; k++, ci++) {
					const int sv = lightNibble(sky, ci);
					const int bv = blk ? lightNibble(blk, ci) : 0;
					dst[k] = (uint8_t)((sv << 4) | bv);
				}
			}
		}
	}
}
