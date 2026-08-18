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

#include "world/world.h"

// Per-slot capacity. A pathological 16³ checkerboard would need 12,288 faces, which
// no terrain produces; 2,048 faces is comfortably past a fully exposed chunk surface
// (1,536) and keeps a slot at 88 KB. A chunk that will not fit is counted and skipped
// rather than half-drawn.
#define MESH_SLOT_FACES    2048
#define MESH_SLOT_VERTS    (MESH_SLOT_FACES * 4)
#define MESH_SLOT_INDICES  (MESH_SLOT_FACES * 6)
#define MESH_SLOTS         64

// The sky colour, in one place because step 6.4's fog has to be the same colour as the
// background it fades into. Fog that does not match the clear colour reads as a grey sheet
// hung in front of the sky instead of as distance, and one constant living in two files is
// exactly how that drifts apart. Two byte orders because the two registers disagree:
// C3D_RenderTargetClear takes RGBA8 and the PICA's fog colour register is 0x00BBGGRR.
// Change one, change the other.
#define SKY_CLEAR_RGBA8  0x102A33FF
#define SKY_FOG_BGR      0x00332A10

// Loads the shader, the atlas and the buffer pool. False if any of it failed.
bool chunkRenderInit(void);
void chunkRenderExit(void);

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

// One block changed: marks what that can have altered as needing a remesh and returns
// immediately — it does no meshing itself. Not just the owning chunk — see world/remesh.h
// for why a corner edit reaches eight. Chunks with no mesh yet are left alone, so this
// never grows the working set. Returns how many chunks were newly queued (a chunk touched
// twice before it drains, or one with no slot, does not add to the count).
//
// Why this queues instead of meshing on the spot: a remesh costs ~1.47 ms (scratchFill
// plus meshChunk — see chunkRenderProfile) and a corner edit reaches all eight chunks
// around it, so meshing immediately would cost 8 x 1.47 ms = 11.8 ms against a 16.71 ms
// frame at 59.83 Hz — one edit would eat almost the whole frame, and holding a break
// button down would drop frames continuously. Queueing lets chunkRenderDrainDirty spread
// that cost over as many frames as it takes.
int chunkRenderTouch(const World* w, int x, int y, int z);

// Remeshes queued chunks until budget_ms of wall clock has been spent, max_chunks have been
// completed, or the queue runs dry, and returns how many it completed. Call once per frame.
// At ~1.47 ms a chunk, a 4 ms budget clears two or three, so the worst case of eight queued
// chunks (a corner edit) drains over two or three frames instead of stalling one frame for
// 11.8 ms.
//
// Two limits rather than one, because they fail differently. The clock can only stop the
// loop *after* a chunk has already overrun it — so on its own the worst case is
// "budget plus however long one chunk took", which is not a number anyone can plan a frame
// around. The count stops the loop before the next chunk starts, which makes the worst case
// max_chunks x ~1.5 ms and lets step 6.2 share one budget across the edit queue and the
// streaming queue without either of them being able to eat the frame.
//
// Always remeshes at least one chunk when the queue is non-empty, whatever budget_ms and
// max_chunks say: a budget that can never make progress would freeze the world's on-screen
// appearance forever with no visible cause, which is worse than one over-budget frame. To
// force the queue empty in a single call — chunkRenderChecksum() needs this, since it
// compares an incrementally remeshed pool against a fully rebuilt one and a straggling dirty
// chunk would make them disagree for a reason that has nothing to do with a real bug — call
// with a large budget and MESH_SLOTS, which is every slot the pool has.
int chunkRenderDrainDirty(const World* w, float budget_ms, int max_chunks);

int  chunkRenderDirtyCount(void);      // chunks still waiting to be remeshed
int  chunkRenderDirtyPeak(void);       // high-water mark since the last reset, for the overlay
void chunkRenderDirtyResetPeak(void);

// Draws every live mesh, one draw call each. Re-establishes its own shader program,
// vertex format, TEV stage, depth test, cull mode and shade uniforms on every call, so
// anything else that draws in the same frame — the step 4.2 highlight, with its own
// program and its own vertex format — does not have to put the GPU back afterwards.
void chunkRenderDraw(const C3D_Mtx* view);

// The projection the world is drawn with, so a second pass can match it exactly instead
// of keeping its own copy of the field of view and clip planes.
const C3D_Mtx* chunkRenderProjection(void);

int      chunkRenderMeshes(void);     // slots holding geometry
uint32_t chunkRenderTris(void);       // triangles across all of them

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
