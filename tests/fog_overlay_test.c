// Host self-test for v1.9.1's fog-overlay fix: source/scene/chunk_render.c's
// chunkRenderFogHalfVis() and its call site in source/main.c's debug overlay.
//
// The bug this replaces: the overlay's "see" field used to print RenderDist.half_vis, which
// comes from renderDistFor()'s solveDistance() over the retired PICA200 fixed-function fog LUT
// (renderDistVisibility/renderDistFogTable in scene/render_dist.c). That LUT has had no GPU path
// since v1.9.0 turned the hardware fog unit off (pipelineBind now calls
// C3D_FogGasMode(GPU_NO_FOG, ...); grep for FogLut_FromArray/C3D_FogLutBind across source/,
// tests/ and tools/ turns up nothing), so half_vis barely moved with the render-distance
// setting while what the player actually saw scaled hugely with it. renderDistFor() itself is
// untouched here — world/world_test.c still asserts on it — this file only checks that the
// OVERLAY no longer reads it.
//
// chunkRenderFogHalfVis()'s own source text is lifted out of chunk_render.c by
// tools/run_host_tests.sh (see its own stanza) the same way tests/horizon_test.c and
// tests/profile_reset_test.c already pull functions out of the same file: chunk_render.c
// includes <3ds.h>/<citro3d.h> and cannot be compiled on the host, so the alternative to
// extraction is a hand-copy, which is the "a test that links nothing tests nothing" failure
// this project has already paid for twice. Sabotage the real function and this binary goes red;
// there is no second copy of it to drift.
#ifndef __3DS__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx/fogramp.h"     // FogShape, fogShapeFor, fogRampTexel, fogHalfVis, FOGRAMP_W — real
#include "scene/render_dist.h"   // renderDistFor — the OLD, now-dead overlay value, for contrast
#include "world/chunk.h"     // CHUNK_DIM — the real one, not a number retyped here

static int  s_checks;
static int  s_fails;
static char s_first[200];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d  %s\n", __LINE__, #cond);                          \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── The real function, extracted ──────────────────────────────────────────────────────────
//
// chunkRenderFogHalfVis() reads chunk_render.c's file-static `s_fog` by name, so this file
// declares one under the same name for the extracted body to resolve against — the same
// arrangement tests/horizon_test.c uses for s_dist-shaped state.
static FogShape s_fog;

#include "fog_overlay_extract.inc"

#ifndef BS_FOG_OVERLAY_EXTRACT_OK
#error "fog_overlay_extract.inc did not carry chunkRenderFogHalfVis() out of chunk_render.c"
#endif

// ── Claim 1: the live value moves with render distance, the dead one barely does ─────────
//
// boundary_blocks = CHUNK_DIM * radius is exactly how scene/render_dist.c's renderDistFor()
// and scene/chunk_render.c's chunkRenderSetDistance() both derive it (rd.boundary and the
// s_fog = fogShapeFor(s_dist.boundary, FOG_DENSITY) call) — see the comment in chunk_render.h
// above chunkRenderFogHalfVis's declaration. FOG_DENSITY is s_dist.fog_density, which
// renderDistFor() sets to 1.0f for an unmodified build, so strength 1.0f here is the
// shipping value, not a stand-in for it.
static float liveHalfVis(int radius)
{
	const float boundary = (float)(CHUNK_DIM * radius);
	s_fog = fogShapeFor(boundary, 1.0f);
	return chunkRenderFogHalfVis();
}

static void checkLiveValueScales(void)
{
	const float r3 = liveHalfVis(3);
	const float r4 = liveHalfVis(4);
	const float r5 = liveHalfVis(5);

	// The coordinator's measured claim: ~28.5 / 38.0 / 47.5 across radii 3/4/5. Derivable
	// exactly from fogramp.h's own documented shape (FOG_START_FRAC 0.25, FOG_END_FRAC 0.95,
	// half_vis = (start+end)/2 for a smoothstep): 0.5*(0.2375+0.95)*boundary = 0.59375 *
	// boundary, which is 28.5/38.0/47.5 at boundary 48/64/80 with no rounding at all — so the
	// tolerance below is generous headroom against a float-arithmetic ULP, not against doubt.
	printf("fog overlay live half_vis: r3=%.4f r4=%.4f r5=%.4f\n", (double)r3, (double)r4,
	       (double)r5);
	CHECK(r3 > 28.0f && r3 < 29.0f);
	CHECK(r4 > 37.5f && r4 < 38.5f);
	CHECK(r5 > 47.0f && r5 < 48.0f);

	// THE CLAIM: raising the render distance visibly raises what the overlay reports, by
	// something close to what a linear scale-up of the boundary predicts (boundary itself
	// goes 48 -> 64 -> 80, i.e. +33.3% then +25.0%). A field that is still dominated by a
	// saturated LUT would fail this outright, the same way half_vis below does.
	CHECK(r4 > r3 * 1.20f);
	CHECK(r5 > r4 * 1.15f);

	// The OLD, dead value the overlay used to print — renderDistFor() itself is untouched
	// (world/world_test.c still asserts on it) and is not being changed or removed here, only
	// contrasted against, to prove this is a real fix and not two names for the same number.
	const float old3 = renderDistFor(3).half_vis;
	const float old4 = renderDistFor(4).half_vis;
	const float old5 = renderDistFor(5).half_vis;
	printf("fog overlay dead half_vis (RenderDist, for contrast): r3=%.4f r4=%.4f r5=%.4f\n",
	       (double)old3, (double)old4, (double)old5);

	// The saturation this whole fix exists to stop reporting: less than 1% movement across
	// the full radius range, against the live value's >45% movement over the same range.
	CHECK(old5 < old3 * 1.01f);
	CHECK(r5 > r3 * 1.45f);

	// And they are not the same number wearing two names — the whole point of the bug.
	CHECK(r3 > old3 * 1.5f);
}

// ── Claim 2: main.c's overlay actually calls the new accessor, not the old field ─────────
//
// Same reasoning and the same idiom as tests/horizon_test.c's testTheRendererStillCallsIt():
// a check that only drives the extracted function directly would stay green even if main.c's
// printf silently went back to reading rd->half_vis, so the call site is asserted over its
// own SOURCE TEXT instead. Comment-aware and whitespace-squashed for the same two reasons
// horizon_test.c documents (PROSE and WRAPS) — main.c's own explanatory comment above the
// printf literally contains the string "rd->half_vis" in prose, which a naive strstr would
// mistake for the bug still being there.

#define MAIN_PATH           "source/main.c"
#define OVERLAY_DEF_NEEDLE  "const RenderDist* rd = chunkRenderDistance();"
#define OVERLAY_NEW_NEEDLE  "chunkRenderFogHalfVis()"
#define OVERLAY_OLD_NEEDLE  "rd->half_vis"

#define SRC_MAX_ENTRIES 6000
#define SRC_MAX_ENTRY   1024
#define SRC_MAX_JOIN    8

static char s_src[SRC_MAX_ENTRIES][SRC_MAX_ENTRY];
static int  s_src_n;

static void stripComment(char* s)
{
	bool in_str = false, in_chr = false;
	for (char* p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') in_str = false;
			continue;
		}
		if (in_chr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '\'') in_chr = false;
			continue;
		}
		if (*p == '"') { in_str = true; continue; }
		if (*p == '\'') { in_chr = true; continue; }
		if (*p == '/' && p[1] == '/') { *p = '\0'; return; }
	}
}

static void squash(const char* in, char* out, size_t cap)
{
	size_t o = 0;
	bool   sp = false;
	for (const char* p = in; *p && o + 1 < cap; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { sp = (o > 0); continue; }
		if (sp) { out[o++] = ' '; sp = false; }
		if (o + 1 < cap) out[o++] = *p;
	}
	out[o] = '\0';
}

static int parenDepth(const char* s, int depth)
{
	bool in_str = false, in_chr = false;
	for (const char* p = s; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') in_str = false;
			continue;
		}
		if (in_chr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '\'') in_chr = false;
			continue;
		}
		if (*p == '"') { in_str = true; continue; }
		if (*p == '\'') { in_chr = true; continue; }
		if (*p == '(') depth++;
		else if (*p == ')' && depth > 0) depth--;
	}
	return depth;
}

static int loadSource(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	s_src_n = 0;
	char raw[4096], one[SRC_MAX_ENTRY];
	int  joined = 0;
	bool joining = false;

	while (s_src_n < SRC_MAX_ENTRIES && fgets(raw, sizeof raw, f)) {
		stripComment(raw);
		squash(raw, one, sizeof one);
		if (one[0] == '\0') continue;

		if (joining && joined < SRC_MAX_JOIN) {
			char*  dst  = s_src[s_src_n - 1];
			size_t used = strlen(dst);
			if (used + 1 < SRC_MAX_ENTRY) dst[used++] = ' ';
			for (size_t k = 0; one[k] && used + 1 < SRC_MAX_ENTRY; k++) dst[used++] = one[k];
			dst[used] = '\0';
			joined++;
		} else {
			snprintf(s_src[s_src_n], SRC_MAX_ENTRY, "%s", one);
			s_src_n++;
			joined = 0;
		}

		joining = (s_src[s_src_n - 1][0] != '#') &&
		          parenDepth(s_src[s_src_n - 1], 0) > 0 && joined < SRC_MAX_JOIN;
	}

	fclose(f);
	return s_src_n;
}

static int countCode(const char* needle, int* idx)
{
	int n = 0;
	if (idx) *idx = -1;
	for (int i = 0; i < s_src_n; i++) {
		if (!strstr(s_src[i], needle)) continue;
		if (n == 0 && idx) *idx = i;
		n++;
	}
	return n;
}

static void testOverlayReadsLiveValue(void)
{
	const int n = loadSource(MAIN_PATH);

	// No early return: a file that will not open must fail every check below, not skip them,
	// for the same reason horizon_test.c's caller check has none.
	CHECK(n > 0);

	int def_idx = -1, new_idx = -1;
	const int defs = countCode(OVERLAY_DEF_NEEDLE, &def_idx);
	const int news = countCode(OVERLAY_NEW_NEEDLE, &new_idx);
	const int olds = countCode(OVERLAY_OLD_NEEDLE, NULL);

	// The overlay's `rd` local, once — the anchor the new call has to appear near.
	CHECK(defs == 1);

	// THE CLAIM: the overlay calls the new accessor exactly once, and — with comments
	// stripped, so main.c's own prose about the old bug does not fool this — never reads
	// rd->half_vis anywhere in CODE any more.
	CHECK(news == 1);
	CHECK(olds == 0);

	// And it is the same statement: the accessor call appears at or after the `rd` local's
	// own line, close enough to be the same printf (within 3 code lines covers the two-line
	// printf itself plus a reasonable reformat).
	CHECK(def_idx >= 0 && new_idx >= 0 && new_idx >= def_idx && new_idx <= def_idx + 3);

	printf("fog overlay call site: %s | %d code lines | rd local L(idx)%d (x%d) | "
	       "chunkRenderFogHalfVis() call idx %d (x%d) | rd->half_vis reads (x%d, must be 0)\n",
	       MAIN_PATH, n, def_idx, defs, new_idx, news, olds);
}

int main(void)
{
	checkLiveValueScales();
	testOverlayReadsLiveValue();

	// Pinned bare, same reason as every other suite in this project: a check count that can
	// silently shrink is how a sabotage passes. 8 (checkLiveValueScales) + 5
	// (testOverlayReadsLiveValue) + 1 (this line, which CHECK counts before it compares) = 14.
	CHECK(s_checks == 14);

	if (s_fails == 0)
		printf("fog overlay self-test: PASS  %d checks\n", s_checks);
	else
		printf("fog overlay self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int fog_overlay_test_host_only_t;

#endif   // !__3DS__
