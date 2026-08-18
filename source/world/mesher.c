#include "world/mesher.h"

#include "world/atlas_uv.h"
#include "world/block.h"

// One face: the neighbour it looks at, an origin corner, and two tangent axes whose
// cross product is the outward normal. Deriving the winding from the tangents is what
// keeps all six faces counter-clockwise from outside without hand-checking twenty-four
// vertices.
//
// This table is the source of truth for face geometry, but it is *not* what the inner
// loop reads — see FacePlan below. Everything the mesher needs per vertex is derived
// from here once, so the hot path does no arithmetic on it.
typedef struct {
	int8_t dir[3];            // the neighbour cell this face faces
	int8_t origin[3];
	int8_t t1[3];
	int8_t t2[3];
	bool   vertical_is_t1;    // which tangent climbs the wall, for texture upness
} MeshFace;

// Index order is the face order of the whole project: east, west, top, bottom,
// south, north — the registry's tex[] is indexed by it, so it is a contract.
static const MeshFace kFaces[BLOCK_FACES] = {
	// +X: t1=+Y, t2=+Z   (Y x Z = +X)
	{ { 1, 0, 0}, {1,0,0}, {0,1,0}, {0,0,1}, true  },
	// -X: t1=+Z, t2=+Y   (Z x Y = -X)
	{ {-1, 0, 0}, {0,0,0}, {0,0,1}, {0,1,0}, false },
	// +Y: t1=+Z, t2=+X   (Z x X = +Y)
	{ { 0, 1, 0}, {0,1,0}, {0,0,1}, {1,0,0}, false },
	// -Y: t1=+X, t2=+Z   (X x Z = -Y)
	{ { 0,-1, 0}, {0,0,0}, {1,0,0}, {0,0,1}, false },
	// +Z: t1=+X, t2=+Y   (X x Y = +Z)
	{ { 0, 0, 1}, {0,0,1}, {1,0,0}, {0,1,0}, false },
	// -Z: t1=+Y, t2=+X   (Y x X = -Z)
	{ { 0, 0,-1}, {0,0,0}, {0,1,0}, {1,0,0}, true  },
};

// Everything the inner loop needs for one face, precomputed.
//
// This exists because the first working mesher measured 3.55 ms per chunk on the
// console against a 195 us scratch fill — the meshing, not the copying, was the whole
// cost. Three things were paying for it, all of them per *vertex*:
//
//   * blockIsSolid() is a call into block.c, so it could not be inlined: about 48,000
//     function calls per chunk, one per neighbour read.
//   * atlasRect() divides and modulos by ATLAS_GRID, and the ARM11 has no divide
//     instruction — that is a libgcc __aeabi_idivmod call per emitted face.
//   * the AO taps rebuilt two int8_t[3] direction vectors on the stack per corner and
//     passed them by pointer, then turned them back into a scratch index with two
//     multiplies.
//
// So solidity becomes a byte table, tile rects become a table, and every neighbour
// read becomes a constant offset from the cell's own scratch index. Derived from
// kFaces and the block registry rather than typed out, so there is still one source of
// truth and no transcription to get wrong.
typedef struct {
	int8_t  px, py, pz;     // vertex position, relative to the cell's own corner
	uint8_t u_hi, v_hi;     // take u1/v1 rather than u0/v0
	int16_t ao1, ao2, aoc;  // scratch index deltas for the two flanks and the diagonal
} CornerPlan;

typedef struct {
	int16_t    neighbour;   // scratch index delta to the cell this face faces
	CornerPlan corner[4];
} FacePlan;

// Scratch index delta for a neighbour offset. Same arithmetic as scratchIndex(), minus
// the base — which is exactly why it is a constant the loop can just add.
#define SCRATCH_DELTA(dx, dy, dz) \
	((int16_t)((((dy) * SCRATCH_DIM) + (dz)) * SCRATCH_DIM + (dx)))

// Solidity of the scratch itself, resolved once per chunk. The mesher asks "is this
// cell solid?" about 44,000 times per chunk, and going through the block id every time
// is two dependent loads; resolving all 5,832 cells up front makes each of those one.
static uint8_t  s_cell_solid[SCRATCH_BLOCKS];

static FacePlan s_plan[BLOCK_FACES];
static uint8_t  s_solid[256];                       // indexed by a raw BlockId byte
static AtlasRect s_rect[BLOCK_COUNT][BLOCK_FACES];
static bool     s_ready;

// Builds the three lookup tables. Runs once, on the first chunk meshed: the block
// registry and kFaces are both const, so nothing here can go stale.
static void planBuild(void)
{
	// A raw id, not a validated one. blockInfo() already maps an unknown id to air, so
	// this table answers for all 256 byte values and the mesher never bounds-checks.
	for (int i = 0; i < 256; i++)
		s_solid[i] = (uint8_t)blockIsSolid((BlockId)i);

	for (int id = 0; id < BLOCK_COUNT; id++)
		for (int face = 0; face < BLOCK_FACES; face++)
			s_rect[id][face] = atlasRect(blockFaceTex((BlockId)id, face));

	// The corners of a quad, in the counter-clockwise order they are emitted.
	static const uint8_t kCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

	for (int face = 0; face < BLOCK_FACES; face++) {
		const MeshFace* f = &kFaces[face];
		FacePlan*       p = &s_plan[face];

		p->neighbour = SCRATCH_DELTA(f->dir[0], f->dir[1], f->dir[2]);

		for (int i = 0; i < 4; i++) {
			const int   a = kCorners[i][0];
			const int   b = kCorners[i][1];
			CornerPlan* c = &p->corner[i];

			c->px = (int8_t)(f->origin[0] + f->t1[0] * a + f->t2[0] * b);
			c->py = (int8_t)(f->origin[1] + f->t1[1] * a + f->t2[1] * b);
			c->pz = (int8_t)(f->origin[2] + f->t1[2] * a + f->t2[2] * b);

			// AtlasRect is texture space, where v grows upwards, so world-up is v1.
			const int up   = f->vertical_is_t1 ? a : b;
			const int side = f->vertical_is_t1 ? b : a;
			c->u_hi = (uint8_t)side;
			c->v_hi = (uint8_t)up;

			// The corner's two flanking directions: the tangent axes, pointing towards
			// this corner rather than away from it. Both taps sit one step out along the
			// face normal, and the diagonal is one step along each flank.
			const int sa[3] = { a ? f->t1[0] : -f->t1[0],
			                    a ? f->t1[1] : -f->t1[1],
			                    a ? f->t1[2] : -f->t1[2] };
			const int sb[3] = { b ? f->t2[0] : -f->t2[0],
			                    b ? f->t2[1] : -f->t2[1],
			                    b ? f->t2[2] : -f->t2[2] };

			c->ao1 = SCRATCH_DELTA(f->dir[0] + sa[0], f->dir[1] + sa[1], f->dir[2] + sa[2]);
			c->ao2 = SCRATCH_DELTA(f->dir[0] + sb[0], f->dir[1] + sb[1], f->dir[2] + sb[2]);
			c->aoc = SCRATCH_DELTA(f->dir[0] + sa[0] + sb[0],
			                       f->dir[1] + sa[1] + sb[1],
			                       f->dir[2] + sa[2] + sb[2]);
		}
	}

	s_ready = true;
}

// Writes one quad: four vertices and the six indices of its two triangles. Refuses
// to write past the caller's buffers and says so, rather than trusting a size
// argument — an overflowing mesher would corrupt the linear heap.
//
// `si` is the cell's own scratch index, so every AO tap is one add and one byte load.
static void emitFace(MeshOut* o, int si, int lx, int ly, int lz,
                     int face, const AtlasRect* r)
{
	if (o->vert_count + 4 > o->vert_cap || o->index_count + 6 > o->index_cap) {
		o->overflow = true;
		return;
	}

	const FacePlan* p    = &s_plan[face];
	const uint16_t  base = (uint16_t)o->vert_count;

	for (int i = 0; i < 4; i++) {
		const CornerPlan* c = &p->corner[i];
		MeshVertex*       v = &o->verts[o->vert_count++];

		v->x = (int8_t)(lx + c->px);
		v->y = (int8_t)(ly + c->py);
		v->z = (int8_t)(lz + c->pz);

		v->u   = c->u_hi ? r->u1 : r->u0;
		v->v   = c->v_hi ? r->v1 : r->v0;
		v->nrm = (uint8_t)face;

		// Ambient occlusion, the standard three-neighbour rule: the two blocks flanking
		// the corner in the face's own plane and the one diagonally across it, all
		// *outside* the face.
		//
		//   both flanks solid            -> 0, fully dark: the corner is a crevice
		//   otherwise 3 - (s1 + s2 + c)  -> 1..3
		//
		// This is why the scratch is 18³ and filled from all 26 neighbours: the diagonal
		// read steps one block sideways *and* one block out, so a six-neighbour border
		// would leave chunk-edge corners guessing. The shader maps 0..3 to 0.45 .. 1.0.
		const int s1 = s_cell_solid[si + c->ao1];
		const int s2 = s_cell_solid[si + c->ao2];
		v->ao = (s1 && s2) ? 0
		                   : (uint8_t)(3 - s1 - s2 - s_cell_solid[si + c->aoc]);
		v->pad = 0;
	}

	// Two triangles round the quad, keeping the counter-clockwise order the corners
	// were emitted in.
	o->indices[o->index_count++] = base;
	o->indices[o->index_count++] = (uint16_t)(base + 1);
	o->indices[o->index_count++] = (uint16_t)(base + 2);
	o->indices[o->index_count++] = base;
	o->indices[o->index_count++] = (uint16_t)(base + 2);
	o->indices[o->index_count++] = (uint16_t)(base + 3);

	o->faces++;
}

void meshChunk(MeshOut* out, const MeshScratch* s)
{
	out->vert_count  = 0;
	out->index_count = 0;
	out->faces       = 0;
	out->overflow    = false;

	if (!s_ready) planBuild();

	for (int i = 0; i < SCRATCH_BLOCKS; i++)
		s_cell_solid[i] = s_solid[s->blocks[i]];

	// y outer, x inner: x is the contiguous axis in both the chunk and the scratch, so
	// this walks memory forwards. The scratch index rides along with the x loop rather
	// than being recomputed per cell — 4,096 multiplies a chunk that buy nothing.
	for (int ly = 0; ly < CHUNK_DIM; ly++) {
		for (int lz = 0; lz < CHUNK_DIM; lz++) {
			int si = scratchIndex(1, ly + 1, lz + 1);

			for (int lx = 0; lx < CHUNK_DIM; lx++, si++) {
				if (!s_cell_solid[si]) continue;
				const BlockId id = s->blocks[si];

				// Solid implies a real registry row, since s_solid says no for every id
				// past the registry — so s_rect is safe to index without a check.
				const AtlasRect* rects = s_rect[id];

				for (int face = 0; face < BLOCK_FACES; face++) {
					// A face exists only where solid meets non-solid. This single test
					// is what removes 22 of every 24 faces in solid rock — and it reads
					// the border of the scratch, so chunk seams are culled against the
					// real neighbour instead of guessing air.
					if (s_cell_solid[si + s_plan[face].neighbour]) continue;

					emitFace(out, si, lx, ly, lz, face, &rects[face]);
				}
			}
		}
	}
}
