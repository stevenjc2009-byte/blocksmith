// Host self-test for scene/minimap.c — fog bitmap, exploration radius, save/load
// round-trip, wrong-magic/version rejection, and per-block colour lookup.
//
// Same shape as scene/loading_test.c: own main(), whole file behind #ifndef __3DS__
// so the console build's glob of source/scene does not link two main()s.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "scene/minimap.h"

// Duplicate of gfx/sprite.h's SPRITE_RGBA — that header pulls in <citro3d.h> which
// is unavailable on the host; this macro is pure integer arithmetic and identical.
#ifndef SPRITE_RGBA
#define SPRITE_RGBA(r, g, b, a) \
	((uint32_t)(uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | \
	 ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(a) << 24))
#endif

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

// ── Fog bit operations ─────────────────────────────────────────────────────────────────

static void testFogInitiallyClear(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	for (int i = 0; i < MINIMAP_SIZE * MINIMAP_SIZE; i++)
		CHECK(!minimapFogGet(&f, i));
}

static void testFogSetAndGet(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogSet(&f, 0);
	minimapFogSet(&f, 63);
	minimapFogSet(&f, 2048);
	minimapFogSet(&f, 4095);
	CHECK(minimapFogGet(&f, 0));
	CHECK(minimapFogGet(&f, 63));
	CHECK(minimapFogGet(&f, 2048));
	CHECK(minimapFogGet(&f, 4095));
	CHECK(!minimapFogGet(&f, 1));
}

static void testFogClearResets(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogSet(&f, 100);
	minimapFogSet(&f, 200);
	minimapFogClear(&f);
	CHECK(!minimapFogGet(&f, 100));
	CHECK(!minimapFogGet(&f, 200));
}

static void testFogIndexBounds(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogSet(&f, -1);
	minimapFogSet(&f, 4096);
	minimapFogSet(&f, 9999);
	CHECK(!minimapFogGet(&f, -1));
	CHECK(!minimapFogGet(&f, 4096));
	CHECK(!minimapFogGet(&f, 9999));
}

// ── Exploration radius ─────────────────────────────────────────────────────────────────

static void testExploreRadiusSetsNeighbours(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogUpdate(&f, 32, 32, 0, 0);
	const int r2 = MINIMAP_EXPLORED * MINIMAP_EXPLORED;
	for (int z = 0; z < MINIMAP_SIZE; z++)
		for (int x = 0; x < MINIMAP_SIZE; x++) {
			int dx = x - 32, dz = z - 32;
			if (dx * dx + dz * dz <= r2)
				CHECK(minimapFogGet(&f, z * MINIMAP_SIZE + x));
		}
}

static void testExploreRadiusExcludesFar(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogUpdate(&f, 32, 32, 0, 0);
	const int r2 = MINIMAP_EXPLORED * MINIMAP_EXPLORED;
	for (int i = 0; i < MINIMAP_SIZE * MINIMAP_SIZE; i++) {
		int x = i % MINIMAP_SIZE, z = i / MINIMAP_SIZE;
		int dx = x - 32, dz = z - 32;
		if (dx * dx + dz * dz > r2)
			CHECK(!minimapFogGet(&f, i));
	}
}

static void testExploreRadiusClamps(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogUpdate(&f, -5, -5, 0, 0);
	CHECK(minimapFogGet(&f, 0));
	for (int i = 0; i < MINIMAP_SIZE * MINIMAP_SIZE; i++) {
		int x = i % MINIMAP_SIZE, z = i / MINIMAP_SIZE;
		int dx = x + 5, dz = z + 5;
		if (dx * dx + dz * dz > MINIMAP_EXPLORED * MINIMAP_EXPLORED)
			CHECK(!minimapFogGet(&f, i));
	}
}

static void testExploreOffset(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	minimapFogUpdate(&f, 100, 100, 64, 64);
	// player at world (100,100), offset (64,64) → minimap cell (36,36)
	const int r2 = MINIMAP_EXPLORED * MINIMAP_EXPLORED;
	for (int z = 0; z < MINIMAP_SIZE; z++)
		for (int x = 0; x < MINIMAP_SIZE; x++) {
			int dx = x - 36, dz = z - 36;
			if (dx * dx + dz * dz <= r2)
				CHECK(minimapFogGet(&f, z * MINIMAP_SIZE + x));
		}
}

// ── Save / load round-trip ─────────────────────────────────────────────────────────────

static void testSaveLoadRoundTrip(void)
{
	MinimapFog a, b;
	minimapFogClear(&a);
	minimapFogClear(&b);
	minimapFogSet(&a, 0);
	minimapFogSet(&a, 1023);
	minimapFogSet(&a, 4095);
	minimapFogUpdate(&a, 50, 50, 0, 0);

	CHECK(minimapSave(&a, "build-host/_minimap_test.bin"));
	CHECK(minimapLoad(&b, "build-host/_minimap_test.bin"));
	for (int i = 0; i < MINIMAP_SIZE * MINIMAP_SIZE; i++)
		CHECK(minimapFogGet(&a, i) == minimapFogGet(&b, i));
	remove("build-host/_minimap_test.bin");
}

static void testLoadMissingFileReturnsZero(void)
{
	MinimapFog f;
	minimapFogSet(&f, 100);
	CHECK(minimapLoad(&f, "build-host/_no_minimap_file.bin"));
	CHECK(!minimapFogGet(&f, 100));
}

static void testLoadWrongMagicReturnsFalse(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	FILE* fp = fopen("build-host/_minimap_bad_magic.bin", "wb");
	uint32_t bad = 0xDEADBEEF;
	fwrite(&bad, 4, 1, fp);
	fclose(fp);
	CHECK(!minimapLoad(&f, "build-host/_minimap_bad_magic.bin"));
	remove("build-host/_minimap_bad_magic.bin");
}

static void testLoadWrongVersionReturnsFalse(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	FILE* fp = fopen("build-host/_minimap_bad_ver.bin", "wb");
	uint32_t magic = MINIMAP_SAVE_MAGIC, bad = 99;
	fwrite(&magic, 4, 1, fp);
	fwrite(&bad, 4, 1, fp);
	fclose(fp);
	CHECK(!minimapLoad(&f, "build-host/_minimap_bad_ver.bin"));
	remove("build-host/_minimap_bad_ver.bin");
}

static void testLoadTruncatedReturnsFalse(void)
{
	MinimapFog f;
	minimapFogClear(&f);
	FILE* fp = fopen("build-host/_minimap_short.bin", "wb");
	uint32_t magic = MINIMAP_SAVE_MAGIC, ver = MINIMAP_SAVE_VER;
	fwrite(&magic, 4, 1, fp);
	fwrite(&ver, 4, 1, fp);
	// Missing fog data entirely.
	fclose(fp);
	CHECK(!minimapLoad(&f, "build-host/_minimap_short.bin"));
	remove("build-host/_minimap_short.bin");
}

// ── Colour lookup ──────────────────────────────────────────────────────────────────────

static void testBlockColors(void)
{
	CHECK(minimapBlockColor(BLOCK_AIR)    == 0x00000000u);
	CHECK(minimapBlockColor(BLOCK_GRASS)  == SPRITE_RGBA(76, 140, 48, 255));
	CHECK(minimapBlockColor(BLOCK_DIRT)   == SPRITE_RGBA(134, 96, 67, 255));
	CHECK(minimapBlockColor(BLOCK_STONE)  == SPRITE_RGBA(128, 128, 128, 255));
	CHECK(minimapBlockColor(BLOCK_SAND)   == SPRITE_RGBA(218, 200, 152, 255));
	CHECK(minimapBlockColor(BLOCK_WOOD)   == SPRITE_RGBA(101, 72, 46, 255));
	CHECK(minimapBlockColor(BLOCK_LEAVES) == SPRITE_RGBA(44, 120, 32, 255));
	CHECK(minimapBlockColor(BLOCK_PLANKS) == SPRITE_RGBA(160, 120, 72, 255));
	CHECK(minimapBlockColor(BLOCK_COUNT)  == SPRITE_RGBA(255, 0, 255, 255));
	CHECK(minimapBlockColor(250)          == SPRITE_RGBA(255, 0, 255, 255));
}

// ── Main ───────────────────────────────────────────────────────────────────────────────

int main(void)
{
	testFogInitiallyClear();
	testFogSetAndGet();
	testFogClearResets();
	testFogIndexBounds();
	testExploreRadiusSetsNeighbours();
	testExploreRadiusExcludesFar();
	testExploreRadiusClamps();
	testExploreOffset();
	testSaveLoadRoundTrip();
	testLoadMissingFileReturnsZero();
	testLoadWrongMagicReturnsFalse();
	testLoadWrongVersionReturnsFalse();
	testLoadTruncatedReturnsFalse();
	testBlockColors();

	if (s_fails == 0)
		printf("minimap self-test: PASS  %d checks\n", s_checks);
	else
		printf("minimap self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int minimap_test_host_only_t;

#endif   // !__3DS__
