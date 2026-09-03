#pragma once

// Which loaded sound is which, and the two cues that are not a single call.
//
// audio/audio.h is the mixer's public face: it can load a sound and play one, and it
// deliberately knows nothing about what any of them MEAN. This file is the other half —
// the game's names for the three sounds that ship, and the small amount of state a
// footstep needs that a break and a place do not.
//
// ── Why the ids are registered rather than assumed ────────────────────────────────
//
// audioLoad() hands back a 1-based id in load order, and until v1.9.0 source/main.c threw
// all three returns away and a comment recorded that break was 1, place 2 and footstep 3.
// That is correct exactly as long as nothing goes wrong, and it fails SILENTLY in two
// ordinary ways:
//
//   * A fourth sound inserted into the middle of the load block renumbers every sound
//     after it. Nothing fails to compile and nothing logs; blocks just start breaking
//     with the wrong noise.
//   * A single load FAILING renumbers everything after it too, and this is the worse one
//     because it needs no edit at all. audioLoad returns AUDIO_SOUND_NONE for a missing,
//     malformed or over-large file (audio.h) and does NOT consume an id — so if
//     block_break.bsnd is the one that fails, block_place.bsnd becomes id 1 and every
//     block break in the game plays the PLACE sound. An assumed id cannot express "that
//     sound is not loaded"; a captured one is AUDIO_SOUND_NONE and plays nothing, which is
//     the honest answer.
//
// So main.c registers what audioLoad actually returned and every call site asks for a
// SLOT. There is nothing here to assert against, because there is no longer an assumption:
// the id is whatever the loader said it was.

#include <stdbool.h>

#include "audio/audio.h"

// The nine sounds tools/make_sounds.py packs into romfs:/sfx/. Slots, not ids — the id is
// whatever audioLoad returned for the file registered into this slot, and an unregistered
// or failed slot is AUDIO_SOUND_NONE and plays nothing.
//
// v1.8.17 lane SOUND-A added the six slots below SFX_FOOTSTEP. Each has a real clip in
// romfs/sfx/ and a real gameplay event to hang off — world/survival.h's health/hunger
// tracking (hurt, death, eat), world/crafting.h's craftMake (craft), world/physics.h's
// Body::wet transition (splash), and scene/ui.c's rising-edge tap (ui_tap) — but NO call
// site for any of them exists yet in this tree. Wiring them into main.c/interact.c/ui.c is
// deliberately not done by this lane; see the six slots' own comments for exactly where
// each one is meant to fire.
typedef enum {
	SFX_BLOCK_BREAK = 0,
	SFX_BLOCK_PLACE = 1,
	SFX_FOOTSTEP    = 2,

	// Fires once per point of health LOST — a fall-damage or starvation tick from
	// world/survival.h's fallDamageUpdate()/survivalTick(), whichever caller notices
	// `survival.health` decreased since the last frame it checked. AUDIO_PRIO_HIGH: the
	// mixer doc names damage as "things the player must hear" (audio_mixer.h).
	SFX_HURT        = 3,

	// Fires once, the frame health reaches 0 — the `true` return from
	// fallDamageUpdate()/survivalTick() (world/survival.h). Also AUDIO_PRIO_HIGH.
	SFX_DEATH       = 4,

	// Fires on a `true` return from survivalEat() (world/survival.h) — a slot the player
	// tapped actually fed them, not a tap on an empty or non-food slot. AUDIO_PRIO_NORMAL.
	SFX_EAT         = 5,

	// Fires on a `true` return from craftMake() (world/crafting.h), reached today from
	// scene/ui.c's handleCraftTap(). Non-positional (audioPlay, not audioSfxPlayAtBlock) —
	// a craft is "about the player", not somewhere in the world. AUDIO_PRIO_NORMAL.
	SFX_CRAFT       = 6,

	// Fires on the frame world/physics.h's bodyWetUpdate() output crosses the BODY_DRY /
	// non-dry boundary in EITHER direction — entering or leaving water. Positional, at the
	// body's own position, same as a footstep. AUDIO_PRIO_NORMAL.
	SFX_SPLASH      = 7,

	// Fires on every rising-edge UI tap scene/ui.c's uiUpdateDraw() accepts — the same
	// `tap` variable ui.c's own comment describes as "rising edge only, same as title.c's
	// own tap". Covers slot pickup/drop, hotbar selection and the crafting panel's own tap
	// target alike; craft SUCCESS still gets its own SFX_CRAFT on top of this one.
	// Non-positional, AUDIO_PRIO_UI — "never dropped, never positional" (audio_mixer.h).
	SFX_UI_TAP      = 8,

	SFX_SLOT_COUNT  = 9,
} SfxSlot;

// Records the id audioLoad() returned. Call once per sound at boot. A slot outside the
// enum is ignored rather than written past the table, and AUDIO_SOUND_NONE is a legal
// value to register: it is what a failed load returns and it is what the slot should hold.
void audioSfxRegister(SfxSlot slot, AudioSoundId id);

// AUDIO_SOUND_NONE for a slot that was never registered or whose load failed.
AudioSoundId audioSfxId(SfxSlot slot);

// Forgets every registration. Exists for the host suite, which drives audioInit() more
// than once; the console never needs it, because sounds are loaded at boot and live for
// the process (audio.h).
void audioSfxReset(void);

// Plays `slot` positionally at the CENTRE of block cell (bx, by, bz).
//
// The centre and not the corner: a block's integer coordinate is its minimum corner, and
// panning a sound from the corner puts a block broken directly underfoot half a block to
// one side of the player. Half a block is inside AUDIO_REF_DIST so it changes no gain, but
// it does change the pan, and the pan is the only thing a positional cue is for.
//
// Returns the voice, which every current caller ignores — a break sound is never stopped
// or moved. Returned anyway because audioPlayAt returns it and swallowing it here would
// make a future looping cue need a second function.
AudioVoice audioSfxPlayAtBlock(SfxSlot slot, AudioPriority prio, float gain,
                               int bx, int by, int bz);

// ── Footsteps ─────────────────────────────────────────────────────────────────────
//
// A footstep is the one cue with no event to hang off. A break and a place each happen at
// exactly one instant in scene/interact.c and the sound goes there; walking has no instant
// at all, only a body that is a little further along than it was last frame. So this is a
// distance accumulator, and the three things it must NOT do are what it is shaped around:
// it must not fire per frame, it must not fire in the air, and it must not fire while
// standing still.
//
// Distance, not time. A timer would keep ticking while the player is pressed against a
// wall going nowhere, and would run at the same rate walking and swimming.

// Horizontal blocks between footsteps.
//
// 2.0 with PLAYER_WALK_SPEED at 4.3 blocks/s is one sound every 0.465 s, i.e. 2.15 a
// second. Human walking cadence is about 1.8-2.1 steps a second, so this lands on the fast
// edge of natural and reads as walking rather than as a metronome or a jog. It is also the
// number that keeps footsteps cheap against the 8-voice mixer (audio_mixer.h): at just over
// two a second, and at AUDIO_PRIO_LOW where they are the first thing a break or a mob
// steals from, a walking player never holds more than one or two voices.
//
// Not derived from PLAYER_WALK_SPEED. This file deliberately does not include
// world/physics.h — a cadence expressed as "a sound every N blocks" stays right when the
// walk speed changes, whereas one expressed as a fraction of the walk speed would silently
// re-tune itself and would couple audio to the physics header for one constant.
#define SFX_FOOTSTEP_STRIDE  2.0f

// A single update that moved the body further than this horizontally is not a stride, it is
// a teleport — a respawn, a world load, a server pose restore — and its distance is thrown
// away rather than banked. At PLAYER_WALK_SPEED one 60 fps frame covers 0.072 blocks and a
// catastrophic 200 ms hitch covers 0.86, so nothing a walking player can do reaches 2.0.
// Without this, spawning at the far side of the world banks thousands of blocks and the
// next step fires instantly.
#define SFX_FOOTSTEP_MAX_STEP  2.0f

// Quieter than a break or a place, which are at 1.0. A footstep is the most frequent sound
// in the game by a wide margin and it is played at zero distance from the listener (the
// listener IS the walker), so it takes the full un-attenuated gain every time; at 1.0 it
// would sit on top of everything else the player is trying to hear.
#define SFX_FOOTSTEP_GAIN  0.5f

typedef struct {
	bool  has_prev;        // false until the first update seeds prev_x/prev_z
	float prev_x, prev_z;  // where the body was on the previous update
	float banked;          // horizontal blocks accumulated toward the next step
} AudioFootsteps;

void audioFootstepsReset(AudioFootsteps* f);

// One frame of walking. `x`, `y`, `z` is the body's position with y at the FEET (the
// convention world/physics.h's Body uses), and `on_ground` is Body::on_ground.
//
// Returns true on the frame a footstep was played, which is what the host suite counts.
// At most one step per call: the per-call distance is capped by SFX_FOOTSTEP_MAX_STEP, so
// the accumulator can never hold two strides' worth after one subtraction.
//
// While airborne the position is still recorded but nothing is banked, so a jump does not
// contribute its arc to the next step and landing does not discharge a fall as footsteps.
// The banked distance SURVIVES the jump rather than being cleared: a player who hops
// mid-walk is still mid-stride when they land.
bool audioFootstepsUpdate(AudioFootsteps* f, float x, float y, float z, bool on_ground);
