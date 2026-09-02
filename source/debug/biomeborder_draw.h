// The console half of the NEON BIOME BORDERS debug overlay. See debug/biomeborder.h for what
// the feature is, why it is debug-only, and where the geometry comes from.
//
// This header is the only part source/main.c sees of the drawing. It is behind <3ds.h> the
// same way scene/highlight.h is, and for the same reason: everything host-testable about this
// feature is in debug/biomeborder.h, and what is left here is binding a shader and issuing
// draw calls, which no host test can execute.
#pragma once

#include <3ds.h>
#include <citro3d.h>

#include <stdbool.h>

// Builds the shader program (once per process) and claims the vertex buffer. Returns false if
// either fails, and a false return degrades to "no overlay" rather than to a crash — exactly
// as scene/highlight.c's does.
//
// Safe to call when the feature will never be switched on; that is the normal case, and it
// costs one DVLB parse and one linearAlloc at boot.
bool biomeBorderDrawInit(void);

// Releases the vertex buffer. Deliberately does NOT free the shader program — see the note in
// biomeborder_draw.c, which is scene/highlight.c's use-after-free note restated: freeing a
// shaderProgram_s across a rejoin hands citro3d a dangling shaderInstance and hard-crashes the
// console.
void biomeBorderDrawExit(void);

// One overlay pass, drawn inside the world pass so the fence is depth-tested against the
// terrain. Call it with the same view matrix chunkRenderDraw() was given.
//
// THE OFF PATH IS THE FIRST LINE OF THIS FUNCTION and it is the whole cost of the feature
// when the toggle is off: one byte loaded, one branch, return. Nothing is built, no GPU state
// is touched, and no draw call is issued, so the frame is byte-for-byte the frame this
// program drew before the overlay existed.
void biomeBorderDraw(const C3D_Mtx* view);

// Drop the cached fence so the next draw rebuilds it. For the moment the world changes under
// the overlay — a re-generation, a join — rather than being called every frame.
void biomeBorderDrawInvalidate(void);
