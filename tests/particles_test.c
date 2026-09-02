// Host tests for the particle system (source/gfx/particles.c), Blocksmith v1.8.9.
//
// Everything exercised here is the pool/spawn/tick half of particles.c, which has no
// <3ds.h> and no citro3d anywhere in it — see particles.h's header comment on why
// particlesDraw is the one function this file cannot and does not call. What this suite
// proves: the pool never overflows or corrupts (it recycles the OLDEST slot, proven by
// exact index, not just by a count staying capped), the fall/fade arithmetic matches a
// hand-derived expected value rather than merely "the code agrees with itself", the splash
// convenience stays inside its documented 8-16 particle range across a swept range of
// impact speeds rather than at one sample, and a splash is a pure function of its inputs
// (bit-identical across two calls from a freshly reset pool) rather than depending on
// anything this file cannot control.
#include "gfx/particles.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int fails  = 0;

#define CHECK(cond) do {                                                        \
	checks++;                                                                    \
	if (!(cond)) {                                                               \
		fails++;                                                                 \
		printf("FAIL %d/%d L%d  %s\n", fails, checks, __LINE__, #cond);          \
	}                                                                            \
} while (0)

static void testInitStartsEmpty(void)
{
	CHECK(particlesInit() == true);
	CHECK(particlesLiveCount() == 0);

	float x, y, z, a;
	CHECK(particlesGet(0, &x, &y, &z, &a) == false);
}

static void testSpawnAddsOneParticle(void)
{
	particlesInit();

	const ParticleSpawnDesc d = {
		.x = 3.0f, .y = 4.0f, .z = 5.0f,
		.vx = 0.0f, .vy = 0.0f, .vz = 0.0f,
		.life_seconds = 2.0f,
		.size = 0.05f,
		.rgba = PARTICLE_RGBA(255, 255, 255, 255),
	};
	particlesSpawn(&d);

	CHECK(particlesLiveCount() == 1);

	float x = 0, y = 0, z = 0, a = 0;
	// poolReset (inside particlesInit) rewinds the ring cursor to slot 0, so the first
	// spawn after init always lands in slot 0 — not an assumption, the contract particlesSpawn
	// documents ("a fixed ring position advanced by exactly one slot per spawn").
	CHECK(particlesGet(0, &x, &y, &z, &a) == true);
	CHECK(x == 3.0f && y == 4.0f && z == 5.0f);
	CHECK(a == 1.0f);   // full alpha at the instant of spawn: life == life_total
}

static void testSpawnRefusesNonPositiveLife(void)
{
	particlesInit();

	ParticleSpawnDesc d = {
		.x = 1, .y = 1, .z = 1, .vx = 0, .vy = 0, .vz = 0,
		.size = 0.05f, .rgba = PARTICLE_RGBA(1, 2, 3, 4),
	};

	d.life_seconds = 0.0f;
	particlesSpawn(&d);
	CHECK(particlesLiveCount() == 0);

	d.life_seconds = -1.0f;
	particlesSpawn(&d);
	CHECK(particlesLiveCount() == 0);
}

static void testSpawnRefusesNullDesc(void)
{
	particlesInit();
	particlesSpawn(NULL);   // must not crash
	CHECK(particlesLiveCount() == 0);
}

static void testTickMovesAndAppliesGravity(void)
{
	particlesInit();

	const ParticleSpawnDesc d = {
		.x = 0.0f, .y = 0.0f, .z = 0.0f,
		.vx = 1.0f, .vy = 0.0f, .vz = 2.0f,
		.life_seconds = 10.0f,
		.size = 0.05f,
		.rgba = PARTICLE_RGBA(0, 0, 0, 255),
	};
	particlesSpawn(&d);

	particlesTick(1.0f);

	// Hand-derived, not re-derived from the implementation: semi-implicit Euler with
	// PARTICLE_GRAVITY == -28.0f/s^2 (matches world/physics.h's PLAYER_GRAVITY -- see
	// particles.c's own comment on that constant) over dt=1 gives
	//   vy = 0 + (-28)*1 = -28          y = 0 + (-28)*1 = -28
	//   x  = 0 + 1*1     =  1           z = 0 + 2*1     =  2
	// and every one of those is an exact power-of-small-integer float, so bit-exact
	// equality is the right check, not a tolerance.
	float x = 0, y = 0, z = 0, a = 0;
	CHECK(particlesGet(0, &x, &y, &z, &a) == true);
	CHECK(x == 1.0f);
	CHECK(y == -28.0f);
	CHECK(z == 2.0f);

	// life went from 10 to 9, alpha = 9/10 = 0.9 -- a tolerance here because 9.0f/10.0f is
	// not guaranteed bit-identical to the literal 0.9f, only numerically close.
	CHECK(a > 0.899f && a < 0.901f);
}

static void testLifeReachesExactlyZeroAndParticleDies(void)
{
	particlesInit();

	const ParticleSpawnDesc d = {
		.x = 9, .y = 9, .z = 9, .vx = 0, .vy = 0, .vz = 0,
		.life_seconds = 1.0f, .size = 0.05f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
	};
	particlesSpawn(&d);
	CHECK(particlesLiveCount() == 1);

	particlesTick(1.0f);   // life: 1.0 - 1.0 = 0.0 exactly

	CHECK(particlesLiveCount() == 0);
	float x, y, z, a;
	CHECK(particlesGet(0, &x, &y, &z, &a) == false);
}

static void testTickClampsNegativeDt(void)
{
	particlesInit();

	const ParticleSpawnDesc d = {
		.x = 7.0f, .y = 8.0f, .z = 9.0f,
		.vx = 5.0f, .vy = 5.0f, .vz = 5.0f,
		.life_seconds = 3.0f, .size = 0.05f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
	};
	particlesSpawn(&d);

	particlesTick(-1.0f);   // must be clamped to dt=0, not run the sim backwards

	float x = 0, y = 0, z = 0, a = 0;
	CHECK(particlesGet(0, &x, &y, &z, &a) == true);
	CHECK(x == 7.0f && y == 8.0f && z == 9.0f);
	CHECK(a == 1.0f);   // life untouched too
}

static void testAlphaFadeMonotonicAndBounded(void)
{
	particlesInit();

	const ParticleSpawnDesc d = {
		.x = 0, .y = 0, .z = 0, .vx = 0, .vy = 0, .vz = 0,
		.life_seconds = 1.0f, .size = 0.05f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
	};
	particlesSpawn(&d);

	float x, y, z, prev = 2.0f;   // above the valid range so the first comparison always passes
	CHECK(particlesGet(0, &x, &y, &z, &prev) == true);
	CHECK(prev == 1.0f);

	for (int step = 0; step < 4; step++) {
		particlesTick(0.25f);
		float a = -1.0f;
		bool alive = particlesGet(0, &x, &y, &z, &a);
		if (step < 3) {
			CHECK(alive == true);
			CHECK(a >= 0.0f && a <= 1.0f);
			CHECK(a < prev);   // strictly decreasing every step while alive
			prev = a;
		} else {
			// step 3: life has counted down by 4 * 0.25 == life_seconds exactly -> dead.
			CHECK(alive == false);
		}
	}
}

static void testSpawningPastCapacityRecyclesTheOldest(void)
{
	particlesInit();

	const int extra = 50;
	const int total = PARTICLES_MAX + extra;

	// Every spawn's x is its own spawn index k, life long enough (1000s) that nothing dies
	// and no velocity so nothing moves -- position is a pure, exact record of "which spawn
	// call last wrote this slot".
	for (int k = 0; k < total; k++) {
		const ParticleSpawnDesc d = {
			.x = (float)k, .y = 0, .z = 0, .vx = 0, .vy = 0, .vz = 0,
			.life_seconds = 1000.0f, .size = 0.01f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
		};
		particlesSpawn(&d);
	}

	// The pool is full -- 512 spawns' worth of slots, every one still alive.
	CHECK(particlesLiveCount() == PARTICLES_MAX);

	// The ring cursor visits slot i once every PARTICLES_MAX spawns, in order, starting at
	// slot 0 for spawn k=0 (particlesInit's poolReset rewinds it there). Slot i therefore
	// holds whichever of {i, i+512, i+1024, ...} is the largest not exceeding total-1=561:
	//   i in [0, 49]    -> last write was k = i + 512   (the 50 recycling spawns)
	//   i in [50, 511]  -> last write was k = i          (never touched again -- still original)
	// This is the exact-index proof that recycling evicts the OLDEST entry, not an
	// approximation of it: every one of the 512 slots is checked, not a sample.
	int mismatches = 0;
	for (int i = 0; i < PARTICLES_MAX; i++) {
		float x = -1.0f, y, z, a;
		const bool alive = particlesGet(i, &x, &y, &z, &a);
		const float expected = (i < extra) ? (float)(PARTICLES_MAX + i) : (float)i;
		if (!alive || x != expected) mismatches++;
	}
	CHECK(mismatches == 0);
}

static void testSplashProducesEightToSixteenAcrossASpeedSweep(void)
{
	// Swept, not sampled once: negative, zero, small, large and very large impact speeds,
	// including a couple of fractional ones. Every one of these must land the spawned count
	// in the documented 8..16 range (docs/plan-particles.md section 3a).
	const float speeds[] = {
		-50.0f, -5.0f, -1.0f, -0.01f, 0.0f, 0.01f, 0.5f, 1.0f, 2.0f, 3.0f,
		4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 15.0f, 20.0f, 50.0f, 1000.0f,
	};
	const int n = (int)(sizeof(speeds) / sizeof(speeds[0]));

	int out_of_range = 0;
	for (int i = 0; i < n; i++) {
		particlesInit();   // isolate each case: fresh pool, fresh spawn counter
		particlesSpawnSplash(0.0f, 0.0f, 0.0f, speeds[i]);
		const int live = particlesLiveCount();
		if (live < 8 || live > 16) out_of_range++;
	}
	CHECK(out_of_range == 0);
}

static void testSplashIsDeterministic(void)
{
	particlesInit();
	particlesSpawnSplash(1.0f, 2.0f, 3.0f, 4.5f);
	const int count_a = particlesLiveCount();

	float ax[PARTICLES_MAX], ay[PARTICLES_MAX], az[PARTICLES_MAX], aa[PARTICLES_MAX];
	for (int i = 0; i < count_a; i++)
		CHECK(particlesGet(i, &ax[i], &ay[i], &az[i], &aa[i]) == true);

	// A full particlesInit() rewinds BOTH the pool and the spawn-hash counter (see
	// particles.c's poolReset), which is exactly what "two calls from the same pool state"
	// requires -- without that rewind this would be testing "the Nth splash after the 1st
	// differs from the 1st", a different and less useful claim.
	particlesInit();
	particlesSpawnSplash(1.0f, 2.0f, 3.0f, 4.5f);
	const int count_b = particlesLiveCount();

	CHECK(count_b == count_a);

	int mismatches = 0;
	for (int i = 0; i < count_a && i < count_b; i++) {
		float x, y, z, a;
		if (!particlesGet(i, &x, &y, &z, &a)) { mismatches++; continue; }
		if (x != ax[i] || y != ay[i] || z != az[i] || a != aa[i]) mismatches++;
	}
	CHECK(mismatches == 0);
}

static void testSplashParticlesStayNearSpawnPointAfterOneTick(void)
{
	particlesInit();
	particlesSpawnSplash(10.0f, 20.0f, 30.0f, 3.0f);
	particlesTick(0.01f);   // one small step -- a gross bug (wrong units, huge velocity) shows up fast

	const int count = particlesLiveCount();
	CHECK(count >= 8 && count <= 16);

	int too_far = 0;
	for (int i = 0; i < count; i++) {
		float x, y, z, a;
		if (!particlesGet(i, &x, &y, &z, &a)) { too_far++; continue; }
		const float dx = x - 10.0f, dy = y - 20.0f, dz = z - 30.0f;
		const float dist2 = dx * dx + dy * dy + dz * dz;
		if (dist2 > 25.0f) too_far++;   // 5 blocks, generously above the ~0.02-block real displacement
	}
	CHECK(too_far == 0);
}

static void testGetRejectsOutOfRangeIndex(void)
{
	particlesInit();
	float x, y, z, a;
	CHECK(particlesGet(-1, &x, &y, &z, &a) == false);
	CHECK(particlesGet(PARTICLES_MAX, &x, &y, &z, &a) == false);
	CHECK(particlesGet(PARTICLES_MAX + 1000, &x, &y, &z, &a) == false);
}

static void testGetWithNullOutParamsDoesNotCrash(void)
{
	particlesInit();
	const ParticleSpawnDesc d = {
		.x = 1, .y = 2, .z = 3, .vx = 0, .vy = 0, .vz = 0,
		.life_seconds = 5.0f, .size = 0.05f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
	};
	particlesSpawn(&d);

	CHECK(particlesGet(0, NULL, NULL, NULL, NULL) == true);   // must not crash
}

static void testInitAfterInitDoesNotInheritParticles(void)
{
	particlesInit();
	const ParticleSpawnDesc d = {
		.x = 1, .y = 1, .z = 1, .vx = 0, .vy = 0, .vz = 0,
		.life_seconds = 999.0f, .size = 0.05f, .rgba = PARTICLE_RGBA(1, 1, 1, 1),
	};
	particlesSpawn(&d);
	particlesSpawn(&d);
	particlesSpawn(&d);
	CHECK(particlesLiveCount() == 3);

	particlesExit();
	CHECK(particlesInit() == true);

	// A fresh session (join, leave, join again -- see particles.h's particlesInit comment)
	// must start with nothing live, not with whatever the previous session left behind.
	CHECK(particlesLiveCount() == 0);
}

static void testCapacityConstantMatchesTheDocumentedBudget(void)
{
	// particles.h's own header comment computes the 55,296-byte linear-heap cost against
	// PARTICLES_MAX == 512. If this ever silently changes, that comment (and the plan
	// section it cites) goes stale without anything else here catching it.
	CHECK(PARTICLES_MAX == 512);
}

int main(void)
{
	testInitStartsEmpty();
	testSpawnAddsOneParticle();
	testSpawnRefusesNonPositiveLife();
	testSpawnRefusesNullDesc();
	testTickMovesAndAppliesGravity();
	testLifeReachesExactlyZeroAndParticleDies();
	testTickClampsNegativeDt();
	testAlphaFadeMonotonicAndBounded();
	testSpawningPastCapacityRecyclesTheOldest();
	testSplashProducesEightToSixteenAcrossASpeedSweep();
	testSplashIsDeterministic();
	testSplashParticlesStayNearSpawnPointAfterOneTick();
	testGetRejectsOutOfRangeIndex();
	testGetWithNullOutParamsDoesNotCrash();
	testInitAfterInitDoesNotInheritParticles();
	testCapacityConstantMatchesTheDocumentedBudget();

	printf("particles self-test: %s %d checks\n", fails ? "FAIL" : "PASS", checks);
	return fails ? 1 : 0;
}
