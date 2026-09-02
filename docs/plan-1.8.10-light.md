# v1.8.10 "Light" — feasibility and design: shaders, torches, particles, water bob

This is a feasibility pass, not an implementation. Nothing under `source/` was touched to
produce it. Every claim below is labelled at the point it is made:

- **measured** — a number computed by hand from constants actually read in this tree, or a
  number this project's own code already asserts as measured (its provenance is quoted).
- **read in code** — a fact taken directly from a file in this tree, cited by path and line.
- **documented for the hardware** — a fact about the PICA200/3DS taken from the devkitPro SDK
  headers installed on this machine (`C:\devkitPro\libctru\include\...`), cited by path.
- **proposal** — my own design suggestion. Never asserted as already true or already working.
- **UNRESOLVED** — something I could not settle from this machine, matching this project's own
  convention (see `docs/plan-1.8.2` task 13b's UNRESOLVED list) for saying so instead of
  guessing.

Four owner asks, in the order given, one with an explicit escape hatch he already granted:
a shaders option (fake lighting + fake water reflections, explicitly not ray tracing, drop it
if it's bad), smooth torch lighting (the one he cares about most), particles for water
splashing, and a calmer Minecraft-style water bob. The headline finding, stated first because
it changes the shape of two of the four asks: **the smooth-lighting infrastructure and the
calmer water bob he is asking for already exist in the committed tree.** Sections 2 and 5 say
exactly what is missing versus what only needs a playtest to confirm.

---

## 0. Hardware facts, verified against this tree and this SDK install, not assumed

| Fact | Value | Provenance |
|---|---|---|
| TexEnv (TEV) combiner stages | 6 total, **2 configured**, 4 free | documented for the hardware (stage count) + read in code (`source/scene/chunk_render.c:876-879` stage 0, `:954-962` stage 1, `:953` comment that 2..5 stay at citro3d's default passthrough) |
| Texture units | 3 total, **2 bound** in the terrain pass (0=atlas, 1=fog ramp), unit 2 free | read in code (`chunk_render.c:872` atlasBind, `:932` fogTexBind→unit 1; `source/gfx/fogtex.c:54-55` states explicitly "unit 2 is still unused") |
| Procedural texture engine (proctex) | a **4th**, distinct texture path, fed to TEV as `GPU_TEXTURE3`, own noise generator + 128-entry LUT, **completely unused in this codebase** | documented for the hardware (`C:\devkitPro\libctru\include\c3d\proctex.h`, `3ds\gpu\enums.h:322` `GPU_TEXTURE3 = 0x06 // Texture unit 3`) + read in code (grep for "proctex"/"ProcTex" across `source/`: zero matches) |
| Vertex uniform registers, `world_dynamic.v.pica` | 86 of 96 nominal spent; **effective ceiling is 95, not 96** | measured, quoted verbatim from `source/debug/biomeborder_draw.c:20-22`: *"world_dynamic.v.pica now spends 86 of the PICA200's 96 vertex uniform registers after the biome-tint work — measured, a 17-row palette assembles and an 18-row one fails picasso"* (78+17=95 assembles, 78+18=96 fails ⇒ picasso reserves at least one register nothing here can claim) |
| Free registers in `world_dynamic.v.pica` today | **9** (against the effective 95 cap) | measured, per the above |
| A *new* shader program's register budget | independent, fresh, ~95 free — proven pattern already in this tree | read in code: `highlight.v.pica` (10 of 95 used) is reused verbatim by three separate callers — `source/scene/highlight.c`, `source/scene/playermodel.c`, `source/debug/biomeborder_draw.c` — each parsing its own `DVLB_s`/`shaderProgram_s` from the same shbin, explicitly chosen over adding a fourth `.pica` file **because** of `world_dynamic`'s register pressure (`biomeborder_draw.c:19-25`) |
| Water's shader | **shares `world_dynamic` exactly** — no `water.v.pica` exists | read in code: `grep -i water source/shaders/*` matches only comment prose; `source/world/water.c` has zero `shaderProgramSetVsh`/`DVLB_ParseFile`/`*_shbin` references; water is fed through the ordinary chunk mesher (`chunk_render.c:1428-1432` `waterFillScratch`) and drawn through `s_program` (world_dynamic) with alpha blending |
| `MeshVertex` (the chunk vertex) | 8 bytes, **zero spare bytes**, 3 spare *bits* (`ao` bits 5..7) | read in code, `source/world/mesh_vertex.h:11-27,68`; `source/world/mesher.h:159` states bits 5..7 free outright |
| `faceFlatKey()` — the greedy-merge signature | already folds AO, light (when lit) and biome tint; a run of up to 15 blocks (`ATLAS_MAX_MERGE_BLOCKS`) merges into ONE quad taking the FIRST cell's attributes if two faces' keys match | read in code, `source/world/mesher.c:624-640`; the smear failure mode is the tree's own documented lesson at `mesher.c:599-617` (quoted in full in §6) |
| `LightColumn` | 32,768 bytes exactly (16,384 sky + 16,384 block, one 4-bit nibble per channel per block, **per-block resolution, not coarser**) | measured/computed from `source/world/light.h:72-73,84` and `light.c:19-22` — confirms the brief's figure to the byte |
| World memory budget | 12,582,912 B cap; 65,648 B/column; radius 5 = 170 columns = 11,160,160 B = **88.7%** | measured/computed against `source/world/budget.h:37-41,75,90-97` — every figure in the brief reproduces exactly from the header's own constants |
| Sky-uniform-per-frame-rebuild hazard (checked because a sibling project has one) | **does not apply here** | read in code: no per-frame *varying* sky/water/fog uniform exists in `chunk_render.c`; `SKY_FOG_BGR`/`SKY_CLEAR_RGBA8` are compile-time constants re-asserted identically every `pipelineBind()`; the fog ramp texture (`fogtex.c`) is a static load-once `.t3x` asset, never regenerated at runtime; the old `C3D_FogLut` hardware-LUT upload path was already removed (`chunk_render.c:920-926` comment, dated 2026-09-01) |
| The `shaderProgramFree` use-after-free hazard | applies to **any new shader** this doc proposes | this codebase already carries and guards against it in three places (`crackoverlay.c:67-75`, `highlight.c`, `biomeborder_draw.c:36-40`): *"One program for the whole process, built the first time and never torn down... `shaderProgramFree` does not null `program->vertexShader` while citro3d separately caches a pointer to the last program it bound. Freeing and rebuilding this static across a rejoin is the exact use-after-free that hard-crashes the console, already paid for once in this project."* Any new shader below must follow this exact pattern. |

---

## 1. Verdict summary

| Ask | Verdict | Why |
|---|---|---|
| 1. Shaders (fake lighting + fake water reflections) | **Reduced version — do the cheap half, drop the expensive half** | Fixed-function TEV can genuinely fake "not flat"; it cannot fake a mirror without a second camera pass |
| 2. Smooth torch lighting | **Mostly already shipped — add the missing content, then playtest** | The corner-averaged smooth-lighting math has been in `mesher.c` since v1.8.0. No block in the game currently emits light. |
| 3. Particles (water splash) | **Do it, scoped** | No particle system exists; a small world-space billboard system is cheap and low-risk using an already-proven shader pattern |
| 4. Calmer water bob | **Already done in the committed tree — verify, don't rebuild** | Commit `e7232ba` (already on the branch this doc was written against) replaced the old 0.223-block/4.4 Hz bug with a 0.055-block/0.35 Hz camera-only swell |

---

## 2. Ask 2 first, because it reframes the version — smooth torch lighting

### 2.1 What already exists (read in code)

`source/world/mesher.c:398-417`, `cornerLight()`:

```c
// Smooth per-corner light: average the four cells touching this corner from outside the
// face — the neighbour cell plus the same two flanks and diagonal the AO rule already walks —
// skipping occluding ones. The neighbour cell is never occluding (the face would not have
// been emitted), so at least one tap always counts.
static inline uint8_t cornerLight(const MeshScratch* s, int si, const FacePlan* p,
                                  const CornerPlan* c)
{
	const int taps[4] = { si + p->neighbour, si + c->ao1, si + c->ao2, si + c->aoc };
	int ssum = 0, bsum = 0, cnt = 0;
	for (int t = 0; t < 4; t++) {
		if (s_cell_flags[taps[t]] & CELL_FLAG_OCCL) continue;
		ssum += s->light[taps[t]] >> 4;
		bsum += s->light[taps[t]] & 15;
		cnt++;
	}
	return (uint8_t)((avgByCnt(ssum, cnt) << 4) | avgByCnt(bsum, cnt));
}
```

This is exactly Minecraft's smooth-lighting rule — average up to 4 cells touching a vertex
corner, independently for the sky channel and the block (torch) channel, skipping occluded
taps. `emitFace` writes the result into `MeshVertex.pad` (`mesher.c:479`:
`v->pad = lit ? cornerLight(s, si, p, c) : 0;`). The GPU then interpolates that per-vertex byte
**in hardware** across the triangle (this is what "smooth" actually means here — it is not a
software blur, it is the rasterizer's own Gouraud interpolation of a vertex-shader output):

`source/shaders/world_dynamic.v.pica:100-105`:
```
; Light unpack constants: 1/16 turns the pad byte into the sky nibble, -16 undoes
; sky*16 inside the byte, 1/15 normalises a nibble to 0..1.
.constf lightConsts(0.0625, -16.0, 0.06666667, 0.0)
```
and `mesh_vertex.h:29-38` (dated, corrects a stale "unread by the GPU" comment): *"this field is
READ by the GPU. shaders/world_dynamic.v.pica:99-106 takes it as inpos.wwww and unpacks it as
sky * 16 + block, scales the sky half by the dayLevel uniform, and takes max(daySky, block) as
the luminance every fragment's colour is multiplied by. Since v1.8.0 that shader is bound on Old
3DS as well as New... so there is no build in which this byte is dead."*

**Cost of what already exists: zero.** It reads the same per-block nibble array
(`s->light[]`, the mesh-scratch copy of the `LightColumn` cells described in §0) that untextured
lighting always needed; it adds no vertex bytes, no uniform registers, no shader instructions
beyond what shipped in v1.8.0, and no `LightColumn` bytes. It is not a v1.8.10 feature to build —
**it is a v1.8.0 feature already running in every build since.**

### 2.2 What is actually missing: a light source

`grep -i torch source/` finds the word only in a stale comment (`source/main.c:3076`, about
remote relight — not a block). Confirmed by direct inspection of every place a block could
declare `luminance`:

- `source/world/registry_test.c:586` and `source/world/world_test.c:7178` set `luminance` — both
  are **tests only**.
- No file under `source/world/block.c` or `source/world/registry.c` sets `.luminance = ` to a
  non-zero value on any core block definition.
- `source/gfx/atlas_tiles.h:24-87` lists all 31 painted atlas tiles (0..30) by name — grass,
  dirt, stone, sand, wood, planks, leaves, water, tall grass, snow, ice, cactus, dead bush, fern,
  birch/spruce logs and planks and leaves, tall-grass-top, poppy, daisy, bluebell, orchid, apple.
  **No torch tile.** 32 slots remain free (31..62), one reserved marker at 63 —
  `code-vault/wiki/blocksmith/blocksmith-decision-atlas-ceiling-is-permanent.md`, read directly,
  confirms this is the permanent remaining budget and that a single new tile costs exactly one
  of the 32.

So the lighting *math* is finished and has been for two major versions; the lighting *engine*
already propagates block light generically for any block with `luminance > 0`
(`source/world/light.c:483-491` seeds "luminous cells" from a `syncLuminance()` scan over every
registered id, not a torch-specific code path) — but nothing in the shipped content tree is a
light source. That is the actual v1.8.10 task.

### 2.3 The cheapest implementation that delivers the feel

**Verdict: do it. This is genuinely cheap — it is a content addition riding on infrastructure
that already works, not new engineering.**

1. One new core block, `BLOCK_SHAPE_CROSS` (the shape tall grass and flowers already use —
   `source/world/block.h:251-253`), non-solid, `luminance` set high (14 or 15, the top of the
   4-bit scale — proposal). Registry cost: 1 of the 112 free core ids
   (`docs/plan-1.8.8-biome-identity.md §0`, cross-referenced, not re-derived here).
2. One new atlas tile (1 of the 32 free slots), drawn by `tools/make_atlas.py` same as every
   other tile.
3. **Nothing else changes.** `emitCross()` (`mesher.c:766-808`) already handles cross-shaped
   geometry, already writes `v->ao = meshAoPack(3, tint)` (full AO, no occlusion — correct, a
   torch does not shadow itself) and — this is the detail worth being explicit about —
   `v->pad = lit ? s->light[si] : 0;` (`mesher.c:791`): **the torch's own model is lit FLAT, by
   its own cell's light value, not corner-averaged.** That is correct and matches Minecraft: the
   light source itself reads unshaded/full-bright; it is the *neighbouring solid faces* that get
   the smooth corner-averaged falloff once the torch's luminance seeds `light.c`'s existing
   flood fill.

### 2.4 A caveat to state honestly, not oversell

16 levels (4 bits) is the *value* resolution, and that has not changed and does not need to.
Corner-averaging smooths the *seams between faces at a given light level* — it does not add new
intensity steps between them. A falloff from a torch will still be built out of up to 16
discrete brightness bands; what corner-averaging removes is the hard per-face **block-shaped**
stepping (Minecraft's own "smooth lighting" option does exactly this and is built on the same
16-level system, so this should read as correct to anyone who has played it). If finer-than-16
gradation is ever wanted, the cost is real and large: doubling to 8 bits/channel doubles
`LightColumn` to 65,536 B/column, which **at radius 5 alone costs 65,536 × 170 = 11,141,120 more
bytes** — total radius-5 usage would be roughly 16.7 MB against a 12.58 MB cap, blowing the
entire world budget by itself before counting anything else. **Not recommended; not part of this
ask; flagged only so nobody reaches for it by surprise later.**

### 2.5 Proof

1. **The already-shipped half** (should already pass — this is a regression check, not new
   work): place a stone wall, shine sky light on one side only, screenshot in Azahar. Correct: a
   soft gradient between lit and shadowed corners, not a hard per-face step. If this is *not*
   what's on screen today, that is a real, separate bug worth its own report — not something to
   silently "fix" as part of this ask (per the standing rule on not touching what wasn't asked).
2. **The new content**: place the new block, screenshot a corner near it. Correct = a soft
   brightness gradient radiating across neighbouring cube faces, fading over several blocks;
   wrong = a uniform flat brightness per face (corner-averaging regressed) or total darkness
   (luminance not wired to `light.c`'s seed scan).
3. **The merge-key guard** (already correct by construction, verify it stays that way): place 15
   blocks of the same id in a row beside the torch. Correct = a visible per-block brightness
   gradient across the row; wrong = the whole row rendering at one uniform brightness, which
   would mean `faceFlatKey()`'s light term (`mesher.c:634-636`, already present) stopped being
   checked. This is exactly the smear bug the tree's own commit history caught for biome tint
   (§6) — a torch is the most likely place a regression here would first be visible.
4. **His playtest, on console** (per the standing workflow) — nothing here has run on hardware.

---

## 3. Ask 4 — water bobbing, calmer, Minecraft-style

### 3.1 What already exists (read in code + git history)

Two commits on the current branch, read in full:

**`40d2206`** ("Minecraft-style flow, recessed surface, see-through, and calm swim") fixed the
*actual bug*: the old bob was a **physics-level limit cycle**, not a deliberate wave — a
three-line bang-bang velocity clamp at the water surface that never settled. Measured, quoted
from the commit message: *"Eye crossings go 140 -> 0, holding at 60, 30 and 20 fps; y travel
over a second falls from 0.223333 to 0.000018."* It was replaced with a `BodyWet` state machine
(`BODY_DRY`/`BODY_SURFACE`/`BODY_SUBMERGED`) that damps velocity toward a target via a bounded
first-order approach (`dampAlpha`) instead of hard-clamping, with gravity switched off only while
the swim input is actively driving.

**`e7232ba`** ("let a swimmer climb out, put a calm swell back, blink the flat battery") then put
a **deliberate, much calmer, purely cosmetic** swell back on top, exactly matching what this ask
describes. Current constants, `source/world/physics.h:143-145`:

```c
#define PLAYER_SURFACE_BOB       0.055f   // blocks, peak offset (up and down)
#define PLAYER_SURFACE_BOB_RATE  0.35f    // cycles/second
#define PLAYER_SURFACE_BOB_FADE  3.0f     // 1/s, envelope attack/release rate
```

Applied in `physics.c`'s `bodySurfaceBob()`, called from `scene/player.c`'s `playerUpdate` and
added **only** to the camera eye height, after `bodyStep()` has already resolved the body's real
position — its state (`bob_phase`, `bob_env`) lives on `Player`, not on `Body`
(`world/physics.h`), specifically so, per the commit's own reasoning, *"a view effect cannot be
mistaken for physics state."* It is read back by nothing, so it cannot affect collision,
velocity, or create a feedback loop — which is structurally why it cannot regress into the old
bug.

**Quoted directly from the commit message, because it is exactly the comparison this ask wants:**
*"The bob v1.8.2 removed was a physics limit cycle — 0.223 blocks at 4.4 Hz... this one is not in
the body... 0.055 blocks peak at 0.35 Hz — a quarter of the old amplitude at a twelfth of the
frequency."*

### 3.2 A correction to the brief

The brief's pointer — "buoyancy is already weight-based, don't re-implement it" — does not exist
anywhere in `code-vault/wiki/blocksmith/` (grepped the whole folder for "weight-based" and
"re-implement": zero matches in this project). That note belongs to a **different project** (the
KSP/Atlas rocket-sim water rework) and would be the wrong precedent to cite here. The real,
on-point Blocksmith precedent is `code-vault/wiki/blocksmith/blueprints/blocksmith-twater_rework_blueprint.md`,
which predicted this exact fix before it was built and explicitly flagged the rest-target
constant (`PLAYER_SURFACE_RISE = 0.0f`) as *"the design fork that is HIS, not mine"* — a taste
call, not an engineering one.

### 3.3 Verdict

**Nothing to build.** The ask, read literally — "water bobbing back, but much calmer,
Minecraft-style, because it existed before and was too strong" — describes precisely the
before/after this git history already contains, and the after is what's on the branch this
research was run against. The one-line fix, if his playtest disagrees with the *current* 0.055
blocks / 0.35 Hz feel, is a single constant in `source/world/physics.h:143-145` — not a
redesign. I am not proposing a new number; picking one without his playtest would be exactly the
kind of unrequested judgment call the standing rules ask not to make.

**Handoff:** load the current build, swim to the surface, and just look at the camera bob.
Correct = a small, slow up/down sway that reads as "calm water," clearly weaker than
pre-v1.8.2's build if he remembers it. If it's still too strong, say by how much it feels wrong
(e.g. "about half") and the fix is changing `0.055f` on that one line — no code path change.

---

## 4. Ask 3 — particles, water splashing

### 4.1 What exists today

`grep -ri particle source/` returns **zero matches anywhere in the tree** — no particle system,
in any form, exists. This is genuinely new work, not a gap in something already built.

### 4.2 The vehicle: not `sprite.c`

The brief flagged `SPRITE_MAX_QUADS` (1024, confirmed) as "not tight," which is true but is the
wrong headline: **`sprite.c` is a 2D, screen-space-only batcher and is not the right tool for a
world-anchored splash.** Its shader (`source/shaders/sprite.v.pica`) declares only
`.fvec projection[4]` — **no `modelView`** — confirmed by direct read: the only uniforms are a
projection and a `consts` constant, 5 registers total. Every caller of it (24 files, grepped) is
UI: hotbar, pause menu, remap screen, battery icon, crosshair, name tags, loading screen, font
glyphs. A splash quad drawn through it would have to be reprojected to screen space by hand every
frame and would lose real depth — it would draw in front of a wall between the camera and the
splash, which is a wrong-looking bug, not a stylistic choice.

The right template already exists in the tree: `source/shaders/crack.v.pica`
(`CrackVertex{x,y,z,u,v}`, all `float`, `projection[4]+modelView[4]+consts`, 9 registers) is
exactly a world-space textured quad shader, used today by `crackoverlay.c` to float decal quads
in front of a block's real position with correct depth. It is 86 registers **free**, of the
effective 95.

### 4.3 Proposal

- **New tiny shader**, built on the `crack.v.pica`/`highlight.v.pica` pattern (float position,
  float UV, vertex colour for fade-alpha), one static `shaderProgram_s` for the process,
  never freed across a rejoin — following the exact pattern `crackoverlay.c`/`highlight.c`
  already use to avoid the `shaderProgramFree` UAF (§0). Cost: ~9-10 registers, on its own fresh
  budget — no pressure on `world_dynamic`'s 9 free registers at all.
- **CPU-side particle pool** — proposal, no number is load-bearing here, but something in the
  range of 32-64 live particles is generous against anything this hardware has shown strain on
  in this tree; each is a camera-facing billboard, its 4 corners computed each frame from the
  camera's own right/up vectors (a handful of vector ops per particle, trivial next to the
  1.1M-vertex terrain pool this hardware already pushes every frame per the atlas blueprint's own
  measurement).
- **One new texture** — a small splash sprite; either its own tiny texture or one more atlas
  slot (31 free after the torch tile from §2, still plenty).
- **Trigger**: spawn on a surface crossing (`BodyWet` transitioning to/from `BODY_SURFACE` —
  the same state machine §3.1 already computes for the bob, so this reuses a value that's already
  being calculated every frame, not a new detection system), optionally also on block break/place
  underwater.
- Fade the particle's vertex alpha over its lifetime; no physics needed beyond a simple upward
  puff + gravity, integrated the same cheap way `physics.c` already does for the player body.

**Cost, stated plainly:** one new ~10-register shader (proven pattern, zero pressure on anything
existing), one small texture, a CPU array on the order of tens of structs, and vertex/index
throughput that is a rounding error against the terrain draw. This does not touch `sprite.c`'s
1024-quad budget at all, because it is a separate draw path — correcting the brief's framing
that sprite headroom was the open question here.

### 4.4 Proof

Screenshot a swim-surface entry in Azahar: particles appear at the entry point and fade over
their lifetime. Then walk behind a wall so the camera's line of sight to the splash point is
blocked by a block and confirm the particles disappear correctly — this is the check that proves
they are genuinely world-space geometry sharing the real depth buffer, not a screen-space sprite
trick that would draw through the wall.

---

## 5. Ask 1 — the shaders option: cheap fake lighting and fake water reflections

Explicit owner instruction: not ray tracing, and drop it entirely if it would look bad or cost
too much. Verdict below is not softened on that basis.

### 5.1 What is actually available

From §0: 4 of 6 TEV stages free, 1 of 3 texture units free, plus a completely unused 4th texture
path (the procedural texture engine). None of this touches vertex uniform registers or
`MeshVertex` — **TEV is a per-fragment fixed-function combiner; it costs GPU fragment-stage
state, not vertex budget**, so this whole ask is orthogonal to the 9-register ceiling that
constrains §2 and §4.

### 5.2 Fake lighting — verdict: DO IT, this is a confident, cheap yes

**Proposal:** a small, tiling grime/detail texture bound to the free texture unit (2), sampled
using the *same* texture coordinate the atlas already uses (`outtc0`, computed once in
`world_dynamic.v.pica:178-179`: `mul r2, uvScale, inpack` / `mov outtc0, r2.xyxy`) and combined
against the existing stage-0 output with a new TEV stage 2 (`GPU_MODULATE` or a mild
`GPU_ADD`/`GPU_INTERPOLATE`, tuned by eye). This reads as extra surface detail/shading without
computing a single light direction — it is the same "fake it with a static multiply texture"
trick fixed-function 3D used for exactly this problem for a decade before programmable shaders
existed.

**Cost:** 1 TEV stage, 1 texture unit, a few KB of VRAM for a small (8×8 or 16×16) tiling
texture, **zero new vertex bytes, zero new uniform registers, zero new shader instructions** —
provided the texture unit actually samples `texcoord0` by default when nothing configures it
otherwise.

**UNRESOLVED:** I could not confirm from the installed headers (`texenv.h`, `texture.h`) exactly
which texcoord slot texture unit 2 samples by default; `texenv.h` exposes no per-unit texcoord
selector API distinct from what each unit is wired to. This is a real gap, not a hedge for its
own sake — it is the one fact that decides whether this costs literally nothing or needs a
tiny bit of extra plumbing. **How to settle it:** bind a second texture to unit 2, add the TEV
stage, and look in Azahar — if the detail pattern tiles correctly across merged multi-block runs
the same way the atlas does, unit 2 is reading `texcoord0`; if it looks wrong (stretched,
static, or blank), it needs to be told explicitly, which is a one-line citro3d call once the
right one is identified — not a redesign.

### 5.3 Fake water reflections — verdict: REDUCED VERSION, real reflections dropped

**What is dropped, and why, stated plainly per the owner's instruction to say so rather than
hand back a weak yes:** actual mirrored reflections (trees, sky, other players visibly reflected
in the water) require rendering the scene a second time from a mirrored camera into a render
target. `chunk_render.c`'s own comment already states the vertex stage is *"the narrow one on
this hardware"* (§ mesher.h:62-65, quoted in the codebase's own reasoning for bucketing draws to
avoid transforming unseen geometry) — doubling relevant vertex throughput for a second camera
pass, on hardware that already treats the vertex stage as the bottleneck, is not a cheap trick by
any reading of "cheap." **What the player would get if this were attempted anyway: nothing
usable in the time available, or a bad frame rate — this is why it's dropped, not softened.**

**What is kept — a genuine, cheap fake, honestly described as such:** a moving glint/shimmer on
the water surface, not a mirror. Two ways to get it, ranked by confidence:

1. **Safe, proposal, ~confident as §5.2:** a second small gradient/sparkle texture on the same
   free texture unit (2), rebound for the water draw call the way the atlas/font sheet already
   get rebound per-pass in this codebase (`atlasBind()`'s own comment, `gfx/atlas.h:48-52`,
   describes exactly this pattern already happening for a different texture), scrolled by
   animating a UV offset with one new time uniform (water already shares `world_dynamic`'s
   budget — 9 free registers, this costs 1), blended against water's existing 0.70-alpha colour
   via a new TEV stage. Cost: 1 more TEV stage (of the 4 free), the same free texture unit as
   §5.2 (reused, not doubled — texture *binding* is already known to be per-draw-call state in
   this codebase), 1 vertex uniform register (8 free left after), a small scrolling texture
   asset, and a few extra vertex-shader instructions on `world_dynamic` (well inside its
   remaining headroom). This is the recommended, committed version.
2. **Stretch goal, proposal, materially less certain:** replace the scrolling bitmap with the
   PICA200's procedural texture engine (`C3D_ProcTex`, §0) — genuine per-pixel animated noise
   with zero VRAM image cost, fed through a 128-entry colour LUT tuned to sky-blue/white shimmer
   tones, exposed to TEV as `GPU_TEXTURE3`. This is real, unused, free hardware — but it has
   **never been used anywhere in this codebase**, has no existing test coverage or usage pattern
   to copy the way `highlight.v.pica`'s reuse pattern exists for shaders, and I could not confirm
   its exact texcoord-routing interaction with the fog pass's existing use of `texcoord1` from
   the headers alone. **Recommend as a v1.8.10 stretch item to spike separately, not as a
   committed deliverable** — if it turns out awkward mid-version, the scrolling-texture fallback
   above is the safe, already-fully-reasoned committed version and nothing about it depends on
   the stretch item succeeding.

**What the player gets, stated in his terms:** water that catches the light and shimmers as you
move past it or as it scrolls — not a mirror. No reflected trees, no reflected sky, no reflected
other players. If that reads as "not really reflections" when he sees it, that is the honest
shape of what fixed-function TEV can do, and it was flagged here rather than oversold.

### 5.4 Proof, both halves

1. **Detail texture:** screenshot a flat stone wall before/after — correct = visible fine
   texture/shading variation that was not there before, still recognisably stone, not a
   different-looking material; wrong = a visible seam or tiling break across a merged run (would
   mean the texcoord-sharing assumption in §5.2 was wrong), or a flat, unnoticeable result
   (contrast too low — a tuning pass, not a feasibility failure).
2. **Water shimmer:** screenshot the same patch of water at two points a second or two apart —
   correct = the highlight pattern has visibly moved/scrolled; wrong = it is static (the time
   uniform isn't reaching the shader) or the whole water surface flickers/strobes (blend factor
   too strong, a tuning pass).
3. **Both**, at three render distances and both console-model code paths this project still
   claims to support, since `world_dynamic` is the one shader bound on both Old and New 3DS
   (§0) — a New-3DS-only regression here would be invisible on the machine most testing happens
   on.
4. **His playtest, on console** — TEV fixed-function state has not caused the specific hazard
   this project already learned about attribute alignment (§ vault lesson
   `pica-attribute-alignment`), but it is new GPU state nonetheless and nothing here has run on
   hardware.

---

## 6. The one risk that threads through every ask that touches per-block visuals

`mesher.c`'s own commit history already caught this exact bug for the biome-tint feature, and it
is the standing reason `faceFlatKey()` looks the way it does. Quoted in full because it is the
best statement of the hazard already in this tree, and because §2's torch feature depends on the
light term already being in this key (it is, `mesher.c:634-636` — no new work needed there, but
worth knowing why it's safe):

> *"Without the tint in this key they hash identically, merge into one quad, and that quad takes
> the four vertices of the FIRST cell in the run. Every cell behind it is then drawn in the first
> cell's biome colour: the border does not fade, it JUMPS, up to fifteen blocks away from where
> the biome actually changes."* (`mesher.c:607-611`)

And, critically for why this class of bug is dangerous specifically: *"the face count is
unchanged, the coverage is perfect... and every existing test stays green — the geometry is
right and only the colour is wrong."* (`mesher.c:613-615`) — no automated check catches it; only
a screenshot does.

**None of the four asks in this document add a new per-block-varying value to `MeshVertex`.**
§2's torch feature rides the existing `pad`/light term, already in the key. §4's particles and
§5's TEV work are both entirely separate draw paths (their own shaders, their own texture units)
that never touch `MeshVertex` or the greedy mesher at all. This section exists so that is a
verified conclusion, not an assumption: if any future work *does* add a new field that varies
per block and feeds it into `world_dynamic`'s shading, it must be added to `faceFlatKey()` or it
will reproduce this exact smear, invisibly to every test in the suite.

---

## 7. What could not be verified from this machine

- **Texture unit 2's default texcoord source** (§5.2) — settled by a five-minute Azahar check,
  not a design question.
- **The procedural texture engine's exact interaction with the existing fog texcoord** (§5.3
  stretch item) — needs a spike, explicitly scoped as optional.
- **Anything on real hardware.** Per the project's own standing note, nothing has run on console
  since v1.2.5; every visual claim above is an Azahar claim until his playtest. Attribute
  alignment and GPU state hazards have both bitten this project on the emulator/hardware gap
  before (`mesh_vertex.h:54-57`), so this is not a formality.
- **The ±1 register discrepancy** noted in §0 between `world_dynamic.v.pica`'s literal `.fvec`/
  `.constf` count (87, by my own count of the declarations) and the file's own stated total (86).
  Both numbers agree on the practical answer (headroom is small, single digits), so nothing in
  this document depends on resolving which is exactly right — flagged so it isn't silently
  smoothed over.

---

## 8. Recommended v1.8.10 scope, ordered

1. **Torch block + tile** (§2) — cheapest, highest-value, rides existing infrastructure.
   Playtest first to confirm the *already-shipped* smooth-lighting math looks right today before
   adding content on top of it.
2. **Verify the water bob** (§3) — no build work unless his playtest says otherwise; costs
   fifteen minutes to check.
3. **Fake lighting detail texture** (§5.2) — cheap, low-risk, settle the one UNRESOLVED texcoord
   question early since it gates the water-shimmer design too.
4. **Water splash particles** (§4) — genuinely new work, but small and low-risk against a proven
   shader pattern.
5. **Water shimmer** (§5.3, committed scrolling-texture version) — do after #3, since it reuses
   the same texture-unit-2 plumbing once proven.
6. **Procedural-texture shimmer** (§5.3 stretch) — only if time remains after 1-5; not a
   committed deliverable.
