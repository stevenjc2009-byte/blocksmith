#include "scene/title.h"

#include <stdio.h>
#include <string.h>

#include "gfx/font.h"
#include "gfx/sprite.h"
#include "world/region.h"

// ── Layout ─────────────────────────────────────────────────────────────────────────────
//
// Every size below is checked against the bottom touch panel's real physical dimensions —
// commonly documented as 53.0 x 39.75 mm for its 320x240 active area, i.e. 6.04 px/mm on
// both axes and evenly so (320/53.0 = 6.038, 240/39.75 = 6.038) — so a pixel count and a
// millimetre figure can be compared honestly instead of guessed at. The design brief for
// this whole file is that the touch UI is *designed* for touch, not adapted to it, so every
// rectangle below is commented with why it is the size it is, not just what the size is.
#define SCR_W 320
#define SCR_H 240

#define TITLE_BAR_H  20   // a label only, never tapped — not held to the finger-size budget below

// Title screen's three primary actions: the buttons a thumb has to land on cold, with
// nothing smaller nearby to fall back on. 48 px = 7.9 mm, inside the 7-10 mm fingertip
// target range general touch-UI guidance converges on (Apple's HIG cites roughly 7 mm,
// Android's Material roughly 9 mm) and the most three stacked buttons plus margins can
// afford on a 240 px screen (3*48 + 2*12 + 40(title+margin) = 232, 8 px of slack).
#define MAIN_BTN_H  48
#define MAIN_GAP    12

// The shared shape behind world-select and options-general: a handful of content rows,
// then two buttons pinned to the bottom of the screen. Solved once, algebraically, rather
// than eyeballed per screen — see the two screens below for what LIST_VISIBLE_ROWS actually
// holds in each case (worlds, or the four general settings).
//
//   240 = TITLE_BAR_H(20) + gap(4) + rows * LIST_ROW_H + gap(6)
//         + LIST_BTN_H + gap(4) + LIST_BTN_H + margin(6)
//
// With 4 rows and two 36 px buttons that is 40 + 4*LIST_ROW_H + 72 = 240, so
// LIST_ROW_H = 32 (5.3 mm) is not a round number chosen for looks — it is what is left
// after both pinned buttons and every gap are accounted for. 36 px (6.0 mm) for the pinned
// buttons is short of the title screen's 48 px on purpose: those two are reached far less
// often (New World / Controls, and Back) than the title screen's three, so the smaller
// target buys back the vertical space a list needs.
#define LIST_TOP_Y         (TITLE_BAR_H + 4)
#define LIST_ROW_H         32
#define LIST_BTN_H         36
#define LIST_VISIBLE_ROWS  4
#define LIST_BTN1_Y        (LIST_TOP_Y + LIST_VISIBLE_ROWS * LIST_ROW_H + 6)
#define LIST_BTN2_Y         (LIST_BTN1_Y + LIST_BTN_H + 4)

// Bindings is the one screen this file could not make comfortable. ACTION_COUNT (7 today,
// app/options.h says more may be appended later, never removed) content rows plus a single
// pinned Back button have to fit the same 240 px, which leaves:
//
//   BIND_ROW_H = (240 - LIST_TOP_Y(24) - gap(6) - LIST_BTN_H(36) - margin(6)) / ACTION_COUNT
//
// = 168 / 7 = 24 px today (4.0 mm) — under every other target in this file, and the reason
// it is a formula rather than a literal: if a future step appends an eighth verb (see
// app/options.h's own comment on why that enum only ever grows), this recomputes instead
// of silently overlapping rows. It stays a *list* tap (the whole 300-px-wide row is the
// target, same Fitts's-law reasoning LIST_ROW_H's comment makes — area, not height, is
// what a touch target is scored on) rather than a small icon, and reaching any row from a
// controller costs exactly the same D-pad presses it would if the row were twice as tall —
// which is the actual mitigation here: nothing on this screen *requires* the undersized tap.
#define BIND_ROW_Y   LIST_TOP_Y
#define BIND_ROW_H   ((SCR_H - LIST_TOP_Y - 6 - LIST_BTN_H - 6) / ACTION_COUNT)
#define BIND_BACK_Y  (BIND_ROW_Y + ACTION_COUNT * BIND_ROW_H + 6)

// ── Palette ────────────────────────────────────────────────────────────────────────────

#define COL_BG        SPRITE_RGBA(26, 20, 36, 255)
#define COL_PANEL     SPRITE_RGBA(52, 40, 76, 255)
#define COL_PANEL_HI  SPRITE_RGBA(90, 74, 130, 255)    // focused row/button fill
#define COL_PANEL_LO  SPRITE_RGBA(38, 30, 58, 255)      // the small -/+ chips inside a row
#define COL_TEXT      SPRITE_WHITE
#define COL_TEXT_DIM  SPRITE_RGBA(180, 170, 200, 255)
#define COL_ACCENT    SPRITE_RGBA(255, 226, 150, 255)
#define COL_WARN      SPRITE_RGBA(255, 140, 140, 255)

// ── Small shared helpers ──────────────────────────────────────────────────────────────

typedef struct { float x, y, w, h; } TRect;

static bool ptIn(TRect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Draws one button/row and reports whether it was activated this frame: a tap landing
// inside it this frame, or A while it is the D-pad-focused item. `focused` only changes
// the fill colour and draws the accent-coloured top edge — the hit test itself does not
// depend on it, so a tap always works regardless of where the D-pad cursor happens to be.
static bool uiButton(TRect r, const char* label, bool focused, bool tap, int tx, int ty,
                      bool a_down)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);

	const int scale = ((int)r.h >= 40) ? 2 : 1;
	const int tw = fontTextWidth(label, scale);
	const float lx = r.x + (r.w - (float)tw) * 0.5f;
	const float ly = r.y + (r.h - (float)(FONT_GLYPH_H * scale)) * 0.5f;
	fontDraw(lx, ly, scale, COL_TEXT, label);

	const bool tapped_here = tap && ptIn(r, tx, ty);
	return tapped_here || (focused && a_down);
}

// A settings row with a numeric value: label left, value and a "-"/"+" chip pair right.
// D-pad Left/Right do the same thing as the chips while the row is focused, and are the
// row's *primary* control from a controller — the chips are genuinely small (22x(h-6), the
// same trade-off BIND_ROW_H's comment makes for the bindings screen) because four of these
// stack in the same budget a single title-screen button gets. Returns -1, 0 or +1: how far
// the caller should step the underlying value this frame.
static int settingRowStepper(TRect r, const char* label, const char* value, bool focused,
                              bool tap, int tx, int ty, bool left, bool right)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);
	fontDraw(r.x + 6, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	const float btn_w = 22.0f, btn_h = r.h - 6.0f;
	const TRect minus_r = {r.x + r.w - 2 * btn_w - 6, r.y + 3, btn_w, btn_h};
	const TRect plus_r  = {r.x + r.w - btn_w - 6,      r.y + 3, btn_w, btn_h};

	spriteRect(minus_r.x, minus_r.y, minus_r.w, minus_r.h, COL_PANEL_LO);
	fontDraw(minus_r.x + (minus_r.w - FONT_ADVANCE) * 0.5f,
	         minus_r.y + (minus_r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "-");
	spriteRect(plus_r.x, plus_r.y, plus_r.w, plus_r.h, COL_PANEL_LO);
	fontDraw(plus_r.x + (plus_r.w - FONT_ADVANCE) * 0.5f,
	         plus_r.y + (plus_r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "+");

	const int vw = fontTextWidth(value, 1);
	fontDraw(minus_r.x - (float)vw - 8, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_ACCENT, value);

	int step = 0;
	if (focused && left)  step = -1;
	if (focused && right) step = 1;
	if (tap && ptIn(minus_r, tx, ty)) step = -1;
	if (tap && ptIn(plus_r,  tx, ty)) step = 1;
	return step;
}

// A settings row with an ON/OFF value. The whole row is the tap target (there is only one
// state to reach, unlike the stepper's two directions, so there is no reason to shrink it
// to a chip) — tapping anywhere on it, or A while it is focused, flips the value.
static bool settingRowToggle(TRect r, const char* label, bool value, bool focused,
                              bool tap, int tx, int ty)
{
	spriteRect(r.x, r.y, r.w, r.h, focused ? COL_PANEL_HI : COL_PANEL);
	if (focused) spriteRect(r.x, r.y, r.w, 2, COL_ACCENT);
	fontDraw(r.x + 6, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	const char* v = value ? "ON" : "OFF";
	const int vw = fontTextWidth(v, 1);
	fontDraw(r.x + r.w - (float)vw - 10, r.y + (r.h - FONT_GLYPH_H) * 0.5f, 1,
	         value ? COL_ACCENT : COL_TEXT_DIM, v);

	return tap && ptIn(r, tx, ty);
}

static const char* actionLabel(int action)
{
	switch (action) {
	case ACTION_MOVE_FORWARD: return "MOVE FORWARD";
	case ACTION_MOVE_BACK:    return "MOVE BACK";
	case ACTION_MOVE_LEFT:    return "MOVE LEFT";
	case ACTION_MOVE_RIGHT:   return "MOVE RIGHT";
	case ACTION_JUMP:         return "JUMP";
	case ACTION_BREAK:        return "BREAK";
	case ACTION_PLACE:        return "PLACE";
	default:                  return "?";
	}
}

// The reverse of options.c's own s_action_keys/OPTIONS_VALID_KEYS pairing, for display:
// a raw OPT_KEY_* bit back to the button name a player recognises. Kept here rather than
// in app/options.* because it is a UI string table, not a setting, and options.h is
// explicit that it wants no display concerns mixed into the format it has to keep
// host-testable.
static const char* keyBitLabel(uint32_t bit)
{
	switch (bit) {
	case OPT_KEY_A:      return "A";
	case OPT_KEY_X:      return "X";
	case OPT_KEY_Y:      return "Y";
	case OPT_KEY_DRIGHT: return "D-PAD RIGHT";
	case OPT_KEY_DLEFT:  return "D-PAD LEFT";
	case OPT_KEY_DUP:    return "D-PAD UP";
	case OPT_KEY_DDOWN:  return "D-PAD DOWN";
	default:             return "?";
	}
}

// ── World select ───────────────────────────────────────────────────────────────────────

// Re-scans REGION_ROOT and puts `ts` on the world-select screen. Called only when the
// player actually navigates here (from the main screen, or back out of world creation) —
// never once per frame. A directory listing is a real SD-card cost the same way opening a
// region file is (see world/region.h's own file comment on why files-per-column was
// rejected for exactly this reason); redoing it 60 times a second for a screen nothing on
// disk changes while it is open would be that mistake again, one layer up.
static void titleEnterWorldSelect(TitleState* ts)
{
	ts->world_count = worldlistScan(REGION_ROOT, ts->worlds, WORLDLIST_MAX,
	                                 &ts->world_list_truncated);
	ts->screen = TITLE_SCR_WORLD_SELECT;
	ts->cursor = 0;
	ts->world_scroll = 0;
	ts->status[0] = '\0';
	ts->status_ttl = 0;

	if (ts->world_list_truncated) {
		snprintf(ts->status, sizeof(ts->status), "showing first %d worlds", WORLDLIST_MAX);
		ts->status_ttl = 180;   // ~3 s at the project's measured 59.83 fps; see main.c
	}
}

// Every failure branch below is something the player must actually act on — retype the
// name, or notice the SD card write is failing — unlike titleEnterWorldSelect's truncation
// notice above, which is only ever informational. That one keeps the 180-frame (~3 s at the
// project's measured 59.83 fps, see main.c) budget; these get roughly double, because a
// message the player has to read *and* respond to before it clears itself needs longer than
// one they can just glance at.
#define WORLD_ERROR_STATUS_TTL 360

// The software keyboard applet, invoked directly from the "New World" button. This blocks
// until the player confirms or cancels — a modal system applet, not a per-frame thing this
// file's "never calls hidScanInput" rule was written about (see title.h's file comment):
// the frame loop is not what is reading input while swkbd owns the screen, libctru is.
static void titleCreateWorldFlow(TitleState* ts, TitleResult* r)
{
	SwkbdState kbd;
	swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, WORLDLIST_NAME_MAX - 1);
	swkbdSetHintText(&kbd, "World name");
	// Backslash is filtered by the keyboard itself; forward slash has no swkbd filter flag,
	// so it — and everything else outside worldlistNameValid's charset — is caught below
	// instead. Two layers on purpose: the filter stops the common case before it is even
	// typed, and worldlistNameValid is the one gate nothing gets past regardless.
	swkbdSetValidation(&kbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_BACKSLASH, 0);

	char name[WORLDLIST_NAME_MAX];
	name[0] = '\0';
	const SwkbdButton btn = swkbdInputText(&kbd, name, sizeof(name));

	// swkbdInit above was called with numButtons=2 and neither button was renamed via
	// swkbdSetButton, so the two buttons libctru actually shows are its defaults for that
	// case: SWKBD_BUTTON_LEFT ("usually Cancel") and SWKBD_BUTTON_RIGHT == SWKBD_BUTTON_CONFIRM
	// ("usually OK") — see the SwkbdButton comments in swkbd.h. SWKBD_BUTTON_MIDDLE cannot
	// fire here; a 2-button dialog has no middle button to press. A genuine Cancel is the one
	// outcome this screen must stay exactly as silent about as it always has; everything else
	// below means the applet did not finish the way the player asked it to.
	if (btn == SWKBD_BUTTON_LEFT) return;   // player cancelled; stay on world select, no message

	if (btn != SWKBD_BUTTON_CONFIRM) {
		// Left as the only silent case above means anything reaching here is
		// SWKBD_BUTTON_NONE in practice — swkbd.h's own doc comment on it says this is what
		// swkbdInputText returns "in special cases" instead of a button at all: out of
		// memory, invalid parameters, or a HOME/soft-reset/power-button interrupt (this
		// screen enables none of the parental-PIN or filter-callback features that add more
		// such cases). swkbdGetResult() is the only way to see which afterwards, so the
		// numeric SwkbdResult goes straight into the status line — a photo of the bottom
		// screen is then enough to look the value up in swkbd.h without reproducing whatever
		// real hardware just hit, which is exactly what this machine cannot do (this flow is
		// verified end-to-end here; the report this is responding to is not reproducible on
		// this machine).
		snprintf(ts->status, sizeof(ts->status), "keyboard error %d - retry",
		         (int)swkbdGetResult(&kbd));
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	if (!worldlistNameValid(name)) {
		// worldlistNameValid's own comment (scene/worldlist.h) is the actual rule: non-empty,
		// no leading/trailing space, under WORLDLIST_NAME_MAX bytes, only [A-Za-z0-9 _-].
		// swkbdSetValidation above already stops a fully empty or all-blank name, and
		// maxTextLength already stops an overlong one, so in practice this fires on a
		// disallowed character — most likely '/', which has no SWKBD_FILTER_* flag of its
		// own (see the comment on swkbdSetValidation above) — or on whitespace at one end of
		// an otherwise non-blank name. Naming the allowed charset here beats making the
		// player guess which of those it was.
		snprintf(ts->status, sizeof(ts->status), "bad name: use A-Z 0-9 _ - only");
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	if (!worldlistCreate(REGION_ROOT, name)) {
		// worldlistCreate's own comment (scene/worldlist.h) says false means one of two
		// things: an invalid name (already ruled out by worldlistNameValid above) or the
		// target directory still not existing once mkdir has been tried. With the name
		// already known good, that leaves exactly one real cause — the SD card write itself
		// failed (card missing, write-protected, full, or REGION_ROOT's parent not mounted)
		// — which is worth saying plainly instead of the old message that gave no hint where
		// to even start looking.
		snprintf(ts->status, sizeof(ts->status), "world create failed (sd write)");
		ts->status_ttl = WORLD_ERROR_STATUS_TTL;
		return;
	}

	// A name that already belonged to a world is not an error — see worldlistCreate's own
	// comment — so this both makes a fresh world and re-opens an existing one typed by name,
	// and either way the player just told this screen exactly what they want to do with it:
	// play it. Making them find it in the list a second time would be busywork.
	r->action = TITLE_START_WORLD;
	snprintf(r->world_name, sizeof(r->world_name), "%s", name);
}

static TitleResult drawWorldSelect(TitleState* ts, const TitleInput* in, bool tap)
{
	TitleResult r = {TITLE_STAY, {0}};
	const int new_world_idx = ts->world_count;
	const int back_idx      = ts->world_count + 1;
	const int total         = ts->world_count + 2;

	if (in->keys_down & KEY_DDOWN) ts->cursor = (ts->cursor + 1) % total;
	if (in->keys_down & KEY_DUP)   ts->cursor = (ts->cursor + total - 1) % total;
	const bool a = (in->keys_down & KEY_A) != 0;

	// Scroll the minimum needed to keep the cursor's row on screen, rather than
	// re-centring on every move — a re-centring list jumps under a thumb mid-scroll, the
	// same "rows reshuffle" problem worldlistScan's own alphabetical sort exists to avoid
	// one layer down (see worldlist.h).
	if (ts->cursor < ts->world_count) {
		if (ts->cursor < ts->world_scroll) ts->world_scroll = ts->cursor;
		if (ts->cursor >= ts->world_scroll + LIST_VISIBLE_ROWS)
			ts->world_scroll = ts->cursor - LIST_VISIBLE_ROWS + 1;
	}

	fontDraw(8, 4, 1, COL_TEXT_DIM, "SELECT WORLD");
	if (ts->status_ttl > 0) fontDraw(120, 4, 1, COL_WARN, ts->status);

	if (ts->world_count == 0)
		fontDraw(10, LIST_TOP_Y + 10, 1, COL_TEXT_DIM, "No worlds yet - tap New World");

	for (int row = 0; row < LIST_VISIBLE_ROWS; row++) {
		const int i = ts->world_scroll + row;
		if (i >= ts->world_count) break;
		const TRect rr = {10, (float)(LIST_TOP_Y + row * LIST_ROW_H), SCR_W - 20, LIST_ROW_H - 2};
		if (uiButton(rr, ts->worlds[i].name, ts->cursor == i, tap, in->touch_x, in->touch_y, a)) {
			r.action = TITLE_START_WORLD;
			snprintf(r.world_name, sizeof(r.world_name), "%s", ts->worlds[i].name);
		}
	}

	const TRect new_r  = {10, (float)LIST_BTN1_Y, SCR_W - 20, LIST_BTN_H};
	const TRect back_r = {10, (float)LIST_BTN2_Y, SCR_W - 20, LIST_BTN_H};

	if (uiButton(new_r, "NEW WORLD", ts->cursor == new_world_idx, tap, in->touch_x, in->touch_y, a))
		titleCreateWorldFlow(ts, &r);

	if (uiButton(back_r, "BACK", ts->cursor == back_idx, tap, in->touch_x, in->touch_y, a)
	    || (in->keys_down & KEY_B)) {
		ts->screen = TITLE_SCR_MAIN;
		ts->cursor = 0;
	}

	return r;
}

// ── Main screen ────────────────────────────────────────────────────────────────────────

#define MAIN_ITEM_COUNT 3   // Play, Options, Quit

static TitleResult drawMain(TitleState* ts, const TitleInput* in, bool tap)
{
	TitleResult r = {TITLE_STAY, {0}};

	if (in->keys_down & KEY_DDOWN) ts->cursor = (ts->cursor + 1) % MAIN_ITEM_COUNT;
	if (in->keys_down & KEY_DUP)   ts->cursor = (ts->cursor + MAIN_ITEM_COUNT - 1) % MAIN_ITEM_COUNT;
	const bool a = (in->keys_down & KEY_A) != 0;

	const int tw = fontTextWidth("BLOCKSMITH", 2);
	fontDraw((SCR_W - (float)tw) * 0.5f, 6, 2, COL_ACCENT, "BLOCKSMITH");

	float y = 44;
	const TRect play_r    = {10, y, SCR_W - 20, MAIN_BTN_H}; y += MAIN_BTN_H + MAIN_GAP;
	const TRect options_r = {10, y, SCR_W - 20, MAIN_BTN_H}; y += MAIN_BTN_H + MAIN_GAP;
	const TRect quit_r    = {10, y, SCR_W - 20, MAIN_BTN_H};

	if (uiButton(play_r, "PLAY", ts->cursor == 0, tap, in->touch_x, in->touch_y, a))
		titleEnterWorldSelect(ts);

	if (uiButton(options_r, "OPTIONS", ts->cursor == 1, tap, in->touch_x, in->touch_y, a)) {
		ts->screen = TITLE_SCR_OPTIONS_GENERAL;
		ts->cursor = 0;
	}

	if (uiButton(quit_r, "QUIT", ts->cursor == 2, tap, in->touch_x, in->touch_y, a))
		r.action = TITLE_QUIT;

	return r;
}

// ── Options: general settings ─────────────────────────────────────────────────────────

#define GEN_ITEM_COUNT 6   // 4 settings + "Controls >" + Back

static TRect optRowRect(int i)
{
	TRect r = {10, (float)(LIST_TOP_Y + i * LIST_ROW_H), SCR_W - 20, LIST_ROW_H - 2};
	return r;
}

static void drawOptionsGeneral(TitleState* ts, Options* opts, const TitleInput* in, bool tap)
{
	const bool a     = (in->keys_down & KEY_A) != 0;
	const bool left  = (in->keys_down & KEY_DLEFT)  != 0;
	const bool right = (in->keys_down & KEY_DRIGHT) != 0;

	if (in->keys_down & KEY_DDOWN) ts->cursor = (ts->cursor + 1) % GEN_ITEM_COUNT;
	if (in->keys_down & KEY_DUP)   ts->cursor = (ts->cursor + GEN_ITEM_COUNT - 1) % GEN_ITEM_COUNT;

	fontDraw(8, 4, 1, COL_TEXT_DIM, "OPTIONS");

	char buf[32];
	int step;

	snprintf(buf, sizeof(buf), "%d", opts->render_dist);
	step = settingRowStepper(optRowRect(0), "RENDER DIST", buf, ts->cursor == 0,
	                          tap, in->touch_x, in->touch_y, left, right);
	if (step) opts->render_dist = clampInt(opts->render_dist + step,
	                                        RENDER_DIST_MIN, RENDER_DIST_MAX);

	snprintf(buf, sizeof(buf), "%.1f", (double)opts->slider_3d);
	step = settingRowStepper(optRowRect(1), "3D DEPTH", buf, ts->cursor == 1,
	                          tap, in->touch_x, in->touch_y, left, right);
	if (step) opts->slider_3d = clampF(opts->slider_3d + (float)step * 0.1f,
	                                    OPTIONS_SLIDER_MIN, OPTIONS_SLIDER_MAX);

	const bool toggled = settingRowToggle(optRowRect(2), "INVERT LOOK", opts->invert_look,
	                                       ts->cursor == 2, tap, in->touch_x, in->touch_y);
	if (toggled || (ts->cursor == 2 && a)) opts->invert_look = !opts->invert_look;

	snprintf(buf, sizeof(buf), "%.2f", (double)opts->look_sensitivity);
	step = settingRowStepper(optRowRect(3), "LOOK SENS", buf, ts->cursor == 3,
	                          tap, in->touch_x, in->touch_y, left, right);
	if (step) opts->look_sensitivity = clampF(opts->look_sensitivity + (float)step * 0.25f,
	                                           OPTIONS_SENS_MIN, OPTIONS_SENS_MAX);

	const TRect controls_r = {10, (float)LIST_BTN1_Y, SCR_W - 20, LIST_BTN_H};
	if (uiButton(controls_r, "CONTROLS >", ts->cursor == 4, tap, in->touch_x, in->touch_y, a)) {
		ts->screen = TITLE_SCR_OPTIONS_BINDINGS;
		ts->cursor = 0;
	}

	const TRect back_r = {10, (float)LIST_BTN2_Y, SCR_W - 20, LIST_BTN_H};
	if (uiButton(back_r, "BACK", ts->cursor == 5, tap, in->touch_x, in->touch_y, a)
	    || (in->keys_down & KEY_B)) {
		// The one point either options screen ever writes to disk — see title.h's file
		// comment for why this is the whole of this file's crash exposure: at worst a
		// battery pull loses whatever was changed since the *previous* time this screen
		// was left, never a whole session's worth.
		optionsSave(opts, TITLE_OPTIONS_PATH);
		ts->screen = TITLE_SCR_MAIN;
		ts->cursor = 1;   // land back on the OPTIONS button, not PLAY
	}
}

// ── Options: key bindings ─────────────────────────────────────────────────────────────

static void drawOptionsBindings(TitleState* ts, Options* opts, const TitleInput* in, bool tap)
{
	const bool a = (in->keys_down & KEY_A) != 0;

	if (ts->rebind_action >= 0) {
		// Modal capture: the next key that is one of the seven valid binding bits wins;
		// anything else this frame (nothing yet, or a key this build never binds) is
		// ignored, so brushing the D-pad while aiming the stylus at a row cannot finish the
		// capture by accident. B cancels without changing the binding.
		fontDraw(20, 90, 1, COL_TEXT, "Press a button for:");
		fontDraw(20, 102, 2, COL_ACCENT, actionLabel(ts->rebind_action));
		fontDraw(20, 130, 1, COL_TEXT_DIM, "(B cancels)");

		if (in->keys_down & KEY_B) {
			ts->rebind_action = -1;
		} else if (in->keys_down) {
			for (int k = 0; k < OPTIONS_VALID_KEY_COUNT; k++) {
				if (in->keys_down & OPTIONS_VALID_KEYS[k]) {
					opts->bindings[ts->rebind_action] = OPTIONS_VALID_KEYS[k];
					ts->rebind_action = -1;
					break;
				}
			}
		}
		return;   // nothing else on this screen is live while a capture is in progress
	}

	if (in->keys_down & KEY_DDOWN) ts->cursor = (ts->cursor + 1) % (ACTION_COUNT + 1);
	if (in->keys_down & KEY_DUP)   ts->cursor = (ts->cursor + ACTION_COUNT) % (ACTION_COUNT + 1);

	fontDraw(8, 4, 1, COL_TEXT_DIM, "CONTROLS");

	for (int i = 0; i < ACTION_COUNT; i++) {
		const TRect rr = {10, (float)(BIND_ROW_Y + i * BIND_ROW_H), SCR_W - 20, BIND_ROW_H - 1};
		const bool focused = ts->cursor == i;

		spriteRect(rr.x, rr.y, rr.w, rr.h, focused ? COL_PANEL_HI : COL_PANEL);
		if (focused) spriteRect(rr.x, rr.y, rr.w, 2, COL_ACCENT);
		fontDraw(rr.x + 4, rr.y + (rr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, actionLabel(i));

		const char* keyname = keyBitLabel(opts->bindings[i]);
		const int kw = fontTextWidth(keyname, 1);
		fontDraw(rr.x + rr.w - (float)kw - 6, rr.y + (rr.h - FONT_GLYPH_H) * 0.5f, 1,
		         COL_ACCENT, keyname);

		const bool tapped_here = tap && ptIn(rr, in->touch_x, in->touch_y);
		if (tapped_here || (focused && a)) ts->rebind_action = i;
	}

	const TRect back_r = {10, (float)BIND_BACK_Y, SCR_W - 20, LIST_BTN_H};
	if (uiButton(back_r, "BACK", ts->cursor == ACTION_COUNT, tap, in->touch_x, in->touch_y, a)
	    || (in->keys_down & KEY_B)) {
		// Back to options-general, not to the title screen — that screen's own Back button
		// is the one save point (see drawOptionsGeneral), so a binding change survives a
		// battery pull exactly as soon as a general-settings change does, no sooner.
		ts->screen = TITLE_SCR_OPTIONS_GENERAL;
		ts->cursor = 4;   // the "CONTROLS >" button, where this screen was entered from
	}
}

// ── Entry points ───────────────────────────────────────────────────────────────────────

void titleInit(TitleState* ts)
{
	memset(ts, 0, sizeof(*ts));
	ts->screen = TITLE_SCR_MAIN;
	ts->rebind_action = -1;
}

TitleResult titleUpdateDraw(TitleState* ts, Options* opts, const TitleInput* in)
{
	TitleResult r = {TITLE_STAY, {0}};

	// Rising edge only: a tap fires once, not every frame the finger stays down, matching
	// how KEY_A works via hidKeysDown() rather than hidKeysHeld(). Computed once here so
	// every screen below reads the same `tap` rather than re-deriving it.
	const bool tap = in->touch_down && !ts->touch_prev;
	ts->touch_prev = in->touch_down;

	if (ts->status_ttl > 0) ts->status_ttl--;

	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	switch (ts->screen) {
	case TITLE_SCR_MAIN:             r = drawMain(ts, in, tap);              break;
	case TITLE_SCR_WORLD_SELECT:     r = drawWorldSelect(ts, in, tap);       break;
	case TITLE_SCR_OPTIONS_GENERAL:  drawOptionsGeneral(ts, opts, in, tap);  break;
	case TITLE_SCR_OPTIONS_BINDINGS: drawOptionsBindings(ts, opts, in, tap); break;
	}

	spriteEnd();
	return r;
}
