#include "world/chunk_codec.h"

#include <string.h>

#include "world/budget.h"

// A run is a length and a palette index. Lengths are stored minus one so a run of 256 fits
// in a byte, which matters more than it sounds: 256 is exactly one 16x16 layer of a chunk,
// and a flat layer of one block id is the single most common shape in generated terrain.
#define RUN_MAX  256

// Palette indices are one byte, so 256 distinct ids in a chunk is the ceiling. The registry
// only defines seven, but the format should not need a version bump the first time a block
// is added, and the encoder falls back to raw if it ever overflows anyway.
//
// This is a DISK palette, up to 256 entries — unrelated to chunk.h's in-memory PALETTE4 form,
// which is capped at 16 for a 4-bit index. The two are deliberately decoupled: a save file's
// layout is a format contract (see this file's header), while PALETTE4's 16-slot ceiling is
// an in-memory tradeoff step 9.2a is free to revisit without touching a single byte on disk.
#define PALETTE_MAX  256

// Builds the palette in first-seen order and reports how big it is: always 1..PALETTE_MAX,
// because a BlockId is a byte and there are only 256 of them, and a chunk always contains
// at least one block. So there is no overflow case to handle here and no failure to return.
//
// A 256-entry seen[] indexed by block id, rather than a scan of the palette per block: the
// scan is O(blocks * palette) and this is O(blocks), and 256 bytes of stack is nothing
// against the 4,096 it is walking.
static int paletteBuild(const BlockId* blocks, uint8_t* pal)
{
	uint8_t seen[256];
	memset(seen, 0, sizeof(seen));

	int n = 0;
	for (int i = 0; i < CHUNK_BLOCKS; i++) {
		const BlockId id = blocks[i];
		if (seen[id]) continue;
		seen[id] = 1;
		pal[n++] = id;
	}
	return n;
}

// Shared by both branches of chunkEncode below: RLE-encodes a chunk whose disk palette is
// `pal[0..pal_n)` and whose cell i's disk-palette index is `index[i]`, into `out`/`cap`,
// falling back to raw (via `pal[index[i]]`, which reconstructs cell i's BlockId either way
// `index` was built) the moment RLE stops paying. Deliberately a plain array in, not a
// callback: an indirect call per cell here cost more than the round trip 9.2d exists to
// remove -- measured on the layered-chunk benchmark in this step's report, and reverted back
// to this shape once the function-pointer version measured slower than the code it replaced.
static size_t encodeFromIndex(const uint8_t* pal, int pal_n, const uint8_t* index,
                               uint8_t* out, size_t cap)
{
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
		const uint8_t pi = index[i];

		int run = 1;
		while (run < RUN_MAX && i + run < CHUNK_BLOCKS && index[i + run] == pi) run++;

		if (w + 2 > rle_cap) { rle_ok = false; break; }
		out[w++] = (uint8_t)(run - 1);
		out[w++] = pi;
		i += run;
	}

	if (rle_ok) return w;

	// Raw. Reached either because RLE would have been bigger, or because the caller's buffer
	// is too small for the RLE form — in which case it is too small for this too, and 0 is
	// the honest answer.
	if (cap < raw_size) return 0;
	out[0] = CODEC_RAW;
	out[1] = 0;                      // reserved: keeps every form's payload 2-byte aligned
	for (int i = 0; i < CHUNK_BLOCKS; i++) out[2 + i] = pal[index[i]];
	return raw_size;
}

size_t chunkEncode(const Chunk* c, uint8_t* out, size_t cap)
{
	// Step 9.2d. A chunk already sitting in CHUNK_FORM_UNIFORM or CHUNK_FORM_PALETTE4 already
	// carries this function's answer in memory: chunkGet(c, 0) for UNIFORM, chunkGetPalette4
	// for PALETTE4. Both skip the chunkDecompressAll + paletteBuild round trip entirely --
	// decompressing the whole chunk into a flat BlockId array and then re-deriving a palette
	// from scratch via a first-seen scan over all 4,096 cells, when the palette already exists
	// (or, for UNIFORM, is the one id itself). Only a RAW-form chunk still pays that, below,
	// because RAW carries no ready-made palette to reuse -- it never did.
	if (chunkGetForm(c) == CHUNK_FORM_UNIFORM) {
		if (cap < 2) return 0;
		out[0] = CODEC_UNIFORM;
		out[1] = chunkGet(c, 0);
		return 2;
	}

	BlockId pal4[16];
	uint8_t pal4_n;
	uint8_t idx4[CHUNK_BLOCKS];
	if (chunkGetPalette4(c, pal4, &pal4_n, idx4)) {
		// idx4[i] is already the disk-palette index for cell i: chunk.c's in-memory palette
		// order is copied verbatim as the disk palette (chunkGetPalette4's contract), so no
		// id -> index reverse map is needed here, unlike the RAW path below.
		return encodeFromIndex(pal4, pal4_n, idx4, out, cap);
	}

	// Step 9.2a. Chunk is opaque now, so this can no longer read c->blocks[i] directly —
	// unpack ONCE, here, into a flat local (4,096 bytes on the stack, the same size
	// chunkDecode's `tmp` below already puts there) and run the existing palette-build and
	// RLE scan against that, instead of paying a chunkGet call per cell examined below.
	BlockId blocks[CHUNK_BLOCKS];
	chunkDecompressAll(c, blocks);

	uint8_t pal[PALETTE_MAX];
	const int pal_n = paletteBuild(blocks, pal);

	// One block id in the whole chunk. Half of a 128-tall column is sky, so this is not the
	// degenerate case, it is the usual one. (A RAW-form chunk can still land here: RAW never
	// demotes, so an edit that converges everything back to one id leaves the chunk RAW in
	// memory even though its content is now uniform -- see chunk.h's file header.)
	if (pal_n == 1) {
		if (cap < 2) return 0;
		out[0] = CODEC_UNIFORM;
		out[1] = pal[0];
		return 2;
	}

	// Map id -> disk palette index once, then apply it once per cell into a plain array --
	// same shape encodeFromIndex expects from the PALETTE4 fast path above, built here
	// instead of already sitting in memory the way idx4 was.
	uint8_t idx[256];
	memset(idx, 0, sizeof(idx));
	for (int i = 0; i < pal_n; i++) idx[pal[i]] = (uint8_t)i;

	uint8_t index[CHUNK_BLOCKS];
	for (int i = 0; i < CHUNK_BLOCKS; i++) index[i] = idx[blocks[i]];

	return encodeFromIndex(pal, pal_n, index, out, cap);
}

// Shared by both of chunkDecode's direct-commit fast paths below: the same
// claim/commit/release shape as the general path at the bottom of this file, just calling
// `commit` (a chunkClear or chunkLoadPalette4 call, already bound to its arguments by the
// caller) instead of chunkLoadAll, and taking the post-commit byte count from `form` instead
// of a chunkFormForAll query -- both fast-path callers already know `form` without asking,
// which is the entire point of them existing.
static bool commitDirect(Chunk* c, ChunkForm form, bool (*commit)(Chunk*, void*), void* arg)
{
	const size_t bytes_before = chunkGetBytes(c);
	const size_t bytes_after_query = chunkFormBytes(form);

	if (bytes_after_query > bytes_before) {
		if (!budgetClaim(bytes_after_query - bytes_before)) return false;
	}

	if (!commit(c, arg)) {
		if (bytes_after_query > bytes_before)
			budgetRelease(bytes_after_query - bytes_before);
		return false;
	}

	const size_t bytes_after = chunkGetBytes(c);
	if (bytes_after < bytes_before) budgetRelease(bytes_before - bytes_after);
	return true;
}

static bool commitClear(Chunk* c, void* arg) { chunkClear(c, *(BlockId*)arg); return true; }

typedef struct { const BlockId* palette; uint8_t pal_n; const uint8_t* index; } P4Commit;
static bool commitPalette4(Chunk* c, void* arg)
{
	const P4Commit* p = (const P4Commit*)arg;
	return chunkLoadPalette4(c, p->palette, p->pal_n, p->index);
}

bool chunkDecode(Chunk* c, const uint8_t* in, size_t len)
{
	if (len < 2) return false;

	// Step 9.2d. CODEC_UNIFORM's whole payload already IS the chunk's CHUNK_FORM_UNIFORM
	// answer -- commit it straight through chunkClear via commitDirect, instead of expanding
	// to a flat BlockId[CHUNK_BLOCKS] buffer and paying chunkFormForAll/chunkLoadAll's
	// buildPalette16 rescan of all 4,096 cells just to rediscover "one distinct id, `in[1]`"
	// a second time.
	if (in[0] == CODEC_UNIFORM) {
		if (len != 2) return false;
		BlockId fill = in[1];
		return commitDirect(c, CHUNK_FORM_UNIFORM, commitClear, &fill);
	}

	// Step 9.2a. Every branch below now decodes into this one flat local instead of writing
	// straight into `c` (the RLE branch always did this — `tmp`, for the power-cut reason
	// explained below; UNIFORM and RAW are folded into the same shape here so all three
	// branches commit through the single budget-aware chunkLoadAll call at the end, rather
	// than each doing its own no-longer-possible direct memset/memcpy into an opaque Chunk).
	BlockId blocks[CHUNK_BLOCKS];

	switch (in[0]) {
	case CODEC_RAW:
		if (len != (size_t)CHUNK_BLOCKS + 2) return false;
		memcpy(blocks, in + 2, CHUNK_BLOCKS);
		break;

	case CODEC_RLE: {
		const int pal_n = (int)in[1] + 1;
		const size_t hdr = 2 + (size_t)pal_n;
		if (len < hdr) return false;

		const uint8_t* pal  = in + 2;
		const uint8_t* runs = in + hdr;
		const size_t   run_bytes = len - hdr;

		// Odd run data means the file was cut between a run's length and its index.
		if (run_bytes % 2 != 0) return false;

		// Step 9.2d. The run table is unpacked into `index` -- one already-validated disk
		// palette slot (0..pal_n-1) per cell -- regardless of pal_n, because that unpacking
		// and its validation (the two checks below) are identical either way; what differs
		// is what happens with the result. pal_n <= 16 fits chunk.c's CHUNK_FORM_PALETTE4
		// exactly, so it commits straight there via chunkLoadPalette4 -- no BlockId
		// translation and no chunkLoadAll/buildPalette16 rescan of a form this branch
		// already knows for certain. pal_n > 16 cannot fit PALETTE4's 4-bit index ceiling
		// (chunk.h), so -- same as CODEC_RAW above, and unchanged from before 9.2d -- it
		// still expands to `blocks` and falls through to the general commit at the bottom.
		uint8_t index[CHUNK_BLOCKS];
		size_t n = 0;
		for (size_t i = 0; i < run_bytes; i += 2) {
			const int run = (int)runs[i] + 1;
			const int pi  = (int)runs[i + 1];

			if (pi >= pal_n) return false;
			if (n + (size_t)run > CHUNK_BLOCKS) return false;

			memset(index + n, (uint8_t)pi, (size_t)run);
			n += (size_t)run;
		}

		// Exactly full. Short means truncated; the overrun case is already rejected above.
		if (n != CHUNK_BLOCKS) return false;

		if (pal_n <= 16) {
			BlockId palette[16];
			memcpy(palette, pal, (size_t)pal_n);
			P4Commit arg = { palette, (uint8_t)pal_n, index };
			return commitDirect(c, pal_n == 1 ? CHUNK_FORM_UNIFORM : CHUNK_FORM_PALETTE4,
			                    commitPalette4, &arg);
		}

		for (size_t i = 0; i < CHUNK_BLOCKS; i++) blocks[i] = pal[index[i]];
		break;
	}

	default:
		return false;
	}

	// Commit through the same budget-aware pre-flight/claim/commit/release shape
	// worldSetChunkAll uses in world.c — duplicated here, not shared, because region.c calls
	// chunkDecode directly with no World* to route a claim through, so this file has to do
	// its own budget interaction to stay self-contained (see this file's header and
	// chunk.h's for why chunk_codec.c and world.c cannot share one code path here). On a
	// refusal or an allocator failure, c is left exactly as it was before this call — the
	// documented contract of chunkDecode returning false.
	const size_t bytes_before = chunkGetBytes(c);
	const size_t bytes_after_query = chunkFormBytes(chunkFormForAll(blocks));

	if (bytes_after_query > bytes_before) {
		if (!budgetClaim(bytes_after_query - bytes_before)) return false;
	}

	if (!chunkLoadAll(c, blocks)) {
		if (bytes_after_query > bytes_before)
			budgetRelease(bytes_after_query - bytes_before);
		return false;
	}

	const size_t bytes_after = chunkGetBytes(c);
	if (bytes_after < bytes_before) budgetRelease(bytes_before - bytes_after);
	return true;
}
