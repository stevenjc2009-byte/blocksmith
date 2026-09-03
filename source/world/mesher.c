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
//
// Step 9.1. Whether each cell *occludes* — solid and opaque. Solidity alone was the test until
// the 2026-08-18 audit, and it culled the face between an opaque block and a see-through one:
// the stone under a leaf, and the trunk inside its own canopy, lost the faces pointing at the
// leaves and showed sky through the alpha holes instead. A second table rather than a second
// condition on the block id, for the same reason solidity is a table: this is read once per
// face, about 24,000 times a chunk, and it has to stay a single byte load.
//
// v1.6.0 task 13. Whether each cell emits geometry at all. Until this task that was the
// same question as solidity — every block that was not air was solid — and emitCollect's
// scan below read the solidity table for it. It is its own question now because the shapes
// this task exists for split the two apart: a plant draws and does not collide, and so will
// water. Air answers no to all three here exactly as it always has, so the four-at-a-time
// word scan below is unaffected.
//
// ── v1.8.6: one packed table instead of three ────────────────────────────────
//
// Until this task the three questions above were three separate uint8_t[SCRATCH_BLOCKS]
// arrays — s_cell_solid, s_cell_occl, s_cell_draw — each resolved from the block id in the
// same loop in meshChunk and each read back from a different address by the callers below.
// That is 3 * 5,832 = 17,496 bytes of .bss for three answers that are decided together, from
// the same BlockId, in the same loop, and never disagree about which cell they describe.
//
// Packed into one table the three answers are three bits of one byte, chosen so cornerAO's
// arithmetic below needs no shift: CELL_FLAG_SOLID is bit 0, so `flags & CELL_FLAG_SOLID` is
// already exactly 0 or 1, the same int cornerAO summed straight out of s_cell_solid before.
// .bss for the merged table is 5,832 bytes — a third of the three separate arrays — and every
// caller below reads one byte and masks it instead of reading one of three byte arrays,
// which is either the same cost (a caller that only ever asked one question) or cheaper (any
// caller — none exist today — that wants more than one answer for the same cell).
//
// The one place this had to be done carefully rather than just swapped in is emitCollect's
// four-at-a-time word scan a little further down: the old code tested `word == 0`, true only
// when four consecutive s_cell_draw bytes were all zero. Four consecutive packed bytes read as
// a zero word only when SOLID, OCCL and DRAW are all clear for all four cells — a strictly
// narrower condition — so the scan now masks the word down to just the four DRAW bits before
// comparing: `(word & CELL_DRAW_WORD) == 0`. That is exact for any future registry, not merely
// exact for today's, where solid and occluding always implies drawn — see the scan's own
// comment for why the narrower literal test would have been a coincidence dressed as a proof.
//
// Considered and rejected: packing only two of the three (leaving s_cell_draw on its own
// array) so the word-scan needed no change at all. Rejected because it saves 5,832 bytes
// instead of 11,664 for the same amount of new code, and the masked comparison the full merge
// needs is one AND more than the two-table version would have needed anyway — the safer
// version is not the more expensive one here.
//
// Verified hash-identical over 408 synthetic chunks across 12 seeds and 5 fixtures (buried
// stone, surface terrain with water, an exact checkerboard, sparse cross-shape scatter, and a
// leaf canopy) plus the four hashes world/world_test.c already pins — see
// scratchpad/mesher_hashcheck.c in the v1.8.6 session's tooling. Not re-measured on console;
// nothing has run on real 3DS hardware since v1.2.5 (code-vault).
#define CELL_FLAG_SOLID  ((uint8_t)(1u << 0))   // fills its cell: AO and occlusion read this
#define CELL_FLAG_OCCL   ((uint8_t)(1u << 1))   // solid and opaque: hides the face behind it
#define CELL_FLAG_DRAW   ((uint8_t)(1u << 2))   // emits geometry at all
// The DRAW bit, replicated into all four byte lanes of a uint32_t — 0x01010101 times the bit
// value rather than the literal 0x04040404, so the constant stays correct if the bit position
// above ever moves. Byte-lane replication does not care about host endianness: byte i of the
// word is CELL_FLAG_DRAW on every platform, whichever end "byte 0" is.
#define CELL_DRAW_WORD   ((uint32_t)CELL_FLAG_DRAW * 0x01010101u)
static uint8_t  s_cell_flags[SCRATCH_BLOCKS];

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

// OPT-MESHER finding 2 (measured): meshChunk's per-cell loop used to look up three
// flags — solid, occludes, draws — from three separate 256-byte tables, three dependent
// byte loads per cell. Compiling both forms in isolation with the project's real console
// flags and reading the disassembly: the three-table loop body was 15 ARM instructions
// per cell, the one-table form below is 3.5 (GCC unrolls it 8x and merges the byte
// stores into word stores) — about 67,068 fewer instructions per chunk meshed. One byte
// per raw BlockId, already carrying exactly the bits s_cell_flags wants; see meshChunk.
static uint8_t  s_cell_flag_of[256];                // indexed by a raw BlockId byte
// OPT-MESHER finding 7 (same change, same loop): blockFaceTintable() was a per-FACE
// switch, evaluated up to six times per drawn cell by meshPass and once more by
// emitCross. One bit per face, built once per id here instead of switched on at mesh
// time. Bit `face` (FACE_EAST..FACE_NORTH) is set iff blockFaceTintable(id, face).
static uint8_t  s_tint_mask[256];
static uint8_t  s_shape[256];                       // BLOCK_SHAPE_*

// Step 7.5: does this block's geometry belong in the transparent pass? Solid *and*
// transparent — air is transparent and never drawn, so both flags have to be read.
// A byte table for the same reason s_cell_flag_of is one: the alternative is a call into
// block.c per cell, and there is no link-time optimisation in this build.
static uint8_t  s_deferred[256];
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

// Forward-declared so planBuild (below) can call it while building s_tint_mask — its own
// definition, with the history of why each id/face answer is what it is, stays where it
// was; only the number of call sites changes (OPT-MESHER finding 7).
static bool blockFaceTintable(BlockId id, int face);

// Builds the lookup tables. Runs once, on the first chunk meshed — and again
// after mesherInvalidateTables(), since dynamic registrations can land between
// joins. kFaces never changes; the block tables can.
static void planBuild(void)
{
	// A raw id, not a validated one. blockInfo() already maps an unknown id to air, so
	// this table answers for all 256 byte values and the mesher never bounds-checks.
	for (int i = 0; i < 256; i++) {
		const BlockInfo* info  = blockInfo((BlockId)i);
		const bool       cube  = info->shape == BLOCK_SHAPE_FULL_CUBE;
		const bool       drawn = blockIsDrawn((BlockId)i);   // see s_cell_flag_of below

		s_shape[i] = info->shape;

		// v1.6.0 task 13 narrowed the three terms below by one term each, and every one
		// of those terms is false for nothing in the registry as it stands — all eight
		// core rows are solid-or-air full cubes, so all three come out byte-for-byte what
		// they were. What each term buys:
		//
		//   solid      "fills its cell", which is what the AO taps mean by solid. A shape
		//              that only crosses its cell diagonally is not a crevice wall, so a
		//              non-cube must never darken the corners of its neighbours even if
		//              something registers it solid.
		//   s_deferred which of the two passes it belongs in. Non-cube geometry has no
		//              per-face buckets, and the opaque run is emitted face-major (six
		//              walks, one per direction) — so a non-cube in the opaque list would
		//              be emitted six times over. Forcing it deferred is structural, not
		//              a stylistic choice about alpha.
		//   occludes   what may hide the face behind it. This is the one that makes holes
		//              in the world if it is wrong: an X of two quads does not cover the
		//              cell's faces, so it can never cull its neighbour's.
		const bool solid    = info->solid && cube;
		const bool occludes = info->solid && !info->transparent && cube;
		s_deferred[i] = (uint8_t)(drawn && (info->transparent || !cube));

		// OPT-MESHER finding 2: the packed byte meshChunk's per-cell loop reads instead of
		// three separate table lookups. solid/occludes are read ONLY here (re-verified by
		// grep across the whole repo — both are `static`, so their scope is this file, and
		// nothing outside this loop names either); drawn was also read at s_deferred just
		// above, which is the only other place it was needed.
		s_cell_flag_of[i] = (uint8_t)((solid    ? CELL_FLAG_SOLID : 0) |
		                               (occludes ? CELL_FLAG_OCCL  : 0) |
		                               (drawn    ? CELL_FLAG_DRAW  : 0));

		// OPT-MESHER finding 7: one bit per face instead of a per-face switch evaluated at
		// mesh time. blockFaceTintable() is pure and takes only (id, face), so building all
		// six answers once per id here is exactly the six answers the old call sites got,
		// just asked in advance.
		uint8_t tint_mask = 0;
		for (int face = 0; face < BLOCK_FACES; face++)
			if (blockFaceTintable((BlockId)i, face)) tint_mask |= (uint8_t)(1u << face);
		s_tint_mask[i] = tint_mask;
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

// Exact floor division by a count known to be 1..5, without the libgcc
// __aeabi_idivmod call the ARM11 would otherwise pay per corner per channel.
// Sums never exceed 15*4 = 60, where (n * 171) >> 9 is exact for /3.
//
// v1.8.16 adds case 5 for crossCornerLight below, which counts one tap more than cornerLight
// does. Sums there never exceed 15*5 = 75, and (n * 205) >> 10 is exact for /5 over 0..75: the
// error against n/5 is n/5120, at most 0.0147 at n = 75, far short of the 0.2 gap between one
// fifth and the next. Cases 1..4 are untouched, so every existing caller's arithmetic is
// bit-for-bit what it was — see the pinned lit cube-only hash in tests/cross_light_test.c.
static inline int avgByCnt(int sum, int cnt)
{
	switch (cnt) {
	case 2:  return sum >> 1;
	case 3:  return (sum * 171) >> 9;
	case 4:  return sum >> 2;
	case 5:  return (sum * 205) >> 10;
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
	// CELL_FLAG_SOLID is bit 0 (see s_cell_flags), so masking it out of the packed byte is
	// already exactly 0 or 1 — the same int this arithmetic summed straight out of the old
	// s_cell_solid array, no shift needed.
	const int s1 = s_cell_flags[si + c->ao1] & CELL_FLAG_SOLID;
	const int s2 = s_cell_flags[si + c->ao2] & CELL_FLAG_SOLID;
	return (s1 && s2) ? 0 : (uint8_t)(3 - s1 - s2 - (s_cell_flags[si + c->aoc] & CELL_FLAG_SOLID));
}

// ── v1.8.8 biome tint ────────────────────────────────────────────────────────
//
// Which faces the biome is allowed to colour. Everything else emits MESH_TINT_NONE and comes
// out of this mesher byte-for-byte what it came out before.
//
// The grass block's TOP only, and the two cross-shaped plants whole. That is a deliberate
// restriction and not an oversight: BTEX_GRASS_SIDE is ONE tile carrying a grass crust over
// dirt (world/registry.c gives grass GRASS_SIDE on all four sides and DIRT on the bottom), and
// this hardware has no fragment shader and no per-texel mask, so tinting that face tints the
// dirt half of it with the grass half. Minecraft solves that with a separate masked overlay
// sampled from a second texture unit; doing it here would need a new atlas tile and a second
// texcoord varying, i.e. redrawing the block. The half-grass/half-dirt tile is to be kept
// exactly as it is, so its sides stay untinted and only its lid takes the biome's colour.
//
// LEAVES ARE NOT IN THIS LIST even though Minecraft tints them. Nobody asked for it; adding
// BLOCK_LEAVES here is a one-line change if it is ever wanted.
//
// BLOCK_DIRT (v1.8.17) is tinted on ALL SIX faces, and that is not the same restriction grass
// needed. world/registry.c gives dirt BTEX_DIRT on every face -- one uniform texture, not shared
// with any other block's crust the way BTEX_GRASS_SIDE is -- so there is no half-tile to protect
// and no reason to hold any face back. steve asked for this by name: "biome identity via TINT,
// Minecraft-style," the same list as wood/leaves/grass/flowers with dirt as the one gap left in
// it (v1.8.8 shipped the rest). The tint band this reads is per COLUMN with no y term -- see
// faceTint()/scratchColumnOf() below -- so this colours every dirt face at every depth in a
// tinted column, including underground walls, and that is a consequence of the existing per-
// column design rather than a new kind of read.
//
// BLOCK_TALL_GRASS_TOP was MISSING here from v1.8.8 until v1.8.16, and the way it failed is
// worth keeping. The two-block clump is one plant written as two stacked cross blocks
// (world/worldgen.c puts TALL_GRASS at y and TALL_GRASS_TOP at y+1), so the tip fell through
// to `default` and emitted MESH_TINT_NONE — palette row 0, {1.00,1.00,1.00}. Row 0 is
// byte-identical to row 3, plains, which is why nobody caught it for eight versions: in plains,
// and only in plains, the bug is invisible. Everywhere else the tinted lower half and the
// untinted plains-coloured tip read as two different plants stacked on each other, which is
// what steve reported — "on top of a biome specific type of grass, there is another piece of
// grass that is not for that biome."
//
// A test fixture in plains would still pass with this line deleted. biome_tint_test.c's
// testTwoBlockClumpIsOneColour uses jungle for that reason.
static bool blockFaceTintable(BlockId id, int face)
{
	switch ((int)id) {
	case BLOCK_GRASS:      return face == FACE_TOP;
	case BLOCK_DIRT:
	case BLOCK_TALL_GRASS:
	case BLOCK_TALL_GRASS_TOP:
	case BLOCK_FERN:       return true;
	default:               return false;
	}
}

// The tint band is indexed per column, so a cell index has to lose its y.
//
// si = sy*(18*18) + sz*18 + sx, so si modulo 18*18 is exactly sz*18 + sx — which is
// scratchColumn(sx, sz) (world/scratch.h) with no division by 18 at all. One modulo by a
// compile-time constant, and only on a face that is actually tintable.
static inline int scratchColumnOf(int si)
{
	return si % (SCRATCH_DIM * SCRATCH_DIM);
}

// The palette index a face in cell `si` takes: the column's biome tint if this face is
// tintable and anything filled the band, and MESH_TINT_NONE otherwise.
//
// The `tint_any` half is what makes this free for every caller that predates the feature —
// scratchFill clears the band and leaves the flag false, so a host suite or a console build
// with no biome source wired in never reads the band at all. See MeshScratch in scratch.h.
static inline uint8_t faceTint(const MeshScratch* s, int si, bool tintable)
{
	if (!tintable || !s->tint_any) return MESH_TINT_NONE;
	return (uint8_t)(s->tint[scratchColumnOf(si)] & MESH_TINT_MASK);
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
		if (s_cell_flags[taps[t]] & CELL_FLAG_OCCL) continue;
		ssum += s->light[taps[t]] >> 4;
		bsum += s->light[taps[t]] & 15;
		cnt++;
	}
	return (uint8_t)((avgByCnt(ssum, cnt) << 4) | avgByCnt(bsum, cnt));
}

// ── v1.8.16: the same smooth light, for a quad that has no face ──────────────
//
// Smooth per-corner light for a BLOCK_SHAPE_CROSS vertex. Same average as cornerLight above,
// over the same four outward taps, PLUS the cell's own light as an always-counted fifth.
//
// WHY THE OWN CELL IS IN HERE AND IS NOT ALLOWED TO BE SKIPPED. cornerLight leaves it out
// because a cube face is a boundary: the face is lit by what is on the far side of it, and the
// neighbour cell can never be occluding or the face would not have been emitted, so its count
// is never zero. A cross has neither property. Its quads are a diagonal plane through the
// MIDDLE of the cell, lit by the cell they stand in, and a torch stands on a floor — so all
// four outward taps of one of its BOTTOM corners are occluding stone, cnt comes out 0, and
// avgByCnt(0, 0) returns 0. Without this seed every torch in the game gets a black base, which
// is a worse artefact than the flat lighting this replaces. tests/cross_light_test.c ARM 3 is
// that check, and it is the one to run before "simplifying" this back into cornerLight.
//
// WHICH FOUR OUTWARD TAPS, i.e. the design question a diagonal quad raises. A cross vertex is
// not on a face, but it IS on a real lattice corner of the cell — kCross below places all
// sixteen vertices on the cell's own integer corners, because MeshVertex positions are whole
// block units and cannot express anything else. The eight lattice corners of a cell are exactly
// the four corners of its FACE_BOTTOM plus the four of its FACE_TOP, so the caller walks those
// two FacePlans and this function is handed the very CornerPlan cornerAO/cornerLight would use
// for a lid or a floor. No new table, no second copy of the corner geometry, and the vertical
// half of the gradient — which is the half a torch shows most, since its own cell is the
// brightest thing in the room — comes out of the existing machinery for free.
//
// The horizontal half comes out of it too: a top corner's ao1/ao2/aoc taps are the three cells
// around that corner one step up, so the four top corners of one plant differ from each other
// whenever the light around it does. That is what makes the two ends of one diagonal quad
// disagree (tests/cross_light_test.c ARM 4), which is the whole visible effect.
//
// Rejected: gathering the four cells around each corner in the HORIZONTAL plane only. It is
// the same tap count, and it makes the top and bottom of a plant identical by construction —
// a torch would still be a flat sprite, only a differently flat one.
// NOT inline, unlike cornerLight above, and the difference was measured rather than assumed.
// The caller below invokes this eight times from one loop nest, which -O3 unrolls and inlines
// eight copies of: arm-none-eabi-gcc with the Makefile's real flags (-O3 -march=armv6k, see
// tools/run_host_tests.sh's cross_light_test stanza for the command) gave mesher.o .text
// 13,856 B before this task, 16,984 B with it inlined, and 14,856 B with the call left as a
// call — 2,128 bytes of a binary the Makefile itself calls code-size-constrained, for eight
// branch-and-links per cross CELL on the chunk-rebuild path, not on the frame path. What they
// cost: host benchmark meshing a chunk holding 256 plants, gcc -O3, 4,000 meshChunk() calls
// per run, two runs each — inlined 0.0818 and 0.0845 ms/call, called 0.0862 and 0.0877 ms/call,
// with a third inlined run at 0.0887. The spread within one arm is wider than the gap between
// the arms, so the honest reading is "no difference this fixture can resolve", not a number.
// If a future profile on real hardware disagrees, delete the attribute — nothing else changes.
static __attribute__((noinline)) uint8_t crossCornerLight(const MeshScratch* s, int si,
                                                          const FacePlan* p,
                                                          const CornerPlan* c)
{
	const int taps[4] = { si + p->neighbour, si + c->ao1, si + c->ao2, si + c->aoc };
	int ssum = s->light[si] >> 4, bsum = s->light[si] & 15, cnt = 1;
	for (int t = 0; t < 4; t++) {
		if (s_cell_flags[taps[t]] & CELL_FLAG_OCCL) continue;
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
//
// `tint` is the biome palette index this face draws with, MESH_TINT_NONE for everything that
// is not tintable — see blockFaceTintable. It is resolved by the caller rather than here
// because the caller is the one that knows the block id, and because mergeRun has to ask the
// same question before it groups two faces into one quad.
static void emitFace(MeshOut* o, const MeshScratch* s, int si, int lx, int ly, int lz,
                     int face, const AtlasRect* r, bool lit, uint8_t drop, uint8_t tint)
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
		//
		// v1.8.8: the occlusion now shares its byte with the biome tint index, packed by
		// meshAoPack (world/mesher.h). With tint 0 — every face that is not tintable, and
		// every face at all until something fills the scratch's tint band — this is exactly
		// the `cornerAO(si, c)` it replaced, which is why the pinned mesh hashes did not move.
		v->ao  = meshAoPack(cornerAO(si, c), tint);
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

// OPT-MESHER finding 4 measurement. Counts how often mergeRun's result is actually a
// merged run (w >= 2, faceFlatKey's corners computed for the run's first cell and then
// recomputed by emitFace) against an unmerged single face (w == 1). Compiled out of every
// build but the probe, same convention as world/region.c's BS_REGION_PROBE, so the game
// pays nothing for it.
#ifdef BS_MESH_MERGE_PROBE
unsigned long g_mesh_merge_w1;
unsigned long g_mesh_merge_w2plus;
#define MERGE_PROBE_BUMP(w) do { if ((w) >= 2) g_mesh_merge_w2plus++; else g_mesh_merge_w1++; } while (0)
#else
#define MERGE_PROBE_BUMP(w) ((void)0)
#endif

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
	if (!(s_cell_flags[ni] & CELL_FLAG_DRAW) || s->blocks[ni] != id) return false;
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
	if (s_cell_flags[ni] & CELL_FLAG_OCCL) return false;
	if (deferred && sameMaterialCovers(s, ni, id, face, drop)) return false;
	return true;
}

// The shading signature of one face, or 0 if it may not be merged at all.
//
// Non-zero means "all four corners carry the same AO, the same light, and the same biome
// tint", and the value identifies which. Two faces merge iff their keys are equal and
// non-zero. The bit above the payload is what keeps a legitimate all-zero corner (AO 0,
// light 0, tint 0 — a fully dark untinted crevice) from reading as "unmergeable".
//
// Bit layout of the returned word:
//   bits  0..7   the packed corner light, sky<<4 | block, 0 when the engine is off
//   bits  8..15  the corner AO (only 0..3 ever occur, so 10..15 are always clear)
//   bit   16     the "this is a real key" sentinel
//   bits 17..19  v1.8.8, the biome tint palette index
//   bits 20..31  free
//
// ── WHY THE TINT HAS TO BE IN HERE ───────────────────────────────────────────
//
// This is the bug the tint would otherwise have shipped with, and it is silent.
//
// mergeRun below merges coplanar faces of the same block id into a single quad up to
// ATLAS_MAX_MERGE_BLOCKS (15) cells wide, and it merges them iff this key matches. The tint
// is per COLUMN, so two grass tops either side of a biome border are the same block id, the
// same shape, and — on flat open ground, which is where biome borders mostly are — the same
// AO and the same light. Without the tint in this key they hash identically, merge into one
// quad, and that quad takes the four vertices of the FIRST cell in the run. Every cell behind
// it is then drawn in the first cell's biome colour: the border does not fade, it JUMPS, up to
// fifteen blocks away from where the biome actually changes, in whichever direction the run
// happened to be walked.
//
// Nothing else catches it. The face count is unchanged, the coverage is perfect, the AO and
// the light are correct, and every existing test stays green — the geometry is right and only
// the colour is wrong, which is precisely the class of defect this project has been bitten by
// before. See world/biome_tint_test.c, which asserts the border is exact by walking the
// emitted quads, and which was measured red with this line's `tint` term removed.
//
// `tintable` rather than the tint itself: a face that cannot be tinted emits MESH_TINT_NONE
// whatever column it stands in, so folding the raw column value in for stone and dirt would
// split their runs at every biome border for no visual difference at all — a pure meshing
// regression. The tint that goes in the key is the tint that goes in the vertex, and
// faceTint() is the one function that answers both.
static uint32_t faceFlatKey(const MeshScratch* s, int si, int face, bool lit, bool tintable)
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

	return 0x10000u | ((uint32_t)faceTint(s, si, tintable) << 17) | ((uint32_t)ao << 8) | pad;
}

// How far a run starting at this cell may reach: the merge cap, the chunk edge, and then
// the actual cells. Returns 1 when nothing merges, which is the ordinary case and costs
// only the single cheap mergeCandidate() probe — faceFlatKey is not computed at all unless
// there is a geometrically eligible neighbour to merge with.
static int mergeRun(const MeshScratch* s, int si, int uc, int face, uint8_t id,
                    bool deferred, bool lit, uint8_t drop, bool tintable)
{
	const FacePlan* p = &s_plan[face];
	if (!p->u_merge) return 1;

	// The run must stay inside this chunk: cells past the edge belong to a neighbour's
	// mesh and are only in the scratch to be read for culling and AO.
	int max_w = CHUNK_DIM - uc;
	if (max_w > ATLAS_MAX_MERGE_BLOCKS) max_w = ATLAS_MAX_MERGE_BLOCKS;
	if (max_w < 2) return 1;

	if (!mergeCandidate(s, si + p->u_step, face, id, deferred, drop)) return 1;

	const uint32_t key = faceFlatKey(s, si, face, lit, tintable);
	if (!key) return 1;

	int w  = 1;
	int nb = si;
	while (w < max_w) {
		nb += p->u_step;
		if (!mergeCandidate(s, nb, face, id, deferred, drop)) break;
		if (faceFlatKey(s, nb, face, lit, tintable) != key) break;
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
                        int face, const AtlasRect* r, bool lit, int width, uint8_t drop,
                        uint8_t tint)
{
	if (width <= 1) {
		emitFace(o, s, si, lx, ly, lz, face, r, lit, drop, tint);
		return;
	}

	AtlasRect wide = *r;
	wide.u1 = (uint8_t)(r->u0 + TILE_PX * width);

	const uint32_t base = o->vert_count;
	// One tint for the whole run, taken from the run's FIRST cell. That is not an
	// approximation: mergeRun only extended this run across cells whose faceFlatKey matched,
	// and since v1.8.8 the tint is part of that key, so every cell under this quad carries
	// this exact palette index. If it did not, this is where the colour would smear.
	emitFace(o, s, si, lx, ly, lz, face, &wide, lit, drop, tint);
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
// a wall, so there are no crevices at its corners to darken.
//
// v1.8.16: LIGHT is no longer the cell's own flat byte. It was, from v1.5.0 to v1.8.15, and
// that made cross blocks the only geometry in the game still lit the blocky pre-smooth-lighting
// way — most visible on the torch, which is itself the sharpest light gradient in the world.
// Each of the eight distinct lattice corners a cross's sixteen vertices sit on now takes
// crossCornerLight() (see its comment for the gather and why the cell's own light is a fifth,
// always-counted tap), computed ONCE into the small table below and shared by both windings.
//
// v1.8.8: `tint` is the biome palette index, and for a cross it is the whole point — a grass
// strand has to come out the same colour as the grass block it is standing on, or every plant
// in a tinted biome reads as the wrong species. Both planes and both windings take it, so one
// plant is one colour from every angle.
static void emitCross(MeshOut* o, const MeshScratch* s, int si, int lx, int ly, int lz,
                      const AtlasRect* r, bool lit, uint8_t tint)
{
	if (o->vert_count + 4 * CROSS_QUADS > o->vert_cap ||
	    o->index_count + 6 * CROSS_QUADS > o->index_cap) {
		o->overflow = true;
		return;
	}

	// The eight lattice corners of this cell, indexed [y][x][z] exactly as a CrossCorner spells
	// its position. Eight crossCornerLight() calls per plant, not one per vertex: the two
	// planes and the two windings between them write each corner twice over, and a corner's
	// light is a property of the corner, not of the quad that happens to touch it. Filled from
	// the FACE_BOTTOM and FACE_TOP plans, whose four corners each are precisely the cell's four
	// lower and four upper lattice points — px/pz are read back out of the CornerPlan rather
	// than assumed, so this cannot drift from the corner geometry the face path uses.
	//
	// Skipped entirely when the engine is off, which is the only state the Old 3DS build and
	// every host suite that never calls lightEngineInit(true) ever sees: pad stays the literal
	// 0 it has always been there and those meshes are byte-identical.
	// Zero-initialised rather than filled on an else branch: the engine-off path then needs no
	// code at all, and no compiler on either toolchain can decide the table is only
	// conditionally written and warn about it under -Werror.
	uint8_t clight[2][2][2] = {{{0}}};
	if (lit) {
		for (int vy = 0; vy < 2; vy++) {
			const FacePlan* p = &s_plan[vy ? FACE_TOP : FACE_BOTTOM];
			for (int i = 0; i < 4; i++) {
				const CornerPlan* c = &p->corner[i];
				clight[vy][c->px][c->pz] = crossCornerLight(s, si, p, c);
			}
		}
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
				v->ao  = meshAoPack(3, tint);
				v->pad = (int8_t)clight[c->y][c->x][c->z];
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
// by the seven walks in meshPass instead of each of them re-reading the OCCL bit at its own
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

			// Step 9.4b. BLOCK_AIR == 0 and its DRAW bit is clear by construction, so four
			// consecutive cells with the DRAW bit clear are four consecutive cells that draw
			// nothing at all, and nothing in a run of four cells like that can ever emit. A
			// 16-wide chunk row is exactly four uint32_t words, so testing the word first
			// turns "4 byte loads + 4 branches" into "1 word load + 1 branch" for every such
			// run, which is most of an open-sky chunk.
			//
			// v1.6.0 task 13 moved this off solidity. The two questions agree on every block
			// in the registry today — air draws nothing and everything else is a solid cube
			// — so the scan skips exactly the runs it has always skipped; what changes is
			// that a block which draws without colliding (a plant now, water next) reaches
			// the emit list instead of being passed over as "not solid".
			//
			// This is the loop that SCANS FOR drawable cells to build the emit list — the
			// only place in this file eligible for an air-skip. meshPass() below tests
			// a single neighbour cell per face, not a run, so there is nothing to skip
			// four of; skipping there would also risk stepping over the exact
			// transparent-neighbour check step 9.1 added. Not touched.
			//
			// v1.8.6: solidity, occlusion and draw-at-all now live packed in one byte per cell
			// (s_cell_flags, declared above) instead of three separate arrays, so the word read
			// below is no longer "draw, and only draw" for each of the four bytes — a packed
			// byte can be non-zero from SOLID or OCCL alone. The word is masked down to just
			// the four DRAW bits (CELL_DRAW_WORD) before the zero test, which is exact for the
			// same reason the old unmasked test was: BLOCK_AIR's packed byte is 0 in every bit,
			// so a masked word of 0 still means, and only means, "these four cells draw
			// nothing" — it is no longer true only by the coincidence that nothing in today's
			// registry is solid-or-occluding without also drawing.
			//
			// si is NOT provably 4-byte aligned: it is scratchIndex(1, ly+1, lz+1) + lx,
			// and SCRATCH_DIM (18, world/scratch.h:17) is not a multiple of 4, so the
			// row's starting address cycles through all four byte phases as ly and lz
			// vary — there is no fixed alignment to assert. A `(uint32_t*)&s_cell_flags[si]`
			// cast would be an unaligned load the ARM11 is not guaranteed to tolerate.
			// memcpy's semantics don't depend on alignment at all: GCC lowers a
			// fixed-size memcpy to a single load when it can prove one is safe for the
			// target and to a safe byte sequence when it can't, so this is correct
			// either way without knowing the CPU's unaligned-access policy at compile
			// time. s_cell_flags itself (world/mesher.c) is a plain uint8_t[]; nothing
			// about its declaration promises 4-byte alignment either, which is exactly
			// why the row offset can't be trusted and memcpy is the right tool, not a
			// cast plus an alignment attribute.
			for (int lx = 0; lx < CHUNK_DIM; lx += 4, si += 4) {
				uint32_t word;
				memcpy(&word, &s_cell_flags[si], sizeof(word));
				if ((word & CELL_DRAW_WORD) == 0) continue;   // four cells that draw nothing

				for (int k = 0; k < 4; k++) {
					const int sik = si + k;
					if (!(s_cell_flags[sik] & CELL_FLAG_DRAW)) continue;

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
						if (!(s_cell_flags[sik + s_plan[face].neighbour] & CELL_FLAG_OCCL))
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
						// mergeCandidate applies the same CELL_FLAG_OCCL test to a run's next
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
		// `!(s_cell_flags[...] & CELL_FLAG_OCCL)` the loop below used to compute for
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
				// OPT-MESHER finding 7: s_tint_mask[id] bit FACE_EAST instead of a call into
				// the blockFaceTintable() switch — see s_tint_mask's own comment.
				emitCross(out, s, si, e->lx, e->ly, e->lz, &rects[FACE_EAST], lit,
				          faceTint(s, si, (s_tint_mask[e->id] & (1u << FACE_EAST)) != 0));
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
			// out of CELL_FLAG_OCCL at the neighbour's offset. Same answer, from the same
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
			// CELL_FLAG_DRAW, not CELL_FLAG_SOLID: the pair of blocks this rule is about is
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

			// v1.8.8. Asked once and handed to BOTH the merge test and the emitter, so the
			// tint that decides whether two faces may share a quad is the same tint that ends
			// up in that quad's vertices. Two answers here is the whole smear bug — see
			// faceFlatKey.
			//
			// OPT-MESHER finding 7: s_tint_mask[id] bit `face` instead of a call into the
			// blockFaceTintable() switch — see s_tint_mask's own comment at its declaration.
			const bool      tintable = (s_tint_mask[e->id] & (1u << face)) != 0;
			const int       w  = mergeRun(s, si, uc, face, e->id, deferred, lit, drop, tintable);
			MERGE_PROBE_BUMP(w);   // OPT-MESHER finding 4 measurement — see the macro's comment

			emitFaceRun(out, s, si, e->lx, e->ly, e->lz, face, &rects[face], lit, w, drop,
			            faceTint(s, si, tintable));

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
//
// ── v1.8.2: the surface floor ────────────────────────────────────────────────
//
// A third rule now sits over the two above: EVERY water cell with a non-water cell above it is
// drawn at least WATER_SURFACE_DROP (world/scratch.h) short, whatever its level says. A source
// has no entry in the simulation's side map at all — that is what makes an ocean free, see
// world/water.h — so its band byte is 0 and its natural drop is 0, and until this task a sea
// was a wall of full cubes flush with the grass beside it. It is now 7/8, which is what
// Minecraft draws and what makes water read as a surface.
//
// The rule is a FLOOR, not an override: a level-3 trickle keeps its drop of 5. Only the cells
// that would have come out taller than 7/8 move, which is levels 8 (source) and 7.
//
// The gate below is the reason this costs nothing on the rest of the map. water_any means "the
// band holds a flow level", and a lake of sources holds none, so it alone would send a pure
// ocean straight back out of this function with the change invisible. has_water means "some
// cell in the scratch is BLOCK_WATER", which world/scratch.c answers while it is already
// copying the blocks. Deleting the gate instead would pay the 5,508-cell scan below on every
// chunk in the world, ~95% of which contain no water at all.
static void dropBuild(const MeshScratch* s)
{
	s_any_drop = false;
	if (!s->water_any && !s->has_water) return;

	memset(s_cell_drop, 0, sizeof s_cell_drop);

	const int above = SCRATCH_DIM * SCRATCH_DIM;
	for (int i = 0; i < SCRATCH_BLOCKS - above; i++) {
		// The block array is the authority on what is there and the band only says how tall it
		// is. The band is a snapshot and the world it describes can have moved on —
		// waterDropColumn clears a column's flow cells, and a placed block can sit where one
		// was — so a level for a cell that is not water any more is ignored.
		//
		// Asked BEFORE the band is read, which is the v1.8.2 inversion: a source's band byte is
		// 0 and is indistinguishable from "not water", so the block test is now the thing that
		// decides whether this cell is a water surface at all.
		if (s->blocks[i] != (BlockId)BLOCK_WATER) continue;
		if (s->blocks[i + above] == (BlockId)BLOCK_WATER) continue;

		const uint8_t lvl = s->water[i];
		const uint8_t nat = lvl ? (uint8_t)(SCRATCH_WATER_STEPS - lvl) : 0u;

		s_cell_drop[i] = nat > WATER_SURFACE_DROP ? nat : (uint8_t)WATER_SURFACE_DROP;
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

	// v1.8.6: one packed byte per cell instead of three separate arrays — see s_cell_flags'
	// own comment above for the reasoning and for why the word-scan in emitCollect needed a
	// mask, not just a renamed array, to stay exact.
	//
	// OPT-MESHER finding 2: that packed byte is now itself a single table lookup — the
	// three-table read above (s_solid[id]/s_occludes[id]/s_draws[id]) folded once, per id,
	// into s_cell_flag_of by planBuild. Same three bits, same values, one load instead of
	// three per cell.
	for (int i = 0; i < SCRATCH_BLOCKS; i++)
		s_cell_flags[i] = s_cell_flag_of[s->blocks[i]];

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
