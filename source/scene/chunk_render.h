// Chunk meshes on the GPU: one vertex buffer and one draw call per chunk.
//
// Buffers come from a pool claimed once at init and recycled in place, never freed
// and re-allocated per remesh. That is not premature: the linear heap is the only
// GPU-visible memory on this console, it cannot be defragmented, and a mesher that
// allocates per edit would fragment it into uselessness within a few minutes of
// building. See code-vault/wiki/blocksmith/blocksmith-plan.md, step 3.3.
#pragma once

#include <3ds.h>
#include <citro3d.h>

#include "scene/mesh_pool_sizing.h"
#include "scene/render_dist.h"
#include "world/water.h"
#include "world/worldgen.h"
#include "world/world.h"

// Per-slot capacity. A pathological 16³ checkerboard would need 12,288 faces, which
// no terrain produces; 2,048 faces is comfortably past a fully exposed chunk surface
// (1,536) and keeps a slot at 88 KB. A chunk that will not fit is counted and skipped
// rather than half-drawn.
//
// MESH_SLOT_FACES/VERTS/INDICES themselves now live in scene/mesh_pool_sizing.h, included
// above — that file is deliberately free of <3ds.h> so the host suite can size a pool
// without pulling in this header's citro3d dependency. Nothing outside chunk_render.h/.c
// refers to the three names, so there is exactly one definition of them, not two.

// Step 9.2b: this is also the size of the ONE index buffer every slot now draws from — see
// s_shared_indices in chunk_render.c. It used to size MESH_SLOTS separate linearAlloc's.

// Step 7.7. The pool is sized for the *largest* render distance the setting allows, not for
// the current one, and it is claimed once at boot. That is not laziness about resizing: the
// linear heap is the only GPU-visible memory on this console and it cannot be defragmented,
// so a pool that grew when the player raised the setting would be exactly the
// allocate-and-free churn the pool exists to avoid. The cost is that a console left on the
// default distance carries slots it never fills — at the default radius 2 that is 25 columns,
// so 200 of the 392 slots can be claimed and 192 cannot, and step 9.2b's shared 24 KB index
// buffer means an unfilled slot now wastes only its vertex buffer (16 KB in the S tier,
// 65 KB in L) rather than 88 KB. The exact idle cost therefore depends on which tiers stay
// empty; the pool's total is worked out in scene/render_dist.h. The benefit is that the
// setting can move at any moment, on any model, without an allocation that can fail.
//
// ── v1.8.5: this is now the CEILING, not the pool ─────────────────────────────────────────
//
// Everything above still describes the *arrays*, and only the arrays. MESH_SLOTS is the
// compile-time widest ring any console the binary runs on may select, and it sizes the four
// plain-.bss tables in scene/chunk_render.c that are indexed by slot — s_slots, s_vis_list,
// s_vis_depth (and, by columns rather than slots, s_hzn_cols). Those stay compile-time on
// purpose: taking all four from the radius-3 ring to the radius-5 one costs 42,336 bytes of
// FCRAM on both models, and making them dynamic would be a large refactor of the hottest file
// in the renderer to save 41 KB.
//
// What is NO LONGER sized from this constant is the part that actually costs memory: the three
// linearAlloc tier arenas. Those are 9,199,616 bytes at radius 3 and 46,948,352 at radius 5 —
// and an Old 3DS has a 33,554,432-byte linear heap, so a pool sized at the compile-time
// ceiling would stop an Old 3DS booting the moment the ceiling moved past 4. chunkRenderInit
// now takes the radius to size for and claims arena for exactly that many slots; the slots
// past it exist as .bss table entries with a NULL `verts` and are never handed out. See
// chunkRenderInit below, and s_pool_slots in the .c.
#define MESH_SLOTS         (RENDER_DIST_MAX_SLOTS)

// The sky colour, in one place because step 6.4's fog has to be the same colour as the
// background it fades into. Fog that does not match the clear colour reads as a grey sheet
// hung in front of the sky instead of as distance, and one constant living in two files is
// exactly how that drifts apart. Two byte orders because the two registers disagree:
// C3D_RenderTargetClear takes RGBA8 and the PICA's fog colour register is 0x00BBGGRR.
// Change one, change the other.
#define SKY_CLEAR_RGBA8  0x102A33FF
#define SKY_FOG_BGR      0x00332A10

// Loads the shader, the atlas and the buffer pool. False if any of it failed. Leaves the
// renderer at RENDER_DIST_MIN; main.c sets the console's default straight after.
//
// v1.8.5. `max_radius` is the widest render distance THIS console will ever be allowed to
// select — the per-model ceiling, not the player's current setting — and it is what the three
// linearAlloc tier arenas are sized from. One int, passed in, for three reasons:
//
//   * the pool is claimed once and never resized (see the comment on MESH_SLOTS above and the
//     one on s_shared_indices in the .c), so the number it needs is a ceiling and not a
//     setting. Sizing from the player's selection would mean re-allocating on a slider press,
//     which is exactly the linear-heap churn the pool exists to avoid.
//   * main.c calls this BEFORE it loads options.ini, so the selection does not exist yet.
//     The model does: hwInit() has already run.
//   * the per-model ceiling is scene/render_dist.h's policy, not this file's. Taking the
//     answer rather than a `bool new_3ds` keeps the renderer out of that decision, and keeps
//     this function callable at any radius by a test.
//
// Clamped into [RENDER_DIST_MIN, RENDER_DIST_MAX] and then into what the .bss tables can
// index, so a wrong argument costs memory or draw distance — never a write past an array.
bool chunkRenderInit(int max_radius);
void chunkRenderExit(void);

// Step 7.7. Sets the render distance, in columns, and rebuilds the two things that are
// functions of it: the fog LUT and the projection. Clamped into range by renderDistFor.
//
// This does NOT resize the mesh pool (it is claimed for RENDER_DIST_MAX at boot) and it does
// not touch the streaming ring — main.c owns that half, because the ring is where columns are
// generated, unloaded and queued, and a distance change has to re-seed it.
void chunkRenderSetDistance(int radius);

// The numbers currently in force, for the report and for main.c's ring loops. Never NULL.
const RenderDist* chunkRenderDistance(void);

// v1.8.0 task 22b. Points the renderer at the water simulation so chunkRenderBuild can ask it
// for the flow levels inside the chunk it is about to mesh, via waterFillScratch. NULL — which
// is the state before main.c calls this — means every water block meshes as a full cube, which
// is exactly v1.7.1's output and exactly what every host test binary that does not link water.c
// keeps producing.
//
// A pointer rather than a #include-and-reach-in because the mesher must not gain a link
// dependency on water.c: mesher.c is in six host binaries and water.c in one. The levels travel
// as plain bytes in MeshScratch, so meshChunk never learns that a WaterSim exists.
void chunkRenderSetWater(const WaterSim* sim);

// v1.8.8 biome tint. Points the renderer at the world's generator so chunkRenderBuild can ask
// it which biome each column of the chunk it is about to mesh sits in, and write the matching
// palette row into the scratch's tint band.
//
// NULL -- the state before main.c calls this, and the state of every host binary -- means the
// band is never filled, every vertex carries tint row 0 (the identity), and the frame is
// byte-for-byte what it was before v1.8.8.
//
// A pointer set from outside, for the same layering reason chunkRenderSetWater gives just
// above: the renderer must not know which module owns the world's generator, only that
// something does. The tint travels to the mesher as plain bytes in MeshScratch, so meshChunk
// still never learns that a WorldGen -- or a biome -- exists.
void chunkRenderSetGen(const WorldGen* gen);

// v1.8.9 day/night. Points the renderer at the world clock's current time of day, so
// pipelineBind's dayLevel upload and the fog/clear colour can track the cycle instead of
// staying pinned to full day. Call once a frame, before the eyes are drawn — main.c owns the
// DayNight clock itself; this file does no arithmetic on the tick at all, only forwards it to
// world/daynight.h's pure functions.
void chunkRenderSetTimeOfDay(uint32_t tod);

// v1.8.10 fake directional lighting option (Options.fake_shading in app/options.h, off by
// default). Swings the EAST/WEST rows of the faceShade uniform table with the sun's current
// celestial angle instead of holding them at their fixed baked constants — see
// faceShadeTableBuild in chunk_render.c for the shape of the swing and why only those two
// faces move. Costs no new shader instructions and no new vertex data: the table this rebuilds
// is the same one pipelineBind already re-uploads to the GPU every bind.
//
// Call once a frame from the live Options struct, unconditionally, the same as
// chunkRenderSetTimeOfDay just above and right next to it — see this function's own comment in
// chunk_render.c for why "every frame, no change-detection" is deliberate and still cheap.
void chunkRenderSetFakeShading(bool on);

// v1.8.10 water shimmer, the second half of that same shaders option. Milliseconds — any
// monotonic clock; main.c passes svcGetSystemTick divided down, which is what every other
// millisecond figure in this project is (app/sleep.c, app/battery.c).
//
// The scrolling glint on water is gated on chunkRenderSetFakeShading's flag, not on a second
// option: steve asked for ONE "shaders" switch covering both the fake directional lighting and
// the fake water reflections, so Options.fake_shading owns both and there is no new field.
//
// Call once a frame, unconditionally, next to the two setters above. ONCE is the operative
// word: pipelineBind runs per eye, and reading the clock there instead would give the two eyes
// different scroll offsets on a high-contrast pattern, which the 3D slider fuses as depth. See
// this function's own comment in chunk_render.c.
//
// With the option off this value is stored and never read — every piece of GPU state the
// shimmer needs is behind the gate in chunkRenderDraw.
void chunkRenderSetShimmerTimeMs(uint64_t ms);

// v1.9.1. The debug overlay's "see" field used to read RenderDist.half_vis, which comes
// from the retired PICA200 fixed-function fog LUT (renderDistVisibility/renderDistFogTable
// in render_dist.c) and has had no GPU path since the v1.9.0 shader-driven fog-ramp redesign
// -- it barely moves across render distances (~14.3-14.4 regardless of radius) while the
// player's actual visibility scales hugely with it. This reads the LIVE half-visibility out
// of the FogShape this file genuinely uploaded to the GPU (s_fog), combined with fogRampTexel(),
// the same reference ramp-texel generator tests/fogramp_test.c already cross-checks against the
// compiled build/fogramp.t3x -- so there is exactly one texel table in play, not a second copy
// that can drift from what the console actually renders.
float chunkRenderFogHalfVis(void);

// Meshes one chunk out of the world into its slot, reusing the slot it already owns
// if it has one. A chunk that meshes to nothing KEEPS its slot: releasing it made that
// chunk invisible to chunkRenderTouch below, so a chunk dug out to nothing could never be
// re-queued again and a block placed back into it would exist in the World and never be
// drawn. The buffer is not stranded — an empty slot is the first thing reused once the
// pool is otherwise full. False means the pool was full or the mesh did not fit.
bool chunkRenderBuild(const World* w, int cx, int cy, int cz);

// Why the most recent chunkRenderBuild returned false. v1.8.11.
//
// The caller has to be able to tell these two apart, because the right response to each is the
// OPPOSITE of the right response to the other:
//
//   POOL     — acquireSlot found no free slot in any tier. Transient: slots come back as
//              columns unload, so asking again later genuinely can succeed, and asking again
//              is the only thing that repairs the hole.
//   OVERFLOW — this one chunk's own geometry exceeds MESH_SLOT_VERTS/MESH_SLOT_INDICES, the
//              absolute per-chunk cap. Permanent for as long as those blocks are those blocks.
//              Retrying it is provably useless: the same chunk meshed again produces the same
//              overflow, so a retry loop here would spin forever and starve the drain budget
//              that every other chunk is waiting on.
//
// A single conflated "it failed" counter cannot support that decision, which is why this exists
// rather than the caller inferring a reason. Valid only immediately after a false return, on the
// same thread; chunkRenderBuild is main-thread only and its two callers both test it straight
// away. A true return leaves this CHUNK_REFUSE_NONE.
typedef enum {
	CHUNK_REFUSE_NONE = 0,
	CHUNK_REFUSE_POOL,
	CHUNK_REFUSE_OVERFLOW,
} ChunkRefuseReason;

ChunkRefuseReason chunkRenderLastRefusal(void);

// Hands back every slot belonging to a column that has just been unloaded, and returns how
// many. Call it *after* the column is gone from the World: a slot whose chunk no longer
// exists would otherwise keep drawing terrain that is not there, and — worse — would still
// match in chunkRenderTouch, so an edit near the world edge would queue a remesh of a chunk
// that reads as air and quietly blank a neighbour.
//
// This is the one place a used slot is released without meshing. The "an empty chunk keeps
// its slot" rule exists so a dug-out chunk stays queueable; a chunk that has left the world
// entirely has nothing to stay queueable for.
int chunkRenderReleaseColumn(int cx, int cz);

// v1.7.1 task 46. Hands the WHOLE pool back, arenas and shader untouched. Call this when a
// world is torn down but the process lives on — i.e. quit-to-title, which is the one path that
// frees the World and then goes back to the menu. Without it the slot table carries the
// previous world's chunks into the next one: their geometry is still drawn at those
// coordinates, and their slots are unavailable to the new world's meshing. See the definition
// for why this is not chunkRenderExit + chunkRenderInit.
void chunkRenderReleaseAll(void);

// One block changed: marks what that can have altered as needing a remesh and returns
// immediately — it does no meshing itself. Not just the owning chunk — see world/remesh.h
// for why a corner edit reaches eight. Chunks with no mesh yet are left alone, so this
// never grows the working set. Returns how many chunks were newly queued (a chunk touched
// twice before it drains, or one with no slot, does not add to the count).
//
// Why this queues instead of meshing on the spot: a remesh costs ~1.55 ms (scratchFill plus
// meshChunk at 917 us (step 9.4c measurement) — see chunkRenderProfile — plus the 0.63 ms step
// 7.3's visibility fill added, see chunkRenderVisUs) and a corner edit reaches all eight chunks
// around it, so meshing immediately would cost 8 x 1.55 ms = 12.4 ms against a 16.71 ms frame
// at 59.83 Hz — one edit would eat most of the frame, and holding a break button down would drop
// frames continuously. Queueing lets chunkRenderDrainDirty spread that cost over as many frames
// as it takes.
int chunkRenderTouch(const World* w, int x, int y, int z);

// Remeshes queued chunks until budget_ms of wall clock has been spent, max_chunks have been
// completed, or the queue runs dry, and returns how many it completed. Call once per frame.
// At ~2.1 ms a chunk, a 4 ms budget clears two or three, so the worst case of eight queued
// chunks (a corner edit) drains over three or four frames instead of stalling one frame for
// 16.8 ms.
//
// Two limits rather than one, because they fail differently. The clock can only stop the
// loop *after* a chunk has already overrun it — so on its own the worst case is
// "budget plus however long one chunk took", which is not a number anyone can plan a frame
// around. The count stops the loop before the next chunk starts, which makes the worst case
// max_chunks x ~2.1 ms and lets step 6.2 share one budget across the edit queue and the
// streaming queue without either of them being able to eat the frame.
//
// Always remeshes at least one chunk when the queue is non-empty, whatever budget_ms and
// max_chunks say: a budget that can never make progress would freeze the world's on-screen
// appearance forever with no visible cause, which is worse than one over-budget frame. To
// force the queue empty in a single call — chunkRenderChecksum() needs this, since it
// compares an incrementally remeshed pool against a fully rebuilt one and a straggling dirty
// chunk would make them disagree for a reason that has nothing to do with a real bug — call
// with a large budget and MESH_SLOTS, which is at least every slot the pool has (v1.8.5: the
// pool may be narrower than the compile-time ceiling, so MESH_SLOTS is now an upper bound on
// the slot count rather than exactly it — which is all this cap needs it to be).
int chunkRenderDrainDirty(const World* w, float budget_ms, int max_chunks);

int  chunkRenderDirtyCount(void);      // chunks still waiting to be remeshed
int  chunkRenderDirtyPeak(void);       // high-water mark since the last reset, for the overlay
void chunkRenderDirtyResetPeak(void);

// Draws every live mesh, one draw call each. Re-establishes its own shader program,
// vertex format, TEV stage, depth test, cull mode and shade uniforms on every call, so
// anything else that draws in the same frame — the step 4.2 highlight, with its own
// program and its own vertex format — does not have to put the GPU back afterwards.
void chunkRenderDraw(const C3D_Mtx* view);

// Step 7.6. Points the world at one eye. Call it before chunkRenderDraw; everything that
// reads the projection — the frustum test, the sight walk, the draw itself, and the
// highlight, which shares this matrix — follows without knowing which eye it is.
//
// `iod` is the interocular offset in blocks: negative for the left eye, positive for the
// right, and exactly 0 for a 2D frame, which is the default and takes a separate branch so
// that the mono matrix is bit-for-bit what it was before this step. chunkRenderMaxIod() is
// the offset at the 3D slider's maximum, so the caller can scale the slider by it without
// owning a second copy of the number.
void  chunkRenderSetEye(float iod);
float chunkRenderMaxIod(void);

// The projection the world is drawn with, so a second pass can match it exactly instead
// of keeping its own copy of the field of view and clip planes.
const C3D_Mtx* chunkRenderProjection(void);

int      chunkRenderMeshes(void);     // slots holding geometry
uint32_t chunkRenderTris(void);       // triangles across all of them

// Chunks the step 7.1 frustum test rejected on the most recent chunkRenderDraw. Reported
// rather than inferred: "draws went down" could equally mean a column failed to mesh, and
// the two have very different causes.
int      chunkRenderCulled(void);

// Step 7.2's evidence. The front-to-back sort is invisible by design — opaque geometry
// behind a depth test draws the same picture in any order — so "it works" and "it is a
// no-op" cannot be told apart on screen. chunkRenderSortMoved() counts how many chunks the
// sort actually took out of slot order on the last draw, and chunkRenderSortOk() says
// whether the sequence it emitted really was non-decreasing in distance.
int      chunkRenderSortMoved(void);
bool     chunkRenderSortOk(void);

// Step 7.3's evidence. chunkRenderCaveCulled() is how many chunks the sight walk rejected
// that the frustum had *kept* — the two counts are disjoint, so they can be added up.
// chunkRenderCaveRan() is false when the walk had to be skipped because the loaded area did
// not fit VisWalk's box, in which case nothing was cave-culled at all and the count is 0 for
// a reason that has nothing to do with the terrain.
int      chunkRenderCaveCulled(void);
bool     chunkRenderCaveRan(void);

// Step 9.1e's evidence. How many chunks the horizon test rejected that the frustum and the
// sight walk had *both* kept — disjoint from chunkRenderCulled() and chunkRenderCaveCulled(),
// so all three add up. See chunk_render.c's BS_HORIZON comment for what the test is and why it
// is safe to leave on unconditionally: it can only under-cull, never over-cull.
int      chunkRenderHorizonCulled(void);

// Step 9.3's evidence, cumulative since boot. chunkRenderCullRuns() counts how many times the
// frustum-plus-sight-walk-plus-sort pass actually ran; chunkRenderWalkRuns() how many times the
// sight walk itself did. Cumulative rather than per frame because the claims are ratios against
// the frame count: culls per frame must stay 1 even in a 3D frame, which draws twice, and walks
// per frame must fall well below 1, since the walk only changes when the camera crosses a chunk
// boundary. Neither is visible on screen — a frame culled twice looks exactly like a frame
// culled once — so without these two numbers the step cannot be checked at all.
uint32_t chunkRenderCullRuns(void);
uint32_t chunkRenderWalkRuns(void);

// The camera position the sight walk started from, recovered by inverting the view matrix
// rather than being passed in. Reported so it can be checked against the camera main.c
// already holds: the inversion is either exact or nonsense, and a walk begun from the wrong
// chunk would cull plausibly and wrongly.
void     chunkRenderCamera(float* x, float* y, float* z);

// Step 7.5's evidence: what the alpha-tested second pass actually submitted on the last
// chunkRenderDraw. Both zero means the pass drew nothing — which looks identical on screen
// to "there are no leaves in view", and only one of those is a bug.
uint32_t chunkRenderAlphaTris(void);
int      chunkRenderAlphaDraws(void);

// Average microseconds step 7.3's flood fill added to a chunk build. Separate from
// chunkRenderProfile's two buckets on purpose — new cost on a path with an existing budget
// has to be visible on its own, not hidden inside a number that was already there.
float    chunkRenderVisUs(void);

// Hash of every vertex byte in the pool, independent of slot order. Comparing an
// incrementally remeshed pool against a fully rebuilt one is the only check that can
// see a missed neighbour: the usual symptom is lost AO, which leaves the triangle
// count untouched.
uint32_t chunkRenderChecksum(void);

// Average microseconds per chunk build, split into the scratch fill and the mesher, since
// only the split says which one to optimise. Reset, run the work, then read.
void chunkRenderProfileReset(void);
void chunkRenderProfile(float* scratch_us, float* mesh_us, int* builds);
int      chunkRenderRefusals(void);   // pool full, or a chunk too big for a slot
size_t   chunkRenderBytes(void);      // linear memory the pool claimed

// The draw guard's default lives HERE, in the header, rather than beside the guard's own code
// in chunk_render.c — deliberately, and the reason is a bug that shipped.
//
// A `#define` is per translation unit. From v1.8.17, when the guard was turned on, the
// `#ifndef BS_DRAW_GUARD / #define BS_DRAW_GUARD 1` block sat in chunk_render.c, so the guard
// itself compiled and ran, but every OTHER .c file still saw the name as undefined. `#if` reads
// an undefined macro as 0, so main.c's reporting block — the only caller of the eight accessors
// below and the only writer of the watchdog's guard line — compiled out entirely, and
// --gc-sections then dropped all eight functions from the link. Measured, not reasoned: the
// shipped v1.8.17 blocksmith.elf contains neither "clean - every chunk draw issued this session
// validated" nor "BAD DRAW code", and nm finds none of the symbols. So the guard ran, recorded
// its verdict, and nothing on the console could read it — which is why steve's hang.txt has no
// draw-guard line despite the flag being "on".
//
// In the header, every includer agrees on the value. app/watchdog.h does the same thing with
// BS_DRAW_PROBE, for the same reason.
#ifndef BS_DRAW_GUARD
#define BS_DRAW_GUARD 1
#endif

#if BS_DRAW_GUARD
// Step: the draw guard's findings, for whoever writes the report. See drawSane() in
// chunk_render.c. Code 0 means every draw issued this session passed; 1 = the bound vertex
// buffer was NULL or not the start of a pool slot, 2 = the index count was not whole
// triangles, 3 = the run ran off the end of the shared index buffer, 4 = the run's indices
// reached past the vertex buffer that was bound for it.
//
// v1.8.18 added three more, closing gaps a freeze-investigation lane found in the original
// four: 5 = the run's `first` did not start on a QUAD boundary (only `count`'s multiple-of-3
// was ever checked, so a run could begin mid-triangle and still pass every other check).
// Tightened same version, lane IDX-HARDEN: the check is `first % 6u`, not `first % 3u`. Lane
// FRZ-INDEX measured 51,740 draw runs against real generated terrain and found `first` a
// multiple of 6 in every one, because world/mesher.c's only two geometry-emitting functions —
// emitFace and emitCross — each write a whole quad's 4 vertices and 6 indices atomically (an
// overflow check gates the write before either counter moves, so there is never a partial
// quad), which makes `index_count` a multiple of 6 at all times and every face_start/
// opaque_index_count boundary — the only values a real `first` is ever built from — a multiple
// of 6 with it. A `first` that is 3-aligned but not 6-aligned starts mid-QUAD: it is still a
// legal triangle boundary (code 2 would not catch it), but code 4's "read the run's last index
// to get its maximum" assumption silently breaks, because the run no longer covers a whole
// quad's worth of indices from its own base vertex. `% 3u` let that through; `% 6u` does not.
// 6 = the run's `first`/`count` belong to a different slot than the one actually bound on the
// GPU (drawSane takes the intended slot explicitly now and checks it against what slotBind
// last set, rather than trusting the two always travel together); 7 = the tier ground-truth
// tables boundVertCap() derives its answer from (s_tier_arena/s_tier_count) no longer match
// the frozen copy taken at the end of a successful chunkRenderInit, i.e. something outside
// this file corrupted them since boot. In every code's report line, `slot`/`verts` are always
// the slot actually bound on the GPU (s_bound), not whichever slot the caller believed it was
// drawing — for code 6 those two differ by definition, and it is what the GPU really has that
// matters for the report.
//
// v1.8.18 added two more, in slotBind() rather than drawSane() — lane IDX-HARDEN task B,
// hardening two citro3d failures disassembly of libcitro3d.a showed neither libctru nor this
// file ever checked: 8 = C3D_GetBufInfo() returned NULL (the 3D context was inactive — calling
// BufInfo_Init on that NULL would otherwise memset 148 bytes at address 4); 9 = BufInfo_Add()
// failed (its 12-entry buffer table was full — and had already incremented bufCount before
// returning that failure, so a failed Add still leaves the C3D_BufInfo dirtier than it found
// it). Neither has been observed to fire in this codebase — this is hardening against citro3d's
// contract, not a fix for anything measured wrong. Both leave `first`/`count`/`maxidx`/`cap` at
// 0, since neither failure ever reaches the point of having a run to describe; `slot`/`verts`
// still name the slot slotBind was asked to bind. Both also leave s_bound unchanged, so the
// caller's next drawIndices() call independently refuses the draw as code 6 — these two codes
// exist to name the real cause first, not to be the only thing standing between a bad bind and
// the GPU.
//
// All read-only and all plain loads, so the watchdog thread can call them while the main
// thread is wedged — it must never take a lock (see app/watchdog.h).
int       chunkRenderGuardCode(void);
int       chunkRenderGuardHits(void);
int       chunkRenderGuardSlot(void);
uint32_t  chunkRenderGuardFirst(void);
uint32_t  chunkRenderGuardCount(void);
uint32_t  chunkRenderGuardMaxIdx(void);
uint32_t  chunkRenderGuardCap(void);
uintptr_t chunkRenderGuardVerts(void);
#endif

// v1.8.18, lane IDX-HARDEN task C. Closes the one open measurement two freeze-investigation
// lanes could not settle by reading the code: C3D_DrawElements programs the GPU's index-buffer
// register as an OFFSET from bufInfo.base_paddr, which BufInfo_Init sets unconditionally to
// 0x18000000 (VRAM base) — so the value actually latched into GPUREG_INDEXBUFFER_CONFIG for a
// draw starting at index `first` is phys(s_shared_indices) + 2*first - 0x18000000, and nobody
// has measured phys(s_shared_indices) on real hardware, nor is the register's offset field width
// documented anywhere in libctru. An overflow there reaches the GPU as a fetch from a bogus
// physical address — inert while the state is merely programmed, lethal the moment a draw
// actually reads it, which is exactly this bug's shape.
//
// Deliberately NOT gated behind BS_DRAW_GUARD: this is a one-time boot measurement, not a
// per-draw check, and it has to stay available in every build so a build with the guard forced
// off still gets it written to sdmc:/blocksmith/memprobe.txt.
//
// chunk_render.c is the only file that may call these — it owns s_shared_indices and
// s_tier_arena[0..2], the only pointers osConvertVirtToPhys needs, and computes both halves
// once, right after each linearAlloc succeeds in chunkRenderInit (see the comment on
// s_indices_virt there). Plain loads afterward, same convention as the draw guard's read-only
// accessors just above. `tier` is 0/1/2 for S/M/L; anything out of range, or a call before
// chunkRenderInit has succeeded or after chunkRenderExit, reads 0 for both halves of a pair — a
// real linearAlloc address on this console is never 0, so the caller can tell "not measured yet"
// from a genuine reading without a separate bool, the same trick MEMPROBE_REL's caller-side
// convention already relies on.
//
// main.c is meant to read these once, right after chunkRenderInit() returns true (which is also
// where its own memProbeBootMark("chunkRenderInit") call already sits, immediately before its
// memProbeBootFlush() — see main.c), format one line per pointer the same way memProbeBootFlush
// formats its own region-summary tail, and append it into sdmc:/blocksmith/memprobe.txt: chunk
// render owns the pointers here, main.c owns the SD write, exactly the same division step 9.2's
// FS writes already use elsewhere in this project.
uintptr_t chunkRenderIndexVirt(void);
uint32_t  chunkRenderIndexPhys(void);
uintptr_t chunkRenderTierVirt(int tier);
uint32_t  chunkRenderTierPhys(int tier);
