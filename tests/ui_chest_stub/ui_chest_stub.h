// Host stubs for everything source/scene/ui.c reaches that is NOT under test in
// tests/ui_chest_test.c — the sprite batch, the font, the block atlas, the audio mixer, the
// biome readout, the world camera, and the one net send net/inv_bridge.c makes. See
// ui_chest_stub.c for what each one does and does not promise.
//
// This header exposes the two things the test reads back out of the stubs: what
// net/inv_bridge.c tried to put on the wire, and what the sprite batch was asked to draw.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Every networldSendInvAction() call the stub has seen since the last uiChestStubResetNet(),
// and the arguments of the most recent one.
//
// net/inv_bridge.c reports every BAG mutation through this (MOVE, SWAP, SELECT, CRAFT, PICKUP,
// CONSUME). A chest transfer applied locally must leave ZERO calls here — the chest path is
// slot-precise and never goes through the bridge (ui.c's chest block says why) — while a
// bag -> bag move made on the chest screen must still leave exactly one BS_INV_OP_MOVE, because
// that path IS the bridge. Reading this is how the test tells the two apart: the slots look the
// same either way, the wire does not.
typedef struct {
	int     calls;
	uint8_t op, a, b, c;   // the most recent call's arguments; meaningful when calls > 0
} UiChestStubNet;

const UiChestStubNet* uiChestStubNet(void);
void                  uiChestStubResetNet(void);

// What the sprite batch was asked to draw during the LAST spriteBegin()..spriteEnd() — the
// stub zeroes this on every spriteBegin(), so after one uiUpdateDraw() it describes exactly
// that frame.
//
// `quads` is what gfx/sprite.c's own per-frame counter would have reached: one per
// spriteRect(), one per spriteQuad(), and — because the real gfx/font.c emits one spriteQuad()
// per glyph it draws and skips spaces (font.c:120-140) — one per non-space printable glyph
// handed to fontDraw()/fontDrawf(). SPRITE_MAX_QUADS is 1024 in sprite.c and the real counter
// wraps silently past it, which is why the test measures this at all.
//
// `rect[]` records every spriteRect() geometry (capped; `rects` keeps counting past the cap),
// so the test can prove a slot cell was actually drawn where scene/ui_layout.h says it is,
// rather than only that "something" was drawn.
typedef struct { float x, y, w, h; uint32_t colour; } UiChestStubRect;

#define UI_CHEST_STUB_RECT_MAX 1200

typedef struct {
	int begins;        // spriteBegin() calls since the last reset (1 after one frame)
	int quads;         // see above — the console batch's total for the frame
	int rects;         // spriteRect() calls
	int icon_quads;    // spriteQuad() calls made directly (the atlas pass's icons)
	int glyph_quads;   // quads the real font would have emitted for the text drawn
	int tex_changes;   // spriteTexture() calls — one per draw call on the console
	UiChestStubRect rect[UI_CHEST_STUB_RECT_MAX];
} UiChestStubDraw;

const UiChestStubDraw* uiChestStubDraw(void);
