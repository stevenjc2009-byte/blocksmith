#include "world/blockstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/crc32.h"

// Nothing in here includes <3ds.h>. Same reasoning as world/region.c, world/inventory.c and
// world/daynight.c: libctru mounts the SD card as a devoptab under "sdmc:/", so
// fopen("sdmc:/.../blockstate.dat") on console and fopen("build-host/.../blockstate.dat") on
// the host go through the exact same code below -- the only reason the crash-recovery path
// (blockStateRecover) can be exercised by a host test at all.

void blockStateInit(BlockStateTable* t)
{
	if (!t) return;
	// Zero gives every slot block_id == BLOCK_AIR, i.e. every slot free -- see blockstate.h's
	// free-slot trick.
	memset(t, 0, sizeof(*t));
}

// Shared by every lookup below: the slot at (x, y, z), or NULL if none is live there. Linear
// scan over BLOCKSTATE_SLOTS -- the same cost shape docs/plan-1.8.15-furnace.md's own
// furnaceTick design already accepts ("O(64) comparisons worst case, per tick, regardless of
// render radius"), reused here for the same reason: 64 is small enough that a hash table
// would be more code for no measurable win, on hardware where the win would matter least.
static BlockStateEntry* findSlot(BlockStateTable* t, int x, int y, int z)
{
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++) {
		BlockStateEntry* e = &t->slots[i];
		if (e->block_id != BLOCK_AIR && e->x == x && e->y == y && e->z == z)
			return e;
	}
	return NULL;
}

static const BlockStateEntry* findSlotConst(const BlockStateTable* t, int x, int y, int z)
{
	// Same body as findSlot, over a const table -- kept separate rather than cast away the
	// const, so blockStateGet (which only reads) cannot accidentally write through this call.
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++) {
		const BlockStateEntry* e = &t->slots[i];
		if (e->block_id != BLOCK_AIR && e->x == x && e->y == y && e->z == z)
			return e;
	}
	return NULL;
}

static BlockStateEntry* findFreeSlot(BlockStateTable* t)
{
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++)
		if (t->slots[i].block_id == BLOCK_AIR) return &t->slots[i];
	return NULL;
}

bool blockStateCreate(BlockStateTable* t, int x, int y, int z, BlockId block_id)
{
	if (!t || block_id == BLOCK_AIR) return false;

	// Reset-in-place if a record already stands here (see blockstate.h's header for why this
	// is deliberate rather than a refusal), otherwise claim a free slot.
	BlockStateEntry* e = findSlot(t, x, y, z);
#ifdef BS_SABOTAGE_NO_RESET_ON_CREATE
	// Sabotage arm for blockstate_test.c: pretend success on an existing record without
	// resetting it, i.e. exactly the "resurrection" bug this file's header says defence 1
	// exists to prevent. Compile-time only -- never built by default.
	if (e) return true;
#endif
	if (!e) e = findFreeSlot(t);
#ifdef BS_SABOTAGE_NO_CAPACITY_CHECK
	// Sabotage arm: when the table is genuinely full and this position is new, silently
	// clobber slot 0 instead of refusing -- the bug a missing capacity check would look like.
	if (!e) e = &t->slots[0];
#else
	if (!e) return false;   // table full, and nothing existing here to reclaim
#endif

	e->x = x; e->y = y; e->z = z;
	e->block_id = block_id;
	memset(e->data, 0, sizeof(e->data));
	return true;
}

void blockStateRemove(BlockStateTable* t, int x, int y, int z)
{
	if (!t) return;
	BlockStateEntry* e = findSlot(t, x, y, z);
	if (!e) return;   // nothing there: silent no-op, matching free(NULL)
	memset(e, 0, sizeof(*e));   // block_id becomes BLOCK_AIR: slot is free again
}

bool blockStateGet(const BlockStateTable* t, int x, int y, int z, BlockId expect_block_id,
                    uint8_t out[BLOCKSTATE_PAYLOAD_BYTES])
{
	if (!t) return false;
	const BlockStateEntry* e = findSlotConst(t, x, y, z);
#ifdef BS_SABOTAGE_NO_ID_CHECK
	// Sabotage arm: drop the expect_block_id check, so a record left behind by one block kind
	// reads back as another's state -- the cross-kind leak defence 2 exists to stop.
	(void)expect_block_id;
	if (!e) return false;
#else
	if (!e || e->block_id != expect_block_id) return false;
#endif
	memcpy(out, e->data, BLOCKSTATE_PAYLOAD_BYTES);
	return true;
}

bool blockStateSet(BlockStateTable* t, int x, int y, int z, BlockId expect_block_id,
                    const uint8_t data[BLOCKSTATE_PAYLOAD_BYTES])
{
	if (!t) return false;
	BlockStateEntry* e = findSlot(t, x, y, z);
#ifdef BS_SABOTAGE_NO_ID_CHECK
	(void)expect_block_id;
	if (!e) return false;
#else
	if (!e || e->block_id != expect_block_id) return false;
#endif
	memcpy(e->data, data, BLOCKSTATE_PAYLOAD_BYTES);
	return true;
}

int blockStateCount(const BlockStateTable* t)
{
	if (!t) return 0;
	int n = 0;
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++)
		if (t->slots[i].block_id != BLOCK_AIR) n++;
	return n;
}

// ── Save / load ────────────────────────────────────────────────────────────────────────
//
// On-disk record layout is written field by field, exactly like world/inventory.c's
// put32/get32 and world/region.c's before it -- never a raw fwrite of BlockStateEntry --
// because this file's own header already explains why a struct's in-memory layout (and
// even its size) is not portable between this project's host test build and its ARM EABI
// console build. The file format is therefore its own contract, independent of the struct.
//
// Header:  magic(4) version(4) count(4) crc(4)                              = 16 bytes
// Record:  x(4) y(4) z(4) block_id(1) reserved(3) data(BLOCKSTATE_PAYLOAD_BYTES)
//                                                                            = 16 + BLOCKSTATE_PAYLOAD_BYTES
// `count` live records follow the header, in slot order. Only live (block_id != BLOCK_AIR)
// slots are written -- an all-empty table saves as a 16-byte file, count 0.

#define BLOCKSTATE_FILE_NAME "blockstate.dat"

// "BSBX" (Blocksmith Block eXtra-state), byte-wise so the file is endian-independent by
// construction -- world/genversion.c's and world/worldseed.c's own argument for their magics.
static const uint8_t BLOCKSTATE_MAGIC[4] = {'B', 'S', 'B', 'X'};

// Bumped only for a format change an old loader could misread -- same rule world/region.h's
// REGION_VERSION and world/inventory.c's INVENTORY_VERSION both state for themselves. A file
// with a version this build does not recognise is treated as absent (blockStateLoad falls
// back to empty), never guessed at.
#define BLOCKSTATE_VERSION  1u

#define BS_HDR_BYTES     16
#define BS_RECORD_BYTES  (16 + BLOCKSTATE_PAYLOAD_BYTES)
#define BS_FILE_BYTES(count)  (BS_HDR_BYTES + (size_t)(count) * BS_RECORD_BYTES)

static void put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool dirUsable(const char* world_dir)
{
	// Mirrors world/inventory.c's dirUsable(): a NULL directory is a caller bug, an empty one
	// is a caller that has lost track of which world it is in, and neither should resolve
	// "%s/%s" into this process's filesystem root instead of a sandboxed world directory.
	return world_dir != NULL && world_dir[0] != '\0';
}

static bool blockStatePath(char* out, size_t cap, const char* world_dir)
{
	return snprintf(out, cap, "%s/%s", world_dir, BLOCKSTATE_FILE_NAME) < (int)cap;
}

static bool tmpPath(const char* path, char* out, size_t cap)
{
	return snprintf(out, cap, "%s.tmp", path) < (int)cap;
}

bool blockStateSave(const BlockStateTable* t, const char* world_dir)
{
	if (!t || !dirUsable(world_dir)) return false;

	char path[512], tmp[512];
	if (!blockStatePath(path, sizeof(path), world_dir)) return false;
	if (!tmpPath(path, tmp, sizeof(tmp))) return false;

	uint32_t count = 0;
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++)
		if (t->slots[i].block_id != BLOCK_AIR) count++;

	const size_t total = BS_FILE_BYTES(count);
	// BLOCKSTATE_SLOTS was a compile-time 64 when this buffer was a stack array (`total`
	// topped out at 16 + 64*32 = 2,080 bytes -- fixed and small enough for a stack frame).
	// Raised to 256 for chests (see blockstate.h), `total` now tops out at 16 + 256*32 =
	// 8,208 bytes -- too much to spend on one frame of libctru's 32 KB default thread stack
	// (an over-large local silently kills the function on this platform, no crash), so this
	// is heap-allocated instead. Freed on every exit path from here on.
	uint8_t* buf = malloc(BS_FILE_BYTES(BLOCKSTATE_SLOTS));
	if (!buf) return false;   // cannot proceed without the buffer: same as any other IO failure

	size_t w = BS_HDR_BYTES;
	for (int i = 0; i < BLOCKSTATE_SLOTS; i++) {
		const BlockStateEntry* e = &t->slots[i];
		if (e->block_id == BLOCK_AIR) continue;

		put32(buf + w + 0, (uint32_t)e->x);
		put32(buf + w + 4, (uint32_t)e->y);
		put32(buf + w + 8, (uint32_t)e->z);
		buf[w + 12] = e->block_id;
		buf[w + 13] = buf[w + 14] = buf[w + 15] = 0;
		memcpy(buf + w + 16, e->data, BLOCKSTATE_PAYLOAD_BYTES);
		w += BS_RECORD_BYTES;
	}

	const uint32_t crc = crc32(buf + BS_HDR_BYTES, total - BS_HDR_BYTES);
	put32(buf + 0,  get32(BLOCKSTATE_MAGIC));
	put32(buf + 4,  BLOCKSTATE_VERSION);
	put32(buf + 8,  count);
	put32(buf + 12, crc);

	FILE* f = fopen(tmp, "wb");
	if (!f) { free(buf); return false; }

	const bool wrote_ok = fwrite(buf, 1, total, f) == total;
	free(buf);

	// fclose is the flush: this is what makes the bytes actually reach the card rather than
	// sitting in stdio's buffer when the rename below runs -- same reasoning as
	// world/inventory.c's inventorySave.
	if (fclose(f) != 0 || !wrote_ok) { remove(tmp); return false; }

	// Windows' rename() refuses to replace an existing destination, so the old file has to go
	// first -- same reason world/inventory.c's inventorySave, app/options.c's optionsSave and
	// world/region.c's regionCompact all remove before they rename. remove() failing because
	// `path` does not exist yet (the very first save) is expected, not a failure here.
	remove(path);

	if (rename(tmp, path) != 0) return false;

	// The window this leaves -- a power cut between the remove and the rename -- is closed by
	// blockStateRecover below the next time anything tries to load this path, same as
	// world/inventory.c's inventoryRecover closes it for inventory.dat.
	return true;
}

// Mirrors world/inventory.c's inventoryRecover / app/options.c's optionsRecover / world/
// region.c's regionRecover: if the last save was cut between removing the old file and
// renaming the new one into place, `path` is gone and `path.tmp` is a complete, unopened
// replacement. Promoting it here means blockStateLoad never has to tell "never saved" apart
// from "saved, then interrupted right after".
static void blockStateRecover(const char* path)
{
	char tmp[512];
	if (!tmpPath(path, tmp, sizeof(tmp))) return;

	FILE* tf = fopen(tmp, "rb");
	if (!tf) return;             // no interrupted save to recover
	fclose(tf);

	FILE* real = fopen(path, "rb");
	if (real) { fclose(real); remove(tmp); return; }   // real file is fine; drop the leftover tmp

	rename(tmp, path);
}

#ifndef __3DS__
// Test-only hook: forces the next blockStateLoad's read-buffer allocation to behave exactly
// as an out-of-memory malloc does, so the allocation-failure path a few lines below can be
// entered on purpose instead of only by actually exhausting the heap. Mirrors world/light.c's
// lightFailEditQueueForTest for the same reason: a failure path that cannot be forced to run
// is unproven, not merely rare. Nothing in production sets this; compiled out of the console
// build entirely (see the declaration in blockstate.h) rather than merely left unused there.
static bool s_fail_load_alloc_for_test;

void blockStateFailLoadAllocForTest(bool fail) { s_fail_load_alloc_for_test = fail; }
#endif

bool blockStateLoad(BlockStateTable* t, const char* world_dir)
{
	if (!t || !dirUsable(world_dir)) return false;

	// Always start from a fully valid, empty table. Every early return below leaves this in
	// place, so "file missing", "file corrupt", "file from an incompatible build" and "file
	// truncated mid-record" all degrade to the same safe result rather than four different
	// failure modes the rest of the game would have to know about.
	blockStateInit(t);

	char path[512];
	if (!blockStatePath(path, sizeof(path), world_dir)) return true;   // path too long: defaults

	blockStateRecover(path);

	FILE* f = fopen(path, "rb");
	if (!f) return true;   // missing file is not an error -- see blockstate.h

	// BS_FILE_BYTES(BLOCKSTATE_SLOTS) + 1: one byte more than the largest legal file, so a
	// file LONGER than any build could have written is refused too, the same reasoning world/
	// inventory.c's inventoryLoad and world/playerpose.c's playerPoseLoad apply to their own
	// records -- a file longer than anything this format can produce is as wrong as a short
	// one, not something to read the first N bytes of and ignore the rest.
	//
	// Heap-allocated for the same reason blockStateSave's buffer is (see there): at the
	// raised BLOCKSTATE_SLOTS = 256 this is 8,209 bytes, too much for one frame of libctru's
	// 32 KB default thread stack.
	//
	// v1.9.x fix: a failed malloc here used to `return true` -- leave `t` at the zeroed
	// default blockStateInit already set above, but report it as if that were a genuine,
	// successful "no file / corrupt file" default. It is not: a real, good file may be
	// sitting right there on the card, unread. Reporting success let a later blockStateSave
	// write this empty table straight over it -- silent, irreversible data loss for every
	// furnace and chest in the world, and it takes only memory pressure at the wrong moment
	// to trigger, no corruption or missing file required. false is the honest answer: the
	// load did not happen. See blockstate.h's comment on this function for why this is safe
	// to change (nothing anywhere branches on false meaning "no file, use the empty table")
	// and for what still has to happen in main.c before this stops the clobber end to end.
#ifndef __3DS__
	uint8_t* buf = s_fail_load_alloc_for_test ? NULL
	                                           : malloc(BS_FILE_BYTES(BLOCKSTATE_SLOTS) + 1);
#else
	uint8_t* buf = malloc(BS_FILE_BYTES(BLOCKSTATE_SLOTS) + 1);
#endif
	if (!buf) { fclose(f); return false; }

	const size_t got = fread(buf, 1, BS_FILE_BYTES(BLOCKSTATE_SLOTS) + 1, f);
	fclose(f);

	if (got < BS_HDR_BYTES) { free(buf); return true; }   // too short even for a header: defaults
	if (get32(buf + 0) != get32(BLOCKSTATE_MAGIC)) { free(buf); return true; }
	if (get32(buf + 4) != BLOCKSTATE_VERSION)      { free(buf); return true; }

	const uint32_t count = get32(buf + 8);
	if (count > (uint32_t)BLOCKSTATE_SLOTS) { free(buf); return true; }   // more records than this build allows

	const size_t want = BS_FILE_BYTES(count);
	if (got != want) { free(buf); return true; }   // short (truncated) or long (trailing garbage): defaults

#ifndef BS_SABOTAGE_NO_CRC_CHECK
	const uint32_t want_crc = get32(buf + 12);
	if (crc32(buf + BS_HDR_BYTES, want - BS_HDR_BYTES) != want_crc) { free(buf); return true; }   // torn write
#endif
	// Sabotage arm BS_SABOTAGE_NO_CRC_CHECK: skip crc validation entirely, so a corrupted
	// payload loads as if it were fine -- the exact bug the crc field exists to catch.

	size_t r = BS_HDR_BYTES;
	for (uint32_t i = 0; i < count; i++) {
		const int32_t  x  = (int32_t)get32(buf + r + 0);
		const int32_t  y  = (int32_t)get32(buf + r + 4);
		const int32_t  z  = (int32_t)get32(buf + r + 8);
		const BlockId  id = buf[r + 12];

		// A corrupt record claiming BLOCK_AIR (the free-slot sentinel) would silently vanish
		// on the very next blockStateCreate scan and is not a value this format ever writes
		// itself -- reject the whole file rather than load a table with a landmine in it.
		if (id == BLOCK_AIR) { blockStateInit(t); free(buf); return true; }

		// Two records at the same position is not a value this format ever writes itself
		// either (blockStateSave walks live slots, which are unique by construction — see
		// findSlot). Loading it anyway would make blockStateGet/Set/Remove see only whichever
		// one findSlot happens to reach first, leaving the other a permanently-inaccessible
		// slot that still counts against BLOCKSTATE_SLOTS -- not a leak this table's own API
		// can cause, but a corrupt file could, so it is rejected the same as any other
		// malformed record rather than silently tolerated.
		bool dup = false;
		for (uint32_t j = 0; j < i; j++) {
			if (t->slots[j].x == x && t->slots[j].y == y && t->slots[j].z == z) {
				dup = true;
				break;
			}
		}
		if (dup) { blockStateInit(t); free(buf); return true; }

		BlockStateEntry* e = &t->slots[i];
		e->x = x; e->y = y; e->z = z;
		e->block_id = id;
		memcpy(e->data, buf + r + 16, BLOCKSTATE_PAYLOAD_BYTES);
		r += BS_RECORD_BYTES;
	}

	free(buf);
	return true;
}
