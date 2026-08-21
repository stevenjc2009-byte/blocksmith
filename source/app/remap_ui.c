#include "app/remap_ui.h"

#include <3ds.h>
#include <stdio.h>

#include "gfx/font.h"
#include "gfx/sprite.h"
#include "scene/ui_layout.h"

// ── Layout ────────────────────────────────────────────────────────────────────
//
// Mirrors pausemenu.c's panel geometry: same panel, same row height, same
// colours. The remap screen replaces the OPTIONS page rather than layering on
// top of it, so B closes it and returns to OPTIONS, not to the world.

#define COL_SCRIM   0xC0100C08u
#define COL_PANEL   0xF0201A14u
#define COL_EDGE    0xFF6B5137u
#define COL_TEXT    0xFFE8DCC8u
#define COL_DIM     0xFF9A8B76u
#define COL_SEL     0xFF4E86C8u
#define COL_HEAD    0xFFB4E0A0u
#define COL_PROMPT  0xFFFFD080u   // "Press a button" flashing prompt

#define PANEL_X 26
#define PANEL_Y 24
#define PANEL_W (SCR_W - PANEL_X * 2)
#define PANEL_H (SCR_H - PANEL_Y * 2)

#define ROW_H     22
#define ROW_X     (PANEL_X + 12)
#define ROW_W     (PANEL_W - 24)
#define TEXT_PAD  6

// How many action rows fit above the "Reset to defaults" row. The panel is
// PANEL_H - 34 (header) - 22 (footer hint) = 184 px. 184 / 22 = 8.3 rows,
// so 6 action rows fit with room for the header, the reset row, and the
// footer hint. The reset row is always the last visible row, never scrolled.
#define VISIBLE_ACTIONS 6

// The reset row is always drawn at the bottom of the visible area.
#define RESET_DRAW (VISIBLE_ACTIONS)

static int s_scroll;    // first visible action index
static int s_cursor;    // 0..action_count (action_count = the "Reset to defaults" row)

// ── Drawing helpers ──────────────────────────────────────────────────────────

static void drawRow(int index, const char* label, bool selected)
{
	const float y = (float)(PANEL_Y + 34 + index * ROW_H);
	if (selected)
		spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W, (float)(ROW_H - 2), COL_SEL);
	fontDraw((float)(ROW_X + TEXT_PAD), y, 1, selected ? COL_TEXT : COL_DIM, label);
}

// ── Entry points ─────────────────────────────────────────────────────────────

void remapUiInit(void)
{
	s_scroll = 0;
	s_cursor = 0;
}

bool remapUiUpdate(RemapState* rs, uint32_t down,
                   bool touch_down, int touch_x, int touch_y)
{
	if (!rs) return false;

	const int action_count = remapActionCount();
	const int total_rows = action_count + 1;  // actions + reset row (cursor positions)

	// ── Capture mode ──────────────────────────────────────────────────────
	if (remapIsCapturing(rs)) {
		// B cancels capture without changing anything.
		if (down & KEY_B) {
			rs->capture_action = ACTION_COUNT;
		} else {
			// Any valid key press ends capture and applies the binding.
			for (int i = 0; i < OPTIONS_VALID_KEY_COUNT; i++) {
				if (down & OPTIONS_VALID_KEYS[i]) {
					remapApplyCapture(rs, OPTIONS_VALID_KEYS[i]);
					break;
				}
			}
		}

		// Draw the capture overlay.
		spriteBegin(SCR_W, SCR_H);
		spriteTexture(fontTexture());

		spriteRect(0.0f, 0.0f, (float)SCR_W, (float)SCR_H, COL_SCRIM);
		spriteRect((float)(PANEL_X - 1), (float)(PANEL_Y - 1),
		           (float)(PANEL_W + 2), (float)(PANEL_H + 2), COL_EDGE);
		spriteRect((float)PANEL_X, (float)PANEL_Y, (float)PANEL_W, (float)PANEL_H, COL_PANEL);

		const int ca = remapCaptureAction(rs);
		const char* action_name = remapActionName(ca);

		fontDraw((float)ROW_X, (float)(PANEL_Y + 10), 1, COL_HEAD, "REMAP");
		fontDrawf((float)ROW_X, (float)(PANEL_Y + 50), 1, COL_TEXT,
		          "%s", action_name ? action_name : "?");
		fontDraw((float)ROW_X, (float)(PANEL_Y + 80), 1, COL_PROMPT,
		         "Press a button...");
		fontDraw((float)ROW_X, (float)(PANEL_Y + 110), 1, COL_DIM,
		         "B to cancel");

		spriteEnd();
		return true;
	}

	// ── Navigation ────────────────────────────────────────────────────────
	if (down & KEY_DUP)   s_cursor = (s_cursor + total_rows - 1) % total_rows;
	if (down & KEY_DDOWN) s_cursor = (s_cursor + 1) % total_rows;

	// Scroll so the cursor is visible. The reset row is always at the bottom,
	// so s_scroll ranges from 0 to max(0, action_count - VISIBLE_ACTIONS).
	const int max_scroll = action_count > VISIBLE_ACTIONS ? action_count - VISIBLE_ACTIONS : 0;
	if (s_cursor < s_scroll) s_scroll = s_cursor;
	if (s_cursor >= s_scroll + VISIBLE_ACTIONS) s_scroll = s_cursor - VISIBLE_ACTIONS + 1;
	if (s_scroll > max_scroll) s_scroll = max_scroll;
	if (s_scroll < 0) s_scroll = 0;

	// A on the reset row: reset all bindings.
	if ((down & KEY_A) && s_cursor == action_count) {
		remapReset(rs);
	}

	// A on an action row: start capture.
	if ((down & KEY_A) && s_cursor < action_count) {
		remapStartCapture(rs, s_cursor);
	}

	// B: exit remap screen.
	if (down & KEY_B) return false;

	// ── Touch ─────────────────────────────────────────────────────────────
	if (touch_down) {
		const int y0 = PANEL_Y + 34;

		// Check action rows first.
		for (int i = 0; i < VISIBLE_ACTIONS; i++) {
			const int idx = s_scroll + i;
			if (idx >= action_count) break;
			const int row_y = y0 + i * ROW_H;
			if (touch_x >= ROW_X && touch_x <= ROW_X + ROW_W &&
			    touch_y >= row_y - 3 && touch_y <= row_y + ROW_H - 5) {
				s_cursor = idx;
				remapStartCapture(rs, idx);
				break;
			}
		}

		// Check the reset row (always at the bottom).
		const int reset_y = y0 + RESET_DRAW * ROW_H;
		if (touch_x >= ROW_X && touch_x <= ROW_X + ROW_W &&
		    touch_y >= reset_y - 3 && touch_y <= reset_y + ROW_H - 5) {
			s_cursor = action_count;
			remapReset(rs);
		}
	}

	// ── Draw ──────────────────────────────────────────────────────────────
	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());

	spriteRect(0.0f, 0.0f, (float)SCR_W, (float)SCR_H, COL_SCRIM);
	spriteRect((float)(PANEL_X - 1), (float)(PANEL_Y - 1),
	           (float)(PANEL_W + 2), (float)(PANEL_H + 2), COL_EDGE);
	spriteRect((float)PANEL_X, (float)PANEL_Y, (float)PANEL_W, (float)PANEL_H, COL_PANEL);

	fontDraw((float)ROW_X, (float)(PANEL_Y + 10), 1, COL_HEAD, "REMAP CONTROLS");

	// Draw visible action rows.
	for (int i = 0; i < VISIBLE_ACTIONS; i++) {
		const int idx = s_scroll + i;
		if (idx >= action_count) break;

		char buf[64];
		const char* name = remapActionName(idx);
		const uint32_t key = remapGetBinding(rs, idx);
		const char* key_name = remapKeyName(key);
		snprintf(buf, sizeof(buf), "%s: %s", name ? name : "?", key_name);
		drawRow(i, buf, s_cursor == idx);
	}

	// The reset row is always the last row.
	drawRow(RESET_DRAW, "Reset to defaults", s_cursor == action_count);

	fontDraw((float)ROW_X, (float)(PANEL_Y + PANEL_H - 16), 1, COL_DIM,
	         "A select   B back");

	spriteEnd();
	return true;
}
