// Host self-test for app/debugmenu.c and app/options.c debug_menu persistence.
// Follows the same pattern as options_test.c — own main(), CHECK macro.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/debugmenu.h"
#include "app/options.h"

// Pulled in as source, not linked, on purpose. tools/run_host_tests.sh builds this
// binary from options.c + debugmenu.c + this file; debugmenu_ui.c's console half is
// compiled out on the host (see its #ifdef __3DS__ block), so including it here puts
// debugMenuUiOpen/debugMenuUiUpdate in this translation unit without the script — and
// without a second binary — having to know about it.
#include "app/debugmenu_ui.c"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                        \
	} while (0)

// ── The pool capacity, pinned to a hand-written literal ─────────────────
//
// DEBUG_MENU_MAX_ENTRIES is the size of app/debugmenu.c's static s_pool, i.e. the total number
// of rows the debug menu can hold across every subsystem that registers one. It is the ceiling
// on the feature, and debugMenuRegister() returns NULL the moment it is reached.
//
// Before 2026-08-25 testRegistryPoolFull() asserted that ceiling against ITSELF. It read
//
//     for (int i = 0; i < DEBUG_MENU_MAX_ENTRIES; i++)
//             CHECK(debugMenuRegister() != NULL);
//     ...
//     CHECK(debugMenuCount() == DEBUG_MENU_MAX_ENTRIES);
//
// so the loop BOUND and the expectation were the same constant: shrinking it shrank the work and
// the expectation together, and the checks that would have caught the shrink were never run at
// all rather than failed. Measured, not theorised: app/debugmenu.h's DEBUG_MENU_MAX_ENTRIES cut
// from 32 to 8 — a 75% cut in how many rows the menu can hold — built clean, and this suite went
// from "debugmenu self-test: PASS  80 checks" to "debugmenu self-test: PASS  56 checks", exit 0
// both times. Twenty-four checks vanished and nothing said so.
//
// So the bound and the expectations are a naked literal now. It must NEVER be computed from
// DEBUG_MENU_MAX_ENTRIES or from anything else app/debugmenu.{c,h} can also move. Resizing the
// pool for real means editing this one line by hand, and the suite going red until you do is the
// entire point. Same layering as source/world/registry_test.c's REGISTRY_DYN_*_PIN (dc1fea3) and
// source/net/networld_test.c's remote-player pin (0258188), so the fleet has one pattern.
#define DEBUG_MENU_MAX_ENTRIES_PIN 32

// Compile-time layer. This fires whenever the host suite builds, which is every
// tools/run_host_tests.sh run; the 3DS build never compiles this file (see the __3DS__ guard at
// the top). The runtime check in testRegistryPoolFull() is what remains if anyone deletes it.
_Static_assert(DEBUG_MENU_MAX_ENTRIES == DEBUG_MENU_MAX_ENTRIES_PIN,
               "app/debugmenu.h's DEBUG_MENU_MAX_ENTRIES is 32, the debug menu's row capacity; "
               "if you resized the pool deliberately, update DEBUG_MENU_MAX_ENTRIES_PIN in "
               "source/app/debugmenu_test.c by hand");

// The remedy text the capacity pins print when they fail, so that someone who resized the pool
// on purpose is told which line to edit instead of being handed a bare failed expression.
static const char* const kPoolCapWhy =
	"32 is the SIZE OF THE DEBUG-MENU ENTRY POOL (DEBUG_MENU_MAX_ENTRIES in\n"
	"         source/app/debugmenu.h, the length of s_pool in source/app/debugmenu.c) — the\n"
	"         total number of rows every subsystem can register between them. If you resized\n"
	"         that pool ON PURPOSE, update DEBUG_MENU_MAX_ENTRIES_PIN in\n"
	"         source/app/debugmenu_test.c to match. If you did NOT, the menu has silently lost\n"
	"         capacity and rows registered late will simply never appear. This pin is\n"
	"         deliberately NOT derived from DEBUG_MENU_MAX_ENTRIES: a pin computed from the\n"
	"         constant it is pinning moves with it and guards nothing.";

// CHECK for a pinned literal. Counts exactly like CHECK does — it must, or it would perturb the
// very total that makes a shrunken loop visible — but a failure also prints the measured value,
// the expected one, and what to do about it.
static void checkPin(bool cond, long got, long want, const char* what, const char* why)
{
	s_checks++;
	if (cond) return;

	s_fails++;
	printf("  FAIL   %s: expected %ld, got %ld.\n         %s\n", what, want, got, why);
	if (!s_first[0]) snprintf(s_first, sizeof(s_first), "%.140s", what);
}

static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

#define TEST_DIR "build-host/dbgtest"

static const char* const s_test_paths[] = {
	TEST_DIR "/debug_roundtrip.ini",
	TEST_DIR "/debug_roundtrip.ini.tmp",
};

static void testCleanup(void)
{
	for (size_t i = 0; i < sizeof(s_test_paths) / sizeof(s_test_paths[0]); i++)
		remove(s_test_paths[i]);
}

// ── Registry tests ──────────────────────────────────────────────────────

static bool s_toggle_val;
static int  s_slider_val;
static int  s_action_count;

static bool getToggle(void* ctx) { (void)ctx; return s_toggle_val; }
static void setToggle(void* ctx, bool v) { (void)ctx; s_toggle_val = v; }
static int  getSlider(void* ctx) { (void)ctx; return s_slider_val; }
static void setSlider(void* ctx, int v) { (void)ctx; s_slider_val = v; }
static void doAction(void* ctx) { (void)ctx; s_action_count++; }

static void testRegistryEmpty(void)
{
	debugMenuReset();
	CHECK(debugMenuCount() == 0);
	CHECK(debugMenuEntry(0) == NULL);
	CHECK(debugMenuHead() == -1);
}

static void testRegistryAddAndCount(void)
{
	debugMenuReset();
	DebugEntry* e1 = debugMenuRegister();
	CHECK(e1 != NULL);
	CHECK(debugMenuCount() == 1);

	DebugEntry* e2 = debugMenuRegister();
	CHECK(e2 != NULL);
	CHECK(debugMenuCount() == 2);

	CHECK(e1 != e2);
}

static void testRegistryIteration(void)
{
	debugMenuReset();
	DebugEntry* a = debugMenuRegister();
	a->name = "Alpha";
	DebugEntry* b = debugMenuRegister();
	b->name = "Beta";
	DebugEntry* c = debugMenuRegister();
	c->name = "Gamma";

	CHECK(debugMenuCount() == 3);

	// Walk the linked list.
	int head = debugMenuHead();
	CHECK(head >= 0 && head < 3);
	DebugEntry* he = debugMenuEntry(head);
	CHECK(he != NULL);

	int n1 = debugMenuNext(head);
	CHECK(n1 >= 0 && n1 < 3);
	int n2 = debugMenuNext(n1);
	CHECK(n2 >= 0 && n2 < 3);
	CHECK(debugMenuNext(n2) == -1);   // end of list
}

static void testRegistryToggleDispatch(void)
{
	debugMenuReset();
	DebugEntry* e = debugMenuRegister();
	e->name    = "Test toggle";
	e->kind    = DEBUG_TOGGLE;
	e->getBool = getToggle;
	e->setBool = setToggle;
	e->ctx     = NULL;

	s_toggle_val = false;
	CHECK(e->getBool(e->ctx) == false);

	e->setBool(e->ctx, true);
	CHECK(s_toggle_val == true);
	CHECK(e->getBool(e->ctx) == true);
}

static void testRegistrySliderDispatch(void)
{
	debugMenuReset();
	DebugEntry* e = debugMenuRegister();
	e->name       = "Test slider";
	e->kind       = DEBUG_SLIDER_INT;
	e->getInt     = getSlider;
	e->setInt     = setSlider;
	e->slider_min = 1;
	e->slider_max = 5;
	e->ctx        = NULL;

	s_slider_val = 3;
	CHECK(e->getInt(e->ctx) == 3);

	e->setInt(e->ctx, 7);
	CHECK(s_slider_val == 7);   // setInt does not clamp; caller is responsible
}

static void testRegistryActionDispatch(void)
{
	debugMenuReset();
	DebugEntry* e = debugMenuRegister();
	e->name   = "Test action";
	e->kind   = DEBUG_ACTION;
	e->action = doAction;
	e->ctx    = NULL;

	s_action_count = 0;
	e->action(e->ctx);
	CHECK(s_action_count == 1);
	e->action(e->ctx);
	CHECK(s_action_count == 2);
}

static void testRegistryUnavailable(void)
{
	debugMenuReset();
	DebugEntry* e = debugMenuRegister();
	e->name      = "Future feature";
	e->kind      = DEBUG_TOGGLE;
	e->available = false;

	CHECK(e->available == false);
	// Counted but available=false — the UI greys it out.
	CHECK(debugMenuCount() == 1);
}

static void testRegistryPoolFull(void)
{
	debugMenuReset();

	// The runtime half of the capacity pin, FIRST so that it is the failure the summary line
	// names when the pool has been resized. The _Static_assert above is the primary defence;
	// this one is what survives someone deleting it.
	checkPin(DEBUG_MENU_MAX_ENTRIES == DEBUG_MENU_MAX_ENTRIES_PIN,
	         (long)DEBUG_MENU_MAX_ENTRIES, (long)DEBUG_MENU_MAX_ENTRIES_PIN,
	         "the debug-menu entry pool still holds 32 rows", kPoolCapWhy);

	// The loop bound is the LITERAL, so this always attempts 32 registrations however big the
	// pool actually is. That is the whole fix: with DEBUG_MENU_MAX_ENTRIES as the bound, a
	// smaller pool ran fewer iterations and emitted fewer checks, and the ones that would have
	// gone red were simply never reached. Now a shrunken pool FAILS these instead of skipping
	// them, and the suite's total does not move.
	for (int i = 0; i < DEBUG_MENU_MAX_ENTRIES_PIN; i++)
		CHECK(debugMenuRegister() != NULL);

	DebugEntry* overflow = debugMenuRegister();
	CHECK(overflow == NULL);
	checkPin(debugMenuCount() == DEBUG_MENU_MAX_ENTRIES_PIN,
	         (long)debugMenuCount(), (long)DEBUG_MENU_MAX_ENTRIES_PIN,
	         "a full pool reports exactly 32 entries", kPoolCapWhy);
}

// ── Options persistence tests ───────────────────────────────────────────

static void testDebugMenuOptionDefault(void)
{
	Options o;
	optionsDefaults(&o);
	CHECK(o.debug_menu == false);
}

static void testDebugMenuOptionRoundTrip(void)
{
	const char* path = TEST_DIR "/debug_roundtrip.ini";

	Options out;
	optionsDefaults(&out);
	out.debug_menu = true;
	CHECK(optionsSave(&out, path));

	Options in;
	int bad = -1;
	CHECK(optionsLoad(&in, path, &bad));
	CHECK(bad == 0);
	CHECK(in.debug_menu == true);
}

static void testDebugMenuOptionBackwardCompat(void)
{
	// An options.ini without debug_menu should default to false.
	const char* path = TEST_DIR "/debug_roundtrip.ini";
	FILE* f = fopen(path, "w");
	fprintf(f, "render_dist=2\n");
	fclose(f);

	Options o;
	int bad = -1;
	CHECK(optionsLoad(&o, path, &bad));
	CHECK(bad == 0);
	CHECK(o.debug_menu == false);
}

// ── UI input tests ──────────────────────────────────────────────────────
//
// The frame-input-reuse bug these guard, in full: main.c reads hidKeysDown() once
// per frame into `down`, hands that same word to pauseMenuInput() — which consumes
// KEY_A to pick the "Debug" row and calls debugMenuUiOpen() — and then, in the SAME
// loop iteration, hands the unchanged word to debugMenuUiUpdate(). debugMenuUiOpen()
// has just parked the cursor on entry 0, which is the "Render distance" slider, so
// the A press that opened the menu also stepped the slider, re-meshing the whole
// ring. The player never pressed A twice; one press did two things.
//
// A test that only checked "A does nothing on the open frame" could pass against a
// build where A never works at all, so testUiSecondFrameAActivates() below is the
// control: same entry, same key, one frame later, and it MUST move.

static void makeSliderEntry(int initial)
{
	debugMenuReset();
	DebugEntry* e = debugMenuRegister();
	e->name       = "Render distance";
	e->kind       = DEBUG_SLIDER_INT;
	e->getInt     = getSlider;
	e->setInt     = setSlider;
	e->slider_min = 2;
	e->slider_max = 6;
	e->ctx        = NULL;
	s_slider_val  = initial;
}

static void testUiOpenFrameDoesNotActivate(void)
{
	makeSliderEntry(3);

	DebugContext ctx = {0};
	debugMenuUiInit();
	debugMenuUiOpen();

	// The exact word main.c passes on the opening frame: the A that chose "Debug".
	CHECK(debugMenuUiUpdate(&ctx, NULL, KEY_A, false, 0, 0) == true);
	CHECK(s_slider_val == 3);   // must NOT have stepped to 4
}

static void testUiSecondFrameAActivates(void)
{
	makeSliderEntry(3);

	DebugContext ctx = {0};
	debugMenuUiInit();
	debugMenuUiOpen();

	debugMenuUiUpdate(&ctx, NULL, KEY_A, false, 0, 0);       // opening frame, swallowed
	debugMenuUiUpdate(&ctx, NULL, KEY_A, false, 0, 0);       // deliberate press
	CHECK(s_slider_val == 4);
}

static void testUiOpenFrameSwallowsTouchToo(void)
{
	// Same defect class: the pause menu is button-driven today, but a touch that is
	// still down on the opening frame would land on whatever row happens to sit under
	// the stylus. Row 0 is at PANEL_Y + 34; ROW_X + 6 is inside it.
	makeSliderEntry(3);

	DebugContext ctx = {0};
	debugMenuUiInit();
	debugMenuUiOpen();

	CHECK(debugMenuUiUpdate(&ctx, NULL, 0, true, ROW_X + 6, PANEL_Y + 34) == true);
	CHECK(s_cursor == 0);
}

static void testUiOpenFrameGuardIsOncePerOpen(void)
{
	// Closing and reopening must arm the guard again — otherwise the second visit to
	// the menu has the original bug back.
	makeSliderEntry(3);

	DebugContext ctx = {0};
	debugMenuUiInit();
	debugMenuUiOpen();
	debugMenuUiUpdate(&ctx, NULL, 0, false, 0, 0);           // opening frame
	CHECK(debugMenuUiUpdate(&ctx, NULL, KEY_B, false, 0, 0) == false);   // B closes

	debugMenuUiOpen();
	CHECK(debugMenuUiUpdate(&ctx, NULL, KEY_A, false, 0, 0) == true);
	CHECK(s_slider_val == 3);
}

// ── main.c debug-menu save-path test ────────────────────────────────────
//
// This one parses source text rather than calling a function, for the same reason
// world/atlas_uv_shader_test.c does: the code under test is inside main.c's frame
// loop, which has no host build and no seam to call into. Mirroring the sequence
// here instead would prove nothing — a mirror passes whatever main.c actually says.
//
// The bug: the pause-menu path assigns opts.render_dist = s_mesh_radius before
// optionsSave (main.c, the `if (dist_step != 0)` block), because s_mesh_radius is
// the CLAMPED value genSetRadius settled on and opts.render_dist is what survives a
// reboot. The debug-menu path calls the same optionsSave with that assignment
// missing, so a render distance changed from the debug menu applies live and is then
// written to options.ini as the OLD number and lost on the next boot.
//
// Every failure path below is a loud failure, never a skip: a parse that cannot find
// the call site must not read as "nothing wrong".

#define MAIN_PATH "source/main.c"

// Reads MAIN_PATH and returns true when `opts.render_dist = s_mesh_radius` appears
// between the debugMenuUiUpdate( call and the optionsSave( that follows it. Fills
// errbuf and returns false on any parse failure, so the caller can fail loudly.
static bool debugSavePathAssignsRenderDist(char* errbuf, size_t errbufsz)
{
	FILE* f = fopen(MAIN_PATH, "r");
	if (!f) {
		snprintf(errbuf, errbufsz, "cannot open %s (run from the project root)", MAIN_PATH);
		return false;
	}

	char line[512];
	bool seen_call   = false;
	bool seen_assign = false;

	while (fgets(line, sizeof(line), f)) {
		if (!seen_call) {
			if (strstr(line, "debugMenuUiUpdate(")) seen_call = true;
			continue;
		}
		if (strstr(line, "opts.render_dist") && strstr(line, "s_mesh_radius"))
			seen_assign = true;
		// The first optionsSave after the call is the debug menu's own save; stop there
		// so a later, unrelated save site cannot make this pass by accident.
		if (strstr(line, "optionsSave(")) {
			fclose(f);
			return seen_assign;
		}
	}

	fclose(f);
	snprintf(errbuf, errbufsz,
	         seen_call ? "no optionsSave( after debugMenuUiUpdate( in %s"
	                   : "no debugMenuUiUpdate( call found in %s",
	         MAIN_PATH);
	return false;
}

static void testMainDebugSaveWritesNewRenderDist(void)
{
	char err[200];
	err[0] = '\0';
	const bool ok = debugSavePathAssignsRenderDist(err, sizeof(err));
	if (!ok && err[0])
		printf("  debug save-path parse: %s\n", err);
	CHECK(ok);
}

// The parse test above proves main.c *contains* the assignment. This one proves the
// assignment is what makes the new number survive, by running the same two lines main.c
// now runs and then reading the file back off disk. The pair is deliberate: the parser
// alone would go green against `opts.render_dist = s_mesh_radius;` written anywhere with
// any semantics, and this alone would go green against a main.c that never does it.
//
// Red-armed: delete the `opts.render_dist = new_radius;` line below and the last CHECK
// fails with in.render_dist == 1 instead of 3 — the exact reboot symptom.
//
// The numbers are RENDER_DIST_MIN (1) and RENDER_DIST_MAX (3), measured from
// scene/render_dist.h, not picked: options.c:277 clamps render_dist into that range on
// load, so a bigger "new" value would come back clamped and the test would pass for the
// wrong reason.
static void testDebugSaveRoundTripsNewRenderDist(void)
{
	const char* path = TEST_DIR "/debug_roundtrip.ini";

	Options opts;
	optionsDefaults(&opts);
	opts.render_dist = RENDER_DIST_MIN;   // what was on disk before the menu was opened
	CHECK(optionsSave(&opts, path));

	// What genSetRadius clamped to and s_mesh_radius now reads back as, live.
	const int new_radius = RENDER_DIST_MAX;
	CHECK(new_radius != RENDER_DIST_MIN);   // otherwise this test could not go red

	opts.render_dist = new_radius;
	CHECK(optionsSave(&opts, path));

	Options in;
	int bad = -1;
	CHECK(optionsLoad(&in, path, &bad));
	CHECK(bad == 0);
	CHECK(in.render_dist == RENDER_DIST_MAX);
}

// ── Main ────────────────────────────────────────────────────────────────

int main(void)
{
	testMkdir("build-host");
	testMkdir(TEST_DIR);

	testRegistryEmpty();
	testRegistryAddAndCount();
	testRegistryIteration();
	testRegistryToggleDispatch();
	testRegistrySliderDispatch();
	testRegistryActionDispatch();
	testRegistryUnavailable();
	testRegistryPoolFull();
	testDebugMenuOptionDefault();
	testDebugMenuOptionRoundTrip();
	testDebugMenuOptionBackwardCompat();
	testUiOpenFrameDoesNotActivate();
	testUiSecondFrameAActivates();
	testUiOpenFrameSwallowsTouchToo();
	testUiOpenFrameGuardIsOncePerOpen();
	testMainDebugSaveWritesNewRenderDist();
	testDebugSaveRoundTripsNewRenderDist();

	testCleanup();

	if (s_fails == 0)
		printf("debugmenu self-test: PASS  %d checks\n", s_checks);
	else
		printf("debugmenu self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int debugmenu_test_host_only_t;

#endif
