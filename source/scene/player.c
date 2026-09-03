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

// v1.8.17 WATER-FX task 3 -- the swim wake. Horizontal speed a body must exceed, at the
// surface, before it leaves one behind -- "roughly 0.5 blocks/s" [proposal, task spec]. Below
// this a body is treated as drifting or holding station, not swimming, and gets no wake.
#define WAKE_SPEED_THRESHOLD  0.5f

// Seconds between wake emissions while the condition above holds -- "about 2 particles every
// 0.3 s" [proposal, task spec]. This is the whole of the rate limit: particles.c's pool has no
// per-frame cap of its own (see docs/plan-particles.md and this lane's own final report for
// why that is a deliberate refusal, not an oversight), so this constant is the only thing
// standing between "a swimmer" and "a firehose" for this one emitter.
#define WAKE_INTERVAL  0.3f

// v1.8.17 WATER-FX. See player.h's own comment for the contract. Bounded search, generously
// past world/physics.c's trySwimUp (which stops at 3 cells and calls that "all the slack a
// legitimate float ever needs"): this is cosmetic placement, not a gate on a player action, so
// a deep or fast dive that lands more than 3 cells under a distant surface should still find
// it rather than silently fall back to underwater -- which is the exact bug this function
// exists to fix. 8 is REASONED, not measured: PLAYER_TERMINAL is -60 blocks/s and playerUpdate
// clamps dt to MAX_TICK (0.05s), so even a body falling at its dry terminal velocity crosses at
// most 3 blocks in the one frame that detects the transition -- 8 is comfortably past that with
// room for the body's own height (PLAYER_HEIGHT 1.8) on top.
//
// worldGet and blockInfo, not a private reimplementation of trySwimUp's loop: both are public
// API (world/world.h, world/block.h) already reachable from this file's existing includes, so
// this is the same search physics.c performs, called by name, not a second copy of it that
// could silently drift from the original.
static float waterSurfaceY(const World* w, const Body* b)
{
	const int bx = (int)floorf(b->x);
	const int bz = (int)floorf(b->z);
	int cy = (int)floorf(b->y);

	int steps = 0;
	while (steps < 8 && blockInfo(worldGet(w, bx, cy, bz))->liquid) {
		cy++;
		steps++;
	}

	// steps == 0: the feet cell was never liquid, so b->y is already the answer -- the shape
	// every exit-splash call is in (see player.h). steps > 0: cy is the floor of the first
	// non-liquid cell found (or, if the 8-cell bound ran out first, the highest cell searched)
	// -- either way strictly closer to the true surface than the feet were.
	return (steps > 0) ? (float)cy : b->y;
}

void playerInit(Player* p, float x, float y, float z, float yaw, float pitch)
{
	bodyInit(&p->body, x, y, z);

	p->bob_phase = 0.0f;
	p->bob_env   = 0.0f;
	p->wake_timer = 0.0f;

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
	//
	// 2026-09-03: this asked for sinf(yaw) and cosf(yaw) TWICE each — fx and rz are the same
	// sine, and rx is the same cosine fz negates. Four transcendental calls where two do, and
	// on an ARM11 with no fast libm those are not free. Folded to one of each; the negation is
	// exact in IEEE, so the four values are bit-identical to what they were, not merely close.
	// Small in absolute terms (this runs once a frame, not the ~5,800 times an atan2f removal
	// once did), but it costs nothing and removes a duplicate that reads like an oversight.
	const float s  = sinf(p->cam.yaw);
	const float c  = cosf(p->cam.yaw);
	const float fx = s;
	const float fz = -c;
	const float rx = c;
	const float rz = s;

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
	//
	// v1.8.17 WATER-FX task 1 -- the spawn-position fix. This used to read p->body.y
	// directly, which is the FEET (physics.h's Body: "centre of the box in x/z, FEET in y"),
	// not the surface: the instant the body is BODY_SURFACE or deeper the feet are already
	// below the waterline, so the splash spawned underwater instead of at the plane it was
	// supposed to mark. waterSurfaceY walks up from the feet to find that plane -- see its
	// own comment above.
	if (prev_wet == BODY_DRY && wet != BODY_DRY)
		particlesSpawnSplash(p->body.x, waterSurfaceY(w, &p->body), p->body.z, p->body.vy);

	// v1.8.17 WATER-FX task 2 -- the exit splash. The exact mirror of the entry check above:
	// prev_wet is last frame's (stale-by-one-move) answer and `wet` is this frame's fresh one
	// at the body's current, already-moved-by-last-frame's-bodyStep position (see the comment
	// on prev_wet's declaration two lines up) -- so this fires exactly once, on the frame the
	// body's eye clears the surface from BODY_SURFACE into BODY_DRY, not on a submerged body
	// simply drifting back up into BODY_SURFACE (that is still wet, not an exit) and not once
	// per frame thereafter (wet stays BODY_DRY, so the edge does not re-fire).
	//
	// Specifically BODY_SURFACE -> BODY_DRY, per the task, not BODY_SUBMERGED -> BODY_DRY:
	// physics.c's trySwimUp/tryStepUp only ever lift a body OUT through BODY_SURFACE (a diver
	// cannot climb a bank from underwater, see trySwimUp's own comment), so a direct
	// SUBMERGED -> DRY transition cannot happen by climbing -- the only way this project's
	// physics produces one is teleporting or being placed, neither of which is a splash.
	//
	// particlesSpawnSplash, not a second emitter: matching the entry splash's character is
	// the whole ask (steve's task 2), and the function's own count/speed scaling already
	// degrades gracefully for a gentle climb-out (p->body.vy is small or zero the moment a
	// climb has just planted the body on the bank, which lands the particle count at or near
	// its 8-particle floor -- a quieter splash for a quieter exit, "for free", the same way
	// particlesSpawnSplash's own header comment describes impact-speed scaling on entry).
	// waterSurfaceY here is close to a no-op (the feet are already at or above the plane the
	// instant this fires -- see its own comment), which is exactly right: this is not a
	// second search, it is the same one function agreeing with itself at a different moment.
	if (prev_wet == BODY_SURFACE && wet == BODY_DRY)
		particlesSpawnSplash(p->body.x, waterSurfaceY(w, &p->body), p->body.z, p->body.vy);

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

	// v1.8.17 WATER-FX task 3 -- the swim wake. p->body.wet and p->body.vx/vz, not the `wet`
	// read at the top of this function or the ix/iz written to body.vx/vz before bodyStep --
	// the same reasoning bodySurfaceBob below already applies to p->body.wet: bodyStep has
	// just re-answered the wet question, and re-resolved the actual horizontal velocity
	// (resolveX/resolveZ zero it on a blocked axis), at the position this frame's move
	// actually left the body at. A wake keyed off the pre-step answer could fire one frame
	// into a wall the body never actually reached.
	//
	// The interval is a plain accumulator that keeps its remainder on firing (-= WAKE_INTERVAL,
	// not = 0): a frame-time hiccup that overshoots the threshold must not cost the next
	// interval part of its own budget, the identical reasoning world/tick.c's own tick clock
	// gives for banking a remainder instead of truncating it.
	const float speed_h = sqrtf(p->body.vx * p->body.vx + p->body.vz * p->body.vz);
	if (p->body.wet == BODY_SURFACE && speed_h > WAKE_SPEED_THRESHOLD) {
		p->wake_timer += dt;
		if (p->wake_timer >= WAKE_INTERVAL) {
			particlesSpawnWake(p->body.x, waterSurfaceY(w, &p->body), p->body.z,
			                    p->body.vx, p->body.vz);
			p->wake_timer -= WAKE_INTERVAL;
		}
	} else {
		// Leaving the surface or slowing down cancels a partial interval outright rather
		// than letting it keep counting: resuming later should feel like a fresh 0.3s wait,
		// not fire on the very next qualifying frame off banked time from a swim that ended.
		p->wake_timer = 0.0f;
	}

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
