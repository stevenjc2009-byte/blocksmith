#include "app/options.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Nothing in here includes <3ds.h>. Same reasoning as world/region.c: the SD card is a
// devoptab mounted at "sdmc:/", so plain fopen/fgets/fwrite reach it on console exactly as
// they reach a host temp directory in the test build — see options.h's file comment.

const uint32_t OPTIONS_VALID_KEYS[OPTIONS_VALID_KEY_COUNT] = {
	OPT_KEY_A, OPT_KEY_X, OPT_KEY_Y,
	OPT_KEY_DRIGHT, OPT_KEY_DLEFT, OPT_KEY_DUP, OPT_KEY_DDOWN,
};

// One string per OptionsAction, in the same order, so the two arrays can never disagree by
// accident — a mismatched pair here would silently save one action's binding under another
// action's name. ACTION_COUNT below is what catches the array from going out of sync in size.
static const char* const s_action_keys[ACTION_COUNT] = {
	"bind.move_forward",
	"bind.move_back",
	"bind.move_left",
	"bind.move_right",
	"bind.jump",
	"bind.break",
	"bind.place",
};

// The default binding for each action, same order again. This is the one place the game's
// current hardcoded buttons (player.c's D-pad reads, PLAYER_KEY_JUMP, INTERACT_KEY_BREAK,
// INTERACT_KEY_PLACE) are re-stated in terms of the host-safe OPT_KEY_* constants, so that a
// player who never opens the options page gets exactly the controls that shipped before this
// step existed.
static const uint32_t s_action_defaults[ACTION_COUNT] = {
	OPT_KEY_DUP, OPT_KEY_DDOWN, OPT_KEY_DLEFT, OPT_KEY_DRIGHT,
	OPT_KEY_A, OPT_KEY_X, OPT_KEY_Y,
};

// ── Defaults ───────────────────────────────────────────────────────────────────────────

void optionsDefaults(Options* o)
{
	if (!o) return;

	// renderDistDefault() needs to know new-3DS-or-not, which is a console question this
	// pure-C file cannot ask (see render_dist.h's own file comment: that split lives in
	// main.c). RENDER_DIST_MIN is the conservative side of that split — the Old 3DS radius
	// — so a fresh options.ini never claims more mesh-pool budget than every console can
	// actually give it. The console-side boot code is expected to raise this once, on the
	// very first run, the same way it already asks APT_CheckNew3DS for render_dist.c.
	o->render_dist      = RENDER_DIST_MIN;
	o->slider_3d        = OPTIONS_SLIDER_DEFAULT;
	o->invert_look      = false;
	o->look_sensitivity = OPTIONS_SENS_DEFAULT;

	for (int i = 0; i < ACTION_COUNT; i++)
		o->bindings[i] = s_action_defaults[i];
}

// ── Small parsing helpers ─────────────────────────────────────────────────────────────
//
// Deliberately not sscanf: sscanf's %f and %d both accept trailing garbage silently unless
// %n is threaded through every call site, which is exactly the "malformed value" case this
// module has to catch reliably. strtol/strtof plus an endptr check is the version that
// cannot be fooled by "42blah".

// Trims ASCII whitespace off both ends of `s` in place and returns it. Used on both the key
// and the value half of a line, so "  render_dist = 2  " and "render_dist=2" parse the same.
static char* trim(char* s)
{
	while (*s && isspace((unsigned char)*s)) s++;

	char* end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) end--;
	*end = '\0';

	return s;
}

// Whole-string integer parse. False on empty input or any trailing character that is not
// whitespace — "2" and " 2 " pass, "2x" and "" do not.
static bool parseInt(const char* s, long* out)
{
	if (!s || !*s) return false;

	char* end;
	long v = strtol(s, &end, 10);
	if (end == s) return false;
	while (*end && isspace((unsigned char)*end)) end++;
	if (*end) return false;

	*out = v;
	return true;
}

static bool parseFloat(const char* s, float* out)
{
	if (!s || !*s) return false;

	char* end;
	float v = strtof(s, &end);
	if (end == s) return false;
	while (*end && isspace((unsigned char)*end)) end++;
	if (*end) return false;

	*out = v;
	return true;
}

// Accepts the handful of spellings a human is likely to type as well as what optionsSave
// writes. Anything else is malformed rather than guessed at.
static bool parseBool(const char* s, bool* out)
{
	if (!s) return false;
	if (!strcmp(s, "1") || !strcmp(s, "true")  || !strcmp(s, "yes")) { *out = true;  return true; }
	if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "no"))  { *out = false; return true; }
	return false;
}

// A binding's value is a bit, written and read as 0xHHHHHHHH (see optionsSave) but accepted
// in decimal too, since a hand-edited file is exactly the case this module has to survive.
// Valid only if it is exactly one of OPTIONS_VALID_KEYS — a hid.h bit for a button this
// build does not bind is exactly as wrong as a typo, and both are malformed the same way
// rather than one of them being silently accepted as a binding that can never fire.
static bool parseKeyBit(const char* s, uint32_t* out)
{
	if (!s || !*s) return false;

	char* end;
	unsigned long v = strtoul(s, &end, 0);   // base 0: "0x400" and "1024" both work
	if (end == s) return false;
	while (*end && isspace((unsigned char)*end)) end++;
	if (*end) return false;

	for (int i = 0; i < OPTIONS_VALID_KEY_COUNT; i++)
		if (OPTIONS_VALID_KEYS[i] == (uint32_t)v) { *out = (uint32_t)v; return true; }

	return false;
}

// Clamps in `long` and only then narrows to `int`. strtol on a file holding "99999999999999"
// hands back LONG_MAX with the digits fully consumed — a valid parse by parseInt's own rule
// — and clamping *after* narrowing to int would be undefined behaviour on exactly that input,
// the one case this function exists to make safe.
static int clampLongToInt(long v, int lo, int hi)
{
	if (v < (long)lo) return lo;
	if (v > (long)hi) return hi;
	return (int)v;
}

static float clampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ── Crash-safe write ───────────────────────────────────────────────────────────────────

// "<path>.tmp", the same suffix world/region.c's compaction uses. Returns false only if
// `path` will not fit — a caller passing a path this long has bigger problems, and the
// alternative (silently truncating a save path) is not one of them.
static bool tmpPath(const char* path, char* out, size_t cap)
{
	return snprintf(out, cap, "%s.tmp", path) < (int)cap;
}

bool optionsSave(const Options* o, const char* path)
{
	if (!o || !path) return false;

	char tmp[512];
	if (!tmpPath(path, tmp, sizeof(tmp))) return false;

	FILE* f = fopen(tmp, "w");
	if (!f) return false;

	// Plain key=value, one per line, in struct order. Floats get enough digits to round-trip
	// a float32 (9 significant decimal digits is the documented bound); bindings are hex so
	// a hand-edited file reads the same shape hid.h's own KEY_* comments do.
	int n = 0;
	n += fprintf(f, "# blocksmith options — machine-written, safe to hand-edit\n");
	n += fprintf(f, "render_dist=%d\n",         o->render_dist);
	n += fprintf(f, "slider_3d=%.9g\n",         (double)o->slider_3d);
	n += fprintf(f, "invert_look=%d\n",         o->invert_look ? 1 : 0);
	n += fprintf(f, "look_sensitivity=%.9g\n",  (double)o->look_sensitivity);
	for (int i = 0; i < ACTION_COUNT; i++)
		// PRIX32 rather than a plain %08X: uint32_t is `unsigned int` on the host and
		// `long unsigned int` on devkitARM, so either fixed conversion is wrong on one of
		// the two and -Wformat says so. This file is compiled by both.
		n += fprintf(f, "%s=0x%08" PRIX32 "\n", s_action_keys[i], o->bindings[i]);

	// fprintf returning negative anywhere above means a write failed; n is only meaningful
	// as "did every call succeed", not as a byte count, since fprintf's return value is
	// signed and the calls above are summed rather than checked individually.
	const bool wrote_ok = n >= 0;

	// fclose is the flush: this is what makes the bytes actually reach the card rather than
	// sitting in stdio's buffer when the rename below runs.
	if (fclose(f) != 0 || !wrote_ok) { remove(tmp); return false; }

	// Windows' rename() (unlike POSIX's) refuses to replace an existing destination, so the
	// old file has to be removed first — exactly the reason world/region.c's regionCompact
	// removes its .bsr before renaming its .tmp over it. remove() on a path that does not
	// exist yet (first save ever) is allowed to fail; that failure is not this function's
	// failure.
	remove(path);

	if (rename(tmp, path) != 0) return false;

	// The window this leaves: a power cut between remove(path) and rename(tmp, path) leaves
	// no file at `path` and a complete `path.tmp` sitting next to it. optionsLoad's recovery
	// step below promotes that tmp back into place before it ever tries to open `path`, so
	// the next load — not just the next save — is what closes the window, same as
	// region.c's regionRecover does for region files.
	return true;
}

// Mirrors world/region.c's regionRecover: if the last save was cut between removing the old
// file and renaming the new one into place, `path` is gone and `path.tmp` is a complete,
// unopened replacement. Promoting it here means optionsLoad never has to know the difference
// between "never saved" and "saved, then interrupted right after".
static void optionsRecover(const char* path)
{
	char tmp[512];
	if (!tmpPath(path, tmp, sizeof(tmp))) return;

	FILE* t = fopen(tmp, "rb");
	if (!t) return;             // no interrupted save to recover
	fclose(t);

	FILE* real = fopen(path, "rb");
	if (real) { fclose(real); remove(tmp); return; }   // the real file is fine; drop the leftover tmp

	rename(tmp, path);
}

// ── Load ───────────────────────────────────────────────────────────────────────────────

// 256 covers every line this module ever writes with a lot of room left over; a longer
// hand-edited line is not a crash, just a line optionsLoad cannot use — see the loop below.
#define LINE_MAX 256

bool optionsLoad(Options* o, const char* path, int* bad_keys_out)
{
	if (!o || !path) return false;

	optionsDefaults(o);
	int bad = 0;

	optionsRecover(path);

	FILE* f = fopen(path, "r");
	if (!f) {
		// Missing file is not an error — see options.h. `o` is already the full default set.
		if (bad_keys_out) *bad_keys_out = 0;
		return true;
	}

	char line[LINE_MAX];
	while (fgets(line, sizeof(line), f)) {
		// A line fgets could not fit whole is not treated specially: whatever arrived gets
		// parsed as a (probably malformed) line, and the remainder appears as its own line
		// next iteration. Neither half can crash the parser below, which is all this
		// guarantees — a perfectly recovered value from a split line is not promised.

		char* s = trim(line);
		if (!*s || *s == '#') continue;               // blank line or comment

		char* eq = strchr(s, '=');
		if (!eq) continue;                             // not "key=value" shaped: ignored

		*eq = '\0';
		char* key = trim(s);
		char* val = trim(eq + 1);

		bool matched = false;

		if (!strcmp(key, "render_dist")) {
			matched = true;
			long v;
			if (parseInt(val, &v))
				o->render_dist = clampLongToInt(v, RENDER_DIST_MIN, RENDER_DIST_MAX);
			else
				bad++;
		} else if (!strcmp(key, "slider_3d")) {
			matched = true;
			float v;
			if (parseFloat(val, &v))
				o->slider_3d = clampFloat(v, OPTIONS_SLIDER_MIN, OPTIONS_SLIDER_MAX);
			else
				bad++;
		} else if (!strcmp(key, "invert_look")) {
			matched = true;
			bool v;
			if (parseBool(val, &v))
				o->invert_look = v;
			else
				bad++;
		} else if (!strcmp(key, "look_sensitivity")) {
			matched = true;
			float v;
			if (parseFloat(val, &v))
				o->look_sensitivity = clampFloat(v, OPTIONS_SENS_MIN, OPTIONS_SENS_MAX);
			else
				bad++;
		} else {
			for (int i = 0; i < ACTION_COUNT && !matched; i++) {
				if (strcmp(key, s_action_keys[i])) continue;
				matched = true;
				uint32_t v;
				if (parseKeyBit(val, &v))
					o->bindings[i] = v;
				else
					bad++;
			}
		}

		// An unrecognised key falls through with matched still false and is silently
		// ignored, per options.h — forward compatibility for a future key this build does
		// not know about yet.
	}

	fclose(f);

	if (bad_keys_out) *bad_keys_out = bad;
	return true;
}
