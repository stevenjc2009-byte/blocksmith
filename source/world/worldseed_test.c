// Host tests for the per-world seed sidecar (world/worldseed.c, v1.8.3 Phase 1).
//
// WHAT THIS MODULE DECIDES, AND WHY IT NEEDS ITS OWN SUITE.
//
// Terrain is not stored — it is generated from a seed, and only edits are saved
// (world/region.h). Until v1.8.3 that seed was one compiled-in constant, BS_WORLD_SEED 1337,
// for every single-player world on every console. worldseed.c makes it per-world. That means
// the value this module returns for a directory is not a preference: it IS the landscape the
// player's base is standing on, and returning a different one for a world that already has
// buildings in it is indistinguishable from corruption. There is no way to notice it in a
// screenshot and no way to recover from it afterwards.
//
// Its own binary rather than a block inside world_test.c, and that is the same argument
// world/tick_test.c and world/water_test.c make: a suite of 5,000 checks that aborts under
// `set -e` takes this module's coverage down with it, and this module's answers are wanted most
// on exactly the days something else is mid-edit.
//
// WHAT IS COVERED HERE THAT NOTHING ELSE REACHES. Nothing else reaches any of it — worldseed.c
// is new in this commit and worldseed_test.c is its only caller besides main.c's genStart(),
// which cannot be linked into a host binary at all (see app/session_test.c for that problem
// stated one module over). Concretely:
//
//   1. THE TWO BRANCHES THAT DECIDE A WORLD'S IDENTITY, kept apart. An unstamped directory that
//      HAS a region file is a pre-v1.8.3 world and must come back 1337 forever; an unstamped
//      directory that has NOT is brand new and must come back a minted seed and have it written
//      down before the first save. Those two are one `if` apart in worldseed.c and swapping them
//      silently rewrites either every existing world or every new one.
//
//   2. THE WRITE THAT IS NOT OPTIONAL. A brand-new world generates from `mint`, saves its first
//      column, and from the NEXT boot the absent-sidecar branch would read that .bsr and answer
//      LEGACY — different terrain under a base that is already built, one boot later, with no
//      error anywhere. testResolveBrandNewWorldKeepsItsSeedAfterSaving walks exactly that
//      sequence rather than asserting the write happened.
//
//   3. THE REFUSALS AS REFUSALS. WSEED_MINT_FAILED and WSEED_STAMP_FAILED both have 1337 sitting
//      right there as an obvious fallback, and taking it would hand the player the one world
//      this rung exists to stop handing them, silently. Both are driven to a real return here,
//      the stamp failure through a real filesystem the write genuinely cannot reach.
//
//   4. THE ROUTING. This project has a recorded case of a status that was returned, tested, and
//      read by nobody: 82d4a1e added GENVER_STAMP_FAILED and main.c's `if` named two statuses by
//      hand, so the third fell through and the world loaded anyway. world/genrefuse.h is the fix
//      and testSeedRefusalRouting is its test — including that the three seed sentences are
//      distinct from the three generator sentences, which is the difference between the player
//      deleting the right file out of their world directory and the wrong one.
//
//   5. THE DRIFT the whole genVersionWorldHasRegionFile export exists to prevent.
//      testSeedAndVersionAgreeOnHistory asks genversion.c and worldseed.c the same question about
//      the same directory and requires the same answer. Two copies of that scan free to drift is
//      precisely how a world gets stamped LEGACY by one and minted a fresh seed by the other.
//
// Every expectation below is a hand-written literal. Nothing is derived from a value worldseed.c
// also computes — a check parameterised by the thing under test cannot detect that thing moving,
// which is this project's recorded case-3 trap.
//
// The __3DS__ guard around the whole file is load-bearing, not tidy: the console Makefile globs
// every .c under source/world, so without it this file's main() collides with source/main.c's.
#ifndef __3DS__

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// rmdir/getpid. Same include split, and for the same reason, as world/worldlist_test.c: MinGW
// supplies the non-underscore spellings from <direct.h>/<process.h>, and without this the host
// build dies on an implicit declaration under -Werror.
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "world/genrefuse.h"
#include "world/genversion.h"
#include "world/worldseed.h"

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

// How many CHECK() calls this suite makes on a healthy tree. A LITERAL on purpose.
//
// A suite that reports only "0 failed" cannot tell a check that PASSED from a check that never
// RAN, and this project has the measurement: a sabotage that shortened a production-constant-
// bounded loop in the net suite took its count from 326 to 318 and it reported a pass. Eight
// checks were deleted and nothing said so.
//
// This number must never be computed from a loop bound, a production constant, or anything else
// the code under test can also move. Adding or removing a check means editing this by hand in
// the same commit — the suite going red until you do is the review, not an accident.
// v1.8.3, the path-buffer refusal: testSeedPathTooLongIsRefused adds 39 checks in four blocks —
// 7 at the 150-character boundary that must still work, 9 one byte over it, 13 on the planted
// neighbouring file, and 10 on the two directories that used to collapse onto one sidecar.
// 186 + 39 = 225. Counted off the source by hand FIRST and confirmed against the run second,
// in that order and not the other one.
// Then +1, to 226, for the snprintf-fits check in the neighbour/155 block. That one is not a
// claim about worldseed.c at all: `dir` is a char[256] and gcc cannot prove "/see" fits it, so
// the file did not compile at all under -Werror=format-truncation until the return was used,
// and using it is worth a check because it separates "the code under test is wrong" from "the
// harness planted at a name it did not mean to". Found by this guard rather than by reading:
// it printed "1 check(s) were ADDED - expected 225, ran 226", which is what it is for.
#define WORLDSEED_TEST_EXPECTED_CHECKS 226

// Deliberately NOT routed through CHECK(): it must not perturb the number it is testing, so it
// bumps g_fails only. Copied in shape from world/tick_test.c's checkCountPin.
static void checkCountPin(void)
{
	if (g_checks == WORLDSEED_TEST_EXPECTED_CHECKS)
		return;

	g_fails++;
	if (g_checks < WORLDSEED_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran: a loop bound shrank, an early\n"
		       "         return or a continue fired, or a check was deleted. The checks that\n"
		       "         did run passing tells you nothing about the ones that did not.\n"
		       "         Find them. Do NOT re-pin WORLDSEED_TEST_EXPECTED_CHECKS to go green.\n",
		       WORLDSEED_TEST_EXPECTED_CHECKS - g_checks,
		       WORLDSEED_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If you added them on purpose, set WORLDSEED_TEST_EXPECTED_CHECKS in\n"
		       "         source/world/worldseed_test.c to %d. If you did not, something is\n"
		       "         running checks more times than it should.\n",
		       g_checks - WORLDSEED_TEST_EXPECTED_CHECKS,
		       WORLDSEED_TEST_EXPECTED_CHECKS, g_checks, g_checks);
}

// ── Filesystem fixtures ──────────────────────────────────────────────────────────────────

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Copied from
// world/worldlist_test.c's testMkdir, solving the identical host/console split.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

// Deletes `path` and everything under it. remove() takes files and empty directories on both
// platforms and refuses only a non-empty directory, so recursing on failure and letting the tail
// rmdir finish covers every case. Best-effort — a leftover file is untidy, not a failure.
static void testRmTree(const char* path)
{
	DIR* d = opendir(path);
	if (!d) return;

	for (const struct dirent* e = readdir(d); e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

		char child[512];
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (remove(child) != 0) testRmTree(child);
	}

	closedir(d);
	rmdir(path);
}

// The sandbox root, per process. Two copies of this binary running at once — which is what
// tools/run_host_tests.sh does when two sessions run it together — would otherwise fight over
// the same world directories, and world/worldlist_test.c measured what that looks like: 7 of 8
// concurrent runs failing at four different assertions, all of them passing on a re-run.
static char g_root[128];

static void seedTestRoot(void)
{
	testMkdir("build-host");
	snprintf(g_root, sizeof g_root, "build-host/worldseedtest-%ld", (long)getpid());
	testRmTree(g_root);
	testMkdir(g_root);
}

// A fresh, empty world directory named `name` under the sandbox. Removed first, because a
// leftover from a failed run would silently satisfy the brand-new tests for the wrong reason.
static void seedTestDir(char* out, size_t cap, const char* name)
{
	snprintf(out, cap, "%s/%s", g_root, name);
	testRmTree(out);
	testMkdir(out);
}

static void seedPath(char* out, size_t cap, const char* dir)
{
	snprintf(out, cap, "%s/%s", dir, WORLD_SEED_FILE);
}

// Size of the sidecar in `dir`, or -1 if there is not one. Used both to prove a write landed and
// to prove one did NOT — the second is the harder claim and the one the refusal tests rest on.
static long seedSize(const char* dir)
{
	char p[256];
	seedPath(p, sizeof p, dir);
	FILE* f = fopen(p, "rb");
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	const long n = ftell(f);
	fclose(f);
	return n;
}

// Overwrite `len` bytes at `off` in the sidecar, in place. The corruption arms below all go
// through this rather than writing a whole hand-built file, so every one of them starts from a
// record worldSeedWrite() genuinely produced.
static void seedPoke(const char* dir, long off, const uint8_t* bytes, size_t len)
{
	char p[256];
	seedPath(p, sizeof p, dir);
	FILE* f = fopen(p, "r+b");
	if (!f) return;
	if (fseek(f, off, SEEK_SET) == 0) fwrite(bytes, 1, len, f);
	fclose(f);
}

// How many entries `dir` holds, ignoring . and .., or -1 if it cannot be listed. This is the
// check that "nothing was written" is made of in the path-length test below, and it has to count
// entries rather than ask for seed.bin specifically: the whole defect there is a write landing
// under a DIFFERENT name, which a seedSize() of -1 cannot see.
static int dirEntryCount(const char* dir)
{
	DIR* d = opendir(dir);
	if (!d) return -1;

	int n = 0;
	for (const struct dirent* e = readdir(d); e; e = readdir(d)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		n++;
	}
	closedir(d);
	return n;
}

static bool plantFile(const char* path, const uint8_t* bytes, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(bytes, 1, n, f) == n;
	return (fclose(f) == 0) && ok;
}

// Bytes of `path` into `buf`, or -1 if it will not open. Returns how many were read.
static long readWholeFile(const char* path, uint8_t* buf, size_t cap)
{
	FILE* f = fopen(path, "rb");
	if (!f) return -1;
	const size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return (long)n;
}

// A REAL directory under the sandbox whose full path is exactly `want` characters long, padded
// with a single 'w' component ending in `tag` so two cases of the same length are two different
// directories. False if the sandbox root is already too long for that, which is a broken fixture
// rather than a failing assertion and is checked as one.
static bool seedTestPaddedDir(char* out, size_t cap, size_t want, char tag)
{
	const size_t rootlen = strlen(g_root);
	if (want < rootlen + 2 || want >= cap) return false;

	memcpy(out, g_root, rootlen);
	out[rootlen] = '/';
	memset(out + rootlen + 1, 'w', want - rootlen - 1);
	out[want - 1] = tag;
	out[want]     = '\0';

	testRmTree(out);
	testMkdir(out);
	return strlen(out) == want && dirEntryCount(out) == 0;
}

// A region file, i.e. the evidence that this world has been played. region.c names them
// r.<rx>.<rz>.bsr and the extension alone is the whole test genversion.c applies.
static bool makeRegionFile(const char* dir, const char* name)
{
	char p[320];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE* f = fopen(p, "wb");
	if (!f) return false;
	fputc(0, f);
	return fclose(f) == 0;
}

// ── The control ──────────────────────────────────────────────────────────────────────────
//
// Asserted first and expected GREEN in every sabotage arm recorded in this file's stanza in
// tools/run_host_tests.sh. It touches only the no-directory guard at the top of worldSeedRead
// and worldSeedResolve — the earliest line in either function — so a break anywhere in the mint,
// the CRC, the sidecar format or the two history branches must leave it standing. If it ever
// goes red alongside everything else, the arm is too broad to have localised anything.
//
// It is not vacuous: pointed at WSEED_OK instead of WSEED_NO_WORLD_DIR on an otherwise healthy
// tree it reads back red, and that run is quoted in the runner stanza.
static void testControlNoWorldDir(void)
{
	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(NULL, true, 0x11111111u, &v) == WSEED_NO_WORLD_DIR,
	      "control: resolving a NULL world directory is WSEED_NO_WORLD_DIR");
	CHECK(v == WORLD_SEED_LEGACY,
	      "control: and it leaves *out at the legacy seed rather than at the caller's value");

	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve("", true, 0x11111111u, &v) == WSEED_NO_WORLD_DIR,
	      "control: an empty world directory is WSEED_NO_WORLD_DIR too");
	CHECK(v == WORLD_SEED_LEGACY,
	      "control: and it also leaves *out at the legacy seed");

	v = 0xDEADBEEFu;
	CHECK(worldSeedRead(NULL, &v) == WSEED_NO_WORLD_DIR,
	      "control: reading a NULL world directory is WSEED_NO_WORLD_DIR");
	CHECK(v == WORLD_SEED_LEGACY,
	      "control: and reading leaves *out at the legacy seed as well");

	// NULL out on every entry point. main.c never passes one, but genversion.c's sibling checks
	// exist and a crash in a refusal path is the worst possible place for one.
	CHECK(worldSeedRead(NULL, NULL) == WSEED_NO_WORLD_DIR,
	      "control: worldSeedRead tolerates a NULL out pointer");
	CHECK(worldSeedResolve(NULL, false, 0u, NULL) == WSEED_NO_WORLD_DIR,
	      "control: worldSeedResolve tolerates a NULL out pointer");
}

// ── The constants, as naked literals ─────────────────────────────────────────────────────
//
// WORLD_SEED_LEGACY is the seed every world made before v1.8.3 was generated from, so it is a
// fact about worlds that already exist on players' cards rather than a tuning knob. Nothing else
// in the tree pins it: main.c's BS_WORLD_SEED is a `#ifndef` default and is overridable with -D
// for instrumented builds, which is exactly why worldseed.h writes the number down a second time.
// Every assertion elsewhere in this file is written in terms of the macro, so this is the only
// line that can notice the macro itself changing.
static void testSeedConstantsPinned(void)
{
	CHECK(WORLD_SEED_LEGACY == 1337u,
	      "WORLD_SEED_LEGACY is 1337 - the seed every pre-v1.8.3 world was generated from, and "
	      "therefore the seed every one of them must keep");
	CHECK(WORLD_SEED_BYTES == 12,
	      "WORLD_SEED_BYTES is 12: 4 magic + 4 seed + 4 CRC, fixed, so a short read is itself a "
	      "detectable fault");
	CHECK(strcmp(WORLD_SEED_FILE, "seed.bin") == 0,
	      "the sidecar is named seed.bin - a rename orphans every world already stamped");
	CHECK(strcmp(WORLD_SEED_FILE, GEN_VERSION_FILE) != 0,
	      "and it is not the generator stamp's name, so the two sidecars cannot overwrite each "
	      "other in the same directory");
}

// ── Minting ──────────────────────────────────────────────────────────────────────────────

// No clock of any kind. Refused rather than fabricated: a counter on its own restarts at the
// same place every boot, so the first world made after every power-on would share a seed across
// every console in existence — BS_WORLD_SEED again, with extra steps and no announcement.
static void testMintWithNoClockRefuses(void)
{
	uint32_t v = 0x5A5A5A5Au;
	CHECK(!worldSeedMintFrom(false, 0u, false, 0u, &v),
	      "a mint with neither a wall clock nor a tick counter is refused");
	CHECK(v == 0x5A5A5A5Au,
	      "and the refused mint does not write through the out pointer");

	// The readings are still passed, so this cannot pass merely because both values are zero.
	v = 0x5A5A5A5Au;
	CHECK(!worldSeedMintFrom(false, 1724688000u, false, 987654u, &v),
	      "it is the two `have` flags that decide, not whether the readings happen to be zero");
	CHECK(v == 0x5A5A5A5Au,
	      "and that refusal does not write through the out pointer either");

	CHECK(!worldSeedMintFrom(true, 1u, true, 1u, NULL),
	      "a NULL out pointer is refused before anything is minted");
}

// Either source alone is enough. Both are checked because the console reaches this function
// through two independent failure modes — a dead or unset RTC makes time() answer -1, and it is
// clock() that is missing on a host that never started a process clock.
static void testMintWithEitherClockAlone(void)
{
	uint32_t v = 0u;
	CHECK(worldSeedMintFrom(true, 1724688000u, false, 0u, &v),
	      "a wall clock on its own is enough to mint");
	uint32_t w = 0u;
	CHECK(worldSeedMintFrom(false, 0u, true, 4242u, &w),
	      "a tick counter on its own is enough to mint");
	CHECK(v != w,
	      "and the two do not collapse onto the same seed");

	uint32_t both = 0u;
	CHECK(worldSeedMintFrom(true, 1724688000u, true, 4242u, &both),
	      "and both together mint as well");
	CHECK(both != v && both != w,
	      "folding in the second reading changes the answer rather than being ignored");
}

// THE LOAD-BEARING PROPERTY, and the reason worldSeedMintFrom takes its readings as arguments at
// all. The wall clock is whole seconds on both platforms, so two "New World" presses from the
// title screen can genuinely land in the same second on a console with no sub-second clock.
// Without the per-process counter, that is two worlds with one landscape — the exact defect this
// whole rung exists to remove, reintroduced at the only moment it is invisible.
//
// 256 mints from IDENTICAL readings, every pair compared. Not a sample: rngMix is a bijection
// and multiplication by an odd constant is a bijection, so distinct counter values give distinct
// seeds outright and this is a guarantee rather than a probability.
static void testMintFromIdenticalReadingsIsAlwaysDistinct(void)
{
	uint32_t a = 0u, b = 0u;
	CHECK(worldSeedMintFrom(true, 1724688000u, true, 500u, &a), "first mint of the pair succeeds");
	CHECK(worldSeedMintFrom(true, 1724688000u, true, 500u, &b), "second mint of the pair succeeds");
	CHECK(a != b,
	      "two mints from byte-identical readings return DIFFERENT seeds - two New Worlds in the "
	      "same second must not share a landscape");

	uint32_t seen[256];
	for (int i = 0; i < 256; i++) {
		seen[i] = 0u;
		(void)worldSeedMintFrom(true, 1724688000u, true, 500u, &seen[i]);
	}

	int collisions = 0;
	for (int i = 0; i < 256; i++)
		for (int j = i + 1; j < 256; j++)
			if (seen[i] == seen[j]) collisions++;

	CHECK(collisions == 0,
	      "256 consecutive mints from one frozen reading produce 256 distinct seeds, every pair "
	      "compared");

	// Falsifiability: the sweep must actually have run 256 mints rather than 256 memsets. A zero
	// seed is legal, so this cannot be "none of them is zero" - it is that they are not all the
	// same value, which a loop that never called the mint would fail.
	int distinct_from_first = 0;
	for (int i = 1; i < 256; i++)
		if (seen[i] != seen[0]) distinct_from_first++;
	CHECK(distinct_from_first == 255,
	      "falsifiability: all 255 of the remaining sweep entries differ from the first, so the "
	      "sweep really minted 256 times");
}

// The platform wrapper. Not a tautology: it is the one line that decides which of the two
// documented failure values counts as "no clock", and getting either comparison backwards turns
// a working console into WSEED_MINT_FAILED on every new world.
static void testMintFromThisPlatformsClocks(void)
{
	uint32_t a = 0u, b = 0u;
	CHECK(worldSeedMint(&a),
	      "worldSeedMint succeeds on this host - time() and clock() both answer");
	CHECK(worldSeedMint(&b),
	      "and it succeeds a second time in the same process");
	CHECK(a != b,
	      "two back-to-back real mints inside one second still differ");
	CHECK(!worldSeedMint(NULL),
	      "worldSeedMint refuses a NULL out pointer");
}

// ── The sidecar on disk ──────────────────────────────────────────────────────────────────

static void testSidecarRoundTrip(void)
{
	char dir[256];
	seedTestDir(dir, sizeof dir, "rt");

	// Reading a directory that has no sidecar is OK/LEGACY and, crucially, creates nothing.
	// "No sidecar" is not a fault — it is what every pre-v1.8.3 world looks like — and deciding
	// whether legacy is the right answer for THIS directory is worldSeedResolve's job.
	uint32_t v = 0u;
	CHECK(worldSeedRead(dir, &v) == WSEED_OK,
	      "an absent sidecar reads as WSEED_OK rather than as a fault");
	CHECK(v == WORLD_SEED_LEGACY,
	      "and reports the legacy seed");
	CHECK(seedSize(dir) == -1,
	      "and reading does not create one");

	// Every corner of the 32-bit space, because a seed carries no capability and so there is no
	// equivalent of genversion.h's GENVER_TOO_NEW here: every value must round-trip.
	static const uint32_t wanted[] = {
		0u, 1u, WORLD_SEED_LEGACY, 0x0000FF00u, 0x7FFFFFFFu, 0x80000000u, 0xC0FFEEu, 0xFFFFFFFFu
	};
	for (size_t i = 0; i < sizeof wanted / sizeof wanted[0]; i++) {
		CHECK(worldSeedWrite(dir, wanted[i]), "the sidecar write succeeds");
		CHECK(seedSize(dir) == WORLD_SEED_BYTES, "and produces exactly 12 bytes");
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_OK, "and reads back OK");
		CHECK(v == wanted[i], "with the seed it was given, byte for byte");
		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(dir, true, 0x13579BDFu, &v) == WSEED_OK,
		      "and resolving a directory that already has a sidecar reads it");
		CHECK(v == wanted[i], "rather than minting over the top of it");
	}

	// A sidecar is a per-world fact, so writing one must be idempotent in the byte sense: the
	// same seed twice is the same twelve bytes, not a file that grows.
	CHECK(worldSeedWrite(dir, 0xC0FFEEu), "rewriting the sidecar with the same seed succeeds");
	CHECK(seedSize(dir) == WORLD_SEED_BYTES, "and it is still exactly 12 bytes, not appended to");

	CHECK(!worldSeedWrite(NULL, 1u), "worldSeedWrite refuses a NULL directory");
	CHECK(!worldSeedWrite("", 1u), "worldSeedWrite refuses an empty directory");

	testRmTree(dir);
}

// Five ways a sidecar can be wrong, each starting from a record worldSeedWrite really produced.
// Every one of them must REFUSE, and none may report the stored value — falling back to a
// different seed than the one that shaped this world is the whole harm the file exists to stop,
// and doing it while reporting success is the worst version of it.
//
// The stored seed is deliberately not WORLD_SEED_LEGACY, so "*out was left at legacy" is a real
// observation about the refusal rather than a coincidence.
static void testSidecarDamagedFiveWays(void)
{
	char dir[256];
	char p[256];
	seedTestDir(dir, sizeof dir, "damaged");
	seedPath(p, sizeof p, dir);

	const uint32_t stored = 0x0BADF00Du;
	uint32_t v;

	{
		// Bad magic: something else wrote a file with this name.
		CHECK(worldSeedWrite(dir, stored), "damaged/magic: baseline sidecar written");
		const uint8_t bad[1] = {'X'};
		seedPoke(dir, 0, bad, 1);
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED, "bad magic is refused");
		CHECK(v == WORLD_SEED_LEGACY, "and does not report the stored seed");
		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(dir, true, 0x13579BDFu, &v) == WSEED_DAMAGED,
		      "and resolve refuses it too rather than minting over it");
		CHECK(v == WORLD_SEED_LEGACY, "and resolve does not report the stored seed either");
		CHECK(seedSize(dir) == WORLD_SEED_BYTES,
		      "and a refused resolve leaves the damaged file exactly as it found it");
	}
	{
		// Good magic, flipped payload: only the CRC can catch this one, and it is the only
		// corruption shape that produces a *plausible* seed.
		CHECK(worldSeedWrite(dir, stored), "damaged/payload: baseline sidecar written");
		const uint8_t flip[1] = {0x7Fu};
		seedPoke(dir, 4, flip, 1);
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED,
		      "a flipped seed byte under a good magic is caught by the CRC");
		CHECK(v == WORLD_SEED_LEGACY, "and the plausible-looking wrong seed is not reported");
	}
	{
		// Good record, corrupted CRC field.
		CHECK(worldSeedWrite(dir, stored), "damaged/crc: baseline sidecar written");
		const uint8_t junk[4] = {0xA5u, 0xA5u, 0xA5u, 0xA5u};
		seedPoke(dir, 8, junk, 4);
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED, "a corrupted CRC field is refused");
		CHECK(v == WORLD_SEED_LEGACY, "and the intact seed beside it is still not reported");
	}
	{
		// Truncated: a torn write, which is the failure the fixed record length exists to make
		// detectable rather than parseable.
		CHECK(worldSeedWrite(dir, stored), "damaged/short: baseline sidecar written");
		uint8_t head[WORLD_SEED_BYTES];
		FILE* f = fopen(p, "rb");
		CHECK(f != NULL, "the baseline sidecar reopens for reading");
		if (f) {
			CHECK(fread(head, 1, sizeof head, f) == sizeof head, "and holds all 12 bytes");
			fclose(f);
		}
		f = fopen(p, "wb");
		CHECK(f != NULL, "and reopens for a truncating write");
		if (f) { fwrite(head, 1, 6, f); fclose(f); }
		CHECK(seedSize(dir) == 6, "leaving a 6-byte torn record");
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED, "a torn write is refused");
		CHECK(v == WORLD_SEED_LEGACY, "and reports no seed");
	}
	{
		// Longer than the record. Nothing this build writes produces it, so something else wrote
		// here and the first twelve bytes cannot be trusted just because they parse.
		CHECK(worldSeedWrite(dir, stored), "damaged/long: baseline sidecar written");
		FILE* f = fopen(p, "ab");
		CHECK(f != NULL, "the sidecar reopens for appending");
		if (f) { fputc(0x00, f); fclose(f); }
		CHECK(seedSize(dir) == WORLD_SEED_BYTES + 1, "leaving a 13-byte file");
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED,
		      "a file longer than the record is as wrong as a short one");
		CHECK(v == WORLD_SEED_LEGACY, "and reports no seed");
	}

	testRmTree(dir);
}

// ── The path buffer ──────────────────────────────────────────────────────────────────────
//
// worldseed.c builds "<world_dir>/seed.bin" into a fixed `char path[160]`. Until this test the
// truncating branch was UNEXECUTED by the suite, not merely unasserted: every world_dir above is
// a short sandbox path. It was not hypothetical either — it bit a live session host-testing from
// a deep WSL path, and it was measured on the unfixed module before the fix went in:
//
//   dirlen  write  created-file  read-status  seed
//      150  true   seed.bin      OK           0xFEEDFACE     <- the last length that works
//      151  true   seed.bi       OK           0xFEEDFACE
//      155  true   see           OK           0xFEEDFACE
//
// Every one reported SUCCESS, and that is the shape of the bug: the writer and the reader
// truncate identically, so they agree with each other about the wrong filename and a round trip
// reads back perfectly. Nothing in a PASS line can see it. Past 159 the directory half is cut as
// well, and two world directories sharing their first 159 bytes become ONE world — writing
// 0xAAAAAAAA into A and then resolving B, which had never been stamped, answered WSEED_OK with
// 0xAAAAAAAA and threw B's own mint away. That is the landscape moving under a base the player
// has already built, arriving as a success code, which is the single harm this module exists to
// prevent.
//
// THE NUMBERS BELOW ARE HAND-WRITTEN LITERALS derived from `char path[160]` and "seed.bin":
// 160 - 1 (NUL) - 1 ('/') - 8 ("seed.bin") = 150 is the longest world_dir that fits, and 159 is
// the longest path that can be written. They are deliberately NOT derived from anything
// worldseed.c exports — a check parameterised by the thing under test cannot detect that thing
// moving. If that buffer is ever resized these go red, and that is the review, not an accident.
//
// Note what is NOT asserted here: that the buffer is 160. Widening it is not the fix and does not
// need to be forbidden; refusing what does not fit is the fix, and refusing is what every check
// below is about.
static void testSeedPathTooLongIsRefused(void)
{
	char dir[256];
	uint32_t v;

	// ── 150: the last world_dir that fits. Everything must still work. ───────────────────
	//
	// The near half of the boundary, and the reason it is here is that a "fix" which refuses
	// one byte too early is a fix that stops opening perfectly good worlds. An off-by-one in
	// either direction has to be visible.
	{
		const uint32_t kFits = 0x15150150u;
		CHECK(seedTestPaddedDir(dir, sizeof dir, 150, 'a'),
		      "boundary: a real 150-character world directory exists and starts empty");
		CHECK(worldSeedWrite(dir, kFits),
		      "boundary/150: the sidecar write succeeds - 150 + '/' + 'seed.bin' + NUL is exactly "
		      "160 and fits");
		CHECK(seedSize(dir) == WORLD_SEED_BYTES,
		      "boundary/150: and lands in seed.bin at the full 12 bytes");
		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_OK, "boundary/150: and reads back OK");
		CHECK(v == kFits, "boundary/150: with the seed it was given");
		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(dir, true, 0x77770000u, &v) == WSEED_OK,
		      "boundary/150: and resolve reads the sidecar rather than minting");
		CHECK(v == kFits, "boundary/150: returning the stored seed");
		testRmTree(dir);
	}

	// ── 151: one byte over. Refused, and NOTHING is created under any name. ──────────────
	//
	// On the unfixed module this wrote a 12-byte record into a file called "seed.bi" and
	// answered true. seedSize() cannot see that — it asks for seed.bin — so the "nothing was
	// written" claim is made with a directory listing instead.
	{
		CHECK(seedTestPaddedDir(dir, sizeof dir, 151, 'b'),
		      "over/151: a real 151-character world directory exists and starts empty");
		CHECK(!worldSeedWrite(dir, 0x0BADBAD0u),
		      "over/151: the write is REFUSED rather than performed into a truncated name");
		CHECK(dirEntryCount(dir) == 0,
		      "over/151: and the directory is still EMPTY - not a file called seed.bi, which is "
		      "what a truncating write leaves behind and what seedSize() cannot see");
		CHECK(seedSize(dir) == -1, "over/151: there is no seed.bin either");

		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED,
		      "over/151: the read REFUSES rather than reporting WSEED_OK - 'the path did not fit' "
		      "is not 'this world has no sidecar'");
		CHECK(v == WORLD_SEED_LEGACY,
		      "over/151: and leaves *out at the legacy seed, which the caller must not use");

		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(dir, true, 0x5EED5EEDu, &v) == WSEED_DAMAGED,
		      "over/151: resolve refuses too, rather than minting for a world it cannot inspect");
		CHECK(v != 0x5EED5EEDu, "over/151: and does not report the mint it was offered");
		CHECK(dirEntryCount(dir) == 0,
		      "over/151: and the refused resolve created nothing either");
		testRmTree(dir);
	}

	// ── 155: the truncated name is a REAL file, and it is somebody else's. ───────────────
	//
	// "<dir>/seed.bin" at dirlen 155 is 164 characters, truncated to 159: "<dir>/see". A file of
	// that name is planted with a GENUINE record — produced by worldSeedWrite in a short
	// directory, not hand-built — so that the unfixed behaviour is not "an open that fails" but
	// "an open that succeeds and answers about the wrong file". That is the whole point of
	// refusing instead of truncating, and it is the only case here that can show it.
	{
		const uint32_t kPlanted = 0xFEEDFACEu;
		uint8_t record[WORLD_SEED_BYTES];

		char src[256];
		seedTestDir(src, sizeof src, "neighbour-src");
		CHECK(worldSeedWrite(src, kPlanted), "neighbour: a genuine 12-byte record is produced");
		{
			char sp[256];
			seedPath(sp, sizeof sp, src);
			CHECK(readWholeFile(sp, record, sizeof record) == WORLD_SEED_BYTES,
			      "neighbour: and its twelve bytes are read back out");
		}
		testRmTree(src);

		CHECK(seedTestPaddedDir(dir, sizeof dir, 155, 'c'),
		      "neighbour/155: a real 155-character world directory exists and starts empty");

		// snprintf's return is USED rather than discarded, which is not decoration here: `dir`
		// is a char[256] and gcc cannot prove the "/see" suffix fits it, so discarding the
		// return is -Werror=format-truncation at -Wall and this file does not compile. That is
		// the same warning, asking for the same thing, as the one that motivates the fix under
		// test -- so the test buffer is held to the contract the production buffer now is.
		char victim[256];
		const int victim_n = snprintf(victim, sizeof victim, "%s/see", dir);
		CHECK(victim_n > 0 && (size_t)victim_n < sizeof victim,
		      "neighbour/155: the truncated name this test plants at fits the test's own buffer, "
		      "so a failure below is the code under test and not the harness");
		CHECK(plantFile(victim, record, sizeof record),
		      "neighbour/155: the file the truncated path names, '<dir>/see', is planted with it");

		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(dir, &v) == WSEED_DAMAGED,
		      "neighbour/155: the read REFUSES - the neighbour opens perfectly well and parses "
		      "perfectly well, and it is still not this world's sidecar");
		CHECK(v != kPlanted,
		      "neighbour/155: and specifically does NOT report the neighbour's seed as this "
		      "world's, which is the silent wrong answer the refusal exists to stop");
		CHECK(v == WORLD_SEED_LEGACY, "neighbour/155: *out is left at the legacy seed");

		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(dir, true, 0x24242424u, &v) == WSEED_DAMAGED,
		      "neighbour/155: resolve refuses on the same evidence");
		CHECK(v != kPlanted, "neighbour/155: and does not adopt the neighbour's seed either");

		CHECK(!worldSeedWrite(dir, 0x31313131u),
		      "neighbour/155: and a write is refused rather than landing on top of it");

		uint8_t after[WORLD_SEED_BYTES + 4];
		const long got = readWholeFile(victim, after, sizeof after);
		CHECK(got == WORLD_SEED_BYTES,
		      "neighbour/155: the neighbouring file is still exactly twelve bytes");
		CHECK(got == WORLD_SEED_BYTES && memcmp(after, record, WORLD_SEED_BYTES) == 0,
		      "neighbour/155: and byte-for-byte what was planted - a refused write CLOBBERED it "
		      "before this fix");
		CHECK(dirEntryCount(dir) == 1,
		      "neighbour/155: and nothing new appeared beside it");
		testRmTree(dir);
	}

	// ── Two worlds, one sidecar. The catastrophic shape. ─────────────────────────────────
	//
	// Both directories are 170 characters and differ only at byte 159 — past the 159 the buffer
	// can hold, so the truncated path is IDENTICAL for the two of them. On the unfixed module
	// that made them the same world: A's seed was written and B, which had never been stamped,
	// read it straight back as its own and discarded its mint. Different landscape, no error.
	{
		const uint32_t kA = 0xAAAAAAAAu;
		char a[256], b[256];
		const size_t rootlen = strlen(g_root);
		const size_t want    = 170;

		CHECK(rootlen + 1 < 159 && 159 < want,
		      "collision: the fixture's differing byte falls inside the padding, past the 159 "
		      "characters the path buffer can hold");

		memcpy(a, g_root, rootlen);
		a[rootlen] = '/';
		memset(a + rootlen + 1, 'w', want - rootlen - 1);
		a[want] = '\0';
		memcpy(b, a, want + 1);
		a[159] = 'A';
		b[159] = 'B';
		testRmTree(a); testMkdir(a);
		testRmTree(b); testMkdir(b);

		CHECK(strlen(a) == want && strlen(b) == want && strcmp(a, b) != 0,
		      "collision: two DIFFERENT 170-character world directories");
		CHECK(memcmp(a, b, 159) == 0,
		      "collision: identical for the first 159 bytes, so a truncated path cannot tell them "
		      "apart");
		CHECK(dirEntryCount(a) == 0 && dirEntryCount(b) == 0,
		      "collision: both exist on disk and both start empty");

		CHECK(!worldSeedWrite(a, kA),
		      "collision: stamping A is refused rather than writing to the shared truncated name");

		v = 0xDEADBEEFu;
		CHECK(worldSeedRead(b, &v) == WSEED_DAMAGED,
		      "collision: reading B refuses");
		CHECK(v != kA,
		      "collision: and B does NOT come back holding A's seed - two worlds sharing one "
		      "sidecar is the same landscape under two different bases");

		v = 0xDEADBEEFu;
		CHECK(worldSeedResolve(b, true, 0xBBBBBBBBu, &v) == WSEED_DAMAGED,
		      "collision: resolving B refuses");
		CHECK(v != kA, "collision: and still does not adopt A's seed");
		CHECK(seedSize(b) == -1, "collision: and B was given no sidecar of its own");

		testRmTree(a);
		testRmTree(b);
	}
}

// ── The two history branches ─────────────────────────────────────────────────────────────

// A brand-new world: no sidecar, and nothing has ever been saved here. It gets the caller's
// minted seed, and the write is not optional — see the next test for why.
static void testResolveBrandNewWorldMints(void)
{
	char dir[256];
	seedTestDir(dir, sizeof dir, "new");

	// Preconditions, so a pass cannot be a pass for the wrong reason: there is no sidecar, and
	// there is no region file, so neither of the other two branches can be what answers.
	CHECK(seedSize(dir) == -1, "brand new: the world directory has no sidecar");
	CHECK(!genVersionWorldHasRegionFile(dir), "brand new: and it has never been saved");

	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0xABCDEF01u, &v) == WSEED_OK,
	      "a brand-new world resolves OK");
	CHECK(v == 0xABCDEF01u,
	      "with the caller's minted seed, not with the legacy constant");
	CHECK(seedSize(dir) == WORLD_SEED_BYTES,
	      "and the sidecar is written before anything else can happen");

	// A second resolve reads the sidecar rather than re-deriving. A DIFFERENT mint is offered so
	// that "it read the file" and "it happened to mint the same value" cannot be confused.
	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x02020202u, &v) == WSEED_OK,
	      "resolving it again is OK");
	CHECK(v == 0xABCDEF01u,
	      "and returns the first seed, ignoring the second mint entirely");

	// And the file really holds it, rather than the answer coming from somewhere in memory.
	v = 0xDEADBEEFu;
	CHECK(worldSeedRead(dir, &v) == WSEED_OK, "and a plain read of the sidecar agrees");
	CHECK(v == 0xABCDEF01u, "with the same seed");

	testRmTree(dir);
}

// THE LINE THE WHOLE SIDECAR TURNS ON, walked as a sequence rather than asserted as a fact.
//
// A brand-new world generates from `mint`. The player's first saved column creates a .bsr. From
// the NEXT boot, the absent-sidecar branch would see that .bsr, conclude the world predates
// v1.8.3, and answer 1337 — a different landscape under a base that is already built, one boot
// later, with no error and nothing to recover from. The sidecar written at creation is the only
// thing standing between those two paragraphs.
static void testResolveBrandNewWorldKeepsItsSeedAfterSaving(void)
{
	char dir[256];
	seedTestDir(dir, sizeof dir, "new-then-saved");

	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x5EED0001u, &v) == WSEED_OK, "boot 1: the world is made");
	CHECK(v == 0x5EED0001u, "boot 1: and generates from the minted seed");

	CHECK(makeRegionFile(dir, "r.0.0.bsr"), "the player plays, and a region file is saved");
	CHECK(genVersionWorldHasRegionFile(dir),
	      "so the directory now looks exactly like a pre-v1.8.3 world to the history scan");

	// The whole point. Without the sidecar this line returns WORLD_SEED_LEGACY.
	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x77777777u, &v) == WSEED_OK, "boot 2: the world reopens");
	CHECK(v == 0x5EED0001u,
	      "boot 2: WITH THE SAME SEED - the sidecar outranks the region file, so the terrain "
	      "under the player's base does not move");
	CHECK(v != WORLD_SEED_LEGACY,
	      "boot 2: and specifically has not fallen back to the legacy constant");

	testRmTree(dir);
}

// An unstamped directory that HAS been saved: every single-player world in existence before
// v1.8.3. It gets WORLD_SEED_LEGACY, and this is the case that keeps every existing save
// byte-identical. A minted seed here rewrites the landscape under every base every player has
// ever built.
static void testResolvePlayedWorldStaysLegacy(void)
{
	char dir[256];
	seedTestDir(dir, sizeof dir, "old");

	CHECK(makeRegionFile(dir, "r.-1.2.bsr"), "an old world's region file is in place");
	CHECK(seedSize(dir) == -1, "and it has no sidecar, because sidecars did not exist yet");

	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x33333333u, &v) == WSEED_OK,
	      "a played world with no sidecar resolves OK");
	CHECK(v == WORLD_SEED_LEGACY,
	      "with the legacy seed - the offered mint is NOT used, which is what keeps every "
	      "existing save byte-identical");
	CHECK(seedSize(dir) == WORLD_SEED_BYTES,
	      "and the derivation is written down, best effort, so it stops being derived");

	// And the written value is the legacy seed, not the mint that was sitting in the arguments.
	v = 0xDEADBEEFu;
	CHECK(worldSeedRead(dir, &v) == WSEED_OK, "the sidecar it wrote reads back OK");
	CHECK(v == WORLD_SEED_LEGACY, "and holds 1337, not the offered mint");

	// A world with no clock is still an old world, and must still open. The mint is only
	// consulted on the brand-new branch, so mint_ok being false must not reach a refusal here —
	// a dead RTC must not lock a player out of a world they already have.
	seedTestDir(dir, sizeof dir, "old-noclock");
	CHECK(makeRegionFile(dir, "r.0.-3.bsr"), "a second old world's region file is in place");
	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, false, 0u, &v) == WSEED_OK,
	      "an old world opens even when no seed could be minted - the mint is not consulted on "
	      "this branch");
	CHECK(v == WORLD_SEED_LEGACY, "and still gets the legacy seed");

	testRmTree(dir);
}

// ── The two refusals ─────────────────────────────────────────────────────────────────────

// A brand-new world on a console whose clocks all refused. 1337 is sitting right there and
// taking it would hand the player the one landscape this rung exists to replace, without saying
// so. Nothing has ever been saved in this directory, so refusing costs a world that does not
// exist yet.
static void testResolveMintFailedRefuses(void)
{
	char dir[256];
	seedTestDir(dir, sizeof dir, "mintfail");

	CHECK(seedSize(dir) == -1, "mint-failed: no sidecar to start with");
	CHECK(!genVersionWorldHasRegionFile(dir), "mint-failed: and the world has never been saved");

	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, false, 0x44444444u, &v) == WSEED_MINT_FAILED,
	      "a brand-new world with no mintable seed is REFUSED, not defaulted to 1337");
	CHECK(v == WORLD_SEED_LEGACY,
	      "and the value left behind is the header's documented one, which the caller must not "
	      "use - the status is the answer");
	CHECK(seedSize(dir) == -1,
	      "and nothing was written, so the next boot sees the same brand-new world");

	// The mint argument really is ignored when mint_ok is false, rather than being read anyway.
	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, false, 0u, &v) == WSEED_MINT_FAILED,
	      "and a zero mint refuses identically, so the flag is what decides");

	testRmTree(dir);
}

// A brand-new world with a minted seed the card would not take.
//
// Forcing a real write refusal on a host that is not the console took finding, and
// world/world_test.c's genversion arm records the two obvious routes that are dead on this
// machine: a chmod 0555 directory does not stick (WSL mounts C: as drvfs with no metadata) and a
// DIRECTORY named seed.bin does not work either, because glibc's fopen(<a directory>, "rb")
// SUCCEEDS on Linux — the read probe would find a "sidecar" and answer WSEED_DAMAGED without
// ever reaching the branch under test.
//
// What is left, and behaves the same on Windows and Linux, is a world_dir underneath a path
// component that is a regular FILE: every open below it fails with ENOTDIR. Honest caveat, the
// same one world_test.c states: opendir() fails there too, so genVersionWorldHasRegionFile()
// answers false because the listing failed rather than because the directory was empty. It is
// the same branch either way, and the preconditions below pin down which branch answered rather
// than assuming.
static void testResolveStampFailedRefuses(void)
{
	char blocked[192];
	snprintf(blocked, sizeof blocked, "%s/notadir", g_root);
	remove(blocked);
	FILE* nf = fopen(blocked, "wb");
	CHECK(nf != NULL, "stamp-failed: a regular file stands where a directory would be");
	if (nf) fclose(nf);

	char inside[256];
	snprintf(inside, sizeof inside, "%s/w", blocked);

	// Preconditions. The path is non-empty, so the NO_WORLD_DIR early return is not what answers;
	// and no sidecar is readable there, so the sidecar-exists branch is not what answers either.
	uint32_t v = 0xDEADBEEFu;
	CHECK(worldSeedRead(inside, &v) == WSEED_OK,
	      "stamp-failed: nothing readable there, so the read reports absent rather than damaged");
	CHECK(v == WORLD_SEED_LEGACY, "stamp-failed: and reports the legacy seed for the absence");
	CHECK(seedSize(inside) == -1, "stamp-failed: there is genuinely no sidecar under the file");
	CHECK(!genVersionWorldHasRegionFile(inside),
	      "stamp-failed: and no region file either, so the brand-new branch is the one reached");
	CHECK(!worldSeedWrite(inside, 0x99999999u),
	      "stamp-failed: and a direct write there really does fail");

	// The refusal itself. WSEED_STAMP_FAILED together with *out holding the MINT is the signature
	// of the brand-new branch specifically: the legacy branch returns WSEED_OK with 1337, and the
	// no-directory return leaves 1337 as well.
	v = 0xDEADBEEFu;
	CHECK(worldSeedResolve(inside, true, 0x8BADF00Du, &v) == WSEED_STAMP_FAILED,
	      "a brand-new world whose sidecar the card refuses is REFUSED, not entered");
	CHECK(v == 0x8BADF00Du,
	      "and reports the seed it would have used, which is what says the mint branch answered");
	CHECK(seedSize(inside) == -1, "and nothing was written anywhere");

	remove(blocked);
}

// ── The routing (world/genrefuse.h) ──────────────────────────────────────────────────────
//
// Everything above tests what worldSeedResolve DECIDES. None of it tests whether anything acts
// on the decision, and that gap is not hypothetical here: 82d4a1e added GENVER_STAMP_FAILED with
// a red/green test proving it is returned, and main.c gated its refusal on an `if` naming two
// statuses by hand. The third fell straight through and the world loaded anyway.
//
// Written out one enumerator at a time rather than looped, so a red run names WHICH status
// stopped being routed. The two non-refusals are a second control: they must stay green whatever
// happens to the three refusals.
static void testSeedRefusalRouting(void)
{
	CHECK(worldSeedRefusalText(WSEED_OK) == NULL,
	      "WSEED_OK is not a refusal: a usable seed has nothing to say and nothing to stop");
	CHECK(worldSeedRefusalText(WSEED_NO_WORLD_DIR) == NULL,
	      "WSEED_NO_WORLD_DIR is not a refusal: a joined session's seed comes off the wire");
	CHECK(worldSeedRefusalText(WSEED_DAMAGED) != NULL,
	      "WSEED_DAMAGED is a refusal");
	CHECK(worldSeedRefusalText(WSEED_MINT_FAILED) != NULL,
	      "WSEED_MINT_FAILED is a refusal");
	CHECK(worldSeedRefusalText(WSEED_STAMP_FAILED) != NULL,
	      "WSEED_STAMP_FAILED is a refusal");

	CHECK(!worldSeedRefuses(WSEED_OK), "and the predicate agrees for WSEED_OK");
	CHECK(!worldSeedRefuses(WSEED_NO_WORLD_DIR),
	      "and for WSEED_NO_WORLD_DIR");
	CHECK(worldSeedRefuses(WSEED_DAMAGED), "and for WSEED_DAMAGED");
	CHECK(worldSeedRefuses(WSEED_MINT_FAILED), "and for WSEED_MINT_FAILED");
	CHECK(worldSeedRefuses(WSEED_STAMP_FAILED), "and for WSEED_STAMP_FAILED");

	const char* const d = worldSeedRefusalText(WSEED_DAMAGED);
	const char* const m = worldSeedRefusalText(WSEED_MINT_FAILED);
	const char* const s = worldSeedRefusalText(WSEED_STAMP_FAILED);

	// scene/title.h's TitleState.status is a char[48], so 47 characters plus a terminator is the
	// whole budget. A message that overruns it is silently cut off on a real console, and the
	// half the player loses is the end of the sentence that tells them what to do.
	CHECK(d && strlen(d) <= 47u, "the damaged message fits TitleState.status' 47-char budget");
	CHECK(m && strlen(m) <= 47u, "the mint-failed message fits it");
	CHECK(s && strlen(s) <= 47u, "the stamp-failed message fits it");

	// Three faults that ask the player for three different things: a damaged sidecar is something
	// they can only lose, a mint with no clock is a console whose date is unset and which they
	// can set, and a refused write is free space or a write-lock or a reseat.
	CHECK(d && m && strcmp(d, m) != 0, "damaged and mint-failed are different sentences");
	CHECK(d && s && strcmp(d, s) != 0, "damaged and stamp-failed are different sentences");
	CHECK(m && s && strcmp(m, s) != 0, "mint-failed and stamp-failed are different sentences");

	// And distinct from the generator's three, which matters more than it looks: both files write
	// to the same status line, and "world version file is damaged" versus "world seed file is
	// damaged" is the difference between the player deleting the right file and the wrong one.
	static const GenVersionStatus gv[] = { GENVER_TOO_NEW, GENVER_DAMAGED, GENVER_STAMP_FAILED };
	int clashes = 0, compared = 0;
	const char* const ws[] = { d, m, s };
	for (size_t i = 0; i < 3; i++) {
		const char* g = genVersionRefusalText(gv[i]);
		for (size_t j = 0; j < 3; j++) {
			compared++;
			if (g && ws[j] && strcmp(g, ws[j]) == 0) clashes++;
		}
	}
	CHECK(clashes == 0,
	      "no seed refusal reads the same as a generator refusal, so the player is never told to "
	      "go and delete the wrong file");
	CHECK(compared == 9,
	      "falsifiability: all nine seed-against-generator pairs were actually compared");
}

// ── The drift the shared scan exists to prevent ──────────────────────────────────────────
//
// genversion.c and worldseed.c ask the SAME question about the SAME directory, two lines apart
// inside main.c's genStart(): is this world old enough that its answer is the pre-v1.8.3 one.
// v1.8.3 Phase 1 made hasRegionFile() non-static as genVersionWorldHasRegionFile() so there is
// one scan rather than two free to drift. Two copies disagreeing is exactly how a world gets
// stamped LEGACY by one and given a freshly minted seed by the other: legacy terrain, new seed,
// rewritten under a base the player has already built.
//
// This is the check that goes red if anyone reintroduces a private copy of the scan.
static void testSeedAndVersionAgreeOnHistory(void)
{
	char dir[256];
	uint32_t seed, ver;

	// A brand-new directory. Both modules must call it new.
	seedTestDir(dir, sizeof dir, "agree-new");
	CHECK(!genVersionWorldHasRegionFile(dir), "agree/new: the shared scan says never played");

	ver = 0u;
	CHECK(genVersionResolve(dir, &ver) == GENVER_OK, "agree/new: the generator resolves OK");
	CHECK(ver == GEN_VERSION_FOR_NEW_WORLDS,
	      "agree/new: and stamps it as a world made by this build");

	seed = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x0C0FFEE0u, &seed) == WSEED_OK,
	      "agree/new: the seed resolves OK");
	CHECK(seed == 0x0C0FFEE0u,
	      "agree/new: and mints - the two modules agree this world is new");
	CHECK(seed != WORLD_SEED_LEGACY,
	      "agree/new: specifically, the seed side did NOT take the legacy branch");
	testRmTree(dir);

	// A played directory. Both modules must call it old, and neither may be swayed by the other's
	// sidecar sitting beside it — genver.bin and seed.bin are not region files.
	seedTestDir(dir, sizeof dir, "agree-old");
	CHECK(makeRegionFile(dir, "r.5.-7.bsr"), "agree/old: a region file is in place");
	CHECK(genVersionWorldHasRegionFile(dir), "agree/old: the shared scan says played");

	ver = 0u;
	CHECK(genVersionResolve(dir, &ver) == GENVER_OK, "agree/old: the generator resolves OK");
	CHECK(ver == GEN_VERSION_LEGACY, "agree/old: and derives the legacy generator");

	seed = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x0C0FFEE0u, &seed) == WSEED_OK,
	      "agree/old: the seed resolves OK");
	CHECK(seed == WORLD_SEED_LEGACY,
	      "agree/old: and derives the legacy seed - the two modules agree this world is old");

	// And the generator's own sidecar, written a moment ago, did not make the world look played
	// to the seed scan or vice versa.
	CHECK(genVersionWorldHasRegionFile(dir),
	      "agree/old: and both sidecars beside the region file leave the scan's answer alone");
	testRmTree(dir);

	// The falsifiable half: a directory holding a FILE that is not a region file must be called
	// new by both. Without this, "agree" could be satisfied by a scan that answers true for
	// anything at all.
	seedTestDir(dir, sizeof dir, "agree-notregion");
	{
		char p[320];
		snprintf(p, sizeof p, "%s/notes.txt", dir);
		FILE* f = fopen(p, "wb");
		CHECK(f != NULL, "agree/notregion: a non-region file is in place");
		if (f) { fputc('x', f); fclose(f); }
	}
	CHECK(!genVersionWorldHasRegionFile(dir),
	      "agree/notregion: a .txt is not evidence the world was ever played");
	seed = 0xDEADBEEFu;
	CHECK(worldSeedResolve(dir, true, 0x0C0FFEE0u, &seed) == WSEED_OK,
	      "agree/notregion: the seed resolves OK");
	CHECK(seed == 0x0C0FFEE0u, "agree/notregion: and mints, agreeing the world is new");
	testRmTree(dir);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== worldseed test ==");

	seedTestRoot();

	testControlNoWorldDir();
	testSeedConstantsPinned();

	testMintWithNoClockRefuses();
	testMintWithEitherClockAlone();
	testMintFromIdenticalReadingsIsAlwaysDistinct();
	testMintFromThisPlatformsClocks();

	testSidecarRoundTrip();
	testSidecarDamagedFiveWays();
	testSeedPathTooLongIsRefused();

	testResolveBrandNewWorldMints();
	testResolveBrandNewWorldKeepsItsSeedAfterSaving();
	testResolvePlayedWorldStaysLegacy();

	testResolveMintFailedRefuses();
	testResolveStampFailedRefuses();

	testSeedRefusalRouting();
	testSeedAndVersionAgreeOnHistory();

	testRmTree(g_root);

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	if (g_fails) printf("FAILED - %d of %d checks\n", g_fails, g_checks);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
