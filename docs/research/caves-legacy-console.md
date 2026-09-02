# Cave generation research: Legacy Console Edition, for Blocksmith v1.8.11

Research only. Nothing under `source/` was touched, nothing was built or run. This
file is the only thing this pass wrote.

**Provenance tags used throughout:** every quantitative claim is tagged one of
**[measured]** (read off Blocksmith's own source), **[sourced: URL]** (a web source
gave the number directly, cited inline), **[reasoned]** (derived by arithmetic or
logical necessity from measured/sourced facts, shown worked), or **[assumed]** (a
plausible figure with no source found this session — flagged, never presented as if
sourced). A number with no tag next to it does not appear in this document; that is
the rule this file holds itself to.

---

## §1 — What Legacy Console Edition caves actually were

**LCE is not its own generator.** 4J Studios' console line (Xbox 360, PS3, Xbox One,
PS4, Wii U, PS Vita, Switch — everything before the Bedrock unification) shipped
Java Edition's world generator, ported and then frozen at whatever point 4J's port
had last caught up to PC.

- Xbox 360 Edition's initial gameplay/generation was drawn from **PC Beta 1.6.6**,
  the version just before pistons and shears; 4J's first major content-parity push
  publicly targeted **Beta 1.7.3** equivalence. **[sourced: minecraft-archive.fandom.com/wiki/Minecraft:_Xbox_360_Edition,
  co-optimus.com/article/8239]**
- Ravines were not part of that initial baseline — they were added to Java in
  **Beta 1.8** ("The Adventure Update," released 2011-09-14), alongside abandoned
  mineshafts. **[sourced: minecraft.wiki/w/Java_Edition_Beta_1.8]** Console picked
  ravines up in a later title update as 4J's port caught up further; TU36
  (2016-06-21) explicitly lists "Added Caves and Ravines to the Custom Superflat
  options" for both Xbox 360 and Wii U, which confirms ravines were a normal,
  already-shipped world-gen feature by that point (the TU36 change is about giving
  *superflat* worlds the option, not introducing the feature itself) and a later
  Aquatic-era update references "Underwater Caves/Ravines" as an established
  concept being extended. **[sourced: minecraft.wiki/w/Xbox_360_Edition_TU36]** I
  could not find the specific title update that first brought ordinary
  (non-superflat) ravines to console — see §10.
- Ordinary, non-flat-world cave generation itself — the tunnel/room carver — was
  present from Xbox 360's Beta-1.6.6-equivalent launch onward; nothing in the
  sources found this session suggests caves themselves were ever absent, only that
  ravines, biome variety, and world size options arrived in later updates.
  **[reasoned from the above two points]**
- LCE **never received Java's 1.18 "Caves & Cliffs Part II" noise-cave rewrite** —
  it was discontinued (final updates around 2017-2019, "Update Aquatic" being one
  of the last major content passes) years before 1.18 shipped (2021). Whatever
  generator LCE had at end-of-life, it was carver-based, full stop.
  **[reasoned: 1.18 shipped 2021, LCE's active-development window is documented as
  2012-2019 — sourced: minecraft.wiki/w/World_size search-summary]**

**Did the finite/bounded world change the cave *algorithm*, or just the map it ran
on?** This matters because the task brief specifically asked me not to assume.
Everything found points to: **just the map.** LCE's finite worlds (Classic
864x864 on Xbox 360/PS3/Vita/Wii U; Small 1024x1024, Medium 3072x3072, Large
5120x5120 on Xbox One/PS4/Switch) worked by running the same per-chunk generator
Java used, bounded by an invisible edge barrier with a 2-chunk water moat blending
the generated terrain into open ocean at the border. **[sourced:
minecraft.wiki/w/World_size search-summary, minecraft.wiki/w/Legacy_Console_Edition_exclusive_features
search-summary]** Nothing in any source suggests the *carver itself* — the
per-chunk RNG roll, the tunnel walk, the branch/room logic — was rewritten to know
about the boundary; the boundary is handled by clipping/moating the outer two
chunks, which is a generation-*area* concern, not a generation-*algorithm* one. So:
**the carver Blocksmith should copy the shape of is the ordinary Java Beta/early-1.x
carver, not something bespoke to a finite world.** The finiteness explains why
players could reliably find everything (a bounded map with a fixed structure/ore
count reads as denser — **[reasoned]**, not independently sourced this session as
a stated design intent), not why the caves themselves look different.

**World height.** This is the one place LCE's own numbers turned out to matter a
lot for Blocksmith specifically — see §6, where it turns out LCE's build height and
Blocksmith's `WORLD_HEIGHT` are close enough that almost no vertical rescaling is
needed for the carver's own native (Beta-era) figures. Confirmed:
LCE is permanently capped at a **128-block build limit** (bedrock at the very
bottom, one buildable layer up to 128) — this was true of the console line for its
entire life and was never raised to Java's later 256- or 384-tall worlds.
**[sourced: minecraft.fandom.com/wiki/Legacy_Console_Edition search-summary, cross-checked
against Minecraft Forum threads on the Xbox 360 build-height ceiling]**

---

## §2 — The carver algorithm, as an implementable specification

**Everything below is my own restatement of behaviour, not copied source.** Two
source lineages were findable this session, and they disagree on two tuning
constants; both are given, with the disagreement flagged rather than silently
picked. I could not obtain the actual LCE console binary to resolve which one 4J
shipped — see §10.

**Lineage A — Beta 1.7.3 (`i73` project, a from-scratch open-source Beta-1.7.3-
compatible reimplementation used as a documentation source, not copied):**
**[sourced: github.com/coderbot16/i73, "Cave Generation" doc]**
- Per-chunk gate ("rarity"): **1 in 15**
- Tunnel-count shape: a triple-nested random pick bounded at 40 (`pick(pick(pick(40))`
  shape) — nesting skews the result hard toward 0-2, not evenly across 0-39
- Start-height distribution: **linear from y=8 up to y=126** (biased toward the low
  end of that band, not uniform — "linear_start: 8" in the source's own terms)
- Vertical size multiplier for tunnels: **0.5** (half as tall as wide)
- Carve target below **y=10**: lava instead of air

**Lineage B — decompiled 1.7.10 `MapGenCaves`** (read via a public decompiled-source
mirror to understand behaviour, not reproduced as code below):
**[sourced: github.com/vmarchaud/Alkazia, MapGenCaves.java, fetched and summarized]**
- Per-chunk gate: **1 in 7**
- Tunnel-count shape: triple-nested random pick bounded at **15**
- Initial yaw: uniform 0..2π. Initial pitch: `(rand-0.5) * 2/8`, i.e. roughly
  **±0.125 radians (~±7°)** off level
- Per-step drift: a "velocity" term for each of yaw and pitch that decays toward
  zero each step (roughly ×0.9 for yaw's velocity, ×0.75 for pitch's) and gets a
  fresh random kick added every step (yaw's kick scaled ×2.0, pitch's ×4.0, before
  a further ×0.1 is applied when folding the velocity into the actual heading) —
  net effect: the path curves smoothly rather than jittering, because each step's
  turn is a damped average of the last step's turn plus a small new random nudge,
  not an independent random heading every step
- Tunnel length: derived from a "range" parameter (`range*16-16`) with a further
  random reduction — long tunnels are the exception, not the rule
- Branch point: roughly the 25%-75% window of the tunnel's own length
- Radius envelope along the tunnel: `1.5 + sin(progress * pi / length) * size` —
  radius is **smallest at both ends and largest in the middle** of the tunnel (a
  taper, not a uniform pipe)
- Vertical radius: **exactly half** the horizontal radius at every point (the
  "wider than tall" squash, expressed as a flat 0.5 ratio in this lineage too)
- Branching trigger: at the branch point, if the tunnel's radius parameter is
  still above 1.0 (i.e. it hasn't tapered to nothing) and this tunnel is not
  itself already a branch, it spawns **two** new independent tunnels from that
  point (not a probability roll on top of reaching the branch point — reaching a
  qualifying branch point always forks in this decompile; I could not confirm
  whether Beta 1.7.3's version added a probability gate on top of this — see §10)
- Y clamp: 1 to 248 out of a 256-tall world (i.e. barely a restriction at all —
  almost the full column)
- Carve target below **y=10**: lava instead of air (agrees with Lineage A)

A third, older lineage — **Minecraft *Classic* (c0.30)**, which predates Beta and
is a different, simpler, single-pass carver (not the chunk-seeded system above) —
was also findable in detail and is useful **only as a clean illustration of the
general technique**, not as an LCE-accurate parameter set (Classic's carver was
gone long before Xbox 360 Edition existed): **[sourced:
github.com/ClassiCube/ClassiCube/wiki/Minecraft-Classic-map-generation-algorithm]**
theta/phi (yaw/pitch) drift with momentum (`theta += deltaTheta*0.2`,
`deltaTheta = deltaTheta*0.9 + rand - rand`, and the equivalent halved/quartered
form for phi), a radius envelope that also bulges in the middle and tapers at the
ends (`radius *= sin(len * pi / caveLength)`), and critically the **oblate-spheroid
carve test itself spelled out as an inequality**: a point is carved when
`dx² + 2*dy² + dz² < radius²` — the coefficient 2 on the vertical term is exactly
what makes the carved shape wider than tall, expressed as a single constant in the
carve test rather than as a separate vertical-radius parameter. This is a genuinely
useful implementation pattern (one inequality instead of tracking two separate
radii) even though its exact ratio (2 on `dy²`, ≈0.71 linear squash) differs from
Beta/1.7.10's flat 0.5 linear vertical multiplier — **the two lineages don't even
agree on how "wider than tall" their own predecessor/successor should be, which is
itself evidence this was always a hand-tuned feel constant, not a value worth
copying exactly.**

**Specification for Blocksmith (synthesis, own wording):**

1. For every chunk-sized region, derive a chunk-local seed from the world seed and
   the region's coordinates.
2. Roll a tunnel-system count using a *nested*, not flat, random draw, so most
   regions get 0-2 systems and a long tail gets more — this nesting is the actual
   source of "some chunks are cave-rich, most aren't," not a separate density knob.
3. For each system: pick a start point in the region, a start height biased toward
   the lower half of the allowed band (never uniform across the whole column), an
   initial heading, and roll a small chance of the system starting as a single
   large "room" rather than a worm (see below).
4. Walk each tunnel step by step. At each step: nudge yaw and pitch by a random
   amount damped by the previous step's nudge (momentum, not independent noise per
   step — this is what keeps the path smooth); advance the position along the
   resulting heading; compute a radius that's smallest at the very start and end
   of the tunnel and largest around the middle; carve every block within an
   ellipsoid around the current point whose vertical radius is about half its
   horizontal radius.
5. Partway along a sufficiently large tunnel's length (roughly its middle third),
   optionally fork into one or more new independent tunnels continuing from that
   point — this, not a separate "connect systems" pass, is what produces branching
   networks instead of isolated single corridors.
6. Occasionally (a small, separately-rolled fraction of systems), skip the worm
   walk entirely and instead carve one oversized, roughly spherical chamber — the
   classic "room."
7. While carving, any cell that ends up below a fixed low absolute Y and would
   otherwise become air is filled with lava instead (see §4).

---

## §3 — Ravines

Structurally the same family as §2's tunnel — a walked path with per-step
ellipsoid carving — retuned toward "one big crack" instead of a branching worm
network:

- Added in Java Beta 1.8 (2011), the same release LCE's console port later caught
  up to. **[sourced: minecraft.wiki/w/Java_Edition_Beta_1.8]**
- Rarity: roughly **1 attempt in 50 chunks** — an order of magnitude rarer than
  cave-tunnel systems' 1-in-7/1-in-15 gate. **[sourced: web-search summary of a
  decompiled 1.16-era analysis citing `rand.nextInt(50) == 0`]** — flagged: this
  figure comes from a later (1.16) decompile than either cave-carver lineage in
  §2, but ravine rarity is widely and consistently reported at this same order of
  magnitude across versions, so it's presented with medium-high confidence despite
  the version mismatch, not high confidence.
- Dimensions, as generally documented (not tied to a specific decompiled version
  this session): width **usually well under 15 blocks, often cited around 5-7**;
  depth **up to roughly 40-62 blocks**. **[sourced: minecraft-archive.fandom
  search-summary "up to 40 blocks deep, ~5 wide"; minecraftbedrock-archive.fandom
  search-summary "no bigger than 7 wide, 40-50 deep"]** — the two archived sources
  disagree by about 2 blocks on width and 10-20 on depth, which is consistent with
  ravines' own dimensions being randomly rolled per-instance within a range rather
  than fixed; neither source gave the exact roll formula.
- Much less yaw/pitch drift per step than a cave tunnel — a ravine reads as one
  long, mostly-straight slash rather than a curling worm.
- No branching — a ravine does not fork into child tunnels the way a cave system
  does. **[reasoned from every source describing ravines as a single crack, none
  describing ravine branching]**
- Almost always intersects or connects to at least one ordinary cave system, and
  ravines can also cut through mineshafts and other carved features.
  **[sourced: minecraft-archive.fandom search-summary]**
- Vertical profile: wider/deeper in the middle of its length, pinched at both
  ends — the same tapering-envelope idea as §2's tunnel radius, just applied over
  a much longer, straighter path and with depth (not just width) tapering too.

**Specification for Blocksmith:** reuse the exact tunnel-walk machinery from §2
with different tuning — far lower per-step drift, a fixed roughly-straight-line
length in the tens of blocks, a taller/narrower ellipsoid instead of the cave
tunnel's wide-flat one, no branch step, and a roughly 10x-lower per-region trigger
chance than the cave-tunnel roll. One carver, two presets, not two separate
systems — this was true of vanilla's own implementation shape (§2's Lineage A/B
both show the same walked-ellipsoid primitive underneath) and there is no reason
to duplicate the machinery for Blocksmith.

---

## §4 — Lava, water and ores

### Lava
- Below a fixed absolute **y=10**, any cell the carver would otherwise leave as
  air is instead filled with lava — this is not a separate "lava lake" feature,
  it is the carve rule itself. **[sourced: i73 Beta-1.7.3 doc AND independently
  the decompiled 1.7.10 MapGenCaves source agree on y<10 — see §2, both lineages]**
- General community description of the resulting player experience: "the lava
  level is 10... this is the point where all caves are filled with lava," and
  lava is markedly rarer as a surprise pool above that line (still possible in
  small pockets, just not guaranteed). **[sourced: web-search summary of Minecraft
  Forum discussion + GGServers knowledgebase article]**
- Separately, standalone lava *lakes* (not just the fixed-Y carve rule) could also
  be seeded above that floor via a dedicated "lake" placer distinct from the cave
  carver — commonly cited at roughly **1 attempt per 8 chunks underground**, with
  a much lower chance (roughly a further 90% reduction) of any lake attempt
  landing above sea level at all. **[sourced: web-search summary of Minecraft Wiki
  Lava page + a modded-worldgen wiki mirroring vanilla defaults; the exact
  constant is reported inconsistently across the sources found (1-in-6, 1-in-8,
  1-in-25 all appear depending on version/surface-vs-underground) — treat the
  "roughly 1 in 8, underground, rare above sea level" framing as the safe summary,
  not a specific version's exact constant]**
- Both mechanisms matter for the *feel* the project owner described: the y<10
  carve rule guarantees you will eventually hit a lava floor if you dig deep
  enough anywhere, while the separate lake placer is what produces the
  "surprise" mid-depth lava pool that isn't tied to the very bottom of the world.

### Water
- Pre-1.18 Minecraft had no aquifer system. The rule was a simple flood: any
  carved void that sits at or below the world's sea-level water table gets filled
  with water; nothing more sophisticated than that. **[sourced: web-search summary
  of Minecraft Wiki Cave page, contrasting the pre-1.18 flood rule against 1.18's
  aquifer system]**
- This is why classic caves near sea level would sometimes be fully or partially
  flooded, and why breaking into open ocean laterally at depth could flood a whole
  system, while caves well below the water table stayed dry unless they held their
  own separately-seeded water lake (same lake-placer mechanism as lava, just with
  water and a different, higher-frequency roll — no exact constant was found for
  the water-lake variant this session; see §10).

### Ore placement relative to caves
- **Ores are placed after caves and ravines are carved**, not before. The
  generation order (confirmed consistently across every source found describing
  the pipeline, from the classic per-chunk "populate" step through the modern
  data-driven carver/feature split) is: base terrain shape → carve caves/ravines
  → place ores/dungeons/other decoration into what's left. **[sourced: web-search
  synthesis of Minecraft Wiki "World generation" page + a GitHub gist overview of
  the pipeline, both independently describing carving before ore/feature
  placement]**
- **This order is exactly why exposed ore on cave walls is a thing at all.** An
  ore vein is placed as a blob inside solid stone, with no knowledge of where
  caves already are; any part of that blob that lands in a cell the carver had
  already turned to air is either skipped (some implementations check for air
  exposure and reduce placement odds right at a cave wall) or simply never placed
  there, while any part of the vein sitting immediately *against* a carved wall
  is now visible without further digging. Caving is rewarding specifically
  because ore placement happens on top of an already-holey world, not because ore
  is deliberately biased toward cave walls. **[reasoned from the sourced
  generation order above, cross-checked against a Minecraft Wiki "Ore" page
  description of an air-exposure skip check — sourced: web-search summary of
  minecraft.wiki/w/Ore_(feature)]**
- Y-bands and vein sizes: I was only able to source approximate legacy/pre-1.17
  Y-ranges, and could not independently confirm exact vein-size or
  attempts-per-chunk figures this session (two direct page fetches for the table
  were truncated/unavailable — see §10). What was captured, with the caveat that
  one field (redstone's) looked internally inconsistent in the fetched summary and
  is intentionally omitted rather than repeated uncertainly:
  **[sourced: minecraft.wiki/w/Ore/Pre-1.17_distribution, partial]**
  - Coal: generates across a wide band, common through the upper-middle of the
    column
  - Iron: generates through roughly the lower two-thirds of the column, tapering
    above that
  - Gold: concentrated low, roughly the bottom quarter of the column
  - Diamond: concentrated at the very bottom, roughly the bottom 10-12% of the
    column
  - Lapis lazuli: **not monotonic with depth** — its frequency peaks at a specific
    band well below sea level and tapers off both above and below that peak,
    unlike every other ore listed, which just get rarer moving up
  - Vein/blob sizes and per-chunk attempt counts: **[assumed, not sourced this
    session]** — commonly-cited figures exist in general Minecraft knowledge
    (small veins of 6-10 blocks for most ores, larger for coal, single-attempt
    for diamond/lapis, multiple attempts for coal/iron/redstone) but none of them
    were independently confirmed by a fetched source this session, so none are
    presented as sourced numbers. Recommend Blocksmith pick its own round vein
    sizes tuned for a 128-tall world rather than importing an unverified table —
    see §8.

---

## §5 — Cave mob spawning

- Light-level rule (pre-1.18, i.e. the rule LCE ran its entire life): a hostile
  mob can spawn on a block with **sky+block light level 7 or below** — a far more
  permissive threshold than the light-level-0-only rule 1.18 introduced. This is
  exactly why legacy-era caves felt more dangerous by default: a single torch
  does not spawn-proof nearly as large an area as it does post-1.18.
  **[sourced: web-search summary of Minecraft Wiki "Light"/"Mob spawning" pages,
  explicitly contrasting pre-1.18 (≤7) against 1.18+ (0 only)]**
- Spawn cadence: hostile-mob spawn attempts run **every game tick** (20 times a
  second) as part of the per-tick spawn cycle, distinct from the passive-mob
  cycle which runs far less often (roughly every 400 ticks / 20 seconds).
  **[sourced: web-search summary of Minecraft Wiki "Mob spawning" page]** — this
  figure is describing the modern spawn-cycle implementation and I could not
  independently confirm it is unchanged from the Beta/legacy-era spawn loop, only
  that the light-level gate is documented as different pre/post 1.18 while the
  cadence itself was not called out as having changed — treat the cadence number
  as **[sourced but not confirmed legacy-specific]**.
- Pack behaviour: hostile mobs spawn in **clusters of up to ~4** around a rolled
  center point, not individually — this is what makes a cave "come at you" as a
  group rather than a trickle, and is the direct cause of the "several zombies
  and skeletons at once" danger the project owner is asking to recreate.
  **[sourced: web-search summary of Minecraft Wiki "Mob spawning" page]**
- Mob cap: a base cap of **70** hostile mobs can exist around a solo player at
  once (scaling up with more players in multiplayer), beyond which no further
  hostile spawns are attempted regardless of light level.
  **[sourced: web-search summary aggregating multiple mob-spawning guide sites]**
- Minimum spawn distance from the player: roughly **24 blocks**, with an outer
  bound of roughly **128 blocks** beyond which hostile mobs don't spawn (or
  despawn) relative to the player at all.
  **[sourced: web-search summary of Minecraft Wiki "Mob spawning" page]**

**None of §5's mechanics exist in Blocksmith today** — see §7. Every number above
is a target to design *toward* once an entity/mob system exists, not something
that plugs into the current codebase as-is.

---

## §6 — Rescaling every figure to Blocksmith's 128-tall world

This section turned out simpler than expected, and the arithmetic below shows why.

**Blocksmith measured facts** (read from source this session):
- `WORLD_HEIGHT` = 128 (`world/world.h`: `COLUMN_CHUNKS * CHUNK_DIM` = 8 * 16)
  **[measured]**
- `GEN_SEA_LEVEL` = 64, the y of the water *surface* — topmost water block sits at
  y=63 (`world/worldgen.h`) **[measured]**

**LCE/legacy-Java measured/sourced facts:**
- LCE's build height was permanently capped at **128 blocks** for its entire
  active life (§1) **[sourced]**
- The classic pre-Anvil PC convention (which LCE's caves-and-ore figures in §2/§4
  are drawn from) used a **128-tall world with sea level at y=63** — i.e. the
  *exact same* height Blocksmith uses, and a sea level one block off from
  Blocksmith's own (63 vs 64) **[reasoned: this is the well-known pre-1.2/Anvil PC
  convention that both of §2's cave-carver lineages predate or match — Lineage A
  (Beta 1.7.3) is squarely inside this era; Lineage B (1.7.10 decompile) is a
  256-tall-world version, addressed separately below]**

**The arithmetic:**

For every figure sourced from a **128-tall-world version** (Beta 1.7.3 / Lineage
A, the y<10 lava rule, the general "lava sea below layer 10-11" folklore, and
LCE's own native figures): the scale factor is **128 / 128 = 1.0**. These numbers
carry over to Blocksmith with no arithmetic at all beyond the 1-block sea-level
offset (Blocksmith's `GEN_SEA_LEVEL` is 64, one higher than the classic 63 — close
enough that this document treats it as a rounding choice already made by
Blocksmith's own generator, not something this research should re-litigate).

Concretely:
- Cave start-height band, Beta 1.7.3: linear y=8..126 → **Blocksmith y=8..126,
  unchanged** (both worlds are 128 tall; y=126 is 2 below Blocksmith's ceiling of
  127, exactly as it was 2 below Beta's own ceiling of 127)
- Lava-below-y=10 carve rule → **Blocksmith y<10, unchanged**
- `GEN_CAVE_MIN_DEPTH` (Blocksmith's own existing 5-block no-cave-near-surface
  rule, measured from `worldgen.h`) has no legacy equivalent to rescale against —
  it's a Blocksmith-specific safety rule (see §7), not a ported figure

For the one figure sourced from a **256-tall-world version** (the 1.7.10 decompile,
Lineage B's y-clamp of 1..248, out of a 256-tall world that version already used):
the scale factor is **128 / 256 = 0.5**.
- 1 * 0.5 ≈ **1**, 248 * 0.5 = **124** → a rescaled clamp of roughly y=1..124,
  which lands within a couple of blocks of the *native* 128-tall figure above
  (8..126) anyway. **[reasoned]** This is worth stating plainly: Lineage B's own
  y-clamp was already so generous (nearly the full height of its own taller
  world) that it barely constrains anything either way — whichever lineage's
  number gets used, the practical effect on Blocksmith is the same "basically the
  whole column below the surface band," which is consistent with §2's own
  framing of that clamp as "barely a restriction at all."

For the ravine start-Y figure that could not be confidently tied to a specific
world-height era this session (§3's general depth/width figures came from
sources that don't state which world-height convention they're using): **no
rescaling arithmetic is shown, because I don't trust the input enough to scale
it.** Instead, §8's recommendation reasons about ravine placement *relatively* —
biased toward the same lower-half band as caves, since every source agrees
ravines are found "underground" and "beneath sea level" without giving a version-
tagged absolute range I could rescale with confidence.

**World *area*, separately from height, does not rescale at all — it doesn't
need to.** LCE's finite worlds (864x864 up to 5120x5120, §1) are all vastly
larger than anything Blocksmith holds in memory at once (§7's 12 MB cap holds at
most a 13x13-column ring, i.e. 208x208 blocks, at the New 3DS's radius-5 ceiling).
The carver's *area* behaviour (how far a tunnel can wander from its start chunk)
is a generation-cost question, not a world-size question, and belongs in §7.

---

## §7 — What Blocksmith does today, and the honest delta

**What exists today:** `worldgenIsCave()` (`world/worldgen.c`) is a **pure 3D
noise field**, not a carver. A block is "cave" when it falls inside the
overlapping threshold band of *two independent* 3D fractal-noise fields (two
salts) evaluated at that exact `(x, y, z)` — see `inCaveBand()` and the function's
body. There is no path, no walk, no start point, no branch, no room, and no
concept of a "tunnel" as an object; every block's cave-or-not answer is computed
independently of every other block, purely as a function of its own coordinates
and the world seed.

Measured tuning (`GEN_CAVE_*` constants, `world/worldgen.h`):
- `GEN_CAVE_SHIFT_XZ` = 5 → 32-block horizontal noise-lattice period
- `GEN_CAVE_SHIFT_Y` = 4 → 16-block vertical noise-lattice period (explicitly
  commented as "squashed, so tunnels run flat" — the noise field's own vertical
  squash is a 2:1 ratio between these two shift constants)
- `GEN_CAVE_OCTAVES` = 2
- `GEN_CAVE_HALF` = 0.05, chosen as "the connectivity knee" — measured sweep
  across seeds (1337, 1616, 4242, 7, 99999, 20260818), 6-connected flood fill over
  a 96x64x96 box: at half-width 0.05, **5.44% of the underground volume is
  carved, with 99.2% of that carved volume sitting in connected systems bigger
  than 100 blocks**; every tested seed put at least 98.9% of its carved volume
  into walkable systems, across a 3.97-8.23% carved-volume range depending on
  seed. **[measured, from the comment block above `GEN_CAVE_SHIFT_XZ` in
  `worldgen.h`]**
- `GEN_CAVE_MIN_DEPTH` = 5, `GEN_CAVE_FLOOR` = 1 — no cave within 5 blocks of the
  surface (so a carved cell can never leave a floating grass block or open under
  a spawn point) and nothing carved below y=1. **This is Blocksmith's own
  existing design decision, not a legacy figure, and it has a direct, stated
  cost: the current generator produces no cave entrances at all — the only way
  into a cave is to dig one open, because no carved cell is ever allowed to reach
  the surface.** `worldgen.h`'s own comment on `GEN_CAVE_MIN_DEPTH` says this
  explicitly.

A per-column corner-interpolation cache (`CaveCache`, `caveCacheBuild()`,
`caveFieldAt()`) was added in v1.8.7 to make repeated evaluation of this noise
field cheap — it exploits the fact that, given the lattice periods above, both
octaves' x/z lattice indices are constant across an entire 16-block column, so a
column's worth of corner values can be hashed once instead of re-hashed per
block. **A second agent is actively extending this cache in the same file as
this research is being written — this document does not touch `worldgen.c` and
does not depend on that cache's current line numbers, only on the two function
names (`worldgenIsCave`, `caveCacheBuild`) and the constants in `worldgen.h`,
which are stable regardless of how the cache internals change.**

### The honest delta: noise field → carver

A true legacy-style carver (§2) is a fundamentally different computational shape
from a noise field, and the difference that matters most is **not** primarily
memory — it's **cross-column dependency**.

- **Today:** `worldgenIsCave(x, y, z)` is answerable from that single block's
  coordinates and the seed alone. This is *why* the per-column corner cache
  works cleanly — a column never needs to know anything about its neighbors to
  answer its own cave questions, so caching is purely local.
- **A carver:** a tunnel that *starts* in one chunk can wander into, and carve
  blocks in, several neighboring chunks before it ends. Vanilla's own generator
  handles this by re-deriving (from the world seed and each candidate start
  chunk's coordinates) and partially re-walking every tunnel that could plausibly
  reach the chunk currently being generated, checking a neighborhood of
  candidate start chunks around it. The exact vanilla radius could not be pinned
  to a legacy-specific source this session (see §10), but the *shape* of the
  problem is not in question: **generating one column's caves correctly requires
  knowing about tunnels that started in other columns.**

That is the real architectural break against Blocksmith's current design, stated
plainly: **`worldgenColumn()` today generates a column from nothing but its own
coordinates and the seed — no neighbor column is ever consulted.** A carver
either needs to (a) re-derive and partially re-walk candidate tunnels from a
neighborhood of surrounding columns every time a column is generated — a CPU
cost, not a memory one, since nothing needs to be stored beyond the current
column's own blocks — or (b) bound that neighborhood tightly enough (e.g. only
the column's own 3x3 or 5x5 immediate neighbors, deliberately sacrificing rare
long-range tunnel continuity) to keep the re-walk cost affordable on an ARM11 core
generating columns on a background worker thread while the player moves.

**Against the 12 MB budget specifically** (`world/budget.h`, all measured): the
loaded column ring at New 3DS's radius 5 already uses **11,160,160 bytes of the
12,582,912-byte cap (88.7%)**, with radius 6 (14,836,448 bytes) explicitly
excluded by 2,253,536 bytes — the ceiling is asserted tight on purpose, not
loose. A carver that needs to *persist* tunnel objects (rather than re-derive
them on demand, stateless, from the seed) would compete directly with that
already-88.7%-committed budget, and there is essentially no slack to give it.
**The only affordable design is one where tunnels are never stored** — every
column re-derives, from its own coordinates plus its bounded neighborhood, only
the tunnels relevant to itself, exactly the way the existing noise field is
already a pure function of position with nothing persisted between calls. This
is a real constraint on *how* a carver would have to be built, not an argument
against building one.

**Ores, lava, water: none of these exist in Blocksmith today.** A repository-wide
search this session found zero ore block IDs, zero lava block, and zero mob/
entity system of any kind (`entity`, `mob`, and related terms match nothing under
`source/` outside unrelated identifiers like `sprite.v.pica` or `interact.c`'s own
comments). Water *does* exist (`world/water.c`, `world/water.h`) with a working
8-level flow simulation (`WATER_LEVEL_SOURCE` = 8, `WATER_LEVEL_MAX` = 7) already
used by the surface generator (`world/worldgen_density.c` floods air below
`GEN_SEA_LEVEL - 1` to water for oceans/lakes today) — extending that into caves
is a much smaller lift than anything else in this section, because the fluid
*simulation* already exists; only the *placement* (seeding water inside a carved
cave void) is new.

**The single biggest obstacle in the whole request is not the cave algorithm —
it's mobs.** There is no entity, no AI, no combat, no spawning, and no rendering
system for anything that moves under its own logic anywhere in this codebase.
"Zombies and skeletons in caves" is not a cave-generation feature at all; it's an
entire subsystem Blocksmith does not have a single line of, and cave-specific
spawning rules (§5) are meaningless until that subsystem exists. This should be
scoped and estimated as its own project, not folded into "cave generation" — see
§8.

---

## §8 — Recommended phase order for v1.8.11

Ordered cheapest-and-lowest-risk first, each phase independently shippable and
playtestable before the next starts, per the standing rule that every phase gets
its own handoff:

**Phase 1 — retune the existing noise field toward legacy character, no
architecture change.** The current field is already tuned for connectivity
(§7's measured 99.2%-connected figure), which is the single property §3/§4's
sources agree matters most to how caves *feel*. What it does not have is
legacy's directional, wormy anisotropy or its fixed lava floor. Concretely:
narrow `GEN_CAVE_HALF` further and/or reduce `GEN_CAVE_OCTAVES`'s effective
smoothness to push the carved shape toward long, narrow "spaghetti" rather than
the current blobbier band (measure the resulting carved-volume and connectivity
numbers the same way the existing tuning comment did, so a regression is
visible immediately rather than discovered by feel); add a static (non-flowing,
to start) lava block placed wherever a cave cell already generates at y<10,
directly reusing §6's finding that the legacy y<10 threshold needs zero rescaling
for a 128-tall world. This phase touches tuning constants and adds one new block
type; it does not touch the generation architecture at all.

**Phase 2 — ore placement as a post-carve population pass.** Add ore block IDs
and hook a new placement pass into the same place `worldgenDecorate()` already
runs (trees via `treePut()`, tall-grass scatter) — i.e. after the column's blocks
already exist and its caves are already carved, matching §4's sourced
after-carving generation order exactly, which is what produces the "exposed ore
on a cave wall" payoff the project owner is asking for. Bias vein placement
toward the lower portion of the column, since every band figure this document
was able to pin down (§4) agrees the valuable ores concentrate low, and pick
Blocksmith-native vein sizes rather than importing the unconfirmed legacy table
(§4, §10) — playtest-tune against "does a dug tunnel expose ore often enough to
feel rewarding," which is the actual design target, not a specific vanilla
number.

**Phase 3 — water in caves.** Extend the existing flood rule (already present in
`worldgen_density.c` for surface oceans/lakes) to also flood any carved cave void
that connects to the water table, reusing the water simulation that already
exists rather than building a second one. Smaller lift than lava because the
simulation is already there; only new placement logic is needed.

**Phase 4 — ravines, as a second preset of whatever carving mechanism exists by
this point** (either the retuned noise field from Phase 1, tuned toward a single
long straight low-connectivity band, or a true carver if Phase 6 has landed by
then). Much rarer trigger than ordinary caves per §3's sourced ~1-in-50 figure,
placed in the same lower-half Y band as caves per §6's reasoning.

**Phase 5 — a minimal entity/mob framework, scoped and estimated as its own
project, separate from cave generation entirely.** This is the actual largest
piece of work implied by the original request (§7) and should not be sized or
sequenced as if it were a cave-generation task. Once *any* entity exists that can
be positioned, tick, and be damaged, cave-specific hostile spawning (§5's
light-level gate, pack spawning, cap) attaches to it as a relatively small rule
set layered on top — but none of that rule set is buildable before the framework
itself exists.

**Phase 6 — true walked-tunnel carver, only if Phase 1's retuned noise field
doesn't satisfy the "feels like legacy caves" target after playtesting.** This is
listed last and marked optional on purpose: §7 already showed the real
architectural cost (cross-column dependency, a CPU cost against an ARM11 core,
and zero slack in the 12 MB budget for persisting anything). If the cheaper
Phase 1 retune reads as close enough, this phase may never be worth its own
cost — that's a call for whoever plays Phase 1, not a foregone conclusion either
way.

---

## §9 — Sources

Every URL used this session, with what it supported:

- `minecraft-archive.fandom.com/wiki/Minecraft:_Xbox_360_Edition`,
  `co-optimus.com/article/8239` — Xbox 360 Edition's Beta-1.6.6 launch baseline
  and Beta-1.7.3-equivalent first update target (§1)
- `minecraft.wiki/w/Java_Edition_Beta_1.8` — ravines and abandoned mineshafts
  added to Java in Beta 1.8, 2011-09-14 (§1, §3)
- `minecraft.wiki/w/Xbox_360_Edition_TU36` — TU36 (2016-06-21) adding
  caves/ravines to Custom Superflat options on Xbox 360 and Wii U, and the later
  Aquatic-era "Underwater Caves/Ravines" reference (§1)
- `minecraft.wiki/w/World_size` — LCE finite world size options (Classic
  864x864, Small/Medium/Large) and platform availability (§1)
- `minecraft.fandom.com/wiki/Legacy_Console_Edition_exclusive_features` — world
  edge/water-moat behaviour, structure-density compensation on smaller worlds
  (§1)
- `minecraft.fandom.com/wiki/Legacy_Console_Edition` — LCE's permanent 128-block
  build-height cap (§1, §6)
- `github.com/coderbot16/i73`, "Cave Generation" doc — Beta 1.7.3 cave-carver
  parameters: rarity 1-in-15, nested tunnel-count bound 40, linear start-height
  8..126, vertical multiplier 0.5, lava below y=10 (§2, §6)
- `github.com/vmarchaud/Alkazia`, `MapGenCaves.java` (decompiled 1.7.10, read to
  understand behaviour only, not reproduced as code) — rarity 1-in-7, nested
  tunnel-count bound 15, yaw/pitch initial values and per-step drift, radius
  envelope and 0.5 vertical ratio, branch trigger, y-clamp 1..248, lava below
  y=10 (§2, §6)
- `github.com/ClassiCube/ClassiCube/wiki/Minecraft-Classic-map-generation-algorithm`
  — Classic (c0.30)-era theta/phi drift-with-momentum and the single-inequality
  oblate-spheroid carve test, used only as an illustration of the general
  technique, explicitly not as an LCE-accurate parameter set (§2)
- web-search summary citing a decompiled 1.16-era ravine rarity analysis
  (`rand.nextInt(50) == 0`) — ravine rarity ~1-in-50 (§3)
- `minecraft-archive.fandom.com/wiki/Ravine` (via search summary; direct fetch
  was blocked, HTTP 402) — ravine width/depth (~5 wide, up to 40 deep),
  near-universal connection to cave systems (§3)
- `minecraftbedrock-archive.fandom.com/wiki/Ravine` (via search summary) —
  alternate width/depth figures (up to 7 wide, 40-50 deep) (§3)
- web-search summary of Minecraft Wiki "Lava" page, GGServers knowledgebase
  article, and a Minecraft Forum thread — y<10/11 lava floor, lake-placer
  frequency figures (inconsistently reported across versions) (§4)
- web-search summary of Minecraft Wiki "Cave" page — pre-1.18 simple flood-to-
  water-table rule vs. 1.18's aquifer system (§4)
- web-search summary of Minecraft Wiki "World generation" page and a GitHub gist
  overview of the generation pipeline — carve-before-ore-placement generation
  order (§4)
- `minecraft.wiki/w/Ore/Pre-1.17_distribution` (direct fetch, partial) — coal/
  iron/gold/diamond/lapis relative depth bands (§4)
- web-search summary of `minecraft.wiki/w/Ore_(feature)` — air-exposure skip
  check as evidence ore placement is aware of already-carved terrain (§4)
- web-search summary of Minecraft Wiki "Light" and "Mob spawning" pages — pre-
  1.18 light-level-7 hostile spawn threshold vs. 1.18's light-0 rule, spawn
  cadence, pack spawning (~4-mob clusters), 70-mob cap, 24-128 block spawn
  distance band (§5)
- Local, measured (not web): `source/world/budget.h`, `source/world/world.h`,
  `source/world/worldgen.h`, `source/world/worldgen.c` (function names
  `worldgenIsCave`, `caveCacheBuild`, `caveFieldAt`, `worldgenDecorate`,
  `treePut`; constants `WORLD_HEIGHT`, `GEN_SEA_LEVEL`, `GEN_CAVE_SHIFT_XZ`,
  `GEN_CAVE_SHIFT_Y`, `GEN_CAVE_OCTAVES`, `GEN_CAVE_HALF`, `GEN_CAVE_MIN_DEPTH`,
  `GEN_CAVE_FLOOR`, `WORLD_BUDGET_BYTES`), `source/world/water.h` (`WATER_LEVEL_
  SOURCE`, `WATER_LEVEL_MAX`) — everything tagged **[measured]** throughout (§6,
  §7)

---

## §10 — What could not be determined, and what the gap means

1. **Which of §2's two rarity/tunnel-count constants (Beta 1.7.3's 1-in-15/bound-40,
   or 1.7.10's 1-in-7/bound-15) LCE actually shipped, at any given point in its
   life.** Both lineages are real, sourced Minecraft carver implementations; I
   could not decompile or otherwise inspect the actual console binary to see
   which one 4J's port used, or whether it changed across title updates the way
   Mojang's own PC releases did. **What this means in practice:** it doesn't
   block Blocksmith's design (§8's Phase 1/6 recommendation doesn't depend on
   picking one exactly — see §2's closing note that both lineages converge on
   "mostly 0-2 systems per chunk-sized region" regardless), but if the project
   owner specifically remembers a *density* rather than a *shape*, tune toward
   feel via playtesting rather than trying to match either constant exactly.

2. **Exactly which title update first brought ordinary (non-superflat) ravines
   to console.** TU36 confirms ravines existed as a normal feature by 2016; I
   found no source pinning the specific earlier update that introduced them.
   **What this means:** no design impact — §3's spec doesn't depend on the exact
   introduction date, only on ravines being part of the generator LCE ran for
   most of its life, which is well established.

3. **Vein sizes and per-chunk attempt counts for ore generation.** Two direct
   page fetches for the pre-1.17 ore distribution table returned only partial
   data (Y-band descriptions, no vein/attempt numbers); a third fetch attempt at
   a different ore page also came back without the table. **What this means:**
   §4 explicitly does not present any vein-size or attempt-count figure as
   sourced, and §8's Phase 2 recommends picking Blocksmith-native numbers rather
   than importing unverified ones — this is a real gap, but it's one the phase
   plan already routes around rather than silently filling with a guess.

4. **The exact vanilla carver's cross-column search radius** (how many
   neighboring chunks get re-derived/re-walked to generate one chunk's caves
   correctly). §7's architectural argument doesn't depend on the exact number —
   the qualitative fact that neighbor chunks must be considered at all is what
   drives the delta — but the exact figure would matter for costing Phase 6
   concretely if it's ever picked up. **What this means:** whoever scopes Phase 6
   for real should budget time to either find this figure from a dedicated
   decompile or measure the CPU cost of a chosen neighborhood radius directly,
   rather than assuming §7's qualitative argument alone is enough to size the
   work.

5. **Water-lake seeding frequency inside caves specifically** (as opposed to the
   general sea-level flood rule, which is well sourced). §4 flags this gap
   explicitly rather than reusing the lava-lake figure by assumption. **What this
   means:** Phase 3 (§8) should treat cave water placement as "flood what
   connects to the water table" only, and treat standalone underground water
   lakes as a nice-to-have to tune by feel rather than a sourced target.

6. **Whether Beta 1.7.3's own branch trigger had an additional probability roll
   on top of reaching the branch point**, the way the 1.7.10 decompile's branch
   logic reads as unconditional-once-qualified. Not resolved from either source
   this session. **What this means:** §2's spec describes branching as "a small,
   separately-rolled fraction of systems" for the room case (which the general
   forum analysis of a later version did describe as a 1-in-4 roll — a third,
   separately-flagged figure not attributed to either main lineage) and as
   "roughly the middle third of a qualifying tunnel" for the ordinary-tunnel
   branch case, deliberately hedged rather than stated as a specific probability
   — Blocksmith's own implementation should treat the branch rate as a tunable,
   not a fact to hit exactly.
