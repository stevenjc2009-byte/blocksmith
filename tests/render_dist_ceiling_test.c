// Host gate for the per-console render-distance ceiling — renderDistMaxFor(),
// renderDistClampFor() and the default that has to sit inside them (scene/render_dist.c).
//
// tests/mesh_pool_bytes_test.c already proves the two CONSTANTS: that RENDER_DIST_MAX_OLD and
// RENDER_DIST_MAX_NEW each fit their console's linear heap and that one radius past NEW does
// not. This binary proves the other half — that the FUNCTIONS which hand those constants to the
// rest of the game cannot hand out a radius the mesh pool has no slots for.
//
// The distinction is the whole point of the file. Today RENDER_DIST_MAX is 3 while
// RENDER_DIST_MAX_NEW is 5, because scene/chunk_render.c's chunkRenderInit still sizes its
// arenas from the compile-time constant on both models. Until that is runtime-sized, a New 3DS
// asked for radius 5 would not get a wider ring; it would get acquireSlot returning NULL and
// chunks that never mesh at all — the terrain-hole failure v1.7.0 paid a release for.
// renderDistMaxFor() takes the min of the two for exactly that reason, and everything below
// checks that it still does.
//
// ── WHY THE CHECKS ARE RELATIONS AND NOT LITERALS ────────────────────────────────────────
//
// At RENDER_DIST_MAX 3 both consoles are pinned to 3, so every check that tries to tell the two
// models apart is dead in the shipped configuration. Writing `CHECK(renderDistMaxFor(true) == 5)`
// here would simply be red today and would prove nothing about the day it is not.
//
// So this file is written to be run in TWO arms from one source: once against the real header
// (RENDER_DIST_MAX 3, both models pinned) and once against a SHADOW copy of scene/render_dist.h
// whose only difference is RENDER_DIST_MAX 5 — the same shadowing technique render_dist.h's own
// v1.8.5 re-measurement used, and never an edit to the tree. The `RENDER_DIST_MAX >=
// RENDER_DIST_MAX_NEW` branch below is what fires in the wide arm; it is the check that the two
// consoles actually diverge once the pool can hold it.
//
// Own main(), same pattern as tests/mesh_pool_bytes_test.c and tests/hw_test.c.
#include <stdbool.h>
#include <stdio.h>

#include "scene/render_dist.h"

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

// The min the implementation is supposed to be taking, spelled once here so the two tie-down
// checks below read as one rule rather than as two hand-computed numbers that happen to match.
#define EXPECT_MAX_FOR(model_ceiling) \
	((model_ceiling) < RENDER_DIST_MAX ? (model_ceiling) : RENDER_DIST_MAX)

int main(void)
{
	const int max_old = renderDistMaxFor(false);
	const int max_new = renderDistMaxFor(true);

	printf("render distance ceilings, from scene/render_dist.c:\n");
	printf("  compile-time pool ceiling RENDER_DIST_MAX=%d  (MIN=%d)\n",
	       RENDER_DIST_MAX, RENDER_DIST_MIN);
	printf("  per-model heap ceilings   OLD=%d  NEW=%d\n",
	       RENDER_DIST_MAX_OLD, RENDER_DIST_MAX_NEW);
	printf("  renderDistMaxFor          Old 3DS %d   New 3DS %d\n", max_old, max_new);
	printf("  renderDistDefault         Old 3DS %d   New 3DS %d\n",
	       renderDistDefault(false), renderDistDefault(true));

	// ── 1. The ceiling accessor ───────────────────────────────────────────────────────────

	// The rule, tied down. Not a literal, so it holds in both arms.
	CHECK(max_old == EXPECT_MAX_FOR(RENDER_DIST_MAX_OLD));
	CHECK(max_new == EXPECT_MAX_FOR(RENDER_DIST_MAX_NEW));

	// A New 3DS is never offered a narrower ring than an Old 3DS. Non-strict on purpose: at
	// today's RENDER_DIST_MAX the two are equal, and equal is correct — the branch further down
	// is where the strict version lives.
	CHECK(max_new >= max_old);

	// THE LOAD-BEARING PAIR. Neither model may be handed a radius past what the mesh pool was
	// allocated for, because MESH_SLOTS is RENDER_DIST_MAX_SLOTS and chunkRenderInit claims that
	// at boot from the compile-time constant on every console. If either of these goes red, some
	// console is being offered slots that do not exist.
	CHECK(max_old <= RENDER_DIST_MAX);
	CHECK(max_new <= RENDER_DIST_MAX);

	// And a ceiling below the floor would leave renderDistClampFor with an empty range.
	CHECK(max_old >= RENDER_DIST_MIN);
	CHECK(max_new >= RENDER_DIST_MIN);

	if (RENDER_DIST_MAX >= RENDER_DIST_MAX_NEW) {
		// The wide arm — what the shipped build becomes once chunkRenderInit sizes its arenas
		// from the selected radius and RENDER_DIST_MAX is lifted. The whole feature is here: the
		// two consoles must actually diverge, and each must land on its own measured ceiling.
		printf("  (pool ceiling is wide enough for the New 3DS ceiling: checking divergence)\n");
		CHECK(max_new == RENDER_DIST_MAX_NEW);
		CHECK(max_old == RENDER_DIST_MAX_OLD);
		CHECK(max_new > max_old);
	} else {
		// The shipped arm — the pool binds, so BOTH models are pinned to it and must not differ.
		// This is the check that stops the ceiling being lifted here instead of where it belongs.
		printf("  (pool ceiling binds: both models pinned to RENDER_DIST_MAX)\n");
		CHECK(max_new == RENDER_DIST_MAX);
		CHECK(max_old == RENDER_DIST_MAX);
		CHECK(max_new == max_old);
	}

	// ── 2. The console-aware clamp ────────────────────────────────────────────────────────

	for (int model = 0; model < 2; model++) {
		const bool n   = (model == 1);
		const int  hi  = renderDistMaxFor(n);

		// Both ends, including values a hand-edited options.ini can hold.
		CHECK(renderDistClampFor(RENDER_DIST_MIN - 1, n) == RENDER_DIST_MIN);
		CHECK(renderDistClampFor(0, n)                   == RENDER_DIST_MIN);
		CHECK(renderDistClampFor(-99, n)                 == RENDER_DIST_MIN);
		CHECK(renderDistClampFor(hi + 1, n)              == hi);
		CHECK(renderDistClampFor(99, n)                  == hi);

		// In range, it must be the identity — a clamp that quietly moved a legal value would be
		// a setting the player cannot select.
		for (int r = RENDER_DIST_MIN; r <= hi; r++)
			CHECK(renderDistClampFor(r, n) == r);

		// Monotone, and the range it produces is exactly [MIN, hi]. Sweeps well past both ends
		// so the widest ceiling this project has a constant for is covered in every arm.
		int prev = renderDistClampFor(-9, n);
		for (int r = -8; r <= RENDER_DIST_MAX_NEW + 4; r++) {
			const int c = renderDistClampFor(r, n);
			CHECK(c >= prev);
			CHECK(c >= RENDER_DIST_MIN && c <= hi);
			prev = c;
		}

		// THE COHERENCE INVARIANT, and the reason there are two clamps rather than one.
		// renderDistFor() keeps its own unconditional clamp to RENDER_DIST_MAX, which guards the
		// pool; this one guards the setting. They must never disagree — if they do, the options
		// slider reads one radius while the renderer draws another and nothing reports it.
		// Stated as a fixed point: whatever the setting clamp allows, the pool clamp must leave
		// alone.
		for (int r = -4; r <= RENDER_DIST_MAX_NEW + 4; r++) {
			const int c = renderDistClampFor(r, n);
			CHECK(renderDistFor(c).radius == c);
		}
	}

	// ── 3. The default, which has to live inside all of that ──────────────────────────────

	// Unchanged behaviour, restated here rather than only in world_test.c because v1.8.5 routes
	// the default through renderDistClampFor and this is the file that would notice if the
	// routing moved it.
	CHECK(renderDistDefault(true)  == RENDER_DIST_DEFAULT_NEW);
	CHECK(renderDistDefault(false) == RENDER_DIST_MIN);

	for (int model = 0; model < 2; model++) {
		const bool n = (model == 1);
		const int  d = renderDistDefault(n);

		// Reachable: a default the clamp would move is not a default, it is a value that gets
		// silently rewritten at first boot.
		CHECK(d >= RENDER_DIST_MIN && d <= renderDistMaxFor(n));
		CHECK(renderDistClampFor(d, n) == d);

		// The v1.6.0 task 12 decision recorded at RENDER_DIST_DEFAULT_NEW, restated in the
		// console-aware world: a default is what ships to everyone unattended and a ceiling is
		// what a player opts into, so the two must not be the same number. Written strictly
		// rather than as `!=` so that a default which somehow ended up ABOVE the ceiling fails
		// here too, rather than only failing the clamp check above.
		CHECK(d < renderDistMaxFor(n));
	}

	printf("render distance ceiling self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}
