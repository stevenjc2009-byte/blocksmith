# Blueprint v1.9.1 — "The new interface"

Project root: `C:/Users/steve/Documents/3ds-project-folder/mc`. Base: the committed v1.9.0 tree (HEAD is d3ab9de = v1.8.20; the working tree carries v1.9.0 chest work across 36 files, and background lanes are still editing `title.c`, `ui.c`, net files). **v1.9.1 branches only after v1.9.0 is committed** — see §8 Phase 0.

Corrections to `docs/plan-1.9.1-interface.md` found while reading the tree: (a) there is no citro2d anywhere in `source/` — the batcher is `gfx/sprite.c` (own shader, `SPRITE_MAX_QUADS 1024` per **frame**, sprite.c:47); the "C2D_TextBuf/C2D_Prepare" facts map onto that file and are handled in §4. (b) `RECIPE_COUNT` is 7 (`world/crafting.h`), so `CRAFT_ROW_H = (120-28)/7 = 13 px` — the crafting panel already crossed the threshold `ui_layout.h`'s own comment names ("after which this has to become a scroll list"). (c) The atlas has **five** free slots, 58..62 (`gfx/atlas_tiles.h`; chest took 57), not six. (d) The debug block list's input map is `debugmenu_ui.c:195-210`, not a player-facing surface.

## 1. Success criteria

### 1A — measurable on the host (WSL gcc `-std=c11 -Wall -Wextra -Werror -O1`)

1. `sh tools/run_host_tests.sh` is green and prints two lines: `ui_layout self-test: PASS  N checks` with `N` equal to the new `UI_LAYOUT_TEST_EXPECTED_CHECKS` (moved from 6290 by the ADDED path, delta recorded in the pin comment — never fewer than 6290), and `barnav self-test: PASS  M checks` with `M == BARNAV_TEST_EXPECTED_CHECKS`.
2. Layout invariants asserted by `ui_layout_test.c`: strip `40..66` disjoint from hotbar `0..40`, from `GRID_Y 72`, `CHEST_ROW_Y 72`, `FURN_ROW_Y 72`; `barTabRect(i,n)` tiles `[0,294)` exactly for `n` in 1..4 with no gap or overlap; `barCloseRect()` is `{294,40,26,26}`; 7 list rows fit `[72,240)` with 2-px gaps and none overlaps the strip; for every kind and every `(row,col)`, `barCellFromPoint(centre of barCellRect(row,col))` returns `(row,col)`; every pre-existing furnace/chest/hotbar/pip check still passes unchanged.
3. Navigation invariants asserted by `barnav_test.c`: after any key sequence, `0 <= cat < count`, `0 <= row < rows`, `0 <= col < cols(row)`; B with a lift is `CANCEL` and never `BACK`; d-pad left/right in a list category switches category, in a grid moves the cell, on a stepper row steps; the stick edge detector fires once per deflection past 40 and re-arms below 24.
4. Seam greps (the anti-inert checks; each must be true on the integrated tree): `grep -c "\.keys_down" source/main.c >= 1`; `grep -c "inputMapSetMenuOwnsPad" source/main.c == 1` and `source/app/input_map.c == 1`; `grep -c "pauseMenuTouch" source/main.c == 1`; `grep -c "UI_SCR_CHEST\|UI_SCR_INVENTORY\|UI_SCR_FURNACE" source/ == 0`; `grep -c "barnav" tools/run_host_tests.sh >= 2`; `grep -c "_Static_assert(BAR_KEY_" source/scene/ui.c >= 1`.
5. Quad arithmetic (§4): worst bar screen ≤ 260 bottom-UI quads by count; never-frame ≤ 900 of 1024.
6. `sh tools/check_readme_current.sh` passes with the Controls table updated (B = open/close menu; L/R = tabs while the menu is open).

### 1B — playtest only (real 3DS, both models)

1. HUD: press B — the bar opens with the cursor on the in-hand hotbar slot; press B — it closes. Tap the strip under the hotbar — same.
2. With the bar open the player does not walk, jump, break, place, eat or look; the world keeps ticking (drops fall, other players move). Closing the bar restores all six on the next frame.
3. L/R (and left/right on CRAFT) switch tabs; d-pad moves the cell/row; A lifts/drops/withdraws/deposits/crafts exactly as a tap does; B cancels a lift first, then closes.
4. Open a chest (Y on the block): the bar opens on a third tab CHEST; walk-away/close removes the tab; same for FURNACE.
5. Crafting rows are 24 px with output and input icons and have/need counts; all 7 recipes fit without scrolling.
6. Pause (SELECT) shows a three-tab panel; d-pad, stick, and stylus all work; Controls/Debug still open their sub-screens.
7. Title shows PLAY / MULTIPLAYER / OPTIONS / SYSTEM tabs; every previous action is reachable in ≤ 2 inputs.
8. The debug "Frame timing" row shows `q<N>`; on the worst screen (HUD + pause SYSTEM tab + 3D on) `N` reads below 900.
9. No one-frame flash on open/close; 3D top screen unchanged.

## 2. Current state, with file:line evidence

### 2.1 Bottom-screen screens and their dispatch — `source/scene/ui.h`, `ui.c`

- `UiScreen { UI_SCR_HUD, UI_SCR_INVENTORY, UI_SCR_FURNACE, UI_SCR_CHEST }`; `UiState { screen, touch_prev, picked_slot }`; `UiInput { touch_down, touch_x, touch_y }` — the header states it carries no keys "deliberately"; `UiResult { inventory_open, furnace_open }` (no `chest_open`, flagged in the header as a deliberate scope cut) — `ui.h`.
- `uiUpdateDraw(ui, inv, block_icons, stats, in, furnace, chest)` at `ui.c:921-1149`: rising-edge `tap = in->touch_down && !ui->touch_prev`; NULL-pointer corrections (`UI_SCR_FURNACE`/`CHEST` with a NULL state fall back to `UI_SCR_INVENTORY`); tap dispatch per screen — HUD: `hudToggleRect()` opens the overlay; INVENTORY: `craftCloseRect()` returns to HUD, `craftRowRect(i)` crafts, slots lift/drop; FURNACE/CHEST: their close bars return to `UI_SCR_INVENTORY`. Draw: `spriteBegin(SCR_W,SCR_H); spriteTexture(fontTexture()); spriteRect(0,0,SCR_W,SCR_H,COL_BG)`, hotbar on every screen (rule at `ui.c:1051-1057`: "the player must be able to see what they are holding"), font pass, then the atlas pass if `block_icons`, `spriteEnd()`.
- Gestures reused unchanged by this version: `handleSlotTap(ui, inv, slot)` two-tap lift/drop/swap at `ui.c:160-193`; `handleHotbarSelect` at `201`; `handleCraftTap(inv, i)` at `206-215`; furnace one-tap deposit/withdraw (`furnaceDeposit` 319, `furnaceWithdraw` 344); chest transfers reuse the SAME two-tap gesture (no dedicated `chestWithdraw`/`chestDeposit` pair exists) — resolved locally by `placeStack()` (`ui.c:815-870`) and, with a session up, by the `UiChestTransferFn` network callback (`uiSetChestTransferFn`, `ui.h`) instead.
- No stack-split gesture exists (`grep split ui.c` finds only comments; `net/inv_bridge.h` has no split wrapper) — the bar reserves an event for it but adds none.

### 2.2 Geometry — `source/scene/ui_layout.h`, `ui_layout.c`

- Hotbar `HOTBAR_Y 0, HOTBAR_H 40`; grid `GRID_Y 40, GRID_H 80`; crafting `CRAFT_Y 120, CRAFT_H 120, CRAFT_CLOSE_H 28, CRAFT_ROW_Y0 148, CRAFT_ROW_H = (CRAFT_H-CRAFT_CLOSE_H)/RECIPE_COUNT` = **13 px at RECIPE_COUNT 7**; HUD toggle `HUD_TOGGLE_Y 40, H 32`; status rows from 78; pips `HUD_PIPS_Y0 200`, two rows 12 apart.
- Furnace: close `40/26`, slot row `FURN_ROW_Y 72`, slots at x 20/76/228, arrow 132x80 at y 86, burn bar y 116, hints 136/148, `FURN_GRID_Y 160`. Chest: close `40/26`, `CHEST_ROW_Y 72`, hints 118/130, `CHEST_GRID_Y 160`. Both keep the bag at 160 so it "does not move between containers" (header comment).
- `ui_layout.c` is 245 lines of rect constructors and hit-tests; `hitInventorySlot(x,y,overlay_open)` at `:40-51` reaches the grid only when `overlay_open`.
- `ui_layout_test.c`: pin `UI_LAYOUT_TEST_EXPECTED_CHECKS 6290` at `:62`; checks that encode the geometry this version retires: `:187-194` (points inside the crafting band are not slots, written against `CRAFT_Y`), `:203-211` (`hitInventorySlot(0, HOTBAR_H, true) == INV_HOTBAR_SLOTS` — i.e. the grid begins at y 40), `:226-235` (written against `GRID_Y`/`GRID_H` macros). Furnace checks `:505-662` use `furnCloseRect()`/`FURN_CLOSE_*` macros; `:378/:391` use `HUD_TOGGLE_*` macros.

### 2.3 Input in the game loop — `source/main.c`

- `hidScanInput` 5703; `down = hidKeysDown()` 5704; `held` 5709; START quits 5714; touch read 5742-5753 (`UiInput touch = {.touch_down…, .touch_x, .touch_y}`; `touch_press` rising edge); SELECT toggles pause 5762; `pauseMenuInput(down, …)` 5770-5772 gated on `!s_remap_open && !s_debug_open`; `paused = pauseMenuOpen()` 5821; L/R render distance 5870-5871 read raw `down`; eat behind `inputKey(ACTION_EAT)` 5895-5896; `cameraUpdate` 5984 and `playerUpdate` 5986-5987 both under `if (!paused)`; furnace open 6656-6667 and chest open 6686-6696 on `inputKey(ACTION_PLACE)`; `interactEdit(&it, …, edit_down, edit_held, …)` 6752 with masks built at 6722-6751.
- `playerUpdate` reads the pad itself: `hidKeysHeld()` at `scene/player.c:103`, movement via `held & inputKey(ACTION_MOVE_*)` at `:178-181`, jump via `inputKey(ACTION_JUMP)` and `hidKeysDown()` at `:201-202`. `cameraUpdate` reads `hidCircleRead` at `scene/camera.c:81` and `hidKeysHeld()` at `:118`. `inputKey()` is `app/input_map.c:32`, set from options at `main.c:4771`.
- `ures.inventory_open` has **no reader** in `main.c`: the inventory overlay does not gate any world input today — the player walks, jumps and breaks with the overlay open.
- B is unused in live gameplay (`KEY_B` has no hit in `main.c`'s loop or `player.c`; `options.h` allows bindings only on A/X/Y/d-pad/ZL/ZR, so B can never be bound to an action).
- Draw phase 7508-7597: `drawBottomUi(…, paused ? &blank : &touch)` 7520 (builds `UiStats`, unpacks `FurnaceState`/`ChestState` from `blockStateGet` into stack locals with `BLOCKSTATE_PAYLOAD_BYTES 16` buffers, calls `uiUpdateDraw` at 3252, packs back, `if (!ures.furnace_open) s_furnace_open = false;` 3273, `if (s_ui.screen != UI_SCR_CHEST) s_chest_open = false;` 3291); battery own pass 7525-7528; `if (paused) { pauseMenuDraw(); if (s_remap_open) … remapUiUpdate(…) else if (s_debug_open) … debugMenuUiUpdate(…) }` 7547-7595 — the pause page is fully drawn under the remap/debug panels that cover it.

### 2.4 Pause menu — `scene/pausemenu.c/.h`

d-pad/A/B only, no touch; two pages (main: Resume/Options/Quit; options: dist, 3D, volume, Controls, Debug, memory readout); `pauseMenuInput(uint32_t down, int* dist_step, bool* stereo_toggle)` `:91`, `pauseMenuDraw(const PauseStats*)` `:187` with its own `spriteBegin/End`, panel `PANEL_X 26, PANEL_Y 24` (268x192), `ROW_H 22`; `pauseMenuOpen/Toggle/Close/TakeVolumeChanged` `:64-91`.

### 2.5 Title — `scene/title.c/.h`

`TitleInput { keys_down, keys_held, touch_down, touch_x, touch_y }`; screens MAIN (4 stacked buttons), WORLD_SELECT (`drawWorldSelect` `:444-494`, 4 visible 32-px rows + NEW WORLD + BACK), OPTIONS_GENERAL (`drawOptionsGeneral` `:565-641`, `GEN_ITEM_COUNT 8`, three 22-px buttons — `OPT_BTN_H`'s own comment at `:120-123` says the right fix is "CONTROLS/UPDATE/BACK on their own screen, or a scrolling list"), BINDINGS, MULTIPLAYER (dense text rows), UPDATE, VERSION_HISTORY. Helpers `uiButton`, `settingRowStepper`, `settingRowToggle`. Rename/delete from v1.9.0 6.3 are not yet in `title.c` (0 hits) — an in-flight lane is adding them.

### 2.6 Sprite budget — `gfx/sprite.c`

`SPRITE_MAX_QUADS 1024` per frame across all batches (`:47`); overflow flushes and **wraps** the cursor, overwriting geometry the GPU has not drawn (`:242-246`); the counter was removed in v1.8.7. `spriteBegin` binds the sprite program itself (`:163`) and the world pass rebinds its own, so the "2D steals the 3D shader" hazard is already handled per pass — new code must only ever draw inside `spriteBegin/spriteEnd`. Budget comment `:22-46` estimates ~790 of 1024 for a never-seen worst frame and defers shrinking to "the UI redesign".

### 2.7 Atlas — `gfx/atlas_tiles.h`, `tools/make_atlas.py`

64-slot strip; 0..47 blocks, 48..56 item icons, 57 chest top, **58..62 free (five)**, 63 reserved. Append-only, paint before naming, `block_tiles_check.c` static-asserts pairs.

## 3. Design — every decision resolved

**D1. Scope of "the menus and inventory".** Three surfaces get the bar: the in-game bottom screen (replaces INVENTORY/FURNACE/CHEST with one `UI_SCR_BAR`), the pause panel, and the title screen. Leaf screens (key bindings, update, version history, remap, debug menu) keep their layouts and are reached from bar rows. *Rejected:* merging pause into the in-game bar — pause is a modal that stops the local sim and hosts remap/debug; the bar is a live-world screen. Two bars with one chrome is the honest shape.

**D2. Bar categories in-game: INVENTORY · CRAFT · [CHEST | FURNACE].** The container tab exists only while `chest`/`furnace` is non-NULL; a chest and a furnace can never be open at once (`main.c` opens one per PLACE press). Category index 2 is always "the container". *Rejected:* a "BLOCKS" category reusing the debug block list — it has no commit action in survival (nothing to do with a registry row), its layout hides the hotbar (`BL_GRID_Y 26` owns the screen, breaking the `ui.c:1051` rule), and its page costs ~368 quads. It stays in the debug menu untouched. This resolves HIS CALL 2 by removing its cause.

**D3. Inventory is the first tab and the default focus** (HIS CALL 3). The bag grid is content under the strip; nothing else "is" the inventory. *Rejected:* inventory as a persistent background with categories on top — with one 320x240 surface the bag and a recipe list cannot share the height at 40-px cells.

**D4. Type scale.** The font is 5x7 with integer scales only (`gfx/font.h`), so the scale has exactly two sizes and two weights: T1 = scale 2 `COL_TEXT` (wordmark, focused-cell item name); T2 = scale 1 `COL_TEXT` (tab labels, row labels, values); T3 = scale 1 `COL_TEXT_DIM` (unfocused tab labels, hints, footers). Colours are `ui.c`'s existing palette; the pause/title keep theirs. *Rejected:* scale-2 for the focused tab — "INVENTORY" is 108 px at scale 2 and would not fit a 98-px tab, and a tab that grows on focus moves its neighbours' touch targets (`pausemenu.c:224` names why that reads as a glitch).

**D5. Bar geometry (bottom screen, pixels).**
- Hotbar `0..40` unchanged on every screen.
- Strip `BAR_STRIP_Y 40, BAR_STRIP_H 26` (= the old furnace/chest close bars, so `FURN_ROW_Y`/`CHEST_ROW_Y 72` keep a 6-px gap). Tabs tile `[0,294)` with `w = (i+1)*294/n - i*294/n` (n = 2 → 147 px; n = 3 → 98 px; "INVENTORY" is 54 px at scale 1). Close tab `BAR_CLOSE_W 26` at `294..320`, label "X". Focused tab: `COL_PANEL_HI` fill + 2-px `COL_ACCENT` underline at y 64..66 + T2 label; others `COL_PANEL` + T3; 1-px dividers.
- HUD toggle strip shrinks `HUD_TOGGLE_H 32 → 26` so the band under the hotbar is the same band open or closed; label "INVENTORY   B".
- INVENTORY content: `GRID_Y 40 → 72` (grid `72..152`); detail band `BAR_DETAIL_Y 160`: focused/lifted item name at T1 y 164, "x N" T2 y 186; pips unchanged at `HUD_PIPS_Y0 200/212` (vitals visible here and on the HUD); hint line T3 at y 228 "A LIFT/DROP  B CLOSE  L/R TAB".
- CRAFT content: `BAR_LIST_Y0 72, BAR_LIST_ROW_H 24, BAR_LIST_VISIBLE 7`; row i = `{4, 72+24i, 312, 22}`; fill `COL_CRAFT_OK` when makeable else `COL_PANEL`; focused adds `COL_PANEL_HI` + 3-px accent left edge; 16-px output icon at x 8, "NAME xN" at x 30; right side "IN xK (have)" ending at x 292 with a 16-px input icon at 296. Scrolls (cursor-follow) only when `RECIPE_COUNT > 7`. *Rejected:* keeping the 13-px rows — below the readable floor the header's own comment sets.
- CHEST / FURNACE content: byte-identical to today's `CHEST_*`/`FURN_*` geometry (row 72, hints, bag 160). Only the close bar becomes the strip.
- Touch hit-boxes: tabs ≥ 98x26 (≈19x5 mm), close 26x26, slots 40x40, list rows 312x22, all ≥ the existing 22-px minimum the project already ships.

**D6. Controller map while the bar is open.** L/R = previous/next tab (clamped, no wrap). D-pad up/down = row (wraps; column restored from `pref_col` clamped to the row's width). D-pad left/right = move within a grid row; on a list category (CRAFT) = switch tab; on a stepper row (pause/title options) = step the value. A = commit (synthesized as a tap at the cell's centre — see D8). B = cancel a lift if one exists, else close the bar. X = `BAR_EV_ALT`, unmapped in v1.9.1 (reserved for the stack-split gesture v1.9.0 still owes). Circle pad mirrors the d-pad through `barStickEdge` (deadzone 40 of 156, re-arm 24), for menus only. *Rejected:* d-pad-only categories (spec HIS CALL 1) — grids need left/right for cells; *rejected:* shoulder-only categories — a list with dead left/right feels broken; *rejected:* a separate "bar level" focus — XMB has none, and B-as-back would then need two meanings.

**D7. Opening/closing.** B opens from the HUD (the only free button; never bindable per `options.h`), B closes; the HUD strip and the close tab are the stylus doors. `uiOpenFurnace/uiOpenChest` keep their names: `screen = UI_SCR_BAR, nav.cat = 2`. *Rejected:* touch-only open (spec) — a bar you can only enter by stylus is half a design.

**D8. Modal input.** While `s_ui.screen == UI_SCR_BAR`, `inputKey()` returns 0 (`inputMapSetMenuOwnsPad(true)`), so `playerUpdate` keeps physics but gets no move/jump, `interactEdit` gets no break/place, eat is off; `cameraUpdate` is skipped (freezes look, no physics inside it); raw L/R render-distance lines get `&& !bar_open`. The world is **not** paused (multiplayer keeps flowing). Commit-by-A is implemented as a synthetic tap at `barCellRect()`'s centre fed into the existing touch dispatch, so touch and controller can never diverge. *Rejected:* skipping `playerUpdate` — a player frozen mid-jump is visible to other clients; *rejected:* new `playerSetInputEnabled`/camera APIs — two more files for what one guard in `input_map.c` does.

**D9. Pause bar.** Same scrim/panel; strip inside the panel at y 30 (h 22, three 89-px tabs) — GAME (Resume, Quit to title) · OPTIONS (Render dist ±, 3D on/off, Sound ±) · SYSTEM (Controls >, Debug >, memory readout under the rows). Rows from y 58, `ROW_H 22`; footer "A SELECT  B RESUME  L/R TAB". New `pauseMenuTouch()`; every existing function keeps its signature and `PauseAction` values. *Rejected:* adding the SHADING toggle here — needs a new out-poll and a `main.c` seam; not this version.

**D10. Title bar.** Wordmark y 4 (T1); strip y 22..48, four 80-px tabs PLAY · MULTIPLAYER · OPTIONS · SYSTEM; content rows 28 px from y 52, six visible; footer y 228. PLAY = "NEW WORLD" + world rows (scroll, plus whatever verbs v1.9.0's rename/delete lane lands on a row); MULTIPLAYER = existing body shifted to y ≥ 52, BACK button removed (B/tabs); OPTIONS = the five settings rows + CONTROLS > (six rows, steppers consume left/right; `optionsSave` on leaving the category — the one disk write, unchanged in spirit); SYSTEM = CHECK FOR UPDATE, VERSION HISTORY, QUIT. Leaf screens unchanged. This is the redesign `OPT_BTN_H`'s comment asks for.

**D11. Icons.** None from the atlas: tabs are text; the five free slots (58..62) stay free because v1.9.2 redstone needs more tiles than exist. *Rejected:* `tools/make_ui_icons.py` pictograms (spec) — 16-px marks at 5.2 px/mm are unreadable without a label anyway; *rejected:* a second UI texture — a third bind/flush per frame and a new asset pipeline for glyphs the labels already carry.

**D12. Top screen.** Unchanged: the world (stereo, 400x240 per eye). No bar content up there — every top quad is drawn twice in 3D and the bottom screen is the full second surface. Title's release-notes panel unchanged.

**D13. Sound.** The existing `SFX_UI_TAP` on every commit/tab change; no new SFX.

## 4. 3DS constraints check

- **Quads per frame (1024 cap, wrap on overflow).** Counted from D5, one quad per glyph/rect/icon: INVENTORY ≈ 1 bg + 36 hotbar + 31 strip + 52 grid + 48 detail/hints + 26 pips + 24 icons = **218**; CRAFT ≈ 1 + 36 + 31 + 7×24 rows + 8 + 14 icons = **258**; CHEST ≈ 1 + 36 + 31 + 24 chest + 60 hints + 52 bag + 32 icons = **236**; FURNACE ≈ 230. The HUD (unchanged, ≈ 257 with 7 status rows) stays the bottom-screen maximum. Pause bar worst (SYSTEM with memory lines) ≈ 270 (today ≈ 180). Never-frame = HUD 257 + battery 6 + pause 270 + debug menu ≈ 200 + top (15 name tags + reticle, stereo) 338 = **1071 → over**; with seam S9 (do not draw the pause page under a remap/debug panel that covers it) it is **≈ 800**, and the ordinary worst (bar CHEST + battery + top) is ≈ 580. `SPRITE_MAX_QUADS` is not changed. Agent A adds a per-frame counter with a reader (`spriteFrameQuads()`), surfaced on the timing row (S10), so 1B.8 is measured, not estimated.
- **Shader rebind.** All new drawing is inside existing or new `spriteBegin/spriteEnd` pairs placed where `pauseMenuDraw` already runs; nothing calls `C3D_BindProgram` or draws after the bottom-screen passes; `spriteFrameBegin()` stays once per frame in `main.c`.
- **VRAM delta: 0** (no textures). **Linear memory delta: 0** (buffer unchanged).
- **Stack (32 KB).** New structs: `BarNav` 16 B + `row_mem[6]`; `BarCatSpec[4]` ≈ 4×20 B on `uiUpdateDraw`'s frame; `char buf[64]` per row; `BarStick` 2 B static. No new local over 256 B; `BlockListOp[64]` (2 KB) is never on the stack because D2 drops Blocks. Rule for implementers: any new struct over 1 KB is `static`.
- **Atlas: 0 new tiles; 58..62 remain free.** No `make_atlas.py` change, no `block_tiles_check.c` change.
- **Console build hygiene.** `barnav_test.c` sits under `source/scene/` and is globbed by the Makefile (`Makefile:26`, `:906`), so it is whole-file `#ifndef __3DS__` with `typedef int barnav_test_host_only_t;` in the `#else`. Every object depends on every header (`Makefile:1068`), so header edits trigger full rebuilds — integrator plans one clean build per phase, not per edit.
- **`-Wtype-limits`.** `cols[]` are `uint8_t`; no `>= 0` tests on unsigned; tab/cat counts are `int`.

## 5. Wire/server implications

**None.** No new opcode (S→C or C→S), no registry row, no new tile, so `registryCrc16()` is unchanged and no server release is needed. Every mutation the bar performs is the same call the touch UI makes today: `invBridgeMoveUnits/SwapSlots/SelectHotbar/Craft/Add/Remove`, the v1.9.0 chest transfer path (`placeStack()` locally, or the `UiChestTransferFn` callback with a session up — there is no `chestWithdraw`/`chestDeposit` pair), and `furnaceDeposit/Withdraw`. The server never learns which screen is open. If the in-flight "client net multiplayer chests" lane changes the chest helpers' wire behaviour, the bar inherits it by calling the same functions.

## 6. Work partition (exclusive file lists) and exchanged signatures

| Agent | Owns (exclusively) | Delivers |
|---|---|---|
| **A — Nav core** | `source/scene/barnav.h`, `source/scene/barnav.c`, `source/scene/barnav_test.c`, `tools/run_host_tests.sh` (append one stanza only), `source/gfx/sprite.h`, `source/gfx/sprite.c` | pure navigation model + stick edge + scroll helper; frame quad counter |
| **B — Layout** | `source/scene/ui_layout.h`, `source/scene/ui_layout.c`, `source/scene/ui_layout_test.c` | bar geometry, kinds, cell round-trip; pin re-baseline |
| **C — In-game bar screen** | `source/scene/ui.h`, `source/scene/ui.c` | `UI_SCR_BAR`, drawing, synthetic-tap commit, result flags |
| **D — Pause bar** | `source/scene/pausemenu.h`, `source/scene/pausemenu.c` | three-tab pause panel, touch |
| **E — Title bar** | `source/scene/title.h`, `source/scene/title.c` | four-tab title |
| **Integrator (sequential, last)** | `source/main.c`, `source/app/input_map.h`, `source/app/input_map.c`, `README.md`, `CHANGELOG.md`, `docs/ROADMAP.md`, `source/version.h` | every seam in §6.3 |

Nobody touches `debugmenu_ui.c`, `remap_ui.c`, `blocklist.*`, `make_atlas.py`, `atlas_tiles.h`, `title_nav.*`, `crafting.*`, `chest.*`, `furnace.*`.

### 6.1 `scene/barnav.h` (Agent A) — pinned, C, D, E code against this text

```c
#define BAR_KEY_A 0x0001u  /* == KEY_A  */   #define BAR_KEY_B 0x0002u  /* == KEY_B */
#define BAR_KEY_DRIGHT 0x0010u  #define BAR_KEY_DLEFT 0x0020u
#define BAR_KEY_DUP    0x0040u  #define BAR_KEY_DDOWN 0x0080u
#define BAR_KEY_R 0x0100u  #define BAR_KEY_L 0x0200u  #define BAR_KEY_X 0x0400u  #define BAR_KEY_Y 0x0800u
#define BARNAV_MAX_CATS 6
#define BARNAV_MAX_ROWS 8
typedef struct {
    int     rows;                   /* cursor rows; 0 = no cursor content */
    int     uniform_cols;           /* >0: every row has this many columns (a list is 1); cols[] ignored */
    uint8_t cols[BARNAV_MAX_ROWS];  /* per-row widths when uniform_cols == 0 (rows <= BARNAV_MAX_ROWS) */
    bool    horizontal_is_content;  /* left/right: true = move/step in the row; false = switch category */
} BarCatSpec;
typedef struct { int cat, row, col, pref_col; uint8_t row_mem[BARNAV_MAX_CATS]; } BarNav;
typedef enum { BAR_EV_NONE, BAR_EV_CAT, BAR_EV_MOVE, BAR_EV_COMMIT, BAR_EV_ALT,
               BAR_EV_STEP_LEFT, BAR_EV_STEP_RIGHT, BAR_EV_CANCEL, BAR_EV_BACK } BarEvent;
void     barNavReset(BarNav* n, int cat, int row, int col);
BarEvent barNavInput(BarNav* n, uint32_t keys, const BarCatSpec* cats, int cat_count, bool has_lift);
void     barNavClamp(BarNav* n, const BarCatSpec* cats, int cat_count);   /* cat >= count -> 0 */
void     barNavSetCat(BarNav* n, int cat, const BarCatSpec* cats, int cat_count);
void     barNavSetCell(BarNav* n, int row, int col, const BarCatSpec* cats, int cat_count);
int      barNavScrollFor(int cursor_row, int scroll, int visible, int rows);
typedef struct { int8_t dir_x, dir_y; } BarStick;
uint32_t barStickEdge(BarStick* s, int dx, int dy);   /* BAR_KEY_D* rising edges; fire >= 40, re-arm < 24 */
```
Priority inside `barNavInput`, one event per call: B → CANCEL/BACK; A → COMMIT; X → ALT; L/R → CAT (clamped, restores `row_mem[cat]`); up/down → MOVE (wrap, `col = min(pref_col, cols-1)`); left/right → per `horizontal_is_content` (grid: MOVE, clamp, sets `pref_col`; 1-col: STEP_*; else CAT).

Also A: `int spriteFrameQuads(void);` in `sprite.h` — quads submitted in the previous frame (reset in `spriteFrameBegin`, counted in `spriteQuad`).

### 6.2 `scene/ui_layout.h` additions (Agent B) — pinned

```c
#define BAR_STRIP_Y 40   #define BAR_STRIP_H 26   #define BAR_CLOSE_W 26
#define BAR_TABS_W (SCR_W - BAR_CLOSE_W)   #define BAR_MAX_TABS 4
#define BAR_HIT_NONE (-1)   #define BAR_HIT_CLOSE (-2)
#define BAR_LIST_Y0 72   #define BAR_LIST_ROW_H 24   #define BAR_LIST_VISIBLE ((SCR_H - BAR_LIST_Y0) / BAR_LIST_ROW_H)
#define BAR_DETAIL_Y 160
/* changed values: GRID_Y 40 -> 72 ; HUD_TOGGLE_H 32 -> 26 ; FURN_CLOSE_Y/H and CHEST_CLOSE_Y/H become aliases of BAR_STRIP_Y/H (same numbers) */
/* retired: CRAFT_Y, CRAFT_H, CRAFT_CLOSE_H, CRAFT_ROW_Y0, CRAFT_ROW_H, craftCloseRect(), craftRowRect() */
typedef enum { BAR_KIND_INVENTORY, BAR_KIND_CRAFT, BAR_KIND_CHEST, BAR_KIND_FURNACE } BarKind;
URect barStripRect(void);
URect barTabRect(int i, int count);
URect barCloseRect(void);
int   hitBarStrip(int x, int y, int count);              /* tab index, BAR_HIT_CLOSE, or BAR_HIT_NONE */
URect barListRowRect(int visible_row);
int   barBuildKinds(bool has_chest, bool has_furnace, BarKind out[BAR_MAX_TABS]);   /* 2 or 3 */
const char* barKindLabel(BarKind k);                     /* "INVENTORY" "CRAFT" "CHEST" "FURNACE" */
int   barKindRows(BarKind k, int list_len);              /* 3, list_len, 4, 4 */
int   barKindCols(BarKind k, int row);                   /* 8; CRAFT 1; FURNACE row 1 -> 3 */
bool  barKindHorizontalIsContent(BarKind k);             /* CRAFT false, grids true */
URect barCellRect(BarKind k, int row, int col, int scroll);
bool  barCellFromPoint(BarKind k, int x, int y, int scroll, int* row, int* col);
```
Row order per kind (top to bottom): row 0 = hotbar for every grid kind; INVENTORY rows 1-2 = `gridSlotRect`; CHEST row 1 = `chestSlotRect`, rows 2-3 = `chestGridSlotRect`; FURNACE row 1 cols 0/1/2 = `furnInputRect/furnFuelRect/furnOutputRect`, rows 2-3 = `furnGridSlotRect`; CRAFT row r = `barListRowRect(r - scroll)` (zero rect when off-window).

### 6.3 `scene/ui.h` (Agent C) — pinned, consumed by the integrator

```c
typedef enum { UI_SCR_HUD, UI_SCR_BAR } UiScreen;
typedef struct { UiScreen screen; BarNav nav; int list_scroll; bool touch_prev; int picked_slot; bool opened_this_frame; } UiState;
typedef struct { bool touch_down; int touch_x, touch_y; uint32_t keys_down; } UiInput;
typedef struct { bool bar_open; bool furnace_open; bool chest_open; } UiResult;   /* *_open = pointer non-NULL && screen == UI_SCR_BAR */
/* unchanged: uiInit, uiOpenFurnace, uiOpenChest, UiStats, uiUpdateDraw(...) */
```
`ui.c` carries `_Static_assert(BAR_KEY_A == KEY_A && BAR_KEY_B == KEY_B && BAR_KEY_L == KEY_L && BAR_KEY_R == KEY_R && BAR_KEY_DUP == KEY_DUP && BAR_KEY_DDOWN == KEY_DDOWN && BAR_KEY_DLEFT == KEY_DLEFT && BAR_KEY_DRIGHT == KEY_DRIGHT, "barnav key bits mirror libctru");`.

`scene/pausemenu.h` (Agent D) adds only: `void pauseMenuTouch(bool press, int x, int y);` — call immediately before `pauseMenuInput`. `scene/title.h` (Agent E): no signature change.

`app/input_map.h` (Integrator) adds: `void inputMapSetMenuOwnsPad(bool owns);` — when true, `inputKey()` returns 0 for every action.

### 6.4 Integrator seams — every hand-wired line, none assumed

| # | File:site | Change |
|---|---|---|
| S1 | `main.c:5744` UiInput initializer | add `.keys_down = menu_down`; `blank` stays `{0}` |
| S2 | `main.c` after 5709 | `circlePosition cp; hidCircleRead(&cp); const uint32_t menu_down = down \| barStickEdge(&s_stick, cp.dx, cp.dy);` new `static BarStick s_stick;`, `#include "scene/barnav.h"` |
| S3 | `main.c` right after `hidScanInput` 5703 | `const bool bar_open = (s_ui.screen == UI_SCR_BAR); inputMapSetMenuOwnsPad(bar_open);` (before any `inputKey` read; `input_map.c` gets the static flag + guard) |
| S4 | `main.c:5984` | `if (!paused && !bar_open) cameraUpdate(...)`; `playerUpdate` at 5987 unchanged |
| S5 | `main.c:5870-5871` | `if (!paused && !bar_open && (down & KEY_L))` and the R line |
| S6 | `main.c:3291` | `if (!ures.chest_open) s_chest_open = false;` (old line no longer compiles — good) |
| S7 | `main.c:5770-5772` | `pauseMenuTouch(touch_press, touch.touch_x, touch.touch_y);` then `pauseMenuInput(menu_down, ...)` |
| S8 | `main.c` title loop (grep `TitleInput`) | `.keys_down = down \| barStickEdge(...)` |
| S9 | `main.c:7547` | `if (!s_remap_open && !s_debug_open) pauseMenuDraw(&pstats);` — verify first that `remap_ui.c`/`debugmenu_ui.c` draw their own scrim (both open a full panel at the pause rect); if either does not, keep the call and accept the quad cost |
| S10 | `main.c` timing string in `drawBottomUi` | append `" q%d", spriteFrameQuads()` |
| S11 | `tools/run_host_tests.sh` | Agent A appends the `barnav` stanza (gcc `barnav.c barnav_test.c`); integrator confirms the run prints both self-test lines |
| S12 | `README.md` Controls, `CHANGELOG.md`, `docs/ROADMAP.md` v1.9.1 tick, `version.h` → 1.9.1 | release chores; `tools/check_readme_current.sh` |
| S13 | boot | none new (`uiInit` unchanged); confirm `_Static_assert` from 6.3 is present — the console build proves it |

## 7. Per-implementer test plan with red arms

**A — `barnav_test.c`** (host, own `main`, CHECK macro, `BARNAV_TEST_EXPECTED_CHECKS` pin). Green arms: clamp for every cat count 1..6, wrap rows, clamp cols, `pref_col` memory across a 3-wide furnace row, `row_mem` on tab return, scroll follow (up and down), stick fire/re-arm/diagonal. Red arms (each breaks a different named check): (1) delete the `cat >= count → 0` clamp → "clamp: cat 2 of 2 -> 0"; (2) make DLEFT return MOVE on a list category → "list: DLEFT is CAT"; (3) stop restoring `pref_col` on vertical moves → "furnace: col 7 up 2 down 7"; (4) return BACK for B when `has_lift` → "B while lifting is CANCEL". Sprite counter: no host arm (device: 1B.8).

**B — `ui_layout_test.c`**. Keep all 6290 checks; retarget exactly the ones encoding retired geometry: `:192-194` (`CRAFT_Y` → `BAR_STRIP_Y` and `BAR_DETAIL_Y`: "the strip and the detail band are not slots"), `:209-211` (`HOTBAR_H` → `GRID_Y`: "the grid starts at GRID_Y, and the strip row above it is not a slot"). Add tests: strip/tab tiling, close tab, list rows, kinds/rows/cols tables, cell round-trip for all kinds, `barBuildKinds` (2/3, never both containers, container always index 2), strip disjoint from every row constant. Re-pin through the ADDED path; the pin comment records "6290 → N: +k added in testBar*, 6 retargeted in testBoundaryPixels for GRID_Y 40→72 and the crafting band's retirement". Red arms: (1) `w = BAR_TABS_W / count` (truncation) → "tabs tile [0,294) exactly, n=3"; (2) close rect x off by one → "close hit at (307,53), miss at (293,53)"; (3) swap FURNACE cols 1↔2 in `barCellRect` → "furnace round trip (1,1)"; (4) `BAR_LIST_Y0` 72 → 70 → "list rows disjoint from strip".

**C — `ui.c`** (console-only; its logic is deliberately pushed into A and B so the host arms above cover it). Device arms with the observation that fails: (1) drop `opened_this_frame` → B opens and closes in one press (bar never shows); (2) forget `barNavClamp` after `barBuildKinds` → close a chest while on its tab: cursor on a tab that no longer exists, `nav.cat` 2 of 2; (3) synthesize the tap at the rect origin instead of centre → A on a FURNACE slot misses (`ptInRect` on the border); (4) set `chest_open` from `nav.cat == 2` instead of `chest != NULL` → switching to CRAFT with a chest open closes the chest (`s_chest_open` false) and the tab vanishes.

**D — `pausemenu.c`**: (1) forget `pauseMenuTouch` consumption → taps do nothing (also caught by 1A.4 if the call is missing); (2) memory readout drawn on every tab → quads readout jumps on GAME tab; (3) OPTIONS category `horizontal_is_content` false → left/right switch tabs instead of stepping Render dist; (4) `PauseAction` value renumbered → REMAP opens Debug.

**E — `title.c`**: (1) `optionsSave` not called on category leave → settings lost on quit; (2) world list scroll not clamped → 7th world unreachable; (3) leaf screen B returns to cat 0 instead of remembered → Controls > B lands on PLAY; (4) MULTIPLAYER content not offset by 28 → rows under the strip.

## 8. Build order / phase gates

- **Phase 0 (gate: git).** v1.9.0 committed, including the in-flight rename/delete title lane and chest net lanes. Nobody starts on `ui.c`, `title.c`, `main.c` before this; A and B may start immediately on a v1.9.0-based branch since their files are not in those lanes' diff (`ui_layout.*` is in the v1.9.0 diff — B waits too).
- **Phase 1 (host-verifiable): A + B in parallel.** Gate: `run_host_tests.sh` green with both self-test lines, pins recorded. D may start with A (needs only `barnav.h`).
- **Phase 2 (console, no playtest yet): C + D + E in parallel against the pinned headers.** Gate: each compiles in isolation once A/B are merged (`make` — the integrator runs the console build, not the agents).
- **Phase 3 (integrator): S1-S13**, one clean console build, seam greps (1A.4) all true.
- **Phase 4 (playtest, both models): 1B.1-1B.9.** Gate for release: 1B.8 quads readout < 900 on the worst screen; `check_readme_current.sh`; `make clean && make cia BS_PSK=...`.
- E (title) is independently shippable: if it slips, v1.9.1 ships in-game + pause bars and E lands as 1.9.1.1 — the roadmap line still holds ("menus and inventory") because pause is a menu.

## 9. Size estimate and riskiest item

| Agent | Lines (add/del) | Hours |
|---|---|---|
| A | barnav.h 120, barnav.c 230, barnav_test.c 350, sh +12, sprite +15 | 6 |
| B | ui_layout.h +140/−30, ui_layout.c +140/−10, test +360/−10 | 6 |
| C | ui.h +40/−20, ui.c +460/−260 | 12 |
| D | pausemenu.c +230/−130, .h +8 | 5 |
| E | title.c +520/−320, .h +8 | 12 |
| Integrator | main.c +60/−15, input_map +14, docs/README/CHANGELOG +90 | 6 (incl. builds/playtest loop) |

≈ 3,400 lines touched across 16 files, ≈ 47 agent-hours; three calendar days with A/B/D on day 1, C/E on days 1-2, integration and playtest on day 3.

**Riskiest item:** Agent C plus seam S3 together. `ui.c` is console-only glue that has to carry three shipped screens' exact semantics (two-tap lift, furnace one-tap, chest one-tap) onto one screen without a host test, and the modal mask lives in a file (`main.c`/`input_map.c`) that C does not own. Forgetting S3 ships a bar that opens by stylus, takes buttons, and lets the player keep walking while "in" the menu — every host test green. Mitigations built in: the `keys_down` field and the removal of `UI_SCR_CHEST` make `main.c` fail to compile until S1/S6 are done; the seam greps in 1A.4 catch S3/S7/S11; commit-by-synthetic-tap means the controller path cannot drift from the touch path.
