// Step 7.7. Render distance as a setting, and everything that has to move with it.
//
// The setting is one number — the radius, in columns of 16 blocks, of the ring that gets
// meshed and drawn. Four other things are functions of it and none of them can be left
// behind when it changes:
//
//   * the generated ring, which is always one column wider (main.c)
//   * the chunk mesh pool, which has to be big enough for the largest ring allowed
//   * the fog, which has to be opaque before the load boundary or the boundary is visible
//   * the near clip plane, because the near plane is what decides where the fog LUT can
//     put a knot, and therefore how far away the fade can be shaped at all
//
// That last one is the whole reason this file exists instead of two more #defines. The fog
// LUT is 128 entries indexed by 1/w and the first knot past the far plane sits at roughly
// 128 x near — 12.03 blocks at near 0.1. *Everything beyond the first knot is one linear
// segment*, so a fog curve cannot be shaped out at 32 blocks; it can only be interpolated
// there. Raising the render distance without raising `near` therefore buys geometry the fog
// still has to hide, and the player sees no further. Raising `near` moves the knot out.
//
// Deliberately free of <3ds.h> so the host suite can test it. The one thing here that is a
// console question — which model we are running on, and so what the default should be —
// lives in main.c, which asks APT_CheckNew3DS and passes the answer in.
#pragma once

#include <stdbool.h>

#include "world/chunk.h"
#include "world/world.h"

// Bounds of the setting, in columns. MIN 1 is what steps 5 and 6 shipped: 3x3 columns, a
// 48x48 block area. MAX 3 is 7x7 columns, 112x112 blocks.
//
// v1.6.0 task 12 raised MAX from 2 to 3. The old comment here said MAX was 2 "because of the
// mesh pool", and that reasoning was wrong on both halves:
//
//   * the memory. It compared the pool against the 12 MB world/budget.h heap. The pool is
//     linearAlloc, a different heap entirely, and the only hardware reading on record shows
//     25.83 MB of linear free. The re-cut pool for radius 3 is 7,593,984 bytes (7.24 MB)
//     against today's 4,448,256 (4.24 MB) — see scene/chunk_render.c's TIER_*_SLOTS, which
//     is where the measurement backing that cut is written down.
//   * the arithmetic. "~28.8 KB a slot" was the average slot size of the OLD uniform pool;
//     the tiered pool's average is what the tier split makes it, which is the thing being
//     chosen, not a constant to multiply by.
//
// What actually stops this at 3 is the fog, which is this file's own argument (see the header
// comment above) finally measured. RENDER_DIST_NEAR_MAX pins the near plane at 0.18, the near
// plane pins the first LUT knot, and the knot pins how far the fade can be shaped — so the
// visible distance saturates. Evaluating renderDistFor() with only this clamp moved, half_vis
// (the distance at which the world is half faded, i.e. what the player can actually see) goes:
//
//     radius 2 -> 7.80 blocks
//     radius 3 -> 9.49 blocks   +21.7 %, and the near plane reaches its cap here
//     radius 4 -> 9.95 blocks   + 4.8 % for 64 % more columns
//     radius 5 -> 10.46 blocks
//
// So 3 is where the ring stops buying sight and starts buying geometry the fog hides. Radius 4
// would cost 6.6 MB more pool and 64 % more chunks for under 5 % more visible world.
//
// NOT PROVEN, and it cannot be proven here: the GPU cost. Radius 3 submits roughly 1.9x the
// triangles of radius 2 before culling, and this machine has no GPU to measure on — Azahar's
// C3D_GetDrawingTime returns a constant 0.249 ms, so the emulator does not model it either.
// Whether an Old 3DS holds 60 fps at radius 3 is a hardware question. That is exactly why the
// DEFAULTS below did not move with this ceiling.
#define RENDER_DIST_MIN  1
#define RENDER_DIST_MAX  3

// Chunk mesh slots to claim for a given radius. Columns in the ring times the chunks in a
// column that hold blocks, which is measured rather than assumed: the shipped world meshes
// 40 chunks across 9 columns, so 4.44 per column, and caves are already counted in that
// number because cave walls are interior surface that has to be meshed. 6 per column is the
// margin over it, and a column can only ever have COLUMN_CHUNKS of them anyway.
//
// v1.6.0 task 12 re-measured this against the real mesher over 9,000 columns (8 seeds x 5
// areas far apart in the world, 37,682 chunk meshes): the mean is 4.187 meshed chunks per
// column and NO column in the whole sample meshed more than 5. So 6 is still margin over the
// measurement and not a number that has quietly gone tight — it is what makes the widest ring
// 294 slots against a worst observed simultaneous occupancy of 242.
//
// ── v1.7.0: 6 WENT UNDER, and the margin stops being a measurement ───────────────────────
//
// The paragraph above is exactly the trap it claims not to be. Its measurement was taken on
// the generator v1.7.0 replaced, and the density field moved it. Re-measured with the real
// mesher on the real new worldgen (4 seeds x 81 radius-3 ring positions on an 11-column
// stride, 324 rings, each ring in its own world with a one-column margin so no chunk meshes
// against unloaded-as-air):
//
//     worst meshed chunks in ONE COLUMN         7      <- this constant was 6
//     worst meshed chunks in one radius-3 ring  298    <- the pool held 294
//     mean meshed chunks per column             4.685  (was 4.187)
//
// The 7 held on all four seeds independently, so it is not one freak column. A slot the pool
// cannot supply is not a degraded frame: acquireSlot returns NULL, chunk_render.c counts a
// refusal, and the chunk is never meshed at all — a hole in the terrain that walking away and
// back does not repair. Greedy merging did NOT absorb this; merging made each chunk SMALLER
// (chunks over 512 faces fell 103 -> 87, over 1024 fell 8 -> 3) while the density field's
// caves made MORE chunks non-empty, and it is the COUNT that sizes the pool, not the size.
//
// So this stops being a measured margin and becomes the arithmetic ceiling. A column holds
// COLUMN_CHUNKS chunks and cannot hold more, so at COLUMN_CHUNKS per column the pool covers
// every possible ring of every possible terrain and no future generator can put it back
// under. That is the same reasoning world/jobq.h already applies to JOBQ_CAP, which clears
// the structural 49 * COLUMN_CHUNKS = 392 rather than the measured 242 — this file was the
// odd one out.
//
// Rejected: 7, which matches the measurement exactly and leaves no margin; 7 plus a
// hand-picked fudge, which has no principled stopping point and would need re-measuring on
// the next generator anyway. Going to the ceiling instead of to 7 costs 49 extra S-tier
// slots, 802,816 bytes — paid once, against a bug class that has now cost a release.
#define RENDER_DIST_SLOTS_PER_COLUMN  COLUMN_CHUNKS

// The guarantee spelled out, so that trimming the constant above to reclaim memory fails the
// build instead of quietly reintroducing terrain holes at the widest ring. Checked by BOTH
// the console build and the host suite, since this header is deliberately <3ds.h>-free.
_Static_assert(RENDER_DIST_SLOTS_PER_COLUMN >= COLUMN_CHUNKS,
               "the mesh pool must cover a ring in which EVERY chunk of every column is "
               "meshed; anything below COLUMN_CHUNKS per column is a measurement that a "
               "worldgen change can invalidate, which is what happened in v1.7.0");

// Slots the widest allowed ring needs. This is what MESH_SLOTS is, and therefore what the
// tiered pool costs at boot on every model. At RENDER_DIST_MAX 3 that is 49 columns, so 392
// slots, split 256/120/16 across the three tiers (see scene/chunk_render.c's TIER_*_SLOTS for
// the measurement behind that split): 256*512 + 120*1024 + 16*2048 face slots at 4 vertices of
// 8 bytes each, plus the one shared index buffer step 9.2b introduced — 9,199,616 bytes, 8.77 MB.
//
// It was 7,593,984 bytes (7.24 MB) at 294 slots split 158/120/16, before v1.7.0 took the
// per-column figure to the arithmetic ceiling: +1,605,632 bytes, all of it S tier, which is
// the cheapest slot there is (16,384 bytes against L's 65,536). Before that it was 4,448,256
// bytes (4.24 MB) at RENDER_DIST_MAX 2 with 150 slots split 70/60/20, and 9.40 MB when every
// slot was a uniform 2048-face slab, before step 9.2c.
//
// NOT VERIFIED ON HARDWARE: the one hardware reading on record shows 25.83 MB of linear heap
// free, so 8.77 MB fits it with room, but that reading was taken at 4.24 MB of pool and no
// console has run any build since v1.2.5.
#define RENDER_DIST_MAX_COLUMNS  ((2 * RENDER_DIST_MAX + 1) * (2 * RENDER_DIST_MAX + 1))
#define RENDER_DIST_MAX_SLOTS    (RENDER_DIST_MAX_COLUMNS * RENDER_DIST_SLOTS_PER_COLUMN)

// Where the fade has to have finished. 0.5% visibility is "the same colour as the sky" to
// within a value the 8-bit framebuffer cannot represent as different.
#define RENDER_DIST_TARGET_VIS  0.005f

// The furthest the near plane may go. The near plane's *corner* is 1.5917 x near away from
// the eye at this field of view and aspect (sqrt(1 + tan^2(fovy/2) + tan^2(fovx/2))), and
// the player's half-width is 0.3 blocks, so a near plane whose corner reaches past 0.3
// would let a player pressed against a wall see through it. 0.18 puts the corner at 0.286.
#define RENDER_DIST_NEAR_MAX  0.18f

// Everything the renderer needs for one setting value. Built by renderDistFor(), which is
// pure arithmetic — no state, no allocation, safe to call whenever.
typedef struct {
	int   radius;         // the setting itself, in columns
	int   area_radius;    // generated ring, always radius + 1
	int   slots;          // mesh pool slots this radius needs

	float near_plane;     // view near, blocks
	float far_plane;      // view far, blocks
	float fog_density;    // FogLut_Exp's density
	float fog_gradient;   // FogLut_Exp's gradient

	// Diagnostics, so the numbers can be asserted instead of trusted.
	float boundary;       // worst-case distance to the load boundary, blocks
	float first_knot;     // first LUT knot past the far plane, blocks
	float knot_vis;       // visibility the curve reaches at that knot
	float half_vis;       // distance at which the world is half faded — what you can see
	bool  fog_hides;      // does the effective curve reach the target by `boundary`?
} RenderDist;

// The complete set of numbers for a radius. Radius is clamped into
// [RENDER_DIST_MIN, RENDER_DIST_MAX] rather than rejected: this is a setting a player pokes
// at, and one that silently did nothing at the ends would read as a broken button.
RenderDist renderDistFor(int radius);

// citro3d's FogLut_CalcZ, duplicated because the host suite cannot link citro3d and the
// whole design here depends on where this function puts its knots. `index01` is the LUT
// position, 0 at the far plane and 1 at the near plane.
float renderDistLutZ(float index01, float near_plane, float far_plane);

// Visibility (1 = clear, 0 = fully fogged) the *hardware* produces at a given distance,
// which is not the same as the exponential curve FogLut_Exp was asked for: the GPU stores
// 128 samples of it and interpolates linearly between them in the 1/w domain. Past the
// first knot there are no samples at all, so this is the function that decides whether the
// load boundary is hidden, and the exponential is only an input to it.
float renderDistVisibility(const RenderDist* rd, float blocks);

// The default a New 3DS boots at, and deliberately NOT RENDER_DIST_MAX.
//
// This used to be spelled `RENDER_DIST_MAX` inside renderDistDefault(), which meant raising
// the ceiling silently raised the default with it. v1.6.0 task 12 raised the ceiling to 3 and
// left this at 2 on purpose. The reasoning, because it is a behaviour choice and not an
// oversight:
//
//   * a default is what ships to everyone unattended; a ceiling is what a player opts into
//     and can back out of with one shoulder press. They do not carry the same risk and should
//     not be the same constant.
//   * the binding cost of the wider ring is GPU time, and GPU time cannot be measured on this
//     machine at all (see the note on RENDER_DIST_MAX above). Radius 3 has never run on any
//     hardware. This file's own rule for a default is that it is the value with a measurement
//     behind it — "the radius that has been running at 59.83 fps all along" is how the Old 3DS
//     default was justified, and radius 2 is the only New 3DS value with any standing at all.
//   * raising a ceiling costs an unattended player nothing. Raising a default makes every New
//     3DS first boot the experiment.
//
// So the wider ring is available on both models and automatic on neither. Move this to 3 when
// there is a frame-time reading from a real New 3DS at radius 3 and not before — it is one
// constant and one test line (world_test.c's testRenderDist).
#define RENDER_DIST_DEFAULT_NEW  2

// The default for a console. `new_3ds` comes from APT_CheckNew3DS in main.c.
//
// They differ because the cost of the extra ring is a GPU cost, and the GPU is the one
// thing this project cannot measure before it runs on hardware (see
// blocksmith-lesson-azahar-gpu-timing). An Old 3DS gets the radius that has been running
// at 59.83 fps all along; a New 3DS, with roughly twice the GPU clock and four times the
// memory, gets the wider one and is the machine the option was written for.
int renderDistDefault(bool new_3ds);
