// Region file growth, measured rather than argued.
//
// world/region.c's payload arena is append-only: a column that is saved again lands its new
// bytes on the end of the file and the previous copy is left where it is, pointed at by
// nothing. That is deliberate and it is what makes a torn write survivable (region.h says
// why). The cost is that a region file grows every time a column is saved, forever, and for
// most of v1.7/v1.8 nothing reclaimed any of it — regionCompact() existed, was proved by
// world_test.c's four testRegionCompact* tests, and had no caller anywhere in the tree.
//
// This binary is the number behind that sentence. It drives the REAL region.c through a
// realistic edit session — a working set of columns inside one region, each one re-saved
// many times with payloads that change length the way a player's edits change how a column
// RLE-compresses — and stat()s the file on disk as it goes. It then asserts the growth is
// bounded, which is only true with compaction wired in.
//
// Its own main(), like every other host binary: tests/host_test.c owns world_test's.
//
// The __3DS__ guard is load-bearing, not tidy — the console Makefile globs every .c under
// source/world (Makefile:216), and this file's main() collides with source/main.c's. Measured
// rather than assumed: without it the console link fails with "multiple definition of `main';
// main.o:source/main.c:2798 first defined here".
#ifndef __3DS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_codec.h"
#include "world/region.h"
#include "world/registry.h"
#include "world/world.h"

static int s_checks;
static int s_fails;

#define CHECK(expr)                                                      \
	do {                                                                 \
		s_checks++;                                                      \
		if (!(expr)) {                                                   \
			s_fails++;                                                   \
			printf("FAIL L%d %s\n", __LINE__, #expr);                    \
		}                                                                \
	} while (0)

// ── Fixture ───────────────────────────────────────────────────────────────────────────

// All inside region (0,0) and all in distinct directory slots, so no column can mask
// another and a slot-arithmetic error cannot hide inside the totals.
#define WORK_COLS   24

// How many times each column in the working set is re-saved. A player who spends an evening
// building in one place saves the same handful of columns over and over; that is the whole
// shape of the problem, so the workload is that and nothing else.
#define REWRITES    40

static const char* testDir(void)
{
	static char dir[256];
	if (!dir[0]) {
		const char* base = getenv("TMPDIR");
		if (!base || !base[0]) base = "/tmp";
		snprintf(dir, sizeof(dir), "%s/bs_rgc_%ld", base, (long)getpid());
	}
	return dir;
}

static void testMkdir(const char* p)
{
#ifdef _WIN32
	mkdir(p);
#else
	mkdir(p, 0755);
#endif
}

static long fileBytes(const char* path)
{
	struct stat st;
	if (stat(path, &st) != 0) return -1;
	return (long)st.st_size;
}

static long regionBytes(void)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/r.0.0.bsr", testDir());
	return fileBytes(p);
}

static void cleanup(void)
{
	char p[320];
	regionCacheClose();
	snprintf(p, sizeof(p), "%s/r.0.0.bsr", testDir()); remove(p);
	snprintf(p, sizeof(p), "%s/r.0.0.tmp", testDir()); remove(p);
	snprintf(p, sizeof(p), "%s/r.0.0.bak", testDir()); remove(p);
	rmdir(testDir());
}

// A cheap deterministic generator. Not rng.h's, because this file has to produce the same
// byte counts on every machine that runs it for the numbers in the report to mean anything.
static uint32_t s_seed = 0x9E3779B9u;
static uint32_t nextRand(void)
{
	s_seed ^= s_seed << 13;
	s_seed ^= s_seed >> 17;
	s_seed ^= s_seed << 5;
	return s_seed;
}

// Builds one column of realistic terrain in `w` and encodes it exactly the way the game
// does. `churn` is how many scattered single-block edits are sprinkled on top, which is what
// makes the payload length vary between saves: the codec's run-length form gets longer as
// the content gets less uniform, so a column that has been built in encodes larger than one
// straight out of the generator. That is the real reason a save's length changes, so it is
// the mechanism used here rather than a random length.
static uint32_t buildColumn(World* w, int32_t cx, int32_t cz, int churn,
                            uint8_t* out, uint32_t cap)
{
	static BlockId flat[CHUNK_BLOCKS];

	worldColumnRemove(w, cx, cz);

	// Four solid chunks under four empty ones: 64 blocks of ground in a 128-tall world, which
	// is the shape worldgen produces. The empty half stays unallocated and costs one bit each
	// in the payload's present mask, exactly as it does in the game.
	for (int cy = 0; cy < 4; cy++) {
		const BlockId id = (cy == 3) ? BLOCK_GRASS : (cy == 2 ? BLOCK_DIRT : BLOCK_STONE);
		for (int i = 0; i < CHUNK_BLOCKS; i++) flat[i] = id;
		if (!worldSetChunkAll(w, cx, cy, cz, flat)) return 0;
	}

	for (int i = 0; i < churn; i++) {
		const int x = (int)(nextRand() & 15);
		const int y = (int)(nextRand() % 64);
		const int z = (int)(nextRand() & 15);
		const BlockId id = (nextRand() & 1) ? BLOCK_AIR : BLOCK_STONE;
		worldSet(w, cx * CHUNK_DIM + x, y, cz * CHUNK_DIM + z, id);
	}

	const Column* col = worldColumn(w, cx, cz);
	if (!col) return 0;
	return regionEncodeColumn(col, out, cap);
}

// ── The measurement ───────────────────────────────────────────────────────────────────

static int32_t s_cx[WORK_COLS];
static int32_t s_cz[WORK_COLS];

// The last payload written for each column, kept so the readback control can compare bytes
// and so live-vs-dead can be computed without reaching into region.c's private Dir.
static uint8_t  s_last[WORK_COLS][REGION_COL_MAX];
static uint32_t s_last_len[WORK_COLS];

static int s_compactions;

static uint8_t s_scratch[REGION_COL_MAX];
static uint8_t s_got[REGION_COL_MAX];

static void spreadColumns(void)
{
	// 24 DISTINCT columns inside the 16x16 region. Distinct is the load-bearing word: the
	// first draft used cx=(i*7)&15, cz=(i*5)&15, whose period in i is 16, so columns 16..23
	// landed on the same slots as 0..7 and the readback control reported "8 of 24 columns
	// wrong" in BOTH arms — a fixture bug that looked exactly like a region.c bug.
	for (int i = 0; i < WORK_COLS; i++) {
		s_cx[i] = (int32_t)(i % 16);
		s_cz[i] = (int32_t)((i / 16) * 5 + 1);
	}
	for (int i = 0; i < WORK_COLS; i++)
		for (int j = i + 1; j < WORK_COLS; j++)
			CHECK(!(s_cx[i] == s_cx[j] && s_cz[i] == s_cz[j]));
}

static uint32_t liveBytes(void)
{
	uint32_t live = 0;
	for (int i = 0; i < WORK_COLS; i++) live += s_last_len[i];
	return live;
}

// Every column reads back byte for byte as the last thing written to it. This is the CONTROL:
// it is a statement about correctness, not about size, and it must be green whether or not
// compaction runs. A sabotage that turns compaction off must leave this green; a sabotage
// that breaks the repack must turn it red.
static void controlReadback(const char* tag)
{
	int bad = 0;
	for (int i = 0; i < WORK_COLS; i++) {
		memset(s_got, 0, sizeof(s_got));
		const uint32_t n = regionReadColumn(testDir(), s_cx[i], s_cz[i], s_got, sizeof(s_got));
		if (n != s_last_len[i] || memcmp(s_got, s_last[i], n) != 0) bad++;
	}
	printf("  control_readback[%s]: %d of %d columns wrong\n", tag, bad, WORK_COLS);
	CHECK(bad == 0);
}

// Runs the session. `compact_every` of 0 means "never compact from here" — that is the
// pre-fix arm, kept as a callable so the before and after numbers come off the same workload
// rather than off two programs that only look alike.
//
// Returns the file size in bytes at the end.
static long runSession(bool wire_compaction, const char* tag)
{
	World w;
	worldInit(&w);
	spreadColumns();

	long peak = 0;

	printf("\n[%s] %d columns x %d rewrites = %d saves into one region file\n",
	       tag, WORK_COLS, REWRITES, WORK_COLS * REWRITES);
	printf("  %-8s %-12s %-12s %-12s %s\n", "saves", "file bytes", "live bytes", "dead bytes", "dead %");

	int saves = 0;
	for (int pass = 0; pass < REWRITES; pass++) {
		for (int i = 0; i < WORK_COLS; i++) {
			// Churn climbs with the pass, so the payloads grow the way a column being built in
			// grows. A workload whose payloads were all one length would let a compactor that
			// happened to reuse offsets look correct.
			const int churn = 8 + pass * 6 + i;
			const uint32_t n = buildColumn(&w, s_cx[i], s_cz[i], churn,
			                               s_scratch, sizeof(s_scratch));
			if (n == 0) { printf("  encode refused at pass %d col %d\n", pass, i); CHECK(n != 0); break; }

			CHECK(regionWriteColumn(testDir(), s_cx[i], s_cz[i], s_scratch, n));

			memcpy(s_last[i], s_scratch, n);
			s_last_len[i] = n;
			saves++;

			// The wired-in behaviour under test. regionMaintain is a no-op when there is
			// nothing worth reclaiming, so calling it per save is what the game does.
			// The wired-in behaviour under test, and the SAME function app/worker.c's
			// workerWriteSave calls — not regionCompact directly, which would leave the
			// wiring itself untested by anything that links.
			if (wire_compaction) {
				const long before = regionBytes();
				regionMaintain(testDir(), regionOf(s_cx[i]), regionOf(s_cz[i]));
				if (regionBytes() < before) s_compactions++;
			}
		}

		const long bytes = regionBytes();
		if (bytes > peak) peak = bytes;

		if (pass % 5 == 0 || pass == REWRITES - 1) {
			const uint32_t live = liveBytes();
			const long arena = bytes - (long)REGION_ARENA_OFF;
			const long dead  = arena - (long)live;
			printf("  %-8d %-12ld %-12lu %-12ld %.1f%%\n",
			       saves, bytes, (unsigned long)live, dead,
			       arena > 0 ? 100.0 * (double)dead / (double)arena : 0.0);
		}
	}

	worldExit(&w);
	regionCacheClose();

	const long final_bytes = regionBytes();
	printf("  peak file bytes: %ld   final: %ld   compactions: %d\n",
	       peak, final_bytes, s_compactions);
	return final_bytes;
}

// ── The .bak swap, which is what makes wiring compaction up safe ──────────────────────
//
// v1.8.2 changed regionCompact's swap from remove(.bsr)+rename(.tmp) to
// rename(.bsr,.bak)+rename(.tmp,.bsr)+remove(.bak), so that no cut ever leaves the path
// depending on freshly written bytes being on the card. world_test.c's testRegionCompactPowerCut
// sweeps the .tmp states and is unchanged by that (a .bak never appears in it), so the three
// .bak states are checked here.
static bool copyFile(const char* from, const char* to)
{
	FILE* a = fopen(from, "rb");
	if (!a) return false;
	FILE* b = fopen(to, "wb");
	if (!b) { fclose(a); return false; }

	static uint8_t io[8192];
	size_t n;
	bool ok = true;
	while ((n = fread(io, 1, sizeof(io), a)) > 0)
		if (fwrite(io, 1, n, b) != n) { ok = false; break; }

	fclose(a);
	ok = (fclose(b) == 0) && ok;
	return ok;
}

static int readAll(void)
{
	int bad = 0;
	for (int i = 0; i < WORK_COLS; i++) {
		memset(s_got, 0, sizeof(s_got));
		const uint32_t n = regionReadColumn(testDir(), s_cx[i], s_cz[i], s_got, sizeof(s_got));
		if (n != s_last_len[i] || memcmp(s_got, s_last[i], n) != 0) bad++;
	}
	return bad;
}

static void testBakRecovery(void)
{
	char bsr[320], tmp[320], bak[320], keep[320], junk[320];
	snprintf(bsr,  sizeof(bsr),  "%s/r.0.0.bsr", testDir());
	snprintf(tmp,  sizeof(tmp),  "%s/r.0.0.tmp", testDir());
	snprintf(bak,  sizeof(bak),  "%s/r.0.0.bak", testDir());
	snprintf(keep, sizeof(keep), "%s/good.bin",  testDir());
	snprintf(junk, sizeof(junk), "%s/junk.bin",  testDir());

	regionCacheClose();
	CHECK(copyFile(bsr, keep));
	const long good = fileBytes(keep);
	CHECK(good > (long)REGION_ARENA_OFF);

	// A deliberately worthless stand-in for "a .tmp whose sectors never reached the card".
	// The whole point of the .bak is that recovery never has to look at this file, so it is
	// filled with something that could not possibly load.
	{
		FILE* f = fopen(junk, "wb");
		CHECK(f != NULL);
		if (f) { for (int i = 0; i < 4096; i++) fputc(0xA5, f); fclose(f); }
	}

	// State 1: cut between the two renames — .bak present, no .bsr, and a torn .tmp beside
	// it. The old region must come back and the .tmp must be ignored, not promoted.
	regionCacheClose();
	remove(bsr); remove(tmp); remove(bak);
	CHECK(copyFile(keep, bak));
	CHECK(copyFile(junk, tmp));
	printf("  bak_state1 (bak, no bsr, torn tmp): %d of %d columns wrong\n", readAll(), WORK_COLS);
	CHECK(readAll() == 0);
	CHECK(fileBytes(bsr) == good);      // restored from the .bak
	CHECK(fileBytes(bak) < 0);          // and the .bak cleaned up
	CHECK(fileBytes(tmp) < 0);          // the torn .tmp never got a look in

	// State 2: cut after the second rename but before the cleanup — .bak and .bsr both
	// present. The .bsr is the new file and wins; the stale .bak must be dropped, not
	// restored over the top of it.
	regionCacheClose();
	remove(bsr); remove(tmp); remove(bak);
	CHECK(copyFile(keep, bsr));
	CHECK(copyFile(junk, bak));
	printf("  bak_state2 (bak + bsr): %d of %d columns wrong\n", readAll(), WORK_COLS);
	CHECK(readAll() == 0);
	CHECK(fileBytes(bsr) == good);
	CHECK(fileBytes(bak) < 0);

	// State 3: a completed compaction leaves neither sidecar behind. Checked because the
	// two above would both still pass if regionCompact simply never removed its .bak.
	regionCacheClose();
	remove(bsr); remove(tmp); remove(bak);
	CHECK(copyFile(keep, bsr));
	regionMaintain(testDir(), 0, 0);
	CHECK(fileBytes(bak) < 0);
	CHECK(fileBytes(tmp) < 0);
	CHECK(readAll() == 0);

	remove(keep);
	remove(junk);
}

int main(void)
{
	registryInitCore();
	registryFreeze();

	testMkdir(testDir());
	cleanup();
	testMkdir(testDir());

	// ── Arm 1: the append-only file with nothing reclaiming it ────────────────────────
	//
	// Compaction is not called at all here. This is what every shipped build did, and the
	// number it prints is the bug.
	s_seed = 0x9E3779B9u;
	const long unbounded = runSession(false, "no compaction");
	controlReadback("no compaction");

	const uint32_t live_end = liveBytes();

	cleanup();
	testMkdir(testDir());

	// ── Arm 2: the same workload with maintenance wired in ────────────────────────────
	s_seed = 0x9E3779B9u;
	const long bounded = runSession(true, "compaction wired");
	controlReadback("compaction wired");

	// ── Arm 3: the crash states the new three-name swap can leave ─────────────────────
	testBakRecovery();

	printf("\nsummary: no-compaction %ld bytes, compaction-wired %ld bytes, live at end %lu\n",
	       unbounded, bounded, (unsigned long)live_end);

	// The claim, and it has to be able to fail on its own. Without compaction the file holds
	// every one of the 960 payloads ever written; with it the file is held near what is
	// actually live. The bound is deliberately generous — regionCompact only fires past its
	// own half-wasted gate, so a compacted file is expected to sit somewhere between "live"
	// and "twice live" plus the two fixed directory copies — and it still separates the two
	// arms by an order of magnitude.
	const long ceiling = (long)REGION_ARENA_OFF + 3 * (long)live_end;
	printf("bound: compacted file must be under %ld bytes\n", ceiling);
	CHECK(bounded < ceiling);

	// Non-vacuity. If the unbounded arm were not far above the ceiling the check above would
	// be passing for free, and this file would be proving nothing at all.
	CHECK(unbounded > ceiling * 2);

	cleanup();

	if (s_fails) {
		printf("\nregion growth: FAILED - %d of %d checks\n", s_fails, s_checks);
		return 1;
	}
	printf("\nregion growth: PASS  %d checks\n", s_checks);
	return 0;
}

#endif  // !__3DS__
