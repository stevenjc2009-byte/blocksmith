# v1.8.19 — The new interface: implementation spec

This is the detailed version of `docs/ROADMAP.md:386-390`'s v1.8.19 entry (*"Changed. The
menus and inventory redrawn around a long horizontal bar of options — the shape the legacy
console crafting menu and the PS3 system menu share. Referenced, not copied."*) and
`docs/VERSION-LIST.md`'s matching entry.

**This document builds on `docs/research/ui-skin.md`, which is the settled research for this
version.** Its XMB and Legacy Console Edition (LCE) research (§1-3), its read of the engine
(§4), its proposed bar/list/block-list design (§5-7), its battery-blink finding (§8), its art
plan (§9), its quad-cost method (§10), and its phase order (§11) are not restated here — they
are cited by section number and built on directly.

**`docs/research/interface.md` is confirmed here as the stale duplicate.** It is an earlier,
incomplete pass at the same brief — Part C ("What the project's UI is today") and Part D
("Proposed design") are both unfilled placeholder headers in that file, and its own Part A
research (LCE/XMB) covers the same ground `ui-skin.md` §1-3 covers, independently sourced but
reaching the codebase-specific design questions this document has to answer. **`ui-skin.md` is
canonical; `interface.md` should be treated as superseded, not as a second opinion** — except
for the one real, still-useful fact `interface.md` A1 carries that `ui-skin.md` does not: LCE's
own controller scheme used the **shoulder buttons** for category-switching, not d-pad
left/right. That single fact is addressed directly in §5.2 and §7 (HIS CALL 1), because it is
the one place the two documents' research genuinely diverges on an input-mapping question that
matters to this build.

This document does not implement anything and does not edit any file under `source/`, `tests/`,
or `tools/`. Three things this pass discovered by reading further than `ui-skin.md`'s own pass
went, all of which change what "build the long bar" actually costs:

1. **The block-list screen `ui-skin.md` §6/§11 proposes building already exists**, already
   works, and is already host-tested — it shipped as a v1.8.8 debug feature
   (`source/debug/blocklist.c`/`.h`, `source/debug/blocklist_test.c`). §3.1 corrects
   `ui-skin.md`'s framing of this as new work.
2. **That existing screen's input handling already matches the bar's proposed scheme by
   accident** — it accepts `KEY_DUP`/`KEY_DDOWN` (and `KEY_L`/`KEY_R`) for paging and `KEY_B`
   for close, which is exactly `ui-skin.md` §5's "Up/Down moves the list, B backs out" — zero
   changes needed to reuse it. §3.1.
3. **That same screen's own layout claims the whole 320x240 bottom screen**, including the
   y=0-40 band where the hotbar is drawn today. Reusing it unmodified as a bar category means
   the hotbar cannot stay visible while that category is open — a real difference from how the
   existing inventory/crafting overlay behaves today, and one `ui-skin.md` did not have the
   information to catch since it did not read `debug/blocklist.h`'s layout constants directly.
   §3.2 works this out in full; §7 (HIS CALL 2) is where the owner picks the resolution.

---

## 0. What this version actually is, stated once

Per `docs/ROADMAP.md:386-390`, this version is a **reshaping of existing screens**, not new
gameplay content — the crafting panel, the inventory grid, and (newly, per this pass's own
finding) the already-built block list all move into one shared navigation shape: a horizontal
bar of categories with a vertical list descending from whichever one has focus. The Debug menu
is explicitly **not** one of the bar's categories — it stays reached only through the pause
menu's OPTIONS page, exactly where it is today (`ui-skin.md` §7, unchanged by this version).

Nothing about `BLOCK_COUNT`, `registryCrc16()`, or the wire protocol moves for this version —
every change here is client-side presentation over data the client already has. The one
cross-version dependency worth stating plainly: **v1.8.18 (storage/QoL) ships first** per the
roadmap's own ordering, so by the time this version's work begins, `RECIPE_COUNT` is already 5
(the chest recipe, `docs/plan-1.8.18-storage-qol.md` §3.6) and a `BLOCK_CHEST` row already
exists in the registry — this document's own numbers (§3, §4) are read against that already-
landed state, not against today's 4-recipe, chest-less tree, so a reader checking this document
against a v1.8.18-built tree will see the numbers this document quotes; a reader checking it
against today's un-built tree will see one fewer recipe and one fewer block than quoted.

> **⚠ CORRECTION [2026-09-03] — the "today's tree has `RECIPE_COUNT` 4" premise is false; it
> is already 5, for a reason unrelated to the chest.** `source/world/crafting.h` (read
> directly): `RECIPE_COUNT == 5` today — `RECIPE_COAL_ORE_TO_TORCH` landed via v1.8.12, which
> has nothing to do with storage/QoL or a chest. No `BLOCK_CHEST` row exists anywhere in the
> registry yet (confirmed: zero grep hits for chest/container/per-instance block state in
> `source/`) — the chest itself has not landed, contrary to what this paragraph's framing
> implies. So the real progression is: `RECIPE_COUNT` is 5 **today**, independent of v1.8.18,
> and will become **6** once a chest recipe actually lands (per the same correction now in
> `docs/plan-1.8.18-storage-qol.md` §2.8). A reader checking this document against the tree as
> it stands right now will see `RECIPE_COUNT == 5` already — not because a chest shipped, but
> because an unrelated torch recipe did. This same off-by-one, from the same pre-v1.8.12
> research snapshot, is why §5.2's `CRAFT_ROW_H` worked example below lands on the right
> number (5) for the wrong reason — see the correction there.

---

## 1. Success criteria

Same split as `docs/plan-1.8.8-biome-identity.md`'s and the storage document's own §1 — a green
host suite is not the same claim as "it feels right on a real screen," and this version is
almost entirely about feel.

### A. Measurable, host-only

1. `blockListCount()`, `blockListBuild()`, `blockListQuadsFor()` and the rest of
   `debug/blocklist.c`'s existing host-tested surface are **untouched** — this version adds a
   new caller, not a new implementation, and the existing `blocklist_test.c` suite staying green
   with no edits to the files it tests is itself a form of proof that nothing about the reused
   screen was disturbed.
2. A new bar-dispatch layer (wherever it lands — §4.4) has its own host-testable input-mapping
   function: given a category index and a key bitmask, it returns the resulting category index
   and/or forwarded sub-screen keys, checked the same way `blockListUiInput()` already is —
   pure function, no GPU, no console.
3. The quad cost of every screen this version adds or changes is checked against a real number,
   not an estimate — `ui-skin.md` §10's own method (cite the exact op-count function where one
   exists, §3.3 below) rather than hand arithmetic wherever the codebase already has the real
   figure available.
4. Crafting stays single-input (§5) — a host test that a chest-style multi-item recipe cannot be
   expressed by `CraftRecipe` today (already implicitly proven by the type's own two-ItemId
   shape) is not new work this version needs to add; it is an existing structural fact, restated
   here as a success criterion so nothing in this version accidentally tries to grow it.

### B. Visual, playtest-only

1. **The bar reads as one bar, not three unrelated panels** — Craft, Blocks, and Inventory
   switching by d-pad left/right needs to be looked at, not just exercised by a script, since the
   whole point of the XMB shape (`ui-skin.md` §2) is a *feel*.
2. **Whichever resolution the owner picks for the hotbar-visibility question (§7, HIS CALL 2)
   does not look broken** — hotbar popping in/out as the player switches categories, or a category
   that looks like it is missing content because the hotbar is occluding it, are both real risks
   only a look can catch.
3. **The reused block-list screen still reads correctly when entered from the bar instead of the
   debug menu** — same screen, same code, but a different door into it, and the footer text it
   already draws (`"B back   L/R or D-pad: page"`, `blocklist.h:28`) was written for a debug-menu
   context; whether it still reads sensibly from the bar is a wording question a look will answer
   and a host test cannot.
4. **Battery blink** (`ui-skin.md` §8, already shipped v1.8.3): still unconfirmed on real
   hardware, per `ui-skin.md` §11 Phase 0 — re-flagged here since this version's own build order
   (§6) inherits that phase unchanged.

---

## 2. What `ui-skin.md` already settled — cited, not restated

Read `ui-skin.md` in full before this section; it is not reproduced here. The load-bearing
settled facts this document builds on directly:

- The two-screen hardware split and why the bottom screen is the only sane home for this UI
  (§4: no stereo doubling, no depth buffer to fight).
- The sprite pipeline is one shared textured-quad batcher, `SPRITE_MAX_QUADS=1024`, currently
  ~550 quads worst-case shipped, comfortable headroom (§4, §10).
  > ⚠ CORRECTION [2026-09-03]: stale, carried from `ui-skin.md` — see the correction added
  > there. `source/gfx/sprite.c`'s comment now gives ~790 of 1024 shipped, ~234 headroom, not
  > ~550/~450. The conclusion ("fits without raising `SPRITE_MAX_QUADS`") is unaffected.
- The XMB crossing-point model and why it suits a true digital d-pad better than a cursor (§2,
  §2b) — the interaction shape this whole version is built around.
- What to take from LCE and XMB, and explicitly what not to (§3) — structure, never art, never
  the reference's own taxonomy.
- The existing bottom-screen budget: hotbar (`HOTBAR_Y=0`, 40px), main grid (`GRID_Y=40`, 80px),
  crafting panel (`CRAFT_Y=120`, 120px) (§4).
- The proposed bar shape: d-pad left/right = category, up/down = list row, A = commit, B = back,
  touch = tap either axis directly (§5).
- The block-list screen's reuse of the existing atlas/font with no new art (§6) — correct in its
  conclusion, though §3.1 below corrects its premise that the screen itself is new work.
- The biome readout and neon-border-toggle design, fitting the existing `bsDebugRegister()`
  pattern (§7) — unaffected by anything in this document, not touched further here.
  > ⚠ CORRECTION [2026-09-03]: both already shipped, in v1.8.8 — see the correction added to
  > `ui-skin.md` §7. This document's own Build order (§6) repeats the stale "Phase 1 — biome
  > readout + neon toggle" framing; corrected there too.
- The battery blink already being shipped, real work being confirmation only (§8).
- The art plan: one new script, `tools/make_ui_icons.py`, for category-bar pictograms only;
  everything else reuses existing atlas/font assets (§9).
- The quad-cost method and its own worked estimate, ~183 quads worst case for the bar + list +
  an 8-block list (§10) — §3.3 below updates the block-list half of this number now that the
  real registry count and the real op-cost function are both known.
- The six-phase build order (§11) — §6 below carries it forward with one re-ordering.

---

## 3. Corrections and additions to the settled research

### 3.1 The block-list screen is not new work — it already shipped, in v1.8.8

`ui-skin.md` §6 proposes building "a full-screen mode... a simple grid... each cell an icon
quad plus a name label," and §11 Phase 2 budgets a whole phase for it. Reading
`source/debug/blocklist.c`, `source/debug/blocklist.h`, and `source/debug/blocklist_test.c`
directly (none of which `ui-skin.md`'s own pass opened) shows this screen **already exists,
already works, and is already host-tested**, as a v1.8.8 debug feature reached today only
through the debug menu (`source/app/debugmenu_ui.c:210`, `blockListUiOpen()` called from a
debug-menu row).

What it already does, read directly from `blocklist.h:1-174` and `blocklist.c`:

- **Registry-driven, not `BLOCK_COUNT`-driven.** It walks `registryIsDefined()` across the full
  `0..255` id space (`blocklist.c:30-36`), which is the correct span — §3.2 below explains why
  `ui-skin.md` §6's own `BLOCK_COUNT==8` framing for this screen is stale.
- **Paged, 2 columns x 8 rows, `BL_PER_PAGE=16`** (`blocklist.h:60-65`), at `BL_CELL_W=154`,
  `BL_CELL_H=24`, occupying `BL_GRID_Y=26` through `26 + 8*24 = 218`, header at `BL_HEAD_Y=8`,
  footer at `BL_FOOT_Y=224` — **the whole 320x240 screen, top to bottom** (`blocklist.h:60-72`).
- **Emits a draw-program, not just rects** — `BlockListOp[]`, host-testable, consumed by a dumb
  loop in `debugmenu_ui.c`'s console half (`blocklist.h:19-38`'s own file comment explains why:
  the project has already shipped one "test re-derives what it thinks the screen draws" bug, and
  this shape makes that class of bug impossible by construction).
- **Already has an exact, host-tested quad-cost function**, `blockListQuadsFor()`
  (`blocklist.c:92-111`) — not an estimate, a real count over the real op list, "safe to quote
  against `gfx/sprite.h`'s `SPRITE_MAX_QUADS`" in the header's own words (`blocklist.h:143-146`).
- **Its input handling already matches the bar's proposed axis scheme, today, for the debug
  menu's own reasons, not this version's.** `debugmenu_ui.c:195-199`:

```c
if (down & KEY_B)                          bl |= BL_KEY_CLOSE;
if (down & (KEY_R | KEY_DRIGHT | KEY_DDOWN)) bl |= BL_KEY_NEXT;
if (down & (KEY_L | KEY_DLEFT  | KEY_DUP))   bl |= BL_KEY_PREV;
```

  Every one of `KEY_DUP`/`KEY_DDOWN`/`KEY_L`/`KEY_R` already pages; `KEY_B` already closes. The
  bar's own proposed scheme (§2, `ui-skin.md` §5) uses **exactly** `KEY_DUP`/`KEY_DDOWN` for
  list movement and `KEY_B` for back — this screen's existing input set is a strict superset of
  what the bar needs from it, and needs **zero code changes** to slot into the bar's "Blocks"
  category: a new call site only has to route `KEY_DUP`/`KEY_DDOWN` into `blockListUiInput()`
  and let the bar's own d-pad-left/right claim stay unambiguous (§3.2 covers why left/right are
  never actually needed by this screen, since up/down alone already covers paging).

**Consequence:** the "Blocks" bar category is a wiring task — a new call site opening
`blockListUiOpen()`/routing input to `blockListUiInput()`/drawing via `blockListBuild()` — not
a rebuild of a screen that does not exist yet. `ui-skin.md` §11 Phase 2's phase content should
be read as "wire in the existing screen," which is materially cheaper than "build a block list
screen," and §6 below reorders the phase list to reflect that.

### 3.2 The real design fork: this screen wants the whole 320x240, hotbar included

`ui-skin.md` §5 places the bar at `CRAFT_Y=120` specifically because that is where the crafting
panel already lives, reusing an existing vertical budget rather than claiming new space — and
that reasoning is correct **for the Craft category**, because the existing overlay convention
(`source/scene/ui.c:352-407`, read directly) already keeps the hotbar visible (`y=0-40`, drawn
unconditionally on every frame, in or out of the overlay) and only swaps the `y=40-240` region
between the plain HUD and the inventory-grid-plus-crafting-panel content.

**The block-list screen does not fit that convention.** `BL_HEAD_Y=8` (`blocklist.h:71`) puts
its own header text inside the exact band (`y=0-40`) the hotbar occupies today, and its grid
runs `y=26` to `218` — it was written, correctly, as a screen that owns the entire bottom
display, the same way `app/debugmenu_ui.c`'s own full-screen overlay does
(`ui-skin.md` §4's own citation of `debugmenu_ui.c:199-206`). **Reusing it unmodified inside the
bar means the hotbar cannot stay visible while "Blocks" has focus** — a real, different
visibility rule from what "Craft" and "Inventory" get under the existing convention, where the
hotbar stays on screen throughout.

This is not a defect in either piece of code — it is two screens built for two different
contexts (an always-reachable HUD overlay vs. a full-screen debug tool) now being asked to sit
behind one shared bar. `ui-skin.md` itself flagged wanting to resolve exactly this shape of
question — its §5 says Inventory is "reachable as its own category — or left always-visible
above the bar, see the open question in §13" — **but `ui-skin.md`'s own §13 does not actually
contain that open question**; §13 as written lists five different unresolved items (LCE's
missing-ingredient affordance, XMB's long-column behaviour, the imaging library, `fontDraw`'s
batching, and the neon-border quad count), none of which is this one. That reference is a
dangling one in the settled research, and this document is where it is finally answered — see
§7, HIS CALL 2.

### 3.3 The real quad cost of the Blocks category — measured via the existing function, not estimated

`ui-skin.md` §10 estimates the block list at "`BLOCK_COUNT=8` cells... 8 x 8 = 64 quads,"
explicitly using `BLOCK_COUNT` as the item count. §3.1/§3.2 above establish that this screen
never used `BLOCK_COUNT` — it walks every **registry-defined** id, which (per
`docs/plan-1.8.18-storage-qol.md` §2.1 and §3.1, read directly against the live tree in that
pass) is **27 core rows today, 28 once the storage version's chest row lands** — and it pages 16
at a time, not all-at-once.

The honest number is not a new hand estimate — it is **already computed, exactly, by
`blockListQuadsFor()` for whatever op list `blockListBuild()` returns for a given page**
(§3.1). A full 16-block page's worst case (every cell filled, no air, every name near
`REGISTRY_NAME_MAX`'s 16-character ceiling) is on the order of: 1 background rect + ~27 header
glyphs (`"BLOCK LIST  28 blocks  page 2/2"`, counted the way `blockListTextQuads()` counts —
non-space printable characters only) + 16 x (1 cell rect + 1 icon + up to ~18 label glyphs
`"%02X %s"`) + ~20 footer glyphs — **roughly 1 + 27 + 16x20 + 20 ≈ 368 quads for one fully
populated page**, a real number `blockListQuadsFor()` would confirm exactly rather than this
document asserting it. This is materially higher than `ui-skin.md` §10's 64-quad estimate, and
worth stating plainly: **`ui-skin.md`'s own worst-case total (~183 quads) undercounts the Blocks
category specifically**, though the corrected number (~368) is still comfortably inside the
documented ~450-quad remaining headroom under `SPRITE_MAX_QUADS=1024` (`ui-skin.md` §4, §10) —
so the conclusion "no change to `SPRITE_MAX_QUADS` needed" still holds, only the arithmetic
behind it needed correcting. **My proposal, not yet in any code:** whoever wires this in should
call `blockListQuadsFor()` on the real built op list and log or assert the number once, rather
than trust either this document's or `ui-skin.md`'s hand count — the function exists precisely
so nobody has to.

---

## 4. Design — the bar, and the mockups

### 4.1 Categories

Three player-facing categories on the bar, exactly as `ui-skin.md` §5 proposes: **Craft**,
**Blocks**, **Inventory**. Debug is not on the bar (§0, unchanged from `ui-skin.md` §7).

### 4.2 Interaction model — unchanged from `ui-skin.md` §5, restated once for completeness

- **D-pad Left/Right** — move focus one category at a time along the bar.
- **D-pad Up/Down** — move focus one row at a time in the focused category's content (for
  Blocks, this means "one page at a time," per §3.1's finding that the existing screen pages
  rather than scrolls row-by-row — a real, different granularity from Craft's per-recipe
  stepping, noted honestly rather than smoothed over).
- **A** — commit the focused row (craft the recipe / select the block / pick up-or-drop an
  inventory slot).
- **B** — back out one level.
- **Touch** — tap a bar icon to jump categories directly; tap a list row to select it.

**How the bar itself opens and closes — unchanged from today, my proposal.** The existing
inventory/crafting overlay (`UI_SCR_HUD` vs `UI_SCR_INVENTORY`, `source/scene/ui.c:352`) is
opened and closed **by touch only** today — tapping `hudToggleRect()` (`y=40-72`,
`ui_layout.h:58-59`) opens it, tapping `craftCloseRect()` closes it
(`source/scene/ui.c:357-368`, read directly). **No button opens or closes it today at all.**
Every other button is already spoken for: `KEY_SELECT` already toggles the pause menu
(`main.c:4415`), `KEY_START` is already a quit/confirm key in several screens (`main.c:1733,
3401, 4367`), and `KEY_A`/`KEY_X`/`KEY_Y` are Jump/Break/Place (§2.10 of the storage document).
**This document proposes leaving the open/close gesture exactly as it is — touch-only** — since
every remaining button is either already claimed or already spoken for by the storage document's
own proposals (`KEY_B` for chest open/close, shoulder `L`/`R` for hotbar cycling), and the 3DS
always has its touch screen physically present, so a button-only path in is a nice-to-have, not
a requirement the roadmap's wording asks for. If the owner wants a button binding for this too,
that is a small, separate addition, not assumed here.

### 4.3 Mockups — bottom screen, 320x240, real pixel coordinates

**The bar strip and the Craft category** (reuses the existing `CRAFT_Y` budget, per
`ui-skin.md` §5 — this is the one category that fits the existing overlay convention with no
change):

```
y=0    +--------------------------------------------------------------+
       | [x][x][x][x][x][x][x][x]                                     |  hotbar, 0-40
y=40   +--------------------------------------------------------------+
       | [  ][  ][  ][  ][  ][  ][  ][  ]                              |  main grid r1
       | [  ][  ][  ][  ][  ][  ][  ][  ]                              |  main grid r2, 40-120
y=120  +--------------------------------------------------------------+
       | <CRAFT>   BLOCKS    INVENTORY                    X close     |  bar, 120-148
y=148  +--------------------------------------------------------------+
       | > Planks            x1 Wood -> x4 Planks           [craft]   |  row 0, 148-171
       |   Chest              x6 Planks -> x1 Chest                   |  row 1, 171-194
       |   ...                                                        |  rows scroll/step
y=240  +--------------------------------------------------------------+
```

(`CRAFT_ROW_H` is `(CRAFT_H - CRAFT_CLOSE_H) / RECIPE_COUNT` — at the post-v1.8.18
`RECIPE_COUNT=5` this is `(120-28)/5 = 18.4`, not the `23` `ui_layout.h`'s own comment states
for `RECIPE_COUNT=4` — the constant self-adjusts by its own formula, per that file's comment,
"so adding one costs no layout edit here"; **read in this codebase**, `ui_layout.h:51-56`.)

> ⚠ CORRECTION [2026-09-03]: the `18.4` result is right, but "post-v1.8.18" is not why —
> `RECIPE_COUNT` is already 5 today, before v1.8.18 or any chest lands (see §0's correction).
> Once a chest recipe actually does land, `RECIPE_COUNT` becomes 6, and `CRAFT_ROW_H` becomes
> `(120-28)/6 = 15.3`, not 18.4. This worked example is only valid for the tree as it stands
> right now (5 recipes, no chest) — it will need redoing again once v1.8.18 actually ships.

**The Blocks category, per §3.2's finding — the full screen, hotbar included, if the owner
picks the "reuse it whole" resolution in §7:**

```
y=0    +--------------------------------------------------------------+
       | BLOCK LIST  28 blocks  page 1/2                              |  header, y=8
y=26   +--------------------------------------------------------------+
       | [i] 00 AIR                  | [i] 08 WATER                   |
       | [i] 01 GRASS                | [i] 09 TALL_GRASS               |
       | [i] 02 DIRT                 | [i] 0A SNOW                     |
       | [i] 03 STONE                | [i] 0B ICE                      |
       | [i] 04 SAND                 | [i] 0C CACTUS                   |  2 cols x 8 rows,
       | [i] 05 WOOD                 | [i] 0D DEAD_BUSH                |  154px each column,
       | [i] 06 LEAVES               | [i] 0E FERN                     |  24px each row,
       | [i] 07 PLANKS               | [i] 0F BIRCH_LOG                |  y=26-218
y=218  +--------------------------------------------------------------+
y=224  | B back   L/R or D-pad: page                                  |  footer (existing text)
y=240  +--------------------------------------------------------------+
```

This is the **existing, unmodified `blockListBuild()` output**, drawn verbatim
(`blocklist.c:158-222`) — the mockup above is not a new layout, it is the real one, transcribed.
If instead the owner picks the "keep the hotbar visible, shrink Blocks to fit under the bar"
resolution in §7, this mockup does not apply and a new, denser layout (not designed in this
document, since it would be new work with its own quad/paging cost, not the free reuse §3.1
found) would be needed instead.

**The Inventory category** (identical to today's existing grid+hotbar layout, `y=0-120`, now
reached via the bar rather than as the overlay's default content — the bar itself would sit
below it at `y=120-148` with an empty or hidden list region below that, since Inventory has no
"descending list" content of its own beyond the grid it already shows):

```
y=0    +--------------------------------------------------------------+
       | [x][x][x][x][x][x][x][x]                                     |  hotbar, 0-40
y=40   +--------------------------------------------------------------+
       | [  ][  ][  ][  ][  ][  ][  ][  ]                              |  main grid r1
       | [  ][  ][  ][  ][  ][  ][  ][  ]                              |  main grid r2, 40-120
y=120  +--------------------------------------------------------------+
       | CRAFT    <BLOCKS>   <INVENTORY>                   X close     |  bar, 120-148
y=148  +--------------------------------------------------------------+
       | (no list content — the grid above already shows everything)   |
y=240  +--------------------------------------------------------------+
```

This mockup makes concrete the redundancy `ui-skin.md` §5 flagged without resolving: Inventory
as a "category" has nothing to put in the descending-list region, because its content is already
fully shown above the bar. **This is itself part of HIS CALL 2's real question (§7)** — if
Inventory has no list, is it really a category at all, or is it simply "what's already visible
whenever the bar itself is open, regardless of which of Craft/Blocks has focus"? My proposal,
stated plainly rather than left open: **make Inventory the default/no-selection state of the bar
rather than a third category to step onto** — the grid and hotbar are already always visible
whenever the overlay is open, so there is nothing to gain by making "look at the grid" a fourth
d-pad press away. This shrinks the bar to two real categories (Craft, Blocks) plus the
always-visible grid, which is a real, opinionated design simplification of `ui-skin.md` §5, not
a neutral restatement of it — flagged as my proposal, not fact, and worth the owner's explicit
sign-off since it changes what "three categories" in the ROADMAP's own wording actually means.

### 4.4 Where the bar's own logic lives

Not a source-file decision this document can make (§ constraints), but worth naming the shape:
a new pure host-testable module (mirroring `scene/ui_layout.h`'s and `debug/blocklist.h`'s own
"no `<3ds.h>`, layout and input math only" pattern) that owns which category has focus and
dispatches d-pad left/right between Craft/Blocks, forwarding up/down/A/B either to the existing
crafting-panel handlers (`scene/ui.c`) or to `blockListUiInput()`/`blockListBuild()`
(`debug/blocklist.c`) depending on focus — a thin router, not a reimplementation of either
screen's own logic. This keeps both of this version's constituent screens exactly as tested as
they are today.

---

## 5. Does crafting stay single-input?

**Yes — this document proposes no change to `CraftRecipe`'s shape.** Confirmed directly
(`docs/plan-1.8.18-storage-qol.md` §2.8, itself read from `source/world/crafting.h` in that
pass): `CraftRecipe` is `{name, input_item, input_count, output_item, output_count}` — one
ingredient, one result — and the header's own comment records that shaped and shapeless
multi-ingredient recipes were **already considered and rejected** for this project. The v1.8.19
bar reshapes how a single-input recipe list is *browsed and selected* (LCE's contribution, §2),
not what a recipe *is* — nothing in the XMB or LCE research requires a richer recipe shape, since
LCE's own "recipe-first, not ingredient-first" model (`ui-skin.md` §1, `interface.md` A1) is
already what a single-input, no-placement recipe list naturally is. **Growing `CraftRecipe` past
one ingredient stays a separate, larger, not-yet-proposed decision**, out of scope here exactly
as the storage document's own §2.8/§3.6 already stated it for the chest recipe.

---

## 6. Build order

`ui-skin.md` §11's six phases, carried forward with one reordering and one phase's cost revised
per §3.1's finding:

1. **Phase 0 — confirm the battery blink on real hardware.** Unchanged from `ui-skin.md` §11.
2. **Phase 1 — biome readout + neon toggle in the existing debug menu.** Unchanged from
   `ui-skin.md` §11; nothing in this document's own findings touches §7's design.

   > **⚠ CORRECTION [2026-09-03] — this phase is not work; both pieces already shipped, in
   > v1.8.8.** `source/debug/biomeborder.c`/`.h`/`biomeborder_draw.c`/`.h`/`biomeborder_test.c`
   > and `source/debug/biomeinfo.c`/`.h`/`biomeinfo_test.c` all exist; the toggle is a real,
   > available `DEBUG_TOGGLE` registered at `source/main.c:1394-1398`, wired at boot and drawn
   > every frame once enabled. `CHANGELOG.md`'s v1.8.8 entry documents both directly. This
   > "Phase 1" should be dropped from the build order entirely, not executed — a lane following
   > this document as written would re-implement two features that have shipped for several
   > versions. §7's design was accurate when this document cited it and rotted afterwards, not
   > wrong from the start; `ui-skin.md` §7 itself carries the fuller correction.
3. **Phase 2 — wire the existing block-list screen into a new, non-debug entry point, still
   reached from the pause menu or a temporary key for now, not yet the bar.** Revised from
   `ui-skin.md`'s "build a block list screen" to "add a second call site for the one that
   already exists" (§3.1) — cheaper than the original phase, and a good isolated playtest on its
   own: does the reused screen still read correctly outside the debug-menu context it was
   written for (§1.B item 3), and does the owner want the hotbar-visibility question (§7, HIS
   CALL 2) resolved before or after seeing it running.
4. **Phase 3 — the bar itself, Craft only**, exactly as `ui-skin.md` §11 Phase 3 — re-skin the
   existing crafting panel into the bar shape with one category, proving the d-pad axis feel
   before there is anything to switch between.
5. **Phase 4 — add Blocks as a second bar category**, now that Phase 2 has already proven the
   reused screen works from a non-debug door, and HIS CALL 2 (§7) has been answered so this
   phase knows which layout to build against.
6. **Phase 5 — resolve Inventory's role** per §4.3's proposal (default/always-visible state, not
   a third steppable category) or the owner's alternative if HIS CALL 3 (§7) goes the other way.
7. **Phase 6 — category-bar icon art** (`ui-skin.md` §9's `tools/make_ui_icons.py`), once the
   final category set from Phases 4-5 is stable — unchanged reasoning from `ui-skin.md` §11
   Phase 5, renumbered to account for the reordering above.

Each phase is independently playable and reversible, matching `ui-skin.md` §11's own stated
property and the storage document's §5.

---

## 7. HIS CALL

1. **D-pad left/right vs. shoulder L/R for bar category-switching.** `ui-skin.md` §5 proposes
   d-pad left/right (the XMB's own mapping, §2). `docs/research/interface.md` A1 documents that
   LCE itself used the shoulder buttons for tab-switching instead, with the stick/d-pad reserved
   for within-tab movement — a real, differently-sourced convention from a reference this
   project is also drawing on. **This document recommends keeping `ui-skin.md`'s d-pad choice**,
   for a concrete reason beyond precedent: this version's storage/QoL sibling
   (`docs/plan-1.8.18-storage-qol.md` §6.1) proposes repurposing the **shoulder** L/R for hotbar
   cycling during live gameplay. Since the bar's own d-pad left/right and the storage document's
   shoulder L/R are different physical controls, keeping the bar on d-pad avoids a direct button
   conflict between the two sibling versions' proposals — but this is exactly the kind of
   cross-version consistency call that is the owner's to confirm, not assume, especially since
   both documents are proposals, not yet built.
2. **Does the Blocks category reuse the existing block-list screen whole (hiding the hotbar
   while it has focus, §3.2/§4.3) or does it get a new, denser, hotbar-compatible layout built
   for it instead (real new work, not free)?** This is the fork `ui-skin.md` §5 gestured at
   without landing (§3.2's dangling-reference finding) and this document does not resolve it
   unilaterally, because it is a real feel/consistency trade the owner has to weigh: reusing the
   existing screen is materially cheaper and already proven, but means the hotbar visibly
   disappears and reappears as the player steps between bar categories, which may or may not
   read as jarring — only a look answers that (§1.B item 2).
3. **Does Inventory stay a third bar category, or does it become the bar's default/always-shown
   state with only Craft and Blocks as real steppable categories** (§4.3's proposal)? This
   changes what "the menus and inventory redrawn around a long horizontal bar... Craft/Blocks/
   Inventory categories" (paraphrasing the ROADMAP's own wording) is actually understood to mean,
   and is worth a direct answer rather than an assumption either way.

---

## LINES SOMEONE ELSE MUST ADD

Exact, load-bearing values for a future implementer, collected here rather than scattered
through the narrative — everything else above is intentionally descriptive, matching
`docs/plan-1.8.8-biome-identity.md`'s and this project's own house style.

- **A new call site** (file not chosen by this document — §4.4 names the shape, not the file)
  calling `blockListUiOpen()` / `blockListUiInput()` / `blockListBuild()`
  (`source/debug/blocklist.h`) from the bar's own dispatcher, in place of or alongside
  `app/debugmenu_ui.c:210`'s existing debug-menu call site — **`blocklist.c`/`.h` themselves are
  not edited**, only a second caller is added elsewhere.
- **`tools/make_ui_icons.py`** (per `ui-skin.md` §9) — three pictograms (Craft, Blocks, and
  Inventory only if HIS CALL 3 keeps it as a real category; two if not), in the same one-tile-
  wide-strip convention `tools/make_atlas.py` already uses.
- **Whatever new bar-dispatch header/source pair is created** (§4.4) needs its own entry in
  `tools/run_host_tests.sh`'s build list, matching how `debug/blocklist_test.c` and
  `scene/ui_layout_test.c` are already built there — not a specific line this document can quote
  without knowing that script's exact current content, flagged rather than guessed at.
