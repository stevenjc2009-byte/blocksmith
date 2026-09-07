// Host self-test for world/chest.c. Self-contained (its own main()), same reasoning as
// world/blockstate_test.c and world/furnace_test.c.
//
// The __3DS__ guard below is load-bearing rather than tidy, same reason as every other
// world/*_test.c: the Makefile globs every .c under source/world into the console build, so
// without it this file's main() would link against source/main.c's and the build would die
// with "multiple definition of `main'".
//
// ── What chest.c is, and why this suite is shaped the way it is ───────────────────────────
//
// docs/decision-1.9.0-chest-storage.md settled a three-way disagreement between chest.h's
// own prototype and two separate v1.9.0 plan documents about where a chest's contents live.
// The winner: chests share world/blockstate.h's existing BlockStateTable with furnaces
// (BLOCKSTATE_SLOTS raised 64 -> 256 to make room), not a dedicated table. chest.c is
// therefore small on purpose — pack/unpack over an already-generalised store — and this
// suite's job is to prove that small surface is actually correct, not to re-derive
// blockstate.c's own correctness (blockstate_test.c already owns 890 checks of that).
//
// chest.h's own header comment gives the two facts this suite is built to check most
// carefully:
//
//   "8 of them is meant to use the whole 16-byte payload with nothing reserved... if this
//   ever goes false on purpose, chestStatePack needs a zeroing pass for the newly-unused
//   tail, the same one furnaceStatePack already has."
//
// i.e. unlike FurnaceState (11 of 16 bytes used, 5 reserved and zeroed by furnaceStatePack),
// ChestState uses all 16 with nothing left to zero — CHEST_SLOTS*2 == BLOCKSTATE_PAYLOAD_BYTES
// exactly, guarded by chest.h's own _Static_assert. testPackWritesExactlyPayloadBytes below
// is the check that would go red the day that assert's premise stops holding and nobody adds
// the zeroing pass the comment warns about: it poisons the output buffer before packing and
// confirms every one of the 16 bytes is overwritten with real data, none left as poison.
//
// Deliberately NOT hand-copying chest.c's pack/unpack arithmetic into this file to check it
// against — this codebase's own comments (world/*_test.c generally, and this project's own
// session notes on "a test that links nothing tests nothing") warn repeatedly that two copies
// of one rule agreeing with itself proves nothing. Every check below links and calls the real
// world/chest.c; there is no second implementation here to compare it to.
#include "world/chest.h"

#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"

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

// How many CHECK()s this suite makes on a healthy tree. A LITERAL on purpose, never derived
// from CHEST_SLOTS or anything else the code under test can move -- see world/blockstate_
// test.c's own comment on its equivalent pin for why: a check that silently stops running
// must show up as a missing check, not a quiet pass.
//
// Measured (2026-09-05): seeded at a placeholder 129 before this suite ever ran once; the
// first real run reported "CHECK COUNT: 4 check(s) were ADDED - expected 129, ran 133" and
// exited 1, confirming the pin can go red. 133 is what it actually counted.
#define CHEST_TEST_EXPECTED_CHECKS 133

static void checkCountPin(void)
{
	if (s_checks == CHEST_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < CHEST_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  CHEST_TEST_EXPECTED_CHECKS to go green.\n",
		       CHEST_TEST_EXPECTED_CHECKS - s_checks,
		       CHEST_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         CHEST_TEST_EXPECTED_CHECKS - s_checks,
			         CHEST_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set CHEST_TEST_EXPECTED_CHECKS in\n"
		       "  source/world/chest_test.c to %d.\n",
		       s_checks - CHEST_TEST_EXPECTED_CHECKS,
		       CHEST_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - CHEST_TEST_EXPECTED_CHECKS,
			         CHEST_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// ── init ────────────────────────────────────────────────────────────────────────────────

static void testInitLeavesEverySlotEmpty(void)
{
	ChestState cs;
	memset(&cs, 0xAA, sizeof(cs));   // poison first -- init must set every field itself
	chestStateInit(&cs);

	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(cs.item[i]  == ITEM_NONE);
		CHECK(cs.count[i] == 0);
	}
}

// ── pack / unpack round trip ───────────────────────────────────────────────────────────────

static void testPackUnpackRoundTripFullChest(void)
{
	ChestState cs;
	chestStateInit(&cs);
	// Every slot distinct, so a swapped-index bug (slot i's data landing at slot j) would
	// show up as a mismatch rather than an accidental match.
	for (int i = 0; i < CHEST_SLOTS; i++) {
		cs.item[i]  = (ItemId)(BLOCK_STONE + i);
		cs.count[i] = (uint8_t)(i + 1);
	}

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, buf);

	ChestState back;
	memset(&back, 0xAA, sizeof(back));   // poison, so unpack must set every field itself
	chestStateUnpack(&back, buf);

	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(back.item[i]  == cs.item[i]);
		CHECK(back.count[i] == cs.count[i]);
	}
}

static void testPackUnpackRoundTripEmptyChest(void)
{
	ChestState cs;
	chestStateInit(&cs);   // every slot ITEM_NONE / 0

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, buf);

	ChestState back;
	memset(&back, 0xAA, sizeof(back));
	chestStateUnpack(&back, buf);

	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(back.item[i]  == ITEM_NONE);
		CHECK(back.count[i] == 0);
	}
}

static void testPackUnpackRoundTripSparseChest(void)
{
	ChestState cs;
	chestStateInit(&cs);
	// Odd slots filled, even slots left empty -- exercises a chest that is not full and not
	// empty, and that mixes ITEM_NONE slots in among live ones rather than at one end.
	cs.item[1]  = BLOCK_SAND;        cs.count[1]  = 12;
	cs.item[3]  = BLOCK_PLANKS;      cs.count[3]  = 64;
	cs.item[5]  = BLOCK_TORCH;       cs.count[5]  = 1;
	cs.item[7]  = BLOCK_IRON_ORE;    cs.count[7]  = 5;

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, buf);

	ChestState back;
	memset(&back, 0xAA, sizeof(back));
	chestStateUnpack(&back, buf);

	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(back.item[i]  == cs.item[i]);
		CHECK(back.count[i] == cs.count[i]);
	}
}

// ── the zeroing-pass question chest.h's own header comment predicts ───────────────────────

// chest.h: "8 of them is meant to use the whole 16-byte payload with nothing reserved...
// chestStatePack has nothing left to zero." This is the check that would catch it if that
// stopped being true and nobody added the zeroing pass the comment says furnaceStatePack
// already needed. Poison every byte of the output buffer first, so a byte chestStatePack
// forgets to write would still read back as poison, not as an accidental zero.
static void testPackWritesExactlyPayloadBytesWithNoUninitialisedTail(void)
{
	ChestState cs;
	chestStateInit(&cs);
	for (int i = 0; i < CHEST_SLOTS; i++) {
		cs.item[i]  = (ItemId)(10 + i);
		cs.count[i] = (uint8_t)(20 + i);
	}

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	memset(buf, 0xFF, sizeof(buf));   // poison every byte first
	chestStatePack(&cs, buf);

	CHECK(CHEST_PAYLOAD_USED_BYTES == BLOCKSTATE_PAYLOAD_BYTES);   // the premise the header names

	// Every byte must be exactly the expected wire value -- 0xFF surviving anywhere means
	// chestStatePack skipped it.
	for (int i = 0; i < CHEST_SLOTS; i++) {
		CHECK(buf[i * 2]     == (uint8_t)(10 + i));
		CHECK(buf[i * 2 + 1] == (uint8_t)(20 + i));
	}
	// No loop tail beyond CHEST_SLOTS*2 exists to check separately -- CHEST_PAYLOAD_USED_BYTES
	// == BLOCKSTATE_PAYLOAD_BYTES is exactly what the check above already covers byte-for-byte.
}

// ── boundary values ────────────────────────────────────────────────────────────────────────

static void testBoundaryCountZeroAndStackMax(void)
{
	ChestState cs;
	chestStateInit(&cs);
	cs.item[0]  = BLOCK_STONE;
	cs.count[0] = 0;              // a slot can carry an item id with count 0 -- pack/unpack
	                               // does not enforce the game rule that count 0 means empty;
	                               // that rule belongs to whatever writes a ChestState, not to
	                               // the wire format (chest.c takes no view on it either way).
	cs.item[1]  = BLOCK_PLANKS;
	cs.count[1] = INV_STACK_MAX;   // 99 -- the highest count the game rules ever produce

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, buf);

	ChestState back;
	memset(&back, 0xAA, sizeof(back));
	chestStateUnpack(&back, buf);

	CHECK(back.item[0]  == BLOCK_STONE);
	CHECK(back.count[0] == 0);
	CHECK(back.item[1]  == BLOCK_PLANKS);
	CHECK(back.count[1] == INV_STACK_MAX);
}

// BLOCK_CHEST (43) is the highest id block.h currently defines (see its own enum and the
// _Static_assert pinning BLOCK_CHEST == 43). BLOCK_CHEST + 1 (44) is one past the defined
// table -- an id no registry row exists for. chestStatePack/Unpack take ItemId as an opaque
// uint8_t-sized value and validate nothing against the registry (chest.h's header: this file
// "owns what the 16 opaque bytes MEAN for a chest and nothing about where they are kept"),
// so both must round-trip byte-exact whether or not the id is one the registry defines --
// that validation, if wanted, is a caller's job (inventoryCanHold), not chestStatePack's.
static void testBoundaryHighestDefinedItemIdAndOnePastIt(void)
{
	ChestState cs;
	chestStateInit(&cs);
	cs.item[0]  = BLOCK_CHEST;            // highest id the registry defines today
	cs.count[0] = 1;
	cs.item[1]  = (ItemId)(BLOCK_CHEST + 1);   // one past it -- undefined, still a legal byte
	cs.count[1] = 1;

	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, buf);

	ChestState back;
	memset(&back, 0xAA, sizeof(back));
	chestStateUnpack(&back, buf);

	CHECK(back.item[0] == BLOCK_CHEST);
	CHECK(back.item[1] == (ItemId)(BLOCK_CHEST + 1));
	CHECK(buf[0] == BLOCK_CHEST);
	CHECK(buf[2] == (uint8_t)(BLOCK_CHEST + 1));
}

// chestStateUnpack reads raw bytes with no interpretation (chest.c: "for i in 0..CHEST_SLOTS-1
// ... cs->item[i] = (ItemId)in[i*2]"). ItemId and the count field are both plain uint8_t, so
// this is a structural guarantee rather than a validated clamp: whatever byte sits in the
// payload, unpack cannot produce a slot count outside 0..255 -- it has no wider type to widen
// into, and no sign bit to misread as negative. That is a real and useful property even though
// it is not the same claim as "unpack enforces INV_STACK_MAX" (it does not; see
// testBoundaryCountZeroAndStackMax's comment on count 0, and chest.h's header on this file
// taking no view on what the bytes mean). A caller that wants the *game* rule enforced still
// has to check count <= INV_STACK_MAX itself -- this test documents what unpack hands that
// caller, not a rule unpack does not have.
static void testUnpackArbitraryBytesStaysInByteRange(void)
{
	uint8_t buf[BLOCKSTATE_PAYLOAD_BYTES];
	for (int i = 0; i < BLOCKSTATE_PAYLOAD_BYTES; i++)
		buf[i] = (uint8_t)(i * 37 + 5);   // an arbitrary, non-repeating byte pattern --
		                                   // not all-same, not all-zero, not a value chest.c's
		                                   // own code happens to special-case
	// Cover the type's actual extremes too, not just a mid-range pattern.
	buf[0] = 0xFF;   // slot 0 item: the highest byte value at all, defined or not
	buf[1] = 0xFF;   // slot 0 count: same

	ChestState back;
	memset(&back, 0x55, sizeof(back));   // different poison than the round-trip tests use
	chestStateUnpack(&back, buf);

	for (int i = 0; i < CHEST_SLOTS; i++) {
		// Every unpacked value is exactly the source byte -- no transformation, and (being a
		// uint8_t compared against uint8_t bytes) structurally within 0..255. A slot count
		// later code trusts without its own range check would have to come from a wider or
		// signed type than this; ChestState.count[] is neither.
		CHECK((uint8_t)back.item[i]  == buf[i * 2]);
		CHECK(back.count[i]          == buf[i * 2 + 1]);
	}
	CHECK((uint8_t)back.item[0] == 0xFF);
	CHECK(back.count[0]         == 0xFF);
}

// ── blockstate integration: no second side table ────────────────────────────────────────

// Proves the whole point of chest.h's own header comment and docs/decision-1.9.0-chest-
// storage.md: a chest's live state round-trips through world/blockstate.c's real 16-byte
// opaque payload and nothing else. If this ever needed a second table, this is the test
// that would have to change to prove it -- mirrors furnace_test.c's own
// testRoundTripsThroughRealBlockstateTable exactly, for the identical reason.
static void testRoundTripsThroughRealBlockstateTable(void)
{
	BlockStateTable t;
	blockStateInit(&t);
	CHECK(blockStateCreate(&t, 5, 70, -12, BLOCK_CHEST));

	uint8_t raw[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 5, 70, -12, BLOCK_CHEST, raw));

	ChestState cs;
	chestStateUnpack(&cs, raw);
	for (int i = 0; i < CHEST_SLOTS; i++)
		CHECK(cs.item[i] == ITEM_NONE);   // a freshly-created chest starts empty

	cs.item[0]  = BLOCK_SAND;
	cs.count[0] = 30;
	cs.item[7]  = BLOCK_APPLE;
	cs.count[7] = 3;

	uint8_t packed[BLOCKSTATE_PAYLOAD_BYTES];
	chestStatePack(&cs, packed);
	CHECK(blockStateSet(&t, 5, 70, -12, BLOCK_CHEST, packed));

	uint8_t readBack[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&t, 5, 70, -12, BLOCK_CHEST, readBack));
	CHECK(memcmp(readBack, packed, sizeof(packed)) == 0);

	ChestState fromTable;
	chestStateUnpack(&fromTable, readBack);
	CHECK(fromTable.item[0]  == BLOCK_SAND);
	CHECK(fromTable.count[0] == 30);
	CHECK(fromTable.item[7]  == BLOCK_APPLE);
	CHECK(fromTable.count[7] == 3);
	for (int i = 1; i < 7; i++)
		CHECK(fromTable.item[i] == ITEM_NONE);   // untouched slots stayed empty

	// A different block id at the same position must not read the chest's bytes back.
	CHECK(!blockStateGet(&t, 5, 70, -12, BLOCK_FURNACE, readBack));

	// Breaking the chest removes its record, same rule every other stateful block follows.
	blockStateRemove(&t, 5, 70, -12);
	CHECK(!blockStateGet(&t, 5, 70, -12, BLOCK_CHEST, readBack));
	CHECK(blockStateCount(&t) == 0);
}

int main(void)
{
	testInitLeavesEverySlotEmpty();

	testPackUnpackRoundTripFullChest();
	testPackUnpackRoundTripEmptyChest();
	testPackUnpackRoundTripSparseChest();

	testPackWritesExactlyPayloadBytesWithNoUninitialisedTail();

	testBoundaryCountZeroAndStackMax();
	testBoundaryHighestDefinedItemIdAndOnePastIt();
	testUnpackArbitraryBytesStaysInByteRange();

	testRoundTripsThroughRealBlockstateTable();

	checkCountPin();

	if (s_fails == 0)
		printf("chest self-test: PASS  %d checks\n", s_checks);
	else
		printf("chest self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: an empty translation unit is not valid ISO C, so give the compiler one
// declaration to chew on. Same shape as every other world/*_test.c's else-branch.
typedef int chest_test_host_only_t;

#endif   // !__3DS__
