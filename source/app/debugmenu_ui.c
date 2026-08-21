#include "app/debugmenu_ui.h"

#include <3ds.h>
#include <stdio.h>

#include "gfx/font.h"
#include "gfx/sprite.h"
#include "scene/ui_layout.h"

#define COL_SCRIM   0xC0100C08u
#define COL_PANEL   0xF0201A14u
#define COL_EDGE    0xFF6B5137u
#define COL_TEXT    0xFFE8DCC8u
#define COL_DIM     0xFF9A8B76u
#define COL_SEL     0xFF4E86C8u
#define COL_HEAD    0xFFB4E0A0u
#define COL_UNAVAIL 0xFF605040u

#define PANEL_X 26
#define PANEL_Y 24
#define PANEL_W (SCR_W - PANEL_X * 2)
#define PANEL_H (SCR_H - PANEL_Y * 2)

#define ROW_H     22
#define ROW_X     (PANEL_X + 12)
#define ROW_W     (PANEL_W - 24)
#define TEXT_PAD  6

#define VISIBLE_ROWS 7

static bool s_open;
static int  s_cursor;
static int  s_scroll;

void debugMenuUiInit(void)
{
	s_open   = false;
	s_cursor = 0;
	s_scroll = 0;
}

void debugMenuUiOpen(void)
{
	s_open   = true;
	s_cursor = 0;
	s_scroll = 0;
}

bool debugMenuUiIsOpen(void) { return s_open; }

bool debugMenuUiUpdate(const DebugContext* ctx, bool* enabled,
                       uint32_t down,
                       bool touch_down, int touch_x, int touch_y)
{
	(void)enabled;
	if (!ctx) return false;

	if (s_open && (down & KEY_B)) {
		s_open = false;
		return false;
	}

	if (!s_open) return false;

	const int total = debugMenuCount();
	if (total <= 0) return false;

	if (down & KEY_DUP)   s_cursor = (s_cursor + total - 1) % total;
	if (down & KEY_DDOWN) s_cursor = (s_cursor + 1) % total;

	if (s_cursor < s_scroll) s_scroll = s_cursor;
	if (s_cursor >= s_scroll + VISIBLE_ROWS)
		s_scroll = s_cursor - VISIBLE_ROWS + 1;
	if (s_scroll < 0) s_scroll = 0;
	const int max_scroll = total > VISIBLE_ROWS ? total - VISIBLE_ROWS : 0;
	if (s_scroll > max_scroll) s_scroll = max_scroll;

	if (down & KEY_A) {
		DebugEntry* e = debugMenuEntry(s_cursor);
		if (e && e->available) {
			switch (e->kind) {
			case DEBUG_TOGGLE:
				if (e->getBool && e->setBool)
					e->setBool(e->ctx, !e->getBool(e->ctx));
				break;
			case DEBUG_SLIDER_INT:
				if (e->getInt && e->setInt) {
					int v = e->getInt(e->ctx) + 1;
					if (v > e->slider_max) v = e->slider_min;
					e->setInt(e->ctx, v);
				}
				break;
			case DEBUG_ACTION:
				if (e->action) e->action(e->ctx);
				break;
			default:
				break;
			}
		}
	}

	if (down & (KEY_DLEFT | KEY_DRIGHT)) {
		DebugEntry* e = debugMenuEntry(s_cursor);
		if (e && e->available && e->kind == DEBUG_SLIDER_INT &&
		    e->getInt && e->setInt) {
			int v = e->getInt(e->ctx);
			if (down & KEY_DLEFT)  v--;
			if (down & KEY_DRIGHT) v++;
			if (v < e->slider_min) v = e->slider_min;
			if (v > e->slider_max) v = e->slider_max;
			e->setInt(e->ctx, v);
		}
	}

	if (touch_down) {
		const int y0 = PANEL_Y + 34;
		for (int i = 0; i < VISIBLE_ROWS; i++) {
			int idx = s_scroll + i;
			if (idx >= total) break;
			int row_y = y0 + i * ROW_H;
			if (touch_x >= ROW_X && touch_x <= ROW_X + ROW_W &&
			    touch_y >= row_y - 3 && touch_y <= row_y + ROW_H - 5) {
				s_cursor = idx;
				DebugEntry* e = debugMenuEntry(idx);
				if (e && e->available) {
					if (e->kind == DEBUG_TOGGLE && e->getBool && e->setBool)
						e->setBool(e->ctx, !e->getBool(e->ctx));
					else if (e->kind == DEBUG_ACTION && e->action)
						e->action(e->ctx);
				}
				break;
			}
		}
	}

	spriteBegin(SCR_W, SCR_H);
	spriteTexture(fontTexture());

	spriteRect(0.0f, 0.0f, (float)SCR_W, (float)SCR_H, COL_SCRIM);
	spriteRect((float)(PANEL_X - 1), (float)(PANEL_Y - 1),
	           (float)(PANEL_W + 2), (float)(PANEL_H + 2), COL_EDGE);
	spriteRect((float)PANEL_X, (float)PANEL_Y, (float)PANEL_W, (float)PANEL_H, COL_PANEL);

	fontDraw((float)ROW_X, (float)(PANEL_Y + 10), 1, COL_HEAD, "DEBUG MENU");

	for (int i = 0; i < VISIBLE_ROWS; i++) {
		int idx = s_scroll + i;
		if (idx >= total) break;
		DebugEntry* e = debugMenuEntry(idx);
		if (!e) continue;

		const float y = (float)(PANEL_Y + 34 + i * ROW_H);
		const bool sel = (idx == s_cursor);

		if (sel)
			spriteRect((float)ROW_X, y - 3.0f, (float)ROW_W,
			           (float)(ROW_H - 2), COL_SEL);

		const uint32_t col = e->available
			? (sel ? COL_TEXT : COL_DIM)
			: COL_UNAVAIL;
		fontDraw((float)(ROW_X + TEXT_PAD), y, 1, col,
		         e->name ? e->name : "?");

		if (e->available) {
			char val[48];
			val[0] = '\0';

			switch (e->kind) {
			case DEBUG_TOGGLE:
				if (e->getBool)
					snprintf(val, sizeof(val), "%s",
					         e->getBool(e->ctx) ? "ON" : "OFF");
				break;
			case DEBUG_SLIDER_INT:
				if (e->getInt)
					snprintf(val, sizeof(val), "%d", e->getInt(e->ctx));
				break;
			case DEBUG_INFO:
				if (e->info)
					e->info(val, sizeof(val), e->ctx);
				break;
			default:
				break;
			}

			if (val[0]) {
				const int vw = fontTextWidth(val, 1);
				fontDraw((float)(ROW_X + ROW_W - vw - TEXT_PAD), y,
				         1, COL_TEXT, val);
			}
		}
	}

	fontDraw((float)ROW_X, (float)(PANEL_Y + PANEL_H - 16), 1, COL_DIM,
	         "A/Touch toggle   B close");

	spriteEnd();
	return true;
}
