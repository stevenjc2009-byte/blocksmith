#pragma once

// The audio subsystem's public face. This is the only header the rest of the game
// includes; audio_mixer.h, audio_pan.h and audio_bsnd.h are the internals it is built
// out of and are included directly only by the audio tests.
//
// ── The contract that matters most ────────────────────────────────────────────────
//
// EVERY function here is safe to call when there is no audio at all.
//
// The 3DS's DSP needs a firmware blob that homebrew cannot ship — it is dumped from the
// console by the user. On a console where that has never been done, ndspInit() fails and
// there is nothing anyone can do about it from inside the game. The answer is that the
// game runs silently: audioInit() returns false, audioAvailable() answers false, every
// play returns AUDIO_VOICE_NONE, and nothing crashes, blocks or logs per frame. Callers
// are NOT expected to check anything. `audioPlaySfx(SFX_BLOCK_BREAK, ...)` at a mining
// site is correct code whether or not the console can make a noise.
//
// That path is not reasoned about, it is tested: audio_mixer_test.c drives the whole API
// against a backend whose init fails and asserts the hardware is never touched.
//
// ── Memory ────────────────────────────────────────────────────────────────────────
//
// Sound data must live in LINEAR memory, because the DSP reads it by physical address.
// Linear is the same pool the GPU draws vertices out of, which on this project is the
// scarce one — audio competes with render distance, not with the app heap. So the pool
// is a single fixed-size arena taken once at boot, sized per console, and audioInit()
// reports exactly what it took. Nothing here ever allocates again after boot.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_mixer.h"
#include "audio/audio_pan.h"

// Handle to a loaded sound. Zero is invalid and safe to pass anywhere.
typedef uint16_t AudioSoundId;
#define AUDIO_SOUND_NONE ((AudioSoundId)0)

// How many distinct sounds can be resident. A ceiling rather than a budget — the real
// limit is AUDIO_POOL_BYTES below. 64 is enough for every sound this game is planned to
// have and costs 64 * sizeof(AudioSample) of static app heap, not linear.
#define AUDIO_SOUND_MAX 64

// ── The linear pool ───────────────────────────────────────────────────────────────
//
// Measured context, from two byte-identical boots of this build in Azahar on a New 3DS
// profile: of the 67,108,864-byte linear heap, about 18,285,568 bytes are free in play.
// The rest is the GPU's — framebuffers, the chunk mesh arenas in scene/chunk_render.c,
// sprite and overlay vertex buffers. The app heap is a separate and far roomier
// 60,724,120 bytes and is NOT where any of this can go.
//
// The three sounds that ship today need 87,668 bytes (tools/make_sounds.py --check).
// The pool is sized well above that because the point of a fixed arena taken at boot is
// that adding a sound later does not move the render-distance boundary: the cost is
// paid once, visibly, here, rather than creeping up a sound at a time.
//
//   New 3DS: 1,048,576 bytes — 5.7% of the free linear pool. Roughly 23 seconds of
//            22.05 kHz mono PCM16, which is the whole planned sound set with room over.
//   Old 3DS:   393,216 bytes — the same headroom argument at a third of the size,
//            because an Old 3DS has materially less linear free and its render distance
//            is already the thing being protected (scene/render_dist.h caps it lower).
//            8.9 seconds of audio; the three shipped sounds are 2.0 of that.
//
// If these ever need to grow, the honest statement is that linear taken here is linear
// the chunk mesh arenas do not get, and that trade shows up as render distance.
#define AUDIO_POOL_BYTES_NEW3DS 1048576u
#define AUDIO_POOL_BYTES_OLD3DS  393216u

typedef struct {
	AudioSoundId  sound;
	AudioPriority priority;
	float         gain;        // 0..1, before distance attenuation
	bool          positional;  // false: plays centred at full gain, ignoring x/y/z
	float         x, y, z;     // world position, blocks
	bool          looping;     // for ambience; the caller keeps the handle to stop it
} AudioPlayParams;

// ── Lifecycle ─────────────────────────────────────────────────────────────────────

// Brings up the DSP and takes the linear pool. Call once, after hwInit() — the pool size
// depends on which console this is.
//
// romfs does NOT need to be mounted first, and must not be left mounted for this: audioLoad
// takes and releases the mount itself. See the romfsHoldBegin comment in audio.c for why
// holding it open would silently switch off the self-updater.
//
// Returns false when there is no audio. That is not a failure the caller must handle:
// the game continues, silently, and every other function here keeps working as a no-op.
// It is worth logging once, and nothing more.
//
// Safe to call twice; the second call does nothing and returns the first call's answer.
bool audioInit(void);

// Releases the pool and the DSP. Safe on a failed init and safe to call twice.
void audioShutdown(void);

// True when sound can actually be heard. The ONLY legitimate use is a debug overlay or a
// one-time log line — do not gate play calls on it, they handle it themselves.
bool audioAvailable(void);

// Why audioInit() returned false, for that one log line. Never NULL. Reads "ok" after a
// successful init.
const char* audioFailureReason(void);

// ── Loading ───────────────────────────────────────────────────────────────────────

// Loads a BSND file into the pool. `path` is a full devoptab path — "romfs:/sfx/x.bsnd".
// A "romfs:" path mounts and unmounts romfs around the read, leaving the mount exactly as it
// was found; any other path is opened as-is.
//
// Returns AUDIO_SOUND_NONE if the file is missing, malformed, or does not fit in the
// remaining pool. All three are logged once with the reason and are not fatal: a game
// missing one sound is a game missing one sound.
//
// There is no unload. Sounds are loaded at boot and live for the process, which is what
// lets the pool be a bump arena with no free list and no fragmentation.
AudioSoundId audioLoad(const char* path);

// ── Playing ───────────────────────────────────────────────────────────────────────

// The general form. Returns AUDIO_VOICE_NONE when the sound did not start — silently,
// out of range, or outranked by every busy voice. Callers that do not intend to stop or
// move the sound can ignore the return entirely.
AudioVoice audioPlayEx(const AudioPlayParams* p);

// A sound with no position: UI, and anything that is "about the player" rather than
// happening somewhere. Full gain on both sides.
AudioVoice audioPlay(AudioSoundId sound, AudioPriority prio, float gain);

// A sound at a world position, attenuated and panned against the listener set by
// audioSetListener. This is the one block break/place and footsteps use.
AudioVoice audioPlayAt(AudioSoundId sound, AudioPriority prio, float gain,
                       float x, float y, float z);

// Stops a specific voice. A stale or zero handle is ignored — see audio_mixer.h on why
// handles carry a generation.
void audioStop(AudioVoice v);

void audioStopAll(void);

// ── Per-frame ─────────────────────────────────────────────────────────────────────

// Where the player is and which way they are facing. Call once a frame before any
// positional play, from wherever the camera is updated. Until it is called the listener
// sits at the origin facing -Z, so a positional sound played before the first
// audioSetListener is attenuated against the origin rather than crashing.
void audioSetListener(float x, float y, float z, float yaw_rad);

// Reaps finished voices. Call once a frame. See mixerUpdate in audio_mixer.h for what
// skipping it costs.
void audioUpdate(void);

// ── Volume ────────────────────────────────────────────────────────────────────────

// 0..1, clamped. This is what the options slider drives. The value is remembered even
// when there is no DSP, so the setting round-trips through options.ini on any console.
void audioSetMasterVolume(float v);
float audioGetMasterVolume(void);

// ── Reporting, for the debug overlay and the boot log ─────────────────────────────

size_t audioPoolCapacityBytes(void);
size_t audioPoolUsedBytes(void);
int    audioLoadedSoundCount(void);
int    audioActiveVoiceCount(void);

// linearSpaceFree() sampled at three points during audioInit. All three are 0 on the host
// and on a console where audioInit never ran.
//
//   Before()    entry, before ndspInit
//   AfterDsp()  after the DSP came up, before the pool was taken
//   After()     after the pool was taken
//
// Three samples rather than two because the pool is NOT the whole linear cost. ndspInit()
// allocates its own buffers inside libctru and this project has no measurement of how
// much: nothing has run on real 3DS hardware since v1.2.5. Splitting the sample answers
// both halves separately on the first hardware boot.
//
//   ndsp's own linear cost = Before()   - AfterDsp()
//   this subsystem's pool  = AfterDsp() - After()    (should equal AUDIO_POOL_BYTES_*)
//
// If that second line does not come out at exactly the pool constant, the allocator added
// padding and the budget stated above is wrong by the difference. Nothing in the tree can
// measure any of it without a console.
uint32_t audioLinearFreeBefore(void);
uint32_t audioLinearFreeAfterDsp(void);
uint32_t audioLinearFreeAfter(void);

#ifndef __3DS__
// Host-only seam. Installs the backend the next audioInit() will use. NULL — the state a
// host build starts in — makes audioInit take the no-audio path, which is correct for a
// PC and is the same path a console with no DSP firmware takes.
struct AudioBackend;
void audioTestSetBackend(const struct AudioBackend* b);
// Forces the next audioInit() to take the backend-init-failed path, so
// the silent-fallback contract at the top of this file can be exercised against the real
// audio.c rather than a description of it. Console builds have no reason to.
void audioTestForceInitFailure(bool fail);
// Forgets that audioInit() ran, so a test can re-arm it.
void audioTestReset(void);
// Which console the pool is sized for is NOT a seam of this module's own: audioInit
// asks app/hw.c's hwIsNew3ds(), and app/hw.h already carries hwTestSetNew3ds() for
// exactly this. A test that wants the Old 3DS pool calls that, so there is one answer to
// "which console is this" in the tree rather than two that can disagree.
#endif
