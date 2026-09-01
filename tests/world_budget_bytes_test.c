// Host probe and GATE for the world store's application-heap cost at a render radius, and
// for the constant that caps it — world/budget.h's WORLD_BUDGET_BYTES.
//
// Same shape and same job as tests/mesh_pool_bytes_test.c, which does this for the chunk mesh
// pool's LINEAR heap. The two are deliberately separate binaries because they are separate
// heaps: the mesh pool is linearAlloc and the world store is calloc/malloc, and app/heapsplit.h
// is the file that had to work that out before the New 3DS could be given a wider ring at all.
// A New 3DS at radius 5 spends 46,948,352 bytes of its 64 MB linear heap on the pool; the
// world store's 11,160,160 comes out of the 59,715,584 bytes of application heap left beside
// it. They do not compete, and this binary is where that stops being a claim.
//
// ── Why the numbers are carried rather than computed ──────────────────────────────────────
//
// The two sizes that matter most are opaque types. `struct Chunk` is defined only in
// world/chunk.c and `LightColumn` only in world/light.c, on purpose — nothing outside those
// files is allowed to know their layout. So this binary cannot take sizeof() of either, and
// it does the next best thing: it carries the TARGET sizes as measured constants, and it
// pins them to values it CAN reach on the host, so that a change to any of the structures
// moves the host figure, fails a check here, and forces the target figure to be re-measured.
//
// The target numbers came off the compiler, not off paper. Compiled with the console
// toolchain — arm-none-eabi-gcc (devkitARM) 16.1.0, -march=armv6k -mtune=mpcore
// -mfloat-abi=hard -mtp=soft — and read out of the emitted .word constants:
//
//     sizeof(Column)              48        (host x86-64: 88)
//     sizeof(struct Chunk)         8        (host x86-64: 16)
//     chunkFormBytes(RAW)      4,104        (host x86-64: 4,112)
//     sizeof(LightColumn)     32,768        (same both — two uint8_t arrays)
//     ---------------------------------
//     one loaded column       65,648        (host x86-64: 65,752)
//
// The chunk header is 8 bytes on the console and 16 on the host, and paper arithmetic gets
// that wrong in BOTH directions: the pointer is 4 bytes rather than 8, and the ChunkForm enum
// collapses to one byte because the ARM EABI defaults to -fshort-enums. Deriving it by hand
// gives 12. This is why the number is measured.
//
// The host figure is the larger of the two on every term — every difference is pointer width
// and 3DS pointers are smaller — so the host arithmetic is an upper bound on the console's,
// 0.16 % high. Both are checked below, and they must reach the same verdict at every radius;
// if they ever disagreed, the 0.16 % would have grown into something that matters.
//
// Own main(), same pattern as tests/mesh_pool_bytes_test.c and the other host-only binaries
// tools/run_host_tests.sh builds and runs.
#include <stdio.h>

#include "app/heapsplit.h"
#include "scene/render_dist.h"
#include "world/budget.h"
#include "world/chunk.h"
#include "world/light.h"
#include "world/world.h"

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

// ── The console's sizes, measured (see the header comment for the toolchain and flags) ─────
#define TARGET_COLUMN_STRUCT_BYTES   48u
#define TARGET_CHUNK_MAX_FORM_BYTES  4104u
#define TARGET_LIGHT_COLUMN_BYTES    32768u

// ── The same three on this host, which is what pins the three above to reality ─────────────
#define HOST_COLUMN_STRUCT_BYTES     88u
#define HOST_CHUNK_MAX_FORM_BYTES    4112u

// The New 3DS application heap, MEASURED after app/heapsplit.c takes 64 MB for the linear
// heap: app/heapsplit.h's header comment, and the same reading tests/heapsplit_test.c gates.
// This is the pool the world store actually comes out of.
#define APP_HEAP_NEW_3DS  59715584u

int main(void)
{
	// ── 1. Tie the carried target constants to something this binary can actually reach ────
	//
	// sizeof(Column) is reachable — world.h is not opaque about it — so it is checked outright.
	// The chunk header and the light column are not, so they are checked through the two public
	// surfaces that DO expose their size: chunkFormBytes(), which world.c already trusts to size
	// every claim it makes, and LIGHT_COL_BYTES, which light.h _Static_asserts and out of which
	// LightColumn is exactly two arrays. If any of these three moves, the target figures above
	// are stale and every byte total below is fiction.
	CHECK(sizeof(Column) == HOST_COLUMN_STRUCT_BYTES);
	CHECK(chunkFormBytes(CHUNK_FORM_RAW) == HOST_CHUNK_MAX_FORM_BYTES);
	CHECK(chunkFormBytes(CHUNK_FORM_RAW) >= chunkFormBytes(CHUNK_FORM_PALETTE4));
	CHECK(chunkFormBytes(CHUNK_FORM_RAW) >= chunkFormBytes(CHUNK_FORM_UNIFORM));
	CHECK(2u * LIGHT_COL_BYTES == TARGET_LIGHT_COLUMN_BYTES);
	CHECK(COLUMN_CHUNKS == 8);

	// Every target term is at or below its host term, because the only difference is pointer
	// width. Stated as a check rather than as a sentence: it is what makes the host arithmetic
	// a safe upper bound, and it is the thing that would break first if the two ABIs diverged
	// for some other reason.
	CHECK(TARGET_COLUMN_STRUCT_BYTES  <= HOST_COLUMN_STRUCT_BYTES);
	CHECK(TARGET_CHUNK_MAX_FORM_BYTES <= HOST_CHUNK_MAX_FORM_BYTES);

	const size_t col_target = budgetColumnBytes(TARGET_COLUMN_STRUCT_BYTES, COLUMN_CHUNKS,
	                                            TARGET_CHUNK_MAX_FORM_BYTES,
	                                            TARGET_LIGHT_COLUMN_BYTES);
	const size_t col_host   = budgetColumnBytes(sizeof(Column), COLUMN_CHUNKS,
	                                            chunkFormBytes(CHUNK_FORM_RAW),
	                                            2u * LIGHT_COL_BYTES);

	CHECK(col_target == 65648u);
	CHECK(col_host   == 65752u);

	printf("world store bytes, formula from world/budget.h:\n");
	printf("  per column: Column %u + %d chunks x %u (RAW) + light %u = %zu B  console\n",
	       TARGET_COLUMN_STRUCT_BYTES, COLUMN_CHUNKS, TARGET_CHUNK_MAX_FORM_BYTES,
	       TARGET_LIGHT_COLUMN_BYTES, col_target);
	printf("              (this host, upper bound: %zu B)\n", col_host);
	printf("  cap WORLD_BUDGET_BYTES = %u B  staging columns %d\n",
	       WORLD_BUDGET_BYTES, BUDGET_STAGING_COLUMNS);

	for (int r = RENDER_DIST_MIN; r <= RENDER_DIST_MAX_NEW + 1; r++) {
		const int    cols = budgetColumnsForRadius(r);
		const size_t need = budgetBytesForRadius(r, col_target);

		printf("  radius %d: loaded ring %dx%d = %3d columns +%d staging  bytes=%8zu  "
		       "MB=%5.2f  %s\n",
		       r, 2 * (r + 1) + 1, 2 * (r + 1) + 1, cols, BUDGET_STAGING_COLUMNS, need,
		       (double)need / (1024.0 * 1024.0),
		       need <= WORLD_BUDGET_BYTES ? "fits" : "OVER CAP");
	}

	// ── 2. The gate. Everything above prints; what follows decides. ────────────────────────
	//
	// The cap has to hold the widest ring each console can select. scene/render_dist.h owns
	// those two ceilings and tests/mesh_pool_bytes_test.c gates them against the LINEAR heap;
	// this is the other half of the same question, against the application heap.
	const size_t need_old  = budgetBytesForRadius(RENDER_DIST_MAX_OLD, col_target);
	const size_t need_new  = budgetBytesForRadius(RENDER_DIST_MAX_NEW, col_target);
	const size_t need_over = budgetBytesForRadius(RENDER_DIST_MAX_NEW + 1, col_target);

	CHECK(need_old <= WORLD_BUDGET_BYTES);
	CHECK(need_new <= WORLD_BUDGET_BYTES);

	// MAXIMALITY, and it is the whole reason the two fits above say anything. Without it they
	// would pass just as happily with the cap at 32 MB, or 64, and would be asserting only that
	// some number large enough exists. With it, 12 MB is pinned as very nearly the smallest cap
	// that holds radius 5 — one radius further is over by 2,253,536 bytes — which makes it a
	// measurement rather than a round number somebody liked.
	CHECK(need_over > WORLD_BUDGET_BYTES);

	// The host's own upper-bound arithmetic must reach the same three verdicts. If it ever did
	// not, the 0.16 % gap between the two ABIs would have become load-bearing and the console
	// figures could no longer be checked from here at all.
	CHECK(budgetBytesForRadius(RENDER_DIST_MAX_OLD,     col_host) <= WORLD_BUDGET_BYTES);
	CHECK(budgetBytesForRadius(RENDER_DIST_MAX_NEW,     col_host) <= WORLD_BUDGET_BYTES);
	CHECK(budgetBytesForRadius(RENDER_DIST_MAX_NEW + 1, col_host) >  WORLD_BUDGET_BYTES);

	// Why one cap serves both consoles instead of a per-model one. The cap allocates nothing —
	// budget.c is three scalars and a comparison — so charging an Old 3DS the shared figure
	// costs it no memory at all; what it buys is margin. This check states the margin: the Old
	// 3DS ceiling fits TWICE over, so the constant is not merely adequate there, it is the
	// reason a per-console cap would be machinery with nothing to do. The day the Old 3DS ring
	// costs more than half the cap, that stops being true and this goes red.
	CHECK(need_old * 2u <= WORLD_BUDGET_BYTES);
	CHECK(need_old < need_new);

	// The cap is a ceiling on the APPLICATION heap, so it has to fit inside one. Both consoles,
	// against the two figures app/heapsplit.h establishes: the measured New 3DS heap after the
	// split, and the floor the policy refuses to go below on either machine.
	CHECK((size_t)WORLD_BUDGET_BYTES <= APP_HEAP_NEW_3DS);
	CHECK((size_t)WORLD_BUDGET_BYTES <= HEAPSPLIT_APP_FLOOR_BYTES);

	// ── 3. The real budget, not arithmetic about it ────────────────────────────────────────
	//
	// Everything above compares numbers to a macro. These four run the shipped budgetClaim()
	// against the shipped cap, so a change that made the cap and the claim path disagree — the
	// counter widened, the comparison inverted, a cap that stopped being what budgetCap()
	// returns — is caught here and not only in world_test.c's unit checks.
	budgetReset();
	CHECK(budgetCap() == WORLD_BUDGET_BYTES);
	CHECK(budgetUsed() == 0);              // the cap reserves nothing; it only compares
	CHECK(budgetClaim(need_new) == true);  // a whole radius-5 world, worst case, fits
	budgetReset();
	printf("  (one BUDGET REFUSED line below is expected — the radius %d claim being refused)\n",
	       RENDER_DIST_MAX_NEW + 1);
	CHECK(budgetClaim(need_over) == false);
	CHECK(budgetRefusals() == 1);
	budgetReset();

	printf("per-console ceilings, application heap:\n");
	printf("  Old 3DS  max radius %d  world store %zu of cap %u  (%.1f %%)\n",
	       RENDER_DIST_MAX_OLD, need_old, WORLD_BUDGET_BYTES,
	       100.0 * (double)need_old / (double)WORLD_BUDGET_BYTES);
	printf("  New 3DS  max radius %d  world store %zu of cap %u  (%.1f %%)\n",
	       RENDER_DIST_MAX_NEW, need_new, WORLD_BUDGET_BYTES,
	       100.0 * (double)need_new / (double)WORLD_BUDGET_BYTES);
	// The subtraction is guarded because this line has to stay readable in a RED run: with a
	// raised cap need_over is the smaller number, and an unsigned wrap would print 1.8e19 and
	// make the failure look like an arithmetic accident instead of the deliberate one it is.
	if (need_over > WORLD_BUDGET_BYTES)
		printf("  one further (radius %d) needs %zu, which is %zu over the cap\n",
		       RENDER_DIST_MAX_NEW + 1, need_over, need_over - WORLD_BUDGET_BYTES);
	else
		printf("  one further (radius %d) needs %zu, which the cap STILL HOLDS with %zu spare\n",
		       RENDER_DIST_MAX_NEW + 1, need_over, (size_t)WORLD_BUDGET_BYTES - need_over);
	printf("  cap %u vs New 3DS application heap %u (measured) — %.1f %% of it\n",
	       WORLD_BUDGET_BYTES, APP_HEAP_NEW_3DS,
	       100.0 * (double)WORLD_BUDGET_BYTES / (double)APP_HEAP_NEW_3DS);

	printf("world budget bytes self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}
