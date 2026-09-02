# v1.9.0 — Redstone: what subset this hardware can actually afford

This document answers what `docs/VERSION-LIST.md` says has nothing beyond
`docs/ROADMAP.md`'s one-line entry yet: which pieces of Minecraft's redstone system
Blocksmith should build. **This reads as a scoping decision, not a build plan** — the
honest answer is that a small, deliberately bounded subset is affordable and the rest
is not, for one specific, load-bearing reason found while researching this document
(§2): **this engine has no per-block metadata of any kind.** That single fact shapes
almost every choice below, more than any performance number does.

**Provenance convention**, matching the ores document: every number is tagged
**[research]**, **[codebase]**, or **[proposal]**.

`docs/ROADMAP.md`'s own v1.9.0 entry already draws the line this document builds on:
*"Wire, power, levers, buttons, pressure plates, doors, pistons."* **[codebase]**
(`docs/ROADMAP.md:396`) No comparator, no hopper, no dropper, no dispenser, no
redstone lamp, no TNT, no observer, and — notably — **no redstone torch**. This
document does not second-guess that list; it designs precisely to it and explains,
with real numbers, why that list is exactly the affordable cut.

---

## 1. What the player experiences

A lever or button, wired with dust along the ground, throws power up to 15 blocks
before it runs out — open a door or push a piston at the far end. No auto-repeating
clocks, no item-sorting hopper lines, no comparator logic reading a chest's contents.
It is the small, legible subset of redstone that exists to open a door from a
distance and build a simple trap or a lever-operated gate — closer to what a new
player builds in their first hour than to a server full of hopper-clock computers.

---

## 2. The constraint that shapes everything: no per-block metadata

A chunk cell today stores exactly one byte: a `BlockId`. All three chunk storage
forms confirm this — uniform (one `BlockId` for the whole chunk), 4-bit-palette
(indices into a 16-slot `BlockId` palette), and raw (`BlockId` per cell). **There is
no second byte, nibble, or bit anywhere for orientation, power level, or on/off
state.** **[codebase]** (`chunk.c:10-28`) The closest existing precedent — water,
the one block that might need "how full" or "which way is it flowing" — has *no*
such mechanism either: water is a full cube with no per-cell fill level or flow
direction stored **[codebase]**.

This matters immediately: redstone dust wants a 0–15 power level per cell; a door
wants open/closed; a lever wants on/off; a piston wants a facing direction and
extended/retracted. **None of that has anywhere to live except the `BlockId` itself.**

Two ways to solve this exist, and this document picks the cheap one for every
component below:

1. **Add real per-block metadata** — a new nibble array alongside the existing
   4-bit-palette indices, at 2,048 bytes/chunk (matching the existing `Palette4`
   layout). Across 8 chunks/column that is **16,384 bytes/column**, added on top of
   the existing 65,648 B/column figure **[codebase]** (per the caves document's own
   extraction of `budget.h:37-41`). At New 3DS's own radius-5 ceiling (169 columns,
   already at **89.2%** of the 12,582,912 B cap — `budget.h:54` **[codebase]**), that
   is `16,384 × 169` = **2,768,896 more bytes**, pushing the total to roughly
   **13,994,704 B — about 111% of the world budget, over by 1.4 MB.** This is not a
   design choice this document can make casually; it would not fit at New 3DS's own
   current render distance without cutting something else first. It is cheaper at Old
   3DS's radius 3 (81 columns: `16,384 × 81` = 1,327,104 B added to 5,448,784 B =
   6,775,888 B, 53.8% — fits), but a feature that only fits on one console is exactly
   what the owner's own stated priority rules out as a default.
2. **Encode every needed state as a distinct `BlockId`** — a lever's on/off, a
   door's open/closed, a piston's facing, all become separate rows in the block
   registry rather than separate bytes of metadata. This costs registry rows and
   atlas tiles (both cheap and currently well under their own ceilings — §6) instead
   of world-budget bytes (currently almost gone). **This document uses option 2
   throughout.**

The consequence worth stating plainly: **redstone dust in this design carries a
binary on/off, not a 0–15 power level.** A wire is either powered or it isn't — no
gradient of brightness, no "just barely reaches." This is a real fidelity loss
against real Minecraft's own dust, and it is the direct, traceable cost of not
touching the world budget. It does **not** cost the falloff mechanic itself (§4
still enforces "wire stops carrying power after 15 blocks") — only the *visual*
distinction between "strongly powered" and "weakly powered" is gone.

---

## 3. Component design

### 3.1 Redstone wire (`redstone_wire`)

Two `BlockId`s, `redstone_wire_off` / `redstone_wire_on`, sharing one new flat
"ground overlay" shape (§6) and, conservatively, **two** atlas tiles — a shared tile
via per-block tinting (the mechanism v1.8.8 built for biome-tinted grass) was
considered but **not confirmed available for a non-biome, per-`BlockId` tint hook**
in this pass — `BlockDef` (`registry.h:86-94` **[codebase]**) has no tint field, so
this document counts the safe, conservative number (2 tiles) rather than assume an
unverified shortcut. If a per-block tint hook turns out to exist or is cheap to add,
this drops to 1 tile — noted, not assumed.

No orientation is stored or needed: wire's on-screen connections to neighboring wire,
levers, and buttons are computed **at mesh time** from what actually occupies the
six neighboring cells, the same way v1.6.0's non-cube shape system already reads
neighbor state for occlusion (`mesher.c`'s `s_occludes`/`s_deferred` logic,
per the caves document's own extraction of adjacent mesher behavior). No new stored
state, just a mesh-time neighbor read.

### 3.2 Lever (`lever`)

One `BlockId`, `lever` — on/off is a **texture** difference (two atlas tiles, one
flat "wall/floor mount" shape shared by both states), not a geometry difference.
Sharing one shape between the two states costs one atlas tile rather than one shape
slot — see §6 for why that trade was made deliberately, not by default.

### 3.3 Button (`button`)

One `BlockId`, `button`. "Pressed" is **not stored at all** — it is a short-lived,
in-memory timer (§4), the same class of transient state a falling-block or particle
system tracks, never written to the chunk. On press, the button's own cell briefly
counts as a power source for propagation (§4) and reverts on its own after a fixed
duration; nothing about this needs a second `BlockId` or a saved flag. If the
world is saved mid-press, the save simply reflects "not pressed" — an acceptable, and
correct-feeling, edge case (real Minecraft doesn't preserve a button's press state
across a server restart either).

### 3.4 Pressure plate (`pressure_plate`)

One `BlockId`, `pressure_plate`. "Pressed" is **fully derived, live, from physics** —
whether any entity's collision box currently overlaps the plate's cell, exactly the
kind of check `physics.c` already performs for collision every frame. Zero bytes of
new state, saved or otherwise.

### 3.5 Door (`door_closed` / `door_open`)

Two `BlockId`s. Closed reuses the existing `BLOCK_SHAPE_FULL_CUBE` (shape 0) with
`REG_FLAG_SOLID` set — it is, mechanically, just a plank-textured solid cube. Open
uses one new thin, non-solid shape (§6), rendered flat against whichever neighboring
cell is solid, again computed at mesh time rather than stored.

**This gives up real per-instance facing.** A closed door has no stored "which way
does it swing" — it is a solid cube either way, and an open door's thin visual
always leans toward whichever adjacent side has a wall behind it, not toward a
player-chosen hinge side. This is a genuine, visible simplification against real
Minecraft doors, made for the same reason as §2's binary wire: no metadata byte to
put a facing in. **This is flagged as a HIS CALL item (§10)**, not decided here,
because it changes what a door looks like in play, not just how it is built.

### 3.6 Piston (`piston_north/south/east/west/up/down`, `piston_arm`)

Six `BlockId`s for the base (one per facing) plus one for the arm — seven total.
**No stored "extended" flag at all**: a piston reads as extended purely because its
`piston_arm` `BlockId` occupies the neighboring cell in its facing direction; retract
removes that cell back to air. Facing costs six rows, not six textures — `BlockDef`
already carries a **per-face** texture array (`tex[BLOCK_FACES]`, six independent
tile indices per row, `registry.h:86-94` **[codebase]**), so all six facings reuse
exactly the same two tiles (a "piston face" tile and a "piston side" tile), just
assigned to a different physical face per row. The arm reuses the "piston side" tile
for its own sides and needs no new tile of its own beyond the one new shape (§6) for
its geometry.

**Push distance: one block, not twelve.** Real Minecraft's piston can push up to
twelve blocks of world content in front of it **[research]**
([Piston — Minecraft Wiki](https://minecraft.wiki/w/Piston)); this document proposes
a piston that only extends if the single cell directly in front of it is air — if
occupied, it simply does not extend, the same "jammed, does nothing" failure mode
real Minecraft already has for an over-full push, just with a much lower ceiling.
**[proposal]** This bounds every piston activation to touching exactly two cells
(the arm's cell, and nothing beyond it) regardless of what the player has built in
front of it — no block-displacement algorithm, no chain-push logic, at all. Pushing
more than one block of world content is listed as a candidate for later (§9),
matching the pattern `docs/ROADMAP.md`'s own v1.9.1 entry already uses for descoped
extras.

**No extend/retract animation.** The base's `BlockId` and the arm's presence/absence
toggle instantly, in the same block-edit step that already exists for any other
block change. **[proposal]** An animated slide over several frames would mean
remeshing the piston's chunk every animation frame — real, repeated cost this
engine's mesh-rebuild-on-any-change model was not designed to absorb cheaply — for a
purely cosmetic flourish. Real Minecraft's own slide animation is a client-side
interpolation trick with no equivalent already built here; adding one is out of scope
for a first version.

**No sticky piston.** Not in `docs/ROADMAP.md`'s v1.9.0 list, and it needs the same
block-displacement machinery this document is deliberately avoiding — a clean
descope, not an oversight.

---

## 4. Power propagation and tick cost

**Redstone here is event-driven, not tick-driven — it has zero steady-state
per-tick cost by construction**, which is a stronger property than "throttled."
Propagation only runs when a source changes: a lever flips, a button is pressed, a
wire or a source is placed or broken. On that event, a bounded flood fill walks
outward through connected wire cells, exactly the way real Minecraft's own dust
falloff works — max signal strength 15, dropping by exactly 1 per block of dust
traversed **[research]** ([Redstone Dust — Minecraft Wiki](https://minecraft.wiki/w/Redstone_Dust)) —
except here the falloff is enforced as a hard 15-cell walk limit rather than a
stored, decrementing power value (§2's binary-power tradeoff). Anything the walk
reaches — another wire cell, a piston base, a door — gets its `BlockId` swapped
exactly the way a player's own block edit would, going through the same
mesh-dirty/remesh pipeline that already exists for mining and placing. **No new
pipeline is needed for propagation itself.**

**Worst-case bound, reasoned, not measured** (nothing here is built yet)
**[proposal]**: without a repeater in scope (`docs/ROADMAP.md`'s list does not
include one), no single continuous dust run can exceed 15 cells before power runs
out, so one event's flood fill is bounded to on the order of a few dozen to a few
hundred cells even for a heavily branched network (15-cell radius, branching capped
by the same falloff at every branch) — this is comfortably sub-millisecond, several
orders of magnitude below the per-column terrain cost already measured at ~1ms
(`docs/ROADMAP.md:333-336`). **The realistic worst case at an Old 3DS's radius 3 is
identical to the worst case at any radius**, because a single event's cost is bounded
by the 15-cell falloff, not by how many columns happen to be loaded — this is the
central reason event-driven design was chosen over any scheme that scans the loaded
world every tick.

**Contrast with a naive whole-world-per-tick scan**, which this document explicitly
does **not** build: re-evaluating every redstone component in the loaded world every
20 Hz tick, the way a first instinct might design it, is exactly the class of cost
`source/world/tick.h`'s existing `TICK_NEAR_BLOCKS`/`TICK_FAR_PERIOD` machinery exists
to bound for *other* systems (entities). **Correction to this document's own
starting brief**: `tick.h` was described as currently unused; direct reading shows it
is **already wired up** for exactly this kind of distance-based decimation — entity
think-rate uses `tickPeriodForDistSq()` at `entity.c:207` and `tickDue()` at
`entity.c:213,245-246` **[codebase]**. This is good news for redstone's own remaining
tick need (below), which can reuse this proven mechanism rather than build new
scheduling from nothing.

**What still genuinely needs a tick**: a pressed button's auto-release timer, and
nothing else (no piston animation to tick, per §3.6; wire/lever/door/plate all have
zero timed behavior). Proposed release delay: **15 ticks (0.75s)** **[proposal]**,
loosely between real Minecraft's stone (1.0s) and wooden (1.5s) button timers
**[research]** (general Minecraft knowledge, not confirmed LCE-specific), rounded to
Blocksmith's own 20 Hz tick grid. This is a **small, sparse list** — bounded by how
many buttons a player has actually pressed recently, not by how many columns are
loaded — checked via `tickDue()` exactly the way entities already are.

---

## 5. Component list: what real Minecraft has, what Legacy Console had, what this ships

| Component | In `docs/ROADMAP.md`'s v1.9.0 list | Legacy Console history | This document's call |
|---|---|---|---|
| Dust / wire | Yes | Present since launch (Beta 1.6.6 base) | **In**, binary power (§2) |
| Lever, button, pressure plate | Yes | Present since launch | **In** |
| Door | Yes | Present since launch | **In**, no stored facing (§3.5) |
| Piston | Yes | Present since launch | **In**, 1-block push, no sticky (§3.6) |
| Redstone torch | **No** | Present since launch | **Out** — not in ROADMAP's list; no NOT-gate or self-clock is buildable without it. Candidate for later (§9). |
| Comparator, dropper, hopper, weighted plate, daylight sensor, block of redstone | No | Added **TU19, Dec 2014** — years after launch | **Out**, and correctly so: none of these existed for most of Legacy Console's own life |
| Observer | No | Added **~TU54, 2017**, refined TU57 | **Out** — the newest, least-precedented component in the console line this game is styled after |
| Redstone lamp, TNT+redstone | No | Present since launch | **Out** — simply not asked for |

**[research]** Legacy-Console component timeline: [Legacy Console Edition Tutorial](https://minecraft.wiki/w/Legacy_Console_Edition_Tutorial)
(TU19, TU57 feature lists), [Legacy Console Edition exclusive features](https://minecraft.fandom.com/wiki/Legacy_Console_Edition_exclusive_features)
(TU54/Exploration Update observer bundling, general console-update history). The
practical reading: `docs/ROADMAP.md`'s existing v1.9.0 scope already lands almost
exactly on "what Legacy Console had from day one," and none of what's excluded here
is excluded by accident — every excluded piece is either not asked for or was itself
a late, second-half-of-the-console-line addition on the real hardware this game is
styled after.

**No source ranks redstone components by computational cost** — what's cited above
in §2–§4 for "cheap vs. expensive" is this document's own reasoned engineering
judgment **[proposal]**, not a citation, stated plainly as such: hopper-style polling
chains and self-sustaining clocks are the two classes of real Minecraft redstone
known informally to cause the worst lag (server-admin community sources, not wiki
documentation — [Wabbanode redstone-lag guide](https://wabbanode.com/blog/minecraft/how-to-reduce-redstone-lag-minecraft-servers)),
and both are structurally absent from this document's scope: no hopper exists to
poll, and no torch exists to build a self-inverting clock loop from.

---

## 6. Blocks and atlas tiles — and the shape-enum budget this document nearly exhausts

Registry currently has 27 of 127 core `BlockId` slots used, no pressure there. The
scarcer resource turns out **not** to be `BlockId`s or even atlas tiles — it is the
**block-shape enum**, which this document did not expect to be a live constraint
until counting it directly.

`BlockDef.flags` reserves 3 bits for shape, 8 possible values, 2 already spent
(`BLOCK_SHAPE_FULL_CUBE` = 0, `BLOCK_SHAPE_CROSS` = 1) — **6 free** **[codebase]**
(`block.h:250-254`, `registry.h:57-59,70`). This document's component list, read
naively, wants **7** new shapes (wire's flat overlay, lever-up, lever-down, button,
plate, door-open, piston-arm) — one more than exists. **This document resolves it by
making lever's on/off a texture difference instead of a shape difference (§3.2)** —
trading one atlas tile (cheap: 26 free after the ores document, or see the running
tally below) for one shape slot (scarce: only 6 exist, ever, without a format
change). With that trade, the final count is exactly six new shapes: wire-overlay,
lever, button, plate, door-open, piston-arm — **using every remaining shape slot**,
with **zero spare** for anything later that also wants a new non-cube geometry.
Flagged plainly: whatever comes after v1.9.0 that needs its own new block shape
will need the 3-bit field widened first — a real, stated consequence of this
document's own design, not a hidden cost.

| Block | New `BlockId`s | New shapes | New atlas tiles |
|---|---|---|---|
| Wire (off/on) | 2 | 1 | 2 |
| Lever | 1 | 1 (shared by both states) | 2 (on/off, texture-only difference) |
| Button | 1 | 1 | 1 |
| Pressure plate | 1 | 1 | 1 |
| Door (closed/open) | 2 | 1 (closed reuses shape 0) | 1 (shared) |
| Piston (6 facings + arm) | 7 | 1 (arm only; base reuses shape 0) | 2 (face + side, shared across all 6 facings) |
| **Total** | **15** (of 100 free core IDs) | **6** (of 6 free — exhausted) | **9** |

Running atlas tally against the 32-slot ceiling: ores document used 6, leaving 26;
this document uses 9, leaving **17** for the dimensions document.

---

## 7. State, bytes, save compatibility

- **No new persistent storage.** Every piece of redstone state that must survive a
  save/reload (wire on/off, lever on/off, door open/closed, piston facing and
  arm-presence) is captured entirely by which `BlockId` occupies the cell — the same
  one-byte-per-cell format every other block already uses. Nothing is added to
  `Column`, `Chunk`, or `LightColumn`.
- **No load-time recomputation needed.** Because power state is a `BlockId`, not a
  derived value recomputed from scratch, a reloaded world already shows the correct
  last-computed on/off state for every wire cell — there is no "recompute the whole
  network on chunk load" step to build or budget for.
- **Old saves**: purely additive — an old save simply has none of these blocks
  anywhere, and generates none until the player places one. No migration, no version
  gate, no `GEN_VERSION` interaction at all (redstone components are player-placed,
  never terrain-generated).

---

## 8. Wire protocol cost: zero new opcodes

Because every persistent redstone state change is, mechanically, indistinguishable
from an ordinary block edit — a lever flip is "replace `lever` with a different
texture selection" in exactly the shape a mined-and-replaced block already is — the
existing `BS_APP_BLOCK_EDIT` opcode (`bs_proto.h:216-239` **[codebase]**) already
covers 100% of what redstone needs to sync, both the player's triggering action and
every cell the propagation flood-fill subsequently swaps. **No new client→server or
server→client opcode is required for this version at all.** This is a direct
consequence of the §2 architecture choice (encode state as `BlockId`, not metadata) —
it was chosen for byte-budget reasons, and it happens to also remove this version's
entire wire-protocol risk as a side effect. Propagation itself must run
server-authoritatively (like all world state), broadcasting the resulting block edits
to clients exactly as any other server-driven world change already does.

**Registry CRC cost still applies** exactly as described in the ores document §6:
15 new block rows change `registryCrc16()`; the server release for this version must
ship before any client carrying it.

---

## 9. Build order

1. **New shape values** in `block.h`/`registry.h`: wire-overlay, lever (shared),
   button, plate, door-open, piston-arm — six, exhausting the 3-bit field (§6).
2. **Fifteen new registry rows** in `registry.c`'s `kCoreDefs`.
3. **Nine atlas tiles**, `tools/make_atlas.py`.
4. **Neighbor-aware mesh-time logic**: wire connection rendering, lever/button
   wall-mount orientation, door open-side inference — all computed from neighbor
   cells at mesh time, no new stored state (§3.1, §3.2, §3.5).
5. **Event-driven propagation**: the bounded 15-cell flood fill (§4), triggered on
   source block-edit events, running server-side, emitting ordinary block edits.
6. **Button auto-release timer**, reusing `tick.h`'s existing `tickDue()` mechanism
   (§4), as a small sparse list, not a per-block-in-world scan.
7. **`registry_test.c`** row-count and CRC-pin updates, matching the ores document's
   own build-order step 6.
8. **Server release ships first** (§8).
9. **Client release**, then playtest: confirm a lever-to-door and a lever-to-piston
   circuit both work at 15 cells and correctly fail to reach at 16; confirm a piston
   jams rather than crashes when blocked; confirm a button self-releases.

---

## 10. Risks and cheapest mitigation

1. **Binary wire power (§2) is a visible fidelity loss** against real Minecraft's
   15-level dust. Cheapest mitigation: none needed unless the owner wants it changed
   — it is the direct, necessary consequence of not touching the world budget, and
   the alternative (§2's option 1) is the one hard-blocked option in this whole
   document. This is really a HIS CALL item disguised as a risk — see §11 below.
2. **The shape-enum budget is now fully spent (§6)** — any future version needing a
   new non-cube block shape needs the 3-bit field widened first. Cheapest mitigation:
   none available now; flagged so the next feature that hits this isn't a surprise.
3. **Doors have no stored facing (§3.5)** — a real, visible simplification.
   Cheapest mitigation: accept it (this document's default) or fund the full §2
   option-1 metadata cost specifically for doors alone, which is smaller in isolation
   than doing it for every component (one door state needs 1 bit, not the full
   4-bit palette-style array this document costed for the general case) — genuinely
   a HIS CALL, not decided here.
4. **Propagation must be server-authoritative** in multiplayer, and this document did
   not verify exactly where in the existing server code an equivalent "apply this
   set of block edits atomically" primitive already exists versus needs to be added.
   Cheapest mitigation: whoever implements §9 step 5 confirms this against the
   server's actual block-edit handling before assuming it is a drop-in reuse.

---

## 11. HIS CALL

- **Accept binary (on/off) redstone power (§2), or fund real per-block metadata
  storage?** The metadata option does not fit at New 3DS's own current render
  distance without giving up roughly 1.4 MB elsewhere first (§2's own math) — this
  document's recommendation is binary power, but it is a real fidelity choice, not
  an implementation detail.
- **Accept doors with no stored facing (§3.5), or fund door-specific 1-bit
  metadata** (cheaper than the general case, per Risk 3 above) to get a real swing
  direction? This document's recommendation is to accept the simplification for a
  first version and revisit if it reads badly in play.
- **Accept a 1-block piston push (§3.6), or is multi-block push valuable enough to
  fund the block-displacement algorithm this document deliberately avoided?** Listed
  as a candidate for later, matching `docs/ROADMAP.md`'s own pattern for descoped
  extras (its v1.9.1 entry already does exactly this for dimension ideas).
- **Redstone torch — genuinely out of scope, or an intentional near-miss in
  `docs/ROADMAP.md`'s own list worth asking about before committing to a version with
  no NOT-gate and no clock at all?** This document takes ROADMAP's list literally and
  does not add it back in on its own judgment.

---

## 12. LINES SOMEONE ELSE MUST ADD

Nothing under `source/`, `tests/`, or `tools/` was edited to produce this document.

- **`source/world/block.h`** (near `BLOCK_SHAPE_CROSS` at lines 250-254) — six new
  `BLOCK_SHAPE_*` constants: wire-overlay, lever, button, plate, door-open,
  piston-arm.
- **`source/world/registry.h:70`** — the `_Static_assert` bounding the 3-bit shape
  field needs re-checking against the now-full 8/8 shape count (still fits; there is
  no headroom left behind it, per §6).
- **`source/world/registry.c`**, appended after the ore rows this document assumes
  land first (per `plan-1.8.12-ores.md`'s own build order) — fifteen new `BlockDef`
  rows: `redstone_wire_off`, `redstone_wire_on`, `lever`, `button`,
  `pressure_plate`, `door_closed`, `door_open`, and six `piston_*` facing rows plus
  `piston_arm`.
- **`source/world/mesher.c`** — neighbor-aware mesh-time logic for wire connections,
  lever/button wall-mount side inference, and door open-side inference (§3.1, §3.2,
  §3.5) — none of this exists today and needs new functions, not a flag flip.
- **A new server-side propagation module** — the bounded flood-fill (§4), run
  server-authoritatively on every source block-edit event, emitting further block
  edits for whatever it touches. No existing file was identified as the natural home
  for this in the codebase-facts pass behind this document; whoever implements it
  should confirm the right location against the server tree directly.
- **A small client/server timer list** for button auto-release (§4), built on
  `source/world/tick.h`'s existing `tickDue()` (already used by `source/world/entity.c:207,213,245-246`
  for an unrelated purpose — the mechanism, not the call sites, is what's reused).
- **`source/world/registry_test.c`** — row-count and CRC-pin updates, same shape as
  the ores document's own build-order step 6.
- **The server's own registry table** — matching fifteen-row update, released before
  any client build carrying these rows (§8).
