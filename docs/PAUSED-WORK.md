# Paused work — resume here after the quota reset

**Created 2026-09-04 11:20. Reason: steve hit 93% quota and ordered all feature work
paused so the remaining budget goes to the v1.8.17 boot freeze and to shipping a
functional build.**

Nothing in this file is cancelled. Everything here was deliberately stopped mid-flight
and is meant to be finished. Each entry says where the work is, what state it stopped
in, and what the next concrete step is, so that resuming does not mean re-deriving.

Order below is the recommended resume order.

---

## 1. Chests and storage — target v1.9.0

**Status: paused mid-landing. Partially lost and reconstructed — read this whole entry
before touching it.**

**Where the work is.**
- `source/world/chest.h.paused-v1.9.0` (6,728 bytes) — the design and interface. Intact.
  This is the bulk of the thinking and it survived.
- `source/world/chest.c.paused-v1.9.0` (884 bytes) — barely started. Intact.
- `chest-paused-v1.9.0.patch.txt`, at `C:\Users\steve\Documents\3ds-project-folder\` —
  the integration wiring, reconstructed from the lane's transcript.

Both `.paused-v1.9.0` files are renamed **only** so the Makefile's `source/` glob does
not compile them. Rename them back (dropping the suffix) to resume.

**What happened, plainly.** The integration wiring was reverted out of the working tree
by mistake on 2026-09-04 at ~11:16, one message before steve said to pause rather than
undo. It was uncommitted, so git held no copy. **Eight** files were affected:

- `source/world/block.h` — the `BLOCK_CHEST = 43` id, its static assert, `BTEX_CHEST_TOP`
- `source/world/registry.c` — the chest registry row
- `source/gfx/atlas_tiles.h` — `TILE_CHEST_TOP = 57` and the painted-slot count changes
- `source/world/block_tiles_check.c` — the `X(BTEX_CHEST_TOP, TILE_CHEST_TOP)` row
- `tools/make_atlas.py` — the chest tile painter (~90 lines)
- `gfx/atlas.png` — the painted tile itself (regenerable from `make_atlas.py`)
- `source/world/registry_test.c` — 67 lines, including the hardness-40 assertion
- `source/world/atlas_uv_shader_test.c` — 29 lines

The last two were reverted separately, at ~11:23, once the build showed they still
referenced `BLOCK_CHEST` after the id was gone. Every one of the eight was verified
chest-only before reverting — each had zero added lines that did not mention chest — so
no other lane's work was caught up in it.

The patch file above is the reconstruction. **Treat it as a reconstruction, not as the
original** — re-read it against the current tree before applying, and expect the count
pins to have moved.

**What state it stopped in.** Two things were still broken when it was paused:
1. `source/world/block_tiles_check.c:98` — `BTEX_CHEST_TOP` and `TILE_CHEST_TOP` held
   different values, failing the static assert and taking the whole host suite dark
   (the suite runs under `set -e`, so the first failing stanza hides every later one).
2. Two pre-existing, already-committed tests carried stale count pins that a 44th block
   row invalidates: `tests/food_placeable_invariant_test.c` (`rows == 43`, found 44) and
   `source/world/mining_test.c` (`targetable_rows == 41`, found 42). A third,
   `tests/inventory_test.c` around line 463, was also reported failing on the dirty tree
   but was never attributed — check it, do not assume.

**The hard constraint that makes this expensive.** A chest is a new `BlockId` (43). That
moves the registry CRC, and `source/world/block.h` and `source/world/registry.c` are
byte-mirrored into `deps/blocksmith-server/game/world/` behind the `check-world-drift`
target, which is wired into both `all:` and `test:`. So landing a chest **forces a
coordinated client *and* server release** — a server branch, tag, published Release, and
a CT 105 update. There is no client-only path. This is the single reason it was deferred
rather than finished.

It also claims **atlas slot 57**, leaving 58–62 free (slot 63 is reserved). See entry 8.

**Next three steps on resume.**
1. Rename both `.paused-v1.9.0` files back and re-apply the patch file.
2. Make `BTEX_CHEST_TOP` and `TILE_CHEST_TOP` numerically equal, then fix the three test
   count pins, then run the full host suite and read the real exit code.
3. Mirror `block.h` and `registry.c` into `deps/blocksmith-server/game/world/`, confirm
   `check-world-drift` passes, and plan the paired server release.

---

## 2. The v1.8.17 boot freeze — NOT paused, this is the active priority

Kept running deliberately. This is the only lane still working. See `docs/VERSION-LIST.md`
and the vault log for the full investigation; the short version is that eighteen
hypothesis classes are eliminated with real measurement and none was it.

The decisive missing artifact is the **10,592-byte GPU command list at `0x14289c00`**
that the console is wedged on. Nobody has ever read it. v1.8.18 ships an unconditional
dump of it to `sdmc:/blocksmith/cmdhang.bin`, with an offline decoder at
`tools/decode_cmdhang.py`, so the next freeze produces the evidence.

---

## 3. Multiplayer sync audit

**Status: DONE 2026-09-05.** The audit was re-run and its findings are recorded here so they
cannot be lost a second time. Nothing was applied to the tree — this entry is the deliverable.

The protocol in `deps/blocksmith-server/proto/bs_proto.h` carries block edits, player pose,
world sync and seed, per-column diff streaming, inventory, player state (health, hunger,
armour, XP) and the block registry. The client implements **all nine inbound types and every
outbound one** — nothing in the wire format is unimplemented client-side. The gaps are all
state that the protocol has no message for at all.

Unsynced, worst first:

1. **Mobs — the worst by a distance.** No message type carries entities. `monsterSpawnTick()`
   (`source/entity/monster.c:379`) rolls spawns from `(tick, player_x, player_y)` locally, so
   two players standing together see different monsters, or one sees none. Killing one does
   nothing to the other player's copy. Fixing it needs a server-authoritative entity system —
   `deps/blocksmith-server/game/bsgame.c` has no entity simulation at all today, so this is a
   new server subsystem, not a new opcode. Largest of the three by far.
2. **Time of day.** Two independent local clocks. One player sees noon while the other sees
   dusk. **The fix is already specified**: `source/world/daynight.h:338-365` names the exact
   missing opcode — `BS_APP_TIME_SYNC` (0x10), an 8-byte tick counter, S→C only, sent at join
   and about once a second — and `dayNightSet()` already exists as the client entry point, so
   the client change is one decoder case. The server must own and broadcast an authoritative
   tick. Additive and S→C only, so an old client simply ignores it.
3. **Weather.** Rides on time-of-day for free: `source/world/weather.h:31-47` says `weatherAt`
   is already a pure function of `(seed, tick, x, z)`, so once the tick is shared there is no
   separate payload to send. Today it drifts — snow piling on a hillside the other player sees
   bare. **No extra server work beyond item 2.**
4. **Furnace state.** A remotely-placed furnace's block syncs but its blockstate record (fuel,
   progress, lit) is never created, so the other player cannot use it. Already reproduced by a
   host test whose own comment calls it "THE DEFECT" (`source/net/networld_test.c:1717-1794`).
   **This one is a client-only bug fix** in `net/networld.c`'s block-edit apply path — no new
   message, no protocol change, no coordinated release.
5. Water flow — self-correcting, since the block edits that cause it do sync. Low priority.
6. Break-progress crack overlay — cosmetic, and arguably should not sync.

Lighting is not a gap: it is recomputed client-side from already-synced block data. Item drops
are not a gap either — no ground-item entity exists, pickups go straight to the inventory.

**Recommended order when this is picked up: 4 (client-only bug), then 2+3 (one server change
buys both), then 1 (its own project).**

---

## 4. Tool tiers — target v1.10.0

**Status: designed and costed, deliberately not built.** No code was written. The full
design is recoverable from the ORE-PAYOFF lane's report; the load-bearing facts:

- There are genuinely no tools today. `source/world/mining.h:24-36` says so outright, and
  `registry.c:561` confirms every ore is breakable by hand. This is a documented
  deferral, not an oversight.
- The call shape is already in place: `interact.c:429` already passes `it->holding` into
  `breakTicksRequired()`, and `miningSpeedMultiplier(ItemId holding)` already takes the
  held item, with a comment marking where the tool rows go.
- `ItemId` **is** `BlockId` — there is no separate item registry. So each pickaxe needs a
  brand-new block id, which means editing `block.h` and `registry.c`, which forces the
  same coordinated server release as chests.
- A minimal three-tier design (wood/stone/iron) costs **3 of the 6 remaining atlas
  slots** — half the entire remaining texture budget for the project.

**Open question for steve, not for me:** whether that atlas spend is acceptable, given
entry 8.

---

## 5. Coal as furnace fuel

**Status: one-line change, blocked on a design call that is steve's.**

`source/world/furnace.c:96-115`'s `furnaceIsFuel()` accepts only planks and logs. Adding
`case BLOCK_COAL_ORE:` needs **zero new block ids** and therefore **no server release** —
it is genuinely cheap, unlike everything else on this list.

The reason it was not just done: `furnace.h:142-150` explains the current state as
deliberate, and the real question is whether raw coal *ore* should burn, or whether a
distinct "coal" item should exist first (which is the Minecraft-faithful answer and does
need a new id). That is a gameplay-design fork, so it needs steve.

Note also that coal ore is **not** useless today — `RECIPE_COAL_ORE_TO_TORCH` turns
1 coal ore into 4 torches, verified end to end (12 checks, exit 0).

---

## 6. Documentation corrections found but not yet applied

**Status: ALL THREE FIXED 2026-09-05.** Kept below with what each turned out to be, because
two of them were worse or differently shaped than this entry recorded, and the third could not
be settled by reading at all.

**1 — fixed.** The v1.7.1 paragraph now says a fifth item, region compaction, was claimed there
when the entry was written but was not wired until v1.8.2, and is credited to that release.

**2 — fixed, and it was worse than recorded here.** The contradiction was not only between the
doc and the source: `source/scene/chunk_render.c`'s own prose disagreed with its own measurement
table four lines above it, and `VERSION-LIST.md` printed 0.0089 in one sentence and 0.0051 two
lines later. **It could not be resolved by reading, because both numbers were plausible and the
harness that produced either had been thrown away.** So it was re-measured: both sorts lifted
out again (the insertion sort recovered from commit `f8c4dc22`, which removed it) into a new
`tools/sortbench.c`, gcc -O2, three runs. The worst-case new arm measured 0.0062 / 0.0065 /
0.0061 ms against an old arm of 0.2825 / 0.2756 / 0.2781 — nowhere near 0.0089. The table was
right, the prose was wrong, and both copies now read 0.0051 ms / 50.9×. `tools/sortbench.c` is
left in the tree so this figure cannot be lost a third time.

**3 — fixed.** The ROADMAP v1.8.18 entry now says spawning gates on darkness alone and explains
that caves fill with monsters because caves are dark, not because a cave test exists.

The `.text +792 bytes` sub-claim noted below remains unverified — it needs an ARM build with
`arm-none-eabi-size` run against both arms, which was not done.

---

The original record of the three, as found:

1. **`docs/VERSION-LIST.md`, v1.7.1 section (~lines 231-233)** — the "Changed" paragraph
   says region compaction was "one of the four" measured performance fixes and was later
   found unwired. That inverts the history. v1.7.1's CHANGELOG entry correctly listed
   region compaction in a *separate* section titled "Measured and not fixed", which said
   plainly it was not wired and that this was deliberate. It should read as a fifth,
   separate item, not folded into the four. The substance is right; the framing is wrong.
2. **`docs/VERSION-LIST.md`, v1.8.12 section** — claims the radix sort's worst case is
   "29x (0.2583→0.0089ms)". The source's own benchmark table three lines above that prose
   (`source/scene/chunk_render.c:3071`) says `reverse (worst case) 0.2583 ms → 0.0051 ms,
   50.9x`. Both were introduced in the same commit. The full n-by-n sweep quoted in the
   doc has **no backing artifact anywhere in the repo** — the probe that produced it lived
   in a scratchpad and was never committed. The `.text +792 bytes` sub-claim is likewise
   unverifiable without an ARM build.
3. **`docs/ROADMAP.md` (~lines 434-437)** — the v1.8.18 entry still says zombies and
   skeletons spawn "in caves". No `worldgenIsCave()` gate exists. VERSION-LIST already
   records this paragraph as superseded; ROADMAP itself was never corrected.

Everything else audited across v0.1.0–v1.9.3 checked out — roughly 180 substantive claims
verified, with only these three plus the already-corrected v1.8.11 lava/ravines/water
entries found wrong.

---

## 7. libcurl → mbedTLS updater rewrite

**Status: deferred by decision, with a measured payoff.** Would save **337,079 bytes** of
the shipped image; libcurl + mbedTLS is currently 47.3% of it. Held back until the
`arena` and `free` numbers the updater now prints are read off steve's actual console,
because the updater's "could not reach GitHub / SSL CA" failure is **curl error 77, a
local CA load/parse failure**, not a certificate rejection by GitHub — and the rewrite
should be aimed at the real cause, not at a guess.

**Needs from steve:** a photograph of the updater's error screen, which now prints those
two numbers.

---

## 8. The atlas slot ceiling — a scheduling conflict, not a bug

Only **six** block-atlas slots remain: 57–62. Slot 63 is reserved
(`source/gfx/atlas_uv.h:48-49`, `source/gfx/atlas_tiles.h:157-158`).

Three separate pieces of planned work each want a share of those six:
- chests (entry 1) want 1
- tool tiers (entry 4) want 3
- `docs/plan-1.9.2-redstone.md` already earmarks **all 6**
- `docs/plan-1.9.3-dimensions.md` adds more on top

Together the redstone and dimensions plans alone ask for **15 tiles against a ceiling of
6**. This is a real conflict between committed plans and it will have to be resolved by a
decision — either a larger atlas, a second sheet, or cutting scope. **That decision is
steve's and has not been made.** Nothing should spend those slots until it is.

**[2026-09-05] The three options are now costed, so the decision can be made on numbers.**
The first thing to say is that "just make the sheet bigger" is **not available**:

- The atlas is a **16 × 1024** one-tile-wide strip, RGBA5551, 32,768 bytes of VRAM, 64 slots
  (`source/world/atlas_uv.h:48-50`, `tools/make_atlas.py:113-120`).
- **`ATLAS_H_PX` is already 1024, which `atlas_uv.h:15-21` documents as the PICA200's hard
  per-dimension maximum** (citro3d's `checkTexSize`: each dimension 8..1024 and a power of
  two). The dimension that determines slot count is at the ceiling and cannot go up.
- The width *could* grow, but it is load-bearing, not spare. It is exactly one tile so that
  `GPU_REPEAT` in U has a period of exactly one tile, which is what lets the greedy mesher
  merge N co-planar faces into a single quad and still have the tile repeat across it
  (`atlas_uv.h:23-31`). Widening to two or more columns makes a merged quad sample into the
  neighbouring column instead of repeating — every merged face past column 1 corrupts. That
  is a mesher and shader redesign, not a constant.

The two real options:

1. **A second atlas on the spare texture unit.** The PICA200 has three; unit 0 is the atlas,
   unit 1 the fog ramp, unit 2 is bound by weather and water shimmer in *separate* passes, so
   it is free during the opaque terrain pass. Four of six TEV stages are also free. Cost: up
   to +32 KiB VRAM (less if the second sheet is sized to what is actually needed — ~10 KiB for
   about 20 tiles), an extra draw call per chunk, and real surgery in `mesher.c` (bucket faces
   by sheet), `chunk_render.c`, both world vertex shaders, and the tile-id headers. Spends the
   last free texture unit and two of the last four TEV stages — permanently.
2. **Halve `TILE_PX` from 16 to 8.** Doubles capacity to 128 slots inside the same strip and
   keeps the REPEAT invariant intact, so it is structurally the cheapest by a long way, and
   costs zero extra VRAM. The price is that **all 57 existing tiles must be redrawn at half
   resolution** in `tools/make_atlas.py`, with a real risk they read as mush on a 400×240 top
   screen where 16 px is already small. `make_atlas.py` is properly parameterised, so the
   script does not need rewriting — the art does.

Not a third option but worth stating: cutting scope costs nothing and is reversible, which
neither of the above is.
