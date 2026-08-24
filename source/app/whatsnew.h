// The release notes the updater shows before it downloads anything — v1.6.0 task 14b.
//
// steve's brief, in his words: "whenever I check for a new update and it says new version
// found, I want there to be a simplified little changelog for the 3DS that shows up on the
// actual screen... the top one... features added are this, and then bugs fixed... scroll
// down with the down and up D-pad and have a little slider... so the player knows what
// they're downloading."
//
// ── Where the text comes from ─────────────────────────────────────────────────────────
//
// A second asset on the GitHub release, `whatsnew<version>.txt`, fetched from the same
// predictable /releases/download/<tag>/ path the .cia already comes from (see
// app/updater.c's buildAssetUrl and updaterBuildNotesName in app/updater_version.h).
//
// NOT api.github.com, and not the release body. api.github.com allows sixty unauthenticated
// requests an hour per IP address, shared by everything on the connection, and this project
// has already been bitten by that once — app/updater.c's own comment above checkViaRedirect
// records it. The whole updater was rebuilt around the /releases/latest 302 redirect for
// that reason, and a second rationed request would put the ceiling straight back.
//
// ── Everything here is pure C on purpose ──────────────────────────────────────────────
//
// No <3ds.h>, no libcurl, no citro3d. The parse, the word wrap, the scroll clamp, the
// scrollbar geometry and the held-to-repeat timing are all arithmetic, so tests/whatsnew_test.c
// links THIS file and proves the shipping copy rather than a hand-written twin of it — the
// same split app/battery.c and app/debugmenu_ui.c already use, and for the reason recorded in
// tools/run_host_tests.sh: before v1.6.0 both of those had hand-copied test logic that stayed
// green with the real module deleted.
//
// The fetch lives in app/updater.c (it needs libcurl and the worker thread) and the drawing
// lives in scene/title.c (it needs citro3d). Neither of them makes a decision this file does
// not already make and get tested on.
//
// ── The caps, and why every one of them exists ────────────────────────────────────────
//
// This is data arriving over the network onto a console with a 12 MB app heap and no virtual
// memory. Nothing here allocates: every buffer below is a fixed array sized at compile time,
// so a hostile or corrupt file cannot make the console reserve anything. Each cap truncates
// visibly — the reader is told the notes were cut — and never overruns.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// The most of a whatsnew file that is ever read. 4 KB is roughly sixty full-width lines of
// a 400 px screen, which is already far more than anyone will scroll through; the fetch in
// app/updater.c stops copying past this and whatsnewParse() ignores anything beyond it, so
// the two agree on the number rather than each having an opinion.
#define WHATSNEW_BYTES_MAX   4096

// The most items (feature lines plus fix lines) that are kept. Past this, parsing stops and
// the notes are flagged truncated.
#define WHATSNEW_ITEMS_MAX   32

// Bytes per item, terminator included. An item longer than this is cut and its last three
// characters replaced with "..." so the cut is visible rather than silent.
#define WHATSNEW_ITEM_CHARS  96

// The most wrapped display lines the layout ever produces, and the buffer each one gets.
// 72 is comfortably past the 61 characters a 366 px text column fits at gfx/font.h's
// FONT_ADVANCE of 6, so the wrap width can be tightened later without resizing this.
#define WHATSNEW_LINES_MAX   64
#define WHATSNEW_LINE_CHARS  72

// Held-to-repeat timing for D-pad scrolling, in frames at the project's measured 59.83 fps.
// Explicit constants rather than feel: the press itself steps once, then nothing for
// WHATSNEW_REPEAT_DELAY frames (~0.33 s), then one step every WHATSNEW_REPEAT_PERIOD frames
// (~15 lines a second). Both are exercised directly by tests/whatsnew_test.c.
#define WHATSNEW_REPEAT_DELAY   20
#define WHATSNEW_REPEAT_PERIOD  4

// Which group an item belongs to. The file format has exactly two, because the brief asks
// for exactly two.
typedef enum {
	WN_SECTION_NONE = 0,
	WN_SECTION_FEATURES,
	WN_SECTION_FIXES,
} WhatsNewSection;

typedef struct {
	char          text[WHATSNEW_ITEM_CHARS];
	unsigned char section;   // WhatsNewSection
} WhatsNewItem;

// The parsed file. Fixed size, ~3 KB, so it can live as a static in app/updater.c and be
// handed out by pointer without anything owning a heap block across threads.
typedef struct {
	WhatsNewItem items[WHATSNEW_ITEMS_MAX];
	int  count;
	int  feature_count;
	int  fix_count;
	bool truncated;   // a cap was hit: bytes, item count or item length
	bool malformed;   // something in the file was not understood and was dropped
} WhatsNew;

// What a wrapped display line is for, so the caller can colour it without re-deciding.
typedef enum {
	WN_LINE_HEADING = 0,   // "FEATURES ADDED" / "BUGS FIXED"
	WN_LINE_ITEM,          // an item's first line, carrying the bullet
	WN_LINE_CONT,          // an item's continuation line, indented under the bullet
	WN_LINE_NOTE,          // the placeholder, or the "notes were cut" footnote
	WN_LINE_BLANK,         // spacer between the two groups
} WhatsNewLineKind;

typedef struct {
	char          lines[WHATSNEW_LINES_MAX][WHATSNEW_LINE_CHARS];
	unsigned char kind[WHATSNEW_LINES_MAX];   // WhatsNewLineKind
	int           count;
	bool          truncated;   // the line cap was hit while wrapping
} WhatsNewLayout;

// Where the scrollbar's thumb goes inside its track, in pixels from the track's top.
typedef struct { int y, h; } WhatsNewThumb;

// Empties `wn` to the "no notes at all" state — which is exactly what the screen shows when
// a release carries no whatsnew asset, so this is the fail-soft resting state, not an error.
void whatsnewClear(WhatsNew* wn);

// Parses `len` bytes of a whatsnew file into `wn`, which is cleared first.
//
// The format, deliberately trivial so a person can write one in a text editor:
//
//   # lines starting with a hash are comments
//   [features]
//   Registry-defined blocks now work in multiplayer.
//   - a leading dash or star is optional and is stripped
//   [fixes]
//   The block texture sheet was rebuilt.
//
// Section headers are matched case-insensitively; leading and trailing whitespace on any
// line is ignored; CRLF and LF both work. Bytes outside printable ASCII (32..126) become
// '?', because gfx/font.h draws nothing at all for them and a silent gap reads as a bug in
// the game rather than as junk in the file — which is also what makes binary garbage safe
// to hand to this.
//
// An item before any section header is dropped and `malformed` is set: there is no sensible
// group to put it in, and guessing would file a bug fix under features.
//
// `text` may be NULL or `len` zero; the result is simply the empty state.
void whatsnewParse(const char* text, size_t len, WhatsNew* wn);

// True when there is at least one item to show. False means the caller should draw the
// placeholder — which whatsnewBuildLayout() already does on its own, so this is only for a
// caller that wants to word it differently.
bool whatsnewAny(const WhatsNew* wn);

// The one line shown when a release has no notes: honest, short, and never an error, because
// every release published before this feature existed is in exactly this position and must
// still update normally.
const char* whatsnewPlaceholder(void);

// Wraps `wn` into display lines that fit `wrap_px` pixels of a font whose per-character
// advance is `advance_px` (gfx/font.h's FONT_ADVANCE times the draw scale). Real metrics,
// not a guessed character count — this font is fixed-pitch, so a column count IS the pixel
// measurement, and passing the real numbers keeps it that way if the column ever moves.
//
// A word longer than the column is hard-split rather than allowed to overrun. A NULL or
// empty `wn` yields the placeholder line, so the caller never has to special-case it.
void whatsnewBuildLayout(const WhatsNew* wn, int wrap_px, int advance_px, WhatsNewLayout* out);

// The largest first-visible-line index that still fills the view, i.e. how far down the
// player can scroll. Zero when everything already fits.
int whatsnewMaxScroll(int line_count, int visible_lines);

// `scroll` brought into 0..whatsnewMaxScroll(). Call every frame rather than only when the
// player presses something: the content can change under the scroll position when a check
// finishes.
int whatsnewClampScroll(int scroll, int line_count, int visible_lines);

// True when there is anything below the fold — the scrollbar draws as an inert full-height
// bar when this is false, which is the visual difference between "you have seen it all" and
// "there is more".
bool whatsnewScrollable(int line_count, int visible_lines);

// The scrollbar thumb's rectangle inside a `track_h`-pixel track, honouring `min_thumb_h` so
// a very long changelog does not shrink it to something invisible.
//
// When everything fits the thumb is the whole track (y 0, h track_h) — deliberately, so the
// bar always reads as a bar rather than vanishing. Otherwise it is proportioned by
// visible/total and positioned so scroll 0 puts its top edge at the track's top and full
// scroll puts its bottom edge exactly on the track's bottom.
void whatsnewThumb(int line_count, int visible_lines, int scroll,
                   int track_h, int min_thumb_h, WhatsNewThumb* out);

// Held-to-repeat state for one direction. Zero-initialise it; nothing else to do.
typedef struct { int held_frames; } WhatsNewRepeat;

// One frame of a held D-pad direction. Returns 1 on a frame the view should step by one
// line, 0 otherwise.
//
// Driven from hidKeysHeld(), never hidKeysDown(). That is not a detail: this project has
// twice shipped a bug where one hidKeysDown() word was consumed by two UI layers in the same
// loop iteration (app/debugmenu_ui.c's opening A press also stepping the render-distance
// slider is the recorded one). Scrolling reads *held* and the buttons on the same screen read
// *down* on different bits, so the two layers cannot take the same press. The rising edge is
// synthesised here from held_frames == 0, so the caller must not also feed it the down bit.
int whatsnewRepeatStep(WhatsNewRepeat* r, bool held);
