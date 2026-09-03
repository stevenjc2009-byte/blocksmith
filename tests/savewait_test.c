// Host suite for source/app/savewait.c — the save-slot wait that runs on the MAIN thread.
//
// ── what this is for ──────────────────────────────────────────────────────────────────────
//
// steve's number-one report, twice from real hardware: "I walked a chunk or two from spawn,
// and my game is frozen, not unfrozen, and is unresponsive. It has froze the entire console."
// The framerate was "slightly lagging" first, and nothing was written to the SD card.
//
// The mechanism, traced in the design: main.c's genFollow -> genRecenter -> genUnloadColumn
// calls workerSubmitSave once per dirty column leaving the ring — up to nine columns on one
// axis-aligned ring step, seventeen on a diagonal — and workerSubmitSave used to spin the main
// thread in a bare `for (;;)` with a 1 ms sleep, no deadline and no aptMainLoop(). SAVE_SLOTS
// is 2, so up to seven of those nine waits land in a single frame, and while the main thread is
// in that loop APT is unserviced: HOME stops responding and the console reads as dead rather
// than as slow.
//
// savewait.c is that loop, lifted out so it can be run here. It is the code the console runs,
// not a model of it — tests/savewait_guard_test.c reads source/app/worker.c as text and asserts
// worker.c has no loop of its own left, because a module that is perfect while worker.c still
// carries its own `for (;;)` would be the test-links-nothing failure all over again.
//
// ── the three arms ────────────────────────────────────────────────────────────────────────
//
//   1. GREEN-capable: a slot frees at 20 fake ms. Asserts SAVEWAIT_ROOM, waited_ms 19..21, and
//      that idle() was never asked to pump (20 ms is inside one frame). Without this arm the
//      deadline could be "return EXPIRED immediately" and arm 2 would still pass — a check that
//      cannot go green proves nothing.
//   2. THE CLAIM: room() is never true. Asserts SAVEWAIT_EXPIRED, waited_ms <= budget + 2, and
//      that idle() was called with pump == false for every call before 32 ms and pump == true
//      for every call at or after it. That second half is the HOME-alive property, asserted
//      rather than described in a comment.
//   3. RED ARM: this same file and savewait.c rebuilt with -DSAVEWAIT_NO_DEADLINE, which
//      compiles the deadline out of savewait.c. The binary MUST exit non-zero. The harness
//      checks the exit VALUE, not a grep for "FAIL", and treats 127 as a build failure rather
//      than a caught bug.
//
// ── the runaway cap, which is what makes arm 3 fail instead of hang ───────────────────────
//
// The fake idle() trips a flag and starts answering room() true after SAVEWAIT_BUDGET_MS + 64
// calls. Without it the deadline-free build would spin here forever, and a hanging test is not
// a failing test — it is a suite that never finishes and a CI job that gets killed.
//
// ── what this file does NOT prove ─────────────────────────────────────────────────────────
//
// Nothing about a real SD card, a real LightLock, or a real aptMainLoop(). The clock, the slot
// and the sleep are all fakes. It proves the POLICY: that the wait terminates inside its
// budget, that it does not terminate early, that the pump threshold is where it says it is,
// and that removing the deadline makes all of that go red.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/savewait.h"

static int s_checks;
static int s_fails;

// Reports every failure, not just the first, so a red run is readable case by case and a check
// that stayed green under the sabotage can be spotted.
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

// ── The fake console half ─────────────────────────────────────────────────────────────────
//
// A clock that only moves when idle() is called (1 ms a call, which is what svcSleepThread
// (1000000ULL) does on the console), a slot that frees on a schedule, and a per-call log of
// what pump was.

#define RUNAWAY_AT (SAVEWAIT_BUDGET_MS + 64u)
#define PUMP_LOG_MAX (RUNAWAY_AT + 8u)

typedef struct {
	uint32_t now;             // fake ms since the fixture was reset
	uint32_t free_at_ms;      // room() answers true from here on; UINT32_MAX = never
	uint32_t idle_calls;
	bool     runaway;         // idle() was called more times than any bounded wait could
	uint8_t  pump_log[PUMP_LOG_MAX];
} Fake;

static Fake s_f;

static void fakeReset(uint32_t free_at_ms)
{
	memset(&s_f, 0, sizeof s_f);
	s_f.free_at_ms = free_at_ms;
}

static bool fakeRoom(void* ud)
{
	(void)ud;
	// The runaway escape. Once tripped, room() answers true so saveWaitForRoom RETURNS and the
	// checks below can report a failure — a test that hangs reports nothing at all.
	if (s_f.runaway) return true;
	return s_f.now >= s_f.free_at_ms;
}

static void fakeIdle(void* ud, bool pump)
{
	(void)ud;
	if (s_f.idle_calls < PUMP_LOG_MAX) s_f.pump_log[s_f.idle_calls] = pump ? 1u : 0u;
	s_f.idle_calls++;
	if (s_f.idle_calls >= RUNAWAY_AT) s_f.runaway = true;
	s_f.now += 1u;   // svcSleepThread(1000000ULL) — one millisecond
}

static uint32_t fakeNow(void* ud)
{
	(void)ud;
	return s_f.now;
}

static SaveWait makeWait(uint32_t budget_ms)
{
	SaveWait w;
	w.room      = fakeRoom;
	w.idle      = fakeIdle;
	w.now_ms    = fakeNow;
	w.ud        = NULL;
	w.budget_ms = budget_ms;
	return w;
}

// ── 1. A slot that frees on time is waited for, and not pumped ────────────────────────────

static void testSlotFreesOnTime(void)
{
	puts("savewait: a slot that frees inside one frame is waited out normally");

	fakeReset(20u);
	const SaveWait w = makeWait(SAVEWAIT_BUDGET_MS);

	uint32_t waited = 0xFFFFFFFFu, iters = 0xFFFFFFFFu;
	const SaveWaitResult r = saveWaitForRoom(&w, &waited, &iters);

	CHECK(r == SAVEWAIT_ROOM,
	      "returns SAVEWAIT_ROOM when the slot frees at 20 ms (got %d) — a deadline that "
	      "expired immediately would satisfy arm 2 and be useless",
	      (int)r);
	CHECK(waited >= 19u && waited <= 21u,
	      "waited_ms is 19..21 for a 20 ms wait (got %lu)", (unsigned long)waited);
	CHECK(iters == 20u, "idle() ran once per fake millisecond: iters %lu, want 20",
	      (unsigned long)iters);
	CHECK(!s_f.runaway, "the runaway cap was NOT reached (idle calls %lu, cap %lu)",
	      (unsigned long)s_f.idle_calls, (unsigned long)RUNAWAY_AT);

	unsigned pumped = 0;
	for (uint32_t i = 0; i < s_f.idle_calls && i < PUMP_LOG_MAX; i++)
		if (s_f.pump_log[i]) pumped++;
	CHECK(pumped == 0,
	      "APT was never pumped on a 20 ms wait (pumped %u of %lu calls) — a wait inside one "
	      "frame must be byte-for-byte the old behaviour, no mid-frame re-entry into APT",
	      pumped, (unsigned long)s_f.idle_calls);
}

// ── 2. THE CLAIM: a slot that never frees still returns, and HOME stays alive ─────────────

static void testNeverFreesExpires(void)
{
	puts("savewait: a slot that NEVER frees returns EXPIRED inside the budget, pumping APT "
	     "past one frame");

	fakeReset(0xFFFFFFFFu);   // never
	const SaveWait w = makeWait(SAVEWAIT_BUDGET_MS);

	uint32_t waited = 0, iters = 0;
	const SaveWaitResult r = saveWaitForRoom(&w, &waited, &iters);

	CHECK(r == SAVEWAIT_EXPIRED,
	      "returns SAVEWAIT_EXPIRED rather than spinning the main thread forever (got %d)",
	      (int)r);
	CHECK(waited <= SAVEWAIT_BUDGET_MS + 2u,
	      "waited_ms %lu is within SAVEWAIT_BUDGET_MS+2 (%lu)",
	      (unsigned long)waited, (unsigned long)(SAVEWAIT_BUDGET_MS + 2u));
	CHECK(!s_f.runaway,
	      "the runaway cap was NOT reached — the wait ended because of its DEADLINE, not "
	      "because the fixture rescued it (idle calls %lu, cap %lu)",
	      (unsigned long)s_f.idle_calls, (unsigned long)RUNAWAY_AT);

	// The HOME-alive half. Every idle() call before SAVEWAIT_PUMP_MS of elapsed time must have
	// had pump == false, and every call at or after it pump == true. The fake clock advances
	// 1 ms per idle call, so call index i happens at i ms.
	unsigned early_pumped = 0, late_unpumped = 0;
	int first_pump = -1;
	for (uint32_t i = 0; i < s_f.idle_calls && i < PUMP_LOG_MAX; i++) {
		if (s_f.pump_log[i] && first_pump < 0) first_pump = (int)i;
		if (i <  SAVEWAIT_PUMP_MS && s_f.pump_log[i]) early_pumped++;
		if (i >= SAVEWAIT_PUMP_MS && !s_f.pump_log[i]) late_unpumped++;
	}

	CHECK(early_pumped == 0,
	      "no idle() call before %lu ms asked for a pump (found %u)",
	      (unsigned long)SAVEWAIT_PUMP_MS, early_pumped);
	CHECK(late_unpumped == 0,
	      "every idle() call at or after %lu ms asked for a pump (found %u that did not) — "
	      "this is the half that keeps HOME responding during the stall",
	      (unsigned long)SAVEWAIT_PUMP_MS, late_unpumped);
	CHECK(first_pump == (int)SAVEWAIT_PUMP_MS,
	      "the first pumped idle() is call %d, i.e. at %lu ms (got %d)",
	      (int)SAVEWAIT_PUMP_MS, (unsigned long)SAVEWAIT_PUMP_MS, first_pump);
}

// ── 3. budget_ms == 0 is the flush path: no deadline, by request ──────────────────────────
//
// workerFlushSaves passes 0 on purpose (the quit and lid-close path, where a long wait is
// legitimate) and still wants the pump. Checked so that "0 means no deadline" is a tested
// contract rather than an accident of the `if (w->budget_ms && ...)` guard, and so a future
// edit that made 0 mean "expire at once" would lose a save and be caught here.

static void testZeroBudgetHasNoDeadline(void)
{
	puts("savewait: budget_ms == 0 (the flush path) has no deadline and still pumps");

	fakeReset(100u);
	const SaveWait w = makeWait(0u);

	uint32_t waited = 0, iters = 0;
	const SaveWaitResult r = saveWaitForRoom(&w, &waited, &iters);

	CHECK(r == SAVEWAIT_ROOM, "returns SAVEWAIT_ROOM after 100 ms with no budget (got %d)",
	      (int)r);
	CHECK(waited == 100u, "waited_ms %lu, want 100", (unsigned long)waited);
	CHECK(!s_f.runaway, "the runaway cap was NOT reached (idle calls %lu)",
	      (unsigned long)s_f.idle_calls);
	CHECK(s_f.pump_log[SAVEWAIT_PUMP_MS] == 1u,
	      "the flush path pumps APT past one frame too — call %lu had pump %u, want 1",
	      (unsigned long)SAVEWAIT_PUMP_MS, (unsigned)s_f.pump_log[SAVEWAIT_PUMP_MS]);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== savewait test ==");
#ifdef SAVEWAIT_NO_DEADLINE
	puts("  ...RED ARM: built with -DSAVEWAIT_NO_DEADLINE, which compiles the deadline out of");
	puts("  ...source/app/savewait.c. This build MUST fail and MUST exit non-zero.");
#endif
	printf("  ...SAVEWAIT_BUDGET_MS=%lu SAVEWAIT_PUMP_MS=%lu runaway cap=%lu idle calls\n",
	       (unsigned long)SAVEWAIT_BUDGET_MS, (unsigned long)SAVEWAIT_PUMP_MS,
	       (unsigned long)RUNAWAY_AT);

	testSlotFreesOnTime();
	testNeverFreesExpires();
	testZeroBudgetHasNoDeadline();

	printf("\n%s %d checks, %d failed\n", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}
