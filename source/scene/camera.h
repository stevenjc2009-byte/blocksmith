// Free-fly camera.
//
// Convention, because getting it wrong shows up as inverted controls: the view
// matrix is Rx(pitch) * Ry(yaw) * T(-position), so the world-space forward
// direction is (sin yaw * cos pitch, -sin pitch, -cos yaw * cos pitch). Positive
// pitch therefore looks *down*, and the circle pad has to subtract to look up.
#pragma once

#include <3ds.h>
#include <citro3d.h>

typedef struct {
	float x, y, z;
	float yaw;     // radians, 0 looks along -Z
	float pitch;   // radians, positive looks down
} Camera;

void cameraInit(Camera* cam);

// Reads the circle pad and D-pad and moves the camera. `dt_ms` is the real frame
// time, so speed does not depend on framerate.
void cameraUpdate(Camera* cam, float dt_ms);

// The looking half of cameraUpdate on its own: circle pad to yaw and pitch, clamped
// short of straight up and down. Walking mode (scene/player.c) owns the position but
// wants exactly this aiming behaviour, and a second copy of the pad deadzone and the
// pitch limit is how the two modes end up feeling different for no stated reason.
void cameraLook(Camera* cam, float dt_ms);

void cameraView(const Camera* cam, C3D_Mtx* out);
