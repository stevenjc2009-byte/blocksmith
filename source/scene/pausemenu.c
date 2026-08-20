#include "scene/pausemenu.h"

#include <3ds.h>

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

// Options-page rows. These two are interactive; the memory figures below them are a
// readout, so the cursor stops at OPT_ROW_COUNT rather than at the end of the list.
enum { OPT_ROW_DIST = 0, OPT_ROW_3D, OPT_ROW_COUNT };

static bool s_open;
static Page s_page;
static int  s_cursor;

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
#define ROW_X     (PANEL_X + 12)
#define ROW_W     (PANEL_W - 24)
#define TEXT_PAD  6

bool pauseMenuOpen(void) { return s_open; }

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
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "Render dist");
	fontDrawf((float)(ROW_X + ROW_W - 58), y, 1,
	          st->render_dist > st->dist_min ? COL_TEXT : COL_DIM, "<");
	fontDrawf((float)(ROW_X + ROW_W - 44), y, 1, COL_TEXT, "%d", st->render_dist);
	fontDrawf((float)(ROW_X + ROW_W - 26), y, 1,
	          st->render_dist < st->dist_max ? COL_TEXT : COL_DIM, ">");

	// 3D. This lived on SELECT until SELECT became the button that opens this menu; it is a
	// setting, so a settings page is where it belongs, but it is here mainly so the feature
	// did not simply vanish when its button was reused.
	y += (float)ROW_H;
	if (s_cursor == OPT_ROW_3D)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, COL_TEXT, "3D");
	fontDraw((float)(ROW_X + ROW_W - 44), y, 1,
	         st->stereo ? COL_HEAD : COL_DIM, st->stereo ? "On" : "Off");

	// Memory. Three numbers because they answer three different questions and are drawn
	// from three different pools — a single "free RAM" figure would be a fiction on this
	// console. World is the one the player's own building moves; linear is what the mesh
	// pool came out of and therefore what render distance actually spends; VRAM holds the
	// atlas and the framebuffers and barely moves at all.
	float ly = y + (float)ROW_H + 6.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_HEAD, "MEMORY");
	ly += 14.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "World  %lu / %lu KB",
	          (unsigned long)kb(st->world_used), (unsigned long)kb(st->world_budget));
	ly += 12.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "Linear %lu KB free",
	          (unsigned long)kb(st->linear_free));
	ly += 12.0f;
	fontDrawf((float)(ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "VRAM   %lu KB free",
	          (unsigned long)kb(st->vram_free));

	fontDraw((float)ROW_X, (float)(PANEL_Y + PANEL_H - 16), 1, COL_DIM,
	         "left/right change   B back");
	spriteEnd();
}
