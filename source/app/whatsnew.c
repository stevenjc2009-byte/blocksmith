#include "app/whatsnew.h"

#include <string.h>

// Pure C, nothing else. See app/whatsnew.h for why this file has no <3ds.h> in it and what
// tests/whatsnew_test.c is therefore able to prove about the code that actually ships.

// ── Parsing ───────────────────────────────────────────────────────────────────────────

void whatsnewClear(WhatsNew* wn)
{
	if (!wn) return;
	memset(wn, 0, sizeof(*wn));
}

bool whatsnewAny(const WhatsNew* wn)
{
	return wn != NULL && wn->count > 0;
}

const char* whatsnewPlaceholder(void)
{
	return "No change notes for this version.";
}

static char lowerAscii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive compare of a counted slice against a NUL-terminated word. Written out
// rather than reached for via strncasecmp because that one is POSIX, not ISO C, and this
// file compiles under both devkitARM and the host's -std=c11.
static bool sliceEqualsWord(const char* slice, size_t n, const char* word)
{
	size_t i = 0;
	for (; i < n; i++) {
		if (word[i] == '\0') return false;
		if (lowerAscii(slice[i]) != lowerAscii(word[i])) return false;
	}
	return word[i] == '\0';
}

// Copies one item's text in, sanitising and capping. `n` is the source length; anything past
// WHATSNEW_ITEM_CHARS-1 is dropped and the tail replaced with "..." so the cut is on screen
// rather than only in the parser's conscience.
static bool copyItemText(const char* src, size_t n, char* dst)
{
	const size_t cap = WHATSNEW_ITEM_CHARS - 1;
	const size_t take = (n <= cap) ? n : cap;

	for (size_t k = 0; k < take; k++) {
		const unsigned char c = (unsigned char)src[k];
		// gfx/font.h draws nothing for anything outside 32..126 and still advances, so an
		// unprintable byte would leave a mystery gap. '?' says "there was a byte here".
		dst[k] = (c >= 32 && c <= 126) ? (char)c : '?';
	}
	dst[take] = '\0';

	if (n > cap) {
		dst[cap - 3] = '.';
		dst[cap - 2] = '.';
		dst[cap - 1] = '.';
		return true;   // truncated
	}
	return false;
}

// One already-trimmed, non-empty line. `section` is carried across calls.
static void handleLine(const char* s, size_t n, int* section, WhatsNew* wn)
{
	if (s[0] == '#') return;   // comment

	if (s[0] == '[' && s[n - 1] == ']') {
		const char*  inner = s + 1;
		const size_t inner_n = n - 2;

		if (sliceEqualsWord(inner, inner_n, "features")) {
			*section = WN_SECTION_FEATURES;
		} else if (sliceEqualsWord(inner, inner_n, "fixes")) {
			*section = WN_SECTION_FIXES;
		} else {
			// A header nobody knows. Falling back to the previous section would file the
			// lines under it silently and wrongly, so the section is closed instead and the
			// lines that follow are dropped the same way stray lines are.
			*section = WN_SECTION_NONE;
			wn->malformed = true;
		}
		return;
	}

	if (*section == WN_SECTION_NONE) {
		// No group to put it in. Guessing would file a bug fix under features.
		wn->malformed = true;
		return;
	}

	if (wn->count >= WHATSNEW_ITEMS_MAX) {
		wn->truncated = true;
		return;
	}

	// An optional bullet, because a person writing one of these by hand will type one about
	// half the time and both halves should look the same on screen.
	if ((s[0] == '-' || s[0] == '*') && n >= 2 && (s[1] == ' ' || s[1] == '\t')) {
		s += 2;
		n -= 2;
		while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
	}
	if (n == 0) return;

	WhatsNewItem* item = &wn->items[wn->count];
	if (copyItemText(s, n, item->text)) wn->truncated = true;
	item->section = (unsigned char)*section;

	if (*section == WN_SECTION_FEATURES) wn->feature_count++;
	else                                  wn->fix_count++;
	wn->count++;
}

void whatsnewParse(const char* text, size_t len, WhatsNew* wn)
{
	whatsnewClear(wn);
	if (!wn || !text || len == 0) return;

	if (len > WHATSNEW_BYTES_MAX) {
		// Read the front of it and say so. Refusing outright would turn a fat file into no
		// notes at all, which is the same outcome as a missing file and tells the player
		// less than a cut one does.
		len = WHATSNEW_BYTES_MAX;
		wn->truncated = true;
	}

	int section = WN_SECTION_NONE;
	size_t i = 0;

	while (i < len) {
		size_t end = i;
		while (end < len && text[end] != '\n') end++;
		const size_t next = (end < len) ? end + 1 : len;

		// Trim both ends. The trailing pass also eats the '\r' of a CRLF file, which is what
		// a file authored on Windows — this project's own machine — will always be.
		size_t start = i;
		while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ' ||
		                       text[end - 1] == '\t')) end--;
		while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;

		if (start < end) handleLine(text + start, end - start, &section, wn);

		i = next;
	}
}

// ── Word wrap ─────────────────────────────────────────────────────────────────────────

static void pushLine(WhatsNewLayout* o, int kind, const char* prefix,
                     const char* body, size_t n)
{
	if (o->count >= WHATSNEW_LINES_MAX) {
		o->truncated = true;
		return;
	}

	char*  dst = o->lines[o->count];
	size_t w   = 0;

	for (const char* p = prefix; p && *p && w + 1 < WHATSNEW_LINE_CHARS; p++) dst[w++] = *p;
	for (size_t k = 0; k < n && w + 1 < WHATSNEW_LINE_CHARS; k++)            dst[w++] = body[k];

	dst[w] = '\0';
	o->kind[o->count] = (unsigned char)kind;
	o->count++;
}

// Greedy wrap of one item. `cols` is the column budget the prefix has to fit inside too, so
// a continuation line lines up under its bullet instead of under the margin.
static void wrapInto(WhatsNewLayout* o, const char* prefix_first, const char* prefix_cont,
                     int kind_first, int kind_cont, const char* text, int cols)
{
	const size_t plen_first = strlen(prefix_first);
	const size_t plen_cont  = strlen(prefix_cont);
	const size_t len        = strlen(text);

	if (len == 0) {
		pushLine(o, kind_first, prefix_first, "", 0);
		return;
	}

	size_t at    = 0;
	bool   first = true;

	while (at < len) {
		const size_t plen = first ? plen_first : plen_cont;
		// At least one character always goes out, or a narrow column would spin here forever.
		const size_t avail = ((size_t)cols > plen) ? (size_t)cols - plen : 1;
		const size_t remain = len - at;

		size_t take;
		if (remain <= avail) {
			take = remain;
		} else {
			size_t brk = 0;
			for (size_t k = 0; k < avail; k++)
				if (text[at + k] == ' ') brk = k;
			// The character just past the column: if it is a space the whole run fits exactly.
			if (text[at + avail] == ' ') brk = avail;
			// brk == 0 means a single word longer than the column. Hard-split it — the
			// alternative is a line that overruns the panel it was measured for.
			take = (brk > 0) ? brk : avail;
		}

		pushLine(o, first ? kind_first : kind_cont, first ? prefix_first : prefix_cont,
		         text + at, take);

		at += take;
		while (at < len && text[at] == ' ') at++;
		first = false;

		if (o->count >= WHATSNEW_LINES_MAX) {
			// Bailing out is not by itself a truncation — the content may have ended exactly
			// on the last line. It is one only if there is text left with nowhere to put it,
			// and the flag has to be set HERE rather than left to the next pushLine: if this
			// was the final item, there is no next pushLine and the cut would go unreported.
			if (at < len) o->truncated = true;
			return;
		}
	}
}

void whatsnewBuildLayout(const WhatsNew* wn, int wrap_px, int advance_px, WhatsNewLayout* out)
{
	if (!out) return;
	memset(out, 0, sizeof(*out));

	// The real metric: this font is fixed-pitch (gfx/font.h's FONT_ADVANCE), so dividing the
	// pixel column by the advance IS the character budget rather than an estimate of it.
	int cols = (advance_px > 0) ? (wrap_px / advance_px) : 0;
	if (cols < 1)                        cols = 1;
	if (cols > WHATSNEW_LINE_CHARS - 1)  cols = WHATSNEW_LINE_CHARS - 1;

	if (!whatsnewAny(wn)) {
		wrapInto(out, "", "", WN_LINE_NOTE, WN_LINE_NOTE, whatsnewPlaceholder(), cols);
		return;
	}

	if (wn->feature_count > 0) {
		wrapInto(out, "", "", WN_LINE_HEADING, WN_LINE_HEADING, "FEATURES ADDED", cols);
		for (int i = 0; i < wn->count; i++) {
			if (wn->items[i].section != WN_SECTION_FEATURES) continue;
			wrapInto(out, "- ", "  ", WN_LINE_ITEM, WN_LINE_CONT, wn->items[i].text, cols);
		}
	}

	if (wn->feature_count > 0 && wn->fix_count > 0)
		pushLine(out, WN_LINE_BLANK, "", "", 0);

	if (wn->fix_count > 0) {
		wrapInto(out, "", "", WN_LINE_HEADING, WN_LINE_HEADING, "BUGS FIXED", cols);
		for (int i = 0; i < wn->count; i++) {
			if (wn->items[i].section != WN_SECTION_FIXES) continue;
			wrapInto(out, "- ", "  ", WN_LINE_ITEM, WN_LINE_CONT, wn->items[i].text, cols);
		}
	}

	// Say it on screen when a cap bit. A silently shortened changelog is the one outcome
	// worse than a short one.
	if (wn->truncated || out->truncated) {
		pushLine(out, WN_LINE_BLANK, "", "", 0);
		wrapInto(out, "", "", WN_LINE_NOTE, WN_LINE_NOTE,
		         "(these notes were too long to show in full)", cols);
	}
}

// ── Scrolling and the scrollbar ───────────────────────────────────────────────────────

int whatsnewMaxScroll(int line_count, int visible_lines)
{
	if (visible_lines <= 0) return 0;
	return (line_count > visible_lines) ? (line_count - visible_lines) : 0;
}

int whatsnewClampScroll(int scroll, int line_count, int visible_lines)
{
	const int max = whatsnewMaxScroll(line_count, visible_lines);
	if (scroll < 0)   return 0;
	if (scroll > max) return max;
	return scroll;
}

bool whatsnewScrollable(int line_count, int visible_lines)
{
	return whatsnewMaxScroll(line_count, visible_lines) > 0;
}

void whatsnewThumb(int line_count, int visible_lines, int scroll,
                   int track_h, int min_thumb_h, WhatsNewThumb* out)
{
	if (!out) return;

	out->y = 0;
	out->h = (track_h > 0) ? track_h : 0;
	if (track_h <= 0) return;

	const int max_scroll = whatsnewMaxScroll(line_count, visible_lines);
	if (max_scroll == 0) return;   // everything fits: full-height and inert, by design

	// long, not int: track_h * visible_lines is at most a few thousand here, but the cast
	// costs nothing and the alternative is an overflow that depends on how long a changelog
	// somebody writes.
	int h = (int)(((long)track_h * (long)visible_lines) / (long)line_count);
	if (h < min_thumb_h) h = min_thumb_h;
	if (h > track_h)     h = track_h;

	const int travel = track_h - h;
	const int at     = whatsnewClampScroll(scroll, line_count, visible_lines);

	int y = (int)(((long)travel * (long)at) / (long)max_scroll);
	if (y < 0)      y = 0;
	if (y > travel) y = travel;

	out->y = y;
	out->h = h;
}

// ── Held-to-repeat ────────────────────────────────────────────────────────────────────

int whatsnewRepeatStep(WhatsNewRepeat* r, bool held)
{
	if (!r) return 0;

	if (!held) {
		r->held_frames = 0;
		return 0;
	}

	const int f = r->held_frames++;

	if (f == 0)                       return 1;   // the press itself
	if (f < WHATSNEW_REPEAT_DELAY)    return 0;   // the pause before it starts repeating
	return ((f - WHATSNEW_REPEAT_DELAY) % WHATSNEW_REPEAT_PERIOD == 0) ? 1 : 0;
}
