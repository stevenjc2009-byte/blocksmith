// NEON BIOME BORDERS — the WIRING probe.
//
// biomeborder_test.c proves the arithmetic and the offline rasterizer proves the picture.
// Neither can prove the thing the owner actually asked for: that this is a DEBUG OPTION and
// not a game feature, and that the one draw site added to source/main.c is in the right place.
// Both of those are facts about main.c's text, so this reads main.c's text.
//
// A test that re-implemented the wiring would prove only that two pieces of code agree. This
// asserts on the SHIPPED SOURCE, which is why the strings below are quoted exactly as they
// appear: an edit that renames or moves any of them fails here rather than silently unhooking
// the overlay and leaving a green suite behind.
//
// Host only. Run from the repo root; every path is relative to it.
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;
static char g_first[8][256];

#define CHECK(cond, ...) do {                                              \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
        if (g_fails < 8) snprintf(g_first[g_fails], sizeof g_first[0],     \
                                  "L%d " #cond, __LINE__);                 \
        g_fails++;                                                         \
        printf("FAIL  L%d: ", __LINE__); printf(__VA_ARGS__); printf("\n");\
    }                                                                      \
} while (0)

static char* slurp(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* b = malloc((size_t)n + 1);
	if (!b) { fclose(f); return NULL; }
	if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
	b[n] = 0;
	fclose(f);
	return b;
}

static const char* findAfter(const char* hay, const char* needle, const char* from)
{
	const char* start = from ? from : hay;
	return strstr(start, needle);
}

// The three files are addressable so a red-arm harness can point this at a SABOTAGED COPY in
// a scratchpad instead of editing the repo. Five other lanes are building out of this tree at
// the same time; a broken main.c on disk, even for a second, is their build failing for a
// reason they cannot see. The defaults are the real paths, so an ordinary run is unchanged.
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* p_main = argc > 1 ? argv[1] : "source/main.c";
	const char* p_draw = argc > 2 ? argv[2] : "source/debug/biomeborder_draw.c";
	const char* p_bb   = argc > 3 ? argv[3] : "source/debug/biomeborder.c";

	char* main_c = slurp(p_main);
	CHECK(main_c != NULL, "cannot read %s — run from the repo root", p_main);
	if (!main_c) return 1;

	// ── 1. it is compiled in at all ──────────────────────────────────────────────────
	CHECK(strstr(main_c, "#include \"debug/biomeborder.h\"") != NULL,
	      "main.c does not include debug/biomeborder.h");
	CHECK(strstr(main_c, "#include \"debug/biomeborder_draw.h\"") != NULL,
	      "main.c does not include debug/biomeborder_draw.h");

	// ── 2. the DEBUG MENU row, and only that ─────────────────────────────────────────
	//
	// The owner's words were "make sure this is only a debug option, not a regular game
	// feature". A DEBUG_TOGGLE row registered in bsDebugRegister() is the whole of the user
	// interface; there is no other reachable path and the checks in section 5 hold that.
	const char* reg = strstr(main_c, "e->name      = \"Biome borders\";");
	CHECK(reg != NULL, "no \"Biome borders\" row registered in main.c");
	if (reg) {
		const char* end = reg + 400 < main_c + strlen(main_c) ? reg + 400 : main_c + strlen(main_c);
		char win[401];
		size_t wn = (size_t)(end - reg);
		memcpy(win, reg, wn); win[wn] = 0;
		CHECK(strstr(win, "DEBUG_TOGGLE") != NULL, "the Biome borders row is not a DEBUG_TOGGLE");
		CHECK(strstr(win, "bsDbgGetBiomeBorders") != NULL, "the row has no getBool");
		CHECK(strstr(win, "bsDbgSetBiomeBorders") != NULL, "the row has no setBool");
	}
	CHECK(strstr(main_c, "return biomeBorderEnabled();") != NULL,
	      "the row's getBool does not read biomeBorderEnabled()");
	CHECK(strstr(main_c, "biomeBorderSetEnabled(on);") != NULL,
	      "the row's setBool does not call biomeBorderSetEnabled()");

	// The row must sit inside the debug-menu registration function and nowhere else. If it
	// drifted into, say, the pause menu's own row list it would become a game setting.
	const char* dbgreg = strstr(main_c, "bsDebugRegister(void)");
	CHECK(dbgreg != NULL, "bsDebugRegister not found in main.c");
	CHECK(dbgreg != NULL && reg != NULL && reg > dbgreg,
	      "the Biome borders row is registered outside bsDebugRegister");

	// ── 3. THE DRAW SITE ─────────────────────────────────────────────────────────────
	//
	// There is no render hook in this program and none was invented for this. drawEye() calls
	// its passes as direct symbols, so the overlay is one more direct symbol call, and WHERE
	// it sits is load-bearing: after the terrain so the fence is depth-tested against the
	// ground it stands on, before the selection highlight so the cage the player is aiming
	// with stays on top of it.
	const char* eye = strstr(main_c, "drawEye(");
	CHECK(eye != NULL, "drawEye not found in main.c");
	const char* terrain = findAfter(main_c, "chunkRenderDraw(view);", eye);
	const char* fence   = findAfter(main_c, "biomeBorderDraw(view);", eye);
	const char* hilite  = findAfter(main_c, "highlightDraw(", eye);
	CHECK(terrain != NULL, "chunkRenderDraw(view); not found after drawEye");
	CHECK(fence   != NULL, "biomeBorderDraw(view); IS NOT CALLED — the overlay is unhooked");
	CHECK(hilite  != NULL, "highlightDraw not found after drawEye");
	CHECK(terrain && fence && terrain < fence,
	      "biomeBorderDraw runs BEFORE the terrain — the fence would not be depth-tested");
	CHECK(fence && hilite && fence < hilite,
	      "biomeBorderDraw runs AFTER the highlight — the fence would cover the aim cage");

	// ── 4. lifecycle ─────────────────────────────────────────────────────────────────
	CHECK(strstr(main_c, "biomeBorderDrawInit()") != NULL, "biomeBorderDrawInit is never called");
	CHECK(strstr(main_c, "biomeBorderDrawExit()") != NULL, "biomeBorderDrawExit is never called");
	CHECK(strstr(main_c, "biomeBorderSetWorldGen(&s_gen);") != NULL,
	      "the live WorldGen is never handed over — the overlay would classify nothing");
	CHECK(strstr(main_c, "biomeBorderDrawInvalidate();") != NULL,
	      "the cache is never invalidated on world start — a new world would show the old fence");
	CHECK(strstr(main_c, "biomeBorderReset();") != NULL,
	      "biomeBorderReset is never called — the toggle would survive a world teardown");

	// The POINTER, never a seed. worldgenInit stores rngMix(seed ^ 'BLKS'), so a round trip
	// through a seed accessor classifies a different world and says nothing about it.
	CHECK(strstr(main_c, "biomeBorderSetWorldGen(s_gen.seed") == NULL,
	      "main.c hands the overlay a SEED — see the hazard note in debug/biomeborder.h");

	// ── 5. it is a debug option and NOTHING turns it on ──────────────────────────────
	//
	// Nowhere in main.c may the toggle be set true except through the debug row's setBool,
	// which takes its argument from the menu. A literal true anywhere would be the feature
	// switching itself on.
	CHECK(strstr(main_c, "biomeBorderSetEnabled(true)") == NULL,
	      "main.c turns the overlay ON by itself — it would not be a debug option");

	free(main_c);

	// ── 6. the OFF gate is the FIRST thing the draw half does ────────────────────────
	char* draw_c = slurp(p_draw);
	CHECK(draw_c != NULL, "cannot read %s", p_draw);
	if (draw_c) {
		const char* fn = strstr(draw_c, "void biomeBorderDraw(const C3D_Mtx* view)");
		CHECK(fn != NULL, "biomeBorderDraw not defined with the expected signature");
		if (fn) {
			const char* gate = strstr(fn, "if (!biomeBorderEnabled())");
			CHECK(gate != NULL, "biomeBorderDraw has NO off gate");
			// Nothing may run before it. C3D_ is the prefix of every GPU call in the file, so
			// if one appears between the opening brace and the gate the off path is not free.
			const char* brace = strchr(fn, '{');
			if (gate && brace) {
				CHECK(gate > brace, "the gate is above the function body");
				size_t span = (size_t)(gate - brace);
				char* head = malloc(span + 1);
				memcpy(head, brace, span); head[span] = 0;
				CHECK(strstr(head, "C3D_") == NULL,
				      "a GPU call runs BEFORE the off gate — off would not be free");
				CHECK(strstr(head, "biomeBorderBuild") == NULL,
				      "the search runs BEFORE the off gate — off would not be free");
				free(head);
			}
		}
		free(draw_c);
	}

	// ── 7. the default is OFF, in the source, not just at runtime ────────────────────
	char* bb_c = slurp(p_bb);
	CHECK(bb_c != NULL, "cannot read %s", p_bb);
	if (bb_c) {
		CHECK(strstr(bb_c, "static bool            s_enabled;") != NULL,
		      "s_enabled is not a bare uninitialised bool — the default-off guarantee is gone");
		CHECK(strstr(bb_c, "s_enabled = true;") == NULL, "s_enabled is set true somewhere in the module");
		free(bb_c);
	}

	printf("checks: %d  failures: %d\n", g_checks, g_fails);
	if (g_fails) {
		printf("first failures:\n");
		for (int i = 0; i < g_fails && i < 8; i++) printf("  %s\n", g_first[i]);
		printf("biomeborder wiring self-test: FAIL  %d/%d\n", g_fails, g_checks);
		return 1;
	}
	printf("biomeborder wiring self-test: PASS  %d checks\n", g_checks);
	return 0;
}

#endif /* !__3DS__ */
