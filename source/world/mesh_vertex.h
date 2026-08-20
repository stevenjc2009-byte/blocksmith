// The vertex the mesher emits — the same 8 bytes locked down in Phase 1.
//
// It lives in world/ rather than scene/ because the mesher is host-testable and must
// same layout; world/block_tiles_check.c static-asserts the two byte-for-byte, so a
// change to either one fails the console build instead of shipping a mismatch.
#pragma once

#include <stdint.h>
#include <stddef.h>   // offsetof, for the alignment static-assert below

typedef struct {
	int8_t  x, y, z;   // position in block units, chunk-local 0..16
	int8_t  pad;       // unread by the GPU. See below - its POSITION is load-bearing.
	uint8_t u, v;      // atlas pixel coordinates, texture space (v grows upwards)
	uint8_t nrm;       // face index 0..5, resolved to a baked brightness in the shader
	uint8_t ao;        // ambient occlusion 0..3, 3 being unoccluded
} MeshVertex;

// Why pad sits in the MIDDLE, at offset 3, and must never be moved back to the end.
//
// citro3d has no per-attribute offset: AttrInfo_AddLoader appends, and the PICA200 computes
// each attribute's byte offset as the running sum of the sizes of the ones declared before it.
// So declaring a 3-component attribute followed by a 4-component one puts the second attribute
// at byte offset 3 of every vertex - unaligned, for the whole life of the buffer.
//
// That is what this layout used to do. The pad byte was at the end, "to keep the stride a power
// of two", when the stride was already 8 either way; what actually needed padding was the gap
// between the two attributes. Measured straight out of the command list the console hung on
// (v1.2.2 captured it): GPUREG_ATTRIBBUFFERS_FORMAT_LOW = 0x000000d8, which decodes as
// attribute 0 = 3 signed bytes and attribute 1 = 4 unsigned bytes, i.e. the GPU was being told
// to fetch a four-component attribute from offset 3 of an 8-byte stride, every vertex, forever.
//
// An emulator runs that on an x86 host that does unaligned loads without complaint, which is
// why this never once reproduced in Azahar across twelve builds. source/gfx/sprite.c, whose
// vertex is aligned throughout, draws correctly on the real console in the very same frames
// where the chunk draw wedges the GPU.
//
// With pad at offset 3, attribute 0 is declared as 4 components (x, y, z, pad) and attribute 1
// starts at offset 4. Both are 4-byte aligned, the vertex is still exactly 8 bytes, and the
// memory budget is unchanged. The shader reads inpos.xyz and ignores w.
//
// If you ever add a field here, keep both attributes on a 4-byte boundary.
_Static_assert(offsetof(MeshVertex, u) == 4,
               "attribute 1 must start 4-byte aligned - see the comment above, this is the bug "
               "that froze the console from v1.1.0 to v1.2.4");

_Static_assert(sizeof(MeshVertex) == 8, "vertex format changed - update the memory budget in the vault");
