# v1.8.8 — Biome identity: implementation spec

This is the detailed version of `docs/ROADMAP.md:253-287`'s v1.8.8 entry. It builds on
`docs/research/biome-identity.md` (the biome-identity research brief, 2026-09-01) and does not
redo that research. Where this spec and the brief agree, the brief is cited rather than
repeated; where this spec had to **correct** the brief, that is said out loud with the line the
correction was read off (§3.7, §3.8, §5.4). Where a number could not be reconciled against
another number in this tree, it is left unreconciled and both are shown (§5.3).

It mirrors `docs/plan-1.8.7-terrain.md`'s structure deliberately, including its habit of
splitting "provable by a host test" from "only a look at the screen can settle it" (§1) and of
naming what it could not verify (§7).

This document does not implement anything. It is not itself a change to any file under
`source/`. Every figure below is labelled **measured**, **reasoned** or **assumed**.

---

## 0. What this version actually is, stated once

`docs/ROADMAP.md:286-287` says: *"Groundwork. The block ID ceiling is lifted, client and server
in lockstep. Nearly everything below this line needs it."*

**That sentence is right that there is a ceiling problem and wrong about which ceiling, and the
difference changes the shape of this whole version.** Read off the tree:

- A block id is **8 bits**, `typedef uint8_t BlockId;` (`source/world/block.h:15`), and it is 8
  bits at every single layer independently: in memory (`world/chunk.c:21-28`'s `BlockId* raw`),
  on the wire (`deps/blocksmith-server/proto/bs_proto.h:247`, `BS_BLOCK_EDIT_BYTES` = header +
  4 + 4 + 4 + **1**; decoded at `net/networld.c:507` as `const uint8_t block = msg[13];`), and
  on disk (`world/chunk_codec.c:20`, `#define PALETTE_MAX 256`, *"Palette indices are one byte,
  so 256 distinct ids in a chunk is the ceiling"*).
- The **world-block** ceiling is therefore `REG_ID_DYN_HI` = **0xFD** (`world/registry.h:30`),
  and the compiled-in core span is `REG_ID_CORE_LO`..`REG_ID_CORE_HI` = 0x01..0x7F
  (`registry.h:27-28`) — **127 core ids, of which 15 are used today** (air through
  `BLOCK_FERN` = 14, `block.h:110-120`). **112 free core ids** [reasoned: 127 − 15].
- The **item** ceiling is a different number: `BLOCK_COUNT` = 8 (`block.h:29`), pinned by
  `_Static_assert(BLOCK_COUNT == 8, ...)` at `block.h:121-123`, mirrored server-side as
  `BS_BLOCK_COUNT` 8 (`deps/blocksmith-server/game/validate.h:43`) and tied to it by
  `_Static_assert(BLOCK_COUNT == BS_BLOCK_COUNT, ...)`
  (`deps/blocksmith-server/game/validate.c:146-147`). `block.h:40-50` states this in its own
  words: *"BLOCK_COUNT has exactly one live use left in this client — `inventoryCanHold()`...
  the ceiling on what a slot may carry."*
- **And that item ceiling is not a capacity question. It is a live bug that has already shipped,
  on three blocks, one of which the owner has reported directly.** `scene/interact.c:178` refuses
  a break when `!blockDropsNothing(here) && !inventoryCanHold(here)`, so any block that is (a)
  id ≥ 8 and (b) not `BLOCK_SHAPE_CROSS` cannot be broken at all — **the refusal happens before a
  break timer even starts**. That is snow (10), ice (11) **and** cactus (12), all three plain
  `REG_FLAG_SOLID` full cubes (`registry.c:215-256`). It is also **every cube-shaped block a
  server defines in the dynamic space `0x80..0xFD`**, which makes it a multiplayer correctness
  problem and not only a content one (§6.7). §3.12 has the full mechanism and the evidence.
- Block **placement** stopped being bounded by `BLOCK_COUNT` in v1.6.0. The server's own edit
  validator bounds it by the registry instead: `deps/blocksmith-server/game/validate.c:188`,
  `if (block > REG_ID_DYN_HI) return false;`. The client's mirror is `net/networld.c:462`, the
  same test.

**So: every block v1.8.8 wants to add — per-species logs, planks, leaves, short grass, 2-tall
grass, flowers, a redone dead bush — is an ordinary core registry row in the 112 free ids, needs
no width change, no protocol change, and no save-format change.** This is exactly the pattern
`block.h:61-79` already used for snow/ice/cactus/dead bush/fern in v1.8.3, and the research
brief's B1 reached the same conclusion independently.

**What *does* force client/server lockstep is not the id width. It is two other things, and one
of them is silent:**

1. **Any core registry row at all moves `registryCrc16()`**, which is the join-time fingerprint
   the server sends in `BS_APP_REGISTRY_INFO` (`bs_proto.h:291`, `{rev u8, count u8, crc16 u16
   LE}`). `net/networld.c:279-301` spells out, in the source, what happens when the two builds
   disagree about `kCoreDefs`: the client burns its whole FETCH budget asking for **dynamic**
   rows (core rows are untransmittable — `registryDefUnpack()` refuses any id below
   `REG_ID_DYN_LO`, `registry.c:363`, `:367`), the 2000 ms deadline expires, and *"the client
   enters the world DEGRADED with s_reg_synced false for the rest of the session. Nothing on the
   wire can lift it."* The same comment continues: *"That degraded state is effectively
   invisible to a player... every server-defined id resolves to air through registryView()'s
   never-NULL contract, and a world full of holes reads as terrain."* And then:
   *"a core registry addition FORCES a matching server release, shipped FIRST or simultaneously
   and never client-first."*
2. **The item ceiling itself.** `world/inventory.h:70-73`, `inventoryCanHold()` = `item !=
   ITEM_NONE && item < BLOCK_COUNT`. Moving it is a hand-edit on the server across four files
   (§6.6), not a script run, because the server does **not** call this function — it hard-codes
   `a < BS_BLOCK_COUNT` at `deps/blocksmith-server/game/bsgame.c:1339` and `:1345`. Three things
   need it: the three unbreakable shipped blocks above, every new *cube* this version adds if it
   is to obey the owner's "breakable with its own durability" rule, and the apple
   (`ROADMAP.md:273`, *"picked up and eaten"*).

   **The good news, verified rather than assumed: the item id is already `uint8_t` end to end
   with no truncation anywhere, so lifting this ceiling needs no protocol bump and no save-format
   bump.** `BS_INV_ACTION_BYTES` is header + 1 op + **3 one-byte operands**
   (`bs_proto.h:560`); `BS_INV_OP_PICKUP`/`CONSUME` carry `a = item id`, `b = count`
   (`bs_proto.h:541-548`); `BS_INV_STATE_BYTES` is header + 1 + `BS_INV_SLOT_COUNT` x **2**
   = 50, one byte of id and one of count per slot (`bs_proto.h:574-575`); and the inventory save
   record is the same shape on disk, `buf[20 + i*2 + 0]` for the id
   (`world/inventory.c:357`, `INV_FILE_BYTES` = 20 + 24 x 2 at `inventory.c:210-211`).
   **What is a ceiling is the validation constant, in five places, and nothing else.** §6.6.

**Second framing correction, and it is the one that decides §4's order.** The roadmap and the
research brief both describe the tint as sitting on top of a biome scalar the mesher already
has. It does not. `meshChunk(MeshOut* out, const MeshScratch* s)` (`world/mesher.h:152`) takes
**no world coordinates and no `WorldGen*`**, and `MeshScratch` (`world/scratch.h:53-74`) carries
`blocks`, `light`, `water` and two bools and nothing else. The mesher does not know where it is.
Getting a per-column climate value to it is a real, if small, piece of work (§4 Phase 2), not a
lookup that is already sitting there.

**So this version is:** an item-ceiling lift that fixes three shipped blocks nobody can break
(and every dynamic cube a server defines); a render-side tint (no new ids, no generator change,
no server exposure); a batch of new core registry rows and atlas art (no id-width change, but a
forced server release); a new generator version for the placement of those rows; and three
debug-menu screens. In that order, for the reasons §4 gives.

Nothing here changes `BlockId`'s width, `REGION_VERSION` (`world/region.h:190`, still 1),
`INVENTORY_VERSION` (`world/inventory.c:208`, still 1) or `BS_PROTO_VERSION`
(`deps/blocksmith-server/proto/bs_proto.h:56`, still 1). §6 says why each of those four is left
alone and what it would cost to move them — and in `INVENTORY_VERSION`'s case, why moving it
would make the compatibility story **worse**, not safer (§6.6).

---

## 1. Success criterion

Split in two, because conflating them is the mistake this spec is written to avoid. The
project's standing rule is that a visual claim needs a look, not a diff read.

### A. Measurable, host-only — `tools/run_host_tests.sh` green, with these specific claims

1. **The generator did not move for existing worlds.** A pinned-hash test over the twelve
   columns this tree already uses for exactly this purpose — seeds 1337 / 4242 / 90210 crossed
   with columns (0,0) (1,0) (−1,−1) (7,−3), the set named in `world/genversion.h:82-84` — shows
   **byte-identical** column output at `GEN_VERSION_LEGACY`, `GEN_VERSION_DENSITY` and
   `GEN_VERSION_BIOME` before and after this version, and **different** output at the newly
   minted `GEN_VERSION_4`. Both halves matter: identical proves old worlds are safe, different
   proves the new content is actually reached.
2. **The tint is render-side only.** The same twelve-column hash test, run with the tint code
   present and with it removed, produces identical hashes. If it does not, the tint has leaked
   into generation and §4 Phase 2 is wrong.
3. **Snow, ice and cactus break and drop themselves — and so does every block this version
   adds.** Stated as the concrete thing, not as a capacity claim, because it is a bug fix
   (§0, §3.12). The test asserts, for ids **10, 11, 12** and for every id this version adds:
   `inventoryCanHold(id)` is **true**; the `interact.c:178` guard does **not** refuse the break;
   `blockIsTargetable(id)` is true; `breakTicksRequired(id, …)` returns the row's own hardness
   rather than a shared default; and a full break puts one of that id into an empty inventory
   slot. It is armed red by construction: **all three fail today**, and if the fix is
   incomplete only some of them will pass. It also walks the whole `registryIsDefined()` span
   (`registry.h:135`) and asserts no defined, targetable, non-liquid id is refused — which is
   what catches the dynamic `0x80..0xFD` cubes (§6.7) rather than only the three core ones.
4. **The registry fingerprint moved exactly once and is pinned to its new value.**
   `world/registry_test.c:465-466`'s golden (`0x189B` today) moves to one new pinned value, and
   `REGISTRY_FULL_COUNT_PIN` (`registry_test.c:60`, 141 today = 15 core + 126 dyn) moves to
   match the new core-row count. Both are hand-edited, both are asserted.
5. **The merge does not smear tint across a biome border.** A mesher test builds a chunk whose
   grass-top row spans two different tint indices and asserts the emitted quads split at the
   index change rather than merging across it (§9). Armed red by removing the tint term from
   `faceFlatKey` and confirming the test fails.
6. **The atlas still fits.** `world/block_tiles_check.c`'s assert-count check and
   `world/atlas_uv_shader_test.c` stay green with the new tiles, and `TILE_USED_COUNT`
   (`gfx/atlas_tiles.h:60`) is still below `ATLAS_TILE_MISSING` (63, `world/atlas_uv.h:120`).

### B. Visual, playtest-only — the claims a green suite cannot settle

1. **Biomes read as different places.** Walking from plains into jungle, and from taiga into
   tundra, the grass changes colour, and the change is legible without being lurid.
2. **Grass strands match the block they stand on.** A tall-grass or fern blade on a tinted grass
   block is the same hue as that block's top face, at every biome, with no visible seam between
   the plant and the ground.
3. **The half-grass/half-dirt block still looks like the block steve praised** (requirement 8).
   This is the one that a diff cannot answer at all, because §4 Phase 2 desaturates
   `tile_grass_top` (`tools/make_atlas.py:201-210`) so the tint has room to work — the *top*
   face changes by construction. The side face is untouched, and whether the two still read as
   one block is the question. **See §9 and §8 item 3: there is a known hue-mismatch risk here
   and it has an escape hatch.**
4. **The tint bands do not read as stripes.** A 16-entry lookup table gives 16 flat hues with
   hard steps between them; it is not a gradient (§5.5). Whether those steps are invisible or
   look like contour lines on a map is a look, not a number.
5. **The neon biome-border overlay lands on the boundary the debug readout names**, and the
   block list is legible on the bottom screen at 320x240.

Both halves must hold. A green suite with no look is unverified.

---

## 2. What is worth reproducing from the reference, and at what scale

Drawn from `docs/research/biome-identity.md` Part A, restated only as far as it bears on §4.
Minecraft is **visual reference only**: nothing borrowed, no assets, no ported code, and every
tile generated by `tools/make_atlas.py`, which states the project's own rule in its own words at
`tools/make_atlas.py:3-6` (*"nothing is traced, sampled or copied from another game"*).

- **Tint carries biome colour; block ids carry material.** Reference: a 256x256 colormap indexed
  by temperature x downfall (brief A1). **Blocksmith's scale: 16 entries, not 65,536** — a 4x4
  quantisation of the same two axes, because the carrier is 4 spare bits of an 8-byte vertex,
  not a texture fetch (§5.5 does the arithmetic). Blocksmith already has both continuous axes
  and needs no new noise field: `worldgenBiome()` (`world/worldgen.h:503`) and
  `worldgenHumidity()` (`worldgen.h:517`), with temperature defined as the inverted biome field
  (`worldgen.h:298`).
- **Only some faces get tinted.** Reference: the grass block's top is tinted, its sides are a
  never-tinted dirt base plus a tinted alpha overlay, two texture samples per fragment
  (brief A2). **Blocksmith declines the overlay** (brief C4): it costs a second texture unit and
  two more TexEnv stages on a part with no programmable fragment stage, and the side face is the
  block the owner explicitly asked to keep. Tinted here: grass **top** face, leaf blocks, and
  `BLOCK_SHAPE_CROSS` plant geometry. Everything else takes the neutral entry.
- **Biome blend is baked at chunk-build time, not per frame** (brief A3, and vanilla's own
  "affects chunk update time rather than steady-state FPS"). Blocksmith bakes light and AO into
  `MeshVertex` at remesh already (`world/mesh_vertex.h:11-27`); the tint index rides the same
  path and costs nothing per frame afterwards.
- **One shape, many biomes; density varies, id does not.** Reference: double plants are two
  block states, not one per biome. This codebase already ships that pattern — `BLOCK_FERN` is
  one id used in taiga and jungle, differing only in a per-biome chance table
  (`worldgen.h:GEN_FERN_TAIGA` 20 / `GEN_FERN_JUNGLE` 32). §4 Phase 4 follows it exactly.
- **Species, not biomes, is the axis for real blocks.** The roadmap asks for per-biome wood,
  leaves and planks (`ROADMAP.md:261-263`). Six biomes x (log side, log top, planks, leaves)
  would be 24 atlas tiles. Only four of the six biomes grow trees at all — tundra and desert are
  0 (`worldgen.h`'s `GEN_TREE_TUNDRA 0`, `GEN_TREE_DESERT 0`) — and plains and forest can share
  a species, so **three wood species (the existing oak, plus spruce and jungle) is 8 new tiles,
  not 24** [reasoned]. §5.2 does that budget against the 46 free atlas slots.

Nothing in this section calls for a new noise field, a taller world, a wider block id, or a
fragment shader.

---

## 3. Current state, honestly, with file:line evidence

Every citation below was opened and read at the line quoted. Two anchors the research brief
cites had **moved** since it was written and are corrected here rather than repeated (§3.9) —
that is the same failure `docs/plan-1.8.7-terrain.md:330-337` records twice already, and this
spec is not adding a third.

### 3.1 The id spaces

- `world/block.h:15` — `typedef uint8_t BlockId;`
- `world/registry.h:25-31` — `REG_ID_AIR` 0x00, `REG_ID_CORE_LO` 0x01, `REG_ID_CORE_HI` 0x7F,
  `REG_ID_DYN_LO` 0x80, `REG_ID_DYN_HI` 0xFD, with `0xFE/0xFF reserved so a u8 count can never
  overflow`.
- `world/registry.h:33` — `#define REGISTRY_MAX 256`. Every id-indexed table in the client is
  already sized to it: `world/mesher.c:197` `s_rect[REGISTRY_MAX][BLOCK_FACES]`,
  `world/light.c:32,36`, `world/registry.c:282-283` `s_defs`/`s_used`/`s_view`. **Adding core
  rows allocates nothing.**
- `world/block.h:29` — `BLOCK_COUNT` closes the item enum at 8; `block.h:121-123` asserts it
  never moves; `block.h:124-129` asserts ids 8..14 never move.
- `world/inventory.h:33` — `typedef BlockId ItemId;`. `inventory.h:41-67` is the current, honest
  statement of what the ceiling is for: *"The ceiling is BLOCK_COUNT and NOT the registry's full
  256-id space, on purpose — but the reason is a WIRE AGREEMENT, not memory safety."* It also
  flags that `deps/blocksmith-server/game/validate.h` still states the **old**, no-longer-true
  memory-safety rationale — do not lean on that one.

### 3.2 The wire

- `deps/blocksmith-server/proto/bs_proto.h:56` — `#define BS_PROTO_VERSION 1u`. Enforced by
  dropping the datagram: client `net/bsnet_transport.c:570`, server
  `deps/blocksmith-server/gateway/bsgate.c:867`. The server sends **no error packet, ever**
  (`bsgate.c:415-419`) — an old client meeting a bumped version gets silence, not a message.
- `bs_proto.h:247` — `BS_BLOCK_EDIT_BYTES` = header + 4 + 4 + 4 + **1**. One byte for the id.
  Encoded at `net/networld.c:1118` (`out[13] = block;`), decoded at `networld.c:507`.
- `bs_proto.h:397` — `BS_SYNC_ENTRY_BYTES 13u`, reused by `BS_APP_CHUNK_DIFFS`
  (`bs_proto.h:451`). Decoded at `networld.c:531` (`const uint8_t block = p[12];`).
- `bs_proto.h:291-300` — `BS_APP_REGISTRY_INFO` 0x0C `{rev u8, count u8, crc16 u16 LE}`,
  `BS_APP_REGISTRY_FETCH` 0x0D, `BS_APP_REGISTRY_DEFS` 0x0E `{first u8, n u8, last u8, n x 28B
  records}`. `world/registry.h:99-102` pins the record at 28 bytes = id byte + a 27-byte
  `BlockDef` (`registry.h:96`).
- `world/registry.h:137-143` — `registryCrc16()` is CRC-16/CCITT-FALSE **over every defined id
  in ascending order, core rows included**. This is the fingerprint §0 item 1 is about.

### 3.3 The save

- `world/region.h:190` — `#define REGION_VERSION 1`, read at `world/region.c:124`, written at
  `region.c:162`. `region.h:187-190`: *"A file with a version this build does not know is
  ignored, not guessed at: the world regenerates from the seed, which loses player edits but
  cannot corrupt anything."* There is no migration code anywhere in `region.c`.
- `world/chunk_codec.c:20` — `#define PALETTE_MAX 256`, one byte per palette index.
  `chunk_codec.c:107-108` (UNIFORM), `:67-68` (RLE palette), `:90-92` (RAW) — every id written
  is one byte.
- `world/genversion.h:68,73,97` — `GEN_VERSION_LEGACY` 1, `GEN_VERSION_DENSITY` 2,
  `GEN_VERSION_BIOME` 3; `:101` `GEN_VERSION_NEWEST`; `:104`
  `GEN_VERSION_FOR_NEW_WORLDS`.

### 3.4 The vendored server, and how the two trees are kept together

- `deps/blocksmith-server` is a **separate git repository**, not a submodule; `mc/.gitignore:21`
  is a bare `deps/`, which is why the Grep tool cannot see it (this bit a previous session; use
  bash `grep -rn` with an explicit path).
- `deps/blocksmith-server/tools/sync-world-sources.sh:46` — `FILES=(block.h inventory.h
  inventory.c crafting.h crafting.c crc32.h crc32.c registry.h registry.c tick.h tick.c)`.
  These eleven files are byte-identical copies of `mc/source/world/`.
- `deps/blocksmith-server/game/Makefile:185-208` — target `check-world-drift`, `cmp -s` per
  vendored file, fails the build on any diff **when a client tree is beside it**; a **no-op in
  the deployed standalone build** (`Makefile:206-207`). So a stale vendored copy can ship
  undetected in production. `deps/blocksmith-server/game/validate.h:35-41` records that
  happening for real once already, when `BLOCK_PLANKS` was appended client-side and
  `bsEditValid()` silently refused every planks placement.
- **The server does *not* call `inventoryCanHold()`.** `deps/blocksmith-server/game/bsgame.c:1339`
  and `:1345` hard-code `a < BS_BLOCK_COUNT` for `BS_INV_OP_PICKUP`/`CONSUME`;
  `deps/blocksmith-server/game/playerstate.c:14` uses `BS_BLOCK_COUNT` for armour slots. So
  widening `inventoryCanHold()` in the vendored `inventory.h` and re-running the sync script
  does **not** fix the server. That is §6.4.

### 3.5 The vertex, and where a tint index can live

- `world/mesh_vertex.h:11-27` — the 8-byte `MeshVertex`: `int8_t x,y,z; int8_t pad; uint8_t u,v;
  uint8_t nrm; uint8_t ao;`. `:64-66` asserts `offsetof(MeshVertex, u) == 4` (the alignment
  invariant that froze the console from v1.1.0 to v1.2.4); `:70` asserts `sizeof == 8`.
- `ao` is `uint8_t` and only ever holds **0..3** — written at `world/mesher.c:420`
  (`v->ao = cornerAO(si, c)`) and `world/mesher.c:690` (`v->ao = 3` for every CROSS vertex).
  **6 spare bits, at zero cost to the stride and zero risk to the alignment invariant.**
- The shader already does dynamic indexing into a uniform array from a packed vertex byte:
  `source/shaders/world_dynamic.v.pica:103-104`, `mova a0.x, inpack.zzzz` /
  `mov r3, faceShade[a0.x]`. A tint LUT is the same idiom a second time.
- What today's shader calls colour is **one scalar broadcast to three channels**:
  `world_dynamic.v.pica:176-178`, `mul r6.y, r3.xxxx, r4.xxxx` / `mul r6.z, r6.yyyy, r5.wwww` /
  `mov outclr.xyz, r6.zzz`. There has never been a hue in this pipeline, only brightness.

### 3.6 Two shaders, one of them never bound, both of them tested

`source/shaders/world.v.pica` and `world_dynamic.v.pica` both exist. Since v1.8.0 task 24,
`scene/chunk_render.c` binds **only** `world_dynamic` — `world/atlas_uv_shader_test.c:601-624`
says so and checks it (*"chunkRenderInit parses world_dynamic_shbin and parses world_shbin
nowhere"*). But `atlas_uv_shader_test.c:146-147` and `world/water_alpha_test.c:29` **parse the
source text of both files** and require them to agree. `world_dynamic.v.pica:181-186` says the
same thing inline: *"Must match world.v.pica exactly."*

**Consequence for §4 Phase 2: a tint change must be written into both `.pica` files or the host
suite goes red on a file that is never on screen.** This is not obvious from the roadmap or from
running the game.

### 3.7 The mesher DOES merge runs — the research brief's headline is stale

`world/mesher.h:8` still reads *"Deliberately **not** greedy meshing"*, and
`docs/research/biome-identity.md:32-36` takes that as its headline finding. **The comment is out
of date.** The code merges coplanar faces along U:

- `world/mesher.c:533-549`, `faceFlatKey()` — *"The shading signature of one face, or 0 if it may
  not be merged at all... Two faces merge iff their keys are equal and non-zero"*, returning
  `0x10000u | ((uint32_t)ao << 8) | pad`.
- `world/mesher.c:553-560`, `mergeRun()` — *"How far a run starting at this cell may reach: the
  merge cap, the chunk edge, and then the actual cells."*
- `world/atlas_uv.h:137` — `#define ATLAS_MAX_MERGE_BLOCKS 15`.
- `world/mesher.c:472` — `static uint8_t s_face_done[SCRATCH_BLOCKS]`, the per-face
  already-consumed table a run walk needs.

This is the single most consequential correction in this spec, and §9 is written about it.

### 3.8 The mesher has no idea where it is

`world/mesher.h:152` — `void meshChunk(MeshOut* out, const MeshScratch* s);`. No coordinates, no
`WorldGen*`. `world/scratch.h:53-74` — `MeshScratch` is `blocks[5832]`, `light[5832]`,
`water[5832]`, `water_any`, `has_water`. `world/scratch.h:21-22` — `SCRATCH_DIM` 18,
`SCRATCH_BLOCKS` 5832. The one instance is `static MeshScratch s_scratch;`
(`scene/chunk_render.c:674`), filled at `chunk_render.c:1302` (`scratchFill(&s_scratch, w, cx,
cy, cz)`) and meshed at `:1329`. The caller has `(cx, cy, cz)`; the mesher does not.

`docs/research/biome-identity.md:435-438` assumes the mesher "currently reads
`worldgenBiomeAt`/temperature/humidity for a column". It does not, and nothing in `mesher.c`
includes `worldgen.h`.

### 3.9 Anchors in the research brief that have moved

- The brief cites `main.c:1119-1174` for `bsDebugRegister`. It is at **`main.c:1139`**.
- The brief cites `main.c:4285-4287` for the `debugMenuUiOpen()` call site. It is at
  **`main.c:4309`**.

Both were followed to the line before being used here. The brief's other anchors that this spec
relies on — `scene/ui.c:184`, `scene/highlight.c:16-18`, `tools/make_atlas.py:201-210`,
`world/registry.c:250-256` — were checked and are correct.

### 3.10 The debug menu, and what it can and cannot hold

- `app/debugmenu.h:17` — `DEBUG_MENU_MAX_ENTRIES 32`; `:20-24` — four kinds, `DEBUG_TOGGLE`,
  `DEBUG_SLIDER_INT`, `DEBUG_ACTION`, `DEBUG_INFO`; `:27-49` — the entry struct. **No icon,
  image or grid concept anywhere.**
- `main.c:1139` — `bsDebugRegister()`, which is the registration order and therefore the row
  order (`main.c:1156-1159` says so). `main.c:4309` — `debugMenuUiOpen()`.
- `registry.h:135` — `bool registryIsDefined(BlockId id);`. **This resolves the research brief's
  open question 4**: the block list needs no new enumeration API; walking 0..0xFD and filtering
  on `registryIsDefined()` is the enumeration, and `registryCount()` (`registry.h:133`) is the
  expected total.
- `scene/ui.c:182-184` — `iconUv()`, already turning a `BlockId` into an atlas rect via
  `atlasTile(blockFaceTex(id, FACE_TOP))`. Direct reuse for the block list.
- `scene/highlight.c:7-18` — the block cursor is twelve thin boxes with a position-only float
  vertex and *"Colour comes from a TEV constant instead of a vertex attribute"*. Direct reuse
  for the neon border overlay.

### 3.11 The atlas

- `gfx/atlas.h:3-7` — one 16x1024 RGBA5551 strip, 64 slots of 16x16, 1024 px being the PICA200's
  maximum texture dimension, so **64 is permanent**.
- `gfx/atlas_tiles.h:25-60` — 17 named tiles, `TILE_GRASS_TOP` = 0 through `TILE_FERN` = 16, then
  `TILE_USED_COUNT`.
- `world/atlas_uv.h:120` — `ATLAS_TILE_MISSING` = slot 63, reserved forever.
- `tools/make_atlas.py:55-58` — *"there are exactly **46 free slots** for the rest of the
  project's life"*, and past that the sheet cannot be made taller.
- `tools/make_atlas.py:201-210` — `tile_grass_top`, fully saturated green today: base colours
  (58,112,74), (66,126,82), (74,138,90), (50,100,66), highlights at (88,152,100).

### 3.12 The unbreakable blocks — the exact mechanism, and how far it reaches

**Three shipped blocks cannot be broken, the reason is one line, and it is the same line the
apple needs. This is the version's foundation, not a footnote.**

The guard, `scene/interact.c:178`:

```c
if (!blockDropsNothing(here) && !inventoryCanHold(here)) {
        breakCancel(it);
        if (pressed) it->refused++;
        return 0;
}
```

Its two terms:

- `blockDropsNothing(id)` (`world/block.h:258-261`) is `shape == BLOCK_SHAPE_CROSS`, nothing
  more. It answers from the **shape**, not from an id.
- `inventoryCanHold(id)` (`world/inventory.h:70-73`) is `item != ITEM_NONE && item <
  BLOCK_COUNT`, and `BLOCK_COUNT` is **8** (`block.h:29`, asserted `block.h:121-123`).

So the break is refused, **before a break timer starts**, for any block that is both id ≥ 8 and
not CROSS. Read off `registry.c` row by row:

| id | Block | Flags | Shape | Result today |
|---|---|---|---|---|
| 8 | water | `TRANSPARENT \| LIQUID` (`registry.c:126-130`) | FULL_CUBE | Not targetable at all — `blockIsTargetable()` is `drawn && !liquid`; the crosshair passes through. Correct, and must stay so |
| 9 | tall_grass | `TRANSPARENT \| SHAPE(CROSS)` | CROSS | **Breaks**, drops nothing |
| 10 | snow | `REG_FLAG_SOLID` (`registry.c:215-221`) | FULL_CUBE | **Refused. Unbreakable** |
| 11 | ice | `REG_FLAG_SOLID` (`registry.c:238-243`) | FULL_CUBE | **Refused. Unbreakable** |
| 12 | cactus | `REG_FLAG_SOLID` (`registry.c:250-256`) | FULL_CUBE | **Refused. Unbreakable** |
| 13 | dead_bush | `TRANSPARENT \| SHAPE(CROSS)` | CROSS | **Breaks**, drops nothing |
| 14 | fern | `TRANSPARENT \| SHAPE(CROSS)` | CROSS | **Breaks**, drops nothing |

That table is the answer to a question the owner would reasonably ask — why do *some* blocks
above the ceiling break and others not. The CROSS ones break because `blockDropsNothing()`
short-circuits the guard; they simply yield nothing.

**Cactus's `.hardness = 8` is perfectly good and is simply never read.** `registry.c:206-209`
says so in its own words: *"Only the two CROSS rows can ever be asked (the three cubes are
refused before a break timer starts), but every row carries a real number anyway."*

**This was a deliberate decision, not an accident, and it has now been overruled.**
`registry.c:194-199` describes snow, ice and cactus as *"scenery: you can walk on them, aim at
them and build against them, and you cannot mine them"*, and `block.h:70-73` calls it the same
end state argued for water. `ROADMAP.md:277` now calls it a bug. Saying that plainly matters:
the fix is not repairing a mistake somebody made, it is reversing a documented judgement, and
`registry.c:194-209` and `block.h:70-73` are the comments that have to be rewritten along with
the code, or the next reader will restore the old behaviour believing it was intended.

**The tree already knows the exact fix and wrote the instruction down.**
`scene/interact.c:176-177`:

> ⚠ Delete this branch when inventory becomes registry-aware — i.e. when `inventoryCanHold()`
> widens past `BLOCK_COUNT` on both this client and the server.

**And it reaches further than three blocks.** The guard tests `inventoryCanHold()`, which is
bounded at 8 — so **every** cube-shaped block a server defines in the dynamic space
`0x80..0xFD` (`registry.c:383`, applied at `registry.c:492-499`) is unbreakable on the client
too. A server can define a block, place it in the world, and no client can mine it. That is a
multiplayer correctness problem, and §6.7 states it as one.

`interact.c:170-174` explains why the guard is where it is, and it is worth keeping when the
predicate widens: this function is the **only** origin of a player-initiated break in the
client, so refusing here also refuses the `BS_APP_BLOCK_EDIT` — *"which matters, because the
server would have honoured the removal and then dropped the matching `BS_INV_OP_PICKUP`
(`deps/blocksmith-server/game/bsgame.c` mirrors the same ceiling), destroying the block for
every player on the server, not just this one."* The guard is not the bug. The **constant** is.

For reference, the scale every new row is tuned against — `world/registry.c:17-38`, toolless
ticks at 20 TPS: grass/dirt 12, sand 10, stone 45, wood/planks 40, leaves 4, tall grass 1.

### 3.13 The predicate that replaces `item < BLOCK_COUNT`, and why no flag bit is needed

`BlockDef.flags` has **no free bit** — `registry.h:37-43` uses bits 0..4 for behaviour and
`registry.h:57-59` uses bits 5..7 for the shape, with `registry.h:67-69` asserting they never
meet. So a new `REG_FLAG_ITEM` is not available, and this looked like a blocker.

It is not, because the flag that means *"cannot be aimed at, mined, or placed against"* already
exists and already does exactly this job for water: `REG_FLAG_LIQUID`
(`registry.c:117-119`, and `blockIsTargetable()` is `drawn && !liquid`). Water's row is
`.flags = REG_FLAG_TRANSPARENT | REG_FLAG_LIQUID` (`registry.c:130`). So:

```c
inventoryCanHold(item) := item != ITEM_NONE
                       && registryIsDefined(item)
                       && !(registryView(item)->flags & REG_FLAG_LIQUID)
```

keeps water uncarryable — the one exclusion `block.h:53` insists on — using a flag that is
already set, already synced to the server (`registry.h` is in the sync list, §3.4), and already
carried on the wire in byte 23 of every 28-byte registry record (`registry.c:465`, `:483`). No
new bit, no new field, no wire change. `registryIsDefined()` (`registry.h:135`) and
`registryView()`'s never-NULL contract (`registry.h:120-121`) are both already there.

The one real consequence: `inventoryCanHold()` stops being a `static inline` over a constant and
gains a registry dependency. `world/inventory.h` is one of the eleven files
`sync-world-sources.sh` copies (§3.4), and so is `registry.h`, so both trees get it — but the
server's own call sites do **not** go through it (§3.4, §6.6) and must be changed by hand.

### 3.14 A stale comment that will mislead the next person who touches the inventory

`source/gfx/sprite.c:22-28` sizes `SPRITE_MAX_QUADS` against *"the bottom screen's full
inventory grid — 9x5 slots... so 45 * 4 = 180 quads"*. The inventory is **24 slots**, not 45:
`INV_HOTBAR_SLOTS` 8 + `INV_MAIN_COLS` 8 x `INV_MAIN_ROWS` 2 = 24
(`world/inventory.h:103-107`, and `bs_proto.h:488` mirrors it as `BS_INV_SLOT_COUNT 24u`).

The buffer is **oversized, not short** — 1024 quads against a real worst case well under the
comment's own 550 — so this is cosmetic today and nothing in this version is affected.
**Flagged, deliberately not fixed: `source/gfx/sprite.c` is not this plan's file and nobody
asked for it to change.** It is written down here because it is exactly the stale assumption
someone widening the inventory would trust, and because the same paragraph's sibling in
`inventory.h:99` (*"the 6 items this game currently has"*) is stale in the same direction —
`BLOCK_COUNT` is 8, not 6.

### 3.15 There is no health or hunger system

`net/networld.h:189` — *"this client has no armour, XP, health or hunger systems yet"*; `:219`
repeats it. `networld.h:198` receives a `hunger` float from the server that nothing consumes.
`net/inv_bridge.c:103` can send `BS_INV_OP_CONSUME`, but there is no meter for an apple to
restore. **"Eaten" (`ROADMAP.md:273`) cannot be delivered in this version without inventing a
hunger system, which is not in this version.** §4 Phase 5 puts this to the owner rather than
picking.

---

## 4. Phases

Each phase is independently testable. The order is chosen so the cheapest thing that removes the
most uncertainty goes first, and so the one irreversible obligation — the forced server release
(§0 item 1) — is not incurred until the phase that actually needs it.

**Why `ROADMAP.md:286-287`'s "block ID ceiling is lifted" is Phase 1 but not for the reason the
roadmap gives.** The *block* id width needs no change (§0). The *item* ceiling does, and it is
not a prerequisite in the abstract — it is the fix for three shipped blocks nobody can break
(§3.12) plus every dynamic cube a server defines (§6.7), and it is what makes the owner's
"every new block is breakable with its own durability" rule achievable for any cube this version
adds. So it goes first, as a bug fix that happens to unblock the rest, and its acceptance test
is a concrete observable — snow, ice and cactus break and drop themselves — not a capacity
assertion. Phase 2 (tint) is the highest-risk *engineering* in the version and has zero lockstep
exposure, which is why it precedes Phase 3, the phase that commits to a server release.

### Phase 1 — Lift the item ceiling; snow, ice and cactus become breakable

**This is a bug fix (§3.12) and it is also the version's foundation. It is the one phase whose
success criterion is a thing the owner can see for himself in thirty seconds.**

**Outcome.** `inventoryCanHold()` becomes registry-aware on both client and server; snow, ice
and cactus break at their own already-tuned hardnesses (8, 10, 8 ticks — `registry.c:215-256`)
and drop themselves; every cube-shaped dynamic block a server defines becomes breakable too.
Water stays uncarryable and untargetable.

**What to build, client side:**

1. **`world/inventory.h:70-73`** — replace `item < BLOCK_COUNT` with the registry-aware
   predicate from §3.13: defined, and not `REG_FLAG_LIQUID`. No new flag bit is needed and no
   record on the wire or on disk changes shape (§0 item 2). The function stops being a
   `static inline` over a constant; `inventory.h`'s own comment block at `:41-67`, which
   currently explains the old ceiling, is rewritten in the same change.
2. **`world/block.h:29,40-59,121-123`** — `BLOCK_COUNT` stops being the item ceiling. It stays
   as the item-enum terminator and its `_Static_assert` stays at 8; what changes is the
   paragraph at `:40-50` that names `inventoryCanHold()` as its one live use, which is no longer
   true. **Do not delete the assert** — `block.h:124-129`'s sibling asserts are what keep ids
   8..14 from moving.
3. **`scene/interact.c:176-177`** — the `⚠ Delete this branch when inventory becomes
   registry-aware` instruction is now satisfiable. **Keep the branch, delete the ⚠** — the guard
   still has a job (an undefined id, or a liquid) and `interact.c:170-174` explains why it must
   stay at the *start* of the break rather than at the end: without it the server honours the
   removal and drops the pickup, destroying the block for every player.
4. **`world/registry.c:194-209`** and **`world/block.h:70-73`** — rewrite the comments that
   describe snow, ice and cactus as deliberate unminable scenery. Leaving them is how the
   behaviour gets restored later by someone who reads them and believes it was intended (§3.12).
5. **`world/inventory.c`** — nothing changes. `inventoryLoad()`'s filter at `:364` already calls
   `inventoryCanHold()`, so it widens for free, and `INVENTORY_VERSION` does **not** move (§6.6
   explains why moving it would be actively worse).

**What to build, server side — by hand, in a tree this client does not own:**

6. Run `deps/blocksmith-server/tools/sync-world-sources.sh` for `inventory.h` / `registry.h`,
   then edit, individually: `game/validate.h:43` (`BS_BLOCK_COUNT`, and its stale
   memory-safety rationale that `inventory.h:41-67` already flags as wrong),
   `game/validate.c:146-147` (the `BLOCK_COUNT == BS_BLOCK_COUNT` drift assert),
   `game/bsgame.c:1339` and `:1345` (`a < BS_BLOCK_COUNT` for `BS_INV_OP_PICKUP` and
   `BS_INV_OP_CONSUME` — these do **not** call `inventoryCanHold()`, which is why the sync
   script does not carry the fix), `game/playerstate.c:14`, and the operand comments at
   `proto/bs_proto.h:541` and `:545` that document the bound as `< BS_BLOCK_COUNT`.
   Build with a client tree beside it so `check-world-drift` (`game/Makefile:185-208`) actually
   runs — it is a **no-op** in a standalone build (`Makefile:206-207`).

**Plus the audit that turns §0's arithmetic into checks** (`world/registry_test.c` is the home;
**not** `source/world/world_test.c`, which another workstream owns): assert
`REG_ID_CORE_HI - REG_ID_CORE_LO + 1 == 127` and that 15 core ids are defined, so the 112 free
ids are a checked number; assert `sizeof(BlockId) == 1` and `REGISTRY_MAX == 256` so a later
widening cannot happen quietly.

**Proof.** §1.A item 3, which fails on all three blocks today and therefore cannot pass by luck.
Plus a live multiplayer session: break a cactus on a server and confirm the block is gone **and**
the item arrives — that round trip crosses `BS_APP_BLOCK_EDIT`, `BS_INV_OP_PICKUP` and
`BS_INV_STATE`, and no host test reaches it.

**Cost.** Days, most of it the server hand-edit and the round-trip test. No console strictly
needed for the host half.

### Phase 2 — Tint (render side only, no new block ids, no generator change)

This is the version's headline feature and its riskiest engineering, and it touches nothing the
server or the save format can see. That is why it is second.

**Outcome.** Grass tops, leaves and CROSS-shape plants take a per-column climate hue; everything
else is unchanged, byte for byte, including generated terrain.

**What to build, in the order it should land:**

1. **`tools/make_atlas.py`** — desaturate `tile_grass_top` (`make_atlas.py:201-210`) toward a
   near-neutral base so `GPU_MODULATE` has room to carry the hue. Same for the leaf tile and any
   grass-strand tile meant to tint. **Do not touch `tile_grass_side`** — that is requirement 8,
   and §8 item 3 is the escape hatch if the untinted side and the tinted top stop reading as one
   block.
2. **`world/scratch.h`** — add `uint8_t tint[SCRATCH_DIM * SCRATCH_DIM];` to `MeshScratch`. That
   is 18 x 18 = **324 bytes**, a per-*column* band, not a per-cell one (§5.1). Add
   `void scratchFillTint(MeshScratch* s, const WorldGen* g, int cx, int cz);`.
3. **`scene/chunk_render.c`** — call `scratchFillTint` beside the existing `scratchFill` at
   `chunk_render.c:1302`. The caller already has `(cx, cy, cz)`; the mesher never will (§3.8).
4. **`world/mesher.c`** — pack the index into the spare bits of `ao`:
   `v->ao = (uint8_t)((ao_value & 0x3) | (tint_index << 2))`, at both write sites
   (`mesher.c:420` for cube faces, `mesher.c:690` for CROSS). Non-tinted blocks get index 0.
   **Add the tint index to `faceFlatKey()`'s returned key** (`mesher.c:548`) — `| (tint << 17)`
   fits the existing `uint32_t` with no widening, because bits 17..31 are free. §9 is why this
   step is not optional.
5. **Both shaders** — `source/shaders/world_dynamic.v.pica` **and** `source/shaders/world.v.pica`
   (§3.6): one `.fvec tintLUT[16]`, a mask/shift to lift the index out of the ao component, one
   `mova`/lookup mirroring `world_dynamic.v.pica:103-104`, and one `mul` so
   `mov outclr.xyz, r6.zzz` (`:178`) becomes a multiply by `tintLUT[...].xyz`. Entry 0 is
   RGB (1,1,1) so every untinted block multiplies by white.
6. **A generator for the 16 LUT values**, written in Python under `tools/` alongside
   `make_atlas.py`, producing the palette procedurally from the six-biome table in
   `worldgen.h:288-296` rather than by hand-picking colours. Same rule as every other colour in
   this game.
7. **`world/mesh_vertex.h`** — document the repurposed `ao` bits. No size change, no layout
   change, the two `_Static_assert`s at `:64-70` untouched.

**Proof.** §1.A items 1, 2 and 5, plus §1.B items 1, 2, 3 and 4. Note that item 2 (tint is
render-side only) is the claim §0's second half depends on, and it is proven by running the
twelve-column hash test with and without this phase's code, not by reading the diff.

**Cost.** §5.1 and §5.5.

### Phase 3 — New core registry rows, atlas art and durability

**This is the phase that incurs the forced server release** (§0 item 1, `net/networld.c:279-301`).
Nothing before it does, and nothing after it can undo it.

**Outcome.** Every block this version adds exists as a registry row with real art and a real
hardness, and can be placed by hand from a creative-style flow, but **nothing generates it yet**
— placement is Phase 4. Splitting it this way means the registry/CRC/server work is proven
before any generator version is minted.

**What to build:**

1. **`tools/make_atlas.py`** — the new tiles, appended after `TILE_FERN` (16), and the redone
   `tile_dead_bush` (a rewrite of the existing generator function, so no new slot). Budget in
   §5.2.
2. **`gfx/atlas_tiles.h`** — the matching names above `TILE_USED_COUNT` (`atlas_tiles.h:60`),
   plus a line each in `world/block_tiles_check.c`'s `BS_BTEX_TILE_PAIRS`, whose length is
   `_Static_assert`ed against `TILE_USED_COUNT` (`atlas_tiles.h:18-21`) — a name added without
   its assert fails the build.
3. **`world/block.h`** — new id literals appended after `BLOCK_FERN = 14`, spelled as
   **literals**, never as `BLOCK_COUNT + n` (`block.h:86-87`'s ⚠, which records what happened
   the last time somebody tidied them). A new `_Static_assert` pinning them, matching
   `block.h:126-129`'s pattern. **`BLOCK_COUNT` does not move in this phase.**
4. **`world/registry.c`** — one designated-initialiser row each, with `.hardness` assigned by
   material to the existing scale (`registry.c:17-38`), never a bespoke number:

   | New block | Class | Ticks | Why |
   |---|---|---|---|
   | Spruce / jungle log | Timber | 40 | Same material as the shipped wood row |
   | Spruce / jungle planks | Timber | 40 | Same material as the shipped planks row |
   | Spruce / jungle leaves | Leaves | 4 | Same material as the shipped leaves row |
   | Short grass | Plant matter | 1 | Tall grass's number |
   | 2-tall grass, both halves | Plant matter | 1 | Tall grass's number |
   | Each flower | Plant matter | 1 | Fern and dead bush's number |
   | Apple (if Phase 5 says so) | Plant matter | 1 | It is picked, not mined |
   | Dead bush, redone | Plant matter | 1 | Unchanged — the redesign is art, not hardness |

   `registry.c:206-209`'s own rule applies: write a real number even where it will never be
   read, so it says it is a decision.
5. **`world/registry_test.c`** — move the pinned core CRC golden (`:465-466`, `0x189B` today) and
   `REGISTRY_FULL_COUNT_PIN` (`:60`, 141 today) to their new values, by hand, with a dated
   comment saying what moved them, following the existing chain of four such moves recorded at
   `registry_test.c:422-451`.
6. **The server release.** Run `deps/blocksmith-server/tools/sync-world-sources.sh` so the
   vendored `block.h` / `registry.c` match, build the server with a client tree beside it so
   `check-world-drift` (`game/Makefile:185-208`) actually runs, and ship the server **first or
   simultaneously, never client-first** (`net/networld.c:298-301`).

**Proof.** §1.A items 3, 4 and 6. Plus one thing a host test cannot reach: join a real
new-client-to-new-server session and confirm `networldRegistrySynced()` is true — the debug
overlay's net line drops its `!` (`networld.c:290-294`). This is a live-session check, not a
suite check.

### Phase 4 — `GEN_VERSION_4`, and the placement of everything Phase 3 added

**Outcome.** New worlds grow short grass, 2-tall grass, biome-specific flowers, and per-species
trees. Worlds stamped 1, 2 or 3 come back **byte-identical** to what they are today.

**The one fact this phase turns on.** Placing a new plant changes what `worldgenColumn` produces.
`world/genversion.h:78-96` records what happened the last time somebody nearly shipped a
generation change in place: biome identity was gated on `== GEN_VERSION_DENSITY`, a host probe
generated the twelve pinned columns against v1.8.2 and v1.8.3, and **seven of the twelve hashes
moved**. It also gives the rule to keep: *"Anything written as `== GEN_VERSION_DENSITY` from
here on is a bug waiting for version 4."* This is version 4.

**What to build:**

1. `world/genversion.h` — `#define GEN_VERSION_4 4u`, appended, never renumbered, with its own
   comment block in the shape `GEN_VERSION_BIOME`'s has. `GEN_VERSION_NEWEST` (`:101`) moves to
   it. `GEN_VERSION_FOR_NEW_WORLDS` (`:104`) moves **only after** the gate in step 2 exists and
   the pinned-hash test in step 4 is green.
2. `world/worldgen.c` — every new placement behind `if (g->version >= GEN_VERSION_4)`, in the
   exact shape the file already uses at `:202`, `:358`, `:662` and `:689-691`. Nothing already
   inside a `>= GEN_VERSION_BIOME` branch is edited; new code is added beside it.
3. New per-biome density constants in `worldgen.h`, following the `GEN_GRASS_*` / `GEN_FERN_*`
   idiom exactly — chance out of 256 per eligible **surface cell** for plants, per **8x8 cell**
   for trees, and the two scales never confused (`worldgen.h`'s own warning on that). Each new
   table needs its own `_MAX` bound if `worldgenScatter` rejects against one before resolving the
   biome, matching `GEN_GRASS_CHANCE_MAX`.
4. **The pinned-hash regression test.** Twelve columns — seeds 1337 / 4242 / 90210 x columns
   (0,0) (1,0) (−1,−1) (7,−3), the set `genversion.h:82-84` names — generated at versions 1, 2
   and 3 and asserted **unchanged**, and at version 4 asserted **different**. Armed red by
   temporarily removing the version gate and confirming the version-3 hashes move.
5. 2-tall grass is **two ordinary `BLOCK_SHAPE_CROSS` ids**, not a third `BLOCK_SHAPE`
   (`block.h:186-190` has three reserved bits, and they should stay reserved for slabs and
   stairs). Placement checks the cell above is clear first, reusing the `cellsClear()` shape
   `worldgenFlora` already uses for cactus height; breaking either half removes both, a rule
   that lives in `scene/interact.c` — the file `block.h:244-257` already identifies as owning
   this class of decision.

**Proof.** §1.A item 1, both halves. Plus §1.B item 1.

### Phase 5 — The apple (one owner decision, and it is the only one left)

Cactus, snow and ice are already fixed by Phase 1. What remains is the half of
`ROADMAP.md:273` that Phase 1 does **not** deliver.

**The question: what does "eaten" mean in a game with no hunger bar?** §3.15 — this client has
no health and no hunger (`net/networld.h:189`, `:219`). `networld.h:198` receives a `hunger`
float from the server that nothing consumes, and `net/inv_bridge.c:103` can send
`BS_INV_OP_CONSUME`, but there is no meter for an apple to restore.

After Phase 1 the *carrying* half is free — the item ceiling is already lifted, so an apple is an
ordinary registry row that `inventoryCanHold()` accepts, needing no further server change. So the
apple's cost has collapsed to: one atlas tile, one registry row, a drop rule on leaf breaks, and
an answer to what happens when you use it. The three honest options:

- **Drop and carry only.** It falls from leaves, it goes in a slot, using it does nothing. Honest
  and small; `ROADMAP.md:273` is then half-delivered and should say so.
- **Defer the apple entirely** to whichever version brings hunger, and say so in the roadmap
  rather than shipping a food item that cannot feed anyone (§8 item 1).
- **Invent a minimal hunger meter.** Real scope, not in this version, and it would need the
  server's `hunger` float (`networld.h:198`) wired to something.

**This spec does not pick.** It is a scope decision and the owner's to make. **Nothing else in
v1.8.8 is blocked on the answer** — that is the point of putting Phase 1 first.

**Proof.** If the apple ships: a live multiplayer round trip, break a leaf block and confirm the
apple arrives in a slot and survives a save/load (`inventoryLoad()`'s filter at
`world/inventory.c:364` is the thing that would silently eat it if Phase 1 were incomplete).

### Phase 6 — Debug menu: biome readout, neon borders, block list

All three are debug-only and none is reachable in normal play.

1. **Current biome, bottom screen.** A `DEBUG_INFO` row (`debugmenu.h:23`, `:44-45`) registered
   in `bsDebugRegister()` (`main.c:1139`), formatting `worldgenBiomeAt(g, px, pz)`
   (`worldgen.h:526`) through a name table. `worldgenBiomeParams()` (`worldgen.h:530`) is
   never-NULL, so the lookup is safe by construction. **Cost, honestly:** `worldgenBiome`/
   `worldgenHumidity` are **not cached** — two fresh `noiseFbm2` evaluations every frame the menu
   is open. Cheap for one call a frame, not free, and the same gap
   `docs/research/sky-and-weather.md` already flags for a different caller.
2. **Neon biome-border toggle.** A `DEBUG_TOGGLE` row plus a second cheap pass reusing
   `scene/highlight.c`'s box builder (`highlight.c:7-18`): one tall thin box per boundary
   *column*, y = 0 to world height, wherever `worldgenBiomeAt(x,z)` differs from its +x or +z
   neighbour. Colour comes from a TEV constant, so a neon value costs one uniform write and no
   shader change. Biome is a pure 2D function of world (x, z), so the boundary list is built once
   when the loaded ring changes and never touched again. **Bound the list by the Old 3DS ring**
   (radius 3 + 1, `budget.h:52`), not the New 3DS one, or this becomes a New-3DS-only feature by
   accident (§5.6).
3. **Block list, bottom screen.** Not a `DebugEntry` — the registry has no icon or grid concept
   (`debugmenu.h:27-49`). Its own screen, opened by a `DEBUG_ACTION` row the same way
   `debugMenuUiOpen()` is called at `main.c:4309`, drawn on the bottom screen under the existing
   `#if BS_BOTTOM_UI` convention. Icons are a direct reuse of `scene/ui.c:184`'s
   `atlasTile(blockFaceTex(id, FACE_TOP))`, which already handles CROSS plants correctly
   (`registry.c:260-261`: all six tex entries carry the same tile). Enumeration is
   `registryIsDefined()` over 0..0xFD (§3.10) — deliberately **not** bounded by `BLOCK_COUNT`,
   because the whole point of this screen is to show blocks the player cannot carry.

**Proof.** §1.B item 5, plus a host test for the biome-name table (a pure function: every
`BiomeId` 0..`BIOME_COUNT-1` returns a non-NULL, distinct name).

### Phase 7 — Visual sign-off (playtest, not a host check)

**Outcome.** Every claim in §1.B is actually looked at.

**What to do.** Boot the game and look, at: a plains/jungle border; a taiga/tundra border; a
grass block up close from the side, at three different biomes (requirement 8); a patch of tall
grass on tinted ground; the desert with the redone dead bush; the bottom-screen block list; and
the neon border toggle on and off. Screenshot each. Real console preferred; Azahar is acceptable
**for shape and colour**, which is what every claim in §1.B is about.

**Proof.** The screenshots, described against §1.B's five criteria. §7 says why no host test can
substitute.

---

## 5. Cost

### 5.1 Memory

**Against `world/budget.h`'s 12 MB world-store cap (`budget.h:75`) and its 65,648 B per loaded
column (`budget.h:38-41`): this version adds zero.** That budget counts exactly three things —
`sizeof(Column)` 48 B, eight chunks at `chunkFormBytes(RAW)` 32,832 B, and one 32,768 B
`LightColumn` (`budget.h:38-41`, measured off the ARM compiler, not derived on paper). Nothing
in §4 adds a field to `Column`, a chunk form, or a light channel, so `budgetColumnBytes()`
(`budget.h:103-111`) returns the same number and radius 5 still fits at 88.7% and radius 6 is
still refused (`budget.h:52-54`). `tests/world_budget_bytes_test.c` asserts both halves and stays
green untouched.

What this version does add, all of it outside that cap:

| Item | Bytes | Provenance | Where |
|---|---|---|---|
| `MeshScratch.tint[18*18]` | **324** | measured (18 = `SCRATCH_DIM`, `scratch.h:21`) | Static BSS, one instance (`chunk_render.c:674`) |
| New core registry rows | **0** | measured | `s_defs`/`s_used`/`s_view` are already `[REGISTRY_MAX]` = 256 (`registry.c:282-283`); `s_rect` is already `[REGISTRY_MAX][BLOCK_FACES]` (`mesher.c:197`) |
| New atlas tiles | **0** | reasoned | The sheet is a fixed 16 x 1024 RGBA5551 = 32,768 B (`make_atlas.py:112-114`; 2 B/px) whether a slot is painted or not |
| `tintLUT[16]` shader uniform | 16 x 16 = **256** of GPU uniform space | measured (a PICA `fvec` is 4 floats) | §5.3 |

**Phase 1 and Phase 6 cost nothing here either.** Widening `inventoryCanHold()` changes a
comparison, not a slot count: `INV_SLOT_COUNT` stays 24 (`inventory.h:103-107`), the save record
stays 68 bytes (`inventory.c:210-211`), and `BS_INV_STATE_BYTES` stays 50 (`bs_proto.h:574`).
The UI sprite buffer is unaffected and already has room — but **§3.14 records a stale comment in
`source/gfx/sprite.c:22-28` that sizes it against a "9x5 grid, 45 slots" inventory which is
really 24.** The buffer is oversized rather than short, so it is cosmetic; it is flagged and
deliberately not fixed, because it is the assumption someone widening the inventory grid later
would trust.

**324 bytes is the whole memory cost of this version** [reasoned, from measured constants]. Had
the tint been stored per *cell* rather than per *column* it would have been
`SCRATCH_BLOCKS` = 5,832 B (`scratch.h:22`) — 18x more, for information that does not vary with
y, since biome is a 2D field (`worldgen.h:288-296`). The per-column band is the right shape.

### 5.2 Atlas space — the tightest budget in this version

`tools/make_atlas.py:55-58` states it outright: 17 slots named, slot 63 reserved, **46 free for
the rest of the project's life**, and the sheet cannot grow because 1024 px is the PICA200's
maximum dimension (`gfx/atlas.h:3-7`).

| New tile | Count | Note |
|---|---|---|
| Spruce log side, log top, planks | 3 | |
| Jungle log side, log top, planks | 3 | |
| Spruce leaves, jungle leaves | 2 | Oak leaves already exist and are tinted, not duplicated |
| Short grass | 1 | |
| 2-tall grass upper, lower | 2 | |
| Flowers | 4 | One per flower species, shared across biomes; density varies, id does not |
| Apple | 1 | Only if Phase 5 says so |
| Dead bush, redone | 0 | Rewrite of `tile_dead_bush`, same slot |
| **Total** | **16** | leaving **30 free** [reasoned: 46 − 16] |

**The number that matters is the one this budget avoids.** Minting per-*biome* rather than
per-*species* wood — six biomes x (log side, log top, planks, leaves) — is **24 tiles** for the
wood alone, which with the plants above would spend 34 of 46 and leave 12 slots for the rest of
the game's life. `ROADMAP.md:258-259` makes exactly this argument against per-biome blocks; §2
applies it to the blocks the roadmap *does* ask for. **Species, not biomes.**

### 5.3 Vertex-shader uniform registers — a number this spec could not reconcile

The PICA200 vertex stage has **96** float uniform registers. `world/mesher.h:128-129` states, in
this project's own words, that the shader *"already spends 77 of them"*.

Counted off `source/shaders/world_dynamic.v.pica` directly:

| Declaration | Line | Registers |
|---|---|---|
| `.fvec projection[4], modelView[4]` | `:24` | 8 |
| `.fvec faceShade[64]` | `:32` | 64 |
| `.fvec dayLevel[1]` | `:36` | 1 |
| `.fvec fogParams[1]` | `:54` | 1 |
| `.constf uvScale` | `:63` | 1 |
| `.constf consts` | `:65` | 1 |
| `.constf aoMix` | `:70` | 1 |
| `.constf lightConsts` | `:76` | 1 |
| **Total** | | **78** |

The same count over `world.v.pica` (no `dayLevel`, no `lightConsts`) gives **76**.
`mesher.h:129`'s 77 matches neither. **This spec does not pick one.** It is one register either
way and the conclusion does not move: free registers are **18** by the count above, **19** by
`mesher.h`'s, and a `tintLUT[16]` fits with 2 or 3 to spare either way. What would *not* fit is
anything bigger — an 8x8 grid is 64 entries, over 3x the free budget — which is why §2 caps the
palette at 16 and why a colormap **texture** (the alternative design, brief C1 Design 2) was
declined: it would consume one of the four free TexEnv stages that `sky-and-weather.md` has
already earmarked for v1.8.9's rain.

**Whoever implements Phase 2 should settle this by assembling the shader and reading picasso's
own register allocation**, and correct `mesher.h:129` in the same change. It is a five-minute
check and it removes a number this tree currently states two ways.

### 5.4 Per-frame and per-remesh cost

- **Per frame: three extra vertex-shader instructions** — one mask/shift, one `mova`+lookup, one
  `mul` — on a stage that already does the identical `mova`/lookup once (`world_dynamic.v.pica:
  103-104`). [reasoned, not measured] The **instruction-slot** ceiling of the PICA200 vertex
  stage was not measured for this spec and no citable figure was found for it; the research
  brief's Part F item 3 flags the same gap. Three instructions is very unlikely to be the
  constraint that bites, but it is not proven here, and Phase 2 should assemble the shader and
  confirm before the change is called done.
- **Per remesh: `scratchFillTint`.** Sampling `(worldgenBiome, worldgenHumidity)` at every one of
  the 324 scratch columns is 648 uncached `noiseFbm2` evaluations per chunk remesh — these
  functions cache nothing (`docs/research/sky-and-weather.md` D5a, re-confirmed). **Sample on a
  5x5 lattice at `GEN_D_CELL_XZ` (4-block) spacing instead and bilinearly interpolate the two
  scalars before quantising** — 50 evaluations per chunk, matching the spacing
  `worldgen_density.h:64-67` already uses for the density lattice. Against `wgdColumn`'s 22,031
  noise evaluations per column (`docs/ROADMAP.md:145-148`), 50 per chunk is noise. [reasoned]
  Interpolate the **scalars**, then quantise; interpolating the **index** would blend through
  unrelated palette entries.
- **Per remesh: the merge split.** Adding the tint index to `faceFlatKey` (§4 Phase 2 step 4)
  makes runs stop at a tint change, so a chunk straddling a biome border emits slightly more
  quads. Bounded by `ATLAS_MAX_MERGE_BLOCKS` = 15 (`atlas_uv.h:137`) and confined to boundary
  chunks. **Not measured.** If it shows up, the mitigation is to widen the quantisation cells,
  not to remove the term (§9).

### 5.5 What 16 entries actually buys, said plainly

A 4x4 quantisation of (temperature, humidity) gives **16 flat hues with hard steps between
them**. It is not a gradient, and the research brief's C2 claim that continuity comes "by
construction" from the continuous input fields is only half right: the *inputs* are continuous,
the *output* is quantised to 16 buckets, and the bucket boundary is a visible line.

Vanilla bands too unless Biome Blend is on, and Blocksmith cannot afford Biome Blend's per-block
averaging. The cheap mitigation, if §1.B item 4 comes back badly: dither the bucket choice by a
per-column hash so the boundary stipples rather than draws a straight edge — free, one extra
hash, no new bits. **Do not implement it speculatively.** Look first (§7).

The reason 16 is the cap and not a tuning choice: 4 bits of `MeshVertex.ao` (§3.5), and a
16-entry `fvec` LUT against 18 free uniform registers (§5.3). Going to 32 entries needs 5 bits
(available — 6 are spare) but 32 registers, which is not available.

### 5.6 Old 3DS and New 3DS

Everything in §4 costs both consoles the same: three vertex-shader instructions, 324 bytes of
BSS, registry rows in arrays that are already 256 long, and atlas slots in a sheet that is
already fully allocated. **No part of this version needs New 3DS RAM, clock or L2, and no part
of it should be allowed to degrade silently on an Old 3DS.**

The one place that could go wrong by accident is Phase 6's neon border overlay, whose geometry
scales with the loaded ring: radius 3 + 1 on Old (`budget.h:52`, 81 columns) against radius 5 + 1
on New (`budget.h:53`, 169 columns). Build and cap the boundary list against the Old 3DS ring, so
the New 3DS simply covers more ground with the same code rather than the Old 3DS quietly running
a bigger pass than it can afford. If a measurement later shows the overlay is too expensive on
Old 3DS at radius 3, **say so and make it New-3DS-only explicitly** — do not let it degrade
without a name.

---

## 6. World and multiplayer compatibility

The owner's stated top correctness concern. Every row below was read off the code, not inferred
from the roadmap.

### 6.1 What changes, and what does not

| Thing | Version today | After v1.8.8 | Why |
|---|---|---|---|
| `BlockId` width | `uint8_t` (`block.h:15`) | **unchanged** | 112 free core ids (§0); nothing needs 9 bits |
| `REGION_VERSION` | 1 (`region.h:190`) | **unchanged** | No field moves; a new block id is an ordinary byte in an existing format |
| `BS_PROTO_VERSION` | 1 (`bs_proto.h:56`) | **unchanged** | No wire message changes shape — item ids are already `uint8_t` with no truncation (§0 item 2). See §6.5 for why bumping it would be worse, not safer |
| `INVENTORY_VERSION` | 1 (`inventory.c:208`) | **unchanged, deliberately** | The record layout does not change; only which ids pass the filter. §6.6 — bumping it would make an old client discard the **whole** inventory instead of one slot |
| `registryCrc16()` | `0x189B` (`registry_test.c:465`) | **moves, once** | Any core row changes it. This is the forced server release |
| `GEN_VERSION_NEWEST` | 3 (`genversion.h:101`) | **4** | New flora changes generated columns |
| The item ceiling | `item < BLOCK_COUNT` = 8 (`inventory.h:70-73`) | **registry-aware predicate** (§3.13) | Three shipped blocks are unbreakable and so is every dynamic cube (§3.12, §6.7). `BLOCK_COUNT` itself stays 8 as the item-enum terminator |

### 6.2 A world saved by an old client, opened by a new one

**Safe.** `REGION_VERSION` is unchanged, so `region.c:124`'s check passes and every saved column
decodes. Terrain is regenerated from the seed and the stamp: a world stamped `GEN_VERSION_BIOME`
(3) stays on version 3 forever (`genversion.h:24`: *"A world directory with a valid stamp loads
as that generator"*), so it gets **no** short grass, no 2-tall grass, no flowers and no new tree
species — exactly as intended, and exactly what §1.A item 1 proves. It **does** get the tint,
because the tint is render-side and reads a scalar that already existed.

That last point is worth stating for the owner: **an existing world will look different after
this update, because its grass will be tinted.** No block moves and no terrain changes, but the
colour does. That is the feature, and it is unavoidable if tint is to apply to worlds people
already have. If it should *not* apply to old worlds, that is a decision, not an oversight — say
so and it becomes a version gate on the render path instead.

### 6.3 A world saved by a new client, opened by an old one

**Safe against corruption, lossy in appearance, and it does not get worse over time.**

- The stamp is 4. `genVersionResolve` returns `GENVER_TOO_NEW` (`genversion.h:125`) and the old
  build **refuses to enter the world** rather than generating version-3 terrain under version-4
  buildings. This is the designed behaviour and it is the right one.
- If a region file from such a world reached an old build by another route, `REGION_VERSION` 1
  still parses, and a cell holding a new id (say 20) resolves through `registryView()`'s
  never-NULL contract to the air row — **a hole, not a crash, not a wrong block**
  (`registry.h:120-121`, `block.h:208-209`). [reasoned; not measured] The raw byte survives in
  memory and would be re-encoded verbatim if that column were saved again, because the codec
  writes the ids it holds (`chunk_codec.c:90-92`) — so an old build viewing a new world does not
  *destroy* the new blocks, it just cannot see them. **This specific claim is worth a host test
  in Phase 3** (encode a column containing an undefined id, decode it, assert the byte is
  unchanged), because it is the difference between "old build shows holes" and "old build eats
  the world", and this spec is reasoning about it rather than having run it.

### 6.4 An old client connecting to a new server — the one that fails silently

**This is the dangerous case, and the code says so in its own words.**

`net/networld.c:279-301`, in full force: a core registry addition means the client's compiled-in
`kCoreDefs` disagrees with the server's `BS_APP_REGISTRY_INFO` fingerprint. The client sends up
to `NETWORLD_REG_FETCH_MAX_SENDS` (4) FETCHes 250 ms apart; a `BS_APP_REGISTRY_DEFS` batch can
only carry **dynamic** rows (`registry.c:363`, `:367` refuse any id below `REG_ID_DYN_LO`); the
2000 ms deadline expires; and the client enters the world with `s_reg_synced` false **for the
rest of the session**. The comment continues:

- *"That degraded state is effectively invisible to a player."* The "Joined - syncing block
  table..." row disappears on the deadline exactly as it would on success. The only lasting
  indicator is a `!` on a debug overlay that is off unless the player turned it on.
- *"every server-defined id resolves to air through registryView()'s never-NULL contract, and a
  world full of holes reads as terrain."*
- *"a core registry addition FORCES a matching server release, shipped FIRST or simultaneously
  and never client-first. Nothing at runtime detects it for the player, nothing at runtime
  repairs it, and the server will happily accept the join either way. The lockstep is a
  release-process obligation that people have to keep; the protocol does not enforce it and
  cannot."*

**So the answer to "what happens to an old client on a new server" is: it joins successfully, and
every new block in the world is an invisible hole, with no message anywhere the player will see.**
The only mitigation available is procedural: ship the server first. §4 Phase 3 step 6 is that
obligation written into the plan.

The reverse — a **new** client on an **old** server — degrades the same way and is harmless in
practice, because an old server's world contains no new ids to render. `networldRegistrySynced()`
is false and nothing looks wrong.

### 6.5 Should `BS_PROTO_VERSION` be bumped to make 6.4 loud instead of silent?

**Recommendation: no, and it should be a conscious no rather than an omission.**

Bumping it (`bs_proto.h:56`) is the only mechanism in this protocol that refuses an old peer. But
the refusal is a **silent datagram drop** with no error packet, ever
(`deps/blocksmith-server/gateway/bsgate.c:415-419`, *"an unauthenticated sender learns nothing,
not even that something is listening"*). So bumping it converts "the player sees holes" into "the
player's game cannot connect and says nothing about why" — for every fielded CIA, all at once,
including ones whose owners are mid-session. And it would be a lie about the wire: no message
changes shape in this version (§6.1).

`deps/blocksmith-server/game/bsgame.c:751-755` says the same thing from the other side:
`BS_PROTO_VERSION` is explicitly *not* the mechanism that guards registry/content drift, and the
two must not be conflated.

**If the owner wants 6.4 to be loud, the right change is a visible client-side message when
`networldRegistrySynced()` is false at world entry — a UI change, not a protocol change.** That
is not in this version's scope and is flagged here rather than smuggled in.

### 6.6 The item ceiling lift: exactly what survives, in all four directions

This is Phase 1, and it is the change the owner's multiplayer concern most directly touches.
The reassuring half first: **the item id is already `uint8_t` end to end and nothing truncates
it**, so no format widens. Verified, not assumed:

| Carrier | Where | Width |
|---|---|---|
| Inventory action (`PICKUP`, `CONSUME`) | `bs_proto.h:560`, `BS_INV_ACTION_BYTES` = hdr + 1 op + 3 operands | `a` = item id, **1 byte** (`bs_proto.h:541`, `:545`) |
| Inventory state broadcast | `bs_proto.h:574-575`, `BS_INV_STATE_BYTES` = hdr + 1 + 24 x 2 = 50 | **1 byte** id + 1 byte count per slot |
| Inventory save file | `world/inventory.c:210-211`, `INV_FILE_BYTES` = 20 + 24 x 2 = 68 | `buf[20 + i*2 + 0]`, **1 byte** (`inventory.c:357`) |
| Block edit | `bs_proto.h:247`, `BS_BLOCK_EDIT_BYTES` | **1 byte** (`networld.c:507`, `:1118`) |

So the ceiling is a **validation constant in five places**, not a field width anywhere.

**New client, new server.** Everything works. This is the case Phase 1 delivers.

**New client, OLD server** — *the dangerous one, and it is silent.* The client would allow the
break of a cactus and send `BS_APP_BLOCK_EDIT`; the old server's `bsEditValid()` accepts it
(placement was never bounded by `BS_BLOCK_COUNT` — `validate.c:188`), **so the block is removed
for everyone**. Then the client sends `BS_INV_OP_PICKUP` with `a = 12`, and the old server's
`bsgame.c:1339` rejects it, because `12 < 8` is false. **The block is destroyed and nobody gets
the item, for every player on that server.** `interact.c:170-174` describes this exact sequence
as the reason the guard sits where it does, and `validate.h:66-67` frames the class of failure
honestly: *"a mismatch loses the player's item, it does not corrupt their console."* Survivable,
invisible, and it eats blocks. **The mitigation is the same release-process obligation as §6.4:
server first or simultaneously, never client-first.** Nothing on the wire detects it.

**OLD client, new server.** Harmless and self-limiting. The old client still refuses to break
cactus locally, so it never sends the pickup. A server-initiated inventory state carrying an id
≥ 8 is filtered out by the old client's own `inventoryCanHold()` — the slot reads empty. Nothing
is destroyed.

**Old client reading a NEW client's inventory save.** `inventoryLoad()` validates every slot with
`inventoryCanHold()` and `continue`s past a failure onto a slot that is already zeroed
(`world/inventory.c:364`). So an old build opening a save containing item id 12 **silently drops
that one slot** and keeps the rest. The CRC still verifies, because it covers the bytes as
written (`inventory.c:349`).

**Which is exactly why `INVENTORY_VERSION` must NOT be bumped.** A version mismatch makes
`inventoryLoad()` return early with defaults (`inventory.c:343`) — the player loses the
**entire** inventory rather than the one new item in it. The record layout is unchanged
(§6.6's table), so a bump would communicate nothing true and would strictly worsen the failure.
**Leave it at 1.** [reasoned from `inventory.c:340-364`; worth one host test in Phase 1 —
write a save with a high id, load it with the narrow predicate, assert exactly one slot is lost
and the others survive.]

**The server hand-edit is the part a script will not do.** §4 Phase 1 step 6 lists the six
places. The one that bites is that `bsgame.c:1339`/`:1345` and `playerstate.c:14` compare
against `BS_BLOCK_COUNT` **directly** and never call `inventoryCanHold()`, so re-running
`sync-world-sources.sh` copies a widened predicate into a tree that does not use it and the
build stays green. And if the deployed server has no client tree beside it, `check-world-drift`
is a no-op (`game/Makefile:206-207`) and the drift assert at `validate.c:146-147` never runs.

### 6.7 Server-defined blocks are unbreakable today, and that is a multiplayer bug

Not a v1.8.8 feature — a thing already true in the shipped client, found while establishing the
above, and worth stating because it is invisible from the roadmap.

`scene/interact.c:178`'s guard tests `inventoryCanHold()`, bounded at 8. **Every** block a server
defines in the dynamic space `0x80..0xFD` (`registry.c:383`, applied by `registryRemoteApply()`
at `registry.c:492-499`) therefore fails that test, and any such block that is not
`BLOCK_SHAPE_CROSS` **cannot be broken by any client**. A server can define a block, generate or
place it in the world, and no player can ever mine it — with no error, only the debug overlay's
`r` refusal counter (`interact.c:182-186`).

Phase 1's registry-aware predicate (§3.13) fixes this for free and by construction: a
server-defined row is `registryIsDefined()` and is not `REG_FLAG_LIQUID`, so it becomes
carryable and breakable exactly like a core row. §1.A item 3's walk over the whole
`registryIsDefined()` span is what proves it rather than only proving the three core cases.

---

## 7. How each phase is proven, and which claims cannot be settled without a playtest

| Phase | Proof mechanism | Needs hardware? | Needs a look, not just a number? |
|---|---|---|---|
| 1 (item ceiling) | §1.A item 3 — snow, ice and cactus break and drop themselves; **all three fail today**, so the check cannot pass by luck. Plus the whole-registry walk that covers dynamic ids (§6.7), and the save round-trip in §6.6 | No, for the suite. **Yes, for a live server session**: break a cactus in multiplayer and confirm the block goes **and** the item arrives — that crosses `BS_APP_BLOCK_EDIT`, `BS_INV_OP_PICKUP` and `BS_INV_STATE`, and no host test reaches it | No — "the cactus broke and I have a cactus" is observable, not aesthetic |
| 2 (tint) | Twelve-column hash test with and without the tint code (identical = render-side only); the merge-split mesher test, armed red by removing the tint term from `faceFlatKey` | No, for the tests. **Yes, for the shader**: assemble it with picasso and read the register allocation (§5.3) before calling it done | **Yes.** §1.B items 1-4. A green hash test proves the terrain did not move; it says nothing whatever about whether the colour reads right, and the desaturation of `tile_grass_top` guarantees the grass block *looks different* |
| 3 (rows, art, durability) | `registry_test` with its moved CRC golden and count pin; `block_tiles_check`'s assert count; the breakability walk over every defined id, which **goes red today on cactus** | No, for the suite. **Yes, for a live session**: join new-client-to-new-server and confirm `networldRegistrySynced()` is true (the `!` leaves the debug net line) | Partly — the new art has to be looked at, it is generated by a script nobody has run yet |
| 4 (`GEN_VERSION_4`) | Pinned-hash test: versions 1/2/3 byte-identical, version 4 different. Armed red by removing the version gate | No | **Yes.** Whether the new flora density reads as a meadow or as a lawn is `worldgen.h`'s own standard for these constants and it was set by eye |
| 5 (apple) | The Phase 1 breakability walk covers the carrying; the drop rule needs a leaf-break test | A live multiplayer session is the only thing that proves the item survives the round trip and the save | Only if it ships — and "eaten" cannot be proven at all, because there is nothing to feed (§3.15) |
| 6 (debug menu) | Host test for the biome-name table (total, distinct, never NULL) | No, for the table | **Yes.** Legibility on a 320x240 bottom screen, and whether the neon border lands where the readout says, are looks |
| 7 (sign-off) | The screenshots themselves against §1.B | Real console preferred; Azahar acceptable — every §1.B claim is about shape and colour, not timing | **Yes. This is the phase nothing else substitutes for** |

**What cannot be settled at all in this environment, and what that gap means:**

- **The PICA200 vertex-shader instruction-slot ceiling.** No citable figure was found (§5.4, and
  the research brief's Part F item 3 has the same gap). Three extra instructions is very
  unlikely to overflow it, but "unlikely" is not "measured", and the check is cheap: assemble and
  read the output. If it does overflow, Phase 2's design has to shrink and the owner needs to
  know before Phase 3 commits to a server release.
- **Whether the merge split (§5.4) costs anything visible in frame time.** Not measured. Bounded
  in theory by a 15-block cap on boundary chunks only; unmeasured in practice on either console.
- **The 77-vs-78 uniform register count (§5.3).** Unreconciled. Does not change any conclusion.
- **Whether an old build re-saves a column containing an unknown id without destroying it**
  (§6.3). Reasoned from `chunk_codec.c:90-92` and `registryView()`'s contract; not run. Phase 3
  should run it.
- **Whether an old client loading a new client's inventory save loses exactly one slot rather
  than all of them** (§6.6). Reasoned from `world/inventory.c:340-364` — the loop `continue`s
  past a failed `inventoryCanHold()` onto an already-zeroed slot, and the CRC covers the bytes
  as written. Not run. Phase 1 should run it, because it is the evidence for leaving
  `INVENTORY_VERSION` at 1, and if it is wrong the right answer flips to bumping it.
- **What an old server actually does with a widened client's `BS_INV_OP_PICKUP`** (§6.6). The
  rejection at `deps/blocksmith-server/game/bsgame.c:1339` was read; the resulting block loss is
  reasoned from `interact.c:170-174`'s description of the same sequence, not observed. It needs
  two builds and a live session, which no host test replaces — and it is the case in this
  version most capable of losing a player's work.

---

## 8. What to cut, in priority order, if this version turns out too expensive

1. **Cut the apple entirely** (`ROADMAP.md:273`). "Eaten" cannot be delivered at all without a
   hunger system that does not exist (§3.15), so shipping it half-done is shipping a food item
   that cannot feed anyone. **Cutting it costs nothing else in the version** — Phase 1 lifts the
   item ceiling for the unbreakable-blocks bug regardless, so the apple is not paying for that
   work and dropping it does not recover it.
2. **Cut per-species wood down to two species instead of three.** Spruce for taiga, oak for
   plains and forest and jungle. Saves 3 atlas tiles and a chunk of `make_atlas.py` work, and the
   biomes still read as different places because the tint is doing that job.
3. **Cut the tint on the grass block's side face question by keeping the side exactly as it is,
   and if the untinted side stops reading as one block with the tinted top, narrow the LUT's hue
   range rather than adding a second texture.** Requirement 8 is that the block the owner praised
   survives. Narrowing the palette is free; the overlay technique (brief A2) is not, and adding
   per-climate side tiles would cost 4 more atlas slots. Narrow first, look, and only spend slots
   if narrowing is not enough.
4. **Cut the block list before the other two debug items.** It needs its own screen, its own
   layout and its own scroll handling (§4 Phase 6 item 3), where the biome readout is one
   `DEBUG_INFO` row and the border toggle is one `DEBUG_TOGGLE` plus a reuse of an existing
   renderer. It is the largest of the three by a wide margin.
5. **Cut 2-tall grass before short grass.** Short grass is one new id and one density constant.
   2-tall grass is two ids, a paired placement check, a break-both rule in `scene/interact.c`,
   and roughly double the CROSS geometry wherever it lands (unmeasured — the research brief's D4
   flags the same gap).
6. **Do not cut Phase 4's version gate.** §6.2 and `genversion.h:78-96` state the cost of editing
   generation in place, and the file records it nearly happening once already, caught only
   because somebody ran a hash probe. This is not a corner this version can cut and still keep
   its promise that existing worlds keep their terrain.
7. **Do not cut Phase 7.** An unverified visual claim is not a smaller version, it is an
   unfinished one — and this version is almost entirely a visual claim.
8. **Do not cut Phase 1.** It is not new content, it is three shipped blocks the owner cannot
   break plus every dynamic block a server defines (§3.12, §6.7). Cutting it also means every
   *new* cube this version adds arrives unbreakable, which directly contradicts the owner's
   "every single new block you add is breakable with its own durability" — so cutting Phase 1
   would silently cut Phase 3's cubes with it.

---

## 9. The single biggest risk, and what to do about it

**`world/mesher.h:8` says the mesher is "deliberately not greedy". It is wrong, and believing it
would ship a tint that is smeared up to 15 blocks across every biome border in the game.**

The code merges coplanar faces along U. `faceFlatKey()` (`world/mesher.c:533-549`) decides
whether two faces may merge, and its key is built from **AO and light only** —
`return 0x10000u | ((uint32_t)ao << 8) | pad;` (`mesher.c:548`). `mergeRun()` (`mesher.c:553-560`)
walks the run, capped at `ATLAS_MAX_MERGE_BLOCKS` = 15 (`world/atlas_uv.h:137`).

So if the tint index is packed into the spare bits of `ao` (§4 Phase 2 step 4) **and
`faceFlatKey` is left alone**, two grass tops with different tint hues but the same AO and the
same light will merge into one quad, and the whole run takes the first cell's tint. At a biome
border — which is precisely where the tint changes and precisely what this version exists to make
visible — the boundary would be displaced by up to 15 blocks along the merge axis, in a pattern
that looks like a slightly ragged edge rather than like a bug. No test in the tree catches it. The
hash tests would be green (generation is untouched). The shader test would be green (both `.pica`
files agree). It would compile, run, and look *almost* right.

This is the same shape of failure `world/atlas_uv.h` already warns about and this project's own
history records more than once: a wrong constant still renders *a* texture, so the bug presents as
bad art rather than as an error.

**What to do about it, in order:**

1. **Add the tint index to `faceFlatKey`'s key.** Bits 17..31 of the returned `uint32_t` are free
   (the flag is 0x10000, `ao` is bits 8..15, `pad` is bits 0..7), so `| ((uint32_t)tint << 17)`
   costs nothing and needs no widening. Two faces then merge only when they agree on AO, light
   **and** tint.
2. **Write the test before the fix, and arm it red.** §1.A item 5: build a chunk whose grass-top
   row spans two tint indices, assert the run splits at the change, then remove the tint term and
   confirm the test fails. A check that could not have failed proves nothing.
3. **Fix the comment.** `world/mesher.h:8-11` should say what the mesher actually does. It is the
   comment that would make a careful implementer skip step 1 entirely, and it has already misled
   one document in this tree: `docs/research/biome-identity.md:32-36` takes it as that brief's
   headline finding. Correcting the comment is the durable half of this fix.
4. **Watch the cost.** Splitting runs at tint boundaries emits more quads in boundary chunks
   (§5.4). Bounded, unmeasured. If it shows up on an Old 3DS, widen the quantisation cells so
   boundaries are rarer — **do not remove the term**, because that trades a measurable cost for
   an invisible wrong.

**Runner-up, and it is the one that destroys data rather than mis-drawing it:** §6.6's
new-client-on-old-server case. Once Phase 1 widens the item ceiling, a new client meeting an old
server will break a cactus — the old server accepts the `BS_APP_BLOCK_EDIT` (placement was never
bounded by `BS_BLOCK_COUNT`, `validate.c:188`) and **removes the block for every player**, then
rejects the `BS_INV_OP_PICKUP` at `bsgame.c:1339` because `12 < 8` is false. The block is gone
and nobody gets it. `scene/interact.c:170-174` describes this exact sequence as the reason its
guard sits at the *start* of a break, so keeping that guard — with a widened predicate rather
than a deleted branch — is what confines the damage to a version-mismatched session.

**Third, silent in yet another way:** §6.4. A core registry addition means an old client on a new
server sees every new block as an invisible hole, joins successfully, and is told nothing. The
protocol cannot detect or repair it (`net/networld.c:298-301`).

All three share one mitigation and it is not a technical one: **the server ships first or
simultaneously, never client-first.** It is written into §4 Phase 1 step 6 and Phase 3 step 6,
and it is the one obligation in this plan that a green test suite will never enforce.
