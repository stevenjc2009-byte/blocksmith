// The block-breaking crack overlay: the spreading fracture drawn on the block the player
// is currently mining. Roadmap task 50 part 3.
//
// This is a SEPARATE DRAW PASS and it deliberately never touches the chunk mesher or the
// mesh-slot pool. Break progress advances up to twenty times a second and a chunk remesh
// costs about 2.1 ms, so pushing the crack stage into the mesh would spend the whole
// 16.72 ms Old 3DS frame budget on rebuilding geometry that is already correct. Instead
// this module owns one small static vertex buffer, built once, and re-draws six faces at
// the target block every frame — the same trade scene/highlight.c makes for the targeting
// cage, and for the same reason.
//
// The art is its own texture, not a slot in the world atlas: see gfx/crackatlas.h for why
// the shared sheet cannot hold eight more tiles.
#pragma once

#include <3ds.h>
#include <citro3d.h>

// Loads the shader, builds the geometry and uploads the crack sheet. False if any of
// those failed, in which case crackOverlayDraw is a no-op — a failed load degrades to
// "no crack overlay", not a crash. Call after chunkRenderInit and before the first frame.
bool crackOverlayInit(void);
void crackOverlayExit(void);

// Draws the crack overlay on the block at world block coordinates (bx, by, bz), showing
// stage `stage`.
//
// `view` is the camera's view matrix — the same one handed to highlightDraw. The
// projection is read from chunkRenderProjection() rather than passed in, for the reason
// highlight.h gives: an overlay drawn with even a slightly different field of view does
// not sit on the block.
//
// `stage` is CLAMPED into 0..CRACK_STAGE_MAX (see gfx/crackatlas.h). Out of range is not
// an error and does not draw a marker — there is nowhere to put one on an eight-row sheet
// — so it saturates instead: below 0 draws stage 0, at or above CRACK_STAGES draws stage
// CRACK_STAGE_MAX. The clamp is stated here and enforced in one place in crackoverlay.c
// because an unclamped stage index would index past the sheet's last row and still sample
// *a* texture; this project has repeatedly paid for the fact that a wrong texture
// coordinate presents as bad art and never as an error.
//
// Unlike highlightDraw this carries NO "is anything happening" flag, because there is no
// natural one to carry: a break in progress is a property of the mining state, not of the
// block coordinates. The caller must not call this when no break is in progress — every
// stage from 0 up draws visible cracks, so calling it unconditionally paints a permanent
// fracture on whatever block the player is looking at.
//
// GPU state: this binds its own program, attribute layout, TEV stage 0, cull mode and the
// crack texture on unit 0, and puts none of them back — exactly as highlightDraw does,
// because chunkRenderDraw re-establishes the world's entire pipeline at the top of every
// call. The one exception is the ALPHA TEST, which this does switch back off before
// returning: scene/chunk_render.c ends its own draw by disabling it and its comment states
// that the later passes never set it, so leaving it armed would alpha-test whatever draws
// next against a channel it does not control.
//
// Call inside the frame, after the world has been drawn.
void crackOverlayDraw(const C3D_Mtx* view, int bx, int by, int bz, int stage);
