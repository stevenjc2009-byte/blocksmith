# Light and torches — research brief for Blocksmith v1.8.10 "Light"

Scope note: this is research only. **Nothing under `source/` was changed to produce this
file.** Every other file in the tree was read, not written — including
`source/shaders/world_dynamic.v.pica`, which already names this document (its v1.8.10
comment block, quoted in full in E2, ends with "See `docs/research/light-and-torches.md`").
That citation existed before this file did: the shader was written first, and this brief is
the record that backs it, not a green-field proposal written ahead of the code. Where the
already-shipped code and the research agree, that is stated as a confirmation, not
discovered as a coincidence. Where they do not — E2 and E5 both turn up one — it is flagged
as a discrepancy in the code, not corrected, because the only file this pass is scoped to
touch is this one.

Minecraft is used throughout as **visual and behavioural reference only.** No code and no
assets from it, or from any decompile of it, were copied into Blocksmith; every quoted line
below is quoted to document a *behaviour* to reproduce, the same way `sky-and-weather.md`
and `caves-legacy-console.md` already do for this project.

Provenance is marked throughout, matching `docs/research/sky-and-weather.md`'s convention:

- **[COMPUTED]** — a formula cited below, evaluated by hand for this brief.
- **[DERIVED]** — arithmetic on top of cited numbers, shown so it can be checked.
- **[INFERENCE]** — engineering judgement. Not sourced.
- **[NO SOURCE]** — looked for and not found. Named explicitly so it is never implemented as
  if it were a fact.
- Unmarked, in-repo claims are cited by `path:line` and can be checked directly against the
  tree; unmarked web claims carry an inline source link.

---

## The headline: two of six findings are already in the shipped shader

`source/shaders/world_dynamic.v.pica` was read in full for this brief. As of this pass it
already implements:

- **E1**, the vanilla light-to-brightness curve `b = l / (4 - 3l)` — shipped in v1.8.9,
  unchanged here.
- **E2**, vanilla's real sky+block combine rule — **additive, not `max()`** — landed in
  v1.8.10, the version this brief is written for, with the shader's own comment already
  stating the `max()` reading is a trap and pointing at this file for the citation.

The other four findings (E3–E6) are the research that was still owed when that shader
comment was written: light removal, smooth lighting's relationship to AO, what a torch
actually is, and what the PICA200's fixed-function hardware can fake cheaply. `BLOCK_TORCH`
does not exist yet — `grep -i torch source/world/block.h source/world/registry.c` returns
nothing outside test files and comments — so E5's job is to specify the content Blocksmith
is missing, not to review something already built.
`docs/plan-1.8.10-light.md` is the project's own feasibility pass for the whole v1.8.10
version (shaders option, torches, particles, water bob) and is the right document to read
alongside this one; this brief goes deeper on the lighting math and the web research behind
it, and does not repeat plan-1.8.10-light.md's read-in-code survey where that survey already
answered the question.

---

## E1. The light-to-brightness curve — settled in v1.8.9, unchanged by this version

**Source, verbatim**, from
[MCP-919 `WorldProvider.generateLightBrightnessTable`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java)
(fetched and confirmed this session):

```java
protected void generateLightBrightnessTable()
{
    float f = 0.0F;
    for (int i = 0; i <= 15; ++i)
    {
        float f1 = 1.0F - (float)i / 15.0F;
        this.lightBrightnessTable[i] = (1.0F - f1) / (f1 * 3.0F + 1.0F) * (1.0F - f) + f;
    }
}
```

With `f = 0` (the Overworld) and `l = i/15`, this reduces to `b = l / (4 - 3l)` — the same
reduction `docs/research/sky-and-weather.md` §A4 worked through for v1.8.9, and the same
number `source/shaders/world_dynamic.v.pica:110–124` already cites:

```
; v1.8.9. The vanilla light-to-brightness curve, b = l / (4 - 3l), read off the decompiled
; client (MCP-919 EntityRenderer.updateLightmap, cited in full in
; docs/research/sky-and-weather.md E3) and restated in docs/VERSION-LIST.md's v1.8.9 entry:
; "A linear light-to-brightness ramp would render full night 3.2x too bright."
```

**What Blocksmith should do: nothing — it already does this.** `curveConsts(4.0, -3.0, 1.5,
0.0)` in that same file is exactly `(4, -3)` from the formula above, plus a third constant
(`curveBlockMul`, 1.5) that belongs to E2, not E1.

One thing worth naming, because the shader's own comment names it and it is still true in
the v1.8.10 code below it: vanilla applies this curve to the sky and block channels
*separately, before they combine* (see E2's quote of `updateLightmap`). v1.8.9 applied it
once, to a single already-combined value, because v1.8.9 combined with `max()` and a concave
curve applied to a max is the same curve applied to whichever input the max picked. **That
justification stopped being exactly true the moment v1.8.10 switched to summing the two
channels** (E2) — a sum can exceed either input, and curving two values separately then
summing is not, in general, the same as summing then curving one value. The current shader
(`world_dynamic.v.pica:265–284`, quoted in full in E2) already does the *correct* thing —
two separate curve evaluations, `r5.z` for sky and `r5.y` for block, each through the same
three-instruction `mad`/`rcp`/`mul` sequence, summed afterward — so this is not a bug in the
code. It is a stale justification in the comment above `curveConsts` that describes the
v1.8.9-era reasoning and was not updated when the v1.8.10 code below it stopped needing it.
Flagged in Open Questions; out of scope for this file to fix.

---

## E2. Sky light and block light do not combine with `max()` — the trap, and why v1.8.10 sums them instead

This is the finding the shader already cites this document for, so it is documented in full.

### The wrong formula, and why it is easy to reach for

[Minecraft Wiki, "Light"](https://minecraft.wiki/w/Light) states, and this was fetched and
confirmed verbatim this session:

> "the larger value of the two main light types currently at a block's location for some
> computations. This is what is known as *client light*, and its value, displayed after the
> words 'Client Light' on the debug screen, is the number before the parentheses, which is
> calculated by the formula: `max(sky light, block light)`."

That sentence is correct, sourced, and describes a real, named quantity — **client light**,
the number the F3 debug screen prints and the number gameplay checks like hostile-mob-spawn
eligibility read. It is *not* a description of how the lightmap texture — the thing that
actually multiplies a rendered face's colour — is built. Reading `max(sky, block)` off this
page and wiring it into a shader's lightmap combine is the exact trap
`source/shaders/world_dynamic.v.pica`'s own v1.8.10 comment (quoted below) names, and it is
what v1.8.9's shader did — reasonably, since nothing had yet distinguished the two concepts
in this project's own research.

### The real formula the rendered lightmap uses

[MCP-919 `EntityRenderer.updateLightmap`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/client/renderer/EntityRenderer.java)
was fetched in full this session. The part that matters, verbatim:

```java
float f = world.getSunBrightness(1.0F);
float f1 = f * 0.95F + 0.05F;

for (int i = 0; i < 256; ++i)
{
    float f2 = world.provider.getLightBrightnessTable()[i / 16] * f1;
    float f3 = world.provider.getLightBrightnessTable()[i % 16] * (this.torchFlickerX * 0.1F + 1.5F);
    ...
    float f4 = f2 * (f * 0.65F + 0.35F);
    float f5 = f2 * (f * 0.65F + 0.35F);
    float f6 = f3 * ((f3 * 0.6F + 0.4F) * 0.6F + 0.4F);
    float f7 = f3 * (f3 * f3 * 0.6F + 0.4F);
    float f8 = f4 + f3;   // red
    float f9 = f5 + f6;   // green
    float f10 = f2 + f7;  // blue
    f8 = f8 * 0.96F + 0.03F;
    f9 = f9 * 0.96F + 0.03F;
    f10 = f10 * 0.96F + 0.03F;
    ...
}
```

`i / 16` is the sky nibble, `i % 16` the block nibble — the exact same `sky<<4 | block`
packing `source/world/mesher.c`'s `cornerLight()` writes into `MeshVertex.pad`. Three things
matter:

1. **It is a sum (`f8 = f4 + f3`), not a `max()`.** Sky and block each go through
   `generateLightBrightnessTable()` (E1) *independently*, are each scaled — sky by
   `sunBrightness`-derived `f1`, block by a **torch-flicker term**, `torchFlickerX * 0.1F +
   1.5F` — and then added.
2. **The per-channel weighting is not uniform — it is what makes torches read orange.** The
   sky term (`f4`, `f5`, and `f2` itself for blue) is the same value on all three channels,
   scaled only by how much daylight there is (`f*0.65+0.35`). The block term is *not*: red
   gets the raw `f3`, green gets `f3` run through one damping curve (`f6`), blue gets `f3`
   run through a steeper one (`f7`, involving `f3*f3`). Red rises fastest, blue rises
   slowest — that is the actual mechanism behind torchlight reading warm/orange while
   daylight reads neutral, and it is a genuinely separate design decision from "how bright is
   this pixel."
3. **The whole thing is then remapped**, `x * 0.96F + 0.03F`, before being clamped to
   `[0, 1]` — so no lit pixel, however dark, is ever driven to literal `0.0`, and the
   brightest is driven to `0.99` before the second, gamma-setting-dependent pass in the real
   method (elided above; it does not change the shape of the finding) can push it further.

**Client light and lightmap colour are different numbers describing different things, from
different code, and they do not have to agree.** A block sitting in a torch-lit doorway at
noon has `client light = max(15, 14) = 15`; its lightmap colour is `table[15]*sunBrightness +
table[14]*1.5`, which is *brighter* than either channel alone, because it is a sum. Reading
the wiki's `max()` sentence as "how bright the face renders" throws away exactly the case
that makes a torch look like a torch instead of a slightly-less-dark patch.

### What Blocksmith's shader does about it

`source/shaders/world_dynamic.v.pica:246–284`, read in full this session (the whole block is
quoted because it is short and it is the thing this brief exists to back):

```
; v1.8.10. The two light channels are CURVED SEPARATELY and then ADDED, replacing v1.8.9's
; single curve over max(daySky, block). Vanilla's lightmap is
;   table[sky] * sunBrightness  +  table[block] * (torchFlickerX * 0.1F + 1.5F)
; then * 0.96F + 0.03F, clamped to 1. It is a SUM. The `max(sky, block)` line the wiki is
; usually quoted for describes a different quantity — the "client light" the F3 readout and
; the mob-spawning checks use — and taking it for the lightmap is the trap this shader was
; in until now. See docs/research/light-and-torches.md.
...
mul r6.x, curveNegThree, r5.zzzz     ; -3 * skyLum
add r6.x, curveFour, r6.xxxx         ; 4 - 3*skyLum
rcp r6.y, r6.xxxx
mul r5.z, r5.zzzz, r6.yyyy           ; curved sky

mul r6.x, curveNegThree, r5.yyyy     ; -3 * blockLum
add r6.x, curveFour, r6.xxxx         ; 4 - 3*blockLum
rcp r6.y, r6.xxxx
mul r5.y, r5.yyyy, r6.yyyy           ; curved block
mul r5.y, curveBlockMul, r5.yyyy     ; x 1.5, vanilla's torch multiplier at rest

add r5.w, r5.zzzz, r5.yyyy           ; ADDITIVE — not max()
min r5.w, ones, r5.wwww
```

This is, instruction-for-instruction, the two-table-lookup-then-sum shape of
`updateLightmap` above, minus the final `*0.96F+0.03F` remap (Blocksmith clamps with a bare
`min(1, sum)`, no floor) and minus the per-channel red/green/blue weighting — the single
scalar `r5.w` this block produces is then applied *uniformly* to all three colour channels a
few lines later (`mul r11.xyz, r6.zzzz, r10` — one luminance, times the biome tint row,
times all of x/y/z alike). **What Blocksmith should do, stated as two separate, honestly
labelled deviations, not one:**

- **The additive combine itself: keep it exactly as shipped.** It is the behaviourally
  correct read of vanilla and it is already in the tree; this section exists to be its
  citation, not to propose a change.
- **The missing per-channel warm tint (`f6`/`f7`'s damping curves): a real, deliberate
  simplification, and a reasonable one to ship as-is.** **[INFERENCE]** Adding it would cost
  two more curved terms and two more constants inside an already-tight vertex uniform budget
  (`docs/plan-1.8.10-light.md` §0 measures 9 free registers in this shader today), for an
  effect — torchlight skewing warm rather than just bright — that is genuinely secondary to
  "a torch makes things brighter, and does not dim at night," which is what this shader
  already delivers correctly. If a torch reading as flat-white rather than warm-orange looks
  wrong in his playtest, the fix is adding the two extra curved terms to the existing
  `r5.y`/`r5.z` block above, not a redesign — but it is not something to build ahead of that
  playtest.
- **The missing `*0.96F+0.03F` floor/ceiling remap: leave it out, and say why.** Its whole
  purpose in vanilla is to keep the *darkest* lit pixel above literal black and the
  *brightest* below literal white, mostly so `1 - x` gamma-curve tricks later in the same
  method have room to work. Blocksmith has no equivalent later pass reading this value, so
  the remap would cost two more instructions to produce a visual difference of at most 3%
  at either end — not worth the register.

---

## E3. Block light propagation, and what happens when a source is destroyed

### Propagation — the flood fill, and why it is already generic in Blocksmith

[0fps, "Voxel lighting" (2018)](https://0fps.net/2018/02/21/voxel-lighting/), fetched this
session: *"Flood fill lighting is an approximation of ambient occlusion where lighting
values are propagated using a breadth-first search along the 6-faces of each cube."* And,
on the core rule: *"the light level of each voxel decreases by 1 as we propagate."* That is
the entire algorithm: seed every light-emitting or sky-exposed cell at its source level, walk
outward one face at a time, and every step across a non-opaque face costs the light level 1,
until nothing changes.

`source/world/light.c:394–506` (`lightPropagateColumn`, read this session) already
implements exactly this, generically, for *any* block with a declared `luminance` — it does
not have a torch-shaped code path to add:

```c
// Block seeds: luminous cells. syncLuminance() refreshes the table from the
// registry and answers whether anything in it emits at all; no core row declares
// a luminance today, so on a single-player world the scan is skipped entirely.
if (syncLuminance()) {
    for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
        ...
        const uint8_t lum = s_luminance[id];
        if (!lum) continue;
        ...
        setNibble(lc->blk, ci, lum);
        queuePush(q, (uint16_t)ci, &dropped);
    }
    spread(lc->blk, chunks, q, &dropped);
}
```

**What Blocksmith should do: nothing new here either.** The moment E5's new block declares a
non-zero `luminance`, this scan seeds it and `spread()` (the BFS) does the decrement-by-one
walk. This is the same conclusion `docs/plan-1.8.10-light.md` §2.2 already reached by reading
the same function.

### Removal — the classic algorithm, and what Blocksmith does instead

The naive fix for "a light source was destroyed" — walk outward re-running the flood fill
from every neighbour — is wrong, and the reason is well documented. The classic correct
version, from the [Seed of Andromeda voxel-lighting devlog](https://notverymoe.github.io/md-gamedev-gems/voxel/lighting/soa/index.html)
(the original `blog.seedofandromeda.com` is offline; this is a maintained mirror, fetched
this session), is a **two-queue, two-pass** algorithm:

1. **Removal pass.** Read the destroyed source's old light level, set that cell to 0, and
   push it onto a *removal* queue carrying the value it used to hold. For each neighbour: if
   its stored light is non-zero **and less than** the popped value, it was almost certainly
   lit *by* the thing being removed — zero it too and push it onto the same removal queue.
   But if a neighbour's stored light is **greater than or equal to** the popped value, it was
   being lit by something else (a second nearby light, or sunlight) — **do not clear it**;
   instead push it onto a second, ordinary propagation queue, because it is now a valid
   *source* for refilling the hole the removal just made.
2. **Refill pass**, run only after the removal queue is fully drained: an ordinary flood fill
   (E3's propagation section) seeded from everything the removal pass collected in step 1's
   second queue.

The two-queue split is what prevents the two failure modes naive removal has: re-lighting a
cell that should now be dark (because a same-source neighbour was checked against the wrong
comparison), and leaving a cell dark that a second, unrelated light source should still be
covering.

**What Blocksmith does instead, and why that is also correct:** `source/world/light.c` does
not implement this incremental algorithm at all. Every edit — placing or breaking a block
anywhere in a column — calls `lightRelightColumn` (`light.c:521–548`), which reseeds the
*entire column* from scratch (every sky-exposed top cell, every luminous cell found by
`syncLuminance()`'s scan) and reruns the full flood fill over it, exactly as if the column
had just been generated. This sidesteps the removal algorithm's whole failure class by
construction — there is no "was this cell lit by the thing that just disappeared?" question
to get wrong, because nothing is *inferred* from old values; every value in the column is
recomputed from the current block layout. The cost is a full-column relight per edit instead
of a relight proportional to the affected region, and that cost is already measured:
`light.c:87–89`, read this session — *"measured on the host over one real generated column,
200 iterations each, the sweeps take 4.064 ms against the flood fill's 0.156 ms"* — so a
torch broken today costs **0.156 ms of column relight**, not the removal-then-refill's
smaller-but-more-complex alternative.

**What Blocksmith should do: keep the full-column recompute.** **[INFERENCE]** It is
simpler, it is already proven (the flood-fill engine has a second, independently-agreeing
implementation — `lightRelightColumnSweeps`, `light.c:550–641` — that the host suite diffs
it against byte-for-byte), and 0.156 ms per edit is not a cost this project's own numbers
suggest is worth trading for the extra complexity of a two-queue incremental remover. The one
case where that trade might change is a *cave full of many torches lit or broken in the same
frame* — a full-column recompute pays 0.156 ms per edit regardless of how many other lights
are in the column, where an incremental remover pays roughly the size of the affected region
only. That is a real, plausible v1.8.10+ hot spot (a player mining out a torch-lined
corridor), but it is not a case this brief has a measurement for, and building the
two-queue remover ahead of evidence that it is needed would be exactly the kind of
unrequested engineering the project's standing rules ask not to do. **Flagged, not
recommended, in Open Questions.**

---

## E4. Smooth lighting — a correction to when it shipped, and how it relates to AO

### The date correction

The brief this research was commissioned against names "Beta 1.8 'SMOOTH_LIGHTING'." That is
wrong on the date, and worth correcting explicitly the way `sky-and-weather.md` corrects the
sun's rise direction (§A5): **the Smooth Lighting option was added in Beta 1.3, not Beta
1.8.** [Minecraft Wiki, "Java Edition Beta 1.3"](https://minecraft.wiki/w/Java_Edition_Beta_1.3),
fetched this session, lists under Options: *"Added 'Smooth Lighting' option"* and under
Lighting: *"Smooth lighting engine added with the help of MrMessiah."* (MrMessiah's
BetterLife mod is the same one the "beta lighting" nostalgia threads found in this session's
search reference as the pre-Beta-1.3 way to get the effect at all.)

Beta 1.8 did ship a real, separate lighting change, which is very likely what the brief's
"SMOOTH_LIGHTING" framing was actually pointing at: [Minecraft Wiki, "Java Edition Beta
1.8"](https://minecraft.wiki/w/Java_Edition_Beta_1.8), fetched this session, lists *"Implemented
a new lighting engine. The lighting on a block is given a tint based on the most prominent
source of light"* and, separately, *"Artificial light now gives a very subtle 'flicker'."*
That second line is E5's `torchFlickerX` in E2's quoted `updateLightmap` — flicker is a Beta
1.8 feature, and it is real, but it is not the same feature as smooth per-vertex corner
lighting. **[NO SOURCE]** for the literal identifier `SMOOTH_LIGHTING` existing anywhere in
decompiled `GameSettings` source at any version searched this session — the actual field
backing the option, in every decompiled `GameSettings.java` found this session (1.8.x-era),
is an integer, `public int ambientOcclusion`. Treat "SMOOTH_LIGHTING" as this brief's own
shorthand for the option, not a verified in-game constant name.

### The mechanism, and why it is also the AO mechanism

Smooth lighting and ambient occlusion are not two features that happen to interact — in
Minecraft's own implementation they are **the same per-vertex averaging step**, read two
ways. [0fps, "Ambient occlusion for Minecraft-like worlds" (2013)](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/),
fetched this session, gives the canonical formula for a face corner's occlusion from its two
edge-adjacent cells (`side1`, `side2`) and the diagonal cell (`corner`):

```
function vertexAO(side1, side2, corner) {
  if (side1 && side2) return 0
  return 3 - (side1 + side2 + corner)
}
```

— each argument is a 0/1 "is this neighbour opaque" test, and the sum is subtracted from the
maximum (3) so more occluding neighbours means a smaller, darker value. `source/world/mesher.c`'s
own `cornerAO()` (feeding `v->ao = meshAoPack(cornerAO(si, c), tint)`, `mesher.c:478`) already
implements this shape for occlusion; `cornerLight()` (`mesher.c:398–417`, quoted in
`docs/plan-1.8.10-light.md` §2.1) is the **light** version of the identical idea — average up
to four cells touching a corner, skipping occluding taps, once for the sky nibble and once
for the block nibble. **This is exactly what smooth lighting is**: not a blur pass, not a
post-process, but per-vertex averaged light values that the rasterizer's own Gouraud
interpolation smears across a triangle in hardware. `docs/plan-1.8.10-light.md` §2.1 already
established this exists, ships since v1.8.0, and costs nothing new — this brief's job is only
to confirm that reading against real vanilla sourcing, which it does: 0fps's own comment
thread (summarised in this session's search results) has the AO mod's original author stating
Minecraft's discrete per-face lighting came first and the vertex-averaged "smooth lighting"
variant came later and works by *"averaging light levels at vertices on a plane"* — the same
description as `cornerLight()`.

### What it does for non-cube shapes — and why flat is correct, not a gap

Vanilla does **not** smooth-light cross-shaped models (flowers, saplings, and — relevant here
— torches). This is not an incidental limitation of the geometry; it is an explicit,
named model flag. [Minecraft Wiki, "Model"](https://minecraft.wiki/w/Model), fetched this
session, documents `ambientocclusion` as a per-model boolean, *"Whether to use ambient
occlusion (`true` — default), or not (`false`)."* The actual vanilla torch model was fetched
directly from a mirror of the shipped asset tree,
[`assets/minecraft/models/block/template_torch.json`](https://github.com/InventivetalentDev/minecraft-assets/blob/1.20.1/assets/minecraft/models/block/template_torch.json):
it sets **`"ambientocclusion": false`** and every element in it sets **`"shade": false"`** —
both a corner-averaging opt-out and a directional-face-darkening opt-out, on the same model.
So the torch's own geometry is deliberately flat-lit in vanilla, not merely too thin for the
corner average to look right on.

Blocksmith already matches this, independently — `source/world/mesher.c`'s cross-emitter,
read this session:

```c
// mesher.c:788–791, emitCross()
v->v   = c->v_hi ? r->vslot1 : r->vslot0;
v->nrm = CROSS_NRM;
v->ao  = meshAoPack(3, tint);         // 3 = fully unoccluded — a cross never shadows itself
v->pad = lit ? s->light[si] : 0;      // the CELL's own light, not cornerLight()'s average
```

against the regular quad emitter three lines above it in the same function region
(`mesher.c:478–479`): `v->ao = meshAoPack(cornerAO(si, c), tint); v->pad = lit ?
cornerLight(s, si, p, c) : 0;` — corner-averaged. **What Blocksmith should do: nothing —
E5's new torch block should use `BLOCK_SHAPE_CROSS` (`docs/plan-1.8.10-light.md` §2.3
already recommends this for the geometry reason; this section is the confirmation that flat
lighting on that shape is also the behaviourally correct choice, not just the cheap one.**

---

## E5. Torches themselves — the content Blocksmith is actually missing

Per the headline, none of this exists in the registry yet. Specification, sourced:

- **Light level 14.** [Minecraft Wiki, "Light"](https://minecraft.wiki/w/Light), light-source
  table, fetched this session: *"Torch ... 14."* One level below the 15 maximum (beacons,
  conduits, the sun at noon) — `docs/plan-1.8.10-light.md` §2.3 proposed "14 or 15" as a
  placeholder; **14 is the sourced number and should be used exactly**, not rounded up.
- **Shape: a thin, off-centre post, not a full block.** The vanilla model (fetched this
  session, same source as E4) defines the torch's core post element as `"from": [7, 0, 7],
  "to": [9, 10, 9]` in the model's 0–16 unit space — a 2×2-pixel-footprint post, 10⁄16 of a
  block tall, offset to sit centred in its cell. `template_torch.json` sets
  `"ambientocclusion": false` and `"shade": false` (E4).
- **No collision.** [Minecraft Wiki, "Torch"](https://minecraft.wiki/w/Torch), fetched this
  session: *"Being non-solid, torches have no collision box."* `BLOCK_SHAPE_CROSS` in
  Blocksmith is already non-solid by convention (`docs/plan-1.8.10-light.md` §2.3), so this
  falls out of the shape choice for free.
- **Wall-mounted variants need orientation metadata.** Same Torch page: the Java block state
  is `facing`, one of `east / north / south / west`, meaning *"the direction the top of the
  torch is facing"* — i.e. which way it leans off the wall it's stuck to, not which wall it's
  stuck to. **[NO SOURCE this session]** for whether Blocksmith's registry format has a slot
  for per-instance block state/orientation data at all — `docs/plan-1.8.10-light.md` scoped
  its recommendation to a single freestanding (floor) torch and did not raise wall torches.
  **Recommendation: ship the freestanding torch only for v1.8.10**, matching the plan
  document's already-scoped recommendation; wall variants are a real, sourced follow-up, not
  a gap in this content pass.
- **Flicker is real, sourced, and already partially wired.** [Minecraft Wiki, "Java Edition
  Beta 1.8"](https://minecraft.wiki/w/Java_Edition_Beta_1.8) (E4): *"Artificial light now
  gives a very subtle 'flicker'."* The mechanism, from
  [MCP-919 `EntityRenderer`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/client/renderer/EntityRenderer.java),
  fetched this session — a per-tick random walk on a delta, decayed and re-randomised, not a
  fixed sine wave:

  ```java
  this.torchFlickerDX = (float)((double)this.torchFlickerDX +
      (Math.random() - Math.random()) * Math.random() * Math.random());
  this.torchFlickerDX = (float)((double)this.torchFlickerDX * 0.9D);
  this.torchFlickerX += (this.torchFlickerDX - this.torchFlickerX) * 1.0F;
  ```

  This value feeds `torchFlickerX * 0.1F + 1.5F` — E2's block-term multiplier — so at rest
  (`torchFlickerX = 0`) it is exactly **1.5**, which is precisely
  `source/shaders/world_dynamic.v.pica`'s `curveBlockMul` constant today (`curveConsts(4.0,
  -3.0, 1.5, 0.0)`, its own comment naming it *"vanilla's torch multiplier at rest"*). **The
  animation itself — the per-tick wander — is not implemented.** Blocksmith ships the
  flicker's rest value and nothing that moves it. **[INFERENCE]** Recommendation: this is a
  legitimate, small follow-up, not a v1.8.10 blocker — a torch that is merely warm and
  static still reads as "a torch," and the animated version is a single extra uniform (the
  current-frame flicker value, computed on the CPU with the same random-walk shape above, one
  float per frame like `dayLevel` already is) multiplying into where `curveBlockMul` sits
  today. Cost: one uniform write per frame, zero vertex-shader instructions if the CPU folds
  the `*0.1+1.5` into the value it writes, zero remesh.

---

## E6. Cheap fake-lighting and fake-water tricks on PICA200 fixed-function hardware

### What the hardware actually has — two separate mechanisms, not one

`docs/plan-1.8.10-light.md` §0 already measured, by reading this codebase and the installed
devkitPro SDK headers, that the terrain pass uses 2 of 6 TEV combiner stages and 2 of 3
texture units, leaving 4 stages and 1 unit free, plus a fully unused fourth texture path (the
procedural texture engine). [GBATEK's TexEnv register page](http://problemkaputt.de/gbatek-3ds-gpu-internal-registers-texturing-registers-environment.htm),
fetched this session, confirms the combiner shape independently: six `TEXENV` stages, each
selecting its combine mode from *"Replace, Modulate, Add, Add signed, Interpolate, Subtract,
Dot3 RGB, Dot3 RGBA, Multiply then add, Add then multiply"* over sources that include
textures 0–3, primary/fragment colour, a constant, and the previous stage's own output.

What this brief adds to that survey: **the PICA200 also has a second, entirely separate
fixed-function lighting mechanism — real per-pixel hardware lighting, not a TEV trick —
called fragment lighting**, and it is genuinely available to homebrew, not a
commercial-SDK-only feature. Three independent, fetched-this-session confirmations:

1. **The library exposes it as a first-class API.**
   [`citro3d/include/c3d/light.h`](https://github.com/devkitPro/citro3d/blob/master/include/c3d/light.h)
   defines `C3D_LightEnv_t` with room for **up to 8 simultaneous lights**
   (`C3D_Light* lights[8]`), and lookup-table slots for Fresnel, spotlight, distance
   attenuation, and up to 6 general-purpose LUTs (`C3D_LightEnvLut`) — enough to build a
   Phong term (`LightLut_Phong`) or a cheap NdotL ramp entirely in fixed function.
2. **It is a working, shipped example, not theory.**
   [`devkitPro/3ds-examples/graphics/gpu/fragment_light/source/main.c`](https://github.com/devkitPro/3ds-examples/blob/master/graphics/gpu/fragment_light/source/main.c),
   fetched this session, sets up a single Phong-lit light (`LightLut_Phong(&lut_Phong, 30)`,
   `C3D_LightEnvLut(&lightEnv, GPU_LUT_D0, GPU_LUTINPUT_LN, false, &lut_Phong)`,
   `C3D_LightPosition`) and composites the result with `C3D_TexEnvFunc(env, C3D_Both,
   GPU_ADD)` — **with no custom fragment shader at all**, because none can exist on this
   hardware; the whole lighting term comes from the fixed-function unit.
3. **Real, shipped 3DS titles used exactly this, alongside baked vertex colour.** A decompiled
   vertex shader from *The Legend of Zelda: Ocarina of Time 3D*
   ([public gist, fetched this session](https://gist.github.com/M-1-RLG/c0a3ef277241781297ddeb4763dfbc88))
   branches on an `IsVertexLighting` flag: when set, the shader dots surface normals against
   up to three light directions and writes a baked `outColor`, the same Gouraud-interpolated
   idea Blocksmith already ships (E4). When a `FragmentLighting` flag is set instead, the
   shader writes a normal-encoding **quaternion** to `outQuat` and defers the actual lighting
   math to the fixed-function hardware unit described above. **A real, commercial 3DS title
   used both mechanisms this brief describes, choosing per-material.**

### Why Blocksmith should use the cheap one, not the hardware lighting unit

**[INFERENCE]** Fragment lighting needs a per-vertex normal (as a quaternion output) to
operate on, and `source/world/mesh_vertex.h` (read this session) is explicit that
`MeshVertex` is locked at exactly 8 bytes with **zero spare bytes** — every field is already
spoken for (`x, y, z` int8, `pad` the packed light byte E2 covers, `u, v` atlas coordinates,
`nrm` the face/water-drop byte, `ao` occlusion-and-tint). Adding a real per-vertex normal
would mean growing every terrain vertex in the world, which the file's own static asserts
(`sizeof(MeshVertex) == 8`, alignment locked at `offsetof(MeshVertex, u) == 4` for the exact
attribute-alignment hazard `docs/research/sky-and-weather.md`-adjacent lessons in this
project's vault already paid for once) exist specifically to prevent being done casually.
Blocksmith's terrain does not need real normals for its lighting to look directional — E4
already establishes that the corner-averaged vertex light Blocksmith ships today *is*
Minecraft's own "directional-enough" answer to the same problem, on hardware with real,
unused per-pixel lighting available. Reaching for the fixed-function lighting unit on top of
that would be solving a problem Blocksmith's existing baked lighting already answers, at the
cost of a vertex format change this codebase treats as a last resort.

**What is cheap, and is already the project's own committed answer:**
`docs/plan-1.8.10-light.md` §5.2 (read this session, not re-derived — quoted because it is
the definitive, already-reasoned answer):

> "a small, tiling grime/detail texture bound to the free texture unit (2), sampled using the
> *same* texture coordinate the atlas already uses ... and combined against the existing
> stage-0 output with a new TEV stage 2 ... Cost: 1 TEV stage, 1 texture unit, a few KB of
> VRAM for a small (8×8 or 16×16) tiling texture, zero new vertex bytes, zero new uniform
> registers, zero new shader instructions."

This is the historically standard fixed-function trick — a static multiply/detail texture
faking surface variation without computing a light direction at all — and it is genuinely
free on this hardware in a way a lighting-unit approach is not. **Recommendation: this is the
answer to "cheap fake directional lighting," and it is already specified; nothing in this
brief's research changes it.**

### Fake water reflection — what is feasible, and what is honestly not

**True mirrored reflection needs a second camera pass** — rendering the scene again from a
mirrored viewpoint into a render target, then compositing it. `docs/plan-1.8.10-light.md`
§5.3 already established, from this project's own comments, that the vertex stage is *"the
narrow one on this hardware"* — and doubling relevant vertex throughput for a second full
camera pass is not a cheap trick by any reading of "cheap" on hardware already treating
vertex transform as the bottleneck. **This brief's research agrees and adds nothing that
weakens that conclusion: there is no fixed-function shortcut to a second camera's worth of
geometry.** State it plainly rather than softening it, per the owner's own instruction on
this ask (quoted in `plan-1.8.10-light.md` §5): drop it, do not fake a worse version of it.

What *is* feasible, and both options are grounded in real, present hardware capability:

1. **A scrolling shimmer/gradient texture** on the same free texture unit and a second free
   TEV stage, blended against water's existing 0.70-alpha colour, animated by one time
   uniform — `docs/plan-1.8.10-light.md` §5.3's own recommended, committed version. This is
   the safe answer, costed there at one more TEV stage, the same reused texture unit, and one
   vertex uniform register.
2. **The procedural texture engine (`proctex`)** — real per-pixel animated noise with zero
   VRAM image cost, exposed to TEV as a genuine fourth texture source (`GPU_TEXTURE3`).
   Confirmed present on this machine's SDK install this session:
   `C:\devkitPro\libctru\include\c3d\proctex.h` exists. **`grep -ri proctex source/` returns
   zero matches anywhere in this codebase** — it has never been used here, has no proven
   integration pattern the way the detail-texture trick above has (`atlasBind()`'s per-draw
   rebind pattern), and `docs/plan-1.8.10-light.md` §5.3 already flags its texcoord-routing
   interaction with the existing fog pass as unconfirmed from the headers alone.
   **[NO SOURCE this session]** for any homebrew or commercial 3DS title actually using
   `proctex` for a water effect specifically — the searches this session that turned up real
   fixed-function lighting usage (fragment lighting, above) did not turn up a proctex
   precedent. **Recommendation: keep this a stretch item, exactly as the plan document already
   scopes it, not a committed v1.8.10 deliverable.**

What none of this delivers, stated in the terms the player will actually see: water that
catches a moving highlight as the camera or the scroll passes it, not a reflection of
anything actually in the scene. No reflected trees, no reflected sky, no reflected other
players. That is the honest ceiling of fixed-function TEV on this GPU, and both the plan
document and this brief's independent research agree it is the ceiling, not a temporary
limitation of either author's search effort.

---

## Open questions and discrepancies found

Per this brief's scope, only `docs/research/light-and-torches.md` was written or edited.
Everything below was found by reading other files, not changed in them.

1. **`source/shaders/world_dynamic.v.pica`'s own top-of-file docstring (lines 14–16) and
   `source/world/mesh_vertex.h`'s comment (lines 29–38, 30–32 specifically) both still
   describe the pre-v1.8.10 model** — `brightness = ... x curve(max(sky x dayLevel, block) /
   15)` and *"takes max(daySky, block) as the luminance"* — even though the actual code a few
   lines below the first (`world_dynamic.v.pica:246–284`) and the code the second file
   describes have both already moved to the additive model E2 documents and cites this file
   for. This is exactly the kind of drift `docs/research/sky-and-weather.md` §6 warns about
   for a different feature (biome tint merging) — a comment that stops matching the code
   beneath it, invisible to every test because the code is correct and only the prose is
   stale. **Consequence of leaving it:** the next person to read either file for orientation
   will be told the wrong combine rule by the file's own header before reaching the code that
   contradicts it. Not fixed here — out of this brief's scope (only this file was to be
   touched) — but worth a one-line comment update in both places the next time either file is
   touched for an unrelated reason.
2. **E3's incremental light-removal algorithm was not built**, and per this brief's own
   analysis should not be, unless a future profiling pass on a torch-dense area (a
   torch-lined mined-out corridor, many lights broken in one frame) shows the current
   full-column-recompute cost (0.156 ms/column, `light.c:88`) actually mattering. No such
   measurement exists yet. Flagged so it is picked up by evidence, not speculation, if it
   ever becomes a real cost.
3. **E5's torch flicker animation is unbuilt** — Blocksmith currently ships flicker's *rest*
   value (`curveBlockMul = 1.5`) with nothing driving it away from rest. Real, sourced,
   cheap, and explicitly scoped as a follow-up rather than a blocker in E5.
4. **E6's `proctex` path is real and present on this machine's SDK but has zero usage
   anywhere in this codebase and zero precedent found this session for using it on a water
   effect specifically.** Matches `docs/plan-1.8.10-light.md`'s own UNRESOLVED flag on the
   same question; this brief did not resolve it further.
5. **Wall-mounted torch orientation (E5) is sourced but unscoped for this version** — the
   Java `facing` block state is documented, but whether Blocksmith's registry/block-state
   format can carry per-instance orientation data at all was not established this session.
   Freestanding-only is this brief's recommendation for v1.8.10; wall torches are real,
   sourced, future work.
