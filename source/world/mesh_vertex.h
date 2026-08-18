// The vertex the mesher emits — the same 8 bytes locked down in Phase 1.
//
// It lives in world/ rather than scene/ because the mesher is host-testable and must
// same layout; world/block_tiles_check.c static-asserts the two byte-for-byte, so a
// change to either one fails the console build instead of shipping a mismatch.
#pragma once

#include <stdint.h>

typedef struct {
	int8_t  x, y, z;   // position in block units, chunk-local 0..16
	uint8_t u, v;      // atlas pixel coordinates, texture space (v grows upwards)
	uint8_t nrm;       // face index 0..5, resolved to a baked brightness in the shader
	uint8_t ao;        // ambient occlusion 0..3, 3 being unoccluded
	uint8_t pad;       // unread by the GPU; keeps the stride a power of two
} MeshVertex;

_Static_assert(sizeof(MeshVertex) == 8, "vertex format changed - update the memory budget in the vault");
