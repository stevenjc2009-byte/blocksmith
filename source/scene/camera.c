#include "scene/camera.h"

#include <math.h>

// Set -DBS_ORBIT=1 to replace the controls with an automatic orbit of the block.
// This exists because no keyboard key reaches the emulated console, so a build
// that needs a stick or a D-pad cannot be checked from a script. The orbit aims
// the camera at the block using the same yaw/pitch convention the controls use,
// so if the block stays centred through a full circle the convention is proven.
#ifndef BS_ORBIT
#define BS_ORBIT 0
#endif

#define MOVE_SPEED    4.0f    // blocks per second
#define LOOK_SPEED    2.6f    // radians per second at full stick
#define BOOST         3.0f    // multiplier while A is held
#define STICK_MAX     150.0f
#define STICK_DEAD    18.0f
#define PITCH_LIMIT   1.45f   // just short of straight up/down, which gimbals

#define ORBIT_CENTRE_X  0.5f
#define ORBIT_CENTRE_Y  0.5f
#define ORBIT_CENTRE_Z  0.5f
// Overridable with -DBS_ORBIT_R=14 to check the block at minification, which is
// where an atlas without padding would start bleeding.
#ifndef BS_ORBIT_R
#define BS_ORBIT_R      2.8f
#endif
#define ORBIT_RADIUS    BS_ORBIT_R

void cameraInit(Camera* cam)
{
	// Standing back from a block sitting at the origin, looking at it.
	cam->x = 0.5f;
	cam->y = 1.4f;
	cam->z = 3.2f;
	cam->yaw = 0.0f;
	cam->pitch = 0.25f;
}

static float stickAxis(int raw)
{
	const float v = (float)raw;
	if (v > STICK_DEAD)  return (v - STICK_DEAD) / (STICK_MAX - STICK_DEAD);
	if (v < -STICK_DEAD) return (v + STICK_DEAD) / (STICK_MAX - STICK_DEAD);
	return 0.0f;
}

#if BS_ORBIT
static void cameraOrbit(Camera* cam, float dt)
{
	static float t;
	t += dt;

	// Circle the block while rising and falling, so every face including the
	// bottom comes into view.
	const float angle  = t * 0.55f;
	const float height = ORBIT_CENTRE_Y + 2.1f * sinf(t * 0.37f);

	cam->x = ORBIT_CENTRE_X + ORBIT_RADIUS * sinf(angle);
	cam->y = height;
	cam->z = ORBIT_CENTRE_Z + ORBIT_RADIUS * cosf(angle);

	const float dx = ORBIT_CENTRE_X - cam->x;
	const float dy = ORBIT_CENTRE_Y - cam->y;
	const float dz = ORBIT_CENTRE_Z - cam->z;
	const float horiz = sqrtf(dx * dx + dz * dz);

	cam->yaw   = atan2f(dx, -dz);
	cam->pitch = -atan2f(dy, horiz);
}
#endif

void cameraLook(Camera* cam, float dt_ms)
{
	const float dt = dt_ms * 0.001f;

	circlePosition circle;
	hidCircleRead(&circle);

	cam->yaw   += stickAxis(circle.dx) * LOOK_SPEED * dt;
	cam->pitch -= stickAxis(circle.dy) * LOOK_SPEED * dt;   // stick up looks up

	if (cam->pitch >  PITCH_LIMIT) cam->pitch =  PITCH_LIMIT;
	if (cam->pitch < -PITCH_LIMIT) cam->pitch = -PITCH_LIMIT;
}

void cameraUpdate(Camera* cam, float dt_ms)
{
	const float dt = dt_ms * 0.001f;

#if BS_ORBIT
	cameraOrbit(cam, dt);
	return;
#else
	cameraLook(cam, dt_ms);

	const float cp = cosf(cam->pitch);
	const float fx = sinf(cam->yaw) * cp;
	const float fy = -sinf(cam->pitch);
	const float fz = -cosf(cam->yaw) * cp;

	// Right = forward x up, which for this convention is (cos yaw, 0, sin yaw).
	const float rx = cosf(cam->yaw);
	const float rz = sinf(cam->yaw);

	const u32 held = hidKeysHeld();
	float speed = MOVE_SPEED * dt;
	if (held & KEY_A) speed *= BOOST;

	if (held & KEY_DUP)    { cam->x += fx * speed; cam->y += fy * speed; cam->z += fz * speed; }
	if (held & KEY_DDOWN)  { cam->x -= fx * speed; cam->y -= fy * speed; cam->z -= fz * speed; }
	if (held & KEY_DRIGHT) { cam->x += rx * speed; cam->z += rz * speed; }
	if (held & KEY_DLEFT)  { cam->x -= rx * speed; cam->z -= rz * speed; }
	if (held & KEY_R)      { cam->y += speed; }
	if (held & KEY_L)      { cam->y -= speed; }
#endif
}

void cameraView(const Camera* cam, C3D_Mtx* out)
{
	Mtx_Identity(out);
	Mtx_RotateX(out, cam->pitch, true);
	Mtx_RotateY(out, cam->yaw, true);
	Mtx_Translate(out, -cam->x, -cam->y, -cam->z, true);
}
