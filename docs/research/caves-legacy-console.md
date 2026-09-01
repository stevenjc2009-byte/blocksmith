# Legacy Console Edition Caves — Research Brief for Blocksmith v1.8.11

Scope note: this is research only. Nothing in the Blocksmith project tree was touched.
Everything below is sourced via inline links unless explicitly marked **[INFERENCE]** —
my own reasoning/extrapolation, not a cited claim — or **[FOLKLORE]** — a claim I found
repeated by players but could not pin to an authoritative technical source.

---

## (a) What LCE caves actually were

**Legacy Console Edition (LCE)** is 4J Studios' console line (Xbox 360, PS3, Xbox One,
PS4, Wii U, PS Vita, Switch — pre-"Better Together"/Bedrock unification). Its world
generation is NOT its own algorithm; it tracked Java Edition's generator at whatever
point 4J had last ported it, then froze.

- Xbox 360 launched with generation "roughly equivalent to Java Edition Beta 1.6."
  Title Update 5 brought it in line with **Java Edition Beta 1.8** ("The Adventure
  Update," Sept 2011) — the same release that added ravines and gave the terrain
  generator "a complete rewrite ... aimed to make the code simpler," which is also
  documented as controversial at the time (old chunks meeting new chunks produced
  visible seams and caves that "abruptly stop and start"). [Minecraft Wiki: World
  generation/History](https://minecraft.wiki/w/World_generation/History),
  [Minecraft Wiki: Legacy Console Edition](https://minecraft.wiki/w/Legacy_Console_Edition),
  [dither8.xyz technical blog](https://dither8.xyz/blog/minecraft-cliffs-terrain/)
- Infinite worlds arrived on console in mid-2014, bringing across the biome set from
  the Java 1.7 ("The Update that Changed the World") generator. This generator —
  carver-based caves and ravines, no noise-cave rewrite — is what LCE ran **until it
  was discontinued**, i.e. it never received Java's 1.18 Caves & Cliffs Part II
  overhaul at all. [Minecraft Wiki: World generation/History](https://minecraft.wiki/w/World_generation/History)
- Confirmed by the wiki directly: "The seeds for a Large world in the final versions
  of Legacy Console Edition are mostly identical to Java Edition 1.12.2 in terms of
  terrain generation" (biome *layer* stack differs slightly — roughly Java 1.8.1+
  equivalent — but the noise stack and river stack, which is what actually shapes
  caves/terrain, are the same as Java). [Minecraft Wiki: Legacy Console Edition](https://minecraft.wiki/w/Legacy_Console_Edition)

**So: the caves people are nostalgic for are literally 2011–2017-era Java Edition
carver caves (Beta 1.8 through ~1.12), running inside a finite, much smaller world,
frozen there by console hardware/update cycles while PC Java moved on to noise caves
in 2021.** This is documented, not folklore.

### World size (documented)
Finite world options: **Classic 864×864** (the only option on Xbox 360/PS3/Vita/Wii U),
Small 1024×1024, Medium 3072×3072, Large 5120×5120 (Xbox One/PS4/Switch only). The
outer two chunks are an unbuildable water moat blending to open ocean; there is an
invisible world-edge barrier, not an infinite void border like Java/Bedrock.
[Minecraft Wiki: Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)

### World height — the messiest data point, flagged explicitly
- Java itself was 0–128 tall before the **1.2 "Anvil" update (2012)** raised it to
  0–255. Xbox 360 launched matching the *contemporary* PC limit of 128, then Title
  Update 12 raised it to 256 alongside adopting the Anvil save format — mirroring
  PC's own 1.2 change. [Minecraft Forum: New build height limit](https://www.minecraftforum.net/forums/minecraft-editions/minecraft-xbox-360-edition/mcx360-discussion/2016616-new-build-height-limit), [Minecraft Forum: Building height limit](https://www.minecraftforum.net/forums/minecraft-editions/minecraft-xbox-360-edition/mcx360-discussion/2012238-building-height-limit)
- BUT the current Minecraft Wiki page on Legacy Console Edition states the *effective*
  placeable height is **511 blocks** ("while it is theoretically 2,147,483,647 blocks
  tall, the world height stops the player at 511"). [Minecraft Wiki: Legacy Console Edition](https://minecraft.wiki/w/Legacy_Console_Edition)
- I could not reconcile 256 (generation ceiling, Anvil-format) with 511 (wiki's stated
  hard stop) from the sources gathered — they may be measuring different things (build
  limit including empty air above generated terrain, vs. the terrain generation
  ceiling itself). **Treat "0–128" from the brief prompt as shorthand for the
  pre-Anvil Java baseline LCE launched from, not a literal final-LCE number** — the
  generation ceiling that actually shaped LCE's caves/ores for most of its life was
  most plausibly 128, rising to 256 with TU12. This is the one fact in this brief I'd
  flag as needing outside verification if it matters to a specific design decision.

### Why players describe LCE caves as bigger/better connected — documented vs. folklore
Documented, sourced reasons:
1. **No 1.18 noise-cave rewrite** — LCE ran carver caves (worm tunnels + ravines) its
   entire life; Java's "sprawling" post-2021 caves never existed on console at all.
   [Minecraft Wiki: World generation/History](https://minecraft.wiki/w/World_generation/History)
2. **A finite, small world compresses discovery density.** On Classic (864×864), a
   fixed *count* of world features (ravines, mineshafts, ore) is packed into a tiny
   area compared to Java's practically-infinite border — more villages, monuments and
   mansions were documented to attempt generation on Classic than on larger sizes,
   which is Mojang/4J explicitly compensating for the small map by raising structure
   density. [Minecraft Wiki: Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features) — the same compensating logic plausibly applied to how densely caves/ravines were felt, though I did not find a source stating cave *density per chunk* was deliberately tuned up for console; that specific causal claim is **[INFERENCE]**.
3. **A shorter vertical column concentrates the same absolute cave volume into less
   space.** Documented mechanically (fewer Y-levels to spread carvers across); the
   *player-felt* consequence ("caves feel denser/more rewarding") is **[INFERENCE]**
   from that mechanical fact, though it matches general player commentary that older,
   shorter-column Minecraft felt more "compact." One general sentiment piece frames
   old-Minecraft's rewarding claustrophobia in terms of atmosphere/scarcity rather
   than pure density — related but not identical claim. [Sportskeeda: Minecraft
   players discuss features that have lost their value in recent years](https://www.sportskeeda.com/minecraft/minecraft-players-discuss-features-lost-value-recent-years)
4. **Pre-1.18 caves cut less than post-1.18 "cheese caves."** One documented 1.7.2
   change note: "Cave generation tweaked, making caves less dense and interconnected"
   (13w36a) — showing Mojang itself iterated cave density even within the carver era,
   and that at least one point in the carver era was *pulled back* for being too
   dense/interconnected, i.e. the "nice tunnels" era had a specific known-good window
   rather than being one static thing across all carver versions.
   [Minecraft Wiki: Cave](https://minecraft.wiki/w/Cave)

Folklore (repeated by players, not independently sourced by me here):
- That noise caves (1.18+) feel like "swiss cheese" and less purposeful despite being
  literally bigger — a real and recurring player complaint but reported here via a
  forum paraphrase, not a primary technical source. [Minecraft Forum: Exploring
  ravines... Fun or just a death trap?](https://www.minecraftforum.net/forums/minecraft-java-edition/survival-mode/251101-exploring-ravines-fun-or-just-a-death-trap?page=3) **[FOLKLORE, but well-attested]**
- That LCE specifically (as opposed to "old Minecraft" generally) is remembered as
  *better* than modern caves — I could not find a dedicated Reddit/forum thread
  making exactly this comparison after repeated targeted search; what I found instead
  is broad "old Minecraft caves were more atmospheric/claustrophobic" nostalgia not
  specific to the console SKU. Treat the LCE-specific framing as **the project
  owner's own lived memory**, corroborated only indirectly (LCE ran the carver
  generator its whole life, which is a documented, sufficient mechanical cause).

---

## (b) The carver cave algorithm — shape, in prose + pseudocode (not copied source)

This describes the pre-1.18 Java Edition ("MapGenCaves"-era, i.e. the algorithm LCE
ran) approach. Sourced from Minecraft Wiki, Bedrock's still-live `cave_carver_feature`
docs (which preserve the same knob names because the classic carver was later exposed
as a configurable datapack feature rather than replaced), and a modding-community
explanation of the decompiled parameter list.
[Bedrock cave_carver_feature docs](https://learn.microsoft.com/en-us/minecraft/creator/reference/content/featuresreference/examples/features/minecraftcave_carver_feature?view=minecraft-bedrock-stable),
[Minecraft Wiki: Carver definition](https://minecraft.wiki/w/Carver_definition)

### Per-chunk seeding
For every chunk, a chunk-local RNG is seeded deterministically from the world seed and
the chunk's coordinates. That RNG then rolls **how many tunnel systems start in this
chunk** — a *nested* random roll (three levels of `rand.nextInt` feeding into each
other) rather than a flat `rand.nextInt(N)`. Nesting like this skews the result hard
toward small numbers (0, 1, 2 are common; double-digit counts are rare), which is the
actual source of "most chunks have nothing, a few chunks are cave-rich" rather than an
even distribution. Community decompilation notes describe this concretely as
`rand.nextInt(rand.nextInt(rand.nextInt(15)+1)+1)`. [Web search summary of decompiled
MapGenCaves parameters, corroborated by Bedrock's `skip_carve_chance` knob which is the
same idea exposed as a flat "1-in-N chance to skip carving entirely" for the modern
configurable carver — see Carver definition page above]

```
Pseudocode — per chunk (x, z):
    rng = seed_rng(world_seed, x, z)
    tunnel_count = nested_skewed_roll(rng, max=15)   # usually 0-2
    for i in 0..tunnel_count:
        start = random_position_in_chunk(rng, y_range = [start_y_low, start_y_high])
        if rng.chance(1/7):
            spawn_large_room(start)      # a single big carved sphere/chamber
        else:
            spawn_tunnel_worm(rng, start)
```

### The tunnel walk
A tunnel is a **walked sequence of overlapping spheres/ellipsoids** along a path, not
a single shape:

- The walk has a **yaw** (horizontal heading) and **pitch** (vertical heading), both
  initialized randomly and then perturbed by small random deltas every step —
  producing the organic "worm" curve rather than a straight line.
- A `yaw/pitch ratio` parameter (explicitly named in the decompiled signature) governs
  how much more the path is allowed to turn horizontally vs. vertically per step —
  this is what keeps tunnels mostly winding sideways with occasional climbs/drops,
  rather than corkscrewing wildly.
- At each step, the carver **removes a sphere or vertically-scaled ellipsoid** of
  blocks around the current point. Radius is not constant along the tunnel — it's
  modulated by a smooth per-step multiplier (in the modern exposed form this is the
  `width_modifier`/`horizontal_radius_multiplier`/`vertical_radius_multiplier` trio),
  so a tunnel visibly narrows and widens as you walk it rather than being a uniform
  pipe.
- **Vertical bias**: tunnels are seeded with a start-Y range weighted toward the lower
  portion of the generatable column (documented for the modern equivalent as "the
  probability of cave generation being higher" in the lower band of its Y-range,
  [Minecraft Wiki: Cave / World generation](https://minecraft.wiki/w/World_generation)) — the classic carver inherited this same lower-weighted bias, just over a much
  shorter column (0–128ish instead of -56–180).
- **Branching**: a `branchStartIndex`/`branchCount` pair lets a tunnel spawn a second,
  independent worm partway along its own path, at a reduced scale — this is the whole
  mechanism behind cave systems having side-passages instead of being one single
  corridor.

```
Pseudocode — spawn_tunnel_worm(rng, start):
    yaw   = rng.uniform(0, 2*PI)
    pitch = rng.uniform(-PITCH_LIMIT, PITCH_LIMIT)
    length = rng.range(MIN_LEN, MAX_LEN)
    base_radius = rng.range(MIN_R, MAX_R)
    branch_point = rng.chance(BRANCH_CHANCE) ? rng.int(0, length) : NONE

    pos = start
    for step in 0..length:
        yaw   += rng.uniform(-YAW_DRIFT, YAW_DRIFT)
        pitch += rng.uniform(-PITCH_DRIFT, PITCH_DRIFT) * yaw_pitch_ratio
        pos += direction(yaw, pitch) * STEP_SIZE
        radius = base_radius * radius_envelope(step / length)   # widens/narrows along path
        carve_ellipsoid(pos, radius_x=radius, radius_y=radius*vscale, radius_z=radius)
        if step == branch_point:
            spawn_tunnel_worm(rng, pos)   # recursive, smaller scale
```

### Water/lava interaction
Pre-1.18, there was no aquifer system. Fluid placement was a simple flood rule tied to
sea level: any carved void that connects to (or sits at/below) the world's water table
gets flooded with water; anything carved below a fixed low Y threshold near bedrock is
lava instead (documented for the modern system's non-aquifer fallback as "areas below
Y=-55 are always filled with lava," which is the direct descendant of the classic
fixed lava-floor rule — [Minecraft Wiki: Cave](https://minecraft.wiki/w/Cave)). This
is why classic caves reliably had a "lava floor" near the bottom of the reachable
column and could flood if you broke into open ocean at depth — both are load-bearing
for the *feel* the project owner is describing (see section (d)).

---

## (c) The ravine ("canyon") carver — same family, different envelope

Documented dimensions (current wiki, descended unchanged in spirit from the classic
Beta 1.8 implementation): length **~85–127 blocks**, width **<15 blocks**, depth
**~27–62 blocks**, generation Y-range **10–72** in the classic/default config.
[Minecraft Wiki: Canyon](https://minecraft.wiki/w/Canyon)

Structurally a ravine is generated the same way as a cave tunnel — a walked path with
per-step carving — but with different tuning:

- **Much rarer per chunk** than cave tunnels: added in Beta 1.8 at a rate of roughly
  **1 attempt per 50 chunks** (vs. caves' usually-0-to-2-per-chunk). [Minecraft Wiki:
  World generation/History](https://minecraft.wiki/w/World_generation/History) — the search agent's synthesis of the wiki's "ravines added ... at a rate of one every 50 chunks" note.
- **Shape**: a single long, mostly-straight horizontal slash (much less yaw/pitch
  drift per step than a cave worm — a ravine reads as "one big crack," not a curling
  tunnel) whose cross-section is *not* a uniform tube. The Carver definition page
  (which still exposes the classic ravine's tuning knobs under their original
  intent, just as a datapack schema) names the shaping parameters directly:
  - `distance_factor` — scales overall length
  - `thickness` — scales overall breadth/height together
  - `horizontal_radius_factor` — scales width independently
  - `vertical_radius_default_factor` — scales depth independently
  - **`vertical_radius_center_factor`** — explicitly "scales the height based on
    horizontal distance from the canyon's center, resulting in a deeper center" — this
    is the documented mechanism for the classic "wide/deep in the middle, pinched at
    both ends" spindle profile the project owner is remembering.
  - `width_smoothness` — smooths the wall profile on the vertical axis (keeps it from
    looking like stacked cylinders)
  [Minecraft Wiki: Carver definition](https://minecraft.wiki/w/Carver_definition)

```
Pseudocode — spawn_ravine(rng, start):
    yaw   = rng.uniform(0, 2*PI)
    pitch = rng.uniform(-SMALL_LIMIT, SMALL_LIMIT)   # ravines drift far less than caves
    length = rng.range(85, 127)                       # documented range
    base_half_width = rng.range(2, 7)                 # keeps total width < 15
    base_half_height = rng.range(14, 31)               # keeps total depth in 27-62 range

    pos = start
    for step in 0..length:
        yaw   += rng.uniform(-TINY_DRIFT, TINY_DRIFT)   # much straighter than caves
        pos += direction(yaw, pitch) * STEP_SIZE
        t = step / length
        length_envelope = sin(PI * t)                    # pinched at both ends, wide mid
        height = base_half_height * center_bulge(vertical_offset_from_pos)
        carve_ellipsoid(pos,
                         radius_x = base_half_width  * length_envelope,
                         radius_y = height,
                         radius_z = base_half_width  * length_envelope)
```

Practically: a ravine is a *cave carver with the drift dialed down, the length/aspect
ratio dialed up, and a bespoke vertical taper* — not a separate system from scratch.
That's useful for Blocksmith: one core "walked ellipsoid carver" with two tunings
(tight-radius/high-drift/branching = cave; wide-radius/low-drift/no-branching/length-
capped = ravine) gets both shapes for roughly one implementation's cost.

---

## (d) What makes cave systems fun (developer/player consensus)

- **Connectivity beats size.** The single most consistent player complaint across both
  eras is dead ends killing momentum, not caves being too small. One detailed
  community writeup explicitly compares interconnectivity across generations and
  states plainly that "it is no fun at all if you can't explore ... without
  constantly running into dead ends." [Minecraft Forum: Exploring ravines... Fun or
  just a death trap?](https://www.minecraftforum.net/forums/minecraft-java-edition/survival-mode/251101-exploring-ravines-fun-or-just-a-death-trap?page=3)
- **Ravines are connective tissue, not just scenery.** The same source: ravines (along
  with mineshafts) are what tie separate cave pockets into one traversable network;
  removing them was observed to make caves measurably less interconnected. This is a
  direct, concrete reason to prioritize the ravine carver, not just tunnel carvers, in
  Blocksmith — a world with only tunnel-worms and no long-range ravine/canyon shapes
  will read as choppier and less "found a system" than one with both.
- **Vertical movement and landmarks matter.** The same discussion documents a single
  explored ravine over 300 blocks long / 30 wide / 60 deep as part of a cluster of 7
  intersecting ravines — the *memorability* comes from scale variance (most ravines
  small, a rare one huge) plus intersections creating recognizable junctions, not from
  every ravine being uniformly large.
- **Lava/water are both hazard and wayfinding tool.** Documented mechanically in (b):
  fixed lava floor near the bottom of the column, water flooding near sea level. Their
  presence as fixed reference *bands* (you always know roughly how deep you are by
  whether you're seeing water or lava) is itself a navigation aid, independent of any
  explicit map/waypoint system.
- **Too much open space reads as "purposeless," even when technically bigger.** The
  1.18 "swiss cheese" complaint (folklore, cited above) is the cautionary case: making
  caves objectively larger (which 1.18 documentedly did — depth alone expanded from a
  128-tall column to a -64–320 column, i.e. tripled+) did **not** universally read as
  "better" to players; several found the added openness diluted the sense of a
  purposeful, explorable network. This directly supports the project owner's framing:
  volume is not the goal, *tunnel character and connectivity* is.
- **Ore-in-wall visibility drives exploration** — this is common design knowledge
  (seeing a diamond vein embedded in a cave wall pulls the player toward it) but I did
  not find a dedicated citation distinct from the general ore-distribution pages
  already covered in (a)/(b); treating this point as **[INFERENCE from well-known
  design practice, not independently sourced here]**.

---

## (e) Recommended design for Blocksmith, given the 3DS constraints

Stated constraints from the task: 12 MB world-store cap, 16×16×16 chunks, a small
fixed column height, and generation must not stall a 268 MHz (O3DS) / 804 MHz (N3DS)
ARM11 core.

### The cost problem, quantified
Vanilla's carver reaches into a neighborhood of chunks around the one being generated
so that a tunnel *started* in a distant chunk still carves correctly into the current
one. Bedrock's own feature docs describe this literally: `cave_carver_feature` "carves
a cave through the world in the current chunk, and in every chunk around the current
chunk **in an 8 radial pattern**." [Bedrock cave_carver_feature docs](https://learn.microsoft.com/en-us/minecraft/creator/reference/content/featuresreference/examples/features/minecraftcave_carver_feature?view=minecraft-bedrock-stable) A radius of 8 chunks means a **17×17 = 289-chunk neighborhood**
must be considered (rolled for tunnel starts, and for any chunk that rolls a start,
walked) every time a single chunk's caves are finalized. That figure is corroborated
independently by a community technical explanation of the same "radius 8" constant.
[Web search: cave generation chunk radius technical breakdown, synthesizing Bedrock docs and MapGenCaves community analysis]

**289× the per-chunk RNG/walk cost, paid for every chunk generated, is not affordable
on this hardware.** Even though most of those 289 rolls resolve to "zero tunnels here"
(the nested skewed roll from (b) keeps most chunks empty), you still pay the *roll*
cost 289 times, and for every chunk that *does* roll a tunnel you pay a multi-step walk
that must then be tested against blocks potentially several chunks away — worst case,
a full worm walk (up to `MAX_LEN` steps, each doing an ellipsoid stamp) has to run to
completion just to find out whether it grazes the one chunk you're actually trying to
finish generating.

### Recommendation: decouple cost from neighborhood radius entirely

Rather than reduce the radius and accept vanilla's simulate-then-discard cost model at
a smaller scale, restructure the carver so a chunk can determine **exactly which
tunnels could possibly reach it, in O(1) lookups, without simulating any neighbor**:

1. **Coarse deterministic tunnel index.** Partition the world into coarse cells much
   larger than a chunk (e.g. 4×4 or 8×8 chunks per cell — tune to taste). For each
   cell, a tunnel's full parameter set (start point, yaw, pitch, length, radius,
   branch point/branch params, cave-vs-ravine tuning) is derived **entirely** from a
   hash of `(world_seed, cell_x, cell_z)` — no RNG state carried from anywhere else,
   no simulation. This is the standard trick for turning a stateful walked-RNG process
   into something any chunk can query independently: the whole tunnel is a pure
   function of its cell coordinates, so any chunk can recompute it from scratch and
   get the identical answer a neighboring chunk would.
2. **Bounding check, not simulation, for reach.** Because a tunnel's max length and
   radius are bounded (cap `MAX_LEN` and `MAX_R` at generation-parameter level), you
   can compute a tunnel's bounding box/sphere cheaply from its hashed parameters
   *before* walking a single step. A chunk being generated only needs to check the
   handful of coarse cells within `MAX_LEN + MAX_R` of itself (almost always just its
   own cell plus immediate neighbors, not 289 chunks) — reject any tunnel whose
   bounding volume doesn't touch the chunk, and only walk the ones that survive that
   cheap test.
3. **Analytic evaluation, not voxel-by-voxel stamping across the whole tunnel.** For a
   tunnel that does reach the current chunk, evaluate the walked path (per (b)'s
   pseudocode) but only emit carve operations for the segment of the path whose
   bounding sphere intersects this chunk — you still need to walk from the tunnel's
   start to reach the right segment (the walk is sequential/stateful because yaw/pitch
   drift each step), but you never touch the block grid for steps outside this chunk,
   and you never do this for tunnels whose cell is out of range at all.
4. **Coarse cave-presence mask for early-out.** Before running any tunnel evaluation
   for a chunk, generate a cheap low-resolution 3D presence mask (e.g., one sample
   every 4 blocks per axis, from fast value/gradient noise, no RNG walk at all) as a
   first-pass gate: if the mask says "definitely solid, far from any hashed tunnel's
   bounding volume," skip the fine carve pass for that region of the chunk entirely.
   This mirrors vanilla's own effect (most chunks are cave-free) but achieves it via a
   cheap noise sample instead of a full nested-RNG roll plus a discarded walk.

Net effect: **generation cost is proportional to how many tunnels actually reach a
given chunk (typically 0–3), not to a fixed 289-chunk neighborhood.** This is a
different implementation strategy from vanilla's (vanilla literally re-derives and
partially re-walks neighbor chunks' RNG state; this instead makes every tunnel a
stateless, independently-recomputable object), which also satisfies the project's
"no borrowed Minecraft code" requirement structurally, not just in wording — this is
a reimplementation of the *shape* of the classic algorithm, built differently to fit
the memory/CPU budget.

**Recommended neighborhood radius, concretely: cap tunnel `MAX_LEN`/`MAX_R` such that
a tunnel's coarse cell need only check its own cell plus a 1-cell ring (radius 1 in
coarse-cell units) — i.e., far short of vanilla's 8-chunk radius, but because cells
are themselves several chunks wide, this still comfortably covers tunnels several
chunks long.** This is a design choice (cell size × radius), not a fixed constant —
tune cell size against how long you want the longest tunnel/ravine to be able to run
before it's forced to stop and branch.

### 2D vs. 3D — do not go pure 2D for the primary system
A 2D (heightmap-modulated) cave system is far cheaper (one hash per column, no walk)
but section (d) is explicit that **vertical movement and multi-level connectivity are
named repeatedly as core to what makes caves feel rewarding**, and that is exactly
what a 2D approach cannot produce (a 2D mask can carve "wide or narrow at this X/Z,"
but not "this tunnel dips under that one and reconnects three levels down"). Recommend
true 3D worm-walk carving (per (b)) as the primary tunnel/ravine system, with a cheap
**2D or low-res-3D mask used only as the early-out gate** described in step 4 above —
2D as an optimization layer, not as the generator itself.

### Practical sequencing note **[INFERENCE — general engineering judgment, not sourced]**
Because tunnel objects are fully deterministic from `(seed, cell)`, they don't need to
be persisted at all — they can be recomputed on demand whenever a chunk near them is
touched (loaded, or an adjacent chunk is generated), which sidesteps needing to store
any cave-specific data in the 12 MB world store beyond the resulting carved voxels
themselves. This is very likely necessary given the stated memory cap, but I have not
seen Blocksmith's actual save format (out of scope per this task's restriction) so
cannot confirm it's compatible with what's already built — flag this as a question for
whoever owns the world-store format.

---

## (f) World height recommendation

LCE's generation ceiling was most plausibly **128 blocks** for most of its
documented lifetime (see the flagged discrepancy in section (a) about the later
256/511 figures). Given a much tighter memory cap than any console LCE ran under (12
MB vs. a console generation with tens/hundreds of MB of working RAM available for
world data), Blocksmith should target **noticeably shorter than even LCE's 128**,
because column height is one of the most direct memory-budget levers available
(every additional 16-tall chunk slab is a fixed, multiplicative cost across every
column in the loaded area). **[INFERENCE — memory-scaling claim, not sourced, but
follows directly from the stated 16×16×16 chunk unit and 12 MB cap]**

Recommendation: **a column in the roughly 64–96 block range**, structured the same
way LCE's shorter-than-Java column concentrated its cave/ore story: keep the "cave
band" (where tunnel/ravine carving is weighted, per the vertical-bias note in (b))
proportionally similar to LCE's — i.e. don't just shrink the *reachable* column and
leave caves spread thin through it; deliberately keep the carver's Y-weighting biased
toward a compact lower band so the *player-felt density* of tunnels-per-block-dug
stays close to what LCE delivered in its own shorter column, rather than diluting to
match a taller Java-style spread. Concretely: if the column is ~80 blocks, don't
spread cave generation evenly across all 80 — bias it toward roughly the bottom
two-thirds, mirroring how LCE's own 128-tall column concentrated its documented
lower-weighted cave probability rather than spreading generation evenly floor to
ceiling. This is a direct, intentional echo of the mechanism identified in (a)/(b),
not a new claim.

---

## Open questions

1. **The LCE height discrepancy (128 vs 256 vs 511)** in section (a) — worth a
   dedicated verification pass (e.g. a decompilation-based wiki like a fandom talk
   page, or directly testing on an emulator) if the exact number matters to a design
   decision, rather than the "shorter than modern Java, roughly comparable to
   pre-Anvil" framing used here.
2. **Whether Blocksmith's existing chunk/world-store format already supports
   recomputing generated content on demand** (needed for the stateless-tunnel-object
   approach in (e)) — out of scope for this research pass since it required reading
   the project tree, which this task explicitly excluded.
3. **Exact numeric tuning** (tunnel count skew, radius ranges, branch chance, ravine
   rarity) was described here in shape/pseudocode per the task's instruction not to
   copy Java source — actual constants for Blocksmith's version should be chosen by
   playtesting against the "feel" target (connectivity + vertical movement + rare-but-
   memorable big ravines), not by trying to match vanilla's numbers exactly, since the
   world is a different size/height/hardware target entirely.
4. **I could not find a single, dedicated primary source (Reddit thread, dev post) that
   makes the LCE-specific "these caves were better" comparison** as opposed to general
   "old Minecraft was more atmospheric" nostalgia — the causal case here rests on the
   mechanical fact that LCE ran carver-only generation its entire life (well-sourced)
   plus the project owner's own recollection, not on a third-party writeup confirming
   the exact sentiment. Worth knowing this is an inference chain, not a documented
   consensus claim, if it's ever repeated externally.
