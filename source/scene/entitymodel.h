// entitymodel.h — draws the animals in the world.
//
// v1.8.14 P1. This is the *probe*, not the renderer. Its purpose is a measurement, not a
// feature: nothing in this codebase has ever drawn a model in the world with a texture on
// it, and — the part that actually matters — no per-draw-call cost has ever been measured
// here at all. The only figure on record is playermodel.h's own assertion that 15 remotes
// at 2 draws each holds 60 FPS on an Old 3DS, i.e. 30 draws per eye, and that assertion is
// itself unmeasured. The animal design wants a cap of 32, which in stereo is 64 draws per
// frame — already past the unmeasured figure. Two unmeasured numbers are not a budget, so
// this file exists to turn the larger of them into a real one before anyone paints a pig.
//
// Consequently it is deliberately the cheapest thing that can be measured. One flat-shaded
// axis-aligned box per animal, in a per-kind colour from a TEV constant, on a position-only
// 12-byte vertex, through scene/highlight.c's shader reused verbatim — exactly what
// scene/playermodel.c already does, because that is the only world-space moving-model
// precedent this project has and copying it means the cost being measured is the cost of
// the technique the real renderer will use. No atlas, no UVs, no new .pica: that is P6/P7's
// work and it is not started here.
//
// The one place this departs from playermodel.c is culling, which it must have and
// playermodel.c does not: fifteen remote players is a rounding error, thirty-two animals is
// not. See entityModelDraw.
//
// Position-only vertices are also what keeps this file clear of the PICA attribute-alignment
// trap. citro3d has no per-attribute offset — AttrInfo_AddLoader appends, and the hardware
// computes each attribute's offset as the running sum of the sizes before it — so a
// 3-component attribute followed by a 4-component one puts the second at byte offset 3 and
// leaves it unaligned forever. That froze real consoles from v1.1.0 to v1.2.4 and never
// reproduced in Azahar, because x86 does unaligned loads silently. With a single attribute
// there is no second offset to get wrong. The textured vertex format that P6 introduces has
// to solve this properly (world/mesh_vertex.h documents the fix); this file sidesteps it.
#pragma once

#include <stdbool.h>

#include <citro3d.h>

#include "entity/entity.h"

// Builds the shared box mesh and picks up the highlight shader's uniforms. False if the
// vertex buffer could not be allocated, in which case every draw below is a silent no-op and
// the rest of the game is unaffected — the same degrade-to-nothing posture playerModelInit()
// and highlightInit() have.
bool entityModelInit(void);

void entityModelExit(void);

// Draws every ANIMAL-kind entity in ew. Culls internally (distance + frustum).
// Call from inside drawEye(), beside playerModelDraw(). Does not restore GPU
// state -- the next chunkRenderDraw()'s pipelineBind() re-establishes it, the
// same contract playermodel.h:39-41 states.
void entityModelDraw(const C3D_Mtx* view, const EntityWorld* ew);
