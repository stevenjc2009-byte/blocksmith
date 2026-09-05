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
#include <malloc.h>
#include <string.h>
#include <sys/stat.h>

#include "app/battery.h"
#include "app/crash.h"
#include "app/debugmenu.h"
#include "app/debugmenu_ui.h"
#include "app/diag_retire.h"
#include "app/hw.h"
#include "audio/audio.h"
#include "audio/audio_sfx.h"
#include "app/input_map.h"
#include "app/memprobe.h"
#include "app/options.h"
#include "app/remap.h"
#include "app/remap_ui.h"
#include "app/session.h"
#include "app/sleep.h"
#include "app/updater.h"
#include "app/gputest.h"
#include "app/stage_probe.h"
#include "app/watchdog.h"
#include "app/worker.h"
#include "debug/biomeinfo.h"
#include "debug/biomeborder.h"        // v1.8.8 debug overlay: the toggle, and the geometry
#include "debug/biomeborder_draw.h"   // ...and the half that binds a shader
#include "debug/loadprof.h"
#include "debug/metrics.h"
#include "entity/animal.h"
#include "entity/entity.h"
#include "entity/monster.h"
#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/particles.h"
#include "gfx/screen.h"
#include "gfx/sprite.h"
#include "gfx/weatherdraw.h"
#include "net/bsnet.h"
#include "net/inv_bridge.h"
#include "net/networld.h"
#include "scene/camera.h"
#include "scene/chunk_render.h"
#include "scene/crackoverlay.h"
#include "scene/crosshair.h"
#include "scene/highlight.h"
#include "scene/playermodel.h"
#include "scene/entitymodel.h"
#include "scene/interact.h"
#include "scene/loading.h"
#include "scene/loading_draw.h"
#include "scene/player.h"
#include "scene/pausemenu.h"
#include "scene/ringorder.h"
#include "scene/title.h"
#include "scene/ui.h"
#include "world/blockstate.h"
#include "world/budget.h"
#include "world/daynight.h"
#include "world/furnace.h"
#include "world/genrefuse.h"
#include "world/genretry.h"
#include "world/handbuilt.h"
#include "world/inventory.h"
#include "world/jobq.h"
#include "world/light.h"
#include "world/mesh_vertex.h"
#include "world/meshq.h"
#include "world/playerpose.h"
#include "world/region.h"
#include "world/registry.h"
#include "world/relight_drain.h"
#include "world/relightq.h"
#include "world/rng.h"
#include "world/survival.h"
#include "world/tick.h"
#include "world/water.h"
#include "world/weather.h"
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

// v1.8.8 entity foundation. Owns nothing but a fixed pool -- see entity/entity.h. Initialised
// once a world is entered (worker_ok, beside the sleep hooks below) and ticked alongside water
// in the per-tick loop.
//
// v1.8.14. This comment used to end "No renderer reads it yet ... a spawned entity is simulated
// and never drawn", and BOTH halves of that are now false, so it is corrected here rather than
// left to be read as current -- a stale comment that still parses as a live statement has cost
// this project time before. scene/entitymodel.c implements the draw side and drawEye() calls
// entityModelDraw(view, &s_entities); entity/animal.c implements the creature side and the
// per-tick entityTick() below now passes animalThink instead of NULL. An entity in this pool is
// simulated, thought for, drawn, and can be hit.
static EntityWorld s_entities;

// v1.8.14 animals. The decision stream animalThink() and animalSpawnForColumn() roll against.
//
// It lives here, in main.c, and is handed to animal.c through entityTick()'s `user` pointer
// inside an AnimalCtx -- entity/animal.h:121-127 requires exactly that shape, so that a host
// test can own its own stream and no part of the creature layer holds hidden global state.
//
// A STREAM and not a positional hash, which is the distinction world/rng.h opens with. Terrain
// must hash, because a chunk's contents cannot be allowed to depend on how many chunks were
// generated before it. Animal decisions carry no such requirement -- a herd that rolled
// differently after a reload is not a different world -- and a sequence of decisions is exactly
// what a stream is for.
//
// Seeded from the world seed (below, beside entityWorldInit) rather than from the system clock,
// so a session is reproducible from a cold load, and salted so it cannot be walking in step with
// anything else derived from the same seed.
static Rng s_animal_rng;

// v1.8.0. Columns whose light a remote edit has invalidated, coalesced. Pushed by
// onRemoteEdit, drained once per frame ahead of the mesh drain — see world/relightq.h for
// why the relight cannot stay inline in the hook.
static RelightQueue s_relightq;

// v1.8.3. The clock world/relight_drain.c's budget runs against. A wrapper rather than
// svcGetSystemTick itself because RelightClockFn is declared in terms of uint64_t and the drain
// has to stay <3ds.h>-free to be host-testable — the host suite passes a counter of its own here,
// which is the only way the TIME half of that budget can be tested at all.
static uint64_t relightNowTicks(void) { return (uint64_t)svcGetSystemTick(); }

// v1.8.0 task 21. The simulation clock. Everything the spec defines per-tick — fluid spread,
// redstone, crop growth, smelting, hunger, mob AI — is phased against this and not against the
// render frame, so a mechanic runs at the same speed whether the console is holding 59.83 fps or
// struggling at 30. See world/tick.h.
static TickClock s_tickclock;

// v1.8.9 day/night. The world clock -- ticks within the current day plus elapsed-day count -- see
// world/daynight.h. Initialised from the world's time.bin sidecar (or DAY_START_TICKS on a fresh
// world / damaged file) once genStart() succeeds, advanced once per real tick alongside water and
// entities, and read back every frame in drawEye() to drive the sky colour and the chunk shader's
// dayLevel uniform.
static DayNight s_daynight;

// v1.8.9 weather rendering. The camera-following rain/snow strip's animation state (kind,
// scroll/drift phase, snapped grid position) -- see gfx/weatherdraw.h. Reset once per
// new/loaded world in genStart(), beside dayNightInit(), because it needs a real world to
// poll weatherAt() against; the GPU resources it draws through (shader/texture/vertex
// buffer) are claimed once for the process by weatherDrawInit(), beside chunkRenderInit().
static WeatherDrawState s_weatherdraw;

// v1.8.0 task 22. The water simulation: sources, flow levels, spread and drainage. Its state is
// the flow-level side map and the candidate queue — see world/water.h for why the level is not a
// block id and why an untouched ocean costs nothing.
static WaterSim s_water;

// Measured ticks per second, for the debug menu. Deliberately timed against svcGetSystemTick
// rather than against the same metricsFrameMs that feeds the clock: a readout derived from the
// clock's own time source would print 20.0 even if the clock were running against a wrong idea
// of what a second is, which is the one thing this readout exists to catch on hardware.
static float s_tps;
static u64   s_tps_mark_tick;
static u64   s_tps_mark_count;

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

	// The grid has done its whole job the moment `refused` is known: it exists to prove the
	// worst case can be claimed at all, not to be played in. It used to be left standing, and
	// the streaming ring then generated the real world *around* it — so a session that should
	// have held 25 columns held 305, and the overlay's column count was mostly this grid. The
	// cost was never just the bytes: these 289 columns also sat in 289 of world.h's 1024 hash
	// slots for the entire session, lengthening every probe the ring makes for its own.
	//
	// worldExit then worldInit rather than a loop of worldColumnRemove, because worldExit is
	// the one path that is already responsible for releasing a column's chunks at whatever form
	// each had grown into, and it leaves exactly the state worldInit gives — which is what the
	// caller believed it was getting all along.
	worldExit(&s_world);
	worldInit(&s_world);

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
// = 7.19 of 16.71) and the constants are left alone; if that stops being true, the chunk count
// (drainMaxChunks() below) is the knob, at the cost of a slower drain rather than a dropped
// frame.
//
// v1.8.17: THE COUNT IS NOW PER-CONSOLE, THE CLOCK IS NOT. Until now both were flat, so a New
// 3DS at 804 MHz with an L2 cache built exactly three chunks per frame, the same as an Old 3DS
// at 268 MHz, and left more than half of its 4.0 ms clock unspent every frame.
//
// Which of the two limits actually stops this drain is arithmetic on the loop below, and the
// loop checks the count BEFORE the clock: at a per-chunk cost c it admits 3 chunks whenever
// 2c < 4.0 ms, and stops on the clock instead once c >= 2.0 ms. So the answer depends entirely
// on c, and c was measured rather than assumed:
//
//   c on an OLD 3DS, MEASURED ON THE CONSOLE: 917 us meshChunk + 241 us neighbourhood gather
//   (both from the step 9.4c paired measurement recorded in CHANGELOG.md's "292 -> 241 us ...
//   with the mesher's own column unmoved (918 -> 917 us) as the control") + 630 us step 7.3
//   visibility flood fill = 1.79 ms. 2c = 3.58 < 4.0, so THE COUNT BINDS at 3 and the worst
//   case is 3 x 1.79 = 5.36 ms, not the 4.0 ms clock.
//
//   c on a NEW 3DS, PROJECTED, not measured: 1.79 / 3.0 = 0.60 ms, from the documented clock
//   ratio 804 / 268 = 3.0 with the L2 cache's contribution deliberately ignored so the figure
//   is a floor on the speedup rather than a hope. At 0.60 ms the unchanged 4.0 ms clock admits
//   7 chunks; the count stopped it at 3, with 2.2 ms of clock unspent.
//
// Hence 3 x the count on a New 3DS, which is the clock ratio: the number of chunks a 3x faster
// CPU gets through inside the SAME wall-clock budget. It is deliberately a little above the 7
// the projection allows, so that the CLOCK is the binder on a New 3DS. That is what makes this
// raise frame-safe without a console measurement of c_new, and the safety is a bound rather
// than an estimate: with the clock binding, the worst case is 4.0 ms + one chunk = 4.60 ms,
// which is LESS than the 5.36 ms an Old 3DS is already living inside today. Raising the count
// can only convert clock the Old 3DS never had to spend into chunks; it cannot lengthen the
// worst frame.
//
// DRAIN_BUDGET_MS is NOT per-console, on purpose. The frame is 16.71 ms on both consoles, so
// the clock is a frame-safety limit and there is nothing about a faster CPU that makes 4.0 ms
// of it safer to spend. Raising it would need a console measurement of what the rest of the
// frame costs on a New 3DS, and nothing in this tree has run on hardware since v1.2.5. If the
// New-3DS per-chunk cost ever turns out to be ABOVE 2.0 ms, the clock binds at 2 chunks there
// too and this count raise is inert -- in that case DRAIN_BUDGET_MS is the constant to move,
// and it should move on a measurement, not on this comment.
#define DRAIN_BUDGET_MS   4.0f
#define DRAIN_MAX_CHUNKS_OLD3DS  3
#define DRAIN_MAX_CHUNKS_NEW3DS  9

// Asked per use rather than cached in a file static, the same way audio.c:127 asks for its
// pool size and main.c already asks for the render distance ceiling at six call sites.
// hwIsNew3ds() returns a bool cached by hwInit() (app/hw.c), so this is a load and a branch,
// and every call in a frame is guaranteed to give the same answer.
static inline int drainMaxChunks(void)
{
	return hwIsNew3ds() ? DRAIN_MAX_CHUNKS_NEW3DS : DRAIN_MAX_CHUNKS_OLD3DS;
}

// The same two knobs for the relight drain that runs just before the mesh drain (v1.8.3). It had
// none until now — see the drain itself in the main loop, and world/relight_drain.h for what the
// old comment there got wrong.
//
// MEASURED, on the host, for this change. The tree quoted three inconsistent figures for one
// lightRelightColumn — 0.141 ms (light.h:13, world.h:54), 0.156 ms (light.c:47, light.h:152) and
// 720.1 us (relightq.h:4) — so the cost was timed directly rather than picked from among them.
// 240 samples, gcc -O1, six column profiles chosen to bracket the shape the drain actually meets
// (open plain, half-full, nearly-full, rolling hills, blocky mesa, noisy, the last three with
// air pockets under solid roof, which is where a flood fill iterates most):
//
//     flood fill, all profiles, n=240:  min 0.013  med 0.156  p95 0.227  max 0.287 ms
//
// The median lands on 0.156 ms EXACTLY, which is light.c:47's figure — that is the true one.
// 720.1 us in relightq.h:4 is not a flood-fill number at all: the same run measures the SWEEPS
// engine at med 0.950 / max 1.650 ms, and 720.1 us sits in that range, so relightq.h is quoting
// a pre-v1.8.0 sweeps-era cost. It is left alone here because correcting it is not this fix.
//
// The count cap binds the worst case, for the reason the mesh count above binds the mesh one:
// a clock can only stop the loop after a column has already overrun it. Four columns at the
// measured max of 0.287 ms is 1.148 ms, and that is the number to write down. Plus the mesh
// drain's own written-down 6.2 ms worst case, 0.62 ms of steady-state CPU and a 0.37 ms submit,
// it is 8.34 ms of 16.71 — inside, with 8.4 ms to spare.
//
// What it replaces, at the same measured figures: RELIGHTQ_CAP = 64 columns unbudgeted is 9.98 ms
// at the median and 18.4 ms at the max — a dropped frame ON THE HOST, before any ARM11 penalty.
//
// The time budget is a quarter of the mesh drain's, because a relight is a PREREQUISITE of a
// mesh and not a competitor to it: the drain exists so a chunk is not meshed with stale light,
// and it should never be the reason the mesh it is preparing for does not get built. On the host
// the COUNT is what binds — 4 columns at p95 is 0.908 ms, inside the 1.0 ms clock — so the clock
// is the backstop for the console, where every column is dearer. That asymmetry is the point: at
// any per-column cost above 1.0 ms the clock stops the drain after ONE column, so the console
// worst case degrades to one column plus a clock read rather than to four.
//
// NOT MEASURED ON A CONSOLE. Everything above is host gcc -O1 on x86; a 268 MHz ARM11 with no L2
// is dearer by a factor nobody here has measured, and nothing since v1.2.5 has run on hardware.
// The time budget is what makes that gap safe rather than fatal, but the four-column figure is a
// host figure and should not be quoted as a console one.
//
// Cost of the deferral: a 64-entry backlog now takes 16 frames to clear at 4/frame, so 0.27 s at
// 59.83 Hz instead of one 18.4 ms frame. If that turns out to be visible, this is the knob, at
// the cost of a slower drain rather than a dropped frame.
//
// v1.8.17: BOTH OF THESE STAY FLAT, and that is a measured decision rather than an oversight.
// The mesh count above went per-console in the same change, so the obvious symmetric move was
// to take RELIGHT_MAX_COLUMNS 4 -> 12 beside it. It was measured first, and the measurement
// says it would buy nothing on either console:
//
//   The four-column cap only throttles anything while the 1.0 ms clock still has room. Driven
//   through the REAL relightDrain on the host (gcc -O1, a real 7x7 generated ring rather than
//   the six synthetic profiles the figures above came from), one lightRelightColumn measures
//   101-146 us over six repeats, and the count/clock crossover is exactly where that predicts:
//   at max_columns 4 the drain returns 4 columns in 422-457 us of its 1000 us clock, and at
//   max_columns 12 or more it returns 10 columns in 1011-1130 us. So on the HOST the count is
//   the binder and raising it is worth 2.5x.
//
//   On a console it is not the binder at all. The host-to-console ratio for this project's own
//   code is ~19.5x, from the one function measured on both: meshChunk at 917 us on the console
//   (see the mesh block above) against 46.4-47.7 us for the same function on this host. At
//   that ratio a column costs ~2.1 ms on an Old 3DS and ~0.7 ms on a New one, and the sentence
//   three paragraphs up already says what happens then -- above 1.0 ms the clock stops the
//   drain after ONE column. Old 3DS: 1 column per frame. New 3DS: 2. Neither is anywhere near
//   4, so a cap of 12 would never be reached on either.
//
// So the binder here is RELIGHT_BUDGET_MS, not the count, and raising a TIME budget is the one
// move that can actually drop a frame. It needs a console measurement of one lightRelightColumn
// -- which this tree has never had, as the paragraph above says -- and not an argument. Left
// alone deliberately; do not "finish the job" by bumping the count to match the mesh one.
#define RELIGHT_BUDGET_MS    1.0f
#define RELIGHT_MAX_COLUMNS  4

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

// v1.8.1 task 50. How long BS_INTERACT_DEMO holds the break button. 240 frames is four
// seconds at 60 fps against a hardest-block requirement of 45 ticks (2.25 s), so the probe
// completes its break whatever it happens to be aimed at, and still completes it if the
// frame rate has halved — which on this console it can.
#define DEMO_BREAK_FRAMES 240

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
// See scene/render_dist.h for why RENDER_DIST_MAX is 3 and what else moves with it.
#define GEN_MESH_RADIUS_MAX  RENDER_DIST_MAX
#define GEN_AREA_RADIUS_MAX  (GEN_MESH_RADIUS_MAX + 1)

static int s_mesh_radius = RENDER_DIST_MIN;                   // 3x3 columns = 48x48 blocks
static int s_area_radius = RENDER_DIST_MIN + 1;               // 5x5 columns generated

// Streaming shares DRAIN_BUDGET_MS / drainMaxChunks() with the edit queue and takes
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

	// v1.8.11. mesh_refused above is the TOTAL, and it conflates a job-ring shortage
	// (world/meshq.c bumps it directly, and that path already retries itself) with a
	// chunkRenderBuild refusal (which until now retried nothing at all). These two split the
	// build refusals by cause, because the response to each is opposite — see
	// scene/chunk_render.h's ChunkRefuseReason.
	//
	// A single conflated counter is exactly why this bug survived: a nonzero mesh_refused
	// could not tell anyone whether the pool was momentarily full (recoverable, retry it) or
	// a chunk was too complex to mesh at all (unrecoverable, retrying spins forever), so it
	// read as noise and nothing acted on it.
	int  mesh_refused_pool;      // acquireSlot found nothing — transient, retried
	int  mesh_refused_overflow;  // chunk exceeds the per-chunk cap — permanent, NOT retried

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

// v1.8.3. Why the world the player just picked would not open, or NULL when nothing refused it.
// Set by genStart() below and carried, exactly like s_left_server above, across the one lap back
// to the menu that has to say it — cleared the moment runTitleScreen has put it on screen.
//
// A `const char*` rather than a buffer because every value it can hold is a string literal out
// of world/genrefuse.h with static storage duration, so there is nothing here that can dangle
// and nothing to copy. It lives beside s_left_server because it is the same kind of thing: a
// one-shot fact about the session that just ended, whose only reader is the menu.
static const char* s_world_refused_why = NULL;

// How long that line stays on the menu, in frames — ~6 s at the project's measured 59.83 fps.
// The same budget scene/title.c gives its own error lines (its WORLD_ERROR_STATUS_TTL, and for
// the same reason stated there: a message the player has to read *and act on* needs longer than
// one they can glance at). Spelled out again here rather than shared because scene/title.h does
// not export that constant; if either moves, move both.
#define WORLD_REFUSED_STATUS_TTL 360

static const char* saveWorldDir(void)
{
	static char dir[128];

	// v1.7.1 task 48b. Timed here rather than at the call sites because there are two of them
	// on the world-entry path — main()'s boot_dir and genStart()'s world_dir — and the whole
	// point of a stage count is that it says how many times the card was made to do this.
	const uint64_t t_dir = loadprofMark();

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
	if (!f) {
		loadprofSince(LOAD_STAGE_WORLDDIR, t_dir);
		return NULL;
	}
	const bool wrote = fwrite("bs", 1, 2, f) == 2;
	fclose(f);
	remove(probe);
	loadprofSince(LOAD_STAGE_WORLDDIR, t_dir);
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

	// v1.7.1 task 48b. A whole directory listing of the world folder, and it is on the entry
	// path of every single-player world, so it gets its own stage rather than being folded into
	// the setup total: it is the one piece of card work whose cost grows with how much the
	// player has built, which is exactly the shape of the complaint this task is chasing.
	const uint64_t t_played = loadprofMark();

	DIR* d = opendir(dir);
	if (!d) {
		loadprofSince(LOAD_STAGE_PLAYED, t_played);
		return false;
	}

	bool found = false;
	const struct dirent* e;
	while (!found && (e = readdir(d)) != NULL) {
		const char* dot = strrchr(e->d_name, '.');
		found = dot && strcmp(dot, ".bsr") == 0;
	}
	closedir(d);
	loadprofSince(LOAD_STAGE_PLAYED, t_played);
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
// redundancy. worldReportBuild() used to leave its 17x17 proving grid standing, so
// worldColumn() returned non-NULL for columns 0..16 from the first moment: testing the world
// called their ring complete before a single block had been generated and meshed them against
// air — 32,358 triangles where the same world meshed after full generation is 29,754. That
// grid is now freed as soon as its number is known, but the flag stays, because "the World has
// a Column object here" and "this column's blocks have arrived" were never the same question:
// a partially generated column is installed and meshable, and net/blockdiff.c can create a
// Column for an edit that landed ahead of the terrain it belongs to.
typedef struct {
	int32_t cx, cz;
	bool    set;
} ColSlot;

static ColSlot s_col_in[GEN_AREA_SPAN][GEN_AREA_SPAN];      // generated and installed
static ColSlot s_col_queued[GEN_MESH_SPAN][GEN_MESH_SPAN];  // chunks pushed to the mesh queue
static ColSlot s_col_asked[GEN_AREA_SPAN][GEN_AREA_SPAN];   // submitted to the worker

// v1.8.10. Columns that installed with ok == false above (world/genretry.h) — a budget or
// column-table refusal, not a hole s_col_in's "generated and installed" flag can tell apart
// from a genuinely complete column. Recovering them without ever touching s_col_in is the
// point: unmarking a partial column here would re-open the exact neighbour-stall problem the
// "marked before the ring is tested" comment on genInstallOne exists to avoid. Initialised by
// genStart() below, alongside the memsets that clear s_col_in/s_col_asked/s_col_queued for a
// fresh world.
static GenRetryLedger s_gen_retry;

// v1.8.11. The same ledger again, one layer up, for the bug world/genretry.h's own header
// describes but only half fixed.
//
// genretry.h fixed the GENERATION half: a column that installed with a hole was in s_col_in
// forever, so genRequestArea would never ask for it again. The MESH half was left with exactly
// the same shape and nobody noticed, because the two failures look identical from the player's
// seat. genQueueReadyColumns sets s_col_queued the moment meshqPushColumn accepts a column, and
// then skips any column carrying that mark. But acceptance into the job ring is not the same
// event as a successful mesh: genDrainMesh pops the job, calls chunkRenderBuild, and if that
// refuses, the job is already gone and the mark is still set. Nothing re-pushes it. The column
// stays a hole until it leaves the mesh ring entirely (genRecenter/genSetRadius are the only
// places that clear s_col_queued) and the player walks back into it.
//
// That is the reported symptom word for word: chunks loaded beyond a gap, and the gap filling
// only once you are almost on top of it. Nearest-first ordering (scene/ringorder.c) rules out a
// merely slow queue as the cause — a throttled queue fills inward-out and can never leave a
// loaded column BEHIND an unloaded one — so a dropped request was the only shape left.
//
// Keyed by column, not by chunk, because that is what the ledger stores and what the re-push
// operates on. Re-meshing a column's other chunks alongside the one that failed is a few
// hundred microseconds of duplicated work on a path that runs at most once per column per
// backoff period, which is a straight trade against a permanent hole.
static GenRetryLedger s_mesh_retry;

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

// Set whenever a column could not be fully queued because the mesh ring was full, and cleared
// by the pass that finally gets all of them in. Read by genDrainMesh, which is the one function
// called every frame in both the loading loop and the play loop and therefore the only place
// that reliably notices the queue has room again.
//
// v1.6.0 task 12. Without this, "the column stays re-queueable" is only true in principle:
// genQueueReadyColumns runs when a column arrives (genInstallOne) or when the ring moves
// (genRecenter/genSetRadius), so a player standing still in a fully installed world would never
// trigger another pass and the short column would stay unmeshed until they walked. A flag is
// enough because the retry is a whole-ring rescan — nothing has to remember WHICH column.
static bool s_mesh_queue_short;

// v1.7.1 task 49. Two per-frame accumulators for debug/metrics.h's MetricsWork. They live here
// rather than in the frame loop because the work they measure happens several call levels down
// — genFollow -> genRecenter -> genUnloadColumn -> workerSubmitSave — and threading a timer
// through four signatures to instrument them would be a worse change than two file statics.
//
// Reset by the frame loop immediately after it publishes the previous frame's figures, and
// accumulated with += because genUnloadColumn is called once per column dropped and a single
// recentre drops a whole row of them.
static float s_frame_recenter_ms;
static float s_frame_save_ms;

// Queues every column inside the meshed radius whose ring is now complete and that has not
// been queued already.
//
// v1.6.0 task 12: the per-chunk pushing moved to world/meshq.c so the host suite could link the
// real thing, and — the actual fix — the column is marked queued only if every push was
// accepted. It used to be marked first and unconditionally, which made a refused push a
// permanent hole in the world. See world/meshq.h for the full account.
//
// v1.8.5. This function is the burst that sizes JOBQ_CAP: on a distance change or a boot it can
// push one JOB_MESH per non-air chunk of every column in the ring, all in one pass, before
// anything drains. The structural worst case is therefore every column times every chunk.
//
// The assert lives here rather than in world/jobq.h because source/world deliberately does not
// include source/scene, so jobq.h cannot see RENDER_DIST_MAX (world/dirtyq.h has the same
// problem and is handled the same way). It exists because JOBQ_CAP has silently gone stale under
// a render-distance change TWICE — at RENDER_DIST_MAX 2 and again at 5 — and on both occasions
// the build was completely clean. A refused push is not corruption, jobqPush counts it in
// `dropped`, but it is a column that never gets meshed and so a hole in the terrain.
_Static_assert(JOBQ_CAP >= RENDER_DIST_MAX_COLUMNS * COLUMN_CHUNKS,
               "world/jobq.h's JOBQ_CAP is smaller than one full ring of mesh jobs, so a boot "
               "or a distance change can overflow the queue and silently drop chunks. It is a "
               "hand-maintained number; raise it to the next power of two above "
               "RENDER_DIST_MAX_COLUMNS * COLUMN_CHUNKS.");
_Static_assert((JOBQ_CAP & (JOBQ_CAP - 1)) == 0,
               "JOBQ_CAP must stay a power of two: jobq.c wraps head/tail with % JOBQ_CAP and "
               "ARM11 has no integer divide instruction.");

// v1.8.7. Both loops below walk scene/ringorder.h's table instead of `for dz { for dx }`, so the
// ring is asked for and meshed NEAREST FIRST. See that header for why the order matters at all;
// the short version is that the worker's queue is FIFO, so submission order is arrival order, and
// row-major spent its first columns on the corner furthest from the player.
//
// The table has to COVER the widest ring either loop can be walked over, which is the AREA ring
// (mesh radius + 1), not the mesh ring. A table one short would not fail to compile and would not
// crash: the outermost columns would simply never be visited, so the far edge of the world would
// stop generating. That is the exact silent-staleness failure the two JOBQ_CAP asserts above
// exist for, twice over, so it gets the same treatment.
_Static_assert(RING_ORDER_MAX_RADIUS >= GEN_AREA_RADIUS_MAX,
               "scene/ringorder.h's table is narrower than the generated ring, so the outermost "
               "columns would never be requested and the far edge of the world would stop "
               "loading — with a completely clean build. RING_ORDER_MAX_RADIUS is derived from "
               "RENDER_DIST_MAX; GEN_AREA_RADIUS_MAX is GEN_MESH_RADIUS_MAX + 1.");

// v1.8.10. Same failure shape as the two JOBQ_CAP asserts above: world/genretry.h's
// GEN_RETRY_SLOTS is a hand-maintained bound, sized for today's GEN_AREA_SPAN. A render-distance
// change that grows GEN_AREA_SPAN past it would not fail to compile and would not crash —
// genRetryInit(&s_gen_retry, GEN_AREA_SPAN) below would just return false, silently leaving
// s_gen_retry zeroed and every future budget refusal an unrecovered hole again.
_Static_assert(GEN_AREA_SPAN * GEN_AREA_SPAN <= GEN_RETRY_SLOTS,
               "world/genretry.h's GEN_RETRY_SLOTS is smaller than one full area ring, so "
               "genRetryInit would refuse to start and every budget-refused column would go "
               "back to being a silent permanent hole. Raise GEN_RETRY_SLOTS to at least "
               "GEN_AREA_SPAN * GEN_AREA_SPAN.");

// v1.8.11, and the same argument for s_mesh_retry. GEN_MESH_SPAN is the narrower of the two, so
// this cannot fail while the assert above passes — it is here anyway, because "it is implied by
// the other one" is a property of today's spans, not a rule, and if the mesh ring ever grows
// past the area ring this is the assert that says so instead of genRetryInit quietly returning
// false and every refused column going back to being a permanent hole.
_Static_assert(GEN_MESH_SPAN * GEN_MESH_SPAN <= GEN_RETRY_SLOTS,
               "world/genretry.h's GEN_RETRY_SLOTS is smaller than one full mesh ring, so "
               "genRetryInit(&s_mesh_retry, GEN_MESH_SPAN) would refuse to start and every "
               "mesh-refused column would be a silent permanent hole again.");
static void genQueueReadyColumns(void)
{
	bool still_short = false;

	// Nearest first. This loop is also the one that decides who gets the mesh queue's remaining
	// room when it is short (s_mesh_queue_short below), so the old order meant a full queue
	// spent its last slots on the columns furthest away and left the near ones to the retry.
	//
	// `seen < want` is what keeps this from being a regression at short render distances. The
	// table always holds the WIDEST ring the game can ask for, so a radius-1 player walking it
	// end to end would test 169 entries to find the 9 that are theirs, where the old nested pair
	// tested exactly 9. Stopping once every in-range column has been visited is exact, not an
	// approximation: the table is a permutation of the square (ringorder_test.c checks it), so a
	// ring of radius r contains precisely (2r+1)^2 of its entries and there is nothing left to
	// find after the last one. At radius 1 that stops on entry 9.
	const RingOffset* const ord = ringOrder();
	const int want = (2 * s_mesh_radius + 1) * (2 * s_mesh_radius + 1);
	int seen = 0;
	for (int i = 0; i < RING_ORDER_COUNT && seen < want; i++) {
		const int32_t dx = ord[i].dx, dz = ord[i].dz;
		if (dx < -s_mesh_radius || dx > s_mesh_radius ||
		    dz < -s_mesh_radius || dz > s_mesh_radius) continue;
		seen++;

		const int32_t cx = s_center_cx + dx, cz = s_center_cz + dz;
		if (genSlotHas(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz)) continue;
		if (!genColumnRingComplete(cx, cz)) continue;

		if (meshqPushColumn(&s_meshq, &s_world, cx, cz, &s_genr.mesh_refused))
			genSlotSet(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);
		else
			still_short = true;
	}

	s_mesh_queue_short = still_short;
}

// Submits every column of the current area that has neither arrived nor been asked for, nearest
// first — so on a recentre the column the player is walking INTO is the first one asked for
// rather than the seventh of thirteen, and on a boot the ring fills outward from their feet.
static void genRequestArea(void)
{
	// See genQueueReadyColumns above for why the walk stops at `want` rather than at the end of
	// the table.
	const RingOffset* const ord = ringOrder();
	const int want = (2 * s_area_radius + 1) * (2 * s_area_radius + 1);
	int seen = 0;
	for (int i = 0; i < RING_ORDER_COUNT && seen < want; i++) {
		const int32_t dx = ord[i].dx, dz = ord[i].dz;
		if (dx < -s_area_radius || dx > s_area_radius ||
		    dz < -s_area_radius || dz > s_area_radius) continue;
		seen++;

		const int32_t cx = s_center_cx + dx, cz = s_center_cz + dz;
		if (genSlotHas(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz)) continue;
		if (genSlotHas(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz)) continue;

		if (workerSubmitColumn(cx, cz)) genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
		else                            s_genr.submit_failed++;
	}
}

// world/genretry.h's submit callback for s_gen_retry. A retried column is not a different kind
// of request — it is asked for exactly the way genRequestArea above asks the first time
// (workerSubmitColumn, then genSlotSet(s_col_asked) on success) — the only thing different about
// it is WHY it is being asked again, which genRequestArea's own "not already in s_col_in" gate
// would otherwise never allow (s_col_in already has it, from genInstallOne marking it installed
// with a hole). Returning false here means the job ring was full this frame, not a generation
// failure, so genRetryTick leaves the backoff alone and offers it again next frame instead of
// waiting out a whole cooldown for what is a one-frame condition.
static bool genRetrySubmit(void* ud, int32_t cx, int32_t cz)
{
	(void)ud;
	if (!workerSubmitColumn(cx, cz)) return false;
	genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
	return true;
}

// world/genretry.h's submit callback for s_mesh_retry — the mesh-side twin of genRetrySubmit
// above. v1.8.11.
//
// The single load-bearing line is the genSlotClear. s_col_queued is what makes the hole
// permanent: genQueueReadyColumns will not look at a column that carries it, and nothing on the
// mesh path ever takes it off. Dropping it here is what lets the column be pushed again at all.
//
// RETURN VALUE. This deliberately returns false on the success path, which reads backwards and
// is not a mistake. genRetryTick marks an entry `inflight` when submit returns true, meaning
// "resubmitted, now waiting to hear back" — and for the generation ledger something genuinely
// does report back later (genInstallOne calls genRetryClear or genRetryMark). On the mesh side
// there is no such callback: the re-push either lands in the job ring now or it does not, and
// whether the resulting build succeeds is reported by genDrainMesh marking the column afresh.
// So the entry is retired here, the moment the re-push is accepted, and returning false keeps
// genRetryTick from setting inflight on a slot that no longer holds anything. A column that
// fails again is simply marked again, at attempts 0.
//
// That flat retry cadence (a fixed GEN_RETRY_BASE_FRAMES rather than a doubling backoff) is
// only safe because the caller never marks the unrecoverable class: genDrainMesh retries
// CHUNK_REFUSE_POOL and never CHUNK_REFUSE_OVERFLOW. Pool pressure is transient and clears as
// columns unload, so this loop terminates on its own. An overflowing chunk would not, which is
// exactly why it is kept out of this ledger rather than given a longer backoff.
static bool meshRetrySubmit(void* ud, int32_t cx, int32_t cz)
{
	GenRetryLedger* h = (GenRetryLedger*)ud;

	// The ring moved past it, or a neighbour it needs has gone away again. genRetryTick's own
	// radius check catches the first; the second is this function's to catch, because a column
	// whose 3x3 is no longer complete cannot be meshed and re-pushing it would put a job in the
	// ring that genDrainMesh would only refuse again.
	if (!genInMesh(cx, cz) || !genColumnRingComplete(cx, cz)) {
		genRetryClear(h, cx, cz);
		return false;
	}

	genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);

	// Job ring full this frame. The mark is left standing and the s_col_queued bit stays
	// cleared, so either this callback gets another go next frame or genQueueReadyColumns
	// picks the column up first — both of which end with it meshed, which is the only thing
	// that matters here.
	if (!meshqPushColumn(&s_meshq, &s_world, cx, cz, &s_genr.mesh_refused)) return false;

	genSlotSet(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);
	genRetryClear(h, cx, cz);
	return false;
}

// Frees a column and everything drawn from it. Safe to call for a column that was only
// asked for and never arrived.
static void genUnloadColumn(int32_t cx, int32_t cz)
{
	// Tell the server to stop sending edits for this column before anything else, because
	// after this function nothing here can use them: the blocks are gone and a diff arriving
	// for them would only be queued for a column that is no longer loaded. A no-op in single
	// player and whenever there is no session. Columns dropped for having gone stale (see
	// genInstallOne) never subscribed in the first place and correctly never unsubscribe.
	networldUnsubscribeColumn(cx, cz);

	// Meshes first. A slot outliving its chunks would keep drawing terrain that is not
	// there, and would still match in chunkRenderTouch — so an edit near the boundary
	// would queue a remesh of a chunk that now reads as air and blank a neighbour.
	chunkRenderReleaseColumn(cx, cz);
	genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, cx, cz);

	// v1.8.0 task 22. Flowing water leaves with its column, and it MUST leave before the save
	// encode two lines below rather than after: a flow cell written to the card comes back with
	// no map entry, and world/water.h's absence-means-source rule would read it as a spring. One
	// walk away and back would turn a puddle into a permanent fountain, and it would be in the
	// save file, so it would never go away again. Sources are left exactly where they are.
	//
	// Runs for every unloading column, dirty or not — the map is keyed by world coordinate and
	// an entry left behind for a column that no longer exists would still be found by a later
	// waterLevelAt at the same place in a different world.
	(void)waterDropColumn(&s_water, &s_world, cx, cz);

	// Step 8.1. The last moment this column's blocks exist. Only columns the player edited
	// are written — everything else the seed reproduces exactly, and saving it would turn
	// walking in a straight line into a stream of pointless SD writes.
	//
	// Before worldColumnRemove and not after, for the obvious reason, and the encode happens
	// here on the main thread because this thread owns the World; only the bytes cross to
	// the worker. See app/worker.h.
	const Column* col = worldColumn(&s_world, cx, cz);
	if (col && col->dirty) {
		// v1.7.1 task 49. Timed because workerSubmitSave can BLOCK: there are two save slots,
		// and with both busy it spin-sleeps the main thread in 1 ms steps until one frees, on a
		// worker that shares core 0 with this thread. That only ever happens where the player
		// has built — which is exactly the "certain spots" in steve's report, and is invisible
		// to every other column in this CSV.
		//
		// v1.8.16 FRZ-FIX. Bracketed with its own phase for the same reason gpuWaitPrevFrame is
		// bracketed: without it a stall in here reports `phase: SIM` in hang.txt, because
		// WD_PHASE_SIM was set at the top of the frame and WD_PHASE_SAVE was only ever set on
		// the quit path. A report naming SIM points at player and camera code that has nothing
		// to do with the SD card, which defeats the point of having a phase at all.
		//
		// The phase is SAVED and RESTORED rather than set back to WD_PHASE_SIM, because this
		// function has two callers: the in-frame ring follow (SIM) and the startup save
		// self-check (HANDOFF_SAVE). A hardcoded restore would be correct for one and a lie for
		// the other.
		const u64 t0 = svcGetSystemTick();
		const WdPhase prev_phase = watchdogPhaseGet();
		watchdogPhase(WD_PHASE_SAVE);
		workerSubmitSave(col);
		watchdogPhase(prev_phase);
		s_frame_save_ms +=
			(float)((double)(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
	}

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
	// v1.8.5: the ceiling is the CONSOLE's, not the compile-time one. An Old 3DS must still
	// stop at 3 in a binary whose pool can hold 5.
	radius = renderDistClampFor(radius, hwIsNew3ds());
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
	// v1.8.5: per-console ceiling, same reasoning as genInitRadius above.
	radius = renderDistClampFor(radius, hwIsNew3ds());
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

// Everything from here down to the #if below is shared, not streaming machinery: the debug
// menu's state and rows, and the per-session UI flags beside them, are read from main() and
// from the frame loop, neither of which is guarded. Hoisted out for exactly the reason
// saveWorldDir was (see its comment above): sitting inside compiled fine for every ordinary
// build, because BS_WORLD_GEN defaults to 1, and left BS_WORLD_GEN=0 with a dozen undeclared
// identifiers and implicit declarations at call sites that were never generated-world only.
#endif   // BS_WORLD_GEN

// The one thing the hoisted block still needs from the streaming machinery, in the build that
// has none. There is no ring to resize when the world is hand-built — it is filled and meshed
// in one call and never re-meshed — so a render-distance change has nothing to act on and this
// does nothing. A no-op definition rather than an #if at each caller because there are three
// of them and two (the debug menu's slider, the pause menu's stepper) are shared UI compiled
// into both builds; the third, the L and R shortcut in the frame loop, is already compiled out
// under BS_WORLD_GEN=0 for the same reason, and that is the precedent this follows. It is
// deliberately not chunkRenderSetDistance: changing the fog and the near plane in a world
// nothing streams into would be new behaviour, and nothing has asked for it.
#if !BS_WORLD_GEN
static void genSetRadius(int radius) { (void)radius; }
#endif

// The seed genStart() actually generated from — BS_WORLD_SEED in single player, the server's
// own seed in a session. Kept because the gen overlay's readout is otherwise a lie the moment
// the two differ, and "which world am I standing in" is exactly what that line is for.
static uint32_t s_seed_used = BS_WORLD_SEED;

// v1.4.0: control-remap screen state and the debug-menu flag. Reset per session, like the
// seed above. The minimap's fog-of-war buffer and save path used to live here too; the
// whole feature was removed in v1.5.1 (see below, at the old init site).
static RemapState s_remap;
static bool       s_remap_open;
static bool       s_debug_open;

// Live numbers the debug menu's INFO rows read. Refreshed every frame the menu is open;
// entries hold this struct's address, so it lives at file scope rather than on the frame.
static DebugContext s_dctx;

// The debug menu's render-distance stepper reports through this wrapper because
// chunkRenderSetDistance lives behind <3ds.h> and the DebugContext only carries a pointer.
// genSetRadius clamps and re-meshes the ring live; persisting opts.render_dist is the
// caller's job, done right after debugMenuUiUpdate returns.
static void bsDebugSetRenderDist(int r) { genSetRadius(r); }

#if BS_BOTTOM_UI
static int bsDbgGetDist(void* ctx)
{
	return ((const DebugContext*)ctx)->render_dist;
}

static void bsDbgSetDist(void* ctx, int v)
{
	(void)ctx;
	bsDebugSetRenderDist(v);
}

static const char* bsDbgInfoFrame(char* buf, int cap, void* ctx)
{
	snprintf(buf, (size_t)cap, "%.2f ms", ((const DebugContext*)ctx)->frame_ms);
	return buf;
}

static const char* bsDbgInfoDraw(char* buf, int cap, void* ctx)
{
	const DebugContext* d = (const DebugContext*)ctx;
	snprintf(buf, (size_t)cap, "%d meshes %lu tris", d->meshes, (unsigned long)d->tris);
	return buf;
}

static const char* bsDbgInfoCull(char* buf, int cap, void* ctx)
{
	snprintf(buf, (size_t)cap, "%d", ((const DebugContext*)ctx)->culled);
	return buf;
}

// v1.8.0 task 21. The measured simulation rate, and the ticks the catch-up clamp refused. Both
// on one line because the second number is only ever interesting next to the first: "20.0 TPS"
// with a rising drop count means the clock is right and the console is not.
static const char* bsDbgInfoTick(char* buf, int cap, void* ctx)
{
	const DebugContext* d = (const DebugContext*)ctx;
	snprintf(buf, (size_t)cap, "%.1f tps %lu drop", (double)d->tps,
	         (unsigned long)d->ticks_dropped);
	return buf;
}

static const char* bsDbgInfoWorld(char* buf, int cap, void* ctx)
{
	const DebugContext* d = (const DebugContext*)ctx;
	snprintf(buf, (size_t)cap, "%d cols %d chunks", d->columns, d->chunks);
	return buf;
}

static const char* bsDbgInfoMem(char* buf, int cap, void* ctx)
{
	const DebugContext* d = (const DebugContext*)ctx;
	snprintf(buf, (size_t)cap, "%lu / %lu KB",
	         (unsigned long)(d->blocks_bytes / 1024u),
	         (unsigned long)(d->blocks_budget / 1024u));
	return buf;
}

static const char* bsDbgInfoPlayer(char* buf, int cap, void* ctx)
{
	const DebugContext* d = (const DebugContext*)ctx;
	snprintf(buf, (size_t)cap, "%.1f %.1f %.1f", d->player_x, d->player_y, d->player_z);
	return buf;
}

// v1.8.16 FRZ-FIX. The save-slot wait, in the one place a shipped CIA can show a number.
//
// workerSaveWaits() has shipped since v1.7.1 and has never once been readable by anybody: its
// only readout is a printf inside #if !BS_BOTTOM_UI, and BS_BOTTOM_UI defaults to 1, so the
// installed CIA has no text console and that line is not even compiled. This row is the fix for
// that, and it is the readout for the freeze work: sv/w/max say whether the main thread ever
// waited for a save slot at all, and sync/over say whether the v1.8.16 deadline fired and
// whether the card was locked when it did.
//
// No ctx: these are four calls into app/worker.c, which keeps the counters itself. Every one of
// them is safe before workerStart — they read plain statics.
static const char* bsDbgInfoSaves(char* buf, int cap, void* ctx)
{
	(void)ctx;
	snprintf(buf, (size_t)cap, "sv%d w%d max%.0fms sync%d over%d",
	         workerSaveSubmits(), workerSaveWaits(), (double)workerSaveWaitMaxMs(),
	         workerSaveSync(), workerSaveOverrun());
	return buf;
}

// v1.8.8 NEON BIOME BORDERS. Thin wrappers because DebugEntry's TOGGLE callbacks take a void*
// ctx and debug/biomeborder.h's do not — the flag has no per-entry state to carry, so rather
// than widen the module's API to fit the menu, the menu's shape is absorbed here. (void)ctx
// and not an unnamed parameter: -Werror=unused-parameter is on.
static bool bsDbgGetBiomeBorders(void* ctx)          { (void)ctx; return biomeBorderEnabled(); }
static void bsDbgSetBiomeBorders(void* ctx, bool on) { (void)ctx; biomeBorderSetEnabled(on); }

// v1.8.11 METRICS-VISIBLE. Same two-wrapper shape as the biome-border pair above, and for the
// same reason: the flag is a file static in debug/metrics.c with two accessors, so there is no
// per-entry state to carry and no ctx to keep in step.
static bool bsDbgGetFrameTiming(void* ctx)          { (void)ctx; return metricsRowEnabled(); }
static void bsDbgSetFrameTiming(void* ctx, bool on) { (void)ctx; metricsRowSetEnabled(on); }

// The menu's rows, registered once at boot. Weather and Dimension are the spec'd systems
// that do not exist yet: registered unavailable so they show greyed and the menu visibly
// reserves their place, and a later version flips available=true instead of redesigning.
static void bsDebugRegister(void)
{
	DebugEntry* e;

	e = debugMenuRegister();
	if (e) {
		e->name       = "Render distance";
		e->kind       = DEBUG_SLIDER_INT;
		e->available  = true;
		e->getInt     = bsDbgGetDist;
		e->setInt     = bsDbgSetDist;
		e->slider_min = RENDER_DIST_MIN;
		e->slider_max = renderDistMaxFor(hwIsNew3ds());   // v1.8.5: per-console, not compile-time
		e->ctx        = &s_dctx;
	}

	// v1.8.8 NEON BIOME BORDERS. A DEBUG_TOGGLE and nothing else: this is the ONLY place in
	// the tree that can switch the overlay on. There is no options.c key for it, so it does
	// not persist across a boot; there is no hotkey and no bottom-screen control; and the row
	// itself is only reachable through pause -> Options -> Debug. That is the whole of "make
	// sure this is only a debug option, not a regular game feature".
	//
	// No ctx: the flag is a file static in debug/biomeborder.c and these two functions are its
	// only accessors, so there is nothing per-entry to carry. The other rows pass &s_dctx
	// because they read live per-frame numbers out of it; this one has no such state, and an
	// unused ctx pointer would be a thing to keep in step for no reason.
	//
	// Registered ABOVE Weather rather than appended, so the two unavailable placeholders stay
	// at the bottom of the working rows instead of a real toggle appearing under two greyed
	// ones. app/debugmenu_test.c builds its own entries and pins no index from this function,
	// so the order here is a readability choice and not a contract — checked, not assumed.
	e = debugMenuRegister();
	if (e) {
		e->name      = "Biome borders";
		e->kind      = DEBUG_TOGGLE;
		e->available = true;
		e->getBool   = bsDbgGetBiomeBorders;
		e->setBool   = bsDbgSetBiomeBorders;
	}

	// v1.8.11 METRICS-VISIBLE. Switches the bottom-screen HUD's timing row on. A DEBUG_TOGGLE
	// and nothing else, registered beside Biome borders because it is the same kind of thing:
	// reachable only through pause -> Options -> Debug, no options.ini key, no hotkey, and off
	// again after a reboot. That is the whole of "a debug option, not a regular game feature".
	//
	// The ROW it turns on is on the HUD and not in this menu on purpose — see the block comment
	// at the row itself in scene/ui.c. This menu's own "Frame" INFO row further down is a
	// PAUSED reading and always was; it is left exactly as it is rather than quietly repaired,
	// because changing what an existing readout means is not this task's to decide.
	e = debugMenuRegister();
	if (e) {
		e->name      = "Frame timing";
		e->kind      = DEBUG_TOGGLE;
		e->available = true;
		e->getBool   = bsDbgGetFrameTiming;
		e->setBool   = bsDbgSetFrameTiming;
	}

	// A "Minimap" toggle used to sit here, between Render distance and Weather. It went
	// with the feature in v1.5.1, so every entry below it moved up one index — the debug
	// menu builds its rows from this registration order, so this list IS the row order.
	e = debugMenuRegister();
	if (e) {
		e->name      = "Weather";
		e->kind      = DEBUG_TOGGLE;
		e->available = false;
	}

	e = debugMenuRegister();
	if (e) {
		e->name      = "Dimension";
		e->kind      = DEBUG_ACTION;
		e->available = false;
	}

	static const struct {
		const char* name;
		const char* (*info)(char*, int, void*);
	} infos[] = {
		{ "Frame",   bsDbgInfoFrame  },
		{ "Draws",   bsDbgInfoDraw   },
		{ "Culled",  bsDbgInfoCull   },
		{ "Tick",    bsDbgInfoTick   },
		{ "World",   bsDbgInfoWorld  },
		{ "Memory",  bsDbgInfoMem    },
		{ "Player",  bsDbgInfoPlayer },
		// v1.8.16 FRZ-FIX. The save-slot wait, in the one place a shipped CIA can show a
		// number: this menu. See bsDbgInfoSaves above for why it has never been visible.
		{ "Saves",   bsDbgInfoSaves  },
	};
	for (int i = 0; i < (int)(sizeof(infos) / sizeof(infos[0])); i++) {
		e = debugMenuRegister();
		if (!e) break;
		e->name      = infos[i].name;
		e->kind      = DEBUG_INFO;
		e->available = true;
		e->info      = infos[i].info;
		e->ctx       = &s_dctx;
	}
}
#endif   // BS_BOTTOM_UI

// And back into the streaming machinery.
#if BS_WORLD_GEN

// Starts the worker and queues the whole area. The spawn column goes first because the
// ring is FIFO, so it is the first one back and the player has ground under them before
// the loop starts.
static bool genStart(int32_t cx, int32_t cz)
{
	// v1.8.3. Cleared on the way IN, before anything can set it, rather than by whoever reads it.
	// A refusal is a fact about THIS attempt at THIS world; one left standing from the previous
	// attempt would be put on the menu after a world that opened perfectly well.
	s_world_refused_why = NULL;

	// Which world this is, decided here and nowhere else. In a session the server owns the
	// seed and sends it as BS_APP_WORLD_INFO right after JOIN (net/networld.h): the terrain is
	// never transmitted, so generating from anything other than the server's seed would put
	// this player in their own private landscape and make every other player's block edit land
	// in the wrong hillside. networldWorldSeed() leaves `seed` untouched when it has nothing to
	// say, so a session against a server too old to send one keeps the value below.
	//
	// v1.8.3 Phase 1: BS_WORLD_SEED is no longer the answer for single player, only the starting
	// point. The per-world seed is resolved off the card further down, after the world directory
	// exists — see the worldSeedResolve block below world/genversion's refusal.
	uint32_t seed = BS_WORLD_SEED;
	(void)networldWorldSeed(&seed);
	s_seed_used = seed;

	// v1.7.0. The save directory is resolved HERE, above worldgenInit, rather than at its old
	// site thirty lines down, because which generator this world uses is read off the card and
	// a WorldGen cannot be built without it. saveWorldDir() is called exactly once per session
	// either way — it mkdirs three levels and write-probes the card, which is most of
	// genStart's measured 117 ms, so calling it twice would be a real cost and not a tidiness
	// question.
	//
	// NULL in a server session, which app/worker.h documents as "no save file at all: every
	// column is generated and none is ever written". That is exactly the contract a joined
	// world wants — terrain comes from the server's seed and edits come from its diff store,
	// so a local copy could only ever be a second, staler answer to a question the server has
	// already answered. saveWorldDir() is not called at all rather than called and ignored,
	// because calling it is what creates the directory.
	const char* const world_dir = s_server_session ? NULL : saveWorldDir();

	// v1.7.1 task 48b. Opened AFTER saveWorldDir rather than at the top of the function, so the
	// two do not nest: saveWorldDir times itself into LOAD_STAGE_WORLDDIR, and a setup bracket
	// wrapped round it would charge the same milliseconds to two stages and make the per-stage
	// figures stop adding up to the wall time. The handful of microseconds above this line
	// (networldWorldSeed) fall into `other` instead, which is the honest place for them.
	const uint64_t t_setup = loadprofMark();

	// v1.7.0 task 15 prerequisite. Which terrain generator this world belongs to — see
	// world/genversion.h for the whole rule. A world with no stamp is one that predates
	// versioning and keeps the legacy generator; a brand-new directory is stamped with this
	// build's newest; a server session has no directory and takes genVersionForSession().
	uint32_t gen_version = GEN_VERSION_LEGACY;
	const GenVersionStatus gv = genVersionResolve(world_dir, &gen_version);

	// A world this build cannot generate must NOT be entered. Refusing is the whole point:
	// generating a stamped world at the wrong version writes the wrong landscape under the
	// player's buildings, and there is no way to tell that apart afterwards from the world
	// corruption it looks exactly like.
	//
	// v1.8.3. This line used to be an `if` naming TOO_NEW and DAMAGED by hand, and that shape is
	// what let GENVER_STAMP_FAILED ship doing nothing: 82d4a1e added the status with a red/green
	// test behind it and no expression anywhere in this file mentioned it, so a world whose stamp
	// the card refused fell past the test and loaded. world/genrefuse.h is the same question
	// asked as a switch over the whole enum, so the next status added cannot repeat it — it stops
	// the build instead. See that file for why it is not in genversion.c.
	//
	// The message is stashed rather than only printed. printf goes to a console nobody is looking
	// at on a retail 3DS; the player gets s_world_refused_why on the menu, below.
	const char* const refusal = genVersionRefusalText(gv);
	if (refusal) {
		printf("world refused: generator %lu (%s)\n", (unsigned long)gen_version, refusal);
		s_world_refused_why = refusal;
		loadprofSince(LOAD_STAGE_SETUP, t_setup);
		return false;
	}

	// v1.8.3 Phase 4. A joined session has no world directory, so genVersionResolve() above
	// returned GENVER_NO_WORLD_DIR and left gen_version at genVersionForSession() — which is
	// the right answer only for a server that has not said otherwise. Now it can say: the
	// server declares its world's generator as BS_APP_WORLD_GEN (proto/bs_proto.h) and
	// networldServerGenVersion() holds what it said.
	//
	// Deliberately a second resolve with its own refusal rather than a branch folded into the
	// one above, exactly as the seed block below is: the two questions are asked of two
	// different things — one of a directory on this card, one of a server on the network — and
	// merging them would produce one status whose sentence could not be right for both.
	//
	// The decision is entirely this client's. The server announces and never verifies (see
	// bsgame.c's send_registry_info for the same posture stated at length), and there is no
	// acknowledgement to send: refusing means not joining that world, and nothing about that is
	// the server's business.
	if (s_server_session) {
		uint32_t wire_gen = 0u;
		const bool have_wire = networldServerGenVersion(&wire_gen);
		const GenVersionStatus sgv =
			genVersionForSessionResolve(have_wire, wire_gen, &gen_version);

		// Same switch, same file, same reason as the refusal above — genrefuse.h answers over
		// the whole enum with no default, so GENVER_SESSION_MISMATCH could not be added
		// without a decision being made about it here.
		const char* const sess_refusal = genVersionRefusalText(sgv);
		if (sess_refusal) {
			printf("session refused: server generator %lu (%s)\n",
			       (unsigned long)wire_gen, sess_refusal);
			s_world_refused_why = sess_refusal;
			loadprofSince(LOAD_STAGE_SETUP, t_setup);
			return false;
		}
	}

	// v1.8.3 Phase 1. WHICH world this is, as opposed to which generator shapes it. The whole
	// rule is in world/worldseed.h; the two lines that matter here are that a single-player
	// world's seed now comes off the card instead of out of BS_WORLD_SEED, and that a joined
	// session's seed still comes off the wire and is not touched.
	//
	// Deliberately AFTER genVersionResolve and its refusal rather than merged into it. Both are
	// questions about the same directory and either can refuse, but the generator is the one
	// that decides whether this build can open the world at all — a world stamped by a newer
	// build must say so, not report a seed problem it also happens to have. Two resolves in a
	// row, each with its own refusal, keeps the answers in the order the player needs them.
	//
	// The mint is done here rather than inside worldSeedResolve() so that the resolve stays a
	// pure decision over the filesystem with no clock in it — that is what lets the host suite
	// drive every branch of it, refusals included (world/worldseed.h, worldSeedMintFrom).
	// worldSeedMint() answering false is a console with no usable clock at all, and it is passed
	// through as a fact rather than papered over: only a brand-new world needs it, so an old
	// world on a console with a dead RTC still opens.
	uint32_t mint = 0u;
	const bool mint_ok = worldSeedMint(&mint);

	// NOT `&seed`. worldSeedResolve() always writes through this pointer, including
	// WORLD_SEED_LEGACY on the WSEED_NO_WORLD_DIR return, and that return is exactly the joined
	// session — whose seed is already in `seed`, came from the server, and is the one thing in
	// this function that must not be overwritten with 1337.
	uint32_t world_seed = WORLD_SEED_LEGACY;
	const WorldSeedStatus ws = worldSeedResolve(world_dir, mint_ok, mint, &world_seed);

	// Same shape as the generator refusal above and for the same reason — world/genrefuse.h
	// answers this over the whole enum so a status added later cannot fall through unrouted.
	// A refusal here is a world that must not be entered: generating from a seed that is not
	// the one that shaped this world rewrites the landscape under the player's buildings, which
	// is the identical harm the version stamp refuses for.
	const char* const seed_refusal = worldSeedRefusalText(ws);
	if (seed_refusal) {
		printf("world refused: seed (%s)\n", seed_refusal);
		s_world_refused_why = seed_refusal;
		loadprofSince(LOAD_STAGE_SETUP, t_setup);
		return false;
	}

	// Adopted only on WSEED_OK, which worldSeedResolve only ever returns when there was a world
	// directory to resolve against. WSEED_NO_WORLD_DIR is the session, and falls through here
	// leaving the server's seed exactly as networldWorldSeed() left it.
	if (ws == WSEED_OK) {
		seed = world_seed;
		s_seed_used = seed;   // the gen overlay's readout, kept honest — see s_seed_used above
	}

	// v1.8.9 day/night. Reloads the world's time.bin sidecar; dayNightRead always writes
	// day_ticks (DAY_START_TICKS on a fresh world, a missing file, or no world_dir at all —
	// e.g. a server session), so this is safe unconditionally, unlike the seed/version reads
	// above which are gated on WSEED_OK.
	uint64_t day_ticks = DAY_START_TICKS;
	(void)dayNightRead(world_dir, &day_ticks);
	dayNightInit(&s_daynight, day_ticks);

	// v1.8.9 weather rendering. Reset here, alongside the day/night clock it shares wx_tick
	// with (see the per-tick weather loop below), and not at process boot: weatherDrawUpdate
	// needs a real world's camera position to snap its grid against, and a state left over
	// from a previous world would show that world's scroll/drift phase and grid snap in the
	// first frame of this one. The GPU resources it draws through are process-level and were
	// already claimed by weatherDrawInit() at boot -- only the animation state resets here.
	weatherDrawStateInit(&s_weatherdraw);

	if (!worldgenInit(&s_gen, seed, gen_version)) {
		// Unreachable while genVersionResolve only ever returns OK for a known version — this
		// is the same predicate asked twice, on purpose, at the one other place a WorldGen can
		// come into existence.
		printf("world refused: generator %lu unknown\n", (unsigned long)gen_version);
		s_world_refused_why = "world could not be started";
		loadprofSince(LOAD_STAGE_SETUP, t_setup);
		return false;
	}

	// v1.8.8 biome tint. Handed over HERE, immediately after the one call that makes a
	// WorldGen valid, rather than beside chunkRenderSetWater further down: s_gen is a static
	// that never moves, but a renderer holding a generator that worldgenInit had REFUSED would
	// tint the world from uninitialised salts. The refusal path above returns before this line.
	chunkRenderSetGen(&s_gen);

	// v1.8.8 debug biome row. Same place, same reason, and a POINTER on purpose: worldgenInit
	// stores rngMix(seed ^ 'BLKS'), not the seed it was given, so anything that re-derived a
	// generator from an exposed seed would classify the player against a different noise field
	// and print a valid-looking WRONG biome name. Nothing here exposes a seed accessor.
	debugBiomeSetWorldGen(&s_gen);

	// v1.8.8 NEON BIOME BORDERS, the debug overlay. Same place, same pointer, and the same
	// trap avoided for the third time — see debug/biomeborder.h. The cached fence is dropped
	// as well: it was built around the PREVIOUS world's noise field, and a fence left standing
	// across a re-generation would be a seam drawn where this world has none.
	//
	// This hands the world over; it does NOT switch the overlay on. The toggle is .bss and
	// stays false until someone presses A on its row in the debug menu.
	biomeBorderSetWorldGen(&s_gen);
	biomeBorderDrawInvalidate();

	jobqInit(&s_meshq);
	memset(s_col_queued, 0, sizeof(s_col_queued));
	memset(s_col_in, 0, sizeof(s_col_in));
	memset(s_col_asked, 0, sizeof(s_col_asked));
	// With the queue and the queued-column window both cleared, there is nothing outstanding
	// to retry. Left set from a previous world it would only cost one wasted rescan, but a
	// flag that survives the thing it describes is how the queued-before-pushed bug happened
	// in the first place.
	s_mesh_queue_short = false;
	// v1.8.10. GEN_AREA_SPAN is the same compile-time constant s_col_in/s_col_asked are sized
	// for, and the _Static_assert next to GEN_RETRY_SLOTS above guarantees it fits, so this
	// cannot fail for any configuration that compiled — same reasoning the JOBQ_CAP asserts
	// rely on elsewhere in this file instead of a runtime check here.
	genRetryInit(&s_gen_retry, GEN_AREA_SPAN);
	// v1.8.11. Keyed on the mesh ring, which is the ring s_col_queued is indexed by — using
	// GEN_AREA_SPAN here would wrap mesh columns onto the wrong slots.
	genRetryInit(&s_mesh_retry, GEN_MESH_SPAN);
	s_genr = (GenResult){0};
	s_genr.ran = true;
	s_center_cx = cx;
	s_center_cz = cz;
	s_col_unloaded = s_col_dropped = 0;

	// `world_dir` is resolved at the top of this function now (v1.7.0) — the generator version
	// is read off the card and worldgenInit needs it. It is still established before
	// workerStart, which is the ordering constraint that mattered here: the worker copies the
	// directory on its very first job, so a directory set afterwards would miss the spawn
	// column.

	// v1.6.0 Phase A: single-player registry sidecar, applied before any column can be
	// decoded — the worker that reads region files does not exist until workerStart() below.
	// A false return means "no file yet" (a fresh world whose creation write failed) or an
	// unreadable one; either way the table stays core-only, which is exactly what an empty
	// sidecar would have said. In a session the dynamic rows arrive over the wire instead
	// (REGISTRY_DEFS at join), so there is nothing to load here by design.
	if (world_dir) {
		char reg_path[96];
		snprintf(reg_path, sizeof reg_path, "%s/registry.bin", world_dir);
		(void)registrySidecarLoad(reg_path);
	}

	// v1.6.0 Phase A: the table is complete by now — core rows at boot, dynamic rows from
	// the sidecar or off the wire during join — so it freezes before the worker exists.
	// Post-freeze every consumer reads it without locks; workerSubmitColumn() refuses to
	// run against an unfrozen table.
	//
	// v1.6.0 task 8: "complete by now" was a claim, not a fact, and on the join path it was
	// false. genStart() runs one frame after scene/title.c sees the server's seed, and the
	// server's BS_APP_REGISTRY_DEFS are a round trip behind that seed — so this line always
	// slammed shut before the first dynamic row could be committed, and registryRemoteApply()
	// refused every batch that followed for the whole session. The fix is the second gate at
	// the bottom of drawMultiplayer() (scene/title.c): world entry is now held open until
	// networldRegistryWaiting() says the registry question is settled or bounded out, so by
	// the time control reaches this line the DEFS have either landed or provably are not
	// coming. Nothing moved here, because the worker's no-locks-after-freeze contract
	// (world/registry.h) needs the freeze exactly where it is — before workerStart() below.
	registryFreeze();

	workerSetWorldDir(world_dir);

	if (!workerStart(&s_gen)) {
		// The third and last way out of this function, and it gets a reason for the same reason
		// the two above do: `false` from here means the same thing to the caller — there is no
		// world to walk into — so the player must be told something rather than watching a bar
		// that can never fill. Nothing more specific than this is honest; app/worker.h's failure
		// is "the thread would not start", which is not a fault the player can act on.
		s_world_refused_why = "world could not be started";
		loadprofSince(LOAD_STAGE_SETUP, t_setup);
		return false;
	}

	if (workerSubmitColumn(cx, cz)) genSlotSet(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);
	else                            s_genr.submit_failed++;
	genRequestArea();

	loadprofSince(LOAD_STAGE_SETUP, t_setup);
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
		// v1.8.10. If this was a retry landing after the ring had already moved past it,
		// s_gen_retry must hear about it here — genRetryTick's own out-of-radius drop (see
		// world/genretry.h) never runs for an INFLIGHT entry, which this one is the moment it
		// was submitted, so nothing else would ever clear it. genretry.h documents exactly this
		// case: "the caller no longer wants it (the ring moved past it)". A no-op if this
		// column was never a retry.
		genRetryClear(&s_gen_retry, cx, cz);
		return true;
	}

	// Marked before the ring is tested, so the column that completes a ring counts towards
	// it. A partial column (ok == false) is still marked: it is a hole either way, it is
	// already counted in columns_failed, and leaving it unmarked would stall its four
	// neighbours forever instead of meshing what did arrive.
	genSlotSet(&s_col_in[0][0], GEN_AREA_SPAN, cx, cz);
	genSlotClear(&s_col_asked[0][0], GEN_AREA_SPAN, cx, cz);

	// v1.8.10. This is the actual fix (world/genretry.h): ok == false no longer leaves a
	// silent, permanent hole. s_col_in above still marks the column installed — that half is
	// unchanged, and still has to stay unconditional or a partial column would stall its four
	// neighbours' ring-complete check forever — but now s_gen_retry also remembers it, and
	// genRetryTick (called once per frame, below and in the loading loop) resubmits it with
	// backoff once budget allows. ok == true clears any pending retry for this column, which is
	// a documented no-op if it was never one.
	if (ok) genRetryClear(&s_gen_retry, cx, cz);
	else    genRetryMark(&s_gen_retry, cx, cz);

	// This column is now genuinely part of the live world, every chunk workerInstall() had
	// already copied in above — the moment net/blockdiff.h's pending diffs for it (a remote
	// edit that arrived while it was still streaming in) can land. Before genQueueReadyColumns
	// so anything a drained diff touches gets swept into the same meshing pass as the rest of
	// this column instead of waiting a frame.
	networldOnColumnLoad(&s_world, cx, cz);

	// v1.8.14 animals. THE main-thread "this column is now genuinely part of the live world"
	// moment, which is the whole reason the spawner is called from here.
	//
	// NOT from worldColumnCreate(), which docs/plan-1.8.14-animals.md P2 names and is wrong
	// about: that runs on the terrain worker thread (app/worker.c's workerMain) against a
	// STAGING world, and nothing in entity/entity.c is written to survive an EntityWorld
	// mutated off the main thread. entity/animal.h says the same in its THREADING note.
	//
	// Verified rather than assumed, because "it reads like a main-loop site" is not evidence:
	// genInstallOne() is a file static, so main.c is the only file that can call it, and it
	// has exactly three callers -- runLoadingScreen() (which drives aptMainLoop() itself),
	// saveCheck(), and the frame loop. All three are reached only from main(), and main.c
	// creates no thread of its own: every threadCreate() in this tree is in app/worker.c,
	// app/watchdog.c or app/updater.c, and none of them can reach a static in this file.
	//
	// AFTER networldOnColumnLoad above, so a herd is never rolled against terrain that a
	// pending remote edit is about to change underneath it, and before genQueueReadyColumns
	// below only to keep the meshing pass last.
	//
	// The return is dropped on purpose. 0 is the ORDINARY outcome -- about eleven columns in
	// twelve roll no herd at all and a desert never rolls one (entity/animal.h) -- so it is
	// not a failure and there is no counter here for it to feed.
	(void)animalSpawnForColumn(&s_entities, &s_world, &s_gen, cx, cz, &s_animal_rng);

	// And now ask the server for whatever it already holds for this column. Here rather than
	// where the column was requested, because a subscription is a promise that the diffs can
	// be applied on arrival, and until the blocks are in the world they cannot be. After the
	// stale-ring check above for the same reason it exists: a column that arrived outside the
	// ring is thrown away on the next line but one, and subscribing to it would leave the
	// server broadcasting edits for terrain this console no longer has.
	networldSubscribeColumn(cx, cz);

	// v1.7.1 task 48b. Its own stage because it is the one piece of per-column main-thread work
	// that is O(ring) rather than O(column): it rescans the mesh window every time a column
	// lands, so its total grows with the square of the render distance while everything else on
	// this path grows linearly with the column count.
	const uint64_t t_queue = loadprofMark();
	genQueueReadyColumns();
	loadprofSince(LOAD_STAGE_QUEUE, t_queue);
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
	if (jobqCount(&s_meshq) == 0) {
		// v1.6.0 task 12. Nothing left to drain — so if a column went short of queue space
		// earlier, now is exactly when to ask again, and this is the one place called every
		// frame by both the loading loop and the play loop. Gated on the flag AND on an empty
		// queue so the whole-ring rescan happens once per shortage rather than every frame:
		// in the normal case (nothing short) this costs one bool test.
		if (s_mesh_queue_short) genQueueReadyColumns();
		if (jobqCount(&s_meshq) == 0)
			return 0;
	}

	const u64 t0 = svcGetSystemTick();
	int built = 0;
	Job j;
	while (jobqPop(&s_meshq, &j)) {
		// v1.7.1 task 48b. Per chunk, not per drain call, so mesh_n is the chunk count the
		// loading screen actually built and mesh/us-per-call is directly comparable with the
		// DRAIN_BUDGET_MS the drain is trying to stay inside.
		const uint64_t t_mesh = loadprofMark();
		if (chunkRenderBuild(&s_world, j.cx, j.cy, j.cz)) {
			s_genr.meshed++;
		} else {
			s_genr.mesh_refused++;

			// v1.8.11. THE FIX. Until now this line was the whole of the failure handling,
			// and the job had already been popped — so a refused chunk was simply forgotten,
			// with s_col_queued still set against its column so genQueueReadyColumns would
			// never offer it again. That is the permanent hole. See s_mesh_retry's comment.
			//
			// Only the pool class is retried. An overflowing chunk meshed again overflows
			// again by definition, so putting it in the ledger would resubmit it every
			// backoff period forever, burning drain budget that the chunks which CAN be
			// meshed are queued behind — turning a one-chunk hole into a world-wide stall.
			// It is counted and surfaced instead, because a nonzero overflow count is a
			// capacity fact that wants fixing at the tier sizing, not at the retry.
			switch (chunkRenderLastRefusal()) {
			case CHUNK_REFUSE_POOL:
				s_genr.mesh_refused_pool++;
				genRetryMark(&s_mesh_retry, j.cx, j.cz);
				break;
			case CHUNK_REFUSE_OVERFLOW:
				s_genr.mesh_refused_overflow++;
				break;
			case CHUNK_REFUSE_NONE:
				// chunkRenderBuild returned false without naming a reason. Not reachable
				// today — every `return false` in it sets one — and deliberately not
				// silently swallowed: treat it as the recoverable class, because retrying
				// something that did not need it costs one remesh, while dropping
				// something that did need it costs a permanent hole.
				s_genr.mesh_refused_pool++;
				genRetryMark(&s_mesh_retry, j.cx, j.cz);
				break;
			}
		}
		loadprofSince(LOAD_STAGE_MESH, t_mesh);
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
//
// v1.8.17: "6 chunks" in the first sentence is now the OLD 3DS figure. The count half rides
// drainMaxChunks(), so it is 6 on an Old 3DS and 18 on a New one, for the reason that function's
// comment gives; the time half stays 2 x 4.0 ms on both. Nothing else here changes: the loading
// screen's whole point is that this is the one place with the frame to itself, so the console
// that can build more chunks inside 8 ms should build them.
#define LOADING_DRAIN_BUDGET_MS  (2.0f * DRAIN_BUDGET_MS)
#define LOADING_DRAIN_MAX_CHUNKS (2 * drainMaxChunks())

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
		BS_STAGE_ONCE(BS_STAGE_ID_INPUT_POLL, "INPUT_POLL_FIRST");
		const u32 down = hidKeysDown();

		// Same two-signal touch read the title screen and the HUD use, for the same reason —
		// hid.h flags KEY_TOUCH as "Not actually provided by HID".
		touchPosition tp = {0};
		hidTouchRead(&tp);
		const bool touch_down = (hidKeysHeld() & KEY_TOUCH) != 0 || tp.px != 0 || tp.py != 0;
		const bool tap = touch_down && !touch_prev;
		touch_prev = touch_down;

		// Feed the world. Each generator lane stages one column at a time and waits for it to be
		// taken, so in practice this installs one per frame per lane — one on an Old 3DS, two on
		// a New 3DS running the v1.8.8 second lane; the bound of 8 is for the case genInstallOne
		// returns true without consuming a staged column (a column that arrived for a ring
		// position already left behind), which must not become an unbounded loop in here of all
		// places. Left at 8 rather than tied to workerLanes() precisely because that case is
		// what it is for, and it is not bounded by the lane count.
		watchdogPhase(WD_PHASE_LOAD_GEN);
		for (int i = 0; i < 8 && genInstallOne(); i++) { }
		// v1.8.10. Once per frame, same as the play loop below — ages every outstanding hole by
		// one frame and resubmits whichever ones have backed off long enough. Bounded the same
		// way genInstallOne's own drain is: at most one resubmission per still-pending column per
		// call, never a loop that can spin.
		genRetryTick(&s_gen_retry, s_center_cx, s_center_cz, s_area_radius, genRetrySubmit, NULL);
		watchdogPhase(WD_PHASE_LOAD_MESH);
		// v1.8.11. Before the drain, not after: a column re-pushed by this tick should get its
		// chance in the very same frame's drain rather than waiting a frame for the next one.
		genRetryTick(&s_mesh_retry, s_center_cx, s_center_cz, s_mesh_radius, meshRetrySubmit,
		             &s_mesh_retry);
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
		// v1.7.1 task 48b. This bracket is the VBlank wait as much as it is the drawing:
		// C3D_FrameBegin(C3D_FRAME_SYNCDRAW) blocks until the previous frame's GPU work is
		// done, and on a screen that draws one flat panel that is almost all of it. It is timed
		// anyway — in fact especially — because a load whose `present` total is roughly
		// frames * 16.71 ms is a load bound by the number of frames it takes, and no stage
		// below it can be optimised into a faster world entry until that changes.
		const uint64_t t_present = loadprofMark();
		loadprofFrame();
		if (bottom) {
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			BS_STAGE_ONCE(BS_STAGE_ID_FRAME_BEGIN, "FRAME_BEGIN_FIRST");
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
			BS_STAGE_ONCE(BS_STAGE_ID_FRAME_END, "FRAME_END_FIRST");
		} else {
			gspWaitForVBlank();
		}
		loadprofSince(LOAD_STAGE_PRESENT, t_present);

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
	// v1.7.1 task 49. Timed, not just called. genRecenter is the one piece of streaming work
	// that fires on a *position* rather than on a clock — it does nothing at all on 220 frames
	// out of 221 and then unloads a row of columns, requests a new one, and queues every
	// newly-ready chunk, all on the main thread with no budget. That is precisely the shape of
	// "my FPS drops in certain spots", so the CSV has to be able to say whether the slow frames
	// are the recentre frames. Two tick reads on a frame that does nothing.
	const u64 t0 = svcGetSystemTick();
	genRecenter(genColumnOf(x), genColumnOf(z));
	s_frame_recenter_ms +=
		(float)((double)(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
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

// Out of the streaming block again, and for the same reason as the debug-menu state above: the
// boot timer and the draw probe are instrumentation for the whole program, and every one of
// their call sites — BOOT_BEGIN/BOOT_END around the boot steps, probeBegin/probeFrame in the
// game loop, bsProbeArm() from scene/chunk_render.c — is shared code that both worlds compile.
// Both already have a correct #if/#else with an off arm defined; nesting them in here meant a
// BS_WORLD_GEN=0 build got neither arm. The save-check probe below stays inside, because that
// one really is generated-world only — it unloads and reloads a streamed column.
#endif   // BS_WORLD_GEN

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
	// n < 0 is an encoding error and adds nothing. Otherwise snprintf reports how many
	// characters it *would* have written, which can exceed the space it actually had — clamp
	// to what it really placed before the NUL it always terminates with, or bootTimingWrite's
	// fwrite(s_boot_log, 1, s_boot_len, f) reads past the end of the array and puts whatever
	// follows it in .bss into boot_timing.txt. Measured at 5 BOOT_END call sites this cannot
	// fire; measured at 22 it does, at 792 against a 768-byte buffer. Adding boot stages is an
	// ordinary thing to do and nothing would have warned. Same shape as app/crash.c's
	// dumpAppend, which is the other saturating string builder in this program.
	if (n <= 0) return;
	const size_t room = sizeof(s_boot_log) - s_boot_len - 1;
	s_boot_len += ((size_t)n < room) ? (size_t)n : room;
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
// It is now version-stamped as well (see probeBegin), which is what makes turning it back on
// safe. v1.2.3 turns it on deliberately: v1.2.2's hardware report showed the freeze happens on
// the FIRST world frame — `frame captures: 1`, `columns in: 49`, the frame's own
// ProcessCommandList submitted and never completed — so the failure is deterministic and
// immediate, and stepping one arm per boot costs seconds rather than a play session.
//
// The breadcrumb alone cannot finish the job: it names the draw stage the main thread was in,
// but the main thread is parked inside C3D_FrameEnd waiting on a GPU that stopped, so the
// breadcrumb says "FrameEnd" no matter which of the draws inside the list is the poison. Only
// removing them one at a time can say which.
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

	// Version stamp, and the reason this whole mechanism can be trusted again.
	//
	// v1.1.2's bisect read v1.1.1's leftover drawprobe.txt, concluded from it that every arm had
	// already had its turn, and parked on the arm that draws nothing. steve installed it, launched
	// a world and saw only block outlines — a build meant to freeze came back alive and was
	// reported as passing when it had never been tested at all.
	//
	// Discarding a foreign file is not merely tidiness here: the arms are RENUMBERED between
	// rounds. Round 1's arm 2 was "no highlight cage"; round 2's arm 2 is "cull only, not one GPU
	// command". A line saying "arm 2" from an older build is not a fact about this build, so it is
	// deleted rather than interpreted. Anything not written by exactly this version goes.
	{
		char stamp[80];
		snprintf(stamp, sizeof stamp, "# blocksmith %s round 2 bisect\n",
		         BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "(unset)");

		bool ours = false;
		FILE* v = fopen(PROBE_FILE, "r");
		if (v) {
			char first[80];
			ours = (fgets(first, sizeof first, v) != NULL) && strcmp(first, stamp) == 0;
			fclose(v);
		}
		if (!ours) {
			remove(PROBE_FILE);
			FILE* n = fopen(PROBE_FILE, "w");
			if (n) { fputs(stamp, n); fclose(n); }
		}
	}

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

// And back into the streaming machinery for the save-check probe, which needs it.
#if BS_WORLD_GEN

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
                    int crack_stage, float iod, int eye)
{
	drawStage(WD_DRAW_EYE_SETUP, eye);
	(void)eye;   // the markers are the only reader; a non-probe build has none
	chunkRenderSetEye(iod);
	// v1.8.9 day/night. Only this clear -- the one the player actually walks around in --
	// tracks the world clock. The loading-screen and title-screen clears elsewhere in this
	// file keep the literal CLEAR_COLOR/SKY_CLEAR_RGBA8 constant on purpose: neither screen
	// has a world clock to follow.
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL,
	                      dayNightSkyClearRgba8(dayNightTimeOfDay(&s_daynight), SKY_CLEAR_RGBA8), 0);
	C3D_FrameDrawOn(target);
	// Round 1's cuts were here. They are gone because round 1 is answered: on real hardware
	// every arm froze except the one that skipped this call, so the bisect has moved inside
	// chunkRenderDraw and this file draws the whole frame again in every arm.
	chunkRenderDraw(view);
	for (int i = 0; i < BS_GPU_STRESS; i++) chunkRenderDraw(view);

	// v1.8.9 weather rendering. Immediately after the terrain (and its BS_GPU_STRESS
	// repeats), not before: weatherDrawDraw blends against whatever is already in the colour
	// buffer, and an opaque chunkRenderDraw running after it would overwrite the blended
	// rain/snow rather than composite with it. Passed chunkRenderProjection() (this eye's
	// matrix, already skewed by chunkRenderSetEye(iod) above) so stereo separation matches
	// the terrain exactly -- see docs/plan-1.8.9-weather-integration.md §4. Restores its own
	// GPU state (depth test, alpha blend, cull face) back to chunk_render.c's own defaults
	// before returning, so nothing below needs to know it ran.
	weatherDrawDraw(chunkRenderProjection(), view, &s_weatherdraw);

	// v1.8.9 particle system. Same placement rule as weatherDrawDraw immediately above and
	// for the same reason (blends against the colour buffer, must run after every opaque
	// draw that could cover the same pixels) -- see
	// docs/plan-1.8.9-particles-integration.md §3. particlesDraw depth-TESTS against the
	// world's buffer without writing to it, so it needs chunkRenderDraw's terrain already
	// resolved, which it is by this point. Also restores its own GPU state before returning.
	particlesDraw(view);

	// v1.8.8 NEON BIOME BORDERS — the debug overlay, and the ONLY line of the world pass it
	// costs. There is no render hook in this file and none was invented for it: every stage
	// below is a direct symbol call and this is one more, which is the smallest draw site that
	// works and the shape the four calls under it already have.
	//
	// HERE, after the terrain and before the highlight, for two reasons. It has to be after
	// chunkRenderDraw so the fence is depth-tested against the world and a seam behind a hill
	// is hidden by the hill — an overlay that showed through terrain would be a different and
	// much less useful feature. It has to be before highlightDraw so the selection cage is
	// drawn over the fence rather than under it: the cage says what you are about to break and
	// must never be the thing that is obscured.
	//
	// Unconditional, like playerModelDraw two stages down and for the same reason: the
	// function's own first line is the toggle test, so there is exactly one place that decides
	// whether this feature happens. A second copy of the condition here is the kind of thing
	// that goes out of step. With the toggle off this is a call, a byte loaded and a return,
	// and the frame is unchanged.
	biomeBorderDraw(view);

	// After the world, because it re-binds the GPU for its own vertex format and does not
	// put it back — chunkRenderDraw re-establishes everything it needs at the top of its
	// next call instead. See pipelineBind in chunk_render.c. highlightDraw itself checks
	// hit->hit (see highlight.h), so there is no guard here to keep in sync with it.
	// It reads chunkRenderProjection(), so it gets this eye's matrix for free.
	drawStage(WD_DRAW_HIGHLIGHT, eye);
	highlightDraw(view, hit);

	// v1.8.1 task 50. The spreading fracture on the block being mined.
	//
	// Gated HERE and not inside crackOverlayDraw, because scene/crackoverlay.h states that it
	// carries no "is anything happening" flag on purpose: stage 0 already draws visible
	// cracks, so calling it unconditionally would paint a permanent fracture on whatever the
	// player happens to look at. -1 from interactBreakStage is the whole of that signal.
	//
	// hit->x/y/z rather than Interact's break_x/y/z, and they are the same coordinates by
	// construction: scene/interact.c restarts progress the moment the crosshair leaves the
	// block, so a stage of 0 or more already means the target IS the block being broken.
	// Passing the target keeps drawEye's parameter list to one added int.
	//
	// After the highlight so the cage's bars sit in front of the fracture, and inside drawEye
	// rather than beside it so a stereo frame cracks the same block in both eyes.
	if (crack_stage >= 0 && hit->hit)
		crackOverlayDraw(view, hit->x, hit->y, hit->z, crack_stage);

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
	entityModelDraw(view, &s_entities);
	drawStage(WD_DRAW_TAGS, eye);
	playerModelDrawTags(view, 400.0f, 240.0f);

	// The reticle, last of all so it sits over the name tags rather than under them, and
	// inside drawEye rather than outside it so that each eye gets one at the same screen
	// position — which is what puts it at screen depth instead of floating in front of or
	// behind the block being aimed at. It opens and closes its own sprite batch.
	crosshairDraw(400.0f, 240.0f);
}

// The player's items. Outside the BS_BOTTOM_UI guard on purpose: a build with no bottom
// screen still has an inventory, because Interact.holding is fed from it and a break still
// has to put the block somewhere. Such a build simply has no way to *see* or rearrange it.
// A file static rather than a local of main() for the same reason s_world is one — the
// bottom-screen draw below is a file-scope function and needs it.
static Inventory s_inv;

// v1.8.15. Per-block state for blocks that carry any — currently only the furnace, which needs
// to remember its fuel, its input and how far along the smelt is. A single 2 KB side table
// (BLOCKSTATE_SLOTS 64 entries of 32 bytes) rather than widening the block id or the chunk
// format, because the overwhelming majority of blocks carry no state at all and paying for a
// payload on every cell in the world to serve sixty-four of them is the expensive way round.
//
// File static for the same two reasons as s_inv above: the bottom-screen draw is a file-scope
// function, and the lid-close flush hook is a callback. Neither can see main()'s locals.
static BlockStateTable s_blockstate;

// v1.8.13 SURV-WIRE. The player's vitals, reachable from file scope for exactly the two
// reasons s_sleep_player below is a file static: the bottom-screen draw is a file-scope
// function and the lid-close flush hook is a callback, and neither can see main()'s locals.
//
// ONE pointer rather than one per consumer, and deliberately outside both #if BS_BOTTOM_UI
// and #if BS_WORLD_GEN: a second static holding the same address is how the two end up
// pointing at different things after a refactor. Set once at world entry beside
// s_sleep_player, cleared in the same teardown — a stale pointer read after `survival` has
// gone out of scope is the one way this could be worse than not drawing the bars at all.
static const Survival* s_survival = NULL;

// v1.8.13 SURV-WIRE. The whole of death: full vitals, a cleared fall tracker, and the body
// back at the spawn the world entry already computed.
//
// A function rather than the same six lines written twice, because there are two callers —
// the fall-damage check on the frame path and the tick that could one day poison or drown —
// and the failure mode of letting them drift is invisible: one of them forgetting to reset
// the FallTrack means the player dies, respawns, and is immediately killed again by the fall
// damage of their own respawn drop, over and over, with nothing on screen to explain it.
// fallTrackInit here is what makes that unrepresentable rather than remembered.
//
// Takes the spawn as three floats rather than reading a global: the spawn is a const local of
// the world-entry function under #if BS_WORLD_GEN, and passing it keeps this honest about
// having no opinion on where spawn is.
static void survivalRespawn(Player* p, Survival* s, FallTrack* ft,
                            float sx, float sy, float sz)
{
	survivalInit(s);
	fallTrackInit(ft);
	// The same yaw and pitch the world-entry playerInit uses, so a respawn faces the way a
	// fresh spawn does instead of keeping the angle the player happened to die at.
	playerInit(p, sx, sy, sz, C3D_AngleFromDegrees(135.0f), C3D_AngleFromDegrees(35.0f));
}

#if BS_BOTTOM_UI

static UiState s_ui;

// v1.8.15 FURNACE. WHICH furnace the bottom-screen panel is currently showing, as a cell
// address rather than as a FurnaceState. That distinction is the whole design of this wiring
// and is worth stating, because holding the state here instead would be the obvious shape and
// would be wrong.
//
// world/blockstate.c owns the only copy of every furnace's state. The per-tick loop below
// advances all sixty-four of them — burning fuel, cooking, going dark — on the same table the
// panel is editing. If this file kept its own FurnaceState across frames and wrote it back at
// the end, every tick the panel was open would be undone: the player would watch a furnace
// that never burns down, because the frame path would keep restoring the copy it took before
// the ticks ran. So the panel's state is UNPACKED FRESH from the table immediately before
// uiUpdateDraw and PACKED BACK immediately after, every frame, and nothing is carried between
// them. The tick owns fuel_ticks_left/cook_ticks/lit, the panel owns the four item fields, and
// neither can lose the other's writes because neither ever holds a stale copy.
//
// s_furnace_open rather than a sentinel coordinate, for blockstate.h's own reason: (0,0,0) is
// an ordinary cell a player can build a furnace in, so no coordinate triple is free to mean
// "no furnace open".
static bool s_furnace_open;
static int  s_furnace_x, s_furnace_y, s_furnace_z;

// v1.8.17. What a broken furnace hands back. Registered with scene/interact.c once at startup
// and called by it from breakComplete, at the one moment the block has been decided broken but
// its blockstate record still exists — the removal is twenty lines further down in this file's
// edit handler, and reading after that would read nothing.
//
// It lives here rather than in interact.c because s_blockstate is this file's static and
// interact.c has no world state of its own. The alternative was to hand interact.c a
// BlockStateTable*, and that was rejected for a build reason rather than a design one: it
// would pull blockstate.c, furnace.c and crc32.c into three separate shared gcc stanzas in
// tools/run_host_tests.sh, and the callback needs no new link dependency at all.
//
// Returns the number of stacks written, never more than INTERACT_BROKE_EXTRA_MAX. The three
// slots are exactly the three a furnace has, so the bound is the furnace's own shape and not
// an arbitrary cap — if a future stateful block has more, that constant moves with it.
//
// The blockStateGet call is guarded on BLOCK_FURNACE by its expect_block_id argument as well
// as by the test above it. That is not redundant: a cell can hold a record whose block_id
// disagrees with what was just broken, and the id check is what stops a furnace's payload
// being unpacked out of some other block's bytes.
static int mainFurnaceBrokeContents(BlockId id, int x, int y, int z,
                                    BlockId items_out[INTERACT_BROKE_EXTRA_MAX],
                                    uint8_t counts_out[INTERACT_BROKE_EXTRA_MAX])
{
	if (id != BLOCK_FURNACE) return 0;

	uint8_t payload[BLOCKSTATE_PAYLOAD_BYTES];
	if (!blockStateGet(&s_blockstate, x, y, z, BLOCK_FURNACE, payload)) return 0;

	FurnaceState fs;
	furnaceStateUnpack(&fs, payload);

	int n = 0;
	if (fs.input_count  > 0) { items_out[n] = fs.input_item;  counts_out[n] = fs.input_count;  n++; }
	if (fs.fuel_count   > 0) { items_out[n] = fs.fuel_item;   counts_out[n] = fs.fuel_count;   n++; }
	if (fs.output_count > 0) { items_out[n] = fs.output_item; counts_out[n] = fs.output_count; n++; }
	return n;
}

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

	// v1.8.11 METRICS-VISIBLE. The frame-timing row, built only when the debug menu's "Frame
	// timing" toggle is on and left NULL otherwise, so a normal session draws exactly the panel
	// it drew before. Formatted here rather than in scene/ui.c because scene/ui.h must not gain
	// a dependency on debug/metrics.h (and through it <3ds.h>), which is the same rule every
	// other number on that panel already follows.
	//
	// Four figures and no more, because four is what fits and what the question needs:
	//   cpu   — main-thread WORK, frame_ms minus the one blocking wait in the loop
	//   wait  — blocked in gpuWaitPrevFrame(): VBlank plus the GPU command queue
	//   frame — wall clock, so cpu + wait can be checked against it on screen
	//   fps   — derived from `frame`, because "is this below 60" is half of the reading
	// Derived from the averaged frame rather than metricsFrameMs(): an fps computed from the
	// instantaneous frame and printed beside an averaged frame time would disagree with it on
	// screen, and the two disagreeing is exactly what makes a readout untrustworthy.
	char timing[64];
	timing[0] = '\0';
	if (metricsRowEnabled()) {
		const float f = metricsFrameAvgMs();
		snprintf(timing, sizeof timing, "cpu %.1f  wait %.1f  frame %.1f ms  %.0f fps",
		         (double)metricsCpuAvgMs(), (double)metricsWaitAvgMs(), (double)f,
		         (f > 0.0f) ? (double)(1000.0f / f) : 0.0);
	}

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
		.timing     = timing[0] ? timing : NULL,
		// v1.8.13 SURV-WIRE. Zero when s_survival is NULL, which ui.h's own comment already
		// names as a legitimate reading drawn as an empty bar rather than as "unset" — so the
		// one frame between this panel first drawing and the pointer being handed over at
		// world entry shows two empty bars, never a crash and never a stale value.
		.health     = s_survival ? s_survival->health : 0,
		.hunger     = s_survival ? s_survival->hunger : 0,
	};

	// v1.8.15 FURNACE. Unpack the open furnace's state out of world/blockstate.c, hand the
	// panel a pointer to it, and pack whatever the player did straight back. See s_furnace_open
	// above for why nothing is carried between frames.
	//
	// The blockStateGet() failure branch is not defensive padding — it is the one case that
	// actually happens. A furnace can stop existing while its panel is open: another player
	// breaks it over the network, or the record is evicted. blockStateGet refuses on both "no
	// record here" and "a record whose block_id is not BLOCK_FURNACE", so a cell that became a
	// dirt block reads exactly like a cell that became nothing, and both close the panel rather
	// than editing whatever happens to occupy the slot now.
	//
	// `fp` stays NULL in that case and ui.h's contract takes over: uiUpdateDraw corrects
	// UI_SCR_FURNACE to UI_SCR_INVENTORY on the spot and drops any lifted stack, so the player
	// is put back in their bag holding their items rather than looking at a panel for a block
	// that is gone.
	FurnaceState  fs;
	FurnaceState* fp = NULL;
	uint8_t       fpay[BLOCKSTATE_PAYLOAD_BYTES];

	if (s_furnace_open) {
		if (blockStateGet(&s_blockstate, s_furnace_x, s_furnace_y, s_furnace_z,
		                   BLOCK_FURNACE, fpay)) {
			furnaceStateUnpack(&fs, fpay);
			fp = &fs;
		} else {
			s_furnace_open = false;
		}
	}

	// atlasTexture() rather than atlasBind(): gfx/sprite.c tracks the bound texture itself so
	// it can flush exactly once per switch, and binding behind its back would leave it certain
	// it had not switched. See gfx/atlas.h.
	const UiResult ures = uiUpdateDraw(&s_ui, &s_inv, atlasTexture(), &stats, in, fp);

	// Written back BEFORE the close is acted on, not after, and the order matters: the tap that
	// closes the panel can be the same tap that took the cooked item out of the output slot, so
	// packing only when the panel stayed open would silently eat the last withdrawal of every
	// session. The write is guarded on `fp` and not on `ures.furnace_open` for that reason.
	if (fp) {
		furnaceStatePack(fp, fpay);
		(void)blockStateSet(&s_blockstate, s_furnace_x, s_furnace_y, s_furnace_z,
		                     BLOCK_FURNACE, fpay);
	}
	if (!ures.furnace_open) s_furnace_open = false;
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
	//
	// v1.9.1: `see` used to read rd->half_vis, which comes from the retired PICA200
	// fixed-function fog LUT model and has had no GPU path since v1.9.0 — it barely moved
	// across render distances (~14.3-14.4 regardless of radius) while what the player actually
	// saw scaled hugely with the setting. chunkRenderFogHalfVis() reads the live shader-fog
	// ramp instead — see its comment in scene/chunk_render.c.
	const RenderDist* rd = chunkRenderDistance();
	printf("dist %d  see %4.1f  edge %4.1f %s\n", rd->radius, chunkRenderFogHalfVis(), rd->boundary,
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

		// v1.7.1 task 48b. The save-slot wait, on the overlay because it is the one main-thread
		// stall that only appears where the player has BUILT — every capture taken on a clean
		// world leaves this path untouched, and `save_ms` reads 0.0 either way.
		//
		// How to read it: `sv` is columns handed to the save ring, `w` how many of those made
		// this thread sleep waiting for one of the two slots, then the total and worst wait in
		// ms. `sv 0` means the branch was never entered and the line says NOTHING about
		// whether it stalls — walk out of somewhere you have built to make it say something.
		// `sv` large with `w 0` is the measured negative. `w` climbing with a `max` of several
		// ms is the positional hitch it was added to catch.
		printf("save sv%-3d w%-3d %5.1f max %4.1f ms\n", workerSaveSubmits(),
		       workerSaveWaits(), workerSaveWaitMs(), workerSaveWaitMaxMs());
#if BS_WORLD_GEN
		// Step 6.1's line. `at` is the column the ring is centred on, `unl` the columns
		// freed because they left it, and `stale` the columns that finished generating for
		// a ring position the player had already walked out of — those are thrown away on
		// arrival, and a large number there means the ring is moving faster than the worker.
		//
		// v1.8.10 `hole`: columns installed with a real gap in them (world/genretry.h) that
		// s_gen_retry is still waiting to resubmit — the "one chunk away and it never loads in"
		// bug's own counter. 0 almost always; briefly non-zero under budget pressure and always
		// falling back to 0 within GEN_RETRY_MAX_FRAMES (300, ~5 s) of the budget freeing up. A
		// number that climbs and never comes back down is the one this line exists to catch.
		printf("ring at %+3ld %+3ld unl %3d stale %d hole %d\n", (long)s_center_cx,
		       (long)s_center_cz, s_col_unloaded, s_col_dropped, genRetryOutstanding(&s_gen_retry));

		// v1.8.11. The mesh-side twin of `hole`, and the pair of numbers that says which of the
		// two mesh refusal classes is actually firing — the thing a single conflated
		// mesh_refused count could never say.
		//
		//   mhole — columns whose mesh build was refused and which s_mesh_retry has not yet got
		//           back in. Behaves like `hole`: brief spikes are fine, a number that climbs
		//           and never returns to 0 is a real leak.
		//   pool  — cumulative build refusals from an exhausted slot pool. RECOVERABLE; each one
		//           is retried. A large and growing count means the tier sizing in
		//           scene/chunk_render.c is short for this render distance.
		//   ovfl  — cumulative refusals from a chunk too complex to fit the per-chunk cap.
		//           NOT recoverable and NOT retried. This one must be zero. Any nonzero value
		//           is a permanent hole that no amount of walking will fix, and it means
		//           MESH_SLOT_VERTS/MESH_SLOT_INDICES need raising.
		printf("mhole %d pool %3d ovfl %3d     \n", genRetryOutstanding(&s_mesh_retry),
		       gen->mesh_refused_pool, gen->mesh_refused_overflow);

		// Step 6.2's line, and the only per-frame costs the streaming path adds. `strm` is
		// install + mesh drain on the worst frame, `rc` the recentre on the frame that
		// crossed a column boundary, `qp` the deepest the mesh queue ever got and `n` the
		// most chunks meshed in one frame. Read `strm` against the 16.71 ms frame.
		printf("strm %5.2f rc %5.2f qp%3d n%d  \n", gen->worst_stream_ms,
		       gen->worst_recenter_ms, jobqPeak(&s_meshq), gen->worst_built);
		// v1.8.8. `lanes` is how many generator lanes actually started and `core`/`c1` where
		// each of them landed, because both are requests the system may refuse: an Old 3DS and
		// a New 3DS whose exheader did not grant core 2 both read "lanes 1 core 0 c1 -1" and
		// that is the v1.8.7 arrangement unchanged, not a fault. `pk` is the ready ring's
		// high-water mark — 2 is the only value that says the two lanes ever really overlapped
		// — and `dup` counts columns the second lane was refused because the first already had
		// them, which is expected traffic when the player walks out of a column and back, not
		// a hole in the world (app/worker.h).
		printf("fill %4d frames  core %d      \n", gen->fill_frames, workerCore());
		printf("lanes %d c1 %2d pk%d dup %-3d  \n", workerLanes(), workerLaneCore(1),
		       workerReadyPeak(), workerDupDropped());
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

// ── v1.6.0: the same writes, one at a time, for app/sleep.c's lid-close flush ──────────
//
// saveDirtyColumns() above is the quit path and is allowed to take as long as the card
// takes. Closing the lid is not the quit path — it is a player who expects the console to
// settle immediately — but a battery that dies while the lid is shut takes the whole
// session with it, so doing nothing is not the answer either. app/sleep.h's
// SLEEP_FLUSH_BUDGET_MS is the compromise, and this is the step it spends that budget on.
//
// Split as a step rather than a whole loop because the budget is checked BETWEEN columns:
// sleep.c drives, this writes exactly one column and blocks until it lands, and the clock
// gets looked at in between. workerFlushSaves() per column costs more total time than one
// flush at the end would, and that is the trade — a batched submit would leave the budget
// measuring a queue rather than the card, which is the number that matters here.
//
// Slot order, not newest-first. `Column` (world/world.h) holds cx, cz, its eight chunk
// pointers, `dirty` and `light` — there is no timestamp, no sequence number and no
// touched-at counter anywhere in it, so there is nothing to sort by. Adding a field to get
// an ordering would be inventing state the game does not otherwise keep, so this walks the
// slot table in order and the cursor below is what stops that from being unfair.
//
// The cursor persists across calls and is only rewound once the walk reaches the end, so a
// lid-close that runs out of budget halfway resumes where it stopped the next time rather
// than re-writing the same head of the table forever. It is reset when the hook is
// registered, in genStart(), so a new world starts at slot 0.
//
// ⚠ `dirty` is never cleared — world/world.h documents it as raised at the edit site and
// the column is simply freed when it unloads — so a column written by one lid-close is
// still dirty at the next one and will be written again when the cursor comes back round.
// That is wasted card time, not lost data, and clearing the flag here would change what
// the quit path above means. Left alone deliberately.
//
// Guarded on BS_WORLD_GEN because the only place that registers these is inside the same
// guard — a BS_REMESH_STRESS/BS_DIG_OUT/BS_EDIT_STRESS build has a hand-built world and no
// worker, so both would be statics nobody calls and -Wall would say so.
#if BS_WORLD_GEN
static int s_sleep_flush_cursor;

// v1.7.1 task 46b. What the lid-close flush needs to write the pose, and the only reason
// these are file statics rather than locals: sleepFlushOneColumn() is a hook app/sleep.c
// calls, so it cannot see main()'s `player` or `inv_dir`. Set once at world entry, cleared
// in the same teardown that clears the hooks — a stale Player pointer read after the world
// has been left is the one way this could be worse than not saving at all.
//
// s_sleep_pose_done makes it a ONE-SHOT. sleepFlushOneColumn is a step function: sleep.c
// calls it repeatedly until the budget runs out or it returns false, and writing the pose
// on every one of those calls would spend the whole SLEEP_FLUSH_BUDGET_MS rewriting 32
// bytes instead of flushing the columns the budget exists for. Reset beside
// s_sleep_flush_cursor when the hook is registered, so a new world starts owing a write.
static const Player* s_sleep_player   = NULL;
static const char*   s_sleep_pose_dir = NULL;
static bool          s_sleep_pose_done;

static bool sleepFlushOneColumn(void)
{
	// The pose first, before any column, and this is the ordering to prefer on purpose: it
	// is 32 bytes against a column's kilobytes, and it is the field the player would most
	// notice losing. app/sleep.h budgets 500 ms and a card write is 5-20 ms, so spending one
	// of roughly 25-100 slots on it costs at most 4% of the budget.
	//
	// Returns true, like a column write does, so the budget is checked between this and the
	// first column rather than after both — sleep.c drives one step at a time.
	if (!s_sleep_pose_done) {
		s_sleep_pose_done = true;
		if (s_sleep_pose_dir && s_sleep_player) {
			const PlayerPose pose = {
				s_sleep_player->body.x, s_sleep_player->body.y, s_sleep_player->body.z,
				s_sleep_player->cam.yaw, s_sleep_player->cam.pitch
			};
			(void)playerPoseSave(&pose, s_sleep_pose_dir);
			// v1.8.13 SURV-WIRE, and written on the SAME one-shot slot as the pose rather
			// than given a slot of its own. Two reasons. It is 14 bytes against the pose's
			// 32, so the budget argument above covers both together; and the two must not be
			// able to diverge — a lid closed between them would restore a player at the
			// position they slept at with the health they had some earlier session, which is
			// a stranger bug than losing both.
			//
			// Not gated on world_refused the way the quit path below is: a refused world
			// never registers this hook (s_sleep_pose_dir stays NULL), so the guard above
			// already covers it.
			if (s_survival) (void)survivalSave(s_survival, s_sleep_pose_dir);
			return true;
		}
	}

	while (s_sleep_flush_cursor < WORLD_MAP_SLOTS) {
		const Column* col = s_world.slots[s_sleep_flush_cursor++];
		if (!col || !col->dirty) continue;

		// The return value is deliberately not branched on: app/worker.h says false means a
		// column that could not be encoded, which is a corrupt column and a bug report
		// rather than something to retry, and either way the budget has been spent.
		(void)workerSubmitSave(col);
		workerFlushSaves();
		return true;
	}

	s_sleep_flush_cursor = 0;
	return false;
}

// The other half of the lid-close work. Unconditional netDisconnect() would be wrong here
// only in the sense that it is pointless off a server session — bsnet.h says it is safe to
// call in any state — but s_server_session is the honest gate and it is free, so the
// single-player lid-close touches nothing at all.
//
// Why leaving at all is the right answer, and what was rejected instead, is argued in
// app/sleep.c above the call to this. The short version: the gateway drops a silent session
// after 30 s (BS_SESSION_IDLE_MS, deps/blocksmith-server/gateway/bsgate.c:75) and nothing of
// ours runs while the lid is shut to stop it.
static void sleepLeaveSession(void)
{
	// v1.7.1 task 46b. This is also the per-lid-close signal the pose write needs, and the
	// only one available: app/sleep.c calls the leave hook exactly once at ONSLEEP, before
	// sleepFlushBounded() starts stepping the flush. Rearming here rather than where the
	// flush cursor rewinds is what makes the pose written once per LID CLOSE instead of once
	// per world — the cursor only rewinds when the whole slot table has been walked, so a
	// lid-close that ran out of budget halfway would otherwise leave the next one owing
	// nothing and the player's later position unsaved.
	s_sleep_pose_done = false;

	if (s_server_session) netDisconnect();
}
#endif  // BS_WORLD_GEN

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
// v1.8.13 survival. Same guard as the seven above, for the two bits ACTION_EAT's default
// needed — options.h cannot include <3ds.h>, so this is the only place the hex literals it
// hardcodes are ever checked against the real header.
_Static_assert(OPT_KEY_ZL     == KEY_ZL,     "OPT_KEY_ZL drifted from libctru KEY_ZL");
_Static_assert(OPT_KEY_ZR     == KEY_ZR,     "OPT_KEY_ZR drifted from libctru KEY_ZR");

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

	// v1.8.3. The sentence the player gets when the world they picked would not open — see
	// s_world_refused_why and world/genrefuse.h. Written straight into the state titleInit just
	// zeroed, the same way the screen above it is: scene/title.c owns the status line's layout
	// and its countdown, and this is the one caller that has something to put on it before the
	// first frame is drawn. It lands on the MAIN screen, which is where the menu opens from here,
	// and scene/title.c's drawMain draws it there.
	//
	// Consumed, not just read: cleared so the next trip back to the menu — after a world that
	// opened fine, or after a server session — does not repeat a refusal the player has already
	// been told about. The message itself is a string literal with static storage duration, so
	// the copy below is about the TTL, not about lifetime.
	if (s_world_refused_why) {
		snprintf(ts.status, sizeof(ts.status), "%s", s_world_refused_why);
		ts.status_ttl = WORLD_REFUSED_STATUS_TTL;
		s_world_refused_why = NULL;
	}

	while (aptMainLoop()) {
		hidScanInput();
		BS_STAGE_ONCE(BS_STAGE_ID_INPUT_POLL, "INPUT_POLL_FIRST");

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

		// keys_held is read for exactly one thing — the update screen's release-notes scroll,
		// which repeats while the D-pad is held (scene/title.h's own comment on the field, and
		// app/whatsnew.h on why it is a different HID word from keys_down rather than the same
		// one read twice).
		const u32 held = hidKeysHeld();

		const TitleInput in = {
			.keys_down  = hidKeysDown(),
			.keys_held  = held,
			.touch_down = (held & KEY_TOUCH) != 0 || tp.px != 0 || tp.py != 0,
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
		BS_STAGE_ONCE(BS_STAGE_ID_FRAME_BEGIN, "FRAME_BEGIN_FIRST");
		spriteFrameBegin();

		// title.c calls spriteBegin(320, 240) itself and assumes the caller has already
		// bound the target it wants drawn on — see the file comment in scene/title.h.
		C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0x1A1424FF, 0);
		C3D_FrameDrawOn(bottom);
		const TitleResult r = titleUpdateDraw(&ts, opts, &in);

		// The top screen is cleared and bound every frame whether or not the menu puts
		// anything on it. C3D_FrameEnd only presents screens with a target bound this frame,
		// so skipping it would leave the top screen holding whatever was in the framebuffer
		// at boot — uninitialised VRAM — for as long as the player sits in the menu.
		//
		// It is drawn AFTER the bottom half rather than before it, which is the order v1.6.0
		// task 14b needs and the reason this moved: titleUpdateDraw is where the D-pad is
		// read, and titleDrawTop is what clamps and paints the release-notes scroll position
		// that reading produced. Drawing the top first showed the previous frame's scroll —
		// harmless, but a frame of lag on the one control this screen exists for.
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
		titleDrawTop(&ts);

		C3D_FrameEnd(0);
		BS_STAGE_ONCE(BS_STAGE_ID_FRAME_END, "FRAME_END_FIRST");

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

// diagRetire() now lives in app/diag_retire.h/.c — see there for the full incident history
// (why the fence renames rather than deletes, and why retirement is conditional on this boot
// having anything to retire). Moved out on 2026-09-04 so tests/diag_retire_test.c can link the
// real function directly instead of a hand-copied twin: main.c pulls in <3ds.h> at its very
// first #include and can never be compiled as a whole on the host.

static void diagFenceBoot(void)
{
	static const char* const kill[][2] = {
		{ "sdmc:/blocksmith/hang.txt",        "sdmc:/blocksmith/prev-hang.txt"        },
		{ "sdmc:/blocksmith/postmortem.txt",  "sdmc:/blocksmith/prev-postmortem.txt"  },
		{ "sdmc:/blocksmith/gxprobe.txt",     "sdmc:/blocksmith/prev-gxprobe.txt"     },
		{ "sdmc:/blocksmith/selftest.txt",    "sdmc:/blocksmith/prev-selftest.txt"    },
		{ "sdmc:/blocksmith/cmdhang.bin",     "sdmc:/blocksmith/prev-cmdhang.bin"     },
		{ "sdmc:/blocksmith/cmdprev.bin",     "sdmc:/blocksmith/prev-cmdprev.bin"     },
		{ "sdmc:/blocksmith/bisect.bin",      "sdmc:/blocksmith/prev-bisect.bin"      },
		{ "sdmc:/blocksmith/boot_timing.txt", "sdmc:/blocksmith/prev-boot_timing.txt" },
		{ "sdmc:/blocksmith/bootid.txt",      "sdmc:/blocksmith/prev-bootid.txt"      },
	};
	for (size_t i = 0; i < sizeof(kill) / sizeof(kill[0]); i++)
		diagRetire(kill[i][0], kill[i][1]);

	// The capture ring's numbered frames, same reasoning. Longest path built here is
	// "sdmc:/blocksmith/prev-cmd7.bin" at 30 characters, so 64 is not a tight fit.
	for (int k = 0; k < 8; k++) {
		char live[64], prev[64];
		snprintf(live, sizeof live, "sdmc:/blocksmith/cmd%d.bin", k);
		snprintf(prev, sizeof prev, "sdmc:/blocksmith/prev-cmd%d.bin", k);
		diagRetire(live, prev);
	}

	// hwInit() asked this at the top of main(); asking again here would be a second answer
	// that can disagree with the one the worker and the render distance were built on.
	const bool n3ds = hwIsNew3ds();

	FILE* f = fopen("sdmc:/blocksmith/bootid.txt", "wb");
	if (!f) return;
	fprintf(f,
		"Blocksmith boot fence\n"
		"\n"
		"version   : %s\n"
		"console   : %s\n"
		"boot tick : %llu\n"
		"\n"
		"Every other diagnostic file in this folder was retired to prev-<name> immediately\n"
		"before this one was written, but only if this boot actually had one to retire — a\n"
		"boot that produced nothing new leaves prev-<name> exactly as it was. So anything\n"
		"sitting beside it WITHOUT that prefix — hang.txt, postmortem.txt, gxprobe.txt,\n"
		"cmd*.bin — was written by THIS boot and no other, and anything WITH it is from the\n"
		"most recent boot that actually wrote one, however many clean boots came after it.\n"
		"drawprobe.txt is the exception and is meant to span boots.\n"
		"\n"
		"If you are here because the console froze: the file you want is hang.txt if this boot\n"
		"is the one that froze, or prev-hang.txt if you have already relaunched since. Clean\n"
		"relaunches do NOT discard prev-hang.txt any more — it is only replaced once another\n"
		"freeze writes a new hang.txt and you relaunch again after that.\n",
		BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "(unset)",
		n3ds ? "New 3DS" : "Old 3DS",
		(unsigned long long)svcGetSystemTick());
	fclose(f);
}

// Wait for everything the GPU was handed last frame to actually finish.
//
// C3D_FrameEnd(0) hands the frame's command list to GX and RETURNS. The GPU is still reading
// the chunk vertex buffers — out of physical RAM, on its own bus — for however long that draw
// takes. Everything the loop does next can write to exactly those buffers: genFollow frees
// columns and hands their mesh slots to different chunks, and the two mesh drains memcpy fresh
// geometry into slots that are already on screen. Until now the only wait for the GPU came
// later, at C3D_FrameBegin — AFTER all of that had already run. So from the second world frame
// onward the CPU rewrote geometry underneath a draw that was still in flight, and the vertex
// fetch unit followed indices into half-overwritten memory and stopped without ever reporting
// completion. That is the wedge every hardware report has shown.
//
// Frame 2 is exactly when steve's console died, and that is not a coincidence: the loading
// loop drains meshes but never draws, so frame 1 is the first frame that ever submits a chunk
// draw, and frame 2 is therefore the first iteration where an in-flight draw exists to race.
// It never reproduced in an emulator because an emulator runs each GX command inline at
// submission — there is no in-flight window to race at all.
//
// The v1.2.0 cache flushes made the CPU's writes reach RAM sooner, which if anything widened
// the window rather than closing it. That is why they did not help.
static void gpuWaitPrevFrame(void)
{
#if BS_GPU_TESTS
	// Bounded (BS_GPU_WEDGE_TICKS) and self-reporting: if the queue really has stopped, this
	// gives up rather than hanging the console, and writes sdmc:/blocksmith/postmortem.txt.
	//
	// FIXED 2026-09-04: gpu_dead added. Without it, this call paid a fresh bounded wait EVERY
	// FRAME once the GPU wedged — gxCmdQueueWait can only return false by burning its whole
	// timeout, so a wedged GPU turned every subsequent frame into another multi-second stall.
	// That is exactly the failure the draw block's own gpu_dead guard further down was written
	// to prevent ("a frozen console becoming one that merely runs at a small fraction of a frame
	// per second"), and that guard was never mirrored up here — so the thing its comment says
	// was fixed was still happening, at this call site, in every shipped build.
	//
	// Skipping the wait once dead is safe. The race this function exists to prevent is the GPU
	// still READING mesh slots that this frame's CPU work is about to overwrite; once the GPU is
	// proven to be executing nothing at all, there is no reader to race.
	static u32 wait_frame;
	static bool gpu_dead;
	if (gpu_dead) return;
	wait_frame++;
	if (!gpuTestFrameWait()) {
		gpuTestPostMortem(wait_frame);
		gpu_dead = true;
	}
#else
	// CORRECTED v1.8.10. This was C3D_FrameSync(), on the strength of c3d/renderqueue.h's
	// one-line doc comment "Waits for the GPU to finish rendering." That comment is misleading
	// and this function was measurably not doing its job.
	//
	// Disassembling the actually-linked libcitro3d.a (1.7.1-2) shows C3D_FrameSync is:
	//
	//     r6 = frameCounter[0]; r5 = frameCounter[1];
	//     do { gspWaitForAnyEvent(); } while (frameCounter[0]==r6 || frameCounter[1]==r5);
	//
	// It waits for a VBLANK TICK ON BOTH SCREENS. There is no bl to gxCmdQueueWait, and no
	// GX/GPU-busy check of any kind anywhere in it — confirmed by the absence of the call, and
	// matching upstream renderqueue.c at tag v1.7.1 instruction for instruction. A vblank
	// arrives every ~16.7 ms whether or not the GPU has finished. The two are unrelated events.
	//
	// So the wait that actually blocks until the queue drains, gxCmdQueueWait(-1), was only ever
	// reached later, inside C3D_FrameBegin(C3D_FRAME_SYNCDRAW) at the draw — AFTER genFollow and
	// both mesh drains had already reassigned and memcpy'd over slots the GPU was still reading.
	// This function narrowed the race window from "the rest of the frame" to "until the next
	// vblank" and was then described at its call site as "THE fix for the hardware freeze". It
	// was not. It is harmless only while a frame finishes inside one vblank, and radius 5 does
	// not: steve's New 3DS froze at render distance 5 with a visibly low frame rate, which is
	// precisely the condition in which the remaining window is wide open.
	//
	// C3D_FrameBegin(C3D_FRAME_SYNCDRAW) is that same vblank wait FOLLOWED by the real
	// gxCmdQueueWait(-1) — also measured, from the same disassembly: bit 0 of the flags gates an
	// inlined copy of the C3D_FrameSync loop, and both paths then fall through to an
	// unconditional gxCmdQueueWait. It is public API in c3d/renderqueue.h, unlike the queue
	// pointer itself, which app/gputest.c can only reach through a documented __C3D_Context hack
	// that its own comment calls fragile and confines to the diagnostic build.
	//
	// Calling FrameBegin here rather than at the draw is safe, for one checked reason: there is
	// not a single citro3d or GX call anywhere between this line and the draw block. Everything
	// in that span — the sim, genFollow, both mesh drains — is CPU work and memcpy into linear
	// memory. So setting citro3d's inFrame flag early cannot capture commands into the wrong
	// frame, because no commands are emitted. The later C3D_FrameBegin(C3D_FRAME_SYNCDRAW) then
	// finds inFrame already set and early-returns 0 without waiting or clearing anything, which
	// the disassembly confirms is its entire behaviour on that path. If a citro3d call is ever
	// added between the two, this stops being true — that is the invariant to protect.
	//
	// Unbounded, like the call it replaces. That is deliberate in a build with the safety net
	// compiled out: BS_GPU_TESTS above is where the bounded version lives, and it has always
	// used the real queue wait, which is why the diagnostic build never carried this bug.
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#endif
}

// net/networld.h's edit hook. A remote player's edit has just been written into s_world, and
// the cached mesh of the chunk holding that block — plus any neighbour whose faces the change
// exposed or hid — now describes a world that no longer exists. This is the exact call
// scene/interact.c makes after a *local* break or place; before this existed, only local edits
// ever reached it, so another player's work was fully real to collision and to the block
// raycast while being invisible on screen until the chunk happened to stream out and back in.
//
// Queue-only, like every other chunkRenderTouch() caller: the remesh itself is charged to the
// frame budget in the main loop, so a burst of remote edits cannot blow a frame here.
static void onRemoteEdit(void* userdata, int x, int y, int z)
{
	(void)userdata;

	// v1.8.17. The cell this hook names has just been overwritten by something that is
	// not this player -- a remote player's break or place, or the water simulation via
	// onWaterChange below -- so whatever block used to stand there no longer does, and
	// any per-position state it carried is now an orphan.
	//
	// This is the exact mirror of the LOCAL break's blockStateRemove in the main loop,
	// and it is here rather than in net/networld.c for the reason net/networld.h's own
	// NetworldEditFn comment gives about chunkRenderTouch: that module stays host-testable
	// precisely because it never reaches into world/'s tables, so it reports which block
	// changed and the owner of the table -- this file -- decides what that means.
	//
	// Unconditional, and gated on nothing, for the same two reasons the local break path
	// spells out: blockStateRemove on a cell holding no state is a no-op by contract, and
	// asking "is this a stateful block" here would be a second, separate opinion about
	// which blocks those are -- the day it disagreed with the table the symptom would be a
	// slot that never frees. With BLOCKSTATE_SLOTS at 64 that is not a memory leak, it is
	// a fixed pool that fills: enough remote furnace breaks and a legitimate new furnace
	// gets no state at all. Worse, a furnace later placed on the same coordinate read the
	// dead one's contents straight back out of blockStateGet, and since v1.8.17 breaking a
	// furnace pays its contents into the inventory -- so a stale record was a duplication
	// route, not merely stale data.
	blockStateRemove(&s_blockstate, x, y, z);

	// v1.8.18. The place half of the mirror above, and the reason net/networld_test.c's
	// "a remote place of a furnace makes the block AND its blockstate record" stanza was
	// red: the remove above has always been here, but nothing ever CREATED a record on
	// this path. A furnace placed by another player therefore existed as a block and
	// nothing else -- blockStateGet on it answered false forever, so it held no fuel, no
	// input, no progress, and no amount of interacting with it could change that. The
	// local place path in the main loop has done this since v1.8.15; only the remote path
	// was missed.
	//
	// The block is read back out of the world rather than passed in because this hook's
	// signature deliberately carries only a coordinate -- net/networld.h's NetworldEditFn
	// reports WHICH CELL changed and leaves what that means to the owner of the tables,
	// which is this file. worldGet is the same question the mesher asks about the same
	// cell one line below, so it is already warm.
	//
	// Gated on BLOCK_FURNACE, unlike the remove above, and the asymmetry is the same one
	// the local path spells out at length: removing state costs nothing and is idempotent,
	// so the break side can afford to hold no opinion about which blocks are stateful.
	// Creating is not free -- BLOCKSTATE_SLOTS is 64, and creating a record for every
	// remote dirt block would fill the table in seconds of somebody else building and
	// leave no slot for a real furnace.
	//
	// The return is discarded for the same reason it is discarded locally: a full table
	// means 64 live records, and the furnace block itself has already landed in the world
	// via net/networld.c's worldSet. Un-placing another player's block underneath them
	// would be a far stranger outcome than a furnace that will not hold fuel, and every
	// read of an absent record is a clean "no state here" by world/blockstate.h's contract.
	if (worldGet(&s_world, x, y, z) == BLOCK_FURNACE) {
		if (blockStateCreate(&s_blockstate, x, y, z, BLOCK_FURNACE)) {
			// Packing a zeroed FurnaceState over blockStateCreate's own zero-fill is
			// redundant today and done anyway, for the reason the local path gives: "zero
			// happens to mean empty" is a property of world/furnace.h's current field
			// layout, not a promise it makes. The day a field gains a non-zero resting
			// value, this is what keeps a remotely placed furnace correct.
			FurnaceState fs;
			furnaceStateInit(&fs);

			uint8_t payload[BLOCKSTATE_PAYLOAD_BYTES];
			furnaceStatePack(&fs, payload);
			(void)blockStateSet(&s_blockstate, x, y, z, BLOCK_FURNACE, payload);
		}
	}

	// v1.5.0: remote edits relight before remeshing, same as scene/interact.c does for
	// local break/place — otherwise another player's torch-adjacent or shade-casting
	// change lands with stale light until the column happens to regenerate.
	//
	// v1.8.0: QUEUED, not run here. lightRelightColumn recomputes the whole column and
	// this hook is called once per edit — so a rejoin, where networldOnColumnLoad drains
	// every diff the server holds for a column the moment that column installs, paid for
	// one full relight per diff on a single column instead of one relight for all of them.
	// world/relightq.h dedups by column and the main loop drains it below, ahead of the
	// mesh drain so nothing bakes stale light. A full set (never expected: only loaded
	// columns can be pushed, and there are at most RENDER_DIST_MAX_COLUMNS of those)
	// falls back to relighting inline, which is the old behaviour rather than a dropped
	// relight.
	if (lightEnabled() && !relightqPush(&s_relightq, x >> 4, z >> 4))
		lightRelightColumn(&s_world, x >> 4, z >> 4);
	chunkRenderTouch(&s_world, x, y, z);
}

// v1.8.0 task 22. world/water.h's change hook: the simulation has just turned a cell into water
// or drained one back to air. Screen-side that is indistinguishable from a remote player having
// done it, so it is deliberately the same two calls and not a second copy of the reasoning above
// — relight the column (queued, falling back to inline on a full queue) and queue the remesh.
//
// Fired only for a cell whose BLOCK changed. A flow cell whose LEVEL changed inside water that
// was already there does not reach here; onWaterLevel below is what carries that case.
static void onWaterChange(void* ud, int x, int y, int z, BlockId id)
{
	(void)ud;
	(void)id;
	onRemoteEdit(NULL, x, y, z);
}

// v1.8.0 task 22b. world/water.h's LEVEL hook: the cell is still water and still water it was
// before, but its flow level moved, so its surface now sits at a different height and the chunk
// has to be re-meshed to show it. Before task 22b every level drew as the same full cube and
// this hook did not need to exist.
//
// A remesh and NOTHING ELSE, which is the whole reason it is not onWaterChange. onRemoteEdit
// relights the column first, and light is a function of which cells are opaque — a level change
// moves no block, so every one of those cells is exactly as it was and the relight would
// recompute an identical answer. Water settles over many ticks and touches a lot of cells on the
// way down, so that would be the most expensive thing in the frame, spent on nothing.
static void onWaterLevel(void* ud, int x, int y, int z, BlockId id)
{
	(void)ud;
	(void)id;
	chunkRenderTouch(&s_world, x, y, z);
}

// v1.8.0 task 22. world.h's edit hook, installed once. Every path that changes a single block —
// scene/interact.c's break and place, net/networld.c's remote edits, the BS_EDIT_STRESS harness —
// goes through worldSet, so hooking there is what lets water react to all of them without editing
// any of them.
//
// Cheap on purpose: it only queues candidates. The recompute happens on the tick, under the
// budget below, so no edit path can be made to do simulation work inline.
static void onWorldEdit(void* ud, const World* w, int x, int y, int z, BlockId prev, BlockId now)
{
	(void)ud;
	(void)prev;
	(void)now;

	// v1.8.3, and the whole reason world.h's hook now carries a World*.
	//
	// world.c's s_edit_fn is ONE file static (world.c:217) and worldSet fires it for any World*
	// at all (world.c:278). This program has two: s_world, which the line below simulates, and
	// app/worker.c's s_staging (worker.c:52), which worldgenColumn generates into on the WORKER
	// THREAD (worker.c:266) and whose decoration pass writes single blocks through worldSet
	// (worldgen.c:339 treePut, worldgen.c:458 tall grass). Until this compare existed, every
	// decorated cell of every streamed column arrived here and was pushed into s_water.
	//
	// Two separate faults, both closed by this one line:
	//
	//   * Every one of those candidates was WRONG. The cell it named did not change in the
	//     player's world, so the recompute it asked for was a guaranteed no-op that displaced
	//     real work — measured at 8590 firings across 144 streamed columns, worst single column
	//     221, offering up to 1547 candidates against a ring of 1024, so the ring sat pinned at
	//     its cap for as long as terrain was streaming.
	//
	//   * They arrived on the WORKER THREAD with no synchronisation against this thread's
	//     waterTick. app/worker.c runs on core 0 by default (BS_WORKER_CORE, worker.c:22), so
	//     the two never execute at the same instant — but the scheduler is priority-preemptive,
	//     so the main thread interrupts the worker part-way through qPush and runs a whole tick
	//     before it resumes. Measured on the host against the real water.c with a single
	//     preemption point injected into qPush: 64 preemptions left 12 keys in the dedup set
	//     with no ring entry behind them, 512 left 177, and the count only ever rises — nothing
	//     in water.c can remove a qset entry except a qPop that finds it in the ring. Each one
	//     is a coordinate qPush then refuses for ever (water.c:192 short-circuits on qsetFind)
	//     with nothing queued to examine it: a cell of the player's world permanently outside
	//     the simulation, while waterPending(), waterFlowCells() and waterQueueFull() all report
	//     a simulation in perfect health.
	//
	// Comparing the POINTER, not the contents: s_water is keyed by world block coordinate and
	// s_world is a file static that never moves, so identity is exactly the question being asked.
	//
	// Safe to evaluate on the worker thread: it reads s_edit_fn, s_edit_ud and &s_world, and
	// &s_world is a link-time constant. worldSetEditHook is called only at session_start, and
	// workerStop() joins the worker before either goto back to that label — so there is never a
	// live worker thread while world.c's statics are being written.
	if (w != &s_world) return;

	// v1.8.19. The second filter, and the one that keeps a rejoin from throwing away the
	// player's own work.
	//
	// A rejoining client's columns are unloaded, so every stored diff for them is queued and
	// then landed by net/networld.c's blockdiffDrain when the column installs — uncapped, one
	// column's whole backlog inside one frame, every write arriving here because applyDrained
	// writes the live world itself. Measured 2026-08-30 with main.c's frame order modelled
	// exactly: 49 columns x 200 diffs is 9800 firings and 68600 pushes against a ring of 1024,
	// which sits at its cap on 33 of 60 frames. A water source the PLAYER places on any of
	// frames 0..46 is evicted before it is ever examined and settles to one wet cell instead of
	// 113. 47 of 60 placements lost, 78.3%.
	//
	// Suppressing every notify during the drain fixes that completely and breaks the other end:
	// water that arrives AS a diff spreads to 1 block instead of 117, which moves the bug from
	// the local player onto the remote one. So the filter is narrow rather than blanket — skip
	// only when there is no water within the radius a notify can reach. Both halves have to be
	// true to skip, and the order matters for cost as well as for meaning:
	// networldReplayingDiffs() is one bool read, so a live edit pays that and nothing else, and
	// is never filtered.
	if (networldReplayingDiffs() && waterReplaySkippable(&s_water, &s_world, x, y, z)) return;

	waterNotify(&s_water, x, y, z);
}

// v1.3.0. net/networld.h's inventory hook: the server has sent its authoritative copy of this
// player's inventory (BS_APP_INV_STATE) and it replaces whatever s_inv currently holds.
//
// Overwriting rather than merging is the whole point — see net/inv_bridge.h on why every local
// inventory change is applied optimistically and sent afterwards. On an unacknowledged UDP
// transport a dropped INV_ACTION leaves this console holding a stack the server never moved,
// and the snapshot is the only thing that ever notices. Merging would preserve exactly the
// divergence the snapshot exists to erase.
//
// No remesh, no UI reset, nothing else to do: scene/ui.c reads s_inv fresh every frame (it owns
// no copy of it) and the main loop re-reads inventoryHeldItem() every frame too, for reasons its
// own comment already gives. A rejected snapshot (an item id this build does not have) leaves
// s_inv untouched — invBridgeApplyState's comment covers what that means and why it is still the
// right failure.
static void onInvState(void* userdata, const NetworldInvState* state)
{
	(void)userdata;
	(void)invBridgeApplyState(&s_inv, state);
}

// v1.8.5. What the boot actually costs, read off the console instead of derived on paper.
//
// Every memory figure this project quotes is arithmetic: the mesh pool is 9,199,616 bytes
// because four constants multiply to that, the world store is 12 MB because world/budget.h
// says so, and the 24,348,874 byte total is a sum of fourteen such derivations. The only
// hardware reading on record — 25.83 MB of linear free — was taken before v1.7.0, when the
// pool was 4.24 MB. v1.8.5 is being asked how much further a New 3DS can see, and answering
// that by subtracting one unverified number from another is how this project has been wrong
// before. So mark the free heap at each milestone and let the console do the subtraction.
//
// The arithmetic, the sign handling and the formatting live in app/memprobe.c, which is
// <3ds.h>-free and host-tested (tests/memprobe_test.c, 73 checks, red-armed two ways). All
// that happens here is the two libctru readings and the SD write.
static MemProbe s_memprobe;

// Three readings, because no two of them can see the same allocations.
//
// linearSpaceFree/vramSpaceFree cover linearAlloc and VRAM — the mesh pool, the framebuffers,
// the atlas. mallinfo().uordblks covers the ordinary heap, which is where net/blockdiff.h's
// 1.02 MB pending-edit store and most of the rest of the game live. The first Azahar run of
// this probe is what proved the third column necessary: linearSpaceFree() read 0 through
// hwInit, psInit, netInit and networldInit and only became meaningful after screenInit, so
// against linear alone the entire pre-graphics boot looked free.
static void memProbeBootMark(const char* name)
{
	const struct mallinfo mi = mallinfo();
	memProbeMark(&s_memprobe, name,
	             (unsigned)linearSpaceFree(), (unsigned)vramSpaceFree(),
	             (unsigned)mi.uordblks);
}

// Written once, at the end of the boot, to sdmc:/blocksmith/memprobe.txt.
//
// watchdogWriteFile rather than stdio for the same reason app/gputest.c uses it: it takes the
// raw-FS path the hang report uses and sizes the file to exactly the length written, so a
// shorter report cannot leave the tail of a longer previous one trailing it and be read as
// this boot's numbers. A refused format is reported in the file rather than swallowed — an
// empty memprobe.txt with no explanation is the failure this whole module exists to avoid.
//
// The path is absolute and must stay that way. watchdog.c's fileWrite() opens ARCHIVE_SDMC at
// the root and creates "/blocksmith" as a directory, then passes this string straight to
// FSUSER_OpenFile — so a bare "memprobe.txt" is not a file in the game's folder, it is a path
// the open silently refuses. That was the first version of this line, and the emulator boot
// that was supposed to prove the probe works produced no file at all and no error: bootid.txt
// and frames.csv were freshly written by the same boot, so the game plainly ran and reached
// here. Matches app/gputest.c's SELFTEST_REL, which is "/blocksmith/selftest.txt" for exactly
// this reason.
#define MEMPROBE_REL  "/blocksmith/memprobe.txt"

// Room for the stage table plus the region summary appended after it. memProbeFormat is still
// handed only MEMPROBE_REPORT_MAX, so its "never truncates" contract is unchanged and the host
// suite still tests the real cap; the tail is main.c's, and only main.c can write it because
// osGetMemRegion* is libctru and memprobe.c is deliberately free of <3ds.h>.
#define MEMPROBE_REGION_TAIL 512

// v1.8.18, lane IDX-HARDEN task C. A second tail, appended after the region summary, for the
// index-buffer/tier-arena physical addresses chunk_render.c now measures at boot — see
// chunkRenderIndexVirt/Phys and chunkRenderTierVirt/Phys in scene/chunk_render.h for what this
// closes. Same reasoning as MEMPROBE_REGION_TAIL just above: chunk_render.c owns the pointers
// and osConvertVirtToPhys, main.c owns the SD write, and neither wants the other's dependency.
#define MEMPROBE_CHUNK_TAIL 512

static void memProbeBootFlush(void)
{
	static char report[MEMPROBE_REPORT_MAX + MEMPROBE_REGION_TAIL + MEMPROBE_CHUNK_TAIL];
	const int len = memProbeFormat(&s_memprobe, report, MEMPROBE_REPORT_MAX);
	if (len <= 0) {
		static const char kFailed[] = "boot memory probe: memProbeFormat refused\n";
		watchdogWriteFile(MEMPROBE_REL, kFailed, sizeof(kFailed) - 1);
		return;
	}

	// The stage table alone answered the wrong question. It measures the LINEAR HEAP, and a
	// New 3DS CXI boot (ExHeader SystemModeExt : 124MB, game correctly reporting "New 3DS")
	// read 31,679,488 bytes of linear free — byte for byte the same as the same build booted
	// as a .3dsx under the homebrew launcher. libctru reserves its linear heap once at
	// startup, so linearSpaceFree() reports free space INSIDE that reservation and never the
	// console's budget. The extra New 3DS memory is real but sits outside it, unmapped, and
	// invisible to every reading taken so far.
	//
	// osGetMemRegionSize(MEMREGION_APPLICATION) is the actual ceiling, and the gap between it
	// and the linear heap is the honest answer to how much further the render distance can
	// go. Printed as three regions because APPLICATION is the only one the game may spend,
	// and quoting a total that includes SYSTEM and BASE would overstate it.
	// APPLICATION came back size == used, free == 0, which is not "the console is full": the
	// process claims its whole region at startup and then sub-divides it. So the split is what
	// has to be printed, not the region total. envGetLinearHeapSize() and envGetHeapSize() are
	// the two halves libctru carved out, and mallinfo's arena/fordblks say how much of the
	// app-heap half has actually been touched. Anything in the region that neither half claims
	// is memory the game paid for and cannot currently reach — which is the number that decides
	// how much further the render distance can go.
	const u32 app_size  = osGetMemRegionSize(MEMREGION_APPLICATION);
	const u32 app_used  = osGetMemRegionUsed(MEMREGION_APPLICATION);
	const u32 lin_size  = envGetLinearHeapSize();
	const u32 heap_size = envGetHeapSize();
	const struct mallinfo mi = mallinfo();

	const int n = snprintf(report + len, MEMPROBE_REGION_TAIL,
	                       "memory regions at end of boot\n"
	                       "  APPLICATION  size %10lu used %10lu free %10lu\n"
	                       "  SYSTEM       size %10lu\n"
	                       "  BASE         size %10lu\n"
	                       "  linear heap  size %10lu free %10lu\n"
	                       "  app heap     size %10lu arena %10lu free-in-arena %10lu\n"
	                       "  unclaimed by either heap %10ld\n"
	                       "  vram         free %10lu\n",
	                       (unsigned long)app_size, (unsigned long)app_used,
	                       (unsigned long)(app_size - app_used),
	                       (unsigned long)osGetMemRegionSize(MEMREGION_SYSTEM),
	                       (unsigned long)osGetMemRegionSize(MEMREGION_BASE),
	                       (unsigned long)lin_size, (unsigned long)linearSpaceFree(),
	                       (unsigned long)heap_size, (unsigned long)mi.arena,
	                       (unsigned long)mi.fordblks,
	                       (long)app_size - (long)lin_size - (long)heap_size,
	                       (unsigned long)vramSpaceFree());

	// A short tail is still worth writing; a mangled one is not. Anything that did not fit is
	// dropped rather than half-printed, and the table above is unaffected either way.
	const size_t total = (n > 0 && n < MEMPROBE_REGION_TAIL)
	                   ? (size_t)len + (size_t)n
	                   : (size_t)len;

	// v1.8.18, lane IDX-HARDEN task C. chunkRenderInit() already succeeded by the time this
	// function is ever reached — main() returns 1 above (the "shader/atlas/pool init FAILED"
	// screen) before memProbeBootMark("chunkRenderInit")/memProbeBootFlush() are ever called —
	// so these four accessors are always real boot-time readings here, never the "not measured"
	// 0 they return before init or after chunkRenderExit.
	//
	// idx0_off is GPUREG_INDEXBUFFER_CONFIG's own offset field at first=0 — phys(s_shared_
	// indices) - 0x18000000, VRAM base, which is what BufInfo_Init always sets base_paddr to —
	// computed here rather than left for whoever reads memprobe.txt to subtract by hand, because
	// that number is the entire reason this probe exists: C3D_DrawElements adds 2*first to it
	// for every real draw, and if THIS value already sits near the top of whatever field width
	// the register actually has, some in-range `first` later in the session pushes it over.
	const uint32_t idx_phys = chunkRenderIndexPhys();
	const uint32_t idx0_off = idx_phys - 0x18000000u;

	const int n2 = snprintf(report + total, MEMPROBE_CHUNK_TAIL,
	                        "chunk render buffers (lane IDX-HARDEN task C)\n"
	                        "  index buf    virt 0x%08lx phys 0x%08lx  offset@first=0 0x%08lx\n"
	                        "  tier S arena virt 0x%08lx phys 0x%08lx\n"
	                        "  tier M arena virt 0x%08lx phys 0x%08lx\n"
	                        "  tier L arena virt 0x%08lx phys 0x%08lx\n",
	                        (unsigned long)chunkRenderIndexVirt(), (unsigned long)idx_phys,
	                        (unsigned long)idx0_off,
	                        (unsigned long)chunkRenderTierVirt(0), (unsigned long)chunkRenderTierPhys(0),
	                        (unsigned long)chunkRenderTierVirt(1), (unsigned long)chunkRenderTierPhys(1),
	                        (unsigned long)chunkRenderTierVirt(2), (unsigned long)chunkRenderTierPhys(2));

	// Same "short tail beats a mangled one" rule as the region summary just above — if this
	// block did not fit, the file still ends with a complete, honest region summary rather than
	// a truncated address block.
	const size_t total2 = (n2 > 0 && n2 < MEMPROBE_CHUNK_TAIL) ? total + (size_t)n2 : total;
	watchdogWriteFile(MEMPROBE_REL, report, total2);
}

int main(void)
{
	// FRZ-PROBE. Before literally anything else, so a stage.txt from a PREVIOUS boot can never
	// be misread as this boot's history. See app/stage_probe.h; a no-op unless built with
	// -DBS_STAGE_PROBE=1.
	BS_STAGE_INIT();
	BS_STAGE("MAIN_ENTER");

	// Step 8.5, first of everything. It only writes this thread's TLS exception slot — no
	// gfx, no FS, nothing that can itself fail — so every line after it is covered. A crash
	// during screenInit is exactly the kind that is impossible to diagnose otherwise,
	// because there is not yet a screen to print the error on.
	crashInit();

	// Second of everything, and before anything that branches on the console model or
	// starts a thread. hwInit() asks APT_CheckNew3DS once and, on a New 3DS, asks for the
	// 804 MHz clock and the L2 cache -- so it has to run before workerStart() picks a core,
	// before the render-distance default is read at step 8.4, and before gpuTestPreflight()
	// writes a report that names the model. APT is already open here: libctru's __appInit
	// runs before main(), which is the same reason the old APT_CheckNew3DS calls further
	// down worked. See app/hw.h for why the clock is asked for here and not in app/sleep.c.
	hwInit();

	// v1.8.9 audio. Mixer/channel setup only -- no romfs access happens here, so this cannot
	// collide with updaterInit()'s romfsInit() further down. audioLoad() mounts and unmounts
	// romfs per-read internally for exactly that reason; see audio/audio.h.
	audioInit();

	// The baseline, and taken here rather than at the top of main() on purpose: hwInit() is
	// what asks a New 3DS for its 124 MB memory mode, so a reading taken before it would be
	// the Old 3DS heap on both models and every cost below would be measured against the
	// wrong ceiling.
	//
	// The linear and VRAM columns will both read 0 here and stay there until screenInit(),
	// which is not a fault and is why the heap column exists — see memProbeBootMark. The
	// mark is still taken, because the heap column IS meaningful this early and the two
	// claims v1.8.5 is reclaiming are both made before graphics come up.
	memProbeReset(&s_memprobe);
	memProbeBootMark("boot");

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

	// v1.8.5: this mark used to justify itself by the 1.02 MB the pending block-diff store cost
	// at boot whether or not the player ever opened Multiplayer. That is no longer true, and the
	// note stays rather than being deleted because the mark's VALUE changed and a reader
	// comparing against an older probe log needs to know why. The store is now allocated lazily
	// on the first server packet that needs it and freed on leaving a session, so this mark
	// should read about 1,064,976 bytes lighter than it did before — measured off .bss
	// (1,725,944 → 660,968 in arm-none-eabi-size), not estimated. The mark is kept separate from
	// the net stage precisely so that drop is visible here instead of being averaged into it.
	// Not yet confirmed on a console: nothing has run the memprobe since the change.
	memProbeBootMark("net+networld");

	// FRZ-PROBE. hwInit/audioInit/psInit/netInit/networldInit have all run; screenInit (the
	// GPU/citro3d claim) has not. If the freeze is in one of the basic 3DS services rather
	// than in anything game-specific, this is the last marker on the card.
	BS_STAGE("SERVICES_INIT");

	screenInit();
	metricsInit();
	memProbeBootMark("screenInit+metrics");

	// FRZ-PROBE. screenInit() is gfxInit + C3D_Init — the GPU/citro3d context itself. If the
	// freeze is inside citro3d's own setup, SERVICES_INIT is the last marker and this one
	// never appears.
	BS_STAGE("GPU_INIT");

#if BS_GPU_TESTS && BS_GPU_PREFLIGHT
	// Before the title screen and before a world exists, so a console that fails one of these
	// says so without the player having to reach the freeze first. Costs a few seconds of boot
	// and writes sdmc:/blocksmith/selftest.txt. Diagnostic builds only. See app/gputest.h.
	gpuTestPreflight();
#endif

	// Marked unconditionally, so the report has the same five rows in every build and a
	// release build's row reads 0 rather than being absent. gputest.c holds 512 KB of linear
	// scratch for the whole run in a preflight build; this is the row that will show whether
	// that is really what it costs.
	memProbeBootMark("gpuTestPreflight");

	// v1.8.5. The mesh pool is claimed once and never resized, so it is sized for the widest
	// radius this console will ever be OFFERED, not the radius selected now — which does not
	// exist yet, because optionsLoad is still ~90 lines below. hwInit() has already run.
	if (!chunkRenderInit(renderDistMaxFor(hwIsNew3ds()))) {
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

	// The row this whole probe was added for. chunkRenderInit claims all three mesh tier
	// arenas at boot, sized from the COMPILE-TIME RENDER_DIST_MAX rather than from the
	// radius the player has selected — so this number is what raising the ceiling would
	// charge an Old 3DS whose owner never touches the slider. The formula says 9,199,616
	// bytes at radius 3 (confirmed exactly by tests/mesh_pool_bytes_test.c). If the console
	// disagrees, the console is right.
	memProbeBootMark("chunkRenderInit");
	memProbeBootFlush();

	// FRZ-PROBE. chunkRenderInit() just succeeded: the shader program is bound, the terrain
	// atlas is uploaded to the GPU and the three mesh-tier arenas are claimed (see this
	// function's own "shader/atlas/pool init FAILED" message above for what it covers).
	BS_STAGE("ATLAS_SHADER_INIT");

	// v1.8.9 weather rendering. Same phase as chunkRenderInit above -- a one-time GPU-object
	// claim (shader/texture/vertex buffer) made once for the process, before session_start,
	// and freed once at app_shutdown by weatherDrawExit() beside chunkRenderExit() -- but NOT
	// the same failure-handling shape: chunkRenderInit's failure above is fatal because the
	// world literally cannot be drawn without it, whereas rain/snow is not load-bearing, the
	// same not-fatal footing as crackOverlayInit/highlightInit/playerModelInit below. A
	// failed weatherDrawInit leaves weatherDrawShouldDraw() false forever (nothing was armed
	// to draw), so weatherDrawDraw() degrades to a no-op rather than crashing.
	(void)weatherDrawInit();

	// Step 8.3. The GUI's batch and its font, brought up together because they are one
	// thing in practice: spriteRect() samples the solid white texel that lives *inside*
	// the font texture, so a batch without a font can draw nothing at all. A failure here
	// is not fatal the way chunkRenderInit's is — a game with no HUD is worse than a game,
	// but it is still a game — so it is recorded and the boot continues.
	const bool ui_ok = spriteInit() && fontInit();

	// Brought up in both builds on purpose, even though only a BS_BOTTOM_UI build draws
	// with it. The batch claims a shader and 96 KB of linear memory, and a console build
	// that skipped that would be measuring a different memory profile from the one that
	// ships — which is exactly the kind of difference that makes an Old 3DS headroom figure
	// wrong. The cast is what keeps -Wunused-variable quiet in the console build.
	//
	// 2026-09-03: this said 48 KB. The real figure is 96 KB and has been since the batch was
	// doubled — gfx/sprite.c:43 states the arithmetic (1024 * 4 * 24) and says in the same
	// breath that it is "up from 48 KB", so this line was quoting the number the OTHER file
	// had already retired. Corrected rather than deleted because the sentence exists to make
	// an Old 3DS headroom figure trustworthy, and it was doing the exact opposite: understating
	// the reservation by half, in the one comment a reader would go to for it.
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
	//
	// v1.8.19: bracketed with the same boot-heap probe as chunkRenderInit above, so
	// memprobe.txt records free heap either side of probeCertBundle()'s mbedtls_x509_crt_parse()
	// call inside updaterInit() -- the one live untested hypothesis for the CACERT_BADFILE
	// reports is OOM during that parse (121 certs / 188,900 bytes of PEM).
	memProbeBootMark("preUpdaterInit");
	updaterInit(true);
	memProbeBootMark("postUpdaterInit");

	// v1.4.0: sleep preserver (APT hook) and battery telemetry (PTMU), both process-wide for
	// the whole life of the app, so they sit here with the other boot-time services rather
	// than inside the session loop. The UI-side one-time inits come with them.
	sleepInit();
	batteryInit();
#if BS_BOTTOM_UI
	remapUiInit();
	debugMenuUiInit();
	bsDebugRegister();
#endif

	// ── Step 8.4: settings, then the menu ────────────────────────────────────────────
	//
	// Hoisted out of the BS_WORLD_GEN block below, where it used to be the only caller,
	// because the options default needs the same answer — see the first-run block.
	const bool new_3ds = hwIsNew3ds();

	// optionsSave writes into sdmc:/blocksmith, which nothing has created yet at this point
	// in the boot: saveWorldDir() makes it, and that does not run until genStart. Made here
	// so a first run can actually keep the render distance it just worked out. mkdir's return
	// is ignored for the same reason it is inside saveWorldDir — EEXIST is the normal answer,
	// and the only thing that settles whether the card is usable is trying to write to it,
	// which optionsSave then does.
	mkdir("sdmc:/blocksmith", 0777);
	mkdir("sdmc:/blocksmith/fog", 0777);

	// Immediately after the folder exists and before anything can write a diagnostic into it.
	// See diagFenceBoot: this is what makes "the file is on the card" mean "this boot wrote it".
	diagFenceBoot();

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

	// v1.8.9 audio. The saved volume, applied once opts is loaded; and the three sound effects,
	// loaded once here so every later cue can reference them without re-reading romfs. audioLoad
	// mounts/unmounts romfs per call internally (see audio/audio.h and audio.c's own comment) and
	// never disturbs updaterInit's mount if one happens to still be up, so this is safe
	// regardless of call order against updaterInit() above.
	//
	// v1.9.0: the returned ids are CAPTURED into audio/audio_sfx.h's slots instead of being
	// discarded and assumed to be 1, 2, 3 in load order. The assumption was true and was not
	// safe — audioLoad consumes no id when it fails, so a single missing or corrupt .bsnd
	// renumbers everything behind it and blocks start breaking with the place sound, silently.
	// audio_sfx.h states the whole argument. Registering is also what stops the reverse
	// mistake: a fourth sound added to this block cannot renumber the three below it.
	audioSetMasterVolume(opts.audio_volume);
	audioSfxRegister(SFX_BLOCK_BREAK, audioLoad("romfs:/sfx/block_break.bsnd"));
	audioSfxRegister(SFX_BLOCK_PLACE, audioLoad("romfs:/sfx/block_place.bsnd"));
	audioSfxRegister(SFX_FOOTSTEP,    audioLoad("romfs:/sfx/footstep.bsnd"));
	audioSfxRegister(SFX_HURT,        audioLoad("romfs:/sfx/hurt.bsnd"));
	audioSfxRegister(SFX_DEATH,       audioLoad("romfs:/sfx/death.bsnd"));
	audioSfxRegister(SFX_EAT,         audioLoad("romfs:/sfx/eat.bsnd"));
	audioSfxRegister(SFX_CRAFT,       audioLoad("romfs:/sfx/craft.bsnd"));
	audioSfxRegister(SFX_SPLASH,      audioLoad("romfs:/sfx/splash.bsnd"));
	audioSfxRegister(SFX_UI_TAP,      audioLoad("romfs:/sfx/ui_tap.bsnd"));

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
	// The previous pass may have left these true. Nothing else in THIS file survives a lap.
	s_server_session = false;
	s_remap_open     = false;
	s_debug_open     = false;

	// ...but other modules' per-session state does, and one piece of it was being missed.
	// v1.6.0 F2: the block registry is per-session — genStart() below calls registryFreeze()
	// in single player exactly as it does in a session, and registryInitCore() is the only
	// thing that ever clears that flag. The server exit twenty lines from the bottom of this
	// function reached it by accident, through netDisconnect() -> networldInit(); the
	// single-player `if (quit_to_title) goto session_start;` right below it reached nothing,
	// so a player who played single player, backed out here and then joined a server had
	// every one of that server's block definitions refused by registryRemoteApply() for the
	// whole session — each one an invisible hole they walked through and fell into. Only a
	// reboot cleared it, which is what made it look intermittent.
	//
	// Here rather than at either exit, for two reasons. There are two `goto session_start;`
	// statements and nothing stopping a third, and a rule attached to the exits has to be
	// remembered once per exit forever; this label is the one place that means "a new
	// session is beginning", so a future exit gets the reset for free. And it is the only
	// placement early enough: a joining client's BS_APP_REGISTRY_DEFS arrive while the
	// player is still on the multiplayer screen inside the title screen below, so a reset
	// done at world entry would land after the rows it was meant to make room for.
	//
	// app/session.c, host-tested by app/session_test.c — which walks this exact sequence and
	// also reads this file to check the call below is still here. Nothing in main.c can be
	// linked into a host binary, and that is why this defect had no coverage at all.
	sessionBegin();

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
	// else has been editing rather than a private copy of the same terrain. Note that s_world is
	// empty at this point — worldReportBuild() frees its proving grid before returning and the
	// worker has not started — so this call deliberately does nothing but register the pointer.
	// See networldSetWorld()'s own comment.
	BOOT_BEGIN();
	networldSetWorld(&s_world);

	// Paired with the line above, and deliberately re-registered on every pass through
	// session_start rather than once at boot: netDisconnect() (net/bsnet.c) calls
	// networldInit(), which clears the hook along with everything else, so a hook set once at
	// boot would be live for the first server session and silently gone for every one after it
	// — the invisible-edits bug back again, but only on a rejoin, which is the worst possible
	// version of it to have to reproduce. Harmless in single player: nothing ever fires it.
	networldSetEditHook(onRemoteEdit, NULL);

	// v1.8.0, and cleared on the same schedule and for the same reason: a column index left
	// over from the previous session names a column in a world that no longer exists, and the
	// first frame of the new one would relight whatever now happens to sit at that index.
	relightqInit(&s_relightq);

	// v1.8.0 task 21, reset here for a third version of the same reason: tick 0 must be the
	// first tick of THIS world. Carrying a count over from the previous session would hand every
	// periodic mechanic a phase inherited from a world that no longer exists, and carrying the
	// accumulator over would spend the loading screen's worth of banked real time as a burst of
	// catch-up ticks on the first frame the player can see.
	tickClockInit(&s_tickclock, TICK_MAX_CATCHUP_DEFAULT);

	// v1.8.0 task 22, and the fourth version of the same reason: the flow map is keyed by world
	// block coordinate, so an entry carried over from the previous session would claim that a cell
	// of the NEW world is flowing water at a level nothing there ever set — and, because absence
	// means source, the stale entry would also stop a real source at that coordinate from
	// spreading. Cleared with the world, not at boot.
	waterInit(&s_water);
	worldSetEditHook(onWorldEdit, NULL);

	// v1.8.0 task 22b, and both of these have to be re-done HERE and not once at boot: waterInit
	// memsets the simulation, so the level hook installed for the previous world is gone with it.
	// The renderer's pointer survives (s_water is a static and never moves), but it is set beside
	// the hook so the two can never drift apart — a renderer pointing at a live simulation with no
	// level hook would draw the first height correctly and then never update it again.
	waterSetLevelHook(&s_water, onWaterLevel, NULL);
	chunkRenderSetWater(&s_water);

	s_tps            = 0.0f;
	s_tps_mark_tick  = svcGetSystemTick();
	s_tps_mark_count = 0;

	// The inventory hook is NOT registered here alongside the edit hook, though it was in the
	// v1.3.0 release and that is exactly what made the feature do nothing. It is registered
	// further down, immediately after s_inv is loaded or initialised — see that call site for
	// why the order matters and why registering it at this point loses the server's inventory
	// twice over.
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
	// v1.7.1 task 48b. The world-load instrument is armed HERE, which is as close as this file
	// gets to "the player picked a world": the title screen has returned, the name is in
	// s_world_name, and nothing on the card has been touched for this world yet. Everything
	// from this line to the frame runLoadingScreen hands over on is inside the wall time.
	// See debug/loadprof.h, and note that it replaces a measurement that was never valid —
	// the CSV's frame_ms across a world entry is wall clock across the whole menu, not a load.
	loadprofBegin(s_world_name, 0);

	BOOT_BEGIN();
	// One saveWorldDir() call answering both questions below, rather than two. It mkdirs
	// three levels and write-probes the card — most of genStart's measured 117 ms — so a
	// second call to ask the same directory a second question is a real cost, not a
	// tidiness point. NULL in a server session, which worldHasBeenPlayed() reads as "new"
	// and playerPoseLoad() refuses outright: the server owns that world and this console
	// gains no file from it.
	const char* const boot_dir = s_server_session ? NULL : saveWorldDir();

	// FRZ-PROBE. saveWorldDir() just mkdir'd the world's save directory and write-probed the
	// card — the save system is up (or, in a server session, deliberately not touched at
	// all: boot_dir is NULL there and this still marks the point that decision was made).
	BS_STAGE("SAVE_INIT");

	const bool world_played_before = worldHasBeenPlayed(boot_dir);
	BOOT_END("worldHasBeenPlayed");
	// v1.7.1 task 48b. The distinction steve's report turns on — "creating a world" against
	// coming back to one that has been changed — recorded on the row rather than inferred from
	// it, so a load.csv full of rows can be sorted by it without guessing.
	loadprofSetReloaded(world_played_before);

	// v1.7.1 task 46b. The saved pose is read HERE, before genStart, and not down beside the
	// networldSavedPose() replay where it is finally used. That is not a preference; it is
	// what makes the restore correct at all.
	//
	// genStart() centres the streaming ring, runLoadingScreen() fills exactly that ring, and
	// worldGet() answers BLOCK_AIR for a chunk that is not resident. So worldStandingY() over
	// a column outside the ring returns the y it was given and silently does nothing. With
	// the ring centred on the spawn column the way this line used to read — genStart(0, 0) —
	// a pose restored anywhere but spawn would have been un-stuck against air and the player
	// would have come back inside their own blocks: exactly the bug task 46 just fixed,
	// wearing a new hat, and with a green test suite over it.
	//
	// Reading the pose first and centring the ring on ITS column is the fix. Nothing else
	// changes: the loading screen already waits for the whole ring and its geometry, so by
	// the time the un-stick runs below, the restored column is installed on the same
	// guarantee the spawn column has always had. A world with no pose file — every world on
	// the card today — falls back to (0, 0) and takes a byte-for-byte identical path.
	PlayerPose saved_pose;
	const bool have_saved_pose = playerPoseLoad(&saved_pose, boot_dir);

	// v1.8.13 SURV-WIRE. Read HERE, beside the pose, and off the same `boot_dir` — not off a
	// second saveWorldDir() call — so the directory and its lifetime are the ones the pose
	// already proved correct rather than a second opinion that could disagree.
	//
	// The server rule comes free rather than being restated: boot_dir is NULL in a server
	// session (line above's comment), and survival.c's dirUsable() refuses a NULL directory,
	// so survivalLoad() declines for exactly the reason playerPoseLoad() does — the server
	// owns that world's state and this console must not read a local file for it. The server
	// arm that DOES restore vitals in a session is the networldPlayerMeters() read down beside
	// the pose replay, which is where the server's own copy arrives.
	//
	// survivalInit FIRST and unconditionally: survivalLoad's contract is that a false leaves
	// *out untouched, so the caller must already hold a valid struct rather than reading one
	// the load half-filled. A world with no survival.dat — every world on the card today —
	// starts at full health and full hunger, which is what survivalInit gives.
	Survival  survival;
	FallTrack falltrack;
	survivalInit(&survival);
	fallTrackInit(&falltrack);
	(void)survivalLoad(&survival, boot_dir);

	// Centred on the restored column, or on the spawn column (0, 0) when there is no pose to
	// restore — the same column playerInit puts the feet in below. From here on the ring
	// follows the player (genFollow).
	BOOT_BEGIN();
	const bool worker_ok = genStart(have_saved_pose ? genColumnOf(saved_pose.x) : 0,
	                                have_saved_pose ? genColumnOf(saved_pose.z) : 0);
	BOOT_END("genStart");

	// FRZ-PROBE. genStart() just returned: it allocates the new World, centres the streaming
	// ring and starts the worker lane(s) (app/worker.c's threadCreate — see WORKER_LANE0 /
	// WORKER_LANE1 for whether either lane actually got scheduled). worker_ok says whether it
	// was refused outright; either way, control reached here. The whole block sits behind its
	// own #if, not just the BS_STAGE() call inside it — otherwise the snprintf() and its
	// format-string literal would still be compiled into every shipped build, which is
	// exactly what this flag promises never to do.
#if BS_STAGE_PROBE
	{
		char sp_buf[48];
		snprintf(sp_buf, sizeof(sp_buf), "NEW_WORLD_ALLOC worker_ok=%d", (int)worker_ok);
		BS_STAGE(sp_buf);
	}
#endif

	// v1.8.3. genStart() returning false has always meant "there is no world to walk into", and
	// until now nothing acted on it. worker_ok gated the sleep hooks below and fed WORLD_INTACT()
	// on the world report, and that was all: control carried straight on into runLoadingScreen(),
	// which then waited for columns from a worker that was never started until scene/loading.c's
	// LOADING_STALL_FRAMES (900 frames, ~15 s at the measured 59.83 fps) turned it into "no
	// progress" and offered a way out. So a refused world cost the player a quarter-minute stare
	// at a bar stuck on 0% and then dropped them out of the game entirely, with the only
	// explanation printf'd to a console a retail 3DS has no way to show.
	//
	// Named separately from worker_ok rather than testing worker_ok twice, because the two are
	// different questions that happen to share an answer today: worker_ok is "is there a thread
	// writing this world", which is what the sleep hooks and the world report want, and this is
	// "should the player be put back on the menu", which is what the three lines below want. They
	// are pinned together here, in one place, instead of at each use.
	const bool world_refused = !worker_ok;
	if (world_refused && !s_world_refused_why) {
		// genStart sets a reason on every one of its three false returns, so this is belt and
		// braces for a fourth added later without one. A refusal with no sentence is still far
		// better than the stall it replaces — but it must never be a refusal with a NULL that
		// the menu code below would have to guard.
		s_world_refused_why = "world could not be started";
	}

	// v1.6.0. From here until workerStop() below there is a world in memory and a worker
	// able to write it, which is exactly the window in which closing the lid should flush.
	// Registered on worker_ok and nowhere else: with no worker, sleepFlushOneColumn() would
	// be handing columns to a thread that was never started. Outside this window both hooks
	// stay NULL and app/sleep.c's ONSLEEP does nothing — which is what makes closing the lid
	// on the title screen or the world list safe.
	if (worker_ok) {
		s_sleep_flush_cursor = 0;
		s_sleep_pose_done    = false;   // v1.7.1 task 46b: a new world starts owing a pose write
		sleepSetFlushHook(sleepFlushOneColumn);
		sleepSetLeaveHook(sleepLeaveSession);
		// v1.8.9 entity foundation. There is now a world in memory, which is the same gate
		// the sleep hooks above are keyed on -- entityCapFor sizes the fixed pool per console
		// the same way the render distance default above does.
		entityWorldInit(&s_entities, entityCapFor(hwIsNew3ds()));
		// v1.8.14 animals. Seeded beside the store the spawner writes into, and under the same
		// worker_ok gate, because both facts this line needs are established by it: genStart()
		// has run (it is what returned worker_ok) and genStart() is the only place a WorldGen
		// is built, so s_gen.seed is the real seed of the world about to be walked into rather
		// than whatever a zeroed static held. Seeding outside this branch would seed from 0 on
		// a world that failed to open.
		//
		// Salted through rngMix so the animal stream is not the cave salts' or the terrain
		// hashes' sequence wearing a different name. rngSeed() itself already refuses 0, which
		// is xorshift32's fixed point, so a seed-0 world is handled without a special case.
		rngSeed(&s_animal_rng, rngMix(s_gen.seed ^ 0xA14Du));
	}
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
	//
	// v1.8.3. Not entered at all for a refused world, and that is the half of the fix the player
	// actually feels. There is no worker, so genColumnsInstalled() can only ever answer 0 and
	// every frame spent in here is a frame spent waiting for something that is not coming; the
	// screen's own stall detector would eventually say so, ~15 s later, in a sentence about
	// columns rather than about the world's version. quit_requested instead takes the same route
	// a player who gave up on a stalled load already takes — one pass of the game loop below,
	// which breaks immediately — with the difference that quit_to_title is set from
	// world_refused, so the loop's teardown ends on the menu instead of on app_shutdown.
	if (!world_refused &&
	    !runLoadingScreen(ui_ok,
	                       s_server_session   ? "JOINING WORLD"
	                       : world_played_before ? "LOADING WORLD" : "CREATING WORLD",
	                       s_server_session ? netServerAddress() : s_world_name))
		quit_requested = true;

	if (world_refused) quit_requested = true;

	// v1.7.1 task 48b. The load is over: the ring is full, its geometry is built and the next
	// frame is one the player can move in. Stopped and written here rather than at the top of
	// the game loop so nothing the first playable frame does is charged to the load.
	//
	// Two outputs, deliberately. The CSV row goes beside frames.csv and is what gets read off
	// the card afterwards with no emulator involved; the printf is what a -DBS_BOTTOM_UI=0
	// probe build shows on the console. Neither can fail in a way that matters — loadprofWrite
	// ignores card errors, and a build with no console prints into a stream nobody reads.
	loadprofEnd();
	loadprofWrite(LOADPROF_PATH);
	{
		char lp[768];
		loadprofFormat(lp, sizeof lp);
		printf("%s", lp);
	}

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

	// v1.8.3. The other arm of the world_refused declared beside genStart above, for the same
	// reason menu_quit has one: everything that reads it — quit_to_title's initialiser and the
	// two saves on the way out — is below this #endif and is compiled in both configurations, so
	// a name that existed in only one of them would break the build nobody runs by hand.
	//
	// Always false here, and that is not a stub. Refusing is a decision about a world's stored
	// generator version, and a BS_WORLD_GEN=0 build has no stored world at all: handbuiltFill()
	// builds the same fixed test scene into memory every launch, there is no world directory,
	// no stamp, and nothing on a card that this build could be inconsistent with. handbuilt_ok
	// covers the one way that can fail and already feeds WORLD_INTACT() below.
	const bool world_refused = false;
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

	// v1.8.1 task 50. The crack overlay, on exactly the same not-load-bearing footing as the
	// highlight above: a break still takes its time and still completes without it, the
	// player just cannot see how far along it is. crackOverlayDraw is a no-op when this
	// fails, so nothing downstream needs the answer. It loads its own texture — do not call
	// crackAtlasInit here as well, crackOverlayInit already has.
	(void)crackOverlayInit();

	// v1.8.9 particle system. Same not-load-bearing footing as the crack overlay right above,
	// and paired with it directly: both are per-session GPU claims made every time control
	// reaches here (including a loop back to session_start after quitting to title), and
	// particlesInit() also resets the pool so a fresh world join never inherits a previous
	// session's leftover splashes -- see gfx/particles.h's own comment on particlesInit.
	(void)particlesInit();

	// Remote player bodies, on the same not-load-bearing footing as the highlight above: a
	// failure here costs the ability to see other players, which is worse in a session and
	// completely irrelevant in single player, and either way is not a reason to refuse to
	// boot. playerModelDraw checks its own ready flag, so nothing downstream needs this.
	(void)playerModelInit();
	(void)entityModelInit();

	// v1.8.8 NEON BIOME BORDERS. Same not-load-bearing footing as the three above: a failure
	// costs a debug overlay nobody has switched on, and biomeBorderDraw checks its own ready
	// flag, so nothing downstream needs the answer.
	//
	// Called in every boot even though the overlay starts off, rather than lazily on the first
	// switch-on. Deliberate: a linearAlloc on the frame the player presses A would be a
	// hitch — and, worse, a failure at that moment would be a debug feature that silently does
	// nothing when asked for, which is the hardest kind of bug to believe. It costs one DVLB
	// parse of a shader already in the binary and 36,864 bytes of linear memory.
	(void)biomeBorderDrawInit();

	// The v1.4.0 minimap claimed its 64x64 texture and loaded this world's fog-of-war here.
	// Removed entirely in v1.5.1, not repaired: scene/minimap.c:251 called worldGet(NULL, wx,
	// 0, wz) and world/world.c:44 dereferences w->slots[i] with no NULL guard, so the first
	// pixel it ever drew was a data abort on a real console. The spec's touch-screen map is
	// a fresh build, not a fix of this one. Fog files already written to
	// sdmc:/blocksmith/fog/*.bin are left where they are — deliberately, no cleanup code.

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
	// v1.7.1 task 46. The generator's height is the STARTING guess, not the answer.
	//
	// steve, 2026-08-24: "whenever I create a world, make a bunch of changes, exit quick to
	// title, and then try reload the world — it loads in, but the chunk I'm in doesn't get
	// loaded in. The other chunks do."
	//
	// Nothing was failing to load. worldgenHeight() is a pure function of seed and generator
	// version and never reads s_world, so it returns the surface the terrain would have if
	// nobody had ever edited it. A player who built anything at the spawn cell and came back was
	// therefore placed at the ORIGINAL ground height — inside their own blocks. physics.c has no
	// un-stick (resolveX/Y/Z each move and snap back on bodyBlocked), so they could not walk
	// out; player.c put the camera at body.y + 1.62, also inside solid blocks; and back-face
	// culling drew nothing for the geometry around them while the columns further out rendered
	// normally. That is exactly what "the chunk I'm in doesn't get loaded in" looks like from
	// inside it.
	//
	// worldStandingY steps UP from the guess rather than scanning down from the sky. On an
	// unedited world the first test passes and the result is identical to what this line used to
	// be, so a fresh world cannot regress — where a top-down scan would put the player on the
	// canopy of any tree that grew at (8,8), which worldgenHeight deliberately ignores. Built a
	// tower at spawn: on top of it. Built a roof five blocks up: still on the ground under it,
	// where they were. Dug down: unblocked at the guess, and falls into their own hole, which is
	// what already happened.
	//
	// Safe to read the world here: runLoadingScreen has returned, so the spawn column is
	// installed and worldGet is answering from real blocks rather than from an absent column.
	//
	// NOT done: persisting the player's real pose in single player, the way a server session
	// already does through networldSavedPose(). Better long-term — you should come back where
	// you left, not at spawn — but it is a save-format change and would do nothing for the
	// worlds steve already has. On the roadmap as its own task.
	const int spawn_y = worldStandingY(&s_world, spawn_x, spawn_z,
	                                   worldgenHeight(&s_gen, spawn_x, spawn_z));
#else
	const int spawn_x = 18, spawn_z = 18;
	const int spawn_y = handbuiltHeight(spawn_x, spawn_z);
#endif

	Player player;
	playerInit(&player, (float)spawn_x + 0.5f, (float)spawn_y, (float)spawn_z + 0.5f,
	           C3D_AngleFromDegrees(135.0f), C3D_AngleFromDegrees(35.0f));

	// v1.5.0 server-side persistence, the pose half. The server volunteered this player's
	// saved position and facing in its join-time PLAYER_STATE — which landed while the title
	// screen was still pumping networldUpdate(), long before `player` existed above, so
	// net/networld.c retained it and it is replayed here rather than at arrival. This is the
	// same arrival-order fix the inventory snapshot got (see networldSetInvHook below): the
	// read-back happens after init so a server that had something saved gets the last word
	// over this client's own spawn choice, and reconnecting puts the player back where they
	// logged out, facing the same way, instead of respawning at spawn.
	//
	// Consulted exactly once, here — not polled per frame afterwards. A PLAYER_STATE that
	// somehow arrives later than this line is missed, which is the same accepted race
	// networldWorldSeed() has always had (read once at genStart): both packets are sent by
	// the server immediately after JOIN, so anything slow enough to lose them never made the
	// handshake usable in the first place. False — single player, old server, fresh spawn —
	// leaves the playerInit above untouched.
	//
	// v1.7.1 task 46b hangs the single-player restore off the same decision, as its other
	// arm, so there is one place in this file that answers "where does the player start"
	// rather than two that can disagree. The server arm stays first and stays authoritative:
	// in a session the card holds no pose for this world by construction (boot_dir is NULL
	// above), so the two can never actually compete — the ordering is stated rather than
	// relied on.
	{
		float px, py, pz, pyaw, ppitch;
		if (networldSavedPose(&px, &py, &pz, &pyaw, &ppitch)) {
			playerInit(&player, px, py, pz, pyaw, ppitch);
		}
#if BS_WORLD_GEN
		else if (have_saved_pose) {
			// The un-stick, and it runs LAST and on the restored y only — task 46's fix
			// applied to task 46b's new input. A pose written at y=40 whose y=40 has since
			// been filled in (a generator-version change, a region file that failed its CRC
			// and regenerated, or a falling block that landed after the write) would put the
			// player inside solid blocks with no way to walk out, which is the failure this
			// whole pair of tasks exists to stop.
			//
			// Safe to read the world here for the same reason the spawn line above is, and
			// only because genStart() was centred on this pose's column: runLoadingScreen()
			// has returned, so the column is installed and worldGet() is answering from real
			// blocks. x and z keep their fractional parts — only y moves.
			PlayerPose p = saved_pose;
			playerPoseUnstick(&s_world, &p);
			playerInit(&player, p.x, p.y, p.z, p.yaw, p.pitch);
		}
#endif
	}

	// v1.8.13 SURV-WIRE, and the vitals half of the exact same decision the pose block above
	// makes: in a server session the server had the last word on where this player is, so it
	// gets the last word on their health and hunger too. net/networld.h retained the meters
	// block from the join-time PLAYER_STATE for precisely this — its own comment says the
	// block is "held only so those systems can hook in without a wire redesign", and this is
	// that hook.
	//
	// Consulted exactly once, here, for the same accepted race networldSavedPose() has: both
	// arrive immediately after JOIN, long before this line. A false — single player, an old
	// server, or a fresh spawn the server has nothing saved for — leaves the survivalLoad
	// above untouched, and the two can never compete for real because boot_dir is NULL in a
	// session and the local load already declined.
	//
	// The wire carries these as 0..20 floats; Survival keeps them as 0..20 bytes. Rounded and
	// clamped rather than cast, because the cast alone would turn a server's 19.7 health into
	// 19 and a malformed 300.0 into whatever a u8 truncation happens to give.
	{
		const NetworldPlayerMeters* const m = networldPlayerMeters();
		if (m) {
			const float h = m->health, g = m->hunger;
			survival.health = (uint8_t)(h < 0.0f ? 0.0f
			                          : h > (float)SURVIVAL_MAX_HEALTH ? (float)SURVIVAL_MAX_HEALTH
			                          : h + 0.5f);
			survival.hunger = (uint8_t)(g < 0.0f ? 0.0f
			                          : g > (float)SURVIVAL_MAX_HUNGER ? (float)SURVIVAL_MAX_HUNGER
			                          : g + 0.5f);
		}
	}

	// Re-armed AFTER every playerInit above, not before them. playerInit places the body at a
	// restored or spawned y, and a tracker still holding a peak from the previous placement
	// would score the difference as a fall and take the damage for it on the first frame the
	// player touches the ground. Cheap insurance: this is two stores.
	fallTrackInit(&falltrack);

	// v1.8.13 SURV-WIRE, the send half. What the last PLAYER_REPORT put on the wire, so the
	// frame loop can send on CHANGE rather than every frame — the server persists a report
	// that differs (save_player_state), so sixty identical reports a second would be sixty
	// card writes a second on the far end for no new information.
	//
	// Seeded from the values just restored, not from a sentinel, so simply walking into a
	// world does not immediately send a report echoing back what the server just told us.
	uint8_t last_sent_health = survival.health;
	uint8_t last_sent_hunger = survival.hunger;

	Interact it;
	interactInit(&it);
	// v1.8.7. Hands scene/interact.c the same worklist onRemoteEdit above already pushes to
	// and the frame loop below already drains (relightDrain, ahead of chunkRenderDrainDirty),
	// so a LOCAL break or place stops paying for a full 32768-cell column relight on the very
	// frame the player pressed the button. It was the last caller in the tree still doing
	// that inline. Set once and never cleared: s_relightq outlives every edit, and interact.c
	// treats a NULL queue as "relight inline", which is only what the host tests want.
	interactSetRelightQueue(&s_relightq);

	// v1.8.17. Without this line the whole broke-contents mechanism in scene/interact.c is
	// dead code: it defaults to no reader, reports nothing, and every furnace's contents are
	// destroyed exactly as they were before. Registered here, beside the relight queue, for
	// the same reason — both are this file's state being lent to interact.c for the duration.
	interactSetBrokeContentsFn(mainFurnaceBrokeContents);

	// v1.9.0 audio. The footstep cadence, beside `it` because it has the same lifetime: one
	// visit to one world. A break and a place each have an instant to hang a sound on inside
	// scene/interact.c and are wired there; walking has none, so this accumulates horizontal
	// travel and fires every SFX_FOOTSTEP_STRIDE blocks. Declared here rather than as a file
	// static so the backward `goto` that starts a new session cannot carry the previous world's
	// half-finished stride, or its last position, into the new one — a fresh session would
	// otherwise see the spawn as one enormous step. (The teleport clamp in audio_sfx.c would
	// discard that step anyway; this makes it not arise rather than rely on the guard.)
	AudioFootsteps footsteps;
	audioFootstepsReset(&footsteps);

#ifdef BS_WATER_VIS_TEST
	// VERIFICATION ONLY. Never defined by the Makefile, never in a release build — it exists
	// so one claim can be looked at, and it is the only claim task 22b has that no host test
	// can settle: that the eight flow levels are visibly DIFFERENT heights on a screen.
	// world/water_test.c proves the levels are 8-d at manhattan distance d, and
	// world/water_mesh_test.c proves every unequal pair emits exactly one side quad running
	// from the lower surface to the higher one — but both of those are assertions about
	// numbers in a vertex buffer, and "it renders as steps" is not a number.
	//
	// Built here rather than driven to, because driving to it failed. Six scripted Azahar
	// sessions tried to dig a natural shoreline open at seed 1592 and all six failed on camera
	// geometry — the aim ray ended up in the sky, in a pit wall, or six blocks inland — while
	// never once contradicting the code. So the arrangement is put where the camera already
	// points instead: the spawn faces yaw 135 degrees, camera.c:110 makes that forward vector
	// (sin yaw, -cos yaw) = (+x, +z), so a pad six blocks out on both axes lands in the middle
	// of frame one, tilted 35 degrees down, with no input needed at all.
	//
	// worldSet and nothing else. Every block goes through world.h's edit hook exactly as a
	// player's own pick would, so waterNotify queues the source, the spread runs on the normal
	// per-frame waterTick budget in the main loop, and the remesh arrives through onWaterLevel.
	// Nothing here reaches past the path being verified, which is the entire point: a harness
	// that called waterSettle directly would prove the simulation and not the game.
	{
		const int vy  = spawn_y - 1;                     // the surface being stood on
		const int vcx = spawn_x + 6, vcz = spawn_z + 6;  // in front, per the yaw above
		for (int dx = -8; dx <= 8; ++dx) {
			for (int dz = -8; dz <= 8; ++dz) {
				const int x = vcx + dx, z = vcz + dz;
				// Stone, not the sand or grass around it, so the pad's own edge is
				// unmistakable in the screenshot and a water quad cannot be confused
				// with the terrain it sits on.
				(void)worldSet(&s_world, x, vy, z, BLOCK_STONE);
				for (int h = 1; h <= 12; ++h)
					(void)worldSet(&s_world, x, vy + h, z, BLOCK_AIR);
			}
		}
		// A plank pillar BS_WATER_VIS_TEST blocks tall, one step in front of the player. It is
		// there to answer, from the screenshot alone, the two questions the mode-2 run could
		// not otherwise settle: did this block run at all, and what value did the compiler
		// actually see? Mode 2 came back with untouched natural terrain twice in a row, which
		// is consistent both with "the block never ran" and with "it ran and something put the
		// terrain back", and no amount of staring at the two pictures separates those.
		// Parked at the pad's far corner rather than the near one it started at. One step in
		// front of the player it filled the whole screen with plank texture and hid the very
		// thing the run was taking a picture of; twenty blocks out it is a thumbnail that says
		// the same thing.
		for (int h = 1; h <= BS_WATER_VIS_TEST; ++h)
			(void)worldSet(&s_world, vcx + 8, vy + h, vcz + 8, BLOCK_PLANKS);
#if BS_WATER_VIS_TEST == 2
		// Mode 2, the waterfall: the same sunken 7x7 basin with a 3x3 shaft six blocks deep
		// through the middle of it and a single source in one corner. The water spreads across
		// the basin floor, reaches the shaft, falls, and pools at the bottom. Two claims here
		// that mode 1 cannot make — that a falling column draws at FULL height instead of as a
		// stack of shrinking slabs, and that a fall turns into a spreading pool where it lands.
		//
		// DOWN a shaft rather than off a raised shelf, which is what this mode tried first.
		// The spawn looks 35 degrees DOWNWARD and the script can only flatten that by about
		// eight degrees per nudge, so a shelf six blocks up sat off the top of the screen in
		// every frame of the run. A hole in the ground is the one shape this camera is already
		// pointed into.
		//
		// The spreading water sits ON TOP of the pad, not in a sunken basin. The basin version
		// came back looking empty: world/water_test.c's testSunkenBasin proves the same
		// arrangement wets 31 of its 49 cells in six ticks on the host, so the water was there
		// — but a one-block basin is in its own rim's shadow, and a flow cell of level 1 or 2 is
		// a film a couple of pixels thick. On the open pad, which is the arrangement mode 1's
		// bright blue rings already came out of, there is nothing to shade it.
		for (int dx = -1; dx <= 1; ++dx) {
			for (int dz = -1; dz <= 1; ++dz) {
				for (int h = 0; h <= 6; ++h)
					(void)worldSet(&s_world, vcx + dx, vy - h, vcz + dz, BLOCK_AIR);
				(void)worldSet(&s_world, vcx + dx, vy - 7, vcz + dz, BLOCK_STONE);
			}
		}
		// Three blocks diagonally out from the shaft mouth, on the pad, offset toward the
		// camera so the fall is seen across the shaft rather than through its far wall.
		// Three and not four: the nearest mouth cell is then four steps away by the manhattan
		// rule the pour follows, so water arrives at the lip at level 4 — half a block deep,
		// which reads on a 240-pixel screen. At four out it arrives at level 2, a film two
		// pixels thick, and the picture would be arguing about nothing.
		//
		// NOT over the mouth itself: testSunkenBasin's second arm measured what that does — a
		// source whose cell below is open can still go down, so by water.h's feeding rule it
		// goes down and nothing else, pours straight into the shaft and spreads not one cell.
		// Correct behaviour, and exactly the wrong picture for this test.
		(void)worldSet(&s_world, vcx - 3, vy + 1, vcz - 3, BLOCK_WATER);
#else
		// Mode 1: a walled 7x7 basin, a 3x3 block of SOURCES in the middle of it, and the
		// stepped rings that spread from them. Both halves of the claim in one frame.
		//
		// The rings alone cannot settle it, which is what the first two runs of this test
		// found out. In a pour on flat ground the level is 8 minus the manhattan distance, so
		// EVERY orthogonally adjacent pair differs by exactly one — and a mesher that
		// correctly emits a 1/8 step between unequal levels draws the identical picture to a
		// mesher that wrongly walls off every pair of water cells. Run 1 produced exactly that
		// ambiguous grid of isolated tiles and proved nothing either way.
		//
		// The block of sources is the control that separates them. Sources are level 8 by
		// definition (world/water.h: absence from the map means source), so those nine cells
		// are one height with twelve internal faces between EQUAL levels, and a correct
		// mesher must merge every one of them away into a single unbroken plateau. Stepped
		// rings around a seamless middle is a picture only the correct mesher can draw; the
		// always-a-wall bug still rules a grid across the middle.
		//
		// The basin is what keeps the whole thing inside one screenshot. Run 2 used a 5x5
		// source patch with nothing to stop it, and it spread its full seven blocks in every
		// direction — a pool wider than the frame at the spawn distance, with the plateau lost
		// somewhere in the middle of it. Bounded to 7x7 the pour is 49 cells carrying levels 8
		// down to 4: five distinct heights in a shape that fits.
		//
		// SUNK one block rather than walled by a rim on top of the pad, which is what run 3
		// tried. A rim is a block tall and stands between the camera and the water, and at the
		// spawn's 35-degree downward pitch it filled the frame with its own near corner and
		// hid every cell behind it. Sinking the floor to vy-1 puts the lip flush with the pad,
		// so the near edge occludes nothing and the water surface — vy plus one to eight
		// eighths — sits between the pad top and one block below it.
		for (int dx = -3; dx <= 3; ++dx) {
			for (int dz = -3; dz <= 3; ++dz) {
				(void)worldSet(&s_world, vcx + dx, vy - 1, vcz + dz, BLOCK_STONE);
				(void)worldSet(&s_world, vcx + dx, vy, vcz + dz, BLOCK_AIR);
			}
		}
		for (int dx = -1; dx <= 1; ++dx)
			for (int dz = -1; dz <= 1; ++dz)
				(void)worldSet(&s_world, vcx + dx, vy, vcz + dz, BLOCK_WATER);
#endif
		// worldSet does NOT remesh, and this harness has to say so itself. world.h's edit hook
		// — onWorldEdit above — only notifies the water simulation; every in-game path that
		// changes a block calls chunkRenderTouch on its own line afterwards, and nothing here
		// had. Mode 2 is what found it: its blocks were all present, and the crosshair proved
		// it by drawing a selection box around a plank pillar that was not on the screen, but
		// no chunk had been asked to rebuild so the entire arrangement was invisible.
		//
		// Mode 1 was only ever correct by accident. Its water spreads inside the pad's own
		// chunks, every flow-level change calls onWaterLevel, and onWaterLevel calls
		// chunkRenderTouch — so the pad appeared as a side effect of the water moving. Mode 2
		// puts its shelf six blocks up, in the chunk above, so the pad's chunks were never
		// touched and nothing was drawn at all.
		for (int dx = -8; dx <= 8; ++dx)
			for (int dz = -8; dz <= 8; ++dz)
				for (int h = -8; h <= 12; ++h)   // -8 reaches mode 2's shaft floor
					chunkRenderTouch(&s_world, vcx + dx, vy + h, vcz + dz);
	}
#endif

	// Step 8.2. Loaded after genStart, because that is what creates the world directory this
	// file lives beside; saveWorldDir() returns NULL if the card is not writable, and
	// inventoryLoad refuses a NULL directory, so the guard is the same test the region writer
	// uses rather than a second opinion about the SD card. inventoryLoad always leaves `s_inv`
	// valid — a missing or corrupt file is an empty inventory, not an error to handle here.
	// NULL in a server session, for the same reason the worker's directory is: the server owns
	// the world, this console keeps nothing from it, and saveWorldDir() would mkdir the very
	// directory the session is supposed not to create. One variable covers both ends — the
	// load here and the inventorySave on the quit path below are both already guarded on it —
	// so a joined session starts with an empty inventory and writes none back — the server owns
	// what this player is carrying, and the hook registered immediately below is what delivers it.
	watchdogPhase(WD_PHASE_HANDOFF_SAVE);
	const char* const inv_dir = s_server_session ? NULL : saveWorldDir();
	if (inv_dir) inventoryLoad(&s_inv, inv_dir);
	else         inventoryInit(&s_inv);

	// v1.8.15, on exactly the same guard and for exactly the same reasons. inv_dir is reused
	// rather than calling saveWorldDir() a second time: two calls could in principle disagree,
	// and a world whose inventory persists but whose furnaces do not would be a strange,
	// hard-to-report halfway state. One directory decision, both files.
	//
	// blockStateLoad, like inventoryLoad, always leaves the table valid — a missing or corrupt
	// sidecar is an empty table, not an error to handle here. Its return value says whether a
	// file was actually read, which nothing needs yet; ignoring it is deliberate rather than an
	// oversight, and is why the (void) is written out.
	if (inv_dir) (void)blockStateLoad(&s_blockstate, inv_dir);
	else         blockStateInit(&s_blockstate);

	// v1.7.1 task 46b. What the lid-close flush hook needs to write a pose, handed over now
	// that both halves exist — the Player was built above and the directory on the line
	// above that. Pointers, not a copy: the hook fires at an arbitrary moment during play and
	// must read where the player IS, not where they were when the world opened. Both are
	// cleared in the teardown beside sleepSetFlushHook(NULL), which is what stops the hook
	// from ever reaching a Player that has gone out of scope.
#if BS_WORLD_GEN
	s_sleep_player   = &player;
	s_sleep_pose_dir = inv_dir;
#endif
	// v1.8.13 SURV-WIRE. Handed over beside the pair above and for the identical reason, but
	// OUTSIDE the BS_WORLD_GEN guard: the bottom-screen draw reads this in every build, not
	// just a generated-world one, whereas the lid-close hook only exists under that guard.
	// Cleared in the same teardown.
	s_survival = &survival;

	// Registered HERE, and not up with the edit hook, because both halves of the order matter and
	// v1.3.0 got both of them wrong.
	//
	// Too early is wrong: JOIN completes inside runTitleScreen(), which pumps networldUpdate()
	// itself, so the server's unprompted join-time INV_STATE has already arrived and been decoded
	// by the time control reaches this function's top. Registering up there meant the hook was
	// still NULL when the snapshot landed, and it was dropped.
	//
	// Too late is also wrong, and is why this sits below the two lines above rather than above
	// them: net/networld.c replays the retained snapshot the instant a hook is registered, so
	// registering before inventoryInit() would have that replay immediately overwritten by the
	// empty inventory a server session initialises. Landing after it, the replay is the last
	// writer and the console shows what the server is actually holding.
	//
	// Re-registered on every pass through session_start for the same reason the edit hook is:
	// netDisconnect() calls networldInit(), which clears both. Harmless in single player — no
	// INV_STATE ever arrives, so there is nothing to replay and nothing to fire.
	networldSetInvHook(onInvState, NULL);
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

	// Set by the pause menu's Quit row, read once the play loop has torn the world down. It
	// cannot simply fall through to app_shutdown the way leaving the loop used to: the only
	// exits that existed before this menu were HOME and a lost server session, and both of
	// those genuinely end the session. Quit does not — it means "put me back on the title
	// screen", which is where choosing another world and quitting the app both live.
	//
	// v1.8.3. Initialised from world_refused rather than from `false`, and that is the whole
	// reason the refusal takes this route instead of a `goto` from up beside genStart. "Put me
	// back on the title screen" is exactly what a world that would not open needs, the code that
	// does it correctly already exists, and every line of teardown between the loop and that goto
	// runs on the way — pauseMenuClose, the sleep hooks (whose own comment at the clear already
	// anticipates "a hook that was never set because genStart() failed"), workerStop,
	// watchdogStop, networldSetWorld(NULL), chunkRenderReleaseAll, worldExit. A jump that skipped
	// them is what main.c's comment beside sleepSetFlushHook(NULL) calls a crash on lid-close.
	bool quit_to_title = world_refused;

#if BS_BOTTOM_UI
	// Last frame's touch_down, kept across iterations so the screens layered over the pause
	// panel can be handed a touch EDGE instead of a touch LEVEL.
	//
	// The bug this fixes: touch_down below comes from hidKeysHeld() & KEY_TOUCH, which is
	// true for every frame the stylus is on the glass. debugmenu_ui.c and remap_ui.c both
	// treated it as a press, so holding the stylus on a toggle row flipped it at 60 Hz and
	// whichever state it happened to land on when the player lifted off was a coin flip.
	//
	// Derived here rather than by giving each UI module its own static touch_prev, for the
	// reason the touch read itself is here (see the UiInput comment below): the frame's
	// input is read in one place. It also keeps the two modules' host tests able to drive
	// a single press as one call with touch_down=true, instead of having to model an edge
	// detector's internal state.
	//
	// `touch` itself is NOT edge-filtered — drawBottomUi still gets the level, because
	// scene/ui.c edge-detects against its own touch_prev and taking that away would break
	// the hotbar.
	bool touch_prev = false;
#endif

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
		// v1.8.1 task 50. Read here beside `down` rather than at the edit, so both masks come
		// from the same hidScanInput() and cannot describe two different instants. Held is what
		// a timed break needs: the edge only says the button went down, not that it is still
		// down two seconds later.
		u32 held = hidKeysHeld();
		// quit_requested comes from the loading screen (a stalled load the player gave up on,
		// or the system closing us while it was up). Taken here rather than skipping the loop
		// entirely so the exit runs exactly the path a START quit runs — inventory saved, dirty
		// columns flushed, worker stopped — instead of a second, less-tested teardown.
		if (down & KEY_START || quit_requested) break;

		// v1.4.0. Battery is polled at most once a second internally, so this is free on 59
		// of 60 frames.
		//
		// The `if (sleepShouldSkip()) continue;` that stood on the next line from v1.4.0 to
		// v1.6.0 is gone, and it never once ran its `continue`. It was written for a "sleep
		// frame" that does not exist: disassembling libctru's aptMainLoop() shows it calling
		// aptHandleSleep(), which fires APTHOOK_ONSLEEP, parks the process in
		// LightEvent_Wait for the whole of the sleep, fires APTHOOK_ONWAKEUP and returns —
		// all of it before aptMainLoop() hands control back to this loop body. The flag was
		// therefore always false again by the time this line could read it. app/sleep.h
		// carries the disassembly and the rest of the reasoning; the lid-close work now
		// happens in the hook, where the console actually is when the lid is shut.
		batteryPoll();

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
		// The rising edge of the above, for the remap screen and the debug menu — see
		// touch_prev's declaration before the loop. One press is one true, however long the
		// stylus stays down.
		const bool touch_press = touch.touch_down && !touch_prev;
		touch_prev = touch.touch_down;
#endif

		// SELECT opens the pause menu. It used to toggle 3D directly (step 7.6); that toggle
		// moved onto the menu's options page, which is where a setting belongs and where it
		// now sits beside the render distance and the memory readout. gfxSet3D is still
		// called from exactly one place — just below, off the menu's answer instead of off
		// the button — for the reason step 7.6 gave: leaving 3D on while only one eye is
		// drawn shows the right eye a stale buffer.
		if (down & KEY_SELECT) pauseMenuToggle();

		int  dist_step     = 0;
		bool stereo_toggle = false;
		PauseAction pause_action = PAUSE_ACTION_NONE;
#if BS_BOTTOM_UI
		// While the remap screen or the debug menu owns the frame, the pause menu underneath
		// must not also read the pad — D-pad would move its cursor invisibly.
		if (!s_remap_open && !s_debug_open)
#endif
			pause_action = pauseMenuInput(down, &dist_step, &stereo_toggle);
		if (stereo_toggle) {
			s_stereo = !s_stereo;
			gfxSet3D(s_stereo);
		}
		if (dist_step != 0) {
			// genSetRadius clamps and re-meshes the ring live; opts.render_dist is the value
			// that survives a reboot, so both have to move, and reading s_mesh_radius back
			// after the call rather than trusting the requested value is what makes the saved
			// setting the clamped one. Saved immediately rather than on menu exit — the
			// player can leave this menu by quitting the world, and a setting they just
			// watched take effect must not then be forgotten.
			genSetRadius(s_mesh_radius + dist_step);
			opts.render_dist = s_mesh_radius;
			optionsSave(&opts, TITLE_OPTIONS_PATH);
		}
		// v1.8.9 audio. Same "save immediately, not on menu exit" reasoning as render_dist
		// above: the pause menu's own volume slider already wrote the live mixer value via
		// audioSetMasterVolume, and this only mirrors it into opts once so it survives a reboot.
		if (pauseMenuTakeVolumeChanged()) {
			opts.audio_volume = audioGetMasterVolume();
			optionsSave(&opts, TITLE_OPTIONS_PATH);
		}
		if (pause_action == PAUSE_ACTION_QUIT) {
			quit_to_title = true;
			break;
		}
#if BS_BOTTOM_UI
		if (pause_action == PAUSE_ACTION_REMAP) {
			remapInit(&s_remap, &opts);
			// The screen's own cursor/scroll are statics that outlive a visit, so without
			// this the second visit opens halfway down the list where the last one left
			// off. remapUiInit also arms the guard that throws away this frame's already-
			// spent A press — the same A that just chose the "Controls" row, which without
			// it fell straight through into "Press a button..." on row 0. This is the
			// mirror of debugMenuUiOpen() just below.
			remapUiInit();
			s_remap_open = true;
		}
		if (pause_action == PAUSE_ACTION_DEBUG) {
			debugMenuUiOpen();
			s_debug_open = true;
		}
#endif

		// A paused world does not tick. Everything from here to the draw is gated on this:
		// the player does not move, terrain does not stream in, and the memory figures the
		// options page is showing hold still while they are being read. Networking is the
		// deliberate exception — see the netUpdate() block below.
		const bool paused = pauseMenuOpen();

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

		// v1.8.9 audio. Once a frame, ahead of any positional audioPlayAt call this frame:
		// the listener has to be current before a sound's pan/attenuation can be computed
		// against it, and audioUpdate is what retires finished voices and mixes the frame.
		audioSetListener(player.body.x, player.body.y, player.body.z, player.cam.yaw);
		audioUpdate();

#if BS_WORLD_GEN && !BS_FLY
		// Step 7.7. L and R change the render distance. They are free in walking mode — only
		// the free-fly camera uses them, for up and down — and this is the one control the
		// setting can have until step 8.4 gives the game an options screen to hold it. It is
		// wired up rather than left for 8.4 because a setting that cannot be changed cannot be
		// verified: the two distances have to be reachable in one run for the fog and the ring
		// to be judged against each other.
		if (!paused && (down & KEY_L)) genSetRadius(s_mesh_radius - 1);
		if (!paused && (down & KEY_R)) genSetRadius(s_mesh_radius + 1);
#endif

		// v1.8.13 SURV-WIRE. Eat the SELECTED hotbar slot, on the edge and never on held —
		// world/survival.h takes a slot rather than an item id precisely so the slot the player
		// is pointing at is the one that empties, and a held button would drain a stack in a
		// third of a second.
		//
		// Outside the BS_WORLD_GEN && !BS_FLY guard above so it works in every build that has a
		// survival state to feed, and behind inputKey(ACTION_EAT) rather than a literal so the
		// remap screens actually reach it. inputKey answers 0 for an action the map does not
		// know, and `down & 0` is false, so a build whose options never loaded simply cannot eat
		// rather than eating on every button.
		//
		// The return is deliberately dropped: survivalEat refuses at full hunger and on a slot
		// that is not food, both of which are "nothing happened", and there is no HUD message
		// system to report either through. The hunger bar not moving IS the feedback.
		// ONE flag for both eat paths, and it is what stops them doubling up. ACTION_EAT is
		// rebindable, so a player is free to put it on the very key ACTION_PLACE already holds
		// — and then a single press would satisfy the dedicated check here AND the held-food
		// check below, eating two apples for one button press. Neither path is wrong on its
		// own; the flag is what makes them exclusive.
		bool ate_this_frame = false;

		if (!paused) {
			const uint32_t eat_key = inputKey(ACTION_EAT);
			if (eat_key && (down & eat_key)) {
				ate_this_frame = survivalEat(&survival, &s_inv, s_inv.selected_hotbar);
				// v1.9.0 SFX-WIRE-MAIN. Gated on the return, not the press, so a refusal
				// (full hunger, non-food slot) stays silent same as the hunger bar does.
				if (ate_this_frame) audioPlay(audioSfxId(SFX_EAT), AUDIO_PRIO_NORMAL, 1.0f);
			}
		}

#if BS_WORLD_GEN
		// Take delivery of at most one generated column, before anything reads the world
		// this frame. One column is all the worker can hand over per install
		// (app/worker.h). Meshing what that made ready happens further down, with the
		// edit queue, so the two share one budget rather than one each — see
		// DRAIN_BUDGET_MS. Install stays here because it is what puts ground under the
		// player.
		//
		// v1.7.1 task 49 (install half). This comment used to end "it is a memcpy, not a
		// mesh: 15.3 ms over 125 columns, 0.12 ms each", and that sentence was wrong twice
		// over. It has not been a memcpy since step 9.2a made Chunk opaque — app/worker.c's
		// workerInstall now DECOMPRESSES each chunk of the staged column into a flat buffer
		// and hands it to worldSetChunkAll, which used to re-derive the buffer's palette
		// from scratch twice and then pack it with a linear palette search per cell. And
		// 0.12 ms was about 36x under: the 3522-frame Azahar capture that opened this task
		// found mesh_ms over its 4.0 ms budget on 25% of frames in two distinct classes,
		// and of the 207 frames in the expensive 8-14 ms class, 173 were frames where the
		// mesh queue grew by exactly 5 or 6 — the signature of one column install queueing
		// its chunks. Of the 683 cheap-class frames the queue grew on ZERO. The install is
		// ~4.36 ms of those frames, and because install_ms is folded into work_ms below
		// (see the work_ms line further down) it has been published as the CSV's mesh_ms
		// all along, which is why it read as mesh cost and hid here behind this comment.
		//
		// Where that time went, profiled on the host over a real 7x7-column streaming pass
		// (49 columns, 300 chunks, medians of 5 passes, us per installed COLUMN):
		// worldSetChunkAll 93.3 (89.3%), chunkDecompressAll 9.4 (9.0%), worldExit of the
		// staging world 1.2, genQueueReadyColumns 0.6, lightColumnCopy 0.1 — total 104.5.
		// Nothing outside worldSetChunkAll was worth touching.
		//
		// world/chunk.h's ChunkPlan is the fix: one walk of the 4096-cell buffer instead of
		// three. Paired and interleaved against the unchanged code, four rounds each, the
		// worldSetChunkAll total per pass went 4513.7 / 4733.8 / 4121.9 / 4201.0 us before
		// against 1049.6 / 971.1 / 1107.4 / 1061.1 us after (4.2x), and the whole install
		// path 104.5 -> 29.8 us per column (3.5x). The stored chunks are identical: an
		// FNV-1a over the form byte, all 4096 decompressed cells, the chunkEncode payload
		// bytes and the byte count of every chunk of the pass, plus the budget peak, is
		// a8ed2826ea80538c in both arms.
		//
		// So the honest console figure is: MEASURED ~4.36 ms per install frame before,
		// PROJECTED ~1.4 ms after (4.36 x 0.31, the host ratio). The projection has NOT
		// been measured on hardware — that needs another Azahar or console capture, and
		// until one is taken this number is arithmetic, not evidence.
		//
		// None of this touches genDrainMesh's guarantee that at least one chunk is built
		// per drain whatever the budget says (see DRAIN_BUDGET_MS at the top of this file).
		// Install has never been inside that budget: it is timed here, separately, before
		// the drain runs, and the fix only makes the same call cheaper.
		//
		// v1.8.8. Bounded by the LANE COUNT rather than fixed at one. With two generator lanes
		// there can be two finished columns waiting, and workerInstall() takes exactly one per
		// call — so a fixed single install would leave the second lane parked on its claim,
		// unable to take another job until the following frame, and the second lane would
		// deliver at most the same one column per frame the first already did. The bound is
		// workerLanes() and not a constant because that is the real ceiling: a lane holds its
		// claim until its result is installed, so there can never be more than one column
		// waiting per lane, and on an Old 3DS this is literally the v1.8.7 loop with one
		// iteration. The install cost per column is unchanged; the worst case is that this
		// frame pays for two of them instead of one.
		watchdogPhase(WD_PHASE_INSTALL);
		const u64 t_install = svcGetSystemTick();
		if (!paused) for (int i = 0; i < workerLanes() && genInstallOne(); i++) { }
		// v1.8.10. Once per frame — ages every outstanding hole (world/genretry.h) by one frame
		// and resubmits whichever have backed off long enough, bounded to at most one
		// resubmission per pending column per call, same as the loading loop's call to this.
		if (!paused) genRetryTick(&s_gen_retry, s_center_cx, s_center_cz, s_area_radius,
		                           genRetrySubmit, NULL);
		// v1.8.11. The mesh-side ledger, ticked on the same schedule and under the same pause
		// gate. s_mesh_radius, not s_area_radius: this ledger tracks columns that failed to
		// MESH, and the mesh ring is the narrower of the two — ticking it at the area radius
		// would keep re-pushing columns that genQueueReadyColumns is not even walking.
		if (!paused) genRetryTick(&s_mesh_retry, s_center_cx, s_center_cz, s_mesh_radius,
		                           meshRetrySubmit, &s_mesh_retry);
		const float install_ms =
			(float)((double)(svcGetSystemTick() - t_install) / CPU_TICKS_PER_MSEC);
#endif
		watchdogPhase(WD_PHASE_SIM);

		C3D_Mtx view;
#if BS_FLY
		if (!paused) cameraUpdate(&player.cam, metricsFrameMs());
#else
		if (!paused) {
			playerUpdate(&player, &s_world, metricsFrameMs());
			// v1.9.0 audio. Straight after the step that moved the body, so the distance this
			// banks is the distance the player actually travelled this frame and not last
			// frame's. Inside the !paused gate for the same reason the step is: a paused world
			// does not tick, and a player standing still in the menu must not be walking.
			//
			// Under the !BS_FLY branch only. The free-fly camera never touches player.body, so a
			// BS_FLY build would call this with a body that never moves — no travel, no steps,
			// and no footsteps is the right answer for a camera that is not a person walking.
			audioFootstepsUpdate(&footsteps, player.body.x, player.body.y, player.body.z,
			                      player.body.on_ground);

			// v1.8.13 SURV-WIRE. IMMEDIATELY after the step that moved the body, and this
			// placement is forced rather than tidy: world/survival.h says on_ground is set by
			// bodyMove() on the frame the fall was stopped and physics.c zeroes vy in the same
			// branch, so the landing is observable on exactly one frame and nowhere else.
			// Anything between this and playerUpdate that could step the body again would eat
			// the landing; nothing does.
			//
			// Inside the !paused gate with the step it reads, so a paused world cannot land a
			// player who is not moving. Under !BS_FLY only, for the reason audioFootsteps is:
			// the free-fly camera never touches player.body, so there is no fall to score.
			// v1.9.0 SFX-WIRE-MAIN. health_before captured ahead of the call because
			// fallDamageUpdate() itself only returns death, not whether it merely hurt —
			// world/survival.h's health field is the only way to tell those apart.
			// audioPlay, not audioPlayAt: audio_sfx.h has no explicit call for HURT/DEATH the
			// way it does for CRAFT, but CRAFT's own reasoning applies just as well here —
			// being hurt or dying is about the player, not a point in the world.
			const uint8_t health_before_fall = survival.health;
			if (fallDamageUpdate(&falltrack, &survival, &player.body, &s_world)) {
				audioPlay(audioSfxId(SFX_DEATH), AUDIO_PRIO_HIGH, 1.0f);
				survivalRespawn(&player, &survival, &falltrack,
				                (float)spawn_x + 0.5f, (float)spawn_y, (float)spawn_z + 0.5f);
			} else if (survival.health < health_before_fall) {
				// Once per damage event, not scaled by points lost — a multi-point fall gets
				// one stinger, not a burst of them stacking against the mixer.
				audioPlay(audioSfxId(SFX_HURT), AUDIO_PRIO_HIGH, 1.0f);
			}
		}
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

		// THE fix for the hardware freeze. Nothing below this line may run while last
		// frame's draw is still fetching vertices — genFollow reassigns mesh slots and the
		// two drains memcpy over them. See gpuWaitPrevFrame() for the full reasoning.
		//
		// This must sit ahead of genFollow, not merely ahead of the drains: genFollow
		// reaches chunkRenderReleaseColumn, which hands a live slot to a different chunk.
		//
		// v1.8.10: the sync metric moved here with the wait it measures. It used to wrap the
		// C3D_FrameBegin at the draw, which is where the queue wait used to happen; now that
		// gpuWaitPrevFrame performs it, that call early-returns and wrapping it would have
		// printed a permanent ~0.00 ms on the debug overlay while the real wait went unmeasured.
		// A live readout wired to something that no longer measures the thing is worse than no
		// readout, because it reads as evidence.
		metricsSyncBegin();
		watchdogPhase(WD_PHASE_MESH);
		gpuWaitPrevFrame();
		watchdogPhase(WD_PHASE_SIM);
		metricsSyncEnd();

		// Bracketed with its own phase, immediately above. WD_PHASE_SIM was set at the top of
		// this iteration and, before this bracket existed, was never touched again until
		// WD_PHASE_MESH further down — so a hang inside gpuWaitPrevFrame() itself (a bounded
		// gxCmdQueueWait under BS_GPU_TESTS; an unbounded C3D_FrameBegin(C3D_FRAME_SYNCDRAW)
		// without it) reported hang.txt phase SIM, pointing straight at player/camera/aim code
		// that has nothing to do with a GPU wait. This IS "the single most likely place" a real
		// freeze stops, per the bug this whole function exists for, so mislabelling it defeats
		// the point of reporting a phase at all.
		//
		// WD_PHASE_MESH, not a new enumerator: this wait exists to protect exactly the mesh-slot
		// work MESH already names — genFollow's chunkRenderReleaseColumn and the two drains
		// further down, per the comment above. The DRAW phase marker would read more literally
		// (this is where C3D_FrameBegin actually happens on the shipped path) but is not
		// available here: world/framesync_guard_test.c's testNothingEmitsBetween() takes the
		// call setting that phase, spelled exactly once in this file, as its own anchor for
		// where the draw block begins — a second call setting the same phase would give that
		// needle two matches and fail the guard rather than extend it. (Written split apart in
		// this sentence on purpose, so this comment does not itself become a second match.)
		// Restored to SIM immediately after: genFollow, the tick loop, and interactAim/Edit below
		// are genuinely SIM's declared territory (player, camera, ring follow, aim, edit —
		// app/watchdog.h) and must keep reporting as such.

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

		// v1.8.0 task 21. The simulation clock, advanced once per frame by however much real
		// time the last frame actually took. It sits ahead of both drains further down, because
		// a tick that changes blocks — task 22's fluid step — has to have changed them before
		// this frame relights and remeshes, or the change is a frame late and, worse, is lit by
		// the previous frame's light.
		//
		// v1.8.1 task 50 moved it from just below the edit to just above it. A held break now
		// consumes these same ticks, and interactEdit is handed `ticks_now` directly: advancing
		// the clock after the edit would hand every frame the PREVIOUS frame's tick count, so a
		// break would finish one frame late and, on the frame the player let go, would bank
		// ticks against a hold that had already ended. Water does not care which side of the
		// edit it is stepped on — the drains are what it has to precede — so the whole block
		// moves rather than being split in two.
		//
		// Water is stepped once per tick, never once per frame: a pour that spread faster on a
		// console holding 60 fps than on one at 30 would be a different game on the two
		// machines, which is the whole reason the clock exists. Each step is capped at
		// WATER_TICK_BUDGET cells, so the catch-up clamp handing four ticks to one frame costs
		// at most four budgets and not an unbounded drain.
		//
		// v1.8.7. `paused ? 0 :` and not a plain call. The block comment at `const bool paused`
		// further up states the contract — "A paused world does not tick. Everything from here to
		// the draw is gated on this" — and this line was never gated on it, so the simulation
		// clock kept running with the pause menu open and waterTick below kept spreading water
		// while the player sat in a menu. Skipping the ADVANCE rather than the loop is what makes
		// it a real stop: TickClock accumulates the elapsed microseconds it is handed, so feeding
		// it nothing holds the clock still, whereas advancing it and then not consuming the ticks
		// would bank them and dump the whole backlog into the frame the player unpauses on.
		const int ticks_now = paused ? 0
		                             : tickClockAdvance(&s_tickclock,
		                                                (int64_t)(metricsFrameMs() * 1000.0f));

		// v1.8.9 day/night. Advanced by the same n this frame's catch-up loop is about to
		// consume below, not once per iteration -- dayNightAdvance takes the tick COUNT, the
		// same shape tickClockAdvance already returned, per world/daynight.h's own header
		// comment. paused already zeroed ticks_now above, so the guard here is redundant with
		// dayNightAdvance's own n<=0 no-op but kept for symmetry with the loop it sits beside.
		if (ticks_now > 0) dayNightAdvance(&s_daynight, ticks_now);
		//
		// Not separately timed into the CSV row below: MetricsWork lives in app/metrics.h,
		// which this task does not own, and the cost is bounded by construction and measured
		// directly in world/water_test.c against a real body of water.
		for (int t = 0; t < ticks_now; t++) {
			// v1.8.13 SURV-WIRE. In the TICK loop, not the frame loop, because world/survival.h
			// specifies hunger drain and regen as 20 TPS periods (SURVIVAL_HUNGER_PERIOD 1200 =
			// 60 s, SURVIVAL_REGEN_PERIOD 80 = 4 s). Counting those down per rendered frame
			// would make a console holding 60 fps starve twice as fast as one at 30 — the same
			// argument the entity tick immediately below is written from.
			//
			// paused already zeroed ticks_now, so a paused world neither starves nor regenerates.
			//
			// The return is death, and it is wired even though survival.h states it is always
			// false under this version's rules (starvation floors at 1). That is deliberate: the
			// day a lethal tick effect lands — poison, drowning, lava — the handling is already
			// here and the change stays inside survival.c rather than needing a new call site
			// found in this file.
			// v1.9.0 SFX-WIRE-MAIN. Same health_before/else-if pattern as the fall-damage
			// hook above, and for the same reason: survivalTick() itself only returns death.
			const uint8_t health_before_tick = survival.health;
			if (survivalTick(&survival)) {
				audioPlay(audioSfxId(SFX_DEATH), AUDIO_PRIO_HIGH, 1.0f);
				survivalRespawn(&player, &survival, &falltrack,
				                (float)spawn_x + 0.5f, (float)spawn_y, (float)spawn_z + 0.5f);
			} else if (survival.health < health_before_tick) {
				audioPlay(audioSfxId(SFX_HURT), AUDIO_PRIO_HIGH, 1.0f);
			}

			(void)waterTick(&s_water, &s_world, WATER_TICK_BUDGET, onWaterChange, NULL);
			// v1.8.9 entity foundation. Ticked alongside water, once per simulation tick and
			// not once per frame, for the same reason water is: a creature must not move
			// faster on a console holding 60 fps than on one struggling at 30.
			//
			// This comment used to end "No renderer reads s_entities yet -- see the s_entities
			// declaration above." That stopped being true when entityModelDraw(view,
			// &s_entities) was spliced into drawEye(); the declaration's own comment has been
			// corrected too, so neither of them still reads as a live claim.
			//
			// v1.8.14. The think hook and its user pointer were NULL through v1.8.13 and are
			// now animalThink and an AnimalCtx (entity/animal.h). The tick/frame separation
			// above survives the creature layer intact and is not something this call has to
			// arrange: entityTick supplies the hook's `dt_s` itself as `period * ENTITY_DT`
			// (entity.c:255) -- the SIMULATED seconds that entity is about to be advanced by,
			// which for a distance-decimated entity is its own period and not the frame's
			// time. Nothing here passes a frame duration, and there is no frame clock in
			// reach of this hook to pass by accident.
			//
			// The ctx is a stack local, rebuilt each tick. It is two pointers, so a static
			// would buy nothing measurable and would put the decision stream back into file
			// scope inside animal.c, which is precisely what animal.h's AnimalCtx exists to
			// prevent. `gen` is documented there as reserved -- the think does not read it
			// today -- and is passed anyway rather than NULL, so the day a rule wants the
			// heightmap or the biome it is a change inside animal.c and not a new argument
			// every lane has to agree to.
			// v1.8.18 MOB-SPAWN. animalThink alone can no longer be the think hook once the
			// pool can hold zombies and skeletons too -- entityThinkDispatch()
			// (entity/monster.h) is the one function that knows how to route a slot to
			// animalThink() or monsterThink() by kind, so it replaces animalThink() here and
			// takes an EntityDispatchCtx (both context structs bundled) instead of a bare
			// AnimalCtx. The AnimalCtx half is unchanged -- same &s_gen, same &s_animal_rng,
			// and every word of the reasoning above about why `gen` is passed despite being
			// unread today still applies to it.
			//
			// The one-hook alternative -- keep calling animalThink() here and add a SECOND
			// entityTick()-style pass for monsters -- was considered and rejected. entityTick()
			// reaps despawned slots and steps bodies for the WHOLE pool in one pass
			// (entity.c), so calling it twice would double-step every animal's physics the
			// moment a zombie existed, breaking animal movement in a way that would look like
			// a movement bug and not like a wiring bug.
			//
			// MonsterCtx's player_x/z/eye_y feed detection, chase and skeleton line-of-sight
			// on the same tick entityTick() is about to run think() with. The eye height is
			// `player.body.y + PLAYER_EYE`, which is exactly what line 5728 above computes for
			// player.cam.y -- the same existing number, not a new one, and written out the
			// same way rather than reading cam.y so that this does not silently depend on the
			// camera having already been moved this frame.
			//
			// player_damage is an OUT field -- monster.h documents it as "accumulated by
			// think(), read by the caller after entityTick() returns" -- zeroed here every
			// tick and read back immediately below.
			AnimalCtx animal_ctx = { &s_gen, &s_animal_rng };
			EntityDispatchCtx entity_ctx;
			entity_ctx.animal  = animal_ctx;
			entity_ctx.monster = (MonsterCtx){
				&s_animal_rng, tickClockCount(&s_tickclock),
				player.body.x, player.body.z, player.body.y + PLAYER_EYE,
				0
			};
			(void)entityTick(&s_entities, &s_world, tickClockCount(&s_tickclock),
			                  player.body.x, player.body.z, entityThinkDispatch, &entity_ctx);

			// v1.8.18 MOB-SPAWN. Same health_before/SFX_HURT/SFX_DEATH/survivalRespawn shape
			// as the fall-damage hook and the survivalTick() starvation hook above -- both
			// already establish that pattern for a reason (survivalDamage() returns only
			// death, not whether it merely hurt), and mob damage is the same contract, not a
			// new one. Guarded on player_damage != 0 so a tick with nothing to report never
			// captures health_before or touches audio for no reason.
			if (entity_ctx.monster.player_damage != 0) {
				const uint8_t health_before_mob = survival.health;
				if (survivalDamage(&survival, entity_ctx.monster.player_damage)) {
					audioPlay(audioSfxId(SFX_DEATH), AUDIO_PRIO_HIGH, 1.0f);
					survivalRespawn(&player, &survival, &falltrack,
					                (float)spawn_x + 0.5f, (float)spawn_y, (float)spawn_z + 0.5f);
				} else if (survival.health < health_before_mob) {
					audioPlay(audioSfxId(SFX_HURT), AUDIO_PRIO_HIGH, 1.0f);
				}
			}

			// v1.8.18 MOB-SPAWN. Unconditional every tick, matching this loop's own
			// established convention (see the furnace tick immediately below, and waterTick()
			// above it): every 20 TPS subsystem here gates its own period internally rather
			// than being called conditionally from this site, and monsterSpawnTick() is no
			// exception -- its own `tick % MONSTER_SPAWN_PERIOD_TICKS != 0` gate
			// (entity/monster.c) is what limits it to once every 40 ticks (2 s). It reuses
			// s_animal_rng rather than opening a second stream, the same way
			// animalSpawnForColumn() already shares it: a second Rng would be one more thing
			// to seed, save and carry for no observed benefit.
			(void)monsterSpawnTick(&s_entities, &s_world, tickClockCount(&s_tickclock),
			                       player.body.x, player.body.y, player.body.z, &s_animal_rng);

			// v1.8.15 "Furnace". Every lit furnace in the world advances one tick, here in the
			// tick loop for the same reason water and entities are: world/furnace.h specifies
			// fuel in TICKS (FURNACE_FUEL_TICKS_PLANKS 300 = 15 s at 20 TPS, LOG 1200 = 60 s),
			// and burning those down per rendered frame would make a log last twice as long on
			// a console holding 30 fps as on one at 60. That is the same argument the entity
			// tick above and waterTick below are written from, and a furnace is the case where
			// getting it wrong is most visible, because the player is standing still watching
			// the flame while it happens.
			//
			// The loop walks s_blockstate.slots[] DIRECTLY rather than through an accessor,
			// which is worth justifying because it is the only place in this file that reaches
			// into another module's struct. blockstate.h exposes BlockStateTable fully and
			// documents the property this depends on -- a slot whose block_id is BLOCK_AIR is
			// free, because air can never own state -- so this is reading a published contract,
			// not a private layout. The alternative was adding a blockStateIterate() to
			// blockstate.h for a single caller; the struct is already public and the "zero is
			// empty" rule is already the module's stated invariant, so a new function would
			// have restated it rather than encapsulated it.
			//
			// BLOCKSTATE_SLOTS is 64, so this is a fixed 64-iteration scan whose cost does not
			// depend on how many furnaces exist. That is the point: it cannot degrade as a
			// world fills up, and 64 comparisons of a uint8_t at 20 TPS is not a budget worth
			// managing. It is deliberately NOT decimated by distance the way entityTick is --
			// a furnace the player walked away from must keep burning, or leaving the room to
			// go mining would silently pause the smelt they left running, which is exactly the
			// behaviour a player would report as "the furnace is broken".
			//
			// The pack-back is unconditional rather than gated on furnaceTick's return. That
			// return reports whether anything OBSERVABLE changed (for a future re-mesh of the
			// lit front face), not whether the state was touched: fuel_ticks_left counts down
			// on a tick that changes nothing else, and skipping the write on a false return
			// would silently discard that countdown and leave every furnace burning forever.
			for (int fi = 0; fi < BLOCKSTATE_SLOTS; fi++) {
				BlockStateEntry* e = &s_blockstate.slots[fi];
				if (e->block_id != BLOCK_FURNACE) continue;

				FurnaceState fs;
				furnaceStateUnpack(&fs, e->data);
				(void)furnaceTick(&fs);
				furnaceStatePack(&fs, e->data);
			}

			// v1.8.9 weather. docs/plan-1.8.9-weather.md §7 asked for exactly this and named
			// the shape: once per loaded column, decimated through world/tick.h rather than
			// with a second scheduler invented here.
			//
			// The tick comes from dayNightTicks() and NOT from tickClockCount() beside it,
			// which is the one thing §2 of that plan is emphatic about. Both count the same
			// simulation ticks today, so this looks like a distinction without a difference —
			// but the day/night counter is the one persisted to the time.bin sidecar and the
			// one BS_APP_TIME_SYNC agrees across a server. A weather model keyed to a counter
			// that resets on load would have every client seeing different rain, and would
			// have the rain jump on rejoin. Keyed to this one, it does not.
			//
			// Sweeping all WORLD_MAP_SLOTS costs 1024 pointer tests; the real work is behind
			// tickDue, which staggers by column so the far-field 2 Hz saving is a smooth load
			// and not a 2 Hz spike. weatherTickColumn itself is capped at
			// WEATHER_CELLS_PER_VISIT cells, so a due column is bounded work, not a scan.
			const uint64_t wx_tick = dayNightTicks(&s_daynight);
			for (int i = 0; i < WORLD_MAP_SLOTS; i++) {
				const Column* col = s_world.slots[i];
				if (!col) continue;
				// Column centre against the player, in blocks, to pick near or far rate.
				const int32_t ddx = (col->cx * CHUNK_DIM + CHUNK_DIM / 2) - (int32_t)player.body.x;
				const int32_t ddz = (col->cz * CHUNK_DIM + CHUNK_DIM / 2) - (int32_t)player.body.z;
				const int period = tickPeriodForDistSq(ddx * ddx + ddz * ddz);
				if (!tickDue(wx_tick, period, (uint32_t)i)) continue;
				(void)weatherTickColumn(&s_gen, &s_world, wx_tick, col->cx, col->cz);
			}
		}

		// v1.8.13 SURV-WIRE, the wire half. AFTER the tick loop and after the frame's fall
		// check, so one report carries the frame's final vitals rather than an intermediate
		// one, and only when a value actually moved.
		//
		// Seeded from the retained INBOUND meters rather than from a zeroed struct, and that
		// is the load-bearing line here. deps/blocksmith-server's playerStateApplyMeters()
		// takes a report as authoritative and overwrites everything in it — armour, XP level
		// and XP progress included. This client has no armour or XP system to source those
		// from, so a report built from {0} would silently wipe both on the server every time
		// the player took a point of fall damage. networld.h kept the decoded block for
		// exactly this ("held only so those systems can hook in without a wire redesign"), so
		// health and hunger are overwritten onto it and everything else round-trips untouched.
		//
		// NULL meters means the server sent a pose-only PLAYER_STATE or none at all — a fresh
		// spawn or an old server. Then there is nothing to preserve, a zeroed block is the
		// truth rather than a wipe, and networldSendPlayerReport's own capability probe still
		// refuses to send if no PLAYER_STATE was ever heard.
		if (survival.health != last_sent_health || survival.hunger != last_sent_hunger) {
			NetworldPlayerMeters m;
			const NetworldPlayerMeters* const held = networldPlayerMeters();
			if (held) m = *held;
			else      memset(&m, 0, sizeof m);

			m.health = (float)survival.health;
			m.hunger = (float)survival.hunger;

			// Only bank the values on a send that actually went out. A false — no session, no
			// PLAYER_STATE heard — must leave the pair unchanged so the report is retried on
			// the next frame rather than dropped and never mentioned again.
			if (networldSendPlayerReport(&m)) {
				last_sent_health = survival.health;
				last_sent_hunger = survival.hunger;
			}
		}

		// v1.8.9 weather rendering: the render-side poll, per
		// docs/plan-1.8.9-weather-integration.md §3. Deliberately NOT reusing the `wx_tick`
		// declared above -- that one lives inside `for (int t = 0; t < ticks_now; t++)` and
		// does not exist when ticks_now == 0, which is exactly the paused case. Calling
		// dayNightTicks(&s_daynight) fresh here instead is always safe (a live world is
		// guaranteed by this point in the frame, so s_daynight is long since initialised) and
		// gives the right paused-game answer for free: dayNightAdvance was skipped above when
		// paused, so this reads the same tick every frame the game sits paused, which holds
		// the weather classification and grid still rather than reading stale/uninitialised
		// data -- exactly the behaviour a paused world should have. The animation update
		// below still runs every frame regardless of pause, so scroll/drift keep advancing
		// (the visible "is it still raining" motion), matching every other purely-visual,
		// non-simulation update in drawEye's neighbourhood.
		const uint64_t wx_tick_draw = dayNightTicks(&s_daynight);
		if (tickDue(wx_tick_draw, TICK_HZ / 4, 0)) {
			WeatherKind wk = weatherAt(&s_gen, wx_tick_draw,
			                           (int32_t)floorf(player.cam.x),
			                           (int32_t)floorf(player.cam.z));
			weatherDrawSetState(&s_weatherdraw, wk);
		}
		weatherDrawUpdate(&s_weatherdraw, metricsFrameMs() / 1000.0f,
		                   player.cam.x, player.cam.y, player.cam.z);

		// v1.8.13 SURV-WIRE. The SECOND eat path, and the one that has to exist: ACTION_EAT
		// defaults to ZR, which an original 3DS or 2DS does not physically have, so a dedicated
		// button alone would ship a game where a whole class of console cannot eat without
		// first visiting the remap screen. This is Minecraft's own rule — you eat by using the
		// item in your hand — so it is what a player tries first, and it needs no button that
		// an Old 3DS lacks.
		//
		// DELIBERATELY ABOVE interactAim(), which is the raycast. Nothing here consults
		// it.has_target or any aimed cell, and putting it above the aim is what guarantees that
		// stays true: eating must work while facing open sky, which is exactly where a player
		// running from something at 2 hunger will be looking. Buried inside the "am I aiming at
		// a placeable face" branch it would fail precisely when it matters most.
		//
		// The hunger test is what keeps BLOCK_APPLE a placeable block. At full hunger this
		// whole branch declines and the press falls through to the ordinary place path, so
		// apples can still be built with — an existing capability is not being removed to add
		// a new one. survivalEat refuses at full hunger on its own, but the test is repeated
		// here because it also decides whether the press gets swallowed below.
		if (!paused && !ate_this_frame) {
			const uint32_t place_key = inputKey(ACTION_PLACE);
			if (place_key && (down & place_key)
			    && survivalFoodValue(inventoryHeldItem(&s_inv)) > 0
			    && survival.hunger < SURVIVAL_MAX_HUNGER) {
				// survivalEat's return, not the guard above, is the authority on whether the
				// input was consumed — a refusal for any reason must leave the place path its
				// press rather than swallowing it into nothing.
				ate_this_frame = survivalEat(&survival, &s_inv, s_inv.selected_hotbar);
				// v1.9.0 SFX-WIRE-MAIN. Same gate as the dedicated-key eat path above; the two
				// sites share ate_this_frame and are mutually exclusive by construction (this
				// one is guarded !ate_this_frame), so at most one SFX_EAT fires per frame.
				if (ate_this_frame) audioPlay(audioSfxId(SFX_EAT), AUDIO_PRIO_NORMAL, 1.0f);
			}
		}

		// v1.8.14 animals, the aim half. Cast at the animals ALONG THE SAME RAY interactAim()
		// is about to walk, and before it, so that the two results below are two answers to one
		// question rather than to two slightly different ones.
		//
		// The forward vector is the project's one convention, copied from scene/interact.c:526-529
		// (which is itself the vector scene/player.c's cameraUpdate moves along): the view matrix
		// is Rx(pitch) * Ry(yaw) * T(-pos), so positive pitch looks down and -Z is forward at zero
		// yaw. Re-derived here rather than reached through interactAim(), which keeps the vector
		// to itself and hands back only a RayHit. If these four lines and interact.c's four lines
		// ever disagree, the crosshair and the animal hit point in different directions, and
		// interact.c's own comment is on record about how hard that is to find.
		//
		// INTERACT_REACH, the same constant the block cast uses, so an animal is hit at exactly
		// the distance a block is broken at. A second reach constant is how the two silently
		// drift apart.
		//
		// Gated on !paused: with the pause menu up the crosshair is frozen on whatever it was
		// on and nothing in the world may be acted upon, which is the same contract the edit
		// masks further down enforce for blocks.
		int   animal_slot = -1;
		float animal_dist = 0.0f;
		if (!paused) {
			const float acp = cosf(player.cam.pitch);
			animal_slot = animalRaycast(&s_entities,
			                            player.cam.x, player.cam.y, player.cam.z,
			                            sinf(player.cam.yaw) * acp,
			                            -sinf(player.cam.pitch),
			                            -cosf(player.cam.yaw) * acp,
			                            INTERACT_REACH, &animal_dist);
		}

		// Aim first, then edit, so the highlight and the edit in the same frame cannot
		// disagree about which block was being pointed at.
		interactAim(&it, &s_world, &player.cam);

		// v1.8.14 animals, the tie-break entity/animal.h asks for. The two casts are deliberately
		// separate walkers -- animalRaycast is a float box test, worldRaycast a block-grid DDA
		// with integer hit coordinates -- and neither could sensibly be written in terms of the
		// other. Comparing their distances is both the simpler structure and the honest one.
		//
		// They ARE comparable: both start at the camera, both normalise the direction internally,
		// and both report blocks travelled to the surface (world/raycast.h's RayHit.distance,
		// entity/animal.h's out_dist). If no block is hit at all -- open sky -- any animal
		// within reach wins by default, which is the !it.target.hit arm below.
		//
		// `<=` and not `<`, so a TIE goes to the animal. That is the case the whole splice exists
		// for: a pig standing flat against a wall has a box front and a wall face that can land on
		// the same distance to the last bit, and a player who aimed at the pig has to hit the pig.
		const bool animal_owns_aim =
			animal_slot >= 0 && (!it.target.hit || animal_dist <= it.target.distance);

		// v1.8.14 animals, the damage half. Mirrors ate_this_frame at the top of this loop and
		// means the same thing: this frame's press was consumed by something other than the
		// ordinary verb, so the mask below has to take it away from that verb.
		//
		// Edge-triggered on `down`, not `held`, so one press is one swing. A block break is a
		// multi-second hold (scene/interact.h task 50) but a hit is instant, and reading the
		// level here would land ANIMAL_FIST_DAMAGE every frame the button was down -- three
		// frames would kill a cow.
		//
		// Behind inputKey(ACTION_BREAK) rather than INTERACT_KEY_BREAK, because the binding is
		// remappable and interact.c:404 reads exactly this. inputKey answers 0 for an action the
		// map does not know and `down & 0` is false, so a build whose options never loaded
		// simply cannot swing rather than swinging on every button.
		bool hit_animal_this_frame = false;
		if (!paused && animal_owns_aim) {
			const u32 break_key = inputKey(ACTION_BREAK);
			if (break_key && (down & break_key)) {
				uint8_t drop_item = 0, drop_count = 0;
				// The return is whether this hit KILLED it, and it is dropped: the drop
				// out-parameters already say so (animal.h writes 0 to both on a survivor), and
				// there is nothing else here that treats a kill differently from a hit.
				(void)animalHurt(&s_entities, animal_slot, ANIMAL_FIST_DAMAGE,
				                 player.body.x, player.body.z, &drop_item, &drop_count);
				hit_animal_this_frame = true;

				// The drop goes STRAIGHT INTO THE BAG. There is no ground-item entity anywhere
				// in this codebase -- a broken block goes to the inventory, which is what the
				// it.broke_id line below does -- so inventing one here would be a design change
				// nobody asked for and a second lifetime to get wrong.
				//
				// Through invBridgeAdd and not inventoryAdd, so the pickup is reported to the
				// server as BS_INV_OP_PICKUP exactly like a mined block, and so a refusal by a
				// full inventory tells the server nothing. A refusal is the honest outcome and
				// is not tracked separately, matching the block pickup's comment below.
				//
				// The id is tested first, the same way that line tests against BLOCK_AIR:
				// entity/animal.h defines drop_item 0 as "drops nothing", and it stays 0 on
				// every row until the meat items land, so this must be a real branch and not an
				// assumption that a kill always drops something.
				if (drop_item != 0 && drop_count > 0)
					invBridgeAdd(&s_inv, drop_item, drop_count, NULL);
			}
		}

#if BS_INTERACT_DEMO
		// v1.8.1 task 50. Break is a HOLD now, so the demo has to hold it: a single frame of
		// the bit being set banks one tick and then reads as a release, and the probe would
		// report a break that never happened. DEMO_BREAK_FRAMES is four seconds at 60 fps,
		// comfortably past the 2.25 s of the hardest block in the registry, so the demo does
		// not have to know what it is aimed at. The break bit goes into `held` and the place
		// bit into `down`, because that is now which mask each verb reads.
		if (frame >= DEMO_START_FRAME && frame < DEMO_START_FRAME + DEMO_BREAK_FRAMES)
			held |= INTERACT_KEY_BREAK;
		if (frame == DEMO_START_FRAME + DEMO_BREAK_FRAMES + 30)
			down |= INTERACT_KEY_PLACE;
#endif

		// Step 8.2. Refreshed every frame rather than written once when the player taps a
		// hotbar slot: the selected slot's contents change without the selection changing —
		// place the last block of a stack and the same slot is now empty — and a `holding`
		// updated only on selection would let the player keep placing a block they no longer
		// have. interactEdit refuses BLOCK_AIR, so an empty hand is a refusal, not a
		// long-range break.
		it.holding = inventoryHeldItem(&s_inv);

		// v1.8.15 FURNACE. Aiming at a furnace and pressing PLACE opens its panel instead of
		// putting a block down — Minecraft's "use" on an interactive block, and the reason no
		// new binding was added for it. The 3DS has no spare face button here that would not
		// cost something else: the pause menu owns SELECT, the debug menu and the bag own the
		// rest, and interact.h's two actions are already remappable, so a third world action
		// would have to appear in app/remap_ui.c and in every saved binding file.
		//
		// Overloading PLACE is safe precisely because the two cases are disjoint by their
		// TARGET, not by a modifier the player has to remember: the ray either ends on a
		// furnace or it does not. There is no gesture that means both.
		//
		// Not gated on the panel already being open, deliberately. Re-pressing on a DIFFERENT
		// furnace retargets the panel to that one, which is what a player who walked to the next
		// furnace expects; re-pressing on the same one is a harmless no-op. The alternative —
		// refusing when a panel is open — would make the press fall through to the mask below as
		// a placement, so the player would build a block onto the face of the furnace they were
		// trying to look at, which is the one outcome nobody wants.
		//
		// interactAim() ran at the top of this block, so it.target is THIS frame's raycast and
		// not the previous frame's; that matters because the whole decision hangs on it.
		bool opened_furnace_this_frame = false;
#if BS_BOTTOM_UI
		if (!paused && (down & inputKey(ACTION_PLACE)) && it.target.hit &&
		    worldGet(&s_world, it.target.x, it.target.y, it.target.z) == BLOCK_FURNACE) {
			s_furnace_x = it.target.x;
			s_furnace_y = it.target.y;
			s_furnace_z = it.target.z;
			s_furnace_open = true;
			// uiOpenFurnace rather than writing s_ui.screen here: it also drops any stack the
			// player had lifted in the bag, which a raw assignment would forget and which would
			// leave that stack held over a panel whose slots it cannot legally go into.
			uiOpenFurnace(&s_ui);
			opened_furnace_this_frame = true;
		}
#endif

		// v1.8.7. The masks are zeroed while the pause menu is open, and until now they were not.
		// INTERACT_KEY_BREAK is KEY_X and INTERACT_KEY_PLACE is KEY_Y (scene/interact.h); the
		// pause menu reads only the D-pad, A and B (scene/pausemenu.c), so X and Y fell straight
		// through it into the world. With the menu up the player cannot move or look — playerUpdate
		// is gated — so the crosshair is frozen on whatever it was on when they hit SELECT, and
		// holding X for the block's break time destroyed it from inside the menu. Same for Y
		// placing one. That contradicts this loop's own "a paused world does not tick" contract.
		//
		// MASKED rather than the call being skipped, and that is load-bearing: interactEdit clears
		// broke_id and placed_id at the top of every call (see Interact.broke_id), and the two
		// invBridge lines below read them immediately after. Skip the call and last frame's ids
		// stay standing, so a block broken just before pausing would be re-awarded to the
		// inventory on every single frame the menu stayed open.
		// v1.8.13 SURV-WIRE: the place bit is stripped on a frame the food above was eaten, so
		// one press cannot both feed the player and place the apple they just swallowed.
		// Masked here rather than out of `down` itself, because `down` is read by several other
		// consumers between the eat check and this line and none of them should see a press
		// disappear. interact.c:405 reads exactly inputKey(ACTION_PLACE), so this is the same
		// bit it is about to test.
		// v1.8.15 FURNACE: and the place bit is stripped on the frame that opened a furnace
		// panel, for exactly the reason the eat mask above exists — one press must not both open
		// the furnace and place a block against its face. Same mask, same line, same bit.
		const u32 edit_down = (paused ? 0u : down)
		                    & ~(ate_this_frame ? inputKey(ACTION_PLACE) : 0u)
		                    & ~(opened_furnace_this_frame ? inputKey(ACTION_PLACE) : 0u)
		// v1.8.14 animals: and the break bit is stripped on a frame the swing above consumed it,
		// so one press cannot both damage an animal and start breaking the block behind it.
		// interact.c reads keys_down for break in two places -- `fresh & key_break` becomes
		// breakProgress's `pressed`, and a press with no target counts a refusal -- so this is
		// not a no-op, and clearing the bit out of prev_keys also re-arms the edge cleanly for
		// the first press after the animal moves out of the way.
		                    & ~(hit_animal_this_frame ? inputKey(ACTION_BREAK) : 0u);

		// v1.8.14 animals: the HELD half, and it is the one that actually delivers "hitting a
		// pig standing in front of a wall hits the pig". interact.c:410 reads break as a raw
		// LEVEL (`keys_held & key_break`) because a break is a hold, so masking `down` alone
		// would swing at the pig on the press frame and then quietly mine the wall through it
		// for the next two seconds.
		//
		// Keyed on animal_owns_aim and not on hit_animal_this_frame, because the flag above is
		// true for exactly one frame per press and one frame of masking cannot stop a hold.
		// animal_owns_aim is true on every frame the animal is nearer than the block, which is
		// the condition that has to be answered. The consequence is deliberate and worth
		// stating: an animal wandering across a break already in progress cancels it
		// (interactEdit's `if (!holding_break) breakCancel`), and the player starts that block
		// again once it has moved. Progress not surviving an interruption is already this
		// module's rule for letting go of the button.
		const u32 edit_held = (paused ? 0u : held)
		                    & ~(animal_owns_aim ? inputKey(ACTION_BREAK) : 0u);
		interactEdit(&it, &s_world, &player.body, edit_down, edit_held, ticks_now);

		// Read once, straight after the call that sets them, because interactEdit clears both
		// at the top of its next call — see Interact.broke_id. A block that does not fit is
		// left on the floor, which is the honest outcome: inventoryAdd reports the refusal
		// rather than eating it, and the block is already gone from the world by the time we
		// are told, so there is nothing here that could put it back. The count is not tracked
		// separately — `it.refused` is about edits the world rejected, and this edit was
		// accepted; it is the pickup that failed.
		//
		// v1.3.0: through net/inv_bridge.h, which additionally reports the pickup to the server
		// as BS_INV_OP_PICKUP — and reports the amount that actually *landed*, so a refusal by a
		// full inventory tells the server nothing, matching the sentence above exactly.
		if (it.broke_id != BLOCK_AIR)
			invBridgeAdd(&s_inv, it.broke_id, 1, NULL);

		// v1.8.15. A block that carried per-position state has just stopped existing, so the
		// state goes with it. Read on the same schedule and immediately after the same call,
		// for the same reason as broke_id above: interactEdit clears broke_valid at the top of
		// its next call.
		//
		// Gated on broke_valid, NOT on broke_id, and the difference matters. broke_id is
		// BLOCK_AIR for a break that dropped nothing — a plant, or a block mined by the wrong
		// tool — and those still vacate the cell. Hanging this off broke_id would leave the
		// state of any such block in the table forever, which with only BLOCKSTATE_SLOTS 64
		// entries is a leak that fills a fixed pool rather than one that merely wastes memory.
		//
		// blockStateRemove on a cell holding no state is a no-op by contract, so this runs
		// unconditionally on every landed break rather than asking first. That is deliberate:
		// a "does this block have state" test here would be a second, separate opinion about
		// which blocks are stateful, and the day it disagreed with the table the symptom would
		// be a slot that never frees.
		//
		// v1.8.17. Before the record goes, its contents come out. interactEdit filled
		// broke_extra_* by calling mainFurnaceBrokeContents above while the record still
		// existed; this is only the payout, and it must stay ahead of the blockStateRemove
		// below in this same block. A furnace broken with ore in it used to destroy the ore
		// silently, which is the one outcome a player cannot undo.
		//
		// Each stack goes through invBridgeAdd exactly as the block's own drop does twenty
		// lines up, so a full inventory refuses it and reports the refusal rather than eating
		// it — the contents are then lost, but they were already lost before this existed, and
		// the block is gone from the world by the time we are told, so there is nothing here
		// that could put it back.
		if (it.broke_valid) {
			for (int i = 0; i < it.broke_extra_count; i++)
				invBridgeAdd(&s_inv, it.broke_extra_item[i], it.broke_extra_qty[i], NULL);
			blockStateRemove(&s_blockstate, it.broke_x, it.broke_y, it.broke_z);
		}

		// Charged only for a placement the world actually accepted. it.placed_id stays
		// BLOCK_AIR on every refusal path in interactEdit — no target, no entry face, cell
		// occupied, would entomb the player, empty hand — so a refused place cannot silently
		// consume a block out of the hotbar.
		//
		// v1.3.0: through net/inv_bridge.h, reporting BS_INV_OP_CONSUME. Same rule as the pickup
		// above — the number sent is what was really taken out, so a placement charged against an
		// inventory that somehow no longer held the block sends nothing rather than asking the
		// server to remove a block it can see the player does not have.
		if (it.placed_id != BLOCK_AIR)
			invBridgeRemove(&s_inv, it.placed_id, 1);

		// v1.8.15 "Furnace". A placed furnace acquires its own empty state, the exact mirror of
		// the blockStateRemove above -- place claims a slot, break frees it.
		//
		// Gated on the block id and NOT run unconditionally, which is the opposite of the break
		// path twenty lines up, and the asymmetry is deliberate rather than an inconsistency.
		// Removing state for a cell that has none is free and idempotent, so the break side can
		// afford to hold no opinion about which blocks are stateful. CREATING state is not free:
		// BLOCKSTATE_SLOTS is 64, so calling blockStateCreate for every dirt block placed would
		// fill the table within seconds of ordinary building and leave no room for a furnace.
		// The table has to be told what deserves a slot; only the break side can afford not to
		// care.
		//
		// The return is deliberately discarded. blockStateCreate answers false when the table is
		// genuinely full -- 64 live records, none of them this cell -- and there is nothing
		// useful to do about it here: the furnace block itself has already been placed into the
		// world by interactEdit, and un-placing it underneath the player would be a stranger
		// outcome than a furnace that refuses to hold fuel. A player who has 64 furnaces going
		// at once has found the limit; the 65th behaves like decoration rather than crashing or
		// silently eating what they put in it, because every read of an absent record is a clean
		// "no state here" by blockstate.h's contract.
		if (it.placed_valid && it.placed_id == BLOCK_FURNACE) {
			if (blockStateCreate(&s_blockstate, it.placed_x, it.placed_y, it.placed_z,
			                      BLOCK_FURNACE)) {
				// blockStateCreate hands back an all-zero payload, and an all-zero FurnaceState
				// is already the correct empty furnace -- every slot ITEM_NONE, no fuel, no
				// progress, unlit. Packing furnaceStateInit's result over the top is therefore
				// redundant TODAY. It is done anyway, because "zero happens to mean empty" is a
				// property of furnace.h's current field layout and not a promise it makes; the
				// day a field gains a non-zero resting value, this line is what keeps a freshly
				// placed furnace correct, and its absence would be a bug nobody could see.
				FurnaceState fs;
				furnaceStateInit(&fs);

				uint8_t payload[BLOCKSTATE_PAYLOAD_BYTES];
				furnaceStatePack(&fs, payload);
				(void)blockStateSet(&s_blockstate, it.placed_x, it.placed_y, it.placed_z,
				                     BLOCK_FURNACE, payload);
			}
		}

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
			if (lightEnabled())
				lightRelightColumn(&s_world, sx >> 4, sz >> 4);
			chunkRenderTouch(&s_world, sx, sy, sz);
		}
#endif

		// The measured rate, recomputed about once a second against the system tick counter.
		{
			const u64   now_st  = svcGetSystemTick();
			const double win_ms = (double)(now_st - s_tps_mark_tick) / CPU_TICKS_PER_MSEC;
			if (win_ms >= 1000.0) {
				const u64 now_count = tickClockCount(&s_tickclock);
				s_tps            = (float)((double)(now_count - s_tps_mark_count)
				                           * 1000.0 / win_ms);
				s_tps_mark_tick  = now_st;
				s_tps_mark_count = now_count;
			}
		}

		// v1.8.0. Every column a remote edit invalidated this frame, relit once each. This
		// MUST come before the mesh drain below: chunkRenderTouch has already queued those
		// chunks, and a chunk meshed before its column is relit bakes the old light into
		// its vertices and keeps it until something else touches it.
		//
		// v1.8.3: BUDGETED, and the two sentences that used to stand here are gone because
		// both were false. They said this drain was "bounded by the loaded column count
		// (RENDER_DIST_MAX_COLUMNS = 49)" and that it "is multiplayer-only, so it should read
		// 0.000 in single player". Neither survives reading the code:
		//
		//   * The bound is RELIGHTQ_CAP = 64 (world/relightq.h), which is what caps how many
		//     entries relightqPop can return. And 49 was not the loaded column count either —
		//     that is the RENDER distance ring; the columns actually loaded are the generation
		//     area, GEN_AREA_SPAN * GEN_AREA_SPAN = 81.
		//   * Single player feeds it every time water moves. onWorldEdit -> waterNotify, then
		//     world/water.c's change hook -> onWaterChange -> onRemoteEdit -> relightqPush.
		//     Digging into a lake fills this queue with no server involved, so "it should read
		//     0.000 in single player" was pointing anyone who read the metrics CSV away from
		//     the thing they were looking at.
		//
		// 64 columns at the MEASURED host cost of one lightRelightColumn (med 0.156 ms, max
		// 0.287 ms over 240 samples) is 9.98 to 18.4 ms in one frame against a budget of 16.71
		// — a dropped frame on the host, before any ARM11 penalty. See RELIGHT_BUDGET_MS at the
		// top of this file for the measurement and where the two limits come from; the drain
		// itself is world/relight_drain.c, split out of here so the budget can be host-tested —
		// main.c links into no host binary, so a loop written in this function cannot be.
		//
		// WHAT THE DEFERRAL COSTS, stated because it is a real regression against the old
		// behaviour and not only a saving. Nothing is dropped — a column this frame does not
		// reach stays in the set and is relit on a later frame — but the ordering guarantee in
		// the paragraph above is now only guaranteed for the columns that DID drain. A chunk
		// belonging to a deferred column can be meshed by the drain below before its column is
		// relit, and it then holds the old light until something touches it again. That window
		// opens only when more than RELIGHT_MAX_COLUMNS columns are pending in one frame (a
		// rejoin, or a large water collapse). Repairing it needs a remesh policy — re-touching
		// a deferred column dirties all COLUMN_CHUNKS = 8 of its chunks, eight times the mesh
		// work of the edit that caused it — and that is a decision, not a detail, so it is
		// left alone here and reported rather than invented.
		//
		// SEAM, deliberately left open (v1.8.3). A water-aware notify suppression is queued
		// behind Phase 4: it wants a drain-in-progress flag raised around the drain in
		// net/networld.c and read by onWorldEdit, so that the edits a relight itself provokes
		// do not re-enter the queue it is draining. Nothing here needs changing for it —
		// relightDrain is ONE call with the whole drain inside it, which is exactly the shape
		// such a flag needs to bracket. Do not widen it.
		//
		// BLANKET notify suppression is NOT the fix and must not be substituted for it. It was
		// tried and measured, and it regressed the control arm from 117 blocks / 608 flow back
		// to 1 block / 0 flow — i.e. it switched the water simulation off. Only the water-aware
		// version is wanted.
		// The phase marker sits AHEAD of the relight drain, not below it (moved v1.8.15). It
		// used to sit under the drain, which left relightDrain running while the phase still
		// read WD_PHASE_SIM — a phase whose declared scope is "player, camera, ring follow,
		// aim, edit". A watchdog report naming SIM for a stall that actually happened in the
		// relight queue would send the next investigation into the simulation code, and a
		// wrong signpost costs more than a missing one: it spends the investigator's time
		// before it spends their doubt.
		//
		// This is about where a report POINTS, not about whether a hang is possible here. The
		// drain is bounded twice over — RELIGHT_MAX_COLUMNS caps the work and RELIGHT_BUDGET_MS
		// caps the time — so it is not an indefinite-block risk the way a GPU wait is. The
		// marker moved anyway, because the cost of moving it is one line and the cost of
		// leaving it is a mislabelled report on the one occasion anybody ever reads one.
		watchdogPhase(WD_PHASE_MESH);
		// v1.8.18 MESH-SUBPHASE. relightDrain is the one piece of WD_PHASE_MESH's work that
		// chunk_render.c cannot mark itself (it lives outside that file), so it is set here,
		// at the only call site. Cleared implicitly: chunkRenderBuild sets WD_MESH_BUILD_ENTER
		// on its own first line, so this value is only ever visible while relightDrain is
		// actually running, never stale into the drains that follow it in the same phase.
		watchdogMeshStage(WD_MESH_RELIGHT);
		const u64 t_relight = svcGetSystemTick();
		relightDrain(&s_world, &s_relightq, RELIGHT_MAX_COLUMNS,
		             (u64)(RELIGHT_BUDGET_MS * CPU_TICKS_PER_MSEC), relightNowTicks);
		const float relight_ms =
			(float)((double)(svcGetSystemTick() - t_relight) / CPU_TICKS_PER_MSEC);

		// All of this frame's meshing, under one budget. Edits first: a broken block that
		// takes two frames to disappear is felt, and a chunk of scenery that takes two
		// frames to arrive at the edge of the render distance is not.
		//
		// v1.8.17: asked ONCE, into a local, and then shared between the two drains below.
		// Not because the call is dear -- it is a cached bool -- but because "one budget for
		// all meshing, not one each" is the invariant this block exists to hold, and two
		// separate reads of a per-console count is exactly the shape that quietly becomes two
		// wallets again the next time somebody edits one of the two call sites.
		const u64 t_work = svcGetSystemTick();
		const int max_chunks = drainMaxChunks();
		const int edit_built = chunkRenderDrainDirty(&s_world, DRAIN_BUDGET_MS, max_chunks);
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
		                                      max_chunks - edit_built);
		const float work_ms =
			(float)((double)(svcGetSystemTick() - t_work) / CPU_TICKS_PER_MSEC) + install_ms;

		if (work_ms > s_genr.worst_stream_ms) s_genr.worst_stream_ms = work_ms;
		if (edit_built + stream_built > s_genr.worst_built)
			s_genr.worst_built = edit_built + stream_built;
#endif

		// v1.7.1 task 49. Publish this frame's annotation into the CSV row metricsFrameEnd is
		// about to write, then clear the two accumulators for the next frame. Done here, after
		// both drains, so `meshq` is the backlog that SURVIVED the budget rather than the one
		// that went into it — a queue that is always empty afterwards means the budget is not
		// what is costing the frame, and a queue that only grows means it is.
		{
			MetricsWork mw;
			memset(&mw, 0, sizeof mw);
			mw.player_cx  = s_center_cx;
			mw.player_cz  = s_center_cz;
			mw.recenter_ms = s_frame_recenter_ms;
			mw.relight_ms  = relight_ms;
			mw.save_ms     = s_frame_save_ms;
#if BS_WORLD_GEN
			mw.mesh_ms = work_ms;
			mw.built   = (u16)(edit_built + stream_built);
			mw.meshq   = (u16)(jobqCount(&s_meshq) + chunkRenderDirtyCount());
#else
			// No streaming build: the edit queue is the only queue there is.
			mw.mesh_ms = edit_ms;
			mw.built   = (u16)edit_built;
			mw.meshq   = (u16)chunkRenderDirtyCount();
#endif
			metricsSetWork(&mw);

			s_frame_recenter_ms = 0.0f;
			s_frame_save_ms     = 0.0f;
		}

		// Phase 4's status line. Every field answers a question a screenshot would
		// otherwise leave open: whether the ray found anything (`aim` block and face),
		// whether an edit reached the world (`b`roke / `p`laced / `r`efused), and whether
		// the queue is keeping up (`dq` current/peak) — read that against `worst` and
		// `over` above. Typically short — "aim -12 8 -10 f2 b9 p9 r9 dq8/8" is 31 — but the
		// buffer is NOT sized off that, for the same reason netline below is not.
		//
		// Built here, before the frame opens, rather than after it closes where it used to
		// live: step 8.3's bottom-screen UI draws this same string *inside* the frame, and
		// a value cannot be drawn before it exists. Both consumers now read one buffer, so
		// the console overlay and the touch screen can never disagree about what the player
		// is aiming at.
		//
		// 93 bytes, not the 33 this used to be. What sized it at 33 was a claim that "the
		// console is 32 columns wide and truncates without complaining, so this has to stay
		// inside it", and that reasoning was wrong twice over. The 32-column clip is real but
		// it belongs to debug/metrics.c:393's `printf("%-32s\n", status)` — a MINIMUM field
		// width, which pads and never shortens — and that whole overlay is compiled only when
		// BS_BOTTOM_UI is 0 (gfx/screen.c:21-35 is the only consoleInit in the program), so
		// the CIA the player runs has no console for it to be clipped by. Its real consumer is
		// drawBottomUi() -> scene/ui.c:268, `fontDrawf(6, y, 1, ...)` with FONT_ADVANCE 6
		// (gfx/font.h:39) on the 320px bottom screen, where 52 characters are visible. A
		// display clip loses characters off the right edge of one frame; a short buffer makes
		// snprintf truncate silently, which is the failure mode that matters, and 33 was
		// already short of the declared ranges long before anyone counted:
		//   literals "aim " " " " " " f" " b" " p" " r" " dq" "/"                     18
		//   x, z — world block coords, "signed and unbounded" (world/world.h:9-10)
		//          and reached through floorToInt of a float camera, so 11 each        22
		//   y — 11, not 3. worldGet clamps reads (world/world.c:192-193) but not this
		//       field: above the ceiling reads air so no hit lands there, and below 0
		//       reads WORLD_FLOOR_BLOCK, which is targetable, so the DDA stops at -1
		//       whenever the camera starts inside the world. A BS_FLY build (main.c:231)
		//       replaces playerUpdate with cameraUpdate and keeps interactAim, so the
		//       camera flies below the floor and worldRaycast's inside-a-solid early
		//       return hands back floorToInt(oy) unbounded. Bounded by physics, not by
		//       the type, and one of the in-tree builds removes the physics             11
		//   face — FACE_* 0..5 or RAY_FACE_NONE (-1), world/raycast.h:13                 2
		//   b p r — Interact.broke/placed/refused, unclamped `int` lifetime counters
		//           (scene/interact.h:51-53), so 11 each ("-2147483648")                33
		//   dq pair — dirtyqCount/dirtyqPeak, both bounded by DIRTYQ_MAX 392 and never
		//             negative (world/dirtyq.c:33-53), so 3 each                         6
		//   NUL                                                                          1
		// The "aim none" branch is shorter (58) and does not set the size.
		char status[93];
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
		// Typically short — "net s99 r999 y999 a999 q99 p9" is 30 — but do not size the buffer
		// off that. netline's only consumer is drawBottomUi(), which draws it through
		// scene/ui.c:270 where 52 characters fit. That used to add "and note this is NOT the
		// 32-column console `status` above is bound by" — `status` is not bound by it either,
		// and was silently truncating because of that belief. See the note there.
		// See the buffer arithmetic below.
		//
		// The trailing "!" is v1.6.0 task 8's readout of net/networld.h's
		// networldRegistrySynced(): in a session it means this client's block table never
		// agreed with the server's, so any dynamic block id in the world is resolving to air
		// through registryView()'s never-NULL contract. That looks *exactly* like terrain the
		// server happens to have dug out, which is the same "silently identical to working"
		// problem the rest of this line exists for. Printed only in a session — single player
		// has no server table to agree with, so its absence there is not a warning.
		//
		// The trailing "X" is the sibling report for a different silent loss, and one this
		// line was until now blind to: net/networld.h's networldPendingRefusals(). The pending
		// store turns a diff away once it is holding BLOCKDIFF_MAX_PENDING (65536) of them
		// rather than evicting one to make room (net/blockdiff.c), and it has always counted
		// those refusals — but nothing outside the test suites ever read the count, so a
		// client that refused 65536 edits looked on screen exactly like one that refused
		// none. That is the same "silently identical to working" failure the whole line
		// exists to break, so it belongs here next to "!".
		//
		// Read it as "this session refused edits, so a loaded column may be showing a stale
		// world until it is reloaded" — NOT as "the save is damaged". The loss is recoverable:
		// the server's handle_chunk_sub() calls send_chunk_diffs() unconditionally, so every
		// column the player loads gets its diffs delivered again.
		//
		// It is also not a sign that the two caps should meet. The server's BS_DIFF_MAX is
		// 131072 on purpose (server commit b95f980, 2026-08-21): what capped it was never the
		// server's memory but the console's, and with per-column delivery the 3DS holds edits
		// for loaded columns only, so the two ceilings are meant to come apart. Nothing here
		// should be read as asking the client to grow to match.
		//
		// 69 bytes, not the 33 this buffer used to be, computed from the declared ranges
		// rather than from the typical values the estimate above quotes:
		//   literals "net s" " r" " y" " a" " q" " p"                              15
		//   s r y a — unclamped `int` lifetime counters, so 11 each ("-2147483648") 44
		//   q — bounded by BLOCKDIFF_MAX_PENDING, 65536                             5
		//   p — bounded by NETWORLD_MAX_REMOTE, 15                                  2
		//   "!" and "X", which are independent and can both be present              2
		//   NUL                                                                     1
		// 33 was already short of that before this marker was added — y alone reaches 131072
		// in the replay case blockdiff_test.c measures — and snprintf truncates without
		// saying so, which is precisely the species of silent failure being reported here.
		// Sized to the types so it cannot happen. This is the buffer bound only, not a screen
		// bound: scene/ui.c:270 draws this at x=6 with FONT_ADVANCE 6 on the 320px bottom
		// screen, so 52 characters are visible, comfortably past any plausible content.
		char netline[69];
		snprintf(netline, sizeof(netline), "net s%d r%d y%d a%d q%d p%d%s%s",
		         networldSentEdits(), networldRecvMsgs(), networldSyncEntries(),
		         networldAppliedEdits(), networldPendingCount(), networldRemoteCount(),
		         (s_server_session && !networldRegistrySynced()) ? "!" : "",
		         (networldPendingRefusals() > 0) ? "X" : "");

#if BS_CMDBUF_PROBE
		// Throwaway instrument, not shipped. hang.txt proved the main thread finishes the
		// whole frame and then blocks in C3D_FrameBegin(C3D_FRAME_SYNCDRAW), so the GPU took
		// the world's commands and never signalled. One thing that does exactly that, and
		// grows with the number of chunks on screen, is the command buffer overrunning: it is
		// 0x40000 bytes (C3D_DEFAULT_CMDBUF_SIZE, passed in gfx/screen.c).
		//
		// CORRECTED v1.8.10, and the correction is the point of keeping this paragraph. It used
		// to end "and citro3d does not bounds-check the writes into it, so a frame that needs
		// more silently walks off the end and the GPU is handed whatever follows it in the
		// linear heap." That is FALSE, and it was reasoning that never got checked against the
		// SDK. Disassembling the actually-linked /c/devkitPro/libctru/lib/libctru.a (2.7.0-1)
		// shows GPUCMD_Add ends with `cmp r1, ip / bls / mov r0,#0 / bl svcBreak` — it compares
		// gpuCmdBufOffset + paramlength + 1 against gpuCmdBufSize and calls
		// svcBreak(USERBREAK_PANIC) rather than writing past the end. Every citro3d emission
		// path (AddWrite, AddMaskedWrite, AddWrites, AddIncrementalWrites) funnels through it.
		//
		// So an overrun is a deterministic supervisor panic, NOT a silent wedge — a DIFFERENT
		// signature from the unresponsive hang this probe was built to explain. The 2026-09-02
		// hardware freeze was chased down this paragraph for two hours on the strength of the
		// sentence above. Arithmetic since put radius 5 at ~59% of the ceiling (radius 3 ~24%),
		// so the instrument is still worth having — but it can only ever exonerate, and a low
		// reading here does not narrow anything, because a high one would have panicked instead.
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
		// v1.8.10: metricsSyncBegin/End no longer wrap this block. The queue wait they existed
		// to time now happens in gpuWaitPrevFrame(), far above, and the metric moved with it.
		//
		// FRZ-NETFIX, 2026-09-03. True unless one of the two bounded waits below (BS_DRAW_PROBE's
		// nested BS_GPU_TESTS arm, or the plain #elif BS_GPU_TESTS shipping arm) catches a wedged
		// GPU, in which case it goes false and C3D_FrameBegin is skipped instead of being called
		// unconditionally against a queue that has just proven it does not drain. Read below by
		// the `if (frame_opened)` that wraps the whole draw body, from spriteFrameBegin() down to
		// the metricsDrawOverlay()/worldReportDraw() at the end of the frame — see the shipping
		// arm's comment for the reasoning. The plain #else arm (BS_GPU_TESTS=0, no bounded wait
		// exists there to catch anything) never touches this flag and always draws.
		bool frame_opened = true;
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
		//
		// FRZ-NETFIX, 2026-09-03. This arm had the same unconditional-FrameBegin-after-a-detected-
		// wedge bug as the plain `#elif BS_GPU_TESTS` shipping arm below — see that arm's comment
		// for the evidence and the full reasoning, which applies here unchanged. gpu_dead is its
		// own static in this block rather than shared with the shipping arm's, the same way
		// gpu_frame already was, because only one of the two arms is ever compiled into a given
		// binary.
		{
			static uint32_t gpu_frame;
			static bool     gpu_dead;
			gpu_frame++;
			if (gpu_dead) {
				frame_opened = false;
			} else if (!gpuTestFrameWait()) {
				gpuTestPostMortem(gpu_frame);
				gpu_dead     = true;
				frame_opened = false;
			}
		}
#endif
		if (frame_opened) C3D_FrameBegin(0);
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
		//
		// FRZ-NETFIX, 2026-09-03. That difference was a lie. Up to and including v1.8.16 this was:
		//
		//   if (!gpuTestFrameWait()) gpuTestPostMortem(gpu_frame);
		//   C3D_FrameBegin(0);
		//
		// — unconditional. The post-mortem ran, and the very next line called C3D_FrameBegin(0)
		// anyway, which gpuWaitPrevFrame()'s own header comment (above, ~L3812) documents from the
		// disassembly as C3D_FRAME_SYNCDRAW's vblank wait FOLLOWED BY an unconditional
		// gxCmdQueueWait(-1) — the exact unbounded wait this whole file exists to avoid. Against a
		// GPU that had JUST failed a 2-second bounded wait on the line above, that unbounded wait
		// never returns. steve's console proved it on 2026-09-03 running v1.8.16: postmortem.txt
		// was written to the card AND the console was still frozen solid, HOME included. The
		// safety net caught the fall and then walked off the same cliff.
		//
		// The fix is frame_opened (declared above the #if BS_DRAW_PROBE chain this block sits in)
		// and the sticky gpu_dead below. A detected wedge now skips C3D_FrameBegin(0) entirely for
		// this iteration — no frame opens, so nothing is left unpaired — and the whole draw body
		// from spriteFrameBegin() down to the metricsDrawOverlay()/worldReportDraw() at the bottom
		// of the frame is wrapped in `if (frame_opened)` for the same reason: every line in that
		// span either assumes an open C3D frame or submits GPU work of its own (spriteFrameBegin,
		// both drawEye calls, the bottom-screen UI, C3D_FrameEnd, the BS_DRAW_PROBE hooks, the
		// completion screenshot in worldReportDraw), and none of it can run against a queue that
		// just proved it does not drain. watchdogCounters/watchdogBeat/probeFrame and
		// watchdogPhase(WD_PHASE_APT) sit AFTER that guard's closing brace and stay unconditional —
		// that split is the whole fix, because those are what keep aptMainLoop() serviced and HOME
		// answering on a skipped frame.
		//
		// gpu_dead is sticky on purpose. The GPU does not un-wedge itself, so without it every
		// later frame would pay another 2-second gxCmdQueueWait for an answer already known,
		// turning a frozen console into one that merely runs at a small fraction of a frame per
		// second — not a fix, just a slower version of the same failure. Once set, later frames
		// skip gpuTestFrameWait() entirely and go straight to frame_opened = false.
		// gpuTestPostMortem() already has its own one-shot `static bool done` guard
		// (app/gputest.c) and would only ever run its report once regardless — gpu_dead is what
		// stops paying for the 2-second wait that leads to it, not what stops it firing twice.
		//
		// There is deliberately no on-screen message. Every draw call this build has, including
		// BS_BOTTOM_UI's text panel, is itself a GPU submission and lives inside the same
		// frame_opened guard for the reason above — a wedged GPU cannot be asked to display a
		// sentence about being wedged. What the player gets instead is the last frame that
		// actually completed, held on both screens, with HOME still live. That is the entire
		// distinction this fix draws: it does not un-wedge the GPU, it stops the wedge from
		// taking the console down with it.
		C3D_FrameSync();
		{
			static uint32_t gpu_frame;
			static bool     gpu_dead;
			gpu_frame++;
			if (gpu_dead) {
				frame_opened = false;
			} else if (!gpuTestFrameWait()) {
				gpuTestPostMortem(gpu_frame);
				gpu_dead     = true;
				frame_opened = false;
			}
		}
		if (frame_opened) C3D_FrameBegin(0);
#else
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#endif
		// FRZ-NETFIX, 2026-09-03. Everything from here down to the metricsDrawOverlay()/
		// worldReportDraw() below either assumes C3D_FrameBegin actually opened a frame this
		// iteration or submits GPU work of its own, so none of it is safe to run when the bounded
		// wait above just caught a wedge and left frame_opened false. See the shipping arm's
		// comment a few lines up for the full reasoning and what stays unconditional instead.
		if (frame_opened) {
		spriteFrameBegin();
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
			// v1.8.1 task 50. Read once, outside the two calls, so both eyes cannot possibly
			// disagree about how cracked the block is.
			const int crack_stage = interactBreakStage(&it);
			// v1.8.9 day/night. Same reasoning as crack_stage above: read once, outside the
			// two eye calls, so a stereo frame's left and right eye cannot land on opposite
			// sides of a tick boundary and disagree about the sky.
			chunkRenderSetTimeOfDay(dayNightTimeOfDay(&s_daynight));
			// v1.8.10 shaders option: fake directional lighting. Same shape as the call just
			// above — once a frame, unconditionally, from the live Options struct — see
			// chunkRenderSetFakeShading's own comment in chunk_render.c for why an
			// unconditional call every frame is safe and cheap here.
			chunkRenderSetFakeShading(opts.fake_shading);
			// v1.8.10 shaders option: the water shimmer's clock. Same reasoning as
			// crack_stage and the time of day above — sampled ONCE, outside the two eye
			// calls, so a stereo frame cannot scroll the glints to two different places
			// and have the 3D slider fuse the difference as depth. The tick-to-ms divisor
			// is the same one app/sleep.c and app/battery.c use, and it stays correct at
			// 804 MHz (see app/hw.h).
			chunkRenderSetShimmerTimeMs(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));
			drawEye(screenTop(), &view, &it.target, crack_stage, s_stereo ? -iod : 0.0f, 0);
			if (s_stereo)
				drawEye(screenTopRight(), &view, &it.target, crack_stage, iod, 1);
#if BS_BOTTOM_UI
			// Step 8.3. Last in the frame, and deliberately so: the sprite batch sets its
			// own render state and does not restore it (see gfx/sprite.h), while
			// chunkRenderDraw re-establishes everything it needs at the top of its next
			// call. Drawing the UI first would mean paying to put the world's state back.
			drawStage(WD_DRAW_BOTTOM, -1);
			// The menu covers this screen, so the panel under it must not still be taking
			// taps: a finger landing on the QUIT row would otherwise also swap the hotbar
			// slot it happens to sit over. Feeding a zeroed UiInput is enough — ui.c edge-
			// detects against its own touch_prev, so releasing into a closed menu does not
			// then register as a fresh press.
			const UiInput blank = {0};
			drawBottomUi(ui_ok, highlight_ok ? status : "HIGHLIGHT INIT FAILED",
			             netline, paused ? &blank : &touch);

			// v1.4.0 battery gauge, top-right corner of the bottom screen. Own sprite pass
			// for the same reason pauseMenuDraw opens one: uiUpdateDraw closed its batch.
			spriteBegin(320, 240);
			spriteTexture(fontTexture());
			batteryDraw(284.0f, 4.0f);
			spriteEnd();

			// The v1.4.0 live minimap drew here, bottom-right. Removed in v1.5.1 — see the
			// init site above for why it was cut rather than fixed.

			// After the UI and on the same target, so it lands on top of it. See
			// scene/pausemenu.c on why this opens its own sprite pass rather than joining
			// the one uiUpdateDraw has already closed.
			if (paused) {
				const PauseStats pstats = {
					.linear_free  = (uint32_t)linearSpaceFree(),
					.vram_free    = (uint32_t)vramSpaceFree(),
					.world_used   = (uint32_t)worldBytes(&s_world),
					.world_budget = (uint32_t)budgetCap(),
					.render_dist  = s_mesh_radius,
					.dist_min     = RENDER_DIST_MIN,
					.dist_max     = renderDistMaxFor(hwIsNew3ds()),
					.stereo       = s_stereo,
				};
				pauseMenuDraw(&pstats);

			// v1.4.0. The remap screen and the debug menu draw over the pause panel and take
			// the frame's input themselves (their update calls draw internally, which is why
			// they live here in the draw phase rather than up with the other input). Closing
			// either persists: remap applies its bindings into opts and re-snapshots the
			// input map; debug saves when it changed the render distance or toggled itself.
			if (s_remap_open) {
				s_remap_open = remapUiUpdate(&s_remap, down, touch_press,
				                             touch.touch_x, touch.touch_y);
				if (!s_remap_open) {
					remapApply(&s_remap, &opts);
					optionsSave(&opts, TITLE_OPTIONS_PATH);
					inputMapSet(&opts);
				}
			} else if (s_debug_open) {
				s_dctx.render_dist     = s_mesh_radius;
				s_dctx.render_dist_min = RENDER_DIST_MIN;
				s_dctx.render_dist_max = renderDistMaxFor(hwIsNew3ds());
				s_dctx.set_render_dist = bsDebugSetRenderDist;
				s_dctx.frame_ms        = metricsFrameMs();
				s_dctx.meshes          = chunkRenderMeshes();
				s_dctx.tris            = chunkRenderTris();
				s_dctx.culled          = chunkRenderCulled();
				s_dctx.tps             = s_tps;
				s_dctx.ticks_dropped   = tickClockDropped(&s_tickclock);
				s_dctx.columns         = s_world.columns;
				s_dctx.chunks          = s_world.chunks;
				s_dctx.blocks_bytes    = (uint32_t)worldBytes(&s_world);
				s_dctx.blocks_budget   = (uint32_t)budgetCap();
				s_dctx.player_x        = player.body.x;
				s_dctx.player_y        = player.body.y;
				s_dctx.player_z        = player.body.z;
				const int dist_before  = s_mesh_radius;
				s_debug_open = debugMenuUiUpdate(&s_dctx, &opts.debug_menu, down,
				                                 touch_press, touch.touch_x, touch.touch_y);
				if (s_mesh_radius != dist_before || !s_debug_open) {
					// The assignment is the load-bearing half and was missing until now: the
					// debug menu's slider goes through bsDebugSetRenderDist -> genSetRadius,
					// which clamps and re-meshes the ring live but never touches opts, so this
					// save wrote the OLD number back. Measured symptom: change render distance
					// in the debug menu, watch it apply, reboot, and it is back where it was.
					// Reading s_mesh_radius back rather than the requested value is what makes
					// the saved setting the clamped one — same rule, and same two lines, as the
					// pause menu's `if (dist_step != 0)` path above.
					opts.render_dist = s_mesh_radius;
					optionsSave(&opts, TITLE_OPTIONS_PATH);
				}
			}
			}
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
		} // FRZ-NETFIX, 2026-09-03: if (frame_opened), opened just above spriteFrameBegin()

		// Last in the iteration, so a frame only counts once it has actually finished. The
		// counters go out with it: what the world was doing is half of what a hang report needs
		// to be worth reading, the other half being the phase markers above.
		//
		// FRZ-NETFIX, 2026-09-03. Unconditional on purpose, on the far side of the frame_opened
		// guard above: these are bookkeeping, not GPU work, and they are what keep aptMainLoop()
		// serviced and HOME answering on an iteration whose draw was skipped. watchdogBeat() in
		// particular is the watchdog thread's proof of life — a skipped frame that also skipped
		// this would look exactly like the hang the wedge already survived.
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
	//
	// v1.8.3. And not for a world that was refused. The whole of world/genversion.h is one rule —
	// a world this build cannot account for is left exactly as it was found — and a refusal that
	// wrote an inventory file into it on the way out would break that rule in the one direction
	// it cannot afford: the world was never entered, so anything written here describes a session
	// that did not happen. Same test on the pose immediately below.
	if (inv_dir && !world_refused) inventorySave(&s_inv, inv_dir);

	// v1.8.15, on the identical guard including !world_refused. A refused world is one this
	// build cannot account for and must leave exactly as it was found, and a blockstate sidecar
	// written into it would describe a session that did not happen — the same rule the two lines
	// above and below are both keeping. Written whole for the same reason the inventory is: 2 KB
	// is not worth an SD round trip inside the frame that just broke a block.
	if (inv_dir && !world_refused) (void)blockStateSave(&s_blockstate, inv_dir);

	// v1.7.1 task 46b, and deliberately on this exact line rather than anywhere else on the
	// main thread. app/worker.h:92-97 allows exactly one thread inside the SD's FS service
	// session at a time and names the worker as the owner of every byte; this call is legal
	// here only because saveDirtyColumns() above ends in workerFlushSaves(), so the worker is
	// provably parked, and workerStop() below has not run yet. Inside the frame loop it would
	// be neither.
	//
	// Gated on the same inv_dir the inventory is, and not on a second opinion about the card:
	// NULL is a server session, where the server owns the pose and this console must write
	// nothing. A failed save is not retried and not reported, for inventory.h's reason — the
	// cost of losing a pose is landing at spawn, and stalling the quit to retry is worse.
	//
	// This one line covers every ordinary way a session ends, because they all funnel through
	// here: "Quit to title" from the pause menu, a HOME exit, a power-off, and a lost server
	// session. The lid-close case is the one that does not, and sleepFlushOneColumn() above
	// is where that one is answered.
	//
	// v1.8.3: `&& !world_refused`, for the reason on the inventory save above, and here it is not
	// only a principle. A refused world still reaches this line with a `player` that was built at
	// the generator's spawn point, so without the test a world made by a newer build would have
	// had its saved pose overwritten with (8.5, y, 8.5) by the very code that refused to open it
	// — the player's position lost by the safety check, on a world it could not otherwise touch.
	if (inv_dir && !world_refused) {
		const PlayerPose pose = { player.body.x, player.body.y, player.body.z,
		                          player.cam.yaw, player.cam.pitch };
		(void)playerPoseSave(&pose, inv_dir);
		// v1.8.13 SURV-WIRE. Same line, same guard, same reasoning as the pose it sits under:
		// inv_dir is NULL in a server session (the server owns these vitals and this console
		// writes nothing for them), and !world_refused keeps a world this build could not open
		// exactly as it was found rather than dropping a survival.dat into it describing a
		// session that never happened.
		(void)survivalSave(&survival, inv_dir);
	}

	// v1.8.9 day/night. Same guard, same reasoning, as the inventory and pose saves above: a
	// refused world must leave time.bin exactly as it was found, same as everything else here.
	if (inv_dir && !world_refused) (void)dayNightWrite(inv_dir, dayNightTicks(&s_daynight));

	// The v1.4.0 explored-fog map was saved here on the way out. Gone with the feature in
	// v1.5.1; existing sdmc:/blocksmith/fog/*.bin files are left orphaned by design.

	// The play loop can be left with the menu still up. Choosing Quit closes it on the way
	// out, but a lost server session (the netStatus() break above) does not, and neither
	// would a HOME exit — and pausemenu.c's open flag is a static that outlives this world.
	// Left set, the next world entered would boot straight into a paused menu over a world
	// the player never asked to pause.
	pauseMenuClose();

	// v1.6.0, and before workerStop() rather than after it. Past this line the worker is
	// joined and s_world is about to be freed, so a lid closed during teardown must not be
	// able to reach either. Cleared unconditionally — clearing a hook that was never set
	// because genStart() failed is a no-op, and getting this wrong is a crash on lid-close
	// back at the title screen rather than anything visible here.
	sleepSetFlushHook(NULL);
	sleepSetLeaveHook(NULL);
#if BS_WORLD_GEN
	// v1.7.1 task 46b. Cleared with the hooks and for the same reason: `player` is about to
	// go out of scope, and a hook that could still fire holding a pointer into it is a crash
	// on lid-close rather than anything visible here.
	s_sleep_player   = NULL;
	s_sleep_pose_dir = NULL;
#endif
	// v1.8.13 SURV-WIRE. Cleared with them and for the same reason — `survival` is about to go
	// out of scope, and the bottom-screen draw reads this pointer every frame it runs. Outside
	// the guard because the handover was.
	s_survival = NULL;

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

	// v1.7.1 task 46, and it has to be here rather than in app_shutdown with chunkRenderExit:
	// both paths below can jump back to session_start with the process still alive, and the
	// mesh slot table is per-WORLD state living in a module whose arenas are per-process. Left
	// standing, session 1's slots stay `used` at session 1's coordinates — the next world draws
	// the previous one's geometry there, and cannot claim those slots for its own chunks.
	// Beside worldExit because it is the same act: this world's blocks and this world's meshes
	// stop existing together.
	chunkRenderReleaseAll();

	worldExit(&s_world);
	highlightExit();
	crackOverlayExit();

	// v1.8.9 particle system. Paired with particlesInit() above (both re-run on a loop back
	// to session_start), on the same footing as crackOverlayExit() right above -- see that
	// call's own comment.
	particlesExit();

	playerModelExit();
	entityModelExit();

	// v1.8.8 NEON BIOME BORDERS. Frees the vertex buffer and drops the world pointer, so a
	// fence cannot outlive the WorldGen it was classified from. biomeBorderReset also puts the
	// toggle back to OFF, which is the behaviour a debug option should have across a quit to
	// title: the next world starts in the shipped state, not in whatever state the last
	// session's debugging left behind.
	biomeBorderDrawExit();
	biomeBorderReset();

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

	// A single-player world left through the pause menu goes back to the same place, by the
	// same route. Without this it falls through to app_shutdown and closes the game, which
	// is what a row labelled "Quit to title" must not do.
	if (quit_to_title) goto session_start;

app_shutdown:
	chunkRenderExit();

	// v1.8.9 weather rendering. Paired with weatherDrawInit() at boot, beside chunkRenderInit
	// -- both are true process-level, once-only GPU claims (unlike particlesInit/Exit and
	// crackOverlayInit/Exit above, which re-run every loop back to session_start), so this is
	// the one and only place their exit belongs, beside the other process-level exit here.
	weatherDrawExit();

	metricsExit();
	screenExit();
	// Before netExit, which is what closes the SOC session the updater was told to share: the
	// updater's own worker thread is joined inside updaterExit, and joining it after its
	// sockets had been pulled out from under it would be the same ordering mistake workerStop
	// above exists to avoid.
	updaterExit();
	sleepExit();
	batteryExit();
	// v1.8.9 audio. Before netExit for no ordering reason of its own -- audio and net do not
	// touch each other -- placed here simply because this is the last line before netExit and
	// the task's own recipe put it there.
	audioShutdown();
	netExit();
	psExit();
	crashExit();
	return 0;
}
