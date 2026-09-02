// Host self-test for source/debug/biomeinfo.c — the bottom-screen biome readout row.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "debug/biomeinfo.h"
#include "world/genversion.h"

static int g_checks;
static int g_fails;
static int g_first_line;

#define CHECK(cond)                                                            \
	do {                                                                       \
		g_checks++;                                                            \
		if (!(cond)) {                                                         \
			if (!g_fails) { g_first_line = __LINE__; }                         \
			g_fails++;                                                         \
			if (g_fails <= 8)                                                  \
				printf("  FAIL  L%d  %s\n", __LINE__, #cond);                  \
		}                                                                      \
	} while (0)

// Bare literals, not BIOME_NAMES[BIOME_TAIGA]. A pin written in terms of the thing it is
// pinning moves with it and catches nothing — this project has recorded cases of exactly
// that. These six strings are the contract; if the table is reordered these go red.
static void testNames(void)
{
	CHECK(strcmp(debugBiomeName(BIOME_TUNDRA), "tundra") == 0);
	CHECK(strcmp(debugBiomeName(BIOME_TAIGA),  "taiga")  == 0);
	CHECK(strcmp(debugBiomeName(BIOME_PLAINS), "plains") == 0);
	CHECK(strcmp(debugBiomeName(BIOME_FOREST), "forest") == 0);
	CHECK(strcmp(debugBiomeName(BIOME_DESERT), "desert") == 0);
	CHECK(strcmp(debugBiomeName(BIOME_JUNGLE), "jungle") == 0);

	// Out of range in both directions, and one past the end, rather than off the table.
	CHECK(strcmp(debugBiomeName((BiomeId)-1), "?") == 0);
	CHECK(strcmp(debugBiomeName((BiomeId)BIOME_COUNT), "?") == 0);
	CHECK(strcmp(debugBiomeName((BiomeId)999), "?") == 0);

	// Every name distinct and non-empty — a table with two identical entries would pass a
	// per-name check written the lazy way but make two biomes indistinguishable on screen.
	for (int a = 0; a < BIOME_COUNT; a++) {
		CHECK(debugBiomeName((BiomeId)a)[0] != '\0');
		for (int b = a + 1; b < BIOME_COUNT; b++)
			CHECK(strcmp(debugBiomeName((BiomeId)a), debugBiomeName((BiomeId)b)) != 0);
	}
}

// The placeholder half: with no world wired up the row must SAY so, not guess and not go
// blank. This is the state the build actually ships in until main.c gets its one line.
static void testUnwired(void)
{
	char buf[64];

	debugBiomeSetWorldGen(NULL);
	CHECK(!debugBiomeKnown());
	CHECK(strcmp(debugBiomeRow(0, 0, buf, sizeof(buf)), BIOME_ROW_UNWIRED) == 0);
	// Far from the origin too — nothing about the unwired answer may depend on position.
	CHECK(strcmp(debugBiomeRow(999999, -50000, buf, sizeof(buf)), BIOME_ROW_UNWIRED) == 0);

	// It must not be mistakable for a real reading: no biome name may appear in it.
	for (int i = 0; i < BIOME_COUNT; i++)
		CHECK(strstr(BIOME_ROW_UNWIRED, debugBiomeName((BiomeId)i)) == NULL);
}

// The instrument must share the code path. The row is not compared against a string this
// test builds from its own idea of the biome — it is compared against a fresh call to
// worldgenBiomeAt, the same classifier the world itself generates terrain with. If the row
// ever stops asking the generator, every one of these goes red.
static void testRowMatchesTheGenerator(void)
{
	WorldGen g;
	CHECK(worldgenInit(&g, 12345u, GEN_VERSION_BIOME));
	debugBiomeSetWorldGen(&g);
	CHECK(debugBiomeKnown());

	int seen[BIOME_COUNT];
	memset(seen, 0, sizeof(seen));

	char buf[64], expect[64];
	// A wide, coarse sweep: GEN_BIOME_SHIFT is 7, so biome features are 128 blocks across
	// and a 64-block step visits many distinct cells rather than resampling one.
	for (int32_t x = -2048; x <= 2048; x += 64) {
		for (int32_t z = -2048; z <= 2048; z += 64) {
			const BiomeId b = worldgenBiomeAt(&g, x, z);
			snprintf(expect, sizeof(expect), "biome %s", debugBiomeName(b));
			if (strcmp(debugBiomeRow(x, z, buf, sizeof(buf)), expect) != 0) {
				CHECK(strcmp(buf, expect) == 0);
				return;   // one report, not 4225 of them
			}
			if ((int)b >= 0 && (int)b < BIOME_COUNT) seen[(int)b]++;
		}
	}
	CHECK(1);   // the sweep completed without a mismatch

	// Every biome actually reached, so the name table is exercised end to end rather than
	// one entry of it being checked 4000 times. A world where a biome never appears would
	// leave that row's label unproven.
	for (int i = 0; i < BIOME_COUNT; i++) {
		CHECK(seen[i] > 0);
		if (seen[i] == 0) printf("  (biome %d never sampled)\n", i);
	}

	debugBiomeSetWorldGen(NULL);
	CHECK(!debugBiomeKnown());
	CHECK(strcmp(debugBiomeRow(0, 0, buf, sizeof(buf)), BIOME_ROW_UNWIRED) == 0);
}

// The row shares a 320-px line with nothing else, but it is drawn at x=6 like every other
// status row in scene/ui.c's drawHudFont and must not run off the bottom screen.
// FONT_ADVANCE is 6 (gfx/font.h) and SCR_W is 320 (scene/ui_layout.h); both are written as
// bare literals here because gfx/font.h is behind <3ds.h> and cannot be included on the host.
static void testRowFitsTheScreen(void)
{
	WorldGen g;
	worldgenInit(&g, 777u, GEN_VERSION_BIOME);
	debugBiomeSetWorldGen(&g);

	char buf[64];
	size_t worst = strlen(BIOME_ROW_UNWIRED);
	for (int i = 0; i < BIOME_COUNT; i++) {
		char row[64];
		snprintf(row, sizeof(row), "biome %s", debugBiomeName((BiomeId)i));
		if (strlen(row) > worst) worst = strlen(row);
	}
	const int px = 6 + (int)worst * 6;   // x origin + advance per character
	CHECK(px <= 320);
	printf("biomeinfo: widest row %d chars = %d px of 320 (draw x=6)\n", (int)worst, px);

	// A caller that hands over a buffer too small must get a truncated, terminated string
	// rather than a smashed stack.
	char tiny[4];
	memset(tiny, 0x7F, sizeof(tiny));
	debugBiomeRow(0, 0, tiny, sizeof(tiny));
	CHECK(tiny[3] == '\0');
	CHECK(strlen(tiny) <= 3);

	// cap 0 must not be written to at all.
	char guard[2] = {0x5A, 0x5A};
	CHECK(debugBiomeRow(0, 0, guard, 0)[0] == '\0');
	CHECK(guard[0] == 0x5A);
	CHECK(debugBiomeRow(0, 0, NULL, sizeof(buf))[0] == '\0');

	debugBiomeSetWorldGen(NULL);
}

int main(void)
{
	testNames();
	testUnwired();
	testRowMatchesTheGenerator();
	testRowFitsTheScreen();

	if (g_fails) {
		printf("biomeinfo self-test: FAIL %d/%d  first: L%d\n",
		       g_fails, g_checks, g_first_line);
		return 1;
	}
	printf("biomeinfo self-test: PASS  %d checks\n", g_checks);
	return 0;
}

#endif  // !__3DS__
