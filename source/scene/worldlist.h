// Step 8.4 (world-select half). Directory listing for "which worlds exist", plus the one
// piece of write access the world-select screen needs: naming and creating a new one.
//
// Pure C, no <3ds.h> — same trick as world/region.c and app/options.c: libctru mounts the
// SD card as a devoptab under "sdmc:/", so opendir/readdir/mkdir on
// "sdmc:/blocksmith/worlds" on console and on "build-host/worldlisttest/worlds" on the
// host go through the identical code in worldlist.c. dirent.h is what makes that true —
// both MinGW's runtime (the host build) and libctru's newlib (the console build) implement
// it, so there is no host/console fork anywhere in this file the way region.c's tests need
// one for mkdir(). See worldlist.c's file comment for the one mkdir() *does* need.
//
// A world is a directory under some root (main.c passes world/region.h's REGION_ROOT), so
// world-select is a listing of that directory rather than a format of its own — REGION_ROOT
// says as much in its own comment, one step before this file existed to act on it.
//
// Lives under source/scene, not source/world, because it belongs to the world-*select
// screen*, not to the world save format itself — worldlist_test.c stays under source/world
// alongside its sibling world_test.c, but this header and worldlist.c ship with title.c,
// the one thing that calls them.
#pragma once

#include <stdbool.h>

// One directory name, NUL-terminated, never the full path. 32 comfortably holds anything a
// player is likely to type on the software keyboard (see scene/title.c's New World flow)
// and is short enough that a world-select row can show a name in full at the row height
// title.c's touch-UI budget allows — see title.c's LIST_ROW_H comment for where that
// number actually comes from. FAT32's 255-byte name limit is not the constraint here; the
// UI is.
#define WORLDLIST_NAME_MAX 32

// How many world directories worldlistScan will ever report at once. A fixed ceiling
// rather than a malloc'd list because this can be called from a frame with no allocator
// budget (the console has no virtual memory and no reason to fragment the heap for a menu
// screen) — 32 is a player creating a new save roughly once a session for eight months
// straight before the list would ever have to say "and N more".
#define WORLDLIST_MAX 32

typedef struct {
	char name[WORLDLIST_NAME_MAX];
} WorldEntry;

// Lists the subdirectories of `root` into `out[0 .. min(count, cap))`, alphabetically
// sorted (strcmp order) so the same set of worlds always draws in the same order — a
// player mid-scroll should not see rows reshuffle under their thumb because readdir()
// happened to walk the FAT table in a different order this boot.
//
// Only real directories count: a stray regular file sitting in the worlds root (there is
// nothing to stop one existing — a player could drop one there over USB) is silently
// skipped rather than shown as a broken entry.
//
// `root` that does not exist yet is not an error — it is what "no world has ever been
// created" looks like on a fresh SD card — and returns 0 exactly as an empty directory
// would, so the caller (title.c's world-select screen) never has to special-case first
// boot versus an empty list.
//
// Returns the number of entries written, 0..cap. `*truncated_out` (may be NULL) is set
// true when `root` held more real subdirectories than `cap` could hold — the entries that
// *do* get written are still exactly `cap` of the real ones, sorted among themselves, just
// not guaranteed to be the alphabetically-first `cap` of the whole set (finding those would
// need a second, unbounded buffer to sort before truncating, which is the one thing a
// fixed-size, no-malloc scan cannot afford). The caller only needs to know honestly that
// there is more than what it is showing, which this gives it either way.
int worldlistScan(const char* root, WorldEntry* out, int cap, bool* truncated_out);

// True if `name` is safe to use as a single path component directly under `root`:
// non-empty, no leading or trailing whitespace, at most WORLDLIST_NAME_MAX-1 bytes (so it
// always fits a WorldEntry with room for the terminator), and built only from
// [A-Za-z0-9 _-]. That charset alone already excludes '.', so "." and ".." are rejected by
// the same loop that rejects "/" or "\\" — the explicit check below for them is kept
// anyway, on purpose, as a second line of defence: if the allowed charset ever grows to
// include '.', the loop stops catching them for free and this still does.
bool worldlistNameValid(const char* name);

// Creates `root`/`name`, creating `root` itself first if this is the very first world —
// the same parent-then-child mkdir shape main.c's saveWorldDir uses for the same reason:
// the two are independent failure points (a fresh SD card has neither yet) and mkdir on a
// directory that already exists is the expected, non-error case for the parent.
//
// False if `name` fails worldlistNameValid — this is the one gate between whatever a
// player typed into the software keyboard and a real directory on the card, so nothing
// downstream of it has to distrust the name again — or if the final directory does not
// exist afterwards. True either way if `root`/`name` exists as a directory once this
// returns, which includes the case where it already existed before the call: a player
// re-typing an existing world's name from "New World" is treated as opening that world,
// not as an error, the same way the game already treats a re-requested save directory in
// main.c's saveWorldDir.
bool worldlistCreate(const char* root, const char* name);
