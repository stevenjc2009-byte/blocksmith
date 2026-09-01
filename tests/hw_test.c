// Host self-test for the console-model policy in source/app/hw.c. The REAL file is linked
// in by tools/run_host_tests.sh — hw.c's libctru half is behind #ifdef __3DS__ precisely so
// the half with a decision in it, hwPreferredWorkerCore(), is host-compilable.
//
// Linking the real file is the whole point, and it is not a stylistic preference. Until
// v1.6.0 tests/battery_test.c was compiled as a lone translation unit carrying private
// copies of the arithmetic it claimed to check, so its 16 checks passed no matter what
// battery.c did — sabotaging batteryBars() to return 99 still printed PASS. This file must
// never grow a private copy of the core ladder for the same reason.
//
// Built as a separate binary with its own main(), same pattern as tests/battery_test.c,
// app/options_test.c and app/updater_version_test.c.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/hw.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

// Prints EVERY failure, not just the first. battery_test.c records only the first because it
// predates the lesson; the lesson is that when a sabotage arm is run to prove the test can go
// red, a count alone cannot tell you whether the checks that stayed green stayed green because
// they are unaffected or because the sabotage neutralised them. Reading each failing line by
// name is the only way to answer that, and it costs nothing on a green run.
#define CHECK(cond) do {                                                           \
		s_checks++;                                                                 \
		if (!(cond)) {                                                              \
			s_fails++;                                                              \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                            \
			if (!s_first[0])                                                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                            \
	} while (0)

// Runs the ladder and checks the whole answer — length AND every entry, in order. Checking
// only out[0] would let a ladder that offers core 2 and then stops pass, and that ladder
// hangs the worker on a console that refuses core 2.
static void checkLadder(bool new_3ds, bool cpu_ok,
                        int want_n, int w0, int w1, int w2)
{
	int out[HW_CORE_LADDER_MAX];
	// Poison first, so an entry the function never writes cannot accidentally hold the
	// value the test is hoping for.
	for (int i = 0; i < HW_CORE_LADDER_MAX; i++) out[i] = -99;

	const int n = hwPreferredWorkerCore(new_3ds, cpu_ok, out);

	CHECK(n == want_n);
	if (n != want_n) return;   // the entry checks below would be noise

	const int want[3] = { w0, w1, w2 };
	for (int i = 0; i < n; i++)
		CHECK(out[i] == want[i]);
}

// ── The ladder: every combination of the two inputs ────────────────────────────────────
static void testCoreLadder(void)
{
	// New 3DS, exheader granted core 2 usable and APT gave us the CPU time limit: all three,
	// best first. Core 2 is idle on the console, core 1 is shared with the system, core 0 is
	// the main thread's.
	checkLadder(true,  true,  3, 2, 1, 0);

	// New 3DS but APT_SetAppCpuTimeLimit failed, so core 1 was never ours to ask for. Core 2
	// is still worth trying; core 0 always works.
	checkLadder(true,  false, 2, 2, 0, 0);

	// Old 3DS with the time limit: exactly the ladder worker.c has always walked.
	checkLadder(false, true,  2, 1, 0, 0);

	// Old 3DS, no time limit: core 0 and nothing else. This is the case that must never be
	// empty — a zero-length ladder means threadCreate is never called and the terrain worker
	// never starts, which presents as a world that loads no chunks at all.
	checkLadder(false, false, 1, 0, 0, 0);
}

// Core 2 must NEVER be offered on an Old 3DS: it does not exist there, and asking costs a
// threadCreate that is guaranteed to fail. Stated separately from the table above because
// it is the one property that is a correctness bug rather than a lost optimisation.
static void testNoCore2OnOld3ds(void)
{
	for (int cpu_ok = 0; cpu_ok <= 1; cpu_ok++) {
		int out[HW_CORE_LADDER_MAX];
		const int n = hwPreferredWorkerCore(false, cpu_ok != 0, out);
		for (int i = 0; i < n; i++)
			CHECK(out[i] != 2);
	}
}

// Every ladder, on either console, must end at core 0 and must be non-empty. Core 0 is the
// only core that cannot be refused, so it is the only safe last rung.
static void testLadderAlwaysEndsAtCore0(void)
{
	for (int n3 = 0; n3 <= 1; n3++) {
		for (int cpu_ok = 0; cpu_ok <= 1; cpu_ok++) {
			int out[HW_CORE_LADDER_MAX];
			const int n = hwPreferredWorkerCore(n3 != 0, cpu_ok != 0, out);
			CHECK(n >= 1);
			CHECK(n <= HW_CORE_LADDER_MAX);
			if (n >= 1) CHECK(out[n - 1] == 0);
		}
	}
}

// No core may appear twice — a duplicate would make worker.c retry a core it just failed on
// instead of falling back, so a refusal on core 2 would burn two threadCreates and still not
// reach core 0 on the pass that matters.
static void testLadderHasNoDuplicates(void)
{
	for (int n3 = 0; n3 <= 1; n3++) {
		for (int cpu_ok = 0; cpu_ok <= 1; cpu_ok++) {
			int out[HW_CORE_LADDER_MAX];
			const int n = hwPreferredWorkerCore(n3 != 0, cpu_ok != 0, out);
			for (int i = 0; i < n; i++)
				for (int j = i + 1; j < n; j++)
					CHECK(out[i] != out[j]);
		}
	}
}

// The ladder must be strictly descending. Not decoration: it encodes "best first", and the
// fallback loop in worker.c walks it in order and stops at the first core that accepts. A
// ladder in the wrong order silently pins the worker to the worst available core.
static void testLadderIsDescending(void)
{
	for (int n3 = 0; n3 <= 1; n3++) {
		for (int cpu_ok = 0; cpu_ok <= 1; cpu_ok++) {
			int out[HW_CORE_LADDER_MAX];
			const int n = hwPreferredWorkerCore(n3 != 0, cpu_ok != 0, out);
			for (int i = 1; i < n; i++)
				CHECK(out[i] < out[i - 1]);
		}
	}
}

// ── The cached model answer ────────────────────────────────────────────────────────────
static void testModelFlagDefaultsSafe(void)
{
	hwTestReset();
	// Before hwInit() the answer must be "Old 3DS". Guessing New would ask a console for a
	// clock it does not have and offer a core that is not there; guessing Old only costs
	// performance on a machine that has some to spare.
	CHECK(!hwIsNew3ds());
	CHECK(!hwSpeedupRequested());

	hwInit();
	CHECK(!hwIsNew3ds());
	// The host build has no PTM:SYSM and never asks for the speedup, so this stays false
	// here whatever the model flag says. It is the console build that sets it.
	CHECK(!hwSpeedupRequested());
}

static void testModelFlagFollowsTestSeam(void)
{
	hwTestReset();
	hwTestSetNew3ds(true);
	CHECK(hwIsNew3ds());

	hwInit();
	CHECK(hwIsNew3ds());        // hwInit must not clobber what the seam set
	CHECK(!hwSpeedupRequested()); // ...and still must not claim a host speedup

	hwTestReset();
	CHECK(!hwIsNew3ds());       // reset really resets
}

// hwInit() is called from boot and is documented as safe to call again. If a second call
// re-ran the console half it would ask PTM:SYSM for the clock twice; more importantly the
// idempotence is what lets a caller that is unsure whether boot ran call it defensively.
static void testInitIsIdempotent(void)
{
	hwTestReset();
	hwTestSetNew3ds(true);
	hwInit();
	const bool n3ds_after_first = hwIsNew3ds();
	hwInit();
	hwInit();
	CHECK(hwIsNew3ds() == n3ds_after_first);
}

// The ladder is pure: same inputs, same answer, no dependence on hwInit() having run or on
// what the model seam was last set to. worker.c passes hwIsNew3ds() in explicitly rather
// than letting the ladder read it, and this is what keeps that honest.
static void testLadderIgnoresGlobalState(void)
{
	int a[HW_CORE_LADDER_MAX], b[HW_CORE_LADDER_MAX];

	hwTestReset();
	const int na = hwPreferredWorkerCore(true, true, a);

	hwTestSetNew3ds(true);
	hwInit();
	const int nb = hwPreferredWorkerCore(true, true, b);

	CHECK(na == nb);
	if (na == nb)
		for (int i = 0; i < na; i++) CHECK(a[i] == b[i]);

	// And the mirror: asking for the Old 3DS ladder on a "New 3DS" process still returns the
	// Old 3DS ladder, because the argument is the truth, not the global.
	int c[HW_CORE_LADDER_MAX];
	const int nc = hwPreferredWorkerCore(false, true, c);
	CHECK(nc == 2);
	if (nc == 2) { CHECK(c[0] == 1); CHECK(c[1] == 0); }
}

int main(void)
{
	testCoreLadder();
	testNoCore2OnOld3ds();
	testLadderAlwaysEndsAtCore0();
	testLadderHasNoDuplicates();
	testLadderIsDescending();
	testModelFlagDefaultsSafe();
	testModelFlagFollowsTestSeam();
	testInitIsIdempotent();
	testLadderIgnoresGlobalState();

	printf("hw core ladder and model flag: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif /* !__3DS__ */
