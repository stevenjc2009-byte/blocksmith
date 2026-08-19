// Breaking and placing blocks: the bridge between where the camera is pointing and what
// the world and the mesh pool are told about it.
//
// This lives in scene/ rather than world/ because it is the one piece of the edit path
// that needs the camera and the mesh pool. The geometry it rests on (world/raycast.c) and
// the storage it writes to (world/world.c) are both host-tested on the PC; what is left
// here is button wiring and the ordering of "write the block, then tell the renderer",
// which is small on purpose.
#pragma once

#include <3ds.h>

#include "scene/camera.h"
#include "world/physics.h"
#include "world/raycast.h"
#include "world/world.h"

// How far the player can reach, in blocks. Five is the distance the genre trained people
// on and it is also about as far as the 400x240 top screen can show a highlight cage that
// still reads as sitting on one particular block rather than somewhere over there.
#define INTERACT_REACH  5.0f

typedef struct {
	RayHit  target;      // what the camera is looking at, recomputed every frame
	BlockId holding;      // what a place would put down
	int     broke;        // cumulative, for the overlay — a placed block that silently
	int     placed;        // did not appear is otherwise very hard to notice
	int     refused;      // edits the world, the player's own body, or a miss rejected
	u32     prev_keys;    // keys_down seen on the previous interactEdit call, so a held
	                       // button can only fire once per press regardless of what the
	                       // caller passes in — see interactEdit

	// What THIS interactEdit call broke and placed, or BLOCK_AIR for neither. Step 8.2's
	// inventory is filled from the first and charged for the second, and it needs the block
	// *id*, which the cumulative counters above cannot carry. Both are cleared at the top of
	// every interactEdit — they report one call, they do not accumulate — so a caller that
	// reads them once per frame after the call sees each edit exactly once. Only one edit of
	// each kind can happen per call, because the press that triggers it is edge-detected.
	BlockId broke_id;
	BlockId placed_id;
} Interact;

void interactInit(Interact* it);

// Recomputes `target` from the camera's position and facing. Call once per frame, before
// drawing, so the highlight and any edit in the same frame agree on what was aimed at.
void interactAim(Interact* it, const World* w, const Camera* cam);

// Applies this frame's presses to the world: INTERACT_KEY_BREAK sets the targeted block
// to air, INTERACT_KEY_PLACE puts `holding` in the empty cell the ray entered from.
// Returns the number of chunks newly queued for a remesh, so a caller can see that an
// edit reached the renderer at all.
//
// `body` is the player, and a placement that would put a block inside it is refused
// rather than performed: standing in a wall is worse than a press that did nothing.
// `body` is required — passing NULL would silently disable that check, so there is no
// bypass for it here; every caller must supply the real player body.
//
// `keys_down` may be either hidKeysDown() or hidKeysHeld(): interactEdit tracks which
// bits were already set on the previous call internally (see Interact.prev_keys) and
// acts only on newly-set bits, so a held button still fires once per press either way.
int interactEdit(Interact* it, World* w, const Body* body, u32 keys_down);

// The buttons, in one place so the handoff and the code cannot disagree. The camera
// already owns A (boost), the D-pad (move), L and R (up/down) and START (exit), which
// leaves the two right-hand face buttons free.
#define INTERACT_KEY_BREAK  KEY_X
#define INTERACT_KEY_PLACE  KEY_Y
