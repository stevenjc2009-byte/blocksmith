// Host self-test for world/worldlist.c. Self-contained (its own main()), same shape and
// same reason as app/options_test.c: this module has nothing to do with the world data
// tools/run_host_tests.sh already links into build-host/world_test, and folding it in would
// mean one broken parser could stop the whole world suite from running.
//
// The CHECK macro and the PASS/FAIL summary line are copied from app/options_test.c, which
// copied them from world/world_test.c — same shape everywhere so a failure here reads the
// same way a failure anywhere else in this project does.
//
// The __3DS__ guard around the *whole file* is load-bearing, not tidy — copied verbatim
// from options_test.c's own comment on this: the Makefile globs every .c under source/world
// into the console build, so without the guard this file's main() links against
// source/main.c's and the build dies with "multiple definition of `main'". That exact bug
// already happened once today (a sibling module's test file), which is the only reason this
// comment is this blunt about it.
#ifndef __3DS__

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// rmdir/chdir/getcwd/getpid. The tests below already call bare rmdir(), so the
// non-underscore spellings are what this file needs on both platforms; MinGW supplies them
// from <direct.h>/<process.h> as oldname aliases. Without this include -Werror stops the
// host build on an implicit declaration of rmdir.
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

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Copied from
// app/options_test.c's testMkdir, solving the exact same host/console split.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

// Relative, and deliberately still a plain literal — main() runs the whole suite inside a
// per-process directory, so these paths resolve somewhere private to this run. See the
// comment in main() for why that is done there rather than here.
#define TEST_ROOT "build-host/worldlisttest"

// Deletes `path` and everything under it. remove() takes files and empty directories on
// both platforms, and the only thing it refuses is a non-empty directory, so recursing on
// failure and letting the tail rmdir finish the job covers every case. Best-effort — a
// leftover file is untidy, not a test failure.
static void testRmTree(const char* path)
{
	DIR* d = opendir(path);
	if (!d) return;

	for (const struct dirent* e = readdir(d); e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

		char child[512];
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (remove(child) != 0) testRmTree(child);
	}

	closedir(d);
	rmdir(path);
}

// ── Tests ──────────────────────────────────────────────────────────────────────────────

// A root that has never existed at all — the fresh-SD-card case. Every other test below
// creates its root first; this one must not.
static void testMissingRootIsEmpty(void)
{
	const char* root = TEST_ROOT "/missing";
	// In case a previous failed run left it behind — this test's whole point is that the
	// root does *not* exist, so a leftover from last time would silently pass it for the
	// wrong reason.
	rmdir(root);

	WorldEntry out[8];
	bool truncated = true;   // deliberately wrong, so CHECK below proves worldlistScan set it
	const int n = worldlistScan(root, out, 8, &truncated);

	CHECK(n == 0);
	CHECK(truncated == false);
}

static void testEmptyDirIsEmpty(void)
{
	const char* root = TEST_ROOT "/empty";
	testMkdir(root);

	WorldEntry out[8];
	bool truncated = true;
	const int n = worldlistScan(root, out, 8, &truncated);

	CHECK(n == 0);
	CHECK(truncated == false);
}

// Several real worlds, one stray regular file that must not appear as a world, listed back
// alphabetically regardless of the order they were created in.
static void testListsWorldsSortedAndSkipsFiles(void)
{
	const char* root = TEST_ROOT "/sorted";
	testMkdir(root);

	// Created out of alphabetical order on purpose — the sort is what worldlistScan has to
	// do, not an accident of creation order.
	testMkdir(TEST_ROOT "/sorted/charlie");
	testMkdir(TEST_ROOT "/sorted/alpha");
	testMkdir(TEST_ROOT "/sorted/bravo");

	FILE* f = fopen(TEST_ROOT "/sorted/notaworld.txt", "wb");
	CHECK(f != NULL);
	if (f) fclose(f);

	WorldEntry out[8];
	bool truncated = true;
	const int n = worldlistScan(root, out, 8, &truncated);

	CHECK(n == 3);
	CHECK(truncated == false);
	if (n == 3) {
		CHECK(!strcmp(out[0].name, "alpha"));
		CHECK(!strcmp(out[1].name, "bravo"));
		CHECK(!strcmp(out[2].name, "charlie"));
	}

	remove(TEST_ROOT "/sorted/notaworld.txt");
	rmdir(TEST_ROOT "/sorted/alpha");
	rmdir(TEST_ROOT "/sorted/bravo");
	rmdir(TEST_ROOT "/sorted/charlie");
	rmdir(root);
}

// More real world directories than the caller's array holds. Must return exactly `cap`,
// report the truncation honestly, and every entry it *did* return must be one of the real
// ones and sorted among itself — see worldlist.h for why it is not required to be the
// alphabetically-first `cap` of the whole set.
static void testTruncationReportsHonestly(void)
{
	const char* root = TEST_ROOT "/trunc";
	testMkdir(root);

	static const char* const names[] = {"w0", "w1", "w2", "w3", "w4"};
	char path[128];
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, names[i]);
		testMkdir(path);
	}

	WorldEntry out[2];   // cap smaller than the 5 real directories above
	bool truncated = false;
	const int n = worldlistScan(root, out, 2, &truncated);

	CHECK(n == 2);
	CHECK(truncated == true);

	// Whatever the two survivors are, each must be a real name from the set above (not
	// garbage, not a truncated/corrupted copy), and sorted between themselves.
	if (n == 2) {
		for (int i = 0; i < 2; i++) {
			bool known = false;
			for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++)
				if (!strcmp(out[i].name, names[j])) known = true;
			CHECK(known);
		}
		CHECK(strcmp(out[0].name, out[1].name) < 0);
	}

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, names[i]);
		rmdir(path);
	}
	rmdir(root);
}

// Every rejection case worldlistNameValid documents, plus the two round numbers that prove
// the length cap is enforced at exactly the right byte: WORLDLIST_NAME_MAX-1 (fits) and
// WORLDLIST_NAME_MAX (one over, does not).
static void testNameValidation(void)
{
	CHECK(worldlistNameValid("") == false);
	CHECK(worldlistNameValid(NULL) == false);

	CHECK(worldlistNameValid("hello") == true);
	CHECK(worldlistNameValid("hello world") == true);
	CHECK(worldlistNameValid("hello_world-2") == true);

	CHECK(worldlistNameValid(".") == false);
	CHECK(worldlistNameValid("..") == false);
	CHECK(worldlistNameValid("../etc") == false);
	CHECK(worldlistNameValid("a/b") == false);
	CHECK(worldlistNameValid("a\\b") == false);
	CHECK(worldlistNameValid(" leading") == false);
	CHECK(worldlistNameValid("trailing ") == false);
	CHECK(worldlistNameValid("bad!char") == false);
	CHECK(worldlistNameValid("tab\tin") == false);

	// Exactly the length cap: WORLDLIST_NAME_MAX-1 real characters plus the NUL the buffer
	// below supplies for free must be accepted.
	char at_cap[WORLDLIST_NAME_MAX];
	memset(at_cap, 'a', WORLDLIST_NAME_MAX - 1);
	at_cap[WORLDLIST_NAME_MAX - 1] = '\0';
	CHECK(strlen(at_cap) == (size_t)(WORLDLIST_NAME_MAX - 1));
	CHECK(worldlistNameValid(at_cap) == true);

	// One character over: WORLDLIST_NAME_MAX real characters, still NUL-terminated, must be
	// rejected — the off-by-one this test exists to catch is worldlistNameValid comparing
	// with <= instead of < against WORLDLIST_NAME_MAX.
	char over_cap[WORLDLIST_NAME_MAX + 1];
	memset(over_cap, 'a', WORLDLIST_NAME_MAX);
	over_cap[WORLDLIST_NAME_MAX] = '\0';
	CHECK(strlen(over_cap) == (size_t)WORLDLIST_NAME_MAX);
	CHECK(worldlistNameValid(over_cap) == false);
}

// S_ISDIR, not `st_mode & S_IFDIR`. Two reasons, and the second is the one that matters:
// glibc hides the S_IF* constants under -std=c11 (they are gated on __USE_MISC, which
// strict ISO mode turns off — the S_ISDIR macros stay visible), and S_IFMT is a
// multi-bit type field rather than a set of flags, so the bitwise test is also true for
// a symlink (S_IFLNK 0120000 & S_IFDIR 0040000 is non-zero).
static bool dirExists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void testCreateWorld(void)
{
	const char* root = TEST_ROOT "/create";
	// Removed rather than left from a previous run: this test's first assertion is that
	// worldlistCreate makes `root` itself from nothing, which a leftover root would hide.
	rmdir(TEST_ROOT "/create/myworld");
	rmdir(root);

	CHECK(dirExists(root) == false);
	CHECK(worldlistCreate(root, "myworld") == true);
	CHECK(dirExists(root) == true);
	CHECK(dirExists(TEST_ROOT "/create/myworld") == true);

	// Re-creating the same name is success, not failure — see worldlist.h on why.
	CHECK(worldlistCreate(root, "myworld") == true);
	CHECK(dirExists(TEST_ROOT "/create/myworld") == true);

	// An invalid name must not reach mkdir at all.
	CHECK(worldlistCreate(root, "bad/name") == false);
	CHECK(dirExists(TEST_ROOT "/create/bad") == false);
	CHECK(worldlistCreate(root, "") == false);
	CHECK(worldlistCreate(root, "..") == false);

	// And the one it did create is exactly what worldlistScan reports back.
	WorldEntry out[8];
	bool truncated = true;
	const int n = worldlistScan(root, out, 8, &truncated);
	CHECK(n == 1);
	CHECK(truncated == false);
	if (n == 1) CHECK(!strcmp(out[0].name, "myworld"));

	rmdir(TEST_ROOT "/create/myworld");
	rmdir(root);
}

int main(void)
{
	// Every path this suite touches is relative and fixed, so two copies of this binary
	// running at once fight over the same directories: one removes a world the other has
	// just created and is about to assert on. It does not present as a race — it presents
	// as assertions failing at random line numbers on a suite that passes when re-run.
	// Measured before this guard existed: 7 of 8 concurrent runs failed, at four different
	// assertions.
	//
	// Rather than thread a per-process name through the dozen concatenated TEST_ROOT
	// literals above, the whole suite runs inside its own per-pid directory — the relative
	// paths stay exactly as written and are simply resolved somewhere private.
	// world/world_test.c has the same problem and solves it the other way (a per-pid
	// TEST_WORLD_DIR), because its entry point is shared with the console build, which must
	// not chdir.
	char sandbox[64];
	snprintf(sandbox, sizeof(sandbox), "build-host/worldlisttest-%ld", (long)getpid());

	char home[512];
	if (!getcwd(home, sizeof(home))) {
		printf("worldlist self-test: FAIL  cannot read the working directory\n");
		return 1;
	}

	testMkdir("build-host");
	testMkdir(sandbox);

	// Refusing rather than carrying on in the shared directory: running anyway would
	// re-create the exact flakiness this exists to remove, and would pass most of the time.
	if (chdir(sandbox) != 0) {
		printf("worldlist self-test: FAIL  cannot enter %s\n", sandbox);
		return 1;
	}

	testMkdir("build-host");
	testMkdir(TEST_ROOT);

	testMissingRootIsEmpty();
	testEmptyDirIsEmpty();
	testListsWorldsSortedAndSkipsFiles();
	testTruncationReportsHonestly();
	testNameValidation();
	testCreateWorld();

	rmdir(TEST_ROOT);   // best-effort; leaves nothing if every test above cleaned up right

	// Back out and take the whole sandbox with it, including anything a test left behind on
	// purpose (testEmptyDirIsEmpty never removes its own root).
	if (chdir(home) == 0) testRmTree(sandbox);

	if (s_fails == 0)
		printf("worldlist self-test: PASS  %d checks\n", s_checks);
	else
		printf("worldlist self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int worldlist_test_host_only_t;

#endif   // !__3DS__
