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

// ── v1.9.0 item 6.3: rename and delete ────────────────────────────────────────────────
//
// Both take `root` as their first parameter, exactly as worldlistScan and worldlistCreate
// above already do, rather than reaching for world/region.h's REGION_ROOT themselves. Two
// reasons, and the first one is the load-bearing one: REGION_ROOT is "sdmc:/blocksmith/worlds",
// a devoptab path that exists on a console and nowhere else, so a version of these that
// hard-coded it could not be pointed at a host temp directory and therefore could not be
// tested at all — and a recursive directory removal is the last function in this codebase that
// should ship untested. The second is simply that a module with three functions taking `root`
// and two not taking it would be inviting the next reader to guess which convention is the
// real one.

// A second, INDEPENDENT line of defence against path traversal, deliberately not folded into
// worldlistNameValid: `name` is safe to join onto a directory path as exactly one component.
// Rejects NULL/empty, "." and "..", anything containing '/', '\\' or ':', and any embedded
// ".." at all.
//
// worldlistNameValid's charset already excludes every one of those characters, so on today's
// tree nothing can reach this that NameValid would have let through. That redundancy is the
// point, and it is the same reasoning worldlistNameValid's own comment gives for keeping its
// explicit "."/".." strcmp: if the allowed charset ever grows to include '.' or '/', this is
// what still stands between a name and an rmdir somewhere else on the card.
//
// It is exported rather than left static because a guard that no test can reach on its own is
// a guard nobody can prove still works — the only way to sabotage-test this layer in isolation
// is to be able to call it in isolation. worldlistRmTree also calls it on every name readdir()
// hands back, which is the case where it is NOT redundant: a directory entry on the card was
// not necessarily written by this build.
bool worldlistPathComponentSafe(const char* name);

// How many directory levels below `root`/`name` worldlistDelete will descend before it
// refuses. Deliberately small.
//
// The 3DS default thread stack is 32 KiB (app/worker.c pins its own worker at exactly that,
// WORKER_STACK_BYTES, for the same reason), and a recursive tree walk that declares a path
// buffer per level is the classic way to run one out with no crash and no message. So the
// walk here declares its 512-byte path buffer ONCE, in worldlistDelete, and every level below
// appends to and truncates that one buffer instead of holding its own. What is left in a
// recursion frame is a DIR*, a `struct dirent*`, a `const char*`, one size_t, two ints and one
// `struct stat`. REASONED, not measured: nothing in the tree prints an ARM frame size. Sizing
// it from the types — newlib's `struct stat` is on the order of a hundred bytes and the rest
// are word-sized — puts a level under 256 bytes even with spill slots, so the whole 8-level
// budget is under 2 KiB, under 7% of the smallest stack this can run on. What the test DOES
// pin is only that the bound sits where this says (tests/worldlist_ops_test.c's
// testDeleteDepthBound), not how much each level costs.
//
// 8 rather than 2: a world directory this build writes is FLAT — region files and
// registry.bin sit directly in it, one level down — so 8 is four times deeper than anything
// the game itself can create, and still bounded. Past 8 the walk stops, sets errno to ELOOP
// (the nearest standard "this tree is too deep to follow" errno; there is no better one) and
// reports failure, leaving that subtree in place. It never silently deletes a partial tree
// and calls it success.
#define WORLDLIST_DELETE_MAX_DEPTH 8

// Recursively removes `root`/`name` and everything under it. THIS IS IRREVERSIBLE: it is the
// only function in this codebase that destroys a player's save, so read the guards before
// calling it.
//
// Refuses, touching nothing, when: `root` is NULL or empty; `name` fails
// worldlistPathComponentSafe; `name` fails worldlistNameValid; the joined path does not fit
// the 512-byte buffer; `root`/`name` does not exist; or it exists but is not a directory.
// Every one of those sets errno (EINVAL, ENAMETOOLONG, ENOTDIR, or whatever stat() left) and
// returns false.
//
// Returns true only when `root`/`name` is gone. A partial failure anywhere in the tree — an
// entry that cannot be removed, a name readdir() returned that is not a safe single component,
// a level past WORLDLIST_DELETE_MAX_DEPTH — leaves the parent directory in place, restores
// errno to the FIRST failure's value (not the last syscall's), and returns false. Nothing is
// swallowed: a caller that gets false can print errno and know which of those it was.
//
// It never leaves the subtree rooted at `root`/`name`. "." and ".." are skipped, every other
// name is re-checked with worldlistPathComponentSafe before it is joined on, and the join
// itself is bounds-checked. FAT32 (the card) has no symlinks for it to be led out through;
// on a host filesystem that does have them, stat() follows them and this would delete a
// link's TARGET rather than the link — untested, and not a case the console can produce.
bool worldlistDelete(const char* root, const char* name);

// Renames `root`/`old_name` to `root`/`new_name`. Both names go through
// worldlistPathComponentSafe and worldlistNameValid, so `new_name` is held to exactly the
// standard worldlistCreate holds a typed-in name to.
//
// REFUSES IF THE DESTINATION ALREADY EXISTS (errno EEXIST), and that check is not decoration.
// The platform call underneath is plain rename(2), and what rename() does to an existing
// destination is not the same answer on the three filesystems this code runs on: POSIX
// replaces an EMPTY destination directory silently and fails with ENOTEMPTY on a non-empty
// one; Win32/MinGW fails outright; libctru's sdmc devoptab reaches FSUSER_RenameDirectory,
// which also fails. So without this check the behaviour would be "usually refuses, but
// silently eats an empty world on a POSIX host" — and an empty world directory is exactly
// what a freshly created, never-played world is. The explicit stat() makes all three
// platforms refuse, which is the only answer that never loses a save.
//
// Not case-insensitivity-aware: on FAT32 "alpha" and "Alpha" are the same directory, so the
// destination stat() succeeds and a case-only rename is refused rather than performed. Known,
// accepted — the alternative is a case-folding compare that would be wrong on any host
// filesystem that is case-sensitive.
//
// True only when `root`/`new_name` exists and `root`/`old_name` does not. False leaves both
// sides exactly as they were, with errno set.
bool worldlistRename(const char* root, const char* old_name, const char* new_name);

// ── The world-select screen's "press DELETE again to confirm" latch ───────────────────
//
// This lives here, not in scene/title.c, for one reason: title.c includes <3ds.h> and cannot
// be linked outside a devkitARM build, so a confirm rule written there could never be tested,
// and the specific bug this rule exists to prevent — arming deletion of world A, moving the
// cursor to world B, and having B be one button press from deletion — is a save-destroying
// bug that a human is unlikely to catch by looking at the emulator. Same split, same reasoning,
// as scene/title_nav.h (the multiplayer back/enter rule) one file over.
//
// It is deliberately NOT a general confirm-dialog framework. There is no precedent for one
// anywhere in this codebase and nobody asked for one; this is two ints.
#define WORLDLIST_CONFIRM_NONE (-1)

// How many frames an armed delete stays armed with nothing pressed, before it drops on its
// own. 300 is ~5 s at the project's measured 59.83 fps (see main.c) — long enough to read
// "PRESS Y AGAIN TO DELETE <name>" and decide, short enough that a Y pressed by accident, then
// forgotten, is not still one press from destroying a save a minute later when the player
// comes back to the screen. Counted in worldlistConfirmTrack, which the screen already calls
// once per frame, so there is no second per-frame hook to forget.
#define WORLDLIST_CONFIRM_FRAMES 300

typedef struct {
	// (armed world index + 1), or 0 for disarmed. Stored PLUS ONE on purpose: it makes the
	// all-zero struct — what memset(ts, 0) in titleInit produces, and what a forgotten
	// worldlistConfirmReset leaves behind — mean "off" rather than "armed for row 0". The
	// first cut of this stored the index directly with -1 as the sentinel, and a zeroed
	// TitleState was then one Y press from deleting the first world in the list; that trap is
	// now impossible by representation, and tests/worldlist_ops_test.c pins it. Never read
	// directly by the UI — the functions below are the whole interface, so the "armed for
	// WHICH selection" half can never be forgotten at a call site.
	int armed_plus1;
	// Frames left before the arm expires; meaningful only while armed_plus1 != 0.
	int frames_left;
} WorldDeleteConfirm;

// Disarms. Call on entering the world-select screen, on B, and after anything that changes
// what the list holds (a delete, a rename, a rescan). Equivalent to zeroing the struct.
void worldlistConfirmReset(WorldDeleteConfirm* c);

// Call once per frame with the world index the DELETE button would act on (or
// WORLDLIST_CONFIRM_NONE when there is no world to act on). Disarms if that is not the
// selection the latch was armed for. This is what stops an armed confirm from following the
// cursor onto a different world — and what keeps the on-screen prompt from naming a world the
// next press would not actually delete. It is also the clock: each call while armed spends
// one of WORLDLIST_CONFIRM_FRAMES, and the call that spends the last one disarms.
void worldlistConfirmTrack(WorldDeleteConfirm* c, int sel);

// One press of DELETE on world `sel`. Returns true ONLY on the second consecutive press for
// the same `sel` inside the window — that is the confirmation, and it consumes the latch, so a
// third press arms again rather than deleting a second time. The first press (or any press for
// a `sel` the latch is not armed for, which includes the moved-cursor and the expired cases
// above) arms for WORLDLIST_CONFIRM_FRAMES and returns false.
bool worldlistConfirmPress(WorldDeleteConfirm* c, int sel);

// True when the next worldlistConfirmPress(c, sel) would delete. The UI asks this to decide
// whether to draw the confirm prompt, so the prompt and the behaviour cannot disagree.
bool worldlistConfirmArmedFor(const WorldDeleteConfirm* c, int sel);

// ── The world-select screen's rename/delete glue, host-testable half ──────────────────
//
// scene/title.c reads the buttons, opens the keyboard and draws; every DECISION lives here:
// which row a press acts on, whether DELETE arms or fires, whether B is a cancel or a back,
// what a typed-in rename is refused for, and where the cursor lands once the list has changed
// underneath it. Same split, same reason, as the latch above and as scene/title_nav.h —
// title.c includes <3ds.h> and cannot be linked on the host, and every one of these rules is
// a save-destroying bug if it is wrong. tests/worldlist_ops_test.c scripts a press sequence
// through worldlistUiStep and drives worldlistRenameAt/worldlistDeleteAt against a real
// directory tree.

typedef enum {
	WORLDLIST_UI_NONE = 0,          // nothing for the screen to do this frame
	WORLDLIST_UI_RENAME_PROMPT,     // open the keyboard pre-filled with worlds[sel].name
	WORLDLIST_UI_DELETE_ARMED,      // first DELETE press: draw the "press again" prompt
	WORLDLIST_UI_DELETE_FIRE,       // second DELETE press on the same row: delete it NOW
	// B while a delete was armed: disarmed, and B is CONSUMED — the screen must not also
	// treat it as "back". B with nothing armed comes back as WORLDLIST_UI_NONE and is the
	// screen's to act on exactly as before.
	WORLDLIST_UI_DELETE_CANCELLED,
} WorldlistUiAction;

// One frame of the world-select screen. `sel` is the D-pad cursor AFTER this frame's
// Up/Down have been applied, `world_count` the number of world rows; a `sel` outside
// [0, world_count) — the NEW WORLD / BACK buttons — is "no world", so RENAME/DELETE do
// nothing there and any armed latch is dropped (worldlistConfirmTrack). Order of precedence
// when two land on one frame: back (cancel) first, then rename, then delete — the
// non-destructive answer always wins. RENAME_PROMPT also disarms: a modal keyboard is about
// to take the screen, and a delete must not stay one press away underneath it.
WorldlistUiAction worldlistUiStep(WorldDeleteConfirm* c, int sel, int world_count,
                                  bool rename_press, bool delete_press, bool back_press);

typedef enum {
	WORLDLIST_RENAME_OK,          // renamed on disk; list rescanned; *sel follows the world
	WORLDLIST_RENAME_UNCHANGED,   // the world's own name typed back: nothing done, no message
	WORLDLIST_RENAME_NO_WORLD,    // *sel is not a world row: nothing done
	WORLDLIST_RENAME_BAD_NAME,    // fails worldlistPathComponentSafe or worldlistNameValid
	WORLDLIST_RENAME_DUPLICATE,   // another world already has that name (listed, or on disk)
	WORLDLIST_RENAME_FAILED,      // worldlistRename refused for any other reason; errno says why
} WorldlistRenameResult;

// Renames worlds[*sel] to `new_name` under `root`, then rescans `root` into worlds[0..cap)
// (worldlistScan — so the list is re-sorted, exactly as entering the screen would sort it)
// and moves *sel onto the renamed world's NEW row, wherever the sort put it. Anything but
// WORLDLIST_RENAME_OK leaves the disk, the list and *sel exactly as they were; `c` is reset
// after a successful rename because the rows it was armed against no longer mean the same
// thing (may be NULL).
//
// `new_name` is refused by exactly the gate worldlistCreate holds a typed-in name to —
// worldlistPathComponentSafe AND worldlistNameValid — and then by a case-sensitive scan of
// the list; worldlistRename's own stat() refusal (EEXIST) is folded into DUPLICATE too, which
// is what catches a FAT32 case-only collision and a directory the listing did not show.
WorldlistRenameResult worldlistRenameAt(const char* root, WorldEntry* worlds, int cap,
                                        int* count, bool* truncated, int* sel,
                                        WorldDeleteConfirm* c, const char* new_name);

// The status-line text for a result, in scene/title.c's existing message idiom, or NULL when
// there is nothing to say (OK, UNCHANGED, NO_WORLD).
const char* worldlistRenameResultText(WorldlistRenameResult r);

// Deletes worlds[*sel] from disk (worldlistDelete), then ALWAYS rescans `root` into
// worlds[0..cap) — on success because the deleted world is gone and the list must say so, and
// on failure (v1.9.x audit fix) because worldlistRmTree's fail-closed walk (see its own
// comment) can still have removed part of the tree before the failure that stopped it, so a
// list frozen at its pre-attempt contents would show an intact-looking row over a directory
// that is no longer what it was. *sel follows the rescan the same way on both outcomes
// (worldlistClampSel — the next world alphabetically, or the last row, or 0 for an empty
// list). `c` is reset (may be NULL) either way too, though in practice worldlistUiStep has
// already consumed it by the time this runs.
//
// False with errno set — *sel not a world row (EINVAL), or whatever worldlistDelete refused
// for — still leaves the row (or whatever survives of it) visible after the rescan: the
// failure is reported through the caller's errno-bearing status line, not by freezing the list
// at a state the disk may no longer match.
bool worldlistDeleteAt(const char* root, WorldEntry* worlds, int cap, int* count,
                       bool* truncated, int* sel, WorldDeleteConfirm* c);
