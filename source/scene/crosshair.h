// The aiming reticle at the centre of the top screen.
//
// Separate from scene/ui.c because that file owns the bottom screen and its inventory
// panels; this is the only thing Blocksmith draws over the 3D view itself.
#pragma once

// Draws the reticle centred on a `w` x `h` target. Call inside a frame, after the world,
// with no sprite batch open — it opens and closes its own.
//
// Call it once per EYE, not once per frame. Drawing at the same screen coordinates in both
// eyes gives it zero parallax, which puts it at screen depth: the one place a reticle can
// sit without the eyes having to converge somewhere other than the block being aimed at.
void crosshairDraw(float w, float h);
