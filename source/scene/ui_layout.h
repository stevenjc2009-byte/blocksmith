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
// crafting.h for RECIPE_COUNT, and — since v1.9.0 CHEST — world/chest.h for CHEST_SLOTS,
// which the chest panel block below divides the bottom screen's width by). All three are
// already pure C themselves — see their own file comments — so pulling them in here does
// not reintroduce the console dependency this split exists to remove.
//
// world/crafting.h is KEPT as an include even though v1.9.1's bar retired the one macro that
// used it (CRAFT_ROW_H divided the crafting panel's height by RECIPE_COUNT; the recipe list is
// now a scrolling list whose length its caller passes in as `list_len`). Retained deliberately
// rather than tidied away: scene/ui.c reaches RECIPE_COUNT through this header, and dropping
// the include here would remove that symbol from ui.c's translation unit as a side effect of a
// layout change — a compile break in a file this one does not own, for no gain.
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

// ── The bar strip (v1.9.1 INTERFACE) ───────────────────────────────────────────────────
//
// One band under the hotbar, on every bottom-screen surface, carrying the category tabs and
// the close tab. It replaces three separate screens' three separate close bars (the inventory
// overlay's crafting-panel close strip, FURN_CLOSE_*, CHEST_CLOSE_*) with the same 26 px of
// screen those bars already used, so the band under the hotbar is the SAME band whether the
// bar is open or shut and nothing under it has to move when a container tab appears.
//
// 26 px, not 22 and not 32: 26 is exactly what the furnace and chest close bars already
// shipped at, so FURN_ROW_Y / CHEST_ROW_Y (72) keep the 6 px gap they were laid out with and
// those two panels are byte-identical below the strip. Picking any other height would have
// re-laid out two shipped screens to save nothing.
//
// The horizontal split is 294 + 26, not an even share of 320. The close tab is a fixed 26 px
// square at the right edge — a target whose size must not shrink as tabs are added, because it
// is the stylus door out of the bar and the one control that is in the same place on every
// screen. Everything left of it is tab space, split evenly among however many categories are
// live. At the widest (2 tabs) that is 147 px and at the narrowest this version can produce
// (3 tabs) it is 98 px, against a 54 px "INVENTORY" at font scale 1.
//
// BAR_HIT_NONE and BAR_HIT_CLOSE are negative for the reason FURN_HIT_NONE is: a tab index is
// 0-based and a caller that forgets to test the result reads a miss as tab 0. Two distinct
// negatives rather than one, because "the player tapped nothing" and "the player tapped close"
// are different actions and collapsing them would close the bar on a tap in the gutter.
#define BAR_STRIP_Y   40                      // directly under the hotbar
#define BAR_STRIP_H   26
#define BAR_CLOSE_W   26                      // the close tab, square, at the right edge
#define BAR_TABS_W    (SCR_W - BAR_CLOSE_W)   // 294 — the span the category tabs tile
#define BAR_MAX_TABS  4                       // INVENTORY, CRAFT, one container, one spare

#define BAR_HIT_NONE  (-1)
#define BAR_HIT_CLOSE (-2)

// The grid moved down by the strip's height plus the same 6 px gap the furnace and chest rows
// already sit at, so the bag lands at 72 on EVERY tab rather than at 40 on one and 72 on the
// others. 72 + 80 = 152, which leaves the detail band below it (BAR_DETAIL_Y) clear.
#define GRID_Y   (BAR_STRIP_Y + BAR_STRIP_H + 6)   // 72
#define GRID_H   (INV_MAIN_ROWS * SLOT_PX)         // 80 (2 rows of 40) — the grid is 72..152

// ── The bar's content bands (v1.9.1 INTERFACE) ──────────────────────────────────────────
//
// ── What was here before, and why it is gone ────────────────────────────────────────────
//
// CRAFT_Y / CRAFT_H / CRAFT_CLOSE_H / CRAFT_ROW_Y0 / CRAFT_ROW_H and their two rect
// constructors (craftCloseRect, craftRowRect) are RETIRED. They split a fixed 120 px panel
// evenly among RECIPE_COUNT recipes, and CRAFT_ROW_H's own comment set the floor that ended
// them: "at 6 recipes this is 15 px, at 7 it is 13, ... after which this has to become a
// scroll list rather than a fixed split." RECIPE_COUNT reached 7. The rows were 13 px for a
// 7 px font, which is not a touch target, and the panel had no headroom left at all.
//
// So the recipe list is a LIST: fixed 24 px rows in a window that scrolls, rather than a fixed
// band divided by a count that keeps growing. The row height stops depending on how many
// recipes exist, which is the property the old split could never have.
//
// BAR_LIST_VISIBLE is derived, not written as 7, so it stays correct if the band moves — it is
// how many whole rows fit between the top of the content band and the bottom of the screen,
// and it is what a caller compares its recipe count against to decide whether to scroll at all.
//
// BAR_LIST_Y0 is written as the literal 72 rather than as GRID_Y so that the two are checked
// against each other in ui_layout_test.c instead of one silently following the other: the claim
// that every tab's content starts in the same band is a claim about the DESIGN, and a constant
// defined as another constant cannot state it.
#define BAR_LIST_Y0      72
#define BAR_LIST_ROW_H   24
#define BAR_LIST_VISIBLE ((SCR_H - BAR_LIST_Y0) / BAR_LIST_ROW_H)   // 7

// The INVENTORY tab's detail band: the focused or lifted item's name and count, drawn under
// the bag grid (which ends at 152) and above the vitals pips (which start at HUD_PIPS_Y0, 200).
//
// Deliberately the same y as FURN_GRID_Y / CHEST_GRID_Y. Those are the RELOCATED bag on the two
// container tabs; this is the detail readout on the inventory tab. They are never on screen at
// the same time, so sharing the band is not a collision — it is the same 80 px of screen doing
// the one job each tab has for it, which is why ui_layout_test.c pins them equal rather than
// pinning them disjoint.
#define BAR_DETAIL_Y 160

// The HUD's "open the bar" strip is the SAME band as the bar's own strip, at the same y and
// the same height, so pressing it does not make anything under it jump. That is the whole
// point of shrinking it from 32 to 26: at 32 the closed HUD's band was 40..72 and the open
// bar's would have been 40..66, and a 6 px shift on every open/close is the kind of one-frame
// jolt pausemenu.c's own comment already names as reading like a glitch.
//
// NOT tracked to BAR_STRIP_Y by an alias on purpose — see BAR_LIST_Y0's comment for the same
// reasoning: ui_layout_test.c asserts hudToggleRect() and barStripRect() are the identical
// rect, which is a claim two aliases could not make.
#define HUD_TOGGLE_Y  40                           // directly under the hotbar
#define HUD_TOGGLE_H  26                           // was 32 before v1.9.1's bar
#define HUD_STATUS_Y0 (HUD_TOGGLE_Y + HUD_TOGGLE_H + 6)   // 72

// The vertical extent the HUD screen's status text can reach, as a constant, so the vitals
// strip below can be checked against it instead of against a number somebody remembered.
//
// ui.c's drawHudFont draws its rows at HUD_STATUS_Y0 + k * 12 and there are at most seven of
// them (cols/chunks, meshes/tris/cull, blocks/peak, `status`, `net`, biome, and the optional
// `timing` row), after which it adds a 2 px gap and draws the "tap a hotbar slot to select it"
// hint. Worst case — every optional row present — the hint sits at 158 and its glyphs end at
// 165 (FONT_GLYPH_H is 7). HUD_STATUS_BOTTOM budgets one whole extra row on top of that, so it
// is 168: a deliberate over-estimate, because the point of the constant is to be a floor the
// vitals strip stays below, not a tight measurement.
//
// v1.9.1's bar moved this band up 6 px as a consequence, not as a decision: HUD_STATUS_Y0 is
// "6 px under the toggle strip", the toggle strip shrank from 32 to 26 to match the bar's own
// strip, so the status rows follow it from 78 to 72 and this floor from 174 to 168. The gap
// between the strip and the first status row is unchanged; only the strip above it is shorter.
//
// Deliberately NOT wired into drawHudFont's own `y += 12` steps. Those literals are what
// shipped, three separate comments in that function assert the rows above them are unchanged
// byte-for-byte, and rewriting them to derive from here would be a re-layout of a panel this
// change is only appending to. This constant exists to be *checked against* (see
// ui_layout_test.c's testPipsClearTheStatusRows), and it goes red if either side moves.
#define HUD_STATUS_STEP     12
#define HUD_STATUS_MAX_ROWS  7
#define HUD_STATUS_BOTTOM   (HUD_STATUS_Y0 + (HUD_STATUS_MAX_ROWS + 1) * HUD_STATUS_STEP)  // 168

// ── Vitals strip (health + hunger pips) ────────────────────────────────────────────────
//
// Where these can actually go on a 320x240 bottom screen, which is the whole reason they are
// down here at y=200 and not tucked under the hotbar where a first sketch put them:
//
//   HUD screen (bar closed)          hotbar        y   0..40   (HOTBAR_Y/HOTBAR_H)
//                                    INVENTORY   B y  40..66   (HUD_TOGGLE_Y/_H)
//                                    status text    y  72..165  (see HUD_STATUS_BOTTOM)
//                                    FREE           y 166..240
//   BAR, INVENTORY tab (bar open)    hotbar        y   0..40
//                                    strip          y  40..66   (BAR_STRIP_Y/_H)
//                                    main grid      y  72..152  (GRID_Y/GRID_H)
//                                    detail band    y 160..200  (BAR_DETAIL_Y)
//                                    vitals         y 200..220  — see below
//
// The band this strip sits in is therefore free on the HUD and deliberately given to it on the
// bar's inventory tab as well: v1.9.1 draws the pips on BOTH, which is what "vitals visible
// here and on the HUD" in the interface blueprint's D5 means. Anything at y=2/y=10 lands inside
// the 40 px hotbar band; anything between 40 and 66 lands on the strip; anything from 72 down
// to ~165 lands on the debug/status rows on the HUD, or on the grid and detail band on the bar.
//
// Before v1.9.1 this said the vitals were visible on the HUD and NOT on the inventory overlay,
// because that overlay's every pixel from y=40 down belonged to the grid or the crafting panel.
// The crafting panel is retired (see BAR_LIST_Y0's block) and the band from 160 is now the
// detail readout, which ends where the pips begin — so the exclusion that comment recorded no
// longer holds and the strip is drawn on both surfaces.
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
// A THIRD screen, not a fourth region bolted onto the inventory overlay. The overlay's own
// 240 px were fully spoken for — hotbar 0..40, main grid 40..120, crafting panel 120..240,
// with the crafting panel already down to an 18 px row and one recipe of headroom left. That
// is the layout as it stood in v1.8.15, and it is why these rects exist; v1.9.1 turned the
// three screens into one bar's three tabs and everything below the strip here is unchanged
// (see the bar strip block above). There was no band on that screen a furnace's three slots
// and two indicators could take without evicting the crafting list, so the furnace got its
// own screen and reused the vertical budget from scratch.
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
// The grid cannot stay at GRID_Y here: that band is where the furnace's own slots have to go,
// directly under the close bar, or the furnace ends up below the fold. So this screen draws
// the grid at FURN_GRID_Y (160) instead, through furnGridSlotRect() rather than
// gridSlotRect(). (GRID_Y was 40 when this was written and is 72 since v1.9.1's strip; the
// two grids are still in different bands, which is all this paragraph ever claimed.) The two
// grids are separate functions rather than one
// function taking a y, because a single "which screen am I on" parameter threaded through
// gridSlotRect() would put the furnace's existence inside the inventory screen's own hit
// test, which is exactly the coupling hitInventorySlot()'s `overlay_open` flag already shows
// gets confusing at two callers, let alone three.
//
// ── The vertical budget, in full ───────────────────────────────────────────────────────
//
//   hotbar          y   0..40    HOTBAR_Y / HOTBAR_H, unchanged and always visible
//   close bar       y  40..66    FURN_CLOSE_Y / _H — 26 px, comfortably over the 7 px font
//                                 (since v1.9.1 this band is the bar's tab strip, same rect)
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
// Aliases of the bar strip since v1.9.1, and the same numbers they always were (40 and 26).
// They were written against GRID_Y, which has since moved to 72, so tracking the strip is what
// keeps them where they were rather than what moves them.
#define FURN_CLOSE_Y  BAR_STRIP_Y       // 40 — directly under the hotbar, like HUD_TOGGLE_Y
#define FURN_CLOSE_H  BAR_STRIP_H       // 26

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
// v1.9.1: the reasoning below is unchanged and the constants are unchanged, but the noun is.
// There is no longer a separate chest SCREEN — chest, furnace, craft and inventory are tabs
// on one bar (see BarKind above), and what used to be "the chest screen's 240px" is now the
// CHEST tab's share of the same 240. Every rectangle here still lands where it landed, which
// is the whole reason the tab rework could reuse this block instead of renumbering it.
//
// A THIRD storage screen, same reason the furnace got one of its own instead of a
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
// Aliases of the bar strip since v1.9.1 — see FURN_CLOSE_Y's note for why tracking the strip
// rather than GRID_Y is what leaves these two at the numbers they shipped with.
#define CHEST_CLOSE_Y  BAR_STRIP_Y       // 40 — directly under the hotbar, like FURN_CLOSE_Y
#define CHEST_CLOSE_H  BAR_STRIP_H       // 26 — same close-bar height as the furnace panel

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

// craftCloseRect() and craftRowRect() were here. They are RETIRED — see the BAR_LIST_Y0 block
// above for why the fixed crafting split became a scrolling list. Their replacements are
// barListRowRect() and the CRAFT arm of barCellRect() at the bottom of this header; there is
// no close bar to replace, because the bar's own close tab is the one door off every tab.
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

// ── The bar: strip, tabs, list rows and cells (v1.9.1 INTERFACE) ───────────────────────
//
// Everything below is the geometry of ONE screen with several tabs, replacing three screens
// with one layout each. The rects underneath the strip are not new — barCellRect() hands back
// exactly the rects the three panels already shipped (hotbarSlotRect, gridSlotRect,
// chestSlotRect, chestGridSlotRect, furnInputRect/furnFuelRect/furnOutputRect,
// furnGridSlotRect). What is new is the (row, col) ADDRESSING on top of them, which is what a
// d-pad needs and a stylus never did.
//
// The kinds are in tab order and the order is load-bearing: INVENTORY and CRAFT are always
// present and always first, and the container — chest or furnace, never both, because main.c
// opens one per PLACE press — is always index 2 when it exists. A caller that has just closed
// a container can therefore clamp a cursor by comparing against the count and nothing else.
typedef enum {
	BAR_KIND_INVENTORY = 0,
	BAR_KIND_CRAFT     = 1,
	BAR_KIND_CHEST     = 2,
	BAR_KIND_FURNACE   = 3,
} BarKind;

// The whole strip band, full screen width: tabs on the left, the close tab on the right. Drawn
// as one fill before the tabs go on top of it, so the dividers between tabs are the background
// showing through rather than three more quads.
URect barStripRect(void);

// Tab `i` of `count`, tiling [0, BAR_TABS_W) with no gap and no overlap.
//
// The width is (i+1)*BAR_TABS_W/count - i*BAR_TABS_W/count and NOT BAR_TABS_W/count. The
// division truncates: at 4 tabs the even share is 73 px and four of those leave the last two
// columns of the strip belonging to no tab at all — a 2 px dead strip a stylus can land in and
// get nothing from. Taking the difference of two running edges spreads the remainder across
// the tabs instead, so the tabs differ by at most one pixel and the last one always ends
// exactly on BAR_TABS_W. ui_layout_test.c sweeps every pixel column to prove it.
//
// An out-of-range `i`, or a `count` below 1, is a zero rect — the same "cannot be mistaken for
// a real answer" shape furnSlotRect() uses for a bad index.
URect barTabRect(int i, int count);

// The close tab: BAR_CLOSE_W square at the right edge of the strip, x 294..320. Fixed width,
// so it does not shrink as categories are added — see the bar strip block above.
URect barCloseRect(void);

// Which tab a point on the strip belongs to: a 0-based tab index, BAR_HIT_CLOSE, or
// BAR_HIT_NONE. Knows about the strip and nothing else — a point on the hotbar, on a cell or
// on a list row answers BAR_HIT_NONE here, the same way hitFurnaceSlot() knows about three
// rects and leaves the dispatch order to its caller.
int hitBarStrip(int x, int y, int count);

// Row `visible_row` (0..BAR_LIST_VISIBLE-1) of a list category, in SCREEN space — the caller
// has already subtracted its scroll. 4 px of margin either side and a 2 px gap under each row,
// so a 24 px pitch draws a 22 px row: enough clearance for the 7 px font at both ends and a
// visible seam between rows without a divider quad.
//
// A row outside the window is a zero rect, so a caller that draws the whole recipe list without
// clipping draws nothing for the rows that are scrolled off rather than drawing them on top of
// the hotbar.
URect barListRowRect(int visible_row);

// Fills `out` with the live categories in tab order and returns how many there are: 2 with no
// container open, 3 with one. Never 4 — `has_chest` wins if both are somehow set, because a
// chest and a furnace cannot be open at once and producing two container tabs would be a
// layout that no navigation state could describe.
int barBuildKinds(bool has_chest, bool has_furnace, BarKind out[BAR_MAX_TABS]);

// The tab label. An unknown kind answers "" rather than a placeholder: an empty tab is visibly
// wrong on screen, where a "?" reads like a deliberate category.
const char* barKindLabel(BarKind k);

// How many cursor rows a kind has. The grids ignore `list_len` (INVENTORY is hotbar + 2 grid
// rows = 3; CHEST and FURNACE are hotbar + their own row + 2 grid rows = 4); CRAFT is exactly
// `list_len`, because a recipe list's height is however many recipes there are.
int barKindRows(BarKind k, int list_len);

// How many columns row `row` of a kind has. Everything is INV_HOTBAR_SLOTS (8) wide except the
// furnace's own row (input, fuel, output — 3) and a list (1). An out-of-range row answers 0.
int barKindCols(BarKind k, int row);

// Whether d-pad left/right moves WITHIN the row or switches category. True for the grids, where
// left/right is the only way to reach column 7; false for CRAFT, whose rows are one cell wide
// and whose left/right would otherwise be dead — a list with dead left/right feels broken.
bool barKindHorizontalIsContent(BarKind k);

// The pixel rect of cell (row, col) on a kind's content area, or a zero rect if that cell does
// not exist. `scroll` is the list window's first row and is ignored by every grid kind.
//
// Row order, top to bottom: row 0 is the hotbar on every grid kind; INVENTORY rows 1-2 are the
// bag; CHEST row 1 is the chest, rows 2-3 the bag; FURNACE row 1 is input/fuel/output, rows 2-3
// the bag; CRAFT row r is list row r - scroll.
URect barCellRect(BarKind k, int row, int col, int scroll);

// The inverse: which cell a touch point lands in, or false for a point on the strip, in a
// gutter, or off the panel. `row` and `col` are only written when it returns true.
//
// Deliberately NOT implemented by walking barCellRect() and testing ptInRect on each. It routes
// through the hit tests the three panels already shipped (hitInventorySlot, hitChestSlot,
// hitChestInvSlot, hitFurnaceSlot, hitFurnaceInvSlot) and converts their slot index into a
// (row, col). That costs a second mapping to keep right, and buys the only thing that makes the
// round-trip test in ui_layout_test.c worth running: the rect side and the hit side are two
// independent paths, so swapping two columns in barCellRect() makes the round trip FAIL. Derive
// one from the other and that test passes by construction and can never go red.
//
// The one place the two paths do share code is CRAFT, whose rows have no pre-existing hit test
// to reuse — one rect constructor, walked. Stated here rather than left to be discovered.
bool barCellFromPoint(BarKind k, int x, int y, int scroll, int* row, int* col);

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
