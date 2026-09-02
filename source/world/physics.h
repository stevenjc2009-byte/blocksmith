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

// v1.8.2 — the surface. The four constants above describe a body INSIDE a water column
// and describe it well; what they had no answer for was the waterline itself, where
// SWIM_UP_SPEED's hard clamp, the -8/-28 gravity step and a submersion test with no middle
// state formed a three-line bang-bang relay. Measured before this change: holding the swim
// button at the top of a lake made the body cross vy=0 fifty-nine times in the last 399 of
// 600 frames -- a 4.4 Hz judder through 0.223 blocks. Nothing was wrong with any one of the
// three lines; the loop was in how they met.
//
//   SURFACE_RISE    the held-jump target for vy once the head is out of the water, as
//                   opposed to SWIM_UP_SPEED which still governs the climb through the
//                   column. 0.0 means the button holds you AT the surface rather than
//                   throwing you off it, which is the whole ask.
//   SWIM_RESPONSE   1/s. How fast vy is pulled TOWARDS its target instead of being
//                   assigned it. The old code set vy every frame from whatever it was,
//                   including straight through zero from a negative value, which is the
//                   velocity reversal that made the bob feel like a judder rather than a
//                   float. ~100 ms to close the gap.
//   WATER_VDRAG     1/s. Vertical drag on an UNDRIVEN upward coast, which water had none
//                   of: gravity and the terminal clamp alone let a body that stopped being
//                   pushed keep every bit of its velocity and arc ballistically.
//   WET_HYSTERESIS  blocks. The band the eye must clear ABOVE the waterline before a
//                   submerged body stops counting as submerged. See bodyWetUpdate.
//
// The first cut of this (v1.8.2 attempt 1) got the target right and the FORCE BALANCE
// wrong, and the difference is the whole of attempt 2. Pulling vy towards 0 does not hold
// a body still, because a velocity target cannot cancel a constant acceleration: gravity
// re-establishes about -0.5 blocks/s of droop every frame, the body sinks out of
// BODY_SURFACE, the state flips to BODY_SUBMERGED, the +3 target throws it back, and the
// relay simply moved from vy=0 to the state boundary. Measured: travel fell from 0.223333
// blocks to 0.022027, which reads like a win, but the eye crossed the waterline 140 times
// in 399 frames where the old code crossed it 0 -- a 10.5 Hz strobe of the water plane
// through the camera, which is worse than the bob it replaced.
//
// So the rule here is a force rule, not a velocity rule: WHILE THE BUTTON IS ACTIVELY
// PULLING, gravity is not applied at all (bodyStep, via Body::swim_drive). vy = target is
// then a genuine fixed point rather than a point the body falls away from -- nothing
// restores the error, so there is no limit cycle to damp. The two consequences are both
// wanted:
//   * at the surface the body stops dead and stays stopped, and
//   * a driven climb settles at exactly SWIM_UP_SPEED again, undoing attempt 1's
//     unasked-for 2.866667 -> 1.215686 blocks/s regression.
// Drag is likewise confined to the undriven coast, which is what it was for; attempt 1
// applied it to the driven climb and to the sink as well, dragging the free-sink terminal
// off PLAYER_WATER_TERMINAL to -1.328954. Both are back.
//
// Both rates are applied through dampAlpha() in physics.c rather than as `k * dt`, so that
// no tick length can overshoot the target -- the body runs on the RENDER frame
// (scene/player.c passes metricsFrameMs()), not on the 20 TPS tick, so dt here is whatever
// the frame took. See dampAlpha for what that does and does not buy.
#define PLAYER_SURFACE_RISE      0.0f
#define PLAYER_SWIM_RESPONSE    10.0f
#define PLAYER_WATER_VDRAG       6.0f
#define PLAYER_WET_HYSTERESIS    0.25f

// v1.8.3 — the surface swell, and it is CAMERA ONLY.
//
// Reported as: "I said to remove bobbing up and down on top of the water, but I want you to
// add it back but way calmer, similar to how actual Minecraft does it."
//
// Two separate things were asked for in that sentence and they are answered in two separate
// places. The FUNCTION of the bob — being able to get out of the water — is not a bob at
// all, it is a climb, and it lives in physics.c's trySwimUp. What is left is the LOOK of
// floating, and that is this.
//
// The bob v1.8.2 removed was a physics limit cycle: 0.223 blocks at 4.4 Hz, the BODY
// oscillating because three individually correct rules met badly, and its worst symptom was
// the water plane strobing through the camera. Putting a bob back into the body would put
// that whole failure mode back with it. So this one is not in the body: it is an offset
// added to the eye after the physics has finished. It cannot move the box, cannot affect
// collision, cannot creep, and cannot form a loop with anything, because nothing reads it.
//
//   SURFACE_BOB       blocks, peak offset — 5.5 cm up and the same down. The resting float
//                     leaves the eye 0.485 blocks clear of the water, so the trough still
//                     has nearly nine times the amplitude in hand and the swell can never
//                     dip the camera under the surface. That margin is the point: dipping
//                     it is exactly the strobe this file spent v1.8.2 removing.
//   SURFACE_BOB_RATE  cycles per second. 0.35 Hz is one slow swell every 2.9 seconds,
//                     against 4.4 Hz for the bob that was taken out. "Way calmer" is a
//                     twelfth of the frequency at a quarter of the amplitude.
//   SURFACE_BOB_FADE  1/s. How fast the swell arrives on entering the water and leaves on
//                     climbing out, so neither transition snaps the view.
#define PLAYER_SURFACE_BOB       0.055f
#define PLAYER_SURFACE_BOB_RATE  0.35f
#define PLAYER_SURFACE_BOB_FADE  3.0f

// v1.8.4. How long a fresh press of the jump button keeps the climb-onto-the-bank armed,
// in seconds. 0.25 is fifteen frames at 60 fps.
//
// A window rather than a single frame, because the press and the blocked horizontal move
// have to coincide for trySwimUp to be reached at all, and a player who taps A as he
// arrives rather than after he arrives would otherwise get nothing and read it as the
// game ignoring him. A window rather than a level (button held) because A is BOTH the
// swim input and the jump button: a body floating at the surface is already holding it,
// so a level gate is true exactly when the climb is reachable and gates nothing. That is
// not a guess -- it was measured: with the level gate in place, deleting the gate line
// altogether left the whole suite green.
//
// Short enough that it cannot be mistaken for auto-climb: at PLAYER_WALK_SPEED x
// PLAYER_WATER_SPEED_MUL the body covers well under a block inside the window, so an
// exit only happens at a bank the player was already pressed against when he pressed.
#define PLAYER_SWIM_EXIT_WINDOW  0.25f

// There is deliberately no step-height constant. Auto-step is a flat one-block rise
// gated by a headroom probe, because every block in this game is a full 1.0 cube and
// there is nothing shorter to measure a sub-block threshold against. See tryStepUp in
// physics.c — if the registry ever grows a slab or a stair, that is where a real
// height comparison would go.

// Which axes a move was blocked on.
#define BLOCKED_X  (1 << 0)
#define BLOCKED_Y  (1 << 1)
#define BLOCKED_Z  (1 << 2)

// How wet the body is. The two cells this is read from are the same two the old boolean
// bodySubmerged() already probed — feet and eye — so this is a wider ANSWER, not a more
// expensive question.
//
// Both are checked, not just the feet, because a body falling through a waterfall can have
// its head in water with air under its feet, and one that has just jumped out of a lake has
// the opposite. Either counts as "in water" — this answers the swim question, not a
// drowning question, and there is no drowning.
//
// BODY_SURFACE exists because the old boolean had to answer the same thing for a diver
// twenty blocks down and for a swimmer with his head in the air, and those two want
// opposite vertical inputs: the diver wants to be lifted, the swimmer wants to be held
// still. One bit could not say that, so the relay between the two answers was the bob.
//
// Declared above Body rather than next to bodyWetState() because Body now CARRIES one:
// the boundary between BODY_SURFACE and BODY_SUBMERGED is hysteretic, and hysteresis is
// by definition a function of where you came from. See bodyWetUpdate().
typedef enum {
	BODY_DRY       = 0,   // neither cell is liquid
	BODY_SURFACE   = 1,   // feet in liquid, eyes in air — floating at the waterline
	BODY_SUBMERGED = 2    // eye cell is liquid — under, whatever the feet are in
} BodyWet;

typedef struct {
	float x, y, z;      // centre of the box in x/z, FEET in y
	float vx, vy, vz;
	bool  on_ground;

	// v1.8.2 attempt 2. Two frames' worth of memory, both owned by physics.c and both
	// meaningless to read from outside it.
	//
	// `wet` is last frame's answer, and it is what makes the wet test hysteretic instead
	// of a bare threshold that anything sitting on it can chatter across.
	//
	// `swim_drive` is set by bodyJump when the water branch actually pulled this frame,
	// and consumed (and cleared) by bodyStep. It is state rather than a bodyStep argument
	// because bodyStep has three call sites — the player, main.c's walk-stress probe and
	// the tests — and only one of them knows anything about a jump button; the comment on
	// bodyStep below is about exactly that mistake. A body nobody calls bodyJump for is
	// simply never driven, which is the right answer for a walk probe.
	// v1.8.4. Seconds left on an armed climb-out. Set to PLAYER_SWIM_EXIT_WINDOW by
	// bodyJump on a fresh press of the jump button while in water, counted down by
	// bodyStep, and required (and consumed) by trySwimUp before it will lift a floating
	// body onto a bank.
	//
	// Before v1.8.4, walking into the shore climbed you out on its own with no input at
	// all. steve asked for Minecraft's rule instead: you stay in the water, bobbing
	// against the bank, until you press jump.
	//
	// An edge with a window, NOT the button level -- see PLAYER_SWIM_EXIT_WINDOW for the
	// measurement that rules the level out. A body nobody calls bodyJump for is never
	// armed and so is never lifted, which is the right answer for main.c's walk-stress
	// probe and for every test that steps a body without an input.
	float   swim_exit_t;
	BodyWet wet;
	bool    swim_drive;

	// v1.8.8 entity foundation. The collision box, in blocks, carried by the body instead
	// of being the compile-time constants above.
	//
	// This is ADDITIVE and it is deliberately additive. Before this, bodyBlocked/resolveY/
	// resolveX/resolveZ/wetAt read PLAYER_WIDTH, PLAYER_HEIGHT and PLAYER_EYE directly, so
	// bodyMove could only ever sweep the player's box. An entity subsystem that wanted its
	// own box had exactly two options: parameterise these, or write a second collision
	// routine. A second routine is the worse one by a long way — it drifts from this one,
	// and the resulting bugs appear for mobs but not for the player (or the reverse), which
	// is the hardest shape of bug to see.
	//
	// bodyInit() sets all three to the player's values, so EVERY existing caller — the
	// player, main.c's walk-stress probe, world_test.c's 68 call sites — is unchanged and
	// none of them had to be touched. bodyBlocked() likewise keeps its four-argument
	// signature and its player box; bodyBlockedBox() is the widened one.
	//
	// `eye` is a separate field rather than a fraction of `height` on purpose. The obvious
	// economy — eye = height * (PLAYER_EYE / PLAYER_HEIGHT), a ratio that is exactly 0.9 in
	// decimal — is not exactly 0.9 in float32, so it would move the player's eye probe by
	// up to an ulp and the "player is bit-identical" claim would stop being true. Four bytes
	// is cheaper than an unprovable claim.
	//
	// half_w, not width, because every use site was already `PLAYER_WIDTH * 0.5f` and a
	// stored half-extent removes a multiply from the inner loop as well as a chance to
	// forget it.
	float half_w;   // half the box's x/z extent
	float height;   // full box height, feet to head
	float eye;      // height the wet probe reads the "eye" cell at, above the feet
} Body;

// Initialises at (x, y, z), at rest, dry, with the PLAYER box.
void bodyInit(Body* b, float x, float y, float z);

// Overrides the box on an already-initialised body. Call AFTER bodyInit.
//
// `width` is the full x/z extent (it is halved in here) so the argument reads the same way
// PLAYER_WIDTH does; `eye_frac` is the eye height as a fraction of `height`, because an
// entity's wet probe has no absolute number of its own to quote. Non-positive arguments are
// ignored rather than stored: a zero-width box would collide with nothing and a zero-height
// one would read the same cell twice, and both are the kind of value that arrives from an
// uninitialised struct rather than from a deliberate choice.
void bodySetBox(Body* b, float width, float height, float eye_frac);

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

// True if the body would overlap a solid block at the given position, testing the PLAYER
// box. Signature and meaning unchanged since before v1.8.8 — world_test.c and scene/ both
// call it and neither had to move.
bool bodyBlocked(const World* w, float x, float y, float z);

// The same test for an arbitrary box. bodyBlocked(w, x, y, z) is exactly
// bodyBlockedBox(w, x, y, z, PLAYER_WIDTH * 0.5f, PLAYER_HEIGHT) and is implemented as
// that call, so there is one sweep in this file and not two.
bool bodyBlockedBox(const World* w, float x, float y, float z, float half_w, float height);

// The INSTANTANEOUS wet state: what the two cells say right now, with no memory and no
// band. This is the honest reading of the world, and it is what bodySubmerged() and the
// tests' "is this body in water" assertions want. It is NOT what the vertical input is
// driven from — see bodyWetUpdate.
BodyWet bodyWetState(const World* w, const Body* b);

// The DRIVEN wet state: the same reading, with PLAYER_WET_HYSTERESIS applied to the
// submerged boundary, stored into b->wet and returned. Call it once per frame, before
// bodyJump; bodyStep calls it too, and the two agree because the update is idempotent at a
// fixed position.
//
// The band is one-sided and sits ABOVE the waterline: the eye entering the water submerges
// you immediately, but leaving takes the eye a further PLAYER_WET_HYSTERESIS clear of the
// surface. That asymmetry is deliberate and is what places the resting float. Centring the
// band on the waterline instead would make a body with its eyes 0.1 blocks UNDER the water
// read as BODY_SURFACE, whose target is 0 — it would hover there, head under, doing
// nothing, which is the exact opposite of what the state is for. Biased this way the
// answer to "am I under?" is always yes when in doubt, so the only thing the band can do is
// hold you up.
//
// Two frames of hysteresis are two frames of lag on a boundary the player can also cross by
// walking off a ledge; that is accepted. The state is only ever read for the vertical
// input, and the wrong answer for one frame there is a fifth of a swim stroke.
BodyWet bodyWetUpdate(const World* w, Body* b);

// True if the body is in water at all. Kept as the exact predicate it always was so that
// bodyWalkSpeed(), the tests and player.c's horizontal branch do not have to care about the
// distinction that only the vertical input needs.
static inline bool bodySubmerged(const World* w, const Body* b)
{
	return bodyWetState(w, b) != BODY_DRY;
}

// Horizontal speed for one tick, given the answer bodySubmerged() just gave. A function
// rather than a macro at the call site so the rule is linkable and testable: player.c
// includes <3ds.h> and cannot be built by the host suite, so any decision left there is a
// decision nothing checks.
float bodyWalkSpeed(bool submerged);

// The vertical half of the movement input for one tick. On land this is the ground-gated
// single-impulse jump, unchanged from before task 25 (a HELD button must not fly). In
// water it is a sustained pull of vy TOWARDS a target while the button is held, with no
// ground test at all — the target being SWIM_UP_SPEED under the surface and SURFACE_RISE at
// it. A pull rather than an assignment, and a target that depends on the state, are two of
// the three halves of the v1.8.2 surface fix; the third is that a pull also sets
// Body::swim_drive, which tells bodyStep to leave gravity off for that frame. See the
// constants above for why a target alone was not enough.
//
// `wet` is the answer bodyWetUpdate() just gave, `jump_pressed` the this-frame edge
// (hidKeysDown), `jump_held` the level (hidKeysHeld), and `dt_s` the same frame time
// bodyStep is about to be handed — the water branch is rate-based, so it needs it.
void bodyJump(Body* b, BodyWet wet, bool jump_held, bool jump_pressed, float dt_s);

// The camera's vertical offset for one frame, in blocks — the surface swell described above
// the three PLAYER_SURFACE_BOB_* constants. Returns the offset and advances the two floats
// it is handed.
//
// The state is the CALLER's (scene/player.h's Player carries it), not Body's, and that is
// the whole design: a physics body must not carry a view effect, or something will
// eventually read it and the offset will stop being cosmetic. Nothing in world/ reads these
// two floats but this function.
//
// Kept in this file rather than in scene/player.c even though it is a view effect, for the
// reason stated on bodyWalkSpeed: player.c includes <3ds.h>, cannot be linked into the host
// suite, and a rule left there is a rule nothing checks.
float bodySurfaceBob(BodyWet wet, float dt_s, float* phase, float* envelope);
