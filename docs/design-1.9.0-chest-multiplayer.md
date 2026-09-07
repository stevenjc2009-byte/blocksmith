# Design: chest contents over the wire

**Written 2026-09-05 18:10. Not yet implemented — this is the spec, not a record of work
done.** Supersedes `docs/plan-1.9.0-storage-qol.md` section 3.4's opcode numbering, which is
now impossible (see "Opcode numbers" below). Companion to
`docs/decision-1.9.0-chest-storage.md`, which settles where the client keeps the bytes.

> **⚠ Superseded 2026-09-07: this spec is implemented. The banner above is HISTORY.** —
> `blocksmith-server` v1.9.10 (commit `13843c1cb940758143e347df54bacc9c50770146`) shipped the
> three opcodes below — `BS_APP_SERVER_CAPS` (`0x11`), `BS_APP_CHEST_STATE` (`0x12`) and
> `BS_APP_CHEST_ACTION` (`0x13`), gated on `BS_CAP_CHESTS` — on 2026-09-07: branch and
> annotated tag pushed to origin, GitHub release published. The client half described here is
> built and host-suite green in the working tree, and is pending its own release as v1.9.0 —
> not yet committed, tagged or published. See `docs/VERSION-LIST.md`'s v1.9.0 entry and
> `CHANGELOG.md`'s `[1.9.0]` section for the client-side detail.

## What exists today

Nothing carries container contents. Verified by reading both trees rather than inferring:

- The client's `BlockStateTable` never reaches the network layer — `net/networld.c` contains
  zero references to `blockState`/`BLOCKSTATE`.
- The server has no equivalent at all. `blockstate.h`/`.c` are deliberately absent from the
  eleven files `tools/sync-world-sources.sh` mirrors, and `furnace.h`/`.c` are absent too.
  Furnaces are therefore already local-only in multiplayer; chests would inherit that.
- The server's only per-position store is `BsDiff` — `{x, y, z, block}`, 16 bytes on disk,
  a single id byte and no room for a payload. It answers "which block id is here" and
  nothing else. The server has no semantic knowledge of block types at all: `bsEditValid`
  range-checks the id and never asks whether it is a container.

So this needs a new server-side store, a new persistence file, and new opcodes.

## Opcode numbers

`bs_proto.h` numbers **both directions in one shared enum**, `0x01` through `0x10`, each
value carrying its own fixed direction. (This was worth checking: an earlier analysis claimed
the two directions had independent numbering, which would have allowed reusing low values.
They do not.) `BS_APP_TIME_SYNC = 0x10` is the current ceiling, so `docs/plan-1.9.0-storage-qol.md`'s
proposed `BS_APP_CHEST_STATE = 0x10` now collides with the clock and cannot be used.

Next free is `0x11`. `BS_APP_HDR_BYTES` is 1 — the opcode byte alone.

## The release-ordering problem, and the general fix

The two directions are not symmetric, and this is load-bearing:

- An unknown **S→C** opcode hits `networld.c`'s `default: break;` and is silently ignored.
  Safe to ship client-first.
- An unknown **C→S** opcode hits `handle_app_payload`'s `default: send_kick()` — the server
  disconnects the sender. A new C→S opcode shipped before its server **kicks every player on
  every older server**. This is not hypothetical; it is how client v1.2.7 broke.

Chests need a C→S opcode, so the server must ship first. But "ship the server first" is not
sufficient on its own, because servers are operator-run and a new client will meet old ones
for as long as those exist. The client needs to *know* before it sends.

There is no capability negotiation today. Rather than solve this once for chests and again
for the next C→S addition, add it properly:

**`BS_APP_SERVER_CAPS = 0x11` (S→C), sent at join before anything else. Payload: a `u32`
little-endian bitfield.**

This works precisely because of the asymmetry above. An old server never sends it, and its
absence is the signal — a new client that has not received CAPS by the time it is joined
assumes the v1 baseline and sends no new C→S opcode ever. A new server sends it and the
client gates on the bit. `BS_PROTO_VERSION` stays at 1; nothing breaks in either direction.

Bumping `BS_PROTO_VERSION` to 2 was the alternative and is rejected: it makes every old
server reject every new client outright, which is a hard break of every existing world to
gain nothing this does not already give.

Bit 0 is `BS_CAP_CHESTS`. Bits 1–31 are reserved and must be ignored, not validated, so an
older client meeting a newer server does not care about capabilities it has never heard of.

## The three opcodes

- **`BS_APP_SERVER_CAPS = 0x11`** — S→C only. `{caps u32 LE}`, 5 bytes with the header.
  Sent once at join.
- **`BS_APP_CHEST_STATE = 0x12`** — S→C only. `{x i32, y i32, z i32, slots (item u8, count u8) x8}`,
  29 bytes with the header. A whole-chest snapshot, never a delta. Sent when a subscribed
  client needs it and after every accepted mutation.
- **`BS_APP_CHEST_ACTION = 0x13`** — C→S only, gated on `BS_CAP_CHESTS`. `{op u8, x i32,
  y i32, z i32, a u8, b u8, count u8}`, 17 bytes with the header. One requested transfer.

Position is `i32 x, y, z` to match `BS_APP_BLOCK_EDIT` exactly, rather than the packed
`{x i32, z i32, y u8}` the older plan proposed. The reason is reuse, not symmetry: the
server's existing `bsEditValid(x, y, z, block)` already range-checks that triple, and a
matching layout means chest positions validate through the same function instead of a second
one written to agree with it.

Eight slots, matching `CHEST_SLOTS` in `source/world/chest.h`. Snapshots rather than deltas,
matching `BS_APP_INV_STATE`, which sends all 24 inventory slots every time for the same
reason: a snapshot cannot desynchronise, and at 29 bytes the saving from a delta is not worth
the class of bug it buys.

## Authority — the verified model, not the trusted one

The server's existing inventory handling splits into two trust models, and the split is
documented as permanent rather than as a gap awaiting a fix:

- `MOVE`, `SWAP`, `SPLIT`, `SELECT`, `CRAFT` are **fully verified**. They can only rearrange
  units the server's own `Inventory` already holds. Duplication is impossible.
- `PICKUP` and `CONSUME` are **taken on trust** — the client's self-report of what a block
  break did to its own hand. The server cannot check them because it has no terrain
  generator; it only ever holds the diffs players have made, so it cannot answer "did you
  really just mine wood there".

**Chest transfers use the verified model.** This is not a preference, it is available for
free: both endpoints of a chest transfer — the chest's slots and the player's inventory — are
already server-resident state. Unlike a block break, no terrain knowledge is needed to
validate one. The server can check the units exist and move them, so it must.

This matters more for chests than for anything else on the wire. A trusted chest transfer is
a duplication machine: two clients reporting a take from the same chest both get the items.

Flow: client sends `CHEST_ACTION`; server validates the chest exists at that position and the
requested units are present; applies the move; then broadcasts `CHEST_STATE` to everyone
subscribed to that column and sends `INV_STATE` to the actor. The client never applies its own
chest mutation optimistically — it waits for the snapshot. At 20 TPS on a LAN that is not a
felt delay, and optimistic application is the other half of the duplication bug.

## Server-side store

A new keyed store, modelled on `BsDiffStore` and keyed the same way (absolute x, y, z), with
its own persistence file `chests.bin` in `--state-dir`.

Persistence follows `day_time.txt`'s precedent, not `block_diffs.bin`'s. The diff store is an
append-only log because a block edit is an event; chest contents are *mutable state*, so an
append-only log would grow without bound and need compaction. A whole-file tmp-plus-rename
rewrite on a debounce is what the server already does for mutable state, and chest data is
small enough that this is not worth optimising.

## The orphan hazard

`blockstate.h` spends sixty lines of its header defending one invariant: **state must not
resurrect when a different block is placed at the same position.** Break a chest, place a
furnace in the same cell, and the furnace must not inherit the chest's record.

The server needs the same discipline, and it does not get it for free — its chest store and
its diff store are separate, so a `BLOCK_EDIT` that changes a cell away from `BLOCK_CHEST`
must explicitly drop the chest record in the same operation. Getting this wrong produces
either orphaned records that accumulate forever or, worse, contents appearing inside an
unrelated block.

## Open — these need a decision before implementation

**1. ~~What happens to contents when a chest is broken.~~ RESOLVED 2026-09-05 18:30 — the
mechanism already exists and the codebase says to use it.**

`source/scene/interact.h:49-53` defines `INTERACT_BROKE_EXTRA_MAX 3`, and says why: *"3
because a furnace — the only stateful block that exists today — never carries more than input
+ fuel + output at once. **Raise this if a future stateful block (a chest) ever needs more
slots than that.**"* Breaking a stateful block already reports its contents back to the
breaker through `broke_extra_item`/`broke_extra_qty`, read by `breakComplete()` on the success
path *before* `main.c`'s `blockStateRemove()` discards the record — "the one chance anything
has to read it".

So: contents go to the breaker, through the existing path, with
**`INTERACT_BROKE_EXTRA_MAX` raised 3 → 8** to match `CHEST_SLOTS`. This is what
`docs/PAUSED-WORK.md` already listed as part of the chest wiring, and it is now confirmed as
the codebase's own stated intent rather than a preference.

**The full-bag case is the dangerous half, and the shape of the existing fix is NOT what this
document first claimed.** Corrected 2026-09-05 18:55 by reading `interact.c` rather than
trusting the summary of it written here earlier.

`interact.h:12-14` records the v1.6.0 defect: a break *"wrote air FIRST and only then asked
whether the block could be carried, so a server-registered block was deleted from the world and
refused by the bag — gone, with no message."* What fixed it was **not** a reordering inside the
break. It was a guard moved to the *start of the hold* — `interact.c:420`,
`if (!blockDropsNothing(here) && !inventoryCanHold(here))`, which cancels before `breaking`
is ever set. v1.8.1 moved it earlier still, deliberately, so the player never watches a full
crack animation on a block that was never going to go.

Two things follow, and both are load-bearing for the chest:

1. **That guard asks a TYPE question, not a capacity one.** `inventoryCanHold()` is "defined,
   not air, not a liquid" — is this block carryable *at all*. It does not and cannot ask
   whether there is room right now. `BLOCK_CHEST` passes it trivially, so it provides no
   protection whatsoever for chest contents.
2. **Air is still written first, and always was.** `breakComplete()` calls
   `worldSet(w, x, y, z, BLOCK_AIR)` at `interact.c:248`, before anything else; the contents
   callback does not run until `:330-345`. What v1.8.16 F1 guarantees is narrower than "check
   before you write" — it guarantees the contents are read out *before main.c's
   `blockStateRemove()` discards the record*, which the comment there calls "the only point in
   the whole break path where anything still can". The world write is not part of that promise.

So refusing a chest break on a full bag is **new work with no existing precedent to copy**, not
an ordering rule already in force. It has to be a capacity question asked at the same place as
the type guard (`interact.c:420`, before the hold starts), which means the guard needs to know
the chest's contents at that point — the side-table record is still live there, so it can. The
alternative, checking inside `breakComplete` before line 248, is worse: it reintroduces exactly
the "crack animation on a block that will not go" problem v1.8.1 removed.

The same header notes an ordering bug "is not provable by reading a diff", which is why the
pure half of that module is host-linked. This must be proven by a host test — and, given that
the claim in this document survived being written, cited and acted on before anyone checked it
against the source, by a test with a red arm that actually fires.

**2. What a chest does on a server without `BS_CAP_CHESTS`.** Recommended: the chest can be
placed and broken normally, but opening it shows "this server does not support chests" and
refuses. The tempting alternative — let contents be stored locally — is the bad option, and
worth naming so it is not chosen later by accident: block edits *are* synced, so every player
would see the same chest holding different things, and each would believe the others had
taken their items. A refusal is confusing for a moment; local contents look exactly like
theft.

**3. Whether furnaces get the same treatment.** They have the identical problem and are
already shipping with it. Out of scope here, but the store and opcodes designed above would
extend to furnaces with a second capability bit and no structural change, which is a reason
to keep the store keyed by position and block id rather than making it chest-specific.

## Release ordering

1. Server ships `0x11`, `0x12`, `0x13`, the store, and `chests.bin` — a paired release, and
   it must land first.
2. Client v1.9.0 ships chests, gated on `BS_CAP_CHESTS`, and re-pins `PROTO_COMMIT`.
3. The pin must be verified by taking an actual clone from the remote, not by the three local
   checks — all three read the local clone and so cannot see an unpushed commit. That failure
   already happened once this month.

Single-player chests depend on none of this and can ship independently.

## Verified / not verified

- **Verified by reading both trees:** the shared opcode enum and that `0x10` is the ceiling;
  `BS_APP_HDR_BYTES` is 1; the kick-versus-ignore asymmetry in both dispatch defaults; the
  two inventory trust models and why the trusted one exists; `BsDiff`'s layout having no room
  for a payload; the eleven mirrored files excluding both `blockstate` and `furnace`.
- **Also verified by reading, 18:30:** `INTERACT_BROKE_EXTRA_MAX` and its stated reason for
  being 3, and the `broke_extra_item`/`qty` read happening before `blockStateRemove` discards
  the record.
- **Read wrong at 18:30 and corrected at 18:55.** Open question 1 was closed partly on a claim
  that the v1.6.0 fix had made the break path "check before writing air". It had not — the fix
  is a type guard at the start of the hold (`interact.c:420`) and air is still written first
  (`interact.c:248`). That was taken from `interact.h`'s narrative of the old bug rather than
  from the code that replaced it, which is the standing trap of reading a comment block's
  history as a description of the present. The half of question 1 that survives is the half
  that was actually read: contents go to the breaker through `broke_extra_*` with
  `INTERACT_BROKE_EXTRA_MAX` raised 3 → 8. The full-bag refusal is now correctly listed as new
  work.
- **Not verified:** nothing here has been implemented or run. The 20 TPS round-trip being
  imperceptible is an assumption, not a measurement.
