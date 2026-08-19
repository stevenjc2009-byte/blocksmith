// The part of the updater (app/updater.c) that has no business touching a socket: parsing
// a version tag, comparing two of them, and building the filename/URL pieces that come out
// of a tag alone. Pulled into its own pure-C, no-<3ds.h> module for the same reason
// app/options.c is pure C — so it can be linked into a host test and proven correct without
// a console, an emulator, or a real GitHub fetch.
//
// Modelled directly on the equivalent static helpers in the sibling project's
// source/updater.c (3ds-project-folder/model-making): versionParse, versionIsNewer,
// tagFromRedirect and buildAssetUrl. The logic is unchanged; only the asset-name piece is
// factored out on its own (updaterBuildAssetName) so the URL-building in updater.c and the
// host test here can both use it without formatting a full URL.
#pragma once

#include <stddef.h>
#include <stdbool.h>

// Splits "v1.2.3", "1.2.3", "1.2" or "1" into three numbers. Anything it cannot read stays
// zero, which makes a malformed tag compare as older than any real release rather than
// triggering a spurious update.
void updaterVersionParse(const char* text, int out[3]);

// True if `candidate` (a tag straight off GitHub, "v" or no "v") is a newer version than
// `current` (this build's own BLOCKSMITH_VERSION, never prefixed with "v").
bool updaterVersionIsNewer(const char* candidate, const char* current);

// Pulls the version tag out of the URL /releases/latest redirects to. That URL is
// .../releases/tag/v0.1.0, so the tag is simply what follows. False if `url` is NULL or
// does not contain the marker, or if nothing follows it.
bool updaterTagFromRedirect(const char* url, char* out, size_t out_size);

// Builds the release asset's filename from its tag — "v0.1.0" (or "0.1.0") becomes
// "blocksmith0.1.0.cia" — the name every release's .cia is actually published under. Used
// both to build the download URL (updater.c) and as the fallback filename compared against
// what a real release's browser_download_url carries.
void updaterBuildAssetName(const char* tag, char* out, size_t out_size);
