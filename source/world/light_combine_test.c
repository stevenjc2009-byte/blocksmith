// Host tests for v1.8.10's additive lightmap combine in source/shaders/world_dynamic.v.pica.
//
// v1.8.9 combined the sky and block light channels with `max r5.w, r5.zzzz, r5.yyyy` and
// applied ONE brightness curve to the winner. v1.8.10 replaced that with vanilla's real rule:
// each channel is curved SEPARATELY, then ADDED, then clamped to 1 --
//
//   lum = min(1, curve(sky x dayLevel) + 1.5 x curve(block))
//   curve(l) = l / (4 - 3l)
//
// This file pins that change from both sides, because neither side can fail loudly on its own:
//
//   THE SHADER TEXT   No C compiler ever reads a .pica file, and picasso does not care whether
//                      the combine is `add` or `max` -- both assemble cleanly and both produce
//                      A picture, never an error. So the source text is parsed directly, the
//                      same technique world/water_alpha_test.c and world/atlas_uv_shader_test.c
//                      use on the same file, for the same reason: a silent revert to max() is a
//                      wrong picture, not a build failure.
//
//                      Comments are STRIPPED before matching (picasso's comment character is
//                      ';', same as those two files' PICA_COMMENTS handling). This shader's own
//                      header prose says "max()" eight times while explaining why it was
//                      removed -- a needle that did not strip comments would find the word "max"
//                      all over a file that no longer contains the instruction.
//
//   THE ARITHMETIC     The shader half proves the INSTRUCTIONS are right; it says nothing about
//                      whether the numbers they produce are the numbers v1.8.9 measured and
//                      v1.8.10 promised not to move. So the combine is reproduced in C and
//                      pinned against the values in the task brief and in the shader's own
//                      comments (curveConsts' citation, and the "3.2x too bright" arithmetic in
//                      docs/VERSION-LIST.md's v1.8.9 entry).
//
// A parse that finds nothing must not read as "nothing wrong": every failure path below (file
// missing, needle missing, malformed number) is itself a loud failure, never a skip. Same rule
// and same reason as the two files this one is modelled on.
//
// Own main(), own binary, appended to tools/run_host_tests.sh next to water_alpha_test and
// atlas_uv_shader_test. No __3DS__ guard is needed here (nothing in this file is linked into any
// console binary -- it has no companion .c under source/world with a colliding main()), but one
// is included anyway, at no cost, because every sibling test in this directory carries one and
// the console Makefile globs the whole directory.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DYNAMIC_SHADER_PATH "source/shaders/world_dynamic.v.pica"

static int  s_checks;
static int  s_fails;

// Reports every failure, not just the first: a red run has to be readable case by case, so that
// a case which stayed green under sabotage can be spotted as neutralised. Lifted verbatim from
// world/water_alpha_test.c and world/atlas_uv_shader_test.c.
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

// ── Reading the shader source as lines ───────────────────────────────────────
//
// Lifted from world/atlas_uv_shader_test.c rather than reinvented, including its comment
// stripping: both files parse the same two .pica files and both need "a line" to mean the same
// thing. picasso's comment character is ';', so a line is truncated at its first ';' BEFORE
// whitespace-squashing, and every needle below is matched against code only. See that file's own
// header for the concrete failure this prevents (its fog-comment false-positive).
#define MAX_LINES 4096
#define MAX_LINE  512

static char s_lines[MAX_LINES][MAX_LINE];
static int  s_nlines;

// Whitespace-collapsed copy of one line: every run of spaces/tabs becomes one space and the
// ends are trimmed. Both .pica files column-align operands, so a needle matched against raw
// text would be checking the indentation rather than the instruction.
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

// Returns the number of lines read, or -1 if the file could not be opened. The caller turns -1
// into a failure; it is never treated as "no lines, therefore nothing to complain about".
static int loadLines(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	s_nlines = 0;
	char raw[4096];
	while (s_nlines < MAX_LINES && fgets(raw, sizeof raw, f)) {
		char* c = strchr(raw, ';');
		if (c) *c = '\0';
		squash(raw, s_lines[s_nlines++], MAX_LINE);
	}

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

// How many loaded lines in the INCLUSIVE 1-based range [lo, hi] start with `opcode` -- i.e. the
// squashed line's first token is the mnemonic itself, not merely a substring of some operand or
// alias name. This is what makes the "no max left" check specific: `strstr` for "max" would also
// match nothing here (no register or alias in this file contains "max"), but a mnemonic check is
// the correct tool for "is this instruction present" and is what the rest of this file uses it
// for, so the same helper serves both the rcp count and the max-absence check.
static int countOpcodeInRange(const char* opcode, int lo, int hi)
{
	const size_t oplen = strlen(opcode);
	int n = 0;
	for (int i = lo; i <= hi && i >= 1; i++) {
		if (i > s_nlines) break;
		const char* line = s_lines[i - 1];
		if (strncmp(line, opcode, oplen) == 0 && (line[oplen] == ' ' || line[oplen] == '\0'))
			n++;
	}
	return n;
}

// How many loaded lines in the INCLUSIVE 1-based range [lo, hi] contain `needle` anywhere, and
// the line number of the first.
static int countNeedleInRange(const char* needle, int lo, int hi, int* first_line)
{
	int n = 0;
	if (first_line) *first_line = -1;
	for (int i = lo; i <= hi && i >= 1; i++) {
		if (i > s_nlines) break;
		if (!strstr(s_lines[i - 1], needle)) continue;
		if (first_line && *first_line < 0) *first_line = i;
		n++;
	}
	return n;
}

// The loaded line numbered `line1` (1-based), or a placeholder -- for printing the line a check
// is actually complaining about, so a red run does not have to be taken on trust.
static const char* lineText(int line1)
{
	if (line1 < 1 || line1 > s_nlines) return "<past the end of the file>";
	return s_lines[line1 - 1];
}

// ── Half 1: the shader text ──────────────────────────────────────────────────
//
// Anchors that bound "the adaptive-light block" without depending on the comments that name it
// (those are stripped before anything here runs). BLOCK_START is the FIRST instruction of the
// unpack (sky/block nibble split); BLOCK_END is the clamp, the LAST instruction the block
// writes. Both are checked for uniqueness before being trusted as bounds, the same discipline
// water_alpha_test.c's checkIndexedFetch applies to its own fetch/mova pair.
#define BLOCK_START_NEEDLE "mul r5.x, inv16, inpos.wwww"
#define BLOCK_END_NEEDLE   "min r5.w, ones, r5.wwww"
#define ADD_COMBINE_NEEDLE "add r5.w, r5.zzzz, r5.yyyy"
#define CURVECONSTS_NEEDLE ".constf curveConsts("

// Parses ".constf curveConsts(a, b, c, d)" and returns all four components. Modelled on
// world/atlas_uv_shader_test.c's readShaderUvScale, generalised from two components to four.
static bool readCurveConsts(double out[4], char* errbuf, size_t errbufsz)
{
	int line = -1;
	if (countLines(CURVECONSTS_NEEDLE, &line) != 1) {
		snprintf(errbuf, errbufsz, "expected exactly one '%s' line in %s, found %d",
		         CURVECONSTS_NEEDLE, DYNAMIC_SHADER_PATH, countLines(CURVECONSTS_NEEDLE, NULL));
		return false;
	}

	const char* found = lineText(line);
	const char* open_paren = strstr(found, CURVECONSTS_NEEDLE) + strlen(CURVECONSTS_NEEDLE);
	const char* close_paren = strchr(open_paren, ')');
	if (!close_paren) {
		snprintf(errbuf, errbufsz, "malformed curveConsts(...) line in %s: %.150s",
		         DYNAMIC_SHADER_PATH, found);
		return false;
	}

	char args[256];
	const size_t len = (size_t)(close_paren - open_paren);
	if (len >= sizeof(args)) {
		snprintf(errbuf, errbufsz, "curveConsts(...) line too long in %s", DYNAMIC_SHADER_PATH);
		return false;
	}
	memcpy(args, open_paren, len);
	args[len] = '\0';

	char* fields[4] = {0};
	int nfields = 0;
	char* tok = strtok(args, ",");
	while (tok && nfields < 4) {
		fields[nfields++] = tok;
		tok = strtok(NULL, ",");
	}
	if (nfields != 4) {
		snprintf(errbuf, errbufsz, "expected 4 components in curveConsts(...), found %d in %s",
		         nfields, DYNAMIC_SHADER_PATH);
		return false;
	}

	for (int i = 0; i < 4; i++) {
		char* end = NULL;
		const double v = strtod(fields[i], &end);
		if (end == fields[i]) {
			snprintf(errbuf, errbufsz, "component %d of curveConsts(...) is not a number in %s: '%s'",
			         i, DYNAMIC_SHADER_PATH, fields[i]);
			return false;
		}
		while (*end == ' ' || *end == '\t') end++;
		if (*end != '\0') {
			snprintf(errbuf, errbufsz, "trailing garbage after component %d in %s: '%s'",
			         i, DYNAMIC_SHADER_PATH, fields[i]);
			return false;
		}
		out[i] = v;
	}
	return true;
}

static void testShaderTextCombine(void)
{
	puts("light combine: world_dynamic.v.pica computes curve(sky)+1.5*curve(block), clamped, "
	     "not max()");

	const int n = loadLines(DYNAMIC_SHADER_PATH);
	CHECK(n >= 0, "%s opens and reads", DYNAMIC_SHADER_PATH);
	if (n < 0) return;

	// The block's bounds, each proven unique before being trusted as a boundary.
	int start_count, end_count, start_line, end_line;
	start_count = countLines(BLOCK_START_NEEDLE, &start_line);
	end_count   = countLines(BLOCK_END_NEEDLE, &end_line);

	CHECK(start_count == 1, "%s contains `%s` exactly once (found %d) -- this is the block's "
	      "start anchor and has to be unambiguous before anything below can trust it",
	      DYNAMIC_SHADER_PATH, BLOCK_START_NEEDLE, start_count);
	CHECK(end_count == 1, "%s contains `%s` exactly once (found %d) -- the clamp, and the "
	      "block's end anchor", DYNAMIC_SHADER_PATH, BLOCK_END_NEEDLE, end_count);
	if (start_count != 1 || end_count != 1) return;

	CHECK(start_line < end_line, "the block starts (L%d) before it ends (L%d)",
	      start_line, end_line);
	if (start_line >= end_line) return;

	// No `max` instruction anywhere in the block. A leftover max would mean the change silently
	// reverted; this is a mnemonic match (see countOpcodeInRange), not a substring match, so it
	// is blind to the word "max" appearing in prose -- which this file's own header does, eight
	// times, to explain why the instruction is gone.
	const int max_count = countOpcodeInRange("max", start_line, end_line);
	CHECK(max_count == 0, "no `max` instruction between L%d and L%d (found %d) -- a leftover max "
	      "would mean v1.8.10's additive combine silently reverted to v1.8.9's",
	      start_line, end_line, max_count);

	// The combine is an `add`, on the exact registers the two curved terms live in.
	int add_line = -1;
	const int add_count = countNeedleInRange(ADD_COMBINE_NEEDLE, start_line, end_line, &add_line);
	CHECK(add_count == 1, "the block contains `%s` exactly once (found %d) -- the additive "
	      "combine of the curved sky and block terms", ADD_COMBINE_NEEDLE, add_count);

	// The clamp. BLOCK_END_NEEDLE already located it as the block's end anchor; this restates it
	// as its own check so a reader does not have to infer the clamp's presence from the anchor
	// search above.
	CHECK(countLines(BLOCK_END_NEEDLE, NULL) == 1, "the block ends with `%s`, the clamp to 1",
	      BLOCK_END_NEEDLE);

	// Exactly TWO `rcp` instructions -- one curve per channel. One `rcp` would mean someone
	// collapsed the two separate curves back into a single curve over a combined value, which is
	// exactly the v1.8.9 shape this task replaced.
	const int rcp_count = countOpcodeInRange("rcp", start_line, end_line);
	CHECK(rcp_count == 2, "the block contains exactly 2 `rcp` instructions (found %d) -- one "
	      "curve per channel; one would mean the two curves were collapsed back into one over "
	      "the combined value", rcp_count);

	// ORDER: both curves must be computed before the add, and the add before the clamp. Presence
	// alone does not prove the shape -- the same three lessons water_alpha_test.c and
	// atlas_uv_shader_test.c both record for their own ordered pairs.
	if (add_count == 1) {
		int last_rcp = -1;
		for (int i = start_line; i <= add_line && i <= s_nlines; i++)
			if (strncmp(s_lines[i - 1], "rcp ", 4) == 0) last_rcp = i;
		CHECK(last_rcp > 0 && last_rcp < add_line, "the last `rcp` (L%d) comes before the add "
		      "(L%d) -- both channels must be curved before they are combined", last_rcp, add_line);
		CHECK(add_line < end_line, "the add (L%d) comes before the clamp (L%d)",
		      add_line, end_line);
	}

	// The constant bank: 4.0 and -3.0 for the curve, 1.5 for the block multiplier.
	double consts[4] = {0};
	// 512, not the 256 the sibling tests use for a similar buffer: readCurveConsts's own
	// snprintf calls embed DYNAMIC_SHADER_PATH (a compile-time constant, so gcc can size this
	// exactly) plus up to 150 bytes of quoted line content, and -Wformat-truncation rejects a
	// buffer it can prove is too small for the worst case. 256 was provably enough for THIS
	// file's own short path and still failed the proof once this file's copies were pointed at
	// a long scratch path during the red-run rehearsal below -- so this is sized for headroom,
	// not for the one path this repo happens to use today.
	char err[512] = {0};
	const bool got_consts = readCurveConsts(consts, err, sizeof(err));
	CHECK(got_consts, "%s", got_consts ? "curveConsts(...) parsed" : err);
	if (got_consts) {
		CHECK(fabs(consts[0] - 4.0) < 1e-9, "curveConsts.x is %.6f, expected 4.0 (the curve's "
		      "denominator constant)", consts[0]);
		CHECK(fabs(consts[1] - (-3.0)) < 1e-9, "curveConsts.y is %.6f, expected -3.0 (the "
		      "curve's l coefficient)", consts[1]);
		CHECK(fabs(consts[2] - 1.5) < 1e-9, "curveConsts.z is %.6f, expected 1.5 (vanilla's "
		      "torch multiplier at rest, aliased to curveBlockMul)", consts[2]);
	}
}

// ── Half 2: the arithmetic ───────────────────────────────────────────────────
//
// Reproduces the combine in plain C, in the same order the shader computes it, and pins real
// numbers. This is deliberately NOT a copy of the shader's instructions -- there is no way to
// execute a .pica file on a host -- it is an independent statement of the same formula, checked
// against values either given in the task brief or derivable from the shader's own cited
// arithmetic (curveConsts' comment and docs/VERSION-LIST.md's v1.8.9 "3.2x too bright" entry).
#define CURVE_FOUR       4.0
#define CURVE_NEG_THREE (-3.0)
#define CURVE_BLOCK_MUL  1.5
#define LIGHT_TOLERANCE  1e-5

static double curveOf(double l)
{
	return l / (CURVE_FOUR + CURVE_NEG_THREE * l);
}

// sky/block are the raw 0..15 nibbles the mesher packs (world/mesher.c); dayLevel is the
// day-night uniform. Mirrors the shader instruction for instruction: normalise both nibbles by
// 1/15, scale sky by dayLevel, curve each channel SEPARATELY, weight the block curve by 1.5,
// add, clamp to 1.
static double combinedLum(double skyRaw, double blockRaw, double dayLevel)
{
	const double skyNorm   = skyRaw / 15.0;
	const double blockNorm = blockRaw / 15.0;
	const double curvedSky   = curveOf(dayLevel * skyNorm);
	const double curvedBlock = curveOf(blockNorm) * CURVE_BLOCK_MUL;
	const double sum = curvedSky + curvedBlock;
	return sum > 1.0 ? 1.0 : sum;
}

// The clamp removed: for proving the clamp actually engages, and nothing else.
static double unclampedLum(double skyRaw, double blockRaw, double dayLevel)
{
	const double skyNorm   = skyRaw / 15.0;
	const double blockNorm = blockRaw / 15.0;
	const double curvedSky   = curveOf(dayLevel * skyNorm);
	const double curvedBlock = curveOf(blockNorm) * CURVE_BLOCK_MUL;
	return curvedSky + curvedBlock;
}

static double absd(double v) { return v < 0.0 ? -v : v; }

static void testArithmeticCombine(void)
{
	puts("light combine: the additive formula reproduced in C matches the measured/pinned "
	     "numbers");

	// A fully sunlit face must still match the baked shader's output exactly, unchanged from
	// before v1.8.10: with no emitter the block term is zero and the sky term alone must hit 1.0.
	CHECK(fabs(combinedLum(15.0, 0.0, 1.0) - 1.0) < LIGHT_TOLERANCE,
	      "sky=15 block=0 dayLevel=1.0 -> %.7f, expected 1.0 (a fully sunlit face)",
	      combinedLum(15.0, 0.0, 1.0));

	// v1.8.9's MEASURED midnight value. With block=0 the additive combine reduces to exactly the
	// v1.8.9 expression, and this release must not move it.
	CHECK(fabs(combinedLum(15.0, 0.0, 4.0 / 15.0) - 0.0833333) < LIGHT_TOLERANCE,
	      "sky=15 block=0 dayLevel=4/15 -> %.7f, expected 0.0833333 (v1.8.9's measured midnight "
	      "value, unchanged)", combinedLum(15.0, 0.0, 4.0 / 15.0));

	// Pitch dark except for a torch at block=14: 1.5 * curve(14/15) = 1.16667 uncurved, clamped
	// to 1.0. Also proves the clamp actually engages: the unclamped sum must exceed 1.0, or this
	// check could not tell "clamped to 1" apart from "happened to equal 1".
	const double dark_unclamped = unclampedLum(0.0, 14.0, 1.0);
	const double dark_clamped   = combinedLum(0.0, 14.0, 1.0);
	CHECK(dark_unclamped > 1.0, "sky=0 block=14: unclamped sum is %.7f, expected > 1.0 -- if it "
	      "is not, the clamp check below cannot distinguish clamping from coincidence",
	      dark_unclamped);
	CHECK(fabs(dark_unclamped - 1.166667) < LIGHT_TOLERANCE,
	      "sky=0 block=14: unclamped sum is %.7f, expected 1.166667 (1.5 * curve(14/15) = "
	      "1.5 * 0.777778)", dark_unclamped);
	CHECK(fabs(dark_clamped - 1.0) < LIGHT_TOLERANCE,
	      "sky=0 block=14: clamped result is %.7f, expected exactly 1.0 -- the raw sum (%.7f) "
	      "must not leak through unclamped, or a torch at point-blank range washes the face to "
	      "pure white past white", dark_clamped, dark_unclamped);

	// A mid falloff, and the block=0 floor stated as its own exact check.
	CHECK(fabs(combinedLum(0.0, 7.0, 1.0) - (1.5 * curveOf(7.0 / 15.0))) < LIGHT_TOLERANCE,
	      "sky=0 block=7 -> %.7f, expected 1.5*curve(7/15) = %.7f",
	      combinedLum(0.0, 7.0, 1.0), 1.5 * curveOf(7.0 / 15.0));
	CHECK(combinedLum(0.0, 0.0, 1.0) == 0.0,
	      "sky=0 block=0 -> %.7f, expected exactly 0.0 (no sky, no emitter, no light at all)",
	      combinedLum(0.0, 0.0, 1.0));

	// ADDITIVE != MAX. Under max(), a torch could never brighten a spot past whatever the sky
	// already gave it -- that is the exact bug v1.8.10 fixes, so the test must go red if anyone
	// puts max() back. sky=8 gives a bigger sky term than block=8 gives a block term, so max()
	// would answer with the sky term alone; the additive combine must answer higher than either
	// one taken alone.
	{
		const double sky_term   = curveOf(1.0 * (8.0 / 15.0));         // curve(dayLevel*skyNorm)
		const double block_term = curveOf(8.0 / 15.0) * CURVE_BLOCK_MUL; // 1.5*curve(blockNorm)
		const double max_answer = sky_term > block_term ? sky_term : block_term;
		const double additive   = combinedLum(8.0, 8.0, 1.0);

		CHECK(absd(sky_term - block_term) > LIGHT_TOLERANCE,
		      "control: sky=8's curved term (%.7f) and block=8's weighted curved term (%.7f) "
		      "must actually differ, or this arm cannot tell add from max", sky_term, block_term);
		CHECK(absd(additive - max_answer) > 0.1,
		      "sky=8 block=8 dayLevel=1.0: additive gives %.7f, max() of the two terms would "
		      "give %.7f -- these must differ by a wide margin, or a shader reverted to max() "
		      "would not be caught", additive, max_answer);
		CHECK(fabs(additive - (sky_term + block_term)) < LIGHT_TOLERANCE,
		      "sky=8 block=8 dayLevel=1.0: combinedLum gives %.7f, expected the SUM %.7f",
		      additive, sky_term + block_term);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== light combine test ==");

	testShaderTextCombine();
	testArithmeticCombine();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
