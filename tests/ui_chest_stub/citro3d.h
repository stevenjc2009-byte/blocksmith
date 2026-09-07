// Host stub for tests/ui_chest_test.c: the smallest <citro3d.h> that lets source/scene/ui.c
// be COMPILED on a host gcc, unmodified, so its chest transfer code can be tested against the
// real thing rather than against a hand-copied restatement of it.
//
// Same shape and the same reasoning as tests/player_water_stub/citro3d.h and
// tests/metrics_timing_stub/citro3d.h, which already do exactly this for scene/camera.c and
// for the metrics clock — see either file's own header comment. This is NOT a general-purpose
// citro3d stub: it names the two types the header chain scene/ui.c pulls in actually mentions
// on a code line (C3D_Tex, through gfx/font.h, gfx/sprite.h, gfx/atlas.h and scene/ui.h;
// C3D_Mtx, through scene/chunk_render.h) and nothing else.
//
// Nothing in this stub is ever dereferenced. ui.c only ever passes a C3D_Tex* along to
// spriteTexture()/drawSlotIcon(), and ui_chest_test.c passes NULL for `block_icons` — the
// same NULL scene/ui.h's own file comment documents as legal and as skipping the whole atlas
// pass — so the field list below exists to give the type a size, not a layout. It is
// deliberately NOT a byte-for-byte copy of libctru's, because a test that depended on that
// would be depending on a header this repo does not own.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct { void* data; uint16_t width, height; uint32_t fmt; } C3D_Tex;
typedef struct { float m[4][4]; } C3D_Mtx;
