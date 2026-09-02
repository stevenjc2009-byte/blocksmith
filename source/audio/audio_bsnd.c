#include "audio/audio_bsnd.h"

#include <string.h>

// No <3ds.h>, no allocation, no file IO. See audio_bsnd.h.

// Reads are done byte-at-a-time rather than by casting the buffer to a struct. Two
// reasons, both real on this target: the buffer comes from a file read and is only
// guaranteed 1-byte aligned, and devkitARM builds with -fshort-enums and its own
// struct padding rules, so a cast-to-struct parser would agree with the packer on the
// host and disagree on the console. This costs nothing — it runs once per sound at
// boot — and it cannot be wrong in a way that only shows up on hardware.
static uint16_t rd16(const uint8_t* p)
{
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t bsndFrameBytes(uint16_t encoding)
{
	switch (encoding) {
		case BSND_ENC_PCM8:  return 1;
		case BSND_ENC_PCM16: return 2;
		default:             return 0;
	}
}

// Table-free CRC32, the reflected PKZIP polynomial 0xEDB88320. A table would be 1 KiB of
// .rodata to save time this never spends: the whole shipped sound set is under 100 KiB
// and this runs once at boot. Kept bit-at-a-time so the constant above is the only thing
// that has to match zlib, rather than a generated table nobody can check by eye.
uint32_t bsndCrc32(const void* buf, size_t len)
{
	const uint8_t* p = (const uint8_t*)buf;
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
	}
	return crc ^ 0xFFFFFFFFu;
}

BsndResult bsndParse(const void* buf, size_t len, bool verify_crc, BsndHeader* out)
{
	if (!buf || !out) return BSND_ERR_TOO_SMALL;
	if (len < BSND_HEADER_BYTES) return BSND_ERR_TOO_SMALL;

	const uint8_t* p = (const uint8_t*)buf;

	if (p[0] != BSND_MAGIC0 || p[1] != BSND_MAGIC1 ||
	    p[2] != BSND_MAGIC2 || p[3] != BSND_MAGIC3)
		return BSND_ERR_MAGIC;

	const uint16_t version  = rd16(p + 4);
	if (version != BSND_VERSION) return BSND_ERR_VERSION;

	const uint16_t encoding = rd16(p + 6);
	const uint32_t frame_bytes = bsndFrameBytes(encoding);
	if (frame_bytes == 0) return BSND_ERR_ENCODING;

	const uint32_t rate = rd32(p + 8);
	if (rate == 0 || rate > BSND_RATE_MAX) return BSND_ERR_RATE;

	const uint32_t frames     = rd32(p + 12);
	const uint32_t data_bytes = rd32(p + 16);
	const uint32_t crc        = rd32(p + 20);

	if (frames == 0) return BSND_ERR_FRAMES;
	if (data_bytes % 4u) return BSND_ERR_ALIGN;

	// frames * frame_bytes is the sound; data_bytes is that rounded up to 4. Anything
	// else means the header disagrees with itself, and the direction that matters is
	// frames being LARGER than the payload can hold — that is the one that makes the
	// DSP read past the end of the pool. Both directions are refused anyway, because a
	// header that is wrong about its own size has no claim to be trusted about the rest.
	//
	// The multiply is done in 64 bits: frames and frame_bytes are both attacker-supplied
	// in the sense that matters (a corrupt SD read supplies them), and a 32-bit
	// frames * 2 wraps for frames >= 0x80000000 and would compare EQUAL to a small
	// data_bytes. That is exactly the overflow that turns a bad header into a read of
	// two gigabytes.
	const uint64_t sound_bytes = (uint64_t)frames * (uint64_t)frame_bytes;
	if (sound_bytes > (uint64_t)data_bytes) return BSND_ERR_FRAMES;
	if ((uint64_t)data_bytes - sound_bytes >= 4u) return BSND_ERR_FRAMES;

	if (len < (size_t)BSND_HEADER_BYTES + (size_t)data_bytes) return BSND_ERR_TRUNCATED;

	if (verify_crc && bsndCrc32(p + BSND_HEADER_BYTES, data_bytes) != crc)
		return BSND_ERR_CRC;

	out->encoding    = encoding;
	out->sample_rate = rate;
	out->frame_count = frames;
	out->data_bytes  = data_bytes;
	out->crc32       = crc;
	out->data_offset = BSND_HEADER_BYTES;
	return BSND_OK;
}

const char* bsndResultName(BsndResult r)
{
	switch (r) {
		case BSND_OK:            return "ok";
		case BSND_ERR_TOO_SMALL: return "too small";
		case BSND_ERR_MAGIC:     return "bad magic";
		case BSND_ERR_VERSION:   return "bad version";
		case BSND_ERR_ENCODING:  return "bad encoding";
		case BSND_ERR_RATE:      return "bad sample rate";
		case BSND_ERR_FRAMES:    return "bad frame count";
		case BSND_ERR_ALIGN:     return "misaligned payload";
		case BSND_ERR_TRUNCATED: return "truncated";
		case BSND_ERR_CRC:       return "checksum mismatch";
	}
	return "unknown";
}
