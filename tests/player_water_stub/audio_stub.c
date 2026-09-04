// Host stubs for the two audio entry points source/scene/player.c reaches on entering and
// leaving water. Same philosophy as citro3d_stub.c beside it: this is a stub, not a
// reimplementation of anything under test. The real source/scene/player.c is linked and
// exercised; only the sound system it calls out to is replaced, because the real one is
// libctru/DSP code that cannot build or run on the host.
//
// Why this file exists at all: player.c's splash handling gained
//   audioPlayAt(audioSfxId(SFX_SPLASH), ...)
// alongside the particle spawn it already did. The console build links the real audio
// sources so it was unaffected, but this stanza links player.c WITHOUT them, so the suite
// died at link time with "undefined reference to 'audioSfxId'" - and under
// tools/run_host_tests.sh's `set -e` that takes every later stanza dark rather than red.
//
// These return the "did nothing" values the real API documents for a failure, so player.c
// takes exactly the same branch it would on a console with audio unavailable:
// audio.h:39 defines AUDIO_SOUND_NONE and audio_mixer.h:52 AUDIO_VOICE_NONE, and audio.h's
// own header comment says play "returns AUDIO_VOICE_NONE, and nothing crashes, blocks or
// logs per frame" in that state. Nothing in player_water_test.c asserts on sound, so this
// is deliberately silent rather than counting: if a future test wants to assert a splash
// cue fires, the counter belongs here and the assertion in the test, not in player.c.

#include "audio/audio.h"
#include "audio/audio_sfx.h"

AudioSoundId audioSfxId(SfxSlot slot)
{
	(void)slot;
	return AUDIO_SOUND_NONE;
}

AudioVoice audioPlay(AudioSoundId sound, AudioPriority prio, float gain)
{
	(void)sound;
	(void)prio;
	(void)gain;
	return AUDIO_VOICE_NONE;
}

AudioVoice audioPlayAt(AudioSoundId sound, AudioPriority prio, float gain,
                       float x, float y, float z)
{
	(void)sound;
	(void)prio;
	(void)gain;
	(void)x;
	(void)y;
	(void)z;
	return AUDIO_VOICE_NONE;
}
