#include "audio/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/hw.h"
#include "audio/audio_bsnd.h"

// This file owns the linear pool and the romfs reads. It is deliberately still
// host-compilable — the only #ifdef __3DS__ in it is the allocator and the free-space
// query — because the silent-fallback contract in audio.h is a property of THIS code and
// a test that exercised a host-only copy of it would prove nothing about the console.
//
// fopen reaching "romfs:/..." on console and a plain path on the host is the same
// devoptab trick app/options.c and world/region.c already rely on; see options.c's
// header comment.

#ifdef __3DS__
#include <3ds.h>
#include "audio/audio_backend.h"
extern const AudioBackend* audioNdspBackend(void);
#else
#include "audio/audio_backend.h"
// The host has no DSP. audio.c is compiled into the host suite only so that the
// no-audio path and the pool arithmetic can be driven; the backend it gets is supplied
// by the test through audioTestForceInitFailure(), and with nothing supplied it simply
// has no audio, which is the correct answer for a PC.
static const AudioBackend* audioNdspBackend(void);
#endif

// ── State ─────────────────────────────────────────────────────────────────────────

static bool         s_inited;
static bool         s_available;
static const char*  s_reason = "not initialised";

static uint8_t*     s_pool;
static size_t       s_pool_cap;
static size_t       s_pool_used;

static AudioSample  s_sounds[AUDIO_SOUND_MAX];
static int          s_sound_count;      // ids are 1..s_sound_count

static AudioMixer   s_mixer;
static AudioListener s_listener;

static uint32_t     s_linear_before;
static uint32_t     s_linear_after_dsp;
static uint32_t     s_linear_after;

#ifndef __3DS__
static bool s_force_init_failure;
#endif

// ── Platform shims. The only two things this file needs a console for. ────────────

static void* poolAlloc(size_t bytes)
{
#ifdef __3DS__
	// linearAlloc, not malloc: the DSP reads sample data by physical address and cannot
	// see the app heap at all. Getting this wrong does not fail to compile and does not
	// fail at load — it plays noise, or nothing, from whatever physical page happened to
	// be under the virtual address.
	return linearAlloc(bytes);
#else
	return malloc(bytes);
#endif
}

static void poolFree(void* p)
{
#ifdef __3DS__
	linearFree(p);
#else
	free(p);
#endif
}

static uint32_t linearFreeNow(void)
{
#ifdef __3DS__
	return (uint32_t)linearSpaceFree();
#else
	return 0;
#endif
}

// ── Init / shutdown ───────────────────────────────────────────────────────────────

bool audioInit(void)
{
	if (s_inited) return s_available;
	s_inited = true;

	s_available = false;
	s_pool = NULL;
	s_pool_cap = 0;
	s_pool_used = 0;
	s_sound_count = 0;
	memset(s_sounds, 0, sizeof(s_sounds));
	audioListenerFromYaw(&s_listener, 0.0f, 0.0f, 0.0f, 0.0f);

	const AudioBackend* backend = audioNdspBackend();

#ifndef __3DS__
	if (s_force_init_failure) backend = NULL;
#endif

	// Taken BEFORE ndspInit, not before the pool. ndspInit allocates its own buffers
	// inside libctru and this project has no measurement of how much; the only way to get
	// that number is to read linear free either side of it on a real console. The three
	// probes below split the total into "what ndsp took" and "what the pool took" so the
	// first hardware boot answers both at once.
	s_linear_before = linearFreeNow();

	// The DSP comes up BEFORE the pool is taken. Order matters: if there is no audio,
	// the 1 MiB of linear must not be taken at all — that memory is render distance, and
	// spending it on sounds that can never play would make a console with no DSP firmware
	// ALSO draw a smaller world. That is the sort of second-order cost that is invisible
	// until someone measures it and cannot explain the difference.
	if (!mixerInit(&s_mixer, backend, AUDIO_VOICE_COUNT)) {
		s_reason = "DSP unavailable (no dspfirm dumped?) — running silently";
		return false;
	}

	const size_t want = hwIsNew3ds() ? AUDIO_POOL_BYTES_NEW3DS : AUDIO_POOL_BYTES_OLD3DS;

	s_linear_after_dsp = linearFreeNow();
	s_pool = (uint8_t*)poolAlloc(want);
	s_linear_after = linearFreeNow();

	if (!s_pool) {
		// Refusing to run half-initialised: the DSP goes back down rather than being left
		// up with no sounds to play, so audioAvailable() answering false means exactly one
		// thing everywhere.
		mixerShutdown(&s_mixer);
		s_reason = "linear pool allocation failed";
		return false;
	}

	s_pool_cap = want;
	s_available = true;
	s_reason = "ok";
	return true;
}

void audioShutdown(void)
{
	if (!s_inited) return;
	mixerShutdown(&s_mixer);
	if (s_pool) poolFree(s_pool);
	s_pool = NULL;
	s_pool_cap = 0;
	s_pool_used = 0;
	s_sound_count = 0;
	s_available = false;
	s_inited = false;
	s_reason = "shut down";
}

bool audioAvailable(void) { return s_available; }
const char* audioFailureReason(void) { return s_reason ? s_reason : "unknown"; }

// ── Loading ───────────────────────────────────────────────────────────────────────

// ── romfs, mounted only for as long as a load takes ───────────────────────────────
//
// This game does NOT keep romfs mounted. The only romfsInit() in the tree is inside
// app/updater.c's updaterInit(), which mounts it for the certificate bundle and gives up
// entirely — `return false` — if the mount fails.
//
// libctru's romfsInit() is romfsMountSelf("romfs"), which goes through devoptab AddDevice,
// and adding a device name that is already registered FAILS. So audio holding the mount open
// would make updaterInit() return false and silently switch the self-updater off. That is a
// regression in a completely unrelated feature, caused by a subsystem that only needed to
// read three files at boot, and it would present as "updates stopped working" with nothing
// pointing back at audio.
//
// So the mount is taken per load and handed back in exactly the state it was found in. If
// romfsInit() fails, either romfs is already mounted (someone else owns it — do not unmount
// it) or there is no romfs at all (the load will fail on its own, correctly). Either way
// `mounted_here` is false and nothing is unmounted. Three mount/unmount pairs at boot is a
// cheap price for not being able to break another module by existing.
#ifdef __3DS__
static bool romfsHoldBegin(const char* path)
{
	if (strncmp(path, "romfs:", 6) != 0) return false;
	return R_SUCCEEDED(romfsInit());
}

static void romfsHoldEnd(bool mounted_here)
{
	if (mounted_here) romfsExit();
}
#else
static bool romfsHoldBegin(const char* path) { (void)path; return false; }
static void romfsHoldEnd(bool mounted_here)  { (void)mounted_here; }
#endif

// Reads the whole file into `out` (at most cap bytes) and returns the byte count, or 0.
// Deliberately does NOT seek to find the length first: a devoptab that reports a length
// it then cannot deliver is exactly the corrupt-SD case the CRC exists for, so the read
// itself is the authority.
static size_t readWhole(const char* path, void* out, size_t cap)
{
	const bool held = romfsHoldBegin(path);
	FILE* f = fopen(path, "rb");
	if (!f) { romfsHoldEnd(held); return 0; }
	const size_t n = fread(out, 1, cap, f);
	// A file that exactly fills the buffer is indistinguishable from one that overflowed
	// it, so check for more bytes rather than assume. A sound too big for the pool is a
	// refusal, not a truncation.
	const bool overflowed = (n == cap) && (fgetc(f) != EOF);
	fclose(f);
	romfsHoldEnd(held);
	return overflowed ? 0 : n;
}

AudioSoundId audioLoad(const char* path)
{
	if (!s_available || !path) return AUDIO_SOUND_NONE;
	if (s_sound_count >= AUDIO_SOUND_MAX) return AUDIO_SOUND_NONE;

	// The file is read STRAIGHT into the free part of the pool, header and all, and then
	// the payload is slid down over the header once it has been validated. That avoids a
	// second staging buffer the size of the largest sound — on a machine where the whole
	// point is that linear is scarce, a scratch buffer would double the peak cost of a
	// load for the duration of the load.
	uint8_t* dst = s_pool + s_pool_used;
	const size_t avail = s_pool_cap - s_pool_used;
	if (avail <= BSND_HEADER_BYTES) return AUDIO_SOUND_NONE;

	const size_t n = readWhole(path, dst, avail);
	if (n == 0) return AUDIO_SOUND_NONE;

	BsndHeader h;
	const BsndResult r = bsndParse(dst, n, true, &h);
	if (r != BSND_OK) return AUDIO_SOUND_NONE;

	memmove(dst, dst + h.data_offset, h.data_bytes);

	// data_bytes is a multiple of 4 by the format's own rule, which bsndParse enforced,
	// so the next sound also starts 4-aligned and the arena never needs padding.
	s_pool_used += h.data_bytes;

	AudioSample* s = &s_sounds[s_sound_count];
	s->data        = dst;
	s->frame_count = h.frame_count;
	s->sample_rate = h.sample_rate;
	s->encoding    = h.encoding;
	s_sound_count++;

	return (AudioSoundId)s_sound_count;   // ids are 1-based; 0 stays invalid
}

static const AudioSample* sampleFor(AudioSoundId id)
{
	if (id == AUDIO_SOUND_NONE || (int)id > s_sound_count) return NULL;
	return &s_sounds[id - 1];
}

// ── Playing ───────────────────────────────────────────────────────────────────────

AudioVoice audioPlayEx(const AudioPlayParams* p)
{
	if (!s_available || !p) return AUDIO_VOICE_NONE;

	const AudioSample* s = sampleFor(p->sound);
	if (!s) return AUDIO_VOICE_NONE;

	float left, right;
	if (p->positional) {
		// The range check happens before the mixer is asked for a voice. Without it, a
		// distant sound would take a channel, be given a gain of zero, and hold that
		// channel for its whole duration — silently starving the sounds that CAN be
		// heard. Eight voices makes that a real effect, not a theoretical one.
		if (audioOutOfRange(&s_listener, p->x, p->y, p->z)) return AUDIO_VOICE_NONE;
		audioPanCompute(&s_listener, p->x, p->y, p->z, p->gain, &left, &right);
		if (!(left > 0.0f) && !(right > 0.0f)) return AUDIO_VOICE_NONE;
	} else {
		left = right = p->gain;
	}

	return mixerPlay(&s_mixer, s, p->priority, left, right, p->looping);
}

AudioVoice audioPlay(AudioSoundId sound, AudioPriority prio, float gain)
{
	AudioPlayParams p;
	memset(&p, 0, sizeof(p));
	p.sound = sound;
	p.priority = prio;
	p.gain = gain;
	p.positional = false;
	return audioPlayEx(&p);
}

AudioVoice audioPlayAt(AudioSoundId sound, AudioPriority prio, float gain,
                       float x, float y, float z)
{
	AudioPlayParams p;
	memset(&p, 0, sizeof(p));
	p.sound = sound;
	p.priority = prio;
	p.gain = gain;
	p.positional = true;
	p.x = x; p.y = y; p.z = z;
	return audioPlayEx(&p);
}

void audioStop(AudioVoice v)  { mixerStop(&s_mixer, v); }
void audioStopAll(void)       { mixerStopAll(&s_mixer); }
void audioUpdate(void)        { mixerUpdate(&s_mixer); }

void audioSetListener(float x, float y, float z, float yaw_rad)
{
	audioListenerFromYaw(&s_listener, x, y, z, yaw_rad);
}

void audioSetMasterVolume(float v) { mixerSetMasterVolume(&s_mixer, v); }
float audioGetMasterVolume(void)   { return mixerGetMasterVolume(&s_mixer); }

// ── Reporting ─────────────────────────────────────────────────────────────────────

size_t audioPoolCapacityBytes(void) { return s_pool_cap; }
size_t audioPoolUsedBytes(void)     { return s_pool_used; }
int    audioLoadedSoundCount(void)  { return s_sound_count; }
int    audioActiveVoiceCount(void)  { return mixerActiveCount(&s_mixer); }
uint32_t audioLinearFreeBefore(void) { return s_linear_before; }
uint32_t audioLinearFreeAfterDsp(void) { return s_linear_after_dsp; }
uint32_t audioLinearFreeAfter(void)  { return s_linear_after; }

// ── Host seam ─────────────────────────────────────────────────────────────────────

#ifndef __3DS__

// Set by audio_mixer_test.c through audioTestReset/ForceInitFailure plus this weak-ish
// hook. On the host with nothing installed there is no backend and audioInit reports the
// silent path — which is the correct answer for a PC and also the exact path a console
// with no DSP firmware takes.
static const AudioBackend* s_host_backend;

static const AudioBackend* audioNdspBackend(void)
{
	return s_host_backend;
}

void audioTestSetBackend(const AudioBackend* b) { s_host_backend = b; }

void audioTestForceInitFailure(bool fail) { s_force_init_failure = fail; }

void audioTestReset(void)
{
	if (s_inited) audioShutdown();
	s_inited = false;
	s_available = false;
	s_reason = "not initialised";
	s_linear_before = 0;
	s_linear_after_dsp = 0;
	s_linear_after = 0;
}

#endif
