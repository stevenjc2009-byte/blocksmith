#include "world/chunk.h"

#include <stdlib.h>
#include <string.h>

// The PALETTE4 payload: a 16-slot palette (however many of the 16 are actually in use,
// tracked by pal_n) and one 4-bit index per cell. 16 + 1 + 2048 = 2,065 bytes, and every
// member is uint8_t-sized, so the struct has no alignment padding to worry about -- its
// sizeof is exactly that sum on every target this project builds for.
typedef struct {
	BlockId palette[16];
	uint8_t pal_n;                     // 1..16: how many of the 16 slots are meaningful
	uint8_t indices[CHUNK_BLOCKS / 2]; // 4 bits per cell; see get/setNibble below
} Palette4;

// The header. Fixed size, and -- this is the load-bearing property -- NEVER reallocated:
// a promotion only ever replaces the payload blob and updates `form`/`payload`, so a
// Chunk* handed out by chunkAlloc stays valid and unchanged in address for the chunk's
// whole life. That is what lets world.c hold a Chunk* in a Column across any number of
// writes without ever having to notice a promotion happened.
struct Chunk {
	ChunkForm form;
	BlockId   uniform_id;   // meaningful iff form == CHUNK_FORM_UNIFORM
	union {
		Palette4* p4;        // meaningful iff form == CHUNK_FORM_PALETTE4
		BlockId*  raw;       // meaningful iff form == CHUNK_FORM_RAW; CHUNK_BLOCKS entries
	} payload;
};

static inline uint8_t getNibble(const uint8_t* idx, int i)
{
	const uint8_t b = idx[i >> 1];
	return (i & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
}

static inline void setNibble(uint8_t* idx, int i, uint8_t v)
{
	uint8_t* b = &idx[i >> 1];
	*b = (i & 1) ? (uint8_t)((*b & 0x0F) | (uint8_t)(v << 4))
	             : (uint8_t)((*b & 0xF0) | (v & 0x0F));
}

size_t chunkFormBytes(ChunkForm form)
{
	switch (form) {
	case CHUNK_FORM_UNIFORM:  return sizeof(struct Chunk);
	case CHUNK_FORM_PALETTE4: return sizeof(struct Chunk) + sizeof(Palette4);
	case CHUNK_FORM_RAW:      return sizeof(struct Chunk) + (size_t)CHUNK_BLOCKS;
	}
	return sizeof(struct Chunk);
}

Chunk* chunkAlloc(BlockId fill)
{
	Chunk* c = (Chunk*)malloc(sizeof(struct Chunk));
	if (!c) return NULL;
	c->form = CHUNK_FORM_UNIFORM;
	c->uniform_id = fill;
	c->payload.p4 = NULL;   // either union member as NULL reads the same bit pattern
	return c;
}

// Frees the payload blob, if c has one, without touching `form` or `uniform_id` -- callers
// set those themselves right after, as part of installing whatever comes next.
static void freePayload(Chunk* c)
{
	switch (c->form) {
	case CHUNK_FORM_PALETTE4: free(c->payload.p4);  break;
	case CHUNK_FORM_RAW:      free(c->payload.raw); break;
	case CHUNK_FORM_UNIFORM:                        break;
	}
}

void chunkFree(Chunk* c)
{
	if (!c) return;
	freePayload(c);
	free(c);
}

ChunkForm chunkGetForm(const Chunk* c) { return c->form; }
size_t    chunkGetBytes(const Chunk* c) { return chunkFormBytes(c->form); }

bool chunkIsUniform(const Chunk* c, BlockId fill)
{
	switch (c->form) {
	case CHUNK_FORM_UNIFORM:
		return c->uniform_id == fill;

	case CHUNK_FORM_PALETTE4:
		// pal_n == 1 is sufficient but not necessary: a write that overwrites the one
		// different cell back to the surrounding value leaves a second, orphaned
		// palette slot behind (chunkSet never demotes -- see chunk.h) without pal_n
		// going back down. That is a missed optimisation, never a wrong answer: this
		// function must never say "uniform" for a chunk that is not, because the
		// caller uses it to skip building geometry, so the orphaned-slot case falls
		// through to the conservative "false" below rather than paying for a scan.
		return c->payload.p4->pal_n == 1 && c->payload.p4->palette[0] == fill;

	case CHUNK_FORM_RAW: {
		const BlockId* raw = c->payload.raw;
		for (int i = 0; i < CHUNK_BLOCKS; i++)
			if (raw[i] != fill) return false;
		return true;
	}
	}
	return false;
}

bool chunkIsAllAir(const Chunk* c) { return chunkIsUniform(c, BLOCK_AIR); }

void chunkClear(Chunk* c, BlockId fill)
{
	freePayload(c);
	c->form = CHUNK_FORM_UNIFORM;
	c->uniform_id = fill;
	c->payload.p4 = NULL;
}

ChunkForm chunkFormFor(const Chunk* c, BlockId id)
{
	switch (c->form) {
	case CHUNK_FORM_UNIFORM:
		// Two distinct ids -- the id already filling the chunk, and this new one --
		// always fits a 16-slot palette, so a UNIFORM chunk can never need to jump
		// straight to RAW on a single write.
		return (id == c->uniform_id) ? CHUNK_FORM_UNIFORM : CHUNK_FORM_PALETTE4;

	case CHUNK_FORM_PALETTE4: {
		const Palette4* p = c->payload.p4;
		for (int i = 0; i < p->pal_n; i++)
			if (p->palette[i] == id) return CHUNK_FORM_PALETTE4;   // already representable
		return (p->pal_n < 16) ? CHUNK_FORM_PALETTE4 : CHUNK_FORM_RAW;
	}

	case CHUNK_FORM_RAW:
		return CHUNK_FORM_RAW;   // already the fallback; nothing promotes further
	}
	return CHUNK_FORM_RAW;
}

bool chunkSet(Chunk* c, int idx, BlockId id)
{
	switch (c->form) {
	case CHUNK_FORM_UNIFORM: {
		if (id == c->uniform_id) return true;   // no-op: already this value everywhere

		Palette4* p = (Palette4*)malloc(sizeof(Palette4));
		if (!p) return false;   // c is untouched: still UNIFORM, still c->uniform_id

		p->palette[0] = c->uniform_id;
		p->palette[1] = id;
		p->pal_n = 2;
		// Every cell starts at palette slot 0 (the old uniform value)...
		memset(p->indices, 0, sizeof(p->indices));
		// ...except idx, which becomes the new value, palette slot 1.
		setNibble(p->indices, idx, 1);

		c->form = CHUNK_FORM_PALETTE4;
		c->payload.p4 = p;
		return true;
	}

	case CHUNK_FORM_PALETTE4: {
		Palette4* p = c->payload.p4;

		int slot = -1;
		for (int i = 0; i < p->pal_n; i++)
			if (p->palette[i] == id) { slot = i; break; }

		if (slot < 0 && p->pal_n < 16) {
			slot = p->pal_n++;
			p->palette[slot] = id;
		}

		if (slot >= 0) {
			setNibble(p->indices, idx, (uint8_t)slot);
			return true;
		}

		// The 17th distinct id: promote to RAW. Unpack what is here into a flat array
		// first -- chunkCopyRun still sees c in PALETTE4 form at this point, since
		// nothing has been changed yet -- then apply the one new write, and only then
		// swap the payload in. A malloc failure here leaves c fully untouched, still
		// PALETTE4, still holding exactly what it held before this call.
		BlockId* raw = (BlockId*)malloc((size_t)CHUNK_BLOCKS);
		if (!raw) return false;
		chunkCopyRun(c, 0, CHUNK_BLOCKS, raw);
		raw[idx] = id;

		free(p);
		c->form = CHUNK_FORM_RAW;
		c->payload.raw = raw;
		return true;
	}

	case CHUNK_FORM_RAW:
		c->payload.raw[idx] = id;
		return true;
	}
	return false;
}

BlockId chunkGet(const Chunk* c, int idx)
{
	switch (c->form) {
	case CHUNK_FORM_UNIFORM:  return c->uniform_id;
	case CHUNK_FORM_PALETTE4: {
		const Palette4* p = c->payload.p4;
		return p->palette[getNibble(p->indices, idx)];
	}
	case CHUNK_FORM_RAW:      return c->payload.raw[idx];
	}
	return BLOCK_AIR;
}

void chunkCopyRun(const Chunk* c, int begin, int count, BlockId* out)
{
	switch (c->form) {
	case CHUNK_FORM_UNIFORM:
		memset(out, c->uniform_id, (size_t)count);
		return;

	// Two cells per byte rather than one getNibble() per cell. The per-cell form costs a
	// load, an index halving and a shift decision on (i & 1) for every single block; this
	// pays the load and the halving once per PAIR and resolves the shift statically, because
	// after the alignment step below the low nibble is always the even cell and the high
	// nibble always the odd one -- exactly getNibble's convention, unrolled rather than
	// changed.
	//
	// Worth the extra lines because of who calls this: scratch.c asks for a CHUNK_DIM-long
	// run per axis row across 26 neighbours plus the centre for every chunk the mesher
	// builds, and step 9.2a's move to opaque chunks made that gather cost a measured
	// 145 -> 292 us per chunk. tools/bench_scratch.c times this path on the host; the form
	// mix it reports for the widest shipped ring is 114 PALETTE4 chunks to 10 UNIFORM and no
	// RAW, so this branch is very nearly the whole of that cost.
	case CHUNK_FORM_PALETTE4: {
		const Palette4* p   = c->payload.p4;
		const uint8_t*  ind = p->indices;
		const BlockId*  pal = p->palette;
		int i = 0;

		// An odd `begin` opens on a byte's HIGH nibble, so that cell is taken on its own to
		// leave the pair loop byte-aligned. scratch.c's 16-cell runs always start at a
		// multiple of CHUNK_DIM and so never take this branch; its single-cell border reads
		// at local x = CHUNK_DIM - 1 are the ones that do.
		if (count > 0 && (begin & 1)) {
			out[0] = pal[ind[begin >> 1] >> 4];
			i = 1;
		}

		const uint8_t* src = &ind[(begin + i) >> 1];
		for (; i + 1 < count; i += 2) {
			const uint8_t b = *src++;
			out[i]     = pal[b & 0x0F];
			out[i + 1] = pal[b >> 4];
		}

		// An odd-length run ends on a low nibble, by the same parity argument.
		if (i < count) out[i] = pal[*src & 0x0F];
		return;
	}

	case CHUNK_FORM_RAW:
		memcpy(out, c->payload.raw + begin, (size_t)count);
		return;
	}
}

void chunkDecompressAll(const Chunk* c, BlockId out[CHUNK_BLOCKS])
{
	chunkCopyRun(c, 0, CHUNK_BLOCKS, out);
}

// Shared by chunkFormForAll and chunkLoadAll so the two can never disagree about what a
// flat buffer's distinct-id count is: builds the palette in first-seen order and stops
// tracking, reporting -1, the moment a 17th distinct id shows up -- RAW is coming
// regardless at that point, so there is no reason to keep scanning for more of them.
static int buildPalette16(const BlockId in[CHUNK_BLOCKS], BlockId pal[16])
{
	int n = 0;
	for (int i = 0; i < CHUNK_BLOCKS; i++) {
		const BlockId id = in[i];
		bool seen = false;
		for (int j = 0; j < n; j++)
			if (pal[j] == id) { seen = true; break; }
		if (seen) continue;
		if (n == 16) return -1;
		pal[n++] = id;
	}
	return n;
}

ChunkForm chunkFormForAll(const BlockId in[CHUNK_BLOCKS])
{
	BlockId pal[16];
	const int n = buildPalette16(in, pal);
	if (n < 0) return CHUNK_FORM_RAW;
	if (n == 1) return CHUNK_FORM_UNIFORM;
	return CHUNK_FORM_PALETTE4;
}

bool chunkLoadAll(Chunk* c, const BlockId in[CHUNK_BLOCKS])
{
	BlockId pal[16];
	const int n = buildPalette16(in, pal);

	if (n == 1) {
		// Never fails: chunkClear only ever shrinks or holds steady.
		chunkClear(c, pal[0]);
		return true;
	}

	if (n > 0) {
		Palette4* p = (Palette4*)malloc(sizeof(Palette4));
		if (!p) return false;   // c untouched
		memcpy(p->palette, pal, (size_t)n * sizeof(BlockId));
		p->pal_n = (uint8_t)n;
		for (int i = 0; i < CHUNK_BLOCKS; i++) {
			int slot = 0;
			for (int j = 0; j < n; j++)
				if (pal[j] == in[i]) { slot = j; break; }
			setNibble(p->indices, i, (uint8_t)slot);
		}
		freePayload(c);
		c->form = CHUNK_FORM_PALETTE4;
		c->payload.p4 = p;
		return true;
	}

	// n < 0: more than 16 distinct ids, RAW is the only form that fits.
	BlockId* raw = (BlockId*)malloc((size_t)CHUNK_BLOCKS);
	if (!raw) return false;   // c untouched
	memcpy(raw, in, (size_t)CHUNK_BLOCKS);
	freePayload(c);
	c->form = CHUNK_FORM_RAW;
	c->payload.raw = raw;
	return true;
}

// Step 9.2d. See chunk.h for the contract -- this is a plain copy out of the Palette4 struct
// already defined above, one getNibble per cell for `index` because that is exactly what
// chunkCopyRun's PALETTE4 branch already does, just without the extra palette[] lookup that
// turns a slot number into a BlockId (the caller wants the slot number itself).
bool chunkGetPalette4(const Chunk* c, BlockId palette[16], uint8_t* pal_n, uint8_t index[CHUNK_BLOCKS])
{
	if (c->form != CHUNK_FORM_PALETTE4) return false;

	const Palette4* p = c->payload.p4;
	memcpy(palette, p->palette, (size_t)p->pal_n * sizeof(BlockId));
	*pal_n = p->pal_n;
	for (int i = 0; i < CHUNK_BLOCKS; i++) index[i] = getNibble(p->indices, i);
	return true;
}

// Step 9.2d. chunkLoadAll's PALETTE4 branch, minus buildPalette16 -- the caller already has
// both the palette and the per-cell slot numbers, so there is nothing left here to re-derive,
// only to pack.
bool chunkLoadPalette4(Chunk* c, const BlockId palette[16], uint8_t pal_n, const uint8_t index[CHUNK_BLOCKS])
{
	if (pal_n == 1) {
		chunkClear(c, palette[0]);   // never fails: chunkClear only ever shrinks or holds steady
		return true;
	}

	Palette4* p = (Palette4*)malloc(sizeof(Palette4));
	if (!p) return false;   // c untouched
	memcpy(p->palette, palette, (size_t)pal_n * sizeof(BlockId));
	p->pal_n = pal_n;
	for (int i = 0; i < CHUNK_BLOCKS; i++) setNibble(p->indices, i, index[i]);

	freePayload(c);
	c->form = CHUNK_FORM_PALETTE4;
	c->payload.p4 = p;
	return true;
}
