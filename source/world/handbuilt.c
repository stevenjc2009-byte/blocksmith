#include "world/handbuilt.h"

#include "world/block.h"

#define GROUND_Y      12   // flat base height

// How far the checker plateaus stand up. This is 1 and must stay <= 1 while the player
// can only climb one block: auto-step rises exactly one block, and a jump clears
// PLAYER_JUMP_SPEED^2 / (2 * -PLAYER_GRAVITY) = 8.5^2 / 56 = 1.29 blocks. At 2 every
// plateau edge was a one-way drop -- you could walk off but never back up -- which made
// the test world a set of pens rather than somewhere to walk around.
#define PLATEAU_STEP  1
#define PYRAMID_X0    20
#define PYRAMID_X1    32   // exclusive
#define PYRAMID_Z0    20
#define PYRAMID_Z1    32
// The pit sits wholly inside one 8x8 checker cell, at 1..6 rather than the 6..11 it used
// to occupy. That is not cosmetic. The checker's parity flips on multiples of 8, so the
// old range straddled a boundary in both x and z: at x=8,z=7 the plateau's +1 and the
// pit's next ring down landed on the same seam and added up to a two-block wall, which
// nothing can climb. Keeping the pit inside a single cell means every column around and
// inside it shares one parity, so the only height changes near it are the pit's own
// one-per-ring steps.
#define PIT_X0        1
#define PIT_X1        7    // exclusive
#define PIT_Z0        1
#define PIT_Z1        7
#define PIT_DEPTH     3

static int minInt(int a, int b) { return a < b ? a : b; }

int handbuiltHeight(int x, int z)
{
	int h = GROUND_Y;

	// 8x8 plateaus in a checker. The 8-block period is deliberate: chunk borders fall
	// on multiples of 16, so every chunk edge has a step running across it and a seam
	// would be obvious.
	if ((((x >> 3) + (z >> 3)) & 1) != 0)
		h += PLATEAU_STEP;

	// A stepped pyramid: convex corners, which is what baked AO has to darken.
	if (x >= PYRAMID_X0 && x < PYRAMID_X1 && z >= PYRAMID_Z0 && z < PYRAMID_Z1) {
		const int dx = minInt(x - PYRAMID_X0, PYRAMID_X1 - 1 - x);
		const int dz = minInt(z - PYRAMID_Z0, PYRAMID_Z1 - 1 - z);
		h += minInt(dx, dz);
	}

	// A pit: concave corners, the other half of the AO check. Its walls are stepped one
	// block per ring, for the same reason PLATEAU_STEP is 1 -- with sheer walls three
	// blocks deep, anything that fell in was stuck there for good, because a jump reaches
	// 1.29 blocks and auto-step climbs one. Stepping the walls keeps the concave corners
	// the AO check needs (now three nested ones instead of a single deep box) while
	// leaving a walkable way out. The floor is still PIT_DEPTH below the surface.
	if (x >= PIT_X0 && x < PIT_X1 && z >= PIT_Z0 && z < PIT_Z1) {
		const int dx = minInt(x - PIT_X0, PIT_X1 - 1 - x);
		const int dz = minInt(z - PIT_Z0, PIT_Z1 - 1 - z);
		const int ring = minInt(dx, dz);            // 0 at the rim, growing inwards
		h -= minInt(PIT_DEPTH, ring + 1);
	}

	return h;
}

bool handbuiltFill(World* w)
{
	bool ok = true;

	for (int z = 0; z < HANDBUILT_BLOCKS_Z; z++) {
		for (int x = 0; x < HANDBUILT_BLOCKS_X; x++) {
			const int h = handbuiltHeight(x, z);

			for (int y = 0; y < h; y++) {
				BlockId id = BLOCK_STONE;
				if (y == h - 1)      id = BLOCK_GRASS;
				else if (y >= h - 4) id = BLOCK_DIRT;

				// Sand lines the pit floor, so a wrong per-face tile is visible as a
				// patch of the wrong colour rather than needing a pixel comparison.
				if (x >= PIT_X0 && x < PIT_X1 && z >= PIT_Z0 && z < PIT_Z1 && y == h - 1)
					id = BLOCK_SAND;

				ok = worldSet(w, x, y, z, id) && ok;
			}
		}
	}

	return ok;
}
