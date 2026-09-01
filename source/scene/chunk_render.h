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

// Meshes one chunk out of the world into its slot, reusing the slot it already owns
// if it has one. A chunk that meshes to nothing KEEPS its slot: releasing it made that
// chunk invisible to chunkRenderTouch below, so a chunk dug out to nothing could never be
// re-queued again and a block placed back into it would exist in the World and never be
// drawn. The buffer is not stranded — an empty slot is the first thing reused once the
// pool is otherwise full. False means the pool was full or the mesh did not fit.
bool chunkRenderBuild(const World* w, int cx, int cy, int cz);

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

#if BS_DRAW_GUARD
// Step: the draw guard's findings, for whoever writes the report. See drawSane() in
// chunk_render.c. Code 0 means every draw issued this session passed; 1 = the bound vertex
// buffer was NULL or not the start of a pool slot, 2 = the index count was not whole
// triangles, 3 = the run ran off the end of the shared index buffer, 4 = the run's indices
// reached past the vertex buffer that was bound for it.
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
