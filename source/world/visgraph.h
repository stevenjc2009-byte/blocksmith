// Cave culling: don't draw what no line of sight can reach.
//
// Step 7.3, and the centrepiece of Phase 7. Tommaso Checchi's algorithm, the one Bedrock
// ships. The idea in one sentence: a chunk you cannot *walk sight to* through the air
// between here and there cannot be on screen, however close it is and however squarely it
// sits in the frustum.
//
// It works in two halves, and they are split across two functions here because they run at
// completely different rates:
//
//   1. visChunkConnectivity() — once per chunk, at mesh time. Flood-fills the see-through
//      cells and reports which of the chunk's six faces are joined to which. A solid block
//      of stone joins nothing; a chunk of open sky joins all fifteen pairs. This is the
//      expensive half and it is amortised over however many frames the chunk survives.
//
//   2. visWalkRun() — once per frame. A breadth-first walk outward from the camera's chunk
//      that may only step through a face pair the connectivity says is open, and may only
//      step *away* from the camera. Everything it reaches is potentially visible; everything
//      it does not reach is provably not, and is skipped.
//
// Why this beats the frustum test rather than duplicating it: the frustum only knows where
// the screen edges are. Standing in a tunnel, every chunk of the hillside around you is
// inside the frustum and none of it can be seen. That case is the normal case underground,
// and it is the case where a GPU-bound console falls over.
//
// ⚠ Its known flaw is *under*-culling, never over-culling — a wide-open ravine or a large
// cavern joins faces that no single sight line really connects, so it keeps chunks it did
// not have to. That failure direction is the safe one: it costs draw calls, never geometry.
// Anything that made it over-cull would punch visible holes in the world, which is why the
// checks in world_test.c are written to catch exactly that.
//
// Pure C, no <3ds.h>, so both halves are host-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk.h"
#include "world/world.h"

// Six faces make fifteen unordered pairs. Bit i of a connectivity mask is set when the pair
// visPairIndex() numbers i is joined by a run of see-through cells.
#define VIS_PAIRS  15

// The sentinel for "no data here, assume the worst". All fifteen real bits are set, plus the
// unused top bit, so the whole table can be initialised with one memset of 0xFF — and the
// worst case is exactly the right default, because a chunk we know nothing about must not be
// allowed to block the walk.
#define VIS_ALL_CONNECTED  0xFFFFu

// Working memory for the flood fill, passed in rather than made static so the fill is a pure
// function and the host test can run several in one process. 18 KB, allocated once by the
// caller and reused for every chunk — the same rule MeshScratch follows, and for the same
// reason: this must never be a per-chunk allocation on a console whose linear heap cannot be
// defragmented.
#define VIS_MAX_COMPONENTS  (CHUNK_BLOCKS / 2 + 1)   // a 16^3 checkerboard, the true worst case

typedef struct {
	uint16_t label[CHUNK_BLOCKS];
	uint16_t stack[CHUNK_BLOCKS];
	uint8_t  comp_faces[VIS_MAX_COMPONENTS];
} VisScratch;

// Index of the unordered pair {a, b} in 0..14, or -1 when a == b. Entering a chunk through a
// face and leaving through the same face is not a traversal, so -1 is a real answer and not
// an error code.
static inline int visPairIndex(int a, int b)
{
	if (a == b) return -1;
	if (a > b) { const int t = a; a = b; b = t; }

	// Start of row a in the flattened upper triangle: 0, 5, 9, 12, 14.
	static const int8_t base[BLOCK_FACES] = { 0, 5, 9, 12, 14, 15 };
	return base[a] + (b - a - 1);
}

static inline bool visConnected(uint16_t mask, int a, int b)
{
	const int i = visPairIndex(a, b);
	return i >= 0 && (mask & (uint16_t)(1u << i)) != 0;
}

// Flood-fills the chunk's see-through cells and returns the fifteen-bit pair mask.
//
// "See-through" is `!(solid && !transparent)`, not `!solid`. Those are the same set today,
// but step 7.5 makes leaves and water transparent, and the day it does they must stop
// blocking sight here as well — writing the test the short way now would leave a canopy
// silently occluding half a forest.
uint16_t visChunkConnectivity(const Chunk* c, VisScratch* sc);

// ---------------------------------------------------------------------------------------
// The per-frame walk.

// The walk runs over a dense box of chunk coordinates rather than over the list of chunks
// that have meshes, because the two are not the same set and the difference is load-bearing:
// main.c never meshes an all-air chunk, so the sky above the terrain is a *hole* in the mesh
// list. Treating a hole as a wall would stop the walk at head height and cull the entire
// world above it. In here a coordinate with nothing in it is VIS_ALL_CONNECTED and not
// drawable — sight passes straight through, and there is nothing there to draw.
#define VIS_BOX_MAX_XZ  16
#define VIS_BOX_MAX_Y   COLUMN_CHUNKS
#define VIS_BOX_CELLS   (VIS_BOX_MAX_XZ * VIS_BOX_MAX_Y * VIS_BOX_MAX_XZ)

typedef struct {
	int ox, oy, oz;      // box origin, in chunk coordinates
	int nx, ny, nz;      // box dimensions, in chunks

	uint16_t mask[VIS_BOX_CELLS];
	bool     drawable[VIS_BOX_CELLS];
	bool     visible[VIS_BOX_CELLS];

	uint8_t  entered[VIS_BOX_CELLS];   // faces this cell has already been entered through
	uint8_t  pending[VIS_BOX_CELLS];   // ...of those, the ones not yet expanded

	// A cell is pushed once per new entry face, so six pushes per cell is the ceiling — plus
	// one for the camera's own cell, which is seeded through no face at all and can still be
	// entered through all six later.
	uint16_t queue[VIS_BOX_CELLS * BLOCK_FACES + 1];
} VisWalk;

// Clears the box and sets its extent. False if the requested box is larger than VisWalk can
// hold, in which case the caller must fall back to drawing everything — over-culling because
// the scratch was too small would be a hole in the world, and a missing optimisation is only
// slow. Returns with every cell marked "empty, see-through, not drawable".
bool visWalkBegin(VisWalk* w, int ox, int oy, int oz, int nx, int ny, int nz);

// Puts one real chunk into the box. Coordinates outside it are ignored rather than rejected:
// the caller builds the box from the same set it then inserts, so an outlier means the box
// was clamped, and the walk's answer for a chunk it never heard of is "visible" anyway.
void visWalkSet(VisWalk* w, int cx, int cy, int cz, uint16_t mask, bool drawable);

// Walks outward from the camera and returns how many *drawable* cells it reached.
//
// The camera is in world block coordinates, and is used for more than picking a starting
// chunk: a face may only be stepped through when the camera is on its inner side, which is
// the N.V < 0 test. That single rule is what stops the walk doubling back — once it has
// stepped +X, the -X face of everything ahead has the camera behind it — so the walk is a
// DAG and terminates without any separate visited-direction bookkeeping.
//
// A camera outside the box starts from the nearest cell in it, which is the correct entry
// point when the player is standing at the edge of the loaded area.
int visWalkRun(VisWalk* w, float cam_x, float cam_y, float cam_z);

// After visWalkRun. A coordinate outside the box answers true: the walk knows nothing about
// it, and the only safe answer to "may I skip this" is no.
bool visWalkVisible(const VisWalk* w, int cx, int cy, int cz);
