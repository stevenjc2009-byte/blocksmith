#include "audio/audio_pan.h"

#include <math.h>

// No <3ds.h>. See audio_pan.h.

void audioListenerFromYaw(AudioListener* l, float x, float y, float z, float yaw_rad)
{
	if (!l) return;
	l->x = x;
	l->y = y;
	l->z = z;
	// Forward is (sin yaw, -cos yaw) for "yaw 0 looks down -Z, +yaw turns toward +X".
	// Right is forward rotated -90 degrees about +Y, which is (-forward_z, forward_x)
	// = (cos yaw, sin yaw). audio_pan_test.c checks this against the four cardinals.
	l->right_x = cosf(yaw_rad);
	l->right_z = sinf(yaw_rad);
}

float audioDistanceGain(float distance)
{
	// The FAR test comes first, and it is spelled !(d < max) rather than d >= max, so that
	// a NaN — which compares false against everything — falls into it and returns silence.
	//
	// The order is load-bearing and was wrong when this was first written: with the near
	// test first, !(NaN > ref) is true and a NaN distance returned FULL gain, which is the
	// worst available answer. audio_bsnd_test.c's testDistanceCurve caught it. Reversing
	// the two costs nothing and makes the degenerate case silent instead of deafening.
	if (!(distance < AUDIO_MAX_DIST)) return 0.0f;
	if (distance <= AUDIO_REF_DIST) return 1.0f;
	return (AUDIO_MAX_DIST - distance) / (AUDIO_MAX_DIST - AUDIO_REF_DIST);
}

static float dist3(float dx, float dy, float dz)
{
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

void audioPanCompute(const AudioListener* l,
                     float sx, float sy, float sz,
                     float gain,
                     float* out_left, float* out_right)
{
	float left = 0.0f, right = 0.0f;
	if (!l) goto done;

	if (!(gain > 0.0f)) goto done;
	if (gain > 1.0f) gain = 1.0f;

	const float dx = sx - l->x;
	const float dy = sy - l->y;
	const float dz = sz - l->z;

	const float d = dist3(dx, dy, dz);
	const float g = gain * audioDistanceGain(d);
	if (!(g > 0.0f)) goto done;

	// Pan is computed from the HORIZONTAL offset only. Height carries into the distance
	// above (a sound two blocks up is genuinely further away) but not into left/right,
	// because there is no vertical axis two speakers can express and folding dy into the
	// horizontal length would make a sound directly overhead pan as if it were beside
	// the player.
	float pan = 0.0f;
	const float horiz = sqrtf(dx * dx + dz * dz);

	// The zero-length guard. Below a millimetre there is no meaningful direction, and
	// dividing by `horiz` would be the NaN that reaches the DSP as a gain. Centred is
	// the honest answer for a sound at the listener's own position.
	if (horiz > 1e-4f) {
		pan = (dx * l->right_x + dz * l->right_z) / horiz;
		// The dot of two unit vectors is in [-1,1] mathematically; float rounding can
		// put it a bit outside, and sqrtf of a negative below would be a NaN.
		if (pan < -1.0f) pan = -1.0f;
		if (pan > 1.0f) pan = 1.0f;
	}

	left  = g * sqrtf((1.0f - pan) * 0.5f);
	right = g * sqrtf((1.0f + pan) * 0.5f);

done:
	if (out_left)  *out_left  = left;
	if (out_right) *out_right = right;
}

bool audioOutOfRange(const AudioListener* l, float sx, float sy, float sz)
{
	if (!l) return true;
	const float d = dist3(sx - l->x, sy - l->y, sz - l->z);
	// Deliberately the same comparison audioDistanceGain makes, spelled the same way, so
	// the two cannot disagree about the boundary case of exactly AUDIO_MAX_DIST.
	return !(d < AUDIO_MAX_DIST);
}
