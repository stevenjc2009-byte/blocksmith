# Decision: chests live in the shared BlockStateTable, not a dedicated side table

**Settled 2026-09-05 18:03. Supersedes `docs/plan-1.9.0-storage-qol.md` sections 3.4 and 8
on this point only** — the rest of that plan (UI, stack splitting, quality-of-life items)
still stands.

## The fork

**Three** documents in this repo disagreed about where a chest's contents live, and none of
them knew about the others. The third was found only after this decision was first written,
while grepping for pins on the number 64 — `docs/plan-1.9.0-storage.md`, a second and
separate v1.9.0 storage spec written the same day as the first
(`docs/plan-1.9.0-storage-qol.md`, 52KB) and never cross-referenced by it. Its section 2 is
titled "Is the 64-slot BlockStateTable capacity enough for chests?" and answers **"No —
recommend a dedicated table instead of raising `BLOCKSTATE_SLOTS`."**

It does not change the decision, and its own evidence is why: the same section records that
raising the constant is "cheap and does not break old saves — a legitimate option, not a
version-format risk". Its objection is that 64 is a low ceiling for a storage room, which is
correct, and which raising the cap answers directly. Both plan documents now carry a note
pointing here.

It also contributes the one fact that decides the refusal policy below, which neither other
document mentions: **there is no toast or status-line system anywhere in this codebase**, so
a refused placement is silent to the player. That is a reason to refuse the placement anyway
— a chest that silently fails to appear is confusing, but a chest that appears and then eats
what is put in it is item loss — and it is a reason the eventual UI work should give the
player some feedback here.

The first two documents:

| | `source/world/chest.h` (the paused prototype) | `docs/plan-1.9.0-storage-qol.md` |
|---|---|---|
| Slots per chest | 8 | 16 |
| Home | the existing shared `BlockStateTable` | a new dedicated sparse table |
| Save file | the existing `blockstate.dat` | a new sibling `chests.dat` |
| Modelled on | `world/blockstate.c` (reuse) | `net/blockdiff.c` (new build) |

The plan document is dated Sep 3 18:42; the prototype patch is Sep 4. So the *older*
document proposed the bigger design, and the prototype was written afterwards without
citing it. That ordering is the only reason this looked like a live argument rather than a
settled one, and it is why it had to be re-derived from evidence rather than from whichever
document was found first.

The plan's stated reason for a dedicated table was that chests would otherwise compete with
furnaces for one 64-slot pool.

## Decision

**Chests use the shared `BlockStateTable`, as the paused prototype does. `BLOCKSTATE_SLOTS`
is raised from 64 to 256, and the two whole-file buffers in `blockstate.c` move from the
stack to the heap because of it.** Slots per chest stay at 8 for v1.9.0.

## Why

**1. The shared table is what `blockstate.h` was generalised for.** Its own header comment
names the next consumer explicitly — "a chest's contents or a sign's text". Building a second
sparse table would re-implement, for one caller, everything `blockstate.c` already does and
has tests for: position keying, the CRC over the body, the tmp-plus-rename torn-write
recovery, and the "state must not resurrect when a different block is placed here"
discipline that its header spends sixty lines defending. That last one is subtle, it was got
right once, and there is no reason to get it right twice.

**2. The plan's objection is real but argues for a bigger cap, not a second table.** Chests
and furnaces competing for 64 slots is a genuine problem. But a dedicated chest table has
its own cap and its own exhaustion behaviour — the same problem, one level down, plus a
second save file to keep consistent with the first. Raising the shared cap solves it once.

**3. Raising the cap is sanctioned by the format and bounded by the stack.**
`blockstate.h` already said the cap is "cheap to raise later: the save format below already
stores a live COUNT and only that many records, so a bigger cap reads an old, smaller save
file with no format change". That is true, and it was verified in `blockStateLoad`:
`if (count > (uint32_t)BLOCKSTATE_SLOTS) return true;` — a *smaller* count loads fine.

What the header did **not** say is that the cap also sizes two stack arrays:

```
BS_HDR_BYTES     16
BS_RECORD_BYTES  (16 + BLOCKSTATE_PAYLOAD_BYTES)   = 32
BS_FILE_BYTES(n) (16 + n * 32)
```

`blockStateSave` holds `uint8_t buf[BS_FILE_BYTES(BLOCKSTATE_SLOTS)]` and `blockStateLoad`
holds the same plus one byte — **on the stack**, in one frame, against libctru's 32KB
default thread stack:

| cap | buffer | share of a 32KB stack |
|---|---|---|
| 64 (before) | 2,064 B | 6% |
| 256 (chosen) | 8,208 B | 25% |
| 512 | 16,400 B | 50% |
| 1024 | 32,784 B | over the whole stack |

An over-large local on this platform does not crash — it kills the function silently. So the
cap is cheap to raise in *format* terms and expensive in *stack* terms, and the two buffers
move to the heap as part of this change. With them on the heap the only remaining cost is
the resident table: `BlockStateEntry` is 32 bytes padded, so 256 slots is 8KB of `.bss`.

256 was chosen as a plausible ceiling for one base's stateful blocks at a cost that is
linear and small. It is not a measured optimum and nothing depends on the exact figure.

**4. Eight slots versus sixteen is a gameplay choice, not an architectural one.** Eight is
what fits `BLOCKSTATE_PAYLOAD_BYTES` at 16 bytes, exactly — `CHEST_SLOTS * 2` fills it with
nothing left over, and `chest.h` has a `_Static_assert` saying so. Widening the payload to
reach 16 slots is a save-format change needing a version bump, and a version bump drops
every existing furnace's contents on upgrade. That is a separate decision with a real cost,
and deferring it does not constrain it: nothing about this choice makes it harder later.

## The consequence that is not optional

**A chest must handle `blockStateCreate` returning false. A furnace does not.**

Both furnace placement sites in `source/main.c` — `:3871` (the network-applied path, another
player's placement) and `:6577` (the local placement path) — use the return as an `if` guard
and do nothing else with it. `main.c:6568` describes this as "the return is deliberately
discarded", which is how it was first written up here and is not quite what the code does: a
false return does skip the `furnaceStatePack`/`blockStateSet` that follows. What is discarded
is any *response* — the block stays placed, the player is told nothing, and the comment
justifies exactly that:

> A player who has 256 furnaces going at once has found the limit; the 257th behaves like
> decoration rather than crashing or **silently eating what they put in it**, because every
> read of an absent record is a clean "no state here".

The distinction matters for the chest work, because "add a check on the return" is already
done at both sites; what a chest needs is a *different action* on the false branch.

That reasoning does not transfer. For a chest, an absent record reads as *empty*, so items
placed into a chest that never got a slot are destroyed on the next read. The furnace's
harmless failure is the chest's item-loss bug, and it would be invisible until a player lost
a stack of something.

`blockstate.h:196` already states the contract: *"The caller must handle this (refuse the
placement, or whatever policy it wants), not assume it cannot happen; nothing here raises
the cap on its own."* Chest placement must refuse the placement. This is tracked as part of
the chest wiring work, not as part of the blockstate change.

## What this does not decide

- **Multiplayer.** The server has no `BlockStateTable` at all — `blockstate.h`/`.c` are
  deliberately absent from the 11 files `tools/sync-world-sources.sh` mirrors, and the
  server's only per-position store is `BsDiff` (`{x,y,z,block}`, 16 bytes on disk, no room
  for a payload). Sharing chest contents needs a server-side store, a verified mutation path
  and two new opcodes. That is designed separately; this decision only fixes where the
  *client* keeps the bytes.
- **Whether 8 slots is enough for players.** A playtest question, not a code one.

## Verified / not verified

- **Verified by reading the source:** the record and header sizes, both buffers being stack
  arrays sized by the cap, the load path's `count > BLOCKSTATE_SLOTS` rejection, the
  `BlockStateEntry` layout, both furnace call sites and what they do with the return, and that
  the server mirrors neither blockstate file.
- **Corrected 2026-09-05 18:55, after this document had already been written and cited.** The
  section above first said both furnace sites "discard the return", taking `main.c:6568`'s own
  comment at its word instead of reading the two lines under it. They test it. The line numbers
  were wrong too (`:3870`/`:6574` for `:3871`/`:6577`) and local and remote were the wrong way
  round. None of it changes the decision — the hazard is that nothing *reacts* to a false
  return, which is unchanged — but a document that misdescribes the code it is briefing
  implementation from is worth correcting rather than quietly leaving.
- **Not verified:** nothing has run on a console. The stack figures above are computed from
  the constants, not measured with `arm-none-eabi-objdump` against the real frames — the
  frames also hold whatever else those functions use, so the true cost is a little higher
  than the buffer alone. The direction of that error is safe (it makes the heap move more
  justified, not less), but the exact numbers are arithmetic, not measurement.
