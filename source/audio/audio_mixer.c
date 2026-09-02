#include "audio/audio_mixer.h"

#include <string.h>

// No <3ds.h>. See audio_mixer.h.

static float clamp01(float v)
{
	// Written as two comparisons rather than fminf/fmaxf so that a NaN — which compares
	// false against everything — falls through to 0 rather than propagating into a gain
	// the DSP would then be handed. A NaN gain is not a theoretical worry here: it is
	// what a zero-length distance vector produces in audio_pan.c's normalise if the
	// guard there is ever removed.
	if (!(v > 0.0f)) return 0.0f;
	if (v > 1.0f) return 1.0f;
	return v;
}

static AudioVoice makeHandle(uint32_t index, uint32_t generation)
{
	return (AudioVoice)((generation << AUDIO_VOICE_INDEX_BITS) | (index & AUDIO_VOICE_INDEX_MASK));
}

// Resolves a handle to a slot index, or -1. This is the single place a handle is trusted,
// which is why the generation check lives here rather than at each call site.
static int resolve(const AudioMixer* m, AudioVoice v)
{
	if (!m || v == AUDIO_VOICE_NONE) return -1;
	const uint32_t index = (uint32_t)(v & AUDIO_VOICE_INDEX_MASK);
	if ((int)index >= m->voice_count) return -1;
	const uint32_t gen = (uint32_t)(v >> AUDIO_VOICE_INDEX_BITS);
	const AudioVoiceSlot* s = &m->voices[index];
	if (!s->in_use || s->generation != gen) return -1;
	return (int)index;
}

bool mixerInit(AudioMixer* m, const AudioBackend* backend, int voice_count)
{
	if (!m) return false;
	memset(m, 0, sizeof(*m));

	if (voice_count < 1) voice_count = 1;
	if (voice_count > AUDIO_VOICE_COUNT) voice_count = AUDIO_VOICE_COUNT;
	m->voice_count = voice_count;
	m->next_seq = 1;
	m->master_volume = 1.0f;
	m->backend = backend;

	if (!backend || !backend->init) return false;
	m->ready = backend->init(backend->ud);

	// Pushing the volume down even on a successful init is what makes "the slider says
	// 0.4 and the game is at 0.4" true from the first frame rather than from the first
	// time the slider is touched.
	if (m->ready && backend->set_master_volume)
		backend->set_master_volume(backend->ud, m->master_volume);

	return m->ready;
}

void mixerShutdown(AudioMixer* m)
{
	if (!m) return;
	if (m->ready) {
		mixerStopAll(m);
		if (m->backend && m->backend->shutdown)
			m->backend->shutdown(m->backend->ud);
	}
	// The backend pointer is cleared along with everything else so a second shutdown, or
	// a stray play after shutdown, cannot reach a backend that has already torn down.
	memset(m, 0, sizeof(*m));
}

// Picks the voice `prio` should take, or -1 if it may not have one. See the stealing
// contract in audio_mixer.h — this function IS that contract and the comment there is
// what it is checked against.
static int pickVoice(const AudioMixer* m, AudioPriority prio)
{
	for (int i = 0; i < m->voice_count; i++)
		if (!m->voices[i].in_use) return i;

	int      victim = -1;
	uint8_t  victim_prio = 0;
	uint64_t victim_seq = 0;

	for (int i = 0; i < m->voice_count; i++) {
		const AudioVoiceSlot* s = &m->voices[i];
		if (victim < 0 ||
		    s->priority < victim_prio ||
		    (s->priority == victim_prio && s->start_seq < victim_seq)) {
			victim = i;
			victim_prio = s->priority;
			victim_seq = s->start_seq;
		}
	}

	// Strictly higher priority is untouchable. Equal priority loses to the newcomer,
	// which is what stops a burst of same-rank sounds from locking the mixer.
	if (victim >= 0 && victim_prio > (uint8_t)prio) return -1;
	return victim;
}

AudioVoice mixerPlay(AudioMixer* m, const AudioSample* s, AudioPriority prio,
                     float left, float right, bool looping)
{
	if (!m || !m->ready) return AUDIO_VOICE_NONE;
	if (!s || s->frame_count == 0 || !s->data) return AUDIO_VOICE_NONE;

	const int idx = pickVoice(m, prio);
	if (idx < 0) return AUDIO_VOICE_NONE;

	AudioVoiceSlot* v = &m->voices[idx];

	// Stopping before reusing matters even though chn_play would overwrite the channel:
	// ndspChnWaveBufAdd QUEUES onto a channel rather than replacing what is on it, so a
	// play without a preceding stop would append the new sound after the old one and the
	// stolen voice would be heard late instead of not at all. The stop is the steal.
	if (v->in_use && m->backend->chn_stop)
		m->backend->chn_stop(m->backend->ud, idx);

	v->generation++;
	if (v->generation == 0) v->generation = 1;   // never hand out a zero generation
	v->in_use    = true;
	v->looping   = looping;
	v->priority  = (uint8_t)prio;
	v->start_seq = m->next_seq++;
	v->left      = clamp01(left);
	v->right     = clamp01(right);

	if (m->backend->chn_play)
		m->backend->chn_play(m->backend->ud, idx, s, v->left, v->right, looping);

	return makeHandle((uint32_t)idx, v->generation);
}

void mixerStop(AudioMixer* m, AudioVoice v)
{
	const int idx = resolve(m, v);
	if (idx < 0) return;
	if (m->ready && m->backend && m->backend->chn_stop)
		m->backend->chn_stop(m->backend->ud, idx);
	m->voices[idx].in_use = false;
	m->voices[idx].looping = false;
}

bool mixerSetMix(AudioMixer* m, AudioVoice v, float left, float right)
{
	const int idx = resolve(m, v);
	if (idx < 0) return false;
	m->voices[idx].left  = clamp01(left);
	m->voices[idx].right = clamp01(right);
	if (m->ready && m->backend && m->backend->chn_set_mix)
		m->backend->chn_set_mix(m->backend->ud, idx,
		                        m->voices[idx].left, m->voices[idx].right);
	return true;
}

void mixerStopAll(AudioMixer* m)
{
	if (!m) return;
	for (int i = 0; i < m->voice_count; i++) {
		if (!m->voices[i].in_use) continue;
		if (m->ready && m->backend && m->backend->chn_stop)
			m->backend->chn_stop(m->backend->ud, i);
		m->voices[i].in_use = false;
		m->voices[i].looping = false;
	}
}

void mixerUpdate(AudioMixer* m)
{
	if (!m || !m->ready || !m->backend || !m->backend->chn_is_playing) return;
	for (int i = 0; i < m->voice_count; i++) {
		AudioVoiceSlot* v = &m->voices[i];
		if (!v->in_use) continue;
		// A looping voice is never reaped by the hardware's opinion — it is supposed to
		// run forever, and asking whether it is still playing invites a race where a
		// buffer boundary reads as finished and the ambience stops for good.
		if (v->looping) continue;
		if (!m->backend->chn_is_playing(m->backend->ud, i))
			v->in_use = false;
	}
}

bool mixerVoiceLive(const AudioMixer* m, AudioVoice v)
{
	return resolve(m, v) >= 0;
}

int mixerActiveCount(const AudioMixer* m)
{
	if (!m) return 0;
	int n = 0;
	for (int i = 0; i < m->voice_count; i++)
		if (m->voices[i].in_use) n++;
	return n;
}

void mixerSetMasterVolume(AudioMixer* m, float v)
{
	if (!m) return;
	m->master_volume = clamp01(v);
	if (m->ready && m->backend && m->backend->set_master_volume)
		m->backend->set_master_volume(m->backend->ud, m->master_volume);
}

float mixerGetMasterVolume(const AudioMixer* m)
{
	return m ? m->master_volume : 0.0f;
}
