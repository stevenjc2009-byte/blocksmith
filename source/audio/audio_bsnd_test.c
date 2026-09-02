// Host self-test for the two pure-maths halves of the audio subsystem: the BSND
// container parser (audio_bsnd.c) and the positional pan curve (audio_pan.c).
//
// Both are the real modules. Neither needs a fake of anything — that is the point of
// having split them out of the loader and the mixer.
#ifndef __3DS__

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_bsnd.h"
#include "audio/audio_pan.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                               \
		if (!(cond)) {                                                            \
			s_fails++;                                                            \
			if (!s_first[0])                                                      \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                         \
	} while (0)

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

// <math.h> only exposes M_PI when _POSIX_C_SOURCE or similar is defined, and this suite
// compiles with -std=c11, which does not. Spelled out here rather than reached for via a
// feature-test macro so the same source compiles under devkitARM's newlib too.
#define TEST_PI 3.14159265358979323846f

// ── Building buffers ──────────────────────────────────────────────────────────────

#define TEST_FRAMES 64
#define TEST_PAYLOAD (TEST_FRAMES * 2)
static uint8_t s_buf[BSND_HEADER_BYTES + TEST_PAYLOAD];

static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Builds a VALID PCM16 buffer. The test suite has to be able to produce a file this
// parser ACCEPTS as well as ones it rejects — a suite that could only make broken files
// would prove nothing about the accept path, and every rejection check below would pass
// against a parser that rejected everything.
static void buildValid(void)
{
	memset(s_buf, 0, sizeof(s_buf));
	s_buf[0] = 'B'; s_buf[1] = 'S'; s_buf[2] = 'N'; s_buf[3] = 'D';
	wr16(s_buf + 4, BSND_VERSION);
	wr16(s_buf + 6, BSND_ENC_PCM16);
	wr32(s_buf + 8, 22050);
	wr32(s_buf + 12, TEST_FRAMES);
	wr32(s_buf + 16, TEST_PAYLOAD);
	for (int i = 0; i < TEST_PAYLOAD; i++)
		s_buf[BSND_HEADER_BYTES + i] = (uint8_t)(i * 7 + 3);
	wr32(s_buf + 20, bsndCrc32(s_buf + BSND_HEADER_BYTES, TEST_PAYLOAD));
}

static BsndResult parseBuf(size_t len, bool crc)
{
	BsndHeader h;
	memset(&h, 0xAA, sizeof(h));
	return bsndParse(s_buf, len, crc, &h);
}

// ── Parser tests ──────────────────────────────────────────────────────────────────

static void testValidParses(void)
{
	buildValid();
	BsndHeader h;
	memset(&h, 0, sizeof(h));
	CHECK(bsndParse(s_buf, sizeof(s_buf), true, &h) == BSND_OK);
	CHECK(h.encoding == BSND_ENC_PCM16);
	CHECK(h.sample_rate == 22050);
	CHECK(h.frame_count == TEST_FRAMES);
	CHECK(h.data_bytes == TEST_PAYLOAD);
	CHECK(h.data_offset == BSND_HEADER_BYTES);
}

// The CRC in this parser must be the SAME CRC tools/make_sounds.py writes. If the two
// ever diverge, every real sound is rejected on the console and nothing here would
// notice — both halves would be self-consistent. This vector is zlib.crc32(b"123456789),
// the standard CRC-32 check value, computed independently of this implementation.
static void testCrcMatchesZlib(void)
{
	CHECK(bsndCrc32("123456789", 9) == 0xCBF43926u);
	CHECK(bsndCrc32("", 0) == 0u);
}

static void testFrameBytes(void)
{
	CHECK(bsndFrameBytes(BSND_ENC_PCM8) == 1);
	CHECK(bsndFrameBytes(BSND_ENC_PCM16) == 2);
	CHECK(bsndFrameBytes(9999) == 0);
}

static void testRejections(void)
{
	buildValid();
	CHECK(parseBuf(BSND_HEADER_BYTES - 1, true) == BSND_ERR_TOO_SMALL);
	CHECK(bsndParse(NULL, 100, true, NULL) == BSND_ERR_TOO_SMALL);

	buildValid(); s_buf[1] = 'X';
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_MAGIC);

	buildValid(); wr16(s_buf + 4, BSND_VERSION + 1);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_VERSION);

	buildValid(); wr16(s_buf + 6, 7);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_ENCODING);

	buildValid(); wr32(s_buf + 8, 0);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_RATE);
	buildValid(); wr32(s_buf + 8, BSND_RATE_MAX + 1);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_RATE);

	buildValid(); wr32(s_buf + 12, 0);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_FRAMES);

	buildValid(); wr32(s_buf + 16, TEST_PAYLOAD - 2);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_ALIGN);

	// frame_count larger than the payload can hold. This is the one that, unchecked,
	// makes the DSP read past the end of the linear pool.
	buildValid(); wr32(s_buf + 12, TEST_FRAMES * 4);
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_FRAMES);

	// The 32-bit overflow: frames * 2 wraps to exactly TEST_PAYLOAD for this value, so a
	// parser multiplying in 32 bits accepts it and then plays two gigabytes of whatever
	// follows the pool. 0x80000000 + TEST_PAYLOAD/2 doubled is TEST_PAYLOAD mod 2^32.
	buildValid(); wr32(s_buf + 12, 0x80000000u + (TEST_PAYLOAD / 2));
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_FRAMES);

	// Header says more payload than the buffer holds.
	buildValid();
	CHECK(parseBuf(BSND_HEADER_BYTES + TEST_PAYLOAD - 4, true) == BSND_ERR_TRUNCATED);

	buildValid(); s_buf[BSND_HEADER_BYTES] ^= 0xFF;
	CHECK(parseBuf(sizeof(s_buf), true) == BSND_ERR_CRC);
	// ...and the same buffer passes with verification off, which proves the CRC check is
	// what rejected it rather than some other field this corruption disturbed.
	CHECK(parseBuf(sizeof(s_buf), false) == BSND_OK);
}

// A rejected parse must leave the caller's header untouched, so a caller that ignored the
// return cannot half-read a bad file into a live sample.
static void testRejectionLeavesOutputUntouched(void)
{
	buildValid();
	s_buf[0] = 'X';
	BsndHeader h;
	memset(&h, 0x5A, sizeof(h));
	CHECK(bsndParse(s_buf, sizeof(s_buf), true, &h) != BSND_OK);
	CHECK(h.frame_count == 0x5A5A5A5Au);
	CHECK(h.data_offset == 0x5A5A5A5Au);
}

static void testResultNamesAreAlwaysUsable(void)
{
	for (int i = 0; i <= BSND_ERR_CRC; i++) {
		const char* n = bsndResultName((BsndResult)i);
		CHECK(n != NULL && n[0] != '\0');
	}
	CHECK(bsndResultName((BsndResult)999) != NULL);
}

// ── Pan tests ─────────────────────────────────────────────────────────────────────

static void testDistanceCurve(void)
{
	CHECK(near(audioDistanceGain(0.0f), 1.0f));
	CHECK(near(audioDistanceGain(AUDIO_REF_DIST), 1.0f));
	CHECK(near(audioDistanceGain(AUDIO_MAX_DIST), 0.0f));
	CHECK(near(audioDistanceGain(AUDIO_MAX_DIST + 100.0f), 0.0f));
	// Midpoint of the linear ramp: ref=2, max=24, so 13 blocks is exactly half.
	CHECK(near(audioDistanceGain(13.0f), 0.5f));
	// Monotonically decreasing across the whole range — a curve that dipped and came
	// back would pass the four endpoints above.
	float prev = 2.0f;
	for (float d = 0.0f; d < 30.0f; d += 0.25f) {
		const float g = audioDistanceGain(d);
		CHECK(g <= prev + 1e-6f);
		prev = g;
	}
	// NaN in must be silence out, never a NaN handed to the DSP.
	volatile float zero = 0.0f;
	CHECK(audioDistanceGain(zero / zero) == 0.0f);
}

// The right vector at the four cardinal yaws, worked out by hand against the convention
// in audio_pan.h rather than against the implementation.
static void testListenerCardinals(void)
{
	AudioListener l;
	audioListenerFromYaw(&l, 0, 0, 0, 0.0f);
	CHECK(near(l.right_x, 1.0f) && near(l.right_z, 0.0f));
	audioListenerFromYaw(&l, 0, 0, 0, TEST_PI / 2.0f);
	CHECK(near(l.right_x, 0.0f) && near(l.right_z, 1.0f));
	audioListenerFromYaw(&l, 0, 0, 0, TEST_PI);
	CHECK(near(l.right_x, -1.0f) && near(l.right_z, 0.0f));
	audioListenerFromYaw(&l, 0, 0, 0, 3.0f * TEST_PI / 2.0f);
	CHECK(near(l.right_x, 0.0f) && near(l.right_z, -1.0f));
}

static void testPanSides(void)
{
	AudioListener l;
	audioListenerFromYaw(&l, 0, 0, 0, 0.0f);   // right is +X
	float lft, rgt;

	// Hard right, inside the reference distance so attenuation is 1.
	audioPanCompute(&l, 1.0f, 0, 0, 1.0f, &lft, &rgt);
	CHECK(near(lft, 0.0f));
	CHECK(near(rgt, 1.0f));

	// Hard left.
	audioPanCompute(&l, -1.0f, 0, 0, 1.0f, &lft, &rgt);
	CHECK(near(lft, 1.0f));
	CHECK(near(rgt, 0.0f));

	// Dead ahead: constant power puts both at sqrt(0.5), NOT at 1.0.
	audioPanCompute(&l, 0.0f, 0, -1.0f, 1.0f, &lft, &rgt);
	CHECK(near(lft, 0.70710678f));
	CHECK(near(rgt, 0.70710678f));

	// Directly behind pans the same as directly ahead — two speakers cannot say otherwise.
	audioPanCompute(&l, 0.0f, 0, 1.0f, 1.0f, &lft, &rgt);
	CHECK(near(lft, 0.70710678f));
	CHECK(near(rgt, 0.70710678f));

	// Constant power: the summed power is 1 at every angle, which is the property the
	// curve exists for. Four endpoints would not catch a curve that sagged between them.
	for (float a = 0.0f; a < 6.28f; a += 0.1f) {
		audioPanCompute(&l, cosf(a), 0.0f, sinf(a), 1.0f, &lft, &rgt);
		CHECK(near(lft * lft + rgt * rgt, 1.0f));
	}
}

static void testPanEdgeCases(void)
{
	AudioListener l;
	audioListenerFromYaw(&l, 5.0f, 5.0f, 5.0f, 0.0f);
	float lft, rgt;

	// A sound at the listener's exact position has no direction. It must be centred, not
	// a NaN from normalising a zero vector.
	audioPanCompute(&l, 5.0f, 5.0f, 5.0f, 1.0f, &lft, &rgt);
	CHECK(near(lft, 0.70710678f) && near(rgt, 0.70710678f));
	CHECK(lft == lft && rgt == rgt);        // not NaN

	// Directly overhead must not pan sideways: height feeds distance, never left/right.
	audioPanCompute(&l, 5.0f, 9.0f, 5.0f, 1.0f, &lft, &rgt);
	CHECK(near(lft, rgt));

	// Out of range is silent, and audioOutOfRange must agree with the curve about it.
	CHECK(audioOutOfRange(&l, 5.0f + AUDIO_MAX_DIST, 5.0f, 5.0f) == true);
	audioPanCompute(&l, 5.0f + AUDIO_MAX_DIST, 5.0f, 5.0f, 1.0f, &lft, &rgt);
	CHECK(lft == 0.0f && rgt == 0.0f);
	CHECK(audioOutOfRange(&l, 6.0f, 5.0f, 5.0f) == false);
	CHECK(audioOutOfRange(NULL, 0, 0, 0) == true);

	// Zero and negative caller gain are silence, not a sign flip.
	audioPanCompute(&l, 6.0f, 5.0f, 5.0f, 0.0f, &lft, &rgt);
	CHECK(lft == 0.0f && rgt == 0.0f);
	audioPanCompute(&l, 6.0f, 5.0f, 5.0f, -3.0f, &lft, &rgt);
	CHECK(lft == 0.0f && rgt == 0.0f);

	// Gain above 1 is clamped, not amplified.
	audioPanCompute(&l, 6.0f, 5.0f, 5.0f, 50.0f, &lft, &rgt);
	CHECK(lft <= 1.0f && rgt <= 1.0f);

	// A NULL listener is centred silence rather than a crash.
	audioPanCompute(NULL, 1, 2, 3, 1.0f, &lft, &rgt);
	CHECK(lft == 0.0f && rgt == 0.0f);
}

int main(void)
{
	testValidParses();
	testCrcMatchesZlib();
	testFrameBytes();
	testRejections();
	testRejectionLeavesOutputUntouched();
	testResultNamesAreAlwaysUsable();

	testDistanceCurve();
	testListenerCardinals();
	testPanSides();
	testPanEdgeCases();

	if (s_fails == 0)
		printf("audio format self-test: PASS  %d checks\n", s_checks);
	else
		printf("audio format self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int audio_bsnd_test_host_only_t;

#endif   // !__3DS__
