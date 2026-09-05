// tools/sortbench.c
//
// Host benchmark re-deriving the measurement table in the comment above the chunk depth sort
// in source/scene/chunk_render.c (the block starting "Sort nearest first: an LSD radix over
// the depth's float bit pattern", around lines 3042-3127 as of the commit named below).
//
// That comment's own prose contradicts its own table for the "reverse (worst case)" row: the
// table says the new sort measured 0.0051 ms there, the prose two lines later says 0.0089 ms
// (repeated in docs/VERSION-LIST.md under "29x"). The probe that produced the original numbers
// lived in a session scratchpad that no longer exists, so neither figure could be re-checked
// against it. This file rebuilds a probe from scratch, self-contained and left in the repo so
// this does not happen again.
//
// ── Provenance of the two sorts below — both lifted VERBATIM, not reconstructed from memory ──
//
// radixSort(): copied unmodified from source/scene/chunk_render.c, lines 3128-3187, inside
// cullFrame()'s `#if BS_SORT` block. This code is CURRENTLY LIVE in the working tree.
//
// insertionSort(): this sort no longer exists anywhere in the working tree — it was removed by
// commit f8c4dc22898207dda413cf1df4ff07ad8f5bda42 ("feat(v1.8.12): six ores, a craftable torch,
// and a pass over the per-frame hot paths", 2026-09-03), which replaced it with the radix sort.
// Recovered verbatim via:
//     git show f8c4dc22898207dda413cf1df4ff07ad8f5bda42 -- source/scene/chunk_render.c
// The pre-image (last commit where it was still live) is 6c6b337d518f7453552452739e44fc0eea4b5211
// ("feat(v1.8.11): the chunk-hole fix, worm caves, and a desert that looks like one").
// Both loop bodies below are copied character-for-character from that diff; only the
// surrounding scratch-array declarations were renamed to be self-contained in this file (they
// were file-scope statics named s_vis_depth/s_vis_list/s_sort_* in chunk_render.c, sized by
// MESH_SLOTS; here they are locals/globals sized by N, with N == MESH_SLOTS == 968).
//
// ── Methodology, matching what the chunk_render.c comment claims it did ──
//
//   - x86, gcc -O2
//   - both sorts driven over the SAME input per rep
//   - ms per sort, mean of 120 reps
//   - n = 968 (MESH_SLOTS, per scene/render_dist.h: RENDER_DIST_MAX_COLUMNS(121) *
//     RENDER_DIST_SLOTS_PER_COLUMN(8) = 968, at RENDER_DIST_MAX = 5)
//   - three repeat runs, to show run-to-run spread
//   - six named input distributions: shuffled-realistic, random, reverse, denormals-reverse,
//     sorted-ascending, all-equal
//
// NOTE ON THE INPUT DISTRIBUTIONS: the original probe's generators are not recoverable — they
// lived in the deleted scratchpad and only their NAMES and the properties the comment attributes
// to them survive ("reverse (worst case)", "denormals-reverse", "sorted-ascending" is insertion
// sort's best case, "all-equal" is degenerate). The generators below reconstruct each name from
// that description as literally as possible:
//   - sorted-ascending : val[i] = i,                     strictly increasing   (insertion best case)
//   - reverse          : val[i] = N - i,                 strictly decreasing   (insertion worst case)
//   - denormals-reverse: val[i] = (N - i) * FLT_TRUE_MIN, strictly decreasing, subnormal magnitudes
//   - all-equal        : val[i] = 42.0f for all i
//   - random           : uniform over [-1e6, 1e6], independent of position
//   - shuffled-realistic: plausible chunk clip-w depths (a render disc of radius ~5 chunks,
//     CHUNK_DIM=16, so roughly [-16, 400] with some negative "behind camera" values), generated
//     in sorted order and then Fisher-Yates shuffled — i.e. the values a real frame would see,
//     in pool-slot (unsorted) order, which is what the chunk_render.c comment says the real
//     input is.
// This is a re-derivation, not a byte-identical replay of whatever the original probe used, and
// is reported as such.
//
// Build (WSL, from the repo root):
//   gcc -O2 -o /mnt/c/.../sortbench tools/sortbench.c -lm
//
// This file is host-only. It is never compiled into the console build.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <float.h>

#define N 968          // MESH_SLOTS
#define REPS 120

// ───────────────────────── scratch storage for both sorts ─────────────────────────

static float    s_vis_depth[N];
static int      s_vis_list[N];

// radixSort's scratch (verbatim shapes of s_sort_ka/kb/oa/ob/hist from chunk_render.c)
static union { uint32_t u[N]; float f[N]; int i[N]; } s_sort_ka;
static union { uint32_t u[N]; float f[N]; int i[N]; } s_sort_kb;
static uint16_t s_sort_oa[N];
static uint16_t s_sort_ob[N];
static uint32_t s_sort_hist[4][256];

// ───────────────────────── insertionSort() — lifted verbatim ─────────────────────────
// From chunk_render.c as of commit 6c6b337d518f7453552452739e44fc0eea4b5211, inside
// cullFrame()'s `#if BS_SORT` block, immediately after `const int n = s_vis_n;`. Operates
// directly on s_vis_depth/s_vis_list, exactly as the original did.
static void insertionSort(int n)
{
	for (int i = 1; i < n; i++) {
		const float d = s_vis_depth[i];
		const int   v = s_vis_list[i];
		int j = i - 1;
		while (j >= 0 && s_vis_depth[j] > d) {
			s_vis_depth[j + 1] = s_vis_depth[j];
			s_vis_list[j + 1]  = s_vis_list[j];
			j--;
		}
		s_vis_depth[j + 1] = d;
		s_vis_list[j + 1]  = v;
	}
}

// ───────────────────────── radixSort() — lifted verbatim ─────────────────────────
// From chunk_render.c, lines 3128-3187 (current working tree), inside cullFrame()'s
// `#if BS_SORT` block, same call site as insertionSort() above. Operates directly on
// s_vis_depth/s_vis_list and the s_sort_* scratch arrays, exactly as the original did.
static void radixSort(int n)
{
	memset(s_sort_hist, 0, sizeof s_sort_hist);
	for (int i = 0; i < n; i++) {
		uint32_t u;
		memcpy(&u, &s_vis_depth[i], sizeof u);
		if (u == 0x80000000u) u = 0u;
		u = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
		s_sort_ka.u[i] = u;
		s_sort_oa[i]   = (uint16_t)i;
		s_sort_hist[0][ u        & 0xFFu]++;
		s_sort_hist[1][(u >>  8) & 0xFFu]++;
		s_sort_hist[2][(u >> 16) & 0xFFu]++;
		s_sort_hist[3][(u >> 24) & 0xFFu]++;
	}

	uint32_t* ks = s_sort_ka.u;
	uint32_t* kd = s_sort_kb.u;
	uint16_t* os = s_sort_oa;
	uint16_t* od = s_sort_ob;

	for (int p = 0; p < 4; p++) {
		uint32_t* h = s_sort_hist[p];
		const int shift = p * 8;

		if (n > 0 && h[(ks[0] >> shift) & 0xFFu] == (uint32_t)n) continue;

		uint32_t sum = 0;
		for (int b = 0; b < 256; b++) { const uint32_t c = h[b]; h[b] = sum; sum += c; }

		for (int i = 0; i < n; i++) {
			const uint32_t k = ks[i];
			const uint32_t d = h[(k >> shift) & 0xFFu]++;
			kd[d] = k;
			od[d] = os[i];
		}

		uint32_t* kt = ks; ks = kd; kd = kt;
		uint16_t* ot = os; os = od; od = ot;
	}

	for (int k = 0; k < n; k++) {
		const int src = os[k];
		s_sort_ka.f[k] = s_vis_depth[src];
		s_sort_kb.i[k] = s_vis_list[src];
	}
	memcpy(s_vis_depth, s_sort_ka.f, (size_t)n * sizeof s_vis_depth[0]);
	memcpy(s_vis_list,  s_sort_kb.i, (size_t)n * sizeof s_vis_list[0]);
}

// ───────────────────────── input distributions ─────────────────────────

static uint32_t s_rng_state = 0;

static uint32_t rngNext(void)
{
	// xorshift32, fixed-seeded per distribution so runs are reproducible.
	uint32_t x = s_rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_rng_state = x;
	return x;
}

static float rngUniform(float lo, float hi)
{
	const float t = (float)(rngNext() & 0xFFFFFFu) / (float)0xFFFFFFu; // [0,1)
	return lo + t * (hi - lo);
}

typedef void (*GenFn)(float* out);

static void genSortedAscending(float* out)
{
	for (int i = 0; i < N; i++) out[i] = (float)i;
}

static void genReverse(float* out)
{
	for (int i = 0; i < N; i++) out[i] = (float)(N - i);
}

static void genDenormalsReverse(float* out)
{
	for (int i = 0; i < N; i++) out[i] = (float)(N - i) * FLT_TRUE_MIN;
}

static void genAllEqual(float* out)
{
	for (int i = 0; i < N; i++) out[i] = 42.0f;
}

static void genRandom(float* out)
{
	s_rng_state = 0xC0FFEEu;
	for (int i = 0; i < N; i++) out[i] = rngUniform(-1.0e6f, 1.0e6f);
}

static void genShuffledRealistic(float* out)
{
	// A render disc of radius 5 chunks, CHUNK_DIM=16: plausible clip-w depths range roughly
	// -16 (a chunk straddling the camera, centre behind it) to ~400 (far corner of the ring).
	s_rng_state = 0x5EED1234u;
	for (int i = 0; i < N; i++) out[i] = rngUniform(-16.0f, 400.0f);
	// Fisher-Yates shuffle so the values are in "fresh pool-slot order", not sorted -- this is
	// the property the chunk_render.c comment says the real per-frame input has.
	for (int i = N - 1; i > 0; i--) {
		const int j = (int)(rngNext() % (uint32_t)(i + 1));
		const float t = out[i]; out[i] = out[j]; out[j] = t;
	}
}

typedef struct { const char* name; GenFn gen; } Dist;

static const Dist DISTS[] = {
	{ "shuffled-realistic", genShuffledRealistic },
	{ "random",             genRandom            },
	{ "reverse",            genReverse           },
	{ "denormals-reverse",  genDenormalsReverse  },
	{ "sorted-ascending",   genSortedAscending   },
	{ "all-equal",          genAllEqual          },
};
#define NUM_DISTS (int)(sizeof(DISTS) / sizeof(DISTS[0]))

// ───────────────────────── timing ─────────────────────────

static double nowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// Runs `n` sort-under-test reps over the same template input, alternating regeneration and
// timed sort so the timed region contains only the sort itself.
static double benchOld(const float* tmpl)
{
	double total = 0.0;
	for (int r = 0; r < REPS; r++) {
		for (int i = 0; i < N; i++) { s_vis_depth[i] = tmpl[i]; s_vis_list[i] = i; }
		const double t0 = nowMs();
		insertionSort(N);
		const double t1 = nowMs();
		total += (t1 - t0);
	}
	return total / REPS;
}

static double benchNew(const float* tmpl)
{
	double total = 0.0;
	for (int r = 0; r < REPS; r++) {
		for (int i = 0; i < N; i++) { s_vis_depth[i] = tmpl[i]; s_vis_list[i] = i; }
		const double t0 = nowMs();
		radixSort(N);
		const double t1 = nowMs();
		total += (t1 - t0);
	}
	return total / REPS;
}

int main(void)
{
	static float tmpl[N];

	printf("sortbench: N=%d REPS=%d, %d distributions\n\n", N, REPS, NUM_DISTS);

	for (int d = 0; d < NUM_DISTS; d++) {
		DISTS[d].gen(tmpl);
		const double oldMs = benchOld(tmpl);
		const double newMs = benchNew(tmpl);
		const double speedup = oldMs / newMs;
		printf("%-20s old=%.4f ms  new=%.4f ms  speedup=%.2fx\n",
		       DISTS[d].name, oldMs, newMs, speedup);
	}

	return 0;
}
