// Host gate for scene/aimtext.c — the block-name readout under the reticle and its fade
// (v1.9.0 AIM-TEXT).
//
// WHY THIS FILE LINKS THE REAL MODULE. The readout's draw is one fontDraw inside the
// reticle's batch (scene/crosshair.c) and cannot be linked here; everything that can be
// WRONG about it — which name, whether it hides, how it fades, whether it re-formats every
// frame — lives in scene/aimtext.c, which has no <3ds.h> in it precisely so this binary can
// link THAT module and the real world/registry.c behind it, the same carve-out
// tests/ringorder_test.c made for main.c's ring walk. Nothing here is a copy of the code
// under test.
//
// WHAT THIS FILE CANNOT PROVE, stated up front: that main.c calls aimTextUpdate() from the
// frame that produces the RayHit, that the pill lands where crosshair.c's comment says, or
// that the text is legible on a real screen. Those are the console's, and the draw is
// unverified visually until someone looks at it.
//
// Own main(), same CHECK macro and PASS/FAIL line as tests/ringorder_test.c.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scene/aimtext.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			if (!s_fails) snprintf(s_first, sizeof s_first, "L%d %s", __LINE__, #cond); \
			s_fails++;                                                             \
		}                                                                          \
	} while (0)

// Pinned, so a check that quietly stops being reached reads as a FAIL rather than as a
// shorter green run. Move it only when a check is deliberately added or removed, and say so.
#define AIMTEXT_EXPECTED_CHECKS 895

// An id no table in this test ever defines: REG_ID_DYN_HI is the last assignable dyn id and
// this test registers two dyn rows (0x80 and 0x81), so 0xFD stays free; 0xFE/0xFF are reserved
// and can never be assigned at all (registry.h's REG_ID_DYN_HI comment).
static const BlockId kUndefined[] = { 0x80 + 2, 0xFD, 0xFE, 0xFF };

// A clock value for the frames where time does not matter. Nothing in the module reads the
// absolute value, only differences, so any constant is as good as any other.
#define T0 1000u

static bool labelIs(const AimText* t, const char* want)
{
	const char* got = aimTextLabel(t);
	return got != NULL && strcmp(got, want) == 0;
}

static bool hidden(const AimText* t)
{
	return aimTextLabel(t) == NULL && aimTextAlpha(t) == 0;
}

static BlockDef makeDef(const char* name)
{
	BlockDef d;
	memset(&d, 0, sizeof d);
	snprintf(d.name, sizeof d.name, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) d.tex[f] = 1;
	d.flags      = REG_FLAG_SOLID;
	d.hardness   = 1;
	d.variant_of = 0;
	return d;
}

// ── 1. The initial state, both ways it can be reached ─────────────────────────────────
static void testInitialState(void)
{
	AimText t;
	memset(&t, 0xA5, sizeof t);   // garbage in, so init has to do real work
	aimTextInit(&t);
	CHECK(t.state == AIMTEXT_HIDDEN);
	CHECK(t.cached == BLOCK_AIR);
	CHECK(t.formats == 0);
	CHECK(hidden(&t));

	// The header promises all-zero is the same state, which is what lets main.c's static
	// skip an init call at boot. Check it by comparing the bytes, not by re-reading the promise.
	AimText z;
	memset(&z, 0, sizeof z);
	CHECK(memcmp(&z, &t, sizeof t) == 0);
	CHECK(hidden(&z));

	// A hidden readout with nothing cached stays hidden however the clock moves and however
	// many misses it is fed — no fade can start from nothing.
	const uint32_t clocks[] = { 0u, 1u, T0, 0x7FFFFFFFu, 0xFFFFFFFFu, 3u };
	for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++) {
		CHECK(aimTextUpdate(&t, false, BLOCK_AIR, clocks[i]) == false);
		CHECK(hidden(&t));
		CHECK(t.formats == 0);
	}
}

// ── 2. Targeted -> the right name at full alpha; untargeted -> the fade, then gone ─────
static void testShowAndFade(void)
{
	AimText t;
	aimTextInit(&t);

	CHECK(aimTextUpdate(&t, true, BLOCK_STONE, T0) == true);
	CHECK(t.state == AIMTEXT_SHOWN);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(t.formats == 1);

	// The frame the target is lost: the label is STILL there at full opacity. Losing it is
	// the start of the fade, not a blink.
	CHECK(aimTextUpdate(&t, false, BLOCK_STONE, T0 + 10u) == true);
	CHECK(t.state == AIMTEXT_FADING);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 255);

	// Half way: 255 - 255 * 200 / 400 = 255 - 127 = 128. Same text.
	CHECK(aimTextUpdate(&t, false, BLOCK_STONE, T0 + 10u + 200u) == true);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 128);

	// The last millisecond of the ramp is still visible: 255 - 255 * 399 / 400 = 1.
	CHECK(aimTextUpdate(&t, false, BLOCK_STONE, T0 + 10u + AIMTEXT_FADE_MS - 1u) == true);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 1);

	// Exactly the fade time: gone.
	CHECK(aimTextUpdate(&t, false, BLOCK_STONE, T0 + 10u + AIMTEXT_FADE_MS) == false);
	CHECK(t.state == AIMTEXT_HIDDEN);
	CHECK(hidden(&t));

	// And it stays gone; a garbage id on a miss is still just a miss and is never looked at.
	CHECK(aimTextUpdate(&t, false, 0xFF, T0 + 10000u) == false);
	CHECK(hidden(&t));
	CHECK(t.formats == 1);   // nothing about hiding or fading formats

	// Back onto the same block: shows again at 255, and did NOT re-format (the cache survives
	// the fade and the hide).
	CHECK(aimTextUpdate(&t, true, BLOCK_STONE, T0 + 20000u) == true);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(t.formats == 1);

	// A different block: new name, exactly one more format, full alpha.
	CHECK(aimTextUpdate(&t, true, BLOCK_DIRT, T0 + 20001u) == true);
	CHECK(labelIs(&t, "dirt"));
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(t.formats == 2);
}

// ── 3. The ramp, every millisecond: monotone, pinned at both ends, label all the way ──
static void testFadeIsMonotone(void)
{
	AimText t;
	aimTextInit(&t);
	aimTextUpdate(&t, true, BLOCK_GRASS, T0);
	CHECK(aimTextAlpha(&t) == 255);

	// Lose it at T0 + 5 and then walk the clock one millisecond at a time. A frame that runs
	// slow skips values, which is fine; what must hold is that no step ever goes UP and the
	// label is the same string on every frame it is visible at all.
	const uint32_t lost = T0 + 5u;
	uint8_t prev = 255;
	bool monotone = true, same_label = true, ret_matches = true;
	for (uint32_t ms = 0; ms <= AIMTEXT_FADE_MS + 50u; ms++) {
		const bool    vis = aimTextUpdate(&t, false, BLOCK_AIR, lost + ms);
		const uint8_t a   = aimTextAlpha(&t);
		if (a > prev) monotone = false;
		if (vis != (a != 0)) ret_matches = false;
		if (a != 0 && !labelIs(&t, "grass")) same_label = false;
		if (a == 0 && aimTextLabel(&t) != NULL) same_label = false;
		if (ms == 0)                 CHECK(a == 255);
		if (ms == AIMTEXT_FADE_MS-1) CHECK(a >= 1);
		if (ms == AIMTEXT_FADE_MS)   CHECK(a == 0);
		prev = a;
	}
	CHECK(monotone);
	CHECK(same_label);
	CHECK(ret_matches);
	CHECK(t.formats == 1);
}

// ── 4. Retargeting resets the fade — same block or a different one ────────────────────
static void testRetargetResets(void)
{
	AimText t;
	aimTextInit(&t);
	aimTextUpdate(&t, true, BLOCK_STONE, T0);

	// Lose it, get half way down the ramp...
	aimTextUpdate(&t, false, BLOCK_AIR, T0 + 100u);
	aimTextUpdate(&t, false, BLOCK_AIR, T0 + 300u);
	CHECK(aimTextAlpha(&t) == 128);

	// ...and re-acquire the SAME block: back to 255 immediately, no rebuild.
	CHECK(aimTextUpdate(&t, true, BLOCK_STONE, T0 + 301u) == true);
	CHECK(t.state == AIMTEXT_SHOWN);
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(labelIs(&t, "stone"));
	CHECK(t.formats == 1);

	// A fresh loss after that starts a fresh ramp from ITS OWN moment, not from the old one:
	// 200 ms after this loss is 128 again, not "already gone" because the first loss was
	// 500 ms ago.
	aimTextUpdate(&t, false, BLOCK_AIR, T0 + 600u);
	CHECK(aimTextUpdate(&t, false, BLOCK_AIR, T0 + 800u) == true);
	CHECK(aimTextAlpha(&t) == 128);

	// Mid-fade onto a DIFFERENT block: the new name at 255, exactly one more format, and the
	// old name is not shown for even a frame.
	CHECK(aimTextUpdate(&t, true, BLOCK_DIRT, T0 + 801u) == true);
	CHECK(labelIs(&t, "dirt"));
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(t.formats == 2);

	// Hold on dirt while the clock runs far past where the abandoned stone fade would have
	// ended: a live target never fades, whatever the clock says.
	for (uint32_t ms = 0; ms < 5000u; ms += 250u) {
		CHECK(aimTextUpdate(&t, true, BLOCK_DIRT, T0 + 802u + ms) == true);
		CHECK(aimTextAlpha(&t) == 255);
	}
	CHECK(t.formats == 2);
}

// ── 5. Same id two frames (and two hundred) -> no re-format ───────────────────────────
static void testNoReformatOnSameId(void)
{
	AimText t;
	aimTextInit(&t);

	aimTextUpdate(&t, true, BLOCK_GRASS, T0);
	CHECK(t.formats == 1);
	const char* first = aimTextLabel(&t);
	CHECK(first != NULL);

	for (uint32_t frame = 0; frame < 200; frame++) {
		CHECK(aimTextUpdate(&t, true, BLOCK_GRASS, T0 + frame * 16u) == true);
		CHECK(t.formats == 1);
	}
	CHECK(aimTextLabel(&t) == first);   // same storage, not a rebuilt copy
	CHECK(labelIs(&t, "grass"));

	// Flapping on and off the same block — the crosshair grazing an edge — also formats
	// nothing, and because every "off" frame is the first frame of a fade it never dips
	// below 255 either: a graze does not flicker.
	bool never_dipped = true;
	for (uint32_t frame = 0; frame < 50; frame++) {
		aimTextUpdate(&t, false, BLOCK_AIR,   T0 + 4000u + frame * 32u);
		if (aimTextAlpha(&t) != 255) never_dipped = false;
		aimTextUpdate(&t, true,  BLOCK_GRASS, T0 + 4000u + frame * 32u + 16u);
		if (aimTextAlpha(&t) != 255) never_dipped = false;
	}
	CHECK(never_dipped);
	CHECK(t.formats == 1);
	CHECK(labelIs(&t, "grass"));

	// Alternating between two blocks DOES format each time — the cache is one deep on purpose
	// (a second slot is bytes for a case that costs a 16-byte copy once per swing of the aim).
	for (uint32_t frame = 0; frame < 10; frame++) {
		aimTextUpdate(&t, true, BLOCK_STONE, T0 + 9000u + frame * 2u);
		aimTextUpdate(&t, true, BLOCK_GRASS, T0 + 9000u + frame * 2u + 1u);
	}
	CHECK(t.formats == 21);
}

// ── 6. Unknown id -> nothing to show, never a garbage string ──────────────────────────
static void testUnknownIdHides(void)
{
	AimText t;
	aimTextInit(&t);

	// Air, targeted. Cannot happen through the raycast (it stops on solids) but the contract
	// is on this function, not on its caller.
	CHECK(aimTextUpdate(&t, true, BLOCK_AIR, T0) == false);
	CHECK(hidden(&t));
	CHECK(t.formats == 0);

	// Undefined ids. registryGet() answers the AIR row for these, so the trap is a readout
	// that says "air" — check for that string by name, not just for "hidden".
	for (size_t i = 0; i < sizeof kUndefined / sizeof kUndefined[0]; i++) {
		CHECK(!registryIsDefined(kUndefined[i]));
		CHECK(aimTextUpdate(&t, true, kUndefined[i], T0 + (uint32_t)i) == false);
		CHECK(hidden(&t));
		CHECK(!labelIs(&t, "air"));
		CHECK(t.formats == 0);
	}

	// Unknown straight after a valid one is a LOST target, not a new one: the valid name
	// fades out on the ordinary ramp (that is the crosshair sliding off a block onto
	// something the table does not know, and a blink there would be a bug) and is gone
	// after AIMTEXT_FADE_MS. It is never replaced by "air" on the way.
	aimTextUpdate(&t, true, BLOCK_STONE, T0 + 100u);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextUpdate(&t, true, kUndefined[0], T0 + 101u) == true);
	CHECK(labelIs(&t, "stone"));
	CHECK(t.state == AIMTEXT_FADING);
	CHECK(aimTextUpdate(&t, true, kUndefined[1], T0 + 101u + AIMTEXT_FADE_MS) == false);
	CHECK(hidden(&t));
	CHECK(!labelIs(&t, "air"));
	CHECK(t.formats == 1);

	// ...and the valid one after that shows again without a rebuild.
	CHECK(aimTextUpdate(&t, true, BLOCK_STONE, T0 + 2000u) == true);
	CHECK(labelIs(&t, "stone"));
	CHECK(aimTextAlpha(&t) == 255);
	CHECK(t.formats == 1);
}

// ── 7. Every defined row names itself, within the font's and the buffer's limits ──────
static void testEveryCoreRow(int* longest_len, char longest_name[REGISTRY_NAME_MAX])
{
	AimText t;
	aimTextInit(&t);

	const int count = registryCount();
	CHECK(count >= 10);   // registry.h: 10 after InitCore on the original core table; more now

	uint32_t expect_formats = 0;
	*longest_len = 0;
	longest_name[0] = '\0';

	for (int id = 1; id < REGISTRY_MAX; id++) {
		if (!registryIsDefined((BlockId)id)) continue;

		const char* want = registryGet((BlockId)id)->name;
		CHECK(aimTextUpdate(&t, true, (BlockId)id, T0 + (uint32_t)id) == true);
		CHECK(labelIs(&t, want));
		CHECK(aimTextAlpha(&t) == 255);
		expect_formats++;
		CHECK(t.formats == expect_formats);

		// Fits the buffer with its terminator, and fits the draw: at most AIMTEXT_MAX_CHARS
		// glyphs, every one a character gfx/font.c actually has a cell for (32..126) — a byte
		// outside that range draws as a GAP, which would read as a missing letter.
		const size_t len = strlen(aimTextLabel(&t));
		CHECK(len >= 1);
		CHECK(len <= AIMTEXT_MAX_CHARS);
		bool printable = true;
		for (size_t i = 0; i < len; i++) {
			const unsigned char c = (unsigned char)want[i];
			if (c < 32 || c > 126) printable = false;
		}
		CHECK(printable);

		if ((int)len > *longest_len) {
			*longest_len = (int)len;
			snprintf(longest_name, REGISTRY_NAME_MAX, "%s", want);
		}
	}

	// The shipped core table's longest name is the buffer's maximum exactly, which is the
	// case crosshair.c's budget arithmetic is written against. If a longer one ever lands
	// the registry itself refuses it (nameFits), so this is a floor check on the arithmetic
	// staying honest, not a ceiling that could be raised by accident.
	CHECK(*longest_len == AIMTEXT_MAX_CHARS);
	CHECK(strcmp(longest_name, "cooked_porkchop") == 0);
}

// ── 8. A dynamic row at the maximum length, and one with an EMPTY name off the wire ───
static void testDynamicRows(void)
{
	// The longest name the registry will take: REGISTRY_NAME_MAX - 1 characters.
	char longest[REGISTRY_NAME_MAX];
	memset(longest, 'x', sizeof longest);
	longest[AIMTEXT_MAX_CHARS] = '\0';
	CHECK(strlen(longest) == AIMTEXT_MAX_CHARS);

	BlockDef d = makeDef(longest);
	const BlockId dyn = registryRegister(&d);
	CHECK(dyn == REG_ID_DYN_LO);

	AimText t;
	aimTextInit(&t);
	CHECK(aimTextUpdate(&t, true, dyn, T0) == true);
	CHECK(labelIs(&t, longest));
	CHECK(strlen(aimTextLabel(&t)) == AIMTEXT_MAX_CHARS);
	CHECK(t.formats == 1);

	// An EMPTY name. registryRegister() refuses one, but the join path does not: a DEFS
	// record only has to CONTAIN a NUL (registryDefUnpack), so a server can install a row
	// called "". The readout must treat that as nothing to say — a lost target, so the
	// previous label fades and is gone after AIMTEXT_FADE_MS — not as a label of "".
	uint8_t rec[REGISTRY_WIRE_RECORD_BYTES];
	memset(rec, 0, sizeof rec);
	rec[0]  = (uint8_t)(dyn + 1);    // the next free slot, which is what RemoteApply insists on
	rec[23] = REG_FLAG_SOLID;         // flags
	rec[25] = 1;                      // hardness
	const size_t applied = registryRemoteApply((uint8_t)(dyn + 1), rec, 1);
	CHECK(applied == 1);
	const BlockId empty = (BlockId)(dyn + 1);
	CHECK(registryIsDefined(empty));
	CHECK(registryGet(empty)->name[0] == '\0');

	CHECK(aimTextUpdate(&t, true, empty, T0 + 1u) == true);   // the old label, fading
	CHECK(labelIs(&t, longest));
	CHECK(aimTextUpdate(&t, true, empty, T0 + 1u + AIMTEXT_FADE_MS) == false);
	CHECK(hidden(&t));
	CHECK(t.formats == 1);   // nothing was built for it

	// And the good row still works afterwards, from cache.
	CHECK(aimTextUpdate(&t, true, dyn, T0 + 5000u) == true);
	CHECK(labelIs(&t, longest));
	CHECK(t.formats == 1);
}

// ── 9. Init after a session: the cache AND the fade are dropped, not carried ───────────
static void testInitDropsCache(void)
{
	AimText t;
	aimTextInit(&t);
	aimTextUpdate(&t, true, BLOCK_STONE, T0);
	CHECK(t.formats == 1);

	// A rejoin re-inits; the next frame on the same id must rebuild from the (possibly new)
	// table rather than trust a label built against the old one.
	aimTextInit(&t);
	CHECK(t.formats == 0);
	CHECK(hidden(&t));
	aimTextUpdate(&t, true, BLOCK_STONE, T0 + 1u);
	CHECK(t.formats == 1);
	CHECK(labelIs(&t, "stone"));

	// Init in the MIDDLE of a fade: hidden now, not "still fading the old server's name".
	aimTextUpdate(&t, false, BLOCK_AIR, T0 + 2u);
	aimTextUpdate(&t, false, BLOCK_AIR, T0 + 102u);
	CHECK(aimTextAlpha(&t) != 0);
	aimTextInit(&t);
	CHECK(hidden(&t));
	CHECK(aimTextUpdate(&t, false, BLOCK_AIR, T0 + 103u) == false);
	CHECK(hidden(&t));
}

// ── 10. The clock wraps under a fade and the ramp does not notice ─────────────────────
static void testClockWrap(void)
{
	AimText t;
	aimTextInit(&t);

	// Lose the target 100 ms before the 32-bit rollover; check the ramp on the far side of it.
	const uint32_t lost = 0xFFFFFFFFu - 99u;
	aimTextUpdate(&t, true, BLOCK_STONE, lost - 1u);
	CHECK(aimTextUpdate(&t, false, BLOCK_AIR, lost) == true);
	CHECK(aimTextAlpha(&t) == 255);

	// 200 ms after the loss is 100 ms past zero: half way, 128.
	CHECK(aimTextUpdate(&t, false, BLOCK_AIR, 100u) == true);
	CHECK(aimTextAlpha(&t) == 128);
	CHECK(labelIs(&t, "stone"));

	// 400 ms after the loss is 300 ms past zero: gone.
	CHECK(aimTextUpdate(&t, false, BLOCK_AIR, 300u) == false);
	CHECK(hidden(&t));
}

int main(void)
{
	registryInitCore();   // a fresh core table; the dyn rows below land on top of it

	testInitialState();
	testShowAndFade();
	testFadeIsMonotone();
	testRetargetResets();
	testNoReformatOnSameId();
	testUnknownIdHides();

	int  longest_len = 0;
	char longest_name[REGISTRY_NAME_MAX];
	testEveryCoreRow(&longest_len, longest_name);
	testDynamicRows();
	testInitDropsCache();
	testClockWrap();

	// The budget crosshair.c's comment quotes, printed from the table rather than remembered:
	// one quad per printable non-space glyph, plus one for the pill, per eye.
	printf("longest core name: \"%s\" (%d chars) -> %d quads/eye with the pill, "
	       "%d for a stereo frame (cap %d, sprite.c SPRITE_MAX_QUADS); sizeof(AimText) = %u\n",
	       longest_name, longest_len, longest_len + 1, 2 * (longest_len + 1), 1024,
	       (unsigned)sizeof(AimText));

	// The pinned total. A check that is no longer reached would otherwise pass by absence.
	// CHECK increments s_checks BEFORE it tests the condition, so this one counts itself.
	CHECK(s_checks == AIMTEXT_EXPECTED_CHECKS);

	if (s_fails) {
		printf("aimtext self-test: FAIL %d of %d checks, first: %s\n",
		       s_fails, s_checks, s_first);
		return 1;
	}
	printf("aimtext self-test: PASS %d checks\n", s_checks);
	return 0;
}
