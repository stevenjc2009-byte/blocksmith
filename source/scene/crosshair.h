// The aiming reticle at the centre of the top screen, and (v1.9.0 AIM-TEXT) the block-name
// readout directly under it.
//
// Separate from scene/ui.c because that file owns the bottom screen and its inventory
// panels; this is the only thing Blocksmith draws over the 3D view itself.
#pragma once

#include <stdint.h>

// Draws the reticle centred on a `w` x `h` target, and, when `label` is non-NULL and
// non-empty and `alpha` is non-zero, that text on a dark backing centred under the reticle —
// see crosshair.c for the exact rect. `alpha` is the label's opacity, 0..255, applied to the
// text and its backing alike. Pass scene/aimtext.h's aimTextLabel() and aimTextAlpha(): the
// label is NULL for every hidden case and the alpha ramps to 0 through the fade, so nothing
// here has to know why there is no label or how far gone it is. Call inside a frame, after
// the world, with no sprite batch open — it opens and closes its own, and the label joins
// that same batch rather than opening a second one (one draw call for both).
//
// Call it once per EYE, not once per frame. Drawing at the same screen coordinates in both
// eyes gives it zero parallax, which puts it at screen depth: the one place a reticle can
// sit without the eyes having to converge somewhere other than the block being aimed at.
// The label rides at the same depth for the same reason — a name floating in front of or
// behind the block it names would be worse than no name.
void crosshairDraw(float w, float h, const char* label, uint8_t alpha);
