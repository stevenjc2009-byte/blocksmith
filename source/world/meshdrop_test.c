// Host self-test for the "mesh refusal becomes a permanent hole" bug in main.c's streaming
// ring.
//
// ── THE BUG ────────────────────────────────────────────────────────────────────────────
//
// genDrainMesh() (main.c) pops a JOB_MESH off s_meshq and calls chunkRenderBuild(). On
// failure it only does s_genr.mesh_refused++ — the job is gone. Meanwhile s_col_queued's bit
// for that column was set the moment the job was PUSHED (genQueueReadyColumns), and
// genQueueReadyColumns refuses to re-push any column whose bit is already set. The bit is
// cleared only when the column leaves the mesh ring (genUnloadColumn / genRecenter /
// genSetRadius) or on a fresh genStart(). So a column whose mesh build is refused becomes a
// permanent hole in the terrain that heals only if the player walks far enough away and back
// — not by standing still, however many frames pass.
//
// ── WHY THIS IS EXTRACTED, NOT HAND-COPIED ────────────────────────────────────────────────
//
// main.c includes <3ds.h>, <citro3d.h> and carries main(), so it cannot be compiled on the
// host. This project's rule is that a test links the real module, never a hand-copied
// duplicate (see run_host_tests.sh's own record of tests/battery_test.c and
// tests/sleep_test.c passing with the real module deleted) — so the functions under test are
// lifted out of source/main.c by tools/run_host_tests.sh, exactly the way tests/horizon_test.c
// and source/world/cavewalk_test.c already lift caveWalk() out of chunk_render.c. The awk
// range runs from each function's signature to its closing brace and is #included below as
// meshdrop_extract.inc. There is no second copy of genQueueReadyColumns or genDrainMesh in
// this file — sabotage main.c's real function and this binary goes red; break the awk anchor
// and this binary does not compile, which the #error in the .inc (see run_host_tests.sh) makes
// explicit instead of silently linking nothing.
//
// Anchors extracted: genWrap, genSlot, genSlotHas, genSlotSet, genSlotClear, genInArea,
// genInMesh, genColumnInstalled, genColumnRingComplete, genQueueReadyColumns, genDrainMesh.
// genColumnInstalled and genInArea are not on the caller's original naming list (genSlot*,
// genWrap, genInMesh, genColumnRingComplete, genQueueReadyColumns, genDrainMesh) but are
// pulled in anyway because genColumnRingComplete calls genColumnInstalled, which calls
// genInArea — leaving either behind would make this file fail to compile with an implicit
// declaration instead of exercising the real ring-complete test the bug depends on.
//
// world/meshq.c, world/jobq.c and scene/ringorder.c are NOT extracted — they are host-clean
// and linked for real, so the queue and the visit order these checks observe are the ones the
// console runs. world/world.c drags in block.c, registry.c, chunk.c and budget.c; scratch.c
// and tests/net_stub.c are linked for the same reason interact_test.c and playerpose_test.c
// link them — world.c's worldSet() reaches into net/networld.h through a hook, and
// tests/net_stub.c supplies the stub that satisfies it on the host. -lm is for meshq.c's
// (none) — carried over from the sibling stanzas' convention regardless.
//
// The few things genQueueReadyColumns/genDrainMesh touch that ARE hardware are stubbed below:
// svcGetSystemTick, CPU_TICKS_PER_MSEC, loadprofMark/loadprofSince (both no-ops, matching the
// real "profiler not armed" contract — loadprofMark() returns 0, loadprofSince() with mark==0
// is defined to do nothing), and chunkRenderBuild() itself, which is the instrument: it
// refuses the target chunk's first build and succeeds on every one after, standing in for a
// transient pool-full refusal.
//
// The __3DS__ guard is load-bearing: the console Makefile globs every .c under source/world,
// and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debug/loadprof.h"   // LoadStage / LOAD_STAGE_MESH only — loadprofMark/Since stubbed below
#include "scene/ringorder.h"  // real ringOrder()/RING_ORDER_COUNT/RingOffset, and RENDER_DIST_* via render_dist.h
#include "world/block.h"      // BLOCK_STONE
#include "world/genretry.h"   // real GenRetryLedger/genRetryInit/Mark/Tick/Clear/Outstanding
#include "world/jobq.h"       // real Job/JobQueue/jobqInit/Push/Pop/Count
#include "world/meshq.h"      // real meshqPushColumn
#include "world/world.h"      // real World/worldInit/worldSet

static int g_checks = 0;
static int g_fails  = 0;

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   line %d: %s\n", line, what);
	} else {
		printf("  ok     %s\n", what);
	}
}
#define CHECK(cond, what) checkAt((cond), (what), __LINE__)

// ── The state genQueueReadyColumns/genDrainMesh read and write ─────────────────────────
//
// Same names and same types as main.c's, because the extracted functions refer to them by
// name. GEN_MESH_SPAN/GEN_AREA_SPAN are reproduced by the same formula main.c uses
// (GEN_MESH_RADIUS_MAX = RENDER_DIST_MAX, GEN_AREA_RADIUS_MAX = GEN_MESH_RADIUS_MAX + 1) off
// the real scene/render_dist.h, not hand-picked, so the arrays are sized exactly as wide as
// the ring the extracted code is ever asked to address.
#define GEN_MESH_RADIUS_MAX  RENDER_DIST_MAX
#define GEN_AREA_RADIUS_MAX  (GEN_MESH_RADIUS_MAX + 1)
#define GEN_MESH_SPAN (2 * GEN_MESH_RADIUS_MAX + 1)
#define GEN_AREA_SPAN (2 * GEN_AREA_RADIUS_MAX + 1)

typedef struct {
	int32_t cx, cz;
	bool    set;
} ColSlot;

static ColSlot s_col_in[GEN_AREA_SPAN][GEN_AREA_SPAN];
static ColSlot s_col_queued[GEN_MESH_SPAN][GEN_MESH_SPAN];

static int32_t s_center_cx, s_center_cz;
static int s_mesh_radius = RENDER_DIST_MIN;
static int s_area_radius = RENDER_DIST_MIN + 1;

static bool s_mesh_queue_short;

static World s_world;
static JobQueue s_meshq;

// Only the two fields genDrainMesh/genQueueReadyColumns actually touch — see the cavewalk_test.c
// convention this mirrors: give the mirrored struct only what the extracted code reads.
typedef struct {
	int meshed;
	int mesh_refused;
	// v1.8.11. The fix splits the refusal total by cause, because the two causes want opposite
	// handling — see scene/chunk_render.h's ChunkRefuseReason. genDrainMesh writes both, so
	// both have to exist here or the extracted text will not compile.
	int mesh_refused_pool;
	int mesh_refused_overflow;
} GenResult;
static GenResult s_genr;

// v1.8.11. The mesh-side retry ledger. State, so it is mirrored here rather than extracted —
// same convention as s_col_queued and s_genr above; only FUNCTIONS come out of main.c, because
// a hand-copied function would be a second implementation and would prove nothing about the
// one that ships. The ledger itself is the real world/genretry.c, linked, not stubbed.
static GenRetryLedger s_mesh_retry;

// ── Hardware stubs ──────────────────────────────────────────────────────────────────────
//
// ctrulib's tick type. genDrainMesh's extracted body reads `const u64 t0 = svcGetSystemTick();`
// verbatim — u64 is <3ds/types.h>'s typedef, not std C, so it is reproduced here rather than
// silently widened to uint64_t (which would compile even if a real build's u64 were ever
// something narrower — it is not, but the extraction is supposed to prove the real text
// compiles, not a lightly-adjusted copy of it).
typedef uint64_t u64;

// svcGetSystemTick/CPU_TICKS_PER_MSEC: genDrainMesh only uses these to decide whether to stop
// early inside its while loop. Every fixture below pushes at most one job, so the loop pops
// it, finds the queue empty and exits on its own — the budget check is never the thing that
// stops it. The fake tick just has to be non-zero-denominator and monotonic.
static u64 s_fake_tick;
u64 svcGetSystemTick(void) { return ++s_fake_tick; }
#define CPU_TICKS_PER_MSEC 1000.0

// loadprofMark/Since: matches the real "profiler not armed" contract exactly (loadprof.h) —
// Mark returns 0, and Since is specified to no-op when mark == 0. Genuinely not faked, just
// permanently in the disarmed state the console is in outside a world load.
uint64_t loadprofMark(void) { return 0; }
void loadprofSince(LoadStage s, uint64_t mark) { (void)s; (void)mark; }

// Not called by anything extracted here (genQueueReadyColumns pushes through meshqPushColumn,
// not workerSubmitColumn), but listed among the hardware calls the caller named, so it is
// stubbed for completeness / to keep this file safe if a future extraction pulls in
// genRequestArea or genRetrySubmit alongside these.
bool workerSubmitColumn(int32_t cx, int32_t cz) { (void)cx; (void)cz; return true; }

// The real ChunkRefuseReason enum, lifted verbatim out of scene/chunk_render.h at build time.
// That header cannot simply be #included on the host — it pulls in citro3d. Extracted rather
// than retyped for the same reason every function here is extracted: a hand-copied enum could
// drift from the shipping one, and this test would go on passing while steering its stub with
// values the real genDrainMesh no longer uses.
#include "meshdrop_refuse.inc"

#ifndef BS_MESHDROP_REFUSE_OK
#error "meshdrop_refuse.inc did not carry ChunkRefuseReason out of source/scene/chunk_render.h"
#endif

// chunkRenderBuild(): the instrument. Refuses the TARGET chunk's first `s_refuse_first_n`
// builds (a stand-in for chunk_render.c's real CHUNK_REFUSE_POOL — transient, not
// CHUNK_REFUSE_OVERFLOW) and succeeds every time after. Any other chunk always succeeds; none
// of the fixtures below ever ask for one.
#define TARGET_CX 0
#define TARGET_CY 4
#define TARGET_CZ 0

static int s_build_calls_total;
static int s_build_calls_target;
static int s_refuse_first_n;

// v1.8.11. Mirrors chunk_render.c's own s_last_refuse: set on every refusal, cleared on entry
// so a success leaves it NONE. s_refuse_reason picks which class this fixture simulates —
// POOL by default (transient, and the class the fix retries). A fixture that sets it to
// OVERFLOW is asserting the opposite behaviour: that the fix does NOT retry.
static ChunkRefuseReason s_last_refuse;
static ChunkRefuseReason s_refuse_reason = CHUNK_REFUSE_POOL;

bool chunkRenderBuild(const World* w, int cx, int cy, int cz)
{
	(void)w;
	s_last_refuse = CHUNK_REFUSE_NONE;
	s_build_calls_total++;
	if (cx == TARGET_CX && cy == TARGET_CY && cz == TARGET_CZ) {
		s_build_calls_target++;
		if (s_build_calls_target <= s_refuse_first_n) {
			s_last_refuse = s_refuse_reason;
			return false;
		}
	}
	return true;
}

ChunkRefuseReason chunkRenderLastRefusal(void) { return s_last_refuse; }

// The real thing. Generated at build time by tools/run_host_tests.sh — see the file comment.
#include "meshdrop_extract.inc"

#ifndef BS_MESHDROP_EXTRACT_OK
#error "meshdrop_extract.inc did not carry genQueueReadyColumns()/genDrainMesh() and their helper functions out of source/main.c"
#endif

// ── Fixture ──────────────────────────────────────────────────────────────────────────────

static void resetAll(void)
{
	memset(s_col_in, 0, sizeof(s_col_in));
	memset(s_col_queued, 0, sizeof(s_col_queued));
	s_center_cx = s_center_cz = 0;
	s_mesh_radius = RENDER_DIST_MIN;
	s_area_radius = RENDER_DIST_MIN + 1;
	s_mesh_queue_short = false;

	worldInit(&s_world);
	jobqInit(&s_meshq);
	s_genr = (GenResult){0};

	s_fake_tick = 0;
	s_build_calls_total = 0;
	s_build_calls_target = 0;
	s_refuse_first_n = 0;
	s_last_refuse = CHUNK_REFUSE_NONE;
	s_refuse_reason = CHUNK_REFUSE_POOL;

	// Same call and same span main.c's genStart uses (main.c:1657). GEN_MESH_SPAN, not
	// GEN_AREA_SPAN: the ledger indexes by the mesh ring, and the wider span would wrap onto
	// the wrong slots.
	genRetryInit(&s_mesh_retry, GEN_MESH_SPAN);
}

// One simulated frame of main.c's real per-frame order. Not genDrainMesh alone: the loading
// loop (main.c:1963-1967) ticks the mesh ledger and THEN drains, deliberately in that order so
// a column the tick re-pushes gets its chance in the same frame's drain. The play loop
// (main.c:4968) ticks it under the same pause gate. A fixture that called only genDrainMesh
// would be testing a call sequence the program never performs, and on the fixed code it would
// stay red for a reason that has nothing to do with the bug.
static void frameStep(void)
{
	genRetryTick(&s_mesh_retry, s_center_cx, s_center_cz, s_mesh_radius, meshRetrySubmit,
	             &s_mesh_retry);
	genDrainMesh(1000.0f, 64);
}

// Installs every column of the 3x3 block centred on (cx, cz) — exactly what
// genColumnRingComplete(cx, cz) requires before it will call the column ring-complete.
static void installNeighbourhood(int32_t cx, int32_t cz)
{
	for (int32_t dz = -1; dz <= 1; dz++)
		for (int32_t dx = -1; dx <= 1; dx++)
			genSlotSet(&s_col_in[0][0], GEN_AREA_SPAN, cx + dx, cz + dz);
}

// One real block in the target column's chunk (TARGET_CX, TARGET_CY, TARGET_CZ), so
// meshqPushColumn (the real world/meshq.c) finds a non-air chunk there and actually pushes a
// JOB_MESH instead of skipping an empty column — chunkIsAllAir chunks are deliberately never
// queued (world/meshq.h's own comment on why). Block coordinates, not chunk coordinates:
// chunk (0, 4, 0) is blocks x 0..15, y 64..79, z 0..15.
static void seedTargetColumn(void)
{
	CHECK(worldSet(&s_world, TARGET_CX * 16, TARGET_CY * 16, TARGET_CZ * 16, BLOCK_STONE),
	      "setup: worldSet placed one real block in the target chunk");
}

// ── testMeshRefusalIsAPermanentHoleOnHead ────────────────────────────────────────────────
//
// The bug, reproduced end to end: install the target column's neighbourhood, run one real
// genQueueReadyColumns() pass (pushes the target chunk's JOB_MESH and sets s_col_queued),
// then drive genDrainMesh() once per frame for many frames with the player standing still —
// no genRecenter, no genInstallOne — exactly the real per-frame call genDrainMesh() gets from
// main.c's play loop (main.c:5174) once genFollow (main.c:4893) finds the player has not
// moved. chunkRenderBuild refuses the very first attempt and would succeed on every attempt
// after, if it were ever asked again.
//
// On the buggy code this never recovers: frame 1 pops the job, chunkRenderBuild refuses it,
// and the job is gone. s_col_queued's bit for (0,0) is still set, so genQueueReadyColumns
// (main.c:940) will not push it again, and nothing in genDrainMesh's own short-queue path
// (main.c:1710, gated on s_mesh_queue_short, which a build refusal never sets) ever calls
// genQueueReadyColumns again either. The column is meshed on exactly zero of MAX_FRAMES
// frames.
#define MAX_FRAMES 1000

static void testMeshRefusalIsAPermanentHoleOnHead(void)
{
	resetAll();
	s_refuse_first_n = 1;   // one transient refusal, like a pool that is full for one frame

	seedTargetColumn();
	installNeighbourhood(0, 0);

	genQueueReadyColumns();
	CHECK(jobqCount(&s_meshq) == 1, "setup: one JOB_MESH queued for the target column's one non-air chunk");
	CHECK(genSlotHas(&s_col_queued[0][0], GEN_MESH_SPAN, 0, 0),
	      "setup: s_col_queued marks (0,0) queued the moment the job was pushed");

	int frames_to_recover = -1;
	for (int frame = 1; frame <= MAX_FRAMES; frame++) {
		frameStep();
		// control: never touches genRecenter/genFollow — the player has not moved, so this
		// loop's only claim on the ring is genInMesh, exercised here to confirm the column
		// never left the mesh ring and the hole is not a ring-boundary artifact.
		if (frame == 1 || frame == MAX_FRAMES)
			CHECK(genInMesh(0, 0), "control: (0,0) is still inside the mesh ring (player stood still)");
		if (s_genr.meshed > 0) { frames_to_recover = frame; break; }
	}

	CHECK(frames_to_recover > 0 && frames_to_recover <= MAX_FRAMES,
	      "THE BUG: the column is re-queued and meshed within MAX_FRAMES frames of standing still after one refused build");

	if (frames_to_recover < 0) {
		printf("  DIAGNOSTIC: after %d frames standing still — s_genr.meshed=%d "
		       "s_genr.mesh_refused=%d jobqCount=%d s_col_queued(0,0)=%s "
		       "chunkRenderBuild(target) called %d time(s) total, %d call(s) for chunk (0,0,4)\n",
		       MAX_FRAMES, s_genr.meshed, s_genr.mesh_refused, jobqCount(&s_meshq),
		       genSlotHas(&s_col_queued[0][0], GEN_MESH_SPAN, 0, 0) ? "still set" : "cleared",
		       s_build_calls_total, s_build_calls_target);
	}

	// This is the smoking gun regardless of whether the timing assertion above happened to
	// pass: exactly one refusal occurred, the job queue emptied, and the queued bit for the
	// column that lost its job is STILL SET — which is precisely the state genQueueReadyColumns
	// reads as "nothing to do here" forever after.
	CHECK(s_genr.mesh_refused == 1, "control: chunkRenderBuild refused the job exactly once");
	CHECK(jobqCount(&s_meshq) == 0, "control: the mesh queue is empty (the refused job is gone, not retried in place)");
	if (frames_to_recover < 0) {
		CHECK(!genSlotHas(&s_col_queued[0][0], GEN_MESH_SPAN, 0, 0),
		      "THE BUG's mechanism: s_col_queued(0,0) was cleared so the column could be re-queued (it was not)");

		// What actually heals it, demonstrated with the real genSlotClear/genQueueReadyColumns —
		// not asserted, just shown, so a reader can see the bit is the whole story: nothing else
		// about the column's state changed since the stuck loop above (the world content, the
		// installed neighbourhood and the JOBQ_CAP-worth of free ring space are all still there).
		genSlotClear(&s_col_queued[0][0], GEN_MESH_SPAN, 0, 0);
		genQueueReadyColumns();
		genDrainMesh(1000.0f, 64);
		CHECK(s_genr.meshed > 0,
		      "DEMONSTRATION: clearing s_col_queued(0,0) by hand is the ONLY thing this file "
		      "found that lets the same column mesh — genQueueReadyColumns re-pushes it the "
		      "instant the bit is gone, and chunkRenderBuild (already past its one simulated "
		      "refusal) accepts it immediately");
	}
}

// ── testCleanBuildStillMeshesNormally ────────────────────────────────────────────────────
//
// Control for the whole file: with no refusal at all, one genQueueReadyColumns() pass and one
// genDrainMesh() call mesh the column on the very first frame. Proves the fixture (world
// content, neighbourhood install, queue push) is sound and that a failure above is really
// about the refusal-recovery path, not about the harness never being able to mesh anything.
static void testCleanBuildStillMeshesNormally(void)
{
	resetAll();
	s_refuse_first_n = 0;

	seedTargetColumn();
	installNeighbourhood(0, 0);
	genQueueReadyColumns();
	CHECK(jobqCount(&s_meshq) == 1, "control fixture: one JOB_MESH queued");

	genDrainMesh(1000.0f, 64);
	CHECK(s_genr.meshed == 1, "control: an unrefused build meshes on the very first frame");
	CHECK(s_genr.mesh_refused == 0, "control: no refusals when chunkRenderBuild never refuses");
}

// ── testOverflowIsNotRetried ─────────────────────────────────────────────────────────────
//
// The other half of the fix, and the reason CHUNK_REFUSE_POOL and CHUNK_REFUSE_OVERFLOW are
// distinguished at all. An OVERFLOW refusal means this chunk's own geometry exceeds the
// per-chunk vertex/index cap: meshing it again produces the same overflow, by definition. So
// it must NOT enter the retry ledger — a ledger entry would resubmit it every backoff period
// forever, burning drain budget that every meshable chunk is queued behind, and turning a
// one-chunk hole into a world-wide stall.
//
// This fixture exists so the file cannot be satisfied by a fix that simply retries every
// failure. Such a fix passes testMeshRefusalIsAPermanentHoleOnHead and fails here. The refusal
// is made permanent (s_refuse_first_n far above MAX_FRAMES) because that is what an overflow
// genuinely is — the point is that the ledger stays empty, not that the chunk recovers.
static void testOverflowIsNotRetried(void)
{
	resetAll();
	s_refuse_reason  = CHUNK_REFUSE_OVERFLOW;
	s_refuse_first_n = MAX_FRAMES * 10;

	seedTargetColumn();
	installNeighbourhood(0, 0);
	genQueueReadyColumns();
	CHECK(jobqCount(&s_meshq) == 1, "overflow fixture: one JOB_MESH queued");

	for (int frame = 1; frame <= MAX_FRAMES; frame++) frameStep();

	CHECK(s_genr.mesh_refused_overflow == 1,
	      "overflow: counted exactly once — the chunk was attempted once and never resubmitted");
	CHECK(s_genr.mesh_refused_pool == 0,
	      "overflow: not miscounted as the recoverable pool class");
	CHECK(genRetryOutstanding(&s_mesh_retry) == 0,
	      "overflow: the retry ledger is EMPTY — an unmeshable chunk must never be queued for retry");
	CHECK(s_build_calls_target == 1,
	      "overflow: chunkRenderBuild was called for the target exactly once across 1000 frames "
	      "(a retry loop here would spin forever and starve the drain budget)");
}

int main(void)
{
	testMeshRefusalIsAPermanentHoleOnHead();
	testCleanBuildStillMeshesNormally();
	testOverflowIsNotRetried();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif   // __3DS__
