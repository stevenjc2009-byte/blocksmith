// Host self-test for a cross-module invariant that v1.8.16 introduced but never checked:
// every block that is FOOD must be UNPLACEABLE.
//
// ── The gap this file closes ─────────────────────────────────────────────────────────────
//
// v1.8.16 shipped a user-visible fix: steve reported it himself, "make the apple an item, not
// a block, similar to Minecraft" - apples and the eight meats stopped being things a player
// could put down in the world. world/placeable.h's own comment records why that fix is a
// hand-written list (kUnplaceable in world/placeable.c) rather than a registry flag or a
// change to the affected rows' SHAPE - both of those were closed off for reasons specific to
// this game (registry.h's flags byte has no spare bit; a CROSS apple would break and drop
// nothing, undoing the v1.8.8 apple supply).
//
// What nothing checks is that the two tables agree: world/survival.c's kFoods[] (which ids are
// food, and for how much hunger) and world/placeable.c's kUnplaceable[] (which ids refuse the
// place button) are separate arrays, hand-maintained, and NOTHING TIES THEM TOGETHER. A future
// edit to either - a new food added to kFoods and forgotten in kUnplaceable, or a registry
// change that makes a food's id carry different behaviour - can silently reopen exactly the
// bug steve reported, and every existing suite stays green, because none of them ever asks
// the cross-module question "is this food placeable?".
//
// ── Why this walks the WHOLE table and not the nine known foods ─────────────────────────────
//
// A hand-picked list of the nine current foods can only ever re-prove what is already true
// today. The failure mode this guards against is a TENTH food added later without its id also
// being added to kUnplaceable - and a test built from today's nine names would not see that
// row at all. Looping over every DEFINED row of the real registry (registryIsDefined(), the
// same predicate world/registry_test.c's coreHardnessIsDeclared() walks the core span with)
// means a new food lands inside this loop the moment registryInitCore() defines its row,
// whether or not this file is ever touched again.
//
// ── What is real and what is faked ──────────────────────────────────────────────────────
//
// Nothing is faked. This links the REAL world/survival.c (survivalFoodValue(), the exact
// function the running game asks "how much hunger does eating this restore") and the REAL
// world/placeable.c (itemIsPlaceable(), the exact function scene/interact.c's place path
// asks). Both read the REAL world/registry.c's compiled-in core table through
// registryInitCore()/registryIsDefined()/registryGet() - nothing here hand-copies either
// table's contents, which is the trap world/placeable.c's own comment names: "Two copies of
// one array agreeing with itself would prove nothing." This file does not carry a copy of
// either array; it calls both real functions and compares their answers.
//
// ── Confirmed before this file was written ───────────────────────────────────────────────
//
// A throwaway probe linking these same real files (not part of any suite) walked all 43
// defined rows of the core table and printed food value and placeability for each. Measured:
// rows=43, food_count=9 (apple, four raw cuts, four cooked cuts - matching the "nine ids"
// world/placeable.c's own comment names), violations=0. The invariant holds on the tree this
// file was written against; this suite is what keeps it holding.

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/placeable.h"
#include "world/registry.h"
#include "world/survival.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   %s\n", what);
	} else {
		printf("  ok     %s\n", what);
	}
}

// How many check() calls this suite makes on a healthy tree, per world/registry_test.c's
// REGISTRY_TEST_EXPECTED_CHECKS idiom: a literal, never computed from a production constant or
// a loop bound, so a check that silently stops running (a shrunk loop, an early continue) shows
// up as a missing check rather than as a quiet pass.
//
//   44   one check per defined core row (testFoodImpliesUnplaceable's loop body)
//    1   the loop ran over all 44 rows (not a truncated prefix, not zero)
//    1   the real food count is 9 (apple + four raw cuts + four cooked cuts)
//
// 44 + 1 + 1 = 46.
//
// v1.9.0 "Storage": 45 -> 46. One new core row, the chest at id 43, so the loop body runs once
// more. The food count does NOT move — a chest is not food — which is the useful half of this
// pin: a new row that had wrongly declared a food value would have moved BOTH numbers, and this
// one moved exactly one. Read off the run that went red as "CHECK COUNT: 1 check(s) were ADDED
// - expected 45, ran 46", not adjusted on paper.
#define FOOD_PLACEABLE_TEST_EXPECTED_CHECKS 46

static void checkCountPin(void)
{
	if (g_checks == FOOD_PLACEABLE_TEST_EXPECTED_CHECKS)
		return;

	g_fails++;
	if (g_checks < FOOD_PLACEABLE_TEST_EXPECTED_CHECKS)
		printf("  FAIL   CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "         They did not fail. They never ran: a loop bound shrank, an early\n"
		       "         return or a continue fired, or a check was deleted. The checks that\n"
		       "         did run passing tells you nothing about the ones that did not.\n",
		       FOOD_PLACEABLE_TEST_EXPECTED_CHECKS - g_checks,
		       FOOD_PLACEABLE_TEST_EXPECTED_CHECKS, g_checks);
	else
		printf("  FAIL   CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "         If this is a deliberate new food or a deliberate new core row, update\n"
		       "         FOOD_PLACEABLE_TEST_EXPECTED_CHECKS in\n"
		       "         tests/food_placeable_invariant_test.c to %d.\n",
		       g_checks - FOOD_PLACEABLE_TEST_EXPECTED_CHECKS,
		       FOOD_PLACEABLE_TEST_EXPECTED_CHECKS, g_checks, g_checks);
}

// THE invariant: for every defined block id, being food implies being unplaceable.
//
// Deliberately the CONTRAPOSITIVE of "the nine foods are unplaceable" - written as an
// implication over the whole table rather than an equality against nine names, because the
// implication is what a tenth food has to satisfy too, and an equality against nine names is
// exactly the hand-picked list the file header explains is the wrong shape for this check.
static void testFoodImpliesUnplaceable(void)
{
	puts("food/placeable: every defined block where survivalFoodValue() > 0 refuses "
	     "itemIsPlaceable()");

	registryInitCore();

	int rows = 0;
	int food_count = 0;

	for (int id = 0; id <= REG_ID_CORE_HI; id++) {
		if (!registryIsDefined((BlockId)id)) continue;
		rows++;

		const uint8_t food      = survivalFoodValue((BlockId)id);
		const bool    placeable = itemIsPlaceable((ItemId)id);
		const char   *name      = registryGet((BlockId)id)->name;

		if (food > 0) food_count++;

		char what[96];
		snprintf(what, sizeof what,
		         "id %d (%s): food=%u placeable=%d -- food>0 implies unplaceable",
		         id, name, (unsigned)food, (int)placeable);
		// The implication itself: food==0 makes it vacuously true (not food, placeability is
		// not this test's business); food>0 requires placeable to be false.
		check(food == 0 || !placeable, what);
	}

	// The loop ran, and over the whole core id space rather than a truncated prefix of it.
	// Without this a registry that answered registryIsDefined() false for everything would
	// leave every check above unrun and the suite would still print "0 failed" - a check that
	// cannot go red proves nothing, and world/registry_test.c's coreHardnessIsDeclared() states
	// the identical concern about its own loop.
	check(rows == 44, "and it ran over all 44 defined core rows (air + forty-three)");

	// The real count, not the count this file's comment header claims. If a food is ever added
	// to world/survival.c's kFoods[] without also reaching world/placeable.c's kUnplaceable[],
	// the per-row check above is what goes red - this check only pins how many foods exist
	// today, so a reader who sees it move to 10 (say) knows a new food landed and can go read
	// which row broke the loop above.
	check(food_count == 9, "and exactly 9 of them are food: the apple, four raw cuts, four "
	                        "cooked cuts");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== food/placeable invariant test ==");

	testFoodImpliesUnplaceable();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}
