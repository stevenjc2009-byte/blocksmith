#pragma once

// Turns "a sound happened at this block, and the player is standing here facing there"
// into the two per-side gains the mixer wants. Pure maths — no <3ds.h>, no state, no
// allocation — so the curve can be checked against numbers in the host suite instead of
// by walking around the world with your ear to the speaker.

#include <stdbool.h>

typedef struct {
	float x, y, z;
	// The listener's RIGHT vector, flattened to the XZ plane and normalised. Stored
	// rather than a yaw so the caller does the trig once per frame instead of this file
	// doing it once per sound, and so a test can hand in an exact (1,0) without going
	// through a sinf/cosf whose last bit differs between host and devkitARM.
	float right_x, right_z;
} AudioListener;

// Fills `l` from a yaw in radians, using the same convention scene/camera.c uses:
// yaw 0 looks down -Z, and +yaw turns toward +X. The right vector is therefore
// (cos yaw, -sin yaw)... which is the kind of sentence that is wrong half the time it is
// written, so audio_pan_test.c checks all four cardinal yaws against hand-worked
// expectations rather than against this comment.
void audioListenerFromYaw(AudioListener* l, float x, float y, float z, float yaw_rad);

// Distances in blocks. A block is 1.0 world unit in this game.
//
// Inside AUDIO_REF_DIST a sound is at full gain — without it, a block broken directly
// under the player would divide by a distance near zero. Beyond AUDIO_MAX_DIST it is
// silent and, more usefully, the caller can skip it entirely.
#define AUDIO_REF_DIST 2.0f
#define AUDIO_MAX_DIST 24.0f

// LINEAR rolloff between the two, not inverse-square.
//
// Inverse-square is physically right and wrong for this game. On a 3DS's speakers, with
// a 24-block audible radius, an inverse-square curve puts 90% of the perceived loudness
// change inside the first four blocks and leaves everything from 8 to 24 blocks sounding
// identically faint — so the cue that a sound is "over there rather than right here",
// which is the only thing this is for, lands in a range the player is rarely in. Linear
// spreads the change across the whole radius. It is the same reason game mixers ship a
// rolloff CURVE setting at all.
//
// Both are one expression; if this ever needs to change, it changes here and
// audio_pan_test.c's table changes with it.
float audioDistanceGain(float distance);

// The full computation. `gain` is the caller's own 0..1 volume for this sound, applied
// on top of distance attenuation.
//
// Panning is CONSTANT POWER: at dead centre both sides get 0.707 rather than 1.0, so a
// sound sweeping past the listener holds a steady loudness instead of bulging in the
// middle. The consequence worth knowing at a call site is that a positional sound at
// gain 1.0 directly in front is quieter than a NON-positional sound at gain 1.0, which
// gets 1.0 on both sides. That is correct — they are different things — but it is why
// UI sounds and world sounds should not be balanced against each other by ear alone.
//
// Behind the listener pans exactly as in front: the 3DS has two speakers a few
// centimetres apart and no way to express front/back, so pretending otherwise would only
// add a discontinuity as a sound passes the 90-degree line.
//
// A sound AT the listener's exact position has no direction. Rather than normalising a
// zero vector (a NaN that would reach the DSP as a gain), it is treated as centred.
void audioPanCompute(const AudioListener* l,
                     float sx, float sy, float sz,
                     float gain,
                     float* out_left, float* out_right);

// True when a sound at (sx,sy,sz) is far enough away to be inaudible. Callers use it to
// skip the work of loading/starting a sound at all; audioPanCompute agrees with it, so a
// caller that ignores this still gets silence rather than a quiet sound.
bool audioOutOfRange(const AudioListener* l, float sx, float sy, float sz);
