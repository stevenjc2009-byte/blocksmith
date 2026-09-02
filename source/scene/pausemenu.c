#include "scene/pausemenu.h"

#include <3ds.h>

#include "app/options.h"      // OPTIONS_AUDIO_VOL_* — one definition of the range
#include "audio/audio.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "scene/ui_layout.h"

// Two pages, not a tree. RESUME/OPTIONS/QUIT on the first, the settings on the second, and
// B walks back up. Anything deeper would need a navigation stack for a menu with six rows
// in it.
typedef enum { PAGE_MAIN = 0, PAGE_OPTIONS } Page;

// Main-page rows, in draw order. The enum is the cursor's value, so the order here is the
// order on screen and MAIN_ROW_COUNT is what the cursor wraps against.
enum { ROW_RESUME = 0, ROW_OPTIONS, ROW_QUIT, MAIN_ROW_COUNT };

// Options-page rows. All five are interactive; the memory figures below them are a
// readout, so the cursor stops at OPT_ROW_COUNT rather than at the end of the list.
//
// Sound sits next to 3D because they are the two output settings, with the two rows that
// open another screen kept together at the bottom.
enum { OPT_ROW_DIST = 0, OPT_ROW_3D, OPT_ROW_VOLUME, OPT_ROW_REMAP, OPT_ROW_DEBUG,
       OPT_ROW_COUNT };

static bool s_open;
static Page s_page;
static int  s_cursor;

// Set when the volume row moves, cleared by pauseMenuTakeVolumeChanged. A flag rather
// than an immediate save because a held left/right would otherwise rewrite options.ini
// once per frame, and every one of those writes is a remove()+rename() on the SD card.
static bool s_volume_changed;

// Colours are 0xAABBGGRR, matching gfx/sprite.h's spriteRect and gfx/font.h's fontDraw.
#define COL_SCRIM   0xC0100C08u   // darkens the world UI behind the panel without hiding it
#define COL_PANEL   0xF0201A14u
#define COL_EDGE    0xFF6B5137u
#define COL_TEXT    0xFFE8DCC8u
#define COL_DIM     0xFF9A8B76u
#define COL_SEL     0xFF4E86C8u
#define COL_HEAD    0xFFB4E0A0u

#define PANEL_X 26
#define PANEL_Y 24
#define PANEL_W (SCR_W - PANEL_X * 2)
#define PANEL_H (SCR_H - PANEL_Y * 2)

#define ROW_H     22

// The options page uses a tighter pitch than the main page's ROW_H. It has to: five rows
// at 22 px push the memory readout's last line to y=212, past the footer at y=200 and out
// through the bottom of the panel at y=216. At 20 the last readout line lands at 190,
// exactly where it sits today with four rows. Measured against PANEL_Y 24 + PANEL_H 192
// and gfx/font.h's FONT_GLYPH_H of 7, not eyeballed — but nobody has LOOKED at it on a
// screen, and this project has no way to.
#define OPT_ROW_H 20
#define ROW_X     (PANEL_X + 12)
#define ROW_W     (PANEL_W - 24)
#define TEXT_PAD  6

bool pauseMenuOpen(void) { return s_open; }

bool pauseMenuTakeVolumeChanged(void)
{
	const bool changed = s_volume_changed;
	s_volume_changed = false;
	return changed;
}

void pauseMenuClose(void)
{
	s_open   = false;
	s_page   = PAGE_MAIN;
	s_cursor = ROW_RESUME;
}

void pauseMenuToggle(void)
{
	if (s_open) {
		pauseMenuClose();
		return;
	}
	s_open   = true;
	s_page   = PAGE_MAIN;
	s_cursor = ROW_RESUME;
}

PauseAction pauseMenuInput(uint32_t down, int* out_dist_step, bool* out_stereo_toggle)
{
	if (out_dist_step)     *out_dist_step = 0;
	if (out_stereo_toggle) *out_stereo_toggle = false;
	if (!s_open) return PAUSE_ACTION_NONE;

	// Wrapping rather than clamping at the ends: three rows is short enough that running
	// off the bottom to reach the top is a shortcut, not a surprise.
	const int rows = (s_page == PAGE_MAIN) ? MAIN_ROW_COUNT : OPT_ROW_COUNT;
	if (down & KEY_DUP)   s_cursor = (s_cursor + rows - 1) % rows;
	if (down & KEY_DDOWN) s_cursor = (s_cursor + 1) % rows;

	if (s_page == PAGE_OPTIONS) {
		// Left/right step the setting under the cursor. Reported, not applied — see the
		// note on this function in pausemenu.h.
		if (s_cursor == OPT_ROW_DIST && out_dist_step) {
			if (down & KEY_DLEFT)  *out_dist_step = -1;
			if (down & KEY_DRIGHT) *out_dist_step = +1;
		}
		// 3D is a two-state setting, so left, right and A all mean the same thing: flip it.
		// Accepting A here as well as the arrows matters because this row used to be the
		// SELECT button — a player reaching for the old behaviour finds it on the first key
		// they try rather than having to learn that this one row is arrow-only.
		if (s_cursor == OPT_ROW_3D && out_stereo_toggle &&
		    (down & (KEY_DLEFT | KEY_DRIGHT | KEY_A)))
			*out_stereo_toggle = true;
		// Sound volume is APPLIED here rather than reported like the two rows above it,
		// because there is nothing for the caller to do with it: audioSetMasterVolume() is
		// the entire action, main.c holds no audio state that could fall out of step, and
		// the call is a no-op on a console with no DSP firmware. Only persistence is still
		// the caller's, and that is what s_volume_changed is for.
		if (s_cursor == OPT_ROW_VOLUME && (down & (KEY_DLEFT | KEY_DRIGHT))) {
			float v = audioGetMasterVolume();
			v += (down & KEY_DRIGHT) ? OPTIONS_AUDIO_VOL_STEP : -OPTIONS_AUDIO_VOL_STEP;
			// Snapped back onto the step grid every time. Without this, accumulating 0.1f
			// ten times lands on 0.99999994 rather than 1.0 and the row reads 100% while the
			// maximum is never actually reached — a bug that is invisible on screen.
			v = (float)((int)(v / OPTIONS_AUDIO_VOL_STEP + (v < 0.0f ? -0.5f : 0.5f)))
			    * OPTIONS_AUDIO_VOL_STEP;
			if (v < OPTIONS_AUDIO_VOL_MIN) v = OPTIONS_AUDIO_VOL_MIN;
			if (v > OPTIONS_AUDIO_VOL_MAX) v = OPTIONS_AUDIO_VOL_MAX;
			audioSetMasterVolume(v);
			s_volume_changed = true;
		}
		// The remap and debug rows are plain A-activations, reported to the caller the same
		// way RESUME and QUIT are: this module cannot open those screens itself without
		// owning their state, which is main.c's job.
		if ((down & KEY_A) && s_cursor == OPT_ROW_REMAP) return PAUSE_ACTION_REMAP;
		if ((down & KEY_A) && s_cursor == OPT_ROW_DEBUG) return PAUSE_ACTION_DEBUG;
		// B backs out to the main page. It does NOT close the menu: a player who opened
		// options to try a render distance is mid-comparison, and dumping them back into
		// the world would undo the reason they are here.
		if (down & KEY_B) {
			s_page   = PAGE_MAIN;
			s_cursor = ROW_OPTIONS;
		}
		return PAUSE_ACTION_NONE;
	}

	// B on the main page is the same as choosing RESUME — the standard "back closes it".
	if (down & KEY_B) {
		pauseMenuClose();
		return PAUSE_ACTION_RESUME;
	}

	if (down & KEY_A) {
		switch (s_cursor) {
		case ROW_RESUME:
			pauseMenuClose();
			return PAUSE_ACTION_RESUME;
		case ROW_OPTIONS:
			s_page   = PAGE_OPTIONS;
			s_cursor = OPT_ROW_DIST;
			return PAUSE_ACTION_NONE;
		case ROW_QUIT:
			pauseMenuClose();
			return PAUSE_ACTION_QUIT;
		default:
			break;
		}
	}
	return PAUSE_ACTION_NONE;
}

// Rounds bytes to whole KB for display. Truncating rather than rounding to nearest, so a
// figure shown as "16 KB free" is never one the allocator would refuse 16 KB out of.
static uint32_t kb(uint32_t bytes) { return bytes / 1024u; }

static void drawRow(int index, const char* label, bool selected)
{
	const float y = (float)(PANEL_Y + 34 + index * ROW_H);
	if (selected)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, selected ? COL_TEXT : COL_DIM, label);
}

void pauseMenuDraw(const PauseStats* st)
{
	if (!s_open || !st) return;

	// Opens its own sprite pass rather than joining the one scene/ui.c uses, because
	// uiUpdateDraw closes its pass before returning — this draws after it, on top of the
	// UI it just finished. spriteTexture is the font's sheet for the same reason ui.c uses
	// it: spriteRect draws from the font texture's white texel, so a panel and the text on
	// it come from one binding and cost no flush between them.
	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());

	// Scrim over the whole bottom screen first, then the panel on top of it. The scrim is
	// what makes the panel read as *over* the game rather than as another game screen.
	spriteRect(0.0f, 0.0f, (float)SCR_W, (float)SCR_H, COL_SCRIM);
	spriteRect((float)(PANEL_X - 1), (float)(PANEL_Y - 1),
	           (float)(PANEL_W + 2), (float)(PANEL_H + 2), COL_EDGE);
	spriteRect((float)PANEL_X, (float)PANEL_Y, (float)PANEL_W, (float)PANEL_H, COL_PANEL);

	if (s_page == PAGE_MAIN) {
		fontDraw((float)ROW_X, (float)(PANEL_Y + 10), 1, COL_HEAD, "PAUSED");
		drawRow(ROW_RESUME,  "Resume",  s_cursor == ROW_RESUME);
		drawRow(ROW_OPTIONS, "Options", s_cursor == ROW_OPTIONS);
		// "Quit to title", not "Quit": the row leaves the world and lands on the title
		// screen, which is where the app's own quit lives. Saying just "Quit" next to a
		// Resume would read as quitting the game outright, and a player who wanted another
		// world would never press it.
		drawRow(ROW_QUIT,    "Quit to title", s_cursor == ROW_QUIT);
		fontDraw((float)ROW_X, (float)(PANEL_Y + PANEL_H - 16), 1, COL_DIM,
		         "A choose   B/SELECT resume");
		spriteEnd();
		return;
	}

	fontDraw((float)ROW_X, (float)(PANEL_Y + 10), 1, COL_HEAD, "OPTIONS");

	// Render distance. Arrows are drawn greyed at the ends rather than hidden, so the row
	// does not change width when it hits a limit — a control that moves as you use it reads
	// as a glitch.
	float y = (float)(PANEL_Y + 34);
	if (s_cursor == OPT_ROW_DIST)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(OPT_ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "Render dist");
	fontDrawf((float)(ROW_X + ROW_W - 58), y, 1,
	          st->render_dist > st->dist_min ? COL_TEXT : COL_DIM, "<");
	fontDrawf((float)(ROW_X + ROW_W - 44), y, 1, COL_TEXT, "%d", st->render_dist);
	fontDrawf((float)(ROW_X + ROW_W - 26), y, 1,
	          st->render_dist < st->dist_max ? COL_TEXT : COL_DIM, ">");

	// 3D. This lived on SELECT until SELECT became the button that opens this menu; it is a
	// setting, so a settings page is where it belongs, but it is here mainly so the feature
	// did not simply vanish when its button was reused.
	y += (float)OPT_ROW_H;
	if (s_cursor == OPT_ROW_3D)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(OPT_ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "3D");
	fontDraw((float)(ROW_X + ROW_W - 44), y, 1,
	         st->stereo ? COL_HEAD : COL_DIM, st->stereo ? "On" : "Off");

	// Sound. The value is read live out of the audio system rather than passed in through
	// PauseStats, so there is exactly one copy of the current volume in the process and no
	// way for the row to disagree with what the DSP was told.
	//
	// The label greys out when there is no audio at all — a console whose DSP firmware was
	// never dumped. The row still works and still saves, because the setting is a
	// preference about the game and not a property of this console; greying it is the
	// only honest signal that moving it will not change anything the player can hear.
	//
	// %3d%% rather than %d%%: a fixed four-character field means the number does not shift
	// left and right under the cursor as it crosses 9% and 100%.
	y += (float)OPT_ROW_H;
	if (s_cursor == OPT_ROW_VOLUME)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(OPT_ROW_H - 2), COL_SEL);
	{
		const float vol = audioGetMasterVolume();
		const int   pct = (int)(vol * 100.0f + 0.5f);
		fontDraw((float)(ROW_X + TEXT_PAD), y, 1,
		         audioAvailable() ? COL_TEXT : COL_DIM, "Sound");
		fontDraw((float)(ROW_X + ROW_W - 66), y, 1,
		         pct > 0 ? COL_TEXT : COL_DIM, "<");
		fontDrawf((float)(ROW_X + ROW_W - 52), y, 1, COL_TEXT, "%3d%%", pct);
		fontDraw((float)(ROW_X + ROW_W - 24), y, 1,
		         pct < 100 ? COL_TEXT : COL_DIM, ">");
	}

	y += (float)OPT_ROW_H;
	if (s_cursor == OPT_ROW_REMAP)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(OPT_ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "Controls");
	fontDraw((float)(ROW_X + ROW_W - 44), y, 1, COL_DIM, ">");

	y += (float)OPT_ROW_H;
	if (s_cursor == OPT_ROW_DEBUG)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(OPT_ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "Debug");
	fontDraw((float)(ROW_X + ROW_W - 44), y, 1, COL_DIM, ">");

	// Memory. Three numbers because they answer three different questions and are drawn
	// from three different pools — a single "free RAM" figure would be a fiction on this
	// console. World is the one the player's own building moves; linear is what the mesh
	// pool came out of and therefore what render distance actually spends; VRAM holds the
	// atlas and the framebuffers and barely moves at all.
	float ly = y + (float)OPT_ROW_H - 2.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_HEAD, "MEMORY");
	ly += 12.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "World  %lu / %lu KB",
	          (unsigned long)kb(st->world_used), (unsigned long)kb(st->world_budget));
	ly += 11.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "Linear %lu KB free",
	          (unsigned long)kb(st->linear_free));
	ly += 11.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "VRAM   %lu KB free",
	          (unsigned long)kb(st->vram_free));

	fontDraw((float)ROW_X, (float)(PANEL_Y + PANEL_H - 16), 1, COL_DIM,
	         "left/right change   B back");
	spriteEnd();
}
