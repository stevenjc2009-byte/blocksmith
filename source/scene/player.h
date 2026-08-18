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
#define PLAYER_KEY_JUMP  KEY_A

typedef struct {
	Body   body;   // feet position, in blocks
	Camera cam;    // position derived from the body; yaw and pitch owned here
} Player;

// Places the feet at (x,y,z) and points the camera at the given yaw and pitch.
void playerInit(Player* p, float x, float y, float z, float yaw, float pitch);

// Reads input, steps the physics and moves the camera to the new eye position.
// `dt_ms` is the real frame time.
void playerUpdate(Player* p, const World* w, float dt_ms);
