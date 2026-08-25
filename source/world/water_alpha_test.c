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

#include "world/block.h"
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
	const int movas = countLines("mova ", NULL);
	int mova_line = -1;
	const int nrm_movas = countLines(MOVA_NEEDLE, &mova_line);

	CHECK(movas == 1, "%s has exactly one mova, so a0.x has exactly one source (found %d)",
	      path, movas);
	CHECK(nrm_movas == 1,
	      "%s loads a0.x from the nrm byte with `%s` (found %d) — .xxxx or .yyyy there indexes "
	      "the table by a texture coordinate instead, which draws a lit world with every face "
	      "at another face's brightness and alpha", path, MOVA_NEEDLE, nrm_movas);
	CHECK(mova_line > 0 && fetch > 0 && mova_line < fetch,
	      "%s sets a0.x BEFORE `%s` reads it (mova L%d, fetch L%d) — after it, the fetch uses "
	      "whatever the previous vertex left in the address register",
	      path, FETCH_NEEDLE, mova_line, fetch);

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

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== water alpha test ==");

	testFaceShadeAlphaTable();
	testAlphaTestSurvives();
	testShadersReadAlpha();
	testRendererOwnsBlendState();
	testUiStillArmsItsOwnBlend();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
