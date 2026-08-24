#include "scene/ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "net/inv_bridge.h"
#include "scene/ui_layout.h"
#include "world/block.h"
#include "world/crafting.h"

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
	y += 14;
	fontDraw(6, y, 1, COL_TEXT_DIM, "tap a hotbar slot to select it");
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

// ── Entry points ───────────────────────────────────────────────────────────────────────

void uiInit(UiState* ui)
{
	memset(ui, 0, sizeof(*ui));
	ui->screen = UI_SCR_HUD;
	ui->picked_slot = -1;
}

UiResult uiUpdateDraw(UiState* ui, Inventory* inv, C3D_Tex* block_icons,
                       const UiStats* stats, const UiInput* in)
{
	// Rising edge only, same as title.c's own `tap` — see title.h's TitleInput comment for
	// why (KEY_TOUCH is never actually set by hidScanInput, so the caller derives
	// touch_down some other way, and a tap should still fire once per press regardless).
	const bool tap = in->touch_down && !ui->touch_prev;
	ui->touch_prev = in->touch_down;

	// Captured once and used for both the tap logic and the draw below, so a tap is always
	// interpreted against the screen the player actually saw this frame — any screen switch
	// a handler makes below (opening or closing the overlay) takes effect next frame, the
	// same "change applies next frame" rule title.c's own ts->screen follows.
	const bool overlay_open = (ui->screen == UI_SCR_INVENTORY);

	if (tap) {
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
	// regardless of which of the two screens is showing — one texture, one flush, one draw
	// call, batched exactly the way gfx/sprite.h's own file comment describes a whole UI
	// screen should be.
	spriteTexture(fontTexture());
	spriteRect(0, 0, SCR_W, SCR_H, COL_BG);

	for (int i = 0; i < INV_HOTBAR_SLOTS; i++)
		drawSlotIcon(PASS_FONT, hotbarSlotRect(i), &inv->slots[i],
		             inv->selected_hotbar == i, ui->picked_slot == i, block_icons);

	if (overlay_open) {
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

		if (overlay_open) {
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
	return out;
}
