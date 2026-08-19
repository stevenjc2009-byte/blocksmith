// Step 8.3. The GUI's bitmap font — every character the game ever puts on screen.
//
// One 128x64 texture of 95 glyphs (ASCII 32..126), 5x7 pixels each in an 8x8 cell, drawn
// from scratch by tools/make_font.py. Nothing is borrowed: the glyph shapes are authored
// in that script as rows of '#' and '.', and gfx/font_preview.png is the 4x proof sheet
// they were checked against.
//
// ── Why a bitmap font and not a real one ──────────────────────────────────────────────
//
// libctru ships no font rasteriser worth the name for a game loop, and the shared system
// font (fontEnsureMapped) is a licensed asset of the console, not of this project. A
// bitmap font is also the only kind that stays sharp: at 1x a glyph is exactly its texels,
// and at 2x it is exactly four texels per texel, because the sampler is GPU_NEAREST. A
// scaled vector font on a 240px-tall screen would be a blur.
//
// ── The white pixel ───────────────────────────────────────────────────────────────────
//
// Cell 95 — one past the last glyph — is a solid white 8x8 block, and spriteRect() samples
// its centre. That is what lets a panel, its border and the label on top of it all be the
// same texture, so a whole screen of UI is one draw call rather than one per material.
//
// ── Colour ────────────────────────────────────────────────────────────────────────────
//
// The texture is white with an alpha channel; the colour comes from the vertex, modulated
// in the TEV stage (gfx/sprite.c). So text is tinted, not recoloured — there is exactly
// one font image no matter how many colours the UI uses.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <citro3d.h>

// A glyph's drawn size, and the pitch from one character to the next, in *unscaled*
// pixels. The advance is 6, not 5: the gap between characters is a real pixel of spacing
// rather than a fraction, so text stays aligned to the pixel grid at every integer scale.
#define FONT_GLYPH_W 5
#define FONT_GLYPH_H 7
#define FONT_ADVANCE 6
#define FONT_LINE    9   // baseline-to-baseline: 7 tall + 2 of leading

// Loads the texture. False if it could not be uploaded, which the caller should treat the
// same way it treats spriteInit failing.
bool fontInit(void);
void fontExit(void);

// The font's texture, to hand to spriteTexture(). Exposed rather than bound internally
// because the batch has to know when the texture changes, and only the caller knows the
// order it is drawing things in.
C3D_Tex* fontTexture(void);

// Draws `text` with its top-left corner at (x, y), `scale` pixels per font pixel. Stops at
// the terminator; a newline moves down FONT_LINE * scale and back to x. Characters outside
// 32..126 draw nothing and still advance, so a stray byte leaves a gap rather than garbage.
//
// The font texture must already be bound (fontTexture / spriteTexture) — this does not
// bind it itself, so a caller can interleave text and panels in one batch without forcing
// a flush between every one.
void fontDraw(float x, float y, int scale, uint32_t colour, const char* text);

// printf into a fixed 128-byte buffer, then fontDraw. Truncates rather than allocating:
// this is called per frame from the HUD, and a UI that mallocs every frame on a console
// with no virtual memory is a fragmentation problem waiting to happen.
void fontDrawf(float x, float y, int scale, uint32_t colour, const char* fmt, ...)
	__attribute__((format(printf, 5, 6)));

// Width in pixels the string would occupy, for centring and right-alignment. Counts the
// trailing advance of the last character, i.e. it is a *cell* width, which is what lines
// up when text sits next to a panel edge.
int fontTextWidth(const char* text, int scale);
