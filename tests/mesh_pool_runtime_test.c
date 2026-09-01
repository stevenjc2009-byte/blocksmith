// Host self-test for the RUNTIME sizing of scene/chunk_render.c's three linearAlloc mesh-pool
// tier arenas — v1.8.5's load-bearing change.
//
// THE DEFECT THIS EXISTS FOR, stated as the thing that must never happen. Until v1.8.5 those
// arenas were sized from MESH_SLOTS, a COMPILE-TIME constant derived from RENDER_DIST_MAX. The
// pool costs 9,199,616 bytes at radius 3 and 46,948,352 at radius 5, and an Old 3DS has a
// 33,554,432-byte linear heap with 1,874,944 of it already spent on screen and GPU buffers
// before chunkRenderInit runs. So raising RENDER_DIST_MAX to 5 to give a New 3DS the wider ring
// would have charged an Old 3DS 48,823,296 bytes out of 33,554,432 and it would not have
// booted — no message, no frame, the v1.6.0 failure again with a different constant.
// tests/mesh_pool_bytes_test.c's L106 states that ordering constraint as a check; this file is
// what makes the constraint stop applying, so it is the file that has to prove the pool is
// sized from the radius chunkRenderInit is HANDED and not from the ceiling it was built with.
//
// HOW IT AVOIDS BEING A HAND-COPY. scene/chunk_render.c includes <3ds.h> and <citro3d.h> and
// cannot be compiled on this host, so tools/run_host_tests.sh lifts the REAL source text of
// poolSlotsForRadius() and poolTierSlots(), plus the tier #defines they read, out of that file
// with awk and this file #includes the result — exactly the technique tests/horizon_test.c and
// tests/profile_reset_test.c already use on the same file, and for the reason recorded against
// battery_test and sleep_test in run_host_tests.sh: a test that carries its own copy of the
// logic passes forever with the shipped code broken. There is no second copy here. Sabotage
// chunk_render.c's sizing and this binary goes red.
//
// The byte totals come from scene/mesh_pool_sizing.h — the same meshPoolTotalBytes()
// chunkRenderInit itself calls and tests/mesh_pool_bytes_test.c gates the per-console ceilings
// with — not from arithmetic restated here.
//
// Own main(), same pattern as tests/mesh_pool_bytes_test.c and every other host-only binary.
#ifndef __3DS__

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scene/mesh_pool_sizing.h"
#include "scene/render_dist.h"
#include "world/dirtyq.h"
#include "world/mesh_vertex.h"

// The real source text of the two sizing functions and the constants they read, extracted from
// source/scene/chunk_render.c at build time by tools/run_host_tests.sh. See this file's header
// comment for why it is an extraction and not a copy.
#include "pool_sizing_extract.inc"

// Which file the source-text checks at the bottom read. Overridable so a red arm can point at a
// SHADOW copy of chunk_render.c instead of sabotaging the production file — a red arm left
// behind in real code compiles and reads as a deliberate constant, which has happened here.
#ifndef CHUNK_RENDER_SRC
#define CHUNK_RENDER_SRC "source/scene/chunk_render.c"
#endif

static int  s_checks;
static int  s_fails;
static char s_first[200];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                           \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

static size_t poolBytesFor(int radius)
{
	return meshPoolTotalBytes(poolSlotsForRadius(radius), MESH_SLOT_FACES, sizeof(MeshVertex));
}

// ── The constants the extraction pulled in really are the ones the byte formula assumes ──────
//
// chunk_render.c carries a _Static_assert tying its TIER_S_SLOTS/TIER_M_SLOTS to
// mesh_pool_sizing.h's MESH_POOL_* copies, but that assert lives in a translation unit this
// host cannot compile. Restated here so the extraction cannot silently pull in constants that
// disagree with the formula every byte total below is computed through.
static void testExtractedConstantsMatchTheFormula(void)
{
	CHECK(TIER_S_SLOTS == MESH_POOL_TIER_S_SLOTS);
	CHECK(TIER_M_SLOTS == MESH_POOL_TIER_M_SLOTS);
	CHECK(sizeof(MeshVertex) == 8);
	CHECK(POOL_MIN_SLOTS == TIER_S_SLOTS + TIER_M_SLOTS + TIER_L_MIN_SLOTS);
}

// ── The success criterion, byte-for-byte ─────────────────────────────────────────────────────
//
// "At radius 3 the game allocates byte-for-byte what it allocates today, and at radius 5 it
// allocates 46,948,352, and which one happens is decided at runtime rather than at compile
// time." Radius 3 is checked in EVERY build, because RENDER_DIST_MAX is at least 3; radius 5 is
// checked whenever the build's ceiling allows it, which is how the same binary proves both
// halves when it is built against a header whose ceiling is 5.
static void testRadiusThreeIsUnchanged(void)
{
	int tiers[3];
	poolTierSlots(poolSlotsForRadius(3), tiers);

	CHECK(poolSlotsForRadius(3) == 392);
	CHECK(poolBytesFor(3) == 9199616u);

	// The split, not just the total: two pools with different tier counts can add to the same
	// number of slots and cost very different amounts, because an L slot is 65,536 bytes and an
	// S slot is 16,384.
	CHECK(tiers[0] == 256);
	CHECK(tiers[1] == 120);
	CHECK(tiers[2] == 16);
	CHECK(tiers[0] + tiers[1] + tiers[2] == poolSlotsForRadius(3));

	// The radius-3 pool fits an Old 3DS with the screen and GPU buffers counted. This is the
	// console that must keep booting, and it is the reason the arenas became runtime-sized.
	CHECK(poolBytesFor(3) + RENDER_DIST_LINEAR_OVERHEAD_BYTES <= RENDER_DIST_LINEAR_HEAP_OLD);
}

static void testRadiusFive(void)
{
#if RENDER_DIST_MAX >= 5
	int tiers[3];
	poolTierSlots(poolSlotsForRadius(5), tiers);

	CHECK(poolSlotsForRadius(5) == 968);
	CHECK(poolBytesFor(5) == 46948352u);

	// Every slot the wider ring adds lands in L — the relationship meshPoolTotalBytes() assumes
	// and the reason the cost grows faster than the slot count does. 592 = 968 - 256 - 120.
	CHECK(tiers[0] == 256);
	CHECK(tiers[1] == 120);
	CHECK(tiers[2] == 592);

	// The whole point of the change, in two lines: the same binary sizes a small pool for one
	// radius and a large one for another, and the large one is a New-3DS-only pool.
	CHECK(poolBytesFor(5) > poolBytesFor(3));
	CHECK(poolBytesFor(5) + RENDER_DIST_LINEAR_OVERHEAD_BYTES <= RENDER_DIST_LINEAR_HEAP_NEW);
	CHECK(poolBytesFor(5) + RENDER_DIST_LINEAR_OVERHEAD_BYTES > RENDER_DIST_LINEAR_HEAP_OLD);
#else
	// Today's build, where the ceiling is still 3. A radius the binary was not built for must
	// CLAMP to the ceiling and must never allocate past what the .bss slot tables can index —
	// this is the check that says the pool cannot outrun the arrays if an options file from a
	// wider build is read by a narrower one.
	CHECK(poolSlotsForRadius(5) == poolSlotsForRadius(RENDER_DIST_MAX));
	CHECK(poolSlotsForRadius(5) <= RENDER_DIST_MAX_SLOTS);
	CHECK(poolBytesFor(5) == poolBytesFor(3));
#endif
}

// ── The wrap, which is the trap this project has already stepped in once ─────────────────────
//
// meshPoolTierBytes() casts its slot count to size_t. A tier driven negative therefore does not
// make the total smaller, it makes it about 1.8e19 — and run_host_tests.sh's mesh_pool_bytes
// stanza records a red arm being REJECTED for going red through exactly that wrap. So the L
// tier must be positive at every radius this function can be handed, including absurd ones, and
// the total must stay a number a linear heap could plausibly hold.
static void testNoTierCanGoNegative(void)
{
	static const int kAbsurd[] = { INT_MIN, -1000, -1, 0, 1, 2, 3, 4, 5, 6, 7, 12, 1000, INT_MAX };

	for (unsigned i = 0; i < sizeof(kAbsurd) / sizeof(kAbsurd[0]); i++) {
		const int r     = kAbsurd[i];
		const int slots = poolSlotsForRadius(r);
		int       tiers[3];
		poolTierSlots(slots, tiers);

		// Inside the arrays, above the floor, and every tier positive.
		CHECK(slots >= POOL_MIN_SLOTS);
		CHECK(slots <= RENDER_DIST_MAX_SLOTS);
		CHECK(tiers[0] > 0 && tiers[1] > 0 && tiers[2] > 0);
		CHECK(tiers[0] + tiers[1] + tiers[2] == slots);

		// 1 GB is not a real limit anywhere; it is far above the largest console heap
		// (67,108,864) and far below the ~1.8e19 a wrapped size_t produces, so it separates
		// "big pool" from "the subtraction went negative" without pinning a number.
		const size_t bytes = meshPoolTotalBytes(slots, MESH_SLOT_FACES, sizeof(MeshVertex));
		CHECK(bytes > 0 && bytes < 1073741824u);
	}
}

// The clamp is monotonic: a bigger ceiling never buys a smaller pool. Cheap, and it is what
// would catch a clamp written with its two comparisons the wrong way round — which passes every
// single-radius check above.
static void testMonotonic(void)
{
	for (int r = 1; r < 12; r++)
		CHECK(poolSlotsForRadius(r) <= poolSlotsForRadius(r + 1));
}

// ── The remesh backlog is wide enough for the ceiling lift, BEFORE the lift ──────────────────
//
// world/dirtyq.h's DIRTYQ_MAX is a hand-maintained copy of MESH_SLOTS, and v1.6.0 shipped a
// build that refused to start because it was raised second. scene/chunk_render.c's
// _Static_assert turns that into a build error now — but only for somebody who is there to see
// it. This checks the backlog already covers the widest ring any console will be allowed, so
// the ceiling lift is a one-line change and cannot resurrect that failure.
static void testDirtyqCoversTheWidestRing(void)
{
	CHECK(DIRTYQ_MAX >= meshSlotsForRadius(RENDER_DIST_MAX_NEW, RENDER_DIST_SLOTS_PER_COLUMN));
	CHECK(DIRTYQ_MAX >= meshSlotsForRadius(RENDER_DIST_MAX_OLD, RENDER_DIST_SLOTS_PER_COLUMN));
	CHECK(DIRTYQ_MAX >= RENDER_DIST_MAX_SLOTS);

	// The capacity chunkRenderInit actually asks for is the RUNTIME pool, so every radius has
	// to be acceptable to dirtyqInit — not just the ceiling.
	for (int r = 1; r < 12; r++)
		CHECK(poolSlotsForRadius(r) <= DIRTYQ_MAX);
}

// ── Nothing walks the pool by the compile-time ceiling any more ──────────────────────────────
//
// The extraction above proves the SIZING is right. This proves the CONSUMPTION is: a slot past
// s_pool_slots has a NULL `verts` and no arena behind it, so a loop left at `i < MESH_SLOTS`
// would hand the GPU a draw out of address 0 — and on this console that is silent corruption,
// not a fault, so it would present as terrain flickering somewhere unrelated. There are twelve
// such loops in the file and no compiler diagnostic for getting one of them wrong.
//
// A source-text check because there is no other kind available: chunk_render.c cannot be linked
// here, and the loops are not functions that can be extracted and driven. The same technique
// app/session_test.c uses to prove sessionBegin() is actually wired into main.c's lap.
static void testNoLoopUsesTheCompileTimeCeiling(void)
{
	FILE* f = fopen(CHUNK_RENDER_SRC, "rb");
	CHECK(f != NULL);
	if (!f) {
		printf("  (cannot open %s — run this binary from the repository root)\n",
		       CHUNK_RENDER_SRC);
		return;
	}

	static char src[1 << 20];
	const size_t n = fread(src, 1, sizeof(src) - 1, f);
	fclose(f);
	src[n] = '\0';

	// Non-vacuity first: a check over an empty or truncated read would pass by default, and
	// this project has a recorded case of exactly that.
	CHECK(n > 50000);
	CHECK(strstr(src, "static int s_pool_slots;") != NULL);
	CHECK(strstr(src, "s_pool_slots = poolSlotsForRadius(max_radius);") != NULL);
	CHECK(strstr(src, "bool chunkRenderInit(int max_radius)") != NULL);
	CHECK(strstr(src, "dirtyqInit(&s_dirty, s_pool_slots)") != NULL);

	// And the actual claim. Every loop over the slot table must be bounded by the runtime
	// count; "< MESH_SLOTS" is how all twelve of them used to be written and is not needed by
	// anything that remains (the surviving uses of the macro size arrays, or compare with >=).
	CHECK(strstr(src, "< MESH_SLOTS") == NULL);

	// The guards that make a mis-sized pool fail at boot instead of at a random draw.
	CHECK(strstr(src, "if (next_slot != s_pool_slots)") != NULL);
	CHECK(strstr(src, "if (s_bytes != meshPoolTotalBytes(s_pool_slots, MESH_SLOT_FACES, "
	                  "sizeof(MeshVertex)))") != NULL);
}

int main(void)
{
	printf("mesh pool RUNTIME sizing, through the real poolSlotsForRadius/poolTierSlots:\n");
	printf("  built at RENDER_DIST_MAX=%d  MESH_SLOTS=%d  POOL_MIN_SLOTS=%d  DIRTYQ_MAX=%d\n",
	       RENDER_DIST_MAX, RENDER_DIST_MAX_SLOTS, POOL_MIN_SLOTS, DIRTYQ_MAX);

	for (int r = 1; r <= 6; r++) {
		const int slots = poolSlotsForRadius(r);
		int       tiers[3];
		poolTierSlots(slots, tiers);
		printf("  asked radius %d -> slots=%4d  tiers S%d/M%d/L%-4d  bytes=%10zu\n",
		       r, slots, tiers[0], tiers[1], tiers[2],
		       meshPoolTotalBytes(slots, MESH_SLOT_FACES, sizeof(MeshVertex)));
	}

	testExtractedConstantsMatchTheFormula();
	testRadiusThreeIsUnchanged();
	testRadiusFive();
	testNoTierCanGoNegative();
	testMonotonic();
	testDirtyqCoversTheWidestRing();
	testNoLoopUsesTheCompileTimeCeiling();

	printf("mesh pool runtime self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif   // !__3DS__
