// Host probe for the chunk mesh pool's linear-heap cost at a render radius, byte-exact,
// computed by the same functions scene/chunk_render.c's chunkRenderInit calls to size its
// three linearAlloc's (scene/mesh_pool_sizing.h) — not a hand-copy of the arithmetic. See
// that header for the formula and scene/chunk_render.c's TIER_S_FACES comment for where the
// two fixed tier constants (256 S-tier slots, 120 M-tier slots) came from.
//
// Written to settle a disagreement between two paper derivations of this cost at radii the
// console does not currently build for (RENDER_DIST_MAX caps the shipped pool at 3): one
// walked the tiered arena formula by hand, one probed against a flat per-slot average taken
// at radius 3. They agreed at r3 and diverged by roughly 2x by r6. This binary is the
// tie-breaker — it runs the real formula, not a re-derivation of it.
//
// Own main(), same pattern as tests/hw_test.c and the other host-only binaries
// tools/run_host_tests.sh builds and runs.
#include <stdint.h>
#include <stdio.h>

#include "scene/mesh_pool_sizing.h"
#include "scene/render_dist.h"
#include "world/mesh_vertex.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                 \
		if (!(cond)) {                                                              \
			s_fails++;                                                              \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                            \
			if (!s_first[0])                                                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                            \
	} while (0)

int main(void)
{
	// Ties meshSlotsForRadius() — the generalisation this file needs to evaluate radii
	// RENDER_DIST_MAX does not currently allow — back to the actual compile-time constant
	// the console build uses today. If this ever goes red, the generalisation has drifted
	// from render_dist.h's own RENDER_DIST_MAX_COLUMNS/RENDER_DIST_MAX_SLOTS arithmetic and
	// every number this binary prints below is suspect.
	CHECK(meshSlotsForRadius(RENDER_DIST_MAX, RENDER_DIST_SLOTS_PER_COLUMN) == RENDER_DIST_MAX_SLOTS);

	CHECK(sizeof(MeshVertex) == 8);

	printf("mesh pool bytes, exact formula from scene/mesh_pool_sizing.h:\n");
	printf("  MESH_SLOT_FACES=%d  vertex_bytes=%zu  tier S=%d slots x %d faces  "
	       "tier M=%d slots x %d faces  RENDER_DIST_SLOTS_PER_COLUMN=%d\n",
	       MESH_SLOT_FACES, sizeof(MeshVertex),
	       MESH_POOL_TIER_S_SLOTS, MESH_POOL_TIER_S_FACES,
	       MESH_POOL_TIER_M_SLOTS, MESH_POOL_TIER_M_FACES,
	       RENDER_DIST_SLOTS_PER_COLUMN);

	for (int r = 3; r <= 6; r++) {
		const int    mesh_slots = meshSlotsForRadius(r, RENDER_DIST_SLOTS_PER_COLUMN);
		const size_t bytes      = meshPoolTotalBytes(mesh_slots, MESH_SLOT_FACES, sizeof(MeshVertex));
		const double mb         = (double)bytes / (1024.0 * 1024.0);

		printf("  radius %d: columns=%d mesh_slots=%d bytes=%zu MB=%.2f\n",
		       r, mesh_slots / RENDER_DIST_SLOTS_PER_COLUMN, mesh_slots, bytes, mb);
	}

	// ── v1.8.5. The per-console ceilings, checked rather than asserted in a comment ────────
	//
	// Everything above this line PRINTS. What follows GATES, and it is the whole reason the
	// ceiling is allowed to differ by console at all. Two things had to be true before
	// RENDER_DIST_MAX_NEW could be 5, and both are checked here so that a later change to a
	// tier split, to MESH_SLOT_FACES, or to sizeof(MeshVertex) fails the build instead of
	// failing to boot on somebody's console.

	// First: tie the host arithmetic to a real console reading. The boot probe on a New 3DS
	// CXI printed `chunkRenderInit  cost lin +9199616` at radius 3. If this line goes red, the
	// formula in scene/mesh_pool_sizing.h and the pool chunk_render.c actually allocates have
	// drifted apart, and every fit check below is arithmetic about a pool that does not exist.
	const size_t pool_r3 = meshPoolTotalBytes(
		meshSlotsForRadius(3, RENDER_DIST_SLOTS_PER_COLUMN), MESH_SLOT_FACES, sizeof(MeshVertex));
	CHECK(pool_r3 == 9199616u);

	// Second: each console's ceiling has to fit that console's linear heap, with the screen and
	// GPU buffers counted, because those are claimed BEFORE the pool and the pool only ever
	// sees what is left.
	const size_t pool_old = meshPoolTotalBytes(
		meshSlotsForRadius(RENDER_DIST_MAX_OLD, RENDER_DIST_SLOTS_PER_COLUMN),
		MESH_SLOT_FACES, sizeof(MeshVertex));
	const size_t pool_new = meshPoolTotalBytes(
		meshSlotsForRadius(RENDER_DIST_MAX_NEW, RENDER_DIST_SLOTS_PER_COLUMN),
		MESH_SLOT_FACES, sizeof(MeshVertex));

	CHECK(pool_old + RENDER_DIST_LINEAR_OVERHEAD_BYTES <= RENDER_DIST_LINEAR_HEAP_OLD);
	CHECK(pool_new + RENDER_DIST_LINEAR_OVERHEAD_BYTES <= RENDER_DIST_LINEAR_HEAP_NEW);

	// Third, and this is the one that stops the ceiling being a taste: one radius further must
	// NOT fit. Without this the pair above would still pass at RENDER_DIST_MAX_NEW 3, 4 or 5
	// and would be saying nothing about which of them is right. With it, 5 is pinned as the
	// largest radius the New 3DS's linear heap can hold — a measurement, not a preference.
	const size_t pool_over = meshPoolTotalBytes(
		meshSlotsForRadius(RENDER_DIST_MAX_NEW + 1, RENDER_DIST_SLOTS_PER_COLUMN),
		MESH_SLOT_FACES, sizeof(MeshVertex));
	CHECK(pool_over + RENDER_DIST_LINEAR_OVERHEAD_BYTES > RENDER_DIST_LINEAR_HEAP_NEW);

	// An Old 3DS must never be charged the New 3DS ceiling. This is the failure the runtime
	// pool sizing exists to prevent: today MESH_SLOTS is a compile-time constant, so raising
	// RENDER_DIST_MAX alone would allocate the wider pool on BOTH consoles and the Old 3DS
	// would not boot. Stated as a check so the ordering constraint is enforced, not remembered.
	CHECK(pool_new + RENDER_DIST_LINEAR_OVERHEAD_BYTES > RENDER_DIST_LINEAR_HEAP_OLD);

	printf("per-console ceilings:\n");
	printf("  Old 3DS  max radius %d  pool %zu + overhead %u = %zu  vs heap %u\n",
	       RENDER_DIST_MAX_OLD, pool_old, RENDER_DIST_LINEAR_OVERHEAD_BYTES,
	       pool_old + RENDER_DIST_LINEAR_OVERHEAD_BYTES, RENDER_DIST_LINEAR_HEAP_OLD);
	printf("  New 3DS  max radius %d  pool %zu + overhead %u = %zu  vs heap %u\n",
	       RENDER_DIST_MAX_NEW, pool_new, RENDER_DIST_LINEAR_OVERHEAD_BYTES,
	       pool_new + RENDER_DIST_LINEAR_OVERHEAD_BYTES, RENDER_DIST_LINEAR_HEAP_NEW);
	printf("  one further (radius %d) needs %zu, which is %zu over the New 3DS heap\n",
	       RENDER_DIST_MAX_NEW + 1, pool_over + RENDER_DIST_LINEAR_OVERHEAD_BYTES,
	       (pool_over + RENDER_DIST_LINEAR_OVERHEAD_BYTES) - RENDER_DIST_LINEAR_HEAP_NEW);

	printf("mesh pool bytes self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}
