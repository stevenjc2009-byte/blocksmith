#include "scene/ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "debug/biomeinfo.h"
#include "net/inv_bridge.h"
#include "scene/chunk_render.h"   // chunkRenderCamera, for the biome row's position
#include "scene/ui_layout.h"
#include "world/block.h"
#include "world/crafting.h"
#include "world/furnace.h"   // furnaceRecipeForInput / furnaceIsFuel, for the accept rules

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
// URect, ptInRect, hotbarSlotRect, gridSlotRect, craftCloseRect, craftRowRect,
// hudToggleRect and hitInventorySlot all moved to scene/ui_layout.h/.c — see that header's
// file comment for why (host-testability) and its own comments for what each one does.
// This file now only calls into them.

// ── Mutating gesture handlers ─────────────────────────────────────────────────────────
//
// v1.3.0: every mutation below goes through net/inv_bridge.h rather than calling
// world/inventory.h and world/crafting.h directly. Each invBridge* call performs the exact
// same local operation it is named after and returns the same value — the semantics
// described in each comment below are unchanged — and additionally reports what happened to
// the server, so a joined session's inventory survives a rejoin instead of living only on
// this console. In single player nothing is sent (see inv_bridge.h on the capability probe),
// so this file behaves identically offline.

// See the file comment for the two-tap pick-up/drop gesture this implements.
static void handleSlotTap(UiState* ui, Inventory* inv, int slot)
{
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
	if (craftCanMake(inv, recipe_index)) invBridgeCraft(inv, recipe_index);
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
// FACE_TOP is used for every block's icon — grass shows its green top, wood shows its ring
// pattern, and every other block in this game's six-block list (world/block.h) is the same
// texture on every face, so there is no "which face reads best as an icon" decision to make
// beyond picking one consistently.
static void iconUv(BlockId id, float* u0, float* v0, float* u1, float* v1)
{
	const AtlasRect r = atlasTile(blockFaceTex(id, FACE_TOP));
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
	spriteQuad((float)(r.x + ICON_INSET_X), (float)(r.y + ICON_INSET_TOP),
	           (float)(SLOT_PX - 2 * ICON_INSET_X), (float)ICON_H,
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
	const URect tr = hudToggleRect();
	spriteRect((float)tr.x, (float)tr.y, (float)tr.w, (float)tr.h, COL_PANEL);
	const int tw = fontTextWidth("OPEN INVENTORY", 1);
	fontDraw((float)tr.x + (tr.w - (float)tw) * 0.5f,
	         (float)tr.y + (tr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, "OPEN INVENTORY");

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

// ── Inventory overlay (font pass) ─────────────────────────────────────────────────────

static void drawCraftPanelFont(const Inventory* inv)
{
	const URect cr = craftCloseRect();
	spriteRect((float)cr.x, (float)cr.y, (float)cr.w, (float)cr.h, COL_PANEL_HI);
	const char* label = "CRAFTING - TAP TO CLOSE";
	const int lw = fontTextWidth(label, 1);
	fontDraw((float)cr.x + (cr.w - (float)lw) * 0.5f,
	         (float)cr.y + (cr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

	for (int i = 0; i < RECIPE_COUNT; i++) {
		const URect rr = craftRowRect(i);
		const bool makeable = craftCanMake(inv, i);
		spriteRect((float)rr.x, (float)rr.y, (float)rr.w, (float)rr.h,
		           makeable ? COL_CRAFT_OK : COL_PANEL);
		fontDraw((float)rr.x + 6, (float)rr.y + (rr.h - FONT_GLYPH_H) * 0.5f, 1,
		         makeable ? COL_TEXT : COL_TEXT_DIM, CRAFT_RECIPES[i].name);
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
	const URect cr = furnCloseRect();
	spriteRect((float)cr.x, (float)cr.y, (float)cr.w, (float)cr.h, COL_PANEL_HI);
	const char* label = "FURNACE - TAP TO CLOSE";
	const int lw = fontTextWidth(label, 1);
	fontDraw((float)cr.x + (cr.w - (float)lw) * 0.5f,
	         (float)cr.y + (cr.h - FONT_GLYPH_H) * 0.5f, 1, COL_TEXT, label);

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

// ── Entry points ───────────────────────────────────────────────────────────────────────

void uiInit(UiState* ui)
{
	memset(ui, 0, sizeof(*ui));
	ui->screen = UI_SCR_HUD;
	ui->picked_slot = -1;
}

void uiOpenFurnace(UiState* ui)
{
	ui->screen = UI_SCR_FURNACE;
	// Never carry a lift across a screen boundary — the same rule the OPEN INVENTORY toggle
	// and the crafting close bar already follow in uiUpdateDraw, and it matters more here:
	// picked_slot indexes inv->slots[], and the furnace screen draws a DIFFERENT set of those
	// slots in different places, so a stale lift would highlight a cell the player never
	// touched.
	ui->picked_slot = -1;
}

UiResult uiUpdateDraw(UiState* ui, Inventory* inv, C3D_Tex* block_icons,
                       const UiStats* stats, const UiInput* in, FurnaceState* furnace)
{
	// Rising edge only, same as title.c's own `tap` — see title.h's TitleInput comment for
	// why (KEY_TOUCH is never actually set by hidScanInput, so the caller derives
	// touch_down some other way, and a tap should still fire once per press regardless).
	const bool tap = in->touch_down && !ui->touch_prev;
	ui->touch_prev = in->touch_down;

	// v1.8.15 FURNACE. The one same-frame screen correction in this file, and the only one:
	// UI_SCR_FURNACE with no FurnaceState behind it is a caller contract violation (ui.h says
	// the two travel together), and every alternative to correcting it here is worse. Drawing
	// the furnace panel would mean dereferencing the NULL this function was told not to touch;
	// drawing nothing would leave the player on a dead screen with no close bar to tap and no
	// way back. Falling back to the inventory overlay is the one outcome that is both safe and
	// escapable, and it happens BEFORE the two flags below are read so the rest of this
	// function never sees the inconsistent state at all.
	if (ui->screen == UI_SCR_FURNACE && furnace == NULL) {
		ui->screen = UI_SCR_INVENTORY;
		ui->picked_slot = -1;
	}

	// Captured once and used for both the tap logic and the draw below, so a tap is always
	// interpreted against the screen the player actually saw this frame — any screen switch
	// a handler makes below (opening or closing the overlay) takes effect next frame, the
	// same "change applies next frame" rule title.c's own ts->screen follows.
	const bool overlay_open = (ui->screen == UI_SCR_INVENTORY);
	const bool furnace_open = (ui->screen == UI_SCR_FURNACE);

	if (tap && furnace_open) {
		const int tx = in->touch_x, ty = in->touch_y;

		if (ptInRect(furnCloseRect(), tx, ty)) {
			// Back to the inventory overlay rather than to the HUD: the player opened this
			// panel to move things around, and the bag is where the rest of their things are.
			// Closing all the way out to the world would make "put the leftovers away" a
			// second trip through the OPEN INVENTORY toggle.
			ui->screen = UI_SCR_INVENTORY;
			ui->picked_slot = -1;
		} else {
			const int fslot = hitFurnaceSlot(tx, ty);
			if (fslot != FURN_HIT_NONE) {
				// The output slot is a withdrawal either way — see the gesture block above
				// furnaceDeposit for why it needs no lifted/not-lifted branch.
				if (fslot == FURN_HIT_OUTPUT || ui->picked_slot < 0)
					furnaceWithdraw(inv, furnace, fslot);
				else
					furnaceDeposit(ui, inv, furnace, fslot);
			} else {
				// hitFurnaceInvSlot, not hitInventorySlot: the main grid is at FURN_GRID_Y on
				// this screen. Same two-tap lift/drop as the inventory overlay from here on —
				// the bag behaves identically whichever screen it is drawn on.
				const int slot = hitFurnaceInvSlot(tx, ty);
				if (slot >= 0) handleSlotTap(ui, inv, slot);
			}
		}
	} else if (tap) {
		const int tx = in->touch_x, ty = in->touch_y;

		if (!overlay_open) {
			if (ptInRect(hudToggleRect(), tx, ty)) {
				ui->screen = UI_SCR_INVENTORY;
				ui->picked_slot = -1;   // never carry a lift across the screen boundary
			} else {
				const int slot = hitInventorySlot(tx, ty, false);
				if (slot >= 0) handleHotbarSelect(inv, slot);
			}
		} else {
			if (ptInRect(craftCloseRect(), tx, ty)) {
				ui->screen = UI_SCR_HUD;
				ui->picked_slot = -1;
			} else {
				const int slot = hitInventorySlot(tx, ty, true);
				if (slot >= 0) {
					handleSlotTap(ui, inv, slot);
				} else {
					for (int i = 0; i < RECIPE_COUNT; i++) {
						if (ptInRect(craftRowRect(i), tx, ty)) {
							handleCraftTap(inv, i);
							break;
						}
					}
				}
			}
		}
	}

	spriteBegin(SCR_W, SCR_H);

	// ── Pass 1: font texture. Every panel, border, label and count badge on the screen,
	// regardless of which of the three screens is showing — one texture, one flush, one draw
	// call, batched exactly the way gfx/sprite.h's own file comment describes a whole UI
	// screen should be.
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	// The hotbar is the one strip drawn on every screen, at the same rects, always — the
	// standing rule that the player must be able to see what they are holding. The furnace
	// screen changed nothing about this loop, including the "in hand" accent, which stays
	// visible on all three screens exactly as it already did on two: it answers "what is in
	// your hand", which remains true and worth knowing while the player is loading a furnace,
	// even though a tap on that same cell means pick-up rather than select while a storage
	// screen is up.
	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		drawSlotIcon(PASS_FONT, hotbarSlotRect(i), &inv->slots[i],
		             inv->selected_hotbar == i, ui->picked_slot == i, block_icons);

	if (furnace_open) {
		for (int i = 0; i < INV_MAIN_SLOTS; i++) {
			const int slot = INV_HOTBAR_SLOTS + i;
			drawSlotIcon(PASS_FONT, furnGridSlotRect(i), &inv->slots[slot],
			             false, ui->picked_slot == slot, block_icons);
		}
		drawFurnacePanelFont(furnace, inv, ui->picked_slot, block_icons);
	} else if (overlay_open) {
		for (int i = 0; i < INV_MAIN_SLOTS; i++) {
			const int slot = INV_HOTBAR_SLOTS + i;
			drawSlotIcon(PASS_FONT, gridSlotRect(i), &inv->slots[slot],
			             false, ui->picked_slot == slot, block_icons);
		}
		drawCraftPanelFont(inv);
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

		if (furnace_open) {
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
		} else if (overlay_open) {
			for (int i = 0; i < INV_MAIN_SLOTS; i++) {
				const int slot = INV_HOTBAR_SLOTS + i;
				drawSlotIcon(PASS_ATLAS, gridSlotRect(i), &inv->slots[slot], false, false,
				             block_icons);
			}
		}
	}

	spriteEnd();

	UiResult out;
	out.inventory_open = overlay_open;
	out.furnace_open   = furnace_open;
	return out;
}
