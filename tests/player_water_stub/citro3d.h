// Host stub for tests/player_water_test.c: the smallest <citro3d.h> that source/scene/
// camera.c's cameraView() compiles against (C3D_Mtx and the four Mtx_* calls it makes).
//
// v1.8.16 WATER-FX. camera.h is not owned by this lane (see tests/player_water_stub/3ds.h's
// header comment for the full reasoning) — this stub exists so camera.c can be compiled and
// linked, unmodified, into a private test binary; it is not a general-purpose citro3d stub and
// implements only the four calls camera.c's cameraView() makes.
//
// Real math, not a no-op: cheap to get right, and it removes any doubt that a wrong view
// matrix could somehow be the reason a position-based assertion in player_water_test.c read
// what it read — nothing in that test actually inspects a C3D_Mtx's contents (playerUpdate
// never calls cameraView; only main.c, which this test does not link, does that), but
// player.c's own struct includes a Camera by value and camera.c's translation unit has to
// link cleanly regardless of what this particular test happens to call.
//
// Column layout matches C:/devkitPro/libctru/include/c3d/mtx.h's C3D_Mtx closely enough for
// these four operations (m[4][4], row r, column c at m[r][c]); nothing outside this stub and
// camera.c ever reads a field of this type, so an exact byte-for-byte match to the real
// libctru struct is not required, only correct 4x4 homogeneous-transform arithmetic.
#pragma once

#include <stdbool.h>

typedef struct { float m[4][4]; } C3D_Mtx;

void Mtx_Identity(C3D_Mtx* out);
void Mtx_RotateX(C3D_Mtx* mtx, float angle, bool bRightSide);
void Mtx_RotateY(C3D_Mtx* mtx, float angle, bool bRightSide);
void Mtx_Translate(C3D_Mtx* mtx, float x, float y, float z, bool bRightSide);
