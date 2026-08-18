#include "world/visgraph.h"

#include <math.h>
#include <string.h>

#include "world/block.h"

// The fill decomposes a linear chunk index with masks and shifts instead of / and %, because
// the ARM11 has no divide instruction and this runs up to 4,096 times per chunk build. That
// is only equivalent while the chunk is 16 on a side, so say so here rather than leave a
// silent wrong answer if CHUNK_DIM ever moves.
_Static_assert(CHUNK_DIM == 16, "visgraph.c decomposes chunk indices assuming CHUNK_DIM 16");

// See-through for the purposes of sight, which is not the same question the mesher asks.
// The mesher wants "does this hide the face behind it"; this wants "can I see past it".
//
// Built as a 256-entry table once per chunk rather than called per cell, and that is not
// micro-optimisation for its own sake — it is the difference the step was measured at. The
// fill asks this question about a cell and about each of its six neighbours, so a 16^3 chunk
// makes up to 28,672 of them; blockInfo() lives in another translation unit and there is no
// link-time optimisation in this build, so every one of those was a real function call. With
// the table the first measurement of 749 us per chunk fell to the number in the log.
//
// 256 entries, not BLOCK_COUNT, so an out-of-range id needs no branch — blockInfo() answers
// air for those, and air is open.
static void openTable(uint8_t out[256])
{
	const BlockInfo* air = blockInfo(BLOCK_AIR);
	const uint8_t    air_open = (uint8_t)!(air->solid && !air->transparent);

	for (int i = 0; i < 256; i++) out[i] = air_open;
	for (int i = 0; i < BLOCK_COUNT; i++) {
		const BlockInfo* info = blockInfo((BlockId)i);
		out[i] = (uint8_t)!(info->solid && !info->transparent);
	}
}

// Which of the six faces this local coordinate sits on, as a bit mask. A corner cell is on
// three of them at once, which is the whole reason this returns a mask and not a face.
static inline uint8_t faceBitsAt(int x, int y, int z)
{
	uint8_t b = 0;
	if (x == CHUNK_DIM - 1) b |= 1u << FACE_EAST;
	if (x == 0)             b |= 1u << FACE_WEST;
	if (y == CHUNK_DIM - 1) b |= 1u << FACE_TOP;
	if (y == 0)             b |= 1u << FACE_BOTTOM;
	if (z == CHUNK_DIM - 1) b |= 1u << FACE_SOUTH;
	if (z == 0)             b |= 1u << FACE_NORTH;
	return b;
}

// Every pair of distinct faces present in `bits`, as a connectivity mask.
static uint16_t pairsOf(uint8_t bits)
{
	uint16_t m = 0;
	for (int a = 0; a < BLOCK_FACES; a++) {
		if (!(bits & (1u << a))) continue;
		for (int b = a + 1; b < BLOCK_FACES; b++) {
			if (!(bits & (1u << b))) continue;
			m |= (uint16_t)(1u << visPairIndex(a, b));
		}
	}
	return m;
}

uint16_t visChunkConnectivity(const Chunk* c, VisScratch* sc)
{
	uint8_t open[256];
	openTable(open);

	memset(sc->label, 0, sizeof(sc->label));

	// Component 0 is "unvisited", so real components start at 1 and comp_faces is indexed
	// by the component number directly.
	uint16_t components = 0;
	memset(sc->comp_faces, 0, sizeof(sc->comp_faces));

	// chunkIndex is (y * 16 + z) * 16 + x, so a neighbour is a fixed offset on the linear
	// index and the local coordinates come back out with a mask and two shifts. Decomposing
	// with % and / and re-composing with chunkIndex, which is how this was first written,
	// costs three divisions per cell on an ARM11 that has no divide instruction at all.
	enum { STEP_X = 1, STEP_Z = CHUNK_DIM, STEP_Y = CHUNK_DIM * CHUNK_DIM };

	// Every cell is a candidate seed. Seeding only from the six face planes was tried — it
	// visits 1,536 candidates instead of 4,096 and never floods a pocket of air sealed inside
	// the chunk, which can contribute no pair to the mask — and measured 637 us against 630,
	// i.e. no change. The cost is the flood, not the search for somewhere to start it, so the
	// simpler loop is the one that stays. Do not re-try this without a measurement.
	for (int seed = 0; seed < CHUNK_BLOCKS; seed++) {
		if (sc->label[seed] || !open[c->blocks[seed]]) continue;

		// An explicit stack, not recursion. A 16^3 open chunk is 4,096 cells deep in the
		// worst case and the 3DS main thread's stack is not there to be spent on a flood
		// fill.
		components++;
		uint8_t faces = 0;
		int top = 0;
		sc->stack[top++] = (uint16_t)seed;
		sc->label[seed]  = components;

		while (top > 0) {
			const int i  = sc->stack[--top];
			const int lx = i & (CHUNK_DIM - 1);
			const int lz = (i >> 4) & (CHUNK_DIM - 1);
			const int ly = i >> 8;
			faces |= faceBitsAt(lx, ly, lz);

			// Six-connected. Diagonal-only gaps are not sight lines, and treating them as
			// connections is how a cull starts keeping chunks behind a checkerboard wall.
			int nbr[6];
			int n = 0;
			if (lx < CHUNK_DIM - 1) nbr[n++] = i + STEP_X;
			if (lx > 0)             nbr[n++] = i - STEP_X;
			if (ly < CHUNK_DIM - 1) nbr[n++] = i + STEP_Y;
			if (ly > 0)             nbr[n++] = i - STEP_Y;
			if (lz < CHUNK_DIM - 1) nbr[n++] = i + STEP_Z;
			if (lz > 0)             nbr[n++] = i - STEP_Z;

			for (int d = 0; d < n; d++) {
				const int j = nbr[d];
				if (sc->label[j] || !open[c->blocks[j]]) continue;

				sc->label[j] = components;
				sc->stack[top++] = (uint16_t)j;
			}
		}

		sc->comp_faces[components] = faces;
	}

	uint16_t mask = 0;
	for (uint16_t k = 1; k <= components; k++)
		mask |= pairsOf(sc->comp_faces[k]);
	return mask;
}

// ---------------------------------------------------------------------------------------

static inline int cellIndex(const VisWalk* w, int lx, int ly, int lz)
{
	return (ly * w->nz + lz) * w->nx + lx;
}

bool visWalkBegin(VisWalk* w, int ox, int oy, int oz, int nx, int ny, int nz)
{
	if (nx <= 0 || ny <= 0 || nz <= 0) return false;
	if (nx > VIS_BOX_MAX_XZ || nz > VIS_BOX_MAX_XZ || ny > VIS_BOX_MAX_Y) return false;

	w->ox = ox; w->oy = oy; w->oz = oz;
	w->nx = nx; w->ny = ny; w->nz = nz;

	const size_t cells = (size_t)nx * (size_t)ny * (size_t)nz;

	// 0xFF, not 0: an empty coordinate has to read as fully connected. See the header —
	// the sky above the terrain is a hole in the mesh list, and a hole that reads as a wall
	// culls the world.
	memset(w->mask, 0xFF, cells * sizeof(w->mask[0]));
	memset(w->drawable, 0, cells * sizeof(w->drawable[0]));
	memset(w->visible,  0, cells * sizeof(w->visible[0]));
	memset(w->entered,  0, cells * sizeof(w->entered[0]));
	memset(w->pending,  0, cells * sizeof(w->pending[0]));
	return true;
}

void visWalkSet(VisWalk* w, int cx, int cy, int cz, uint16_t mask, bool drawable)
{
	const int lx = cx - w->ox, ly = cy - w->oy, lz = cz - w->oz;
	if (lx < 0 || lx >= w->nx || ly < 0 || ly >= w->ny || lz < 0 || lz >= w->nz) return;

	const int i = cellIndex(w, lx, ly, lz);
	w->mask[i]     = mask;
	w->drawable[i] = drawable;
}

bool visWalkVisible(const VisWalk* w, int cx, int cy, int cz)
{
	const int lx = cx - w->ox, ly = cy - w->oy, lz = cz - w->oz;
	if (lx < 0 || lx >= w->nx || ly < 0 || ly >= w->ny || lz < 0 || lz >= w->nz) return true;
	return w->visible[cellIndex(w, lx, ly, lz)];
}

static inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Chunk coordinate of a world block position. floorf first and *then* the arithmetic shift,
// which is world.h's convention — a cast to int truncates toward zero, so at x = -1.5 it
// would answer chunk 0 for a block that is in chunk -1.
static inline int chunkOfBlock(float block)
{
	return (int)floorf(block) >> 4;
}

// The N.V < 0 test. `face` is stepped through only when the camera is on its inner side, so
// the walk can never move back toward the camera. Distances are in world blocks.
static bool faceLeadsAway(int face, int cx, int cy, int cz, float cam_x, float cam_y, float cam_z)
{
	const float lo_x = (float)(cx * CHUNK_DIM), hi_x = lo_x + (float)CHUNK_DIM;
	const float lo_y = (float)(cy * CHUNK_DIM), hi_y = lo_y + (float)CHUNK_DIM;
	const float lo_z = (float)(cz * CHUNK_DIM), hi_z = lo_z + (float)CHUNK_DIM;

	switch (face) {
	case FACE_EAST:   return cam_x <= hi_x;
	case FACE_WEST:   return cam_x >= lo_x;
	case FACE_TOP:    return cam_y <= hi_y;
	case FACE_BOTTOM: return cam_y >= lo_y;
	case FACE_SOUTH:  return cam_z <= hi_z;
	case FACE_NORTH:  return cam_z >= lo_z;
	default:          return false;
	}
}

static inline int oppositeFace(int face)
{
	// The enum is written in +/- pairs, so the opposite is the low bit flipped. Asserted by
	// the host test rather than trusted, because block.h's face order is a contract other
	// files also depend on and this would fail silently if it changed.
	return face ^ 1;
}

// Bit 6 marks the chunk the camera is in: it was not entered through any face, so every
// face is a legal exit.
#define ENTRY_ORIGIN  0x40u

int visWalkRun(VisWalk* w, float cam_x, float cam_y, float cam_z)
{
	const size_t cells = (size_t)w->nx * (size_t)w->ny * (size_t)w->nz;
	memset(w->visible, 0, cells * sizeof(w->visible[0]));
	memset(w->entered, 0, cells * sizeof(w->entered[0]));
	memset(w->pending, 0, cells * sizeof(w->pending[0]));

	const int cam_cx = chunkOfBlock(cam_x);
	const int cam_cy = chunkOfBlock(cam_y);
	const int cam_cz = chunkOfBlock(cam_z);

	// Clamped rather than rejected. A camera outside the loaded box means the player is at
	// its edge, and the nearest cell is the right place for sight to enter from.
	const int lx = clampi(cam_cx - w->ox, 0, w->nx - 1);
	const int ly = clampi(cam_cy - w->oy, 0, w->ny - 1);
	const int lz = clampi(cam_cz - w->oz, 0, w->nz - 1);

	int head = 0, tail = 0;
	const int start = cellIndex(w, lx, ly, lz);
	w->entered[start] = (uint8_t)ENTRY_ORIGIN;
	w->pending[start] = (uint8_t)ENTRY_ORIGIN;
	w->queue[tail++]  = (uint16_t)start;

	int reached = 0;

	while (head < tail) {
		const int i = w->queue[head++];

		const uint8_t fin = w->pending[i];
		w->pending[i] = 0;
		if (!fin) continue;   // a duplicate push whose faces were already expanded

		if (w->drawable[i] && !w->visible[i]) {
			w->visible[i] = true;
			reached++;
		}

		const int ix = i % w->nx;
		const int iz = (i / w->nx) % w->nz;
		const int iy = i / (w->nx * w->nz);

		// Every face reachable from any of the entry faces still waiting on this cell.
		uint8_t exits = 0;
		if (fin & ENTRY_ORIGIN) {
			exits = 0x3F;
		} else {
			for (int a = 0; a < BLOCK_FACES; a++) {
				if (!(fin & (1u << a))) continue;
				for (int b = 0; b < BLOCK_FACES; b++)
					if (visConnected(w->mask[i], a, b)) exits |= (uint8_t)(1u << b);
			}
		}

		for (int f = 0; f < BLOCK_FACES; f++) {
			if (!(exits & (1u << f))) continue;

			const int cx = w->ox + ix, cy = w->oy + iy, cz = w->oz + iz;
			if (!faceLeadsAway(f, cx, cy, cz, cam_x, cam_y, cam_z)) continue;

			int jx = ix, jy = iy, jz = iz;
			switch (f) {
			case FACE_EAST:   jx++; break;
			case FACE_WEST:   jx--; break;
			case FACE_TOP:    jy++; break;
			case FACE_BOTTOM: jy--; break;
			case FACE_SOUTH:  jz++; break;
			default:          jz--; break;
			}
			if (jx < 0 || jx >= w->nx || jy < 0 || jy >= w->ny || jz < 0 || jz >= w->nz)
				continue;

			const int     j   = cellIndex(w, jx, jy, jz);
			const uint8_t opp = (uint8_t)(1u << oppositeFace(f));
			if (w->entered[j] & opp) continue;

			w->entered[j] |= opp;
			w->pending[j] |= opp;
			w->queue[tail++] = (uint16_t)j;
		}
	}

	return reached;
}
