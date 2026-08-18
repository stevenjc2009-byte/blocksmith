#include "world/region.h"

#include <stdio.h>
#include <string.h>

#include "world/crc32.h"

// Nothing in here includes <3ds.h>. libctru's SD card is a devoptab mounted under "sdmc:/",
// which means plain stdio reaches it — so this file is ordinary C against fopen/fread/fseek
// and compiles and runs on the host with a temp directory in place of the card. That is the
// only reason the power-cut behaviour can be tested at all: the host test truncates a real
// file at every byte offset and reloads it, which is not something an emulator run can do.

// ── On-disk layout ────────────────────────────────────────────────────────────────────
//
//   0x0000  directory copy A
//   0x0C10  directory copy B
//   0x1820  payload arena, append only
//
// A directory copy is a 16-byte header followed by 256 entries of 12 bytes.

#define DIR_MAGIC     0x31525342u    // "BSR1" little-endian
#define DIR_HDR_BYTES REGION_DIR_HDR_BYTES
#define DIR_ENT_BYTES REGION_DIR_ENT_BYTES
#define DIR_BYTES     REGION_DIR_BYTES                 // 3088
#define DIR_A_OFF     0u
#define DIR_B_OFF     ((uint32_t)DIR_BYTES)
#define ARENA_OFF     ((uint32_t)REGION_ARENA_OFF)

// One column's slot in the directory. len 0 means "never saved", which is distinct from
// "saved as empty" — an empty column still encodes to a few bytes.
typedef struct {
	uint32_t off;    // absolute byte offset of the payload, 0 when unused
	uint32_t len;
	uint32_t crc;    // of the payload bytes
} DirEnt;

typedef struct {
	uint32_t seq;               // higher wins; 0 means this copy was never written
	uint32_t arena_end;         // first free byte of the arena
	DirEnt   ent[REGION_COLS];
	bool     valid;
} Dir;

// Little-endian on both the ARM11 and the host, but written and read a byte at a time
// anyway. A save file that only loads on the machine that wrote it is a trap waiting for
// the first time a world is copied off the card.
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

static int slotOf(int32_t cx, int32_t cz)
{
	// Mask, not modulo: modulo of a negative column would give a negative slot.
	return (int)((cz & (REGION_DIM - 1)) * REGION_DIM + (cx & (REGION_DIM - 1)));
}

static void regionPath(char* out, size_t cap, const char* world_dir, int32_t rx, int32_t rz,
                       const char* ext)
{
	snprintf(out, cap, "%s/r.%ld.%ld.%s", world_dir, (long)rx, (long)rz, ext);
}

// ── Directory read and write ──────────────────────────────────────────────────────────

// Reads one directory copy. Everything about it is checked: the magic, the version, and a
// CRC over the entry table. An unreadable copy comes back with valid=false rather than as
// an error, because "one of the two copies is torn" is the normal state after a power cut
// and the other one is expected to carry the world.
static void dirRead(FILE* f, uint32_t at, Dir* d)
{
	uint8_t buf[DIR_BYTES];

	memset(d, 0, sizeof(*d));

	if (fseek(f, (long)at, SEEK_SET) != 0) return;
	if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) return;

	if (get32(buf + 0) != DIR_MAGIC)        return;
	if (get32(buf + 4) != REGION_VERSION)   return;

	const uint32_t seq       = get32(buf + 8);
	const uint32_t stored    = get32(buf + 12);
	const uint32_t computed  = crc32(buf + DIR_HDR_BYTES, REGION_COLS * DIR_ENT_BYTES);
	if (stored != computed) return;

	d->seq   = seq;
	d->valid = true;

	uint32_t end = ARENA_OFF;
	for (int i = 0; i < REGION_COLS; i++) {
		const uint8_t* e = buf + DIR_HDR_BYTES + i * DIR_ENT_BYTES;
		d->ent[i].off = get32(e + 0);
		d->ent[i].len = get32(e + 4);
		d->ent[i].crc = get32(e + 8);

		const uint32_t tail = d->ent[i].off + d->ent[i].len;
		if (d->ent[i].len && tail > end) end = tail;
	}
	d->arena_end = end;
}

// Writes a directory copy at `at` with sequence `seq`. The header's CRC covers the entries
// only, so a torn write that lands the header but not the entries fails the check.
static bool dirWrite(FILE* f, uint32_t at, const Dir* d, uint32_t seq)
{
	uint8_t buf[DIR_BYTES];

	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < REGION_COLS; i++) {
		uint8_t* e = buf + DIR_HDR_BYTES + i * DIR_ENT_BYTES;
		put32(e + 0, d->ent[i].off);
		put32(e + 4, d->ent[i].len);
		put32(e + 8, d->ent[i].crc);
	}

	put32(buf + 0, DIR_MAGIC);
	put32(buf + 4, REGION_VERSION);
	put32(buf + 8, seq);
	put32(buf + 12, crc32(buf + DIR_HDR_BYTES, REGION_COLS * DIR_ENT_BYTES));

	if (fseek(f, (long)at, SEEK_SET) != 0) return false;
	if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) return false;
	return fflush(f) == 0;
}

// Reads both copies and orders them newest first, and reports which slot the *next* write
// should go to. Both invalid — a brand new or wholly destroyed file — yields two empty
// directories at sequence 0, which the first write then supersedes.
//
// Both are returned, not just the winner, because the newest directory is not always the
// one that can be honoured. A card can flush the directory sectors before the payload
// sectors — they are 3 KB apart and FAT drivers do not promise an order — so the state
// "newest directory points at a payload that is not all there" is reachable, and the
// previous directory still describes a complete, older world. regionReadColumn falls back
// to it rather than reporting the column as lost. That is the difference between a power
// cut costing the last save and a power cut costing the whole column.
static void dirLoadPair(FILE* f, Dir* newer, Dir* older, uint32_t* next_off, uint32_t* next_seq)
{
	Dir a, b;
	dirRead(f, DIR_A_OFF, &a);
	dirRead(f, DIR_B_OFF, &b);

	const bool a_first = a.valid && (!b.valid || a.seq >= b.seq);

	if (a_first) {
		*newer = a;
		*older = b;
		*next_off = DIR_B_OFF;
		*next_seq = a.seq + 1;
	} else if (b.valid) {
		*newer = b;
		*older = a;
		*next_off = DIR_A_OFF;
		*next_seq = b.seq + 1;
	} else {
		memset(newer, 0, sizeof(*newer));
		memset(older, 0, sizeof(*older));
		newer->arena_end = ARENA_OFF;
		older->arena_end = ARENA_OFF;
		*next_off = DIR_A_OFF;
		*next_seq = 1;
	}

	// The append point has to clear everything *either* copy points at. Taking it from the
	// live copy alone would let a new payload land on top of bytes the fallback still needs,
	// which would turn the fallback above into a way of reading somebody else's column.
	if (older->arena_end > newer->arena_end) newer->arena_end = older->arena_end;
}

// The common case: just the live directory.
static void dirLoad(FILE* f, Dir* live, uint32_t* next_off, uint32_t* next_seq)
{
	Dir older;
	dirLoadPair(f, live, &older, next_off, next_seq);
}

// ── Column encode and decode ──────────────────────────────────────────────────────────
//
// A column payload is [u8 present mask][per present chunk: u16 length, then the codec's
// bytes]. The mask is one bit per chunk of the column, so an unallocated sky chunk costs a
// zero bit and nothing else — which is most of a 128-tall column.

uint32_t regionEncodeColumn(const Column* col, uint8_t* out, uint32_t cap)
{
	if (cap < 1) return 0;

	uint32_t w = 1;                 // byte 0 is the mask, filled in at the end
	uint8_t  mask = 0;

	for (int i = 0; i < COLUMN_CHUNKS; i++) {
		const Chunk* c = col->chunks[i];
		if (!c) continue;

		if (w + 2 > cap) return 0;
		const size_t n = chunkEncode(c, out + w + 2, cap - w - 2);
		if (n == 0) return 0;

		out[w + 0] = (uint8_t)(n & 0xFF);
		out[w + 1] = (uint8_t)(n >> 8);
		w += 2 + (uint32_t)n;
		mask |= (uint8_t)(1u << i);
	}

	out[0] = mask;
	return w;
}

bool regionDecodeColumn(World* w, int32_t cx, int32_t cz, const uint8_t* in, uint32_t len)
{
	if (len < 1) return false;

	const uint8_t mask = in[0];
	uint32_t      r    = 1;

	for (int i = 0; i < COLUMN_CHUNKS; i++) {
		if (!(mask & (1u << i))) continue;

		if (r + 2 > len) return false;
		const uint32_t n = (uint32_t)in[r] | ((uint32_t)in[r + 1] << 8);
		r += 2;
		if (r + n > len) return false;

		Chunk* c = worldChunkCreate(w, cx, i, cz);
		if (!c) return false;                       // budget refused: caller regenerates

		if (!chunkDecode(c, in + r, n)) return false;
		r += n;
	}

	// Trailing bytes mean the payload is not what the length said it was. Rejecting rather
	// than ignoring, because the usual way to get here is a length from a directory entry
	// that survived a power cut while the payload did not.
	return r == len;
}

// ── Public read and write ─────────────────────────────────────────────────────────────

// Closes regionCompact's one unsafe window. Compaction writes a complete .tmp, removes the
// .bsr, then renames — so the only state a power cut can leave that is not already correct
// is "no .bsr, complete .tmp". Promoting the .tmp before any open turns that into a normal
// file again. A .tmp sitting next to a .bsr is the other interrupted case, and there the
// .bsr is the good one, so the .tmp is dropped.
static void regionRecover(const char* world_dir, int32_t rx, int32_t rz)
{
	char src[256], tmp[256];
	regionPath(src, sizeof(src), world_dir, rx, rz, "bsr");
	regionPath(tmp, sizeof(tmp), world_dir, rx, rz, "tmp");

	FILE* t = fopen(tmp, "rb");
	if (!t) return;
	fclose(t);

	FILE* s = fopen(src, "rb");
	if (s) { fclose(s); remove(tmp); return; }

	rename(tmp, src);
}

// Opens for update, creating the file if it is not there. "r+b" fails on a missing file and
// "w+b" truncates an existing one, so neither alone does what a save needs.
static FILE* openRegion(const char* world_dir, int32_t rx, int32_t rz, bool create)
{
	char path[256];

	regionRecover(world_dir, rx, rz);
	regionPath(path, sizeof(path), world_dir, rx, rz, "bsr");

	FILE* f = fopen(path, "r+b");
	if (f || !create) return f;

	f = fopen(path, "w+b");
	return f;
}

bool regionWriteColumn(const char* world_dir, int32_t cx, int32_t cz,
                       const uint8_t* data, uint32_t len)
{
	if (!len) return false;

	FILE* f = openRegion(world_dir, regionOf(cx), regionOf(cz), true);
	if (!f) return false;

	Dir      dir;
	uint32_t next_off, next_seq;
	dirLoad(f, &dir, &next_off, &next_seq);

	// Append. The old payload for this column stays exactly where it is until the directory
	// write below succeeds, which is what makes an interrupted append harmless.
	const uint32_t at = dir.arena_end < ARENA_OFF ? ARENA_OFF : dir.arena_end;

	if (fseek(f, (long)at, SEEK_SET) != 0)      { fclose(f); return false; }
	if (fwrite(data, 1, len, f) != len)         { fclose(f); return false; }
	if (fflush(f) != 0)                         { fclose(f); return false; }

	const int slot = slotOf(cx, cz);
	dir.ent[slot].off = at;
	dir.ent[slot].len = len;
	dir.ent[slot].crc = crc32(data, len);

	const bool ok = dirWrite(f, next_off, &dir, next_seq);
	fclose(f);
	return ok;
}

// Pulls one entry's payload out and validates it. Zero on anything wrong, which is the same
// answer for "never saved", "cut short" and "checksum failed" — the caller does the same
// thing in all three cases.
static uint32_t entryRead(FILE* f, const DirEnt* e, uint8_t* out, uint32_t cap)
{
	if (!e->len || e->len > cap || e->off < ARENA_OFF) return 0;
	if (fseek(f, (long)e->off, SEEK_SET) != 0)         return 0;

	// Short read: the payload was appended but the file was cut before it finished, and the
	// directory write that pointed at it landed anyway. The checksum would catch it too;
	// this catches it without reading past the end of the buffer.
	if (fread(out, 1, e->len, f) != e->len)            return 0;
	if (crc32(out, e->len) != e->crc)                  return 0;

	return e->len;
}

uint32_t regionReadColumn(const char* world_dir, int32_t cx, int32_t cz,
                          uint8_t* out, uint32_t cap)
{
	FILE* f = openRegion(world_dir, regionOf(cx), regionOf(cz), false);
	if (!f) return 0;

	Dir      newer, older;
	uint32_t next_off, next_seq;
	dirLoadPair(f, &newer, &older, &next_off, &next_seq);

	const int slot = slotOf(cx, cz);

	uint32_t n = entryRead(f, &newer.ent[slot], out, cap);

	// The newest save for this column is not readable, so serve the one before it. Both
	// entries pointing at the same bytes is the normal case and costs a second read only
	// when the first one has already failed.
	if (!n && older.valid) n = entryRead(f, &older.ent[slot], out, cap);

	fclose(f);
	return n;
}

bool regionCompact(const char* world_dir, int32_t rx, int32_t rz)
{
	char src[256], tmp[256];
	regionPath(src, sizeof(src), world_dir, rx, rz, "bsr");
	regionPath(tmp, sizeof(tmp), world_dir, rx, rz, "tmp");

	FILE* in = fopen(src, "rb");
	if (!in) return false;

	Dir      dir;
	uint32_t next_off, next_seq;
	dirLoad(in, &dir, &next_off, &next_seq);

	// Live bytes against file bytes. Below half wasted there is nothing worth the rewrite,
	// and rewriting on every close would make quitting slower for no gain.
	uint32_t live = 0;
	for (int i = 0; i < REGION_COLS; i++) live += dir.ent[i].len;
	if (dir.arena_end <= ARENA_OFF || live * 2 >= dir.arena_end - ARENA_OFF) {
		fclose(in);
		return false;
	}

	FILE* out = fopen(tmp, "w+b");
	if (!out) { fclose(in); return false; }

	Dir      packed;
	memset(&packed, 0, sizeof(packed));
	packed.arena_end = ARENA_OFF;

	static uint8_t buf[REGION_COL_MAX];
	bool ok = true;

	for (int i = 0; i < REGION_COLS && ok; i++) {
		const DirEnt e = dir.ent[i];
		if (!e.len || e.len > sizeof(buf)) continue;

		if (fseek(in, (long)e.off, SEEK_SET) != 0)        { ok = false; break; }
		if (fread(buf, 1, e.len, in) != e.len)            { ok = false; break; }
		if (crc32(buf, e.len) != e.crc)                   continue;   // drop the corrupt one

		if (fseek(out, (long)packed.arena_end, SEEK_SET) != 0) { ok = false; break; }
		if (fwrite(buf, 1, e.len, out) != e.len)               { ok = false; break; }

		packed.ent[i].off = packed.arena_end;
		packed.ent[i].len = e.len;
		packed.ent[i].crc = e.crc;
		packed.arena_end += e.len;
	}

	if (ok) ok = dirWrite(out, DIR_A_OFF, &packed, 1);

	fclose(in);
	fclose(out);

	if (!ok) { remove(tmp); return false; }

	// The original is not touched until the replacement is closed and complete. A power cut
	// between these two lines leaves no .bsr and a complete .tmp, which is why the loader
	// falls back to the .tmp — see regionRecover.
	if (remove(src) != 0)     return false;
	if (rename(tmp, src) != 0) return false;
	return true;
}
