// Host self-test for world/blockstate.c. Self-contained (its own main()), same reasoning
// as world/inventory_test.c and app/options_test.c.
//
// ── ABI pin: compiled on BOTH targets, not just the host ──────────────────────────────────
//
// blockstate.h explains why BlockStateEntry carries no enum and only fixed-width fields:
// this project's ARM EABI build compiles with -fshort-enums, which has already made a host
// sizeof() disagree with the console's once (Chunk: 8 bytes on ARM, 12 on host). Reasoning
// that THIS struct is fine because it avoids that trap is still reasoning, not measurement --
// so the _Static_asserts below are placed outside the "#ifndef __3DS__" guard on purpose: an
// empty translation unit's worth of assertions costs nothing at runtime, but the Makefile
// globs every .c under source/world into the console build (see the guard's own comment
// below), so these DO get compiled by arm-none-eabi-gcc under the real console flags every
// time anyone builds the 3DS target, not just when a host test happens to run.
//
// The literal numbers here are not guessed: they were measured by compiling a throwaway probe
// (furna_abi_probe.c, host gcc via WSL) and then compiling a second throwaway probe carrying
// these exact _Static_asserts through arm-none-eabi-gcc with this project's real console flags
// (-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -Wall -Wextra -Werror -O3),
// from devkitPro's own MSYS2 shell. Both targets agreed on every value below; a deliberate
// negative control (temporarily asserting sizeof(BlockStateEntry) == 31) was confirmed to fail
// that same ARM compile with "static assertion failed", proving the check can go red before
// trusting that it reads green for the right reason. See this lane's final report for the
// full transcript of both compiles.
#include <stddef.h>

#include "world/blockstate.h"

_Static_assert(sizeof(BlockId) == 1,
               "BlockId width diverged between host and ARM EABI");

_Static_assert(offsetof(BlockStateEntry, x) == 0,
               "BlockStateEntry.x offset diverged between host and ARM EABI");
_Static_assert(offsetof(BlockStateEntry, y) == 4,
               "BlockStateEntry.y offset diverged between host and ARM EABI");
_Static_assert(offsetof(BlockStateEntry, z) == 8,
               "BlockStateEntry.z offset diverged between host and ARM EABI");
_Static_assert(offsetof(BlockStateEntry, block_id) == 12,
               "BlockStateEntry.block_id offset diverged between host and ARM EABI");
_Static_assert(offsetof(BlockStateEntry, data) == 13,
               "BlockStateEntry.data offset diverged between host and ARM EABI");

_Static_assert(sizeof(BlockStateEntry) == 32,
               "sizeof(BlockStateEntry) diverged between host and ARM EABI");
_Static_assert(sizeof(BlockStateTable) == 8192,
               "sizeof(BlockStateTable) diverged between host and ARM EABI");
_Static_assert(sizeof(BlockStateTable) == (size_t)BLOCKSTATE_SLOTS * sizeof(BlockStateEntry),
               "BlockStateTable is not a flat array of BlockStateEntry on ARM EABI");

// The __3DS__ guard below is load-bearing rather than tidy: the Makefile globs every .c under
// source/world into the console build, so without it this file's main() would link against
// source/main.c's and the build would die with "multiple definition of `main'" -- the same
// bug world/inventory_test.c's own guard comment documents having already happened once.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "world/crc32.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// How many CHECK()s this suite makes on a healthy tree. A LITERAL on purpose -- see
// world/inventory_test.c's own INVENTORY_TEST_EXPECTED_CHECKS comment for why this is never
// derived from BLOCKSTATE_SLOTS or anything else the code under test can move: a shrunk loop
// bound must make this suite go red for MISSING checks, not quietly pass with fewer of them.
//
// Measured from this suite's own first clean run, not predicted: see checkCountPin() below.
// 191, from the run itself (2026-09-03): "CHECK COUNT: 48 check(s) were ADDED - expected 143,
// ran 191" -- 143 was a placeholder guess before the suite ever ran once; 191 is what it
// actually counted.
//
// Re-pinned to 890 (2026-09-05): BLOCKSTATE_SLOTS raised 64 -> 256 for chests, and
// testSaveLoadRoundTrip extended to fill the table to the new cap and round-trip it (see that
// test) added 699 CHECKs on top of the prior 191 -- measured the same way, from the run itself:
// "CHECK COUNT: 699 check(s) were ADDED - expected 191, ran 890."
//
// Re-pinned to 900 (2026-09-07): testLoadAllocFailureDoesNotClobberGoodFile added, covering the
// seam audit's blockStateLoad-lies-about-a-failed-malloc bug. Measured the same way: "CHECK
// COUNT: 10 check(s) were ADDED - expected 890, ran 900."
#define BLOCKSTATE_TEST_EXPECTED_CHECKS 900

static void checkCountPin(void)
{
	if (s_checks == BLOCKSTATE_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < BLOCKSTATE_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  BLOCKSTATE_TEST_EXPECTED_CHECKS to go green.\n",
		       BLOCKSTATE_TEST_EXPECTED_CHECKS - s_checks,
		       BLOCKSTATE_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         BLOCKSTATE_TEST_EXPECTED_CHECKS - s_checks,
			         BLOCKSTATE_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set BLOCKSTATE_TEST_EXPECTED_CHECKS in\n"
		       "  source/world/blockstate_test.c to %d.\n",
		       s_checks - BLOCKSTATE_TEST_EXPECTED_CHECKS,
		       BLOCKSTATE_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - BLOCKSTATE_TEST_EXPECTED_CHECKS,
			         BLOCKSTATE_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Copied from
// world/inventory_test.c's testMkdir, which copied it from world/world_test.c.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

#define TEST_DIR "build-host/bstest"

static const char* const s_test_paths[] = {
	TEST_DIR "/roundtrip/blockstate.dat",
	TEST_DIR "/roundtrip/blockstate.dat.tmp",
	TEST_DIR "/missing/blockstate.dat",
	TEST_DIR "/corrupt/blockstate.dat",
	TEST_DIR "/truncated/blockstate.dat",
	TEST_DIR "/wrongver/blockstate.dat",
	TEST_DIR "/duppos/blockstate.dat",
	TEST_DIR "/crash_recover/blockstate.dat",
	TEST_DIR "/crash_recover/blockstate.dat.tmp",
	TEST_DIR "/allocfail/blockstate.dat",
	TEST_DIR "/allocfail/blockstate.dat.tmp",
};

static void testCleanup(void)
{
	for (size_t i = 0; i < sizeof(s_test_paths) / sizeof(s_test_paths[0]); i++)
		remove(s_test_paths[i]);
	// Directories are left in place, same as inventory_test.c leaves TEST_DIR itself.
}

static bool writeRaw(const char* path, const void* data, size_t len)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

static void payload(uint8_t out[BLOCKSTATE_PAYLOAD_BYTES], uint8_t seed)
{
	for (int i = 0; i < BLOCKSTATE_PAYLOAD_BYTES; i++)
		out[i] = (uint8_t)(seed + i);
}

static bool payloadIsAllZero(const uint8_t data[BLOCKSTATE_PAYLOAD_BYTES])
{
	for (int i = 0; i < BLOCKSTATE_PAYLOAD_BYTES; i++)
		if (data[i] != 0) return false;
	return true;
}

// ── Basic create / get / set / remove round trip ──────────────────────────────────────────

static void testCreateGetSetRoundTrip(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(blockStateCount(&t) == 0);

	CHECK(blockStateCreate(&t, 5, 6, 7, BLOCK_STONE));
	CHECK(blockStateCount(&t) == 1);

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 5, 6, 7, BLOCK_STONE, out));
	CHECK(payloadIsAllZero(out));   // freshly created: zeroed payload

	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES];
	payload(in, 0x11);
	CHECK(blockStateSet(&t, 5, 6, 7, BLOCK_STONE, in));

	memset(out, 0, sizeof(out));
	CHECK(blockStateGet(&t, 5, 6, 7, BLOCK_STONE, out));
	CHECK(memcmp(out, in, sizeof(out)) == 0);

	blockStateRemove(&t, 5, 6, 7);
	CHECK(blockStateCount(&t) == 0);
	CHECK(!blockStateGet(&t, 5, 6, 7, BLOCK_STONE, out));
}

static void testCreateRejectsAirBlockId(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(!blockStateCreate(&t, 1, 1, 1, BLOCK_AIR));
	CHECK(blockStateCount(&t) == 0);
}

static void testRemoveOnEmptyPositionIsNoOp(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	blockStateRemove(&t, 9, 9, 9);   // must not crash
	CHECK(blockStateCount(&t) == 0);

	CHECK(blockStateCreate(&t, 1, 2, 3, BLOCK_STONE));
	blockStateRemove(&t, 100, 200, 300);   // different position: no-op
	CHECK(blockStateCount(&t) == 1);
}

static void testGetSetRefuseWrongBlockId(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(blockStateCreate(&t, 2, 2, 2, BLOCK_STONE));

	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES];
	payload(in, 0x22);
	CHECK(!blockStateSet(&t, 2, 2, 2, BLOCK_WATER, in));   // wrong id: refused

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(!blockStateGet(&t, 2, 2, 2, BLOCK_WATER, out));   // wrong id: refused

	// Refusal must not have touched the record.
	CHECK(blockStateGet(&t, 2, 2, 2, BLOCK_STONE, out));
	CHECK(payloadIsAllZero(out));
}

static void testGetSetRefuseMissingPosition(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES] = {0};
	CHECK(!blockStateGet(&t, 4, 4, 4, BLOCK_STONE, out));
	CHECK(!blockStateSet(&t, 4, 4, 4, BLOCK_STONE, in));
}

// ── Multiple simultaneous records stay independent ────────────────────────────────────────

static void testMultipleRecordsIndependent(void)
{
	BlockStateTable t;
	blockStateInit(&t);

	CHECK(blockStateCreate(&t, 0, 0, 0, BLOCK_STONE));
	CHECK(blockStateCreate(&t, 1, 0, 0, BLOCK_WATER));
	CHECK(blockStateCreate(&t, 0, 1, 0, BLOCK_STONE));
	CHECK(blockStateCount(&t) == 3);

	uint8_t a[BLOCKSTATE_PAYLOAD_BYTES], b[BLOCKSTATE_PAYLOAD_BYTES], c[BLOCKSTATE_PAYLOAD_BYTES];
	payload(a, 0x01);
	payload(b, 0x40);
	payload(c, 0x80);
	CHECK(blockStateSet(&t, 0, 0, 0, BLOCK_STONE, a));
	CHECK(blockStateSet(&t, 1, 0, 0, BLOCK_WATER, b));
	CHECK(blockStateSet(&t, 0, 1, 0, BLOCK_STONE, c));

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 0, 0, 0, BLOCK_STONE, out));
	CHECK(memcmp(out, a, sizeof(out)) == 0);
	CHECK(blockStateGet(&t, 1, 0, 0, BLOCK_WATER, out));
	CHECK(memcmp(out, b, sizeof(out)) == 0);
	CHECK(blockStateGet(&t, 0, 1, 0, BLOCK_STONE, out));
	CHECK(memcmp(out, c, sizeof(out)) == 0);

	// Removing one leaves the other two untouched.
	blockStateRemove(&t, 1, 0, 0);
	CHECK(blockStateCount(&t) == 2);
	CHECK(blockStateGet(&t, 0, 0, 0, BLOCK_STONE, out));
	CHECK(memcmp(out, a, sizeof(out)) == 0);
	CHECK(blockStateGet(&t, 0, 1, 0, BLOCK_STONE, out));
	CHECK(memcmp(out, c, sizeof(out)) == 0);
	CHECK(!blockStateGet(&t, 1, 0, 0, BLOCK_WATER, out));
}

// ── Table capacity ──────────────────────────────────────────────────────────────────────

static void testTableFillsAndRefusesTheNextNewPosition(void)
{
	BlockStateTable t;
	blockStateInit(&t);

	for (int i = 0; i < BLOCKSTATE_SLOTS; i++)
		CHECK(blockStateCreate(&t, i, 0, 0, BLOCK_STONE));
	CHECK(blockStateCount(&t) == BLOCKSTATE_SLOTS);

	// A genuinely new position: refused, table unchanged.
	CHECK(!blockStateCreate(&t, 999, 999, 999, BLOCK_STONE));
	CHECK(blockStateCount(&t) == BLOCKSTATE_SLOTS);

	// An EXISTING position must still succeed (reset-in-place) even while the table is full --
	// this is not "one more slot", it is reclaiming one already spent on this exact position.
	CHECK(blockStateCreate(&t, 0, 0, 0, BLOCK_WATER));
	CHECK(blockStateCount(&t) == BLOCKSTATE_SLOTS);
	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 0, 0, 0, BLOCK_WATER, out));
	CHECK(payloadIsAllZero(out));
}

// ── Block destruction: the subtle one ──────────────────────────────────────────────────────

// remove() then create() with a DIFFERENT block kind at the same position: the new record
// must not inherit the old one's payload, and the old id must no longer answer for it.
static void testDestructionThenDifferentBlockDoesNotResurrectOldData(void)
{
	BlockStateTable t;
	blockStateInit(&t);

	CHECK(blockStateCreate(&t, 7, 8, 9, BLOCK_STONE));
	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES];
	payload(in, 0x77);
	CHECK(blockStateSet(&t, 7, 8, 9, BLOCK_STONE, in));

	blockStateRemove(&t, 7, 8, 9);   // the primary defence: the break handler's job

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(!blockStateGet(&t, 7, 8, 9, BLOCK_STONE, out));   // old id: gone

	CHECK(blockStateCreate(&t, 7, 8, 9, BLOCK_WATER));      // a different block placed here
	CHECK(!blockStateGet(&t, 7, 8, 9, BLOCK_STONE, out));   // old id still refused
	CHECK(blockStateGet(&t, 7, 8, 9, BLOCK_WATER, out));    // new id answers
	CHECK(payloadIsAllZero(out));                           // and its payload is fresh, not 0x77
}

// The structural backstop: a LEAK (remove() never called) followed by create() with the
// SAME block kind at the same position must still zero the payload -- this is the case
// blockStateGet's id check cannot see (both records carry the same id), so
// blockStateCreate's unconditional reset is the only thing standing between this and a
// resurrected furnace.
static void testLeakedRecordSelfHealsOnRecreateSameId(void)
{
	BlockStateTable t;
	blockStateInit(&t);

	CHECK(blockStateCreate(&t, 3, 3, 3, BLOCK_STONE));
	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES];
	payload(in, 0x99);
	CHECK(blockStateSet(&t, 3, 3, 3, BLOCK_STONE, in));

	// No blockStateRemove() here -- simulating a break handler that forgot to call it.
	CHECK(blockStateCreate(&t, 3, 3, 3, BLOCK_STONE));   // "a new furnace placed here"

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 3, 3, 3, BLOCK_STONE, out));
	CHECK(payloadIsAllZero(out));   // NOT 0x99 -- the resurrection this test exists to catch
	CHECK(blockStateCount(&t) == 1);   // still one record, not two
}

// The same leak, but the id check's own job: a leaked record of one kind must not answer for
// a DIFFERENT kind that never called blockStateCreate at this position at all.
static void testLeakedRecordDoesNotAnswerForADifferentUncreatedKind(void)
{
	BlockStateTable t;
	blockStateInit(&t);

	CHECK(blockStateCreate(&t, 6, 6, 6, BLOCK_STONE));
	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES];
	payload(in, 0x55);
	CHECK(blockStateSet(&t, 6, 6, 6, BLOCK_STONE, in));
	// Leaked: no remove(), and no create() for BLOCK_WATER either -- just a query for it.

	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(!blockStateGet(&t, 6, 6, 6, BLOCK_WATER, out));
	uint8_t setdata[BLOCKSTATE_PAYLOAD_BYTES] = {0};
	CHECK(!blockStateSet(&t, 6, 6, 6, BLOCK_WATER, setdata));

	// The original record is untouched by either refused call.
	CHECK(blockStateGet(&t, 6, 6, 6, BLOCK_STONE, out));
	CHECK(memcmp(out, in, sizeof(out)) == 0);
}

// ── ABI/NULL-safety edges ──────────────────────────────────────────────────────────────────

static void testNullTableIsSafe(void)
{
	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	uint8_t in[BLOCKSTATE_PAYLOAD_BYTES] = {0};
	CHECK(!blockStateCreate(NULL, 1, 1, 1, BLOCK_STONE));
	blockStateRemove(NULL, 1, 1, 1);   // must not crash
	CHECK(!blockStateGet(NULL, 1, 1, 1, BLOCK_STONE, out));
	CHECK(!blockStateSet(NULL, 1, 1, 1, BLOCK_STONE, in));
	CHECK(blockStateCount(NULL) == 0);
}

// ── Save / load ─────────────────────────────────────────────────────────────────────────

static bool tablesEqual(const BlockStateTable* a, const BlockStateTable* b)
{
	// Field-by-field over every slot, in whatever order each currently holds them -- save/
	// load is not documented (or expected) to preserve slot ORDER, only slot CONTENTS, so
	// this checks "same set of live records with the same payloads", not memcmp(a, b, ...).
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++) {
		const BlockStateEntry* ea = &a->slots[i];
		if (ea->block_id == BLOCK_AIR) continue;
		uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
		if (!blockStateGet(b, ea->x, ea->y, ea->z, ea->block_id, out)) return false;
		if (memcmp(out, ea->data, BLOCKSTATE_PAYLOAD_BYTES) != 0) return false;
	}
	return blockStateCount(a) == blockStateCount(b);
}

static void testSaveLoadRoundTrip(void)
{
	const char* dir = TEST_DIR "/roundtrip";
	testMkdir(dir);

	BlockStateTable out;
	blockStateInit(&out);
	CHECK(blockStateCreate(&out, 1, 2, 3, BLOCK_STONE));
	CHECK(blockStateCreate(&out, -5, 60, 200, BLOCK_WATER));
	CHECK(blockStateCreate(&out, 0, 0, 0, BLOCK_STONE));
	uint8_t d1[BLOCKSTATE_PAYLOAD_BYTES], d2[BLOCKSTATE_PAYLOAD_BYTES];
	payload(d1, 0x10);
	payload(d2, 0x20);
	CHECK(blockStateSet(&out, 1, 2, 3, BLOCK_STONE, d1));
	CHECK(blockStateSet(&out, -5, 60, 200, BLOCK_WATER, d2));

	// Fill every remaining slot too, so this round-trip exercises the table at its NEW cap
	// (BLOCKSTATE_SLOTS, raised 64 -> 256 for chests -- see blockstate.h) rather than at a
	// handful of records that would have also fit on the old stack-allocated save/load
	// buffers. This is what proves blockStateSave/blockStateLoad's now-heap-allocated buffers
	// (see blockstate.c) are sized, filled and freed correctly at the size that actually
	// matters, not just at a small count that happened to already work before this change.
	// x = 2000+i never collides with the three explicit positions above (1, -5, 0).
	for (int i = 0; blockStateCount(&out) < BLOCKSTATE_SLOTS; i++) {
		const int x = 2000 + i;
		CHECK(blockStateCreate(&out, x, 0, 0, BLOCK_STONE));
		uint8_t d[BLOCKSTATE_PAYLOAD_BYTES];
		payload(d, (uint8_t)(i * 3));
		CHECK(blockStateSet(&out, x, 0, 0, BLOCK_STONE, d));
	}
	CHECK(blockStateCount(&out) == BLOCKSTATE_SLOTS);

	CHECK(blockStateSave(&out, dir));

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));
	CHECK(tablesEqual(&out, &in));

	FILE* leftover = fopen(TEST_DIR "/roundtrip/blockstate.dat.tmp", "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

static void testSaveLoadEmptyTable(void)
{
	const char* dir = TEST_DIR "/roundtrip";   // reuse: this runs after testSaveLoadRoundTrip
	testMkdir(dir);

	BlockStateTable out;
	blockStateInit(&out);
	CHECK(blockStateSave(&out, dir));   // an empty table saves fine (16-byte header-only file)

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));
	CHECK(blockStateCount(&in) == 0);
}

static void testLoadMissingFileGivesEmptyTable(void)
{
	const char* dir = TEST_DIR "/missing";
	testMkdir(dir);
	remove(TEST_DIR "/missing/blockstate.dat");

	BlockStateTable t;
	CHECK(blockStateLoad(&t, dir));
	CHECK(blockStateCount(&t) == 0);
}

static void testLoadCorruptFileGivesEmptyTable(void)
{
	const char* dir = TEST_DIR "/corrupt";
	testMkdir(dir);

	BlockStateTable out;
	blockStateInit(&out);
	CHECK(blockStateCreate(&out, 1, 1, 1, BLOCK_STONE));
	uint8_t d[BLOCKSTATE_PAYLOAD_BYTES];
	payload(d, 0x30);
	CHECK(blockStateSet(&out, 1, 1, 1, BLOCK_STONE, d));
	CHECK(blockStateSave(&out, dir));

	const char* path = TEST_DIR "/corrupt/blockstate.dat";
	FILE* f = fopen(path, "rb+");
	CHECK(f != NULL);
	if (f) {
		CHECK(fseek(f, 20, SEEK_SET) == 0);   // inside the one record's bytes
		const uint8_t flipped = 0xFF;
		CHECK(fwrite(&flipped, 1, 1, f) == 1);
		fclose(f);
	}

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));   // must not crash, must still report success
	CHECK(blockStateCount(&in) == 0);   // crc catches it: falls back to empty, not garbage
}

static void testLoadTruncatedFileGivesEmptyTable(void)
{
	const char* dir = TEST_DIR "/truncated";
	testMkdir(dir);

	BlockStateTable out;
	blockStateInit(&out);
	CHECK(blockStateCreate(&out, 2, 2, 2, BLOCK_STONE));
	CHECK(blockStateSave(&out, dir));

	const char* path = TEST_DIR "/truncated/blockstate.dat";
	FILE* f = fopen(path, "rb");
	CHECK(f != NULL);
	uint8_t header[16];
	size_t n = f ? fread(header, 1, sizeof(header), f) : 0;
	if (f) fclose(f);
	CHECK(n == sizeof(header));
	CHECK(writeRaw(path, header, n));   // payload never arrived

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));
	CHECK(blockStateCount(&in) == 0);
}

static void testLoadWrongVersionGivesEmptyTable(void)
{
	const char* dir = TEST_DIR "/wrongver";
	testMkdir(dir);

	BlockStateTable out;
	blockStateInit(&out);
	CHECK(blockStateCreate(&out, 4, 4, 4, BLOCK_STONE));
	CHECK(blockStateSave(&out, dir));

	// The version field is NOT covered by the crc (same as world/inventory.c's own header
	// layout), so this exercises the version check on its own rather than tripping the crc
	// check by accident.
	const char* path = TEST_DIR "/wrongver/blockstate.dat";
	FILE* f = fopen(path, "rb+");
	CHECK(f != NULL);
	if (f) {
		CHECK(fseek(f, 4, SEEK_SET) == 0);   // version is bytes 4..7
		const uint8_t bogus[4] = {0xFF, 0xFF, 0xFF, 0xFF};
		CHECK(fwrite(bogus, 1, 4, f) == 4);
		fclose(f);
	}

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));
	CHECK(blockStateCount(&in) == 0);
}

// A hand-crafted file with two records at the same (x, y, z) -- not a shape blockStateSave
// ever writes itself, but a corrupt-file/hand-edited-save possibility the loader must still
// refuse rather than load a permanently-shadowed slot.
static void testLoadDuplicatePositionsRejected(void)
{
	const char* dir = TEST_DIR "/duppos";
	testMkdir(dir);
	const char* path = TEST_DIR "/duppos/blockstate.dat";

	uint8_t buf[16 + 2 * 32];
	memset(buf, 0, sizeof(buf));
	// magic "BSBX", little-endian bytes -- matches blockstate.c's BLOCKSTATE_MAGIC.
	buf[0] = 'B'; buf[1] = 'S'; buf[2] = 'B'; buf[3] = 'X';
	buf[4] = 1; buf[5] = 0; buf[6] = 0; buf[7] = 0;    // version 1
	buf[8] = 2; buf[9] = 0; buf[10] = 0; buf[11] = 0;   // count 2
	// crc filled in below, after both records are written

	// Record 0: (9, 9, 9), BLOCK_STONE
	size_t r = 16;
	buf[r + 0] = 9; buf[r + 12] = BLOCK_STONE;
	// Record 1: (9, 9, 9) again, BLOCK_WATER -- the duplicate
	r += 32;
	buf[r + 0] = 9; buf[r + 12] = BLOCK_WATER;

	// crc32 over everything after the 16-byte header, same range blockStateLoad checks.
	uint32_t crc = crc32(buf + 16, sizeof(buf) - 16);
	buf[12] = (uint8_t)(crc);
	buf[13] = (uint8_t)(crc >> 8);
	buf[14] = (uint8_t)(crc >> 16);
	buf[15] = (uint8_t)(crc >> 24);

	CHECK(writeRaw(path, buf, sizeof(buf)));

	BlockStateTable in;
	CHECK(blockStateLoad(&in, dir));
	CHECK(blockStateCount(&in) == 0);   // whole file rejected, not "first one wins"
}

static void testCrashRecovery(void)
{
	const char* dir = TEST_DIR "/crash_recover";
	testMkdir(dir);
	const char* path = TEST_DIR "/crash_recover/blockstate.dat";
	const char* tmp  = TEST_DIR "/crash_recover/blockstate.dat.tmp";
	remove(path);
	remove(tmp);

	BlockStateTable saved;
	blockStateInit(&saved);
	CHECK(blockStateCreate(&saved, 8, 8, 8, BLOCK_STONE));
	uint8_t d[BLOCKSTATE_PAYLOAD_BYTES];
	payload(d, 0x60);
	CHECK(blockStateSet(&saved, 8, 8, 8, BLOCK_STONE, d));
	CHECK(blockStateSave(&saved, dir));

	// Simulate the cut: the on-disk state a power loss between blockStateSave's remove() and
	// rename() would leave -- the bytes sitting under ".tmp", nothing at the real path.
	FILE* src = fopen(path, "rb");
	CHECK(src != NULL);
	uint8_t buf[4096];
	size_t n = src ? fread(buf, 1, sizeof(buf), src) : 0;
	if (src) fclose(src);
	CHECK(writeRaw(tmp, buf, n));
	remove(path);

	BlockStateTable recovered;
	CHECK(blockStateLoad(&recovered, dir));
	uint8_t out[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&recovered, 8, 8, 8, BLOCK_STONE, out));
	CHECK(memcmp(out, d, sizeof(out)) == 0);

	FILE* real = fopen(path, "rb");
	CHECK(real != NULL);
	if (real) fclose(real);
	FILE* leftover = fopen(tmp, "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

// The seam audit's bug: blockStateLoad used to `return true` when its read-buffer malloc
// failed, indistinguishable from a genuine "no file"/"corrupt file" default. A caller that
// (reasonably) trusts true to mean "table is fine" and later calls blockStateSave with it
// then overwrites a perfectly good on-disk file with an empty one -- silent, irreversible.
//
// blockStateFailLoadAllocForTest forces that malloc to fail without actually exhausting the
// heap, so this failure path is exercised deterministically rather than only under real OOM.
static void testLoadAllocFailureDoesNotClobberGoodFile(void)
{
	const char* dir  = TEST_DIR "/allocfail";
	const char* path = TEST_DIR "/allocfail/blockstate.dat";
	testMkdir(dir);
	remove(path);
	remove(TEST_DIR "/allocfail/blockstate.dat.tmp");

	// A real, good file on disk -- what a live world's sidecar looks like right before the
	// load that is about to be forced to fail.
	BlockStateTable good;
	blockStateInit(&good);
	CHECK(blockStateCreate(&good, 9, 9, 9, BLOCK_STONE));
	uint8_t d[BLOCKSTATE_PAYLOAD_BYTES];
	payload(d, 0x70);
	CHECK(blockStateSet(&good, 9, 9, 9, BLOCK_STONE, d));
	CHECK(blockStateSave(&good, dir));

	// The good file's raw bytes, read now, before the forced failure -- so "untouched" below
	// is checked against the real thing on disk, not against blockStateGet's opinion of it.
	FILE* rf = fopen(path, "rb");
	CHECK(rf != NULL);
	uint8_t before[4096];
	size_t before_len = rf ? fread(before, 1, sizeof(before), rf) : 0;
	if (rf) fclose(rf);
	CHECK(before_len > 0);

	// Force the read buffer's malloc to fail, the same shape a real OOM leaves: fopen()
	// succeeds (the file is right there), the allocation right after it does not.
	blockStateFailLoadAllocForTest(true);

	BlockStateTable in;
	const bool load_ok = blockStateLoad(&in, dir);
	blockStateFailLoadAllocForTest(false);   // never leak the forced failure into later tests

	CHECK(!load_ok);                     // the bug: this used to be true
	CHECK(blockStateCount(&in) == 0);    // still the safe default -- blockStateInit ran

	// The behaviour this fix exists to guarantee: a caller that checks blockStateLoad's return
	// value (as main.c must -- see this fix's report for the patch main.c still needs) never
	// carries this empty table into a save, so the good file on disk is never touched. Written
	// as an explicit "if" rather than a bare assumption so the red arm (blockStateLoad reverted
	// to always returning true) actually performs the clobbering save and gets caught below,
	// instead of the bug quietly not being exercised.
	if (load_ok) CHECK(blockStateSave(&in, dir));   // NOT reached once the fix is in place

	FILE* af = fopen(path, "rb");
	CHECK(af != NULL);
	uint8_t after[4096];
	size_t after_len = af ? fread(after, 1, sizeof(after), af) : 0;
	if (af) fclose(af);
	CHECK(after_len == before_len);
	CHECK(after_len > 0 && memcmp(before, after, before_len) == 0);
}

static void testSaveLoadRefuseNullOrEmptyWorldDir(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(blockStateCreate(&t, 1, 1, 1, BLOCK_STONE));

	CHECK(!blockStateSave(&t, NULL));
	CHECK(!blockStateSave(&t, ""));
	CHECK(!blockStateSave(NULL, TEST_DIR "/missing"));

	BlockStateTable in;
	CHECK(!blockStateLoad(&in, NULL));
	CHECK(!blockStateLoad(&in, ""));
	CHECK(!blockStateLoad(NULL, TEST_DIR "/missing"));
}

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testCreateGetSetRoundTrip();
	testCreateRejectsAirBlockId();
	testRemoveOnEmptyPositionIsNoOp();
	testGetSetRefuseWrongBlockId();
	testGetSetRefuseMissingPosition();
	testMultipleRecordsIndependent();
	testTableFillsAndRefusesTheNextNewPosition();

	testDestructionThenDifferentBlockDoesNotResurrectOldData();
	testLeakedRecordSelfHealsOnRecreateSameId();
	testLeakedRecordDoesNotAnswerForADifferentUncreatedKind();

	testNullTableIsSafe();

	testSaveLoadRoundTrip();
	testSaveLoadEmptyTable();
	testLoadMissingFileGivesEmptyTable();
	testLoadCorruptFileGivesEmptyTable();
	testLoadTruncatedFileGivesEmptyTable();
	testLoadWrongVersionGivesEmptyTable();
	testLoadDuplicatePositionsRejected();
	testCrashRecovery();
	testLoadAllocFailureDoesNotClobberGoodFile();
	testSaveLoadRefuseNullOrEmptyWorldDir();

	testCleanup();

	checkCountPin();

	if (s_fails == 0)
		printf("blockstate self-test: PASS  %d checks\n", s_checks);
	else
		printf("blockstate self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here beyond the _Static_asserts above. An empty translation unit is
// not valid ISO C, so give the compiler one declaration to chew on.
typedef int blockstate_test_host_only_t;

#endif   // !__3DS__
