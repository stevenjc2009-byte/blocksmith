# UI skin research — v1.8.19 "The new interface"

Prepared 2026-09-02. Research only. Nothing under `source/` or `tools/` was touched, and
`make`/`make clean` were never run — a shared `build/` is in use by other work in parallel.
This is the only file this pass wrote.

Provenance is marked on every claim:

- **sourced-with-URL** — an external claim, with the URL that backs it.
- **read-from-code** — a claim about Blocksmith, with a `file:line` citation.
- **measured** — a number obtained by actually running or computing something in this pass.
- **reasoned** — a conclusion drawn from the above, not itself directly sourced.
- **assumed** — stated as an assumption because no source was found; flagged, not asserted.

---

## §1 The Legacy Console crafting UI — how it really worked

Legacy Console Edition (4J Studios' Xbox 360/One, PS3/4/Vita, Wii U build of Minecraft) shipped
a **simplified crafting system** as its default, distinct from Java Edition's free-form 3x3
grid. **sourced-with-URL**: "did not require the player to place items in the correct place in
the crafting interface, but instead displayed the ingredients required to craft the selected
item and allow[ed] the player to craft that item as long as the player has the required
crafting ingredients." — [Legacy Console Edition exclusive features, Minecraft
Wiki](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features). Java's classic
grid was still available as a toggle in the GUI options, but the console-native experience was
the menu. **sourced-with-URL** (GameFAQs phrasing of the same distinction): "Console crafting
is a 'menu' where any item chosen will show the recipe." —
[gamefaqs.gamespot.com/xbox360/632873](https://gamefaqs.gamespot.com/xbox360/632873-minecraft-xbox-360-edition/answers/332829-crafting-interface).

**Category structure.** Recipes were organised into fixed tabs — Structures, Tools and
Weapons, Food, Armor, Mechanisms, Transport, Decorations, Banners, Fireworks, Dyes — plus a
separate Stonecutter interface. **sourced-with-URL**: [Legacy Console Edition exclusive
features, Minecraft Wiki](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features).
The tab groupings did not match the Creative inventory's own categories — they were curated
for the crafting context specifically, not reused wholesale. **sourced-with-URL**: same page.

**How a recipe was selected.** Within a tab, the player moved a cursor over item icons (up/
down/left/right) and pressed left/right at the tab strip itself to change category — i.e. two
different axes of movement, one to change tab, one to browse within it.
**sourced-with-URL**: [Legacy Console Edition exclusive features, Minecraft
Wiki](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features). There was
deliberately **no search tab**, unlike Java/Bedrock's recipe book — the player either browsed
the fixed tabs or toggled a "show craftable only" filter that hid recipes they lacked
ingredients for. **sourced-with-URL**: same page.

**How it showed what you were missing.** The menu auto-filled the crafting grid from the
selected recipe and crafted directly against inventory contents, rather than requiring the
player to manually place items — so "what's missing" was answered implicitly by which items
in a tab were craftable (or filterable to "craftable only") rather than by an explicit
missing-ingredients readout. **sourced-with-URL**: same page. This brief could not find a
source describing an explicit red-highlight-on-missing-ingredient affordance (Java's later
recipe book has one); see §13.

**Why players remember it fondly.** No single retrospective essay surfaced in this pass, but
the consistent thread across the wiki and player Q&A sources is that it removed the
Java-original's "you have to already know the shape" problem — a controller-driven, browsable,
auto-filling menu suits a d-pad and a couch far better than a 9-cell placement puzzle with no
in-game shape reference. **reasoned**, from the sourced facts above; no single citation makes
this claim directly, so it is not asserted as sourced.

**Special-case texture and quirks** (armor tab absent from the inventory-grid version, banner
crafting its own sub-interface with its own button, stonecutter its own screen, furnaces
having no recipe-book equivalent, colour-sorted material ordering) are recorded for
completeness but are LCE-specific enough that they belong in "what NOT to take" — see §3.
**sourced-with-URL**: [Legacy Console Edition exclusive features, Minecraft
Wiki](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features).

---

## §2 The XMB interaction model

The XrossMediaBar (XMB) is a two-axis menu: a **horizontal row of category icons** and, for
whichever category has focus, a **vertical column of items descending from it** —
Sony's own description is that a selected icon causes "several more [to] appear vertically,
above and below it," navigable with up/down. **sourced-with-URL**: [XrossMediaBar,
Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar).

**Motion model.** Selection moves the *icons themselves*, not a free cursor drawn over static
icons — "Navigation moves the icons, instead of a cursor." **sourced-with-URL**: same page.
The eight PS3-era top-level categories (Users, Friends, Settings, Photo, Music, Video, Game,
Network/Store) sit on the horizontal bar; sub-items for the focused category are the vertical
list, and the currently-focused item sits at the crossing point of the two axes — a fixed
screen position the content scrolls through, rather than a moving highlight. **sourced-with-URL**:
[XrossMediaBar, Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar); [About the XMB
menu, PlayStation manuals](https://manuals.playstation.net/document/en/ps3/current/basicoperations/xmb.html).

**Input model.** Left/right on the d-pad or stick changes category; up/down moves through the
focused category's vertical list; two further buttons commit a selection or back out one
level. **sourced-with-URL**: [XrossMediaBar, Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar).
This is exactly the "stepping through discrete focus" style, not a positional cursor — see §2's
overlap with the controller-design sources in §3.

**Long lists.** The Wikipedia source does not describe an explicit paging/scrolling mechanic
for an unusually long vertical column — this brief could not find one; flagged in §13 rather
than assumed. What *is* sourced is that the column is always anchored at the fixed crossing
point and scrolls past it, which is the part worth carrying forward regardless of list length
— see §5.

**What made it feel good on a d-pad.** The design is exactly the "focus moves between
discrete, adjacent targets" pattern that the controller-UX sources in §2b (below) identify as
the preferred shape for d-pad input: one press = one guaranteed, discrete step, no analog
aiming, no risk of overshoot. **reasoned**, connecting the XMB's documented mechanic to the
generic controller-UX research in the next section.

---

## §2b Controller-first menu design — why discrete focus beats a cursor

Apple's own tvOS guidance is blunt about the general case: shifting focus between elements
"would be a far more intuitive and enjoyable" experience than driving a cursor, and that
"cursoring isn't a pleasant experience, but the focus model maps over very well" to
controller input. **sourced-with-URL**: cited via [Figma — Press Start: How Controllers
Shaped Video Game Design](https://www.figma.com/blog/press-start-video-game-navigation/) and
[Game UX: the cursor that wasn't supposed to be there — UX
Collective](https://uxdesign.cc/game-ux-the-cursor-that-wasnt-supposed-to-be-there-f9369dafc451).

Community and practitioner consensus (ResetEra discussion, UX Collective's analysis of games
like *Destiny* and *Prey*) converges on the same split: **discrete/list-style focus wins for
controller-first menus** because each d-pad press is a guaranteed, unambiguous step to the
next adjacent target; **free cursors only earn their place in genuinely spatial, dense,
icon-grid layouts** where a natural reading order doesn't exist (character-select grids,
diegetic in-world terminals). **sourced-with-URL**: [Game UX: the cursor that wasn't supposed
to be there](https://uxdesign.cc/game-ux-the-cursor-that-wasnt-supposed-to-be-there-f9369dafc451).

A d-pad is also measurably worse at precision in one axis than the other on handheld form
factors — thumb movement is more accurate horizontally than vertically for a remote-style
grip — which is part of why focus-snapping outperforms analog cursor movement on this class of
input. **sourced-with-URL**: same UX Collective piece, citing Apple's tvOS interaction
guidance.

**What this means for the 3DS specifically.** The 3DS d-pad is a true 4-way digital pad (no
analog ambiguity at all on the left cluster; the Circle Pad is analog but the existing game
code already treats direction input as digital KEY_D* bits — see §4), so the "discrete focus"
case is even stronger here than on a PS3 controller: there is no partial-press state to
design around at all.

---

## §3 What to take, and explicitly what NOT to take

**Take (structure, not art):**

1. **The horizontal-bar-of-categories, vertical-list-under-the-selected-one shape**, from the
   XMB (§2). This is the "long bar with multiple options" the brief asked for by name.
2. **Auto-filled, non-placement crafting from a browsable, tabbed list**, from LCE (§1) — the
   player picks a result, not a grid of components.
3. **Discrete d-pad-stepped focus, never a free cursor**, from both §1 and §2b — appropriate to
   a d-pad with no analog cursor in this game (§4).
4. **A "craftable only" filterability concept** — not literal UI, but the *idea* that the list
   can be narrowed by what the player can currently make, from §1.

**Explicitly do NOT take (the copied bits):**

- LCE's actual tab names, tab groupings, or icon art (Structures/Tools and Weapons/Food/etc.)
  — Blocksmith has 8 blocks total (read-from-code, §4) and no tools, armor, banners,
  fireworks, or dyes; copying LCE's category taxonomy would import categories for content that
  does not exist.
- LCE's asterisk-for-crafting-table-only convention, its separate banner/stonecutter
  sub-screens, or its colour-data sort order — all are artifacts of Java-parity content
  Blocksmith does not have.
- The XMB's specific ribbon background animation, icon shrink/grow treatment, its eight-icon
  system taxonomy (Users/Friends/Settings/...), or its blue/black colour language — none of
  which are load-bearing to the *interaction shape*, all of which are Sony's visual identity.
- Any font, icon, or texture asset from either reference. Every pixel Blocksmith draws already
  comes from a `tools/*.py` generator (§4, §9) and that stays true here — a "long bar" menu
  gets a new generator script, not a traced or sampled icon set.

---

## §4 What Blocksmith's UI is today

**Two screens, confirmed from code.** Top screen is created at `240, 400` (citro3d creates
render targets in the rotated, height-first orientation — 400x240 logical)
`source/gfx/screen.c:39`, and is drawn **twice**, once per eye, when 3D is enabled: a second
render target `s_top_right` exists purely for `GFX_RIGHT` `source/gfx/screen.c:49-51`, gated
by `gfxSet3D` **read-from-code**. Bottom screen is `240, 320` (320x240 logical), single
target, **no depth buffer** ("A UI pass draws in submission order with depth testing off... a
depth attachment here would be ~150 KB of VRAM allocated to be cleared every frame and never
read") `source/gfx/screen.c:60-64`, and is touch-capable — the whole existing inventory/
crafting/debug UI is already built against its 320x240 space (`SCR_W`/`SCR_H` in
`source/scene/ui_layout.h:34-35`). **read-from-code**.

**The sprite pipeline.** `source/gfx/sprite.c` is a single textured-quad batcher: one shared
vertex/index buffer, `spriteQuad`/`spriteRect` primitives, one bound texture at a time
(`spriteTexture` flushes the batch on a texture change) `source/gfx/sprite.c:214-220`. Colour
is per-vertex RGBA8 modulated against the texture in a single TexEnv stage
`source/gfx/sprite.c:178-181` — everything the UI can do colour-wise is "tint this texture,"
because the PICA200 has no programmable fragment shader for this pass to reach for anything
cleverer (hardware fact, not sourced from this codebase but true of the console generally).

**The stale sprite-budget comment, corrected.** `source/gfx/sprite.c:22-28`'s comment sizing
`SPRITE_MAX_QUADS` at 1024 describes the worst frame as "the bottom screen's full inventory
grid — 9x5 slots, each a panel quad plus an icon quad plus up to two digits of count, so
45 * 4 = 180 quads." **This is stale**: the actual inventory is `INV_MAIN_COLS=8` x
`INV_MAIN_ROWS=2` = 16 main slots plus `INV_HOTBAR_SLOTS=8` = **24 total slots**, not 45
(`source/world/inventory.h:103-107`). **read-from-code.** Redoing the comment's own arithmetic
with the real slot count: 24 slots x up to 4 quads (panel + icon + up to 2 digits) = 96 quads,
plus background/hotbar/text (the comment's own "+220" catch-all, unchanged) — call it 140,
against the comment's original 220 for the inventory alone. **measured** (arithmetic run this
pass, not a new claim about behaviour).

The **true current maximum is still `SPRITE_MAX_QUADS = 1024`**, unchanged and not stale
itself — only the worked example in its comment undercounts because it multiplies the wrong
slot count. **read-from-code**, `source/gfx/sprite.c:34`. The buffer costs `1024 * 4 * 24 =
96 KB` of linear memory, stated in the same comment and unaffected by the correction
`source/gfx/sprite.c:30-33`. **read-from-code.**

**Does a richer UI fit inside 1024 quads?** Yes, with real headroom. The comment's own budget
already reserves the full stereo-doubled top-screen name-tag cost (330 quads) and the entire
current bottom-screen HUD (~220), totalling ~550 against a 1024 cap — **1024 leaves not quite
2x headroom on a frame that has never actually occurred**, in the file's own words
`source/gfx/sprite.c:27-28`. **read-from-code.** A long-bar menu (§5) drawn on the bottom
screen only (no stereo doubling, §4 above) at, say, 10 category icons + a 6-row descending
list at 3 quads each (panel + icon + label-as-one-quad-run is an approximation; see §10 for
the honest per-element count) is on the order of 60-100 quads — comfortably inside the
existing headroom without raising `SPRITE_MAX_QUADS` at all. **reasoned**, from the measured
numbers above; the exact new count is worked in §10 once the concrete element list is fixed.

> **⚠ CORRECTION [2026-09-03] — the ~550/~450-headroom figure above is stale; the comment it
> quotes has since been updated to a larger number.** `source/gfx/sprite.c:18-37` today gives
> a different, already-revised accounting: bottom screen worst case ~450 (not this section's
> ~220) plus top-screen stereo name tags 338 (not 330), "roughly 790 of 1024" — leaving
> **~234** quads of headroom, not ~450. The file's own comment now explicitly notes "A UI
> redesign is planned for v1.8.19," which is why it was revised after this research pass ran.
> `SPRITE_MAX_QUADS` itself still comfortably covers this section's ~60-100-quad bar estimate
> and §10's ~183-quad worked total either way, so the bottom-line conclusion ("fits without
> raising the constant") survives — only the "not quite 2x headroom" framing is wrong. This
> was an accurate read of the file when written and the file changed under it afterwards, not
> a research error. `docs/plan-1.9.1-interface.md`'s own citation of this figure (§2, "~550
> quads worst-case shipped") carries the same correction.

**Bottom-screen usage today.** The 320x240 bottom screen already carries, top to bottom: the
hotbar (`HOTBAR_Y=0`, 40px), the 2-row main inventory grid (`GRID_Y=40`, 80px), and a crafting
panel with a close bar and one row per recipe (`CRAFT_Y=120` through `SCR_H=240`, i.e. 120px)
— `source/scene/ui_layout.h:40-56`. **read-from-code.** The debug menu (below) is drawn as a
full-screen overlay on top of this when open, not alongside it
(`source/app/debugmenu_ui.c:199-206` — it draws its own scrim, panel and rows over the whole
320x240). **read-from-code.** There is currently no dead/unused strip of the bottom screen at
any one time — every pixel already belongs to hotbar, grid, or crafting when the game HUD is
showing, and the debug menu takes the whole screen exclusively when it is open. A new
block-list screen (§6) has to be its own full-screen mode, entered and exited explicitly, the
same way the debug menu already is — not squeezed into a gap that does not exist. **reasoned.**

**The existing debug-menu pattern.** `source/app/debugmenu.c` + `.h` is a fixed-capacity
(`DEBUG_MENU_MAX_ENTRIES=32`, `source/app/debugmenu.h:17`) linked-list registry of `DebugEntry`
rows, each with a `kind` (`DEBUG_TOGGLE`, `DEBUG_SLIDER_INT`, `DEBUG_ACTION`, `DEBUG_INFO`)
and a getter/setter or callback pair `source/app/debugmenu.h:19-48`. **read-from-code.**
Callers register rows once at boot — `source/main.c:1139-1193`'s `bsDebugRegister()` is the
one and only registration site today, adding "Render distance" (slider), "Weather"/
"Dimension" (unavailable placeholders, greyed out on purpose so the menu visibly reserves
their place — `source/main.c:1155-1157`), and seven `DEBUG_INFO` rows (Frame, Draws, Culled,
Tick, World, Memory, Player) that each format a live string from a shared `DebugContext`.
**read-from-code.** The UI half, `source/app/debugmenu_ui.c`, is a self-contained
scrollable-list screen (7 visible rows, `VISIBLE_ROWS=7`, `source/app/debugmenu_ui.c:53`) with
its own d-pad cursor, A-to-activate, B-to-close, and per-row touch hit-testing
`source/app/debugmenu_ui.c:84-196`; it is reached only from the pause menu's OPTIONS page via
a "Debug" row (`source/scene/pausemenu.c:211`). **read-from-code.** This is the pattern §7's
biome readout and neon-border toggle are designed to fit into rather than invent a second one.

**The battery indicator — and it already blinks at one bar.** This is not new work; it exists
and matches the ask exactly. `source/app/battery.h:49-63` documents it as built for "the
battery indicator, whenever it gets low on, like, one bar, it should start blinking" — see
§8 for the full mechanism, already shipped as of v1.8.3. **read-from-code.**

**Current menu shape — everything is a vertical list, today.** The title screen's main menu
is `MAIN_ITEM_COUNT=4` (Play, Multiplayer, Options, Quit) stepped with `KEY_DDOWN`/`KEY_DUP`
`source/scene/title.c:45,465-466`. The pause menu is two vertical pages (`PAGE_MAIN`,
`PAGE_OPTIONS`) of rows, `ROW_RESUME`/`ROW_OPTIONS`/`ROW_QUIT` on the first
`source/scene/pausemenu.c:9-16`. The debug menu, above, is a vertical scrolling list. **There
is currently no horizontal axis anywhere in the UI** — every existing menu is a single column,
which is the exact thing v1.8.19's brief in `docs/ROADMAP.md:386-390` calls out to change: "The
menus and inventory redrawn around a long horizontal bar of options." **read-from-code.**

---

## §5 A proposed interface: the long bar, the categories, the descending list

**Shape.** A single horizontal strip of category icons across the bottom screen, one row tall,
with the currently-focused category's items descending in a vertical list below it — the XMB
crossing-point model (§2), applied to Blocksmith's actual content rather than Sony's. The bar
sits where the crafting panel's header already sits today (`CRAFT_Y=120`,
`source/scene/ui_layout.h:46`) so this reuses an existing vertical budget rather than claiming
new screen real estate; the descending list occupies the space `CRAFT_ROW_Y0` through `SCR_H`
already reserves for crafting rows today (`source/scene/ui_layout.h:50,56`). **reasoned**, from
the read-from-code layout constants above.

**Categories, content-sized rather than LCE-sized.** With `BLOCK_COUNT == 8`
(`source/world/block.h:121-122`, `_Static_assert`) and `RECIPE_COUNT` driving the current
crafting panel (`source/scene/ui_layout.h:25,56`), Blocksmith does not have enough distinct
content today to justify LCE's nine-plus tabs (§3). A defensible starting bar is: **Craft**
(the existing recipe list, now under this shape instead of its own fixed panel), **Blocks**
(the new bottom-screen block list, §6), **Inventory** (the existing hotbar/grid, reachable as
its own "category" rather than always-visible — or left always-visible above the bar, see the
open question in §13), and — reached only through the pause menu, never this bar — **Debug**
stays exactly where it is (§7 explains why it must not become a bar category).

**Interaction, mapped to 3DS buttons.**

- **D-pad Left/Right**: move focus one category at a time along the bar (mirrors XMB's
  left/right-changes-category, §2). Discrete stepping only, never analog — consistent with
  §2b's finding that a true digital d-pad has no partial-press state to design a cursor around.
- **D-pad Up/Down**: move focus one row at a time in the focused category's descending list
  (mirrors XMB's up/down-through-the-column, §2, and reuses the exact input pattern already
  proven in `debugMenuUiUpdate`'s `KEY_DUP`/`KEY_DDOWN` cursor stepping,
  `source/app/debugmenu_ui.c:122-123`). **read-from-code** for the reused pattern.
- **A**: commit the focused list row (craft the recipe / select the block / open the sub-page)
  — same semantics as the existing debug menu and pause menu's A-to-activate.
- **B**: back out one level (list focus back to the bar; bar back to whatever screen opened it)
  — same semantics as the existing debug menu's B-to-close (`source/app/debugmenu_ui.c:112-115`).
- **Touch**: tap a bar icon to jump categories directly, tap a list row to select it — both
  already have a proven hit-testing shape to copy from (`hitInventorySlot`,
  `source/scene/ui_layout.h:85`, and the debug menu's per-row touch rect test,
  `source/app/debugmenu_ui.c:177-196`). **read-from-code.**

**Why this survives being drawn on the bottom screen only, not the top.** The bottom screen is
the one target with no stereo doubling and no depth buffer to fight
(`source/gfx/screen.c:60-64`) — exactly the property the owner's ask already leans on ("on the
bottom screen, not the top" for the block list, §6, and every existing dense UI element is
already there, §4). This bar follows the same rule for the same reason: **read-from-code**
plus **reasoned**.

---

## §6 The bottom-screen block list

**Layout.** A full-screen mode on the bottom 320x240, entered from the long bar's "Blocks"
category (§5) or, until that lands, as its own debug-menu entry (§7) — either way it is its
own screen state the same way the debug menu already is (§4), not an overlay squeezed into
existing space, because there is no free space to squeeze into (§4). A simple grid — e.g. 4
columns x however many rows `BLOCK_COUNT` needs (8 blocks today = 2 rows of 4, with headroom
to grow, §10) — each cell an icon quad plus a name label below or beside it.

**Icon source — no new art required.** Every block already has a face-icon path: `iconUv()`
in `source/scene/ui.c:182-184` calls `atlasTile(blockFaceTex(id, FACE_TOP))` to look up a
block's top-face tile in the existing 16x1024 procedurally-generated atlas strip
(`tools/make_atlas.py:8-9`, "Every tile here is generated by this script — nothing is traced,
sampled or copied from another game," `tools/make_atlas.py:4`). **read-from-code.** The block
list reuses this exact function — it is the same call the inventory slots already make to draw
their icons, so a block-list cell is drawn with the identical `spriteQuad` + `iconUv()` call
inventory slots use today, just against `BlockId` 0..`BLOCK_COUNT-1` instead of whatever a slot
holds. **reasoned**, directly from the read-from-code call site.

**Name source.** `blockInfo(id)->name` (`const char* name;` field, `source/world/block.h:193`)
is already the string the inventory/crafting UI draws for a slot's label — reused verbatim,
no new text asset. **read-from-code.**

**How it scales as blocks are added.** The list is driven by iterating `0..BLOCK_COUNT-1`
against the registry (`blockInfo`), not by a hand-maintained array — so a new block that
registers itself in the block table appears in this list automatically, the same way it
already appears in the inventory and crafting UI without either of those needing an edit.
**reasoned**, from the registry-backed pattern `source/world/inventory.h:46-49` documents for
every other slot-drawing path. At `BLOCK_COUNT == 8` today (`source/world/block.h:121-122`)
a 4-wide grid is 2 rows; even a generous future growth to, say, 24 blocks (three times today's
count) is 6 rows of 4 — well inside a 320x240 screen at any reasonable cell size (§10 works the
actual quad-count cost). Past some count the same `VISIBLE_ROWS` + scroll pattern the debug
menu already uses (`source/app/debugmenu_ui.c:53,125-130`) is the proven fallback, not a new
design. **reasoned.**

---

## §7 The biome readout and the debug-only neon border toggle

**Fitting the existing pattern.** Both of these are additions to `bsDebugRegister()`
(`source/main.c:1139-1193`), the one and only place `DebugEntry` rows are registered today —
not a new registry, not a new screen. **read-from-code.**

**Biome readout.** A `DEBUG_INFO` row, the same kind as the seven already there (Frame, Draws,
Culled, Tick, World, Memory, Player — `source/main.c:1172-1192`). Its callback reads
`worldgenBiomeAt(g, player_x, player_z)` (`source/world/worldgen.h:536`) and formats one of
the six named `BiomeId` values — `BIOME_TUNDRA`, `BIOME_TAIGA`, `BIOME_PLAINS`, `BIOME_FOREST`,
`BIOME_DESERT`, `BIOME_JUNGLE` (`source/world/worldgen.h:288-296`) — as a short string, exactly
the shape `bsDbgInfoPlayer`/`bsDbgInfoWorld`/etc. already take (`source/main.c:1113-1134`).
**read-from-code.** This satisfies "the current biome shown on the bottom-screen debug
readout" literally: it is one more row in the menu that already lives on the bottom screen
(`source/app/debugmenu_ui.c` draws the whole panel via `spriteBegin(SCR_W, SCR_H)` against the
320x240 `SCR_W`/`SCR_H` constants, `source/app/debugmenu_ui.c:199`, `source/scene/ui_layout.h:34-35`).
**read-from-code.**

**Neon biome-border toggle — structurally debug-only.** A `DEBUG_TOGGLE` row
(`source/main.c:1158-1163`'s "Weather" row is the exact shape to copy: name, kind, available,
getBool/setBool, ctx). The toggle's `bool` state lives in a variable owned by whatever draws
the world (not in `app/options.c`'s persisted settings, and not reachable from the pause
menu's own OPTIONS page or the title screen's Options screen at all) — the owner's requirement
is "structurally impossible to reach from the normal game menus, not merely hidden," and the
existing registry already gives that for free: `DebugEntry` rows only ever appear inside
`debugMenuUiUpdate`'s panel (`source/app/debugmenu_ui.c`), which is only ever reached through
the pause menu's OPTIONS page's "Debug" row (`source/scene/pausemenu.c:211`) — there is no
other call site in the tree that draws a `DebugEntry` list. **read-from-code.** Nothing about
this toggle needs a second gate; it inherits the debug menu's own reachability, the same as
every other row already there.

**Rendering the border on the fixed-function pipeline.** The PICA200 has 3 texture units and 6
fixed-function TexEnv stages, no programmable fragment shader (stated as a hardware constraint
in the task brief; consistent with how `source/gfx/sprite.c:178-211` configures TexEnv stages
by hand rather than compiling a fragment program for this pass). **read-from-code** for the
consistency, hardware-fact for the constraint itself. A "neon border" at a biome boundary
therefore cannot be a screen-space edge-detect shader — there is no fragment shader stage to
write one in. The buildable version is a **geometry-side** highlight: at biome-boundary
columns (adjacent columns whose `worldgenBiomeAt` differs), draw a thin, bright, unlit
quad/line strip along the boundary using the existing `spriteRect`/`C3D` quad path the world
renderer already has (`source/scene/highlight.c` already draws a comparable block-outline
highlight today, by the same technique) rather than any TexEnv trick. **reasoned**, from
`source/scene/highlight.c`'s existence as a precedent for "outline geometry drawn as extra
quads, not a shader effect."

> **⚠ CORRECTION [2026-09-03] — both of §7's proposals already shipped, in v1.8.8; this
> section is not Phase-1 work still to build, it is a description of a feature that already
> exists.** `CHANGELOG.md`'s v1.8.8 entry states both directly: "Debug-only neon biome
> borders. A toggle in the debug menu — off by default... draws a glowing fence standing on
> the ground exactly along every biome boundary" and "The current biome is now shown on the
> bottom-screen debug readout." Verified live in code, not just the changelog:
> `source/debug/biomeborder.c`/`.h`, `biomeborder_draw.c`/`.h`, `biomeborder_test.c` exist;
> the toggle is a real `DEBUG_TOGGLE` registered at `source/main.c:1394-1398` (name "Biome
> borders", `available = true`, wired to `bsDbgGetBiomeBorders`/`bsDbgSetBiomeBorders`), drawn
> every frame once enabled. The readout is `source/debug/biomeinfo.c`/`.h` and
> `biomeinfo_test.c`, a real host-tested bottom-screen debug row. This section was written
> before v1.8.8 shipped and describes genuinely future work at the time it was drafted; it
> simply rotted once that version landed. `docs/plan-1.9.1-interface.md` §0 already notes
> this section is "unchanged by this version" and its own build order still lists this as
> Phase 1 — that citation needs the same correction, made there directly.

---

## §8 The battery indicator blink at one bar

**Already built — v1.8.3, verified in this pass by reading the shipping code, not a proposal.**
`source/app/battery.h:49-63` records the exact ask, verbatim: "Asked for as 'the battery
indicator, whenever it gets low on, like, one bar, it should start blinking'." **read-from-code.**

**How the level is read.** `PTMU_GetBatteryLevel(&level)` returns a **0-5 discrete level from
the PTMU service** (`source/app/battery.c:114-117`, `int batteryLevel(void)` doc: "0-5 (0 =
empty, 5 = full)," `source/app/battery.h:29`). **This is a real, discrete API value, not
something the game derives** — "one bar" is level 1 exactly, one of the six values PTMU can
report. **read-from-code.**

**What "one bar" means concretely, in this codebase.** `batteryBars()` maps the raw 0-5 PTMU
level to a 0-4 bar count for a 4-segment gauge (`source/app/battery.c:36-45`: level 0→0 bars,
1→1, 2→2, 3-4→3, 5→4). `batteryLow()` is `true` exactly when the raw level is `<= 1` (i.e.
PTMU level 0 or 1 — the two lowest readings, level 1 being literally "one bar" on the 4-segment
gauge) **and** the reading is trustworthy **and** the console is not charging
(`source/app/battery.c:47-53`). **read-from-code.**

**The blink mechanism.** `batteryBlinkOn(now_ms)` is gated on `batteryLow()`: always `true`
(steady) unless the battery is low, in which case it cycles on a 1000ms period, lit for the
first 600ms (`BATTERY_BLINK_PERIOD_MS=1000`, `BATTERY_BLINK_ON_MS=600`,
`source/app/battery.h:63-64`), read from a free-running millisecond clock so the console build
and the host-testable pure-logic half share one clock source. **read-from-code.**
`batteryDraw()` (console-only half, `source/app/battery.c:128-168`) uses this to withhold the
filled bars entirely on the off phase, and additionally dims the *outline* on the off phase
when the battery is at 0 bars — "at zero bars there is no bar left to hide, so the outline has
to carry the warning on its own" (`source/app/battery.c:139-140`). **read-from-code.**

**Verdict for this brief.** No new work is proposed here — the mechanism already exists,
already matches the owner's exact phrasing, and already has host-testable pure logic
(`batteryApplyReading`/`batteryBars`/`batteryLow`/`batteryBlinkOn` all live above the
`__3DS__` guard specifically so `tools/run_host_tests.sh` can link and check them without a
console, `source/app/battery.c:1-9`). **read-from-code.** The only open item is confirming it
on real hardware or in an emulator with a battery-level override, which this research pass did
not attempt — see §13.

---

## §9 Art plan — new procedural generation scripts needed in `tools/`

Every existing texture in this project is generated by a `tools/*.py` script and committed as
a build artifact, never hand-drawn or sourced from elsewhere (`tools/make_atlas.py:1-6`,
`tools/make_font.py`, `tools/make_crack_atlas.py`, `tools/make_banner.py` — **read-from-code**,
confirmed by directory listing). This brief proposes no new *art*, only new *layout code* that
draws with assets already generated, with two narrow exceptions:

1. **Category-bar icons for the long bar (§5).** Craft/Blocks/Inventory need small (roughly
   16x16, matching the existing atlas tile size, `tools/make_atlas.py:8-9`) glyph-style icons —
   not photographic block textures, simple pictograms (a hammer-and-anvil shape for Craft, a
   stack-of-cubes shape for Blocks, a bag shape for Inventory). Proposed as a new script,
   `tools/make_ui_icons.py`, following `make_atlas.py`'s existing one-tile-wide-strip
   convention so it can be sampled with the same `spriteQuad`/UV pattern the block atlas
   already uses, drawn as flat-colour vector shapes with `PIL`/`Pillow` primitives (the same
   library `make_atlas.py` and `make_font.py` already depend on — not confirmed by this pass
   which exact library each uses; flagged in §13). **reasoned**, following the established
   convention rather than inventing a new one.
2. **The neon border quad's colour ramp (§7).** No new texture needed at all — `spriteRect`
   draws flat colour from the existing font-sheet's solid cell (`source/gfx/sprite.c:259-280`),
   so a bright, saturated colour constant is sufficient; no script required.

**Nothing else needs new art.** The block list (§6) reuses the existing atlas and font
verbatim (§6). The biome readout (§7) is text through the existing font. The bar's descending
list rows reuse the existing row/panel drawing primitives already in `debugmenu_ui.c` and
`ui.c` (flat `spriteRect` panels, `fontDraw` labels) — no texture at all, just geometry and
colour constants, matching how every other panel in this UI is already drawn.

---

## §10 Cost — sprite quads, texture units, bytes, and whether `SPRITE_MAX_QUADS` must rise

**Texture units.** The proposed bar + list + block list all draw through the same single-TexEnv
modulate stage the whole UI already shares (`source/gfx/sprite.c:178-181`) — one texture bound
at a time (the font sheet or the block atlas, swapped via `spriteTexture`'s flush-on-change,
`source/gfx/sprite.c:214-220`). **No new texture unit usage** beyond what the UI already does;
the PICA200's 3-unit ceiling is nowhere close to being reached by this design because the
existing sprite pass only ever binds unit 0 for content (unit 1 is explicitly disabled for fog,
`source/gfx/sprite.c:201-203`) — 2 of 3 units are already unused. **read-from-code.**

**Quad budget — worked estimate.** Using the existing UI's own per-element costs as the unit of
account (a slot is "a panel quad plus an icon quad plus up to two digits of count," the
sprite.c comment's own accounting method, `source/gfx/sprite.c:23`):

- **Long bar** (§5): ~4 categories today, each an icon quad + a highlight panel quad when
  focused = 8 quads, plus per-glyph label quads if labels are drawn under icons (a 4-5 char
  label at `FONT_GLYPH_W=5` px/glyph, `source/gfx/font.h:37`, is one quad per glyph the way
  `fontDraw` already works — **not confirmed from this pass whether `fontDraw` batches per-
  glyph or per-string; assumed per-glyph, consistent with the existing name-tag quad-cost
  accounting in `source/gfx/sprite.c:25-26`**) — call it 4 x (1 icon + 1 panel + 5 glyphs) =
  28 quads.
- **Descending list** (§5): up to `VISIBLE_ROWS`-equivalent rows (the debug menu's own
  `VISIBLE_ROWS=7`, `source/app/debugmenu_ui.c:53`, is a reasonable reuse target), each a row
  panel + up to ~12 glyphs of label = 7 x 13 = 91 quads.
- **Block list** (§6): `BLOCK_COUNT=8` cells today, each an icon quad + panel quad + ~6 glyphs
  of name = 8 x 8 = 64 quads; at a future 24 blocks (§6's growth scenario), 24 x 8 = 192 quads.
- **Biome readout row + neon toggle row** (§7): 2 more `DEBUG_INFO`/`DEBUG_TOGGLE` rows in the
  existing debug menu, each already accounted for by the debug menu's own per-row cost (already
  inside the shipping 1024-quad budget today, since the debug menu already draws up to
  `VISIBLE_ROWS=7` rows every frame it is open — this is not new load, only two more rows of a
  kind the screen already draws). **read-from-code.**
- **Neon border quads** (§7): one thin quad per boundary column-edge on-screen, bounded by
  render distance and chunk visibility — not bounded by a fixed constant in this pass; flagged
  as the one cost this brief could not tightly bound (§13), but debug-only and drawn only on
  the toggle's `true` state, so it costs nothing when the feature is off (the overwhelming
  majority of frames, by construction of it being debug-only).

**Total, worst case, everything on screen at once (bar + list + block list at 8 blocks, no
neon borders):** roughly 28 + 91 + 64 = **183 quads**, against a documented working headroom of
"not quite 2x" over the shipping worst-case-so-far of ~550 quads inside a 1024 cap
(`source/gfx/sprite.c:27-28`). **measured** (arithmetic performed this pass on read-from-code
inputs). This fits inside the existing 1024-quad `SPRITE_MAX_QUADS` with no change needed.
**reasoned**, directly from the above.

**Does `SPRITE_MAX_QUADS` need to rise?** **No, not for anything proposed in this brief.**
183 new worst-case quads added to the existing ~550 worst-case total is ~733, still under 1024.
**reasoned.** It would need to rise only if a future block count grew far past the 24-block
scenario modelled above (each +16 blocks costs ~+128 quads at this accounting) or if every one
of these new screens were required to be visible *simultaneously* with the full stereo-doubled
top-screen name-tag load *and* the existing bottom-screen HUD at once, which does not match how
any of these screens are designed to be reached (each is its own full-screen mode, §4, §6, not
an overlay stacked on everything else). If it ever does need to rise, the cost is stated in the
existing comment already: **96 KB → linear scaling per 1024-quad increment**
(`source/gfx/sprite.c:30-33` states 1024 quads = 96 KB, i.e. ~93.75 bytes/quad = 24 bytes/vert
x 4 verts, `SpriteVertex` being a fixed 24-byte struct, `source/gfx/sprite.c:12-16`).
**read-from-code**, arithmetic **measured**.

---

## §11 Recommended phase order for v1.8.19

Per the operating rules governing this project, each phase below is a stop-and-playtest
boundary, not a sequence to run through unattended — this brief only orders them, it does not
authorise building past a phase without sign-off.

1. **Phase 0 — confirm, no new code.** Verify the battery blink (§8) on real hardware or in an
   emulator that can force a low-battery reading, since it already exists but this research
   pass could not run it. Cheapest possible phase; establishes a known-good baseline before
   touching anything else.
2. **Phase 1 — biome readout + neon toggle in the existing debug menu (§7).** Smallest unit of
   new code: two `DebugEntry` registrations in `bsDebugRegister()`, no new screen, no new art,
   fits the pattern exactly. Good first playtest: does the biome name update as the player
   walks between biomes, does the toggle visibly do nothing when off and something distinct
   when on, and is it genuinely unreachable outside the debug menu.
3. **Phase 2 — block list screen (§6).** New full-screen mode, but zero new art (reuses the
   atlas and font verbatim) and a well-understood grid-of-cells layout close to the existing
   inventory grid's own code shape. Good playtest: does every current block appear with a
   correct icon and name, and does the layout still read cleanly at `BLOCK_COUNT=8`.
4. **Phase 3 — the long bar itself (§5), starting with just Craft as a single category** (i.e.
   the existing crafting panel re-skinned into the bar shape with only one category, so the
   d-pad left/right axis exists but has nothing to prove yet). Isolates "does the bar+list
   interaction feel right" from "are there enough categories to need one."
5. **Phase 4 — add Blocks and Inventory as further bar categories**, wiring Phase 2 and the
   existing hotbar/grid into the bar shape. This is the phase that actually delivers "a long
   bar with multiple options" as asked.
6. **Phase 5 — category-bar icon art (§9)**, once the bar's category set is stable enough that
   icons are worth drawing (building icons before Phase 4 settles the category list risks
   drawing icons for categories that get renamed or merged).

Each phase produces something independently playable and reversible; nothing here requires a
later phase to make an earlier one useful.

---

## §12 Sources

- [Legacy Console Edition exclusive features — Minecraft Wiki](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)
- [Minecraft Wiki:Projects/LCE/Crafting/Legacy Console Edition](https://minecraft.wiki/w/Minecraft_Wiki:Projects/LCE/Crafting/Legacy_Console_Edition)
- [Crafting interface? — GameFAQs, Minecraft: Xbox 360 Edition Q&A](https://gamefaqs.gamespot.com/xbox360/632873-minecraft-xbox-360-edition/answers/332829-crafting-interface)
- [XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)
- [About the XMB (XrossMediaBar) menu — PS3 manuals](https://manuals.playstation.net/document/en/ps3/current/basicoperations/xmb.html)
- [Press Start: How Controllers Shaped Video Game Design — Figma blog](https://www.figma.com/blog/press-start-video-game-navigation/)
- [Game UX: the cursor that wasn't supposed to be there — UX Collective](https://uxdesign.cc/game-ux-the-cursor-that-wasnt-supposed-to-be-there-f9369dafc451)
- [Does anybody actually like the trend of making you navigate menus in console games with a virtual mouse cursor? — ResetEra](https://www.resetera.com/threads/does-anybody-actually-like-the-trend-of-making-you-navigate-menus-in-console-games-with-a-virtual-mouse-cursor.505422/page-3)

---

## §13 What could NOT be determined

- **LCE's explicit missing-ingredient affordance.** Sources describe the auto-fill/craft-
  against-inventory mechanic and the "craftable only" filter, but no source in this pass
  described a specific visual treatment (e.g. red-highlighted icon) for a recipe the player
  cannot currently make. Not assumed; §1 states only what was sourced.
- **XMB's exact behaviour for an unusually long vertical column** (paging vs. continuous
  scroll vs. a hard cap on visible items). The Wikipedia source describes the crossing-point
  model but not this edge case. §2 flags this rather than inventing a mechanic.
- **Which Python imaging library `tools/make_atlas.py` and `tools/make_font.py` actually use**
  for pixel-level drawing (assumed `Pillow`/`PIL` by convention in §9's icon-script proposal,
  but this pass read `make_atlas.py`'s header comment and layout rationale, not its full
  import list) — worth confirming before writing `tools/make_ui_icons.py` for real.
- **Whether `fontDraw` batches one quad per glyph or has some other internal batching** — §10's
  quad-budget estimate assumes per-glyph (consistent with the existing sprite.c comment's own
  accounting method for name tags, `source/gfx/sprite.c:25-26`), but this pass did not open
  `gfx/font.c` to confirm the implementation directly.
- **Real-hardware or emulator confirmation of the battery blink (§8).** The code was read and
  is internally consistent and host-tested for its pure-logic half, but this pass did not boot
  the game or force a low-battery PTMU reading to see it fire — recommended as Phase 0 in §11
  precisely because it is unverified by observation, only by reading.
- **The exact on-screen worst-case count of neon-border boundary quads (§7, §10).** This
  depends on render distance and how many biome-boundary columns are simultaneously visible,
  which this pass did not measure by running the game — flagged as the one part of the quad
  budget in §10 that is bounded qualitatively (debug-only, off by default) rather than
  numerically.
