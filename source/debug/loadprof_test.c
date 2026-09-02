// v1.7.1 task 48b. The host harness for debug/loadprof.h, and the answer to "where does world
// loading actually spend its time".
//
// Two things in one binary, because they need the same 81-column world and building it twice
// would double the suite's runtime for nothing:
//
//   1. CHECKS on the instrument and on the load path's shape. Every one of them is written to
//      be able to go red — see the sabotage log in tools/run_host_tests.sh.
//   2. A printed STAGE BREAKDOWN for a freshly created world and for a reloaded, edited one,
//      which is the comparison steve's report is about: "as soon as I start making some
//      changes, the world takes quite a long time to load, almost doubles in the time of
//      creating a world."
//
// ── What this drives, and what it cannot ──────────────────────────────────────────────
//
// It calls the REAL functions the game calls, in the order the game calls them, taken from
// app/worker.c's workerMain/workerLoadColumn/workerInstall and main.c's genInstallOne and
// genDrainMesh:
//
//   per column   regionReadColumnCached -> regionDecodeColumn      (card had it)
//                                       -> worldgenColumn          (card did not)
//                lightPropagateColumn / lightRelightColumn
//                per chunk: chunkDecompressAll -> worldSetChunkAll
//                lightColumnCopy, worldExit(staging)
//   per chunk    scratchFill (+scratchFillLight) -> meshChunk
//
// It CANNOT drive four of the eleven stages, and does not pretend to:
//
//   worlddir / played / setup   live in main.c, which carries main() and includes <3ds.h>, so
//                               nothing here can link them. They are instrumented on the
//                               console and read out of sdmc:/blocksmith/load.csv.
//   present                     is C3D_FrameBegin(C3D_FRAME_SYNCDRAW) and a vsync. There is no
//                               such thing on a host.
//
// The mesh stage here is the CPU half of scene/chunk_render.c's chunkRenderBuild — the scratch
// fill and meshChunk — and not its GPU upload, which is console-only. Stated rather than
// glossed: a mesh figure from this binary is a lower bound on the console's.
//
// The two worlds are also not interleaved the way the game interleaves them: the game installs
// one column per frame and drains a few chunks alongside it, this installs all 81 and then
// meshes. Per-stage TOTALS are unaffected — the same functions run the same number of times on
// the same data — but the ordering is not a claim this binary makes.
#ifndef __3DS__

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug/loadprof.h"
#include "world/block.h"
#include "world/budget.h"
#include "world/chunk.h"
#include "world/genversion.h"
#include "world/light.h"
#include "world/mesher.h"
#include "world/region.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

// The ring the console fills at RENDER_DIST_MAX: main.c's s_area_radius is the render distance
// plus one, so radius 3 generates a 9x9 area and meshes the 7x7 inside it. This is the worst
// case a player can select and the one the complaint is about.
#define AREA_R  4
#define MESH_R  3
#define SPAN    (2 * AREA_R + 1)
#define COLUMNS (SPAN * SPAN)

// The seed and generator the game uses in single player (main.c BS_WORLD_SEED, and
// genVersionResolve's answer for a directory this build just stamped).
#define TEST_SEED 1337u

static int  s_checks;
static int  s_fails;
static char s_first[128];

#define CHECK(cond) do {                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			fprintf(stderr, "FAIL L%d %s\n", __LINE__, #cond);                  \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, #cond);  \
		}                                                                       \
	} while (0)

// ── Scratch storage ────────────────────────────────────────────────────────────────────
//
// Statics rather than locals for the same reason app/worker.c's are: these are worst-case
// buffers and this file's call frames are not the place for them.
static World       s_live;      // the world the player ends up in
static World       s_stage;     // app/worker.c's s_staging
static MeshScratch s_scratch;
static LightQueue  s_lq;
static BlockId     s_install[CHUNK_BLOCKS];   // worker.c's s_install_blocks
static uint8_t     s_iobuf[REGION_COL_MAX];   // worker.c's s_load_buf
static uint8_t     s_savebuf[REGION_COL_MAX];
static WorldGen    s_gen;
static WorldGenScratch s_wgs;   // worker.c's s_wgs: this lane's generator scratch

static MeshVertex* s_verts;
static uint16_t*   s_idx;

// ── The per-process world directory, same rule as world_test.c's ──────────────────────
//
// Derived from the pid because this suite does get run concurrently and a shared path means
// one run rewrites the region file another run is part-way through reading. world_test.c's
// comment records what that looks like when it bites: unrelated assertions failing at random
// line numbers on a suite that passes when re-run.
static const char* testDir(void)
{
	static char dir[64];
	if (dir[0] == '\0') snprintf(dir, sizeof dir, "build-host/loadprof-%ld", (long)getpid());
	return dir;
}

static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

static void testRmTree(const char* path)
{
	DIR* d = opendir(path);
	if (!d) return;

	for (const struct dirent* e = readdir(d); e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char child[512];
		snprintf(child, sizeof child, "%s/%s", path, e->d_name);
		if (remove(child) != 0) testRmTree(child);
	}
	closedir(d);
#if defined(_WIN32)
	_rmdir(path);
#else
	rmdir(path);
#endif
}

// ── The identity hash ─────────────────────────────────────────────────────────────────
//
// FNV-1a over every allocated chunk of every loaded column: the column coordinate, the chunk
// index, and all 4096 decompressed cells. This is the check that a load-path change did not
// change the world that loads — the same shape of proof task 49 used for the install rewrite,
// and the reason a "faster" arm cannot quietly drop a chunk and look like a win.
//
// Walked in ring order rather than in world-slot order: the slot table is a hash and its order
// depends on insertion, so hashing it directly would make the answer depend on the order the
// columns happened to arrive in, which is exactly the thing an optimisation is allowed to
// change.
static uint64_t hashWorld(const World* w, int32_t cx0, int32_t cz0)
{
	uint64_t h = 1469598103934665603ULL;
	#define MIX(b) do { h ^= (uint64_t)(uint8_t)(b); h *= 1099511628211ULL; } while (0)

	for (int32_t dz = -AREA_R; dz <= AREA_R; dz++)
		for (int32_t dx = -AREA_R; dx <= AREA_R; dx++) {
			const int32_t cx = cx0 + dx, cz = cz0 + dz;
			const Column* col = worldColumn(w, cx, cz);
			MIX(col ? 1 : 0);
			if (!col) continue;

			MIX(cx); MIX(cx >> 8); MIX(cz); MIX(cz >> 8);
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
				MIX(cy);
				MIX(col->chunks[cy] ? 1 : 0);
				if (!col->chunks[cy]) continue;

				static BlockId cells[CHUNK_BLOCKS];
				chunkDecompressAll(col->chunks[cy], cells);
				for (int i = 0; i < CHUNK_BLOCKS; i++) MIX(cells[i]);
			}
		}

	#undef MIX
	return h;
}

// ── One world entry, driven through the real functions ────────────────────────────────
//
// `dir` is the world directory, or NULL for a world with no save file at all. Returns the
// identity hash of the loaded ring; the stage figures are left in the profiler for the caller
// to print.
static uint64_t runLoad(const char* dir, int reloaded, int light_on)
{
	lightEngineInit(light_on != 0);
	lightQueueInit(&s_lq);

	worldInit(&s_live);
	worldInit(&s_stage);

	loadprofBegin("bench", reloaded);

	// ── The generation + install pass: app/worker.c's JOB_GENERATE followed by
	//    workerInstall, once per column, in main.c genRequestArea's sweep order.
	for (int32_t dz = -AREA_R; dz <= AREA_R; dz++)
		for (int32_t dx = -AREA_R; dx <= AREA_R; dx++) {
			const int32_t cx = dx, cz = dz;

			// workerLoadColumn: the card first, generation as the fallback. The two timers
			// bracket exactly what worker.c brackets.
			bool ok = false;
			if (dir) {
				const uint64_t t_io = loadprofMark();
				const uint32_t n = regionReadColumnCached(dir, cx, cz, s_iobuf, sizeof s_iobuf);
				loadprofSince(LOAD_STAGE_REGION_IO, t_io);

				if (n) {
					const uint64_t t_dec = loadprofMark();
					ok = regionDecodeColumn(&s_stage, cx, cz, s_iobuf, n);
					loadprofSince(LOAD_STAGE_DECODE, t_dec);
					if (!ok) worldColumnRemove(&s_stage, cx, cz);
					else if (!worldColumnCreate(&s_stage, cx, cz)) ok = false;
				}
			}

			if (!ok) {
				const uint64_t t_gen = loadprofMark();
				ok = worldgenColumn(&s_gen, &s_wgs, &s_stage, cx, cz);
				loadprofSince(LOAD_STAGE_GENERATE, t_gen);
			}

			if (ok && lightEnabled()) {
				const uint64_t t_light = loadprofMark();
				if (!lightPropagateColumn(&s_stage, cx, cz, &s_lq))
					lightRelightColumn(&s_stage, cx, cz);
				loadprofSince(LOAD_STAGE_LIGHT, t_light);
			}

			// workerInstall, verbatim in structure: unpack each staged chunk and hand it whole
			// to worldSetChunkAll, copy the light across, then reset the staging world — which
			// is what the worker expects to find when it takes the next job.
			const uint64_t t_inst = loadprofMark();
			const Column* src = worldColumn(&s_stage, cx, cz);
			bool all = ok;
			if (!src) {
				all = false;
			} else {
				for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
					if (!src->chunks[cy]) continue;
					chunkDecompressAll(src->chunks[cy], s_install);
					if (!worldSetChunkAll(&s_live, cx, cy, cz, s_install)) { all = false; break; }
				}
				if (all) lightColumnCopy(src, worldColumn(&s_live, cx, cz));
			}
			worldExit(&s_stage);
			loadprofSince(LOAD_STAGE_INSTALL, t_inst);

			CHECK(all);
		}

	// ── The mesh pass: main.c's genDrainMesh, whose per-chunk work is chunkRenderBuild's
	//    scratch fill and meshChunk. The GPU upload has no host equivalent.
	MeshOut out = {0};
	out.vert_cap  = MESH_MAX_VERTS;
	out.index_cap = MESH_MAX_INDICES;
	out.verts     = s_verts;
	out.indices   = s_idx;

	for (int32_t dz = -MESH_R; dz <= MESH_R; dz++)
		for (int32_t dx = -MESH_R; dx <= MESH_R; dx++)
			for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
				const uint64_t t_mesh = loadprofMark();
				scratchFill(&s_scratch, &s_live, dx, cy, dz);
				if (lightEnabled()) scratchFillLight(&s_scratch, &s_live, dx, cy, dz);
				meshChunk(&out, &s_scratch);
				loadprofSince(LOAD_STAGE_MESH, t_mesh);
			}

	loadprofEnd();

	const uint64_t h = hashWorld(&s_live, 0, 0);
	worldExit(&s_live);
	worldExit(&s_stage);
	return h;
}

// Saves the edited columns exactly the way main.c's saveDirtyColumns does: encode the live
// Column, hand the bytes to regionWriteColumn. Only DIRTY columns are written, which is the
// property that makes a reloaded world a mixture of card and seed rather than all card.
static int saveEdits(const char* dir, int32_t half)
{
	int written = 0;

	worldInit(&s_live);
	for (int32_t dz = -half; dz <= half; dz++)
		for (int32_t dx = -half; dx <= half; dx++) {
			CHECK(worldgenColumn(&s_gen, &s_wgs, &s_live, dx, dz));

			// One edit per column, in the chunk the surface is in. worldSet raises nothing —
			// world.h documents `dirty` as raised at the EDIT SITE — so this stands in for
			// scene/interact.c's place, which is what makes the column worth saving.
			worldSet(&s_live, dx * CHUNK_DIM + 7, 40, dz * CHUNK_DIM + 9, BLOCK_WOOD);

			const Column* col = worldColumn(&s_live, dx, dz);
			CHECK(col != NULL);
			if (!col) continue;

			const uint32_t n = regionEncodeColumn(col, s_savebuf, sizeof s_savebuf);
			CHECK(n > 0);
			if (n && regionWriteColumn(dir, dx, dz, s_savebuf, n)) written++;
		}
	worldExit(&s_live);

	// The worker drops the cache when it stops; a bench that kept it would answer the reload
	// pass out of a directory parsed before the file existed.
	regionCacheClose();
	return written;
}

static void report(const char* label)
{
	char buf[1024];
	loadprofFormat(buf, sizeof buf);
	printf("\n-- %s --\n%s", label, buf);
}

// ── The instrument's own contract ─────────────────────────────────────────────────────
//
// Nothing here touches a world. It is the arm that catches a profiler that has stopped
// recording: an accumulator that never adds, a guard that never disarms, a stage that lands in
// the wrong slot, a name table out of step with the enum.
static void testProfilerContract(void)
{
	loadprofReset();
	CHECK(!loadprofActive());
	CHECK(loadprofMark() == 0);

	// A bracket opened while disarmed must be dropped whole, not charged as garbage.
	loadprofSince(LOAD_STAGE_MESH, 0);
	CHECK(loadprofGet()->calls[LOAD_STAGE_MESH] == 0);

	loadprofBegin("contract", 1);
	CHECK(loadprofActive());
	CHECK(loadprofGet()->reloaded == 1);
	loadprofSetReloaded(0);
	CHECK(loadprofGet()->reloaded == 0);

	const uint64_t m = loadprofMark();
	CHECK(m != 0);

	// Something with a floor on how long it takes, so "the accumulator adds" is provable
	// without asserting on a clock nobody controls. 200k iterations of a volatile add is
	// microseconds on any host and is not optimisable away.
	static volatile int sink;
	for (int i = 0; i < 200000; i++) sink = sink + 1;

	loadprofSince(LOAD_STAGE_DECODE, m);
	CHECK(loadprofGet()->calls[LOAD_STAGE_DECODE] == 1);
	CHECK(loadprofGet()->ticks[LOAD_STAGE_DECODE] > 0);
	// Stage isolation: charging decode must not touch its neighbours in the enum.
	CHECK(loadprofGet()->ticks[LOAD_STAGE_REGION_IO] == 0);
	CHECK(loadprofGet()->ticks[LOAD_STAGE_GENERATE] == 0);

	loadprofFrame();
	loadprofFrame();
	CHECK(loadprofGet()->frames == 2);

	loadprofEnd();
	CHECK(!loadprofActive());
	CHECK(loadprofGet()->wall_ticks >= loadprofGet()->ticks[LOAD_STAGE_DECODE]);

	// Disarmed means disarmed: no further stage may move.
	const uint32_t after = loadprofGet()->calls[LOAD_STAGE_DECODE];
	CHECK(loadprofMark() == 0);
	loadprofSince(LOAD_STAGE_DECODE, m);
	CHECK(loadprofGet()->calls[LOAD_STAGE_DECODE] == after);

	// The name table is index-matched to the enum, and a stage added without a name here would
	// read off its end. Distinctness is what makes that detectable.
	for (int i = 0; i < LOAD_STAGE_COUNT; i++) {
		const char* a = loadprofStageName((LoadStage)i);
		CHECK(a != NULL && a[0] != '\0' && strcmp(a, "?") != 0);
		for (int j = i + 1; j < LOAD_STAGE_COUNT; j++)
			CHECK(strcmp(a, loadprofStageName((LoadStage)j)) != 0);
	}
	CHECK(strcmp(loadprofStageName((LoadStage)LOAD_STAGE_COUNT), "?") == 0);

	loadprofReset();
	CHECK(loadprofGet()->wall_ticks == 0);
	CHECK(loadprofGet()->frames == 0);
	CHECK(loadprofGet()->calls[LOAD_STAGE_DECODE] == 0);
}

// ── loadprofFormat's return value, and the buffer it was handed ────────────────────────
//
// loadprof.h documents the return as "the length written", which is the number a caller hands
// to fwrite. snprintf does not return that: it returns the length it WOULD have written, and
// accumulating that directly walks the write cursor past the end of the buffer the moment
// anything truncates — after which `cap - len` underflows a size_t into a huge value and the
// next snprintf is told it has essentially unlimited room.
//
// Nothing asserted any of this before. loadprofFormat was called once in this file, inside
// report(), into a char[1024] generous enough never to truncate, with its return discarded —
// so the whole truncation path was unexecuted by the suite rather than merely unchecked.
//
// The buffer is a window inside a larger '#'-filled array so an overrun is caught as data, not
// as a crash that may or may not happen: writing one byte past `cap` is silent on this host and
// would otherwise show up only as a corrupted neighbour much later.
static void testFormatTruncation(void)
{
	loadprofReset();
	loadprofBegin("truncation", 1);
	for (int i = 0; i < LOAD_STAGE_COUNT; i++) loadprofSince((LoadStage)i, 0);

	// Caps straddling the three points the old accumulation could run away: too small for even
	// the header line, big enough for the header but not a stage line, and big enough that the
	// stage loop runs and the "other" tail then has to fit in what is left.
	static const size_t kCaps[] = { 4, 8, 16, 32, 64, 96, 128, 200, 320, 512, 640, 800, 1024 };

	static const size_t kPad = 64;
	for (size_t k = 0; k < sizeof kCaps / sizeof kCaps[0]; k++) {
		const size_t cap = kCaps[k];
		char raw[1200];
		memset(raw, '#', sizeof raw);
		char* buf = raw + kPad;

		const int n = loadprofFormat(buf, cap);

		// The contract, stated three ways because each one fails differently: a negative
		// return would index backwards, a return at or past cap would overrun an fwrite, and
		// a return that disagrees with strlen is the "would have written" number leaking out.
		CHECK(n >= 0);
		CHECK((size_t)n < cap);
		CHECK((size_t)n == strlen(buf));

		bool pad_before_intact = true;
		for (size_t i = 0; i < kPad; i++)
			if (raw[i] != '#') pad_before_intact = false;
		CHECK(pad_before_intact);

		bool pad_after_intact = true;
		for (size_t i = kPad + cap; i < sizeof raw; i++)
			if (raw[i] != '#') pad_after_intact = false;
		CHECK(pad_after_intact);
	}

	loadprofReset();
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== load profile test ==");

	registryInitCore();
	registryFreeze();
	CHECK(worldgenInit(&s_gen, TEST_SEED, GEN_VERSION_NEWEST));

	s_verts = (MeshVertex*)malloc(sizeof(MeshVertex) * MESH_MAX_VERTS);
	s_idx   = (uint16_t*)malloc(sizeof(uint16_t) * MESH_MAX_INDICES);
	CHECK(s_verts != NULL && s_idx != NULL);
	if (!s_verts || !s_idx) return 1;

	testProfilerContract();
	testFormatTruncation();

	testMkdir("build-host");
	testMkdir(testDir());

	// ── Arm A: a world nobody has ever played. No region file exists, so every column of the
	//    ring comes from the seed. This is "creating a world".
	const uint64_t hash_fresh = runLoad(NULL, 0, 0);
	const LoadProfile fresh = *loadprofGet();
	report("A  fresh world, light off");

	// ── Arm B: the same world, played and changed. 25 of the 81 columns are edited and
	//    saved — main.c only ever writes DIRTY columns — and then the ring is loaded again
	//    from scratch. This is "as soon as I start making some changes".
	const int saved = saveEdits(testDir(), 2);
	CHECK(saved == 25);

	const uint64_t hash_reload = runLoad(testDir(), 1, 0);
	const LoadProfile reload = *loadprofGet();
	report("B  reloaded edited world, light off");

	// ── The same pair with the lighting engine on, which is what a New 3DS runs. Printed
	//    rather than asserted: it is here so the light stage is not a column of zeros in the
	//    only numbers this task leaves behind.
	regionCacheClose();
	(void)runLoad(NULL, 0, 1);
	report("C  fresh world, light on");
	regionCacheClose();
	(void)runLoad(testDir(), 1, 1);
	report("D  reloaded edited world, light on");

	printf("\nidentity  fresh=%016llx  reload=%016llx\n",
	       (unsigned long long)hash_fresh, (unsigned long long)hash_reload);

	// ── The shape of the two arms. These are the checks that say the harness reproduced
	//    steve's case rather than running the same load twice.
	//
	// A fresh world never touches the card and generates all 81 columns.
	CHECK(fresh.calls[LOAD_STAGE_REGION_IO] == 0);
	CHECK(fresh.calls[LOAD_STAGE_DECODE] == 0);
	CHECK(fresh.calls[LOAD_STAGE_GENERATE] == COLUMNS);

	// A reloaded world asks the card about every column, decodes the 25 that were saved, and
	// still generates the other 56 from the seed. If these two ever add up to something other
	// than COLUMNS, a column has been loaded twice or not at all.
	CHECK(reload.calls[LOAD_STAGE_REGION_IO] == COLUMNS);
	CHECK(reload.calls[LOAD_STAGE_DECODE] == 25);
	CHECK(reload.calls[LOAD_STAGE_GENERATE] == COLUMNS - 25);
	CHECK(reload.calls[LOAD_STAGE_DECODE] + reload.calls[LOAD_STAGE_GENERATE] == COLUMNS);

	// Both arms install every column and mesh the whole 7x7 window.
	CHECK(fresh.calls[LOAD_STAGE_INSTALL] == COLUMNS);
	CHECK(reload.calls[LOAD_STAGE_INSTALL] == COLUMNS);
	CHECK(fresh.calls[LOAD_STAGE_MESH] == (2 * MESH_R + 1) * (2 * MESH_R + 1) * COLUMN_CHUNKS);
	CHECK(reload.calls[LOAD_STAGE_MESH] == fresh.calls[LOAD_STAGE_MESH]);

	// The edits came back. Without this the reload arm could be decoding 25 columns of
	// nothing and still look identical in every count above.
	worldInit(&s_live);
	const uint32_t back = regionReadColumnCached(testDir(), 1, 1, s_iobuf, sizeof s_iobuf);
	CHECK(back > 0);
	CHECK(regionDecodeColumn(&s_live, 1, 1, s_iobuf, back));
	CHECK(worldGet(&s_live, 1 * CHUNK_DIM + 7, 40, 1 * CHUNK_DIM + 9) == BLOCK_WOOD);
	worldExit(&s_live);

	// ...and the two worlds are genuinely different, which is what makes the two breakdowns
	// worth comparing at all.
	CHECK(hash_fresh != hash_reload);

	// ── The behaviour lock ────────────────────────────────────────────────────────────
	//
	// hashWorld() is FNV-1a over every loaded chunk's DECOMPRESSED cells, in ring order,
	// including which columns and which chunks exist at all. Pinned to a literal here, not
	// merely printed, because printing it only proves something if a human happens to
	// compare two runs — and the whole point of task 48b was to make the load faster
	// WITHOUT changing a single block of the world it loads.
	//
	// Any optimisation to the generator, the region codec, the chunk codec or the install
	// path that is not exactly output-preserving lands here as a red check with the new
	// value printed one line above, rather than as a world that quietly differs from the
	// one the player saved.
	//
	// If you have deliberately changed world generation, this check is SUPPOSED to go red.
	// Update the two literals, say so in the changelog, and be aware that every existing
	// save now generates different terrain outside the columns already written to the card.
	//
	// Seed 1337, GEN_VERSION_NEWEST, 81 columns of the RENDER_DIST_MAX ring, lighting off.
	//
	// ── WHY THESE TWO LITERALS MOVED (2026-08-25) ────────────────────────────────────
	//
	// They were 0x5a7a4b485f734255 / 0x6bf92d6e4a81c08c. Commit 31add01, "six biomes on a
	// temperature/humidity rectangle (v1.8.3 Phase 2)", deliberately changed world
	// generation by adding a biome classifier, so this check went red exactly as the
	// paragraph above says it is supposed to. GEN_VERSION_NEWEST is GEN_VERSION_DENSITY
	// (genversion.h), so what these two watch is the DENSITY generator; the frozen legacy
	// path is watched separately and did not move — see the last bullet.
	//
	// The change was bisected before the pins were touched, and it is confined to exactly
	// where a biome classifier should touch and nowhere else. Measured over this same
	// 81-column ring of seed 1337, 2,654,208 cells:
	//
	//   * 7,588 cells differ, 0.2859% of the ring.
	//   * Above the surface: 5,088.
	//   * At the surface: 2,498, and EVERY ONE of them is GRASS -> DIRT — the tundra cap.
	//   * Below the surface: exactly 2, both leaves of a canopy overhanging a cliff.
	//   * PLAINS cells at or below the surface that moved: 0.
	//   * No height, no density and no cave moved anywhere.
	//   * First differing byte is 17744 = (x -49, y 69, z -60), above the surface at y 68,
	//     TALL_GRASS -> AIR, biome TAIGA.
	//   * All twelve GEN_VERSION_LEGACY byte-identity fingerprints in world_test.c stayed
	//     green through the whole feature, so no existing pre-v1.7.0 save re-generates.
	//
	// The two values below were re-derived by a standalone probe that drives the same real
	// modules but does NOT link loadprof.c, rather than by copying what the failing run
	// printed — a failing test's printout is computed by the very code under suspicion and
	// is not evidence on its own. The probe reproduced both values on two consecutive runs,
	// and moved to a completely different pair when fed seed 1338 or GEN_VERSION_LEGACY, so
	// it is sensitive to the thing it claims to measure rather than printing a constant.
	//
	// ── WHY THEY MOVED AGAIN (2026-08-30) ────────────────────────────────────────────
	//
	// They were 0xee631edfa54555fd / 0x90f77b85b6bd007c. Commit 6d4847e, "add snow, ice,
	// cactus, dead bush and fern core block ids (v1.8.3 Phase 3)", deliberately changed
	// world generation again — a snow cap on tundra ground, ice on the top cell of a cold
	// sea, and a flora pass on its own salt — so this check went red exactly as the
	// paragraph above says it is supposed to.
	//
	// Re-derived the same way as last time and for the same reason: a standalone probe that
	// links the real world modules but NOT loadprof.c, re-implementing the ring walk and the
	// FNV-1a mix from this file. Its validity is not asserted, it is demonstrated — built
	// against the PARENT commit 263e887 that probe printed 0xee631edfa54555fd /
	// 0x90f77b85b6bd007c, i.e. it reproduces the literals it is replacing. Built against
	// 6d4847e it printed the two below, twice, and a different pair for seed 1338.
	//
	// Measured over the same 81-column ring of seed 1337, 2,654,208 cells:
	//
	//   * 4,316 cells differ, 0.1626% of the ring.
	//   * DIRT -> SNOW    2,498 — the tundra cap. The same 2,498 cells the biome classifier
	//                     turned GRASS -> DIRT in Phase 2, now wearing the block that
	//                     placeholder was standing in for. One for one.
	//   * AIR  -> FERN    1,060, y 67..98, on taiga and jungle grass.
	//   * WATER -> ICE      758, EVERY ONE of them at y=63, which is GEN_SEA_LEVEL - 1.
	//   * Nothing else moved at all. The only ids appearing on the new side are 10 SNOW,
	//     11 ICE and 14 FERN; the only ids leaving are AIR, DIRT and WATER. No height, no
	//     density, no cave and no tree moved.
	//   * First differing byte is 17422 = column (-4,-4), local (14, y 68, 0), AIR -> FERN.
	//   * NOT exercised by this ring: cactus (12) and dead bush (13) never appear, because
	//     seed 1337's 81 columns contain no desert. They are covered by world_test.c's flora
	//     census on seed 90210, not here, and this pin would not catch a regression in them.
	//   * GEN_VERSION_LEGACY did not move: the same probe run with genv 1 printed
	//     0x5261168a27a7fa18 / 0xb5dca6b8b125ef35 against BOTH commits, so no existing
	//     pre-v1.7.0 save re-generates.
	//
	// ── WHY THEY MOVED A THIRD TIME (2026-08-31) ─────────────────────────────────────
	//
	// They were 0xb51487523ffaaf9b / 0x4a9b3a1d2fffd77a. Roadmap task 52, "per-biome
	// tree silhouettes", gave every biome its own trunk range, canopy width range and
	// crown shape (round / conifer / broad) plus a 2 x 2 jungle trunk, so biome worlds
	// grow different foliage than they used to. That is precisely the change this pin
	// exists to notice, and it went red as it is supposed to.
	//
	// Not re-derived from a standalone probe this time. A probe is what you build when
	// the failing value cannot be trusted to have come from the code under test; here
	// the suite prints the pair it computed itself, on the same ring it always uses:
	//
	//   identity  fresh=e959bfae490df573  reload=f4bc0b9d80bb2f8e
	//
	// The two literals below are that line, transcribed. What makes them safe to take
	// at face value is the A/B cell census run against the parent commit c8c9e90 over
	// the same 81-column ring of seed 1337, 2,654,208 cells:
	//
	//   * 2,123 cells differ, 0.0800% of the ring, every one of them in y 71..99.
	//   * AIR -> LEAVES 1,539, LEAVES -> AIR 512, LEAVES -> WOOD 63, WOOD -> LEAVES 2,
	//     AIR -> WOOD 1, WOOD -> AIR 1, and 5 cells trading a flora id for leaves.
	//   * ZERO ground-to-ground transitions. No height, no density, no cave, no biome
	//     surface and no water moved — only what grows on top of them, which is all
	//     task 52 was allowed to touch.
	//
	// GEN_VERSION_LEGACY and GEN_VERSION_DENSITY worlds are unaffected: the new tree
	// table is only consulted at genv >= GEN_VERSION_BIOME, so no existing save
	// re-generates.
	// ── WHY THEY MOVED A FOURTH TIME (2026-09-02) ────────────────────────────────────
	//
	// They were 0xe959bfae490df573 / 0xf4bc0b9d80bb2f8e. v1.8.8, "per-biome blocks and
	// plants": taiga now grows spruce and forest grows birch instead of both growing oak,
	// tall grass can be two blocks tall, four flowers are scattered per biome, and oak and
	// birch hang apples under their canopies. Biome worlds therefore contain block ids this
	// ring has never seen. That is precisely the change this pin exists to notice, and it
	// went red as it is supposed to.
	//
	// Taken the same way as the third move — transcribed from the line the suite prints on
	// its own ring, not from a standalone probe:
	//
	//   identity  fresh=ba19cb5b038483fc  reload=c3ebcc973d6b3675
	//
	// What makes them safe to take at face value is the A/B cell census against the
	// pre-change generator (world/worldgen.{c,h} from HEAD, everything else from this tree)
	// over 90 columns — the same 81-column ring of seed 1337 plus world_test.c's twelve
	// pinned columns — 2,949,120 cells:
	//
	//   * 5,338 cells differ, 0.1810%, every one of them in y 67..99.
	//   * LEAVES -> SPRUCE_LEAVES 2,905, LEAVES -> BIRCH_LEAVES 688, WOOD -> SPRUCE_LOG 448,
	//     WOOD -> BIRCH_LOG 63, and AIR -> {BLUEBELL 522, TALL_GRASS_TOP 461, POPPY 128,
	//     DAISY 102, APPLE 21}. Nine transitions, and no others at all.
	//   * ZERO ground-to-ground transitions in all 90 columns. No height, no density, no
	//     cave, no biome surface and no water moved.
	//   * Counted over world_test.c's six-biome area, the two species sums BALANCE EXACTLY:
	//     1,108 oak logs became 753 oak + 300 birch + 55 spruce, and 6,985 oak leaves became
	//     3,851 + 2,778 + 356. Not one tree was gained, lost, moved or resized — the species
	//     change is a pure relabel. Tall grass is the same 1,628 base cells in both arms,
	//     1,017 bare and 611 now carrying a top, so no grass cell moved either.
	//
	// GEN_VERSION_LEGACY and GEN_VERSION_DENSITY worlds are unaffected: every one of these
	// behaviours is gated on genv >= GEN_VERSION_BIOME or on the biome-params branch, so no
	// existing save re-generates. world_test.c's legacy byte-identity hashes and its legacy
	// intruder sweep (widened to reject all ten new ids) are the checks that say so.
	// ── WHY THEY MOVED A FIFTH TIME (2026-09-02) ─────────────────────────────────────
	//
	// They were 0xba19cb5b038483fc / 0xc3ebcc973d6b3675. v1.8.11 adds GEN_VERSION_CAVES: a
	// sparse worm carver (world/cave_carve.c) replaces the old per-block noise-field cave
	// test for genv >= GEN_VERSION_CAVES. This ring generates at GEN_VERSION_NEWEST, which
	// is now CAVES, so it re-carves. That is exactly what this pin exists to notice.
	//
	// Unlike the four moves above, this one DOES move ground-to-ground cells, and that is
	// intended — a cave carver's entire job is turning STONE into AIR. So the census below
	// is what makes the new numbers safe to take, rather than the "zero ground transitions"
	// argument the previous four leaned on:
	//
	//   * 122,295 of 2,654,208 cells differ, 4.6%.
	//   * The dominant transition is AIR -> STONE, 118,083 — i.e. the OLD dense noise caves
	//     are being FILLED BACK IN, and the new carver opens far fewer, larger tunnels.
	//     That direction is the point of the change: the old field made swiss cheese.
	//
	// GEN_VERSION_LEGACY, _DENSITY and _BIOME worlds are unaffected — worldgen_density.c
	// reaches the new carver only at genv >= GEN_VERSION_CAVES, so no existing save
	// re-generates. Every world on a card today predates this and keeps its old caves.
	//
	// Transcribed from the line the suite prints on its own ring, not from a standalone
	// probe:  identity fresh=67c24ddd7a91454b reload=f03ef44657a3596d
	CHECK(hash_fresh  == 0x67c24ddd7a91454bULL);
	CHECK(hash_reload == 0xf03ef44657a3596dULL);

	regionCacheClose();
	testRmTree(testDir());
	free(s_verts);
	free(s_idx);

	printf("\n%s %d checks, %d failed", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("  first %s", s_first);
	printf("\n");
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
