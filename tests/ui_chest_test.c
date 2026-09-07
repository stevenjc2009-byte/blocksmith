// Host self-test for the chest panel in source/scene/ui.c (v1.9.0 CHEST) — the two-tap
// lift/place TRANSFER SEMANTICS, what the panel DRAWS, and the v1.9.0 CHEST-NET transfer
// callback, driven through the real uiUpdateDraw() with real taps, against the real
// ChestState and the real Inventory. Own main(); lives under tests/ so the console Makefile's
// source/ glob never sees it, the same placement rule as every other tests/*_test.c.
//
// ── Why this suite exists ────────────────────────────────────────────────────────────────
//
// scene/ui_layout_test.c proves the chest panel's GEOMETRY (rects, hit-tests, and
// uiMoveStackInto()'s arithmetic) and world/chest_test.c proves pack/unpack. Neither ever
// called uiUpdateDraw() with a chest open, so until this file nothing had checked that a tap
// on the panel moves the stack it says it moves, that a swap swaps, that a refused transfer
// leaves BOTH sides alone and the lift in hand, or that what the player put in a chest is
// still there after the panel closes and reopens. This codebase has already shipped a feature
// that was green and inert (v1.9.0's full-bag guard: every test registered its own spy,
// main.c's registration was missing), which is the failure shape this suite is built against:
// it links the REAL scene/ui.c, ui_layout.c, inventory.c, chest.c, blockstate.c and
// net/inv_bridge.c, and stubs only what cannot exist on a host — the GPU sprite batch, the
// font, the audio mixer, the transport — see tests/ui_chest_stub/. There is no second copy of
// any rule in here to agree with itself.
//
// Every transfer check asserts SLOT STATE — which item, how many, on which side — never a
// return code, because the transfers under test return nothing; and every transfer, the
// refused ones included, is followed by a conservation check: units of each item across
// bag + chest are what they were before the taps.
//
// ── Taps ──────────────────────────────────────────────────────────────────────────────────
//
// uiUpdateDraw() fires a tap on the RISING edge of UiInput.touch_down (ui.c's `tap`), so a
// tap here is two frames: touch down at the point, then touch up, so the next tap is a fresh
// edge. A transfer is therefore TWO taps: one on the source slot (lift), one on the
// destination (place). Every point is the CENTRE of a rect from scene/ui_layout.h — the same
// chestSlotRect()/chestGridSlotRect()/hotbarSlotRect()/chestCloseRect() ui.c hit-tests
// against — so if the layout moves, the taps move with it and this suite keeps testing
// transfers rather than pixels.
//
// block_icons is NULL for the transfer cases (scene/ui.h documents NULL as legal: the atlas
// pass is skipped) and stats is NULL (same header: the metrics block is not drawn). Neither
// touches a transfer. The draw and quad-budget cases pass a dummy texture so the atlas pass
// runs and is counted.
//
// ── The one piece of main.c restated here ─────────────────────────────────────────────────
//
// testContentsSurviveCloseAndReopen needs the frame glue main.c wraps around uiUpdateDraw()
// for a chest — unpack from world/blockstate.c if open, call, pack back, "closed once the
// screen is no longer UI_SCR_CHEST". main.c cannot be linked (it carries main() and <3ds.h>),
// so hostFrame() below restates those four steps and nothing else, in main.c's order, so they
// can be checked against it by eye. It is the only logic in this file that is a copy rather
// than a link.
#include "scene/ui.h"

#include <stdio.h>
#include <string.h>

#include "proto/bs_proto.h"   // BS_INV_OP_MOVE: what a bag -> bag move reports through inv_bridge
#include "scene/ui_gesture.h"  // UI_GESTURE_KEY_X / _Y: pressing them for real, see below
#include "scene/ui_layout.h"
#include "ui_chest_stub.h"
#include "world/block.h"
#include "world/blockstate.h"
#include "world/chest.h"
#include "world/furnace.h"    // FurnaceState: X's "not into the furnace" screen, see below
#include "world/inventory.h"
#include "world/registry.h"

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

// How many CHECK()s this suite makes on a healthy tree. A LITERAL on purpose, never derived
// from CHEST_SLOTS, INV_SLOT_COUNT or anything else the code under test can move — see
// world/blockstate_test.c's own comment on its equivalent pin: a check that silently stops
// running must show up as a missing check, not a quiet pass.
//
// Seeded at a placeholder 1 first; the first green run's "CHECK COUNT: N check(s) were ADDED"
// line is what set it — see tools/run_host_tests.sh's ui_chest stanza for the measured line.
#define UI_CHEST_TEST_EXPECTED_CHECKS 437

// gfx/sprite.c's per-frame quad ceiling, restated as a literal for the same reason as the pin
// above: sprite.c cannot be linked here (it is the GPU batch), and the number is the budget
// this panel has to fit under. Its counter wraps silently past this, so nothing on the
// console would ever report an overrun — this suite is where it is measured.
#define SPRITE_MAX_QUADS_RESTATED 1024

static void checkCountPin(void)
{
	if (s_checks == UI_CHEST_TEST_EXPECTED_CHECKS)
		return;

	s_fails++;
	if (s_checks < UI_CHEST_TEST_EXPECTED_CHECKS) {
		printf("CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
		       "  They did not fail. They never ran. Find them. Do NOT re-pin\n"
		       "  UI_CHEST_TEST_EXPECTED_CHECKS to go green.\n",
		       UI_CHEST_TEST_EXPECTED_CHECKS - s_checks,
		       UI_CHEST_TEST_EXPECTED_CHECKS, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks MISSING (expected %d, ran %d) - see above",
			         UI_CHEST_TEST_EXPECTED_CHECKS - s_checks,
			         UI_CHEST_TEST_EXPECTED_CHECKS, s_checks);
	} else {
		printf("CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
		       "  If you added them on purpose, set UI_CHEST_TEST_EXPECTED_CHECKS in\n"
		       "  tests/ui_chest_test.c to %d.\n",
		       s_checks - UI_CHEST_TEST_EXPECTED_CHECKS,
		       UI_CHEST_TEST_EXPECTED_CHECKS, s_checks, s_checks);
		if (!s_first[0])
			snprintf(s_first, sizeof(s_first),
			         "CHECK COUNT: %d checks ADDED (expected %d, ran %d) - re-pin to %d",
			         s_checks - UI_CHEST_TEST_EXPECTED_CHECKS,
			         UI_CHEST_TEST_EXPECTED_CHECKS, s_checks, s_checks);
	}
}

// ── Driving the real uiUpdateDraw ─────────────────────────────────────────────────────────

// Where the chest "is" for every case that does not care: distinct, non-zero, one negative,
// so a callback handed (0, 0, 0) — or the coordinates in the wrong order — shows up.
#define CHEST_X   5
#define CHEST_Y   70
#define CHEST_Z   (-12)

static UiResult frameWithIcons(UiState* ui, Inventory* inv, ChestState* chest, C3D_Tex* icons,
                               const UiStats* stats, bool down, int x, int y)
{
	const UiInput in = { down, x, y, 0 };   // v1.9.0 SPLIT: keys_held — no key held in this test
	return uiUpdateDraw(ui, inv, icons, stats, &in, NULL, chest);
}

static UiResult frame(UiState* ui, Inventory* inv, ChestState* chest, bool down, int x, int y)
{
	return frameWithIcons(ui, inv, chest, NULL, NULL, down, x, y);
}

// One tap: touch down at (x, y), then touch up. Returns what the DOWN frame reported.
static UiResult tapAt(UiState* ui, Inventory* inv, ChestState* chest, int x, int y)
{
	const UiResult r = frame(ui, inv, chest, true, x, y);
	(void)frame(ui, inv, chest, false, 0, 0);
	return r;
}

static UiResult tapRect(UiState* ui, Inventory* inv, ChestState* chest, URect r)
{
	return tapAt(ui, inv, chest, r.x + r.w / 2, r.y + r.h / 2);
}

// The rect a BAG slot is drawn at on the chest screen: hotbar in its usual place, the main
// grid relocated to CHEST_GRID_Y (scene/ui_layout.h's chest panel block).
static URect bagRectOnChestScreen(int slot)
{
	return (slot < INV_HOTBAR_SLOTS) ? hotbarSlotRect(slot)
	                                 : chestGridSlotRect(slot - INV_HOTBAR_SLOTS);
}

// A UiState on the chest screen, the way main.c gets there: uiInit() then uiOpenChest().
static void openChest(UiState* ui)
{
	uiInit(ui);
	uiOpenChest(ui, CHEST_X, CHEST_Y, CHEST_Z);
}

static bool nothingLifted(const UiState* ui)
{
	return ui->picked_slot < 0 && ui->picked_chest < 0;
}

// ── Slot fixtures and state readers ───────────────────────────────────────────────────────

static void setBag(Inventory* inv, int slot, ItemId item, uint8_t count)
{
	inv->slots[slot].item  = item;
	inv->slots[slot].count = count;
}

static void fillBag(Inventory* inv, ItemId item, uint8_t count)
{
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		setBag(inv, i, item, count);
}

static void setChest(ChestState* cs, int slot, ItemId item, uint8_t count)
{
	cs->item[slot]  = item;
	cs->count[slot] = count;
}

static void fillChest(ChestState* cs, ItemId item, uint8_t count)
{
	for (int i = 0; i < CHEST_SLOTS; i++)
		setChest(cs, i, item, count);
}

static bool bagSlotIs(const Inventory* inv, int slot, ItemId item, uint8_t count)
{
	return inv->slots[slot].item == item && inv->slots[slot].count == count;
}

static bool chestSlotIs(const ChestState* cs, int slot, ItemId item, uint8_t count)
{
	return cs->item[slot] == item && cs->count[slot] == count;
}

// Field by field rather than memcmp, so a padding byte can never make two equal bags differ.
static bool bagEq(const Inventory* a, const Inventory* b)
{
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		if (a->slots[i].item != b->slots[i].item || a->slots[i].count != b->slots[i].count)
			return false;
	return a->selected_hotbar == b->selected_hotbar;
}

static bool chestEq(const ChestState* a, const ChestState* b)
{
	for (int i = 0; i < CHEST_SLOTS; i++)
		if (a->item[i] != b->item[i] || a->count[i] != b->count[i])
			return false;
	return true;
}

static int bagUnits(const Inventory* inv, ItemId item)
{
	int n = 0;
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		if (inv->slots[i].item == item) n += inv->slots[i].count;
	return n;
}

static int chestUnits(const ChestState* cs, ItemId item)
{
	int n = 0;
	for (int i = 0; i < CHEST_SLOTS; i++)
		if (cs->item[i] == item) n += cs->count[i];
	return n;
}

static int units(const Inventory* inv, const ChestState* cs, ItemId item)
{
	return bagUnits(inv, item) + chestUnits(cs, item);
}

static int bagTotal(const Inventory* inv)
{
	int n = 0;
	for (int i = 0; i < INV_SLOT_COUNT; i++)
		if (inv->slots[i].item != ITEM_NONE) n += inv->slots[i].count;
	return n;
}

static int chestTotal(const ChestState* cs)
{
	int n = 0;
	for (int i = 0; i < CHEST_SLOTS; i++)
		if (cs->item[i] != ITEM_NONE) n += cs->count[i];
	return n;
}

// Whether the last frame drew a spriteRect with exactly this geometry — the slot-cell
// background drawSlotIcon paints first for every visible slot.
static bool drewRect(URect r)
{
	const UiChestStubDraw* d = uiChestStubDraw();
	const int n = d->rects < UI_CHEST_STUB_RECT_MAX ? d->rects : UI_CHEST_STUB_RECT_MAX;
	for (int i = 0; i < n; i++) {
		const UiChestStubRect* q = &d->rect[i];
		if (q->x == (float)r.x && q->y == (float)r.y && q->w == (float)r.w && q->h == (float)r.h)
			return true;
	}
	return false;
}

// ── What the panel draws ──────────────────────────────────────────────────────────────────

static void testPanelDrawsEightChestSlotsAndTheWholeBag(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);

	(void)frame(&ui, &inv, &cs, false, 0, 0);

	CHECK(uiChestStubDraw()->begins == 1);
	for (int i = 0; i < CHEST_SLOTS; i++)
		CHECK(drewRect(chestSlotRect(i)));
	for (int s = 0; s < INV_SLOT_COUNT; s++)
		CHECK(drewRect(bagRectOnChestScreen(s)));
	// The main grid is drawn at its RELOCATED position and not where the overlay draws it:
	// the two never coincide (GRID_Y vs CHEST_GRID_Y), so this is a real distinction.
	CHECK(!drewRect(gridSlotRect(0)));
	CHECK(drewRect(chestCloseRect()));
}

// ── Deposits (lift a bag slot, place on a chest slot) ─────────────────────────────────────

static void testDepositLiftBagPlaceOnEmptyChestSlot(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 2, BLOCK_STONE, 99);   // hotbar slot 2, not 0: a "slot 0" assumption would pass at 0
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, hotbarSlotRect(2));
	CHECK(ui.picked_slot == 2);           // lifted, nothing moved yet
	CHECK(ui.picked_chest == -1);
	CHECK(bagSlotIs(&inv, 2, BLOCK_STONE, 99));
	CHECK(chestTotal(&cs) == 0);

	tapRect(&ui, &inv, &cs, chestSlotRect(5));   // slot 5, not 0: the PLAYER picks the slot
	CHECK(bagSlotIs(&inv, 2, ITEM_NONE, 0));
	CHECK(chestSlotIs(&cs, 5, BLOCK_STONE, 99));
	CHECK(chestSlotIs(&cs, 0, ITEM_NONE, 0));
	CHECK(nothingLifted(&ui));
	CHECK(units(&inv, &cs, BLOCK_STONE) == 99);
	CHECK(ui.screen == UI_SCR_CHEST);        // a transfer does not close the panel
	CHECK(uiChestStubNet()->calls == 0);     // the chest path never goes through inv_bridge
}

static void testDepositMergesUpToTheStackCap(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 3, BLOCK_STONE, 90);                    // room for 9
	setBag(&inv, INV_HOTBAR_SLOTS + 3, BLOCK_STONE, 30);  // main grid, index 3

	tapRect(&ui, &inv, &cs, chestGridSlotRect(3));
	tapRect(&ui, &inv, &cs, chestSlotRect(3));

	CHECK(chestSlotIs(&cs, 3, BLOCK_STONE, INV_STACK_MAX));   // topped up to the cap, never past
	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS + 3, BLOCK_STONE, 21));   // the remainder stays put
	CHECK(nothingLifted(&ui));   // resolved, not still in hand
	CHECK(units(&inv, &cs, BLOCK_STONE) == 120);
	CHECK(chestTotal(&cs) == INV_STACK_MAX);
}

static void testDepositOntoADifferentItemSwaps(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 4, BLOCK_STONE, 5);
	setChest(&cs, 6, BLOCK_DIRT, 7);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(4));
	tapRect(&ui, &inv, &cs, chestSlotRect(6));

	CHECK(chestSlotIs(&cs, 6, BLOCK_STONE, 5));
	CHECK(bagSlotIs(&inv, 4, BLOCK_DIRT, 7));
	CHECK(nothingLifted(&ui));
	CHECK(units(&inv, &cs, BLOCK_STONE) == 5);
	CHECK(units(&inv, &cs, BLOCK_DIRT) == 7);
	CHECK(bagTotal(&inv) + chestTotal(&cs) == 12);
}

static void testPlaceOnAFullSameStackMovesNothingAndClearsTheLift(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 1, BLOCK_STONE, INV_STACK_MAX);
	setBag(&inv, 0, BLOCK_STONE, 5);
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	tapRect(&ui, &inv, &cs, chestSlotRect(1));

	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(nothingLifted(&ui));   // the overlay's own outcome for a drop onto a full stack
	CHECK(units(&inv, &cs, BLOCK_STONE) == INV_STACK_MAX + 5);
}

// ── Withdrawals (lift a chest slot, place on a bag slot) ──────────────────────────────────

static void testWithdrawLiftChestPlaceOnEmptyBagSlot(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 7, BLOCK_STONE, 12);
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, chestSlotRect(7));
	CHECK(ui.picked_chest == 7);
	CHECK(ui.picked_slot == -1);
	CHECK(chestSlotIs(&cs, 7, BLOCK_STONE, 12));   // lifted, not removed

	tapRect(&ui, &inv, &cs, chestGridSlotRect(9));   // main grid index 9 = bag slot 17
	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS + 9, BLOCK_STONE, 12));
	CHECK(bagSlotIs(&inv, 0, ITEM_NONE, 0));        // NOT inventoryAdd's first-empty slot
	CHECK(chestSlotIs(&cs, 7, ITEM_NONE, 0));       // emptied AND cleared to ITEM_NONE
	CHECK(nothingLifted(&ui));
	CHECK(bagTotal(&inv) == 12);
	CHECK(chestTotal(&cs) == 0);
	CHECK(uiChestStubNet()->calls == 0);   // no PICKUP report: the chest path is not the bridge
}

static void testWithdrawMergesUpToTheStackCap(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 6, BLOCK_STONE, 80);    // room for 19
	setChest(&cs, 2, BLOCK_STONE, 40);

	tapRect(&ui, &inv, &cs, chestSlotRect(2));
	tapRect(&ui, &inv, &cs, hotbarSlotRect(6));

	CHECK(bagSlotIs(&inv, 6, BLOCK_STONE, INV_STACK_MAX));
	CHECK(chestSlotIs(&cs, 2, BLOCK_STONE, 21));   // remainder stays IN THE CHEST, same slot
	CHECK(bagSlotIs(&inv, 0, ITEM_NONE, 0));       // nothing spilled into another bag slot
	CHECK(nothingLifted(&ui));
	CHECK(units(&inv, &cs, BLOCK_STONE) == 120);
}

static void testWithdrawOntoADifferentItemSwaps(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 0, BLOCK_STONE, 3);
	setBag(&inv, 5, BLOCK_DIRT, 9);

	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	tapRect(&ui, &inv, &cs, hotbarSlotRect(5));

	CHECK(bagSlotIs(&inv, 5, BLOCK_STONE, 3));
	CHECK(chestSlotIs(&cs, 0, BLOCK_DIRT, 9));
	CHECK(nothingLifted(&ui));
	CHECK(units(&inv, &cs, BLOCK_STONE) == 3);
	CHECK(units(&inv, &cs, BLOCK_DIRT) == 9);
}

// ── Cancels, no-ops, and the other two pairings ───────────────────────────────────────────

static void testTappingTheLiftedSlotAgainCancelsOnBothSides(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 1, BLOCK_STONE, 4);
	setChest(&cs, 1, BLOCK_DIRT, 6);
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;

	tapRect(&ui, &inv, &cs, hotbarSlotRect(1));
	CHECK(ui.picked_slot == 1);
	tapRect(&ui, &inv, &cs, hotbarSlotRect(1));
	CHECK(nothingLifted(&ui));

	tapRect(&ui, &inv, &cs, chestSlotRect(1));
	CHECK(ui.picked_chest == 1);
	tapRect(&ui, &inv, &cs, chestSlotRect(1));
	CHECK(nothingLifted(&ui));

	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
}

static void testTapsOnEmptySlotsWithNothingLiftedLiftNothing(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 3);
	setChest(&cs, 1, BLOCK_DIRT, 2);
	setChest(&cs, 3, ITEM_NONE, 9);   // a corrupt { none, 9 } payload slot must not be liftable
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, chestSlotRect(4));       // an empty chest slot
	CHECK(nothingLifted(&ui));
	tapRect(&ui, &inv, &cs, chestSlotRect(3));       // the corrupt one
	CHECK(nothingLifted(&ui));
	tapRect(&ui, &inv, &cs, hotbarSlotRect(7));      // an empty hotbar slot
	CHECK(nothingLifted(&ui));
	tapRect(&ui, &inv, &cs, chestGridSlotRect(15));  // an empty grid slot
	CHECK(nothingLifted(&ui));
	tapAt(&ui, &inv, &cs, SCR_W / 2, CHEST_HINT_Y + 4);   // the hint text: no target at all
	CHECK(nothingLifted(&ui));

	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(uiChestStubNet()->calls == 0);
	CHECK(ui.screen == UI_SCR_CHEST);
}

static void testPlacingOnNoTargetKeepsTheLift(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 3);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	tapAt(&ui, &inv, &cs, SCR_W / 2, CHEST_HINT_Y + 4);

	CHECK(ui.picked_slot == 0);   // still in hand: bare panel is not a place
	CHECK(bagSlotIs(&inv, 0, BLOCK_STONE, 3));
	CHECK(chestTotal(&cs) == 0);
}

static void testChestToChestMergesAndSwapsLocally(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 0, BLOCK_STONE, 50);
	setChest(&cs, 1, BLOCK_STONE, 60);   // merge: 0 -> 1 leaves 99 in 1 and 11 in 0
	setChest(&cs, 2, BLOCK_DIRT, 4);

	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	tapRect(&ui, &inv, &cs, chestSlotRect(1));
	CHECK(chestSlotIs(&cs, 1, BLOCK_STONE, INV_STACK_MAX));
	CHECK(chestSlotIs(&cs, 0, BLOCK_STONE, 11));
	CHECK(nothingLifted(&ui));

	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	tapRect(&ui, &inv, &cs, chestSlotRect(2));   // swap with the dirt
	CHECK(chestSlotIs(&cs, 2, BLOCK_STONE, 11));
	CHECK(chestSlotIs(&cs, 0, BLOCK_DIRT, 4));
	CHECK(nothingLifted(&ui));

	tapRect(&ui, &inv, &cs, chestSlotRect(1));
	tapRect(&ui, &inv, &cs, chestSlotRect(5));   // relocate into an empty slot
	CHECK(chestSlotIs(&cs, 5, BLOCK_STONE, INV_STACK_MAX));
	CHECK(chestSlotIs(&cs, 1, ITEM_NONE, 0));

	CHECK(chestUnits(&cs, BLOCK_STONE) == 110);
	CHECK(chestUnits(&cs, BLOCK_DIRT) == 4);
	CHECK(bagTotal(&inv) == 0);
}

// Bag -> bag on the chest screen is the overlay's own handleSlotTap — it goes through
// net/inv_bridge.c and so REPORTS a BS_INV_OP_MOVE, which is how the test tells it apart from
// the chest path (which reports nothing). The chest is not touched.
static void testBagToBagOnTheChestScreenIsTheOverlaysOwnMove(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 3);
	setChest(&cs, 0, BLOCK_DIRT, 2);
	const ChestState cs0 = cs;
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	tapRect(&ui, &inv, &cs, chestGridSlotRect(3));

	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS + 3, BLOCK_STONE, 3));
	CHECK(bagSlotIs(&inv, 0, ITEM_NONE, 0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(nothingLifted(&ui));
	CHECK(uiChestStubNet()->calls == 1);
	CHECK(uiChestStubNet()->op == BS_INV_OP_MOVE);
	CHECK(uiChestStubNet()->a == 0);
	CHECK(uiChestStubNet()->b == INV_HOTBAR_SLOTS + 3);
}

// Every one of the 24 bag cells deposits ITSELF and every one of the 8 chest cells withdraws
// ITSELF — the end-to-end tap -> hit-test -> slot-index mapping through uiUpdateDraw(), which
// ui_layout_test.c's per-function checks cannot see. One unit per cell, so a cell that lifts
// its neighbour shows up as the wrong bag slot going empty.
static void testEveryBagCellDepositsItselfAndEveryChestCellWithdrawsItself(void)
{
	for (int s = 0; s < INV_SLOT_COUNT; s++) {
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, s, BLOCK_STONE, 1);

		tapRect(&ui, &inv, &cs, bagRectOnChestScreen(s));
		CHECK(ui.picked_slot == s);
		tapRect(&ui, &inv, &cs, chestSlotRect(s % CHEST_SLOTS));

		CHECK(bagSlotIs(&inv, s, ITEM_NONE, 0));
		CHECK(chestSlotIs(&cs, s % CHEST_SLOTS, BLOCK_STONE, 1));
	}

	for (int c = 0; c < CHEST_SLOTS; c++) {
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setChest(&cs, c, BLOCK_STONE, 1);

		tapRect(&ui, &inv, &cs, chestSlotRect(c));
		CHECK(ui.picked_chest == c);
		tapRect(&ui, &inv, &cs, bagRectOnChestScreen(c * 3));   // 0, 3, ... 21: hotbar AND grid

		CHECK(chestSlotIs(&cs, c, ITEM_NONE, 0));
		CHECK(bagSlotIs(&inv, c * 3, BLOCK_STONE, 1));
	}
}

// ── Entering and leaving the screen ───────────────────────────────────────────────────────

static void testCloseReturnsToTheHudAndClearsAnyLift(void)
{
	// With a BAG slot lifted.
	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 0, BLOCK_STONE, 5);
		setChest(&cs, 0, BLOCK_DIRT, 7);
		const Inventory  inv0 = inv;
		const ChestState cs0  = cs;

		tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
		CHECK(ui.picked_slot == 0);
		const UiResult r_close = tapRect(&ui, &inv, &cs, chestCloseRect());
		CHECK(ui.screen == UI_SCR_HUD);
		CHECK(nothingLifted(&ui));
		CHECK(!r_close.inventory_open);   // the tap frame DREW the chest screen
		CHECK(!r_close.furnace_open);
		CHECK(bagEq(&inv, &inv0));        // a cancelled lift moved nothing
		CHECK(chestEq(&cs, &cs0));

		// Next frame the HUD is what draws; the chest pointer still handed in must be inert.
		const UiResult r_next = frame(&ui, &inv, &cs, false, 0, 0);
		CHECK(!r_next.inventory_open);
		CHECK(!drewRect(chestSlotRect(0)));
		tapRect(&ui, &inv, &cs, chestSlotRect(2));   // on the HUD this pixel is nothing
		CHECK(chestEq(&cs, &cs0));
		CHECK(bagEq(&inv, &inv0));
	}

	// With a CHEST slot lifted.
	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setChest(&cs, 3, BLOCK_DIRT, 7);
		const ChestState cs0 = cs;

		tapRect(&ui, &inv, &cs, chestSlotRect(3));
		CHECK(ui.picked_chest == 3);
		tapRect(&ui, &inv, &cs, chestCloseRect());
		CHECK(ui.screen == UI_SCR_HUD);
		CHECK(nothingLifted(&ui));
		CHECK(chestEq(&cs, &cs0));
		CHECK(bagTotal(&inv) == 0);
	}
}

static void testOpenChestCancelsAPendingLiftAndRecordsThePosition(void)
{
	// A bag lift pending on the inventory overlay when the player walks up to a chest.
	{
		UiState ui; Inventory inv; ChestState cs;
		uiInit(&ui);
		ui.screen = UI_SCR_INVENTORY;
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 2, BLOCK_STONE, 5);
		const Inventory inv0 = inv;

		tapRect(&ui, &inv, NULL, hotbarSlotRect(2));   // lift on the overlay
		CHECK(ui.picked_slot == 2);

		uiOpenChest(&ui, 9, 64, 3);
		CHECK(ui.screen == UI_SCR_CHEST);
		CHECK(nothingLifted(&ui));
		CHECK(ui.chest_x == 9 && ui.chest_y == 64 && ui.chest_z == 3);
		CHECK(bagEq(&inv, &inv0));

		// And the first tap on the new panel is a fresh LIFT, not a place of the stale one.
		tapRect(&ui, &inv, &cs, chestSlotRect(0));   // empty chest slot: lifts nothing
		CHECK(nothingLifted(&ui));
		CHECK(chestTotal(&cs) == 0);
		CHECK(bagEq(&inv, &inv0));
	}

	// A chest-slot lift pending when a DIFFERENT chest is opened.
	{
		UiState ui; Inventory inv; ChestState a; ChestState b;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&a);
		chestStateInit(&b);
		setChest(&a, 4, BLOCK_DIRT, 7);
		const ChestState a0 = a;

		tapRect(&ui, &inv, &a, chestSlotRect(4));
		CHECK(ui.picked_chest == 4);

		uiOpenChest(&ui, -1, 65, 100);
		CHECK(nothingLifted(&ui));
		CHECK(ui.chest_x == -1 && ui.chest_y == 65 && ui.chest_z == 100);

		tapRect(&ui, &inv, &b, hotbarSlotRect(0));   // would have been a withdraw of a's slot 4
		CHECK(bagTotal(&inv) == 0);
		CHECK(chestEq(&a, &a0));
		CHECK(chestTotal(&b) == 0);
	}

	// uiOpenFurnace clears a chest lift too — the other panel a lift could leak into.
	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setChest(&cs, 1, BLOCK_DIRT, 1);
		tapRect(&ui, &inv, &cs, chestSlotRect(1));
		CHECK(ui.picked_chest == 1);
		uiOpenFurnace(&ui);
		CHECK(nothingLifted(&ui));
	}
}

static void testChestScreenWithNoChestIsCorrectedToTheOverlayAndDropsBothLifts(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);
	setChest(&cs, 0, BLOCK_DIRT, 1);

	tapRect(&ui, &inv, &cs, chestSlotRect(0));   // a chest lift, then the chest goes away
	CHECK(ui.picked_chest == 0);

	// ui.h: "a UiState left on UI_SCR_CHEST with no chest handed in is corrected to
	// UI_SCR_INVENTORY on the spot". The tap lands where chest slot 0 was, which on the
	// overlay is a bag-grid cell — an empty one, so it lifts nothing.
	const URect r = chestSlotRect(0);
	const UiResult res = tapAt(&ui, &inv, NULL, r.x + r.w / 2, r.y + r.h / 2);

	CHECK(ui.screen == UI_SCR_INVENTORY);
	CHECK(res.inventory_open);   // corrected BEFORE the frame's flags were read: the overlay drew
	CHECK(nothingLifted(&ui));
	CHECK(bagUnits(&inv, BLOCK_STONE) == 5);
	CHECK(chestSlotIs(&cs, 0, BLOCK_DIRT, 1));
}

// The transfer block is `else if (tap && chest_open)`: no other screen can reach the chest
// handlers even with a live ChestState handed in. Every chest-panel target is tapped twice
// (a "lift" then a "place") on the two other screens a caller could plausibly be on with the
// pointer still set, and the chest must not move.
static void testChestTransfersNeverFireOffTheChestScreen(void)
{
	static const UiScreen screens[] = { UI_SCR_HUD, UI_SCR_INVENTORY };

	for (size_t k = 0; k < sizeof(screens) / sizeof(screens[0]); k++) {
		UiState ui; Inventory inv; ChestState cs;
		uiInit(&ui);
		ui.screen = screens[k];   // ui.h: setting the field directly is legal, it just isn't a contract
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 0, BLOCK_STONE, 5);
		setChest(&cs, 0, BLOCK_DIRT, 7);
		setChest(&cs, 3, BLOCK_STONE, 2);
		const ChestState cs0 = cs;

		for (int i = 0; i < CHEST_SLOTS; i++) {
			tapRect(&ui, &inv, &cs, chestSlotRect(i));
			tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
			CHECK(chestEq(&cs, &cs0));
			CHECK(ui.picked_chest == -1);
		}
		tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
		tapRect(&ui, &inv, &cs, chestSlotRect(1));
		CHECK(chestEq(&cs, &cs0));
		tapRect(&ui, &inv, &cs, chestCloseRect());
		CHECK(chestEq(&cs, &cs0));
	}
}

// The three hit-tests the chest branch consults — the close bar, hitChestSlot(), and
// hitChestInvSlot() (hotbar + relocated grid) — must never claim the same pixel, or a tap has
// two meanings and the dispatch order in ui.c silently picks one. Swept over every pixel of
// the 320x240 panel. The three area checks are what stop the sweep passing vacuously: a rect
// that had collapsed to nothing would overlap nothing.
static void testChestScreenHitTestsNeverOverlap(void)
{
	int overlaps = 0, close_px = 0, chest_px = 0, bag_px = 0;

	for (int y = 0; y < SCR_H; y++) {
		for (int x = 0; x < SCR_W; x++) {
			const int c = ptInRect(chestCloseRect(), x, y) ? 1 : 0;
			const int s = hitChestSlot(x, y)    >= 0 ? 1 : 0;
			const int b = hitChestInvSlot(x, y) >= 0 ? 1 : 0;
			close_px += c;
			chest_px += s;
			bag_px   += b;
			if (c + s + b > 1) overlaps++;
		}
	}

	CHECK(overlaps == 0);
	CHECK(close_px == SCR_W * CHEST_CLOSE_H);
	CHECK(chest_px == CHEST_SLOTS * SLOT_PX * SLOT_PX);
	CHECK(bag_px   == INV_SLOT_COUNT * SLOT_PX * SLOT_PX);
}

// ── The v1.9.0 CHEST-NET transfer callback ────────────────────────────────────────────────

static int s_spy_token;   // the `ud` the spy is registered with; only its address matters

static struct {
	int     calls;
	uint8_t op, a, b, count;
	int     x, y, z;
	void*   ud;
	bool    answer;   // what the spy returns: true = "sent, the server owns it"
} s_spy;

static bool spyTransfer(void* ud, uint8_t op, int x, int y, int z, uint8_t a, uint8_t b,
                        uint8_t count)
{
	s_spy.calls++;
	s_spy.ud    = ud;
	s_spy.op    = op;
	s_spy.x     = x;
	s_spy.y     = y;
	s_spy.z     = z;
	s_spy.a     = a;
	s_spy.b     = b;
	s_spy.count = count;
	return s_spy.answer;
}

static void armSpy(UiState* ui, bool answer)
{
	memset(&s_spy, 0, sizeof(s_spy));
	s_spy.answer = answer;
	uiSetChestTransferFn(ui, spyTransfer, &s_spy_token);
}

// `count` is checked here rather than in a CHECK of its own: it is one more field of the same
// single call, and the wire frame is wrong if any one of them is.
static bool spyGot(uint8_t op, uint8_t a, uint8_t b, uint8_t count)
{
	return s_spy.calls == 1 && s_spy.op == op && s_spy.a == a && s_spy.b == b
	    && s_spy.count == count
	    && s_spy.x == CHEST_X && s_spy.y == CHEST_Y && s_spy.z == CHEST_Z
	    && s_spy.ud == &s_spy_token;
}

static void testHookDepositSendsTheItemIdAndTheChestSlotAndAppliesNothing(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	inventoryInit(&inv);
	chestStateInit(&cs);
	// Bag slot 5, not 3: BLOCK_STONE IS 3 (world/block.h), so a lift from slot 3 could not tell
	// "a = item id" from "a = source bag slot" apart.
	setBag(&inv, 5, BLOCK_STONE, 7);
	setChest(&cs, 0, BLOCK_DIRT, 2);
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, hotbarSlotRect(5));
	CHECK(s_spy.calls == 0);   // a lift is not a transfer
	CHECK(ui.picked_slot == 5);

	tapRect(&ui, &inv, &cs, chestSlotRect(6));

	// a = ITEM ID, b = chest slot, count = the whole lifted stack
	CHECK(spyGot(UI_CHEST_OP_DEPOSIT, (uint8_t)BLOCK_STONE, 6, 7));
	CHECK(s_spy.a != 5);            // and NOT the bag slot the stack came from
	CHECK(bagEq(&inv, &inv0));      // applied nothing, either side
	CHECK(chestEq(&cs, &cs0));
	CHECK(nothingLifted(&ui));      // sent: the server owns it, the lift is done
	CHECK(uiChestStubNet()->calls == 0);
	CHECK(ui.screen == UI_SCR_CHEST);
}

static void testHookWithdrawSendsTheChestSlotAndTheBagSlotAndAppliesNothing(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 4, BLOCK_STONE, 9);
	setBag(&inv, 1, BLOCK_DIRT, 1);   // a swap target, so a local apply would have shown
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, chestSlotRect(4));
	CHECK(s_spy.calls == 0);
	tapRect(&ui, &inv, &cs, chestGridSlotRect(2));   // bag slot INV_HOTBAR_SLOTS + 2

	CHECK(spyGot(UI_CHEST_OP_WITHDRAW, 4, (uint8_t)(INV_HOTBAR_SLOTS + 2), 9));
	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(nothingLifted(&ui));
	CHECK(uiChestStubNet()->calls == 0);
}

// The callback runs BEFORE the lift is cleared (ui.h's contract, so a registrant can read the
// count off the lifted source). Proved by a spy that looks.
static int s_seen_picked_slot, s_seen_picked_chest;
static UiState* s_spy_ui;

static bool spyThatLooksAtTheLift(void* ud, uint8_t op, int x, int y, int z, uint8_t a, uint8_t b,
                                  uint8_t count)
{
	s_seen_picked_slot  = s_spy_ui->picked_slot;
	s_seen_picked_chest = s_spy_ui->picked_chest;
	return spyTransfer(ud, op, x, y, z, a, b, count);
}

static void testHookRunsWhileTheLiftIsStillRecorded(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	uiSetChestTransferFn(&ui, spyThatLooksAtTheLift, &s_spy_token);
	s_spy_ui = &ui;
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 5, BLOCK_STONE, 7);
	setChest(&cs, 2, BLOCK_DIRT, 3);

	s_seen_picked_slot = s_seen_picked_chest = -99;
	tapRect(&ui, &inv, &cs, hotbarSlotRect(5));
	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	CHECK(s_spy.calls == 1);
	CHECK(s_seen_picked_slot == 5 && s_seen_picked_chest == -1);
	CHECK(nothingLifted(&ui));   // ...and cleared afterwards

	s_seen_picked_slot = s_seen_picked_chest = -99;
	tapRect(&ui, &inv, &cs, chestSlotRect(2));
	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	CHECK(s_spy.calls == 2);
	CHECK(s_seen_picked_slot == -1 && s_seen_picked_chest == 2);
	CHECK(nothingLifted(&ui));
}

static void testHookRefusingChangesNothingAndKeepsTheLift(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, false);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 2, BLOCK_STONE, 99);
	setChest(&cs, 1, BLOCK_DIRT, 4);
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;
	uiChestStubResetNet();

	// Deposit: consulted, refused, still in hand.
	tapRect(&ui, &inv, &cs, hotbarSlotRect(2));
	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	CHECK(spyGot(UI_CHEST_OP_DEPOSIT, (uint8_t)BLOCK_STONE, 0, 99));
	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(ui.picked_slot == 2);   // the lift is KEPT
	CHECK(ui.picked_chest == -1);

	// Still in hand means it can be put back: the cancel gesture still works.
	tapRect(&ui, &inv, &cs, hotbarSlotRect(2));
	CHECK(nothingLifted(&ui));
	CHECK(bagEq(&inv, &inv0));

	// Withdraw: consulted, refused, still in hand.
	memset(&s_spy, 0, sizeof(s_spy));
	tapRect(&ui, &inv, &cs, chestSlotRect(1));
	tapRect(&ui, &inv, &cs, hotbarSlotRect(7));
	CHECK(spyGot(UI_CHEST_OP_WITHDRAW, 1, 7, 4));
	CHECK(bagEq(&inv, &inv0));
	CHECK(chestEq(&cs, &cs0));
	CHECK(ui.picked_chest == 1);
	CHECK(ui.picked_slot == -1);

	// The kept lift is a real one: clear the callback (session gone) and the same place tap
	// now applies locally, from the lift that was refused a moment ago.
	uiSetChestTransferFn(&ui, NULL, NULL);
	tapRect(&ui, &inv, &cs, hotbarSlotRect(7));
	CHECK(bagSlotIs(&inv, 7, BLOCK_DIRT, 4));
	CHECK(chestSlotIs(&cs, 1, ITEM_NONE, 0));
	CHECK(nothingLifted(&ui));
	CHECK(uiChestStubNet()->calls == 0);
}

static void testHookIsNotConsultedForLiftsCancelsBagToBagOrTheCloseBar(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 3);
	setChest(&cs, 1, BLOCK_DIRT, 2);
	const ChestState cs0 = cs;

	tapRect(&ui, &inv, &cs, chestSlotRect(4));      // empty chest slot: no lift
	tapRect(&ui, &inv, &cs, hotbarSlotRect(7));     // empty bag slot: no lift
	tapAt(&ui, &inv, &cs, SCR_W / 2, CHEST_HINT_Y + 4);   // no target
	CHECK(s_spy.calls == 0);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));     // lift
	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));     // cancel
	tapRect(&ui, &inv, &cs, chestSlotRect(1));      // lift
	tapRect(&ui, &inv, &cs, chestSlotRect(1));      // cancel
	CHECK(s_spy.calls == 0);
	CHECK(nothingLifted(&ui));

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));     // bag -> bag: the overlay's own move
	tapRect(&ui, &inv, &cs, chestGridSlotRect(0));
	CHECK(s_spy.calls == 0);
	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, 3));   // and it DID move, locally
	CHECK(chestEq(&cs, &cs0));

	tapRect(&ui, &inv, &cs, chestSlotRect(1));      // lift, then the close bar is not a place
	tapRect(&ui, &inv, &cs, chestCloseRect());
	CHECK(s_spy.calls == 0);
	CHECK(ui.screen == UI_SCR_HUD);
	CHECK(chestEq(&cs, &cs0));
}

static void testHookRefusesChestToChestWithoutBeingCalled(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 0, BLOCK_STONE, 5);
	setChest(&cs, 1, BLOCK_DIRT, 2);
	const ChestState cs0 = cs;

	tapRect(&ui, &inv, &cs, chestSlotRect(0));
	tapRect(&ui, &inv, &cs, chestSlotRect(1));   // would be a swap locally
	CHECK(s_spy.calls == 0);        // no opcode carries this, so it is not sent...
	CHECK(chestEq(&cs, &cs0));      // ...and not applied...
	CHECK(ui.picked_chest == 0);    // ...and still in hand

	tapRect(&ui, &inv, &cs, chestSlotRect(3));   // an empty slot: same answer
	CHECK(s_spy.calls == 0);
	CHECK(chestEq(&cs, &cs0));
	CHECK(ui.picked_chest == 0);
}

static void testHookIsConsultedOnlyOnTheChestScreen(void)
{
	UiState ui; Inventory inv; ChestState cs;
	uiInit(&ui);
	armSpy(&ui, true);
	ui.screen = UI_SCR_INVENTORY;
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);
	setChest(&cs, 0, BLOCK_DIRT, 7);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));   // lift on the overlay
	tapRect(&ui, &inv, &cs, chestSlotRect(0));    // on the overlay this pixel is a grid cell

	CHECK(s_spy.calls == 0);
	CHECK(chestSlotIs(&cs, 0, BLOCK_DIRT, 7));
	CHECK(bagUnits(&inv, BLOCK_STONE) == 5);
}

static void testClearingTheHookRestoresTheLocalApply(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	armSpy(&ui, true);
	uiSetChestTransferFn(&ui, NULL, NULL);   // registered, then cleared — the default is back
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	tapRect(&ui, &inv, &cs, chestSlotRect(2));

	CHECK(s_spy.calls == 0);
	CHECK(chestSlotIs(&cs, 2, BLOCK_STONE, 5));
	CHECK(bagSlotIs(&inv, 0, ITEM_NONE, 0));

	// And uiInit() clears it too, the way ui.h says it does.
	armSpy(&ui, true);
	uiInit(&ui);
	CHECK(ui.chest_fn == NULL && ui.chest_ud == NULL);
}

// ── Open -> mutate -> close -> reopen, through the real BlockStateTable ───────────────────

typedef struct {
	BlockStateTable table;
	int  x, y, z;
	bool open;
} Host;

// main.c's per-frame chest glue, restated (see the file comment): unpack if open, call, pack
// back if a chest was handed in, closed once the screen has left UI_SCR_CHEST. The pack-back
// is gated on "a chest was handed in" and NOT on "still open", exactly as main.c's is, so a
// transfer and a close on the same frame would both still land.
static void hostFrame(Host* h, UiState* ui, Inventory* inv, bool down, int tx, int ty)
{
	uint8_t     pay[BLOCKSTATE_PAYLOAD_BYTES];
	ChestState  cs;
	ChestState* cp = NULL;

	if (h->open) {
		if (blockStateGet(&h->table, h->x, h->y, h->z, BLOCK_CHEST, pay)) {
			chestStateUnpack(&cs, pay);
			cp = &cs;
		} else {
			h->open = false;
		}
	}

	const UiInput in = { down, tx, ty, 0 };   // v1.9.0 SPLIT: keys_held — no key held here
	(void)uiUpdateDraw(ui, inv, NULL, NULL, &in, NULL, cp);

	if (cp) {
		chestStatePack(cp, pay);
		(void)blockStateSet(&h->table, h->x, h->y, h->z, BLOCK_CHEST, pay);
	}
	if (ui->screen != UI_SCR_CHEST) h->open = false;
}

static void hostTap(Host* h, UiState* ui, Inventory* inv, URect r)
{
	hostFrame(h, ui, inv, true, r.x + r.w / 2, r.y + r.h / 2);
	hostFrame(h, ui, inv, false, 0, 0);
}

// What the table holds for the chest RIGHT NOW, unpacked through the real chest.c.
static bool hostRead(const Host* h, ChestState* out)
{
	uint8_t pay[BLOCKSTATE_PAYLOAD_BYTES];
	if (!blockStateGet(&h->table, h->x, h->y, h->z, BLOCK_CHEST, pay)) return false;
	chestStateUnpack(out, pay);
	return true;
}

static void testContentsSurviveCloseAndReopenThroughTheRealBlockstateTable(void)
{
	Host h;
	blockStateInit(&h.table);
	h.x = CHEST_X; h.y = CHEST_Y; h.z = CHEST_Z;
	CHECK(blockStateCreate(&h.table, h.x, h.y, h.z, BLOCK_CHEST));
	h.open = true;

	UiState ui; Inventory inv; ChestState cs;
	uiInit(&ui);
	uiOpenChest(&ui, h.x, h.y, h.z);
	inventoryInit(&inv);
	setBag(&inv, 0, BLOCK_STONE, 99);
	setBag(&inv, 1, BLOCK_APPLE, 4);

	// Two deposits, each lift+place a full frame round trip through the table — the lift
	// frame packs back an unchanged chest, the place frame packs back the moved stack.
	hostTap(&h, &ui, &inv, hotbarSlotRect(0));
	hostTap(&h, &ui, &inv, chestSlotRect(0));
	hostTap(&h, &ui, &inv, hotbarSlotRect(1));
	hostTap(&h, &ui, &inv, chestSlotRect(1));
	CHECK(hostRead(&h, &cs));
	CHECK(chestSlotIs(&cs, 0, BLOCK_STONE, 99));
	CHECK(chestSlotIs(&cs, 1, BLOCK_APPLE, 4));
	CHECK(bagTotal(&inv) == 0);
	CHECK(h.open);
	CHECK(ui.screen == UI_SCR_CHEST);

	// The bytes in the table are chest.c's real format — item, count, item, count... — not a
	// copy this file keeps, so a round trip that "worked" through a broken pack would show here.
	uint8_t raw[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&h.table, h.x, h.y, h.z, BLOCK_CHEST, raw));
	CHECK(raw[0] == BLOCK_STONE && raw[1] == 99 && raw[2] == BLOCK_APPLE && raw[3] == 4);

	// Close with a lift pending. main.c's rule: the frame that leaves UI_SCR_CHEST is the last
	// one handed a chest, and it still packs back — which must be the UNCHANGED chest.
	hostTap(&h, &ui, &inv, chestSlotRect(0));
	CHECK(ui.picked_chest == 0);
	hostTap(&h, &ui, &inv, chestCloseRect());
	CHECK(ui.screen == UI_SCR_HUD);
	CHECK(!h.open);
	CHECK(nothingLifted(&ui));

	// Frames while closed: the pixel where chest slot 0 was is nothing on the HUD, and the
	// table is not touched.
	hostTap(&h, &ui, &inv, chestSlotRect(0));
	CHECK(hostRead(&h, &cs));
	CHECK(chestSlotIs(&cs, 0, BLOCK_STONE, 99));
	CHECK(chestSlotIs(&cs, 1, BLOCK_APPLE, 4));

	// Reopen — a fresh unpack from the table, exactly what main.c does on the next PLACE.
	uiOpenChest(&ui, h.x, h.y, h.z);
	h.open = true;
	hostTap(&h, &ui, &inv, chestSlotRect(0));    // take the stone back...
	hostTap(&h, &ui, &inv, hotbarSlotRect(3));   // ...into hotbar slot 3
	CHECK(bagSlotIs(&inv, 3, BLOCK_STONE, 99));
	CHECK(hostRead(&h, &cs));
	CHECK(chestSlotIs(&cs, 0, ITEM_NONE, 0));
	CHECK(chestSlotIs(&cs, 1, BLOCK_APPLE, 4));   // the other stack survived the round trip untouched

	hostTap(&h, &ui, &inv, chestSlotRect(1));    // and the apples
	hostTap(&h, &ui, &inv, chestGridSlotRect(0));
	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_APPLE, 4));
	CHECK(hostRead(&h, &cs));
	CHECK(chestTotal(&cs) == 0);
	CHECK(bagTotal(&inv) == 103);
	CHECK(bagUnits(&inv, BLOCK_STONE) == 99);
	CHECK(bagUnits(&inv, BLOCK_APPLE) == 4);
}

// ── v1.9.0 SPLIT/QUICK-MOVE: the real X/Y dispatch (ui.c:1171-1178) ──────────────────────
//
// The gap this section closes: scene/ui_gesture_test.c proves uiGestureFeed() CLASSIFIES a
// held X or Y correctly in isolation, and net/inv_bridge_test.c proves invBridgeSplitStack()
// and invBridgeQuickMove() MOVE units correctly in isolation, but nothing before this pressed
// a real X or Y through the real uiUpdateDraw() switch (source/scene/ui.c:1171-1178,
// `case UI_GESTURE_SPLIT: handleSplit(...); case UI_GESTURE_QUICK_MOVE: handleQuickMove(...);`)
// that wires the two together. Every check below reads the resulting SLOT STATE, never a
// return code, and every gesture is followed by a conservation check — the same discipline the
// transfer tests above already use.
//
// keys_held is set for REAL here, via the new frameKeys()/tapRectKeys()/pressKeyOnLift() below,
// rather than the file's existing frame()/frameWithIcons()/hostFrame() helpers, which hardcode
// it to 0 at lines 134 and 1138 above. Every OTHER test in this file drives those helpers and
// none of them wants a key event, so those two zeros are load-bearing; new paths are added
// instead of mutating them.

// One frame with a real key word. `furnace` and `chest` are each optional and never both
// non-NULL at once here, the same mutual exclusivity main.c's own caller keeps.
static UiResult frameKeys(UiState* ui, Inventory* inv, FurnaceState* furnace, ChestState* chest,
                          bool down, int x, int y, uint32_t keys)
{
	const UiInput in = { down, x, y, keys };
	return uiUpdateDraw(ui, inv, NULL, NULL, &in, furnace, chest);
}

static UiResult tapKeys(UiState* ui, Inventory* inv, FurnaceState* furnace, ChestState* chest,
                        int x, int y)
{
	const UiResult r = frameKeys(ui, inv, furnace, chest, true, x, y, 0);
	(void)frameKeys(ui, inv, furnace, chest, false, 0, 0, 0);
	return r;
}

static UiResult tapRectKeys(UiState* ui, Inventory* inv, FurnaceState* furnace, ChestState* chest,
                            URect r)
{
	return tapKeys(ui, inv, furnace, chest, r.x + r.w / 2, r.y + r.h / 2);
}

// Presses `key` for one edge with the stylus UP throughout, so uiGestureFeed() targets
// ui->picked_slot rather than anything under the stylus — scene/ui_gesture.h: "the lifted slot
// is the fallback so the one-handed tap-then-thumb flow works here too" for both X and Y. Two
// frames, held then released, the same shape a tap already has, so a LATER press is a fresh
// edge and not swallowed by this one (ui_gesture.h: "a button held across several frames is
// one event").
static void pressKeyOnLift(UiState* ui, Inventory* inv, FurnaceState* furnace, ChestState* chest,
                           uint32_t key)
{
	(void)frameKeys(ui, inv, furnace, chest, false, 0, 0, key);
	(void)frameKeys(ui, inv, furnace, chest, false, 0, 0, 0);
}

// Field by field, the same reason bagEq/chestEq above are not a memcmp.
static bool furnaceEq(const FurnaceState* a, const FurnaceState* b)
{
	return a->input_item == b->input_item && a->input_count == b->input_count
	    && a->fuel_item  == b->fuel_item  && a->fuel_count  == b->fuel_count
	    && a->fuel_ticks_left == b->fuel_ticks_left && a->cook_ticks == b->cook_ticks
	    && a->output_item == b->output_item && a->output_count == b->output_count
	    && a->lit == b->lit;
}

// Y, inventory-overlay-only (ui.c:1167 `.split_enabled = overlay_open`), on an ODD count: the
// two halves DIFFER, which is exactly where a ceil/floor swap or an off-by-one would show.
static void testYSplitsAnOddStackCeilToTheLiftFloorBehind(void)
{
	UiState ui; Inventory inv;
	uiInit(&ui);
	ui.screen = UI_SCR_INVENTORY;
	inventoryInit(&inv);
	setBag(&inv, 4, BLOCK_STONE, 7);   // odd: ceil(7/2)=4, floor(7/2)=3, genuinely different

	tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(4));   // lift
	CHECK(ui.picked_slot == 4);

	pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_Y);

	CHECK(ui.lift.item == BLOCK_STONE);
	CHECK(ui.lift.count == 4);                  // ceil(7/2) detached into the lift
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 3));  // floor(7/2) stays behind, slot never empties
	CHECK(ui.lift_from == 4);
	CHECK(ui.picked_slot == 4);                 // still the picked slot: stays highlighted
	CHECK(bagUnits(&inv, BLOCK_STONE) + ui.lift.count == 7);   // conservation, mid-air

	// One half in the air at a time (ui.c's handleSplit: `if (liftDetached(ui)) return;`) — a
	// second Y on the same lifted slot must change nothing.
	pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_Y);
	CHECK(ui.lift.count == 4);
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 3));

	// Place the detached half on an empty slot: both halves now sit side by side.
	tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(6));
	CHECK(bagSlotIs(&inv, 6, BLOCK_STONE, 4));
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 3));
	CHECK(nothingLifted(&ui));
	CHECK(bagUnits(&inv, BLOCK_STONE) == 7);    // conservation, resolved
}

// Y is legal on the overlay alone (ui.c:1167). On the furnace and chest screens split_enabled
// is false, so pressing Y with a real lift already picked up must do nothing — not to the
// lift, not to the bag, not to the panel.
static void testYIsInertOnFurnaceAndChestScreens(void)
{
	{
		UiState ui; Inventory inv; FurnaceState furnace;
		uiInit(&ui);
		inventoryInit(&inv);
		furnaceStateInit(&furnace);
		setBag(&inv, 2, BLOCK_STONE, 7);
		uiOpenFurnace(&ui);

		tapRectKeys(&ui, &inv, &furnace, NULL, hotbarSlotRect(2));   // lift_enabled IS true here
		CHECK(ui.picked_slot == 2);

		pressKeyOnLift(&ui, &inv, &furnace, NULL, UI_GESTURE_KEY_Y);

		CHECK(ui.lift.item == ITEM_NONE);            // nothing detached
		CHECK(bagSlotIs(&inv, 2, BLOCK_STONE, 7));    // whole stack, untouched
		CHECK(ui.picked_slot == 2);                   // still just a plain whole-stack lift
		CHECK(ui.screen == UI_SCR_FURNACE);
	}

	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 2, BLOCK_STONE, 7);

		tapRectKeys(&ui, &inv, NULL, &cs, hotbarSlotRect(2));
		CHECK(ui.picked_slot == 2);

		pressKeyOnLift(&ui, &inv, NULL, &cs, UI_GESTURE_KEY_Y);

		CHECK(ui.lift.item == ITEM_NONE);
		CHECK(bagSlotIs(&inv, 2, BLOCK_STONE, 7));
		CHECK(ui.picked_slot == 2);
		CHECK(chestTotal(&cs) == 0);
		CHECK(ui.screen == UI_SCR_CHEST);
	}
}

// X on the overlay: the whole lifted stack goes to the OTHER strip (ui.c's handleQuickMove ->
// invBridgeQuickMove -> invBridgePlanQuickMove's real plan, not a hand-rolled move) — an empty
// destination (single relocation) and a same-item destination already holding some (merge
// capped at INV_STACK_MAX, remainder stays behind), the same two shapes the deposit tests above
// already cover for taps.
static void testXQuickMovesTheWholeStackToTheOtherStripOnTheOverlay(void)
{
	// Hotbar -> main grid, into the first EMPTY grid slot.
	{
		UiState ui; Inventory inv;
		uiInit(&ui);
		ui.screen = UI_SCR_INVENTORY;
		inventoryInit(&inv);
		setBag(&inv, 2, BLOCK_STONE, 50);

		tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(2));
		CHECK(ui.picked_slot == 2);

		pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_X);

		CHECK(bagSlotIs(&inv, 2, ITEM_NONE, 0));
		CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, 50));
		CHECK(nothingLifted(&ui));      // handleQuickMove drops any lift up front
		CHECK(bagUnits(&inv, BLOCK_STONE) == 50);
	}

	// Main grid -> hotbar, capped onto an EXISTING same-item stack; every other hotbar slot is
	// blocked with a different item so there is no empty slot to spill the remainder into,
	// which is what proves the cap (not a lucky empty landing).
	{
		UiState ui; Inventory inv;
		uiInit(&ui);
		ui.screen = UI_SCR_INVENTORY;
		inventoryInit(&inv);
		setBag(&inv, 0, BLOCK_STONE, 90);   // room for 9 before the cap
		for (int i = 1; i < INV_HOTBAR_SLOTS; i++)
			setBag(&inv, i, BLOCK_DIRT, 10);   // blocks every other hotbar slot
		setBag(&inv, INV_HOTBAR_SLOTS + 5, BLOCK_STONE, 30);

		tapRectKeys(&ui, &inv, NULL, NULL, gridSlotRect(5));
		CHECK(ui.picked_slot == INV_HOTBAR_SLOTS + 5);

		pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_X);

		CHECK(bagSlotIs(&inv, 0, BLOCK_STONE, INV_STACK_MAX));          // topped up, never past
		CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS + 5, BLOCK_STONE, 21));  // remainder stays put
		CHECK(nothingLifted(&ui));
		CHECK(bagUnits(&inv, BLOCK_STONE) == 120);
	}
}

// X on the furnace screen: legal (the dispatch fires), but `to_chest` is only ever
// `in->chest_open` (ui.c:1168), which is false here — so the stack goes to the bag's other
// strip exactly as it does on the overlay, and the furnace's own three slots are never touched.
// Proved with the furnace actually holding something, so "untouched" is a real assertion.
static void testXQuickMoveOnFurnaceScreenMovesWithinTheBagNotIntoTheFurnace(void)
{
	UiState ui; Inventory inv; FurnaceState furnace;
	uiInit(&ui);
	inventoryInit(&inv);
	furnaceStateInit(&furnace);
	furnace.fuel_item  = BLOCK_STONE;
	furnace.fuel_count = 3;
	const FurnaceState furnace0 = furnace;
	setBag(&inv, 2, BLOCK_STONE, 12);
	uiOpenFurnace(&ui);

	tapRectKeys(&ui, &inv, &furnace, NULL, hotbarSlotRect(2));
	CHECK(ui.picked_slot == 2);

	pressKeyOnLift(&ui, &inv, &furnace, NULL, UI_GESTURE_KEY_X);

	CHECK(bagSlotIs(&inv, 2, ITEM_NONE, 0));
	CHECK(bagSlotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, 12));   // the OTHER strip of the bag
	CHECK(furnaceEq(&furnace, &furnace0));                        // the furnace itself never saw it
	CHECK(nothingLifted(&ui));
	CHECK(bagUnits(&inv, BLOCK_STONE) == 12);
	CHECK(ui.screen == UI_SCR_FURNACE);
}

// X on the chest screen: `to_chest` is true, so the whole lifted stack is sent INTO the chest
// (ui.c's chestQuickSlot: first same-item slot with room, else first empty) — and a full chest
// refuses it, leaving the bag untouched, though the lift pointer is still cleared (handleQuickMove
// drops any lift BEFORE it knows whether the chest has room; see its own comment on why).
static void testXQuickMoveOnChestScreenDepositsIntoTheChest(void)
{
	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 3, BLOCK_STONE, 40);
		uiChestStubResetNet();

		tapRectKeys(&ui, &inv, NULL, &cs, hotbarSlotRect(3));
		CHECK(ui.picked_slot == 3);

		pressKeyOnLift(&ui, &inv, NULL, &cs, UI_GESTURE_KEY_X);

		CHECK(bagSlotIs(&inv, 3, ITEM_NONE, 0));
		CHECK(chestSlotIs(&cs, 0, BLOCK_STONE, 40));
		CHECK(nothingLifted(&ui));
		CHECK(units(&inv, &cs, BLOCK_STONE) == 40);
		CHECK(uiChestStubNet()->calls == 0);   // the chest path never goes through inv_bridge
	}

	{
		UiState ui; Inventory inv; ChestState cs;
		openChest(&ui);
		inventoryInit(&inv);
		chestStateInit(&cs);
		fillChest(&cs, BLOCK_DIRT, INV_STACK_MAX);   // full, and a different item: no room anywhere
		setBag(&inv, 3, BLOCK_STONE, 40);
		const Inventory  inv0 = inv;
		const ChestState cs0  = cs;

		tapRectKeys(&ui, &inv, NULL, &cs, hotbarSlotRect(3));
		CHECK(ui.picked_slot == 3);

		pressKeyOnLift(&ui, &inv, NULL, &cs, UI_GESTURE_KEY_X);

		CHECK(bagEq(&inv, &inv0));      // refused: nothing moved
		CHECK(chestEq(&cs, &cs0));
		CHECK(nothingLifted(&ui));      // still cleared: X drops the pointer regardless of outcome
		CHECK(units(&inv, &cs, BLOCK_STONE) == 40);
	}
}

// X is fed every frame including the HUD (ui_gesture.h's contract), but on the HUD slot_under
// is always -1 (ui.c only hit-tests furnace/chest/overlay grids) and nothing is ever lifted
// there, so the classifier's target is -1 on both sides and the gesture is inert.
static void testXIsInertOnTheHudScreen(void)
{
	UiState ui; Inventory inv;
	uiInit(&ui);   // UI_SCR_HUD, nothing lifted — the default
	inventoryInit(&inv);
	setBag(&inv, 0, BLOCK_STONE, 5);

	pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_X);

	CHECK(ui.screen == UI_SCR_HUD);
	CHECK(bagSlotIs(&inv, 0, BLOCK_STONE, 5));
	CHECK(nothingLifted(&ui));
}

// ── Quad budget ───────────────────────────────────────────────────────────────────────────
//
// gfx/sprite.c holds SPRITE_MAX_QUADS (1024) per frame and its counter wraps silently, so
// the only place an overrun would ever be seen is here. Worst case for each screen: every
// slot full with a two-digit count (a count badge is two glyphs), a dummy atlas texture so
// the icon pass runs, and for the HUD a UiStats with every optional line present. Two of the
// three numbers are printed for the record; all three are asserted under the ceiling.
static int quadsForScreen(UiScreen screen, ChestState* cs, const UiStats* stats)
{
	static C3D_Tex dummy_atlas;   // never dereferenced by ui.c (ui.h: it only passes it on)
	UiState ui; Inventory inv;
	uiInit(&ui);
	if (screen == UI_SCR_CHEST) uiOpenChest(&ui, CHEST_X, CHEST_Y, CHEST_Z);
	else                        ui.screen = screen;
	inventoryInit(&inv);
	fillBag(&inv, BLOCK_STONE, 99);
	(void)frameWithIcons(&ui, &inv, cs, &dummy_atlas, stats, false, 0, 0);
	return uiChestStubDraw()->quads;
}

static void testEveryScreenFitsTheSpriteQuadBudget(void)
{
	ChestState cs;
	chestStateInit(&cs);
	fillChest(&cs, BLOCK_STONE, 99);

	UiStats stats;
	memset(&stats, 0, sizeof(stats));
	stats.columns = 999; stats.chunks = 9999; stats.meshes = 9999; stats.culled = 9999;
	stats.tris = 9999999u; stats.bytes = 99999999u; stats.bytes_peak = 99999999u;
	stats.status = "aim -12 8 -10 f2 b9 p9 r9 dq8/8";
	stats.net    = "net s4 r91 y12 a12 q0 p1";
	stats.timing = "cpu 2.1  wait 14.6  frame 16.7ms  60fps";
	stats.health = 19; stats.hunger = 19;

	const int hud   = quadsForScreen(UI_SCR_HUD, NULL, &stats);
	const int inven = quadsForScreen(UI_SCR_INVENTORY, NULL, NULL);
	const int chest = quadsForScreen(UI_SCR_CHEST, &cs, NULL);
	const UiChestStubDraw* d = uiChestStubDraw();   // the chest frame is the last one drawn

	printf("chest_ui quads/frame (worst case): hud %d  inventory %d  chest %d "
	       "(chest: %d rects, %d glyphs, %d icons; %d texture binds) of %d\n",
	       hud, inven, chest, d->rects, d->glyph_quads, d->icon_quads, d->tex_changes,
	       SPRITE_MAX_QUADS_RESTATED);

	CHECK(hud   > 0 && hud   < SPRITE_MAX_QUADS_RESTATED);
	CHECK(inven > 0 && inven < SPRITE_MAX_QUADS_RESTATED);
	CHECK(chest > 0 && chest < SPRITE_MAX_QUADS_RESTATED);
	CHECK(d->tex_changes == 2);   // font pass, atlas pass: the two-draw-call promise ui.c makes
	CHECK(d->icon_quads == INV_SLOT_COUNT + CHEST_SLOTS);   // one icon per full slot, both sides
}

int main(void)
{
	// inventoryAdd() asks the registry whether an item is holdable (inventory.h's
	// inventoryCanHold), same as world/inventory_test.c's own main() does first.
	registryInitCore();

	testPanelDrawsEightChestSlotsAndTheWholeBag();

	testDepositLiftBagPlaceOnEmptyChestSlot();
	testDepositMergesUpToTheStackCap();
	testDepositOntoADifferentItemSwaps();
	testPlaceOnAFullSameStackMovesNothingAndClearsTheLift();

	testWithdrawLiftChestPlaceOnEmptyBagSlot();
	testWithdrawMergesUpToTheStackCap();
	testWithdrawOntoADifferentItemSwaps();

	testTappingTheLiftedSlotAgainCancelsOnBothSides();
	testTapsOnEmptySlotsWithNothingLiftedLiftNothing();
	testPlacingOnNoTargetKeepsTheLift();
	testChestToChestMergesAndSwapsLocally();
	testBagToBagOnTheChestScreenIsTheOverlaysOwnMove();
	testEveryBagCellDepositsItselfAndEveryChestCellWithdrawsItself();

	testCloseReturnsToTheHudAndClearsAnyLift();
	testOpenChestCancelsAPendingLiftAndRecordsThePosition();
	testChestScreenWithNoChestIsCorrectedToTheOverlayAndDropsBothLifts();
	testChestTransfersNeverFireOffTheChestScreen();
	testChestScreenHitTestsNeverOverlap();

	testHookDepositSendsTheItemIdAndTheChestSlotAndAppliesNothing();
	testHookWithdrawSendsTheChestSlotAndTheBagSlotAndAppliesNothing();
	testHookRunsWhileTheLiftIsStillRecorded();
	testHookRefusingChangesNothingAndKeepsTheLift();
	testHookIsNotConsultedForLiftsCancelsBagToBagOrTheCloseBar();
	testHookRefusesChestToChestWithoutBeingCalled();
	testHookIsConsultedOnlyOnTheChestScreen();
	testClearingTheHookRestoresTheLocalApply();

	testContentsSurviveCloseAndReopenThroughTheRealBlockstateTable();

	testYSplitsAnOddStackCeilToTheLiftFloorBehind();
	testYIsInertOnFurnaceAndChestScreens();
	testXQuickMovesTheWholeStackToTheOtherStripOnTheOverlay();
	testXQuickMoveOnFurnaceScreenMovesWithinTheBagNotIntoTheFurnace();
	testXQuickMoveOnChestScreenDepositsIntoTheChest();
	testXIsInertOnTheHudScreen();

	testEveryScreenFitsTheSpriteQuadBudget();

	checkCountPin();

	if (s_fails == 0)
		printf("chest_ui self-test: PASS  %d checks\n", s_checks);
	else
		printf("chest_ui self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}
