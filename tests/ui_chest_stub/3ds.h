// Host stub for tests/ui_chest_test.c: the smallest <3ds.h> that lets source/scene/ui.c be
// COMPILED on a host gcc, unmodified. Same reasoning, and the same "name exactly the surface
// these files reference and nothing more" rule, as tests/player_water_stub/3ds.h and
// tests/metrics_timing_stub/3ds.h — see either for the longer argument. It is not a
// general-purpose libctru stub.
//
// scene/ui.c itself never calls a libctru function. <3ds.h> arrives only because three
// headers in its include chain pull it in unconditionally — gfx/sprite.h:37, gfx/atlas.h:23
// and scene/chunk_render.h:10 — and every libctru name those headers (and everything they
// include) mention sits in a comment: grepped 2026-09-06, the only citro3d/libctru
// identifiers on a CODE line anywhere in that chain are C3D_Tex and C3D_Mtx, both supplied by
// the citro3d.h beside this file. The typedefs below are the fixed-width aliases the sibling
// stubs carry; they cost nothing, and a header in the chain that grows a real u32 field
// tomorrow still compiles here instead of failing on a name libctru would have had.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define BIT(n) (1u << (n))
