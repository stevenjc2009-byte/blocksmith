#include "world/placeable.h"

#include <stddef.h>

#include "world/block.h"

// ── the food ids, and nothing else ──────────────────────────────────────────────────────
//
// See placeable.h for why this list exists here rather than as a registry flag or a change to
// the apple's row. This table is the whole rule.
//
// It is deliberately the SAME nine ids world/survival.c's kFoods table carries a hunger value
// for — apple, four raw cuts, four cooked ones — and that is not a coincidence to be tidied
// away into a call to survivalFoodValue(). Two reasons it stays a separate list:
//
//  1. They are different questions that happen to agree today. "Can this be eaten" and "can
//     this be put down" are independent properties, and the first food that is edible AND
//     placeable (a cake, a pie on a table) makes them disagree. Deriving one from the other now
//     would make that future row silently wrong instead of a one-line edit here.
//
//  2. It would put world/survival.c into three host-test link lines that do not have it
//     (scene/interact_test.c, tests/audio_cue_test.c, scene/craft_torch_e2e_test.c all link
//     scene/interact.c), buying a dependency for a nine-entry array.
//
// scene/interact_test.c hand-writes its own copy of these nine ids rather than including this
// header, precisely so the two lists can disagree and be caught. Two copies of one array
// agreeing with itself would prove nothing.
static const ItemId kUnplaceable[] = {
	// v1.8.8. The reported item: apples drop from oak and birch leaves and are eaten for 4
	// hunger. Worldgen still grows them as real cubes in canopies and they are still broken back
	// out of the world as themselves — only the PLAYER putting one down is refused.
	BLOCK_APPLE,

	// v1.8.14. The four raw cuts. A porkchop placeable as a solid cube of meat is the identical
	// wrongness the apple was reported for, so they are fixed in the same change rather than
	// left to be reported separately.
	BLOCK_RAW_PORKCHOP,
	BLOCK_RAW_BEEF,
	BLOCK_RAW_CHICKEN,
	BLOCK_RAW_MUTTON,

	// v1.8.15 "Furnace". The four cooked cuts, which exist only to be eaten — there is no other
	// use for one in this build at all.
	BLOCK_COOKED_PORKCHOP,
	BLOCK_COOKED_BEEF,
	BLOCK_COOKED_CHICKEN,
	BLOCK_COOKED_MUTTON,
};

bool itemIsPlaceable(ItemId item)
{
	// A linear scan over nine bytes, matching survivalFoodValue()'s shape for the same reason it
	// is right there: this runs once per press of the place button, not once per block per
	// frame, so there is nothing here worth a lookup table's static storage.
	for (size_t i = 0; i < sizeof kUnplaceable / sizeof kUnplaceable[0]; i++)
		if (kUnplaceable[i] == item)
			return false;

	return true;
}
