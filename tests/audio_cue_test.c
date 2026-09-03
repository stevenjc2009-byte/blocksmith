// Host self-test for the game's sound CUES — the wiring that decides a sound happens at all.
//
// ── The defect this file was written for ────────────────────────────────────────────────
//
// v1.8.9 shipped a complete audio subsystem with two host suites of its own, and the game
// made no sound whatsoever, because nothing in gameplay ever called it. Measured before the
// fix, over the whole tree:
//
//     grep -rn "audioPlay" source/ --include=*.c --include=*.h | grep -v "^source/audio/"
//     source/main.c:3845:  // ... so every later audioPlay*             <- a comment
//     source/main.c:4881:  // ... ahead of any positional audioPlayAt   <- a comment
//
// Two comments and zero calls. Every existing audio test passed, because every one of them
// asks "if a sound is played, is it played correctly" and none of them could ask "does
// anything play one". That is the gap this binary closes, and it is why the checks below
// count BACKEND CALLS rather than inspecting state: the only claim worth making here is that
// a real break, driven through the real scene/interact.c, reaches real hardware.
//
// ── What is real and what is faked ──────────────────────────────────────────────────────
//
// Real: scene/interact.c, audio/audio.c, audio/audio_mixer.c, audio/audio_pan.c,
// audio/audio_bsnd.c, audio/audio_sfx.c, and the three .bsnd files that ship in romfs/sfx/.
// A break here is a genuine held break against a genuine World, and the sound it makes is
// carried by the genuine mixer.
//
// Faked: the DSP, through the AudioBackend seam (audio/audio_backend.h) — the same seam
// source/audio/audio_mixer_test.c uses, a recorder that remembers every chn_play. Stubbed in
// the LINK and never inside a module under test: tests/interact_stub.c for chunkRenderTouch
// (citro3d), tests/net_stub.c for networldOnColumnLoad, and networldSendBlockEdit /
// networldSessionActive below, copied from source/scene/interact_test.c for the same reason
// it has them.
//
// ── What this CANNOT prove ──────────────────────────────────────────────────────────────
//
// That a noise comes out of the speakers, that it is the right noise, or that it is at a
// comfortable volume. Those need a console and an ear; nothing on a PC can stand in for
// them. What is provable here is that the call happens, exactly once, for the right sound,
// at the right place — which is precisely the part that was missing.
//
// This file lives in tests/ rather than beside its module. source/audio and source/scene are
// both globbed into the console build by the Makefile's SOURCES, so a _test.c there needs an
// #ifndef __3DS__ around its main(); tests/ is in no SOURCES directory and needs no guard.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/hw.h"
#include "app/input_map.h"
#include "audio/audio.h"
#include "audio/audio_backend.h"
#include "audio/audio_bsnd.h"
#include "audio/audio_pan.h"
#include "audio/audio_sfx.h"
#include "scene/interact.h"
#include "world/block.h"
#include "world/mining.h"
#include "world/registry.h"
#include "world/world.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                               \
		if (!(cond)) {                                                            \
			s_fails++;                                                            \
			printf("  FAIL  L%d %s\n", __LINE__, #cond);                          \
			if (!s_first[0])                                                      \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                         \
	} while (0)

// ── the fake DSP ────────────────────────────────────────────────────────────────────────
//
// It records rather than decides, so it cannot make a failing case pass. frame_count is
// recorded because it is the only field of AudioSample that differs between the three
// shipped sounds, and "did the BREAK sound play, or the place one" is a question this file
// has to be able to answer — an id mix-up is exactly the failure mode audio_sfx.h's captured
// ids exist to prevent, and a test that only counted plays could not see it.

#define FAKE_MAX_PLAYS 256

typedef struct {
	int      chn;
	float    left, right;
	bool     looping;
	uint32_t frame_count;
	uint32_t sample_rate;
} FakePlay;

typedef struct {
	FakePlay play[FAKE_MAX_PLAYS];
	int      plays;
	bool     playing[AUDIO_VOICE_COUNT];
} Fake;

static Fake s_fake;

static bool fakeInit(void* ud)     { (void)ud; return true; }
static void fakeShutdown(void* ud) { (void)ud; }
static void fakeMaster(void* ud, float v) { (void)ud; (void)v; }

static void fakePlayFn(void* ud, int chn, const AudioSample* s,
                        float l, float r, bool loop)
{
	(void)ud;
	if (chn >= 0 && chn < AUDIO_VOICE_COUNT) s_fake.playing[chn] = true;
	if (s_fake.plays >= FAKE_MAX_PLAYS) return;
	FakePlay* p = &s_fake.play[s_fake.plays++];
	p->chn         = chn;
	p->left        = l;
	p->right       = r;
	p->looping     = loop;
	p->frame_count = s ? s->frame_count : 0;
	p->sample_rate = s ? s->sample_rate : 0;
}

static void fakeStopFn(void* ud, int chn)
{
	(void)ud;
	if (chn >= 0 && chn < AUDIO_VOICE_COUNT) s_fake.playing[chn] = false;
}

static bool fakeIsPlaying(void* ud, int chn)
{
	(void)ud;
	return (chn >= 0 && chn < AUDIO_VOICE_COUNT) ? s_fake.playing[chn] : false;
}

static void fakeSetMix(void* ud, int chn, float l, float r)
{
	(void)ud; (void)chn; (void)l; (void)r;
}

static const AudioBackend s_fake_backend = {
	fakeInit, fakeShutdown, fakeMaster, fakePlayFn, fakeStopFn, fakeIsPlaying,
	fakeSetMix, NULL,
};

// Clears the recording but NOT the loaded sounds: the ids are registered once in main and
// every case shares them, exactly as the console does (audio.h: sounds are loaded at boot
// and live for the process).
static void fakeClearPlays(void)
{
	memset(s_fake.play, 0, sizeof s_fake.play);
	s_fake.plays = 0;
}

// One frame's worth of "the previous frame's sounds have finished". Every sound in this game
// is well under a frame's worth of samples at 60 fps... which is not the point: the point is
// that with voices never retired, the ninth play in a case would be a STEAL rather than a
// fresh allocation and the counts below would start describing the mixer's eviction policy
// instead of the cue wiring. audio_mixer_test.c is where stealing is tested. Here every play
// gets a free voice, deliberately.
static void frameTick(void)
{
	for (int i = 0; i < AUDIO_VOICE_COUNT; i++) s_fake.playing[i] = false;
	audioUpdate();
}

// ── the wire spy, copied from source/scene/interact_test.c ───────────────────────────────
//
// net/networld.c is not in this binary's link (it needs the transport, which is not host
// portable — see tests/net_stub.c). interact.c's break and place both consult it before the
// edit may stand, so without these two the binary does not link and the break path cannot be
// driven at all. Both are knobs, not decisions: "the send worked, there is no server", which
// is the single-player answer, and no case here changes them.
static int s_edits_sent;

bool networldSendBlockEdit(int x, int y, int z, uint8_t block);
bool networldSessionActive(void);

bool networldSendBlockEdit(int x, int y, int z, uint8_t block)
{
	(void)x; (void)y; (void)z; (void)block;
	s_edits_sent++;
	return true;
}

bool networldSessionActive(void) { return false; }

// ── the sounds ──────────────────────────────────────────────────────────────────────────

#define SFX_DIR_BREAK  "romfs/sfx/block_break.bsnd"
#define SFX_DIR_PLACE  "romfs/sfx/block_place.bsnd"
#define SFX_DIR_STEP   "romfs/sfx/footstep.bsnd"

// The frame_count field straight out of a .bsnd header (offset 12, u32 little-endian — see
// audio/audio_bsnd.h's layout table). Read from the FILE rather than written down here, so
// that regenerating the sounds with tools/make_sounds.py cannot turn a correct test red, and
// so nothing in this file has to know how long any sound is.
static uint32_t fileFrameCount(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	uint8_t h[BSND_HEADER_BYTES];
	const size_t n = fread(h, 1, sizeof h, f);
	fclose(f);
	if (n != sizeof h) return 0;
	return (uint32_t)h[12] | ((uint32_t)h[13] << 8) |
	       ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
}

static uint32_t s_fc_break, s_fc_place, s_fc_step;

// Loads the three shipped sounds and records what audioLoad actually returned, which is what
// source/main.c now does. Called again by the cases that need a clean slot table.
//
// The paths are the SFX_DIR_* ones above — "romfs/sfx/..." with no colon — where main.c passes
// "romfs:/sfx/...". That is not a second copy of the filenames drifting from the real ones: it
// is the one difference audio.c's header comment calls out, that the same fopen reaches
// "romfs:/..." through libctru's devoptab on the console and a plain relative path on a PC.
// source/audio/audio_mixer_test.c loads the same three files the same way. This script cds to
// the repo root, so the relative path resolves.
static void registerTheShippedSounds(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_BREAK, audioLoad(SFX_DIR_BREAK));
	audioSfxRegister(SFX_BLOCK_PLACE, audioLoad(SFX_DIR_PLACE));
	audioSfxRegister(SFX_FOOTSTEP,    audioLoad(SFX_DIR_STEP));
}

// ── the world fixture, the same shape source/scene/interact_test.c uses ──────────────────

#define TX 5
#define TY 40
#define TZ 6

static World s_world;

static void freshAimedAt(Interact* it, BlockId target_block)
{
	registryInitCore();
	worldExit(&s_world);
	worldInit(&s_world);
	s_edits_sent = 0;
	interactSetRelightQueue(NULL);

	if (target_block != BLOCK_AIR)
		worldSet(&s_world, TX, TY, TZ, target_block);

	interactInit(it);
	it->target.hit  = true;
	it->target.x    = TX;
	it->target.y    = TY;
	it->target.z    = TZ;
	it->target.face = FACE_TOP;
	it->target.px   = TX;
	it->target.py   = TY + 1;
	it->target.pz   = TZ;
}

static Body farAwayBody(void)
{
	Body b;
	memset(&b, 0, sizeof b);
	b.x = TX + 8.0f;
	b.y = 4.0f;
	b.z = TZ + 8.0f;
	return b;
}

static u32 breakKey(void) { return inputKey(ACTION_BREAK); }
static u32 placeKey(void) { return inputKey(ACTION_PLACE); }

// ── 1. the slot table ───────────────────────────────────────────────────────────────────

static void testTheThreeShippedSoundsLoadAndGetDistinctIds(void)
{
	registerTheShippedSounds();

	const AudioSoundId b = audioSfxId(SFX_BLOCK_BREAK);
	const AudioSoundId p = audioSfxId(SFX_BLOCK_PLACE);
	const AudioSoundId f = audioSfxId(SFX_FOOTSTEP);

	// Not decoration. If romfs/sfx/ ever stops shipping a sound, or a .bsnd goes malformed,
	// every play-count check below would still pass while the game made no noise — the same
	// shape of pass-by-vacuum this whole binary exists to close.
	CHECK(b != AUDIO_SOUND_NONE);
	CHECK(p != AUDIO_SOUND_NONE);
	CHECK(f != AUDIO_SOUND_NONE);
	CHECK(b != p);
	CHECK(b != f);
	CHECK(p != f);

	// The frame counts this file uses to tell the sounds apart have to actually differ, or
	// the "was it the break sound or the place sound" checks prove nothing.
	CHECK(s_fc_break != 0);
	CHECK(s_fc_place != 0);
	CHECK(s_fc_step  != 0);
	CHECK(s_fc_break != s_fc_place);
	CHECK(s_fc_break != s_fc_step);
	CHECK(s_fc_place != s_fc_step);
}

static void testAnUnregisteredSlotIsSilentRatherThanWrong(void)
{
	// The exact shape of the bug audio_sfx.h's captured ids prevent: block_break.bsnd fails
	// to load, which consumes no id, so under the old fixed-id scheme block_place.bsnd became
	// id 1 and every break played the place sound. Here the failed load registers
	// AUDIO_SOUND_NONE and the OTHER two slots keep the ids they were really given.
	audioSfxReset();
	CHECK(audioSfxId(SFX_BLOCK_BREAK) == AUDIO_SOUND_NONE);
	CHECK(audioSfxId(SFX_BLOCK_PLACE) == AUDIO_SOUND_NONE);
	CHECK(audioSfxId(SFX_FOOTSTEP)    == AUDIO_SOUND_NONE);

	const AudioSoundId place_id = audioLoad(SFX_DIR_PLACE);
	audioSfxRegister(SFX_BLOCK_BREAK, AUDIO_SOUND_NONE);   // the load that failed
	audioSfxRegister(SFX_BLOCK_PLACE, place_id);

	CHECK(audioSfxId(SFX_BLOCK_BREAK) == AUDIO_SOUND_NONE);
	CHECK(audioSfxId(SFX_BLOCK_PLACE) == place_id);

	fakeClearPlays();
	CHECK(audioSfxPlayAtBlock(SFX_BLOCK_BREAK, AUDIO_PRIO_NORMAL, 1.0f, 0, 0, 0)
	      == AUDIO_VOICE_NONE);
	CHECK(s_fake.plays == 0);

	// ...and the slot that DID load is unaffected, so this is "one sound missing", not
	// "audio off".
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);
	CHECK(audioSfxPlayAtBlock(SFX_BLOCK_PLACE, AUDIO_PRIO_NORMAL, 1.0f, 0, 0, 0)
	      != AUDIO_VOICE_NONE);
	CHECK(s_fake.plays == 1);
	CHECK(s_fake.play[0].frame_count == s_fc_place);

	registerTheShippedSounds();
	frameTick();
}

// ── 2. a block break makes exactly one sound, and it is the break sound ──────────────────

static void testABreakPlaysExactlyOneBreakSound(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	fakeClearPlays();

	// The listener sits four blocks away on +Z from the CENTRE of the target cell, facing
	// yaw 0 (down -Z), so its right vector is exactly +X. A sound at the cell centre is then
	// dead ahead and must come out with equal gain on both sides; a sound at the cell's
	// integer CORNER is half a block to the left and cannot. That is the whole positional
	// check, and it is why this listener is placed where it is.
	audioSetListener((float)TX + 0.5f, (float)TY + 0.5f, (float)TZ + 4.5f, 0.0f);

	const uint32_t need = breakTicksRequired(BLOCK_STONE, it.holding);
	CHECK(need > 1);   // a block that broke instantly could not show the "not yet" case

	// Every frame of the hold before the last one is silent. This is the check that a break
	// sound cannot be wired to the HOLD (interactBreakStage is a state, true for the whole
	// press) rather than to its completion.
	int silent_frames = 0;
	for (uint32_t t = 0; t + 1 < need; t++) {
		interactEdit(&it, &s_world, &body, breakKey(), breakKey(), 1);
		if (s_fake.plays == 0) silent_frames++;
		frameTick();
	}
	CHECK(silent_frames == (int)need - 1);
	CHECK(s_fake.plays == 0);
	CHECK(it.broke == 0);

	// The frame it lands.
	interactEdit(&it, &s_world, &body, breakKey(), breakKey(), 1);
	CHECK(it.broke == 1);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR);
	// The wire spy, for the same reason source/scene/interact_test.c reads it: the sound sits
	// BELOW sendEditOrRevert in scene/interact.c, so "one sound" is only the right answer if
	// the edit really was accepted rather than rolled back.
	CHECK(s_edits_sent == 1);
	CHECK(s_fake.plays == 1);
	CHECK(s_fake.play[0].frame_count == s_fc_break);
	CHECK(s_fake.play[0].looping == false);

	// Dead centre: equal on both sides. Sabotaging the +0.5f block-centre offset in
	// audioSfxPlayAtBlock puts the sound at the corner and this goes red.
	CHECK(fabsf(s_fake.play[0].left - s_fake.play[0].right) < 1e-6f);

	// ...and at the gain a source four blocks away earns, which pins the distance as well as
	// the pan. 4.0 blocks on the linear rolloff is (24-4)/(24-2) = 0.909091, and constant
	// power puts sqrt(0.5) of that on each side: 0.642824. The corner would be 4.5552 blocks
	// away and land at 0.658, which this tolerance separates.
	const float want = (20.0f / 22.0f) * sqrtf(0.5f);
	CHECK(fabsf(s_fake.play[0].left - want) < 0.002f);

	frameTick();

	// Keeping the button down after the block is gone plays nothing more: the target is now
	// air, breakProgress never restarts, and there is no second sound. This is the check that
	// the cue is not per-frame.
	for (int i = 0; i < 120; i++) {
		interactEdit(&it, &s_world, &body, breakKey(), breakKey(), 1);
		frameTick();
	}
	CHECK(s_fake.plays == 1);
	CHECK(it.broke == 1);
}

static void testABreakThatNeverCompletesIsSilent(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	fakeClearPlays();
	audioSetListener((float)TX, (float)TY, (float)TZ, 0.0f);

	// Held, released, held, released — never long enough. Progress does not survive letting
	// go (scene/interact.c), so this can run forever without a break.
	for (int i = 0; i < 200; i++) {
		interactEdit(&it, &s_world, &body, breakKey(), breakKey(), 1);
		frameTick();
		interactEdit(&it, &s_world, &body, 0, 0, 1);
		frameTick();
	}
	CHECK(it.broke == 0);
	CHECK(worldGet(&s_world, TX, TY, TZ) == BLOCK_STONE);
	CHECK(s_fake.plays == 0);
}

static void testAPressAimedAtNothingIsSilent(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	it.target.hit = false;
	fakeClearPlays();
	audioSetListener((float)TX, (float)TY, (float)TZ, 0.0f);

	for (int i = 0; i < 60; i++) {
		interactEdit(&it, &s_world, &body,
		              breakKey() | placeKey(), breakKey() | placeKey(), 1);
		frameTick();
	}
	CHECK(it.broke == 0);
	CHECK(it.placed == 0);
	CHECK(it.refused > 0);
	CHECK(s_fake.plays == 0);
}

// ── 3. a placement makes exactly one sound, and it is the place sound ────────────────────

static void testAPlacePlaysExactlyOnePlaceSound(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_STONE;
	fakeClearPlays();

	// Same geometry as the break case, but around the PLACE cell (one above the target), so
	// the block-centre assertion covers the place call site independently. It is a separate
	// audioSfxPlayAtBlock call with its own coordinates and could be wrong on its own.
	audioSetListener((float)TX + 0.5f, (float)TY + 1.5f, (float)TZ + 4.5f, 0.0f);

	// The place key held DOWN for sixty frames, not tapped. interactEdit edge-detects
	// internally, so this must still be one block and one sound; a cue driven off the key
	// rather than off the accepted write would make sixty.
	for (int i = 0; i < 60; i++) {
		interactEdit(&it, &s_world, &body, placeKey(), placeKey(), 1);
		frameTick();
	}

	CHECK(it.placed == 1);
	CHECK(worldGet(&s_world, TX, TY + 1, TZ) == BLOCK_STONE);
	CHECK(s_fake.plays == 1);
	CHECK(s_fake.play[0].frame_count == s_fc_place);
	CHECK(fabsf(s_fake.play[0].left - s_fake.play[0].right) < 1e-6f);
	const float want = (20.0f / 22.0f) * sqrtf(0.5f);
	CHECK(fabsf(s_fake.play[0].left - want) < 0.002f);
}

static void testAPlaceWithAnEmptyHandIsSilent(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_AIR;          // an empty hotbar slot: interactEdit refuses it
	fakeClearPlays();
	audioSetListener((float)TX, (float)TY, (float)TZ, 0.0f);

	for (int i = 0; i < 30; i++) {
		interactEdit(&it, &s_world, &body, placeKey(), placeKey(), 1);
		frameTick();
	}
	CHECK(it.placed == 0);
	CHECK(it.refused > 0);
	CHECK(s_fake.plays == 0);
}

static void testAPlaceIntoAnOccupiedCellIsSilent(void)
{
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	it.holding = BLOCK_STONE;
	worldSet(&s_world, TX, TY + 1, TZ, BLOCK_STONE);   // the place cell is already solid
	fakeClearPlays();
	audioSetListener((float)TX, (float)TY, (float)TZ, 0.0f);

	for (int i = 0; i < 30; i++) {
		interactEdit(&it, &s_world, &body, placeKey(), placeKey(), 1);
		frameTick();
	}
	CHECK(it.placed == 0);
	CHECK(it.refused > 0);
	CHECK(s_fake.plays == 0);
}

// ── 4. footsteps ────────────────────────────────────────────────────────────────────────
//
// Walked in small increments against the REAL audioFootstepsUpdate. Every case counts backend
// plays as well as the function's own return, so a state machine that reported a step it never
// played (or the reverse) is red rather than half-green.

// Walks `steps` updates of `dx` blocks each along +X from `x0`, and returns how many times
// the cue reported firing. The first update only SEEDS the previous position and is not part
// of the distance — that is the real shape of the first frame in a world, and counting it
// would make every expected total below off by one step's worth.
//
// 0.125 blocks is the increment every caller uses and it is chosen, not arbitrary: it is
// exactly representable in binary32, so `steps * 0.125` is an exact number of blocks and an
// expectation like "ten blocks is exactly five footsteps" cannot fail on a rounding edge.
// The one caller that needs a REAL walking speed (0.0716667 blocks a frame) uses it well away
// from a stride boundary for the same reason.
static int walkFrom(AudioFootsteps* f, float x0, int steps, float dx, bool on_ground)
{
	int fired = 0;
	audioFootstepsUpdate(f, x0, 0.0f, 0.0f, on_ground);
	for (int i = 0; i < steps; i++) {
		if (audioFootstepsUpdate(f, x0 + (float)(i + 1) * dx, 0.0f, 0.0f, on_ground))
			fired++;
		frameTick();
	}
	return fired;
}

// The listener is parked at the origin and this walk happens right there, so every footstep
// is at distance ~0 and can never be rejected as out of range — a cadence test, not a
// distance test. testAFootstepFarFromTheListenerIsNotPlayed is the one that moves away.
static int walk(AudioFootsteps* f, int steps, float dx, bool on_ground)
{
	return walkFrom(f, 0.0f, steps, dx, on_ground);
}

static void testStandingStillNeverPlaysAFootstep(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	// The listener stands where the player stands, which is the console's arrangement (main.c
	// sets it from the same body every frame) and is load-bearing here: at the origin it would
	// be 42 blocks from this spot, past AUDIO_MAX_DIST, and the play-count check below would be
	// green even for a cue that fired on all six hundred frames.
	audioSetListener(12.0f, 40.0f, 7.0f, 0.0f);

	int fired = 0;
	for (int i = 0; i < 600; i++) {
		if (audioFootstepsUpdate(&f, 12.0f, 40.0f, 7.0f, true)) fired++;
		frameTick();
	}
	CHECK(fired == 0);
	CHECK(s_fake.plays == 0);
}

static void testWalkingOnTheGroundFiresOnTheStrideCadence(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// 80 updates of 0.125 blocks is 10.0 blocks travelled, exactly. At SFX_FOOTSTEP_STRIDE
	// that is exactly five footsteps — not "about five": the accumulator subtracts the stride
	// rather than zeroing, so the remainder carries and the count is exact.
	const int fired = walk(&f, 80, 0.125f, true);
	CHECK(fired == (int)(10.0f / SFX_FOOTSTEP_STRIDE));
	CHECK(fired == 5);
	CHECK(s_fake.plays == 5);
	for (int i = 0; i < s_fake.plays; i++)
		CHECK(s_fake.play[i].frame_count == s_fc_step);

	// Quieter than a break or a place, which play at 1.0. Asserted as TOTAL energy —
	// sqrt(l^2 + r^2) — rather than as a per-side number, because the panning is constant
	// power (audio_pan.h) and the two sides therefore trade against each other as the walker
	// moves past the listener while their sum of squares does not. The first footstep lands at
	// exactly AUDIO_REF_DIST, where the distance rolloff is still 1.0, so the energy is the
	// caller's gain and nothing else.
	const float l0 = s_fake.play[0].left, r0 = s_fake.play[0].right;
	const float energy0 = sqrtf(l0 * l0 + r0 * r0);
	CHECK(fabsf(energy0 - SFX_FOOTSTEP_GAIN) < 1e-4f);
	CHECK(energy0 < 1.0f - 0.05f);
}

static void testWalkingDoesNotFireEveryFrame(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// The real per-frame distance: 4.3 blocks/s at 60 fps is 0.0716667 blocks. Sixty frames
	// is one second and 4.3 blocks — two footsteps, not sixty. This is the check that goes
	// red the instant the cue is moved to fire unconditionally.
	const int fired = walk(&f, 60, 4.3f / 60.0f, true);
	CHECK(fired == 2);
	CHECK(s_fake.plays == 2);
	CHECK(s_fake.plays < 60);
}

static void testAirborneTravelPlaysNothingAndBanksNothing(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// Ten blocks of travel with no ground under the body: five footsteps' worth, and silent.
	CHECK(walk(&f, 80, 0.125f, false) == 0);
	CHECK(s_fake.plays == 0);

	// ...and landing does not discharge the flight. The body is at x = 10.0 when it lands, so
	// a state machine that had banked the airborne travel — or that had simply not been
	// tracking position while off the ground — fires on the very first grounded update.
	fakeClearPlays();
	int fired = 0;
	for (int i = 0; i < 5; i++) {
		if (audioFootstepsUpdate(&f, 10.0f, 0.0f, 0.0f, true)) fired++;
		frameTick();
	}
	CHECK(fired == 0);
	CHECK(s_fake.plays == 0);
}

static void testAJumpDoesNotResetTheStrideInProgress(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// 1.5 blocks on the ground: no step yet, three quarters of a stride banked.
	int fired = 0;
	audioFootstepsUpdate(&f, 0.0f, 0.0f, 0.0f, true);          // seed
	for (int i = 0; i < 12; i++)
		if (audioFootstepsUpdate(&f, (float)(i + 1) * 0.125f, 0.0f, 0.0f, true)) fired++;
	CHECK(fired == 0);

	// A block and a half through the air, banking nothing.
	for (int i = 0; i < 12; i++)
		audioFootstepsUpdate(&f, 1.5f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, false);
	CHECK(s_fake.plays == 0);

	// Half a block back on the ground completes the stride that was in progress before the
	// jump. If landing had cleared the accumulator this would still be silent.
	for (int i = 0; i < 4; i++)
		if (audioFootstepsUpdate(&f, 3.0f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, true))
			fired++;
	CHECK(fired == 1);
	CHECK(s_fake.plays == 1);
}

static void testVerticalMovementAloneIsNotAStride(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// Straight up a ladder that does not exist yet, or an auto-step staircase in place: y
	// changes by fifty blocks, x and z do not move. A stride is horizontal travel.
	int fired = 0;
	for (int i = 0; i < 100; i++) {
		if (audioFootstepsUpdate(&f, 3.0f, (float)i * 0.5f, 9.0f, true)) fired++;
		frameTick();
	}
	CHECK(fired == 0);
	CHECK(s_fake.plays == 0);
}

static void testATeleportIsDiscardedRatherThanBanked(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	CHECK(audioFootstepsUpdate(&f, 0.0f, 0.0f, 0.0f, true) == false);   // seed

	// The listener travels with the player — main.c sets it from the same body every frame —
	// so it arrives with them. It is moved BEFORE the teleport rather than after, and that is
	// the difference between a live check and a decorative one: with the listener left at the
	// origin, the play-count check below would be green even if the teleport banked all two
	// thousand blocks and fired, because audio.c would decline the voice as out of range.
	audioSetListener(2000.0f, 0.0f, 0.0f, 0.0f);

	// A respawn across the world in one update. Two thousand blocks is a thousand strides if
	// it is banked, and exactly nothing if it is recognised for what it is.
	CHECK(audioFootstepsUpdate(&f, 2000.0f, 0.0f, 0.0f, true) == false);
	CHECK(s_fake.plays == 0);

	// The banked distance is cleared with it, so the next stride starts from zero: 1.875
	// blocks of real walking is still not a step...
	int fired = 0;
	for (int i = 0; i < 15; i++)
		if (audioFootstepsUpdate(&f, 2000.0f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, true))
			fired++;
	CHECK(fired == 0);
	CHECK(s_fake.plays == 0);

	// ...and reaching 2.0 is.
	for (int i = 15; i < 16; i++)
		if (audioFootstepsUpdate(&f, 2000.0f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, true))
			fired++;
	CHECK(fired == 1);
	CHECK(s_fake.plays == 1);
}

static void testResetForgetsThePreviousWorld(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);

	// Nearly a full stride banked at x = 1.875.
	audioFootstepsUpdate(&f, 0.0f, 0.0f, 0.0f, true);          // seed
	for (int i = 0; i < 15; i++)
		audioFootstepsUpdate(&f, (float)(i + 1) * 0.125f, 0.0f, 0.0f, true);
	CHECK(s_fake.plays == 0);

	// A new session. The walk CONTINUES from where it was rather than jumping across the
	// world, and that is deliberate: a reset followed by a teleport proves nothing, because
	// SFX_FOOTSTEP_MAX_STEP would discard the distance and clear the accumulator on its own and
	// the check would be green with reset doing nothing at all. Every delta below is an ordinary
	// 0.125-block walking step, so the ONLY thing that can stop the banked 1.875 from finishing
	// a stride is audioFootstepsReset having cleared it.
	audioFootstepsReset(&f);
	CHECK(audioFootstepsUpdate(&f, 1.875f, 0.0f, 0.0f, true) == false);
	CHECK(s_fake.plays == 0);

	// A further 1.875 blocks: a full stride's worth counting from the reset, and two strides'
	// worth counting from the start of the walk. Silent, because the first 1.875 no longer
	// exists.
	int fired = 0;
	for (int i = 0; i < 15; i++)
		if (audioFootstepsUpdate(&f, 1.875f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, true))
			fired++;
	CHECK(fired == 0);
	CHECK(s_fake.plays == 0);

	// ...and the step lands on the stride boundary measured from the reset, not from before it.
	for (int i = 15; i < 16; i++)
		if (audioFootstepsUpdate(&f, 1.875f + (float)(i + 1) * 0.125f, 0.0f, 0.0f, true))
			fired++;
	CHECK(fired == 1);
	CHECK(s_fake.plays == 1);
}

static void testAFootstepFarFromTheListenerIsNotPlayed(void)
{
	AudioFootsteps f;
	audioFootstepsReset(&f);
	fakeClearPlays();

	// The listener is at the origin; the walk happens well past AUDIO_MAX_DIST. The cadence
	// still fires — that is the cue's job — and the mixer correctly declines to take a voice
	// for something inaudible (audio.c's audioOutOfRange gate). Worth pinning because it is
	// what stops a future remote-player footstep from starving the voice table.
	audioSetListener(0.0f, 0.0f, 0.0f, 0.0f);
	const int fired = walkFrom(&f, 500.0f, 80, 0.125f, true);
	CHECK(fired == 5);
	CHECK(s_fake.plays == 0);
}

// ── 5. the silent-console contract still holds for the cues ─────────────────────────────

static void testEveryCueIsSafeWithNoAudioAtAll(void)
{
	// A console with no DSP firmware dumped: audioInit fails and every play is a no-op
	// (audio.h's contract). The cues must be callable anyway — no crash, no divide, and no
	// caller anywhere gated on audioAvailable().
	audioTestReset();
	audioTestForceInitFailure(true);
	CHECK(audioInit() == false);
	CHECK(audioAvailable() == false);
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_BREAK, audioLoad(SFX_DIR_BREAK));
	CHECK(audioSfxId(SFX_BLOCK_BREAK) == AUDIO_SOUND_NONE);   // loading needs a pool
	fakeClearPlays();

	CHECK(audioSfxPlayAtBlock(SFX_BLOCK_BREAK, AUDIO_PRIO_NORMAL, 1.0f, 1, 2, 3)
	      == AUDIO_VOICE_NONE);

	AudioFootsteps f;
	audioFootstepsReset(&f);
	// The cadence still runs — it is not gated on audio being available, and it must not be,
	// or the state would diverge between a console that can play sounds and one that cannot.
	CHECK(walk(&f, 80, 0.125f, true) == 5);
	CHECK(s_fake.plays == 0);

	// A real break with no audio: the edit still lands, nothing plays, nothing crashes.
	Interact it;
	const Body body = farAwayBody();
	freshAimedAt(&it, BLOCK_STONE);
	const uint32_t need = breakTicksRequired(BLOCK_STONE, it.holding);
	for (uint32_t t = 0; t <= need; t++)
		interactEdit(&it, &s_world, &body, breakKey(), breakKey(), 1);
	CHECK(it.broke == 1);
	CHECK(s_fake.plays == 0);

	audioTestForceInitFailure(false);
}

int main(void)
{
	s_fc_break = fileFrameCount(SFX_DIR_BREAK);
	s_fc_place = fileFrameCount(SFX_DIR_PLACE);
	s_fc_step  = fileFrameCount(SFX_DIR_STEP);

	// The Old 3DS pool (393,216 bytes), which is the smaller of the two and therefore the one
	// worth loading the real sounds into: if they fit here they fit everywhere.
	hwTestSetNew3ds(false);
	audioTestReset();
	audioTestSetBackend(&s_fake_backend);
	memset(&s_fake, 0, sizeof s_fake);
	if (!audioInit()) {
		printf("audio cue self-test: FAIL  audioInit() refused the fake backend: %s\n",
		        audioFailureReason());
		return 1;
	}
	registerTheShippedSounds();

	testTheThreeShippedSoundsLoadAndGetDistinctIds();
	testAnUnregisteredSlotIsSilentRatherThanWrong();

	testABreakPlaysExactlyOneBreakSound();
	testABreakThatNeverCompletesIsSilent();
	testAPressAimedAtNothingIsSilent();

	testAPlacePlaysExactlyOnePlaceSound();
	testAPlaceWithAnEmptyHandIsSilent();
	testAPlaceIntoAnOccupiedCellIsSilent();

	testStandingStillNeverPlaysAFootstep();
	testWalkingOnTheGroundFiresOnTheStrideCadence();
	testWalkingDoesNotFireEveryFrame();
	testAirborneTravelPlaysNothingAndBanksNothing();
	testAJumpDoesNotResetTheStrideInProgress();
	testVerticalMovementAloneIsNotAStride();
	testATeleportIsDiscardedRatherThanBanked();
	testResetForgetsThePreviousWorld();
	testAFootstepFarFromTheListenerIsNotPlayed();

	testEveryCueIsSafeWithNoAudioAtAll();

	if (s_fails == 0)
		printf("audio cue self-test: PASS  %d checks\n", s_checks);
	else
		printf("audio cue self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}
