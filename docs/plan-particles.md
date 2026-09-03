# Particles — where they're worth it, and where they're not

Scope note: this document designs a **general-purpose particle system** —
one shared quad-batch renderer that several effects spawn into — not a
single bespoke effect. Water splash is the one named explicitly in the ask;
this document also weighs block-break, rain/snow, underwater bubbles,
footsteps, and torch/furnace smoke against it, and says plainly which of
those earn their cost and which don't.

This document was produced without touching `source/`, `tests/`, `tools/`,
or the Makefile.

## Provenance key

- **[measured]** — a real tool was run and this is its real output.
- **[codebase]** — read directly out of a file in this tree; cited `file:line`.
- **[research]** — an external source; cited by URL, with its reliability noted.
- **[proposal]** — a design choice made in this document, not a fact.

---

## 0. Starting point — this is greenfield, and someone already scoped half of it

**[codebase]** `source/world/weather.h:308`: "There is no particle system
anywhere in this project (checked: zero hits for 'particle' across `source/`
and `tools/`)." Confirmed independently this session by the same search —
still zero hits. Nothing here is extending an existing system; it's
designing the first one.

`weather.h:303-333` — the weather model's own author already scoped "the
visible half" (rendering rain/snow) and left it explicitly unbuilt, sizing it
at "roughly a dozen new files — a vertex format, a shader, a texture
generator, a render pass with its own GPU state save/restore." That estimate
was for a **rain-specific** pipeline. §3's design below is smaller than that
because it is not rain-specific: one shared particle renderer, and rain/snow
becomes just one more emitter into it alongside splash and block-break,
rather than its own dedicated vertex format and shader. This document is the
continuation of `weather.h`'s own note, not a parallel, competing design —
it uses the exact query surface that note recommends
(`weatherAt(g, tick, x, z)`, asked "at the player's own position... a few
times a second — not per particle, not per frame," `weather.h:317-319`).

---

## 1. The memory budget this actually has to fit — corrected

The task that produced this document pointed at `world/budget.h`'s 12 MB
`WORLD_BUDGET_BYTES` cap as the figure to check particle costs against.
**That is the wrong budget**, and it matters enough to correct before any
byte count below is quoted.

**[codebase]** `world/budget.h`'s cap governs one thing: the calloc'd
application-heap world store (`Column`/`Chunk`/`LightColumn`,
`world/world.c:90,123`). A particle vertex buffer is not that. It has to be
read by the GPU via DMA, exactly like the chunk mesh pool
(`chunk_render.c:1142,1181`) and the sprite UI batch
(`gfx/sprite.c:79-80`) — both `linearAlloc`'d, neither anywhere near
`budget.h`'s cap. The relevant ceiling for a particle buffer is **linear-heap
headroom**, a separate and much tighter pool than the 12 MB figure, and — at
high render distance — the pool with almost nothing spare in it.

**[codebase, measured off the shipped constants]** `scene/render_dist.h`:

```
radius 3:  mesh pool  9,199,616 B + overhead 1,874,944 B = 11,074,560 B   (render_dist.h:145,255)
radius 5:  mesh pool 46,948,352 B + overhead 1,874,944 B = 48,823,296 B   (render_dist.h:147)
RENDER_DIST_LINEAR_HEAP_OLD = 33,554,432 B  (32 MB)   (render_dist.h:196)
RENDER_DIST_LINEAR_HEAP_NEW = 67,108,864 B  (64 MB)   (render_dist.h:197)
```

Free linear heap after the mesh pool and overhead:

- **Old 3DS, radius 3 (its only option):** 33,554,432 − 11,074,560 =
  **22,479,872 B (~21.44 MB) free.** Cross-checked against
  `app/heapsplit.h:6-7`'s own independently-quoted figure for a real CXI
  boot ("22,479,872 free after boot") — the two numbers agree.
- **New 3DS, radius 5 (its max):** 67,108,864 − 48,823,296 =
  **18,285,568 B (~17.44 MB) free.**

Both figures dwarf anything a particle pool needs (§3's sizing comes to
tens of KB). **The linear heap is not the constraint** — it was never going
to be, once the right pool is checked — so nothing in this document is
gated on memory. What follows is gated on draw calls, spawn-hook
availability, and honest design judgement about which effects are worth
looking at, not on bytes.

---

## 2. Vertex format and renderer shape

**[proposal]** A `ParticleVertex` identical in *shape* to the existing
`SpriteVertex` — `float x, y, z; float u, v; uint32_t rgba;`, 24 bytes
(`gfx/sprite.c:8-16` for the pattern; not proposing to reuse that struct
directly, since sprite.c is a screen-space UI batch and particles are
world-space geometry — a new, identically-shaped struct in its own module
keeps those concerns separate the way `crackoverlay.c` keeps its own
`CrackVertex` separate from both). All-float-plus-`uint32_t` is trivially
4-byte aligned on every field with no packing decisions to get wrong — the
same property that made `SpriteVertex` and `CrackVertex` safe on real
hardware from the start, in contrast to `MeshVertex`'s 8-byte packed layout,
which is exactly what froze the console from v1.1.0 to v1.2.4
(`world/mesh_vertex.h`, `_Static_assert` at line ~63, and the long account of
the bug in `chunk_render.c:844-864`). A particle format has no reason to
take on that risk for a buffer this small.

**Module shape — modelled directly on `scene/crackoverlay.c`, not
`gfx/sprite.c`.** Both exist in this tree, and they demonstrate two
different disciplines:

- `crackoverlay.c` is the better structural template: its own shader
  program (`crackOverlayInit`, `crackoverlay.c:140-170`), its own
  `linearAlloc`'d vertex buffer, `GSPGPU_FlushDataCache` before every use
  (`:166`), a draw function taking the frame's `view` matrix
  (`crackOverlayDraw(const C3D_Mtx* view, ...)`, `:186`) and explicitly
  owning only the GPU state it needs to (program, attributes, TEV stage 0,
  fog-off, alpha test) while deliberately inheriting the rest (depth state,
  per its own comment at `:265-269`) — and, critically, putting back only
  the one piece of state (`C3D_AlphaTest(false, ...)`, `:287`) that would
  otherwise leak into whatever draws next.
- `gfx/sprite.c` is the better template for the **per-frame vertex-write**
  pattern: quads are written dynamically into a pre-allocated pool every
  frame (`spriteBegin`/quad-push calls around `sprite.c:242-257`), unlike
  `crackoverlay.c`'s bake-once-at-init cube table — because particles move
  and die every frame, the way sprite quads change every frame and crack
  cubes never do.

**[proposal]**: a new `source/scene/particles.c` combining both —
`crackoverlay.c`'s init/exit/draw shape and per-pass GPU-state discipline,
`sprite.c`'s per-frame dynamic vertex-write pattern. Called from the same
per-frame 3D draw sequence `main.c` already runs `chunkRenderDraw`,
`highlightDraw`, and `crackOverlayDraw` from
(`main.c:2362,2390,2407`) — a fourth call, `particlesDraw(view)`, added to
that same block, in that same style.

**Draw call cost**: one more draw call per frame (when any particles are
live), same category as `crackOverlayDraw`'s one call. Not free, but not a
new category of cost either — the project already pays this shape of cost
for the crack overlay and the block-selection highlight every frame the
respective feature is active.

**Pool sizing [proposal]**: a single fixed-capacity pool, no per-type
sub-pools, since all particle types share one vertex format and one draw
call. 512 live particles × 4 verts × 24 bytes = 49,152 bytes, plus indices
(512 quads × 6 × 2 bytes = 6,144 bytes) — **55,296 bytes total**, against
17.44–21.44 MB free (§1). This is a starting number, not a measured one —
it is sized the same way `sprite.c:47`'s `SPRITE_MAX_QUADS = 1024` was, by
picking a round cap well clear of any plausible simultaneous count (a full
splash plus a block-break plus a screen of rain, at once, is well under 512
quads), not by a frame-budget calculation this project's tooling cannot
perform (§4).

---

## 3. Per-effect design

### 3a. Water splash — the one named explicitly. Ship it.

**Hook, with an exact citation, and it costs zero new state.**
`scene/player.c:62`: `const BodyWet wet = bodyWetUpdate(w, &p->body);`. The
`Body` struct already stores last frame's answer at `Body.wet`
(`world/physics.h:229`, and the comment at `:206-208` confirms it exists
specifically so this kind of transition can be read: "`wet` is last frame's
answer... what makes the wet test hysteretic"). So the transition check
needs no new field: read `p->body.wet` **before** the call at
`player.c:62` overwrites it, compare against the `wet` the call returns —
`BODY_DRY → BODY_SURFACE`/`BODY_SUBMERGED` is an entry splash,
the reverse is an exit splash.

**Intensity for free, too.** `Body.vy` (`physics.h:200`) is the vertical
velocity at the moment of entry — already computed, already available at
that exact call site, before `bodyStep` (`player.c:96`) changes it for the
water branch. Scaling splash particle count/spread by `|vy|` at the instant
of the transition needs no new physics.

**Cost**: 0 new registers, 0 new textures. Spawns a handful of quads
(8-16 [proposal]) from the pool in §2, coloured by a plain flat tint (no
atlas sampling needed — a splash doesn't need to look like anything but a
pale, fading dot), positioned around the player's feet
(`Body.x, Body.z`, water surface `y`).

**Recommendation: ship this. It was the one named explicitly, it has a
zero-new-state hook, and it is the cheapest of everything considered here.**

### 3b. Block break — strong candidate. Ship it.

**Hook**: `scene/interact.c:173`, `if (worldSet(w, x, y, z, BLOCK_AIR))` —
already has the broken block's `BlockId` (`breakComplete`'s `broken`
parameter, `interact.c:163`) and its position, at the exact point the break
is committed. `worldSet` has already applied the removal client-side by
this point (optimistic local update — the same thing the block's own visual
disappearance already relies on), so spawning particles here is consistent
with how the rest of this function already treats that moment as final,
not a new consistency risk introduced by this design.

**Zero new art — reuses the existing atlas, exactly what "nothing borrowed"
requires.** `world/block.h:350`, `blockFaceTex(BlockId id, int face)`,
returns the atlas tile index for a block's face. `world/atlas_uv.h:167`,
`atlasRect(int tile)`, turns that into the tile's pixel/slot bounds on the
existing 16×1024 atlas strip (`ATLAS_W_PX`/`ATLAS_H_PX`/`TILE_PX`,
`atlas_uv.h:48-50`). Debris quads sample small random sub-rects **within**
that 16×16-texel tile — the same "chip a fleck off the block's own texture"
trick Minecraft's own break particles use — needing no new texture, no new
`tools/` generator script, and no new procedural-art code anywhere.
`world/rng.h:37,45`'s `rngHash2`/`rngHash3` (positional, deterministic) or
`rngNext` (stateful, `rng.h:79`) cover the randomness for which sub-rect and
which scatter direction, with no new PRNG needed either.

**Cost**: reuses `atlasBind()`'s existing unit-0 binding (no new texture
unit spent — this is drawn in the *same* particle pass as everything else
in §2, sampling the world atlas that's already resident). A handful of
quads (6-10 [proposal]) per break, short-lived (under a second).

**Recommendation: ship this.** Cheapest-to-implement candidate after
splash — one clean hook, zero new assets, and it is the most immediately
visible piece of "juice" a player would notice missing once splash exists.

### 3c. Rain / snow — candidate, gated correctly by what already exists.

**Query pattern is already specified, not invented here.** `weather.h:317-319`
already states the recommended integration: call `weatherAt(g, tick, x, z)`
at the player's position "a few times a second — not per particle, not per
frame." **[proposal]**: decimate at roughly the same 2 Hz
`TICK_FAR_PERIOD` already establishes for other per-tick systems
(`world/tick.h:47`, `TICK_HZ / TICK_FAR_PERIOD == 2`) — reusing an existing
cadence convention rather than inventing a new one.

**Indoor gating is not optional, and a hook already exists for it.**
Spawning rain particles that visibly fall through a roof is exactly the
kind of bug this project's own "visual claims need a look at the actual
pixels" habit (`crackoverlay.c:290-296`'s closing note is one example of
that habit in practice) would catch immediately and embarrassingly. The
gate is cheap: `world/light.h:134`, `lightGetSky(const Column* col, int lx,
int y, int lz)`, already returns 0..15 sky exposure for any cell — a value
of 15 at/above the player's head means genuinely open to sky; anything
less means under cover and rain should not spawn there. `world/world.h:70`,
`worldColumn(const World* w, int cx, int cz)`, is the existing lookup that
gets from world coordinates to the `Column*` `lightGetSky` needs. No new
world query is required — both pieces already exist for other reasons
(the mesher's own light unpack, `world_dynamic.v.pica:209-216`, reads the
same sky/block split this would reuse).

**Cost**: a moving screen-relative or player-relative scatter of quads,
recycled continuously rather than spawned-and-forgotten like splash/break —
a genuinely different lifecycle shape from 3a/3b, closer to a steady-state
pool than a burst. Sizing this against §2's 512-particle pool needs an
explicit density decision (§5, HIS CALL) since "how much rain is visible at
once" is a look-and-feel call, not a technical one.

**Recommendation: candidate, not a slam-dunk.** Genuinely worth building —
the hooks are all there and cheap — but it is the one effect in this list
whose *density* (not its feasibility) is a judgement call this document
can't make (§5.2), and it is meaningfully more code than 3a/3b (a
continuous emitter with its own lifecycle, not a one-shot burst).

### 3d. Underwater bubbles — cheap add-on, riding 3a's machinery. Weak candidate.

**[proposal]** `BODY_SUBMERGED` already exists as a distinct state
(`physics.h:195`) from the same `bodyWetUpdate()` call 3a already hooks.
Spawning a slow trickle of bubble quads continuously while `wet ==
BODY_SUBMERGED` (rather than once on a transition, like splash) is a small
extension of 3a's existing hook, not a new one.

**Recommendation: only if 3a and 3c both ship and there's still appetite
left.** It adds real but modest value (submersion currently has almost no
visual feedback beyond a camera tint, if that), but it is asking the same
hook to do a second, different-shaped job (continuous vs. one-shot), and
this document was asked to say what to cut, not just what to add — this is
the first thing to cut if scope needs trimming, not because it's expensive,
but because it's the least load-bearing of the four candidates that have a
working hook at all.

### 3e. Footsteps — cut for this pass. No existing hook, and it's the most design-open item here.

A grep across `player.c`, `physics.h`, and the rest of `scene/` for
"footstep" found zero hits — unlike every effect in 3a-3d, this has **no
existing state to read a transition off of**. It would need genuinely new
logic: an on-ground velocity threshold, a distance-travelled accumulator to
pace individual spawns against footfall rather than continuous motion, and
— unlike break particles, which get their texture for free from
`blockFaceTex` — a *surface* look-up (what's underfoot) with its own
texture-selection question (dust on grass vs. a splash-like mark on wet
ground vs. nothing on stone) that block-break's debris trick doesn't need
to solve, since footsteps aren't destroying the block they're standing on.

**Recommendation: cut.** Every other candidate in this document rides a
hook or a table that already exists for some other reason. This one would
be the first wholly-new piece of gameplay-adjacent state built purely to
serve a particle effect, and it's also the one most likely to read as
noisy/annoying if the pacing is even slightly wrong (constant dust puffs
underfoot). Not "infeasible" — genuinely just the weakest cost/value ratio
of everything considered, and the only one needing a real design pass of
its own before implementation could start.

### 3f. Torch / furnace smoke — cut. The blocks don't exist yet.

**[codebase]** Neither `BLOCK_TORCH` nor `BLOCK_FURNACE` (nor anything
matching `torch`/`furnace` case-insensitively) appears anywhere in
`world/block.h`'s registry. Both are **planned, not built** —
`docs/plan-1.8.15-furnace.md` is a Planned-section entry in
`docs/VERSION-LIST.md`, and torches are discussed only in
`docs/plan-1.8.10-light.md`, itself also unbuilt as a placeable block.

**Recommendation: cut, and not a judgement call — there is nothing to hang
this on.** A smoke emitter needs a source position, and the only two
sources named in the brief don't exist as spawnable blocks in this codebase
today. This isn't "lower priority" the way footsteps is; it's simply not
buildable against the current block registry. Worth reconsidering once
either block ships, at which point it becomes a small, cheap add-on to §2's
existing renderer (a fixed emitter position instead of a transition-driven
one) — not worth designing further now.

---

## 4. What could not be measured — frame cost

Exactly the same gap as the shaders document, and for the same underlying
reason: **[codebase]** `debug/metrics.h:8-21` — Azahar reports a constant
0.25 ms for `C3D_GetDrawingTime()`, so real GPU time is unmeasurable under
this project's emulator. `docs/VERSION-LIST.md:308-309` — the most recently
released version states plainly that nothing in it has run on real
hardware. `scene/render_dist.h:431-432` independently confirms the same
gap from a different angle: New 3DS's render-distance default is pinned at
2, not 3, explicitly because — quoting the code comment directly — no
frame-time reading from a real New 3DS at radius 3 exists yet to justify
moving it.

> **⚠ CORRECTION [2026-09-03]** — this paragraph was already imprecise when written and is now
> also stale. It misquotes `render_dist.h`: the comment never said "pinned at 2, not 3" — it
> said the default deliberately did not move when the *ceiling* was raised. More importantly,
> as of commit `bc6ddbd` the New 3DS default is no longer 2: `RENDER_DIST_DEFAULT_NEW` in
> `source/scene/render_dist.h` moved to 3, and no frame-time reading from a real New 3DS has
> been taken in the meantime either — the move was justified instead by the New 3DS already
> paying for the radius-5 mesh pool at boot regardless of the setting (see that constant's
> comment for the full argument). The underlying point this section makes — that GPU cost on
> this project cannot be measured off real hardware — still stands.

**Consequence**: "how many particles fit in the frame budget" cannot be
answered in milliseconds here. §2's 512-particle pool cap and each effect's
per-spawn quad count are sized by comparison to existing, shipped costs of
the same shape (one more draw call, comparable to `crackOverlayDraw`; a
vertex buffer two orders of magnitude smaller than the mesh pool) — not by
a frame-time calculation this project's tooling cannot perform. A real
console frame-time reading, once one exists for anything, is the honest way
to confirm this fits; nothing in this document substitutes for that.

---

## 5. Does this need to touch the world budget, saves, or the network? No — and here's why, precisely

- **Not part of the world's memory budget.** §1 already establishes the
  buffer lives in the linear heap, not the `budget.h`-governed application
  heap. It is also not persisted anywhere the world store's budget
  accounting would ever see it.
- **Not saved to disk.** Particles are transient, camera-relative visual
  state with no gameplay effect — indistinguishable in principle from the
  crack overlay (`crackoverlay.c`) or the block-selection highlight
  (`highlight.c`), neither of which is saved, both of which simply don't
  exist between frames that don't need them.
- **Not networked, and for a reason grounded in how this codebase already
  treats client-only cosmetics, not just asserted here.** `world/weather.h`
  itself is the precedent: it's a pure function of `(seed, tick, x, z)`
  with **no weather-owned network state at all**
  (`weather.h:58-67`'s own "Weather needs no packet of its own" reasoning,
  cited in `docs/plan-1.8.9-weather.md:58-67`) — because two clients that
  agree on `tick` automatically agree on everything weather derives from
  it, with nothing to broadcast. Particles are a strictly weaker case than
  weather: they don't even need cross-client agreement, since a splash or a
  block-break spark is a local reaction to a local event each client
  already knows about independently (its own player entering water, a
  block it just watched get broken) — there's no state here for a packet
  to carry that the receiving client doesn't already have.

---

## 6. HIS CALL

1. **Which of 3a-3d actually ship, beyond the mandatory 3a.** This document
   recommends 3a (splash) + 3b (break) as the clear pair, treats 3c (rain/
   snow) as a real candidate gated on the density question below, and 3d
   (bubbles) as the first cut if scope needs trimming. That's a
   recommendation, not a decision — how much "juice" this release wants is
   a judgement call.
2. **Rain/snow particle density** (§3c) — how many quads on screen at once
   reads as "atmospheric" versus "a wall of noise obscuring the view,"
   especially given this hardware's fill-rate is already a known constraint
   elsewhere in the tree (the whole reason the distance-fog ramp exists,
   `world_dynamic.v.pica:38-49`). This is a look-and-feel call this
   document cannot make from a design pass alone.
3. **Whether footsteps (3e) get built at all**, given it is the one effect
   here that needs genuinely new state rather than riding an existing hook.
   Recommended cut for this pass; reversible if the value is judged worth
   the extra design surface it needs (surface-lookup, pacing).
4. **Whether to revisit torch/furnace smoke (3f)** once either block ships
   — not a call to make now, since neither exists yet, but worth flagging
   so it isn't silently forgotten once `plan-1.8.15-furnace.md` or the
   torch half of `plan-1.8.10-light.md` land.

## 7. LINES SOMEONE ELSE MUST ADD

None of these were touched by this document — write access was `docs/`
only.

- **New files**: `source/scene/particles.h` / `particles.c` — the module
  described in §2 (`particlesInit`/`particlesExit`/`particlesDraw`/
  `particlesSpawn*`-style API, following `crackoverlay.c`'s shape).
- **New shader**: `source/shaders/particle.v.pica` — a small vertex shader
  (position × view/projection, pass through UV and vertex colour, no
  faceShade/AO/tint machinery needed — closer to `crack.v.pica`'s
  simplicity than `world_dynamic.v.pica`'s).
- `source/main.c` — a `particlesDraw(view)` call added to the same 3D
  draw-call sequence as `chunkRenderDraw` (`main.c:2362`), `highlightDraw`
  (`:2390`), and `crackOverlayDraw` (`:2407`); a `particlesInit()`/
  `particlesExit()` pair alongside wherever `crackOverlayInit`/
  `crackOverlayExit` are already called for session start/end.
- `source/scene/player.c:62` — the splash-trigger check (§3a): capture
  `p->body.wet` immediately before this line, compare it against the `wet`
  this line assigns, and call the new splash-spawn function on a
  transition, using `p->body.vy` (read before `bodyStep` at `:96` changes
  it) for intensity.
- `source/scene/interact.c:173` — the break-particle spawn call (§3b),
  inside the `if (worldSet(...))` block, using `broken` (already in scope,
  `interact.c:163`'s parameter) with `blockFaceTex`/`atlasRect` for the
  debris texture.
- Wherever the per-frame weather query is added for rendering (§3c) — a new
  call site, at roughly 2 Hz per `tick.h:47`'s existing decimation
  constant, to `weatherAt(g, tick, playerX, playerZ)` gated by
  `lightGetSky(worldColumn(w, cx, cz), lx, y, lz)` (`light.h:134`,
  `world.h:70`) before spawning any rain/snow particle. This document does
  not know the exact per-frame update function this belongs in and did not
  go looking for it, since that's implementation wiring, not design.
- If 3d (bubbles) is approved — extend the same `player.c:62` check to also
  spawn continuously (not just on transition) while
  `wet == BODY_SUBMERGED`.

## 8. What could not be verified

- **Real frame cost, on either console** — §4. No honest millisecond figure
  exists for any of this on current tooling.
- **What any of this actually looks like.** No screenshot exists or could
  be produced from a docs-only pass with no `source/` write access. The
  density question in §6.2 in particular cannot be settled by reasoning
  alone — it needs a real build and a real look.
- **Whether 512 particles is the right pool cap** (§2). It's sized by
  comparison to existing costs of the same shape, not derived from a
  frame-time measurement this project's tooling cannot produce (§4). It
  should be treated as a starting number to tune against a real build, not
  a final one.
