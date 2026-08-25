// Host self-test for the cave cull's per-frame ORCHESTRATOR — source/scene/chunk_render.c's
// caveWalk().
//
// The algorithm underneath it (world/visgraph.c: visWalkBegin/Set/Run/Visible) has had host
// coverage since Phase 7, in world_test.c. caveWalk itself had none. That is the half that
// decides, every frame, WHICH chunks the algorithm is even told about, how big a box to run it
// over, whether to re-run it at all, and — critically — whether the renderer is allowed to
// trust the answer. Every one of those decisions can be wrong while visgraph.c is perfect, and
// three of them fail in the direction visgraph.h calls out in bold: over-culling, which is a
// hole in the world rather than a missed optimisation.
//
// ── What is actually under test, and how it gets here ──────────────────────────────────
//
// caveWalk() is `static` inside a file that cannot be compiled on the host: chunk_render.c
// includes <3ds.h>, <citro3d.h> and the picasso-generated shader headers. So its source text is
// lifted out by tools/run_host_tests.sh — an awk range from its signature to its closing brace
// — and #included below as cavewalk_extract.inc, together with chunkOfFloat() and
// cullInvalidate(), the two other statics its contract depends on.
//
// This is the same mechanism tests/horizon_test.c already uses on the same file, and it is
// chosen for the same reason: the alternative is hand-copying the function into the test, which
// is precisely the "a test that links nothing tests nothing" failure this project has already
// paid for twice (see tests/battery_test.c and run_host_tests.sh's meshq entry). There is no
// second copy of caveWalk in this file. Sabotage chunk_render.c and this binary goes red;
// break the awk anchor and this binary does not COMPILE, which the #error below makes explicit.
//
// visgraph.c is NOT extracted — it is host-clean and is linked for real, so the walk these
// checks observe is the walk the console runs, not a stand-in.
//
// ── The check families, and what each one catches that its neighbours do not ────────────
//
//   testChunkOfFloat      caveWalk keys its frame cache with chunkOfFloat(); visWalkRun starts
//                         its flood fill from visgraph.c's chunkOfBlock(). Two different
//                         functions, in two different files, that MUST name the same chunk or
//                         the cache is keyed on a chunk the walk never started from. Swept,
//                         including the negative side, where a truncating cast (the bug the
//                         floorf is there to prevent) puts everything in the first chunk west
//                         of the origin into chunk 0.
//
//   testBoxFromUsedSlots  The bounding box is min/max over the slots that are `used`. Catches
//                         a box built from ALL slots (a stale slot at a far coordinate then
//                         blows past VIS_BOX_MAX_XZ and the whole cull silently switches off),
//                         a box that ignores negative coordinates, and a box off by one on any
//                         axis (the far corner slot falls outside it and stops being culled).
//
//   testRefusalIsSafe     Every path that does NOT complete a walk must leave s_cave_ran false,
//                         because chunkRenderDraw reads s_cave_ran as "may I skip chunks".
//                         Three refusals: nothing loaded, a box too big for VisWalk, and a
//                         refusal arriving after a good walk — that last one is the dangerous
//                         one, because a stale-but-valid cache would cull against a box that
//                         no longer describes the world.
//
//   testEmptySlotsFedIn   A slot with index_count == 0 (a chunk the player has dug out, or one
//                         that meshed to nothing) is still inserted, with its real mask and
//                         drawable=false. Catches BOTH ways to get this wrong: skipping such
//                         slots leaves a VIS_ALL_CONNECTED hole that sight pours through
//                         (under-cull), and marking them drawable makes the cull claim a chunk
//                         is on screen when it draws no triangles.
//
//   testHolesAreSeeThrough  caveWalk builds a DENSE box and fills only the coordinates it has
//                         slots for. Every other cell is a hole — the unmeshed sky is the
//                         normal case — and a hole must let sight through. Read as a wall it
//                         culls everything behind it. This family exists because sabotaging
//                         visWalkBegin's 0xFF fill to 0x00 left the rest of this suite GREEN:
//                         every other fixture here fills its box completely.
//
//   testFrameCache        The cache is the whole reason caveWalk is cheap: it must re-use the
//                         previous walk while the camera stays in one chunk, and it must re-run
//                         the moment the camera crosses a boundary or cullInvalidate() fires.
//                         Counted through s_walk_runs, so "re-ran when it should not have" and
//                         "did not re-run when it had to" are separate, opposite failures.
//
//   testCacheKeyIsSound   The property caveWalk's own comment claims and its frame cache bets
//                         on: the walk's answer is "a function of the camera's *chunk* ... and
//                         not the camera's position inside that chunk". It was FALSE until
//                         v1.8.2 — at an exact chunk boundary the walk reached one extra chunk
//                         — and it is checked here on all three axes, because the comparison
//                         that makes it true is a separate line of visgraph.c per axis and two
//                         of them have no other coverage in this file. See the long note on
//                         that function before "fixing" a red here.
//
//   testDeterminism       Same slots, same camera, same answer — twice, from a cold cache, with
//                         unrelated slot churn in between. Worldgen reproduces a world from a
//                         seed forever; the cull must not make the same world look different
//                         from one visit to the next.
//
//   testNoSlotOverrun     Sentinel arrays either side of s_slots and of the VisWalk, checked
//                         after the widest legal box and after coordinates at both extremes.
//                         Catches an index that walks off the pool, which on the console is not
//                         a wrong picture but a crash.
//
// Controls that must stay GREEN in every sabotage arm are named "control:" and are chosen to be
// independent of caveWalk: they exercise visPairIndex, the out-of-box answer, and the sentinels.
//
// The __3DS__ guard is load-bearing, not tidy: the console Makefile globs every .c under
// source/world and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/chunk.h"      // CHUNK_DIM — the real one, not a number retyped here
#include "world/visgraph.h"   // the REAL walk, linked not extracted

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

// ── The state caveWalk() reads and writes ──────────────────────────────────────────────
//
// Same names and same types as chunk_render.c's, because the extracted function refers to them
// by name. Only the fields caveWalk actually touches are declared: giving MeshSlot its real
// 100-plus bytes of vertex bookkeeping would mean dragging in MeshVertex and the mesher's
// face-bucket layout for fields the function under test never reads.
//
// MESH_SLOTS is 64 here rather than the console's 294/392. caveWalk reads the bound as a loop
// limit and nothing else — it never assumes the pool is a particular size — and a smaller pool
// keeps the sentinel checks below able to name the slot that was overrun. horizon_test.c
// resizes HZN_MAX_COLUMNS for the same reason, in the other direction.
#define MESH_SLOTS 64

typedef struct {
	int      cx, cy, cz;
	uint32_t index_count;
	bool     used;
	uint16_t vis_mask;
} MeshSlot;

// Sentinels either side of the pool. caveWalk indexes s_slots with a plain 0..MESH_SLOTS loop,
// so an off-by-one in either direction lands in one of these rather than in unrelated statics,
// where it would be invisible.
#define SENTINEL_SLOTS 8
static MeshSlot s_guard_lo[SENTINEL_SLOTS];
static MeshSlot s_slots[MESH_SLOTS];
static MeshSlot s_guard_hi[SENTINEL_SLOTS];

static uint8_t  s_walk_guard_lo[256];
static VisWalk  s_walk;
static uint8_t  s_walk_guard_hi[256];

static float s_cam_x, s_cam_y, s_cam_z;

static bool s_cull_valid;
static bool s_walk_valid;
static int  s_walk_cx, s_walk_cy, s_walk_cz;
static int  s_walk_runs;

static bool s_cave_ran;

// The real thing. Generated at build time by tools/run_host_tests.sh — see the file comment.
#include "cavewalk_extract.inc"

// If the awk anchors in run_host_tests.sh ever stop matching (chunk_render.c reformatted, the
// function renamed, the file moved), the extract comes out empty or partial. Without these the
// build would fail with a bare "undefined reference to caveWalk" at link time; with them it
// fails here, at compile time, saying which anchor was lost. Either way it fails LOUDLY, which
// is the point: this test carries no fallback copy to quietly pass against.
#ifndef BS_CAVEWALK_EXTRACT_OK
#error "cavewalk_extract.inc did not carry caveWalk() out of source/scene/chunk_render.c"
#endif

// ── Fixture helpers ────────────────────────────────────────────────────────────────────

static void resetPool(void)
{
	memset(s_guard_lo, 0, sizeof(s_guard_lo));
	memset(s_slots,    0, sizeof(s_slots));
	memset(s_guard_hi, 0, sizeof(s_guard_hi));
	memset(s_walk_guard_lo, 0xA5, sizeof(s_walk_guard_lo));
	memset(s_walk_guard_hi, 0xA5, sizeof(s_walk_guard_hi));

	s_walk_valid = false;
	s_cull_valid = false;
	s_walk_cx = s_walk_cy = s_walk_cz = 0;
	s_walk_runs = 0;
	s_cave_ran  = false;
}

static bool guardsIntact(void)
{
	for (int i = 0; i < SENTINEL_SLOTS; i++) {
		if (s_guard_lo[i].used || s_guard_lo[i].cx || s_guard_lo[i].cy || s_guard_lo[i].cz ||
		    s_guard_lo[i].vis_mask || s_guard_lo[i].index_count) return false;
		if (s_guard_hi[i].used || s_guard_hi[i].cx || s_guard_hi[i].cy || s_guard_hi[i].cz ||
		    s_guard_hi[i].vis_mask || s_guard_hi[i].index_count) return false;
	}
	for (unsigned i = 0; i < sizeof(s_walk_guard_lo); i++)
		if (s_walk_guard_lo[i] != 0xA5 || s_walk_guard_hi[i] != 0xA5) return false;
	return true;
}

static int s_next_slot;

static void addSlot(int cx, int cy, int cz, uint16_t mask, uint32_t index_count)
{
	MeshSlot* s = &s_slots[s_next_slot++];
	s->used = true;
	s->cx = cx; s->cy = cy; s->cz = cz;
	s->vis_mask = mask;
	s->index_count = index_count;
}

// chunkRenderDraw's cullFrame clears s_cave_ran before every caveWalk() (chunk_render.c's
// "s_cave_ran = false;"), which is what makes a refusal mean "draw everything". Modelled here
// so a refusal is tested the way the renderer really reaches it, rather than against whatever
// the previous check happened to leave behind.
static void frame(float cam_x, float cam_y, float cam_z)
{
	s_cam_x = cam_x; s_cam_y = cam_y; s_cam_z = cam_z;
	s_cave_ran = false;
	caveWalk();
}

#define PAIR(a, b)  ((uint16_t)(1u << visPairIndex((a), (b))))

// ── testChunkOfFloat ───────────────────────────────────────────────────────────────────

// visgraph.c's chunkOfBlock() is static, so it cannot be called from here. It is
// `(int)floorf(block) >> 4` and it is the function visWalkRun really starts from; this is the
// oracle, and it is allowed to be a copy because it is the REFERENCE, not the code under test.
static int chunkOfBlockRef(float block)
{
	return (int)floorf(block) >> 4;
}

static void testChunkOfFloat(void)
{
	puts("\n-- chunkOfFloat: the cache key must name the chunk the walk starts from --");

	CHECK(chunkOfFloat(0.0f) == 0,      "the origin block is in chunk 0");
	CHECK(chunkOfFloat(15.999f) == 0,   "the last block of chunk 0 is still chunk 0");
	CHECK(chunkOfFloat(16.0f) == 1,     "the first block of chunk 1 is chunk 1");

	// The whole reason chunkOfFloat uses floorf instead of a cast. A cast truncates toward
	// zero, so every one of these would answer 0 and the cache would stop noticing the
	// boundary crossing exactly where the world is most likely to be wrong.
	CHECK(chunkOfFloat(-0.001f) == -1,  "one thousandth of a block west of the origin is chunk -1");
	CHECK(chunkOfFloat(-1.0f) == -1,    "one block west of the origin is chunk -1");
	CHECK(chunkOfFloat(-15.999f) == -1, "the far end of chunk -1 is still chunk -1");
	CHECK(chunkOfFloat(-16.0f) == -1,   "the first block of chunk -1 is chunk -1");
	CHECK(chunkOfFloat(-16.001f) == -2, "one thousandth past it is chunk -2");

	// The cross-module contract, swept. Every quarter-block over sixteen chunks either side
	// of the origin: two functions in two files that must never disagree about which chunk a
	// camera is in.
	int disagreements = 0;
	int first_bad = 0;
	for (int q = -1024; q <= 1024; q++) {
		const float v = (float)q * 0.25f;
		if (chunkOfFloat(v) != chunkOfBlockRef(v)) {
			if (!disagreements) first_bad = q;
			disagreements++;
		}
	}
	if (disagreements)
		printf("  note   first disagreement at %.2f: chunkOfFloat %d, chunkOfBlock %d\n",
		       (float)first_bad * 0.25f, chunkOfFloat((float)first_bad * 0.25f),
		       chunkOfBlockRef((float)first_bad * 0.25f));
	CHECK(disagreements == 0,
	      "chunkOfFloat agrees with visgraph's chunkOfBlock at all 2049 swept positions");

	CHECK(CHUNK_DIM == 16, "control: CHUNK_DIM is still 16, which both functions assume");
}

// ── testBoxFromUsedSlots ───────────────────────────────────────────────────────────────

static void testBoxFromUsedSlots(void)
{
	puts("\n-- the walk box is min/max over USED slots --");

	// A 3x1x1 line of open chunks with a sealed one in the middle. The camera is in the west
	// chunk, so the east chunk is reachable only through the sealed one — it must be culled.
	// That is the answer the box has to be right for: get the box wrong and the east chunk
	// either falls outside it (and answers "visible", the out-of-box default) or is never
	// walked to at all.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 100);   // sealed
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "a three-chunk box walks");
	CHECK(visWalkVisible(&s_walk, 0, 0, 0), "the camera's own chunk is visible");
	CHECK(visWalkVisible(&s_walk, 1, 0, 0), "the sealed chunk itself is on screen");
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0),
	      "the chunk behind the sealed one is culled, so the box really reached it");

	// The same three chunks at negative coordinates. If the box origin were pinned to 0, or
	// computed as a size rather than a min, these three would land outside it and the sealed
	// chunk would stop culling anything.
	resetPool(); s_next_slot = 0;
	addSlot(-5, -2, -7, VIS_ALL_CONNECTED, 100);
	addSlot(-4, -2, -7, 0,                 100);
	addSlot(-3, -2, -7, VIS_ALL_CONNECTED, 100);
	frame(-5.0f * 16.0f + 8.0f, -2.0f * 16.0f + 8.0f, -7.0f * 16.0f + 8.0f);

	CHECK(s_cave_ran, "a box at negative chunk coordinates walks");
	CHECK(visWalkVisible(&s_walk, -5, -2, -7), "the camera's own chunk is visible at -5,-2,-7");
	CHECK(!visWalkVisible(&s_walk, -3, -2, -7),
	      "the negative-coordinate box still culls what the sealed chunk hides");

	// An UNUSED slot holding a far-away stale coordinate. This is the state the pool is really
	// in — chunkRenderReleaseColumn clears `used` and leaves cx/cy/cz where they were — so a
	// box built without the `used` test would be 101 chunks wide, visWalkBegin would refuse
	// it, and the entire cave cull would switch itself off for as long as that slot sat there.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 100);
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);
	s_slots[40].used = false;
	s_slots[40].cx = 100; s_slots[40].cy = 0; s_slots[40].cz = 100;
	s_slots[40].vis_mask = VIS_ALL_CONNECTED;
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "a released slot's stale coordinate does not blow the box out");
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0),
	      "and the cull still works with that stale slot in the pool");

	// The far corner of a box that is exactly at the limit on every axis: 16 x 8 x 16, the
	// largest VisWalk can hold. An off-by-one in any of the three `max - min + 1` expressions
	// drops this corner out of the box, where it silently answers "visible" forever.
	resetPool(); s_next_slot = 0;
	for (int i = 0; i < 3; i++) addSlot(i, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(15, 7, 15, 0, 100);           // the far corner, sealed so it is distinguishable
	addSlot(14, 7, 15, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "a 16x8x16 box is exactly what VisWalk can hold, so it walks");
	CHECK(guardsIntact(), "control: the pool sentinels are intact after the widest legal box");
}

// ── testRefusalIsSafe ──────────────────────────────────────────────────────────────────

static void testRefusalIsSafe(void)
{
	puts("\n-- a refusal must draw everything, never cull --");

	// Nothing loaded. `if (!any) return;` — s_cave_ran must be left exactly as cullFrame set
	// it, which is false, or the draw loop culls against a VisWalk that was never populated.
	resetPool(); s_next_slot = 0;
	frame(8.0f, 8.0f, 8.0f);
	CHECK(!s_cave_ran,  "an empty pool refuses the walk rather than culling against nothing");
	CHECK(!s_walk_valid, "and it does not validate the cache");
	CHECK(s_walk_runs == 0, "and it does not count as a walk");

	// A box wider than VIS_BOX_MAX_XZ. visWalkBegin returns false and caveWalk must bail.
	// 17 on X is one past the limit; nothing else about the fixture is unusual.
	resetPool(); s_next_slot = 0;
	addSlot(0,  0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(16, 0, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);
	CHECK(!s_cave_ran,   "a 17-wide box is refused, so everything is drawn");
	CHECK(!s_walk_valid, "a refused box does not validate the cache");
	CHECK(s_walk_runs == 0, "a refused box is not counted as a walk");

	// A box too tall. VIS_BOX_MAX_Y is COLUMN_CHUNKS (8), and Y is the axis where a bad clamp
	// is easiest to miss because the world is only ever 8 chunks deep.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(0, 8, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);
	CHECK(!s_cave_ran, "a 9-tall box is refused too");

	// The dangerous one: a good walk, then a frame whose box cannot be built. The stale walk
	// describes a world that no longer exists, so re-using it would cull real geometry.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 100);
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);
	CHECK(s_cave_ran && s_walk_valid, "the good walk ran and validated the cache");

	// Same camera chunk, but the pool has grown past what VisWalk holds. cullInvalidate() is
	// what the pool change really calls, so the cache is dropped exactly as it would be.
	addSlot(20, 0, 0, VIS_ALL_CONNECTED, 100);
	cullInvalidate();
	CHECK(!s_cull_valid, "control: cullInvalidate drops the frustum cache too");
	frame(8.0f, 8.0f, 8.0f);
	CHECK(!s_cave_ran,
	      "a refusal after a good walk still refuses: the stale walk is not re-used");
	CHECK(!s_walk_valid, "and the stale cache stays invalid");
}

// ── testEmptySlotsFedIn ────────────────────────────────────────────────────────────────

static void testEmptySlotsFedIn(void)
{
	puts("\n-- a slot that meshed to nothing is still fed to the walk --");

	// The sealed middle chunk has index_count == 0: the player has dug it out, or it meshed to
	// nothing. It draws no triangles, but it still has to be inserted with its REAL mask,
	// because visWalkBegin fills every cell with VIS_ALL_CONNECTED. Skip it and its cell stays
	// a hole that sight pours straight through, and the chunk behind it stops being culled.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 0);     // sealed AND empty
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "the walk runs with an empty slot in the pool");
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0),
	      "an empty slot still blocks sight, so what is behind it stays culled");

	// The other half of the same insert: `drawable` is `index_count != 0`. The empty chunk was
	// certainly REACHED by the walk — it is the camera's neighbour — but it must not be
	// reported visible, because there is nothing in it to draw. Marking it drawable would put
	// a chunk with no geometry into every visible count the renderer reports.
	CHECK(!visWalkVisible(&s_walk, 1, 0, 0),
	      "the empty chunk is reached but not drawable, so it is not 'visible'");

	// Control on the same fixture: a non-empty chunk in the same position IS visible. Without
	// this, "drawable is always false" would pass the check above.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 1);     // sealed, but it has geometry
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);
	frame(8.0f, 8.0f, 8.0f);
	CHECK(visWalkVisible(&s_walk, 1, 0, 0),
	      "the same chunk with one index of geometry IS visible");
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0),
	      "control: it blocks sight either way, so the mask reached the walk both times");
}

// ── testHolesAreSeeThrough ─────────────────────────────────────────────────────────────

// The single most dangerous thing the box can get wrong, and the reason visWalkBegin fills the
// mask with 0xFF and not 0.
//
// caveWalk builds a DENSE box and then inserts only the coordinates that have a slot. Every
// other coordinate in that box is a HOLE — and holes are the normal case, not an edge case:
// main.c never meshes an all-air chunk, so the whole sky above the terrain is missing from the
// pool. A hole must read as "see-through and not drawable". Read as a WALL it culls everything
// behind it, which is a hole in the world; read as drawable it puts chunks that do not exist
// into the visible set.
//
// Every other fixture in this file fills its box completely, so none of them can catch this —
// which was found the hard way, by sabotaging visWalkBegin's memset from 0xFF to 0x00 and
// watching the whole suite stay green. This family is that arm's tripwire.
static void testHolesAreSeeThrough(void)
{
	puts("\n-- an unmeshed coordinate is see-through, not a wall --");

	// Camera at (0,0,0). The direct route east is sealed by (1,0,0). The only way to the
	// target at (2,1,0) is up, then east THROUGH the hole at (1,1,0), which no slot covers.
	// (2,0,0) is a hole too, and is unreachable because the sealed chunk stops the walk.
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED,             100);   // camera
	addSlot(1, 0, 0, 0,                             100);   // sealed: kills the direct route
	addSlot(0, 1, 0, PAIR(FACE_BOTTOM, FACE_EAST),  100);   // up and over
	addSlot(2, 1, 0, VIS_ALL_CONNECTED,             100);   // the target, beyond the hole
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "a box with holes in it still walks");
	CHECK(visWalkVisible(&s_walk, 2, 1, 0),
	      "sight reaches the target THROUGH an unmeshed coordinate, so a hole is see-through");
	CHECK(!visWalkVisible(&s_walk, 1, 1, 0),
	      "but the hole itself draws nothing, so it is not reported visible");
	CHECK(!visWalkVisible(&s_walk, 2, 0, 0),
	      "control: a hole the sealed chunk hides is still culled, so holes do not "
	      "unconditionally answer visible");
}

// ── testFrameCache ─────────────────────────────────────────────────────────────────────

static void testFrameCache(void)
{
	puts("\n-- the frame cache: re-use within a chunk, re-run on crossing --");

	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, VIS_ALL_CONNECTED, 100);
	addSlot(1, 0, 0, 0,                 100);
	addSlot(2, 0, 0, VIS_ALL_CONNECTED, 100);

	frame(8.0f, 8.0f, 8.0f);
	CHECK(s_walk_runs == 1, "the first frame runs the walk");
	CHECK(s_walk_valid && s_walk_cx == 0 && s_walk_cy == 0 && s_walk_cz == 0,
	      "and records which chunk it ran for");

	// Moving inside the same chunk. Every one of these is a cache hit, and every one must
	// still leave s_cave_ran true — a cache hit that forgot to set it would draw the whole
	// world on most frames, which is the performance bug the walk exists to prevent.
	frame(1.0f, 8.0f, 8.0f);
	CHECK(s_walk_runs == 1, "moving inside the chunk does not re-run the walk");
	CHECK(s_cave_ran,       "and the cached walk is still offered to the cull");
	frame(15.0f, 15.0f, 15.0f);
	CHECK(s_walk_runs == 1, "still no re-run at the far corner of the same chunk");
	CHECK(s_cave_ran,       "and still offered to the cull");

	// Crossing on each axis in turn. Three separate checks and not one, because a cache key
	// that compared only cx would pass a single combined test that moved on all three.
	frame(24.0f, 8.0f, 8.0f);
	CHECK(s_walk_runs == 2, "crossing on X re-runs the walk");
	CHECK(s_walk_cx == 1,   "and the key follows the camera on X");
	frame(24.0f, 24.0f, 8.0f);
	CHECK(s_walk_runs == 3, "crossing on Y re-runs the walk");
	CHECK(s_walk_cy == 1,   "and the key follows the camera on Y");
	frame(24.0f, 24.0f, 24.0f);
	CHECK(s_walk_runs == 4, "crossing on Z re-runs the walk");
	CHECK(s_walk_cz == 1,   "and the key follows the camera on Z");

	// Crossing westward, across zero. chunkOfFloat's floorf is what makes -1 a different key
	// from 0; with a truncating cast both sides of the origin key as chunk 0 and the walk
	// would not re-run when the player stepped west out of the origin chunk.
	frame(24.0f, 24.0f, 24.0f);
	CHECK(s_walk_runs == 4, "repeating the same position is still a cache hit");
	frame(-8.0f, 24.0f, 24.0f);
	CHECK(s_walk_runs == 5, "stepping west across the origin re-runs the walk");
	CHECK(s_walk_cx == -1,  "and the key is chunk -1, not chunk 0");

	// The pool changing under a stationary camera. This is the case caveWalk cannot detect on
	// its own — the camera has not moved — so cullInvalidate() is the only thing that makes
	// the next frame re-walk. A cache that survived it would cull against the old pool.
	const int before = s_walk_runs;
	frame(-8.0f, 24.0f, 24.0f);
	CHECK(s_walk_runs == before, "a stationary camera does not re-run the walk");
	cullInvalidate();
	frame(-8.0f, 24.0f, 24.0f);
	CHECK(s_walk_runs == before + 1, "cullInvalidate forces the next frame to re-walk");
	CHECK(s_cave_ran, "and that re-walk is offered to the cull");
}

// ── testCacheKeyIsSound ────────────────────────────────────────────────────────────────

// Defined with testDeterminism, which is where it is explained. Used here too, because "the
// answer did not change" is exactly what both families are asking, and hashing the whole box
// asks it of every coordinate rather than of one hand-picked target.
static uint32_t visibleSignature(int nx, int ny, int nz);

// caveWalk's frame cache is keyed on the camera's CHUNK, and its comment states the property
// that makes that key sufficient: the walk's answer is a function of the camera's chunk and the
// pool, "and nothing else — not the camera's position inside that chunk". This family is that
// sentence turned into a check, on all three axes.
//
// It was FALSE until v1.8.2, and this family was originally written as testCacheKeyIsUnsound to
// pin the bug. visgraph.c's faceLeadsAway compared the camera against every chunk bound
// inclusively, `cam_x <= hi_x` as well as `cam_x >= lo_x`. With the camera exactly on a chunk
// boundary that made BOTH x faces of the chunk west of it "lead away", so the walk could step
// west out of the camera's chunk and then straight back east — toward the camera, which that
// test exists to forbid — and reach strictly MORE chunks than it does from anywhere else in the
// same chunk. Measured on the X fixture below: 5 chunks from x = 24.0, 6 from x = 16.0.
//
// The fix made the three HIGH faces strict (`cam_x < hi_x`, and likewise TOP and SOUTH) while
// the three LOW faces keep `>=`. That asymmetry is the point, and it is not a taste choice: it
// is chunkOfBlock's interval convention written down. chunkOfBlock is `(int)floorf(b) >> 4`, so
// a chunk owns the HALF-OPEN span [lo, hi) and a camera at exactly hi is already in the chunk
// east of it. Making both ends inclusive claims the camera is inside two chunks at once on that
// axis, which contradicts the very function that decided which chunk the walk starts from — and
// the walk's own start cell, testChunkOfFloat's whole subject, comes from that function. With
// the half-open span each case reduces to a comparison of chunk INDICES: `cam_x < hi_x` is
// exactly `chunkOfBlock(cam_x) <= cx`, because the camera's x is confined to
// [cam_cx * 16, cam_cx * 16 + 16). Symmetric `<=`/`>=` cannot have that property at all — an
// exact boundary is the one position that satisfies both ends — so no symmetric form fixes this.
//
// Why the change cannot over-cull, which is the direction visgraph.h forbids: the step the
// strict `<` removes is one no straight sight line can take. A ray from a camera at exactly
// x = 16.0 to anything east of x = 16.0 never has x < 16.0 at any point, so it cannot pass
// through a chunk west of x = 16.0 first. The extra reach was a false positive — an under-cull —
// and dropping it loses no chunk that was ever really in view. Verified separately against a
// ray-marched oracle that shares none of this code, over every chunk that changed answer.
//
// The three fixtures are hand-derived, not hashes and not random masks, and each is the same
// shape rotated onto a different axis. Each one is closed under the fixed code and would open
// under exactly ONE of the three reverted comparisons, which is what makes them able to tell the
// three apart: FACE_EAST reddens only xFixture, FACE_TOP only yFixture, FACE_SOUTH only
// zFixture. Two of those three lines have no other coverage anywhere in this file.

// Camera in chunk (1,0,0), on x = 16.0. The target (1,0,2) is sealed off from the direct route
// by (1,0,1) and is reachable only by the dog-leg west, south, south, then EAST — and that final
// eastward step is the one the strict FACE_EAST comparison forbids.
static void xFixture(void)
{
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, PAIR(FACE_EAST, FACE_SOUTH),  100);
	addSlot(0, 0, 1, PAIR(FACE_NORTH, FACE_SOUTH), 100);
	addSlot(0, 0, 2, PAIR(FACE_NORTH, FACE_EAST),  100);
	addSlot(1, 0, 0, VIS_ALL_CONNECTED,            100);   // the camera's chunk
	addSlot(1, 0, 1, 0,                            100);   // sealed: kills the direct route
	addSlot(1, 0, 2, VIS_ALL_CONNECTED,            100);   // the target
}

// The same shape stood on end. Camera in chunk (0,1,0), on y = 16.0; the dog-leg goes down,
// south, south, then back UP, and that upward step is the strict FACE_TOP comparison.
static void yFixture(void)
{
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, PAIR(FACE_TOP, FACE_SOUTH),   100);
	addSlot(0, 0, 1, PAIR(FACE_NORTH, FACE_SOUTH), 100);
	addSlot(0, 0, 2, PAIR(FACE_NORTH, FACE_TOP),   100);
	addSlot(0, 1, 0, VIS_ALL_CONNECTED,            100);   // the camera's chunk
	addSlot(0, 1, 1, 0,                            100);   // sealed
	addSlot(0, 1, 2, VIS_ALL_CONNECTED,            100);   // the target
}

// And laid along Z. Camera in chunk (0,0,1), on z = 16.0; the dog-leg goes north, east, east,
// then back SOUTH, and that southward step is the strict FACE_SOUTH comparison. The eastward
// steps happen from x = 8.0, the middle of chunk 0, so they are unaffected by FACE_EAST.
static void zFixture(void)
{
	resetPool(); s_next_slot = 0;
	addSlot(0, 0, 0, PAIR(FACE_SOUTH, FACE_EAST),  100);
	addSlot(1, 0, 0, PAIR(FACE_WEST, FACE_EAST),   100);
	addSlot(2, 0, 0, PAIR(FACE_WEST, FACE_SOUTH),  100);
	addSlot(0, 0, 1, VIS_ALL_CONNECTED,            100);   // the camera's chunk
	addSlot(1, 0, 1, 0,                            100);   // sealed
	addSlot(2, 0, 1, VIS_ALL_CONNECTED,            100);   // the target
}

// One axis' worth of the property: a fresh walk from the middle of the chunk and a fresh walk
// from exactly the low boundary of the same chunk must agree about the target, and the target
// must be CULLED from both — the dog-leg is closed, because its closing step leads back toward
// the camera. Taking both answers from cold caches is what makes this about visgraph.c's
// geometry rather than about caveWalk re-using a result.
static void axisIsPositionIndependent(void (*fixture)(void), float mx, float my, float mz,
                                      float ex, float ey, float ez,
                                      int tx, int ty, int tz,
                                      const char* same, const char* culled)
{
	fixture();
	frame(mx, my, mz);
	const bool mid = visWalkVisible(&s_walk, tx, ty, tz);
	CHECK(s_walk_runs == 1, "walked once from the middle of the camera's chunk");

	fixture();
	frame(ex, ey, ez);
	const bool edge = visWalkVisible(&s_walk, tx, ty, tz);
	CHECK(s_walk_runs == 1, "walked once from the boundary of the same chunk");

	CHECK(mid == edge, same);
	CHECK(!edge, culled);
}

static void testCacheKeyIsSound(void)
{
	puts("\n-- the cache key IS sufficient: the boundary is not a special position --");

	CHECK(chunkOfFloat(16.0f) == chunkOfFloat(24.0f),
	      "control: both camera positions below are in the same chunk, so the cache is entitled "
	      "to call them equal");

	axisIsPositionIndependent(xFixture, 24.0f, 8.0f, 8.0f, 16.0f, 8.0f, 8.0f, 1, 0, 2,
	      "X: the boundary and the middle of the chunk give the same answer",
	      "and from x=16.0 the dog-leg back east stays closed, so the target is culled");

	axisIsPositionIndependent(yFixture, 8.0f, 24.0f, 8.0f, 8.0f, 16.0f, 8.0f, 0, 1, 2,
	      "Y: the boundary and the middle of the chunk give the same answer",
	      "and from y=16.0 the dog-leg back up stays closed, so the target is culled");

	axisIsPositionIndependent(zFixture, 8.0f, 8.0f, 24.0f, 8.0f, 8.0f, 16.0f, 2, 0, 1,
	      "Z: the boundary and the middle of the chunk give the same answer",
	      "and from z=16.0 the dog-leg back south stays closed, so the target is culled");

	// The whole visible set, not just the target, over every corner and face-centre of the
	// camera's chunk — all three low boundaries at once, all three approached from inside, and
	// the mixtures. A comparison that went wrong on only one axis, or only when two axes are on
	// a boundary together, would slip past the three single-axis checks above.
	static const float off[5] = { 0.0f, 0.001f, 8.0f, 15.0f, 15.999f };
	uint32_t sig = 0;
	bool     have_sig = false;
	int      differing = 0;
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 5; j++)
			for (int k = 0; k < 5; k++) {
				xFixture();
				frame(16.0f + off[i], off[j], off[k]);
				const uint32_t s = visibleSignature(2, 1, 3);
				if (!have_sig) { sig = s; have_sig = true; }
				else if (s != sig) differing++;
			}
	if (differing)
		printf("  note   %d of 125 positions inside one chunk disagreed with the first\n",
		       differing);
	CHECK(differing == 0,
	      "125 camera positions spanning the camera's whole chunk, boundaries included, give "
	      "one identical visible set");

	// And the consequence the cache actually depends on, end to end through the real caveWalk:
	// walk from the middle, then step onto the boundary without leaving the chunk. The cache
	// hits — and the answer it keeps serving is now the answer a fresh walk would have computed,
	// so no chunk is culled that this frame would have drawn.
	xFixture();
	frame(24.0f, 8.0f, 8.0f);
	const uint32_t cached_from_mid = visibleSignature(2, 1, 3);
	frame(16.0f, 8.0f, 8.0f);
	CHECK(s_walk_runs == 1, "moving onto the boundary is a cache hit, so no re-walk happens");
	CHECK(visibleSignature(2, 1, 3) == cached_from_mid,
	      "control: the cache really did serve the earlier walk's answer unchanged");

	xFixture();
	frame(16.0f, 8.0f, 8.0f);
	CHECK(visibleSignature(2, 1, 3) == cached_from_mid,
	      "and a COLD walk from the boundary computes that same answer, so the cache hit cost "
	      "nothing - no over-cull, which is the failure visgraph.h forbids by name");
}

// ── testDeterminism ────────────────────────────────────────────────────────────────────

static uint32_t visibleSignature(int nx, int ny, int nz)
{
	// FNV-1a over the visible answer for every coordinate in the box. Not a pinned constant —
	// it is only ever compared against another run in the same process, so it captures "the
	// same answer" without asserting anything about what that answer should be. The checks
	// that say what the answer should be are the ones above.
	uint32_t h = 2166136261u;
	for (int y = 0; y < ny; y++)
		for (int z = 0; z < nz; z++)
			for (int x = 0; x < nx; x++) {
				const uint8_t b = visWalkVisible(&s_walk, x, y, z) ? 1u : 0u;
				h = (h ^ b) * 16777619u;
			}
	return h;
}

// A deliberately intricate but entirely reproducible pool: masks derived from the coordinates
// so the shape is fixed, and varied enough that a wrong answer is very unlikely to hash equal.
static void tangledPool(void)
{
	resetPool(); s_next_slot = 0;
	for (int y = 0; y < 3; y++)
		for (int z = 0; z < 4; z++)
			for (int x = 0; x < 4; x++) {
				const int k = (y * 4 + z) * 4 + x;
				const uint16_t mask = (uint16_t)((k * 2477u + 13u) & 0x7FFFu);
				addSlot(x, y, z, mask, (k % 3) ? 100 : 0);
			}
}

static void testDeterminism(void)
{
	puts("\n-- the same world seen twice looks the same --");

	tangledPool();
	frame(24.0f, 24.0f, 24.0f);
	CHECK(s_cave_ran, "the tangled pool walks");
	const uint32_t first = visibleSignature(4, 3, 4);

	// Rebuild the identical pool from scratch, with an unrelated walk in between to make sure
	// nothing carries over in the VisWalk's entered/pending/visible arrays. visWalkRun memsets
	// them, and this is the check that says so.
	resetPool(); s_next_slot = 0;
	addSlot(9, 5, 9, VIS_ALL_CONNECTED, 100);
	addSlot(10, 5, 9, 0,                100);
	frame(9.0f * 16.0f + 8.0f, 5.0f * 16.0f + 8.0f, 9.0f * 16.0f + 8.0f);

	tangledPool();
	frame(24.0f, 24.0f, 24.0f);
	const uint32_t second = visibleSignature(4, 3, 4);

	CHECK(first == second,
	      "an identical pool and camera give an identical visible set, from a cold cache");

	// Two DIFFERENT cameras in the same pool must not give the same answer, or the check above
	// would pass against a walk that always returns the same thing.
	tangledPool();
	frame(24.0f, 24.0f, 24.0f);
	const uint32_t from_middle = visibleSignature(4, 3, 4);
	tangledPool();
	frame(8.0f, 8.0f, 8.0f);
	const uint32_t from_corner = visibleSignature(4, 3, 4);
	CHECK(from_middle != from_corner,
	      "two different camera chunks in the same pool give different visible sets");

	CHECK(guardsIntact(), "control: the sentinels survived the determinism runs");
}

// ── testNoSlotOverrun ──────────────────────────────────────────────────────────────────

static void testNoSlotOverrun(void)
{
	puts("\n-- caveWalk stays inside the pool and inside the VisWalk --");

	// The first and last slot of the pool both in use, at opposite extremes of a legal box,
	// with every slot between them released. If the loop bound were off by one in either
	// direction it reads a sentinel; if it wrote, the sentinel changes.
	resetPool(); s_next_slot = 0;
	s_slots[0].used = true;
	s_slots[0].cx = 0; s_slots[0].cy = 0; s_slots[0].cz = 0;
	s_slots[0].vis_mask = VIS_ALL_CONNECTED; s_slots[0].index_count = 100;
	s_slots[MESH_SLOTS - 1].used = true;
	s_slots[MESH_SLOTS - 1].cx = 15; s_slots[MESH_SLOTS - 1].cy = 7;
	s_slots[MESH_SLOTS - 1].cz = 15;
	s_slots[MESH_SLOTS - 1].vis_mask = 0; s_slots[MESH_SLOTS - 1].index_count = 100;
	frame(8.0f, 8.0f, 8.0f);

	CHECK(s_cave_ran, "the pool's first and last slot alone still make a walkable box");
	CHECK(guardsIntact(), "no write landed outside s_slots or outside the VisWalk");

	// The last slot is what sets the box's far corner. If it had been skipped the box would be
	// 1x1x1 and nothing would have been reached but the origin — so this check is what says
	// the loop really visited slot MESH_SLOTS-1 rather than stopping one short.
	CHECK(visWalkVisible(&s_walk, 0, 0, 0), "the camera's chunk is visible");
	CHECK(!visWalkVisible(&s_walk, 8, 4, 8),
	      "an empty coordinate inside the box is not drawable, so it is not visible");

	// Far negative coordinates, where a signed/unsigned slip in the box arithmetic shows up.
	resetPool(); s_next_slot = 0;
	addSlot(-1000, -3, -1000, VIS_ALL_CONNECTED, 100);
	addSlot(-999,  -3, -1000, 0,                 100);
	addSlot(-998,  -3, -1000, VIS_ALL_CONNECTED, 100);
	frame(-1000.0f * 16.0f + 8.0f, -3.0f * 16.0f + 8.0f, -1000.0f * 16.0f + 8.0f);
	CHECK(s_cave_ran, "a box a thousand chunks west of the origin still walks");
	CHECK(!visWalkVisible(&s_walk, -998, -3, -1000),
	      "and culls correctly there");
	CHECK(guardsIntact(), "control: the sentinels are intact after the far-negative box");

	// A coordinate that is nowhere near the box must answer "visible" — the walk knows nothing
	// about it and the only safe answer to "may I skip this" is no. Independent of every
	// decision caveWalk makes, so it is a control.
	CHECK(visWalkVisible(&s_walk, 500, 2, 500),
	      "control: a coordinate outside the box answers visible");
}

// ── testPairIndexControl ───────────────────────────────────────────────────────────────

static void testPairIndexControl(void)
{
	puts("\n-- controls independent of caveWalk --");

	// visPairIndex numbers the fifteen unordered face pairs uniquely, and the fixtures above
	// build their masks with it. If this were broken the fixtures would be meaningless, so it
	// is checked separately and stays green in every arm.
	int seen[VIS_PAIRS];
	for (int i = 0; i < VIS_PAIRS; i++) seen[i] = 0;
	int bad = 0;
	for (int a = 0; a < BLOCK_FACES; a++)
		for (int b = 0; b < BLOCK_FACES; b++) {
			const int i = visPairIndex(a, b);
			if (a == b) { if (i != -1) bad++; continue; }
			if (i < 0 || i >= VIS_PAIRS) { bad++; continue; }
			if (visPairIndex(b, a) != i) bad++;
			seen[i]++;
		}
	int covered = 0;
	for (int i = 0; i < VIS_PAIRS; i++) if (seen[i] == 2) covered++;
	CHECK(bad == 0 && covered == VIS_PAIRS,
	      "control: visPairIndex numbers all 15 face pairs uniquely and symmetrically");

	CHECK(VIS_ALL_CONNECTED == 0xFFFFu,
	      "control: the 'assume the worst' sentinel is still all-bits-set");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== cavewalk test (source/scene/chunk_render.c caveWalk, extracted) ==");

	testChunkOfFloat();
	testBoxFromUsedSlots();
	testRefusalIsSafe();
	testEmptySlotsFedIn();
	testHolesAreSeeThrough();
	testFrameCache();
	testCacheKeyIsSound();
	testDeterminism();
	testNoSlotOverrun();
	testPairIndexControl();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
