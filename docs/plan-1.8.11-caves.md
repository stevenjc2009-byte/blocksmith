# v1.8.11 — Caves: algorithm and cost

This document answers one question: **how** would legacy-console-style cave generation
actually be built on this hardware, and what would it cost. It is the algorithm-and-cost
half of the v1.8.11 rung; `docs/research/caves-legacy-console.md` is the companion
document covering what Legacy Console Edition caves *felt* like and why — that research
is cited here where it bears on an engineering decision, and is not repeated.

This document does not implement anything. Nothing under `source/` was touched to
produce it. Where a number below could not be measured, it is labelled **[reasoned]** or
**[assumed]** rather than presented as fact — see the provenance convention
`docs/research/caves-legacy-console.md` §0 already uses, which this document follows.

A second agent is actively editing `source/world/worldgen.c` and the plant/scatter
placement code while this was written. Every citation below is to a function name and a
constant name, not a line range that edit could move; where a line number is given it is
for the reader's convenience against the tree as read, not a claim that it will still be
there.

---

## 1. What the current generator actually does

**It is a pure 3D noise field, not a carver.** There is no path, no walk, no start point,
no branch, no room. `worldgenIsCave(g, x, y, z)` (`world/worldgen.c:443-449`) answers a
single block's cave-or-not question from that block's own coordinates and the seed alone:

1. Below `GEN_CAVE_FLOOR` (y = 1) the answer is always false — the bottom of the world is
   solid (`world/worldgen.h:207`).
2. Otherwise, two **independent** 3D fractal-noise fields are evaluated at `(x, y, z)`,
   each with its own salt (`g->cave_salt[0]`, `g->cave_salt[1]`, mixed once in
   `worldgenInit`, `world/worldgen.c:145-146`) and each squashed vertically relative to
   horizontal — `GEN_CAVE_SHIFT_XZ` = 5 (32-block horizontal lattice period) against
   `GEN_CAVE_SHIFT_Y` = 4 (16-block vertical period), a 2:1 squash so tunnels "run flat"
   rather than stand on end (`world/worldgen.h:190-192`, comment at `:48-49` in
   `worldgen.c`).
3. A block is cave if **both** fields land inside a narrow band around their measured
   median (`GEN_CAVE_CENTRE` 0x7800 ≈ 0.469, `GEN_CAVE_HALF` 0x0CCD ≈ 0.05 —
   `world/worldgen.h:193-194`). The comment on `GEN_CAVE_HALF` explains why a band and not
   a threshold: one field below a threshold gives disconnected blobs; a band around a
   value is a thickened iso-*surface* (a sheet), and two independent sheets intersect in a
   curve — a thickened curve is a tunnel. This is the same "isosurface intersection"
   technique, not a walked path.
4. `GEN_CAVE_MIN_DEPTH` = 5 blocks below the column's own surface, enforced by every
   caller, not by `worldgenIsCave` itself (`worldgen.h:196-206`; the function's own
   comment at `worldgen.c:711-717` says explicitly that it does not know depth). This is a
   **deliberate design decision**, not a placeholder: it guarantees no carved cell can ever
   leave a grass block floating, open under a spawn point, or reach the surface at all. The
   stated price, in the same comment, is that **the current generator produces no cave
   entrances** — the only way into a cave is to dig into it.

**Measured connectivity** (the tuning comment at `worldgen.h:181-189`, a 6-connected flood
fill over a 96x64x96 box across seeds 1337/1616/4242/7/99999/20260818): at the shipped
half-width 0.05, 5.44% of the underground is carved and 99.2% of that carved volume sits
in systems bigger than 100 blocks, every tested seed reaching at least 98.9%. **This
matters for §1.5**: the current field is *not* the disconnected "swiss cheese" the band
technique gives at a threshold — it is already predominantly one connected structure.

### 1.1 The per-column corner cache (v1.8.7)

`caveCacheBuild()` / `caveFieldAt()` (`world/worldgen.c:359-429`) do not change what is
computed, only how often. The two shift constants above force a specific redundancy: a
16-block column begins on a multiple of 16, `GEN_CAVE_SHIFT_XZ` is 5, so both octaves' x
and z lattice indices are **constant across the whole column** and only the y index
varies. Measured on host for one biome column at seed 1337 (comment at `:304-310`):
18,571 `worldgenIsCave()` calls, 41,786 octave evaluations, but only 342 distinct lattice
cells behind them — a 131x redundancy.

`caveCacheBuild(c, g, bx, bz)` pays 2 salts x 2 octaves x (9 + 17 rows) x 4 corners = 208
`rngHash3` calls and 96 smooths **once per column**, into a `WorldGenCaveCache` that lives
in the caller's `WorldGenScratch` (`world/worldgen_scratch.h:46-62`), not a file static —
see §1.3 for why that distinction is load-bearing. `caveFieldAt()` then answers each of up
to 32,768 per-column cave tests by trilinear interpolation read out of that table, in
`world/noise.c`'s own order and normalisation — verified bit-identical to the uncached
form over 3,436,800 cells across 4 seeds with zero mismatches (`worldgen.c:438-442`).

`worldgenIsCaveCached()` (`worldgen.c:451-481`) is the cached entry point both fill loops
actually call. It also orders the two field tests so the second (more expensive, because
fewer blocks reach it) is only evaluated for the roughly 22% of blocks that survive the
first — "worth about a third of the whole pass, and free" per the comment at `:473-476`.

### 1.2 Where it is called from, and how the sea fill interacts with it

Two independent fill loops call `worldgenIsCaveCached()`:

- The **legacy** fill loop in `worldgen.c` (not read in full for this document, but its
  cave test is `caveDirect`/`worldgenIsCave` per the file's own top-of-file comment: the
  legacy generator is frozen byte-for-byte and must never change).
- The **density** generator's `wgdColumn()` (`world/worldgen_density.c:588-811`), which is
  what every world made from v1.7.0 onward actually uses. Inside its per-chunk downward
  walk (`:701-808`), a solid cell is tested for cave **only if** `d_surf =
  top[z][x] - 1 - y >= GEN_CAVE_MIN_DEPTH` (`:757-759`) — depth measured from the
  column's own top surface, not from the top of the current solid run, because measuring
  from the run would let the first cave reset the counter and turn a tunnel system into a
  scatter of isolated pockets (measured: run-depth gave 10,360 air cells under the surface
  against 27,312 for surface-depth, comment at `:752-756`).
- **The sea fill interacts with caves already, and the interaction is instructive for
  §3.3.** `wgdColumn()`'s `sea[z][x]` flag (`:692-787`) floods air from `GEN_SEA_LEVEL - 1`
  downward **only until the first solid cell**, in that one column's own downward walk —
  deliberately, because filling *every* air cell below sea level would flood every sealed
  cave in every world (comment at `:678-682`). A carved cell that happens to be reached by
  that same walk before it hits solid (an overhang breaking into the underside of open
  water, still the same column) gets water or ice exactly like any other air cell
  (`:772-776`). **A cave that connects to the ocean sideways, through a neighbouring
  column, is never flooded by this mechanism at all** — the walk is strictly per-column and
  top-down. This is not a bug in the existing code; it is a scope boundary this plan has to
  respect rather than silently expand (see §3.3).

### 1.3 Why this shape was chosen, and the file-static lesson

Nothing above reads or writes state outside the call it belongs to. `worldgenIsCave()` is
explicitly "stateless as of v1.8.7, and therefore safe to call from any thread"
(`worldgen.h:721-725`), and the cache that makes it cheap lives in the caller's
`WorldGenScratch`, not a file static, **because a file static already failed once,
measured**: the same comment block and `worldgen_scratch.h:1-19` both cite the pre-v1.8.7
failure — with the cave cache and the column-top buffer as file statics, two lanes running
disjoint columns corrupted 24 of 32 columns and refused 159 of 192 generations outright.
This is the standing lesson §2.7 and §7.4 below are built around: **anything new must be
caller-owned scratch, never a file static, from the first commit.**

### 1.4 Cost today (measured)

CHANGELOG.md:61-63: the v1.8.7 cache made the cave pass **3.09x faster** and a whole
column **2.20x faster** (1.190 → 0.542 ms/column, **on host x86-64**, not the ARM11). No
on-device timing exists anywhere in this tree for the cave pass or for any other part of
generation — `docs/plan-1.8.7-terrain.md` states plainly that no hardware run has happened
since v1.2.5, and that standing gap applies here unchanged.

### 1.5 Verdict: tune this, or replace it?

Both, for different reasons, and the split is worth stating plainly rather than picking
one:

- **Connectivity is already right.** 99.2% of carved volume in systems over 100 blocks is
  not "pockets" in any sense the ROADMAP entry's phrase would object to. A pure tuning
  pass (narrower band, fewer octaves) could push the *shape* further toward directional
  spaghetti without changing the architecture at all — this is
  `docs/research/caves-legacy-console.md` §8 Phase 1's recommendation, and it is real and
  cheap.
- **But the ROADMAP entry for v1.8.11 already commits to more than a retune**: "The carver
  walks a damped random path with a sine-tapered radius, spawns rooms and branches, and
  crosses chunk borders so a tunnel actually goes somewhere" (`docs/ROADMAP.md:326-328`).
  A noise field, however tuned, cannot produce a *room* (a discrete oversized chamber) or
  a *branch point* (a specific place where one tunnel becomes two) — those are objects a
  walked path has, and a continuous field does not. **If the ROADMAP wording is the
  target, a walker is required**, and no amount of retuning the existing field reaches it.
  This document takes that as the target and designs the walker; §6 and §8 note where a
  cheaper retuned-field fallback would sit if the walker's cost turns out not to be
  affordable.

---

## 2. The proposed approach: a column-independent worm carver

**[proposed]** — everything in this section is a design, not a measurement, checked
against the codebase's own existing patterns and constraints rather than against a
running implementation.

### 2.1 The core idea, and the determinism argument made explicit

Classic Minecraft-style cave carving walks a random path that starts in one chunk and can
wander into several of its neighbours before it stops (`caves-legacy-console.md` §7).
`worldgenColumn()` today generates a column from nothing but its own coordinates and the
seed (`world/worldgen.h:741-748`) — no neighbour column is ever consulted, by any pass.
The task brief's own framing is the answer: **iterate every tunnel system deterministically
from the seed for every region in a neighbourhood radius around the column being
generated, and carve only the part that lands inside that column.**

Concretely, following the exact idiom `worldgenDecorate()` already uses for trees
(`worldgen.c:793-931`, comment at `:718-725`):

1. Divide the world into **regions** — proposed at one region = one column, 16x16 blocks,
   for reasons given in §2.2.
2. Every region `(rx, rz)` is deterministically assigned zero or more tunnel systems by
   hashing `(g->seed, rx, rz)` through a new salt, following the house convention every
   other decorator in this file uses (`rngHash2(rngMix(g->seed ^ SALT_X), rx, rz)` — the
   same call shape `treeInCell()` uses for `SALT_TREE`, `worldgen.c:580`). **A pure
   function of the region coordinate and the seed, nothing else.**
3. Each system's **entire path** — start point, start height, initial heading, every
   step's yaw/pitch drift, every step's tapered radius, every branch point and its
   children, whether it is a room instead of a walk — is *itself* a pure function of
   `(g->seed, rx, rz, system_index)`, derived the same way. No part of it is read from or
   written to anything outside the current call's own stack and scratch.
4. To generate column `(cx, cz)`: enumerate every region in a fixed, symmetric
   neighbourhood `[cx - R, cx + R] x [cz - R, cz + R]` (§2.2 sizes `R`), and for each
   region that step 2 says has any systems, **re-walk each one from scratch** (step 3),
   testing every carve stamp the walk produces against the target column's own bounds —
   `(x >> 4) == cx && (z >> 4) == cz`, exactly `treePut()`'s clip
   (`worldgen.c:726-737`) — and only writing cells that pass.

**Why this is deterministic and column-independent, stated as a proof rather than an
assertion.** Every system is uniquely identified by `(rx, rz, i)`. Its geometry (step 3)
depends on nothing but that identity and the seed, so two columns that both include region
`(rx, rz)` in their neighbourhood independently re-derive **bit-identical** geometry for
system `i` — there is no cache, no order, no "who computed it first" to disagree about.
Each column then applies the identical clip rule to its own, disjoint 16x16 footprint, so
no cell is ever written by two columns and no cell that should be carved is ever missed,
**provided** `R` is large enough that every system whose geometry could reach into the
column is actually enumerated (§2.2's job). This is the same non-clash argument
`worldgenDecorate()`'s own comment already makes for trees ("That clip is what makes the
pass order-independent" — `:718-725`) applied to a wider neighbourhood and a heavier
per-region cost; it is not a new argument, it is the existing one at a different radius.
Thread-independence and session-independence follow for the same reason `treeInCell()`
already has them: nothing but `(seed, coordinates)` is ever read.

### 2.2 Region size and the neighbourhood radius — the central cost/quality tradeoff

This is the single biggest lever on cost, and it deserves to be sized with real arithmetic
rather than picked by feel.

A region is proposed at one column (16 blocks), for two reasons: it reuses the existing
per-column iteration `worldgenColumn`/`wgdColumn` already do, and it is roughly the same
granularity the legacy carver's own "per-chunk gate" worked at (1-in-15 or 1-in-7 per
16-block chunk, per `caves-legacy-console.md` §2).

Let `max_reach` be the largest distance (in blocks) any point of a system's carved geometry
can be from its region's own footprint — a **hard clamp** enforced inside the walk itself
(§7, risk 2), not a statistical hope. To guarantee no system that could touch column
`(cx, cz)` is missed, every region whose 16x16 footprint could hold a start point within
`max_reach` of that column must be scanned, which needs a neighbourhood radius (in
columns):

```
R = ceil((max_reach + CHUNK_DIM) / CHUNK_DIM)      CHUNK_DIM = 16
```

and a `(2R+1) x (2R+1)` region scan per column generated. The relationship is what makes
this the central tradeoff — it is not linear:

| max_reach (blocks) | R (columns) | regions scanned per column |
|---:|---:|---:|
| 8  | 2 | 25 |
| 16 | 2 | 25 |
| 24 | 3 | 49 |
| 32 | 3 | 49 |
| 40 | 4 | 81 |
| 48 | 4 | 81 |

**[reasoned]** — this table is arithmetic on the formula above, not a measurement. The
recommendation (§5) is to start at `max_reach` in the 16–24 range (R = 2 or 3, 25–49
regions), well short of legacy's own generous reach (Beta 1.7.3's `range*16-16` figure with
further random reduction, `caves-legacy-console.md` §2 Lineage A), and to widen it later
**only if a host measurement (§4.2) shows headroom** — tightening after the fact from a
measured number is cheap; discovering the cost is unaffordable after committing to a wide
`max_reach` is not.

The early-out this table's cost depends on: most regions roll **zero** systems (the
legacy gate is 1-in-7 to 1-in-15 per region, `caves-legacy-console.md` §2), so step 2's
single hash-and-compare rejects the large majority of the 25–49 scanned regions for the
cost of one `rngHash2` call each — no walk is ever started for a region that rolled
nothing. Of the regions that do roll a system, a second cheap reject — the system's own
`[start ± max_reach]` bounding box against the target column's bounds — throws away most
of the rest before a single step is walked, the same shape of optimisation
`treeInCell()`'s horizontal-reach reject already is for trees (measured to win "about
6.6% of column generation" there, `worldgen.h:596-624`). §4.2 estimates what is actually
left to walk after both rejects.

### 2.3 The walk-and-carve primitive

**[proposed]**, following the shape `caves-legacy-console.md` §2's synthesis lays out,
adapted to this codebase's fixed-point-only, no-divide, no-FPU constraints (the ARM11 has
no integer divide instruction, stated repeatedly across `noise.c`, `worldgen.c` and
`worldgen_density.c`, and it also has no hardware trig — every existing noise function
avoids both).

Per step of a walk: advance position along the current (yaw, pitch) heading by one unit
step; damp the yaw/pitch **velocity** toward zero and add a small fresh random kick, so the
path curves smoothly rather than jittering (the momentum idiom both cited lineages and the
Classic carver use, `caves-legacy-console.md` §2); compute a radius that is smallest at
both ends of the system and largest near the middle — the "sine-tapered radius" the
ROADMAP entry itself names. Nothing in this codebase currently has a fixed-point sine —
`noise.h` defines only `FX_SHIFT`/`fxToInt`/`fxFromInt` (`noise.h:24-27`) and a search of
`noise.h`/`rng.h` for trig turned up nothing. **A small new lookup table (a quarter-sine in
`fx`, the same shape as `noise.c`'s own `smooth()` avoiding a divide) is new code this
version has to add**, not something to be found and reused.

The **carve stamp**: at each step, test every integer cell in a local bounding box around
the current position against an ellipsoid inequality — `dx² + k*dy² + dz² < radius²`, the
single-inequality technique `caves-legacy-console.md` §2's Classic-carver citation
describes, with `k` giving the wider-than-tall squash (both Beta/1.7.10 lineages use 0.5
linear on the vertical radius; the Classic lineage folds the same idea into `k = 2` on
`dy²` — either is a design choice for playtesting, not a fact to import exactly, per that
document's own closing note that the two lineages disagree on this ratio). A hit inside the
target column's bounds sets a bit in the column's own carve mask (§3.1); a hit outside
those bounds is discarded — this is the clip from §2.1, applied per cell rather than per
block-loop-iteration because the carve stamp, not the whole tunnel, is the unit of work
that needs clipping.

**Memory shape**: the walk is carved *as it steps*, not stored and carved afterward — each
step's state (position, yaw, pitch, their velocities, a step counter, the current radius)
is a handful of words, overwritten every step, so the peak scratch a walk in progress needs
is O(1) rather than O(path length). This matters directly for §4.1.

### 2.4 Rooms, as a degenerate case

A "room" (an oversized roughly-spherical chamber, `caves-legacy-console.md` §2 point 6) is
the same carve-stamp primitive with zero steps: one ellipsoid, centred on the region's own
draw point, with a larger radius than any single step of an ordinary tunnel produces. No
new machinery — the per-region draw decides "room instead of walk" and calls the same
stamp function once instead of calling the walk loop.

### 2.5 Ravines, as a second preset sharing the same scan

Per `caves-legacy-console.md` §3 and §8 Phase 4: structurally the same walked-ellipsoid
primitive, retuned — far less yaw/pitch drift (straighter), a taller/narrower ellipsoid
instead of wide/flat, no branch step, and roughly an order of magnitude rarer trigger
(~1-in-50 chunks against tunnels' ~1-in-7 to 1-in-15).

**Recommendation: one neighbourhood scan, two draws per region, not two scans.** Since the
scan itself (§2.2) is the expensive part, running it twice — once for tunnels, once for
ravines — would roughly double the per-column region-scan cost for a feature that fires an
order of magnitude less often than the one it would be riding alongside. Instead, the same
per-region hash that decides tunnel-system count should also make a second, independently-
salted draw (a new `SALT_RAVINE`, following the house convention) for "does this region
also start a ravine" — both answered while the region is being visited once.

**This is where ravines could make the whole neighbourhood more expensive than tunnels
alone need it to be, and it needs to be flagged rather than assumed away.**
`caves-legacy-console.md` §3 gives width (5–7 blocks) and depth (40–62 blocks) for
ravines, sourced from general descriptions, but **no horizontal length figure** — the
document itself does not claim one (§10 point 5 area; the gap is in what §3/§6 actually
source). If a ravine's horizontal reach needs to be longer than an ordinary tunnel's
`max_reach` to read as "one long crack" rather than a short gash, sizing the *shared*
neighbourhood radius `R` to the ravine's larger reach would inflate the region count for
every column, most of which are only asking about ordinary tunnels. The cheaper answer
(§7, risk 6) is a **separate, wider `R_ravine`** used only for the ravine draw, scanned as
its own (also early-out-guarded) pass over more regions than the tunnel scan — affordable
specifically because the rarity of the gate rejects nearly all of that wider neighbourhood
for one hash call each.

### 2.6 Version gating — not optional

`genversion.h`'s whole design exists because terrain is never stored, only regenerated
from the seed (`genversion.h:1-17`), and the v1.8.3 biome-identity change already
demonstrated the failure mode this must avoid: gating the wrong way moved **seven of
twelve** pinned legacy fingerprint hashes before it was caught (`genversion.h:79-91`). A
new carve pass landing on an existing `GEN_VERSION_DENSITY` or `GEN_VERSION_BIOME` world
would silently reshape every cave under every player's existing buildings on their next
load — exactly what this file's entire mechanism exists to prevent.

**This plan requires a new `GEN_VERSION_CAVES` constant**, appended after
`GEN_VERSION_BIOME` (never renumbered, per the file's own rule at `genversion.h:60-63`),
becoming the new `GEN_VERSION_NEWEST` / `GEN_VERSION_FOR_NEW_WORLDS`. The carve pre-pass
(§3.1) is called only when `g->version >= GEN_VERSION_CAVES`; every world stamped
`LEGACY`, `DENSITY`, or `BIOME` keeps exactly the noise-field cave test it has today,
unconditionally. A host suite check in the same shape as
`testWorldgenLegacySandyWideSweep()` — hash a fixed set of pre-`GEN_VERSION_CAVES` columns
before and after the change and assert zero movement — is the check that would have caught
the v1.8.3 incident on day one and is the one this change needs from day one too.

### 2.7 Two-lane safety

Every byte of state the walk and the neighbourhood scan touch — the walker's step state,
the per-column carve mask (§3.1) — belongs in `WorldGenScratch`, the caller-owned struct
`worldgen_scratch.h` already defines for exactly this reason (§1.3). Nothing here may
become a file static, including "just for now" — that was the exact shape of the pre-v1.8.7
bug. `tests/lanes_test.c` already exercises the claim/ready-ring bookkeeping with two real
host pthreads over the real `worldgenColumn` (`app/lanes.h:1-19`); the same methodology —
two threads generating overlapping neighbourhoods concurrently, hashes compared — is the
check this new pass needs before it can be trusted on a New 3DS's second lane.

---

## 3. Interaction with the existing fill loops

### 3.1 A new pre-pass carve mask

The existing per-block `worldgenIsCaveCached()` calls inside `wgdColumn()`'s downward walk
(§1.2) cannot be reused as-is: they answer one block's question from that block's own
coordinates, and a worm carver's answer for one block depends on a neighbourhood scan that
should be paid **once per column**, not once per block. The proposed shape mirrors
`interpolateColumn()`'s own existing bitmask idiom (`worldgen_density.c:309-376`, `s->solid`):
a **new pre-pass**, run once at the start of column generation, that walks the
neighbourhood (§2) and writes hits into a new per-column bitmask —
`uint16_t carve[WORLD_HEIGHT][CHUNK_DIM]`, the same shape and size as the existing `solid`
buffer. The downward fill loop then tests this mask in place of calling
`worldgenIsCaveCached()`, for `GEN_VERSION_CAVES`-and-above worlds only (§2.6). This is a
**replacement of the per-block noise test for carved-version worlds, not an addition to
it** — running both would double-carve and mix two visually incompatible shapes (an
isotropic band and a directional tube) for no benefit; the whole point of the new version
is a different shape, not a superset.

### 3.2 Lava

Per `caves-legacy-console.md` §4 and §6: the sourced legacy rule is a **fixed absolute Y**
— any cell the carve stamp would otherwise leave as air, below a lava line, is lava
instead — and the same document's own rescaling arithmetic (§6) finds this needs **no
adjustment** for a 128-tall world, because Beta 1.7.3's own y<10 figure was already sourced
from an identical 128-tall convention. This is nearly free to add: one more branch inside
the carve-stamp function from §2.3 (`if (y < GEN_LAVA_LINE) write LAVA else write AIR`,
folded into the same per-cell write the mask already makes), no new pass and no new scan.
A new `BLOCK_LAVA` id is required — `block.h` currently defines no lava block at all
(`caves-legacy-console.md` §7 confirms this by repository search) — and per that same
file's own append discipline (`block.h:104-167`, the `BLOCK_COUNT`/id-numbering comments
and their `_Static_assert`s), it must be appended past the current highest id, never
inserted or renumbered.

Standalone rarer lava "lakes" above the fixed floor (§4 of the research document) are a
separate, rarer per-region draw of the same shape as ravines (§2.5) — explicitly proposed
as a **deferral** (§6 below), both because the research document itself flags the sourced
frequency as inconsistent across versions and because it is a second neighbourhood-scan
contributor this plan should not add until the fixed-floor version is measured and shipped.

### 3.3 Water — and the honest limit of what is affordable here

The fluid simulation itself already exists and does not need to be rebuilt
(`world/water.c`, `world/water.h`, an 8-level flow model already used for
oceans/lakes). What is missing is only *placement* inside a carved cave.

**The honest limit, stated plainly rather than glossed over**: §1.2 already established
that the existing sea fill is strictly column-local and top-down — it cannot flood a cave
that connects to the ocean sideways through a neighbouring column, because no column's fill
loop ever looks at its neighbour's blocks. A worm-carved tunnel is far more likely than the
current noise field to actually reach sideways into a flooded neighbour (that lateral
connectivity is the entire point of a walked path that "crosses chunk borders"), which
makes this existing limitation more visible, not less, the moment this version ships.
**True cross-column flood-fill connectivity is a materially bigger feature** — it needs
either multi-column state that survives past one call (in direct tension with the "nothing
persists" architecture §2.7 depends on) or a second full pass over an already-generated
neighbourhood of columns, and it is not scoped into this version (see §6).

The affordable v1.8.11 answer, matching `caves-legacy-console.md` §8 Phase 3's own
recommendation: keep water in caves **column-local**, using exactly the mechanism that
already exists — a carved cell reached by the *same column's own* downward sea-fill walk
(the overhang case `wgdColumn()` already handles at `:772-776`) gets flooded exactly as it
does today, and nothing more. A cave that opens into the ocean only through a neighbouring
column's territory stays dry until that gap is worth its own, separate architectural
effort.

### 3.4 Ores (v1.8.12) — ordering, stated for completeness

Not part of this version's scope, but the ordering question the brief asks about has one
answer worth recording here so v1.8.12 does not have to re-derive it: **ore placement runs
after the carve mask has already been applied to the column's blocks**, as a decoration
pass in the same slot `worldgenDecorate()` already occupies (after the terrain fill, before
or alongside the scatter pass), placing vein blocks only where the target cell is still
`BLOCK_STONE` — the same `only_into_air`-shaped guard `treePut()` already uses for
canopies, inverted to `only_into_stone`. This is exactly the sourced legacy order
(`caves-legacy-console.md` §4: "ores are placed after caves and ravines are carved"), and
it is what makes an ore vein straddling a tunnel wall visible without further digging: the
part of the vein inside the carved void is silently never placed, and the part against the
wall is. No coupling to the carve pass's internals is needed beyond reading the finished
block grid, so v1.8.12 can be built and version-gated independently of this version's own
gate.

---

## 4. Cost

### 4.1 Memory, with real numbers

`WorldGenScratch` is currently **16,256 bytes per lane**, measured with
`arm-none-eabi-size` on the real ARM target, not derived on paper
(`worldgen_density.c:37-44`, cross-checked by `worldgen_scratch.h`'s own per-field byte
comments totalling the same figure). Two lanes on a New 3DS: 32,512 bytes of lane-static
storage.

The proposed addition:

| addition | size | shape |
|---|---:|---|
| carve mask, `uint16_t[WORLD_HEIGHT][CHUNK_DIM]` | 4,096 B | identical shape to the existing `solid` buffer (`worldgen_density.c` comment, `:91-94`) |
| walker step state (position, yaw/pitch + velocities, radius, step counter) | ≈ 32–64 B | O(1) per walk in progress — carved as it steps, not stored (§2.3) |
| **total addition, per lane** | **≈ 4,128–4,160 B** | |

**[reasoned]**, not measured — the true figure depends on struct packing and would need
the same `arm-none-eabi-size` check `worldgen_density.c`'s own comment insists on (that
comment explicitly corrects an earlier hand-computed figure that was wrong by omitting two
buffers, `:37-44` — the standing lesson is to measure this, not restate arithmetic).
Against the current 16,256 bytes this is roughly a **25–26% increase**, to about
20,384–20,416 bytes per lane, ≈ 40.8 KB for two lanes.

**This is not charged against `WORLD_BUDGET_BYTES`.** `budget.h`'s own scope note
(`budget.h:10-22`) is explicit: the 12 MB cap counts block *storage* — `malloc`'d Columns,
Chunks and LightColumns on the application heap — and nothing in `WorldGenScratch` outlives
the call that fills it. The two budgets are unrelated, and `docs/ROADMAP.md`'s own v1.8.5
section already records a prior instance of this exact confusion being raised and rejected
for a different feature (`ROADMAP.md:51-54`) — the same conclusion applies here without
new argument.

### 4.2 Compute — an operation-count estimate, and how to actually measure it

**No ms figure is claimed here.** The existing 0.542 ms/column figure (§1.4) is a real,
measured number; nothing below is. What follows is a reasoned operation count, built the
same way the existing cave-cache work was justified before it was measured (the redundancy
argument at `worldgen.c:304-310`), offered so the shape of the cost is visible before a
single line of code exists.

Take the recommended starting point from §2.2: `max_reach` = 16–24 blocks, `R` = 2–3,
25–49 regions scanned per column.

- **Region-level reject** (§2.2): one `rngHash2` call and a compare, per region. At a
  legacy-shaped gate of roughly 1-in-10 (between the two sourced figures of 1-in-7 and
  1-in-15), roughly 90% of 25–49 regions — **22 to 44 regions** — cost exactly this one
  hash call each and nothing more.
- **System-level reject**: for the remaining ~3–5 regions that do roll at least one
  system, a bounding-box test (a handful of integer compares, no hash) against the target
  column, following `treeInCell()`'s own horizontal-reach-reject shape. Most systems whose
  start point is far from the target column are rejected here too.
- **Real walks**: **[assumed]**, in the absence of a measurement, that 1–3 systems per
  column generation survive both rejects and are actually walked in full. Each walk is
  roughly 24–64 steps (bounded by `max_reach` and the per-step advance distance); each
  step does a table-lookup sine/cos, a handful of fixed-point multiplies for the
  yaw/pitch/position update, and a carve-stamp over a local bounding box of perhaps 3–7
  blocks per axis (up to roughly 5x5x3 = 75 cell tests at a typical mid-tunnel radius,
  fewer near the tapered ends).

Multiplying through: 2 real walks x 40 steps x 50 cell tests/step ≈ **4,000 cell tests**,
each a handful of integer compares and a possible mask-bit write; plus roughly 30–50 hash
calls for the region/system rejects, each of the same order of cost as the existing cave
cache's own `rngHash3` calls. **The resulting order of magnitude — tens of thousands of
integer operations per column — is comparable to, not obviously larger than, the pre-v1.8.7
cave pass's own per-column cost** (18,571 `worldgenIsCave()` calls per column before that
cache existed, per §1.1's citation), which the existing codebase already generated at
interactive rates on host. This is offered as a plausibility argument that the design is in
the right cost class, **not as a substitute for measuring it.**

**How this must actually be measured**, following the exact methodology
`docs/plan-1.8.7-terrain.md` and the CHANGELOG's own 3.09x figure already used:

1. **Host first.** Instrument the new pre-pass with a call counter (regions scanned,
   regions rejected at each stage, walks completed, cells tested) and a `clock()`-based
   per-column timer, exactly the shape `worldgen_density.c`'s own measured comments already
   use. Run it across dozens of seeds, not one — §1.4's 0.542 ms figure and the connectivity
   sweep in `worldgen.h:181-189` both used a seed set for exactly this reason: a single seed
   cannot be trusted to represent the design.
2. **Only then, ARM11.** The project's standing, repeated caveat applies unchanged: no
   hardware run has happened since v1.2.5, and a host x86-64 number is not a console
   number — an ARM11 timing claim needs `LOAD_STAGE_GENERATE` read off a real device
   (`plan-1.8.7-terrain.md:148, :374`), which this plan cannot produce and does not
   pretend to.
3. **The rejection rate is the number to watch first.** If the host measurement shows the
   two early-out rejects are not actually discarding the ~85–95% of scanned regions §2.2's
   design assumes, that is the signal to tighten `max_reach` (shrinking `R` and the region
   count directly, per §2.2's table) rather than to add more machinery on top of a leaky
   reject.

---

## 5. Build order for v1.8.11

Ordered so each step is independently host-testable before the next depends on it:

1. **`GEN_VERSION_CAVES`** — the new version constant and its gate (§2.6), landed first,
   before any carve code exists to gate. Nothing downstream is safe to build without this
   in place first — the v1.8.3 lesson is that retrofitting a gate after code already
   exists is exactly how a hash gets moved by accident.
2. **`WorldGenScratch` additions** — the carve mask and walker-state fields (§4.1), sized
   and measured with `arm-none-eabi-size` on the real target immediately, not estimated on
   paper.
3. **The fixed-point sine table** (§2.3) — new, small, host-testable in complete isolation
   (a lookup table has no dependency on worldgen at all).
4. **The walk-and-carve primitive alone** — the damped-drift walk, the tapered radius, the
   ellipsoid carve-stamp — built and unit-tested against a fixed seed with no neighbourhood
   scan around it yet: does one system carve a plausible connected tube, clipped correctly
   at an arbitrary column boundary the way `treePut()` already is? This is testable on host
   with no console involvement.
5. **Branches and rooms** (§2.4) — the same primitive, called recursively or once-only.
6. **The per-region hash and the neighbourhood scan** (§2.1, §2.2), wired to call step 4
   for every region that step's own gate hash selects, with the two-level early-out.
   **This is the step §4.2's cost estimate needs a host measurement taken against.**
7. **Wire the scan into `wgdColumn()`** as a pre-pass producing the carve mask, consumed
   by the existing downward fill loop in place of `worldgenIsCaveCached()`, gated on
   `GEN_VERSION_CAVES` (§3.1). The legacy fill loop is untouched.
8. **Lava** (§3.2) — the fixed-Y branch inside the carve-stamp write, plus the new
   `BLOCK_LAVA` id appended per `block.h`'s own discipline.
9. **Water, column-local only** (§3.3) — extending the existing `sea[]` overhang case to
   also see carved cells from the new mask, no cross-column work.
10. **Ravines** (§2.5) — the second per-region draw and preset, folded into the existing
    scan from step 6, with its own wider `R_ravine` where warranted.
11. **Determinism and two-lane suite coverage** (§2.7) — forward/reverse/interleaved
    column-order host hashing, plus a two-pthread run in `tests/lanes_test.c`'s own
    methodology. **This must land before the feature is considered done**, not as an
    afterthought — it is the exact check that would have caught the pre-v1.8.7 file-static
    corruption on day one had it existed then.
12. **Host cost measurement** (§4.2) — instrumented counters and `clock()` timing across a
    real seed sweep, reported the same way the CHANGELOG's 3.09x/2.20x figures were, before
    any cost claim is made about this feature. Console timing remains a standing gap this
    plan cannot close.

---

## 6. Explicit deferrals

Named here so they are decisions, not omissions:

- **Ore placement** (v1.8.12 per `ROADMAP.md:334-340`) — cleanly separable per §3.4;
  deliberately not folded into this version.
- **Lava "lakes"** above the fixed floor (§3.2) — the sourced frequency is itself
  inconsistent across the versions `caves-legacy-console.md` §4/§10 found, and it is a
  second rare per-region draw this plan does not add until the fixed-floor version has
  shipped and been measured.
- **True cross-column water-flood connectivity** (§3.3) — a materially larger feature in
  real tension with the "nothing persists between calls" architecture this whole design
  depends on. May never be worth its own cost; not scoped here either way.
- **Cave entrances** (breaching the surface) — see §7's risk 7 and §8. This is a genuine
  design fork, not an engineering deferral, and is called out separately below.
- **Mob spawning in caves** — per `caves-legacy-console.md` §7/§8 Phase 5, this is an
  entirely separate subsystem (no entity/AI/combat framework exists anywhere in this
  codebase today) and is explicitly out of scope for a cave-*generation* plan.

---

## 7. Risks, and the cheapest mitigation for each

1. **Neighbourhood cost stalls generation on the ARM11.** Cheapest mitigation: keep
   `max_reach` conservative to start (16–24 blocks, §2.2's table), measure on host before
   widening, and treat a low rejection rate in the host counters (§4.2 point 3) as the
   signal to tighten further rather than to add machinery.
2. **A random walk's actual drift exceeds the `max_reach` the neighbourhood radius assumed**,
   silently missing carve cells right at the search boundary and leaving an abrupt cut in a
   tunnel. Cheapest mitigation: enforce `max_reach` as a hard clamp on cumulative
   displacement *inside* the walk itself (§2.3), making the geometric guarantee exact
   rather than statistical, and add a host check asserting every carved cell of every
   walked system is within `max_reach` of its region's start.
3. **Version-gating mistake reshapes an existing world's underground** — the exact failure
   `genversion.h` and the v1.8.3 incident both exist to prevent. Cheapest mitigation: the
   `GEN_VERSION_CAVES` gate (§2.6) plus a `testWorldgenLegacySandyWideSweep()`-shaped host
   check, both landed in step 1 of the build order, before any carve code exists to need
   gating retroactively.
4. **A file-static regression reintroduces the pre-v1.8.7 corruption** (24/32 columns
   wrong, 159/192 refused, §1.3). Cheapest mitigation: every byte of new state in
   `WorldGenScratch` from the first commit, plus the two-lane host test in step 11 of the
   build order, reusing `tests/lanes_test.c`'s existing harness rather than inventing a new
   one.
5. **The `WorldGenScratch` growth (§4.1) is bigger in practice than the paper estimate.**
   Cheapest mitigation: `arm-none-eabi-size` on the real target the moment the new fields
   exist (build-order step 2), the same discipline `worldgen_density.c`'s own comment
   already insists on after being burned by a wrong hand-computed figure once.
6. **Ravines' longer reach inflates the shared neighbourhood radius for every column**,
   even the ones with no ravine nearby. Cheapest mitigation: a separate, wider `R_ravine`
   used only for the ravine draw (§2.5), affordable specifically because ravines' own
   rarity (~1-in-50) rejects nearly all of that wider neighbourhood for one hash call each.
7. **Cave entrances are a design fork, not an engineering detail, and this document does
   not decide it** — see §8.

---

## 8. Open decisions — not this document's to make

Per this project's standing rule that a design fork gets asked about rather than decided
by whoever is implementing:

**Should the new carver be allowed to breach the surface?** `GEN_CAVE_MIN_DEPTH` (§1)
exists specifically to guarantee no carved cell can ever float a grass block or open under
a spawn point, and the stated price has always been "no cave entrances — the way in is to
dig" (`worldgen.h:196-206`). The ROADMAP's v1.8.11 wording does not ask for entrances, but
walking into a cave rather than only digging into one is a recognisable part of the
"legacy feel" this version is chasing. Keeping the depth floor for the new carver as well
is the conservative default this plan assumes throughout (§2.3's carve stamp has no
surface-breach logic in it); allowing breaches would need new handling for the surface
material pass and the floating-block edge cases the depth floor currently makes impossible
by construction, and is exactly the kind of scope change that needs an explicit yes before
any code is written toward it.

**How wide should `max_reach` and `R_ravine` actually be**, once §4.2's host measurement
exists? §2.2 and §2.5 give a starting point and the arithmetic to reason about widening it,
but the final numbers are a feel-and-cost tradeoff to be made after playtesting a real
build, not a constant this document can responsibly fix in advance.
