#include "scene/worldlist.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Nothing in here includes <3ds.h>. Same reasoning as world/region.c and app/options.c: the
// SD card is a devoptab mounted at "sdmc:/", so opendir/readdir/stat/mkdir reach it on
// console exactly as they reach a host temp directory in the test build.

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX (and libctru's newlib)
// takes a mode. Copied from world/world_test.c's testMkdir / app/options_test.c's
// testMkdir rather than re-derived — same problem, same fix, three times now. Only the
// host build takes the #if branch; devkitARM always takes the POSIX one.
static void worldlistMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

static int worldEntryCmp(const void* a, const void* b)
{
	return strcmp(((const WorldEntry*)a)->name, ((const WorldEntry*)b)->name);
}

int worldlistScan(const char* root, WorldEntry* out, int cap, bool* truncated_out)
{
	if (truncated_out) *truncated_out = false;

	// A cap <= 0 or a NULL root/out is a caller bug, not a filesystem condition — reported
	// as an empty, non-truncated list rather than crashing on the memset/qsort below.
	if (!root || !out || cap <= 0) return 0;

	DIR* d = opendir(root);
	if (!d) return 0;   // missing root: no worlds have ever been created, not an error

	int n = 0;
	bool truncated = false;

	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		const char* name = ent->d_name;
		if (!strcmp(name, ".") || !strcmp(name, "..")) continue;

		// d_type is not trustworthy across the two platforms this file compiles on — MinGW's
		// dirent does not populate it at all on some runtimes — so directory-ness is decided
		// by stat()ing the full path, which is the one answer both platforms agree on.
		char path[512];
		if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path))
			continue;   // a path this long cannot be a world this build ever created; skip it

		struct stat st;
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

		const size_t len = strlen(name);
		if (len >= WORLDLIST_NAME_MAX) {
			// A directory name too long for a WorldEntry to hold at all. Not one of this
			// build's worlds (worldlistCreate never makes one this long), so it is dropped
			// rather than truncated to something that might collide with a shorter real
			// name — but the caller still needs to know the listing is not the whole truth.
			truncated = true;
			continue;
		}

		if (n >= cap) {
			// Keep walking the directory rather than break: the only way to report
			// `truncated_out` honestly is to know whether *any* further entry existed, and
			// stopping early would silently under-report on a `root` whose last real entry
			// happens to be its (cap+1)th.
			truncated = true;
			continue;
		}

		memset(out[n].name, 0, WORLDLIST_NAME_MAX);
		memcpy(out[n].name, name, len);
		n++;
	}
	closedir(d);

	// Sorted only among what survived — see worldlist.h's comment on why the truncated set
	// is not guaranteed to be the alphabetically-first `cap` of the real total.
	qsort(out, (size_t)n, sizeof(WorldEntry), worldEntryCmp);

	if (truncated_out) *truncated_out = truncated;
	return n;
}

bool worldlistNameValid(const char* name)
{
	if (!name || !*name) return false;

	const size_t len = strlen(name);
	if (len >= WORLDLIST_NAME_MAX) return false;

	// Redundant with the charset loop below (neither "." nor ".." contains anything outside
	// [A-Za-z0-9 _-] because '.' itself is not in it) — kept explicit anyway; see the header
	// comment on why.
	if (!strcmp(name, ".") || !strcmp(name, "..")) return false;

	if (isspace((unsigned char)name[0]) || isspace((unsigned char)name[len - 1]))
		return false;

	for (size_t i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)name[i];
		if (isalnum(c) || c == ' ' || c == '_' || c == '-') continue;
		return false;   // includes '/', '\\', '.', and every control/punctuation character
	}

	return true;
}

bool worldlistCreate(const char* root, const char* name)
{
	if (!root || !worldlistNameValid(name)) return false;

	worldlistMkdir(root);   // EEXIST (already there) is the expected case, not checked here

	char path[512];
	if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path))
		return false;   // worldlistNameValid's length cap makes this unreachable in practice,
	                     // but `root` itself is caller-supplied and not bounded by it

	worldlistMkdir(path);

	// The only thing that actually matters: is there a real directory at `path` now, however
	// it got there — freshly made this call, or already existing from a previous one.
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
