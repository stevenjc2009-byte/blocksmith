#include "world/scratch.h"

#include <string.h>

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
