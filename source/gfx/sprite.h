// Step 8.3. The GUI's only drawing primitive: a batched 2D quad.
//
// Everything the game draws that is not the world goes through here — the bitmap font
// (gfx/font.h), the hotbar, the inventory, the title screen. There is one shader, one
// vertex buffer and one texture, and a whole screen of UI is normally **one draw call**.
//
// ── Why batched, on a console with no draw-call problem to speak of ───────────────────
//
// Not because a hundred draws would be slow in isolation, but because of what a draw call
// costs *here*: every C3D_DrawArrays writes commands into the shared command buffer, and
// step 6.x measured that buffer's usage as one of the numbers that has to stay under
// control (metrics reports cmdbuf_usage for exactly this reason). A line of twenty
// characters is twenty quads; an inventory screen is a few hundred. Submitting those one
// at a time would put a UI-shaped spike into a per-frame budget that the world already
// owns most of. Filling an array and submitting once keeps the UI's cost proportional to
// its *pixels* rather than to how many things are on it.
//
// The batch flushes on exactly three events: the texture changes, the buffer fills, or
// spriteEnd is called. Nothing else invalidates it, which is why the solid-white texel
// lives inside the font texture (see tools/make_font.py) — a panel behind a label is then
// the same texture as the label, and the two go out together.
//
// ── Coordinates ───────────────────────────────────────────────────────────────────────
//
// Screen pixels, origin top-left, y down — the way a UI is laid out on paper, not the way
// the 3DS framebuffer is stored. The 90-degree rotation every 3DS screen needs is carried
// by the ortho matrix (Mtx_OrthoTilt), so no caller ever has to think about it.
//
// z is the layer: 0 is nearest the viewer, larger is further away. Depth *testing* is off
// during a UI pass — the batch draws in submission order, which is what a UI wants — so z
// exists only for the rare case where something has to be drawn out of order.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <3ds.h>
#include <citro3d.h>

// Packs a colour the way the vertex attribute wants it. Alpha is real: the UI blends.
#define SPRITE_RGBA(r, g, b, a) \
	((uint32_t)(uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | \
	 ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(a) << 24))

#define SPRITE_WHITE  SPRITE_RGBA(255, 255, 255, 255)

// Compiles the shader and claims the vertex buffer. False if either failed, which the
// caller must treat as "no UI this run" rather than a reason to refuse to boot — a game
// with no hotbar is worse than a game, but it is still a game.
bool spriteInit(void);
void spriteExit(void);

// Opens a 2D pass. `w` and `h` are the target's logical size in pixels — 400x240 for the
// top screen, 320x240 for the bottom — and set the ortho matrix, so passing the wrong
// ones scales the whole UI rather than failing.
//
// Sets its own render state (blending on, depth test off, no culling) and does not put it
// back: the world's own draw sets everything it needs at the top of its pass, so restoring
// here would be two state changes to undo one.
void spriteBegin(int w, int h);

// Binds a texture for subsequent quads, flushing whatever is pending if it differs. Safe
// to call with the same texture repeatedly — that costs nothing.
void spriteTexture(C3D_Tex* tex);

// One textured quad. UVs are 0..1 in the bound texture, v0 the quad's top edge.
//
// V runs BOTTOM-UP, because that is the axis PICA200 texture space uses: a source image's
// top row is v=1 and its bottom row is v=0, so a cell at top-down pixel row `py` in a
// texture `H` tall is v = 1 - py/H. Converting from an image editor's coordinates without
// that subtraction samples the vertically mirrored row, which does not read as a bug — it
// reads as a corrupt font, because every glyph still resolves to some other glyph.
// gfx/font.c does the conversion; anything else handing pixel rows to this must too.
void spriteQuad(float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, uint32_t colour);

// A filled rectangle in the current texture's solid texel (gfx/font.h supplies one). Same
// batch, same draw call as the text on top of it.
void spriteRect(float x, float y, float w, float h, uint32_t colour);

// Submits whatever is pending. Must be called before the frame ends or the last batch is
// simply lost.
void spriteEnd(void);

// Draw calls and quads issued since the last spriteBegin. Reported rather than assumed:
// "the UI is one draw call" is a claim, and this is the number that settles it.
int spriteDrawCount(void);
int spriteQuadCount(void);
