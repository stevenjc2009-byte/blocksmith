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

// v1.7.1 task 48. Counts the two operations the region cache exists to remove. Compiled out
// of every build but the probe in tools/, so the game pays nothing for them — the alternative
// was to claim the saving from reading the diff, which is not a measurement.
#ifdef BS_REGION_PROBE
unsigned long g_region_fopens;
unsigned long g_region_dirreads;
#define PROBE_BUMP(counter) ((counter)++)
#else
#define PROBE_BUMP(counter) ((void)0)
#endif

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

	PROBE_BUMP(g_region_dirreads);

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

	PROBE_BUMP(g_region_fopens);
	FILE* t = fopen(tmp, "rb");
	if (!t) return;
	fclose(t);

	PROBE_BUMP(g_region_fopens);
	FILE* s = fopen(src, "rb");
	if (s) { fclose(s); remove(tmp); return; }

	rename(tmp, src);
}

// Opens for update, creating the file if it is not there. "r+b" fails on a missing file and
// "w+b" truncates an existing one, so neither alone does what a save needs.
static FILE* openRegion(const char* world_dir, int32_t rx, int32_t rz, bool create)
{
	char path[256];

	// v1.7.1 task 48. create=true is what a *writer* asks for and nothing else in this file
	// asks for it, so this one line is where the region cache is dropped for every writer
	// there is or will be — put here rather than in each writer precisely so that adding a
	// third one cannot forget it. A cached directory that has not seen a just-saved column
	// would report the column as never saved and the generator would rebuild it from the
	// seed, which is a player's build silently deleted; that outcome is far worse than the
	// load cost this cache exists to remove, so it is dropped before the write rather than
	// selectively after it. Anything that ever writes a region file WITHOUT coming through
	// here must call regionCacheClose() itself.
	if (create) regionCacheClose();

	regionRecover(world_dir, rx, rz);
	regionPath(path, sizeof(path), world_dir, rx, rz, "bsr");

	PROBE_BUMP(g_region_fopens);
	FILE* f = fopen(path, "r+b");
	if (f || !create) return f;

	PROBE_BUMP(g_region_fopens);
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

	// Dropped a second time, after the write as well as before it (openRegion above). The
	// before-drop is what makes the invalidation unforgettable; this one closes the window a
	// second thread would open by filling the cache from the old directory while this write
	// was in flight. Costs two comparisons and at most two fclose per SD write, against a
	// stale entry costing the player a column of blocks.
	regionCacheClose();
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

// ── v1.7.1 task 48: the load-side region cache ────────────────────────────────────────
//
// See region.h for why this exists and what it is measured to save. What follows is only
// what a reader of this file needs to know to change it safely.
//
// Two entries, not one and not eight. A 9x9 column area — RENDER_DIST_MAX 3, which is what
// genRequestArea submits — is 9 columns wide against a 16-wide region, so it straddles at
// most two regions on each axis and at worst four files; two entries cover the common case
// (the player is somewhere inside a region, or on one boundary) and a straddled corner
// degrades to today's behaviour rather than to anything wrong. The reason not to go to four
// is the byte cost below.
//
// Byte cost, and it is the whole reason this is not bigger: an entry carries BOTH parsed
// directory copies, because regionReadColumn's fallback to the older copy is what turns a
// power cut into "the last save was lost" instead of "the column was lost", and dropping it
// here would quietly drop that guarantee for the only path the game actually loads through.
// sizeof(Dir) is 3084 (two u32, 256 x 12-byte entries, one bool, padded), so an entry is
// two of those plus the 128-byte directory path, a FILE* and four words. Measured rather
// than estimated — the task-48 probe prints sizeof(s_cache) and it is **12656 bytes** of
// .bss for the two entries, 6328 each. On a console with 64 MB that is affordable and on a
// console with less it would not be; anyone raising REGION_CACHE_ENTS is spending 6.2 KB a
// step, and should re-run the probe rather than trust this line.
#define REGION_CACHE_ENTS    2

// Matches app/worker.c's s_world_dir[128], which is the only thing that ever supplies a
// world_dir in the game. A path longer than this is not cached at all rather than cached
// under a truncated key — two different worlds whose names differ past byte 127 would
// otherwise share an entry, which is the same lost-blocks failure the invalidation above is
// written to prevent.
#define REGION_CACHE_DIR_MAX 128

typedef struct {
	bool     used;
	char     dir[REGION_CACHE_DIR_MAX];
	int32_t  rx, rz;
	FILE*    f;                 // NULL when this region has no file yet; see cacheFill
	Dir      newer, older;
	uint32_t stamp;             // LRU clock reading at the last hit
} RegionCacheEnt;

static RegionCacheEnt s_cache[REGION_CACHE_ENTS];
static uint32_t       s_cache_clock;

#ifdef BS_REGION_PROBE
// So the .bss figure in the comment above is a measurement and not an estimate.
const unsigned long g_region_cache_bytes = (unsigned long)sizeof(s_cache);
#endif

static void cacheEvict(RegionCacheEnt* e)
{
	if (e->f) fclose(e->f);
	e->f    = NULL;
	e->used = false;
}

void regionCacheClose(void)
{
	for (int i = 0; i < REGION_CACHE_ENTS; i++) cacheEvict(&s_cache[i]);
}

static RegionCacheEnt* cacheFind(const char* world_dir, int32_t rx, int32_t rz)
{
	for (int i = 0; i < REGION_CACHE_ENTS; i++) {
		RegionCacheEnt* e = &s_cache[i];
		if (!e->used || e->rx != rx || e->rz != rz) continue;
		if (strcmp(e->dir, world_dir) != 0)         continue;

		e->stamp = ++s_cache_clock;
		return e;
	}
	return NULL;
}

// Fills the least recently used slot from disk. NULL means "do not cache this one" — the
// caller falls back to the uncached read rather than guessing, so an over-long path costs
// speed and never correctness.
//
// A region with no file yet is cached too, as an entry with f == NULL. That is not an
// oversight: a fresh world has no .bsr at all, so without it every one of the 81 columns of
// the first view would pay two failed opens for nothing. The moment a save creates the file,
// openRegion(create=true) drops the whole cache, so the negative answer cannot outlive the
// thing that made it true.
static RegionCacheEnt* cacheFill(const char* world_dir, int32_t rx, int32_t rz)
{
	if (strlen(world_dir) >= REGION_CACHE_DIR_MAX) return NULL;

	RegionCacheEnt* e = &s_cache[0];
	for (int i = 1; i < REGION_CACHE_ENTS; i++)
		if (!s_cache[i].used || s_cache[i].stamp < e->stamp) e = &s_cache[i];

	cacheEvict(e);

	e->f = openRegion(world_dir, rx, rz, false);
	if (e->f) {
		uint32_t next_off, next_seq;
		dirLoadPair(e->f, &e->newer, &e->older, &next_off, &next_seq);
	} else {
		memset(&e->newer, 0, sizeof(e->newer));
		memset(&e->older, 0, sizeof(e->older));
	}

	snprintf(e->dir, sizeof(e->dir), "%s", world_dir);
	e->rx    = rx;
	e->rz    = rz;
	e->used  = true;
	e->stamp = ++s_cache_clock;
	return e;
}

uint32_t regionReadColumnCached(const char* world_dir, int32_t cx, int32_t cz,
                                uint8_t* out, uint32_t cap)
{
	const int32_t rx = regionOf(cx), rz = regionOf(cz);

	RegionCacheEnt* e = cacheFind(world_dir, rx, rz);
	if (!e) e = cacheFill(world_dir, rx, rz);
	if (!e) return regionReadColumn(world_dir, cx, cz, out, cap);   // path too long to key

	if (!e->f) return 0;            // no region file: every column here is unsaved

	// From here down this is regionReadColumn's body verbatim, minus the open, the two
	// directory reads and the close. The fallback to the older copy is kept for the reason
	// that function gives for having it — dropping it on the game's only load path would
	// mean a power cut cost the whole column instead of the last save of it.
	const int slot = slotOf(cx, cz);

	uint32_t n = entryRead(e->f, &e->newer.ent[slot], out, cap);
	if (!n && e->older.valid) n = entryRead(e->f, &e->older.ent[slot], out, cap);
	return n;
}

bool regionCompact(const char* world_dir, int32_t rx, int32_t rz)
{
	// The other writer, and the one that does not come through openRegion(create=true): it
	// opens the source read-only and builds its replacement with a bare fopen. Dropped on the
	// way in so nothing can be serving this file's old directory while it is being replaced,
	// and again on the way out because the rename at the bottom changes which bytes the path
	// names. See the note in openRegion.
	regionCacheClose();

	char src[256], tmp[256];
	regionPath(src, sizeof(src), world_dir, rx, rz, "bsr");
	regionPath(tmp, sizeof(tmp), world_dir, rx, rz, "tmp");

	FILE* in = fopen(src, "rb");
	if (!in) return false;

	// The PAIR, not just the live copy — see the copy loop below, which is where the older
	// one is needed and where taking the live one alone used to cost a column outright.
	Dir      dir, older;
	uint32_t next_off, next_seq;
	dirLoadPair(in, &dir, &older, &next_off, &next_seq);

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
		// entryRead, and the fallback to the older copy on top of it, because a repack has to
		// carry forward exactly what regionReadColumn would have served — no more and no less.
		//
		// v1.7.1 task 48c, and this is a fix rather than a tidy-up. The loop used to read
		// dir.ent[i] only and `continue` past a payload whose CRC failed. dirLoadPair's own
		// comment explains why that state exists: a card can flush the directory sectors
		// before the payload sectors, so after a power cut the newest directory can point at
		// a payload that is not all there while the previous directory still describes a
		// complete older copy of that column. Reads fall back to it — "the difference between
		// a power cut costing the last save and a power cut costing the whole column". A
		// repack that looked only at the newer copy dropped such a column from the new file
		// and then deleted the old file that still held it, converting the recoverable case
		// into a permanent loss, silently, at world close. Measured: without this fallback the
		// host test read the column correctly one line before regionCompact and as nothing
		// one line after.
		uint32_t len = entryRead(in, &dir.ent[i], buf, sizeof(buf));
		uint32_t crc = dir.ent[i].crc;

		if (!len && older.valid) {
			len = entryRead(in, &older.ent[i], buf, sizeof(buf));
			crc = older.ent[i].crc;
		}

		// Neither copy can be read, so there is nothing here to preserve — the column was
		// already lost before this function ran, and carrying the broken entry forward would
		// only make the new file lie about it.
		if (!len) continue;

		if (fseek(out, (long)packed.arena_end, SEEK_SET) != 0) { ok = false; break; }
		if (fwrite(buf, 1, len, out) != len)                   { ok = false; break; }

		packed.ent[i].off = packed.arena_end;
		packed.ent[i].len = len;
		packed.ent[i].crc = crc;
		packed.arena_end += len;
	}

	if (ok) ok = dirWrite(out, DIR_A_OFF, &packed, 1);

	fclose(in);
	fclose(out);

	if (!ok) { remove(tmp); return false; }

	// The original is not touched until the replacement is closed and complete. A power cut
	// between these two lines leaves no .bsr and a complete .tmp, which is why the loader
	// falls back to the .tmp — see regionRecover.
	regionCacheClose();
	if (remove(src) != 0)     return false;
	if (rename(tmp, src) != 0) return false;
	return true;
}
