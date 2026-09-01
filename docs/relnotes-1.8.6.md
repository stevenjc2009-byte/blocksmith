# Blocksmith v1.8.6 — Speed

This release adds nothing and changes nothing you can see. It is four pieces of work that
make the game do less, and two test harnesses to stop that work quietly undoing itself
later.

**Everything here is byte-for-byte invisible.** The world generates the same terrain, the
mesher draws the same geometry, and your saves are unchanged. That was the acceptance
condition rather than a hoped-for side effect, and it is proved rather than asserted — the
numbers are at the bottom.

Nothing in this release is Old-3DS-only or New-3DS-only. Both consoles get all of it.

## Trees stop asking a question they cannot use the answer to

Decorating a column considers tree positions in a margin around it, and used to ask each one
how tall the terrain was there — an expensive noise query — before working out that the tree
was too far away to place a single block inside the column.

The reach test now happens first. It is a deliberately conservative symmetric bound, so it
can reject a tree only when that tree provably cannot reach; it can never discard one that
would have placed something.

**73.28%** of height queries during decoration are now skipped. The decoration pass runs
**3.3–3.7× faster**, worth **2.8–6.2%** off generating a whole column.

## The mesher keeps one array where it kept three

Per-cell solid / occludes / draws flags were three parallel byte arrays. They are now one
array of packed bits: **17,496 bytes of working memory down to 5,832**, and about **7–10%**
faster.

That figure is lower than an earlier estimate of 25%, which did not survive being measured.
The 7–10% is what the measurement actually says.

## Saving a column no longer re-reads what it just wrote

After writing a column, the region layer asked whether the file needed compacting — and
answered by reopening the file and re-reading its directory, recomputing state the write had
just finished producing. The write now hands that state forward directly.

A save that declines compaction drops from **6 file opens to 3**, and stops re-reading
**6,176 bytes** of directory every time. The old cold-read path is untouched and is still
used whenever the hint does not apply.

## Two new guards, because the old ones were too narrow

**A geometry-equality guard for the mesher** — 408 chunks across 12 seeds and 5 world kinds.
The existing pin was four fixtures, and four is a sample, not a distribution. It self-checks
against those same four first and refuses to report anything at all if its own wiring cannot
reproduce them, so a broken harness fails loudly instead of printing 408 confident matches.

**A region write-hint test** — 5,660 checks. Its central test runs the new hinted path
against a path forced to read cold, over an identical 400-save sequence, and requires that
they compact at the *same points* and produce *byte-identical files*. Agreeing on the total
number of compactions would not have been enough: two paths can compact the same number of
times at different moments and still diverge on disk.

## Fixed: the interaction tests were never testing the lighting paths

`interact_test.c` never started the light engine, so the relight branches taken on every
block break and place had **never once executed in a test run** — 169 checks passing, and
not one of them reaching that code. They do now, and the count is 193.

Proved rather than assumed: an instrumented copy of the interaction code ran against the
unmodified test and printed nothing at all, then printed 20 hits once the engine was forced
on. The same tests, the same 169 checks, the difference being whether the code under test
ran.

The fixture in that file also leaked. Each one called `worldInit` without `worldExit` first,
and `worldInit` is a bare memset that releases nothing, so every fixture inherited the
previous one's memory claims. Test-only, invisible to players — but it was hiding real budget
behaviour from the tests that exist to check budget behaviour.

## Not changed, deliberately

The New 3DS per-frame work caps. The plan was to raise them if the *count*, rather than the
time budget, was what limited them. The measurement taken to decide that was contaminated:
unrelated processes on the measuring machine inflated every frame time by almost exactly 2×,
including frames with no work queued at all. Correcting for that puts the figure directly on
the budget line rather than clearly on either side of it.

Neither constant is being changed on evidence that cannot support the change. The relight cap
got no evidence at all, because a plain walk never engages it.

## Compatibility

Worlds, saves and multiplayer are untouched, and this is a stronger claim than the usual "no
format change":

| Claim | Evidence |
| --- | --- |
| Terrain is byte-identical | 484 columns, 4 seeds, hash `820dd26ab8db0d7a` before and after |
| Geometry is byte-identical | 408 chunks, 12 seeds × 5 world kinds, zero differing lines |
| Saves round-trip identically | 5,280 column-saves, both write orders, 0 wrong |

A 1.8.6 client and a 1.8.5 client see the same world and can play together.

## Not verified

Nothing in this release has run on real 3DS hardware. Every timing figure quoted above is a
host x86-64 ratio, and a host ratio is not a console frame cost. The in-game numbers that
should move as a result — the world-generation load stage, and the save-side counters — can
only be read on a console.
