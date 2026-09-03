// Blocksmith v1.8.9 — the general-purpose particle system.
//
// One shared, fixed-capacity pool of small world-space quads that several effects can spawn
// into — one renderer, several emitters, per docs/plan-particles.md section 2. The ask that
// made this module exist is water splash (plan section 3a, "the one named explicitly. Ship
// it."); the pool and the spawn API below are general enough that a later pass can add
// block-break debris (plan section 3b) or anything else without a second renderer.
//
// ── THIS MODULE IS STANDALONE ───────────────────────────────────────────────────────────
//
// Nothing in source/main.c, source/scene/player.c or source/scene/interact.c calls into
// this yet. See docs/plan-1.8.9-particles-integration.md for the exact call sites, with
// current line numbers, a later pass should add. Wiring it into the game is deliberately
// out of scope for this file — see that document's own header for why.
//
// ── Host-testability, and why particlesDraw is the one function missing on the host ──────
//
// particlesInit / particlesExit / particlesTick / particlesSpawn / particlesSpawnSplash /
// particlesGet / particlesLiveCount are pure pool bookkeeping and arithmetic, and they are
// compiled and actually run on the host by tests/particles_test.c — no <3ds.h>, no citro3d,
// nothing that exists only on a console. particlesDraw is the one function that has
// genuinely nothing to do off real hardware: it takes a C3D_Mtx and issues real GPU state
// changes and a real draw call. It exists only under __3DS__ — the exact split
// source/app/hw.h uses for hwInit (a pure-policy half always compiled, a hardware half
// behind the guard) rather than one function with two bodies stitched together by #ifdef.
// This is why particles.h itself never includes <citro3d.h> unguarded: this header is
// included by tests/particles_test.c on plain host gcc, and C3D_Mtx must not appear in a
// declaration that file has to parse.
//
// ── Two heaps, and which one everything below lives in ────────────────────────────────────
//
// The 512-slot Particle STATE pool (positions, velocities, life) below is a plain static
// array — about 20 KB of .bss. It is simulation state, never read by the GPU directly, so it
// is not linearAlloc'd and it has nothing to do with world/budget.h's 12 MB application-heap
// cap either (that cap governs the calloc'd Column/Chunk/LightColumn world store — see
// budget.h's own header comment — and this module allocates none of those).
//
// The GPU-visible VERTEX buffer particlesInit's __3DS__ half claims IS linearAlloc'd, exactly
// like the chunk mesh pool (chunk_render.c) and the sprite UI batch (gfx/sprite.c): 512
// particles x 4 verts x 24 bytes (sizeof(ParticleVertex), see particles.c) = 49,152 bytes,
// plus a 512 x 6 x 2 byte index buffer = 6,144 bytes — 55,296 bytes total. [reasoned, sized
// the same way sprite.c:47's SPRITE_MAX_QUADS was — a round cap clear of any plausible
// simultaneous count, not a frame-budget calculation this project's tooling can perform; see
// docs/plan-particles.md section 2 and section 4]. Measured free LINEAR heap this has to fit
// against: ~21.44 MB on Old 3DS at render distance 3 (its only option), ~17.44 MB on New 3DS
// at render distance 5 (its max) — both cross-checked independently against
// scene/render_dist.h's own shipped constants (RENDER_DIST_LINEAR_HEAP_OLD/NEW minus the mesh
// pool and its overhead). 55,296 bytes is a rounding error against either figure.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fixed pool capacity. See the header comment above for the byte cost this implies. A
// spawn past this cap does not grow the pool and does not refuse — it recycles the oldest
// live particle (see particlesSpawn's own comment) — so nothing here can overflow or
// corrupt, only visibly run out of "oldest" particles sooner.
#define PARTICLES_MAX 512

// Packs a colour the way the vertex attribute wants it, matching gfx/sprite.h's
// SPRITE_RGBA exactly (R in the low byte, A in the high byte) — the two modules share the
// same GPU_UNSIGNED_BYTE x4 attribute convention, so one packing macro doing it differently
// from the other would be a silent, per-module colour-channel swap waiting to happen.
// Alpha here is the particle's colour AT SPAWN; particlesTick/particlesGet fade it toward 0
// as the particle's life runs out — see particlesGet's comment.
#define PARTICLE_RGBA(r, g, b, a) \
	((uint32_t)(uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | \
	 ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(a) << 24))

// One particle to spawn. Deliberately general — not "a splash" or "a spark" — so a later
// emitter (block-break debris, bubbles, rain) can drive the same pool through this one
// entry point; particlesSpawnSplash below is a convenience built entirely on top of it, not
// a second code path.
typedef struct {
	float    x, y, z;         // spawn position, world block coordinates
	float    vx, vy, vz;      // initial velocity, blocks/second
	float    life_seconds;    // time to live; must be > 0 or the spawn is a no-op
	float    size;            // quad HALF-width, in blocks (0.05 = a 10 cm droplet)
	uint32_t rgba;            // colour at spawn (see PARTICLE_RGBA); alpha fades to 0
} ParticleSpawnDesc;

// Compiles the shader and claims the vertex/index buffers on the console; on the host,
// resets the pool and always succeeds (there is no GPU resource to fail to claim). Also
// clears every particle from any earlier session, so a fresh world join never inherits
// leftover particles from the one before it — see particles.c for why that reset lives
// here and not in particlesExit.
//
// False only on the console, only if the shader or a buffer failed to load; the module then
// degrades to "no particles" for the rest of the session — particlesSpawn* becomes a no-op
// and particlesDraw draws nothing — never a crash, the same contract every other GPU-backed
// module in this tree (crackoverlay.c, gfx/sprite.c) makes.
bool particlesInit(void);
void particlesExit(void);

// Advances every live particle by dt seconds: gravity, motion, life countdown. Pure CPU
// arithmetic over the static pool — no GPU calls, safe to call every frame on either build,
// and safe to call even if particlesInit was never called or failed (the pool starts, and
// stays, all-dead in that case). A negative dt is clamped to 0 rather than run backwards.
void particlesTick(float dt);

// Spawns one particle into the pool. A no-op if particlesInit has not succeeded, or if
// desc->life_seconds <= 0 (nothing would ever be visible) — validated here rather than left
// to produce a particle that instantly reads as dead, per this project's "validate input at
// the boundary" convention.
//
// RECYCLING: the pool never grows and a spawn never fails outright. It writes into a fixed
// ring position advanced by exactly one slot per spawn, unconditionally overwriting
// whatever was there — dead or still alive. Because the cursor visits every slot in a fixed
// cyclic order, a slot is only revisited after PARTICLES_MAX other spawns have happened
// since it was last written, which is "recycle the oldest" by construction, in O(1), with
// no scan and no allocation in the hot path — see tests/particles_test.c's
// testSpawningPastCapacityRecyclesTheOldest for the exact-index proof this really does what
// this comment says rather than merely capping a counter.
void particlesSpawn(const ParticleSpawnDesc* desc);

// Convenience wrapper over particlesSpawn for docs/plan-particles.md section 3a — the water
// splash. 8-16 particles [proposal, plan section 3a], count and outward speed both scaling
// with |impact_speed| (pass Body.vy at the moment of a BODY_DRY -> BODY_SURFACE/SUBMERGED
// transition — see docs/plan-1.8.9-particles-integration.md for the exact call site), pale
// and fading, scattered around (x, z) at height y with no texture sampled at all (plan
// section 3a: "a splash doesn't need to look like anything but a pale, fading dot").
//
// DETERMINISTIC, not random: each call's scatter is a pure hash of an internal spawn
// counter and each particle's index within the call (see particles.c's particleHash), the
// same positional-hash-over-a-mutable-stream choice interact.c's apple drop already makes
// and for the same reason — a stream's output depends on how many times it has already been
// called, which here would mean "how this splash looks depends on how many OTHER splashes
// happened first". Two calls with the same inputs from the same pool state produce
// bit-identical particles; see testSplashIsDeterministic.
void particlesSpawnSplash(float x, float y, float z, float impact_speed);

// Convenience wrapper over particlesSpawn for v1.8.17 WATER-FX task 3 -- the swim wake. Two
// small particles per call [proposal], drifting with the body's own horizontal velocity
// (vx, vz, blocks/second) plus a little scatter rather than firing outward the way a splash
// impact does: a wake trails the swimmer, it does not explode away from him. Smaller and
// paler than a splash droplet (see particlesSpawnWake's own comment in particles.c for the
// exact numbers), so the two effects read as different things on screen even though they
// share one renderer and one pool. The caller (scene/player.c) is the rate limiter -- this
// function does not know or care how often it is being called, the same division of labour
// particlesSpawnSplash already has with its own caller.
//
// DETERMINISTIC for the same reason particlesSpawnSplash is (see that function's own
// comment): a pure hash of an internal call counter and the particle's index within the
// call. A SEPARATE counter from the splash's, so a wake call and a splash call landing in
// the same frame cannot perturb each other's sequences.
void particlesSpawnWake(float x, float y, float z, float vx, float vz);

// Read-only snapshot of pool slot `index` (0..PARTICLES_MAX-1), for tests and for any future
// debug overlay. Returns false and touches no out-parameter if that slot is not currently
// alive. `alpha01` is the particle's current fade fraction — 1.0 at spawn, falling linearly
// to 0.0 as life runs out — the same quantity particlesDraw's __3DS__ half multiplies into
// the vertex colour's alpha channel, exposed here so a host test can check the fade curve
// without any GPU to render it with.
bool particlesGet(int index, float* x, float* y, float* z, float* alpha01);

// How many pool slots currently hold a live particle. 0..PARTICLES_MAX. O(n) over the pool,
// same cost class as particlesTick — fine to call from a debug overlay once a frame, not
// meant for a hot inner loop.
int particlesLiveCount(void);

#ifdef __3DS__

#include <citro3d.h>

// Draws every live particle as a camera-facing billboard quad through this module's own
// shader program, one draw call, same category of cost as crackOverlayDraw
// (scene/crackoverlay.h). No-op if particlesInit failed or nothing is alive.
//
// `view` is the frame's view matrix — the same one handed to crackOverlayDraw and
// highlightDraw. The projection is read from chunkRenderProjection() (scene/chunk_render.h)
// rather than passed in, for the reason crackoverlay.h gives for doing the same: a particle
// drawn through even a slightly different field of view does not sit where it was spawned.
//
// GPU state: binds its own program, attribute layout, TEV stage 0 (primary colour, no
// texture — see particles.c), the fog unit (claimed off, same as every other small pass in
// this tree) and translucent alpha blending, and depth-TESTS against the world's buffer
// (GPU_GREATER, matching chunk_render.c's own convention) without writing to it, since a
// blended quad writing depth would let later, less-transparent particles occlude earlier
// ones behind them for no correct reason. Everything this pass CHANGES from chunk_render.c's
// own baseline it puts back explicitly before returning (alpha blend, depth write mask,
// cull mode) — see particles.c for the exact three lines and why, matching the "put back
// only what would otherwise leak" discipline scene/crackoverlay.c and scene/chunk_render.c
// both already follow. Call after the world and after chunkRenderDraw specifically, so the
// depth buffer particles test against already holds real terrain.
void particlesDraw(const C3D_Mtx* view);

#endif   // __3DS__
