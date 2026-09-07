# Blueprint v1.9.2 — Redstone

Written 2026-09-06 from the v1.9.2 architect agent's report (the agent ran in
plan mode and could not write files; this is its text, saved by the parent).
Reads assume the tree at branch v1.9.0 after the chest wiring
(BLOCK_CHEST = 43, registry 44 rows, REGISTRY_FULL_COUNT_PIN 170,
TILE_USED_COUNT 49, BLOCKSTATE_SLOTS 256).

> **Corrections, 2026-09-06 22:18, applied by the parent after the v1.9.3
> architect cross-checked this document against the code.** Three of the
> baseline facts above were wrong as originally written. Each was re-verified
> against the tree before this note was added; the source line is given so it
> can be checked again rather than taken on trust.
>
> 1. **`TILE_USED_COUNT` is 49, not 54** — `source/gfx/atlas_tiles.h:139`.
>    Every "54 → 59" in §1 is therefore **49 → 54**. The *conclusion* survives:
>    five tiles (58..62) are free, this version claims all five, and the atlas
>    is full afterwards. But anyone deriving a free-slot count from 54 is off
>    by five and will think there is headroom that does not exist.
> 2. **`REGISTRY_FULL_COUNT_PIN` does not live in `registry.h`.** It is
>    `source/world/registry_test.c:60`, currently `170`. A grep of the headers
>    finds nothing and reads as "this constant does not exist"; it does, and
>    moving it is a test-file edit, not a header edit.
> 3. **There is no tick scheduler to register with.** `source/world/tick.h`
>    exposes a bare clock — `tickClockInit/Advance/UntilNextUs/Count/Dropped`,
>    `tickPeriodForDistSq`, and `tickDue(tick, period, id)` — and callers
>    *poll* `tickDue()` with a hand-chosen id. Nothing registers, so §2's
>    "registered with the existing tick scheduler under `REDSTONE_TICK_ID`"
>    and §7 step 4's "scheduler registration" describe an API that is not
>    there. Redstone must call `tickDue(tick, REDSTONE_PERIOD_TICKS,
>    REDSTONE_TICK_ID)` from the main loop like every other consumer, OR the
>    lane must build the registration API first and say so as scope. **That is
>    a decision, not a detail** — it changes the work estimate.
>
> Not corrected here because it is outside this document: `docs/ROADMAP.md`
> claims lava shipped in v1.8.11. It did not — `source/world/cave_carve.h:24`
> records lava as deliberately out of scope, and there is no other reference
> in the tree.

## 0. Scope

Components: redstone dust (wire), redstone torch, lever, button, pressure
plate, redstone lamp, door (2-high, 4 facings, open/closed), piston (6 faces,
base + extended arm). Power 0–15 with the usual dust attenuation; torches
invert; buttons time out; plates read the player standing on them; doors and
lamps and pistons are consumers.

Non-goals for 1.9.2: repeaters, comparators, sticky pistons, moving blocks
through the world (a piston pushes at most ONE block one cell, and only a
block with no state), hoppers, note blocks. Listed in ROADMAP as 1.9.x
follow-ups.

## 1. Block ids and registry (gate 0 — Agent A, server-first release)

Ids append after `BLOCK_CHEST = 43`:

| id | name | notes |
|---|---|---|
| 44 | `BLOCK_REDSTONE_WIRE_OFF` | placed by the redstone item |
| 45 | `BLOCK_REDSTONE_WIRE_ON` | |
| 46 | `BLOCK_REDSTONE_TORCH_ON` | this is the ITEM too (torch places ON) |
| 47 | `BLOCK_REDSTONE_TORCH_OFF` | |
| 48 | `BLOCK_LEVER_OFF` | |
| 49 | `BLOCK_LEVER_ON` | |
| 50 | `BLOCK_BUTTON_OFF` | |
| 51 | `BLOCK_BUTTON_ON` | |
| 52 | `BLOCK_PLATE_OFF` | |
| 53 | `BLOCK_PLATE_ON` | |
| 54 | `BLOCK_LAMP_OFF` | |
| 55 | `BLOCK_LAMP_ON` | |
| 56..71 | `BLOCK_DOOR_BASE = 56` | `id = 56 + facing*4 + open*2 + upper`; facing 0..3 = E, W, S, N |
| 72..77 | `BLOCK_PISTON_BASE = 72 + face` | face 0..5 = +X, -X, +Y, -Y, +Z, -Z |
| 78..83 | `BLOCK_PISTON_ARM_BASE = 78 + face` | the extended arm cell |

- `registry.c`: 44 → 84 rows. `REGISTRY_FULL_COUNT_PIN 170 → 210` (the pin
  counts rows plus item rows; the exact number is whatever the test computes,
  pin it after the first green run and record it). The registry crc moves, so
  the SERVER MUST RELEASE FIRST (registry lockstep). `registry_test.c` gets the
  new rows and the new pin.
- Shapes: `BLOCK_SHAPE_FLAT = 2` (wire, plate — 1/16 high, no collision),
  `BLOCK_SHAPE_BOX = 3` (torch, lever, button — small box, no collision). Doors
  and pistons keep `BLOCK_SHAPE_FULL` for collision (the fine mesh only changes
  what is DRAWN).
- Tiles 58..62 in `atlas_tiles.h`: `BTEX_REDSTONE_WIRE`, `BTEX_REDSTONE_TORCH`,
  `BTEX_LAMP`, `BTEX_DOOR_LOWER`, `BTEX_DOOR_UPPER`. `TILE_USED_COUNT 54 → 59`.
  Lever, button, plate and piston reuse existing tiles (cobble, planks,
  piston = planks side + stone face). The atlas is FULL after this: 59 of 64
  tiles; 1.9.3 gets the last five, then the atlas must grow (256 → 512, a
  6 MB VRAM budget question — flag in ROADMAP).
- OFF states are drawn with the same tile through tint row 7,
  `MESH_TINT_ROW_UNPOWERED = {0.35, 0.35, 0.35}` (mesher tint rows 7 → 8;
  `biome_tint_test.c` pins the row). No extra tiles for OFF.
- `block.h` helpers (all pure, all host-tested in `registry_test.c` /
  `block_tiles_check.c`): `blockIsRedstone(id)`, `blockItemOf(id)` (what
  breaking it awards: WIRE_ON → WIRE_OFF item, TORCH_OFF → TORCH_ON, LEVER_ON →
  LEVER_OFF, door parts → the door item = lower closed E, piston arm → nothing,
  piston base any face → PISTON_BASE+0), `blockTintOverride(id)` (returns
  `MESH_TINT_ROW_UNPOWERED` for the *_OFF ids and the closed door, else the
  biome row), `blockIsDoor(id)`, `doorFacing(id)`, `doorOpen(id)`,
  `doorUpper(id)`, `doorId(facing, open, upper)`, `blockIsPistonBase(id)`,
  `blockIsPistonArm(id)`, `pistonFacing(id)`.
- Crafting (`crafting.c`, `RECIPE_COUNT 7 → 15`): redstone dust ×1 → wire item
  (1:1, dust is mined from a redstone ore that already exists? NO — it does not;
  add `BLOCK_REDSTONE_ORE` is a 1.9.3 job; for 1.9.2 the wire item is crafted
  from 1 coal + 1 stone as a placeholder recipe and ROADMAP records the ore
  as a follow-up), torch = stick + wire, lever = stick + cobble, button =
  stone, plate = 2 planks, lamp = glowstone-equivalent (torch + glass; glass
  exists), door = 6 planks → 3 doors, piston = 3 planks + 4 cobble + 1 wire.
  `crafting_test.c` pins the eight recipes.
- `tools/make_atlas.py` + `gfx/atlas.png`: add the five tiles (16×16 px
  each, hand-drawn in the script's existing style — flat colour + noise, no
  external asset). `block_tiles_check.c` renders every used tile at UV 0..1
  and compares against the expected id; look at the sheet (visual claims need
  visual probes).

## 2. Simulation (Agent B — `source/world/redstone.c/.h`)

- Power 0–15 is TRANSIENT per component, recomputed every redstone tick by a
  fixpoint over the component set (order-independent: iterate until no cell's
  power changes, bounded by `REDSTONE_TICK_BUDGET 2048` cell visits per tick;
  over budget → stop and log once, never freeze). Only ON/OFF persists, in the
  block id. Save format unchanged: the world already stores ids.
- Redstone tick = every `REDSTONE_PERIOD_TICKS 2` game ticks (10 Hz at the
  20 Hz game tick), registered with the existing tick scheduler under
  `REDSTONE_TICK_ID 0x52445354u` ("RDST").
- Component set: `REDSTONE_COMPONENT_MAX 1024` entries of
  `{int16_t x, y, z; uint8_t id_cached; uint8_t power; uint8_t timer;}`
  (8 B) in a static `Redstone` struct with a hash by (x,y,z) → ~22.3 KB .bss,
  `_Static_assert(sizeof(Redstone) <= 32768)`. ZERO bytes from the world
  budget. Registration happens on `redstoneNotify(x,y,z, prev, now)` (called
  by every block edit: placement, break, remote edit, worldgen NOT — worldgen
  places no redstone in 1.9.2) and on chunk load by a scan of the loaded
  chunk for redstone ids (`redstoneScanChunk`).
- Consumers lag one redstone tick behind sources (lamp/door/piston read
  power computed in the previous tick) — this is what makes the fixpoint
  order-independent and is the documented behaviour.
- Sources: torch = 15 unless its attached block is powered (then OFF, 0);
  lever = 15 when ON; button = 15 for `REDSTONE_BUTTON_TICKS 20` game ticks
  after use, then OFF; plate = 15 while an entity AABB overlaps the cell
  (`redstoneBodyAt(x,y,z)` is fed by the player and animal positions each
  tick from main.c — seam 6).
- Wire: power = max(neighbour source power, neighbour wire power − 1); wire
  connects to the four horizontal neighbours and diagonally up/down one cell
  (Minecraft rule), never through a solid block.
- Consumers: lamp ON iff power > 0; door open iff power > 0 (both halves
  change together; a door opened by hand stays open until power changes —
  hand toggle sets a "manual" bit in the timer byte); piston extends iff
  power > 0: the arm id is written into the cell in front if that cell is air
  (or the block there is pushed one cell further if THAT is air and the block
  has no state — chests are never pushed), retracts when unpowered by
  restoring air (the pushed block is NOT pulled back — no sticky).
- API: `redstoneInit`, `redstoneNotify(x,y,z, prev, now)`, `redstoneTick`
  (called by the scheduler), `redstoneUse(x,y,z)` (lever/button/door
  interaction; returns true if it handled it), `redstoneBodyAt(x,y,z)`,
  `redstoneDropColumn(cx, cz)` (chunk unload), `redstonePending()` (count of
  dirty components — used by the debug HUD), `redstonePowered(x,y,z)`,
  `redstonePlaceAllowed(id, x,y,z)` (wire/torch/lever/button/plate need a
  solid block below or behind; doors need two air cells and a solid floor;
  piston any), `redstonePlaceId(item, face, yaw, pitch)` → the concrete id for
  facing-dependent items (door facing from yaw, piston face from the aim
  face, torch/lever attach face — 1.9.2 draws torches/levers as upright only;
  the attach side is not encoded in the id).
- Change callback: `typedef void (*RedstoneChangeFn)(void* ud, int x, int y,
  int z, BlockId prev, BlockId now);` set by main.c to the same path a player
  edit takes (world write + mesh dirty + network BLOCK_EDIT when the local
  player is the actor — see §5).
- Tests (`redstone_test.c`, host): wire attenuation 15 → 0 over 16 cells,
  torch inversion, button timeout at exactly 20 ticks, fixpoint under a loop
  (torch feeding its own base through wire = a clock; must oscillate at the
  redstone period, never spin), budget cut-off logs once and leaves state
  consistent, door halves move together, piston push/no-push (chest not
  pushed), component table full → placement refused (not a crash),
  order-independence (permute the component order, same result).

## 3. Rendering (Agent C — `finemesh.c/.h`, `mesher.c/.h`, `chunk_render.c/.h`)

- Fine mesh: a second, small mesh per chunk for the non-cube blocks, in
  EIGHTHS of a block: `FineVertex {uint8_t x, y, z; int8_t pad; uint8_t u, v,
  nrm, ao;}` (8 B), positions 0..128 relative to the chunk origin, drawn with
  `modelView × Scale(1/8)` using the existing world shader (the scale goes in
  the model matrix; the shader is unchanged — no new shbin).
- Pool: `FINE_SLOTS 48 × FINE_SLOT_VERTS 1024 × 8 B = 393,216 B` linear
  memory, allocated once at boot from the same linear pool as the chunk
  meshes (`mesh_pool_sizing.h` gains the constant; `tests/mesh_pool_bytes_test.c`
  pins the total against the 12 MB budget: measured number goes in the test).
  A chunk with no fine blocks holds no slot. A chunk whose fine geometry
  exceeds 1024 verts draws the first 1024 and counts the overflow (debug HUD).
- Geometry: wire = a flat quad 1/16 high following the connection mask (a
  cross when connected on all four sides, straight when two opposite); torch =
  a 2×2×8 post; lever = base plate + tilted stick; button = 2×4×1 box;
  plate = 14×14×1 quad; lamp and piston base = full cubes in the NORMAL mesh
  (not fine); piston arm = a 4×4×12 rod + 16×16×4 head; door = a 16×2×16
  panel per half, rotated by facing and open.
- `mesher.c/.h`: two small edits — (1) skip FLAT/BOX/door/piston-arm ids in
  the cube pass (they are "air" to face culling of neighbours), (2) the tint
  row override via `blockTintOverride`. Nothing else in the mesher changes;
  `biome_tint_test.c` pins the new row.
- `chunk_render.c/.h`: after each chunk's opaque draw, bind the fine slot
  (same program, same attribute layout — 3 × u8 position + pad, 2 × u8 uv,
  u8 normal index, u8 ao; PICA attribute alignment: keep the 4-byte position
  group BEFORE the 4-byte uv/nrm/ao group, both 4-aligned) and draw it. A
  runtime debug toggle (`chunkRenderSetFineEnabled`) is wired to the debug
  HUD so a GPU hang can be bisected on hardware in one boot.
- THIS IS THE RISKIEST PART OF 1.9.2 (GPU-hang class). Gate: the fine pass
  ships behind the toggle defaulting ON only after an Azahar boot shows the
  geometry AND a second boot with the toggle OFF shows the same frame time.

## 4. Interaction (Agent D — `interact.c/.h`, `networld.c/.h`)

- `interact.h` gains `typedef bool (*InteractPlaceResolveFn)(void* ud, int
  x, int y, int z, ItemId item, int face, float yaw, float pitch, BlockId*
  out_id);` and `void interactSetPlaceResolveFn(Interact*,
  InteractPlaceResolveFn, void*)`; fields `placed_item`, `aim_yaw`,
  `aim_pitch`. Placement asks the resolver first (main.c sets it to
  `redstonePlaceId` + `redstonePlaceAllowed`); a refused placement is not an
  edit. Use (A on a lever/button/door) goes to `redstoneUse` before the
  generic interact.
- Break award uses `blockItemOf(here)` (already the convention for chests;
  extend, don't fork).
- `interact_test.c`: resolver called with the right face/yaw, refusal leaves
  the world untouched, award mapping for every redstone id.

## 5. Multiplayer (Agent E — server)

- Actor-authoritative via plain BLOCK_EDIT: the client that causes a change
  (a player toggling a lever, or the redstone tick running on the client that
  placed the source — the "actor" is whoever last edited a component in that
  network) sends the resulting ON/OFF id flips as ordinary BLOCK_EDITs; every
  client applies remote edits WITHOUT re-simulating them
  (`bool networldApplyingRemote(void)` is true inside the remote-apply path
  and `redstoneNotify` records but does not re-tick while it is set).
  Simulation runs on every client for its own components only; a remote
  client's flips arrive as edits. Divergence is possible when two clients
  edit one circuit in the same tick — accepted for 1.9.2; recorded in ROADMAP
  (a server-side redstone tick is the 1.9.x fix).
- Server: no new opcode. The diff store already persists ids. Batched fsync:
  `BS_DIFF_SYNC_INTERVAL_MS 1000` (today every edit fsyncs; a clock will
  flip ids at 10 Hz — the batch is REQUIRED before 1.9.2 ships). Registry
  crc changes → server release FIRST, then the client pin.

## 6. Integration (Agent F — main.c seams 1..11, stanzas, docs, version)

1. `redstoneInit()` after world init.
2. `redstoneSetChangeFn(worldEditFromRedstone, ...)` → world write + mesh
   dirty + BLOCK_EDIT send when not applying remote.
3. Every existing edit path calls `redstoneNotify` (place, break, remote
   apply, worldgen NO).
4. Scheduler registration with `REDSTONE_TICK_ID`.
5. `interactSetPlaceResolveFn`.
6. Body feed: player + animals → `redstoneBodyAt` once per game tick.
7. Chunk load → `redstoneScanChunk`; unload → `redstoneDropColumn`.
8. Fine mesh pass in the render loop + the debug toggle.
9. Use (A) routing to `redstoneUse` first.
10. Debug HUD line: pending components, fine overflow count, tick budget
    hits.
11. `version.h 1.9.2`, CHANGELOG, whatsnew, QR, ROADMAP follow-ups
    (ore, repeaters, sticky, server tick, atlas growth).
- `tests/redstone_wiring_test.c`: a grep test that every seam above exists in
  main.c (a feature can land unreachable — grep the PAYOFF).
- `tools/run_host_tests.sh`: stanzas for redstone_test, finemesh_test,
  mesh_pool_bytes_test, and the wiring test.

## 7. Partition and order

- A (gate 0): block.h/.c, registry.c/_test, atlas_tiles.h,
  block_tiles_check.c, make_atlas.py, atlas.png, crafting.* — everyone else
  compiles against A's header, so A lands first (ids + helpers can be
  reviewed in an hour; the art can trail).
- B: world/redstone.* (pure, host-tested) — starts on A's header.
- C: finemesh.* + the two mesher edits + biome_tint_test.c + chunk_render.*
  + mesh_pool_sizing.h + tests/mesh_pool_bytes_test.c.
- D: interact.* + networld.* (the `networldApplyingRemote` flag).
- E: server (batched fsync, version 1.9.11 or next, release FIRST).
- F: integrator — main.c seams 1..11, stanzas, wiring test, docs, version.
- Riskiest: C's fine-mesh renderer (GPU hang on hardware). Ship behind the
  toggle; bisect on Azahar first, then hardware.

## 8. Budgets (measured or reasoned — label when pinning)

- .bss: `Redstone` ≈ 22.3 KB (reasoned from the struct; pin by
  `_Static_assert` and print `sizeof` in the test).
- Linear: fine pool 393,216 B (exact); world budget unchanged (12 MB pin in
  `mesh_pool_bytes_test.c` must still pass — measure).
- VRAM: no new textures (atlas tiles are in the existing 256² sheet).
- Sprite quads: +1 debug HUD line ≈ 30 quads.
- Stack: fixpoint is iterative (no recursion); the largest local is the
  visit queue — put it in the static struct, not on the stack.
