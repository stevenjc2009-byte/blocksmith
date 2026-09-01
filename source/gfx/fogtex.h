// v1.9.0. The console half of the distance-fog ramp: the 128x8 A8 texture that turns the fog
// coordinate the vertex shader emits on texcoord1 into a fog factor, on texture unit 1.
//
// Split from gfx/fogramp.c so that file can stay <3ds.h>-free and be linked into the host
// suite, which is where the whole fade is actually checked. Everything interesting is over
// there; this is the four citro3d calls that put the table on the GPU.
#pragma once

#include <stdbool.h>

// Imports build/fogramp.t3x into VRAM, sets GPU_LINEAR filtering and GPU_CLAMP_TO_EDGE wrap on
// both axes, and binds it. Called from chunkRenderInit alongside atlasInit; false means the
// game refuses to boot, same as a failed atlas.
bool fogTexInit(void);

void fogTexExit(void);

// Rebinds unit 1. Called from pipelineBind for the same reason atlasBind() is: texture bindings
// are global GPU state and this file is not the only thing in the program that binds textures.
void fogTexBind(void);
