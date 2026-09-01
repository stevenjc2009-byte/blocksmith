// Host self-test for app/options.c. Self-contained (its own main(), unlike world_test.c
// which is driven by tests/host_test.c) because this module has nothing to do with the
// world data tools/run_host_tests.sh already compiles — see that script's own comment for
// why the two are built separately.
//
// The CHECK macro and the PASS/FAIL summary line are copied in the same shape
// world/world_test.c uses, so a failure here reads the same way a world-test failure does.
//
// The __3DS__ guard below is load-bearing rather than tidy. The Makefile globs every .c
// under source/app into the console build, so without it this file's main() links against
// source/main.c's and the build dies with "multiple definition of `main'". Found exactly
// that way. It is the same guard that keeps region.c's file-IO tests off the console.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/options.h"
#include "app/hw.h"

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

// v1.8.5. optionsLoad's render_dist clamp stopped being an absolute and became a per-console
// one — renderDistMaxFor(hwIsNew3ds()) — so three checks in this file that spelled the ceiling
// RENDER_DIST_MAX went red the moment the lift landed. They were read before they were changed
// and the failures were correct, not spurious: the host reports Old 3DS (hwIsNew3ds() answers
// false until hwInit() is told otherwise), so here the ceiling is 3 while RENDER_DIST_MAX is 5,
// and a saved 5 comes back as 3 exactly as it must on an Old 3DS handed a New 3DS's ini.
//
// Respelling those three as ceilingNow() would clear the red without testing anything new, so
// testPerConsoleCeiling() below drives BOTH models through the real app/hw.c seam. That matters
// more than it looks: with the host permanently answering "Old", every other check in this file
// exercises one branch of a two-branch clamp, and a renderDistMaxFor() that ignored its argument
// entirely would still pass all of them.
static int ceilingNow(void)
{
	return renderDistMaxFor(hwIsNew3ds());
}

// MinGW's <sys/stat.h> declares the one-argument MSVC mkdir; POSIX takes a mode. Copied
// from world/world_test.c's testMkdir rather than re-derived, since it is solving the
// exact same "this only runs on the host, and the host might be either" problem.
static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

#define TEST_DIR "build-host/opttest"

// Every path a test below writes to, gathered in one place so testCleanup() can remove
// them all without each test having to remember to clean up after itself.
static const char* const s_test_paths[] = {
	TEST_DIR "/roundtrip.ini",
	TEST_DIR "/roundtrip.ini.tmp",
	TEST_DIR "/unknown_key.ini",
	TEST_DIR "/malformed.ini",
	TEST_DIR "/clamp.ini",
	TEST_DIR "/missing.ini",
	TEST_DIR "/garbage.ini",
	TEST_DIR "/crash_recover.ini",
	TEST_DIR "/crash_recover.ini.tmp",
};

static void testCleanup(void)
{
	for (size_t i = 0; i < sizeof(s_test_paths) / sizeof(s_test_paths[0]); i++)
		remove(s_test_paths[i]);
	// The directory itself is left in place, same as world_test.c leaves
	// build-host/testworld: rmdir portability buys nothing a second run of this test
	// would not immediately recreate.
}

// Writes `text` verbatim to `path`, for tests that need to hand-craft an ini file rather
// than go through optionsSave. Aborts the check that called it (via CHECK) rather than the
// whole binary if the write itself fails — a broken test fixture should show up as a red
// check, not a segfault.
static bool writeRaw(const char* path, const char* text, size_t len)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(text, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

static bool writeText(const char* path, const char* text)
{
	return writeRaw(path, text, strlen(text));
}

// ── Tests ──────────────────────────────────────────────────────────────────────────────

static void testDefaultsInRange(void)
{
	Options o;
	optionsDefaults(&o);

	CHECK(o.render_dist >= RENDER_DIST_MIN && o.render_dist <= RENDER_DIST_MAX);
	CHECK(o.slider_3d >= OPTIONS_SLIDER_MIN && o.slider_3d <= OPTIONS_SLIDER_MAX);
	CHECK(o.invert_look == false);
	CHECK(o.look_sensitivity >= OPTIONS_SENS_MIN && o.look_sensitivity <= OPTIONS_SENS_MAX);

	// Every default binding must be a real, known key, and the defaults must reproduce the
	// controls the game shipped with before this step existed — see options.c's
	// s_action_defaults comment for where each of these came from.
	CHECK(o.bindings[ACTION_MOVE_FORWARD] == OPT_KEY_DUP);
	CHECK(o.bindings[ACTION_MOVE_BACK]    == OPT_KEY_DDOWN);
	CHECK(o.bindings[ACTION_MOVE_LEFT]    == OPT_KEY_DLEFT);
	CHECK(o.bindings[ACTION_MOVE_RIGHT]   == OPT_KEY_DRIGHT);
	CHECK(o.bindings[ACTION_JUMP]         == OPT_KEY_A);
	CHECK(o.bindings[ACTION_BREAK]        == OPT_KEY_X);
	CHECK(o.bindings[ACTION_PLACE]        == OPT_KEY_Y);

	for (int i = 0; i < ACTION_COUNT; i++) {
		bool known = false;
		for (int k = 0; k < OPTIONS_VALID_KEY_COUNT; k++)
			if (OPTIONS_VALID_KEYS[k] == o.bindings[i]) known = true;
		CHECK(known);
	}
}

// Round-trip: every field pushed away from its default, saved, reloaded into a fresh
// struct, and compared field by field. A save/load pair that agrees only on the default
// values would pass a weaker test and still be broken.
static void testRoundTrip(void)
{
	const char* path = TEST_DIR "/roundtrip.ini";

	Options out;
	optionsDefaults(&out);
	out.render_dist       = ceilingNow();   // v1.8.5: still away from the default, which is 1 here
	out.slider_3d          = 0.75f;
	out.invert_look        = true;
	out.look_sensitivity   = 2.25f;
	for (int i = 0; i < ACTION_COUNT; i++)
		out.bindings[i] = OPTIONS_VALID_KEYS[(i + 3) % OPTIONS_VALID_KEY_COUNT];

	CHECK(optionsSave(&out, path));

	Options in;
	int bad = -1;
	CHECK(optionsLoad(&in, path, &bad));
	CHECK(bad == 0);

	CHECK(in.render_dist == out.render_dist);
	CHECK(in.slider_3d == out.slider_3d);
	CHECK(in.invert_look == out.invert_look);
	CHECK(in.look_sensitivity == out.look_sensitivity);
	for (int i = 0; i < ACTION_COUNT; i++)
		CHECK(in.bindings[i] == out.bindings[i]);

	// The tmp file optionsSave writes through must not still be there afterwards — a
	// leftover .tmp next to a good real file is exactly the state optionsRecover would
	// otherwise mistake for an interrupted save on the *next* load.
	FILE* leftover = fopen(TEST_DIR "/roundtrip.ini.tmp", "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

static void testUnknownKeyIgnored(void)
{
	const char* path = TEST_DIR "/unknown_key.ini";
	CHECK(writeText(path,
		"# a future build's setting, this build has never heard of\n"
		"graphics.motion_blur=1\n"
		"render_dist=2\n"
		"totally_unknown_key = yes please\n"));

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);                     // unknown keys are not bad keys
	CHECK(o.render_dist == 2);            // the one real key present still took effect
}

static void testMalformedFallsBackAndCounted(void)
{
	const char* path = TEST_DIR "/malformed.ini";
	CHECK(writeText(path,
		"render_dist=notanumber\n"
		"invert_look=maybe\n"
		"look_sensitivity=\n"
		"bind.jump=0xDEADBEEF\n"));   // syntactically a number, but not a real key bit

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 4);

	// Every malformed field falls back to its default rather than being left half-set.
	Options def;
	optionsDefaults(&def);
	CHECK(o.render_dist == def.render_dist);
	CHECK(o.invert_look == def.invert_look);
	CHECK(o.look_sensitivity == def.look_sensitivity);
	CHECK(o.bindings[ACTION_JUMP] == def.bindings[ACTION_JUMP]);
}

static void testOutOfRangeClamps(void)
{
	const char* path = TEST_DIR "/clamp.ini";
	CHECK(writeText(path,
		"render_dist=9999\n"
		"slider_3d=5.0\n"
		"look_sensitivity=-10\n"));

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));
	// A value that parsed cleanly but landed outside its range is clamped, not defaulted —
	// see options.h's optionsLoad comment for why that is not the same thing as "bad".
	CHECK(bad == 0);
	CHECK(o.render_dist == ceilingNow());
	CHECK(o.slider_3d == OPTIONS_SLIDER_MAX);
	CHECK(o.look_sensitivity == OPTIONS_SENS_MIN);

	CHECK(writeText(path, "render_dist=-1\n"));
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);
	CHECK(o.render_dist == RENDER_DIST_MIN);
}

static void testMissingFileGivesDefaults(void)
{
	const char* path = TEST_DIR "/missing.ini";
	remove(path);   // in case a previous failed run left it behind

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);

	// Field by field, not memcmp. `Options` has three bytes of padding after `invert_look`
	// (a 1-byte bool followed by a 4-byte float), and neither optionsDefaults nor
	// optionsLoad writes padding — it keeps whatever was on the stack. A memcmp here
	// therefore compares two piles of uninitialised garbage and fails for a reason that has
	// nothing to do with options. This version still goes red if any real field differs.
	Options def;
	optionsDefaults(&def);
	CHECK(o.render_dist == def.render_dist);
	CHECK(o.slider_3d == def.slider_3d);
	CHECK(o.invert_look == def.invert_look);
	CHECK(o.look_sensitivity == def.look_sensitivity);
	for (int i = 0; i < ACTION_COUNT; i++)
		CHECK(o.bindings[i] == def.bindings[i]);
}

// A file that is not text at all — arbitrary bytes, no newline anywhere before EOF, some of
// them NUL. Nothing in optionsLoad may read past what fgets hands it or dereference past a
// NUL it did not expect, and the only claim this test makes is that the process is still
// running after optionsLoad returns and that it still reports success (a garbage file is
// not a missing file, but it is still "nothing usable", which optionsLoad treats the same
// way a file full of only unrecognised keys would).
static void testTruncatedGarbageDoesNotCrash(void)
{
	const char* path = TEST_DIR "/garbage.ini";
	unsigned char junk[600];
	for (size_t i = 0; i < sizeof(junk); i++)
		junk[i] = (unsigned char)((i * 91 + 17) & 0xFF);   // deterministic, not all-zero
	junk[300] = 0x00;                                       // an embedded NUL mid-buffer

	CHECK(writeRaw(path, (const char*)junk, sizeof(junk)));

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));   // reaching this line at all is most of the test
	CHECK(o.render_dist >= RENDER_DIST_MIN && o.render_dist <= RENDER_DIST_MAX);
	(void)bad;   // no claim on the count here — the point is survival, not a specific tally

	// A file that ends mid-token, no trailing newline: the last line's value is whatever
	// arrived before EOF, which must be handled like any other malformed value rather than
	// read past the buffer.
	CHECK(writeText(path, "render_dist=1\nbind.jump=0x"));
	int bad2 = -1;
	CHECK(optionsLoad(&o, path, &bad2));
	CHECK(o.render_dist == 1);
	CHECK(bad2 == 1);   // the truncated bind.jump value
}

// Exercises the crash-safety window optionsSave documents: a save that was cut between
// removing the old file and renaming the new one into place leaves no file at `path` and a
// complete `path.tmp` beside it. optionsLoad must recover from exactly that state rather
// than reporting the settings as missing and quietly reverting to defaults.
static void testCrashRecovery(void)
{
	const char* path = TEST_DIR "/crash_recover.ini";
	const char* tmp  = TEST_DIR "/crash_recover.ini.tmp";
	remove(path);
	remove(tmp);

	Options saved;
	optionsDefaults(&saved);
	saved.render_dist = ceilingNow();
	CHECK(optionsSave(&saved, path));

	// Simulate the cut: put the bytes back as a .tmp and remove the real file, which is
	// exactly the on-disk state a power loss between optionsSave's remove() and rename()
	// would leave.
	FILE* src = fopen(path, "rb");
	CHECK(src != NULL);
	char buf[4096];
	size_t n = src ? fread(buf, 1, sizeof(buf), src) : 0;
	if (src) fclose(src);
	CHECK(writeRaw(tmp, buf, n));
	remove(path);

	Options recovered;
	int bad = -1;
	CHECK(optionsLoad(&recovered, path, &bad));
	CHECK(bad == 0);
	CHECK(recovered.render_dist == ceilingNow());

	// Recovery must have promoted the tmp back to the real path, not merely read through it
	// — a second load has to see the same thing without the .tmp still sitting there.
	FILE* real = fopen(path, "rb");
	CHECK(real != NULL);
	if (real) fclose(real);
	FILE* leftover = fopen(tmp, "rb");
	CHECK(leftover == NULL);
	if (leftover) fclose(leftover);
}

// v1.8.5. The clamp optionsLoad applies is per-console, and the host answers "Old 3DS" for the
// whole rest of this file, so without this every render_dist check here proves one branch of a
// two-branch decision. app/hw.h's host-only seam exists precisely for this: hwTestReset() forgets
// that hwInit() ran, hwTestSetNew3ds() says what the next hwInit() will report.
//
// The interesting direction is the SECOND half — a New 3DS ini (render_dist=5) loaded on an Old
// 3DS. That is not hypothetical: options.ini travels with the SD card, and before this clamp
// existed a 5 out of that file would have been handed straight to a mesh pool allocated for 3.
//
// Written to fail if the clamp stops consulting the model: with renderDistMaxFor() ignoring its
// argument, "new=5" and "old rejects 5" cannot both hold whatever constant it returns.
static void testPerConsoleCeiling(void)
{
	const char* path = TEST_DIR "/console_ceiling.ini";
	Options o;
	int bad = -1;

	// A New 3DS may keep 5.
	hwTestReset();
	hwTestSetNew3ds(true);
	hwInit();
	CHECK(hwIsNew3ds());
	CHECK(ceilingNow() == RENDER_DIST_MAX_NEW);
	CHECK(writeText(path, "render_dist=5\n"));
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);
	CHECK(o.render_dist == 5);

	// The same file on an Old 3DS is clamped down, not honoured and not defaulted — an
	// out-of-range value that parsed cleanly is still not a parse failure.
	hwTestReset();
	hwTestSetNew3ds(false);
	hwInit();
	CHECK(!hwIsNew3ds());
	CHECK(ceilingNow() == RENDER_DIST_MAX_OLD);
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);
	CHECK(o.render_dist == RENDER_DIST_MAX_OLD);

	// Leave the process reporting Old 3DS, which is what it reported before this ran and what
	// every other test in this file was written against. A test that changes global state and
	// does not put it back makes the ORDER of the calls in main() load-bearing.
	CHECK(!hwIsNew3ds());
	remove(path);
}

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testDefaultsInRange();
	testRoundTrip();
	testUnknownKeyIgnored();
	testMalformedFallsBackAndCounted();
	testOutOfRangeClamps();
	testPerConsoleCeiling();
	testMissingFileGivesDefaults();
	testTruncatedGarbageDoesNotCrash();
	testCrashRecovery();

	testCleanup();

	if (s_fails == 0)
		printf("options self-test: PASS  %d checks\n", s_checks);
	else
		printf("options self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int options_test_host_only_t;

#endif   // !__3DS__
