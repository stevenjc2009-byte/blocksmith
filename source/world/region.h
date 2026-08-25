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
// thing that makes a torn write unrecoverable.
//
// ── v1.8.2: that waste is now reclaimed, and here is what it cost to get there ────────
//
// This paragraph said, through v1.7.1 and v1.8.1, that nothing reclaimed the waste. It was
// true: regionCompact() had no caller in any shipped build, so a heavily edited world's
// region file grew on the card until the player deleted the world.
//
// **How bad it was**, measured by source/world/region_growth_test.c driving the real
// region.c through 960 column saves over a 24-column working set inside one region — one
// evening of building in one place: the file reached **482,885 bytes on disk holding 19,452
// bytes of live columns, 95.9 % of the arena dead**, and it was still climbing linearly at
// the end of the run. There is no plateau in an append-only arena; the ceiling is the card.
// With regionMaintain() called after each save the same workload finished at **43,883
// bytes**, 11x smaller, having compacted 36 times.
//
// **The exact workload, and where in it that number is taken**, because this paragraph has
// already been wrong once for want of saying so. It is arm 2 of region_growth_test.c: 24
// columns at DISTINCT directory slots inside region (0,0), each re-saved 40 times = 960 saves,
// payload lengths produced by the real chunkEncode over generated terrain with churn =
// 8 + pass*6 + column scattered single-block edits on top, and regionMaintain() called after
// every one of the 960 saves. The number is a stat() of r.0.0.bsr taken AFTER the last save
// and after the regionMaintain that follows it — final, not peak. In this arm peak and final
// are the same 43,883 bytes: the file oscillates around twice-live instead of climbing, so
// there is no later high-water mark being missed. Bit-identical on three consecutive runs.
//
// **This said 21,763 bytes, 22x, 56 compactions until 2026-08-25, and that was wrong.** Those
// are the numbers the first-draft fixture printed, before spreadColumns() asserted its 24 slots
// were distinct: it placed columns at cx=(i*7)&15, cz=(i*5)&15, whose period in i is 16, so
// columns 16..23 sat on the slots of 0..7 and only 16 columns were really live. Less live data
// makes regionCompact's half-dead gate fire sooner, which is why that run compacted 56 times
// and settled smaller. That run's own control_readback was RED — 8 of 24 columns wrong, in both
// arms — so those figures were never sound. Reproduced deliberately on 2026-08-25 by building a
// COPY of region_growth_test.c with the old placement restored, against an untouched region.c:
// 482,885 / 21,763 / 56 compactions, control_readback 8 of 24 wrong, exit 1. The uncompacted arm
// is unaffected by the fixture bug — the same 960 payloads are appended either way — which is
// why 482,885 and 19,452 were right all along and only the compacted figure moved.
//
// **Where it is called from.** app/worker.c's workerWriteSave, straight after
// regionWriteColumn, on the worker thread. Not world close: the number of regions to rewrite
// at quit is unbounded, so the stall would be, and it would put the file swap at the one
// moment a player is most likely to close the lid. The gate inside regionCompact makes the
// cost amortised — the arena has to grow by its own live size before another rewrite is
// worth doing — so the extra bytes written over a session are bounded by roughly twice the
// bytes saved. 36 rewrites across 960 saves in the measured run — counted as "the file got
// smaller across the regionMaintain call", which is the only thing the test can see from
// outside region.c.
//
// **The window that kept it unwired is closed, not accepted.** The old sequence removed the
// .bsr and then renamed the .tmp onto it, leaving an instant with no file at the path and
// 256 columns living only in a .tmp that nothing here fsyncs. It now renames the old file
// aside to .bak first and removes it last, so recovery never depends on freshly written
// bytes being on the card — see regionRecover in region.c for the full resolution table. The
// .bsr format is byte-for-byte unchanged and REGION_VERSION is untouched: a world written by
// this build loads on v1.7.1 and the other way round. The one asymmetry worth knowing is that
// a v1.7.1 build would not understand a .bak, so a downgrade performed in the microseconds
// between the two renames would see a region with no .bsr and regenerate it from the seed.
//
// **Still not measured, and it is the honest half.** Every number here is a host number on
// ext4 through WSL. A rewrite of a fully built 256-column region moves on the order of 200 KB
// each way, and nobody has timed that on a FAT32 SD card behind libctru's devoptab at
// 268 MHz. If it is slow enough, three column saves landing inside one rewrite would fill
// worker.c's two-slot save ring and make the main thread wait; workerSaveWaits() in the debug
// overlay is where that would show up.
//
// What v1.7.1 task 48c proved about the function itself, all still true:
//
// What was proved about it (host suite, world_test.c's four testRegionCompact* tests, and
// tools/run_host_tests.sh carries the sabotage arms for every claim):
//
//   * It preserves every column byte for byte and the file really does get smaller. Measured
//     on the task-48c fixture (eight columns, six of them rewritten three more times, so 26
//     payloads in the arena and 8 of them live): 9289 bytes down to 7134. The arena itself —
//     the part compaction can actually touch — goes 3113 down to 958, i.e. 69 % reclaimed;
//     the whole-file figure is smaller only because the two fixed 3088-byte directory copies
//     are 6176 bytes that never move, and they dominate a region this lightly filled.
//   * Interrupted at each of its stages it leaves either the complete old region or the
//     complete new one. regionRecover() resolves what a cut leaves behind: a .tmp beside a
//     .bsr is a rewrite that did not finish, so the .tmp is dropped; a .tmp with no .bsr is
//     a rewrite that finished all but the rename, so the .tmp is promoted.
//   * Its two regionCacheClose() calls are together sufficient against the load-side cache,
//     and are deliberately redundant — neither is red on its own.
//   * It had a real defect, found by writing those tests and fixed in region.c: it read only
//     the live directory copy, so a column whose newest payload was torn by an earlier power
//     cut — the exact case dirLoadPair's fallback exists to survive — was dropped from the
//     repack and its recoverable older copy deleted along with the old file. It now takes the
//     same two-entry fallback reads take.
//
// The rename() the swap depends on is still an unverified property of libctru's sdmc
// devoptab. It fails safely now, which it did not before: if rename() does not work on that
// devoptab at all, the FIRST rename below fails and nothing has been touched — the old .bsr
// is still there and still correct, and regionCompact returns false. The old sequence had
// already removed the .bsr by the time it found out.
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

// Rewrites a region file with the dead payloads dropped, carrying forward exactly what
// regionReadColumn would have served for every slot — including the older directory copy's
// version of a column whose newest payload an earlier power cut tore. Safe to interrupt: the
// old file is renamed aside to .bak and removed only once the replacement is in place, and
// regionRecover() resolves whichever set of files a cut leaves behind. False if there was
// nothing worth reclaiming (below half the arena wasted) or the rewrite failed, in which case
// the original is still there and still correct.
//
// Callers want regionMaintain below rather than this. Proved by world_test.c's four
// testRegionCompact* tests.
bool regionCompact(const char* world_dir, int32_t rx, int32_t rz);

// v1.8.2. Call after saving a column. Compacts that region if — and only if — at least half
// its payload arena is dead, and does nothing at all otherwise, which is twenty-six saves out of
// twenty-seven in the measured workload (960/36, and it read "sixteen out of seventeen" while
// that compaction count was the fixture-inflated 56). Cheap when it declines: three file-existence probes,
// one open and two 3088-byte directory reads.
//
// This is the wiring, and it lives in region.c rather than as a bare regionCompact call in
// app/worker.c so that a host test can drive the same line the game does. Measured by
// source/world/region_growth_test.c — see the header comment at the top of this file for the
// numbers and for what the host cannot measure.
void regionMaintain(const char* world_dir, int32_t rx, int32_t rz);

// ── v1.7.1 task 48: the load-side region cache ────────────────────────────────────────
//
// The same answer as regionReadColumn, but it keeps the region file open and its parsed
// directory in memory between calls instead of re-deriving both every time. That matters
// because the cost regionReadColumn pays is per *column* while the thing it pays for is per
// *region*: filling the view at RENDER_DIST_MAX submits 81 columns, and all 81 land in one
// or two region files, so 79 of those 81 opens and 158 of those 162 directory reads are the
// same bytes read again.
//
// Measured on the host at -O1 over 16200 column reads (the 81-column area, 200 times), all 81
// columns saved so both arms do the payload read as well and differ only by the open and the
// parse: 39.2 / 43.5 / 56.0 us per column uncached across three runs against 3.34 / 3.34 /
// 3.64 cached, i.e. one 81-column area falling from 3.2-4.5 ms to 0.27-0.29 ms. The counts
// are not noisy at all and are the honest half of the claim: 32400 fopen and 32400 dirRead
// uncached, 2 and 2 cached. The host has a page cache and the console has a FAT32 SD card, on
// which the open is the more expensive half — so this understates the saving rather than
// overstating it, and none of it is a hardware measurement.
//
// This is why loading a world that has been *edited* took roughly twice as long as creating
// one: a fresh world has no region file at all, so none of this ran.
//
// Only the worker's load-before-generate path uses this. regionReadColumn is left exactly as
// it was and every crash test still goes through it, because those tests rewrite the region
// file behind region.c's back — truncating it, scribbling sectors into it — and a cache is by
// definition wrong about a file that changed without it being told.
//
// The cache is a file-static inside region.c rather than something the caller owns, so that
// invalidation cannot be forgotten: every writer in region.c goes through one place that
// drops it, and there is no way to write a region file from outside that translation unit.
//
// Not thread-safe, and it does not need to be: both callers (the load path and the save
// path) run on the worker thread, and regionCacheClose is called from the main thread only
// once the worker has been joined.
uint32_t regionReadColumnCached(const char* world_dir, int32_t cx, int32_t cz,
                                uint8_t* out, uint32_t cap);

// Drops every cached entry and closes the handles. Call it when the world directory changes
// or the worker is stopped: an entry left behind holds a FILE* open on a world nobody is
// playing, which on a FAT card is a handle that can stop the directory being removed. Safe
// to call when nothing is cached, and safe to call twice.
void regionCacheClose(void);

#ifdef BS_REGION_PROBE
// Probe build only (tools/t48_region_probe.c). These count the two operations the cache
// exists to remove, so "the cache skipped the open and both directory reads" can be measured
// rather than argued from the diff. Never compiled into the game or the host suite.
extern unsigned long g_region_fopens;
extern unsigned long g_region_dirreads;
extern const unsigned long g_region_cache_bytes;
#endif

// Region coordinate for a column coordinate. Arithmetic shift, so it floors on the negative
// side — plain division would fold -1 and 0 into the same region and put two different
// columns in one slot.
static inline int32_t regionOf(int32_t c) { return c >> 4; }
