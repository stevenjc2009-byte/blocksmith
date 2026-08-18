// Step 8.1. Turning a chunk into bytes and back.
//
// Split out from the region file on purpose. This half is pure arithmetic over a Chunk and
// a byte buffer with no file, no path and no <3ds.h>, so every branch of it — including the
// ones that only fire on a corrupt file — is reachable from the host test suite. The region
// file (world/region.h) is the half that knows about SD cards.
//
// ── The three encodings, and why there are three ──────────────────────────────────────
//
// Real terrain is layered: a chunk is usually a few long runs of stone, then dirt, then
// grass, then a lot of air. Run-length encoding over a small palette is close to ideal for
// that and typically lands a 4,096-byte chunk in a few hundred bytes.
//
// But RLE has a worst case worse than doing nothing — a 3D checkerboard is 4,096 runs of
// length one, which is 8,192 bytes of run data to store 4,096 bytes of blocks. So the
// encoder tries all three forms and keeps the smallest, and CHUNK_CODEC_MAX is sized for
// the raw form rather than for the RLE one. A player *can* build a checkerboard.
//
//   CODEC_UNIFORM  one block id, no run data at all           2 bytes
//   CODEC_RLE      palette, then (length, palette index) runs  typically 20-400 bytes
//   CODEC_RAW      the 4,096 bytes as they sit in memory       4,098 bytes
//
// The uniform case is not an optimisation of the RLE case, it is the common case: with the
// world 128 blocks tall and the terrain around y=64, over half of every column is sky.
//
// ── What this format promises the loader ─────────────────────────────────────────────
//
// Decoding validates everything it reads and never writes outside the chunk. A truncated
// buffer, a length that overruns, a palette index past the end of the palette and a run
// table that does not add up to exactly CHUNK_BLOCKS are all rejected rather than clamped.
// That matters because the input is a file that may have been half-written when the console
// lost power, and a decoder that clamps would turn a torn save into a world that loads
// looking almost right.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world/chunk.h"

enum {
	CODEC_UNIFORM = 0,
	CODEC_RLE     = 1,
	CODEC_RAW     = 2,
};

// The most any chunk can encode to: the raw form's tag byte, its block id count and the
// 4,096 blocks. The encoder never returns more than this, so a caller can size one buffer
// and stop checking.
#define CHUNK_CODEC_MAX  (CHUNK_BLOCKS + 2)

// Encodes `c` into `out`, returning the number of bytes written, or 0 if `cap` is smaller
// than the encoding needed. Pass a buffer of CHUNK_CODEC_MAX and 0 becomes impossible.
size_t chunkEncode(const Chunk* c, uint8_t* out, size_t cap);

// Decodes `len` bytes into `c`. False on anything malformed, and on false `c` is left
// untouched rather than half-filled — a caller that ignores the return value gets the
// chunk it had, not a chunk with a corrupt band through the middle of it.
bool chunkDecode(Chunk* c, const uint8_t* in, size_t len);
