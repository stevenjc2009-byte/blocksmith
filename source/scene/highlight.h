// The targeted-block highlight: the cage drawn around whichever block the player is
// aiming at.
//
// The shape of this file is dictated by one hardware fact: GPU_Primitive_t (see
// 3ds/gpu/enums.h) only offers GPU_TRIANGLES, GPU_TRIANGLE_STRIP, GPU_TRIANGLE_FAN and
// GPU_GEOMETRY_PRIM. The PICA200 has no line primitive and no point primitive, and
// neither libctru nor citro3d expose a wireframe fill mode or a polygon-offset control
// to fake one some other way. So "draw a wireframe box" is not an option here — the
// cage is twelve thin rectangular boxes, one per cube edge, built from ordinary
// triangles.
#pragma once

#include <3ds.h>
#include <citro3d.h>

#include "world/raycast.h"

// Loads the shader and builds the cage geometry. False if either failed. highlightDraw
// is a no-op after a false return, so a failed load degrades to "no highlight" rather
// than a crash.
bool highlightInit(void);
void highlightExit(void);

// Draws the cage around the block `hit` names, in world block coordinates (the same
// space the mesher uses: the block occupies the unit cube from (x,y,z) to
// (x+1,y+1,z+1)). Call inside the frame, after the world has been drawn.
//
// Takes the whole RayHit, not loose x/y/z, so that "was anything actually hit" travels
// with the coordinates instead of being a separate condition every call site has to
// remember: if `hit->hit` is false, this returns immediately and draws nothing — a miss
// leaves x/y/z zero-initialised, and drawing that would cage block (0,0,0) instead of
// nothing. This is the one and only place that rule lives; callers should not also gate
// the call on hit->hit themselves.
//
// This leaves the GPU set up for itself and does NOT put anything back — no attribute
// layout, no TEV stage, no cull mode, no bound program. That is safe because
// chunkRenderDraw re-establishes its entire pipeline at the top of every call (see
// pipelineBind in chunk_render.c). The alternative, restoring the world's state from in
// here, means a second copy of the world's vertex format and TEV setup living in this
// file, silently going stale the first time the real one changes.
//
// It reads the world's projection through chunkRenderProjection() for the same reason:
// a cage drawn with even a slightly different field of view does not sit on the block.
void highlightDraw(const C3D_Mtx* view, const RayHit* hit);
