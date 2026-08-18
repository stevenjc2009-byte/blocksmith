// Player physics: an axis-aligned box swept against the voxel grid.
//
// Three ideas carry the whole module, and each exists because the naive version
// of it is a well-known trap:
//
//   * Axes are resolved one at a time -- Y, then X, then Z -- never all three
//     against the world in one test. Resolving them together lets a body that is
//     blocked on X but clear on Y+Z read as "fully blocked" and stick to walls,
//     or worse, slide diagonally through the seam between two blocks that neither
//     axis alone would let it pass.
//   * A move is subdivided into substeps no larger than a safe fraction of a
//     block before it is resolved. Testing only the start and end of a big move
//     can hop clean over a one-block-thick wall; testing every substep cannot.
//   * A blocked substep snaps to the exact grid boundary analytically rather than
//     by searching for it, because the subdivision above guarantees the box can
//     only ever newly touch one fresh layer of cells per substep -- see the
//     comment above resolveY() in physics.c for why that makes the boundary a
//     direct calculation instead of a bisection.
#pragma once

#include "world/world.h"

// Player box, in blocks. 0.6 wide keeps the player able to walk a one-block gap;
// 1.8 tall with a 1.62 eye height matches the proportions the genre trained people on.
#define PLAYER_WIDTH   0.6f
#define PLAYER_HEIGHT  1.8f
#define PLAYER_EYE     1.62f

// Blocks per second, and blocks per second squared.
#define PLAYER_GRAVITY     -28.0f
#define PLAYER_JUMP_SPEED   8.5f
#define PLAYER_WALK_SPEED   4.3f
#define PLAYER_TERMINAL    -60.0f   // clamp, so a long fall cannot tunnel

// There is deliberately no step-height constant. Auto-step is a flat one-block rise
// gated by a headroom probe, because every block in this game is a full 1.0 cube and
// there is nothing shorter to measure a sub-block threshold against. See tryStepUp in
// physics.c — if the registry ever grows a slab or a stair, that is where a real
// height comparison would go.

// Which axes a move was blocked on.
#define BLOCKED_X  (1 << 0)
#define BLOCKED_Y  (1 << 1)
#define BLOCKED_Z  (1 << 2)

typedef struct {
	float x, y, z;      // centre of the box in x/z, FEET in y
	float vx, vy, vz;
	bool  on_ground;
} Body;

void bodyInit(Body* b, float x, float y, float z);

// Moves the body by the given delta, resolving collisions one axis at a time and
// sliding along whatever it hits. Returns a BLOCKED_* mask. Sets on_ground when a
// downward move was stopped.
int bodyMove(Body* b, const World* w, float dx, float dy, float dz);

// One simulation tick: applies gravity to vy, then moves by velocity * dt.
// `dt_s` is seconds. Call with the desired horizontal velocity already in vx/vz.
int bodyStep(Body* b, const World* w, float dt_s);

// True if the body would overlap a solid block at the given position.
bool bodyBlocked(const World* w, float x, float y, float z);
