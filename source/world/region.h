// Step 8.1. The save file: one region file per 16x16 patch of columns.
//
// A file per column would be 289 files open and closed as the player walks, on a FAT32 SD
// card where opening a file is the expensive operation. A single file for the world would
// have to be rewritten whole every time one column changed. A region file is the middle
// one, and 16x16 is chosen so the file covers 256 columns — comfortably more than the 289
// the largest render distance holds, so a player standing still touches one or two files
// and never a hundred.
//
// ── Surviving a power cut, which is the whole verification criterion for this step ────
//
// The 3DS has no shutdown hook worth trusting: the battery can be pulled, the console can
// be closed and drained, and the SD card can be removed. So the format is built so that
// *every* interrupted write leaves a file that still loads — showing either the new state
// or the old one, never a mixture and never garbage.
//
// Two things make that true:
//
//   1. **Payloads are append-only.** A column's new bytes go on the end of the file. The
//      bytes the loader is currently using are never overwritten, so a power cut during a
//      payload write damages only space nothing points at yet.
//
//   2. **The directory is double-buffered.** Two fixed-size copies sit at the front of the
//      file, each with a sequence number and a CRC over its entries. A save writes the copy
//      that is *not* live, with seq+1. The loader takes the copy that has a valid magic, a
//      valid CRC and the higher sequence number. A power cut during the directory write
//      corrupts the copy that nothing was reading, and the other one still describes a
//      complete world.
//
// Each directory entry carries a CRC of its payload as well, so a payload that was appended
// but never pointed at — or one that was pointed at by a directory write that landed while
// the payload write did not — is detected instead of decoded.
//
// The cost of append-only is a file that grows every time a column is saved. That is a
// deliberate trade: steve's instruction for this phase was explicitly not to worry about
// storage, and the alternative — writing a column back over its old bytes — is exactly the
// thing that makes a torn write unrecoverable. regionCompact() reclaims the waste when a
// world is closed, and is the only operation in here that is not crash-safe by itself,
// which is why it writes a new file and only removes the old one once the new one is
// complete on the card.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk_codec.h"
#include "world/world.h"

// Columns per region file along each axis. 16 x 16 = 256 columns per file.
#define REGION_DIM     16
#define REGION_COLS    (REGION_DIM * REGION_DIM)

// Bumped only for a format change that an old loader could misread. A file with a version
// this build does not know is ignored, not guessed at: the world regenerates from the seed,
// which loses player edits but cannot corrupt anything.
#define REGION_VERSION 1

// The most one column can encode to: eight chunks at the codec's worst case, plus the
// column header and per-chunk lengths. Callers size their staging buffer with this.
#define REGION_COL_MAX (COLUMN_CHUNKS * (CHUNK_CODEC_MAX + 2) + 16)

// Where the save files live. A world is a directory, so step 8.4's world select is a
// directory listing rather than a format change.
#define REGION_ROOT    "sdmc:/blocksmith/worlds"

// Size and placement of the two directory copies. Public only so the crash tests can aim at
// a specific copy — no game code needs them, and region.c derives its own layout from these
// so the two cannot drift apart.
#define REGION_DIR_HDR_BYTES 16
#define REGION_DIR_ENT_BYTES 12
#define REGION_DIR_BYTES     (REGION_DIR_HDR_BYTES + REGION_COLS * REGION_DIR_ENT_BYTES)
#define REGION_ARENA_OFF     (2 * REGION_DIR_BYTES)

// Serialises every allocated chunk of `col` into `out`, returning bytes written or 0 if it
// would not fit. Pure memory work, no file access — the caller does this on whichever
// thread owns the World, and hands the bytes to the writer.
uint32_t regionEncodeColumn(const Column* col, uint8_t* out, uint32_t cap);

// The reverse: fills `w`'s column at (cx, cz), allocating chunks as needed. False if the
// bytes are malformed or an allocation was refused; on false the caller must treat the
// column as absent and generate it instead.
bool regionDecodeColumn(World* w, int32_t cx, int32_t cz, const uint8_t* in, uint32_t len);

// Writes one column's bytes into the region file covering (cx, cz), creating the file if
// needed. False on any IO failure, which the caller counts rather than retries — a failed
// save must not stall the frame, and the column is regenerable.
//
// `world_dir` is a directory under REGION_ROOT, e.g. "sdmc:/blocksmith/worlds/default".
bool regionWriteColumn(const char* world_dir, int32_t cx, int32_t cz,
                       const uint8_t* data, uint32_t len);

// Reads one column's bytes into `out`. Returns the length, or 0 when this column has never
// been saved, when the file does not exist, or when the stored copy fails its checksum —
// all three mean the same thing to the caller: generate it from the seed.
uint32_t regionReadColumn(const char* world_dir, int32_t cx, int32_t cz,
                          uint8_t* out, uint32_t cap);

// Rewrites a region file with the dead payloads dropped. Safe to interrupt: the original is
// removed only after the replacement is closed. False if there was nothing to do or the
// rewrite failed, in which case the original is still there and still correct.
bool regionCompact(const char* world_dir, int32_t rx, int32_t rz);

// Region coordinate for a column coordinate. Arithmetic shift, so it floors on the negative
// side — plain division would fold -1 and 0 into the same region and put two different
// columns in one slot.
static inline int32_t regionOf(int32_t c) { return c >> 4; }
