# Blocksmith version list

Every version Blocksmith has had, oldest first, followed by every version currently
planned, in one continuous order. Released and planned are kept clearly apart with a
line, and every entry states its own status. Full detail on everything already shipped
is in `CHANGELOG.md`; full reasoning on everything not yet built is in `docs/ROADMAP.md`
and, for the three versions that have a dedicated plan document, in
`docs/plan-1.8.7-terrain.md` and `docs/plan-1.8.8-biome-identity.md`.

**Newest published version: 1.8.9** (confirmed via `gh release list` — `v1.8.9` is tagged
`Latest`, released 2026-09-02). Everything from `v0.1.0` up to and including `v1.8.9` is
released and published on GitHub. `source/version.h` in the working tree currently reads
`1.8.10`, and `CHANGELOG.md` already carries a full, dated `## [1.8.10]` entry — but that
version has not been committed, tagged, or published as a GitHub Release yet, so it is
mid-release, not "planned" the way everything after it is. See its own entry below for
exactly what that status means. Everything after `v1.8.10` is a plan, not a build; order,
scope, and whether a given version ships at all can still change before it does.

A console note appears below **only** where an Old 3DS and a New 3DS genuinely differ —
a different render-distance ceiling, extra RAM, a higher clock, an extra core, anything
gated on `hwIsNew3ds()`. Most versions affect both consoles identically and say nothing
about hardware, because "works the same on both" is the default, not a feature.

---

## Released

### v0.1.0 — First release — released and published

**Added.** Playable single-player: a seeded infinite world, chunked terrain with caves,
breaking and placing blocks, a three-recipe crafting table, saving with torn-write
recovery, and an installable `.cia`. Never run on real hardware.

### v0.2.0 — Loading screen and updater — released and published

**Added.** An in-app updater (Options → Check for Update). A real loading screen for
world creation, replacing what looked like a freeze. A watchdog that writes a report if
the main thread stops responding.

**Fixed.** World creation sped up 4.7x by turning off an on-console self-test that had
been eating 99% of the load time.

### v1.1.0 — Multiplayer works end to end — released and published

**Added.** Joining a server puts you in the server's world; your edits reach it, other
players' edits reach you, and a lost connection is detected and reported instead of
leaving you in a HUD that lies. Not tested on real hardware.

*v1.1.1 through v1.2.5 are fourteen versions in a row chasing one hardware-only freeze
that never once reproduced in an emulator. Most of them are diagnostic pre-releases —
instruments built to answer one question about where the console was stopping, not
fixes. The freeze was "fixed" twice: a wrong fix shipped in v1.2.0 and was retracted in
v1.2.1; the real cause was found in v1.2.5.*

### v1.1.1 — diagnostic: draw bisect — released and published

A six-arm bisect removes one part of the frame per boot to narrow down where the freeze
happens. No fix.

### v1.1.2 — diagnostic: draw-stage breadcrumb — released and published

Records exactly where inside chunk drawing the console stopped, plus a second, finer
bisect. **Fixed** a bookkeeping bug in v1.1.1's own bisect. No game fix yet.

### v1.1.3 — diagnostic: bisect harness fix — released and published

**Fixed** the diagnostic itself: v1.1.2 was reading a leftover file from v1.1.1 and had
been skipping all drawing the whole time, so it "survived" without ever running the real
test. No game fix.

### v1.1.4 — diagnostic: draw guard, finer breadcrumb — released and published

**Added.** Extends the breadcrumb to cover every GPU submission in the frame, and
validates each chunk draw call before issuing it. Rules out command-buffer overflow and
missing cache flushes as causes. No fix yet.

### v1.1.5 — diagnostic: GX queue wait — released and published

Narrows the freeze to the wait on the GPU's command queue — not frame pacing, not a dead
interrupt thread. No fix yet.

### v1.1.6 — diagnostic: queue contents — released and published

Reports exactly which queued GPU command the console is stuck on. No fix yet.

### v1.1.7 — diagnostic pre-release — never published

Built, then withdrawn in favour of v1.1.8, which carries the same work plus a test
battery. **There is no v1.1.7 release, tag, or CIA** — it exists only as a step in
`CHANGELOG.md`'s record of the investigation.

### v1.1.8 — diagnostic: GPU pre-flight + validator — released and published

**Added.** A GPU self-test at boot and a per-frame command-list validator that flags bad
addresses and NaNs. Diagnostic pre-release, not a fix.

### v1.2.0 — freeze "fixed" (cache flushes) — released and published

**Added.** Missing `GSPGPU_FlushDataCache` calls project-wide, and a bound on the GPU
wait so a wedge no longer hangs the console outright. Believed at the time to be the fix
for the hardware freeze. **It was not** — see v1.2.1.

### v1.2.1 — freeze actually fixed (CPU/GPU race) — released and published

**Fixed.** The real cause: the CPU was rewriting chunk vertex buffers while the GPU was
still drawing from them. Adds a wait that serialises the two. Also fixes two latent
use-after-free crashes in shader teardown. v1.2.0's cache-flush theory is explicitly
retracted here as not the cause — it stays in, but not for that reason.

### v1.2.2 — diagnostic: boot fencing, real reporting — released and published

**Fixed.** The console mixing up which boot a diagnostic file belongs to; restores full
crash reporting; fixes a checker that was misreading GPU uniforms as the wrong float
format, which had been manufacturing false "NaN" reports.

### v1.2.3 — diagnostic: draw bisect, round two — released and published

**Added.** Re-enables a finer bisect that cuts inside the chunk-draw call itself, now
version-stamped so it cannot misread a stale probe file left by a different build.

### v1.2.4 — experiment: vertex shader indexing — released and published

**Changed.** Replaces the world shader's one indexed uniform fetch with a fixed one, as a
test — not a confirmed fix — deliberately flattening lighting so the build is visibly the
experimental one.

### v1.2.5 — freeze fixed for real (vertex misalignment) — released and published

**Fixed.** The actual cause, thirteen builds in: a 4-byte vertex attribute was being
fetched from an unaligned byte offset. Real GPU hardware cannot do that; an emulator
tolerates it silently, which is why nothing here ever reproduced off a real console.
Fixes the vertex layout and adds a compile-time assert so it cannot regress unnoticed.

### v1.2.6 — remote edits now visible — released and published

**Fixed.** Multiplayer: another player's block edits changed the world but not what you
saw on screen, because only local edits marked their chunk for a rebuild.

**Added.** A crosshair.

### v1.2.7 — pause menu — released and published

**Added.** SELECT opens a real pause menu — render distance, a memory readout — instead
of requiring a trip back to the title screen. Wooden planks.

**Changed.** Rewrites how a joining client receives world edits so the amount is bounded
by render distance instead of by the server's whole history.

**Fixed.** 289 columns that stayed allocated for the whole session after a boot-time
memory check.

### v1.3.0 — server-owned inventory — released and published

**Changed.** On a server, your inventory now lives on the server instead of in each
console's own RAM — closes a duplication exploit and stops carried items vanishing on
sleep.

### v1.3.1 — server inventory: actually works — released and published

**Fixed.** Two ordering bugs that meant v1.3.0's server inventory did nothing at all —
every rejoin came back empty.

### v1.4.0 — remapping, sleep, minimap, debug menu — released and published

**Added.** Full control remapping. A sleep/lid handler that pauses the game properly
instead of running on into a suspended console. A live minimap with fog of war. A debug
menu with render distance, live stats and placeholder rows for later systems.

**Fixed.** A broken protocol pin that stopped a fresh clone from building.

### v1.5.0 — saved players, adaptive lighting — released and published

**Added.** Client half of server-side saved player state (position, meters — held dark
until the server side exists to fill them). A real lighting engine: sky light baked per
vertex, computed dynamically on a New 3DS and identical to before on an Old 3DS, which
runs neither the new engine nor its shader. *This is a genuine console difference: only
a New 3DS runs the new lighting engine and its shader.*

### v1.5.1 — repair release — released and published

**Removed.** The minimap — it crashed a real console.

**Fixed.** The debug menu stepping render distance on open, an unreachable controls
screen, mis-clocked timing after sleep on a New 3DS, lost building on lid-close, and a
multiplayer session left as a zombie after sleep.

### v1.6.0 — server-defined blocks, render rebuild — released and published

**Fixed.** Finishes the block registry: a server-defined block can now actually be seen,
mined and synced correctly. Several defects this exposed, including a registry sync that
verified nothing and a server block that deleted itself the moment it was mined.

**Added.** Non-cube block shapes and greedy face merging (15–21% fewer quads). Rebuilds
the texture atlas as a one-tile-wide strip to make that merging possible.

### v1.7.0 — new terrain generator — released and published

**Fixed.** If you had v1.6.0, it did not start — this release fixes that first.

**Added.** A Beta-1.7.3-style density-field generator with real caves and overhangs,
water (still, non-flowing), and tall grass. Worlds record which generator made them, so
existing worlds keep their old terrain. *This is the version that added the Beta
1.7.3-style terrain the project has been asked about — see the closing section below.*

### v1.7.1 — repair and optimization — released and published

**Fixed.** Spawning inside your own blocks after a reload, players returning to the
wrong position, unbreakable tall grass, and repeated region-file rereads.

**Changed.** Four measured performance fixes to the mesher, column install, horizon
culling and light-fill. One of the four originally claimed here (region compaction) was
later found to still be unwired — corrected under v1.8.2 below.

### v1.8.0 — water moves — released and published

**Changed.** Water is simulated rather than static: it spreads, falls, finds its level,
and drains away when its source is removed.

### v1.8.1 — breaking takes time — released and published

**Changed.** Per-block break times (hardness) replace instant breaking, with a crack
overlay and an interruptible hold. The core block registry's checksum changes as a
result — the matching server release is required.

### v1.8.2 — water made to look and feel right — released and published

**Changed.** Water now spreads gradually instead of instantly, sits below the block rim,
is see-through, and settles instead of bobbing at the surface. Texture atlas expands
from 15 to 64 usable slots.

**Fixed.** Documents, after the fact, that region compaction (the write-amplification fix
v1.7.1 claimed) was actually wired up in this release, not that one — correcting three
releases' worth of changelog entries.

### v1.8.3 — biomes — released and published

**Added.** Six biomes — tundra, taiga, plains, forest, desert, jungle — each with its own
ground, plants and tree shape. Worlds get their own terrain seed instead of sharing one.

**Fixed.** A client that joins a server running a different generator is now refused
rather than silently desyncing.

### v1.8.4 — New 3DS treated as a New 3DS — released and published

*A genuine Old/New 3DS split.* **Added.** The 804 MHz clock, the L2 cache, the larger
memory mode and the third CPU core are actually requested and used now — they were
written in an earlier version but gated behind a flag that defaulted off. Background
world work moves onto the New 3DS's extra core; an Old 3DS is unchanged.

**Added.** A `RETRY DOWNLOAD` button — a failed download goes straight back to
downloading instead of re-checking the release list first.

**Changed.** You press jump to climb out of water; a press stays armed for a quarter of
a second and buys exactly one climb.

**Fixed.** The core request was written but gated behind a flag that defaults to off, so
a New 3DS previously got exactly what an Old 3DS got. The two questions are now separate.

### v1.8.5 — render distance — released and published

*A genuine Old/New 3DS split.* **Added.** A New 3DS can now be set to render distance 5;
an Old 3DS stays at 3, which is what its memory holds. A settings file written on a New
3DS is safely clamped back to 3 if it is read on an Old 3DS.

**Changed.** The chunk mesh pool is now sized at runtime from the detected console
instead of fixed at compile time for the smaller one — the pool costs 9,199,616 bytes at
radius 3 and 46,948,352 at radius 5, and an Old 3DS's 33,554,432-byte linear heap could
not have held a compile-time-raised ceiling. The fog is now a depth-based ramp instead of
the PICA200's hardware LUT, whose knots bunched near the camera and capped half-visibility
at about 14.4 blocks *at any render distance*. Half-visibility is now **28.5 blocks at
distance 3, 38 at distance 4, and 47.5 at distance 5** — a clean 9.5 blocks per step, and
the answer to "how far can I see now" — see the closing section below.

**Fixed.** Chunks that could go permanently missing at a wide render distance (the mesh
queue was sized for the old limit and could silently refuse work); the pending
network-edit store, which took a flat 1.02 MB whether or not Multiplayer was ever opened,
now allocates only on the first server packet that needs it (measured off the built ELF:
`.bss` 1,725,944 → 660,968 bytes).

### v1.8.6 — Speed — released and published

No new features and nothing that looks different on screen. **Every change in this
release is byte-for-byte invisible**: the world generates the same terrain (484 columns
across 4 seeds hash identically before and after, `820dd26ab8db0d7a`), the mesher draws
the same geometry (verified across 408 chunks), and saves round-trip identically. Both
consoles get all of it — nothing here is Old-3DS-only or New-3DS-only.

**Changed.** Tree placement no longer queries expensive terrain-height noise for a tree
that provably cannot reach the column being generated — the reach test now runs first.
73.28% of height queries during decoration are skipped; the decoration pass runs
3.3–3.7× faster on host, worth 2.8–6.2% off generating a whole column. The mesher's three
parallel per-cell flag arrays became one packed-bit array (17,496 bytes of scratch down
to 5,832; about 7–10% faster on host — not the 25% an earlier, unmeasured estimate
claimed). Saving a column no longer re-reads its own directory from disk right after
writing it — a save that declines compaction drops from 6 file opens to 3.

**Added.** A geometry-equality guard for the mesher (408 chunks, 12 seeds, 5 world
kinds) and a region write-hint test (5,660 checks) that requires a hinted save and a
cold-read save to compact at the same points and produce byte-identical files.

**Fixed.** `interact_test.c` never started the light engine, so the relight paths taken
on every block break and place had never once executed in a test run — they do now (169
checks became 193). A leak in that same test's fixture that had been hiding real budget
behaviour from the tests that check budget behaviour.

**Not changed, deliberately.** The New 3DS per-frame work caps
(`DRAIN_MAX_CHUNKS`/`RELIGHT_MAX_COLUMNS`) — the measurement meant to justify raising
them was contaminated by unrelated load on the measuring machine, so nothing is being
changed on evidence that cannot support the change.

**Not verified.** Nothing in this release has run on real 3DS hardware; every timing
figure above is a host x86-64 ratio, not a console frame cost.

### v1.8.7 — Terrain — released and published

**Despite the name, this did not add "the Beta 1.7.3 world" — that already shipped, across
v1.7.0 and v1.8.3 (see the closing section below).** What this version actually did:
validated and refined the generator v1.7.0 already shipped, and — unexpectedly — turned up
two bugs that had been quietly corrupting worlds since earlier releases. What grew around
that is a large speed pass and the groundwork for a second world-generation thread on the
New 3DS.

**Fixed.** Lighting could be relit wrong, on both consoles, since v1.8.0: the main thread
and the world worker were both running flood fills over one shared queue, despite a comment
in `worker.c` that had asserted otherwise since before it became false. Measured under
contention, 116 of 120 columns relit wrong. The queue is now claimed atomically, with the
losing thread falling back to the sweep engine the file already keeps; lighting output stays
byte-identical. *Console difference:* on a New 3DS the worker genuinely runs at the same
time as the main thread (the third core), so the window was permanently open; on an Old 3DS
the two share a core but a relight holds the queue for many scheduler slices, so it was wide
open there too — both consoles were affected, the New 3DS more often. Also fixed: a block
you broke or placed could silently fail to reach other players, because a failed send was
discarded rather than checked — the edit (and the inventory change riding on it) is now
rolled back if the packet never left, with single-player left untouched since it reports the
same "no session" condition for an unrelated reason. A client joining a server running a
different block-id mapping used to be let in anyway after a two-second wait, with every
disagreed block silently resolving to air; it now refuses the join with an explicit message
instead. And the pause menu no longer leaves break/place and water flow live in the
background — X and Y kept working, and water kept moving, while the world looked paused.

**Changed.** Cave generation is 3.09× faster and a whole column is 2.20× faster (1.190 →
0.542 ms/column on host), by caching the noise-lattice corners a column's roughly 20,900
cave tests were re-deriving on every call, once per column instead of once per test. The
terrain interpolator now short-circuits the 92.2% of lattice cells that are uniform (43.2%
air, 49.0% solid) exactly rather than running all 224 interpolations to arrive at a constant
— worth 7.6–9.7% off generation on host. Chunk culling does less work per candidate (a
square root moved from per-pair to per-blocker; the view matrix is built directly, proved
bit-identical across 341,280 cases including the signed-zero edge cases). Chunks now load
nearest-first, dropping columns queued ahead of your own immediate surroundings from 98 to
8. (All ratios are host x86-64, not console frame cost.)

**Added.** The world generator no longer keeps its working memory in globals — the
prerequisite for a second generator thread on the New 3DS. Two threads generating separate
columns previously produced 25 of 32 columns wrong and refused 145 of 192 generations
outright (a refused generation is a permanent hole, not a slow frame); now proved 0 wrong
and 0 refused by a permanent two-real-thread test. *Console difference:* this is groundwork
only — there is still just one generator thread running on either console — and it exists
for the New 3DS specifically, since an Old 3DS has no spare core to give it. Also added: a
50-seed audit of the terrain amplitude table (29,491,200 lattice steps), confirming the
overhang preconditions the Beta-1.7.3-style terrain depends on hold across every seed
tested, not just the handful it was originally tuned against.

**Notes.** Terrain is byte-identical to v1.8.6 — same seed, same world, and a 1.8.7 client
and a 1.8.6 client can still play together. Binary size: text +9,064 bytes, static memory
+5,056 bytes. Not run on real 3DS hardware.

*Background this version settled but did not itself change:* `docs/ROADMAP.md` had floated
raising world height as part of "Terrain"; that was decided against, on the numbers rather
than by feel. 128 already matches Beta's own world height, and the amplitude table — not
height — is the measured overhang lever. It is also not affordable: one loaded column costs
65,648 bytes at 128 blocks tall, and at 192 blocks the worst-case 17×17 case would reach
roughly 28,554,560 bytes against a 12 MB budget, about 2.27× over. World height stays at
128. Full research, with sources and file:line evidence: `docs/plan-1.8.7-terrain.md` and
`docs/research/terrain-beta-1.7.3.md`.

### v1.8.8 — Biome identity — released and published

Biomes are now told apart by colour, Minecraft-style, and around that one feature grew
twelve new blocks and plants, a debug-only border overlay for checking where biomes actually
meet, a browsable version history, and the first release where sound, the day/night cycle
and entities are wired into the game and actually run, rather than sitting built and silent.

**Added.** Biomes are told apart by colour: one grass block, and one set of tall-grass and
fern art, tinted per biome rather than needing a separate block per biome. The half-grass/
half-dirt block is untouched by this (checked pixel by pixel), and the grass crust on its
side is deliberately left untinted, because this hardware has no way to tint half a block
without also tinting the dirt half. **The tint can only darken or mute the source art, never
brighten or shift it past what the texture already holds — desert currently comes out a dry
olive rather than the sandy tan you'd expect.** Twelve new blocks and plants: birch and
spruce logs, planks and leaves; a second tall-grass cell; four flowers (poppy, daisy,
bluebell, orchid); and apples, their own breakable, carryable block, with birch the softest
wood and spruce the hardest (and the same ordering for their leaves). Apples grow in oak and
birch leaves only, never spruce, and drop about 1 in 200 breaks — the odds come from the
leaf block's own position rather than break order, so every player in a multiplayer game
agrees on whether a given leaf drops one. The debug menu's bottom screen now lists every
registered block with its icon and name, paged with the D-pad, and now also shows the
current biome. A "Version history" button on the check-for-update screen lists what every
past version added, changed, fixed and removed, generated straight from `CHANGELOG.md` so it
cannot say something the changelog doesn't. A debug-only neon biome-border toggle (off by
default) draws a glowing fence along every biome boundary. *New 3DS only:* a second thread
can now generate world terrain alongside the first, using the spare core — proved not to
duplicate or drop a column under real concurrent load, but never run together on an actual
console, so this is foundation work with no in-game speed claim yet; an Old 3DS has no spare
core to give this and is unaffected. Sound effects, the day/night cycle, and entities are
wired into the game and actually run for the first time — all three existed and were tested
in isolation earlier in the 1.8 line but had never been switched on. Three sounds ship
(block break, block place, footstep), CC0 from Kenney's Impact Sounds pack. Entities are
foundation only: storage, physics and ticking exist and run, but there are no mobs yet.

**Changed.** Dead bushes redone — a bushier clump instead of a single thin diagonal twig.

**Fixed.** Six items, cactus among them, could not be picked up, and cactus specifically
could not be broken at all — this is the same bug `docs/ROADMAP.md` had misnamed as a
`BlockId` ceiling problem. `BlockId` (`source/world/block.h`) is a `uint8_t` with roughly
112 free ids below its real 0xFD/253 ceiling; the actual bug was `inventoryCanHold()` gating
on `item < BLOCK_COUNT`, where `BLOCK_COUNT` was a leftover 8 sized for the game's original
blocks. It now asks the block registry instead, so tall grass, snow, ice, cactus, dead bush
and fern can all be broken, carried and placed, and cactus got its own break time (between
snow's and ice's). Standing grass strands were a jarring lime green against the block's own
darker green — traced to the source art itself, not lighting or the new tinting, and
repainted to match. The debug overlay's "see" distance reading was still computed for the
console's built-in fog hardware, which has been off since v1.8.5 in favour of a hand-drawn
fade, so it read roughly 14 blocks regardless of the setting; it now reports the real
figures — roughly 28 blocks at render distance 3, 38 at 4, 47 at 5.

**Notes.** The chunk pop-in reported after v1.8.7 is not fixed, and still not explained:
this release cleared the built-in fog hardware as a suspect (off since v1.8.5), a long walk
through the streaming code found nothing wrong, and an emulator walk at render distance 5
showed no holes — but none of that proves it can't happen on real hardware, since nobody has
yet measured how fast the console itself generates a chunk. That measurement, not a fix, is
the next step. Server-side groundwork so items like the cactus survive a multiplayer rejoin
shipped on the server, but the client half did not land this version — a cactus or any of
the other five newly-carryable items picked up over multiplayer will not survive leaving and
rejoining yet. Not run on real 3DS hardware.

Full research, with sources and file:line evidence: `docs/plan-1.8.8-biome-identity.md` and
`docs/research/biome-identity.md`.

### v1.8.9 — Sky and weather — released and published

Rain and snow now actually fall, the sun's light is finally shaped the way Minecraft shapes
it, and water splashes when you drop into it. The weather model itself has existed and been
under test since earlier in the 1.8 line, but nothing in the game had ever called it — so
despite the tests being green, no weather had ever happened until this release wired it in.

**Added.** Rain and snow, decided per biome and by how cold the column is — snow in cold
biomes, rain everywhere else — drawn as camera-centred billboard strips, the same cheap
approach Minecraft itself uses on this class of hardware, and depth-tested against already-
drawn terrain so it does not fall through a cave ceiling or a roof you built. Snow settles on
the ground in layers, capped at one block deep. Dropping into water throws up a scatter of
splash particles, fixed by where and when you hit the water rather than a running random
sequence, so two players in the same world see the same splash. The weather simulation is
running for the first time: loaded columns are ticked on the same clock the day/night cycle
uses — saved with your world, agreed with a server — instead of a counter that restarted on
load, which is what stops rain jumping the moment you rejoin; near columns tick often, far
columns rarely.

**Changed.** Night is dark again: brightness is now shaped by Minecraft's own curve instead
of used raw, which is what makes a torchless interior read as gloom instead of dusk.
Measured against the reference, a midnight interior had been rendering 3.2× brighter than it
should have been; daylight is almost unaffected.

**Fixed.** A comment in the world shader claimed it was New-3DS-only; it has run on both
consoles since v1.8.0. Nothing behaved wrongly because of it, but anyone reading the render
path was being told the wrong thing.

**Notes.** Sky light and block light are still combined by taking whichever is brighter,
where Minecraft adds them — that difference only starts to matter once there is a torch to
emit block light, which is v1.8.10. The chunk pop-in reported against v1.8.7 is still not
fixed and still not diagnosed. Not run on real 3DS hardware.

---

## Planned

Everything from here down has not been published as a GitHub Release. v1.8.10 is the one
exception to "not shipped" in this section — it is real, built, tested code sitting
uncommitted in the working tree with a matching `CHANGELOG.md` entry, just not yet
committed, tagged, or published; see its own entry for exactly what that means. Everything
from v1.8.11 onward genuinely is a plan, not a build, and their scope, order, and even
whether a given version ships at all can still change. Where a version has a dedicated
research brief or plan document beyond `ROADMAP.md`'s own entry, that is named so the
deeper detail can be found.

### v1.8.10 — Light — built in the tree, not yet committed, tagged, or published

**What this status means, plainly: this is not a plan.** `source/version.h` already reads
`1.8.10`, `CHANGELOG.md` already carries a full dated `## [1.8.10]` entry, and
`whatsnew1.8.10.txt` exists and passes its checker — but there is no `v1.8.10` git tag, no
commit carrying this work yet (the working tree currently shows it as uncommitted changes),
and no GitHub Release, so there is nothing on record yet that could be reverted to and
nothing installable exists outside this machine. Treat everything below as real but not yet
durable.

Torches, and a lighting model that finally behaves the way Minecraft's does — light
spreading properly from one chunk into the next, sky light and torch light added together
instead of one simply winning. It is also the release that fixes a bug that froze a real
console solid, which has nothing to do with torches and is the most important thing in it.

**Added.** Torches: placeable, breakable like everything else, light the area around them
at brightness 14, the same as Minecraft's. Light now crosses chunk boundaries — block light
used to stop dead at the edge of the chunk it was in, so a torch near an edge lit its own
chunk to 14 and the chunk beside it to 0 with a hard seam; it now hands off properly, 14 on
one side and 13 on the other. A fake-shading option, off by default, costing almost nothing
on this hardware.

**Changed.** Sky light and torch light are added together now instead of the game taking
whichever was brighter — which is why a torch in daylight now visibly brightens what it is
near, and a torchlit room at night no longer looks like a room at dusk. Daylight is
essentially unchanged.

**Fixed.** The whole console could freeze — reported on real hardware at render distance 5,
frame rate dropping and then the system locking up completely a couple of chunks from spawn.
The cause was a wait in the wrong place: every frame the game hands chunk geometry to the
graphics chip and immediately starts rebuilding chunks in that same memory, so it has to wait
for the chip to finish reading first — and it was waiting on the screen refresh instead of on
the chip. Those look identical until a frame runs longer than a screen refresh, which is
exactly what render distance 5 does. There is an expected cost, not a second bug: the old
behaviour let chunk building overlap with drawing for free, and that overlap *was* the bug,
so frame rate at render distance 5 may be lower than before — getting the overlap back safely
is separate work, deliberately left out of this release. Also fixed: a new test file under
`source/world/` was missing the guard that keeps test code out of the console binary, so the
game could not be built for hardware at all; the host suites never caught it because they
never link the console target.

**Not delivered in this version, despite earlier plans for it.** Water bobbing returning
(calmer than before) and a broader "shaders option" (fake directional lighting and fake
water reflection beyond the fake-shading toggle above) do not appear in the actual
`CHANGELOG.md` entry for 1.8.10 and were not found in the tree; both have slipped rather
than shipped.

**Notes.** The chunk pop-in reported against v1.8.7 is still not fixed here. The freeze fix
is proven at the binary level — the compiled object now references the call that really
waits, no longer the one that doesn't — but it has not been confirmed on real hardware,
because that needs a 3DS.

### v1.8.11 — Caves — planned

**Added.** Cave generation in the legacy-console-edition style — long connected tunnels
and open ravines, not the isolated pockets a pure noise field tends to produce. Legacy
console caves were Java's own Beta/early-1.x carver, ported and then frozen; the shape
worth copying is a walked path, not a static field: a per-region roll decides how many
tunnel systems start there (nested, not flat, so most regions get 0–2 and a long tail get
more); each tunnel's yaw and pitch drift with momentum rather than jittering
independently every step, so the path curves smoothly; its carve radius is smallest at
both ends and largest in the middle; and partway along a long enough tunnel it can fork
into further tunnels, which is what produces a branching network rather than a lone
corridor. Ravines reuse the same walked-path machinery with different tuning — far less
drift, roughly straight, about ten times rarer, no branching.

**Added.** Lava pools below a fixed floor of y=10 (this threshold needs no rescaling for
Blocksmith's world — the classic era it comes from was already 128 blocks tall with sea
level one block off from Blocksmith's own), and underground water via a straightforward
flood-to-water-table rule reusing the fluid simulation that already exists.

*A real architectural note, not a console difference:* Blocksmith's current cave
generator (`worldgenIsCave()`) is a pure noise field, answerable from one block's own
coordinates alone — which is why it can be corner-cached per column (v1.8.7). A true
walked carver breaks that: a tunnel that starts in one chunk can carve blocks in several
neighbouring chunks, so generating one column correctly would need to know about tunnels
that started elsewhere, and nothing can be persisted to make that cheaper — the loaded
column ring already sits at 88.7% of the 12 MB budget at a New 3DS's own render-distance
ceiling. The recommended first phase is retuning the existing noise field toward the
legacy carver's connectivity and character (already measured at 99.2% of carved volume
sitting in connected systems larger than 100 blocks) rather than building a true carver
outright, with the carver kept as a later, optional phase if retuning does not read as
close enough.

*Not part of this version, on the evidence:* zombie/skeleton spawning depends on an
entity/mob system that does not exist anywhere in the tree today — no entity, no AI, no
combat, no spawning, and no rendering for anything that moves under its own logic. That
is scoped separately, in `docs/ROADMAP.md`'s own later versions.

Full research, with sources and citations: `docs/research/caves-legacy-console.md`.

### v1.8.12 — Ores — planned

**Added.** Ore generation on the legacy distribution — coal high and common, iron below
it, gold, redstone, lapis and diamond in the deep, each in veins rather than singles.
Tool tiers, so an ore means something to reach.

`docs/ROADMAP.md`'s entry for this version is a short paragraph with no dedicated
research brief behind it yet — treat the ore-tier ordering above as the settled part and
exact vein sizes/depth bands as not yet specified.

### v1.8.13 — Survival — planned

**Added.** Health, hunger, fall damage, eating, death and respawn.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet.

### v1.8.14 — Animals — planned

**Added.** The entity system, and the first animals on it: pigs, cows, chickens, sheep.
They wander, they can be killed, and they drop meat.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet. This is the entity/mob framework v1.8.11's cave-monster work
and v1.8.16 both depend on.

### v1.8.15 — Furnace — planned

**Added.** A furnace block with fuel and a smelting timer. Raw meat becomes cooked meat,
ore becomes ingots.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet.

### v1.8.16 — Monsters — planned

**Added.** Zombies and skeletons, spawning in the dark and in caves, on the light and
space rules that make a torch worth placing.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet. Depends on v1.8.14's entity system existing first.

### v1.8.17 — Sound — planned

**Added.** A real audio system — footsteps that know what surface they're on, block
breaking and placing per material, water entry/exit, crafting, UI feedback, sourced under
licences that permit any use and credited in the repository (`ROADMAP.md`'s own stated
exception to "nothing borrowed": sound may be sourced, everything else stays
procedurally generated). There is no audio subsystem in the tree today at all — no ndsp
use, no mixer, no romfs audio folder — so this is new work, not a wire-up of something
half-built. Roughly 19–20 distinct clips cover everything the code can actually trigger
today (grouped by acoustic class: earth/stone/wood/foliage for breaking and placing,
those four plus snow/ice for footsteps, water entry/exit, one craft sound, and 2–3 UI
sounds); CC0 packs from Kenney and OpenGameArt plausibly cover the whole list.

*Two things named in `docs/ROADMAP.md`'s own wording — "animals" and "ambience" — have no
hook to trigger them yet*, since neither a mob system nor a running world clock exists in
the tree today; the research recommends descoping them to whichever version actually adds
those systems, rather than building sound for something that cannot yet make a sound.

*A real hardware caveat, not a console difference:* a CIA install (how Blocksmith ships)
needs a one-time `dspfirm.cdc` dump on the SD card to get any audio at all — the emulator
used for this project's own automated verification does not need one, so passing tests
under emulation would not prove the real-hardware path works. A hardware playtest with no
DSP firmware dumped, to confirm the game stays silent rather than crashing when it is
missing, is called out as a required step for this version specifically.

Full research, with sources and licence terms: `docs/research/audio.md`.

### v1.8.18 — Storage and quality of life — planned

**Added.** Chests. Stack splitting and merging, shift-move, and the small conveniences a
game gets tiring without.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet.

### v1.8.19 — The new interface — planned

**Changed.** The menus and inventory redrawn around a long horizontal bar of categories,
with the focused category's items descending in a list below it — the interaction shape
the legacy console crafting menu and the PS3 system menu (the XMB) both independently
converged on: left/right steps between categories, up/down steps through the current
category's list, one physical control per axis. **Referenced as interaction design only —
no art, layout metric, or icon is taken from either.** Every visual asset stays
procedurally generated by the project's own `tools/*.py` scripts, matching how every
other texture in the game is already produced.

A proposed starting bar: **Craft** (the existing recipe list, re-skinned into the new
shape), **Blocks** (a new full-screen block list reusing the existing atlas and font
verbatim), and **Inventory** — with **Debug** deliberately kept off this bar entirely and
reachable only through the pause menu, unchanged from today. Cost is not a concern: the
richest version of this proposal is estimated at roughly 183 worst-case sprite quads
against the existing UI's own documented headroom of not-quite-2× inside the current
1024-quad sprite budget, so `SPRITE_MAX_QUADS` does not need to rise for anything
proposed here. The battery-indicator blink at one bar — a separate ask associated with
this version — turns out to already be built and shipped, as of v1.8.3; the only
remaining step is confirming it on real hardware, since this pass could only verify it by
reading the code.

**Two research passes exist for this version.** `docs/research/interface.md` covers the
interaction-design analysis of both references in depth but stops short of a concrete
Blocksmith-specific proposal (its own later sections are left as placeholders).
`docs/research/ui-skin.md` is the later, complete pass — it covers the same two
references plus a full read of Blocksmith's current UI code, a concrete screen-by-screen
design, a quad-cost budget, and a recommended six-phase build order starting with the
already-built battery blink and the debug-menu biome readout/neon toggle before the bar
itself is touched. Treat `ui-skin.md` as the authoritative source for this version; read
`interface.md` only for its Part A/B reference analysis.

### v1.9.0 — Redstone — planned

**Added.** Wire, power, levers, buttons, pressure plates, doors, pistons.

`docs/ROADMAP.md` gives this version one short paragraph; nothing beyond it has been
researched or specified yet.

### v1.9.1 — The other worlds — planned

**Added.** Two more dimensions with their own names — the fire one and the end one, named
but not copied from anything — each with its own generator, its own blocks and its own
way in.

**Candidates for later**, since more were asked for and not committed: a shattered-sky
dimension of floating islands reached by falling; a frozen deep reached from a cave; a
mirrored version of the overworld where it is always night.

---

## Two questions, answered directly

**Which version adds Beta 1.7.3-style Minecraft terrain?** **v1.7.0.** It shipped the
density-field generator with real caves and overhangs, on Beta's own 5×5×17 sample
lattice, at Beta's own 128-block world height and sea level. Biome identity — six biomes,
each with its own ground, plants and tree shape, and each world getting its own seed —
followed two versions later in v1.8.3. `v1.8.7`'s name ("Terrain") is misleading on this
specific question: that version does not add Beta terrain, it tunes the amplitude table
of the terrain v1.7.0 already shipped, across a wider seed sweep than the two seeds it
was originally tuned against.

**Which version improves chunk render distance for Old 3DS and New 3DS?** **v1.8.5.** It
is a genuine split between the two consoles: a New 3DS can be set up to render distance
5, an Old 3DS stays at 3 (what its memory holds), and the chunk mesh pool is now sized at
runtime per console instead of being fixed at compile time for the smaller one. Both
consoles also get a real improvement at whatever distance they already ran at: the old
fog closed the world in at about 14 blocks half-visibility no matter the render-distance
setting, on either console; the new depth-based fade scales properly with distance —
28.5 blocks half-faded at distance 3 (available on both consoles), up to 47.5 at distance
5 (New 3DS only).
