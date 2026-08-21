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

static void testDefaultsMatch(void)
{
	Options def;
	optionsDefaults(&def);

	RemapState rs;
	remapInit(&rs, &def);

	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapGetBinding(&rs, i) == def.bindings[i]);
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

static void testFindAction(void)
{
	Options def;
	optionsDefaults(&def);
	RemapState rs;
	remapInit(&rs, &def);

	CHECK(remapFindAction(&rs, OPT_KEY_A) == ACTION_JUMP);
	CHECK(remapFindAction(&rs, OPT_KEY_X) == ACTION_BREAK);
	CHECK(remapFindAction(&rs, OPT_KEY_Y) == ACTION_PLACE);
	CHECK(remapFindAction(&rs, OPT_KEY_DUP) == ACTION_MOVE_FORWARD);
	CHECK(remapFindAction(&rs, 0) == ACTION_COUNT);
}

static void testCaptureSwap(void)
{
	Options def;
	optionsDefaults(&def);
	RemapState rs;
	remapInit(&rs, &def);

	CHECK(!remapIsCapturing(&rs));

	remapStartCapture(&rs, ACTION_JUMP);
	CHECK(remapIsCapturing(&rs));
	CHECK(remapCaptureAction(&rs) == ACTION_JUMP);

	const bool swapped = remapApplyCapture(&rs, OPT_KEY_X);
	CHECK(swapped);
	CHECK(!remapIsCapturing(&rs));
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_X);
	CHECK(remapGetBinding(&rs, ACTION_BREAK) == OPT_KEY_A);
}

static void testCaptureSameKey(void)
{
	Options def;
	optionsDefaults(&def);
	RemapState rs;
	remapInit(&rs, &def);

	remapStartCapture(&rs, ACTION_JUMP);
	const bool swapped = remapApplyCapture(&rs, OPT_KEY_A);
	CHECK(!swapped);
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_A);
	CHECK(remapGetBinding(&rs, ACTION_BREAK) == OPT_KEY_X);
}

static void testCaptureZeroCancels(void)
{
	Options def;
	optionsDefaults(&def);
	RemapState rs;
	remapInit(&rs, &def);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, 0);
	CHECK(!remapIsCapturing(&rs));
	CHECK(remapGetBinding(&rs, ACTION_JUMP) == OPT_KEY_A);
}

static void testResetToDefaults(void)
{
	Options def;
	optionsDefaults(&def);
	RemapState rs;
	remapInit(&rs, &def);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, OPT_KEY_X);
	remapStartCapture(&rs, ACTION_BREAK);
	remapApplyCapture(&rs, OPT_KEY_Y);

	remapReset(&rs);

	for (int i = 0; i < remapActionCount(); i++)
		CHECK(remapGetBinding(&rs, i) == def.bindings[i]);
}

static void testRoundTrip(void)
{
	const char* path = TEST_DIR "/roundtrip.ini";

	Options out;
	optionsDefaults(&out);
	{
		RemapState rs;
		remapInit(&rs, &out);
		remapStartCapture(&rs, ACTION_JUMP);
		remapApplyCapture(&rs, OPT_KEY_X);
		remapStartCapture(&rs, ACTION_BREAK);
		remapApplyCapture(&rs, OPT_KEY_DUP);
		remapApply(&rs, &out);
	}

	CHECK(optionsSave(&out, path));

	Options in;
	int bad = -1;
	CHECK(optionsLoad(&in, path, &bad));
	CHECK(bad == 0);

	for (int i = 0; i < ACTION_COUNT; i++)
		CHECK(in.bindings[i] == out.bindings[i]);
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
	optionsDefaults(&out);
	RemapState rs;
	remapInit(&rs, &out);

	remapStartCapture(&rs, ACTION_JUMP);
	remapApplyCapture(&rs, OPT_KEY_Y);
	remapApply(&rs, &out);

	CHECK(out.bindings[ACTION_JUMP] == OPT_KEY_Y);
	CHECK(out.bindings[ACTION_PLACE] == OPT_KEY_A);
}

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testDefaultsMatch();
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
