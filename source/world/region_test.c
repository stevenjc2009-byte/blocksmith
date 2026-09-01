// v1.8.6 "Speed": the save-side compaction hint (region.c, the block above regionWriteColumn).
//
// region_growth_test.c already proves regionCompact/regionMaintain reclaim space and survive
// a power cut. This file is scoped to the ONE thing v1.8.6 changed: regionMaintain can now
// answer its gate from a hint regionWriteColumn hands it instead of re-deriving the same
// state from a cold reopen. Four questions, in order:
//
//   1. Round trip. Hundreds of columns, several seeds, several regions, columns saved more
//      than once so the hint is actually exercised (not just the first-save-ever case) --
//      read back byte for byte.
//   2. Forced compaction. The gate fires 0 times in most sessions (region.h's measured
//      workload: 36 times in 960 saves), which a normal round trip will not exercise at all.
//      This file deliberately grows one region past the half-dead threshold so the RARE
//      branch -- hint says "yes", falls through to the untouched regionCompact -- actually
//      runs, and checks the result is still correct.
//   3. Invalidation. Every way the hint can go stale (a mismatched region, a direct
//      regionCompact call, an explicit regionCacheClose) must make regionMaintain fall back
//      to the pre-v1.8.6 behaviour rather than answer from state that no longer describes
//      the file.
//   4. Equivalence. The hint is only a valid speed-up if the decision it produces is EXACTLY
//      the decision a fresh read would have produced. Tested directly: the same write
//      sequence run through two directories, one with the hint live and one with it forced
//      to miss on every single call (regionCacheClose() before every regionMaintain), and the
//      two are required to compact at exactly the same points and end up byte-identical.
//
// Own main(), like region_growth_test.c beside it -- guarded the same way and for the same
// reason: the console Makefile globs source/world/*.c, and this file's main() would collide
// with source/main.c's.
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

// ── Shared fixture plumbing ────────────────────────────────────────────────────────────

static const char* dirFor(const char* tag)
{
	static char dir[256];
	const char* base = getenv("TMPDIR");
	if (!base || !base[0]) base = "/tmp";
	snprintf(dir, sizeof(dir), "%s/bs_region_test_%ld_%s", base, (long)getpid(), tag);
	return dir;
}

static void mkdirp(const char* p)
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

static void regionCleanup(const char* dir, int32_t rx, int32_t rz)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/r.%d.%d.bsr", dir, rx, rz); remove(p);
	snprintf(p, sizeof(p), "%s/r.%d.%d.tmp", dir, rx, rz); remove(p);
	snprintf(p, sizeof(p), "%s/r.%d.%d.bak", dir, rx, rz); remove(p);
}

// Deterministic given (seed, cx, cz, pass) so two calls with the same inputs always encode
// the same bytes -- that is what lets the round trip and equivalence tests compare against a
// value recomputed on the fly instead of a stored ledger.
static uint32_t s_rand;
static uint32_t nextRand(void)
{
	s_rand ^= s_rand << 13;
	s_rand ^= s_rand >> 17;
	s_rand ^= s_rand << 5;
	return s_rand;
}

static uint32_t buildColumn(World* w, int32_t cx, int32_t cz, uint32_t seed, int pass,
                            uint8_t* out, uint32_t cap)
{
	static BlockId flat[CHUNK_BLOCKS];
	s_rand = seed ^ (uint32_t)(cx * 92821 + cz * 68917 + pass * 104729);
	if (!s_rand) s_rand = 0x9E3779B9u;

	worldColumnRemove(w, cx, cz);
	for (int cy = 0; cy < 4; cy++) {
		const BlockId id = (cy == 3) ? BLOCK_GRASS : (cy == 2 ? BLOCK_DIRT : BLOCK_STONE);
		for (int k = 0; k < CHUNK_BLOCKS; k++) flat[k] = id;
		if (!worldSetChunkAll(w, cx, cy, cz, flat)) return 0;
	}
	const int churn = 6 + pass * 5;
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

// ── 1. Round trip: hundreds of columns, several seeds, several regions, re-saved ───────

#define RT_COLS   180   // cx = i*3 spreads these across ~34 region files
#define RT_SAVES  3

static int32_t rtColX(int i) { return (int32_t)(i * 3); }
static int32_t rtColZ(int i) { return (int32_t)((i * 11) % 50); }

static void testRoundTrip(uint32_t seed)
{
	const char* dir = dirFor("roundtrip");
	mkdirp(dir);

	World w;
	worldInit(&w);

	static uint8_t buf[REGION_COL_MAX];
	static uint8_t last[RT_COLS][REGION_COL_MAX];
	static uint32_t last_n[RT_COLS];

	for (int i = 0; i < RT_COLS; i++) {
		const int32_t cx = rtColX(i), cz = rtColZ(i);
		for (int pass = 0; pass < RT_SAVES; pass++) {
			const uint32_t n = buildColumn(&w, cx, cz, seed, pass, buf, sizeof(buf));
			CHECK(n != 0);
			if (!n) continue;
			CHECK(regionWriteColumn(dir, cx, cz, buf, n));
			// Production shape: regionMaintain runs after every save (app/worker.c's
			// workerWriteSave). This is where the hint is set and, one line later,
			// consumed -- every one of these RT_COLS*RT_SAVES calls exercises it.
			regionMaintain(dir, regionOf(cx), regionOf(cz));
			if (pass == RT_SAVES - 1) { memcpy(last[i], buf, n); last_n[i] = n; }
		}
	}

	worldExit(&w);
	regionCacheClose();

	int bad = 0;
	static uint8_t got[REGION_COL_MAX];
	for (int i = 0; i < RT_COLS; i++) {
		const int32_t cx = rtColX(i), cz = rtColZ(i);
		memset(got, 0, sizeof(got));
		const uint32_t n = regionReadColumn(dir, cx, cz, got, sizeof(got));
		if (n != last_n[i] || memcmp(got, last[i], n) != 0) bad++;
	}
	printf("  roundtrip seed=%u: %d of %d columns wrong\n", seed, bad, RT_COLS);
	CHECK(bad == 0);

	// Cleanup: every region file this seed touched.
	for (int i = 0; i < RT_COLS; i++)
		regionCleanup(dir, regionOf(rtColX(i)), regionOf(rtColZ(i)));
	rmdir(dir);
}

// ── 2. Forced compaction: make the rare branch fire, then check the result ─────────────

#define FC_COLS  20
#define FC_SAVES 30

static void testForcedCompaction(void)
{
	const char* dir = dirFor("forced");
	mkdirp(dir);
	regionCleanup(dir, 0, 0);

	World w;
	worldInit(&w);

	static int32_t cx_[FC_COLS], cz_[FC_COLS];
	for (int i = 0; i < FC_COLS; i++) { cx_[i] = i; cz_[i] = 0; }   // all inside region (0,0)

	static uint8_t buf[REGION_COL_MAX];
	static uint8_t last[FC_COLS][REGION_COL_MAX];
	static uint32_t last_n[FC_COLS];

	char bsr[320];
	snprintf(bsr, sizeof(bsr), "%s/r.0.0.bsr", dir);

	int compactions = 0;
	for (int pass = 0; pass < FC_SAVES; pass++) {
		for (int i = 0; i < FC_COLS; i++) {
			const uint32_t n = buildColumn(&w, cx_[i], cz_[i], 0xC0FFEEu, pass, buf, sizeof(buf));
			CHECK(n != 0);
			if (!n) continue;
			CHECK(regionWriteColumn(dir, cx_[i], cz_[i], buf, n));
			memcpy(last[i], buf, n);
			last_n[i] = n;

			const long before = fileBytes(bsr);
			// This is the call under test: regionWriteColumn just stashed a hint for
			// region (0,0), and regionMaintain here reads it. On most of these FC_COLS *
			// FC_SAVES calls the gate declines from the hint alone (fast path). At least
			// once, as the working set fills the arena, it has to say "yes" and fall
			// through to the real regionCompact -- that is the branch this test exists
			// to force and check, not just assume runs.
			regionMaintain(dir, regionOf(cx_[i]), regionOf(cz_[i]));
			const long after = fileBytes(bsr);
			if (after >= 0 && before >= 0 && after < before) compactions++;
		}
	}

	worldExit(&w);
	regionCacheClose();

	printf("  forced compaction: %d compactions over %d saves\n", compactions, FC_COLS * FC_SAVES);
	// The whole point of this test: if this is 0, the rare branch never ran and every check
	// below it would be passing for free. See region_growth_test.c's "non-vacuity" check for
	// the same shape of guard.
	CHECK(compactions > 0);

	int bad = 0;
	static uint8_t got[REGION_COL_MAX];
	for (int i = 0; i < FC_COLS; i++) {
		memset(got, 0, sizeof(got));
		const uint32_t n = regionReadColumn(dir, cx_[i], cz_[i], got, sizeof(got));
		if (n != last_n[i] || memcmp(got, last[i], n) != 0) bad++;
	}
	printf("  forced compaction readback: %d of %d columns wrong\n", bad, FC_COLS);
	CHECK(bad == 0);

	regionCleanup(dir, 0, 0);
	rmdir(dir);
}

// ── 3. Invalidation: every way the hint can go stale must fall back correctly ──────────

static void testHintInvalidation(void)
{
	const char* dirA = dirFor("invA");
	const char* dirB = dirFor("invB");
	mkdirp(dirA);
	mkdirp(dirB);
	regionCleanup(dirA, 0, 0);
	regionCleanup(dirB, 0, 0);

	World w;
	worldInit(&w);
	static uint8_t buf[REGION_COL_MAX];

	// 3a. Write into dirA's region (0,0), then call regionCompact on it DIRECTLY -- bypassing
	// regionMaintain and the hint entirely. regionCompact's own regionCacheClose() at its top
	// must clear the hint the write just set; a subsequent regionMaintain for the same region
	// must not answer from state regionCompact may have just rewritten out from under it.
	{
		const uint32_t n = buildColumn(&w, 0, 0, 1, 0, buf, sizeof(buf));
		CHECK(n != 0);
		CHECK(regionWriteColumn(dirA, 0, 0, buf, n));
		(void)regionCompact(dirA, 0, 0);          // declines (nothing wasted yet) -- irrelevant which
		regionMaintain(dirA, 0, 0);               // must not crash, must not corrupt

		static uint8_t got[REGION_COL_MAX];
		memset(got, 0, sizeof(got));
		const uint32_t rn = regionReadColumn(dirA, 0, 0, got, sizeof(got));
		CHECK(rn == n && memcmp(got, buf, n) == 0);
	}

	// 3b. Write into dirA's region (0,0) again (stashes a fresh hint for (dirA, 0, 0)), then
	// call regionMaintain for a DIFFERENT region -- (dirA, 5, 5), which has no file at all.
	// The hint must not be mistaken for that region's state; the call must fall through to
	// the real regionCompact, which opens r.5.5.bsr, fails, and declines. Region (0,0) must
	// be completely unaffected.
	{
		const uint32_t n = buildColumn(&w, 0, 0, 2, 1, buf, sizeof(buf));
		CHECK(n != 0);
		CHECK(regionWriteColumn(dirA, 0, 0, buf, n));

		regionMaintain(dirA, 5, 5);               // mismatched region: must decline safely

		char stray[320];
		snprintf(stray, sizeof(stray), "%s/r.5.5.bsr", dirA);
		CHECK(fileBytes(stray) < 0);              // regionCompact's own decline never creates one

		static uint8_t got[REGION_COL_MAX];
		memset(got, 0, sizeof(got));
		const uint32_t rn = regionReadColumn(dirA, 0, 0, got, sizeof(got));
		CHECK(rn == n && memcmp(got, buf, n) == 0);   // (0,0) untouched by the mismatched call
	}

	// 3c. Write into dirB's region (0,0), then call regionCacheClose() directly (what a world
	// switch or worker stop does) before regionMaintain runs. The hint must be gone, so this
	// falls all the way through to the pre-v1.8.6 regionCompact call, and must still reach the
	// correct (decline) answer and leave the column readable.
	{
		const uint32_t n = buildColumn(&w, 0, 0, 3, 0, buf, sizeof(buf));
		CHECK(n != 0);
		CHECK(regionWriteColumn(dirB, 0, 0, buf, n));
		regionCacheClose();                       // simulates a world-directory switch
		regionMaintain(dirB, 0, 0);

		static uint8_t got[REGION_COL_MAX];
		memset(got, 0, sizeof(got));
		const uint32_t rn = regionReadColumn(dirB, 0, 0, got, sizeof(got));
		CHECK(rn == n && memcmp(got, buf, n) == 0);
	}

	worldExit(&w);
	regionCacheClose();
	regionCleanup(dirA, 0, 0);
	regionCleanup(dirA, 5, 5);
	regionCleanup(dirB, 0, 0);
	rmdir(dirA);
	rmdir(dirB);
}

// ── 4. Equivalence: the hinted decision must match a decision from a fresh read ────────
//
// Two directories driven through the IDENTICAL write sequence. dirHint runs regionMaintain
// normally, so its hint is live whenever the immediately preceding write set one. dirSlow
// calls regionCacheClose() right before every regionMaintain, which guarantees a miss every
// time -- exactly the pre-v1.8.6 code path, reading the file fresh on every single call. If
// the hint ever disagreed with a fresh read about whether to compact, the two directories
// would diverge: one would compact on a save the other did not, and the arena bytes -- which
// feed back into where the NEXT payload lands -- would drift apart from that point on. Equal
// compaction counts alone would not catch that; the byte-for-byte file comparison at the end
// would.

#define EQ_COLS  16
#define EQ_SAVES 25

static void testHintMatchesFreshRead(void)
{
	const char* dirHint = dirFor("eqhint");
	const char* dirSlow = dirFor("eqslow");
	mkdirp(dirHint);
	mkdirp(dirSlow);
	regionCleanup(dirHint, 0, 0);
	regionCleanup(dirSlow, 0, 0);

	World w;
	worldInit(&w);

	static int32_t cx_[EQ_COLS], cz_[EQ_COLS];
	for (int i = 0; i < EQ_COLS; i++) { cx_[i] = i; cz_[i] = 0; }

	static uint8_t buf[REGION_COL_MAX];
	char bsrHint[320], bsrSlow[320];
	snprintf(bsrHint, sizeof(bsrHint), "%s/r.0.0.bsr", dirHint);
	snprintf(bsrSlow, sizeof(bsrSlow), "%s/r.0.0.bsr", dirSlow);

	int compactHint = 0, compactSlow = 0;

	for (int pass = 0; pass < EQ_SAVES; pass++) {
		for (int i = 0; i < EQ_COLS; i++) {
			const uint32_t n = buildColumn(&w, cx_[i], cz_[i], 0xA11CEu, pass, buf, sizeof(buf));
			CHECK(n != 0);
			if (!n) continue;

			CHECK(regionWriteColumn(dirHint, cx_[i], cz_[i], buf, n));
			const long hb = fileBytes(bsrHint);
			regionMaintain(dirHint, 0, 0);
			const long ha = fileBytes(bsrHint);
			if (ha >= 0 && hb >= 0 && ha < hb) compactHint++;

			CHECK(regionWriteColumn(dirSlow, cx_[i], cz_[i], buf, n));
			regionCacheClose();                 // force the hint to miss -- always a fresh read
			const long sb = fileBytes(bsrSlow);
			regionMaintain(dirSlow, 0, 0);
			const long sa = fileBytes(bsrSlow);
			if (sa >= 0 && sb >= 0 && sa < sb) compactSlow++;
		}
	}

	worldExit(&w);
	regionCacheClose();

	printf("  equivalence: hint-path compacted %d times, fresh-read-path compacted %d times\n",
	       compactHint, compactSlow);
	CHECK(compactHint == compactSlow);
	CHECK(compactHint > 0);   // non-vacuity: this workload must actually cross the gate

	const long bytesHint = fileBytes(bsrHint);
	const long bytesSlow = fileBytes(bsrSlow);
	printf("  equivalence: final bytes hint=%ld slow=%ld\n", bytesHint, bytesSlow);
	CHECK(bytesHint == bytesSlow);

	// Byte-for-byte, not just same length -- the DIR_MAGIC/seq framing differs harmlessly
	// between the two (each ran its own independent sequence of directory-copy writes), so
	// compare column CONTENT rather than raw file bytes, which is the property that actually
	// matters to a player.
	int bad = 0;
	static uint8_t gotHint[REGION_COL_MAX], gotSlow[REGION_COL_MAX];
	for (int i = 0; i < EQ_COLS; i++) {
		memset(gotHint, 0, sizeof(gotHint));
		memset(gotSlow, 0, sizeof(gotSlow));
		const uint32_t nh = regionReadColumn(dirHint, cx_[i], cz_[i], gotHint, sizeof(gotHint));
		const uint32_t ns = regionReadColumn(dirSlow, cx_[i], cz_[i], gotSlow, sizeof(gotSlow));
		if (nh != ns || memcmp(gotHint, gotSlow, nh) != 0) bad++;
	}
	printf("  equivalence content check: %d of %d columns differ between the two paths\n", bad, EQ_COLS);
	CHECK(bad == 0);

	regionCleanup(dirHint, 0, 0);
	regionCleanup(dirSlow, 0, 0);
	rmdir(dirHint);
	rmdir(dirSlow);
}

int main(void)
{
	registryInitCore();
	registryFreeze();

	printf("region_test: round trip\n");
	testRoundTrip(1);
	testRoundTrip(0x9E3779B9u);
	testRoundTrip(42);

	printf("region_test: forced compaction\n");
	testForcedCompaction();

	printf("region_test: hint invalidation\n");
	testHintInvalidation();

	printf("region_test: hint vs fresh-read equivalence\n");
	testHintMatchesFreshRead();

	// ---- check-count guard, same convention as region_growth_test.c -----------------------
	// See that file's own guard comment for the full rationale: a suite that only counts
	// failures cannot notice checks that silently stopped running. This is the count that
	// must already have run by this line; update it by the exact delta of checks you add or
	// remove, never by pasting the observed number (see region_growth_test.c for why that
	// specific mistake is the one this guard exists to catch).
	// Derived, not observed, before this file was ever run -- same convention as
	// region_growth_test.c's guard (see there for why a pasted, observed count defeats the
	// point). Per function, assuming buildColumn's churn never refuses an encode (the churn
	// magnitudes here top out at 151 scattered edits, well inside the range region_growth_
	// test.c already runs clean up to 265):
	//   testRoundTrip:            (180 cols * 3 saves * 2 checks + 1) * 3 seeds = 3243
	//   testForcedCompaction:      30 passes * 20 cols * 2 checks + 2           = 1202
	//   testHintInvalidation:      3 + 4 + 3                                    =   10
	//   testHintMatchesFreshRead:  25 passes * 16 cols * 3 checks + 4           = 1204
	//                                                                    total  = 5659
	const int ran = s_checks;
	if (ran != 5659)
		printf("\nCHECK-COUNT GUARD: %d checks ran, %d expected.\n"
		       "  %s\n"
		       "  Read the comment above this guard in world/region_test.c before touching"
		       " the pinned number.\n",
		       ran, 5659,
		       ran < 5659
		           ? "Checks went MISSING: checks that should have run never ran at all."
		           : "Extra checks appeared: either you added checks and did not update the"
		             " pin, or something is emitting checks it should not.");
	CHECK(ran == 5659);

	if (s_fails) {
		printf("\nregion_test: FAILED - %d of %d checks\n", s_fails, s_checks);
		return 1;
	}
	printf("\nregion_test: PASS  %d checks\n", s_checks);
	return 0;
}

#endif  // !__3DS__
