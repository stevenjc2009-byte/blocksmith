#include "scene/pausemenu.h"

#include <3ds.h>

#include "app/options.h"      // OPTIONS_AUDIO_VOL_* — one definition of the range
#include "audio/audio.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "scene/pausebar.h"

// v1.9.1 INTERFACE (docs/blueprint-1.9.1-interface.md D9). The two stacked pages are gone;
// three tabs replaced them. Everything this file used to decide for itself — which page,
// which row, what a key or a tap means, and where every rect is — now lives in
// scene/pausebar.c, which has no <3ds.h> in it and is gated on the host by
// tests/pausebar_test.c. What is left here is the drawing, the one audio call the volume row
// makes, and the memory figures: the parts no host test can reach, and the parts that were
// never the ones that could be subtly wrong.
//
// scene/ui_layout.h is no longer included. It was here only for SCR_W/SCR_H, and
// scene/pausebar.h carries PB_SCR_W/PB_SCR_H, which pausebar.c static-asserts against
// ui_layout.h's own pair on the console build — so the numbers are still proven against one
// source while this file stops depending on a header another lane is editing.
static PauseBarState s_bar;

// At most one action recorded by pauseMenuTouch and consumed by pauseMenuInput on the same
// frame. A tap and a button press on one frame therefore produce ONE action, the tap's — the
// same one-event-a-frame rule scene/ui_gesture.h states, and for the same reason.
//
// pauseMenuTouch OVERWRITES this rather than only filling it when it has something, so a
// frame with no tap clears the slot. That is what makes a tap on a frame whose
// pauseMenuInput is skipped — main.c gates that call behind !s_remap_open && !s_debug_open —
// get dropped on the next frame instead of firing late against a row the player has since
// moved off.
static PauseBarAction s_pending;

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

// The tab strip. Both are the panel's existing palette re-used rather than new colours —
// blueprint D4: "Colours are ui.c's existing palette; the pause/title keep theirs."
#define COL_TAB_ON  COL_SEL     // the focused tab's fill
#define COL_TAB_OFF COL_PANEL   // the other two

// The one piece of geometry that is still this file's: how far a label is inset from the
// left edge of the box it sits in. Every other number — the panel, the strip, the rows, the
// stepper boxes, the memory block and the footer — comes from scene/pausebar.h, so the rect
// the hit test owns and the rect the draw paints can never drift apart.
#define TEXT_PAD  6

bool pauseMenuOpen(void) { return pauseBarOpen(&s_bar); }

bool pauseMenuTakeVolumeChanged(void)
{
	const bool changed = s_volume_changed;
	s_volume_changed = false;
	return changed;
}

static void clearPending(void)
{
	s_pending.kind = PAUSEBAR_ACT_NONE;
	s_pending.arg  = 0;
}

void pauseMenuClose(void)
{
	pauseBarClose(&s_bar);
	clearPending();
}

void pauseMenuToggle(void)
{
	pauseBarToggle(&s_bar);
	clearPending();
}

// The volume row is APPLIED here rather than reported like the two rows above it, because
// there is nothing for the caller to do with it: audioSetMasterVolume() is the entire action,
// main.c holds no audio state that could fall out of step, and the call is a no-op on a
// console with no DSP firmware. Only persistence is still the caller's, and that is what
// s_volume_changed is for.
//
// Byte-identical arithmetic to what shipped, including the snap back onto the step grid:
// without it, accumulating 0.1f ten times lands on 0.99999994 rather than 1.0 and the row
// reads 100% while the maximum is never actually reached — a bug that is invisible on screen.
static void stepVolume(int dir)
{
	float v = audioGetMasterVolume();
	v += (dir > 0) ? OPTIONS_AUDIO_VOL_STEP : -OPTIONS_AUDIO_VOL_STEP;
	v = (float)((int)(v / OPTIONS_AUDIO_VOL_STEP + (v < 0.0f ? -0.5f : 0.5f)))
	    * OPTIONS_AUDIO_VOL_STEP;
	if (v < OPTIONS_AUDIO_VOL_MIN) v = OPTIONS_AUDIO_VOL_MIN;
	if (v > OPTIONS_AUDIO_VOL_MAX) v = OPTIONS_AUDIO_VOL_MAX;
	audioSetMasterVolume(v);
	s_volume_changed = true;
}

// One pausebar action, translated into this module's shipped vocabulary. The PAUSE_ACTION_*
// values are NOT renumbered — main.c switches on PAUSE_ACTION_REMAP (3) and
// PAUSE_ACTION_DEBUG (4), so a renumbering here would silently open Debug from the Controls
// row. That is the blueprint's own lane-D red arm 4.
static PauseAction applyBarAction(PauseBarAction act, int* out_dist_step,
                                  bool* out_stereo_toggle)
{
	switch (act.kind) {
	case PAUSEBAR_ACT_RESUME: return PAUSE_ACTION_RESUME;
	case PAUSEBAR_ACT_QUIT:   return PAUSE_ACTION_QUIT;
	case PAUSEBAR_ACT_REMAP:  return PAUSE_ACTION_REMAP;
	case PAUSEBAR_ACT_DEBUG:  return PAUSE_ACTION_DEBUG;
	case PAUSEBAR_ACT_DIST_STEP:
		if (out_dist_step) *out_dist_step = act.arg;
		return PAUSE_ACTION_NONE;
	case PAUSEBAR_ACT_STEREO_TOGGLE:
		if (out_stereo_toggle) *out_stereo_toggle = true;
		return PAUSE_ACTION_NONE;
	case PAUSEBAR_ACT_VOLUME_STEP:
		stepVolume(act.arg);
		return PAUSE_ACTION_NONE;
	default:
		// NONE, TAB and MOVE. TAB and MOVE are reported by pausebar so a caller can play
		// SFX_UI_TAP on them (blueprint D13); this module does not own that cue, so they
		// land here and change nothing the caller can see.
		return PAUSE_ACTION_NONE;
	}
}

void pauseMenuTouch(bool press, int x, int y)
{
	// Unconditional: the answer for a frame with no tap is NONE, and recording that NONE is
	// what clears a stale action. pauseBarTouch itself refuses a closed panel, a false press
	// and a point outside the panel, so this needs no guard of its own.
	s_pending = pauseBarTouch(&s_bar, press, x, y);
}

PauseAction pauseMenuInput(uint32_t down, int* out_dist_step, bool* out_stereo_toggle)
{
	if (out_dist_step)     *out_dist_step = 0;
	if (out_stereo_toggle) *out_stereo_toggle = false;

	// The tap wins and the key is DROPPED, not queued, so it cannot fire on a later frame
	// against a row the player has since moved off.
	PauseBarAction act = s_pending;
	clearPending();

	if (act.kind == PAUSEBAR_ACT_NONE) act = pauseBarKey(&s_bar, down);

	// pauseBarKey and pauseBarTouch both refuse a closed panel, so the old
	// `if (!s_open) return PAUSE_ACTION_NONE;` guard is inherited rather than deleted.
	return applyBarAction(act, out_dist_step, out_stereo_toggle);
}

// Rounds bytes to whole KB for display. Truncating rather than rounding to nearest, so a
// figure shown as "16 KB free" is never one the allocator would refuse 16 KB out of.
static uint32_t kb(uint32_t bytes) { return bytes / 1024u; }

static void drawRect(PbRect r, uint32_t colour)
{
	spriteRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, colour);
}

// The strip: one background, three tab fills, two 1-px dividers, and a 2-px accent under the
// focused tab. Labels are centred in their own tab, so the widest ("OPTIONS", 42 px at scale
// 1) sits inside an 89-px tab with 23 px either side. Nothing grows on focus: a tab that
// changes width moves its neighbours' touch targets, which is the reason blueprint D4 rules
// scale-2 out for a focused tab.
static void drawStrip(int focused)
{
	drawRect(pauseBarStripRect(), COL_PANEL);
	for (int i = 0; i < PAUSEBAR_TAB_COUNT; i++) {
		const PbRect t  = pauseBarTabRect(i);
		const bool   on = (i == focused);
		drawRect(t, on ? COL_TAB_ON : COL_TAB_OFF);
		if (i > 0)
			spriteRect((float)t.x, (float)t.y, 1.0f, (float)t.h, COL_EDGE);
		const char* label = pauseBarTabLabel(i);
		const int   w     = fontTextWidth(label, 1);
		fontDraw((float)(t.x + (t.w - w) / 2), (float)(t.y + (t.h - FONT_GLYPH_H) / 2),
		         1, on ? COL_TEXT : COL_DIM, label);
		if (on)
			spriteRect((float)t.x, (float)(t.y + t.h - 2), (float)t.w, 2.0f, COL_HEAD);
	}
}

// A row's highlight. Returns the y the row's text sits at, vertically centred in the band,
// so no caller computes that offset twice.
static float drawRowFrame(int row, bool selected)
{
	const PbRect r = pauseBarRowRect(row);
	if (selected) drawRect(r, COL_SEL);
	return (float)(r.y + (r.h - FONT_GLYPH_H) / 2);
}

// The highlight plus the row's label, straight out of pauseBarRowLabel so the string the
// hit test is documented against is the string on screen. The value half is each caller's,
// because only they know what the value is.
static float drawRowLabel(int tab, int row, bool selected, uint32_t colour)
{
	const float y = drawRowFrame(row, selected);
	fontDraw((float)(PB_ROW_X + TEXT_PAD), y, 1, colour, pauseBarRowLabel(tab, row));
	return y;
}

// The two stepper glyphs, drawn CENTRED IN THEIR OWN TOUCH BOXES rather than at hand-picked
// x's, so what the player aims at is what the hit test owns. Greyed at the ends rather than
// hidden, so the row does not change width when it hits a limit — a control that moves as you
// use it reads as a glitch.
static void drawArrows(int tab, int row, bool left_live, bool right_live)
{
	const PbRect l = pauseBarArrowRect(tab, row, PB_ARROW_LEFT);
	const PbRect r = pauseBarArrowRect(tab, row, PB_ARROW_RIGHT);
	const int    g = (PB_ARROW_W - FONT_GLYPH_W) / 2;
	fontDraw((float)(l.x + g), (float)(l.y + (l.h - FONT_GLYPH_H) / 2), 1,
	         left_live ? COL_TEXT : COL_DIM, "<");
	fontDraw((float)(r.x + g), (float)(r.y + (r.h - FONT_GLYPH_H) / 2), 1,
	         right_live ? COL_TEXT : COL_DIM, ">");
}

void pauseMenuDraw(const PauseStats* st)
{
	if (!pauseBarOpen(&s_bar) || !st) return;

	// Opens its own sprite pass rather than joining the one scene/ui.c uses, because
	// uiUpdateDraw closes its pass before returning — this draws after it, on top of the UI
	// it just finished. spriteTexture is the font's sheet for the same reason ui.c uses it:
	// spriteRect draws from the font texture's white texel, so a panel and the text on it
	// come from one binding and cost no flush between them. Unchanged from what shipped.
	spriteBegin(PB_SCR_W, PB_SCR_H);
	spriteTexture(fontTexture());

	// Scrim over the whole bottom screen first, then the panel on top of it. The scrim is
	// what makes the panel read as *over* the game rather than as another game screen.
	spriteRect(0.0f, 0.0f, (float)PB_SCR_W, (float)PB_SCR_H, COL_SCRIM);
	const PbRect panel = pauseBarPanelRect();
	spriteRect((float)(panel.x - 1), (float)(panel.y - 1),
	           (float)(panel.w + 2), (float)(panel.h + 2), COL_EDGE);
	drawRect(panel, COL_PANEL);

	const int tab = s_bar.tab;
	const int row = s_bar.row;
	drawStrip(tab);

	if (tab == PAUSEBAR_TAB_GAME) {
		drawRowLabel(tab, PB_GAME_RESUME, row == PB_GAME_RESUME,
		             row == PB_GAME_RESUME ? COL_TEXT : COL_DIM);
		// "Quit to title", not "Quit": the row leaves the world and lands on the title
		// screen, which is where the app's own quit lives. Saying just "Quit" next to a
		// Resume would read as quitting the game outright, and a player who wanted another
		// world would never press it. The wording lives in pauseBarRowLabel now.
		drawRowLabel(tab, PB_GAME_QUIT, row == PB_GAME_QUIT,
		             row == PB_GAME_QUIT ? COL_TEXT : COL_DIM);
	} else if (tab == PAUSEBAR_TAB_OPTIONS) {
		float y = drawRowLabel(tab, PB_OPT_DIST, row == PB_OPT_DIST, COL_TEXT);
		fontDrawf((float)PB_VALUE_X, y, 1, COL_TEXT, "%d", st->render_dist);
		drawArrows(tab, PB_OPT_DIST, st->render_dist > st->dist_min,
		           st->render_dist < st->dist_max);

		// 3D lived on SELECT until SELECT became the button that opens this menu; it is a
		// setting, so a settings tab is where it belongs. Both arrows are live because there
		// are two states and either one flips between them — which is also what A does.
		y = drawRowLabel(tab, PB_OPT_3D, row == PB_OPT_3D, COL_TEXT);
		fontDraw((float)PB_VALUE_X, y, 1, st->stereo ? COL_HEAD : COL_DIM,
		         st->stereo ? "On" : "Off");
		drawArrows(tab, PB_OPT_3D, true, true);

		// Sound. The value is read live out of the audio system rather than passed in through
		// PauseStats, so there is exactly one copy of the current volume in the process and no
		// way for the row to disagree with what the DSP was told.
		//
		// The label greys out when there is no audio at all — a console whose DSP firmware was
		// never dumped. The row still works and still saves, because the setting is a
		// preference about the game and not a property of this console; greying it is the only
		// honest signal that moving it will not change anything the player can hear.
		//
		// %3d%% rather than %d%%: a fixed four-character field means the number does not shift
		// left and right under the cursor as it crosses 9% and 100%.
		{
			const int pct = (int)(audioGetMasterVolume() * 100.0f + 0.5f);
			y = drawRowLabel(tab, PB_OPT_VOLUME, row == PB_OPT_VOLUME,
			                 audioAvailable() ? COL_TEXT : COL_DIM);
			fontDrawf((float)PB_VALUE_X, y, 1, COL_TEXT, "%3d%%", pct);
			drawArrows(tab, PB_OPT_VOLUME, pct > 0, pct < 100);
		}
	} else if (tab == PAUSEBAR_TAB_SYSTEM) {
		float y = drawRowLabel(tab, PB_SYS_CONTROLS, row == PB_SYS_CONTROLS, COL_TEXT);
		fontDraw((float)PB_VALUE_X, y, 1, COL_DIM, ">");
		y = drawRowLabel(tab, PB_SYS_DEBUG, row == PB_SYS_DEBUG, COL_TEXT);
		fontDraw((float)PB_VALUE_X, y, 1, COL_DIM, ">");
	}

	// Memory, on SYSTEM ONLY. Three numbers because they answer three different questions and
	// are drawn from three different pools — a single "free RAM" figure would be a fiction on
	// this console. World is the one the player's own building moves; linear is what the mesh
	// pool came out of and therefore what render distance actually spends; VRAM holds the
	// atlas and the framebuffers and barely moves at all.
	//
	// Gated on pauseBarTabShowsMemory rather than on "the tab happens to be 2", so the
	// condition is the one the host suite checks: the blueprint's lane-D red arm 2 is a
	// readout drawn on every tab.
	if (pauseBarTabShowsMemory(tab)) {
		float ly = (float)PB_MEM_Y0;
		fontDraw((float)(PB_ROW_X + TEXT_PAD), ly, 1, COL_HEAD, "MEMORY");
		ly += (float)PB_MEM_STEP;
		fontDrawf((float)(PB_ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "World  %lu / %lu KB",
		          (unsigned long)kb(st->world_used), (unsigned long)kb(st->world_budget));
		ly += (float)PB_MEM_STEP;
		fontDrawf((float)(PB_ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "Linear %lu KB free",
		          (unsigned long)kb(st->linear_free));
		ly += (float)PB_MEM_STEP;
		fontDrawf((float)(PB_ROW_X + TEXT_PAD), ly, 1, COL_TEXT, "VRAM   %lu KB free",
		          (unsigned long)kb(st->vram_free));
	}

	// One footer on every tab, so the panel's bottom line never moves. The string is
	// pausebar's, and it names L/R even on GAME and SYSTEM — where left/right do the same
	// thing — because the shoulder buttons are the only tab control a player can find without
	// discovering that left/right double up.
	fontDraw((float)PB_ROW_X, (float)PB_FOOTER_Y, 1, COL_DIM, pauseBarFooterLabel());
	spriteEnd();
}
