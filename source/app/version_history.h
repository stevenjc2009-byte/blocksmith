// The in-app "VERSION HISTORY" browser's data half — steve's brief, in his words: "it should
// be displaying all the past versions up to the current latest version. What it added, what
// it changed, what it fixed, what it removed ... So that way players can see ... what they've
// missed out on."
//
// ── Where the text comes from ─────────────────────────────────────────────────────────
//
// tools/make_version_history.sh generates app/version_history_data.c from every
// whatsnew<version>.txt this repo ships — the same release-notes files app/updater.c fetches
// one of (the newest) for the update screen's single-version notes (app/whatsnew.h). Baking
// them means no network request at all: the whole history a build was shipped with is on the
// SD card already, in the .cia, so the browser works offline and costs nothing against the
// GitHub rate limit app/updater.c's own file comment already worries about.
//
// This is pure C, no <3ds.h>, for the same reason app/whatsnew.c is: tests/version_history_test.c
// links THIS file (and app/whatsnew.c, which does the actual parsing) rather than a
// hand-written twin of it.
//
// ── What "up to the current latest version" means here ───────────────────────────────
//
// Baked history can only ever cover versions that existed when THIS build was compiled — a
// player who has not updated yet cannot have a build that already knows what the next
// release says about itself. scene/title.c covers the gap without a second network request:
// when a check has already found a newer release (updaterState() == UPDATE_AVAILABLE), the
// browser also shows updaterLatestVersion()/updaterReleaseNotes() — data the update screen's
// own check already fetched — as one more entry above the baked ones. See scene/title.c's
// drawVersionHistory for that seam; nothing in this file or version_history.c knows about the
// network at all.
//
// ── Why versions before v1.6.0 are not here ───────────────────────────────────────────
//
// app/whatsnew.h's own file comment: "every release published before [v1.6.0] has no such
// asset, and that is fine by design". This browser inherits that cutoff rather than inventing
// a second one, and rather than reaching into CHANGELOG.md's pre-v1.6.0 entries — several of
// which are marked "diagnostic pre-release" or, for v1.1.7, "NEVER PUBLISHED" — for text that
// was never meant for a player to read.
#pragma once

#include <stddef.h>

#include "app/whatsnew.h"

// One baked version. `data`/`len` are the exact bytes of that release's whatsnew<version>.txt,
// unparsed — parsing happens on demand (versionHistoryNotesAt), so only ONE WhatsNew's worth
// of memory (app/whatsnew.h, ~3 KB) is ever live at a time no matter how many versions are
// baked in, rather than one WhatsNew per version.
typedef struct {
	const char*          version;   // e.g. "1.8.6", no leading "v" — matches BLOCKSMITH_VERSION
	const unsigned char* data;
	int                  len;
} VersionHistoryEntry;

// Defined in the generated app/version_history_data.c. Oldest version first, so index 0 is
// the earliest release with notes and VERSION_HISTORY_COUNT-1 is the newest one this build
// was compiled with. Do not read these two directly outside version_history.c — the
// versionHistory* functions below are the interface, so callers do not have to know the
// table is sorted oldest-first if that ever changes.
extern const VersionHistoryEntry VERSION_HISTORY[];
extern const int                 VERSION_HISTORY_COUNT;

// How many versions are baked into this build.
int versionHistoryCount(void);

// The version string at `index` (0 = oldest), or "" when `index` is out of range. Never NULL.
const char* versionHistoryVersionAt(int index);

// Parses the notes for the version at `index` into `out` (cleared first — see
// app/whatsnew.h's whatsnewParse). An out-of-range `index` clears `out` to the empty state,
// the same "no notes" state a release with no whatsnew asset produces, rather than leaving it
// holding whatever the caller last put there.
void versionHistoryNotesAt(int index, WhatsNew* out);
