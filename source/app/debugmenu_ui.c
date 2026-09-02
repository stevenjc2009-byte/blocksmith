#include "app/debugmenu_ui.h"

#include <stdio.h>

#include "scene/ui_layout.h"

// v1.8.8. The BLOCK LIST sub-screen. Pure — no <3ds.h> — so this include is safe in the
// host build that app/debugmenu_test.c makes of this file. Only DECLARATIONS come in here;
// every CALL into it is inside the `#ifdef __3DS__` half below, which is what keeps
// tools/run_host_tests.sh's debugmenu_test stanza linking exactly what it linked before.
// See debug/blocklist.h's "screen's own state machine" note.
#include "debug/blocklist.h"

// <3ds.h> and the sprite/font batch are console-only; everything above the draw
// block is plain integer work on a button word. Split the same way app/battery.c
// splits its bar arithmetic from its PTM:U and drawing half, and for the same reason: the input
// half is where the bugs are, and "pressing A on the frame the menu opened must not
// activate the row under the cursor" is a claim about an input sequence that an
// emulator run cannot prove but source/app/debugmenu_test.c can, in a second.
#ifdef __3DS__
#include <3ds.h>

#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#else
// Host stand-ins for the only libctru symbols this file uses. Same duplication —
// and the same risk — as app/options.h's OPT_KEY_* block, which documents these bit
// values as verified against C:\devkitPro\libctru\include\3ds\services\hid.h:
//   KEY_A BIT(0), KEY_B BIT(1), KEY_DRIGHT BIT(4), KEY_DLEFT BIT(5),
//   KEY_DUP BIT(6), KEY_DDOWN BIT(7).
// Only the host build sees these; the console build uses the real header above, so a
// drift upstream changes behaviour on console and is caught by the host test failing.
#define KEY_A      0x00000001u
#define KEY_B      0x00000002u
#define KEY_DRIGHT 0x00000010u
#define KEY_DLEFT  0x00000020u
#define KEY_DUP    0x00000040u
#define KEY_DDOWN  0x00000080u
#endif

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

// Armed by debugMenuUiOpen(), consumed by the first debugMenuUiUpdate() after it. See
// that consumption site for the whole story; the flag lives up here with the rest of the
// screen's state because "am I on my opening frame" is screen state, not input state.
static bool s_swallow_frame;

#ifdef __3DS__
// Submits one page of the BLOCK LIST. It decides nothing: debug/blocklist.c emits the whole
// page as a BlockListOp program and this is the loop that turns each op into exactly one
// draw call. That is the point of the split — the host test and the scratchpad rasteriser
// consume this same op list, so what they check is what this submits, not a re-derivation
// of it.
//
// Two passes over the ops rather than one, because they need two different textures and the
// sprite batch flushes on every texture change (gfx/sprite.h:80). Interleaved, a 16-row page
// would flush ~32 times; split, it flushes once. scene/ui.c splits its hotbar the same way
// and for the same reason.
static void drawBlockList(void)
{
	BlockListOp ops[BL_MAX_OPS];
	const int n = blockListBuild(blockListUiPage(), ops, BL_MAX_OPS);
	if (n <= 0) return;   // build refuses rather than half-drawing; see blocklist.h

	spriteBegin(SCR_W, SCR_H);

	// Pass 1 — the font sheet. It carries the solid white texel that spriteRect samples
	// (gfx/sprite.h:19), so the background, the row plates and every string are one batch.
	spriteTexture(fontTexture());
	for (int i = 0; i < n; i++) {
		switch (ops[i].kind) {
		case BL_OP_RECT:
		case BL_OP_CELL:   // same draw as BL_OP_RECT; the tag is only for the tests
			spriteRect(ops[i].x, ops[i].y, ops[i].w, ops[i].h, ops[i].colour);
			break;
		case BL_OP_TEXT:
			fontDraw(ops[i].x, ops[i].y, 1, ops[i].colour, ops[i].text);
			break;
		default:
			break;
		}
	}

	// Pass 2 — the block atlas, for the icons. atlasTexture() rather than atlasBind(): the
	// batch tracks texture changes itself, and binding behind its back would leave it certain
	// it had not switched. gfx/atlas.h spells that out at the accessor.
	C3D_Tex* atlas = atlasTexture();
	if (atlas) {
		spriteTexture(atlas);
		for (int i = 0; i < n; i++)
			if (ops[i].kind == BL_OP_ICON)
				spriteQuad(ops[i].x, ops[i].y, ops[i].w, ops[i].h,
				           ops[i].u0, ops[i].v0, ops[i].u1, ops[i].v1,
				           ops[i].colour);
	}

	spriteEnd();
}
#endif  // __3DS__

void debugMenuUiInit(void)
{
	s_open          = false;
	s_cursor        = 0;
	s_scroll        = 0;
	s_swallow_frame = false;
#ifdef __3DS__
	// Inside the guard so the host build of this file needs no debug/blocklist.c on its link
	// line — see the seam note in debug/blocklist.h. The block list's own host test drives
	// blockListUiReset() directly, so this call being console-only costs no coverage.
	blockListUiReset();
#endif
}

void debugMenuUiOpen(void)
{
	s_open   = true;
	s_cursor = 0;
	s_scroll = 0;
	// Re-armed on every open, not just the first: closing and reopening otherwise brings
	// the bug straight back on the second visit (testUiOpenFrameGuardIsOncePerOpen).
	s_swallow_frame = true;
}

bool debugMenuUiIsOpen(void) { return s_open; }

bool debugMenuUiUpdate(const DebugContext* ctx, bool* enabled,
                       uint32_t down,
                       bool touch_down, int touch_x, int touch_y)
{
	(void)enabled;
	if (!ctx) return false;

	// The frame that opened this menu carries input that has already been spent. main.c
	// reads hidKeysDown() once per frame into one word, hands it to pauseMenuInput() —
	// which consumes KEY_A to pick the "Debug" row and calls debugMenuUiOpen() — and then
	// hands the SAME, unchanged word to this function in the same loop iteration.
	// debugMenuUiOpen() has just parked the cursor on entry 0, so without this guard the
	// one A press that opened the menu also activated row 0: the "Render distance" slider
	// stepped and the whole ring re-meshed on every single open. The stylus has the same
	// problem from the other direction — a touch still held from the tap that reached the
	// menu lands on whatever row happens to sit under it.
	//
	// Swallowed rather than returned on, so the panel still draws on its opening frame: an
	// early return here would show one frame of bare pause menu before the panel appears.
	// down/touch_down are this function's own parameters, so zeroing them affects nothing
	// outside it, and every branch below sees a clean frame instead of each one needing
	// its own guard.
	if (s_swallow_frame) {
		s_swallow_frame = false;
		down            = 0;
		touch_down      = false;
	}

#ifdef __3DS__
	// ── The BLOCK LIST sub-screen ────────────────────────────────────────────────────
	//
	// Handled here, ABOVE the menu's own KEY_B close, because both screens use B to go
	// back and the innermost one has to win. Below this point B would close the whole
	// debug menu and the block list would be unreachable-but-still-open underneath it.
	//
	// Console-only for the reason debug/blocklist.h documents at its state machine:
	// app/debugmenu_test.c compiles THIS FILE as source and links only options.c + hw.c +
	// render_dist.c + debugmenu.c, so a host-visible call to blockListUiInput() would put
	// the registry and the block table on that link line — a stanza in
	// tools/run_host_tests.sh that this change does not own. The logic behind the seam is
	// host-tested in source/debug/blocklist_test.c instead; what is unguarded here is only
	// the key mapping and the draw.
	if (s_open && blockListUiIsOpen()) {
		uint32_t bl = 0;
		if (down & KEY_B)                          bl |= BL_KEY_CLOSE;
		if (down & (KEY_R | KEY_DRIGHT | KEY_DDOWN)) bl |= BL_KEY_NEXT;
		if (down & (KEY_L | KEY_DLEFT  | KEY_DUP))   bl |= BL_KEY_PREV;

		if (blockListUiInput(bl)) drawBlockList();
		// Either way the frame is spent: on close, returning true keeps the debug menu
		// open and the very next frame draws it, so B steps back one screen instead of
		// dropping the player two.
		return true;
	}

	// X opens it. Not a debug-menu ROW: debugMenuUiInit() is called four times across
	// app/debugmenu_test.c's cases, so registering a row from here would duplicate it and
	// shift every index that file pins. A button costs that file nothing.
	if (s_open && (down & KEY_X)) {
		blockListUiOpen();
		drawBlockList();
		return true;
	}
#endif  // __3DS__

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

	// touch_down is a rising EDGE, not the held level: main.c derives it from
	// hidKeysHeld() & KEY_TOUCH against the previous frame's value before calling here.
	// It has to be, because this block toggles: fed the level, a stylus resting on a
	// toggle row flipped it once per frame at 60 Hz and the state on lift-off was a coin
	// flip. The edge is derived at the call site rather than with a static touch_prev of
	// our own so the whole frame's input is read in one place — and so this function stays
	// a pure function of its arguments, which is what lets debugmenu_test.c drive a single
	// press as one call.
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

#ifdef __3DS__
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
	         "A/Touch toggle  B close  X block list");

	spriteEnd();
#endif  // __3DS__
	return true;
}
