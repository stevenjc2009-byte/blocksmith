# Beta 1.7.3 terrain generation — research brief (b173)

Prepared 2026-09-01. Scope: Part A is web research (Minecraft Beta 1.7.3's real algorithm,
external sources, cited). Part B is a read-only survey of Blocksmith's own
`source/world/worldgen*.c/h` and `source/world/noise.c/h` (file:line evidence only, nothing
inferred without saying so). Part C combines the two into a verdict for the 3DS.

**HEADLINE FINDING, stated up front because it changes the shape of everything below:**
Blocksmith already ships a Beta-1.7.3-shaped 3D density-field generator. It has shipped since
v1.7.0 (`GEN_VERSION_DENSITY`, `source/world/worldgen_density.c`) and gained biome identity in
v1.8.3 (`GEN_VERSION_BIOME`). It is the generator every new world gets today —
`GEN_VERSION_FOR_NEW_WORLDS` resolves to it (`source/world/genversion.h:104`). Its lattice is
**already** 5×17×5 with 4-block/8-block cells, its cost **has already been measured** at
1.37–1.64× the legacy heightmap generator, and its own source comments already cite the Beta
5×5×17 grid as the documented reference shape it copies (not ports). `docs/ROADMAP.md:88-98`
describes "v1.8.7 — Terrain" as *"World generation is rewritten to the Beta 1.7.3 shape... so a
biome is a place you travel through rather than a tile you stand on"* — and the great majority
of that description is already true of the shipped code, under earlier version numbers. See
Part C for what is and is not still open.

---

## Part A — Research: what Beta 1.7.3 actually did

### A1. The 3D density field vs. the 2D heightmap

A 2D heightmap asks one question per `(x, z)` column — "how high is the ground" — and fills
straight down. The result can only ever be single-valued: for a given `(x, z)` there is exactly
one transition from air to solid, so there is no way to represent an overhang (solid, then air,
then solid again going up), a natural arch, or a floating island.

Beta's generator instead computes a scalar **density** value at every point in 3D space and
declares a cell solid where density is positive and air where it is negative. The ground surface
is *wherever the field crosses zero* — and because the field varies with `y` as well as `x, z`,
it can cross zero more than once above the same column. That multiple-crossing property is the
entire mechanism behind overhangs, arches and floating islands; they are not a separate feature
bolted onto a heightmap, they are what happens for free when the "is this solid" question is a
3D field instead of a 2D one.

Pseudocode shape (not Beta's literal code, which is Java and under a different license — this is
the algorithm's structure only):

```
density(x, y, z):
    lo   = fbm3(minLimitNoise, x/scale, y/scale, z/scale)   # calm/broad field
    hi   = fbm3(maxLimitNoise, x/scale, y/scale, z/scale)   # rough/detailed field
    sel  = fbm3(mainNoise,     x/scale, y/scale, z/scale)   # blend weight, saturating
    if sel is near its low extreme: base = lo
    elif sel is near its high extreme: base = hi
    else: base = lerp(lo, hi, normalized(sel))

    depth_bias = depthNoise(x, z)          # per-region "how much terrain here"
    vertical_bias = f(y)                    # tapers density down near the ceiling, up near bedrock

    return base * amplitude(depth_bias) + vertical_bias
solid = density(x, y, z) > 0
```

[Terrain generation in Minecraft (nornagon)](https://blog.nornagon.net/terrain-generation-in-minecraft/);
[World generation – Minecraft Wiki](https://minecraft.wiki/w/World_generation);
[Noise generator – Minecraft Wiki](https://minecraft.wiki/w/Noise_generator)

### A2. The specific noise generators and their roles

Beta 1.7.3's `ChunkProviderGenerate` used (names as commonly documented in decompiled-source
comparisons and reimplementation projects):

- **`minLimitPerlinNoise`** and **`maxLimitPerlinNoise`** — two independent octave-noise fields
  defining a lower and upper density bound at each point. One reads as the "calm" field, the
  other as the "dramatic" field.
- **A "main"/selector noise** — blends between the min and max fields. Where it saturates toward
  one extreme the terrain is entirely the calm field; toward the other, entirely the dramatic
  field; in between is the transition band. This crossfade is what produces regions of gentle
  rolling ground sitting next to regions of broken, cliffy ground, rather than one uniform
  texture of hills everywhere — sometimes called "range noise" or "punctuated equilibrium" for
  this reason.
- **Depth/scale noise** (`depthNoiseScaleX/Z`, `biomeDepthWeight/Scale` families) — a
  regional-scale field that varies how tall and how varied the terrain gets from one region to
  the next, i.e. governs amplitude rather than the fine shape.
- **A separate surface/beach pass** — decides the top few blocks' material (grass/dirt/sand)
  after the density field has already decided where the ground is; it does not feed back into
  the density field itself.

Decompiled-comparison numeric constants (Beta 1.7.3 vs. a later snapshot, confirmed identical
between the two by a community comparison, and independently corroborated by the `i73`
reimplementation project's goal of bit-for-bit Beta 1.7.3 terrain):
`coordinateScale = heightScale = 684.412`, `lowerLimitScale = upperLimitScale = 512.000`,
`depthNoiseScaleX = depthNoiseScaleZ = 200.000`, `depthNoiseScaleExponent = 0.5`,
`mainNoiseScaleX = mainNoiseScaleZ = 80.0`, `mainNoiseScaleY = 160.0`, `baseSize = 8.5`,
`stretchY = 12.0`, and **sea level = 64** in Beta 1.7.3 (63 in the later snapshot compared
against).
[Neo-Beta preset gist](https://gist.github.com/Brottweiler/371012e16bd6f24127934f6750641885);
[Minecraft Forum — Neo-Beta preset thread](https://www.minecraftforum.net/forums/minecraft-java-edition/seeds/customised-worlds/197244-minecraft-beta-customized-terrain-generator-preset);
[Minecraft Forum — "1.7 Generator File" decompile thread](https://www.minecraftforum.net/forums/minecraft-java-edition/discussion/3032686-1-7-generator-file);
[i73 reimplementation project](https://github.com/coderbot16/i73);
[MC-116572 — Far Lands / heightScale interaction](https://bugs-legacy.mojang.com/browse/MC-116572).

Octave counts: multiple sources describe Beta/pre-1.18 Minecraft as combining **three noise
fields, each built from up to 16 octaves** of Perlin noise, though one technical retrospective
on a from-scratch reimplementation notes 16 octaves is *"more than necessary — only 4 or 5
octaves were really needed"* for a visually equivalent result.
[Terrain generation in Minecraft (nornagon)](https://blog.nornagon.net/terrain-generation-in-minecraft/);
[Noise generator – Minecraft Wiki](https://minecraft.wiki/w/Noise_generator);
[open-builder issue #67](https://github.com/Hopson97/open-builder/issues/67) (the "4 or 5 octaves needed" retrospective).

### A3. The coarse sampling grid and trilinear interpolation

Beta did **not** evaluate the density field at every block. It evaluated it on a coarse lattice —
commonly documented as **5 points horizontally by 17 points vertically per chunk**, each
horizontal cell spanning 4 blocks and each vertical cell spanning 8 blocks — and trilinearly
interpolated every block between those lattice points from the 8 surrounding samples.
[World generation – Minecraft Wiki](https://minecraft.wiki/w/World_generation) (density-grid
description); corroborated by a from-scratch reimplementation's DeepWiki documentation citing
the identical "coarse 3D noise grid with trilinear interpolation" mechanism:
[Noise-Based Chunk Generation](https://deepwiki.com/g122622/minecraft-reborn/5.2-noise-based-chunk-generation).

Why this matters for performance: a 16×16×128 chunk column has 32,768 blocks. Evaluating a
multi-octave 3D noise field at every one of them is the naive cost. A 5×5×17 lattice has 425
points — **about 1/77th as many** — and interpolating the remaining 32,343 points costs only
adds and multiplies (lerps), not noise hashes. This is stated as the explicit reason for the
grid's existence in the sources above, and it is the same reasoning Blocksmith's own generator
uses (Part B/C). No source found gives Mojang's own precise "evaluations saved" figure in raw
count form; the 425-vs-32,768 arithmetic above is this brief's own derivation from the documented
grid dimensions, not a quoted Mojang number — flagged as **derived, not sourced**.

### A4. The vertical bias / height-attenuation term

Sources describe a term added to the blended noise value that is a function of `y` alone: it
pulls density strongly positive (solid) near the bottom of the world and strongly negative (air)
near the top, tapering in between. This is what stops the world from being a uniform sponge from
bedrock to the build ceiling — without it, the min/max/selector blend alone has no reason to
prefer "more solid at the bottom." In the modern (post-1.18) data-driven equivalent this is
exposed as explicit `density_factor` / `density_offset` parameters plus a documented `height_bias`
concept: *"the higher a block is, the less density it has and vice versa."*
[Custom world generation/noise settings – Minecraft Wiki](https://minecraft.fandom.com/wiki/Custom_world_generation/noise_settings)
(1.16 experimental settings, `xz_factor 80.0`/`y_factor 160.0` — matching the Beta
`mainNoiseScaleX/Y` constants in A2, evidence the underlying shape survived the 1.18 rewrite even
though the surface behaviour changed); general height-bias description via search synthesis, not
independently re-verified against Mojang source for this brief — flagged **inference from
secondary sources**, not a directly quoted primary constant.

### A5. Biome feed: Beta vs. modern

**Before Beta 1.8**, biome placement used two independent noise fields — **temperature** and
**rainfall** — smoothly blended, with the biome at a point determined by where that
(temperature, rainfall) pair fell in a 2D classification. Critically, **height variation was
independent of biome**: cliffs and mountains could occur in any biome up to the world ceiling,
because biome did not feed the density/height field at all in this era.
[Biome/Before Beta 1.8 – Minecraft Wiki](https://minecraft.wiki/w/Biome/Before_Beta_1.8);
[The World Generation of Minecraft (Alan Zucconi)](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/).

**From Beta 1.8 onward** (and in all modern versions), the biome map feeds the terrain shape
directly via per-biome "depth" and "scale" parameters — average height and how far it can vary —
so a desert biome is deliberately flatter than an extreme-hills biome by construction, not by
coincidence of where temperature/rainfall happened to land.
[The World Generation of Minecraft (Alan Zucconi)](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/).

### A6. Cost, quantified as far as sources allow

- **Naive (per-block) 3D noise evaluation**: 32,768 density evaluations per 16×16×128 column,
  each itself a sum of up to three multi-octave (up to 16-octave) noise fields.
- **With the 5×5×17 coarse lattice + trilinear interpolation**: 425 density evaluations per
  column, the same interpolation cost (cheap lerps) for the remaining 32,343 blocks.
- That is roughly a **77× reduction** in the number of points where the expensive noise math
  runs, for the density field alone. This ratio (425 : 32,768) is this brief's own arithmetic
  from the sourced grid dimensions (A3); no source was found stating Mojang's own chosen ratio
  in exactly these terms, so treat the multiplier as **derived**, not quoted.
- No authoritative source was found giving Beta's actual measured milliseconds-per-chunk cost on
  period hardware; that number could not be determined from the web research available. What
  *is* well attested is that the coarse-lattice trick exists specifically because full-resolution
  3D noise was judged too expensive — every source describing the mechanism frames it as a
  performance optimisation, not a quality one.

### A7. What was lost in 1.8, and why players miss Beta 1.7.3

Beta 1.8 ("the Adventure Update") rewrote world generation to make biome identity drive terrain
height and amplitude, added continents/open ocean framing, strongholds, villages, ravines and
ore-vein-style mineshafts, and removed the old temperature/rainfall-only biome system along with
the far-out "Far Lands" terrain corruption.
[How Beta 1.8 Quietly Ended the Far Lands Era](https://www.vanillaminecraftservers.net/blog/how-beta-1-8-quietly-ended-the-far-lands-era).

Community sentiment, sourced rather than assumed: Minecraft Forum threads from the era describe
the pre-1.8 generator as having **more height variation** — "any type of landscape" reachable in
a short walk, "an overhang and a thin rocky formation... [and] flat land isolating both of
these" all within one small area — and describe extreme-hills-style terrain becoming markedly
more common (i.e., more uniform) after 1.8.
[What did the 1.8 terrain change mean for you? — Minecraft Forum](https://www.minecraftforum.net/forums/minecraft-java-edition/discussion/169046-what-did-the-1-8-terrain-change-mean-for-you);
[Did the Terrain Become BORING? — Minecraft Forum](https://www.minecraftforum.net/forums/minecraft-java-edition/discussion/153131-did-the-terrain-become-boring-update-6-5-13-1-7-is).
Reddit's r/GoldenAgeMinecraft community is reported (secondhand, via the vanillaminecraftservers
retrospective above — not independently fetched for this brief) as characterising Beta 1.7.3 as
"the last beta that still felt like beta." Sustained community effort to recreate it — the
Neo-Beta datapack, Cascades, Better Than Adventure, i73 — is itself evidence of the sentiment's
durability over more than a decade.
[Neo-Beta datapack](https://www.planetminecraft.com/data-pack/neo-beta-datapack-beta-1-7-3-esque-terrain-generation-for-your-1-16-2-worlds/);
[Cascades datapack](https://modrinth.com/datapack/hybrid-beta);
[Minecraft Better Than Adventure mod overview](https://sportskeeda.com/minecraft/minecraft-better-than-adventure-mod-everything-need-know).

---

## Part B — Read-only survey of Blocksmith's own generator

All line numbers are from the tree as read on 2026-09-01, under
`C:\Users\steve\Documents\3ds-project-folder\mc\source\world\`. Nothing in this tree was
modified for this brief.

### B1. 2D heightmap, 3D density field, or hybrid? — **Hybrid, version-dispatched, and this is by design**

The project has **two live generators**, selected per-world by a permanent version stamp
(`world/genversion.h`), never mixed within one world:

- `GEN_VERSION_LEGACY` (1) — a pure 2D value-noise heightmap. `worldgen.c:232-257`
  (`legacyHeight`) computes one height per `(x, z)` via `noiseFbm2` and fills straight down;
  `worldgen.c:806-883` (`legacyColumn`) is the fill loop. No 3D field of any kind feeds the
  ground shape (caves are a separate carve, `worldgen.c:213-228`, `worldgenIsCave`).
- `GEN_VERSION_DENSITY` (2) and `GEN_VERSION_BIOME` (3) — a true 3D density field.
  `worldgen_density.h:8-52` states the design directly: *"The legacy generator asks one question
  per (x, z)... A density field asks a question per **block**... The surface is wherever the
  field crosses zero."* `worldgen_density.c:207-238` (`densityCore`) is the density function
  itself.

Dispatch: `worldgen.c:264-266` (`worldgenHeight`) — `return (g->version >= GEN_VERSION_DENSITY) ?
wgdHeight(...) : legacyHeight(...);` — and `worldgen.c:792-804` (`worldgenColumn`) — the same
`>=` gate on `wgdColumn` vs `legacyColumn`.

**Which one is "current"?** `genversion.h:104`: `#define GEN_VERSION_FOR_NEW_WORLDS
GEN_VERSION_NEWEST`, and `genversion.h:101`: `#define GEN_VERSION_NEWEST GEN_VERSION_BIOME`. Every
new world made by this build today gets the 3D density-field generator with biome identity on
top — **not** the legacy heightmap. The legacy path exists solely so worlds saved before
2026-08-24 keep the terrain they already have (`genversion.h:1-54` states the whole rationale).
This is fact, read directly off the dispatch code, not inferred.

### B2. Chunk and world dimensions

- `chunk.h:50`: `#define CHUNK_DIM 16` — chunks are 16×16×16 blocks.
- `world.h:23-24`: `#define COLUMN_CHUNKS 8` and `#define WORLD_HEIGHT (COLUMN_CHUNKS *
  CHUNK_DIM)` → **128 blocks tall**. A column is 16×128×16 blocks.
- This 128-block height is the **same height Beta 1.7.3 itself used** (sea level 64, i.e. half of
  128 — see A2's sourced Beta constant and `worldgen.h:39-43`'s own comment: *"64 is half of the
  128-block world, the same fraction Beta 1.7.3 uses in a world of the same height."*). This is a
  direct, in-repo claim of parity with Beta's own dimensions, not this brief's inference.

### B3. Noise evaluations per column — counted from the actual loops

**Legacy** (`legacyColumn`, `worldgen.c:806-883`): for the 256 `(x, z)` cells of a column,
`worldgenHeight` → `legacyHeight` calls one `noiseFbm2` at `GEN_OCTAVES = 4` octaves
(`worldgen.c:31,234`), and `worldgenIsSandy`/`worldgenBiome` calls a second `noiseFbm2` at
`GEN_BIOME_OCTAVES = 2` octaves (`worldgen.h:86`, `worldgen.c:123-125`) — 256 × (4+2) = **1,536
`value2At` octave-evaluations** for height+biome alone. On top of that, `worldgenIsCave`
(`worldgen.c:213-228`) runs **two 3-octave-equivalent `noiseFbm3` calls
(`GEN_CAVE_OCTAVES = 2`, `worldgen.h:192`) per underground block** that is deep enough to be
carvable (`legacyColumn`'s inner loop, `worldgen.c:844-877`) — this is the generator's dominant
cost, explicitly measured and stated in `app/worker.h:1-13`: *"Generating one column costs two 3D
fBm evaluations per underground block."*

**Density** (`wgdColumn`/`buildGrid`, `worldgen_density.c:311-338, 516-541`): the coarse lattice
is `GEN_D_GRID_XZ = 5` × `GEN_D_GRID_XZ = 5` × `GEN_D_GRID_Y = 17` = **425 lattice points per
column** (`worldgen_density.h:64-65`). Each lattice point's `densityCore` (`worldgen_density.c:
207-238`) calls three `noiseFbm3`s — `lo` at `GEN_D_LOW_OCTAVES = 2`, `hi` at
`GEN_D_HIGH_OCTAVES = 3`, `sel` at `GEN_D_SEL_OCTAVES = 2` (`worldgen_density.h:97-107`) — so
425 × (2+3+2) = **2,975 `value3At` octave-evaluations for the lattice alone**, plus 25 `xz` grid
points × `GEN_BIOME_OCTAVES` (2) = 50 `value2At` calls to resolve `(base_h, amp)` once per xz
column rather than per lattice point (`worldgen_density.c:314-326`, comment states this
explicitly: *"25 fBm2 evaluations per column, against 425 if it were resolved per lattice
point"*). **The same cave carve as legacy runs on top of this, unchanged** — `wgdColumn` calls
the identical `worldgenIsCave` (`worldgen_density.c:657-670`, explicit comment: *"The existing
one... Reused rather than reinvented"*).

**Corrected 2026-09-01 — this paragraph previously claimed corroboration it does not have.**
It read that the 2,975 figure was "corroborated independently by `docs/ROADMAP.md:73-76`
(v1.8.6 plan)", quoting *"each of those [tree-height] questions costs roughly 476 noise
evaluations, against about 2,975 for an entire column"*. Both halves of that were wrong:

- The line anchor is dead. `docs/ROADMAP.md` was rewritten on 2026-09-01 and lines 73-76 are
  now part of the fog section, not a cost figure.
- The sentence it quoted is one the ROADMAP itself **retracts**. `docs/ROADMAP.md:128-131`
  records the measurement that replaced it: over 1,271 columns and 6 seeds, a height query
  costs **208** noise evaluations and a whole column costs **24,204** — not ~476 and ~2,975.
  Citing it as agreement was citing a known-wrong number as support for another number.

**What 2,975 actually is, and it is still correct:** the octave-evaluation count for the
lattice ALONE — 425 lattice points × 7 octaves (2 low + 3 high + 2 selector) = 2,975, which
is the arithmetic derived immediately above. It is not, and never was, the cost of an entire
column. The whole-column figure is the measured **24,204**, of which `wgdColumn` is 91%
(22,031 evaluations — `docs/ROADMAP.md:145-148`). So 2,975 is a component of 22,031, which is
a component of 24,204; there is no disagreement between the three, only three different
scopes that this document had conflated.

The 77.1× interpolation win computed in the next paragraph is unaffected: both of its terms
(229,376 per-block against 2,975 lattice) are lattice-scope figures, so the ratio stands.

**Cost multiplier of interpolation vs. per-block**: without the lattice, the same density
question asked at every one of 32,768 blocks would cost 32,768 × 7 = 229,376 `value3At`
evaluations; the 425-point lattice costs 2,975 — a **77.1× reduction**, matching Part A's
independently-derived Beta ratio almost exactly (both come from the same 425-vs-32,768 point
count, so the agreement is expected, not coincidental corroboration of two independent
measurements).

**Measured wall-clock cost** (host x86-64, NOT ARM11 — flagged explicitly in the source):
`app/worker.h:10-13`: legacy *"0.757–0.804 ms/column over three seeds at radius 6, twice"*;
density *"1.055–1.310 ms/column on the same runs, a ratio of 1.37–1.64×."* This is the single
most load-bearing measured number in this whole brief, and it was **not measured on real
hardware** — stated as a limitation in the source comment itself, not an omission on this
brief's part.

### B4. Noise implementation

`noise.h:1-16` and `noise.c`: **value noise**, not Perlin or simplex, in 16.16 fixed point
(`fx`, `int32_t`). One hash per lattice corner (`corner2`/`corner3`, `noise.c:13-21`), smoothstep
interpolation (`smooth`, `noise.c:29-34`, `3t²-2t³`), summed as fractional Brownian motion
(`noiseFbm2`/`noiseFbm3`, `noise.c:159-196`). The header states the reason for value noise over
Perlin/simplex explicitly (`noise.h:29-34`): *"it needs one hash per lattice corner and no
gradient table... trivially reproducible in integers."* Fixed-point (not float) is likewise
explicit and deliberate (`noise.h:1-16`): a host test and the ARM11 must agree bit-for-bit, and a
float density field would disagree by an ulp on either side of the solid/air threshold — visible
as terrain differing between the two.

### B5. Caching of noise/height results

**The task's framing needs correcting against what the code actually does.** `worldgen.c:348`
(`t.ground = worldgenHeight(g, t.x, t.z);`) and `worldgen.c:388-390` (three more
`worldgenHeight(...)` calls checking a wide tree's footprint) are **not** cache reads — they are
full, uncached height queries, each of which (on a density-generation world) walks
`wgdHeight` (`worldgen_density.c:463-512`): a 2×2×17 corner sub-lattice, i.e. 68 `densityCore`
calls × 3 `noiseFbm3` = **204 `value3At`-octave-evaluations per single tree-height query**
(matches the `GEN_D_GRID_Y = 17` constant directly: `2*2*17*3 = 204`). `treeInCell`
(`worldgen.c:297-403`) pays this cost for every tree cell it draws, uncached, because a tree cell
can belong to a column that has not (yet, or ever, in this session) had `wgdColumn` run for it.

**The actual cache is `s_top`/`wgdColumnTops`**, a different mechanism, single-column and
single-use: `worldgen_density.c:41-54` declares `static int16_t s_top[CHUNK_DIM][CHUNK_DIM]` plus
`s_top_cx`/`s_top_cz`/`s_top_valid`, filled once per `wgdColumn` call (`worldgen_density.c:
524-540`) and exposed read-only via `wgdColumnTops()` (`worldgen_density.c:727-732`), which
**refuses** (`return NULL`) if the caller asks for any column other than the one most recently
generated (`if (!s_top_valid || s_top_cx != cx || s_top_cz != cz) return NULL;`). Its one
consumer is `worldgenScatter` (`worldgen.c:574-582`), which needs all 256 surface heights of the
column it is currently decorating and would otherwise pay 256 × 204 = 52,224 `value3At` calls —
`worldgen_density.h:266-278` states the reasoning directly: *"Asking worldgenHeight() for them
would cost 256 evaluations of a 2×2×17 corner lattice — 204 fBm3 each... for numbers wgdColumn
has already computed and is holding in a static."*

**So: yes, there is a height cache, but it is scoped to exactly one column at a time, exists only
to serve the tall-grass/flora scatter pass, and does not help `treeInCell`'s neighbour-height
queries at `worldgen.c:348` and `388-390`, which remain full-cost `wgdHeight()` calls.** This
"tree height query is 476 noise evaluations, uncached" gap is exactly what `docs/ROADMAP.md:
73-76` (v1.8.6, "Fixed — chunk loading speed") already identifies as unfixed and already scopes
to a future version — corroborating that this brief's reading matches the project's own plan.

---

## Part C — The verdict

### C1. Framing correction before the numbers: most of "v1.8.7" already shipped

`docs/ROADMAP.md:88-98` describes v1.8.7 — Terrain as: *"World generation is rewritten to the
Beta 1.7.3 shape: the tall, dramatic terrain with overhangs and floating pieces... Existing worlds
keep their generator... Larger and more varied land shapes, so a biome is a place you travel
through rather than a tile you stand on."*

Per Part B, the density-field generator, the per-world version stamp that keeps existing worlds
on their original generator, and biome-driven amplitude/height are **already built and already
the default for new worlds** (`GEN_VERSION_DENSITY` shipped as v1.7.0, `GEN_VERSION_BIOME` as
v1.8.3 — both numerically earlier than v1.8.7 in the roadmap's own sequence). This is stated as
fact from `genversion.h`'s comments and dispatch code, not this brief's speculation. What this
means for whatever work remains under the "v1.8.7" label could not be fully determined from a
read-only pass — the roadmap entry may be stale relative to the shipped code, or it may still
scope real remaining work (see C4) that just isn't spelled out in the four lines quoted above.
**This brief does not resolve that ambiguity — it is a question for whoever owns the roadmap.**

### C2. The lattice: not a recommendation, a report of what is already measured

The task asked for "a concrete recommended lattice size for this project's chunk dimensions."
That lattice already exists and already matches the historically-documented Beta shape almost
exactly:

| | Beta 1.7.3 (Part A) | Blocksmith (Part B) |
|---|---|---|
| Horizontal samples/chunk | 5 (commonly documented) | `GEN_D_GRID_XZ = 5` |
| Vertical samples/chunk | 17 (commonly documented) | `GEN_D_GRID_Y = 17` |
| Horizontal cell size | 4 blocks | `GEN_D_CELL_XZ = 4` |
| Vertical cell size | 8 blocks | `GEN_D_CELL_Y = 8` |
| World height | 128 | `WORLD_HEIGHT = 128` |
| Sea level | 64 (half height) | `GEN_SEA_LEVEL = 64` |

**Arithmetic, per column, at this lattice:** 5 × 5 × 17 = 425 density-lattice points, each
costing 7 octave-evaluations (2 low + 3 high + 2 selector) = **2,975 `value3At` calls**, plus 25
biome-resolution points × 2 octaves = 50 `value2At` calls. **Per-block equivalent** (no lattice):
32,768 blocks × 7 octaves = 229,376 `value3At` calls. **Reduction: 77.1×** (32,768/425 = 77.10,
and the octave multiplier cancels in the ratio since it applies to both sides equally).

There is no case, given this project's own already-measured numbers, for recommending a
*different* lattice. A denser lattice buys smoother density transitions at a steep, direct cost
(a 9×9×17 lattice, for instance, at 4× the xz points, would roughly quadruple the 2,975-call
figure above with no evidence it is needed — the shipped comment at `worldgen_density.c:264`
already states the *coarse* sampling is what makes the terrain "readable" as broad shapes rather
than noise soup, i.e. coarser is closer to correct-looking, not just cheaper).

### C3. Cost multiplier vs. the legacy generator — already measured, shown working

Measured (host x86-64, NOT ARM11 — see below), `app/worker.h:10-13`:

- Legacy: 0.757–0.804 ms/column.
- Density (current default): 1.055–1.310 ms/column.
- **Ratio: 1.37×–1.64×.**

This already includes the sea-level fill and the tall-grass scatter, not just the density field
in isolation — it is a whole-column, apples-to-apples figure. The independent noise-evaluation
arithmetic in B3/C2 (2,975 extra `value3At` calls for the lattice, on top of a cave-carve cost
shared identically by both generators) is consistent with a sub-2× multiplier: the density
field's lattice cost is real but small next to the cave carve, which both generators pay equally
and which `app/worker.h`'s own framing comment identifies as the dominant cost of *either*
generator ("two 3D fBm evaluations per underground block" — this is the cave pass, not the
height/density pass, and it is unchanged between legacy and density).

**The one gap that matters most:** every number above is a host (x86-64, gcc -O2) measurement.
`docs/ROADMAP.md:49-51` (v1.8.5) states plainly: *"Every memory number in this project is
currently arithmetic on paper; the only hardware reading on record is from before v1.7.0 and is
for an Old 3DS."* No ARM11 timing for the density generator specifically was found anywhere in
this read-only pass. Host multipliers do not reliably predict ARM11 multipliers when the two
generators have different instruction mixes (the density path does more integer multiply/shift
work per lattice point and less branchy per-block work than the cave-carve-dominated legacy
path) — an in-order ARM11 with no branch prediction to speak of can shift a ratio measured on an
out-of-order x86 core in either direction. **Could not determine the real ARM11 ratio; only a
console boot-probe timing run would settle it.**

### C4. Is 128 blocks tall enough for overhangs and floating islands to be worth having?

**Yes, by the project's own arithmetic, and this is already measured, not projected.**
`worldgen_density.c:130-141`'s comment states the overhang precondition directly: an overhang
needs the density field's upward step (over one 8-block vertical lattice cell) to exceed 8
blocks, which only happens where amplitude is large enough. It reports two actual measurements:

- At the *old* (smaller) amplitude table (10/18/34/58 blocks peak-to-trough by biome), two test
  seeds' lattices had a **maximum upward step of only −2.68 and −0.56 blocks** — i.e. *"no
  overhang was geometrically possible anywhere in those worlds."*
- At the *current, shipped* amplitude table (14/28/48/64), the mountainous seed reaches **+16.76
  blocks** of upward step and **9.99% of all lattice steps rise** — i.e. overhangs are already
  geometrically occurring in shipped worlds, concentrated in the mountain biome as the comment
  notes matches Beta's own behaviour.

Since the world height (128) already matches Beta's own height (Part B2), and amplitude — not
height — is what the code's own measurement identifies as the actual overhang lever, **there is
no basis in this project's own evidence for recommending a taller world**, and doing so would
have a real, quantified memory cost: `budget.h:21`, `WORLD_BUDGET_BYTES = 12 MB`, sized (per
`budget.h:3,18`) against *"the 17×17-column worst case... ~9.25 MB."* Raising `WORLD_HEIGHT`
(and therefore `COLUMN_CHUNKS`) scales that worst case roughly linearly — a 50% taller world
(192 blocks, `COLUMN_CHUNKS = 12`) would push the same 17×17 worst case toward ~13.9 MB, over the
12 MB cap outright, before any headroom. This was not tested; it is straight-line arithmetic on
the stated 9.25 MB figure and should be treated as **an estimate, not a measurement**, but it is
enough to say a height increase is not free and nothing in the sourced evidence says it is
needed.

### C5. Risks, in the order they would actually bite

1. **No ARM11 measurement of the density generator exists anywhere in this tree.** Everything in
   C3 is a host ratio. Given the project's own standing note that Old 3DS free-memory and timing
   figures are "arithmetic on paper" past v1.7.0, this is the single largest unknown blocking any
   claim that the shipped generator (already the default!) is actually fine on real hardware —
   not a v1.8.7-specific risk, a risk that already exists today for every new world a player
   starts.
2. **`treeInCell`'s uncached neighbour-height queries** (`worldgen.c:348`, `388-390`) are a known,
   roadmapped (v1.8.6) cost — up to 476 noise evaluations per tree, several times per column,
   against 2,975 for the whole rest of the column's generation. This is real work already
   scheduled ahead of v1.8.7 in the roadmap's own order, not new information from this brief.
3. **The roadmap-vs-code discrepancy in C1** — if whoever plans v1.8.7 is working from
   `docs/ROADMAP.md`'s text alone without cross-checking `genversion.h`, there is a real risk of
   re-doing (or worse, re-versioning and thereby breaking the legacy-world guarantee for) work
   that already shipped. The version-stamp mechanism (`genversion.h`) is explicitly designed so
   that this kind of mistake is structurally prevented for *existing* worlds (a wrong new version
   number just mints `GEN_VERSION_4` cleanly), but it cannot prevent wasted engineering time
   against a stale plan.
4. **Amplitude/overhang tuning was validated on very few seeds.** The measurement in C4 cites
   exactly two seeds (4242, 90210) for the "no overhang possible" / "overhang now possible"
   comparison, and the biome-table comment (`worldgen_density.c:70-151`) is explicit that the
   table values were "tuned by eye against the terrain they produce," not derived from a broader
   sweep. Low risk to correctness (nothing here can crash or corrupt a world), moderate risk to
   whether "feels like Beta" is actually being hit consistently across seeds.

**Suggested order, if this brief's findings are accepted:** (1) get one real console timing
reading for `wgdColumn` at radius matching the shipped render-distance settings — this answers
C3's open question directly and cheaply, reusing the existing `app/worker.h` instrumentation; (2)
reconcile `docs/ROADMAP.md`'s v1.8.7 entry against what `genversion.h`/`worldgen_density.c`
already ship, so the next planning pass starts from the true baseline rather than from the
roadmap text as written; (3) only then consider any amplitude/seed-coverage tuning work, since
correctness and the roadmap's own accuracy are cheaper to fix and block everything downstream of
them.

---

## Gaps this brief could not close

- Beta 1.7.3's actual **measured** milliseconds-per-chunk on period hardware — not found in any
  source consulted; would need a period benchmark or the original dev's own notes, neither
  located.
- Beta's *exact* octave counts for `minLimitPerlinNoise`/`maxLimitPerlinNoise`/main noise — every
  source found describes "up to 16 octaves across three fields" in general terms; no source gave
  a definitive per-field count (e.g. "min uses 16, max uses 16, main uses 8") for Beta 1.7.3
  specifically as opposed to the broader Perlin-noise-era range of versions.
- Whether `docs/ROADMAP.md`'s v1.8.7 entry is stale or still scopes real, unwritten work beyond
  what Part B found already shipped — would need the roadmap's author (steve) or a diff against
  whatever task list originally justified the v1.8.7 entry.
- Real ARM11 timing for `wgdColumn` — would need a console boot-probe run, per C5 item 1.
