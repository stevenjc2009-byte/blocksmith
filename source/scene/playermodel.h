// playermodel.h — draws the other people in the session.
//
// net/networld.h decodes BS_APP_POS_UPDATE into a small table of NetworldRemote (sid plus
// position and facing). Until this file existed that table was decoded and then went
// nowhere: two consoles in one world could edit the same blocks and watch each other's
// changes appear, but could not see each other. This is the piece that puts a body on the
// pose data.
//
// Deliberately a placeholder, and named like one. Three axis-aligned boxes — body, head,
// and a nose so that yaw is legible rather than merely applied — in a flat per-player
// colour, with the player's label floating above. There is no skin, no animation and no
// limb: the point is to prove the pose pipeline end to end and give the session something
// to look at, not to be the final character. It borrows scene/highlight.c's approach
// wholesale (position-only float vertices, one static buffer built at init, colour from a
// TEV constant, the world's own projection) because that file already solved "draw a small
// piece of untextured geometry at an arbitrary world position" for this codebase.
//
// No heap allocation after init, and the per-frame cost is bounded by NETWORLD_MAX_REMOTE
// (15) regardless of what arrives on the wire — this runs on an Old 3DS at 60 FPS.
#pragma once

#include <stdbool.h>

#include <citro3d.h>

// Builds the shared box mesh and picks up the highlight shader's uniforms. False if the
// vertex buffer could not be allocated, in which case every draw below is a silent no-op
// and the rest of the game is unaffected — the same degrade-to-nothing posture
// highlightInit() has.
bool playerModelInit(void);

void playerModelExit(void);

// Draws one box body per remote player, in the 3D pass. Call from inside drawEye() after
// chunkRenderDraw(), with the same view matrix the world was drawn with; the projection is
// read from chunkRenderProjection() so this automatically gets the right eye's matrix in
// stereo, exactly as highlightDraw() does.
//
// Like highlightDraw(), this leaves the GPU set up for itself and puts nothing back. That
// is safe for the same reason: chunk_render.c's pipelineBind() re-establishes the whole
// world pipeline at the top of every chunkRenderDraw() call.
void playerModelDraw(const C3D_Mtx* view);

// Draws each remote player's name above their head, in a 2D sprite batch on whichever
// render target is currently bound. Separate from playerModelDraw() because gfx/font.h
// only draws in screen space — there is no world-space text in this engine — so the tag
// position has to be projected by hand and then drawn in a completely different pipeline.
// Call immediately after playerModelDraw(), inside the same drawEye().
//
// `view` must be the same matrix passed to playerModelDraw(). `scr_w` / `scr_h` are the
// target's dimensions in the sprite batch's own top-left-origin coordinates (400 x 240 for
// the top screen).
//
// This opens and closes its own spriteBegin()/spriteEnd() batch, which turns depth testing
// off and alpha blending on and does not restore either (see gfx/sprite.h). Both are
// re-established by the next chunkRenderDraw(). Tags therefore draw through terrain on
// purpose: knowing where someone is standing is more useful than hiding them behind a hill
// while the multiplayer session is still being proven out.
void playerModelDrawTags(const C3D_Mtx* view, float scr_w, float scr_h);
