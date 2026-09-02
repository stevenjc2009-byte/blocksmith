// Chunk -> triangles. Phase 3, step 3.1.
//
// A face is emitted only where a solid block meets a non-solid one, which is the
// whole optimisation: a 16x16x16 chunk of stone has 4,096 blocks and 24,576 faces,
// of which 1,536 are visible. Everything inside is never built, never uploaded and
// never drawn.
//
// It IS greedy along one axis, and has been since v1.6.0 task 11. This comment used to say
// "deliberately *not* greedy meshing ... deferred to Phase 7", which stopped being true the
// moment mergeRun() landed in mesher.c and stayed on the page for three minor versions. It is
// corrected here rather than deleted because the stale version was actively dangerous: it told
// anyone packing a new field into a vertex that no two faces are ever combined, and the whole
// hazard of doing that is that they ARE.
//
// What actually happens: mergeRun() walks coplanar faces of the same block id along the face's
// u axis and emits ONE quad covering up to ATLAS_MAX_MERGE_BLOCKS (15) cells, with the art
// tiled across it by GPU_REPEAT. Two faces merge only when faceFlatKey() gives them the same
// non-zero signature, so ANY per-face value that must not be shared across a run has to be part
// of that key. AO, light and now the biome tint are; see faceFlatKey in mesher.c.
//
// No <3ds.h> here: the mesher is the piece most likely to be wrong in a way a
// screenshot cannot show, so it runs under the PC test suite too.
#pragma once

#include "world/mesh_vertex.h"
#include "world/scratch.h"

// Where a mesh is written. The caller owns the memory — on the console that is the
// linear heap, in the tests it is an ordinary array.
typedef struct {
	MeshVertex* verts;
	uint16_t*   indices;
	uint32_t    vert_cap;    // in vertices
	uint32_t    index_cap;   // in indices

	uint32_t    vert_count;
	uint32_t    index_count;
	uint32_t    faces;       // quads emitted

	// Step 7.5. The index buffer holds two runs back to back: opaque faces in
	// [0, opaque_index_count) and transparent ones in [opaque_index_count, index_count).
	// The renderer draws them as two calls with different GPU state, the second with an
	// alpha test on, after everything opaque in the frame.
	//
	// One buffer with two runs rather than two buffers. The slot pool is 64 fixed-size
	// linear-heap allocations claimed once at init (see scene/chunk_render.h); a second
	// pool sized for the worst case would claim another 5.5 MB of a heap that cannot be
	// defragmented, to hold geometry that is a few percent of the total. Two runs cost
	// one extra draw call per chunk that has any, and no memory at all.
	uint32_t    opaque_index_count;
	uint32_t    opaque_faces;

	// Step 9.2. Where each of the six face buckets starts inside the opaque run, plus a
	// closing entry, so the length of bucket i is always face_start[i+1] - face_start[i] with
	// no last-element special case. face_start[0] is 0 and face_start[BLOCK_FACES] equals
	// opaque_index_count, always.
	//
	// Indexed by BUCKET SLOT, not by face id: slot i holds the face direction kFaceOrder[i].
	//
	// The opaque geometry is emitted face-major — every face of one direction in the chunk,
	// then every face of the next — so each bucket is one contiguous run the renderer can
	// draw or skip whole. At most three of the six directions can face the camera, so the
	// renderer submits at most half of a chunk's opaque vertices. The GPU was already
	// throwing the other half away in the backface test, but only after transforming them,
	// and the vertex stage is the narrow one on this hardware. See chunkRenderDraw.
	//
	// The transparent run is deliberately NOT bucketed: it is a few percent of the geometry
	// and it has to be drawn back-to-front as one ordered sequence.
	uint32_t    face_start[BLOCK_FACES + 1];

	bool        overflow;    // ran out of room: the mesh is incomplete, not corrupt
} MeshOut;

// The order the face buckets are emitted in: slot i of MeshOut.face_start holds the faces
// pointing kFaceOrder[i].
//
// It is not the FACE_* enum order, and the difference is worth a draw call. The renderer
// keeps one direction per axis (or both, for an axis whose slab the camera is inside) and
// merges buckets that survive NEXT TO EACH OTHER into one C3D_DrawElements. Two opposite
// faces of the same axis are never both kept while the camera is outside that slab, so an
// ordering that puts them side by side wastes that adjacency. This one — +X +Y +Z -X -Y -Z
// — has no two neighbours sharing an axis, which is the most merges the layout allows:
// 1.75 draw calls per chunk averaged over the eight view octants, against 2.5 for the enum
// order. Both are always correct; this one is just cheaper. See chunkRenderDraw.
extern const uint8_t kFaceOrder[BLOCK_FACES];

// ── v1.8.0 task 22b: what MeshVertex.nrm carries now ─────────────────────────
//
//   bits 0..2   the face index, FACE_EAST..FACE_NORTH, exactly what the whole byte used to be
//   bits 3..5   a HEIGHT DROP in eighths of a block, 0..7, subtracted from the vertex's y
//
// A drop is non-zero on exactly one thing: the top-plane corners of a flowing water cell. It is
// zero on every other vertex the mesher has ever emitted, so every existing mesh comes out byte
// for byte what it came out before — which is why the four pinned hashes in world/world_test.c
// did not move.
//
// WHY THE HEIGHT IS NOT SIMPLY IN THE VERTEX'S y. MeshVertex.x/y/z are bytes in WHOLE BLOCK
// UNITS and the GPU fetches them unscaled (scene/chunk_render.c's AttrInfo_AddLoader, and
// `mov r0.xyz, inpos` in both world shaders), so the only heights a vertex can express are the
// integers 0..16 — the same limit world/mesher.c's emitCross comment states for a plant's
// quads. A 7/8-high cube is not one of them. The three ways out were: rescale every position to
// eighths and divide the model matrix by eight, which moves the y byte of every vertex in the
// game and re-pins those four hashes; give water its own draw call with its own scaled matrix,
// which splits the transparent run three ways and adds renderer state nobody can test on this
// machine; or carry the fraction in a byte the shader already reads. This is the third. The
// shaders' faceShade table is grown from 8 entries to 64, indexed by the whole nrm byte, and
// carries the drop as a second component beside the brightness it always carried — so the
// decode is one `add` on a value the vertex stage had already fetched, and it is exactly 0.0
// for every face index 0..5.
//
// Anything reading a vertex's face direction must mask. meshNrmDrop() is the height.
#define MESH_NRM_FACE_BITS  3
#define MESH_NRM_FACE_MASK  ((uint8_t)((1u << MESH_NRM_FACE_BITS) - 1))
#define MESH_NRM_STATES     (1u << (MESH_NRM_FACE_BITS + 3))   // 64 faceShade rows

static inline uint8_t meshNrmFace(uint8_t nrm) { return (uint8_t)(nrm & MESH_NRM_FACE_MASK); }
static inline uint8_t meshNrmDrop(uint8_t nrm) { return (uint8_t)(nrm >> MESH_NRM_FACE_BITS); }

// A face index has to fit under the drop, or the two would overlap in the byte.
_Static_assert(BLOCK_FACES <= MESH_NRM_FACE_MASK + 1, "face index no longer fits nrm's low bits");

// ── v1.8.2: the vertex alpha, and why it rides in the same table ─────────────
//
// How opaque a faceShade row draws. 0.70 for water, 1.0 for everything else.
//
// The row index IS the whole nrm byte, so rows 0..7 are the 8 face ids at drop 0 and rows
// 8..63 are every non-zero drop. Since the surface floor landed (WATER_SURFACE_DROP in
// world/scratch.h) a non-zero drop is carried by water and by nothing else in the world, so
// those 56 rows are water-only and the alpha binds with no extra vertex bits, no extra
// instruction and no extra uniform register: the shaders' `mov outclr.w, ones` becomes
// `mov outclr.w, r3.wwww` and r3 was already fetched.
//
// That is the whole reason the recessed surface and the see-through surface had to land in one
// change. Without the floor, a SOURCE sits at drop 0 in rows 0..7 alongside stone and is
// indistinguishable from it; separating them would need a water flag in the nrm byte, which
// doubles the table to 128 rows against a PICA200 vertex stage that has 96 float uniform
// registers in total and already spends 77 of them.
//
// 0.70 is chosen so the alpha TEST does not have to move. scene/chunk_render.c tests
// GPU_GREATER against 127 and the TEV modulates alpha (C3D_Both), so a water fragment is
// 255 * 0.70 = 178 and passes, while a leaf's cutout texel is 255 * 0 = 0 and still fails.
// Anything in (128, 255) would work; below 0.50 the cutoff would have to move with it.
//
// It lives here rather than in scene/chunk_render.c because that file includes <3ds.h> and no
// host test can link it. This header is the one place both the renderer and the host suite can
// read the same number, which is what stops world/water_alpha_test.c from being a test that
// only checks its own copy of the rule.
#define WATER_ALPHA  0.70f

static inline float meshNrmAlpha(uint8_t nrm)
{
	return meshNrmDrop(nrm) >= WATER_SURFACE_DROP ? WATER_ALPHA : 1.0f;
}

// ── v1.8.8: what MeshVertex.ao carries now, and why the tint rides there ──────
//
//   bits 0..1   the ambient occlusion 0..3, 3 unoccluded — exactly what the whole byte was
//   bits 2..4   a TINT PALETTE INDEX, 0..7, resolved to an RGB multiplier by the shader
//   bits 5..7   still free
//
// Biome identity is carried by COLOUR, not by a block id per biome: there is one grass block
// and one tall-grass strand in the registry (world/registry.c) and the biome multiplies what
// they draw. That is Minecraft's model and it is the only one this hardware can afford — a
// grass id per biome would need a tile per biome on a 64-slot atlas with 47 slots left, and it
// would make every biome border a block-id boundary in the save format.
//
// WHY THE ao BYTE AND NOT A NEW ONE. MeshVertex is exactly 8 bytes and every byte is spoken for
// (world/mesh_vertex.h). Widening it to 9 would break the offsetof static-assert that documents
// the v1.1.0-v1.2.4 console freeze, and 10 (the next size that keeps both attributes 4-byte
// aligned) would multiply the chunk mesh pool by 1.25 — 46,948,352 bytes to 58,685,440 at the
// radius-5 ceiling, 11.7 MB of linear heap that cannot be defragmented, to carry three bits.
// cornerAO() has only ever returned 0..3 (mesher.c) and emitCross() only ever writes 3, so six
// bits of that byte have been zero in every vertex the mesher has ever emitted. Three of them
// are now the tint. Cost: ZERO bytes per vertex, ZERO bytes of mesh pool.
//
// THREE BITS, NOT SIX. The shader resolves the index through a uniform array, and the PICA200
// vertex stage has 96 float uniform registers TOTAL of which world_dynamic already spends 78 —
// so the palette is what caps this, not the byte. Eight rows costs 8 registers (86 of 96) and
// covers BIOME_COUNT (6, world/worldgen.h) with a spare and an untinted row. Bits 5..7 are left
// free rather than claimed for a wider index that could not be looked up anyway.
//
// ROW 0 IS THE IDENTITY, (1,1,1). Every face that is not tintable writes tint 0, the scratch's
// tint band is zero unless a caller fills it, and the palette's row 0 is white — so a vertex
// with no tint is byte-for-byte the vertex this mesher emitted before v1.8.8 and multiplies its
// colour by exactly 1. That is what keeps the four pinned mesh hashes in world/world_test.c and
// the two in world/water_mesh_test.c from moving, and it is checked, not assumed.
//
// Anything reading a vertex's occlusion must mask. Reading the raw byte where 0..3 is meant is
// the bug this encoding invites, and it is the one the SHADER hit first: both .pica files
// multiply the attribute by 1/3 with no mask at all, so an unmasked tint index would not be
// ignored, it would read as an ambient occlusion of up to 31 and blow the face to flat white.
#define MESH_AO_BITS    2
#define MESH_AO_MASK    ((uint8_t)((1u << MESH_AO_BITS) - 1))
#define MESH_TINT_BITS  3
#define MESH_TINT_MASK  ((uint8_t)((1u << MESH_TINT_BITS) - 1))
#define MESH_TINT_ROWS  (1u << MESH_TINT_BITS)   // 8 palette rows

// The untinted row. Must be 0 and must be (1,1,1) in the palette — see above.
#define MESH_TINT_NONE  ((uint8_t)0)

static inline uint8_t meshAoValue(uint8_t ao) { return (uint8_t)(ao & MESH_AO_MASK); }
static inline uint8_t meshAoTint(uint8_t ao)  { return (uint8_t)((ao >> MESH_AO_BITS) & MESH_TINT_MASK); }
static inline uint8_t meshAoPack(uint8_t ao, uint8_t tint)
{
	return (uint8_t)((ao & MESH_AO_MASK) | ((tint & MESH_TINT_MASK) << MESH_AO_BITS));
}

// An AO value has to fit under the tint, or the two overlap in the byte.
_Static_assert(MESH_AO_MASK == 3, "cornerAO returns 0..3; the tint starts at bit 2");
_Static_assert(MESH_AO_BITS + MESH_TINT_BITS <= 8, "tint index no longer fits the ao byte");

// ── The palette itself ───────────────────────────────────────────────────────
//
// The eight RGB multipliers the shader's tintPalette bank is filled with, one per tint index.
// Row `1 + biome` is that biome's colour and row 0 is the identity, so the mapping a future
// filler needs is MESH_TINT_ROW_FOR_BIOME below and nothing more.
//
// It lives HERE, in a header with no <3ds.h> in it, for exactly the reason WATER_ALPHA does:
// scene/chunk_render.c includes <3ds.h> and no host test can link it, so this is the one place
// the renderer and the host suite can read the same numbers. world/biome_tint_test.c links this
// function for real and then checks BY TEXT that the renderer calls it rather than carrying its
// own copy of the table — without that second half the suite would be proving things about dead
// code, which is a defect this tree has recorded eleven times.
//
// ── EVERY COMPONENT IS <= 1.0, AND THAT IS A HARDWARE LIMIT, NOT A TASTE ─────
//
// outclr is clamped to [0,1] by the rasterizer before it ever reaches the TexEnv, so a
// component above 1.0 does not brighten anything — it is silently clipped and the biome comes
// out looking like whichever channels did fit. A tint on this hardware can therefore only
// DARKEN or shift hue downward, never lift.
//
// That interacts badly with the art, and the interaction is worth writing down because it caps
// how different two biomes can look. tools/make_atlas.py paints grass_top from four saturated
// greens around (58,112,74) — it is finished art, not a greyscale multiply target the way
// Minecraft's grass texture is. Multiplying an already-saturated green can pull it toward
// black, toward olive, or toward blue-green, but it cannot make it straw-yellow or make it
// brighter than it started.
//
// v1.8.11: the desert row was 0.94/0.82/0.42, which reasoning about the PRODUCT (not the tint
// literal alone) shows was the bug. grass_top's four texels average to roughly (0.245, 0.468,
// 0.307) — green is already ~1.9x red in the base art — so even a tint with R > G (0.94 vs
// 0.82, ratio 1.15) still comes out green-dominant: measured product (0.231, 0.384, 0.129),
// hue ~96 degrees, squarely in the green/olive band. That is the "dry olive" the biome read as.
//
// The fix that fits inside a multiply is to widen the R:G ratio in the TINT far enough to
// overcome the base art's own G:R bias (~1.9x), not to nudge it. 1.00/0.50/0.40 does that:
// tint R:G is 2.0x, which drives every one of grass_top's four base shades (and its highlight
// speckle) to a product hue of 45-60 degrees — the yellow/tan band, never green — measured
// per-shade, not just on the average. The residual limit tools/make_atlas.py's comment already
// named still applies: a multiply cannot lift brightness past the base texel (value tops out
// at ~0.245, i.e. a dark khaki, not a bright sand tan), and closing that gap for real needs the
// ART repainted lighter — grass_top / grass_side / tall_grass / fern — which is out of scope
// here and has not been touched.
//
// PLAINS IS EXACTLY (1,1,1) ON PURPOSE. It is the biome the art was drawn for and the one most
// of the world is, so a plains grass block renders bit-identically to every build before this
// one and the tint is only ever visible where the world actually stops being plains.
typedef struct { float r, g, b; } MeshTint;

static inline MeshTint meshTintRow(uint8_t row)
{
	switch (row & MESH_TINT_MASK) {
	case 0:  return (MeshTint){ 1.00f, 1.00f, 1.00f };   // untinted — the identity
	case 1:  return (MeshTint){ 0.78f, 0.90f, 0.86f };   // tundra — pale and cold
	case 2:  return (MeshTint){ 0.62f, 0.80f, 0.70f };   // taiga  — dark cold green
	case 3:  return (MeshTint){ 1.00f, 1.00f, 1.00f };   // plains — the art as painted
	case 4:  return (MeshTint){ 0.80f, 0.94f, 0.72f };   // forest — deeper green
	case 5:  return (MeshTint){ 1.00f, 0.50f, 0.40f };   // desert — sandy tan (v1.8.11, was 0.94/0.82/0.42 "dry olive")
	case 6:  return (MeshTint){ 0.62f, 1.00f, 0.44f };   // jungle — vivid green
	default: return (MeshTint){ 1.00f, 1.00f, 1.00f };   // row 7, spare — identity
	}
}

// Which palette row a biome takes. Row 0 is reserved for "not tinted", so the biomes start at
// 1 and BIOME_COUNT (6, world/worldgen.h) lands on row 6 with row 7 spare.
//
// Spelled as a macro rather than a function taking a BiomeId so this header does not have to
// include world/worldgen.h — the mesher has no business knowing what a biome IS, only what
// index it was handed.
#define MESH_TINT_ROW_FOR_BIOME(b)  ((uint8_t)(((unsigned)(b) + 1u) & MESH_TINT_MASK))

// Meshes the chunk sitting in the middle of `s`. Positions come out chunk-local,
// 0..16 in block units, so the caller places the chunk with a model matrix.
//
// Indices are 16-bit: a worst-case chunk needs 6,144 vertices, well past a u8 but
// nowhere near a u16.
void meshChunk(MeshOut* out, const MeshScratch* s);

// Drops the derived block tables (solidity, occlusion, atlas rects) so the next
// meshChunk() rebuilds them. Called when remote registry definitions are applied
// at join; until then the tables are built lazily on first use and never change.
void mesherInvalidateTables(void);

// The worst case a single chunk can produce, for sizing buffers: a 3D checkerboard,
// where every solid block has six exposed faces.
#define MESH_MAX_FACES    (CHUNK_BLOCKS / 2 * BLOCK_FACES)   // 12288
#define MESH_MAX_VERTS    (MESH_MAX_FACES * 4)
#define MESH_MAX_INDICES  (MESH_MAX_FACES * 6)
