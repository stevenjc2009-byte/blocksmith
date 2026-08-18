#include "world/chunk_codec.h"

#include <string.h>

// A run is a length and a palette index. Lengths are stored minus one so a run of 256 fits
// in a byte, which matters more than it sounds: 256 is exactly one 16x16 layer of a chunk,
// and a flat layer of one block id is the single most common shape in generated terrain.
#define RUN_MAX  256

// Palette indices are one byte, so 256 distinct ids in a chunk is the ceiling. The registry
// only defines seven, but the format should not need a version bump the first time a block
// is added, and the encoder falls back to raw if it ever overflows anyway.
#define PALETTE_MAX  256

// Builds the palette in first-seen order and reports how big it is: always 1..PALETTE_MAX,
// because a BlockId is a byte and there are only 256 of them, and a chunk always contains
// at least one block. So there is no overflow case to handle here and no failure to return.
//
// A 256-entry seen[] indexed by block id, rather than a scan of the palette per block: the
// scan is O(blocks * palette) and this is O(blocks), and 256 bytes of stack is nothing
// against the 4,096 it is walking.
static int paletteBuild(const Chunk* c, uint8_t* pal)
{
	uint8_t seen[256];
	memset(seen, 0, sizeof(seen));

	int n = 0;
	for (int i = 0; i < CHUNK_BLOCKS; i++) {
		const BlockId id = c->blocks[i];
		if (seen[id]) continue;
		seen[id] = 1;
		pal[n++] = id;
	}
	return n;
}

size_t chunkEncode(const Chunk* c, uint8_t* out, size_t cap)
{
	uint8_t pal[PALETTE_MAX];
	const int pal_n = paletteBuild(c, pal);

	// One block id in the whole chunk. Half of a 128-tall column is sky, so this is not the
	// degenerate case, it is the usual one.
	if (pal_n == 1) {
		if (cap < 2) return 0;
		out[0] = CODEC_UNIFORM;
		out[1] = pal[0];
		return 2;
	}

	// Map id -> palette index once, so the run loop does not search the palette per block.
	uint8_t idx[256];
	memset(idx, 0, sizeof(idx));
	for (int i = 0; i < pal_n; i++) idx[pal[i]] = (uint8_t)i;

	// Try RLE into the caller's buffer, and bail out to raw the moment it stops paying —
	// which is the checkerboard case, where run data would be twice the size of the blocks.
	const size_t raw_size = (size_t)CHUNK_BLOCKS + 2;
	const size_t rle_cap  = raw_size < cap ? raw_size : cap;

	size_t w = 0;
	bool   rle_ok = true;

	// [tag][palette count][palette]
	if ((size_t)(2 + pal_n) > rle_cap) {
		rle_ok = false;
	} else {
		out[w++] = CODEC_RLE;
		// Stored minus one: a 256-entry palette would not fit a byte otherwise, and a
		// zero-entry palette cannot happen (an empty chunk still has air in it).
		out[w++] = (uint8_t)(pal_n - 1);
		memcpy(out + w, pal, (size_t)pal_n);
		w += (size_t)pal_n;
	}

	for (int i = 0; rle_ok && i < CHUNK_BLOCKS; ) {
		const BlockId id = c->blocks[i];

		int run = 1;
		while (run < RUN_MAX && i + run < CHUNK_BLOCKS && c->blocks[i + run] == id) run++;

		if (w + 2 > rle_cap) { rle_ok = false; break; }
		out[w++] = (uint8_t)(run - 1);
		out[w++] = idx[id];
		i += run;
	}

	if (rle_ok) return w;

	// Raw. Reached either because RLE would have been bigger, or because the caller's buffer
	// is too small for the RLE form — in which case it is too small for this too, and 0 is
	// the honest answer.
	if (cap < raw_size) return 0;
	out[0] = CODEC_RAW;
	out[1] = 0;                      // reserved: keeps every form's payload 2-byte aligned
	memcpy(out + 2, c->blocks, CHUNK_BLOCKS);
	return raw_size;
}

bool chunkDecode(Chunk* c, const uint8_t* in, size_t len)
{
	if (len < 2) return false;

	switch (in[0]) {
	case CODEC_UNIFORM:
		if (len != 2) return false;
		memset(c->blocks, in[1], CHUNK_BLOCKS);
		return true;

	case CODEC_RAW:
		if (len != (size_t)CHUNK_BLOCKS + 2) return false;
		memcpy(c->blocks, in + 2, CHUNK_BLOCKS);
		return true;

	case CODEC_RLE:
		break;

	default:
		return false;
	}

	const int pal_n = (int)in[1] + 1;
	const size_t hdr = 2 + (size_t)pal_n;
	if (len < hdr) return false;

	const uint8_t* pal  = in + 2;
	const uint8_t* runs = in + hdr;
	const size_t   run_bytes = len - hdr;

	// Odd run data means the file was cut between a run's length and its index.
	if (run_bytes % 2 != 0) return false;

	// Decoded into scratch, not into `c`, so a run table that turns out to be short or long
	// leaves the caller's chunk exactly as it was. A save interrupted by a power cut is the
	// expected way to get here, and a half-overwritten chunk is worse than a rejected one.
	uint8_t tmp[CHUNK_BLOCKS];
	size_t  n = 0;

	for (size_t i = 0; i < run_bytes; i += 2) {
		const int run = (int)runs[i] + 1;
		const int pi  = (int)runs[i + 1];

		if (pi >= pal_n) return false;
		if (n + (size_t)run > CHUNK_BLOCKS) return false;

		memset(tmp + n, pal[pi], (size_t)run);
		n += (size_t)run;
	}

	// Exactly full. Short means truncated; the overrun case is already rejected above.
	if (n != CHUNK_BLOCKS) return false;

	memcpy(c->blocks, tmp, CHUNK_BLOCKS);
	return true;
}
