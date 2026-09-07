// Step 8.2 (touchscreen UI half) — the pure layout/hit-test arithmetic pulled out of
// scene/ui.c so it can be host-tested. scene/ui.c includes <citro3d.h> for its C3D_Tex*
// draw parameter, which makes the whole file unlinkable outside a devkitARM build; nothing
// in *this* pair of files needs a texture, a render target, or any libctru/citro3d symbol
// at all — it is exactly the rectangle math ui.c's own file comment already calls out as
// "pure integer arithmetic, no GPU state and no I/O" on hitInventorySlot, generalised to
// every rect this screen lays out, not just that one function.
//
// ui.c is the only caller. It includes this header and calls into these functions instead
// of holding its own copies of the rects or the constants below — see this file's own
// comment on why the numbers have to stay byte-identical to what shipped before this split
// (tools/run_host_tests.sh's new ui_layout_test binary is what proves that, not this file).
//
// No <3ds.h>, no <citro3d.h>, no gfx/*.h — only <stdbool.h>/<stdint.h> and three data
// headers (world/inventory.h for INV_HOTBAR_SLOTS/INV_MAIN_COLS/INV_MAIN_ROWS, world/
// crafting.h for RECIPE_COUNT, which CRAFT_ROW_H below divides the crafting panel's height
// by, and — since v1.9.0 CHEST — world/chest.h for CHEST_SLOTS, which the chest panel
// block below divides the bottom screen's width by the same way). All three are already
// pure C themselves — see their own file comments — so pulling them in here does not
// reintroduce the console dependency this split exists to remove.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/chest.h"      // CHEST_SLOTS — see the chest panel block below
#include "world/crafting.h"
#include "world/inventory.h"

// ── Layout constants ──────────────────────────────────────────────────────────────────
//
// Copied verbatim from scene/ui.c's own layout block — see that file for the reasoning
// behind every one of these numbers (the 320x240 panel, the 5.216 px/mm figure, the
// hotbar/grid/crafting vertical budget). This header does not re-derive or re-justify any
// of it; it is the same arithmetic, just made reachable without <citro3d.h>.
#define SCR_W 320
#define SCR_H 240

// One cell, shared by the hotbar and the main grid: SCR_W / INV_HOTBAR_SLOTS = 320/8 = 40 px.
#define SLOT_PX (SCR_W / INV_HOTBAR_SLOTS)

#define HOTBAR_Y 0
#define HOTBAR_H SLOT_PX                          // 40 px

#define GRID_Y   (HOTBAR_Y + HOTBAR_H)             // 40
#define GRID_H   (INV_MAIN_ROWS * SLOT_PX)         // 80 (2 rows of 40)

#define CRAFT_Y  (GRID_Y + GRID_H)                 // 120
#define CRAFT_H  (SCR_H - CRAFT_Y)                 // 120

#define CRAFT_CLOSE_H 28
#define CRAFT_ROW_Y0  (CRAFT_Y + CRAFT_CLOSE_H)              // 148
// Divides the panel evenly among however many recipes crafting.h declares, so adding one
// costs no layout edit here. 30 px a row at RECIPE_COUNT 3, 23 at 4, and 18 at 5.
// The floor is the 7 px font (gfx/font.h FONT_GLYPH_H) plus enough slop to stay a usable
// touch target.
//
// 2026-09-03, v1.8.12's RECIPE_COAL_ORE_TO_TORCH: two claims that were true at 4 recipes are
// no longer true at 5, and both are worth correcting rather than deleting.
//
// "92 divides exactly by 4, leaving no dead strip" — it does not divide by 5. 92/5 = 18 with
// 2 px left over, so there IS now a 2 px dead strip at the bottom of the panel. Two pixels
// below the last row is cosmetic, not a hit-testing bug: craftRowRect() derives every row from
// CRAFT_ROW_Y0 + i * CRAFT_ROW_H, so the rows stay contiguous and correctly sized and the
// leftover simply sits under the last one, inside the panel and outside every row. Nothing
// lands in it and nothing is clipped by it.
//
// "room for two or three more recipes" — that estimate was made against a 23 px row and it
// has now spent one of its own headroom steps. At 6 recipes this is 15 px, at 7 it is 13, and
// 13 px for a 7 px font is a row with 3 px of clearance above and below the glyphs. So the
// real remaining budget is ONE more recipe at a comfortable size and two at a cramped one,
// after which this has to become a scroll list rather than a fixed split. Written as a number
// rather than as "two or three more" because the previous phrasing is what let a fifth recipe
// land without anyone noticing the row shrank by 5 px.
//
// NOT verified on hardware: whether an 18 px row is comfortable to hit with a stylus on a real
// bottom screen, and whether the longest recipe label still fits. Both are playtest questions,
// and neither is settled by the layout suite, which checks that the rects tile correctly — not
// that a finger can hit them.
#define CRAFT_ROW_H   ((CRAFT_H - CRAFT_CLOSE_H) / RECIPE_COUNT)   // (120-28)/5 = 18

#define HUD_TOGGLE_Y  GRID_Y                       // 40, directly under the hotbar
#define HUD_TOGGLE_H  32
#define HUD_STATUS_Y0 (HUD_TOGGLE_Y + HUD_TOGGLE_H + 6)   // 78

// The vertical extent the HUD screen's status text can reach, as a constant, so the vitals
// strip below can be checked against it instead of against a number somebody remembered.
//
// ui.c's drawHudFont draws its rows at HUD_STATUS_Y0 + k * 12 and there are at most seven of
// them (cols/chunks, meshes/tris/cull, blocks/peak, `status`, `net`, biome, and the optional
// `timing` row), after which it adds a 2 px gap and draws the "tap a hotbar slot to select it"
// hint. Worst case — every optional row present — the hint sits at 164 and its glyphs end at
// 171 (FONT_GLYPH_H is 7). HUD_STATUS_BOTTOM budgets one whole extra row on top of that, so it
// is 174: a deliberate over-estimate, because the point of the constant is to be a floor the
// vitals strip stays below, not a tight measurement.
//
// Deliberately NOT wired into drawHudFont's own `y += 12` steps. Those literals are what
// shipped, three separate comments in that function assert the rows above them are unchanged
// byte-for-byte, and rewriting them to derive from here would be a re-layout of a panel this
// change is only appending to. This constant exists to be *checked against* (see
// ui_layout_test.c's testPipsClearTheStatusRows), and it goes red if either side moves.
#define HUD_STATUS_STEP     12
#define HUD_STATUS_MAX_ROWS  7
#define HUD_STATUS_BOTTOM   (HUD_STATUS_Y0 + (HUD_STATUS_MAX_ROWS + 1) * HUD_STATUS_STEP)  // 174

// ── Vitals strip (health + hunger pips) ────────────────────────────────────────────────
//
// Where these can actually go on a 320x240 bottom screen, which is the whole reason they are
// down here at y=200 and not tucked under the hotbar where a first sketch put them:
//
//   HUD screen (overlay closed)      hotbar        y   0..40   (HOTBAR_Y/HOTBAR_H)
//                                    OPEN INVENTORY y  40..72   (HUD_TOGGLE_Y/_H)
//                                    status text    y  78..171  (see HUD_STATUS_BOTTOM)
//                                    FREE           y 172..240
//   INVENTORY screen (overlay open)  hotbar        y   0..40
//                                    main grid      y  40..120  (GRID_Y/GRID_H)
//                                    crafting panel y 120..240  (CRAFT_Y/CRAFT_H)
//                                    FREE           nothing
//
// So the only unoccupied band on the whole panel is the bottom ~68 px of the HUD screen, and
// it is unoccupied *only* on that screen. Anything at y=2/y=10 lands inside the 40 px hotbar
// band; anything between 40 and 72 lands on the inventory toggle; anything from 78 down to
// ~171 lands on the debug/status rows. Nothing moved to make room — HOTBAR_Y, HUD_TOGGLE_Y and
// HUD_STATUS_Y0 are all byte-identical to what shipped — because nothing needed to.
//
// The consequence, and it is a real one rather than an oversight: ui.c draws this strip from
// drawHudFont only, so the vitals are visible on the HUD and not while the inventory overlay
// is open. On the overlay screen every pixel from y=40 down belongs to the grid or the
// crafting panel, and a vitals row there would have to sit on top of a crafting row.
//
// HUD_PIPS_Y0 is written as SCR_H - 40 rather than as 200 so it stays pinned to the bottom of
// the panel if SCR_H ever changes, which is the edge it is actually anchored to. The two rows
// then occupy y 200..208 (health) and 212..220 (hunger), leaving a 20 px margin under them.
#define HUD_PIP_COUNT      10   // pips per bar
#define HUD_STAT_MAX       20   // points a full bar represents, so 2 points per pip
#define HUD_PIP_W          12
#define HUD_PIP_H           8
#define HUD_PIP_GAP         3
#define HUD_PIP_X0         32   // left of this is the 2-char row label, drawn by ui.c at x=6
#define HUD_PIP_ROW_STEP   12
#define HUD_PIP_ROWS        2
#define HUD_PIP_ROW_HEALTH  0
#define HUD_PIP_ROW_HUNGER  1
#define HUD_PIPS_Y0        (SCR_H - 40)   // 200

// ── Furnace panel (v1.8.15 FURNACE) ────────────────────────────────────────────────────
//
// A THIRD screen (ui.h's UI_SCR_FURNACE), not a fourth region bolted onto the inventory
// overlay. The overlay's own 240 px are fully spoken for — hotbar 0..40, main grid 40..120,
// crafting panel 120..240, with the crafting panel already down to an 18 px row and one
// recipe of headroom left (see CRAFT_ROW_H's own note above). There is no band on that
// screen a furnace's three slots and two indicators could take without evicting the
// crafting list, so the furnace gets its own screen and reuses the vertical budget from
// scratch.
//
// ── Why the main grid MOVES on this screen ─────────────────────────────────────────────
//
// The player has to be able to feed the furnace out of their whole bag, not just the eight
// hotbar cells — a furnace you can only load from the hotbar means staging every raw
// porkchop through the inventory screen first, which is two screen switches per smelt. So
// this screen shows the hotbar (kept at y 0..40, the one fixed point: ui.h's own rule is
// that the player must always see what they are holding), the furnace itself, AND the 8x2
// main grid.
//
// The grid cannot stay at GRID_Y (40) here: that band is where the furnace's own slots have
// to go, directly under the close bar, or the furnace ends up below the fold. So this screen
// draws the grid at FURN_GRID_Y (160) instead, through furnGridSlotRect() rather than
// gridSlotRect(). gridSlotRect() and GRID_Y are NOT touched — the inventory overlay is
// byte-identical to what shipped — and the two grids are separate functions rather than one
// function taking a y, because a single "which screen am I on" parameter threaded through
// gridSlotRect() would put the furnace's existence inside the inventory screen's own hit
// test, which is exactly the coupling hitInventorySlot()'s `overlay_open` flag already shows
// gets confusing at two callers, let alone three.
//
// ── The vertical budget, in full ───────────────────────────────────────────────────────
//
//   hotbar          y   0..40    HOTBAR_Y / HOTBAR_H, unchanged and always visible
//   close bar       y  40..66    FURN_CLOSE_Y / _H — 26 px, comfortably over the 7 px font
//   slot row        y  72..112   input, fuel and output, all three at the full SLOT_PX (40)
//   burn bar        y 116..130   under the FUEL slot, because it reads how much of THAT
//                                 item's burn time is left
//   hint line       y 136..143   one 7 px row of text
//   main grid       y 160..240   FURN_GRID_Y, 2 rows of 40 — lands exactly on SCR_H
//
// Every touch target on this screen is at least 26 px tall (the close bar) and most are 40,
// against a 7 px font — the floor the task set. The burn bar and the cook arrow are
// deliberately NOT touch targets at all: they are readouts, they sit in the gaps, and
// hitFurnaceSlot() below returns FURN_HIT_NONE for a point on either.
#define FURN_CLOSE_Y  GRID_Y            // 40 — directly under the hotbar, like HUD_TOGGLE_Y
#define FURN_CLOSE_H  26

#define FURN_SLOT_PX  SLOT_PX           // 40 — same cell size as every other slot on the panel
#define FURN_ROW_Y    72                // top of the three-slot row

// The three slots' x origins. 20 / 76 / 228 rather than an even split of 320: input and fuel
// are a PAIR (what you put in) and are placed close together on the left, the output is the
// result and sits alone on the right, with the cook arrow spanning the gap between them so
// the panel reads left-to-right as "these two become that one". An even three-way split would
// put the same 16 px between input/fuel as between fuel/output and lose that grouping.
#define FURN_INPUT_X   20               // 20..60
#define FURN_FUEL_X    76               // 76..116
#define FURN_OUTPUT_X 228               // 228..268, 52 px clear of the right edge

// Cook progress: a horizontal bar in the gap between the fuel slot and the output slot,
// vertically centred on the slot row.
#define FURN_ARROW_X 132                // 132..212, 16 px clear of the fuel slot on the left
#define FURN_ARROW_W  80
#define FURN_ARROW_H  12
#define FURN_ARROW_Y (FURN_ROW_Y + (FURN_SLOT_PX - FURN_ARROW_H) / 2)   // 86

// Burn time: a horizontal bar directly under the fuel slot and exactly as wide as it, so the
// association is positional and needs no label. Under the FUEL slot and not between input and
// fuel (which is where a Java furnace puts its flame) because there are only 4 px between
// those two cells on this panel and a 4 px indicator is not a readout, it is a smudge.
#define FURN_BURN_X FURN_FUEL_X
#define FURN_BURN_Y (FURN_ROW_Y + FURN_SLOT_PX + 4)   // 116
#define FURN_BURN_W FURN_SLOT_PX                       // 40
#define FURN_BURN_H 14

// Two 7 px text rows telling the player what each slot takes and how the gesture works. A
// 12 px step, the same one the HUD's own status rows use (HUD_STATUS_STEP), so the two blocks
// of small text on this UI are set the same way. The last row's glyphs end at
// FURN_HINT_Y + (FURN_HINT_ROWS-1)*FURN_HINT_STEP + FONT_GLYPH_H = 155, which must stay above
// FURN_GRID_Y — checked in ui_layout_test.c rather than left to this arithmetic being reread
// correctly the next time a row is added.
#define FURN_HINT_Y    (FURN_BURN_Y + FURN_BURN_H + 6)   // 136
#define FURN_HINT_STEP 12
#define FURN_HINT_ROWS 2

// The relocated main grid — see "Why the main grid MOVES" above. 160 + 2*40 == 240 == SCR_H,
// so the grid ends exactly on the bottom edge with no dead strip.
#define FURN_GRID_Y 160

// Which furnace slot a touch landed in. Values are 0/1/2 (not a bitmask and not the ItemId
// space) so hitFurnaceSlot() can return -1 for "none" the same way hitInventorySlot() does,
// and so ui.c's dispatch is one switch rather than three ptInRect calls it could get out of
// order. Not an enum in the FurnaceState sense — this is a UI coordinate, and world/furnace.h
// deliberately carries no enum at all (see its ARM EABI -fshort-enums note).
#define FURN_HIT_NONE   (-1)
#define FURN_HIT_INPUT    0
#define FURN_HIT_FUEL     1
#define FURN_HIT_OUTPUT   2

// ── Chest panel (v1.9.0 CHEST) ─────────────────────────────────────────────────────────
//
// A THIRD storage screen, same reason UI_SCR_FURNACE got one of its own instead of a
// fourth region on the inventory overlay: that overlay's 240px are already fully spoken
// for (see the furnace block above), so a chest reuses the vertical budget from scratch
// the same way the furnace does, and reuses FURN_GRID_Y's own relocated-grid trick outright
// rather than picking a new number for it — CHEST_GRID_Y below is byte-identical to
// FURN_GRID_Y on purpose, so the main grid lands in the same place on either storage
// screen and a player does not have to relearn where their bag is depending on which
// container they opened.
//
// ── Why one row of eight, and not the furnace's three fixed x's ───────────────────────
//
// The furnace's input/fuel/output sit at hand-picked x's because the three slots MEAN
// different things (what goes in, what burns, what comes out — see FURN_INPUT_X's own
// comment on why an even split would lose that grouping). A chest slot means nothing
// different from any other chest slot — world/chest.h's own file comment is explicit that
// every slot accepts every item and nothing distinguishes slot 3 from slot 5 except which
// one was tapped — so there is no grouping to preserve and an even split is not a
// simplification of a real layout, it is the honestly correct one. CHEST_SLOTS (8) times
// SLOT_PX (40) is exactly SCR_W (320), so the row needs no gutter math at all: it is the
// hotbar's own row, shape for shape, just lower on the screen.
//
// ── The vertical budget ─────────────────────────────────────────────────────────────────
//
//   hotbar          y   0..40    HOTBAR_Y / HOTBAR_H, unchanged and always visible
//   close bar       y  40..66    CHEST_CLOSE_Y / _H — same 26px as the furnace's own
//   chest slot row  y  72..112   all eight slots, full SLOT_PX (40), full SCR_W width
//   hint lines      y 118..137   two 7px rows of text, same 12px step as everywhere else
//   main grid       y 160..240   CHEST_GRID_Y, 2 rows of 40 — same position as the furnace
//                                 screen's own relocated grid (FURN_GRID_Y)
//
// The 23px gap between the last hint glyph (137) and the grid (160) is deliberate slack,
// not a measured floor the way FURN_HINT_Y's own comment checks one — there is no third
// hint row here that could grow into it, so nothing currently depends on the gap being
// exactly that size.
#define CHEST_CLOSE_Y  GRID_Y            // 40 — directly under the hotbar, like FURN_CLOSE_Y
#define CHEST_CLOSE_H  26                // same close-bar height as the furnace panel

#define CHEST_SLOT_PX  SLOT_PX           // 40 — same cell size as every other slot on the panel
#define CHEST_ROW_Y    72                // top of the chest's one-row-of-eight, same offset
                                          // under the close bar as the furnace's FURN_ROW_Y

#define CHEST_HINT_Y    (CHEST_ROW_Y + CHEST_SLOT_PX + 6)   // 118
#define CHEST_HINT_STEP 12
#define CHEST_HINT_ROWS 2

// The relocated main grid — see the vertical budget above. Byte-identical to FURN_GRID_Y;
// see this block's own file comment for why that is intentional rather than coincidental.
#define CHEST_GRID_Y 160

// ── Rects ──────────────────────────────────────────────────────────────────────────────

typedef struct { int x, y, w, h; } URect;

bool ptInRect(URect r, int x, int y);

// Pixel rect for hotbar slot `i` (0..INV_HOTBAR_SLOTS-1).
URect hotbarSlotRect(int i);

// Pixel rect for main-grid slot `i` (0..INV_MAIN_SLOTS-1 — an offset from
// INV_HOTBAR_SLOTS, not an inventory.h slot index itself).
URect gridSlotRect(int i);

URect craftCloseRect(void);
URect craftRowRect(int i);
URect hudToggleRect(void);

// The one function that turns a raw touch point into "which inventory.h slot (if any) was
// hit", covering both strips at once. See ui.c's original file comment (now here) for the
// contract: (a) a point inside hotbar cell i always returns i regardless of `overlay_open`,
// (b) a point inside main-grid cell i returns INV_HOTBAR_SLOTS + i only when `overlay_open`
// is true and returns -1 for the same point when it is false, and (c) a point in the gutter
// between cells, or below the grid when the overlay is closed, returns -1.
int hitInventorySlot(int x, int y, bool overlay_open);

// ── Furnace rects and hit tests (v1.8.15 FURNACE) ──────────────────────────────────────

URect furnCloseRect(void);
URect furnInputRect(void);
URect furnFuelRect(void);
URect furnOutputRect(void);

// The same three rects addressed by their FURN_HIT_* index, so a caller that already has one
// (from hitFurnaceSlot, or from looping over the three) does not have to re-map it back to a
// named function with a switch of its own. An index outside the three returns a zero rect,
// which ptInRect() can never match — the same "cannot be mistaken for a real answer" shape
// hudPipFill() uses for an out-of-range pip.
URect furnSlotRect(int which);

URect furnBurnRect(void);    // readout, not a touch target — see hitFurnaceSlot
URect furnArrowRect(void);   // readout, not a touch target — see hitFurnaceSlot

// Pixel rect for main-grid slot `i` (0..INV_MAIN_SLOTS-1) AS DRAWN ON THE FURNACE SCREEN.
// Same columns and same cell size as gridSlotRect(), FURN_GRID_Y instead of GRID_Y — see the
// furnace layout block above for why the grid moves on this one screen and why this is a
// second function rather than a parameter on the first.
URect furnGridSlotRect(int i);

// Which of the three furnace slots (FURN_HIT_INPUT/_FUEL/_OUTPUT) a point lands in, or
// FURN_HIT_NONE. The close bar, the burn bar, the cook arrow, the hotbar and the relocated
// main grid all answer FURN_HIT_NONE here — this function knows about three rects and
// nothing else, so ui.c's dispatch order (close bar, then furnace slots, then inventory
// slots) is stated once in ui.c and not half-encoded in here.
int hitFurnaceSlot(int x, int y);

// The inventory-side hit test for the furnace screen: the hotbar (slots 0..INV_HOTBAR_SLOTS-1,
// at their usual place) plus the RELOCATED main grid (INV_HOTBAR_SLOTS + i, at FURN_GRID_Y).
// Returns a world/inventory.h slot index, or -1.
//
// A separate function from hitInventorySlot() rather than a third mode on its `overlay_open`
// bool: the two screens genuinely disagree about where the grid is, and a function whose
// answer depends on a flag the caller has to get right is how the same point ends up meaning
// two different slots. Both are checked against each other in ui_layout_test.c — a point in
// the furnace screen's grid must miss hitInventorySlot() entirely, and vice versa.
int hitFurnaceInvSlot(int x, int y);

// ── Chest rects and hit tests (v1.9.0 CHEST) ───────────────────────────────────────────

URect chestCloseRect(void);

// Pixel rect for chest slot `i` (0..CHEST_SLOTS-1). One row across the full screen width —
// see the chest panel block above for why there is no per-slot x table the way the
// furnace's three slots have.
URect chestSlotRect(int i);

// Pixel rect for main-grid slot `i` (0..INV_MAIN_SLOTS-1) AS DRAWN ON THE CHEST SCREEN.
// Same columns and cell size as gridSlotRect()/furnGridSlotRect(), CHEST_GRID_Y instead of
// GRID_Y/FURN_GRID_Y — a separate function rather than a parameter on either existing one,
// for the identical reason furnGridSlotRect() is its own function and not a mode on
// gridSlotRect(): see that function's own comment.
URect chestGridSlotRect(int i);

// Which chest slot (0..CHEST_SLOTS-1) a point lands in, or -1. The close bar, the hotbar
// and the relocated main grid all answer -1 here, the same "this function knows about its
// own rects and nothing else" shape hitFurnaceSlot() documents for itself.
int hitChestSlot(int x, int y);

// The inventory-side hit test for the chest screen: the hotbar (at its usual place) plus
// the RELOCATED main grid (at CHEST_GRID_Y). Returns a world/inventory.h slot index, or -1.
// A separate function from hitInventorySlot() and from hitFurnaceInvSlot(), for the same
// reason hitFurnaceInvSlot() is not a third mode on hitInventorySlot()'s own flag: see that
// function's own comment.
int hitChestInvSlot(int x, int y);

// How many pixels of a `total_px`-wide bar are filled at progress `num`/`den`.
//
// Pulled out here, rather than written inline in ui.c's two indicator draws, because it is
// the one piece of furnace-panel arithmetic that can be WRONG rather than merely ugly, and
// because one of its inputs is a divisor that is legitimately zero: a furnace holding no fuel
// has fuel_ticks_left == 0 AND no current fuel item to size the bar against, and an input with
// no recipe has no cook_ticks to divide by. That is not a rare corner — it is what a freshly
// placed furnace looks like, and what every furnace looks like between smelts.
//
// Guaranteed to return a value in [0, total_px] for EVERY input, including negative and absurd
// ones: den <= 0 and num <= 0 both give 0, num >= den gives total_px, total_px <= 0 gives 0.
// Nothing here trusts its caller, because the caller is reading a uint16_t out of a packed save
// payload that a corrupt byte could have made nonsense.
//
// The den <= 0 case answers 0 — an EMPTY bar — and ui_layout.c's own comment on that line
// records what a sabotage run measured about it: it is not what stops a divide-by-zero (the
// other guards already do), it is what stops an idle furnace drawing a FULL gauge.
int furnBarFill(int total_px, int num, int den);

// ── Moving a stack into a slot that is not an inventory slot ───────────────────────────
//
// Moves as much of `src`'s stack as `dst_item`/`dst_count` will take, and returns how many
// units actually moved (0 if none did). `src` is an ordinary inventory slot; the destination
// is an ARBITRARY (item, count) pair — in practice one of a FurnaceState's three, which are
// not inventory slots, have no slot index, and so cannot go through world/inventory.h's own
// inventoryMoveUnits().
//
// It lives HERE rather than in scene/ui.c for exactly the reason the rest of this file does:
// this is the merge/refuse/cap arithmetic, it is the part of the furnace panel most able to be
// subtly wrong (an off-by-one on the room calculation is a duplicated or a vanished item, and
// neither shows up in a screenshot), and ui.c cannot be linked outside a devkitARM build. It
// needs no furnace.h, no inventory.c and no net/ — only InvSlot, ItemId and INV_STACK_MAX,
// all of which world/inventory.h already gives this header as plain declarations. So it costs
// this binary's link nothing.
//
// The rules, all of which are checked in ui_layout_test.c:
//   * an empty `src` (ITEM_NONE, or a count of 0) moves nothing
//   * a destination holding a DIFFERENT item moves nothing — deliberately a refusal and not a
//     swap; see scene/ui.c's furnaceDeposit for why a furnace slot must not be swapped
//   * a destination at INV_STACK_MAX moves nothing
//   * otherwise it tops the destination up to at most INV_STACK_MAX and leaves the remainder
//     in `src`, the same partial-move behaviour inventoryMoveUnits() has
//   * an emptied `src` is left as { ITEM_NONE, 0 }, never a stale id with a zero count, which
//     is the invariant world/inventory.h's InvSlot comment states
//   * a destination whose item is ITEM_NONE is treated as holding NOTHING regardless of its
//     count. A furnace slot is unpacked from a save payload byte-by-byte (world/furnace.c),
//     so a corrupt byte really can present ITEM_NONE with a non-zero count; deriving the room
//     from the item rather than the count means that costs one overwritten stale number and
//     never an overflowed uint8_t.
uint8_t uiMoveStackInto(InvSlot* src, ItemId* dst_item, uint8_t* dst_count);

// ── Vitals pip arithmetic ──────────────────────────────────────────────────────────────
//
// `static inline` in the header, and not a pair of functions in ui_layout.c, because
// ui_layout.c is not this change's to edit — see the lane scope this landed under. The
// consequence that matters is that scene/ui.c and scene/ui_layout_test.c compile the *same*
// definitions rather than two hand-kept copies, which is the only property the test needs to
// be worth running: a test that re-implements the rule it is checking proves nothing about
// the code that ships.

// How full pip `i` of a bar is. `points` is the 0..HUD_STAT_MAX value (health or hunger) and
// each pip is worth 2 points, so pip i covers points [2i, 2i+2).
//
// Out-of-range inputs are safe, which matters because UiStats.health is filled in by another
// module and a 0..20 contract written down only in a comment is one off-by-one from drawing
// nonsense. `i` is guarded explicitly — an empty pip is the only answer that cannot be
// mistaken for a real reading — but `points` needs no clamp: the subtraction below already
// saturates, since any value above HUD_STAT_MAX leaves rem >= 2 for all ten pips (a full bar)
// and any value below 0 leaves rem <= 0 for all ten (an empty one).
//
// That is stated as a fact because it was measured, not assumed. An explicit
// `if (points > HUD_STAT_MAX) points = HUD_STAT_MAX;` was written here first and then deleted:
// a sabotage run that removed it left ui_layout_test.c at PASS 703/703, which is the proof
// that the line could never change an answer. Dead code no check can catch is worse than no
// code, so it is gone; the saturating behaviour it was guarding is still checked, by
// testPipFillClampsOutOfRangeValues.
typedef enum {
	PIP_EMPTY = 0,
	PIP_HALF  = 1,   // an odd `points` leaves exactly one pip in this state
	PIP_FULL  = 2,
} PipFill;

static inline PipFill hudPipFill(int points, int i)
{
	if (i < 0 || i >= HUD_PIP_COUNT) return PIP_EMPTY;

	const int rem = points - 2 * i;
	if (rem >= 2) return PIP_FULL;
	if (rem == 1) return PIP_HALF;
	return PIP_EMPTY;
}

// Pixel rect for pip `i` of row `row` (HUD_PIP_ROW_HEALTH / HUD_PIP_ROW_HUNGER).
static inline URect hudPipRect(int row, int i)
{
	URect r = { HUD_PIP_X0 + i * (HUD_PIP_W + HUD_PIP_GAP),
	            HUD_PIPS_Y0 + row * HUD_PIP_ROW_STEP,
	            HUD_PIP_W, HUD_PIP_H };
	return r;
}
