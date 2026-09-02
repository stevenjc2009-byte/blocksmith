// See physics.h for the three load-bearing ideas. This file is the arithmetic
// behind them.
//
// No <math.h>: source/world is kept free of it so the host test suite can link
// against plain gcc without pulling in libm behaviour that might differ from the
// ARM11's. floorf/fabsf are one-liners here instead.
#include "world/physics.h"

#include "world/block.h"

// A move is cut into substeps no bigger than this, in blocks, before it is
// resolved. It has to stay under 1.0 (a whole block) for the collision snap
// below to hold its "only one new layer of cells" invariant, and it has to stay
// well under that so a fast body still gets several tests against a thin wall
// rather than one lucky-or-unlucky one. 0.4 gives at least two tests per block
// crossed in the worst case.
#define MAX_SUBSTEP  0.4f

// Backoff used when a box edge sits exactly on a grid line. bodyBlocked treats a
// box as the half-open interval [min, max): the max edge must read as belonging
// to the cell *below* it, which floorToInt(max) alone would get wrong whenever
// max lands exactly on an integer (it would round into the next cell up). 1e-4
// is far bigger than float32 rounding error at the coordinate ranges a chunked
// world uses, and far smaller than anything a player could perceive or a wall
// could hide behind.
#define BOX_EPS  1e-4f

// floor(), without pulling in <math.h> for one instruction's worth of function.
// (int) truncates towards zero, which is wrong for negative non-integers; the
// correction below is the whole difference between this and plain truncation,
// and it is exactly the bug the world's own chunk-coordinate shift avoids for
// the same reason (see world.h).
static inline int floorToInt(float v)
{
	int i = (int)v;
	if ((float)i > v) i--;
	return i;
}

static inline float absf(float v)
{
	return v < 0.0f ? -v : v;
}

void bodyInit(Body* b, float x, float y, float z)
{
	b->x = x;
	b->y = y;
	b->z = z;
	b->vx = 0.0f;
	b->vy = 0.0f;
	b->vz = 0.0f;
	b->on_ground = false;

	// BODY_DRY rather than "whatever the world says here", because bodyInit does not take
	// a world and the hysteresis only ever biases TOWARDS submerged: a body initialised
	// inside water reads BODY_SUBMERGED on its very first bodyWetUpdate anyway, since the
	// band does not resist entering. Starting dry therefore costs nothing and needs no
	// world pointer.
	b->wet = BODY_DRY;
	b->swim_drive = false;
	b->swim_exit_t = 0.0f;

	// v1.8.8. The player box, so that a body nobody calls bodySetBox on behaves exactly as
	// every body did before this field existed. This is the line that makes the entity
	// parameterisation additive rather than a breaking change: the 68 call sites in
	// world_test.c and the one in scene/player.c all go through here.
	b->half_w = PLAYER_WIDTH * 0.5f;
	b->height = PLAYER_HEIGHT;
	b->eye    = PLAYER_EYE;
}

void bodySetBox(Body* b, float width, float height, float eye_frac)
{
	if (!b) return;
	if (width  > 0.0f) b->half_w = width * 0.5f;
	if (height > 0.0f) b->height = height;
	if (height > 0.0f && eye_frac > 0.0f) b->eye = height * eye_frac;
}

// blockIsSolid, and that is the whole of v1.6.0 task 13's collision story: nothing here
// changed and nothing here should. The task split "what shape is drawn" away from "what
// fills a cell" precisely so that collision could stay a question about `solid` alone —
// a cross-shaped plant is registered non-solid, so the box walks through it here for the
// same reason it walks through air, with no shape test in the hot triple loop below.
//
// Every solid block is still a full 1.0 cube (see tryStepUp), so the AABB test remains
// exact. A shape that is solid AND smaller than its cell — a slab, a stair — is the case
// that would need a real per-shape box here, and it does not exist yet.
bool bodyBlockedBox(const World* w, float x, float y, float z, float half, float height)
{
	// Min edges are inclusive (the box genuinely starts there), so a plain floor
	// is correct even when the edge sits exactly on a grid line. Max edges are
	// exclusive, which is what the -BOX_EPS is for.
	const int x0 = floorToInt(x - half);
	const int x1 = floorToInt(x + half - BOX_EPS);
	const int y0 = floorToInt(y);
	const int y1 = floorToInt(y + height - BOX_EPS);
	const int z0 = floorToInt(z - half);
	const int z1 = floorToInt(z + half - BOX_EPS);

	for (int by = y0; by <= y1; by++)
		for (int bz = z0; bz <= z1; bz++)
			for (int bx = x0; bx <= x1; bx++)
				if (blockIsSolid(worldGet(w, bx, by, bz)))
					return true;

	return false;
}

// The player-box call, kept as the four-argument function it has always been. Written as a
// forward to the widened one rather than as a second copy of the loop, so there is exactly
// one AABB sweep in this file: two copies is how the entity path and the player path start
// answering the same question differently.
bool bodyBlocked(const World* w, float x, float y, float z)
{
	return bodyBlockedBox(w, x, y, z, PLAYER_WIDTH * 0.5f, PLAYER_HEIGHT);
}

// Resolves vertical motion in isolation. This is where the "only one new layer"
// invariant matters most, because it is what turns "landed on a floor" into an
// exact answer instead of an approximate one: bodyMove never hands this a delta
// bigger than MAX_SUBSTEP, and the body is never blocked before the call (that is
// the invariant the whole module maintains), so if the moved box is blocked, the
// *only* cell that can have newly become solid is the single layer the leading
// face just entered -- every other cell in the box was already part of the box
// before the move and was already known clear. That means the offending cell's
// index can be read straight off the target position with floorToInt(), and the
// stopping point is exactly that cell's near face: no search, no residual gap,
// no risk of resting a hair's width above or below a surface.
// v1.8.8. "Is THIS body blocked there", as opposed to "is the player box blocked there".
// Every internal collision question in this file goes through it, so the box a body was
// given is the box every one of its own resolvers, step-ups and probes uses. A body that
// was never handed a box carries the player's from bodyInit, which is why nothing outside
// this file changed.
static inline bool bodyHits(const Body* b, const World* w, float x, float y, float z)
{
	return bodyBlockedBox(w, x, y, z, b->half_w, b->height);
}

static bool resolveY(Body* b, const World* w, float dy)
{
	if (dy == 0.0f) return false;

	if (dy > 0.0f) b->on_ground = false;   // moving up always leaves the ground

	b->y += dy;

	if (!bodyHits(b, w, b->x, b->y, b->z)) {
		if (dy < 0.0f) b->on_ground = false;   // falling clear, not resting on anything
		return false;
	}

	if (dy > 0.0f) {
		// Leading face is the head, which is bodyBlocked's "max" bound.
		const int cell = floorToInt(b->y + b->height - BOX_EPS);
		b->y = (float)cell - b->height;
	} else {
		// Leading face is the feet, which is bodyBlocked's "min" bound.
		const int cell = floorToInt(b->y);
		b->y = (float)(cell + 1);
		b->on_ground = true;
	}
	b->vy = 0.0f;
	return true;
}

static bool resolveX(Body* b, const World* w, float dx)
{
	if (dx == 0.0f) return false;

	const float half = b->half_w;
	b->x += dx;

	if (!bodyHits(b, w, b->x, b->y, b->z)) return false;

	if (dx > 0.0f) {
		const int cell = floorToInt(b->x + half - BOX_EPS);
		b->x = (float)cell - half;
	} else {
		const int cell = floorToInt(b->x - half);
		b->x = (float)(cell + 1) + half;
	}
	return true;
}

static bool resolveZ(Body* b, const World* w, float dz)
{
	if (dz == 0.0f) return false;

	const float half = b->half_w;
	b->z += dz;

	if (!bodyHits(b, w, b->x, b->y, b->z)) return false;

	if (dz > 0.0f) {
		const int cell = floorToInt(b->z + half - BOX_EPS);
		b->z = (float)cell - half;
	} else {
		const int cell = floorToInt(b->z - half);
		b->z = (float)(cell + 1) + half;
	}
	return true;
}

// Auto-step: when a horizontal move is blocked while grounded, tries planting the
// body exactly one block higher and re-testing the *whole* horizontal target from
// there, rather than the position resolveX/resolveZ already snapped to the wall.
//
// There is deliberately no step-height threshold to compare an obstruction against.
// Every block in this game is a full 1.0 tall cube, so there is nothing shorter than a
// whole block to measure, and lifting by any fraction of one would leave the box's feet
// embedded in the block it is trying to climb. The rise is therefore a flat 1.0.
//
// A two-block wall is refused structurally rather than numerically: the headroom probe
// below runs at y+1, a second stacked block still occupies that headroom, so
// bodyBlocked() at the stepped position comes back true and the step is refused. The
// caller enforces the other half of the rule -- at most one of these may succeed per
// substep -- because otherwise an asymmetric corner rises twice. See bodyMove.
//
// If the registry ever grows a sub-block shape (a slab or a stair) genuinely shorter
// than one block, this is where a real height comparison belongs: the rise would become
// "however tall the obstruction actually is, up to some threshold" instead of a flat 1.0,
// and that threshold is the constant to introduce at that point -- not before.
// There is no sneak state in the game yet. When it arrives, the line below is the one
// place it belongs: sneaking suppresses the step entirely, because a sneaking player is
// deliberately moving carefully and one that auto-climbed would be astonished. That is one
// more term on this same condition -- deliberately NOT built ahead of the state itself,
// which would be a guess about a field that does not exist.
//
// Swimming was the other half of that note and it has since arrived. It did NOT become a
// term on this condition: a swimmer does not step, he pulls himself up onto the waterline,
// which is a different distance and a different rule. See trySwimUp below.
static bool tryStepUp(Body* b, const World* w, float target_x, float target_z)
{
	if (!b->on_ground) return false;   // must never fire mid-air

	const float step_y = b->y + 1.0f;
	if (bodyHits(b, w, target_x, step_y, target_z)) return false;

	b->x = target_x;
	b->y = step_y;
	b->z = target_z;
	return true;
}

// v1.8.3 — the swimmer's climb, and the reason the comment above says swimming "replaces"
// the step rather than gating it.
//
// tryStepUp refuses outright unless b->on_ground, which is right for a walker and left a
// swimmer with no way out of a lake at all. A body floating at the waterline settles with
// its feet 1.135 blocks BELOW the shore -- the top water cell is under it, and the bank at
// sea level has its top face on the water plane -- so pressing into the bank simply zeroed
// vx, every frame, forever. Reported as "I am unable to leave the water".
//
// Deliberately NOT a flat one-block rise like the walker's. The gap between a floating
// body's feet and the waterline is not a whole number and is not even a constant: it falls
// out of PLAYER_WET_HYSTERESIS plus however far the body coasted on arrival, and it
// measures 11.865 at 60 fps against 11.894 at 30. A flat +1.0 from either of those lands
// INSIDE the bank and is refused by the headroom probe, so the fix would not have worked
// at any frame rate.
//
// So the target is found, not assumed: walk up from the feet to the first non-liquid cell
// and stand on its floor, which is the water plane by definition. That makes the rule the
// player's own -- "you can pull yourself onto anything level with the water" -- rather
// than a number, and it is self-limiting in the direction that matters, because a bank a
// block PROUD of the water still has its own block sitting in the way and bodyBlocked
// refuses the lift exactly as it refuses a walker's two-block step.
//
// The three-cell bound is what stops a body under a waterfall, or one swimming up a
// flooded shaft, from finding a surface far overhead and teleporting to it. Two cells is
// all a legitimate float ever needs; the third is slack.
static bool trySwimUp(Body* b, const World* w, float target_x, float target_z)
{
	// Feet in liquid, eyes in air. A submerged body must NOT get this -- otherwise a
	// diver pressed against a cliff would climb it a block at a time -- and a dry one
	// already has tryStepUp.
	if (b->wet != BODY_SURFACE) return false;

	// v1.8.4, and the whole of the behaviour change: the lift is an ANSWER TO AN INPUT,
	// not a consequence of bumping into geometry. Everything below this line is unchanged
	// -- the three-cell bound, the bodyBlocked headroom refusal, all of it -- because the
	// old code was right about HOW to leave the water and wrong only about WHEN.
	//
	// Note what this does NOT do: it does not make water a trap. tryStepUp is tried first
	// at both call sites in bodyMove, so a body whose feet have already reached dry land
	// walks up an ordinary one-block step exactly as it always did. This gate only covers
	// the case where the thing being climbed is the water itself.
	if (b->swim_exit_t <= 0.0f) return false;

	const int bx = floorToInt(b->x);
	const int bz = floorToInt(b->z);

	int cy   = floorToInt(b->y);
	int lift = 0;
	while (lift < 3 && blockInfo(worldGet(w, bx, cy, bz))->liquid) { cy++; lift++; }

	// lift == 0: the feet cell was not liquid after all, so there is nothing to climb out
	// of. lift == 3: the loop ran out of slack without finding air, which is a column of
	// water taller than any float, i.e. not a shoreline.
	if (lift == 0 || lift >= 3) return false;

	const float step_y = (float)cy;
	if (step_y <= b->y) return false;
	if (bodyHits(b, w, target_x, step_y, target_z)) return false;

	// Consumed on success, so one press buys one climb. Without this a single press at a
	// staircase of banks would carry the body up all of them inside the same window, which
	// is the auto-climb being removed wearing a fifteen-frame disguise.
	b->swim_exit_t = 0.0f;

	b->x = target_x;
	b->y = step_y;
	b->z = target_z;
	return true;
}

int bodyMove(Body* b, const World* w, float dx, float dy, float dz)
{
	// Subdivide so no substep can cross more than one block boundary on any
	// axis -- see MAX_SUBSTEP above for why that bound has to hold, both for
	// tunnelling and for the exact-snap invariant in resolveY/X/Z.
	float amax = absf(dx);
	if (absf(dy) > amax) amax = absf(dy);
	if (absf(dz) > amax) amax = absf(dz);

	int steps = 1;
	if (amax > MAX_SUBSTEP)
		steps = (int)(amax / MAX_SUBSTEP) + 1;

	const float sx = dx / (float)steps;
	const float sy = dy / (float)steps;
	const float sz = dz / (float)steps;

	int blocked = 0;

	for (int i = 0; i < steps; i++) {
		if (resolveY(b, w, sy)) blocked |= BLOCKED_Y;

		// X and Z each get their own step-up attempt, using the position from
		// just before that axis's own resolve -- not the position resolveX/Z
		// already snapped to the wall -- as the intended target. Velocity is
		// only zeroed once a substep is known to be genuinely blocked, i.e.
		// after a step-up attempt has had its chance to clear it; a step that
		// succeeds is a completed move, not a stop.
		// At most one block of rise per substep, no matter how many axes are blocked.
		// Without this, an asymmetric corner -- a one-block step on X, a two-block wall
		// on Z -- rises twice: X's step-up lifts the body a block, and Z is still
		// blocked from the new height, so Z's step-up lifts it again. Measured at
		// rise=2.0, which climbs a wall the headroom probe is supposed to refuse.
		bool stepped = false;

		const float pre_x = b->x;
		bool blocked_x = resolveX(b, w, sx);
		if (blocked_x && (tryStepUp(b, w, pre_x + sx, b->z)
		                  || trySwimUp(b, w, pre_x + sx, b->z))) {
			blocked_x = false;
			stepped   = true;
		}
		if (blocked_x) { b->vx = 0.0f; blocked |= BLOCKED_X; }

		const float pre_z = b->z;
		bool blocked_z = resolveZ(b, w, sz);
		if (blocked_z && !stepped && (tryStepUp(b, w, b->x, pre_z + sz)
		                              || trySwimUp(b, w, b->x, pre_z + sz)))
			blocked_z = false;
		if (blocked_z) { b->vz = 0.0f; blocked |= BLOCKED_Z; }
	}

	return blocked;
}

// v1.8.0 task 25. Only the CENTRE column is probed, not the whole box the way
// bodyBlocked() sweeps it, and that is deliberate rather than an economy: water is not
// solid, so there is no overlap to resolve — the question is "is the player in the water",
// and a body whose centre line is in a water cell is in the water. Probing the corners
// would make a body standing with 0.05 of its width over a shoreline cell count as
// swimming, which is the wrong answer and a more expensive way to get it.
// `eye_bias` shifts the height the EYE is probed at, and is the whole of the hysteresis:
// 0 for the honest instantaneous reading, -PLAYER_WET_HYSTERESIS for a body that was
// already submerged, which is what makes it keep counting as submerged until the eye is a
// clear band above the water. The feet probe is never biased — the feet are what say
// "in water at all", and a band there would let a body walk a quarter of a block into a
// pond before the water noticed, or a quarter of a block out of it before the water let go.
static BodyWet wetAt(const World* w, const Body* b, float eye_bias)
{
	const int bx = floorToInt(b->x);
	const int bz = floorToInt(b->z);

	// Eyes first, because eyes-under is the strongest answer and it does not matter what
	// the feet are in once it is true: a body head-first under a waterfall lip is
	// submerged even with air below it, which is the case the two-cell probe was written
	// for in the first place.
	if (blockInfo(worldGet(w, bx, floorToInt(b->y + b->eye + eye_bias), bz))->liquid)
		return BODY_SUBMERGED;
	if (blockInfo(worldGet(w, bx, floorToInt(b->y), bz))->liquid)
		return BODY_SURFACE;
	return BODY_DRY;
}

BodyWet bodyWetState(const World* w, const Body* b)
{
	return wetAt(w, b, 0.0f);
}

// Idempotent at a fixed position, which is why player.c and bodyStep may both call it in
// the same frame: if the answer is BODY_SUBMERGED then the eye was liquid at (eye - band),
// so it is certainly liquid at (eye - band) again next call; if it is not, the eye was air
// at (eye), and the second call — now unbiased in the other direction — probes the same
// cell. Neither can flip the other. That property is what lets the state be read where it
// is needed rather than threaded through three signatures.
BodyWet bodyWetUpdate(const World* w, Body* b)
{
	const float bias = (b->wet == BODY_SUBMERGED) ? -PLAYER_WET_HYSTERESIS : 0.0f;
	b->wet = wetAt(w, b, bias);
	return b->wet;
}

// Fraction of the remaining gap to close in dt seconds, for a first-order approach at
// `rate` per second. This is the backward-Euler step for dv/dt = -k*v, written as
// k*dt/(1 + k*dt) — one divide — rather than as the bare k*dt the naive version uses.
//
// The reason is BOUNDEDNESS, and it is worth being precise about because the other reason
// people reach for this form turns out not to apply here. k*dt passes 1.0 once dt exceeds
// 1/k and keeps going, so a long enough tick does not merely overshoot the target, it
// flings the body past it and then backwards. This form is in [0, 1) for every
// non-negative dt there is, so the worst a long tick can do is arrive. Measured: with the
// naive form in place, world_test.c's tick sweep goes red at dt = 0.25, 1.0 and 10.0
// ("worst.vy <= PLAYER_SWIM_UP_SPEED", three times) and stays green with this one.
//
// What this does NOT buy, measured rather than assumed: the resting float. Swapping the
// naive form back in leaves every check in testSurfaceSwim green — the crossings, the
// travel, the head clearance and the 20/30/60 fps spread alike — and reddens only the tick
// sweep and the three measured single-step values. The resting height is pinned by the
// state boundary, not by the decay rate, so frame-rate consistency of the float was never
// the thing at risk here. The tick sweep is the only witness this form has; if that sweep
// is ever deleted as redundant, this becomes an undefended one-liner.
static float dampAlpha(float rate, float dt_s)
{
	const float kdt = rate * dt_s;
	return kdt / (1.0f + kdt);
}

float bodyWalkSpeed(bool submerged)
{
	return submerged ? PLAYER_WALK_SPEED * PLAYER_WATER_SPEED_MUL : PLAYER_WALK_SPEED;
}

// A smooth bipolar wave over one cycle, without <math.h> — see the note at the top of this
// file for why there is no sinf here to call.
//
// A triangle folded out of the phase, then a cubic that rounds its corners off. The cubic
// is x * (1.5 - 0.5 * x*x), which is the standard cheap stand-in for sin(pi/2 * x): exact at
// -1, 0 and +1, and 0.6875 against a true 0.7071 at the quarter point. That worst error,
// 0.02, lands on an amplitude of 0.055 blocks and is therefore about one millimetre of
// camera. Nothing here is a measurement, so a millimetre of shape error costs nothing; the
// alternative was linking libm into source/world, which costs the host suite its
// independence from the ARM11's float behaviour.
static float bobWave(float t)
{
	float tri = 4.0f * t - 1.0f;             // t 0..1 -> -1..3
	if (tri > 1.0f) tri = 2.0f - tri;        // fold the top half back down: 1..3 -> 1..-1
	return tri * (1.5f - 0.5f * tri * tri);
}

float bodySurfaceBob(BodyWet wet, float dt_s, float* phase, float* envelope)
{
	// Only at the waterline. A submerged body is not floating on anything, and a dry one is
	// walking; both fade the swell out rather than dropping it, or the eye would step by up
	// to 5.5 cm on the frame the state changed.
	const float target = (wet == BODY_SURFACE) ? 1.0f : 0.0f;
	*envelope += (target - *envelope) * dampAlpha(PLAYER_SURFACE_BOB_FADE, dt_s);

	// Advanced unconditionally, so the swell has no memory of where it was interrupted and
	// two entries into the same lake do not look identical.
	*phase += PLAYER_SURFACE_BOB_RATE * dt_s;
	while (*phase >= 1.0f) *phase -= 1.0f;

	return bobWave(*phase) * PLAYER_SURFACE_BOB * *envelope;
}

void bodyJump(Body* b, BodyWet wet, bool jump_held, bool jump_pressed, float dt_s)
{
	// Armed on the EDGE, and BEFORE the early return below, so a press that arrives on the
	// same frame the body is still deciding whether it is wet is not lost. `jump_pressed`
	// and not `jump_held`: holding A is how you swim, so a held gate would arm the climb
	// permanently for anyone at the surface and gate nothing at all. Never cleared here --
	// bodyStep counts it down and trySwimUp consumes it -- because a release is not a
	// cancellation, it is just the player letting go after asking.
	if (jump_pressed && wet != BODY_DRY) b->swim_exit_t = PLAYER_SWIM_EXIT_WINDOW;

	if (wet != BODY_DRY) {
		if (!jump_held) return;

		// Under the surface the button still means "climb"; at the surface it means
		// "stay here". Before v1.8.2 it meant "climb" in both, so the swimmer was thrown
		// clear of the water, went dry, caught the full -28 gravity and fell back in --
		// and the frame he landed the SAME line snapped vy from negative to +3 with no
		// ramp at all. That reversal is the 4.4 Hz judder, and this is the line it came
		// from.
		const float target = (wet == BODY_SUBMERGED) ? PLAYER_SWIM_UP_SPEED
		                                            : PLAYER_SURFACE_RISE;

		// Raised TOWARDS the target, never set to it, and never lowered to it: a body
		// that entered the water already rising faster -- kicked off the bottom, or swum
		// up out of a current -- must not be slowed down by holding the button that is
		// supposed to lift it. Held from below, that same test is why a swimmer breaking
		// the surface with +3 on him is left to coast rather than being braked to zero:
		// he loses it to gravity and drag over the next fifth of a second instead, which
		// is what makes the arrival read as a float rather than as a stop.
		//
		// `<=`, not `<`, and the equal case matters more than it looks. It makes the pull
		// a no-op (the gap is zero) while still setting swim_drive, so a body sitting
		// exactly ON its target keeps gravity switched off and stays there EXACTLY --
		// vy = target is a fixed point of the whole system, not merely of this line. With
		// `<` the body at vy = 0 gets one ungoverned tick of gravity, and while that only
		// costs it 0.0155 blocks before the pull catches it again, "only drifts a bit" is
		// what attempt 1 said too.
		if (b->vy <= target) {
			b->vy += (target - b->vy) * dampAlpha(PLAYER_SWIM_RESPONSE, dt_s);
			b->swim_drive = true;
		}
		return;
	}

	// Unchanged from before task 25, and stated as the ELSE of the branch above rather
	// than as a second independent `if`: two writers to vy on one frame would make the
	// outcome depend on line order, which is the bug shape scene/title_nav.c was extracted
	// over. On land the jump is an edge, not a level, or holding the button flies.
	if (jump_pressed && b->on_ground) b->vy = PLAYER_JUMP_SPEED;
}

int bodyStep(Body* b, const World* w, float dt_s)
{
	// Buoyancy: a much weaker pull and a much lower terminal, chosen together in
	// physics.h. The terminal is the half that makes hitting the surface at speed feel
	// like water rather than like a slower kind of air -- a body arriving at -60 is
	// snapped to the water terminal on this very tick, before it moves.
	const BodyWet wet      = bodyWetUpdate(w, b);
	const bool    in_water = (wet != BODY_DRY);

	// The exit window ages here rather than in bodyJump, because bodyJump is only called
	// for the player and the window has to expire for a body that stops being driven. It
	// is decremented BEFORE the move below, so the frame the press lands still has very
	// nearly the whole window left and the last frame of the window still gets a move.
	// A body out of the water has nothing to climb out of, so the window is dropped.
	if (!in_water) b->swim_exit_t = 0.0f;
	else if (b->swim_exit_t > 0.0f) {
		b->swim_exit_t -= dt_s;
		if (b->swim_exit_t < 0.0f) b->swim_exit_t = 0.0f;
	}
	const float   terminal = in_water ? PLAYER_WATER_TERMINAL : PLAYER_TERMINAL;

	// Consumed, not merely read: the flag describes THIS frame's input and must not
	// survive into a frame bodyJump was not called for, or a player who releases the
	// button hangs in the water until something calls bodyJump again to clear it.
	const bool driven = b->swim_drive;
	b->swim_drive = false;

	// The v1.8.2 attempt-2 line, and the only one that mattered. A swimmer actively
	// pulling towards a target is not in free fall, so gravity is not applied at all while
	// he is: without this, vy is a balance between the pull and a constant -8, the balance
	// sits BELOW the target by g/k, and at the surface (target 0) that residual droop is
	// what sank the body back through the state boundary sixty times a second. With it,
	// vy = target is a genuine fixed point -- there is no error left for anything to
	// restore. It is also what puts the driven climb back at SWIM_UP_SPEED exactly instead
	// of at SWIM_UP_SPEED - WATER_GRAVITY/SWIM_RESPONSE.
	//
	// Only while DRIVEN. Let go and the body is in free fall again on the next frame,
	// which is what makes releasing the button sink you.
	if (!(in_water && driven))
		b->vy += (in_water ? PLAYER_WATER_GRAVITY : PLAYER_GRAVITY) * dt_s;

	// Water had no vertical drag at all, only the terminal clamp -- so a body that stopped
	// being driven kept every bit of the velocity it had and coasted ballistically until
	// the clamp caught it, which is what let a swimmer leaving the water arc into the air
	// before falling back. Drag is what makes the same departure decay instead of arc.
	//
	// Confined to an undriven UPWARD coast, which is the only motion it was ever meant to
	// describe, and attempt 1's failure to confine it is where two unasked-for regressions
	// came from: on the driven climb it fought the pull down to 1.215686 blocks/s, and on
	// the way down it balanced water gravity at -1.328954 and quietly retired
	// PLAYER_WATER_TERMINAL, which is the constant that is supposed to own the sink rate.
	// A sink already has a terminal; it does not need a second one derived from a drag.
	if (in_water && !driven && b->vy > 0.0f)
		b->vy -= b->vy * dampAlpha(PLAYER_WATER_VDRAG, dt_s);

	if (b->vy < terminal) b->vy = terminal;

	return bodyMove(b, w, b->vx * dt_s, b->vy * dt_s, b->vz * dt_s);
}
