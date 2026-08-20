// The version this build believes it is.
//
// Modelled on the sibling project's source/updater.h (3ds-project-folder/model-making):
// the in-app updater (source/app/updater.c) compares this numerically against the newest
// release tag on GitHub, with a leading "v" tolerated on either side, so a tag of "v0.1.0"
// and a value here of "0.1.0" are the same version. Bump this in the same commit that cuts
// the release, or the shipped build will offer itself an update forever.
//
// Left blank between releases on purpose, same as the sibling. While it is blank the
// updater refuses to compare — it reports that the build has no version rather than
// treating an unset value as 0.0.0, which would make every release on GitHub look newer
// and offer a pointless update on every check.
#pragma once

#define BLOCKSMITH_VERSION "1.1.2"

// Whether the line above has been filled in. Everything that prints or compares the
// version goes through this so there is one answer to "do we know".
#define BLOCKSMITH_VERSION_SET (BLOCKSMITH_VERSION[0] != '\0')

// Where to ask. Kept here rather than buried in updater.c so that forking the repo is a
// one-line change — same reasoning as the sibling's MODELKIT_REPO_OWNER/NAME.
#define BLOCKSMITH_REPO_OWNER "stevenjc2009-byte"
#define BLOCKSMITH_REPO_NAME  "blocksmith"

// Releases name their asset with the version on the end — blocksmith0.1.0.cia and so on —
// so there is no single filename to ask for; the updater reads the real download URL out
// of the redirect/API response and takes whatever .cia the release actually carries.
//
// This is only the last-resort fallback base name, used if neither the redirect nor the API
// can be parsed. It resolves only if a release also carries a copy under this fixed name.
#define BLOCKSMITH_CIA_ASSET "blocksmith.cia"
