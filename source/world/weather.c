// Implementation of the weather model declared in weather.h. See that header for the design
// rationale (determinism, statelessness, biome-awareness, the accumulation/melt safety argument,
// and the honest scoping of what this file does not attempt) -- this file is deliberately thin
// and does not repeat that reasoning, only the arithmetic it describes.
//
// ── Per-tick cost, at the Old 3DS's own render radius ─────────────────────────────────
//
// This is an ESTIMATE, not a measurement: nothing in this file's authority can wire a call into
// main.c's real tick loop to time it end to end, so what follows is arithmetic over the
// documented per-call costs, not a captured number. Labelled as such throughout.
//
// weatherTickColumn visits WEATHER_CELLS_PER_VISIT (4) cells per call. If a caller schedules one
// call per loaded column per tick it is due (world/tick.h's tickPeriodForDistSq/tickDue, exactly
// as this project's other per-tick systems already do), then at the Old 3DS's documented radius
// of 5 (121 loaded columns, per this task's own brief) roughly a third of columns are "near"
// (full rate) and the rest are "far" (1-in-10 ticks, TICK_FAR_PERIOD). Call it, generously, all
// 121 columns due on a given tick in the worst case (every far column's decimated tick landing
// on the same one) -- that is the ceiling, not the typical case: 121 columns * 4 cells =
// 484 calls to weatherStepCell in that single worst-case tick.
//
// Each weatherStepCell call costs, per its body below: one worldgenHeight() (a small fixed
// number of noise samples -- the same cost class worldgen.c already pays once per column at
// generation time, see worldgen.h's own header), one weatherAt() (one worldgenBiomeAt() plus one
// rngMix plus one rngHash3, all O(1) integer arithmetic), one or two worldGet() calls, and at
// most one worldSet() call. None of these allocate. worldSet only actually touches memory on the
// ticks where a cell's classification crosses the ACCUM or MELT threshold, which for any one
// cell happens at most twice per WEATHER_EPISODE_TICKS (8192 ticks, ~409 seconds) -- the
// overwhelming majority of weatherStepCell calls in ordinary play read four values and write
// nothing.
//
// Zero persistent bytes either way -- see weather.h's "stateless by construction" section. The
// worst-case 484-calls-in-one-tick figure above is an upper bound on WORK, not on memory, and it
// is a bound this file's own author estimated from counting operations, not something instrumented
// on real hardware; docs/plan-1.8.9-weather.md restates this with the same "estimate" label rather
// than upgrading it to a measurement it is not.
#include "world/weather.h"

#include "world/chunk.h"   // CHUNK_DIM -- weather.h only pulls this in transitively via world.h,
                            // and weatherTickColumn's local x/z split depends on it directly, so
                            // it is included explicitly rather than relied on by accident.
#include "world/rng.h"

// 'W','T','H','R' packed into one uint32_t, the same "spell it and pack it" idiom worldgenInit
// uses for its own per-purpose salts (documented in weather.h's own header: worldgenInit stores
// rngMix(seed ^ 'BLKS'), not the raw seed). Using a different salt than any other system in this
// project means weather's roll and, say, worldgen's own decoration rolls can never accidentally
// collide on the same (x, z) and agree for the wrong reason.
#define SALT_WEATHER 0x57544852u

// weatherTickColumn's local-x/z decomposition below assumes CHUNK_DIM is a power of two and
// specifically 16 (a 4-bit shift). If chunk.h's own CHUNK_DIM ever changes, this must be
// revisited -- fail the build loudly rather than silently sweep the wrong cells.
_Static_assert(CHUNK_DIM == 16,
               "weatherTickColumn hardcodes a 4-bit shift for CHUNK_DIM's local x/z split");

// Arithmetic right shift floors correctly for negative coordinates -- the same convention
// world/world.h documents for its own chunk-coordinate arithmetic ("Arithmetic shift gives the
// right floor behaviour for negative coordinates, which plain division does not").
static inline int32_t weatherFloorShift(int32_t v, int shift)
{
	return v >> shift;
}

WeatherKind weatherAt(const WorldGen* g, uint64_t tick, int32_t x, int32_t z)
{
	int32_t cellx = weatherFloorShift(x, WEATHER_CELL_SHIFT);
	int32_t cellz = weatherFloorShift(z, WEATHER_CELL_SHIFT);
	uint64_t episode = tick >> WEATHER_EPISODE_SHIFT;

	// Salt this generator's own opaque seed field once. Never re-derive a fresh WorldGen from
	// g->seed -- see weather.h's citation of the "324 columns of one flat row" failure mode this
	// project has already hit once from exactly that mistake elsewhere.
	uint32_t weather_seed = rngMix(g->seed ^ SALT_WEATHER);

	// One independent roll per (weather cell, episode). rngHash3 has no native 4-argument form,
	// so `episode` rides in the y slot -- z is still the true third spatial axis, passed in its
	// own argument. Only the low 32 bits of `episode` ever reach the hash; weather_test.c's
	// huge-tick sweep is the check that this does not silently create a repeating cadence within
	// any tick range this console will ever actually reach (episode wraps int32 only after
	// roughly 2^13 * 2^32 ticks, an untouchable number of real seconds at TICK_HZ 20).
	uint32_t roll = rngHash3(weather_seed, cellx, (int32_t)episode, cellz) & 0xFFu;

	BiomeId biome = worldgenBiomeAt(g, x, z);
	uint32_t chance = (biome == BIOME_DESERT) ? WEATHER_CHANCE_DESERT : WEATHER_CHANCE_PRECIP;

	if (roll >= chance) {
		return WEATHER_CLEAR;
	}

	// A front is passing through this cell. What it looks like depends on the biome sampled at
	// the EXACT block asked about (not just the cell's dominant biome), so a cell straddling a
	// biome boundary still answers consistently with worldgenBiomeAt's own per-block granularity.
	switch (biome) {
		case BIOME_TUNDRA:
		case BIOME_TAIGA:
			return WEATHER_SNOW;
		case BIOME_DESERT:
		case BIOME_PLAINS:
		case BIOME_FOREST:
		case BIOME_JUNGLE:
		default:
			return WEATHER_RAIN;
	}
}

BlockId weatherNextSurfaceBlock(WeatherKind kind, uint64_t ticks_into_episode, BlockId current)
{
	if (kind == WEATHER_SNOW && current == BLOCK_AIR &&
	    ticks_into_episode >= WEATHER_SNOW_ACCUM_TICKS) {
		return BLOCK_SNOW;
	}
	if (kind != WEATHER_SNOW && current == BLOCK_SNOW &&
	    ticks_into_episode >= WEATHER_MELT_TICKS) {
		return BLOCK_AIR;
	}
	return current;
}

bool weatherStepCell(const WorldGen* g, World* w, uint64_t tick, int32_t x, int32_t z)
{
	int32_t air_y = worldgenHeight(g, x, z);
	if (air_y < 0 || air_y >= WORLD_HEIGHT) {
		// Outside the world's own vertical range -- worldgenHeight promises a value within it
		// for any position worldgen actually generated, but this file makes no assumption about
		// what a caller might pass, and worldGet/worldSet's own out-of-range behaviour is not a
		// substitute for a real bounds check here since we are about to reason about the cell
		// ONE BELOW air_y too.
		return false;
	}

	BlockId below = worldGet(w, x, air_y - 1, z);
	if (!blockIsSolid(below)) {
		// The generated ground is gone (mined since worldgen ran) or air_y sits at the very
		// bottom of the world -- see weather.h's "why this is safe against a player having
		// edited the ground" section. Do nothing rather than float snow over a hole.
		return false;
	}

	BlockId current = worldGet(w, x, air_y, z);
	if (current != BLOCK_AIR && current != BLOCK_SNOW) {
		// Something else already occupies this cell -- a player placement, a plant, anything.
		// Never overwrite it.
		return false;
	}

	WeatherKind kind = weatherAt(g, tick, x, z);
	uint64_t ticks_into_episode = tick & (uint64_t)(WEATHER_EPISODE_TICKS - 1);
	BlockId next = weatherNextSurfaceBlock(kind, ticks_into_episode, current);
	if (next == current) {
		return false;
	}
	return worldSet(w, x, air_y, z, next);
}

int weatherTickColumn(const WorldGen* g, World* w, uint64_t tick, int32_t cx, int32_t cz)
{
	int32_t base_x = cx * CHUNK_DIM;
	int32_t base_z = cz * CHUNK_DIM;

	// CHUNK_DIM * CHUNK_DIM is 256, a power of two, so masking a cell index by (256 - 1) covers
	// every one of the column's surface cells with no bias -- unlike a modulo by a value that
	// does not divide the range, which would visit some cells more often than others.
	const uint32_t cell_count = (uint32_t)CHUNK_DIM * (uint32_t)CHUNK_DIM;
	uint32_t start = (uint32_t)(tick & (uint64_t)(cell_count - 1));

	int changed = 0;
	for (int i = 0; i < WEATHER_CELLS_PER_VISIT; ++i) {
		uint32_t cell = (start + (uint32_t)i) & (cell_count - 1);
		int32_t lx = (int32_t)(cell & (uint32_t)(CHUNK_DIM - 1));   // low 4 bits: local x
		int32_t lz = (int32_t)(cell >> 4);                          // next 4 bits: local z
		if (weatherStepCell(g, w, tick, base_x + lx, base_z + lz)) {
			++changed;
		}
	}
	return changed;
}
