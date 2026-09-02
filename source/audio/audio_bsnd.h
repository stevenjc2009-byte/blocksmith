#pragma once

// BSND v1 — the on-disk container every sound effect ships in.
//
// Nothing in this file includes <3ds.h> and nothing in it allocates. It is a pure
// byte-buffer parser, for the same reason world/region.c's format code is: the half
// of a loader that can be wrong in a way you cannot see is the header arithmetic, and
// that half can be driven from the host suite with hand-built buffers in a second.
// The console half — opening romfs, reading into linear memory — is in audio.c.
//
// ── Why a custom container ────────────────────────────────────────────────────────
//
// The 3DS DSP plays PCM8, PCM16 and DSPADPCM and nothing else. Anything compressed
// means shipping a decoder into a build whose scarce resource is linear memory, and
// paying ARM11 time per sound at 60 fps. So the decode happens at BUILD time, in
// tools/make_sounds.py, and this format is deliberately the least the console can be
// handed: a fixed header, then samples that are already exactly what ndsp wants. The
// runtime cost of "decoding" a sound is a bounds check and a memcpy.
//
// DSPADPCM was considered and rejected for now. It is 4:1 against PCM16 and the DSP
// decodes it in hardware, so it is the right answer eventually — but it needs the
// per-sound predictor coefficient table plumbed through ndspWaveBuf::adpcm_data and an
// encoder this machine does not have. The format carries an `encoding` field precisely
// so adding it later is a new enum value and a new branch, not a new container.
//
// ── Layout (little-endian, which is what both the ARM11 and the host are) ──────────
//
//   off  size  field
//     0     4  magic, "BSND"
//     4     2  version, 1
//     6     2  encoding, one of BSND_ENC_*
//     8     4  sample_rate in Hz
//    12     4  frame_count — samples, NOT bytes
//    16     4  data_bytes — payload length, always a multiple of 4
//    20     4  crc32 of the payload (zlib/PKZIP polynomial)
//    24  ....  payload
//
// data_bytes is padded up to a multiple of 4 by the packer, so frame_count is the
// authority on how much of the payload is sound and data_bytes is the authority on
// how much memory it occupies. Confusing the two is the bug this comment exists to
// prevent: at PCM8 they differ by up to 3 samples of silence, and a voice told to play
// data_bytes frames of PCM8 reads 3 bytes past the sound.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BSND_MAGIC0 'B'
#define BSND_MAGIC1 'S'
#define BSND_MAGIC2 'N'
#define BSND_MAGIC3 'D'

#define BSND_VERSION      1
#define BSND_HEADER_BYTES 24

// Values match libctru's NDSP_ENCODING_* so the console backend can pass them through
// without a translation table that could drift. That coupling is deliberate and is
// asserted in audio_ndsp.c rather than left as a hope.
enum {
	BSND_ENC_PCM8  = 0,
	BSND_ENC_PCM16 = 1,
};

typedef struct {
	uint16_t encoding;      // BSND_ENC_*
	uint32_t sample_rate;   // Hz
	uint32_t frame_count;   // samples
	uint32_t data_bytes;    // payload length, multiple of 4
	uint32_t crc32;         // of the payload
	uint32_t data_offset;   // always BSND_HEADER_BYTES; named so callers do not hardcode it
} BsndHeader;

// Why the parse can refuse, in the order it checks. The caller logs the reason; nothing
// branches on it beyond "not BSND_OK means do not play this".
typedef enum {
	BSND_OK = 0,
	BSND_ERR_TOO_SMALL,      // fewer than BSND_HEADER_BYTES bytes handed in
	BSND_ERR_MAGIC,          // not a BSND file at all
	BSND_ERR_VERSION,        // a version this build does not know
	BSND_ERR_ENCODING,       // an encoding this build does not know
	BSND_ERR_RATE,           // sample_rate zero or absurd
	BSND_ERR_FRAMES,         // frame_count zero, or inconsistent with data_bytes
	BSND_ERR_ALIGN,          // data_bytes not a multiple of 4
	BSND_ERR_TRUNCATED,      // buffer shorter than header + data_bytes
	BSND_ERR_CRC,            // payload does not match the stored checksum
} BsndResult;

// Anything above this is refused rather than trusted. 48 kHz is already above what the
// 3DS's own output rate (32728.5 Hz) can reproduce; the ceiling exists to stop a corrupt
// header handing ndspChnSetRate a ratio that makes the DSP read the buffer at speed.
#define BSND_RATE_MAX 48000u

// Bytes one frame occupies in the payload for `encoding`. Zero for an unknown encoding,
// which is how callers that skipped bsndParse still fail safe rather than dividing by it.
uint32_t bsndFrameBytes(uint16_t encoding);

// Parses `len` bytes at `buf` into `out`. `out` is untouched unless BSND_OK is returned,
// so a caller cannot half-read a rejected file.
//
// The CRC is checked only when `verify_crc` is true. It is true on the console (an SD
// card read is the thing most likely to hand back plausible-looking garbage) and can be
// turned off in tests that want to prove a different field is what rejected a buffer.
BsndResult bsndParse(const void* buf, size_t len, bool verify_crc, BsndHeader* out);

// The exact CRC32 tools/make_sounds.py writes (zlib.crc32 — the PKZIP polynomial,
// reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF). Exposed because the test suite has
// to be able to build a VALID buffer as well as a broken one; a test that could only
// produce files this parser rejects would prove nothing about the accept path.
uint32_t bsndCrc32(const void* buf, size_t len);

// Human-readable form of a BsndResult, for the one log line audio.c prints per refused
// sound. Never NULL, including for a value outside the enum.
const char* bsndResultName(BsndResult r);
