// The console half of the audio system: the only file in source/audio that includes
// <3ds.h> or knows what ndsp is. Everything above it — voice allocation, priority,
// stealing, panning, the container parse — is in files the host suite links directly.
//
// Nothing in here is host-testable and nothing in here has run on real 3DS hardware.
// Deliberately, therefore, it contains no decisions: it is the thinnest possible
// translation of AudioBackend's six calls into libctru's, so that a bug on hardware is
// a bug in six one-line functions rather than anywhere in the subsystem.

#ifdef __3DS__

#include <3ds.h>
#include <string.h>

#include "audio/audio_backend.h"
#include "audio/audio_bsnd.h"
#include "audio/audio_mixer.h"   // AUDIO_VOICE_COUNT — how many channels to reserve

// The BSND encoding field is passed straight through to ndspChnSetFormat. That is only
// safe because the two enumerations were chosen to agree; this is where that agreement
// is enforced rather than hoped for. If libctru ever renumbers, this stops the build
// instead of playing 8-bit data as 16-bit — which sounds like loud static, not like a
// bug in an enum.
//
// Both sides are cast to int first. Without the casts these are two ANONYMOUS enums and
// the console build dies on -Werror=enum-compare — which is how this was found, by
// compiling the file for ARM rather than by reading it.
_Static_assert((int)BSND_ENC_PCM8  == (int)NDSP_ENCODING_PCM8,
               "BSND/ndsp PCM8 encoding drift");
_Static_assert((int)BSND_ENC_PCM16 == (int)NDSP_ENCODING_PCM16,
               "BSND/ndsp PCM16 encoding drift");

// One wave buffer per voice, kept alive for the process. ndsp reads these structs
// asynchronously while the sound plays, so they cannot be locals — a stack ndspWaveBuf is
// the classic 3DS audio bug and it presents as intermittent silence or noise, never as a
// crash at the call site.
//
// They live in the BSS (app heap), not in linear: the DSP reads the SAMPLES by physical
// address, but the wave buffer descriptor itself is walked by libctru on the ARM11 side.
static ndspWaveBuf s_wavebuf[AUDIO_VOICE_COUNT];

static bool s_up;

static bool ndspBackendInit(void* ud)
{
	(void)ud;

	// The failure this exists to handle: ndspInit() needs the DSP firmware, which
	// homebrew is not allowed to ship and which the user has to dump from their own
	// console. On a console where that was never done this returns an error and there is
	// nothing the game can do but be quiet. It is a normal outcome, not an assertion.
	if (R_FAILED(ndspInit())) return false;

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);

	// Soft clipping is libctru's default and is kept explicitly rather than inherited:
	// eight voices summing at once is exactly the situation where the difference between
	// soft and hard clipping is audible, and a default is a thing that can change.
	ndspSetClippingMode(NDSP_CLIP_SOFT);

	memset(s_wavebuf, 0, sizeof(s_wavebuf));
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++)
		ndspChnReset(i);

	s_up = true;
	return true;
}

static void ndspBackendShutdown(void* ud)
{
	(void)ud;
	if (!s_up) return;
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++)
		ndspChnReset(i);
	ndspExit();
	s_up = false;
}

static void ndspBackendSetMasterVolume(void* ud, float volume)
{
	(void)ud;
	if (s_up) ndspSetMasterVol(volume);
}

// mix[0] and mix[1] are front left/right; [2] and [3] are back, which on a 3DS's two
// speakers are the same pair; [4..11] are aux sends this game does not use. Zeroing the
// whole array every time rather than only writing [0] and [1] means a channel recycled
// from some future code that DID use an aux send cannot leak that send into this sound.
static void applyMix(int chn, float left, float right)
{
	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = left;
	mix[1] = right;
	ndspChnSetMix(chn, mix);
}

static void ndspBackendChnPlay(void* ud, int chn, const AudioSample* s,
                               float left, float right, bool looping)
{
	(void)ud;
	if (!s_up || !s || chn < 0 || chn >= AUDIO_VOICE_COUNT) return;

	ndspChnReset(chn);
	ndspChnSetInterp(chn, NDSP_INTERP_LINEAR);
	ndspChnSetRate(chn, (float)s->sample_rate);
	ndspChnSetFormat(chn, (u16)(NDSP_CHANNELS(1) | NDSP_ENCODING(s->encoding)));
	applyMix(chn, left, right);

	ndspWaveBuf* wb = &s_wavebuf[chn];
	memset(wb, 0, sizeof(*wb));
	wb->data_vaddr = s->data;
	wb->nsamples   = s->frame_count;
	wb->looping    = looping;

	// The cache flush is not optional and its absence does not fail loudly. The ARM11
	// wrote these bytes through its data cache when the file was read; the DSP reads
	// physical memory and does not see that cache. Without the flush the first play of a
	// freshly loaded sound plays whatever was in that physical page before — usually
	// silence, sometimes a fragment of a previous sound, and it "fixes itself" on the
	// second play, which is the shape of bug that costs a day.
	//
	// It is done here rather than once at load time because it is cheap (the data is
	// already in cache) and because doing it per play is correct even if a future caller
	// ever writes into a sample buffer.
	DSP_FlushDataCache(s->data, s->frame_count * bsndFrameBytes(s->encoding));

	ndspChnWaveBufAdd(chn, wb);
}

static void ndspBackendChnStop(void* ud, int chn)
{
	(void)ud;
	if (!s_up || chn < 0 || chn >= AUDIO_VOICE_COUNT) return;
	// WaveBufClear rather than Reset: clearing drops the queue and stops playback, which
	// is what a stop means, while leaving the channel's format and mix configured. Reset
	// is what a play does, because a play is about to set all of that anyway.
	ndspChnWaveBufClear(chn);
	s_wavebuf[chn].status = NDSP_WBUF_FREE;
}

static bool ndspBackendChnIsPlaying(void* ud, int chn)
{
	(void)ud;
	if (!s_up || chn < 0 || chn >= AUDIO_VOICE_COUNT) return false;
	return ndspChnIsPlaying(chn);
}

static void ndspBackendChnSetMix(void* ud, int chn, float left, float right)
{
	(void)ud;
	if (!s_up || chn < 0 || chn >= AUDIO_VOICE_COUNT) return;
	applyMix(chn, left, right);
}

static const AudioBackend s_ndsp_backend = {
	.init              = ndspBackendInit,
	.shutdown          = ndspBackendShutdown,
	.set_master_volume = ndspBackendSetMasterVolume,
	.chn_play          = ndspBackendChnPlay,
	.chn_stop          = ndspBackendChnStop,
	.chn_is_playing    = ndspBackendChnIsPlaying,
	.chn_set_mix       = ndspBackendChnSetMix,
	.ud                = NULL,
};

const AudioBackend* audioNdspBackend(void)
{
	return &s_ndsp_backend;
}

#else

// Host build: nothing here. An empty translation unit is not valid ISO C.
typedef int audio_ndsp_console_only_t;

#endif   // __3DS__
