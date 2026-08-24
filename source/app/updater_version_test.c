// Host self-test for app/updater_version.c — the parsing/compare/naming half of the
// updater that has no <3ds.h> in it. Self-contained (its own main()), same reasoning as
// app/options_test.c: this has nothing to do with the world data tools/run_host_tests.sh
// already compiles, so it is a separate binary rather than folded into that harness.
//
// What this file cannot cover, and says so rather than implying otherwise: the network
// path (app/updater.c's runCheck/runInstall) needs libcurl, AM, RomFs and a real GitHub
// fetch, none of which exist on this host build. Only the pure-C seam is exercised here.
//
// The CHECK macro and the PASS/FAIL summary line are copied in the same shape
// world/world_test.c and app/options_test.c use, so a failure here reads the same way a
// failure anywhere else in this codebase does.
//
// The __3DS__ guard below is load-bearing rather than tidy: if this file were ever pulled
// into the console build's SOURCES scan (it currently is not — see tools/run_host_tests.sh
// for how this binary gets built), its main() would collide with source/main.c's. Guarded
// the same way options_test.c and world_test.c guard theirs, so a future refactor that
// changes SOURCES cannot resurrect that exact bug silently.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/updater_version.h"

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

// ── updaterVersionParse ───────────────────────────────────────────────────────────────

static void testParseForms(void)
{
	int out[3];

	updaterVersionParse("v1.2.3", out);
	CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);

	updaterVersionParse("1.2.3", out);
	CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);

	updaterVersionParse("1.2", out);
	CHECK(out[0] == 1 && out[1] == 2 && out[2] == 0);

	updaterVersionParse("1", out);
	CHECK(out[0] == 1 && out[1] == 0 && out[2] == 0);

	updaterVersionParse("V0.1.0", out);
	CHECK(out[0] == 0 && out[1] == 1 && out[2] == 0);
}

// A malformed tag must stay all-zero rather than half-parse into something that could
// accidentally look "newer" than a real release — the exact failure mode versionIsNewer's
// caller relies on this function to avoid.
static void testParseMalformed(void)
{
	int out[3];

	updaterVersionParse("", out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

	updaterVersionParse(NULL, out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

	updaterVersionParse("not-a-version", out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

	updaterVersionParse("release-candidate", out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

	// Leading garbage before any digit: strtol finds nothing to convert, same as above.
	updaterVersionParse("vX.Y.Z", out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
}

// ── updaterVersionIsNewer ─────────────────────────────────────────────────────────────

static void testCompareVWithNoV(void)
{
	// The exact pairing named in the task: "v0.1.0" vs "0.1.0" must compare equal (neither
	// newer), because a leading "v" is tolerated on either side.
	CHECK(!updaterVersionIsNewer("v0.1.0", "0.1.0"));
	CHECK(!updaterVersionIsNewer("0.1.0", "v0.1.0"));
}

static void testCompareOlderNewerEqual(void)
{
	CHECK(updaterVersionIsNewer("0.2.0", "0.1.0"));   // newer minor
	CHECK(updaterVersionIsNewer("1.0.0", "0.9.9"));   // newer major beats older minor/patch
	CHECK(updaterVersionIsNewer("0.1.1", "0.1.0"));   // newer patch

	CHECK(!updaterVersionIsNewer("0.1.0", "0.2.0"));  // older minor
	CHECK(!updaterVersionIsNewer("0.9.9", "1.0.0"));  // older major
	CHECK(!updaterVersionIsNewer("0.1.0", "0.1.1"));  // older patch

	CHECK(!updaterVersionIsNewer("0.1.0", "0.1.0"));  // equal
	CHECK(!updaterVersionIsNewer("v1.2.3", "1.2.3")); // equal, mixed "v"
}

// A malformed candidate must never win: it parses to 0.0.0, which is older than any real
// shipped version, not newer.
static void testCompareMalformedNeverWins(void)
{
	CHECK(!updaterVersionIsNewer("not-a-version", "0.1.0"));
	CHECK(!updaterVersionIsNewer("", "0.1.0"));
	CHECK(!updaterVersionIsNewer(NULL, "0.1.0"));

	// The reverse also has to hold: a malformed *current* version (e.g. this build shipped
	// with a broken BLOCKSMITH_VERSION somehow) reads as 0.0.0, so any real tag looks newer
	// rather than the check silently refusing to fire.
	CHECK(updaterVersionIsNewer("0.1.0", "not-a-version"));
}

// ── updaterTagFromRedirect ────────────────────────────────────────────────────────────

static void testTagFromRedirect(void)
{
	char tag[32];

	CHECK(updaterTagFromRedirect(
	    "https://github.com/stevenjc2009-byte/blocksmith/releases/tag/v0.1.0",
	    tag, sizeof(tag)));
	CHECK(!strcmp(tag, "v0.1.0"));

	// No marker at all — a page that redirected somewhere unrelated.
	CHECK(!updaterTagFromRedirect("https://github.com/stevenjc2009-byte/blocksmith",
	                               tag, sizeof(tag)));

	// Marker present but nothing after it.
	CHECK(!updaterTagFromRedirect(
	    "https://github.com/stevenjc2009-byte/blocksmith/releases/tag/",
	    tag, sizeof(tag)));

	CHECK(!updaterTagFromRedirect(NULL, tag, sizeof(tag)));
}

// ── updaterBuildAssetName ─────────────────────────────────────────────────────────────

static void testBuildAssetName(void)
{
	char name[64];

	updaterBuildAssetName("v0.1.0", name, sizeof(name));
	CHECK(!strcmp(name, "blocksmith0.1.0.cia"));

	// No leading "v" — same result, the "v" is stripped either way.
	updaterBuildAssetName("0.1.0", name, sizeof(name));
	CHECK(!strcmp(name, "blocksmith0.1.0.cia"));

	updaterBuildAssetName("v1.2.3", name, sizeof(name));
	CHECK(!strcmp(name, "blocksmith1.2.3.cia"));
}

// ── updaterBuildNotesName (v1.6.0 task 14b) ───────────────────────────────────────────

static void testBuildNotesName(void)
{
	char name[64];

	updaterBuildNotesName("v1.6.0", name, sizeof(name));
	CHECK(!strcmp(name, "whatsnew1.6.0.txt"));

	// The "v" is tolerated on either side, exactly as it is for the .cia — the two names
	// share tagNumber() precisely so this cannot drift.
	updaterBuildNotesName("1.6.0", name, sizeof(name));
	CHECK(!strcmp(name, "whatsnew1.6.0.txt"));

	updaterBuildNotesName("V0.1.0", name, sizeof(name));
	CHECK(!strcmp(name, "whatsnew0.1.0.txt"));

	// The pair a release actually publishes, checked side by side: same tag in, the same
	// version number out of both.
	char cia[64];
	updaterBuildAssetName("v1.6.0", cia, sizeof(cia));
	updaterBuildNotesName("v1.6.0", name, sizeof(name));
	CHECK(!strcmp(cia, "blocksmith1.6.0.cia"));
	CHECK(!strcmp(name, "whatsnew1.6.0.txt"));

	// A NULL tag must leave the buffer alone rather than formatting "(null)" into a URL.
	snprintf(name, sizeof(name), "untouched");
	updaterBuildNotesName(NULL, name, sizeof(name));
	CHECK(!strcmp(name, "untouched"));
}

int main(void)
{
	testParseForms();
	testParseMalformed();
	testCompareVWithNoV();
	testCompareOlderNewerEqual();
	testCompareMalformedNeverWins();
	testTagFromRedirect();
	testBuildAssetName();
	testBuildNotesName();

	if (s_fails == 0)
		printf("updater_version self-test: PASS  %d checks\n", s_checks);
	else
		printf("updater_version self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int updater_version_test_host_only_t;

#endif   // !__3DS__
