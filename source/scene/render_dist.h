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
// ── v1.8.5 re-measured this, because the numbers above are the old curve's ────────────────
//
// Those four readings were taken while the fog was an exponential handed to FogLut_Exp. v1.8.5
// fills the LUT directly (renderDistFogTable, below) and every half_vis in the project moved,
// so leaving them here unqualified would have left the ceiling resting on arithmetic that no
// longer describes the build. Re-measured the same way — the clamp moved and nothing else, this
// time by shadowing this header with a copy whose MAX is 5:
//
//     radius 2 -> 12.07 blocks
//     radius 3 -> 14.34 blocks   +18.7 %, and the near plane reaches its cap here
//     radius 4 -> 14.37 blocks   + 0.2 % for 64 % more columns
//     radius 5 -> 14.41 blocks
//
// The conclusion did not just survive, it hardened. Radius 4 bought 4.8 % under the exponential
// and buys 0.20 % now, because the old curve was still being shaped a little by the near field
// and the new one is shaped entirely by knot 1 — which stops moving the moment `near` hits
// RENDER_DIST_NEAR_MAX at radius 3. Past here the ring is flat: 3, 4 and 5 all put knot 1 at
// 20.6767 blocks and differ only in how much geometry they submit behind the same fade.
//
// NOT changed by any of this: the GPU argument in the next paragraph, which is still the reason
// the DEFAULTS did not move. Nothing here has run on a console.
//
// NOT PROVEN, and it cannot be proven here: the GPU cost. Radius 3 submits roughly 1.9x the
// triangles of radius 2 before culling, and this machine has no GPU to measure on — Azahar's
// C3D_GetDrawingTime returns a constant 0.249 ms, so the emulator does not model it either.
// Whether an Old 3DS holds 60 fps at radius 3 is a hardware question. That is exactly why the
// DEFAULTS below did not move with this ceiling.
// ── v1.8.5: 3 → 5, and the whole paragraph above is now HISTORY, not the live argument ────
//
// Everything above was written while the fog was the PICA200's hardware LUT, and its conclusion
// — that radius 4 buys 0.20 % of visibility for 64 % more columns — was correct AND was the
// reason this ceiling stayed at 3 through two versions. That ceiling has now moved because the
// thing holding it down was removed, not because the argument was wrong.
//
// The LUT is 128 entries indexed by 1/w, so its knots bunch towards the camera; with `near`
// pinned at RENDER_DIST_NEAR_MAX from radius 3 onward, knot 1 sits at 20.6767 blocks at every
// radius and the fade past it is one unshapeable segment. v1.8.5 moves the fade off the LUT
// entirely, onto a ramp texture sampled from linear eye depth carried in texcoord1 (gfx/fogramp.h,
// and GPU_NO_FOG everywhere now — leaving both on would double-fog). Re-measured the same way:
//
//     radius 3 -> 28.5 blocks
//     radius 4 -> 38.0 blocks    +9.5 blocks
//     radius 5 -> 47.5 blocks    +9.5 blocks
//
// Linear at 9.5 blocks per radius step, against 0.03 before. Widening the ring now buys what a
// player can actually see, which it did not before, and THAT is what makes this ceiling worth
// raising rather than any change of opinion about the numbers above.
//
// This is a CEILING, not a default, and not what either console gets. RENDER_DIST_MAX_OLD and
// _NEW below are the per-console limits and renderDistMaxFor() is what every caller asks; this
// number only says how wide the mesh pool's .bss tables can index, because MESH_SLOTS is derived
// from it and s_slots/s_vis_list/s_vis_depth are sized by that. An Old 3DS still stops at 3.
//
// Cost of the lift, measured off blocksmith.elf with arm-none-eabi-nm -S rather than derived:
// s_slots 25,088 → 61,952, s_vis_list and s_vis_depth 1,568 → 3,872 each, s_hzn_cols 780 → 1,644,
// plus main.c's column-span tables s_col_queued/s_col_in/s_col_asked at +864/+1056/+1056. Total
// .bss 661,592 → 706,904 = +45,312 bytes, on both consoles, spent so a New 3DS can reach 5.
//
// STILL NOT PROVEN, and still not provable here: the GPU cost. Radius 5 submits 2.47x the
// columns of radius 3, and Azahar's C3D_GetDrawingTime returns a constant 0.249 ms, so the
// emulator does not model it. Whether a New 3DS holds a frame rate at 5 is a hardware question,
// which is why the DEFAULTS below still did not move.
#define RENDER_DIST_MIN  1
#define RENDER_DIST_MAX  5

// ── v1.8.5: the ceiling stops being one number for both consoles ──────────────────────────
//
// These two are the per-model ceilings the option will be clamped to. They are declared here,
// ahead of the code that uses them, because the arithmetic that justifies them is checked by
// tests/mesh_pool_bytes_test.c NOW — before anything is allowed to move — and a constant that
// a test can reach is worth more than a number in a commit message.
//
// The numbers, and where each came from. Both linear-heap sizes are READINGS off a booted
// console (a CXI carrying the shipping ExHeader, one build per memory mode), not arithmetic:
//
//     Old 3DS   linear heap 33,554,432 B    free after boot 22,479,872 B
//     New 3DS   linear heap 67,108,864 B    free after boot 56,034,304 B
//
// The New 3DS figure is twice libctru's own LINEAR_HEAP_SIZE_CAP because app/heapsplit.c
// overrides the split; the Old 3DS figure is libctru's untouched default, because the same
// policy declines on a grant that small (a fixed 64 MB linear heap was built and booted in
// Legacy mode and libctru's guard fired svcBreak — the console never reached chunkRenderInit).
//
// Against that, the pool's cost at each radius (scene/mesh_pool_sizing.h, plus the 1,874,944 B
// of screen and GPU buffers that share the linear heap and are claimed before it):
//
//     radius 3     9,199,616 + 1,874,944 = 11,074,560     fits both
//     radius 4    25,976,832 + 1,874,944 = 27,851,776     fits both
//     radius 5    46,948,352 + 1,874,944 = 48,823,296     New only, 17.44 MB spare
//     radius 6    72,114,176 + 1,874,944 = 73,989,120     fits neither, over by 6.56 MB
//
// So 5 is not a chosen number, it is the largest radius the New 3DS's linear heap holds, and 6
// is excluded by a measurement rather than by taste. mesh_pool_bytes_test.c asserts both halves
// of that — that 5 fits and that 6 does not — so the day someone widens a tier or grows
// MeshVertex, the ceiling fails the build instead of failing to boot.
//
// WHY OLD 3DS STAYS AT 3 even though radius 4 fits its heap with 5.7 MB to spare: memory is not
// the binding cost there, GPU time is, and radius 4 submits 1.64x the columns of radius 3 on a
// GPU that this project has never been able to measure (Azahar's C3D_GetDrawingTime returns a
// constant 0.249 ms). Raising the Old 3DS ceiling on that evidence would be a guess. Raising the
// New 3DS one is a different proposition only because it is opt-in — see RENDER_DIST_DEFAULT_NEW
// below, which does NOT move: the ceiling is what a player can reach, the default is what every
// unattended console ships with, and those two have never carried the same risk.
//
// NOT MEASURED, and it is the honest gap in all of this: what radius 5 does to the frame rate.
// It submits 2.47x the columns of radius 3. No hardware has run it. It is offered as an option a
// player opts into and can back out of with one shoulder press, not as a default, precisely
// because the GPU half of the question cannot be answered on this machine.
#define RENDER_DIST_MAX_OLD  3
#define RENDER_DIST_MAX_NEW  5

// The ordering of the three ceilings, stated so that an edit to any one of them fails the
// build instead of quietly inverting the model. Checked by BOTH the console build and the host
// suite, same as the RENDER_DIST_SLOTS_PER_COLUMN assert further down.
//
// Note what is deliberately NOT asserted: that RENDER_DIST_MAX is at least as large as either
// of these. It is not, today (3 against a MAX_NEW of 5), and that gap is the entire reason the
// two kinds of ceiling are separate constants. RENDER_DIST_MAX_OLD/_NEW are what each console's
// linear HEAP holds; RENDER_DIST_MAX is what the mesh pool was ALLOCATED for. renderDistMaxFor()
// below is where the two are reconciled, and it takes the smaller.
_Static_assert(RENDER_DIST_MAX_OLD <= RENDER_DIST_MAX_NEW,
               "a New 3DS must never be offered a NARROWER ring than an Old 3DS: it has twice "
               "the linear heap and roughly twice the GPU clock, so an inversion here is an "
               "edit slip and not a policy anyone would choose");
_Static_assert(RENDER_DIST_MIN <= RENDER_DIST_MAX_OLD,
               "the narrowest per-console ceiling must still be reachable from the floor, or "
               "renderDistClampFor() would be clamping into an empty range");

// Non-pool linear-heap use, measured at the `screenInit+metrics` mark of the boot probe on both
// consoles: 67,108,864 - 65,233,920 on a New 3DS, and 33,554,432 - 31,679,488 on an Old 3DS,
// which are the same number. Screen framebuffers and citro3d's GPU command buffers, claimed
// before the mesh pool. The ceiling arithmetic has to clear this as well as the pool itself, or
// it would be sizing the pool against a heap it does not have all of.
#define RENDER_DIST_LINEAR_OVERHEAD_BYTES  1874944u

// The linear heap each console actually has, as read off a booted console rather than derived.
// Used only by the host suite, to check the ceilings above against the heap they have to fit in.
#define RENDER_DIST_LINEAR_HEAP_OLD  33554432u
#define RENDER_DIST_LINEAR_HEAP_NEW  67108864u

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

	// v1.8.5. Fog strength: 1.0 is the solved curve, 0.0 is no fog at all, and values between
	// scale the whole fade toward clear. It is NOT a density any more — nothing exponential is
	// left in this file — but the name is kept because scene/chunk_render.c's FOG_DENSITY
	// override macro spells this field, and `-DFOG_DENSITY=0.0f` is the red arm that proves the
	// fog checks can fail. See renderDistFogTable for what the number multiplies.
	float fog_density;

	// The curve's ONE shape parameter, and the reason this stopped being a density. The LUT can
	// only be shaped at its knots; knot 1 is the last one outside the load boundary, so the
	// visibility the ramp is allowed to reach there is the whole design. Solved, not chosen —
	// it is the largest value at which the fade is still complete by `boundary`.
	//
	// Still readable as the diagnostic it used to be: this IS the visibility at `first_knot`.
	// v1.8.5 only changed which direction the arrow points, from derived to derived-from.
	float knot_vis;

	// Diagnostics, so the numbers can be asserted instead of trusted.
	float boundary;       // worst-case distance to the load boundary, blocks
	float first_knot;     // first LUT knot past the far plane, blocks
	float half_vis;       // distance at which the world is half faded — what you can see
	bool  fog_hides;      // does the effective curve reach the target by `boundary`?
} RenderDist;

// The complete set of numbers for a radius. Radius is clamped into
// [RENDER_DIST_MIN, RENDER_DIST_MAX] rather than rejected: this is a setting a player pokes
// at, and one that silently did nothing at the ends would read as a broken button.
RenderDist renderDistFor(int radius);

// ── v1.8.5. Which ceiling applies to THIS console, and the clamp that respects it ─────────
//
// The largest radius a console may be set to, in columns.
//
// WHY `new_3ds` IS A PARAMETER AND NOT A QUESTION THIS FUNCTION ASKS. It would be one libctru
// call to ask APT_CheckNew3DS here, and doing that would take this entire module off the host
// suite. The file's opening comment is explicit that it is "deliberately free of <3ds.h> so the
// host suite can test it", and APT_CheckNew3DS drags <3ds.h> in — at which point renderDistFor,
// renderDistFogTable, renderDistVisibility and every number they produce stop being checkable
// anywhere but on hardware this project has repeatedly been unable to measure on. The seam is
// the same one renderDistDefault() has always used, and app/hw.c is the one place that asks:
// hwInit() calls APT_CheckNew3DS once at boot and hwIsNew3ds() is what callers read (app/hw.h
// records why that consolidation happened — three separate call sites that could disagree).
//
// The seam is also what lets the host check BOTH consoles from one binary, via hwTestSetNew3ds()
// or by passing the flag directly. That matters more than usual here: no New 3DS has ever run
// this code, so a self-detecting version would leave the New 3DS branch completely unexercised.
//
// WHY IT RETURNS THE MINIMUM of the per-model ceiling and RENDER_DIST_MAX, which is
// load-bearing rather than defensive:
//
//   * RENDER_DIST_MAX_OLD/_NEW are what each console's linear HEAP holds. Measured, and gated
//     by tests/mesh_pool_bytes_test.c.
//   * RENDER_DIST_MAX is what the mesh pool was actually ALLOCATED for, because MESH_SLOTS is
//     RENDER_DIST_MAX_SLOTS and scene/chunk_render.c's chunkRenderInit still claims that at
//     boot, on every model, from the compile-time constant.
//
// Today those answers differ — 3 and 5 — and returning 5 for a New 3DS now would hand it a
// radius whose slots do not exist. That failure is not a dropped frame: acquireSlot returns
// NULL, chunk_render.c counts a refusal, and the chunk is never meshed at all, which is the
// terrain-hole bug v1.7.0 already paid a release for. Taking the min makes this function
// correct at today's RENDER_DIST_MAX of 3 (both models pinned to 3) and correct BY CONSTRUCTION
// the moment the arenas are sized from the selected radius and RENDER_DIST_MAX is lifted to 5
// (New 5, Old 3) — one constant moves and nothing here needs remembering.
int renderDistMaxFor(bool new_3ds);

// A radius clamped into [RENDER_DIST_MIN, renderDistMaxFor(new_3ds)]: the console-aware
// counterpart of the clamp renderDistFor() applies internally.
//
// Two clamps rather than one, because they answer two different questions and only one of them
// is a policy:
//
//   renderDistFor()'s clamp guards the POOL. It is an unconditional backstop on a compile-time
//   constant and it must stay unconditional — anything that reaches it has already got past
//   whatever policy its caller applied, including callers that have no idea which console this
//   is (tests/fogramp_test.c evaluates radii above the ceiling on purpose).
//
//   this one guards the SETTING: what the options slider offers, what a hand-edited options.ini
//   may ask for, what a shoulder press is allowed to step to. That is per-console.
//
// The two cannot disagree, and that is checked rather than argued: renderDistMaxFor() never
// exceeds RENDER_DIST_MAX, so this function's output is always a fixed point of renderDistFor()'s
// clamp — renderDistFor(renderDistClampFor(r, n)).radius == renderDistClampFor(r, n) for every r
// on both models. tests/render_dist_ceiling_test.c checks that across the whole range, because
// the day they do disagree is the day the slider reads one radius while the renderer draws
// another and nothing anywhere reports it.
int renderDistClampFor(int radius, bool new_3ds);

// citro3d's FogLut_CalcZ, duplicated because the host suite cannot link citro3d and the
// whole design here depends on where this function puts its knots. `index01` is the LUT
// position, 0 at the far plane and 1 at the near plane.
float renderDistLutZ(float index01, float near_plane, float far_plane);

// Floats FogLut_FromArray consumes: 128 knot VALUES then 128 per-knot DIFFERENCES, where
// entry 128+i is value[i+1] - value[i]. Not a guess — read out of the shipped libcitro3d.a
// with arm-none-eabi-objdump (see renderDistFogTable in the .c for the decoded layout).
#define RENDER_DIST_FOG_KNOTS   128
#define RENDER_DIST_FOG_FLOATS  (2 * RENDER_DIST_FOG_KNOTS)

// v1.8.5. Fills the table for FogLut_FromArray directly, instead of asking FogLut_Exp for an
// exponential and accepting whatever the LUT can represent of it.
//
// This is worth a function rather than a call because of what it buys, which was measured on
// the host before it was written. The hardware's 128 knots are pinned by the near and far
// planes and cannot be moved; an exponential that reaches the target by the load boundary is
// therefore also heavily fogged everywhere INSIDE the first knot, where nothing required it to
// be. Filling the knots directly keeps the near field clear and spends the whole fade on the
// one segment that has to carry it. half_vis at radius 3 goes 9.4950 -> 14.3358 blocks, +51 %,
// with no shader change, no TexEnv change and nothing touched in the alpha path.
//
// Deliberately host-testable, like everything else here: it is plain arithmetic over floats and
// knows nothing about C3D_FogLut. The console side is one FogLut_FromArray call.
void renderDistFogTable(const RenderDist* rd, float out[RENDER_DIST_FOG_FLOATS]);

// Visibility (1 = clear, 0 = fully fogged) the *hardware* produces at a given distance, which
// is not the same as the curve anyone would draw on paper: the GPU stores 128 samples and
// interpolates linearly between them in the 1/w domain. Past the first knot there are no
// samples at all, so this is the function that decides whether the load boundary is hidden.
//
// v1.8.5: it now builds renderDistFogTable's own output and reads THAT, rather than evaluating
// a curve that happens to be the same one the LUT was built from. The two had to agree by hand
// before; now they cannot disagree, because there is only one table. If they had drifted,
// half_vis, knot_vis and fog_hides would all have quietly become fiction and every check in
// world_test.c that rests on them would have been checking nothing.
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

// v1.8.5. A default has to be a value the player could also have SELECTED, on the NARROWER
// console, or it is not a default — it is a number the clamp moves at first boot.
//
// Asserted against RENDER_DIST_MAX_OLD and not RENDER_DIST_MAX_NEW deliberately: this constant
// is the New 3DS default today, but the range check that matters is the one that still holds if
// it is ever reused as a shared default, and the Old ceiling is the binding one there.
//
// This does NOT weaken the decision written above it. That decision is that the default must not
// be SPELLED as a ceiling, and renderDistDefault() still returns RENDER_DIST_DEFAULT_NEW rather
// than renderDistMaxFor(). All this says is that the default lies inside the clamp range, which
// is a strictly weaker and entirely different claim — and the only one a compile-time check can
// make about it.
_Static_assert(RENDER_DIST_DEFAULT_NEW >= RENDER_DIST_MIN &&
               RENDER_DIST_DEFAULT_NEW <= RENDER_DIST_MAX_OLD,
               "the New 3DS default must be a radius a player could have picked by hand on "
               "either console; a default outside the clamp range is a value that gets "
               "silently rewritten the first time the console boots");

// The default for a console. `new_3ds` comes from APT_CheckNew3DS in main.c.
//
// They differ because the cost of the extra ring is a GPU cost, and the GPU is the one
// thing this project cannot measure before it runs on hardware (see
// blocksmith-lesson-azahar-gpu-timing). An Old 3DS gets the radius that has been running
// at 59.83 fps all along; a New 3DS, with roughly twice the GPU clock and four times the
// memory, gets the wider one and is the machine the option was written for.
//
// v1.8.5: the answer now goes out through renderDistClampFor(), so the value this hands to a
// first boot is guaranteed to be one the same clamp would have accepted from options.ini. It is
// a no-op at today's constants — the _Static_assert above is what keeps it one — and it is here
// so that the default and the setting can never be governed by two different ranges. It does not
// raise the default toward the ceiling; see RENDER_DIST_DEFAULT_NEW above for why it must not.
int renderDistDefault(bool new_3ds);
