// entitymodel.h — draws the animals in the world.
//
// v1.8.16 IMP-ANIM. This file used to be the P1 *probe* rather than a renderer: one flat
// axis-aligned box per animal, coloured by a TEV constant, on a position-only 12-byte
// vertex, whose whole purpose was to turn "how much does a per-animal draw call cost?"
// into a measured number before anyone painted a pig. That measurement is done and the
// probe harness below is kept intact (ENTMODEL_PROBE_N in entitymodel.c). What has changed
// is that the animals are now MODELS: six boxes each, textured from gfx/animals.png.
//
// ── the vertex format, and the PICA alignment trap ────────────────────────────────────────
//
// EmVertex is 3 floats of position followed by 2 floats of texture coordinate: offsets 0
// and 12, stride 20. That is byte-identical in shape to CrackVertex (scene/crackoverlay.c)
// and to the first two attributes of SpriteVertex (gfx/sprite.c) and ParticleVertex
// (gfx/particles.c), all three of which are shipping and correct on real hardware.
//
// This is the trap that froze real consoles from v1.1.0 to v1.2.4, so it is spelled out
// rather than waved at. citro3d has no per-attribute offset: AttrInfo_AddLoader appends,
// and the PICA200 computes each attribute's offset inside the vertex as the running sum of
// the SIZES declared before it. Three signed BYTES followed by four bytes therefore puts a
// four-byte fetch on byte offset 3, forever — see the long note at chunk_render.c:1196-1213,
// which records the hardware capture of the hung frame (ATTRIBBUFFERS_FORMAT_LOW =
// 0x000000d8) and names gfx/sprite.c as the control that identified it.
//
// An all-GPU_FLOAT vertex cannot reach that state by construction. Every float component is
// 4 bytes, so every running sum of them is a multiple of 4, so every attribute offset is
// 4-byte aligned no matter how many attributes are declared or in what order. Here that is
// 3*4 = 12 for attribute 1, and 12 % 4 == 0. The hazard needs a sub-4-byte component type to
// exist at all; there is none in this file.
//
// ── the data model ────────────────────────────────────────────────────────────────────────
//
// Everything about an animal's shape and its place on the texture sheet is ONE table,
// kEmBoxes below, and everything else is derived from it: the vertex buffer, the culling
// AABB, and — via tools/make_animals.py, which PARSES this header rather than restating it —
// the art itself. That is deliberate. A model and its texture layout are two halves of one
// fact, and this project has repeatedly paid for the same fact written down twice (the atlas
// slot count, the diff-store capacity, the chunk dimension: see the guards in the Makefile).
// Here there is nothing to keep in step, because there is only one copy.
//
// Units are TEXELS, at EM_TEXELS_PER_BLOCK to the block, and they are the same texels on the
// sheet as in the world: a box that is 10 texels wide is 10 sheet pixels wide on its front
// face. Positions are model space — feet at y = 0, centred on x = z = 0, front facing -Z,
// the same space scene/playermodel.c uses.
//
// The section between here and EM_DATA_ONLY is deliberately free of <3ds.h> and <citro3d.h>,
// for the same reason gfx/fogramp.c and scene/render_dist.c are: the geometry and the UV
// arithmetic are exactly the parts that can be checked on a host instead of by squinting at
// a console, and scene/entitymodel_test.c does check them there.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── sheet geometry ────────────────────────────────────────────────────────────────────────
//
// v1.8.18 MON-BUILD: 256x128 RGBA5551 = 65,536 bytes of VRAM, a 4x2 grid of 64x64 quadrants
// (EM_QUADRANT_COLS wide, 2 tall) — up from 128x128 = 32,768 B / 2x2. Not 128x192: citro3d's
// C3D_TexInitWithParams (checked at atlas_uv.h:15-21 and asserted for the block atlas at
// atlas_uv_shader_test.c:987-988) rejects any texture dimension outside 8..1024 OR not a power
// of two, each axis independently, and 192 is not a power of two. Not exactly six quadrants
// either: the next monster after this pair hits the same wall, and eight quadrants costs the
// same VRAM as the smallest legal size up from six would anyway. Six of the eight are used
// (pig, cow, chicken, sheep, zombie, skeleton); the last two (col 2, col 3 of row 1) are spare.
// Sized from the nets rather than picked: the cow's body box is 12x10x18 texels, whose
// unwrapped net is 2*(12+18) = 60 wide, and 60 does not fit in 32. tools/make_animals.py
// re-derives this from the table and fails if any net leaves its quadrant.
#define EM_SHEET_W            256
#define EM_SHEET_H            128
#define EM_QUADRANT_W          64
#define EM_QUADRANT_H          64
#define EM_QUADRANT_COLS        4    // sheet is EM_QUADRANT_COLS quadrants wide; a kind's
                                     // cell is (k % EM_QUADRANT_COLS, k / EM_QUADRANT_COLS).
                                     // ONE named constant so this ratio is not restated as a
                                     // bare "4" (or worse, a stale "2") anywhere else — see
                                     // tools/make_animals.py's verify_sheet() and
                                     // scene/entitymodel_test.c's testUVsInQuadrant(), which
                                     // both key off it instead of a literal.
#define EM_TEXELS_PER_BLOCK    16

#define EM_KINDS               6    // pig, cow, chicken, sheep, zombie, skeleton — asserted
                                    // against the ENT_KIND_* ids in entitymodel.c, which owns
                                    // that link. NOT the same number as animal.h's
                                    // ENT_KIND_COUNT: that constant is "one past the last
                                    // ANIMAL kind" (stays 5, so animalDef() keeps returning
                                    // NULL for the zombie and skeleton — see animal.h and
                                    // docs/plan-1.8.18-monsters.md §3.1/Phase 2). This is "one
                                    // past the last kind with a MODEL", which is a different
                                    // count and entitymodel.c is where the two are tied
                                    // together deliberately, not restated by accident.
#define EM_BOXES_PER_KIND      6
#define EM_FACES_PER_BOX       6
#define EM_VERTS_PER_FACE      6    // two triangles, no index buffer
#define EM_VERTS_PER_BOX       (EM_FACES_PER_BOX * EM_VERTS_PER_FACE)      // 36
#define EM_VERTS_PER_KIND      (EM_BOXES_PER_KIND * EM_VERTS_PER_BOX)      // 216
#define EM_VERTS               (EM_KINDS * EM_VERTS_PER_KIND)              // 864

// Face order. Named rather than numbered because the UV unwrap below assigns each face a
// different rectangle of the net and a different pair of axes, and "face 3" says nothing
// about which of those it got. RIGHT/LEFT are the ANIMAL's own right and left: the model
// faces -Z with +Y up, so forward x up = (0,0,-1) x (0,1,0) = (+1,0,0), i.e. the animal's
// right hand points along +X.
#define EM_FACE_TOP     0   // +Y
#define EM_FACE_BOTTOM  1   // -Y
#define EM_FACE_RIGHT   2   // +X
#define EM_FACE_FRONT   3   // -Z
#define EM_FACE_LEFT    4   // -X
#define EM_FACE_BACK    5   // +Z

// Position plus texture coordinate, both float. See the alignment note at the top.
//
// Float positions rather than the world's packed 8-bit MeshVertex for scene/crackoverlay.c's
// reason exactly: these are sixteenths of a block in a model that is a block and a half long,
// and the model matrix that places it carries a yaw, so an 8-bit attribute would have to
// agree with a rotation about its own quantisation. Float UVs because they are already
// normalised — the sheet size is divided out here, on the CPU, once at init, so
// source/shaders/crack.v.pica (which this file reuses) needs no uvScale constant and cannot
// drift out of step with EM_SHEET_W/H.
typedef struct {
	float x, y, z;
	float u, v;
} EmVertex;

// One box of one animal.
//
// px/py/pz is the MINIMUM corner in texels; sx/sy/sz the size in texels. tox/toy is the
// top-left corner of this box's unwrapped net in SHEET PIXELS, top-left origin (the origin
// tools/make_animals.py paints in; the V flip that texture coordinates need happens once, in
// emBoxFaceQuad below, and nowhere else).
//
// Fixed-width types, never an enum or a bitfield: the ARM EABI compiles with -fshort-enums
// and a host sizeof() has already lied about a console struct on this project.
typedef struct {
	int8_t  px, py, pz;
	uint8_t sx, sy, sz;
	uint8_t tox, toy;
} EmBox;

// The models.
//
// Kind order is ENT_KIND_PIG, COW, CHICKEN, SHEEP — the order of the four ids, which
// entitymodel.c static-asserts it still holds. Six boxes each, uniformly, so the vertex
// buffer is a flat stride and a kind is selected by first-vertex offset with no per-kind
// table lookup in the draw path (the shape scene/crackoverlay.c's buildCubes established).
//
// The four legs of a quadruped share ONE net: they are the same shape and want the same art,
// and giving each its own rectangle would spend four times the sheet on four identical
// pictures. Nothing about the vertex builder knows this — two boxes with the same tox/toy
// simply get the same UVs.
//
// The extra detail boxes are the chicken's beak and wattle, which is why it has two legs and
// six boxes rather than four legs and six boxes. The pig's snout and the cow's muzzle are
// NOT boxes: they are painted onto the front face of the head, which is what the design asks
// for ("a distinct snout panel on the head front") and costs no geometry.
//
// The zombie and skeleton are humanoid: body, head, two arms, two legs — the SAME six-box
// budget a quadruped spends on body/head/one-shared-leg-net, just distributed differently.
// Neither animates (nothing in this file does — see the header comment). A zombie's arms held
// out in front is GEOMETRY, not a runtime pose: its arm boxes are short in y and long in z,
// planted flush against the torso's front face and reaching forward, so the "held out" look is
// baked into the box table exactly once and costs the same one draw call every other kind
// costs. The skeleton is narrower everywhere except the skull (kept animal-head-sized) for a
// leaner silhouette; the rest of the "reads as bone" work is the shader's, in
// tools/make_animals.py — see zombie_shader/skeleton_shader there.
//
// Quadrants, (k % EM_QUADRANT_COLS, k / EM_QUADRANT_COLS) * (EM_QUADRANT_W, EM_QUADRANT_H):
// pig (0,0), cow (64,0), chicken (128,0), sheep (192,0), zombie (0,64), skeleton (64,64).
// Columns 2 and 3 of row 1 — (128,64) and (192,64) — are the two spare quadrants noted above.
//
// v1.8.18 MON-BUILD: chicken and sheep MOVED here, from (0,64)/(64,64) to (128,0)/(192,0) —
// a flat +128 tox / -64 toy shift on every one of their rows, nothing else. That is a
// consequence of EM_QUADRANT_COLS going from 2 to 4: k=2 and k=3 fall in row 0 now, not row 1,
// under the same (k % COLS, k / COLS) formula this table has always used. Their MODEL geometry
// (px/py/pz/sx/sy/sz) is untouched — only which sheet pixels their unwrap lands on moved, and
// tools/make_animals.py re-paints them at the new location from the same shader, so the art is
// pixel-identical, just relocated. See tools/make_animals.py's module docstring for the other
// half of this: verify_sheet() computes quadrants with the same formula from the same
// EM_QUADRANT_COLS, parsed out of this header rather than restated.
static const EmBox kEmBoxes[EM_KINDS][EM_BOXES_PER_KIND] = {
	// ── pig — 10x8x16 body, 14 texels tall overall (0.875 blocks) ───────────────────────
	{
		{ -5,  6,  -8,  10,  8, 16,   0,  0 },   // body    net 52x24
		{ -4,  5, -14,   8,  8,  8,   0, 24 },   // head    net 32x16
		{ -5,  0,  -7,   4,  6,  4,  32, 24 },   // leg front-left   net 16x10, shared
		{  1,  0,  -7,   4,  6,  4,  32, 24 },   // leg front-right
		{ -5,  0,   3,   4,  6,  4,  32, 24 },   // leg back-left
		{  1,  0,   3,   4,  6,  4,  32, 24 },   // leg back-right
	},
	// ── cow — 12x10x18 body, 20 texels tall (1.25 blocks) ───────────────────────────────
	{
		{ -6, 10,  -9,  12, 10, 18,  64,  0 },   // body    net 60x28
		{ -4, 12, -15,   8,  8,  6,  64, 28 },   // head    net 28x14
		{ -6,  0,  -8,   4, 10,  4,  92, 28 },   // leg front-left   net 16x14, shared
		{  2,  0,  -8,   4, 10,  4,  92, 28 },   // leg front-right
		{ -6,  0,   4,   4, 10,  4,  92, 28 },   // leg back-left
		{  2,  0,   4,   4, 10,  4,  92, 28 },   // leg back-right
	},
	// ── chicken — 12 texels tall (0.75 blocks), two legs plus beak and wattle ───────────
	// v1.8.18 MON-BUILD: tox +128, toy -64 from the pre-256-wide sheet — see the table's own
	// header comment above. Geometry (columns 1-6) unchanged.
	{
		{ -3,  3,  -4,   6,  5,  8, 128,  0 },   // body    net 28x13
		{ -2,  8,  -6,   4,  4,  4, 128, 13 },   // head    net 16x8
		{ -2,  0,  -1,   2,  3,  2, 144, 13 },   // leg left         net 8x5, shared
		{  0,  0,  -1,   2,  3,  2, 144, 13 },   // leg right
		{ -1,  9,  -8,   2,  2,  2, 152, 13 },   // beak    net 8x4
		{ -1,  7,  -7,   2,  2,  1, 160, 13 },   // wattle  net 6x3
	},
	// ── sheep — 12x9x16 woolly body, 18 texels tall (1.125 blocks) ──────────────────────
	// v1.8.18 MON-BUILD: tox +128, toy -64 from the pre-256-wide sheet — see the table's own
	// header comment above. Geometry (columns 1-6) unchanged.
	{
		{ -6,  9,  -8,  12,  9, 16, 192,  0 },   // body    net 56x25
		{ -4, 10, -13,   8,  7,  5, 192, 25 },   // head    net 26x12
		{ -5,  0,  -7,   4,  9,  4, 218, 25 },   // leg front-left   net 16x13, shared
		{  1,  0,  -7,   4,  9,  4, 218, 25 },   // leg front-right
		{ -5,  0,   3,   4,  9,  4, 218, 25 },   // leg back-left
		{  1,  0,   3,   4,  9,  4, 218, 25 },   // leg back-right
	},
	// ── zombie — humanoid, 32 texels tall (2.0 blocks), arms held out in front ──────────
	// v1.8.18 MON-BUILD: new. Body 8x12x4, head an 8-cube sitting flush on top of it, legs
	// 4x12x4 sharing one net (matching the quadrupeds' shared-leg convention), arms 4x4x8 —
	// short in y, long in z — planted against the body's front face (z=-2) and reaching to
	// z=-10, which is the "held out in front" silhouette as pure geometry, no runtime pose.
	{
		{ -4, 12,  -2,   8, 12,  4,  32, 64 },   // body    net 24x16
		{ -4, 24,  -4,   8,  8,  8,   0, 64 },   // head    net 32x16
		{ -8, 18, -10,   4,  4,  8,   0, 80 },   // arm left         net 24x12, shared
		{  4, 18, -10,   4,  4,  8,   0, 80 },   // arm right
		{ -4,  0,  -2,   4, 12,  4,  24, 80 },   // leg left         net 16x16, shared
		{  0,  0,  -2,   4, 12,  4,  24, 80 },   // leg right
	},
	// ── skeleton — humanoid, 31 texels tall (~1.94 blocks), lean everywhere but the skull ──
	// v1.8.18 MON-BUILD: new. Body 6x12x4 and legs 3x12x4 (narrower than the zombie's for a
	// bonier silhouette), head kept a full 6x7x6 so the skull still reads at 16 texels to the
	// block, arms 3x12x4 hanging flush at the torso's sides rather than reaching forward —
	// the zombie's forward reach is deliberately NOT repeated here, so the two silhouettes
	// read apart at a glance before either shader runs.
	{
		{ -3, 12,  -2,   6, 12,  4,  88, 64 },   // body    net 20x16
		{ -3, 24,  -3,   6,  7,  6,  64, 64 },   // head    net 24x13
		{ -6, 12,  -2,   3, 12,  4,  64, 80 },   // arm left         net 14x16, shared
		{  3, 12,  -2,   3, 12,  4,  64, 80 },   // arm right
		{ -3,  0,  -2,   3, 12,  4,  78, 80 },   // leg left         net 14x16, shared
		{  0,  0,  -2,   3, 12,  4,  78, 80 },   // leg right
	},
};

// The unwrapped net of a box, in sheet pixels. The classic cross-less strip:
//
//         +-----+-----+
//         | TOP | BOT |                     row A, d tall
//   +-----+-----+-----+-----+
//   |RIGHT|FRONT|LEFT |BACK |               row B, h tall
//   +-----+-----+-----+-----+
//     d      w     d     w
//
// so the whole net is 2*(w + d) wide and (h + d) tall. Stated as one function because both
// the vertex builder and tools/make_animals.py's packing check ask for it.
static inline void emBoxNetSize(const EmBox* b, int* w, int* h)
{
	*w = 2 * ((int)b->sx + (int)b->sz);
	*h = (int)b->sy + (int)b->sz;
}

// The four corners of one face, in rect order: top-left, top-right, bottom-right,
// bottom-left OF ITS RECTANGLE ON THE SHEET. Positions come out in BLOCKS, texture
// coordinates normalised.
//
// The unwrap is the one a cardboard box makes when you cut the top four edges and flatten
// it, which is what makes it paintable: every face's rectangle shares an edge with the face
// it is folded away from, so a pattern that runs across a seam lines up. Concretely, reading
// the sheet with the FRONT rectangle in front of you: RIGHT is folded away to the left, LEFT
// and BACK away to the right, TOP folded up (so the top rect's BOTTOM edge is the front's top
// edge, and its own top edge is the BACK of the animal), and BOTTOM folded down from the
// front's bottom edge.
//
// Each face therefore picks two of the three model axes to run its s and t along, and pins
// the third. `inv` on an axis means the sheet coordinate runs against that axis — which is
// not decoration: a face whose s runs the wrong way is MIRRORED, and on art that is nearly
// symmetric (a pig's flank) a mirrored face is invisible until the day something asymmetric
// is painted on it. Getting these six rows right is the whole job.
//
// THE V FLIP LIVES HERE AND NOWHERE ELSE. Texture v grows UPWARDS while PNG rows and this
// table's toy grow downwards, so v = 1 - row/H. tools/make_animals.py paints in PNG rows and
// applies no flip of its own; if it ever grows one, one of the two must go. This project has
// been bitten by a V flip more than once (see tools/make_crack_atlas.py's slot_png_y), and
// the failure mode is not an error — it is a plausible-looking animal wearing its own belly
// on its back.
static inline void emBoxFaceQuad(const EmBox* b, int face, EmVertex quad[4])
{
	const int w = b->sx, h = b->sy, d = b->sz;
	const int lo[3] = { b->px, b->py, b->pz };
	const int hi[3] = { b->px + w, b->py + h, b->pz + d };

	int rx, ry, rw, rh;              // this face's rectangle, sheet pixels, top-left origin
	int fixed_axis, fixed_hi;        // the axis the face is flat in, and which side
	int s_axis, s_inv;               // model axis the rect's x runs along
	int t_axis, t_inv;               // model axis the rect's y runs along

	switch (face) {
	case EM_FACE_TOP:                                    // +Y, folded up from FRONT
		rx = b->tox + d;           ry = b->toy;       rw = w; rh = d;
		fixed_axis = 1; fixed_hi = 1;
		s_axis = 0; s_inv = 1;     t_axis = 2; t_inv = 1;
		break;
	case EM_FACE_BOTTOM:                                 // -Y, folded down from FRONT
		rx = b->tox + d + w;       ry = b->toy;       rw = w; rh = d;
		fixed_axis = 1; fixed_hi = 0;
		s_axis = 0; s_inv = 1;     t_axis = 2; t_inv = 0;
		break;
	case EM_FACE_RIGHT:                                  // +X, folded left from FRONT
		rx = b->tox;               ry = b->toy + d;   rw = d; rh = h;
		fixed_axis = 0; fixed_hi = 1;
		s_axis = 2; s_inv = 1;     t_axis = 1; t_inv = 1;
		break;
	case EM_FACE_FRONT:                                  // -Z, the face the animal looks out of
		rx = b->tox + d;           ry = b->toy + d;   rw = w; rh = h;
		fixed_axis = 2; fixed_hi = 0;
		s_axis = 0; s_inv = 1;     t_axis = 1; t_inv = 1;
		break;
	case EM_FACE_LEFT:                                   // -X, folded right from FRONT
		rx = b->tox + d + w;       ry = b->toy + d;   rw = d; rh = h;
		fixed_axis = 0; fixed_hi = 0;
		s_axis = 2; s_inv = 0;     t_axis = 1; t_inv = 1;
		break;
	default:                                             // EM_FACE_BACK, +Z
		rx = b->tox + 2 * d + w;   ry = b->toy + d;   rw = w; rh = h;
		fixed_axis = 2; fixed_hi = 1;
		s_axis = 0; s_inv = 0;     t_axis = 1; t_inv = 1;
		break;
	}

	// Rect corners, clockwise from its top-left as the sheet is read.
	static const int kCorner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

	for (int c = 0; c < 4; c++) {
		const int s = kCorner[c][0] ? rw : 0;
		const int t = kCorner[c][1] ? rh : 0;

		int p[3];
		p[fixed_axis] = fixed_hi ? hi[fixed_axis] : lo[fixed_axis];
		p[s_axis]     = s_inv ? hi[s_axis] - s : lo[s_axis] + s;
		p[t_axis]     = t_inv ? hi[t_axis] - t : lo[t_axis] + t;

		quad[c].x = (float)p[0] / (float)EM_TEXELS_PER_BLOCK;
		quad[c].y = (float)p[1] / (float)EM_TEXELS_PER_BLOCK;
		quad[c].z = (float)p[2] / (float)EM_TEXELS_PER_BLOCK;
		quad[c].u = (float)(rx + s) / (float)EM_SHEET_W;
		quad[c].v = 1.0f - (float)(ry + t) / (float)EM_SHEET_H;
	}
}

// Writes one kind's EM_VERTS_PER_KIND vertices into out. Split out of the draw module so
// scene/entitymodel_test.c builds the SAME vertices the console does rather than a host copy
// of the same idea — the difference between an instrument that shares the code path and one
// that agrees with it by hand.
//
// Winding is not load bearing: entityModelDraw sets GPU_CULL_NONE, matching
// scene/playermodel.c and scene/highlight.c. A transposed corner would show as a shading
// oddity rather than as a hole, and the six boxes of an animal overlap at the joints where
// back faces are hidden by the depth test anyway.
static inline void emBuildKind(int kind_index, EmVertex* out)
{
	int vi = 0;
	for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
		const EmBox* b = &kEmBoxes[kind_index][bi];
		for (int face = 0; face < EM_FACES_PER_BOX; face++) {
			EmVertex q[4];
			emBoxFaceQuad(b, face, q);
			// The usual fan split of a quad; both triangles wound as the quad is.
			out[vi++] = q[0];
			out[vi++] = q[1];
			out[vi++] = q[2];
			out[vi++] = q[0];
			out[vi++] = q[2];
			out[vi++] = q[3];
		}
	}
}

// The model's own bounding box, in blocks, derived from the boxes rather than restated
// beside them. entityModelDraw needs it for the frustum test, and a hand-typed second
// opinion about how big a cow is would be exactly the "one fact written twice" this file's
// header note is about. Feet are at y = 0 by construction, so only the height and the larger
// horizontal half-extent are returned — the AABB the draw builds is deliberately square in
// plan so that yaw can be ignored (see entityModelDraw).
static inline void emKindExtent(int kind_index, float* half_horiz, float* height)
{
	int min_x = 127, max_x = -128, min_z = 127, max_z = -128, max_y = 0;
	for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
		const EmBox* b = &kEmBoxes[kind_index][bi];
		if (b->px < min_x)                    min_x = b->px;
		if (b->px + (int)b->sx > max_x)       max_x = b->px + (int)b->sx;
		if (b->pz < min_z)                    min_z = b->pz;
		if (b->pz + (int)b->sz > max_z)       max_z = b->pz + (int)b->sz;
		if (b->py + (int)b->sy > max_y)       max_y = b->py + (int)b->sy;
	}
	const int hx = (-min_x > max_x) ? -min_x : max_x;
	const int hz = (-min_z > max_z) ? -min_z : max_z;
	const int h  = (hx > hz) ? hx : hz;

	*half_horiz = (float)h     / (float)EM_TEXELS_PER_BLOCK;
	*height     = (float)max_y / (float)EM_TEXELS_PER_BLOCK;
}

#ifndef EM_DATA_ONLY

#include <citro3d.h>

#include "entity/entity.h"

// Builds the shared box mesh, uploads gfx/animals.t3x and picks up the crack shader's
// uniforms. False if the vertex buffer could not be allocated, in which case every draw
// below is a silent no-op and the rest of the game is unaffected — the same
// degrade-to-nothing posture playerModelInit() and highlightInit() have.
//
// A FAILED TEXTURE IS NOT A FAILED INIT. If the sheet cannot be imported this still returns
// true and the animals draw in flat per-kind colours, exactly as the v1.8.14 probe did.
// Turning a missing texture into no animals — or into a crash — would be a worse outcome
// than an untextured pig, and the flat path is a handful of lines that already existed.
bool entityModelInit(void);

void entityModelExit(void);

// Draws every ANIMAL-kind entity in ew. Culls internally (distance + frustum).
// Call from inside drawEye(), beside playerModelDraw(). Does not restore GPU
// state -- the next chunkRenderDraw()'s pipelineBind() re-establishes it, the
// same contract playermodel.h:39-41 states.
void entityModelDraw(const C3D_Mtx* view, const EntityWorld* ew);

// True once the sheet is on the GPU. Exposed for scene/entitymodel_test.c and for anyone
// diagnosing a build whose animals came out flat-coloured; the draw path reads the same
// flag internally.
bool entityModelTextured(void);

#endif  // EM_DATA_ONLY
