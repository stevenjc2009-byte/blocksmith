#include "debug/biomeinfo.h"

#include <stdio.h>

// Indexed by BiomeId, so the order here IS world/worldgen.h's enum order and a shuffle is a
// wrong label on every row rather than a compile error. source/debug/biomeinfo_test.c pins
// each name to its enumerator by hand for that reason, and the _Static_assert below catches
// the other half of it — a biome added to the enum without a name added here.
//
// Lower case, one word, no padding: the readout it joins is "cols 81  chunks 435" and
// "meshes 235  tris 157692", and a "Taiga" or a "TAIGA" in the middle of that would read as
// a different program's output.
static const char* const BIOME_NAMES[] = {
	"tundra",   // BIOME_TUNDRA
	"taiga",    // BIOME_TAIGA
	"plains",   // BIOME_PLAINS
	"forest",   // BIOME_FOREST
	"desert",   // BIOME_DESERT
	"jungle",   // BIOME_JUNGLE
};

_Static_assert(sizeof(BIOME_NAMES) / sizeof(BIOME_NAMES[0]) == BIOME_COUNT,
               "BIOME_NAMES is out of step with BiomeId in world/worldgen.h — a biome was "
               "added to the enum without a name here");

// The live world, or NULL. See the placeholder note in biomeinfo.h: main.c is another lane's
// file, so nothing sets this yet and the row says so on screen rather than guessing.
static const WorldGen* s_gen;

const char* debugBiomeName(BiomeId b)
{
	// `b` is range-checked rather than trusted: it arrives from worldgenBiomeAt and from
	// callers that may be holding it in an int. "?" and not "unknown" because it shares a
	// 320-px line with the rest of the row.
	//
	// ONE unsigned comparison, deliberately, and not the obvious
	// `if ((int)b < 0 || (int)b >= BIOME_COUNT)`. That version compiles clean on the host and
	// FAILS THE CONSOLE BUILD: devkitARM's EABI defaults to -fshort-enums, so BiomeId is a
	// single byte on ARM, `(int)b < 0` can never be true, and -Werror=type-limits rejects it
	// with "comparison is always false due to limited range of data type". Measured — the
	// host stanza was green while arm-none-eabi-gcc returned rc=1 on this exact line.
	//
	// Casting to unsigned covers both ends in one test and is correct under either enum size:
	// a negative value passed in through an int-sized enum wraps to a huge unsigned and fails
	// the bound, and a byte-sized enum has already wrapped it to 255, which fails it too.
	if ((unsigned)b >= (unsigned)BIOME_COUNT) return "?";
	return BIOME_NAMES[(unsigned)b];
}

void debugBiomeSetWorldGen(const WorldGen* g) { s_gen = g; }

bool debugBiomeKnown(void) { return s_gen != NULL; }

const char* debugBiomeRow(int32_t x, int32_t z, char* buf, size_t cap)
{
	if (!buf || cap == 0) return "";

	if (!s_gen) {
		snprintf(buf, cap, "%s", BIOME_ROW_UNWIRED);
		return buf;
	}

	// worldgenBiomeAt is called HERE rather than the name being computed by the caller, so
	// the row and the world's own classifier cannot drift apart: there is exactly one
	// classification in this program and this is a use of it, not a copy of it.
	snprintf(buf, cap, "biome %s", debugBiomeName(worldgenBiomeAt(s_gen, x, z)));
	return buf;
}
