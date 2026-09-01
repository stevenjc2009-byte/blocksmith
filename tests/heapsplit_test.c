// Host tests for the linear/application heap split policy.
//
// The console half of app/heapsplit.c cannot run here — it is svc calls that panic the process
// on failure, before main, before the crash handler. The policy half can, and the policy is
// where a mistake is fatal: too large a linear heap makes libctru's own guard fire
// svcBreak(USERBREAK_PANIC) and the console does not boot. That failure was observed for real
// on an Old 3DS memory-mode boot before these tests existed, which is what they encode.
//
// The two `remaining` figures used throughout are measured, not invented. They are the sum of
// the linear heap and application heap that a real boot reported on each console:
//   New 3DS (ExHeader SystemModeExt : 124MB) : 33,554,432 + 93,270,016 = 126,824,448
//   Old 3DS (ExHeader SystemModeExt : Legacy):  33,554,432 + 30,355,456 =  63,909,888
#include "app/heapsplit.h"

#include <stdio.h>

static int checks = 0;
static int fails  = 0;

#define CHECK(cond) do {                                                        \
	checks++;                                                                   \
	if (!(cond)) {                                                              \
		fails++;                                                                \
		printf("FAIL %d/%d L%d  %s\n", fails, checks, __LINE__, #cond);         \
	}                                                                           \
} while (0)

#define REMAINING_NEW_3DS  126824448u
#define REMAINING_OLD_3DS   63909888u

static void testNew3dsGrowsTheLinearHeap(void)
{
	unsigned lin = 0, app = 0;
	CHECK(heapSplitChoose(REMAINING_NEW_3DS, &lin, &app) == true);
	CHECK(lin == 67108864u);

	// Not an arbitrary expectation: the boot that actually ran with a 64 MB linear heap
	// reported "app heap size 59715584". If this line ever disagrees, the policy has drifted
	// from the thing that was measured.
	CHECK(app == 59715584u);
}

static void testOld3dsDeclinesAndLeavesLibctruAlone(void)
{
	unsigned lin = 0xAAAAAAAAu, app = 0xBBBBBBBBu;
	CHECK(heapSplitChoose(REMAINING_OLD_3DS, &lin, &app) == false);

	// A refusal must not scribble on the out-params. The caller uses them only when the
	// function returned true, but a policy that half-writes on the failure path is one edit
	// away from a console that does not boot.
	CHECK(lin == 0xAAAAAAAAu);
	CHECK(app == 0xBBBBBBBBu);
}

static void testTheSumNeverExceedsWhatTheProcessWasGranted(void)
{
	// The invariant whose violation panics the console, swept rather than spot-checked. Step is
	// a page so alignment is exercised too; the range spans both consoles and well past them.
	for (unsigned remaining = 0; remaining <= 200u * 1024u * 1024u; remaining += 4096u * 257u) {
		unsigned lin = 0, app = 0;
		if (heapSplitChoose(remaining, &lin, &app)) {
			CHECK(lin + app <= remaining);
			CHECK((lin & 0xFFFu) == 0);
			CHECK((app & 0xFFFu) == 0);
			CHECK(app >= HEAPSPLIT_APP_FLOOR_BYTES);
		}
	}
}

static void testTheBoundaryIsAcceptedAndOnePageBelowIsNot(void)
{
	const unsigned exact = HEAPSPLIT_LINEAR_TARGET_BYTES + HEAPSPLIT_APP_FLOOR_BYTES;

	unsigned lin = 0, app = 0;
	CHECK(heapSplitChoose(exact, &lin, &app) == true);
	CHECK(app == HEAPSPLIT_APP_FLOOR_BYTES);

	// One page less must refuse. This is the check that goes red if the floor comparison is
	// loosened, which is the single most dangerous edit anyone can make to this file.
	unsigned lin2 = 0, app2 = 0;
	CHECK(heapSplitChoose(exact - 4096u, &lin2, &app2) == false);
}

static void testTooSmallToHoldTheLinearHeapAtAll(void)
{
	unsigned lin = 0, app = 0;
	CHECK(heapSplitChoose(HEAPSPLIT_LINEAR_TARGET_BYTES - 4096u, &lin, &app) == false);
	CHECK(heapSplitChoose(0u, &lin, &app) == false);
}

static void testTheAppFloorLeavesRoomForTheWorldStore(void)
{
	// world/budget.h caps the world store at 12 MB and it is calloc'd, so it comes out of the
	// application heap. The floor exists to keep that affordable; if someone lowers the floor
	// under the world cap, the split becomes the reason worlds fail to load.
	CHECK(HEAPSPLIT_APP_FLOOR_BYTES >= 12u * 1024u * 1024u);
}

static void testNullOutParamsAreRefusedNotCrashed(void)
{
	unsigned x = 0;
	CHECK(heapSplitChoose(REMAINING_NEW_3DS, NULL, &x) == false);
	CHECK(heapSplitChoose(REMAINING_NEW_3DS, &x, NULL) == false);
	CHECK(heapSplitChoose(REMAINING_NEW_3DS, NULL, NULL) == false);
}

static void testRadiusFiveMeshPoolFitsWhatThePolicyGrants(void)
{
	// The whole point of the change, stated as a number. The mesh pool is
	// 8,151,040 + 65,536 * ((2r+1)^2 * 8 - 376), measured on the host and confirmed to the byte
	// by a boot probe reading of "chunkRenderInit cost lin +9199616" at radius 3.
	const unsigned pool_r5 = 46948352u;   // radius 5
	const unsigned pool_r6 = 72114176u;   // radius 6

	unsigned lin = 0, app = 0;
	CHECK(heapSplitChoose(REMAINING_NEW_3DS, &lin, &app) == true);
	CHECK(pool_r5 < lin);   // fits, with room for the rest of the linear allocations
	CHECK(pool_r6 > lin);   // does not, and this change is not claimed to buy it
}

int main(void)
{
	testNew3dsGrowsTheLinearHeap();
	testOld3dsDeclinesAndLeavesLibctruAlone();
	testTheSumNeverExceedsWhatTheProcessWasGranted();
	testTheBoundaryIsAcceptedAndOnePageBelowIsNot();
	testTooSmallToHoldTheLinearHeapAtAll();
	testTheAppFloorLeavesRoomForTheWorldStore();
	testNullOutParamsAreRefusedNotCrashed();
	testRadiusFiveMeshPoolFitsWhatThePolicyGrants();

	printf("heap split policy: %s %d checks\n", fails ? "FAIL" : "PASS", checks);
	return fails ? 1 : 0;
}
