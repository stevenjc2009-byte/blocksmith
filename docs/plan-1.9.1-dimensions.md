# v1.9.1 — The other worlds: a feasibility study, not a build plan

`docs/VERSION-LIST.md` says nothing beyond `docs/ROADMAP.md`'s own one-line entry
exists for this version yet: *"Two more dimensions with their own names — the fire
one and the end one, named but not copied from anything — each with its own
generator, its own blocks and its own way in."* **[codebase]** (`docs/ROADMAP.md:402-403`)

**This document leads with the headline risk, not a caveat at the end**: a second
dimension is a second world, and this codebase's world-memory budget is a
**module-level singleton**, sized for exactly one resident world, already spending
**89.2%** of its cap on the Overworld alone at a New 3DS's own render distance. The
honest finding of this document is that **two fully-resident worlds do not fit**, on
either console, at their own native render distance — and the recommended design
below is not "make them fit" but "never let two be resident at once," plus a
dimension shape (small, finite, fixed) that makes that restriction cheap rather than
crippling. This is the single biggest risk in this three-document batch.

**Provenance convention**, matching the other two documents: **[research]**,
**[codebase]**, **[proposal]**.

---

## 1. The budget math, stated plainly, first

| Figure | Value | Source |
|---|---|---|
| World budget cap | 12,582,912 B | **[codebase]** `budget.h:76` |
| Cost per loaded column | 65,648 B | **[codebase]** `budget.h:37-41` |
| Old 3DS, radius 3 (81 cols + 2 staging) | 5,448,784 B — **43.3%** | **[codebase]** `budget.h:53` |
| New 3DS, radius 5 (169 cols + 2 staging) | 11,225,808 B — **89.2%** | **[codebase]** `budget.h:54` |

**Budget is explicitly a singleton, not a per-world structure**: *"This is a
module-level singleton on purpose: there is one world, and the bottom-screen report
needs the figures without a pointer threaded through everything that allocates."*
**[codebase]** (`budget.h:7-8`, verbatim) The same is true of the worker: a single
static generator pointer (`s_gen`), a single static save-directory buffer
(`s_world_dir[128]`), and a single static job queue (`s_queue`) all assume there is
exactly one active world at a time **[codebase]** (`worker.c:87,119,135,176`). **No
comment, hook, or API anywhere in `budget.h`, `budget.c`, `worker.h`, `worker.c`,
`world.h`, or `chunk.h` mentions multiple worlds, dimensions, or a swap operation —
this is confirmed absence, not an unexplored gap** **[codebase]**, per a direct read
of all six files.

**Could two full worlds be resident simultaneously, at each console's own native
render distance?**

- **New 3DS, radius 5 + a second world at any real size**: no. 89.2% is already
  spent; there is 10.8% (about 1.36 MB) left, less than a quarter of what radius 3
  alone costs. Not close.
- **Old 3DS, radius 3 + a second radius-3 world**: 2 × 5,448,784 = 10,897,568 B =
  **86.6%**, which *technically* fits, with roughly 1.68 MB (13.4%) left over.
  **This document does not recommend building on that margin.** It assumes zero
  entities, zero overhead this document did not model, and — worse — it is a margin
  that exists **only** on Old 3DS; the equivalent New 3DS case (keep the Overworld at
  its own native radius 5 while a second world loads) does not fit at all, meaning
  "coexist" would need to *also* silently downgrade a New 3DS's own render distance
  the moment a dimension opens, a genuinely awkward, console-different behavior for a
  project whose stated first priority is "works the same on both, New 3DS just gets
  more."

**Recommendation: swap, not coexist.** Entering a dimension fully unloads the
Overworld's resident columns (not its save — the save stays on disk untouched) before
loading the dimension; leaving does the reverse. At no point are two worlds' columns
resident at once. This is a stronger, simpler guarantee than trying to make two
worlds fit under any shared ceiling, and — see §4 — it turns out to need **no change
to the budget singleton at all**, only an orchestration step around it.

---

## 2. What the player experiences

Two small, fixed, self-contained pocket worlds — not more infinite terrain, just
somewhere else. A portal in the Overworld leads to a **single, shared, fixed-size**
version of "the fire one"; another kind of portal or found structure leads to a
**single, shared, fixed-size** version of "the end one." There is no streaming, no
render-distance setting inside either — the whole space loads at once, briefly,
behind the same kind of loading screen v0.2.0 already built for world creation.
Leaving returns the player to exactly the Overworld position they left from.

**No boss fight, no unique mob.** `docs/ROADMAP.md`'s own wording for this version —
*"its own generator, its own blocks and its own way in"* — does not mention one, and
this document does not add scope ROADMAP itself did not ask for. It is listed as a
candidate for later, alongside the shattered-sky, frozen-deep, and mirrored-overworld
ideas ROADMAP's own v1.9.1 entry already keeps as unstarted candidates.

---

## 3. Why finite and fixed-size — a real historical precedent, adapted, not copied

Legacy Console Edition (Xbox 360/PS3/Wii U-era) is the closest real precedent for
building Minecraft under a hard memory ceiling, and it solved exactly this problem by
making **worlds finite** — the opposite of Java's infinite, streamed world.
Old-generation consoles (Xbox 360, PS3, Wii U, Vita) were locked to a single
**864×864 block** "Classic" world size, never receiving the newer, larger tiers
**[research]** ([World size — Minecraft Wiki](https://minecraft.wiki/w/World_size),
[Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)).
Its Nether was scaled proportionally — **1:3** at Classic/Small size (not Java's
constant 1:8), giving a **288×288 block** Nether for a Classic Overworld
**[research]** (same source, direct quote: *"the Nether-Overworld portal ratio...
1:3 in Classic and Small"*). Its End was smaller still and **not** scaled with world
size at all — a fixed **192×192 block** area regardless of Overworld size
**[research]** (same source).

**This document deliberately does not copy the coordinate-scaling part of that
precedent**, because it does not apply here: LCE scaled Nether coordinates against
Overworld coordinates because *both* were finite, related worlds. Blocksmith's
Overworld is explicitly an infinite, streamed world (`CHANGELOG`'s own v0.1.0 entry:
*"a seeded infinite world"* **[codebase]**) — there is no fixed Overworld extent to
scale a Nether against. What this document *does* borrow is the underlying idea that
made LCE's dimensions affordable at all: **make the pocket dimension small and
finite, unlike the world it's reached from.**

**Proposed size, derived from this codebase's own numbers, not LCE's**: cap each
dimension at **81 columns (9×9), 144×144 blocks** — chosen because 81 columns is
*exactly* Old 3DS's own existing radius-3 ceiling (`RENDER_DIST_MAX_OLD`,
`budget.h:53` **[codebase]**), meaning:

- **The Old 3DS chunk-mesh pool is already sized for exactly this many columns** —
  9,199,616 bytes at radius 3, per `docs/VERSION-LIST.md`'s own v1.8.5 entry
  **[codebase]** — a fixed dimension at 81 columns needs **zero new mesh-pool
  provisioning** on Old 3DS.
- **The New 3DS chunk-mesh pool (46,948,352 bytes at radius 5) is already larger
  than that**, so 81 columns fits there for free too.
- **No streaming radius is needed inside a dimension at all** — since the whole
  space is small enough to load in one pass, "render distance" as a concept does not
  apply once inside; every column is always resident, all at once, for as long as the
  player is there.

This is **smaller than LCE's own real Nether (288×288) and End (192×192)**,
proposed deliberately — Blocksmith's per-column cost (65,648 B) times its 12 MB total
ceiling is a tighter constraint than what an Xbox 360 circa-2012 could spend on a
whole resident world, and this document is choosing "smaller, fits with margin, zero
new pool sizing" over "match LCE's absolute numbers." **[proposal]**

---

## 4. Why "swap, not coexist" needs almost no new architecture

Because budget is already a single global counter managed by claim/release calls
across worker lanes (`budget.c`'s CAS-based `s_used` counter, per the tick/budget
facts extraction), a full swap needs **no change to the budget system's own design**
— only an orchestration sequence that already has a natural home:

1. **Release every column the Overworld currently holds** (the same release path
   already used when a column falls outside render distance today — this document
   assumes that path exists and is reused wholesale, not rebuilt).
2. **Reassign the worker's static generator pointer and save-directory buffer**
   (`s_gen`, `s_world_dir[128]`, `worker.c:135,176` **[codebase]**) to the target
   dimension's own generator and its own save subfolder.
3. **Drain the job queue** (`s_queue`, `worker.c:119` **[codebase]**) before step 2 —
   any in-flight job still referencing the old world must finish or be discarded
   first, or it could write into the wrong save path mid-swap. This is a real
   synchronization requirement this document flags but does not resolve — see §8.
4. **Load the dimension's 81 columns in one pass**, not a streaming trickle.
5. **Reverse all four steps on exit**, returning to the Overworld at the player's
   saved entry position.

**The natural home for steps 1–4 is a loading screen** — exactly the mechanism
`docs/VERSION-LIST.md`'s v0.2.0 entry already built, for exactly this class of
problem (*"A real loading screen for world creation, replacing what looked like a
freeze"* **[codebase]**). Reusing it here means dimension entry does not need a new
UI concept, and it gives the player an expected, legitimate place for the one-shot
cost of generating 81 columns to hide — estimated, **not measured**, at roughly
85–106ms on host at the existing ~1.055–1.310ms/column figure
(`docs/ROADMAP.md:333-336` **[codebase]**), with **no ARM-hardware figure existing
anywhere in the tree today** for that per-column cost, exactly as `docs/ROADMAP.md`'s
own v1.8.7 entry already states about terrain generation generally. **[proposal,
reasoned, explicitly not measured]**

---

## 5. Generation — reuse existing cheap machinery, invent nothing new

Neither dimension needs a new noise implementation:

- **"The fire one"**: reuse the existing density generator's noise machinery
  (`worldgen_density.c`), retuned toward more open caverns and a lower, more exposed
  lava line — the same amplitude-table tuning lever `docs/ROADMAP.md`'s own v1.8.7
  entry already identifies as the mechanism for shaping terrain character
  (`worldgen_density.c:70-151` **[codebase, per the caves document's own
  extraction]**), not a new algorithm. Lava itself already exists as of v1.8.11
  (*"Lava pools below the lava line"*, `docs/ROADMAP.md:330` **[codebase]**) — this
  document adds no new fluid, only new floor/wall block textures and a different
  generation tuning.
- **"The end one"**: given the whole space is 81 columns, fixed, and — per §2 — a
  single shared instance rather than one-per-world-seed, this does not need a full
  procedural generator at all. **[proposal]** A small amount of seed-derived noise
  (reusing existing `noise.c` primitives, no new noise code) for island placement,
  otherwise mostly hand-authored layout, satisfies ROADMAP's *"its own generator"*
  wording without building new generation machinery.

**`GEN_VERSION` does not need to change for the Overworld at all.** Each dimension is
a wholly separate save (its own subfolder, its own region files), so it gets its own,
independent version-tracking, decoupled from `GEN_VERSION_LEGACY`/`DENSITY`/`BIOME`
entirely — exactly the same reasoning the ores document uses for why ore placement
does not need a version bump, taken one step further: **a new world doesn't touch an
existing world's version at all, by definition.**

---

## 6. Save compatibility

**Purely additive.** A dimension's save lives in its own subfolder alongside the
Overworld's existing save (e.g. a `dim_<name>/` directory next to the world's current
region files — exact naming a "LINES SOMEONE ELSE MUST ADD" item, §11). An existing
save simply has no such folder until the player first enters that dimension, at
which point it is created. **No migration step, no format change to the Overworld's
own save at all.** This is the same shape of compatibility the ores document
achieves for the same reason: nothing here touches data that already exists on disk.

**Return position**: the server already saves per-player position server-side —
`docs/VERSION-LIST.md`'s v1.5.0 entry: *"Client half of server-side saved player
state (position, meters...)"* **[codebase]**. The same field can double as "where to
resume in the Overworld" when a player exits a dimension — **no new save schema is
needed for this**, just a second saved position (one for "last Overworld position,"
independent of "current position," which becomes the dimension's own coordinates
while inside it).

---

## 7. Wire protocol: the one place this document *does* need new opcodes

Unlike the ores and redstone documents — both of which found they needed **zero**
new opcodes, because every persistent change they make is expressible as an ordinary
block edit — a dimension transition is not a block edit. It needs the client to ask
to travel, and the server to confirm which world the client is now in. Two new
opcodes, in the confirmed-free **0x10–0xFF** range — 240 opcodes, entirely free.

> ⚠ CORRECTION [2026-09-03]: this paragraph previously called **0x05–0xFF**
> confirmed-free, on the belief that `bs_proto.h`'s `bs_app_msg` enum stopped at
> 0x04. It does not. Verified directly against
> `deps/blocksmith-server/proto/bs_proto.h:217-331`, the enum runs **0x01 through
> 0x0F**: `BLOCK_EDIT` 0x01, `POS_UPDATE` 0x02, `WORLD_SYNC` 0x03, `WORLD_INFO` 0x04,
> `CHUNK_SUB` 0x05, `CHUNK_DIFFS` 0x06, `CHUNK_UNSUB` 0x07, `INV_STATE` 0x08,
> `INV_ACTION` 0x09, `PLAYER_STATE` 0x0A, `PLAYER_REPORT` 0x0B, `REGISTRY_INFO` 0x0C,
> `REGISTRY_FETCH` 0x0D, `REGISTRY_DEFS` 0x0E, `WORLD_GEN` 0x0F.
>
> Taking the old text at face value and assigning `BS_APP_DIM_ENTER = 0x05` would
> have **collided with `BS_APP_CHUNK_SUB`** — a wire-format collision between two
> live opcodes, which is a great deal worse than a stale number. The conclusion is
> unchanged (there is ample room), but the usable floor is **0x10**, not 0x05.
>
> Also worth knowing when re-checking this: the Grep *tool* is silently blind to
> `deps/` because it is gitignored, so it returns nothing here and reads as "no such
> enum". This was checked with bash `grep` on an explicit path.

- **`BS_APP_DIM_ENTER`** (client→server): "I am at a portal, requesting entry to
  dimension X." **This is client-to-server — the server must ship this opcode
  before any client build that sends it**, exactly matching the existing asymmetry
  already enforced elsewhere: an old server kicks a client for any application
  opcode it does not recognize (`bsgame.c:462-487`'s `default: send_kick(...)`
  **[codebase]**), while an old client silently ignores an unknown server opcode
  (`networld.c:975-999`'s `default: break;` **[codebase]**).
- **`BS_APP_DIM_SYNC`** (server→client): "you are now in dimension X" — triggering
  the client to run its existing world-load sequence (reusing `BS_APP_WORLD_INFO`,
  `BS_APP_WORLD_GEN`, `BS_APP_CHUNK_DIFFS` unchanged, scoped to the new dimension's
  save) rather than inventing a parallel load path.

**This is the one feature in this three-document batch where the server-first
release requirement is not just about registry CRC — it is about the client being
able to make the request at all.** A client build with dimension travel UI, talking
to a server that has not shipped `BS_APP_DIM_ENTER` yet, gets kicked the first time a
player steps through a portal.

---

## 8. Risks and cheapest mitigation

1. **Job-queue drain during swap (§4 step 3) is asserted, not designed.** This
   document did not read the job queue's cancellation/drain semantics in enough
   depth to state exactly how an in-flight job is safely stopped mid-generation.
   **Cheapest mitigation**: whoever implements this confirms `jobq.h`/`jobq.c`
   supports a clean drain (wait-for-empty) before writing the swap sequence — this
   is the one piece of §4 that is a real open question, not a solved one.
2. **The 81-column cap has zero measured margin behind it** (§3) — it is sized
   exactly to Old 3DS's existing pool, not with headroom. **Cheapest mitigation**: if
   a dimension's own generation ever needs scratch memory beyond what a normal
   column generation pass uses (unlikely, since §5 reuses existing generators), that
   scratch competes with the same ceiling and needs to be re-checked against it, not
   assumed to fit.
3. **The Old 3DS "coexist" margin (§1, 86.6%) is real but this document deliberately
   does not build on it.** If a future decision reverses course and wants some form
   of partial coexistence (e.g., keeping a small ring of the Overworld resident for a
   crossfade transition instead of a hard cut), that decision needs its own budget
   math redone against whatever partial-residency shape is chosen — not assumed safe
   by extension of this document's own numbers.
4. **One-shot 81-column generation cost is unmeasured on real hardware** (§4) —
   flagged, not resolved, consistent with `docs/ROADMAP.md`'s own standing caveat
   that no ARM-hardware per-column timing exists anywhere in the tree yet.
5. **Two new opcodes (§7) is new protocol surface, not just new content** — a larger
   release-coordination cost than the ores or redstone documents carry, since a
   client cannot even attempt dimension travel against an old server, versus ores'
   and redstone's "old server just refuses the CRC" fallback which at least fails at
   login, not mid-play.

---

## 9. Blocks and atlas tiles

Deliberately small and deliberately avoiding any new block **shape** — the redstone
document (§6 there) already spends the last of the 3-bit shape field's 6 free slots,
so every block below reuses `BLOCK_SHAPE_FULL_CUBE` (shape 0), a plain reskinned
cube, no new geometry:

| Block | Dimension | Purpose |
|---|---|---|
| Floor/wall rock | Fire | The dimension's base terrain material |
| Portal-frame block | Fire | Marks/anchors the "way in" |
| Hazard/decoration block | Fire | One distinct block for character (e.g. an ember-lit variant) |
| Floor block | End | The dimension's base terrain material |
| Portal/return block | End | Marks/anchors the "way in" and the way back |
| Decoration block | End | One distinct block for character |

**Six new `BlockId`s, six new atlas tiles, zero new shapes.** Running atlas tally:
ores document used 6, redstone used 9, this document uses 6 — **21 of 32 tile slots
across the whole batch, 11 left over.** Well inside the 32-slot ceiling
`docs/ROADMAP.md`'s reader should be checking for — this document does **not** blow
the atlas budget, unlike the shape-enum budget the redstone document already spends
to zero.

**Registry CRC**: six more rows, same lockstep cost already described in the ores
document §6 and the redstone document §8 — not repeated in full here.

---

## 10. Build order

1. **Confirm the job-queue drain question (Risk 1)** before writing any swap code —
   this is the one piece of the architecture this document could not verify secondhand.
2. **World-swap orchestration**: release-all → reassign worker statics → load-all,
   behind the existing loading-screen UI (§4).
3. **Two new registry-independent save subfolders** (fire, end) and the second saved
   player-position field (§6).
4. **Two new opcodes** (§7), server-side first, including the kick-on-unknown
   behavior already standard for any new client→server message.
5. **Two small generators** (§5), each reusing existing noise/density machinery.
6. **Six new blocks, six new tiles** (§9).
7. **Portal placement/detection and the "way in"** — a real gameplay mechanic this
   document has not designed in detail; scoping it is a `docs/ROADMAP.md`-level
   decision, not resolved here.
8. **Server release ships first** (§7's opcode requirement makes this stricter than
   the ores/redstone precedent, not just a CRC concern).
9. **Client release**, then playtest: confirm entering and leaving a dimension never
   reports over-budget on either console, confirm return position is correct, confirm
   an old client (pre-this-version) is cleanly refused rather than crashing against a
   server that now speaks `BS_APP_DIM_ENTER`.

---

## 11. HIS CALL

- **Naming — exactly the fork the owner asked to be asked about.** Two to three
  candidates per dimension, one favorite marked, final choice his:

  **"The fire one":**
  - **The Underforge** *(favorite)* — ties directly to "Blocksmith" itself (a forge,
    a smithy, molten depths under the world you build in) rather than a generic
    "fire place" name.
  - **The Cinderdeep** — evokes ash and ember plus a sunken, buried place; reads well
    but leans more atmospheric than identity-tied.
  - **Slagmere** — "slag" (molten waste) + an archaic word for a lake, evokes the
    lava seas directly; the most literal of the three, at some cost to originality.

  **"The end one":**
  - **The Hollow Reach** *(favorite)* — "hollow" for void, "reach" for something
    stretching out past the world's own edge; fits floating-island terrain without
    naming it outright.
  - **The Driftvoid** — blunter, immediately legible (drifting islands, empty space),
    at the cost of being a fairly direct compound of two obvious words.
  - **The Farside** — simplest and vaguest of the three; reads as "a place beyond,"
    but carries the least distinct identity on its own.

- **Should the two dimensions share the same 81-column cap (this document's
  recommendation, §3), or should the End specifically get a smaller cap than the
  fire dimension, echoing LCE's own asymmetry (its End was smaller than its Nether at
  every world-size tier)?** This document defaults to "same cap, simpler to build and
  reason about" rather than replicate LCE's asymmetry for its own sake.
- **Is a boss fight or unique mob wanted for the End eventually**, even though it is
  explicitly out of scope for this version (§2)? Flagged because the entity system
  (`docs/VERSION-LIST.md`'s v1.8.14) does land earlier in version order than this one,
  so it would be technically buildable by the time v1.9.1 ships — this document does
  not assume that means it should be, since ROADMAP's own wording for this version
  doesn't ask for it.

---

## 12. LINES SOMEONE ELSE MUST ADD

Nothing under `source/`, `tests/`, or `tools/` was edited to produce this document.

- **`source/world/budget.h`/`budget.c`** — no structural change to the singleton
  counter itself (§4), but a new "release everything currently claimed" entry point
  if one does not already exist in a reusable form.
- **`source/app/worker.h`/`worker.c:87,119,135,176`** — the swap orchestration:
  reassigning `s_gen` and `s_world_dir[128]` between Overworld and dimension saves,
  and confirming/adding a job-queue drain primitive against `s_queue` (Risk 1, §8).
- **`source/world/genversion.h`** — no change to the Overworld's own
  `GEN_VERSION_*` constants; each dimension's save needs its own independent version
  tracking, likely reusing the same mechanism's shape rather than its actual enum
  values.
- **A new save-subfolder convention** for dimension saves, and a second saved
  player-position field alongside the existing one from v1.5.0 (§6) — exact field
  location not identified in this pass (v1.5.0's save code was not read directly for
  this document).
- **The shared protocol header** (`bs_proto.h:216-239`, where `bs_app_msg` is
  defined) — two new opcode constants, `BS_APP_DIM_ENTER` and `BS_APP_DIM_SYNC`, in
  the confirmed-free 0x05–0xFF range.
- **`source/net/networld.c:975-999`** (client dispatch) and the server's
  `bsgame.c:462-487` (`handle_app_payload`) — new `case` arms for both opcodes,
  server first per §7/§10 step 8.
- **`source/world/registry.c`** — six new `BlockDef` rows, all `BLOCK_SHAPE_FULL_CUBE`
  (shape 0, deliberately, per §9), appended after whichever of the other two
  documents' rows land first.
- **A UI/gameplay hook for "the way in"** (§10 step 7) — portal placement and
  detection logic does not exist anywhere in the tree today and is not designed in
  enough depth by this document to point at specific lines; this is genuinely new
  design work, not a wire-up.
- **The server's own registry table and opcode handling** — matching updates,
  released before any client build carrying either (§7, §10 step 8).
