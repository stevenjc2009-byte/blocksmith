# v1.8.18 — Storage and quality of life: implementation spec

> **⚠ RENAMED [2026-09-03, v1.8.17]** — this file was `docs/plan-1.8.18-storage-qol.md`. It
> is now `docs/plan-1.9.0-storage-qol.md`. Storage/QoL moved from v1.8.18 to v1.9.0 in the
> roadmap renumber; the filename is corrected to match. The content below, including its own
> "v1.8.18" title and prose, is unchanged from before the rename.

This is the detailed version of `docs/ROADMAP.md:379-383`'s v1.8.18 entry (*"Added. Chests.
Stack splitting and merging, shift-move, and the small conveniences that a game gets tiring
without."*) and `docs/VERSION-LIST.md:545-551`'s matching entry. Neither document names a
research brief for this version — there is none, so every fact below was read directly out of
the tree rather than restated from an existing brief. Every figure is labelled **read in this
codebase**, **taken from `docs/research/ui-skin.md`**, or **my proposal**.

This document does not implement anything. It is not itself a change to any file under
`source/`, `tests/`, or `tools/` — none of those were edited to produce it. Two things this
spec had to discover rather than assume, because they change the shape of the whole storage
half:

1. **Blocks in this engine carry zero per-instance state, anywhere in the pipeline** — not in
   the chunk array, not on disk, not on the wire (§2.2). A chest is the first block that needs
   any, and nothing existing can be stretched to hold it; it is a new subsystem, not an
   extension of one.
2. **The player's own item ceiling was already split in two by an in-tree change** ahead of
   this version — `inventoryCanHold()` (what the *bag* may hold) is registry-wide today, but
   `inventoryItemOnWire()` (what a *multiplayer packet* may carry) is still pinned at 8
   (`source/world/inventory.h:101-137`, read directly, §2.6). A chest's contents inherit this
   exact same crack the moment it needs to sync over the network, and this spec says so rather
   than discovering it during multiplayer testing.

---

## 0. What this version actually is, stated once

Two independent halves that happen to ship together because the roadmap groups them, not
because one depends on the other:

- **Storage.** A chest block with per-instance contents, its own save file, its own (bounded)
  memory budget, a screen to work its contents against the player's own inventory, a recipe to
  build one, and — if it is ever placed on a multiplayer server — a network story that is
  honestly a fork the owner has to pick (§8, HIS CALL).
- **Quality of life.** The roadmap gives no list. §6 is a real audit of this codebase (not a
  generic Minecraft feature list) for what is actually missing, ranked by value over cost, with
  the three highest-ranked items designed in full and the rest named for later.

Both halves are ordinary core-registry / ordinary-C-struct work. Neither needs `BlockId` to
widen, neither needs `BS_PROTO_VERSION` to move, and only the storage half needs a new opcode —
and only if multiplayer chest sync is the option the owner picks in §8.

---

## 1. Success criteria

Split the way `docs/plan-1.8.8-biome-identity.md:118-176` splits it, because conflating "the
suite is green" with "it looks and feels right" is the mistake that document was written to
avoid, and the same risk exists here: a chest that round-trips correctly in a host test can
still feel wrong to actually use on a touchscreen.

### A. Measurable, host-only

1. A chest placed, filled, and reloaded from a fresh process (`chestSave` → `chestLoad`, or
   whatever the real function names become) returns byte-identical contents to what was saved.
2. A save directory with no `chests.dat` at all (an existing pre-v1.8.18 world) loads as **zero
   chests**, not an error, mirroring `inventoryLoad`'s own "missing file → empty, not a crash"
   contract (`source/world/inventory.h:307-313`).
3. The sparse chest table refuses cleanly (a counted refusal, not a crash or a silent drop) once
   its bound (§3.2) is exceeded, and every already-stored chest is untouched by the refusal —
   the same guarantee `net/blockdiff.c`'s `refusals` counter gives today.
4. A registry-defined chest block has non-zero `.hardness` and passes
   `source/world/registry_test.c`'s existing hardness-nonzero sweep (§2.1) without a new
   exemption being added for it.
5. The three QoL items built in §6 each get a concrete, armed-red-by-construction check: hotbar
   cycling changes `selected_hotbar` on an L/R press outside any menu; the aim-target readout
   returns the correct block name for a synthetic `RayHit`; world delete actually removes the
   directory and world rename actually renames it on disk, checked with a real temp directory
   the way `worldlist_test.c`/`options_test.c` already do.
6. If the multiplayer option in §8 is taken: a chest opcode round-trip test the way
   `net/inv_bridge_test.c` exercises the player's own inventory bridge today.

### B. Visual, playtest-only

1. **The chest screen is usable with a stylus on a real 320x240 screen** — every slot is a
   comfortable tap target, the header and footer text are legible, and nothing overlaps.
   §4.5's mockup is a proposal to check against the real hardware, not a substitute for looking.
2. **Shift-move (double-tap) feels like one gesture, not two accidental taps** — the timing
   window (§4.7) is a number chosen on paper and has to be felt, not just measured.
3. **Opening a chest by aiming and pressing B reads as an action, not an accident** — B is
   reused from its existing "back/close" role (§4.6), and whether that overload is confusing in
   practice is a playtest question, not a code question.
4. **The aim-target HUD line does not clutter the crosshair** at normal render distance, across
   near and far targets.
5. **Battery blink** (`docs/research/ui-skin.md` §8, already shipped v1.8.3) is still not
   confirmed on real hardware. Not this version's job to build — it is already built — but
   worth re-flagging here since storage/QoL is exactly the kind of version a hardware pass
   would naturally also confirm it in.

Both halves must hold. A green suite with no look is unverified, and that rule is not this
document's own invention — it is `docs/plan-1.8.8-biome-identity.md:176`'s, restated because it
applies here without change.

---

## 2. Current state, read in this codebase, with file:line evidence

### 2.1 The block registry today

`source/world/registry.h:86-96` — `BlockDef` is `__attribute__((packed))`, 27 bytes, asserted:

```c
typedef struct __attribute__((packed)) {
    char    name[REGISTRY_NAME_MAX];
    uint8_t tex[BLOCK_FACES];
    uint8_t flags;
    uint8_t luminance;
    uint8_t hardness;
    uint8_t variant_of;
    uint8_t fluid_class;
} BlockDef;
_Static_assert(sizeof(BlockDef) == 27, "BlockDef must stay 27 bytes");
```

`kCoreDefs` (`registry.c:68` onward) currently defines **27 core rows, ids 0x00–0x1A** — air
plus 26 blocks ending at apple (id 26) — read directly off the live tree, which is further
along than `docs/VERSION-LIST.md`'s "current version 1.8.6" line states; the tree already
carries v1.8.8-shaped work ahead of its own changelog entry, the same situation
`docs/ROADMAP.md:347-357`'s v1.8.7 section documents for a different subsystem. `REG_ID_CORE_HI`
is `0x7F` (127) (`registry.h:27-28`, confirmed by `docs/plan-1.8.8-biome-identity.md:33-36`), so
**at least 100 free core ids remain** below it [reasoned: 127 − 26 − 1(air, already counted)].

`registryCrc16()` (`registry.c:655-680`) is CRC-16/CCITT-FALSE over every defined id ascending,
packed 28 bytes per row (1 id + 27 `BlockDef`). This is the join-time fingerprint
(`BS_APP_REGISTRY_INFO`, §2.5) — **any new core row moves it**, which is why §7's server release
is unavoidable the moment a chest becomes a real block.

`registry_test.c`'s hardness sweep (`coreHardnessIsDeclared()`, quoted in the research pass that
fed this document) walks every defined, non-liquid core id and asserts `hardness != 0` — a
chest with `.hardness = 0` fails the build, not just plays badly.

### 2.2 There is no per-block state anywhere — confirmed by direct read, not inferred

- **In memory.** `source/world/chunk.c:10-28` — a chunk is one of `CHUNK_FORM_UNIFORM` (no
  payload), `CHUNK_FORM_PALETTE4` (16-slot palette + 4-bit indices), or `CHUNK_FORM_RAW` (flat
  `BlockId[4096]`). All three are pure block-id representations. `chunk.h:29-37` deliberately
  keeps `struct Chunk` opaque outside `chunk.c` specifically so nothing outside can assume a
  fixed layout — which is good news for a chest side-table, since it never needs `chunk.c`'s
  cooperation at all.
- **On disk.** `source/world/chunk_codec.c` encodes a chunk as `CODEC_UNIFORM` / `CODEC_RLE` /
  `CODEC_RAW` — one byte per cell, palette or raw, no envelope for any extra per-cell payload.
- **On the wire.** `BS_BLOCK_EDIT_BYTES` is header + 4 + 4 + 4 + **1** (one byte, the id) — no
  room for anything else in a block-edit packet.
- **In the block-type registry.** `BlockDef` (§2.1) has six per-*type* fields shared by every
  placed instance of that id — name, six face textures, flags, luminance, hardness,
  variant/fluid class. None of them are per-*instance*.

Grepping `source/` for `chest`, `container`, `block entity`, `tile entity`, `blockdata`,
`BlockEntity` finds zero hits that mean any of those things — confirmed directly, not assumed.
**A chest requires an entirely new subsystem living outside `Chunk`/`Column`, keyed by world
coordinate.** §3.2 designs it.

### 2.3 The world memory budget has essentially no headroom left

`source/world/budget.h:24-98`, ARM-measured (compiled for `arm-none-eabi-gcc`, read out of the
emitted constants, the same discipline `docs/ROADMAP.md:60-70` insists on):

```
WORLD_BUDGET_BYTES = 12,582,912 B (12 MB)
one loaded column   = 65,648 B    (48 B Column + 32,832 B chunks + 32,768 B LightColumn)
radius 5             = 171 columns (169 loaded, 13x13, + 2 staging lanes)
                      = 11,225,808 B = 89.2% of the cap
```

**A per-column allocation for chest data is not available** — there is 10.8% of the cap left at
the New 3DS's own render-distance ceiling, and that margin belongs to the world store, not to a
new feature. This is the single fact that rules out "give every column a fixed chest-slot array"
and points at a sparse table instead (§3.2).

### 2.4 The closest existing precedent: `net/blockdiff.c`

A **bounded, chained-hash table keyed by world coordinate**, sitting **outside**
`WORLD_BUDGET_BYTES` as its own static allocation (`blockdiff.h:38-39`'s own comment: *"a slot
count, not a byte budget... this store's footprint is a separate static on top of it, not
carved out of it"*):

```c
typedef struct {
    int32_t  x, z;
    uint8_t  y;
    BlockId  id;
    uint16_t _pad;
    uint32_t next;
} BlockDiffEntry;   // exactly 16 bytes

typedef struct {
    BlockDiffEntry entry[BLOCKDIFF_MAX_PENDING];   // 65536 x 16 = 1,048,576 B
    uint32_t bucket[BLOCKDIFF_BUCKETS];             // 4096 x 4 = 16,384 B
    uint32_t free_head, high_water;
    int count, refusals;
} BlockDiffStore;   // 1,064,976 B total, stated exactly at blockdiff.h:79-82
```

Fixed slot count, **refusal on overflow, never silent eviction**, hashed at column granularity
so a lookup only walks one column's chain. This is the shape §3.2 copies, sized for a much
smaller entry count because a chest's payload is far larger than one block id.

### 2.5 The network protocol as it stands today

Wire opcodes live in `deps/blocksmith-server/proto/bs_proto.h` (a vendored, hash-pinned copy —
**not** the stale `server/proto/bs_proto.h`, which lacks these entirely). `BS_PROTO_VERSION` is
`1`. Application opcodes `0x01`–`0x0F` are used (`BS_APP_BLOCK_EDIT` … `BS_APP_WORLD_GEN`);
**`0x10`–`0xFF` are free**. Inventory sub-opcodes `0x00`–`0x06` are used inside
`BS_APP_INV_ACTION`; `0x07` upward is free there too.

The asymmetry that decides §7 and §8: an unrecognised **server→client** message type is silently
ignored by the client (`networld.c`'s `default: break;`); an unrecognised **client→server**
message gets the sender kicked (`bsgame.c`'s `default: send_kick()`). Stated plainly in
`bs_proto.h`'s own comment: *"a new CLIENT->SERVER message can only ever be introduced by
shipping the server first."* `docs/plan-1.8.8-biome-identity.md:1260-1262` records the same rule
being written into that version's own plan. **A new S→C opcode is safe client-first. A new C→S
opcode is not, ever.**

`registryCrc16()` is exchanged via `BS_APP_REGISTRY_INFO` (S→C, `{rev, count, crc16}`) right
after `WORLD_INFO`; a mismatch is a hard join refusal, not a soft warning.

### 2.6 The item ceiling was already split in two — read directly, not assumed

`source/world/inventory.h:44-137` (quoted at length because it is exactly the trap a chest
design can fall into if it copies the wrong half):

```c
static inline bool inventoryCanHold(ItemId item)
{
    return item != ITEM_NONE
        && registryIsDefined(item)
        && !registryView(item)->liquid;
}
```

This is what the **bag** (and, by extension, a chest slot, if it reuses the same `InvSlot`
shape) may hold today: any defined, non-liquid, non-air id — effectively the whole registry, not
a fixed count. But immediately below it:

```c
static inline bool inventoryItemOnWire(ItemId item)
{
    return item != ITEM_NONE && (uint32_t)item < BLOCK_COUNT;   // BLOCK_COUNT == 8
}
```

This is what a **multiplayer packet** may carry, and it is still 8, in a tree this client does
not own (`deps/blocksmith-server/game/validate.h`'s `BS_BLOCK_COUNT`, enforced by
`bsgame.c`'s two guards and `playerstate.c`'s armour clamp). The header's own comment
(`inventory.h:128-133`) states the two constants cannot move independently — closing the gap is
a coordinated client+server change, not something this version is obligated to do.

**Consequence for a chest, stated once so it does not have to be rediscovered:** a chest built
against `inventoryCanHold()` (the honest, registry-wide predicate) can hold items in single
player that **cannot currently be reported to a multiplayer server** — the same gap the
player's own bag already lives with today. §8 puts the actual multiplayer decision to the
owner; this section only establishes that the gap is pre-existing, not something v1.8.18
introduces.

### 2.7 Column save cadence — chests cannot copy the player inventory's cadence

`source/world/inventory.h:295-327` and `main.c:4241,5363` — the player's single `Inventory`
struct is **loaded once at world entry and saved once on the quit path**, nowhere in between.
That is fine for one struct that lives in RAM the whole session.

**Block/column data does not follow that cadence.** `app/worker.c:278` calls
`regionWriteColumn()` on a background worker thread, and `worldColumnRemove()` is called from
ordinary gameplay recentring (`main.c:977`) and on session teardown (`main.c:1554`) — a column
can unload **and be written to disk mid-session**, long before the player quits, every time the
player walks far enough that the render-distance ring recentres. A chest's contents are attached
to a specific block position inside a specific column, so **they cannot use the
load-once/save-at-quit cadence the player's own bag uses** — a chest whose column unloads
between two autosaves would lose everything placed in it since the last save. §3.3 designs
around this; §7 flags the one part of it that is a real, unmeasured cost rather than a solved
problem.

### 2.8 Crafting is single-input, single-output only — confirmed, not assumed

```c
typedef struct {
    const char* name;
    ItemId      input_item;
    uint8_t     input_count;
    ItemId      output_item;
    uint8_t     output_count;
} CraftRecipe;
```

`RECIPE_COUNT` is 4 today (dirt→grass, stone→sand, leaves→dirt, wood→planks). `crafting.h`'s own
comment states the project **rejected both shaped and shapeless multi-ingredient recipes** by
design — `CraftRecipe` is "the degenerate case of shapeless with a bag of one," deliberately.

> **⚠ CORRECTION [2026-09-03] — `RECIPE_COUNT` is 5 today, not 4.** `source/world/crafting.h`
> (read directly): the enum is `RECIPE_DIRT_TO_GRASS, RECIPE_STONE_TO_SAND,
> RECIPE_LEAVES_TO_DIRT, RECIPE_WOOD_TO_PLANKS, RECIPE_COAL_ORE_TO_TORCH, RECIPE_COUNT` — five
> named recipes before `RECIPE_COUNT`, so `RECIPE_COUNT == 5`. `RECIPE_COAL_ORE_TO_TORCH`
> landed via v1.8.12, unrelated to storage/QoL, between this document's research pass and now.
> This means the chest recipe below does not move `RECIPE_COUNT` from 4 to 5 — it moves it
> from **5 to 6**. This number was correct when researched and rotted once an unrelated
> version landed a fifth recipe; the two spots below that repeat "4 to 5" need the same fix.
**A chest recipe in this version stays inside that same shape** (§3.6) — the real Minecraft
8-planks-in-a-ring recipe needs a multi-slot grid this codebase has already decided against once,
and re-opening that decision is out of scope here (it belongs to whatever crafting-menu question
`docs/plan-1.9.1-interface.md` §6 answers, not to this document).

### 2.9 The atlas today

64 slots total (`ATLAS_TILE_COUNT`, permanent — 1024px is the PICA200's hardware texture-size
ceiling). **31 used (ids 0..30), 32 genuinely free (31..62), 1 permanently reserved** (slot 63,
the missing-texture marker). Claiming a tile touches five places in lockstep
(`registry.c`'s row, `block.h`'s `BLOCK_*` id, `block.h`'s `BTEX_*` constant, `make_atlas.py`'s
`TILES` list, `block_tiles_check.c`'s pairing assert) — this version's tile budget is §4.1.

### 2.10 Free input and screen-layout precedent

`source/gfx/sprite.h` — `SPRITE_MAX_QUADS = 1024`; a quad is 24 bytes (`SpriteVertex`); a draw
call is one flush per texture-bind change, not per quad. `source/gfx/font.h` — `fontDraw`/
`fontDrawf` emit one quad per glyph; `fontTextWidth()` returns pixel width. `SCR_W=320, SCR_H=240`
(`scene/ui_layout.h`). `SLOT_PX=40` (`320 / INV_HOTBAR_SLOTS`), already the established cell
size for both the hotbar and the debug-menu block list's icon column.

All seven remappable actions and their default keys, confirmed directly (`app/options.h:39-71`,
`scene/player.h`, `scene/interact.h`): `MOVE_FORWARD/BACK/LEFT/RIGHT` (circle pad),
`ACTION_JUMP` = **KEY_A**, `ACTION_BREAK` = **KEY_X**, `ACTION_PLACE` = **KEY_Y**. **`KEY_B`
appears nowhere in the live 3D gameplay loop** — every hit is inside a menu screen (pause,
title, remap, debug), always meaning "back/close." **`KEY_L`/`KEY_R` are already claimed**,
unconditionally whenever the game is not paused: `main.c:4510-4511`,
`if (!paused && (down & KEY_L)) genSetRadius(s_mesh_radius - 1);` / same for `KEY_R` and `+1` —
live render-distance stepping, gated only on `BS_WORLD_GEN && !BS_FLY` (both true in a shipping
build). This is the exact conflict §6.1 has to resolve to give L/R to hotbar cycling.

---

## 3. Storage design

### 3.1 The chest as a block

A new core registry row, one row, following `docs/plan-1.8.8-biome-identity.md`'s own hardness
table convention (assign by material class, never a bespoke number):

| Field | Value | Provenance |
|---|---|---|
| `name` | `"chest"` | my proposal |
| `.hardness` | 40 ticks | my proposal — "Timber" class, same as the shipped wood/planks rows (`registry.c:17-38`'s scale), since a chest is built from planks |
| `.flags` | `REG_FLAG_SOLID` | my proposal — targetable, not transparent, not a liquid |
| `tex[6]` | 2 new tiles (§4.1) | my proposal |
| id | next free core id after apple (26) — a **literal**, never `BLOCK_COUNT + n` (`block.h:86-87`'s own warning about exactly this tidying mistake) | my proposal |

A chest is **not** given a new `BLOCK_SHAPE` — it is `BLOCK_SHAPE_FULL_CUBE`, the same shape
every other placeable block uses. `block.h`'s three reserved shape bits stay reserved for slabs
and stairs, per `docs/plan-1.8.8-biome-identity.md:725`'s own note on that budget.

### 3.2 Where a chest's contents live — a sparse table, outside the 12 MB world budget

§2.3 rules out a per-column array. §2.4's `net/blockdiff.c` is the shape to copy — bounded,
chained-hash, keyed by coordinate, refusal-not-eviction — sized for a much heavier payload:

```c
#define CHEST_SLOT_COUNT 16   // §3.5 — matches the player's own main grid (INV_MAIN_SLOTS)

typedef struct {
    int32_t x, z;
    uint8_t y;
    uint8_t _pad[3];
    InvSlot slots[CHEST_SLOT_COUNT];   // 16 x 2 bytes = 32 B, reusing InvSlot verbatim
} ChestEntry;   // 4 + 4 + 1 + 3 + 32 = 44 bytes — no enum members, so ARM and host agree

typedef struct {
    ChestEntry entry[CHEST_MAX_LOADED];
    uint32_t   bucket[CHEST_BUCKETS];
    uint32_t   free_head;
    int        count, refusals;
} ChestStore;
```

**Every number below this line in this subsection is my proposal, not a measured or established
figure — it needs the same "compile for ARM, read the constant" treatment
`world_budget_bytes_test.c` already gives `Column`/`Chunk` before it is trusted (§2.3's own
citation discipline).**

- `CHEST_SLOT_COUNT = 16` — chosen to match `INV_MAIN_SLOTS` exactly (§3.5's screen layout
  reason), not a vanilla number.
- `CHEST_MAX_LOADED = 1024` — the cap on **loaded** chests at once (chests outside the render
  radius are not resident, following the same load/unload discipline as columns, §3.3). At 44
  bytes/entry that's 45,056 B; `CHEST_BUCKETS = 256` (a 4:1 entries:buckets ratio, `blockdiff.c`
  uses 16:1 at a much larger scale) adds 1,024 B. **Total ≈ 46,080 bytes**, a static allocation
  sitting beside `blockdiff.c`'s own 1.02 MB, outside `WORLD_BUDGET_BYTES` for the identical
  reason — the world budget has 10.8% headroom left and none of it is this feature's to spend.
  1024 concurrent loaded chests is comfortably above what a player is likely to place within a
  radius-5 ring in practice, but that is a guess, not a play-tested number — flagged in §7.
- Refusal, not eviction, on overflow — same guarantee `blockdiff.c`'s `refusals` counter gives:
  a full table refuses a **new** chest placement (or the load of one from a newly-entered
  column) rather than silently discarding an existing one's contents.

### 3.3 Persistence — a sibling save file, and a save cadence that matches columns, not the bag

`chunk_codec.c` has no envelope for extra per-cell payload (§2.2) — chest contents cannot ride
inside the region/chunk file format. The clean analogue already in this tree is `inventory.dat`
sitting beside the region files rather than inside them. Propose `<world_dir>/chests.dat`,
mirroring `inventory.dat`'s header shape exactly:

```
0x00  u32  magic       "BSC1"
0x04  u32  version     1
0x08  u32  count       number of ChestEntry records that follow
0x0C  u32  crc         crc32 over everything after this header
0x10  (ChestEntry) x count     44 bytes each
```

Crash safety: identical tmp-write → fclose → remove → rename pattern to `inventory.c`,
`options.c`, and `region.c` — the project's one proven shape for this, reused rather than
reinvented a fourth time. A missing file loads as **zero chests**, the same "nothing usable on
disk → safe empty default" contract every one of those three files already uses
(`inventory.h:307-313`). An existing pre-v1.8.18 world therefore opens exactly as it does today
— there is nothing in it that references chests, so nothing needs migrating.

**The cadence question, and it is the one real open risk in this section (see also §7).**
§2.7 established that block/column data saves on unload, not only at quit — `app/worker.c:278`,
triggered from `worldColumnRemove()`'s call sites. A chest's contents should save on the same
event, for the same reason: a chest whose column unloads mid-session must not lose its contents
before the next full-file rewrite. The naive version of this — rewrite the **whole**
`chests.dat` (up to ~46 KB at the proposed cap) on every single column unload — is a real SD-card
I/O cost that has not been measured and could be significant if a player is walking near the
render-distance edge, unloading columns every few frames. Two honest options, neither built or
measured here:

1. **Whole-file rewrite, but throttled** — a dirty flag set on any chest mutation, flushed on a
   timer (e.g. every 30 real seconds of play, following whatever cadence `tick.c` already
   exposes) and always at quit, accepting a small crash-loss window bounded by the timer period.
2. **Shard by region**, mirroring however `region.c` already shards column data (not read for
   this document — flagged, not assumed), so a chest only rewrites the shard its column belongs
   to, not the whole store.

**This document does not pick.** Option 1 is cheaper to build and is the recommended first pass
(§5, Phase 2); option 2 is the more scalable answer if a hardware pass shows the naive rewrite is
too slow, and deciding between them needs the same kind of measurement `docs/ROADMAP.md`'s own
v1.8.6 entry insists on before committing to a performance claim — not paper arithmetic.

### 3.4 Network — the part that is genuinely a fork (see §8)

If a chest is ever opened on a multiplayer server, it needs the shape `net/inv_bridge.c` already
established for the player's own bag (§2.4's citation), plus a subscription scope, since unlike
the player's bag a chest is **shared**, not per-connection:

- **S→C, safe client-first**: `BS_APP_CHEST_STATE = 0x10` — `{x i32, z i32, y u8, slots u8[32]}`,
  41 bytes + header. Sent whenever a chest in a column the client is already subscribed to
  changes — piggybacking on the **existing** `BS_APP_CHUNK_SUB`/`CHUNK_UNSUB` (0x05/0x07) scope
  rather than inventing a second subscription mechanism.
- **C→S, needs the server to ship first, no exceptions** (§2.5): `BS_APP_CHEST_ACTION = 0x10`
  in the 0x10-and-up C→S space (a **different number range than the S→C one above** — the two
  directions do not share a namespace, matching how `BS_APP_*` already works) — `{x, z, y, op,
  operands}` mirroring `BS_INV_ACTION`'s op-byte shape exactly (move/swap/split, applied to
  chest slots or across the chest/inventory boundary).
- **The wire-ceiling gap (§2.6) applies unchanged.** A chest holding an item id ≥ 8 (everything
  past the original eight) cannot be reported to, or trusted from, a server until
  `BS_BLOCK_COUNT` and its two guards move — the same pre-existing gap the player's own bag has
  lived with since `inventoryCanHold()` was widened. This version does not have to close that
  gap to ship a chest; it has to be honest that a **shared, server-authoritative** chest
  inherits it (§8).

### 3.5 The chest screen — bottom screen, 320x240, full-screen overlay

Reuses `SLOT_PX = 40` throughout, the same cell size the hotbar and main grid already use, so no
new touch-target-size decision is being made — `inventory.h:148-155`'s own 7.6mm-stylus-target
reasoning for 40px cells carries over unchanged. Drawn as a **full-screen bottom overlay**,
replacing the normal HUD for that frame — the same pattern `app/debugmenu_ui.c:199-206` already
uses (cited in `docs/research/ui-skin.md` §4), not a panel drawn alongside it.

Chosen order, top to bottom — chest first, then player main, then player hotbar last (closest to
the thumbs) — deliberately the opposite of the always-visible inventory screen's hotbar-on-top
layout (`ui_layout.h`'s `HOTBAR_Y=0`). That is a real, separate layout, not a variant of the
existing one, and the ordering is my proposal, chosen to match the near-universal
container-screen convention ("my stuff, closest to me, is always last") rather than for any
technical reason.

```
+----------------------------------------------------------------------+  y=0
| CHEST                                                                |  header, 0-17
+----------------------------------------------------------------------+  y=17
| [  ][  ][  ][  ][  ][  ][  ][  ]                                     |  chest row 1, 17-57
| [  ][  ][  ][  ][  ][  ][  ][  ]                                     |  chest row 2, 57-97
+----------------------------------------------------------------------+  y=97 (divider, 4px)
| [  ][  ][  ][  ][  ][  ][  ][  ]                                     |  player main r1, 101-141
| [  ][  ][  ][  ][  ][  ][  ][  ]                                     |  player main r2, 141-181
+----------------------------------------------------------------------+  y=181 (no divider)
| [  ][  ][  ][  ][  ][  ][  ][  ]                                     |  player hotbar, 181-221
+----------------------------------------------------------------------+  y=221
| tap: pick up / drop    double-tap: quick-move    B: close            |  footer, 221-240
+----------------------------------------------------------------------+  y=240
```

(Rendered above in a monospace box wider than 320px only because Markdown needs the border
characters; every y-coordinate is the real pixel value against the 320x240 target.)

**Interaction model — extends the existing gesture, does not replace it.** `scene/ui.c`'s
established pattern is a two-tap pick-up/drop with no drag (`docs/research/ui-skin.md` §4,
confirmed live at `ui.c`). This screen treats the chest's 16 slots and the player's 24 slots as
one addressable space for exactly that gesture — tap a chest slot, tap a player slot, contents
swap/merge exactly as `inventorySwapSlots`/`inventoryMoveUnits` already do for two slots in the
same `Inventory` today, just now operating across two different backing arrays through one
screen-index-to-(chest-slot | player-slot) mapping. **No new primitive is needed in
`world/inventory.c` or a hypothetical `world/chest.c` beyond what already exists** — the UI
layer is what's new, not the slot-manipulation logic.

**Quad cost (§1's own budget discipline — count every screen):**

| Element | Count | Quads each (worst case) | Total |
|---|---|---|---|
| Chest slots | 16 | 4 (bg + icon + 2-digit count) | 64 |
| Player main slots | 16 | 4 | 64 |
| Player hotbar slots | 8 | 4 | 32 |
| Header text ("CHEST") | 5 glyphs | 1 | 5 |
| Footer text (two lines, ~50 chars) | ~50 glyphs | 1 | 50 |
| Divider rects | 1 | 1 | 1 |
| **Total** | | | **≈ 216** |

Against the 1024-quad cap this is comfortably inside budget on its own — and because this is a
**full-screen bottom overlay replacing the normal HUD draw**, not an addition to it, it is not
additive to `docs/research/ui-skin.md`'s own ~790-of-1024 worst-case estimate for ordinary play;
it is an alternative bottom-screen pass, the same relationship the debug menu already has to the
normal HUD.

### 3.6 The chest recipe — stays inside the existing single-input shape (§2.8)

```c
{ "Chest", BLOCK_PLANKS, 6, BLOCK_CHEST, 1 }   // my proposal
```

`RECIPE_COUNT` moves from 4 to 5, appended, never renumbered — the same append-only discipline
`atlas_tiles.h` and `block.h`'s id lists already use. ⚠ CORRECTION [2026-09-03]: `RECIPE_COUNT`
is already 5 today (see §2.8's correction) — this moves it from **5 to 6**, not 4 to 5. Six planks is a proposal, not sourced from
anywhere — it is deliberately *not* eight (real Minecraft's ring recipe), specifically because
this system has no concept of "arranged in a ring"; a flat count is the only shape
`craftCanMake()`/`craftMake()` understand. If the owner wants the recipe to *feel* like the
familiar 8-plank chest despite the flat-count mechanism, that is a one-line change
(`input_count = 8`), not a design question — noted here only so the "6" isn't mistaken for
anything more considered than a round, modest number.

### 3.7 Stack splitting, merging, and shift-move — what already exists vs. what is new

`ROADMAP.md`'s own wording for this version names "stack splitting and merging, shift-move."
**Splitting and merging already exist at the logic layer, host-tested, today**:
`inventorySplitStack()` and `inventoryMoveUnits()` (`inventory.h:257-281`) do exactly this —
they are simply not yet reachable through any touchscreen gesture beyond the plain two-tap
swap, because there has never been a second container to move *between* until this version.
**Shift-move is the one genuinely new gesture**, and it only makes sense once a chest screen
exists — it is designed as part of §3.5, not separately:

- **Single tap** — existing gesture, unchanged: pick up a stack, tap again to drop/merge.
- **Double-tap (my proposal)** — quick-transfer the tapped slot's entire stack to the other
  container (chest→player or player→chest, whichever side wasn't tapped), landing in the first
  slot that already holds the same item with room, or the first empty slot if none does —
  mirroring `inventoryAdd()`'s own existing fill order (`inventory.h:218-226`: merge into
  existing stacks first, scanning low index to high, then spill into empty slots). **No new
  merge/placement logic is needed** — this is `inventoryAdd()` and `inventoryRemove()` called
  back-to-back across the two backing arrays, driven by a new input gesture, not a new rule
  about where things go.
- The double-tap timing window is a UI constant with no existing precedent to anchor it to in
  this codebase (nothing currently double-taps) — propose 300ms, a common touchscreen default,
  and flag it explicitly as needing a real playtest (§1.B item 2), not a number to trust from
  this document alone.

---

## 4. Interface pieces this version needs that are not chest-specific

### 4.1 Atlas budget

Two new tiles, minimal by design given `docs/ROADMAP.md`'s "several other in-flight versions
want tiles" constraint and the "count yours" instruction: `TILE_CHEST_TOP` (a lidded-plank look)
and `TILE_CHEST_FRONT` (the same base with a latch mark, so the block reads as a chest rather
than a plain crate from the front face; side faces reuse `TILE_CHEST_TOP`). **2 of the 32 free
slots claimed, 30 left** for other work. A three-tile version (distinct front/top/side) was
considered and rejected here as the more expensive default — noted as an easy upgrade later if
the two-tile version reads as too plain on a real screen (§1.B is where that gets judged).

### 4.2 A new gameplay action: opening a chest

There is currently **no interact/use verb in this game at all** — the four existing recipes are
always available from the always-open crafting panel, not gated behind approaching a block, so
nothing in this codebase has ever needed a "walk up and press a button" trigger before. A chest
needs one.

§2.10 confirms `KEY_B` is unclaimed anywhere in the live 3D gameplay loop — every existing use is
inside a menu, always meaning "back/close." **Proposal: aiming at a placed chest and pressing B
opens it; B also closes the chest screen once open**, which keeps B's meaning consistent (always
the "leave/dismiss the current context" button) rather than overloading it with an unrelated
"confirm" meaning. Whether reusing a back-button as an open-button reads as natural rather than
surprising is exactly the kind of thing that needs a look, not a diff (§1.B item 3).

This is a genuinely new `ACTION_INTERACT`-shaped concept, not a rebinding of an existing action —
whether it becomes an eighth remappable action (defaulting to B) or stays a hardcoded B-only
trigger for this one version is a real, small design fork, called out in §8.

---

## 5. Build order

Each phase independently testable and independently playtestable, cheapest-thing-that-removes-
the-most-uncertainty first, and the one irreversible obligation (the forced server release, §7)
held off until the phase that actually needs it — the same ordering discipline
`docs/plan-1.8.8-biome-identity.md:519-533` uses and states its reasons for.

**Phase 1 — The three QoL items, no chest involved at all.** Hotbar L/R cycling (§6.1), the
aim-target HUD readout (§6.2), world rename/delete (§6.3). Zero registry change, zero CRC move,
zero server exposure — the cheapest, most self-contained slice of this whole version, and the
one an owner can see working in thirty seconds each. Ships and is playtestable before anything
else in this document is touched.

**Phase 2 — The chest as a single-player-only block.** Registry row + atlas art (§3.1, §4.1),
the sparse table and `chests.dat` with the throttled-save option from §3.3 (not the sharded one —
cheapest first), the chest screen (§3.5), the recipe (§3.6), shift-move (§3.7). No network code
at all in this phase — a chest works today exactly as well in single player as it ever will,
which is most of this version's value, shipped before the harder multiplayer question is even
asked.

**Phase 3 — The forced server release.** New core registry row moves `registryCrc16()` — ship
`kCoreDefs` server-side first or simultaneously, exactly as §2.5 and every prior version that has
touched the registry already requires. This is the phase that makes an old server refuse a new
client (correctly) until it upgrades.

**Phase 4 — Multiplayer chest sync, only if §8 says to build it at all.** The opcodes in §3.4,
the wire-ceiling caveat made concrete in a real test, the subscription piggyback on
`CHUNK_SUB`/`CHUNK_UNSUB`. This phase does not exist if the owner picks the "defer" option in
§8.

---

## 6. Quality of life — a real audit, not a generic list

Read directly: `main.c`, `app/input_map.c`, `app/remap.c`/`remap_ui.c`, `app/options.c`,
`app/session.c`, `scene/pausemenu.c`, `scene/worldlist.c`, `scene/player.c`, `scene/interact.c`,
`scene/crosshair.c`, `scene/highlight.c`, `world/mining.c`, and the multiplayer join path in
`net/bsnet.c`/`title.c`.

| # | Item | Status found | Genuinely missing? |
|---|---|---|---|
| 1 | Hotbar cycling via L/R | Touch-only; `ui.c:363`'s only trigger is a screen tap; the HUD literally says *"tap a hotbar slot to select it"* (`ui.c:306`) | **Yes** |
| 2 | Break auto-repeat while held | Already level-triggered (`interact.c:341`, `holding_break`), accumulates over held frames — a deliberate, already-good design | No |
| 3 | Placement inside the player's own box | Already guarded, `interact.c:404-409`, `boxOverlapsCell()`, with a non-nullable `body` parameter specifically so the check can't be skipped | No |
| 4 | Aim-target HUD feedback (name/distance) | `crosshair.c` draws a static reticle only; `highlight.c` draws pure 3D geometry, no text, no block-name lookup anywhere | **Yes** |
| 5 | World rename / delete | `worldlist.c` exposes only `Scan`/`NameValid`/`Create` — no delete, no rename function anywhere in the tree | **Yes** (both halves) |
| 6 | Settings persistence | `sdmc:/blocksmith/options.ini`, robust, crash-safe, already shipped | No |
| 7 | Multiplayer join-failure messaging | Specific, actionable, on-screen (`bsnet.c:88-107`, `title.c:786-799`) — *"No /blocksmith/server.txt on the SD card"* and similar, already well designed | No |
| — | Bonus, not ranked | No quit-confirm on pause menu; no in-game keybind reminder; no in-game version string outside the title screen's Update page; no inventory sort/quick-stack | Minor, not built here |

**Ranked by value ÷ cost, in the order they are built (§5, Phase 1):**

### 6.1 Hotbar cycling via L/R — highest value, real cost is the button conflict, not the feature

The feature itself is trivial: on an unpaused L/R press, step `selected_hotbar` by ±1 with wrap,
calling the existing `inventorySelectHotbar()` (already clamps out-of-range input,
`inventory.h:285-288`) — a few lines in `main.c`'s input dispatch, no new function needed
anywhere.

**The real cost is that L/R are already claimed** (§2.10) for live render-distance stepping,
unconditionally whenever the game is not paused. **Proposal: remove the live, unpaused L/R
render-distance stepping** — it is already reachable through the pause menu's own
`OPT_ROW_DIST` row (`pausemenu.c`), so nothing is lost, only a redundant second path to the same
setting. This is a real behaviour change to an existing, currently-live control, which is why it
is named again in §8 rather than only decided here.

### 6.2 Aim-target HUD readout — second-highest value, moderate cost

`highlight.c` already computes the `RayHit` the crosshair cage is drawn from; nothing currently
turns it into text. Proposal: a small `fontDrawf` line near the crosshair (top screen, since
this is live 3D-view feedback, not a list — the project's own established "lists live on the
bottom screen" rule (from the debug block list) does not apply to a HUD readout tied to the
3D camera) showing `blockInfo(hit.block)->name`, drawn once per eye for stereo parity
(matching `crosshair.c`'s own zero-parallax-per-eye pattern). Roughly 10-15 glyphs =
10-15 quads per eye, ~20-30 total — negligible against the 1024 cap.

### 6.3 World rename / delete — third-highest value, moderate cost, needs a confirm step

Neither exists in `worldlist.c` today. Proposal: `worldlistDelete(name)` (an `rmdir`-style
recursive directory removal — real I/O, needs care) and `worldlistRename(old, new)` reusing
`worldlistNameValid()`'s existing validation, both reachable from the world-select screen with
the on-screen keyboard (`swkbdInit`) `title.c` already uses elsewhere for name entry. **Delete
needs a confirm dialog** — there is no precedent for one anywhere in this codebase (the pause
menu's own "Quit to title" is itself unconfirmed today, noted as a bonus item above, not fixed
here since nobody asked for it) — proposing the simplest version: a single "press A again to
confirm" second screen, no new UI framework.

---

## 7. Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| The chest table's 1024-entry / 46 KB budget (§3.2) is a guess, not a measurement | Build the refusal counter first (§1.A item 3) so an undersized cap fails loudly (a counted refusal) rather than silently, and raise the constant later if real play hits it — cheap because it's a single `#define`, not a format change |
| `chests.dat`'s whole-file rewrite-on-unload cost is unmeasured (§3.3) | Ship the throttled-timer version first (Phase 2), measure real SD-card write time on hardware, move to the sharded version only if the number is bad |
| Removing live L/R render-distance stepping (§6.1) changes an existing, currently-shipped control | Named explicitly in §8 as an owner decision, not assumed — the pause-menu path is a real, already-shipped fallback, so nothing is lost if he says yes |
| Reusing B for both "open chest" and "close chest/menu" (§4.2) may read as confusing | Cheapest possible fallback if a playtest says so: make it a proper 8th remappable action defaulting to B, so an owner or player who dislikes the overload can move it, at the cost of one more row in `remap_ui.c`'s list |
| A chest built against `inventoryCanHold()` can hold items a multiplayer server cannot yet be told about (§2.6, §3.4) | §8's "defer multiplayer sync" option sidesteps this entirely for v1.8.18; if sync is built anyway, the gap is inherited knowingly rather than discovered during a bug report, which is the whole point of §2.6 existing in this document |
| A new core registry row forces a server release (§2.1, §2.5) the moment the chest block itself lands, even before any network sync code is written | Phase 3 is deliberately isolated in §5 so the forced release happens once, for the block existing, not twice |

---

## 8. HIS CALL

Design forks this document could not settle on its own, because each is a scope or behaviour
decision, not an implementation detail:

1. **Multiplayer chest sync: build it now, or defer it.** Building it (§3.4, §5 Phase 4) is
   real, new cross-repo work — a new S→C opcode, a new C→S opcode that forces a server release
   of its own on top of the one the block itself already forces, a subscription piggyback, and
   an honest exposure to the pre-existing wire-ceiling gap (§2.6) for any chest holding an item
   past the original eight. Deferring it means a chest placed on a multiplayer server in
   v1.8.18 either doesn't exist there yet (server refuses the block outright until it's added)
   or exists but is silently single-player-only in its effect (a much worse option, not
   proposed here) — the clean version of "defer" is: **the chest block simply is not defined on
   a server build until this is built**, which is a normal, ordinary registry-row decision, not
   a compromise.
2. **Removing the live, unpaused L/R render-distance stepping** (§6.1) to free L/R for hotbar
   cycling. The pause-menu path already covers the same setting, so nothing is lost — but it is
   still a change to a control that has shipped and been playable since v1.4.0, and changing an
   existing live control is squarely a "his call," not an implementation detail.
3. **Chest capacity: 16 slots (this document's proposal, matching the player's own main grid) or
   some other number.** 16 was chosen for a clean, reused screen layout (§3.5), not because it is
   the "right" chest size — a real double-chest-sized container (32+ slots) is a straightforward
   later expansion of the same sparse-table design (§3.2), just not this version's default.
4. **The chest recipe's exact cost** (§3.6: 6 planks, proposed) — any number is equally cheap to
   build; this document picked one so the design has a concrete shape to review, not because 6
   is load-bearing.

---

## LINES SOMEONE ELSE MUST ADD

Exact, load-bearing values a future implementer needs to get right, collected here rather than
scattered through the narrative above — everything else in this document is intentionally
descriptive ("what to build"), matching `docs/plan-1.8.8-biome-identity.md`'s own style, and
does not belong in this section.

- **`source/world/block.h`** — a new `BLOCK_CHEST` id literal appended after the current
  highest core id (27, per §2.1's read of the live tree), spelled as a literal, never
  `BLOCK_COUNT + n` (`block.h:86-87`'s own warning).
- **`source/world/registry.c`** — one `kCoreDefs` row: `.name = "chest"`, `.hardness = 40`,
  `.flags = REG_FLAG_SOLID`, `.tex = { TILE_CHEST_TOP, TILE_CHEST_TOP, TILE_CHEST_FRONT,
  TILE_CHEST_TOP, TILE_CHEST_TOP, TILE_CHEST_TOP }` (face order per `BLOCK_FACES`, front face
  wherever that order places it — confirm the exact face-order enum before writing this row).
- **`source/gfx/atlas_tiles.h`** — `TILE_CHEST_TOP` and `TILE_CHEST_FRONT`, appended above
  `TILE_USED_COUNT`, append-only.
- **`source/world/block_tiles_check.c`** — a matching line in `BS_BTEX_TILE_PAIRS` for each new
  tile, or the build fails its own `_Static_assert`.
- **`source/world/crafting.c`** — `{ "Chest", BLOCK_PLANKS, 6, BLOCK_CHEST, 1 }` appended to the
  recipe table; **`source/world/crafting.h`** — `RECIPE_COUNT` moves from 4 to 5.
  ⚠ CORRECTION [2026-09-03]: from **5 to 6** — `RECIPE_COUNT` is already 5 today, see §2.8.
- **`source/world/registry_test.c`** — the pinned CRC golden and `REGISTRY_FULL_COUNT_PIN` both
  move to new values by hand, with a dated comment, the same way every prior core-row addition
  has recorded the move (`registry_test.c`'s own chain of such comments).
- **`deps/blocksmith-server/proto/bs_proto.h`** (only if §8 item 1 says "build sync now") —
  `BS_APP_CHEST_STATE = 0x10` (S→C) and a separate `BS_APP_CHEST_ACTION` value in the C→S
  0x10-and-up range; ship this file's server side **first**, never alongside or after the
  client (§2.5).
- **`tools/make_atlas.py`** — `tile_chest_top()` and `tile_chest_front()` painter functions,
  procedurally generated per this project's own standing rule (`make_atlas.py:3-6`), appended to
  the `TILES` list in the same order as the `atlas_tiles.h` entries above.


---

## Dated correction: 2026-09-03 06:55

A second planning pass (`plan-1.9.0-storage.md`, kept alongside this file as a second opinion)
re-derived parts of this spec against a later tree and found three things worth carrying back
here. This section is appended rather than edited into the body above, so the original reasoning
stays readable and it is obvious what changed and when.

**1. The multiplayer-sync argument in this document rests on a gap that has since been closed.**
Sections 2.6, 3.4 and 8 argue from a wire item-id ceiling that no longer applies: it was closed
in **v1.8.10 (2026-09-02)**, verified first-hand at `inventory.h:145-186` -- one day before that
pass ran, and after this document was written. Anything here that concludes "the wire cannot
carry this" needs re-deriving before it is acted on. This is the exact failure mode recorded as
"a deferred decision keeps its premise and loses its evidence": the conclusion outlived the fact
it rested on.

**2. `KEY_B`: not a correction after all -- corroboration, plus a warning about the grep.**
I first wrote this up as a finding. It is not one: **section 2.10 above (and its proposal at
lines 581-586) already says `KEY_B` is unclaimed in the live 3D gameplay loop**, already
proposes aiming at a chest and pressing B to open it, and already makes the argument that this
keeps B meaning "leave/dismiss the current context". The second pass reached the same
conclusion independently, from a different direction. That is worth having -- two passes
agreeing is the strongest signal either document carries -- but it changes nothing here, and
filing it as a fix would have misrepresented this document as having been wrong.

What IS worth adding is **how it must be checked**, because the obvious grep gives a false
negative. Grepping `KEY_B` in `scene/interact.c` and `main.c` returns 0 -- but so does
`KEY_X`/`KEY_Y` in `interact.c`, because that file never names a raw key constant at all: it
goes through `inputKey(ACTION_BREAK)` and `inputKey(ACTION_PLACE)`. A zero there means "this
file uses the binding layer", **not** "this button is free". Only a whole-tree grep answers it:
`KEY_B` appears at `app/debugmenu_ui.c:195,216`, `app/remap_ui.c:105,168`,
`scene/pausemenu.c:143,151` and eight sites in `scene/title.c` -- every one a menu, title or
overlay screen, none of them the gameplay loop.

This is the recorded "grep the caller, not the primitive" trap, and it is worth the paragraph
because the wrong grep here returns exactly the answer you were hoping for.

**3. The break-cleanup hook this spec calls for now EXISTS.** That pass observed
`it->broke_valid` / `broke_x/y/z` present in `scene/interact.h` but wired to nothing, and
correctly reported it as unwired. That observation went stale within the hour: they are now
consumed at `main.c:5869`, where `blockStateRemove` is called for every landed break. So the
storage work does not need to build that plumbing -- it needs to *use* it.

Unchanged by any of the above: both documents independently arrived at the same core
architecture -- a **dedicated side table for chests rather than reusing `BlockStateTable`** --
so that chests do not compete with furnaces and future stateful blocks for one 64-slot pool.
Two passes reaching that conclusion from different directions is the strongest signal either
document carries.
