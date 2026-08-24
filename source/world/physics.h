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

// v1.8.0 task 25 — buoyancy and swimming. Water is registered LIQUID and deliberately
// NOT SOLID (world/registry.c), so collision never sees it and none of the code below
// touches bodyBlocked/resolveY/resolveX/resolveZ: a swimmer is a body falling through a
// cell that happens to be blue, with different numbers on the fall.
//
// Four constants, and they are the whole feature:
//
//   WATER_GRAVITY   buoyancy is modelled as a much weaker downward pull rather than as a
//                   separate upward force. Same arithmetic, one branch, and it cannot
//                   push a body out of a lake it is resting at the bottom of.
//   WATER_TERMINAL  the clamp is what makes entering water FEEL like water: a body that
//                   hits the surface at -40 blocks/s is snapped to -1.5 on the first
//                   submerged tick, which is the "plunge, then drift" a player expects.
//   SWIM_UP_SPEED   held, not tapped, and not gated on on_ground — a body floating in
//                   mid-water has nothing under it, which is exactly the mistake a
//                   copy of the land jump would make. Slightly above the sink rate so
//                   holding it wins.
//   WATER_SPEED_MUL horizontal drag, applied to PLAYER_WALK_SPEED by bodyWalkSpeed().
//
// Deliberately NOT here, because the roadmap line reads "Buoyancy and swimming" and
// nothing else: breath/oxygen, drowning damage, an underwater screen tint. registry.c's
// own comment lists those as things water does not do; none of them is being half-built.
#define PLAYER_WATER_GRAVITY    -8.0f
#define PLAYER_WATER_TERMINAL   -1.5f
#define PLAYER_SWIM_UP_SPEED     3.0f
#define PLAYER_WATER_SPEED_MUL   0.5f

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
//
// Water is asked about HERE rather than passed in by the caller, deliberately. There are
// two bodyStep call sites outside the player (main.c's walk-stress probe, and this file's
// own tests), and a flag every caller has to remember to compute is a flag one of them
// will not — the exact shape of defect this project has already shipped twice from a
// decision living in a file with no test. A body in water behaves like a body in water no
// matter who steps it. The cost is two worldGet() calls per tick against the up-to-12 that
// one bodyBlocked() already makes, several times, inside the same call.
//
// 12, not the 27 this comment claimed until v1.7.1: the box is PLAYER_WIDTH (0.6) wide, so
// it straddles at most two cells in x and two in z, and PLAYER_HEIGHT (1.8) tall, so at most
// three in y — 2 x 2 x 3, as the loop bounds in physics.c's bodyBlocked spell out. 27 would
// need a box more than a block across on every axis, which would also mean the player could
// not fit through a one-block gap.
int bodyStep(Body* b, const World* w, float dt_s);

// True if the body would overlap a solid block at the given position.
bool bodyBlocked(const World* w, float x, float y, float z);

// True if the body is in water: either the feet cell or the eye cell holds a liquid.
//
// Both are checked, not just the feet, because a body falling through a waterfall can have
// its head in water with air under its feet, and one that has just jumped out of a lake has
// the opposite. Either counts as "in water" — this answers the swim question, not a
// drowning question, and there is no drowning.
bool bodySubmerged(const World* w, const Body* b);

// Horizontal speed for one tick, given the answer bodySubmerged() just gave. A function
// rather than a macro at the call site so the rule is linkable and testable: player.c
// includes <3ds.h> and cannot be built by the host suite, so any decision left there is a
// decision nothing checks.
float bodyWalkSpeed(bool submerged);

// The vertical half of the movement input for one tick. On land this is the ground-gated
// single-impulse jump, unchanged from before task 25 (a HELD button must not fly). In
// water it is a sustained rise while the button is held, with no ground test at all.
//
// `jump_pressed` is the this-frame edge (hidKeysDown), `jump_held` the level (hidKeysHeld).
void bodyJump(Body* b, bool submerged, bool jump_held, bool jump_pressed);
