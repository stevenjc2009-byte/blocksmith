// Blocksmith — Phase 5.
//
// Phase 3's meshed world is now something you stand in rather than fly over: the camera
// rides a physics body (scene/player.c), a DDA ray says which block it is aimed at
// (world/raycast.c), a cage is drawn round that block (scene/highlight.c), and X and Y
// break and place (scene/interact.c). Edits no longer remesh on the spot — they queue,
// and each frame spends a fixed slice of itself draining the queue.
//
// Since step 5.2 the ground under all of that is generated from a seed
// (world/worldgen.c) rather than hand-built. The hand-built area has not gone away: the
// Phase 3 baseline and the two console probes below are all measured against it, so it
// is still what a probe build loads. See BS_WORLD_GEN.
// See code-vault/wiki/blocksmith/blocksmith-plan.md.

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "app/worker.h"
#include "debug/metrics.h"
#include "gfx/screen.h"
#include "scene/camera.h"
#include "scene/chunk_render.h"
#include "scene/highlight.h"
#include "scene/interact.h"
#include "scene/player.h"
#include "world/budget.h"
#include "world/handbuilt.h"
#include "world/jobq.h"
#include "world/mesh_vertex.h"
#include "world/world.h"
#include "world/world_test.h"
#include "world/worldgen.h"

#define CLEAR_COLOR 0x102A33FF   // dark teal — deliberately not the bottom screen's blue

// The research page's worst case for an Old 3DS: 17x17 columns of fully populated
// block data, ~9.25 MB. Claiming it here, once, is what turns the budget from an
// arithmetic argument into a measurement — and if the hardware refuses it, the
// report says so instead of a random crash surfacing three phases later.
#define REPORT_SPAN 17

static World s_world;

// Fills the worst-case grid and returns the number of allocations that were
// refused, which must be zero.
static int worldReportBuild(void)
{
	int refused = 0;

	worldInit(&s_world);
	for (int cz = 0; cz < REPORT_SPAN; cz++)
		for (int cx = 0; cx < REPORT_SPAN; cx++)
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
				if (!worldChunkCreate(&s_world, cx, cy, cz)) refused++;

	return refused;
}

// The report sits at row 19, below the metrics overlay, which only ever writes rows
// 1-18. Build with -DBS_REPORT_ONLY=1 to suppress the overlay and put the report at
// the top of the console instead: an emulator window is usually too short to show
// all 30 rows, and the report is the one thing the Phase 2 gate must be readable.
#ifndef BS_REPORT_ONLY
#define BS_REPORT_ONLY 0
#endif

#if BS_REPORT_ONLY
#define BS_REPORT_ROW 1
#else
#define BS_REPORT_ROW 19
#endif

// Draw-load knob, for proving that the submit/sync timers actually respond to GPU
// work — C3D_GetDrawingTime() cannot, since Azahar reports a constant 0.25 ms.
// Off by default; the extra draws are the whole chunk set re-submitted.
#ifndef BS_GPU_STRESS
#define BS_GPU_STRESS 0
#endif

// How much of each frame may be spent meshing, and how many chunks that is allowed to be.
//
// One budget for *all* meshing, not one each for edits and for streaming. Until step 6.2
// they were separate — 4 ms for the edit backlog and 4 ms for the streaming backlog — on
// the argument that a boot must not starve the response to a button press. The argument
// was right and the implementation was wrong: two independent 4 ms budgets, each of which
// can only stop *after* a chunk has overrun it, is a measured worst case of 5.56 ms each,
// so a player breaking blocks while walking could hand ~11 ms of a 16.71 ms frame to
// meshing alone. Priority, not separate wallets, is what protects the button press: the
// edit queue drains first and the streaming queue gets what is left.
//
// The frame is 16.71 ms at 59.83 Hz, steady-state CPU work measured 0.62 ms with a 0.37 ms
// submit, and a chunk costs ~1.47 ms. 4 ms and 3 chunks is a quarter of the frame with a
// worst case that can be written down: 3 chunks, plus the one chunk each drain is always
// allowed to complete, is ~5.9 ms and cannot grow.
#define DRAIN_BUDGET_MS   4.0f
#define DRAIN_MAX_CHUNKS  3

// Phase 4 cannot be verified by pressing buttons: no keyboard key reaches the emulated
// console (every button in the emulator's config is bound to an SDL pad), which is the
// same reason camera.c carries BS_ORBIT. These two knobs are the scripted substitutes.
//
//   BS_INTERACT_DEMO=1  fires one break and then one place through the real interact
//                       path — raycast, worldSet, chunkRenderTouch — so the whole 4.3
//                       chain runs and the counters on the overlay say whether it
//                       reached the world.
//   BS_EDIT_STRESS=N    edits a chunk corner every frame for N frames, which is the
//                       plan's stated 4.5 check ("hold the break button on a chunk
//                       corner and watch worst/over") in a form a script can run.
#ifndef BS_INTERACT_DEMO
#define BS_INTERACT_DEMO 0
#endif
#ifndef BS_EDIT_STRESS
#define BS_EDIT_STRESS 0
#endif

// BS_WALK_STRESS=N walks the player due east for N frames, which is step 6.1's check.
// A ring that only ever loads is indistinguishable from a ring that streams correctly
// until something walks out of the area it started in, and no key on this machine reaches
// the emulated console (the same reason BS_INTERACT_DEMO exists).
//
// It drives the body rather than faking a button, because playerUpdate reads hidKeysHeld()
// itself: one extra bodyStep per frame with the walk speed in vx, through the same
// collision and auto-step the player uses. playerUpdate's own step ran with vx = 0, so the
// speed is PLAYER_WALK_SPEED, not double it. The second step re-applies gravity for the
// same tick, which on ground is resolved away by the floor and is the one thing about this
// probe that is not exactly what a player would produce.
#ifndef BS_WALK_STRESS
#define BS_WALK_STRESS 0
#endif

// BS_WALK_EDIT=1 makes the walk probe also edit, which is step 6.2's check. Walking alone
// only ever fills one of the two mesh queues, and a shared budget cannot be told apart from
// two separate ones until both are full at the same time. It edits a *chunk corner* rather
// than a convenient nearby block because that is the edit that dirties eight chunks
// (world/remesh.h), so the edit queue stays permanently backed up and the streaming queue
// has to live on whatever is left.
#ifndef BS_WALK_EDIT
#define BS_WALK_EDIT 0
#endif

// Fly mode keeps the Phase 3 free-fly camera reachable, because it is how the world gets
// inspected from outside and it is what camera.c's BS_ORBIT drives. Walking is the
// default: it is the thing Phase 4 is judged on.
#ifndef BS_FLY
#define BS_FLY 0
#endif

// Frames to let the world settle before either knob fires: the first frame absorbs the
// startup gap (world build, self-test, and on an instrumented build a five-second remesh
// stress), so an edit on it would be measuring that instead.
#define DEMO_START_FRAME 60

// Meshes the hand-built area. Returns the number of chunks the mesh pool refused,
// which must be zero — a refusal is a hole in the world, not a slow frame.
static int meshHandbuilt(void)
{
	for (int cz = 0; cz < HANDBUILT_CHUNKS_Z; cz++)
		for (int cx = 0; cx < HANDBUILT_CHUNKS_X; cx++)
			for (int cy = 0; cy < HANDBUILT_CHUNKS_Y; cy++)
				chunkRenderBuild(&s_world, cx, cy, cz);

	return chunkRenderRefusals();
}

// Remesh stress: the two Phase 3 measurements that only exist on the console.
//
// Step 3.3 — the VBO pool must never free or re-allocate, so free linear memory has to
// come out of a thousand remeshes exactly where it went in. Every edit is applied,
// remeshed, reverted and remeshed again, so the world is left as it was found.
//
// Step 3.6 — one edit on a chunk corner, remeshed through the touch list only, then
// compared against a full rebuild of every chunk. A triangle count cannot do this job:
// a missed neighbour normally only loses AO, which leaves the triangle count untouched,
// so the comparison is over every vertex byte instead.
//
// Off by default. At ~3.7 ms per chunk remesh a thousand edits take about thirteen
// seconds, and a playtest build should not stall before the first frame for that long.
// Run the proof with:  make EXTRA_CFLAGS="-DBS_REPORT_ONLY=1 -DBS_REMESH_STRESS=1000"
#ifndef BS_REMESH_STRESS
#define BS_REMESH_STRESS 0
#endif

typedef struct {
	u32      free_before, free_after;
	uint32_t sum_incremental, sum_full;
	int      edits, rebuilt, refused;
	bool     ran;
	float    ms;
	float    scratch_us, mesh_us;   // average per chunk build, the split that matters
	int      builds;
} StressResult;

static StressResult remeshStress(int edits)
{
	StressResult r = {0};
	if (edits <= 0) return r;

	r.ran         = true;
	r.free_before = (u32)linearSpaceFree();
	r.refused     = chunkRenderRefusals();
	r.edits       = edits;

	chunkRenderProfileReset();
	const u64 t0 = svcGetSystemTick();

	// Deterministic, and aimed at the borders on purpose: every fourth edit lands on a
	// chunk corner, which is the eight-chunk case.
	u32 rng = 0x9E3779B9u;
	for (int e = 0; e < edits; e++) {
		rng = rng * 1664525u + 1013904223u;
		int x = (int)((rng >> 8)  % HANDBUILT_BLOCKS_X);
		int z = (int)((rng >> 16) % HANDBUILT_BLOCKS_Z);
		int y = (int)((rng >> 4)  % (HANDBUILT_CHUNKS_Y * CHUNK_DIM));
		if ((e & 3) == 0) { x = CHUNK_DIM; z = CHUNK_DIM; y = CHUNK_DIM; }

		const BlockId was = worldGet(&s_world, x, y, z);
		const BlockId now = (was == BLOCK_AIR) ? BLOCK_SAND : BLOCK_AIR;

		// Since step 4.5, chunkRenderTouch only queues — so the stress has to drain the
		// queue itself, with a budget large enough to clear it in one call. Without this
		// the loop would measure nothing but the cost of setting a few booleans, and the
		// number on screen would silently become meaningless.
		worldSet(&s_world, x, y, z, now);
		r.rebuilt += chunkRenderTouch(&s_world, x, y, z);
		chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);

		worldSet(&s_world, x, y, z, was);
		r.rebuilt += chunkRenderTouch(&s_world, x, y, z);
		chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);
	}

	r.ms         = (float)(svcGetSystemTick() - t0) / (float)(SYSCLOCK_ARM11 / 1000);
	r.free_after = (u32)linearSpaceFree();
	r.refused    = chunkRenderRefusals() - r.refused;
	chunkRenderProfile(&r.scratch_us, &r.mesh_us, &r.builds);

	// Step 3.6. A block on the corner where four chunk columns meet, sitting on the
	// surface, so the grass it now overshadows lives in three *other* chunks. Remesh it
	// through the touch list, hash the pool, then rebuild everything and hash again.
	const int bx = CHUNK_DIM, bz = CHUNK_DIM;
	const int by = handbuiltHeight(bx, bz);

	worldSet(&s_world, bx, by, bz, BLOCK_STONE);
	chunkRenderTouch(&s_world, bx, by, bz);
	chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);   // a straggler would make the two hashes
	r.sum_incremental = chunkRenderChecksum();  // disagree for a reason that is not a bug

	meshHandbuilt();
	r.sum_full = chunkRenderChecksum();

	// Put the world back, rebuilding from the real block data rather than trusting the
	// touch list to undo itself: what the report says must not depend on the thing the
	// report is testing.
	worldSet(&s_world, bx, by, bz, BLOCK_AIR);
	chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);
	meshHandbuilt();

	return r;
}

// Dig-out check: a chunk emptied of every block must still be re-queueable.
//
// This is the one case Phase 4's interaction path could not reach from a controller — a
// 16x16x16 chunk is thousands of button presses — and it was a real bug for as long as
// chunkRenderBuild released the slot of a chunk that meshed to nothing. chunkRenderTouch
// only queues chunks that already own a slot, so a dug-out chunk became permanently
// unqueueable: a block placed back into it existed in the World and was never drawn.
//
// It lives in main.c rather than the host suite for the same reason the dirty-queue bug
// did not: this is scene/chunk_render.c, which includes <3ds.h> and citro3d and cannot be
// linked into a PC build. The check therefore runs on the console, against the real
// touch/drain/draw path, and reports on the same screen as everything else.
//
// Off by default — it empties and refills 4,096 blocks, which a playtest build has no
// reason to do. Run it with:  make EXTRA_CFLAGS="-DBS_REPORT_ONLY=1 -DBS_DIG_OUT=1"
#ifndef BS_DIG_OUT
#define BS_DIG_OUT 0
#endif

typedef struct {
	bool     ran;
	int      queued_before;   // touch on the chunk while it still has geometry
	int      queued_after;    // touch on the same chunk once emptied — 0 was the bug
	uint32_t tris_empty, tris_back;
	bool     pass;
} DigOutResult;

static DigOutResult digOutCheck(bool run)
{
	DigOutResult r = {0};
	if (!run) return r;
	r.ran = true;

	// Chunk (1,0,1): blocks x/z 16..31, y 0..15. Wholly inside the hand-built area, and
	// its surface at (17,17) is flat ground at h=12, so the top solid block is y=11.
	const int cx = 1, cy = 0, cz = 1;
	const int tx = 17, tz = 17;
	const int ty = handbuiltHeight(tx, tz) - 1;

	// Baseline: this chunk is queueable right now.
	r.queued_before = chunkRenderTouch(&s_world, tx, ty, tz);
	chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);

	// Dig the whole chunk out, then remesh it through the real path.
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				worldSet(&s_world, cx * CHUNK_DIM + x, cy * CHUNK_DIM + y,
				         cz * CHUNK_DIM + z, BLOCK_AIR);
	chunkRenderTouch(&s_world, tx, ty, tz);
	chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);
	r.tris_empty = chunkRenderTris();

	// Put one block back in the middle of the emptied chunk. All six of its neighbours
	// are air, so if it is meshed at all the triangle count must rise by twelve.
	worldSet(&s_world, tx, 5, tz, BLOCK_STONE);
	r.queued_after = chunkRenderTouch(&s_world, tx, 5, tz);
	chunkRenderDrainDirty(&s_world, 1000.0f, MESH_SLOTS);
	r.tris_back = chunkRenderTris();

	r.pass = (r.queued_before > 0) && (r.queued_after > 0) && (r.tris_back > r.tris_empty);

	// Put the world back from the block data, not by undoing the edits: what the report
	// says must not depend on the thing the report is testing.
	handbuiltFill(&s_world);
	meshHandbuilt();
	return r;
}

// Which world the game loads.
//
// Generated terrain is the point of Phase 5, so it is the default. The three probes above
// are not optional consumers of the hand-built area — they are *defined* against it:
// remeshStress edits at handbuiltHeight() and compares against the 048EFC0A pool hash,
// digOutCheck empties chunk (1,0,1) knowing what is in it, and BS_EDIT_STRESS hammers the
// corner at (16, handbuiltHeight(16,16), 16). All three also put the world back with
// handbuiltFill(). Running any of them against generated terrain would produce a number
// that looks like the Phase 3/4 baseline and is not comparable to it, which is worse than
// not running them at all — so switching a probe on switches the world back.
//
// Override explicitly with -DBS_WORLD_GEN=0 or =1 if a build wants the other pairing.
#ifndef BS_WORLD_GEN
#if BS_REMESH_STRESS || BS_DIG_OUT || BS_EDIT_STRESS
#define BS_WORLD_GEN 0
#else
#define BS_WORLD_GEN 1
#endif
#endif

// The world seed. Fixed rather than derived from the clock: every measurement in the vault
// is against one world, and a world that changed each boot would make "is this hill new or
// is this a bug?" unanswerable. Phase 8 gives it a real home in the save file.
#ifndef BS_WORLD_SEED
#define BS_WORLD_SEED 1337
#endif

// Radius, in columns, of the area generated and of the part of it that gets meshed.
//
// The meshed radius is what the 64-slot mesh pool allows, not a design choice: a column
// of generated terrain has 3 to 5 chunks with blocks in them (surface at y=43..64 over
// stone down to 0, sky above), so 3x3 columns is ~45 slots and 5x5 would be ~125 and
// refuse half of them. Growing this is Phase 6's load/unload ring, which evicts; there is
// no eviction policy here to grow into.
//
// Generation runs one column wider than meshing so every meshed chunk has real neighbours
// on all four sides. Without that ring the mesher reads unloaded chunks as air (world.h)
// and walls the whole area in with faces that are not there — and worse, it bakes AO
// against nothing along every border.
#define GEN_MESH_RADIUS  1                        // 3x3 columns = 48x48 blocks
#define GEN_AREA_RADIUS  (GEN_MESH_RADIUS + 1)    // 5x5 columns generated

// Streaming shares DRAIN_BUDGET_MS / DRAIN_MAX_CHUNKS with the edit queue and takes
// whatever the edit queue leaves; see the comment there for why that replaced a second
// budget of its own in step 6.2.

// Chunks waiting to be meshed into the GPU pool. Kept on the main thread: a mesh writes
// GPU-visible linear memory through the citro3d context, which belongs to the thread that
// created it. Generation is the half that moves (app/worker.h).
static JobQueue s_meshq;

typedef struct {
	bool ran;
	int  columns_failed;   // budget or column table refused a column — a hole in the world
	int  columns_in;       // columns installed into the live world so far
	int  submit_failed;    // the job ring refused a column — also a hole in the world
	int  meshed;           // chunks that got a mesh slot
	int  mesh_refused;

	// Step 6.2's measurement. The frame is 16.71 ms and the streaming path is the only
	// thing on it whose cost is not bounded by a constant, so these are what say whether a
	// budget is needed and, afterwards, whether it worked. Worst rather than average: a
	// stall is by definition the outlier, and an average over six thousand frames hides it.
	float worst_stream_ms;    // install + mesh drain, per frame
	float worst_recenter_ms;  // genFollow, which is ~0 except on the frame that crosses
	int   worst_built;        // most chunks meshed in one frame
} GenResult;

// Updated across frames as columns arrive, so it is a file static rather than a local:
// worldReportDraw takes a pointer to it and is called again once the world is complete.
static GenResult s_genr;

// The whole streaming path is generated-world only. The hand-built world is filled and
// meshed in one call and has no generator to move off the main thread, so under
// BS_WORLD_GEN=0 none of this is compiled in rather than sitting there unreachable.
#if BS_WORLD_GEN

static WorldGen s_gen;

// Which of the meshed columns have already had their chunks queued. Meshing a column needs
// all eight of its neighbours present — the mesher reads an absent column as air and walls
// the chunk in, and the AO pass reads the diagonals too — so a column cannot be queued the
// moment it lands. It is queued when its ring completes, which may be when a *later*
// column arrives.
#define GEN_MESH_SPAN (2 * GEN_MESH_RADIUS + 1)
#define GEN_AREA_SPAN (2 * GEN_AREA_RADIUS + 1)
// The ring follows the player, so these are sliding windows, not grids anchored at the
// origin. Both are indexed modulo their span — a torus — and both store the column
// coordinate the slot currently describes.
//
// Storing the coordinate is what makes the wrap safe. When the centre moves one column, the
// column leaving the far edge and the column entering the near edge land in the *same* slot,
// because the window is exactly as wide as the ring. A slot that only held a flag would need
// the caller to clear it in exactly the right order; a slot that names its own column cannot
// be misread whatever the order, and a stale entry is simply a coordinate mismatch.
//
// The `installed` flag is tracked here rather than asked of the World, and that is not
// redundancy: worldReportBuild claims a 17x17 grid of columns before anything is generated
// and never gives it back, so worldColumn() returns non-NULL for columns 0..16 from the
// first moment. Testing the world called their ring complete before a single block had been
// generated and meshed them against air — 32,358 triangles where the same world meshed after
// full generation is 29,754.
typedef struct {
	int32_t cx, cz;
	bool    set;
} ColSlot;

static ColSlot s_col_in[GEN_AREA_SPAN][GEN_AREA_SPAN];      // generated and installed
static ColSlot s_col_queued[GEN_MESH_SPAN][GEN_MESH_SPAN];  // chunks pushed to the mesh queue
static ColSlot s_col_asked[GEN_AREA_SPAN][GEN_AREA_SPAN];   // submitted to the worker

// The column the ring is centred on, and the streaming counters the overlay reports.
static int32_t s_center_cx, s_center_cz;
static int s_col_unloaded;      // columns freed because they left the ring
static int s_col_dropped;       // columns generated for a ring position that had already moved

// Floor-mod, so a negative column maps into the window rather than off the front of it.
// Both spans are compile-time constants, which matters on ARM11: it has no integer divide
// instruction, and the compiler turns a constant modulus into a multiply and a shift.
static int genWrap(int32_t c, int span)
{
	const int m = (int)(c % span);
	return m < 0 ? m + span : m;
}

// The two windows have different spans, so they are addressed as flat arrays with the span
// passed in. `ColSlot[span][span]` is contiguous, so row * span + col is the same element
// the two-index form would reach.
static ColSlot* genSlot(ColSlot* grid, int span, int32_t cx, int32_t cz)
{
	return &grid[genWrap(cz, span) * span + genWrap(cx, span)];
}

static bool genSlotHas(ColSlot* grid, int span, int32_t cx, int32_t cz)
{
	const ColSlot* s = genSlot(grid, span, cx, cz);
	return s->set && s->cx == cx && s->cz == cz;
}

static void genSlotSet(ColSlot* grid, int span, int32_t cx, int32_t cz)
{
	ColSlot* s = genSlot(grid, span, cx, cz);
	s->cx = cx; s->cz = cz; s->set = true;
}

static void genSlotClear(ColSlot* grid, int span, int32_t cx, int32_t cz)
{
	ColSlot* s = genSlot(grid, span, cx, cz);
	if (s->cx == cx && s->cz == cz) s->set = false;
}

static bool genInArea(int32_t cx, int32_t cz)
{
	const int32_t dx = cx - s_center_cx, dz = cz - s_center_cz;
	return dx >= -GEN_AREA_RADIUS && dx <= GEN_AREA_RADIUS &&
	       dz >= -GEN_AREA_RADIUS && dz <= GEN_AREA_RADIUS;
}

static bool genInMesh(int32_t cx, int32_t cz)
{
	const int32_t dx = cx - s_center_cx, dz = cz - s_center_cz;
	return dx >= -GEN_MESH_RADIUS && dx <= GEN_MESH_RADIUS &&
	       dz >= -GEN_MESH_RADIUS && dz <= GEN_MESH_RADIUS;
}

static bool genColumnInstalled(int32_t cx, int32_t cz)
{
	return genInArea(cx, cz) && genSlotHas(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz);
}

static bool genColumnRingComplete(int32_t cx, int32_t cz)
{
	for (int dz = -1; dz <= 1; dz++)
		for (int dx = -1; dx <= 1; dx++)
			if (!genColumnInstalled(cx + dx, cz + dz)) return false;
	return true;
}

// Queues every column inside the meshed radius whose ring is now complete and that has not
// been queued already.
static void genQueueReadyColumns(void)
{
	for (int32_t dz = -GEN_MESH_RADIUS; dz <= GEN_MESH_RADIUS; dz++) {
		for (int32_t dx = -GEN_MESH_RADIUS; dx <= GEN_MESH_RADIUS; dx++) {
			const int32_t cx = s_center_cx + dx, cz = s_center_cz + dz;
			if (genSlotHas(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz)) continue;
			if (!genColumnRingComplete(cx, cz)) continue;
			genSlotSet(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);

			for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
				// Skipped rather than left to chunkRenderBuild's own all-air early-out,
				// which claims a slot for the empty chunk it finds. That is right for a
				// chunk the player dug out — it has to stay queueable — and wrong here,
				// where three or four sky chunks per column would eat the pool before the
				// ground was meshed.
				const Chunk* c = worldChunk(&s_world, cx, cy, cz);
				if (!c || chunkIsAllAir(c)) continue;

				const Job j = {JOB_MESH, cx, cz, cy};
				if (!jobqPush(&s_meshq, j)) s_genr.mesh_refused++;
			}
		}
	}
}

// Submits every column of the current area that has neither arrived nor been asked for.
static void genRequestArea(void)
{
	for (int32_t dz = -GEN_AREA_RADIUS; dz <= GEN_AREA_RADIUS; dz++) {
		for (int32_t dx = -GEN_AREA_RADIUS; dx <= GEN_AREA_RADIUS; dx++) {
			const int32_t cx = s_center_cx + dx, cz = s_center_cz + dz;
			if (genSlotHas(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz)) continue;
			if (genSlotHas(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz)) continue;

			if (workerSubmitColumn(cx, cz)) genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
			else                            s_genr.submit_failed++;
		}
	}
}

// Frees a column and everything drawn from it. Safe to call for a column that was only
// asked for and never arrived.
static void genUnloadColumn(int32_t cx, int32_t cz)
{
	// Meshes first. A slot outliving its chunks would keep drawing terrain that is not
	// there, and would still match in chunkRenderTouch — so an edit near the boundary
	// would queue a remesh of a chunk that now reads as air and blank a neighbour.
	chunkRenderReleaseColumn(cx, cz);
	genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);

	if (worldColumnRemove(&s_world, cx, cz)) s_col_unloaded++;
	genSlotClear(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz);
	genSlotClear(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
}

// Moves the ring to a new centre column: drop what fell outside it, then ask for what is
// newly inside. Dropping first is not cosmetic — the windows are exactly as wide as the
// ring, so the column leaving and the column entering share a slot.
static void genRecenter(int32_t cx, int32_t cz)
{
	if (cx == s_center_cx && cz == s_center_cz) return;

	const int32_t old_cx = s_center_cx, old_cz = s_center_cz;
	s_center_cx = cx;
	s_center_cz = cz;

	// Only the old area needs sweeping: nothing outside it was ever loaded.
	for (int32_t dz = -GEN_AREA_RADIUS; dz <= GEN_AREA_RADIUS; dz++)
		for (int32_t dx = -GEN_AREA_RADIUS; dx <= GEN_AREA_RADIUS; dx++) {
			const int32_t ox = old_cx + dx, oz = old_cz + dz;
			if (!genInArea(ox, oz)) genUnloadColumn(ox, oz);
		}

	// A column that left the *mesh* ring but is still inside the generated ring keeps its
	// blocks and loses its geometry: it is now one of the neighbours the mesher reads, not
	// something drawn. Without this its meshes would stay on screen past the render
	// distance and the pool would fill.
	for (int32_t dz = -GEN_MESH_RADIUS; dz <= GEN_MESH_RADIUS; dz++)
		for (int32_t dx = -GEN_MESH_RADIUS; dx <= GEN_MESH_RADIUS; dx++) {
			const int32_t ox = old_cx + dx, oz = old_cz + dz;
			if (genInMesh(ox, oz)) continue;
			chunkRenderReleaseColumn(ox, oz);
			genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, ox, oz);
		}

	genRequestArea();
	genQueueReadyColumns();
}

// Starts the worker and queues the whole area. The spawn column goes first because the
// ring is FIFO, so it is the first one back and the player has ground under them before
// the loop starts.
static bool genStart(int32_t cx, int32_t cz)
{
	worldgenInit(&s_gen, BS_WORLD_SEED);
	jobqInit(&s_meshq);
	memset(s_col_queued, 0, sizeof(s_col_queued));
	memset(s_col_in, 0, sizeof(s_col_in));
	memset(s_col_asked, 0, sizeof(s_col_asked));
	s_genr = (GenResult){0};
	s_genr.ran = true;
	s_center_cx = cx;
	s_center_cz = cz;
	s_col_unloaded = s_col_dropped = 0;

	if (!workerStart(&s_gen))
		return false;

	if (workerSubmitColumn(cx, cz)) genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
	else                            s_genr.submit_failed++;
	genRequestArea();

	return true;
}

// One column per call, which is all the handshake in app/worker.h can deliver. False when
// nothing was waiting.
static bool genInstallOne(void)
{
	int32_t cx = 0, cz = 0;
	bool ok = false;
	if (!workerInstall(&s_world, &cx, &cz, &ok))
		return false;

	s_genr.columns_in++;
	if (!ok) s_genr.columns_failed++;

	// The ring can have moved while this column was being generated, and the worker has no
	// idea: it took the job before the player crossed the boundary. Installing it anyway
	// would leak a column that no unload sweep will ever visit again, because every sweep
	// only walks the ring.
	if (!genInArea(cx, cz)) {
		worldColumnRemove(&s_world, cx, cz);
		genSlotClear(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
		s_col_dropped++;
		return true;
	}

	// Marked before the ring is tested, so the column that completes a ring counts towards
	// it. A partial column (ok == false) is still marked: it is a hole either way, it is
	// already counted in columns_failed, and leaving it unmarked would stall its four
	// neighbours forever instead of meshing what did arrive.
	genSlotSet(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz);
	genSlotClear(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);

	genQueueReadyColumns();
	return true;
}

// Meshes queued chunks until the budget is spent, max_chunks have been built, or the queue
// runs dry. Returns how many it built.
//
// Same two limits as chunkRenderDrainDirty, for the same reason: the clock can only stop
// this after a chunk has already overrun, so the count is what makes the worst case a
// number that can be written down. Also the same guarantee — at least one chunk whenever
// the queue is non-empty, whatever the limits say, or newly generated terrain could stay
// invisible indefinitely while the world underneath it is perfectly real.
static int genDrainMesh(float budget_ms, int max_chunks)
{
	if (jobqCount(&s_meshq) == 0)
		return 0;

	const u64 t0 = svcGetSystemTick();
	int built = 0;
	Job j;
	while (jobqPop(&s_meshq, &j)) {
		if (chunkRenderBuild(&s_world, j.cx, j.cy, j.cz)) s_genr.meshed++;
		else                                              s_genr.mesh_refused++;
		built++;

		if (built >= max_chunks) break;

		const float spent = (float)((double)(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
		if (spent >= budget_ms) break;
	}
	return built;
}

// Blocks until the column the player spawns in has been installed, or until the worker has
// run out of work to give. Only that one column: the other twenty-four arrive over the
// first frames, which is the whole point of the step. Without this the player would spawn
// above nothing and fall to the world floor before the ground under them existed.
static void genWaitSpawnColumn(void)
{
	while (!genColumnInstalled(s_center_cx, s_center_cz)) {
		if (genInstallOne()) continue;
		if (!workerBusy()) break;      // nothing more is coming; do not spin forever
		svcSleepThread(1000000);       // 1 ms
	}
}

// Called every frame with the player's feet. Cheap when nothing has changed, which is the
// normal case: at the measured 4.31 blocks/s a column boundary comes round every 3.7 s, so
// this is 221 comparisons for every one recentre.
static int32_t genColumnOf(float v)
{
	// Two flooring steps, and neither is optional. A cast truncates *towards zero*, so
	// (int)-0.5f is 0 where the block at -0.5 is block -1; and the block-to-chunk step is
	// an arithmetic shift rather than a divide for the same reason — -1 / 16 is 0, -1 >> 4
	// is -1. Getting either wrong puts the ring one column off on the negative side only,
	// which reads as "the world streams fine until you walk west".
	int b = (int)v;
	if (v < 0.0f && (float)b != v) b--;
	return (int32_t)(b >> 4);
}

static void genFollow(float x, float z)
{
	genRecenter(genColumnOf(x), genColumnOf(z));
}

#endif   // BS_WORLD_GEN

// `built` and `mesh_refused` are startup facts, so they belong in the report drawn once
// rather than on the per-frame status line: the console is 32 columns and the status line
// needs all of them for the aim, edit and queue counters that change every frame.
static void worldReportDraw(int refused, bool built,
                            const char* selftest, const StressResult* st,
                            const DigOutResult* dig, const GenResult* gen)
{
	const float mb = 1024.0f * 1024.0f;

	printf("\x1b[%d;1H", BS_REPORT_ROW);
	printf("%s %s\n", gen->ran ? "PHASE 5  generated " : "PHASE 4  handbuilt ",
	       built ? "fill ok   " : "FILL FAILED");
	printf("--------------------------------\n");
	printf("columns %4d   chunks %6d    \n", s_world.columns, s_world.chunks);
	printf("blocks %5.2f MB of %5.2f MB cap \n", worldBytes(&s_world) / mb,
	       budgetCap() / mb);
	printf("peak   %5.2f MB  refused %4d   \n", budgetPeak() / mb, refused);
	printf("meshes %4d  tris %6lu       \n", chunkRenderMeshes(),
	       (unsigned long)chunkRenderTris());
	printf("vbo pool %5.2f MB  refused %2d  \n", chunkRenderBytes() / mb,
	       chunkRenderRefusals());
	printf("startup mesh refused %2d        \n", gen->mesh_refused);

	// The two equalities are the whole point: same free bytes after a thousand
	// remeshes, and an incrementally remeshed pool identical to a fully rebuilt one.
	if (!st->ran) {
		printf("remesh stress off (see main.c)  \n");
		printf("                                \n");
		printf("                                \n");
	} else {
		printf("remesh %4d ed %5d rb %5.0f ms\n", st->edits, st->rebuilt, st->ms);
		printf("linear %8lu ->%8lu %s\n", (unsigned long)st->free_before,
		       (unsigned long)st->free_after,
		       st->free_before == st->free_after ? "FLAT" : "LEAK");
		printf("mesh %08lX vs %08lX %s\n", (unsigned long)st->sum_incremental,
		       (unsigned long)st->sum_full,
		       st->sum_incremental == st->sum_full ? "SAME" : "DRIFT");
		printf("per chunk %5.0f fill %5.0f mesh us\n", st->scratch_us, st->mesh_us);
	}
	// `in` is columns installed into the live world and `fail` is those the budget, the
	// column table or the job ring refused, so a non-zero there is a hole in the ground
	// rather than a slow boot; `mesh` is how much of the 64-slot pool the generated area
	// actually needed, which is the number that decides how far GEN_MESH_RADIUS can grow
	// before Phase 6's eviction exists.
	//
	// The second line is step 5.5's whole claim in two numbers. `wk` is wall time the
	// worker spent inside the generator and `in` the time the main thread spent copying
	// finished columns across: the first is the cost that used to sit in front of the
	// first frame, the second is all that is left of it on the frame path. Read them
	// against `worst` and `drop` on the overlay — a large `wk` with `drop 0` is the
	// measurement, and a `q` that never reaches 0 would mean the worker is not keeping up.
	if (gen->ran) {
		printf("gen s%-5lu in %2d fail %d mesh %3d\n", (unsigned long)BS_WORLD_SEED,
		       gen->columns_in, gen->columns_failed + gen->submit_failed, gen->meshed);
		printf("wk %6.0f inst %5.1f ms q%-2d d%d \n", workerBusyMs(), workerInstallMs(),
		       workerQueued(), workerDropped());
#if BS_WORLD_GEN
		// Step 6.1's line. `at` is the column the ring is centred on, `unl` the columns
		// freed because they left it, and `stale` the columns that finished generating for
		// a ring position the player had already walked out of — those are thrown away on
		// arrival, and a large number there means the ring is moving faster than the worker.
		printf("ring at %+3ld %+3ld unl %3d stale %d\n", (long)s_center_cx, (long)s_center_cz,
		       s_col_unloaded, s_col_dropped);

		// Step 6.2's line, and the only per-frame costs the streaming path adds. `strm` is
		// install + mesh drain on the worst frame, `rc` the recentre on the frame that
		// crossed a column boundary, `qp` the deepest the mesh queue ever got and `n` the
		// most chunks meshed in one frame. Read `strm` against the 16.71 ms frame.
		printf("strm %5.2f rc %5.2f qp%3d n%d  \n", gen->worst_stream_ms,
		       gen->worst_recenter_ms, jobqPeak(&s_meshq), gen->worst_built);
#endif
	}

	if (dig->ran)
		printf("digout q%d>%d t%lu>%lu %s\n", dig->queued_before, dig->queued_after,
		       (unsigned long)dig->tris_empty, (unsigned long)dig->tris_back,
		       dig->pass ? "PASS" : "FAIL");
	printf("selftest %-22s\n", selftest);
}

int main(void)
{
	screenInit();
	metricsInit();

	if (!chunkRenderInit()) {
		printf("\x1b[2;1Hshader/atlas/pool init FAILED\n");
		printf("press START to exit\n");
		while (aptMainLoop()) {
			hidScanInput();
			if (hidKeysDown() & KEY_START) break;
			gspWaitForVBlank();
		}
		metricsExit();
		screenExit();
		return 1;
	}

	// Self-test first: it resets the memory budget, so it has to run before the
	// world claims anything.
	char selftest[96] = "";
	worldTestRun(selftest, sizeof(selftest), NULL);
	// Onto the overlay as well as into the BS_* report line. The report path is a
	// developer build; a normal Phase 4 boot never showed this result anywhere, which is
	// why "did the suite pass on the console?" was unanswerable rather than merely
	// awkward to read.
	metricsSetSelfTest(selftest);

	const int refused = worldReportBuild();

	// The world. Since step 5.5 the generated one is not built here: the worker is started
	// and the whole area queued, and only the column the player stands in is waited for.
	// The other twenty-four arrive during the first frames, one per frame, while the game
	// is already drawing. The hand-built world has no generator to move off the main
	// thread and is still filled and meshed in one go.
#if BS_WORLD_GEN
	// Centred on the spawn column, which is (0, 0) — the same column playerInit puts the
	// feet in below. From here on the ring follows the player (genFollow).
	const bool worker_ok = genStart(0, 0);
	genWaitSpawnColumn();
#else
	s_genr = (GenResult){0};
	const bool handbuilt_ok = handbuiltFill(&s_world);
	s_genr.mesh_refused = meshHandbuilt();
#endif

	// Runs once, before the first frame, so the numbers on screen are a completed
	// measurement rather than something still moving while he reads it.
	const StressResult stress = remeshStress(BS_REMESH_STRESS);

	// After the stress, so it starts from a world the stress has already put back, and
	// before the highlight and the player so nothing it edits is on screen mid-check.
	const DigOutResult dig = digOutCheck(BS_DIG_OUT);

	// The highlight is not load-bearing: if its shader or buffer fails, the game is still
	// playable without a cage round the target block, so this reports and carries on
	// rather than joining chunkRenderInit's fatal path above.
	const bool highlight_ok = highlightInit();

	// On the flat ground at the foot of the stepped pyramid, looking down at it along the
	// diagonal — so the first frame already shows a target cage without anyone having to
	// walk anywhere, and walking forward goes up the pyramid.
	//
	// All three numbers here were measured on the host, not guessed, because two of the
	// obvious choices are wrong:
	//
	//  - (8,8), the middle of the flat ground, was inside the 3-deep test pit back when the
	//    pit sat at 6..11 with sheer walls: tryStepUp climbs exactly one block and a jump
	//    clears only PLAYER_JUMP_SPEED^2 / (2 * -PLAYER_GRAVITY) = 8.5^2 / 56 = 1.29
	//    blocks, so the player spawned trapped in the sand. That trap is gone — the pit
	//    moved to 1..6 and its walls now step one block per ring — but the reasoning is
	//    kept because it is why the spawn is not simply "the middle".
	//  - (26,26), the pyramid apex, looks ideal and is not: the pyramid rises one block
	//    per ring, and at pitch 35 the aim ray descends 0.99 blocks per ring, so the ray
	//    runs *parallel* to the slope and skims it. First hit is 9.50 blocks out, past
	//    INTERACT_REACH, so there is no target at all on the first frame.
	//
	// From (18,18) at pitch 35 the ray hits (20,11,20) face TOP at 2.82 blocks, and the
	// body sits at y=12 on_ground for 600 ticks without moving. Since 2026-08-18 the whole
	// hand-built area is walkable — PLATEAU_STEP is 1 and the pit is stepped, and
	// testHandbuiltWalkable asserts no two adjacent columns differ by more than one block
	// — so this spawn is now about what is on screen in frame one, not about safety.
	//
	// On generated terrain none of that applies: (8, 8) is the middle of column (0, 0),
	// which is the middle of the meshed 3x3 block of columns, and the surface height comes
	// from the generator rather than a table. Nothing there is a trap — testWorldgenTerrain
	// asserts no two adjacent columns anywhere in a 121x120 sample differ by more than one
	// block, so the whole generated surface is walkable by the measured one-block auto-step.
#if BS_WORLD_GEN
	const int spawn_x = 8, spawn_z = 8;
	const int spawn_y = worldgenHeight(&s_gen, spawn_x, spawn_z);
#else
	const int spawn_x = 18, spawn_z = 18;
	const int spawn_y = handbuiltHeight(spawn_x, spawn_z);
#endif

	Player player;
	playerInit(&player, (float)spawn_x + 0.5f, (float)spawn_y, (float)spawn_z + 0.5f,
	           C3D_AngleFromDegrees(135.0f), C3D_AngleFromDegrees(35.0f));

	Interact it;
	interactInit(&it);

	// "Did the world come out whole?" — the meaning differs between the two paths. The
	// hand-built world is filled in one call that either worked or did not; the generated
	// one is whole only if nothing refused a column on the way in, which is not known
	// until the last one lands, so this is recomputed for the redraw below.
#if BS_WORLD_GEN
	#define WORLD_INTACT() (worker_ok && s_genr.columns_failed == 0 && \
	                        s_genr.submit_failed == 0)
#else
	#define WORLD_INTACT() (handbuilt_ok)
#endif

	// Drawn once here with the spawn column in, and once more when the world is complete —
	// the counters on it move for the first twenty-five frames now, and a report frozen at
	// "1 column in" would read as a failed boot.
	worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr);
	bool report_final = false;

	// The queue's peak is only meaningful from the first real frame onwards: the startup
	// build above went through chunkRenderBuild, not the queue, but the step 3.6 corner
	// edit did touch it, and a peak carried over from before the loop would be reported as
	// if the player had caused it.
	chunkRenderDirtyResetPeak();

	// Only the two scripted knobs read this, so a playtest build does not carry it.
#if BS_INTERACT_DEMO || BS_EDIT_STRESS || BS_WALK_STRESS
	int frame = 0;
#endif

	while (aptMainLoop()) {
		metricsFrameBegin();
#if BS_INTERACT_DEMO || BS_EDIT_STRESS || BS_WALK_STRESS
		frame++;
#endif

		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) break;

#if BS_WORLD_GEN
		// Take delivery of at most one generated column, before anything reads the world
		// this frame. One column is all the worker can hand over per install
		// (app/worker.h). Meshing what that made ready happens further down, with the
		// edit queue, so the two share one budget rather than one each — see
		// DRAIN_BUDGET_MS. Install stays here because it is what puts ground under the
		// player, and it is a memcpy, not a mesh: 15.3 ms over 125 columns, 0.12 ms each.
		const u64 t_install = svcGetSystemTick();
		genInstallOne();
		const float install_ms =
			(float)((double)(svcGetSystemTick() - t_install) / CPU_TICKS_PER_MSEC);
#endif

		C3D_Mtx view;
#if BS_FLY
		cameraUpdate(&player.cam, metricsFrameMs());
#else
		playerUpdate(&player, &s_world, metricsFrameMs());
#endif
#if BS_WALK_STRESS
		// Clear the startup frame's stall out of `worst` first, for the same reason
		// BS_EDIT_STRESS does: it stands for the whole run otherwise, and the streaming
		// cost this probe exists to measure could not be told apart from it.
		if (frame == DEMO_START_FRAME) {
			metricsWorstReset();
			// Same argument for the streaming worsts: the boot meshes twenty-five columns
			// in a burst that no walk reproduces, and it would stand for the whole run.
			s_genr.worst_stream_ms   = 0.0f;
			s_genr.worst_recenter_ms = 0.0f;
			s_genr.worst_built       = 0;
		}

		if (frame > DEMO_START_FRAME && frame <= DEMO_START_FRAME + BS_WALK_STRESS) {
			// The same clamp playerUpdate applies (its MAX_TICK), so a long frame moves the
			// player the distance a real one would instead of teleporting them across a
			// ring boundary and skipping the recentre being measured.
			float dt = metricsFrameMs() * 0.001f;
			if (dt > 0.05f) dt = 0.05f;

			player.body.vx = PLAYER_WALK_SPEED;
			player.body.vz = 0.0f;
			bodyStep(&player.body, &s_world, dt);

			// playerUpdate does this after its own step; the camera would otherwise sit a
			// frame behind the body, and genFollow below would centre on the old column.
			player.cam.x = player.body.x;
			player.cam.y = player.body.y + PLAYER_EYE;
			player.cam.z = player.body.z;

#if BS_WALK_EDIT && BS_WORLD_GEN
			// The corner of the chunk the player is standing in, one block above the
			// surface — found by scanning down rather than assumed, because the terrain
			// under a walking player is generated and its height is not a constant.
			const int bx = (int)genColumnOf(player.body.x) * CHUNK_DIM;
			const int bz = (int)genColumnOf(player.body.z) * CHUNK_DIM;
			int by = WORLD_HEIGHT - 2;
			while (by > 0 && worldGet(&s_world, bx, by, bz) == BLOCK_AIR) by--;

			worldSet(&s_world, bx, by + 1, bz, (frame & 1) ? BLOCK_STONE : BLOCK_AIR);
			chunkRenderTouch(&s_world, bx, by + 1, bz);
#endif
		}
#endif

		cameraView(&player.cam, &view);

#if BS_WORLD_GEN
		// After the move, so the ring is centred on where the player is now rather than
		// where they were last frame — one frame of lag here is one frame of the player
		// standing on a column that has just been unloaded. Before the aim and the edit,
		// so nothing this frame reads a column the recentre is about to free.
		const u64 t_rc = svcGetSystemTick();
		genFollow(player.body.x, player.body.z);
		const float rc_ms = (float)((double)(svcGetSystemTick() - t_rc) / CPU_TICKS_PER_MSEC);
		if (rc_ms > s_genr.worst_recenter_ms) s_genr.worst_recenter_ms = rc_ms;
#endif

		// Aim first, then edit, so the highlight and the edit in the same frame cannot
		// disagree about which block was being pointed at.
		interactAim(&it, &s_world, &player.cam);

#if BS_INTERACT_DEMO
		if (frame == DEMO_START_FRAME)     down |= INTERACT_KEY_BREAK;
		if (frame == DEMO_START_FRAME + 30) down |= INTERACT_KEY_PLACE;
#endif
		interactEdit(&it, &s_world, &player.body, down);

#if BS_EDIT_STRESS
		// The 4.5 check: an edit on a chunk corner every frame, which dirties eight
		// chunks a frame against a 4 ms budget that clears two or three. The queue is
		// meant to hold the backlog and keep the frame inside 16.71 ms; worst/over on the
		// overlay is the answer, and dirty/peak says whether it is genuinely backed up.
		// Clear the startup frame's 20 ms out of `worst` first, or it stands for the whole
		// run and the stress cannot be told apart from it.
		if (frame == DEMO_START_FRAME) metricsWorstReset();

		if (frame > DEMO_START_FRAME && frame <= DEMO_START_FRAME + BS_EDIT_STRESS) {
			const int sx = CHUNK_DIM, sz = CHUNK_DIM;
			const int sy = handbuiltHeight(sx, sz);
			worldSet(&s_world, sx, sy, sz, (frame & 1) ? BLOCK_STONE : BLOCK_AIR);
			chunkRenderTouch(&s_world, sx, sy, sz);
		}
#endif

		// All of this frame's meshing, under one budget. Edits first: a broken block that
		// takes two frames to disappear is felt, and a chunk of scenery that takes two
		// frames to arrive at the edge of the render distance is not.
		const u64 t_work = svcGetSystemTick();
		const int edit_built = chunkRenderDrainDirty(&s_world, DRAIN_BUDGET_MS,
		                                             DRAIN_MAX_CHUNKS);
		const float edit_ms =
			(float)((double)(svcGetSystemTick() - t_work) / CPU_TICKS_PER_MSEC);

#if BS_WORLD_GEN
		// Whatever is left of both limits. Negative time and a zero count are handled the
		// same way at the far end — one chunk still completes — so no clamping here.
		const int stream_built = genDrainMesh(DRAIN_BUDGET_MS - edit_ms,
		                                      DRAIN_MAX_CHUNKS - edit_built);
		const float work_ms =
			(float)((double)(svcGetSystemTick() - t_work) / CPU_TICKS_PER_MSEC) + install_ms;

		if (work_ms > s_genr.worst_stream_ms) s_genr.worst_stream_ms = work_ms;
		if (edit_built + stream_built > s_genr.worst_built)
			s_genr.worst_built = edit_built + stream_built;
#endif

		metricsSyncBegin();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		metricsSyncEnd();
			C3D_RenderTargetClear(screenTop(), C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(screenTop());
			chunkRenderDraw(&view);
			for (int i = 0; i < BS_GPU_STRESS; i++) chunkRenderDraw(&view);

			// After the world, because it re-binds the GPU for its own vertex format and
			// does not put it back — chunkRenderDraw re-establishes everything it needs at
			// the top of its next call instead. See pipelineBind in chunk_render.c.
			// highlightDraw itself now checks it.target.hit (see highlight.h), so there is
			// no guard here to keep in sync with it.
			highlightDraw(&view, &it.target);
		metricsSubmitBegin();
		C3D_FrameEnd(0);
		metricsSubmitEnd();

		metricsFrameEnd();

		// One redraw when the world is finally complete: worker idle, nothing left to
		// install, nothing left to mesh. Once, not every frame — the console text render
		// is expensive enough that drawing it per frame would become the thing being
		// measured, which is why metricsDrawOverlay rate-limits itself.
		if (!report_final && !workerBusy() && jobqCount(&s_meshq) == 0) {
			report_final = true;
			worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr);
		}

#if BS_REPORT_ONLY
		// Verification build: the overlay is suppressed so the whole report fits in
		// a short emulator window. Redrawn each frame because nothing else is.
		worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr);
#else
		// Phase 4's status line. Every field answers a question a screenshot would
		// otherwise leave open: whether the ray found anything (`aim` block and face),
		// whether an edit reached the world (`b`roke / `p`laced / `r`efused), and whether
		// the queue is keeping up (`dq` current/peak) — read that against `worst` and
		// `over` above. The console is 32 columns wide and truncates without complaining,
		// so this has to stay inside it: "aim -12 8 -10 f2 b9 p9 r9 dq8/8" is 31.
		char status[33];
		if (it.target.hit)
			snprintf(status, sizeof(status), "aim %d %d %d f%d b%d p%d r%d dq%d/%d",
			         it.target.x, it.target.y, it.target.z, it.target.face,
			         it.broke, it.placed, it.refused,
			         chunkRenderDirtyCount(), chunkRenderDirtyPeak());
		else
			snprintf(status, sizeof(status), "aim none b%d p%d r%d dq%d/%d",
			         it.broke, it.placed, it.refused,
			         chunkRenderDirtyCount(), chunkRenderDirtyPeak());

		// Init failures belong on screen, not in a comment: without this the only symptom
		// of a broken highlight shader is "the cage does not appear", which is
		// indistinguishable from aiming at nothing.
		metricsDrawOverlay(highlight_ok ? status : "HIGHLIGHT INIT FAILED");
#endif
	}

	// Before worldExit: the worker holds a staging world of its own and must be joined
	// before anything it could still be writing into is freed.
	workerStop();

	worldExit(&s_world);
	highlightExit();
	chunkRenderExit();
	metricsExit();
	screenExit();
	return 0;
}
