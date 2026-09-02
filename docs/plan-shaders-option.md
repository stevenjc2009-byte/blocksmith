# Shaders option — cheap fake lighting + fake water reflections

Scope note: this is the design and feasibility study for a player-facing
**"Shaders" toggle** — not ray tracing, not physically-based rendering. The
ask, verbatim in spirit: the cheap, wrong-but-pretty look of a Minecraft
shader pack running on hardware too weak for the real thing — fake specular
glints, a fake shimmer on water — built out of whatever the PICA200's
fixed-function pipeline actually has spare. If the measurements below say it
doesn't fit, dropping it is the correct outcome, not a failure to find a
clever enough trick. It is earned here, not assumed either way.

This document was produced without touching `source/`, `tests/`, `tools/`, or
the Makefile. Every shader assembled below was assembled from a **scratchpad
copy** of the real file, using the project's real `picasso.exe`
(`/c/devkitPro/tools/bin/picasso.exe`); nothing under `source/shaders/` was
written to.

## Provenance key

- **[measured]** — a real tool was run and this is its real output.
- **[codebase]** — read directly out of a file in this tree; cited `file:line`.
- **[research]** — an external source; cited by URL, with its reliability noted.
- **[proposal]** — a design choice made in this document, not a fact.

---

## 1. What "shaders on" would actually be

No fragment/pixel shader exists on this hardware to write free-form code
into. **[codebase]** `source/scene/chunk_render.c:874`: "No fragment shader:
TEV stage 0 multiplies the atlas sample by the vertex colour." The entire
fragment-side budget is the TEV combiner stages; the entire per-vertex budget
is `source/shaders/world_dynamic.v.pica` (New 3DS) and `world.v.pica` (Old
3DS). "Shaders on" has to be built entirely out of those two things — TEV
stage combinations and vertex-shader arithmetic on the data already reaching
the vertex — not out of anything resembling a real lighting or reflection
pass.

## 2. Constraint analysis, measured

### 2a. TexEnv (TEV) stages — 6 total, 2 in use, 4 spare **[codebase]**

`chunk_render.c:876-879` configures stage 0 (atlas × vertex colour,
`GPU_MODULATE`). `chunk_render.c:954-962` configures stage 1 (distance fog,
`GPU_INTERPOLATE`/`GPU_REPLACE`). The comment immediately after, at
`chunk_render.c:953`, states directly: "Stages 2..5 stay at citro3d's
REPLACE(previous) passthrough, as they always have been" — confirming 6
addressable stages exist (0 through 5) and only the first two are ever
touched by this codebase.

External corroboration was inconclusive: a search turned up one tertiary
source describing a "5-Stage TEV Pipeline" and another describing "6
configurable color combiners" for PICA200, which disagree with each other.
Rather than cite whichever one happens to agree with the codebase, this
document treats "6 stages, 4 free" as **codebase-confirmed** (the project's
own code addresses stage indices 0-5 and explicitly documents 2-5 as unused)
and flags that independent, authoritative external confirmation was not
found.

### 2b. Texture units — 3 total, **2 in use, 1 spare** — corrects the brief

The brief this task started from said "3 texture units, of which the game
uses 1." That is wrong. **[codebase]**:

- Unit 0: the block atlas. `source/gfx/atlas.c:60`, `C3D_TexBind(0, &s_tex)`.
- Unit 1: the distance-fog ramp. `source/gfx/fogtex.c:66`,
  `C3D_TexBind(1, &s_tex)`, with the comment directly above it at
  `fogtex.c:54-55`: "Unit 1. Unit 0 is the atlas in the world pass and the
  font sheet in the UI batch; unit 2 is still unused."

A grep across every `C3D_TexBind` call in `source/gfx/` (`atlas.c`,
`crackatlas.c`, `sprite.c`, `fogtex.c`) found no call anywhere that ever
binds unit 2. **Two units are spent, one is free.** Any fake-reflection
design gets exactly one spare texture unit to work with, not two — this
halves the room the brief assumed was available.

### 2c. Vertex-shader uniform registers — measured, not the number the file's own comment gives

The PICA200 vertex engine has 96 total `float[4]` uniform registers (c0-c95).
**[research, corroborated]**: 3dbrew.org's GPU register documentation states
96 vertex float uniform registers; the codebase's own comment at
`world_dynamic.v.pica:62` states the same total independently ("The PICA200
vertex stage has 96 float uniform registers in total").

That same comment (`world_dynamic.v.pica:61-64`) claims: "this program
already spent 78, so a palette of 8 takes it to 86." That number does not
survive a real compile.

**[measured]** — copied the real, unmodified `world_dynamic.v.pica` into the
scratchpad and ran picasso's header generator:

```
picasso.exe -o vfx_orig.shbin -h vfx_orig_shbin.h vfx_world_dynamic_orig.v.pica
```

The generated header gives the exact base register of every named uniform:

```
VSH_FVEC_projection   0x00  len 4
VSH_FVEC_modelView    0x04  len 4
VSH_FVEC_faceShade    0x08  len 64
VSH_FVEC_dayLevel     0x48  len 1
VSH_FVEC_fogParams    0x49  len 1
VSH_FVEC_tintPalette  0x4A  len 8   (ends at 0x52 = register 82)
```

That's 82 registers for the six *named* uniforms (`.fvec`) — already 4 more
than the in-file comment's "78." The comment is stale.

Named uniforms are not the whole story: the file also declares five
`.constf` blocks — `tintConsts`, `uvScale`, `consts`, `aoMix`, `lightConsts`
(`world_dynamic.v.pica:78,89,91,96,102`) — each a baked constant vec4 that
picasso allocates out of the *same* c0-c95 bank, just without a CPU-side
upload. picasso's own header generator doesn't report where it puts these
(it only reports named `.fvec` uniforms, since those are the ones a C caller
needs a register index for), so the true free count can't be read off the
header alone.

**[measured]**, empirically, by bisection — the only way to get a real
number for this: a probe uniform `.fvec probeReg[N]` was inserted into a
scratchpad copy immediately after the `tintPalette` declaration, and
compiled with the real `picasso.exe` for decreasing `N`:

```
N=15 -> error: not enough uniform space: probeReg[15]
N=12 -> error: not enough uniform space: probeReg[12]
N=10 -> error: not enough space for local constant 'lightConsts'
N=9  -> exit 0 (compiles clean)
```

**Exactly 9 free `float[4]` registers remain in `world_dynamic.v.pica`** —
not 18, not 14, not the ~10 implied by the stale in-file comment. This is a
hard, measured ceiling, not a reasoned one.

The same test run against the **Old 3DS shader**, `world.v.pica` (which has
no `dayLevel` uniform and no `lightConsts` block — it's the baked-lighting
variant, not the day/night-adaptive one):

```
VSH_FVEC_projection   0x00  len 4
VSH_FVEC_modelView    0x04  len 4
VSH_FVEC_faceShade    0x08  len 64
VSH_FVEC_tintPalette  0x48  len 8
VSH_FVEC_fogParams    0x50  len 1     (81 named registers)
```

plus 4 `.constf` blocks (`uvScale`, `tintConsts`, `consts`, `aoMix` —
`world.v.pica:59,79,83,90`). Bisection probe:

```
N=12 -> error: not enough uniform space: probeReg[12]
N=11 -> exit 0
```

**11 free registers in `world.v.pica`.** Counter-intuitively, the Old 3DS
shader has *more* uniform headroom than the New 3DS one, because it doesn't
carry `dayLevel` or the day/night light-unpack constant. Register headroom
here is not the New-3DS-favours-New-3DS story the render-distance memory
budget tells — it runs the other way.

### 2d. Vertex-shader instruction count

**[research, caveated]**: 3dbrew.org's GPU shader instruction set page cites
roughly 512 instruction slots and ~128 operand descriptors for the vertex
shader engine. That page's own sourcing is reverse-engineered debug
information pulled from a commercial title's binary, not an official
Nintendo/DMP specification — treated here as a reasonable but
unauthoritative ceiling.

**[codebase, hand-counted]**: `world_dynamic.v.pica`'s `.proc main` (lines
124-241) contains 41 arithmetic/data-movement instructions plus `end` — 42
total. No devkitPro tool in this toolchain disassembles a `.shbin` back into
an instruction count (`picasso --help` offers only `-o`/`-h`/`-n`/`-v`, no
disassembler), so this is a manual line count of the source I read in full,
not a tool-measured figure. Given the shader has no branches or loops, the
count is unambiguous either way. At ~42 of a ~512 ceiling, instruction count
is nowhere near the binding constraint — **the two real constraints are
register headroom (§2c) and texture units (§2b), not instructions.**

### 2e. Frame budget — cannot be measured on this project's tooling

**[codebase]** `source/debug/metrics.h:8-21`: Azahar (this project's
emulator) reports a constant 0.25 ms for `C3D_GetDrawingTime()` regardless of
actual GPU load, so real GPU time cannot be read at all under emulation; the
file works around this by timing wall-clock submit/sync windows instead,
which the same comment block says "on hardware" would mean something
different than it does under emulation. `metrics.h:43-45`'s
`metricsGpuTimed()` exists specifically to detect and flag this
(`"NOT TIMED"` on the overlay) rather than silently report a fake number.

**[codebase]** `docs/VERSION-LIST.md:308-309`, on the most recently released
version in that document: "Not verified. Nothing in this release has run on
real 3DS hardware; every timing figure above is a host x86-64 ratio, not a
console frame cost." This is the project's own most recent, explicit,
dated admission of the same gap — not an assumption carried over from
somewhere else.

**Consequence for this document**: no per-frame millisecond cost for any of
the design below can be honestly stated. Everything costed here is in the
currencies that *can* actually be measured off this hardware's real
constraints — registers, texture units, TEV stages, bytes — not in
milliseconds. Any claim of "this will run fine" is unverified until it is
run on a real console; see §7.

---

## 3. Design — fake specular ("fake lighting")

**[proposal]** A per-face, vertex-computed highlight added on top of the
existing baked light (faceShade × AO × day/night lum × biome tint), the same
family of cheat Minecraft shader packs use on flat, low-poly faces: a fixed
dot product between the face's normal and a constant "sun" direction,
rather than anything resembling real specular reflection.

**The key finding that makes this cheap**: the existing indexed fetch
`mov r3, faceShade[a0.x]` (`world_dynamic.v.pica:130`) already pulls a full
`float[4]` row into `r3`. Of its four lanes, the shader reads `r3.x` (baked
brightness, `world_dynamic.v.pica:227`), `r3.y` (water-drop height offset,
`:151`), and `r3.w` (vertex alpha, `:238`) — but **never `r3.z`**, confirmed
by grep: no `r3.z` or `r3.zzzz` appears anywhere in the file. On the CPU
side, `chunk_render.c:181-191`'s `faceShadeTableBuild()` currently *fills*
that fourth lane anyway — `chunk_render.c:188`,
`s_faceshade_rows[i][2] = s_faceshade_rows[i][0]`, a duplicate of the
brightness value — but says why in its own comment
(`chunk_render.c:177-180`): purely so the stored table is "a complete record
of the row rather than a compressed one" for a test that compares all four
values against an older loop, not because the shader reads it. **A per-face
fake-specular factor can be packed into that same `.z` lane, overwriting the
harmless duplicate, at zero additional uniform-register cost and zero
additional indexed fetch** — it rides the row already fetched for
brightness, and nothing currently reads the value it would replace.

This deliberately avoids adding a *third* indexed/relative-addressed fetch.
`source/world/water_alpha_test.c:320-344`'s `checkIndexedFetch()` is called
twice inside `checkOneShader()` — once for the face-shade fetch
(`water_alpha_test.c:391`), once for the tint-palette fetch
(`water_alpha_test.c:397`) — and a third fetch pattern would need a matching
third call added there. Packing the specular factor into the *already-fetched*
`faceShade` row's spare `.z` lane sidesteps that entirely: nothing about the
shader's fetch pattern changes, so nothing about that test needs to change
either. This was a deliberate design choice to avoid that coupling, not an
oversight — flagged here so it's visible as a decision rather than a gap.

**Toggle cost**: one new uniform register, `shaderFxLevel[1]` (0.0 = off,
1.0 = on), multiplied against the specular contribution before it's added
into `outclr`. When 0.0, the contribution is exactly zero — a real off
switch, not a "still costs the same, just invisible" one. This is 1 of the 9
free registers measured in §2c.

**Vertex instructions added**: roughly 4-6 (a dot-product-style multiply
against a per-face constant, scaled by `shaderFxLevel`, added into the
existing `outclr` computation at `world_dynamic.v.pica:227-230`) — negligible
against the ~470 free instruction slots from §2d.

**Cost tally**:
- 1 of 9 free vertex-uniform registers (New 3DS) / 1 of 11 (Old 3DS if mirrored, see §6).
- 0 new TexEnv stages, 0 new texture units, 0 new vertex attributes.
- 0 bytes of new VRAM or MeshVertex change — no re-mesh required.
- CPU side: the per-face row-derivation code that already fills 64 rows of
  `faceShade` (`chunk_render.c`, the function documented around
  `chunk_render.c:173-177`) needs one more column filled per face — six
  values, computed once, not per frame differently.

## 4. Design — fake water reflection ("shaders" water half)

**[proposal]** Real reflection would need a second render pass from a
mirrored camera — not "cheap fake," and not affordable here. The cheap-fake
substitute: a shimmering brightness modulation over water's surface, using
the one spare texture unit (§2b) and one spare TEV stage (§2a), active only
during the **existing, separate transparent draw pass** water already uses.

**Reuses an existing texture — zero new art, zero new VRAM.** The distance-
fog ramp (`source/gfx/fogtex.c`) is already an 8-row × 128-texel gradient
texture resident on unit 1. Binding that *same* texture object a second time
to unit 2, sampled with a vertex-animated, scrolling UV instead of the
depth-derived one unit 1 uses, produces a moving brightness band across
water with no new procedurally-generated asset and no new `tools/` script —
which matters, since "nothing borrowed" and "art comes from `tools/`" would
otherwise mean this needs new code outside `docs/`, which this task cannot
write.

**Scroll uniform**: one new register, `scrollPhase[1]`, incremented once per
frame by whatever owns per-frame uniform uploads today (the same place that
re-uploads `faceShade` and `tintPalette` every `pipelineBind()` call) — the
second of the two new registers this design spends, bringing the total to 2
of the 9 free (New 3DS).

**Where it's armed — following an existing precedent exactly.** Water
already gets its own draw pass, separate from opaque terrain:
`chunk_render.c:2415` arms `C3D_AlphaTest(true, GPU_GREATER, ALPHA_CUTOFF)`
for the transparent pass, `chunk_render.c:2431` arms real alpha blending, and
`chunk_render.c:2455`/`chunk_render.c:2462` explicitly put both back
afterward. **[proposal]**: the new TEV stage 2 (reflection blend) should be
armed in that same block, right alongside the existing `C3D_AlphaTest`/
`C3D_AlphaBlend` calls, and disarmed back to passthrough in the same place
those are put back — the codebase already has a "GPU state is unowned,
explicitly bracket it" discipline for exactly this pass; this rides it
instead of inventing a new pattern.

**Toggle cost, and why it's free when off**: the toggle for this half is a
plain CPU `if (shaders_on)` around the `C3D_TexBind(2, ...)` and
`C3D_TexEnv` calls in that bracket — when off, stage 2 is simply never
reprogrammed and stays at citro3d's passthrough default, exactly as it does
today. No uniform multiply is needed on this half; unlike the specular half
(§3), there's no GPU-side "on but zeroed" state to worry about, because the
CPU never arms the stage in the first place when the option is off.

**Cost tally**:
- 1 of 9 free vertex-uniform registers (New 3DS), on top of §3's 1 — **2 of
  9 total spent, 7 remain**, comfortably clear of the measured ceiling.
- 1 of 4 free TexEnv stages (stage 2), used only during the transparent pass.
- 1 of 1 free texture unit (unit 2) — this is the tight one; there is no
  second spare unit for anything further.
- 0 new VRAM: reuses `fogtex.c`'s existing texture object, no new asset.
- 0 new vertex attributes, 0 MeshVertex changes.

---

## 5. What this uses up, against what was actually spare

| Resource | Total | Already used | Spare before this design | Spent by this design | Spare after |
|---|---|---|---|---|---|
| TexEnv stages | 6 | 2 | 4 | 1 | 3 |
| Texture units | 3 | 2 | 1 | 1 | 0 |
| Vertex uniform registers (New 3DS) | 96 | 87 (measured, §2c) | 9 | 2 | 7 |
| Vertex uniform registers (Old 3DS) | 96 | 85 (measured, §2c) | 11 | 2 (if mirrored) | 9 |
| Vertex instructions | ~512 (research ceiling) | ~42 | ~470 | ~10-12 | ~458-460 |

The texture-unit line is the one worth being honest about: this design
spends the *entire* remaining spare texture unit. There is no room left for
a second fake-shader effect that also needs its own sampled texture (a
separate fake-caustics pass, say) without either freeing unit 1 or unit 0
first, or without the reflection effect sharing unit 2 with whatever else
wants it.

## 6. Old 3DS vs New 3DS — named explicitly, only where it differs

- **Which shader gets touched.** `world_dynamic.v.pica` is already
  New-3DS-exclusive — `world_dynamic.v.pica:1-6` states this program is
  "bound INSTEAD of [world.v.pica] on a New 3DS only," chosen once at init
  via `APT_CheckNew3DS`. Adding the specular term (§3) to
  `world_dynamic.v.pica` alone makes fake specular a **New-3DS-only**
  feature by default, at zero extra design cost — that's simply which file
  gets edited.
- **Register headroom does not block shipping it on Old 3DS too.**
  §2c's measurement found `world.v.pica` has *more* spare registers (11) than
  `world_dynamic.v.pica` (9), so mirroring both halves of this design into
  `world.v.pica` fits with room to spare (9 of 11 would remain). This is not
  a hardware-forced asymmetry — it is a genuine choice, and it is §7's first
  "his call."
- **TexEnv stages and texture units are shared, not per-shader.** §2a/§2b's
  budget lives in `chunk_render.c`'s `pipelineBind()`/draw-pass code, which
  runs identically on both consoles — nothing there differs by console.
- **Everything else is identical.** No render-distance interaction, no
  linear-heap interaction (this design adds zero linearAlloc'd bytes — see
  §5, no new VRAM anywhere), no MeshVertex or attribute layout change on
  either console.

---

## 7. HIS CALL

1. **New-3DS-only, or mirrored to Old 3DS too?** Register headroom permits
   either (§6) — this is a scope decision, not a hardware constraint. Ship
   New-3DS-only first is the smaller, lower-risk cut (touches one file, no
   more risk of `world.v.pica`/`world_dynamic.v.pica` drift than exists
   today); mirroring to Old 3DS doubles the shader-file surface this design
   touches and the ongoing maintenance the file's own comments already warn
   about ("a drift here would leave flowing water full-height on exactly the
   hardware nobody tests on" — `world_dynamic.v.pica:31`).
2. **How the fake specular should actually look** — a hard rim/edge
   highlight (more "shader-pack" cartoonish) vs. a soft broad gradient (more
   "reads as decoration, less as a bug") is an art-direction call this
   document does not make. §3 costs either equally; which curve shape the
   six `faceShade[].z` values encode is not decided here.
3. **How the water shimmer should scroll** — direction, speed, and whether
   it should react to anything in-world (e.g. player proximity, weather —
   `source/world/weather.h`'s rain state exists and could plausibly drive
   this) versus running on an unconditional clock. §4 only costs the
   plumbing for *a* scroll register; what drives its value is undecided here.
4. **Whether spending the last spare texture unit here is the right call**
   at all, given §5's finding that this design leaves zero texture units
   spare for anything else. If another feature already wants unit 2, that
   feature and this one are now mutually exclusive as designed, and that
   conflict needs resolving before either ships.

## 8. LINES SOMEONE ELSE MUST ADD

None of these were touched by this document — write access was `docs/` only.

- `source/shaders/world_dynamic.v.pica` — add `.fvec shaderFxLevel[1]` and
  `.fvec scrollPhase[1]` after the existing `.fvec tintPalette[8]`
  (currently line 73); add the specular-lookup and reflection-scroll
  instructions inside `.proc main` (currently lines 124-241), before the
  final `outclr` write at line 230.
- `source/shaders/world.v.pica` — the same two uniform declarations and
  instruction additions, **only if** the owner picks "mirror to Old 3DS" in
  §7.1. Register headroom was measured and fits (§2c); this is not blocked,
  only undecided.
- `source/scene/chunk_render.c:181-191` — `faceShadeTableBuild()`. Replace
  the line at `:188` (`s_faceshade_rows[i][2] = s_faceshade_rows[i][0];`,
  currently a harmless duplicate of the brightness value) with the chosen
  per-face fake-specular factor per §7.2. Nothing else in this function
  needs to change.
- `source/scene/chunk_render.c:2415-2462` — the transparent-pass
  arm/disarm block. Add the unit-2 texture bind and the new stage-2
  `C3D_TexEnv` setup inside the existing arm block (around line 2415-2431),
  and reset stage 2 back to passthrough inside the existing disarm block
  (around line 2455-2462), gated on the shaders-on toggle per §4.
- **No change needed** to `source/world/water_alpha_test.c` — this design
  deliberately avoided adding a third indexed/`mova`-addressed fetch (§3),
  so `checkIndexedFetch()`'s two existing call sites
  (`water_alpha_test.c:391`, `:397`) remain correct as they are. If a future
  revision of this design ever *does* add a third indexed fetch, a matching
  third `checkIndexedFetch()` call would need to be added inside
  `checkOneShader()` immediately after line 397 — noted here only as a
  tripwire, not as something this design currently needs.
- Wherever the options/settings UI lives — a new "Shaders" toggle entry,
  wired to a bool that both (a) drives `shaderFxLevel`'s per-frame uniform
  upload value (0.0/1.0) and (b) gates the `if (shaders_on)` branch around
  the unit-2 bind and stage-2 setup in `chunk_render.c`. This document does
  not know where the existing settings/options UI code lives and did not go
  looking for it, since UI wiring is implementation, not design.

## 9. What could not be verified

- **Real GPU frame cost, on either console.** §2e: Azahar cannot report real
  GPU time, and the project's own most recent released version documents
  that nothing has run on real hardware (`docs/VERSION-LIST.md:308-309`).
  Every cost figure in this document is in registers, stages, units, and
  bytes — quantities that were genuinely measured — never in milliseconds,
  because no honest millisecond figure is available on this project's
  current tooling.
- **What it actually looks like.** No screenshot, no visual check was
  produced or is possible from a docs-only design pass with no `source/`
  write access. §7.2 and §7.3's open art-direction questions are exactly the
  reason a real implementation attempt, followed by a real screenshot, is
  the next honest checkpoint — not this document.
- **Whether 42 instructions is exactly right.** §2d's instruction count was
  hand-counted from the source, not extracted by a disassembler (none was
  found in this devkitPro install). It is precise enough to establish "nowhere
  near the ~512 ceiling," which is the only thing it needs to prove here, but
  it is not a tool-verified figure the way the register count is.

## 10. Recommendation

**Ship a reduced version: fake specular only, New-3DS-only, first.**

The measurements say this is genuinely feasible, not merely clever-sounding:
2 of 9 free vertex-uniform registers, 1 of 4 free TEV stages, and (for the
specular half alone) *zero* of the 1 free texture unit. Splitting the two
halves matters because they cost very different amounts of the scarcest
resource measured here (§5's texture-unit row): the specular half is nearly
free (no texture unit, no VRAM, reuses an already-fetched register lane),
while the reflection half spends the *entire* remaining texture unit and
forecloses anything else that might want it (§7.4).

Recommend shipping §3 (fake specular) alone first, New-3DS-only per §6,
since it has no open resource conflict and the smallest possible surface
(one file, `world_dynamic.v.pica`, plus the row-derivation function in
`chunk_render.c`). Treat §4 (fake water reflection) as a second, separate
decision gated on §7.4 — whether spending the last texture unit here is
worth foreclosing whatever else might want it — rather than bundling both
under one toggle by default.

This is not "drop it because it's hard" — every number above says it fits.
It is "ship the cheaper, resource-safe half now, and make the
texture-unit trade-off in §7.4 a deliberate choice rather than a default."
