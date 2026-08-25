// Host self-test for chunkRenderProfileReset() / chunkRenderProfile() / chunkRenderVisUs() —
// source/scene/chunk_render.c's build profiler.
//
// v1.8.0 task 49h: chunkRenderProfileReset() zeroed s_scratch_ticks, s_mesh_ticks and
// s_build_count and NOT s_vis_ticks, while chunkRenderVisUs() divides s_vis_ticks by
// s_build_count. So the first build after a reset reported every tick accumulated since boot
// as one build's flood-fill cost, and it got worse with every reset — the counter kept
// climbing while the divisor kept restarting. Every visibility microsecond figure this project
// has printed since main.c's boot profile is therefore wrong, including the "0.63 ms" quoted in
// chunk_render.h's chunkRenderTouch comment and in main.c's drain-budget arithmetic.
//
// Same build trick as tests/horizon_test.c and for the same reason: chunk_render.c includes
// <3ds.h> and <citro3d.h> and cannot be compiled on the host, so tools/run_host_tests.sh lifts
// the three functions' REAL source text out of it with awk and this file #includes the result.
// A hand-copy here would pass forever with the bug back in the shipped file, which is precisely
// the failure mode this project has already paid for (see run_host_tests.sh's meshq note).
#ifndef __3DS__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int  s_checks;
static int  s_fails;
static char s_first[200];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── The state the three functions read ─────────────────────────────────────────────────
//
// Same names and same types as chunk_render.c's. u64 is devkitPro's; the host spelling is
// uint64_t and nothing here depends on which.
typedef uint64_t u64;

static u64 s_scratch_ticks;
static u64 s_mesh_ticks;
static u64 s_vis_ticks;
static int s_build_count;

// libctru's ARM11 clock. Its VALUE is irrelevant to every check below — it is a constant
// divisor applied identically to both readings being compared — so a copy here cannot make a
// broken profiler look fixed. It is defined only because the extracted code names it.
#define SYSCLOCK_ARM11  268111856

// The real functions. Generated at build time — see the file comment.
#include "profile_extract.inc"

// One chunk build's worth of ticks, three distinguishable numbers so a mixed-up bucket shows
// up as a wrong value rather than as a coincidence.
#define BUILD_SCRATCH  7000
#define BUILD_MESH     9000
#define BUILD_VIS      5000

static void oneBuild(void)
{
	s_scratch_ticks += BUILD_SCRATCH;
	s_mesh_ticks    += BUILD_MESH;
	s_vis_ticks     += BUILD_VIS;
	s_build_count++;
}

int main(void)
{
	// Boot: a long run of builds nobody is measuring, exactly what main.c's reset exists to
	// throw away. The vis counter is deliberately huge next to one build's, because that is
	// what "ticks since boot" looks like against "ticks for one chunk".
	for (int i = 0; i < 400; i++) {
		s_scratch_ticks += 2000;
		s_mesh_ticks    += 3000;
		s_vis_ticks     += 2500;
		s_build_count++;
	}
	CHECK(s_vis_ticks == 400u * 2500u);

	// ── Arm 1: the reset must actually clear the counter it is named after ──────────────
	chunkRenderProfileReset();
	CHECK(s_scratch_ticks == 0);
	CHECK(s_mesh_ticks == 0);
	CHECK(s_build_count == 0);
	CHECK(s_vis_ticks == 0);          // <- the bug, in one line

	// ── Arm 2: the reading itself, which is what anyone actually looks at ───────────────
	//
	// Two identical measurement windows, each one reset followed by exactly one identical
	// build. Whatever the profiler reports for the first, it must report for the second: the
	// windows are the same, so the numbers are the same. With s_vis_ticks left un-reset the
	// second window inherits the first, and the two diverge — which is the whole defect,
	// stated without needing to know what a tick is worth.
	oneBuild();
	float vis_a = chunkRenderVisUs();
	float scratch_a = 0.0f, mesh_a = 0.0f;
	int   builds_a = 0;
	chunkRenderProfile(&scratch_a, &mesh_a, &builds_a);

	chunkRenderProfileReset();
	oneBuild();
	float vis_b = chunkRenderVisUs();
	float scratch_b = 0.0f, mesh_b = 0.0f;
	int   builds_b = 0;
	chunkRenderProfile(&scratch_b, &mesh_b, &builds_b);

	CHECK(vis_a == vis_b);

	// The absolute value too, so "both windows read the same wrong number" cannot pass. The
	// scale is computed here exactly as chunkRenderVisUs computes it, so this asserts the
	// accounting and not the clock.
	const float per_us = (float)(SYSCLOCK_ARM11 / 1000000);
	CHECK(vis_a == (float)BUILD_VIS / per_us);
	CHECK(vis_b == (float)BUILD_VIS / per_us);

	// ── The control ────────────────────────────────────────────────────────────────────
	//
	// The two buckets that WERE already being reset. These stay green whether or not
	// s_vis_ticks is cleared, so a red run above is a red for the right reason and not a
	// build that fell over.
	CHECK(scratch_a == scratch_b);
	CHECK(mesh_a == mesh_b);
	CHECK(builds_a == 1);
	CHECK(builds_b == 1);
	CHECK(scratch_a == (float)BUILD_SCRATCH / per_us);
	CHECK(mesh_a == (float)BUILD_MESH / per_us);

	// ── And the divide-by-zero floor, which predates 49h and must survive it ───────────
	chunkRenderProfileReset();
	CHECK(chunkRenderVisUs() == 0.0f);      // 0 ticks over the n=1 floor, not a NaN
	int builds_zero = -1;
	chunkRenderProfile(NULL, NULL, &builds_zero);
	CHECK(builds_zero == 0);

	// ── The check count, pinned as a bare number, and it must stay LAST in main() ────────
	//
	// No suite in this project asserted its own check count until 2026-08-25, and that absence
	// is what let a self-referential capacity check elsewhere shrink a bound from 15 to 7,
	// DELETE eight assertions, and still print "0 failed" (326 checks became 318). Any sabotage
	// that shortens a loop bounded by a production constant removes checks instead of failing
	// them, and the only thing that notices is a pinned total. 17 counts this line itself,
	// because CHECK increments before it compares. If a check is added, this number moves with
	// it — that is the point, not a nuisance.
	CHECK(s_checks == 17);

	if (s_fails == 0)
		printf("profile reset self-test: PASS  %d checks\n", s_checks);
	else
		printf("profile reset self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int profile_reset_test_host_only_t;

#endif   // !__3DS__
