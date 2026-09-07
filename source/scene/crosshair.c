#include "scene/crosshair.h"

#include <stdint.h>

#include "gfx/font.h"
#include "gfx/sprite.h"

// Four rectangles, not a texture. spriteRect samples the font sheet's solid texel, so the
// reticle joins whatever batch is already bound and costs no atlas space and no extra draw
// call — see gfx/font.h on cell 95.
//
// It is drawn twice: a dark cross one pixel larger on every side, then the white cross on
// top of it. The outline is the whole point. A plain white reticle vanishes against snow,
// sand and the bright top faces of grass — which is most of what a player looks at — and a
// plain black one vanishes into cave mouths and night. Neither fails visibly enough to be
// reported as a bug; it just becomes hard to aim, so the outline is not decoration.
#define ARM        5.0f    // half-length of each bar, in pixels from the centre
#define THICK      2.0f    // bar thickness
#define EDGE       1.0f    // how far the outline extends past the bar on every side

#define COL_MARK   SPRITE_WHITE
#define COL_EDGE   SPRITE_RGBA(0, 0, 0, 160)

// ── The block-name label (v1.9.0 AIM-TEXT) ────────────────────────────────────────────
//
// White text on a translucent dark pill, centred under the reticle. The pill, not a
// per-glyph outline, for the same reason the reticle has an outline at all (above): white
// text alone vanishes on snow and sand. A pill costs ONE quad; an outline or drop shadow is
// a second copy of every glyph, so on a 15-character name the pill is 16 quads a pass where
// a shadow would be 30. The label is inside the reticle's batch, so it adds no draw call.
//
// Where it sits, on the 400x240 top screen (cx = 200, cy = 120):
//   reticle, with outline   x 194..205, y 114..125   (cx/cy +- (ARM + EDGE))
//   pill                    y 130..140               (LABEL_GAP rows under the outline)
//   glyphs                  y 132..138               (FONT_GLYPH_H rows, PAD inside the pill)
// and horizontally, for the longest name the registry can hold (AIMTEXT_MAX_CHARS = 15 ->
// 89 px of ink): pill x 154..246, glyphs x 156..244. Four clear rows between the outline
// and the pill, 94 px clear of the screen's edge either side at the widest.
//
// What it does NOT have to avoid, and why: the hotbar (scene/ui_layout.h HOTBAR_Y/HOTBAR_H,
// y 0..40), the health and hunger pips (HUD_PIPS_Y0 200, rows at 200..208 and 212..220), and
// the chest / furnace / hotbar hint rows (CHEST_HINT_Y 118, FURN_HINT_Y 136, and scene/ui.c's
// "tap a hotbar slot" line) are ALL drawn on the BOTTOM screen, inside ui.c's
// spriteBegin(SCR_W, SCR_H) batch on a 320x240 target. This function is only ever called
// from main.c's drawEye, which is only ever handed the top targets (screenTop and
// screenTopRight — main.c's own comment: "400x240 is the top screen, which is the only target
// drawEye is ever given"). Different render target, so overlap is impossible by construction,
// not by placement. The only other top-screen 2D in play is the name tags
// (scene/playermodel.c's playerModelDrawTags), which float over remote players wherever they
// project; the reticle is already drawn over those on purpose (main.c, "so it sits over the
// name tags rather than under them") and the label sits in the same batch after it.
//
// Quad budget, against gfx/sprite.c's SPRITE_MAX_QUADS of 1024 (sprite.c:47). That file's
// own worst-case arithmetic is roughly 790 for a frame that has never occurred: bottom screen
// ~450, top screen 2 x (165 tag glyphs + 4 reticle quads). This adds at most 1 + 15 = 16 quads
// an eye (fontDraw emits one quad per printable non-space character and a name is at most
// AIMTEXT_MAX_CHARS long; '_' is printable and counts), so 32 for a stereo frame: 822 of 1024,
// 202 quads of headroom. The cap is not touched.
#define LABEL_GAP    4    // rows between the outline's bottom edge and the pill's top
#define LABEL_PAD_X  2    // pill inset either side of the ink
#define LABEL_PAD_Y  2    // pill inset above and below the glyph rows
#define LABEL_SCALE  1

// Opacities at alpha 255. Both are scaled by the caller's alpha (aimTextAlpha(), 255 while a
// block is targeted and ramping to 0 after it is lost) so the pill and the text fade as one
// object rather than the text vanishing off a pill that is still there.
#define LABEL_ALPHA  255
#define PILL_ALPHA   120

// A cross centred on (cx, cy): one horizontal bar and one vertical bar, each grown by
// `pad` on all four sides. Both bars overlap in the middle, which is invisible for an
// opaque colour and is why the outline pass uses a single alpha rather than two blended
// layers meeting in a darker square at the centre... except that the two outline bars DO
// overlap, so the centre would double-blend. The white pass covers exactly that region,
// so the doubled texel is never seen. Do not reduce ARM below THICK/2 + EDGE or it will be.
static void cross(float cx, float cy, float pad, uint32_t colour)
{
	const float half_t = THICK * 0.5f + pad;
	const float half_l = ARM + pad;

	spriteRect(cx - half_l, cy - half_t, half_l * 2.0f, half_t * 2.0f, colour);
	spriteRect(cx - half_t, cy - half_l, half_t * 2.0f, half_l * 2.0f, colour);
}

// The pill and the text. `label` is non-NULL and non-empty and `alpha` is non-zero here —
// crosshairDraw gates all three.
static void drawLabel(float cx, float cy, const char* label, uint8_t alpha)
{
	// Integer scale, 0..255 in, 0..PILL_ALPHA / 0..LABEL_ALPHA out. uint32_t arithmetic:
	// 255 * 255 does not fit the uint8_t the inputs arrive in.
	const uint32_t pill_a = ((uint32_t)PILL_ALPHA  * (uint32_t)alpha) / 255u;
	const uint32_t text_a = ((uint32_t)LABEL_ALPHA * (uint32_t)alpha) / 255u;

	// fontTextWidth is a CELL width (it counts the last character's trailing advance), so
	// the ink is one pixel narrower; centring on the ink rather than the cell keeps the
	// pill symmetric around the reticle. Whole pixels, for the reason the reticle itself
	// rounds: a half-pixel edge blurs one side and not the other and reads as off-centre.
	const int   ink = fontTextWidth(label, LABEL_SCALE) - 1;
	const float x   = cx - (float)(ink / 2);
	const float y   = cy + (ARM + EDGE) + (float)(LABEL_GAP + LABEL_PAD_Y);

	spriteRect(x - (float)LABEL_PAD_X, y - (float)LABEL_PAD_Y,
	           (float)(ink + 2 * LABEL_PAD_X),
	           (float)(FONT_GLYPH_H * LABEL_SCALE + 2 * LABEL_PAD_Y),
	           SPRITE_RGBA(0, 0, 0, pill_a));
	fontDraw(x, y, LABEL_SCALE, SPRITE_RGBA(255, 255, 255, text_a), label);
}

void crosshairDraw(float w, float h, const char* label, uint8_t alpha)
{
	// Rounded to whole pixels. The centre of a 400x240 screen falls on 200.0, 120.0 exactly,
	// but the reticle is an odd number of pixels wide once the outline is counted, so leaving
	// it on a half-pixel would let the rasteriser blur one edge and not the other and the
	// cross would read as off-centre by a pixel.
	const float cx = (float)(int)(w * 0.5f);
	const float cy = (float)(int)(h * 0.5f);

	spriteBegin((int)w, (int)h);
	spriteTexture(fontTexture());

	cross(cx, cy, EDGE, COL_EDGE);
	cross(cx, cy, 0.0f, COL_MARK);

	// After the reticle so the pill can never cover the aim point, and before spriteEnd so
	// it is the same draw call. The empty-string and zero-alpha tests are boundary hygiene
	// rather than cases aimTextLabel()/aimTextAlpha() can produce together (the label is
	// NULL exactly when the alpha is 0, and an empty registry name is one of the hidden
	// states): a zero-width ink would otherwise put a 4 px pill under the reticle with
	// nothing in it, and an invisible label would still cost its quads.
	if (label && label[0] != '\0' && alpha != 0)
		drawLabel(cx, cy, label, alpha);

	spriteEnd();
}
