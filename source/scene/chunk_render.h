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

// Remeshes queued chunks until budget_ms of wall clock has been spent or the queue runs
// dry, and returns how many it completed. Call once per frame. At ~1.47 ms a chunk, a
// 4 ms budget typically clears two or three, so the worst case of eight queued chunks
// (a corner edit) drains over two or three frames instead of stalling one frame for
// 11.8 ms.
//
// Always remeshes at least one chunk when the queue is non-empty, even if budget_ms is
// already spent, zero, or negative: a budget that can never make progress would freeze
// the world's on-screen appearance forever with no visible cause, which is worse than
// one over-budget frame. To force the queue empty in a single call — chunkRenderChecksum()
// needs this, since it compares an incrementally remeshed pool against a fully rebuilt one
// and a straggling dirty chunk would make them disagree for a reason that has nothing to
// do with a real bug — call with a large budget, e.g. 1000.0f.
int chunkRenderDrainDirty(const World* w, float budget_ms);

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
