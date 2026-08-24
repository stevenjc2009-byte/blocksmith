// Breaking and placing blocks: the bridge between where the camera is pointing and what
// the world and the mesh pool are told about it.
//
// This lives in scene/ rather than world/ because it is the one piece of the edit path
// that needs the camera and the mesh pool. The geometry it rests on (world/raycast.c) and
// the storage it writes to (world/world.c) are both host-tested on the PC; what is left
// here is button wiring and the ordering of "write the block, then tell the renderer",
// which is small on purpose.
//
// ── the __3DS__ split ───────────────────────────────────────────────────────────────────
//
// That ordering is exactly the thing that went wrong (v1.6.0: a break wrote air FIRST and
// only then asked whether the block could be carried, so a server-registered block was
// deleted from the world and refused by the bag — gone, with no message). An ordering bug
// is not provable by reading a diff, so the pure half of this module — the Interact state,
// interactInit and interactEdit — now sits above an `#ifdef __3DS__` guard and the host
// suite links the REAL scene/interact.c. The aiming half needs the camera (and so
// <citro3d.h>) and stays below the guard, along with the libctru key constants.
//
// Same arrangement as app/battery.c and app/debugmenu_ui.c: the directory says where the
// file belongs in the program, the guard says which half the host can actually prove.
#pragma once

#ifdef __3DS__
#include <3ds.h>
#else
// libctru's u32, and only that. The host build has no libctru, but interactEdit's key mask
// is part of the pure half and its type has to stay byte-identical to the console's.
#include <stdint.h>
typedef uint32_t u32;
#endif

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

// ── aiming and the button constants — console build only ────────────────────────────────
#ifdef __3DS__

#include "scene/camera.h"

// Recomputes `target` from the camera's position and facing. Call once per frame, before
// drawing, so the highlight and any edit in the same frame agree on what was aimed at.
void interactAim(Interact* it, const World* w, const Camera* cam);

// The buttons, in one place so the handoff and the code cannot disagree. The camera
// already owns A (boost), the D-pad (move), L and R (up/down) and START (exit), which
// leaves the two right-hand face buttons free.
#define INTERACT_KEY_BREAK  KEY_X
#define INTERACT_KEY_PLACE  KEY_Y

#endif  // __3DS__
