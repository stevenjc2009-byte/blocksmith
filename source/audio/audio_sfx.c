#include "audio/audio_sfx.h"

#include <math.h>

// No <3ds.h> and no allocation, for the same reason audio_mixer.c has none: everything
// here is policy, and the whole point of the slot table and the stride accumulator is that
// the host suite can drive the REAL ones rather than a description of them. The only thing
// below this line that touches hardware is audioPlayAt, and that reaches it through the
// AudioBackend seam a test replaces.

static AudioSoundId s_slots[SFX_SLOT_COUNT];

// One UNSIGNED comparison, not the usual "slot < 0 || slot >= COUNT" pair, and the missing
// half is deleted rather than hidden.
//
// The console builds with -fshort-enums (the Makefile's ARCH flags), so SfxSlot's underlying
// type on hardware is an unsigned char holding 0..3. `slot < 0` is then provably false and
// gcc says so — "comparison is always false due to limited range of data type
// [-Werror=type-limits]" — which under the Makefile's -Werror stops the whole console build.
// It was not a check that could ever fire there, so it is gone.
//
// The cast is not there to quiet the compiler: it makes the remaining check do MORE than the
// pair did. On the host the enum widens to int, and a negative slot passed by mistake
// converts to a very large unsigned and is rejected here — where the old pair would have
// caught it on the host and the console would have indexed off the front of the table.
static bool slotInRange(SfxSlot slot)
{
	return (unsigned)slot < (unsigned)SFX_SLOT_COUNT;
}

void audioSfxRegister(SfxSlot slot, AudioSoundId id)
{
	if (!slotInRange(slot)) return;
	s_slots[slot] = id;
}

AudioSoundId audioSfxId(SfxSlot slot)
{
	if (!slotInRange(slot)) return AUDIO_SOUND_NONE;
	return s_slots[slot];
}

void audioSfxReset(void)
{
	for (int i = 0; i < SFX_SLOT_COUNT; i++)
		s_slots[i] = AUDIO_SOUND_NONE;
}

// ── Per-material resolution (v1.8.19) ───────────────────────────────────────────────
//
// One shared implementation behind the three resolvers audio_sfx.h declares — the four
// arguments after `generic` are literally which slot names each material for THIS event
// (footstep, break or place), and the fallback logic is identical across all three, so it
// is written once here rather than three times with the names changed.
//
// `mat == SFX_MAT_GENERIC` returns `generic` outright rather than falling into the
// unregistered-slot check below: there is no "generic material slot" distinct from the
// pre-1.8.19 slot to register in the first place, so asking audioSfxId() about one would be
// asking about a slot that was never meant to exist.
static SfxSlot materialSlot(SfxMaterial mat, SfxSlot generic,
                             SfxSlot stone, SfxSlot wood, SfxSlot dirt, SfxSlot grass)
{
	SfxSlot chosen;
	switch (mat) {
	case SFX_MAT_STONE: chosen = stone; break;
	case SFX_MAT_WOOD:  chosen = wood;  break;
	case SFX_MAT_DIRT:  chosen = dirt;  break;
	case SFX_MAT_GRASS: chosen = grass; break;
	case SFX_MAT_GENERIC:
	default:
		return generic;
	}

	// The fallback: an unregistered or failed-load slot is AUDIO_SOUND_NONE (this file's
	// own contract, see audioSfxRegister above), and that is the one condition this
	// function exists to catch before audioSfxPlayAtBlock ever sees the slot it returns.
	return (audioSfxId(chosen) == AUDIO_SOUND_NONE) ? generic : chosen;
}

SfxSlot audioSfxFootstepSlot(SfxMaterial mat)
{
	return materialSlot(mat, SFX_FOOTSTEP,
	                     SFX_FOOTSTEP_STONE, SFX_FOOTSTEP_WOOD,
	                     SFX_FOOTSTEP_DIRT, SFX_FOOTSTEP_GRASS);
}

SfxSlot audioSfxBreakSlot(SfxMaterial mat)
{
	return materialSlot(mat, SFX_BLOCK_BREAK,
	                     SFX_BREAK_STONE, SFX_BREAK_WOOD,
	                     SFX_BREAK_DIRT, SFX_BREAK_GRASS);
}

SfxSlot audioSfxPlaceSlot(SfxMaterial mat)
{
	return materialSlot(mat, SFX_BLOCK_PLACE,
	                     SFX_PLACE_STONE, SFX_PLACE_WOOD,
	                     SFX_PLACE_DIRT, SFX_PLACE_GRASS);
}

AudioVoice audioSfxPlayAtBlock(SfxSlot slot, AudioPriority prio, float gain,
                               int bx, int by, int bz)
{
	// An unregistered or failed slot is AUDIO_SOUND_NONE, which audioPlayAt already refuses
	// (audio.c's sampleFor). Returning early anyway keeps the "no sound loaded" case from
	// depending on a detail of another module, and costs one comparison.
	const AudioSoundId id = audioSfxId(slot);
	if (id == AUDIO_SOUND_NONE) return AUDIO_VOICE_NONE;

	return audioPlayAt(id, prio, gain,
	                    (float)bx + 0.5f, (float)by + 0.5f, (float)bz + 0.5f);
}

// ── Footsteps ─────────────────────────────────────────────────────────────────────

void audioFootstepsReset(AudioFootsteps* f)
{
	if (!f) return;
	f->has_prev = false;
	f->prev_x   = 0.0f;
	f->prev_z   = 0.0f;
	f->banked   = 0.0f;
}

bool audioFootstepsUpdate(AudioFootsteps* f, float x, float y, float z, bool on_ground,
                          BlockId under)
{
	if (!f) return false;

	// The previous position is recorded on EVERY path below, including the ones that bank
	// nothing. That is what makes "airborne" cost nothing rather than defer a cost: if the
	// jump's travel were left unmeasured, the first grounded frame after landing would see a
	// delta covering the whole arc and discharge it as a step.
	const float px = f->prev_x, pz = f->prev_z;
	const bool  had_prev = f->has_prev;

	f->prev_x   = x;
	f->prev_z   = z;
	f->has_prev = true;

	(void)y;   // the cue is placed at the feet, but only horizontal travel is a stride

	// Nothing to measure against yet, and nothing to measure while off the ground.
	if (!had_prev || !on_ground) return false;

	const float dx = x - px;
	const float dz = z - pz;
	const float d  = sqrtf(dx * dx + dz * dz);

	// A teleport, not a stride. Thrown away rather than banked, and the accumulator is
	// cleared with it: whatever the player was mid-way through, they are not there now.
	if (!(d <= SFX_FOOTSTEP_MAX_STEP)) {   // written to also reject a NaN delta
		f->banked = 0.0f;
		return false;
	}

	f->banked += d;
	if (f->banked < SFX_FOOTSTEP_STRIDE) return false;

	// Subtracted rather than zeroed, so the remainder carries into the next stride and the
	// cadence does not drift with the frame rate.
	f->banked -= SFX_FOOTSTEP_STRIDE;

	// v1.8.19. `under` is what the caller says this step landed on; sfxMaterialOfBlock()
	// (audio_material.h) buckets it into one of four materials plus the GENERIC fallback,
	// and audioSfxFootstepSlot() (this file) resolves that material to whichever slot
	// should actually play — the material-specific one if its clip was registered, the
	// pre-1.8.19 SFX_FOOTSTEP slot otherwise. Both are pure lookups; nothing here needs to
	// know why either answer came out the way it did.
	const SfxSlot slot = audioSfxFootstepSlot(sfxMaterialOfBlock(under));

	// Positional at the feet, not a non-positional audioPlay. The player's own footstep is
	// at distance zero from the listener, so this is centred at full gain either way today
	// — but a remote player's footsteps, when there are any, are the same cue at a different
	// place, and a cue that is only correct for the local walker would have to be rewritten
	// rather than called. AUDIO_PRIO_LOW is what audio_mixer.h names footsteps as.
	audioPlayAt(audioSfxId(slot), AUDIO_PRIO_LOW, SFX_FOOTSTEP_GAIN, x, y, z);
	return true;
}

// ── Mob damage ────────────────────────────────────────────────────────────────────

AudioVoice audioSfxPlayMobDamage(bool killed, float x, float y, float z)
{
	// The slot choice is the whole policy this function exists to hold. audioSfxId of an
	// unregistered slot is AUDIO_SOUND_NONE and audioPlayAt already refuses that (audio.c's
	// sampleFor) -- nothing here has to special-case a missing clip.
	const SfxSlot slot = killed ? SFX_DEATH : SFX_HURT;
	return audioPlayAt(audioSfxId(slot), AUDIO_PRIO_HIGH, 1.0f, x, y, z);
}
