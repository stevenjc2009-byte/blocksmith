// Host regression guard for main.c's gpuWaitPrevFrame() — the fix for the hardware freeze
// steve's New 3DS hit at render distance 5, and the specific thing this file exists to stop
// from silently regressing back.
//
// ── the bug this pins ─────────────────────────────────────────────────────────────────────
//
// gpuWaitPrevFrame() exists so the CPU never overwrites last frame's chunk vertex buffers
// while the GPU is still reading them: genFollow() reassigns mesh slots (chunkRenderRelease-
// Column hands a live slot to a different chunk) and the mesh drains memcpy new geometry into
// them. In the shipped build (BS_GPU_TESTS=0) its body used to be a bare `C3D_FrameSync();`,
// on the strength of c3d/renderqueue.h's doc comment "Waits for the GPU to finish rendering."
// Disassembling the actually-linked libcitro3d.a (1.7.1-2) shows that comment is wrong:
// C3D_FrameSync only loops on gspWaitForAnyEvent() until a vblank tick lands on both screens —
// there is no bl to gxCmdQueueWait and no GPU-busy check of any kind. A vblank arrives every
// ~16.7 ms whether or not the GPU has finished, so the two are unrelated events, and the
// function only narrowed the race from "the rest of the frame" to "until the next vblank" —
// harmless while a frame finishes inside one vblank, and radius 5 does not.
//
// The fix is C3D_FrameBegin(C3D_FRAME_SYNCDRAW): the same vblank wait FOLLOWED by the real,
// unconditional gxCmdQueueWait(-1) — also measured from the disassembly. See main.c's own
// comment on gpuWaitPrevFrame() for the full account, including why calling FrameBegin this
// early is safe (nothing between the call and the draw block emits a GPU command, so setting
// citro3d's inFrame flag early cannot capture a command into the wrong frame). That safety
// argument is exactly the third property this file checks, because it is also exactly the
// property a careless future edit is most likely to break without anyone noticing — an added
// C3D_/GX_/gsp* call in that span would compile clean and pass every other test here.
//
// ── what is checked, and why grep alone would not catch a regression ────────────────────────
//
//   1. testShippedBranchRealWait — the #else branch (BS_GPU_TESTS=0) of gpuWaitPrevFrame()
//      calls C3D_FrameBegin(C3D_FRAME_SYNCDRAW) and makes no bare C3D_FrameSync() call. This
//      is parsed out of the FUNCTION BODY, brace-matched, not grepped across the whole file —
//      C3D_FrameSync() legitimately appears elsewhere, in the BS_DRAW_PROBE diagnostic path
//      (main.c, further down), and a whole-file grep for it would flag that as a false
//      regression, or worse, average it against a real one and stay green.
//
//   2. testWaitPrecedesMutators — the gpuWaitPrevFrame(); call site sits at a lower line
//      number than the genFollow( and genDrainMesh( calls in the main loop. Ordering, not
//      presence: the fix does nothing if the wait is ever moved below the code it exists to
//      guard. Both functions are also DEFINED earlier in main.c, so the needle for each is
//      matched against every occurrence and the definition line is excluded, rather than
//      taking "first match" and silently checking the wrong site.
//
//   3. testNothingEmitsBetween — no line between the call and the frame's draw block (anchored
//      on watchdogPhase(WD_PHASE_DRAW), main.c's own anchor for "the draw begins here") calls
//      any C3D_*/GX_*/gsp* function. This is the invariant the "safe to call FrameBegin early"
//      argument above rests on, checked directly rather than trusted from the comment. Comment
//      lines are skipped — the span is full of prose that mentions these names while
//      explaining exactly this property, which would make a naive scan permanently red.
//
// A parse that finds nothing must not read as "nothing wrong": every lookup below that can
// fail (file missing, needle absent, an ambiguous count) is itself a loud, named failure, same
// rule and same reason as world/water_alpha_test.c and world/atlas_uv_shader_test.c, which
// this file's line-reading helpers are copied from.
//
// The target file defaults to source/main.c (relative to the repo root, run from tools/
// run_host_tests.sh) but argv[1] overrides it — so a red-arm run can point this binary at a
// sabotaged scratch copy without ever touching the real main.c.
//
// The __3DS__ guard below is load-bearing, not tidy: the Makefile globs every .c under
// source/world into the console build, so without it this file's main() collides with
// source/main.c's and the link dies with "multiple definition of `main'". Found exactly that
// way for source/world/genretry_test.c; see the note at the top of source/app/options_test.c.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_C_PATH "source/main.c"

static int  s_checks;
static int  s_fails;

// Reports every failure, not just the first, so a red run is readable case by case and a
// check that stayed green under sabotage can be spotted.
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

// ── Reading main.c as lines ──────────────────────────────────────────────────────────────

// main.c runs to ~5800 lines; sized with headroom rather than to the measured count so a
// normal amount of future growth does not need this file touched.
#define MAX_LINES 8192
#define MAX_LINE  512

static char s_lines[MAX_LINES][MAX_LINE];
static int  s_nlines;

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

// The LAST line containing `needle`, other than `exclude_line`. Both genFollow( and
// genDrainMesh( appear once as a definition and once (or more) as a call, and the definition
// always comes first in the file — so excluding the definition's line and taking the highest
// remaining match line lands on the call the main loop actually makes, not the definition, and
// not an earlier call from outside the main loop (genDrainMesh( is also called once from the
// loading loop, well above the main loop, so it can never be the LAST match).
static int lastLineWithExcluding(const char* needle, int exclude_line)
{
	int found = -1;
	for (int i = 0; i < s_nlines; i++) {
		if (i + 1 == exclude_line) continue;
		if (strstr(s_lines[i], needle)) found = i + 1;
	}
	return found;
}

// squash() trims leading whitespace, so a comment line reads "//..." from index 0 regardless
// of how it was indented in the source.
static bool isCommentLine(int line1)
{
	const char* s = lineText(line1);
	return s[0] == '/' && s[1] == '/';
}

// Net effect on brace depth of one line, stopping at the first "//" so a comment can never
// desync the count. (None of gpuWaitPrevFrame()'s comments contain a brace, but the guard
// costs nothing and keeps this independent of that happening to stay true.)
static int braceDelta(const char* s)
{
	int depth = 0;
	for (const char* p = s; *p; p++) {
		if (p[0] == '/' && p[1] == '/') break;
		if (*p == '{') depth++;
		else if (*p == '}') depth--;
	}
	return depth;
}

// ── Locating gpuWaitPrevFrame()'s shipped (#else) branch ────────────────────────────────────

#define GPUWAIT_DEF_NEEDLE "static void gpuWaitPrevFrame(void)"

// Brace-matches the function body rather than restating its line numbers, so this keeps
// working if anything above gpuWaitPrevFrame() in main.c grows or shrinks. Returns the line
// numbers of the `#else` and the `#endif` that closes it; the shipped branch is the (exclusive)
// span between them.
static bool findShippedBranch(int* else_line_out, int* endif_line_out, char* err, size_t errsz)
{
	int def_line = -1;
	if (countLines(GPUWAIT_DEF_NEEDLE, &def_line) != 1) {
		snprintf(err, errsz, "expected exactly one `%s` (found %d)",
		         GPUWAIT_DEF_NEEDLE, countLines(GPUWAIT_DEF_NEEDLE, NULL));
		return false;
	}

	int open_line = -1;
	for (int i = def_line; i <= def_line + 3 && i <= s_nlines; i++)
		if (lineHas(i, "{")) { open_line = i; break; }
	if (open_line < 0) {
		snprintf(err, errsz, "no `{` within 3 lines of the signature on L%d", def_line);
		return false;
	}

	int depth = 0, close_line = -1;
	for (int i = open_line; i <= s_nlines; i++) {
		depth += braceDelta(lineText(i));
		if (depth == 0) { close_line = i; break; }
	}
	if (close_line < 0) {
		snprintf(err, errsz, "the `{` on L%d never balances before EOF", open_line);
		return false;
	}

	int else_line = -1;
	for (int i = open_line + 1; i < close_line; i++)
		if (lineHas(i, "#else")) { else_line = i; break; }
	if (else_line < 0) {
		snprintf(err, errsz, "no `#else` inside gpuWaitPrevFrame() (L%d..L%d) — it is expected "
		         "to branch on BS_GPU_TESTS", open_line, close_line);
		return false;
	}

	int endif_line = -1;
	for (int i = else_line + 1; i < close_line; i++)
		if (lineHas(i, "#endif")) { endif_line = i; break; }
	if (endif_line < 0) {
		snprintf(err, errsz, "no `#endif` between the `#else` on L%d and the closing brace on "
		         "L%d", else_line, close_line);
		return false;
	}

	*else_line_out  = else_line;
	*endif_line_out = endif_line;
	return true;
}

// ── 1. The shipped branch performs the real queue wait ──────────────────────────────────────

#define FRAMEBEGIN_SYNCDRAW_NEEDLE "C3D_FrameBegin(C3D_FRAME_SYNCDRAW)"
#define FRAMESYNC_BARE_NEEDLE      "C3D_FrameSync()"

static void testShippedBranchRealWait(void)
{
	puts("framesync guard: the shipped (BS_GPU_TESTS=0) branch calls "
	     "C3D_FrameBegin(C3D_FRAME_SYNCDRAW), not the vblank-only C3D_FrameSync()");

	int else_line = -1, endif_line = -1;
	char err[220] = { 0 };
	const bool found = findShippedBranch(&else_line, &endif_line, err, sizeof err);
	CHECK(found, "found gpuWaitPrevFrame()'s #else branch%s%s", found ? "" : ": ", err);
	if (!found) return;

	printf("  ...#else branch spans L%d..L%d\n", else_line, endif_line);

	int begin_calls = 0, begin_line = -1, bare_calls = 0, bare_line = -1;
	for (int i = else_line + 1; i < endif_line; i++) {
		if (isCommentLine(i)) continue;
		if (strstr(lineText(i), FRAMEBEGIN_SYNCDRAW_NEEDLE)) {
			begin_calls++;
			if (begin_line < 0) begin_line = i;
		}
		if (strstr(lineText(i), FRAMESYNC_BARE_NEEDLE)) {
			bare_calls++;
			if (bare_line < 0) bare_line = i;
		}
	}

	CHECK(begin_calls == 1,
	      "shipped branch calls C3D_FrameBegin(C3D_FRAME_SYNCDRAW) exactly once (found %d) "
	      "— that call is what actually reaches gxCmdQueueWait(-1); without it the CPU can "
	      "overwrite last frame's chunk vertex buffers while the GPU is still reading them",
	      begin_calls);
	if (begin_calls != 1)
		printf("  ...(diagnostic) first/only match at L%d: '%s'\n",
		       begin_line, begin_line > 0 ? lineText(begin_line) : "<none>");

	CHECK(bare_calls == 0,
	      "shipped branch makes no bare C3D_FrameSync() call (found %d) — C3D_FrameSync only "
	      "loops on gspWaitForAnyEvent() for a vblank tick, never calls gxCmdQueueWait, and "
	      "does not guarantee the GPU has finished reading last frame's vertex buffers",
	      bare_calls);
	if (bare_calls != 0)
		printf("  ...(diagnostic) first match at L%d: '%s'\n", bare_line, lineText(bare_line));
}

// ── 2. The wait precedes the mutators ────────────────────────────────────────────────────────

#define GPUWAIT_CALL_NEEDLE     "gpuWaitPrevFrame();"
#define GENFOLLOW_DEF_NEEDLE    "static void genFollow(float x, float z)"
#define GENFOLLOW_CALL_NEEDLE   "genFollow("
#define GENDRAIN_DEF_NEEDLE     "static int genDrainMesh(float budget_ms, int max_chunks)"
#define GENDRAIN_CALL_NEEDLE    "genDrainMesh("
#define CHUNKDRAIN_CALL_NEEDLE  "chunkRenderDrainDirty("
#define FIX_COMMENT_NEEDLE      "THE fix for the hardware freeze"

static void testWaitPrecedesMutators(void)
{
	puts("framesync guard: gpuWaitPrevFrame() runs before genFollow(), genDrainMesh(), and "
	     "chunkRenderDrainDirty() reassign/memcpy into mesh slots the GPU may still be reading");

	// `gpuWaitPrevFrame();` (a call, with empty parens) cannot match the definition's text
	// `gpuWaitPrevFrame(void)`, so this needle is unambiguous by construction — unlike the two
	// below, which need the definition excluded explicitly.
	int wait_line = -1;
	CHECK(countLines(GPUWAIT_CALL_NEEDLE, &wait_line) == 1,
	      "gpuWaitPrevFrame(); called exactly once (found %d)",
	      countLines(GPUWAIT_CALL_NEEDLE, NULL));
	if (wait_line < 0) return;

	// Control: pins that the call site found above is still the one the header comment calls
	// out as THE fix, not some other call that happens to be first/last/only. Searches for the
	// NEAREST preceding occurrence rather than the first in the file, because
	// gpuWaitPrevFrame()'s own header comment (main.c ~L3217) also quotes this exact phrase
	// while narrating the function's history — only the occurrence right above the call site
	// (main.c ~L4870) is the one this check cares about. If this drifts — the comment moved, or
	// a second gpuWaitPrevFrame(); call appeared elsewhere — the property below would be
	// checking the wrong line without saying so.
	int fix_comment_line = -1;
	const int fix_comments_total = countLines(FIX_COMMENT_NEEDLE, NULL);
	for (int i = wait_line - 1; i >= 1 && wait_line - i < 20; i--)
		if (lineHas(i, FIX_COMMENT_NEEDLE)) { fix_comment_line = i; break; }
	CHECK(fix_comment_line > 0,
	      "the call site (L%d) is preceded within 20 lines by a 'THE fix for the hardware "
	      "freeze' comment (found %d such comment(s) total in the file; nearest preceding one "
	      "at L%d)", wait_line, fix_comments_total, fix_comment_line);

	int follow_def = -1;
	CHECK(countLines(GENFOLLOW_DEF_NEEDLE, &follow_def) == 1,
	      "genFollow()'s definition found exactly once (found %d), so its call site can be "
	      "told apart from it", countLines(GENFOLLOW_DEF_NEEDLE, NULL));
	const int follow_call = (follow_def < 0) ? -1
	                       : lastLineWithExcluding(GENFOLLOW_CALL_NEEDLE, follow_def);
	CHECK(follow_call > 0,
	      "found a genFollow( call site distinct from its definition on L%d", follow_def);

	int drain_def = -1;
	CHECK(countLines(GENDRAIN_DEF_NEEDLE, &drain_def) == 1,
	      "genDrainMesh()'s definition found exactly once (found %d), so its call site can be "
	      "told apart from it", countLines(GENDRAIN_DEF_NEEDLE, NULL));
	const int drain_call = (drain_def < 0) ? -1
	                      : lastLineWithExcluding(GENDRAIN_CALL_NEEDLE, drain_def);
	CHECK(drain_call > 0,
	      "found a genDrainMesh( call site distinct from its definition on L%d", drain_def);

	// chunkRenderDrainDirty( has no definition in main.c to exclude — it's declared in
	// chunk_render.h and defined in chunk_render.c — but it IS called many times during the
	// loading screen (main.c ~L512-605), well above the main loop, for the same reason
	// genDrainMesh's own loading-loop call (above) can never be mistaken for its main-loop
	// call: the loading-loop calls are not the LAST occurrence in the file, so passing -1 (a
	// line number no real match can equal) as the "exclude" argument and taking the last match
	// lands unambiguously on the main loop's own call.
	const int chunkdrain_call = lastLineWithExcluding(CHUNKDRAIN_CALL_NEEDLE, -1);
	CHECK(chunkdrain_call > 0,
	      "found a chunkRenderDrainDirty( call site (last occurrence in the file, expected to "
	      "be the main loop's own call)");

	if (follow_call > 0)
		CHECK(wait_line < follow_call,
		      "gpuWaitPrevFrame() (L%d) runs before genFollow()'s call (L%d) — genFollow "
		      "reaches chunkRenderReleaseColumn, which hands a live mesh slot to a different "
		      "chunk", wait_line, follow_call);

	if (drain_call > 0)
		CHECK(wait_line < drain_call,
		      "gpuWaitPrevFrame() (L%d) runs before genDrainMesh()'s call (L%d) — that drain "
		      "memcpy's freshly built geometry into vertex buffers the GPU may still be "
		      "reading", wait_line, drain_call);

	if (chunkdrain_call > 0)
		CHECK(wait_line < chunkdrain_call,
		      "gpuWaitPrevFrame() (L%d) runs before chunkRenderDrainDirty()'s call (L%d) — that "
		      "drain reaches chunkRenderBuild() (chunk_render.c), which memcpy's freshly built "
		      "geometry into vertex buffers the GPU may still be reading, exactly like "
		      "genFollow and genDrainMesh", wait_line, chunkdrain_call);
}

// ── 3. Nothing emits GPU commands between the wait and the draw ─────────────────────────────

#define DRAW_ANCHOR_NEEDLE "watchdogPhase(WD_PHASE_DRAW);"

// True if `s` contains a call to any C3D_*, GX_*, or gsp* function — the prefix immediately
// followed by a run of identifier characters and then `(`, e.g. "C3D_FrameBegin(",
// "GX_SetTextureCopy(", "gspWaitForVBlank(". Comment lines are filtered by the caller, not
// here, so this only ever looks at code.
static bool isGpuCallLine(const char* s)
{
	static const char* prefixes[] = { "C3D_", "GX_", "gsp" };
	for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
		const char* prefix = prefixes[p];
		const size_t plen = strlen(prefix);
		const char* q = s;
		while ((q = strstr(q, prefix)) != NULL) {
			const char* r = q + plen;
			while ((*r >= 'a' && *r <= 'z') || (*r >= 'A' && *r <= 'Z') ||
			       (*r >= '0' && *r <= '9') || *r == '_')
				r++;
			if (*r == '(') return true;
			q += 1;
		}
	}
	return false;
}

static void testNothingEmitsBetween(void)
{
	puts("framesync guard: nothing between gpuWaitPrevFrame()'s call and the draw block issues "
	     "a C3D_*/GX_*/gsp* call — that is what makes calling C3D_FrameBegin this early safe");

	int wait_line = -1;
	CHECK(countLines(GPUWAIT_CALL_NEEDLE, &wait_line) == 1,
	      "gpuWaitPrevFrame(); call site found exactly once (found %d)",
	      countLines(GPUWAIT_CALL_NEEDLE, NULL));
	if (wait_line < 0) return;

	int draw_line = -1;
	CHECK(countLines(DRAW_ANCHOR_NEEDLE, &draw_line) == 1,
	      "`%s` anchor found exactly once, marking where the draw block begins (found %d)",
	      DRAW_ANCHOR_NEEDLE, countLines(DRAW_ANCHOR_NEEDLE, NULL));
	if (draw_line < 0) return;

	CHECK(draw_line > wait_line,
	      "the draw-block anchor (L%d) comes after gpuWaitPrevFrame()'s call (L%d), so there "
	      "is a real span between them to check", draw_line, wait_line);
	if (draw_line <= wait_line) return;

	int violations = 0, first_violation = -1;
	for (int i = wait_line + 1; i < draw_line; i++) {
		if (isCommentLine(i)) continue;
		if (isGpuCallLine(lineText(i))) {
			violations++;
			if (first_violation < 0) first_violation = i;
		}
	}

	CHECK(violations == 0,
	      "no C3D_*/GX_*/gsp* call between L%d and L%d (found %d)",
	      wait_line, draw_line, violations);
	if (violations != 0)
		printf("  ...(diagnostic) first violation at L%d: '%s'\n",
		       first_violation, lineText(first_violation));
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* path = (argc > 1) ? argv[1] : MAIN_C_PATH;

	puts("== framesync guard test ==");
	printf("  ...target: %s\n", path);

	const int n = loadLines(path);
	CHECK(n >= 0, "%s opens and reads", path);
	if (n < 0) {
		printf("\nFAIL %d checks, %d failed\n", s_checks, s_fails);
		return 1;
	}
	// Loud, not silent: a truncated read would make every check below pass or fail against a
	// partial picture of the file rather than the real one.
	CHECK(n < MAX_LINES,
	      "%s is %d lines, under the %d-line MAX_LINES cap (a file at or over the cap would "
	      "have been silently truncated)", path, n, MAX_LINES);

	testShippedBranchRealWait();
	testWaitPrecedesMutators();
	testNothingEmitsBetween();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif   // !__3DS__
