// Chunk -> triangles. Phase 3, step 3.1.
//
// A face is emitted only where a solid block meets a non-solid one, which is the
// whole optimisation: a 16x16x16 chunk of stone has 4,096 blocks and 24,576 faces,
// of which 1,536 are visible. Everything inside is never built, never uploaded and
// never drawn.
//
// Deliberately *not* greedy meshing. Merging coplanar faces into big quads is a
// measured decision deferred to Phase 7 — it complicates AO and per-face textures,
// and this hardware may well be bound by something else first. See
// code-vault/wiki/blocksmith/blocksmith-plan.md.
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

// Meshes the chunk sitting in the middle of `s`. Positions come out chunk-local,
// 0..16 in block units, so the caller places the chunk with a model matrix.
//
// Indices are 16-bit: a worst-case chunk needs 6,144 vertices, well past a u8 but
// nowhere near a u16.
void meshChunk(MeshOut* out, const MeshScratch* s);

// The worst case a single chunk can produce, for sizing buffers: a 3D checkerboard,
// where every solid block has six exposed faces.
#define MESH_MAX_FACES    (CHUNK_BLOCKS / 2 * BLOCK_FACES)   // 12288
#define MESH_MAX_VERTS    (MESH_MAX_FACES * 4)
#define MESH_MAX_INDICES  (MESH_MAX_FACES * 6)
