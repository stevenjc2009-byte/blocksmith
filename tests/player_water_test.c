// Host tests for scene/player.c's v1.8.17 WATER-FX behaviour (Blocksmith).
//
// This links the REAL source/scene/player.c and source/scene/camera.c, not a reimplementation
// of either — see tests/player_water_stub/3ds.h and .../citro3d.h for the two small stand-in
// headers that make that possible on a host build (player.h pulls in <3ds.h> unconditionally
// for KEY_A; camera.h pulls in <3ds.h> and <citro3d.h> for the same reason). Neither stub
// changes what player.c or camera.c DO — see each stub's own header comment for exactly what
// surface it supplies and why an external stub was chosen over restructuring a header this
// lane does not own.
//
// What this file proves, all against the real, linked playerUpdate():
//
//   1. The entry-splash spawn position (v1.8.17 task 1). The bug this task fixed spawned the
//      splash at the FEET (p->body.y), which is below the waterline the instant a body is
//      wet at all. testEntrySplashSpawnsAtWaterPlaneNotFeet drives a body through a real fall
//      into a real water column and checks the ACTUAL feet Y at the moment of entry against
//      the true water plane — both taken from the real linked code, not asserted from theory.
//   2. The exit splash fires on exactly one frame (task 2), not zero and not repeatedly.
//      testExitSplashFiresExactlyOnce drives a body through a real climb onto a real bank
//      (world/physics.c's trySwimUp — the same mechanism a player's press of A at a shoreline
//      already exercises) and counts transitions across the whole run.
//   3. The swim wake is genuinely rate-limited to the pool, not the frame (task 3).
//      testWakeRateAcrossFrameTimeSweep drives a continuous swim across a sweep of frame
//      times INCLUDING the MAX_TICK (50 ms) clamp boundary and checks the observed spawn
//      count tracks elapsed SIMULATED time / WAKE_INTERVAL, not the number of playerUpdate
//      calls made — a one-sample gate at 60 fps alone would not catch a per-frame emitter
//      whose rate only looks right at exactly 60 fps.
//
// What this file does NOT and CANNOT prove: how any of this LOOKS. Particle colour, size,
// fade curve and the visual read of "a splash" or "a wake" are outside what a host test can
// see — see this lane's own final report for what that gap means in practice.
#include "scene/player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx/particles.h"
#include "world/block.h"
#include "world/registry.h"

static int checks = 0;
static int fails  = 0;

#define CHECK(cond) do {                                                        \
	checks++;                                                                    \
	if (!(cond)) {                                                               \
		fails++;                                                                 \
		printf("FAIL %d/%d L%d  %s\n", fails, checks, __LINE__, #cond);          \
	}                                                                            \
} while (0)

// ── Input, owned by this test ─────────────────────────────────────────────────────────────
//
// See tests/player_water_stub/3ds.h's header comment: the real hidCircleRead/hidKeysHeld/
// hidKeysDown are declared there and defined here, driven by these three globals, so every
// frame's input is exact and repeatable.
static u32 g_keys_held;
static u32 g_keys_down;
static circlePosition g_stick;

void hidCircleRead(circlePosition* pos) { *pos = g_stick; }
u32  hidKeysHeld(void) { return g_keys_held; }
u32  hidKeysDown(void) { return g_keys_down; }

static void inputNeutral(void)
{
	g_keys_held = 0;
	g_keys_down = 0;
	g_stick.dx = 0;
	g_stick.dy = 0;
}

// ── World setup ────────────────────────────────────────────────────────────────────────────
//
// A WATER_DEPTH-deep, WATER_WIDTH-wide water pool sitting on nothing (buoyancy needs no floor
// — see world/physics.h's own header comment: "a swimmer is a body falling through a cell that
// happens to be blue"), plus a SUBMERGED wall at the pool's edge for the exit-climb test.
// registryInitCore() is required before any worldSet/blockInfo call — see world/world_test.c's
// own testPlayerWiresSwimming and every *_test.c in this tree that touches the registry for the
// same call, first.
//
// The wall spans y = 0..WATER_DEPTH-1 -- the SAME rows the water itself occupies -- and
// nothing at or above y = WATER_DEPTH (the water plane). This was not the first shape tried:
// a single block sitting AT the plane (y = WATER_DEPTH), flush against the water's edge
// column, blocks the horizontal approach correctly but ALSO occupies the exact cell
// trySwimUp's own headroom probe (bodyHits at the lifted step_y) has to land in — a real
// player's box is 1.8 blocks tall, so any solid cell within reach of the approach also reaches
// the landing spot one water-plane's-width above it, and the climb refuses every time,
// measured: 240 frames, exit_transitions=0, body stalled at exactly bank_x - half_w. A wall
// confined to BELOW the plane blocks the swim (the body's box overlaps it while still
// underwater) without ever overlapping the landing cell above it, which is what "climbing out
// of the water" onto open air past a submerged obstruction is meant to model in the first
// place — trySwimUp's own header comment says as much: "the same [target] a real player
// pressed against the water finds", not a raised, standable block.
#define WATER_DEPTH 5
#define WATER_WIDTH 20
#define BANK_X      WATER_WIDTH

static void buildWorld(World* w)
{
	registryInitCore();
	worldInit(w);
	for (int x = 0; x < WATER_WIDTH; x++)
		for (int y = 0; y < WATER_DEPTH; y++)
			worldSet(w, x, y, 0, BLOCK_WATER);
	for (int y = 0; y < WATER_DEPTH; y++)
		worldSet(w, BANK_X, y, 0, BLOCK_STONE);
}

// ── Task 1: the entry-splash spawn position ─────────────────────────────────────────────────
//
// Drops a body from well above the pool with no input at all (so the only particles that can
// possibly appear are from the entry splash — the wake needs horizontal speed, which stays
// zero the whole fall) and watches, frame by frame, for the exact moment player.c's own
// prev_wet/wet comparison would fire — see the long header comment above for why a snapshot of
// p.body.wet taken immediately before and after one playerUpdate() call is exactly the
// prev_wet/wet pair player.c itself compares that frame, not an approximation of it.
static void testEntrySplashSpawnsAtWaterPlaneNotFeet(void)
{
	World w;
	buildWorld(&w);
	particlesInit();

	Player p;
	// Starting height alone was tried first and dropped: a natural fall from WATER_DEPTH+3
	// (gravity accelerating from rest) crosses the plane at only a modest speed on the exact
	// frame the transition is detected, so the one-frame step below the plane at that moment
	// measured only 0.05-0.15 blocks -- small enough that reverting the fix (spawning at the
	// feet instead of the plane) STILL passed the "spawned Y is near the plane" check, by
	// accident, not by proof (the classic "a check that cannot fail proves nothing" trap,
	// confirmed by actually reverting particlesSpawnSplash's y-argument to p->body.y and
	// re-running: 29/29 checks still passed). A large, EXPLICITLY set vy fixes this the same
	// way testWakeRateAcrossFrameTimeSweep controls dt directly rather than hoping a frame
	// rate produces the case it needs: starting 0.05 blocks above the plane with vy already at
	// -30 (well under PLAYER_TERMINAL's -60, so bodyStep's terminal clamp does not interfere)
	// guarantees a large, deterministic one-frame overshoot below the plane the instant the
	// transition is detected -- the exact separation this check needs to be capable of failing.
	playerInit(&p, 0.3f, (float)WATER_DEPTH + 0.05f, 0.3f, 0.0f, 0.0f);
	p.body.vy = -30.0f;
	inputNeutral();

	const float water_plane = (float)WATER_DEPTH;
	bool found = false;
	float feet_y_at_entry = 0.0f;
	int   entry_frame = -1;

	for (int frame = 0; frame < 300 && !found; frame++) {
		const int    before_count = particlesLiveCount();
		const BodyWet before_wet  = p.body.wet;
		const float   feet_y      = p.body.y;   // what player.c's spawn call will read, if reached this frame

		playerUpdate(&p, &w, 16.0f);

		const BodyWet after_wet = p.body.wet;
		if (before_wet == BODY_DRY && after_wet != BODY_DRY) {
			found = true;
			entry_frame = frame;
			feet_y_at_entry = feet_y;

			CHECK(particlesLiveCount() > before_count);   // the entry splash actually spawned something

			// Section 1 of this lane's report: proof the bug was real. The feet Y at the exact
			// frame the transition fired must NOT already equal the water plane — if it did,
			// this test would not be able to tell the fixed code from the reverted one, which
			// is exactly the "a check that cannot fail proves nothing" trap. 0.2f, not 0.05f:
			// the forced vy=-30 entry above guarantees roughly a 0.43-block one-frame overshoot
			// (measured), so this threshold has real margin rather than sitting on the noise
			// floor of whatever the fall dynamics happen to produce.
			CHECK(fabsf(feet_y_at_entry - water_plane) > 0.2f);

			// The actual splash particle(s): first new slot is exactly `before_count`, since
			// particlesSpawn's ring cursor only advances (see particles.h's own comment).
			float x, y, z, a;
			CHECK(particlesGet(before_count, &x, &y, &z, &a) == true);
			// One playerUpdate tick of drift (rise 0.8-1.8 blocks/s, see particlesSpawnSplash)
			// on top of the spawn Y, so "close to the plane" rather than bit-exact. 0.05f, not
			// 0.15f: tightened once the entry setup above stopped relying on incidental fall
			// timing -- the fixed code's y is the plane plus at most ~0.03 of one tick's rise
			// drift regardless of impact speed, so 0.05f has margin and the buggy value (which
			// would land ~0.43 off) fails it by nearly an order of magnitude, not by luck.
			CHECK(fabsf(y - water_plane) < 0.05f);
			// A third check -- "the spawn Y is far from feet_y_at_entry" -- is implied by the
			// two above (feet ~0.43 below the plane, spawn within 0.05 of it) rather than
			// asserted separately: an earlier version of this test relied on incidental fall
			// timing instead of a forced vy (see the setup comment above) and that version's
			// feet-to-plane gap was small enough at detection that a THIRD check on this same
			// quantity added no proof beyond the two above. With the forced -30 vy entry that
			// earlier flakiness is gone, but the two checks above are already the complete
			// claim -- feet measurably below the plane (line 170), spawn pinned to the plane
			// (line 182) -- so a third redundant check was not brought back.
		}
	}

	CHECK(found);   // the body must actually have entered the water within 300 frames (5s at 60fps)
	if (!found) printf("  (entry never detected -- see the fall setup)\n");
	(void)entry_frame;
}

// ── Task 2: the exit splash fires on exactly one frame ──────────────────────────────────────
//
// Starts already floating (BODY_SURFACE), primes one frame so p.body.wet reflects that
// honestly (see the primer frame's own comment), then swims toward the bank with the jump
// button held down every frame — v1.8.16's own bodyJump comment: "holding A IS the float
// input... it cannot also mean lift me onto the bank [unless] on_ground", and a floater is
// never on_ground, so a HELD press here only ever re-arms the window every frame without ever
// taking the on-ground branch, which removes all timing fragility from this test without
// changing which branch of bodyJump actually fires the climb (trySwimUp, gated on
// swim_exit_t > 0, exactly as a single fresh press would arm it).
static void testExitSplashFiresExactlyOnce(void)
{
	World w;
	buildWorld(&w);
	particlesInit();

	Player p;
	// Feet well inside the pool, eyes clear of the surface: BODY_SURFACE the instant the
	// first bodyWetUpdate reads this position (see wetAt's feet/eye probe in physics.c).
	playerInit(&p, (float)BANK_X - 3.0f, (float)WATER_DEPTH - 0.3f, 0.3f, 0.0f, 0.0f);

	// Primer frame: no input, lets p.body.wet catch up to the honest BODY_SURFACE reading
	// before the run below starts counting. This frame's own prev_wet (BODY_DRY, from
	// playerInit's bodyInit) vs wet (BODY_SURFACE) is itself a legitimate entry-splash
	// transition -- expected, and excluded from this test's own count by starting the loop
	// after it.
	inputNeutral();
	playerUpdate(&p, &w, 16.0f);
	CHECK(p.body.wet == BODY_SURFACE);   // the setup this test depends on actually landed

	int exit_transitions = 0;
	int exit_frame = -1;

	for (int frame = 0; frame < 240; frame++) {   // 4s at 60fps -- ample against a 0.4-block approach at 2.15 blocks/s
		const int    before_count = particlesLiveCount();
		const BodyWet before_wet  = p.body.wet;

		g_keys_held = KEY_DRIGHT | KEY_A;   // swim toward the bank (+x), jump held every frame
		g_keys_down = KEY_A;                // re-arms the exit window every frame -- see this
		                                     // function's own header comment for why a held
		                                     // press cannot instead fire bodyJump's on-ground
		                                     // branch here
		playerUpdate(&p, &w, 16.0f);

		const BodyWet after_wet = p.body.wet;
		if (before_wet == BODY_SURFACE && after_wet == BODY_DRY) {
			exit_transitions++;
			exit_frame = frame;
			// A lower bound only, not a tight one: the continuous swim toward the bank also
			// qualifies for the wake (speed 2.15 blocks/s > WAKE_SPEED_THRESHOLD), so
			// before_count already includes some short-lived wake particles that can die the
			// same frame the exit splash spawns -- see testWakeRateAcrossFrameTimeSweep's own
			// header comment for why a raw live-count delta is the wrong instrument once
			// anything in the pool can be dying and spawning in the same frame. This just
			// confirms SOMETHING grew, corroborating the transition; the wet-state comparison
			// above is the real proof this fires exactly once.
			CHECK(particlesLiveCount() > before_count);
		}
	}

	CHECK(exit_transitions == 1);   // fires on exactly one frame -- not zero, not repeatedly
	if (exit_transitions != 1)
		printf("  exit_transitions=%d final_wet=%d body=(%.4f,%.4f,%.4f)\n",
		       exit_transitions, (int)p.body.wet, p.body.x, p.body.y, p.body.z);
	(void)exit_frame;
}

// ── Task 3: the swim wake is rate-limited to the pool, not the frame ────────────────────────
//
// One arm of this sweep, at a given per-call dt_ms and a target simulated duration. Swims in
// open water (BANK_X keeps the shoreline far away so the run cannot accidentally climb out)
// at a constant, above-threshold horizontal speed the whole time and counts every particle
// SPAWN EVENT (splash or wake) that occurs, then reports how many of those events were
// wake-sized (exactly 2 particles).
//
// NOT a live-count delta. A first attempt used particlesLiveCount() before/after each frame
// and it undercounted badly (measured: 3 wake spawns detected against ~9-10 expected over a
// 3s run) — particles die on their own life countdown (0.3-0.6s, the same order as
// WAKE_INTERVAL itself), so by the time a run is long enough to see several wake events,
// earlier ones are already dying in the background, and a raw before/after count difference
// conflates "N spawned" with "M died" into one number that is neither. The fix: alpha is
// exactly 1.0 at the instant of any particlesSpawn call and only ever DECREASES afterwards for
// a surviving particle (particles.h's own particlesGet comment: "1.0 at spawn, falling
// linearly to 0.0"), so a fixed-size array tracking last frame's (alive, alpha) per SLOT turns
// "did slot i just get written by a fresh spawn" into a simple per-slot comparison — dead-to-
// alive, or an alpha that went UP instead of down — that is correct regardless of what else in
// the pool is dying that same frame. Summing that across all PARTICLES_MAX slots for one frame
// gives the exact number of particlesSpawn calls that frame, splash and wake both, which is
// exactly the real spawn cursor's own movement, read through the public API rather than a
// reimplementation of it.
static int runWakeSweepArm(float dt_ms, float target_seconds, float* sim_seconds_out, BodyWet* final_wet_out)
{
	World w;
	buildWorld(&w);
	particlesInit();

	Player p;
	playerInit(&p, 0.3f, (float)WATER_DEPTH - 0.3f, 0.3f, 0.0f, 0.0f);
	inputNeutral();
	playerUpdate(&p, &w, 16.0f);   // primer, same reasoning as testExitSplashFiresExactlyOnce

	const float MAX_TICK_S = 0.05f;   // player.c's own clamp -- duplicated here only to predict
	                                    // how many frames this arm needs, never to decide a check
	const float dt_s_clamped = (dt_ms * 0.001f > MAX_TICK_S) ? MAX_TICK_S : dt_ms * 0.001f;
	const int frames = (int)(target_seconds / dt_s_clamped) + 1;

	static bool  prev_alive[PARTICLES_MAX];
	static float prev_alpha[PARTICLES_MAX];
	memset(prev_alive, 0, sizeof(prev_alive));
	memset(prev_alpha, 0, sizeof(prev_alpha));
	// The primer frame above may already have spawned an entry splash; seed prev_* from its
	// result so this run's own loop only ever counts spawns that happen INSIDE the loop.
	for (int i = 0; i < PARTICLES_MAX; i++) {
		float x, y, z, a;
		prev_alive[i] = particlesGet(i, &x, &y, &z, &a);
		prev_alpha[i] = prev_alive[i] ? a : 0.0f;
	}

	int spawn_events    = 0;   // total particles spawned across the loop, any size
	int wake_size_events = 0;  // of those, spawn events sized exactly 2 (a wake, never a splash)
	float sim_seconds = 0.0f;

	for (int frame = 0; frame < frames; frame++) {
		// KEY_DRIGHT alone is not "swimming" in this game -- measured, real run: with only the
		// directional key held, physics.c's bodyJump never applies any vertical impulse at all
		// (its whole rise/float branch is gated on `jump_held`, see that function's own comment:
		// "Holding A IS the float input -- it is what keeps him at the surface"), so bodyStep's
		// buoyancy alone (a WEAKER pull, not a net-positive one -- see its own comment, "a much
		// weaker pull and a much lower terminal") is not enough to hold station: the body just
		// sinks to BODY_SUBMERGED and stays there. Confirmed by tracing wet transitions on a
		// first attempt: every dt arm crossed SURFACE -> SUBMERGED once around sim_seconds~1.0s
		// (vy=-1.5 at the crossing) and never came back, which is exactly why wake_spawns
		// flatlined at 3 regardless of run length. Holding KEY_A the whole time is not a special
		// case, it is how this game's own swim works, and it is safe to hold continuously in
		// OPEN water specifically because trySwimUp (the only thing a held A could trigger
		// besides the float impulse) is called from bodyMove ONLY when horizontal movement is
		// BLOCKED by a collision (see bodyMove's `blocked_x && (tryStepUp(...) || trySwimUp(...))`
		// -- physics.c lines 369-370/378-379) -- unreachable here since BANK_X keeps the whole
		// pool clear of any obstruction this run ever reaches.
		g_keys_held = KEY_DRIGHT | KEY_A;
		g_keys_down = (frame == 0) ? KEY_A : 0;   // one genuine press edge, then held -- not a
		                                            // re-press every frame (contrast with
		                                            // testExitSplashFiresExactlyOnce, which
		                                            // deliberately re-presses to force a climb
		                                            // against a bank; nothing here is near one)
		playerUpdate(&p, &w, dt_ms);
		sim_seconds += dt_s_clamped;

		int fresh_this_frame = 0;
		for (int i = 0; i < PARTICLES_MAX; i++) {
			float x, y, z, a;
			const bool alive = particlesGet(i, &x, &y, &z, &a);
			const bool fresh = alive && (!prev_alive[i] || a > prev_alpha[i] + 1e-6f);
			if (fresh) { spawn_events++; fresh_this_frame++; }
			prev_alive[i] = alive;
			prev_alpha[i] = alive ? a : 0.0f;
		}
		// A splash is always 8-16 particles in one call (particlesSpawnSplash's own header
		// comment); a wake is always exactly 2 (particlesSpawnWake's). The two cannot land on
		// the same frame here (nothing in this run's input arms the exit climb or crosses a
		// DRY boundary), so an exact match to 2 unambiguously identifies a wake event.
		if (fresh_this_frame == 2) wake_size_events++;
		else if (fresh_this_frame != 0)
			printf("  [dt_ms=%.1f frame=%d] %d particles spawned this frame (not a wake's 2)\n",
			       (double)dt_ms, frame, fresh_this_frame);
	}

	*sim_seconds_out = sim_seconds;
	*final_wet_out = p.body.wet;
	(void)spawn_events;
	return wake_size_events;
}

static void testWakeRateAcrossFrameTimeSweep(void)
{
	// Below, at, and above the MAX_TICK (50ms) clamp boundary -- the one-sample-gate lesson:
	// a rate limiter that only looks right at 60fps (16.7ms) is not proven right by one sample
	// at 16.7ms.
	const float dt_arms[] = { 8.0f, 16.0f, 33.0f, 50.0f, 70.0f, 100.0f, 200.0f };
	const float target_seconds = 3.0f;

	for (size_t i = 0; i < sizeof(dt_arms) / sizeof(dt_arms[0]); i++) {
		float sim_seconds = 0.0f;
		BodyWet final_wet = BODY_DRY;
		const int wake_spawns = runWakeSweepArm(dt_arms[i], target_seconds, &sim_seconds, &final_wet);

		CHECK(final_wet == BODY_SURFACE);   // the arm actually swam the whole run, not sank/climbed out

		// Expected: floor(sim_seconds / WAKE_INTERVAL), +-1 for the primer frame and the
		// accumulator's own remainder-banking at the run's start/end boundary. WAKE_INTERVAL
		// (0.3s) is duplicated here, the same way MAX_TICK_S is above -- see this lane's final
		// report for why player.c's own constant could not be reached from this file instead.
		const float WAKE_INTERVAL = 0.3f;
		const int expected = (int)(sim_seconds / WAKE_INTERVAL);

		const int frames_at_this_dt = (int)(target_seconds / ((dt_arms[i] * 0.001f > 0.05f) ? 0.05f : dt_arms[i] * 0.001f)) + 1;

		printf("  [dt_ms=%.1f] sim_seconds=%.3f frames=%d wake_spawns=%d expected~%d\n",
		       (double)dt_arms[i], (double)sim_seconds, frames_at_this_dt, wake_spawns, expected);

		// The real claim: NOT one spawn per frame (frames_at_this_dt is 15-375 across this
		// sweep) and NOT zero -- within 2 of the interval-derived expectation, which a
		// per-frame emitter would miss by orders of magnitude in every arm below the clamp,
		// and a never-fires emitter would miss by `expected` in every arm.
		CHECK(wake_spawns >= expected - 2 && wake_spawns <= expected + 2);
		CHECK(wake_spawns < frames_at_this_dt);   // the actual "not per frame" claim, stated directly
	}
}

// ── Live-particle-count measurement (this lane's final report §4) ──────────────────────────
//
// Not a claim about correctness (the three tests above already cover that) -- a measurement of
// the pool pressure the three v1.8.17 emitters actually put on PARTICLES_MAX (512, see
// particles.h), taken from two real scenarios rather than reasoned from the count/life
// constants alone:
//
//   Steady state: a long open-water swim, wake only (no entry or exit in the sampling window).
//   Worst case:   entry splash, then an immediate short swim into a bank inches away, so the
//                 exit splash's climb fires while the entry splash's own particles (life up to
//                 0.6s, particlesSpawnSplash's own comment) are still plausibly alive, stacked
//                 on top of whatever wake fired in the gap between the two.
//
// gfx/particles.c's own particlesSpawnSplash/particlesSpawnWake comments give the constants
// this reasons from: a splash is 8-16 particles at 0.35-0.6s life, a wake is exactly 2 at
// 0.3-0.5s life, fired at most once per WAKE_INTERVAL (0.3s) while swimming.
static void testLiveParticleCountsUnderRealScenarios(void)
{
	// Steady state: open water, far from any bank, float button held the whole time (see
	// runWakeSweepArm's own header comment for why that is required at all to stay at
	// BODY_SURFACE), sampled only after the first 1s so the entry splash's own particles (from
	// the primer/first frame going wet) have long since died (their life is at most 0.6s) and
	// what remains is the wake alone, in steady state.
	{
		World w;
		buildWorld(&w);
		particlesInit();

		Player p;
		playerInit(&p, 0.3f, (float)WATER_DEPTH - 0.3f, 0.3f, 0.0f, 0.0f);
		inputNeutral();
		playerUpdate(&p, &w, 16.0f);   // primer

		int steady_max = 0;
		long long steady_sum = 0;
		int steady_samples = 0;

		const int total_frames = 360;      // 6s at 16ms
		const int skip_frames  = 90;       // first 1.44s excluded -- entry-splash transient
		for (int frame = 0; frame < total_frames; frame++) {
			g_keys_held = KEY_DRIGHT | KEY_A;
			g_keys_down = (frame == 0) ? KEY_A : 0;
			playerUpdate(&p, &w, 16.0f);

			if (frame >= skip_frames) {
				const int n = particlesLiveCount();
				if (n > steady_max) steady_max = n;
				steady_sum += n;
				steady_samples++;
			}
		}

		CHECK(p.body.wet == BODY_SURFACE);   // the measurement window actually stayed swimming

		const double steady_mean = (double)steady_sum / steady_samples;
		printf("  MEASURED steady-state (wake only, %d samples after %d-frame transient): "
		       "max=%d mean=%.2f\n", steady_samples, skip_frames, steady_max, steady_mean);

		// Sanity bound, not a tight one: wake alone (2 particles / 0.3s, life <= 0.5s) cannot
		// plausibly exceed roughly 2 * ceil(0.5 / 0.3) + 2 = 6-ish concurrent, and is nowhere
		// close to PARTICLES_MAX (512). A steady-state reading anywhere near triple digits
		// would mean something is retriggering far faster than WAKE_INTERVAL.
		CHECK(steady_max < 20);
	}

	// Worst case: fall in right at the water's edge next to a bank, so the approach to climb
	// out is as short as this game's own geometry allows, then hold the float+jump combo the
	// exit-splash test already established is safe to hold continuously in the open (see that
	// test's own header comment) while also moving toward the bank, so entry splash, wake, and
	// exit splash all have the best real chance this game's own physics allows of overlapping.
	{
		World w;
		buildWorld(&w);
		particlesInit();

		Player p;
		// 0.5 blocks out from the bank -- as short an approach as this fixture can give the
		// exit climb, to give the entry splash's own particles (life up to 0.6s,
		// particlesSpawnSplash's own comment) the least time to decay before the exit splash
		// piles a second batch on top. Wider approaches were measured first (1.3 blocks: peak
		// 18, more of the entry batch had already died by the time the climb landed) and
		// dropped in favour of this tighter one specifically because a worst-case measurement
		// should report the worst case this design can actually produce, not an arbitrary one.
		//
		// No horizontal input during the fall, until the body is confirmed wet, below: an
		// earlier version held KEY_DRIGHT from frame 0 and the body walked onto and over the
		// bank's TOP before it ever fell far enough to register BODY_SURFACE at all (measured:
		// wet stayed BODY_DRY for the full 180-frame run, live count 0 throughout) -- climbing
		// a step is instant and needs no water, so a horizontal approach started too early races
		// the fall and wins.
		playerInit(&p, (float)BANK_X - 0.5f, (float)WATER_DEPTH + 3.0f, 0.3f, 0.0f, 0.0f);
		p.body.vy = -30.0f;   // same forced-entry-speed reasoning as testEntrySplashSpawnsAtWaterPlaneNotFeet
		inputNeutral();

		int worst_max = 0;
		int worst_frame = -1;
		bool swimming = false;

		for (int frame = 0; frame < 180; frame++) {   // 3s at 16ms -- ample for one entry+exit
			const bool just_started_swimming = swimming && (frame > 0) &&
			                                    (g_keys_held == 0);   // last frame was still falling
			if (swimming) {
				g_keys_held = KEY_DRIGHT | KEY_A;
				// One genuine press edge on the first swimming frame arms swim_exit_t (see
				// bodyJump's own comment: a floater only arms on jump_pressed, never on a bare
				// held button, so this must be set exactly once, not every frame -- contrast
				// testExitSplashFiresExactlyOnce's deliberate every-frame re-press, which exists
				// there only because that test starts already floating with no fall to key off).
				g_keys_down = just_started_swimming ? KEY_A : 0;
			} else {
				g_keys_held = 0;   // straight fall, no horizontal drift, until confirmed wet
				g_keys_down = 0;
			}
			playerUpdate(&p, &w, 16.0f);
			if (!swimming && p.body.wet != BODY_DRY) swimming = true;   // start the approach the
			                                                             // instant entry lands
			const int n = particlesLiveCount();
			if (n > worst_max) { worst_max = n; worst_frame = frame; }
		}

		printf("  MEASURED worst case (entry + short swim + exit, close bank, 180 frames): "
		       "max=%d at frame=%d\n", worst_max, worst_frame);

		// Two full 16-particle splashes overlapping completely, plus wake in the gap, is the
		// theoretical ceiling (up to ~34) this scenario's constants allow -- measured comes in
		// lower (peak 25 at the 0.5-block approach above) because the exit splash's own count
		// scales down for a gentle climb (particlesSpawnSplash's own comment: near-zero impact
		// speed lands it at or near its 8-particle floor) and some of the entry batch has
		// already decayed by the time the climb lands. 40 is a round number safely above the
		// measured peak with margin for a different approach distance or fall speed, not a
		// figure the measurement was tuned to hit; PARTICLES_MAX is 512, so nothing in this
		// scenario is remotely close to pool exhaustion either way.
		CHECK(worst_max < 40);
		CHECK(worst_max < PARTICLES_MAX);
	}
}

int main(void)
{
	testEntrySplashSpawnsAtWaterPlaneNotFeet();
	testExitSplashFiresExactlyOnce();
	testWakeRateAcrossFrameTimeSweep();
	testLiveParticleCountsUnderRealScenarios();

	printf("player water self-test: %s %d checks\n", fails ? "FAIL" : "PASS", checks);
	return fails ? 1 : 0;
}
