# v1.8.15 — Furnace

A furnace block with fuel and a smelting timer. Raw meat becomes cooked meat.
Ore→ingot is explicitly **not** specified here — see below.

This document does not implement anything. It builds on
`docs/plan-1.8.14-animals.md` for the raw-meat items it cooks, and on the
existing `world/crafting.h` module's design reasoning (cited, not reused — see
"Why a parallel table, not an extension" below).

## Provenance legend

Same as the other three documents in this set: `[read]`, `[research-given]`
(a figure supplied by the outside research pass this task was briefed
against, cited to the specific minecraft.wiki page), `[reasoned]`,
`[assumed]`, bare numbers are my own proposal.

**Citation pass note:** this document was originally written with no outside
research in hand. The research has since been supplied. Per the citation-pass
rule: matching numbers get the tag swapped and cited; numbers that differ keep
their original value with the divergence explained rather than silently
corrected; numbers the research doesn't cover stay proposals. One correction
of substance turned up in this pass — the log-fuel-duration entry below was
originally justified by an appeal to a classic-MC "burns as long as its 4
planks" convention that turns out not to be true; see that row and the note
under Fuel items below. As with the other three documents, all cited figures
are Java Edition — Legacy Console Edition has no separately documented furnace
timing figures anywhere the research turned up.

## The mechanic, as the player experiences it

- The furnace is a placeable block, like any other — break/place work exactly
  as they do for every existing block.
- Aiming at a placed furnace within reach shows a prompt to open its panel on
  the touch screen (the same screen the existing crafting panel already uses).
- The panel has an input slot (what's being cooked), a fuel slot (what's
  burning), and a progress indicator.
- Dropping raw meat in the input slot and a burnable item (wood, planks) in the
  fuel slot starts cooking automatically once both are present. After 10
  seconds, one cooked meat item appears in the output; the raw meat and a
  portion of the fuel are consumed.
- A furnace with fuel but no valid input just burns fuel and does nothing
  useful — matches classic MC (fuel isn't refunded for an empty attempt).
  `[research-given]` confirms the specific behavior this implies: once a fuel
  item is consumed and starts burning, it burns to completion even if the
  input is removed partway through (real Java furnaces don't "pause" a
  burning fuel item), which this document's `fuel_ticks_left` field already
  models correctly without having been checked against the source at the time
  it was written.
- A furnace with no fuel does nothing, indefinitely, until fuel is added.
- Walking away from a burning furnace and coming back later finds it exactly
  as it was left — contents persist.

## Numbers, and where each one came from

| Value | Number | Provenance |
|---|---|---|
| Smelt time | 200 ticks (10s) | `[research-given]` — exact match. Real Java smelt time is 200 ticks at 20 TPS, confirmed for the basic (no-recipe-modifier) case. Blocksmith's own simulation clock is also 20 TPS (`TICK_HZ`, `source/world/tick.h`, `[read]`), so this transfers exactly rather than approximately — a rare case of a classic-MC tick figure being usable verbatim. |
| Fuel burn duration, planks | 300 ticks (15s) per plank | `[research-given]` — exact match. Real Java plank burn time is 300 ticks. |
| Fuel burn duration, logs | 1200 ticks (60s) per log | Kept as-is. **Divergence, and a correction to my own original reasoning:** the justification I originally gave — "a log burns as long as the 4 planks it could be turned into" — turns out to be factually wrong. `[research-given]`: real Java logs, planks, *and* crafting tables all burn for an identical 300 ticks (1.5 items' worth of smelt time each), not 1200. There is no "4x" relationship in the real game at all; I invented one and it happened to sound like a plausible classic-MC convention without actually being one. I'm keeping 1200 in the table per this pass's rule (don't silently move to the sourced number), but unlike the deliberate simplifications elsewhere in this document set, this one has no surviving rationale — the reasoning it was built on is gone. My recommendation, if he wants one: change this to 300 to match planks, since the thing that made 1200 attractive (a tidy "logs are worth 4 planks" story) isn't true and there's no other reason on record to prefer 1200. |
| Fuel items accepted | wood, planks, birch/spruce logs and planks — every existing timber block | Mine, but forced by what actually exists: no coal or charcoal item exists anywhere in this codebase today (confirmed by the block/registry read below). Timber is the *only* thing that could plausibly burn. For reference, `[research-given]` real Java fuel values beyond wood: coal 1600 ticks (8 items), charcoal 1600 ticks (8 items), coal block 16000 ticks (80 items), lava bucket 20000 ticks (100 items), blaze rod 2400 ticks (12 items) — none of these items exist in this codebase, so none are actionable now, but they're the numbers to reach for once coal/charcoal land (see HIS CALL item 2). |
| Coal as fuel | explicitly deferred | See HIS CALL — ore doesn't exist in any version this pass covers. |
| Furnace block hardness | 45 ticks (2.25s) | Mine, picked to match `BLOCK_STONE`'s hardness (`world/registry.c`, `[read]`) since a furnace is reasonably modeled as a stone-built object. Not covered by the research — Java's furnace hardness (3.5, using seconds-with-tool units incompatible with this codebase's tick-based hardness) doesn't translate directly, so this stays a proposal rather than a citable match. |
| Concurrent furnace cap | 64 per world | Mine — see "Per-instance state" below for why this number exists at all and what raising it costs. No real-Minecraft equivalent exists to cite (Java/Bedrock don't cap total furnace count). |
| Cook-progress UI granularity | 8 stages | Mine, but grounded: matches `INTERACT_BREAK_STAGES` (`source/scene/interact.h`, `[read]`), the only existing precedent in this codebase for "how many discrete steps does a progress overlay get." Reusing it rather than inventing a different number keeps the two progress bars visually consistent. **Divergence:** `[research-given]` the real furnace GUI doesn't use a staged bar at all — it fills a flame icon (fuel remaining) and an arrow icon (cook progress) continuously, pixel-by-pixel, not in discrete steps. My 8-stage design is a different UI approach entirely, not a numeric mismatch of the same mechanic — kept because it reuses an existing codebase precedent (`INTERACT_BREAK_STAGES`) rather than building continuous-fill rendering from nothing. |

## Furnace state is a new kind of thing this codebase doesn't have yet

Every block in this engine today is *just an id* in a `Column`'s cell array —
`world/world.c`/`chunk.c` store block ids and nothing else per-cell. Nothing
resembling Minecraft's "block entity" (a block that also carries private,
per-instance data — a chest's contents, a furnace's progress) exists anywhere
in `source/`. This is a real architectural gap, not a detail to wave past, and
this document has to propose a shape for it.

**Rejected: reusing the `Entity` store** (`source/entity/entity.h`, the system
`plan-1.8.14-animals.md` builds directly on). A furnace never moves, never
needs a collision box, and isn't a creature — every field `Entity` carries
(`Body`, `yaw`, `ai_state`) would be dead weight. Worse, `EntityWorld` has a
hard cap (`ENTITY_CAP_OLD=24` / `ENTITY_CAP_NEW=48`, `[read]`) shared with
every animal and monster kind — a handful of furnaces would eat directly into
the same budget `plan-1.8.14-animals.md`'s own Risks section already flags as
tight. Rejected.

**Proposed: a small, dedicated, fixed-size table.**

```c
typedef struct {
    int32_t  x, y, z;          // world block position, the key
    uint8_t  input_item, input_count;
    uint8_t  fuel_item,  fuel_count;
    uint16_t fuel_ticks_left;   // ticks of burn remaining on the CURRENT fuel item
    uint16_t cook_ticks;        // progress toward the current smelt
    uint8_t  output_item, output_count;
} FurnaceState;

#define FURNACE_SLOTS 64
```

The three-slot shape (input, fuel, output) and the convention of treating them
as distinct, independently-addressed slots is `[research-given]`-confirmed as
matching the real furnace GUI's own layout exactly (slot 0 input, slot 1 fuel,
slot 2 output in Java's own container-slot numbering) — not something this
document derived, but a case where the obvious design and the real one landed
on the same shape independently.

`sizeof(FurnaceState)` is 20 bytes; 64 of them is 1,280 bytes — comfortably
inside ordinary static memory, and, exactly like `EntityWorld`, **not** counted
against `WORLD_BUDGET_BYTES` (`source/world/budget.h`) at all, since that
budget is specifically the per-loaded-column terrain/light cost and this table
is neither. An earlier pass of this task flagged furnace storage against
"~0% free world-budget headroom" as an open risk; having now read `budget.h` in
full, that concern doesn't apply — this is the same category of memory
`EntityWorld` already occupies for free, and 1.3 KB was never going to be the
thing that mattered next to a 12 MB column budget.

The 64-furnace cap is a genuine, honest limit (not "effectively unlimited"):
build a 65th furnace and one of two things has to happen — refuse the
placement, or evict the coldest (e.g. least-recently-active) entry. Neither is
decided here; flagged as HIS CALL. 64 was picked as "clearly enough for one
base," not measured against anything.

## What must be saved, and what an existing save without it gets

New sidecar, same tmp-fsync-swap pattern as everything else in this tree
(`playerpose.c`, `inventory.c`, `daynight.c`, all `[read]`):
`<world_dir>/furnaces.dat` — magic, version, count, then up to
`FURNACE_SLOTS` packed `FurnaceState` records.

**An old save with no `furnaces.dat`:** loads as zero furnaces — the file
simply doesn't exist yet, same "missing file is normal" rule every sidecar in
this tree already follows. No migration, because nothing about furnaces
existed before this version to migrate from.

This does **not** touch the region/chunk save format (`world/region.h`,
`[read]`) at all — furnace contents live entirely in the new sidecar, keyed by
absolute block position, independent of which region file that position
happens to fall in. The tradeoff: a furnace's *presence in the world* (the
block id itself) is still ordinary column data and saves/loads with the
terrain as normal, but its *contents* live in a second place. Breaking a
furnace block without also clearing its `furnaces.dat` entry would leak a
stale record — the break handler needs to remove the matching entry, which is
new logic, not automatic. Flagged in Risks.

## What goes on the wire, and whether the server has to ship first

Nothing is specified to go on the wire in this version. Furnace contents are
not currently synced to other players in any design this document proposes —
consistent with `plan-entities.md`'s existing "entities are unsynced for now"
precedent for the whole survival/animals/monsters arc, extended here to
furnaces by the same reasoning rather than re-argued. If furnace contents ever
need to be visible to other players (someone else's furnace showing real
progress, not just "a furnace block exists"), that's a new C→S (player edits a
furnace's slots) and S→C (furnace state broadcast) pair of opcodes, and by the
same direction rule `entity.h`'s Networked-entities section already states for
entity sync: the C→S direction needs the server to ship first, the S→C
direction is safe to ship client-first. Not designed further here — flagged
HIS CALL.

## Per-tick cost at an Old 3DS's radius

Only *active* furnaces (fuel present, valid recipe in progress) do work each
tick; the proposed tick function iterates the fixed 64-slot table once per
simulation tick and skips inactive entries on a single flag check first —
**O(64) comparisons worst case, per tick, regardless of render radius**,
identical cost on Old and New 3DS since the table's size doesn't scale with
either. This mirrors the exact reporting style `budget.h` and `entity.h` both
already use: a stated worst-case count instead of a vague claim. In practice
the number of *simultaneously burning* furnaces a player has built will be far
below 64 for a long time, so this is a ceiling, not a typical cost.

## Why a parallel recipe table, not an extension of `CraftRecipe`

`world/crafting.h`'s own header comment explains at length why `CraftRecipe`
is deliberately "one input, one output, no fuel, effectively instant" — a
shape earned by the actual six-to-eight-block crafting list needing nothing
more (`[read]`, `world/crafting.h`). A furnace recipe needs a **third**
dimension neither `CraftRecipe` nor `craftMake()`/`craftCanMake()` have any
notion of: a separate fuel input that is consumed on a different schedule than
the primary ingredient, plus a non-instant timer. Bolting that onto
`CraftRecipe` would either force every ordinary shapeless recipe to carry an
unused fuel field, or require `craftMake()` to grow a furnace-shaped special
case inside a function whose whole point (per its own header) is staying
simple. Both are worse than a second, small, purpose-built table.

Proposed, in a **new, self-contained module** — `source/world/furnace.h`/`.c`,
touching neither `crafting.h` nor `crafting.c`:

```c
typedef struct {
    const char* name;
    ItemId      input_item;
    uint8_t     input_count;
    ItemId      output_item;
    uint8_t     output_count;
    uint16_t    cook_ticks;     // 200 for every recipe below; a field, not a
                                 // constant, because ore->ingot will very
                                 // likely want a different number later
} FurnaceRecipe;

enum {
    FURNACE_RECIPE_PORK    = 0,
    FURNACE_RECIPE_BEEF,
    FURNACE_RECIPE_CHICKEN,
    FURNACE_RECIPE_MUTTON,
    FURNACE_RECIPE_COUNT
};
```

Four rows: raw→cooked for each of the four meats `plan-1.8.14-animals.md`
proposes. **Ore→ingot is not specified here** — see below.

## Ore→ingot: explicitly out of scope

The ROADMAP stub for this version (`docs/ROADMAP.md`, `[read]`) mentions "ore
becomes ingots" in the same breath as meat. Ore and ingot items do not exist in
any version this document covers — they belong to v1.8.12 (Ores), which is a
separately-scoped, separately-researched version (a background research pass
on "Ores, redstone, dimensions" was in progress elsewhere at the time this
document was written, entirely outside this task's brief). This document
deliberately does not invent ore or ingot items to fill that gap. What it does
guarantee: `FurnaceRecipe`'s shape (input/output/cook_ticks, no assumption
about what "input" or "output" mean) needs no redesign to add ore rows later —
whoever specifies ores adds recipe rows, not a new table.

## The furnace panel: reusing the existing touchscreen crafting UI's approach

`world/crafting.h`'s own header describes "a touchscreen crafting panel"
(`[read]`) as the existing mechanism for the four crafting recipes already
shipped. This document proposes the furnace panel work the same way — a
bottom-screen panel that becomes available when `Interact.target`
(`source/scene/interact.h`, `[read]`) is aimed at a furnace block within
`INTERACT_REACH` (5.0 blocks, `[read]`), showing the input/fuel/output slots
and an 8-stage progress bar (see above). This needs no new button binding —
it's gated on aim, matching how the existing crafting panel is reachable via
touch rather than a face button, and leaves `KEY_B` (claimed by eating in
`plan-1.8.13-survival.md`) untouched.

## Legacy feel over modern parity

1. **No hopper, no auto-feeding.** Every fuel/input insertion is a manual
   touch-panel action. Matches every edition of Minecraft including the
   earliest console ones — hoppers arrived comparatively late even in Java.
   Not a deviation at all, just confirmed as in-scope-by-default.
2. **Fixed 10-second smelt regardless of item.** Classic MC (all editions)
   already does this — every basic smelt is 200 ticks flat, no per-item
   variance. Confirmed, not a change.
3. **No smelting XP.** Modern Minecraft awards experience per smelted item —
   **N/A here**, not a simplification of a modeled system: this codebase has
   no XP/leveling system referenced anywhere in the files read across this
   whole four-document pass, confirmed again by this citation pass's fuller
   read of the research (which does give per-item smelting XP values, e.g.
   0.1–1.0 XP depending on what's cooked) — out of scope entirely because
   there's nothing here for XP to plug into, not a legacy-vs-modern call.

## Build order

- **P0 — registry rows.** The furnace block itself needs a new core id (27,
  next free after the four meat items `plan-1.8.14-animals.md` proposes at
  27–30, so the furnace becomes 31 if those land first — order matters here
  and should be pinned at implementation time, not assumed from this
  document). See LINES SOMEONE ELSE MUST ADD.
- **P1 — `source/world/furnace.h`/`.c`.** `FurnaceState` table,
  `FurnaceRecipe` table, `furnaceTick()` (the O(64) per-tick scan), placement/
  removal helpers keyed by block position. Host tests, same red-arm
  methodology as the other three documents in this set.
- **P2 — persistence.** `furnaces.dat` sidecar, tmp-fsync-swap, "missing file
  = zero furnaces" and "corrupt file falls back to empty" tests.
- **P3 — break/place integration.** Placing the furnace block behaves like any
  other block (no new logic). Breaking one must remove its `FurnaceState`
  entry — this is new logic, since nothing today couples a block break to a
  side-table cleanup. See LINES SOMEONE ELSE MUST ADD.
- **P4 — panel UI.** Aim-gated touch panel, slot insertion, progress display.
- **P5 — main-loop integration.** `furnaceTick()` call in the existing
  per-simulated-tick loop.
- **P6 — art.** Furnace block textures (lit/unlit face variants, since a
  burning furnace conventionally looks different — classic MC precedent,
  `[reasoned]`) via `tools/make_atlas.py`.

## Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| Breaking a furnace leaks its `FurnaceState` entry (table entry survives, position now holds a different block or air). | Host-test it directly: place, start cooking, break, assert the table no longer has an entry at that position. Cheap because both the world and the furnace table are host-testable. |
| 64-furnace cap is reached in a real playthrough (unlikely soon, but not impossible on a long-lived world). | Cheapest fix if it happens: raise the constant. No format change needed — the sidecar already stores a count and only that many records, so a higher cap is backward-compatible with older `furnaces.dat` files (they simply have fewer records than the new cap allows). |
| Recipe id ordering assumption in this document (meats at 27–30, furnace at 31) turns out wrong once animals and furnace are actually built in whatever order they land. | Not load-bearing — `registry.c`'s dynamic-row registration doesn't care about specific numbers, only that they're assigned once and pinned via `_Static_assert`. Re-derive the actual next-free id at implementation time; this document's numbers are illustrative, not a reservation. |
| Two furnaces built at the exact same block position (shouldn't be possible if placement already refuses occupied cells, but worth stating). | Not a new risk — ordinary block placement already refuses a non-air target cell; the furnace table's placement helper should refuse an insert if a `FurnaceState` already exists at that position, as a second, cheap belt-and-braces check. |

## LINES SOMEONE ELSE MUST ADD

1. **`source/world/block.h`** and **`source/world/registry.c`** — the furnace
   block's core row, following the exact five-step checklist `registry.c`'s
   header already documents (the row including `.hardness=45`, the
   `world/block.h` literal id + `_Static_assert`, the `BTEX_*` tile constant,
   the `tools/make_atlas.py` art). This document cannot touch any of those
   three files.
2. **`source/scene/interact.c`** (the break path) — the block-break handler
   needs to check "was this a furnace block" and, if so, call the proposed
   furnace-table removal function before or alongside the ordinary break
   logic. This is new coupling between break handling and a system that
   doesn't exist yet; it belongs in a file this document cannot edit.
3. **`source/scene/ui.c`** / **`source/scene/ui_layout.h`** — the furnace
   panel itself (P4), and whatever the existing crafting-panel touch-open
   mechanism is (not read in full during this pass — re-read at
   implementation time to match its exact pattern rather than inventing a
   parallel one).
4. **`source/main.c`** — the `furnaceTick()` call inside the existing
   per-simulated-tick loop (P5), alongside the `survivalTick()` and
   `entityTick()` calls the other two documents already propose adding there.

## HIS CALL

1. **What happens at furnace #65.** Refuse the placement outright (simplest,
   but a hard wall a long-lived world could actually hit), or evict the
   coldest existing furnace (never blocks the player, but silently destroys
   someone's forgotten furnace contents across the world). I lean toward
   refusing placement with a message, since silent eviction of another
   furnace's contents is the kind of thing that reads as a bug/data loss the
   first time it happens — but this is a real tradeoff, not an obvious call.
2. **Coal/charcoal as a future fuel.** Confirmed out of scope for this
   version since neither exists. Worth deciding now whether "wood only" fuel
   is an acceptable permanent state for a while, or whether ore/ores work
   should be sequenced to land before or shortly after the furnace so it isn't
   wood-only for long. Sequencing is his call, not a decision this document
   can make since it doesn't own the ore-version roadmap.
3. **Multiplayer furnace sync.** Confirmed out of scope, consistent with the
   whole entity-sync deferral already accepted elsewhere in this arc. Worth an
   explicit yes/no the same way `plan-entities.md` asked for one on entities
   generally, since it's the same shape of "known, visible inconsistency"
   rather than a bug.
4. **Whether the furnace should visually change when lit** (proposed: yes,
   two texture variants). Cheap either way; flagged only because it's the one
   piece of this document's art plan not forced by anything already in the
   codebase — a plain "always looks the same" furnace is a smaller art ask if
   that's preferred.
5. **Log fuel duration (1200 vs. the now-known-correct 300).** Surfaced by
   this citation pass, not a pre-existing HIS CALL item — see the "Numbers"
   table and the provenance note above. My original reasoning for 1200 doesn't
   hold up; I've kept the number per this pass's rules but flagged it as the
   one figure in this document I'd actually recommend changing if asked,
   rather than a defensible simplification like the others in this set.

## Sources

Every `[research-given]` tag above cites the same outside-research pass
supplied to `docs/plan-1.8.13-survival.md` and `docs/plan-1.8.14-animals.md`:
minecraft.wiki, specifically material covering furnace smelting time, fuel
burn durations for the full fuel-item list, furnace GUI slot layout and
progress-indicator behavior, and smelting experience values. Supplied verbatim
by the task's coordinating agent; not independently re-fetched or re-verified
during this citation pass.
