# v1.8.14 — Animals

Pigs, cows, chickens, sheep: wandering, killable, meat drops. The first thing
built on top of the generic entity system.

This document does not implement anything. It builds on `docs/plan-entities.md`,
which already designed and fully specified the generic store these four kinds
sit on top of — the store, spawn/despawn rule shape, movement/AI states,
rendering seam, combat mechanism and multiplayer decision are **not re-decided
here**; they're cited and then this document adds only what's specific to
"pig/cow/chicken/sheep." Read `plan-entities.md` first; this document assumes it.

## Provenance legend

Same as `docs/plan-1.8.13-survival.md`: `[read]` (from code/docs), `[research-given]`
(a figure supplied by the outside research pass this task was briefed against,
cited to the specific minecraft.wiki page), `[reasoned]` (derived, or stable
classic-MC convention with no source in hand), `[assumed]` (unchecked), bare
numbers are my own proposal.

**Citation pass note:** this document was originally written without the
outside research in hand — every animal-behavior number below was `[reasoned]`
or mine. The research has since been supplied, cited to minecraft.wiki. Per
the citation-pass rule: where a number matches, the tag is swapped and cited;
where it differs, the original number is **kept** and the divergence is
explained rather than silently corrected (see "Divergences" below); numbers
the research doesn't cover stay labelled as proposals. Same Legacy-Console gap
as the survival document — all cited figures are Java Edition; there is no
separately documented Legacy Console figure set for animal mobs anywhere the
research turned up.

## A finding that changes this document's scope: the entity store already exists

`plan-entities.md` was written against a codebase with no `source/entity/`
directory (its own text says so). That is no longer true. **`source/entity/entity.c`,
`entity.h` and `entity_test.c` exist today** — 277, 304 and 840 lines
respectively `[read]` — and implement essentially all of `plan-entities.md`'s P0:
the fixed 48-slot `EntityWorld` pool, `entitySpawn`/`Despawn`/`Kill`, the
`Entity` struct (`Body` embedded, `yaw`, `id`, `ai_timer`, `kind`, `health`,
`flags`, `ai_state`), column-unload-triggered despawn, the distance-decimated
`entityTick()` with the "batched, not scaled" physics rule the header explains
in detail, `ENT_F_REMOTE`, and the rendering/networking seams as *comments*
describing the not-yet-built next layer.

**What is not done:** grepping `source/main.c` for `entityWorldInit`,
`entityTick`, `entitySpawn`, `EntityWorld`, `s_entities` returns **zero
matches** `[read]`. There is no `EntityWorld` instance anywhere in the running
game, no call to `entityTick()`, and no renderer reading the store. It is real,
tested, host-buildable code that nothing calls yet — the same shape as the
day/night clock's situation (see `plan-1.8.16-monsters.md`'s equivalent
finding for that system).

> **⚠ CORRECTION [2026-09-03] — the paragraph immediately above is FALSE as of
> today, and was already false when this document shipped.** The same grep run
> against `source/main.c` on branch `v1.8.14` returns five matches, not zero:
> `s_entities` declared at `main.c:113`, `entityWorldInit(&s_entities,
> entityCapFor(hwIsNew3ds()))` at `main.c:4360`, `entityTick(&s_entities,
> &s_world, ...)` at `main.c:5426`, `entityModelDraw(view, &s_entities)` at
> `main.c:2715`, and a comment at `main.c:5425`. The store is instantiated,
> sized per console, and ticking every frame in the shipping build.
>
> **So P1 below is done as well as P0**, and any lane reading this document
> should not re-wire `main.c`. Note also that the helper is spelled
> `hwIsNew3ds()`, not `isNew3DS()` as the LINES SOMEONE ELSE MUST ADD section
> below writes it, and the init sits at `:4360` rather than near line 3652.
>
> This is recorded rather than deleted because the error is instructive: a plan
> that asserts absence from a grep is only as good as the day the grep was run,
> and this one cost a lane real time. Read to the end of a block before trusting
> its opening.

This means v1.8.14's build order is shorter than `plan-entities.md`'s P0–P7
implied when it was written: **P0 (the store itself) is done.** What remains is
wiring it into `main.c`, defining what a pig/cow/chicken/sheep's `kind` and
`ai_state` values mean, their spawn rule, their art, and their drops — plus
whatever `entity.c`'s current form is still missing against `plan-entities.md`'s
full spec (not verified line-by-line here — `entity.c`'s internals were not
audited in depth; flagged as a thing to re-check at implementation time, per
`plan-1.8.11-caves.md`'s own stated precedent of re-reading a file under
concurrent edit rather than trusting a summary of it).

## The mechanic, as the player experiences it

- Walking into a grass/dirt/sand area at daylight, herds of 2–4 of one animal
  kind are already there or appear shortly after the area loads.
- Animals wander at random, never approach or flee unprovoked, and don't attack.
- Hitting one with the break key (pointed at the animal instead of a block)
  damages it; a few hits kill it.
- A dead animal disappears and drops 1–3 raw meat items on the ground under it
  — pickable up like any block drop.
- Animals don't chase, don't path around obstacles, and can wander into hazards
  (off ledges, into water) with no special handling — this is deliberate, see
  "Legacy feel" below.

## Numbers, and where each one came from

| Value | Number | Provenance |
|---|---|---|
| Herd size | 2–4, one kind per herd | `[read]` — `plan-entities.md`'s existing animal spawn rule. Also `[research-given]`: Java's natural-generation group size for pigs/cows/chickens/sheep is 4 per group (some sources say 2–4 depending on biome/patch), so `plan-entities.md`'s 2–4 sits inside the real range rather than outside it — treated as corroborated, not contradicted. |
| Spawn trigger | once per column load, on grass/dirt/sand with sky light ≥9 | `[read]` — `plan-entities.md`. Light threshold `[research-given]`: Java spawns overworld passive mobs on grass blocks at light level ≥9, matching exactly. **Divergence:** the surface (grass/dirt/sand) does not match — real Java only spawns these mobs on grass blocks, not bare dirt or sand. `plan-entities.md` made the wider call and this document inherits it rather than re-deciding it; noted here as the discrepancy it is, not corrected, since this document doesn't own that design. |
| Kind values | `ENT_KIND_PIG=1, ENT_KIND_COW=2, ENT_KIND_CHICKEN=3, ENT_KIND_SHEEP=4` | Mine — `entity.h`'s `kind` field is deliberately opaque to that file (*"Otherwise opaque to this file"*, `[read]`); nothing has claimed values 1–4 yet (only `ENT_NONE=0` is defined). Assigned in ascending order matching the order steve listed them. Not something research covers — implementation detail. |
| Health | 10 per animal, flat across all four kinds | Mine, kept as-is. **Divergence:** `[research-given]` real per-kind health is pig 10, cow 10, chicken 4, sheep 8 — so my flat 10 happens to match pig and cow exactly, overshoots chicken by 6 (2.5x), and overshoots sheep by 2. This wasn't a guess that missed by accident — the flat-number call was deliberate (see original reasoning: one shared value instead of four, matching the "no distance-scaled RNG, simplicity" precedent from the survival document) and I'm keeping it for that reason. Worth flagging that chicken in particular is a much easier kill in the real game (4 hp = one hit at my stated 4 dmg/hit) than this design gives it. |
| Damage per hit | player's fists deal 4 hp per hit to an animal (kills a 10-hp animal in 3 hits) | Mine, tuned so a kill takes a handful of hits (feels like an action) without turning into a grind — a game-feel number, not anchored. Not covered by the research (Java's bare-hand damage varies by version/enchantment and isn't a fixed animal-specific number) — stays a proposal. |
| Contact/attack behavior | none — animals never attack | `[reasoned]`, now `[research-given]`-confirmed: all four are always-passive mobs in every edition, never deal contact damage. |
| Meat drop count | 1–3 raw meat, uniform random, same range for all four kinds | Mine, kept as-is. **Divergence:** `[research-given]` real per-kind drop ranges are pig 1–3 raw porkchop, cow 1–3 raw beef, sheep 1–2 raw mutton, chicken exactly 1 raw chicken (not a range). My uniform 1–3 matches pig/cow exactly, overshoots sheep's ceiling by 1, and turns chicken's fixed drop into a 1–3 range it doesn't have in the real game. Kept flat for the same one-number-not-four simplicity reason as health; flagged so the divergence is visible rather than assumed to match. |
| Meat item per kind | pig→raw porkchop, cow→raw beef, chicken→raw chicken, sheep→raw mutton | `[research-given]` — exact match to Java's naming/pairing. Wool from sheep is explicitly **out of scope** — see HIS CALL. |
| Wander speed | reuses `PLAYER_WALK_SPEED` (4.3, `source/world/physics.h`) at a slower multiplier, e.g. 0.4x ≈ 1.7 blocks/s | Mine, grounded in a `[read]` constant. Slower than the player's walk so herds don't sprint across a loaded area in seconds. Research gives movement-speed *attribute* values (pig 0.25, cow 0.2, chicken 0.25, sheep 0.23 — vs. zombie 0.23 normal / 0.35 aggro'd) `[research-given]`, but these are Minecraft's internal attribute units, not blocks/second, and the research didn't supply a conversion. No direct substitution is possible without guessing at the conversion factor, which would just replace one unmeasured number with another; I kept my `PLAYER_WALK_SPEED`-fraction design since it's grounded in this codebase's actual physics constant instead. For relative ordering only: pig and chicken share the fastest attribute value, sheep and zombie's base speed are close together, cow is slowest of the four animals. |
| AI states used | IDLE, WANDER only (no FLEE, no CHASE) | `[reasoned]` from `plan-entities.md`'s 4-state design. The no-FLEE choice is a deliberate divergence from real behavior — `[research-given]` confirms Java's passive mobs do run a few steps in a random-ish direction away from the attacker for roughly a second when hit — see "Legacy feel" item 1 below, unchanged from the original reasoning but now backed by a citation instead of a guess at what classic MC does. |

## Divergences from the sourced Java figures, explained

1. **Flat 10 hp for all four kinds vs. real pig 10 / cow 10 / chicken 4 / sheep
   8.** Kept flat deliberately — one shared constant instead of a four-entry
   table, matching the "no distance-scaled RNG, simplicity" call the survival
   document already made for a different system. The real cost of keeping it:
   chicken becomes noticeably tougher here (10 hp, 3 hits at 4 dmg/hit) than in
   the real game (4 hp, effectively 1 hit), and sheep gets a small bump too (10
   vs 8). If steve wants kind-accurate feel this is the cheapest single change
   in the document — a 4-entry table instead of one constant — but it wasn't
   asked for and I'm not making it unasked.
2. **Uniform 1–3 meat drop vs. real per-kind ranges (chicken fixed at 1, sheep
   1–2, pig/cow 1–3).** Same reasoning as health — one range instead of four.
   Chicken is the biggest divergence here: real chicken always drops exactly
   one raw chicken, this design can drop up to three.
3. **No flee-on-hit vs. real passive-mob flee behavior.** This one was already
   flagged as a scope simplification before the research existed (see "Legacy
   feel" item 1) — the research just confirms the real behavior exists and
   gives it a name (a short away-from-attacker run, roughly a second) rather
   than changing the reasoning for leaving it out.
4. **Grass/dirt/sand spawn surface vs. real grass-only.** This is
   `plan-entities.md`'s call, not this document's — surfaced here because it's
   the kind of number a citation pass should flag, not because this document
   is proposing to fix it.
5. **Wander speed left as a `PLAYER_WALK_SPEED` fraction, not converted from
   the research's attribute values.** Not really a divergence in the same
   sense as the others — there's no real number to diverge *from* here, since
   Minecraft's attribute units and this codebase's blocks/second units don't
   have a stated conversion. Documented in the numbers table above.

## Multiplayer: inherited, not re-decided

`plan-entities.md` already made this call for the whole entity system: entities
are **client-local and unsynced** for now, flagged in that document as *"a real
visible inconsistency the owner should confirm is acceptable"* `[read]`. Animals
inherit it verbatim — two players in the same world will each see their own,
independently-simulated herd, which can and will diverge. This document is not
re-opening that decision; it's re-stating it here because it's the single most
visible consequence a playtest of v1.8.14 will surface, and it needs to be
obviously not-a-bug when it's noticed.

## Where the state lives, and its byte cost

Nothing new. Every animal is an `Entity` in the existing `EntityWorld`, not
charged against `WORLD_BUDGET_BYTES`. Measured, not the earlier ~1,744-byte
estimate — figures supplied directly from an ARM-target build by the
coordinating agent, `[read]` (measured on-device figures, not re-measured
independently during this pass): `sizeof(Entity)` = 60 bytes, `sizeof(Body)` =
48 bytes (embedded inside `Entity`), `sizeof(EntityWorld)` = 2,892 bytes total.
`ENTITY_SLOTS` (the array's fixed allocation) = 48 on both consoles; `cap`
(`ENTITY_CAP_OLD` = 24, `ENTITY_CAP_NEW` = 48) limits how many of those 48
slots are allowed to be *occupied* at once, not how many are allocated — the
array itself is always the full 2,892 bytes regardless of which cap is active.
`entity.c`'s compiled ARM `.text` size is 1,688 bytes. This document adds
**zero new storage** — only meaning attached to fields that already exist
(`kind` values, `ai_state` values, `health` starting value).

The one new piece of state is transient and tiny: a per-column "have I already
tried to spawn a herd here" flag, needed so the "once per column load" rule
doesn't refire every time a column is revisited. `plan-entities.md` already
specifies this as part of the spawn rule; the storage question (a bit on
`Column`, or a side table keyed by column coordinate) is left to
`plan-entities.md`'s own design, not re-litigated here.

## What must be saved, and what an existing save without it gets

**Nothing is saved for animals in this version.** Consistent with
`plan-entities.md`'s unsynced-and-uncommitted-to-persistence design for the
whole entity layer at this stage: an `EntityWorld` is populated fresh from the
spawn rule as columns load, and a saved world with no animal data in it (every
world today) spawns its first herds exactly like a brand-new world would —
there's no migration question because there's no format to migrate. Quitting
and reloading loses whatever herds existed and lets the spawn rule re-run as
columns reload. This is a real, visible behavior (animals aren't "remembered"),
flagged as a HIS CALL item below rather than silently accepted.

## What goes on the wire

Nothing, for the same reason as persistence: `plan-entities.md`'s unsynced
decision covers this. If/when entity sync is built (that document's "Networked
entities" section already sketches the wire shape — a 12-byte-per-entity
`BS_APP_ENTITY_SYNC` record, S→C safe to ship client-first, C→S needing the
server first), animals ride on it identically to monsters; nothing about
"animal" specifically changes the wire question.

## Per-tick cost at an Old 3DS's radius

Entirely governed by `entityTick()`'s existing, already-measured cost model
(`entity.h`, `[read]`): distance-decimated via `tick.h`'s `tickPeriodForDistSq`
(near entities tick every simulation tick, far ones at 2 Hz), capped at
`ENTITY_CAP_OLD = 24` live entities on an Old 3DS versus `ENTITY_CAP_NEW = 48`
on a New one. Animals don't add a new per-tick cost model — they use the one
that file already built and documented, including its explicit finding that
distance decimation saves dispatch cost but **not** collision-sweep cost
(`entity.h`'s own comment: *"a body that travels the same distance tests the
same cells however the time is chopped up"*). The one animal-specific addition
is a `think` callback (wander-direction picking on `ai_timer` expiry) — O(1)
per due entity, a random direction pick and a `yaw`/velocity write, negligible
next to the `bodyStep()` call it precedes.

## Combat: reusing `ACTION_BREAK`, per `plan-entities.md`

Killing an animal reuses the existing break-key fresh-press
(`INTERACT_KEY_BREAK` = `KEY_X`, `source/scene/interact.h`, `[read]`) via a new
`entityRaycast()` function `plan-entities.md` already specifies. This document
adds nothing to that mechanism except the specific numbers above (10 hp, 4
dmg/hit, no knockback modeled — see HIS CALL).

## Legacy feel over modern parity

1. **No flee behavior.** Classic Minecraft (all editions) makes passive mobs
   run a few steps away when hit. `plan-entities.md`'s AI design includes a
   FLEE state precisely so this is available; animals in this document
   deliberately **don't use it** — they keep wandering/idling even while being
   killed. Reasoning: FLEE needs a "away from the last hit" direction
   computation every time it retriggers, and buys almost nothing when the
   player is standing in melee range anyway (a hit-and-flee animal is caught
   again in 1–2 steps at these move speeds). This is a genuine simplification,
   not a legacy authenticity claim — legacy console Minecraft did have flee
   behavior — flagged as a scope call, see HIS CALL.
2. **No breeding, no growth, no baby animals.** Steve's stated ask was
   "wandering, killable, meat drops" — nothing about breeding. Explicitly out
   of scope, not a legacy-vs-modern call at all (breeding predates even the
   earliest console editions), just not asked for.
3. **Flat damage-per-hit, no knockback.** Same "simplicity over granularity"
   call already made in the survival document for monster damage — extends
   naturally to animals.

## Build order

- **P0 — done.** `source/entity/entity.h`/`.c` (the generic store). Verify at
  implementation start rather than trust this summary — re-read the current
  `entity.c` before writing against it, since it may still be under active
  work (see the finding above).
- **P1 — wire the store into `main.c`.** Instantiate `EntityWorld`, call
  `entityCapFor(is_new_3ds)` (`entity.h`, already exposed) to size it, call
  `entityTick()` from the tick loop. This is infrastructure `plan-entities.md`
  already designed; it has no animal-specific content, but nothing after it
  can be tested without it landing first. See LINES SOMEONE ELSE MUST ADD.
- **P2 — spawn rule.** Herd spawn on column load per `plan-entities.md`'s
  already-specified rule, hooked at `worldColumnCreate()`
  (`source/world/world.c`, `[read]` — confirmed this is the column-load
  entry point, called from both `worldgen.c` and `worldgen_density.c`).
- **P3 — wander AI.** IDLE/WANDER think callback, the one animal-specific
  logic this document adds.
- **P4 — combat.** `entityRaycast()`, break-key-on-entity handling, health
  decrement, death → despawn.
- **P5 — drops.** Meat item drop on death. **Depends on meat items existing**
  — either stubbed here as new core block/item rows (see LINES SOMEONE ELSE
  MUST ADD) or deferred until v1.8.15's furnace document defines them, since
  raw meat only becomes meaningful once it can be cooked. Recommend landing
  the meat item rows in this phase regardless of furnace timing — a raw,
  uncookable meat item is still a valid inventory item today (matches how
  `BLOCK_APPLE` shipped carryable before eating existed, `[read]`
  `registry.c`'s own note on the apple row).
- **P6 — art.** Four low-poly animal models + textures. `plan-entities.md`'s
  "art-generation plan" section already covers the pipeline shape; not
  re-derived here.
- **P7 — rendering.** The seam `entity.h` already documents in comments (one
  linearAlloc VBO, drawn from `drawEye()`, no draw-list bookkeeping) — first
  real exercise of that seam.

## Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| `entity.c`'s current implementation has drifted from `plan-entities.md`'s spec in some way not caught by this document's read of the header alone. | Re-read `entity.c` in full immediately before P1, and run `entity_test.c` (840 lines, already exists) to see what it currently proves before adding anything on top. |
| Two players' unsynced herds diverging reads as a bug to steve during a multiplayer playtest, since nothing in the UI says "this is expected." | Cheapest fix is a one-line debug-overlay note ("entities: local-only") rather than building sync — costs nothing, defers the real decision to whenever multiplayer entity sync is actually built. |
| 24 entities on an Old 3DS fills up fast if animal herds and (later) monsters compete for the same 24-slot cap. | Not a v1.8.14 problem in isolation (only animals exist at this point), but worth flagging forward: `plan-entities.md`'s cap is shared across every kind, so v1.8.16 (monsters) needs to account for animals already present. Cited forward in the monsters document. |
| Meat item ids need new core registry rows (see below), which this document cannot add. | Already the plan — flagged explicitly as LINES SOMEONE ELSE MUST ADD rather than worked around. |
| `[research-given]`: real Minecraft's two editions cap total mob count very differently — Java scales a single global cap by loaded-chunk count (`mobCap × loadedChunks / 289`), which the research explicitly characterizes as unbounded work relative to view distance; Bedrock uses a flat global cap (200) plus per-9×9-chunk density sub-caps (4 animals / 8 monsters on the surface, 16 monsters in caves), which the research recommends as the shape worth copying. `plan-entities.md`'s flat `ENTITY_CAP_OLD`/`NEW` design is closer to Bedrock's shape (a single bounded number, not scaled by view distance) but has no per-region density mechanism — nothing stops every one of the 24/48 slots from being spent on herds clustered in one loaded area, starving spawns everywhere else. | Not fixed here — flagged forward, same as the row above. A per-region density cap is a real addition neither this document nor `plan-entities.md` currently plans; cheapest partial mitigation if it ever matters is a per-column (not per-region) spawn-attempt flag, which `plan-entities.md` already has for a different reason (avoiding respawn-on-every-load) and could plausibly be reused. |

## LINES SOMEONE ELSE MUST ADD

1. **`source/main.c`** — instantiate `EntityWorld s_entities;`, call
   `entityWorldInit(&s_entities, entityCapFor(isNew3DS()))` at startup (near
   wherever `tickClockInit()` is currently called, ~line 3652 per the earlier
   read of this file), and add `entityTick(&s_entities, &s_world, tick, px, pz,
   think, NULL)` inside the existing per-tick loop, alongside the proposed
   `survivalTick()` call from `plan-1.8.13-survival.md`.
2. **`source/world/block.h`** and **`source/world/registry.c`** — four new
   core item rows for raw porkchop/beef/chicken/mutton, following the exact
   five-step checklist `registry.c`'s own header already documents (*"⚠ ADDING
   A BLOCK — the five places"*): the `kCoreDefs` row (id 27–30, next free after
   `BLOCK_APPLE=26`)
   <!-- ⚠ CORRECTION [2026-09-03]: 27-30 is WRONG and no longer free. Between
        this document being written and v1.8.14 starting, BLOCK_TORCH took 27
        and the six ores took 28-33 (verified by reading the enum in
        source/world/block.h). The meat rows are ids **34-37**:
        BLOCK_RAW_PORKCHOP 34, BLOCK_RAW_BEEF 35, BLOCK_RAW_CHICKEN 36,
        BLOCK_RAW_MUTTON 37. Their ATLAS SLOTS are a different number again --
        38-41 -- because the tile sheet and the id space are not aligned; see
        the note at block.h:217. Do not assume id == slot. -->
   including `.hardness` (these aren't targetable/breakable
   blocks in the world, only carryable items — see the apple row's own
   precedent for a carryable, non-placed-as-terrain item, though unlike apple
   these would never be placed in the world at all, only ever handed out by
   `entitySpawn`'s drop logic; whether they need a placeable form or can be
   inventory-only is a registry-level question for whoever adds the rows), the
   `world/block.h` literal id + `_Static_assert` entry, the `BTEX_*` tile
   constant, and the `tools/make_atlas.py` tile art. This document cannot
   touch any of those three files.
3. **`tools/`** — a new model-generation script for the four animal meshes
   (mirroring whatever `plan-entities.md`'s art-generation plan already
   specifies for the pipeline shape — not re-derived here since that document
   owns it).

## HIS CALL

1. **No persistence for animals in this version** — quitting and reloading
   loses every existing herd and lets the spawn rule refire fresh. This is
   `plan-entities.md`'s existing decision, inherited rather than re-made, but
   it's worth surfacing again here because a playtest of *this specific
   version* is where it becomes visible for the first time ("I killed all the
   pigs near my base, I quit, I came back, there are new pigs" — is that fine,
   or does he want herds to feel permanent?). If permanence matters, it's a
   real addition (column-tagged spawn history, or full entity persistence)
   that neither this document nor `plan-entities.md` currently plans.
2. **No flee-on-hit.** Flagged above as a simplification. Cheap to add later
   (reuse the FLEE state `plan-entities.md` already reserved) if the wandering
   feels too passive when being killed.
3. **No wool from sheep.** Steve's stated ask was specifically "killable for
   meat" — wool implies a shears/tool interaction this codebase has no
   precedent for (no tools exist at all yet, `world/inventory.h`,
   `[read]`, confirmed in the survival document too). Left out entirely rather
   than half-built as a passive drop; his call whether sheep should drop wool
   automatically (no shears) as a cheaper substitute, or wait for a real tool
   system.
4. **Herd cap vs. later monster cap.** Flagged in Risks — worth deciding now
   whether animals and (v1.8.16) monsters should split the 24/48 entity cap
   evenly, or whether animals should be capped lower to leave room for
   monsters once both exist simultaneously. Not urgent for this version alone,
   but the number picked here (or left undecided) constrains v1.8.16.
5. **Kind-accurate health/drop numbers vs. the flat shared values.** Surfaced
   by the citation pass (see "Divergences" above) — real per-kind numbers are
   known now (health pig 10/cow 10/chicken 4/sheep 8; drops pig 1–3/cow
   1–3/sheep 1–2/chicken exactly 1). Whether that granularity is worth a
   4-entry table instead of one constant each is his call — the flat design
   still works, this just wasn't a blind guess anymore once the real numbers
   were in hand.

## Sources

Every `[research-given]` tag above cites the same outside-research pass
supplied to `docs/plan-1.8.13-survival.md`: minecraft.wiki, specifically
material covering passive-mob spawning, health, drops, group/herd size,
movement-speed attributes, and flee-on-hit behavior for pig/cow/chicken/sheep,
plus the Java-vs-Bedrock mob-cap comparison. Supplied verbatim by the task's
coordinating agent; not independently re-fetched or re-verified during this
citation pass. The `sizeof`/ARM `.text`-size figures in "Where the state
lives, and its byte cost" are a separate, measured source — an ARM-target
build read by the coordinating agent, not a wiki citation.
