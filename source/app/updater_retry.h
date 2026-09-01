// Which verb the Update screen's action button offers, decided in one place.
//
// This exists as its own file, free of <3ds.h>, for one reason: it is the only part of the
// updater that can be tested on a host. Everything else in app/updater.c is libcurl, AM and
// SOC, none of which exist off the console, so the decision that actually matters to the
// player — "what happens when I press this button" — used to be unreachable by any test and
// was written straight into the middle of scene/title.c's draw function.
//
// v1.8.4, and the reason it moved. steve, after a failed download on hardware: "if it fails
// to download it, just make sure you click retry then it read as a download, not, like,
// rechecking, then read and downloading". Before this, every failure — a failed version
// check and a failed download alike — put "CHECK NOW" on the button, so recovering from a
// dropped download meant asking GitHub what the newest release was all over again, watching
// it say a newer version is available all over again, and only then pressing download. Three
// presses and two round trips to retry one thing that had already been resolved.
//
// The asset URL is still in hand after a failed download, so the retry has everything it
// needs to go straight back to downloading. All this file does is say so.
#pragma once

#include <stdbool.h>

// What the action button does. UPD_ACT_NONE means draw no action button at all — the state
// is one where the player must not be offered a way out (a live download) or one where
// nothing could work anyway (services down).
typedef enum
{
	UPD_ACT_NONE,
	UPD_ACT_CHECK,           ///< Ask GitHub what the newest release is.
	UPD_ACT_INSTALL,         ///< A newer release is known; download and install it.
	UPD_ACT_RETRY_DOWNLOAD,  ///< The download failed; go straight back to downloading it.
	UPD_ACT_RESTART,         ///< Installed; relaunch into the new build.
} updaterAction;

// The whole decision, as a pure function of what the caller can see.
//
// Taken as separate booleans rather than as an updateState so this file does not have to
// include updater.h and drag <3ds.h> in with it. The call site in scene/title.c does the
// four comparisons; this does the thinking.
//
// `download_failed` distinguishes the two failures that updateState collapses into one:
// true when the thing that failed was the download or the install (so the asset URL is
// still valid and a retry means "download it again"), false when it was the version check
// (so there is nothing to retry but the check itself).
updaterAction updaterActionFor(bool available, bool busy, bool update_available,
                               bool done, bool failed, bool download_failed);

// The button text for an action. Never NULL for a real action; NULL for UPD_ACT_NONE, which
// is the caller's signal to draw no button.
const char* updaterActionLabel(updaterAction action);
