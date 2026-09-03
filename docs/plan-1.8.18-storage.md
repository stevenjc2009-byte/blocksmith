> **STATUS: SECOND OPINION, NOT THE CANONICAL SPEC.** The canonical v1.8.18 plan is
> [`plan-1.8.18-storage-qol.md`](plan-1.8.18-storage-qol.md) (47,601 bytes), which is older,
> broader, and already committed. This document was written by a later lane that was dispatched
> without being told the other one existed -- my error in briefing it, not the lane's. It is
> kept rather than deleted because it independently verified three things the canonical spec
> gets wrong or could not know, listed in that file's dated correction. Read the canonical spec
> first; read this one for those three corrections and for its six-question structure.
>
> Where the two disagree and this file cites a `file:line`, prefer this file -- it was written
> against a later tree. Where they disagree on design, neither is settled; both are proposals.

# v1.8.18 — Storage: six questions, answered from the tree

Written by lane STORE-A as a planning-only pass: no file under `source/`, `tests/`, or `tools/`
was touched to produce this, and this document does not itself implement anything. Every claim
about existing code below carries a `file:line` citation to something actually read in this
tree; anywhere that was not possible is marked `UNVERIFIED:` with what would be needed to check
it. Where a grep for something's absence is the evidence, the same grep is shown returning real
hits elsewhere first — an empty grep alone is "not found yet," not proof of absence.

**Provenance tags used throughout:** `[read]` — quoted or paraphrased from a file this pass
opened directly. `[reasoned]` — a conclusion derived from two or more `[read]` facts, shown with
the arithmetic. `[my proposal]` — a number or design choice this document is introducing, not
found anywhere in the tree. A bare number with no tag is this document's own proposal, same
convention `docs/plan-1.8.16-monsters.md` uses.

---

## 0. A file already exists at a neighbouring name — read before anything below

`docs/plan-1.8.18-storage-qol.md` (756 lines) already exists in this tree, dated with a
correction entry timestamped `2026-09-03` — today. It is a complete, thorough, well-cited plan
for the identical feature (chest + QoL), independently researched, in the same house style. This
document was written **after** discovering that file, not before, and reuses none of its prose —
every citation below was pulled from the source tree directly by this pass. Two things follow:

1. **Where this document's own answers agree with `-storage-qol.md`'s**, that is convergent
   evidence from two independent reads, not copying — noted inline.
2. **Where they disagree**, both readings are given with citations, and the disagreement is
   flagged rather than silently resolved, because picking a winner between two independently-
   researched plans for the same version is not this lane's call to make alone.

**One correction to `-storage-qol.md` this pass found, worth surfacing immediately because it
changes a load-bearing assumption in that document's §2.6/§3.4/§8:** `-storage-qol.md` states the
multiplayer wire item ceiling "is still pinned at 8" (`inventory.h:101-137`, its own citation) and
builds its entire multiplayer-chest-sync argument on that gap being open. Reading the same file
directly, first-hand, for this pass:

`source/world/inventory.h:145-186` — the comment block describing the pinned-at-8 wire ceiling is
followed by a **dated divider**, easy to miss if the comment is read only partway (exactly the
"a comment block's history reads as current" trap):

> `── 2026-09-02, v1.8.10: BOTH HALVES HAVE NOW SHIPPED ──` ... *"Everything above this line
> describes the OLD behaviour and is kept as the record of why the gap existed and what it cost.
> The gap is closed; the predicate below delegates to the bag's own ceiling, so the two can no
> longer drift."*

And the actual current predicate, `inventory.h:183-186`:

```c
static inline bool inventoryItemOnWire(ItemId item)
{
    return inventoryCanHold(item);
}
```

`inventory.h:158-167` states the server side was independently verified by reading
`deps/blocksmith-server`'s own `bsgame.c:1339` and `game/validate.c:146`, not just trusted from a
release note. **v1.8.10 (2026-09-02) closed this gap, one day before this pass's run date
(2026-09-03).** `-storage-qol.md`'s §2.6/§3.4/§8-item-1 were written against the pre-v1.8.10
state and need the same kind of dated `⚠ CORRECTION` that document already gives its own
`RECIPE_COUNT` claim — not made here, since editing that file is out of this lane's scope, but
flagged for whoever reconciles the two documents (§7 below).

---

## 1. Can a chest's contents live in BlockStateTable's 16-byte payload?

**Yes, for exactly 8 slots, with zero slack and zero format change** — but §2 below argues against
actually doing it that way.

- `source/world/blockstate.h:155` — `#define BLOCKSTATE_PAYLOAD_BYTES 16` `[read]`.
- `source/world/blockstate.h:146-154` — the constant's own comment states it was sized for the
  furnace only and explicitly invites widening for "a future kind that needs more than this...
  which is not a reason to over-provision today" `[read]` — an open door, not a closed one.
- `source/world/inventory.h:234-237` — `typedef struct { ItemId item; uint8_t count; } InvSlot;`
  `[read]`, the in-memory shape.
- `source/world/inventory.c:260-263` — the actual **wire** encoding, not `sizeof()` (this
  project's standing anti-`sizeof()`-for-wire-format discipline, since the ARM EABI
  `-fshort-enums` build already made a host `sizeof()` lie once, `world/chunk.h`'s `Chunk`):
  ```c
  buf[20 + i * 2 + 0] = inv->slots[i].item;
  buf[20 + i * 2 + 1] = inv->slots[i].count;
  ```
  Exactly **2 bytes per slot** on disk `[read]`.
- `[reasoned]` 16 bytes ÷ 2 bytes/slot = **8 slots exactly**, no reserved byte left over — unlike
  the furnace's own packing (`source/world/furnace.h:74-90`, `FURNACE_PAYLOAD_USED_BYTES 11`,
  5 bytes spare `[read]`), an 8-slot chest would use all 16 bytes with nothing held back for a
  later field (a "last opened by" byte, a lock flag, anything).

**Why not to actually do this, despite it technically fitting:** widening `BLOCKSTATE_PAYLOAD_BYTES`
past 16 later (e.g. to reach a rounder slot count) changes every record's on-disk byte size.
`source/world/blockstate.c`'s save/load path checks the read byte count against the expected
record size computed from the *current* `BLOCKSTATE_PAYLOAD_BYTES`; a mismatch degrades the load
to an **empty table**, silently discarding every existing record — furnace state included, not
only chests — unless a version bump and migration are written at the same time. Locking a chest
into the *same* payload furnace already lives in means any future chest field growth carries that
risk for furnace saves too, which a chest-only design should not be able to do.

**`-storage-qol.md`'s independent answer agrees with the "don't" half**: it does not use
BlockStateTable at all, proposing instead a dedicated `ChestStore` sized and versioned
independently (`-storage-qol.md` §3.2). This document reaches the same place by a different
route — §2 below, reasoned from BlockStateTable's own capacity ceiling rather than from the
payload-width risk above.

**Recommendation:** an 8-slot chest fits BlockStateTable's existing payload today with no format
change — but do not use BlockStateTable for chests. Give chests their own side table (§2). This is
a design fork worth a line in §7's HIS CALL section, since "reuse the existing table" is a real,
cheaper-to-build alternative someone could reasonably prefer.

---

## 2. Is the 64-slot BlockStateTable capacity enough for chests?

**No — recommend a dedicated table instead of raising `BLOCKSTATE_SLOTS`.**

- `source/world/blockstate.h:144` — `#define BLOCKSTATE_SLOTS 64` `[read]`.
- `source/world/blockstate.c:56-82`, specifically line 75 — the exact failure mode:
  ```c
  if (!e) return false;   // table full, and nothing existing here to reclaim
  ```
  `blockStateCreate()` returns `false` on a full table when the position is new `[read]`. There
  is no eviction — a refusal, matching the same "refuse, never evict" idiom
  `net/blockdiff.h:153-163` already documents for its own store `[read]`, and matching
  `interact.c`'s existing `it->refused++` counter idiom for "the action did nothing."
- `[reasoned]` 64 slots is currently shared by furnace alone. A storage room is a genre
  convention — a player who builds one plausibly wants dozens of containers in the same loaded
  area, not one or two. 64 slots shared between every furnace **and** every chest in render
  distance is a low ceiling for that use case; the 65th chest (or 65th furnace+chest combined)
  placed anywhere loaded simply refuses to place, silently to the player (no toast/status-line
  system exists anywhere in this codebase — confirmed by its total absence from
  `interact.c`/`inventory.h`'s own comments, which repeatedly note the lack when describing a
  refusal).
- `blockstate.h:138-143` — the save format stores a live slot **count**, not the compiled
  constant, so raising `BLOCKSTATE_SLOTS` is cheap and does not break old saves `[read]` — raising
  it is a legitimate option, not a version-format risk the way §1's payload-width change is.

**Why recommend a separate table over just raising the constant anyway:** raising
`BLOCKSTATE_SLOTS` still means chests and furnaces (and any future stateful block) compete for
one shared pool forever — a storage-heavy player's chest room can starve out a build's furnaces,
or vice versa, and the failure is invisible (no toast system, confirmed above). A dedicated chest
table, sized and refused independently, means a chest-heavy build only ever competes with itself.

**First-hand verification of the precedent both this document and `-storage-qol.md` point at**
(`net/blockdiff.c`'s store, read directly for this pass rather than trusted secondhand):
`net/blockdiff.h:113-120` — `BlockDiffEntry` is exactly 16 bytes (`x,z:i32, y:u8, id:BlockId,
_pad:u16, next:u32`) `[read]`; `blockdiff.h:122-138` — chained-hash bucket array keyed by
coordinate, so a lookup only walks one column's chain, not the whole table `[read]`;
`blockdiff.h:153-163` — refusal, never eviction, on overflow, counted (`blockdiffRefusals()`)
`[read]`; `blockdiff.h:79-83,98` — 65536 entries cost 1.02 MB total, as a static **separate from**
`world/budget.h`'s 12 MB world-streaming budget `[read]`. This is a real, shipped, load-bearing
pattern in this exact codebase for "a bounded, coordinate-keyed side table that must not silently
drop or grow forever" — the right shape to copy for a chest table, sized far smaller than
blockdiff's 65536 (a chest entry is far heavier per-slot than one block id).

**Recommendation:** build a dedicated `ChestStore` (name TBD by whoever implements it), same
shape as `blockdiff.c`'s store, sized independently of `BLOCKSTATE_SLOTS`. Do not raise
`BLOCKSTATE_SLOTS` for this. The exact cap (`-storage-qol.md` proposes 1024 loaded chests,
≈46 KB) is itself unmeasured against real play and belongs in §7's HIS CALL alongside §1's fork,
since it is the same underlying "how much do we trust chests will proliferate" judgment call.

---

## 3. How does the player open a chest?

There is genuinely no "use/open" verb anywhere in this codebase today, and both face buttons are
taken:

- `source/scene/interact.h:174-175` — `#define INTERACT_KEY_BREAK KEY_X` /
  `#define INTERACT_KEY_PLACE KEY_Y` `[read]`.
- `source/scene/interact.h:172-173` — comment: the camera already owns A (boost/jump),
  the D-pad, L/R, and START, "which leaves the two right-hand face buttons free" — i.e. already
  fully allocated to break/place `[read]`.
- `source/scene/ui.h:59-62` — `typedef enum { UI_SCR_HUD, UI_SCR_INVENTORY } UiScreen;` — exactly
  two fixed screens, no generic dialog/container-screen concept exists `[read]`.
- `source/scene/ui.h:85-88` — `UiInput` carries only `touch_down, touch_x, touch_y`, deliberately
  no `keys_down`, by the file's own stated design choice `[read]` — a chest screen driven by face
  buttons would be new plumbing, not a reuse of the existing UI input path.

**Option A — a new bindable action on `KEY_ZL` (this document's lean).** `KEY_ZL` is a real,
unbound key in this build's own action table (`app/options.h`'s `OptionsAction` enum and its
default-binding comment, gathered in this lane's earlier research pass before this file was
written — not re-verified against a current line number in this pass, so treat the exact
line numbers as **UNVERIFIED: re-read `app/options.h`'s `OptionsAction` enum and its
default-key comment to confirm `KEY_ZL` is still unbound before building on this**). The existing
`ACTION_EAT`/`KEY_ZR` precedent already accepts a New-3DS-only binding as a shipped trade-off, so
`KEY_ZL` for open/close would not be a novel kind of limitation, only a repeat of one already
accepted.

**Option B — reuse `KEY_B` to open (and close) a chest, keeping B's meaning as "leave the
current context."** Verified first-hand for this pass, with a red control:
`grep -n "KEY_B\b" source/` returns real hits in seven files —
`app/debugmenu_ui.c`, `app/debugmenu_test.c`, `app/remap_ui.c`, `scene/pausemenu.c`,
`scene/title.c` (six times), `scene/title_nav.h` — proving the grep pattern is not simply blind.
**None of those hits are in `scene/interact.c` or `source/main.c`** — the two files that make up
the live 3D gameplay loop and its input dispatch. `KEY_B` is genuinely unclaimed there today.
This matches `-storage-qol.md` §4.2's independent proposal to reuse B the same way, and this
pass's own grep confirms the absence claim first-hand rather than trusting that document's word
for it.

**Option C — a contextual touchscreen button drawn only when aiming at a chest.**
`source/scene/ui_layout.h`'s vitals-strip comment identifies the HUD screen's only free pixel
band (`y≈172..240`, the space below `HUD_STATUS_BOTTOM`/`HUD_PIPS_Y0`) `[read from this lane's
earlier pass; re-confirm exact constants before implementing]`. This needs new plumbing (the
current aim target has to reach the HUD draw call) but requires no new button binding at all.

**Recommendation: Option B (reuse `KEY_B`)** — it needs no new binding, costs nothing on Old
3DS (unlike Option A's New-3DS-only cost), and is corroborated by two independent reads finding
the same key genuinely free in the gameplay loop. Whether reusing a "back" button to mean "open"
reads as natural rather than surprising is explicitly a playtest question, not a code question —
flagged for that, not decided here.

---

## 4. What happens to a chest's contents when the block is broken?

**Confirmed absence, with a red control, and the exact hook point identified:**

`source/scene/interact.c:220-307` is `breakComplete()`, the function that completes a break.
`worldSet(w, x, y, z, BLOCK_AIR)` at line 230 removes the block from the world `[read]`.
Lines 285-296:
```c
// Where it happened, for a caller that has to clean up side-table state keyed by
// position (v1.8.15, the furnace).
it->broke_valid = true;
it->broke_x = x;
it->broke_y = y;
it->broke_z = z;
```
This is the comment's **own stated purpose**: a caller (main.c) is meant to use
`it->broke_valid`/`broke_x/y/z` to clean up side-table state on break `[read]`. But:

`grep -n "blockState\|blockstate" source/` (case-sensitive on the capital-S form too) returns
exactly 5 files: `world/furnace_test.c`, `world/furnace.h`, `world/blockstate.c`,
`world/blockstate_test.c`, `world/blockstate.h` — proving the pattern finds real hits and is not
blind. **Zero hits in `scene/interact.c` or `source/main.c`.** Since the same grep, same scope,
returns real matches elsewhere, the absence here is genuine, not an artifact of a bad search: **no
break-cleanup hook exists yet for any stateful block, furnace included.** The comment at
`interact.c:285-288` describes intent that was never wired up on the caller side.

**What this means for a chest:** the hook does not exist to extend — it has to be built from
scratch, and building it for furnace's own (currently invisible) side-table leak is arguably a
prerequisite this version inherits, not a chest-specific task. Whoever wires it must add, in
`main.c` (locked to this lane), a check on `it->broke_valid` after `interactEdit()` returns, that
looks up and clears any blockstate/ChestStore entry at `(broke_x, broke_y, broke_z)` for the
broken block's id.

**What should happen to the contents themselves — a real design fork, not decided here.** This
codebase has **no item-entity/drop system at all**: `interact.c:272-276`'s own comment confirms a
broken block's drop goes straight into the breaking player's bag via `main.c` checking
`it->broke_id != BLOCK_AIR` — there is no "spawn a pickup in the world" concept anywhere to reuse.
So "drop the chest's contents on the ground" is not a small addition, it would be introducing an
entirely new subsystem. The two realistic options:

1. **Dump the chest's contents into the breaking player's bag**, best-effort (whatever doesn't
   fit is lost, or the break is refused if the bag has no room) — consistent with how every other
   block's own drop already works in this codebase (straight to bag, no world entity), so it needs
   no new subsystem, only extending the existing "what goes in the bag after a break" path to
   also drain a second source (the chest's slots) alongside the block's own drop.
2. **Refuse to break a non-empty chest at all** (require it be emptied first) — simpler to build,
   but a real usability cost, and no other block in this game currently refuses a break for any
   reason like this (breaks are refused only for network/`sendEditOrRevert` reasons, never for
   "you'd lose something").

**Recommendation: option 1** (dump to bag on break), for consistency with the existing
single-inventory-destination pattern and because "you can always empty it first" is not obviously
true — a hostile mob or fall could interrupt a player before they think to. This is named in §7
as a HIS CALL item since a bag-overflow-on-break edge case (what happens to the excess) is a real
UX judgment, not an implementation detail.

---

## 5. Cheapest, highest-value quality-of-life touches — ranked from what's actually missing

Read: `source/scene/interact.c`, `source/scene/ui.c`, `source/world/inventory.h`,
`source/scene/interact.h`, and the grep results already gathered above for §3/§4.

Concrete, code-derived gaps (not a generic Minecraft feature list):

1. **No toast / status-line for a refused action, anywhere.** Referenced repeatedly, as an
   absence, in `interact.c` and `inventory.h`'s own comments while describing refusal counters
   (`it->refused`, `blockdiffRefusals()`) that nothing on screen surfaces. **Highest value, lowest
   cost of anything on this list**: a single-line, timed HUD string (`fontDrawf`, already used
   elsewhere per `-storage-qol.md`'s own §2.10 citation of `source/gfx/font.h`) that main.c sets
   whenever any of the existing refusal counters increments this frame. No new subsystem — every
   refusal event this would surface already exists and is already counted; the only new work is
   drawing the count's already-known cause as text for ~2 seconds. This directly benefits a chest
   too: a full ChestStore refusal (§2) or a bag-overflow-on-break (§4) both become visible instead
   of silently dropped the moment this exists.
2. **No "use/open" verb existed before this version needed one (§3).** Once built for the chest,
   it is free infrastructure for anything else that wants the same trigger later (a furnace
   opened by walking up to it instead of however it is wired today — `furnace.h`/`furnace.c` are
   locked to another lane this session, so this is a note for later, not a claim this version
   should also open furnaces this way).
3. **Hotbar has no keyboard/button cycling**, touch-only today (`-storage-qol.md`'s independent
   §6.1 finding, not re-verified first-hand by this pass — cite that document's `ui.c:363`
   line if implementing, or re-grep to confirm before relying on it). Real value, but the
   button budget conflict that document identifies (L/R already claimed for live render-distance
   stepping) makes this more expensive than it looks — not a "cheap" item despite feeling small.

**Ranked, cheapest-first:** (1) refusal toast — build this regardless of what else in v1.8.18
ships, it is nearly free and makes every other refusal-based system in this plan (chest capacity,
break-to-bag overflow) honest to the player instead of silent. (2) the open/close verb, because
the chest needs it anyway (§3) and it is reusable. (3) hotbar cycling — real value, but only after
the L/R conflict is deliberately resolved (a HIS CALL item, not a coding task).

---

## 6. What must `tools/run_host_tests.sh` gate, and what would go red on a regression?

- `Makefile:26` — `SOURCES := source source/app source/audio source/entity source/gfx
  source/debug source/net source/scene source/shaders source/world deps/libhydrogen` `[read]` —
  confirms any new `source/**/*_test.c` file is swept into the **console** build by this glob
  unless guarded `#ifndef __3DS__`. Files under `tests/` (a directory this glob does not list) are
  exempt from that requirement. Any new chest test file under `source/world/` or `source/scene/`
  needs the guard; a file placed under `tests/` instead does not.
- `tools/run_host_tests.sh`'s `blockstate_test.c` stanza (read via targeted offset in this lane's
  earlier pass, lines ~1536-1577) is the standing cautionary precedent: that test was written
  with 191 checks and **never wired into this script**, so it ran zero times as part of the gate
  until someone noticed the gap. The fix pattern that stanza's neighbours establish: add the new
  test's gcc invocation to this script **in the same session** the test is written, and pin the
  expected check count in the script's own assertion so a future silent shrink (a check quietly
  deleted, not just never added) is also caught, not only "never wired" the way blockstate_test.c
  was.
- `tools/run_host_tests.sh`'s `craft_torch_e2e_test.c` stanza (read at lines ~5098-5166) is the
  standing precedent for the failure mode a chest needs its own version of: five individually
  green unit suites (recipe table, block id, registry row, placement) all passing while the full
  player-reachable chain was actually unreachable. The fix there was one dedicated end-to-end
  binary linking the real `interact.c` + `inventory.c` + `crafting.c` together, not five separate
  mocks.

**Recommendation, concretely:**

1. A unit test for whatever pack/unpack functions the chosen storage design needs (§1/§2) —
   round-trip a slot array through the wire format and back, byte-identical, the same shape
   `furnace.h:96-106`'s `furnaceStatePack`/`furnaceStateUnpack` already establish for its own
   payload.
2. **An end-to-end test, mirroring `craft_torch_e2e_test.c`'s shape**, that proves the whole
   player-reachable chain in one binary: place chest → open (§3's mechanism) → store an item →
   simulate a save/reload (or unload/reload) → item still present, byte-identical → break the
   chest (§4) → contents land wherever §4's chosen option says they should. This is the test that
   would have caught `interact.c`'s own dead `it->broke_valid` hook (§4) if it had existed for
   furnace already — a strong argument for writing it even before deciding every other detail,
   since it is what turns "the pieces work" into "the feature works."
3. Both wired into `tools/run_host_tests.sh` in the same commit that adds the test files, with an
   explicit pinned check count in the script, per the `blockstate_test.c` lesson above — not
   deferred to "wire it in later."
4. Any test file under `source/` gets `#ifndef __3DS__` per `Makefile:26`'s glob; a test under
   `tests/` does not need it. State which directory was actually chosen when the test is written,
   since this document cannot pick that in advance of the storage design itself being settled.

---

## 7. HIS CALL — forks this document would not settle on its own

1. **Reuse `BlockStateTable` for chests (fits today, 8 slots, no format change — §1) vs. a
   dedicated `ChestStore` sized independently (§2, this document's lean, and independently
   `-storage-qol.md`'s design too).** The dedicated table costs more to build (a whole new side
   table + save file) but decouples chest capacity from furnace/future-stateful-block capacity
   forever. The shared-table option is cheaper today and gets more expensive to unwind the more
   stateful blocks land after it.
2. **Chest slot count and table capacity are both proposals, not measurements** — 8 (if reusing
   BlockStateTable, §1) or 16/other (if a dedicated table, matching `-storage-qol.md`'s 16-slot,
   1024-chest proposal, §3.2 of that document) is a genre-feel choice, not derivable from this
   codebase.
3. **How a chest is opened (§3)** — this document recommends reusing `KEY_B`, corroborated by a
   first-hand grep and independently by `-storage-qol.md`. Still a real behaviour choice (a
   "back" button doubling as "open") that should be named explicitly before it's built, not
   discovered as a surprise in a playtest.
4. **What happens to a chest's contents on break (§4)** — recommend dump-to-bag, but the
   bag-overflow-on-break edge case (refuse the break, or drop the excess with no drop-entity
   system to drop it *into*, i.e. destroy it) is a real UX call this document flags but does not
   make.
5. **`-storage-qol.md`'s own §2.6/§3.4/§8-item-1 need a dated correction** (§0 above) — the
   multiplayer wire-ceiling gap they build a "defer multiplayer sync" argument on was closed in
   v1.8.10 (2026-09-02, `inventory.h:145-186`). Whoever reconciles the two documents should
   re-open that decision with the corrected fact, not the stale one.
6. **The two documents themselves** — `docs/plan-1.8.18-storage-qol.md` (756 lines, broader scope:
   also covers QoL items 2 and 3 from §5, network sync design, atlas budget, full build order) and
   this file (`docs/plan-1.8.18-storage.md`, narrower: exactly the six questions this lane's brief
   specified) now both exist for the same version. Whether one supersedes the other, they get
   merged, or they stay as two lenses on the same problem is not this lane's call — flagged for
   whoever owns the version.

---

## LINES SOMEONE ELSE MUST ADD

Collected here, not scattered — every one of these touches a file this lane was told not to
modify.

- **`source/world/block.h`** — a new `BLOCK_CHEST` id, a literal appended after the current
  highest core id, never `BLOCK_COUNT + n` (per that file's own warning, cited secondhand via
  `-storage-qol.md:344`; re-read `block.h` directly before relying on the exact warning text).
- **`source/world/registry.c`** — one new `kCoreDefs` row for the chest (name, hardness, flags,
  six face texture ids).
- **`source/gfx/atlas_tiles.h`, `tools/make_atlas.py`, `source/world/block_tiles_check.c`,
  `gfx/atlas.png`** — one or more new chest tiles, claimed and paired in lockstep across all four
  (per §2.9 of `-storage-qol.md`, not re-verified first-hand by this pass).
- **`source/main.c`** — the open/close key dispatch (§3), the `it->broke_valid` cleanup hook (§4),
  and the chest screen's draw/update call.
- **`source/scene/chunk_render.c`** — nothing chest-specific should be needed here if the chest is
  an ordinary full-cube block (§3.1 reasoning in `-storage-qol.md`), but confirm that assumption
  once the actual `BLOCK_SHAPE` is chosen.
- **`tools/run_host_tests.sh`** — the two new test stanzas from §6, wired in the same session they
  are written, with pinned check counts.
- **`source/world/registry_test.c`** — the pinned CRC/count golden values move by hand, with a
  dated comment, the same way every prior core-row addition has recorded the move.

---

## Numbers, and where each one came from

| Number | Value | Provenance |
|---|---|---|
| BlockStateTable payload | 16 bytes | `blockstate.h:155` [read] |
| BlockStateTable slots | 64 | `blockstate.h:144` [read] |
| InvSlot wire size | 2 bytes/slot | `inventory.c:260-263` [read] |
| Max slots in existing payload | 8 | 16 ÷ 2 [reasoned] |
| Furnace payload usage | 11 of 16 bytes | `furnace.h:88` [read] |
| blockdiff entry size | 16 bytes | `blockdiff.h:113-120` [read] |
| blockdiff table cost | 1.02 MB, outside world budget | `blockdiff.h:79-83,98` [read] |
| World budget headroom | 10.8% at radius 5 | cited secondhand from `-storage-qol.md` §2.3; not re-verified first-hand this pass — re-read `budget.h` before trusting |
| Wire item ceiling gap | CLOSED, v1.8.10, 2026-09-02 | `inventory.h:145-186` [read] — corrects `-storage-qol.md`'s stale claim |
| KEY_B free in gameplay loop | confirmed, 0 of 7 real hits in interact.c/main.c | grep with red control, this pass [read] |
