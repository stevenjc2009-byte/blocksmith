#include "gfx/particles.h"

#include <math.h>

// ── The particle STATE pool — pure CPU data, no GPU type anywhere in this half of the file ─
//
// Deliberately NOT the GPU vertex format. A particle's simulation state (position, velocity,
// remaining life) and its on-screen representation (a coloured quad) are different shapes for
// different reasons — physics.h's Body keeps the same separation between simulation state and
// what gets drawn — and folding them together would mean every tick touched fields the GPU
// never reads. particlesDraw's __3DS__ half converts a live Particle into a ParticleVertex
// quad fresh every frame, the same way gfx/sprite.c turns UI calls into SpriteVertex quads
// fresh every frame rather than keeping the two in one struct.
typedef struct {
	float x, y, z;          // world position, blocks
	float vx, vy, vz;       // velocity, blocks/second
	float life;              // seconds remaining; <= 0 means this slot is dead
	float life_total;        // life_seconds at spawn, for the fade fraction in particlesGet
	float size;               // quad half-width, blocks
	uint32_t rgba;             // colour at spawn; alpha fades with life/life_total
} Particle;

static Particle s_pool[PARTICLES_MAX];
static int      s_next;             // ring cursor: the NEXT slot particlesSpawn will write
static uint32_t s_spawn_counter;    // advances once per particlesSpawnSplash call, never per-particle
// v1.8.17 WATER-FX task 3. A SEPARATE counter from s_spawn_counter -- see particlesSpawnWake's
// own comment on why sharing one stream between two emitters would make each one's scatter
// depend on how many times the OTHER had already been called.
static uint32_t s_wake_counter;
static bool     s_ready;

// Matches world/physics.h's PLAYER_GRAVITY (-28.0f) exactly rather than inventing a second,
// unrelated fall rate — [proposal]: a dropped particle should arc the way anything else in
// this world's gravity field does. Not #included from physics.h, to keep this module's only
// dependency the standard library (see particles.h's header comment on why this file owns
// itself outright); the number is small enough that duplicating it with a citation is safer
// than a coupling this module has no other reason to take on.
#define PARTICLE_GRAVITY (-28.0f)

// Resets every slot to dead and rewinds the ring cursor and the spawn-hash counter. Called
// from BOTH particlesInit halves below so a fresh world join never inherits particles (or
// spawn-counter state, which would change what testSplashIsDeterministic-style determinism
// promises) left over from an earlier session — see particles.h's particlesInit comment.
static void poolReset(void)
{
	for (int i = 0; i < PARTICLES_MAX; i++)
		s_pool[i].life = 0.0f;
	s_next = 0;
	s_spawn_counter = 0;
	s_wake_counter = 0;
}

// A small, fast integer mix (Thomas Wang's 32-bit hash), used ONLY to turn
// (spawn call index, particle index within that call) into deterministic scatter for
// particlesSpawnSplash. Not a general-purpose PRNG and not meant to be one — it is a pure
// function of its two inputs, the same "positional, not a stream" choice world/rng.h's
// rngHash2/rngHash3 make and interact.c's apple-drop comment explains: a stream's output
// depends on how many times it has already been called, which here would mean one splash's
// look depending on how many OTHER splashes happened before it. Kept local rather than
// pulled from world/rng.h so this module takes on no dependency beyond the standard library.
static uint32_t particleHash(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u + b * 0x85EBCA77u + 0xC2B2AE3Du;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

void particlesTick(float dt)
{
	// A negative dt must not run the simulation backwards — nothing upstream promises a
	// non-negative frame time, and a frame-timer wraparound or a caller's arithmetic slip
	// is exactly the kind of input this project's "validate at the boundary" rule exists
	// for. Clamped rather than rejected: a tick that silently does nothing for one bad
	// frame is a better failure than a particle stream running its arc in reverse.
	if (dt < 0.0f) dt = 0.0f;

	for (int i = 0; i < PARTICLES_MAX; i++) {
		Particle* p = &s_pool[i];
		if (p->life <= 0.0f) continue;   // dead slot: skip, do not resurrect it

		// Semi-implicit (symplectic) Euler: gravity updates velocity first, THEN velocity
		// moves position. Matches the order world/physics.h's own bodyStep comment
		// describes ("applies gravity to vy, then moves by velocity * dt") — one falling-
		// body integration rule in this codebase, not two that could silently disagree.
		p->vy += PARTICLE_GRAVITY * dt;
		p->x  += p->vx * dt;
		p->y  += p->vy * dt;
		p->z  += p->vz * dt;

		p->life -= dt;
		if (p->life < 0.0f) p->life = 0.0f;   // pins at exactly dead, never negative
	}
}

void particlesSpawn(const ParticleSpawnDesc* desc)
{
	if (!s_ready || !desc) return;
	if (!(desc->life_seconds > 0.0f)) return;   // nothing would ever be visible; see particles.h

	Particle* p = &s_pool[s_next];
	p->x = desc->x;  p->y = desc->y;  p->z = desc->z;
	p->vx = desc->vx; p->vy = desc->vy; p->vz = desc->vz;
	p->life = desc->life_seconds;
	p->life_total = desc->life_seconds;
	p->size = desc->size > 0.0f ? desc->size : 0.02f;   // a zero/negative size would draw nothing
	p->rgba = desc->rgba;

	s_next++;
	if (s_next >= PARTICLES_MAX) s_next = 0;
}

void particlesSpawnSplash(float x, float y, float z, float impact_speed)
{
	if (!s_ready) return;

	float speed = fabsf(impact_speed);

	// 8-16 particles [proposal, docs/plan-particles.md section 3a], scaling with impact
	// speed so a belly-flop scatters more than a toe-dip — "intensity for free", per the
	// plan: Body.vy at the transition is already computed by the caller, nothing new here.
	int count = 8 + (int)(speed * 2.0f);
	if (count < 8)  count = 8;
	if (count > 16) count = 16;

	const uint32_t call = s_spawn_counter++;

	for (int i = 0; i < count; i++) {
		const uint32_t h = particleHash(call, (uint32_t)i);
		// Two independent-ish fractions in [0, 1) out of one 32-bit hash: low half for
		// angle, high half for everything speed/life related, so a fully deterministic
		// spawn still looks scattered rather than every particle sharing one derived value.
		const float a = (float)(h & 0xFFFFu) / 65536.0f;
		const float b = (float)((h >> 16) & 0xFFFFu) / 65536.0f;

		const float angle       = a * 6.28318530718f;
		const float horiz_speed = 0.6f + b * 1.2f;    // 0.6..1.8 blocks/s outward [proposal]
		const float rise        = 0.8f + b * 1.0f;    // 0.8..1.8 blocks/s upward  [proposal]

		const ParticleSpawnDesc d = {
			.x = x, .y = y, .z = z,
			.vx = cosf(angle) * horiz_speed,
			.vy = rise,
			.vz = sinf(angle) * horiz_speed,
			.life_seconds = 0.35f + b * 0.25f,          // 0.35..0.6 s [proposal]
			.size = 0.05f,                                // 10 cm droplet [proposal]
			// Pale, fading dot, no texture (plan section 3a) — a light, slightly blue-white
			// with a starting alpha well under opaque, since several overlap on one splash.
			.rgba = PARTICLE_RGBA(0xE8, 0xF4, 0xFF, 0xC0),
		};
		particlesSpawn(&d);
	}
}

// v1.8.17 WATER-FX task 3 -- the swim wake. See particles.h's own comment for the division of
// labour with the caller (scene/player.c decides WHEN and WHERE and how often; this function
// decides only what a wake particle looks like).
void particlesSpawnWake(float x, float y, float z, float vx, float vz)
{
	if (!s_ready) return;

	const uint32_t call = s_wake_counter++;

	for (int i = 0; i < 2; i++) {
		// Same Thomas Wang mix particlesSpawnSplash uses, on the SEPARATE s_wake_counter
		// stream -- see this function's header comment and s_wake_counter's own comment for
		// why the two effects do not share one.
		const uint32_t h = particleHash(call, (uint32_t)i);
		const float a = (float)(h & 0xFFFFu) / 65536.0f;
		const float b = (float)((h >> 16) & 0xFFFFu) / 65536.0f;

		const float angle   = a * 6.28318530718f;
		const float scatter = 0.15f + b * 0.25f;   // 0.15..0.4 blocks/s outward [proposal]

		const ParticleSpawnDesc d = {
			.x = x, .y = y, .z = z,
			// A fraction of the swimmer's own velocity, so the wake visibly trails him,
			// plus the same kind of hash-derived scatter the splash uses so two particles
			// spawned in the same call do not sit on top of each other. 0.3 [proposal]: the
			// wake reads as "left behind by the swimmer", not "flung off him" -- a splash
			// already owns the "flung off" look via its own, much larger outward speed.
			.vx = vx * 0.3f + cosf(angle) * scatter,
			.vy = 0.3f + b * 0.3f,    // 0.3..0.6 blocks/s -- a light bob, not a splash's leap
			.vz = vz * 0.3f + sinf(angle) * scatter,
			.life_seconds = 0.3f + b * 0.2f,   // 0.3..0.5s [proposal] -- shorter than a splash
			.size = 0.035f,                     // smaller than a splash's 0.05 droplet [proposal]
			// Foam-white rather than the splash's pale blue-white (0xE8F4FF), and a lower
			// starting alpha (0x90 against the splash's 0xC0) -- a wake is meant to read as a
			// faint trail, not a burst, even though both share this one renderer.
			.rgba = PARTICLE_RGBA(0xF0, 0xF8, 0xFF, 0x90),
		};
		particlesSpawn(&d);
	}
}

bool particlesGet(int index, float* x, float* y, float* z, float* alpha01)
{
	if (index < 0 || index >= PARTICLES_MAX) return false;
	const Particle* p = &s_pool[index];
	if (p->life <= 0.0f) return false;

	if (x) *x = p->x;
	if (y) *y = p->y;
	if (z) *z = p->z;
	if (alpha01) {
		// life_total is guaranteed > 0 here: the only writer, particlesSpawn, already
		// refuses a desc with life_seconds <= 0, so this can never divide by zero.
		float a = p->life / p->life_total;
		if (a < 0.0f) a = 0.0f;
		if (a > 1.0f) a = 1.0f;
		*alpha01 = a;
	}
	return true;
}

int particlesLiveCount(void)
{
	int n = 0;
	for (int i = 0; i < PARTICLES_MAX; i++)
		if (s_pool[i].life > 0.0f) n++;
	return n;
}

#ifndef __3DS__

// Host half of particlesInit/particlesExit — see particles.h's particlesInit comment. There
// is no GPU resource to claim or fail to claim on the host, so this always succeeds; its only
// job is the same one the __3DS__ half's poolReset() call does, for the same reason (a fresh
// session must not inherit particles or spawn-hash state from an earlier one).
bool particlesInit(void)
{
	poolReset();
	s_ready = true;
	return true;
}

void particlesExit(void)
{
	s_ready = false;
}

#endif   // !__3DS__

#ifdef __3DS__

// ── Everything below this line is 3DS-only: shader load, GPU buffers, the draw call. ──────
// Nothing above it has run on real hardware either — see the bottom of this file — but
// everything above it at least runs, for real, on the host every time the test suite does.

#include <3ds.h>
#include <citro3d.h>

#include "scene/chunk_render.h"   // chunkRenderProjection() only — see particles.h's particlesDraw comment

#include "particle_shbin.h"       // generated by picasso from source/shaders/particle.v.pica

// Position, texcoord, colour — 24 bytes, exactly the shape docs/plan-particles.md section 2
// proposes (and exactly SpriteVertex's shape, gfx/sprite.c:8-16, deliberately not shared
// with it — see particles.h's header comment on why this is its own struct in its own
// module). All-float-plus-uint32_t is 4-byte aligned on every field with no packing
// decisions to get wrong, in contrast to world/mesh_vertex.h's packed layout — the exact
// property that made SpriteVertex and CrackVertex safe on real hardware from the start.
typedef struct {
	float    x, y, z;
	float    u, v;
	uint32_t rgba;
} ParticleVertex;

_Static_assert(sizeof(ParticleVertex) == 24,
	"ParticleVertex must stay 24 bytes -- particles.h's linear-heap budget (55,296 B for "
	"PARTICLES_MAX=512) is computed against exactly this size; a padding change here makes "
	"that comment wrong without anything else telling you so.");

static bool s_shader_ready;   // see crackoverlay.c's identical static for why this outlives a session

static DVLB_s*         s_dvlb;
static shaderProgram_s s_program;
static int             s_uloc_viewproj;

static ParticleVertex* s_verts;     // linearAlloc'ed, PARTICLES_MAX * 4 verts, rewritten every draw
static uint16_t*       s_indices;   // linearAlloc'ed, PARTICLES_MAX * 6, built once, never rewritten

bool particlesInit(void)
{
	poolReset();

	if (!s_shader_ready) {
		s_dvlb = DVLB_ParseFile((u32*)particle_shbin, particle_shbin_size);
		if (!s_dvlb) return false;
		shaderProgramInit(&s_program);
		shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);
		s_uloc_viewproj = shaderInstanceGetUniformLocation(s_program.vertexShader, "viewProj");
		s_shader_ready = true;
	}

	s_verts   = (ParticleVertex*)linearAlloc(sizeof(ParticleVertex) * PARTICLES_MAX * 4);
	s_indices = (uint16_t*)linearAlloc(sizeof(uint16_t) * PARTICLES_MAX * 6);
	if (!s_verts || !s_indices) {
		if (s_verts)   linearFree(s_verts);
		if (s_indices) linearFree(s_indices);
		s_verts = NULL;
		s_indices = NULL;
		return false;
	}

	// The same constant six-index-per-quad pattern as gfx/sprite.c, built once: every quad's
	// indices are fixed relative to its own four vertices, so there is nothing here that
	// depends on which particle ends up in which slot.
	for (int q = 0; q < PARTICLES_MAX; q++) {
		const uint16_t v = (uint16_t)(q * 4);
		uint16_t* idx = &s_indices[q * 6];
		idx[0] = v;     idx[1] = (uint16_t)(v + 1); idx[2] = (uint16_t)(v + 2);
		idx[3] = v;     idx[4] = (uint16_t)(v + 2); idx[5] = (uint16_t)(v + 3);
	}
	GSPGPU_FlushDataCache(s_indices, sizeof(uint16_t) * PARTICLES_MAX * 6);

	s_ready = true;
	return true;
}

void particlesExit(void)
{
	if (s_verts)   linearFree(s_verts);
	if (s_indices) linearFree(s_indices);
	s_verts = NULL;
	s_indices = NULL;
	s_ready = false;

	// The shader is deliberately NOT freed here. shaderProgramFree does not null
	// program->vertexShader while citro3d separately caches a pointer to the last program
	// it bound, so freeing and rebuilding this static across a rejoin is the exact
	// use-after-free that hard-crashes the console — already paid for once in this project
	// and carried as the same static-across-sessions pattern in scene/crackoverlay.c and
	// scene/highlight.c, for the identical reason.
}

// Builds this frame's vertex buffer from every live particle, camera-facing (billboarded),
// and flushes the written range. Returns the number of QUADS written (0..PARTICLES_MAX).
//
// Billboarding: `view` transforms world space into camera space, so the camera's own local
// +X and +Y axes, expressed in WORLD space, are the first two ROWS of `view` (the inverse of
// an orthonormal rotation is its transpose, and the transpose of "rows are world axes" is
// "columns are world axes" — reading view's rows directly is exactly that transpose already
// done). This is the standard technique for a camera-facing quad, independent of any content
// in this project; the row-major, row-dot-vector convention it relies on is confirmed by this
// codebase's own shaders (source/shaders/crack.v.pica and highlight.v.pica's dp4 blocks: row
// i of a matrix dotted with the vector produces output component i).
static int buildVertices(const C3D_Mtx* view)
{
	const float right_x = view->r[0].x, right_y = view->r[0].y, right_z = view->r[0].z;
	const float up_x    = view->r[1].x, up_y    = view->r[1].y, up_z    = view->r[1].z;

	int n = 0;
	for (int i = 0; i < PARTICLES_MAX; i++) {
		const Particle* p = &s_pool[i];
		if (p->life <= 0.0f) continue;

		float alpha01 = p->life / p->life_total;
		if (alpha01 < 0.0f) alpha01 = 0.0f;
		if (alpha01 > 1.0f) alpha01 = 1.0f;

		const uint32_t base_alpha = (p->rgba >> 24) & 0xFFu;
		const uint32_t faded_alpha = (uint32_t)((float)base_alpha * alpha01 + 0.5f);
		const uint32_t rgba = (p->rgba & 0x00FFFFFFu) | (faded_alpha << 24);

		const float s = p->size;
		ParticleVertex* v = &s_verts[n * 4];

		// bottom-left, bottom-right, top-right, top-left of the billboard, matching the
		// same fan-split winding gfx/sprite.c's spriteQuad and the world atlas both use —
		// winding does not matter here (GPU_CULL_NONE, see particlesDraw), but a consistent
		// order does, because the index buffer built in particlesInit assumes it.
		v[0].x = p->x - right_x * s - up_x * s;
		v[0].y = p->y - right_y * s - up_y * s;
		v[0].z = p->z - right_z * s - up_z * s;
		v[0].u = 0.0f; v[0].v = 0.0f; v[0].rgba = rgba;

		v[1].x = p->x + right_x * s - up_x * s;
		v[1].y = p->y + right_y * s - up_y * s;
		v[1].z = p->z + right_z * s - up_z * s;
		v[1].u = 1.0f; v[1].v = 0.0f; v[1].rgba = rgba;

		v[2].x = p->x + right_x * s + up_x * s;
		v[2].y = p->y + right_y * s + up_y * s;
		v[2].z = p->z + right_z * s + up_z * s;
		v[2].u = 1.0f; v[2].v = 1.0f; v[2].rgba = rgba;

		v[3].x = p->x - right_x * s + up_x * s;
		v[3].y = p->y - right_y * s + up_y * s;
		v[3].z = p->z - right_z * s + up_z * s;
		v[3].u = 0.0f; v[3].v = 1.0f; v[3].rgba = rgba;

		n++;
	}

	if (n > 0)
		GSPGPU_FlushDataCache(s_verts, (size_t)n * 4u * sizeof(ParticleVertex));

	return n;
}

void particlesDraw(const C3D_Mtx* view)
{
	if (!s_ready) return;   // failed/absent init degrades to "no particles", not a crash

	const int quads = buildVertices(view);
	if (quads == 0) return;   // nothing live: skip the draw call entirely, not just the work

	C3D_BindProgram(&s_program);

	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // v0 position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);          // v1 texcoord (unused by TEV below;
	                                                      // written for a future textured
	                                                      // particle type, see particles.h)
	AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // v2 colour

	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, s_verts, sizeof(ParticleVertex), 3, 0x210);

	// TEV stage 0: REPLACE with the primary (per-vertex) colour. No texture bound, no
	// texture unit spent — plan section 3a: "a splash doesn't need to look like anything
	// but a pale, fading dot", so there is nothing here for a texture to add. Texture unit
	// 2 stays completely free for a later, genuinely textured particle type.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	// TEV stage 1 and the fog unit, explicitly OWNED rather than inherited — the same
	// discipline scene/crackoverlay.c, scene/highlight.c and gfx/sprite.c all follow, for
	// the same reason: nothing in this tree calls GPU_NO_FOG except each pass that needs
	// it, and a leaked stage 1 fades whatever's left of this draw toward the sky colour by
	// however much texcoord1 happens to hold, silently.
	C3D_TexEnv* fog = C3D_GetTexEnv(1);
	C3D_TexEnvInit(fog);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);

	// Straight alpha blend, same equation gfx/sprite.c and chunk_render.c's transparent
	// pass both use, so a splash fades the same way the rest of this project's translucent
	// geometry does rather than inventing a second blend curve.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

	// Depth-TESTED against the world (GPU_GREATER, chunk_render.c's own compare direction,
	// pipelineBind), so a splash behind a wall does not draw through it, but NOT
	// depth-WRITTEN: a blended quad writing depth would let one particle's near face block
	// a farther, equally-translucent one behind it for no correct reason — the standard
	// "translucent geometry does not write depth" rule.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);

	// Billboards face the camera by construction (buildVertices), so there is no back face
	// to cull; GPU_CULL_NONE rather than a winding rule that would have to agree with a
	// billboard's necessarily camera-relative winding.
	C3D_CullFace(GPU_CULL_NONE);

	C3D_Mtx vp;
	Mtx_Multiply(&vp, chunkRenderProjection(), view);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_viewproj, &vp);

	C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, s_indices);

	// Put back the three pieces of state this pass CHANGED from chunk_render.c's own
	// baseline (pipelineBind: depth GPU_WRITE_ALL, cull GPU_CULL_BACK_CCW, blend
	// (ONE, ZERO, ONE, ZERO) — chunk_render.c's own transparent pass restores the same
	// blend tuple at the end of ITS draw for the identical reason) — this pass does not
	// control what runs after it in the frame, unlike scene/crackoverlay.c and
	// scene/highlight.c, which can rely on chunk_render.c redoing all of this at the START
	// of its NEXT call because nothing else runs between them and the next chunk draw.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

// NOT VERIFIED FROM HERE. Nothing in this __3DS__ half has been compiled by devkitARM as
// part of a full console build, linked, or run — see
// docs/plan-1.8.9-particles-integration.md for what WAS done (a standalone devkitARM
// single-TU compile and a picasso pass over the shader) and what that does and does not
// prove. The billboard axes standing up on real hardware, the splash reading as "a pale,
// fading dot" and not as sixteen overlapping hard-edged squares, and the depth/blend
// hand-back genuinely leaving nothing leaked for whatever draws next in a real frame are
// all VISUAL or on-hardware claims this pass cannot make from here — this project's own
// standing rule is that a visual claim needs a look at the actual pixels, and no screenshot
// exists or could be produced without wiring this module in, which this file deliberately
// does not do.

#endif   // __3DS__
