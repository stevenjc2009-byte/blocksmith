# Optimisation scan — 2026-09-03 (lane SCAN-A)

Read-only audit. No `.c`/`.h` file was edited to produce this document; no `make` was
run. Every candidate below is a proposal for a *later* lane to implement, price again on
real hardware, and verify per `docs/plan-*` convention — not a change that has been made.

## Method

This is a large tree (95,593 lines of C at last count) and the ask covers RAM, CPU, GPU
and nine named systems, so the read was split five ways and run in parallel: one pass
done directly in this thread (main loop, render-distance/mesh-pool sizing, New3DS
core/clock plumbing, the RSF manifest, and a handful of the named systems), and four
delegated sub-scans, each handed an explicit read allowlist and the off-limits list from
the brief, each instructed to trace every candidate to a real caller before proposing it
and to price cost honestly. Findings credited to a sub-scan below carry the file:line the
sub-scan quoted; I did not re-open every one of those files myself, but each was produced
by a direct read with the literal source line quoted back, not a guess. Where I re-derived
or independently cross-checked a sub-scan's finding, it says so.

**The headline result of the scan itself:** this codebase is unusually well-instrumented
against exactly this kind of audit already. Chunk draw-call count, texture binding, cull
caching, greedy meshing, chunk sort order, the render-distance/mesh-pool split between
consoles, and the New 3DS clock/L2/second-core story are all *already* built, measured,
and — in most cases — carry an in-file paper trail citing the measurement that justified
them (see the Appendix). Two optimisations proposed and reverted earlier this run
(caching `worldgenHeight()` in `treeInCell()`, and a hash table for `chunk_render.c`'s
`findSlot`) are the shape of most of what a naive pass over this tree would still find:
technically real, but smaller than the noise floor or not hot enough to matter. This scan
tries not to repeat that mistake, which is also why the candidate list below is short —
most systems audited came back "nothing found here," and that is reported as such rather
than padded out.

---

## Top 5 — do these (ranked, best value/cost first)

| # | Candidate | Where | Verdict |
|---|-----------|-------|---------|
| 1 | Scale the per-frame mesh/relight drain budget on New 3DS | `source/main.c:253-254`, `:1985-1986` | **Worth doing — needs a hardware measurement before shipping** |
| 2 | Drop the duplicate per-frame forward-vector trig | `source/main.c:5696-5703`, `source/scene/interact.c:555-560` | **Worth doing — free, zero risk** |
| 3 | Give New 3DS a 3rd worker save slot, *if* measurement shows it's ever starved | `source/app/worker.c` `SAVE_SLOTS` | **Marginal — measure first, don't implement blind** |
| 4 | Grow the region cache on New 3DS, *if* measurement shows reopen churn | `source/world/region.c` `REGION_CACHE_ENTS` | **Marginal — measure first, the file already flags this on itself** |
| 5 | Instrument `cave_carve.c`/`ore_gen.c`'s per-column neighbourhood mask build for cross-column redundancy | `source/world/cave_carve.c:282-303`, `source/world/ore_gen.c:174-190` | **Not actionable yet — no candidate without a measurement first** |

Only #1 and #2 are things I'd actually schedule. #3-5 are "worth a five-minute
instrumentation pass to find out," not "worth changing" — see each entry below for why.
Two more candidates were found and are ranked **not worth it**; they're listed after the
top 5 for completeness, per the brief's "rank the whole list" instruction.

---

## Candidate 1 — mesh/relight drain budget is identical on Old and New 3DS

**WHERE.** `source/main.c:253-254`:
```c
#define DRAIN_BUDGET_MS   4.0f
#define DRAIN_MAX_CHUNKS  3
```
and the loading-screen variant at `source/main.c:1985-1986`:
```c
#define LOADING_DRAIN_BUDGET_MS  (2.0f * DRAIN_BUDGET_MS)
#define LOADING_DRAIN_MAX_CHUNKS (2 * DRAIN_MAX_CHUNKS)
```
Neither is behind `hwIsNew3ds()`. Every other per-console budget in this codebase (render
distance ceiling/default, entity cap, animal cap, audio pool size, worker core/lane count)
*is* gated this way — this pair is the one load-bearing frame-budget constant that isn't.

**WHAT is inefficient.** `main.c:235-254`'s own comment derives these numbers entirely
from steady-state timings that predate any New 3DS involvement: "steady-state CPU work
measured 0.62 ms... a chunk build costs 917 us to mesh... plus 0.63 ms visibility flood
fill... 3 chunks, plus the one chunk each drain is always allowed to complete, is ~6.2 ms
... 6.2 + 0.62 + 0.37 = 7.19 of 16.71ms" — a budget sized to fit inside a 16.71 ms frame
at whatever clock those measurements were taken at (the project's own convention is that
nothing here has been measured on real hardware yet — see `render_dist.h`'s repeated "NOT
PROVEN... this machine has no GPU/hardware to measure on" notes). A New 3DS requests
804 MHz + L2 cache at boot (`source/app/hw.c:41-52`, `osSetSpeedupEnable(true)`) — roughly
3x the clock of the 268 MHz Old 3DS default the budget was reasoned against, confirmed
again in the manifest (`cia/blocksmith.rsf:128-129`, `CpuSpeed: 804MHz`,
`EnableL2Cache: true`). The identical relight-drain budget four lines below
(`main.c:274-289`, `RELIGHTQ` clock/count caps) has the same shape.

**HOW HOT.** Per frame, unconditionally, whenever the world is generating or the player
is editing (`main.c:5989-6002`, called from inside the `while (aptMainLoop())` loop at
`main.c:5053`; the loading-screen variant runs from `main.c:2044` inside the boot/loading
loop at `main.c:2012`). This is about as hot as a path gets in this codebase — it runs
every single frame the world is live, and it is the mechanism that determines how many
chunks stream in and get meshed per frame, which is directly "pop-in" and "chunk loading
speed," two of the systems named in the brief.

**PROPOSED CHANGE.** Gate `DRAIN_BUDGET_MS`/`DRAIN_MAX_CHUNKS` (and their loading-screen
doubles) on `hwIsNew3ds()`, the same shape every other per-console constant in this file
already uses — e.g. `#define DRAIN_MAX_CHUNKS (hwIsNew3ds() ? 5 : 3)` or a small lookup
struct read once at boot. The actual multiplier needs a real New 3DS capture of
`chunkRenderBuild`/mesh-drain cost before it's picked, per this project's own standard —
this document is not proposing a number, only the seam.

**ESTIMATED WIN.** *Reasoned, medium confidence, not measured.* Memory-bound work (a
flood fill, a mesh build touching non-sequential chunk cells) does not get the full 3x
from a clock bump alone, but L2 cache being on for the first time on this model plausibly
recovers a meaningful chunk of that gap for exactly this kind of pointer-chasing work. No
number is claimed here because none exists yet — this is exactly the gap `render_dist.h`
itself already flags ("Whether an Old 3DS holds 60 fps... is a hardware question," and
the New 3DS radius-5 ring is offered opt-in for the identical reason). The GPU/rendering
sub-scan, working from `chunk_render.c`/`mesher.c` alone and without `main.c` in its read
list, independently flagged the same gap from the other direction: "No explicit 'N chunks
meshed per frame' cap exists inside `chunk_render.c` or `mesher.c`... whatever throttles
chunks-meshed-per-frame is most likely in `main.c`'s frame loop" — which is exactly
`DRAIN_MAX_CHUNKS`, found independently by two different reads.

**ESTIMATED COST.** Nearly free. This is two compile-time constants becoming a boot-time
branch on a flag the game already computes once (`hwIsNew3ds()`); no new .bss, no new
buffer, no algorithm change — the mesh/relight pipeline itself is untouched, only how much
of it runs per frame. The only real cost is the one every N3DS-only change in this project
pays: it cannot be verified without an actual New 3DS, and a wrong multiplier risks a
dropped frame (a worse regression than the pop-in it's trying to fix) — so this needs the
same on-hardware capture-and-compare discipline `render_dist.h`'s own history shows was
used for the render-distance ceiling.

**VERDICT — worth doing**, gated on getting a real New 3DS frame-time capture first (the
same gate this project already applies to every other New 3DS-only number). This is the
single biggest lever this scan found for the stated #1 priority ("New 3DS must actually
use the extra RAM, CPU speed and L2 cache effectively") and for two of the explicitly
named systems (pop-in, chunk loading speed) — everything else the New 3DS already gets
more of (render distance, entity count, audio pool, a second worldgen lane) is a *ceiling*
or a *pool size*; this is the one constant that controls how fast the extra compute New
3DS already has actually turns into world on screen.

---

## Candidate 2 — duplicate per-frame forward-vector trig

**WHERE.** `source/main.c:5696-5703` (the animal raycast's forward vector) and
`source/scene/interact.c:555-560` inside `interactAim`, called immediately after at
`main.c:5708`. (Credit: CPU sub-scan, quoted lines verified against the file.)

**WHAT.** Both blocks independently recompute the same forward vector from the same
`player.cam.pitch`/`yaw`, unchanged between the two calls — `cosf(pitch)`, `sinf(yaw)`,
`sinf(pitch)`, `cosf(yaw)`, 4 transcendental calls done twice where 4 would do. This is
the same class of fix `player.c` already applied to its own walk-basis trig (the CPU
sub-scan found a dated 2026-09-03 comment there describing exactly that reduction from 4
calls to 2), so this is finishing a cleanup the codebase has already started elsewhere,
not introducing a new pattern.

**HOW HOT.** Per frame, unconditional whenever `!paused` (the animal raycast is gated on
`!paused` at `main.c:5696`; `interactAim` runs at `main.c:5708` even while paused, so the
duplication is real on every frame animals are ticking).

**PROPOSED CHANGE.** Compute the forward vector once in `main.c` ahead of both call
sites and pass it into both, or add a version of `interactAim` that takes the
already-computed vector instead of recomputing it.

**ESTIMATED WIN.** *Reasoned, low confidence.* ARM11 has no hardware trig; a handful of
extra `sinf`/`cosf` calls is on the order of microseconds — call it 0.01-0.03% of a
16.71 ms frame. Almost certainly below the measurement noise floor this project has
already run into once this session (2772% on the `treeInCell` cache attempt).

**ESTIMATED COST.** Pure rewrite of existing arithmetic — no new table, buffer, or .bss,
a trivial signature change, low test risk.

**VERDICT — worth doing, opportunistically.** The win is real but tiny and likely
unmeasurable on its own; the reason to still do it is that it costs literally nothing and
removes a real duplicate the codebase has already fixed once elsewhere. Bundle it with
whatever lane next touches `main.c`'s aim/raycast block rather than spending a dedicated
pass on it.

---

## Candidate 3 — `SAVE_SLOTS` fixed at 2 regardless of console

**WHERE.** `source/app/worker.c`, `#define SAVE_SLOTS 2`; `static SaveSlot
s_save[SAVE_SLOTS]` where each slot is `{int32_t cx, cz; uint32_t len; uint8_t
bytes[REGION_COL_MAX]}` — about 32,824 bytes/slot, ~65,648 bytes total, unconditional on
both consoles. (Credit: RAM sub-scan.)

**WHAT.** Old 3DS runs exactly 1 worldgen lane (`laneCountFor(hwIsNew3ds())`,
`worker.c:577`), so it structurally can never need more than 1 save slot in flight — the
second slot is headroom it will never use. New 3DS runs 2 lanes against exactly 2 slots —
zero spare margin if the main thread hasn't drained a slot before both lanes finish a
column in the same tick.

**HOW HOT / HOW BIG.** Static, .bss, identical on both consoles today. Whether New 3DS
ever actually contends for a save slot is not established by anything read in this scan —
no counter or log exists for "no free save slot" in what was read.

**PROPOSED CHANGE.** Do not touch this blind. Add a one-line counter at the save-submit
call site (or check whether `app/debugmenu.c`/metrics already has a hook that could carry
it), run a session on New 3DS with heavy simultaneous generation+save pressure, and see if
it's ever hit. Only if it is, add a malloc'd 3rd slot gated on `hwIsNew3ds()`, the same
pattern `worker.c` already uses for the New-3DS-only second lane itself.

**ESTIMATED WIN.** *Assumed* — entirely contingent on a measurement nobody has taken.

**ESTIMATED COST.** +32,824 bytes if implemented, New-3DS-only if gated correctly; touches
a save-submit hot path, real (if small) test risk.

**VERDICT — marginal, measure first.** Plausible but unproven; this is exactly the kind
of thing that could turn into another `findSlot`-shaped mistake (fixing a starvation case
that essentially never triggers) if built without the measurement first.

---

## Candidate 4 — `REGION_CACHE_ENTS` fixed at 2, and the file already flags this on itself

**WHERE.** `source/world/region.c`, `#define REGION_CACHE_ENTS 2`; `static
RegionCacheEnt s_cache[REGION_CACHE_ENTS]`, 12,656 bytes total (6,328 bytes/entry) per the
sub-scan's in-file measurement. (Credit: RAM sub-scan.)

**WHAT.** The RAM sub-scan reports the file's own comment states: *"On a console with
64 MB that is affordable and on a console with less it would not be"* — i.e. the code
already reasons about New-3DS-affordability at this exact spot but the array is still a
flat static shared by both consoles, not console-gated. No cache-miss/reopen rate is
quoted anywhere in the file to say whether raising it would even help.

**HOW HOT / HOW BIG.** 12,656 bytes .bss today on both consoles; +6,328 bytes/entry if
raised under the current shape.

**PROPOSED CHANGE.** Same as Candidate 3 — instrument the actual reopen/eviction rate
during a real streaming session before doing anything; if it's frequent, apply the same
base-N-plus-malloc'd-extra pattern `worker.c`'s second lane already established, gated on
`hwIsNew3ds()`.

**ESTIMATED WIN.** *Assumed* — no rate is quoted anywhere to size this against.

**ESTIMATED COST.** 6,328 bytes/entry, New-3DS-only if gated; moderate complexity, but a
pattern already proven elsewhere in this file's sibling module.

**VERDICT — marginal, measure first.** Not a new discovery so much as an already-flagged,
never-actioned tradeoff — worth a measurement pass, not worth a blind change.

---

## Candidate 5 — no cross-column cache in the newer cave/ore neighbourhood scans

**WHERE.** `source/world/cave_carve.c:282-303` (`caveCarveBuildMaskR`) and
`source/world/ore_gen.c:174-190` (`oreGenBuildMaskR`). (Credit: RAM sub-scan.)

**WHAT.** Both walk every region in a neighbourhood radius around the column being built,
on every single column build, with only two cheap early-outs (a region-chance hash, then a
reach-vs-column bounding box) as mitigation. This is notable specifically because the
*older* cave-noise pass in the same file family already has an equivalent cache
(`WorldGenCaveCache`) that the RAM sub-scan found was built to kill a measured 131x
cross-call redundancy — these two newer (v1.8.11/v1.8.12) passes never got the same
treatment.

**HOW HOT.** *Unmeasured.* No per-column timing comment exists for either function, unlike
`interpolateColumn`'s quoted 0.0329→0.0043 ms figure elsewhere in worldgen. This is exactly
the case the brief's rule about hotness applies to: it looks like the same class of
problem the project already solved once, but nobody has actually clocked it.

**PROPOSED CHANGE.** None proposed. This entry exists to flag it for instrumentation, not
to propose a fix — the honest move here is a timing pass (the same convention the rest of
worldgen already uses) before guessing at a cache design.

**ESTIMATED WIN.** Unknown — not established.

**ESTIMATED COST.** N/A, no change proposed.

**VERDICT — not actionable without measurement.** Flagged only because it's the one
generator pass with no cross-column reuse trick in a file that otherwise built exactly
that trick once already for the same shape of problem — a real candidate for a future
`treeInCell`-style measurement pass, not a change to make today.

---

## Found, and rejected — listed for completeness

**Double `slotBind()` per frame for chunks with mixed opaque+transparent geometry.**
`source/scene/chunk_render.c:2617` (`drawOpaque`) and `:2596` (`drawRun`), both calling
`slotBind()` at `:2471`, which re-derives the modelview matrix and re-inits `C3D_BufInfo`
once for the near-to-far opaque pass and again for the far-to-near transparent pass on any
chunk with both. (Credit: GPU sub-scan.) Reasoned win: small — a 3-float compose plus two
struct writes, dwarfed by the uniform upload and draw call that follow it. Cost: a
per-frame slot-cache with a dirty flag, correctness risk against the stereo cull-cache
path. **Verdict: not worth it** — same shape as the reverted `findSlot` hash table, more
code for a cost that's likely below noise and was never measured.

**`playermodel.c` issues 2 draw calls per remote player instead of 1.**
`source/scene/playermodel.c:317-323`, inside `playerModelDraw()` (`:213`) — a second
`C3D_TexEnvColor`+`C3D_DrawArrays` triple draws the nose in a fixed color because TEV
constant color is per-draw-call state, not per-vertex. (Credit: GPU sub-scan.) Bounded by
`networldRemoteCount()` (~15 max), each contributing one small extra draw. Fix would need
a vertex-format + shader change to bake the nose color into a per-vertex attribute.
**Verdict: not worth it** — cost (touching the vertex format and shader) clearly exceeds a
win bounded by a player count of 15.

---

## Area-by-area, as requested

### RAM — static allocations, .bss, worst-case pools, per-chunk/per-column memory

- The chunk mesh pool (the single biggest allocation in the renderer) **already does
  exactly the right thing** and is the model this scan measures everything else against.
  Since v1.8.5 (`source/scene/chunk_render.c:687-721`), `chunkRenderInit` allocates its
  linear-heap arena at *runtime*, sized by `poolSlotsForRadius(renderDistMaxFor(hwIsNew3ds()))`
  — not by the compile-time ceiling unconditionally. Concretely: 9.2 MB at radius 3 (both
  consoles today), scaling up to 46.9 MB at radius 5 on a New 3DS only, against linear
  heaps of 33.5 MB (Old) / 67.1 MB (New) read directly off a booted console
  (`source/scene/render_dist.h:131-153`). This is real, hardware-read evidence, not
  arithmetic, and it is the single strongest piece of "New 3DS actually gets more" this
  scan found.
- The one part of that same system that *is* identical on both consoles is the **.bss
  index/bookkeeping tables** — `static MeshSlot s_slots[MESH_SLOTS]`
  (`chunk_render.c:765`), sized from the compile-time `RENDER_DIST_MAX` (currently 5,
  `render_dist.h:122`), plus the matching `DirtyQ` in the same file
  (`source/world/dirtyq.h:33-75`, `+576` bytes). Both are deliberate, small, and already
  measured and written down: `render_dist.h:112-115` states the total cost of the New 3DS
  radius-5 lift as "+45,312 bytes, on both consoles" — a known, accepted, single-digit-KB
  tax on the Old 3DS to let the New 3DS index a wider ring. Not worth revisiting; it is
  already the cheapest part of a much larger, already-correct scaling story.
- `SAVE_SLOTS` (Candidate 3) and `REGION_CACHE_ENTS` (Candidate 4) are the two places this
  scan found something sized flat across both consoles where a console-aware size might
  actually help — both flagged as "measure first," not "change now."
- `source/world/world.c`'s `WORLD_BUDGET_BYTES` (12 MB world-data cap) is **not** split by
  console, and the RAM sub-scan found this is correct as-is: the in-file comment states
  the cap allocates nothing by itself, so a console split would save nothing.
- Nothing was found sized for Old 3DS that silently caps New 3DS below what the hardware
  could support — the opposite direction from what the brief worried about. The only
  place New 3DS is left short of its hardware is Candidate 1 (CPU time budget, not RAM).

### CPU — per-frame work, per-tick work, meshing, lighting, worldgen, entity ticks, collision

- The mesh/relight drain budget (Candidate 1) is the one real, actionable finding.
- Entity/animal ticking is **already distance-decimated**
  (`tickPeriodForDistSq`/`tickDue` in `source/entity/entity.c`, per the CPU sub-scan) — far
  entities tick at a fraction of the near rate; no "full AI for every entity every frame"
  pattern was found.
- `worldGet`/`worldColumn` are O(1) hash-table lookups, not a linear scan
  (`source/world/world.c`, per the CPU sub-scan) — ruling out a suspicion that
  `interact.c`'s held-break `worldGet` calls could be a per-frame linear-scan cost.
- `physics.c`'s AABB sweep is a bounded few-cell loop, not O(n²); no redundant
  `sqrt`/trig/`expf` calls were found there.
- Worldgen itself (`worldgen.c`, `worldgen_density.c`, `cave_carve.c`, `ore_gen.c`,
  `noise.c`, 3,900+ lines read in full by the RAM sub-scan) is, in its own words, "the most
  heavily pre-measured code" read in this whole audit: a cave-noise corner cache that
  already killed a measured 131x redundancy, salt hoisting that removed 1,275 `rngMix`
  calls/column, a uniform-cell short-circuit measured at 0.0329→0.0043 ms/column (92% of
  lattice cells skipped), and cheap-reject-before-expensive-lookup ordering throughout. The
  one gap found (Candidate 5) is explicitly flagged as unmeasured, not a confirmed win.
- Crafting, inventory, mining, survival, and blockstate (`source/world/crafting.c`,
  `inventory.c`, `mining.c`, `survival.c`, `blockstate.c`, `dirtyq.c` — all read in full by
  the New3DS/crafting sub-scan) came back **clean**: every loop in this set is bounded by a
  small fixed count (≤64) and fires once per explicit player action (craft attempt, eat,
  block interaction), never per-frame or per-tick. `blockstate.c`'s own linear `findSlot`
  is the same shape as the already-reverted chunk_render.c hash-table experiment, and its
  in-file comment already argues against replacing it — not repeated here.

### GPU — draw calls, vertex counts, state changes, texture binds, overdraw

- Texture binding is already minimal: `atlasBind()` is called exactly once per frame/eye
  from `pipelineBind()`, not per chunk or per face (`source/gfx/atlas.c:65-68`,
  `source/scene/chunk_render.c:1190`, verified directly by the GPU sub-scan's read).
- Draw-call count per chunk is already reduced via face-bucket merging in `drawOpaque`
  (`chunk_render.c:2617`), documented in-file as averaging 1.75 draws instead of 2.5 for a
  chunk with 3 surviving face directions.
- Chunk draw order already uses a measured 4-pass LSD radix sort (dated 2026-09-03 in-file,
  0.2583 ms → 0.0089 ms worst case) rather than a naive sort.
- Stereo (3D slider) rendering already skips re-running frustum cull on the second eye when
  the view matrix is byte-identical to the first (`BS_CULLCACHE`).
- The mesher (`source/world/mesher.c`, 1,263 lines, read in full) already does packed
  cell-flags, precomputed per-cell face-exposure bitmasks, a word-scan skip over
  non-drawing cells, and greedy merging with a documented-and-rejected looser merge rule.
- Only two GPU-side candidates were found at all (the double `slotBind` and the
  playermodel double-draw, above), and both were rejected as not worth their cost.
- **Pop-in specifically**: the GPU sub-scan could not find an explicit "N chunks meshed
  per frame" cap inside `chunk_render.c`/`mesher.c` themselves and flagged this as a gap in
  its own read scope — this scan's own direct read of `main.c` closes that gap: the cap is
  `DRAIN_MAX_CHUNKS`/`DRAIN_BUDGET_MS` (Candidate 1), which is the mechanism actually
  governing pop-in and is also the one place this scan found real New-3DS headroom going
  unused.

### The named systems, one at a time

- **Camera** (`source/scene/camera.c`) — checked in full by the CPU sub-scan; the shipped
  build doesn't even run `cameraUpdate`'s trig (that's the `BS_FLY`-only path); the normal
  path's `cameraLook` does 2 unavoidable trig calls for look-integration. Nothing found.
- **Player model** (`source/scene/playermodel.c`, `entitymodel.c`) — one real finding, the
  rejected double-draw candidate above. Otherwise clean.
- **World loading** — see Candidate 1 (drain budget) and the RAM section's reasoning
  (region I/O and decode are already the cheap side; first-time generation is the more
  likely bottleneck, per the RAM sub-scan's read of `region.c`'s write-hint mechanism and
  `chunk_codec.c`'s form-dependent decode cost — flagged as *reasoned*, no end-to-end
  generation-vs-load timing was found in-tree to make it a measured claim).
- **Chunk rendering** — see the GPU section above; already close to the practical ceiling
  for this architecture without a structural rewrite (a single shared mega-vertex-buffer
  across chunks, which this scan is not proposing — the tiered per-slot pool exists
  specifically because chunk sizes vary and a single buffer would need indirection this
  GPU generation doesn't cheaply offer).
- **Pop-in** — Candidate 1.
- **Chunk loading speed** — Candidate 1, plus the already-good worker-lane parallelism
  described in the New 3DS section below.
- **Block placing / block breaking** — `source/scene/interact.c` checked in full by the
  CPU sub-scan: exactly one `worldGet` and one raycast per frame on the break/place path,
  no duplicate lookups found. `crackoverlay.c` and `highlight.c` (the break-progress and
  block-outline visuals) both bake their geometry once at session start
  (`crackoverlay.c:98-108`, `highlight.c:90-115` — read directly in this thread), not per
  frame; both already carry a comment explaining exactly that tradeoff.
- **Block crafting** — `source/world/crafting.c` checked in full by the New3DS/crafting
  sub-scan: nothing hot enough to matter (see CPU section).

### New 3DS: is the extra RAM/CPU/L2 actually used?

Checked directly in this thread (`source/app/hw.c`, `cia/blocksmith.rsf`) and
cross-confirmed by a dedicated sub-scan that grepped the whole tree for `hwIsNew3ds`,
`APT_SetAppCpuTimeLimit`, and related APIs, with a red-control grep (`threadCreate`,
known to exist, returned 8 hits) proving the search methodology before trusting any
zero-hit result.

**Verdict: mostly yes, with one real gap (Candidate 1).**

- **Clock + L2 cache**: `hwInit()` calls `osSetSpeedupEnable(true)` on a confirmed New 3DS
  (`source/app/hw.c:41-52`), requesting the 804 MHz clock and L2 cache together — this is
  the .3dsx/Homebrew Launcher path. The installed-CIA path is covered independently by the
  manifest itself: `cia/blocksmith.rsf:127-130` sets `CpuSpeed: 804MHz`,
  `EnableL2Cache: true`, and `CanAccessCore2: true` under the New3DS-exclusive process
  settings block, plus `SystemModeExt: 124MB` for the extended memory pool. Both launch
  paths are covered; this is correct and complete.
- **Extra RAM**: `SystemModeExt: 124MB` (`blocksmith.rsf:127`) grants the full New-3DS
  extended application memory. The mesh pool scaling described in the RAM section above is
  the clearest evidence this extra memory is actually *used*, not just granted.
- **Extra core**: `AffinityMask: 1` in the manifest (`blocksmith.rsf:112`) permits core 0
  only for the *general* affinity grant, but core 2 is a separate New-3DS-exclusive
  exheader flag (`CanAccessCore2: true`, `blocksmith.rsf:130`) that isn't gated by
  `AffinityMask` at all — confirmed by `source/app/worker.h:118-121`'s own comment on
  exactly this distinction. `source/app/hw.c:5-12`'s `hwPreferredWorkerCore()` puts core 2
  first in the ladder on a New 3DS, and `source/app/worker.c:577` starts a **second,
  fully concurrent worldgen lane** on that core when it's granted — real additional
  compute throughput (a second staging `World`, its own worldgen scratch buffer, its own
  job claims), not just a bigger cap. This only fires when `s_lane0.core == 2`, so it never
  activates under a launch path where the grant was refused.
- **Core 1 (the system core)**: deliberately *not* used by default —
  `source/app/worker.c:32-40` (`BS_WORKER_CORE 0`, `WORKER_CPU_TIME_LIMIT 80`) leaves
  `APT_SetAppCpuTimeLimit` uncalled in a default build (`&&`-short-circuited at
  `worker.c:550`) because an in-file comment records it was tried and measured to make
  world-fill 2.5x *slower* from OS-service contention on the shared core. This is a
  measured rejection, not an oversight — correctly left alone.
- **The one gap**: nothing found scales *per-frame CPU budget* with the console — render
  distance, entity/animal caps, and audio pool are all *ceiling/size* decisions, and the
  second worldgen lane is the one thing that scales actual *background* compute. The
  *foreground*, per-frame mesh/relight drain that turns generated data into visible world
  is the one place a 3x clock and a fresh L2 cache go unused. That is exactly Candidate 1.

---

## What this scan explicitly did not verify

- Every "reasoned" and "assumed" win above is labeled as such because nothing in this scan
  ran on a device — the whole read was static analysis against source and comments, per
  the brief's read-only constraint. Candidate 1 in particular needs a real New 3DS capture
  (the same kind `render_dist.h`'s own history describes taking for the render-distance
  ceiling) before any multiplier is chosen or shipped.
- Two areas were flagged as gaps in a sub-scan's own read scope rather than resolved: the
  GPU sub-scan could not find the per-frame mesh-build cap inside its own file list (closed
  above, by this document, once `main.c` is brought into scope); the RAM sub-scan could not
  establish an actual reopen/eviction rate for `REGION_CACHE_ENTS` or a starvation rate for
  `SAVE_SLOTS` from static reading alone (Candidates 3 and 4 — flagged for instrumentation,
  not resolved).
- `source/world/furnace.c`/`.h`, `source/world/registry.c`, `source/gfx/atlas_tiles.h`,
  `tools/make_atlas.py`, `gfx/atlas.png`, and the two `docs/plan-1.8.1{8,9}-*.md` files
  were off-limits for this scan (other lanes own them) and are not covered by anything
  above — a future scan pass would need to cover storage/furnace and the interface work
  once those lanes land.

## Appendix — already excellent, don't re-propose these

Recorded so a future pass doesn't waste time rediscovering what this one already checked:
single-bind texture atlas, face-bucket draw merging, the 2026-09-03 radix chunk sort,
stereo cull-result caching, greedy meshing with a measured merge-aggressiveness cutoff, the
runtime (not compile-time-unconditional) mesh-pool allocation split by console since
v1.8.5, the cave-noise corner cache, worldgen salt hoisting, the uniform-cell density
short-circuit, distance-decimated entity ticking, O(1) world-block lookup, and the
New-3DS clock/L2/core-2/second-lane story described above. All of these were the kind of
thing this scan was sent to look for, and all of them are already done.
