// Host self-test for v1.9.0 item 6.3: scene/worldlist.c's worldlistDelete, worldlistRename,
// worldlistPathComponentSafe, and the world-select screen's delete-confirm latch.
//
// SUCCESS CRITERION this file was written against, stated before the implementation was, as
// observable facts on a real directory tree rather than as return codes:
//
//   1. After worldlistDelete(root, "alpha"), the directory root/alpha and every file and
//      subdirectory nested inside it no longer stat(); a SIBLING world root/beta and its file
//      still do; and root itself still does.
//   2. worldlistDelete refuses and deletes NOTHING for: a name carrying a path separator or
//      "..", a name worldlistNameValid rejects (asserted against a directory that really
//      exists on disk under that name, so a refusal that "worked" only because the target was
//      missing cannot pass), a missing name, and a name that is a file.
//   3. After worldlistRename(root, "alpha", "gamma"), root/gamma holds alpha's file with
//      alpha's CONTENT and root/alpha is gone; and when the destination already exists —
//      including the empty-directory case POSIX rename() would silently replace, and a name
//      that collides with an existing world by CASE ONLY on this project's own (case-insensitive
//      drvfs-mounted) host build — the rename is refused with EEXIST and BOTH directories and
//      both their contents untouched; a name that merely resembles an existing world without
//      case-folding to it must still succeed.
//   4. The confirm latch never lets a selection change carry an armed confirm onto a different
//      world: arm on A, move to B, and B is still two presses from deletion, not one.
//   5. An all-zero WorldDeleteConfirm — a memset TitleState with no worldlistConfirmReset — is
//      NOT armed for row 0: its first press arms, never fires. And an arm left alone expires
//      after exactly WORLDLIST_CONFIRM_FRAMES ticks of worldlistConfirmTrack.
//   6. worldlistUiStep hands the screen the right verb for a scripted press sequence: B with a
//      delete armed is a consumed cancel, not a back; rename disarms; a press on the NEW WORLD
//      or BACK row does nothing destructive.
//   7. After worldlistRenameAt the list is re-sorted from disk and *sel sits on the renamed
//      world's NEW row; every refusal (bad name, duplicate in the list, duplicate on disk that
//      the list did not show, unchanged, no world) leaves disk, list and *sel untouched.
//   8. After worldlistDeleteAt the row is gone from disk and from the list, and *sel lands on
//      the row that slid into its place, the last row, or 0 for an empty list. v1.9.x audit
//      fix: a delete that FAILS now rescans too, so the list reflects the disk exactly as
//      worldlistRmTree's fail-closed walk left it (see criterion 9), not a frozen snapshot of
//      before the attempt.
//   9. v1.9.x audit fix. worldlistRmTree stops the instant one entry cannot be removed rather
//      than finishing the rest of the directory first: a delete that cannot remove everything
//      removes nothing MORE than it already had at the moment the failure was discovered. The
//      entry that failed, and everything nested under it, is always still there afterwards —
//      that much is true regardless of directory read order, which POSIX leaves unspecified and
//      this suite does not control. What is NOT claimed, because it cannot be without pinning
//      readdir() order: which of that entry's UNVISITED siblings also survive.
//
// Every check below asserts WORLD STATE — what stat() and fread() say about the tree — and not
// merely what the function returned. That is deliberate and it is the standard this project
// arrived at the hard way: a red arm elsewhere in this tree had four separate counter and flag
// assertions all stay green while the thing they were counting had vanished, and only a direct
// state assertion caught it.
//
// This file lives under tests/, NOT under source/. Makefile:26 globs every .c under source/
// into the CONSOLE build, so a _test.c placed there needs a #ifndef __3DS__ around its whole
// body or the console link dies on "multiple definition of `main'". tests/ is exempt from that
// glob, which is why every suite from v1.8.18 onward goes here instead — see
// tests/monster_test.c's own note on the same asymmetry. No guard is therefore needed or
// present.
//
// The CHECK macro, the check-count pin and the PASS/FAIL summary line are copied in the same
// shape world/inventory_test.c and world/worldlist_test.c use, so a failure here reads the way
// a failure anywhere else in this codebase does.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// rmdir/getpid, for this file's own setup and teardown — the same host/host split
// world/worldlist_test.c already documents. MinGW supplies the non-underscore spellings as
// oldname aliases; without the include, -Werror stops the build on an implicit declaration.
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "scene/worldlist.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// How many CHECK()s this suite makes on a healthy tree. A LITERAL, measured from a real run,
// never derived from anything the code under test can move — the same rule and the same
// reasoning world/inventory_test.c's INVENTORY_TEST_EXPECTED_CHECKS spells out at length.
//
// The hazard is real here rather than theoretical: testDeleteDepthBound loops to
// WORLDLIST_DELETE_MAX_DEPTH, so shrinking that production constant would stop checks RUNNING
// rather than start them failing, and a suite that only reports failures would call that a
// pass.
//
// Measured, not summed. The first run printed:
//
//   CHECK COUNT: 41 check(s) were ADDED - expected 80, ran 121.
//
// Re-pinned when criteria 5-8 landed (the zero-init trap, the expiry window, worldlistUiStep,
// worldlistRenameAt/DeleteAt). 300 of the new checks are testUiStep's frame-by-frame expiry
// loop — one CHECK per idle frame — which is what makes WORLDLIST_CONFIRM_FRAMES a second
// production constant this pin guards. That run printed:
//
//   CHECK COUNT: 430 check(s) were ADDED - expected 121, ran 551.
//
// Re-pinned again for the v1.9.x audit fix (worldlistRmTree fail-closed, worldlistDeleteAt
// rescanning on failure): criterion 9's testDeleteStopsOnFirstFailure added 26 checks, and the
// "removed behind its back" case inside testDeleteAt (criterion 8) went from 4 checks to 5 now
// that a failed delete rescans instead of freezing the list. That run printed:
//
//   CHECK COUNT: 27 check(s) were ADDED - expected 551, ran 578.
//
// Re-pinned again for the case-only collision coverage gap: nothing exercised worldlistRename's
// destination check (lines 335-338 of scene/worldlist.c) against a name that collides with an
// existing world by case only rather than by exact match. testRenameRefusesCaseOnlyCollision
// added 17 checks -- the refusal, its errno, the untouched state of both existing worlds, a
// rescan proving no third entry appeared, and the "Homer" near miss proving the same check does
// not simply refuse every name that starts with "home". "home" is left EMPTY on purpose (the
// same reason testRenameRefusesExistingDestination's "dstempty" is): an empty destination is
// the one case a bare rename() would silently replace instead of refuse, so it is the only
// construction that actually exercises line 335's stat() check rather than being refused for a
// different reason (ENOTEMPTY) if the sabotage below ever regresses this guard. That run
// printed:
//
//   CHECK COUNT: 17 check(s) were ADDED - expected 578, ran 595.
#define WORLDLIST_OPS_TEST_EXPECTED_CHECKS 595

// Deliberately NOT routed through CHECK(): it must not perturb the number it is testing.
static void checkCountPin(void)
{
	if (s_checks == WORLDLIST_OPS_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < WORLDLIST_OPS_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran: a loop bound shrank, an early return or\n"
		       "  a continue fired, or a CHECK was deleted. The checks that did run passing\n"
		       "  tells you nothing about the ones that did not. Find them. Do NOT re-pin\n"
		       "  WORLDLIST_OPS_TEST_EXPECTED_CHECKS to go green.\n",
		       WORLDLIST_OPS_TEST_EXPECTED_CHECKS - s_checks,
		       WORLDLIST_OPS_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         WORLDLIST_OPS_TEST_EXPECTED_CHECKS - s_checks,
			         WORLDLIST_OPS_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set WORLDLIST_OPS_TEST_EXPECTED_CHECKS in\n"
		       "  tests/worldlist_ops_test.c to %d. If you did not, something is running\n"
		       "  checks more times than it should.\n",
		       s_checks - WORLDLIST_OPS_TEST_EXPECTED_CHECKS,
		       WORLDLIST_OPS_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - WORLDLIST_OPS_TEST_EXPECTED_CHECKS,
			         WORLDLIST_OPS_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// ── The tree this suite builds and inspects ───────────────────────────────────────────
//
// Everything lives under build-host/, not under a system temp directory, for the reason every
// other stanza in tools/run_host_tests.sh already gives: build-host is what the suite cleans
// up and what a failed run leaves behind on purpose for inspection. SANDBOX carries this
// process's PID so two suites running at once cannot share a tree — the same $$ rule the shell
// script uses for its binaries.
//
// SANDBOX is deliberately one level ABOVE the worlds root. That is what makes the traversal
// checks mean anything: "root/../outside" resolves to something real that this suite can then
// assert is still there.
static char s_sandbox[192];
static char s_root[224];

static void hostMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

// Joins into a caller-supplied buffer. Every path in this file is built through this so a
// typo in a separator cannot make a check quietly assert about the wrong file.
static void pathJoin(char* out, size_t cap, const char* a, const char* b)
{
	snprintf(out, cap, "%s/%s", a, b);
}

static bool hostExists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static bool hostIsDir(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void hostWriteFile(const char* path, const char* text)
{
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fwrite(text, 1, strlen(text), f);
	fclose(f);
}

// True only if the file exists AND holds exactly `text`. Content, not existence: a rename that
// moved a directory but lost what was in it would pass an existence check and fail this one.
static bool hostFileHas(const char* path, const char* text)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	char buf[128];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strcmp(buf, text) == 0;
}

// This suite's OWN recursive removal, for teardown. Deliberately not worldlistDelete: a
// teardown built out of the function under test cannot clean up after a sabotaged one, and a
// leftover tree from run N would then be the input to run N+1.
static void hostRmTree(const char* path)
{
	struct stat st;
	if (stat(path, &st) != 0) return;
	if (!S_ISDIR(st.st_mode)) {
		remove(path);
		return;
	}

	DIR* d = opendir(path);
	if (d) {
		struct dirent* ent;
		while ((ent = readdir(d)) != NULL) {
			if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
			char child[512];
			pathJoin(child, sizeof(child), path, ent->d_name);
			hostRmTree(child);
		}
		closedir(d);
	}
	rmdir(path);
}

// Makes `s_root`/`name` and returns its full path in `out`.
static void makeWorld(char* out, size_t cap, const char* name)
{
	pathJoin(out, cap, s_root, name);
	hostMkdir(out);
}

// ── Delete ────────────────────────────────────────────────────────────────────────────

// Criterion 1. The sibling assertion at the end is the one that catches a traversal bug: a
// delete that walked out of its own subtree takes beta with it.
static void testDeleteRemovesWholeTreeAndLeavesSiblingAlone(void)
{
	char alpha[320], alpha_file[384], alpha_sub[384], alpha_deep[448];
	char beta[320], beta_file[384];

	makeWorld(alpha, sizeof(alpha), "alpha");
	pathJoin(alpha_file, sizeof(alpha_file), alpha, "a.txt");
	hostWriteFile(alpha_file, "alpha-data");
	pathJoin(alpha_sub, sizeof(alpha_sub), alpha, "sub");
	hostMkdir(alpha_sub);
	pathJoin(alpha_deep, sizeof(alpha_deep), alpha_sub, "deep.txt");
	hostWriteFile(alpha_deep, "deep-data");

	makeWorld(beta, sizeof(beta), "beta");
	pathJoin(beta_file, sizeof(beta_file), beta, "b.txt");
	hostWriteFile(beta_file, "beta-data");

	// Preconditions. Without these the "it is gone" checks below could pass on a tree that was
	// never built — a check that cannot go red proves nothing.
	CHECK(hostIsDir(alpha));
	CHECK(hostFileHas(alpha_file, "alpha-data"));
	CHECK(hostFileHas(alpha_deep, "deep-data"));
	CHECK(hostFileHas(beta_file, "beta-data"));

	CHECK(worldlistDelete(s_root, "alpha") == true);

	CHECK(!hostExists(alpha));
	CHECK(!hostExists(alpha_file));
	CHECK(!hostExists(alpha_sub));
	CHECK(!hostExists(alpha_deep));

	// The sibling, and the root itself, must be exactly as they were.
	CHECK(hostIsDir(beta));
	CHECK(hostFileHas(beta_file, "beta-data"));
	CHECK(hostIsDir(s_root));

	hostRmTree(beta);
}

// Criterion 2, traversal half. Every name here would, if joined naively, reach outside the
// worlds root — and s_sandbox/outside is a real directory this then asserts is still there.
static void testDeleteRefusesTraversalAndTouchesNothing(void)
{
	char outside[320], outside_file[384], victim[320], victim_file[384];

	pathJoin(outside, sizeof(outside), s_sandbox, "outside");
	hostMkdir(outside);
	pathJoin(outside_file, sizeof(outside_file), outside, "o.txt");
	hostWriteFile(outside_file, "outside-data");

	makeWorld(victim, sizeof(victim), "victim");
	pathJoin(victim_file, sizeof(victim_file), victim, "v.txt");
	hostWriteFile(victim_file, "victim-data");

	CHECK(hostFileHas(outside_file, "outside-data"));

	errno = 0;
	CHECK(worldlistDelete(s_root, "..") == false);
	CHECK(errno == EINVAL);
	CHECK(worldlistDelete(s_root, "../outside") == false);
	CHECK(worldlistDelete(s_root, "..\\outside") == false);
	CHECK(worldlistDelete(s_root, "sub/victim") == false);
	CHECK(worldlistDelete(s_root, "a..b") == false);
	CHECK(worldlistDelete(s_root, ".") == false);
	CHECK(worldlistDelete(s_root, "sdmc:") == false);
	CHECK(worldlistDelete(s_root, NULL) == false);
	CHECK(worldlistDelete(NULL, "victim") == false);
	CHECK(worldlistDelete("", "victim") == false);

	// The state assertions. Return codes above say the calls refused; these say nothing was
	// destroyed on the way to refusing.
	CHECK(hostIsDir(outside));
	CHECK(hostFileHas(outside_file, "outside-data"));
	CHECK(hostIsDir(victim));
	CHECK(hostFileHas(victim_file, "victim-data"));
	CHECK(hostIsDir(s_root));

	hostRmTree(outside);
	hostRmTree(victim);
}

// Criterion 2, validation half. The directories here are made with a bare mkdir rather than
// worldlistCreate precisely so they REALLY EXIST under names worldlistNameValid rejects. A
// refusal is only evidence if the thing refused was there to delete.
static void testDeleteRefusesNamesNameValidRejects(void)
{
	static const char* kBadNames[] = {
		"bad!name",      // outside [A-Za-z0-9 _-]
		" lead",         // leading whitespace
		"way_too_long_world_name_for_the_entry_buffer",   // >= WORLDLIST_NAME_MAX
	};

	char made[3][320];
	for (int i = 0; i < 3; i++) {
		makeWorld(made[i], sizeof(made[i]), kBadNames[i]);
		CHECK(hostIsDir(made[i]));
	}

	for (int i = 0; i < 3; i++) {
		CHECK(worldlistDelete(s_root, kBadNames[i]) == false);
		CHECK(hostIsDir(made[i]));   // still there: the refusal deleted nothing
	}

	for (int i = 0; i < 3; i++)
		hostRmTree(made[i]);
}

static void testDeleteRefusesMissingAndNonDirectory(void)
{
	char plain[320];

	errno = 0;
	CHECK(worldlistDelete(s_root, "nosuchworld") == false);
	CHECK(errno == ENOENT);   // errno is not swallowed: the caller can tell WHY it refused

	pathJoin(plain, sizeof(plain), s_root, "plainfile");
	hostWriteFile(plain, "not-a-world");
	errno = 0;
	CHECK(worldlistDelete(s_root, "plainfile") == false);
	CHECK(errno == ENOTDIR);
	CHECK(hostFileHas(plain, "not-a-world"));

	remove(plain);
}

// The recursion bound. Builds one tree exactly at the limit (which must delete cleanly) and
// one tree one level past it (which must refuse and leave the tree standing), so the bound is
// proven to be where the header says it is rather than merely "somewhere".
static void testDeleteDepthBound(void)
{
	char path[512], bottom[512];

	// At the limit: WORLDLIST_DELETE_MAX_DEPTH-1 nested subdirectories under the world
	// directory, plus a file below the deepest one. Files cost no depth (they are removed
	// without descending), which is why the file is legal at the bottom.
	char ok[320];
	makeWorld(ok, sizeof(ok), "okdepth");
	snprintf(path, sizeof(path), "%s", ok);
	for (int i = 0; i < WORLDLIST_DELETE_MAX_DEPTH - 1; i++) {
		char next[512];
		pathJoin(next, sizeof(next), path, "d");
		hostMkdir(next);
		snprintf(path, sizeof(path), "%s", next);
		CHECK(hostIsDir(path));
	}
	pathJoin(bottom, sizeof(bottom), path, "leaf.txt");
	hostWriteFile(bottom, "leaf");
	CHECK(hostFileHas(bottom, "leaf"));

	CHECK(worldlistDelete(s_root, "okdepth") == true);
	CHECK(!hostExists(ok));

	// One level too far.
	char deep[320];
	makeWorld(deep, sizeof(deep), "toodeep");
	snprintf(path, sizeof(path), "%s", deep);
	for (int i = 0; i < WORLDLIST_DELETE_MAX_DEPTH; i++) {
		char next[512];
		pathJoin(next, sizeof(next), path, "d");
		hostMkdir(next);
		snprintf(path, sizeof(path), "%s", next);
	}
	pathJoin(bottom, sizeof(bottom), path, "leaf.txt");
	hostWriteFile(bottom, "leaf");

	errno = 0;
	CHECK(worldlistDelete(s_root, "toodeep") == false);
	CHECK(errno == ELOOP);
	CHECK(hostIsDir(deep));            // the parent is left standing, not half-deleted
	CHECK(hostFileHas(bottom, "leaf"));   // and so is everything past the bound

	hostRmTree(deep);
}

// ── The independent traversal guard, on its own ───────────────────────────────────────
//
// Asserted directly rather than only through worldlistDelete. worldlistNameValid's charset
// already rejects everything below, so through Delete this layer is unreachable and a broken
// version of it would test green — the only way to prove this line of defence still works is
// to call it by itself.
static void testPathComponentSafe(void)
{
	CHECK(worldlistPathComponentSafe("alpha") == true);
	CHECK(worldlistPathComponentSafe("my world-1_2") == true);

	CHECK(worldlistPathComponentSafe(NULL) == false);
	CHECK(worldlistPathComponentSafe("") == false);
	CHECK(worldlistPathComponentSafe(".") == false);
	CHECK(worldlistPathComponentSafe("..") == false);
	CHECK(worldlistPathComponentSafe("a/b") == false);
	CHECK(worldlistPathComponentSafe("/abs") == false);
	CHECK(worldlistPathComponentSafe("a\\b") == false);
	CHECK(worldlistPathComponentSafe("sdmc:") == false);
	CHECK(worldlistPathComponentSafe("a..b") == false);
	CHECK(worldlistPathComponentSafe("../x") == false);
}

// ── Rename ────────────────────────────────────────────────────────────────────────────

// Criterion 3, happy path. The content check is what makes this a move rather than "a
// directory with the right name now exists".
static void testRenameMovesTheWholeTree(void)
{
	char alpha[320], alpha_file[384], alpha_sub[384], alpha_deep[448];
	char gamma[320], gamma_file[384], gamma_deep[448];

	makeWorld(alpha, sizeof(alpha), "alpha");
	pathJoin(alpha_file, sizeof(alpha_file), alpha, "a.txt");
	hostWriteFile(alpha_file, "alpha-data");
	pathJoin(alpha_sub, sizeof(alpha_sub), alpha, "sub");
	hostMkdir(alpha_sub);
	pathJoin(alpha_deep, sizeof(alpha_deep), alpha_sub, "deep.txt");
	hostWriteFile(alpha_deep, "deep-data");

	pathJoin(gamma, sizeof(gamma), s_root, "gamma");
	CHECK(!hostExists(gamma));

	CHECK(worldlistRename(s_root, "alpha", "gamma") == true);

	CHECK(!hostExists(alpha));
	CHECK(hostIsDir(gamma));
	pathJoin(gamma_file, sizeof(gamma_file), gamma, "a.txt");
	CHECK(hostFileHas(gamma_file, "alpha-data"));
	snprintf(gamma_deep, sizeof(gamma_deep), "%s/sub/deep.txt", gamma);
	CHECK(hostFileHas(gamma_deep, "deep-data"));

	hostRmTree(gamma);
}

// Criterion 3, the item-loss half. The EMPTY destination is the dangerous one: POSIX rename()
// removes an empty destination directory and replaces it, and an empty world directory is
// exactly what a freshly created, never-played world is.
static void testRenameRefusesExistingDestination(void)
{
	char src[320], src_file[384], dst_empty[320], dst_full[320], dst_full_file[384], moved[384];

	makeWorld(src, sizeof(src), "src");
	pathJoin(src_file, sizeof(src_file), src, "s.txt");
	hostWriteFile(src_file, "src-data");

	makeWorld(dst_empty, sizeof(dst_empty), "dstempty");   // deliberately left empty

	makeWorld(dst_full, sizeof(dst_full), "dstfull");
	pathJoin(dst_full_file, sizeof(dst_full_file), dst_full, "d.txt");
	hostWriteFile(dst_full_file, "dst-data");

	errno = 0;
	CHECK(worldlistRename(s_root, "src", "dstempty") == false);
	CHECK(errno == EEXIST);
	CHECK(hostIsDir(src));
	CHECK(hostFileHas(src_file, "src-data"));
	CHECK(hostIsDir(dst_empty));
	pathJoin(moved, sizeof(moved), dst_empty, "s.txt");
	CHECK(!hostExists(moved));   // nothing was moved into it

	CHECK(worldlistRename(s_root, "src", "dstfull") == false);
	CHECK(hostFileHas(dst_full_file, "dst-data"));
	CHECK(hostFileHas(src_file, "src-data"));

	// Renaming a world onto its own name is the same refusal, and must not destroy it.
	CHECK(worldlistRename(s_root, "src", "src") == false);
	CHECK(hostFileHas(src_file, "src-data"));

	hostRmTree(src);
	hostRmTree(dst_empty);
	hostRmTree(dst_full);
}

// Criterion 3, the case-only half. worldlistRename's own destination check (source/scene/
// worldlist.c lines 335-338) is a bare `stat(to, &st) == 0` -- no case-folding logic of its
// own -- so whether "office" -> "HOME" collides with an existing "home" depends entirely on
// what the filesystem underneath stat() considers the same path. This project's own host build
// runs under WSL against a drvfs-mounted NTFS volume (see testDeleteStopsOnFirstFailure's
// comment for the same fact established for readdir() order), and that mount was measured, on
// this machine, to be case-insensitive: build-host/x/HOME and build-host/x/home stat() to the
// same inode. So on THIS project's own test environment, line 335's plain existence check is
// exactly the case-only collision guard -- not because it says so anywhere, but because that is
// what stat() does here. The near-miss half exists so this cannot pass by refusing every rename
// that merely starts with "home": a name that differs by more than case -- "Homer" is not
// "home" under any case fold, on a case-insensitive filesystem or otherwise -- must still
// succeed, moving the whole tree exactly like testRenameMovesTheWholeTree already proves for an
// ordinary name.
static void testRenameRefusesCaseOnlyCollision(void)
{
	char home[320], office[320], office_file[384], homer[320], homer_file[384], moved[448];
	WorldEntry worlds[WORLDLIST_MAX];
	int count;
	bool truncated = true;

	// "home" is deliberately left EMPTY, the same way testRenameRefusesExistingDestination's
	// "dstempty" is: an empty destination is the one case a bare rename() would silently
	// replace instead of refuse, so it is also the one case that proves line 335's stat() check
	// is what is doing the refusing here, rather than rename()'s own ENOTEMPTY failing on a
	// non-empty destination for an unrelated reason.
	makeWorld(home, sizeof(home), "home");

	makeWorld(office, sizeof(office), "office");
	pathJoin(office_file, sizeof(office_file), office, "o.txt");
	hostWriteFile(office_file, "office-data");

	// Preconditions. Without these the "untouched" checks below could pass on worlds that were
	// never really there.
	CHECK(hostIsDir(home));
	CHECK(hostIsDir(office));
	CHECK(hostFileHas(office_file, "office-data"));

	errno = 0;
	CHECK(worldlistRename(s_root, "office", "HOME") == false);
	CHECK(errno == EEXIST);   // the exact refusal code line 336 sets

	// Nothing moved: office is exactly as it was, home is exactly as it was (still a directory,
	// still empty -- office's file did NOT land inside it), and a fresh scan of the directory
	// shows only the original two entries, not a third "HOME" a half-finished rename could have
	// left behind.
	CHECK(hostIsDir(office));
	CHECK(hostFileHas(office_file, "office-data"));
	CHECK(hostIsDir(home));
	pathJoin(moved, sizeof(moved), home, "o.txt");
	CHECK(!hostExists(moved));

	count = worldlistScan(s_root, worlds, WORLDLIST_MAX, &truncated);
	CHECK(count == 2);
	CHECK(!strcmp(worlds[0].name, "home"));
	CHECK(!strcmp(worlds[1].name, "office"));

	// The near miss: "Homer" is not "home" under a case fold either, so this rename must go
	// through exactly like any other, taking office's content with it and leaving "home" alone.
	CHECK(worldlistRename(s_root, "office", "Homer") == true);
	CHECK(!hostExists(office));
	pathJoin(homer, sizeof(homer), s_root, "Homer");
	CHECK(hostIsDir(homer));
	pathJoin(homer_file, sizeof(homer_file), homer, "o.txt");
	CHECK(hostFileHas(homer_file, "office-data"));
	CHECK(hostIsDir(home));

	hostRmTree(home);
	hostRmTree(homer);
}

static void testRenameRefusesBadNames(void)
{
	char src[320], src_file[384], evil[320];

	makeWorld(src, sizeof(src), "src");
	pathJoin(src_file, sizeof(src_file), src, "s.txt");
	hostWriteFile(src_file, "src-data");

	pathJoin(evil, sizeof(evil), s_sandbox, "evil");

	CHECK(worldlistRename(s_root, "src", "../evil") == false);
	CHECK(worldlistRename(s_root, "../outside", "fine") == false);
	CHECK(worldlistRename(s_root, "src", "bad!name") == false);
	CHECK(worldlistRename(s_root, "src", "") == false);
	CHECK(worldlistRename(s_root, "src", NULL) == false);
	CHECK(worldlistRename(s_root, NULL, "fine") == false);
	CHECK(worldlistRename(NULL, "src", "fine") == false);
	CHECK(worldlistRename(s_root, "nosuchworld", "fine") == false);

	CHECK(!hostExists(evil));               // no traversal target was created
	CHECK(hostFileHas(src_file, "src-data"));   // and the source is untouched

	hostRmTree(src);
}

// ── The delete-confirm latch ──────────────────────────────────────────────────────────

static void testConfirmNeedsTwoPresses(void)
{
	WorldDeleteConfirm c;
	worldlistConfirmReset(&c);

	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistConfirmPress(&c, 0) == false);    // first press only arms
	CHECK(worldlistConfirmArmedFor(&c, 0) == true);
	CHECK(worldlistConfirmPress(&c, 0) == true);     // second press confirms
	CHECK(worldlistConfirmArmedFor(&c, 0) == false); // and consumes the latch
	CHECK(worldlistConfirmPress(&c, 0) == false);    // so a third press arms again, not deletes
}

// Criterion 4, and the reason the latch is a testable module instead of two ints in title.c.
// Confirming deletion of world A and then moving the cursor to world B must NOT leave B one
// button press from deletion.
static void testConfirmDoesNotSurviveASelectionChange(void)
{
	WorldDeleteConfirm c;

	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmPress(&c, 0) == false);
	CHECK(worldlistConfirmArmedFor(&c, 0) == true);

	worldlistConfirmTrack(&c, 1);   // the cursor moved to world B
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistConfirmArmedFor(&c, 1) == false);   // B is NOT armed
	CHECK(worldlistConfirmPress(&c, 1) == false);      // B's first press only arms B
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);

	// The second, independent layer: even with no Track call at all — a caller that forgot it
	// — a press for a different selection must arm rather than confirm.
	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmPress(&c, 0) == false);
	CHECK(worldlistConfirmPress(&c, 1) == false);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);

	// Tracking the same selection every frame — what the screen actually does while nothing
	// moves — must NOT disarm it.
	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmPress(&c, 2) == false);
	worldlistConfirmTrack(&c, 2);
	worldlistConfirmTrack(&c, 2);
	CHECK(worldlistConfirmArmedFor(&c, 2) == true);

	// "There is no world to act on" disarms too.
	worldlistConfirmTrack(&c, WORLDLIST_CONFIRM_NONE);
	CHECK(worldlistConfirmArmedFor(&c, 2) == false);

	// Reset is the cancel path (B on the world-select screen).
	CHECK(worldlistConfirmPress(&c, 3) == false);
	CHECK(worldlistConfirmArmedFor(&c, 3) == true);
	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmArmedFor(&c, 3) == false);

	// A negative selection is "no world"; it can never arm or confirm.
	CHECK(worldlistConfirmPress(&c, -1) == false);
	CHECK(worldlistConfirmArmedFor(&c, -1) == false);
	CHECK(worldlistConfirmPress(&c, -1) == false);
}

// Criterion 5, first half — the trap the +1 encoding exists to close. titleInit memsets its
// whole TitleState; if the all-zero latch meant "armed for row 0", a caller that forgot
// worldlistConfirmReset would ship a first world one Y press from deletion. No Reset is called
// here ON PURPOSE: the memset IS the case under test.
static void testConfirmZeroInitIsOff(void)
{
	WorldDeleteConfirm c;
	memset(&c, 0, sizeof(c));

	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistConfirmPress(&c, 0) == false);    // arms; a zeroed latch must never FIRE
	CHECK(worldlistConfirmArmedFor(&c, 0) == true);

	// And the documented equivalence: Reset leaves exactly the zero state, so the two ways of
	// producing "off" cannot drift apart.
	WorldDeleteConfirm z;
	memset(&z, 0, sizeof(z));
	worldlistConfirmReset(&c);
	CHECK(memcmp(&c, &z, sizeof(c)) == 0);
}

// Criterion 5, second half — the window. Track is the clock (one call per frame from
// worldlistUiStep), so an arm that nobody follows up drops after WORLDLIST_CONFIRM_FRAMES of
// them, and a press on the frame after that arms again rather than deleting.
static void testConfirmExpires(void)
{
	WorldDeleteConfirm c;
	worldlistConfirmReset(&c);

	CHECK(worldlistConfirmPress(&c, 1) == false);
	for (int i = 0; i < WORLDLIST_CONFIRM_FRAMES - 1; i++)
		worldlistConfirmTrack(&c, 1);
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);    // one frame short: still armed
	worldlistConfirmTrack(&c, 1);
	CHECK(worldlistConfirmArmedFor(&c, 1) == false);   // the last frame spends it
	CHECK(worldlistConfirmPress(&c, 1) == false);      // so this press ARMS, it does not fire

	// A fresh press restarts the whole window, not the remainder of the last one.
	for (int i = 0; i < WORLDLIST_CONFIRM_FRAMES / 2; i++)
		worldlistConfirmTrack(&c, 1);
	CHECK(worldlistConfirmPress(&c, 1) == true);       // second press inside the window fires
	CHECK(worldlistConfirmPress(&c, 1) == false);      // re-arm
	for (int i = 0; i < WORLDLIST_CONFIRM_FRAMES - 1; i++)
		worldlistConfirmTrack(&c, 1);
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);    // a full window again, not half of one

	// Tracking while disarmed must not count anything or arm anything.
	worldlistConfirmReset(&c);
	for (int i = 0; i < 3; i++) worldlistConfirmTrack(&c, 0);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
}

// Criterion 6. The exact verb sequence title.c's drawPlayTab acts on, scripted with three
// worlds (world indices 0..2). NEW WORLD is its own bar row (index -1, ts->cursor's "nothing
// selected" value) rather than a trailing row after the worlds, and there is no BACK row at
// all in the current layout — leaving PLAY is a tab switch, not a row.
static void testUiStep(void)
{
	enum { N = 3 };
	WorldDeleteConfirm c;
	worldlistConfirmReset(&c);

	// A quiet frame on a world row.
	CHECK(worldlistUiStep(&c, 0, N, false, false, false) == WORLDLIST_UI_NONE);

	// Delete: arm, then fire on the same row.
	CHECK(worldlistUiStep(&c, 1, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);
	CHECK(worldlistUiStep(&c, 1, N, false, false, false) == WORLDLIST_UI_NONE);   // idle frame keeps it
	CHECK(worldlistConfirmArmedFor(&c, 1) == true);
	CHECK(worldlistUiStep(&c, 1, N, false, true, false) == WORLDLIST_UI_DELETE_FIRE);
	CHECK(worldlistConfirmArmedFor(&c, 1) == false);   // consumed

	// Arm on row 1, cursor moves to row 2, delete there: ARMED for 2, never FIRE.
	CHECK(worldlistUiStep(&c, 1, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	CHECK(worldlistUiStep(&c, 2, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	CHECK(worldlistConfirmArmedFor(&c, 1) == false);
	CHECK(worldlistConfirmArmedFor(&c, 2) == true);

	// B while armed: CANCELLED (consumed), and the very next B is a plain back (NONE).
	CHECK(worldlistUiStep(&c, 2, N, false, false, true) == WORLDLIST_UI_DELETE_CANCELLED);
	CHECK(worldlistConfirmArmedFor(&c, 2) == false);
	CHECK(worldlistUiStep(&c, 2, N, false, false, true) == WORLDLIST_UI_NONE);

	// B and Y on the same frame with a delete armed: the cancel wins, nothing fires.
	CHECK(worldlistUiStep(&c, 0, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	CHECK(worldlistUiStep(&c, 0, N, false, true, true) == WORLDLIST_UI_DELETE_CANCELLED);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);

	// Rename: prompts, and disarms whatever was armed. Rename and delete together: rename.
	CHECK(worldlistUiStep(&c, 0, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	CHECK(worldlistUiStep(&c, 0, N, true, false, false) == WORLDLIST_UI_RENAME_PROMPT);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistUiStep(&c, 0, N, true, true, false) == WORLDLIST_UI_RENAME_PROMPT);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistUiStep(&c, 0, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);   // arms, no fire

	// Moving onto NEW WORLD (row N) or BACK (row N+1) disarms, and X/Y there do nothing.
	CHECK(worldlistUiStep(&c, N, N, false, true, false) == WORLDLIST_UI_NONE);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(worldlistUiStep(&c, N + 1, N, true, true, false) == WORLDLIST_UI_NONE);
	CHECK(worldlistUiStep(&c, N + 1, N, false, false, true) == WORLDLIST_UI_NONE);   // B there is back

	// An empty list: row 0 is NEW WORLD, so nothing is a world and nothing arms.
	CHECK(worldlistUiStep(&c, 0, 0, false, true, false) == WORLDLIST_UI_NONE);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);

	// The window expires through UiStep's own Track call, with no other per-frame hook.
	CHECK(worldlistUiStep(&c, 2, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);
	for (int i = 0; i < WORLDLIST_CONFIRM_FRAMES; i++)
		CHECK(worldlistUiStep(&c, 2, N, false, false, false) == WORLDLIST_UI_NONE);
	CHECK(worldlistConfirmArmedFor(&c, 2) == false);
	CHECK(worldlistUiStep(&c, 2, N, false, true, false) == WORLDLIST_UI_DELETE_ARMED);

	CHECK(worldlistUiStep(NULL, 0, N, true, true, true) == WORLDLIST_UI_NONE);
}

// Criterion 7. A real tree, a real scan, and the cursor followed through a sort.
static void testRenameAt(void)
{
	char alpha[320], alpha_file[384], beta[320], gamma[320], gamma_file[384], delta[320];
	char zeta[320], zeta_file[384];
	WorldEntry worlds[WORLDLIST_MAX];
	int count = 0, sel = 0;
	bool truncated = true;
	WorldDeleteConfirm c;

	makeWorld(alpha, sizeof(alpha), "alpha");
	pathJoin(alpha_file, sizeof(alpha_file), alpha, "a.txt");
	hostWriteFile(alpha_file, "alpha-data");
	makeWorld(beta, sizeof(beta), "beta");
	makeWorld(gamma, sizeof(gamma), "gamma");
	pathJoin(gamma_file, sizeof(gamma_file), gamma, "g.txt");
	hostWriteFile(gamma_file, "gamma-data");

	count = worldlistScan(s_root, worlds, WORLDLIST_MAX, &truncated);
	CHECK(count == 3);
	CHECK(!strcmp(worlds[0].name, "alpha"));
	CHECK(!strcmp(worlds[1].name, "beta"));
	CHECK(!strcmp(worlds[2].name, "gamma"));

	// alpha -> zeta: sorts to the END, and the cursor must go there with it. The latch was
	// armed for row 0 (alpha) and must not survive the rows changing meaning.
	sel = 0;
	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmPress(&c, 0) == false);
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "zeta")
	      == WORLDLIST_RENAME_OK);
	CHECK(count == 3);
	CHECK(sel == 2);
	CHECK(!strcmp(worlds[0].name, "beta"));
	CHECK(!strcmp(worlds[1].name, "gamma"));
	CHECK(!strcmp(worlds[2].name, "zeta"));
	CHECK(truncated == false);
	CHECK(worldlistConfirmArmedFor(&c, 0) == false);
	CHECK(!hostExists(alpha));
	pathJoin(zeta, sizeof(zeta), s_root, "zeta");
	pathJoin(zeta_file, sizeof(zeta_file), zeta, "a.txt");
	CHECK(hostFileHas(zeta_file, "alpha-data"));   // moved, with its contents

	// Every refusal below must leave disk, list and *sel exactly as they are now.
	sel = 0;   // beta
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "gamma")
	      == WORLDLIST_RENAME_DUPLICATE);   // in the list
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "bad!name")
	      == WORLDLIST_RENAME_BAD_NAME);
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "../evil")
	      == WORLDLIST_RENAME_BAD_NAME);
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "")
	      == WORLDLIST_RENAME_BAD_NAME);
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, NULL)
	      == WORLDLIST_RENAME_BAD_NAME);
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "beta")
	      == WORLDLIST_RENAME_UNCHANGED);

	// A directory that exists on the card but is NOT in the list (made behind the scan's
	// back): the list check cannot see it, so this is worldlistRename's own EEXIST, folded
	// into DUPLICATE rather than reported as a generic failure.
	makeWorld(delta, sizeof(delta), "delta");
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "delta")
	      == WORLDLIST_RENAME_DUPLICATE);

	// Not a world row: NEW WORLD (row == count) and a stale negative cursor.
	sel = count;
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "fine")
	      == WORLDLIST_RENAME_NO_WORLD);
	CHECK(sel == count);
	sel = -1;
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c, "fine")
	      == WORLDLIST_RENAME_NO_WORLD);

	// The state assertions for all of the above.
	CHECK(count == 3);
	CHECK(!strcmp(worlds[0].name, "beta"));
	CHECK(!strcmp(worlds[1].name, "gamma"));
	CHECK(!strcmp(worlds[2].name, "zeta"));
	CHECK(hostIsDir(beta));
	CHECK(hostFileHas(gamma_file, "gamma-data"));
	CHECK(hostFileHas(zeta_file, "alpha-data"));
	CHECK(hostIsDir(delta));
	{
		char fine[320], evil[320];
		pathJoin(fine, sizeof(fine), s_root, "fine");
		pathJoin(evil, sizeof(evil), s_sandbox, "evil");
		CHECK(!hostExists(fine));
		CHECK(!hostExists(evil));
	}

	// A NULL latch is allowed (the header says so) and a rename still goes through.
	sel = 1;   // gamma
	CHECK(worldlistRenameAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, NULL, "aa")
	      == WORLDLIST_RENAME_OK);
	CHECK(sel == 0);   // "aa" sorts first
	CHECK(!strcmp(worlds[0].name, "aa"));
	CHECK(count == 4);   // delta is in the rescan now

	hostRmTree(beta);
	hostRmTree(delta);
	hostRmTree(zeta);
	{
		char aa[320];
		pathJoin(aa, sizeof(aa), s_root, "aa");
		hostRmTree(aa);
	}
}

// The status-line table: something to say for every refusal the player must act on, nothing
// for the three outcomes that need no message.
static void testRenameResultText(void)
{
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_OK) == NULL);
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_UNCHANGED) == NULL);
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_NO_WORLD) == NULL);
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_BAD_NAME) != NULL);
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_DUPLICATE) != NULL);
	CHECK(worldlistRenameResultText(WORLDLIST_RENAME_FAILED) != NULL);
	// Fits title.h's status[48] with the terminator — a message that does not fit is cut
	// silently by snprintf and the player reads half a sentence.
	CHECK(strlen(worldlistRenameResultText(WORLDLIST_RENAME_BAD_NAME)) < 48);
	CHECK(strlen(worldlistRenameResultText(WORLDLIST_RENAME_DUPLICATE)) < 48);
	CHECK(strlen(worldlistRenameResultText(WORLDLIST_RENAME_FAILED)) < 48);
}

// Criterion 8. Four worlds, deleted from the middle, the end, and down to nothing.
static void testDeleteAt(void)
{
	static const char* kNames[4] = {"w1", "w2", "w3", "w4"};
	char dirs[4][320], files[4][384];
	WorldEntry worlds[WORLDLIST_MAX];
	int count = 0, sel = 0;
	bool truncated = true;
	WorldDeleteConfirm c;

	for (int i = 0; i < 4; i++) {
		makeWorld(dirs[i], sizeof(dirs[i]), kNames[i]);
		pathJoin(files[i], sizeof(files[i]), dirs[i], "f.txt");
		hostWriteFile(files[i], kNames[i]);
	}
	count = worldlistScan(s_root, worlds, WORLDLIST_MAX, &truncated);
	CHECK(count == 4);

	// Not a world row: refused with EINVAL, nothing changes.
	sel = 4;
	errno = 0;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == false);
	CHECK(errno == EINVAL);
	CHECK(count == 4 && sel == 4);
	sel = -1;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == false);
	CHECK(count == 4);
	for (int i = 0; i < 4; i++) CHECK(hostFileHas(files[i], kNames[i]));

	// Middle row: w2 goes, w3 slides into row 1, and the cursor stays on row 1 — now w3.
	sel = 1;
	worldlistConfirmReset(&c);
	CHECK(worldlistConfirmPress(&c, 1) == false);
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == true);
	CHECK(count == 3);
	CHECK(sel == 1);
	CHECK(!strcmp(worlds[1].name, "w3"));
	CHECK(!hostExists(dirs[1]));
	CHECK(hostFileHas(files[0], "w1"));
	CHECK(hostFileHas(files[2], "w3"));
	CHECK(hostFileHas(files[3], "w4"));
	CHECK(worldlistConfirmArmedFor(&c, 1) == false);   // the rows changed meaning: disarmed
	CHECK(truncated == false);

	// Last row: w4 goes and the cursor is clamped back onto the new last row (w3).
	sel = 2;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == true);
	CHECK(count == 2);
	CHECK(sel == 1);
	CHECK(!strcmp(worlds[1].name, "w3"));
	CHECK(!hostExists(dirs[3]));

	// A world the list still shows but the card no longer has (removed behind its back):
	// refused, errno says why. v1.9.x audit fix: unlike the old behaviour, the list is NOT left
	// stale here — worldlistDeleteAt rescans on failure too, so the vanished row (w3) drops out
	// of the list on its own, leaving only w1, and the cursor is clamped onto it. A caller that
	// still believed w3 was there would be trusting exactly the stale snapshot the fix removes.
	hostRmTree(dirs[2]);
	sel = 1;
	errno = 0;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == false);
	CHECK(errno == ENOENT);
	CHECK(count == 1);
	CHECK(sel == 0);
	CHECK(!strcmp(worlds[0].name, "w1"));

	// Down to nothing: w1 goes and an empty list puts the cursor on 0 — the screen's NEW WORLD
	// button.
	sel = 0;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, &c) == true);
	CHECK(count == 0);
	CHECK(sel == 0);
	CHECK(!hostExists(dirs[0]));
	CHECK(hostIsDir(s_root));   // the root itself is never touched

	// A NULL latch is allowed.
	makeWorld(dirs[0], sizeof(dirs[0]), "w1");
	count = worldlistScan(s_root, worlds, WORLDLIST_MAX, &truncated);
	sel = 0;
	CHECK(worldlistDeleteAt(s_root, worlds, WORLDLIST_MAX, &count, &truncated, &sel, NULL) == true);
	CHECK(count == 0);
}

// Criterion 9. worldlistRmTree must stop the moment it hits an entry it cannot remove, instead
// of finishing the rest of the directory first (the pre-fix "record first_errno and keep going"
// behaviour a v1.9.x audit caught).
//
// The unremovable entry here is WORLDLIST_DELETE_MAX_DEPTH exceeded, NOT a permission bit,
// deliberately: a depth bound fails identically for every caller including root, where a
// chmod-based denial would not — root ignores Unix permission bits, and this suite must not
// silently stop testing anything the moment it happens to run as one. The construction mirrors
// testDeleteDepthBound's "toodeep" tree exactly, just one level down as a SIBLING of removable
// files instead of as the whole world.
static void testDeleteStopsOnFirstFailure(void)
{
	char world[320], toodeep[384];
	char sibs[20][448];

	makeWorld(world, sizeof(world), "damagedworld");

	// WORLDLIST_DELETE_MAX_DEPTH - 1 nested "d" directories below "aaa_toodeep" put its
	// (WORLDLIST_DELETE_MAX_DEPTH)th directory one level past the bound worldlistRmTree
	// refuses to cross — see testDeleteDepthBound's own comment for the arithmetic;
	// "aaa_toodeep" itself already spends one level of the budget the top-level call gets.
	//
	// Named to sort ahead of "sib00".."sib19" on purpose: this project's own host build runs
	// under WSL against a drvfs-mounted NTFS volume (see this file's header for why the build
	// lives under build-host/ rather than a system temp dir), and readdir() on that mount was
	// measured, on this machine, to hand entries back in roughly name order rather than any
	// hash order — an ext4 host would not owe this test the same order, which is exactly why
	// the CHECK below only claims "at least one survived", not which ones or how many. Picking
	// a name that sorts first is what turns "sometimes 0, sometimes up to 20" into a
	// reproducible, informative failure on THIS project's own CI-equivalent instead of a coin
	// flip — see the red-arm run this fix was verified against for the difference it makes.
	{
		char path[512], next[512];
		pathJoin(toodeep, sizeof(toodeep), world, "aaa_toodeep");
		hostMkdir(toodeep);
		snprintf(path, sizeof(path), "%s", toodeep);
		for (int i = 0; i < WORLDLIST_DELETE_MAX_DEPTH - 1; i++) {
			pathJoin(next, sizeof(next), path, "d");
			hostMkdir(next);
			snprintf(path, sizeof(path), "%s", next);
		}
		CHECK(hostIsDir(path));   // the too-deep leaf really exists before the delete runs
	}

	// Twenty ordinary, fully removable siblings of "toodeep" — enough that "toodeep" landing
	// dead last in whatever order readdir() happens to hand entries back (the one order that
	// would make this run indistinguishable from the pre-fix behaviour) is a 1-in-21 event
	// rather than something to plan a check around.
	for (int i = 0; i < 20; i++) {
		char name[16];
		snprintf(name, sizeof(name), "sib%02d.txt", i);
		pathJoin(sibs[i], sizeof(sibs[i]), world, name);
		hostWriteFile(sibs[i], "sib-data");
	}
	for (int i = 0; i < 20; i++) CHECK(hostFileHas(sibs[i], "sib-data"));

	errno = 0;
	CHECK(worldlistDelete(s_root, "damagedworld") == false);
	CHECK(errno == ELOOP);

	// Deterministic regardless of directory read order: the world itself and the whole
	// "toodeep" chain are still exactly there. Nothing this function does can ever remove
	// them — they are the reason the call failed at all.
	CHECK(hostIsDir(world));
	CHECK(hostIsDir(toodeep));

	// NOT provable in general — POSIX does not define readdir() order and this suite does not
	// control it — but true on THIS run against THIS filesystem: fail-closed means the walk
	// stopped instead of finishing the other 20 removable siblings, so at least one of them is
	// still there. Under the pre-fix "record and keep going" code this count is always 0; see
	// the task this fix was written for, which measured that exact number on this same host.
	int survived = 0;
	for (int i = 0; i < 20; i++) if (hostExists(sibs[i])) survived++;
	CHECK(survived > 0);

	hostRmTree(world);
}

int main(void)
{
	snprintf(s_sandbox, sizeof(s_sandbox), "build-host/run-%d-wlops", (int)getpid());
	snprintf(s_root, sizeof(s_root), "%s/worlds", s_sandbox);

	hostMkdir("build-host");
	hostRmTree(s_sandbox);   // a previous failed run's leftovers are not this run's input
	hostMkdir(s_sandbox);
	hostMkdir(s_root);

	if (!hostIsDir(s_root)) {
		printf("worldlist ops self-test: FAIL  cannot create %s\n", s_root);
		return 1;
	}

	testDeleteRemovesWholeTreeAndLeavesSiblingAlone();
	testDeleteRefusesTraversalAndTouchesNothing();
	testDeleteRefusesNamesNameValidRejects();
	testDeleteRefusesMissingAndNonDirectory();
	testDeleteDepthBound();
	testDeleteStopsOnFirstFailure();

	testPathComponentSafe();

	testRenameMovesTheWholeTree();
	testRenameRefusesExistingDestination();
	testRenameRefusesCaseOnlyCollision();
	testRenameRefusesBadNames();

	testConfirmNeedsTwoPresses();
	testConfirmDoesNotSurviveASelectionChange();
	testConfirmZeroInitIsOff();
	testConfirmExpires();

	testUiStep();
	testRenameAt();
	testRenameResultText();
	testDeleteAt();

	hostRmTree(s_sandbox);

	checkCountPin();

	if (s_fails == 0)
		printf("worldlist ops self-test: PASS  %d checks\n", s_checks);
	else
		printf("worldlist ops self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}
