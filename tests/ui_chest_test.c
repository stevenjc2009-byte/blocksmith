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
// chestSlotRect()/chestGridSlotRect()/hotbarSlotRect()/barCloseRect() ui.c hit-tests
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
// for a chest — unpack from world/blockstate.c if open, call, pack back, "closed once
// UiResult.chest_open comes back false" (v1.9.1's seam S6; it used to be a test against the
// retired UI_SCR_CHEST enum value). main.c cannot be linked (it carries main() and <3ds.h>),
// so hostFrame() below restates those four steps and nothing else, in main.c's order, so they
// can be checked against it by eye. It is the only logic in this file that is a copy rather
// than a link.
//
// ── 2026-09-07, v1.9.1 BAR: what moved, and what did not ─────────────────────────────────
//
// UI_SCR_INVENTORY, UI_SCR_CHEST and UI_SCR_FURNACE are GONE. There is one UI_SCR_BAR screen
// with a tab cursor (scene/barnav.h's BarNav, on UiState.nav), and the three old screens are
// three TABS of it. So every assertion that used to name a screen now names TWO things — the
// bar is up, AND which tab is focused — because "the bar is open" on its own no longer says
// what the player is looking at, and a check that only asserted that would have stopped
// testing the thing it was written for. onTab() below is how the tab half is asserted.
//
// The TRANSFER SEMANTICS this file exists for are unchanged, and so is every rect the chest
// content is drawn at and hit-tested against (chestSlotRect, chestGridSlotRect, hotbarSlotRect).
// One control did move: the close bar. chestCloseRect() is (0, 40, 320, 26), which is now
// exactly the TAB STRIP (barStripRect() is the identical rect) — a tap at its centre lands on
// a tab and switches category rather than closing anything. The door out is barCloseRect(),
// the 26 px square at the right end of the strip, so that is what the close taps below use.
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
//
// v1.9.1 BAR re-pin: 486 -> 516, +30. Every one of the 30 is an ADDITION; nothing was dropped.
//
// +29 of it was the port itself, predicted by counting the edits BEFORE the first run and
// measured at exactly 515 on it. The 30th came out of that run: see the last item.
//   +9   one per screen assertion that became two — `screen == UI_SCR_BAR` plus an onTab()
//         naming the tab. The six old `== UI_SCR_CHEST` checks, the one `== UI_SCR_INVENTORY`,
//         and the two `== UI_SCR_FURNACE`.
//   +18  testChestTransfersNeverFireOffTheChestScreen swept 2 screens (HUD, INVENTORY) at 18
//         checks each; the non-container states are now 3 (HUD, INVENTORY tab, CRAFT tab),
//         because CRAFT is a tab a player can reach with a chest still open and the old model
//         had no equivalent of it.
//   +1   testPanelDraws... now asserts the close TAB (barCloseRect) as well as the strip; the
//         strip and the retired chestCloseRect are the same rect, the close tab is new.
//   +1   the close frame's UiResult went from 2 checks to 3: bar_open is now TRUE on the frame
//         that closes (the bar is what drew), so !inventory_open could not simply be renamed —
//         it flipped, and chest_open joins it because that is the flag main.c's S6 gate reads.
//   +1   testChestTabWithNoChestFallsBackToTheBagTab's one `nothingLifted(&ui)` became two
//         checks, one per lift, because they no longer agree: the bag one is clear and the
//         chest one is STALE. That check ran RED on the first green-compiling run and the
//         defect it found is written up at the check itself — read it before re-pinning.
// 2026-09-07: 516 -> 521, +5. testBagToBagOnChestScreenOntoAFullSameStackMovesNothingAndClears-
// TheLift, added to lock in source/scene/ui.c:257's discarded invBridgeMoveUnits() return —
// see that call site's comment and net/inv_bridge.h. All 5 are additions; nothing was dropped.
#define UI_CHEST_TEST_EXPECTED_CHECKS 521

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
	// keys_held (v1.9.0 SPLIT) then keys_down (v1.9.1 BAR), both zero: no key held and no press
	// edge, so neither uiGestureFeed nor barNavInput claims anything on these frames. Spelled
	// out rather than left to a short initialiser — the stanza builds at -Werror with
	// -Wmissing-field-initializers, so a field added to UiInput has to be answered here.
	const UiInput in = { down, x, y, 0, 0 };
	return uiUpdateDraw(ui, inv, icons, stats, &in, NULL, chest);
}

// ── Which TAB is focused ──────────────────────────────────────────────────────────────────
//
// nav.cat is an INDEX into the live category list, not a kind, so this resolves it the way
// uiUpdateDraw itself does: barBuildKinds() from the two container pointers, indexed by
// nav.cat. Asserting the KIND and never the raw index is the point — the container is index 2
// only because barBuildKinds puts it there, and a check that pinned the number 2 would keep
// passing if the tab that lived at 2 changed underneath it. An out-of-range cat is false, not
// a silent fallback to INVENTORY, so a cursor left off the end shows up as a failure here.
static bool onTab(const UiState* ui, BarKind want, bool has_chest, bool has_furnace)
{
	BarKind   kinds[BAR_MAX_TABS];
	const int n = barBuildKinds(has_chest, has_furnace, kinds);
	return ui->nav.cat >= 0 && ui->nav.cat < n && kinds[ui->nav.cat] == want;
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
	// The main grid is drawn at its RELOCATED position (CHEST_GRID_Y 160) and not where the
	// INVENTORY tab draws it (GRID_Y 72).
	//
	// The witness is grid slot 8 — the second grid ROW, at y 112 — and not slot 0, which is what
	// this check named until v1.9.1 and what MEASURED red on the first run of the ported suite.
	// v1.9.1 moved GRID_Y to BAR_STRIP_Y + BAR_STRIP_H + 6 = 72, which is exactly CHEST_ROW_Y,
	// so gridSlotRect(0) and chestSlotRect(0) are now the SAME rect and "the bag grid is not
	// drawn at 72" is no longer a statement the chest tab can satisfy: its own eight chest cells
	// live there. Nothing on the chest tab draws at y 112, so slot 8 still says the thing this
	// check was written to say.
	CHECK(!drewRect(gridSlotRect(8)));

	// v1.9.1 BAR: the band at y 40..66 is the tab strip, not a full-width close bar. Both quads
	// are asserted — the strip background AND the close tab at its right end — because the strip
	// alone is the same rect the retired chestCloseRect() named, so checking only that would
	// pass unchanged on a build that had lost the door out entirely.
	CHECK(drewRect(barStripRect()));
	CHECK(drewRect(barCloseRect()));
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
	CHECK(ui.screen == UI_SCR_BAR);                    // a transfer does not close the bar...
	CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));    // ...and does not change the tab either
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
	CHECK(ui.screen == UI_SCR_BAR);
	CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));   // and none of those taps changed the tab
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

// 2026-09-07. source/scene/ui.c:257 discards invBridgeMoveUnits()'s return (units actually
// moved) rather than checking it — net/inv_bridge.h documents that return as "exactly what
// inventoryMoveUnits returned", never a success/failure signal, and the call site clears the
// lift unconditionally right after regardless of what came back. This is the witness for the
// zero case: a bag -> bag drop (still routed through handleSlotTap, same as the test above,
// just onto a FULL same-item stack this time) must move nothing on either slot and still end
// the gesture, exactly like the chest panel's own analogous case
// (testPlaceOnAFullSameStackMovesNothingAndClearsTheLift). uiChestStubNet()->calls == 0 on top
// of that proves invBridgeMoveUnits() itself took the moved-into-zero branch and skipped the
// wire report (net/inv_bridge.c: "if (moved > 0 ...)") — not merely that the slots happen to
// look unchanged some other way.
static void testBagToBagOnChestScreenOntoAFullSameStackMovesNothingAndClearsTheLift(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);
	setBag(&inv, INV_HOTBAR_SLOTS + 3, BLOCK_STONE, INV_STACK_MAX);   // the drop target: full
	const Inventory inv0 = inv;
	uiChestStubResetNet();

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));
	CHECK(ui.picked_slot == 0);
	tapRect(&ui, &inv, &cs, chestGridSlotRect(3));   // relocated bag grid: slot INV_HOTBAR_SLOTS+3

	CHECK(bagEq(&inv, &inv0));                  // moved == 0: neither slot changed
	CHECK(nothingLifted(&ui));                  // the lift still clears on a zero-unit move
	CHECK(bagUnits(&inv, BLOCK_STONE) == 5 + INV_STACK_MAX);   // conservation
	CHECK(uiChestStubNet()->calls == 0);        // moved == 0: invBridgeMoveUnits sent nothing
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
		const UiResult r_close = tapRect(&ui, &inv, &cs, barCloseRect());
		CHECK(ui.screen == UI_SCR_HUD);
		CHECK(nothingLifted(&ui));
		// The tap frame DREW the bar, so it REPORTS the bar — the close takes effect next frame,
		// the same "a change applies next frame" rule the old `!inventory_open` here was reading
		// off a screen enum. That flag flipped rather than being renamed: the chest screen was
		// never the inventory overlay, but it IS the bar.
		CHECK(r_close.bar_open);
		CHECK(!r_close.furnace_open);
		// ...and it still reports the chest, which is what makes main.c's seam S6
		// (`if (!ures.chest_open) s_chest_open = false;`) close it on the NEXT frame — the frame
		// that leaves the bar is the last one handed a chest and the last one that packs back.
		CHECK(r_close.chest_open);
		CHECK(bagEq(&inv, &inv0));        // a cancelled lift moved nothing
		CHECK(chestEq(&cs, &cs0));

		// Next frame the HUD is what draws; the chest pointer still handed in must be inert.
		const UiResult r_next = frame(&ui, &inv, &cs, false, 0, 0);
		CHECK(!r_next.bar_open);
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
		tapRect(&ui, &inv, &cs, barCloseRect());
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
		// The bar on its INVENTORY tab: uiInit's memset leaves nav.cat 0, and category 0 is
		// INVENTORY on every list barBuildKinds produces. Written as a screen assignment because
		// ui.h still says setting the field directly is legal — it just is not a contract.
		ui.screen = UI_SCR_BAR;
		inventoryInit(&inv);
		chestStateInit(&cs);
		setBag(&inv, 2, BLOCK_STONE, 5);
		const Inventory inv0 = inv;

		tapRect(&ui, &inv, NULL, hotbarSlotRect(2));   // lift on the bag tab
		CHECK(ui.picked_slot == 2);

		uiOpenChest(&ui, 9, 64, 3);
		CHECK(ui.screen == UI_SCR_BAR);
		CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));   // opened ON the container tab
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

static void testChestTabWithNoChestFallsBackToTheBagTab(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);
	setChest(&cs, 0, BLOCK_DIRT, 1);

	tapRect(&ui, &inv, &cs, chestSlotRect(0));   // a chest lift, then the chest goes away
	CHECK(ui.picked_chest == 0);

	// v1.9.1 BAR replaced the old same-frame screen correction with something narrower: the
	// category list is rebuilt from the two pointers EVERY frame, so a chest that stopped being
	// handed in simply has no tab, and barNavClamp puts the cursor back on category 0 —
	// INVENTORY, which always exists. The screen itself never needed correcting; it was already
	// UI_SCR_BAR. The tap lands where chest slot 0 was, which on the bag tab is a grid cell
	// (gridSlotRect and chestSlotRect are the same rect at GRID_Y 72) — an empty one, so it
	// lifts nothing.
	const URect r = chestSlotRect(0);
	const UiResult res = tapAt(&ui, &inv, NULL, r.x + r.w / 2, r.y + r.h / 2);

	CHECK(ui.screen == UI_SCR_BAR);
	CHECK(onTab(&ui, BAR_KIND_INVENTORY, false, false));   // clamped off the tab that went away
	CHECK(res.bar_open);         // clamped BEFORE the frame's flags were read: the bar drew

	// FIXED 2026-09-07. This test used to be called ...AndDropsBothLifts, then was renamed and
	// re-pinned to MEASURE a defect: barNavClamp moved the cursor but nothing in source/scene/
	// ui.c ever cleared picked_chest when the chest pointer stopped being handed in, so it kept
	// naming slot 0 of a chest that no longer existed — the next chest opened would have been
	// read against that stale index (a wrong-chest place/withdraw). uiUpdateDraw now clears
	// picked_chest at the exact site the tab vanishes (`chest == NULL`, right after the
	// barBuildCats/barNavClamp pair above) — see the comment there for why picked_slot is left
	// alone: an ordinary tab switch never touches the bag lift either, so the automatic clamp
	// forced by the chest vanishing does not treat it differently. This is back to
	// nothingLifted()'s two components in name only — both still asserted separately, because
	// they assert two DIFFERENT things (no bag lift was ever taken here; the chest lift is now
	// correctly gone) and collapsing them would lose that distinction for the next person who
	// reads this test after a regression.
	//
	// This also restores barHasLift()'s answer: picked_chest is -1, so B on the bag tab now
	// answers BAR_EV_BACK and exits, instead of BAR_EV_CANCEL swallowing a press to clear a lift
	// the player can no longer see.
	CHECK(ui.picked_slot == -1);    // the bag lift is clear -- none was taken on this path
	CHECK(ui.picked_chest == -1);   // the chest lift is CLEARED -- see the block above
	CHECK(bagUnits(&inv, BLOCK_STONE) == 5);
	CHECK(chestSlotIs(&cs, 0, BLOCK_DIRT, 1));
}

// The chest handlers hang off `case BAR_KIND_CHEST:` in uiUpdateDraw's tap chain, i.e. off the
// FOCUSED TAB and not off the chest pointer: no other place in the UI can reach them even with
// a live ChestState handed in every frame. Every chest-panel target is tapped twice (a "lift"
// then a "place") in each state a caller could plausibly be in with the pointer still set, and
// the chest must not move.
//
// v1.9.1 BAR: three states, not two. The old model's non-chest screens were the HUD and the
// inventory overlay; the new one's are the HUD and the two non-container TABS, and CRAFT is a
// tab the player can reach with a chest still open (an L press) that had no equivalent before.
// Leaving it out would have been a state the old suite covered by construction and the new one
// did not cover at all.
static void testChestTransfersNeverFireOffTheChestTab(void)
{
	// { screen, nav.cat }. Cat is ignored on the HUD. Cat 0 is INVENTORY and cat 1 is CRAFT on
	// every list barBuildKinds produces, container open or not.
	static const struct { UiScreen screen; int cat; } states[] = {
		{ UI_SCR_HUD, 0 },
		{ UI_SCR_BAR, 0 },
		{ UI_SCR_BAR, 1 },
	};

	for (size_t k = 0; k < sizeof(states) / sizeof(states[0]); k++) {
		UiState ui; Inventory inv; ChestState cs;
		uiInit(&ui);
		ui.screen  = states[k].screen;   // ui.h: setting the field directly is legal, it just isn't a contract
		ui.nav.cat = states[k].cat;
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
		tapRect(&ui, &inv, &cs, barCloseRect());
		CHECK(chestEq(&cs, &cs0));
	}
}

// The three hit-tests a tap on the chest tab is dispatched through — the tab strip, then
// hitChestSlot(), then hitChestInvSlot() (hotbar + relocated grid) — must never claim the same
// pixel, or a tap has two meanings and the dispatch order in ui.c silently picks one. Swept
// over every pixel of the 320x240 panel. The three area checks are what stop the sweep passing
// vacuously: a rect that had collapsed to nothing would overlap nothing.
//
// v1.9.1 BAR: the first of the three is hitBarStrip() at THREE tabs (INVENTORY, CRAFT, CHEST —
// what barBuildKinds produces with a chest open), not the retired full-width chestCloseRect().
// It is the strip that a tap is now tested against first, and it claims the whole band: the
// tabs tile 0..294 and the close tab takes 294..320, so the expected area is the same
// SCR_W * BAR_STRIP_H the old close bar covered — the meaning of those pixels changed, not
// their extent.
static void testChestTabHitTestsNeverOverlap(void)
{
	int overlaps = 0, strip_px = 0, chest_px = 0, bag_px = 0;

	for (int y = 0; y < SCR_H; y++) {
		for (int x = 0; x < SCR_W; x++) {
			const int c = hitBarStrip(x, y, 3) != BAR_HIT_NONE ? 1 : 0;
			const int s = hitChestSlot(x, y)    >= 0 ? 1 : 0;
			const int b = hitChestInvSlot(x, y) >= 0 ? 1 : 0;
			strip_px += c;
			chest_px += s;
			bag_px   += b;
			if (c + s + b > 1) overlaps++;
		}
	}

	CHECK(overlaps == 0);
	CHECK(strip_px == SCR_W * BAR_STRIP_H);
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
	CHECK(ui.screen == UI_SCR_BAR);
	CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));
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

	tapRect(&ui, &inv, &cs, chestSlotRect(1));      // lift, then the close tab is not a place
	tapRect(&ui, &inv, &cs, barCloseRect());
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

static void testHookIsConsultedOnlyOnTheChestTab(void)
{
	UiState ui; Inventory inv; ChestState cs;
	uiInit(&ui);
	armSpy(&ui, true);
	// The bar on the INVENTORY tab with the chest STILL handed in every frame: the chest tab
	// exists at index 2, it is just not the focused one, which is the whole distinction.
	ui.screen = UI_SCR_BAR;
	inventoryInit(&inv);
	chestStateInit(&cs);
	setBag(&inv, 0, BLOCK_STONE, 5);
	setChest(&cs, 0, BLOCK_DIRT, 7);

	tapRect(&ui, &inv, &cs, hotbarSlotRect(0));   // lift on the bag tab
	tapRect(&ui, &inv, &cs, chestSlotRect(0));    // on the bag tab this pixel is a grid cell

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
// back if a chest was handed in, closed once UiResult.chest_open comes back false. The
// pack-back is gated on "a chest was handed in" and NOT on "still open", exactly as main.c's
// is, so a transfer and a close on the same frame would both still land.
//
// v1.9.1 BAR: the close test is main.c's seam S6, `if (!ures.chest_open) s_chest_open = false;`
// — read off the RESULT, not off ui->screen. The old `screen != UI_SCR_CHEST` cannot be
// rewritten as `screen != UI_SCR_BAR`: that is true on every frame the bar is up on any tab, so
// an L press to look at the crafting list would slam the chest shut and take its tab away. The
// pointer is what owns the container's lifetime and chest_open is what reports it.
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

	const UiInput in = { down, tx, ty, 0, 0 };   // keys_held, keys_down: neither pressed here
	const UiResult r = uiUpdateDraw(ui, inv, NULL, NULL, &in, NULL, cp);

	if (cp) {
		chestStatePack(cp, pay);
		(void)blockStateSet(&h->table, h->x, h->y, h->z, BLOCK_CHEST, pay);
	}
	if (!r.chest_open) h->open = false;
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
	CHECK(ui.screen == UI_SCR_BAR);
	CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));

	// The bytes in the table are chest.c's real format — item, count, item, count... — not a
	// copy this file keeps, so a round trip that "worked" through a broken pack would show here.
	uint8_t raw[BLOCKSTATE_PAYLOAD_BYTES];
	CHECK(blockStateGet(&h.table, h.x, h.y, h.z, BLOCK_CHEST, raw));
	CHECK(raw[0] == BLOCK_STONE && raw[1] == 99 && raw[2] == BLOCK_APPLE && raw[3] == 4);

	// Close with a lift pending. main.c's rule: the frame that leaves the bar is the last one
	// handed a chest, and it still packs back — which must be the UNCHANGED chest.
	hostTap(&h, &ui, &inv, chestSlotRect(0));
	CHECK(ui.picked_chest == 0);
	hostTap(&h, &ui, &inv, barCloseRect());
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
	// `keys` is keys_HELD — the level word scene/ui_gesture.h reads. keys_down stays 0: these
	// helpers drive the X/Y gestures, and a press-edge word would additionally hand barNavInput
	// a BAR_KEY_X/_Y and let the cursor act on the same frame, which is not what is under test.
	const UiInput in = { down, x, y, keys, 0 };
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
	ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
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
		CHECK(ui.screen == UI_SCR_BAR);
		CHECK(onTab(&ui, BAR_KIND_FURNACE, false, true));
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
		CHECK(ui.screen == UI_SCR_BAR);
		CHECK(onTab(&ui, BAR_KIND_CHEST, true, false));
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
		ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
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
		ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
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
	CHECK(ui.screen == UI_SCR_BAR);
	CHECK(onTab(&ui, BAR_KIND_FURNACE, false, true));
}

// X on the chest screen: `to_chest` is true, so the whole lifted stack is sent INTO the chest
// (ui.c's handleQuickMove over invBridgePlanChestDeposit's plan) — and a full chest refuses it,
// leaving the bag untouched, though the lift pointer is still cleared (handleQuickMove drops any
// lift BEFORE it knows whether the chest has room; see its own comment on why).
//
// Both cases here land in ONE chest slot: the first has an empty chest, the second has no room
// at all. That is why this test passed for the whole of v1.9.0 while the spill across several
// slots was broken — see testXQuickMoveSpillsAcrossEveryChestSlotWithRoom below, which is the
// case this one never reached.
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

// ── v1.9.1: X on the chest screen SPILLS ─────────────────────────────────────────────────
//
// The defect this pair exists for. Until v1.9.1 handleQuickMove picked ONE chest slot — the
// first same-item slot with ANY room, else the first empty one — and sent the player's whole
// stack to it, while its own comment claimed it applied invBridgePlanQuickMove's strip rule.
// It did not: that rule spills across every eligible slot. Nothing caught it because the only
// chest-X coverage was testXQuickMoveOnChestScreenDepositsIntoTheChest above, whose two cases
// are "empty chest" and "no room anywhere" — the two shapes where one slot is the right answer.
//
// The bug was under-delivery, not loss: deps/blocksmith-server/game/bsgame.c clamps a deposit
// to the slot's remaining room and debits exactly what it took, so the surplus stayed in the
// bag. Silent, and with no refusal shown, which is the harder kind to notice in play.

// A transfer spy that keeps EVERY call, not just the last, and records ui->picked_slot as the
// callback saw it. s_spy above cannot be used: spyGot() hard-codes `calls == 1`, and a spill is
// several calls by construction. picked_slot is captured because it is the subtle half of the
// fix — chestTransferViaFn calls dropLifts() on a successful send, which sets picked_slot back
// to -1, so a loop that set it once before the loop would leave every deposit after the first
// with no source slot for the registrant to read (scene/ui.h says a registrant may read it).
#define SPILL_SPY_MAX 8

static struct {
	int     calls;
	uint8_t op[SPILL_SPY_MAX], a[SPILL_SPY_MAX], b[SPILL_SPY_MAX], count[SPILL_SPY_MAX];
	int     picked[SPILL_SPY_MAX];
	bool    answer;
} s_spill;

// `ud` is the UiState itself, so the spy can read picked_slot as it stood during the call.
static bool spillSpy(void* ud, uint8_t op, int x, int y, int z, uint8_t a, uint8_t b,
                     uint8_t count)
{
	(void)x; (void)y; (void)z;
	if (s_spill.calls < SPILL_SPY_MAX) {
		const int i = s_spill.calls;
		s_spill.op[i]     = op;
		s_spill.a[i]      = a;
		s_spill.b[i]      = b;
		s_spill.count[i]  = count;
		s_spill.picked[i] = ((const UiState*)ud)->picked_slot;
	}
	s_spill.calls++;
	return s_spill.answer;
}

// Chest slot 0 has 9 units of room, slot 1 has 4, slots 2-7 are empty, and the player is
// holding 20. The strip rule tops up 0 then 1 then fills the first empty slot, so all 20 land.
// The old one-slot code moved 9 and left 11 in the bag.
static void testXQuickMoveSpillsAcrossEveryChestSlotWithRoom(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 0, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 9));
	setChest(&cs, 1, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 4));
	setBag(&inv, 3, BLOCK_STONE, 20);
	const int total = units(&inv, &cs, BLOCK_STONE);
	uiChestStubResetNet();

	tapRectKeys(&ui, &inv, NULL, &cs, hotbarSlotRect(3));
	CHECK(ui.picked_slot == 3);

	pressKeyOnLift(&ui, &inv, NULL, &cs, UI_GESTURE_KEY_X);

	CHECK(chestSlotIs(&cs, 0, BLOCK_STONE, INV_STACK_MAX));   // topped up by 9
	CHECK(chestSlotIs(&cs, 1, BLOCK_STONE, INV_STACK_MAX));   // topped up by 4
	CHECK(chestSlotIs(&cs, 2, BLOCK_STONE, 7));               // the remaining 7 to the first empty
	CHECK(bagSlotIs(&inv, 3, ITEM_NONE, 0));                  // and NOTHING is left behind
	CHECK(nothingLifted(&ui));
	CHECK(units(&inv, &cs, BLOCK_STONE) == total);            // conservation across the spill
	CHECK(uiChestStubNet()->calls == 0);   // the chest path never goes through inv_bridge

	// A chest that cannot take all of it still takes everything it CAN, and the surplus stays
	// in the bag rather than vanishing — the clamp, checked on this side of the wire too.
	{
		UiState ui2; Inventory inv2; ChestState cs2;
		openChest(&ui2);
		inventoryInit(&inv2);
		chestStateInit(&cs2);
		fillChest(&cs2, BLOCK_DIRT, INV_STACK_MAX);              // every slot occupied...
		setChest(&cs2, 6, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 3));   // ...except 3 units of room
		setBag(&inv2, 3, BLOCK_STONE, 20);
		const int total2 = units(&inv2, &cs2, BLOCK_STONE);

		tapRectKeys(&ui2, &inv2, NULL, &cs2, hotbarSlotRect(3));
		pressKeyOnLift(&ui2, &inv2, NULL, &cs2, UI_GESTURE_KEY_X);

		CHECK(chestSlotIs(&cs2, 6, BLOCK_STONE, INV_STACK_MAX));
		CHECK(bagSlotIs(&inv2, 3, BLOCK_STONE, 17));   // 20 - 3, still in hand
		CHECK(units(&inv2, &cs2, BLOCK_STONE) == total2);
	}
}

// The same spill under a session: nothing is applied locally (the server owns it), and each
// piece goes out as its own DEPOSIT naming its own chest slot and its own count.
static void testXQuickMoveSpillSendsOneDepositPerChestSlot(void)
{
	UiState ui; Inventory inv; ChestState cs;
	openChest(&ui);
	memset(&s_spill, 0, sizeof(s_spill));
	s_spill.answer = true;                       // "sent — the server owns it"
	uiSetChestTransferFn(&ui, spillSpy, &ui);    // ud is the UiState: see spillSpy's comment
	inventoryInit(&inv);
	chestStateInit(&cs);
	setChest(&cs, 0, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 9));
	setChest(&cs, 1, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 4));
	// Bag slot 5, not 3: BLOCK_STONE IS 3, so a lift from slot 3 could not tell "a = item id"
	// from "a = source bag slot" apart — the same reason the hook tests above use slot 5.
	setBag(&inv, 5, BLOCK_STONE, 20);
	const Inventory  inv0 = inv;
	const ChestState cs0  = cs;

	tapRectKeys(&ui, &inv, NULL, &cs, hotbarSlotRect(5));
	CHECK(ui.picked_slot == 5);
	CHECK(s_spill.calls == 0);   // a lift is not a transfer

	pressKeyOnLift(&ui, &inv, NULL, &cs, UI_GESTURE_KEY_X);

	CHECK(s_spill.calls == 3);
	CHECK(s_spill.op[0] == UI_CHEST_OP_DEPOSIT && s_spill.a[0] == (uint8_t)BLOCK_STONE
	      && s_spill.b[0] == 0 && s_spill.count[0] == 9);
	CHECK(s_spill.op[1] == UI_CHEST_OP_DEPOSIT && s_spill.a[1] == (uint8_t)BLOCK_STONE
	      && s_spill.b[1] == 1 && s_spill.count[1] == 4);
	CHECK(s_spill.op[2] == UI_CHEST_OP_DEPOSIT && s_spill.a[2] == (uint8_t)BLOCK_STONE
	      && s_spill.b[2] == 2 && s_spill.count[2] == 7);

	// The re-set: every deposit names the source bag slot, not just the first. A loop that set
	// picked_slot once would give 5, -1, -1 here, because a successful send clears it.
	CHECK(s_spill.picked[0] == 5 && s_spill.picked[1] == 5 && s_spill.picked[2] == 5);

	CHECK(bagEq(&inv, &inv0));    // applied nothing locally, either side
	CHECK(chestEq(&cs, &cs0));
	CHECK(nothingLifted(&ui));
	CHECK(ui.picked_slot == -1);  // and X holds nothing once the whole spill is out

	uiSetChestTransferFn(&ui, NULL, NULL);
}

// ── v1.9.1: a server snapshot must not duplicate a detached split half ───────────────────
//
// The most serious defect found in the v1.9.0 sweep. A Y split moves half a stack into
// ui->lift, which lives in UiState and NOT in Inventory, and is deliberately never reported to
// the server (net/inv_bridge.h). main.c's inventory hook used to call invBridgeApplyState
// directly, which overwrites every slot wholesale from the server's copy — a copy that still
// showed the whole undivided stack in the origin slot, because it was never told about the
// split. The origin came back full while the lifted half was still held separately: units
// created from nothing, repeatable without bound.
//
// Reaching it needed no exotic input. The bag screen is not the pause menu, so L/R hotbar
// cycling and the craft rows both work with a half in the air, and the server answers EVERY
// accepted inventory action with a fresh snapshot (deps/blocksmith-server/game/bsgame.c's
// handle_inv_action ends in an unconditional send_inv_state). Split, nudge L, place.
//
// This drives the REAL production path: uiApplyInvSnapshot() is what main.c's onInvState calls
// now, so a future edit that routes around it fails here rather than on a console.

// The server's view of the bag: one slot, one item, the count it believes is there. Everything
// else empty, which is what the server would report for a bag holding only this stack.
static NetworldInvState serverSnapshot(int slot, ItemId item, uint8_t count)
{
	NetworldInvState s;
	memset(&s, 0, sizeof(s));
	s.selected_hotbar   = 0;
	s.slots[slot].item  = (uint8_t)item;
	s.slots[slot].count = count;
	return s;
}

static void testSnapshotDuringASplitDropsTheLiftInsteadOfDuplicatingIt(void)
{
	UiState ui; Inventory inv;
	uiInit(&ui);
	ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
	inventoryInit(&inv);
	setBag(&inv, 4, BLOCK_STONE, 80);

	tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(4));
	pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_Y);

	// Mid-air: 40 in the lift, 40 left behind, 80 accounted for. The server still thinks 80.
	CHECK(ui.lift.item == BLOCK_STONE && ui.lift.count == 40);
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 40));
	CHECK(bagUnits(&inv, BLOCK_STONE) + ui.lift.count == 80);

	// The snapshot the server sends in reply to an unrelated action (an L/R hotbar nudge, a
	// craft): its copy of the origin slot, still undivided.
	const NetworldInvState snap = serverSnapshot(4, BLOCK_STONE, 80);
	uiApplyInvSnapshot(&ui, &inv, &snap);

	// picked_slot is deliberately NOT cleared, so nothingLifted() (which reads picked_slot and
	// picked_chest, not the detached lift) is the wrong question here. It stays naming slot 4:
	// it is an INDEX, nothing is conserved through it, and the panel re-reads that slot every
	// frame, so the player sees the restored 80 under the highlight. See uiApplyInvSnapshot.
	CHECK(ui.picked_slot == 4);
	CHECK(ui.lift.item == ITEM_NONE && ui.lift.count == 0);   // the half is gone from the air...
	CHECK(ui.lift_from == -1);
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 80));       // ...and back inside the count the server sent

	// The whole point, stated as conservation rather than as slot contents: 80 units existed
	// before the split and 80 exist after the snapshot. The old code left this at 120.
	CHECK(bagUnits(&inv, BLOCK_STONE) + ui.lift.count == 80);
	CHECK(bagTotal(&inv) == 80);
}

// The same guard with NOTHING in the air: an ordinary snapshot must still apply in full. A fix
// that dropped the SNAPSHOT instead of the lift would pass every check above while breaking
// what invBridgeApplyState is actually for, so this is the other half of the pair.
static void testSnapshotWithNoLiftStillAppliesWholesale(void)
{
	UiState ui; Inventory inv;
	uiInit(&ui);
	ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
	inventoryInit(&inv);
	setBag(&inv, 4, BLOCK_STONE, 12);
	setBag(&inv, 7, BLOCK_DIRT,  3);

	const NetworldInvState snap = serverSnapshot(2, BLOCK_STONE, 55);
	uiApplyInvSnapshot(&ui, &inv, &snap);

	CHECK(bagSlotIs(&inv, 2, BLOCK_STONE, 55));   // the server's slot took effect
	CHECK(bagSlotIs(&inv, 4, ITEM_NONE, 0));      // and the local ones it did not mention are gone
	CHECK(bagSlotIs(&inv, 7, ITEM_NONE, 0));
	CHECK(bagTotal(&inv) == 55);
	CHECK(nothingLifted(&ui));

	// A NULL is a refusal that changes nothing, not a crash. main.c hands this the address of
	// a static, so neither case arises there today; they are checked because this function is
	// now the only door into invBridgeApplyState and a door should not have a hole in it.
	uiApplyInvSnapshot(&ui, &inv, NULL);
	CHECK(bagTotal(&inv) == 55);
	uiApplyInvSnapshot(NULL, &inv, &snap);
	CHECK(bagTotal(&inv) == 55);
}

// v1.9.1, the second half of the same defect. ui.c's returnLift() used to clear lift_from
// UNCONDITIONALLY, even when invBridgeReturnLift refused a remainder (its contract: "units the
// bag would not take back are still in lift"). With lift_from at -1, reconcileLift's
// `picked_slot != lift_from` test went false as soon as picked_slot reset to -1 as well, so
// nothing ever retried; liftDetached() stayed true, and handleSplit's `if (liftDetached) return`
// refused every later split for the rest of the session. Keeping the origin index instead makes
// it self-heal on the next frame that names another slot.
//
// The precondition -- a full bag with a DIFFERENT item in the origin slot -- can only arise
// from a desync, which the snapshot fix above now prevents at source. It is fixed and checked
// anyway: the whole reason the duplication existed is that a piece of state was left in a
// condition no caller expected, and a lift that can never be returned is that same shape.
static void testARefusedLiftReturnKeepsItsOriginAndRetriesLater(void)
{
	UiState ui; Inventory inv;
	uiInit(&ui);
	ui.screen = UI_SCR_BAR;   // nav.cat 0 = the INVENTORY tab, left there by uiInit's memset
	inventoryInit(&inv);

	// Every slot full, so inventoryAdd's fallback inside invBridgeReturnLift has nowhere to
	// put a remainder. BLOCK_STONE + i would collide with the split slot's own item, so the
	// origin gets its own item and the rest get another.
	for (int i = 0; i < INV_SLOT_COUNT; i++) setBag(&inv, i, BLOCK_DIRT, INV_STACK_MAX);
	setBag(&inv, 4, BLOCK_STONE, INV_STACK_MAX);

	tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(4));
	pressKeyOnLift(&ui, &inv, NULL, NULL, UI_GESTURE_KEY_Y);
	CHECK(ui.lift.item == BLOCK_STONE);
	CHECK(ui.lift.count == 50);   // ceil(99/2)
	CHECK(ui.lift_from == 4);

	// The desync, staged directly: the origin now holds a different item, at the cap, and every
	// other slot is full of that item too. invBridgeReturnLift's liftInto refuses on the item
	// mismatch and its inventoryAdd fallback has no room, so the whole 50 come back refused.
	// (This throws away the 49 left behind by the split, so no conservation check here — the
	// point of the fixture is the refusal, not the arithmetic, which the tests above cover.)
	setBag(&inv, 4, BLOCK_DIRT, INV_STACK_MAX);

	// Tap the origin again: handleSlotTap's cancel branch (ui.c:203-207), which is returnLift
	// followed by picked_slot = -1. That -1 is what used to make the loss permanent.
	tapRectKeys(&ui, &inv, NULL, NULL, hotbarSlotRect(4));

	CHECK(ui.lift.item == BLOCK_STONE);   // refused: still in the air
	CHECK(ui.lift.count == 50);
	CHECK(ui.lift_from == 4);   // and it REMEMBERS where it came from. The old code put -1 here.
	CHECK(ui.picked_slot == -1);

	// Free the origin, then give the UI a frame: reconcileLift sees picked_slot != lift_from
	// and retries the return that previously could never happen again.
	setBag(&inv, 4, ITEM_NONE, 0);
	(void)frameKeys(&ui, &inv, NULL, NULL, false, 0, 0, 0);

	CHECK(ui.lift.item == ITEM_NONE);   // recovered
	CHECK(ui.lift.count == 0);
	CHECK(ui.lift_from == -1);
	CHECK(bagSlotIs(&inv, 4, BLOCK_STONE, 50));   // back where it was lifted from
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
// the only place an overrun would ever be seen is here. Worst case for each view: every
// slot full with a two-digit count (a count badge is two glyphs), a dummy atlas texture so
// the icon pass runs, and for the HUD a UiStats with every optional line present. Two of the
// three numbers are printed for the record; all three are asserted under the ceiling.
//
// v1.9.1 BAR: what is measured is a TAB, not a screen — the three screens collapsed into one.
// `tab` is the nav.cat to draw, or -1 for the HUD. The container tab (2) is reached the way
// main.c reaches it, through uiOpenChest, rather than by writing nav.cat: that call also sets
// opened_this_frame and drops any lift, and measuring a state main.c cannot produce would be
// measuring nothing. The strip is drawn on every tab and is part of every bar number here.
static int quadsForTab(int tab, ChestState* cs, const UiStats* stats)
{
	static C3D_Tex dummy_atlas;   // never dereferenced by ui.c (ui.h: it only passes it on)
	UiState ui; Inventory inv;
	uiInit(&ui);
	if (tab == 2)           uiOpenChest(&ui, CHEST_X, CHEST_Y, CHEST_Z);
	else if (tab >= 0)    { ui.screen = UI_SCR_BAR; ui.nav.cat = tab; }
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

	const int hud   = quadsForTab(-1, NULL, &stats);   // the HUD
	const int inven = quadsForTab(0, NULL, NULL);      // the INVENTORY tab
	const int chest = quadsForTab(2, &cs, NULL);       // the CHEST tab
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
	testBagToBagOnChestScreenOntoAFullSameStackMovesNothingAndClearsTheLift();
	testEveryBagCellDepositsItselfAndEveryChestCellWithdrawsItself();

	testCloseReturnsToTheHudAndClearsAnyLift();
	testOpenChestCancelsAPendingLiftAndRecordsThePosition();
	testChestTabWithNoChestFallsBackToTheBagTab();
	testChestTransfersNeverFireOffTheChestTab();
	testChestTabHitTestsNeverOverlap();

	testHookDepositSendsTheItemIdAndTheChestSlotAndAppliesNothing();
	testHookWithdrawSendsTheChestSlotAndTheBagSlotAndAppliesNothing();
	testHookRunsWhileTheLiftIsStillRecorded();
	testHookRefusingChangesNothingAndKeepsTheLift();
	testHookIsNotConsultedForLiftsCancelsBagToBagOrTheCloseBar();
	testHookRefusesChestToChestWithoutBeingCalled();
	testHookIsConsultedOnlyOnTheChestTab();
	testClearingTheHookRestoresTheLocalApply();

	testContentsSurviveCloseAndReopenThroughTheRealBlockstateTable();

	testYSplitsAnOddStackCeilToTheLiftFloorBehind();
	testYIsInertOnFurnaceAndChestScreens();
	testXQuickMovesTheWholeStackToTheOtherStripOnTheOverlay();
	testXQuickMoveOnFurnaceScreenMovesWithinTheBagNotIntoTheFurnace();
	testXQuickMoveOnChestScreenDepositsIntoTheChest();
	testXQuickMoveSpillsAcrossEveryChestSlotWithRoom();
	testXQuickMoveSpillSendsOneDepositPerChestSlot();
	testSnapshotDuringASplitDropsTheLiftInsteadOfDuplicatingIt();
	testSnapshotWithNoLiftStillAppliesWholesale();
	testARefusedLiftReturnKeepsItsOriginAndRetriesLater();
	testXIsInertOnTheHudScreen();

	testEveryScreenFitsTheSpriteQuadBudget();

	checkCountPin();

	if (s_fails == 0)
		printf("chest_ui self-test: PASS  %d checks\n", s_checks);
	else
		printf("chest_ui self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}
