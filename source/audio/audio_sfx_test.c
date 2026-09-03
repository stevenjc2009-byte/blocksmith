// Host self-test for source/audio/audio_sfx.c/.h — v1.8.17 lane SOUND-A.
//
// audio_sfx.c itself needed NO new code for the six slots this lane added: audioSfxRegister/
// audioSfxId/audioSfxReset/audioSfxPlayAtBlock were already written generically against
// SFX_SLOT_COUNT (see audio_sfx.c's own comment on why an unsigned cast replaced the usual
// range-check pair), so growing the enum from 3 members to 9 is the whole diff to that file.
// What this suite actually has to prove is the two things that ARE new:
//
//   1. The nine slots the enum now names still round-trip through the same generic table —
//      nothing about the new slots needed special-casing, and nothing an old slot did is
//      now broken by the table being bigger.
//   2. The six new .bsnd files this lane's tools/make_sfx_synth.py + tools/make_sounds.py
//      pipeline actually wrote into romfs/sfx/ are valid BSND, are the encoding/rate the
//      console expects, and the WHOLE nine-sound set — old three plus new six — still fits
//      the Old 3DS's smaller pool. That last number is the one steve asked to see proven,
//      not assumed.
//
// Reuses audio_bsnd_test.c's CHECK macro shape and audio_mixer_test.c's "read the real
// shipped file" pattern rather than inventing a third convention.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/hw.h"
#include "audio/audio.h"
#include "audio/audio_bsnd.h"
#include "audio/audio_sfx.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                               \
		if (!(cond)) {                                                            \
			s_fails++;                                                            \
			if (!s_first[0])                                                      \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── The slot table, growing from 3 to 9 ────────────────────────────────────────────

static void testRegisterAndQueryEveryNewSlot(void)
{
	audioSfxReset();

	// Distinct fake ids so a mix-up between slots shows up as the wrong id, not just a
	// non-zero one.
	audioSfxRegister(SFX_BLOCK_BREAK, 11);
	audioSfxRegister(SFX_BLOCK_PLACE, 12);
	audioSfxRegister(SFX_FOOTSTEP,    13);
	audioSfxRegister(SFX_HURT,        14);
	audioSfxRegister(SFX_DEATH,       15);
	audioSfxRegister(SFX_EAT,         16);
	audioSfxRegister(SFX_CRAFT,       17);
	audioSfxRegister(SFX_SPLASH,      18);
	audioSfxRegister(SFX_UI_TAP,      19);

	CHECK(audioSfxId(SFX_BLOCK_BREAK) == 11);
	CHECK(audioSfxId(SFX_BLOCK_PLACE) == 12);
	CHECK(audioSfxId(SFX_FOOTSTEP)    == 13);
	CHECK(audioSfxId(SFX_HURT)        == 14);
	CHECK(audioSfxId(SFX_DEATH)       == 15);
	CHECK(audioSfxId(SFX_EAT)         == 16);
	CHECK(audioSfxId(SFX_CRAFT)       == 17);
	CHECK(audioSfxId(SFX_SPLASH)      == 18);
	CHECK(audioSfxId(SFX_UI_TAP)      == 19);

	CHECK(SFX_SLOT_COUNT == 9);

	audioSfxReset();
}

static void testResetClearsAllNineSlots(void)
{
	audioSfxReset();
	for (int i = 0; i < SFX_SLOT_COUNT; i++)
		audioSfxRegister((SfxSlot)i, (AudioSoundId)(100 + i));

	for (int i = 0; i < SFX_SLOT_COUNT; i++)
		CHECK(audioSfxId((SfxSlot)i) == (AudioSoundId)(100 + i));

	audioSfxReset();
	for (int i = 0; i < SFX_SLOT_COUNT; i++)
		CHECK(audioSfxId((SfxSlot)i) == AUDIO_SOUND_NONE);
}

// A slot outside 0..8 must be ignored rather than write past s_slots — the bigger table
// makes this MORE important to re-check, not less, since a future ninth-slot-sized off-
// by-one would corrupt whichever slot sits at index SFX_SLOT_COUNT.
static void testOutOfRangeSlotIsIgnored(void)
{
	audioSfxReset();
	audioSfxRegister((SfxSlot)SFX_SLOT_COUNT, 77);       // one past the end
	audioSfxRegister((SfxSlot)(SFX_SLOT_COUNT + 50), 77); // well past the end
	CHECK(audioSfxId((SfxSlot)SFX_SLOT_COUNT) == AUDIO_SOUND_NONE);

	// And every real slot is still untouched by the attempt.
	for (int i = 0; i < SFX_SLOT_COUNT; i++)
		CHECK(audioSfxId((SfxSlot)i) == AUDIO_SOUND_NONE);
}

// audio.h's contract: every play call is safe with no audio at all. audioInit() is never
// called in this whole test file, which is exactly the "console with no dumped DSP
// firmware" state — so this proves the six new slots inherit the silent-fallback contract
// through audioSfxPlayAtBlock without any new code having been needed for it.
static void testPlayAtBlockSafeWithNoAudioForEveryNewSlot(void)
{
	audioTestReset();
	audioTestSetBackend(NULL);
	audioSfxReset();

	audioSfxRegister(SFX_HURT,   1);
	audioSfxRegister(SFX_DEATH,  2);
	audioSfxRegister(SFX_EAT,    3);
	audioSfxRegister(SFX_CRAFT,  4);
	audioSfxRegister(SFX_SPLASH, 5);
	audioSfxRegister(SFX_UI_TAP, 6);

	CHECK(audioSfxPlayAtBlock(SFX_HURT,   AUDIO_PRIO_HIGH,   1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);
	CHECK(audioSfxPlayAtBlock(SFX_DEATH,  AUDIO_PRIO_HIGH,   1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);
	CHECK(audioSfxPlayAtBlock(SFX_EAT,    AUDIO_PRIO_NORMAL, 1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);
	CHECK(audioSfxPlayAtBlock(SFX_CRAFT,  AUDIO_PRIO_NORMAL, 1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);
	CHECK(audioSfxPlayAtBlock(SFX_SPLASH, AUDIO_PRIO_NORMAL, 1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);
	CHECK(audioSfxPlayAtBlock(SFX_UI_TAP, AUDIO_PRIO_UI,     1.0f, 0, 0, 0) == AUDIO_VOICE_NONE);

	audioSfxReset();
}

// ── The six new .bsnd files, and the Old 3DS pool budget ───────────────────────────

typedef struct {
	const char* path;
	uint32_t    expect_data_bytes;   // measured from tools/make_sounds.py --check
} ShippedSfx;

// All nine, not just the six new ones — a suite that only re-checked the new files could
// go green next to a change that silently broke block_break/place/footstep, which is
// exactly the failure mode audio_mixer_test.c's own testRealSoundFilesLoad guards against
// from the mixer side. This checks the same nine files from the FORMAT side, with no
// backend and no mixer needed at all.
static const ShippedSfx kShipped[] = {
	{ "romfs/sfx/block_break.bsnd", 41344 },
	{ "romfs/sfx/block_place.bsnd", 34356 },
	{ "romfs/sfx/footstep.bsnd",    11968 },
	{ "romfs/sfx/hurt.bsnd",        12348 },
	{ "romfs/sfx/death.bsnd",       24256 },
	{ "romfs/sfx/eat.bsnd",          8820 },
	{ "romfs/sfx/craft.bsnd",        9704 },
	{ "romfs/sfx/splash.bsnd",      13232 },
	{ "romfs/sfx/ui_tap.bsnd",       3968 },
};
#define SHIPPED_COUNT (sizeof(kShipped) / sizeof(kShipped[0]))

// SFX_SLOT_COUNT (audio_sfx.h) and SHIPPED_COUNT (this file's own table) must agree, or one
// of the two lists drifted from the other without either side noticing — the exact "a slot
// with no clip behind it" gap the task asked this suite to close.
static void testShippedFileCountMatchesSlotCount(void)
{
	CHECK((int)SHIPPED_COUNT == SFX_SLOT_COUNT);
}

static void testEveryShippedFileParsesAsValidBsnd(void)
{
	uint32_t total_pool = 0;

	for (size_t i = 0; i < SHIPPED_COUNT; i++) {
		FILE* f = fopen(kShipped[i].path, "rb");
		CHECK(f != NULL);
		if (!f) continue;

		static uint8_t buf[131072];
		const size_t n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		CHECK(n > BSND_HEADER_BYTES);

		BsndHeader h;
		memset(&h, 0, sizeof(h));
		const BsndResult r = bsndParse(buf, n, /*verify_crc=*/true, &h);
		CHECK(r == BSND_OK);
		if (r != BSND_OK) continue;

		// Format the console backend actually expects: PCM16, 22,050 Hz — the same values
		// tools/make_sounds.py's TARGET_RATE and ENC_PCM16 fix for every sound in the
		// manifest, existing and new alike.
		CHECK(h.encoding == BSND_ENC_PCM16);
		CHECK(h.sample_rate == 22050);
		CHECK(h.frame_count > 0);
		CHECK(h.data_offset == BSND_HEADER_BYTES);

		// The exact figure tools/make_sounds.py --check reported for this file. Pins the
		// synthesis + pack pipeline to a byte count rather than trusting it stayed put.
		CHECK(h.data_bytes == kShipped[i].expect_data_bytes);

		total_pool += h.data_bytes;
	}

	// The number this whole task turns on: nine real sounds, summed from their own parsed
	// headers rather than added up by hand, must still fit the SMALLER pool. If a future
	// sound pushes this over, this is where it goes red, on a PC, in under a second —
	// exactly the property audio_mixer_test.c's testShippedSetFitsOldConsole already gives
	// the three-sound set, extended here to cover what this lane added.
	CHECK(total_pool == 159996u);
	CHECK(total_pool < AUDIO_POOL_BYTES_OLD3DS);
	CHECK(total_pool < AUDIO_POOL_BYTES_NEW3DS);

	// Not a tautology: this is the ACTUAL remaining headroom after this lane's additions,
	// printed into the failure message space via CHECK's own line/expression capture if it
	// ever goes red, so a future lane adding a tenth sound can see how much room is left
	// without re-deriving it.
	CHECK(AUDIO_POOL_BYTES_OLD3DS - total_pool == 233220u);
}

int main(void)
{
	testRegisterAndQueryEveryNewSlot();
	testResetClearsAllNineSlots();
	testOutOfRangeSlotIsIgnored();
	testPlayAtBlockSafeWithNoAudioForEveryNewSlot();

	testShippedFileCountMatchesSlotCount();
	testEveryShippedFileParsesAsValidBsnd();

	if (s_fails == 0)
		printf("audio sfx self-test: PASS  %d checks\n", s_checks);
	else
		printf("audio sfx self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int audio_sfx_test_host_only_t;

#endif   // !__3DS__
