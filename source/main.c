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
#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/crash.h"
#include "app/input_map.h"
#include "app/options.h"
#include "app/updater.h"
#include "app/gputest.h"
#include "app/watchdog.h"
#include "app/worker.h"
#include "debug/metrics.h"
#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/screen.h"
#include "gfx/sprite.h"
#include "net/bsnet.h"
#include "net/networld.h"
#include "scene/camera.h"
#include "scene/chunk_render.h"
#include "scene/highlight.h"
#include "scene/playermodel.h"
#include "scene/interact.h"
#include "scene/loading.h"
#include "scene/loading_draw.h"
#include "scene/player.h"
#include "scene/title.h"
#include "scene/ui.h"
#include "world/budget.h"
#include "world/handbuilt.h"
#include "world/inventory.h"
#include "world/jobq.h"
#include "world/mesh_vertex.h"
#include "world/region.h"
#include "world/world.h"
#include "world/world_test.h"
#include "world/worldgen.h"

// Dark teal — deliberately not the bottom screen's blue. It moved into
// scene/chunk_render.h at step 6.4 because the fog has to fade to exactly this colour, and
// two copies of one constant in two files is how they end up different.
#define CLEAR_COLOR SKY_CLEAR_RGBA8

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
// submit. A chunk build costs 917 us to mesh (step 9.4c measurement) plus the 0.63 ms step 7.3's
// visibility flood fill added to every build (chunkRenderVisUs), so ~1.55 ms. The worst case can
// still be written down — 3 chunks, plus the one chunk each drain is always allowed to complete,
// is ~6.2 ms and cannot grow — but it is now lower than before. It still fits (6.2 + 0.62 + 0.37
// = 7.19 of 16.71) and the constants are left alone; if that stops being true, DRAIN_MAX_CHUNKS
// is the knob, at the cost of a slower drain rather than a dropped frame.
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
// Step 7.7 made both of these a runtime setting rather than a constant. The bound above is
// what the arrays and the mesh pool are sized for; the current value is what the loops use.
// See scene/render_dist.h for why RENDER_DIST_MAX is 2 and what else moves with it.
#define GEN_MESH_RADIUS_MAX  RENDER_DIST_MAX
#define GEN_AREA_RADIUS_MAX  (GEN_MESH_RADIUS_MAX + 1)

static int s_mesh_radius = RENDER_DIST_MIN;                   // 3x3 columns = 48x48 blocks
static int s_area_radius = RENDER_DIST_MIN + 1;               // 5x5 columns generated

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
	int   fill_frames;        // frames before the world was first complete — step 6.3
} GenResult;

// Updated across frames as columns arrive, so it is a file static rather than a local:
// worldReportDraw takes a pointer to it and is called again once the world is complete.
static GenResult s_genr;

// Step 8.1. Makes sure sdmc:/blocksmith/worlds/<name> exists and returns it, or NULL if the
// card would not take it — a locked or absent SD card, which is a real state on this console
// and must leave the game playable rather than refusing to boot. NULL turns saving off for
// the session: the world still generates, edits still work, they just do not survive a quit.
//
// Step 8.4 replaced the fixed name with the one the title screen came back with. The default
// is still "default", and it is still what a build with the title screen compiled out plays,
// so nothing about a BS_TITLE=0 automation run changed: the same directory, the same save.
//
// A name, not a path. scene/worldlist.h owns what a legal name is (worldlistNameValid) and
// the title screen refuses to hand back anything that fails it, so this is joined onto
// REGION_ROOT without re-validating — one validator, in the file that also creates the
// directories, rather than a second opinion here that could disagree with it.
//
// Lives above the BS_WORLD_GEN guard on purpose: it is called both from genStart() below
// (generated-world path) and, unconditionally, from the shared inventory-load code further
// down main() — the hand-built world (BS_WORLD_GEN=0) needs a save directory exactly as much
// as the generated one does. It used to sit inside the #if BS_WORLD_GEN block below, which
// compiled fine for every ordinary build (BS_WORLD_GEN defaults to 1) but broke the
// documented remesh-stress recipe below, `-DBS_REMESH_STRESS=1000`, which forces
// BS_WORLD_GEN=0 and left the later unconditional call an implicit declaration.
#define SAVE_WORLD_NAME "default"

static char s_world_name[WORLDLIST_NAME_MAX] = SAVE_WORLD_NAME;

// True when the title screen handed back TITLE_START_SERVER — the player joined a server, so
// the world being played belongs to it and not to this console. It gates every call to
// saveWorldDir() below, not just the ones that obviously write: that function mkdirs three
// levels on its way to the answer (see its body), so even the read-only worldHasBeenPlayed()
// probe would leave an empty world directory behind on a card that should have gained nothing
// from the session. The server is the save file; this console keeps no copy.
static bool s_server_session = false;

// True for exactly one trip back to the menu, set when a server session ends and cleared the
// moment the menu has been opened with it. Distinct from s_server_session, which is cleared at
// the top of every session and so cannot survive the lap that has to read it: this one answers
// "was the thing that just ended a server world?" rather than "is the thing about to start
// one?". Its only job is to decide which screen the menu opens on — see runTitleScreen.
static bool s_left_server = false;

static const char* saveWorldDir(void)
{
	static char dir[128];

	// Each level in turn: mkdir does not create parents, and the two above the world are
	// shared with the metrics CSV and the server address file, so either may already exist.
	// EEXIST is the expected answer and is not distinguished — the only thing that matters
	// is whether the final directory is usable, which the probe below actually tests rather
	// than infers from a return code.
	mkdir("sdmc:/blocksmith", 0777);
	mkdir(REGION_ROOT, 0777);
	snprintf(dir, sizeof(dir), "%s/%s", REGION_ROOT, s_world_name);
	mkdir(dir, 0777);

	// Written to and removed again, because mkdir returning -1/EEXIST proves nothing about
	// whether the card is writable — a read-only SD reports the directory as already there.
	// Finding out now costs one file; finding out at the first unload costs the player's
	// building with no warning.
	char probe[160];
	snprintf(probe, sizeof(probe), "%s/.wtest", dir);
	FILE* f = fopen(probe, "wb");
	if (!f) return NULL;
	const bool wrote = fwrite("bs", 1, 2, f) == 2;
	fclose(f);
	remove(probe);
	return wrote ? dir : NULL;
}

// Whether this world has ever been played, which is the only thing the loading screen's
// heading depends on: "CREATING WORLD" for one with nothing on the card yet, "LOADING WORLD"
// for one being returned to. Asked of the card rather than plumbed down from the title screen
// on purpose — worldlistCreate() deliberately treats an existing name as success (see
// scene/worldlist.h), so "the player pressed New World" and "this world is new" are not the
// same question, and the card is the one that knows.
//
// A region file is the whole test: region.c names them r.<rx>.<rz>.bsr, nothing else in a
// world directory has that extension, and a world only gets one once a column has been saved.
// A NULL dir (unwritable card) reads as new, which is what it will behave like.
static bool worldHasBeenPlayed(const char* dir)
{
	if (!dir) return false;

	DIR* d = opendir(dir);
	if (!d) return false;

	bool found = false;
	const struct dirent* e;
	while (!found && (e = readdir(d)) != NULL) {
		const char* dot = strrchr(e->d_name, '.');
		found = dot && strcmp(dot, ".bsr") == 0;
	}
	closedir(d);
	return found;
}

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
#define GEN_MESH_SPAN (2 * GEN_MESH_RADIUS_MAX + 1)
#define GEN_AREA_SPAN (2 * GEN_AREA_RADIUS_MAX + 1)
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
	return dx >= -s_area_radius && dx <= s_area_radius &&
	       dz >= -s_area_radius && dz <= s_area_radius;
}

static bool genInMesh(int32_t cx, int32_t cz)
{
	const int32_t dx = cx - s_center_cx, dz = cz - s_center_cz;
	return dx >= -s_mesh_radius && dx <= s_mesh_radius &&
	       dz >= -s_mesh_radius && dz <= s_mesh_radius;
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
	for (int32_t dz = -s_mesh_radius; dz <= s_mesh_radius; dz++) {
		for (int32_t dx = -s_mesh_radius; dx <= s_mesh_radius; dx++) {
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
	for (int32_t dz = -s_area_radius; dz <= s_area_radius; dz++) {
		for (int32_t dx = -s_area_radius; dx <= s_area_radius; dx++) {
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

	// Step 8.1. The last moment this column's blocks exist. Only columns the player edited
	// are written — everything else the seed reproduces exactly, and saving it would turn
	// walking in a straight line into a stream of pointless SD writes.
	//
	// Before worldColumnRemove and not after, for the obvious reason, and the encode happens
	// here on the main thread because this thread owns the World; only the bytes cross to
	// the worker. See app/worker.h.
	const Column* col = worldColumn(&s_world, cx, cz);
	if (col && col->dirty) workerSubmitSave(col);

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
	for (int32_t dz = -s_area_radius; dz <= s_area_radius; dz++)
		for (int32_t dx = -s_area_radius; dx <= s_area_radius; dx++) {
			const int32_t ox = old_cx + dx, oz = old_cz + dz;
			if (!genInArea(ox, oz)) genUnloadColumn(ox, oz);
		}

	// A column that left the *mesh* ring but is still inside the generated ring keeps its
	// blocks and loses its geometry: it is now one of the neighbours the mesher reads, not
	// something drawn. Without this its meshes would stay on screen past the render
	// distance and the pool would fill.
	for (int32_t dz = -s_mesh_radius; dz <= s_mesh_radius; dz++)
		for (int32_t dx = -s_mesh_radius; dx <= s_mesh_radius; dx++) {
			const int32_t ox = old_cx + dx, oz = old_cz + dz;
			if (genInMesh(ox, oz)) continue;
			chunkRenderReleaseColumn(ox, oz);
			genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, ox, oz);
		}

	genRequestArea();
	genQueueReadyColumns();
}

// Step 7.7. The distance the world boots at. Separate from genSetRadius below because at boot
// there is nothing to unload and no worker to ask for anything yet: this only has to be called
// before genStart, which then requests the right shape first time.
static void genInitRadius(int radius)
{
	if (radius < RENDER_DIST_MIN) radius = RENDER_DIST_MIN;
	if (radius > RENDER_DIST_MAX) radius = RENDER_DIST_MAX;
	s_mesh_radius = radius;
	s_area_radius = radius + 1;
	chunkRenderSetDistance(radius);
}

// Step 7.7. Changes the render distance, which is the same problem genRecenter solves — a ring
// whose shape changed under it — with the centre standing still instead of the radius.
//
// Deliberately the same three moves in the same order, and for the same reason: drop the
// columns that are now outside the generated ring, drop the *geometry* of columns that are
// still generated but no longer drawn, then ask for whatever the new shape is missing. Doing
// it this way means lowering the distance frees memory immediately rather than at the next
// column boundary, and raising it does not have to wait for the player to walk anywhere.
//
// Nothing is regenerated that already exists: raising the distance keeps every loaded column
// and only requests the new outer rings, so the visible cost of a change is the new columns'
// generation, not a rebuild of the world.
static void genSetRadius(int radius)
{
	if (radius < RENDER_DIST_MIN) radius = RENDER_DIST_MIN;
	if (radius > RENDER_DIST_MAX) radius = RENDER_DIST_MAX;
	if (radius == s_mesh_radius) return;

	const int old_area = s_area_radius, old_mesh = s_mesh_radius;
	s_mesh_radius = radius;
	s_area_radius = radius + 1;

	// Sweep the OLD rings, because those are the columns that exist. Sweeping the new ones
	// would miss everything that just fell outside, which is the whole point when shrinking.
	for (int32_t dz = -old_area; dz <= old_area; dz++)
		for (int32_t dx = -old_area; dx <= old_area; dx++) {
			const int32_t ox = s_center_cx + dx, oz = s_center_cz + dz;
			if (!genInArea(ox, oz)) genUnloadColumn(ox, oz);
		}

	for (int32_t dz = -old_mesh; dz <= old_mesh; dz++)
		for (int32_t dx = -old_mesh; dx <= old_mesh; dx++) {
			const int32_t ox = s_center_cx + dx, oz = s_center_cz + dz;
			if (genInMesh(ox, oz)) continue;
			chunkRenderReleaseColumn(ox, oz);
			genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, ox, oz);
		}

	// The fog and the near plane are functions of the distance and are changed here, together
	// with the ring, so there is never a frame drawn with one of the two out of step.
	chunkRenderSetDistance(radius);

	genRequestArea();
	genQueueReadyColumns();
}

// Starts the worker and queues the whole area. The spawn column goes first because the
// ring is FIFO, so it is the first one back and the player has ground under them before
// the loop starts.
// The seed genStart() actually generated from — BS_WORLD_SEED in single player, the server's
// own seed in a session. Kept because the gen overlay's readout is otherwise a lie the moment
// the two differ, and "which world am I standing in" is exactly what that line is for.
static uint32_t s_seed_used = BS_WORLD_SEED;

static bool genStart(int32_t cx, int32_t cz)
{
	// Which world this is, decided here and nowhere else. In a session the server owns the
	// seed and sends it as BS_APP_WORLD_INFO right after JOIN (net/networld.h): the terrain is
	// never transmitted, so generating from anything other than the server's seed would put
	// this player in their own private landscape and make every other player's block edit land
	// in the wrong hillside. Single player, or a server too old to send one, keeps the client's
	// own fixed seed — networldWorldSeed() leaves `seed` untouched when it has nothing to say.
	uint32_t seed = BS_WORLD_SEED;
	(void)networldWorldSeed(&seed);
	s_seed_used = seed;

	worldgenInit(&s_gen, seed);
	jobqInit(&s_meshq);
	memset(s_col_queued, 0, sizeof(s_col_queued));
	memset(s_col_in, 0, sizeof(s_col_in));
	memset(s_col_asked, 0, sizeof(s_col_asked));
	s_genr = (GenResult){0};
	s_genr.ran = true;
	s_center_cx = cx;
	s_center_cz = cz;
	s_col_unloaded = s_col_dropped = 0;

	// Before workerStart, which is where the worker copies it: the worker reads the card on
	// its very first job, so a directory set afterwards would miss the spawn column.
	//
	// NULL in a server session, which app/worker.h documents as "no save file at all: every
	// column is generated and none is ever written". That is exactly the contract a joined
	// world wants — terrain comes from the server's seed and edits come from its diff store,
	// so a local copy could only ever be a second, staler answer to a question the server has
	// already answered. saveWorldDir() is not called at all rather than called and ignored,
	// because calling it is what creates the directory.
	workerSetWorldDir(s_server_session ? NULL : saveWorldDir());

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

	// This column is now genuinely part of the live world, every chunk workerInstall() had
	// already copied in above — the moment net/blockdiff.h's pending diffs for it (a remote
	// edit that arrived while it was still streaming in) can land. Before genQueueReadyColumns
	// so anything a drained diff touches gets swept into the same meshing pass as the rest of
	// this column instead of waiting a frame.
	networldOnColumnLoad(&s_world, cx, cz);

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

// How many of the area ring's columns are in. Walked rather than counted incrementally
// because the ring moves: genRecenter drops and re-asks, and a counter maintained across that
// would drift the first time a column arrived for a position the ring had already left (which
// genInstallOne handles by dropping it — see s_col_dropped).
static int genColumnsInstalled(void)
{
	int n = 0;
	for (int32_t dz = -s_area_radius; dz <= s_area_radius; dz++)
		for (int32_t dx = -s_area_radius; dx <= s_area_radius; dx++)
			if (genColumnInstalled(s_center_cx + dx, s_center_cz + dz)) n++;
	return n;
}

// One frame's counters for the loading screen. Everything here is already tracked; this only
// gathers it, so the screen cannot disagree with the HUD about what the world is doing.
static LoadingSample genLoadingSample(void)
{
	const int span  = 2 * s_area_radius + 1;
	const int total = span * span;
	const int in    = genColumnsInstalled();

	LoadingSample s = {
		.columns_in     = in,
		.columns_total  = total,
		.meshes         = s_genr.meshed,
		.mesh_queued    = jobqCount(&s_meshq),
		.spawn_ready    = genColumnInstalled(s_center_cx, s_center_cz),
		.ring_ready     = in >= total,
		.worker_busy    = workerBusy(),
		.columns_failed = s_genr.columns_failed,
		.submit_failed  = s_genr.submit_failed,
	};
	return s;
}

// Twice the in-game budget — 8 ms and 6 chunks against the frame's 16.71 ms. Nothing else is
// running while this screen is up: no player, no interaction, no world HUD, and the top screen
// is a flat clear. Spending that headroom roughly halves the number of frames the player
// spends watching a bar. Still bounded rather than "drain the queue": one frame that built all
// forty chunks would freeze the bar for a quarter of a second, which is the exact impression
// this screen exists to remove.
#define LOADING_DRAIN_BUDGET_MS  (2.0f * DRAIN_BUDGET_MS)
#define LOADING_DRAIN_MAX_CHUNKS (2 * DRAIN_MAX_CHUNKS)

// Waits for the world with the console still alive, and shows the player what it is waiting
// for. Returns true to go and play, false if the player asked to quit or the system closed us.
//
// This replaces a plain `while (!installed) { ...; svcSleepThread(1 ms); }`, and the reason it
// replaces it is a report from real hardware on 2026-08-19: "it's frozen my three d s. It is a
// critical bug ... it freezes the entire console." That loop never called aptMainLoop(), and a
// 3DS app that stops servicing APT stops responding to the HOME button — so any hitch in
// generation, on a card or a console nobody here has, was not a slow load but a console the
// player had to hold the power button to escape. Everything that waits in here is inside
// aptMainLoop() now, and scene/loading.c guarantees the wait itself ends: LOADING_STALL_FRAMES
// of no progress turns into an on-screen reason and a way out, never another frame of waiting.
//
// `draw` is false when spriteInit/fontInit failed or there is no bottom target. The loop still
// runs — the APT pumping is the part that matters, not the pixels — it just has nothing to
// paint, exactly as the title screen degrades to "no menu, play the default world".
static bool runLoadingScreen(bool draw, const char* heading, const char* world_name)
{
	C3D_RenderTarget* const bottom = draw ? screenBottom() : NULL;
	C3D_RenderTarget* const top    = screenTop();

	LoadingState st;
	loadingInit(&st);
	bool touch_prev = false;

	while (aptMainLoop()) {
		watchdogPhase(WD_PHASE_INPUT);
		hidScanInput();
		const u32 down = hidKeysDown();

		// Same two-signal touch read the title screen and the HUD use, for the same reason —
		// hid.h flags KEY_TOUCH as "Not actually provided by HID".
		touchPosition tp = {0};
		hidTouchRead(&tp);
		const bool touch_down = (hidKeysHeld() & KEY_TOUCH) != 0 || tp.px != 0 || tp.py != 0;
		const bool tap = touch_down && !touch_prev;
		touch_prev = touch_down;

		// Feed the world. The worker stages one column at a time and waits for it to be taken,
		// so in practice this installs one per frame; the bound is for the case genInstallOne
		// returns true without consuming a staged column (a column that arrived for a ring
		// position already left behind), which must not become an unbounded loop in here of all
		// places.
		watchdogPhase(WD_PHASE_LOAD_GEN);
		for (int i = 0; i < 8 && genInstallOne(); i++) { }
		watchdogPhase(WD_PHASE_LOAD_MESH);
		genDrainMesh(LOADING_DRAIN_BUDGET_MS, LOADING_DRAIN_MAX_CHUNKS);

		const LoadingSample sample = genLoadingSample();
		const LoadingPhase phase = loadingStep(&st, &sample);

		LoadingAction act = LOADING_ACTION_NONE;
		if (phase == LOADING_STALLED) {
			if (down & KEY_A)          act = LOADING_ACTION_PLAY;
			else if (down & KEY_START) act = LOADING_ACTION_QUIT;
			else if (tap)              act = loadingHitAction(&st, tp.px, tp.py);
		}

		watchdogPhase(WD_PHASE_LOAD_DRAW);
		if (bottom) {
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			spriteFrameBegin();

			// The top screen is cleared and bound every frame for the same reason the title
			// screen does it: C3D_FrameEnd only presents a screen with a target bound this
			// frame, so skipping it would leave whatever was in the framebuffer on show.
			C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(top);

			C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0x1A1424FF, 0);
			C3D_FrameDrawOn(bottom);
			loadingDraw(&st, heading, world_name);

			C3D_FrameEnd(0);
		} else {
			gspWaitForVBlank();
		}

		// This loop beats the watchdog for the same reason the game loop does, and it is not
		// optional here: a world that legitimately takes longer than WD_TIMEOUT_MS to build
		// would otherwise be reported as a hang by a watchdog that had never seen a single
		// completed frame. The counters go with it so a report raised *during* a load says how
		// far the load had got.
		watchdogCounters(sample.columns_in, sample.meshes, sample.mesh_queued,
		                 sample.worker_busy);
		watchdogBeat();
		watchdogPhase(WD_PHASE_APT);

		// Tested after the draw, so the frame that says 100% is actually presented rather than
		// being the one frame that gets skipped on the way into the game.
		if (phase == LOADING_READY || act == LOADING_ACTION_PLAY) return true;
		if (act == LOADING_ACTION_QUIT) return false;
	}

	// aptMainLoop() went false — the HOME menu closed us, which is the outcome the old loop
	// could not even represent.
	return false;
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

// ── Step 8.1's verification probe ─────────────────────────────────────────────────────
//
// The plan's criterion for the save format is a behaviour, not a format: "break a block,
// walk away until the column unloads, walk back — the block is still broken; quit, relaunch
// — it is still broken". Both halves need the real card, the real region file, the real
// streaming ring and the real worker, so neither can be checked in the host suite, which is
// why this lives here next to digOutCheck rather than in world_test.c.
//
// It runs in two parts across two launches, and the second part is the one that matters:
//
//   RUN 1 — edit a block, force the column out of the ring (which saves it), force it back
//           in (which loads it), and check the edit survived. Then write down where the
//           edit was, so run 2 knows what to look for.
//   RUN 2 — read that note, and check the block is *still* air in a world that has just
//           been generated from cold. Nothing in memory carries over between launches, so
//           the only way the block can be air is if it came off the card.
//
// Off by default:  make EXTRA_CFLAGS="-DBS_SAVE_CHECK=1"
#ifndef BS_SAVE_CHECK
#define BS_SAVE_CHECK 0
#endif

// A deliberate hang, for proving app/watchdog.c can actually go red. Set it to a frame number
// and the main thread stops dead on that frame, in the world, with the last frame still on both
// screens — the failure the player reported, reproduced on demand. The watchdog should notice
// ten seconds later and leave sdmc:/blocksmith/hang.txt naming phase SIM.
//
// It sleeps rather than spins because that is the realistic shape: every unbounded wait in this
// codebase blocks in a syscall (worker.c's save-slot loop, a LightLock, C3D_FrameBegin waiting
// on the GPU), and a blocked thread yields its core while a spinning one does not — so this is
// also the harder case for a watchdog pinned to the same core to survive.
//
// Off by default:  make EXTRA_CFLAGS="-DBS_HANG_TEST=300"
#ifndef BS_HANG_TEST
#define BS_HANG_TEST 0
#endif

// The same instrument aimed at the other window — the handoff between the loading screen's last
// frame and the game loop's first one, where the second hardware freeze lives ("a hundred
// percent, it says ready, but it just sort of freezes"). Set it to 1 and the main thread stops
// dead inside the handoff, with the loading panel at 100% still on the bottom screen, which is
// what the player described. The watchdog should leave hang.txt naming phase HANDOFF_GPU.
//
// This knob is the only way to prove the three WD_PHASE_HANDOFF_* markers work, and they are
// worth proving: the whole window reported as nothing at all until they were added, because
// runLoadingScreen exits with the phase left at WD_PHASE_APT and an APT stall is ignored.
//
// Off by default:  make EXTRA_CFLAGS="-DBS_HANG_HANDOFF=1"
#ifndef BS_HANG_HANDOFF
#define BS_HANG_HANDOFF 0
#endif

// Where the time between "the player confirmed the world name" and "the loading screen appears"
// actually goes. Measured rather than guessed at: the emulator puts that gap at 23.95 s of a
// completely unrepainted screen (38 byte-identical captures) with nothing in Azahar's own log to
// explain it, and every candidate on that path — the world self-test, the report grid, the
// worker start — is plausible enough that picking one by eye would be a guess.
//
// Writes sdmc:/blocksmith/boot_timing.txt just before the loading screen opens. Plain stdio is
// fine here, unlike in app/watchdog.c: this runs on a healthy main thread that is about to do
// file I/O anyway.
//
// Off by default:  make EXTRA_CFLAGS="-DBS_BOOT_TIMING=1"
#ifndef BS_BOOT_TIMING
#define BS_BOOT_TIMING 0
#endif

// Whether to run world/world_test.c's 818 assertions on the console at boot. Off by default
// because it was measured at 23855.6 ms — see the call site for the full reasoning and for why
// nothing is lost by leaving it off.
#ifndef BS_SELFTEST
#define BS_SELFTEST 0
#endif

#if BS_BOOT_TIMING
static char   s_boot_log[768];
static size_t s_boot_len;
static u64    s_boot_mark;

static void bootBegin(void) { s_boot_mark = svcGetSystemTick(); }

static void bootEnd(const char* what)
{
	const double ms = (double)(svcGetSystemTick() - s_boot_mark) / CPU_TICKS_PER_MSEC;
	if (s_boot_len + 1 >= sizeof(s_boot_log)) return;
	const int n = snprintf(s_boot_log + s_boot_len, sizeof(s_boot_log) - s_boot_len,
	                       "%-22s %9.1f ms\n", what, ms);
	if (n > 0) s_boot_len += (size_t)n;
}

static void bootTimingWrite(void)
{
	FILE* f = fopen("sdmc:/blocksmith/boot_timing.txt", "wb");
	if (!f) return;
	fwrite(s_boot_log, 1, s_boot_len, f);
	fclose(f);
}

#define BOOT_BEGIN()   bootBegin()
#define BOOT_END(what) bootEnd(what)
#define BOOT_WRITE()   bootTimingWrite()
#else
#define BOOT_BEGIN()   ((void)0)
#define BOOT_END(what) ((void)0)
#define BOOT_WRITE()   ((void)0)
#endif

// ── The hardware draw probe ───────────────────────────────────────────────────────────
//
// For the freeze reported from a real Old 3DS on 2026-08-20, which the watchdog caught with
// phase DRAW, worker IDLE and the mesh queue empty — i.e. on the first frame after the world
// finished filling, which is also the first frame that submits the whole world. Azahar does
// not reproduce it in either console mode (measured: 120 s in-world at the same cols 322 the
// hang report names), so the guilty draw cannot be found here; it has to be found there.
//
// A GPU that stops signalling does not say which draw stopped it. Every draw call in a frame
// only appends to a command list and returns, so the phase marker can never be finer than
// "somewhere in this frame" — the block happens at the NEXT C3D_FrameBegin(C3D_FRAME_SYNCDRAW),
// long after the guilty call returned. The only thing that separates the candidates is
// removing them one at a time, so this removes them one at a time.
//
// It self-advances so the console does the bisect without a rebuild between arms: each boot
// reads sdmc:/blocksmith/drawprobe.txt, takes the arm after the last one started, and appends
// "START". A frame count later it appends "SURVIVED". An arm that hangs never writes its
// SURVIVED line, so the next boot sees the gap and records HUNG for it. Six boots, one file.
//
// Arm 0 and arm 5 are the controls, and the probe is worthless without both: 0 draws the whole
// frame and MUST hang (if it does not, the arms that follow prove nothing, because a green
// result would just mean the freeze did not happen this run), and 5 draws nothing and MUST
// survive (if it hangs too, the freeze is not in the drawing at all and the whole bisect is
// aimed at the wrong place — which is itself the answer).
//
// Off by default:  make EXTRA_CFLAGS="-DBS_DRAW_PROBE=1"
#ifndef BS_DRAW_PROBE
#define BS_DRAW_PROBE 0
#endif

// Throwaway: how full citro3d's command buffer gets. See the sample site next to
// C3D_FrameEnd for what it is testing and why it can be answered in an emulator.
//
// Off by default:  make EXTRA_CFLAGS="-DBS_CMDBUF_PROBE=1"
#ifndef BS_CMDBUF_PROBE
#define BS_CMDBUF_PROBE 0
#endif

#if BS_CMDBUF_PROBE
static float s_cmdbuf_last, s_cmdbuf_max;
#endif

// Whether the self-advancing arm bisect runs at all. OFF by default, and that default is a
// bug fix rather than a preference.
//
// v1.1.2 shipped with it on. steve's SD card still held v1.1.1's drawprobe.txt, whose last
// line was "arm 5 START", so the new build read someone else's log, concluded every arm had
// had its turn, and parked on the arm that draws nothing. He installed it, launched a world
// and reported "I can just see the outline of the blocks, I can't actually see the world" —
// which was the probe doing exactly what it was told, on instructions left behind by the
// previous version. A diagnostic that silently inherits state from a build that is no longer
// installed is a diagnostic that lies.
//
// It is off rather than version-stamped because the arms are no longer the instrument. The
// draw-stage breadcrumb in scene/chunk_render.c answers the same question on the first boot
// that freezes, without removing anything from the frame — so the shipped diagnostic now
// draws the whole world, looks like the game, and still names where it died.
#ifndef BS_DRAW_BISECT
#define BS_DRAW_BISECT 0
#endif

#if BS_DRAW_PROBE
#define PROBE_FILE   "sdmc:/blocksmith/drawprobe.txt"
#define PROBE_ARMS   6
// Long enough that a frame merely being slow on hardware cannot be mistaken for surviving —
// the watchdog gives up at 10 s, so 600 frames is ten seconds of healthy 60 Hz frames and
// several times the watchdog's window at any frame rate that still counts as running.
#define PROBE_FRAMES 600

static int  s_probe_arm;
static bool s_probe_written;

// Round 2's arms. Round 1 removed whole draws from the frame and came back off real hardware
// with exactly one survivor — arm 1, "no world" — so chunkRenderDraw() is the call that hangs
// the console and nothing else in the frame does. These cut inside it, and the cuts themselves
// live in scene/chunk_render.c (see app/drawprobe.h).
//
// Arm 2 is the one the whole round turns on. chunkRenderDraw does CPU work (the cull, the sort,
// the sight walk, the horizon test) *and* emits GPU commands, and from outside the two failures
// are identical: the main thread stops beating either way. Arm 2 runs every bit of the CPU work
// and emits not one GPU command. If it hangs, the fault is a loop that never ends. If it
// survives, the fault is something handed to the GPU.
static const char* probeArmName(int arm)
{
	switch (arm) {
	case 0:  return "everything (control: MUST hang)";
	case 1:  return "world draw skipped entirely (control: MUST survive)";
	case 2:  return "cull only - all the CPU work, not one GPU command";
	case 3:  return "opaque pass only - no transparent pass";
	case 4:  return "transparent pass only - no opaque pass";
	default: return "GPU state set up, but no draw calls at all";
	}
}

int bsProbeArm(void) { return s_probe_arm; }

// Works out which arm ran last and whether it came back, by reading the file it wrote. Reading
// it back rather than keeping a counter is what makes a hang self-recording: the boot that
// hangs never gets to write anything, so its silence is what the next boot has to notice.
//
// It tracks the LAST arm to start and whether a survival line followed it, rather than counting
// STARTs against SURVIVEDs. The counting version shipped in the first probe build and was
// wrong: one genuine hang left the two totals permanently one apart, so every boot after it
// wrote a HUNG line for an arm that had plainly survived. steve's round-1 log carries three
// such false lines. The only trustworthy question is per-arm — did THIS arm report back — so
// that is the question now asked.
static void probeBegin(void)
{
	int  last_start = -1;
	bool came_back  = true;
	char line[128];

	FILE* f = fopen(PROBE_FILE, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			int a = -1;
			if (sscanf(line, "arm %d", &a) != 1 || a < 0) continue;

			if (strstr(line, "START")) {
				last_start = a;
				came_back  = false;      // until its own SURVIVED line says otherwise
			} else if (strstr(line, "SURVIVED") && a == last_start) {
				came_back = true;
			}
		}
		fclose(f);
	}

	f = fopen(PROBE_FILE, "a");
	if (!f) return;

	if (last_start >= 0 && !came_back)
		fprintf(f, "arm %d HUNG - no survival line, the console froze on this arm\n",
		        last_start);

#if !BS_DRAW_BISECT
	// One arm, every boot: everything drawn. Nothing is removed from the frame, so the world
	// looks like the game and the breadcrumb is what does the work. The HUNG line above still
	// gets written, because "the last boot froze" is exactly what a build that is meant to
	// freeze needs to record.
	s_probe_arm = 0;
	fprintf(f, "arm 0 START  %s\n", probeArmName(0));
#else
	s_probe_arm = last_start + 1;
	if (s_probe_arm >= PROBE_ARMS) {
		// Every arm has had its turn. Park on the one that draws nothing so the console is
		// still usable rather than freezing again on a run that can no longer learn anything.
		s_probe_arm = PROBE_ARMS - 1;
		fprintf(f, "-- all arms done; parked on arm %d --\n", s_probe_arm);
		s_probe_written = true;   // nothing more to record
	} else {
		fprintf(f, "arm %d START  %s\n", s_probe_arm, probeArmName(s_probe_arm));
	}
#endif
	fclose(f);

	// Once here as well as once per frame: an arm that freezes on its very first frame never
	// reaches probeFrame, and a hang report saying "arm -1, 0 bytes free" would be worse than
	// no report at all.
	watchdogProbeState(s_probe_arm, (u32)linearSpaceFree(), (u32)vramSpaceFree());
}

// Called once per completed game frame. Appends the survival line exactly once, then stops
// touching the card — an arm that gets this far has answered its question and the rest of the
// run is just the player looking at it.
// Hands the watchdog the arm number and the two heap figures, so a hang report names which arm
// was running and how much room was left. Sampled here, on the main thread, because the monitor
// thread may not take libctru's heap lock — see watchdog.c.
static void probeSample(void)
{
	watchdogProbeState(s_probe_arm, (u32)linearSpaceFree(), (u32)vramSpaceFree());
}

static void probeFrame(void)
{
	static int frames;
	probeSample();
	if (s_probe_written) return;
	if (++frames < PROBE_FRAMES) return;

	FILE* f = fopen(PROBE_FILE, "a");
	if (f) {
		fprintf(f, "arm %d SURVIVED %d frames\n", s_probe_arm, PROBE_FRAMES);
		fclose(f);
	}
	s_probe_written = true;
}
#else
#define probeBegin()      ((void)0)
#define probeFrame()      ((void)0)
#endif

#define SAVE_CHECK_NOTE "sdmc:/blocksmith/savecheck.txt"

typedef struct {
	bool ran;
	int  run;             // 1 = wrote the note this launch, 2 = read one from a previous one
	int  x, y, z;         // the block that was broken
	int  before;          // what the generator had put there
	int  after_reload;    // what it read back after the unload/load round trip (run 1)
	int  at_boot;         // what it was at boot, before any edit (run 2)
	bool pass;
} SaveCheckResult;

// Highest solid block in the column, or -1 if the whole stack is air. Scanned from the top
// so the answer is the surface rather than the first thing above the floor.
static int saveCheckSurfaceY(int x, int z)
{
	for (int y = WORLD_HEIGHT - 1; y >= 0; y--)
		if (worldGet(&s_world, x, y, z) != BLOCK_AIR) return y;
	return -1;
}

static SaveCheckResult saveCheck(bool run)
{
	SaveCheckResult r = {0};
	if (!run) return r;
	r.ran = true;

	// The spawn column, which genWaitSpawnColumn has already guaranteed is installed. Any
	// other column would be a race against the ring still filling.
	const int x = 5, z = 5;

	// ── Run 2 first: is there a note from a previous launch? ─────────────────────────
	FILE* f = fopen(SAVE_CHECK_NOTE, "rb");
	if (f) {
		int nx = 0, ny = 0, nz = 0, nbefore = 0;
		const int got = fscanf(f, "%d %d %d %d", &nx, &ny, &nz, &nbefore);
		fclose(f);

		if (got == 4) {
			r.run    = 2;
			r.x      = nx;
			r.y      = ny;
			r.z      = nz;
			r.before = nbefore;
			r.at_boot = worldGet(&s_world, nx, ny, nz);

			// The block must be air now, and the note must say it was NOT air before —
			// otherwise "it is air" proves nothing, because it always was. That second
			// clause is what stops this passing on a world where the surface moved.
			r.pass = (r.at_boot == BLOCK_AIR) && (r.before != BLOCK_AIR);

			// Removed either way, so the next launch starts the cycle again rather than
			// re-reading a note about a block that has since been overwritten.
			remove(SAVE_CHECK_NOTE);
			return r;
		}
		remove(SAVE_CHECK_NOTE);
	}

	// ── Run 1: edit, unload, reload, and check in-session ────────────────────────────
	r.run = 1;
	r.y   = saveCheckSurfaceY(x, z);
	r.x   = x;
	r.z   = z;
	if (r.y < 0) return r;                       // no ground here: nothing to test with

	r.before = worldGet(&s_world, x, r.y, z);
	worldSet(&s_world, x, r.y, z, BLOCK_AIR);
	worldMarkDirty(&s_world, x, z);

	// Straight through the real unload path, so the save goes out the way it would if the
	// player had walked away — including the dirty test, which is the part most likely to be
	// wrong. Then wait for the card, because the reload below must not race the write.
	genUnloadColumn(x >> 4, z >> 4);
	workerFlushSaves();

	// And back in through the real request path, which is where load-before-generate lives.
	if (workerSubmitColumn(x >> 4, z >> 4)) {
		genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, x >> 4, z >> 4);
		while (!genColumnInstalled(x >> 4, z >> 4)) {
			if (genInstallOne()) continue;
			if (!workerBusy()) break;
			svcSleepThread(1000000);
		}
	}

	r.after_reload = worldGet(&s_world, x, r.y, z);
	r.pass = (r.after_reload == BLOCK_AIR) && (r.before != BLOCK_AIR);

	// The note for run 2. Written last, so a run that failed its own in-session check still
	// leaves the file — the relaunch half is worth running even when this half is red, and
	// the two results together say which side of the write/read pair is broken.
	f = fopen(SAVE_CHECK_NOTE, "wb");
	if (f) {
		fprintf(f, "%d %d %d %d\n", r.x, r.y, r.z, r.before);
		fclose(f);
	}
	return r;
}

// The probe's result, onto the card as well as onto the screen. A file rather than only a
// screenshot because the numbers are what decide pass or fail, and reading them off a 240p
// top screen is a transcription step this does not need — and because the two launches have
// to be compared, which a file makes trivial and a pair of screenshots does not.
//
// Appended, so run 1 and run 2 end up in the same file in order.
static void saveCheckReport(const SaveCheckResult* r)
{
	if (!r->ran) return;

	char line[192];
	snprintf(line, sizeof(line),
	         "run%d %s at (%d,%d,%d) before=%d reload=%d boot=%d loaded=%d saved=%d fail=%d\n",
	         r->run, r->pass ? "PASS" : "FAIL", r->x, r->y, r->z,
	         r->before, r->after_reload, r->at_boot,
	         workerLoaded(), workerSaved(), workerSaveFailed());

	printf("savecheck: %s", line);

	FILE* f = fopen("sdmc:/blocksmith/savecheck_result.txt", "ab");
	if (f) { fputs(line, f); fclose(f); }
}

#endif   // BS_WORLD_GEN

// Step 7.6. Stereoscopic 3D, off until the player asks for it with SELECT.
//
// Off is the default and it is not a placeholder: the plan's constraint is that no feature
// may depend on the 3D budget, so the game has to be judged in 2D and 3D has to be something
// it can afford to lose. Nothing else in the program reads this — the only difference a
// second eye makes is that the frame below runs its draw twice with a different projection.
static bool s_stereo = false;

// One eye's worth of frame: point the world's projection at it, clear, and draw. In 2D this
// is called once with iod 0 and the sequence is identical to what it was before step 7.6.
//
// Each eye clears its own target. The right eye is a separate render target with its own
// colour and depth buffer, so drawing into it without clearing would composite this frame's
// right eye on top of the last one's.
// The frame's half of the breadcrumb app/watchdog.h describes. chunkRenderDraw marks its own
// insides; everything after it — the highlight, the two player passes, the bottom screen, the
// submit, and the wait — happens in this file and used to be one unmarked stretch that the
// report could only call "back in main.c".
//
// k carries the eye (0 left, 1 right) for the stages inside drawEye and -1 for the rest. The
// mesh-slot field is always -1 here: none of these stages is inside a chunk loop.
#if BS_DRAW_PROBE
// Set to a WdDrawStage value and the main thread parks forever the instant that marker is set,
// which is exactly the shape of the hardware freeze. This is how the markers are proven able to
// go red: an arm built with, say, -DBS_FRAME_HANG_TEST=WD_DRAW_BOTTOM must produce a hang.txt
// naming drawBottomUi and nothing else. Never in a release build.
//
// #ifdef and not #if, and left undefined by default on purpose: the value is a stage *name*,
// and `#if WD_DRAW_BOTTOM` would quietly evaluate an enum constant the preprocessor has never
// heard of as 0 and compile the whole check away — an armed build that cannot fire.
static void drawStage(int stage, int k)
{
	watchdogDrawStage(stage, k, -1);
#ifdef BS_FRAME_HANG_TEST
	// BS_FRAME_HANG_AFTER lets an arm run normally for that many frames before parking, instead
	// of parking on the very first one. It defaults to 0, so every arm built before this existed
	// behaves exactly as it did. It is needed because some of what the report prints is gathered
	// by the main thread during normal play — watchdogGxSelfTest and watchdogCmdCapture both run
	// after C3D_FrameEnd — and an arm that parks on frame 0 never reaches them, so it reports
	// "NOT CAPTURED" and proves nothing about them either way.
#ifndef BS_FRAME_HANG_AFTER
#define BS_FRAME_HANG_AFTER 0
#endif
	if (stage == (BS_FRAME_HANG_TEST)) {
		static int seen;
		if (seen++ >= (BS_FRAME_HANG_AFTER)) for (;;) svcSleepThread(1000000000ULL);
	}
#endif
}
#else
#define drawStage(stage, k) ((void)0)
#endif

static void drawEye(C3D_RenderTarget* target, const C3D_Mtx* view, const RayHit* hit,
                    float iod, int eye)
{
	drawStage(WD_DRAW_EYE_SETUP, eye);
	(void)eye;   // the markers are the only reader; a non-probe build has none
	chunkRenderSetEye(iod);
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
	C3D_FrameDrawOn(target);
	// Round 1's cuts were here. They are gone because round 1 is answered: on real hardware
	// every arm froze except the one that skipped this call, so the bisect has moved inside
	// chunkRenderDraw and this file draws the whole frame again in every arm.
	chunkRenderDraw(view);
	for (int i = 0; i < BS_GPU_STRESS; i++) chunkRenderDraw(view);

	// After the world, because it re-binds the GPU for its own vertex format and does not
	// put it back — chunkRenderDraw re-establishes everything it needs at the top of its
	// next call instead. See pipelineBind in chunk_render.c. highlightDraw itself checks
	// hit->hit (see highlight.h), so there is no guard here to keep in sync with it.
	// It reads chunkRenderProjection(), so it gets this eye's matrix for free.
	drawStage(WD_DRAW_HIGHLIGHT, eye);
	highlightDraw(view, hit);

	// The other people in the session, from the pose table net/networld.c fills. Both calls
	// are unconditional: each returns immediately when nobody else has sent a pose, which
	// is every frame of a single-player game, so there is no "are we connected" test to
	// keep in sync here.
	//
	// The tag pass has to come second and has to be last in this function: it opens a sprite
	// batch, which turns depth testing off and blending on and puts neither back, so anything
	// 3D drawn after it would be drawn without a depth test.
	//
	// 400x240 is the top screen, which is the only target drawEye is ever given. Passing the
	// wrong size here would not fail — it would scale every tag's position, which is the kind
	// of wrong that reads as a projection bug.
	drawStage(WD_DRAW_PLAYERS, eye);
	playerModelDraw(view);
	drawStage(WD_DRAW_TAGS, eye);
	playerModelDrawTags(view, 400.0f, 240.0f);
}

// The player's items. Outside the BS_BOTTOM_UI guard on purpose: a build with no bottom
// screen still has an inventory, because Interact.holding is fed from it and a break still
// has to put the block somewhere. Such a build simply has no way to *see* or rearrange it.
// A file static rather than a local of main() for the same reason s_world is one — the
// bottom-screen draw below is a file-scope function and needs it.
static Inventory s_inv;

#if BS_BOTTOM_UI

static UiState s_ui;

// Step 8.2. The bottom screen's whole content: hotbar, inventory grid, crafting panel, and
// the engine numbers step 8.3's screen carried.
//
// This replaced step 8.3's font demo outright rather than gaining a toggle beside it. That
// screen existed to settle one visual claim — "the font renders" — by putting every glyph,
// three tints and three scales where they could be looked at, and that claim was settled at
// step 8.3. Keeping it would mean the bottom screen of a shipped build showing an ASCII
// table, and scene/ui.c draws with exactly the same spriteBegin/fontDraw batch, so the font
// is still on screen every frame and would still visibly break if it broke.
//
// The layout, the hit testing and the pick-up/drop gesture all live in scene/ui.c; nothing
// about them is decided here. This function is the frame plumbing only — bind the target,
// hand over the numbers main.c already computed for the console overlay, and pass on the
// touch point.
static void drawBottomUi(bool ok, const char* status, const char* netline, const UiInput* in)
{
	C3D_RenderTarget* target = screenBottom();
	if (!target) return;

	// The clear colour is the panel colour, so the "background" is free: one clear rather
	// than a full-screen quad that would blend over a surface nothing ever looks at.
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x1A1424FF, 0);
	C3D_FrameDrawOn(target);

	if (!ok) return;   // nothing to draw with; the screen stays the clear colour

	const UiStats stats = {
		.columns    = s_world.columns,
		.chunks     = s_world.chunks,
		.meshes     = chunkRenderMeshes(),
		.culled     = chunkRenderCulled(),
		.tris       = chunkRenderTris(),
		.bytes      = (uint32_t)worldBytes(&s_world),
		.bytes_peak = (uint32_t)budgetPeak(),
		.status     = status,
		.net        = netline,
	};

	// atlasTexture() rather than atlasBind(): gfx/sprite.c tracks the bound texture itself so
	// it can flush exactly once per switch, and binding behind its back would leave it certain
	// it had not switched. See gfx/atlas.h.
	uiUpdateDraw(&s_ui, &s_inv, atlasTexture(), &stats, in);
}

#endif   // BS_BOTTOM_UI

// `built` and `mesh_refused` are startup facts, so they belong in the report drawn once
// rather than on the per-frame status line: the console is 32 columns and the status line
// needs all of them for the aim, edit and queue counters that change every frame.
static void worldReportDraw(int refused, bool built,
                            const char* selftest, const StressResult* st,
                            const DigOutResult* dig, const GenResult* gen,
                            const Camera* cam)
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
	printf("meshes %4d  tris %6lu cull %2d\n", chunkRenderMeshes(),
	       (unsigned long)chunkRenderTris(), chunkRenderCulled());
	printf("sort moved %2d  near-to-far %s \n", chunkRenderSortMoved(),
	       chunkRenderSortOk() ? "OK " : "BAD");

	// Step 7.3. `cave` counts chunks the sight walk rejected that the frustum had kept, so it
	// and `cull` above are disjoint. `walk` says whether the walk ran at all — a zero `cave`
	// with `walk off` means the loaded area did not fit VisWalk's box and nothing was cave
	// culled, which is a completely different fact from "there was nothing to cull".
	//
	// `cam` is the camera position chunk_render.c recovered by inverting the view matrix,
	// against the one main.c is holding. They have to agree: the walk starts at whichever
	// chunk that position lands in, and starting a chunk out would cull convincingly and
	// wrongly. Kept in the report rather than deleted after one check, because the day
	// cameraView stops being a rigid transform this line is what will say so.
	float wx = 0.0f, wy = 0.0f, wz = 0.0f;
	chunkRenderCamera(&wx, &wy, &wz);
	printf("cave %2d walk %s  fill %4.0f us  \n", chunkRenderCaveCulled(),
	       chunkRenderCaveRan() ? "on " : "off", chunkRenderVisUs());
	printf("cam %6.2f %6.2f %6.2f %s\n", wx, wy, wz,
	       (fabsf(wx - cam->x) < 0.02f && fabsf(wy - cam->y) < 0.02f &&
	        fabsf(wz - cam->z) < 0.02f) ? "MATCH" : "WRONG");
	// Step 9.1e. Disjoint from `cull` and `cave` above — see chunk_render.h.
	printf("hzn %2d                       \n", chunkRenderHorizonCulled());
	// Step 7.5. What the alpha-tested second pass actually submitted. Both zero looks
	// identical on screen to "there is no canopy in view", and only one of those is a bug —
	// the split is inside the mesher, so a wrong boundary would draw every leaf face in the
	// opaque pass and the world would still look almost right.
	printf("alpha %2d draws %5lu tris        \n", chunkRenderAlphaDraws(),
	       (unsigned long)chunkRenderAlphaTris());

	// Phase 9's evidence, and none of it is visible on screen — every one of these changes is
	// meant to leave the picture exactly as it was.
	//
	// `cull/f` is culls per frame: at most 1.00 is the claim, in 2D and — the point of step
	// 9.3 — in 3D as well, where the frame draws twice and used to cull twice. It goes well
	// below 1 with the camera held still, because the cache is keyed on the view matrix and a
	// stationary camera has nothing to re-cull. `walk/f` is sight walks per frame, which sits
	// lower again: the walk only changes when the camera crosses a chunk boundary.
	const uint32_t frames = metricsFrames();
	const float    denom  = frames ? (float)frames : 1.0f;
	printf("cull/f %4.2f walk/f %4.2f          \n",
	       (float)chunkRenderCullRuns() / denom,
	       (float)chunkRenderWalkRuns() / denom);
	// Step 7.7. The setting, and the two numbers derived from it that decide whether it works:
	// how far the fade lets you see, and whether it finishes before the load boundary. `hides`
	// reading NO is the one failure mode that looks like scenery — chunks appearing out of
	// clear air at a fixed distance — so it is stated rather than left to be noticed.
	const RenderDist* rd = chunkRenderDistance();
	printf("dist %d  see %4.1f  edge %4.1f %s\n", rd->radius, rd->half_vis, rd->boundary,
	       rd->fog_hides ? "hid" : "NO!");
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
	// actually needed, which is the number that decides how far s_mesh_radius can grow
	// before Phase 6's eviction exists.
	//
	// The second line is step 5.5's whole claim in two numbers. `wk` is wall time the
	// worker spent inside the generator and `in` the time the main thread spent copying
	// finished columns across: the first is the cost that used to sit in front of the
	// first frame, the second is all that is left of it on the frame path. Read them
	// against `worst` and `drop` on the overlay — a large `wk` with `drop 0` is the
	// measurement, and a `q` that never reaches 0 would mean the worker is not keeping up.
	if (gen->ran) {
		printf("gen s%-5lu in %2d fail %d mesh %3d\n", (unsigned long)s_seed_used,
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
		printf("fill %4d frames  core %d      \n", gen->fill_frames, workerCore());
#endif
	}

	if (dig->ran)
		printf("digout q%d>%d t%lu>%lu %s\n", dig->queued_before, dig->queued_after,
		       (unsigned long)dig->tris_empty, (unsigned long)dig->tris_back,
		       dig->pass ? "PASS" : "FAIL");
	printf("selftest %-22s\n", selftest);
}

// Step 8.1. Hands every dirty column still in memory to the worker and waits for the card.
// Called on the way out; a no-op when the worker was never started (the hand-built world) or
// when no world directory could be created.
static void saveDirtyColumns(void)
{
	for (int i = 0; i < WORLD_MAP_SLOTS; i++) {
		const Column* col = s_world.slots[i];
		if (col && col->dirty) workerSubmitSave(col);
	}
	workerFlushSaves();
}

// ── Step 8.4: the title screen, and the two things it hands the rest of main ──────────
//
// BS_TITLE follows BS_BOTTOM_UI because it cannot do anything else: scene/title.c draws with
// gfx/sprite.h onto screenBottom(), and screenBottom() is NULL by construction in a
// BS_BOTTOM_UI=0 build (gfx/screen.c never creates the target). A build with a title screen
// and no bottom target would compile, boot, and hang on a black screen with no way past it.
//
// It is a switch of its own rather than just BS_BOTTOM_UI so it can be turned OFF in a build
// that still wants the bottom UI. Every scripted measurement build — BS_REPORT_ONLY, the
// stress knobs, the p9run.ps1 profile runs behind step 10.2 — drives the game with no
// operator, and a menu that waits for a tap would stop all of them dead at boot with no
// output at all. Those builds set BS_TITLE=0 and play SAVE_WORLD_NAME, which is exactly the
// world they played before this step existed, so a measurement taken before and after is
// still comparing the same thing.
#ifndef BS_TITLE
#define BS_TITLE BS_BOTTOM_UI
#endif

#if BS_TITLE && !BS_BOTTOM_UI
#error "BS_TITLE=1 needs BS_BOTTOM_UI=1: scene/title.c draws on screenBottom(), which a \
BS_BOTTOM_UI=0 build never creates, so the menu would be invisible and unescapable."
#endif

// app/options.h has to duplicate libctru's KEY_* bit values as hex literals, because it also
// compiles on the host and cannot include <3ds.h> (see the block comment above OPT_KEY_A in
// that file, which explicitly asks for this check to live on the console side). This is that
// check. If devkitPro ever renumbers hid.h's BIT() assignments, the build stops here instead
// of silently binding "jump" to a key that no longer exists.
_Static_assert(OPT_KEY_A      == KEY_A,      "OPT_KEY_A drifted from libctru KEY_A");
_Static_assert(OPT_KEY_X      == KEY_X,      "OPT_KEY_X drifted from libctru KEY_X");
_Static_assert(OPT_KEY_Y      == KEY_Y,      "OPT_KEY_Y drifted from libctru KEY_Y");
_Static_assert(OPT_KEY_DUP    == KEY_DUP,    "OPT_KEY_DUP drifted from libctru KEY_DUP");
_Static_assert(OPT_KEY_DDOWN  == KEY_DDOWN,  "OPT_KEY_DDOWN drifted from libctru KEY_DDOWN");
_Static_assert(OPT_KEY_DLEFT  == KEY_DLEFT,  "OPT_KEY_DLEFT drifted from libctru KEY_DLEFT");
_Static_assert(OPT_KEY_DRIGHT == KEY_DRIGHT, "OPT_KEY_DRIGHT drifted from libctru KEY_DRIGHT");

#if BS_TITLE

// Runs the menu until the player either picks a world or quits. True means play — and
// s_world_name now holds what they picked, so the very next saveWorldDir() call resolves to
// their directory. False means they chose Quit, or the system asked the app to close
// (aptMainLoop went false), and main must shut down without ever building a world.
//
// Blocking, on purpose. Nothing else in the program is alive yet: this runs before
// worldReportBuild, before the worker, before the player. That is what makes it safe to
// simply loop here rather than adding a mode to the main loop — there is no world to keep
// simulating and no worker thread to keep fed while the menu is up.
// `returning_from_server` opens the menu on the Multiplayer screen instead of the main one.
// It is true exactly when the previous session was a server session that has just ended, and
// it exists so the end of that session is not silent: netErrorText() is still holding
// whatever finished it ("Lost the connection to the server", or the server's own
// "Disconnected by the server"), and the Multiplayer screen is the only screen that prints
// it. Landing on the main menu instead would tell the player their world had vanished and
// nothing else. The cursor is left where titleInit put it, which is CONNECT — the one button
// they are most likely to want next.
static bool runTitleScreen(Options* opts, bool returning_from_server)
{
	C3D_RenderTarget* const bottom = screenBottom();
	C3D_RenderTarget* const top    = screenTop();

	// Not an assertion. The #error above makes the NULL case unreachable in any build that
	// compiles, but screenInit can still fail to create a target at runtime, and losing the
	// menu should cost the player the menu, not the game — so a missing bottom screen plays
	// SAVE_WORLD_NAME exactly as a BS_TITLE=0 build would.
	if (!bottom) return true;

	TitleState ts;
	titleInit(&ts);
	if (returning_from_server) ts.screen = TITLE_SCR_MULTIPLAYER;

	while (aptMainLoop()) {
		hidScanInput();

		// KEY_TOUCH is read from *held*, not down: title.c needs "is the stylus on the
		// screen right now" for its own tap-on-the-edge logic (TitleState.touch_prev), and a
		// down-only bit would be true for exactly one frame of a press.
		//
		// Both signals are consulted because hid.h flags KEY_TOUCH as "Not actually provided
		// by HID" — it is synthesised, and title.h's own comment recommends deriving touch
		// from the point instead. Taking either as a yes cannot lose a real touch, which is
		// the only direction of error that matters here: a spurious extra frame of "touching"
		// re-reports a tap the player really did make, while a missed one makes the button
		// they pressed do nothing and the menu feel broken.
		touchPosition tp = {0};
		hidTouchRead(&tp);

		const TitleInput in = {
			.keys_down  = hidKeysDown(),
			.touch_down = (hidKeysHeld() & KEY_TOUCH) != 0 || tp.px != 0 || tp.py != 0,
			.touch_x    = tp.px,
			.touch_y    = tp.py,
		};

		// The handshake only advances inside netUpdate(): netConnect() sends nothing itself,
		// it arms the transport and returns, and every retry, every packet drained and every
		// state change after that happens here (net/bsnet.c:348). The game loop below pumps it
		// every frame — but Multiplayer lives on the title screen, which is this loop, and this
		// loop did not, so a player who pressed Connect sat on "Connecting..." forever: the
		// HELLO was never retried and the server's COOKIE was never read off the socket. It is
		// pumped before titleUpdateDraw so the status the menu paints is this frame's, not the
		// previous one's, and it is unconditional for the same reason as the game loop's copy —
		// netUpdate() returns immediately when the net module was never initialised or is idle.
		netUpdate();

		// And the application-layer pump, for the same reason one loop up but a different
		// mechanism. netUpdate() decrypts arriving packets into net/bsnet_transport.c's rx ring,
		// which is 16 slots deep and evicts the *oldest* when it overflows. The one message a
		// joining player cannot afford to lose — BS_APP_WORLD_SYNC, every block edit already
		// made in this world — is sent immediately after JOIN, i.e. right here on the title
		// screen, and is then the oldest thing in that ring while the player spends the next
		// half-minute picking a world. Without this call it was reliably evicted by the next
		// sixteen packets and the player entered untouched terrain. networldUpdate() parks
		// early arrivals in the pending diff store (net/networld.c's applyOrQueue), where they
		// wait for the column each one belongs to — every diff lands in networldOnColumnLoad(),
		// on top of the terrain the worker generated, which is the only order that leaves it
		// visible.
		networldUpdate();

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		spriteFrameBegin();

		// The top screen is cleared and bound every frame even though the menu draws
		// nothing on it. C3D_FrameEnd only presents screens with a target bound this frame,
		// so skipping it would leave the top screen holding whatever was in the framebuffer
		// at boot — uninitialised VRAM — for as long as the player sits in the menu.
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);

		// title.c calls spriteBegin(320, 240) itself and assumes the caller has already
		// bound the target it wants drawn on — see the file comment in scene/title.h.
		C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0x1A1424FF, 0);
		C3D_FrameDrawOn(bottom);
		const TitleResult r = titleUpdateDraw(&ts, opts, &in);

		C3D_FrameEnd(0);

		if (r.action == TITLE_QUIT) return false;

		// Joining a server is entering its world — there is no name to copy and no directory
		// to resolve, so this returns without touching s_world_name. That leaves it at
		// SAVE_WORLD_NAME, which is only ever read for the loading screen's caption from here
		// on, because s_server_session gates every saveWorldDir() call downstream.
		if (r.action == TITLE_START_SERVER) {
			s_server_session = true;
			return true;
		}

		if (r.action == TITLE_START_WORLD) {
			// snprintf, not strcpy: both buffers are WORLDLIST_NAME_MAX and title.c will not
			// hand back an over-long name, but this is the one place a name crosses out of
			// the module that validates it, and a truncation here is a wrong save directory
			// rather than a crash.
			snprintf(s_world_name, sizeof(s_world_name), "%s", r.world_name);
			return true;
		}
	}

	// aptMainLoop() went false — the HOME menu closed us. Not a quit the player chose, but
	// the same answer: do not build a world, fall through to the shutdown path.
	return false;
}

#endif   // BS_TITLE

int main(void)
{
	// Step 8.5, first of everything. It only writes this thread's TLS exception slot — no
	// gfx, no FS, nothing that can itself fail — so every line after it is covered. A crash
	// during screenInit is exactly the kind that is impossible to diagnose otherwise,
	// because there is not yet a screen to print the error on.
	crashInit();

	// Before anything that could reach for crypto. libhydrogen's 3DS entropy
	// source is PS_GenerateRandomBytes, and hydro_init() aborts the process
	// rather than failing softly if PS is not open — so this cannot be done
	// lazily inside the net code, or a hydro_* call from anywhere else would
	// take the game down. Paired with psExit() at the bottom of main.
	psInit();

	// Right after psInit() for the same reason it exists: netInit() (net/bsnet.c) can reach
	// libhydrogen through net/bsnet_transport.c, which needs PS open. Safe even if the player
	// never opens Multiplayer — see bsnet.h's own contract.
	netInit();

	// Alongside netInit(), which is where networld.h says to put it — and it matters that it is
	// here rather than at world entry, where it used to live. networldInit() clears the pending
	// block-diff store, and the server's post-JOIN WORLD_SYNC (every edit every other player has
	// made) arrives while the player is still on the title screen choosing a world. Clearing at
	// world entry threw that batch away microseconds before networldSetWorld() would have
	// applied it. Leaving a session clears it instead — see netDisconnect() in net/bsnet.c.
	networldInit();

	screenInit();
	metricsInit();

#if BS_GPU_TESTS && BS_GPU_PREFLIGHT
	// Before the title screen and before a world exists, so a console that fails one of these
	// says so without the player having to reach the freeze first. Costs a few seconds of boot
	// and writes sdmc:/blocksmith/selftest.txt. Diagnostic builds only. See app/gputest.h.
	gpuTestPreflight();
#endif

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

	// Step 8.3. The GUI's batch and its font, brought up together because they are one
	// thing in practice: spriteRect() samples the solid white texel that lives *inside*
	// the font texture, so a batch without a font can draw nothing at all. A failure here
	// is not fatal the way chunkRenderInit's is — a game with no HUD is worse than a game,
	// but it is still a game — so it is recorded and the boot continues.
	const bool ui_ok = spriteInit() && fontInit();

	// Brought up in both builds on purpose, even though only a BS_BOTTOM_UI build draws
	// with it. The batch claims a shader and 48 KB of linear memory, and a console build
	// that skipped that would be measuring a different memory profile from the one that
	// ships — which is exactly the kind of difference that makes an Old 3DS headroom figure
	// wrong. The cast is what keeps -Wunused-variable quiet in the console build.
	(void)ui_ok;

	// Step 8.7's updater, before the menu that offers it: the Options page greys its button out
	// when updaterAvailable() is false, and it has to know that the first time it is drawn
	// rather than a frame later.
	//
	// `true` is not a guess — netInit() above is unconditional and its first act is
	// net/bsnet_sock.c's socInit(), so SOC belongs to the net module for the whole life of the
	// process and the updater must share it rather than re-open it. See updaterInit's own
	// comment for what happens if it does not. The return value is deliberately not checked
	// here: a console that cannot bring up am:net or the RomFs still plays the game, and
	// updaterAvailable() is how the menu finds out.
	updaterInit(true);

	// ── Step 8.4: settings, then the menu ────────────────────────────────────────────
	//
	// Hoisted out of the BS_WORLD_GEN block below, where it used to be the only caller,
	// because the options default needs the same answer — see the first-run block.
	bool new_3ds = false;
	APT_CheckNew3DS(&new_3ds);

	// optionsSave writes into sdmc:/blocksmith, which nothing has created yet at this point
	// in the boot: saveWorldDir() makes it, and that does not run until genStart. Made here
	// so a first run can actually keep the render distance it just worked out. mkdir's return
	// is ignored for the same reason it is inside saveWorldDir — EEXIST is the normal answer,
	// and the only thing that settles whether the card is usable is trying to write to it,
	// which optionsSave then does.
	mkdir("sdmc:/blocksmith", 0777);

	// Whether the file was there BEFORE the load, not after. app/options.c leaves a fresh
	// Options at RENDER_DIST_MIN and says in its own comment on optionsDefaults that it
	// cannot do better — it is host-portable and cannot ask APT_CheckNew3DS — and asks the
	// console side to raise it once on the first run. This is that first run. Probing the
	// file rather than comparing the value is what keeps a player who has deliberately
	// chosen the Old 3DS radius on a New 3DS from having it silently put back every boot.
	FILE* opt_probe = fopen(TITLE_OPTIONS_PATH, "rb");
	const bool options_existed = opt_probe != NULL;
	if (opt_probe) fclose(opt_probe);

	// Loaded once, here, and never again: the options screen edits this struct in place and
	// writes it back itself on the way out (see titleUpdateDraw's contract). A BS_TITLE=0
	// build never shows that screen and still gets a fully valid Options out of this call,
	// because optionsLoad defaults or clamps everything it cannot read.
	Options opts;
	optionsLoad(&opts, TITLE_OPTIONS_PATH, NULL);
	if (!options_existed) {
		opts.render_dist = renderDistDefault(new_3ds);
		optionsSave(&opts, TITLE_OPTIONS_PATH);
	}

	// ── One session ─────────────────────────────────────────────────────────────────────
	//
	// Everything from here to the `goto` at the bottom of main() is one visit to one world:
	// the menu, the world, the play loop and the teardown that undoes exactly what this pass
	// set up. A server session comes back here when the player leaves it; a single-player
	// session never does, and quits the app exactly as it always has.
	//
	// This is a backward goto rather than a `for (;;)` wrapping the same 700 lines, and that
	// is a deliberate trade. Indenting the whole body to gain a brace would have turned a
	// four-line behavioural change into a whole-file diff, which is the kind of change that
	// hides a real mistake inside a wall of whitespace. Jumping backwards past declarations
	// is well defined — every object below is re-initialised when control reaches its
	// declaration again — and the only state that has to be reset by hand is the one static
	// this loop can observe from the previous pass, which is done on the next line.
	//
	// What must stay OUTSIDE this region, and does: crashInit, psInit, netInit, networldInit,
	// screenInit, metricsInit, chunkRenderInit, spriteInit/fontInit and updaterInit, all of
	// them above. Their matching *Exit calls used to sit in the teardown below and have moved
	// past the goto with them, because a second session needs a screen and a chunk renderer
	// just as much as the first one did.
session_start:
	// The previous pass may have left this true. Nothing else survives a lap.
	s_server_session = false;

#if BS_TITLE
	// ui_ok gates it. spriteInit/fontInit failing is survivable for the HUD — a game with no
	// HUD is still a game — but it is not survivable for a menu, because the menu IS its
	// drawing: with no batch there are no buttons, no world list and no way to tell the
	// player any of that. Showing it anyway would trap them on a blank bottom screen with no
	// route into a world at all. A build that cannot draw the menu skips it and plays
	// SAVE_WORLD_NAME, exactly as a BS_TITLE=0 build does.
	//
	// s_left_server is what puts the menu back on the Multiplayer screen after a session
	// ends, so the player reads why it ended rather than guessing — see runTitleScreen.
	//
	// The answer is taken into a plain bool that exists in both configurations, rather than
	// jumping straight out of the #if, so that the goto below is compiled in a BS_TITLE=0
	// build too. A label with no reachable goto is a -Wunused-label warning, and a warning
	// that only appears in one configuration is a warning nobody sees until it is load
	// bearing.
	const bool menu_quit = ui_ok && !runTitleScreen(&opts, s_left_server);
	s_left_server = false;
#else
	const bool menu_quit = false;
#endif

	// Quit from the menu, or the system closed us while it was up. No world was ever built on
	// this pass, so there is nothing to save and nothing to tear down beyond what boot itself
	// brought up — and, on a second lap, the previous pass already tore its own world down
	// before coming back here.
	if (menu_quit) goto app_shutdown;

	// After the menu, because the options screen edits `opts` in place — doing this before it
	// would snapshot the settings the player had when they *opened* the menu, which is the
	// one moment they are guaranteed to be about to change. From here scene/player.c,
	// scene/interact.c and scene/camera.c read their bindings and look feel out of this
	// snapshot instead of out of hardcoded KEY_* constants.
	inputMapSet(&opts);

	// Self-test first: it resets the memory budget, so it has to run before the
	// world claims anything.
	//
	// Off by default since it was measured. This one call was the entire "I named my world and
	// it just sat there" complaint: with BS_BOOT_TIMING=1 it came back at 23855.6 ms, against
	// 3.1 ms for worldReportBuild, 158.0 for worldHasBeenPlayed and 117.1 for genStart — 98.9%
	// of a 24-second wait in which nothing was drawn at all. It is 818 assertions, most of them
	// building and tearing down whole worlds, on a 268 MHz ARM11.
	//
	// Nothing is lost by not running it here. tools/run_host_tests.sh runs this same code on
	// every build and reports "world self-test: PASS 2694 checks"; what the console run added
	// was proof that it also passes on the hardware — worth having on demand, not worth 24
	// seconds of every world the player ever opens. Ask for it with
	//   make EXTRA_CFLAGS="-DBS_SELFTEST=1"
	// and the overlay prints the result exactly as it always did.
	//
	// Skipping it leaves nothing half-initialised. worldTestRun's own first act is
	// budgetReset(), and every case inside it pairs its worldInit with a worldExit, so the
	// budget this was "resetting before the world claims anything" is simply the untouched boot
	// state when it does not run.
	char selftest[96] = "";
	BOOT_BEGIN();
#if BS_SELFTEST
	worldTestRun(selftest, sizeof(selftest), NULL);
#else
	snprintf(selftest, sizeof(selftest), "self-test off (BS_SELFTEST=0)");
#endif
	BOOT_END("worldTestRun");
	// Onto the overlay as well as into the BS_* report line. The report path is a
	// developer build; a normal Phase 4 boot never showed this result anywhere, which is
	// why "did the suite pass on the console?" was unanswerable rather than merely
	// awkward to read.
	metricsSetSelfTest(selftest);

	BOOT_BEGIN();
	const int refused = worldReportBuild();
	BOOT_END("worldReportBuild");

	// s_world exists now (worldReportBuild() ran worldInit()), so net/networld.c can be told
	// which World it is allowed to touch. Everything in networld.h is a harmless no-op before
	// this line, and this is also the pointer worker-thread calls into networldOnColumnLoad()
	// (see world/world.c's worldSet()) are compared against and ignored — see networld.h's
	// threading note.
	// networldInit() is deliberately NOT called here any more — it moved up next to netInit() at
	// boot, because clearing the pending store at this point discarded the joined session's
	// WORLD_SYNC batch. That batch is instead held until the column each edit belongs to is
	// generated and installed, which is what puts the joining player into the world everyone
	// else has been editing rather than a private copy of the same terrain. Note that the grid
	// worldReportBuild() just allocated is empty — the worker has not started — so this call
	// deliberately does nothing but register the pointer; applying a diff to those columns here
	// would only have it generated over. See networldSetWorld()'s own comment.
	BOOT_BEGIN();
	networldSetWorld(&s_world);
	BOOT_END("networldSetWorld");

	// The world. Since step 5.5 the generated one is not built here: the worker is started
	// and the whole area queued, and only the column the player stands in is waited for.
	// The other twenty-four arrive during the first frames, one per frame, while the game
	// is already drawing. The hand-built world has no generator to move off the main
	// thread and is still filled and meshed in one go.

	// Kept initialized in all configs; only used when BS_WORLD_GEN is 1, so the generated-terrain
	// streaming path stays identical across builds.
	(void)s_mesh_radius;
	(void)s_area_radius;

	// Set by the loading screen when the player asked to quit from a stalled load, or when the
	// system closed the app while it was up. Declared out here rather than inside the
	// BS_WORLD_GEN block below because the main loop that reads it is shared by both world
	// paths, and the hand-built world has no loading screen to set it.
	bool quit_requested = false;

#if BS_WORLD_GEN
	// Step 7.7. The console's default render distance, before the first column is asked for,
	// so the boot fills the ring the player is actually going to have rather than filling the
	// small one and then widening it a frame later.
	//
	// APT_CheckNew3DS is asked once, up with the options load — render_dist.c is deliberately
	// free of <3ds.h> so its arithmetic can be tested on the host, which means the one
	// genuinely console-shaped question in the whole setting — which machine is this — has to
	// be answered by the caller.
	//
	// Since step 8.4 the radius comes from the player's setting rather than straight from
	// renderDistDefault(). On a card with no options.ini the two are the same value, because
	// the first-run block above seeds the file with exactly renderDistDefault(new_3ds) — so a
	// fresh install still fills the ring this console can afford, and a profile measurement
	// taken before this step is still comparable to one taken after it. optionsLoad clamps
	// the field to [RENDER_DIST_MIN, RENDER_DIST_MAX], so a hand-edited ini cannot ask for a
	// ring the mesh pool has no slots for.
	genInitRadius(opts.render_dist);

	// Asked before genStart, so it describes the world the player picked rather than one this
	// boot has already written a column into.
	//
	// Not asked at all in a server session: the question is about this card's copy of a world,
	// a server session has none by design, and saveWorldDir() would create the directory just
	// to be told it is empty. The caption below reads "JOINING WORLD" on that path instead, so
	// the false here is never shown as "CREATING WORLD".
	BOOT_BEGIN();
	const bool world_played_before = !s_server_session && worldHasBeenPlayed(saveWorldDir());
	BOOT_END("worldHasBeenPlayed");

	// Centred on the spawn column, which is (0, 0) — the same column playerInit puts the
	// feet in below. From here on the ring follows the player (genFollow).
	BOOT_BEGIN();
	const bool worker_ok = genStart(0, 0);
	BOOT_END("genStart");
	BOOT_WRITE();

	// From here to the end of the run, a second thread is watching this one. Started here rather
	// than at the top of main() on purpose: everything before this point is the title screen,
	// where the software keyboard applet legitimately holds the main thread for as long as the
	// player takes to type a world name, and a watchdog cannot tell that apart from a hang.
	// Failure is not reported or handled — app/watchdog.h says every call is a no-op when the
	// thread could not be created, which is the same game this was before the file existed.
	watchdogStart();

	// Step 8.6. The wait for the world, with the console still answering the HOME button and
	// the player able to see what it is waiting for — see runLoadingScreen's own comment for
	// the hardware report that made this necessary. Two things changed besides the drawing:
	// this waits for the whole ring and its geometry rather than for the single spawn column,
	// which is what "I'm in it, but there's no blocks actually loaded in" was; and it can
	// return false, meaning the player asked to quit from a stalled load or the system closed
	// us. That is not an early return — the world, the worker and the card all exist by now, so
	// it drops through into the normal main loop, which breaks immediately and takes the usual
	// save-and-shutdown path out.
	if (!runLoadingScreen(ui_ok,
	                       s_server_session   ? "JOINING WORLD"
	                       : world_played_before ? "LOADING WORLD" : "CREATING WORLD",
	                       s_server_session ? netServerAddress() : s_world_name))
		quit_requested = true;

	// From here to the game loop's first watchdogPhase call is the handoff, and it is watched
	// in three pieces. Until these existed it was watched in none: runLoadingScreen's last act
	// is watchdogPhase(WD_PHASE_APT), an APT stall is ignored by design, and so a main thread
	// that stopped anywhere in here wrote no report — which is exactly the window the second
	// hardware freeze ("a hundred percent, it says ready, but it just sort of freezes") sits
	// in, since the last frame presented is the loading panel at 100%.
	watchdogPhase(WD_PHASE_HANDOFF_SAVE);

	// Step 8.1's proof. After the spawn column is guaranteed installed and before the player
	// exists, so it can unload and reload the column the player would otherwise be standing
	// in. Compiled out entirely unless BS_SAVE_CHECK is set.
	{
		const SaveCheckResult sc = saveCheck(BS_SAVE_CHECK);
		saveCheckReport(&sc);
	}
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

	// The two GPU-side inits below are the first candidates in the handoff worth naming
	// separately: both build buffers and bind shader programs, and both are the sort of call
	// that can wait on hardware in a way it does not on an emulator.
	watchdogPhase(WD_PHASE_HANDOFF_GPU);

#if BS_HANG_HANDOFF
	// See the BS_HANG_HANDOFF comment near the top of this file. Never in a release build.
	for (;;) svcSleepThread(1000000000ULL);
#endif

	// The highlight is not load-bearing: if its shader or buffer fails, the game is still
	// playable without a cage round the target block, so this reports and carries on
	// rather than joining chunkRenderInit's fatal path above.
	const bool highlight_ok = highlightInit();
	// Kept built in all configs; only used when BS_BOTTOM_UI is 1, so the game's render path
	// stays identical across builds.
	(void)highlight_ok;

	// Remote player bodies, on the same not-load-bearing footing as the highlight above: a
	// failure here costs the ability to see other players, which is worse in a session and
	// completely irrelevant in single player, and either way is not a reason to refuse to
	// boot. playerModelDraw checks its own ready flag, so nothing downstream needs this.
	(void)playerModelInit();

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

	// Step 8.2. Loaded after genStart, because that is what creates the world directory this
	// file lives beside; saveWorldDir() returns NULL if the card is not writable, and
	// inventoryLoad refuses a NULL directory, so the guard is the same test the region writer
	// uses rather than a second opinion about the SD card. inventoryLoad always leaves `s_inv`
	// valid — a missing or corrupt file is an empty inventory, not an error to handle here.
	// NULL in a server session, for the same reason the worker's directory is: the server owns
	// the world, this console keeps nothing from it, and saveWorldDir() would mkdir the very
	// directory the session is supposed not to create. One variable covers both ends — the
	// load here and the inventorySave on the quit path below are both already guarded on it —
	// so a joined session starts with an empty inventory and writes none back.
	watchdogPhase(WD_PHASE_HANDOFF_SAVE);
	const char* const inv_dir = s_server_session ? NULL : saveWorldDir();
	if (inv_dir) inventoryLoad(&s_inv, inv_dir);
	else         inventoryInit(&s_inv);
#if BS_BOTTOM_UI
	uiInit(&s_ui);
#endif

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
	watchdogPhase(WD_PHASE_HANDOFF_REPORT);
	worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr,
			                &player.cam);
	bool report_final = false;

	// Before the first frame, so the arm is chosen and the START line is on the card even if
	// this run is the one that freezes — see the BS_DRAW_PROBE comment. A no-op in any build
	// that does not define it, which is every build that ships.
	probeBegin();

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

		watchdogPhase(WD_PHASE_INPUT);
		hidScanInput();
		u32 down = hidKeysDown();
		// quit_requested comes from the loading screen (a stalled load the player gave up on,
		// or the system closing us while it was up). Taken here rather than skipping the loop
		// entirely so the exit runs exactly the path a START quit runs — inventory saved, dirty
		// columns flushed, worker stopped — instead of a second, less-tested teardown.
		if (down & KEY_START || quit_requested) break;

#if BS_BOTTOM_UI
		// Step 8.2. Read here, next to the rest of the frame's input, rather than inside
		// drawBottomUi: scene/ui.h states the same contract scene/title.h does — the UI never
		// calls into HID itself — so every read of the pad and the panel happens in one place
		// and cannot end up split across the frame.
		//
		// touch_down is taken as KEY_TOUCH *or* a non-zero point. libctru's hid.h comments
		// KEY_TOUCH as "Not actually provided by HID", and taking either as a yes cannot lose
		// a real touch, which is the only direction of error that matters here: a spurious
		// extra frame re-reports a tap the player did make, a missed one makes the slot they
		// pressed do nothing. ui.c edge-detects with its own touch_prev, so the extra frame
		// costs nothing.
		touchPosition tp = {0};
		hidTouchRead(&tp);
		const UiInput touch = {
			.touch_down = (hidKeysHeld() & KEY_TOUCH) != 0 || tp.px != 0 || tp.py != 0,
			.touch_x    = tp.px,
			.touch_y    = tp.py,
		};
#endif

		// Step 7.6. SELECT toggles 3D. gfxSet3D is what actually splits the top screen into
		// two framebuffers, so it moves with the flag rather than being set once: left on
		// while only one eye is being drawn, the hardware would show the right eye's stale
		// buffer to the right eye.
		if (down & KEY_SELECT) {
			s_stereo = !s_stereo;
			gfxSet3D(s_stereo);
		}

		// Every scene, every frame, whether or not Multiplayer is even open — see netUpdate()
		// and networldUpdate()'s own contracts for why neither ever blocks a frame. Pumped
		// before genInstallOne() below so a remote edit that just arrived for a column about
		// to be installed this same frame is already queued (net/blockdiff.h) before that
		// column's own drain call runs.
		watchdogPhase(WD_PHASE_NET);
		netUpdate();
		networldUpdate();

		// A server session with no server left is over, and this is where that is noticed.
		// The world on screen belongs to the server: its terrain came from the server's seed
		// and its edits from the server's diffs, none of it is on this card, and every block
		// broken from here on would go nowhere. Carrying on would be a convincing forgery of
		// a shared world — which is exactly what it used to do, indefinitely and without a
		// word, because net/bsnet_transport.c had no way to notice a server that stopped
		// answering (see BS_PROBE_MAX_TRIES there). Leaving takes the ordinary teardown and
		// lands on the Multiplayer screen with netErrorText() explaining it.
		//
		// Deliberately not gated on which status it moved to: NET_FAILED is the timeout and
		// the server's own kick, NET_IDLE would be something calling netDisconnect() — none
		// of them are a session this player is still in. Single-player never enters here,
		// because s_server_session is only ever true on the join path.
		if (s_server_session && netStatus() != NET_CONNECTED) break;

		// This console's own pose, going out to whoever else is in the session. Unconditional
		// every frame is correct — networldSendPose() rate-limits itself to
		// NETWORLD_POSE_INTERVAL_MS internally (net/networld.h) and is a no-op whether or
		// not Multiplayer is even connected, so there is nothing to gate here. Resending an
		// unchanged pose is deliberate, not waste: server/game/bsgame.c marks a player dirty
		// on every POS_UPDATE it receives, so a player standing perfectly still keeps being
		// broadcast and never ages out of anyone else's remote table.
		networldSendPose(player.body.x, player.body.y, player.body.z,
		                  player.cam.yaw, player.cam.pitch);

#if BS_WORLD_GEN && !BS_FLY
		// Step 7.7. L and R change the render distance. They are free in walking mode — only
		// the free-fly camera uses them, for up and down — and this is the one control the
		// setting can have until step 8.4 gives the game an options screen to hold it. It is
		// wired up rather than left for 8.4 because a setting that cannot be changed cannot be
		// verified: the two distances have to be reachable in one run for the fog and the ring
		// to be judged against each other.
		if (down & KEY_L) genSetRadius(s_mesh_radius - 1);
		if (down & KEY_R) genSetRadius(s_mesh_radius + 1);
#endif

#if BS_WORLD_GEN
		// Take delivery of at most one generated column, before anything reads the world
		// this frame. One column is all the worker can hand over per install
		// (app/worker.h). Meshing what that made ready happens further down, with the
		// edit queue, so the two share one budget rather than one each — see
		// DRAIN_BUDGET_MS. Install stays here because it is what puts ground under the
		// player, and it is a memcpy, not a mesh: 15.3 ms over 125 columns, 0.12 ms each.
		watchdogPhase(WD_PHASE_INSTALL);
		const u64 t_install = svcGetSystemTick();
		genInstallOne();
		const float install_ms =
			(float)((double)(svcGetSystemTick() - t_install) / CPU_TICKS_PER_MSEC);
#endif
		watchdogPhase(WD_PHASE_SIM);

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

		// Step 8.2. Refreshed every frame rather than written once when the player taps a
		// hotbar slot: the selected slot's contents change without the selection changing —
		// place the last block of a stack and the same slot is now empty — and a `holding`
		// updated only on selection would let the player keep placing a block they no longer
		// have. interactEdit refuses BLOCK_AIR, so an empty hand is a refusal, not a
		// long-range break.
		it.holding = inventoryHeldItem(&s_inv);

		interactEdit(&it, &s_world, &player.body, down);

		// Read once, straight after the call that sets them, because interactEdit clears both
		// at the top of its next call — see Interact.broke_id. A block that does not fit is
		// left on the floor, which is the honest outcome: inventoryAdd reports the refusal
		// rather than eating it, and the block is already gone from the world by the time we
		// are told, so there is nothing here that could put it back. The count is not tracked
		// separately — `it.refused` is about edits the world rejected, and this edit was
		// accepted; it is the pickup that failed.
		if (it.broke_id != BLOCK_AIR)
			inventoryAdd(&s_inv, it.broke_id, 1, NULL);

		// Charged only for a placement the world actually accepted. it.placed_id stays
		// BLOCK_AIR on every refusal path in interactEdit — no target, no entry face, cell
		// occupied, would entomb the player, empty hand — so a refused place cannot silently
		// consume a block out of the hotbar.
		if (it.placed_id != BLOCK_AIR)
			inventoryRemove(&s_inv, it.placed_id, 1);

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
		watchdogPhase(WD_PHASE_MESH);
		const u64 t_work = svcGetSystemTick();
		const int edit_built = chunkRenderDrainDirty(&s_world, DRAIN_BUDGET_MS,
		                                             DRAIN_MAX_CHUNKS);
		const float edit_ms =
			(float)((double)(svcGetSystemTick() - t_work) / CPU_TICKS_PER_MSEC);
		// Kept built in all configs; only used when BS_WORLD_GEN is 1, so the measurement of
		// hand-built-world performance stays consistent across configurations.
		(void)edit_built;
		(void)edit_ms;

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

		// Phase 4's status line. Every field answers a question a screenshot would
		// otherwise leave open: whether the ray found anything (`aim` block and face),
		// whether an edit reached the world (`b`roke / `p`laced / `r`efused), and whether
		// the queue is keeping up (`dq` current/peak) — read that against `worst` and
		// `over` above. The console is 32 columns wide and truncates without complaining,
		// so this has to stay inside it: "aim -12 8 -10 f2 b9 p9 r9 dq8/8" is 31.
		//
		// Built here, before the frame opens, rather than after it closes where it used to
		// live: step 8.3's bottom-screen UI draws this same string *inside* the frame, and
		// a value cannot be drawn before it exists. Both consumers now read one buffer, so
		// the console overlay and the touch screen can never disagree about what the player
		// is aiming at.
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
		// The multiplayer counterpart, and the reason it exists: a session that is Connected
		// but silently moving nothing looks *identical* on screen to one that works, so
		// "did my edit go anywhere, and did anyone else's arrive?" was unanswerable from a
		// screenshot. Each field splits the pipeline somewhere different (net/networld.h):
		//   s sent edits   r payloads received   y WORLD_SYNC entries seen
		//   a remote edits applied   q still queued for unloaded columns   p remote players
		// Same 32-column console limit as `status` above: "net s99 r999 y999 a999 q99 p9"
		// is 30.
		char netline[33];
		snprintf(netline, sizeof(netline), "net s%d r%d y%d a%d q%d p%d",
		         networldSentEdits(), networldRecvMsgs(), networldSyncEntries(),
		         networldAppliedEdits(), networldPendingCount(), networldRemoteCount());

#if BS_CMDBUF_PROBE
		// Throwaway instrument, not shipped. hang.txt proved the main thread finishes the
		// whole frame and then blocks in C3D_FrameBegin(C3D_FRAME_SYNCDRAW), so the GPU took
		// the world's commands and never signalled. One thing that does exactly that, and
		// grows with the number of chunks on screen, is the command buffer overrunning: it is
		// 0x40000 bytes (C3D_DEFAULT_CMDBUF_SIZE, passed in gfx/screen.c) and citro3d does not
		// bounds-check the writes into it, so a frame that needs more silently walks off the
		// end and the GPU is handed whatever follows it in the linear heap.
		//
		// Measurable here rather than on his console: Azahar runs the same citro3d and builds
		// the same command list, and the usage figure does not depend on the GPU ever
		// completing the frame.
		snprintf(netline, sizeof(netline), "cmd %5.1f%% max %5.1f%%",
		         (double)(s_cmdbuf_last * 100.0f), (double)(s_cmdbuf_max * 100.0f));
#endif

		// A BS_REPORT_ONLY build with no bottom UI has no reader for either, and an unused
		// buffer is a -Werror stop. Kept built anyway so the two builds run the same code
		// path and the strings cannot rot in the configuration nobody compiles.
		(void)status;
		(void)netline;

		// The one phase marker that is not a formality. C3D_FRAME_SYNCDRAW waits on the GPU
		// before it returns, and a GPU that never signals leaves the main thread here forever —
		// which is the shape of the report this watchdog was built for: a last frame still on
		// both screens, the HUD numbers frozen mid-world, and HOME dead.
#if BS_DRAW_GUARD
		// Refreshed here, immediately before the call that hangs, so the report describes every
		// draw issued up to and including the last frame the console completed. Formatted on
		// this thread into a static that outlives the process — the monitor thread only stores
		// the pointer, and it must not do work of its own (app/watchdog.h).
		{
			static char guard[160];
			const int code = chunkRenderGuardCode();
			if (code)
				snprintf(guard, sizeof(guard),
				         "BAD DRAW code %d hits %d slot %d first %lu count %lu "
				         "maxidx %lu cap %lu verts %08lx",
				         code, chunkRenderGuardHits(), chunkRenderGuardSlot(),
				         (unsigned long)chunkRenderGuardFirst(),
				         (unsigned long)chunkRenderGuardCount(),
				         (unsigned long)chunkRenderGuardMaxIdx(),
				         (unsigned long)chunkRenderGuardCap(),
				         (unsigned long)chunkRenderGuardVerts());
			else
				snprintf(guard, sizeof(guard),
				         "clean - every chunk draw issued this session validated");
			watchdogGuardLine(guard);
		}
#endif
		watchdogPhase(WD_PHASE_DRAW);
		metricsSyncBegin();
#if BS_DRAW_PROBE
		// The same two waits C3D_FrameBegin(C3D_FRAME_SYNCDRAW) performs internally, called
		// separately so the hang report can name which of them the console stopped in. See the
		// WD_DRAW_FRAME_VSYNC comment in app/watchdog.h for the disassembly this comes from:
		// SYNCDRAW is `C3D_FrameSync(); ...gxCmdQueueWait(-1)`, and C3D_FrameBegin(0) is that
		// second half on its own. Identical work in an identical order — only the marker is new.
#ifdef BS_VSYNC_STALL_TEST
		// The red arm for the new marker AND for the report's STOPPED branch, which the
		// emulator otherwise cannot reach: there is no way to switch off vblank interrupts.
		//
		// It does not need to. onVBlank0/1 only bump frameCounter when
		// (framerateCounter - framerate) <= 0, so a framerate small enough that the counter
		// never falls back to zero stops the tick while vblanks keep arriving and keep being
		// dispatched. C3D_FrameSync's exit condition is "a counter changed", so it then spins
		// forever on gspWaitForAnyEvent — precisely the failure mode the VSYNC marker exists to
		// name. A build armed with -DBS_VSYNC_STALL_TEST=<frames> must produce a hang.txt
		// reading "C3D_FrameSync - waiting for a VBLANK TICK" and "gsp vblank : STOPPED".
		//
		// NOT 0.0f, which was tried first and did nothing at all: C3D_FrameRate opens with
		// `vcmpe.f32 s15,#0.0` / `bxle lr` and rejects anything <= 0, so the arm was a no-op
		// and wrote no report. The accepted range is 0 < fps <= 60. 1e-30 is inside it, and
		// after the first tick leaves framerateCounter at 60.0f, where 60.0f - 1e-30f rounds
		// back to 60.0f exactly — so the counter can never reach zero again.
		{
			static int vstall;
			if (++vstall == (BS_VSYNC_STALL_TEST)) C3D_FrameRate(1.0e-30f);
		}
#endif
		drawStage(WD_DRAW_FRAME_VSYNC, -1);
		C3D_FrameSync();
		drawStage(WD_DRAW_FRAME_QUEUE, -1);
#if BS_GPU_TESTS
		// The whole reason this build can answer anything. C3D_FrameBegin(0) is a
		// gxCmdQueueWait(queue, -1) — an unbounded wait, which is why the console dies with the
		// HOME button unresponsive and the only evidence is what the watchdog thread can scrape
		// afterwards. Waiting the same way but with a two-second bound turns that freeze into a
		// return value: the main thread is still alive, and the GPU is still stuck in exactly the
		// state that stuck it, which is the one moment the post-mortem's questions can be asked.
		//
		// On the normal path this changes nothing at all. The queue drains in microseconds, this
		// wait returns true, and the C3D_FrameBegin below finds nothing left to wait for.
		{
			static uint32_t gpu_frame;
			gpu_frame++;
			if (!gpuTestFrameWait()) gpuTestPostMortem(gpu_frame);
		}
#endif
		C3D_FrameBegin(0);
#elif BS_GPU_TESTS
		// The shipping build's safety net, and the only piece of the diagnostic battery it keeps.
		//
		// C3D_FRAME_SYNCDRAW is documented in c3d/renderqueue.h as "perform C3D_FrameSync before
		// checking the GPU status", so the two lines below are exactly what the single call above
		// does — split apart only so the queue wait can be given a bound. On every frame that
		// works this costs nothing: the queue is already drained, the wait returns true at once,
		// and C3D_FrameBegin(0) finds nothing left to wait for.
		//
		// On a frame that does not work it is the difference between a console that has to be
		// held down to power off and one that writes postmortem.txt and keeps running. Up to
		// v1.1.8 that wait was unbounded, which is why every freeze report had to be scraped out
		// of the watchdog thread after the fact.
		C3D_FrameSync();
		{
			static uint32_t gpu_frame;
			gpu_frame++;
			if (!gpuTestFrameWait()) gpuTestPostMortem(gpu_frame);
		}
		C3D_FrameBegin(0);
#else
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#endif
		spriteFrameBegin();
		metricsSyncEnd();
			// Step 7.6. In 2D this is one call with iod 0, which is exactly the frame this
			// loop drew before the step existed. In 3D it is the same work twice, and that
			// is the cost the plan warned about rather than an implementation to be
			// optimised away: two eyes means two frustum tests, two sight walks and two
			// sets of draw calls, so the frame roughly doubles.
			//
			// Both eyes are drawn whenever 3D is on, even at slider 0 where the two
			// pictures are identical. Skipping the right eye there would leave the right
			// framebuffer holding whatever was in it when the slider was last open, and the
			// moment the player nudged the slider a stale frame would flash.
			const float iod = s_stereo ? osGet3DSliderState() * chunkRenderMaxIod() : 0.0f;
			drawEye(screenTop(), &view, &it.target, s_stereo ? -iod : 0.0f, 0);
			if (s_stereo)
				drawEye(screenTopRight(), &view, &it.target, iod, 1);
#if BS_BOTTOM_UI
			// Step 8.3. Last in the frame, and deliberately so: the sprite batch sets its
			// own render state and does not restore it (see gfx/sprite.h), while
			// chunkRenderDraw re-establishes everything it needs at the top of its next
			// call. Drawing the UI first would mean paying to put the world's state back.
			drawStage(WD_DRAW_BOTTOM, -1);
			drawBottomUi(ui_ok, highlight_ok ? status : "HIGHLIGHT INIT FAILED",
			             netline, &touch);
#endif
#if BS_CMDBUF_PROBE
		// Sampled here and not after C3D_FrameEnd, because FrameEnd is what rewinds the
		// command buffer — read it afterwards and the answer is always zero.
		s_cmdbuf_last = C3D_GetCmdBufUsage();
		if (s_cmdbuf_last > s_cmdbuf_max) s_cmdbuf_max = s_cmdbuf_last;
#endif
		drawStage(WD_DRAW_FRAME_END, -1);
		metricsSubmitBegin();
		C3D_FrameEnd(0);
		metricsSubmitEnd();

#if BS_DRAW_PROBE
		// Right here and nowhere else: FrameEnd has just handed this frame's command list and
		// its display transfers to GX and returned without waiting for them, so the queue is
		// guaranteed to be holding commands. That makes this the one place a snapshot can
		// prove the hang report's queue reader actually sees them. Writes one file, once.
		watchdogGxSelfTest();

		// Same instant, every frame rather than once: keeps the last two frames' command lists
		// so the report can write out the one the GPU never finished alongside the one it
		// finished just before it. Two 24 KB memcpys' worth of BSS and one memcpy per frame.
		watchdogCmdCapture();
#endif

		metricsFrameEnd();

		// One redraw when the world is finally complete: worker idle, nothing left to
		// install, nothing left to mesh. Once, not every frame — the console text render
		// is expensive enough that drawing it per frame would become the thing being
		// measured, which is why metricsDrawOverlay rate-limits itself.
		if (!report_final && !workerBusy() && jobqCount(&s_meshq) == 0) {
			report_final = true;
			worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr,
			                &player.cam);
		}
		// Frames the world took to fill, and step 6.3's whole criterion. The main thread
		// gives the worker its CPU by *blocking* on the GPU, so "did moving the worker to
		// the second core help?" cannot be answered by the main thread's own cpu ms — that
		// number is already 0.65 ms and has nowhere to go. How many frames the twenty-five
		// boot columns take to arrive can be.
		if (!report_final) s_genr.fill_frames++;

#if BS_REPORT_ONLY
		// Verification build: the overlay is suppressed so the whole report fits in
		// a short emulator window. Redrawn each frame because nothing else is.
		worldReportDraw(refused, WORLD_INTACT(), selftest, &stress, &dig, &s_genr,
			                &player.cam);
#else
		// `status` is built above, before the frame opens — see the comment there.

		metricsSetStereo(s_stereo, osGet3DSliderState(), screenVramFree(),
		                 screenRightEyeBytes());

		// Init failures belong on screen, not in a comment: without this the only symptom
		// of a broken highlight shader is "the cage does not appear", which is
		// indistinguishable from aiming at nothing.
		metricsDrawOverlay(highlight_ok ? status : "HIGHLIGHT INIT FAILED");
#endif

		// Last in the iteration, so a frame only counts once it has actually finished. The
		// counters go out with it: what the world was doing is half of what a hang report needs
		// to be worth reading, the other half being the phase markers above.
		watchdogCounters(s_world.columns, chunkRenderMeshes(), jobqCount(&s_meshq),
		                 workerBusy());
		watchdogBeat();

		// Next to watchdogBeat because it means the same thing — this frame finished — and a
		// survival line is only worth anything if it counts the same frames the watchdog would
		// have counted against the arm.
		probeFrame();

#if BS_HANG_TEST
		// See the BS_HANG_TEST comment near the top of this file. Never in a release build.
		{
			static int hang_frames = 0;
			if (++hang_frames >= BS_HANG_TEST) {
				watchdogPhase(WD_PHASE_SIM);
				for (;;) svcSleepThread(1000000000ULL);
			}
		}
#endif

		// And back to APT, which the watchdog treats as "not a hang" — the HOME menu can hold
		// the app suspended inside aptMainLoop() for as long as the player likes.
		watchdogPhase(WD_PHASE_APT);
	}

	// Step 8.1. Everything still in memory that the disk does not know about, written before
	// the world is torn down. Without this, quitting would lose every edit inside the ring —
	// which is all of them, since the player is standing in the ring — and the save file
	// would only ever contain the columns they had happened to walk away from.
	//
	// The whole slot table is walked rather than the ring, because the ring is a shape and
	// this is a list of what actually exists. A column can outlive the ring by a frame or
	// two when the radius changes, and one that did would be missed by a ring walk.
	// The quit path is the one place a long main-thread pause is legitimate — the dirty-column
	// flush below is real SD writes — but it is also code the hardware report never reached, so
	// the phase is marked rather than the watchdog stopped. A ten-second flush is worth a report.
	watchdogPhase(WD_PHASE_SAVE);

	saveDirtyColumns();

	// Step 8.2, and only here: unlike the world, the inventory is small enough to write whole
	// every time, so there is nothing to gain from saving it mid-play and a per-frame or
	// per-edit write would put an SD round trip inside the frame that just broke a block.
	// A failed save is counted by nobody and retried by nobody, same as a failed region
	// write — inventory.h says as much — because stalling the quit to retry is worse than
	// losing the last session's pickups.
	if (inv_dir) inventorySave(&s_inv, inv_dir);

	// Before worldExit: the worker holds a staging world of its own and must be joined
	// before anything it could still be writing into is freed.
	workerStop();

	// After the saving is done, so a slow flush is still being watched, and before the screens
	// come down so nothing it could report on has been freed yet.
	watchdogStop();

	// Before worldExit, and not optional now that the menu can be reached again with the app
	// still running. The menu loop pumps networldUpdate() every frame (that is what lets a
	// joining player's WORLD_SYNC arrive while they are still looking at buttons), and
	// net/networld.c would happily go on writing remote edits into the World this line is
	// about to free. networldSetWorld(NULL) is an explicitly supported state — see that
	// function's own early return — and puts arriving diffs back in the pending store where
	// they wait for whatever world comes next.
	networldSetWorld(NULL);

	worldExit(&s_world);
	highlightExit();
	playerModelExit();

	// Leaving a server session hangs up, and it has to happen before the menu is drawn again
	// for two separate reasons. The polite one: the gateway frees the slot and the other
	// players stop seeing a ghost, instead of the session ageing out on a timeout. The
	// load-bearing one: scene/title.c enters the world the instant it sees a live connection
	// with a known seed, so coming back to the Multiplayer screen still connected would drop
	// the player straight back into the world they just left, forever. netDisconnect() also
	// runs networldInit(), which clears the seed — so the next CONNECT waits for the next
	// server's BS_APP_WORLD_INFO rather than reusing this one's.
	if (s_server_session) {
		netDisconnect();
		s_left_server = true;
		goto session_start;
	}

app_shutdown:
	chunkRenderExit();
	metricsExit();
	screenExit();
	// Before netExit, which is what closes the SOC session the updater was told to share: the
	// updater's own worker thread is joined inside updaterExit, and joining it after its
	// sockets had been pulled out from under it would be the same ordering mistake workerStop
	// above exists to avoid.
	updaterExit();
	netExit();
	psExit();
	crashExit();
	return 0;
}
