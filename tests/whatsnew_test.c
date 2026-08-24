// Host self-test for app/whatsnew.c — the release-notes parser, word wrap, scroll clamp,
// scrollbar geometry and held-to-repeat timing behind v1.6.0 task 14b.
//
// The REAL source/app/whatsnew.c is in the link (see tools/run_host_tests.sh). Nothing here
// is a hand-copy of the logic under test, and there are no stubs at all, because that file
// has no <3ds.h> in it to stub around — which is the whole reason it was written as its own
// module rather than inside scene/title.c. tools/run_host_tests.sh records what happened the
// last two times this project got that wrong: tests/battery_test.c and tests/sleep_test.c
// each carried a private copy of the logic and stayed green with the real module deleted.
//
// What this file CANNOT cover, said plainly rather than implied: the HTTPS fetch in
// app/updater.c (libcurl, RomFs, a real GitHub release) and the drawing in scene/title.c
// (citro3d, a 400x240 framebuffer). Those need a console. Everything that decides what the
// player reads and where the slider sits is here.
//
// Every check below was red-proven by sabotage — see tools/run_host_tests.sh's stanza for
// this binary for each sabotage and the measured output it produced.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

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

// Parses a NUL-terminated literal, which is what every fixture below is.
static void parseText(const char* text, WhatsNew* wn)
{
	whatsnewParse(text, strlen(text), wn);
}

// How many layout lines carry a given kind. Used instead of asserting exact indices where
// the exact index is not the property under test.
static int countKind(const WhatsNewLayout* l, int kind)
{
	int n = 0;
	for (int i = 0; i < l->count; i++)
		if (l->kind[i] == (unsigned char)kind) n++;
	return n;
}

static bool layoutHas(const WhatsNewLayout* l, const char* text)
{
	for (int i = 0; i < l->count; i++)
		if (strcmp(l->lines[i], text) == 0) return true;
	return false;
}

// ── Parsing ───────────────────────────────────────────────────────────────────────────

static void testParseWellFormed(void)
{
	WhatsNew wn;
	parseText("# a comment, ignored\n"
	          "[features]\n"
	          "Registry blocks work in multiplayer.\n"
	          "- A leading dash is optional.\n"
	          "\n"
	          "[fixes]\n"
	          "* A leading star works too.\n"
	          "The texture sheet was rebuilt.\n", &wn);

	CHECK(wn.count == 4);
	CHECK(wn.feature_count == 2);
	CHECK(wn.fix_count == 2);
	CHECK(!wn.truncated);
	CHECK(!wn.malformed);
	CHECK(whatsnewAny(&wn));

	CHECK(!strcmp(wn.items[0].text, "Registry blocks work in multiplayer."));
	CHECK(wn.items[0].section == WN_SECTION_FEATURES);

	// The bullet is stripped, not kept — the layout puts its own back, so keeping this one
	// would draw "- - A leading dash...".
	CHECK(!strcmp(wn.items[1].text, "A leading dash is optional."));
	CHECK(wn.items[1].section == WN_SECTION_FEATURES);

	CHECK(!strcmp(wn.items[2].text, "A leading star works too."));
	CHECK(wn.items[2].section == WN_SECTION_FIXES);
	CHECK(!strcmp(wn.items[3].text, "The texture sheet was rebuilt."));
	CHECK(wn.items[3].section == WN_SECTION_FIXES);
}

// Case and surrounding whitespace on a header must not matter: a person types these by hand.
static void testParseHeaderTolerance(void)
{
	WhatsNew wn;
	parseText("  [FEATURES]  \n\tOne\n[Fixes]\nTwo\n", &wn);

	CHECK(wn.count == 2);
	CHECK(wn.feature_count == 1);
	CHECK(wn.fix_count == 1);
	CHECK(!strcmp(wn.items[0].text, "One"));
	CHECK(!strcmp(wn.items[1].text, "Two"));
}

static void testParseEmptyFile(void)
{
	WhatsNew wn;
	parseText("", &wn);
	CHECK(wn.count == 0);
	CHECK(!whatsnewAny(&wn));
	CHECK(!wn.truncated);

	// The two NULL/zero-length forms the fetch can hand over.
	whatsnewParse(NULL, 99, &wn);
	CHECK(wn.count == 0);
	CHECK(!whatsnewAny(&wn));

	whatsnewParse("anything", 0, &wn);
	CHECK(wn.count == 0);
	CHECK(!whatsnewAny(&wn));

	// A file of nothing but blank lines and comments is the same as an empty one.
	parseText("\n\n   \n# only a comment\n\r\n", &wn);
	CHECK(wn.count == 0);
	CHECK(!whatsnewAny(&wn));
	CHECK(!wn.malformed);
}

// Text with no section header at all. Every line is dropped — filing a bug fix under
// features because it happened to come first would be worse than showing nothing.
static void testParseNoSections(void)
{
	WhatsNew wn;
	parseText("Just some prose.\nAnd another line.\n", &wn);

	CHECK(wn.count == 0);
	CHECK(!whatsnewAny(&wn));
	CHECK(wn.malformed);
	CHECK(!wn.truncated);
}

static void testParseSectionWithNoItems(void)
{
	WhatsNew wn;
	parseText("[features]\n[fixes]\n", &wn);

	CHECK(wn.count == 0);
	CHECK(wn.feature_count == 0);
	CHECK(wn.fix_count == 0);
	CHECK(!whatsnewAny(&wn));
	CHECK(!wn.malformed);

	// One populated group and one empty one is the normal shape of a bugfix-only release.
	parseText("[features]\n[fixes]\nOne fix.\n", &wn);
	CHECK(wn.count == 1);
	CHECK(wn.feature_count == 0);
	CHECK(wn.fix_count == 1);
}

// A header nobody knows closes the section rather than inheriting the previous one, so its
// lines are dropped instead of silently filed under the wrong heading.
static void testParseUnknownSection(void)
{
	WhatsNew wn;
	parseText("[features]\nKept.\n[notes]\nDropped.\n", &wn);

	CHECK(wn.count == 1);
	CHECK(!strcmp(wn.items[0].text, "Kept."));
	CHECK(wn.malformed);
}

// A file authored on Windows — which this project's are — arrives with CRLF. The '\r' must
// not survive into an item, where it would draw as a '?' at the end of every single line.
static void testParseCrlf(void)
{
	WhatsNew wn;
	parseText("[features]\r\nOne thing.\r\n[fixes]\r\nAnother thing.\r\n", &wn);

	CHECK(wn.count == 2);
	CHECK(!strcmp(wn.items[0].text, "One thing."));
	CHECK(!strcmp(wn.items[1].text, "Another thing."));
	CHECK(wn.feature_count == 1 && wn.fix_count == 1);
}

// A line past WHATSNEW_ITEM_CHARS is cut, the cut is marked with "..." on screen, and
// nothing is written past the buffer.
static void testParseOverlongLine(void)
{
	char text[512];
	char longline[300];
	memset(longline, 'x', sizeof(longline));
	longline[sizeof(longline) - 1] = '\0';
	snprintf(text, sizeof(text), "[features]\n%s\n", longline);

	WhatsNew wn;
	parseText(text, &wn);

	CHECK(wn.count == 1);
	CHECK(wn.truncated);
	CHECK(strlen(wn.items[0].text) == WHATSNEW_ITEM_CHARS - 1);
	// Visibly truncated, not silently: the last three characters are the ellipsis.
	CHECK(!strcmp(wn.items[0].text + WHATSNEW_ITEM_CHARS - 4, "..."));
	// And the terminator is where it belongs, so nothing ran past the array.
	CHECK(wn.items[0].text[WHATSNEW_ITEM_CHARS - 1] == '\0');

	// A line exactly at the cap is NOT truncated — the boundary is off-by-one country.
	char exact[WHATSNEW_ITEM_CHARS];
	memset(exact, 'y', sizeof(exact));
	exact[WHATSNEW_ITEM_CHARS - 1] = '\0';
	snprintf(text, sizeof(text), "[fixes]\n%s\n", exact);
	parseText(text, &wn);
	CHECK(wn.count == 1);
	CHECK(!wn.truncated);
	CHECK(strlen(wn.items[0].text) == WHATSNEW_ITEM_CHARS - 1);
}

// More items than the store holds. Parsing stops, the flag goes up, and nothing is written
// past items[WHATSNEW_ITEMS_MAX - 1].
static void testParseTooManyItems(void)
{
	char text[4096];
	size_t at = (size_t)snprintf(text, sizeof(text), "[features]\n");
	for (int i = 0; i < WHATSNEW_ITEMS_MAX + 10 && at < sizeof(text) - 16; i++)
		at += (size_t)snprintf(text + at, sizeof(text) - at, "item %d\n", i);

	WhatsNew wn;
	parseText(text, &wn);

	CHECK(wn.count == WHATSNEW_ITEMS_MAX);
	CHECK(wn.feature_count == WHATSNEW_ITEMS_MAX);
	CHECK(wn.truncated);
	CHECK(!strcmp(wn.items[0].text, "item 0"));
	CHECK(!strcmp(wn.items[WHATSNEW_ITEMS_MAX - 1].text, "item 31"));
}

// A file bigger than the byte cap. Only the front is read, the flag goes up, and — the point
// of the whole design — the console never allocates a byte in response to the file's size.
static void testParseOversizeFile(void)
{
	static char big[WHATSNEW_BYTES_MAX * 3];
	memset(big, 'a', sizeof(big));
	memcpy(big, "[features]\nAt the front.\n", 25);
	// A section header far past the cap, whose items must never be seen.
	memcpy(big + WHATSNEW_BYTES_MAX + 100, "\n[fixes]\nPast the cap.\n", 23);

	WhatsNew wn;
	whatsnewParse(big, sizeof(big), &wn);

	CHECK(wn.truncated);
	CHECK(wn.fix_count == 0);
	CHECK(!strcmp(wn.items[0].text, "At the front."));
	for (int i = 0; i < wn.count; i++)
		CHECK(strcmp(wn.items[i].text, "Past the cap.") != 0);
}

// Binary garbage, including embedded NULs, must produce visible junk inside the caps rather
// than a crash, an overrun or an invisible line.
static void testParseBinaryGarbage(void)
{
	static const char garbage[] =
		"[features]\n\x01\x02\x03 \xff\xfe\0 hidden \x7f\n[fixes]\n\x80\x81\n";

	WhatsNew wn;
	whatsnewParse(garbage, sizeof(garbage) - 1, &wn);

	CHECK(wn.count == 2);
	CHECK(wn.feature_count == 1);
	CHECK(wn.fix_count == 1);

	// Every byte the font cannot draw became '?', so the line has a visible width instead of
	// being a mystery gap — and the NUL did not end the line early.
	CHECK(!strcmp(wn.items[0].text, "??? ??? hidden ?"));
	CHECK(!strcmp(wn.items[1].text, "??"));

	for (int i = 0; i < wn.count; i++) {
		const char* t = wn.items[i].text;
		for (size_t k = 0; t[k]; k++)
			CHECK(t[k] >= 32 && t[k] <= 126);
	}
}

// ── Word wrap ─────────────────────────────────────────────────────────────────────────

static void testLayoutPlaceholder(void)
{
	WhatsNew wn;
	whatsnewClear(&wn);

	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, 366, 6, &l);

	CHECK(l.count == 1);
	CHECK(l.kind[0] == WN_LINE_NOTE);
	CHECK(!strcmp(l.lines[0], whatsnewPlaceholder()));

	// NULL is the same answer, so the caller never has to special-case "no check has run".
	whatsnewBuildLayout(NULL, 366, 6, &l);
	CHECK(l.count == 1);
	CHECK(!strcmp(l.lines[0], whatsnewPlaceholder()));
}

static void testLayoutHeadingsAndBullets(void)
{
	WhatsNew wn;
	parseText("[features]\nAlpha\n[fixes]\nBeta\n", &wn);

	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, 366, 6, &l);

	CHECK(layoutHas(&l, "FEATURES ADDED"));
	CHECK(layoutHas(&l, "BUGS FIXED"));
	CHECK(layoutHas(&l, "- Alpha"));
	CHECK(layoutHas(&l, "- Beta"));
	CHECK(countKind(&l, WN_LINE_HEADING) == 2);
	CHECK(countKind(&l, WN_LINE_BLANK) == 1);   // the spacer between the two groups

	// Only one group present: no spacer, and the absent heading is not drawn empty.
	parseText("[fixes]\nBeta\n", &wn);
	whatsnewBuildLayout(&wn, 366, 6, &l);
	CHECK(!layoutHas(&l, "FEATURES ADDED"));
	CHECK(layoutHas(&l, "BUGS FIXED"));
	CHECK(countKind(&l, WN_LINE_BLANK) == 0);
}

// The property the top screen actually depends on: no laid-out line is wider than the pixel
// column it was measured for.
static void testLayoutFitsColumn(void)
{
	WhatsNew wn;
	parseText("[features]\n"
	          "Registry-defined blocks now work in multiplayer, so a server can add its own.\n"
	          "[fixes]\n"
	          "The block texture sheet was rebuilt and every tile lines up again.\n", &wn);

	const int wrap_px = 366, advance = 6;
	const int cols = wrap_px / advance;   // 61

	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, wrap_px, advance, &l);

	CHECK(l.count > 4);   // it really did wrap, rather than fitting on one line each
	for (int i = 0; i < l.count; i++)
		CHECK((int)strlen(l.lines[i]) <= cols);

	// Continuation lines are indented under the bullet, and are marked as continuations so
	// the drawing half can dim them without re-deciding what they are.
	CHECK(countKind(&l, WN_LINE_CONT) > 0);
	for (int i = 0; i < l.count; i++)
		if (l.kind[i] == WN_LINE_CONT)
			CHECK(l.lines[i][0] == ' ' && l.lines[i][1] == ' ' && l.lines[i][2] != ' ');
}

// A single word longer than the column has nowhere to break, so it is hard-split. The one
// outcome that is not allowed is a line wider than the panel it is drawn into.
static void testLayoutWordLongerThanLine(void)
{
	WhatsNew wn;
	parseText("[features]\n"
	          "Supercalifragilisticexpialidocious-and-then-some-more-characters ok\n", &wn);

	// A deliberately narrow column: 20 px at 6 px an advance is 3 characters.
	const int cols = 20 / 6;   // 3
	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, 20, 6, &l);

	CHECK(l.count > 1);
	for (int i = 0; i < l.count; i++)
		CHECK((int)strlen(l.lines[i]) <= cols);

	// And it terminated rather than spinning: a column narrower than the "- " prefix is the
	// case that could loop forever if `avail` were allowed to reach zero.
	whatsnewBuildLayout(&wn, 6, 6, &l);
	CHECK(l.count > 0);
	CHECK(l.count <= WHATSNEW_LINES_MAX);
}

// A cut in the parse has to reach the screen. A changelog that is silently shorter than the
// file is worse than one that says it was shortened.
static void testLayoutReportsTruncation(void)
{
	char text[512];
	char longline[300];
	memset(longline, 'x', sizeof(longline));
	longline[sizeof(longline) - 1] = '\0';
	snprintf(text, sizeof(text), "[features]\n%s\n", longline);

	WhatsNew wn;
	parseText(text, &wn);
	CHECK(wn.truncated);

	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, 366, 6, &l);
	CHECK(layoutHas(&l, "(these notes were too long to show in full)"));

	// The clean case must NOT carry the footnote, or it would say so on every release.
	parseText("[features]\nShort.\n", &wn);
	whatsnewBuildLayout(&wn, 366, 6, &l);
	CHECK(!layoutHas(&l, "(these notes were too long to show in full)"));
}

// More lines than the layout holds: it stops at the cap and never writes past it.
static void testLayoutLineCap(void)
{
	char text[4096];
	size_t at = (size_t)snprintf(text, sizeof(text), "[features]\n");
	for (int i = 0; i < WHATSNEW_ITEMS_MAX && at < sizeof(text) - 80; i++)
		at += (size_t)snprintf(text + at, sizeof(text) - at,
		                        "a fairly long item number %d that will wrap onto two lines "
		                        "because it keeps going\n", i);

	WhatsNew wn;
	parseText(text, &wn);

	WhatsNewLayout l;
	whatsnewBuildLayout(&wn, 366, 6, &l);

	CHECK(l.count == WHATSNEW_LINES_MAX);
	CHECK(l.truncated);
	for (int i = 0; i < l.count; i++)
		CHECK(strlen(l.lines[i]) < WHATSNEW_LINE_CHARS);
}

// ── Scroll clamping ───────────────────────────────────────────────────────────────────

static void testScrollClamp(void)
{
	// Everything fits: there is nowhere to go, in either direction.
	CHECK(whatsnewMaxScroll(10, 21) == 0);
	CHECK(!whatsnewScrollable(10, 21));
	CHECK(whatsnewClampScroll(0, 10, 21) == 0);
	CHECK(whatsnewClampScroll(5, 10, 21) == 0);
	CHECK(whatsnewClampScroll(-3, 10, 21) == 0);

	// Exactly full is still not scrollable — the off-by-one that would show one blank line.
	CHECK(whatsnewMaxScroll(21, 21) == 0);
	CHECK(!whatsnewScrollable(21, 21));

	// One line over: exactly one step of travel.
	CHECK(whatsnewMaxScroll(22, 21) == 1);
	CHECK(whatsnewScrollable(22, 21));
	CHECK(whatsnewClampScroll(1, 22, 21) == 1);
	CHECK(whatsnewClampScroll(2, 22, 21) == 1);

	// Both ends of a long changelog.
	CHECK(whatsnewMaxScroll(64, 21) == 43);
	CHECK(whatsnewClampScroll(-1,   64, 21) == 0);    // top
	CHECK(whatsnewClampScroll(0,    64, 21) == 0);
	CHECK(whatsnewClampScroll(43,   64, 21) == 43);   // bottom
	CHECK(whatsnewClampScroll(9999, 64, 21) == 43);

	// A degenerate view height cannot produce a negative bound.
	CHECK(whatsnewMaxScroll(10, 0) == 0);
	CHECK(whatsnewClampScroll(4, 10, 0) == 0);
}

// ── Scrollbar geometry ────────────────────────────────────────────────────────────────

static void testThumbWhenEverythingFits(void)
{
	WhatsNewThumb t;

	// The contract: full height, at the top, and no travel — the bar still reads as a bar.
	whatsnewThumb(10, 21, 0, 192, 12, &t);
	CHECK(t.y == 0);
	CHECK(t.h == 192);

	// Even asked for a scroll position it cannot have.
	whatsnewThumb(10, 21, 7, 192, 12, &t);
	CHECK(t.y == 0);
	CHECK(t.h == 192);

	whatsnewThumb(21, 21, 0, 192, 12, &t);
	CHECK(t.y == 0 && t.h == 192);
}

static void testThumbProportionAndTravel(void)
{
	WhatsNewThumb t;

	// 42 lines in a 21-line view: half the content is visible, so half the track.
	const int track = 192;
	whatsnewThumb(42, 21, 0, track, 12, &t);
	CHECK(t.h == 96);
	CHECK(t.y == 0);                       // at the top, the top edges meet
	CHECK(t.y + t.h <= track);

	// Fully scrolled: the bottom edges meet exactly. Off-by-one here is a thumb that hangs
	// out of its track or never reaches the end.
	const int max = whatsnewMaxScroll(42, 21);   // 21
	whatsnewThumb(42, 21, max, track, 12, &t);
	CHECK(t.h == 96);
	CHECK(t.y == track - t.h);
	CHECK(t.y + t.h == track);

	// Halfway down is halfway along the travel.
	whatsnewThumb(42, 21, max / 2, track, 12, &t);
	CHECK(t.y > 0 && t.y < track - t.h);
	CHECK(t.y == (track - 96) * (max / 2) / max);

	// A very long changelog cannot shrink the thumb below the minimum, and it still reaches
	// both ends of the track.
	whatsnewThumb(2000, 21, 0, track, 12, &t);
	CHECK(t.h == 12);
	CHECK(t.y == 0);
	whatsnewThumb(2000, 21, whatsnewMaxScroll(2000, 21), track, 12, &t);
	CHECK(t.h == 12);
	CHECK(t.y + t.h == track);

	// Monotonic: scrolling further down never moves the thumb up.
	int previous = -1;
	for (int s = 0; s <= max; s++) {
		whatsnewThumb(42, 21, s, track, 12, &t);
		CHECK(t.y >= previous);
		CHECK(t.y + t.h <= track);
		previous = t.y;
	}

	// A zero-height track is a degenerate caller, not a crash.
	whatsnewThumb(42, 21, 0, 0, 12, &t);
	CHECK(t.y == 0 && t.h == 0);
}

// ── Held-to-repeat ────────────────────────────────────────────────────────────────────

static void testRepeatRate(void)
{
	WhatsNewRepeat r = {0};

	// Not held: nothing, ever.
	for (int i = 0; i < 100; i++) CHECK(whatsnewRepeatStep(&r, false) == 0);

	// The press itself steps exactly once, then the hold-before-repeat gap is silent.
	CHECK(whatsnewRepeatStep(&r, true) == 1);
	for (int f = 1; f < WHATSNEW_REPEAT_DELAY; f++)
		CHECK(whatsnewRepeatStep(&r, true) == 0);

	// Then one step every WHATSNEW_REPEAT_PERIOD frames, starting on the delay frame itself.
	CHECK(whatsnewRepeatStep(&r, true) == 1);              // frame == DELAY
	for (int f = 1; f < WHATSNEW_REPEAT_PERIOD; f++)
		CHECK(whatsnewRepeatStep(&r, true) == 0);
	CHECK(whatsnewRepeatStep(&r, true) == 1);

	// Releasing rearms it: the next press steps immediately rather than inheriting the
	// mid-repeat cadence.
	CHECK(whatsnewRepeatStep(&r, false) == 0);
	CHECK(whatsnewRepeatStep(&r, true) == 1);

	// The measured total over a full second of holding, which is what the rate actually is:
	// 1 on the press, then one every PERIOD frames from frame DELAY onward.
	WhatsNewRepeat h = {0};
	int steps = 0;
	for (int f = 0; f < 60; f++) steps += whatsnewRepeatStep(&h, true);
	CHECK(steps == 1 + 1 + (60 - 1 - WHATSNEW_REPEAT_DELAY) / WHATSNEW_REPEAT_PERIOD);
	CHECK(steps == 11);

	// A NULL state is a caller bug, not a crash.
	CHECK(whatsnewRepeatStep(NULL, true) == 0);
}

// The two halves together: holding down for a second from a fresh screen lands inside the
// scroll range rather than past the end, which is the thing the player actually does.
static void testHoldThenClamp(void)
{
	WhatsNewRepeat r = {0};
	int scroll = 0;
	for (int f = 0; f < 600; f++) scroll += whatsnewRepeatStep(&r, true);

	CHECK(scroll > 0);
	CHECK(whatsnewClampScroll(scroll, 30, 21) == 9);
	CHECK(whatsnewClampScroll(scroll, 10, 21) == 0);
}

int main(void)
{
	testParseWellFormed();
	testParseHeaderTolerance();
	testParseEmptyFile();
	testParseNoSections();
	testParseSectionWithNoItems();
	testParseUnknownSection();
	testParseCrlf();
	testParseOverlongLine();
	testParseTooManyItems();
	testParseOversizeFile();
	testParseBinaryGarbage();

	testLayoutPlaceholder();
	testLayoutHeadingsAndBullets();
	testLayoutFitsColumn();
	testLayoutWordLongerThanLine();
	testLayoutReportsTruncation();
	testLayoutLineCap();

	testScrollClamp();
	testThumbWhenEverythingFits();
	testThumbProportionAndTravel();

	testRepeatRate();
	testHoldThenClamp();

	if (s_fails == 0)
		printf("whatsnew notes self-test: PASS  %d checks\n", s_checks);
	else
		printf("whatsnew notes self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. Same guard as app/updater_version_test.c — an empty
// translation unit is not valid ISO C, so the compiler gets one declaration to chew on.
typedef int whatsnew_test_host_only_t;

#endif   // !__3DS__
