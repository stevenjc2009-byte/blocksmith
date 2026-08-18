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

// Step 9.1. Whether each cell *occludes* — solid and opaque. Solidity alone was the test until
// the 2026-08-18 audit, and it culled the face between an opaque block and a see-through one:
// the stone under a leaf, and the trunk inside its own canopy, lost the faces pointing at the
// leaves and showed sky through the alpha holes instead. A second table rather than a second
// condition on the block id, for the same reason s_cell_solid is a table: this is read once per
// face, about 24,000 times a chunk, and it has to stay a single byte load.
static uint8_t  s_cell_occl[SCRATCH_BLOCKS];

static FacePlan s_plan[BLOCK_FACES];
static uint8_t  s_solid[256];                       // indexed by a raw BlockId byte

// Step 7.5: does this block's geometry belong in the transparent pass? Solid *and*
// transparent — air is transparent and never drawn, so both flags have to be read.
// A byte table for the same reason s_solid is one: the alternative is a call into
// block.c per cell, and there is no link-time optimisation in this build.
static uint8_t  s_deferred[256];
static uint8_t  s_occludes[256];                    // solid and opaque: hides what is behind it
static AtlasRect s_rect[BLOCK_COUNT][BLOCK_FACES];
static bool     s_ready;

// Builds the three lookup tables. Runs once, on the first chunk meshed: the block
// registry and kFaces are both const, so nothing here can go stale.
static void planBuild(void)
{
	// A raw id, not a validated one. blockInfo() already maps an unknown id to air, so
	// this table answers for all 256 byte values and the mesher never bounds-checks.
	for (int i = 0; i < 256; i++) {
		const BlockInfo* info = blockInfo((BlockId)i);
		s_solid[i]    = (uint8_t)info->solid;
		s_deferred[i] = (uint8_t)(info->solid && info->transparent);
		s_occludes[i] = (uint8_t)(info->solid && !info->transparent);
	}

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

// Step 9.2. One cell that is going to emit something: where it is, and what it is.
//
// This list exists because the opaque geometry is now laid out face-major — six runs, one per
// face direction, see meshChunk — and the obvious way to produce that, six sweeps of the 4,096
// cells, re-pays the solidity test, the block-id load and the deferred test five extra times
// per cell. Sweeping once into this list and then walking the list six times pays them once,
// and the list is already filtered down to the cells that can emit at all.
//
// 24 KB of .bss, static for the same reason MeshScratch is: the main thread's stack is 32 KB
// and this runs on it.
typedef struct {
	uint16_t si;              // the cell's scratch index
	uint8_t  id;              // its block, for the atlas rects
	uint8_t  lx, ly, lz;      // its chunk-local position, 0..15
} EmitCell;

static EmitCell s_emit[CHUNK_BLOCKS];
static int      s_emit_opaque_n;   // opaque cells, [0, s_emit_opaque_n)
static int      s_emit_alpha_lo;   // transparent cells, [s_emit_alpha_lo, CHUNK_BLOCKS)

// Both halves in one sweep, growing towards each other from the two ends of one array. They
// cannot collide: together they are at most the 4,096 cells of a chunk.
//
// One sweep, not two. Until step 9.2 the transparent geometry got a full second walk of the
// 4,096 cells, which for the overwhelming majority of chunks — every one with no leaves in it
// anywhere — found nothing and cost the walk regardless.
//
// The transparent half comes out in reverse spatial order, because it is filled from the back.
// That is fine and deliberate: it is drawn as one alpha-tested run inside a chunk that is
// itself depth-sorted against the others, and cutout geometry is order-independent within a
// draw. Nothing downstream may assume otherwise.
static void emitCollect(const MeshScratch* s)
{
	int opaque = 0;
	int alpha  = CHUNK_BLOCKS;

	// y outer, x inner: x is the contiguous axis in both the chunk and the scratch, so
	// this walks memory forwards. The scratch index rides along with the x loop rather
	// than being recomputed per cell — 4,096 multiplies a chunk that buy nothing.
	for (int ly = 0; ly < CHUNK_DIM; ly++) {
		for (int lz = 0; lz < CHUNK_DIM; lz++) {
			int si = scratchIndex(1, ly + 1, lz + 1);

			for (int lx = 0; lx < CHUNK_DIM; lx++, si++) {
				if (!s_cell_solid[si]) continue;

				const BlockId id = s->blocks[si];
				EmitCell*     e  = s_deferred[id] ? &s_emit[--alpha] : &s_emit[opaque++];

				e->si = (uint16_t)si;
				e->id = (uint8_t)id;
				e->lx = (uint8_t)lx;
				e->ly = (uint8_t)ly;
				e->lz = (uint8_t)lz;
			}
		}
	}

	s_emit_opaque_n = opaque;
	s_emit_alpha_lo = alpha;
}

// One geometry pass over a list of cells. `deferred` says which list it is: the flag decides
// whether same-material culling applies, and it is constant for the whole pass, so the opaque
// pass pays nothing for it. [face_first, face_end) selects which face directions to emit, so
// the caller can lay the opaque run out face-major — see meshChunk.
static void meshPass(MeshOut* out, const MeshScratch* s, const EmitCell* cells, int n,
                     bool deferred, int face_first, int face_end)
{
	for (int i = 0; i < n; i++) {
		const EmitCell* e  = &cells[i];
		const int       si = e->si;

		// Solid implies a real registry row, since s_solid says no for every id past the
		// registry — so s_rect is safe to index without a check.
		const AtlasRect* rects = s_rect[e->id];

		for (int face = face_first; face < face_end; face++) {
			const int ni = si + s_plan[face].neighbour;

			// A face is hidden only by a neighbour that actually covers it. That is the
			// test which removes 22 of every 24 faces in solid rock, and it reads the
			// border of the scratch, so chunk seams are culled against the real neighbour
			// instead of guessing air.
			if (s_cell_occl[ni]) continue;

			// ...and, in the transparent pass only, by a neighbour of the same material.
			// This is what keeps a canopy cheap: leaf against leaf is culled, so the pass
			// draws the shell of a tree and not every leaf inside it.
			//
			// The two halves are deliberately not one test on `solid`. Culling on solidity
			// alone is what hid the trunk faces inside a canopy — see
			// testMesherOpaqueBehindTransparent.
			if (deferred && s_cell_solid[ni] && s->blocks[ni] == e->id) continue;

			emitFace(out, si, e->lx, e->ly, e->lz, face, &rects[face]);
		}
	}
}

void meshChunk(MeshOut* out, const MeshScratch* s)
{
	out->vert_count  = 0;
	out->index_count = 0;
	out->faces       = 0;
	out->overflow    = false;

	if (!s_ready) planBuild();

	for (int i = 0; i < SCRATCH_BLOCKS; i++) {
		const BlockId id = s->blocks[i];
		s_cell_solid[i] = s_solid[id];
		s_cell_occl[i]  = s_occludes[id];
	}

	// Which cells emit at all, and which half of the frame each belongs to. One sweep for both.
	emitCollect(s);

	// Step 9.2. The opaque run is laid out face-major: six walks of the emit list, one per face
	// direction, so each direction ends up as one contiguous stretch of indices the renderer
	// can draw or skip whole. It skips the three directions that point away from the camera,
	// which is about half the opaque vertices of every chunk on screen, every frame, per eye.
	out->face_start[0] = 0;
	for (int face = 0; face < BLOCK_FACES; face++) {
		meshPass(out, s, s_emit, s_emit_opaque_n, false, face, face + 1);
		out->face_start[face + 1] = out->index_count;
	}

	// The boundary between the two runs, taken before the second pass writes anything.
	// Recorded even when the second pass turns out to be empty, so the renderer never has
	// to special-case "this chunk has no transparent geometry" — it draws a zero-length
	// second run, which it already skips. face_start[BLOCK_FACES] is this same number by
	// construction, which is what makes the last bucket's length need no special case.
	out->opaque_index_count = out->index_count;
	out->opaque_faces       = out->faces;

	// The transparent pass. Not bucketed by face: it is a few percent of the geometry, and it
	// is drawn back-to-front as one ordered sequence. A chunk with no transparent blocks — most
	// of them — passes a count of zero here and the call returns immediately.
	meshPass(out, s, &s_emit[s_emit_alpha_lo], CHUNK_BLOCKS - s_emit_alpha_lo,
	         true, 0, BLOCK_FACES);
}
