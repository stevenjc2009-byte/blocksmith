#include "scene/worldlist.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// rmdir() and remove(): worldlistDelete needs both. remove() is <stdio.h> on every platform;
// rmdir() is <unistd.h> on POSIX and comes out of <direct.h> as a non-underscore oldname alias
// on MinGW. Copied from world/worldlist_test.c, which already had to solve exactly this split
// for its own cleanup helper — same problem, same fix, rather than re-derived.
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

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

// ── v1.9.0 item 6.3: rename and delete ────────────────────────────────────────────────

// The one path-buffer size in this file, named rather than repeated so worldlistDelete's
// single shared buffer and worldlistScan/worldlistCreate's locals cannot drift apart. 512 is
// what those two already used; this is not a second convention, it is the existing one given
// a name because the recursive walk below has to talk about its size.
#define WORLDLIST_PATH_MAX 512

bool worldlistPathComponentSafe(const char* name)
{
	if (!name || !*name) return false;

	if (!strcmp(name, ".") || !strcmp(name, "..")) return false;

	for (const char* p = name; *p; p++) {
		// ':' is in here with the two separators because the console's paths are devoptab
		// paths — "sdmc:/blocksmith/worlds" — so a ':' in a name is not merely an odd
		// character, it is a drive prefix, and "sdmc:/x" appended to anything still resolves
		// from the root of the card on some path parsers.
		if (*p == '/' || *p == '\\' || *p == ':') return false;
	}

	// Any embedded "..", not just a name that IS "..". Nothing worldlistNameValid accepts can
	// contain a '.' at all, so this refuses no legal world name; it costs one strstr and
	// removes the need for the next reader to reason about whether "a..b" could ever matter.
	if (strstr(name, "..") != NULL) return false;

	return true;
}

// Removes the tree rooted at `path`, which is the FIRST `cap` bytes of the caller's one
// shared buffer — every level below appends "/child" to it and truncates back on the way out,
// so the 512 bytes are paid once by worldlistDelete rather than once per level. See
// WORLDLIST_DELETE_MAX_DEPTH in the header for the stack arithmetic that shape exists for.
//
// `depth` is levels remaining, counting down; 0 means "descend no further". Returns false and
// leaves `path` itself in place if ANY entry beneath it survived, with errno restored to the
// first failure rather than whatever the last readdir/closedir left behind.
//
// FAIL-CLOSED, v1.9.x audit fix. This loop used to record the first failure and keep going —
// "loudly refuse, but still visit every entry" — which meant one unremovable file did not stop
// every OTHER sibling in the same directory from being deleted anyway: the caller got `false`
// back over a directory that was, in practice, gutted rather than merely dirtied. What actually
// failed can never be undone (a file already unlink()'d is gone whether or not this function
// goes on to look at anything else), so the only lever left is to stop looking: the moment one
// entry is known bad, the walk BREAKS instead of continuing, and whatever readdir() had not yet
// handed back — which on most filesystems is most of a directory the failure was hit early in —
// survives untouched. This does not restore what was already removed before the failure was
// hit (nothing can), and it does not bound WHICH entries survive, because POSIX does not define
// readdir() order — see tests/worldlist_ops_test.c's testDeleteStopsOnFirstFailure for what that
// test can and cannot prove about it. It does bound how much WORSE a doomed delete is allowed to
// get once it is known doomed, which is the honest version of "remove nothing it hasn't already"
// a single unlink() syscall (no rollback primitive) can deliver.
static bool worldlistRmTree(char* path, size_t cap, int depth)
{
	struct stat st;
	if (stat(path, &st) != 0) return false;   // errno is stat's

	// A plain file (or anything else that is not a directory) is one remove() and done — no
	// recursion, no depth spent, which is why the depth check below sits under this rather
	// than above it: a flat world directory full of region files uses exactly one level.
	if (!S_ISDIR(st.st_mode)) return remove(path) == 0;

	if (depth <= 0) {
		errno = ELOOP;
		return false;
	}

	DIR* d = opendir(path);
	if (!d) return false;   // errno is opendir's

	const size_t base_len = strlen(path);
	int first_errno = 0;    // 0 == nothing has failed yet

	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		const char* n = ent->d_name;
		if (!strcmp(n, ".") || !strcmp(n, "..")) continue;

		// The one place worldlistPathComponentSafe is genuinely not redundant: this name came
		// off the card, not out of worldlistNameValid, and nothing guarantees this build wrote
		// it. A name carrying a separator here would take the join below outside the subtree
		// this function is allowed to touch, so it is refused — and refused LOUDLY (the parent
		// then survives and the call reports false) rather than skipped quietly, because a
		// "successful" delete that left something behind is the report this must never make.
		//
		// Every failure branch below BREAKS rather than CONTINUEs — see the fail-closed comment
		// above this function. first_errno is still the first (and, since we stop here, only)
		// one recorded, so the field name and the restore below are unchanged.
		if (!worldlistPathComponentSafe(n)) {
			first_errno = EINVAL;
			break;
		}

		const int need = snprintf(path + base_len, cap - base_len, "/%s", n);
		if (need < 0 || (size_t)need >= cap - base_len) {
			path[base_len] = '\0';
			first_errno = ENAMETOOLONG;
			break;
		}

		if (!worldlistRmTree(path, cap, depth - 1)) {
			first_errno = errno ? errno : EIO;
			path[base_len] = '\0';
			break;
		}
		path[base_len] = '\0';   // back to the parent for the next entry, always
	}
	closedir(d);

	if (first_errno) {
		errno = first_errno;
		return false;
	}

	// Only now, with every child gone, does the directory itself go. rmdir on a directory that
	// is not empty fails, so this is also the last check that the loop above really finished.
	return rmdir(path) == 0;
}

bool worldlistDelete(const char* root, const char* name)
{
	if (!root || !*root) {
		errno = EINVAL;
		return false;
	}

	// Traversal guard FIRST, validation second. Order is not load-bearing today (neither can
	// pass what the other rejects) and is written this way anyway so the cheaper, blunter,
	// never-going-to-be-relaxed check is the one nothing gets past.
	if (!worldlistPathComponentSafe(name) || !worldlistNameValid(name)) {
		errno = EINVAL;
		return false;
	}

	char path[WORLDLIST_PATH_MAX];   // the ONE buffer the whole recursive walk shares
	const int n = snprintf(path, sizeof(path), "%s/%s", root, name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return false;
	}

	// Deleting something that is not there is reported as a failure, not as a quiet success.
	// The world-select row that triggers this has just been drawn from a listing of this exact
	// directory, so "it is not there" means the listing and the card disagree, and a screen
	// that silently does nothing is a worse answer to that than one that says so.
	struct stat st;
	if (stat(path, &st) != 0) return false;   // errno is stat's
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return false;
	}

	return worldlistRmTree(path, sizeof(path), WORLDLIST_DELETE_MAX_DEPTH);
}

bool worldlistRename(const char* root, const char* old_name, const char* new_name)
{
	if (!root || !*root) {
		errno = EINVAL;
		return false;
	}

	if (!worldlistPathComponentSafe(old_name) || !worldlistNameValid(old_name) ||
	    !worldlistPathComponentSafe(new_name) || !worldlistNameValid(new_name)) {
		errno = EINVAL;
		return false;
	}

	char from[WORLDLIST_PATH_MAX];
	char to[WORLDLIST_PATH_MAX];
	const int nf = snprintf(from, sizeof(from), "%s/%s", root, old_name);
	const int nt = snprintf(to,   sizeof(to),   "%s/%s", root, new_name);
	if (nf < 0 || (size_t)nf >= sizeof(from) || nt < 0 || (size_t)nt >= sizeof(to)) {
		errno = ENAMETOOLONG;
		return false;
	}

	struct stat st;
	if (stat(from, &st) != 0) return false;   // errno is stat's
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return false;
	}

	// The check the header is blunt about: a destination that already exists is refused here,
	// on every platform, rather than left to whatever rename() would have done with it.
	if (stat(to, &st) == 0) {
		errno = EEXIST;
		return false;
	}

	// A directory rename, never a copy-and-delete: it is one metadata write instead of
	// rewriting every region file in the world, and it cannot half-finish and leave two
	// partial copies of a save on the card.
	return rename(from, to) == 0;
}

// ── The world-select screen's "press DELETE again to confirm" latch ───────────────────

// The +1 encoding (see the struct in the header): 0 is "off", so reset and zero-init agree.
void worldlistConfirmReset(WorldDeleteConfirm* c)
{
	if (!c) return;
	c->armed_plus1 = 0;
	c->frames_left = 0;
}

void worldlistConfirmTrack(WorldDeleteConfirm* c, int sel)
{
	if (!c || c->armed_plus1 == 0) return;

	// The cursor left the armed row (or there is no row at all): drop it.
	if (sel < 0 || c->armed_plus1 != sel + 1) {
		worldlistConfirmReset(c);
		return;
	}

	// Same row, one more frame spent. The frame that spends the last one disarms — a window
	// that could go negative and stay armed is exactly the "forgotten Y" the constant exists
	// to bound.
	c->frames_left--;
	if (c->frames_left <= 0) worldlistConfirmReset(c);
}

bool worldlistConfirmPress(WorldDeleteConfirm* c, int sel)
{
	if (!c || sel < 0) return false;

	if (c->armed_plus1 == sel + 1) {
		// Consumed on use: the latch does not stay armed after it fires, so holding DELETE
		// down through a list that has just shifted under it cannot delete twice.
		worldlistConfirmReset(c);
		return true;
	}

	// Includes the dangerous case this whole type exists for — armed for a DIFFERENT world.
	// It re-arms for the world actually selected now and reports "not confirmed", so the
	// player still has to press again for the world they are looking at.
	c->armed_plus1 = sel + 1;
	c->frames_left = WORLDLIST_CONFIRM_FRAMES;
	return false;
}

bool worldlistConfirmArmedFor(const WorldDeleteConfirm* c, int sel)
{
	return c && sel >= 0 && c->armed_plus1 == sel + 1;
}

// ── The world-select screen's rename/delete glue, host-testable half ──────────────────

WorldlistUiAction worldlistUiStep(WorldDeleteConfirm* c, int sel, int world_count,
                                  bool rename_press, bool delete_press, bool back_press)
{
	if (!c) return WORLDLIST_UI_NONE;

	// NEW WORLD / BACK (and a cursor that has not been placed yet) are "no world": the latch
	// is tracked against WORLDLIST_CONFIRM_NONE so that moving off an armed row onto a button
	// disarms it exactly like moving onto another world does.
	const bool on_world = sel >= 0 && sel < world_count;
	worldlistConfirmTrack(c, on_world ? sel : WORLDLIST_CONFIRM_NONE);

	if (back_press) {
		// B always ends the frame's processing: either it cancels an armed delete (and is
		// consumed so the screen does not also leave), or it is the screen's BACK and nothing
		// destructive may piggy-back on the same press.
		if (!on_world || !worldlistConfirmArmedFor(c, sel)) return WORLDLIST_UI_NONE;
		worldlistConfirmReset(c);
		return WORLDLIST_UI_DELETE_CANCELLED;
	}

	if (!on_world) return WORLDLIST_UI_NONE;

	if (rename_press) {
		worldlistConfirmReset(c);
		return WORLDLIST_UI_RENAME_PROMPT;
	}

	if (delete_press)
		return worldlistConfirmPress(c, sel) ? WORLDLIST_UI_DELETE_FIRE : WORLDLIST_UI_DELETE_ARMED;

	return WORLDLIST_UI_NONE;
}

static int worldlistIndexOf(const WorldEntry* worlds, int count, const char* name)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(worlds[i].name, name)) return i;
	return -1;
}

// The cursor's landing row after the list has been rescanned under it: the same index when
// it still names a world (the next world alphabetically slid into the slot), else the last
// world, else 0 — which on the screen is the NEW WORLD button of an empty list.
static int worldlistClampSel(int sel, int count)
{
	if (sel >= 0 && sel < count) return sel;
	return count > 0 ? count - 1 : 0;
}

WorldlistRenameResult worldlistRenameAt(const char* root, WorldEntry* worlds, int cap,
                                        int* count, bool* truncated, int* sel,
                                        WorldDeleteConfirm* c, const char* new_name)
{
	if (!root || !worlds || cap <= 0 || !count || !sel) return WORLDLIST_RENAME_NO_WORLD;
	if (*sel < 0 || *sel >= *count) return WORLDLIST_RENAME_NO_WORLD;

	// Both gates, both before touching the disk: the same pair worldlistRename applies, so a
	// name that is refused here would have been refused there anyway — this is what lets the
	// screen name the reason ("bad name") instead of relaying an errno.
	if (!worldlistPathComponentSafe(new_name) || !worldlistNameValid(new_name))
		return WORLDLIST_RENAME_BAD_NAME;

	const char* old_name = worlds[*sel].name;
	if (!strcmp(old_name, new_name)) return WORLDLIST_RENAME_UNCHANGED;

	if (worldlistIndexOf(worlds, *count, new_name) >= 0) return WORLDLIST_RENAME_DUPLICATE;

	if (!worldlistRename(root, old_name, new_name))
		return errno == EEXIST ? WORLDLIST_RENAME_DUPLICATE : WORLDLIST_RENAME_FAILED;

	// The disk changed; the list is re-read from it rather than patched, so the row order is
	// the one the player will see the next time the screen is entered, and the cursor is put
	// back on the world it was on — under its new name, wherever the sort moved it.
	*count = worldlistScan(root, worlds, cap, truncated);
	const int idx = worldlistIndexOf(worlds, *count, new_name);
	*sel = idx >= 0 ? idx : worldlistClampSel(*sel, *count);
	worldlistConfirmReset(c);
	return WORLDLIST_RENAME_OK;
}

const char* worldlistRenameResultText(WorldlistRenameResult r)
{
	switch (r) {
	case WORLDLIST_RENAME_BAD_NAME:  return "bad name: use A-Z 0-9 _ - only";
	case WORLDLIST_RENAME_DUPLICATE: return "name already used";
	case WORLDLIST_RENAME_FAILED:    return "rename failed (sd write)";
	case WORLDLIST_RENAME_OK:
	case WORLDLIST_RENAME_UNCHANGED:
	case WORLDLIST_RENAME_NO_WORLD:
	default:                         return NULL;
	}
}

bool worldlistDeleteAt(const char* root, WorldEntry* worlds, int cap, int* count,
                       bool* truncated, int* sel, WorldDeleteConfirm* c)
{
	if (!root || !worlds || cap <= 0 || !count || !sel || *sel < 0 || *sel >= *count) {
		errno = EINVAL;
		return false;
	}

	const bool ok = worldlistDelete(root, worlds[*sel].name);
	const int  delete_errno = errno;   // worldlistScan below must not get to overwrite this

	// v1.9.x audit fix. Rescanned on BOTH outcomes now, not only success: worldlistRmTree's
	// fail-closed walk (see its own comment) can still have removed part of a tree before the
	// failure that made `ok` false, so a list frozen at its pre-attempt contents would show this
	// row exactly as it stood before the delete was ever attempted — an intact-looking row over
	// a directory that, on disk, is no longer what it was. That is precisely the "delete failed"
	// -plus-stale-list report the audit caught, and it is what let a half-gutted world look
	// untouched right up until the player reloaded it. The rescan cannot turn a failed delete
	// into a successful one: the row survives the rescan iff root/name is still a directory,
	// gutted or not, and the caller's errno-bearing status line (title.c) is what tells the
	// player it did not finish cleanly — not a frozen list pretending nothing happened.
	*count = worldlistScan(root, worlds, cap, truncated);
	*sel = worldlistClampSel(*sel, *count);
	worldlistConfirmReset(c);

	errno = delete_errno;
	return ok;
}
