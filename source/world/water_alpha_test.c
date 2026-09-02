// Host tests for SEE-THROUGH water (v1.8.2) — the alpha, the two shaders, and the blend state.
//
// The thing this suite exists to catch is the one this project keeps being bitten by: a diff
// that compiles, links, passes every geometry test, and renders garbage. Water's translucency
// is not geometry. It is one float in a uniform row, one instruction in two shader files that
// no C compiler ever reads, and a piece of global GPU state that until v1.8.2 nothing in the
// world draw owned. None of those three can fail loudly. Each of them fails by drawing a
// picture that is merely wrong.
//
// So, what is checked here and what it would catch:
//
//   THE TABLE      meshNrmAlpha (world/mesher.h) is the rule scene/chunk_render.c fills the
//                  faceShade table's .w with. The REAL function is linked, not a copy of it —
//                  the whole reason WATER_ALPHA lives in that header rather than beside the
//                  renderer is that scene/chunk_render.c includes <3ds.h> and no host test can
//                  link it. A suite carrying its own copy of the ternary would stay green with
//                  the renderer's copy deleted, which is exactly the failure recorded against
//                  app/battery.c and app/sleep.c in tools/run_host_tests.sh.
//
//   THE CUTOFF     the alpha TEST is not being touched by this task, and 0.70 was chosen so it
//                  would not have to be. The renderer tests GPU_GREATER against ALPHA_CUTOFF
//                  and the TEV modulates alpha (C3D_Both), so a water fragment is 255 * 0.70
//                  and a leaf's cutout texel is 255 * 0. Both sides of that are asserted, from
//                  the cutoff value PARSED out of scene/chunk_render.c rather than restated.
//                  The failure it catches is the ugly one: pick an alpha under the cutoff and
//                  water does not become faint, it becomes INVISIBLE, and the seabed shows
//                  through a hole where the sea was.
//
//   THE SHADERS    both source/shaders/world.v.pica and world_dynamic.v.pica must read the
//                  alpha (`mov outclr.w, r3.wwww`) and neither may still nail it to 1.0
//                  (`mov outclr.w, ones`). world_dynamic is the program actually bound on both
//                  console models since v1.8.0 task 24, and world.v.pica is compiled and never
//                  bound — so an edit to only the one a human reads first is invisible on
//                  hardware, and an edit to only the other is invisible to anyone reading the
//                  file. The same drift world/atlas_uv_shader_test.c guards uvScale against.
//                  The BOUND file is checked first, so a red run names it first.
//
//   THE ROW INDEX  (v1.8.3) and — this is the part that was missing — WHICH ROW that fetch
//                  returns. `mov r3, faceShade[a0.x]` is relative addressing; a0.x comes from
//                  `mova` and nowhere else, and the swizzle on mova's operand picks which byte
//                  of the packed attribute is the row number. Every check above was satisfied
//                  by both instructions merely being PRESENT and in order, so the operand was
//                  free. MEASURED: `mova a0.x, inpack.zzzz` changed to `inpack.xxxx` in
//                  world.v.pica — every vertex indexing the table by its atlas u column, so
//                  wrong brightness and wrong alpha on every face in the world — and this
//                  suite printed "PASS 34 checks, 0 failed", not one check moved. It now
//                  asserts the whole instruction and that it precedes the fetch, and that
//                  arm goes red 2/42 in each shader.
//
//   THE BLEND      scene/chunk_render.c must own the blend unit at both ends. Before v1.8.2
//                  nothing in the world draw ever called C3D_AlphaBlend and exactly one place
//                  in the tree did — gfx/sprite.c's spriteBegin, for the UI, which never turns
//                  it off again. citro3d holds blend state in the persistent C3D_Context, so
//                  the world has been drawing with whatever the last HUD batch left standing,
//                  harmless only while every vertex alpha in the game was 255. The moment the
//                  transparent pass writes 0.70 that state decides whether water is
//                  see-through or whether the ENTIRE WORLD is, and a frame-1 screenshot taken
//                  before any UI has drawn would show neither.
//
//                  (v1.8.3) BOTH ENDS OF THAT CALL. C3D_AlphaBlend takes six arguments and
//                  both call sites in the tree wrap after the fourth, so the needle that
//                  described it stopped exactly where its first physical line stopped.
//                  loadLines() is one fgets() per line and joins no continuations, so the
//                  ALPHA src/dst factors were outside anything this file could see. MEASURED:
//                  scene/chunk_render.c:2013 changed from `GPU_SRC_ALPHA,
//                  GPU_ONE_MINUS_SRC_ALPHA);` to `GPU_ONE, GPU_ZERO);` and this suite printed
//                  "PASS 34 checks, 0 failed"; gfx/sprite.c:187 the same. BLEND_ON_TAIL now
//                  requires the factor pair on the immediately following line at both sites,
//                  and each arm goes red 1/42.
//
// A parse that finds nothing must not read as "nothing wrong", so every failure path below —
// file missing, line missing, malformed number — is itself a loud failure and never a skip.
// Same rule and same reason as world/atlas_uv_shader_test.c, which this file is modelled on.
//
// Own main(), own binary, appended to tools/run_host_tests.sh. The __3DS__ guard is
// load-bearing, not tidy: the console Makefile globs every .c under source/world, so without
// it this main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/watershimmer.h"
#include "world/block.h"
#include "world/chunk.h"
#include "world/mesher.h"
#include "world/scratch.h"

#define WORLD_SHADER_PATH   "source/shaders/world.v.pica"
#define DYNAMIC_SHADER_PATH "source/shaders/world_dynamic.v.pica"
#define RENDERER_PATH       "source/scene/chunk_render.c"

static int  s_checks;
static int  s_fails;

// Reports every failure, not just the first: a red run has to be readable case by case, so
// that a case which stayed green under sabotage can be spotted as neutralised.
#define CHECK(cond, ...) do {                                     \
		s_checks++;                                                \
		if (!(cond)) {                                             \
			s_fails++;                                             \
			printf("  FAIL L%d  ", __LINE__);                      \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		} else {                                                   \
			printf("  ok     ");                                   \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                          \
	} while (0)

// ── Reading a source file as lines ───────────────────────────────────────────

#define MAX_LINES 4096
#define MAX_LINE  512

static char s_lines[MAX_LINES][MAX_LINE];
static int  s_nlines;

// Whitespace-collapsed copy of one line: every run of spaces and tabs becomes one space and
// the ends are trimmed. Both .pica files write `mov outclr.w,   r3.wwww` with the operand
// column-aligned, so a needle that matched raw text would be checking the indentation.
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

// Returns the number of lines read, or -1 if the file could not be opened. The caller turns
// -1 into a failure; it is never treated as "no lines, therefore nothing to complain about".
static int loadLines(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	s_nlines = 0;
	char raw[4096];
	while (s_nlines < MAX_LINES && fgets(raw, sizeof raw, f))
		squash(raw, s_lines[s_nlines++], MAX_LINE);

	fclose(f);
	return s_nlines;
}

// How many loaded lines contain `needle`, and the line number (1-based) of the first.
static int countLines(const char* needle, int* first_line)
{
	int n = 0;
	if (first_line) *first_line = -1;
	for (int i = 0; i < s_nlines; i++) {
		if (!strstr(s_lines[i], needle)) continue;
		if (first_line && *first_line < 0) *first_line = i + 1;
		n++;
	}
	return n;
}

// Does the loaded line numbered `line1` (1-based) contain `needle`? The whole reason this
// exists is that loadLines() is one fgets() per line and joins no continuations, so a needle
// can only ever describe ONE physical line. A C call that wraps is therefore half-unchecked by
// construction, and the unchecked half fails silently — see BLEND_ON_TAIL below.
static bool lineHas(int line1, const char* needle)
{
	if (line1 < 1 || line1 > s_nlines) return false;
	return strstr(s_lines[line1 - 1], needle) != NULL;
}

// The loaded line numbered `line1`, or a placeholder — for printing the line a check is
// actually complaining about, so a red run does not have to be taken on trust.
static const char* lineText(int line1)
{
	if (line1 < 1 || line1 > s_nlines) return "<past the end of the file>";
	return s_lines[line1 - 1];
}

// The LAST line strictly before `before` (1-based) containing `needle`, or -1.
//
// This exists because a0.x is one register with one value at a time. What a relative fetch
// actually indexes by is whatever the most recent `mova` put there — so "which mova governs
// this fetch" is a last-before question, and never a count. Until v1.8.8 the file asked it as
// a count (`exactly one mova, so a0.x has exactly one source`), which was true of the shader
// as it stood and stopped being true the moment the biome tint added a second indexed fetch
// of its own. Counting was the weaker question anyway: two movas and two fetches are correct,
// while one mova inserted between the right one and its fetch is a wrong picture that a
// count of two would have accepted just as readily.
static int lastLineBefore(const char* needle, int before)
{
	int found = -1;
	for (int i = 0; i < s_nlines && i + 1 < before; i++)
		if (strstr(s_lines[i], needle)) found = i + 1;
	return found;
}

// ── The table ────────────────────────────────────────────────────────────────

// The claim: the faceShade row index IS the whole nrm byte, so rows 0..7 are the eight face
// slots at drop 0 and rows 8..63 are every non-zero drop. Since WATER_SURFACE_DROP a non-zero
// drop is carried by water and by nothing else, so those 56 rows are water-only and the alpha
// can ride there with no extra vertex bits at all.
static void testFaceShadeAlphaTable(void)
{
	puts("water alpha: the faceShade table's .w is 0.70 on water's rows and 1.0 on the rest");

	// The premise the whole scheme rests on. If the surface floor were 0, water would sit in
	// rows 0..7 alongside stone and there would be nothing to key the alpha off.
	CHECK(WATER_SURFACE_DROP >= 1,
	      "premise: WATER_SURFACE_DROP is %d, so every water vertex leaves rows 0..7",
	      WATER_SURFACE_DROP);
	CHECK(WATER_SURFACE_DROP < (1 << 3),
	      "premise: and it still fits nrm's three drop bits");

	int opaque_rows = 0, water_rows = 0, wrong = 0;
	for (unsigned i = 0; i < MESH_NRM_STATES; i++) {
		const float a = meshNrmAlpha((uint8_t)i);
		if (meshNrmDrop((uint8_t)i) >= WATER_SURFACE_DROP) {
			water_rows++;
			if (a != WATER_ALPHA) wrong++;
		} else {
			opaque_rows++;
			if (a != 1.0f) wrong++;
		}
	}

	CHECK(wrong == 0, "every one of the %u rows carries the alpha its drop calls for",
	      (unsigned)MESH_NRM_STATES);
	CHECK(opaque_rows == 8 && water_rows == 56,
	      "8 rows are drop 0 and 56 are not (got %d and %d)", opaque_rows, water_rows);

	// Spelled out at the two ends, because an off-by-one in the comparison would hide inside
	// the totals above. Row 5 is FACE_NORTH at drop 0; row 8 is FACE_EAST at drop 1, the very
	// first water row and the one WATER_SURFACE_DROP creates.
	CHECK(meshNrmAlpha(5) == 1.0f, "row 5 (a plain face, drop 0) is fully opaque");
	CHECK(meshNrmAlpha(8) == WATER_ALPHA,
	      "row 8 (drop 1, the recessed surface) is the water alpha");
	CHECK(meshNrmAlpha(63) == WATER_ALPHA, "and so is the last row");

	CHECK(WATER_ALPHA > 0.0f && WATER_ALPHA < 1.0f,
	      "WATER_ALPHA is %.3f: below 1.0, or nothing is see-through at all",
	      (double)WATER_ALPHA);
}

// ── The alpha test the renderer already had ──────────────────────────────────

// Parses `#define ALPHA_CUTOFF <n>` out of the renderer. Not restated here: the point of the
// check is that 0.70 clears the cutoff the renderer ACTUALLY uses, and a hand-copied 127 would
// keep saying so after someone raised it.
static bool readAlphaCutoff(long* out, char* err, size_t errsz)
{
	if (loadLines(RENDERER_PATH) < 0) {
		snprintf(err, errsz, "cannot open %s", RENDERER_PATH);
		return false;
	}

	for (int i = 0; i < s_nlines; i++) {
		const char* p = strstr(s_lines[i], "#define ALPHA_CUTOFF ");
		if (!p) continue;
		p += strlen("#define ALPHA_CUTOFF ");
		char* end = NULL;
		const long v = strtol(p, &end, 10);
		if (end == p) {
			snprintf(err, errsz, "ALPHA_CUTOFF found but unparseable: %.120s", s_lines[i]);
			return false;
		}
		*out = v;
		return true;
	}

	snprintf(err, errsz, "no '#define ALPHA_CUTOFF' line in %s", RENDERER_PATH);
	return false;
}

static void testAlphaTestSurvives(void)
{
	puts("water alpha: 0.70 clears the alpha test, and a leaf's cutout still does not");

	long cutoff = -1;
	char err[256] = { 0 };
	const bool got = readAlphaCutoff(&cutoff, err, sizeof err);
	CHECK(got, "ALPHA_CUTOFF parsed out of %s%s%s", RENDERER_PATH, got ? "" : ": ", err);
	if (!got) return;

	printf("  ...ALPHA_CUTOFF = %ld, WATER_ALPHA = %.2f\n", cutoff, (double)WATER_ALPHA);

	// TEV stage 0 is GPU_MODULATE over C3D_Both, so frag.a = tex.a * primary.a. The floor of
	// the conversion is used rather than the round, because the floor is the pessimistic one:
	// 0.70 * 255 = 178.5, and a hardware that truncates gives 178.
	const int water_frag = (int)(255.0f * WATER_ALPHA);
	const int leaf_solid = (int)(255.0f * 1.0f);
	const int leaf_hole  = (int)(0.0f * 1.0f);

	CHECK(water_frag > cutoff,
	      "water is %d > %ld, so the surface DRAWS (under it, water vanishes entirely)",
	      water_frag, cutoff);
	CHECK(leaf_solid > cutoff, "control: a leaf's opaque texel is %d and still draws",
	      leaf_solid);
	CHECK(leaf_hole <= cutoff, "control: a leaf's cutout texel is %d and is still discarded",
	      leaf_hole);
}

// ── The two shaders ──────────────────────────────────────────────────────────

#define ALPHA_READ_NEEDLE "mov outclr.w, r3.wwww"
#define ALPHA_PIN_NEEDLE  "mov outclr.w, ones"
#define MOVA_NEEDLE       "mova a0.x, inpack.zzzz"
#define FETCH_NEEDLE      "mov r3, faceShade[a0.x]"
#define TINT_MOVA_NEEDLE  "mova a0.x, r8.xxxx"
#define TINT_FETCH_NEEDLE "mov r10, tintPalette[a0.x]"

// One relative fetch, and the `mova` that decides what it reads.
//
// Split out because v1.8.8 gave these shaders a SECOND indexed fetch — the biome tint palette
// — and the question this asks is per-fetch, not per-file. `fetch_line_out` is the fetch's
// line number so the caller can keep using it for the ordering checks it already does.
static void checkIndexedFetch(const char* path, const char* fetch_needle,
                              const char* mova_needle, const char* indexed_by,
                              int* fetch_line_out)
{
	int fetch = -1;
	const int fetches = countLines(fetch_needle, &fetch);
	if (fetch_line_out) *fetch_line_out = fetch;

	CHECK(fetches == 1, "%s fetches `%s` exactly once (found %d)", path, fetch_needle, fetches);
	if (fetches != 1) return;

	// WHICH ROW — the only part of the expression the CPU never sees, and the part no needle
	// in this file described before the operand sabotage below was measured.
	const int gov = lastLineBefore("mova ", fetch);

	CHECK(gov > 0,
	      "%s sets a0.x at all before `%s` on L%d — with no mova above it the fetch reads "
	      "whatever the previous vertex left in the address register",
	      path, fetch_needle, fetch);
	if (gov <= 0) return;

	CHECK(lineHas(gov, mova_needle),
	      "%s indexes `%s` (L%d) by %s: the last mova above it is L%d `%s`, wanted `%s`",
	      path, fetch_needle, fetch, indexed_by, gov, lineText(gov), mova_needle);
}

// `bound` says whether this is the program the console actually runs. Both models have run
// world_dynamic.v.pica since v1.8.0 task 24 (scene/chunk_render.c's chunkRenderInit parses
// world_dynamic_shbin and nothing parses world_shbin), so world.v.pica is compiled, checked,
// and never on screen. The flag is only ever printed: both files are checked identically, and
// the point of saying which is which is that a red run should be readable without going and
// looking up which of the two names is the live one.
static void checkOneShader(const char* path, bool bound)
{
	const int n = loadLines(path);
	CHECK(n >= 0, "%s opens and reads", path);
	if (n < 0) return;

	int line = -1;
	const int reads = countLines(ALPHA_READ_NEEDLE, &line);
	const int pins  = countLines(ALPHA_PIN_NEEDLE, NULL);

	CHECK(reads == 1, "%s writes the vertex alpha from the table exactly once (found %d)",
	      path, reads);
	CHECK(pins == 0, "%s no longer nails outclr.w to 1.0 (found %d such lines)", path, pins);

	// The row fetch the alpha rides on. If r3 stopped being faceShade[nrm] the .w above would
	// be reading some other register's leftovers, which is a wrong picture and not an error.
	int fetch = -1;
	CHECK(countLines(FETCH_NEEDLE, &fetch) == 1,
	      "%s still fetches the whole faceShade row into r3 [%s]", path,
	      bound ? "BOUND" : "not bound");
	CHECK(fetch > 0 && line > 0 && fetch < line,
	      "%s fetches it BEFORE it reads .w out of it (fetch L%d, read L%d)",
	      path, fetch, line);

	// WHICH ROW. Everything above proves the fetch is present, in order, and read from — and
	// none of it looks at the INDEX, which is the only part of the expression the CPU never
	// sees. `faceShade[a0.x]` is relative addressing; a0.x comes from `mova` and from nothing
	// else, and the swizzle on `mova`'s operand chooses which byte of the packed attribute
	// becomes the row number. inpack is (u, v, nrm, ao), so .zzzz is the nrm byte and .xxxx is
	// the vertex's atlas u column.
	//
	// MEASURED, not argued: with world.v.pica's `mova a0.x, inpack.zzzz` changed to
	// `inpack.xxxx` — every vertex fetching the row numbered by its u coordinate, so wrong
	// per-face brightness AND wrong per-face alpha across the whole world — this suite printed
	// "PASS 34 checks, 0 failed", the same 34 as a healthy tree, with not one check moved.
	// Both instructions were still present and still in the right order; only the operand
	// changed, and no needle in this file described an operand.
	// .xxxx or .yyyy on that mova's operand indexes the table by a texture coordinate instead,
	// which draws a lit world with every face at another face's brightness and alpha.
	checkIndexedFetch(path, FETCH_NEEDLE, MOVA_NEEDLE, "the nrm byte", NULL);

	// v1.8.8's second indexed fetch. r8.x is the tint index the shader derives from the top
	// bits of the ao byte just above; indexing the palette by anything else paints the world
	// in another biome's colours, which is a picture, not an error — the same silent class of
	// wrongness as the face-row sabotage measured above, one register along.
	checkIndexedFetch(path, TINT_FETCH_NEEDLE, TINT_MOVA_NEEDLE, "the derived tint index", NULL);

	// Task 13b's atlas layout must not have been disturbed on the way past.
	CHECK(countLines(".constf uvScale(0.0625, 0.015625, 0.0, 0.0)", NULL) == 1,
	      "control: %s's uvScale literal is untouched", path);
}

static void testShadersReadAlpha(void)
{
	puts("water alpha: BOTH world shaders read the alpha, and neither still pins it to 1.0");

	// The BOUND one first. Until now this suite checked world.v.pica first, which is the file
	// a human reads first and the file the console has not run since v1.8.0 — so the head of a
	// red run described the dead copy. world/atlas_uv_shader_test.c asserts which of the two
	// scene/chunk_render.c actually parses, so this ordering cannot quietly become wrong.
	checkOneShader(DYNAMIC_SHADER_PATH, true);
	checkOneShader(WORLD_SHADER_PATH, false);
}

// ── The blend state ──────────────────────────────────────────────────────────

#define BLEND_OFF_NEEDLE "C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, " \
                         "GPU_ONE, GPU_ZERO);"
#define BLEND_ON_NEEDLE  "C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, " \
                         "GPU_ONE_MINUS_SRC_ALPHA,"

// The SECOND physical line of that same call, and the reason it needs its own needle.
//
// C3D_AlphaBlend takes six arguments: the two equations, then the COLOUR src/dst factors, then
// the ALPHA src/dst factors. Both call sites in the tree wrap after the fourth argument, so
// BLEND_ON_NEEDLE above ends exactly where the first physical line ends and describes the
// colour factors only. loadLines() is one fgets() per line and joins no continuations, so the
// alpha factors were outside everything this file could see.
//
// MEASURED: with scene/chunk_render.c:2013 changed from `GPU_SRC_ALPHA,
// GPU_ONE_MINUS_SRC_ALPHA);` to `GPU_ONE, GPU_ZERO);` — the destination alpha thrown away, so
// the blend unit is configured differently from what the call is written to say — this suite
// printed "PASS 34 checks, 0 failed". Nothing in the file had an opinion about line 2013.
// Same blind spot, same needle, at gfx/sprite.c:187.
#define BLEND_ON_TAIL    "GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);"

static void testRendererOwnsBlendState(void)
{
	puts("water alpha: the world draw owns the blend unit at both ends");

	const int n = loadLines(RENDERER_PATH);
	CHECK(n >= 0, "%s opens and reads", RENDERER_PATH);
	if (n < 0) return;

	int off_first = -1, on_first = -1;
	const int total = countLines("C3D_AlphaBlend(", NULL);
	const int offs  = countLines(BLEND_OFF_NEEDLE, &off_first);
	const int ons   = countLines(BLEND_ON_NEEDLE, &on_first);

	CHECK(total == 3 && offs == 2 && ons == 1,
	      "three C3D_AlphaBlend calls: two ONE/ZERO and one SRC_ALPHA (got %d, %d, %d)",
	      total, offs, ons);

	// ...and the half of that call the line above cannot reach. The check just above stays
	// green whatever line on_first+1 says, so this one is what makes the SRC_ALPHA call a
	// whole call rather than its first line.
	CHECK(lineHas(on_first + 1, BLEND_ON_TAIL),
	      "the blending call's ALPHA factors are on its continuation line L%d, which reads "
	      "'%s' — expected it to contain '%s'",
	      on_first + 1, lineText(on_first + 1), BLEND_ON_TAIL);

	// Where the blending one sits. It must be inside the transparent pass — after the alpha
	// test is armed against ALPHA_CUTOFF and before the pass turns it off again — or water is
	// blended during the opaque pass, or not at all.
	int arm = -1, disarm = -1;
	CHECK(countLines("C3D_AlphaTest(true, GPU_GREATER, ALPHA_CUTOFF);", &arm) == 1,
	      "the transparent pass still arms the alpha test exactly once");
	for (int i = 0; i < s_nlines; i++)
		if (arm > 0 && i + 1 > arm && strstr(s_lines[i], "C3D_AlphaTest(false, GPU_ALWAYS, 0);"))
			{ disarm = i + 1; break; }

	CHECK(disarm > 0, "and turns it off again after the draws (found at L%d)", disarm);
	CHECK(on_first > arm && on_first < disarm,
	      "blending is armed INSIDE the transparent pass (L%d, between L%d and L%d)",
	      on_first, arm, disarm);

	// And the pass puts it back. The last ONE/ZERO must come after the transparent draws, or
	// scene/highlight.c, scene/crackoverlay.c and scene/playermodel.c inherit SRC_ALPHA
	// blending from the world instead of the plain writes they were written against.
	int last_off = -1;
	for (int i = 0; i < s_nlines; i++)
		if (strstr(s_lines[i], BLEND_OFF_NEEDLE)) last_off = i + 1;
	CHECK(last_off > on_first,
	      "and restored after them, so the passes that follow inherit no blending (L%d)",
	      last_off);

	// The renderer must actually call the shared rule rather than carry its own copy of the
	// ternary — otherwise everything testFaceShadeAlphaTable proved is about dead code.
	int fill = -1;
	CHECK(countLines("meshNrmAlpha((uint8_t)i)", &fill) == 1,
	      "the faceShade fill calls meshNrmAlpha, so the table test is about live code");
	CHECK(countLines("s_uloc_faceshade + (int)i, s, dy, s, a);", NULL) == 1,
	      "and hands the result to the uniform's .w component");
}

// ── The other half of the hazard: the UI still arms its own blending ─────────

static void testUiStillArmsItsOwnBlend(void)
{
	puts("water alpha: the HUD is unaffected because spriteBegin arms blending itself");

	const int n = loadLines("source/gfx/sprite.c");
	CHECK(n >= 0, "source/gfx/sprite.c opens and reads");
	if (n < 0) return;

	int ui_on = -1;
	CHECK(countLines(BLEND_ON_NEEDLE, &ui_on) == 1,
	      "control: gfx/sprite.c still sets SRC_ALPHA blending for its own batch, so the "
	      "world's restore cannot leave the UI unblended");

	// spriteBegin's call wraps after the fourth argument exactly as the renderer's does, so it
	// has the same unchecked second line and gets the same needle. A HUD drawn with the alpha
	// factors thrown away is every translucent panel and every font edge rendered wrong, with
	// nothing anywhere reporting an error.
	CHECK(lineHas(ui_on + 1, BLEND_ON_TAIL),
	      "and its ALPHA factors on the continuation line L%d, which reads '%s' — expected it "
	      "to contain '%s'", ui_on + 1, lineText(ui_on + 1), BLEND_ON_TAIL);
}

// ── v1.8.10: the shimmer scroll ──────────────────────────────────────────────

// gfx/watershimmer.h is the arithmetic behind the water glint's movement, and it is the same
// class of thing as everything else in this file: it cannot fail loudly. A scroll that does not
// advance compiles, links, binds, samples a real texture and renders a perfectly plausible
// picture — water with a static highlight pattern painted on it. That is the difference between
// this feature working and not working, and no test anywhere else in the tree can see it,
// because scene/chunk_render.c includes <3ds.h> and cannot be linked here. The header exists so
// this suite can link the REAL rule; if the renderer is ever changed to compute its own offsets
// inline, these checks go on passing about dead code — which is why the last check below pins
// scene/chunk_render.c to calling the functions rather than restating them.
//
// Appended to this file rather than given its own binary because tools/run_host_tests.sh was
// being edited by two other sessions while this landed and could not be touched. Water's suite
// is the right home for it anyway.
static void testWaterShimmerScroll(void)
{
	puts("water shimmer: the scroll offset advances with time and wraps");

	// The premise the sheet's seamlessness rests on. The shader builds the shimmer coordinate
	// from a CHUNK-LOCAL position (the vertex format is 8 bytes and locked, so there is nowhere
	// to put a world coordinate), which means the sheet restarts at every chunk boundary. That
	// is invisible only while a chunk spans a WHOLE number of repeats. At 0.125 per block and
	// CHUNK_DIM 16 that is exactly 2. Any other scale draws a 16-block grid on the ocean.
	const float per_chunk = (float)CHUNK_DIM * WATER_SHIMMER_UV_PER_BLOCK;
	CHECK(per_chunk == (float)(int)per_chunk,
	      "premise: a chunk spans %.3f whole repeats of the sheet (CHUNK_DIM %d x %.4f per "
	      "block), so the pattern is continuous across a chunk boundary",
	      (double)per_chunk, CHUNK_DIM, (double)WATER_SHIMMER_UV_PER_BLOCK);

	CHECK(WATER_SHIMMER_PERIOD_U_MS != WATER_SHIMMER_PERIOD_V_MS,
	      "premise: the two axes wrap at different rates (%u ms, %u ms), so the pair of phases "
	      "does not repeat on the shorter of the two",
	      WATER_SHIMMER_PERIOD_U_MS, WATER_SHIMMER_PERIOD_V_MS);

	// ADVANCING. Eight samples a second apart, each strictly greater than the last, all inside
	// one period so no wrap can be mistaken for progress. This is the check the whole feature
	// lives or dies by: a shimmer that does not move is the documented wrong answer.
	int rising = 0;
	float prev = waterShimmerOffsetU(0);
	for (uint64_t t = 1000; t < WATER_SHIMMER_PERIOD_U_MS; t += 1000) {
		const float now = waterShimmerOffsetU(t);
		if (now > prev) rising++;
		prev = now;
	}
	CHECK(rising == 8, "u advances at every one of 8 one-second steps inside its %u ms period "
	      "(got %d)", WATER_SHIMMER_PERIOD_U_MS, rising);

	// ...and by a real amount, not by a rounding error. One second is one ninth of a wrap.
	const float step = waterShimmerOffsetU(1000) - waterShimmerOffsetU(0);
	CHECK(step > 0.10f && step < 0.12f,
	      "and by 1/9 of the sheet per second (measured %.5f)", (double)step);

	// WRAPPING. Exactly at the period it is back to the start, and just before it, it is nearly
	// a whole sheet along. A phase that saturated at 1.0 instead of wrapping would pass every
	// "advancing" check above and then freeze after nine seconds.
	CHECK(waterShimmerOffsetU(0) == 0.0f, "u starts at 0.0");
	CHECK(waterShimmerOffsetU(WATER_SHIMMER_PERIOD_U_MS) == 0.0f,
	      "u is back to 0.0 after exactly one period (%u ms)", WATER_SHIMMER_PERIOD_U_MS);
	CHECK(waterShimmerOffsetU(WATER_SHIMMER_PERIOD_U_MS - 1) > 0.999f,
	      "and was still just short of a whole sheet one millisecond earlier (%.5f)",
	      (double)waterShimmerOffsetU(WATER_SHIMMER_PERIOD_U_MS - 1));
	CHECK(waterShimmerOffsetV(WATER_SHIMMER_PERIOD_V_MS) == 0.0f,
	      "v wraps on its own period (%u ms) and not on u's", WATER_SHIMMER_PERIOD_V_MS);
	CHECK(waterShimmerOffsetV(WATER_SHIMMER_PERIOD_U_MS) != 0.0f,
	      "control: v has NOT wrapped at u's period, so the two are genuinely independent");

	// IN RANGE, everywhere. Sampled across three full periods at an interval that is coprime
	// with neither, so the samples land all over both cycles rather than on the same few phases.
	int out_of_range = 0;
	for (uint64_t t = 0; t < 3u * WATER_SHIMMER_PERIOD_V_MS; t += 37) {
		const float u = waterShimmerOffsetU(t);
		const float v = waterShimmerOffsetV(t);
		if (!(u >= 0.0f && u < 1.0f)) out_of_range++;
		if (!(v >= 0.0f && v < 1.0f)) out_of_range++;
	}
	CHECK(out_of_range == 0,
	      "every offset over three periods is in [0, 1) — the texture wrap is GPU_REPEAT, so an "
	      "out-of-range value is not an error, it is a differently-wrong picture");

	// A LARGE CLOCK. The console's tick counter is milliseconds since the SYSTEM booted, not
	// since the game did, so a 3DS left in sleep for a week hands this a number in the hundreds
	// of millions. That is the case the modulo-then-divide shape exists for: the equivalent
	// small time must give the SAME phase, bit for bit. `seconds * speed` in float32 would not.
	const uint64_t week = 7ull * 24ull * 60ull * 60ull * 1000ull;
	CHECK(waterShimmerOffsetU(week + 4000u) == waterShimmerOffsetU((week + 4000u) % WATER_SHIMMER_PERIOD_U_MS),
	      "a week-old clock (%llu ms) gives the same phase as its remainder does",
	      (unsigned long long)week);
	CHECK(waterShimmerOffsetU(week + 4000u) != waterShimmerOffsetU(week + 5000u),
	      "and is still MOVING a week in — the float has not run out of resolution");

	// ...and that the renderer uses this rule rather than a copy of it. Everything above is
	// about dead code if scene/chunk_render.c computes its own offsets inline.
	const int n = loadLines(RENDERER_PATH);
	CHECK(n >= 0, "%s opens and reads", RENDERER_PATH);
	if (n < 0) return;

	CHECK(countLines("waterShimmerOffsetU(s_shimmer_ms)", NULL) == 1 &&
	      countLines("waterShimmerOffsetV(s_shimmer_ms)", NULL) == 1,
	      "the renderer's uniform upload calls both of these functions, so the checks above are "
	      "about live code");

	// THE GATE. The whole design of this feature is that with the shaders option off, no new GPU
	// state is issued at all — v1.8.10 also ships the fix for the freeze that locked up steve's
	// console, and a hardware result has to be attributable to that fix and not to an untested
	// TEV stage. Both calls that touch the GPU must therefore be inside the one `if`.
	int gate = -1, bind_line = -1, tev_line = -1;
	CHECK(countLines("const bool shimmer_on = s_fake_shading && s_shimmer_ready;", &gate) == 1,
	      "the shimmer gate is derived exactly once, from the option AND the texture import");
	CHECK(countLines("waterShimmerBind();", &bind_line) == 1, "unit 2 is bound in exactly one place");
	CHECK(countLines("waterShimmerTevSet();", &tev_line) == 1, "TEV stage 2 is set in exactly one place");
	CHECK(gate > 0 && bind_line == gate + 2 && tev_line == gate + 3,
	      "and both sit immediately inside `if (shimmer_on)` on L%d (bind L%d, tev L%d) — if "
	      "either escapes the gate, a default install starts issuing GPU state it has never "
	      "been tested with",
	      gate, bind_line, tev_line);
	CHECK(countLines("waterShimmerTevReset();", NULL) == 1,
	      "and the stage is put back to passthrough afterwards, so it cannot leak into the HUD");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== water alpha test ==");

	testFaceShadeAlphaTable();
	testAlphaTestSurvives();
	testShadersReadAlpha();
	testRendererOwnsBlendState();
	testUiStillArmsItsOwnBlend();
	testWaterShimmerScroll();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
