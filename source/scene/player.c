#include "scene/player.h"

#include <math.h>

#include "app/input_map.h"
#include "gfx/particles.h"

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

	p->bob_phase = 0.0f;
	p->bob_env   = 0.0f;

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

	// v1.8.0 task 25. Asked once, here, and used by all three of the movement decisions
	// below — the walk speed, the vertical input, and (asked again inside bodyStep, which
	// owns its own answer so every caller of it gets water physics) the fall. Every rule
	// it feeds lives in world/physics.c: this file includes <3ds.h> through input_map.h
	// and cannot be linked into the host suite, so a decision left here is a decision
	// nothing checks. world_test.c's testPlayerWiresSwimming reads this file as text for
	// exactly that reason.
	// v1.8.2 widened this from a bool to a three-state answer, because the vertical input
	// needs to tell "head under" from "head out" and the horizontal one still does not
	// care. The old boolean predicate is now a wrapper over this same call, so the walk
	// speed below reads exactly as it did.
	// bodyWetUpdate, not bodyWetState: the submerged boundary is hysteretic and the band
	// is carried on the body, so the driven answer is the one the vertical input must see.
	// bodyStep calls it again below and gets the same answer at the same position.
	const BodyWet prev_wet = p->body.wet;   // bodyWetUpdate has not overwritten it yet -- see physics.c:412-417
	const BodyWet wet = bodyWetUpdate(w, &p->body);
	const bool submerged = (wet != BODY_DRY);

	// v1.8.9 particle system. docs/plan-1.8.9-particles-integration.md §5 -- the one splash
	// case the parent plan calls REQUIRED. prev_wet is safely last frame's answer (see the
	// comment on the line above and physics.c's own bodyWetUpdate comment: it reads b->wet
	// for its own hysteresis bias before overwriting it), so this fires exactly once, on the
	// frame the body's eye crosses from dry into either the surface or fully submerged --
	// not on a later surface<->submerged transition while already wet. p->body.vy is the
	// fall speed at the moment of entry, read before bodyStep (below) changes it.
	if (prev_wet == BODY_DRY && wet != BODY_DRY)
		particlesSpawnSplash(p->body.x, p->body.y, p->body.z, p->body.vy);

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
	const float speed = bodyWalkSpeed(submerged);
	const float len = sqrtf(ix * ix + iz * iz);
	if (len > 0.0001f) {
		ix = ix / len * speed;
		iz = iz / len * speed;
	}

	p->body.vx = ix;
	p->body.vz = iz;

	// Grounded single impulse on land, sustained rise in water, hold-in-place at the
	// surface. All three rules are in world/physics.c's bodyJump; this passes it the
	// state, the level, the edge and the frame time and lets it decide, so the whole
	// truth table is covered by the host suite. `dt` is the same clamped value bodyStep
	// gets below — the water branch is a rate, so both halves must agree on the tick.
	const u32 jump_bit = inputKey(ACTION_JUMP);
	bodyJump(&p->body, wet, (held & jump_bit) != 0, (hidKeysDown() & jump_bit) != 0, dt);

	bodyStep(&p->body, w, dt);

	// v1.8.9 particle system. playerUpdate's own already-clamped dt, not a second
	// metricsFrameMs()-derived one -- see docs/plan-1.8.9-particles-integration.md §4. Ticks
	// while !paused and not under BS_FLY (main.c only calls playerUpdate in that case); under
	// BS_FLY's spectator camera particles simply hold still rather than animate, a debug-only
	// gap disclosed by that doc rather than silently accepted here.
	particlesTick(dt);

	// v1.8.3 — the surface swell. Added to the eye AFTER the step, and read back by
	// nothing: the body is where the physics left it, and this is the only line in the
	// game that knows the camera is a few centimetres off it. p->body.wet rather than the
	// `wet` above, because bodyStep has just re-answered the question at the position the
	// body actually ended the frame at.
	const float bob = bodySurfaceBob(p->body.wet, dt, &p->bob_phase, &p->bob_env);

	p->cam.x = p->body.x;
	p->cam.y = p->body.y + PLAYER_EYE + bob;
	p->cam.z = p->body.z;
}
