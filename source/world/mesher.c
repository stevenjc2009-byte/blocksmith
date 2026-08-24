#include "world/mesher.h"

#include <string.h>

#include "world/atlas_uv.h"
#include "world/block.h"
#include "world/light.h"
#include "world/registry.h"

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

// Step 9.2. The bucket order, declared in mesher.h — +X +Y +Z -X -Y -Z. No two neighbours
// share an axis, which is what lets the renderer merge the buckets it keeps.
const uint8_t kFaceOrder[BLOCK_FACES] = {
	FACE_EAST, FACE_TOP, FACE_SOUTH, FACE_WEST, FACE_BOTTOM, FACE_NORTH
};

// Everything the inner loop needs for one face, precomputed.
//
// This exists because the first working mesher measured 3.55 ms per chunk on the
// console against a 195 us scratch fill — the meshing, not the copying, was the whole
// cost. Three things were paying for it, all of them per *vertex*:
//
//   * blockIsSolid() is a call into block.c, so it could not be inlined: about 48,000
//     function calls per chunk, one per neighbour read.
//   * atlasRect() used to divide and modulo by the atlas grid width, and the ARM11 has no
//     divide instruction — that was a libgcc __aeabi_idivmod call per emitted face. (The
//     v1.6.0 strip atlas removed the arithmetic entirely: the sheet is one tile wide, so
//     atlasRect() is now four assignments and no division at all. The table below is kept
//     because it is still two byte loads instead of a call into block.c per face.)
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

	// v1.6.0 task 11. The world axis the face's TEXTURE u runs along — the only axis a
	// greedy run may grow down, because the strip atlas repeats in u and only in u (see
	// world/atlas_uv.h). Derived from kFaces below rather than typed out, so the merge and
	// the UVs can never disagree about which way "along the tile" points.
	int16_t    u_step;      // scratch index delta of one block along +u
	int8_t     ux, uy, uz;  // the same step in world block units
	uint8_t    u_axis;      // 0=x, 1=y, 2=z: which chunk-local coordinate u_step moves
	bool       u_merge;     // the step really is +1 on one axis, so a forward run is safe

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

// v1.6.0 task 13. Whether each cell emits geometry at all. Until this task that was the
// same question as s_cell_solid — every block that was not air was solid — and
// emitCollect's scan below read the solidity table for it. It is its own table now
// because the shapes this task exists for split the two apart: a plant draws and does
// not collide, and so will water. Air is 0 here exactly as it is 0 in s_cell_solid, so
// the four-at-a-time word scan is unaffected.
static uint8_t  s_cell_draw[SCRATCH_BLOCKS];

// v1.8.0 task 22b. How far below its cell's top a flowing water surface sits, in eighths of a
// block, per scratch cell. 0 is a full cube, which is every block in the game except a flowing
// water cell — so the table is built only when the scratch actually carries flow levels, and
// s_any_drop gates every read of it so a stale table from a previous chunk can never be read.
//
// A table and not a call into world/water.c, for two reasons that are both structural. The
// mesher is linked into six host binaries and the simulation into one, so a direct call would
// drag the whole of water.c into all of them; and meshChunk() is handed a scratch, not a chunk
// coordinate, so it could not name the cells to ask about even if it wanted to. The levels
// arrive the same way the light does — copied into the scratch by the owner of that data, once,
// before the mesh runs. See world/scratch.h's water band.
static uint8_t  s_cell_drop[SCRATCH_BLOCKS];
static bool     s_any_drop;

// Every read of the drop table goes through here. When no cell in the scratch is flowing, the
// table is not rebuilt at all and this answers 0 without touching it.
static inline uint8_t cellDrop(int si)
{
	return s_any_drop ? s_cell_drop[si] : 0;
}

static FacePlan s_plan[BLOCK_FACES];
static uint8_t  s_solid[256];                       // indexed by a raw BlockId byte
static uint8_t  s_draws[256];                       // emits geometry: see s_cell_draw
static uint8_t  s_shape[256];                       // BLOCK_SHAPE_*

// Step 7.5: does this block's geometry belong in the transparent pass? Solid *and*
// transparent — air is transparent and never drawn, so both flags have to be read.
// A byte table for the same reason s_solid is one: the alternative is a call into
// block.c per cell, and there is no link-time optimisation in this build.
static uint8_t  s_deferred[256];
static uint8_t  s_occludes[256];                    // solid and opaque: hides what is behind it
// Sized for the whole id space, not just BLOCK_COUNT: dynamic block ids arrive
// over the wire (0x80..0xFD) and meshPass indexes this table by that raw byte.
// The old [BLOCK_COUNT] bound was only ever safe because every id in a chunk
// happened to be a core row; a server-registered solid block would have
// written past the array. 256 rows x 6 faces x 4 bytes = 6 KB of bss.
static AtlasRect s_rect[REGISTRY_MAX][BLOCK_FACES];
static bool     s_ready;

// Drops the derived tables so the next meshChunk() rebuilds them lazily.
// Called after remote registry definitions are applied at join; on the console
// that happens before any worker exists, so no locking is needed here.
void mesherInvalidateTables(void)
{
	s_ready = false;
}

// Builds the lookup tables. Runs once, on the first chunk meshed — and again
// after mesherInvalidateTables(), since dynamic registrations can land between
// joins. kFaces never changes; the block tables can.
static void planBuild(void)
{
	// A raw id, not a validated one. blockInfo() already maps an unknown id to air, so
	// this table answers for all 256 byte values and the mesher never bounds-checks.
	for (int i = 0; i < 256; i++) {
		const BlockInfo* info = blockInfo((BlockId)i);
		const bool cube = info->shape == BLOCK_SHAPE_FULL_CUBE;

		s_shape[i] = info->shape;
		s_draws[i] = (uint8_t)blockIsDrawn((BlockId)i);

		// v1.6.0 task 13 narrowed the three tables below by one term each, and every one
		// of those terms is false for nothing in the registry as it stands — all eight
		// core rows are solid-or-air full cubes, so all three tables come out byte-for-byte
		// what they were. What each term buys:
		//
		//   s_solid    "fills its cell", which is what the AO taps mean by solid. A shape
		//              that only crosses its cell diagonally is not a crevice wall, so a
		//              non-cube must never darken the corners of its neighbours even if
		//              something registers it solid.
		//   s_deferred which of the two passes it belongs in. Non-cube geometry has no
		//              per-face buckets, and the opaque run is emitted face-major (six
		//              walks, one per direction) — so a non-cube in the opaque list would
		//              be emitted six times over. Forcing it deferred is structural, not
		//              a stylistic choice about alpha.
		//   s_occludes what may hide the face behind it. This is the one that makes holes
		//              in the world if it is wrong: an X of two quads does not cover the
		//              cell's faces, so it can never cull its neighbour's.
		s_solid[i]    = (uint8_t)(info->solid && cube);
		s_deferred[i] = (uint8_t)(s_draws[i] && (info->transparent || !cube));
		s_occludes[i] = (uint8_t)(info->solid && !info->transparent && cube);
	}

	// Every raw byte value gets a row: blockInfo() maps undefined ids to air,
	// so the mesher never bounds-checks, and a dynamic id (0x80..0xFD) must
	// find its real rect here once it is registered.
	for (int id = 0; id < REGISTRY_MAX; id++)
		for (int face = 0; face < BLOCK_FACES; face++)
			s_rect[id][face] = atlasRect(blockFaceTex((BlockId)id, face));

	// The corners of a quad, in the counter-clockwise order they are emitted.
	static const uint8_t kCorners[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

	for (int face = 0; face < BLOCK_FACES; face++) {
		const MeshFace* f = &kFaces[face];
		FacePlan*       p = &s_plan[face];

		p->neighbour = SCRATCH_DELTA(f->dir[0], f->dir[1], f->dir[2]);

		// The u axis. Below, c->u_hi is set from `side`, which is b when vertical_is_t1 and
		// a otherwise — and a scales t1 while b scales t2 — so the tangent that carries u is
		// t2 for a vertical_is_t1 face and t1 for the others. That makes u run +Z for +X, -X
		// and +Y, and +X for -Y, +Z and -Z: always a POSITIVE unit step, which is what lets
		// mergeRun below walk forward and bound itself with CHUNK_DIM minus one coordinate.
		// u_merge records that rather than assuming it, so a future face table with a
		// negative or diagonal tangent loses merging instead of running off the chunk.
		const int8_t* ut = f->vertical_is_t1 ? f->t2 : f->t1;
		p->ux     = ut[0];
		p->uy     = ut[1];
		p->uz     = ut[2];
		p->u_step = SCRATCH_DELTA(ut[0], ut[1], ut[2]);
		p->u_axis = (uint8_t)(ut[0] ? 0 : (ut[1] ? 1 : 2));
		p->u_merge = (ut[0] + ut[1] + ut[2]) == 1 &&
		             (ut[0] >= 0) && (ut[1] >= 0) && (ut[2] >= 0);

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

// Exact floor division by a count known to be 1..4, without the libgcc
// __aeabi_idivmod call the ARM11 would otherwise pay per corner per channel.
// Sums never exceed 15*4 = 60, where (n * 171) >> 9 is exact for /3.
static inline int avgByCnt(int sum, int cnt)
{
	switch (cnt) {
	case 2:  return sum >> 1;
	case 3:  return (sum * 171) >> 9;
	case 4:  return sum >> 2;
	default: return sum;
	}
}

// Ambient occlusion for one corner, the standard three-neighbour rule: the two blocks
// flanking the corner in the face's own plane and the one diagonally across it, all
// *outside* the face.
//
//   both flanks solid            -> 0, fully dark: the corner is a crevice
//   otherwise 3 - (s1 + s2 + c)  -> 1..3
//
// This is why the scratch is 18³ and filled from all 26 neighbours: the diagonal read steps
// one block sideways *and* one block out, so a six-neighbour border would leave chunk-edge
// corners guessing. The shader maps 0..3 to 0.45 .. 1.0.
//
// It is a function of its own, rather than the four lines emitFace used to carry inline,
// because v1.6.0 task 11 needs to ask the SAME question before emitting: two faces may only
// be merged into one quad if their AO is flat and equal, and a merge rule computing AO its
// own way would drift from the rule that bakes it. One copy, two callers.
static inline uint8_t cornerAO(int si, const CornerPlan* c)
{
	const int s1 = s_cell_solid[si + c->ao1];
	const int s2 = s_cell_solid[si + c->ao2];
	return (s1 && s2) ? 0 : (uint8_t)(3 - s1 - s2 - s_cell_solid[si + c->aoc]);
}

// Smooth per-corner light: average the four cells touching this corner from outside the
// face — the neighbour cell plus the same two flanks and diagonal the AO rule already walks
// — skipping occluding ones. The neighbour cell is never occluding (the face would not have
// been emitted), so at least one tap always counts.
//
// Split out for the same reason as cornerAO above: the merge test has to agree with the
// baker exactly, or a merged quad would carry light its own halves never had.
static inline uint8_t cornerLight(const MeshScratch* s, int si, const FacePlan* p,
                                  const CornerPlan* c)
{
	const int taps[4] = { si + p->neighbour, si + c->ao1, si + c->ao2, si + c->aoc };
	int ssum = 0, bsum = 0, cnt = 0;
	for (int t = 0; t < 4; t++) {
		if (s_cell_occl[taps[t]]) continue;
		ssum += s->light[taps[t]] >> 4;
		bsum += s->light[taps[t]] & 15;
		cnt++;
	}
	return (uint8_t)((avgByCnt(ssum, cnt) << 4) | avgByCnt(bsum, cnt));
}

// Writes one quad: four vertices and the six indices of its two triangles. Refuses
// to write past the caller's buffers and says so, rather than trusting a size
// argument — an overflowing mesher would corrupt the linear heap.
//
// `si` is the cell's own scratch index, so every AO tap is one add and one byte load.
//
// `lit` is the lighting engine gate, resolved once per meshChunk. Off — every Old
// 3DS build, every host test by default — pad is written 0 exactly as it always
// was and the vertex bytes are identical to the pre-lighting mesher's. On, pad
// carries the corner's smooth light (sky in the high nibble, block in the low)
// for the dynamic shader to unpack.
//
// `drop` is the water height, in eighths of a block, that this cell's TOP-PLANE corners are
// pulled down by — 0 for everything that is not a flowing water cell, which is everything the
// mesher emitted before v1.8.0 task 22b. It rides in the nrm byte (see mesher.h): the shader
// already fetches a per-vertex table row from that byte for the face brightness, so the offset
// comes along for one extra `add` and no extra fetch, and a row for face 0..5 carries 0.0.
static void emitFace(MeshOut* o, const MeshScratch* s, int si, int lx, int ly, int lz,
                     int face, const AtlasRect* r, bool lit, uint8_t drop)
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
		v->v   = c->v_hi ? r->vslot1 : r->vslot0;

		// The drop applies to the cell's TOP plane and to nothing else, so a shortened cube
		// keeps its floor where it was and only its lid comes down. c->py is the corner's own
		// y within the cell, 0 or 1, straight out of the face table — so this asks the same
		// question kFaces already answers and does not become a second copy of it. A top face
		// has py == 1 at all four corners, a bottom face 0 at all four, and a side face two of
		// each, which is exactly the wall that has to shrink with the surface it holds up.
		v->nrm = (uint8_t)(face | (c->py > 0 ? (int)drop << MESH_NRM_FACE_BITS : 0));

		// See cornerAO/cornerLight above — the rules live there because the greedy merge
		// has to ask exactly the same questions before it groups two faces into one quad.
		v->ao  = cornerAO(si, c);
		v->pad = lit ? cornerLight(s, si, p, c) : 0;
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

// ── v1.6.0 task 11: greedy meshing, U only ───────────────────────────────────
//
// Merges runs of co-planar block faces along the face's own texture-u axis into one wide
// quad. Four vertices and six indices either way, so a 15-block run costs a fifteenth of
// the vertices it used to and draws in one triangle pair instead of fifteen.
//
// THREE LIMITS, all deliberate, none of them a shortcut:
//
//  * U ONLY. gfx/atlas.c samples the sheet GPU_REPEAT in u and GPU_CLAMP_TO_EDGE in v, and
//    the sheet is exactly one tile wide (world/atlas_uv.h), so the repeat period in u is
//    exactly one tile and u may run as far as a merge needs. v has no such period: the
//    slots are stacked directly on top of each other, so extending v walks into the next
//    block's art and CLAMP_TO_EDGE does not save it. A 2D merge would need a different
//    sheet layout and is not what this task is.
//
//  * AT MOST ATLAS_MAX_MERGE_BLOCKS (15). MeshVertex.u is a uint8_t and a run w blocks wide
//    emits u1 = 16*w; 16*15 = 240 fits and 16*16 = 256 wraps to 0, which would silently
//    draw a zero-width slice of *a* texture rather than fail. The arithmetic is spelled out
//    in world/atlas_uv.h; the cap is read from there, never restated.
//
//  * FLAT, EQUAL SHADING ONLY. Two faces merge only when each has the same AO at all four
//    of its corners, the same light at all four, and the two agree with each other. That is
//    the only rule under which the merged quad is pixel-identical to the quads it replaces:
//    four equal corners interpolate to a constant, so nothing on screen moves. The looser
//    "equal at the shared edge" rule would let a merged quad ramp linearly across a span
//    that used to be flat-then-ramped, and a measurement on real terrain found 43% of quads
//    carry flat AO — dropping AO to merge more would flatten the world's shading visibly.
//
// Crosses never reach here at all: meshPass sends every non-cube shape to emitCross and
// skips the six-face loop, and a run only ever extends into cells holding the SAME block
// id, so a cube run can never absorb one.

// Which faces have already been eaten by an earlier run, one bit per face direction. A
// cell consumed as the middle of a run must not emit that face again when the walk reaches
// it. Cleared once per meshChunk; the opaque pass walks each direction separately and the
// two passes never share a cell, so one table serves all seven walks.
static uint8_t s_face_done[SCRATCH_BLOCKS];

// May cell `nb` join a run of `id` for this face? Everything meshPass would have tested for
// the cell in its own right, in the same order and against the same tables — same block (so
// same tile, same shape, same pass), not already consumed, not covered by its neighbour,
// and not culled against a same-material neighbour in the transparent pass.
// Does a same-material neighbour actually cover this face? This is the transparent pass's
// self-cull — leaf against leaf, water against water — and it is one function with two callers
// so that the merge test and the emit test can never answer it differently.
//
// v1.8.0 task 22b split it off from the single line it used to be. Two blocks of water are the
// same material and are no longer necessarily the same SHAPE, so "same id" stopped being enough:
//
//   * A HORIZONTAL face is covered only while the neighbour's surface is at least as high as
//     this cell's. When the neighbour is shorter, the band between the two surfaces is open air
//     that a player can see straight through, and culling it leaves a hole in the side of the
//     water exactly one step of level tall. The taller cell emits its whole wall — 0 up to its
//     own surface — rather than just the exposed band, because the submerged part of it sits
//     inside a neighbour that is drawn opaque (the pass is alpha-TESTED, not blended, and it
//     writes depth: scene/chunk_render.c), so it is hidden by the neighbour's own outward faces
//     and costs one quad instead of a second, differently-shaped quad type.
//
//   * A VERTICAL face is always covered, and that is provable rather than assumed: a cell with
//     water directly above it is a column interior and world/mesher.c's dropBuild gives it drop
//     0, so its lid is at the cell boundary and so is the floor of the cell above. Neither can
//     be short, so no gap can open between them. The explicit test is what stops a full cell
//     under a SHORT one from emitting a lid that is buried inside that short cell's water.
static inline bool sameMaterialCovers(const MeshScratch* s, int ni, uint8_t id, int face,
                                      uint8_t drop)
{
	if (!s_cell_draw[ni] || s->blocks[ni] != id) return false;
	if (!s_any_drop) return true;
	if (face == FACE_TOP || face == FACE_BOTTOM) return true;
	return s_cell_drop[ni] <= drop;
}

static inline bool mergeCandidate(const MeshScratch* s, int nb, int face, uint8_t id,
                                  bool deferred, uint8_t drop)
{
	if (s->blocks[nb] != id) return false;
	if (s_face_done[nb] & (uint8_t)(1u << face)) return false;

	// v1.8.0 task 22b. Same block id is no longer the same GEOMETRY: two flowing water cells at
	// different levels are both BLOCK_WATER and are different heights, and one quad cannot be
	// two heights. Without this a level-6 cell beside a level-3 one would merge and the whole
	// run would be drawn at whichever height the first cell happened to have — a step in the
	// surface silently flattened, with the face count and the coverage test both still perfect.
	if (cellDrop(nb) != drop) return false;

	const int ni = nb + s_plan[face].neighbour;
	if (s_cell_occl[ni]) return false;
	if (deferred && sameMaterialCovers(s, ni, id, face, drop)) return false;
	return true;
}

// The shading signature of one face, or 0 if it may not be merged at all.
//
// Non-zero means "all four corners carry the same AO, and the same light", and the value
// identifies which. Two faces merge iff their keys are equal and non-zero. The bit above
// the payload is what keeps a legitimate all-zero corner (AO 0, light 0 — a fully dark
// crevice) from reading as "unmergeable".
static uint32_t faceFlatKey(const MeshScratch* s, int si, int face, bool lit)
{
	const FacePlan* p  = &s_plan[face];
	const uint8_t   ao = cornerAO(si, &p->corner[0]);

	for (int i = 1; i < 4; i++)
		if (cornerAO(si, &p->corner[i]) != ao) return 0;

	uint8_t pad = 0;
	if (lit) {
		pad = cornerLight(s, si, p, &p->corner[0]);
		for (int i = 1; i < 4; i++)
			if (cornerLight(s, si, p, &p->corner[i]) != pad) return 0;
	}

	return 0x10000u | ((uint32_t)ao << 8) | pad;
}

// How far a run starting at this cell may reach: the merge cap, the chunk edge, and then
// the actual cells. Returns 1 when nothing merges, which is the ordinary case and costs
// only the single cheap mergeCandidate() probe — faceFlatKey is not computed at all unless
// there is a geometrically eligible neighbour to merge with.
static int mergeRun(const MeshScratch* s, int si, int uc, int face, uint8_t id,
                    bool deferred, bool lit, uint8_t drop)
{
	const FacePlan* p = &s_plan[face];
	if (!p->u_merge) return 1;

	// The run must stay inside this chunk: cells past the edge belong to a neighbour's
	// mesh and are only in the scratch to be read for culling and AO.
	int max_w = CHUNK_DIM - uc;
	if (max_w > ATLAS_MAX_MERGE_BLOCKS) max_w = ATLAS_MAX_MERGE_BLOCKS;
	if (max_w < 2) return 1;

	if (!mergeCandidate(s, si + p->u_step, face, id, deferred, drop)) return 1;

	const uint32_t key = faceFlatKey(s, si, face, lit);
	if (!key) return 1;

	int w  = 1;
	int nb = si;
	while (w < max_w) {
		nb += p->u_step;
		if (!mergeCandidate(s, nb, face, id, deferred, drop)) break;
		if (faceFlatKey(s, nb, face, lit) != key) break;
		w++;
	}
	return w;
}

// One quad covering `width` blocks along u, starting at the cell emitFace would have used.
//
// emitFace itself is untouched — it is the piece the console's vertex format and the byte
// anchor in world_test.c both rest on, and every merged quad is still exactly the four
// vertices and six indices it writes. Only two things differ, and both are applied from
// outside it: the tile rect handed in has u1 stretched to 16*width (GPU_REPEAT tiles the
// art across the span), and the two corners emitFace marked u_hi are pushed width-1 blocks
// further along u. Which corners those are is read from the same CornerPlan emitFace used,
// so the geometry and the UVs cannot disagree.
static void emitFaceRun(MeshOut* o, const MeshScratch* s, int si, int lx, int ly, int lz,
                        int face, const AtlasRect* r, bool lit, int width, uint8_t drop)
{
	if (width <= 1) {
		emitFace(o, s, si, lx, ly, lz, face, r, lit, drop);
		return;
	}

	AtlasRect wide = *r;
	wide.u1 = (uint8_t)(r->u0 + TILE_PX * width);

	const uint32_t base = o->vert_count;
	emitFace(o, s, si, lx, ly, lz, face, &wide, lit, drop);
	if (o->vert_count != base + 4) return;   // refused for want of room; nothing to widen

	const FacePlan* p = &s_plan[face];
	for (int i = 0; i < 4; i++) {
		if (!p->corner[i].u_hi) continue;
		MeshVertex* v = &o->verts[base + i];
		v->x = (int8_t)(v->x + p->ux * (width - 1));
		v->y = (int8_t)(v->y + p->uy * (width - 1));
		v->z = (int8_t)(v->z + p->uz * (width - 1));
	}
}

// ── BLOCK_SHAPE_CROSS ────────────────────────────────────────────────────────
//
// Two quads on the cell's two diagonals, forming an X — the shape a plant is drawn as.
//
// Corner to corner, with no inset. That is not a look decision, it is the vertex format:
// MeshVertex positions are int8_t in whole block units (world/mesh_vertex.h), and that
// header is not to be changed, so the only positions a quad can have are the cell's own
// integer corners. A narrower X would need fractional coordinates the format cannot
// express.
//
// u_hi/v_hi select which end of the tile rect each corner takes, exactly as CornerPlan
// does for a cube face: the two corners at one end of the diagonal take u0, the two at
// the other take u1, and the top pair takes v1. So every UV this emits is a corner of
// the block's own AtlasRect and can never stray into the neighbouring 20px cell.
typedef struct {
	int8_t  x, y, z;
	uint8_t u_hi, v_hi;
} CrossCorner;

static const CrossCorner kCross[2][4] = {
	// Plane A: the (x=0,z=0) → (x=1,z=1) diagonal.
	{ {0,0,0, 0,0}, {1,0,1, 1,0}, {1,1,1, 1,1}, {0,1,0, 0,1} },
	// Plane B: the (x=1,z=0) → (x=0,z=1) diagonal.
	{ {1,0,0, 0,0}, {0,0,1, 1,0}, {0,1,1, 1,1}, {1,1,0, 0,1} },
};

// Four quads, not two: each plane is emitted twice, the second time with its corners
// walked backwards so the triangles wind the other way.
//
// The renderer culls back faces for the whole world pass and never turns it off —
// scene/chunk_render.c sets C3D_CullFace(GPU_CULL_BACK_CCW) once and the transparent
// pass only adds the alpha test — so a single-sided X would be invisible from three of
// the four horizontal quadrants. Disabling culling around the transparent run instead
// would double the overdraw of every leaf in every canopy to fix plants, which is the
// wrong trade on this hardware; and it would make the plant's visibility depend on my
// getting the handedness of a diagonal quad right in a build nobody can look at.
// Emitting both windings is correct whichever way round the projection turns out to be.
#define CROSS_QUADS  4

// Cross quads carry FACE_TOP as their normal index, so world.v.pica's faceShade lookup
// gives them 1.00 — full brightness, the same as a grass top. Any other choice shades
// the two planes of one plant differently from each other, or shades a plant darker than
// the ground it stands on, and neither is a thing a plant does.
#define CROSS_NRM  ((uint8_t)FACE_TOP)

// AO is fixed at 3 (unoccluded) for the same reason the shape is not solid: an X is not
// a wall, so there are no crevices at its corners to darken. Light, when the engine is
// on, is the cell's own — a cross never occludes, so its cell always carries the light
// that reaches it.
static void emitCross(MeshOut* o, const MeshScratch* s, int si, int lx, int ly, int lz,
                      const AtlasRect* r, bool lit)
{
	if (o->vert_count + 4 * CROSS_QUADS > o->vert_cap ||
	    o->index_count + 6 * CROSS_QUADS > o->index_cap) {
		o->overflow = true;
		return;
	}

	for (int plane = 0; plane < 2; plane++) {
		for (int back = 0; back < 2; back++) {
			const uint16_t base = (uint16_t)o->vert_count;

			for (int i = 0; i < 4; i++) {
				const CrossCorner* c = &kCross[plane][back ? 3 - i : i];
				MeshVertex*        v = &o->verts[o->vert_count++];

				v->x = (int8_t)(lx + c->x);
				v->y = (int8_t)(ly + c->y);
				v->z = (int8_t)(lz + c->z);

				v->u   = c->u_hi ? r->u1 : r->u0;
				v->v   = c->v_hi ? r->vslot1 : r->vslot0;
				v->nrm = CROSS_NRM;
				v->ao  = 3;
				v->pad = lit ? s->light[si] : 0;
			}

			// The same six values per quad emitFace writes, in the same order — see the
			// comment on MeshSlot in scene/chunk_render.c, which builds ONE shared index
			// buffer for every slot on the strength of that pattern holding for every
			// quad any pass emits.
			o->indices[o->index_count++] = base;
			o->indices[o->index_count++] = (uint16_t)(base + 1);
			o->indices[o->index_count++] = (uint16_t)(base + 2);
			o->indices[o->index_count++] = base;
			o->indices[o->index_count++] = (uint16_t)(base + 2);
			o->indices[o->index_count++] = (uint16_t)(base + 3);

			o->faces++;
		}
	}
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

// v1.7.1 task 49. Which of a listed cell's six faces is NOT covered by the neighbour it looks
// at, one bit per FACE_* index, parallel to s_emit by list position. Written once here, read
// by the seven walks in meshPass instead of each of them re-reading s_cell_occl at its own
// neighbour offset.
//
// This is the measured fix for task 49, so the reasoning belongs next to it. The opaque
// geometry is emitted face-major (see meshChunk), which means the emit list is walked SIX
// times, and until this task every one of those walks visited every drawable cell in the
// chunk — 4,096 of them in buried rock — only to discover for 22 of every 24 faces that the
// neighbour covers it. A host profile over 719 real generated chunks (seed 1337,
// GEN_VERSION_DENSITY, scratchpad/t49m_meshprofile.c) put 86 % of meshChunk's time inside
// those six walks, and showed the cost tracking the number of CELLS rather than the number of
// quads actually emitted: chunks holding 3,584-4,096 drawable cells averaged 83.5 us in the
// opaque passes while emitting 239 quads, and chunks holding 2,048-2,559 cells averaged
// 69.4 us while emitting 734. Three times the geometry for less time — so what was being paid
// for was the traversal, not the emission.
//
// Two things follow, and both are done below. A cell with no exposed face at all can never
// emit anything from any of the six walks, so it is not put on the list; in buried rock that
// is the overwhelming majority of it. And a cell that IS on the list carries its answer as a
// bit, so the walk for one direction tests a byte it is already streaming through rather than
// making an indirect load into the 5,832-byte occlusion table at a ±1 / ±18 / ±324 offset.
//
// A parallel byte array rather than a sixth field in EmitCell, deliberately. EmitCell is
// 6 bytes and padding a seventh out to 8 would take s_emit from 24 KB of .bss to 32 KB on a
// console where that is real memory. More to the point, most iterations of a per-direction
// walk do not want the EmitCell at all — they want to look at one bit and move on — so
// keeping the bits in their own array means such an iteration touches one byte instead of
// six, and both arrays are still walked strictly forwards, which is what the cache wants.
//
// 4 KB of .bss, on top of s_emit's 24 KB, static for the same reason that one is.
static uint8_t  s_emit_face[CHUNK_BLOCKS];

// Every face uncovered: what a cell is given when the six-neighbour test does not decide
// whether it emits. See the non-cube case in emitCollect.
#define EMIT_ALL_FACES  ((uint8_t)((1u << BLOCK_FACES) - 1))

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
//
// Step 9.4b: the inner loop below reads a chunk row four cells at a time, which only lines up
// if a row is a multiple of 4 wide. Checked at compile time so a future change to CHUNK_DIM
// fails the build instead of silently scanning past the row.
_Static_assert(CHUNK_DIM % 4 == 0, "step 9.4b word scan needs a chunk row divisible by 4");

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

			// Step 9.4b. BLOCK_AIR == 0 and s_cell_draw[BLOCK_AIR] is false by
			// construction, so four consecutive s_cell_draw bytes reading back as the
			// word 0 mean four consecutive cells that draw nothing at all, and nothing
			// in a run of four cells like that can ever emit. A 16-wide chunk row is
			// exactly four uint32_t words, so testing the word first turns "4 byte loads
			// + 4 branches" into "1 word load + 1 branch" for every such run, which is
			// most of an open-sky chunk.
			//
			// v1.6.0 task 13 moved this off s_cell_solid. The two tables agree on every
			// block in the registry today — air draws nothing and everything else is a
			// solid cube — so the scan skips exactly the runs it has always skipped; what
			// changes is that a block which draws without colliding (a plant now, water
			// next) reaches the emit list instead of being passed over as "not solid".
			//
			// This is the loop that SCANS FOR drawable cells to build the emit list — the
			// only place in this file eligible for an air-skip. meshPass() below tests
			// a single neighbour cell per face, not a run, so there is nothing to skip
			// four of; skipping there would also risk stepping over the exact
			// transparent-neighbour check step 9.1 added. Not touched.
			//
			// (The array the cast note below is about is s_cell_draw now, not
			// s_cell_solid; both are plain uint8_t[SCRATCH_BLOCKS] and the argument is
			// unchanged in every other respect.)
			//
			// si is NOT provably 4-byte aligned: it is scratchIndex(1, ly+1, lz+1) + lx,
			// and SCRATCH_DIM (18, world/scratch.h:17) is not a multiple of 4, so the
			// row's starting address cycles through all four byte phases as ly and lz
			// vary — there is no fixed alignment to assert. A `(uint32_t*)&s_cell_solid[si]`
			// cast would be an unaligned load the ARM11 is not guaranteed to tolerate.
			// memcpy's semantics don't depend on alignment at all: GCC lowers a
			// fixed-size memcpy to a single load when it can prove one is safe for the
			// target and to a safe byte sequence when it can't, so this is correct
			// either way without knowing the CPU's unaligned-access policy at compile
			// time. s_cell_solid itself (world/mesher.c) is a plain uint8_t[]; nothing
			// about its declaration promises 4-byte alignment either, which is exactly
			// why the row offset can't be trusted and memcpy is the right tool, not a
			// cast plus an alignment attribute.
			for (int lx = 0; lx < CHUNK_DIM; lx += 4, si += 4) {
				uint32_t word;
				memcpy(&word, &s_cell_draw[si], sizeof(word));
				if (word == 0) continue;   // four cells that draw nothing

				for (int k = 0; k < 4; k++) {
					const int sik = si + k;
					if (!s_cell_draw[sik]) continue;

					const BlockId id = s->blocks[sik];

					// v1.7.1 task 49. The cell's exposure, decided once here instead of six
					// times over in the walks below — see s_emit_face for the measurement
					// that made this the shape of the fix.
					//
					// The six offsets are s_plan[face].neighbour, read from the same table
					// meshPass reads, so the two can never disagree about which cell a face
					// looks at. They are not spelled out as ±1 / ±SCRATCH_DIM / ±SCRATCH_DIM²
					// here for exactly that reason: kFaces is the source of truth for face
					// geometry and this must not become a second copy of it.
					uint8_t mask = 0;
					for (int face = 0; face < BLOCK_FACES; face++)
						if (!s_cell_occl[sik + s_plan[face].neighbour])
							mask |= (uint8_t)(1u << face);

					// A shape that is not a full cube is emitted whole by emitCross and never
					// through the six-face loop, so no neighbour can hide it and the mask
					// above is not the question for it. It is listed unconditionally with
					// every bit set: a plant walled in on all six sides still draws, exactly
					// as it did before this task, and a mask of 0 must not be allowed to drop
					// it off the list.
					const bool cube = s_shape[id] == BLOCK_SHAPE_FULL_CUBE;
					if (cube) {
						// Nothing exposed: every one of the six walks would reach this cell,
						// test the neighbour, and skip. Leaving it off the list is exact, not
						// an approximation — mergeRun cannot pull it back in either, because
						// mergeCandidate applies the same s_cell_occl test to a run's next
						// cell before absorbing it.
						if (!mask) continue;
					} else {
						mask = EMIT_ALL_FACES;
					}

					const int   at = s_deferred[id] ? --alpha : opaque++;
					EmitCell*   e  = &s_emit[at];

					s_emit_face[at] = mask;
					e->si = (uint16_t)sik;
					e->id = (uint8_t)id;
					e->lx = (uint8_t)(lx + k);
					e->ly = (uint8_t)ly;
					e->lz = (uint8_t)lz;
				}
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
static void meshPass(MeshOut* out, const MeshScratch* s, const EmitCell* cells,
                     const uint8_t* masks, int n,
                     bool deferred, int face_first, int face_end, bool lit)
{
	// v1.7.1 task 49. Which bits of a cell's s_emit_face byte this pass could possibly act
	// on — one bit for an opaque pass, all six for the transparent one. Built once, outside
	// the loop, so the rejection below is a single AND against a value already in a register.
	uint8_t want = 0;
	for (int face = face_first; face < face_end; face++)
		want |= (uint8_t)(1u << face);

	for (int k = 0; k < n; k++) {
		// Forwards through the opaque list, backwards through the transparent one — which
		// is the SAME spatial order, because emitCollect fills the transparent half from the
		// back of the array and it therefore comes out reversed. Greedy merging needs the
		// low-u end of a run to be reached first (a run that starts in the middle is still
		// correct, it just splits into more quads than it had to), and the collect order —
		// y, then z, then x — puts the smallest coordinate on every axis first. Walking the
		// transparent half in list order would have met every run from its far end and
		// merged almost nothing. Emission order inside the transparent run is free: it is
		// one alpha-tested cutout draw, order-independent within itself.
		const int idx = deferred ? n - 1 - k : k;

		// v1.7.1 task 49, and this is the whole point of the step: a cell with nothing
		// exposed in the direction THIS pass emits costs one byte load and one test, and the
		// six-byte EmitCell beside it is never touched. An opaque pass over a buried chunk is
		// almost entirely this branch. Correctness is unchanged — the bit is exactly the
		// `!s_cell_occl[si + s_plan[face].neighbour]` the loop below used to compute for
		// itself, written once by emitCollect from the same s_plan table.
		const uint8_t fm = masks[idx];
		if (!(fm & want)) continue;

		const EmitCell* e  = &cells[idx];
		const int       si = e->si;

		// v1.8.0 task 22b. This cell's water surface height, once per cell rather than once per
		// face: it is the same for all six, and when nothing in the scratch is flowing —
		// which is every chunk of every world that has not been dug into — cellDrop() answers 0
		// without reading the table at all.
		const uint8_t   drop = cellDrop(si);

		// Drawable implies a real registry row, since s_draws says no for every id with
		// no row — so s_rect is safe to index without a check.
		const AtlasRect* rects = s_rect[e->id];

		// Non-cube geometry. It has no faces to bucket and no neighbour that can hide it,
		// so it skips the whole six-face loop below and is emitted whole, exactly once.
		//
		// `deferred` is what guarantees the "exactly once": the opaque run is walked six
		// times, once per face direction, and planBuild forces every non-cube shape into
		// the deferred list, which is walked once. Testing it here rather than trusting
		// that is what keeps a mis-set flag a missing plant instead of six stacked copies
		// of one.
		if (s_shape[e->id] != BLOCK_SHAPE_FULL_CUBE) {
			if (deferred) {
				// One tile for the whole shape, the block's FACE_EAST entry — the same
				// slot blockFaceTex() falls back to for a face that does not exist. A
				// cross block's def should set all six to it.
				// Crosses are never water and never short, so they carry no drop and their
				// nrm byte is the bare CROSS_NRM it always was.
				emitCross(out, s, si, e->lx, e->ly, e->lz, &rects[FACE_EAST], lit);
			}
			continue;
		}

		for (int face = face_first; face < face_end; face++) {
			// A face is hidden only by a neighbour that actually covers it. That is the
			// test which removes 22 of every 24 faces in solid rock, and it reads the
			// border of the scratch, so chunk seams are culled against the real neighbour
			// instead of guessing air.
			//
			// Since v1.7.1 task 49 it is read out of the cell's exposure byte rather than
			// out of s_cell_occl at the neighbour's offset. Same answer, from the same
			// table, computed once per cell by emitCollect instead of once per cell per
			// walk — see s_emit_face. It stays first because it is now a test against a
			// value already in hand, so it is the cheapest of the three.
			if (!(fm & (uint8_t)(1u << face))) continue;

			// Already emitted, as the middle of a run some earlier cell started. One byte
			// load, and the only test that can be true for a cell that would otherwise pass
			// every check below.
			if (s_face_done[si] & (uint8_t)(1u << face)) continue;

			// ...and, in the transparent pass only, by a neighbour of the same material.
			// This is what keeps a canopy cheap: leaf against leaf is culled, so the pass
			// draws the shell of a tree and not every leaf inside it.
			//
			// The two halves are deliberately not one test on `solid`. Culling on solidity
			// alone is what hid the trunk faces inside a canopy — see
			// testMesherOpaqueBehindTransparent.
			//
			// s_cell_draw, not s_cell_solid: the pair of blocks this rule is about is
			// "two of the same thing touching", which is a question about geometry and
			// not about collision. Identical today — leaves are the only transparent
			// block and they are solid — and it is what will keep a body of water from
			// meshing every internal face when water arrives non-solid.
			//
			// The neighbour index is resolved inside this branch since task 49, because the
			// occlusion test above no longer needs it and the opaque pass — six of the seven
			// walks — never reaches here at all.
			//
			// Since v1.8.0 task 22b the "same material" half is sameMaterialCovers(), because
			// two flowing water cells of different level are the same id and different heights
			// — see that function for why culling between them opens a hole and why the taller
			// one emits its whole wall.
			if (deferred) {
				const int ni = si + s_plan[face].neighbour;
				if (sameMaterialCovers(s, ni, e->id, face, drop)) continue;
			}

			// v1.6.0 task 11. How many blocks along u this face can swallow, and then one
			// quad for the lot. width 1 is the ordinary case and emits exactly what the
			// pre-task mesher emitted, byte for byte.
			const FacePlan* p  = &s_plan[face];
			const int       uc = p->u_axis == 0 ? e->lx : (p->u_axis == 1 ? e->ly : e->lz);
			const int       w  = mergeRun(s, si, uc, face, e->id, deferred, lit, drop);

			emitFaceRun(out, s, si, e->lx, e->ly, e->lz, face, &rects[face], lit, w, drop);

			// Claim the cells behind the first one. The first needs no mark: this walk is
			// past it and will not return to it.
			for (int j = 1, nb = si; j < w; j++) {
				nb += p->u_step;
				s_face_done[nb] |= (uint8_t)(1u << face);
			}
		}
	}
}

// ── v1.8.0 task 22b: how tall each water cell is drawn ───────────────────────
//
// Turns the scratch's water band (world/scratch.h — 0 for a full cube, 1..7 for a flow level)
// into the per-cell drop the emitters read. Two rules, and the second is the one that makes a
// waterfall a waterfall:
//
//   * A flow cell of level L has its surface L/8 of a block up, so it drops 8-L.
//   * A cell with WATER DIRECTLY ABOVE IT is a column interior and is drawn full height, level
//     or no level. This is not a tweak for looks. water.c gives every cell fed from above
//     WATER_LEVEL_MAX (7), not 8 — "a waterfall does not weaken" — so without this rule every
//     cell of a falling column would draw an eighth of a block short and the column would be a
//     stack of separated slabs with daylight between them. It is also what lets the vertical
//     self-cull stay unconditional: see sameMaterialCovers.
//
// The band's top plane is left at 0 because there is no cell above it to ask about, and nothing
// in this chunk's geometry reads it — it is a skirt cell one block above the chunk's lid.
static void dropBuild(const MeshScratch* s)
{
	s_any_drop = false;
	if (!s->water_any) return;

	memset(s_cell_drop, 0, sizeof s_cell_drop);

	const int above = SCRATCH_DIM * SCRATCH_DIM;
	for (int i = 0; i < SCRATCH_BLOCKS - above; i++) {
		const uint8_t lvl = s->water[i];
		if (!lvl) continue;

		// A level for a cell that is not water any more. The band is a snapshot and the world
		// it describes can have moved on — waterDropColumn clears a column's flow cells, and a
		// placed block can sit where one was — so the block array is the authority on what is
		// there and the band only says how tall it is.
		if (s->blocks[i] != (BlockId)BLOCK_WATER) continue;
		if (s->blocks[i + above] == (BlockId)BLOCK_WATER) continue;

		s_cell_drop[i] = (uint8_t)(SCRATCH_WATER_STEPS - lvl);
		s_any_drop = true;
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
		s_cell_draw[i]  = s_draws[id];
	}

	// v1.6.0 task 11. No face has been claimed by a greedy run yet. One memset of 5,832
	// bytes per chunk, against the ~24,000 face tests the passes below are about to do.
	memset(s_face_done, 0, sizeof s_face_done);

	// v1.8.0 task 22b. How tall each water cell is drawn. One branch and nothing else for a
	// scratch with no flowing water in it, which is every chunk until a player digs into a sea.
	dropBuild(s);

	// Which cells emit at all, and which half of the frame each belongs to. One sweep for both.
	emitCollect(s);

	// Step 9.2. The opaque run is laid out face-major: six walks of the emit list, one per face
	// direction, so each direction ends up as one contiguous stretch of indices the renderer
	// can draw or skip whole. It skips the three directions that point away from the camera,
	// which is about half the opaque vertices of every chunk on screen, every frame, per eye.
	//
	// The buckets go out in kFaceOrder, not enum order, so the ones the renderer keeps tend to
	// land next to each other and merge into fewer draw calls.
	const bool lit = lightEnabled();
	out->face_start[0] = 0;
	for (int slot = 0; slot < BLOCK_FACES; slot++) {
		const int face = kFaceOrder[slot];
		meshPass(out, s, s_emit, s_emit_face, s_emit_opaque_n, false, face, face + 1, lit);
		out->face_start[slot + 1] = out->index_count;
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
	// The exposure bytes are parallel to s_emit by list position, so the transparent half's
	// slice of them starts at the same index its cells do.
	meshPass(out, s, &s_emit[s_emit_alpha_lo], &s_emit_face[s_emit_alpha_lo],
	         CHUNK_BLOCKS - s_emit_alpha_lo, true, 0, BLOCK_FACES, lit);
}
