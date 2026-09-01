#include "app/updater_retry.h"

#include <stddef.h>

updaterAction updaterActionFor(bool available, bool busy, bool update_available,
                               bool done, bool failed, bool download_failed)
{
	// Two refusals first, and in this order, because they are refusals about the console
	// rather than about the update. Nothing is offered while the worker owns the job —
	// updater.h's updaterBusy() contract is that a cancelled download leaves a half-written
	// title behind — and nothing is offered when the services never came up, because both
	// updaterStartCheck() and updaterStartInstall() are no-ops then and a button that does
	// nothing is worse than no button.
	if (!available) return UPD_ACT_NONE;
	if (busy)       return UPD_ACT_NONE;

	// Ahead of the failure cases deliberately. A finished install is not a state anything
	// should be retried from: the binary on the SD card is already the new one, and the only
	// thing left to do is get into it.
	if (done) return UPD_ACT_RESTART;

	// v1.8.4. The one line this file was written for. A failed DOWNLOAD keeps the asset URL
	// the check resolved, so the retry has everything it needs to go straight back to
	// downloading; a failed CHECK has nothing to retry but the check. Collapsing both into
	// "CHECK NOW", which is what shipped up to v1.8.3, made recovering from a dropped
	// download cost three presses and two round trips to reach a decision that had already
	// been made.
	//
	// Checked before update_available rather than after, because both can be true at once:
	// a failed download leaves the newest tag and the notes in hand, so anything keying off
	// "is an update available" is still true and would win the race and say INSTALL. INSTALL
	// would in fact do the right thing — it starts the same job — but it is the wrong WORD.
	// The player just watched a download fail; the button has to admit that.
	if (failed && download_failed) return UPD_ACT_RETRY_DOWNLOAD;

	if (update_available) return UPD_ACT_INSTALL;

	// IDLE, UP_TO_DATE, and a failed check all land here. All three mean the same thing to
	// the player: nothing is known yet, go and ask.
	return UPD_ACT_CHECK;
}

const char* updaterActionLabel(updaterAction action)
{
	switch (action)
	{
		case UPD_ACT_CHECK:          return "CHECK NOW";
		case UPD_ACT_INSTALL:        return "INSTALL";
		// Says DOWNLOAD, not RETRY, because "retry" on its own is exactly the ambiguity
		// steve reported: it does not say whether pressing it re-checks or re-downloads.
		case UPD_ACT_RETRY_DOWNLOAD: return "RETRY DOWNLOAD";
		case UPD_ACT_RESTART:        return "RESTART";
		case UPD_ACT_NONE:           break;
	}
	return NULL;
}
