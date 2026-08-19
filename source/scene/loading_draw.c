#include "scene/loading_draw.h"

#include <string.h>

#include "gfx/font.h"
#include "gfx/sprite.h"

#define SCR_W 320
#define SCR_H 240

// The same palette scene/title.c uses, duplicated rather than exported from it: these two
// screens are back to back — the menu hands straight over to this one — and a player should
// not be able to tell they are different files. Kept as literals in both places because
// title.c's set is private to it and lifting them into a shared header would make a palette
// change a two-file edit for no gain.
#define COL_BG        SPRITE_RGBA(26, 20, 36, 255)
#define COL_PANEL     SPRITE_RGBA(52, 40, 76, 255)
#define COL_PANEL_HI  SPRITE_RGBA(90, 74, 130, 255)
#define COL_TEXT      SPRITE_WHITE
#define COL_TEXT_DIM  SPRITE_RGBA(180, 170, 200, 255)
#define COL_ACCENT    SPRITE_RGBA(255, 226, 150, 255)
#define COL_WARN      SPRITE_RGBA(255, 140, 140, 255)

// The bar. 260 px wide leaves a 30 px margin either side — wider than the 10 px the menu's
// buttons use, because this is the one element on the screen the player is going to stare at
// and centring it reads better than filling the width. 18 px tall is two and a half times the
// 7 px glyph height, which is what makes the percentage legible sitting above it.
#define BAR_X  30
#define BAR_Y  108
#define BAR_W  260
#define BAR_H  18

// The two stall-panel buttons, side by side on one row rather than stacked: there are only
// ever two, and a 150x36 button is 24.8 x 6.0 mm — comfortably inside the 7-10 mm fingertip
// range on its short axis, which is the axis a stacked pair would have had to shrink.
#define ACT_Y   190
#define ACT_H   36
#define ACT_W   150
#define ACT_GAP 10
#define ACT1_X  ((SCR_W - 2 * ACT_W - ACT_GAP) / 2)
#define ACT2_X  (ACT1_X + ACT_W + ACT_GAP)

static void drawCentred(float y, int scale, uint32_t colour, const char* text)
{
	const int w = fontTextWidth(text, scale);
	fontDraw((SCR_W - (float)w) * 0.5f, y, scale, colour, text);
}

// Splits at the last space that fits rather than mid-word, and draws at most two lines. The
// strings that come through here are loading.c's stall reasons, which are a phrase followed by
// a bracketed dump of the counters — at 6 px per glyph a 320 px screen holds 51 characters, and
// the longest reason this can produce is over that.
#define WRAP_COLS 50

static void drawWrapped(float y, uint32_t colour, const char* text)
{
	const size_t len = strlen(text);
	if (len <= WRAP_COLS) {
		drawCentred(y, 1, colour, text);
		return;
	}

	size_t split = WRAP_COLS;
	while (split > 0 && text[split] != ' ') split--;
	if (split == 0) split = WRAP_COLS;   // one unbroken word: cut it rather than lose the tail

	char line[WRAP_COLS + 1];
	memcpy(line, text, split);
	line[split] = '\0';
	drawCentred(y, 1, colour, line);

	const char* rest = text + split;
	while (*rest == ' ') rest++;
	drawCentred(y + FONT_GLYPH_H + 3, 1, colour, rest);
}

static void drawButton(float x, float y, float w, float h, const char* label)
{
	spriteRect(x, y, w, h, COL_PANEL_HI);
	spriteRect(x, y, w, 2, COL_ACCENT);
	const int tw = fontTextWidth(label, 1);
	fontDraw(x + (w - (float)tw) * 0.5f, y + (h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);
}

void loadingDraw(const LoadingState* st, const char* heading, const char* world_name)
{
	const LoadingSample* s = &st->last;
	const bool stalled = st->phase == LOADING_STALLED;

	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	drawCentred(28, 2, stalled ? COL_WARN : COL_ACCENT, heading);
	if (world_name && world_name[0]) drawCentred(56, 1, COL_TEXT, world_name);

	const int pct = loadingPercent(st);
	fontDrawf(BAR_X, BAR_Y - FONT_GLYPH_H - 6, 1, COL_TEXT_DIM, "%d%%", pct);
	drawCentred(BAR_Y - FONT_GLYPH_H - 6, 1, COL_TEXT_DIM, loadingStageLabel(st));

	// Track first, then the fill inside a 2 px inset, so a 0% bar still shows where the bar
	// is rather than nothing at all — "no bar yet" and "the screen is broken" look identical
	// otherwise, which is the whole failure mode this screen exists to end.
	spriteRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_PANEL);
	const float fill = (float)(BAR_W - 4) * (float)pct / 100.0f;
	if (fill > 0.0f) spriteRect(BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4, stalled ? COL_WARN : COL_ACCENT);

	// The counters, always, not only when something has gone wrong. This is the line that
	// makes a photo of the bottom screen worth anything: nobody here has the hardware the
	// boot actually runs on, so the numbers have to be on screen while it is still going
	// rather than in a log nobody can reach.
	fontDrawf(8, 160, 1, COL_TEXT_DIM, "columns %d/%d   meshes %d   queued %d",
	          s->columns_in, s->columns_total, s->meshes, s->mesh_queued);
	fontDrawf(8, 172, 1, COL_TEXT_DIM, "holes %d   refused %d   worker %s   %ds",
	          s->columns_failed, s->submit_failed, s->worker_busy ? "BUSY" : "IDLE",
	          st->frames / 60);

	if (stalled) {
		// Between the bar (ends at 126) and the counters (start at 160), with room for the
		// second wrapped line at 146.
		drawWrapped(136, COL_WARN, st->reason);
		drawButton(ACT1_X, ACT_Y, ACT_W, ACT_H, "A  PLAY ANYWAY");
		drawButton(ACT2_X, ACT_Y, ACT_W, ACT_H, "START  QUIT");
	}

	// Everything above this line is still sitting in gfx/sprite.c's batch; spriteEnd is what
	// actually submits it. Leaving it out is invisible in the source and total on screen, and
	// this file shipped without it: the first emulator run of this screen drew the two clear
	// colours and not one glyph, bar or button — a blank purple panel that looked exactly like
	// the frozen boot this whole screen was written to replace. The early return that used to
	// live here is now a branch for the same reason: one exit, one flush.
	spriteEnd();
}

LoadingAction loadingHitAction(const LoadingState* st, int tx, int ty)
{
	if (st->phase != LOADING_STALLED) return LOADING_ACTION_NONE;
	if (ty < ACT_Y || ty >= ACT_Y + ACT_H) return LOADING_ACTION_NONE;

	if (tx >= ACT1_X && tx < ACT1_X + ACT_W) return LOADING_ACTION_PLAY;
	if (tx >= ACT2_X && tx < ACT2_X + ACT_W) return LOADING_ACTION_QUIT;
	return LOADING_ACTION_NONE;
}
