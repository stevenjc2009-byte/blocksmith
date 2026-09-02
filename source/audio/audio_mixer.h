#pragma once

// Voice allocation, priority and stealing. Pure policy — no <3ds.h>, no allocation, no
// file IO. Everything the hardware does is reached through AudioBackend so this module
// is the one the host suite links, not a copy of it.

#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_backend.h"

// Eight simultaneous sounds. The DSP has 24 channels, so this is a policy number rather
// than a hardware one, and it is deliberately well under the ceiling:
//
//   * Eight is already more than a blocky survival game asks for. The worst honest case
//     is a footstep, a block break, the block-break's placement confirm, water ambience,
//     and a couple of mobs — six.
//   * Every channel above what is used still costs DSP frame time on a console whose
//     audio budget is shared with everything else the system is doing, and this project
//     has never measured that cost. Taking eight and leaving 16 is the conservative
//     direction to be wrong in.
//   * The number is what the stealing policy is worth testing against. With 24 voices a
//     bug in stealing would essentially never fire; with 8 it fires the first time
//     someone stands in the rain.
//
// Raising it is a one-line change and a re-run of audio_mixer_test.c, which drives the
// count rather than hardcoding 8.
#define AUDIO_VOICE_COUNT 8

// Higher wins. Deliberately coarse: the only decision these feed is "may this sound
// take a channel away from that one", and a finer scale would be a scale nobody could
// assign consistently at a call site.
typedef enum {
	AUDIO_PRIO_AMBIENT = 0,   // water, wind — the first thing that should be dropped
	AUDIO_PRIO_LOW     = 1,   // footsteps
	AUDIO_PRIO_NORMAL  = 2,   // block break/place, most world sounds
	AUDIO_PRIO_HIGH    = 3,   // damage, mob attacks — things the player must hear
	AUDIO_PRIO_UI      = 4,   // menu ticks; never dropped, never positional
} AudioPriority;

// An opaque handle to a playing sound. Zero is always invalid and always safe to pass
// to any function taking one.
//
// It is NOT a bare index. The low bits index the voice table and the high bits carry a
// generation counter that increments every time a voice is handed out, so a handle kept
// by a caller across the voice being recycled — a footstep that finished, its channel
// reused by a block break, and the caller then calling audioStop on its stale handle —
// is rejected instead of stopping a stranger's sound. That failure mode is invisible
// until it is not, and it costs one comparison to make impossible.
typedef uint32_t AudioVoice;

#define AUDIO_VOICE_NONE ((AudioVoice)0)

#define AUDIO_VOICE_INDEX_BITS 8
#define AUDIO_VOICE_INDEX_MASK ((AudioVoice)((1u << AUDIO_VOICE_INDEX_BITS) - 1u))

typedef struct {
	// Generation stamped into the handle this voice last issued. Starts at 1 so a
	// zeroed table can never produce a valid handle, and never returns to 0 on wrap.
	uint32_t generation;
	// Monotonic across the whole mixer; the tie-break for stealing between two voices
	// of equal priority. 0 means "this voice has never been used".
	uint64_t start_seq;
	uint8_t  priority;
	bool     in_use;
	bool     looping;
	float    left, right;
} AudioVoiceSlot;

typedef struct {
	const AudioBackend* backend;
	AudioVoiceSlot      voices[AUDIO_VOICE_COUNT];
	int                 voice_count;   // <= AUDIO_VOICE_COUNT; the mixer never exceeds it
	uint64_t            next_seq;
	float               master_volume;
	bool                ready;         // backend->init succeeded
} AudioMixer;

// Zeroes `m`, records `backend`, and calls backend->init. Returns what init returned.
//
// A false return is not an error the caller has to handle beyond noticing: every other
// function in this file is safe to call on a mixer whose init failed and does nothing.
// That is the silent-fallback contract, and it is tested directly rather than assumed.
//
// `voice_count` is clamped into 1..AUDIO_VOICE_COUNT. Passing 0 or a negative is a
// caller bug that yields 1 voice rather than a divide-by-zero later.
bool mixerInit(AudioMixer* m, const AudioBackend* backend, int voice_count);

// Stops everything and calls backend->shutdown. Safe on a mixer that never initialised
// and safe to call twice.
void mixerShutdown(AudioMixer* m);

// Starts `s`. `left` and `right` are the final per-side gains, 0..1, already including
// distance attenuation and pan (see audio_pan.h) but NOT master volume — master is
// applied by the backend, once, for every voice at the same time.
//
// Returns AUDIO_VOICE_NONE when the sound did not start. The three ways that happens:
//
//   1. The mixer is not ready (no DSP). Silent, expected, not an error.
//   2. `s` is NULL or has no frames.
//   3. Every voice is busy AND every one of them is playing something of strictly
//      HIGHER priority than `prio`. See the stealing rules below.
//
// ── Stealing ──────────────────────────────────────────────────────────────────────
//
// A free voice is always preferred, lowest index first — index order rather than
// round-robin so that a given sequence of calls produces a given assignment, which is
// what makes the host test able to assert on channel numbers at all.
//
// With none free, the victim is the busy voice of LOWEST priority, ties broken by the
// OLDEST start_seq. The incoming sound wins iff victim_priority <= prio.
//
// The `<=` rather than `<` is the load-bearing half: equal priority means the NEWER
// sound displaces the OLDEST equal-priority one. Without it, eight simultaneous
// footsteps would lock every later footstep out until they all finished, and the game
// would go quiet exactly when the most is happening. With it, a burst degrades into
// "the most recent eight", which is what a listener expects.
AudioVoice mixerPlay(AudioMixer* m, const AudioSample* s, AudioPriority prio,
                     float left, float right, bool looping);

// Stops the voice `v` refers to. A stale, zero or malformed handle is ignored — this is
// the reason handles carry a generation.
void mixerStop(AudioMixer* m, AudioVoice v);

// Re-pans a still-playing voice without restarting it. A stale handle is ignored.
// Returns true if the handle was live and the mix was applied.
bool mixerSetMix(AudioMixer* m, AudioVoice v, float left, float right);

void mixerStopAll(AudioMixer* m);

// Reaps voices the backend says have finished. Call once a frame. Nothing else marks a
// non-looping voice free, so skipping this leaks every voice into the busy set and the
// mixer degrades to stealing-only — which still plays sounds, but always the loudest
// eight. That is the failure mode to look for if the game goes quiet after a minute.
void mixerUpdate(AudioMixer* m);

// True if `v` names a voice that is still allocated to the caller.
bool mixerVoiceLive(const AudioMixer* m, AudioVoice v);

int mixerActiveCount(const AudioMixer* m);

// Clamped to 0..1 and pushed to the backend. Stored even when the mixer is not ready, so
// that the options slider still round-trips on a console with no DSP.
void mixerSetMasterVolume(AudioMixer* m, float v);
float mixerGetMasterVolume(const AudioMixer* m);
