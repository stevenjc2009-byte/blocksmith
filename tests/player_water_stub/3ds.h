// Host stub for tests/player_water_test.c: the smallest <3ds.h> that source/scene/player.c
// (via player.h's unconditional `#include <3ds.h>`, for KEY_A) and source/scene/camera.c
// compile against. Same reasoning as tests/metrics_timing_stub/3ds.h's own header comment —
// this names exactly the surface those two files reference and nothing more, and it is not a
// general-purpose libctru stub.
//
// v1.8.16 WATER-FX. player.h and camera.h are NOT owned by this lane and are not touched here
// or anywhere else in this change — see this lane's final report for why an external stub
// (nothing shared edited, only a new -I path for one private test binary) was chosen over
// scene/interact.h's `#ifdef __3DS__` split, which would have required restructuring camera.h
// too, a file this lane has no standing to touch.
//
// Unlike metrics_timing_stub, this test controls INPUT rather than the clock: hidCircleRead,
// hidKeysHeld and hidKeysDown are declared here but DEFINED by player_water_test.c itself,
// driven by globals the test sets before each playerUpdate() call — the same "the test owns
// the thing that would otherwise be nondeterministic" reasoning metrics_timing_stub applies to
// svcGetSystemTick, applied here to input instead of the clock.
//
// KEY_* bit values verified against C:/devkitPro/libctru/include/3ds/services/hid.h — the same
// verification app/options.h's own OPT_KEY_* copies already document (see that file for the
// cross-check); only the seven bits camera.c and player.h actually reference are named.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define BIT(n) (1u << (n))

#define KEY_A       BIT(0)    // player.h PLAYER_KEY_JUMP; camera.c's boost modifier
#define KEY_DRIGHT  BIT(4)
#define KEY_DLEFT   BIT(5)
#define KEY_DUP     BIT(6)
#define KEY_DDOWN   BIT(7)
#define KEY_R       BIT(8)
#define KEY_L       BIT(9)

typedef struct { s16 dx, dy; } circlePosition;

// Declared here (as the real libctru header would); defined in player_water_test.c, which
// owns three globals (g_test_stick, g_test_keys_held, g_test_keys_down) the test sets before
// every playerUpdate()/cameraLook() call so a frame's input is exact and repeatable rather
// than read from whatever hardware happens to be attached — there is none, on a host build.
void hidCircleRead(circlePosition* pos);
u32  hidKeysHeld(void);
u32  hidKeysDown(void);
