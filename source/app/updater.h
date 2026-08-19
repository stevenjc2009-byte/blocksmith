// The in-app updater behind the Options page's "Check for Update" button.
//
// Modelled directly on the sibling project's source/updater.c/.h
// (3ds-project-folder/model-making) — same design, same reasoning, ported to this
// project's naming and file layout (source/app/, BLOCKSMITH_* constants in
// source/version.h). There is no update channel for homebrew — Nintendo's own only serves
// signed retail titles — so this is the whole mechanism, built from parts: ask GitHub what
// the newest release is, compare it to the version compiled into this binary, and if
// GitHub is ahead, stream the new .cia straight into the AM service and relaunch into it.
//
// Two things about that are worth knowing before reading updater.c.
//
// First, the console cannot verify github.com on its own. Its root certificate store
// predates every CA in use today, so a plain HTTPS request fails with a TLS error that no
// amount of retrying will fix. The fix is romfs:/cacert.pem, the Mozilla bundle, handed to
// libcurl explicitly. That file is the reason this project has a RomFs at all.
//
// Second, the download runs on its own thread. 3DS wifi is slow enough that a synchronous
// fetch would lock the front end for the better part of a minute, which reads as a crash.
// The caller's frame loop polls updaterState() and paints; the worker does the waiting.
#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "version.h"

// Where things stand. The front end draws from this and nothing else.
typedef enum
{
	UPDATE_IDLE,         ///< Nothing has been asked for yet.
	UPDATE_CHECKING,     ///< Talking to GitHub.
	UPDATE_UP_TO_DATE,   ///< Asked, and this build is already the newest.
	UPDATE_AVAILABLE,    ///< A newer release exists; waiting on the player.
	UPDATE_DOWNLOADING,  ///< Streaming the .cia into the install handle.
	UPDATE_INSTALLING,   ///< Download finished, AM is committing the title.
	UPDATE_DONE,         ///< Installed. The app should relaunch itself now.
	UPDATE_FAILED,       ///< Gave up. updaterMessage() says why.
} updateState;

// Brings up the services the updater needs — sockets, AM, and the RomFs the certificate
// bundle lives in. Safe to call once, from the app's init. Returns false if any of them
// refused, in which case the caller should grey the button out rather than offering
// something that cannot work.
//
// `soc_already_up` must be true when something else in the process has already called
// socInit() — in this game, net/bsnet_sock.c does, from netInit() at the top of main(). Pass
// it and this leaves SOC alone in both directions: it neither re-initialises it (libctru holds
// one SOCU handle per process, and overwriting it orphans every descriptor already opened
// against the old one) nor closes it on the way out.
bool updaterInit(bool soc_already_up);
void updaterExit(void);

// False when updaterInit could not get its services up. The options screen should grey the
// button out rather than offering something that cannot work.
bool updaterAvailable(void);

// Starts the version check on the worker thread and returns immediately. Ignored unless
// the updater is idle or settled from a previous attempt.
void updaterStartCheck(void);

// Starts the download and install, also on the worker thread. Only meaningful once a check
// has reported UPDATE_AVAILABLE; ignored otherwise.
void updaterStartInstall(void);

// Where things are. Cheap; call every frame.
updateState updaterState(void);

// True while the worker thread owns the job and the player must not be offered a way out —
// a cancelled download would leave a half-written title behind.
bool updaterBusy(void);

// One line of plain English for the caller to draw — the failure reason when the state is
// UPDATE_FAILED, a description of what is happening otherwise.
const char* updaterMessage(void);

// Percent of the download completed, or -1 when that is not a meaningful question yet.
// Draw a bar only when this is >= 0.
int updaterProgress(void);

// The newest release tag GitHub reported, valid from UPDATE_AVAILABLE onward. Empty string
// before that.
const char* updaterLatestVersion(void);

// Points the chainloader back at this title, which after a successful install is the new
// build. Only meaningful on UPDATE_DONE.
//
// It returns immediately — the jump happens when the app exits — so the caller must fall
// out of its main loop right after calling this. If the system declines the jump the player
// simply lands on the HOME menu, and tapping the icon starts the new version anyway, so
// there is no failure path to handle.
void updaterRelaunch(void);
