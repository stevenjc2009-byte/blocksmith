// Step 8.4 (settings half). Everything the title screen's options page reads and writes:
// render distance, the 3D slider, look invert and sensitivity, and the button bindings.
//
// Pure C, no <3ds.h>, same trick as world/region.h: libctru mounts the SD card as a
// devoptab under "sdmc:/", so fopen("sdmc:/blocksmith/options.ini") on console and
// fopen("build-host/options.ini") on the host go through the exact same code path here.
// That is the only reason a hand-edited or half-written ini can be tested at all — the
// host test truncates and corrupts a real file and reloads it, which an emulator run
// cannot do.
//
// Nothing in this file trusts the file on disk. A player (or a bug, or a card that lost
// power mid-write) can put anything in options.ini, and the worst that is allowed to
// happen is "this one setting came back as its default" — never a crash, never a value
// silently out of range that breaks the mesh pool or the fog LUT downstream.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "scene/render_dist.h"

// ── Actions ────────────────────────────────────────────────────────────────────────────
//
// Exactly the verbs the game reads today — nothing invented. Sourced from:
//   scene/player.c   playerUpdate(), lines 43-46: the four D-pad directions, hardcoded
//                    KEY_DUP/KEY_DDOWN/KEY_DLEFT/KEY_DRIGHT (walking is D-pad only; the
//                    circle pad is look, owned by cameraLook and not rebindable here).
//   scene/player.h   line 18: #define PLAYER_KEY_JUMP KEY_A
//   scene/interact.h line 58: #define INTERACT_KEY_BREAK KEY_X
//   scene/interact.h line 59: #define INTERACT_KEY_PLACE KEY_Y
//
// There is no sprint, crouch, or inventory verb anywhere in scene/ or world/ yet, so none
// is bound here. Adding a slot for a verb the game cannot fire yet would load, save and
// clamp cleanly and just never do anything — worse than no slot, because it looks finished.
// The day a new verb lands, its action goes on the end of this enum, never in the middle —
// the ini keys below are matched by name (see s_action_keys in options.c), not by enum
// value, so an old options.ini still binds every action it names correctly even after a
// new one is inserted, as long as existing entries keep their position or their name.
typedef enum {
	ACTION_MOVE_FORWARD,   // KEY_DUP
	ACTION_MOVE_BACK,      // KEY_DDOWN
	ACTION_MOVE_LEFT,      // KEY_DLEFT
	ACTION_MOVE_RIGHT,     // KEY_DRIGHT
	ACTION_JUMP,           // KEY_A
	ACTION_BREAK,          // KEY_X
	ACTION_PLACE,          // KEY_Y
	ACTION_COUNT
} OptionsAction;

// ── Key bits ───────────────────────────────────────────────────────────────────────────
//
// A binding has to be a plain uint32_t, not a libctru u32/enum, because this header (and
// options.c) has to compile on the host. So the bit values libctru's hid.h assigns are
// duplicated here as hex literals rather than included. Verified against the installed
// devkitPro copy of the header, C:\devkitPro\libctru\include\3ds\services\hid.h:
//
//   KEY_A       BIT(0)   = 0x00000001
//   KEY_X       BIT(10)  = 0x00000400
//   KEY_Y       BIT(11)  = 0x00000800
//   KEY_DRIGHT  BIT(4)   = 0x00000010
//   KEY_DLEFT   BIT(5)   = 0x00000020
//   KEY_DUP     BIT(6)   = 0x00000040
//   KEY_DDOWN   BIT(7)   = 0x00000080
//
// If hid.h's BIT() numbering ever changes upstream these go stale silently — nothing here
// can detect that without including <3ds.h>, which is the one thing this file cannot do.
// The console-side wiring code (not part of this step) is the place to add a static-assert
// against the real KEY_* constants before these are ever passed to hidKeysDown().
#define OPT_KEY_A       0x00000001u
#define OPT_KEY_X       0x00000400u
#define OPT_KEY_Y       0x00000800u
#define OPT_KEY_DRIGHT  0x00000010u
#define OPT_KEY_DLEFT   0x00000020u
#define OPT_KEY_DUP     0x00000040u
#define OPT_KEY_DDOWN   0x00000080u

// The set a binding is allowed to hold. A value read from the ini that is not one of
// these is treated as malformed (see optionsLoad) rather than accepted verbatim — an
// unrecognised bit would silently never fire, which is the same failure mode as the
// invented-action trap above, just reachable from a hand-edited file instead of from code.
#define OPTIONS_VALID_KEY_COUNT 7
extern const uint32_t OPTIONS_VALID_KEYS[OPTIONS_VALID_KEY_COUNT];

// ── Ranges ─────────────────────────────────────────────────────────────────────────────
//
// Render distance has its own real min/max in scene/render_dist.h (RENDER_DIST_MIN/MAX) —
// used directly below, not copied, so this file cannot drift from the mesh-pool budget
// that number is actually load-bearing for.
//
// The 3D slider mirrors the hardware slider's own range: 0 is off, 1 is full depth. There
// is no stereoscopic renderer yet (nothing under source/gfx or source/scene draws a right
// eye), so this is a preference stored ahead of that step rather than one wired to
// anything today — unlike render distance and the bindings, which are read by code that
// exists right now.
#define OPTIONS_SLIDER_MIN      0.0f
#define OPTIONS_SLIDER_MAX      1.0f
#define OPTIONS_SLIDER_DEFAULT  0.0f

// Sensitivity is a multiplier on camera.c's LOOK_SPEED (2.6 rad/s at full stick), not a
// replacement for it — camera.c is not one of this step's three files, so the multiply
// happens on the console side, wherever LOOK_SPEED is read. 1.0 reproduces today's feel
// exactly; the range is wide enough to matter and narrow enough that the low end still
// turns at all and the high end is not a whole-screen flick from a twitch of the stick.
#define OPTIONS_SENS_MIN      0.25f
#define OPTIONS_SENS_MAX      3.0f
#define OPTIONS_SENS_DEFAULT  1.0f

typedef struct {
	int      render_dist;                 // columns radius; RENDER_DIST_MIN..RENDER_DIST_MAX
	float    slider_3d;                   // OPTIONS_SLIDER_MIN..OPTIONS_SLIDER_MAX
	bool     invert_look;
	float    look_sensitivity;            // OPTIONS_SENS_MIN..OPTIONS_SENS_MAX
	uint32_t bindings[ACTION_COUNT];      // action -> raw key bit, one of OPTIONS_VALID_KEYS
	bool     debug_menu;                  // debug menu on/off
} Options;

// Fills `o` with the shipped defaults. Every field is already inside its legal range —
// optionsLoad calls this first and then overlays whatever the file can prove is good, so
// "the file does not exist" and "every key in the file was garbage" produce the identical,
// fully-valid Options this function alone would produce.
void optionsDefaults(Options* o);

// Loads `path` into `o`. Always leaves `o` fully valid and always returns true, *except*
// when `o` or `path` is NULL, which is a caller bug rather than a file-format problem and
// is reported as false rather than silently defaulting.
//
//   - A missing file is not an error: `o` comes back as optionsDefaults() would leave it.
//   - `#` comments and blank lines are skipped; leading/trailing whitespace around the
//     key and the value is trimmed.
//   - An unrecognised key is ignored, not an error — this is what lets an older save
//     survive a newer build adding a key, and a newer save survive an older build that
//     does not know it yet.
//   - A recognised key whose value cannot be parsed at all (wrong type, empty, garbage)
//     falls back to that field's default and is counted in `*bad_keys_out` (may be NULL).
//   - A recognised key that parses but is out of range (a render_dist of 9999, a
//     sensitivity of -4) is *clamped* into range rather than defaulted — a value that was
//     nearly right stays nearly right instead of jumping all the way back to default. This
//     is not counted as a bad key: the value on disk was a real number, just out of bounds.
//   - A truncated or binary-garbage file cannot crash this: every line is bounded, every
//     parse is checked before use, and any line that is not "key=value" shaped is skipped
//     exactly like an unrecognised key.
bool optionsLoad(Options* o, const char* path, int* bad_keys_out);

// Writes `o` to `path`. Crash-safe the same way world/region.c's compaction is: the new
// bytes are written whole to "<path>.tmp" and flushed out of stdio's buffer via fclose, the
// real path is removed, and only then is the tmp renamed over it. See the comment on
// optionsSave in options.c for why the remove has to happen before the rename rather than
// after.
//
// False on any IO failure (could not open the tmp file, a short write, the final rename
// failing) — the caller counts this rather than retries, same as a failed region write.
// On false, the previous options.ini (if any) is left exactly as it was: nothing here ever
// touches the real path until the replacement is known-good and closed.
bool optionsSave(const Options* o, const char* path);
