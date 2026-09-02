# v1.8.16 — Monsters

Zombies and skeletons, spawning in the dark and in caves, on the light and
space rules that make a torch worth placing.

This document does not implement anything. It builds on `docs/plan-entities.md`
(the generic entity store, already specified there and already **built** —
see `plan-1.8.14-animals.md`'s finding, restated below) and on
`docs/plan-1.8.13-survival.md` (health/damage/death, and the single-difficulty
decision this document inherits rather than re-argues).

## Provenance legend

Same as the other three documents in this set: `[read]`, `[research-given]`
(a figure supplied by the outside research pass this task was briefed
against, cited to the specific minecraft.wiki page), `[reasoned]`,
`[assumed]`, bare numbers are my own proposal.

**Citation pass note:** this document was originally written with no outside
research in hand. The research has since been supplied. Matching numbers get
the tag swapped and cited; numbers that differ keep their original value with
the divergence explained rather than silently corrected; numbers the research
doesn't cover, or mechanics the research describes that this design never
mentioned at all, are flagged honestly without redesigning anything — this is
a provenance pass, not a rewrite. All cited figures are Java Edition, Normal
difficulty (matching `plan-1.8.13-survival.md`'s adopted difficulty tier);
Legacy Console Edition has no separately documented monster-combat figures
anywhere the research turned up.

## The mechanic, as the player experiences it

- Underground, anywhere it's genuinely dark (no sky exposure and no light
  source nearby), zombies and skeletons periodically appear and come after the
  player.
- On the surface, the same thing happens at night — **once the day/night
  system is actually wired up**, which as of this writing it is not (see the
  finding below). Until then, surface spawning has no time-of-day signal to
  key off and doesn't function; cave spawning is unaffected, since caves don't
  need to know what time it is to be dark.
- A zombie walks toward the player and hits them in melee when close.
- A skeleton keeps its distance and shoots arrows.
- Placing a torch nearby stops new spawns in the lit area, and despawns
  existing monsters that are now standing in light.
- Killing either takes a handful of hits; neither drops anything at ship time
  (see HIS CALL).

## A finding this document depends on: the entity store exists, day/night doesn't run yet

Both facts matter to this document specifically, more than to the animals one,
because monsters' spawn rule is time-of-day-conditional on the surface (caves
aren't).

**The entity store is real and built**, not just designed:
`source/entity/entity.c`/`.h`/`entity_test.c` exist (277/304/840 lines,
`[read]`), implementing the fixed-pool `EntityWorld`, distance-decimated
`entityTick()`, `ENT_F_REMOTE`, and the rendering/sync seams
`plan-entities.md` specified. It is not yet instantiated or called from
`main.c` (zero matches for `entityWorldInit`/`entityTick`/`EntityWorld` there,
`[read]`) — `plan-1.8.14-animals.md`'s P1 is the phase that wires it in, and
this document's monsters ride on that same wiring rather than duplicating it.

**Day/night logic exists and is tested; it is a queued dependency this
document waits on, not a defect this document is reporting.** Per the
coordinating agent: the day/night module landed from a separate, parallel
work lane during this same session, and its integration lines into `main.c`
are already queued to be added — this is a normal hand-off between two pieces
of work landing in a coordinated order, not something broken that needs
fixing. `source/world/daynight.h`/`.c` fully implement a 24,000-tick day,
`dayNightSkyLight()` (returns 4–15, explicitly documented in its own header as
*"the value a spawn rule will read"*, `[read]`), and a `time.bin` sidecar with
the same tmp-fsync-swap pattern as every other save file in this tree. As of
this document being written: grepping `source/main.c` for `dayNightAdvance`
returns **zero matches** `[read]` — nothing calls it yet — and
`source/scene/chunk_render.c`'s `dayLevel` shader uniform remains pinned at
1.0, per that file's own comment (`[read]`). This corrects an earlier version
of `plan-entities.md §11.1`, which had stated "there is no day/night clock" —
that was true when written and is now imprecise: the clock exists and is
host-tested, it is simply waiting on its queued wiring step landing.

**Consequence for this document's scope:** cave spawning (pure darkness,
`skyLight == 0 && blockLight == 0`, independent of time of day by definition —
a cave has no sky exposure whether it's noon or midnight) works regardless of
whether the day/night wiring has landed by the time this version is built.
Surface night-spawning is **plausible but not guaranteed** by ship time — it
depends on that queued wiring step landing first, which per the grep above had
not happened as of this document being written. This document specifies the
surface rule as designed and ready; it flags, rather than assumes, that the
prerequisite is done.

**Divergence, darkness test:** `[research-given]` real Java's actual hostile
spawn check is looser than the strict `skyLight == 0 && blockLight == 0` rule
above — it's `skyLight <= 7 && blockLight == 0`, plus a further randomized 0–7
roll against the light level before a spawn attempt succeeds, meaning
hostiles can spawn in dim-but-not-pitch-black areas (up to light level 7) some
of the time, not only in total darkness. This strict rule is
`plan-entities.md`'s existing design, not something this document is
proposing or revising — surfaced here as a factual discrepancy because this
document is the one that cites and depends on that spawn rule, not as a
request to change a file this document doesn't own.

## Numbers, and where each one came from

| Value | Number | Provenance |
|---|---|---|
| Spawn check interval | every 40 ticks (2s), repeating | `[read]` — `plan-entities.md`'s existing monster spawn rule, cited not re-derived. |
| Darkness test | `skyLight == 0 && blockLight == 0` | `[read]`, same, and directly matches `dayNightSkyLight()`'s own stated purpose. |
| Light despawn check rate | 2 Hz | `[read]`, same — `plan-entities.md`'s existing torch-despawn design. |
| Kind values | `ENT_KIND_ZOMBIE=5, ENT_KIND_SKELETON=6` | Mine — continuing `plan-1.8.14-animals.md`'s `1..4` assignment for animals; `entity.h`'s `kind` field is opaque to that file (`[read]`), nothing above 4 is claimed yet by this pass. |
| Health | 20 per monster (both kinds) | `[research-given]` — exact match. Zombie and skeleton are both 20 HP / 10 hearts in Java, stable across editions. Also matches the 0..20 scale `Entity.health` already uses (`[read]`), a clean fit. |
| Zombie melee damage | 2 hp per hit, ~20-tick (1s) cooldown | Kept as-is. **Divergence:** `[research-given]` real Java zombie melee damage on Normal difficulty is 3 hp per hit (2 on Easy, 4–5 on Hard, scaling with difficulty — a mechanic this document's single-ruleset decision already excludes). My original 2 wasn't derived from anything — it was a flat feel-guess with no stated justification for landing on 2 specifically rather than 3 or any other small number. Kept per this pass's rule, but unlike the deliberate simplifications elsewhere in this arc, there's no real reasoning behind the original choice to defend — 3 would be the better-grounded number if he wants one. Fire-rate/cooldown (~1s) is not covered by the research as a fixed figure — Java's melee attack is player-initiated-adjacent (the zombie attacks on its own AI cooldown, not on a clean fixed timer the wiki states as a single number), so that part stays a proposal. |
| Skeleton arrow damage | 2 hp per hit | Kept as-is. **Divergence:** `[research-given]` real Java skeleton bow damage on Normal difficulty ranges roughly 3.5–5 hp depending on distance/charge (arrows lose damage over range and gain it from a full draw), call it ~4 as a rough midpoint — again well above my flat 2, and again the original 2 has no stated derivation behind it, same caveat as the melee row above. |
| Skeleton fire rate | fired every 60 ticks (3s) | `[research-given]` — exact match. Real Java skeletons fire roughly every 3 seconds (variance exists in the real AI's shot-timing but 3s is the commonly cited figure) — this is the one skeleton number in the table this pass confirms rather than flags. |
| AI states used | IDLE, WANDER, CHASE (both kinds); FLEE unused | `[reasoned]` from `plan-entities.md`'s 4-state design — monsters use CHASE, which `plan-1.8.14-animals.md`'s animals deliberately don't; FLEE has no natural use for either monster kind — `[research-given]` confirms neither classic MC zombie nor skeleton has a flee behavior. |
| Difficulty | inherits `plan-1.8.13-survival.md`'s single-ruleset decision (Normal-equivalent) | `[read]` (this document's own dependency) — not re-decided. Every number above is the Normal-tier figure; there is no Easy/Hard variant to pick between because that document already decided not to build a selector. |
| Zombie move speed (reference only, not modeled) | not specified in this design — see Legacy-feel item 2 | `[research-given]`: real Java zombie base speed is a 0.23 movement-speed attribute (same units-mismatch caveat as the animals document's wander-speed row — not directly convertible to this codebase's blocks/second). Not modeled here since this document doesn't propose a chase-speed number at all; included for completeness since the research covers it. |
| Zombie aggro/detection range (reference only, not modeled) | not specified in this design — "some detection range... not re-specified here" per the Zombies section below | `[research-given]`: real Java zombies notice and begin chasing a player from roughly 35 blocks away (exact figure varies slightly by source/version). This document explicitly deferred this number to `plan-entities.md`'s AI section rather than picking one itself; included here as a concrete reference figure for whoever specifies it. |

## Zombies: melee, chase

Reuses `plan-entities.md`'s CHASE state directly — once a zombie is within
some detection range of the player (a range that document's AI section should
define; not re-specified here since it's not monster-specific), it switches
from WANDER to CHASE and moves toward the player each due tick. On contact
(distance below some small threshold), it deals damage on its cooldown. No new
AI machinery beyond what `plan-entities.md` already designed — the only
zombie-specific content is the numbers table above.

**An omission this citation pass surfaced, honestly flagged rather than
designed:** `[research-given]` real Java (and most other editions) zombies
burn in direct sunlight and take periodic fire damage until they find shade or
are shot with a bow to put them out. This design has no mention of that
mechanic anywhere — not a deliberate simplification stated anywhere in the
original write-up, just never brought up at all, because it wasn't in the
outside research when the document was first written. Not designed here
either (this is a provenance pass, not a redesign), but worth being explicit
that it's a real gap between this document and the source material, not a
considered omission. If it matters, it interacts directly with this
document's own "surface spawning depends on day/night wiring" dependency —
burning would need the same day/night signal.

## Skeletons: ranged, and the one genuinely new mechanic this document adds

Nothing in `plan-entities.md` or `plan-1.8.14-animals.md` needed a projectile.
A skeleton's arrow is proposed as **another `Entity`** in the same store — not
a new subsystem:

- `ENT_KIND_ARROW` (=7, continuing the sequence), spawned via the existing
  `entitySpawn()` call with a small collision box (proposed 0.25/0.25/0.5 —
  `width`/`height`/`eye_frac`, all parameters `entitySpawn()` already accepts,
  `[read]` `entity.h`), velocity set toward the player at spawn time, `flags`
  carrying no `ENT_F_REMOTE` (it's locally simulated like everything else in
  this arc).
- No AI think callback beyond "keep flying" — an arrow's `ai_state` isn't
  meaningfully opaque behavior here, it's closer to "does nothing, physics
  carries it." It despawns on one of: `ai_timer` reaching zero (a fixed TTL,
  proposed 60 ticks — 3 seconds of flight, comfortably past any realistic shot
  distance), a hit-test against the player passing (proposed as a simple
  distance check each due tick rather than a real raycast, since an arrow
  entity already *has* a position each tick — no need for
  `plan-entities.md`'s `entityRaycast()`, which exists for the player's
  break-key-on-entity targeting, a different problem), or the entity's own
  `bodyStep()` reporting it stopped moving (arrow embedded in a block — gravity
  and collision already apply to it exactly like any other `Entity`, for free,
  because `Body` is embedded and every entity goes through the same
  `bodyMove()` the player does, per `entity.h`'s promise 2, `[read]`).
- This costs **zero new fields** on `Entity` and no change to `entity.h`/`.c` —
  every mechanism an arrow needs (custom collision box, gravity, despawn-on-
  timer via `ai_timer`, despawn-on-column-unload for free) already exists for
  a different reason and happens to fit exactly.

## Where the state lives, and its byte cost

Nothing new, same as animals: every zombie, skeleton and arrow is an `Entity`
in the existing `EntityWorld`. No new storage. The shared 24/48-slot cap
(`ENTITY_CAP_OLD`/`NEW`, `[read]`) is now genuinely contested — animals,
monsters and arrows all draw from the same pool, and arrows in particular are
short-lived but can spike the count during a fight (a skeleton firing every 3
seconds, each arrow living up to 3 seconds, means one active skeleton can have
1–2 arrows in flight at once — not a lot on its own, but additive with however
many animals and other monsters are also loaded). Flagged in Risks.

## What must be saved, and what an existing save without it gets

Nothing, for the same reason as `plan-1.8.14-animals.md`: `plan-entities.md`'s
entity layer isn't persisted in this arc at all. An old save with no monster
data loads with no monsters, and the spawn rule (re-)populates them as columns
load and time passes, identically to a fresh world. No migration.

## What goes on the wire

Nothing — inherits the same unsynced decision as animals, for the same reason,
not re-argued. Worth restating the multiplayer-visible consequence explicitly
here because it matters more for monsters than animals: **an unsynced zombie
is a zombie only the player who spawned it can see or be hurt by.** In a
multiplayer session, two players in the "same" dark cave are each fighting
their own private monsters, and neither can help the other survive one they
can't see. This is the same deferred decision `plan-entities.md` already
flagged for the whole system (*"a real visible inconsistency the owner should
confirm is acceptable"*, `[read]`), restated here because monsters are the
kind where "my friend got hit by something I couldn't see" is the most
noticeable version of it.

## Per-tick cost at an Old 3DS's radius

Governed entirely by `entityTick()`'s existing cost model, same as animals —
no new per-tick cost category. The one addition worth quantifying: arrows are
short-lived `Entity` occupants of the same 24/48-slot pool, so a fight
involving skeletons temporarily raises live-entity count without raising
per-entity cost — `entityTick()`'s cost is linear in occupied slots regardless
of what's occupying them (`[read]`, `entity.h`'s `EntityTickStats.live`
counter treats every kind identically). No separate accounting needed; the
existing `live`/`thought`/`decimated`/`steps` stats already reported by
`entityTick()` cover arrows for free.

## Combat, mirrored from animals

Player-attacks-monster reuses the same `entityRaycast()` +
`ACTION_BREAK`-fresh-press mechanism `plan-1.8.14-animals.md` already
describes and `plan-entities.md` already specifies — no new mechanism, only
new numbers (20 hp, per the table above, versus animals' 10).

Monster-attacks-player is new: this is the first thing in the whole four-
document arc where damage flows *toward* the player rather than away from an
animal. It depends directly on `plan-1.8.13-survival.md`'s `Survival.health`
field existing and being decrementable — `zombieTick()`/`arrowHitTest()` (both
proposed, living in a new `source/entity/monster.c` or similar, not yet named
definitively) call into `survivalDamage(&s_survival, amount)` (a small function
this document proposes adding to `survival.h`/`.c`, extending that document's
design rather than duplicating it — flagged there implicitly by this
dependency, explicit here since it's the first caller).

## Legacy feel over modern parity

1. **Flat, non-distance-scaled damage**, both melee and ranged — already
   established as the house call in `plan-1.8.13-survival.md`, applied here
   rather than re-decided.
2. **No difficulty-scaled numbers** — a direct consequence of that document's
   single-ruleset decision. Classic MC (every edition, including PS3-era) ties
   zombie damage, skeleton fire rate and even whether zombies spawn with armor
   or reinforcements to difficulty. This document deliberately doesn't model
   any of that — one flat set of numbers, full stop — which is a bigger
   simplification than "legacy feel" alone would justify; it's really "ship
   one thing that works," inherited honestly from the survival document's
   scoping call rather than dressed up as authenticity.
3. **No sound design mentioned.** Zombie groans, skeleton bone-rattle and bow
   twang are a real part of the classic-MC monster experience, and are
   entirely absent from this document because no audio subsystem is confirmed
   to exist in this codebase as of this pass (an audio-subsystem research/
   build effort was reportedly in progress elsewhere at the time this document
   was written, outside this task's brief — not verified directly here).
   Flagged as an absence, not a decision.

## Build order

- **P0 — done.** The generic entity store. Shared dependency with
  `plan-1.8.14-animals.md`, not re-built here.
- **P1 — shared with animals.** `EntityWorld` wired into `main.c`'s tick loop.
  If animals ship first, this is already done by the time monsters are built;
  if monsters ship first (unlikely given roadmap order, but not impossible),
  this document's P1 is identical to that document's P1 and should not be
  duplicated — build whichever lands first, the other references it.
- **P2 — zombie AI.** CHASE-state pursuit + melee contact damage, reusing
  `plan-entities.md`'s existing AI machinery.
- **P3 — skeleton AI + arrows.** Ranged fire-on-cooldown, the new
  `ENT_KIND_ARROW` spawn-and-flight behavior described above.
- **P4 — spawn rule.** The 40-tick darkness check, cave-only functional
  immediately, surface-conditional on v1.8.9's day/night wiring having landed
  — **verify this at implementation time, not assumed from this document**,
  since it may have landed by then.
- **P5 — player damage.** `survivalDamage()` added to `survival.h`/`.c`, wired
  from both monster attack paths.
- **P6 — art.** Zombie/skeleton models and textures, arrow model. Follows
  whatever pipeline shape `plan-entities.md`'s art-generation section already
  specifies.
- **P7 — light-despawn.** The 2 Hz torch-despawn check `plan-entities.md`
  already designed, first real exercise of it (animals never needed it, since
  they aren't light-sensitive).

## Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| Surface spawning silently does nothing because v1.8.9's day/night wiring hasn't landed by the time this ships, and nobody notices why. | One explicit runtime check at spawn-rule init: log/assert whether `dayNightAdvance()` has ever been called (a boolean the day/night module could expose cheaply) so a build with dead surface spawning says so instead of just quietly under-spawning. Cave spawning is unaffected either way, so the game still "has monsters" even if this slips — worth stating plainly during a playtest handoff rather than only in this table. |
| Entity-cap contention: animals + monsters + arrows all sharing 24 slots on an Old 3DS could mean a fight with a skeleton crowds out nearby animals (they simply can't spawn, or existing ones get evicted — eviction policy isn't decided by any document in this arc). | Cheapest fix: don't decide it in code by accident — `plan-entities.md` or a future document should state an explicit eviction/priority policy (e.g. "monsters may evict animals, animals never evict monsters") before this becomes a real problem at 24-slot Old 3DS radius. Flagged forward, not solved here. |
| A skeleton in melee range still tries to back away and shoot, per classic MC "keep distance" behavior, which needs pathfinding this codebase doesn't have (`plan-entities.md` explicitly says "no pathfinding"). `[research-given]` confirms this is a real, specific behavior in the source game, not a vague impression: Java skeletons actively maintain a preferred firing distance and will strafe/retreat when the player closes in, alternating between a "flee" and "shoot" sub-state at close range — a genuinely more complex AI than this document's CHASE-only design covers. Skeleton move speed itself is also not confidently pinned down by the research (sources vary slightly on the exact attribute value), so even the "how fast does it retreat" half of that behavior isn't cleanly citable, on top of the pathfinding gap. | Cheapest workable version: skeleton doesn't retreat, just stops advancing and shoots regardless of range once within firing distance — simpler than real kiting AI, and consistent with "no pathfinding" already being an accepted limitation for the whole entity system. This is now a confirmed, not just suspected, simplification relative to the real game — kept for the same reason (no pathfinding exists to build kiting on top of), just no longer a guess about what's being simplified away. |
| Arrow entities colliding with animals (should they hurt animals too, per classic MC's general "projectile hits whatever it hits" rule) is undecided. | Cheapest default: arrows only test against the player, not against other entities — avoids needing entity-vs-entity collision at all for v1.8.16, a real feature this document doesn't otherwise need. Flagged as a scope-narrowing choice, not an oversight. |

## LINES SOMEONE ELSE MUST ADD

1. **`source/main.c`** — nothing beyond what `plan-1.8.14-animals.md` already
   lists (the shared `EntityWorld` wiring). This document adds no further
   `main.c` hook beyond what that one specifies, since monster ticking rides
   the same `entityTick()` call.
2. **`source/world/survival.h`** / **`.c`** — the proposed `survivalDamage()`
   function. This document cannot edit those files (they belong to
   `plan-1.8.13-survival.md`'s scope) but depends on a function existing there
   that document doesn't currently specify. Flagged both here and implicitly
   in that document's own future revision.
3. **`tools/`** — model-generation entries for zombie, skeleton, and arrow,
   same pipeline dependency `plan-1.8.14-animals.md` already flags for its own
   four animals.
4. **`source/world/daynight.h`/`.c` or `source/main.c`** (whichever v1.8.9's
   own document specifies) — the day/night wiring this document's surface-
   spawn rule depends on but does not itself implement. Not this document's
   responsibility to fix; flagged as a hard dependency to verify before
   claiming surface spawning works.

## HIS CALL

1. **Entity-cap eviction policy** (animals vs. monsters vs. arrows sharing 24
   slots on an Old 3DS). Flagged in Risks as unsolved by any document in this
   arc. Needs a decision before both animals and monsters exist in the same
   loaded area at once, which is the normal case, not an edge case.
2. **Monster drops.** Proposed: neither zombie nor skeleton drops anything at
   ship time. Classic MC drops rotten flesh and bones/arrows respectively —
   `[research-given]` gives concrete reference figures if this is revisited:
   zombie drops 0–2 rotten flesh (and, rarely, an equipped item it spawned
   wearing — not applicable, since this design has no zombie equipment),
   skeleton drops 0–2 bone and 0–1 arrow, both drop counts boosted by
   Looting/higher difficulty (not applicable here, no enchantment system
   exists). None of rotten flesh, bone, or arrow are modeled items in this
   codebase, and inventing them was not part of steve's stated ask ("zombies
   and skeletons, including in caves" — no drop requirement stated).
   Skeleton-dropped arrows would actually be useful later if a player-usable
   bow is ever added (none exists today), which is worth deciding now versus
   revisiting once a bow exists.
3. **Whether surface spawning should be deferred entirely** until v1.8.9's
   wiring is confirmed landed, rather than building it now against a
   dependency that may not be ready. I lean toward building it now (it's a
   small, isolated check against `dayNightSkyLight()` that costs nothing if
   the clock never advances — it just never returns true) rather than blocking
   this version on another one, but it's worth an explicit yes since "ship a
   feature whose precondition might silently never fire" is exactly the kind
   of thing that should be a stated choice, not a default.
4. **Arrow-vs-animal collision.** Proposed off (arrows only hit the player).
   Cheap to add later if wanted; flagged as a scope-narrowing choice rather
   than an oversight, per Risks above.
5. **Zombie/skeleton melee and arrow damage (2/2 vs. the researched Normal
   figures of ~3/~4).** Surfaced by this citation pass — see the numbers
   table. Unlike most of this document's simplifications, the original 2/2
   values have no stated reasoning behind the specific number 2 beyond "flat
   and small" — worth a real look if he wants combat difficulty closer to the
   source material, since these are the two numbers in this whole four-
   document set with the weakest original justification.

## Sources

Every `[research-given]` tag above cites the same outside-research pass
supplied to the other three documents in this set: minecraft.wiki, covering
zombie/skeleton health, melee and ranged damage by difficulty, skeleton
fire-rate and kiting behavior, movement-speed attributes, aggro/detection
range, drop tables, the real hostile-mob darkness/light spawn test, and
zombie daytime-burning. Supplied verbatim by the task's coordinating agent;
not independently re-fetched or re-verified during this citation pass.
