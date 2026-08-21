// Host self-test for app/debugmenu.c and app/options.c debug_menu persistence.
// Follows the same pattern as options_test.c — own main(), CHECK macro.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/debugmenu.h"
#include "app/options.h"

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
	for (int i = 0; i < DEBUG_MENU_MAX_ENTRIES; i++)
		CHECK(debugMenuRegister() != NULL);

	DebugEntry* overflow = debugMenuRegister();
	CHECK(overflow == NULL);
	CHECK(debugMenuCount() == DEBUG_MENU_MAX_ENTRIES);
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
