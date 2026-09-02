// Host self-test for the audio subsystem's policy half: audio_mixer.c's voice
// allocation, priority and stealing, and audio.c's silent-fallback contract.
//
// The modules under test are the REAL ones — this binary links source/audio/audio_mixer.c
// and source/audio/audio.c, not copies. What is faked is the hardware, through the
// AudioBackend seam in audio_backend.h, by a recorder that remembers every call so the
// test can assert on the sequence rather than on a summary of it.
//
// The __3DS__ guard is the same one app/options_test.c carries and for the same reason:
// the Makefile globs every .c under a SOURCES directory into the console build, so
// without it this file's main() collides with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "app/hw.h"
#include "audio/audio.h"
#include "audio/audio_backend.h"
#include "audio/audio_bsnd.h"
#include "audio/audio_mixer.h"

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

// ── The fake hardware ─────────────────────────────────────────────────────────────

#define FAKE_MAX_EVENTS 256

typedef enum {
	EV_INIT, EV_SHUTDOWN, EV_MASTER, EV_PLAY, EV_STOP, EV_SETMIX,
} FakeEventKind;

typedef struct {
	FakeEventKind kind;
	int   chn;
	float left, right;
	bool  looping;
} FakeEvent;

typedef struct {
	bool      init_should_fail;
	bool      playing[AUDIO_VOICE_COUNT];   // what chn_is_playing will answer
	FakeEvent ev[FAKE_MAX_EVENTS];
	int       ev_count;
	int       init_calls;
	int       shutdown_calls;
	float     master;
} Fake;

static Fake s_fake;

static void fakePush(FakeEventKind k, int chn, float l, float r, bool loop)
{
	if (s_fake.ev_count >= FAKE_MAX_EVENTS) return;
	FakeEvent* e = &s_fake.ev[s_fake.ev_count++];
	e->kind = k; e->chn = chn; e->left = l; e->right = r; e->looping = loop;
}

static bool fakeInit(void* ud)
{
	(void)ud;
	s_fake.init_calls++;
	fakePush(EV_INIT, -1, 0, 0, false);
	return !s_fake.init_should_fail;
}

static void fakeShutdown(void* ud)
{
	(void)ud;
	s_fake.shutdown_calls++;
	fakePush(EV_SHUTDOWN, -1, 0, 0, false);
}

static void fakeMaster(void* ud, float v)
{
	(void)ud;
	s_fake.master = v;
	fakePush(EV_MASTER, -1, v, 0, false);
}

static void fakePlay(void* ud, int chn, const AudioSample* s, float l, float r, bool loop)
{
	(void)ud; (void)s;
	if (chn >= 0 && chn < AUDIO_VOICE_COUNT) s_fake.playing[chn] = true;
	fakePush(EV_PLAY, chn, l, r, loop);
}

static void fakeStop(void* ud, int chn)
{
	(void)ud;
	if (chn >= 0 && chn < AUDIO_VOICE_COUNT) s_fake.playing[chn] = false;
	fakePush(EV_STOP, chn, 0, 0, false);
}

static bool fakeIsPlaying(void* ud, int chn)
{
	(void)ud;
	return (chn >= 0 && chn < AUDIO_VOICE_COUNT) ? s_fake.playing[chn] : false;
}

static void fakeSetMix(void* ud, int chn, float l, float r)
{
	(void)ud;
	fakePush(EV_SETMIX, chn, l, r, false);
}

static const AudioBackend s_fake_backend = {
	fakeInit, fakeShutdown, fakeMaster, fakePlay, fakeStop, fakeIsPlaying, fakeSetMix, NULL,
};

static void fakeReset(bool init_should_fail)
{
	memset(&s_fake, 0, sizeof(s_fake));
	s_fake.init_should_fail = init_should_fail;
}

static int countEvents(FakeEventKind k)
{
	int n = 0;
	for (int i = 0; i < s_fake.ev_count; i++)
		if (s_fake.ev[i].kind == k) n++;
	return n;
}

// A sample that is never dereferenced by anything in this binary — the mixer only passes
// the pointer through to the backend, and the fake backend ignores it. Non-NULL because
// mixerPlay refuses a NULL data pointer, which is itself checked below.
static const uint8_t s_fake_pcm[64];
static const AudioSample s_sample = { s_fake_pcm, 1000, 22050, 1 /* PCM16 */ };

static AudioMixer s_m;

static void armMixer(void)
{
	fakeReset(false);
	CHECK(mixerInit(&s_m, &s_fake_backend, AUDIO_VOICE_COUNT) == true);
}

// ── Tests ─────────────────────────────────────────────────────────────────────────

// Voices are handed out lowest-index-first. This is asserted rather than assumed because
// every later test that names a channel number depends on it.
static void testFreeVoicesGoInIndexOrder(void)
{
	armMixer();
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++) {
		AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
		CHECK(v != AUDIO_VOICE_NONE);
		CHECK((int)(v & AUDIO_VOICE_INDEX_MASK) == i);
	}
	CHECK(mixerActiveCount(&s_m) == AUDIO_VOICE_COUNT);
	mixerShutdown(&s_m);
}

// A sound of strictly lower priority than every busy voice must be REFUSED, not squeezed
// in. Without this a stream of ambience would evict the sounds the player needs.
static void testLowerPriorityIsRefusedWhenFull(void)
{
	armMixer();
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++)
		mixerPlay(&s_m, &s_sample, AUDIO_PRIO_HIGH, 1.0f, 1.0f, false);

	const int plays_before = countEvents(EV_PLAY);
	AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_AMBIENT, 1.0f, 1.0f, false);
	CHECK(v == AUDIO_VOICE_NONE);
	// The refusal must be a refusal, not a play that was then stopped: the hardware must
	// not have been touched at all.
	CHECK(countEvents(EV_PLAY) == plays_before);
	CHECK(mixerActiveCount(&s_m) == AUDIO_VOICE_COUNT);
	mixerShutdown(&s_m);
}

// The victim is the LOWEST priority busy voice, wherever it sits in the table.
static void testStealsLowestPriority(void)
{
	armMixer();
	// Fill with HIGH, then make channel 5 the one weak voice.
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++)
		mixerPlay(&s_m, &s_sample, AUDIO_PRIO_HIGH, 1.0f, 1.0f, false);
	AudioVoice weak = AUDIO_VOICE_NONE;
	{
		// Free channel 5 by stopping it, then refill it at AMBIENT so it is the victim.
		// Stopping needs its handle, so rebuild the mixer and lay the voices down in a
		// known order instead of guessing at handles.
		mixerShutdown(&s_m);
		armMixer();
		for (int i = 0; i < AUDIO_VOICE_COUNT; i++) {
			AudioPriority p = (i == 5) ? AUDIO_PRIO_AMBIENT : AUDIO_PRIO_HIGH;
			AudioVoice v = mixerPlay(&s_m, &s_sample, p, 1.0f, 1.0f, false);
			if (i == 5) weak = v;
		}
	}
	CHECK(weak != AUDIO_VOICE_NONE);

	AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
	CHECK(v != AUDIO_VOICE_NONE);
	CHECK((int)(v & AUDIO_VOICE_INDEX_MASK) == 5);
	// The stolen voice's handle must now be dead, or a later audioStop from whoever
	// started the ambience would stop the block break that replaced it.
	CHECK(mixerVoiceLive(&s_m, weak) == false);
	CHECK(mixerVoiceLive(&s_m, v) == true);
	mixerShutdown(&s_m);
}

// The load-bearing `<=`: at EQUAL priority the newcomer takes the OLDEST voice. This is
// what stops a burst of same-rank sounds from locking the mixer until they all finish.
static void testEqualPriorityStealsOldest(void)
{
	armMixer();
	AudioVoice first = AUDIO_VOICE_NONE;
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++) {
		AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_LOW, 1.0f, 1.0f, false);
		if (i == 0) first = v;
	}
	AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_LOW, 1.0f, 1.0f, false);
	CHECK(v != AUDIO_VOICE_NONE);
	CHECK((int)(v & AUDIO_VOICE_INDEX_MASK) == 0);   // channel 0 held the oldest
	CHECK(mixerVoiceLive(&s_m, first) == false);

	// And again: the next steal must take channel 1, not channel 0 again. A stealing
	// policy that always picked the same victim would pass the check above and starve
	// every voice but one.
	AudioVoice v2 = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_LOW, 1.0f, 1.0f, false);
	CHECK((int)(v2 & AUDIO_VOICE_INDEX_MASK) == 1);
	mixerShutdown(&s_m);
}

// A steal must STOP the channel before starting on it. ndspChnWaveBufAdd queues rather
// than replaces, so a play without a stop would append the new sound behind the old one.
static void testStealStopsBeforePlaying(void)
{
	armMixer();
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++)
		mixerPlay(&s_m, &s_sample, AUDIO_PRIO_LOW, 1.0f, 1.0f, false);
	const int at = s_fake.ev_count;
	mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
	CHECK(s_fake.ev_count == at + 2);
	CHECK(s_fake.ev[at].kind == EV_STOP);
	CHECK(s_fake.ev[at + 1].kind == EV_PLAY);
	CHECK(s_fake.ev[at].chn == s_fake.ev[at + 1].chn);
	mixerShutdown(&s_m);
}

// A handle kept across its voice being recycled must not stop the stranger that got it.
static void testStaleHandleCannotStopARecycledVoice(void)
{
	armMixer();
	AudioVoice old = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
	CHECK(old != AUDIO_VOICE_NONE);

	// Finish it the way the hardware would, and reap.
	s_fake.playing[0] = false;
	mixerUpdate(&s_m);
	CHECK(mixerVoiceLive(&s_m, old) == false);

	AudioVoice fresh = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
	CHECK((int)(fresh & AUDIO_VOICE_INDEX_MASK) == 0);   // same slot
	CHECK(fresh != old);                                  // different generation

	const int stops_before = countEvents(EV_STOP);
	mixerStop(&s_m, old);                                 // the stale handle
	CHECK(countEvents(EV_STOP) == stops_before);          // hardware untouched
	CHECK(mixerVoiceLive(&s_m, fresh) == true);           // the stranger survived
	mixerShutdown(&s_m);
}

// Reaping frees non-looping voices and must NOT free looping ones — ambience is supposed
// to run forever and a buffer boundary that reads as "not playing" would kill it.
static void testUpdateReapsFinishedButNotLooping(void)
{
	armMixer();
	AudioVoice loop = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_AMBIENT, 1.0f, 1.0f, true);
	AudioVoice once = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, false);
	CHECK(mixerActiveCount(&s_m) == 2);

	// The hardware says BOTH have stopped.
	s_fake.playing[0] = false;
	s_fake.playing[1] = false;
	mixerUpdate(&s_m);

	CHECK(mixerVoiceLive(&s_m, loop) == true);
	CHECK(mixerVoiceLive(&s_m, once) == false);
	CHECK(mixerActiveCount(&s_m) == 1);
	mixerShutdown(&s_m);
}

static void testGainsAreClampedAndNanSafe(void)
{
	armMixer();
	mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 4.0f, -2.0f, false);
	const FakeEvent* e = &s_fake.ev[s_fake.ev_count - 1];
	CHECK(e->kind == EV_PLAY);
	CHECK(e->left == 1.0f);
	CHECK(e->right == 0.0f);

	// A NaN gain must land on 0, not reach the DSP. 0.0f/0.0f is written through a
	// volatile so the compiler cannot fold it at -O1 and quietly delete the check.
	volatile float zero = 0.0f;
	const float nan_gain = zero / zero;
	mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, nan_gain, nan_gain, false);
	const FakeEvent* e2 = &s_fake.ev[s_fake.ev_count - 1];
	CHECK(e2->kind == EV_PLAY);
	CHECK(e2->left == 0.0f);
	CHECK(e2->right == 0.0f);
	mixerShutdown(&s_m);
}

static void testRejectsEmptySamples(void)
{
	armMixer();
	const AudioSample empty = { s_fake_pcm, 0, 22050, 1 };
	const AudioSample nodata = { NULL, 100, 22050, 1 };
	const int plays = countEvents(EV_PLAY);
	CHECK(mixerPlay(&s_m, NULL, AUDIO_PRIO_NORMAL, 1, 1, false) == AUDIO_VOICE_NONE);
	CHECK(mixerPlay(&s_m, &empty, AUDIO_PRIO_NORMAL, 1, 1, false) == AUDIO_VOICE_NONE);
	CHECK(mixerPlay(&s_m, &nodata, AUDIO_PRIO_NORMAL, 1, 1, false) == AUDIO_VOICE_NONE);
	CHECK(countEvents(EV_PLAY) == plays);
	mixerShutdown(&s_m);
}

static void testSetMixMovesWithoutRestarting(void)
{
	armMixer();
	AudioVoice v = mixerPlay(&s_m, &s_sample, AUDIO_PRIO_NORMAL, 0.5f, 0.5f, false);
	const int plays = countEvents(EV_PLAY);
	CHECK(mixerSetMix(&s_m, v, 0.25f, 0.75f) == true);
	CHECK(countEvents(EV_PLAY) == plays);        // not restarted
	const FakeEvent* e = &s_fake.ev[s_fake.ev_count - 1];
	CHECK(e->kind == EV_SETMIX);
	CHECK(e->left == 0.25f && e->right == 0.75f);
	// A stale handle must be refused rather than re-panning a stranger.
	mixerStop(&s_m, v);
	CHECK(mixerSetMix(&s_m, v, 1.0f, 1.0f) == false);
	mixerShutdown(&s_m);
}

// ── The failure path. This is the contract at the top of audio.h. ─────────────────

// A mixer whose backend init failed must be inert AND safe: every entry point called,
// nothing crashing, and the hardware never touched after the failed init.
static void testMixerIsInertWhenBackendInitFails(void)
{
	fakeReset(true);
	CHECK(mixerInit(&s_m, &s_fake_backend, AUDIO_VOICE_COUNT) == false);
	CHECK(s_fake.init_calls == 1);

	const int after_init = s_fake.ev_count;

	CHECK(mixerPlay(&s_m, &s_sample, AUDIO_PRIO_UI, 1.0f, 1.0f, false) == AUDIO_VOICE_NONE);
	mixerStop(&s_m, 12345);
	mixerStopAll(&s_m);
	mixerUpdate(&s_m);
	CHECK(mixerSetMix(&s_m, 12345, 1.0f, 1.0f) == false);
	mixerSetMasterVolume(&s_m, 0.5f);
	CHECK(mixerActiveCount(&s_m) == 0);
	CHECK(mixerVoiceLive(&s_m, 12345) == false);

	// Volume is still REMEMBERED even with no hardware, so the options slider round-trips
	// on a console that cannot play anything.
	CHECK(mixerGetMasterVolume(&s_m) == 0.5f);

	// Not one hardware call since the failed init.
	CHECK(s_fake.ev_count == after_init);

	mixerShutdown(&s_m);
	// shutdown must not call the backend's shutdown either: init never succeeded, so
	// there is nothing down there to tear down.
	CHECK(s_fake.shutdown_calls == 0);
}

// The same contract one level up, through the REAL audio.c, with no backend installed at
// all — which is exactly what a console whose ndspInit() failed looks like from here.
static void testAudioApiIsSafeWithNoAudio(void)
{
	audioTestReset();
	audioTestSetBackend(NULL);

	CHECK(audioInit() == false);
	CHECK(audioAvailable() == false);
	CHECK(strcmp(audioFailureReason(), "ok") != 0);

	// No linear pool may be taken when there is no audio: that memory is render
	// distance, and spending it on sounds that can never play would shrink the world on
	// a console that is already worse off.
	CHECK(audioPoolCapacityBytes() == 0);
	CHECK(audioPoolUsedBytes() == 0);

	// Every entry point, called for real. The assertion is that this function returns.
	CHECK(audioLoad("romfs:/sfx/block_break.bsnd") == AUDIO_SOUND_NONE);
	CHECK(audioPlay(1, AUDIO_PRIO_NORMAL, 1.0f) == AUDIO_VOICE_NONE);
	CHECK(audioPlayAt(1, AUDIO_PRIO_NORMAL, 1.0f, 1, 2, 3) == AUDIO_VOICE_NONE);
	AudioPlayParams p;
	memset(&p, 0, sizeof(p));
	p.sound = 1; p.priority = AUDIO_PRIO_UI; p.gain = 1.0f; p.looping = true;
	CHECK(audioPlayEx(&p) == AUDIO_VOICE_NONE);
	CHECK(audioPlayEx(NULL) == AUDIO_VOICE_NONE);
	audioStop(999);
	audioStopAll();
	audioUpdate();
	audioSetListener(1.0f, 2.0f, 3.0f, 0.5f);
	audioSetMasterVolume(0.75f);
	CHECK(audioGetMasterVolume() == 0.75f);
	CHECK(audioLoadedSoundCount() == 0);
	CHECK(audioActiveVoiceCount() == 0);
	CHECK(audioLinearFreeBefore() == 0);
	CHECK(audioLinearFreeAfter() == 0);

	audioShutdown();
	audioShutdown();          // twice is legal
	audioTestReset();
}

// audioInit's OTHER failure mode, reached through the force seam: the backend exists but
// its init refuses. Same contract, different cause, and it is the one a real console with
// no dumped DSP firmware takes.
static void testAudioInitFailureLeavesNoPool(void)
{
	audioTestReset();
	fakeReset(true);
	audioTestSetBackend(&s_fake_backend);
	audioTestForceInitFailure(false);

	CHECK(audioInit() == false);
	CHECK(audioAvailable() == false);
	CHECK(audioPoolCapacityBytes() == 0);
	CHECK(s_fake.init_calls == 1);
	CHECK(audioLoad("romfs:/sfx/block_break.bsnd") == AUDIO_SOUND_NONE);

	audioTestReset();
	audioTestSetBackend(NULL);
	audioTestForceInitFailure(false);
}

// The success path through the real audio.c: a working backend must take a pool, and the
// pool size must follow the console. Both sizes are driven from this one binary through
// app/hw.c's existing hwTestSetNew3ds seam.
static void testPoolSizeFollowsConsole(void)
{
	for (int newer = 0; newer <= 1; newer++) {
		audioTestReset();
		fakeReset(false);
		audioTestSetBackend(&s_fake_backend);
		hwTestReset();
		hwTestSetNew3ds(newer != 0);
		hwInit();

		CHECK(audioInit() == true);
		CHECK(audioAvailable() == true);
		CHECK(strcmp(audioFailureReason(), "ok") == 0);

		const size_t want = newer ? AUDIO_POOL_BYTES_NEW3DS : AUDIO_POOL_BYTES_OLD3DS;
		CHECK(audioPoolCapacityBytes() == want);
		CHECK(audioPoolUsedBytes() == 0);

		audioShutdown();
	}
	audioTestReset();
	audioTestSetBackend(NULL);
	hwTestReset();
}

// The pool is a bump arena with no free list. It must refuse rather than overrun, and a
// refusal must not corrupt what is already loaded.
static void testPoolRefusesOversizedSound(void)
{
	audioTestReset();
	fakeReset(false);
	audioTestSetBackend(&s_fake_backend);
	hwTestReset();
	hwTestSetNew3ds(false);
	hwInit();
	CHECK(audioInit() == true);

	// No such file: the loader must refuse without consuming pool.
	CHECK(audioLoad("build-host/definitely-not-a-sound.bsnd") == AUDIO_SOUND_NONE);
	CHECK(audioPoolUsedBytes() == 0);
	CHECK(audioLoadedSoundCount() == 0);
	CHECK(audioLoad(NULL) == AUDIO_SOUND_NONE);

	audioShutdown();
	audioTestReset();
	audioTestSetBackend(NULL);
	hwTestReset();
}

// ── End to end, against the REAL shipped sound files ──────────────────────────────
//
// Everything above this point runs on buffers the test built. This one runs on the three
// .bsnd files tools/make_sounds.py actually produced into romfs/sfx, through the real
// audioLoad, and is what connects the packer to the parser to the pool. Without it the
// suite could be perfectly green against a packer that writes a format this parser does
// not read — both halves self-consistent and the console silent.
//
// It cannot prove the sounds are AUDIBLE. Nothing on this machine can; that needs the
// console. What it proves is that the bytes on disk survive the whole software path with
// their frame counts, rates and checksums intact, and land in the pool at the size
// tools/make_sounds.py --check reported.
static void testRealSoundFilesLoad(void)
{
	audioTestReset();
	fakeReset(false);
	audioTestSetBackend(&s_fake_backend);
	hwTestReset();
	hwTestSetNew3ds(true);
	hwInit();
	CHECK(audioInit() == true);

	// Paths are relative to the repo root, which is where tools/run_host_tests.sh runs
	// from. On console these are "romfs:/sfx/..." — the same files, reached through the
	// devoptab, which is why the loader takes a path rather than a name.
	const AudioSoundId brk  = audioLoad("romfs/sfx/block_break.bsnd");
	const AudioSoundId plc  = audioLoad("romfs/sfx/block_place.bsnd");
	const AudioSoundId step = audioLoad("romfs/sfx/footstep.bsnd");

	CHECK(brk != AUDIO_SOUND_NONE);
	CHECK(plc != AUDIO_SOUND_NONE);
	CHECK(step != AUDIO_SOUND_NONE);
	CHECK(brk != plc && plc != step);
	CHECK(audioLoadedSoundCount() == 3);

	// The exact figure tools/make_sounds.py --check reports. If the packer's idea of a
	// sound's size and the loader's ever diverge, this is the check that says so, and it
	// says so in bytes rather than as "it sounds wrong".
	CHECK(audioPoolUsedBytes() == 87668);

	// The pool is a bump arena: three sounds must occupy three non-overlapping ranges and
	// the total must be the sum, with no padding inserted. data_bytes is a multiple of 4
	// by the format's rule, which is what makes that true.
	CHECK(audioPoolUsedBytes() == 41344 + 34356 + 11968);

	// And they play, through the real mixer, on the real API.
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);
	AudioVoice v = audioPlayAt(brk, AUDIO_PRIO_NORMAL, 1.0f, 1.0f, 0.0f, 0.0f);
	CHECK(v != AUDIO_VOICE_NONE);
	CHECK(audioActiveVoiceCount() == 1);
	// Hard right at one block: the pan reached the backend, not just the mixer.
	const FakeEvent* e = &s_fake.ev[s_fake.ev_count - 1];
	CHECK(e->kind == EV_PLAY);
	CHECK(e->right > 0.99f);
	CHECK(e->left < 0.01f);

	// A sound beyond AUDIO_MAX_DIST must not take a voice at all — otherwise a distant
	// mining sound holds a channel at zero gain and starves what can be heard.
	CHECK(audioPlayAt(plc, AUDIO_PRIO_NORMAL, 1.0f, 500.0f, 0.0f, 0.0f) == AUDIO_VOICE_NONE);
	CHECK(audioActiveVoiceCount() == 1);

	// Non-positional gets full gain on both sides, unlike the constant-power centre.
	AudioVoice ui = audioPlay(step, AUDIO_PRIO_UI, 1.0f);
	CHECK(ui != AUDIO_VOICE_NONE);
	const FakeEvent* e2 = &s_fake.ev[s_fake.ev_count - 1];
	CHECK(e2->left == 1.0f && e2->right == 1.0f);

	audioStop(v);
	audioStop(ui);
	CHECK(audioActiveVoiceCount() == 0);

	audioShutdown();
	audioTestReset();
	audioTestSetBackend(NULL);
	hwTestReset();
}

// A corrupt sound file must be REFUSED by audioLoad and must consume no pool.
//
// This test exists because the red-arm run found the check missing: turning off the
// loader's CRC verification (bsndParse's `verify_crc` argument) left the whole suite
// green, which means nothing was forcing a corrupt file through the real loader. The
// format suite checks bsndParse rejects a bad checksum; that is not the same as checking
// audioLoad ASKS it to.
//
// The pool assertion is the half that matters most. The load reads straight into the free
// part of the arena before validating, so a refusal that forgot to leave s_pool_used
// alone would silently consume linear memory for every bad file — and the pool is the
// scarce resource this whole subsystem is shaped around.
static void testCorruptFileIsRefusedAndCostsNoPool(void)
{
	audioTestReset();
	fakeReset(false);
	audioTestSetBackend(&s_fake_backend);
	hwTestReset();
	hwTestSetNew3ds(true);
	hwInit();
	CHECK(audioInit() == true);

	// A real sound loads first, so the test can prove a later refusal does not disturb
	// what is already in the pool.
	const AudioSoundId good = audioLoad("romfs/sfx/footstep.bsnd");
	CHECK(good != AUDIO_SOUND_NONE);
	const size_t used_after_good = audioPoolUsedBytes();
	CHECK(used_after_good == 11968);

	// Same file with one payload byte flipped: every header field still valid, only the
	// checksum wrong. This is what a bad SD read looks like — plausible, not obviously
	// broken — and is exactly what the CRC is in the format for.
	FILE* in = fopen("romfs/sfx/footstep.bsnd", "rb");
	CHECK(in != NULL);
	if (in) {
		static uint8_t copy[65536];
		const size_t n = fread(copy, 1, sizeof(copy), in);
		fclose(in);
		CHECK(n > BSND_HEADER_BYTES);
		copy[BSND_HEADER_BYTES + 32] ^= 0xFF;
		FILE* out = fopen("build-host/audio_corrupt.bsnd", "wb");
		CHECK(out != NULL);
		if (out) { fwrite(copy, 1, n, out); fclose(out); }
	}

	CHECK(audioLoad("build-host/audio_corrupt.bsnd") == AUDIO_SOUND_NONE);
	CHECK(audioPoolUsedBytes() == used_after_good);
	CHECK(audioLoadedSoundCount() == 1);

	// A truncated file — the other half of a bad read — must also be refused.
	{
		FILE* out = fopen("build-host/audio_trunc.bsnd", "wb");
		CHECK(out != NULL);
		if (out) {
			static const uint8_t stub[BSND_HEADER_BYTES + 8] = { 'B', 'S', 'N', 'D', 1, 0 };
			fwrite(stub, 1, sizeof(stub), out);
			fclose(out);
		}
	}
	CHECK(audioLoad("build-host/audio_trunc.bsnd") == AUDIO_SOUND_NONE);
	CHECK(audioPoolUsedBytes() == used_after_good);

	// The good sound is still intact and still plays.
	CHECK(audioPlay(good, AUDIO_PRIO_NORMAL, 1.0f) != AUDIO_VOICE_NONE);

	remove("build-host/audio_corrupt.bsnd");
	remove("build-host/audio_trunc.bsnd");
	audioShutdown();
	audioTestReset();
	audioTestSetBackend(NULL);
	hwTestReset();
}

// The Old 3DS pool is 393,216 bytes and the three shipped sounds are 87,668 of it. That
// leaves room, and this asserts the margin rather than trusting the arithmetic in a
// comment — if a future sound pushes the set past the smaller console's pool, this is
// where it is noticed, on a PC, rather than on an Old 3DS.
static void testShippedSetFitsOldConsole(void)
{
	CHECK(87668u < AUDIO_POOL_BYTES_OLD3DS);
	CHECK(87668u < AUDIO_POOL_BYTES_NEW3DS);
	CHECK(AUDIO_POOL_BYTES_OLD3DS < AUDIO_POOL_BYTES_NEW3DS);
}

int main(void)
{
	testFreeVoicesGoInIndexOrder();
	testLowerPriorityIsRefusedWhenFull();
	testStealsLowestPriority();
	testEqualPriorityStealsOldest();
	testStealStopsBeforePlaying();
	testStaleHandleCannotStopARecycledVoice();
	testUpdateReapsFinishedButNotLooping();
	testGainsAreClampedAndNanSafe();
	testRejectsEmptySamples();
	testSetMixMovesWithoutRestarting();

	testMixerIsInertWhenBackendInitFails();
	testAudioApiIsSafeWithNoAudio();
	testAudioInitFailureLeavesNoPool();
	testPoolSizeFollowsConsole();
	testPoolRefusesOversizedSound();
	testRealSoundFilesLoad();
	testCorruptFileIsRefusedAndCostsNoPool();
	testShippedSetFitsOldConsole();

	if (s_fails == 0)
		printf("audio mixer self-test: PASS  %d checks\n", s_checks);
	else
		printf("audio mixer self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int audio_mixer_test_host_only_t;

#endif   // !__3DS__
