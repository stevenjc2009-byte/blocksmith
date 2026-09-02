# Cave feel research: Legacy Console Edition, for Blocksmith

Research only — a design/experience brief, not code or pseudocode. Nothing under
`source/` was read for the purpose of changing it, and nothing was built or run.
This file is the only thing this pass wrote.

**Companion document:** `docs/research/caves-legacy-console.md` already covers the
carver *algorithm* (rarity constants, tunnel-walk math, y-clamps, ore/lava/water
placement rules, and Blocksmith's own measured constants). This file does not
repeat that content. Where a number from that document matters to the *feel*
argument being made here, it is referenced by section (e.g. "per the companion
doc §2") rather than re-derived. This file's job is the other half of the
request: what legacy caves were like to actually walk through, and why — pacing,
sightlines, danger, navigation, landmarks — so that gets built even if every
carver constant lands somewhere different than a spec would predict.

**Provenance tags used throughout:** **[documented]** (a specific, checkable
source states the fact directly), **[community-reported]** (multiple players/
forum threads/wikis converge on the same account but no single authoritative
source pins the exact number or date), or **[inferred]** (my own reasoning from
documented facts, shown worked, not independently found stated anywhere). A claim
with no tag does not belong in this document.

---

## 1. What Legacy Console Edition caves actually were, concretely

**The headline fact, and why it matters more than any single number below:**
Legacy Console Edition (Xbox 360, PS3, then Xbox One/PS4/Wii U/Vita/Switch) ran a
carver frozen at a very early point in Minecraft's development — Beta 1.6.6
launch, catching up toward Beta 1.7.3 parity — and **never received the October
2013 PC release "1.7.2" terrain/cave overhaul**, let alone the 2021 Caves &
Cliffs rewrite. **[documented, per companion doc §1, citing
minecraft-archive.fandom.com/wiki/Minecraft:_Xbox_360_Edition and
co-optimus.com/article/8239]**

This matters because Mojang's own October 2013 "1.7.2" PC release — nicknamed
**"The Update that Changed the World"** for its terrain generator, not its caves
— quietly shipped a major cave-density *reduction* as part of the same patch,
and it is a big one:

- Per a modder (TheMasterCaver) who reverse-engineered and compared both
  generators: the size and density of individual cave systems dropped **nearly
  3-fold** in 1.7.2 versus 1.6.4, offset by more but smaller systems, so overall
  carved volume fell by "only" ~16% while raw system count fell 23% — meaning the
  *character* changed even more than the *volume*: fewer huge sprawling systems,
  more small disconnected ones. **[community-reported: minecraftforum.net thread
  "Did they intentionally nerf the caves?", citing TheMasterCaver's own analysis
  and mod]**
- The same source states the largest possible single cave system became roughly
  **154x rarer** after 1.7.2 (1-in-23,625 chunks versus 1-in-960,000 in the old
  generator — read that ratio as "went from very rare to vanishingly rare," the
  precise chunk counts themselves are the modder's own reverse-engineered
  figures, not an official Mojang statement, hence community-reported rather than
  documented). **[community-reported: same thread]**
- This was contentious enough at the time that multiple long-running Minecraft
  Forum threads exist purely to litigate it ("Did they intentionally nerf the
  caves?", "Cut down on the caves?", "Caves in the later updates"), and it is
  cited as one motivation behind Mojang's eventual 2021 Caves & Cliffs rewrite.
  **[community-reported]**

**The inference that actually shapes this brief:** LCE's carver baseline (Beta
1.6.6 → 1.7.3-equivalent, established 2011) is **two years earlier** than the
October 2013 patch that nerfed cave density on PC. Nothing found this session
documents 4J Studios ever backporting that PC terrain-generator change to the
console line — the console line's own generation pipeline is separately
described (companion doc §1) as staying on its own frozen Beta-era baseline for
structural features (ravines, biome variety) that it *caught up on later*, not
as re-syncing to PC's evolving overworld generator. **This means the caves the
project owner remembers from "legacy Minecraft PS3 or early PS4" were very
plausibly running the OLDER, denser, more interconnected pre-nerf carver the
entire time LCE was alive (2012-2019) — not the sparser carver most current
"vanilla survival" guides and YouTube content describe, which is the post-2013
PC baseline almost everyone who plays Minecraft today has only ever known.**
**[inferred — flagged explicitly because no source found this session states
"LCE never got the 1.7.2 cave nerf" directly; it is a strong but unconfirmed
inference from two independently documented facts (LCE's frozen early baseline,
and the 2013 nerf's date) with no evidence found either way about a backport]**

**Practical consequence for Blocksmith:** if the target is "what caves actually
felt like on a PS3 or 360," the companion document's Lineage A (Beta 1.7.3, i73
project) is the *more* legacy-accurate of its two sourced lineages, not an
equally-valid alternative to Lineage B (1.7.10 decompile) — Lineage A predates
the nerf, Lineage B (1.7.10, 2013) postdates it. Where the two disagree on
density (rarity 1-in-15 vs 1-in-7, tunnel-count bound 40 vs 15), **lean toward
Lineage A's shape, or denser than either**, not toward the sparser modern
baseline. This also lines up with — and now explains *why* — Blocksmith's own
existing noise-field tuning already targets a 99.2%-connected, sprawling result
(companion doc §7): that connectivity target is accidentally already pointed at
the right era.

### The finite-world effect on feel, separately from carver density

LCE worlds were bounded — Classic 864x864 up to Large 5120x5120 depending on
platform/generation, with an invisible edge and a water-moat blend at the
boundary. **[documented, per companion doc §1]** A bounded world with a fixed
total amount of cave, ore, and structure reads as *denser* than an infinite one
even at identical per-chunk generation rules, because there's no "more world" to
wander into looking for a better spot — you work with what's in range.
**[inferred, per companion doc §1's own framing]** For Blocksmith this is not a
generation-algorithm point, it's a *design temperament* point: Blocksmith's
loaded-column budget (companion doc §7: 11.16 MB of 12.58 MB used at New 3DS
radius 5, i.e. a 13x13-column/208x208-block ring) is itself a hard, small,
bounded window on the world at any given moment — which is the same *felt*
effect LCE got from a bounded save file, achieved by a completely different
mechanism (streaming radius instead of world size). This is worth stating
plainly as a reason **not** to chase "make caves sparser to feel more special" —
a small hardware-bounded window already does perceptual work in that direction
for free, and stacking a sparse carver on top of an already-small view radius
risks producing long dry stretches of unremarkable stone with nothing in a
loaded chunk worth finding, which is the opposite of the dense, "always
something nearby" feel LCE actually had.

### Console-specific terrain complaints — read with caution

A 2012-era Xbox 360 Edition forum thread complaining about an *upcoming* terrain
update describes the console version's underground, at least as that poster
expected/experienced it, as "a swiss cheese complex of caves," in the same post
that separately complains the *surface* was getting flatter with "almost no
mountains outside of Extreme Hills" and biomes roughly 1/4 PC's size.
**[community-reported: minecraftforum.net, "[RANT] The problems with the
upcoming 1.8 terrain generation" and "Large Biomes" threads]** This is a single
forum poster's impression, not a measurement, and it's ambiguous whether "swiss
cheese" describes the current state or a feared future state — flagged, not
treated as a number. What *is* useful from it, at low confidence: **LCE's flatter
overworld terrain (fewer mountains, smaller biomes than PC) means cliffside cave
mouths — which need vertical rock face to expose — were rarer on console than on
PC of the same era, purely as a side effect of gentler surface terrain, not
because the cave carver itself was tuned differently.** **[inferred]** This
matters for §3 below (surface breakage should be intentionally sparse) and is a
second, independent reason (alongside §1's "5-block minimum depth prevents
entrances" note in the companion doc §7) pointing the same direction.

---

## 2. Legacy carver vs. modern rewrite — what actually changed, and which parts
   the owner is asking to avoid

The owner asked specifically for "legacy" feel, which is a request to avoid a
particular set of *documented* 2021+ design decisions, not merely "old-looking"
caves. The 2021 Caves & Cliffs Part II update replaced the single walked-tunnel
carver with **three deliberately distinct noise-driven cave archetypes**, named
in Mojang's own announcement:

- **"Cheese" caves** — large, open, blobby caverns
- **"Spaghetti" caves** — long, winding, branching tunnel networks (the closest
  modern analogue to what the legacy single carver already did)
- **"Noodle" caves** — very narrow, tightly winding tunnels, a genuinely new
  shape with no legacy equivalent at all

**[documented: minecraft.net/en-us/article/caves---cliffs-part-ii-the-features]**
Alongside this, **aquifers** were introduced — a local water-table system that
produces dry caves, lake-filled caves, or fully flooded caves as separate
per-region rolls, replacing the old simple "carved void below sea level floods,
above it doesn't" rule. **[documented: same source, and companion doc §4]**
Mojang's own developers describe researching this generation by physically
visiting real-world caves and building "mesh caves" informed by that field
research. **[documented: search-summary of Minecraft.net Caves & Cliffs
developer coverage]**

**The single most concrete, quantified consequence of the 2021 rewrite, for
"legacy feel" purposes: caves break the surface dramatically more often now.**
Player feedback posted directly to Mojang's own feedback site during the 1.18
beta period describes the new generation making the surface "swiss cheese," and
explicitly contrasts it against the older behaviour: *"most of the time it
wouldn't be 2 or 3 blocks below the surface without creating an actual
entrance"* pre-1.18. **[documented: feedback.minecraft.net/hc/en-us/community/
posts/4409333938957 "Too much caves!"]** This is the clearest, most actionable
"do not copy this" item in the entire brief — see §7.

**What to explicitly avoid copying, with the reason each one breaks legacy
feel:**

1. **Multiple distinct cave archetypes as separate generation passes**
   ("cheese"/"spaghetti"/"noodle").** Legacy had one carver shape (companion doc
   §2) with a room-vs-worm roll baked into the *same* system, not three
   independently-tuned generators layered together. Building three distinct
   Blocksmith cave "biomes" would itself be a post-2021 design decision, however
   good it looks — it wasn't how caves worked when LCE was alive.
2. **Aquifers / local water tables producing arbitrary flooded regions.** Legacy
   used one simple rule: a carved void at or below sea level floods, one above it
   doesn't (companion doc §4). Multiplying that into a per-region roll is a
   2021-era mechanic.
3. **Frequent, large surface cave mouths.** Directly contradicted by the
   feedback-site quote above; legacy cave entrances were the exception a player
   found and remembered, not routine terrain texture.
4. **Light-level-0-only hostile spawn threshold.** Companion doc §5 already
   covers this in mechanical detail (pre-1.18: spawn at light 7 or below; 1.18+:
   light 0 only) — restated here because it's the single biggest lever on *how
   dangerous caving feels*, and it's a spawning-rule change, not a
   cave-generation change, so it would be easy to accidentally get this "for
   free" from whatever mob system Blocksmith eventually builds without
   deliberately choosing the old, harsher threshold.
5. **Ore exposed-to-air placement penalties.** One community source notes 1.18
   deliberately made ore *less* likely to generate where it's exposed to a cave
   void, specifically to rebalance strip-mining against cave-diving.
   **[community-reported]** Legacy ore placement had no such penalty (companion
   doc §4) — caving being strictly *better* than mining blind, ore-density-wise,
   is part of what made caving worth the risk in the legacy era; reintroducing an
   air-exposure penalty would undercut the exact reward loop being asked for.
6. **Biome-specific cave dressing** (dripstone, lush caves, deep dark, sculk).
   None of this existed in the legacy era; caves were undecorated stone/ore/
   lava/water with no per-region visual theme. Sourced as absent by omission —
   every source describing legacy-era caves describes them in exactly those four
   material terms and nothing else.

---

## 3. Shape, pacing, and sightlines — what made exploring them feel the way it did

### Shape and structure

- One carver, two outcomes: a branching, curling **tunnel network** (the common
  case) or, on a separate low-probability roll, a single oversized **room**
  (companion doc §2). Ravines are a structurally related but visually distinct
  third shape — one long, mostly-straight, narrow-but-deep **slash** rather than
  a branching worm, "up to roughly 40 blocks deep, ~5-7 wide," almost always
  intersecting an ordinary cave system somewhere along its length (companion doc
  §3). **A player exploring legacy caves therefore encountered three, and only
  three, distinct large-scale shapes: winding tunnel, occasional big room,
  occasional deep ravine crack** — not a taxonomy of named biome-caves, just
  those three primitives recombined by seed.
- A forum account of one memorably large system, describing "a large cavern that
  [was] fairly unique with how large and flat the floor space [was], with nice
  smoothly rounded walls," was explicitly called out by other players as
  unusually large **because it came from a pre-1.7.2 world** — i.e. the exact
  "old dense carver" era this brief argues LCE most likely also ran.
  **[community-reported: minecraftforum.net "How should I deal with a huge
  endless cave system?"]** This is a useful concrete image for what a genuinely
  memorable legacy cave landmark looked like: not a themed biome, just an
  unusually large, unusually round instance of the ordinary room roll.

### Sightlines: hardware, not design, set the pacing

This is the single most transferable insight in this document, because it comes
from a shared hardware constraint rather than a Minecraft-specific design
choice. **Xbox 360/PS3 Edition ran with a render distance far below PC's** —
player and forum accounts describe terrain "rendering almost beside them rather
than in front of them while moving quickly," and even the later Bedrock Realms
service (built on the same console-heritage renderer lineage) caps view distance
at **8 chunks** as its baseline. **[community-reported: minecraftforum.net
"Draw Distance" thread; documented for the 8-chunk Realms figure: forum
discussion citing the current Realms view-distance cap]** A PC player of the
same era routinely ran 12-16+ chunks.

**Why this matters more than any carver constant:** a short render distance
means a player underground physically cannot see a tunnel's far end, cannot spot
a lava glow two rooms away, and cannot pre-plan a route by looking — they
navigate by walking into darkness and reacting, not by surveying ahead. This is
*exactly* the situation Blocksmith is already in on real 3DS hardware for
unrelated reasons (a small ARM11 core budget, not a deliberate atmosphere
choice) — which means **Blocksmith does not need to add anything to get this
part of the legacy feel; it already has it, for free, from the same category of
constraint LCE had.** The design implication is the opposite of "build long
sightline showpiece caverns": a cave system whose *interior* rewards short-range
discovery (a torn-open wall exposing ore right where the player's light reaches,
a lava glow just visible around the next bend) plays to this constraint, while a
system depending on long, dramatic vistas to read as impressive will mostly go
unseen, exactly as it would have on original console hardware.

### Pacing and danger, moment to moment

- **The core exploration loop pre-1.18, as described by players and guides:**
  descend via dug-out tunnels or a natural cave mouth, place torches as you go
  (light level matters far more than 1.18+, see §2 point 4), decide whether to
  keep pushing deeper or turn back based on remaining torches/food/tools, and
  treat every branch as a real decision because turning back costs real travel
  time in a world with no fast-travel and, per the render-distance point above,
  no way to see the payoff of a branch before committing to it.
  **[community-reported: minecraft.fandom.com/wiki/Tutorials/Exploring_caves and
  multiple mining-guide sources synthesized]**
- **A widely-repeated player navigation trick**, framed explicitly as mirroring
  real-world spelunking convention: place torches on the **left** wall going in,
  so on the way out they're consistently on the **right** — a purely
  player-side, emergent solution to a game that gave you no map, no waypoint,
  and no minimap marker for "where you came from." **[community-reported:
  aggregated mining-guide sources]** This is worth naming because it's evidence
  the *lack* of a built-in navigation aid was part of what made caving feel like
  real exploration, not a gap to be quietly patched — see §7's explicit
  recommendation against adding one.
- **Getting lost was a real, common outcome**, not a rare edge case — sources
  describe large interconnected systems as "easy to get lost in," and separately
  note that even so, individual caves usually occupy less horizontal footprint
  than they *feel* like while inside them, because a winding path covers far
  less straight-line distance than the number of turns and branches makes it
  feel like. **[community-reported: minecraft.fandom.com/wiki/Tutorials/
  Exploring_caves]** **[inferred: this "feels bigger than it is" effect is a
  direct consequence of the branching/curling tunnel shape itself (companion doc
  §2's momentum-damped yaw/pitch drift) — a path that keeps turning covers less
  net distance per step than a straight corridor, so a system that reads as
  huge while inside it can be geometrically compact, which is good news for
  Blocksmith's per-column generation cost even if it never matches legacy's
  exact walked-tunnel algorithm.]**
- **Resource pressure, not a UI timer, paced play sessions.** The commonly-cited
  reason players surfaced wasn't reaching a depth goal, it was running out of
  torch material — described in guides as "the main reason players have to exit
  caves," with the standard advice being to carry a full stack or two of
  wood/torches specifically because players routinely underestimate consumption.
  **[community-reported: aggregated mining-guide sources]** This is a resource-
  economy point as much as a level-design one: legacy caving's session length was
  self-regulating through inventory pressure, not through any explicit "you've
  gone far enough" game signal.

### Ambient sound as a pacing tool, separate from the carver

Minecraft's cave ambience is driven by a hidden **"mood" value (0-100)** that
rises while the player stands in darkness and resets (triggering one of a pool
of eerie ambient sounds) at 100 — in total darkness this fires roughly **every
5 minutes (6000 ticks)**. **[documented: aggregated from Minecraft Wiki
"Ambience" page description of the mood mechanic and its 6000-tick period]**
Sound designers and players alike describe the effectiveness as coming from
**unpredictability and total darkness as the trigger condition** — the sound
itself does nothing mechanically, but it recalibrates the player's sense of
danger on a timer they can't see, which is a cheap, non-visual way to sustain
tension across a long cave session without needing any new visual content.
**[community-reported/documented mix, per multiple sources on the "mood"
mechanic and its stated design intent]** This is flagged separately from the
carver shape because it's **the cheapest item in this entire document to build**
(a timer plus a handful of short audio clips gated on "player is in near-total
darkness") and it's disproportionately responsible for legacy caves feeling
dangerous even in stretches where nothing is actually happening — see §7's
phase ordering.

---

## 4. Vertical distribution — legacy Y-values, and why they need no rescaling

**Blocksmith's world is 128 blocks tall with sea level at y=64** (measured this
session directly from `world/world.h`: `WORLD_HEIGHT` = `COLUMN_CHUNKS *
CHUNK_DIM` = 8*16 = 128; and `world/worldgen.h`: `GEN_SEA_LEVEL` = 64)
**[measured]**. This is the same 128-tall convention the classic pre-Anvil PC
build (and LCE's own permanent 128-block build cap) used, one block off on sea
level (63 vs 64) — the companion doc's §6 already works this arithmetic in
detail for the carver constants; the figures below are the same era's numbers,
restated for what they mean *experientially* rather than as carver inputs:

- **y<10: the lava floor.** Below this line, any void the carver would otherwise
  leave as air becomes lava instead — not a placed feature, a *rule*. Community
  description of the resulting experience: "the lava level is 10... this is the
  point where all caves are filled with lava." **[documented/community-reported,
  per companion doc §4]** Experientially: **the very bottom of the world is
  reliably, predictably dangerous, every single time, everywhere** — a player
  who digs deep enough anywhere eventually meets it, which makes "how deep do I
  dare go" a real, spatial decision rather than a randomly-seeded hazard some
  players never encounter.
- **y10-50: where standalone lava lakes live**, separate from the y<10 floor —
  described as "usually found between Y-levels 10 to 50," generating with a
  visible air pocket above them (which is itself a landmark cue — a strangely
  hollow patch of stone with nothing obviously carved often means a lake is
  behind the next wall). **[documented: minecraft.wiki/w/Lava_Lake, cross-
  referenced against companion doc §4's separate "lake placer" figure]** These
  are the **"surprise" lava encounters** distinct from the guaranteed-eventually
  bottom floor — the ones that produce a startled reaction rather than a known
  risk, because they can appear well above the guaranteed floor.
- **Around y63 (sea level): the flood line.** Any carved void at or below the
  water table simply fills with water — no aquifer roll, no per-region
  variation (companion doc §4). Experientially this means water in caves reads
  as a *predictable band*, not a rare feature: dig near sea level and you may
  well be swimming, dig well below it and you probably won't be unless you hit a
  separately-seeded pocket.
- **The full column (roughly y8-126) carries cave-tunnel starts**, biased toward
  the lower half rather than spread evenly (companion doc §2, §6) — meaning
  caves can start almost anywhere in the column, but *most* of them cluster low,
  which is why "dig down until you find a cave" was a reliable strategy across
  the whole legacy era, not a lucky roll.
- **Ravines, separately, read as "deep" rather than tied to one Y-band** —
  sourced depth figures (up to ~40-50 blocks) describe the ravine's own vertical
  extent, not a fixed starting Y (companion doc §3) — a ravine can crack open
  from well below the lava floor up toward the surface, which is part of why
  ravines specifically were remembered as more dangerous than ordinary tunnels:
  a single misstep can be a much longer fall, straight down into whatever's at
  the bottom (often, per the y<10 rule, lava).

**Nothing here needs rescaling for Blocksmith.** The companion doc's §6 already
shows the arithmetic (scale factor 128/128 = 1.0 for every 128-tall-era figure);
this section exists to translate those same numbers into what a player
experiences at each depth, since that's the framing this brief is scoped to
cover.

---

## 5. Landmarks and structures — what gave a cave system memorable shape

Two structure types intersect cave systems in the legacy era and were
specifically called out by players as memorable waypoints, distinct from
"generic tunnel":

- **Abandoned mineshafts.** A "complex multi-layered labyrinth" of nearly
  identical wooden-supported tunnels, disorienting on purpose, containing
  **cave spider spawners** sealed behind dense cobweb — cave spiders climb
  walls, attack in groups (a spawner "pours out several at a time"), and their
  bite poisons on Normal/Hard, which is what turns "found a spawner" into a real
  threat rather than a curiosity. **[documented/community-reported: aggregated
  from minecraft.wiki/w/Mineshaft and minecraft.wiki/w/Tutorial:Exploring_a_
  Mineshaft]** A cobweb-sealed room with a visible spawner inside is a strong,
  cheap, instantly-readable "something dangerous and valuable is here" landmark
  — the cobwebs alone (before the player even sees the spawner) function as a
  visual sting.
- **Ravines intersecting the cave network.** Already covered structurally in §3;
  worth restating here specifically as a *landmark*, because a ravine reads
  differently from a tunnel opening into it — a player following an ordinary
  tunnel who suddenly has the floor drop away into open space (with, per §4,
  real odds of lava at the bottom) is a distinct, memorable "oh" moment that a
  gradually-widening cavern doesn't produce, because the transition is abrupt
  rather than gradual.
- **Exposed ore veins on a cave wall**, functioning as an ambient, constantly-
  recurring micro-landmark rather than a rare set-piece: because ore is placed
  into solid rock *after* caves are already carved (companion doc §4), any vein
  that happens to touch an already-carved wall becomes visible without further
  digging — this is the single most frequent "landmark" a player encounters,
  far more common than a mineshaft or ravine, and it's what makes idle
  wall-scanning while walking through a tunnel a rewarding habit rather than
  wasted attention.

**What Blocksmith does not have yet, honestly stated:** cave spiders/mineshafts
and any other structure require the entity/mob framework the companion doc's §7
already flags as the largest missing subsystem in the whole request — this
section is a target for what that framework should eventually support, not
something buildable ahead of it.

---

## 6. What to build first vs. later — feel-side recommendations

These are sequenced by "cheapest change with the biggest effect on how caving
*feels*," which does not always match the companion doc's own algorithm-
complexity ordering (its §8), because a feel-first ordering can front-load a
cheap audio/pacing win ahead of a more expensive shape change if the shape
change won't be player-visible until other pieces (ore, lava, hostile mobs)
exist to give exploration a reason to happen at all.

1. **Ambient "mood" sound system (§3).** Cheapest item in this document — a
   darkness timer plus a small pool of short audio clips. Disproportionately
   responsible for legacy caves feeling dangerous even when nothing mechanical
   is happening, and it does not depend on the carver, ore, lava, or mobs
   existing first. Worth building early specifically because every other item
   below takes longer to land and this one can start paying off immediately.
2. **Keep cave-mouth frequency deliberately low (§1, §2).** This is a "don't"
   more than a "build," but it needs to be a stated target before any carver
   tuning pass, not discovered afterward — the single most quotable legacy-vs-
   modern contrast found this session is exactly this ("wouldn't be 2 or 3
   blocks below the surface without creating an actual entrance," §2). Whatever
   carver shape Blocksmith lands on, measure surface-breakage rate the same
   deliberate way the companion doc's §7 already measured connectivity, so a
   regression toward "swiss cheese surface" is caught by a number, not by feel
   after the fact.
3. **Lava as a guaranteed-eventually, not randomly-seeded, hazard (§4).** The
   y<10 floor rule is cheap (companion doc's Phase 1 already scopes this as a
   tuning-constant-plus-one-block-type change) and it's what makes "how deep do
   I dare go" a spatial decision instead of a coin flip. This should land before
   any standalone lava-lake feature, because the floor rule is what makes depth
   itself feel dangerous everywhere, while lake placement (companion doc's open
   item, §10.5) is a "surprise" flourish on top of an already-dangerous baseline
   — building the flourish before the baseline would produce lava that feels
   random rather than earned.
4. **Ore-on-cave-wall exposure (§5).** Already scoped in the companion doc's
   Phase 2; flagged here again because §5 identifies it as the single most
   *frequent* landmark a player will encounter — far more common than any
   structure — so its absence is felt on every single cave trip, not just
   occasionally.
5. **Water flood-to-sea-level rule (§4).** Cheap per the companion doc (existing
   simulation, new placement only) and it's what makes the sea-level band read
   as a distinct, predictable zone rather than uniform stone everywhere.
6. **Ravines, as a rarer, straighter, deeper carve preset (§1, §3, §5).** Worth
   sequencing after the above because a ravine's landmark value (§5: the
   "floor drops away" moment) depends on the surrounding tunnel network already
   existing and already being explored at a normal pace — a ravine with nothing
   around it to contrast against doesn't read as a landmark, it just reads as
   more terrain.
7. **Mineshafts and cave spiders, and hostile mob spawning generally.** Restated
   from the companion doc's §7/§8: this is the largest missing subsystem in the
   entire request and should be scoped as its own project. Feel-wise, it is also
   the single biggest lever on "danger" of everything in this document (§1's
   light-7 threshold, §5's spawner-as-landmark, pack spawning) — but sequencing
   it last here isn't a statement that it matters least, only that nothing on
   this list depends on it existing first, while it depends on almost everything
   above already existing (a cave system worth being afraid of, ore worth
   risking a fight for, lava worth being pushed into).

---

## 7. Explicit list: things NOT to copy from modern Minecraft, and why each one
   specifically breaks legacy feel

A consolidated pull of every "avoid" item raised above, kept together for a
single reference point:

1. **Three separate cave archetypes ("cheese"/"spaghetti"/"noodle") as distinct
   generation systems.** Legacy had one carver with a room-vs-worm roll built
   in, not a taxonomy (§2).
2. **Aquifers / per-region water-table variation.** Legacy used one flood rule
   tied to sea level, full stop (§2, §4).
3. **Frequent surface cave mouths / "swiss cheese" terrain.** The most directly
   quotable, most player-documented legacy-vs-modern contrast found this session
   (§2, §6 item 2).
4. **Light-level-0-only hostile spawning.** Legacy's light-7-or-below threshold
   is what made a single torch an inadequate defense and made caves feel
   dangerous by default (§2 item 4, companion doc §5).
5. **Ore air-exposure spawn penalties.** Legacy ore placement doesn't know or
   care that a cave wall is nearby; that's *why* caving paid off (§2 item 5).
6. **Biome-specific cave dressing** (dripstone spikes, lush caves, deep dark,
   sculk, glow berries, amethyst geodes). None of it existed in the legacy era;
   legacy caves are stone, ore, lava, water, and nothing else visually themed
   (§2 item 6).
7. **A built-in in-cave map, waypoint, or "return to surface" aid of any kind.**
   Not sourced as a modern Minecraft feature per se, but flagged because it
   would directly undercut the emergent, player-invented left-wall-torch
   navigation trick (§3) that's part of what made getting lost feel like a real,
   self-solved problem rather than a UI failure.
8. **Long-sightline showpiece caverns designed to be appreciated from a
   distance.** Legacy console hardware's short render distance meant these
   mostly went unseen even where PC's carver could theoretically produce them
   (§3) — and Blocksmith's own 3DS render budget puts it in the same position
   for unrelated hardware reasons, so building for long sightlines would be
   spending effort on something the platform can't deliver either way.

---

## 8. Sources

Every URL used this session, with what it supported:

- `minecraft-archive.fandom.com/wiki/Minecraft:_Xbox_360_Edition`,
  `co-optimus.com/article/8239` — LCE's frozen Beta 1.6.6→1.7.3-equivalent
  baseline (§1; both already cited in the companion doc, re-used here as the
  basis for the pre/post-1.7.2-nerf argument)
- `minecraftforum.net`, "Did they intentionally nerf the caves?" — TheMasterCaver's
  reverse-engineered comparison of 1.6.4 vs 1.7.2 cave density/system-size figures
  (§1)
- `minecraftforum.net`, "Cut down on the caves?" and "Caves in the later updates"
  — corroborating community discussion of the same 1.7.2 density change (§1)
- `minecraftforum.net`, "[RANT] The problems with the upcoming 1.8 terrain
  generation" and "Large Biomes" (MCX360 Discussion forum) — console-specific
  terrain complaints, "swiss cheese" underground description, biome-size
  comparison to PC (§1)
- `minecraft.net/en-us/article/caves---cliffs-part-ii-the-features` — Mojang's
  own naming and description of cheese/spaghetti/noodle cave archetypes and
  aquifers (§2)
- `feedback.minecraft.net/hc/en-us/community/posts/4409333938957`, "Too much
  caves!" — direct player quote contrasting pre-1.18 surface-breakage rate
  against 1.18's (§2, the single most load-bearing citation in this document)
- `minecraftforum.net`, "How should I deal with a huge endless cave system?" —
  a large-cavern account explicitly tied by other posters to a pre-1.7.2 world
  (§3)
- `minecraftforum.net`, "Draw Distance" (MCX360 Discussion forum) — Xbox 360
  Edition's render-distance complaints relative to PC (§3)
- web-search summary discussing Bedrock Realms' 8-chunk view-distance cap,
  cited as a heritage data point for the same console-line renderer lineage (§3)
- `minecraft.fandom.com/wiki/Tutorials/Exploring_caves`,
  `minecraft.wiki/w/Tutorial:Exploring_caves` — core exploration loop, getting
  lost, "caves feel bigger than they are," torch/resource pressure (§3)
- aggregated mining-guide web-search summary (multiple sites) — left-wall torch
  navigation convention, torch/wood consumption as the real reason players
  surface (§3)
- `minecraft.wiki` "Ambience" page (via search summary) — the mood-value
  mechanic driving cave ambient sound, 6000-tick/5-minute period in total
  darkness (§3)
- `minecraft.wiki/w/Lava_Lake` — standalone lava lake Y-range (10-50), visible
  air-pocket generation detail (§4)
- `minecraft.wiki/w/Mineshaft`, `minecraft.wiki/w/Tutorial:Exploring_a_Mineshaft`
  — mineshaft structure, cave spider spawners, cobweb sealing, pack-attack/
  poison threat (§5)
- Companion document `docs/research/caves-legacy-console.md`, all sections —
  referenced throughout rather than re-derived; its own §9 carries the original
  citations for every algorithmic figure this document restates experientially
- Local, measured (not web): `source/world/world.h` (`WORLD_HEIGHT`,
  `COLUMN_CHUNKS`, `CHUNK_DIM`), `source/world/worldgen.h` (`GEN_SEA_LEVEL`) —
  re-confirmed directly this session, matching the companion doc's own figures
  (§4)

---

## 9. What could not be determined, and what the gap means

1. **Whether 4J Studios ever backported any part of PC's October 2013 "1.7.2"
   cave-density reduction to the console line, at any point across LCE's
   2012-2019 life.** This is the load-bearing open question behind §1's central
   claim. No source found this session states either way. **What this means in
   practice:** §1's "lean toward the denser, pre-nerf carver shape" is presented
   as the best-supported reading of the evidence found, not a confirmed fact —
   if the project owner's own memory of "legacy caves" reads as closer to
   today's sparser vanilla baseline than to a dense, sprawling one, that's a
   real signal worth weighing over this document's inference, since he played
   the actual game and this document did not.
2. **The exact console-edition render distance in chunks**, at any specific
   platform/era. Sources described the *effect* (terrain rendering "beside"
   rather than "ahead of" the player) and gave the modern Realms 8-chunk figure
   as a heritage data point, but no source gave a confirmed Xbox 360/PS3-era
   number directly. **What this means:** §3's sightline argument holds
   regardless of the exact figure (it only needs "meaningfully shorter than PC of
   the same era," which is well established), but a precise chunk count wasn't
   found and shouldn't be assumed if this ever needs to inform a specific
   Blocksmith render-distance decision.
3. **Whether the mineshaft/cave-spider material in §5 is legacy-era accurate
   specifically**, versus describing a later-refined version of the same
   structure. Mineshafts predate LCE's Beta-1.7.3-equivalent baseline (they
   shipped in the same Beta 1.8 that added ravines, per the companion doc's §1),
   so the core structure is legacy-appropriate, but the exact spawner-count and
   cobweb-density details cited in §5 come from current-wiki descriptions that
   may reflect later tuning passes. **What this means:** treat §5's mineshaft
   material as directionally right (labyrinth structure, cobweb-sealed spawners,
   pack-spawning cave spiders) rather than as exact legacy-era numbers to hit.
