// The player: a physics body with a camera sitting at eye height on top of it.
//
// This exists to keep the split honest. world/physics.c knows about boxes and blocks
// and is tested on the PC without a console anywhere near it; scene/camera.c knows
// about the view matrix; neither of them should know about the circle pad. Reading
// input and deciding that A means jump is this file's whole job.
#pragma once

#include <3ds.h>

#include "scene/camera.h"
#include "world/physics.h"
#include "world/world.h"

// Jump. The camera already owns the D-pad (move), START (exit) and, in fly mode, A and
// L/R; walking mode has no boost, so A is free for the thing it is used for everywhere
// else in the genre.
//
// Since step 8.4 this is the *default*, not the binding: playerUpdate reads
// inputKey(ACTION_JUMP) (app/input_map.h), and app/options.c's s_action_defaults is what
// makes that answer KEY_A on a console with no options.ini. Kept here because this is where
// the reasoning for the choice lives, and app/options.h cites this line by name as its source
// for the default — a header that cannot include <3ds.h> and so cannot state KEY_A itself.
#define PLAYER_KEY_JUMP  KEY_A

typedef struct {
	Body   body;   // feet position, in blocks
	Camera cam;    // position derived from the body; yaw and pitch owned here

	// v1.8.3 — the surface swell's two floats, owned here rather than by Body so that a
	// view effect can never be mistaken for physics state. Both are advanced by, and only
	// meaningful to, world/physics.c's bodySurfaceBob; see the PLAYER_SURFACE_BOB_*
	// constants for what they are and why the effect is not in the body.
	float  bob_phase;
	float  bob_env;

	// v1.8.17 WATER-FX task 3 -- the swim wake's own rate-limit accumulator, owned here for
	// the identical reason bob_phase/bob_env are: it is a COSMETIC effect's timing state, not
	// physics, so it does not belong on Body. Advanced and consumed entirely inside
	// scene/player.c's playerUpdate; see WAKE_INTERVAL there for the rate it enforces.
	float  wake_timer;
} Player;

// Places the feet at (x,y,z) and points the camera at the given yaw and pitch.
void playerInit(Player* p, float x, float y, float z, float yaw, float pitch);

// Reads input, steps the physics and moves the camera to the new eye position.
// `dt_ms` is the real frame time.
void playerUpdate(Player* p, const World* w, float dt_ms);
