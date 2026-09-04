// Host self-test for source/app/diag_retire.c — the boot fence that keeps one previous
// generation of Blocksmith's diagnostic files alive across a relaunch. The REAL file is
// linked in by tools/run_host_tests.sh, same arrangement as app/battery.c: diagRetire()
// itself is plain C stdio (fopen/remove/rename), not 3DS-specific, so no __3DS__ guard is
// needed at all and the exact shipping function runs here against real temp files.
//
// This exists because of a bug that cost real evidence. diagRetire() called remove(prev)
// unconditionally, every boot, before checking whether `live` existed. A boot that did NOT
// freeze had nothing to rename, so that boot's fence net effect was "destroy prev-<name>,
// replace it with nothing" — a clean relaunch silently ate the previous freeze's evidence
// even though nothing new had happened to earn the slot. steve's v1.8.16 freeze report
// survives today only as prose pasted into a commit message, because prev-hang.txt and
// prev-postmortem.txt were both gone from the card by the time anyone went looking.
//
// The scenario below is exactly the one specified as the success criterion: simulate boot 1
// (freeze, files written), boot 2 (clean), boot 3 (clean), and check that boot 1's evidence
// is still on disk afterwards. Against the pre-fix code (unconditional remove(prev)) this
// goes red at boot 3 — proven below by a second run of the same scenario against a
// hand-instantiated copy of the OLD behaviour, kept only as the sabotage control.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/diag_retire.h"

static int  s_checks;
static int  s_fails;
static char s_first[200];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.180s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

static bool fileExists(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

static bool fileContains(const char* path, const char* needle)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	char buf[256] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	(void)n;
	return strstr(buf, needle) != NULL;
}

static void writeFile(const char* path, const char* contents)
{
	FILE* f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "diag_retire_test: could not create %s\n", path); exit(2); }
	fputs(contents, f);
	fclose(f);
}

// The OLD, buggy shape of diagRetire(), kept here ONLY as a sabotage control so the test
// scenario below can be shown to actually go red against it. This is a snapshot of exactly
// what source/app/diag_retire.c looked like before 2026-09-04 (git blame / the 2026-09-04
// comment in diag_retire.h record the same body) — not a hand-invented twin, and it exists
// so this test file can prove "the check goes red on the real old code" without needing to
// git-checkout a previous revision of the shipping file mid-run.
static void diagRetireOldBuggy(const char* live, const char* prev)
{
	remove(prev);
	if (rename(live, prev) != 0)
		remove(live);
}

// Runs the exact scenario the task specifies: boot 1 freezes and writes `live`, boot 2 is
// clean, boot 3 is clean. `retireFn` is called at the start of each boot, before that boot's
// own (simulated) outcome, matching main.c's diagFenceBoot() calling diagRetire() before
// anything in that boot's session can write a fresh diagnostic file.
typedef void (*RetireFn)(const char*, const char*);

static void runThreeBootScenario(RetireFn retireFn, const char* live, const char* prev)
{
	remove(live);
	remove(prev);

	// Boot 1's fence: nothing exists yet, so this is a no-op either way.
	retireFn(live, prev);
	// Boot 1 freezes and writes its evidence.
	writeFile(live, "boot1 freeze evidence");

	// Boot 2's fence: live (boot 1's freeze) gets retired to prev.
	retireFn(live, prev);
	// Boot 2 is clean: no new live file.

	// Boot 3's fence: nothing new happened at boot 2, so THIS is the boot that used to
	// destroy prev unconditionally.
	retireFn(live, prev);
	// Boot 3 is clean too.
}

static void testFixedCodeKeepsEvidenceAcrossTwoCleanBoots(void)
{
	const char* live = "diag_retire_test_live.txt";
	const char* prev = "diag_retire_test_prev.txt";

	runThreeBootScenario(diagRetire, live, prev);

	CHECK(!fileExists(live));
	CHECK(fileExists(prev));
	CHECK(fileContains(prev, "boot1 freeze evidence"));

	remove(live);
	remove(prev);
}

// The red arm: same three-boot scenario, driven through the pre-fix behaviour. Proves the
// scenario above is a real check — one that goes red on the code it was written to catch —
// rather than a check that would pass no matter what diagRetire() did.
static void testOldBuggyCodeLosesEvidenceAtBoot3(void)
{
	const char* live = "diag_retire_test_live_old.txt";
	const char* prev = "diag_retire_test_prev_old.txt";

	runThreeBootScenario(diagRetireOldBuggy, live, prev);

	// This is the bug: boot 3's unconditional remove(prev) destroys boot 1's evidence even
	// though boot 2 produced nothing new to replace it with.
	CHECK(!fileExists(prev));

	remove(live);
	remove(prev);
}

static void testCleanBootWithNothingEverWrittenIsANoop(void)
{
	const char* live = "diag_retire_test_live2.txt";
	const char* prev = "diag_retire_test_prev2.txt";
	remove(live);
	remove(prev);

	diagRetire(live, prev);
	CHECK(!fileExists(live));
	CHECK(!fileExists(prev));
}

static void testSecondFreezeInARowKeepsBothGenerations(void)
{
	const char* live = "diag_retire_test_live3.txt";
	const char* prev = "diag_retire_test_prev3.txt";
	remove(live);
	remove(prev);

	// Boot N freezes.
	diagRetire(live, prev);
	writeFile(live, "boot N freeze");

	// Boot N+1's fence retires boot N's evidence to prev...
	diagRetire(live, prev);
	CHECK(fileContains(prev, "boot N freeze"));
	CHECK(!fileExists(live));

	// ...and boot N+1 ALSO freezes, writing a fresh live file.
	writeFile(live, "boot N+1 freeze");

	CHECK(fileContains(prev, "boot N freeze"));
	CHECK(fileContains(live, "boot N+1 freeze"));

	remove(live);
	remove(prev);
}

static void testDiagShouldRetirePolicy(void)
{
	CHECK(diagShouldRetire(true) == true);
	CHECK(diagShouldRetire(false) == false);
}

static void testRemoveThenRenameStillReplacesStalePrev(void)
{
	// When live DOES exist, a stale prev from an even older generation must still be
	// replaced, not merged with or left beside it — this is the FAT-rename constraint the
	// 2026-09-03 comment in diag_retire.h records.
	const char* live = "diag_retire_test_live4.txt";
	const char* prev = "diag_retire_test_prev4.txt";
	remove(live);
	remove(prev);

	writeFile(prev, "stale ancient generation");
	writeFile(live, "fresh generation");

	diagRetire(live, prev);

	CHECK(!fileExists(live));
	CHECK(fileContains(prev, "fresh generation"));
	CHECK(!fileContains(prev, "stale"));

	remove(live);
	remove(prev);
}

int main(void)
{
	testDiagShouldRetirePolicy();
	testCleanBootWithNothingEverWrittenIsANoop();
	testFixedCodeKeepsEvidenceAcrossTwoCleanBoots();
	testSecondFreezeInARowKeepsBothGenerations();
	testRemoveThenRenameStillReplacesStalePrev();
	testOldBuggyCodeLosesEvidenceAtBoot3();

	printf("diag_retire: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif /* !__3DS__ */
