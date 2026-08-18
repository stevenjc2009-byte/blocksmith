#include "world/remesh.h"

// One axis: which neighbour offsets read this local coordinate. Only the two extreme
// local coordinates are visible from outside, which is why an edit in the middle of a
// chunk costs one remesh and an edit on a corner costs eight.
static int axisOffsets(int local, int out[2])
{
	out[0] = 0;
	if (local == 0)              { out[1] = -1; return 2; }
	if (local == CHUNK_DIM - 1)  { out[1] =  1; return 2; }
	return 1;
}

int remeshList(int x, int y, int z, ChunkCoord out[REMESH_MAX])
{
	if (y < 0 || y >= WORLD_HEIGHT) return 0;

	const int cx = x >> 4, cy = y >> 4, cz = z >> 4;

	int ox[2], oy[2], oz[2];
	const int nx = axisOffsets(x & 15, ox);
	const int ny = axisOffsets(y & 15, oy);
	const int nz = axisOffsets(z & 15, oz);

	int n = 0;
	for (int i = 0; i < nx; i++)
		for (int j = 0; j < ny; j++)
			for (int k = 0; k < nz; k++) {
				const int ncy = cy + oy[j];
				if (ncy < 0 || ncy >= COLUMN_CHUNKS) continue;   // outside the world

				out[n].cx = cx + ox[i];
				out[n].cy = ncy;
				out[n].cz = cz + oz[k];
				n++;
			}

	return n;
}
