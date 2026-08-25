// Host self-test for app/remap.c. Own main(), same pattern as options_test.c.

#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/options.h"
#include "app/remap.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

#define TEST_DIR "build-host/remaptest"

static const char* const s_test_paths[] = {
	TEST_DIR "/roundtrip.ini",
	TEST_DIR "/roundtrip.ini.tmp",
};

static void testCleanup(void)
{
	for (size_t i = 0; i < sizeof(s_test_paths) / sizeof(s_test_paths[0]); i++)
		remove(s_test_paths[i]);
}

// ── A saved, NON-default binding table ────────────────────────────────────────
//
// Every test below used to build its Options with optionsDefaults() and nothing
// else, which meant remapInit's "copy the caller's bindings" branch and a branch
// that ignored the caller entirely produced byte-identical state — the suite could
// not tell them apart, so a player's saved bindings could have been loading as
// defaults with every check still green. This table is the fix: every one of the
// seven entries differs from the shipped default, so any code path that quietly
// substitutes defaults shows up as a failed compare rather than as nothing.
//
// Defaults, for reference: DUP DDOWN DLEFT DRIGHT A X Y.
static const uint32_t s_saved_bindings[ACTION_COUNT] = {
	OPT_KEY_DDOWN,   // ACTION_MOVE_FORWARD  (default DUP)
	OPT_KEY_DUP,     // ACTION_MOVE_BACK     (default DDOWN)
	OPT_KEY_DRIGHT,  // ACTION_MOVE_LEFT     (default DLEFT)
	OPT_KEY_DLEFT,   // ACTION_MOVE_RIGHT    (default DRIGHT)
	OPT_KEY_Y,       // ACTION_JUMP          (default A)
	OPT_KEY_A,       // ACTION_BREAK         (default X)
	OPT_KEY_X,       // ACTION_PLACE         (default Y)
};

// Fills `o` with the defaults and then overlays the saved table above, the way
// optionsLoad leaves an Options read off a card the player has remapped on.
static void testSavedOptions(Options* o)
{
	optionsDefaults(o);
	for (int i = 0; i < ACTION_COUNT; i++)
		o->bindings[i] = s_saved_bindings[i];
}

// CONTROL. Deliberately fed the plain defaults, and deliberately kept that way:
// this is the check that must stay green whether remapInit honours the caller or
// ignores it, so a red run elsewhere can be read as "the saved-bindings path
// broke" rather than "the binary is broken". Do not convert it to
// testSavedOptions() — its whole value is that it cannot distinguish the two.
static void testControlDefaultsMatch(void)
{
	Options def;
	optionsDefaults(&def);

	RemapState rs;
	remapInit(&rs, &def);

	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapGetBinding(&rs, i) == def.bindings[i]);
}

// The check the CONTROL above cannot make: a player's saved, non-default bindings
// must arrive in the remap screen intact. Goes red the moment remapInit stops
// reading `o`.
static void testInitCopiesSavedBindings(void)
{
	Options saved;
	testSavedOptions(&saved);

	Options def;
	optionsDefaults(&def);

	RemapState rs;
	remapInit(&rs, &saved);

	for (int i = 0; i < remapActionCount(); i++) {
		CHECK(remapGetBinding(&rs, i) == saved.bindings[i]);
		// Guards the check above against passing by luck: if this ever trips, the
		// saved table has drifted back onto a default and that slot proves nothing.
		CHECK(saved.bindings[i] != def.bindings[i]);
	}
}

static void testActionNames(void)
{
	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapActionName(i) != NULL);
	CHECK(remapActionName(-1) == NULL);
	CHECK(remapActionName(remapActionCount()) == NULL);
}

static void testKeyNames(void)
{
	CHECK(strcmp(remapKeyName(OPT_KEY_A), "A") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_X), "X") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_Y), "Y") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_DRIGHT), "D-right") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_DLEFT), "D-left") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_DUP), "D-up") == 0);
	CHECK(strcmp(remapKeyName(OPT_KEY_DDOWN), "D-down") == 0);
	CHECK(strcmp(remapKeyName(0xDEADBEEF), "???") == 0);
}

// Fed the saved table, not the defaults: with defaults, A/X/Y happen to sit on
// JUMP/BREAK/PLACE, so a lookup that ignored the state entirely and returned the
// default owner would have looked correct. Here the saved table moves all three,
// so only a lookup that really reads rs->bindings answers right.
static void testFindAction(void)
{
	Options saved;
	testSavedOptions(&saved);
	RemapState rs;
	remapInit(&rs, &saved);

	CHECK(remapFindAction(&rs, OPT_KEY_Y) == ACTION_JUMP);
	CHECK(remapFindAction(&rs, OPT_KEY_A) == ACTION_BREAK);
	CHECK(remapFindAction(&rs, OPT_KEY_X) == ACTION_PLACE);
	CHECK(remapFindAction(&rs, OPT_KEY_DDOWN) == ACTION_MOVE_FORWARD);
	CHECK(remapFindAction(&rs, OPT_KEY_DUP) == ACTION_MOVE_BACK);
	CHECK(remapFindAction(&rs, 0) == ACTION_COUNT);
}

// JUMP holds Y and BREAK holds A in the saved table, so capturing A onto JUMP is
// a real conflict and must swap the two.
static void testCaptureSwap(void)
{
	Options saved;
	testSavedOptions(&saved);
	RemapState rs;
	remapInit(&rs, &saved);

	CHECK(!remapIsCapturing(&rs));

	remapStartCapture(&rs, ACTION_JUMP);
	CHECK(remapIsCapturing(&rs));
	CHECK(remapCaptureAction(&rs) == ACTION_JUMP);

	const bool swapped = remapApplyCapture(&rs, OPT_KEY_A);
	CHECK(swapped);
	CHECK(!remapIsCapturing(&rs));
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_A);
	CHECK(remapGetBinding(&rs, ACTION_BREAK) == OPT_KEY_Y);
}

static void testCaptureSameKey(void)
{
	Options saved;
	testSavedOptions(&saved);
	RemapState rs;
	remapInit(&rs, &saved);

	remapStartCapture(&rs, ACTION_JUMP);
	const bool swapped = remapApplyCapture(&rs, OPT_KEY_Y);
	CHECK(!swapped);
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_Y);
	CHECK(remapGetBinding(&rs, ACTION_BREAK) == OPT_KEY_A);
}

static void testCaptureZeroCancels(void)
{
	Options saved;
	testSavedOptions(&saved);
	RemapState rs;
	remapInit(&rs, &saved);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, 0);
	CHECK(!remapIsCapturing(&rs));
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_Y);
}

static void testResetToDefaults(void)
{
	Options def;
	optionsDefaults(&def);

	// Started from the saved table rather than from the defaults: when the starting
	// state already IS the defaults, remapReset doing nothing at all passes this
	// test. From a fully non-default table it has to do the whole job.
	Options saved;
	testSavedOptions(&saved);
	RemapState rs;
	remapInit(&rs, &saved);

	// Proves the pre-reset state really was non-default, so the compares after the
	// reset are measuring a change rather than a table that never moved.
	CHECK(remapGetBinding(&rs, ACTION_JUMP) != def.bindings[ACTION_JUMP]);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, OPT_KEY_X);
	remapStartCapture(&rs, ACTION_BREAK);
	remapApplyCapture(&rs, OPT_KEY_DUP);

	remapReset(&rs);

	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapGetBinding(&rs, i) == def.bindings[i]);
}

static void testRoundTrip(void)
{
	const char* path = TEST_DIR "/roundtrip.ini";

	// Starts from an already-remapped table rather than the defaults, so what goes
	// to disk is a table no default-producing code path could have generated.
	Options out;
	testSavedOptions(&out);
	{
		RemapState rs;
		remapInit(&rs, &out);
		remapStartCapture(&rs, ACTION_JUMP);
		remapApplyCapture(&rs, OPT_KEY_X);      // X sits on PLACE here -> swap
		remapStartCapture(&rs, ACTION_BREAK);
		remapApplyCapture(&rs, OPT_KEY_DUP);    // DUP sits on MOVE_BACK here -> swap
		remapApply(&rs, &out);
	}

	CHECK(out.bindings[ACTION_JUMP]  == OPT_KEY_X);
	CHECK(out.bindings[ACTION_PLACE] == OPT_KEY_Y);
	CHECK(out.bindings[ACTION_BREAK] == OPT_KEY_DUP);
	CHECK(out.bindings[ACTION_MOVE_BACK] == OPT_KEY_A);

	CHECK(optionsSave(&out, path));

	Options in;
	int bad = -1;
	CHECK(optionsLoad(&in, path, &bad));
	CHECK(bad == 0);

	for (int i = 0; i < ACTION_COUNT; i++)
		CHECK(in.bindings[i] == out.bindings[i]);

	// The compare above would also pass if both sides had collapsed to the
	// defaults, which is the exact failure this file was blind to. This says the
	// table that survived the round trip is the remapped one.
	Options def;
	optionsDefaults(&def);
	int differs = 0;
	for (int i = 0; i < ACTION_COUNT; i++)
		if (in.bindings[i] != def.bindings[i]) differs++;
	CHECK(differs >= 4);
}

static void testInitNullOptions(void)
{
	RemapState rs;
	remapInit(&rs, NULL);

	Options def;
	optionsDefaults(&def);
	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapGetBinding(&rs, i) == def.bindings[i]);
}

static void testApplyWritesOptions(void)
{
	Options out;
	testSavedOptions(&out);
	RemapState rs;
	remapInit(&rs, &out);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, OPT_KEY_X);   // X sits on PLACE in the saved table
	remapApply(&rs, &out);

	CHECK(out.bindings[ACTION_JUMP]  == OPT_KEY_X);
	CHECK(out.bindings[ACTION_PLACE] == OPT_KEY_Y);
	// The four actions the capture never touched must come back unchanged from the
	// saved table, not reset to their defaults.
	CHECK(out.bindings[ACTION_MOVE_FORWARD] == OPT_KEY_DDOWN);
	CHECK(out.bindings[ACTION_MOVE_BACK]    == OPT_KEY_DUP);
	CHECK(out.bindings[ACTION_MOVE_LEFT]    == OPT_KEY_DRIGHT);
	CHECK(out.bindings[ACTION_BREAK]        == OPT_KEY_A);
}

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testControlDefaultsMatch();
	testInitCopiesSavedBindings();
	testActionNames();
	testKeyNames();
	testFindAction();
	testCaptureSwap();
	testCaptureSameKey();
	testCaptureZeroCancels();
	testResetToDefaults();
	testRoundTrip();
	testInitNullOptions();
	testApplyWritesOptions();

	testCleanup();

	if (s_fails == 0)
		printf("remap self-test: PASS  %d checks\n", s_checks);
	else
		printf("remap self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int remap_test_host_only_t;

#endif
