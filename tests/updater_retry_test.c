// Host test for source/app/updater_retry.c — the Update screen's action-button decision.
//
// Links the REAL source file. The rest of app/updater.c cannot be host-compiled (libcurl, AM,
// SOC), which is exactly why the decision was pulled out into its own translation unit for
// v1.8.4: the part the player actually experiences is now the part a test can reach.
//
// The bug being pinned: up to v1.8.3 every failure put "CHECK NOW" on the button, so a
// dropped download was recovered by re-asking GitHub for the release list, re-reading that a
// newer version exists, and only then downloading. steve: "just make sure you click retry
// then it read as a download, not, like, rechecking".

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/updater_retry.h"

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

// Named wrapper so each case below reads as the situation it describes rather than as six
// bare booleans in a row, which is unreadable and easy to get out of order.
static updaterAction act(bool available, bool busy, bool update_available,
                         bool done, bool failed, bool download_failed)
{
	return updaterActionFor(available, busy, update_available, done, failed, download_failed);
}

// ── The v1.8.4 behaviour change ───────────────────────────────────────────────────────────

static void testFailedDownloadOffersRetryDownload(void)
{
	// A download that failed. The check already succeeded, so update_available is still true
	// and the asset URL is still in hand — that combination is the whole point.
	CHECK(act(true, false, true, false, true, true) == UPD_ACT_RETRY_DOWNLOAD);

	// Even if whatever the front end reports for "is an update available" has gone false in
	// the meantime, a failed download is still a failed download.
	CHECK(act(true, false, false, false, true, true) == UPD_ACT_RETRY_DOWNLOAD);

	CHECK(strcmp(updaterActionLabel(UPD_ACT_RETRY_DOWNLOAD), "RETRY DOWNLOAD") == 0);
}

static void testFailedCheckStillOffersCheck(void)
{
	// The other half of the split, and the half that must NOT change. A check that failed has
	// no asset URL to retry with, so the only thing to do is ask again.
	CHECK(act(true, false, false, false, true, false) == UPD_ACT_CHECK);
	CHECK(strcmp(updaterActionLabel(UPD_ACT_CHECK), "CHECK NOW") == 0);
}

static void testRetryBeatsInstallOnAFailedDownload(void)
{
	// Both conditions true at once is the ordinary case, not a corner: a failed download
	// leaves update_available true. INSTALL would start the correct job, so this is not a
	// correctness bug — it is a wording bug, and the wording is the thing steve reported.
	CHECK(act(true, false, true, false, true, true) != UPD_ACT_INSTALL);
}

// ── Everything that must not have changed ─────────────────────────────────────────────────

static void testUnchangedStates(void)
{
	// Idle.
	CHECK(act(true, false, false, false, false, false) == UPD_ACT_CHECK);
	// Up to date (reports as idle-ish: nothing available, nothing failed).
	CHECK(act(true, false, false, false, false, false) == UPD_ACT_CHECK);
	// A successful check with something to install.
	CHECK(act(true, false, true, false, false, false) == UPD_ACT_INSTALL);
	CHECK(strcmp(updaterActionLabel(UPD_ACT_INSTALL), "INSTALL") == 0);
	// Installed.
	CHECK(act(true, false, false, true, false, false) == UPD_ACT_RESTART);
	CHECK(strcmp(updaterActionLabel(UPD_ACT_RESTART), "RESTART") == 0);
}

static void testBusyOffersNothing(void)
{
	// updaterBusy() covers CHECKING, DOWNLOADING and INSTALLING. In every one of them the
	// worker owns the job and a way out would leave a half-written title on the SD card.
	CHECK(act(true, true, false, false, false, false) == UPD_ACT_NONE);
	CHECK(act(true, true, true,  false, false, false) == UPD_ACT_NONE);
	CHECK(act(true, true, false, true,  false, false) == UPD_ACT_NONE);
	CHECK(act(true, true, false, false, true,  true)  == UPD_ACT_NONE);
	CHECK(updaterActionLabel(UPD_ACT_NONE) == NULL);
}

static void testUnavailableOffersNothing(void)
{
	// Services down. Both start calls are no-ops, so a button here would do nothing at all.
	// Checked for every combination of the remaining flags, because "unavailable" has to win
	// against all of them and not just against the quiet ones.
	for (int i = 0; i < 16; i++) {
		const bool busy    = (i & 1) != 0;
		const bool avail_u = (i & 2) != 0;
		const bool done    = (i & 4) != 0;
		const bool failed  = (i & 8) != 0;
		CHECK(act(false, busy, avail_u, done, failed, failed) == UPD_ACT_NONE);
	}
}

static void testDoneBeatsFailure(void)
{
	// A finished install is not retryable: the new binary is already on the card. If some
	// earlier failure flag were still set, RESTART still has to win.
	CHECK(act(true, false, false, true, true, true) == UPD_ACT_RESTART);
	CHECK(act(true, false, true,  true, true, true) == UPD_ACT_RESTART);
}

static void testDownloadFailedIsIgnoredWithoutFailure(void)
{
	// download_failed only means anything alongside failed. On its own — which is what the
	// caller passes while a download is merrily running, since the flag records which job is
	// live — it must not turn an ordinary state into a retry.
	CHECK(act(true, false, true,  false, false, true) == UPD_ACT_INSTALL);
	CHECK(act(true, false, false, false, false, true) == UPD_ACT_CHECK);
}

static void testEveryActionHasALabel(void)
{
	// A label that came back NULL for a real action would draw no button at all, which is the
	// same symptom as UPD_ACT_NONE and would be very hard to tell apart on hardware.
	const updaterAction all[] = { UPD_ACT_CHECK, UPD_ACT_INSTALL,
	                              UPD_ACT_RETRY_DOWNLOAD, UPD_ACT_RESTART };
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const char* label = updaterActionLabel(all[i]);
		CHECK(label != NULL);
		if (label) CHECK(label[0] != '\0');
	}
}

int main(void)
{
	testFailedDownloadOffersRetryDownload();
	testFailedCheckStillOffersCheck();
	testRetryBeatsInstallOnAFailedDownload();
	testUnchangedStates();
	testBusyOffersNothing();
	testUnavailableOffersNothing();
	testDoneBeatsFailure();
	testDownloadFailedIsIgnoredWithoutFailure();
	testEveryActionHasALabel();

	if (s_fails) {
		printf("updater retry action: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);
		return 1;
	}
	printf("updater retry action: PASS  %d checks\n", s_checks);
	return 0;
}
