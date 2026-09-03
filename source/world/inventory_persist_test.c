/* inventory_persist_test — host unit test for world/inventory.c's on-disk SAVE FORMAT.
 *
 * Own binary rather than more checks in world/inventory_test.c, for the reason
 * source/world/Makefile.playerpose-test gives about playerpose_test.c: this is a
 * self-contained contract with its own failure modes, and a suite that is one file per
 * contract says which contract broke without anybody reading a line number.
 *
 * world/inventory_test.c already exists and already covers inventoryAdd/Remove/Swap/
 * MoveUnits/SplitStack/crafting, plus a handful of save/load smoke checks (round trip,
 * missing file, one CRC-corruption case, one truncation case, one crash-recovery
 * direction) — but it was never given a Makefile of its own the way playerpose's,
 * blockdiff's and networld's suites were, and it does not exercise every degradation
 * path the loader in inventory.c actually has: bad magic, bad version, bad slot_count,
 * the "real file beats a leftover tmp" half of crash recovery, the per-slot defence in
 * depth (an out-of-range item id, ITEM_NONE paired with a nonzero count, a valid item
 * paired with a zero count, an oversized count), the hotbar-selection clamp, or the two
 * NULL guards. This file is that missing coverage, built and run the way
 * world/playerpose_test.c is: its own gcc stanza in tools/run_host_tests.sh, plus a
 * Makefile.inventory-persist-test alongside it for the finer warning set.
 *
 * This suite reads and writes <world_dir>/inventory.dat directly at the byte level,
 * independent of inventory.c's own save/load, for the corruption and boundary cases —
 * inventoryAdd() cannot put an out-of-range item id or an oversized count into a slot
 * (inventoryCanHold refuses it before it is ever written), so the only way to exercise
 * the loader's defence in depth is to hand-build a file the writer itself would never
 * produce.
 *
 * The __3DS__ guard is load-bearing, not tidy — the same one net/blockdiff_test.c and
 * world/playerpose_test.c carry. mc/Makefile globs every .c under source/world into the
 * console build, so without it this file's main() links against source/main.c's and the
 * build dies with "multiple definition of `main'".
 */
#ifndef __3DS__

/* mkdir(2) and the S_IRWXU macros are POSIX, and -std=c11 (not gnu11) makes glibc hide
 * them behind this. Same reason world/playerpose_test.c and every other host test in
 * this tree does it. */
#define _POSIX_C_SOURCE 200809L

#include "world/inventory.h"

#include "world/block.h"
#include "world/crc32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) checkAt((cond), #cond, __LINE__)

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL  L%d %s\n", line, what);
	}
}

/* ── the scratch world directory ─────────────────────────────────────────────────────
 *
 * Under build-host/, same reasoning world/playerpose_test.c gives: tools/run_host_tests.sh
 * already owns that directory and cleans it, and a test that writes outside the tree
 * behaves differently on the one machine where it matters. A name distinct from
 * world/inventory_test.c's own "build-host/invtest" so the two suites, which may run in
 * the same shell in either order, never share a directory.
 */
#define INV_DIR "build-host/tinv-persist"

static const char* invFile(void) { return INV_DIR "/inventory.dat"; }
static const char* invTmp(void)  { return INV_DIR "/inventory.dat.tmp"; }

static void freshDir(void)
{
	mkdir("build-host", 0777);
	mkdir(INV_DIR, 0777);
	remove(invFile());
	remove(invTmp());
}

static bool writeBytes(const char* path, const uint8_t* buf, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static size_t readBytes(const char* path, uint8_t* buf, size_t cap)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	const size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

static bool fileExists(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

static bool invEqual(const Inventory* a, const Inventory* b)
{
	if (a->selected_hotbar != b->selected_hotbar) return false;
	for (int i = 0; i < INV_SLOT_COUNT; i++) {
		if (a->slots[i].item  != b->slots[i].item)  return false;
		if (a->slots[i].count != b->slots[i].count) return false;
	}
	return true;
}

/* ── a hand-built file, so the corruption and defence-in-depth cases can be exact ────
 *
 * Deliberately NOT built by calling inventorySave and then poking the result, for the
 * same reason world/playerpose_test.c gives about its own buildFile: several checks
 * below are about a specific byte or a specific per-slot value being wrong, and a
 * helper that shares the writer's idea of the layout would move with it if the writer
 * ever did. This encodes the layout from inventory.c's own header comment
 * independently, so a load that starts reading the wrong offset is caught rather than
 * agreed with.
 *
 * Also the ONLY way to get an out-of-range item id or an oversized count onto disk at
 * all: inventoryAdd()/inventorySave() can never produce one, because inventoryCanHold()
 * and INV_STACK_MAX refuse them before a slot is ever written.
 */
#define TEST_HDR_BYTES  20
#define TEST_FILE_BYTES (TEST_HDR_BYTES + INV_SLOT_COUNT * 2)   /* 68 today */

static void put32le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* "BSI1" little-endian, spelled out here rather than taken from the module, so a magic
 * changed in inventory.c without meaning to is caught here instead of agreeing with
 * itself — the same reasoning world/playerpose_test.c gives for EXPECT_MAGIC. */
#define EXPECT_MAGIC   0x31495342u
#define EXPECT_VERSION 1u

/* Builds a full TEST_FILE_BYTES record from a raw (selected_hotbar, slots[]) pair, with
 * whatever magic/version/slot_count the caller wants (so the bad-header tests can lie
 * about them) and a CRC computed over the ACTUAL bytes written — including any
 * out-of-range item id or oversized count the caller put in `inv`, which is the whole
 * point: those files are byte-for-byte self-consistent and a checksum cannot catch them,
 * only inventoryLoad()'s own defence in depth can. */
static void buildFile(uint8_t* buf, uint32_t magic, uint32_t version, uint32_t slot_count,
                       const Inventory* inv)
{
	buf[16] = inv->selected_hotbar;
	buf[17] = buf[18] = buf[19] = 0;
	for (int i = 0; i < INV_SLOT_COUNT; i++) {
		buf[20 + i * 2 + 0] = inv->slots[i].item;
		buf[20 + i * 2 + 1] = inv->slots[i].count;
	}
	const uint32_t crc = crc32(buf + 16, TEST_FILE_BYTES - 16);
	put32le(buf + 0,  magic);
	put32le(buf + 4,  version);
	put32le(buf + 8,  slot_count);
	put32le(buf + 12, crc);
}

static Inventory refInventory(void)
{
	Inventory inv;
	inventoryInit(&inv);
	inv.slots[0] = (InvSlot){ .item = BLOCK_DIRT,   .count = 40 };
	inv.slots[1] = (InvSlot){ .item = BLOCK_STONE,  .count = 12 };
	inv.slots[8] = (InvSlot){ .item = BLOCK_LEAVES, .count = 3  };
	inv.selected_hotbar = 5;
	return inv;
}

/* ── 1. round trip ─────────────────────────────────────────────────────────────────── */

static void testRoundTripIsBitExact(void)
{
	freshDir();

	const Inventory in = refInventory();
	CHECK(inventorySave(&in, INV_DIR));

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));

	/* The file the writer actually produced is the layout inventory.c's header comment
	 * documents. Without this, a writer and a reader that agree with each other on a
	 * wrong offset round-trip perfectly and nothing notices until a save from a
	 * different build is read — world/playerpose_test.c makes exactly this argument
	 * about its own format. */
	uint8_t got[TEST_FILE_BYTES + 8];
	const size_t n = readBytes(invFile(), got, sizeof got);
	CHECK(n == TEST_FILE_BYTES);
	if (n == TEST_FILE_BYTES) {
		uint8_t want[TEST_FILE_BYTES];
		buildFile(want, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &in);
		CHECK(memcmp(got, want, TEST_FILE_BYTES) == 0);
	}

	/* The tmp file is gone once the save has landed — a leftover would be promoted by
	 * the recover pass the next time a real file went missing. */
	CHECK(!fileExists(invTmp()));
}

/* The control for the degradation arms below: a clean save and load of an ordinary
 * inventory, which must stay green while any of the refusal checks is being sabotaged.
 * Not a control for the writer or the checksum — with the CRC computed over the wrong
 * range this goes red along with everything else, because a broken writer breaks every
 * valid case there is. */
static void testControlCleanSaveLoad(void)
{
	freshDir();
	const Inventory in = refInventory();
	CHECK(inventorySave(&in, INV_DIR));

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));
}

/* ── 2. boundaries ────────────────────────────────────────────────────────────────── */

static void testFullInventoryRoundTrips(void)
{
	freshDir();

	Inventory in;
	inventoryInit(&in);
	/* Every slot occupied, cycling the game's own six item ids, at INV_STACK_MAX — the
	 * densest a real save can ever be. */
	static const ItemId items[] = { BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE,
	                                 BLOCK_SAND,  BLOCK_WOOD, BLOCK_LEAVES };
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		in.slots[i] = (InvSlot){ .item = items[i % 6], .count = INV_STACK_MAX };
	in.selected_hotbar = INV_HOTBAR_SLOTS - 1;

	CHECK(inventorySave(&in, INV_DIR));
	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));
}

static void testEmptyInventoryRoundTrips(void)
{
	freshDir();

	Inventory in;
	inventoryInit(&in);   /* every slot ITEM_NONE/0, hotbar 0 — the emptiest a real save can be */

	CHECK(inventorySave(&in, INV_DIR));
	Inventory out;
	memset(&out, 0xAA, sizeof(out));   /* poison it so a no-op load could not pass by accident */
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));
}

/* The item-id ceiling, both sides.
 *
 * v1.8.8 MOVED what "the highest id the bag can hold" means, and the old text is worth quoting
 * because it dates the defect precisely. It read:
 *
 *     BLOCK_LEAVES (BLOCK_COUNT-1) is the highest id the bag can hold
 *
 * BLOCK_LEAVES is 6. There were fifteen core rows by then. It was the highest id below the
 * CONSTANT, and this file called that the ceiling because inventoryCanHold() did.
 *
 * The ceiling is now the edge of the DEFINED table. It was fern at 14 when this case was
 * written; v1.8.8's twelve per-biome rows (ids 15..26) moved it to apple at 26, and the
 * premise check below caught the move exactly the way its own comment said it would — id 15
 * is BLOCK_BIRCH_LOG now, not an undefined row, so `!inventoryCanHold(BLOCK_FERN + 1)` went
 * red. Re-pointed to the real edge rather than patched in place, so the fixture keeps testing
 * the actual maximum rather than a row that stopped being one. The fixture carries both: an id
 * the old build could store and one it could not. The first-invalid side is covered separately
 * below, since inventoryAdd() can never put one on disk in the first place — only a hand-built
 * file can, and only the loader's defence in depth stands between that file and a slot nothing
 * has validated.
 *
 * Moved a second time, apple (26) -> torch (27), on 2026-09-02 by v1.8.10 "Light": exactly the
 * predicted case fired — `!inventoryCanHold(BLOCK_APPLE + 1)` went red because id 27 is now
 * BLOCK_TORCH, not an undefined row (inventoryCanHold() only checks defined-and-not-liquid, and
 * the torch is neither undefined nor a liquid). Re-pointed again rather than patched, same
 * reasoning as the first move.
 *
 * Moved a THIRD time, torch (27) -> diamond_ore (33), on 2026-09-03 by v1.8.12 "Ores", which
 * added ids 28..33 in one go. The prediction written into the body below — "if a future row
 * lands at 28 this goes red" — is the line that fired, verbatim:
 *
 *   FAIL  L306 !inventoryCanHold((ItemId)(BLOCK_TORCH + 1))
 *
 * and 28 is BLOCK_COAL_ORE. Worth noting that this is the fourth id to sit at "the real edge"
 * in three versions, and every single move has been caught by this pair rather than by anyone
 * remembering to look. That is the whole argument for keeping a hard-coded id on the left side
 * instead of `registryCount() - 1`: a derived bound would never go red, would never need this
 * comment, and would also never tell anyone that the table had grown. The recurring edit is
 * the feature.
 *
 * Moved a FOURTH time, diamond_ore (33) -> raw_mutton (37), on 2026-09-03 by v1.8.14 "Animals",
 * which added ids 34..37 in one go (raw_porkchop 34, raw_beef 35, raw_chicken 36, raw_mutton
 * 37). The prediction written into the body below — "if a future row lands at 34 this goes red"
 * — is the line that fired, verbatim, from a run of this stanza on its own:
 *
 *   FAIL  L319 !inventoryCanHold((ItemId)(BLOCK_DIAMOND_ORE + 1))
 *
 * and 34 is BLOCK_RAW_PORKCHOP. One failure and no others in that run ("inventory persistence
 * self-test: FAIL 106 checks, 1 failed"), which is the control: the meat rows moved the edge
 * and nothing else in this file. Re-pointed rather than patched, same reasoning as the three
 * moves above. */
static void testMaxValidItemIdRoundTrips(void)
{
	freshDir();

	/* The premise, checked rather than assumed: these really are the two sides of the edge.
	 * If a future row lands at 38 this goes red and the case gets re-pointed, instead of
	 * quietly testing the middle of the table while calling it the boundary. */
	CHECK(inventoryCanHold((ItemId)BLOCK_RAW_MUTTON));
	CHECK(!inventoryCanHold((ItemId)(BLOCK_RAW_MUTTON + 1)));

	Inventory in;
	inventoryInit(&in);
	in.slots[0] = (InvSlot){ .item = BLOCK_LEAVES, .count = INV_STACK_MAX };  /* 6: the old top */
	in.slots[1] = (InvSlot){ .item = BLOCK_RAW_MUTTON, .count = INV_STACK_MAX }; /* 37: real one */

	CHECK(inventorySave(&in, INV_DIR));
	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));
	/* Spelled out as well as compared, so a failure says WHICH id was lost rather than only
	 * that two structs differ. invEqual is the check; this is the error message. */
	CHECK(out.slots[1].item == BLOCK_RAW_MUTTON && out.slots[1].count == INV_STACK_MAX);
}

/* ── v1.8.8: the widened capacity, on disk ──────────────────────────────────────────────
 *
 * The cactus through the whole persistence path. This is the deliverable's last mile: it is not
 * enough that the bag accepts id 12 in memory — if the loader threw it away on the next boot
 * the player would watch it vanish overnight, which is the v1.6.0 "gone from both places"
 * defect with a longer fuse.
 *
 * Every check here is red against the pre-v1.8.8 loader: inventoryLoad()'s per-slot guard calls
 * inventoryCanHold(), which refused 12, so the slot came back empty and the file loaded
 * "cleanly" with the cactus silently gone. */
static void testAnIdPastTheOldCeilingRoundTrips(void)
{
	freshDir();

	Inventory in;
	inventoryInit(&in);
	in.slots[0] = (InvSlot){ .item = BLOCK_CACTUS, .count = 3 };
	in.slots[1] = (InvSlot){ .item = BLOCK_SNOW,   .count = INV_STACK_MAX };
	in.slots[2] = (InvSlot){ .item = BLOCK_ICE,    .count = 1 };
	in.slots[3] = (InvSlot){ .item = BLOCK_STONE,  .count = 9 };   /* control: an old-era id */

	CHECK(inventorySave(&in, INV_DIR));
	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));

	CHECK(out.slots[0].item == BLOCK_CACTUS && out.slots[0].count == 3);
	CHECK(out.slots[1].item == BLOCK_SNOW);
	CHECK(out.slots[2].item == BLOCK_ICE);
	CHECK(out.slots[3].item == BLOCK_STONE && out.slots[3].count == 9);
}

/* SAVE COMPATIBILITY, in the direction that matters: a file written by the build BEFORE the
 * ceiling moved must still load, unchanged, slot for slot.
 *
 * What makes this provable inside one binary rather than needing two: v1.8.8 changed a
 * PREDICATE, not a format. world/inventory.c is untouched — same magic 0x31495342, same
 * INVENTORY_VERSION 1, same 20-byte header, same 24 x (u8 item, u8 count), same crc32 over
 * bytes 16..EOF. So the byte image an old build produced is exactly the byte image buildFile()
 * produces here, and buildFile() is this suite's own writer rather than a call back into the
 * code under test.
 *
 * The contents are constrained the way an old file really was: ids 1..7 only, because that is
 * everything the old inventoryCanHold() would let into a slot. If the widened predicate had
 * accidentally NARROWED anywhere — an off-by-one at the bottom of the range, air handled
 * differently, a liquid rule that caught something it should not — one of these seven slots
 * comes back empty and this goes red.
 *
 * (The reverse direction is not symmetric, and is not a defect: a NEW save holding a cactus,
 * read by an OLD build, loses that one slot and keeps the rest, because inventoryLoad()'s
 * per-slot guard `continue`s rather than rejecting the file. Written down here because it is
 * the question a downgrade raises and the answer is not recorded anywhere else.) */
static void testASaveWrittenBeforeTheCeilingChangeStillLoads(void)
{
	freshDir();

	Inventory old;
	inventoryInit(&old);
	/* Every id the pre-v1.8.8 bag could hold, one per slot, with distinct counts so a slot
	 * that came back holding its NEIGHBOUR's contents is caught too. */
	old.slots[0] = (InvSlot){ .item = BLOCK_GRASS,  .count = 1 };
	old.slots[1] = (InvSlot){ .item = BLOCK_DIRT,   .count = 2 };
	old.slots[2] = (InvSlot){ .item = BLOCK_STONE,  .count = 3 };
	old.slots[3] = (InvSlot){ .item = BLOCK_SAND,   .count = 4 };
	old.slots[4] = (InvSlot){ .item = BLOCK_WOOD,   .count = 5 };
	old.slots[5] = (InvSlot){ .item = BLOCK_LEAVES, .count = 6 };
	old.slots[6] = (InvSlot){ .item = BLOCK_PLANKS, .count = 7 };
	old.selected_hotbar = 3;

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &old);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	/* Not one slot lost, not one count altered. */
	CHECK(invEqual(&out, &old));
	for (int i = 0; i < 7; i++)
		CHECK(out.slots[i].item == old.slots[i].item
		      && out.slots[i].count == old.slots[i].count);
	CHECK(out.selected_hotbar == 3);

	/* And the version stamp did NOT move. This is what catches a "compatible" change that
	 * quietly re-versioned the file: it must be the same format, not a new one that happens
	 * to be able to read the old one. */
	CHECK(EXPECT_VERSION == 1u);
	CHECK(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
}

/* ── 3. every degradation path the header check ────────────────────────────────────── */

static void testMissingFileGivesEmptyInventory(void)
{
	freshDir();

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));   /* missing file is not an error — see inventory.h */

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

static void testBadMagicGivesEmptyInventory(void)
{
	freshDir();
	const Inventory bad = refInventory();
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, 0x31505342u /* "BSP1", the pose sidecar's */, EXPECT_VERSION,
	          (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

static void testBadVersionGivesEmptyInventory(void)
{
	freshDir();
	const Inventory bad = refInventory();
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, 2u, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

/* Unique to inventory.c among this tree's save formats: the slot count is checked
 * separately from the CRC precisely so "this file was written by a build with a
 * different INV_SLOT_COUNT" reads as what it is — see inventory.c's own header comment. */
static void testBadSlotCountGivesEmptyInventory(void)
{
	freshDir();
	const Inventory bad = refInventory();
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT + 1, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

static void testBadCrcGivesEmptyInventory(void)
{
	freshDir();
	const Inventory bad = refInventory();
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	buf[21] ^= 0x01u;   /* one bit inside slot 0's count, after the crc was computed */
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

/* Measured, and recorded rather than assumed: dropping the length check alone does not
 * turn this red, because a file short by even one byte loses payload the CRC covers —
 * exactly the finding world/playerpose_test.c records about its own length check
 * ("defence in depth behind the checksum"). Sabotaging BOTH together is the arm that
 * proves the length check does real work; see this suite's own report for the numbers. */
static void testOneByteShortGivesEmptyInventory(void)
{
	freshDir();

	/* Every slot occupied, not just refInventory()'s three — a truncation that lands on
	 * the LAST slot's count byte (the one byte actually dropped here) must corrupt a
	 * live slot, not an already-empty one, or a sabotaged length check could pass this
	 * check by luck: the missing byte would belong to a slot the defence-in-depth guard
	 * (item == ITEM_NONE) was already going to zero regardless of what that byte held. */
	Inventory bad;
	inventoryInit(&bad);
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		bad.slots[i] = (InvSlot){ .item = BLOCK_STONE, .count = 50 };

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES - 1));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));
}

/* Gap 1 (closed): a file LONGER than the record must be refused, not silently
 * truncated-read. Before this fix, inventoryLoad() read exactly sizeof(buf)
 * (TEST_FILE_BYTES) bytes into a fixed buffer and only checked `n != sizeof(buf)` — so a
 * file longer than the record read its first TEST_FILE_BYTES bytes as a perfectly good
 * save and the trailing bytes were never looked at, never counted, never refused.
 * world/playerpose.c's playerPoseLoad() already reads one byte more than its own record
 * specifically so a longer file is refused too, and its own comment named this exact gap
 * in inventory.c. Sabotage: append one extra byte (0x7F) past an otherwise fully valid,
 * checksummed 68-byte record. */
static void testTrailingBytesAreRejected(void)
{
	freshDir();
	const Inventory bad = refInventory();
	uint8_t buf[TEST_FILE_BYTES + 1];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	buf[TEST_FILE_BYTES] = 0x7Fu;   /* one extra byte past a fully valid record */
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES + 1));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));   /* the documented contract: still returns true */

	Inventory empty;
	inventoryInit(&empty);
	CHECK(invEqual(&out, &empty));   /* but the payload is refused, same as any other bad file */
}

/* ── 4. per-slot defence in depth ──────────────────────────────────────────────────── */
/*
 * These files are byte-for-byte self-consistent — the CRC is computed over the exact
 * bytes written, out-of-range id and all — so nothing above this layer can catch them.
 * Only inventoryLoad()'s own `if (!inventoryCanHold(item) || count == 0) continue;`
 * stands between a hand-built (or hostile, or bit-flipped-and-lucky) file and a slot
 * state nothing else has ever validated. inventoryAdd()/inventorySave() can never
 * produce any of these on their own — that is the whole reason a hand-built file is
 * the only way to reach this code at all.
 */

/* An id the bag cannot hold must drop to empty rather than hand the rest of the game an item
 * id nothing registered, and every OTHER slot in the same file must still load normally: this
 * is a per-slot fallback, not a whole-file refusal.
 *
 * v1.8.8 had to REPOINT this case, and the reason is the interesting part. It used to say "an
 * id at or past BLOCK_COUNT — BLOCK_WATER is the first one" and it used water for the fixture.
 * Water is still refused, so this case would still have passed verbatim — for a completely
 * different reason. It is refused now for being a LIQUID, and the fact that liquid happens to
 * sit at id 8, exactly where the old constant was, is a coincidence of the table. A check that
 * keeps passing while the reason underneath it changes is the most expensive kind to leave
 * alone, so BOTH reasons are now spelled out, each with its own slot:
 *
 *   slot 0  an UNDEFINED id — the real "past the end of the table" case, and the one that
 *           actually reaches a player: a server whose registry this client never received.
 *   slot 2  a LIQUID — defined, in range, and still refused, because a bucket of water is not
 *           a thing this game has.
 *
 * A predicate that had simply been widened to `id < registryCount()` passes on slot 2 and
 * fails on nothing; a predicate that dropped the definedness test passes slot 2 and fails
 * slot 0. Neither can be green here. */
static void testOutOfRangeItemIdSlotIsDropped(void)
{
	freshDir();
	Inventory bad;
	inventoryInit(&bad);
	/* Premise, so the fixture cannot rot into "an id that happens to be defined after all". */
	CHECK(!inventoryCanHold((ItemId)0x40));
	CHECK(!inventoryCanHold((ItemId)BLOCK_WATER));

	bad.slots[0] = (InvSlot){ .item = 0x40,        .count = 10 };   /* undefined: no row at all */
	bad.slots[1] = (InvSlot){ .item = BLOCK_DIRT,  .count = 7  };   /* neighbour: must still load */
	bad.slots[2] = (InvSlot){ .item = BLOCK_WATER, .count = 5  };   /* defined, but a liquid */
	bad.slots[3] = (InvSlot){ .item = BLOCK_CACTUS, .count = 2 };   /* past the OLD ceiling: keep */

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));   /* still a clean load, not a refusal */
	CHECK(out.slots[0].item == ITEM_NONE && out.slots[0].count == 0);
	CHECK(out.slots[1].item == BLOCK_DIRT && out.slots[1].count == 7);
	CHECK(out.slots[2].item == ITEM_NONE && out.slots[2].count == 0);
	/* The other half of the same file, and the half that makes this a test of the NEW rule
	 * rather than of a rule that refuses everything unfamiliar. */
	CHECK(out.slots[3].item == BLOCK_CACTUS && out.slots[3].count == 2);
}

/* ITEM_NONE (== BLOCK_AIR == 0) paired with a nonzero count: the exact malformed slot
 * inventory.h's InvSlot comment says can never happen through the normal API ("0 iff
 * item == ITEM_NONE"), and the one a torn or hand-edited file could still contain. */
static void testItemNoneWithNonzeroCountIsDropped(void)
{
	freshDir();
	Inventory bad;
	inventoryInit(&bad);
	bad.slots[0] = (InvSlot){ .item = ITEM_NONE, .count = 50 };
	bad.slots[1] = (InvSlot){ .item = BLOCK_SAND, .count = 9 };

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(out.slots[0].item == ITEM_NONE && out.slots[0].count == 0);
	CHECK(out.slots[1].item == BLOCK_SAND && out.slots[1].count == 9);
}

/* The other half of the same OR: a VALID item id paired with a zero count. Not covered
 * by the ITEM_NONE case above — this is inventoryCanHold(item) succeeding and the
 * `count == 0` half of the guard doing the work alone. */
static void testValidItemWithZeroCountIsDropped(void)
{
	freshDir();
	Inventory bad;
	inventoryInit(&bad);
	bad.slots[0] = (InvSlot){ .item = BLOCK_WOOD, .count = 0 };
	bad.slots[1] = (InvSlot){ .item = BLOCK_STONE, .count = 4 };

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(out.slots[0].item == ITEM_NONE && out.slots[0].count == 0);
	CHECK(out.slots[1].item == BLOCK_STONE && out.slots[1].count == 4);
}

/* A count past INV_STACK_MAX (99) — the byte itself can hold up to 255 — must clamp
 * down rather than hand the rest of the game (and the touchscreen count badge) a stack
 * size nothing enforces. */
static void testOversizedCountIsClamped(void)
{
	freshDir();
	Inventory bad;
	inventoryInit(&bad);
	bad.slots[0] = (InvSlot){ .item = BLOCK_GRASS, .count = 200 };

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(out.slots[0].item  == BLOCK_GRASS);
	CHECK(out.slots[0].count == INV_STACK_MAX);
}

/* selected_hotbar past INV_HOTBAR_SLOTS (8) clamps to 0 rather than being carried
 * through — inventoryHeldItem()/inventoryHeldCount() index slots[] with it directly,
 * with no bounds check of their own, so an unclamped value here would be an
 * out-of-bounds read the very next time either is called. */
static void testHotbarSelectionClampsToZero(void)
{
	freshDir();
	Inventory bad;
	inventoryInit(&bad);
	bad.selected_hotbar = 250;
	bad.slots[0] = (InvSlot){ .item = BLOCK_DIRT, .count = 1 };

	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &bad);
	CHECK(writeBytes(invFile(), buf, TEST_FILE_BYTES));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(out.selected_hotbar == 0);
}

/* ── 5. the interrupted-save window ─────────────────────────────────────────────────── */

static void testTmpIsPromotedWhenRealFileGone(void)
{
	freshDir();
	const Inventory in = refInventory();
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &in);
	CHECK(writeBytes(invTmp(), buf, TEST_FILE_BYTES));   /* power cut between remove and rename */

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &in));
	CHECK(fileExists(invFile()));
	CHECK(!fileExists(invTmp()));
}

static void testGoodRealFileBeatsLeftoverTmp(void)
{
	freshDir();

	const Inventory real = refInventory();
	CHECK(inventorySave(&real, INV_DIR));

	Inventory stale;
	inventoryInit(&stale);
	stale.slots[0] = (InvSlot){ .item = BLOCK_SAND, .count = 77 };
	uint8_t buf[TEST_FILE_BYTES];
	buildFile(buf, EXPECT_MAGIC, EXPECT_VERSION, (uint32_t)INV_SLOT_COUNT, &stale);
	CHECK(writeBytes(invTmp(), buf, TEST_FILE_BYTES));

	Inventory out;
	inventoryInit(&out);
	CHECK(inventoryLoad(&out, INV_DIR));
	CHECK(invEqual(&out, &real));   /* the real file, not the stale tmp */
	CHECK(!fileExists(invTmp()));
}

/* ── 6. NULL guards and a directory nothing can be written to ──────────────────────── */

/* ⚠ Belt-and-braces, same disclosure world/playerpose_test.c makes about its own
 * equivalent: the NULL checks are real — with `!inv`/`!world_dir` removed,
 * inventoryLoad(NULL, dir) and inventorySave(&inv, NULL) segfault a binary that was
 * otherwise passing, and a crashed binary is as red as a failed check. */
static void testNullGuards(void)
{
	freshDir();
	const Inventory in = refInventory();

	CHECK(!inventorySave(NULL, INV_DIR));
	CHECK(!inventorySave(&in, NULL));
	CHECK(!fileExists(invFile()));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(!inventoryLoad(&out, NULL));
	/* NULL world_dir leaves *out untouched, same contract inventory.h documents for
	 * every other refusal. */
	CHECK(out.slots[0].item == 0xAA);

	/* Called against a directory that really does hold a valid save, which is the
	 * whole difference between a check and a decoration — see the equivalent note in
	 * world/playerpose_test.c. */
	CHECK(inventorySave(&in, INV_DIR));
	CHECK(!inventoryLoad(NULL, INV_DIR));
}

/* Gap 2 (closed): an empty world_dir must be refused exactly like a NULL one. Before this
 * fix, inventory.c's inventorySave/inventoryLoad only checked `!world_dir`, so an empty
 * string sailed straight through "%s/%s" and resolved to "/inventory.dat" — this
 * process's filesystem root, not the sandboxed world directory every other path in this
 * file writes under. world/playerpose.c's dirUsable() already refuses both NULL and
 * empty and says why: "an empty one is a caller that has lost track of which world it is
 * in". inventory.c's own dirUsable() now mirrors it. Sabotage: call both functions with
 * world_dir == "". */
static void testEmptyWorldDirIsRefused(void)
{
	remove("/inventory.dat");   /* in case a stale probe from before this fix left one */

	const Inventory in = refInventory();
	CHECK(!inventorySave(&in, ""));
	CHECK(!fileExists("/inventory.dat"));

	Inventory out;
	memset(&out, 0xAA, sizeof(out));
	CHECK(!inventoryLoad(&out, ""));
	/* Empty world_dir leaves *out untouched, same contract as a NULL world_dir. */
	CHECK(out.slots[0].item == 0xAA);

	remove("/inventory.dat");   /* best-effort cleanup, in case anything still landed */
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== inventory persistence test ==");

	testControlCleanSaveLoad();
	testRoundTripIsBitExact();

	testFullInventoryRoundTrips();
	testEmptyInventoryRoundTrips();
	testMaxValidItemIdRoundTrips();

	/* v1.8.8 — the widened item ceiling, and the save-compatibility proof that goes with it. */
	testAnIdPastTheOldCeilingRoundTrips();
	testASaveWrittenBeforeTheCeilingChangeStillLoads();

	testMissingFileGivesEmptyInventory();
	testBadMagicGivesEmptyInventory();
	testBadVersionGivesEmptyInventory();
	testBadSlotCountGivesEmptyInventory();
	testBadCrcGivesEmptyInventory();
	testOneByteShortGivesEmptyInventory();
	testTrailingBytesAreRejected();

	testOutOfRangeItemIdSlotIsDropped();
	testItemNoneWithNonzeroCountIsDropped();
	testValidItemWithZeroCountIsDropped();
	testOversizedCountIsClamped();
	testHotbarSelectionClampsToZero();

	testTmpIsPromotedWhenRealFileGone();
	testGoodRealFileBeatsLeftoverTmp();

	testNullGuards();
	testEmptyWorldDirIsRefused();

	/* ---- check-count guard -----------------------------------------------------------------
	 *
	 * This suite counts failures, and until 2026-08-25 that was ALL it counted. A suite that
	 * only counts failures cannot notice checks that never ran. Measured on net/networld_test.c
	 * the same day: shrinking one production constant took it from "PASS 326 checks, 0 failed"
	 * to "PASS 318 checks, 0 failed" — both green, exit 0, eight checks silently DELETED rather
	 * than failed.
	 *
	 * This file's own deletion shape is the `if (n == TEST_FILE_BYTES)` arm in the round-trip
	 * test: the byte-for-byte comparison against the documented on-disk layout only runs if the
	 * file that inventorySave() produced came back the expected length. A writer that emits the
	 * wrong number of bytes therefore turns the layout check OFF rather than red — the one check
	 * that goes red says the length is wrong, and the check that would have said WHICH BYTES are
	 * wrong is the one that disappears. That is the most useful check in the file deleting
	 * itself at the exact moment it was needed.
	 *
	 * So: the number below is the count of checks that must already have run by the time control
	 * reaches this line. It is a naked literal on purpose — it is the one number in this file
	 * that is not derived from anything the tests themselves compute, which is precisely what
	 * lets it notice them vanishing. A count computed from INV_SLOT_COUNT or TEST_FILE_BYTES
	 * would move with the very code it is supposed to be watching; that self-reference is the
	 * bug that let networld_test.c's 326 -> 318 hide.
	 *
	 * HOW TO UPDATE IT WHEN YOU ADD OR REMOVE CHECKS — read this before changing the number:
	 *   Work out the delta from what you actually changed (checks added minus checks removed)
	 *   and ADD THAT DELTA to the number below. Do NOT paste whatever the failing run printed.
	 *   Pasting the observed count is the single failure mode this guard exists to catch: if a
	 *   production change silently deleted checks, the printed count is the SYMPTOM, and copying
	 *   it in here re-arms the trap and throws away the only evidence you had. If your
	 *   recomputed delta and the observed count disagree, that disagreement is a bug report — go
	 *   and find out which checks stopped running, and why.
	 *
	 *   Note the number is the count BEFORE this guard itself, so the summary line prints one
	 *   more than it (78 here, 79 on the PASS line). That off-by-one is deliberate: it means
	 *   blind-pasting the number off the PASS line lands you a red, not a false green.
	 *
	 *   Latched into `ran` first, and compared through that, so the pin means "checks before
	 *   this line" no matter how the check machinery is spelled. It matters: the macro-based
	 *   suites in this fleet (world/region_growth_test.c, app/session_test.c) increment the
	 *   counter BEFORE evaluating the condition, and a guard written against the live counter
	 *   there silently wants a number one higher. This file's CHECK expands to a checkAt()
	 *   CALL, so it does not have that hazard today — the latch is what keeps it from
	 *   acquiring one if the macro is ever rewritten to increment inline. */
	/* 78 -> 105 on 2026-09-02 (v1.8.8). Arrived at by DELTA, exactly as the paragraph above
	 * insists, and written out so the arithmetic can be disagreed with:
	 *
	 *   testMaxValidItemIdRoundTrips            3 -> 6   (+3)  two premise checks, one
	 *                                                          spell-out of the fern slot
	 *   testAnIdPastTheOldCeilingRoundTrips     0 -> 7   (+7)  new
	 *   testASaveWrittenBeforeTheCeilingChange  0 -> 13  (+13) new; 7 of those are the loop
	 *                                                          over the seven old-era slots
	 *   testOutOfRangeItemIdSlotIsDropped       4 -> 8   (+4)  two premise checks, plus the
	 *                                                          liquid and cactus slots
	 *
	 *   +3 +7 +13 +4 = +27, and 78 + 27 = 105.
	 *
	 * The observed count from the run agreed. That agreement is the point of doing it this way
	 * round: had it printed anything other than 105, the number to chase would have been the
	 * discrepancy, not the pin. */
	const int ran = g_checks;
	if (ran != 105)
		printf("\nCHECK-COUNT GUARD: %d checks ran, %d expected.\n"
		       "  %s\n"
		       "  This is NOT an ordinary assertion failure.\n"
		       "  Read the comment above this guard in world/inventory_persist_test.c before"
		       " touching the pinned number.\n",
		       ran, 105,
		       ran < 105
		           ? "Checks went MISSING: checks that should have run never ran at all."
		           : "Extra checks appeared: either you added checks and did not update the"
		             " pin, or something is emitting checks it should not.");
	CHECK(ran == 105);

	printf("\ninventory persistence self-test: %s %d checks, %d failed\n",
	       g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
