# v1.8.7 — Terrain: implementation spec

This is the detailed version of `docs/ROADMAP.md`'s v1.8.7 entry, which was itself rewritten
2026-09-01 from measurement after the old entry turned out to describe work that had already
shipped. It builds on `docs/research/terrain-beta-1.7.3.md` (the b173 research brief) and does
not redo that research. Where this spec and the brief agree, the brief is cited rather than
repeated; where a number in this spec could not be reconciled against a number elsewhere in the
tree, that is said plainly rather than picked silently (see §5).

This document does not implement anything. It is not itself a change to any file under
`source/`.

---

## 0. What "v1.8.7 — Terrain" actually is, stated once so the rest of this spec is not read
##    against the old framing

The old v1.8.7 entry said this version adds Beta 1.7.3-style terrain. It does not, because that
terrain already exists and already ships to every new world:

- `world/genversion.h:104` stamps every new world `GEN_VERSION_FOR_NEW_WORLDS`, which is
  `GEN_VERSION_BIOME` (3) (`genversion.h:101`). The density field is reached at
  `version >= GEN_VERSION_DENSITY` (2). The density generator shipped in v1.7.0, biome identity
  in v1.8.3 — both before this version.
- The lattice is 5 x 5 x 17 at 4 blocks horizontally / 8 vertically
  (`world/worldgen_density.h:64-67`), matching the historically-documented Beta grid
  (research brief A3).
- The world is 128 blocks tall with sea level 64 (`world/world.h:23-24`,
  `world/worldgen.h:39-43`), matching Beta's own height and sea-level fraction.
- Overhangs are already occurring and were measured, not hoped for: at the current amplitude
  table, one test seed reaches a +16.76-block upward step across one 8-block lattice cell, with
  9.99% of all lattice steps rising (`world/worldgen_density.c:130-141`).

So this version is **not** a rewrite. Per `docs/ROADMAP.md:210` ("What this version is instead:
making it good on every seed, not on two") and the research brief's own Part C verdict
(`docs/research/terrain-beta-1.7.3.md:459-466`), the two things actually open are:

1. The amplitude table's overhang behaviour was validated on exactly two seeds
   (4242, 90210). Whether it holds across a real distribution of seeds is unmeasured.
2. Every cost figure for the density generator is host x86-64. No ARM11 reading exists anywhere
   in the tree for either console model.

This spec covers those two, in the order the research brief itself recommends
(`terrain-beta-1.7.3.md:459-461`): the timing reading first, because it is cheap and answers a
question that blocks trusting anything downstream of it; the seed sweep second, because it is
slower and its outcome (retune or no retune) is not yet known.

World height is explicitly **not** touched. 128 already matches Beta, amplitude — not height —
is what the code's own measurement identifies as the overhang lever
(`worldgen_density.c:133-141`), and a taller world has a real, quantified memory cost against
the 12 MB world-store cap (`world/budget.h:75`, derivation at `:24-68`; `docs/ROADMAP.md:214-243`).

That memory figure was itself corrected in `docs/ROADMAP.md` while this spec was being written,
and the corrected numbers are the ones to use: one loaded column is **65,648 B** at 128 blocks —
48 B `Column` + 32,832 B of chunks (8 x an 8-byte header plus 4,096 cells) + a 32,768 B
`LightColumn` — compiled for ARM and read off the emitted constants (`world/budget.h:24-41`), not
derived on paper. The number this spec's own task brief quotes (65,648 bytes) already matches
this corrected figure, not the retracted one. At 192 blocks the same arithmetic gives ~98,464 B
per column, and a 17x17-column worst case (290 columns including the worker's one staging
column) reaches **~28,554,560 B against the 12 MB cap — about 2.27x over**, not the 1.16x an
earlier draft of that section implied before its own correction. The conclusion this spec relies
on is unchanged by the correction — 192 does not fit, by a wide margin — so nothing else in this
spec needed to change. Nothing in this spec revisits the height decision.

---

## 1. Success criterion

Two claims, because one is provable without hardware and one is not — conflating them is the
mistake this spec is written to avoid.

**A. Measurable, host-only.** A seed-sweep report exists (§4 Phase 2) covering at least 50 seeds
(25x the existing sample of 2) and states, per amplitude band of the biome table
(`worldgen_density.c:146-151`), the fraction of sampled lattice steps that rise and the largest
single upward step observed. The report shows the two high-amplitude bands (plains/hills, amp
48; mountains, amp 64) producing a positive overhang rate on **every** seed sampled, not just the
one seed currently on record — i.e. the failure mode the *old* amplitude table had (maximum
upward step negative on both test seeds, `worldgen_density.c:137-138`) does not recur anywhere in
the wider sample for those two bands. The two low-amplitude bands (lowland, amp 14; coastal
plain, amp 28) staying at or near 0% is not a failure — it is the design intent
(`worldgen_density.c:100-104`) — and the report must say so rather than reading a flat lowland as
a bug.

**B. Visual, playtest-only.** Standing in a location the sweep identifies as high-amplitude
(mountains band) on a booted console or in Azahar shows, by eye: at least one ledge with a
visible gap underneath it (an overhang), or one place where you can walk under an outcropping of
solid ground. Standing in a location the sweep identifies as the lowland band shows none. This is
the claim that only a screenshot or a live camera can settle — see §7's note on why a passing host
test cannot stand in for it.

Both must hold. A green sweep report with no visual check is unverified per the project's own
rule that a visual claim needs a look, not a diff read (see §7).

---

## 2. What Beta 1.7.3 terrain is worth reproducing, and at what scale

Drawn from the research brief (`terrain-beta-1.7.3.md`, Part A), restated here only as far as
it bears on what to check for in §1 and §4:

- **Overhangs, arches and floating pieces come from a 3D field crossing zero more than once
  above the same (x, z)**, not from a separate carve (brief A1). Blocksmith's density field
  already has this property by construction (`worldgen_density.h:8-18`); the open question is
  whether it is happening *enough* and *consistently*, not whether the mechanism exists.
- **A coarse lattice with trilinear interpolation**, not per-block noise: Beta's documented grid
  is 5 x 5 horizontally, 17 vertically, at 4 and 8 blocks per cell (brief A3). Blocksmith's is
  identical (`worldgen_density.h:64-67`). No change proposed here.
- **A vertical bias term** that pulls density solid near the floor and empty near the ceiling,
  so the world is not a uniform sponge (brief A4). Blocksmith's is a flat one
  density-block-per-world-block slope (`worldgen_density.c:126-141`). No change proposed here.
- **Amplitude, not world height, governs how much relief and how many overhangs a region gets**
  (brief A2's `depthNoiseScaleX/Z` family; Blocksmith's own biome amplitude table, same role).
  This is the one lever this spec's Phase 2/3 actually touches.
- **Scale, quoted from the brief for reference**: Beta's world was also 128 blocks tall, sea
  level 64 (brief A2). Blocksmith matches both already (§0).

Nothing in this section calls for new noise fields, a denser lattice, or a taller world. The
research brief's own C2 finding is explicit that a denser lattice "buys smoother density
transitions at a steep, direct cost... with no evidence it is needed" — this spec does not
revisit that either.

---

## 3. Current state, honestly, with file:line evidence

- **Generator dispatch**: `world/worldgen.c:264-266` (`worldgenHeight`) and `worldgen.c:792-804`
  (`worldgenColumn`) branch on `g->version >= GEN_VERSION_DENSITY`. A `GEN_VERSION_LEGACY` world
  never reaches any file this spec discusses.
- **The density function**: `world/worldgen_density.c:207-238` (`densityCore`) — two limit
  fields (`lo`, `hi`), a selector (`selectorAt`, `:190-201`) that blends them, and the vertical
  bias term, exactly the shape brief A1 describes. This is implemented, shipped, and this spec
  proposes no change to it.
- **The biome-to-relief table**: `world/worldgen_density.c:146-151` (`s_biome_table`), four
  control points, continuous interpolation between them via `wgdBiomeParams`
  (`worldgen_density.c:153-184`). This IS the open lever (§0 item 1).
- **The overhang measurement on record**: `worldgen_density.c:130-141`'s comment, covering
  exactly two seeds (4242, 90210), stating the old table produced no overhang anywhere and the
  current table produces one on the mountainous seed at +16.76 blocks / 9.99% of lattice steps.
  There is no test file anywhere in the tree that reproduces or automates this measurement — it
  was a one-off host probe whose output was pasted into the comment. Confirmed by search: no
  match for the "16.76" / "9.99" figures outside that comment, `docs/ROADMAP.md`, and the
  research brief, which both quote the same comment rather than an independent run.
- **Timing figures on record**: `app/worker.h:10-13` and `docs/ROADMAP.md:219-224` — legacy
  0.757-0.804 ms/column, density 1.055-1.310 ms/column, both **host x86-64, gcc -O2, not ARM11**,
  stated as a limitation in the source itself. No file anywhere in the tree contains an ARM11
  timing figure for either generator. This is the gap Phase 1 closes.
- **The timing instrument already exists and needs no new code.** `debug/loadprof.h:72`
  (`LOAD_STAGE_GENERATE`) already brackets `worldgenColumn`/`wgdColumn` on the real device path
  (`app/worker.c:274`), converts ticks to microseconds via `loadprofTicksPerUs()`
  (268.111856 ticks/us on console, `loadprof.h:96-99`), and is already written to
  `sdmc:/blocksmith/load.csv` by `loadprofWrite` — a file that already exists in the shipped
  build. Phase 1 is a *reading*, not an instrument to build.
- **The shape is not the gap.** Every structural claim the old v1.8.7 entry made — 3D field,
  coarse lattice, biome-driven amplitude, Beta's own height and sea level — is already true and
  already cited above with file:line evidence. What is open is validation breadth (one table,
  two seeds) and one missing hardware measurement, nothing algorithmic.

---

## 4. Phases

Each phase is independently testable and none but Phase 3 requires touching `worldgen_density.c`
or `genversion.h` at all. Phase 3 only runs if Phase 2's evidence says it must.

### Phase 1 — ARM11 timing reading

**Outcome.** A real console figure for `wgdColumn`'s per-column cost exists in the tree,
alongside the existing host figures, on at least an Old 3DS (New 3DS is a should-have, not a
must-have — see §6 of the project's own rule that both consoles must work, and §8 below for what
to cut if only one console is reachable).

**What to do.** No code change. Build and run the existing shipped instrumentation:

1. Boot the game on hardware (or, failing that, report the block per this project's own rule —
   see §7's note on why this specific claim cannot be faked with an emulator reading).
2. Start a fresh world at the shipped default render distance so `JOB_GENERATE` runs enough
   columns to be a real sample, not a handful.
3. Read `sdmc:/blocksmith/load.csv` off the card afterwards (or the on-screen debug readout at
   `main.c:2473`, which already prints `workerBusyMs()` per frame) and pull the
   `LOAD_STAGE_GENERATE` row: total ticks, call count. Convert with `loadprofTicksPerUs()`
   (268.111856 on console) to ms/column.
4. Record the figure in `docs/ROADMAP.md`'s v1.8.7 entry and in `app/worker.h`'s own comment
   block (`worker.h:10-13` already carries the host figures; this is where the console one goes),
   alongside the seed/radius/console-model it was taken under.

**Proof.** The reading itself — a number, with the console model, radius and seed it was taken
under written beside it. This is not a claim reasoning can settle; see §7.

### Phase 2 — Seed-sweep amplitude audit (host-only, no hardware needed)

**Outcome.** A report, run and read rather than assumed, stating per-amplitude-band overhang
statistics across at least 50 seeds instead of 2.

**What to build.** A new test function in `source/world/world_test.c` — not a new file, so no
change to `tools/run_host_tests.sh` is needed; that file already compiles and links
`world/worldgen_density.c` and `world/genversion.c` into the `world_test` binary
(`tools/run_host_tests.sh:270-273`).

The function should:

1. For each of >=50 seeds (a fixed, checked-in list, not `rand()`, so the report is
   reproducible), and for a grid of (x, z) at `GEN_D_CELL_XZ` (4-block) spacing over an area at
   least as large as the one the existing two-seed measurement used (`worldgen_density.c:136`
   says "16,384 steps per seed" — match or exceed that per-seed sample size), for each of the 16
   vertical lattice cells (`y = k * GEN_D_CELL_Y`, `k = 0..15`):
   - Call `wgdDensityAt(g, x, y, z)` and `wgdDensityAt(g, x, y + GEN_D_CELL_Y, z)` (both public,
     `worldgen_density.h:285`) and take the difference in blocks (`>> FX_SHIFT`). This is the
     exact quantity the two-seed comment measured — a positive value is the overhang
     precondition (`worldgen_density.c:133-136`).
   - Resolve `amp` for that (x, z) via `wgdBiomeParams(worldgenBiome(g, x, z), NULL, &amp)`
     (both public) and bucket the sample by which of the four table amplitudes (14, 28, 48, 64)
     it is nearest to, **not** by `BiomeId`. This matters: the relief table is keyed on the raw
     `worldgenBiome()` scalar directly (`worldgen_density.c:70-151`), which is a different axis
     from `BiomeId` (`worldgen.h:289-296`, a temperature x rainfall classification where
     temperature is `FX_ONE - worldgenBiome()`, `worldgen.h:298`). A named biome like desert or
     tundra can occur at any amplitude band; bucketing by `BiomeId` would silently answer a
     different question than the one the amplitude table asks.
2. Per bucket, report: sample count, % of steps with a positive delta, and the single largest
   delta observed. Print it (the existing test files' convention — see `world_test.c`'s output
   style) so a run of `tools/run_host_tests.sh` leaves the table in the console output, not only
   in a return code.
3. Assert nothing that fails the build on its own for the amp-14/28 buckets staying near 0% —
   that is expected (§1, §2) — but do assert (fail loud) if the amp-48 or amp-64 bucket ever
   shows 0% positive steps for an individual seed, since that is the exact failure mode the old
   table had and is what this phase exists to catch.

**Proof.** Running `tools/run_host_tests.sh` and reading the printed table. This can be done in
this environment or any host with the existing toolchain — no hardware, no GPU, matching the
project's stated constraint. The check is built so it *can* fail: feed it the *old* amplitude
values (10/18/34/58, quoted in `worldgen_density.c:137`) as a sanity run first and confirm the
assertion trips — a check that cannot go red proves nothing (this is a standing lesson recorded
elsewhere in this project's own history and worth applying here explicitly).

### Phase 3 — Conditional retune and version gate (only if Phase 2's evidence says so)

**This phase is gated on Phase 2's result, and gated on the project owner's decision before any
code is touched.** Do not start it speculatively.

**Trigger.** Phase 2's report shows the amp-48 or amp-64 bucket producing zero (or the owner's
judgment call: too low) overhang rate on some non-trivial fraction of the 50+ seeds. If it does
not — if the current table already clears every seed — this phase is a no-op, and that no-op is
itself worth recording (in `docs/ROADMAP.md` and the vault) as the validated result, not left
unstated.

**The one fact this spec insists on, because getting it wrong breaks other people's worlds.**
`s_biome_table` (`worldgen_density.c:146-151`) belongs to `GEN_VERSION_BIOME` (3), which is
**already shipped and already the default for new worlds** (§0). `world/genversion.h`'s own
design rule (`genversion.h:1-54`) exists for exactly this situation: *"Replace [the generator]
and every world that already exists comes back a different shape underneath the player's
buildings... on an update nobody opted into."* The file even records a real incident of this
happening — biome identity nearly landed as an in-place change to `GEN_VERSION_DENSITY`, a host
probe caught seven of twelve pinned column hashes moving, and it was minted as `GEN_VERSION_BIOME`
instead (`genversion.h`, the "SEVEN OF THE TWELVE HASHES MOVED" paragraph). **Editing
`s_biome_table`'s numbers in place would repeat exactly that mistake against version 3, this
time for real, on every world made since v1.8.3.**

**What to do, if triggered:**

1. Mint `GEN_VERSION_4` in `world/genversion.h`, following the existing pattern
   (`GEN_VERSION_BIOME`'s own comment block is the template): append-only, never renumber.
2. Gate the new/retuned amplitude values behind `version >= GEN_VERSION_4`, leaving
   `s_biome_table`'s current four rows reachable byte-for-bit unchanged for
   `version == GEN_VERSION_BIOME`. The cleanest shape, matching how `wgdColumn` already branches
   on `g->version >= GEN_VERSION_BIOME` for the biome-vs-no-biome split
   (`worldgen_density.c:564`), is a second table selected the same way — not a single table
   mutated in place.
3. Only after that gate exists does `GEN_VERSION_FOR_NEW_WORLDS` move to `GEN_VERSION_4`
   (`genversion.h:104`).
4. Add a regression test in `world_test.c` that generates a fixed set of columns (reuse the
   precedent already on record: seeds 1337 / 4242 / 90210 crossed with columns (0,0) (1,0)
   (-1,-1) (7,-3), the same set `genversion.h`'s own history names) at `GEN_VERSION_BIOME` before
   and after this change, and asserts the hashes are identical. This is the same check that
   caught the version-2 mistake; running it here is how this phase proves it did not repeat it.

**Proof.** (a) The pinned-hash regression test in 4, green. (b) Phase 2's sweep re-run against
`GEN_VERSION_4`, showing the previously-failing bucket now clearing on every seed. (c) A
diff of `genversion.h` showing `GEN_VERSION_BIOME`'s own table and dispatch untouched — reviewed
by eye, not just by the test, since this is the one place in this spec where "the test passed" is
not automatically enough: see §7's note that a passing check that could not have failed proves
nothing, and the sanity-run in Phase 2 is what makes this check actually able to fail.

**Why this needs an explicit go-ahead rather than being done as part of "retune the table":**
minting a new generator version, and deciding what counts as an acceptable overhang rate, are
both judgment calls with no single correct number — exactly the kind of decision this project's
own working rules reserve for the owner rather than for whoever is implementing the spec.

### Phase 4 — Visual sign-off (playtest, not a host check)

**Outcome.** The visual claim in §1.B is actually looked at, not inferred from Phase 2's numbers.

**What to do.** Using a seed and coordinate pair Phase 2's report identifies as high-amplitude
(mountains bucket) and one it identifies as low-amplitude (lowland bucket), boot the game (real
console preferred; Azahar acceptable for this phase specifically, because the claim here is
about shape, not timing — see §7 for why the two phases have different hardware requirements)
and look. Screenshot both locations.

**Proof.** The screenshots themselves, described against §1.B's criteria: an overhang or
walkable void visible in the mountain location, none in the lowland one. This is the phase that
cannot be automated or reasoned about from the diff — it is the one this whole spec's §1
explicitly splits out as unprovable by host test alone.

---

## 5. Cost, in noise evaluations per column

**None of Phase 1, 2 or 4 changes `wgdColumn` at all.** Phase 1 reads an existing counter. Phase
2 is a new host test function that calls the *existing* `wgdDensityAt`/`wgdBiomeParams` — it adds
zero evaluations to the shipped column-generation path because it never runs on the console and
never touches `wgdColumn` itself. Phase 4 is a playtest. **The noise-evaluation cost of this
version, as specified, is the current shipped cost, unchanged**, unless Phase 3 triggers.

Phase 3, if triggered, adds a second lookup table selected by a version compare — a few integer
compares and an array index, not a noise evaluation, so it does not change the per-column
evaluation count either. What Phase 3 might change is which *values* come out of the lookup
(different `amp`), which does not change how many times `densityCore` or `noiseFbm3` run.

**So this spec's answer is: 0 additional noise evaluations, against any baseline.** That said,
the baseline itself has three figures on record in this tree, at three different scopes, and
they are not in tension — they are a component, its container, and the whole:

| Figure | Source | Scope |
|---|---|---|
| 2,975 `value3At` + 50 `value2At` | `worldgen_density.c:255-266` comment; research brief B3 | The density lattice alone: 425 points x 7 octaves, plus 25 xz points x 2 biome octaves. Does not include the cave carve, the tree pass, or anything else `wgdColumn` does. |
| 22,031 | `docs/ROADMAP.md:145-148` ("`wgdColumn` is **91%** of the cost of generating a column: 22,031 noise evaluations and 1.25 ms"); this is also this task's stated baseline | All of `wgdColumn` — the lattice above plus the cave carve, which `app/worker.h` identifies as the dominant per-column cost. |
| 24,204 | `docs/ROADMAP.md:128-131` ("Measured over 1,271 columns and 6 seeds") and restated at `:145-148` as "1.379 ms for everything" | The entire column-generation path: `wgdColumn` (91%, the 22,031 above) plus the tree pass (the other 9%). |

22,031 / 24,204 = 91.02%, matching the ROADMAP's own 91% claim exactly — 2,975 is a component
of 22,031, which is in turn a component of 24,204. There is nothing to reconcile here; an
earlier draft of this section flagged a "9% discrepancy" between 22,031 and 24,204 that does
not exist — that was this document's own error, not the tree's, and it has been removed. The
lesson worth keeping for the next reader: `docs/research/terrain-beta-1.7.3.md` made the mirror
mistake in the other direction (citing a dead `docs/ROADMAP.md:73-76` anchor and a since-retracted
figure as "corroboration" for the 2,975 number — corrected in that document, not this one, since
the research brief is not this spec's file to rewrite beyond noting it). Both mistakes had the
same cause: trusting a remembered or quoted figure instead of opening the citation and reading
what is actually on that line, in a tree where `docs/ROADMAP.md` and `world/budget.h` were both
rewritten the same day this spec was written.

**Confidence: high.** The three figures are the same measurement at three nested scopes, and the
91% relationship is arithmetic anyone can re-check, not a reading this spec is asking to be
trusted.

---

## 6. World compatibility

**Phases 1, 2 and 4: no effect, no risk.** None of them changes a single value any generator
dispatch reads. A world generated before, during or after these phases lands on identical
terrain, because no file `worldgenColumn` or `wgdColumn` depends on is touched.

**Phase 3, if triggered: existing worlds survive only if the version-gate step (§4 Phase 3, step
2) is done, and do not survive if it is skipped.** This is stated loudly because it is exactly
the mistake `genversion.h`'s own history records happening once already (§4 Phase 3). If someone
edits `s_biome_table`'s numbers directly instead of minting `GEN_VERSION_4` and gating behind it:

- Every world stamped `GEN_VERSION_BIOME` (3) — every world made from v1.8.3 to whenever this
  ships — regenerates any chunk it has not yet visited with the new amplitude values the moment
  the player walks there, silently reshaping unexplored terrain under a version number that is
  supposed to mean "this exact shape, forever."
- This is not hypothetical or this spec's caution alone — it is the documented near-miss in
  `genversion.h` itself, where the same mistake was caught by a hash-pinning test before it
  shipped, for the version-2-to-3 transition.

This spec's recommendation is unambiguous: **mint a new version, do not edit the shipped table.**
But whether Phase 3 fires at all is not this spec's call (§4 Phase 3's closing paragraph), and
that decision belongs to the project owner before any of Phase 3's code is written.

---

## 7. How each phase is proven, and which claims cannot be settled without a playtest

| Phase | Proof mechanism | Needs hardware? | Needs a look, not just a number? |
|---|---|---|---|
| 1 (ARM11 timing) | Reading `load.csv`'s `LOAD_STAGE_GENERATE` row off a real device | **Yes, real console.** No emulator — an emulator's CPU model is not the ARM11 this figure exists to characterise, and this project's own standing note is that no hardware run has happened since v1.2.5; faking this with an Azahar timing would produce a number that looks like a fix and is not one. | No — it is a logged number. |
| 2 (seed sweep) | `tools/run_host_tests.sh` output, plus the negative-control run against the old amplitude table | No — host-only by design. | No — it is a printed table, and the sanity check (old table must fail the assertion) is what makes the check able to go red at all. |
| 3 (retune, conditional) | Pinned-hash regression test (old table byte-identical) + Phase 2 re-run against the new table | No, for the test suite. | Recommended anyway before shipping — a retuned amplitude table changing what a mountain looks like is exactly the kind of change this project's own rule says needs a look, even though the hash test proves the *old* table is untouched. |
| 4 (visual sign-off) | Screenshots at sweep-identified coordinates | Real console preferred; Azahar acceptable **for this phase only**, because the claim is shape, not timing. | **Yes — this is the one claim in this whole spec that a passing test cannot substitute for.** No amount of Phase 2's numbers proves an overhang reads as an overhang on screen; only looking does. |

---

## 8. What to cut, in priority order, if this version turns out too expensive

1. **Cut Phase 3 entirely if Phase 2 comes back clean.** This is not really a cut so much as the
   expected outcome — the amplitude table was tuned by eye and got measured, not designed, to be
   wrong, and there is a real chance 50 seeds simply confirms 2 seeds' answer. If so, this
   version's remaining scope is Phase 1 and Phase 2 alone, both of which are cheap.
2. **Cut the New 3DS half of Phase 1 if only one console is reachable.** An Old 3DS reading
   alone still closes the "no ARM11 number anywhere" gap for the console that matters most (it is
   the tighter budget on every other axis in this project's history). A New 3DS reading is a
   should-have, not a blocker for calling Phase 1 done — but say explicitly, in whatever writeup
   follows, that the New 3DS figure is still open rather than letting an Old 3DS number stand in
   for both silently.
3. **Cut Phase 3's scope down to the specific failing bucket, not the whole table**, if the
   sweep shows only one band (say, mountains but not plains/hills) needs retuning. Touching
   fewer control points is less to regress-test and less for the pinned-hash suite to have to
   prove untouched.
4. **Do not cut Phase 4.** If time runs out, ship Phase 1-3 and mark Phase 4 as the outstanding
   playtest item rather than skip it and call the version done — per this project's own rule, an
   unverified visual claim is not a smaller version, it is an unfinished one.
5. **Do not cut the version-gate in Phase 3** to save time by editing the table in place "just
   this once." §6 states the cost of that plainly; it is not a corner this version can cut and
   still keep its own stated promise that existing worlds keep their terrain.

---

## 9. The single biggest risk, and what to do about it

**No ARM11 measurement of this generator exists anywhere, and it is not specific to this
version** — every new world any player starts today already runs on a generator whose real
console cost is unknown (research brief C5 item 1; `docs/ROADMAP.md:219-224` says the same).
Phase 1 exists specifically to close this, and it is placed first in this spec's phase order for
that reason: if the console reading comes back showing the density generator is meaningfully
worse than the 1.37-1.64x host ratio suggests — an in-order ARM11 core with no branch prediction
can shift that ratio in either direction relative to an out-of-order x86 core, and nothing in
this tree has tested which way — that is a finding that should reach the project owner before
Phase 2's seed sweep or Phase 3's retune spend any further effort on a generator whose
fundamental cost on an Old 3DS might need to be revisited first. The mitigation is exactly
Phase 1's ordering: cheap, first, and reported honestly whichever way it comes out.
