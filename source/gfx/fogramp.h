// v1.9.0. The fog ramp: distance fog computed from LINEAR eye-space depth and applied through a
// TEV stage, replacing the PICA200's fixed-function fog unit.
//
// ── why the hardware fog unit had to go ──────────────────────────────────────────────────
//
// The PICA200's fog is a 128-entry LUT indexed by 1/w. The knots therefore sit in the 1/w
// domain and bunch up against the camera: with the near plane at RENDER_DIST_NEAR_MAX (0.18,
// and it is NOT moving — see that macro in scene/render_dist.h, it has a second justification
// that has nothing to do with fog) the first knot outside the far plane lands at 20.6767
// blocks, and EVERYTHING beyond it is one straight line the table cannot bend. So the fade
// could be shaped up to ~20.7 blocks and nowhere further, and the distance at which the world
// is half faded saturated:
//
//     radius 3 -> 14.3358 blocks
//     radius 4 -> 14.3651 blocks   +0.20 % for 64 % more columns
//     radius 5 -> 14.4065 blocks
//
// Raising the render distance bought geometry the fog hid. That ceiling is not a tuning
// problem, it is where the knots are, and the only way past it is to stop indexing by 1/w.
//
// ── what replaces it ─────────────────────────────────────────────────────────────────────
//
// The vertex shader emits a fog coordinate that is an affine function of the eye-space depth:
//
//     u = depth * inv_range + bias        (fogParams.x and .y, both shaders)
//
// and a 128x8 A8 texture on unit 1 turns u into a fog factor, which TEV stage 1 blends the
// terrain colour toward the sky colour by. Two properties matter and both are load bearing:
//
//   * EYE-SPACE DEPTH, not a Euclidean sqrt(x^2+y^2+z^2). Greedy meshing merges coplanar runs
//     (world/mesher.c mergeRun/mergeCandidate), so a single quad can be sixteen blocks across.
//     Eye-space depth is linear across a plane, and the PICA's perspective-correct varying
//     interpolation reconstructs a world-linear quantity EXACTLY at every fragment — so a
//     merged quad fades identically to sixteen unmerged ones. A per-vertex radial distance is
//     NOT linear across a plane and would flatten the fade toward the middle of big runs,
//     which reads as banding on exactly the ground quads greedy meshing produces most of.
//   * The coordinate rides on texcoord1. The atlas already relies on perspective-correct
//     texcoord interpolation across merged quads (gfx/atlas.c: GPU_REPEAT in U, u running past
//     16 on a merge, and it renders correctly), so this is the one varying channel on this
//     GPU whose behaviour across a large merged quad is evidenced by the shipping game rather
//     than assumed. That is why the factor is not routed through the vertex colour instead:
//     outclr.xyz is three copies of one scalar and two of them are genuinely spare, but every
//     merged quad in the game today has a CONSTANT colour across it (merging requires flat AO),
//     so nothing here has ever exercised colour interpolation over a long run.
//
// ── shape ────────────────────────────────────────────────────────────────────────────────
//
// Smoothstep between `start` and `end`, both derived from the load boundary, which is the one
// distance that constrains anything: the fade has to be finished before the boundary or the
// player sees the edge of the loaded world. There is no LUT-knot term left anywhere in it, so
// half_vis is now a straight multiple of the radius and raising the render distance raises what
// the player can see, which is the entire point of the change.
//
// Deliberately free of <3ds.h>, like scene/render_dist.c and for the same reason: the whole
// fade — the curve, the texel table, the sampler the GPU will run, the resulting visibility at
// a distance — is checked on the host in tests/fogramp_test.c instead of by squinting at a
// console. gfx/fogtex.c is the console half and is four calls long.
#pragma once

#include <stdint.h>

// The ramp texture. 128 texels across the fade is 0.78 % of it per texel, against an 8-bit
// framebuffer that cannot show a step smaller than 1/255 — so the sampled ramp is finer than
// anything that can be drawn. 8 is the PICA200's minimum texture dimension, and all 8 rows are
// identical so the v coordinate the shader emits does not have to mean anything.
#define FOGRAMP_W  128
#define FOGRAMP_H  8

// Where the fade finishes, as a fraction of the load boundary. Not 1.0: the boundary is already
// the worst case (see rd.boundary in scene/render_dist.c — the ring is centred on the player's
// column, so standing on that column's edge puts the far boundary at radius*16 and the near one
// at nothing), and 5 % of margin costs 5 % of half_vis while making "the boundary is hidden"
// true with room rather than exactly.
#define FOG_END_FRAC    0.95f

// Where it begins, as a fraction of where it finishes. Everything inside `start` is completely
// clear. This is the only free number in the file: it trades near-field clarity against how
// gradual the fade is, and it comes straight off half_vis (half_vis = (start+end)/2 for a
// smoothstep, so 0.25 gives 0.625*end). Chosen at 0.25 so a quarter of the visible world is
// untouched by fog while the fade still has three quarters of the distance to happen over.
#define FOG_START_FRAC  0.25f

// The affine map from eye-space depth in blocks to the ramp's texture coordinate, plus the two
// distances it was built from so a test can assert on them directly.
typedef struct {
	float start;      // blocks; at and inside this the fog factor is 0
	float end;        // blocks; at and outside this it is 1
	float inv_range;  // texcoord per block          -> fogParams.x
	float bias;       // texcoord at zero distance   -> fogParams.y
} FogShape;

// `boundary_blocks` is the worst-case distance to the load boundary, CHUNK_DIM * radius.
// Passed in rather than derived from a radius so the host suite can evaluate radii above
// RENDER_DIST_MAX, which renderDistFor() clamps away.
//
// `strength` is scene/chunk_render.c's FOG_DENSITY override, kept working through the new
// mechanism: 1 is the solved fade, 0 leaves the world completely clear, and values between
// stretch the fade outward proportionally. At 0 both inv_range and bias come out exactly 0,
// so every fragment samples texel 0 and the fog factor is exactly 0 — "no fog through this
// exact code path", which is what the red arm has always meant.
FogShape fogShapeFor(float boundary_blocks, float strength);

// The ramp itself, on 0..1. Smoothstep: zero derivative at BOTH ends, so there is no visible
// ring where the fog switches on and no hard edge where it finishes — the two artefacts a
// plain linear ramp has.
float fogRampCurve(float t);

// Texel i of the ramp texture, i.e. the curve sampled at the texel CENTRE (i + 0.5) / W and
// quantised to 8 bits the way the .png does. This is the reference tools/make_fog_ramp.py has
// to reproduce, and tests/fogramp_test.c asserts the built build/fogramp.t3x against it texel
// by texel — the same "two copies of one number, cross-checked" arrangement world/
// atlas_uv_shader_test.c uses for uvScale, and the only reason the duplication is safe.
uint8_t fogRampTexel(int i);

// The GPU's sampler, modelled exactly: GPU_LINEAR filtering between texel centres,
// GPU_CLAMP_TO_EDGE outside them, and the 8-bit texel values divided by 255. Everything a
// fragment's fog factor depends on is in here, so a number this returns is a claim about the
// hardware's arithmetic and not about a curve on paper.
float fogRampSample(const uint8_t texels[FOGRAMP_W], float u);

// Visibility at a distance: 1 is clear, 0 is the sky colour. Same sense as
// renderDistVisibility() in scene/render_dist.c, so the two are directly comparable.
float fogVisibility(const FogShape* s, const uint8_t texels[FOGRAMP_W], float blocks);

// The distance at which visibility is 0.5 — what the player can actually see. Bisected over
// fogVisibility rather than solved from the curve, for the same reason render_dist.c bisects:
// the function is a quantised staircase, and inverting the smooth curve it was built from
// would answer a question about a different function.
float fogHalfVis(const FogShape* s, const uint8_t texels[FOGRAMP_W]);
