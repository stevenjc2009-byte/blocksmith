// Host probe for v1.9.0's distance fog: the fade the GPU will actually produce, as numbers.
//
// This is a VISUAL change to the world and a compiling build proves nothing about it, so the
// claim has to be made somewhere it can be checked. What is checkable on a host is the
// arithmetic every fragment runs, and all of it is here:
//
//   1. build/fogramp.t3x, DECODED — the real artefact the console is handed, not the .png it
//      was made from and not a table restated in C. The t3x header, the PICA200's 8x8-tile
//      Morton swizzle, all 1,024 texels. Every one is asserted against fogRampTexel() in
//      source/gfx/fogramp.c, which is what keeps tools/make_fog_ramp.py and the C reference
//      from drifting apart — the same cross-check world/atlas_uv_shader_test.c runs on the
//      uvScale constant duplicated across two .pica files, and the only reason a second copy
//      of a curve is allowed here.
//   2. The sampler — GPU_LINEAR between texel centres, GPU_CLAMP_TO_EDGE outside them — and
//      the affine depth-to-texcoord map the vertex shader computes, both out of
//      source/gfx/fogramp.c so this binary runs the shipped arithmetic rather than a copy.
//   3. half_vis at radius 1..6, which is the number the whole change exists to move.
//
// WHAT IT DOES NOT CHECK, stated rather than left implied. That the PICA200 interpolates
// texcoord1 perspective-correctly, that GPU_INTERPOLATE is src0*src2 + src1*(1-src2), and that
// row 3 of the projection reaches the shader intact are all facts about hardware this machine
// does not have. Nothing here has run on a console. The strongest thing behind the first is
// that the atlas already depends on it (gfx/atlas.c: GPU_REPEAT in U with u running past 16 on
// a greedy-merged quad, and the world renders correctly); the third is measured out of
// libcitro3d.a with objdump, quoted in both .pica files.
//
// Own main(), same pattern as tests/mesh_pool_bytes_test.c and the other host-only binaries.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx/fogramp.h"

// Overridable so the parent build can point this at a t3x somewhere other than the tree it is
// run from. Default is where a devkitPro build puts it.
#ifndef FOGRAMP_T3X_PATH
#define FOGRAMP_T3X_PATH  "build/fogramp.t3x"
#endif

// The load boundary a radius produces, CHUNK_DIM * radius. Spelled here rather than pulled out
// of world/chunk.h so this binary links nothing but fogramp.c — and 16 is checked against the
// real constant by every other stanza in the suite.
#define BOUNDARY_FOR(r)  (16.0f * (float)(r))

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
		}                                                                           \
	} while (0)

// CHECK with a message, for the source-text checks below. Those compare a needle against a
// file, so the condition text (`got == want`) says nothing useful on its own — what the reader
// needs is which file, which needle, and what breaks. Same counters, so one total.
// The line is a PARAMETER rather than __LINE__, because these checks all run inside two shared
// helpers: with __LINE__ every source-text failure in the file reported the same line number —
// the helper's — which points whoever hits it at the checking code instead of at the check they
// broke. The helpers take the caller's __LINE__ and pass it down.
#define CHECK_MSG_AT(line, cond, ...) do {                                         \
		s_checks++;                                                                 \
		if (!(cond)) {                                                              \
			s_fails++;                                                              \
			printf("  FAIL L%d: ", (line));                                         \
			printf(__VA_ARGS__);                                                    \
			printf("\n");                                                           \
			if (!s_first[0]) {                                                      \
				char msg_[160];                                                     \
				snprintf(msg_, sizeof(msg_), __VA_ARGS__);                          \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", (line), msg_);     \
			}                                                                       \
		}                                                                           \
	} while (0)

#define CHECK_MSG(cond, ...) CHECK_MSG_AT(__LINE__, cond, __VA_ARGS__)

// The PICA200's in-tile Morton interleave: x and y bits alternate, x first. Same function as
// picaMorton() in world/atlas_uv_shader_test.c, and MEASURED here as well as inherited — the
// first eight bytes of the decoded ramp are 0,0,0,0,0,1,0,1 for a curve whose first four texel
// values are 0,0,0,1, which is only consistent with this interleave and this row order.
static unsigned picaMorton(unsigned x, unsigned y)
{
	return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2)
	     | ((x & 4u) << 2) | ((y & 4u) << 3);
}

// build/fogramp.t3x, decoded down to texels.
//
// Three fixed layers, none of them guessed:
//   1. The t3x header tex3ds writes: u16 subtexture count, one packed byte holding
//      log2(width)-3 in bits 0..2 and log2(height)-3 in bits 3..5, the GPU format byte, the
//      mipmap level count; then one 12-byte subtexture record.
//   2. A four-byte compression header: low byte the method, upper three the decompressed size.
//      gfx/fogramp.t3s asks for "-z none", so the method MUST be 0 and the bytes that follow
//      are raw. Anything else is a loud failure rather than a wrong decode — an LZ11 body read
//      as raw would produce a plausible-looking ramp made of compression opcodes.
//   3. The swizzle above.
static bool readRamp(uint8_t out[FOGRAMP_W], char* err, size_t errsz)
{
	unsigned char raw[8192];

	FILE* f = fopen(FOGRAMP_T3X_PATH, "rb");
	if (!f) {
		snprintf(err, errsz,
		         "cannot open %s — a build product (tex3ds, from gfx/fogramp.t3s), not in git. "
		         "Run \"make\" from devkitPro MSYS2 and re-run. Skipping it would leave the "
		         "whole fog fade unchecked, which is the only thing this binary is for",
		         FOGRAMP_T3X_PATH);
		return false;
	}
	const size_t len = fread(raw, 1, sizeof(raw), f);
	fclose(f);

	if (len < 21) {
		snprintf(err, errsz, "%s is only %zu bytes, too short for a t3x header", FOGRAMP_T3X_PATH, len);
		return false;
	}

	const unsigned subtex  = (unsigned)raw[0] | ((unsigned)raw[1] << 8);
	const unsigned packed  = raw[2];
	const unsigned w       = 8u << (packed & 7u);
	const unsigned h       = 8u << ((packed >> 3) & 7u);
	const unsigned format  = raw[3];
	const unsigned mipmaps = raw[4];

	if (w != FOGRAMP_W || h != FOGRAMP_H) {
		snprintf(err, errsz,
		         "%s says the ramp is %ux%u, but FOGRAMP_W/FOGRAMP_H say %dx%d. A t3x built "
		         "before the ramp was resized is STALE, not merely different",
		         FOGRAMP_T3X_PATH, w, h, FOGRAMP_W, FOGRAMP_H);
		return false;
	}
	if (format != 8u) {
		snprintf(err, errsz,
		         "%s is GPU format %u, not 8 (GPU_A8). TEV stage 1 sources this texture's ALPHA "
		         "(GPU_TEVOP_RGB_SRC_ALPHA); any other format puts the ramp in a channel nothing "
		         "reads and the world would draw with no fog at all",
		         FOGRAMP_T3X_PATH, format);
		return false;
	}
	if (mipmaps != 0u) {
		snprintf(err, errsz,
		         "%s carries %u mipmap levels. gfx/fogtex.c asks for GPU_LINEAR, not a "
		         "GPU_*_MIPMAP_* mode, so a chain here is dead weight and a sign the .t3s grew "
		         "a -m nobody wanted", FOGRAMP_T3X_PATH, mipmaps);
		return false;
	}

	const size_t data = 5u + 12u * (size_t)subtex;
	if (len < data + 4u) {
		snprintf(err, errsz, "%s ends inside its compression header", FOGRAMP_T3X_PATH);
		return false;
	}
	const unsigned method = raw[data];
	const size_t   dsize  = (size_t)raw[data + 1] | ((size_t)raw[data + 2] << 8)
	                      | ((size_t)raw[data + 3] << 16);
	if (method != 0u) {
		snprintf(err, errsz,
		         "%s is compressed (method 0x%02x). gfx/fogramp.t3s asks for \"-z none\" and this "
		         "reader has no inflater — reading a compressed body as raw would decode into a "
		         "plausible-looking ramp made of opcodes", FOGRAMP_T3X_PATH, method);
		return false;
	}
	if (dsize != (size_t)FOGRAMP_W * FOGRAMP_H) {
		snprintf(err, errsz, "%s holds %zu bytes of texels, expected %d for a %dx%d A8 sheet",
		         FOGRAMP_T3X_PATH, dsize, FOGRAMP_W * FOGRAMP_H, FOGRAMP_W, FOGRAMP_H);
		return false;
	}
	if (len < data + 4u + dsize) {
		snprintf(err, errsz, "%s is truncated: %zu bytes for %zu of texels", FOGRAMP_T3X_PATH, len, dsize);
		return false;
	}

	const unsigned char* px          = raw + data + 4u;
	const unsigned       tiles_across = (unsigned)FOGRAMP_W / 8u;

	// Every row must be identical, because the vertex shader writes the fog coordinate into
	// BOTH components of texcoord1 and therefore does not control v. If the rows ever differ,
	// the fade a fragment gets depends on its distance twice over and the shader is wrong, not
	// the texture. Checked over the whole sheet, then row 0 is taken as the ramp.
	bool rows_equal = true;
	for (unsigned x = 0; x < (unsigned)FOGRAMP_W; x++) {
		const unsigned tile0 = (0u / 8u) * tiles_across + (x / 8u);
		const uint8_t  v0    = px[tile0 * 64u + picaMorton(x & 7u, 0u)];
		out[x] = v0;
		for (unsigned y = 1; y < (unsigned)FOGRAMP_H; y++) {
			const unsigned tile = (y / 8u) * tiles_across + (x / 8u);
			if (px[tile * 64u + picaMorton(x & 7u, y & 7u)] != v0)
				rows_equal = false;
		}
	}
	if (!rows_equal) {
		snprintf(err, errsz,
		         "%s has rows that differ. All %d rows must be identical: the shader writes the "
		         "fog coordinate into both components of texcoord1, so v is not a coordinate it "
		         "controls", FOGRAMP_T3X_PATH, FOGRAMP_H);
		return false;
	}
	return true;
}

// ── Source-text checks ───────────────────────────────────────────────────────────────────
//
// For the half of this change that cannot be linked here: scene/playermodel.c and
// scene/highlight.c both need <3ds.h>, and source/shaders/highlight.v.pica is not C at all.
// Reading them as text is the same technique world/atlas_uv_shader_test.c uses, and it carries
// the same limitation, stated rather than glossed: this proves a CALL IS WRITTEN, not that it
// runs or that the GPU does what the call asks. That gap is what the console settles.
//
// `strip_pica_comments` exists because picasso's comment character is ';', so a line of prose
// naming an instruction is indistinguishable from the instruction under a plain strstr(). That
// is not a hypothetical — it is the exact defect that made this project's atlas guard report
// four failures against two correct shaders, once the fog work added comments naming the
// instructions they were about. C sources are matched raw: ';' ends a statement there.
static int countInFile(const char* path, const char* needle, bool strip_pica_comments,
                       bool* opened)
{
	*opened = false;
	FILE* f = fopen(path, "r");
	if (!f) return 0;
	*opened = true;

	int  n = 0;
	char line[4096];
	while (fgets(line, sizeof(line), f)) {
		if (strip_pica_comments) {
			char* c = strchr(line, ';');
			if (c) *c = '\0';
		}
		// One count per LINE, not per occurrence: every needle below appears at most once on
		// a line, and counting lines keeps the numbers readable against the source.
		if (strstr(line, needle)) n++;
	}
	fclose(f);
	return n;
}

// Asserts `needle` appears on exactly `want` lines of `path`. `why` says what breaks when it
// does not, in terms of what the player sees, because a bare needle in a failure message tells
// whoever hits it nothing about whether it matters.
static void checkSourceAt(int line, const char* path, const char* needle, int want,
                          const char* why)
{
	bool      opened = false;
	const int got    = countInFile(path, needle, false, &opened);

	CHECK_MSG_AT(line, opened, "cannot open %s", path);
	if (!opened) return;

	CHECK_MSG_AT(line, got == want, "%s has `%s` on %d line(s), expected %d — %s",
	             path, needle, got, want, why);
}
#define checkSource(...) checkSourceAt(__LINE__, __VA_ARGS__)

// The same fog wiring world/atlas_uv_shader_test.c asserts for the two terrain shaders, for
// the third .pica file that now carries it. Kept here rather than there because that suite is
// about the atlas; this one is about the fade.
//
// The instructions are asserted individually AND in order. Order is the part that is easy to
// lose and impossible to see: a PICA200 register holds the previous vertex's value, so a
// coordinate computed out of sequence draws a world one vertex out of step and errors nowhere.
static void checkPicaFogWiringAt(int at_line, const char* path)
{
	static const struct { const char* needle; const char* why; } kParts[] = {
		{ ".fvec projection[4], modelView[4], fogParams[1]",
		  "does not declare fogParams, so the uniform lookup returns -1 and the fade is "
		  "whatever register 0 holds" },
		{ ".out outtc1 texcoord1",
		  "does not declare the varying, so there is nothing for TEV stage 1 to sample" },
		{ "mov outpos.w, r7.wwww",
		  "never writes clip w, so nothing rasterises at all" },
	};
	for (size_t i = 0; i < sizeof(kParts) / sizeof(kParts[0]); i++) {
		bool      opened = false;
		const int got    = countInFile(path, kParts[i].needle, true, &opened);
		CHECK_MSG_AT(at_line, opened, "cannot open %s", path);
		if (!opened) return;
		CHECK_MSG_AT(at_line, got == 1, "%s: expected exactly one `%s`, found %d — %s",
		          path, kParts[i].needle, got, kParts[i].why);
	}

	// The old direct write must be GONE, not merely joined by the new one: two writes to
	// clip w is a shader that assembles cleanly and draws wrongly.
	bool      opened = false;
	const int old_w  = countInFile(path, "dp4 outpos.w, projection[3], r1", true, &opened);
	CHECK_MSG_AT(at_line, old_w == 0,
	          "%s still writes clip w directly (`dp4 outpos.w, projection[3], r1`) as well as "
	          "capturing the depth — clip w is written twice", path);

	// Order: capture the depth, scale it, bias it, output it.
	static const char* const kChain[] = {
		"dp4 r7.w, projection[3], r1",
		"mul r7.x, fogParams.xxxx, r7.wwww",
		"add r7.x, fogParams.yyyy, r7.xxxx",
		"mov outtc1, r7.xxxx",
	};
	int line_of[4] = { -1, -1, -1, -1 };
	FILE* f = fopen(path, "r");
	CHECK_MSG_AT(at_line, f != NULL, "cannot open %s to check the fog instruction order", path);
	if (!f) return;
	char line[4096];
	for (int ln = 1; fgets(line, sizeof(line), f); ln++) {
		char* c = strchr(line, ';');
		if (c) *c = '\0';
		for (int k = 0; k < 4; k++)
			if (line_of[k] < 0 && strstr(line, kChain[k])) line_of[k] = ln;
	}
	fclose(f);

	for (int k = 0; k < 4; k++)
		CHECK_MSG_AT(at_line, line_of[k] > 0, "%s is missing `%s`", path, kChain[k]);
	for (int k = 1; k < 4; k++)
		CHECK_MSG_AT(at_line, line_of[k - 1] > 0 && line_of[k] > 0 && line_of[k - 1] < line_of[k],
		          "%s computes the fog coordinate out of order: `%s` is at L%d but `%s` is at "
		          "L%d, and it must come first", path, kChain[k - 1], line_of[k - 1],
		          kChain[k], line_of[k]);
}
#define checkPicaFogWiring(p) checkPicaFogWiringAt(__LINE__, (p))

int main(void)
{
	uint8_t ramp[FOGRAMP_W];
	char    err[512] = {0};

	if (!readRamp(ramp, err, sizeof(err))) {
		printf("fog ramp: FAIL — %s\n", err);
		return 1;
	}

	// ── the artefact against the reference ────────────────────────────────────────────────
	//
	// The whole Python-versus-C guard. tools/make_fog_ramp.py wrote gfx/fogramp.png, tex3ds
	// turned it into the .t3x decoded above, and every texel of it has to be exactly what
	// fogRampTexel() says. A single texel out means the two copies of the curve have drifted
	// and every number below describes a fade the console does not have.
	int mismatch = -1;
	for (int i = 0; i < FOGRAMP_W; i++)
		if (ramp[i] != fogRampTexel(i) && mismatch < 0)
			mismatch = i;
	CHECK(mismatch < 0);
	if (mismatch >= 0)
		printf("    first mismatch at texel %d: t3x=%u, fogRampTexel=%u\n",
		       mismatch, ramp[mismatch], fogRampTexel(mismatch));

	// Shape facts that hold for any sane ramp, so a curve edit that breaks the ENDS fails here
	// rather than in a half_vis number nobody recognises as wrong.
	CHECK(ramp[0] == 0);                    // fully clear inside the fade's start
	CHECK(ramp[FOGRAMP_W - 1] == 255);      // fully opaque at and past its end
	bool monotone = true;
	for (int i = 1; i < FOGRAMP_W; i++)
		if (ramp[i] < ramp[i - 1]) monotone = false;
	CHECK(monotone);

	// ── the fade, per radius ──────────────────────────────────────────────────────────────
	printf("fog ramp: %s decoded, %d texels, FOG_START_FRAC=%.2f FOG_END_FRAC=%.2f\n",
	       FOGRAMP_T3X_PATH, FOGRAMP_W, (double)FOG_START_FRAC, (double)FOG_END_FRAC);

	printf("\nvisibility (1 = clear, 0 = sky) by distance in blocks, at radius 3:\n");
	{
		const FogShape s = fogShapeFor(BOUNDARY_FOR(3), 1.0f);
		printf("  start=%.4f end=%.4f inv_range=%.6f bias=%.6f\n",
		       (double)s.start, (double)s.end, (double)s.inv_range, (double)s.bias);
		static const float kDist[] = {
			0.5f, 1.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f,
			24.0f, 28.0f, 30.0f, 32.0f, 36.0f, 40.0f, 44.0f, 45.6f, 46.0f, 48.0f, 64.0f, 200.0f
		};
		for (size_t i = 0; i < sizeof(kDist) / sizeof(kDist[0]); i++)
			printf("    %7.2f  vis %.6f   fog %.6f\n", (double)kDist[i],
			       (double)fogVisibility(&s, ramp, kDist[i]),
			       (double)(1.0f - fogVisibility(&s, ramp, kDist[i])));
	}

	printf("\nhalf_vis — the distance at which the world is half faded, i.e. what the player sees:\n");
	float prev_half = 0.0f;
	for (int r = 1; r <= 6; r++) {
		const FogShape s    = fogShapeFor(BOUNDARY_FOR(r), 1.0f);
		const float    half = fogHalfVis(&s, ramp);
		const float    bvis = fogVisibility(&s, ramp, BOUNDARY_FOR(r));

		printf("  radius %d: boundary %6.2f  start %6.3f  end %6.3f  half_vis %8.4f  "
		       "vis@boundary %.6f\n",
		       r, (double)BOUNDARY_FOR(r), (double)s.start, (double)s.end,
		       (double)half, (double)bvis);

		// The whole point of the change: half_vis has to keep RISING with the radius. Under the
		// hardware LUT it went 14.3358 / 14.3651 / 14.4065 at radius 3 / 4 / 5 — a ceiling, not
		// a curve, and raising the render distance bought geometry the fog then hid.
		//
		// The threshold is an ABSOLUTE step, not a ratio, and that is a correction this check
		// earned the hard way: it was first written as `half > prev_half * 1.4f`, which the
		// probe rejected at radius 4, 5 and 6. The fade is linear in the radius, so the RATIO
		// between consecutive radii is r/(r-1) — 2.0, 1.5, 1.333, 1.25, 1.2 — and it falls
		// below 1.4 from radius 4 onward no matter how healthy the fog is. A ratio test on a
		// linear series is a test of the series' curvature, which is not the claim. The claim
		// is that each extra chunk of render distance buys visible distance, so the step is
		// what has to be measured: half a chunk, against a real step of CHUNK_DIM * 0.59375 =
		// 9.5 blocks and an old one of about 0.03.
		CHECK(half - prev_half >= 8.0f);
		prev_half = half;

		// And the constraint that shapes it: the fade must be complete before the player can
		// see the edge of the loaded world. Same target as scene/render_dist.h's
		// RENDER_DIST_TARGET_VIS, spelled as the literal so this binary links only fogramp.c.
		CHECK(bvis <= 0.005f);

		// Nothing inside `start` is touched at all — the fog coordinate is <= 0 there and the
		// sampler clamps to texel 0, which is 0.
		CHECK(fogVisibility(&s, ramp, s.start) >= 0.999f);
		CHECK(fogVisibility(&s, ramp, s.start * 0.5f) >= 0.999f);
	}

	// ── the red arm the console build already had ─────────────────────────────────────────
	//
	// -DFOG_DENSITY=0.0f is the switch scene/chunk_render.c has always carried to prove the fog
	// checks can fail. It used to leave every LUT entry clear; it now collapses inv_range and
	// bias to exactly 0, so every vertex emits fog coordinate 0 and every fragment samples
	// texel 0. Checked at the far plane, because that is where the difference is total.
	{
		const FogShape off = fogShapeFor(BOUNDARY_FOR(3), 0.0f);
		CHECK(off.inv_range == 0.0f);
		CHECK(off.bias == 0.0f);
		CHECK(fogVisibility(&off, ramp, 200.0f) >= 0.999f);
		CHECK(fogVisibility(&off, ramp, 0.0f) >= 0.999f);

		// And half strength really is a half-strength fade rather than a no-op or a clamp: the
		// range doubles, so the boundary sits at the fade's midpoint.
		const FogShape half = fogShapeFor(BOUNDARY_FOR(3), 0.5f);
		CHECK(half.inv_range > 0.0f);
		CHECK(fogVisibility(&half, ramp, BOUNDARY_FOR(3)) > 0.4f);
		CHECK(fogVisibility(&half, ramp, BOUNDARY_FOR(3)) < 0.6f);
	}

	// ── the sampler itself ────────────────────────────────────────────────────────────────
	//
	// GPU_CLAMP_TO_EDGE at both ends, and a texel centre landing exactly on its own value. A
	// ramp read with an off-by-half-a-texel coordinate would still be monotone, still reach
	// both ends and still produce a believable half_vis — this is the check that would catch
	// it.
	CHECK(fogRampSample(ramp, -5.0f) == (float)ramp[0] / 255.0f);
	CHECK(fogRampSample(ramp, 0.0f) == (float)ramp[0] / 255.0f);
	CHECK(fogRampSample(ramp, 5.0f) == (float)ramp[FOGRAMP_W - 1] / 255.0f);
	CHECK(fogRampSample(ramp, 1.0f) == (float)ramp[FOGRAMP_W - 1] / 255.0f);
	CHECK(fogRampSample(ramp, 64.5f / (float)FOGRAMP_W) == (float)ramp[64] / 255.0f);

	// ── the THIRD call site: remote players ───────────────────────────────────────────────
	//
	// The arithmetic above proves the fade's shape. It cannot prove that a player is shaded by
	// it, because scene/playermodel.c needs <3ds.h> and cannot be linked here — so that half is
	// checked as source text, the same technique world/atlas_uv_shader_test.c uses on the .pica
	// files and net/session_test.c uses on main.c.
	//
	// Why this is worth checking at all. Players were fogged before v1.9.0 only by accident:
	// the hardware unit the world pass switched on was never switched off, so it applied to
	// them too. v1.9.0 turns that unit off everywhere, which would have silently DROPPED the
	// fade from player bodies — a regression that this version's much longer view distances
	// (half_vis 28.5 blocks at radius 3 against the old 14.34) would have made obvious, as a
	// figure at full contrast against terrain dissolving into sky behind them.
	//
	// The requirement is not "players are fogged somehow" but "players are fogged by the SAME
	// curve as the terrain". Two fades that merely resemble each other would drift the first
	// time one is retuned, so what is asserted is that the same fogShapeFor() is called on the
	// same boundary, that the same ramp texture is bound, and that the shader those draws run
	// computes the coordinate the same way world_dynamic.v.pica does.
	checkPicaFogWiring("source/shaders/highlight.v.pica");

	// playermodel.c: the fade is ON, and built from the live render distance rather than a
	// cached copy — chunkRenderSetDistance() can change the radius mid-session and a stale
	// shape would fade players on the previous distance's curve.
	checkSource("source/scene/playermodel.c", "fogTexBind()", 1,
	            "does not bind the ramp texture, so TEV stage 1 samples whatever unit 1 held");
	checkSource("source/scene/playermodel.c", "GPU_INTERPOLATE", 1,
	            "does not blend toward the sky colour");
	checkSource("source/scene/playermodel.c", "fogShapeFor(chunkRenderDistance()->boundary", 1,
	            "does not build the fade from the LIVE render distance — the whole point is "
	            "that this is the terrain's own curve, on the terrain's own boundary");
	// Three lines: the static, the shaderInstanceGetUniformLocation lookup at init, and the
	// C3D_FVUnifSet upload at draw. Any two of the three without the third is a fade that is
	// silently never applied.
	checkSource("source/scene/playermodel.c", "s_uloc_fogparams", 3,
	            "does not declare, look up AND upload fogParams — all three are needed before "
	            "a single player fades");

	// highlight.c: the fade is explicitly OFF, and off by an upload rather than by omission.
	// The cage shares highlight.v.pica with the player models, and uniform registers are global
	// GPU state, so an unwritten fogParams would inherit playermodel.c's values and the cage
	// would fog or not depending on whether anyone else happened to be on screen.
	checkSource("source/scene/highlight.c",
	            "C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_fogparams, 0.0f, 0.0f, 0.0f, 0.0f)", 1,
	            "does not zero fogParams, so the cage inherits whatever scene/playermodel.c "
	            "last uploaded and fogs intermittently");
	checkSource("source/scene/highlight.c", "GPU_INTERPOLATE", 0,
	            "blends the selection cage toward the sky; the cage must stay crisp");

	// All four passes that are not the terrain must leave the hardware unit off. This is the
	// pre-existing leak (GPU_NO_FOG appeared nowhere in the tree before v1.9.0) and it is
	// checked as a set rather than one file at a time, because it was the SET being incomplete
	// that made it a bug.
	static const char* const kOwnFog[] = {
		"source/scene/highlight.c", "source/scene/crackoverlay.c",
		"source/scene/playermodel.c", "source/gfx/sprite.c",
	};
	for (size_t i = 0; i < sizeof(kOwnFog) / sizeof(kOwnFog[0]); i++)
		checkSource(kOwnFog[i], "C3D_FogGasMode(GPU_NO_FOG", 1,
		            "does not turn the fixed-function fog unit off, so it inherits the "
		            "terrain pass's fog as persistent GPU state");

	printf("\n");
	if (s_fails) {
		printf("fog ramp: FAILED - %d of %d checks (first: %s)\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("fog ramp: PASS %d checks\n", s_checks);
	return 0;
}
