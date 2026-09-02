#pragma once

// The seam between the mixer's policy and the DSP's hardware.
//
// Everything interesting about a voice allocator — which voice a sound lands on, what
// happens when they are all busy, whether a stale handle can stop the wrong sound — is
// pure bookkeeping, and none of it needs a 3DS. Putting the four calls the mixer makes
// into a struct of function pointers is what lets audio_mixer.c be the REAL module in a
// host test rather than a copy of it, the same split app/hw.c and app/debugmenu_ui.c
// already use in this tree.
//
// The console implementation is audio_ndsp.c. The test implementation is a recorder in
// audio_mixer_test.c that remembers every call so the test can assert on the sequence.
//
// Deliberately NOT in this seam: sample loading and the linear pool. Those are in
// audio.c because they are about memory rather than policy, and a fake for them would
// be a fake for malloc.

#include <stdbool.h>
#include <stdint.h>

// One loaded sound, as the mixer sees it. `data` points into the linear pool on the
// console and into a plain malloc on the host; the mixer never dereferences it, it only
// hands it to the backend, which is what makes the host build legal.
typedef struct {
	const void* data;
	uint32_t    frame_count;
	uint32_t    sample_rate;
	uint16_t    encoding;      // BSND_ENC_*
} AudioSample;

typedef struct AudioBackend {
	// Brings the hardware up. Returning false is a NORMAL outcome, not an assertion
	// failure: a console with no DSP firmware dumped cannot play anything, and the
	// game's answer to that is to run silently, not to refuse to boot. Everything
	// downstream of this returning false is exercised by the host suite.
	bool (*init)(void* ud);

	void (*shutdown)(void* ud);

	// 0.0 .. 1.0. Called once at init and again whenever the options slider moves.
	void (*set_master_volume)(void* ud, float volume);

	// Starts `s` on hardware channel `chn` with the given per-side gains, both already
	// clamped to 0..1 by the mixer. `looping` keeps the buffer playing until stopped.
	void (*chn_play)(void* ud, int chn, const AudioSample* s,
	                 float left, float right, bool looping);

	// Idempotent: stopping a channel that is not playing is legal and does nothing.
	void (*chn_stop)(void* ud, int chn);

	// The only thing the mixer knows about whether a sound has finished. On the console
	// this is ndspChnIsPlaying; in a test it is whatever the test decided.
	bool (*chn_is_playing)(void* ud, int chn);

	// Re-pans an already-playing voice, for a positional sound whose source or listener
	// moved. Separate from chn_play so moving a sound does not restart it.
	void (*chn_set_mix)(void* ud, int chn, float left, float right);

	void* ud;
} AudioBackend;
