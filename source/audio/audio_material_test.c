// Host self-test for source/audio/audio_material.c/.h — v1.8.19 "per-material sound".
//
// Two things need proving, and they are split the same way the two files under test are
// split:
//
//   1. sfxMaterialOfBlock() (audio_material.c) answers the mapping the task brief asked
//      for, id by id, and answers SFX_MAT_GENERIC for everything the brief said should be
//      generic — air, water, the eight food blocks, and an id nobody has ever taught it
//      about.
//   2. audioSfxFootstepSlot()/audioSfxBreakSlot()/audioSfxPlaceSlot() (audio_sfx.c) resolve
//      a material to a slot correctly in BOTH directions: the per-material slot when it was
//      registered, and the pre-1.8.19 generic slot when it was not — the fallback is the
//      one thing about this feature that is easy to get right for the happy path and wrong
//      for the case that actually ships first (romfs missing a clip, or a load failing).
//
// Reuses audio_sfx_test.c's CHECK macro shape, which itself reuses audio_bsnd_test.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "audio/audio_material.h"
#include "audio/audio_sfx.h"
#include "world/block.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                               \
		if (!(cond)) {                                                            \
			s_fails++;                                                            \
			if (!s_first[0])                                                      \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── sfxMaterialOfBlock() ─────────────────────────────────────────────────────────────

static void testStoneBlocksMapToStone(void)
{
	CHECK(sfxMaterialOfBlock(BLOCK_STONE)       == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_COAL_ORE)    == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_IRON_ORE)    == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_GOLD_ORE)    == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_REDSTONE_ORE)== SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_LAPIS_ORE)   == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_DIAMOND_ORE) == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_FURNACE)     == SFX_MAT_STONE);
	CHECK(sfxMaterialOfBlock(BLOCK_ICE)         == SFX_MAT_STONE);
}

static void testWoodBlocksMapToWood(void)
{
	CHECK(sfxMaterialOfBlock(BLOCK_WOOD)           == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_PLANKS)         == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_BIRCH_LOG)      == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_BIRCH_PLANKS)   == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_SPRUCE_LOG)     == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_SPRUCE_PLANKS)  == SFX_MAT_WOOD);
	CHECK(sfxMaterialOfBlock(BLOCK_TORCH)          == SFX_MAT_WOOD);
}

static void testDirtBlocksMapToDirt(void)
{
	CHECK(sfxMaterialOfBlock(BLOCK_DIRT) == SFX_MAT_DIRT);
	CHECK(sfxMaterialOfBlock(BLOCK_SAND) == SFX_MAT_DIRT);
	CHECK(sfxMaterialOfBlock(BLOCK_SNOW) == SFX_MAT_DIRT);
}

static void testGrassBlocksMapToGrass(void)
{
	CHECK(sfxMaterialOfBlock(BLOCK_GRASS)          == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_LEAVES)         == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_BIRCH_LEAVES)   == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_SPRUCE_LEAVES)  == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_TALL_GRASS)     == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_TALL_GRASS_TOP) == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_FERN)           == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_DEAD_BUSH)      == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_CACTUS)         == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_POPPY)          == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_DAISY)          == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_BLUEBELL)       == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_ORCHID)         == SFX_MAT_GRASS);
	CHECK(sfxMaterialOfBlock(BLOCK_APPLE)          == SFX_MAT_GRASS);
}

// air, water, the eight food blocks, and an id no version of this game has ever defined —
// all five have nothing in common except that none of them should sound like rock, wood,
// dirt or grass, and this is the one test that would catch a food block accidentally
// falling into BLOCK_APPLE's grass case if the two were ever reordered near each other.
static void testUnmappedFoodAirAndWaterAreGeneric(void)
{
	CHECK(sfxMaterialOfBlock(BLOCK_AIR)   == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_WATER) == SFX_MAT_GENERIC);

	CHECK(sfxMaterialOfBlock(BLOCK_RAW_PORKCHOP)    == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_RAW_BEEF)        == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_RAW_CHICKEN)     == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_RAW_MUTTON)      == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_COOKED_PORKCHOP) == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_COOKED_BEEF)     == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_COOKED_CHICKEN)  == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock(BLOCK_COOKED_MUTTON)   == SFX_MAT_GENERIC);

	// One past the last core id this tree defines (BLOCK_FURNACE == 42) and one from deep
	// in the dynamic range a server can register — neither has ever been taught to the
	// switch in audio_material.c, which is exactly the case its `default:` exists for.
	CHECK(sfxMaterialOfBlock((BlockId)43)  == SFX_MAT_GENERIC);
	CHECK(sfxMaterialOfBlock((BlockId)0x90) == SFX_MAT_GENERIC);
}

// ── The three resolvers (audio_sfx.c) ───────────────────────────────────────────────

static void testFootstepResolverFallsBackWhenUnregistered(void)
{
	audioSfxReset();
	// Only the generic slot is registered — none of the twelve per-material ones. Every
	// non-generic material must still resolve to something playable: the generic slot.
	audioSfxRegister(SFX_FOOTSTEP, 1);

	CHECK(audioSfxFootstepSlot(SFX_MAT_STONE) == SFX_FOOTSTEP);
	CHECK(audioSfxFootstepSlot(SFX_MAT_WOOD)  == SFX_FOOTSTEP);
	CHECK(audioSfxFootstepSlot(SFX_MAT_DIRT)  == SFX_FOOTSTEP);
	CHECK(audioSfxFootstepSlot(SFX_MAT_GRASS) == SFX_FOOTSTEP);
	CHECK(audioSfxFootstepSlot(SFX_MAT_GENERIC) == SFX_FOOTSTEP);

	audioSfxReset();
}

static void testFootstepResolverPrefersRegisteredMaterialSlot(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_FOOTSTEP,       1);
	audioSfxRegister(SFX_FOOTSTEP_STONE, 2);
	audioSfxRegister(SFX_FOOTSTEP_WOOD,  3);
	audioSfxRegister(SFX_FOOTSTEP_DIRT,  4);
	audioSfxRegister(SFX_FOOTSTEP_GRASS, 5);

	CHECK(audioSfxFootstepSlot(SFX_MAT_STONE) == SFX_FOOTSTEP_STONE);
	CHECK(audioSfxFootstepSlot(SFX_MAT_WOOD)  == SFX_FOOTSTEP_WOOD);
	CHECK(audioSfxFootstepSlot(SFX_MAT_DIRT)  == SFX_FOOTSTEP_DIRT);
	CHECK(audioSfxFootstepSlot(SFX_MAT_GRASS) == SFX_FOOTSTEP_GRASS);
	// GENERIC always plays the generic slot, registered material clips or not — there is
	// no SFX_FOOTSTEP_GENERIC to prefer instead.
	CHECK(audioSfxFootstepSlot(SFX_MAT_GENERIC) == SFX_FOOTSTEP);

	audioSfxReset();
}

static void testBreakResolverFallsBackWhenUnregistered(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_BREAK, 1);

	CHECK(audioSfxBreakSlot(SFX_MAT_STONE) == SFX_BLOCK_BREAK);
	CHECK(audioSfxBreakSlot(SFX_MAT_WOOD)  == SFX_BLOCK_BREAK);
	CHECK(audioSfxBreakSlot(SFX_MAT_DIRT)  == SFX_BLOCK_BREAK);
	CHECK(audioSfxBreakSlot(SFX_MAT_GRASS) == SFX_BLOCK_BREAK);

	audioSfxReset();
}

static void testBreakResolverPrefersRegisteredMaterialSlot(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_BREAK, 1);
	audioSfxRegister(SFX_BREAK_STONE, 2);
	audioSfxRegister(SFX_BREAK_WOOD,  3);
	audioSfxRegister(SFX_BREAK_DIRT,  4);
	audioSfxRegister(SFX_BREAK_GRASS, 5);

	CHECK(audioSfxBreakSlot(SFX_MAT_STONE) == SFX_BREAK_STONE);
	CHECK(audioSfxBreakSlot(SFX_MAT_WOOD)  == SFX_BREAK_WOOD);
	CHECK(audioSfxBreakSlot(SFX_MAT_DIRT)  == SFX_BREAK_DIRT);
	CHECK(audioSfxBreakSlot(SFX_MAT_GRASS) == SFX_BREAK_GRASS);
	CHECK(audioSfxBreakSlot(SFX_MAT_GENERIC) == SFX_BLOCK_BREAK);

	audioSfxReset();
}

static void testPlaceResolverFallsBackWhenUnregistered(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_PLACE, 1);

	CHECK(audioSfxPlaceSlot(SFX_MAT_STONE) == SFX_BLOCK_PLACE);
	CHECK(audioSfxPlaceSlot(SFX_MAT_WOOD)  == SFX_BLOCK_PLACE);
	CHECK(audioSfxPlaceSlot(SFX_MAT_DIRT)  == SFX_BLOCK_PLACE);
	CHECK(audioSfxPlaceSlot(SFX_MAT_GRASS) == SFX_BLOCK_PLACE);

	audioSfxReset();
}

static void testPlaceResolverPrefersRegisteredMaterialSlot(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_PLACE, 1);
	audioSfxRegister(SFX_PLACE_STONE, 2);
	audioSfxRegister(SFX_PLACE_WOOD,  3);
	audioSfxRegister(SFX_PLACE_DIRT,  4);
	audioSfxRegister(SFX_PLACE_GRASS, 5);

	CHECK(audioSfxPlaceSlot(SFX_MAT_STONE) == SFX_PLACE_STONE);
	CHECK(audioSfxPlaceSlot(SFX_MAT_WOOD)  == SFX_PLACE_WOOD);
	CHECK(audioSfxPlaceSlot(SFX_MAT_DIRT)  == SFX_PLACE_DIRT);
	CHECK(audioSfxPlaceSlot(SFX_MAT_GRASS) == SFX_PLACE_GRASS);
	CHECK(audioSfxPlaceSlot(SFX_MAT_GENERIC) == SFX_BLOCK_PLACE);

	audioSfxReset();
}

// A slot registered but PARTIALLY — only some of the four materials shipped a clip — must
// fall back per-material, not all-or-nothing. This is the shape a real build is likely to
// be in mid-rollout (e.g. stone and wood clips landed, dirt and grass have not yet), and it
// is the one case none of the tests above exercises: each of them registers either all four
// material slots or none of them.
static void testResolverFallsBackPerMaterialNotAllOrNothing(void)
{
	audioSfxReset();
	audioSfxRegister(SFX_BLOCK_BREAK, 1);
	audioSfxRegister(SFX_BREAK_STONE, 2);
	// SFX_BREAK_WOOD deliberately left unregistered.

	CHECK(audioSfxBreakSlot(SFX_MAT_STONE) == SFX_BREAK_STONE);   // shipped: use it
	CHECK(audioSfxBreakSlot(SFX_MAT_WOOD)  == SFX_BLOCK_BREAK);   // missing: fall back

	audioSfxReset();
}

int main(void)
{
	testStoneBlocksMapToStone();
	testWoodBlocksMapToWood();
	testDirtBlocksMapToDirt();
	testGrassBlocksMapToGrass();
	testUnmappedFoodAirAndWaterAreGeneric();

	testFootstepResolverFallsBackWhenUnregistered();
	testFootstepResolverPrefersRegisteredMaterialSlot();
	testBreakResolverFallsBackWhenUnregistered();
	testBreakResolverPrefersRegisteredMaterialSlot();
	testPlaceResolverFallsBackWhenUnregistered();
	testPlaceResolverPrefersRegisteredMaterialSlot();
	testResolverFallsBackPerMaterialNotAllOrNothing();

	if (s_fails == 0)
		printf("audio material self-test: PASS  %d checks\n", s_checks);
	else
		printf("audio material self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

typedef int audio_material_test_host_only_t;

#endif   // !__3DS__
