#include "scene/crosshair.h"

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

void crosshairDraw(float w, float h)
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

	spriteEnd();
}
