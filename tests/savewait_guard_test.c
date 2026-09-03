// Source-scan guard for the v1.8.16 FRZ-FIX: asserts that source/app/worker.c actually USES
// source/app/savewait.c instead of carrying its own copy of the loop.
//
// ── why this file exists at all ───────────────────────────────────────────────────────────
//
// tests/savewait_test.c links the real savewait.c and proves the wait terminates inside its
// budget. That proof is worth nothing if worker.c still spins in a `for (;;)` of its own —
// savewait.c would simply be dead code with a passing test attached to it, which is the
// test-links-nothing failure this project has already been bitten by. So this file reads
// source/app/worker.c AS TEXT and asserts three things:
//
//   1. workerSubmitSave() and workerFlushSaves() each call saveWaitForRoom(.
//   2. Neither of them contains a bare `for (;;)` / `for(;;)` between its own braces.
//   3. workerSaveIdle() — the console half savewait.c calls back into — calls aptMainLoop(),
//      which is the half that keeps HOME responding while the main thread is stalled. That is
//      the entire reported symptom: "it has froze the entire console".
//
// ── WHAT THIS CHECK CANNOT DO, stated so nobody trusts it further than it goes ────────────
//
// This is a TEXT SCAN. It is modelled on source/world/framesync_guard_test.c and inherits that
// file's limits exactly:
//
//   * It cannot follow calls. workerSubmitSave() could call some new helper that itself spins
//     in a `for (;;)`, and every check below would stay green. All it establishes is that the
//     two named functions do not spin in their OWN bodies and do name saveWaitForRoom.
//   * It cannot tell reachable code from unreachable. A saveWaitForRoom( call inside an
//     `#if 0` would be counted.
//   * It knows nothing about whether the code compiles, links, or runs. It never builds
//     worker.c — worker.c is libctru all the way down and cannot be built on a host at all.
//   * It proves nothing whatsoever about behaviour on hardware.
//
// Comment lines are skipped when scanning for the needles, because worker.c's own comments
// describe the `for (;;)` that was removed — a naive scan would be permanently red on the
// prose explaining the fix.
//
// A parse that finds nothing must not read as "nothing wrong": every lookup below that can
// fail (file missing, signature absent, an unbalanced brace) is itself a loud, named failure.
//
// The target file defaults to source/app/worker.c (relative to the repo root, run from
// tools/run_host_tests.sh) but argv[1] overrides it, so a red-arm run can point this binary at
// a sabotaged scratch copy without ever touching the real worker.c.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_C_PATH "source/app/worker.c"

static int s_checks;
static int s_fails;

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

// ── Reading worker.c as lines ─────────────────────────────────────────────────────────────
//
// Same helpers as source/world/framesync_guard_test.c, deliberately: squash() collapses runs
// of whitespace so `for (;;)` and `for(;;)` and a tab-indented one all reduce to comparable
// text, and it trims the leading indent so a comment line starts with "//" at index 0.

#define MAX_LINES 4096
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

// Returns the number of lines read, or -1 if the file could not be opened. The caller turns -1
// into a failure; it is never treated as "no lines, therefore nothing to complain about".
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

static const char* lineText(int line1)
{
	if (line1 < 1 || line1 > s_nlines) return "<past the end of the file>";
	return s_lines[line1 - 1];
}

static bool isCommentLine(int line1)
{
	const char* s = lineText(line1);
	return s[0] == '/' && s[1] == '/';
}

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

// Net effect on brace depth of one line, stopping at the first "//" so a comment can never
// desync the count. worker.c uses // comments throughout and no /* */ blocks; if that ever
// changes, this needs to change with it, which is why the assumption is written down here.
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

// Brace-matches one function body from its exact signature line. Returns false with a reason
// rather than a silent -1, so a renamed function is a named failure and not a skipped check.
static bool findBody(const char* sig, int* open_out, int* close_out, char* err, size_t errsz)
{
	int def_line = -1;
	const int n = countLines(sig, &def_line);
	if (n != 1) {
		snprintf(err, errsz, "expected exactly one `%s` (found %d)", sig, n);
		return false;
	}

	int open_line = -1;
	for (int i = def_line; i <= def_line + 3 && i <= s_nlines; i++)
		if (strchr(lineText(i), '{')) { open_line = i; break; }
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

	*open_out  = open_line;
	*close_out = close_line;
	return true;
}

// How many non-comment lines in [open, close] contain `needle`.
static int countInBody(int open_line, int close_line, const char* needle, int* first_line)
{
	int n = 0;
	if (first_line) *first_line = -1;
	for (int i = open_line; i <= close_line; i++) {
		if (isCommentLine(i)) continue;
		if (!strstr(lineText(i), needle)) continue;
		if (first_line && *first_line < 0) *first_line = i;
		n++;
	}
	return n;
}

// ── The three properties ──────────────────────────────────────────────────────────────────

// Both spellings, because squash() does not normalise `for (;;)` to `for(;;)` — it only
// collapses runs of whitespace, and there is no space to collapse in `for(;;)`.
#define SPIN_A "for (;;)"
#define SPIN_B "for(;;)"
#define WAIT_CALL "saveWaitForRoom("

static void checkOneWaiter(const char* sig, const char* what)
{
	int open_line = -1, close_line = -1;
	char err[220] = { 0 };
	const bool found = findBody(sig, &open_line, &close_line, err, sizeof err);
	CHECK(found, "%s: found the body of `%s`%s%s", what, sig, found ? "" : " -- ", err);
	if (!found) return;

	printf("  ...%s spans L%d..L%d\n", what, open_line, close_line);

	int wait_line = -1;
	const int waits = countInBody(open_line, close_line, WAIT_CALL, &wait_line);
	CHECK(waits >= 1,
	      "%s calls %s (found %d) -- without this the extracted, tested loop in "
	      "source/app/savewait.c is dead code and tests/savewait_test.c proves nothing about "
	      "what the console runs",
	      what, WAIT_CALL, waits);
	if (waits >= 1) printf("  ...first call at L%d: '%s'\n", wait_line, lineText(wait_line));

	int spin_line_a = -1, spin_line_b = -1;
	const int spins = countInBody(open_line, close_line, SPIN_A, &spin_line_a) +
	                  countInBody(open_line, close_line, SPIN_B, &spin_line_b);
	CHECK(spins == 0,
	      "%s contains no bare `for (;;)` of its own (found %d) -- that unbounded spin on the "
	      "MAIN thread, with no deadline and no aptMainLoop(), is the mechanism behind the "
	      "\"it has froze the entire console\" report",
	      what, spins);
	if (spins != 0) {
		const int first = (spin_line_a > 0) ? spin_line_a : spin_line_b;
		printf("  ...(diagnostic) first spin at L%d: '%s'\n", first, lineText(first));
	}
}

#define SUBMIT_SIG "bool workerSubmitSave(const Column* col)"
#define FLUSH_SIG  "void workerFlushSaves(void)"
#define IDLE_SIG   "static void workerSaveIdle(void* ud, bool pump)"
#define APT_NEEDLE "aptMainLoop()"

static void testIdlePumpsApt(void)
{
	puts("savewait guard: workerSaveIdle() -- savewait.c's console-side callback -- services "
	     "APT, which is what keeps HOME alive during a stall");

	int open_line = -1, close_line = -1;
	char err[220] = { 0 };
	const bool found = findBody(IDLE_SIG, &open_line, &close_line, err, sizeof err);
	CHECK(found, "found the body of `%s`%s%s", IDLE_SIG, found ? "" : " -- ", err);
	if (!found) return;

	printf("  ...workerSaveIdle spans L%d..L%d\n", open_line, close_line);

	int apt_line = -1;
	const int apts = countInBody(open_line, close_line, APT_NEEDLE, &apt_line);
	CHECK(apts >= 1,
	      "workerSaveIdle calls %s (found %d) -- with no APT service the console stops "
	      "responding to HOME during the wait, which is the symptom steve reported twice from "
	      "hardware",
	      APT_NEEDLE, apts);
	if (apts >= 1) printf("  ...at L%d: '%s'\n", apt_line, lineText(apt_line));
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* path = (argc > 1) ? argv[1] : WORKER_C_PATH;

	puts("== savewait guard test ==");
	printf("  ...target: %s\n", path);
	puts("  ...NOTE: this is a TEXT SCAN. It cannot follow calls, cannot tell reachable code");
	puts("  ...from dead code, and proves nothing about hardware. See the top of this file.");

	const int n = loadLines(path);
	CHECK(n >= 0, "%s opens and reads", path);
	if (n < 0) {
		printf("\nFAIL %d checks, %d failed\n", s_checks, s_fails);
		return 1;
	}
	CHECK(n < MAX_LINES,
	      "%s is %d lines, under the %d-line MAX_LINES cap (a file at or over the cap would "
	      "have been silently truncated)", path, n, MAX_LINES);

	checkOneWaiter(SUBMIT_SIG, "workerSubmitSave");
	checkOneWaiter(FLUSH_SIG,  "workerFlushSaves");
	testIdlePumpsApt();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}
