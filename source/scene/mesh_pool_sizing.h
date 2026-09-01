// The tiered mesh-pool sizing arithmetic, pulled out of scene/chunk_render.c so it can be
// proven on the host. chunk_render.c includes <3ds.h> and <citro3d.h> (linearAlloc,
// C3D_*) and cannot be compiled outside a devkitARM build — but nothing about HOW MANY
// BYTES the pool asks linearAlloc for depends on the console: it is arithmetic over slot
// counts and face-slot sizes. chunk_render.c calls the functions below to compute those
// same byte totals; this file holds only the arithmetic, not the measurement or the
// reasoning behind the constants, which stays in chunk_render.c's own comments (see the
// long comment on TIER_S_FACES there for where 256/120/512/1024 came from).
//
// Deliberately free of <3ds.h>, the same rule scene/render_dist.h states for itself.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Per-slot capacity. A pathological 16^3 checkerboard would need 12,288 faces, which no
// terrain produces; 2,048 faces is comfortably past a fully exposed chunk surface (1,536)
// and keeps a slot at 88 KB. Moved here from scene/chunk_render.h (v1.6.0 task 12's tier
// split is the only thing in that header that varies by render distance) so the host suite
// can size a pool without including a header that pulls in <3ds.h>. scene/chunk_render.h
// includes this file and uses these same three names — nothing else in the tree refers to
// them, so there is exactly one definition, not two kept in sync by hand.
#define MESH_SLOT_FACES    2048
#define MESH_SLOT_VERTS    (MESH_SLOT_FACES * 4)
#define MESH_SLOT_INDICES  (MESH_SLOT_FACES * 6)

// The two smaller tiers' sizes, fixed regardless of render distance. Measured against the
// RENDER_DIST_MAX 3 ring (see chunk_render.c's TIER_S_FACES comment) and deliberately NOT
// re-derived when MESH_SLOTS grows — chunk_render.c's TIER_L_SLOTS is
// "MESH_SLOTS - TIER_S_SLOTS - TIER_M_SLOTS", so every slot a wider ring adds lands in the
// L tier, the most expensive one. That is what makes the pool's cost grow faster than
// linearly in the slot count once MESH_SLOTS passes S + M — which is exactly the thing a
// flat per-slot average cannot see.
//
// chunk_render.c keeps its own TIER_S_FACES/TIER_M_FACES/TIER_S_SLOTS/TIER_M_SLOTS
// #defines, with all the measurement history in their comments; a _Static_assert right
// after them ties each one to the constant of the same value here, so the two can never
// silently drift apart.
#define MESH_POOL_TIER_S_FACES  512
#define MESH_POOL_TIER_M_FACES  1024

#define MESH_POOL_TIER_S_SLOTS  256
#define MESH_POOL_TIER_M_SLOTS  120

// Bytes one tier's linearAlloc costs: faces_per_slot * 4 vertices/face * vertex_bytes,
// times how many slots the tier holds. vertex_bytes is passed in rather than assumed, so
// this file never needs to know MeshVertex's layout — world/mesh_vertex.h already
// _Static_asserts sizeof(MeshVertex) == 8 for the one caller that cares about that number.
static inline size_t meshPoolTierBytes(int faces_per_slot, int slot_count, size_t vertex_bytes)
{
	return vertex_bytes * (size_t)faces_per_slot * 4 * (size_t)slot_count;
}

// Bytes the one shared index buffer costs (scene/chunk_render.c's s_shared_indices): 6
// uint16_t indices per face-slot, across slot_faces — every slot in every tier draws from
// this one buffer, so it is sized once for the largest a slot can ever be, not per tier.
static inline size_t meshPoolIndexBytes(int slot_faces)
{
	return sizeof(uint16_t) * (size_t)slot_faces * 6;
}

// Slots the tiered pool would need for ring radius `radius`, generalising
// RENDER_DIST_MAX_SLOTS (scene/render_dist.h) from the one compile-time RENDER_DIST_MAX to
// any radius: the same arithmetic, (2*radius+1)^2 columns times slots_per_column chunks per
// column. Used only by the host suite to evaluate radii the console does not currently
// build for (RENDER_DIST_MAX caps the console pool at 3); the console pool itself is still
// sized once, at RENDER_DIST_MAX, by render_dist.h's own macros, and this function is not on
// that path.
static inline int meshSlotsForRadius(int radius, int slots_per_column)
{
	const int columns = (2 * radius + 1) * (2 * radius + 1);
	return columns * slots_per_column;
}

// Total linear-heap bytes the tiered pool claims for a pool sized at `mesh_slots` slots,
// with slot_faces the absolute per-slot face cap (MESH_SLOT_FACES) and vertex_bytes
// sizeof(MeshVertex). The L tier receives whatever is left after the two fixed tiers —
// exactly chunk_render.c's TIER_L_SLOTS. A mesh_slots below
// MESH_POOL_TIER_S_SLOTS + MESH_POOL_TIER_M_SLOTS would make the L tier negative; the real
// build forbids that at compile time (chunk_render.c's `TIER_L_SLOTS > 0` _Static_assert),
// so this function does not guard against it — a caller evaluating a hypothetical radius is
// expected to check mesh_slots first, the way the test that calls this does.
static inline size_t meshPoolTotalBytes(int mesh_slots, int slot_faces, size_t vertex_bytes)
{
	const int l_slots = mesh_slots - MESH_POOL_TIER_S_SLOTS - MESH_POOL_TIER_M_SLOTS;

	size_t total = meshPoolIndexBytes(slot_faces);
	total += meshPoolTierBytes(MESH_POOL_TIER_S_FACES, MESH_POOL_TIER_S_SLOTS, vertex_bytes);
	total += meshPoolTierBytes(MESH_POOL_TIER_M_FACES, MESH_POOL_TIER_M_SLOTS, vertex_bytes);
	total += meshPoolTierBytes(slot_faces, l_slots, vertex_bytes);
	return total;
}
