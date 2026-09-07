#include "scene/ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/audio_sfx.h"
#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/item_icons.h"   // itemIconTile, for the drops that have real item art
#include "gfx/sprite.h"
#include "debug/biomeinfo.h"
#include "net/inv_bridge.h"
#include "scene/chunk_render.h"   // chunkRenderCamera, for the biome row's position
#include "scene/ui_layout.h"
#include "world/block.h"
#include "world/chest.h"     // ChestState, CHEST_SLOTS — see handleChestSlotTap/handleChestInvSlotTap
#include "world/crafting.h"
#include "world/furnace.h"   // furnaceRecipeForInput / furnaceIsFuel, for the accept rules

// ── v1.9.1 BAR: the key-bit mirror, proved where a real pad word arrives ────────────────
//
// scene/barnav.h mirrors libctru's KEY_* bits so it can be host-tested without <3ds.h>, the
// same carve-out scene/hotbar.h and scene/ui_gesture.h already make. barnav.c asserts the
// mirror for its own half; THIS file is where a real hidKeysDown() word (main.c seam S1,
// UiInput.keys_down) is handed to barNavInput, so the claim has to hold here too — a caller
// that passed the raw word while the mirror had drifted would get silently wrong buttons, with
// nothing anywhere to say so. Guarded because the host stanzas that link this file
// (tests/ui_chest_test.c) have no libctru to compare against; the console build is what proves
// it, exactly as blueprint §6.3's S13 says.
#ifdef __3DS__
#include <3ds.h>
_Static_assert(BAR_KEY_A == KEY_A && BAR_KEY_B == KEY_B && BAR_KEY_L == KEY_L &&
               BAR_KEY_R == KEY_R && BAR_KEY_DUP == KEY_DUP && BAR_KEY_DDOWN == KEY_DDOWN &&
               BAR_KEY_DLEFT == KEY_DLEFT && BAR_KEY_DRIGHT == KEY_DRIGHT,
               "barnav key bits mirror libctru");
#endif

// ── Interaction model: tap to pick up, tap again to drop ─────────────────────────────────
//
// inventory.h's own primitives (inventorySwapSlots / inventoryMoveUnits) are built around
// "here is a source slot and a destination slot", which is exactly what a continuous drag
// gesture produces too — but a continuous drag needs the touch position tracked smoothly
// frame to frame, a start point remembered from before the current frame's read, and a
// decision about what counts as "still dragging" versus "let go", none of which this
// project's touch handling anywhere else does (title.c's own uiButton is a single rising-
// edge tap, nothing more — see title.h's TitleInput comment). A resistive stylus panel is
// also the more failure-prone place to trust continuous sampling: a real drag can report a
// jittery path if the contact pressure varies over the drag, whereas a single tap is either
// registered where the finger actually was or it is not registered at all.
//
// So this file uses two taps instead of one drag: the first tap on a non-empty slot lifts
// its whole stack (ui->picked_slot = that slot, see handleSlotTap below); the second tap
// resolves against whatever is now under the finger — the same slot again cancels the pick-
// up, an empty or matching slot merges/relocates the whole stack via inventoryMoveUnits, and
// a slot holding a different item swaps the two stacks via inventorySwapSlots, which is
// exactly what inventory.h's own comment describes a "plain drag-and-drop (no merge)" as
// doing. Both taps are ordinary rising-edge touch reads — nothing here remembers a
// coordinate from a frame where touch_down was already true — so it needs nothing this file
// doesn't already have to track for the rest of the screen.

// ── Layout ─────────────────────────────────────────────────────────────────────────────
//
// The Old 3DS's bottom touch panel is documented as 3.02" diagonal at 320x240 (0/GPU
// resolution == physical dot count on this panel, unlike the top screen's pixel-doubling).
// diag_px = sqrt(320^2 + 240^2) = 400, so one dot pitch is (3.02 * 25.4) / 400 = 0.1918 mm,
// i.e. 1 mm = 5.216 px on both axes (the panel is exactly 4:3, so the same pitch serves
// both). scene/title.c's own header cites a different figure for the same panel (53.0 x
// 39.75 mm, 6.04 px/mm); the two disagree, and this file follows the 5.216 px/mm figure
// because it is what world/inventory.h's own comment already computed 40 px hotbar cells
// against ("about 7.6mm") — this file's hotbar geometry has to match the cell size that
// comment already committed to, so it uses the same conversion that comment used rather
// than reconciling which citation is more accurate.
#define PX_PER_MM 5.216f

// SCR_W/SCR_H and every layout constant derived from them (SLOT_PX, HOTBAR_*, GRID_*,
// CRAFT_*, HUD_TOGGLE_*, HUD_STATUS_Y0) now live in scene/ui_layout.h — see that header for
// the numbers (unchanged) and ui_layout.c for the reasoning comments that used to sit here.
// They moved out so the pixel/hit-test arithmetic could be host-tested (ui_layout_test.c)
// without pulling in this file's <citro3d.h> dependency; nothing about the pixels they
// produce changed in the move.

// One slot's icon sits inset from the cell's top-left, leaving the bottom-right corner free
// for the count badge (see drawSlotIcon). This is not just cosmetic spacing: the icon is
// drawn from the block atlas texture and the badge from the font texture, and
// gfx/sprite.h's batch flushes on every texture change (see sprite.h's own file comment on
// why it batches at all). Reserving disjoint pixels for the two, rather than overlapping
// them and relying on draw order, is what keeps this screen at exactly two draw calls
// (font, then atlas) instead of needing a third font pass on top of the icons to redraw
// every badge that an icon would otherwise have painted over — see uiUpdateDraw's own
// comment on the two-pass split for the count.
#define ICON_INSET_X   3
#define ICON_INSET_TOP 2
#define ICON_H         26   // icon spans rows [2, 28) of the 40-px cell; the badge (see
                             // drawSlotIcon) sits at row 31, a clear 3 px below it

// v1.8.16 IMP-ICONS. The icon QUAD is square; ICON_INSET_X is not its inset any more.
//
// It was. The quad was drawn (SLOT_PX - 2*ICON_INSET_X) = 34 px wide and ICON_H = 26 px tall
// from a 16x16 atlas tile, which is a 31% horizontal stretch. On a block face that is
// invisible — dirt stretched is still dirt — so it survived from step 8.2 unnoticed. It stops
// being invisible the moment an icon has a SHAPE: a round apple came out an oval, which is
// the wrong shape, which is the thing steve was complaining about. So the quad is now
// ICON_H x ICON_H and centred in the cell.
//
// ICON_INSET_X stays 3 and keeps its old meaning for the FONT fallback in drawSlotIcon —
// that path draws the block's NAME when no atlas texture was handed in, and its comment's
// "34px icon column at FONT_ADVANCE(6) fits every <=6-char name" arithmetic depends on the 3.
// Narrowing that to 26 px would have cut every name to four characters. Two different things
// wanted two different numbers; they now have them.
//
// Geometry check, against the cell the badge and the icon share (see the note above on why
// they must not overlap): the quad spans x [7, 33) of the 40-px cell — a 7 px margin each
// side, so it cannot leave the slot — and rows [2, 28), which ICON_INSET_TOP/ICON_H already
// fixed and this does not touch. The badge is unmoved at rows [31, 38). Disjoint in y, which
// is what the two-draw-call guarantee actually rests on; the x narrowing only adds slack.
#define ICON_W       ICON_H                        // 26 — square, so a 16x16 tile is unstretched
#define ICON_QUAD_X  ((SLOT_PX - ICON_W) / 2)      // 7 — centred, where ICON_INSET_X was 3

// ── Palette ────────────────────────────────────────────────────────────────────────────

#define COL_BG        SPRITE_RGBA(26, 20, 36, 255)
#define COL_PANEL     SPRITE_RGBA(52, 40, 76, 255)
#define COL_PANEL_HI  SPRITE_RGBA(90, 74, 130, 255)
#define COL_SLOT_BG   SPRITE_RGBA(40, 32, 58, 255)
#define COL_TEXT      SPRITE_WHITE
#define COL_TEXT_DIM  SPRITE_RGBA(180, 170, 200, 255)
#define COL_ACCENT    SPRITE_RGBA(255, 226, 150, 255)   // hotbar "in hand" selection
#define COL_PICKED    SPRITE_RGBA(120, 200, 255, 255)   // slot lifted for a move
#define COL_CRAFT_OK  SPRITE_RGBA(70, 110, 70, 255)     // recipe currently makeable

// v1.8.13 SURV-HUD. Three states per pip, and the half state gets its own colour rather than
// being rounded away — see drawPipRow for the whole reading of it. The empty track is a lift
// off COL_BG rather than COL_BG itself, so a drained bar still reads as ten pips that are
// empty instead of as a bar that has vanished; that is the difference between "you are at
// zero" and "the HUD stopped drawing", and at 12x8 px it is the only thing telling them apart.
// v1.8.15 FURNACE. The two indicator bars reuse COL_PIP_TRACK for their empty track, so an
// empty burn bar and an empty pip read as the same kind of "this is a gauge and it is at
// zero" rather than as two different absences — same argument the pip track's own comment
// below makes, applied to a second gauge instead of restated for it.
//
// Burn is a fire colour and cook is a heat-of-the-metal colour, deliberately different hues
// rather than one colour used twice: the two bars sit 20 px apart on the same panel and mean
// opposite things (one is a resource draining, the other is progress filling), and at 12-14 px
// tall the only cue that separates them at a glance is hue.
#define COL_BURN        SPRITE_RGBA(255, 150, 52, 255)
#define COL_COOK        SPRITE_RGBA(120, 210, 140, 255)
// The border a furnace slot gets when the stack the player is currently carrying WOULD be
// accepted there. Green rather than COL_PICKED's blue, because blue already means "this is the
// slot you lifted from" everywhere else on this UI and reusing it here would put two meanings
// on one colour on a screen showing both at once.
#define COL_FURN_OK     SPRITE_RGBA(120, 220, 120, 255)

#define COL_PIP_TRACK   SPRITE_RGBA(58, 48, 74, 255)
#define COL_HEALTH      SPRITE_RGBA(226, 64, 64, 255)
#define COL_HEALTH_HALF SPRITE_RGBA(236, 130, 112, 255)
#define COL_HUNGER      SPRITE_RGBA(226, 158, 64, 255)
#define COL_HUNGER_HALF SPRITE_RGBA(240, 202, 132, 255)

// ── Small shared helpers ──────────────────────────────────────────────────────────────
//
// URect, ptInRect, hotbarSlotRect, gridSlotRect, hudToggleRect, hitInventorySlot and the
// v1.9.1 bar geometry (barStripRect, barTabRect, barCellRect and the rest) all live in
// scene/ui_layout.h/.c — see that header's file comment for why (host-testability) and its own
// comments for what each one does. This file now only calls into them.

// ── Mutating gesture handlers ─────────────────────────────────────────────────────────
//
// v1.3.0: every mutation below goes through net/inv_bridge.h rather than calling
// world/inventory.h and world/crafting.h directly. Each invBridge* call performs the exact
// same local operation it is named after and returns the same value — the semantics
// described in each comment below are unchanged — and additionally reports what happened to
// the server, so a joined session's inventory survives a rejoin instead of living only on
// this console. In single player nothing is sent (see inv_bridge.h on the capability probe),
// so this file behaves identically offline.

// ── v1.9.0 SPLIT: the detached lift ────────────────────────────────────────────────────
//
// A Y split (handleSplit, further down) moves the larger half of a stack OUT of the bag into
// ui->lift; picked_slot still names the origin slot (which keeps the smaller half and stays
// highlighted), and ui->lift_from remembers it for the wire report. While that half is in
// the air, the second tap places the HALF, not the slot: onto the origin it goes back, onto
// an empty or same-item slot it merges (any overflow stays lifted), onto a different item it
// is refused and stays lifted — no swap, because a swap of half a stack has no meaning.
// net/inv_bridge.h's lift functions do the arithmetic and the wire report.

static bool liftDetached(const UiState* ui)
{
	return ui->lift.item != ITEM_NONE;
}

// Puts a detached half back into the bag. Called by handleSlotTap on the cancel tap, and by
// reconcileLift whenever picked_slot has stopped naming lift_from — which is how every
// existing "drop the lift" site in this file (uiOpenFurnace, uiOpenChest, the toggle, the
// close bars, the NULL-guard corrections) returns the half without knowing it exists.
static void returnLift(UiState* ui, Inventory* inv)
{
	if (!liftDetached(ui)) return;
	(void)invBridgeReturnLift(inv, ui->lift_from, &ui->lift);

	// v1.9.1. Cleared only if the bag actually took the whole half back. invBridgeReturnLift
	// can refuse a remainder (its header: "units the bag would not take back are still in
	// lift"), and clearing lift_from unconditionally stranded that remainder: with lift_from
	// at -1, reconcileLift's `picked_slot != lift_from` goes false as soon as picked_slot
	// resets to -1 too, so nothing ever retried, while liftDetached() stayed true and
	// handleSplit refused every later split for the rest of the session. Keeping the origin
	// index means reconcileLift picks it up again on the next frame that names a different
	// slot, so it self-heals the moment the bag has room.
	if (!liftDetached(ui)) ui->lift_from = -1;
}

static void reconcileLift(UiState* ui, Inventory* inv)
{
	if (liftDetached(ui) && ui->picked_slot != ui->lift_from) returnLift(ui, inv);
}

// See the file comment for the two-tap pick-up/drop gesture this implements.
static void handleSlotTap(UiState* ui, Inventory* inv, int slot)
{
	if (liftDetached(ui)) {
		if (slot == ui->picked_slot) {
			returnLift(ui, inv);          // the cancel: back where it came from
			ui->picked_slot = -1;
			return;
		}
		(void)invBridgePlaceLift(inv, ui->lift_from, slot, &ui->lift);
		if (!liftDetached(ui)) {          // all of it landed; a remainder stays lifted
			ui->lift_from   = -1;
			ui->picked_slot = -1;
		}
		return;
	}

	if (ui->picked_slot < 0) {
		// Nothing lifted: pick this slot up, but only if there is something in it — an
		// empty slot has nothing to move, so tapping one here is a no-op rather than
		// entering a "carrying nothing" state that a second tap would have to special-case.
		if (inv->slots[slot].item != ITEM_NONE) ui->picked_slot = slot;
		return;
	}

	if (ui->picked_slot == slot) {
		// Tapping the same slot again is "put it back where it was" — the one gesture a
		// two-tap system needs that a continuous drag gets for free by just not moving.
		ui->picked_slot = -1;
		return;
	}

	const InvSlot* dst = &inv->slots[slot];
	const InvSlot* src = &inv->slots[ui->picked_slot];

	if (dst->item == ITEM_NONE || dst->item == src->item) {
		// Empty destination or the same item: inventoryMoveUnits relocates or merges the
		// whole lifted stack, leaving any overflow behind in the source slot exactly the
		// way dropping onto a nearly-full stack of the same item should (see
		// inventory.h's own comment on inventoryMoveUnits).
		invBridgeMoveUnits(inv, ui->picked_slot, slot, src->count);
	} else {
		// A different item already sits there: swap the two stacks outright. This is
		// inventory.h's inventorySwapSlots, described in its own comment as "what a plain
		// drag-and-drop (no merge) looks like" — exactly this situation.
		invBridgeSwapSlots(inv, ui->picked_slot, slot);
	}
	ui->picked_slot = -1;
}

// Selecting the hotbar "in hand" slot (gameplay hand selection) and rearranging storage
// (handleSlotTap above) share the same 40x40 cells but never the same frame: a hotbar tap
// means "select this for placing" while the HUD screen is up (the world is what's being
// played), and it means "pick up / drop" once the inventory overlay is open (the player is
// visibly managing storage instead). The two screens gate which meaning applies, so there
// is no gesture — long-press, double-tap — needed to disambiguate them.
static void handleHotbarSelect(Inventory* inv, int hotbar_slot)
{
	invBridgeSelectHotbar(inv, (uint8_t)hotbar_slot);
}

static void handleCraftTap(Inventory* inv, int recipe_index)
{
	// craftCanMake is re-checked here rather than trusted from the draw pass a few
	// milliseconds earlier in the same frame: this project is single-threaded and nothing
	// else can touch `inv` between the two, so the check can never disagree with what was
	// drawn, but calling it again costs nothing and means this function has no invisible
	// precondition on draw order.
	if (craftCanMake(inv, recipe_index) && invBridgeCraft(inv, recipe_index))
		audioPlay(audioSfxId(SFX_CRAFT), AUDIO_PRIO_NORMAL, 1.0f);
}

// ── Furnace transfers (v1.8.15 FURNACE) ────────────────────────────────────────────────
//
// ── The gesture, and why it is not the two-tap lift used everywhere else ────────────────
//
// Inventory-to-inventory moves on this UI are two taps: lift a slot, then tap where it goes
// (see the file comment at the top). That model needs `ui->picked_slot`, which is an
// inventory.h slot INDEX — and the three furnace slots are not inventory slots. They live in
// a FurnaceState the caller owns, they are not part of `inv->slots[]`, and there is no index
// that names one. So a symmetric "lift the furnace's input slot, then tap a bag slot" gesture
// would need a second lifted-from-where field on UiState plus a rule for what happens when the
// player closes the panel mid-lift, holding items that belong to neither container.
//
// Instead the furnace side is one tap, resolved by what the player is already carrying:
//
//   carrying a stack, tap INPUT or FUEL  -> deposit as much of it as the slot will take
//   carrying nothing,  tap INPUT or FUEL -> withdraw the whole stack into the bag
//   tap OUTPUT, either way               -> withdraw into the bag; a result slot is never a
//                                            place to put things, so there is no ambiguity to
//                                            resolve and no reason to make the player drop
//                                            what they are carrying first
//
// Nothing is ever "in the air" between the two containers, so closing the panel at any moment
// cannot lose an item — which is the failure mode the symmetric version would have had to
// grow a rule for.
//
// ── What is reported to the server, and what is not ────────────────────────────────────
//
// This is the one place in this file where a mutation does NOT go through net/inv_bridge.h,
// and the asymmetry is deliberate rather than an oversight, so it is worth being exact:
//
//   WITHDRAW uses invBridgeAdd(), like every other bag gain in this file. The bag really did
//   gain the items; reporting it is what stops the server's next BS_APP_INV_STATE snapshot
//   taking them straight back off the player.
//
//   DEPOSIT does the bag-side decrement DIRECTLY on inv->slots[], unreported. There is no
//   slot-precise wrapper in inv_bridge.h to use — invBridgeRemove() is item-based and scans
//   from slot 0 upward, so depositing the porkchops the player lifted out of slot 20 would
//   visibly drain slot 0's porkchops instead. That is wrong in SINGLE PLAYER, which is the
//   mode this game is actually played in, and it was rejected on exactly that ground.
//
// The honest summary of the multiplayer position: a furnace's contents live in
// world/blockstate.c's side table, and NOTHING about that table is on the wire — there is no
// BS_INV_OP for "into a furnace" and no packet carries a FurnaceState. So no reporting scheme
// available here is actually correct, and the choice between them is only about which way the
// error falls. Both halves above fall the same way: toward the player keeping their items
// (a deposit springs back on the next snapshot, a withdrawal sticks) rather than losing them,
// which is the right direction under an unreliable-UDP optimistic model — see inv_bridge.h's
// own "apply locally now" reasoning. The furnace is a single-player feature until blockstate
// is networked, and this comment is the record of what has to be revisited when it is.

// Resolves a FURN_HIT_* index to the two fields of `fs` holding that slot's stack, so deposit
// and withdraw below are written once against "a furnace slot" rather than three times against
// input, fuel and output. False for FURN_HIT_NONE and for anything outside the three, leaving
// both out-pointers untouched — the caller treats that as "nothing to do", the same answer a
// tap on bare panel background gets.
static bool furnSlotFields(FurnaceState* fs, int which, ItemId** out_item, uint8_t** out_count)
{
	switch (which) {
	case FURN_HIT_INPUT:
		*out_item = &fs->input_item;  *out_count = &fs->input_count;  return true;
	case FURN_HIT_FUEL:
		*out_item = &fs->fuel_item;   *out_count = &fs->fuel_count;   return true;
	case FURN_HIT_OUTPUT:
		*out_item = &fs->output_item; *out_count = &fs->output_count; return true;
	default:
		return false;
	}
}

// Whether `item` is allowed in furnace slot `which` at all, ignoring what is already in that
// slot. This is a GAME rule, not a safety one, and it is enforced rather than left permissive
// on purpose.
//
// A Java furnace lets any item sit in its input slot and simply never smelts it. That was the
// alternative and it was rejected: on a 320x240 panel with no tooltip and no error text, an
// item sitting in the input slot doing nothing forever is indistinguishable from a furnace
// that is broken, and the player's dirt is now somewhere they have to remember to go and get
// it back from. Refusing the deposit outright means the stack stays visibly in their bag,
// where they can see it, and the slot's border (drawn green when the carried stack WOULD be
// accepted — see drawFurnacePanelFont) says which slot it does belong in before they tap.
//
// world/furnace.h owns both predicates. Nothing here re-lists which meats smelt or which
// timber burns; adding a recipe or a fuel to that file makes it depositable here with no edit
// to this one.
static bool furnaceAccepts(int which, ItemId item)
{
	if (item == ITEM_NONE) return false;

	switch (which) {
	case FURN_HIT_INPUT:  return furnaceRecipeForInput(item) != NULL;
	case FURN_HIT_FUEL:   return furnaceIsFuel(item, NULL);
	// The output slot is where a smelt's RESULT appears. Nothing is ever put there by hand;
	// see the gesture block above for why tapping it is unambiguously a withdrawal.
	case FURN_HIT_OUTPUT: return false;
	default:              return false;
	}
}

// Moves as much of the lifted stack as fits into furnace slot `which`. A refusal — wrong slot
// for this item, slot already holding something else, slot already full — leaves BOTH the bag
// and the furnace exactly as they were AND leaves the stack lifted, so the player can carry it
// somewhere it does fit rather than having to pick it up again.
static void furnaceDeposit(UiState* ui, Inventory* inv, FurnaceState* fs, int which)
{
	InvSlot* src = &inv->slots[ui->picked_slot];
	if (!furnaceAccepts(which, src->item)) return;

	ItemId*  dst_item;
	uint8_t* dst_count;
	if (!furnSlotFields(fs, which, &dst_item, &dst_count)) return;

	// The merge/refuse/cap arithmetic — including the "a furnace slot holding a different item
	// is REFUSED, not swapped" rule — lives in scene/ui_layout.c so it can be host-tested; see
	// uiMoveStackInto's header comment for every rule it applies and why it is not in this
	// file. A swap would hand the player the burning fuel item out from under a lit furnace
	// mid-tick with no gesture to undo it, which is why the refusal is the rule.
	//
	// It also does the BAG-side decrement, slot-precisely and unreported — see the "what is
	// reported" block above for why this one transfer does not go through net/inv_bridge.h.
	if (uiMoveStackInto(src, dst_item, dst_count) == 0) return;

	ui->picked_slot = -1;
}

// Empties furnace slot `which` into the bag. Whatever the bag could not take stays in the
// furnace — the count is set to what was left over, never cleared optimistically — so a full
// inventory means the smelt output sits and waits rather than evaporating.
static void furnaceWithdraw(Inventory* inv, FurnaceState* fs, int which)
{
	ItemId*  item;
	uint8_t* count;
	if (!furnSlotFields(fs, which, &item, &count)) return;
	if (*item == ITEM_NONE || *count == 0) return;

	uint8_t leftover = 0;
	invBridgeAdd(inv, *item, *count, &leftover);

	// INV_ADD_REFUSED leaves leftover == count and the inventory provably unchanged (see
	// inventory.h), so this is the branch where nothing moved and nothing may be deducted.
	// Written as >= rather than == so a future inventoryAdd that reported leftover oddly could
	// only ever under-deduct, never hand the player free items.
	if (leftover >= *count) return;

	*count = leftover;
	if (*count == 0) *item = ITEM_NONE;
}

// ── Icon UVs ───────────────────────────────────────────────────────────────────────────

// The block atlas's arithmetic (world/atlas_uv.h's atlasRect, wrapped by gfx/atlas.h's
// atlasTile) already returns bounds with the top/bottom flip applied — see atlas_uv.h's own
// comment: r.vslot1 is the art's top edge, r.vslot0 its bottom, both already in the
// "v grows upward" space gfx/sprite.h's spriteQuad wants. Nothing here re-derives the flip
// the way gfx/font.c has to for its own top-down cell table.
//
// The two axes are NOT in the same units, which is the whole point of task 13b's rename.
// r.u0/r.u1 are atlas PIXEL columns and divide by ATLAS_W_PX (16). r.vslot0/r.vslot1 are
// SLOT-EDGE INDICES — 0..64, one per tile boundary, not pixel rows — so they must be
// multiplied up by TILE_PX before dividing by ATLAS_H_PX. Skipping that factor is not a
// visible error: it yields a valid UV inside the sheet, so every icon would quietly draw
// the top 1/16th of slot 0 (grass) instead of its own art. The field rename is what turns
// that silent mis-draw into a compile failure; keep the names, do not re-alias them.
//
// Every icon's u spans the full 0..1, which is fine under the GPU_REPEAT wrap gfx/atlas.c
// sets in U: a quad's texel centres interpolate strictly inside (0,1) and never land on the
// wrapping endpoint itself.
//
// FACE_TOP is used for every BLOCK's icon — grass shows its green top, wood shows its ring
// pattern, and every other cube in this game is the same texture on every face, so there is
// no "which face reads best as an icon" decision to make beyond picking one consistently.
//
// v1.8.16 IMP-ICONS: items that are not really cubes no longer go through FACE_TOP at all.
// A block face is a full opaque square by construction, so a porkchop's icon was a square of
// pink with no chop in it — steve's report, and the reason this line moved. gfx/item_icons.h
// answers with a dedicated item-art slot for the nine drops that have one (apple, the four
// raw cuts, the four cooked cuts) and ITEM_ICON_NONE for everything else, which is every
// placeable block in the game and keeps their icons byte-identical to what they drew before.
//
// The fallback is the FIRST thing to check if an icon ever looks wrong: an id with no case in
// itemIconTile() lands on `default:` and draws its top face, which is a correct-looking icon
// for a cube and a plain square for a drop — i.e. the failure mode here is exactly the old
// behaviour, not a magenta marker, so it will not announce itself.
static void iconUv(BlockId id, float* u0, float* v0, float* u1, float* v1)
{
	const int icon = itemIconTile(id);
	const AtlasRect r = atlasTile(icon != ITEM_ICON_NONE ? icon : (int)blockFaceTex(id, FACE_TOP));
	*u0 = (float)r.u0 / (float)ATLAS_W_PX;
	*v0 = (float)(r.vslot1 * TILE_PX) / (float)ATLAS_H_PX;  // quad's top edge = art's top row
	*u1 = (float)r.u1 / (float)ATLAS_W_PX;
	*v1 = (float)(r.vslot0 * TILE_PX) / (float)ATLAS_H_PX;  // quad's bottom edge = art's bottom
}

// ── Slots ──────────────────────────────────────────────────────────────────────────────

typedef enum { PASS_FONT, PASS_ATLAS } DrawPass;

// Draws one slot's contents for one pass. Called twice per visible slot — once per pass,
// see uiUpdateDraw — rather than once with both textures interleaved, because interleaving
// would mean a texture switch (and a batch flush, see gfx/sprite.h) per slot instead of one
// for the whole screen.
static void drawSlotIcon(DrawPass pass, URect r, const InvSlot* s, bool selected, bool picked,
                          C3D_Tex* icons)
{
	if (pass == PASS_FONT) {
		spriteRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, COL_SLOT_BG);

		if (picked) {
			// A full border, not just a top stripe — this is the one indicator this
			// screen's whole interaction model hinges on (which slot is mid-move), so it
			// gets a heavier treatment than the hotbar's own "selected" stripe below.
			spriteRect((float)r.x, (float)r.y, (float)r.w, 2, COL_PICKED);
			spriteRect((float)r.x, (float)(r.y + r.h - 2), (float)r.w, 2, COL_PICKED);
			spriteRect((float)r.x, (float)r.y, 2, (float)r.h, COL_PICKED);
			spriteRect((float)(r.x + r.w - 2), (float)r.y, 2, (float)r.h, COL_PICKED);
		} else if (selected) {
			spriteRect((float)r.x, (float)r.y, (float)r.w, 2, COL_ACCENT);
		}

		if (s->item != ITEM_NONE) {
			if (!icons) {
				// No block-atlas texture handed in (see ui.h's file comment) — fall back
				// to the block's own name instead of inventing art for it. Every name in
				// world/block.c's table is <=6 chars, which at FONT_ADVANCE(6)px fits the
				// (SLOT_PX - 2*ICON_INSET_X) = 34px icon column with room to spare.
				fontDraw((float)(r.x + ICON_INSET_X), (float)(r.y + ICON_INSET_TOP), 1,
				         COL_TEXT_DIM, blockInfo(s->item)->name);
			}
			if (s->count > 1) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%u", (unsigned)s->count);
				const int tw = fontTextWidth(buf, 1);
				fontDraw((float)(r.x + r.w - tw - 3),
				         (float)(r.y + r.h - FONT_GLYPH_H - 2), 1, COL_TEXT, buf);
			}
		}
		return;
	}

	// PASS_ATLAS
	if (!icons || s->item == ITEM_NONE) return;

	float u0, v0, u1, v1;
	iconUv(s->item, &u0, &v0, &u1, &v1);
	// ICON_QUAD_X / ICON_W, not ICON_INSET_X — see their definition for why the quad has to
	// be square now that an icon can have a shape.
	spriteQuad((float)(r.x + ICON_QUAD_X), (float)(r.y + ICON_INSET_TOP),
	           (float)ICON_W, (float)ICON_H,
	           u0, v0, u1, v1, SPRITE_WHITE);
}

// ── Vitals: health and hunger pips ─────────────────────────────────────────────────────
//
// v1.8.13 SURV-HUD. Ten flat rects a bar, no art asset and no atlas tile, drawn in the font
// pass with spriteRect — the same mechanism this file already uses for the picked-slot border
// and the crafting rows, so this adds nothing to the two-draw-call budget uiUpdateDraw's own
// comment sets out. There is deliberately no new texture: a pip is a rectangle, and a rectangle
// that has to come out of the block atlas would tie the vitals readout to a sheet that has no
// spare tiles and would put the whole strip behind the `block_icons != NULL` check, which is
// exactly the condition ui.h says the HUD must still draw usefully under.
//
// ── How a half pip reads ────────────────────────────────────────────────────────────────
//
// Health is 0..20 over ten pips, so a pip is worth two points and an odd value leaves exactly
// one pip half. That half is drawn as a HALF-WIDTH rect in its own third colour
// (COL_HEALTH_HALF / COL_HUNGER_HALF) rather than being rounded to the nearest whole pip.
//
// Rounding was the alternative and it was rejected on what it costs at exactly the moment the
// number matters: health 1 and health 2 are the difference between one more hit killing you and
// not, and rounding makes them draw identically. The cost of keeping the half state is one extra
// colour and a /2 on the width.
//
// Both cues are used together on purpose, because at 12x8 px neither is reliable alone. Width
// alone asks the player to judge 6 px against 12 px in their peripheral vision; colour alone
// asks them to tell two reds apart at 96 px². Half width AND a distinctly lighter tone is
// unambiguous even at a glance, and it stays legible if either cue is degraded — a bad
// screenshot, a scaled-down capture, or a player who is not looking straight at it.
//
// The half colour is LIGHTER than the full one rather than darker, which is the opposite of
// what "less health" suggests and is a deliberate choice: the empty track (COL_PIP_TRACK) is
// already the dark end of this strip, so a dark half pip would be competing with the state it
// most needs to be distinguishable from. Lighter puts the three states at three separate
// brightnesses — track darkest, full mid, half brightest — instead of two of them close together.
static void drawPipRow(int row, int points, uint32_t col_full, uint32_t col_half,
                        const char* label)
{
	const URect first = hudPipRect(row, 0);

	// The label sits in the gutter left of HUD_PIP_X0 (x 6..18 for a two-glyph string at
	// FONT_ADVANCE 6), vertically centred on an 8 px pip against a 7 px glyph.
	fontDraw(6.0f, (float)first.y + (HUD_PIP_H - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT_DIM, label);

	for (int i = 0; i < HUD_PIP_COUNT; i++) {
		const URect r = hudPipRect(row, i);

		// Every pip gets its track first, then the fill on top, rather than branching to draw
		// one rect of the right colour: the half state needs both (a half-width fill over a
		// full-width track is what makes the empty half visible at all), and drawing the track
		// unconditionally is what keeps an empty bar readable as ten empty pips.
		spriteRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, COL_PIP_TRACK);

		switch (hudPipFill(points, i)) {
		case PIP_FULL:
			spriteRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, col_full);
			break;
		case PIP_HALF:
			spriteRect((float)r.x, (float)r.y, (float)(r.w / 2), (float)r.h, col_half);
			break;
		case PIP_EMPTY:
			break;
		}
	}
}

// `stats` is non-NULL here — drawHudFont has already returned if it wasn't, which is what
// keeps ui.h's "NULL draws exactly what it drew before, with no pips" promise true.
static void drawHudVitals(const UiStats* stats)
{
	drawPipRow(HUD_PIP_ROW_HEALTH, (int)stats->health, COL_HEALTH, COL_HEALTH_HALF, "HP");
	drawPipRow(HUD_PIP_ROW_HUNGER, (int)stats->hunger, COL_HUNGER, COL_HUNGER_HALF, "FD");
}

// ── HUD screen (font pass only — no icons beyond the hotbar's, drawn separately) ────────

static void drawHudFont(const UiStats* stats)
{
	// v1.9.1 BAR. "INVENTORY   B", not "OPEN INVENTORY": the strip is the same band as the
	// bar's own strip (HUD_TOGGLE_Y/H == BAR_STRIP_Y/H), so it reads as the bar's first tab
	// with the button that opens it named on the end — which is the one thing a player cannot
	// discover by looking, B being unbindable and therefore unlabelled anywhere else.
	const URect  tr    = hudToggleRect();
	const char*  label = "INVENTORY   B";
	spriteRect((float)tr.x, (float)tr.y, (float)tr.w, (float)tr.h, COL_PANEL);
	const int tw = fontTextWidth(label, 1);
	fontDraw((float)tr.x + (tr.w - (float)tw) * 0.5f,
	         (float)tr.y + (tr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	if (!stats) return;

	float y = HUD_STATUS_Y0;
	fontDrawf(6, y, 1, COL_TEXT_DIM, "cols %d  chunks %d", stats->columns, stats->chunks);
	y += 12;
	fontDrawf(6, y, 1, COL_TEXT_DIM, "meshes %d  tris %lu  cull %d",
	          stats->meshes, (unsigned long)stats->tris, stats->culled);
	y += 12;
	fontDrawf(6, y, 1, COL_TEXT_DIM, "blocks %lu B  peak %lu B",
	          (unsigned long)stats->bytes, (unsigned long)stats->bytes_peak);
	y += 12;
	if (stats->status) fontDrawf(6, y, 1, COL_TEXT_DIM, "%s", stats->status);
	y += 12;
	if (stats->net) fontDrawf(6, y, 1, COL_TEXT_DIM, "%s", stats->net);
	y += 12;

	// v1.8.8. The biome the player is standing in.
	//
	// Appended after the existing rows rather than inserted among them so the readout above
	// is byte-for-byte the one that was already there — this is an addition to the panel, not
	// a re-layout of it. Same x, same colour, same 12-px step and the same terse
	// label-then-value shape as "cols 81  chunks 435".
	//
	// The position comes from chunkRenderCamera and not from UiStats, because UiStats is
	// built in source/main.c and that file belongs to another lane right now. It is the
	// camera's column, so it is where the player is LOOKING FROM, which for a first-person
	// camera is where they are standing.
	//
	// **This row reads "biome ?  unwired" in the build as it stands.** debugBiomeRow has no
	// WorldGen until main.c hands it one; see the routed one-liner spelled out at the top of
	// source/debug/biomeinfo.h. Everything else about the row — wording, colour, width,
	// position, and the lookup itself — is finished and covered by
	// source/debug/biomeinfo_test.c.
	{
		float cx = 0.0f, cy = 0.0f, cz = 0.0f;
		chunkRenderCamera(&cx, &cy, &cz);
		char biome[48];
		// Floored, not truncated: (int) rounds toward zero, so the 128-block biome cell
		// straddling the origin would be reported one column off for every negative
		// coordinate — the classic half-of-the-world-is-wrong bug that only shows up west or
		// north of spawn.
		const int32_t bx = (int32_t)((cx < 0.0f) ? (cx - 1.0f) : cx);
		const int32_t bz = (int32_t)((cz < 0.0f) ? (cz - 1.0f) : cz);
		fontDrawf(6, y, 1, COL_TEXT_DIM, "%s",
		          debugBiomeRow(bx, bz, biome, sizeof(biome)));
	}
	y += 12;

	// v1.8.11 METRICS-VISIBLE. The CPU-vs-wait split, which is the one question docs/ROADMAP.md
	// leaves open ("GPU-blocked for roughly 15.7 of every 16.71 ms") and which no shipped build
	// could previously answer: the console overlay that used to print it is compiled out under
	// BS_BOTTOM_UI, and BS_BOTTOM_UI is the default. See debug/metrics.h for the arithmetic and
	// for why this is a different — and more complete — figure than that overlay's "cpu" row.
	//
	// Drawn HERE, on the live HUD, and not only on the debug menu's existing "Frame" INFO row,
	// because that row is a PAUSED reading and cannot answer the question. The debug menu is
	// modal over the pause menu, and main.c gates the simulation on its `paused` flag — grep
	// `if (!paused)` there — so with it open there is no camera update, no player update, no
	// column install and no mesh drain. Its CPU figure is the cost of a frame doing almost none
	// of the work a real frame does. The world behind it still draws, so the GPU side stays
	// representative, and that is exactly the combination that makes a paused reading look
	// trustworthy.
	//
	// NULL when the toggle is off, so the row does not exist in a normal session. Same shape as
	// `status` and `net` directly above.
	if (stats->timing) {
		fontDrawf(6, y, 1, COL_TEXT_DIM, "%s", stats->timing);
		y += 12;
	}

	y += 2;
	fontDraw(6, y, 1, COL_TEXT_DIM, "tap a hotbar slot to select it");

	// v1.8.13 SURV-HUD. Appended after every existing row rather than inserted among them, and
	// positioned from its own constants (HUD_PIPS_Y0) rather than from the running `y` above,
	// so the status block is byte-for-byte the one that was already here and so a row added or
	// removed up there cannot silently shove the vitals off the bottom of the screen. The gap
	// between the two is checked, not assumed — see ui_layout_test.c's
	// testPipsClearTheStatusRows against HUD_STATUS_BOTTOM.
	drawHudVitals(stats);
}

// ── The CRAFT tab's recipe list (v1.9.1 BAR) ───────────────────────────────────────────
//
// What was here before: drawCraftPanelFont, a fixed 120-px band with its own "TAP TO CLOSE"
// bar and RECIPE_COUNT rows of CRAFT_ROW_H each. At RECIPE_COUNT 7 that was 13-px rows for a
// 7-px font — below the readable floor scene/ui_layout.h's own CRAFT_ROW_H comment set, and
// not a touch target at all. Those constants and their two rect constructors are retired; the
// rows are now a fixed 24 px in a window that scrolls (barListRowRect), so the row height
// stops depending on how many recipes exist, and the close bar is gone because the bar's own
// close tab is the one door off every tab.
//
// The row now says what the recipe DOES rather than only naming it: the output item's icon and
// "NAME xN" on the left, and on the right what it costs and whether the player has it — "xK
// (have)" ending at x 292 with the input item's icon at 296. The two icons are atlas art and
// so are drawn by drawBarCraftIcons below, in the frame's single atlas pass; everything here
// is font-pass work, which is what keeps the whole screen at two draw calls (see uiUpdateDraw).
#define CRAFT_ICON_PX  16
#define CRAFT_ICON_Y   3     // a 16-px icon centred in a 22-px row
#define CRAFT_OUT_X    8
#define CRAFT_LABEL_X  30
#define CRAFT_COST_END 292   // the cost text's RIGHT edge, clear of the input icon at 296
#define CRAFT_IN_X     296

// The cost readout for recipe `i`: how many of the input it takes, and how many the bag holds.
// Built into the caller's buffer rather than returned, the same shape debugBiomeRow uses, so
// the draw pass and the width measurement read one string and cannot disagree about it.
static void craftCostText(const Inventory* inv, int i, char* buf, size_t cap)
{
	const CraftRecipe* r = &CRAFT_RECIPES[i];
	snprintf(buf, cap, "x%u (%lu)", (unsigned)r->input_count,
	         (unsigned long)inventoryCount(inv, r->input_item));
}

static void drawBarCraftList(const Inventory* inv, int scroll, int focused_row)
{
	for (int v = 0; v < BAR_LIST_VISIBLE; v++) {
		const int i = scroll + v;
		if (i >= RECIPE_COUNT) break;

		const URect rr = barListRowRect(v);
		const CraftRecipe* rec = &CRAFT_RECIPES[i];
		const bool makeable = craftCanMake(inv, i);
		const bool focused  = (i == focused_row);

		// The fill carries the MAKEABLE cue and the focus cue carries focus, rather than the
		// focused row being repainted COL_PANEL_HI outright: a green row that turns grey the
		// moment the cursor lands on it would have hidden the one thing the row exists to say.
		// So a focused row that is not makeable lifts to COL_PANEL_HI (which is what the
		// blueprint's "focused adds COL_PANEL_HI" buys on the rows where it costs nothing), and
		// every focused row gets the 3-px accent left edge, which reads at a glance on either.
		spriteRect((float)rr.x, (float)rr.y, (float)rr.w, (float)rr.h,
		           makeable ? COL_CRAFT_OK : (focused ? COL_PANEL_HI : COL_PANEL));
		if (focused)
			spriteRect((float)rr.x, (float)rr.y, 3, (float)rr.h, COL_ACCENT);

		const float ty = (float)rr.y + (rr.h - FONT_GLYPH_H) * 0.5f;
		const uint32_t col = makeable ? COL_TEXT : COL_TEXT_DIM;

		fontDrawf((float)(rr.x + CRAFT_LABEL_X), ty, 1, col, "%s x%u",
		          rec->name, (unsigned)rec->output_count);

		char cost[24];
		craftCostText(inv, i, cost, sizeof(cost));
		fontDraw((float)(CRAFT_COST_END - fontTextWidth(cost, 1)), ty, 1, COL_TEXT_DIM, cost);
	}
}

// The two icons a recipe row carries, in the frame's atlas pass. Split from the row above for
// the same reason drawSlotIcon is called once per pass and not once per slot: interleaving the
// font and atlas textures would cost a batch flush per row (see gfx/sprite.h) instead of one
// for the whole screen.
static void drawBarCraftIcons(int scroll)
{
	for (int v = 0; v < BAR_LIST_VISIBLE; v++) {
		const int i = scroll + v;
		if (i >= RECIPE_COUNT) break;

		const URect rr = barListRowRect(v);
		const CraftRecipe* rec = &CRAFT_RECIPES[i];

		float u0, v0, u1, v1;
		iconUv(rec->output_item, &u0, &v0, &u1, &v1);
		spriteQuad((float)(rr.x + CRAFT_OUT_X), (float)(rr.y + CRAFT_ICON_Y),
		           (float)CRAFT_ICON_PX, (float)CRAFT_ICON_PX, u0, v0, u1, v1, SPRITE_WHITE);

		iconUv(rec->input_item, &u0, &v0, &u1, &v1);
		spriteQuad((float)CRAFT_IN_X, (float)(rr.y + CRAFT_ICON_Y),
		           (float)CRAFT_ICON_PX, (float)CRAFT_ICON_PX, u0, v0, u1, v1, SPRITE_WHITE);
	}
}

// ── Furnace panel (v1.8.15 FURNACE) ────────────────────────────────────────────────────

// A furnace slot is an (item, count) pair exactly like an inventory slot, so it is drawn by
// the same drawSlotIcon above rather than by a second copy of the background/border/icon/badge
// code. Built as a VALUE here and not by casting a pointer into FurnaceState: the matching
// fields are not adjacent in that struct and never will be (see world/furnace.h's packed
// layout), so there is no cast that makes one look like the other, and there should not be.
//
// An ITEM_NONE slot is normalised to count 0, so a stale count left behind by a corrupt
// blockstate payload cannot draw a count badge on a visibly empty cell.
//
// v1.9.0 CHEST reuses this unchanged for a chest slot's (item, count) pair — its name still
// says "furn" only because it was written first, not because the shape is furnace-specific.
// Renaming it was considered and rejected as a diff this task does not need to make: the
// function has no furnace-specific behaviour to begin with (it never touches a FurnaceState
// field), so the two callers sharing it costs nothing and a rename here would only be
// cosmetic churn on a file that already has enough surface area to review.
static InvSlot furnSlotView(ItemId item, uint8_t count)
{
	InvSlot s;
	s.item  = item;
	s.count = (item == ITEM_NONE) ? 0 : count;
	return s;
}

// Track first, fill on top — the same shape drawPipRow uses, and for the same reason: an empty
// gauge has to still read as a gauge at zero rather than as a gauge that stopped being drawn.
static void drawFurnaceBar(URect r, int fill_px, uint32_t col)
{
	spriteRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, COL_PIP_TRACK);
	if (fill_px > 0)
		spriteRect((float)r.x, (float)r.y, (float)fill_px, (float)r.h, col);
}

// ── What the two gauges actually measure ───────────────────────────────────────────────
//
// BURN is an ABSOLUTE scale: fuel_ticks_left out of FURNACE_FUEL_TICKS_LOG (1200 ticks, 60 s),
// NOT out of the burn time of whatever fuel item is sitting in the slot.
//
// That is forced, not preferred. world/furnace.c consumes the fuel unit at IGNITION and clears
// fuel_item to ITEM_NONE when that was the last one (see its step 1), so from the moment a
// furnace lights on its final plank there is nothing left in the state to say what lit it.
// FurnaceState has no field for the ignition duration and this lane does not own that file, so
// a "fraction of the current fuel item" bar would have no denominator for exactly the case it
// most needs one. Scaling everything against the longest fuel in the game instead is honest
// about what it shows: a plank-fed furnace draws a quarter-length bar draining, and that IS
// the true reading — a plank really does buy a quarter of what a log buys.
//
// COOK is a RELATIVE scale: cook_ticks out of the current input's recipe cook_ticks. With no
// valid input there is no recipe and no denominator, so the bar reads empty. Worth being
// explicit because it is visibly lossy in one real case: pulling the input out of a
// half-smelted furnace PAUSES cook_ticks rather than discarding it (furnace.c's step 3), but
// the bar has nothing to scale the paused value against and so shows nothing. The progress is
// not lost, and it reappears the instant a valid input goes back in. Reading it off a
// second denominator — the maximum cook_ticks across FURNACE_RECIPES — was rejected: every
// recipe is 200 ticks today, so it would look right and would silently start lying the day a
// recipe with a different duration is added, which is the shape of bug this codebase has
// already been bitten by.
static void drawFurnacePanelFont(const FurnaceState* fs, const Inventory* inv, int picked_slot,
                                  C3D_Tex* icons)
{
	// v1.9.1 BAR. The "FURNACE - TAP TO CLOSE" bar that used to open this function is GONE:
	// furnCloseRect() and the bar's own strip are the same band (FURN_CLOSE_Y/H are aliases of
	// BAR_STRIP_Y/H), the strip already names the tab and already carries the close tab, and
	// two panels stacked in one 26-px band would draw one over the other. Everything below is
	// byte-identical to what this panel shipped — blueprint D5: "only the close bar becomes
	// the strip".

	// What the player is carrying right now, so each slot can advertise whether it would take
	// it. ITEM_NONE when nothing is lifted, which furnaceAccepts() already answers false for,
	// so no slot lights up when there is nothing to put anywhere.
	const ItemId carried = (picked_slot >= 0) ? inv->slots[picked_slot].item : ITEM_NONE;

	for (int which = FURN_HIT_INPUT; which <= FURN_HIT_OUTPUT; which++) {
		const URect r = furnSlotRect(which);

		ItemId  item  = ITEM_NONE;
		uint8_t count = 0;
		switch (which) {
		case FURN_HIT_INPUT:  item = fs->input_item;  count = fs->input_count;  break;
		case FURN_HIT_FUEL:   item = fs->fuel_item;   count = fs->fuel_count;   break;
		default:              item = fs->output_item; count = fs->output_count; break;
		}

		const InvSlot view = furnSlotView(item, count);
		drawSlotIcon(PASS_FONT, r, &view, false, false, icons);

		// The accept cue, drawn OVER drawSlotIcon's own background so it reads as a border on
		// the finished cell. A full border rather than a top stripe, matching the picked-slot
		// treatment: this is the other half of the same "where can this stack go" question,
		// and answering half of it in a heavier weight than the other half would be worse than
		// answering neither.
		if (furnaceAccepts(which, carried)) {
			spriteRect((float)r.x, (float)r.y, (float)r.w, 2, COL_FURN_OK);
			spriteRect((float)r.x, (float)(r.y + r.h - 2), (float)r.w, 2, COL_FURN_OK);
			spriteRect((float)r.x, (float)r.y, 2, (float)r.h, COL_FURN_OK);
			spriteRect((float)(r.x + r.w - 2), (float)r.y, 2, (float)r.h, COL_FURN_OK);
		}
	}

	// Burn: absolute, against the longest fuel in the game — see the block comment above.
	const URect br = furnBurnRect();
	drawFurnaceBar(br, furnBarFill(br.w, (int)fs->fuel_ticks_left, FURNACE_FUEL_TICKS_LOG),
	               COL_BURN);

	// Cook: relative to the current input's own recipe, empty when there is no recipe.
	// furnBarFill() takes the den == 0 case itself, so there is no branch here and no way for
	// an idle furnace to reach a division.
	const FurnaceRecipe* recipe = furnaceRecipeForInput(fs->input_item);
	const URect ar = furnArrowRect();
	drawFurnaceBar(ar, furnBarFill(ar.w, (int)fs->cook_ticks,
	                                recipe ? (int)recipe->cook_ticks : 0), COL_COOK);

	fontDraw(6, (float)FURN_HINT_Y, 1, COL_TEXT_DIM,
	         "IN raw meat   FUEL logs or planks");
	fontDraw(6, (float)(FURN_HINT_Y + FURN_HINT_STEP), 1, COL_TEXT_DIM,
	         "tap a bag slot, then a furnace slot");
}

// ── Chest transfers (v1.9.0 CHEST) ─────────────────────────────────────────────────────
//
// The chest uses the SAME two-tap lift/place gesture as the inventory overlay (see the file
// comment at the top), not the furnace's carry-and-resolve one. The furnace needed its own
// gesture because its three slots each accept different things and a lift/place across them
// would have needed a "would this slot take what I'm carrying" answer per slot; a chest has no
// such distinction — every chest slot accepts every item (world/chest.h's own file comment) —
// so the overlay's gesture carries over unchanged: lift a stack on either side, tap where it
// goes on either side. The player chooses the destination slot, which is also what the wire
// contract needs (a chest slot index for a deposit, an inventory slot index for a withdraw —
// see ui.h's UiChestTransferFn), and which a "tap once, the code picks a slot" mapping could
// not have supplied.
//
// A lifted CHEST slot cannot live in ui->picked_slot (an inventory.h slot INDEX, and the
// furnace block above says why that matters), so it has its own field, ui->picked_chest. At
// most one of the two is ever >= 0: a lift only starts when nothing is lifted on either side,
// and every place clears whichever one it consumed. Closing the panel or opening another panel
// clears both (dropLifts()). The NULL-chest correction in uiUpdateDraw clears picked_chest
// ALONE — see that site for why picked_slot is deliberately left out of it — so nothing chest-
// side is ever "in the air" once the chest is gone, while a bag lift in progress survives
// exactly as it would across any other tab switch. Either way, the lifted stack is never
// removed from its source until the place tap resolves it.
//
// ── The place rules, and why this does not hand-write a second copy of them ───────────
//
// placeStack() below is the overlay's handleSlotTap resolution restated against a destination
// that is not an inventory slot: empty or same item -> merge via scene/ui_layout.c's
// uiMoveStackInto() (tops the destination up to INV_STACK_MAX and leaves the remainder in the
// source — proved by ui_layout_test.c's testMoveStackIntoConservesUnits); different item ->
// swap the two (item, count) pairs outright, which is inventory.h's inventorySwapSlots rule
// for the bag. It cannot go through inventoryMoveUnits()/inventorySwapSlots() themselves
// because those address BOTH slots by inventory index and a chest slot has none — the same
// structural fact that sends furnaceDeposit through uiMoveStackInto(). A chest -> bag place
// builds a one-slot InvSlot VIEW of the chest slot, resolves it exactly as if it were the
// source, and writes the view back; a chest -> chest place does the same against another
// chest slot.
//
// Nothing here can lose a fraction of a stack: uiMoveStackInto() only ever decrements the
// source by exactly what it credited, and a swap moves both pairs whole. A place onto a full
// same-item stack moves nothing and still clears the lift — the identical outcome the overlay
// gives a bag -> bag drop onto a full stack, and for the same reason: the player put it down
// where it does not fit, and "still holding it" would need a cue this panel has no room for.
//
// ── What is reported to the server ─────────────────────────────────────────────────────
//
// NOTHING, on the local path, in either direction. This differs from furnaceWithdraw above,
// which goes through invBridgeAdd() and so reports a BS_INV_OP_PICKUP. It cannot be reused
// here: inventoryAdd() picks the bag slot by its own scan, and this gesture's whole point is
// that the player picked the slot. In single player nothing would be sent anyway. In a
// session the local path does not run at all: main.c registers the transfer callback below
// off networldSessionActive(), and the callback path applies nothing — the server answers
// with BS_APP_CHEST_STATE and BS_APP_INV_STATE snapshots that rewrite both sides. A session
// against a server WITHOUT BS_CAP_CHESTS therefore has read-only chests (the send refuses,
// the lift is kept, nothing moves); docs/design-1.9.0-chest-multiplayer.md's open question 2
// recommends a visible refusal for that case and that is not built here.

// Resolves a lifted stack `src` onto the destination (`dst_item`, `dst_count`). See the block
// comment above for the three rules and where each comes from.
static void placeStack(InvSlot* src, ItemId* dst_item, uint8_t* dst_count)
{
	if (*dst_item == ITEM_NONE || *dst_item == src->item) {
		(void)uiMoveStackInto(src, dst_item, dst_count);
		return;
	}

	const ItemId  held_item  = *dst_item;
	const uint8_t held_count = *dst_count;
	*dst_item  = src->item;
	*dst_count = src->count;
	src->item  = held_item;
	src->count = held_count;
}

static void dropLifts(UiState* ui)
{
	ui->picked_slot  = -1;
	ui->picked_chest = -1;
}

// v1.9.0 CHEST-NET. The place tap of a bag<->chest transfer with a callback registered: hand
// it over and apply nothing, whatever it answers. True means "sent" (the lift is cleared,
// the server's snapshots will rewrite both sides); false means "refused" (nothing changes and
// the lift is kept, so the player is still holding it). See ui.h's UiChestTransferFn.
static void chestTransferViaFn(UiState* ui, uint8_t op, uint8_t a, uint8_t b, uint8_t count)
{
	if (ui->chest_fn(ui->chest_ud, op, ui->chest_x, ui->chest_y, ui->chest_z, a, b, count))
		dropLifts(ui);
}

// A tap on chest slot `c` (0..CHEST_SLOTS-1): lift, cancel, or place onto it.
static void handleChestSlotTap(UiState* ui, Inventory* inv, ChestState* cs, int c)
{
	if (ui->picked_slot < 0 && ui->picked_chest < 0) {
		// Nothing lifted: lift this slot, but only if there is something in it — same rule
		// as handleSlotTap. Read off the item AND the count: a chest slot is unpacked from a
		// save payload byte by byte, so { item, 0 } is a corrupt state this must not carry.
		if (cs->item[c] != ITEM_NONE && cs->count[c] != 0) ui->picked_chest = c;
		return;
	}

	if (ui->picked_chest == c) {
		// Same slot again: put it back where it was — the overlay's own cancel gesture.
		ui->picked_chest = -1;
		return;
	}

	if (ui->picked_slot >= 0) {
		// bag -> chest: a DEPOSIT of the lifted bag slot's item onto chest slot c.
		InvSlot* src = &inv->slots[ui->picked_slot];
		if (ui->chest_fn) {
			// The units on the wire come from the DETACHED lift when there is one (ui.h's
			// `lift`), and from the bag slot otherwise. Today those two always agree, because
			// the chest screen can never hold a detached lift: split_enabled is `overlay_open`
			// alone (uiUpdateDraw, below) and reconcileLift returns the lift to lift_from on
			// any screen change. So this costs nothing right now and is written the safe way
			// round on purpose — the day split_enabled grows `|| chest_open`, the bag slot
			// holds the FLOOR half left behind while the lift holds the CEIL half the player
			// is actually carrying (net/inv_bridge.c's invBridgeSplitStack), and sending the
			// left-behind number would deposit the wrong amount on every odd stack. ui.h's
			// UiChestTransferFn comment states this as a rule; this is that rule obeyed.
			const InvSlot* held = liftDetached(ui) ? &ui->lift : src;
			chestTransferViaFn(ui, UI_CHEST_OP_DEPOSIT, (uint8_t)held->item, (uint8_t)c,
			                   held->count);
			return;
		}
		placeStack(src, &cs->item[c], &cs->count[c]);
		dropLifts(ui);
		return;
	}

	// chest -> chest. No opcode carries this, so with a callback registered it is refused
	// outright — see the block comment above — and the lift is kept.
	if (ui->chest_fn) return;

	InvSlot view;
	view.item  = cs->item[ui->picked_chest];
	view.count = cs->count[ui->picked_chest];
	placeStack(&view, &cs->item[c], &cs->count[c]);
	cs->item[ui->picked_chest]  = view.item;
	cs->count[ui->picked_chest] = view.count;
	dropLifts(ui);
}

// A tap on inventory slot `slot` while the chest screen is up. With a chest slot lifted this
// is a WITHDRAW onto that bag slot; otherwise it is the overlay's own bag -> bag lift/place.
static void handleChestInvSlotTap(UiState* ui, Inventory* inv, ChestState* cs, int slot)
{
	// v1.9.0 SPLIT: with no chest slot lifted this tap is a bag -> bag lift/place, and that is
	// the gesture block's in uiUpdateDraw (fed before this chain, with lift_enabled on for
	// exactly this case) — handling it here too would lift and cancel on the same tap.
	if (ui->picked_chest < 0) return;

	if (ui->chest_fn) {
		chestTransferViaFn(ui, UI_CHEST_OP_WITHDRAW, (uint8_t)ui->picked_chest, (uint8_t)slot,
		                   cs->count[ui->picked_chest]);
		return;
	}

	InvSlot view;
	view.item  = cs->item[ui->picked_chest];
	view.count = cs->count[ui->picked_chest];
	placeStack(&view, &inv->slots[slot].item, &inv->slots[slot].count);
	cs->item[ui->picked_chest]  = view.item;
	cs->count[ui->picked_chest] = view.count;
	dropLifts(ui);
}

// ── v1.9.0 SPLIT: the two key gestures ─────────────────────────────────────────────────

// Y. Detaches the larger half of `slot` into ui->lift — see the detached-lift block above
// handleSlotTap. Only ever reached on the inventory overlay (uiUpdateDraw passes
// split_enabled for that screen alone; ui_gesture.h says why).
static void handleSplit(UiState* ui, Inventory* inv, int slot)
{
	if (liftDetached(ui)) return;                          // one half in the air at a time
	if (!invBridgeSplitStack(inv, slot, &ui->lift)) return;
	ui->lift_from   = slot;
	ui->picked_slot = slot;
}

// X. Sends `slot`'s whole stack to the other container without a second tap: the other strip
// of the bag, or — with the chest panel up — the open chest, by exactly the deposit path a
// lift-and-tap takes (placeStack locally; the transfer callback under a session — it gets
// the count, and picked_slot names the source for the frame of the call, as ui.h says a
// registrant may read it). Any lift is dropped first: a detached half goes back to its
// slot, so it is the whole stack that moves, never half of it.
//
// v1.9.1: the chest half of this used to pick ONE landing slot — a private chestQuickSlot()
// that returned the first same-item slot with ANY room, else the first empty one — and send
// the entire stack to it, while its own comment claimed it applied invBridgePlanQuickMove's
// strip rule. It did not: that rule SPILLS across every eligible slot. Two chest slots at 90
// of an item with 20 in hand moved 9 and left 11 in the bag with no refusal shown, even
// though the second slot had room for all of them. The rule now lives in
// invBridgePlanChestDeposit (net/inv_bridge.c), stated as a plan so a test can read the
// answer directly; tests/ui_chest_test.c drives THIS function through a real frame to prove
// the two agree.
static void handleQuickMove(UiState* ui, Inventory* inv, ChestState* cs, int slot, bool to_chest)
{
	returnLift(ui, inv);
	dropLifts(ui);

	if (!to_chest || !cs) {
		(void)invBridgeQuickMove(inv, slot);
		return;
	}

	InvBridgeChestDeposit plan[INV_BRIDGE_CHEST_PLAN_MAX];
	const int n = invBridgePlanChestDeposit(inv, slot, cs, plan, INV_BRIDGE_CHEST_PLAN_MAX);
	if (n == 0) return;                                    // chest full: refused, nothing moves

	InvSlot* src = &inv->slots[slot];
	for (int i = 0; i < n; i++) {
		const int c = plan[i].chest_slot;

		if (ui->chest_fn) {
			// picked_slot is re-set EVERY iteration, not once before the loop:
			// chestTransferViaFn calls dropLifts() on a successful send, which sets
			// picked_slot back to -1, and the registrant reads it to name the source bag
			// slot. Set once, only the first deposit of a spill would carry a source.
			ui->picked_slot = slot;
			chestTransferViaFn(ui, UI_CHEST_OP_DEPOSIT, (uint8_t)src->item, (uint8_t)c, plan[i].count);
			continue;
		}

		// Offline: placeStack moves min(src->count, room), which for a slot this plan chose
		// is exactly plan[i].count — the plan is computed from the same counts placeStack
		// then reads, and each planned slot is visited once. Reusing the tested primitive
		// rather than restating the arithmetic is the point; its swap branch is unreachable
		// here because the plan only ever names an empty or same-item slot.
		if (src->item == ITEM_NONE) break;
		placeStack(src, &cs->item[c], &cs->count[c]);
	}
	ui->picked_slot = -1;                                  // sent or refused, X holds nothing
}

// ── Chest panel (v1.9.0 CHEST) ─────────────────────────────────────────────────────────

// Same font-pass shape as drawFurnacePanelFont above, minus the two gauges and the
// per-slot accept border: a chest has no burn/cook state to read and no slot that would
// ever refuse an item, so neither has anything to draw here. A lifted chest slot gets the
// same COL_PICKED border a lifted bag slot gets, from the same drawSlotIcon — one cue for
// one gesture, whichever side it started on. The two hint lines are what tells the player
// which half of the screen is which rather than a second label drawn over the grid below.
static void drawChestPanelFont(const ChestState* cs, int picked_chest, C3D_Tex* icons)
{
	// v1.9.1 BAR. The "CHEST - TAP TO CLOSE" bar is gone for the reason the furnace panel's is
	// — see drawFurnacePanelFont above; chestCloseRect() and the strip are the same band.

	for (int i = 0; i < CHEST_SLOTS; i++) {
		const URect r = chestSlotRect(i);
		const InvSlot view = furnSlotView(cs->item[i], cs->count[i]);
		drawSlotIcon(PASS_FONT, r, &view, false, picked_chest == i, icons);
	}

	fontDraw(6, (float)CHEST_HINT_Y, 1, COL_TEXT_DIM,
	         "tap a slot to lift it, tap where it goes");
	fontDraw(6, (float)(CHEST_HINT_Y + CHEST_HINT_STEP), 1, COL_TEXT_DIM,
	         "CHEST above, BAG below");
}

// ── The bar (v1.9.1 BAR) ───────────────────────────────────────────────────────────────
//
// Three screens became three tabs of one. What each tab CONTAINS is unchanged — the bag grid,
// the recipe list, the chest's row of eight, the furnace's three slots and two gauges — and so
// is every gesture that acts on them: the mutating handlers above are called from exactly the
// places they were called from before, just switched on the focused category instead of on the
// screen. What is new is the strip, the d-pad/A/B cursor, and one rule that makes those two
// impossible to get out of step with the stylus:
//
//   A COMMIT IS A SYNTHETIC TAP. barNavInput answering BAR_EV_COMMIT does not run a second
//   copy of the tap logic; it produces a touch point at the focused cell's CENTRE and feeds it
//   into the same hit-test chain a stylus tap goes through (blueprint D8). There is therefore
//   no controller path to keep in sync with the touch path, because there is only one path.
//
// The centre and not the origin, deliberately: the three panels' hit tests were written
// independently and disagree about which side of a cell border belongs to which cell, and the
// origin is exactly the pixel where they disagree. The centre of a cell is the one point every
// one of them answers the same way.

// The hint line under the INVENTORY tab's detail band. Only that tab draws it: CHEST and
// FURNACE already carry their own hint lines at 118/130 and 136/148 and put the relocated bag
// grid across 160..240, and CRAFT's seventh list row ends on 240 — on any of the three, a line
// at 228 would land on top of content that is already there.
#define BAR_HINT_Y 228

// The live category list for one frame: the tab order barBuildKinds fixes, plus the same list
// described to scene/barnav.h so a cursor can walk it. A stack local on uiUpdateDraw's frame
// (~100 bytes at BAR_MAX_TABS 4), which is inside the blueprint's 3DS stack rule that anything
// over 1 KB has to be static.
typedef struct {
	BarKind    kinds[BAR_MAX_TABS];
	BarCatSpec specs[BAR_MAX_TABS];
	int        count;
} BarCats;

// One category's shape, read out of scene/ui_layout.h's tables rather than restated here. The
// uniform/per-row choice is DERIVED (do all this kind's rows have the same width?) instead of
// hardcoded per kind, so a kind whose row widths change in ui_layout.c cannot leave a stale
// table in this file disagreeing with it.
static BarCatSpec barSpecFor(BarKind k, int list_len)
{
	BarCatSpec s;
	memset(&s, 0, sizeof(s));
	s.rows                  = barKindRows(k, list_len);
	s.horizontal_is_content = barKindHorizontalIsContent(k);
	if (s.rows <= 0) return s;

	const int c0 = barKindCols(k, 0);
	bool uniform = true;
	for (int r = 1; r < s.rows; r++) {
		if (barKindCols(k, r) != c0) { uniform = false; break; }
	}

	// cols[] is only BARNAV_MAX_ROWS long, so a category with more rows than that MUST use the
	// uniform escape hatch. Today only CRAFT can exceed it and CRAFT is uniformly one column
	// wide, so the two conditions never disagree; written as an `||` rather than an assumption
	// because the day a recipe list grows past 8 rows is not the day to discover this.
	if (uniform || s.rows > BARNAV_MAX_ROWS) {
		s.uniform_cols = (c0 > 0) ? c0 : 1;
	} else {
		// The loop counts to the ARRAY bound and leaves on the row count, rather than the other
		// way round, because of the compiler rather than because of the logic. Reaching here
		// already means s.rows <= BARNAV_MAX_ROWS, so every shape of this loop writes the same
		// bytes; only this one builds.
		//
		// devkitARM's gcc inlines barSpecFor into uiUpdateDraw with list_len fixed at
		// RECIPE_COUNT, loses the correlation between `uniform` and `s.rows` across the `||`
		// above, and store-merges the whole loop into one write sized by s.rows — which it then
		// reports as -Werror=stringop-overflow= "writing 1 byte into a region of size 0 ... at
		// offset 8 into destination object 'cols' of size 8". Two narrower bounds were tried
		// against the real compiler first and BOTH still gave offset 8, because neither changes
		// the merged store's size: `for (r = 0; r < s.rows && r < BARNAV_MAX_ROWS; r++)`, and a
		// hoisted `const int n = min(s.rows, BARNAV_MAX_ROWS)`. A constant trip count does clear
		// it, because the merged store is then 8 bytes into an 8-byte object.
		//
		// The clamp is real either way: the day a kind gains a ninth non-uniform row this
		// truncates to eight instead of scribbling past cols[].
		for (int r = 0; r < BARNAV_MAX_ROWS; r++) {
			if (r >= s.rows) break;
			const int c = barKindCols(k, r);
			s.cols[r] = (uint8_t)((c > 0) ? c : 1);
		}
	}
	return s;
}

// Rebuilt EVERY frame from the two container pointers, never cached on the UiState. That is
// what replaces the two same-frame screen corrections the container screens used to carry: a
// container whose state stopped being handed in simply has no tab this frame, and the caller
// does not have to have remembered to close anything.
static void barBuildCats(BarCats* c, bool has_chest, bool has_furnace)
{
	c->count = barBuildKinds(has_chest, has_furnace, c->kinds);
	for (int i = 0; i < c->count; i++)
		c->specs[i] = barSpecFor(c->kinds[i], RECIPE_COUNT);
}

// Anything in the air, on either side, whole or split. barNavInput needs this for the one rule
// that turns on it: B with a lift is CANCEL and never BACK, so a player holding a stack cannot
// close the bar out from under it.
static bool barHasLift(const UiState* ui)
{
	return ui->picked_slot >= 0 || ui->picked_chest >= 0 || liftDetached(ui);
}

// Put everything back down. returnLift first, then the two indices: a detached half belongs in
// lift_from, and dropLifts alone would strand it (reconcileLift would then have to notice next
// frame). The same prologue handleQuickMove already uses, for the same reason.
static void barCancelLift(UiState* ui, Inventory* inv)
{
	returnLift(ui, inv);
	dropLifts(ui);
}

static void closeBar(UiState* ui, Inventory* inv)
{
	barCancelLift(ui, inv);
	ui->screen = UI_SCR_HUD;
}

// B on the HUD, or a tap on the HUD strip. The cursor starts on the IN-HAND hotbar slot: row 0
// is the hotbar on every grid kind, and the column the player is holding is the one they were
// already thinking about — blueprint 1B.1 is written as an observation about exactly this.
//
// opened_this_frame is deliberately NOT set here, unlike uiOpenChest/uiOpenFurnace. This runs
// inside uiUpdateDraw, after the frame's `bar_open` was captured, so the bar does not read the
// pad until the next frame and the B that opened it is long gone by then (hidKeysDown is an
// edge). Setting it here would cost the player one whole frame of d-pad on every open and buy
// nothing.
static void openBar(UiState* ui, const Inventory* inv)
{
	ui->screen      = UI_SCR_BAR;
	ui->list_scroll = 0;
	barNavReset(&ui->nav, 0, 0, (int)inv->selected_hotbar);
}

// ── The strip ──────────────────────────────────────────────────────────────────────────

static void drawBarStrip(const BarCats* cats, int focused)
{
	const URect sr = barStripRect();
	spriteRect((float)sr.x, (float)sr.y, (float)sr.w, (float)sr.h, COL_BG);

	for (int i = 0; i < cats->count; i++) {
		const URect t = barTabRect(i, cats->count);
		const bool  f = (i == focused);

		// One pixel narrower than the tab it fills, so the strip's own background shows through
		// as the divider. barTabRect TILES — it leaves no gap for a divider to sit in, by
		// design — so the alternative is a quad per seam for a 1-px line, and this is the same
		// "draw the track, then the fill on top" trick drawPipRow and drawFurnaceBar already
		// use. The HIT box is the full tab: the divider is a drawn pixel, not a dead one.
		spriteRect((float)t.x, (float)t.y, (float)(t.w - 1), (float)t.h,
		           f ? COL_PANEL_HI : COL_PANEL);
		if (f)
			spriteRect((float)t.x, (float)(t.y + t.h - 2), (float)(t.w - 1), 2, COL_ACCENT);

		// Scale 1 on the focused tab too, not scale 2. "INVENTORY" is 108 px at scale 2 and a
		// three-tab tab is 98; and a label that grew on focus would move its neighbours'
		// centres, which pausemenu.c's own comment already names as reading like a glitch.
		const char* label = barKindLabel(cats->kinds[i]);
		const int   lw    = fontTextWidth(label, 1);
		fontDraw((float)t.x + ((float)(t.w - 1) - (float)lw) * 0.5f,
		         (float)t.y + (t.h - FONT_GLYPH_H) * 0.5f, 1,
		         f ? COL_TEXT : COL_TEXT_DIM, label);
	}

	const URect cr = barCloseRect();
	spriteRect((float)cr.x, (float)cr.y, (float)cr.w, (float)cr.h, COL_PANEL);
	const int xw = fontTextWidth("X", 1);
	fontDraw((float)cr.x + (cr.w - (float)xw) * 0.5f,
	         (float)cr.y + (cr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "X");
}

// The focused cell's outline, drawn over the finished cell. One pixel and COL_ACCENT: the
// hotbar's "in hand" cue is a 2-px top stripe in the same colour and the lifted-slot cue is a
// 2-px full border in COL_PICKED, so this is a third weight inside a family the player already
// reads rather than a fourth colour to learn. A zero rect (an off-window list row, or a cell
// that does not exist) draws nothing.
static void drawFocusRing(URect r)
{
	if (r.w <= 0 || r.h <= 0) return;
	spriteRect((float)r.x, (float)r.y, (float)r.w, 1, COL_ACCENT);
	spriteRect((float)r.x, (float)(r.y + r.h - 1), (float)r.w, 1, COL_ACCENT);
	spriteRect((float)r.x, (float)r.y, 1, (float)r.h, COL_ACCENT);
	spriteRect((float)(r.x + r.w - 1), (float)r.y, 1, (float)r.h, COL_ACCENT);
}

// The INVENTORY tab's detail band: what the cursor is on, or what is in the air. The lift wins
// when there is one, because that is the stack the player is currently deciding about and the
// cursor has already moved off it by the time they are looking for somewhere to put it.
//
// The focused cell is resolved back to a bag slot through the SAME hit test a stylus tap uses,
// against the centre of barCellRect — not by arithmetic on (row, col). Two ways of turning a
// cursor into a slot index is exactly how the readout ends up naming a different slot from the
// one A would act on.
static void drawBarDetail(const UiState* ui, const Inventory* inv)
{
	InvSlot show = { .item = ITEM_NONE, .count = 0 };

	if (liftDetached(ui)) {
		show = ui->lift;
	} else if (ui->picked_slot >= 0) {
		show = inv->slots[ui->picked_slot];
	} else {
		const URect r = barCellRect(BAR_KIND_INVENTORY, ui->nav.row, ui->nav.col, 0);
		if (r.w > 0) {
			const int slot = hitInventorySlot(r.x + r.w / 2, r.y + r.h / 2, true);
			if (slot >= 0) show = inv->slots[slot];
		}
	}

	if (show.item == ITEM_NONE) {
		fontDraw(6, (float)(BAR_DETAIL_Y + 4), 1, COL_TEXT_DIM, "EMPTY");
		return;
	}

	// T1 (scale 2) for the name, T2 (scale 1) for the count — the whole of the type scale this
	// font supports (gfx/font.h is 5x7 with integer scales only), see blueprint D4.
	fontDraw(6, (float)(BAR_DETAIL_Y + 4), 2, COL_TEXT, blockInfo(show.item)->name);
	fontDrawf(6, (float)(BAR_DETAIL_Y + 26), 1, COL_TEXT_DIM, "x %u", (unsigned)show.count);
}

// ── Entry points ───────────────────────────────────────────────────────────────────────

void uiInit(UiState* ui)
{
	memset(ui, 0, sizeof(*ui));   // also clears chest_fn/chest_ud — register after this
	ui->screen = UI_SCR_HUD;
	dropLifts(ui);

	// A memset-zero BarNav is already the correct initial state (scene/barnav.h says so). Reset
	// anyway, so the one place a reader looks for "what does a fresh UiState's cursor hold"
	// answers it out loud instead of by reference to another header's promise.
	barNavReset(&ui->nav, 0, 0, 0);
	ui->list_scroll       = 0;
	ui->opened_this_frame = false;
}

void uiOpenFurnace(UiState* ui)
{
	ui->screen = UI_SCR_BAR;

	// The container is always tab 2 — barBuildKinds fixes that order and ui_layout.h's own
	// comment says why a caller may rely on it. Written straight into nav.cat rather than
	// through barNavSetCat because the category list is built inside uiUpdateDraw out of the
	// two pointers the caller has not handed in yet; the next frame's barNavClamp is what makes
	// this real, and it is also what puts the cursor back on tab 0 if the caller opened a
	// container it then never passed in.
	ui->nav.cat = 2;

	// This runs BETWEEN frames, from main.c's world-interact path, and the button that got the
	// player here is still a press edge in the keys_down word uiUpdateDraw is about to be
	// handed. options.h lets ACTION_PLACE be bound to A, and A on the bar is COMMIT — so
	// without this, opening a chest would lift or drop on whatever cell the cursor landed on,
	// in the same frame, before the player saw the panel. See UiState.opened_this_frame.
	ui->opened_this_frame = true;

	// Never carry a lift across a screen boundary — the same rule the HUD toggle and the
	// container close bars already followed, and it matters more here: picked_slot indexes
	// inv->slots[], and the furnace tab draws a DIFFERENT set of those slots in different
	// places, so a stale lift would highlight a cell the player never touched. Both lifts,
	// since v1.9.0 CHEST: a chest-slot lift left over from a chest panel would otherwise index
	// a chest this tab is not showing.
	dropLifts(ui);
}

void uiOpenChest(UiState* ui, int x, int y, int z)
{
	ui->screen            = UI_SCR_BAR;
	ui->nav.cat           = 2;              // see uiOpenFurnace above for both of these
	ui->opened_this_frame = true;
	ui->chest_x = x;
	ui->chest_y = y;
	ui->chest_z = z;
	// Same reset as uiOpenFurnace above, for the same reason, and one more: a chest-slot lift
	// pending from a PREVIOUS chest would index a slot of the chest now being opened, which is
	// not the stack the player lifted. Cancelling a lift never moves anything — the lifted
	// stack was never taken out of its source — so this is always safe to do.
	dropLifts(ui);
}

void uiSetChestTransferFn(UiState* ui, UiChestTransferFn fn, void* ud)
{
	ui->chest_fn = fn;
	ui->chest_ud = ud;
}

void uiApplyInvSnapshot(UiState* ui, Inventory* inv, const NetworldInvState* state)
{
	if (!ui || !inv || !state) return;

	// Cleared BEFORE the apply, not after, and by zeroing rather than by returnLift(): the
	// units in the air are already inside the counts `state` is about to write (the server was
	// never told they left the origin slot), so putting them back into `inv` first would land
	// them in slots the apply is about to overwrite anyway — and putting them back AFTER would
	// be the duplication this function exists to stop, just spelled differently. See ui.h.
	ui->lift.item  = ITEM_NONE;
	ui->lift.count = 0;
	ui->lift_from  = -1;

	// picked_slot is deliberately left alone. It is an INDEX, not a copy of anything: it names
	// a bag slot whose contents the snapshot is free to change, and the panel re-reads that
	// slot every frame, so the player sees the new contents under the highlight rather than a
	// stale amount. Nothing is conserved through it, so nothing can be duplicated by it.
	(void)invBridgeApplyState(inv, state);
}

UiResult uiUpdateDraw(UiState* ui, Inventory* inv, C3D_Tex* block_icons,
                       const UiStats* stats, const UiInput* in, FurnaceState* furnace,
                       ChestState* chest)
{
	// Rising edge only, same as title.c's own `tap` — see title.h's TitleInput comment for
	// why (KEY_TOUCH is never actually set by hidScanInput, so the caller derives
	// touch_down some other way, and a tap should still fire once per press regardless).
	const bool tap = in->touch_down && !ui->touch_prev;
	ui->touch_prev = in->touch_down;
	if (tap) audioPlay(audioSfxId(SFX_UI_TAP), AUDIO_PRIO_UI, 1.0f);

	// v1.9.1 BAR. The frame the bar became visible ignores this frame's press edges — read and
	// cleared here, before anything below can set it again, so it is a one-frame flag and not a
	// mode. See UiState.opened_this_frame for the button that makes it necessary.
	const bool fresh_open = ui->opened_this_frame;
	ui->opened_this_frame = false;

	// v1.9.1 BAR. The live category list, rebuilt from the two container pointers every frame.
	// This is what REPLACES the two same-frame screen corrections that used to sit here: a
	// container whose state is no longer being handed in simply has no tab, so there is no
	// screen to correct and no pointer that could be dereferenced while NULL. barNavClamp then
	// puts a cursor that was sitting on that tab back onto a real cell — category 0 is
	// INVENTORY, which always exists — which is the same "safe and escapable" outcome the old
	// fallback produced, reached without a special case.
	//
	// Clamped HERE and not left to barNavInput's own leading clamp, because everything below —
	// the tap chain, the detail band, the focus ring, the draw — indexes cats.kinds[nav.cat],
	// and a frame where the bar draws but no key is pressed never reaches barNavInput at all.
	BarCats cats;
	barBuildCats(&cats, chest != NULL, furnace != NULL);
	barNavClamp(&ui->nav, cats.specs, cats.count);

	// v1.9.1 BAR fix (2026-09-07). barNavClamp above moves the CURSOR off the vanished chest
	// tab, but a cursor position was never what ui->picked_chest is — it is a slot INDEX into
	// the ChestState the caller is no longer handing in, and clamping the nav never touched it.
	// Left alone, it keeps its old value across however many chests get opened after this one:
	// handleChestSlotTap's "nothing lifted" branch is gated on `picked_chest < 0`, so the very
	// next chest tap is read as a place/cancel against a DIFFERENT chest's slot at that same
	// stale index — a wrong-chest write, the shape of bug this fix exists to close. This is the
	// vanish path itself (barBuildCats/barNavClamp is where the tab actually disappears, not
	// some call site nearby), so this is where the index that only made sense on that tab has
	// to go with it: `chest == NULL` is precisely "no ChestState exists for picked_chest to
	// name" regardless of which tab the cursor lands on.
	//
	// Deliberately NOT dropLifts(): that also clears ui->picked_slot, the BAG lift, and an
	// ordinary tab switch (BAR_EV_CAT, handled below) never touches picked_slot — a bag lift
	// survives moving from the chest tab to the inventory tab by hand, so an automatic clamp
	// forced by the chest vanishing must not treat it any differently, or picking up a bag item
	// then having the chest close under you (out of range, broken by another player) would
	// silently drop what you were holding. Only the chest-side index is meaningless here.
	if (!chest && ui->picked_chest >= 0)
		ui->picked_chest = -1;

	// Captured once and used for both the input logic and the draw below, so a tap is always
	// interpreted against the screen the player actually saw this frame — any screen switch a
	// handler makes below (opening or closing the bar) takes effect next frame, the same
	// "change applies next frame" rule title.c's own ts->screen follows. The focused TAB is
	// different and is read after the key phase: a tab change is a move inside one screen, and
	// making L/R wait a frame would read as lag rather than as a rule.
	const bool bar_open = (ui->screen == UI_SCR_BAR);

	// v1.9.0 SPLIT. A detached half whose origin is no longer the lifted slot — some site
	// above or in a previous frame dropped the lift — goes back into the bag before anything
	// reads or draws the bag this frame. See the detached-lift block above handleSlotTap.
	reconcileLift(ui, inv);

	// ── The controller phase. Runs BEFORE the tab is read and before the gesture block, because
	// a COMMIT has to become a touch point that the rest of this frame then processes exactly as
	// if the stylus had produced it (blueprint D8) — the whole reason there is only ONE hit-test
	// chain below and not a parallel controller one that could drift away from it.
	//
	// Skipped on the frame the bar opened (see fresh_open), and skipped entirely on the HUD:
	// barNavInput would move a cursor nobody can see, and B on the HUD is the OPEN button
	// handled after the tap chain.
	bool synth_tap = false;
	int  synth_x = 0, synth_y = 0;
	if (bar_open && !fresh_open) {
		const BarEvent ev = barNavInput(&ui->nav, in->keys_down, cats.specs, cats.count,
		                                barHasLift(ui));
		switch (ev) {
		case BAR_EV_BACK:
			closeBar(ui, inv);
			break;
		case BAR_EV_CANCEL:
			// B with something in the air puts it down and leaves the bar up. barnav.h states
			// the rule; this is the whole of acting on it.
			barCancelLift(ui, inv);
			break;
		case BAR_EV_COMMIT: {
			// The CENTRE of the focused cell, never its origin: barCellRect's origin is the
			// top-left CORNER, which on every one of these rects is a border pixel, and the hit
			// tests below reject a border pixel the same way they reject a gutter. A commit
			// aimed at the origin would silently do nothing on the cells whose rects abut.
			const URect r = barCellRect(cats.kinds[ui->nav.cat], ui->nav.row, ui->nav.col,
			                            ui->list_scroll);
			if (r.w > 0 && r.h > 0) {
				synth_tap = true;
				synth_x   = r.x + r.w / 2;
				synth_y   = r.y + r.h / 2;
			}
			break;
		}
		case BAR_EV_CAT:
		case BAR_EV_MOVE:
		case BAR_EV_ALT:
		case BAR_EV_STEP_LEFT:
		case BAR_EV_STEP_RIGHT:
			// Movement and the reserved X. barnav has already updated the cursor; there is
			// nothing for this file to do but say it happened. STEP_* cannot occur on any v1.9.1
			// category (no kind sets horizontal_is_content on a one-column row) and ALT has no
			// caller until the split gesture gets a button — both are listed rather than folded
			// into a default so adding one later is a compile-visible edit here.
			break;
		case BAR_EV_NONE:
			break;
		}
		if (ev != BAR_EV_NONE) audioPlay(audioSfxId(SFX_UI_TAP), AUDIO_PRIO_UI, 1.0f);
	}

	// Read AFTER the key phase, so an L/R tab change is visible on the frame it happens rather
	// than a frame later — see the bar_open comment above for why the SCREEN does not work that
	// way and a tab does.
	const BarKind kind = bar_open ? cats.kinds[ui->nav.cat] : BAR_KIND_INVENTORY;
	const bool inv_tab   = bar_open && kind == BAR_KIND_INVENTORY;
	const bool craft_tab = bar_open && kind == BAR_KIND_CRAFT;
	const bool chest_tab = bar_open && kind == BAR_KIND_CHEST;
	const bool furn_tab  = bar_open && kind == BAR_KIND_FURNACE;

	// The recipe list is the one category taller than its window. Followed here, after the
	// cursor has moved and before anything converts a cell to a rect, so the scroll the hit
	// tests use is the same one the draw uses.
	if (craft_tab)
		ui->list_scroll = barNavScrollFor(ui->nav.row, ui->list_scroll, BAR_LIST_VISIBLE,
		                                  RECIPE_COUNT);

	// The effective touch point for the rest of the frame: the stylus, or the synthetic tap a
	// COMMIT produced. One point, one chain — a controller cannot reach a slot the stylus
	// cannot, and it cannot miss one the stylus hits.
	const bool eff_tap  = tap || synth_tap;
	const bool eff_down = in->touch_down || synth_tap;
	const int  eff_x    = synth_tap ? synth_x : in->touch_x;
	const int  eff_y    = synth_tap ? synth_y : in->touch_y;

	// v1.9.0 SPLIT. The gestures: bag-slot taps (lift / place / merge), Y (split) and X
	// (quick-move), classified by scene/ui_gesture.h and acted on here. Fed EVERY frame, HUD
	// included — the module's edge state must track the buttons continuously (its header says
	// why) — and BEFORE the tap chain below, so a tap event is classified against the lift
	// state the player saw. The chain no longer handles bag-slot taps on the INVENTORY or
	// FURNACE tabs (those are this block's), and on the CHEST tab it handles them only while a
	// CHEST slot is lifted (the withdraw), which is exactly when lift_enabled is off.
	{
		int slot_under = -1;
		if (eff_down) {
			if (furn_tab)       slot_under = hitFurnaceInvSlot(eff_x, eff_y);
			else if (chest_tab) slot_under = hitChestInvSlot(eff_x, eff_y);
			else if (inv_tab)   slot_under = hitInventorySlot(eff_x, eff_y, true);
		}
		const UiGestureInput gin = {
			.touch_down    = eff_down,
			.slot_under    = slot_under,
			.keys_held     = in->keys_held,
			.lifted_slot   = ui->picked_slot,
			.lift_enabled  = inv_tab || furn_tab || (chest_tab && ui->picked_chest < 0),
			.split_enabled = inv_tab,
			.chest_open    = chest_tab,
		};
		const UiGestureEvent ev = uiGestureFeed(&ui->gesture, &gin, inv);
		switch (ev.kind) {
		case UI_GESTURE_LIFT:       handleSlotTap(ui, inv, ev.src); break;
		case UI_GESTURE_PLACE:
		case UI_GESTURE_MERGE:      handleSlotTap(ui, inv, ev.dst); break;
		case UI_GESTURE_SPLIT:      handleSplit(ui, inv, ev.src); break;
		case UI_GESTURE_QUICK_MOVE: handleQuickMove(ui, inv, chest, ev.src, ev.to_chest); break;
		case UI_GESTURE_NONE:       break;
		}
	}

	if (eff_tap && bar_open) {
		const int tx = eff_x, ty = eff_y;

		// The strip first, on every tab: it is the one control that means the same thing
		// everywhere, and it sits above every content area, so nothing below can shadow it.
		const int hit = hitBarStrip(tx, ty, cats.count);
		if (hit == BAR_HIT_CLOSE) {
			closeBar(ui, inv);
		} else if (hit != BAR_HIT_NONE) {
			barNavSetCat(&ui->nav, hit, cats.specs, cats.count);
		} else {
			// A tap inside the content area moves the CURSOR to the cell it landed on before
			// that cell is acted on, so the focus ring never trails the stylus and a player who
			// mixes the two input methods finds the cursor where they last touched. Failing the
			// hit test (a gutter, the detail band) leaves the cursor alone.
			int row = 0, col = 0;
			if (barCellFromPoint(kind, tx, ty, ui->list_scroll, &row, &col))
				barNavSetCell(&ui->nav, row, col, cats.specs, cats.count);

			switch (kind) {
			case BAR_KIND_CRAFT: {
				// Walked over the VISIBLE rows and offset by the scroll, not over all
				// RECIPE_COUNT rows: barListRowRect answers in screen space for a window row,
				// so a recipe scrolled off the top has no rect here and cannot be tapped.
				for (int v = 0; v < BAR_LIST_VISIBLE; v++) {
					const int r = ui->list_scroll + v;
					if (r >= RECIPE_COUNT) break;
					if (ptInRect(barListRowRect(v), tx, ty)) { handleCraftTap(inv, r); break; }
				}
				break;
			}
			case BAR_KIND_FURNACE: {
				const int fslot = hitFurnaceSlot(tx, ty);
				if (fslot != FURN_HIT_NONE) {
					// The output slot is a withdrawal either way — see the comment above
					// furnaceDeposit for why it needs no lifted/not-lifted branch.
					if (fslot == FURN_HIT_OUTPUT || ui->picked_slot < 0)
						furnaceWithdraw(inv, furnace, fslot);
					else
						furnaceDeposit(ui, inv, furnace, fslot);
				}
				// A tap on the bag grid (hitFurnaceInvSlot) is the gesture block's above, which
				// hit-tests the same rect and runs the same two-tap lift/drop.
				break;
			}
			case BAR_KIND_CHEST: {
				const int cslot = hitChestSlot(tx, ty);
				if (cslot >= 0) {
					handleChestSlotTap(ui, inv, chest, cslot);
				} else {
					// hitChestInvSlot, not hitInventorySlot: the bag grid is at CHEST_GRID_Y on
					// this tab, the same relocation the furnace tab makes for its own grid.
					const int slot = hitChestInvSlot(tx, ty);
					if (slot >= 0) handleChestInvSlotTap(ui, inv, chest, slot);
				}
				break;
			}
			case BAR_KIND_INVENTORY:
			default:
				// Every bag tap on this tab is the gesture block's — it is the only tab where
				// split and quick-move are live, and routing the tap twice would run the lift
				// again on top of the gesture's.
				break;
			}
		}
	} else if (eff_tap) {
		// The HUD. Two controls: the toggle strip and the hotbar.
		const int tx = eff_x, ty = eff_y;
		if (ptInRect(hudToggleRect(), tx, ty)) {
			openBar(ui, inv);
		} else {
			const int slot = hitInventorySlot(tx, ty, false);
			if (slot >= 0) handleHotbarSelect(inv, slot);
		}
	}

	// v1.9.1 BAR, blueprint D7. B opens the bar from the HUD. Read here rather than in the key
	// phase above, which only runs while the bar is already up. B is the one button options.h
	// never lets the player rebind, so this cannot collide with a world action.
	if (!bar_open && (in->keys_down & BAR_KEY_B)) openBar(ui, inv);

	spriteBegin(SCR_W, SCR_H);

	// ── Pass 1: font texture. Every panel, border, label and count badge on the screen,
	// regardless of which tab is showing — one texture, one flush, one draw call, batched
	// exactly the way gfx/sprite.h's own file comment describes a whole UI screen should be.
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	// The hotbar is the one strip drawn on every tab, at the same rects, always — the standing
	// rule that the player must be able to see what they are holding. That includes the "in
	// hand" accent: it answers "what is in your hand", which remains true and worth knowing
	// while the player is loading a furnace, even though a tap on that same cell means pick-up
	// rather than select while a container tab is up.
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		drawSlotIcon(PASS_FONT, hotbarSlotRect(i), &inv->slots[i],
		             inv->selected_hotbar == i, ui->picked_slot == i, block_icons);

	if (bar_open) {
		drawBarStrip(&cats, ui->nav.cat);

		switch (kind) {
		case BAR_KIND_CRAFT:
			drawBarCraftList(inv, ui->list_scroll, ui->nav.row);
			break;
		case BAR_KIND_FURNACE:
			for (int i = 0; i < INV_MAIN_SLOTS; i++) {
				const int slot = INV_HOTBAR_SLOTS + i;
				drawSlotIcon(PASS_FONT, furnGridSlotRect(i), &inv->slots[slot],
				             false, ui->picked_slot == slot, block_icons);
			}
			drawFurnacePanelFont(furnace, inv, ui->picked_slot, block_icons);
			break;
		case BAR_KIND_CHEST:
			for (int i = 0; i < INV_MAIN_SLOTS; i++) {
				const int slot = INV_HOTBAR_SLOTS + i;
				drawSlotIcon(PASS_FONT, chestGridSlotRect(i), &inv->slots[slot],
				             false, ui->picked_slot == slot, block_icons);
			}
			drawChestPanelFont(chest, ui->picked_chest, block_icons);
			break;
		case BAR_KIND_INVENTORY:
		default:
			for (int i = 0; i < INV_MAIN_SLOTS; i++) {
				const int slot = INV_HOTBAR_SLOTS + i;
				drawSlotIcon(PASS_FONT, gridSlotRect(i), &inv->slots[slot],
				             false, ui->picked_slot == slot, block_icons);
			}
			drawBarDetail(ui, inv);

			// The vitals move onto this tab rather than off the screen: the INVENTORY tab is
			// the one with room below the bag grid, and a player deciding what to eat should not
			// have to close the bar to see how hungry they are.
			if (stats) drawHudVitals(stats);
			fontDraw(6, (float)BAR_HINT_Y, 1, COL_TEXT_DIM, "A LIFT/DROP  B CLOSE  L/R TAB");
			break;
		}

		// Last in the font pass, so it outlines a cell that is already finished. CRAFT is
		// excluded because drawBarCraftList already highlights its focused row across the full
		// width — a ring on top of that would be two cues for one thing.
		if (kind != BAR_KIND_CRAFT)
			drawFocusRing(barCellRect(kind, ui->nav.row, ui->nav.col, ui->list_scroll));
	} else {
		drawHudFont(stats);
	}

	// ── Pass 2: the block atlas, if the caller has one to give (see ui.h's file comment on
	// why this can be NULL). Every icon on the screen, one texture, one more flush — two
	// draw calls total for a screen with block icons on it, which is the honest floor the
	// step 8.2 UI task asked for; skipped entirely when there is nothing to draw with it, so
	// a caller with no atlas texture wired up yet costs exactly one draw call, not a wasted
	// second one that would just bind and draw nothing.
	if (block_icons) {
		spriteTexture(block_icons);

		for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
			drawSlotIcon(PASS_ATLAS, hotbarSlotRect(i), &inv->slots[i], false, false,
			             block_icons);

		if (bar_open) {
			switch (kind) {
			case BAR_KIND_CRAFT:
				drawBarCraftIcons(ui->list_scroll);
				break;
			case BAR_KIND_FURNACE:
				for (int i = 0; i < INV_MAIN_SLOTS; i++) {
					const int slot = INV_HOTBAR_SLOTS + i;
					drawSlotIcon(PASS_ATLAS, furnGridSlotRect(i), &inv->slots[slot], false, false,
					             block_icons);
				}

				// The three furnace cells, in the same pass as every other icon on the screen, so
				// adding this panel costs no third draw call — the whole point of the two-pass
				// split described above.
				for (int which = FURN_HIT_INPUT; which <= FURN_HIT_OUTPUT; which++) {
					ItemId  item  = ITEM_NONE;
					uint8_t count = 0;
					switch (which) {
					case FURN_HIT_INPUT:  item = furnace->input_item;  count = furnace->input_count;  break;
					case FURN_HIT_FUEL:   item = furnace->fuel_item;   count = furnace->fuel_count;   break;
					default:              item = furnace->output_item; count = furnace->output_count; break;
					}
					const InvSlot view = furnSlotView(item, count);
					drawSlotIcon(PASS_ATLAS, furnSlotRect(which), &view, false, false, block_icons);
				}
				break;
			case BAR_KIND_CHEST:
				for (int i = 0; i < INV_MAIN_SLOTS; i++) {
					const int slot = INV_HOTBAR_SLOTS + i;
					drawSlotIcon(PASS_ATLAS, chestGridSlotRect(i), &inv->slots[slot], false, false,
					             block_icons);
				}

				// The chest's CHEST_SLOTS cells, in the same pass as every other icon on the
				// screen — same two-draw-call reasoning as the furnace cells just above.
				for (int i = 0; i < CHEST_SLOTS; i++) {
					const InvSlot view = furnSlotView(chest->item[i], chest->count[i]);
					drawSlotIcon(PASS_ATLAS, chestSlotRect(i), &view, false, false, block_icons);
				}
				break;
			case BAR_KIND_INVENTORY:
			default:
				for (int i = 0; i < INV_MAIN_SLOTS; i++) {
					const int slot = INV_HOTBAR_SLOTS + i;
					drawSlotIcon(PASS_ATLAS, gridSlotRect(i), &inv->slots[slot], false, false,
					             block_icons);
				}
				break;
			}
		}
	}

	spriteEnd();

	// Derived from the POINTERS, never from nav.cat. The caller asks these to decide whether to
	// keep feeding this furnace/chest in, and answering "a container tab is focused" would make
	// that circular: the tab exists because the pointer was passed, so reporting the tab back
	// would let one frame on a different tab tear the container down under the player.
	UiResult out;
	out.bar_open     = bar_open;
	out.furnace_open = bar_open && furnace != NULL;
	out.chest_open   = bar_open && chest != NULL;
	return out;
}
