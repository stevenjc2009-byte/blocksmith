# v1.8.12 — Ores: what generates, where, what it takes, what it gives

This document answers what `docs/VERSION-LIST.md`'s own v1.8.12 entry says has "no
dedicated research brief... yet": exactly which ores, at what Y-bands, in what
quantities, needing what tool, dropping what. It builds directly on top of
`docs/plan-1.8.11-caves.md` — that document is settled and is not restated or
contradicted here; ore placement is designed as a pass that runs *after* it, reading
its output, exactly as `plan-1.8.11-caves.md` §3.4 already anticipated ("ore
placement runs after the carve mask has already been applied to the column's blocks
... no coupling to the carve pass's internals is needed beyond reading the finished
block grid").

This document does not implement anything. Nothing under `source/`, `tests/`, or
`tools/` was touched to produce it — see the closing "LINES SOMEONE ELSE MUST ADD"
section for exactly what a future change needs to add, file and line.

**Provenance convention**, matching the caves document: every number below is tagged
**[research]** (cited web source), **[codebase]** (read directly in this repository,
file:line given), or **[proposal]** (this document's own choice, not yet decided by
anyone else). Nothing is presented as settled fact unless it is.

Of the three documents in this batch, this is the one that reads as a build plan, not
a scoping study or a feasibility study — the risk here is low, the numbers are
well-precedented, and the codebase already has an explicit integration marker waiting
for exactly this feature (`mining.c:16-26`, see §5).

---

## 1. What the player experiences

Digging into stone below the surface, veins of six ores turn up in bands — coal
common and shallow, iron a little deeper, gold and the "deep three" (redstone, lapis,
diamond) only in the lowest quarter of the world. A bare-handed or wrong-tool swing
still breaks the ore block (it does not become unbreakable scenery), but yields
nothing — the tool in your hand is what turns an ore block into a resource, not
just what makes it break faster. This exactly matches `docs/ROADMAP.md`'s own
one-line framing for this version: *"coal high and common, iron below it, gold,
redstone, lapis and diamond in the deep, each in veins rather than singles"* plus
*"tool tiers, so an ore means something to reach."* **[codebase]**
(`docs/ROADMAP.md:336-340`)

The six ores below are exactly ROADMAP's list — no emerald, no copper, nothing
added and nothing dropped from what is already committed there.

---

## 2. Ore list: Y-bands, vein size, generation attempts

All Y-ranges below are quoted directly from pre-1.18 Java/old-Beta ore generation,
which is the right family to cite: Blocksmith's world is 128 tall with sea level 64,
the same height the Beta-era generator used, so **no rescaling arithmetic is needed
anywhere in this table** — a Y-range from 2014-era Minecraft documentation maps onto
Blocksmith's Y axis unchanged.

| Ore | Y-range | Below sea level (64) | Vein size | Attempts/chunk | Min. tool |
|---|---|---|---|---|---|
| Coal | Y 1–127 (all depths) | surface to bedrock | ~17 blocks | 20 | Wood pick |
| Iron | Y 1–64 | 0–63 blocks | ~9 blocks | 20 | Stone pick |
| Lapis lazuli | Y 1–32, peaks ~15–16 | 32–63 blocks | ~7 blocks | 1 | Stone pick |
| Gold | Y 1–32 | 32–63 blocks | ~9 blocks | 2 | Iron pick |
| Redstone | Y 1–16 | 48–63 blocks | ~8 blocks | 8 | Iron pick |
| Diamond | Y 1–16 | 48–63 blocks | ~8 blocks | 1 | Iron pick |

**[research]** Y-ranges, vein sizes and tool tiers: [Ore — Minecraft Wiki](https://minecraft.wiki/w/Ore),
[Ore/Pre-1.18 distribution](https://minecraft.wiki/w/Ore/Pre-1.18_distribution),
[Ore/Pre-1.17 distribution](https://minecraft.wiki/w/Ore/Pre-1.17_distribution),
[Coal Ore](https://minecraft.wiki/w/Coal_Ore), [Iron Ore](https://minecraft.wiki/w/Iron_Ore),
[Gold Ore](https://minecraft.wiki/w/Gold_Ore), [Redstone Ore](https://minecraft.wiki/w/Redstone_Ore),
[Lapis Lazuli Ore](https://minecraft.wiki/w/Lapis_Lazuli_Ore), [Diamond Ore](https://minecraft.wiki/w/Diamond_Ore).

**Honest caveat on vein-size and attempts-per-chunk**: these are old-Java-general
figures, not numbers a source confirms specifically for Legacy Console Edition's own
generator. What *is* confirmed **[research]** is that LCE's terrain generator is
described as matching **Java 1.12.2** ([Legacy Console Edition — Minecraft Wiki](https://minecraft.wiki/w/Legacy_Console_Edition)),
which used this exact old-generation ore algorithm unchanged from Beta — so this is
a well-supported family to build from, not a directly-decompiled LCE constant. If
exact LCE vein/attempt numbers matter later, that requires decompiling an actual LCE
build, which is out of scope for a web-research pass.

**Min. tool tiers [research]**: no source documents an LCE-specific deviation from
the standard Java pickaxe hierarchy (wood → stone → iron → diamond), so the table
above uses the general figures. There is deliberately **no gold pickaxe tier** —
real Minecraft's own golden tools are a *worse*, not better, tier than stone (fast,
fragile, a novelty), and reproducing that quirk adds a tool with no gameplay purpose
here. **[proposal]**

---

## 3. Generation mechanism — hangs off the existing carve pass, adds nothing new

`plan-1.8.11-caves.md` §3.4 already states ore placement's intended shape: a
decorator pass that runs *after* the carve mask has been applied and the column's
block grid is finished, replacing `BLOCK_STONE` cells with an ore block wherever a
vein lands. **[codebase, per the caves document's own extraction]**

Concretely, for each of the six ores, per chunk column: roll `attempts` (Table §2)
times; each attempt picks a random anchor `(x, y, z)` with `y` uniformly inside the
ore's Y-range, then walks a short random path of `vein_size` steps from that anchor,
turning any `BLOCK_STONE` cell it touches into the ore block and leaving anything
else (air, an already-placed ore, a cave-carved gap) untouched. This is the same
"blob/vein via short random walk" shape every source describes for old-Java ore
generation, and it reuses exactly the kind of per-column, seed-derived RNG entropy
already established for cave noise (`rngHash3`, per the caves document's own
extraction) — **no new noise field, no new RNG scheme.**

**Cost**: six ores × attempts from Table §2 sums to 52 vein-placement attempts per
column, each attempt touching on the order of 10–20 cells. That is on the order of a
few hundred RNG calls and block writes per column — **[proposal, reasoned]**, several
orders of magnitude below the terrain/cave passes' own already-measured ~1ms/column
host figure (`docs/ROADMAP.md:333-336`, v1.8.7's own timing note). Vein-walk state
(a position, a remaining-step counter) lives entirely in a local scratch of a few
tens of bytes — comparable in order of magnitude to the caves document's own ~32–64B
per-walker figure, itself labelled reasoned, not measured, there too. **This never
touches `WORLD_BUDGET_BYTES`** — `budget.h`'s cap counts only malloc'd
`Column`/`Chunk`/`LightColumn` allocations **[codebase, per the caves document's own
extraction of `budget.h:10-22`]**, and ore placement adds no new persistent
allocation, only writes into cells the carve/fill pass already allocated.

**Does this need a new `GEN_VERSION`?** Caves needed `GEN_VERSION_CAVES` because it
*reshapes* what is solid versus carved — replaying it on an old world would change
that world's own defining shape, which is exactly what the version pin exists to
prevent. Ore placement is different in kind: it only ever recolors `BLOCK_STONE`
cells that already exist under every generator (legacy, density, biome) — it removes
nothing, reshapes nothing, and needs to read nothing about which generator produced
the stone underneath it. **[proposal]** the recommended design is therefore to run
ore placement as a **generator-agnostic post-pass**, unconditionally, on every newly
generated column regardless of the world's pinned `GEN_VERSION` — not gated behind a
new version constant at all. This means:

- A world already saved before this version ships keeps its **already-generated**
  columns exactly as they are today (no ore, because nothing regenerates a saved
  column) — same behavior every prior generator change has had.
- Any **not-yet-generated** column in that same old world — new territory the player
  walks into after updating — gets ore, immediately, on whichever legacy/density/biome
  generator that world is otherwise pinned to.

If whoever implements this finds real coupling between ore placement and a specific
generator's internals that this document did not anticipate, the safe fallback is to
gate it behind a new `GEN_VERSION_ORES` exactly the way caves did — strictly more
conservative, at the cost of old worlds never getting ore at all, anywhere, until a
brand new world is created. See the "HIS CALL" section below.

---

## 4. Tool tiers

`mining.c` today has no tool tiers at all — every block breaks at a flat 1×
(`MINING_SPEED_ONE` = 256) regardless of what is held, and `breakTicksRequired()`
computes `ceil(hardness * 256 / speed)`, floored at 1 tick. **[codebase]**
(`mining.h:44-75`, `mining.c:16-44`) Crucially, the integration point already exists
and is already commented for exactly this purpose:

```c
uint32_t miningSpeedMultiplier(ItemId holding)
{
    switch (holding) {
    // ── Task 32 adds its tool rows HERE, one case each. ──
    default:
        return MINING_SPEED_ONE;   // bare hands, 1x
    }
}
```
**[codebase]** (`mining.c:16-26`, comment verbatim)

Proposed multipliers, in the same fixed-point unit (`MINING_SPEED_ONE` = 256 = 1×):

| Tool | Multiplier | Fixed-point value |
|---|---|---|
| Wood pickaxe | 2× | 512 |
| Stone pickaxe | 4× | 1024 |
| Iron pickaxe | 6× | 1536 |
| Diamond pickaxe | 8× | 2048 |

**[proposal]**, loosely inspired by real Minecraft's own hand→diamond speed
progression but simplified to a clean doubling-then-stepping curve rather than
copying Java's exact per-material constants, which were never a target here.

**Ore hardness**: existing core-block hardness values, read directly —
`stone = 45`, `dirt`/`grass = 12`, `sand = 10`, `wood`/`planks = 40`, `leaves = 4`
**[codebase]** (`registry.c:80-120`). Real Minecraft's own pre-1.17 ore hardness is a
flat `3.0` across all six ores, against stone's `1.5` — exactly a 2× ratio
**[research]** ([Tiers — Minecraft Wiki](https://minecraft.wiki/w/Tiers), general
per-block hardness figures cross-referenced against each ore's wiki page above).
Applying that same 2× ratio to Blocksmith's own stone hardness of 45 gives:

**Proposed ore hardness: 90 (uniform across all six ores)** — **[proposal]**, at 1×
speed (bare hands) that is 90 ticks = 4.5 seconds; at the diamond pick's proposed 8×
it is 90×256/2048 = 11.25 ticks ≈ 0.56 seconds.

Breaking with an insufficient tool (or bare hands) still removes the block at the 1×
rate — it is not made unbreakable — but the drop table below simply yields nothing
below the required tier, exactly matching real Minecraft's own behavior and
`docs/ROADMAP.md`'s framing that a tool is what makes an ore "mean something," not
what makes the block exist.

---

## 5. Drops

| Ore block | Drops (sufficient tool) | Drops (insufficient tool) |
|---|---|---|
| Coal ore | 1 coal (item) | nothing |
| Iron ore | Iron ore (the block itself — no raw item) | nothing |
| Gold ore | Gold ore (the block itself — no raw item) | nothing |
| Redstone ore | 4 redstone dust | nothing |
| Lapis lazuli ore | 6 lapis lazuli | nothing |
| Diamond ore | 1 diamond | nothing |

**[research]**: raw-ore items (raw iron, raw gold) were introduced in Java 1.17
(snapshot 21w14a), years after Legacy Console Edition's active development window
(2012–2019, discontinued 2019-12-10, well before 1.17 shipped in 2021) —
[Iron Ore — Minecraft Wiki](https://minecraft.wiki/w/Iron_Ore) (states the raw-drop
change is dated to 21w14a), [Legacy Console Edition — Minecraft Wiki](https://minecraft.wiki/w/Legacy_Console_Edition)
(discontinuation date). Iron and gold ore correctly drop **themselves** here, needing
a future furnace (`docs/VERSION-LIST.md`'s v1.8.15) to become ingots — matching the
PS3-era game the owner wants, not modern Java.

Redstone dust's "4" and lapis's "6" are flattened from real Minecraft's small random
ranges (4–5 and 4–9 respectively) to flat counts — **[proposal]**, a simplification;
if a random range is wanted instead it costs nothing structurally, it is purely a
numbers choice at drop time.

---

## 6. State, bytes, save compatibility, wire cost

- **No new per-column persistent state.** Ore placement writes into the same
  `Chunk` cell array the carve/fill pass already owns; there is nothing to add to
  `Column`, `Chunk`, or `LightColumn`, and nothing new to save per column beyond what
  already gets saved (the block IDs themselves).
- **Save format**: unaffected. A saved column's cells are just `BlockId` bytes today
  (`chunk.c`, read via the codebase-facts pass for this document — one byte per cell
  across all three chunk storage forms, no metadata field exists **[codebase]**);
  an ore cell is exactly as cheap to store as a stone cell. Nothing about the region
  file format changes.
- **Old saves**: per §3's recommendation, already-generated territory is unaffected
  forever; newly explored territory gets ore immediately, on whatever generator that
  world is pinned to. If the "HIS CALL" item below is answered the other way (gate
  behind a new `GEN_VERSION_ORES`), old saves get no ore anywhere until a new world is
  created — stated here so the tradeoff is visible before it is chosen.
- **Wire cost**: **zero new opcodes.** An ore block appearing in a newly generated
  column is sent to a joining/streaming client exactly the way any other generated
  block already is — through whatever opcode ships terrain today (`BS_APP_WORLD_GEN`
  / `BS_APP_CHUNK_DIFFS`, **[codebase]**, `networld.c:981-998`). A player mining an
  ore and it changing to air is an ordinary block edit, already covered by
  `BS_APP_BLOCK_EDIT` (**[codebase]**, `bs_proto.h:216-239`). Nothing about ore
  generation needs a new message type.
- **Registry CRC / lockstep cost**: six new block rows (see §7) change
  `registryCrc16()`'s output — a client running these rows **will be refused by an
  older server** and vice versa, exactly the precedent `docs/VERSION-LIST.md`
  documents for v1.8.1 ("The core block registry's checksum changes as a result — the
  matching server release is required.") **The server release for this version must
  ship before any client release carrying it.** This is a real, recurring cost of
  every version in this three-document batch, not unique to ores — stated once here,
  referenced rather than repeated in the other two documents.

---

## 7. Blocks and atlas tiles

Six new placeable blocks, one tile each (ore-in-stone textures do not share cleanly
with the biome-tint trick v1.8.8 uses for grass, since each ore's speckle pattern is
genuinely a different image, not a recolor of the same image):

| Block | Atlas tiles |
|---|---|
| `coal_ore` | 1 |
| `iron_ore` | 1 |
| `gold_ore` | 1 |
| `redstone_ore` | 1 |
| `lapis_ore` | 1 |
| `diamond_ore` | 1 |
| **Total** | **6** |

Atlas has 32 free slots today (31 of 64 used) **[codebase, per the atlas-facts
extraction of `atlas_uv.h:48-49,76-77,120` and `tools/make_atlas.py:134-218`]**.
Six tiles leaves **26 free** for the redstone and dimensions documents that follow —
see each document's own tally, and the cross-batch total in the dimensions
document's closing section.

**Open question this document does not answer**: whether non-block *item* icons
(coal, redstone dust, lapis lazuli, diamond, and the four pickaxe tiers) draw from
this same 64-slot world atlas or a separate UI icon sheet. The debug-menu block list
(v1.8.8, already shipped) implies blocks already have on-screen icons drawn from
somewhere, but this pass did not confirm whether that reuses the world atlas or a
distinct system. **Flagged as a risk in §9, not guessed at here.**

**Block ID cost**: 6 new core rows against 100 free core IDs today (27 of 127 core
slots used, `REG_ID_CORE_HI` = 0x7F **[codebase]**) — no pressure on ID space.

---

## 8. Build order

1. **ItemId/BlockId split** (prerequisite, not optional — see §9's first risk).
   `inventory.h`'s own comment already names this as the anticipated future work.
2. **Tool-tier rows** in `mining.c`'s `miningSpeedMultiplier()` switch, plus
   minimum-tier drop gating in whatever function resolves a break into a drop.
3. **Six ore rows** appended to `registry.c`'s `kCoreDefs`, hardness 90, `REG_FLAG_SOLID`.
4. **Six atlas tiles**, painted procedurally by `tools/make_atlas.py`, matching the
   existing "generated, not hand-drawn" art pipeline.
5. **Ore-vein decorator pass**, hooked after the carve/fill step per
   `plan-1.8.11-caves.md` §3.4's stated hook point — this step depends on that plan's
   own carve pass existing first, i.e. v1.8.11 ships before v1.8.12 can, matching
   `docs/VERSION-LIST.md`'s own ordering.
6. **`registry_test.c` updates**: the `coreHardnessIsDeclared()` row-count assertion
   (currently `rows == 25`) and the `registryCrc16()` golden pin (currently `0xD236`)
   both need updating to reflect the six new rows.
7. **Server release ships first** (CRC lockstep, §6).
8. **Client release**, then a real playtest: confirm veins actually land inside the
   stated Y-bands on a real generated world, confirm the right tool yields the right
   drop and the wrong tool yields nothing, confirm an old save's existing territory is
   visually unchanged and its new territory gets ore.

---

## 9. Risks and cheapest mitigation

1. **ItemId is currently the same type as BlockId** (`typedef BlockId ItemId;`,
   `inventory.h:38` **[codebase]**), and "every item this game has today is a block
   you mined" is stated as deliberate in that same file. Pickaxes are not
   placeable blocks, and three of six ore drops (coal, redstone dust, lapis) are not
   either. Forcing them into the block registry as fake unplaceable rows would also
   need a new exemption in `registry_test.c`'s hardness-declared check (today's only
   exemption is liquids). **Cheapest mitigation**: do the ItemId split `inventory.h`
   already anticipates, as build-order step 1 — it is smaller now (nothing depends on
   the old conflated scheme yet outside water/dirt-style block-items) than it will
   ever be again.
2. **Item icon atlas is unconfirmed** (§7's open question). **Cheapest mitigation**:
   a five-minute grep for how the existing debug-menu block list (v1.8.8) draws its
   icons, before any new tile is committed to either scheme — cheap to check, expensive
   to guess wrong on.
3. **Ore placement's hook point in `worldgen.c`/`worldgen_density.c` was read
   secondhand** (through the caves document's own extraction, not this document's own
   direct read of that 43KB file). **Cheapest mitigation**: whoever implements step 5
   above re-confirms the exact call site against the tree as it stands then, not
   against this document's citation.
4. **Old saves never retroactively gain ore in already-explored territory** (§3, §6).
   This is a real, visible consequence for the owner's own existing test worlds, not
   a hypothetical — see "HIS CALL" below.

---

## 10. HIS CALL

- **Should ore placement apply universally to any newly generated column regardless
  of a world's pinned `GEN_VERSION` (this document's recommendation, §3), or be gated
  behind a brand new `GEN_VERSION_ORES` the way caves was?** The universal approach
  means every existing save — including whatever world the owner is already
  playtesting on — gets ore the moment this ships, in any territory not yet explored.
  The gated approach means no existing save ever gets ore unless a new world is
  created. This is a real fork in what his own current save will look like, not an
  implementation detail.
- **Flat drop counts (4 redstone dust, 6 lapis) versus a random range** (§5) — minor,
  not blocking, noted for completeness.

---

## 11. LINES SOMEONE ELSE MUST ADD

Nothing under `source/`, `tests/`, or `tools/` was edited to produce this document.
A future implementation needs, at minimum:

- **`source/world/inventory.h`**, after the `typedef BlockId ItemId;` at line 38 —
  split `ItemId` into its own type/table, per that file's own comment at lines 16-23.
- **`source/world/mining.c:16-26`** — add four tool-tier `case` rows to
  `miningSpeedMultiplier()`'s switch (currently only `default:` exists).
- **`source/world/registry.c`**, appended after the last existing `kCoreDefs` row
  (around line 501) — six new `BlockDef` rows: `coal_ore`, `iron_ore`, `gold_ore`,
  `redstone_ore`, `lapis_ore`, `diamond_ore`, each `.hardness = 90`,
  `.flags = REG_FLAG_SOLID`.
- **`source/world/registry_test.c:640-651`** — `coreHardnessIsDeclared()`'s
  `rows == 25` assertion becomes `rows == 31`; the `registryCrc16()` golden pin at
  line 546 (currently `0xD236`) needs recomputing and updating.
- **`tools/make_atlas.py:134-218`** — six new `TILES` entries for the ore textures.
- **`source/world/worldgen.c` and/or `source/world/worldgen_density.c`** — the new
  ore-vein decorator pass, hooked after the carve+fill step `plan-1.8.11-caves.md`
  §3.4 describes (exact call site to be re-confirmed against the tree at
  implementation time, per Risk 3 above).
- **The server's own registry table** — the matching six-row update, and it must be
  released before any client build carrying these rows (§6, §8 step 7).
