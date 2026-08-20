#pragma once

// The draw bisect's arm number, for the files that have to skip something to answer it.
//
// Round 1 lived entirely in main.c, because the four things it removed — the world, the
// highlight cage, the player models, the bottom-screen UI — are all called from there. It came
// back off real hardware with one survivor: arm 1, "no world". Every other arm froze the
// console, so chunkRenderDraw() is the call at fault and nothing else in the frame is.
//
// Round 2 has to cut inside that call, which lives in scene/chunk_render.c, so the arm number
// has to be readable from outside main.c. That is all this header is for. The bookkeeping —
// which arm runs next, and writing down that the last one never came back — stays in main.c
// where it already works.
//
// Defined only when BS_DRAW_PROBE is on; the callers guard their use the same way.
#ifndef BS_DRAW_PROBE
#define BS_DRAW_PROBE 0
#endif

#if BS_DRAW_PROBE
int bsProbeArm(void);
#endif
