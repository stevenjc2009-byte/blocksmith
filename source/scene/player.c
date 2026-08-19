#include "scene/player.h"

#include <math.h>

#include "app/input_map.h"

// The largest tick the physics is ever handed, in seconds. Nothing in a steady frame
// comes close to this — 16.71 ms is 0.0167 s — but the first frame after startup follows
// the world build, the self-test and (on an instrumented build) a five-second remesh
// stress, and handing that whole gap to bodyStep would drop the player a hundred blocks
// before he ever saw the ground. Collision is substepped so a big tick is not a
// tunnelling risk; it is a "where did I go" risk, which is what this prevents.
#define MAX_TICK  0.05f

void playerInit(Player* p, float x, float y, float z, float yaw, float pitch)
{
	bodyInit(&p->body, x, y, z);

	cameraInit(&p->cam);
	p->cam.yaw   = yaw;
	p->cam.pitch = pitch;
	p->cam.x = x;
	p->cam.y = y + PLAYER_EYE;
	p->cam.z = z;
}

void playerUpdate(Player* p, const World* w, float dt_ms)
{
	float dt = dt_ms * 0.001f;
	if (dt > MAX_TICK) dt = MAX_TICK;

	cameraLook(&p->cam, dt_ms);

	// Walking is horizontal only, so the movement basis drops the pitch entirely: aiming
	// at your feet should not make you walk into the floor. Same yaw convention as
	// cameraView — forward at yaw 0 is -Z, and right is forward crossed with up.
	const float fx = sinf(p->cam.yaw);
	const float fz = -cosf(p->cam.yaw);
	const float rx = cosf(p->cam.yaw);
	const float rz = sinf(p->cam.yaw);

	const u32 held = hidKeysHeld();
	float ix = 0.0f, iz = 0.0f;

	// Step 8.4. The four directions come from the player's bindings rather than from KEY_DUP
	// and friends directly. app/input_map.c answers with exactly those defaults until an
	// options.ini says otherwise, so an unconfigured console walks on the D-pad as it always
	// did. The bits are the same libctru KEY_* values — main.c static-asserts that, since
	// app/options.h has to spell them as hex literals to stay host-compilable.
	if (held & inputKey(ACTION_MOVE_FORWARD)) { ix += fx; iz += fz; }
	if (held & inputKey(ACTION_MOVE_BACK))    { ix -= fx; iz -= fz; }
	if (held & inputKey(ACTION_MOVE_RIGHT))   { ix += rx; iz += rz; }
	if (held & inputKey(ACTION_MOVE_LEFT))    { ix -= rx; iz -= rz; }

	// Two directions at once must not be faster than one. The D-pad only ever produces
	// unit or 45-degree vectors, so this is a single normalise rather than a special case
	// per diagonal.
	const float len = sqrtf(ix * ix + iz * iz);
	if (len > 0.0001f) {
		ix = ix / len * PLAYER_WALK_SPEED;
		iz = iz / len * PLAYER_WALK_SPEED;
	}

	p->body.vx = ix;
	p->body.vz = iz;

	// Grounded only: holding jump must not fly. on_ground is set by bodyMove when a
	// downward move is stopped, so it is already false on the frame after a jump.
	if ((hidKeysDown() & inputKey(ACTION_JUMP)) && p->body.on_ground)
		p->body.vy = PLAYER_JUMP_SPEED;

	bodyStep(&p->body, w, dt);

	p->cam.x = p->body.x;
	p->cam.y = p->body.y + PLAYER_EYE;
	p->cam.z = p->body.z;
}
