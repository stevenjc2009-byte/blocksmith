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

#include "debug/metrics.h"
#include "gfx/screen.h"
#include "scene/camera.h"
#include "scene/chunk_render.h"
#include "scene/highlight.h"
#include "scene/interact.h"
#include "scene/player.h"
#include "world/budget.h"
#include "world/handbuilt.h"
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

// How much of each frame may be spent remeshing what an edit dirtied. The frame is
// 16.71 ms at 59.83 Hz and steady-state CPU work measured 0.24 ms with a 0.37 ms submit,
// so 4 ms is a quarter of the frame handed to the queue and still leaves three quarters
// of it unaccounted for. At ~1.47 ms a chunk that clears two or three chunks a frame, so
// the worst single edit — a chunk corner, eight chunks — lands over three frames instead
// of stalling one for 11.8 ms.
#define DRAIN_BUDGET_MS  4.0f

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
		chunkRenderDrainDirty(&s_world, 1000.0f);

		worldSet(&s_world, x, y, z, was);
		r.rebuilt += chunkRenderTouch(&s_world, x, y, z);
		chunkRenderDrainDirty(&s_world, 1000.0f);
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
	chunkRenderDrainDirty(&s_world, 1000.0f);   // a straggler would make the two hashes
	r.sum_incremental = chunkRenderChecksum();  // disagree for a reason that is not a bug

	meshHandbuilt();
	r.sum_full = chunkRenderChecksum();

	// Put the world back, rebuilding from the real block data rather than trusting the
	// touch list to undo itself: what the report says must not depend on the thing the
	// report is testing.
	worldSet(&s_world, bx, by, bz, BLOCK_AIR);
	chunkRenderDrainDirty(&s_world, 1000.0f);
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
	chunkRenderDrainDirty(&s_world, 1000.0f);

	// Dig the whole chunk out, then remesh it through the real path.
	for (int y = 0; y < CHUNK_DIM; y++)
		for (int z = 0; z < CHUNK_DIM; z++)
			for (int x = 0; x < CHUNK_DIM; x++)
				worldSet(&s_world, cx * CHUNK_DIM + x, cy * CHUNK_DIM + y,
				         cz * CHUNK_DIM + z, BLOCK_AIR);
	chunkRenderTouch(&s_world, tx, ty, tz);
	chunkRenderDrainDirty(&s_world, 1000.0f);
	r.tris_empty = chunkRenderTris();

	// Put one block back in the middle of the emptied chunk. All six of its neighbours
	// are air, so if it is meshed at all the triangle count must rise by twelve.
	worldSet(&s_world, tx, 5, tz, BLOCK_STONE);
	r.queued_after = chunkRenderTouch(&s_world, tx, 5, tz);
	chunkRenderDrainDirty(&s_world, 1000.0f);
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

static WorldGen s_gen;

typedef struct {
	bool ran;
	int  columns_failed;   // budget or column table refused a column — a hole in the world
	int  meshed;           // chunks that got a mesh slot
	int  mesh_refused;
} GenResult;

static GenResult genBuild(void)
{
	GenResult r = {0};
	r.ran = true;

	worldgenInit(&s_gen, BS_WORLD_SEED);
	r.columns_failed = worldgenArea(&s_gen, &s_world, 0, 0, GEN_AREA_RADIUS);

	const int refused_before = chunkRenderRefusals();
	for (int cz = -GEN_MESH_RADIUS; cz <= GEN_MESH_RADIUS; cz++)
		for (int cx = -GEN_MESH_RADIUS; cx <= GEN_MESH_RADIUS; cx++)
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
				// Skipped rather than left to chunkRenderBuild's own all-air early-out,
				// which claims a slot for the empty chunk it finds. That is right for a
				// chunk the player dug out — it has to stay queueable — and wrong here,
				// where three or four sky chunks per column would eat the pool before the
				// ground was meshed. worldReportBuild has already allocated every chunk in
				// this area, so "not allocated" is not available as the test.
				const Chunk* c = worldChunk(&s_world, cx, cy, cz);
				if (!c || chunkIsAllAir(c)) continue;
				if (chunkRenderBuild(&s_world, cx, cy, cz)) r.meshed++;
			}
	r.mesh_refused = chunkRenderRefusals() - refused_before;

	return r;
}

// `built` and `mesh_refused` are startup facts, so they belong in the report drawn once
// rather than on the per-frame status line: the console is 32 columns and the status line
// needs all of them for the aim, edit and queue counters that change every frame.
static void worldReportDraw(int refused, bool built, int mesh_refused,
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
	printf("startup mesh refused %2d        \n", mesh_refused);

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
	// `fail` is columns the budget or the column table refused, so a non-zero there is a
	// hole in the ground rather than a slow boot; `mesh` is how much of the 64-slot pool
	// the generated area actually needed, which is the number that decides how far
	// GEN_MESH_RADIUS can grow before Phase 6's eviction exists.
	if (gen->ran)
		printf("gen s%-5lu fail %d mesh %3d\n", (unsigned long)BS_WORLD_SEED,
		       gen->columns_failed, gen->meshed);

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

	// The world, and the meshes for it. Both happen once, before the loop: chunk
	// streaming is Phase 6, so nothing here is meshed on the fly.
#if BS_WORLD_GEN
	const GenResult gen = genBuild();
	const bool built = (gen.columns_failed == 0);
	const int  mesh_refused = gen.mesh_refused;
#else
	const GenResult gen = {0};
	const bool built = handbuiltFill(&s_world);
	const int  mesh_refused = meshHandbuilt();
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

	// Static report — nothing in it changes, so it is drawn once rather than every
	// frame, and the metrics overlay above it never touches these rows.
	worldReportDraw(refused, built, mesh_refused, selftest, &stress, &dig, &gen);

	// The queue's peak is only meaningful from the first real frame onwards: the startup
	// build above went through chunkRenderBuild, not the queue, but the step 3.6 corner
	// edit did touch it, and a peak carried over from before the loop would be reported as
	// if the player had caused it.
	chunkRenderDirtyResetPeak();

	// Only the two scripted knobs read this, so a playtest build does not carry it.
#if BS_INTERACT_DEMO || BS_EDIT_STRESS
	int frame = 0;
#endif

	while (aptMainLoop()) {
		metricsFrameBegin();
#if BS_INTERACT_DEMO || BS_EDIT_STRESS
		frame++;
#endif

		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) break;

		C3D_Mtx view;
#if BS_FLY
		cameraUpdate(&player.cam, metricsFrameMs());
#else
		playerUpdate(&player, &s_world, metricsFrameMs());
#endif
		cameraView(&player.cam, &view);

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

		chunkRenderDrainDirty(&s_world, DRAIN_BUDGET_MS);

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

#if BS_REPORT_ONLY
		// Verification build: the overlay is suppressed so the whole report fits in
		// a short emulator window. Redrawn each frame because nothing else is.
		worldReportDraw(refused, built, mesh_refused, selftest, &stress, &dig, &gen);
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

	worldExit(&s_world);
	highlightExit();
	chunkRenderExit();
	metricsExit();
	screenExit();
	return 0;
}
