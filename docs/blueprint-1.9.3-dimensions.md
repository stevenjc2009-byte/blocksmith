# Blueprint v1.9.3 — Dimensions

Written 2026-09-06 by the v1.9.3 architect lane, read against the working tree on
branch `v1.9.0` (HEAD `d3ab9de` = v1.8.20, 77 dirty paths, several lanes live).
Reads assume v1.9.2 has landed as its own blueprint describes: block ids 44..83
claimed, `REGISTRY_FULL_COUNT_PIN` 170 → 210, atlas slots 58..62 spent, mesher
tint row 7 spent by `MESH_TINT_ROW_UNPOWERED`.

Supersedes `docs/plan-1.9.3-dimensions.md`, which is a feasibility study written
against a tree that has since moved. Where this document contradicts it, this
document is the one that was read against the code; §11 lists every correction.

Provenance is labelled throughout: **[code]** with a file:line, **[measured]**
with the run that produced the number, **[reasoned]** for arithmetic on cited
constants, **[NEW]** for anything that does not exist today.

---

## 0. The headline, before anything else

Three hard facts came out of the read, and together they set the whole scope.

**0.1 — The atlas is not nearly full. After v1.9.2 it is completely full.**
The sheet is 16 × 1024 px, `ATLAS_W_PX`/`ATLAS_H_PX` (`world/atlas_uv.h:48-49`),
`TILE_PX 16` (`:50`), so `ATLAS_TILE_SLOTS = 1024/16 = 64` (`atlas_uv.h:76`)
**[code]**. Painted today: 48 block-face tiles (0..47) + 9 item icons (48..56,
`gfx/item_icons.h:52-60`) + `TILE_CHEST_TOP = 57` (`gfx/atlas_tiles.h:124`) = 58.
Slot 63 is `ATLAS_TILE_MISSING` and is reserved forever (`atlas_uv.h:120`). Free:
**58, 59, 60, 61, 62 — exactly five** (`atlas_tiles.h:168`). v1.9.2's blueprint
claims all five. **v1.9.3 therefore has zero atlas tiles available.**

And it cannot grow. 1024 px is citro3d's maximum texture dimension — `checkTexSize`
rejects anything outside 8..1024 or non-power-of-two, verified by disassembly
(`atlas_uv.h:16-21`) **[code]**. There is no 1024 → 2048. Growing in U instead
(16 px → 32 px wide) breaks the greedy mesher's `GPU_REPEAT` merge, which encodes
`u0/u1` in *pixels* up to 240 for a 15-block run (`atlas_uv.h:137`, `gfx/atlas.c:55`).
A second sheet is possible (`gfx/crackatlas.png` already is one) but costs a second
texture bind and a per-quad sheet-select bit, splitting every chunk's draw.

**So: v1.9.3 adds no atlas tiles at all.** The dimension's identity has to come
from somewhere else. §3 says where.

**0.2 — Two worlds resident at once do not fit, and do not need to.**
`WORLD_BUDGET_BYTES = 12u*1024u*1024u = 12,582,912` (`world/budget.h:76`);
per column `48 + 8×4104 + 32768 = 65,648 B` (`budget.h:37-41`, pinned by
`tests/world_budget_bytes_test.c:126`) **[measured, ARM sizes]**. New 3DS at its
own radius 5 holds `169 + 2 staging = 171` columns = `171 × 65,648 = 11,225,808 B`
= **89.2%** of the cap (`budget.h:54`). There is 1,357,104 B left — a fifth of what
one radius-3 world costs. **[reasoned]**

The budget is a module singleton on purpose: *"there is one world, and the
bottom-screen report needs the figures without a pointer threaded through
everything that allocates"* (`budget.h:7-8`, verbatim) **[code]**.

This blueprint does not fight that. **One world is resident at a time. Ever.**
The swap is a hard barrier with `budgetUsed() == 0` in the middle. §5.

**0.3 — The swap machinery already exists and is already shipped.**
The old plan doc flagged "job-queue drain during swap is asserted, not designed"
as its #1 risk. It is not a risk. There is no `jobqDrain()` — `jobq.h:95-113`
exposes push/pop/count only **[code]** — but `workerStop()` joins both lanes and
drops any staged-but-uninstalled column (`app/worker.c:766-773`), and
`workerFlushSaves()` blocks until every submitted save reaches the card
(`main.c:7731`). `main.c` already runs the exact sequence this feature needs, at
world teardown (`main.c:7726-7821`) and at world entry (`main.c:4991-5141`).

**The dimension swap is those two blocks, refactored into two functions, called
back-to-back without going via the title screen.** That is the single most
de-risking finding in this document.

---

## 1. Scope

**In.** One second dimension — **the Underforge** (name is HIS CALL, §12; the
work is not blocked on it, the code uses `DIM_UNDERFORGE`). Its own generator, its
own sky/fog/light mood, its own six blocks drawn on existing tiles. Portals: built
by hand, lit by completion, linked by a 1:4 coordinate map with an
always-succeeds arrival. Its own save subtree. Player position, inventory and
spawn across the boundary. A capability-gated wire contract with two new opcodes
and no change to any existing payload.

**Out, and why.**

- **The End.** `docs/ROADMAP.md:541-546` asks for two dimensions. This ships one.
  The machine (swap, portals, save subtree, wire, tint rows) is the whole cost;
  the second dimension is then a table row and a generator. Shipping both at once
  doubles the untested surface for no extra proof. Tint rows 14 and 15 and block
  ids 90+ are left free for it. **Recorded as v1.9.4 in ROADMAP.**
- **Lava.** It does not exist. `grep -rn LAVA source/` returns exactly one hit,
  `world/cave_carve.h:24`, saying so outright: *"Lava needs a new BLOCK_LAVA id,
  which moves the registry CRC and forces a coordinated server release"* **[code]**.
  `docs/ROADMAP.md:375` claims v1.8.11 added "lava pools below the lava line";
  it did not. `plan-1.9.3-dimensions.md` §5 builds on that claim. A fire dimension
  with no lava is the honest v1.9.3; the Underforge is lit by ember blocks, not
  by a fluid. Lava is a v1.9.x follow-up with its own fluid design.
- **New block shapes.** `BLOCK_SHAPE_COUNT` is 2 of 8 today
  (`world/block.h:421-425`, 3-bit field at `registry.h:57-58`) but v1.9.2 spends
  four more. Every v1.9.3 block is `BLOCK_SHAPE_FULL_CUBE = 0`.
- **New atlas tiles.** §0.1.
- **A dimension-aware chunk wire.** §7 gets there with two opcodes and a sticky
  per-session dimension instead of six dimension-qualified opcode variants.
- **Mobs, structures, a boss.** ROADMAP does not ask for them here.

---

## 2. Success criteria

### 2A — measurable on the host (`sh tools/run_host_tests.sh`, WSL gcc `-std=c11 -Wall -Wextra -Werror -O1`)

1. `dimension self-test: PASS N checks` with `N == DIMENSION_TEST_EXPECTED_CHECKS`.
2. `registry self-test` green with `registryCount() == 90` and
   `REGISTRY_FULL_COUNT_PIN == 216` (= 90 core + 126 dyn, `registry_test.c:57-60`
   arithmetic unchanged). The golden core CRC pin at `registry_test.c:799` moves
   from `0x2A61` to whatever the 90-row table computes — recorded, not predicted.
3. `world_budget_bytes_test` still prints `per column 65,648 B` and
   `col_target == 65648u` (`tests/world_budget_bytes_test.c:126`). v1.9.3 must not
   move it.
4. New `tests/dim_budget_test.c`: for every `(console, dimension)` pair the
   resident cost is `budgetBytesForRadius(r, 65648)` and never the sum of two —
   asserted by driving a simulated swap through `budgetClaim`/`budgetRelease` and
   checking `budgetUsed() == 0` at the barrier. Red arm: remove the barrier
   assert and the peak reads `2 × 11,225,808 = 22,451,616 > 12,582,912`.
5. `biome_tint_test` green with `MESH_TINT_BITS == 4`, `MESH_TINT_ROWS == 16`, and
   `_Static_assert(MESH_AO_BITS + MESH_TINT_BITS <= 8)` still true (2+4 = 6).
6. `dim_save_test.c`: an on-disk fixture written by the *v1.9.0* format (world dir
   with `player.dat` v1, `genver.bin`, one `.bsr`, and **no** `dim/` subtree and
   **no** `dim.dat`) loads to `DIM_OVERWORLD` with the pose byte-identical to what
   `playerPoseLoad` returns today. Red arm: make `dimStateResolve` refuse an absent
   file and this goes red with "absent dim.dat is overworld, not an error".
7. `portal_test.c`: frame detection accepts every legal interior
   (2..3 wide × 3..4 tall, both orientations) and rejects every one-block-off
   variant; the 1:4 map round-trips; `portalPlaceAt` succeeds against a solid
   destination, an air destination, and a destination at `y = 0` and `y = 127`.
8. Seam greps on the integrated tree (the anti-inert checks — a feature can land
   unreachable): `grep -c "dimensionSwapTo" source/main.c >= 1`;
   `grep -c "budgetUsed() == 0" source/main.c == 1`;
   `grep -c "blockStateSave" source/main.c >= 2` (teardown plus swap);
   `grep -c "networldSendDimEnter" source/main.c == 1`;
   `grep -c "dimension" tools/run_host_tests.sh >= 2`.
9. `sh tools/check_readme_current.sh` passes.

### 2B — playtest only (real 3DS, both models)

1. Build a frame of portal-frame blocks with a 2×3 hole and place the last block:
   the hole fills with portal blocks and makes the place sound. A frame one block
   short does nothing.
2. Stand in the portal for one second: the screen fades, and inside two seconds
   you are standing in the Underforge on solid ground, in a portal, with the same
   inventory and the same health.
3. The Underforge sky is dark maroon, the fog closes in at about half the
   overworld distance, and there is no day/night change however long you stay.
4. Walking away from the arrival portal streams terrain normally — no stutter that
   the overworld does not also have.
5. Walk 40 blocks, dig a hole, put a chest in it with something in it. Go back
   through the portal: you are in the overworld within about ten blocks of where
   you left. Come back: the hole and the chest are still there with the contents.
6. Mine a scorchstone, carry it to the overworld, place it: it is still red, not
   grey. (This is the check that the tint is per-block and not per-dimension.)
7. Break the frame from inside so the portal goes out with you in the dimension,
   then walk to where you arrived: a new portal has been built for you and the way
   home works.
8. Die in the Underforge: you respawn at the overworld spawn, not in the dimension.
9. Quit to the title from inside the dimension, reload the world: you are back in
   the Underforge where you left, not in the overworld.
10. Join a multiplayer server that has not shipped dimensions: standing in a portal
    prints "This server does not have portals" and nothing else happens; no kick,
    no freeze.
11. Debug memory readout after five entries and five exits: the world-store figure
    returns to the same number it read before the first entry.

---

## 3. Design — every decision resolved

### D1. The dimension's look comes from tint rows, not tiles. Widen the tint field 3 bits to 4.

Today `MESH_TINT_BITS 3` gives `MESH_TINT_ROWS 8` (`world/mesher.h:194-196`), packed
into bits 2..4 of the vertex's `ao` byte by `meshAoPack` (`mesher.h:203-206`), so it
costs **zero** vertex bytes — `sizeof(MeshVertex) == 8` is static-asserted
(`world/mesh_vertex.h:69`) **[code]**. Row 0 is the untinted identity, rows 1..6 are
the six biomes via `MESH_TINT_ROW_FOR_BIOME(b) = (b+1) & MASK` (`mesher.h:280`), and
row 7 is v1.9.2's `MESH_TINT_ROW_UNPOWERED`. **Full.**

**Decision: `MESH_TINT_BITS 3 → 4`.** Rows 8..15 appear; v1.9.3 claims 8..13 and
leaves 14, 15 for the End.

Costs, all checkable:
- **Vertex bytes: 0.** The tint moves from bits 2..4 to bits 2..5 of the `ao` byte.
  Bits 6 and 7 were already free. `_Static_assert(MESH_AO_BITS + MESH_TINT_BITS <= 8)`
  (`mesher.h:210`) becomes `2 + 4 = 6 <= 8` and still passes. **[reasoned]**
- **Merge key: 0.** `faceFlatKey` returns
  `0x10000u | (faceTint << 17) | (ao << 8) | pad` (`mesher.c:915-931`). A 3-bit tint
  occupies bits 17..19; a 4-bit one occupies 17..20. Bits 21..31 are unused and bit
  16 is the sentinel. No collision. **[reasoned, from the shift arithmetic]**
- **Vertex-shader uniform registers: +8, and this is the tight one.**
  `world_dynamic.v.pica:91` declares `.fvec tintPalette[8]`; the file's own comment
  at `:107` states `waterShimmer` is *"the 87th of the vertex stage's 96 float
  uniform registers"* **[code]**. Widening the bank to 16 takes the program to
  **95 of 96**. It fits, with one register left. `.fvec faceShade[64]` (`:50`) is
  where the other 64 went; shrinking it is not this version's job.

**This is the decision most likely to need his input.** The alternative that costs
zero registers is to keep 8 rows and swap the *contents* of rows 1..6 per dimension
(the palette is re-uploaded every `pipelineBind` anyway, `chunk_render.c:1398-1405`).
**Rejected** because a scorchstone mined in the Underforge and placed in the
overworld would then render with the tundra row — playtest 2B.6 exists precisely to
catch that. If the widened shbin will not assemble, that fallback is the
documented retreat and 2B.6 becomes a known defect rather than a gate.

*Also rejected:* a second atlas sheet (a second bind and a per-quad sheet bit,
splitting every chunk's draw); negotiating tiles back from v1.9.2 (another lane's
scope, and it is not mine to spend).

### D2. Six blocks, ids 84..89, all on existing tiles, all `BLOCK_SHAPE_FULL_CUBE`.

v1.9.2 claims 44..83, so v1.9.3 starts at 84. Ids today run 0..43 contiguously with
`BLOCK_CHEST = 43` (`world/block.h:262`) **[code]**.

| id | name | tiles reused | tint row | notes |
|---|---|---|---|---|
| 84 | `BLOCK_PORTAL_FRAME` | `TILE_STONE` ×6 | 8 | dark violet; craftable, visible in both worlds |
| 85 | `BLOCK_PORTAL` | `TILE_ICE` ×6 | 9 | violet; transparent, so it rides the existing transparent pass |
| 86 | `BLOCK_SCORCHSTONE` | `TILE_STONE` ×6 | 10 | deep red; the dimension's bulk terrain |
| 87 | `BLOCK_ASHDIRT` | `TILE_SAND` ×6 | 11 | grey-brown; the soft layer |
| 88 | `BLOCK_EMBERGLOW` | `TILE_SAND` ×6 | 12 | amber, `luminance = 14` |
| 89 | `BLOCK_BLACKROCK` | `TILE_STONE` ×6 | 13 | near-black; the roof and floor caps |

`BlockDef` already carries `tex[6]` and `luminance` (`registry.h:86-94`, packed,
`sizeof == 27` static-asserted at `:96`), so an ember block that lights the room is
a table row, not code **[code]**.

The tint multiply can only darken — the hardware clamps `outclr` to [0,1] before
TEV (`mesher.h:225-236`) **[code]**. Every row above darkens two channels and holds
one, which is exactly how you get red rock out of grey stone. Grey stone at (0.5,
0.5, 0.5) × row 10 at (1.00, 0.32, 0.26) = (0.50, 0.16, 0.13). **[reasoned]**

`blockFaceTintable(id, face)` (`mesher.c:474-487`) is an explicit id list and must
gain all six ids for all six faces, or they render untinted grey and 2B.6 fails.
This is the single easiest way to ship this feature looking broken.

### D3. Registry: 84 rows → 90. `REGISTRY_FULL_COUNT_PIN` 210 → 216. Server first.

The pin is `core + REGISTRY_DYN_ROWS_PIN (126)` — 44 + 126 = 170 today
(`registry_test.c:57-60`) **[code]**, 84 + 126 = 210 after v1.9.2, **90 + 126 = 216**
after this. **[reasoned]**

Table bytes do not move: `s_defs[REGISTRY_MAX]` is `[256]` already
(`registry.c:807-813`, `REGISTRY_MAX 256` at `registry.h:33`), so six more rows cost
**0 bytes**. `s_count` moves, nothing allocates. **[code]**

`registryCrc16()` moves. The client — and only the client — compares:
`registryMatchesInfo()` at `net/networld.c:331-337` checks rev, count and crc, and on
a core-row mismatch the client **refuses the server itself** since v1.8.7,
`netTransportRefuse("Server is a different Blocksmith version - update")`
(`networld.c:923-946`) **[code]**. So **the server must release first**, and this is
true independently of the new opcodes.

`REGISTRY_REV` stays 1 (`registry.h:106`); `BS_PROTO_VERSION` stays 1
(`bs_proto.h:56`) — v1.8.1 already moved the core CRC 0x72A8 → 0x4066 with the proto
version unchanged (`networld.c:311-317`) **[code]**, and this follows that precedent.

### D4. Save format: a subdirectory and one new sidecar. No version bump anywhere.

Today a world is a directory under `REGION_ROOT "sdmc:/blocksmith/worlds"`
(`region.h:198`) holding `r.<rx>.<rz>.bsr`, `seed.bin`, `genver.bin`, `time.bin`,
`player.dat`, `inventory.dat`, `survival.dat`, `blockstate.dat`, `registry.bin`
**[code]**.

**Added, and only added:**

- `<world_dir>/dim1/` — the Underforge's own `r.*.bsr`, its own `genver.bin`, its own
  `blockstate.dat`. Nothing else. **[NEW]**
- `<world_dir>/dim.dat` — 40 bytes, the only new file at the world root. **[NEW]**

**Unchanged, byte for byte:** `REGION_VERSION` stays 1 (`region.h:190`);
`PLAYERPOSE_VERSION` stays 1 (`world/playerpose.h:28-37`, the file stays 32 bytes);
the inventory format stays at version 1 and 68 bytes
(`world/inventory_persist_test.c:135-136,149-150`); `seed.bin`, `time.bin`,
`survival.dat`, `registry.bin` are untouched.

**How an existing v1.9.x save still loads: it takes no new code path at all.**
It has no `dim1/` and no `dim.dat`. `dimStateResolve()` reads an absent `dim.dat`
as `DIM_OVERWORLD` with no return position, exactly the way `dayNightRead` reads an
absent `time.bin` as `DAY_START_TICKS` rather than refusing the world
(`world/daynight.h:304-374`) **[code]**. The world then loads through the identical
sequence it uses today. **There is no migration step, because there is nothing to
migrate.**

**The rule that makes it downgrade-safe too, and which is also just correct:
`player.dat` always holds the OVERWORLD position.** While the player is inside the
Underforge, the dimension position lives in `dim.dat` and `player.dat` holds where
they will come back to. So a v1.9.2 client opening a world saved from inside the
dimension ignores the unknown `dim.dat`, reads `player.dat`, and puts the player
back in the overworld at the portal — which is the right answer, not a fallback.
**[reasoned]** *Rejected:* bumping `PLAYERPOSE_VERSION` to 2 and widening
`player.dat` to 56 bytes — an old client would then fail `playerPoseLoad`, get
`false`, and drop the player at the computed spawn, losing their position for good.

`dim.dat`, 40 bytes, same magic/version/crc32 shape as `time.bin` and `genver.bin`:

```
0x00  u32  magic    'B','S','D','M'  = 0x4D445342 LE   DIM_STATE_MAGIC   [NEW]
0x04  u32  version  = 1                                DIM_STATE_VERSION [NEW]
0x08  u32  crc32    over 0x0C..EOF
0x0C  u8   cur_dim  0 = overworld, 1 = Underforge
0x0D  u8   pad[3]   zero
0x10  f32  dim_x        the position INSIDE the dimension
0x14  f32  dim_y
0x18  f32  dim_z
0x1C  f32  dim_yaw
0x20  f32  dim_pitch
0x24  u32  reserved  zero, ignored on read
```
`4+4+4+1+3+20+4 = 40` = `DIM_STATE_BYTES` **[NEW, reasoned]**. Bad magic, bad
version, bad CRC, short file, long file — every one degrades to
`{DIM_OVERWORLD, no position}`, never a refusal, matching `inventory.dat`'s
degrade-to-empty contract rather than `seed.bin`'s refuse-the-world contract.

**The dimension's seed is derived, never stored.**
`dim_seed = rngMix(overworld_seed ^ SALT_DIM_UNDERFORGE)` with
`SALT_DIM_UNDERFORGE 0x55465247u` ('UFRG'), following the existing salt convention
and its collision-check comment block (`worldgen.c:65-111`) **[code]**. So there is
no `dim1/seed.bin` and no way for the two to disagree.

`dim1/genver.bin` is written by `genVersionWrite` with
`GEN_VERSION_FOR_NEW_WORLDS` the first time the player enters, through
`genVersionResolve(dim_dir, &v)` unchanged (`world/genversion.h:202-209`,
`main.c:1656`) **[code]**. **Zero new code in `genversion.c`.** *Rejected:* a
separate `GEN_VERSION_DIM_*` ladder — two version spaces to keep straight for a
generator that has exactly one version today.

### D5. Portals: built by completion, linked 1:4, arrival always succeeds.

**Building.** A rectangular frame of `BLOCK_PORTAL_FRAME` in a vertical plane, with
an air interior 2 or 3 wide and 3 or 4 tall (`PORTAL_INTERIOR_W_MIN 2`,
`_W_MAX 3`, `_H_MIN 3`, `_H_MAX 4` **[NEW]**). Corners are not required.

**Lighting.** On every `BLOCK_PORTAL_FRAME` placement, `portalScanAt(x,y,z)` tests
the two vertical planes through that block for a closed frame. Bounded above by
`2 orientations × 5 wide × 6 tall = 60` `worldGet` calls — a scan that cannot be
felt. **[reasoned]** If it closes, the interior fills with `BLOCK_PORTAL`.

*Rejected:* a flint-and-steel item. It needs a new item id, a new atlas tile for
its icon (there are none, §0.1), a new recipe, and a new use-verb. Lighting on
completion needs none of those and is self-teaching.

**Coordinate map: 1:4.**
`dim_x = floor(over_x / 4)`, `dim_z = floor(over_z / 4)`, `dim_y = clamp(over_y, 8, 119)`.
Reverse multiplies by 4. Y is **not** scaled — both worlds are `WORLD_HEIGHT 128`
(`world.h:24`, = `COLUMN_CHUNKS 8 × CHUNK_DIM 16`) **[code]**.
*Rejected:* 1:8 (a portal 8 km away in the overworld maps into a Nether column the
player will never re-find, so portals stop re-linking); 1:1 (no travel benefit — the
dimension is then just a second overworld and the whole feature is decorative).

**Arrival, and what happens when the far side is obstructed or unloaded.**
The far side is *always* unloaded — that is the normal case, not the exception,
because the swap released everything first. The arrival step is therefore ordered:

1. Load or generate the `DIM_ARRIVAL_RADIUS 1` disc — **3×3 = 9 columns** —
   centred on the mapped destination column. **[NEW]**
2. Search those columns for an existing `BLOCK_PORTAL` within
   `PORTAL_SEARCH_RADIUS 8` blocks horizontally, over the full 128 height:
   `17 × 17 × 128 = 36,992` block reads, all resident. The 3×3 disc spans ±24
   blocks from centre, so a radius-8 search never reads an unloaded column.
   **[reasoned]**
3. If none, **build one**. `portalPlaceAt` carves a 4 wide × 5 tall × 3 deep pocket
   at the destination, floors it with `BLOCK_BLACKROCK` if the cell below is air,
   erects the frame, fills the interior. It runs on solid rock (carve), on open air
   (floor it) and against either world edge (`y` is clamped into 8..119 before the
   pocket is sited, so a 5-tall pocket always fits inside 0..127). **It cannot
   fail.** A portal that fails to place strands the player, so this is written as
   a total function with no error return, and `portal_test.c` case 7 drives it
   against a solid, an empty and both clamped destinations.
4. Place the player, clear the fade, let the rest of the radius stream in behind
   the fog.

**Dwell and cooldown.** `PORTAL_DWELL_TICKS 20` (1 s at `TICK_HZ 20`, `tick.h:30`)
before a portal fires, so you can build next to one; `PORTAL_COOLDOWN_TICKS 40`
(2 s) after arriving, because you arrive standing inside one. **[NEW]** Both live
in a 16-byte static, neither is saved.

**No link table.** The search is cheap enough (step 2) that caching links would buy
nothing and cost a persistent structure that can go stale when someone mines a
portal. *Rejected explicitly*, and it is the reason there is no `portals.dat`.

### D6. Mood: sky base colour, fog scale, and a flat sky-light fill.

All three are already parameters. This is the cheapest part of the feature.

- **Sky and fog colour.** `dayNightSkyClearRgba8(tod, base)` and
  `dayNightSkyFogBgr(tod, base)` (`world/daynight.c:245-272`) already **take the base
  colour as an argument** — both call sites just pass the `SKY_CLEAR_RGBA8` macro
  (`scene/chunk_render.h:69`, used at `chunk_render.c:1398`) **[code]**. Replace the
  macro at those two sites with a module variable set by
  `chunkRenderSetSkyBase(uint32_t)` **[NEW]**, default `SKY_CLEAR_RGBA8`,
  `DIM_SKY_BASE_UNDERFORGE 0x2A1210FFu` **[NEW]** inside. **Two lines and one
  setter.** Clear and fog cannot drift apart because they still share one base,
  which is the property the header comment protects.
- **Fog distance.** `fogShapeFor(boundary_blocks, strength)` (`gfx/fogramp.c:27-45`)
  derives the whole ramp from those two floats **[code]**. Add
  `chunkRenderSetFogScale(float)` **[NEW]** multiplying `boundary_blocks`;
  `DIM_FOG_SCALE_UNDERFORGE 0.45f` **[NEW]** closes the fog to under half the
  overworld's distance. The LUT is already rebuilt in `chunkRenderSetDistance`
  (`chunk_render.h:94-100`) so there is a rebuild path to reuse; the texture is
  128×8 A8 = **1,024 B** (`gfx/fogtex.c:12-16`) and is re-uploaded, not
  re-allocated.
- **Sky light: flat, not propagated.** `light.c:837-865` unconditionally seeds
  sky = 15 into every cell above `hm.top[z][x]` **[code]** — it assumes open sky
  above every column. A roofed dimension needs that off. Add
  `lightSetSkyMode(LightSkyMode)` **[NEW]** with `LIGHT_SKY_PROPAGATE` (today's
  behaviour, the default) and `LIGHT_SKY_FLAT`. Flat mode replaces the entire sky
  pass with `memset(lc->sky, 0x55, LIGHT_COL_BYTES)` — level 5 in both nibbles,
  `DIM_SKY_FLAT_LEVEL 5` **[NEW]**, `LIGHT_COL_BYTES 16384` (`light.h:70-73`).
  Block light (torches, `BLOCK_EMBERGLOW` at luminance 14) then floods on top
  exactly as it does in a cave, unchanged.
  **This makes dimension columns cheaper to light than overworld ones** — one
  16 KB memset instead of a BFS. **[reasoned, not measured]**
- **No day/night inside.** The shader's `dayLevel[1]` uniform
  (`world_dynamic.v.pica:54`) is pinned to 1.0 while resident in the Underforge, so
  the flat 5/15 ambient does not swing with the overworld clock. `time.bin` keeps
  counting — it is total ticks since world creation (`daynight.h:304-374`), so the
  overworld clock advances normally while you are away, which is what a player
  expects.

**VRAM delta: 0.** No new texture. The atlas is 16×1024 RGBA5551 = **32,768 B** of
the ~6 MB `OS_VRAM_SIZE 0x600000` (`gfx/atlas.c:12`, `app/gputest.c:225`) and does
not move **[code]**.

### D7. Generation: one new file, no new noise.

`worldgen_underforge.c/.h` **[NEW]**. The existing machinery is already
caller-parameterised — `WorldGen { seed, version, cave_salt[2] }` is a plain
12-byte struct the caller owns (`worldgen.h:578-596`) and `WorldGenScratch` is
per-lane and passed in, never a file static (`worldgen.h:604-607`, a v1.8.7 bug fix)
**[code]**. So a second generator is a second function with the same signature,
sharing every buffer.

Shape: a roofed slab. `BLOCK_BLACKROCK` cap at y 120..127 and floor at y 0..7;
between them, `worldgen_density.c`'s grid retuned toward wide open caverns, filled
with `BLOCK_SCORCHSTONE`, `BLOCK_ASHDIRT` in the top two cells of any solid run, and
`BLOCK_EMBERGLOW` scattered on ceilings by a salted hash. No trees, no scatter, no
flora, no biomes, no water. The reused levers are `GenBiomePoint` and the control
table behind `wgdBiomeParams()` (`worldgen_density.h:198-209`) **[code]** — a
different four-point height/amplitude curve, not a different algorithm.

**No `GEN_VERSION_*` constant is added and the overworld's ladder does not move.**
The dimension's `genver.bin` is a separate file (D4), so the two cannot interfere.

### D8. Multiplayer: two opcodes, one cap bit, zero changes to any existing payload.

The problem is real and it is in the wire's shape: every chunk-addressing opcode
keys a column as `(cx, cz)` `int32` only —
`BS_APP_BLOCK_EDIT` 14 B, `CHUNK_SUB`/`CHUNK_UNSUB` 9 B, `CHUNK_DIFFS` 12-byte
header, `WORLD_SYNC`, all strict-length and all documented as *"reject on length
alone"* with no reserved padding (`deps/blocksmith-server/proto/bs_proto.h:470-551`)
**[code]**. There is no third axis and no spare byte to make one. Two players in
different dimensions subscribed to column (0,0) would receive each other's diffs.

**Decision: a sticky per-session dimension, not dimension-qualified opcodes.**
The server records which dimension each player is in and interprets that player's
`CHUNK_SUB`, `CHUNK_UNSUB` and `BLOCK_EDIT` in it. Every existing payload is
unchanged, byte for byte.

*Rejected:* dimension-qualified variants of the four chunk opcodes — six new
opcodes instead of two, four parallel encode/decode paths on both sides, and a
permanent fork in the message set for a field that is constant per player between
transitions.
*Rejected:* namespacing dimensions by a large `(cx, cz)` offset, which would need
**no** new opcodes at all. It breaks on `POS_UPDATE`, which carries positions as
`f32` (`bs_proto.h:476-482`): at an offset large enough to be collision-proof the
f32 ULP exceeds a block. Keeping ULP under 1/64 block needs `|x| < 2^18 = 262,144`
blocks = 16,384 columns, which is not a safe bound on an "infinite" overworld.
**[reasoned]** This is a trap worth naming — it looks free and it is not.

**New, in `deps/blocksmith-server/proto/bs_proto.h`:**

- `BS_CAP_DIMENSIONS = (1u << 1)` **[NEW]**, beside `BS_CAP_CHESTS = (1u << 0)`
  at `bs_proto.h:600-601`. Bits 1..31 are already documented as
  ignore-never-validate (`:397-399`) **[code]**, so this is the reserved-bit
  mechanism v1.9.0 built, used for the first time as designed.
- `BS_APP_DIM_ENTER = 0x14` **[NEW]**, C→S, gated on `BS_CAP_DIMENSIONS`.
  `{type u8, dim u8, x i32, y i32, z i32}` = `BS_DIM_ENTER_BYTES 14`. The x/y/z is
  the portal block used, so the server can check it really is one before agreeing.
- `BS_APP_DIM_STATE = 0x15` **[NEW]**, S→C.
  `{type u8, sid u32, dim u8}` = `BS_DIM_STATE_BYTES 6`. Sent for every player at
  join and on every accepted transition, including the sender's own.

0x13 is the current ceiling (`BS_APP_CHEST_ACTION`, `bs_proto.h:419-420`), so 0x14
and 0x15 are the next two free **[code]**.

**Degrade on an old server.** `networldSendDimEnter()` **[NEW]** returns false when
`!(networldServerCaps() & BS_CAP_DIMENSIONS)` — the exact shape of
`networldSendChestAction()` at `networld.c:1429-1445` **[code]**. Caps arrive as a
`u32` at join or never; never reads as 0 (`networld.h:215`), which is the right
answer for every server shipped to date. The client then leaves the portal inert
and prints one line. **A new client on an old server is never kicked**, which is the
entire point of the v1.9.0 caps design.

**Why the server must ship first, twice over.** `BS_APP_DIM_ENTER` is C→S, and an
unknown C→S opcode hits `default: send_kick(...); playerFree(p);` at
`deps/blocksmith-server/game/bsgame.c:1919-1922` **[code]** — it kicks and frees the
player. Separately, the registry CRC moves (D3) and the client refuses a server
whose core table differs. Both are hard.

**Transition ordering.** In-flight `CHUNK_DIFFS` for the old dimension must not land
in the new world. The client, on sending `DIM_ENTER`: unsubscribes every column,
sets `s_dim_pending`, and **drops every incoming `CHUNK_DIFFS` and `BLOCK_EDIT`**
until `DIM_STATE` for its own sid arrives. The server drops the player's whole
subscription set when it accepts. Timeout `NETWORLD_DIM_TIMEOUT_MS 3000` **[NEW]**:
abort, stay where you are, print a line. This is the whole synchronisation design
and it needs no epoch counter because the client is tearing its world down anyway.

**Peers across the boundary.** `uint8_t s_peer_dim[NET_MAX_PLAYERS]` **[NEW]**, 16
bytes (`net/bsnet.h:35`). A sid whose dim differs from the local one is not drawn,
not name-tagged, and not collided with. Default 0 on join, which is correct against
an old server because on an old server nobody can be anywhere else. `POS_UPDATE`
and `PLAYER_STATE` are otherwise untouched.

**Server-side work, stated so it is not underestimated:** the diff store must be
keyed by `(dim, cx, cz)` rather than `(cx, cz)`, and per-player subscription sets
must be dimension-scoped. That is the real cost of this feature on the server, and
it is why §10 puts the server release two phases ahead of the client.

### D9. What crosses the boundary with the player, and what does not.

- **Inventory: crosses, unchanged, zero work.** `Inventory` is 49 bytes
  (`world/inventory.h:239-242`, `INV_SLOT_COUNT 24`), lives in `main.c`, and
  persists to `<world_dir>/inventory.dat` at the world root, not per dimension
  **[code]**. Nothing to do.
- **Health and hunger: cross, unchanged.** `survival.dat` at the world root, payload
  `{health u8, hunger u8}` (`world/survival.h:129-140`) **[code]**.
- **Spawn and death: always the overworld.** There is no persisted spawn point
  anywhere — `playerpose.h:23-26` states the missing-file case computes spawn from
  `worldStandingY()`/`worldgenHeight()` **[code]**, and `survival.h` has no death
  handler. Dying in the Underforge runs the swap home and then the existing respawn.
  *Rejected:* a dimension spawn point — new persistent state for a rule ("you always
  come home to die") that is simpler without it.
- **Entities: do not cross, and this needs no code.** `entityTick()` despawns an
  entity the instant its column unloads, `worldColumn() == NULL`
  (`entity/entity.h:203-210`) **[code]**. Releasing every overworld column at the
  swap barrier despawns the entire overworld population as a side effect. Entities
  are never saved (no save/load in `entity.h`), so there is nothing to key by
  dimension either.
- **Block state: does NOT cross, and this one is a real bug if missed.**
  `BlockStateTable` is a flat 256-slot side table keyed by **absolute** `(x,y,z)`
  (`world/blockstate.h:160,185-189`) **[code]**. A chest at (10,64,10) in the
  overworld and one at (10,64,10) in the Underforge would be the same record. The
  swap must `blockStateSave(cur_dir)` → `blockStateInit()` → `blockStateLoad(new_dir)`.
  One table, reused; **0 bytes** of new memory; three calls. Named as seam S6.
- **Water: does not cross.** `WaterSim`'s flow map is a 2048-slot hash on absolute
  position (`world/water.h:137,196-249`) **[code]** — same collision. Reset on swap.
  Flowing water in the overworld settles at whatever block ids it had reached, which
  is already what happens on world exit today.

---

## 4. The swap sequence

This is the load-bearing part and it is almost entirely existing code.

```
dimensionSwapTo(int target_dim)                                          [NEW]
 1  fade / loading screen up                       runLoadingScreen, main.c:2132
 2  saveDirtyColumns(); workerFlushSaves();                        main.c:7731
 3  blockStateSave(cur_dir);                                       main.c:7726
 4  write dim.dat with the position we are leaving from             [NEW]
 5  workerStop();               joins both lanes, drops staged   worker.c:766-773
 6  chunkRenderReleaseAll();    slot bookkeeping only        chunk_render.c:2151
 7  worldExit(&s_world); worldInit(&s_world);                       main.c:7821
 8  ===== BARRIER:  assert(budgetUsed() == 0)  =====                 [NEW]
 9  waterInit(); blockStateInit(); relightq/dirtyq reset             [NEW glue]
10  s_dim_cur = target; apply sky base, fog scale, sky mode, dayLevel [NEW]
11  workerSetWorldDir(dir_for(target));                             main.c:1868
12  genVersionResolve(dir_for(target), &v);                         main.c:1656
13  blockStateLoad(dir_for(target));                                 [NEW glue]
14  workerStart(&s_dim_gen);                                        main.c:1870
15  generate/load the 3x3 arrival disc, driving loadingStep()   loading.h:71-87
16  portal search, else portalPlaceAt                                [NEW]
17  place the player; genRequestArea() for the full radius          main.c:1883
18  fade down
```

Steps 2, 3, 5, 6, 7 are `main.c`'s existing teardown (`main.c:7726-7821`); steps
11, 12, 14, 17 are its existing entry (`main.c:4991-5141`). **The refactor is to
lift both into named functions and call them without passing through the title
screen** — not to write a swap from scratch.

**Step 8 is the whole memory argument in one line.** `worldExit` frees every column
and every `LightColumn`, so `budgetUsed()` returns to 0 before a single dimension
column is claimed. The peak is `max(overworld, dimension)`, never the sum. Test 2A.4
drives this and its red arm reads `22,451,616 > 12,582,912`.

**Why `workerStop()`/`workerStart()` rather than a new drain primitive.** There is
no `jobqDrain()` and `workerBusy()` is a poll, not a blocking wait
(`worker.h:257-260`, `worker.c:1124-1138`) **[code]**. `workerStop()` already is the
drain: it joins, and it drops staged columns rather than installing them into a
world that is about to be freed. It is what `main.c:7797` already does at teardown.
*Rejected:* adding `jobqDrain()` — new synchronisation code on the one path where a
race writes a column into the wrong save directory, when a join that is already
shipped and already exercised does the job.

**Cost.** Step 15 is 9 columns. Generation is **1.055–1.310 ms/column**
(`docs/ROADMAP.md:333-336`) **[measured, host gcc -O1]** → 9 × 1.310 = **11.8 ms**.
Region IO uncached is **39.2–56.0 µs/column** (`region.h:276-283`, 16,200 reads)
**[measured, host]** → 9 × 56 = **0.50 ms**. Host total ≈ **12.3 ms**.

**Not measured on ARM, and there is no ARM per-column figure anywhere in the tree**
— `region.h:97-100` and `worker.c:341-345` both say so about SD access specifically
**[code]**. A 10× ARM factor puts the arrival at ~120 ms, well inside "not a stall".
Step 2's save is the real unknown: it writes only `Column.dirty` columns
(`world.h:31-58`), typically single digits, over the `s_fs_lock` on a real SD card,
and that has never been timed on hardware. The observability answer is in §9: the
transition emits a `loadprof` line with the real numbers so his first playtest
produces a measurement instead of an impression, and `LOADING_STALLED` already
fires at 900 frames (~15 s, `scene/loading.h:36`) so a pathological card cannot
hang the console silently.

---

## 5. Budgets

Every figure is arithmetic on a cited constant.

| Pool | Today | v1.9.3 delta | After |
|---|---|---|---|
| World store, cap 12,582,912 (`budget.h:76`) | 11,225,808 peak at r5 (89.2%) | **0** | 11,225,808 |
| Linear, mesh pool at r5 | 46,948,352 | **0** | 46,948,352 |
| VRAM, atlas (`atlas.c:12`) | 32,768 | **0** | 32,768 |
| VRAM, fog LUT (`fogtex.c:12`) | 1,024 | **0** | 1,024 |
| `sizeof(MeshVertex)` (`mesh_vertex.h:69`) | 8 | **0** | 8 |
| VS float uniform registers (`world_dynamic.v.pica:107`) | 87 of 96 | **+8** | **95 of 96** |
| Registry table `s_defs[256]` (`registry.c:807`) | 6,912 | **0** | 6,912 |
| Existing wire payload bytes | — | **0** | — |
| Stack, 32 KB | — | **0** | — |
| New `.bss` | — | **+284** | — |

**World store delta is 0 and that is the central claim.** One world resident, ever;
the barrier at step 8 is what enforces it, and 2A.4 is what proves it.

**New `.bss`, itemised. [NEW, reasoned]**

```
Dimension s_dims[2]            2 x 28 =  56   id u8, dir[8], sky_base u32,
                                              fog_scale f32, sky_mode u8,
                                              sky_level u8, ratio u8, gen u8
char      s_dim_dir[128]              = 128   mirrors worker.c:221's s_world_dir
uint8_t   s_dim_file[40]              =  40   dim.dat I/O buffer, off the stack
PortalState s_portal                  =  16   cooldown, dwell, pending, pad,
                                              entry_x/y/z i32
uint8_t   s_peer_dim[16]              =  16   NET_MAX_PLAYERS, bsnet.h:35
WorldGen  s_dim_gen                   =  12   worldgen.h:578-596
s_dim_cur / s_dim_pending / deadline  =  16
                                        ----
                                         284 B
```

**Where the memory for a second dimension comes from, in one sentence:** it is the
overworld's, borrowed while the player is away and given back on the way home —
nothing is bought, nothing is doubled, and the only thing that grows is 284 bytes of
bookkeeping and eight shader uniform registers.

**Regenerated rather than stored:** the dimension's terrain (derived seed, D4);
its light (flat fill, D6); the portal link (searched on arrival, D5). Three
structures that a naive design would have persisted, and none of them exist.

**Disk.** A world whose player never builds a portal grows by **0 bytes** — no
`dim1/`, no `dim.dat`. Entered, it grows by one `dim.dat` (40 B) plus region files
sized exactly like any other world's.

**The one number with no margin: 95 of 96 vertex-shader uniform registers.** It fits
and it leaves one. Anything later that wants a uniform must first shrink
`faceShade[64]` (`world_dynamic.v.pica:50`). Recorded in ROADMAP as a standing
constraint, not as this version's job.

---

## 6. Work partition

Exclusive file lists. Nobody writes another lane's files — a whole-file write
silently reverts a parallel lane's edits.

| Lane | Owns exclusively | Delivers |
|---|---|---|
| **A — ids and registry** (gate 0) | `world/block.h`, `block.c`, `world/registry.c`, `registry_test.c`, `world/crafting.c`, `crafting_test.c` | ids 84..89, `blockIsDimension()`, 6 rows, pin 210 → 216, new CRC recorded, portal-frame recipe |
| **B — dimension model** | `world/dimension.h`, `world/dimension.c`, `world/dimension_test.c` | descriptor table, 1:4 map, `dimStateRead/Write`, dim.dat codec — all pure, all host-tested |
| **C — portals** | `world/portal.h`, `world/portal.c`, `world/portal_test.c` | frame scan, light-on-completion, arrival search, `portalPlaceAt` |
| **D — generation** | `world/worldgen_underforge.h`, `.c`, `tests/underforge_gen_test.c` | the slab generator on B's header |
| **E — mood** | `world/mesher.h` tint widen, `world/mesher.c` `blockFaceTintable` + `faceTint`, `world/light.h`, `light.c` sky mode, `scene/chunk_render.h`, `chunk_render.c` sky base + fog scale, `shaders/world_dynamic.v.pica`, `world/biome_tint_test.c` | tint rows 8..15, `LIGHT_SKY_FLAT`, the two setters, the shbin |
| **F — persistence** | `tests/dim_save_test.c`, `tests/dim_budget_test.c` | the compatibility and barrier proofs |
| **G — net** | `net/networld.h`, `networld.c` | cap bit read, `networldSendDimEnter`, `DIM_STATE` handler, `s_peer_dim`, the quiet window |
| **H — server** (releases FIRST) | the server repo, `deps` pin at `Makefile:444` | 2 opcodes, cap bit, 90-row registry, `(dim,cx,cz)` diff store, dimension-scoped subscriptions |
| **Integrator** (sequential, last) | `source/main.c`, `source/version.h`, `README.md`, `CHANGELOG.md`, `docs/ROADMAP.md`, `tools/run_host_tests.sh` | §4's sequence, seams S1..S12, the wiring grep test |

Nobody touches `world/budget.h`, `budget.c`, `world/region.h`, `region.c`,
`chunk_codec.*`, `playerpose.*`, `inventory.*`, `survival.*`, `genversion.*`,
`app/worker.*`, `world/jobq.*`. **That is deliberate and it is the strongest
compatibility guarantee this document makes:** the save format's core, the memory
accountant and the worker are read-only for the whole of v1.9.3.

---

## 7. Integrator seams

| # | Where | Change |
|---|---|---|
| S1 | `main.c` teardown block, 7726-7821 | lift verbatim into `static void worldTeardown(void)`; the existing exit path calls it, unchanged behaviour |
| S2 | `main.c` entry path, 4991-5141 | lift into `static bool worldBringUp(const char* dir, const WorldGen* g)` |
| S3 | `main.c` new | `dimensionSwapTo(int)` = §4 steps 1-18, built from S1 + S2 |
| S4 | `main.c` after `worldExit` in S3 | `assert(budgetUsed() == 0)` — the barrier, and the grep target of 2A.8 |
| S5 | `main.c` place path | after a successful `BLOCK_PORTAL_FRAME` placement, `portalScanAt` |
| S6 | `main.c` S3 steps 3 and 13 | `blockStateSave` before, `blockStateInit` + `blockStateLoad` after; likewise `waterInit` |
| S7 | `main.c` tick | portal dwell/cooldown; when dwell fires, either `networldSendDimEnter` (session) or `dimensionSwapTo` (single player) |
| S8 | `main.c` net callback | `DIM_STATE` for the local sid completes a pending transition and calls `dimensionSwapTo`; for a peer sid it writes `s_peer_dim` |
| S9 | `main.c` player draw | skip any peer whose `s_peer_dim` differs |
| S10 | `main.c` load, after `genVersionResolve` | `dimStateRead(world_dir)`; if `cur_dim != 0`, bring the world up on the dimension directly rather than the overworld |
| S11 | `main.c` death path | force `dimensionSwapTo(DIM_OVERWORLD)` before respawn |
| S12 | `version.h` → 1.9.3, CHANGELOG, whatsnew1.9.3.txt, QR, ROADMAP (End as 1.9.4; lava; atlas full; the 95/96 register ceiling) | release chores |

`tests/dim_wiring_test.c` greps `main.c` for S3, S4, S5, S6, S7, S8, S10, S11. A
feature can land unreachable with every suite green; grep the payoff.

---

## 8. Build order and phase gates

- **Phase 0 (gate: v1.9.2 released).** Ids 44..83, tint row 7, atlas 58..62 are
  spent and their real values are on disk. A and E cannot compute their deltas
  before that. Nobody starts on `mesher.h` or `registry.c` while v1.9.2's lanes hold
  them.
- **Phase 1: A + B in parallel.** Gate: host suite green, `registryCount() == 90`,
  pin 216, the new core CRC recorded verbatim in `registry_test.c:799`.
- **Phase 2: H, the server, and it releases.** Gate: a real server build serves a
  join with `BS_CAP_DIMENSIONS` set and a 90-row registry; `Makefile:444`'s
  `PROTO_COMMIT` moves. **No client build ships before this, and no client build
  that sends 0x14 is even built before this.**
- **Phase 3: C + D + E + F + G in parallel** on A and B's headers. Gate: host suite
  green including `portal_test`, `underforge_gen_test`, `dim_save_test`,
  `dim_budget_test`, `biome_tint_test`; one clean console build.
- **Phase 4: integrator, S1..S12.** Gate: the seam greps, one clean
  `make clean && make`, and an Azahar boot that enters and leaves the dimension
  without the memory readout drifting.
- **Phase 5: playtest, 2B.1-2B.11, both models.** — **this is a phase boundary and
  the build is handed over here, not rolled past.**

**Riskiest item: lane E, and specifically the shbin.** `tintPalette[8] → [16]`
takes the vertex program to 95 of 96 registers. If `picasso` rejects it, or if the
program silently mis-assembles, the failure mode is a *rendering* one on hardware —
which is exactly the class this project has been bitten by (`mesher.h:194-200`'s own
comment: geometry right, colour wrong, every test green). Gate for E: assemble the
shbin and read `picasso`'s register report before writing any C against the wider
bank, and put the six new tint rows in front of a camera on Azahar before calling it
done. The documented retreat is D1's rejected alternative.

**Second riskiest: the swap on real hardware.** Everything in §4 is shipped code,
but it has never been run twice in one process lifetime. `workerStart` after
`workerStop` re-creates threads with 32 KB stacks each (`worker.c:29`) and the New
3DS second lane costs ~114 KB (`worker.h:78`); ten transitions is ten cycles of that.
Playtest 2B.11 is the check, and it is a check that can actually go red.

---

## 9. Observability

An unreproducible transition bug is the likeliest thing to come out of a playtest,
so the instrument ships with the feature rather than after it.

- One `loadprof` CSV row per transition: `dim_from, dim_to, save_ms, teardown_ms,
  barrier_bytes, arrival_gen_ms, arrival_io_ms, portal_found (0/1), total_ms`.
  `barrier_bytes` is `budgetUsed()` at step 8 and must read 0; if it ever does not,
  the number is on disk rather than gone.
- The debug HUD gains one line: current dimension, `s_peer_dim` occupancy,
  last transition total_ms.
- `LOADING_STALLED` (900 frames, `loading.h:36`) already covers a pathological card.

`BS_BOTTOM_UI=1` compiles the overlay out of a shipped build, so anything phrased as
"read the number on screen" is void on a release CIA — the CSV is the channel that
survives.

---

## 10. Test plan with red arms

**B — `dimension_test.c`.** Green: the 1:4 map round-trips for ±100,000; y clamps
at 8 and 119; `dim.dat` round-trips bit-exact against a hand-built byte image;
absent/short/long/bad-magic/bad-version/bad-CRC all yield `{DIM_OVERWORLD, none}`.
Red arms: (1) `over_x/4` written as truncating divide instead of floor →
"map: -1 maps to -1, not 0"; (2) accept a bad CRC → "corrupt dim.dat is overworld";
(3) drop the y clamp → "arrival y 127 leaves no headroom".

**C — `portal_test.c`.** Green: all four legal interiors × two orientations light;
every one-block-off variant does not; the 60-read scan bound holds; `portalPlaceAt`
against solid / air / y=0 / y=127. Red arms: (1) accept a 1-wide interior →
"interior 1 wide is not a portal"; (2) skip the air check on the interior →
"a solid-filled frame does not light"; (3) make `portalPlaceAt` return failure on
solid rock → "arrival must never fail".

**E — `biome_tint_test.c`.** Keep every existing check. Add: 16 rows exist; rows
8..13 are the dimension rows; every row's components are ≤ 1.0 (the hardware clamp);
`meshAoPack(3, 15)` round-trips through `meshAoValue`/`meshAoTint`. Red arms:
(1) leave `MESH_TINT_BITS` at 3 → "row 13 packs as row 5"; (2) drop the six ids from
`blockFaceTintable` → "scorchstone emits MESH_TINT_NONE"; (3) a row with a
component of 1.2 → "tint cannot brighten".

**F — `dim_budget_test.c`.** The barrier. Red arm: remove the release loop before
the claim loop and the peak reads 22,451,616 against a 12,582,912 cap.

**F — `dim_save_test.c`.** A hand-built v1.9.0 world directory. Red arm: make
`dimStateResolve` treat absent as an error → the fixture refuses to load, which is
exactly the regression this feature must never ship.

**G — `networld_test.c`** additions. Green: `sendDimEnter` returns false with caps 0
and with caps `BS_CAP_CHESTS` only; true with the dimensions bit; `DIM_STATE` of the
wrong length is dropped; diffs arriving during the quiet window are dropped; the
3000 ms timeout aborts cleanly. Red arms: (1) drop the caps gate → "old server,
enter must not send"; (2) accept a 5-byte `DIM_STATE` → "strict length"; (3) apply
diffs during the pending window → "old-dimension diff landed in the new world".

**Console-only, no host arm, therefore playtest gates:** the shbin (2B.3, 2B.6), the
swap's thread cycling (2B.11), the arrival timing (the CSV).

---

## 11. Corrections to `docs/plan-1.9.3-dimensions.md`

Recorded rather than silently diverged from. That document is a feasibility study
and its conclusions mostly hold; these specifics do not.

1. **Lava does not exist.** Its §5 builds the fire dimension on *"Lava itself
   already exists as of v1.8.11"*. It does not — one hit tree-wide,
   `cave_carve.h:24`, saying it is deliberately out of scope. `ROADMAP.md:375`
   carries the same wrong claim.
2. **The job-queue drain (its Risk 1, "the one open question") is not open.**
   `workerStop()` (`worker.c:766-773`) plus `workerFlushSaves()` is the drain and
   `main.c:7797` already calls it. §0.3, §4.
3. **Its §9 tile arithmetic was already corrected in its own 2026-09-04 appendix to
   "6 free"; the real number today is 5** (58..62) and **after v1.9.2 it is 0**.
   Its six-tile ask cannot be met by any means. §0.1.
4. **Two new opcodes, but not its two.** `BS_APP_DIM_ENTER` is kept (C→S, correctly
   identified as the server-first driver). `BS_APP_DIM_SYNC` is replaced by
   `BS_APP_DIM_STATE`, which carries a `sid` — without it, peers in another
   dimension cannot be filtered. Both move to 0x14/0x15; its "floor is 0x10" note
   predates 0x11-0x13, which v1.9.0 spent.
5. **The 81-column fixed pocket dimension is dropped.** It streams at the console's
   own radius like the overworld. One-shot 81-column generation is 85-106 ms on host
   and unmeasured on ARM, all of it in front of the player; a 9-column arrival disc
   is 12.3 ms host and the rest arrives behind the fog the engine already has.
6. **`GEN_VERSION` handling is simpler than it proposes.** The dimension's
   `genver.bin` uses the existing constants and the existing `genVersionResolve`
   with a different directory. Zero new code in `genversion.c`.
7. **A second saved player-position field is not added to `player.dat`.** D4: a new
   sidecar, `player.dat` untouched at version 1, which is downgrade-safe as well as
   additive.
8. **`TILE_USED_COUNT` is 49, not 54** (`atlas_tiles.h:139`) — v1.9.2's blueprint
   carries 54 in its header note and again in its §1. The free-slot conclusion is
   unaffected (free slots are the count that matters, and it is 5), but anyone
   deriving from 54 will be off by five.

---

## 12. HIS CALL

Four things, each with a default so no lane is blocked waiting.

1. **The name.** Default `DIM_UNDERFORGE` / "The Underforge" — the old plan doc's
   own favourite, and it ties to Blocksmith rather than to a generic fire word. The
   alternatives it offered were Cinderdeep and Slagmere. Changing it later is a
   string and an enum name, not a design change — but the save directory `dim1` is
   deliberately a number, not a name, so a rename never breaks a world.

2. **The 95th and 96th uniform registers (D1).** Widening the tint bank spends 8 of
   the 9 remaining vertex-shader uniform registers to get the dimension blocks
   looking right in both worlds. The alternative keeps all 9 and accepts that a
   scorchstone carried to the overworld renders in the tundra tint. This is the
   decision most likely to be wrong and it is the one where his taste, not the
   arithmetic, should decide.

3. **One dimension or two.** ROADMAP asks for the fire one *and* the end one. §1
   ships one and defers the End to v1.9.4, on the argument that the machine is the
   cost and the second dimension is a table row. If he wants both in 1.9.3, block
   ids 90..95 and tint rows 14..15 are reserved for it and the cost is roughly lane
   D again plus a second generator's playtest — but the End's floating-island
   terrain is a different generator shape, not a retune, so it is not the cheap
   half it looks like.

4. **1:4 or 1:8 (D5).** 1:4 is chosen so portals actually re-link over the distances
   a 3DS player walks. 1:8 makes the dimension a genuine shortcut and makes portals
   harder to pair. It is one constant, `DIM_RATIO_UNDERFORGE`, and it is a feel
   question the playtest answers better than this document does.

---

## 13. Size estimate

| Lane | Lines add/del | Hours |
|---|---|---|
| A | block.h +40, registry.c +170, registry_test +90, crafting +30 | 5 |
| B | dimension.h 120, dimension.c 260, test 340 | 8 |
| C | portal.h 90, portal.c 380, test 400 | 12 |
| D | worldgen_underforge.h 80, .c 420, test 200 | 12 |
| E | mesher +60/-20, light +90/-15, chunk_render +70/-10, shbin +20/-8, biome_tint_test +180 | 12 |
| F | dim_save_test 320, dim_budget_test 220 | 6 |
| G | networld.h +50, networld.c +260, networld_test +400 | 12 |
| H | server: opcodes, caps, registry, dim-keyed store | 16 |
| Integrator | main.c +260/-190 (mostly the S1/S2 lift), wiring test 160, docs 140 | 10 |

≈ 5,200 lines across 26 files, ≈ 93 agent-hours, plus one server release that has to
land and be deployed before the client is built.
