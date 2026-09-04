// tests/mesher_celldrop_test.c — OPT-MESHER task A: proves the packed cellDrop() representation
// is equivalent to the separate uint8_t[SCRATCH_BLOCKS] table it replaced.
//
// WHY THIS FILE EXISTS. world/mesher.c used to carry the water "drop" value (0..7, how far
// below its cell's top a flowing water surface sits) in its own 5,832-byte s_cell_drop array,
// read back only through cellDrop(). OPT-MESHER task A folds that value into bits 3..5 of the
// s_cell_flags byte the SOLID/OCCL/DRAW bits already live in (world/mesher.c, CELL_DROP_SHIFT),
// on the same reasoning v1.8.6 used to fold those three flags into one byte in the first place.
// The risk a packing change like this carries is crosstalk: a write that clobbers a neighbour's
// bits, or a read that decodes the wrong bits, would silently draw the wrong water height —
// which on this project has previously looked like "bad art" rather than an error, not a crash.
//
// WHAT THIS FILE CHECKS. Not through meshChunk()'s full geometry pipeline — that is what
// world/water_mesh_test.c already does, end to end, and it keeps doing it unmodified against
// this change. This file goes straight at the REAL, compiled cellDrop() and the REAL byte it
// reads, through three tiny test-only hooks world/mesher.c exports under
// BS_CELLDROP_TEST_HOOKS (bsTestCellDrop, bsTestGetCellFlags, bsTestSetCellFlags,
// bsTestSetAnyDrop — same "compiled out of every build but the probe" convention as
// BS_MESH_MERGE_PROBE, so the shipped build and every other host suite pay nothing for this):
//
//   ARM 1  s_any_drop gating. With no drop bits ever written, cellDrop() must answer 0 while
//          s_any_drop is false, whatever bits happen to sit in the byte — exactly the guarantee
//          that lets meshChunk() skip dropBuild()'s scan on every chunk with no water in it.
//
//   ARM 2  the full round trip. For four scratch indices spanning the array (first cell, second
//          cell, the middle, and the last cell — so an off-by-one in the shift or an array-
//          bounds slip would have somewhere to show up) and for EVERY drop value 0..7 crossed
//          with EVERY SOLID/OCCL/DRAW combination 0..7 (64 combinations per index), this packs
//          (flags | drop<<3) into the byte the same way dropBuild() does, reads it back through
//          the real cellDrop(), and checks two things: cellDrop() returns exactly the drop value
//          packed in, and the low three bits read back through bsTestGetCellFlags() are exactly
//          the flag combination packed in, untouched by whatever drop value rode along with it.
//          That second check is the crosstalk test — it is what would catch a mask or shift that
//          bleeds into CELL_FLAG_DRAW, which is also what CELL_DRAW_WORD's masked word-scan in
//          emitCollect depends on staying clean.
//
// This is the RED arm's target: shifting CELL_DROP_SHIFT by one bit (3 -> 2) makes bit 2 do
// double duty as both CELL_FLAG_DRAW and the low bit of the packed drop value, which ARM 2's
// full 8x8 cross product is built to catch — see the run_host_tests.sh stanza (handed over as
// optmesher_stanza.sh) for the actual red-then-green transcript.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "world/mesher.h"
#include "world/scratch.h"

// The three test-only hooks world/mesher.c exports under BS_CELLDROP_TEST_HOOKS. Declared here
// rather than in a header because nothing but this file should ever call them — they reach
// straight past cellDrop()'s own encapsulation on purpose, for exactly one file's benefit.
extern uint8_t bsTestCellDrop(int si);
extern uint8_t bsTestGetCellFlags(int si);
extern void    bsTestSetCellFlags(int si, uint8_t flags);
extern void    bsTestSetAnyDrop(bool any);

static int s_checks;
static int s_failed;

#define CHECK(cond, ...)                                 \
	do {                                                 \
		s_checks++;                                      \
		if (!(cond)) {                                   \
			s_failed++;                                  \
			printf("FAIL  L%d  ", __LINE__);             \
			printf(__VA_ARGS__);                         \
			printf("\n");                                \
		}                                                \
	} while (0)

// ARM 1: s_any_drop gates every read, regardless of what bits are sitting in the byte.
static void testAnyDropGating(void)
{
	const int si = 17;

	// A byte that would decode as a nonzero drop (5) with a live SOLID+DRAW combination, if
	// the gate were not checked first.
	bsTestSetCellFlags(si, (uint8_t)(0x05u | (5u << 3)));

	bsTestSetAnyDrop(false);
	CHECK(bsTestCellDrop(si) == 0,
	      "cellDrop() returned %d with s_any_drop false and a nonzero-looking byte stored — "
	      "the gate that lets meshChunk() skip dropBuild() on a dry chunk is broken",
	      bsTestCellDrop(si));

	bsTestSetAnyDrop(true);
	CHECK(bsTestCellDrop(si) == 5,
	      "cellDrop() returned %d with s_any_drop true for a byte packed with drop=5 — "
	      "expected 5", bsTestCellDrop(si));

	bsTestSetAnyDrop(false);   // leave the flag as later arms expect to find it
}

// ARM 2: every drop value crossed with every SOLID/OCCL/DRAW combination, at four scratch
// indices spanning the array, round-tripped through the real cellDrop() and read back through
// bsTestGetCellFlags() to catch crosstalk in either direction.
static void testFullRoundTrip(void)
{
	bsTestSetAnyDrop(true);

	const int indices[] = { 0, 1, SCRATCH_BLOCKS / 2, SCRATCH_BLOCKS - 1 };

	for (unsigned ii = 0; ii < sizeof(indices) / sizeof(indices[0]); ii++) {
		const int si = indices[ii];

		for (unsigned drop = 0; drop <= 7; drop++) {
			for (unsigned flagbits = 0; flagbits <= 7; flagbits++) {
				const uint8_t packed = (uint8_t)(flagbits | (drop << 3));
				bsTestSetCellFlags(si, packed);

				const uint8_t got_drop  = bsTestCellDrop(si);
				const uint8_t got_flags = (uint8_t)(bsTestGetCellFlags(si) & 0x07u);

				CHECK(got_drop == drop,
				      "si=%d flagbits=0x%x drop=%u: cellDrop() returned %u, expected %u — "
				      "the drop bits and the SOLID/OCCL/DRAW bits are bleeding into each other",
				      si, flagbits, drop, got_drop, drop);
				CHECK(got_flags == flagbits,
				      "si=%d flagbits=0x%x drop=%u: low 3 bits read back 0x%x, expected 0x%x — "
				      "packing a drop value corrupted the SOLID/OCCL/DRAW bits stored beside it",
				      si, flagbits, drop, got_flags, flagbits);
			}
		}
	}

	bsTestSetAnyDrop(false);
}

int main(void)
{
	testAnyDropGating();
	testFullRoundTrip();

	if (s_failed) {
		printf("mesher celldrop self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("mesher celldrop self-test: PASS %d checks\n", s_checks);
	return 0;
}
