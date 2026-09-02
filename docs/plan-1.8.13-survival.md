# v1.8.13 — Survival

Health, hunger, fall damage, eating, death and respawn. The rung under animals
(v1.8.14), the furnace (v1.8.15) and monsters (v1.8.16) — none of those three can
deal or take damage, or make "eat something" mean anything, until this one exists.

This document does not implement anything. It is the design and the citations a
build phase works from. Nothing here has been written to `source/`.

## Provenance legend

- `[read]` — a fact taken directly from the code or an existing doc, cited by file.
- `[reasoned]` — derived by argument from `[read]` facts, or from well-established,
  stable Minecraft convention (Legacy Console Edition / classic Java, pre-1.9
  combat era) — general genre knowledge, not a quote from a specific source.
- `[assumed]` — an unchecked working assumption, flagged so it can be verified
  before it is relied on.
- `[research-given]` — a figure supplied directly by the outside research pass
  this task was briefed against, cited to the specific minecraft.wiki page it
  came from. See the note below for what this covers and what it doesn't.
- A bare number with no tag is **my own proposal** — a game-feel choice, offered
  with its reasoning, meant to be retuned by hand.

**A note on provenance for this document specifically — updated after a citation
pass.** An earlier draft of this document was written after the outside research
this task was briefed against had dropped out of context; every number in that
draft was labeled `[reasoned]` (stable classic-MC convention, no source in hand)
rather than mislabeled as a citation I couldn't produce. The research has since
been supplied back, verbatim, and this document has been revised in place: figures
now confirmed to match a sourced value are tagged `[research-given]` with the
citation; figures that turn out to **differ** from the sourced value are left as
originally chosen, with the sourced figure quoted alongside and an explicit note
on why the two don't match — never silently swapped. Figures the research doesn't
cover at all stay as my own proposal, unchanged.

**One gap the research itself flags, and this document inherits:** the sourced
figures below are all **Java Edition**. Legacy Console Edition (the PS3/Xbox 360-
era version steve wants this game to feel like) does not have its own separately
documented numbers anywhere the research turned up — Java's figures are the best
available baseline, used on the working assumption that LCE's hunger/health/fall
math tracked Java's fairly closely for the systems this document covers, not on
direct confirmation. Flagged here once rather than re-caveated at every row.
Sources for every `[research-given]` tag below: minecraft.wiki pages *Hunger*,
*Food*, *Healing*, *Damage*, *Death*, *Tutorial:Hunger management*,
*Tutorial:Breaking a fall*.

## The mechanic, as the player experiences it

- Two meters exist: **health** and **hunger**, each 0–20.
- Hunger drains slowly over time regardless of what the player is doing, a little
  faster if they've been breaking blocks. It does not drain at all in the menus
  or while paused.
- While hunger is high (≥18/20), health slowly regenerates on its own.
- At 0 hunger, health slowly drains instead — down to 1, never further. Starving
  cannot kill you by itself.
- Falling more than 3 blocks and landing hurts, proportional to how far past 3
  blocks the fall was. Landing in water cancels it entirely.
- Pressing **B** eats whatever is in the selected hotbar slot, if it's food and
  hunger isn't already full. At ship time the only food is the apple
  (`BLOCK_APPLE`) — meat arrives in v1.8.14/v1.8.15.
- At 0 health, the player dies and respawns immediately at the world spawn point,
  full health and full hunger. There is no death screen and no delay.

## Numbers, and where each one came from

| Value | Number | Provenance |
|---|---|---|
| Health/hunger scale | 0..20 | `[read]` — already the scale `Entity.health` uses (`source/entity/entity.h` line 105: *"0..20, the scale BS_APP_PLAYER_STATE already uses"*) and the scale `NetworldPlayerMeters.health`/`.hunger` use as floats (`source/net/networld.h`). Also `[research-given]` — "Max hunger 20 points = 10 HUD shanks" (*Hunger*). Both agree; the player's meters were chosen to match the code, and the code's scale turns out to match Java's own. |
| Storage type | `uint8_t`, not `float` | Mine. `Entity.health` is already `uint8_t` at this scale; the player doesn't need finer precision than that and a float buys nothing but 3 extra bytes and a rounding question at the save boundary. |
| Safe fall distance | 3 blocks | `[research-given]` — "`damage = max(0, fallDistance - 3)`" (*Tutorial:Breaking a fall*). |
| Fall damage | 1 hp per block beyond 3, uncapped (can kill) | `[research-given]` — "about 1 HP per block beyond" the 3-block threshold (*Tutorial:Breaking a fall*). The source adds a real implementation nuance worth carrying forward: a 23-block fall *should* kill mathematically (23−3=20=lethal) but the reference engine actually needs ≈23.5 blocks because of when fall distance updates relative to the ground check — a half-block timing quirk in how "landed" is detected, not a different damage formula. Worth having a host test for the boundary case specifically, not just the general formula, when this is built. No hay bale, no slow-falling effect, no elytra exist in this codebase, so none of classic MC's other mitigations apply. |
| Water landing negates fall damage | full negation, not partial | `[research-given]` — "Water fully resets fall distance if touched before the ground" (*Tutorial:Breaking a fall*). The source also names ladders and vines as resetting fall distance; neither exists as a block in this codebase, so water is the only mitigation that applies here. Detected via `bodySubmerged()` (`source/world/physics.h`, already public) at the landing frame. |
| Hunger drain model | flat time-based, **not** Java's exhaustion/saturation system | Mine — **differs from the sourced model, on purpose.** See "Divergences from the sourced Java figures" below for the real numbers and why this document doesn't implement them. |
| Hunger drain rate | −1 every 1800 ticks (90s), full bar lasts ~30 min of continuous play | Mine, not derivable from the sourced exhaustion system since this design doesn't track exhaustion at all. **Tunable — flagged HIS CALL below.** |
| Extra hunger cost for mining | −1 extra hunger per 20 blocks broken | Mine. The sourced figure for the thing this is standing in for — Java's actual per-block mining exhaustion — is 0.005 exhaustion/block against a 4.0 reset threshold, i.e. 1 exhaustion-driven hunger loss per 800 blocks mined (`[research-given]`, *Hunger*). My number is roughly 40× more aggressive than that and was picked for legibility (a mining session should visibly cost something), not to approximate it. See the divergence note below. |
| Natural regen threshold | hunger ≥ 18/20 | `[research-given]` — "Natural regeneration needs hunger >= 18/20" (*Healing*). |
| Natural regen rate | +1 hp every 80 ticks (4s) | `[research-given]` — "Rate 1 HP every 4 seconds (80 ticks)" (*Healing*). The source also gives a second, faster regen tier — at hunger exactly 20 with saturation remaining, 1 HP every 0.5s costing 1.5 saturation — sourced to a community post rather than the wiki's edition table and flagged there as moderate confidence. This document does not implement that faster tier at all, since it has no saturation value to gate it on (see below) — noted for reference, not adopted. Regen also does **not** cost hunger/exhaustion in this design, versus Java's real 6.0-exhaustion-per-HP-healed cost (`[research-given]`, *Healing*) — a deliberate omission, see the divergence note. |
| Starvation rate | −1 hp every 80 ticks (4s), floor at 1 hp on Normal | `[research-given]` — "Starvation at 0 hunger: 1 damage every 4 seconds"; difficulty floors: "Easy stops at 10 HP, Normal stops at 1 HP and cannot kill, Hard has no floor, Peaceful disables hunger drain entirely" (*Damage*/*Death*). This document's single-difficulty decision below adopts the Normal row exactly. |
| Eat button | `KEY_B`, fresh-press | Mine, but grounded: `[read]` — `source/scene/player.h`, `source/scene/interact.h` and every gameplay `KEY_*` binding in `source/` (grepped) account for A (jump), X (break), Y (place), L/R (fly up/down), D-pad (move), START (exit). `KEY_B` is bound nowhere in gameplay — only in menu "back" navigation (`source/scene/title_nav.h`), which doesn't run during play. It is the one face button genuinely free. Not a research-covered question — this is a this-codebase-specific input decision. |
| Apple hunger restore | +4 hunger | `[research-given]` — "apple 4 / 2.4" hunger/saturation (*Food*). The saturation half (2.4) is not modeled — this design has no saturation value at all (see below) — so only the hunger figure is used. No direct health restore, matching the source: nothing in the *Food* table gives apple a direct-heal component; healing here is indirect, through natural regen once hunger is high enough. |
| Respawn location | fixed world spawn, `(spawn_x=8, spawn_z=8)`, y = `worldStandingY(...)` at that column | `[read]` — `source/main.c` lines 3983–4023: this is the exact computation the game already runs once at first boot (there's a second, debug-only `(18,18)` "handbuilt" variant at the same lines). It is not a stored field anywhere — it's recomputed live from a fixed x/z each time, which respawn can reuse verbatim. The sourced respawn-priority order is "bed, then respawn anchor, then world spawn" (`[research-given]`, *Death*) — neither a bed nor a respawn-anchor block exists anywhere in this codebase, so world spawn isn't a simplification of that list, it's the only entry on it that applies. |
| Respawn health/hunger | 20/20 (full) | Mine, obvious default, matches "missing state = full" used everywhere else in this design. |

## Divergences from the sourced Java figures, explained

Everything in this section was already the design before the citation pass —
nothing here is a new decision. What's new is having the real numbers to compare
against, and being honest about where this document doesn't match them.

1. **No exhaustion/saturation system at all.** Java's real hunger model is two
   hidden values on top of the visible hunger bar: **exhaustion** (0..4.0,
   accumulates from sprinting 0.1/metre, sprint-jumping 0.2, a normal jump 0.05,
   swimming 0.01/metre, a landed attack 0.1, mining a block 0.005, taking
   unblocked damage 0.1, or natural regen at 6.0 per HP healed; at 4.0 it resets
   and consumes 1 saturation, or 1 hunger once saturation is already 0) and
   **saturation** (starts at 5.0 for a new player, drains first and invisibly —
   only once it hits 0 does the visible hunger bar start moving) `[research-given,
   Hunger]`. This document replaces both with a single flat timer (see the table
   above). Why: this codebase has no sprint action at all — the largest single
   term in the real system doesn't apply here — and modeling two hidden
   accumulators for what's left (jump, mine, take-damage, a per-tick regen cost)
   is real complexity (two more `Survival` fields, a reset-and-borrow rule
   between them) for a game-feel difference a player is unlikely to be able to
   tell apart from a flat drain at these small block counts. This was a
   deliberate scope call before the citation pass and remains one now that the
   real numbers are in hand — it's a bigger simplification than it looked before
   (two systems collapsed to one, not just "no sprint term"), which is worth
   saying plainly rather than understating.
2. **Mining's hunger cost is far more aggressive than Java's.** Table above:
   mine picked −1 hunger per 20 blocks; Java's sourced rate is 1 hunger-equivalent
   per 800 blocks (0.005 exhaustion × 800 = 4.0 = one reset). Not a considered
   trade-off — I didn't have this number when I picked mine. Since the whole
   hunger model here is already a deliberate simplification (point 1), I'm
   leaving the constant as originally chosen rather than adopting 1/800 wholesale
   — a 1-per-800-blocks cost would be invisible at the "mine for a few minutes"
   timescale this document's other numbers are tuned for, and would make the
   "mining has a cost" bullet in the mechanic description above simply untrue in
   practice. Flagged as a number worth revisiting once played, same as the base
   drain rate.
3. **Natural regen doesn't cost hunger.** Java's real regen consumes 6.0
   exhaustion per HP healed — at the mining/jump exhaustion rates above, that's
   a substantial cost, enough to noticeably eat into the hunger a player just
   spent time regaining. This design's flat-timer hunger model has no
   exhaustion value for regen to consume, so regen is free here. This is the
   same simplification as point 1, restated at the specific spot it matters
   most (regen refilling health "for free" is the most player-visible
   consequence of not having exhaustion).
4. **Apple's saturation figure (2.4) is unused.** The hunger figure (4) is
   adopted directly since it matches; saturation has no equivalent value to
   receive it. Once meat items are defined in `plan-1.8.14-animals.md` /
   `plan-1.8.15-furnace.md`, their real hunger/saturation pairs (research
   supplied: raw porkchop 4/1.8, cooked porkchop 8/12.8, raw beef 4/1.8, cooked
   beef 8/12.8, raw chicken 3/1.2, cooked chicken 6/7.2, bread 5/6.0 — bread
   isn't a modeled item here) should have their hunger halves adopted the same
   way, saturation halves left unused for the same reason.

## Where the state lives, and its byte cost

New file, no `<3ds.h>`, host-testable: **`source/world/survival.h` / `.c`** — matching
the pattern of `physics.h`, `mining.h`, `daynight.h` (all pure C, no console-only
include, all host-linked into the test suite).

```c
typedef struct {
    uint8_t  health;         // 0..20
    uint8_t  hunger;         // 0..20
    uint16_t hunger_ticks;   // ticks accumulated toward the next -1 hunger
    uint16_t regen_ticks;    // ticks accumulated toward the next +1 / -1 health
    uint8_t  blocks_mined;   // wraps at 20, feeds the mining hunger surcharge
} Survival;

typedef struct {
    bool  falling;
    float peak_y;             // highest y reached since leaving the ground
} FallTrack;
```

`sizeof(Survival)` is 7 bytes rounded to 8 with padding; `sizeof(FallTrack)` is 8.
Both live in ordinary static/BSS memory — **not** charged against
`WORLD_BUDGET_BYTES` (`source/world/budget.h`), exactly like `EntityWorld` isn't
(`source/entity/entity.h`'s own comment: *"not connected to budgetClaim() in
either direction"*). At single-digit bytes this was never a real question, but it
is worth saying explicitly since an earlier pass of this task flagged it as an
open budget concern before finding the entity precedent that settles it.

`FallTrack` is **not persisted** — see below.

## What must be saved, and what an existing save without it gets

New sidecar, following the exact pattern already used three times over
(`world/playerpose.c`'s `posePath`, `world/inventory.c`'s `inventoryPath`,
`world/daynight.c`'s `pathFor` — all: build `<world_dir>/<name>`, check
`dirUsable`, write to `.tmp`, fsync/close, `remove()` the real file, `rename()`
the tmp over it; missing or corrupt file is normal, not an error):

`<world_dir>/survival.dat` — magic `"BSS1"`, version byte, `health`, `hunger`,
crc16, padded to 16 bytes like `daynight.h`'s `time.bin`. **~9 bytes of real
content, 16 on disk.**

**Why a new sidecar and not an extra field on `player.dat`:**
`playerpose.h` documents its own extension rule outright — *"if a sixth field is
ever added, version becomes 2 and a version-1 file is refused"* (`world/playerpose.h`
line ~68, `[read]`). That's the right rule for a pose (a missing pose isn't
meaningful data, it's just "we don't know where you were"), but it is the wrong
rule to inherit for health and hunger: bumping `player.dat` to version 2 would
refuse *every* existing save's **position** data the moment survival ships, purely
because an unrelated field was added to the same file. A separate sidecar keeps
`player.dat` untouched and gives an old save exactly the well-defined default
every other sidecar in this tree already gives a missing file.

**An old save with no `survival.dat`:** loads as `health=20, hunger=20` — the
same as a brand-new spawn. No migration step, no version flag, nothing to write
until the first `survivalTick()` actually changes something.

`FallTrack` is not saved at all — a save/load boundary is, by construction, a
safe moment to have "not currently falling," and inventing a persisted in-air
state to survive a save/quit/reload is complexity with no player-visible payoff
(nobody will notice that quitting mid-fall and reloading resets their fall
counter — they were mid-air a second ago and they're mid-air now, at most a few
pixels different).

## What goes on the wire, and whether the server has to ship first

`NetworldPlayerMeters` (`source/net/networld.h`) **already has** `float health`
and `float hunger` fields, gated by `BS_CLIENT_HAS_METERS` — currently `0`,
because, quoting the existing comment, *"sending zeros would be worse than not
sending"* (an empty `PLAYER_REPORT` would wipe a returning player's saved state
on the server). `[read]`

This means v1.8.13 needs **no new opcode**. The wire shape for health/hunger was
anticipated before this document existed. The only change is flipping
`BS_CLIENT_HAS_METERS` to `1` once `Survival.health`/`.hunger` are real numbers
instead of nothing, and populating the two floats from the `uint8_t` pair
(trivial upcast) in whatever function currently builds a zeroed
`NetworldPlayerMeters`.

Because the field already exists in the wire struct and an old client already
sends *something* in its place (the report just doesn't carry meters yet, it
doesn't omit the message), this does not need the "server ships first" rule that
a genuinely new C→S opcode would trigger (`source/entity/entity.h`'s own
Networked-entities section spells out that asymmetry for the entity-sync case;
the same reasoning applies here in reverse — nothing new is being asked of the
server, an existing field is finally being filled in).

## Per-tick cost at an Old 3DS's radius

`survivalTick()` touches exactly one thing: the local player's `Survival`
struct. It never reads a column, a chunk, or the entity store. Per call it is a
handful of integer comparisons and at most one `+1`/`-1` — work that happens
once per simulated tick (up to `TICK_MAX_CATCHUP_DEFAULT` = 4 catch-up ticks in
one frame, `source/world/tick.h`, `[read]`), never scaled by render radius, and
**identical in cost on an Old 3DS and a New 3DS**, because nothing about it
touches world-column data. `fallDamageUpdate()` similarly touches only `Body.y`
and `Body.on_ground`, once per rendered frame (not once per tick — see below).
Neither function's cost is measurable against the water-tick loop or the column
budget already in `main.c`'s frame.

## Fall tracking: a frame-rate function, not a tick-rate one

Fall distance is a continuous physical quantity — the peak height reached before
landing. Tracking it only once per 20Hz simulation tick risks missing the true
peak between ticks on a fast fall. `fallDamageUpdate(FallTrack*, const Body*)`
is designed to run once per **rendered frame** (same cadence as `bodyStep()`
itself), watching `Body.on_ground` and `Body.y`:

- `on_ground` was true, now false → `falling = true`, `peak_y = body->y`.
- `falling` and `body->y > peak_y` → `peak_y = body->y`.
- `on_ground` was false, now true → compute `peak_y − body->y`, apply the
  3-block-threshold formula above (unless `bodySubmerged(body)` at that instant),
  clear `falling`.

This needs no change to `physics.h`/`.c` — every field it reads (`y`,
`on_ground`) is already public, and `bodySubmerged()` is already an exported
predicate (`source/world/physics.h`, confirmed `[read]`).

## Eating, and the gap it exposes in the registry

`BlockDef` (`source/world/registry.h`) has no notion of "this is food" or "this
restores N hunger." The house rule for exactly this situation is stated in
`world/registry.c`'s own header comment on `.hardness`: *widen the predicate,
never hardcode by id* — the same lesson the v1.8.8 `BLOCK_COUNT` saga already
paid for once. The correct long-term shape is a `food_hunger` (and maybe
`food_health`) byte on `BlockDef`, populated per-row exactly like `.hardness` is
today, so a future food item (any of v1.8.14/15's meats) is food the moment its
registry row says so — no second table to keep in sync.

That field addition touches `world/registry.h` and `world/registry.c`, both of
which this document cannot edit (see LINES SOMEONE ELSE MUST ADD below). Until
it lands, `source/world/survival.h`/`.c` — a file this design **does** own — can
ship an interim hardcoded switch over `BlockId` (`BLOCK_APPLE → +4 hunger`,
and later the meat ids). This is stated plainly as a deliberate, temporary
deviation from house style, not a recommendation to keep it that way — the doc
for whichever version adds the registry field should delete the switch.

`eatAction()` (proposed, in `survival.c`): fires on `KEY_B` fresh-press
(edge-detected the same way `interactEdit()` already edge-detects PLACE against
`Interact.prev_keys` — `source/scene/interact.h` line ~140, `[read]`), only when
the selected hotbar slot holds a food id and `hunger < 20`. On success: consumes
one item from the slot (via the existing inventory removal path, not a new one),
adds the food's hunger value, clamped to 20.

## Difficulty — the decision this document is making

**Proposal: no selectable difficulty in v1.8.13.** One fixed ruleset, calibrated
to what classic Minecraft calls Normal: starvation floors at 1 hp rather than
killing outright; the numbers monsters will use later (v1.8.16) are Normal-tier
too, by reference forward from this decision rather than re-decided there.

The full sourced difficulty table, for reference, and to show exactly which row
this document is adopting `[research-given, Damage/Death]`:

| Difficulty | Starvation floor |
|---|---|
| Peaceful | hunger drain disabled entirely |
| Easy | stops at 10 HP |
| **Normal (adopted)** | **stops at 1 HP, cannot kill by itself** |
| Hard | no floor — starvation alone can kill |

Reasoning:
1. A difficulty selector is a real feature on its own — a new save field, a new
   menu screen, and *every* number this document and the monsters document pick
   would need 3–4 parallel tunings instead of one. That's a second feature
   riding in on this one's back.
2. Nothing in steve's stated intent for this pass ("survival with hunger, health
   and fall damage... zombies and skeletons... PS3-era feel") asked for a
   difficulty selector.
3. Honesty check on "legacy feel": PS3-era Minecraft *did* ship a difficulty
   selector. Omitting it here is a real deviation from strict legacy parity, not
   a legacy-authenticity claim — flagged as such rather than smuggled past as
   "that's what the classics did."

## Legacy feel over modern parity

Two explicit departures from modern (post-1.9-ish Java) Minecraft, taken toward
something simpler and older-feeling, per steve's stated preference:

1. **Hunger drains on a flat timer, not per-action exhaustion.** Modern Java
   tracks a float "exhaustion" value that accumulates per block walked (0.01),
   per block mined (0.005), per jump (0.2), per sprint-block (0.1 on top of
   walking) and converts it to hunger loss at a threshold. This build has **no
   sprint action at all** (`source/*` — the `ACTION_*` enum is exactly
   `MOVE_FORWARD/BACK/LEFT/RIGHT, JUMP, BREAK, PLACE`, confirmed by grep, no
   sprint anywhere) — which already removes the single largest term in that
   system. What's left of Java's model, minus sprint, is thin enough that
   tracking a per-tick float accumulator buys nothing a flat "−1 hunger every
   90 seconds, a bit faster if you've been mining" doesn't already deliver, at
   a fraction of the per-tick cost and none of the accumulator-precision
   questions. It is also closer to what the *earliest* console Minecraft
   editions did — early Xbox 360/PS3 Minecraft's hunger, when present at all,
   was simpler and less granular than Java's, which is exactly the "PS3-era,
   not modern" feel being asked for.
2. **No distance-scaled RNG on monster damage** (forward reference for
   v1.8.16, decided here because it's the same "simplicity over granularity"
   call): flat per-hit damage values rather than Java's randomized,
   range-scaled rolls. Cheaper, and matches an earlier, plainer console feel.

## Build order

- **P0 — `source/world/survival.h`/`.c`.** `Survival` struct, `survivalTick()`
  (hunger drain, mining surcharge, regen, starvation), `FallTrack` +
  `fallDamageUpdate()`. Host tests (`survival_test.c`) using the same red-arm
  sabotage methodology `plan-entities.md` and `plan-1.8.11-caves.md` already
  establish: assert the numbers above, then assert a deliberately-broken build
  (e.g. hunger that never drains, fall damage that never fires) actually fails
  the check that's supposed to catch it.
- **P1 — persistence.** `survivalSave`/`Load`, the `survival.dat` sidecar,
  tests for "missing file defaults to full," "corrupt file falls back safely,"
  matching `playerpose_test.c`'s and `inventory_persist_test.c`'s existing
  shape.
- **P2 — eating.** Interim food-value switch in `survival.c`, `eatAction()`,
  the `KEY_B` binding (glue in a `<3ds.h>`-guarded half, matching
  `interact.h`'s own split between pure state and console-only input).
- **P3 — main-loop integration.** Instantiate `Survival`/`FallTrack`, call
  `survivalTick()` from the existing per-simulated-tick loop, call
  `fallDamageUpdate()` once per frame, wire `eatAction()` into input handling.
  See LINES SOMEONE ELSE MUST ADD.
- **P4 — death and respawn.** Health-reaches-0 handling, teleport to the fixed
  spawn computation, reset both meters to full.
- **P5 — HUD.** Hearts/hunger icons — new art via `tools/make_atlas.py`, new
  draw calls in `scene/ui.c`/`ui_layout.h`. Genuinely a separate small phase:
  everything above is playable and testable (health/hunger visible only via a
  debug overlay or log) before a single pixel of UI exists for it.
- **P6 — wire.** Flip `BS_CLIENT_HAS_METERS`, populate the two floats. Depends
  on nothing above breaking; can land any time after P0.

## Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| A `KEY_B` binding collides with something added later before this ships (menu systems evolve). | Confirm with one more grep for `KEY_B` immediately before P2, not at design time — cheap, and it's the one fact in this doc most likely to drift. |
| Hunger drain feels wrong once actually played (too fast/slow — it's my own number, not measured). | It's a single constant (`hunger_ticks` threshold) in one file. Cheapest possible mitigation: playtest, adjust the constant, no architecture to revisit. |
| Fall damage double-fires or misses a landing because `fallDamageUpdate()` runs at frame rate while everything else in this design runs at tick rate. | Host-test it directly against a scripted `Body` trajectory (drop from a known height, assert exactly one damage event, of the right size, on the landing frame) — cheap because `Body` and `World` are both already host-testable. |
| The interim hardcoded food switch in `survival.c` is never replaced once the registry grows a real `food_hunger` field, and the two silently diverge. | One code comment in `survival.c` pointing at this document's "Eating" section, and a line item in whichever later document adds the registry field, saying explicitly "delete the switch in survival.c." |
| An old save's `player.dat` is fine but its `survival.dat` write races a crash mid-session (same crash-safety class every sidecar in this tree already handles). | Already mitigated by construction — the tmp/fsync/rename pattern is the same one `playerpose.c`/`inventory.c`/`daynight.c` already use and already have tests for; `survival.c` copies it, it doesn't invent a new one. |

## LINES SOMEONE ELSE MUST ADD

Nothing in v1.8.13's core design requires an edit outside files this document's
build order already proposes creating. Two spots are genuine exceptions:

1. **`source/main.c`**, inside the existing per-simulated-tick loop (the block
   that currently reads, per the earlier read of this file:
   `for (int t = 0; t < ticks_now; t++) (void)waterTick(&s_water, &s_world, WATER_TICK_BUDGET, onWaterChange, NULL);`
   near line ~4700) — add a `survivalTick(&s_survival, &s_world)` call inside
   that same `for` loop, and (outside the tick loop, once per frame, alongside
   wherever `interactAim()` is called) a `fallDamageUpdate(&s_falltrack,
   &player.body)` call. This is P3 above; it's listed here because `main.c` is
   under `source/` and this document does not edit it.
2. **`source/scene/ui.c`** / **`source/scene/ui_layout.h`** — the hearts/hunger
   HUD (P5) needs new draw calls in whichever function already draws the
   hotbar, and `tools/make_atlas.py` needs a heart/hunger-icon tile entry
   (following the exact "5 places" checklist `world/registry.c`'s own header
   documents for adding art-bearing content, minus the block-registry-specific
   steps that don't apply to a HUD icon).

## HIS CALL

1. **Hunger drain rate.** I picked "−1 every 90 seconds, full bar ≈ 30 minutes
   of play." This has no research anchor — it's a game-feel guess. Faster
   makes food management matter more; slower makes it nearly cosmetic. Easy to
   change later (one constant), but the *initial* pick is his to make or
   accept.
2. **Difficulty selector.** Recommended: ship Normal-only now (see above),
   revisit a real difficulty system as its own small version later if wanted.
   Alternative: build a minimal selector now and thread it through this
   document's numbers and v1.8.16's before either ships. I recommend the
   former — it's strictly less work now and doesn't block anything, since
   adding levels later only means turning single constants into small tables.
3. **Death and inventory.** Recommended default: **keep inventory on death** —
   nothing is lost. Reasoning: this codebase has no dropped-item-on-ground
   entity or pickup system at all today (mining goes straight from
   `breakComplete()` into the bag; nothing is ever placed in the world as a
   loose item). Clearing the inventory on death, as classic Minecraft does,
   would therefore be **strictly worse than vanilla** here — vanilla drops are
   at least recoverable by walking back; this build's would just vanish. I
   recommend keeping inventory intact until a real ground-item system exists
   as its own feature, at which point classic on-death dropping becomes safe
   to add. This is his call because it's a real behavior choice, not an
   implementation detail — flagging it rather than picking the "authentic"
   option silently.
   For reference, if a ground-item system is ever built and classic dropping
   is adopted: Java despawns dropped items **5 minutes** after the chunk
   holding them has been loaded (`[research-given]`, *Death*) — the timer to
   copy if this is revisited. Java also reverts XP to level 0 on death and
   drops `min(level × 7, 100)` as recoverable XP orbs (`[research-given]`,
   *Death*/*Experience*) — this is **not applicable** here, not a simplification:
   nothing in this codebase's four-document survey (`plan-1.8.13` through
   `plan-1.8.16`) found any XP or leveling system anywhere in `source/`, so
   there is nothing for a death to revert.
4. **Mining hunger surcharge (−1 per 20 blocks broken).** Another feel number
   with no anchor. Could just as easily be dropped entirely, folding mining
   into the flat timer with no separate accounting — simpler, one fewer field
   in `Survival`. I kept it because it gives mining a small, legible cost, but
   it's the easiest thing in this document to cut if he'd rather not track it.
   Now that the real figure is known (1 per 800 blocks, "Divergences" above) it's
   also worth considering whether to drop it for that reason specifically — my
   number was never trying to approximate Java's, and knowing that makes it
   easier to see it as a standalone game-feel choice rather than a rough copy.

## Sources

Every `[research-given]` tag above cites one of: minecraft.wiki *Hunger*,
*Food*, *Healing*, *Damage*, *Death*, *Tutorial:Hunger management*,
*Tutorial:Breaking a fall*, *Experience*. Supplied verbatim by the task's
coordinating agent as the outside research this document was originally
briefed against; not independently re-fetched or re-verified during this
citation pass.
