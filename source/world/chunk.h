// A chunk: 16 x 16 x 16 blocks, one byte each -- logically. Physically, since step 9.2a,
// a chunk is whichever of three storage forms is smallest for what is actually in it:
//
//   CHUNK_FORM_UNIFORM   one block id, everywhere. No payload allocation at all -- the
//                        chunk is just its header. This is not a rare case: half of every
//                        128-tall generated column is sky, and every sky chunk is this.
//   CHUNK_FORM_PALETTE4  <=16 distinct ids. A 16-entry palette plus one 4-bit index per
//                        cell (2,048 bytes), because most real terrain is a handful of
//                        materials repeated a lot, and 4 bits addresses 16 of them exactly.
//   CHUNK_FORM_RAW       the fallback: one byte per cell, 4,096 bytes, same layout the
//                        chunk always used before this step. Always correct, whatever a
//                        player builds -- a checkerboard of 200 distinct dyes still works,
//                        it just costs what it always cost.
//
// A chunk only ever PROMOTES (UNIFORM -> PALETTE4 -> RAW), and only on a write that
// genuinely needs the bigger form. It never demotes on its own: a write that happens to
// make a PALETTE4 chunk logically uniform again (say, overwriting the one different cell
// back to the original id) leaves it in PALETTE4 form, with one palette slot nobody points
// at any more. Demoting on every write that could shrink would mean re-deriving the whole
// chunk's palette on every single edit -- exactly the per-cell cost this step exists to
// remove -- for a case (an edit that exactly undoes an earlier one) that is not the common
// shape of play. There is no separate compaction pass either: nothing in this codebase
// currently revisits a chunk's storage on a timer or an idle tick, and adding one purely to
// shave an occasional orphaned palette slot was judged not worth the new failure surface
// (another place a promotion-shaped bug could hide) for a saving that chunkClear and
// chunkLoadAll already deliver for free every time a chunk's whole content is replaced
// wholesale (worldgen, and loading a save) rather than edited one cell at a time.
//
// Chunk is opaque on purpose. Before this step every one of world.c, scratch.c,
// visgraph.c, worldgen.c and chunk_codec.c read c->blocks[i] directly, which is exactly
// what made this step hard: any one of those reads is a silent assumption that a chunk is
// always the same fixed size. Hiding the struct means the compiler now enforces what used
// to be a convention -- nothing outside this file can take sizeof(Chunk) and mean anything
// by it, or index into a member that may not exist for the chunk's current form. Every
// caller goes through chunkGet/chunkSet for one cell, chunkCopyRun for a contiguous run
// (what scratch.c wants), or chunkDecompressAll/chunkLoadAll for the whole chunk at once
// (what visgraph.c and chunk_codec.c want).
//
// Cubic on purpose, unrelated to the point above and unchanged since Phase 7: the
// cave-culling algorithm is defined on cubic chunks with six faces and a flood fill inside
// each one; a column-shaped chunk breaks it, and finding that out in Phase 7 would have
// meant rewriting the mesher.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "world/block.h"

#define CHUNK_DIM     16
#define CHUNK_BLOCKS  (CHUNK_DIM * CHUNK_DIM * CHUNK_DIM)   // 4096 cells

// x is the contiguous axis. The mesher and the padded scratch fill both walk x
// innermost, so a run of x is a memcpy (RAW) or an unpack loop (PALETTE4) rather than
// scattered single-cell reads.
static inline int chunkIndex(int x, int y, int z)
{
	return (y * CHUNK_DIM + z) * CHUNK_DIM + x;
}

typedef enum {
	CHUNK_FORM_UNIFORM  = 0,
	CHUNK_FORM_PALETTE4 = 1,
	CHUNK_FORM_RAW      = 2,
} ChunkForm;

// Defined only in chunk.c -- see the file header for why. A pointer is a complete type,
// so world.h can hold Chunk* chunks[COLUMN_CHUNKS] without ever seeing the definition.
typedef struct Chunk Chunk;

// The exact number of bytes chunk.c will allocate in total (header plus payload blob, if
// the form has one) for a chunk in the given form. This is a pure function of `form` alone
// -- PALETTE4's palette is always 16 slots and its index array is always 2,048 bytes
// regardless of how many of the 16 slots are actually used, so there is no "how full" input
// here. Exposed so world.c can size a budget claim/release around a create, a promotion or
// a whole-chunk replace without duplicating this file's layout knowledge, and so
// world_test.c can assert the exact costs instead of trusting a comment.
size_t chunkFormBytes(ChunkForm form);

// Allocates a fresh chunk, uniform `fill`, with a plain malloc/calloc -- no budget check.
// NULL only on a genuine allocator failure. world.c is the layer that owns the 12 MB
// budget (see budget.h) and is the only caller that should ever see this return NULL in
// practice: it claims chunkFormBytes(CHUNK_FORM_UNIFORM) bytes first and only calls this
// once the claim succeeds.
Chunk* chunkAlloc(BlockId fill);

// Frees c (and its payload blob, if it has one). NULL is a silent no-op, matching free()'s
// contract. No budget interaction -- the caller releases chunkGetBytes(c) worth of budget
// around this call, the same way it claimed around the alloc.
void chunkFree(Chunk* c);

ChunkForm chunkGetForm(const Chunk* c);
size_t    chunkGetBytes(const Chunk* c);   // == chunkFormBytes(chunkGetForm(c))

// True if every cell in c currently reads `fill`. O(1) for UNIFORM and for a PALETTE4
// chunk with only one palette slot in use; a real scan only for RAW (and for a PALETTE4
// chunk whose palette holds an orphaned second slot from an undone edit -- see the file
// header). That fallback answers `false` rather than paying for the scan: an undercount
// here only costs a mesh build that could have been skipped, never one that should have
// been but was not -- this function must never return true for a chunk that is not
// actually uniform, because the caller uses it to skip building geometry.
bool chunkIsUniform(const Chunk* c, BlockId fill);

// chunkIsUniform(c, BLOCK_AIR) under its own name, because every existing caller (main.c's
// mesh-queue gate, scene/chunk_render.c's remesh skip) already spells it this way and
// neither file is this step's to edit.
//
// The mesher emits a face only for a solid cell *inside* the chunk -- neighbours are read
// for occlusion, never as a source of geometry -- so an all-air chunk is guaranteed to mesh
// to nothing whatever surrounds it, which is what makes this a sound early-out for a
// remesh: scene/chunk_render.c skips both the 27-chunk scratch fill and the mesher walk
// when this returns true.
bool chunkIsAllAir(const Chunk* c);

// Resets the WHOLE chunk to one value, always ending in CHUNK_FORM_UNIFORM. Unlike
// chunkSet below this can only shrink or hold steady, never grow, so it cannot be refused
// by a budget and has nothing to return. world.c reads chunkGetBytes(c) before and after
// to know how many bytes to release.
void chunkClear(Chunk* c, BlockId fill);

// The form c would need to be in, in addition to what it already holds, to accept a write
// of `id` -- without allocating or changing anything. world.c calls this BEFORE chunkSet,
// so a budget refusal is decided before any memory moves: it claims
// chunkFormBytes(result) - chunkGetBytes(c) (only if that is positive; a write that fits
// the current form already needs nothing) and calls chunkSet only once that claim, if any,
// succeeds.
ChunkForm chunkFormFor(const Chunk* c, BlockId id);

// Writes id at idx (0..CHUNK_BLOCKS-1, e.g. from chunkIndex), promoting c's storage if
// needed. The caller must already have secured the budget for chunkFormFor(c, id) -- this
// function does not check it and is never refused for a budget reason. Its only failure is
// a genuine allocator failure on the promotion's own malloc, which leaves c completely
// untouched (same form, same bytes, same value at every cell including idx) and returns
// false; the caller must then release whatever it had claimed for the promotion, because
// those bytes were never spent.
bool chunkSet(Chunk* c, int idx, BlockId id);

BlockId chunkGet(const Chunk* c, int idx);

// Fills out[0..count) with the blocks at linear indices [begin, begin+count) -- a
// contiguous run in chunkIndex's x-innermost order. This is the bulk primitive scratch.c
// uses in place of the old raw memcpy/memset off c->blocks: one call per axis-run (still
// either 1 or CHUNK_DIM cells, exactly as many calls as before scratchFill made), routed
// through a form-aware copy (memset for UNIFORM, an unpack loop for PALETTE4, memcpy for
// RAW) instead of assuming a flat array exists.
void chunkCopyRun(const Chunk* c, int begin, int count, BlockId* out);

// chunkCopyRun(c, 0, CHUNK_BLOCKS, out) under its own name. visgraph.c calls this ONCE per
// chunk, at the top of visChunkConnectivity, and floods the resulting flat array exactly
// the way it flooded c->blocks before this step -- see visgraph.c:17-22 for why the flood
// fill itself must stay a raw array walk with no per-cell function call. chunk_codec.c's
// encoder calls this once too, for the same reason: pay the unpack cost once, not per cell
// of the RLE/palette scan.
void chunkDecompressAll(const Chunk* c, BlockId out[CHUNK_BLOCKS]);

// The form a flat CHUNK_BLOCKS buffer would need if loaded via chunkLoadAll below --
// UNIFORM for 1 distinct id, PALETTE4 for <=16, RAW otherwise. Pure, allocates nothing;
// callers that build a whole chunk's answer before committing it (worldgen.c,
// chunk_codec.c's decoder) budget-claim chunkFormBytes(this) - chunkGetBytes(c) first, the
// same pre-flight chunkFormFor/chunkSet's caller does.
ChunkForm chunkFormForAll(const BlockId in[CHUNK_BLOCKS]);

// Replaces c's whole content with `in`, choosing whichever of the three forms
// chunkFormForAll(in) says is smallest. Unlike chunkSet this MAY shrink c -- it is a
// wholesale replace, not an incremental edit, so the "never demote on a single write" rule
// above does not apply to it. Used once per chunk by worldgen.c (which already builds a
// chunk's answer in a local buffer before committing it, so there is no reason to promote
// it cell by cell afterwards) and by chunk_codec.c's decoder. False only on a genuine
// allocator failure, in which case c is untouched; the caller budget-claims/releases the
// difference the same way it does around chunkClear.
bool chunkLoadAll(Chunk* c, const BlockId in[CHUNK_BLOCKS]);

// Step 9.2d, both added so chunk_codec.c's encoder/decoder can stop routing a chunk that is
// already (or is about to become) CHUNK_FORM_PALETTE4 through a decompress-to-BlockId-array
// and rebuild-the-palette-from-scratch round trip when the palette already exists, or is
// already known, on the other side of the call.
//
// chunkGetPalette4: false (touching neither out param) unless c is currently
// CHUNK_FORM_PALETTE4. On true, fills palette[0..*pal_n) with a copy of c's actual in-memory
// palette (see chunk.h's file header for why *pal_n can include a slot no cell currently
// references -- an orphaned slot from an undone edit -- and why that is not this function's
// problem to fix) and index[0..CHUNK_BLOCKS) with one already-unpacked slot number
// (0..*pal_n-1) per cell -- the same nibbles chunkGet's PALETTE4 case reads, just handed back
// as a flat byte array instead of one cell at a time.
bool chunkGetPalette4(const Chunk* c, BlockId palette[16], uint8_t* pal_n, uint8_t index[CHUNK_BLOCKS]);

// chunkLoadPalette4: the commit-side counterpart. Replaces c's whole content with a known
// palette of pal_n (1..16) ids and one already-unpacked slot number (0..pal_n-1) per cell in
// `index` -- chunkLoadAll's PALETTE4 commit, minus the buildPalette16 rescan that function
// pays to re-derive a palette the caller here already has (chunk_codec.c's CODEC_RLE decoder,
// straight out of the run table it just validated). Picks CHUNK_FORM_UNIFORM when pal_n == 1,
// same as chunkLoadAll would. False only on a genuine allocator failure, c left untouched.
bool chunkLoadPalette4(Chunk* c, const BlockId palette[16], uint8_t pal_n, const uint8_t index[CHUNK_BLOCKS]);
