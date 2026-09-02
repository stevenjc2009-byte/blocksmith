// Host self-test for app/version_history.c — the baked "up to the current latest version"
// half of the version history browser (v1.8.8, scene/title.c's drawVersionHistory). Same
// shape as app/updater_version_test.c: self-contained main(), CHECK/PASS/FAIL summary line,
// __3DS__-guarded so a future SOURCES change cannot pull two main()s into one build.
//
// What this file cannot cover, and says so rather than implying otherwise: the actual pixels
// scene/title.c draws (bottom-screen list, top-screen notes panel) need <3ds.h>,
// gfx/sprite.h and gfx/font.h, none of which build on this host — see title.h's own file
// comment on why that half has no host test. Only the pure-C data seam is exercised here.
//
// ── Why several checks below hardcode a version string at a fixed index ──────────────────
//
// tools/make_version_history.sh sorts whatsnew<version>.txt files oldest-first (sort -V) and
// only ever APPENDS a newer release to the end of app/version_history_data.c's table — it
// never reorders or removes an existing entry. So versionHistoryVersionAt(0) is "1.6.0" (the
// oldest version this browser ever bakes in, by the v1.6.0 cutoff app/version_history.h's
// file comment explains) for as long as this project ships, and versionHistoryVersionAt(9)/
// (10) stay "1.8.6"/"1.8.7" — the last two versions baked in as of this test being written —
// even after a future release appends an 11th index. A hardcoded COUNT would break on every
// single release; a hardcoded prefix does not, which is why versionHistoryCount() below is
// checked with >=, never ==.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/updater_version.h"
#include "app/version_history.h"
#include "app/whatsnew.h"

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

// ── versionHistoryCount / VERSION_HISTORY_COUNT ───────────────────────────────────────

static void testCount(void)
{
	// A floor, not an exact count — see the file comment. As of this test, 11 whatsnew
	// files are baked (1.6.0, 1.7.0, 1.7.1, 1.8.0..1.8.7); a future release can only ever
	// raise this.
	CHECK(versionHistoryCount() >= 11);
	CHECK(versionHistoryCount() == VERSION_HISTORY_COUNT);
}

// ── versionHistoryVersionAt: stable oldest-first ordering ─────────────────────────────

static void testOrderingIsStable(void)
{
	// Index 0 is always the oldest baked version — the v1.6.0 cutoff — for as long as this
	// project ships; nothing older is ever baked (app/version_history.h's file comment).
	CHECK(!strcmp(versionHistoryVersionAt(0), "1.6.0"));

	// Indices 9 and 10 are the last two versions baked in as of this test — stable prefixes
	// that a future append cannot move, per the file comment above.
	CHECK(!strcmp(versionHistoryVersionAt(9),  "1.8.6"));
	CHECK(!strcmp(versionHistoryVersionAt(10), "1.8.7"));

	// The real invariant that DOES survive every future release: every entry is strictly
	// newer than the one before it. Procedural — reuses app/updater_version.c's own compare
	// rather than re-implementing one, and would catch tools/make_version_history.sh's
	// sort -V regressing or a hand-edit landing entries out of order.
	const int n = versionHistoryCount();
	for (int i = 1; i < n; i++) {
		CHECK(updaterVersionIsNewer(versionHistoryVersionAt(i), versionHistoryVersionAt(i - 1)));
	}
}

// ── versionHistoryNotesAt: known-immutable content ─────────────────────────────────────

static void testKnownVersionContent(void)
{
	// v1.8.6's whatsnew file is 425 bytes, [features] x4 / [fixes] x1 — already-published
	// release notes, which cannot change retroactively.
	WhatsNew wn;
	versionHistoryNotesAt(9, &wn);
	CHECK(!strcmp(versionHistoryVersionAt(9), "1.8.6"));
	CHECK(wn.feature_count == 4);
	CHECK(wn.fix_count == 1);
	CHECK(!wn.truncated);
	CHECK(!wn.malformed);

	versionHistoryNotesAt(0, &wn);
	CHECK(!strcmp(versionHistoryVersionAt(0), "1.6.0"));
	CHECK(wn.feature_count == 4);
	CHECK(wn.fix_count == 11);
	CHECK(!wn.truncated);
	CHECK(!wn.malformed);
}

// ── Every baked entry, not just the spot-checked ones ──────────────────────────────────

static void testEveryEntryParsesCleanly(void)
{
	const int n = versionHistoryCount();
	for (int i = 0; i < n; i++) {
		const char* v = versionHistoryVersionAt(i);
		CHECK(v[0] != '\0');

		// A malformed version string would parse to 0.0.0 (app/updater_version.c's
		// documented failure mode) — every baked entry's version must be a real one.
		int parts[3];
		updaterVersionParse(v, parts);
		CHECK(parts[0] != 0 || parts[1] != 0 || parts[2] != 0);

		WhatsNew wn;
		versionHistoryNotesAt(i, &wn);
		// Truncated/malformed would mean a whatsnew file blew WHATSNEW_BYTES_MAX/
		// WHATSNEW_ITEMS_MAX/WHATSNEW_ITEM_CHARS (app/whatsnew.h) without anyone noticing —
		// tools/make_whatsnew.sh is supposed to catch that at generation time, but this
		// re-checks what actually shipped rather than trusting that it did.
		CHECK(!wn.truncated);
		CHECK(!wn.malformed);
		CHECK(whatsnewAny(&wn));
	}
}

// ── Out-of-range behaviour ──────────────────────────────────────────────────────────────

static void testOutOfRange(void)
{
	CHECK(!strcmp(versionHistoryVersionAt(-1), ""));
	CHECK(!strcmp(versionHistoryVersionAt(versionHistoryCount()), ""));
	CHECK(!strcmp(versionHistoryVersionAt(999), ""));

	WhatsNew wn;
	versionHistoryNotesAt(-1, &wn);
	CHECK(wn.count == 0 && !whatsnewAny(&wn));

	versionHistoryNotesAt(versionHistoryCount() + 50, &wn);
	CHECK(wn.count == 0 && !whatsnewAny(&wn));

	// NULL out-pointer must not crash — versionHistoryNotesAt's own documented contract.
	versionHistoryNotesAt(0, NULL);
}

int main(void)
{
	testCount();
	testOrderingIsStable();
	testKnownVersionContent();
	testEveryEntryParsesCleanly();
	testOutOfRange();

	if (s_fails == 0)
		printf("version_history self-test: PASS  %d checks\n", s_checks);
	else
		printf("version_history self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int version_history_test_host_only_t;

#endif   // !__3DS__
