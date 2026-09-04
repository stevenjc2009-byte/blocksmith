# v1.8.18 — Monsters

Zombies and skeletons, spawning in the dark of caves, on a light rule that makes a torch
worth placing.

This document does not implement anything. It **supersedes `docs/plan-1.8.16-monsters.md`**,
which was written against a tree where the entity system was a design document and animals
had not shipped. Almost every structural assumption in that file has since been overtaken by
real code — some of it in this version's favour, some of it not. §12 lists what went stale
and why. Read this document instead of that one; read that one only for its research
citations, which are still good.

`docs/plan-entities.md` remains the architectural parent and is cited throughout rather than
re-argued. Where it and the shipped code disagree, **the code wins** — §2 says where they
disagree.

## Provenance legend

Same as the rest of this set, with one change made deliberately.

- **`[read]`** — read out of this tree during this pass, with the file named.
- **`[measured]`** — a real number that came back from running or counting something, not
  from arithmetic on paper.
- **`[reasoned]`** — derived from something read, with the derivation shown.
- **`[research-given]`** — a figure from the outside research pass cited by
  `plan-1.8.16-monsters.md`, restated here with its original attribution intact. **Not
  independently re-fetched during this pass** — see §Sources.
- Bare numbers are my own proposal and carry no authority beyond the reasoning next to them.

The change: `plan-1.8.16-monsters.md` used `[read]` for things it had taken from another
*planning document* rather than from code. That is how it ended up asserting a `main.c` state
that was already false. In this document `[read]` means source, never a plan.

---

## 0. What this version is, and the slot it moved into

The owner's ask, in full: *"zombies and skeletons in caves."*

This was originally v1.8.16. It did not ship there — v1.8.16 became three reported fixes plus
food items, animal textures and smooth cross-block lighting, and `docs/ROADMAP.md:381-397`
now says so outright and points forward to v1.8.18. `docs/VERSION-LIST.md:848-854` carries the
matching planned entry. Nothing was descoped in the move; the feature simply waited for a free
slot.

**A naming collision that existed when this was written, and has since been cleared.** Two
files in this directory used to be called `plan-1.8.18-storage.md` and
`plan-1.8.18-storage-qol.md` despite not being about this version: storage and quality of life
moved to **v1.9.0** (`docs/ROADMAP.md:443`) in the same correction pass that moved monsters to
v1.8.18, and those two filenames were not renamed with it. As of [2026-09-03] they have been,
to `plan-1.9.0-storage.md` and `plan-1.9.0-storage-qol.md`, along with three more that had
drifted the same way (`plan-1.9.1-interface.md`, `plan-1.9.2-redstone.md`,
`plan-1.9.3-dimensions.md`). Each renamed file carries a header note recording its old name, so
an old citation is traceable rather than merely broken. This document remains the only v1.8.18
plan.

---

## 1. Success criteria

Split the way `docs/plan-1.9.0-storage-qol.md:47-97` splits it, and for its stated reason: a
green host suite and "it feels right in a cave" are different claims, and conflating them is
how an unverified thing gets called done. Both halves must hold.

### A. Measurable, host-only

1. **The darkness predicate accepts nothing lit.** Over **at least 500 candidate positions**
   across varied seeds, 100% of accepted spawns have `lightGetSky == 0 && lightGetBlock == 0`,
   and **0% of surface-daylight samples are accepted**. Stated as a rate over a distribution,
   never as a single sample — this project has already had a 47-of-47 pass conceal a 28%
   failure rate.
2. **The spawn footprint is real.** 100% of accepted spawns stand on a solid block with two
   air cells above, 0% land inside solid geometry, 0% land in a column `worldColumn()` returns
   `NULL` for.
3. **The monster sub-cap holds.** With the animal spawner and the monster spawner both running
   against a generated world, occupancy never exceeds `ENTITY_CAP_OLD` (24), and monsters never
   exceed their own sub-cap, over a long run — not a single tick.
4. **`animalDef()` still returns `NULL` for nothing it should.** `source/entity/animal_test.c`
   pins today that kinds 5/6/7 are not animals. That pin must still pass **unchanged** after
   monsters exist — a monster is not an animal, and the moment `animalDef(5)` starts returning
   a row, `animalThink()` will start running wander AI on a zombie.
5. **`entitymodel.c`'s static assertions still tie the model table to the kind enum.** They
   are the build-time trip-wire that stops the box table and the kind list drifting apart; a
   monster that renders as a pig is exactly what they exist to prevent.
6. **The player can actually be killed by a monster** in a host test: a synthetic hostile
   adjacent to a synthetic player drives `health` from 20 to 0 through the new damage entry
   point, and the death path fires once, not once per tick.
7. **Every suite's check count is pinned and updated deliberately.** `animal_test.c` pins 236
   internally *and* in `tools/run_host_tests.sh:5443-5451`, in two places on purpose. Any new
   suite follows that convention. **The exit code is the only gate** — that file says so at
   :5437, because a FAIL-string grep has already missed a format on this project.

### B. Visual, playtest-only

1. **A cave with no torch in it produces monsters, and a cave with a torch in it stops.**
   This is the whole feature. It is a look-at-it claim and no host test replaces it.
2. **A zombie reads as coming for you**, not as a pig that happens to be walking your way. If
   the chase is invisible at the speeds involved, the number is wrong, not the design.
3. **The skeleton's attack is dodgeable** — whichever of §3.5's two options is chosen, the
   player must be able to tell they are about to be shot and do something about it.
4. **The monster skins read as a zombie and a skeleton at 16 texels to the block**, right way
   up, with no seam bleed between quadrants. Render the sheet at UV 0..1 on a quad and **look
   at it before trusting a single model UV** — a wrong texture constant on the PICA200 renders
   *a* texture, never an error, and this project has lost time to exactly that.
5. **Frame rate holds on a real Old 3DS with the 3D slider up**, in a cave, at the cap, with
   monsters and animals both loaded. See §7 for why this is the criterion that actually binds
   and why no existing number can answer it in advance.

---

## 2. Current state, read in this codebase

Every claim below was read during this pass. `git status --porcelain` shows seven other lanes
mid-edit; none of them touch the files this section relies on except `source/main.c`, which is
under concurrent edit and whose **line numbers should be re-derived at implementation time,
not taken from here**. This document quotes `main.c` line numbers only where nothing else
identifies the site, and flags each one.

### 2.1 The entity system, and exactly where it is animal-shaped

`source/entity/entity.h`/`.c` is genuinely generic and needs **no edits** to carry a hostile
mob `[read]`. It gives, for free:

- the fixed pool, slot/id separation, `entitySpawn` / `Despawn` / `Kill` / `At` / `Get` /
  `FindById`;
- `entityTick()` with distance decimation, and an `EntityThinkFn` hook that is the seam a
  behaviour plugs into;
- despawn when the entity's column unloads, checked before physics every tick;
- full physics parity with the player — gravity, water, block AABB collision — because `Body`
  is embedded and every entity goes through the same `bodyStep()`/`bodyMove()` the player
  does. `entity_test.c` regression-tests that the player's own box is bit-identically
  unchanged by this sharing.

`kind`, `ai_state` and `ai_timer` are documented in `entity.h` as opaque carriers with no
meaning assigned by `entity.c`. That is the extension point, and it is real.

**Where it is animal-shaped**, concretely `[read]`:

| Thing | File | What a monster hits |
|---|---|---|
| `ENT_KIND_PIG..SHEEP = 1..4`, `ENT_KIND_COUNT = 5` | `entity/animal.h:43-48` | `ENT_KIND_COUNT` means "one past the last **animal**", not one past the last entity. A monster kind is 5/6/7 and lives *outside* it. |
| `AnimalDef kDefs[ENT_KIND_COUNT]` | `entity/animal.c:98-107` | A flat table indexed by kind. Monsters need their own table; `animalDef()` returning `NULL` above 4 is a **pinned test**, not an accident. |
| `animalThink()` | `entity/animal.c:193-245` | Early-returns on `!animalDef(kind)`, so it will correctly ignore a zombie — but it will not drive one. |
| The single `EntityThinkFn` argument | `main.c`, at the `entityTick()` call | **`entityTick()` takes one think function.** Today it is `animalThink`. Two creature families need a dispatcher. This is the one structural change monsters force, and it is small. |
| `animalCount()`, `animalCapFor()`, `animalRaycast()`, `animalHurt()` | `entity/animal.{c,h}` | Each hardcodes "is this kind 1..4". Each needs a monster-side twin, or generalising. |
| `kEmBoxes[EM_KINDS][6]`, `EM_KINDS = 4` | `scene/entitymodel.h:67,135` | The render table is 4 kinds wide and statically asserted against `ENT_KIND_COUNT - ENT_KIND_PIG`. It already *skips* kinds 5-7, so a monster will not render as a pig — it will not render at all. |

**The good news, and it is better than `plan-1.8.16-monsters.md` knew.** The animal layer was
built anticipating this version, and said so in comments that are still there `[read]`:

- `animal.h:47` — `/* 5 = zombie, 6 = skeleton, 7 = arrow -- reserved for v1.8.16, do not
  reuse */`, and the reservation is **enforced by a test**, not merely commented.
- `AnimalDef.hostile` (`animal.h:95`) already exists as a byte, documented as *"0 for every
  animal. The field v1.8.16 fills in."* It is allocated, aligned, pinned by `offsetof` checks,
  and read by nothing.
- `ANIMAL_CAP_OLD 16` / `ANIMAL_CAP_NEW 32` (`animal.h:70-71`) are soft sub-caps on the shared
  pool whose stated purpose is *"hold back a third of each for v1.8.16's monsters, so that
  version does not arrive to find every slot full of sheep."* Both are statically asserted
  against the pool caps.

So the eviction problem `plan-1.8.16-monsters.md` raised as an open risk was **already
mitigated in shipped code** — not solved, but reserved against. §10 still carries the residual
decision.

### 2.2 The pool, the caps, and the byte cost

- `sizeof(Entity)` = **60 bytes on ARM, 64 on host** `[read]` — `entity.h:88-90` states it, and
  a field-by-field re-derivation during this pass agrees. The two differ because `Body` embeds
  a `BodyWet` enum and the ARM EABI compiles `-fshort-enums`. **Do not write a
  `_Static_assert` on `sizeof(Entity)`** — `animal.h:85-89` says so explicitly, because it
  would be a different number on each machine. `AnimalDef` *is* pinned at 20 bytes on both,
  by construction (fixed-width fields plus an explicit `pad`).
- `ENTITY_SLOTS = 48` `[read]` `entity.h:39` — the physical array, one size on both consoles.
  `EntityWorld` embeds `Entity e[48]` directly and lives as a static in `main.c`, so it is
  **outside `world/budget.h`'s 12 MB world budget entirely** — `budget.h` has no entity term.
- Occupancy caps: `ENTITY_CAP_OLD = 24`, `ENTITY_CAP_NEW = 48` `[read]` `entity.h:44-45`,
  chosen by `entityCapFor(bool is_new_3ds)`.
- **`EntityWorld` total ≈ 2,892 bytes** `[read]`, the figure `plan-1.8.14-animals.md:157-158`
  records from an ARM build.

**Monsters add zero new pool storage.** They are `Entity` rows in the array that already
exists, at 60 bytes each, already allocated whether occupied or not. The only new bytes are a
small `MonsterDef` table (§3.1) — on the order of a hundred bytes of `.rodata` — and the art
in §2.6, which is the real cost.

### 2.3 Light and caves: the spawn query is cheap, and is a two-line composition

**Light** `[read]` `source/world/light.h`, `light.c`. Two separate 4-bit channels, sky and
block, packed two-to-a-byte, one `LightColumn` per column: `LIGHT_COL_CELLS = 32768`,
`LIGHT_COL_BYTES = 16384` per channel, statically asserted at `light.h:84`. The read API is
column-local:

```c
uint8_t lightGetSky  (const Column* col, int lx, int y, int lz);   // light.h:134
uint8_t lightGetBlock(const Column* col, int lx, int y, int lz);   // light.h:135
```

There is **no world-space wrapper today** — every shipped caller works inside `light.c` on the
raw channel arrays; the only callers that go through `worldColumn()` are tests. A spawner
composes it itself, in two lines:

```c
Column* col = worldColumn(w, x >> 4, z >> 4);
uint8_t sky = lightGetSky(col, x & 15, y, z & 15);
uint8_t blk = lightGetBlock(col, x & 15, y, z & 15);
```

This is a composition of two public functions, not new machinery. **It is null-safe end to
end** `[read]` `light.c:242-263`: an unloaded column, or a loaded-but-unlit one, reads as 0 on
both channels — dark — and never faults. That default is worth staring at, because **"unloaded
reads as dark" means a naive predicate would happily spawn a monster into an unloaded
column.** The footprint check in §3.2 is what prevents that, not the light check.

Light is computed **eagerly**, a full column flood-fill at generation and again on edit before
the remesh `[read]` `light.h:142-154` — so for a resident column the value is current when
asked. The one transient-staleness window is `relightq`, the coalescing worklist used for bulk
diff replay on a multiplayer rejoin `[read]` `relightq.h:1-11`; a column queued there holds
pre-edit light for a few frames. Worth a comment in the spawner. Not a blocker.

**Caves** `[read]` `source/world/cave_carve.c`/`.h`. v1.8.11 shipped a deterministic worm
carver: one column-footprint region hashes to zero-or-one tunnel system at
`CAVE_REGION_CHANCE = 26` in 256, walked with damped drift and a tapered ellipsoid stamp,
`CAVE_MAX_REACH = 24` blocks, 49 regions scanned per column.

**There is no cave-volume index.** The carve mask lives in `WorldGenScratch`, is built during
generation and discarded; nothing on `Column` or `Chunk` records "this contains cave air"
`[measured]` — a grep for `has_cave|hasCave|cave_flag|caveVolume` returns zero across
`source/`, while the control grep for `worldgenIsCave|caveCacheBuild|GEN_VERSION_CAVES`
returns real hits in ten files, so the search was capable of finding one. **A spawner samples
blocks blind**, via `worldGet()`, exactly as `animal.c`'s surface spawner already does.

The existing shape to copy is `spotIsGood()` (`entity/animal.c:290-302`) — three `worldGet()`
calls: solid ground at `y-1`, air at `y`, air at `y+1`. A cave version drops its
grass/dirt/sand allow-list (any solid works underground) and adds the light test. What it
**cannot** reuse is `worldStandingY()` (`world/world.c:200-214`), which seeds from
`worldgenHeight()` and is surface-only by construction. Underground, `y` has to be picked and
tested directly.

**Verdict on the brief's question: cave-dark spawning is cheap and needs no new machinery.**
Every one of the five classic checks is answerable today:

| Check | Answerable today? | With what |
|---|---|---|
| Solid block below | Yes | `worldGet(w,x,y-1,z)` + `blockIsSolid()` |
| Two air cells above | Yes | two `worldGet()` calls — `spotIsGood()`'s shape |
| Dark enough | Yes, after a two-line composition | `worldColumn()` + `lightGetSky`/`lightGetBlock` |
| Not too near the player | Yes, free | distance-squared against the player position |
| Not too far / not unloaded | Yes | `worldColumn()` returning `NULL` self-rejects |

The cost that matters is not the per-query cost — see §7, where it is a locality problem, not
an arithmetic one.

### 2.4 Combat: three hand-written damage paths, and no fourth

`[read]` `source/world/survival.h`/`.c`, `entity/animal.c`.

What v1.8.13 and v1.8.14 actually left behind:

- Player health is `Survival.health`, `uint8_t`, 0..20, `SURVIVAL_MAX_HEALTH 20`
  (`survival.h:38,53-58`). It is **not** on `Player`.
- **There is no generic damage function.** Fall damage writes `s->health` inline in
  `fallDamageUpdate()` (`survival.c:156`, floors at 0 — can kill); starvation writes it inline
  in `survivalTick()` (`survival.c:93`, floors at 1 — deliberately cannot kill). Animal damage
  is a third, unrelated path writing `Entity.health` in `animalHurt()` (`animal.c:442`). Three
  hand-written paths, no shared entry point. `survival.h`'s public API is
  `survivalInit/Tick/FoodValue/Eat/Save/Load` plus `fallTrackInit`/`fallDamageUpdate` — and
  nothing else `[read]`.
- **No invulnerability frames, no hit cooldown, no knockback** `[measured]` — greps for
  `invuln|iframe|i_frame|hurt_cool|damage_cool|last_hurt` and for `knockback` return zero
  across the survival and player files, while the control grep for `_ticks`/`cooldown` returns
  the real `hunger_ticks`/`regen_ticks` fields, so the search works.
- Death and respawn exist and are instant: `survivalRespawn()` full-heals and resets to world
  spawn, no death screen, and **inventory is kept** — it never touches `Inventory`.
- `SFX_HURT` and `SFX_DEATH` are **reserved slots with no call site** `[read]`
  `audio/audio_sfx.h:56-60`. Free feedback, sitting there.
- Player-hits-animal: `animalRaycast()` (`animal.c:367`), a float ray-vs-AABB slab test that
  deliberately does **not** reuse the block-grid DDA. Reach is `INTERACT_REACH 5.0f`
  (`scene/interact.h:47`), reused verbatim so hitting a mob has the same range as breaking a
  block. Damage is a flat `ANIMAL_FIST_DAMAGE 4` (`animal.h:63`) — the only damage number in
  the game, because there are no tools. No cooldown timer: it is edge-triggered on the break
  key's rising edge, because a level trigger killed an animal in three frames.

**So the missing piece is exactly one thing: damage flowing toward the player.** Nothing in
the tree does it `[measured]` — `playerHurt|playerDamage|hurtPlayer|damagePlayer|attackPlayer|
entityAttack` returns zero hits, against a control grep on `s->health|survivalTick(|
fallDamageUpdate(` that returns thirty-plus. This is the first version in which a thing in the
world hurts the player, and it needs a real entry point rather than a fourth inline write.

**Projectiles do not exist** `[measured]`. No thrown item, no arrow, no snowball, nothing with
collision. The particle system (`gfx/particles.h`) spawns and fades and has no collision
concept at all — and is mid-edit by another lane right now, so read it fresh. The nearest
existing thing to a projectile is `animalRaycast()`: a ray, not a moving object. **A skeleton
arrow would be entirely new work**, which is why §3.5 treats it as the version's one real
fork rather than a detail.

### 2.5 Multiplayer, the protocol, and the server verdict

**Animals are not synchronised. At all.** `[measured]` The complete `BS_APP_*` opcode list in
`deps/blocksmith-server/proto/bs_proto.h:217-331` — BLOCK_EDIT, POS_UPDATE, WORLD_SYNC/INFO,
CHUNK_SUB/DIFFS/UNSUB, INV_STATE/ACTION, PLAYER_STATE/REPORT, REGISTRY_INFO/FETCH/DEFS,
WORLD_GEN — contains no entity record of any kind. A grep of the protocol headers for
`entity|mob|animal|arrow|projectile` returns one hit, and it is prose about item reachability,
not an opcode; the control grep for `BS_APP_[A-Z_]*` returns the twelve above, so the search
works. `entity.c`/`animal.c` include nothing from `source/net/`, and the spawner and tick run
with no session gate. `ENT_F_REMOTE` exists as a seam that nothing sets.

`plan-entities.md` §6.2 made this call deliberately and said what it costs: *"Mobs will look
like a single-player feature that happens to run while multiplayer is on."*

**Three gates, and they are not interchangeable** `[read]`:

1. **`PROTO_COMMIT`** — `Makefile:344`, currently
   `11caee37f8b1bf7ea3e9b0d3534c47b90e48cd8d`. A byte-level pin on the vendored
   `bs_proto.h`, enforced by the `check-proto-drift` target. Bumped only when that file's
   bytes change.
2. **`BS_PROTO_VERSION`** — `bs_proto.h:56`, currently `1u`. Gates transport packet types.
   New `BS_APP_*` opcodes in the free 0x10-0xFF range do **not** require a bump.
3. **`REGISTRY_REV`** — `source/world/registry.h:106`, currently `1u`, with a
   `{rev, count, crc16}` fingerprint checked by `registryMatchesInfo()`. **Soft and
   human-enforced**: a mismatch degrades near-silently (unknown ids render as air) instead of
   refusing the connection. This is the one that gates block-table *content*.

**The block table is 43 rows, verified by counting** `[measured]`: `kCoreDefs[REG_ID_DYN_LO]`
(`registry.c:82`) has exactly 43 designated initialisers, `BLOCK_AIR = 0` through
`[42] = furnace`. `docs/VERSION-LIST.md:794` says the same.

**The server mirror is exactly 11 files, verified by listing** `[measured]`
`deps/blocksmith-server/game/world/`: `block.h`, `crafting.c`, `crafting.h`, `crc32.c`,
`crc32.h`, `inventory.c`, `inventory.h`, `registry.c`, `registry.h`, `tick.c`, `tick.h`.
(The directory also holds `.d`/`.o` build artefacts; those are not mirrored source.) Note
`block.c` is **not** vendored — only its header. There is **no entity, light, cave, chunk or
worldgen code server-side**; `bsgame.c:221` says *"This process has no terrain generator and
never will."*

> **The Grep tool is silently blind to `deps/`.** Every claim in this section was made with
> bash `grep -r` against an explicit path. A lane using the Grep tool here will get a
> confident empty result and conclude the server does not mention something it mentions.

**Verdict, and it turns on one design choice:**

- **Monsters that drop nothing require no server release and no `PROTO_COMMIT` bump.** They
  add no wire record, touch none of the 11 mirrored files, and change no registry row. They
  ship exactly as v1.8.14's animals did — client-only.
- **Monsters that drop items do force a server release.** A drop needs an `ItemId`, `ItemId`
  *is* `BlockId`, and there is no separate item registry — so a drop is a new **core**
  registry row. That moves `registryCrc16()`, which moves the `REGISTRY_REV` fingerprint,
  which is the soft gate that must ship server-first or simultaneously and **cannot be
  enforced by the protocol**. v1.8.1 already hit this once (CRC `0x72A8` → `0x4066`).

That makes §10's "do monsters drop anything" fork a **release-shape decision, not a flavour
decision**, and it should be read that way.

**Is there an entity equivalent of the remote-edit stale-cache bug?** The original is real and
already fixed: a remote block edit updated `World` but never called `chunkRenderTouch()`, so
the cached mesh stayed stale (`CHANGELOG.md [1.2.6]`); a second instance of the same class hit
`visgraphInvalidateTables()` later. **No entity equivalent can exist today**, because there is
no remote entity write at all — the precondition is absent. It becomes real the moment entity
sync ships with a fixed slot pool: any derived per-entity state not carried in the synced
record (a hit cooldown, a hurt flash, a target lock) must key off the stable entity id, never
the reused slot index, or a remote despawn/respawn leaves stale state pointing at the wrong
occupant. That is a forward-looking note for whoever builds §6.3 of `plan-entities.md`, not a
bug in this tree.

### 2.6 Art: the sheet is full, and it is not the block atlas

This is where `plan-1.8.16-monsters.md` and `plan-entities.md` are most wrong, and where the
real cost of this version sits.

**Mob skins do not live in the block atlas.** `[read]` They live on a separate sheet,
`gfx/animals.png`, 128×128 RGBA5551 = **32,768 bytes of VRAM**, laid out as four 64×64
quadrants, one animal per quadrant (`scene/entitymodel.h:57-64`, and the sheet's own file
confirms 128×128). **All four quadrants are occupied. There is zero free space on it.**

The block atlas is a separate question and mostly irrelevant here: 16×1024 RGBA5551, 64 slots
of 16×16, `ATLAS_PAINTED_SLOTS = 57` `[measured]` (`atlas_uv_shader_test.c:228`, cross-checked
by counting 57 entries in `tools/make_atlas.py`'s `TILES` list). Slot 63 is permanently
`ATLAS_TILE_MISSING`. **So there are exactly 6 free block-atlas slots, 57..62**, at 512 bytes
each. Those are 16×16 block-face tiles — the wrong shape for a mob skin. They matter to this
version **only if monsters drop items**, since a drop needs a hotbar icon.

> **Do not trust `tools/make_atlas.py`'s own docstring**, which still says *"TILES holds 17…
> exactly 46 free slots"* `[read]`. That is a stale comment from before v1.8.8 and contradicts
> the live count by forty. Trust `atlas_tiles.h`'s note and `len(TILES)`.

**The generator.** `tools/make_animals.py` writes `gfx/animals.png` `[read]`, and it is a
genuinely good piece of work worth understanding before extending: it does **not** paint in
sheet space. It parses the box table `kEmBoxes` straight out of `source/scene/entitymodel.h`
by regex, inverts the UV unwrap for every texel — sheet pixel → which box, which face, which
point in *model* space — and evaluates colour as a function of that 3D point. That is why a
cow's patches continue across the seam between flank and leg: the leg genuinely is somewhere
else in the same 3D field. One fact, two consumers — the header feeds both the console's
vertex buffer and the PNG, and the script **fails rather than silently painting the old
layout** if the header's shape stops matching. Pillow, per-pixel, fixed seed, no numpy,
self-contained, in the house style every other `tools/make_*.py` uses. It is run by hand;
`make` then picks up the changed PNG via the explicit `animals.t3x: animals.png` rule that
exists to defeat a `.d`-file stem collision.

**What a zombie and a skeleton actually cost:**

- Geometry is free of new concepts. `EM_BOXES_PER_KIND = 6` `[read]`, and a humanoid fits it
  exactly — body, head, two arms, two legs — against the quadruped's body/head/shared-leg-net.
  `EmVertex` stays 20 bytes, `EM_VERTS_PER_KIND` stays 216. Only new table rows and new net
  layouts.
- The sheet must grow, and **it must grow to a power of two.** `[read]`
  `atlas_uv.h:15-21` records the constraint disassembled out of citro3d 1.7.1:
  `C3D_TexInitWithParams` rejects any dimension outside 8..1024 **and any dimension that is
  not a power of two**, the two dimensions checked independently.
  `atlas_uv_shader_test.c:987-988` asserts it for the block atlas in as many words. **A
  128×192 sheet — six quadrants, the obvious minimal growth — is illegal and would be
  rejected at runtime.** The real step is **128×128 → 256×128**: eight quadrants, four
  animals, two monsters, **two spare**, at 256 × 128 × 2 = **65,536 bytes**, i.e.
  **+32,768 bytes of VRAM over today**.
  >
  > **[2026-09-04 correction, ANIMAL-ART]** This subsection originally said the grown sheet
  > is **128×256** — width 128, height 256, i.e. **2 columns × 4 rows** of 64×64 quadrants.
  > That is not what shipped. MON-BUILD's actual layout, read directly out of
  > `source/scene/entitymodel.h:57-73`, is the **transpose**: `#define EM_SHEET_W 256` /
  > `#define EM_SHEET_H 128`, `#define EM_QUADRANT_COLS 4`, with the header's own comment
  > stating it outright — *"256x128 RGBA5551 = 65,536 bytes of VRAM, a 4x2 grid of 64x64
  > quadrants (EM_QUADRANT_COLS wide, 2 tall)"* (`entitymodel.h:57-58`). `entitymodel.h:166-172`
  > confirms the same thing from the quadrant table: `(k % EM_QUADRANT_COLS, k / EM_QUADRANT_COLS)`
  > with `EM_QUADRANT_COLS = 4`, and `tools/make_animals.py` parses `EM_SHEET_W`/`EM_SHEET_H`
  > out of that same header rather than restating them, so the shipped PNG is 256×128 too —
  > `python tools/make_animals.py` reports `256x128 RGBA`. Both orientations are legal powers
  > of two and both total 65,536 bytes, which is why nothing caught the mismatch until it was
  > checked directly against the header. **The code is authoritative here — it is already
  > built and tested (`entitymodel` self-test, `animal_test`) against its own 256×128/4-column
  > layout — so this correction fixes the prose, not the code.** Every other "128×256" in this
  > document (§4's numbers table, §7, §8's risk table, §9 item 3) means the same **256×128,
  > 4×2** layout and should be read that way; they are not re-typed individually.
- Two new `skin_*()` painters in `make_animals.py`. `plan-entities.md:849-852` already
  sketches how a zombie and a skeleton should look in this style, and that sketch survives
  even though its sheet layout does not.

### 2.7 The frame budget, and the number nobody has ever measured

`entityTick()` is called once per simulation tick at `TICK_HZ = 20` `[read]`, from inside the
tick loop in `main.c`. It is distance-decimated: within `TICK_NEAR_BLOCKS = 24` blocks every
tick, beyond that every 10th (2 Hz), staggered by entity id. **Physics is batched, not
scaled** — a decimated entity runs `period` separate fixed-dt `bodyStep()` calls when it comes
due, not one large step, because a scaled dt measured *worse* for resting entities.
`entityTick()`'s cost is linear in occupied slots and indifferent to what occupies them.

Rendering is **one `C3D_DrawArrays` per visible entity** `[read]` `scene/entitymodel.c` —
not instanced, not batched across entities: a translate, a yaw rotate, a uniform upload and a
draw, each. Culling is a horizontal distance-squared reject against the fog boundary, then
`frustumTestAABB`. It is called from inside `drawEye()`, so stereo doubles it for free.

**And here is the thing this version has to be honest about: there is no measured entity cost
anywhere.** `[measured]` A grep across `docs/` and `CHANGELOG.md` for any entity timing
returns exactly two lines, and both are targets rather than results:
`plan-entities.md:938` sets `entityTick <= 2.5 ms` as a **criterion** for a hardware phase,
and `:1020` states plainly that `ENTITY_CAP_NEW = 48` *"is a hypothesis, not a budget."*
There are no entity metrics in `source/debug/metrics.c` at all. `entitymodel.c` carries a
build-time probe harness (`ENTMODEL_PROBE_N`) built precisely to price per-instance draw cost,
and **no numeric result from it is recorded anywhere in this repository.**

So `plan-1.8.14-animals.md:197`'s phrase *"entityTick()'s existing, already-measured cost
model"* is **not supported by anything in this tree**. The cost model is documented and
reasoned; it was never measured. Any lane that reads that line and skips a measurement is
inheriting a claim nobody made.

---

## 3. Design

### 3.1 The kind table, and the field that is already waiting

Monsters get their own table, parallel to `AnimalDef` and in the same shape, for the reason
`animal.c` gives for having a table at all: per-kind rows are less total work than flat
constants plus a later migration.

```c
#define ENT_KIND_ZOMBIE    5
#define ENT_KIND_SKELETON  6
/* 7 = arrow -- reserved; claimed only if §3.5 takes the projectile option */
```

Fixed-width fields, an explicit `pad`, **never an enum** — `-fshort-enums` has already made a
host `sizeof()` lie about a console struct on this project, and `animal.h:85-89` says not to
write that assert. Pin the size and every field offset with `offsetof`, on both ABIs, the way
`animal_test.c:299-315` already does for `AnimalDef`.

`AnimalDef.hostile` already exists and is already zero for every animal. Whether monsters
reuse `AnimalDef` outright (filling in `hostile`) or get a sibling `MonsterDef` is an
implementation call for the lane that builds it — **not a scope call**, and this document
does not force it. What it does force: `animalDef()` must keep returning `NULL` for 5/6/7, or
`animalThink()` will start running wander AI on a zombie. That pin is criterion A.4.

### 3.2 The spawn rule

A repeating timer, not `animal.c`'s once-per-column-load, because darkness changes and a
column load does not repeat.

Every 40 ticks (2 s), pick a small number of candidate positions **clustered near the player,
not scattered across the loaded ring** — see §7, this is a cache decision, not a gameplay one
— and for each, in this order, cheapest and most-rejecting first:

1. Distance from the player inside a min/max band. Free arithmetic, rejects most candidates
   before any memory is touched.
2. `worldColumn()` non-`NULL`. **This check is load-bearing and must come before the light
   test**, because an unloaded column reads as light 0 on both channels (§2.3) — dark — and a
   predicate that trusted darkness alone would spawn into unloaded space.
3. Footprint: solid at `y-1`, air at `y` and `y+1` (`spotIsGood()`'s shape, without the
   grass/dirt/sand allow-list).
4. Darkness: `lightGetSky == 0 && lightGetBlock == 0`.
5. Monster sub-cap not already reached.

**Only step 4 is new logic.** Everything else is a call that exists.

**A divergence worth stating, not fixing.** `[research-given]` real Java's hostile check is
looser — `skyLight <= 7 && blockLight == 0`, plus a randomised roll against the light level —
so hostiles appear in dim-but-not-black places some of the time. The strict
both-channels-zero rule is `plan-entities.md`'s existing design and this document keeps it,
because in a cave it makes a single torch produce a clean, legible, immediate result, which
is the whole point of the feature. Recorded as a divergence, not corrected.

**Surface night spawning is not blocked any more.** `source/world/daynight.c`/`.h`/`_test.c`
exist and the clock is wired in — `plan-1.8.16-monsters.md`'s own dated correction verified
`dayNightAdvance()` running live. But surface spawning is **out of this version** anyway, for
a different reason: see §6.

### 3.3 Despawn: the torch check

On the 2 Hz decimated tick, a monster standing in light despawns. This is the mechanic that
makes a torch mean something, and it is the first real exercise of a check animals never
needed. It is one light query per monster per half-second — at a cap of 8 monsters, sixteen
queries a second, against a 20 Hz tick. Negligible.

`entityKill()` already exists and is deferred-and-reaped, so there is nothing to build but the
predicate.

### 3.4 Zombie: chase and melee

A CHASE state entered when the player is within a detection radius, leaving it when the player
is beyond it. Movement is the existing "heading plus obstacle reaction" — set a yaw toward the
player, let `bodyStep()` do the rest, and reuse `animalThink()`'s stuck check (did
`bodyMove()` zero horizontal velocity?) to pick a new heading at a wall.

**There is no pathfinding in this codebase and this version does not add any.** A zombie will
walk into a wall and slide along it. It will not go around a pillar. In a cave corridor —
which is where this feature lives — that reads as menacing rather than stupid, which is a real
part of why "in caves" is the right first scope.

Contact damage on a cooldown when within a small distance. That distance check is arithmetic
on two positions; no raycast, no collision query.

### 3.5 Skeleton: the one genuine fork in this version

A skeleton needs a ranged attack, and **nothing ranged exists** (§2.4). There are two ways to
build it and they are not close in cost. This is §10's headline decision.

**Option A — hitscan with a telegraph.** `plan-entities.md` §8 recommends this. The skeleton
stops, plays a visible wind-up for ~20 ticks, then applies damage directly if the player is
still in range and in line of sight. No projectile, no new entity kind, no new mesh, no new
collision. The tint machinery for the telegraph is nearly free — primary colour is already
modulated in the TexEnv stage. **The telegraph is what makes it dodgeable**, which is the part
a player actually experiences; an arrow they cannot see coming is worse than a wind-up they
can.

**Option B — a real arrow entity.** `ENT_KIND_ARROW = 7` is already reserved.
`plan-1.8.16-monsters.md` argues it costs no new fields: spawn via `entitySpawn()` with a
small box, velocity toward the player, `ai_timer` as a TTL, gravity and block collision for
free via the embedded `Body`, despawn on timer or on `bodyStep()` reporting it stopped. That
argument is **correct about the entity layer and incomplete about everything else.** An arrow
still needs: a seventh box-model kind and a seventh sheet quadrant (§2.6 — the sheet grows to
256×128 either way, so this consumes one of the two spares), a hit test against the player
each tick, and — the part nobody has costed — **it occupies a slot in the same 24-slot pool**,
so a fight adds transient entities exactly when the pool is most contested.

**Recommendation: Option A for v1.8.18.** It is smaller, it is more dodgeable, it does not
contest the pool during a fight, and it does not commit kind 7 — which stays reserved for a
real arrow later, when a player-usable bow gives one a second reason to exist. **But this is
the owner's call**, because it is the difference between a skeleton that shoots and a
skeleton that zaps, and that is a feel question, not a cost question. §10.1.

### 3.6 Damage to the player: the new entry point

This is the first time in the project's history that something in the world hurts the player,
and it should be built as an entry point, not a fourth inline write.

Add to `source/world/survival.h`/`.c`:

```c
// Returns true if this damage JUST killed the player.
bool survivalDamage(Survival* s, uint8_t amount);
```

Floors at 0 and can kill, matching fall damage rather than starvation — being eaten by a
zombie should be able to finish you. It is the same three lines `fallDamageUpdate()` already
contains at `survival.c:151-158`; **the value is that there is one of them.** Whether
`fallDamageUpdate()` should then be refactored to call it is a tidy-up this document does
**not** propose — that file is `plan-1.8.13-survival.md`'s and touching it is scope this
version was not given.

`survival.h` includes no `<3ds.h>` and must stay that way, so this stays host-testable, which
is criterion A.6.

**Invulnerability frames are the honest open question here** (§10.2). With no i-frames and a
20 Hz tick, a zombie in contact damages on its own attack cooldown, and two zombies have two
independent cooldowns. Whether that is "cornered by two zombies is genuinely dangerous" or
"you died instantly and never saw why" is a playtest answer, and the cheap insurance is a
short per-player hit cooldown rather than per-monster.

`SFX_HURT` and `SFX_DEATH` are reserved and uncalled (§2.4) — wiring them here is close to
free and is the difference between damage the player notices and damage they only see in the
health bar.

### 3.7 The think dispatcher

`entityTick()` takes **one** `EntityThinkFn` and today gets `animalThink`. The smallest honest
change is a dispatcher at the call site that switches on kind range and calls `animalThink`
or `monsterThink`. That is one function and one changed argument in `main.c`.

**Do not** widen `animalThink()` to cover monsters — that would mean `animalDef()` must return
rows for 5/6/7, which breaks the pinned test that is currently the only thing enforcing the
kind reservation.

---

## 4. Numbers, and where each one came from

| Value | Number | Provenance |
|---|---|---|
| Spawn check interval | every 40 ticks (2 s) | `[read]` `plan-entities.md`'s existing monster rule. Kept. |
| Darkness test | `skyLight == 0 && blockLight == 0` | `[read]` same. Diverges from real Java's `skyLight <= 7` + roll — see §3.2, kept deliberately. |
| Light-despawn check rate | 2 Hz, on the existing decimated tick | `[read]` same. Costs one light query per monster per half-second. |
| Kind values | `ZOMBIE = 5`, `SKELETON = 6`, `ARROW = 7` reserved | `[read]` — `animal.h:47` already reserves exactly these three, enforced by a test. Not a new assignment; an existing one being claimed. |
| Health | 20 both kinds | `[research-given]` exact match — zombie and skeleton are both 20 hp in Java. Also a clean fit to the 0..20 scale `Entity.health` and the protocol already use `[read]`. |
| Player fist damage vs. a monster | 4, unchanged | `[read]` `ANIMAL_FIST_DAMAGE 4`. At 20 hp that is five hits — noticeably tougher than a three-hit cow, which is the right relationship. No new number needed. |
| Zombie melee damage | **3** hp per hit | `[research-given]` Java Normal is 3 (2 Easy, 4-5 Hard). `plan-1.8.16-monsters.md` proposed 2 and its own citation pass admitted the 2 had *no derivation behind it*. Taking the researched figure rather than inheriting an unjustified guess. |
| Zombie attack cooldown | ~20 ticks (1 s) | Mine. Not covered by the research as a fixed figure. A feel number. |
| Skeleton ranged damage | **4** hp per shot | `[research-given]` Java Normal bow damage is ~3.5-5 depending on draw and distance; 4 is the midpoint. Same reasoning as the row above — the old plan's flat 2 was undefended. Flat, not distance-scaled, per §Legacy feel. |
| Skeleton fire rate | every 60 ticks (3 s) | `[research-given]` exact match — the one skeleton figure the research confirms outright. |
| Detection / chase radius | 16 blocks | `[read]` `plan-entities.md` §8's existing figure. Deliberately far below the `[research-given]` real Java ~35 blocks: 16 is inside `TICK_NEAR_BLOCKS = 24`, so a chasing monster is always on the full-rate tick and never chases at 2 Hz. That is a real reason, not a rounding. |
| Monster sub-cap | 8 Old / 16 New | Mine, derived: `ENTITY_CAP_OLD 24 − ANIMAL_CAP_OLD 16 = 8`, and `48 − 32 = 16`. These are exactly the slots `animal.h:70-73` already holds back. Naming them makes the reservation symmetric and assertable. |
| AI states | IDLE, WANDER, CHASE; no FLEE | `[reasoned]` — `[research-given]` confirms neither classic zombie nor skeleton flees. |
| Monster drops | none — **his call**, §10.3 | Proposal. This is a release-shape decision, not flavour: a drop forces a registry row and a server release (§2.5). |
| Entity sheet after growth | 256×128 RGBA5551 = 65,536 B | `[read]` + `[reasoned]` — 8 quadrants (4 cols × 2 rows) at the existing 64×64 convention, from a measured 128×128/32,768 B today. **Power of two is mandatory**, so 128×192 is not an option. See §2.6's 2026-09-04 correction: this row originally said 128×256, transposed from what shipped. |
| Free block-atlas slots | **6** (57..62); 63 permanently reserved | `[measured]` `ATLAS_PAINTED_SLOTS = 57`, `ATLAS_TILE_SLOTS = 64`, `ATLAS_TILE_MISSING = 63`. Relevant only if monsters drop items. |

---

## 5. Build order

Each phase has one outcome, a stated check, and a **red arm** — the deliberate breakage that
must turn the check red. A check that could not have failed proves nothing, and this project
has already shipped a suite that stayed green with the module it tested deleted. Follow
`tools/run_host_tests.sh`'s existing form, including its md5-verified restore after each
sabotage, and remember that **the exit code is the only gate** (`run_host_tests.sh:5437`).

Anything new under `source/` is globbed into the console build by `Makefile:26`, so a new
`*_test.c` there needs the `#ifndef __3DS__` guard `animal_test.c` already carries. And
because `Makefile` has no header dependencies, **`make clean` between arms** — a header-only
edit will otherwise leave `make` reporting "up to date".

**Phase 1 — `survivalDamage()`, alone, with nothing hostile in the world.**
Outcome: a single generic entry point exists for damage to the player, host-tested.
Check: a host test drives `health` 20 → 0 through it and asserts the just-died return fires
exactly once. Red arm: floor it at 1 like starvation — the kill assertion must go red.
This phase touches no entity code at all and is independently shippable. It is also the
cheapest possible proof that the version's genuinely new mechanic works.

**Phase 2 — the monster kind table and the think dispatcher, with no behaviour in it.**
Outcome: kinds 5 and 6 exist, spawn on demand, tick through a `monsterThink()` that does
nothing, and are ignored by every animal path.
Check: `animal_test.c`'s existing pin that `animalDef(5|6|7) == NULL` still passes
**unchanged**; a new host test spawns a monster and confirms `animalCount()` does not see it
and `animalThink()` does not drive it. Red arm: add a row to `kDefs` for kind 5 — the pin must
go red. This is the phase that proves the two families are actually separate.

**Phase 3 — the spawn rule, headless, stated as a rate.**
Outcome: monsters appear only in dark, open, loaded, near-the-player space.
Check: **at least 500 candidate positions across varied seeds** — 100% of accepted spawns dark
on both channels and standing on solid with two air above; 0% accepted in surface daylight;
0% in a `NULL` column. Red arm: **invert the light comparison, and separately, remove the
`worldColumn() != NULL` check.** The first must make the daylight figure go red; the second
must make the unloaded-column figure go red. **The report must name percentages, not "a
failure"** — a rate is the claim, so a rate is the evidence.

**Phase 4 — zombie chase and melee, measured on hardware.**
Outcome: a zombie pursues and hurts the player.
Check, host: a synthetic zombie adjacent to a synthetic player drives health to 0 through
Phase 1's entry point, on its cooldown and not once per tick. Check, hardware: **read the
frame-time counter on a real Old 3DS in a cave at the monster sub-cap with the 3D slider up**
— see §7 for why this is the phase that must not be skipped. Red arm for the damage test:
remove the cooldown; the "not once per tick" assertion must go red. Red arm for the counter
itself: raise the cap far past 8 and confirm the number moves — **a counter that cannot move
is not an instrument.**

**Phase 5 — the skeleton, in whichever shape §10.1 chooses.**
Outcome: a ranged attacker the player can see coming and get away from.
Check, host: the range/line-of-sight predicate accepts and rejects the right geometry.
Check, playtest: **can the owner actually dodge it.** That is the criterion; no host test
substitutes. Red arm: remove the range bound and confirm the host check notices a shot that
should have been out of range.
If Option B (a real arrow) is chosen instead, this phase gains the arrow kind, its quadrant
and its per-tick hit test, and Phase 6's sheet grows by one more quadrant — say so in the
handoff rather than absorbing it.

**Phase 6 — art: the sheet grows, and the texture probe comes first.**
Outcome: a zombie and a skeleton that read as themselves at 16 texels to the block.
Check: **render the grown sheet at UV 0..1 on a full-screen quad and look at it before
trusting a single model UV.** Then each skin on the right kind, right way up, no bleed at
quadrant edges. Red arm: offset the UV scale by one quadrant and confirm the check notices —
a visual check that passes against a deliberately wrong UV is not a check.
`entitymodel.c`'s static assertions are the second, free check here: they must be updated
deliberately, and if they can be left alone then something is wrong.

**Phase 7 — the torch despawn.**
Outcome: placing a torch clears a cave, within a second.
Check: host test that a lit position despawns a standing monster on the next 2 Hz tick;
playtest that it feels immediate. Red arm: invert the despawn predicate — monsters must then
despawn in darkness, and the test must say so.
Kept last deliberately: it is the payoff mechanic, it is the cheapest phase, and putting it
last means every phase before it has been playable on its own.

**Stop at each phase boundary and hand the build over.** Phase 4 and Phase 7 in particular are
the two the owner has to feel rather than read.

---

## 6. What is explicitly out of v1.8.18, and why

1. **Surface night spawning.** The day/night clock exists and is wired in, so this is *not*
   blocked — it is descoped. The ask was "in caves"; caves are a bounded, dark-by-definition,
   always-testable space, and surface spawning multiplies the searched volume by the whole
   loaded ring at exactly the moment §7 says locality is the thing to protect. It is a small
   addition later once the cave version has been measured on hardware.
2. **Zombies burning in sunlight.** `[research-given]` real and characteristic, and flagged by
   `plan-1.8.16-monsters.md` as a genuine gap rather than a considered omission. It is out
   because item 1 is out — it needs the same surface-and-daylight signal.
3. **Monster drops.** Proposed out, and the reason is structural, not flavour: a drop is a core
   registry row, which moves the registry CRC, which forces a server release (§2.5). **His
   call**, §10.3.
4. **Entity multiplayer sync.** Inherited from `plan-entities.md` §6.2 and not reopened. Worth
   restating in this version's terms because monsters are where it stings: **an unsynced
   zombie is a zombie only one player can see or be hurt by**, so two players in the same cave
   fight their own private monsters and neither can help the other. That is the most visible
   version of a decision already on record. §10.5.
5. **Entity persistence.** Same inheritance. Quit and reload and the cave repopulates.
   Materially *less* strange for monsters than it was for animals — nobody expects a specific
   zombie to still be there.
6. **Difficulty selection.** `plan-1.8.13-survival.md` already decided on one ruleset; every
   number in §4 is the Normal-tier figure. Not re-decided here. §10.4 asks only whether that
   still holds now that something can actually kill you.
7. **Pathfinding, kiting, and strafing.** `plan-entities.md` says there is none; there is
   none. `[research-given]` real skeletons maintain a firing distance and retreat when closed
   on. Not built. A skeleton stops advancing and attacks from where it stands.
8. **A player-usable bow.** No ranged item exists, no per-item damage table exists, and none
   was asked for.
9. **Refactoring fall damage and starvation onto `survivalDamage()`.** Tidy, tempting, and out
   of scope — that file belongs to another version's plan.

---

## 7. Cost and risk on an Old 3DS, which is the machine that binds

The frame is **16.71 ms** on both consoles `[read]` `source/debug/metrics.h:108`. The Old 3DS
is a 268 MHz ARM11 with **no L2 cache**. Everything below is about that machine; a New 3DS at
804 MHz with L2 is not the constraint and is not what this section is for.

**What is genuinely cheap:**

- The monster tick. `entityTick()`'s cost is linear in occupied slots and indifferent to kind
  `[read]`. Monsters occupy slots the pool already allocated, inside sub-caps
  (`ANIMAL_CAP_OLD 16` + 8 monsters = 24 = `ENTITY_CAP_OLD`) that were reserved for this
  version before it was written. **Total occupancy does not rise.**
- The torch-despawn check: one light query per monster per half-second.
- The chase logic: a yaw toward the player and the existing `bodyStep()`. No raycast.
- The melee hit test: arithmetic on two positions.
- Memory. `sizeof(Entity) = 60` bytes on ARM, in an array of 48 that is already allocated,
  outside the 12 MB world budget. A `MonsterDef` table is ~100 bytes of `.rodata`. **The RAM
  cost of this version is approximately zero.**

**What actually costs something:**

1. **VRAM: +32,768 bytes**, growing the entity sheet 128×128 → 256×128 (§2.6). Small against
   6 MB, but note `plan-entities.md` §11.2's standing gap — **nobody has ever summed total
   VRAM in use** across the block atlas, crack atlas, font, fog ramp, entity sheet and render
   targets. VRAM exhaustion on this hardware surfaces as a texture silently rendering as
   garbage, never as an error. Sum it before Phase 6 ships.
2. **Draw calls.** One `C3D_DrawArrays` per visible entity, doubled by stereo `[read]`. This
   version does not raise the entity cap, so the worst case is unchanged from what animals
   already permit — but nobody has measured *that* worst case either (below), and a cave is a
   place where several monsters are plausibly in frame at once.
3. **Spawn-query locality — the one genuinely new per-second cost, and the one to design
   around.** One candidate check is a `worldColumn()` hash probe, three `worldGet()` calls,
   and two nibble reads into a 16,384-byte light array. Each is cheap in isolation, and
   `chunkGet()` is often free outright — a `CHUNK_FORM_UNIFORM` chunk returns a struct field
   with no array access at all, and half of every 128-tall column is sky `[read]`
   `chunk.h:6`. **The risk is not arithmetic, it is cache.** Scattering random `(x,y,z)`
   candidates across the loaded ring means consecutive candidates touch different offsets in
   different 16 KB light arrays, with no spatial locality and, on a part with no L2, a full
   main-memory round trip per miss. Nothing in the current API amortises this: there is no
   batch or prefetch form of the light or block getters.
   **Mitigation, and it costs nothing: cluster the candidates.** Several tries within the same
   column and chunk, in a small radius around the player, rather than N independent random
   picks across the ring. This converts most of the cost into warm re-hits of arrays already
   resident — the same locality argument the light engine's own column-local design leans on.
   It is a design constraint on the spawner, and it should be written into the code as a
   comment explaining *why*, not just done.

**The honest gap, stated plainly.** No number in this section can be converted to milliseconds
in advance, because **no entity timing has ever been measured on this project** (§2.7). The
2.5 ms figure in `plan-entities.md:938` is a target for a phase that was never run; the
`ENTMODEL_PROBE_N` harness exists and its result was never recorded;
`plan-1.8.14-animals.md:197`'s "already-measured cost model" is not supported by anything in
this tree. The `ROADMAP.md:181` claim that the main thread is *"GPU-blocked for roughly 15.7
of every 16.71 ms"* — which would mean CPU work is nearly free — is itself flagged at
`metrics.h:132` as **measured in Azahar, which has no GPU cost model**, and therefore as the
number that decides whether every CPU optimisation is visible or invisible.

**So Phase 4's hardware check is not a formality, it is the only real budget evidence this
feature will ever have.** If it misses, the fix is to lower the monster sub-cap before doing
anything clever — the same instruction `plan-entities.md` gives for `ENTITY_CAP_OLD`.

---

## 8. Risks, and the cheapest mitigation for each

| Risk | Cheapest mitigation |
|---|---|
| An unloaded column reads as light 0 on both channels (`light.c:242-263`), so a darkness-first predicate spawns monsters into unloaded space — and the symptom is invisible, because the monster is somewhere the player cannot see. | Order the checks so `worldColumn() != NULL` runs **before** the light test (§3.2), and give Phase 3 a dedicated red arm that removes exactly that check. Stated as a rate, so a small leak shows up instead of hiding behind a passing sample. |
| The spawner scatters random candidates across the loaded ring and quietly costs milliseconds on an Old 3DS through cache misses into the 16 KB light arrays. | Cluster candidates within one column/chunk (§7). Costs nothing, needs no new storage, and must be written down as a reason in the code — otherwise a later tidy-up "simplifies" it back into a random scatter. |
| `entitymodel.c`'s box table and the kind enum drift apart, and a zombie renders as a sheep or as nothing. | Do nothing — the `_Static_assert`s already there are the trip-wire, and criterion A.5 requires them to still tie the two together afterwards. If a phase can leave them untouched, that is the signal something is wrong. |
| `animalDef()` is widened to cover kinds 5-7 as the "obvious" way to share the table, and `animalThink()` silently starts running wander AI on a zombie. | The existing pinned test (`animal_test.c:254-256`) is the guard, and criterion A.4 requires it to pass **unchanged**. Use a dispatcher (§3.7), never a widened animal table. |
| The entity sheet is grown to 128×192 — the obvious minimal step — and `C3D_TexInitWithParams` rejects it at runtime for not being a power of two. | Grow to 256×128 (§2.6). The constraint is recorded at `atlas_uv.h:15-21`, disassembled out of citro3d; it is not folklore. Two spare quadrants are a side benefit, not the reason. |
| The texture lands wrong and reads as bad art rather than as an error — the PICA200 failure mode this project has already paid for. | Phase 6's probe: render the sheet at UV 0..1 and look at it **before** trusting any model UV, with a one-quadrant offset as the red arm. |
| Monster drops are added late, "while we're in here", and the registry CRC moves without a server release — which the protocol cannot enforce, so a mismatched pair connects happily and degrades near-invisibly. | Keep drops out of the build until §10.3 is answered. If the answer is yes, drops become their own phase with the server release shipped first or simultaneously, exactly as v1.8.1 and v1.8.15 already required. |
| No i-frames plus two zombies in a corridor equals an instant death the player never understands. | A short per-**player** hit cooldown (not per-monster) is a handful of lines and can be added in Phase 4 if the playtest says so. Flagged as §10.2 rather than pre-emptively built. |
| Phase 4's hardware measurement is skipped because the host suites are green and the animals plan says the cost model is "already measured". | It is not (§2.7). The measurement has never been taken. Phase 4 names the counter, the console, the slider position and the red arm precisely so this cannot be waved through. |
| A new `*_test.c` under `source/` is globbed into the console build by `Makefile:26` and breaks the CIA build, or is written and wired into no suite and never runs. | The `#ifndef __3DS__` guard `animal_test.c` already carries, plus an explicit stanza in `run_host_tests.sh` with a pinned check count updated in **both** places (`:5443-5451`). Watch the count move. |

---

## 9. LINES SOMEONE ELSE MUST ADD

1. **`source/world/survival.h` / `.c`** — `survivalDamage()` (§3.6). This file belongs to
   `plan-1.8.13-survival.md`'s scope. This version needs the function to exist and does not
   propose touching anything else in it — specifically **not** refactoring `fallDamageUpdate()`
   or `survivalTick()` onto it.
2. **`source/main.c`** — the think dispatcher argument at the `entityTick()` call, the monster
   spawn timer alongside the existing per-tick work, and the `survivalDamage()` call site.
   `main.c` is under concurrent edit by several lanes right now; **re-derive every line number
   at implementation time.** Two research passes during this document's own preparation
   already reported the `entityTick()` call at two different lines, hours apart.
3. **`source/scene/entitymodel.h` / `.c`** — two new `kEmBoxes` rows for a six-box biped, the
   `EM_KINDS` bump, the sheet-height bump, and the static assertions updated deliberately.
4. **`tools/make_animals.py`** — `skin_zombie()` and `skin_skeleton()`, plus their quadrant
   assignments. The script parses the box table out of `entitymodel.h` and will fail rather
   than mispaint if the header moves without it, which is the behaviour to preserve.
   `plan-entities.md:849-852` sketches both looks; that sketch is still usable even though its
   sheet layout is superseded.
5. **`gfx/animals.t3s`** — unchanged in flags (`-f rgba5551 -z auto`); only the PNG behind it
   grows.
6. **`tools/run_host_tests.sh`** — an explicit stanza per new suite, with the check count
   pinned there and in the suite itself.
7. **Only if §10.3 says monsters drop things:** `source/world/block.h` and
   `source/world/registry.c` need new core rows following `registry.c`'s own five-place
   checklist, plus block-atlas tiles from the **6** free slots (57..62), plus a matching
   server release. **Do not assume id == atlas slot** — `block.h:217` says they are not
   aligned, and `plan-1.8.14-animals.md` already got this wrong once.

---

## 10. HIS CALL

1. **Skeleton: hitscan-with-telegraph, or a real arrow entity?** (§3.5.) I recommend the
   telegraph for this version — smaller, more dodgeable, does not contest the 24-slot pool
   during a fight, and leaves `ENT_KIND_ARROW = 7` reserved for when a player bow gives an
   arrow a second reason to exist. The arrow is the more faithful thing and costs a quadrant,
   a hit test and pool pressure. **This is a feel decision, so it is yours, not a cost
   decision I should settle.**
2. **Invulnerability frames.** There are none today, for anything (§2.4). With a 20 Hz tick
   and no i-frames, two zombies in a corridor deal damage on two independent cooldowns.
   Cheapest insurance is a short per-player hit cooldown. I lean toward adding it in Phase 4
   *if the playtest says so* rather than pre-emptively — but if you would rather never
   experience the version without it, say so and it goes in from the start.
3. **Do monsters drop anything?** Proposed: no. **This is a release-shape question, not
   flavour** (§2.5) — a drop needs a core registry row, which moves the registry CRC, which
   forces a server release that the protocol cannot enforce. `[research-given]` real drops are
   rotten flesh (zombie) and bone plus arrows (skeleton); none of those items exist here, and
   skeleton-dropped arrows are only useful once a bow exists, which is not this version.
   Saying no keeps v1.8.18 client-only.
4. **Does one flat difficulty still hold now that something can kill you?**
   `plan-1.8.13-survival.md` decided one ruleset when nothing could hurt the player. Every
   number in §4 is the Normal-tier figure. I am not proposing to reopen it — but this is the
   version where that decision first has consequences, so it is worth a yes rather than an
   assumption.
5. **Unsynced monsters in multiplayer.** Already decided (`plan-entities.md` §6.2) and not
   reopened here, but restated because monsters are where it is most visible: **two players in
   the same dark cave fight their own private monsters, and neither can help the other with
   one they cannot see.** Worth confirming that is still acceptable when it is zombies rather
   than pigs. The cheapest acknowledgement, if it is, is a one-line debug-overlay note rather
   than building sync.
6. **The monster sub-cap of 8 on an Old 3DS.** Derived from the slots `animal.h` already holds
   back, not from a measurement. If Phase 4's hardware check comes back tight, the correct
   response is to lower this number — and I would rather you knew in advance that is the
   intended lever than have it look like a retreat.

---

## 11. What I could not determine, and what each gap means

1. **Any real entity timing, on any console.** No `entityTick()` measurement, no
   `entitymodel.c` probe result, no entity metrics in `source/debug/metrics.c` — nothing
   `[measured]`, against a control search that finds the target lines fine. **Consequence:**
   every cost claim in §7 is reasoned, and Phase 4's hardware check is the version's only
   real budget evidence. Do not let a green host suite stand in for it.
2. **Total VRAM in use.** `plan-entities.md` §11.2 flagged this and it is still open. The
   entity sheet's +32 KB is very probably fine against 6 MB — but "probably" is not a
   measurement, and the failure mode is a silently garbled texture, not an error.
3. **Whether the Old 3DS main thread has CPU headroom at all.** `ROADMAP.md:181` says
   GPU-blocked 15.7 of 16.71 ms; `metrics.h:132` says that was measured in an emulator with no
   GPU cost model. **Consequence:** whether a monster's CPU work is free or contested is
   genuinely unknown, in both directions.
4. **The exact `main.c` insertion points.** Several lanes are editing it concurrently and two
   research passes hours apart reported the `entityTick()` call at different lines.
   **Consequence:** re-derive them. Stale `file:line` reasoning has produced at least one
   wrong conclusion on this project already, and the previous monsters plan is the
   demonstration.
5. **Whether `AnimalDef` should be reused with `hostile` filled in, or a sibling `MonsterDef`
   added.** Both are defensible; the choice needs the implementing lane to have `animal.c`'s
   internals in front of it. **Consequence:** none to scope — it is an implementation detail
   either way, called out so nobody thinks this document silently decided it.
6. **How a six-box humanoid's nets actually pack into a 64×64 quadrant.** The box count fits
   `[reasoned]`, and `make_animals.py` re-derives packing from the header and fails rather
   than mispainting. **Consequence:** if a biped's nets do not fit, the script will say so at
   Phase 6 rather than producing a wrong sheet — but the fallback (larger quadrants, hence a
   larger sheet again) is not costed here.

---

## 12. What in `plan-1.8.16-monsters.md` has gone stale

Kept as a list rather than a deletion, because the *pattern* of how it rotted is the useful
part: almost every error is a planning document citing another planning document as though it
were code.

1. **Its version number and slot.** It is v1.8.18 now, not v1.8.16 — and v1.8.16 shipped as
   something else entirely.
2. **"The entity store… is not yet instantiated or called from `main.c`" and the zero-match
   greps behind it.** False, and the file carries its own dated correction saying so. Also
   false in the same way: the day/night clock is wired in. **A plan that asserts absence from
   a grep is only as good as the day the grep was run.**
3. **`ENT_KIND_ZOMBIE = 5, SKELETON = 6, ARROW = 7` described as "mine — nothing above 4 is
   claimed yet."** Those three values are now **reserved in shipped code and enforced by a
   test** (`animal.h:47`, `animal_test.c:254-256`). It is no longer a proposal to make; it is
   an existing reservation to honour.
4. **"The shared 24/48-slot cap… is now genuinely contested" as an unmitigated risk.** Partly
   overtaken: `ANIMAL_CAP_OLD 16` / `ANIMAL_CAP_NEW 32` shipped specifically to hold back a
   third of the pool for this version, statically asserted. The residual decision is §10.6,
   which is much smaller than what that document described.
5. **`plan-entities.md` §7.4's "the atlas has 64 slots and 17 are painted, so 47 are free."**
   Now **57 painted, 6 free** (57..62, with 63 permanently reserved). Off by forty. The stale
   docstring at the top of `tools/make_atlas.py` says the same wrong thing and is also still
   there.
6. **The entity art layout it inherited — `gfx/entatlas.png`, 64×256, six 64×32 regions "with
   two spare", zombie and skeleton painters already budgeted for.** None of that shipped. The
   real sheet is `gfx/animals.png`, 128×128, four 64×64 quadrants, **all four occupied, zero
   spare.** A lane trusting the design doc would arrive expecting reserved headroom that does
   not exist. This is the single most consequential staleness in the set, because it is the
   difference between "add a painter" and "grow a texture".
7. **Its melee and ranged damage numbers, 2 and 2.** Its own citation pass admitted these had
   no derivation and that the researched Normal figures are ~3 and ~4. §4 takes the researched
   numbers rather than inheriting an undefended guess.
8. **"No audio subsystem is confirmed to exist in this codebase."** False since v1.8.8 —
   `source/audio/` has a real ndsp backend, mixer, positional panning and a `.bsnd`
   container, and `SFX_HURT`/`SFX_DEATH` are **reserved slots waiting for a call site**
   (`audio_sfx.h:56-60`). `docs/VERSION-LIST.md`'s v1.8.19 entry carries the same correction
   for the same reason: a grep for the low-level primitive returns nothing because gameplay
   calls the wrappers one layer up.
9. **Its arrow design's "zero new fields, no change to `entity.h`/`.c`" cost claim.** Correct
   about the entity layer and incomplete elsewhere — it does not account for the model
   quadrant, the sheet growth, or the pool pressure during a fight (§3.5).
10. **`survivalDamage()` described as something that document "proposes adding."** Still
    needed, still absent — but now verifiable rather than assumed: `survival.h`'s public API
    is six functions and none of them is a damage entry point (§2.4).
11. **Its "surface spawning depends on the day/night wiring landing" hedge.** The dependency
    landed. Surface spawning is out of this version for a scope reason instead (§6.1) — a
    different answer to a question that is no longer blocked.

---

## Sources

Every `[research-given]` tag restates the outside research pass cited by
`docs/plan-1.8.13-survival.md`, `plan-1.8.14-animals.md` and `plan-1.8.16-monsters.md`:
minecraft.wiki, covering zombie and skeleton health, melee and ranged damage by difficulty,
skeleton fire rate and kiting behaviour, movement-speed attributes, aggro range, drop tables,
the real hostile-mob light spawn test, and zombie daytime burning. All figures are Java
Edition, Normal difficulty. **That research was supplied to those documents by their
coordinating agent and was not re-fetched or re-verified during this pass** — it is carried
forward with its original attribution, and where this document takes a researched number over
an inherited guess (§4's zombie and skeleton damage rows) it says so explicitly.

Everything tagged `[read]`, `[measured]` or `[reasoned]` came from this tree during this pass,
with the file named at the point of claim. Where a number contradicts an earlier planning
document, §12 says which and why.
