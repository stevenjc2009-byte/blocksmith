#include "world/scratch.h"

#include <string.h>

#include "world/light.h"

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
void scratchFillLight(MeshScratch* s, const World* w, int cx, int cy, int cz)
{
	const Column* cols[3][3];
	for (int dz = -1; dz <= 1; dz++)
		for (int dx = -1; dx <= 1; dx++)
			cols[dz + 1][dx + 1] = worldColumn(w, cx + dx, cz + dz);

	for (int sy = 0; sy < SCRATCH_DIM; sy++) {
		const int wy = cy * CHUNK_DIM + sy - 1;

		for (int sz = 0; sz < SCRATCH_DIM; sz++) {
			const int wz = cz * CHUNK_DIM + sz - 1;
			const int ci_z = (wz >> 4) - cz + 1;

			for (int sx = 0; sx < SCRATCH_DIM; sx++) {
				const int wx = cx * CHUNK_DIM + sx - 1;
				const Column* c = cols[ci_z][(wx >> 4) - cx + 1];
				const uint8_t* sky = lightChannelSky(c);
				const uint8_t* blk = lightChannelBlock(c);

				uint8_t v;
				if (!sky) {
					// No light data yet: full sky, today's brightness — a column
					// still streaming in must not paint black seams at the ring's
					// edge while its own propagation is still in flight.
					v = 0xF0;
				} else {
					int sv, bv;
					if (wy >= WORLD_HEIGHT) { sv = 15; bv = 0; }
					else if (wy < 0)        { sv = 0;  bv = 0; }
					else {
						const int ci = lightIndex(wx & 15, wy, wz & 15);
						sv = lightNibble(sky, ci);
						bv = blk ? lightNibble(blk, ci) : 0;
					}
					v = (uint8_t)((sv << 4) | bv);
				}

				s->light[scratchIndex(sx, sy, sz)] = v;
			}
		}
	}
}
