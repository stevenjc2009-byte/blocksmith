# Entity subsystem plan — v1.8.14 Animals, v1.8.16 Monsters

Status: PLAN ONLY. Nothing in `source/` has been changed by this document.

Every claim below carries a provenance tag:

- **[read]** — read out of the code in this tree.
- **[measured]** — a number produced by running something. Where the run was not mine, the
  source of the measurement is named.
- **[reasoned]** — derived by argument from **[read]** facts. Could be wrong.
- **[assumed]** — a working assumption that nothing here checks. Every one of these is
  repeated in §11.

Line-number citations are deliberately absent for `light.c`, `chunk_render.c`, `interact.c`,
`worldgen.c`, `main.c`, `worker.c` and `worldgen_density.c` — all live under concurrent edit.
Those are cited by function name only.

---

## §1 What exists today

### 1.1 The no-entity-system finding — verified, it holds

The claim was checked with bash `grep -rniE` over `source/` and, separately, over
`deps/blocksmith-server/` with an explicit path (the Grep *tool* is blind to `deps/` because
it is gitignored, which has produced a wrong conclusion on this project before).

**[measured]** `grep -rniE "entity|entities|\bmob\b|mobs|creature|\bnpc\b" source/` returns 88
lines. Every single one is a false positive of one of three kinds:

- the substring `identity` — network identity keys, world generator identity, `Mtx_Identity`;
- forward-looking prose in a comment (`main.c`, `mining.h`, `tick.h`) naming mob AI as a thing
  that does not exist yet;
- one line in `net/hosttest/bsnet_transport_hosttest.c` describing a stand-in payload as
  "world/entity sync" traffic.

**[measured]** The same sweep over `deps/blocksmith-server/` returns 26 lines, all `identity`
or the same forward-looking `tick.h` prose (the server carries a byte-identical copy of that
file).

There is no entity struct, no entity array, no entity file, no entity opcode, and no entity
draw call anywhere in either tree. **The finding holds.**

### 1.2 The remote-player path

Three separate modules, and they generalise by very different amounts.

**Transport and storage — `source/net/networld.c` / `networld.h`** **[read]**

- `RemoteSlot` is the storage: `bool occupied; uint32_t sid; float x,y,z; float yaw,pitch;
  uint64_t last_seen_ms;`. Held in `static RemoteSlot s_remotes[NETWORLD_MAX_REMOTE];`.
- `NETWORLD_MAX_REMOTE` is 15 — `BS_GAME_MAX_PLAYERS` (16) minus the local player.
- `applyPosUpdate()` decodes `BS_APP_POS_UPDATE`; `upsertRemote()` claims the first free slot
  on an unrecognised `sid` and **silently drops the newcomer if the table is full**.
- There is no join message and no leave message. The only despawn is `ageOutRemotes()`, run
  every `networldUpdate()`, clearing any slot untouched for `NETWORLD_REMOTE_TIMEOUT_MS` (3000).
- `NETWORLD_POSE_INTERVAL_MS` is 100, i.e. 10 Hz, rate-limited outbound in `networldSendPose()`
  and unlimited inbound.
- **There is no interpolation.** `upsertRemote()` overwrites the pose in place and the renderer
  reads it raw, so remote players snap between 10 Hz samples.
- `networld.c` never asks which world column a remote player is in. Its header says pose
  tracking is "deliberately independent of the World this file otherwise guards".

**Rendering — `source/scene/playermodel.c` / `playermodel.h`** **[read]**

Its own header calls it a placeholder and names it like one. What it actually does:

- `playerModelInit()` builds three axis-aligned boxes with `addBox()` — body, head, and a
  wedge "nose" so yaw is legible — into one `linearAlloc`'d VBO. `PM_VERTS` is 108, static-
  asserted. Vertex format is `PmVertex { float x, y, z; }` — **position only, no UVs**.
- Built once, `GSPGPU_FlushDataCache`'d once, never rebuilt. No heap allocation after init.
- It reuses the compiled `highlight_shbin` binary but parses it into its **own** `DVLB_s` and
  `shaderProgram_s`, deliberately, so it cannot disturb `scene/highlight.c`'s state.
- `playerModelDraw()` issues **two** `C3D_DrawArrays` per player: body+head as one range in the
  player's colour, nose as a second range in a darker one.
- **There is no texture.** Colour comes from a TEV constant, `kPlayerColours[sid & 7]`.
- Fog is TexEnv stage 1, `GPU_INTERPOLATE(GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1)`, with
  `fogTexBind()` on unit 1 and the shape recomputed every draw from
  `fogShapeFor(chunkRenderDistance()->boundary, FOG_DENSITY)`. The hardware fog unit is forced
  off with `GPU_NO_FOG` so nothing double-fogs.
- **Nothing is culled.** `playerModelDraw()` loops over `networldRemoteCount()` and draws every
  one, with `GPU_CULL_NONE`. The only cost bound is the hard cap of 15.
- `playerModelInit()` returning false makes every later draw a silent no-op — the whole game
  survives a failed init.

**Stereo — `source/main.c`** **[read]** `drawEye()` calls `drawStage(WD_DRAW_PLAYERS, eye)` and
then `playerModelDraw(view)`. `drawEye()` is called once for the left eye unconditionally and
again for the right eye when `s_stereo` is set. Both eyes draw even at slider 0. So the player
model is drawn **once per eye**, and any entity renderer placed at the same call site inherits
that for free — and inherits the doubling too.

### 1.3 Honest verdict: what generalises

**Generalises — copy it, do not reinvent it:**

- `playermodel.c`'s *renderer shape* is exactly right and should be the template: one
  `linearAlloc`'d VBO built once at init; uniforms re-uploaded on every draw; fog via TexEnv
  stage 1 and `fogTexBind()`; drawn inside `drawEye()` so stereo needs no new code; a failed
  init degrading to "nothing is drawn" instead of a crash. **An entity renderer that is a
  sibling of this file is the right plan. One that invents a parallel pipeline is not.**
- `source/world/tick.h` **was written for this subsystem and is currently unused by it.** It
  already carries `TICK_NEAR_BLOCKS 24`, `TICK_NEAR_DIST_SQ 576`, `TICK_FAR_PERIOD 10`,
  `tickPeriodForDistSq()` and `tickDue(tick, period, id)` — whose `id` parameter is documented
  as taking "an entity index" to stagger decimated work. Its own comment names "v1.9.0 task 29"
  as the customer and says the server "will want the identical rule for its own entity update,
  from this same shared file". It has no `<3ds.h>` and is already host-tested.
- Free helpers that need no change: `worldStandingY()` (ground finder for spawning),
  `lightGetSky()` / `lightGetBlock()` (the monster darkness predicate), `frustumTestAABB()` in
  `scene/frustum.c`, `bodyStep()` / `bodyMove()` / `bodyJump()` in `world/physics.c`, and
  `invBridgeAdd()` for drops.

**Does NOT generalise — and pretending otherwise is where this goes wrong:**

- `RemoteSlot` as a data model. It is 15 slots keyed by a server-assigned `sid`, created by
  upsert-on-unknown-sid and destroyed by a 3-second silence timeout. An entity has no `sid`, is
  not created by a packet, and must not vanish because nothing spoke for three seconds.
- **Flat TEV-constant colour.** This is the single largest gap. A pig has to be textured. The
  entity pass needs its own `.pica` shader with a texcoord output and its own texture atlas.
  Nothing in `playermodel.c` helps with that half.
- **No culling.** Fine at 15 unconditional models; not fine at 48. Distance and frustum culling
  are new work.
- **Two draw calls per model.** At entity counts this doubles the draw budget for nothing. An
  entity must be one contiguous range, one draw call.
- `networld.c`'s file-static store. The entity store must be a struct passed by pointer.

---

## §2 The entity model

### 2.1 Where it lives

`source/world/entity.h` and `source/world/entity.c`, with **no `<3ds.h>`**, matching every
other file in `source/world/`. This is not decoration: it is what makes the red-arm tests in
§10 possible at all, and `physics.h`'s own comment states the rule plainly — a decision left in
a file the host suite cannot build is "a decision nothing checks".

### 2.2 No file statics

The concurrent second-generator-thread investigation is blocked by file-static mutable state.
This plan adds none. The store is a struct the caller owns:

```c
typedef struct {
    Entity   e[ENTITY_SLOTS];
    int      count;        // occupied slots
    int      cap;          // per-console occupancy limit, <= ENTITY_SLOTS
    uint16_t next_id;      // monotonic handle source, for tickDue() stagger
    uint32_t spawn_accum;  // spawn-attempt phase
} EntityWorld;
```

`main.c` holds exactly one `static EntityWorld s_entities;` and passes `&s_entities` into every
entity function. Nothing in `world/entity.c` is file-static, so nothing there is unsafe for a
worker thread to be near. **[reasoned]** — the generator worker never touches entity state at
all under this design, so this is belt-and-braces rather than load-bearing, but it costs
nothing and it removes the file from the list of things the threading work has to reason about.

### 2.3 The struct, and the byte cost

```c
typedef struct {
    uint8_t  kind;      // ENT_NONE = 0 means the slot is free
    uint8_t  health;    // 0..20, the scale the protocol already uses (see §7)
    uint8_t  flags;     // on_ground / in_water / hurt_flash / despawn_pending
    uint8_t  ai_state;  // IDLE / WANDER / FLEE / CHASE
    float    x, y, z;
    float    vx, vy, vz;
    float    yaw;       // no pitch: a quadruped does not pitch
    uint16_t ai_timer;  // ticks until the next AI decision
    uint16_t id;        // stable handle
} Entity;
```

Layout arithmetic, 32-bit ARM EABI, largest member alignment 4:

- offset 0: four `uint8_t` — 4 bytes, no padding, they pack exactly
- offset 4: `x, y, z` — 12 bytes, ends at 16
- offset 16: `vx, vy, vz` — 12 bytes, ends at 28
- offset 28: `yaw` — 4 bytes, ends at 32
- offset 32: `ai_timer`, `id` — 4 bytes, ends at 36
- struct alignment 4, 36 is a multiple of 4, **no trailing pad**

**36 bytes per entity** **[reasoned]** — hand arithmetic, not read off the compiler. Note the
struct deliberately uses `uint8_t` rather than enums for `kind`/`ai_state`: the ARM EABI's
`-fshort-enums` has already made host `sizeof()` lie about a console struct on this project
once (`Chunk`, 8 vs 16 bytes). Using fixed-width types removes that failure mode, but the 36
is still unverified against `arm-none-eabi-gcc` and a `_Static_assert(sizeof(Entity) == 36)` is
a required deliverable of phase P0.

For comparison **[read]**: `RemoteSlot` is 40 bytes by the same arithmetic. An entity is
*smaller* than the thing the game already keeps 15 of.

### 2.4 How many, and where the memory comes from

```
ENTITY_SLOTS      48      the array size, compiled in, both consoles
ENTITY_CAP_OLD    24      runtime occupancy limit on an Old 3DS
ENTITY_CAP_NEW    48      runtime occupancy limit on a New 3DS
```

`cap` is set at boot from `hwIsNew3ds()`, mirroring the existing `renderDistMaxFor(bool)`
pattern in `scene/render_dist.h` **[read]**. One array size for both consoles, for exactly the
argument `world/budget.h` makes about `WORLD_BUDGET_BYTES` not being per-console: a limit that
allocates nothing costs the smaller machine nothing, and two sizes would buy a second failure
mode for no saving.

**Total store cost: 48 x 36 = 1,728 bytes, plus 16 bytes of bookkeeping = 1,744 bytes.**

**Which heap: none of them.** `EntityWorld` is a single static instance in `main.c`, so it
lives in the process **BSS**, not on the application heap, not on the linear heap, and not
inside `budgetClaim()`.

This matters because of where the pressure actually is:

- The world store is on the **application heap** behind `budgetClaim()`, capped at 12 MB by
  `WORLD_BUDGET_BYTES`, and radius 5 already sits at 11,160,160 B = **88.7%** of it **[read,
  `world/budget.h`]**. Charging entities there would eat 1,422,752 B of the only 1,422,752 B
  left. **This plan takes nothing from the world budget.**
- The **linear heap** is the genuinely tight one: 67,108,864 B with **18,285,568 B free**
  **[measured — owner's live v1.8.6 New 3DS reading]**, and the planned audio system already
  wants 1.68 MB of it. Entities take **~21 KB** of it, once, for the shared mesh VBO (§5.2) —
  0.11% of the free linear heap.
- The application heap is 60,977,152 B with 253,032 B in use at boot **[measured — same
  reading]**. Entities take **zero** of it.

So the answer to "where does the space come from" is: 1,744 B of BSS, 21 KB of linear for one
VBO, 32 KB of VRAM for one texture. Nothing from the 88.7%-full world budget.

### 2.5 Storage and iteration

A flat array with an occupancy sentinel, not a linked list and not a free list.

- Free slot is `kind == ENT_NONE`. Spawning scans for the first such slot. **[reasoned]** At 48
  slots a linear scan is ~48 byte comparisons and happens at most a few times a second — a free
  list would be faster and would introduce a class of bug (double-issue, cycle) that the scan
  cannot have. Take the scan.
- Iteration is `for (i = 0; i < ENTITY_SLOTS; i++) if (ew->e[i].kind != ENT_NONE)`. Always the
  full 48 whatever `cap` is; the branch is predictable and 48 is nothing.
- `id` is never reused within a session (`next_id++`, wrapping at 16 bits). It exists so
  `tickDue()` can stagger decimated work, and so a future networked version has a handle.
  Slot index is **not** an identity — a slot is reused, an id is not.

**Crucially: an `Entity` holds no pointer to anything.** Not to a `Column`, not to a `Chunk`,
not to a `LightColumn`. Position is float world coordinates. This is the design decision that
makes §3 safe.

---

## §3 Spawning and despawning

### 3.1 The column-unload question, answered first

This is the question most likely to produce a crash or a leak, so it gets answered before the
rules that lead to it.

**What happens to an entity when its column unloads: nothing at all, and that is by
construction.**

- Entities are not stored in `Column`. `worldColumnRemove()` frees a `Column`, its eight
  `Chunk`s and its `LightColumn`; it cannot free entity memory because no entity memory is
  reachable from a `Column`. **No leak is possible** — the array is fixed and freed with the
  process.
- Entities hold no pointers into world memory. **No dangling pointer is possible** — there is
  nothing to dangle.
- The remaining hazard is a *live* entity standing in a column that no longer exists, whose
  next physics step would query blocks that are gone. That is handled by making
  loaded-ness a **despawn condition evaluated at the top of every entity tick**, before any
  physics runs:

```
cx = floor(x / CHUNK_DIM);  cz = floor(z / CHUNK_DIM);
if (worldColumn(w, cx, cz) == NULL) { despawn; continue; }   /* before bodyStep */
```

An entity in an unloaded column is deleted on the tick that notices, and is never stepped
against missing terrain. No hook into the unload path is needed, nothing has to be kept in sync
with `main.c`'s column dropping, and the check is one hash lookup per entity per tick.

**[reasoned]** This trades a small amount of wasted work (an entity can survive up to one tick
— 50 ms — after its column goes) for the removal of an entire class of lifetime bug. On this
hardware that is the right trade, and it is the same posture `budget.h` takes: prefer the
arithmetic ceiling that cannot be wrong over the measurement that can.

It also gives despawn-by-distance for free. The loaded ring is `radius + 1` **[read,
`budget.h`: "main.c's s_area_radius = radius + 1"]**, so an entity that wanders past the loaded
ring hits a NULL column and despawns without any distance constant existing.

### 3.2 Spawn rules

Spawning runs on the 20 TPS tick, at most **one candidate per spawn attempt, with no rejection
loop**. A failed candidate is simply dropped and the next attempt comes on the next spawn tick.
**[reasoned]** — a "keep trying until you find a spot" loop is the standard way this costs an
unpredictable amount of ARM11 time, and unpredictable is worse than occasionally-empty here.

Common gate, checked in this order and cheapest-first:

1. `ew->count < ew->cap` — else return immediately.
2. Pick `(x, z)` from a loaded column inside the drawn radius, at least 8 blocks and at most
   `radius * CHUNK_DIM` blocks from the player.
3. `worldColumn(w, cx, cz)` non-NULL — else drop the candidate.
4. `y = worldStandingY(w, x, z, base)` **[read — this function already exists in `world.h`]**.
5. Per-kind predicate, below.

Animals (v1.8.14):

- The block at `y - 1` must be `BLOCK_GRASS`, `BLOCK_DIRT` or `BLOCK_SAND`.
- `lightGetSky(col, lx, y, lz) >= 9`.
- Spawn a **herd of 2 to 4** of one kind at once, scattered within 3 blocks. One animal alone
  reads as a bug; a small group reads as a world. Herd spawning happens **once per column, at
  column load**, not on a repeating timer — so animals populate as you explore and do not
  accumulate behind you.

Monsters (v1.8.16):

- `lightGetSky(...) == 0 && lightGetBlock(...) == 0` — true darkness.
- The two blocks at `y` and `y + 1` must be air, and `y - 1` solid.
- Spawns on a **repeating timer**, one attempt every 40 ticks (2 s).
- Additional despawn condition, checked on the decimated 2 Hz tick: if the light predicate is
  no longer true where the monster stands, despawn. This is what makes placing a torch clear a
  cave, and it costs one light lookup per monster per half-second.

### 3.3 The caps interact with column load, not with the budget

`cap` is a count of live entities, not of bytes, and it is not connected to `budgetClaim()` in
either direction. A world at radius 5 with 48 entities and a world at radius 5 with 0 entities
consume identical application-heap memory. **[reasoned]** This is deliberate: the world budget
is 88.7% full and the one thing it must never gain is a second, variable claimant.

---

## §4 Movement, physics and pathing

### 4.1 What this is deliberately NOT doing

**No A\*. No navigation mesh. No path caching. No line-of-sight raycast for AI.**

An Old 3DS is a single usable core at 268 MHz **[read, project constraints]**. A single A\*
expansion over a 3D voxel grid with a priority queue, run for even a handful of mobs, is not
affordable at 20 TPS, and the honest version of this plan says so rather than planning it and
discovering it later.

There is also no reason to pay for it. The thing a player reads as "the pig is walking around"
and "the zombie is coming for me" does not require planning. It requires: a heading, forward
motion, and a plausible reaction to hitting something.

### 4.2 The cheap substitute: heading plus obstacle reaction

Four AI states, one `uint8_t`, ticked on the existing `TickClock`:

- **IDLE** — stand still, `ai_timer` counts down, then roll into WANDER.
- **WANDER** — a yaw was chosen once on entry. Walk forward at `0.25 * PLAYER_WALK_SPEED`.
  When `bodyMove()` returns a mask containing `BLOCKED_X` or `BLOCKED_Z`, first try
  `bodyJump()` (which handles the ground gate); if still blocked on the following tick, choose
  a new random yaw. **That is the entire pathfinder.** It climbs one-block steps, it walks
  around walls by bouncing off them, and from outside it is indistinguishable from an animal
  that does not know where it is going — because it is one.
- **FLEE** — animals only, entered on damage. Yaw set once, away from the damage source, run at
  full `PLAYER_WALK_SPEED` for 60 ticks, then IDLE.
- **CHASE** — monsters only. If the player is within 16 blocks, set yaw toward the player every
  decision tick and walk forward. **No line-of-sight test** — a zombie will happily walk into
  the wall between you and it and bounce along it, which is both cheap and, as it happens, what
  the reference game looks like.

### 4.3 Decimation is already written

`tick.h` **[read]** provides the whole thing:

```
period = tickPeriodForDistSq(dx*dx + dz*dz);        /* 1 inside 24 blocks, 10 beyond */
if (!tickDue(clock_count, period, entity->id)) continue;
```

`tickDue`'s `id` argument staggers the decimated set so the 2 Hz group does not all fire on the
same tick — its own comment says that without it "the 2 Hz saving would be a 2 Hz spike instead
of a smooth load". Nothing new is needed here. **This is free, and it is the single biggest CPU
saving available.**

Note that decimation applies to the **AI decision**, not to the physics integration. An entity
on the far tier still integrates, it just integrates once per 10 ticks with a proportionally
larger `dt`. **[reasoned]** — this is where far-away mobs will look slightly wrong (they will
move in visible 500 ms hops), and at 24+ blocks through fog that is not going to be legible.

### 4.4 The collision box problem — a real blocker, stated honestly

`bodyMove()` and `bodyStep()` in `world/physics.c` are exactly what entities want: an
axis-at-a-time AABB sweep, gravity, water handling, a terminal-velocity clamp, and no
`<3ds.h>`. **[read]**

But the AABB dimensions are not in `Body`. They are the compile-time constants `PLAYER_WIDTH
0.6f` and `PLAYER_HEIGHT 1.8f` **[read, `physics.h`]**, so `bodyMove()` is hardwired to the
player's box. `Body` carries only position, velocity, `on_ground` and the water state.

Two options, and this is a decision, not a detail:

**Option A — reuse the player box verbatim in the first phases.** A pig collides as if it were
1.8 m tall and 0.6 m wide. Costs nothing, touches no shared file, works immediately. The visible
symptom is that animals cannot walk under one-block overhangs that they visually fit beneath.
For a pig on open grass this is nearly invisible; for a zombie in a cave it is more noticeable.

**Option B — parameterise the AABB.** Add `float half_w, height;` to `Body`, default them in
`bodyInit()` to the player values, and replace the two constants inside `bodyMove()`'s sweep
with the struct fields. This is a small, surgical change — but it changes a **shared struct in
a host-tested file that both the client and the dedicated server compile**, and `physics.c` is
a file this plan was told to leave alone.

**Recommendation:** ship P0 through P3 on Option A, measure, and then ask before doing Option B.
Option B is the correct end state and should not be done quietly as part of some other phase.

### 4.5 Cost estimate, and why it is only an estimate

**[reasoned]** Per full-rate entity per tick: one `bodyStep()` (three axis sweeps, each
touching a handful of blocks through `worldGet`) plus roughly twenty float operations of AI.
At the New 3DS cap of 48 with, say, 10 inside the 24-block near radius and 38 decimated, the
per-tick load is `10 + 38/10 = ~13.8` full body steps, or ~276 body steps per second at 20 TPS.
The player alone currently performs one `bodyStep` per *render* frame, i.e. ~60/s **[read —
`bodyStep` takes `dt_s` and is driven off the frame, not the tick]**. So this is roughly 4.6x
the game's existing physics cost.

That ratio is **reasoned, not measured**, and it is the number phase P2 exists to replace with
a real one. I did not read `physics.c`'s inner sweep loop and I do not know how many `worldGet`
calls one `bodyMove` actually makes. The estimate could be wrong by a large factor in either
direction. Do not plan the caps around it — measure it.

---

## §5 Rendering

### 5.1 The shader, and the uniform-register accounting

A new `source/shaders/entity.v.pica`. Its declarations and their cost:

```
.fvec projection[4], modelView[4], fogParams[1]     9 registers
.constf consts(0.0, 1.0, 0.0, 0.0)                  1 register
.constf uvScale(...)                                1 register
                                                   --
                                                   11 registers
```

**The register accounting needs a correction to the brief's premise.** **[read, from
`chunk_render.c`'s `pipelineBind()` comment]** the 96-register float bank is **shared hardware,
but each compiled shader has its own register layout**, and only one program is bound at a
time. `pipelineBind()`'s comment states it directly: "the vertex shader's float uniform bank is
shared hardware — a second program's uniforms land in the same registers and quietly overwrite
faceShade". That is precisely why every existing pass re-uploads all of its own uniforms on
every draw rather than relying on anything surviving a `C3D_BindProgram`.

Measured per-shader occupancy **[measured — counted from the `.fvec`/`.constf` declarations in
each `.pica` file]**:

- `world.v.pica` — 76 of 96. Compiled but **never bound**; `world_shbin.h` is included in
  `chunk_render.c` and its DVLB is never parsed. Dead.
- `world_dynamic.v.pica` — **78 of 96, 18 free**. This is the one actually bound, on both
  consoles, since v1.8.0 task 24. `faceShade[64]` is 64 of the 78.
- `highlight.v.pica` — 10 of 96.
- `crack.v.pica` — 9 of 96.
- `sprite.v.pica` — 5 of 96.

So the brief's "77 spent, 19 free" is very close but one register light; the live figure is 78
spent, 18 free, in `world_dynamic`.

**The cost this plan states: 11 registers in the entity program's own bank. Zero registers
taken from `world_dynamic`'s 78.** Register pressure is genuinely not the constraint here. The
real obligation the shared bank imposes is procedural: `entityDraw()` must re-upload
`projection`, `modelView` and `fogParams` on every bind, exactly as `playerModelDraw()` and
`highlightDraw()` already do.

### 5.2 Mesh and vertex format

One shared VBO holding every kind's mesh concatenated, built once at init, with a per-kind
`{first, count}` table — so drawing entity of kind K is
`C3D_DrawArrays(GPU_TRIANGLES, kFirst[K], kCount[K])`. **One draw call per entity.**

```c
typedef struct {
    float   x, y, z;        // 12 bytes, offset 0
    uint8_t u, v, pad0, pad1;  // 4 bytes, offset 12
} EntVertex;                // 16 bytes
```

**PICA attribute alignment** **[read, `mesh_vertex.h`]**: the PICA200 has no per-attribute
offset — offsets are the running sum of preceding attribute sizes — and this project has
already lost releases to a 3-component attribute landing unaligned and freezing real hardware
while emulators shrugged. Here attribute 0 is 3 *floats* = 12 bytes, and 12 is 4-byte aligned,
so attribute 1 starting at offset 12 is safe. A `_Static_assert(offsetof(EntVertex, u) == 12)`
mirroring `mesh_vertex.h`'s own guard is a required deliverable, not an optional nicety.

Geometry, and a deliberate two-step:

- **Phase P3 model: 3 boxes** — body, head, legs-as-one-block. 3 x 12 triangles x 3 = **108
  vertices**, exactly the same count as the existing player model. This is the "cheapest
  convincing thing".
- **Phase P4 model: 6 boxes** — body, head, four separate legs. 216 vertices, minus the four
  downward leg-bottom faces the generator can omit (4 x 6 = 24) = **192 vertices**.

VBO size at the 6-box model, six kinds: `6 x 192 x 16 = 18,432 B`, plus slack for the chicken
being smaller and the zombie taller. Call it **~21 KB of linear heap**, allocated once at init.
Against 18,285,568 B of free linear **[measured]** that is 0.11%.

### 5.3 Texture, TexEnv stages and texture units

**Current usage** **[measured — read out of each pass's TexEnv setup]**:

- Every existing pass uses exactly **2 of 6 TexEnv stages**: stage 0 for material, stage 1 for
  fog. Stages 2 through 5 sit at citro3d's default passthrough. **4 stages free.**
- World pass binds **2 of 3 texture units**: unit 0 = block atlas, unit 1 = fog ramp. 1 free.
- Player-model pass binds **1 of 3**: unit 1 = fog ramp only, because there is no material
  texture at all. 2 free.

**The entity pass costs:**

- **TexEnv stage 0**: `GPU_TEXTURE0` modulated with `GPU_PRIMARY_COLOR` (primary carries a
  per-entity tint so a hurt flash costs no extra stage).
- **TexEnv stage 1**: fog, `GPU_INTERPOLATE(GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1)` — copied
  verbatim from `playerModelDraw()`, same `fogShapeFor(chunkRenderDistance()->boundary,
  FOG_DENSITY)` curve, `GPU_NO_FOG` on the hardware unit.
- **Total: 2 of 6 stages, 4 free. 2 of 3 texture units (unit 0 entity atlas, unit 1 fog ramp),
  1 free.** Identical to the world pass. No new pressure of any kind.

**The entity atlas**: a new `gfx/entatlas.png`, 64 x 256 px, RGBA5551, matching `atlas.t3s`'s
`-f rgba5551 -z auto`. Six 64 x 32 skin regions, one per kind, with two spare. Runtime size
`64 x 256 x 2 = 32,768 B`, imported to **VRAM** via `Tex3DS_TextureImport(..., true)` exactly as
`gfx/atlas.c` does for the block atlas **[read]**. Same 32 KB the block atlas already costs.

### 5.4 The per-frame budget, and the stereo double

The existing proven point **[read — `playermodel.h` asserts it, I did not measure it]**: 15
remote players, 2 draw calls each, 108 vertices each, drawn once per eye with no culling, and
the header claims "this runs on an Old 3DS at 60 FPS". That is **30 draws and 1,620 vertices
per eye**.

The budget this plan commits to:

- Old 3DS, cap 24, P3 model (108 verts): 24 draws and 2,592 verts per eye. Stereo doubles it to
  **48 draws and 5,184 verts per frame**.
- Old 3DS, cap 24, P4 model (192 verts): 24 draws and 4,608 verts per eye — **48 draws and
  9,216 verts per frame** in stereo.
- New 3DS, cap 48, P4 model: 48 draws and 9,216 verts per eye — **96 draws and 18,432 verts per
  frame** in stereo.

Read honestly: the *draw-call* count at the Old 3DS cap (24 per eye) sits **below** the 30 per
eye the player model already claims to survive. The *vertex* count does not — 4,608 per eye is
2.8x the player model's 1,620. And the New 3DS figure is 1.6x the proven draw count and 5.7x
the proven vertex count.

So: **the Old 3DS cap of 24 is defensible from the existing precedent. The New 3DS cap of 48 is
a hypothesis and must be measured in P3 before it ships.** If it misses, the lever to pull first
is the cap, not the model.

### 5.5 Culling — the thing playermodel.c does not do and entities must

Two cheap tests, in this order, per entity per eye:

1. **Distance**, against `chunkRenderDistance()->boundary` squared. Beyond the fog boundary the
   entity is fully fogged out and contributes nothing. One float compare.
2. **Frustum**, via the existing `frustumTestAABB()` in `scene/frustum.c` **[read — it exists
   and is used by `chunk_render.c` for chunk meshes]**. ~20 float ops against 192 vertices of
   transform. Worth it.

Also set `GPU_CULL_BACK` rather than `playermodel.c`'s `GPU_CULL_NONE` — the meshes come from a
generator script (§9) with consistent winding, so back-face culling roughly halves the
rasterised triangles for free. `playermodel.c`'s comment declines this on the grounds that "36
triangles per player against at most fifteen players is not a triangle count worth risking a
hand-derived winding over"; at 48 entities the arithmetic changes, and the winding is not
hand-derived.

**[reasoned]** With culling, the worst case above is only reached when 48 entities are
simultaneously in front of the camera and inside the fog boundary, which is a rare frame rather
than every frame. The budget figures are ceilings, and the measurement in P3 must deliberately
construct that ceiling rather than measuring a typical frame.

---

## §6 Multiplayer

### 6.1 The protocol facts

**[read, `deps/blocksmith-server/proto/bs_proto.h`]**

- `BS_PROTO_VERSION` is **1**. It gates the *transport* packet types (`BS_PKT_HELLO`,
  `BS_PKT_DATA`, ...), enforced on both ends by dropping the datagram before anything else
  looks at it: `bsgate.c` server-side and `bsnet_transport.c`'s `process_datagram()`
  client-side.
- Application messages are `BS_APP_*` opcodes carried **inside** a `BS_PKT_DATA` payload. 15
  are used, `0x01` through `0x0F`. **`0x10` through `0xFF` are free — 241 spare opcodes.**
- **Therefore: adding entity messages does NOT require a `BS_PROTO_VERSION` bump.** The answer
  to "does the protocol need a bump" is **no**.
- But direction matters, and the ratchet is documented in the header itself: a new **S→C**
  opcode is safe to ship client-first, because an old client hits `default: break;` in
  `networldApplyPayload()`. A new **C→S** opcode is **not** — an old server's
  `handle_app_payload()` default case calls `send_kick()`. **Any client-to-server entity
  message forces a server release shipped first.**
- Separately, the client `Makefile` pins `PROTO_COMMIT` to an exact server git commit and the
  `check-proto-drift` target fails the build if the vendored `bs_proto.h` differs. So even an
  additive opcode requires the server-side commit to exist before the client can build.

### 6.2 The decision: v1.8.14 and v1.8.16 ship entities client-local and unsynced

Four reasons, all read from code, and they compound:

1. **The server has no terrain generator and never will.** `diffstore.h`: "Terrain itself is
   never stored... every client generates identical terrain locally, so the server only ever
   needs to remember what players changed." `bsgame.c`: "This process has no terrain generator
   and never will... it holds a seed and a diff store, nothing else." A process that cannot
   answer "is there solid ground at (x, z)" **cannot authoritatively spawn a mob**. Server
   authority is not a design choice here; it is unavailable.
2. **The server is already not authoritative over player position.** `handle_pos_update()`
   decodes five floats and stores them — no bounds check, no speed check, not even a finite
   check. Inventory pickups are documented as "taken on trust, not verified". Entity authority
   would be the *only* authoritative thing in the game, which is a strange place to start.
3. **Any client→server entity traffic forces a coordinated server release, shipped first**
   (§6.1). v1.8.14 already inherits one server-lockstep obligation from v1.8.8 (§7.4). Two is
   worse than one.
4. Deterministic spawning from the shared world seed is *theoretically* possible and would give
   consistent mobs with zero wire cost — but it requires every client to agree on light levels
   and column load order, which they do not, and the moment a mob moves the determinism is gone
   anyway. Not viable.

**The honest cost of this decision, stated plainly:** two players in the same world will see
**different animals**. Player A's pig is not on Player B's screen. If A kills a pig, B sees
nothing happen and B's own pigs are unaffected. Mobs will look like a single-player feature
that happens to run while multiplayer is on. **This is a real and visible inconsistency and the
owner should decide whether it is acceptable before P3, not after.**

### 6.3 If sync is wanted later — the shape and the wire cost

Deferred beyond v1.8.16, sketched here so the decision has numbers attached.

Host election: the connected client with the lowest `sid` owns the mobs. It sends
`BS_APP_ENTITY_SYNC` (C→S, a **new C→S opcode, hence server-first**); the server relays it
verbatim to every other session as a second S→C opcode, adding no logic it cannot support.

Per-entity wire record, 12 bytes:

```
id     u16    2
kind   u8     1
flags  u8     1
x      i16    2    fixed point, 1/16 block
y      i16    2
z      i16    2
yaw    u8     1
pad    u8     1
```

- 48 entities x 12 = 576 B, plus a 2-byte header = **578 B**, comfortably inside
  `BS_MAX_PAYLOAD` (1024) so it never fragments.
- At the existing 10 Hz pose rate: **5,780 B/s** per receiving client.
- For scale **[read]**, the current pose traffic is 25 B x 15 players x 10 Hz = 3,750 B/s.
- **So entity sync roughly doubles downstream traffic on a full server.** On 3DS Wi-Fi that is
  not free, and it is another reason to defer it rather than bundle it into v1.8.14.

---

## §7 Combat, health and drops

### 7.1 What already exists

**[read]** Almost nothing — with one genuinely useful exception.

- There is no health, damage, hunger, drowning, fall damage or death anywhere in `source/`.
  `registry.c`'s cactus comment says it outright: "No damage-on-touch: there is no damage
  system in this build at all."
- There is no `ACTION_ATTACK`. The complete action enum in `app/options.h` is
  `MOVE_FORWARD / BACK / LEFT / RIGHT, JUMP, BREAK (KEY_X), PLACE (KEY_Y), COUNT`.
- **But**: `NetworldPlayerMeters` in `net/networld.h` already decodes `armor[4][2]`, `xp_level`,
  `xp_progress`, `health` (0..20) and `hunger` (0..20) off `BS_APP_PLAYER_STATE`, stores them in
  `s_player_meters`, and exposes them via `networldPlayerMeters()`. The header says the client
  "has no armour, XP, health or hunger systems yet" and that its boundary is "decodes bytes, not
  knows what health means".

**So the health scale is already decided by the protocol: 0..20.** Use it for entities too, so
that if a survival system ever lands, nothing has to be rescaled.

### 7.2 Attacking, without adding an input

The 3DS has no mouse and the action enum is full of things that mean something. Adding
`ACTION_ATTACK` would touch the remap table and the on-disk options format — a scope decision,
not a detail.

**Recommendation: reuse `ACTION_BREAK`.** On a *fresh press* of BREAK, if an entity is nearer
along the crosshair ray than the block `worldRaycast()` found, the press damages the entity and
does not begin a block break. Held BREAK continues to work exactly as it does today when
nothing is in the way. No new input, no options-format change, no remap-UI change.

### 7.3 The raycast — do not touch `raycast.c`

**[read]** `worldRaycast()` is pure voxel DDA. Its `RayHit` (`hit, x, y, z, face, px, py, pz,
distance`) has fields that only make sense for a block cell. It is shared and host-tested.
Extending it to also hit AABBs would mean interleaving a second hit test into the DDA walk and
widening `RayHit` to distinguish two kinds of hit.

**Do not do that.** Instead add, in `world/entity.c`:

```c
int entityRaycast(const EntityWorld* ew, float ox, float oy, float oz,
                  float dx, float dy, float dz, float max_dist, float* out_dist);
```

— a plain ray-vs-AABB slab test over at most 48 boxes, returning a slot index or -1. Then
`interact.c` compares `out_dist` against `RayHit.distance` and takes whichever is nearer.

Two things this buys: `raycast.c` stays untouched and its tests stay meaningful, and
`entityRaycast()` is independently host-testable because it needs no `World` at all.

### 7.4 Drops — and the v1.8.8 dependency, which is load-bearing

The existing drop path **[read]**: `breakComplete()` in `interact.c` sets `it->broke_id`;
`main.c` reads it immediately after `interactEdit()` returns and calls `invBridgeAdd(&s_inv,
it.broke_id, 1, NULL)`, which calls `inventoryAdd()` and reports `BS_INV_OP_PICKUP` to the
server. **There is no dropped-item entity — a mined block teleports into the bag in the same
frame.**

Entity drops use the same mechanism: on death, call `invBridgeAdd(&s_inv, ITEM_MEAT, 1, NULL)`.
No floor item, no pickup radius, no third entity kind. Consistent with the rest of the game and
costs nothing.

**The blocker.** `world/inventory.h`:

```c
static inline bool inventoryCanHold(ItemId item)
{
	return item != ITEM_NONE && (uint32_t)item < BLOCK_COUNT;
}
```

`BLOCK_COUNT` is 8, static-asserted **[read]**. And `ItemId` *is* `BlockId` — `inventory.h`
says "There is no item registry distinct from the block registry". There is no item id space at
all; meat has to become a registry row, and the predicate has to widen past 8.

That is the v1.8.8 fix, and **v1.8.14 cannot ship without it**. Three consequences worth
knowing before v1.8.8 is designed:

1. Meat needs a **core** registry row (`REG_ID_CORE_LO..HI` = `0x01..0x7F`), not a dynamic one
   (`0x80..0xFD`) — dynamic rows are server-registered at join and a single-player world has no
   server.
2. **A core registry addition forces a matching server release, shipped first or
   simultaneously, and the protocol does not and cannot enforce it.** `networld.c` is explicit:
   the registry CRC moves while `BS_PROTO_VERSION` stays at 1, so a mismatched pair connects
   happily and then degrades near-invisibly. v1.8.1 already hit this (CRC `0x72A8` → `0x4066`).
   **So v1.8.14 inherits a server-lockstep obligation from v1.8.8.**
3. Neither the disk format nor the wire format breaks: both store the item id as a single byte
   (`inventory.dat` is `(u8 item, u8 count)` per slot; `BS_INV_ACTION` passes byte-sized
   parameters), so widening the *predicate* changes no offsets. `interact.c`'s own comment
   states the sanctioned rule: **"widen the PREDICATE, never the constant."**

Also **[read]**: the hotbar draws an item as `atlasTile(blockFaceTex(id, FACE_TOP))` — there is
no separate item-icon table. So meat needs a tile in the block atlas. The atlas has 64 slots
and 17 are painted **[measured]**, so 47 are free. Cheap.

### 7.5 Damage numbers

**[assumed — these are design starting points, not derived from anything]**

- Player hit deals 4 (2 hearts on the 0..20 scale), with a 10-tick cooldown per entity.
- Knockback: `vx`/`vz` set to 6.0 blocks/s away from the attacker, `vy += 4.0`.
- Health: pig / sheep 10, cow 10, chicken 4, zombie 20, skeleton 20.
- Damage triggers FLEE in animals and does not change monster state.
- `hurt_flash` in `flags` tints the primary colour red for 5 ticks — free, since primary colour
  is already modulated in TexEnv stage 0 (§5.3).

**Monsters damaging the player requires a player health system that does not exist.** That is
new work belonging to v1.8.16, it must be local-only (the meters are S→C and the server does
not compute them), and it should be scoped and asked about rather than absorbed silently.

---

## §8 The animals and the monsters

Shared: struct, store, spawn machinery, physics, renderer, atlas, drop path. **Everything
except the rows in a kind table and one spawn predicate.**

The kind table, all `const`, no per-kind code:

```c
typedef struct {
    const char* name;
    uint16_t    mesh_first, mesh_count;   // into the shared VBO
    uint8_t     health;
    uint8_t     drop_item;
    uint8_t     drop_count;
    uint8_t     ai_class;                 // AI_PASSIVE or AI_HOSTILE
    float       speed_mul;
} EntityKind;
```

**v1.8.14 Animals** — pig, cow, chicken, sheep. `AI_PASSIVE`. Spawn on grass/dirt/sand with
sky light >= 9, as a herd of 2 to 4, **once at column load**. FLEE on damage, never target the
player, never despawn for light. Drop meat.

**v1.8.16 Monsters** — zombie, skeleton. What actually differs beyond the model:

- **Spawn predicate**: `lightGetSky == 0 && lightGetBlock == 0`. Repeating 2 s timer rather
  than once-per-column.
- **Light despawn**: checked on the 2 Hz decimated tick; a torch clears a cave.
- **AI class `AI_HOSTILE`**: adds the CHASE state, entered when the player is within 16 blocks.
- **Ranged attack for the skeleton.** An arrow would be a second entity kind, a second mesh,
  its own spawn/despawn lifetime and its own collision — a meaningful fraction of this whole
  subsystem for one enemy. **Recommend a hitscan with a visible wind-up instead**: the skeleton
  stops, plays a 20-tick telegraph via `hurt_flash`-style tint, then applies damage directly if
  the player is still within range. No projectile entity, no new mesh, and the telegraph is
  what makes it dodgeable, which is the part that matters to a player.
- Chickens get `PLAYER_TERMINAL` softened so they flutter down instead of falling. One float,
  and it is the single cheapest piece of character in the whole plan.

**A gap that belongs here, not in §11 only.** The ROADMAP says monsters spawn "in the dark and
in caves". **[measured]** A grep of `source/` for `daynight|day_night|time_of_day|daytime|
sun_angle` returns **zero header hits** — there is no day/night clock. `world_dynamic.v.pica`
does declare a `dayLevel[1]` uniform **[read]**, but I did not establish that anything drives
it from a clock. **Without a day/night cycle, only the cave half of "in the dark and in caves"
is implementable**, and surface night spawning would require a day/night system that no planned
version currently contains.

---

## §9 Art plan — scripts, not files

Nothing is borrowed. Minecraft is a visual reference only. Every asset below is output by a
Python script in `tools/`, in the house style the existing scripts already establish.

**[measured]** The house style, read from `tools/make_atlas.py`, `make_crack_atlas.py`,
`make_font.py`, `make_fog_ramp.py`: Pillow (`Image`, `ImageDraw`) plus `pathlib`, `random`,
`math`. No numpy. **No shared helper module — every script is self-contained.** Drawing is
per-pixel via `img.load()` loops, driven by a fixed-seed `random.Random(...)` so every rebuild
is byte-identical. Representative: `tile_ice()` in `make_atlas.py` seeds five Voronoi points,
assigns nearest-seed ownership per pixel, then draws fracture lines at the boundaries.

**[measured]** The pipeline: `tools/make_*.py` writes `gfx/*.png`; `tex3ds` reads the matching
`gfx/*.t3s` flags file and emits `build/*.t3x`; `bin2o` embeds that into the ELF;
`Tex3DS_TextureImport()` decodes it at runtime into a `C3D_Tex`. Every existing sheet has an
**explicit** `foo.t3x: foo.png` Makefile line, working around a documented `.d`-file stem
collision with the same-named `.c`.

### 9.1 `tools/make_entity_atlas.py` (new)

Writes `gfx/entatlas.png`, 64 x 256 RGBA. Six 64 x 32 skin regions, two spare. Fixed seed.

- `skin_pig()` — pink base `(240,160,170)`, seeded per-pixel value jitter of ±8 so it is not a
  flat plane of colour, a darker snout rectangle with two nostril pixels, two black eye pixels,
  darker hoof bands at the leg-bottom rows.
- `skin_cow()` — white base with 3 to 5 seeded irregular black blobs grown by a small random
  walk from seed points (the same technique `make_crack_atlas.py` uses for cracks), pink muzzle,
  two horn pixels on the head-top face.
- `skin_chicken()` — off-white base, orange beak and feet blocks, a red wattle of 3 pixels,
  seeded feather-direction dither of ±6 in vertical runs (the run-length "grain" technique
  `make_atlas.py` uses for wood).
- `skin_sheep()` — cream fleece drawn as clustered 2x2 blobs at seeded offsets so the surface
  reads as wool rather than paint, darker bare face.
- `skin_zombie()` — desaturated green base, darker torn rectangles on the torso, two black eye
  sockets, a lighter "shirt" band across the body-front face.
- `skin_skeleton()` — light grey base, dark rib lines as 1-pixel horizontal runs at seeded
  vertical spacing, two black eye sockets, darker joint bands.

### 9.2 `gfx/entatlas.t3s` (new)

One line, matching `atlas.t3s`: `-f rgba5551 -z auto entatlas.png`.

### 9.3 `tools/make_entity_meshes.py` (new)

Generates `source/world/entity_meshes.h` — a `const EntVertex` table plus the per-kind
`{first, count}` rows. **Generated rather than hand-written so the geometry and its UV layout
come from one source and cannot drift**, which is the same reasoning `atlas_tiles.h` and
`atlas_uv.h` already embody.

Core helper: `box(cx, cy, cz, w, h, d, uv_origin)` emitting 36 vertices with a standard
six-face box unwrap, consistent winding for `GPU_CULL_BACK`, and an option to omit the bottom
face (used for legs). Per kind, a small list of `box()` calls with block-fraction dimensions —
a pig is body `0.6 x 0.5 x 0.9`, head `0.5 x 0.5 x 0.5`, four legs `0.15 x 0.4 x 0.15`. **This
is the script that makes "a handful of boxes" concrete, and it is where the whole art budget
for this subsystem lives.**

### 9.4 One tile added to `tools/make_atlas.py`

A `tile_meat()` entry appended to the existing `TILES` list, taking slot 17 of 64. Raw-meat
red-pink with a seeded marbling of lighter pixels and a pale fat edge, in the same per-pixel
style as its neighbours. **[scoped change — `make_atlas.py` and the generated `atlas_tiles.h`
are shared; this is an append, but it must be an append.]**

### 9.5 Makefile

One new explicit rule, mirroring the four that already exist:

```
$(GFXBUILD)/entatlas.t3x : $(CURDIR)/gfx/entatlas.png
```

**[scoped change — the Makefile is shared with six concurrent agents. This line must be added
deliberately and in isolation, not folded into another edit.]**

---

## §10 Phase order, and how each phase is proven

The project rule is that a fix is not done until proven by running something and reading real
output, and that every check must be demonstrated able to go red. Each phase below names its
success criterion, its check, and its **sabotage arm** — the deliberate breakage that must turn
the check red, proving the check could have failed.

`tools/run_host_tests.sh` already documents this practice at length for prior tasks, including
md5-verified restores after each sabotage. Follow that file's existing form.

### P0 — the store, headless

`world/entity.h` + `entity.c`, no `<3ds.h>`: struct, `EntityWorld`, spawn, despawn, slot scan,
id issue, and the column-NULL despawn rule from §3.1. New `tests/entity_test.c`, added to
`run_host_tests.sh`.

- **Criterion**: `sizeof(Entity) == 36` on the target ABI; no slot is ever double-issued; an
  entity in a removed column is gone by the next tick; occupancy never exceeds `cap`.
- **Check**: host binary, assertion count printed.
- **Red arms**: (1) make the column-NULL despawn a no-op — the "gone after `worldColumnRemove`"
  assertion must fail; (2) let the slot scan return an occupied slot — the unique-id assertion
  must fail; (3) remove the `cap` guard — the occupancy assertion must fail.
- **This is the phase that prevents the crash**, and it costs nothing on hardware. Do it first.

### P1 — spawning against real generated terrain, still headless

Spawn selection wired to `worldStandingY()` and the light predicates, run against the host
suite's existing world generation.

- **Criterion, stated as a rate over a distribution, not a single sample**: over **at least 500
  spawn attempts** across varied seeds, 100% of accepted spawns stand on a solid block with air
  above, 0% land inside solid geometry, 0% land in an unloaded column, and the monster
  predicate accepts 0% of surface daylight samples.
- **Why a rate**: a green check on one seeded spawn says one draw passed. This project has
  already had a 47-of-47 pass conceal 28% of a population failing. Assert the rate.
- **Red arm**: invert the light comparison — the daylight assertion must go red, and the report
  must name a *percentage*, not "a failure".

### P2 — movement, measured on hardware

`entityTick()` driving `bodyStep()`, using the player AABB (§4.4 Option A). Decimation via
`tickPeriodForDistSq()` / `tickDue()`.

- **Host criterion**: 48 entities x 10,000 ticks, none escapes the world AABB, none reaches NaN,
  a wall-blocked entity reorients within 20 ticks.
- **Hardware criterion**: a debug-menu counter for microseconds spent inside `entityTick()` per
  tick. **`entityTick` <= 2.5 ms at the Old 3DS cap of 24 with all 24 forced inside the 24-block
  near radius** — 5% of the 50 ms tick period. Read off the bottom screen on a real Old 3DS.
- **Red arm for the counter itself**: raise the cap to 200 and confirm the number goes over 2.5
  ms. A counter that cannot move is not an instrument.
- **This phase replaces §4.5's reasoned 4.6x estimate with a real number.** If it misses, lower
  `ENTITY_CAP_OLD` before doing anything clever.

### P3 — the cheapest convincing render: flat-colour pigs

Clone `playermodel.c`'s structure for a 3-box pig in a flat TEV constant. No texture, no new
shader, no atlas. Distance + frustum cull, `GPU_CULL_BACK`. Drawn from inside `drawEye()`.

- **Criterion**: pigs visibly wander on a real Old 3DS, **with the 3D slider up**, holding 60
  FPS at the cap of 24, with the worst case deliberately constructed (all 24 in frustum, inside
  the fog boundary).
- **Check**: look at it, and read the frame-time counter. The project's own rule applies —
  a compiling, test-passing diff has already rendered garbage on this codebase; if the claim is
  visual, look at it.
- **Then measure the New 3DS cap of 48.** It is a hypothesis (§5.4). Lower it if it misses.
- **This is the demo.** Everything after this is refinement of something already convincing.

### P4 — texture and the real shader

`entity.v.pica`, `make_entity_atlas.py`, `make_entity_meshes.py`, the 6-box model, the `.t3s`
and the Makefile rule.

- **Do the texture probe FIRST**: render `entatlas` at UV 0..1 on a full-screen quad and look at
  it, before trusting a single model UV. A wrong texture constant on the PICA200 still renders
  *a* texture — it reads as bad art, never as an error, and this project has lost time to
  exactly that.
- **Criterion**: the atlas reads correctly at 0..1; then each of the six skins appears on the
  right kind, right way up, with no bleed at region edges.
- **Red arm**: offset the UV scale by one tile and confirm the check notices. A visual check
  that passes against a deliberately wrong UV is not a check.

### P5 — combat, health and drops

Blocked on v1.8.8's `inventoryCanHold()` widening (§7.4). `entityRaycast()`, the BREAK-press
branch in `interact.c`, damage, knockback, death, `invBridgeAdd`.

- **Criterion**: killing a pig puts exactly one meat in the bag; `entityRaycast` beats
  `worldRaycast` only when genuinely nearer.
- **Check**: host test for `entityRaycast` (needs no `World`); host test for the drop landing in
  a real `Inventory`.
- **Red arm**: revert the predicate widening — the drop test must go red with "meat refused",
  not silently drop nothing.
- **Re-read `interact.c` at implementation time.** It is under concurrent edit and the
  insertion point described in §7.2 was established through a summary, not read line by line.

### P6 — v1.8.16 monsters

Kind-table rows, the CHASE state, the darkness predicate, the light despawn, the skeleton
telegraph. Almost entirely data on top of P0–P5.

- **Criterion**: monsters appear only where both light channels are 0; placing a torch despawns
  them within one second; the New 3DS cap holds with mixed animals and monsters.
- **Red arm**: relax the predicate to sky-only and confirm monsters appear in shaded surface
  spots the check must reject.

### P7 — multiplayer sync (deferred, needs a decision)

Not part of v1.8.14 or v1.8.16. Requires a new C→S opcode and therefore a server release shipped
first (§6.1), and roughly doubles downstream traffic (§6.3). **Do not start this without asking.**

---

## §11 What I could not determine, and what each gap means

1. **Whether anything drives a day/night clock.** `world_dynamic.v.pica` declares a
   `dayLevel[1]` uniform **[read]**, but a grep of `source/` for `daynight`, `day_night`,
   `time_of_day`, `daytime` and `sun_angle` returns zero header hits **[measured]**, and I did
   not trace what writes that uniform. **Consequence:** if there is no clock, v1.8.16's "spawn
   in the dark" is **cave-only**, and surface night spawning needs a day/night system that no
   planned version contains. This should be settled before v1.8.16 is scoped.

2. **Total VRAM in use.** The block atlas is 32,768 B in VRAM **[measured — 16 x 1024 RGBA5551,
   `Tex3DS_TextureImport(..., true)`]**, but I did not sum the crack atlas, font, fog ramp and
   render targets against the console's VRAM. **Consequence:** a 32 KB entity atlas is very
   probably fine against 6 MB, but "probably" is not a measurement, and VRAM exhaustion on this
   hardware surfaces as a texture that silently renders as garbage rather than as an error.
   Check the total before P4 ships.

3. **Whether 48 entities at 1 draw call each holds 60 FPS.** My only precedent is
   `playermodel.h`'s *claim* that 15 players at 2 draws each runs at 60 FPS on an Old 3DS — an
   author's assertion in a comment, not a measurement I read or made. **Consequence:**
   `ENTITY_CAP_NEW = 48` is a hypothesis, not a budget. P3 must measure it against a
   deliberately constructed worst case and lower it if it misses. `ENTITY_CAP_OLD = 24` is
   better supported (it is below the claimed precedent's draw count) but is still unmeasured.

4. **The real cost of `bodyMove()` at 24 concurrent bodies.** I read its shape from `physics.h`
   (three axis sweeps, AABB, `BLOCKED_*` mask) but did **not** read `physics.c`'s inner loop and
   do not know how many `worldGet()` calls one sweep makes. **Consequence:** §4.5's "roughly
   4.6x the game's existing physics cost" could be wrong by a large factor either way. It is an
   estimate to be replaced by P2's measurement, not a figure to design caps around.

5. **The exact insertion point in `interact.c` for the entity-hit branch.** `interactEdit()`'s
   structure and the `breakProgress` / `breakComplete` split came to me through a summary; I did
   not read the function bodies myself, and `interact.c` is under concurrent edit right now.
   **Consequence:** §7.2's "compare the two distances and take the nearer" is sound as a design
   but its placement must be re-derived from the file at implementation time. Stale
   `file:line` reasoning has already produced one wrong conclusion on this project.

6. **`sizeof(Entity)` on the target ABI.** The 36 bytes in §2.3 is hand arithmetic, not read off
   `arm-none-eabi-gcc`. The struct deliberately avoids enums so `-fshort-enums` cannot bite (it
   already made host `sizeof()` lie about `Chunk`), but the number is unverified.
   **Consequence:** small — the total is 1,728 B either way and nothing depends on the exact
   figure — but the `_Static_assert` in P0 is not optional, because this document quotes the
   number as if it were known.

7. **Whether `budgetClaim()` should be involved at all.** This plan deliberately keeps entities
   out of the world budget by putting them in BSS (§2.4). I did not check whether the
   bottom-screen memory report would then under-report total usage by 1,744 B.
   **Consequence:** trivial in bytes, but if the owner wants one number that covers everything,
   the report needs a separate entity line rather than a `budgetClaim()` call — do not solve it
   by charging the 88.7%-full world budget.
