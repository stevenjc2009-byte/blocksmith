// A block-precise raycast: "what is the player looking at, and where would a
// placed block go".
//
// This is the one piece of the world code that genuinely needs floating point
// geometry rather than integer block arithmetic, so it lives in its own file
// rather than being bolted onto world.c.
#pragma once

#include "world/world.h"

// Returned in RayHit.face when the ray began inside a solid block: there is no
// entry face in that case, and pretending there is one places blocks inside walls.
#define RAY_FACE_NONE  (-1)

typedef struct {
	bool  hit;
	int   x, y, z;        // the solid block that was hit
	int   face;           // FACE_* (from world/block.h), or RAY_FACE_NONE
	int   px, py, pz;     // the empty cell the ray entered from: where a block goes
	float distance;       // blocks travelled to the surface, along a normalised dir
} RayHit;

RayHit worldRaycast(const World* w, float ox, float oy, float oz,
                    float dx, float dy, float dz, float max_distance);
