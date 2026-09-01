// Host test for source/app/memprobe.c — the boot memory probe's arithmetic and formatting.
//
// Links the REAL source file. memprobe.c is <3ds.h>-free on purpose: main.c takes the two
// readings from libctru and passes them in, so everything that can be wrong about the numbers
// — the subtraction, the sign, the overflow behaviour, the truncation contract — is reachable
// from here and none of it needs a console.
//
// The two things this exists to pin, both of which were live bugs in the first draft:
//
//   * a stage that FREES memory must report a negative cost. gpuTestPreflight releases its
//     512 KB scratch, so `after > before` is a real case, and an unsigned subtraction would
//     print 4,294,443,776 bytes of cost and read as a catastrophe rather than a release.
//   * memProbeFormat must never truncate at MEMPROBE_REPORT_MAX. The first version of that
//     cap budgeted 80 bytes a line against a real worst line of 94 — and the obvious check
//     for it, "does the whole report still fit", stayed GREEN under that mistake, because the
//     cap carries four spare lines of slack that absorbed the shortfall. So the budget is
//     pinned by measuring the longest line the formatter actually emits instead. See
//     testNoLineExceedsTheLineBudget, which is the one that goes red.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/memprobe.h"

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

// ── The subtraction ───────────────────────────────────────────────────────────────────────

static void testCostIsTheDropBetweenAdjacentMarks(void)
{
	MemProbe p;
	memProbeReset(&p);
	CHECK(p.count == 0);
	CHECK(p.overflowed == false);

	CHECK(memProbeMark(&p, "boot",     30000000, 6000000, 1000000));
	CHECK(memProbeMark(&p, "meshpool", 20800384, 6000000, 1000000));
	CHECK(p.count == 2);

	// The mesh pool's own arithmetic says 9,199,616 bytes. This is the check that would go
	// red on a console if that arithmetic were wrong, which is the entire point of the probe.
	CHECK(memProbeLinearCost(&p, 1) == 9199616);
	CHECK(memProbeVramCost(&p, 1) == 0);

	// Stage 0 is the baseline and has no predecessor, so it has no cost. Not "a cost of
	// zero" — the formatter prints "(baseline)" there rather than a number for that reason.
	CHECK(memProbeLinearCost(&p, 0) == 0);

	// Out of range in both directions, rather than reading off the end of the table.
	CHECK(memProbeLinearCost(&p, 2) == 0);
	CHECK(memProbeLinearCost(&p, -1) == 0);
	CHECK(memProbeVramCost(&p, 99) == 0);
}

static void testAStageThatFreesMemoryReportsNegative(void)
{
	MemProbe p;
	memProbeReset(&p);
	memProbeMark(&p, "gputest.begin", 20000000, 6000000, 3000000);
	memProbeMark(&p, "gputest.end",   20524288, 6000000, 2000000);  // released 512 KB linear
	                                                              // and 1 MB of heap

	// The bug this pins: (long)(before - after) on unsigned operands wraps BEFORE the cast,
	// so the widening has to happen first. 20000000 - 20524288 is -524288, not 4294443008.
	CHECK(memProbeLinearCost(&p, 1) == -524288);
	CHECK(memProbeLinearTotal(&p) == -524288);
}

static void testTotalSpansFirstToLast(void)
{
	MemProbe p;
	memProbeReset(&p);
	CHECK(memProbeLinearTotal(&p) == 0);   // no marks at all

	memProbeMark(&p, "a", 30000000, 6000000, 1000000);
	CHECK(memProbeLinearTotal(&p) == 0);   // one mark is a baseline, not a span

	memProbeMark(&p, "b", 25000000, 5900000, 1500000);
	memProbeMark(&p, "c", 21000000, 5800000, 2200000);

	// The total is the span, NOT the sum of the per-stage costs — they agree here, and they
	// must, but the total is defined against the endpoints so a dropped middle mark cannot
	// silently change it.
	CHECK(memProbeLinearTotal(&p) == 9000000);
	CHECK(memProbeVramTotal(&p) == 200000);
	CHECK(memProbeLinearCost(&p, 1) + memProbeLinearCost(&p, 2) == memProbeLinearTotal(&p));
}

// The reading that counts the other way.
//
// `linear` and `vram` are FREE bytes and fall as memory is consumed; `heap` is bytes IN USE
// and rises. Both cost accessors normalise to positive-means-consumed, so a caller never has
// to remember which is which — and this is the check that stops the two conventions being
// quietly swapped, which would report every heap allocation as a release.
//
// It matters for v1.8.5 specifically: net/blockdiff.h's 1.02 MB pending-edit store and the
// 512 KB gputest scratch are the two claims being reclaimed, and the first Azahar run of this
// probe showed linearSpaceFree() reading 0 until screenInit() and never moving for either of
// them. Whatever they cost, linear cannot see it. This column can.
static void testHeapIsUsedBytesNotFreeBytes(void)
{
	MemProbe p;
	memProbeReset(&p);
	memProbeMark(&p, "before", 30000000, 6000000, 2000000);
	memProbeMark(&p, "after",  30000000, 6000000, 3048576);   // claimed 1.02 MB

	CHECK(memProbeHeapCost(&p, 1) == 1048576);
	CHECK(memProbeHeapTotal(&p) == 1048576);

	// Linear did not move, and must not be made to look as though it did.
	CHECK(memProbeLinearCost(&p, 1) == 0);

	// And a release is negative here too, by the same widen-before-subtract rule.
	memProbeMark(&p, "freed", 30000000, 6000000, 2524288);
	CHECK(memProbeHeapCost(&p, 2) == -524288);
	CHECK(memProbeHeapTotal(&p) == 524288);

	CHECK(memProbeHeapCost(&p, 0) == 0);
	CHECK(memProbeHeapCost(&p, 99) == 0);
	CHECK(memProbeHeapCost(NULL, 1) == 0);
	CHECK(memProbeHeapTotal(NULL) == 0);
}

// ── Overflow ──────────────────────────────────────────────────────────────────────────────

static void testFullTableDropsMarksAndSaysSo(void)
{
	MemProbe p;
	memProbeReset(&p);

	for (int i = 0; i < MEMPROBE_MAX_STAGES; i++)
		CHECK(memProbeMark(&p, "fill", 30000000u - (unsigned)i * 1000u, 6000000, 0));

	CHECK(p.count == MEMPROBE_MAX_STAGES);
	CHECK(p.overflowed == false);

	// One past the end: refused, flagged, and — the part that matters — the stage that WAS
	// stored last is untouched, so the costs either side of the drop stay honest.
	const unsigned last_before = p.stage[MEMPROBE_MAX_STAGES - 1].linear;
	CHECK(memProbeMark(&p, "one too many", 1, 1, 1) == false);
	CHECK(p.overflowed == true);
	CHECK(p.count == MEMPROBE_MAX_STAGES);
	CHECK(p.stage[MEMPROBE_MAX_STAGES - 1].linear == last_before);
}

static void testNullIsRefusedNotCrashed(void)
{
	CHECK(memProbeMark(NULL, "x", 1, 1, 1) == false);
	CHECK(memProbeLinearCost(NULL, 1) == 0);
	CHECK(memProbeVramCost(NULL, 1) == 0);
	CHECK(memProbeLinearTotal(NULL) == 0);
	CHECK(memProbeVramTotal(NULL) == 0);
	memProbeReset(NULL);   // must not fault
	CHECK(true);

	MemProbe p;
	memProbeReset(&p);
	memProbeMark(&p, NULL, 1, 1, 1);
	CHECK(p.stage[0].name != NULL);   // a NULL name becomes "?", never a NULL deref in printf
}

// ── Formatting ────────────────────────────────────────────────────────────────────────────

// The names main.c passes. Kept here rather than in main.c because main.c cannot be host
// compiled, and MEMPROBE_NAME_MAX is a promise the formatter's line budget depends on.
static const char* const MAIN_STAGE_NAMES[] = {
	"boot",
	"net+networld",
	"screenInit+metrics",
	"gpuTestPreflight",
	"chunkRenderInit",
};

static void testEveryMainStageNameFitsTheNameCap(void)
{
	for (size_t i = 0; i < sizeof(MAIN_STAGE_NAMES) / sizeof(MAIN_STAGE_NAMES[0]); i++) {
		const size_t len = strlen(MAIN_STAGE_NAMES[i]);
		CHECK(len <= MEMPROBE_NAME_MAX);
		CHECK(len > 0);
	}
	// And main.c cannot ask for more marks than the table holds.
	CHECK(sizeof(MAIN_STAGE_NAMES) / sizeof(MAIN_STAGE_NAMES[0]) <= MEMPROBE_MAX_STAGES);
}

static void testAFullReportFitsTheDeclaredCap(void)
{
	MemProbe p;
	memProbeReset(&p);

	// Worst case on every axis at once: a full table, a name at exactly the pad width, ten-
	// digit readings, and alternating signs so every cost prints its sign character.
	static const char kWide[MEMPROBE_NAME_MAX + 1] = "aaaaaaaaaaaaaaaaaaaaaaaa";
	CHECK(strlen(kWide) == MEMPROBE_NAME_MAX);

	for (int i = 0; i < MEMPROBE_MAX_STAGES; i++) {
		const unsigned lin = (i % 2) ? 4294967295u : 1u;
		memProbeMark(&p, kWide, lin, (i % 2) ? 1u : 4294967295u, lin);
	}
	memProbeMark(&p, kWide, 0, 0, 0);   // refused; sets overflowed, widening the header line

	char buf[MEMPROBE_REPORT_MAX];
	const int n = memProbeFormat(&p, buf, sizeof(buf));

	// The check that goes red if MEMPROBE_LINE_MAX is ever trimmed back under the true worst
	// line. -1 here means main.c would write an empty memprobe.txt on a real console.
	CHECK(n > 0);
	CHECK((size_t)n < sizeof(buf));
	CHECK(buf[n] == '\0');
	CHECK(strstr(buf, "TABLE FULL") != NULL);
	CHECK(strstr(buf, "TOTAL") != NULL);

	// One line per stage, plus the header, plus the total.
	int newlines = 0;
	for (int i = 0; i < n; i++)
		if (buf[i] == '\n') newlines++;
	CHECK(newlines == MEMPROBE_MAX_STAGES + 2);
}

// The invariant MEMPROBE_REPORT_MAX is derived from, asserted directly.
//
// This exists because testAFullReportFitsTheDeclaredCap above does NOT pin it. With
// MEMPROBE_LINE_MAX put back to the 80 it was first written as, that test still passed —
// the "+ 4" spare lines in MEMPROBE_REPORT_MAX quietly absorbed a per-line budget that was
// 14 characters short of the real worst line, so the total fitting was luck. A check that
// stays green under the mistake it is supposed to catch is not a check.
static void testNoLineExceedsTheLineBudget(void)
{
	MemProbe p;
	memProbeReset(&p);

	// Same worst case as above: full table, name at the pad width, ten-digit readings both
	// ways round so every %u prints its widest, alternating so every cost prints a sign.
	static const char kWide[MEMPROBE_NAME_MAX + 1] = "aaaaaaaaaaaaaaaaaaaaaaaa";
	for (int i = 0; i < MEMPROBE_MAX_STAGES; i++)
		memProbeMark(&p, kWide, (i % 2) ? 4294967295u : 1u, (i % 2) ? 1u : 4294967295u,
	             (i % 2) ? 1u : 4294967295u);

	char buf[MEMPROBE_REPORT_MAX];
	const int n = memProbeFormat(&p, buf, sizeof(buf));
	CHECK(n > 0);
	if (n <= 0) return;

	int longest = 0;
	int cur     = 0;
	for (int i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			if (cur + 1 > longest) longest = cur + 1;   // count the newline itself
			cur = 0;
		} else {
			cur++;
		}
	}

	// Report the measurement, so the constant can be set from a number rather than from a
	// guess the next time a column is added to the line.
	printf("  longest formatted line: %d of %d budgeted\n", longest, MEMPROBE_LINE_MAX);
	CHECK(longest <= MEMPROBE_LINE_MAX);
	CHECK(longest > 0);
}

static void testTooSmallABufferIsRefusedNotTruncated(void)
{
	MemProbe p;
	memProbeReset(&p);
	memProbeMark(&p, "boot", 30000000, 6000000, 1000000);
	memProbeMark(&p, "chunkRenderInit", 20800384, 6000000, 1000000);

	char tiny[16];
	CHECK(memProbeFormat(&p, tiny, sizeof(tiny)) == -1);
	CHECK(memProbeFormat(&p, tiny, 0) == -1);
	CHECK(memProbeFormat(&p, NULL, 100) == -1);
	CHECK(memProbeFormat(NULL, tiny, sizeof(tiny)) == -1);
}

static void testTheReportCarriesTheRealNumbers(void)
{
	MemProbe p;
	memProbeReset(&p);
	memProbeMark(&p, "boot",            30000000, 6000000, 1000000);
	memProbeMark(&p, "chunkRenderInit", 20800384, 6000000, 1500000);

	char buf[MEMPROBE_REPORT_MAX];
	CHECK(memProbeFormat(&p, buf, sizeof(buf)) > 0);

	// A report that formats cleanly but omits the figure is the failure mode that makes a
	// probe worthless, so the numbers are asserted present, not just the shape.
	CHECK(strstr(buf, "30000000") != NULL);
	CHECK(strstr(buf, "20800384") != NULL);
	CHECK(strstr(buf, "+9199616") != NULL);
	CHECK(strstr(buf, "(baseline)") != NULL);
	CHECK(strstr(buf, "TABLE FULL") == NULL);
}

int main(void)
{
	testCostIsTheDropBetweenAdjacentMarks();
	testAStageThatFreesMemoryReportsNegative();
	testTotalSpansFirstToLast();
	testHeapIsUsedBytesNotFreeBytes();
	testFullTableDropsMarksAndSaysSo();
	testNullIsRefusedNotCrashed();
	testEveryMainStageNameFitsTheNameCap();
	testAFullReportFitsTheDeclaredCap();
	testNoLineExceedsTheLineBudget();
	testTooSmallABufferIsRefusedNotTruncated();
	testTheReportCarriesTheRealNumbers();

	if (s_fails) {
		printf("boot memory probe: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("boot memory probe: PASS  %d checks\n", s_checks);
	return 0;
}
