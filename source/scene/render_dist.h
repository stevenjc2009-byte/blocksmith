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
// 48x48 block area. MAX 2 is 5x5 columns, 80x80 blocks.
//
// MAX is 2 because of the mesh pool, which is the binding constraint and not a soft one: a
// three-tier vertex slab pool (512/1024/2048 face slots, 70/60/20 slots each — see
// scene/chunk_render.c's TIER_*_FACES/TIER_*_SLOTS) claimed once out of the linear heap at
// boot for the largest ring the setting allows, and the heap cannot be defragmented. A
// radius of 3 would be 49 columns, so 294 slots, which at this pool's measured average of
// ~28.8 KB a slot is roughly 8.7 MB — about twice what the setting costs today, out of a
// linear heap that also has to hold the atlas, the sprite batch and both framebuffers.
#define RENDER_DIST_MIN  1
#define RENDER_DIST_MAX  2

// Chunk mesh slots to claim for a given radius. Columns in the ring times the chunks in a
// column that hold blocks, which is measured rather than assumed: the shipped world meshes
// 40 chunks across 9 columns, so 4.44 per column, and caves are already counted in that
// number because cave walls are interior surface that has to be meshed. 6 per column is the
// margin over it, and a column can only ever have COLUMN_CHUNKS of them anyway.
#define RENDER_DIST_SLOTS_PER_COLUMN  6

// Slots the widest allowed ring needs. This is what MESH_SLOTS is, and therefore what the
// tiered pool costs at boot on every model: 150 slots split 70/60/20 across the three tiers,
// 70*512 + 60*1024 + 20*2048 face slots at 4 vertices of 8 bytes each, plus the one shared
// index buffer step 9.2b introduced — 4,448,256 bytes, and the console reports the pool as
// 4.24 MB. It was 9.40 MB when every slot was a uniform 2048-face slab, before step 9.2c.
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

// The default for a console. `new_3ds` comes from APT_CheckNew3DS in main.c.
//
// They differ because the cost of the extra ring is a GPU cost, and the GPU is the one
// thing this project cannot measure before it runs on hardware (see
// blocksmith-lesson-azahar-gpu-timing). An Old 3DS gets the radius that has been running
// at 59.83 fps all along; a New 3DS, with roughly twice the GPU clock and four times the
// memory, gets the wider one and is the machine the option was written for.
int renderDistDefault(bool new_3ds);
