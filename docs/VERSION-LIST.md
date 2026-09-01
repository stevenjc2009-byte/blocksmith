# Blocksmith version list

Every version Blocksmith has had, oldest first, followed by every version currently
planned, in one continuous order. This is an index — short entries pointing at where
the real detail lives, not a rewrite of it.

Full detail on everything already shipped is in `CHANGELOG.md`. Full detail and
reasoning on everything not yet built is in `docs/ROADMAP.md`.

**Current version: 1.8.5** (see `source/version.h`). Everything up to and including it
has shipped. Everything after it is a plan, not a build — order, scope, and whether a
given version ships at all can still change before it does.

---

## Released

### v0.1.0 — First release — released

Playable single-player: seeded infinite world, chunked terrain with caves, breaking
and placing blocks, a three-recipe crafting table, saving with torn-write recovery,
and an installable `.cia`. Never run on real hardware.

### v0.2.0 — Loading screen and updater — released

An in-app updater (Options → Check for Update). A real loading screen for world
creation, replacing what looked like a freeze. World creation sped up 4.7x by turning
off an on-console self-test that had been eating 99% of the load time. A watchdog
that writes a report if the main thread stops responding.

### v1.1.0 — Multiplayer works end to end — released

Joining a server puts you in the server's world, your edits reach it, other players'
edits reach you, and a lost connection is detected and reported instead of leaving you
in a HUD that lies. Not tested on real hardware.

*v1.1.1 through v1.2.5 are fourteen versions in a row chasing one hardware-only freeze
that never once reproduced in an emulator. Most of them are diagnostic pre-releases —
instruments built to answer one question about where the console was stopping, not
fixes. The freeze was "fixed" twice: a wrong fix shipped in v1.2.0 and was retracted in
v1.2.1; the real cause was found in v1.2.5.*

### v1.1.1 — diagnostic: draw bisect — released

First diagnostic pre-release. A six-arm bisect removes one part of the frame per boot
to narrow down where the freeze happens. No fix.

### v1.1.2 — diagnostic: draw-stage breadcrumb — released

Records exactly where inside chunk drawing the console stopped, plus a second, finer
bisect. Also fixes a bookkeeping bug in v1.1.1's own bisect. No fix yet.

### v1.1.3 — diagnostic: bisect harness fix — released

Fixes the diagnostic itself: v1.1.2 was reading a leftover file from v1.1.1 and had
been skipping all drawing the whole time, so it "survived" without ever running the
real test. No game fix.

### v1.1.4 — diagnostic: draw guard, finer breadcrumb — released

Extends the breadcrumb to cover every GPU submission in the frame, and validates each
chunk draw call before issuing it. Rules out command-buffer overflow and missing cache
flushes as causes. No fix yet.

### v1.1.5 — diagnostic: GX queue wait — released

Narrows the freeze to the wait on the GPU's command queue — not frame pacing, not a
dead interrupt thread. No fix yet.

### v1.1.6 — diagnostic: queue contents — released

Reports exactly which queued GPU command the console is stuck on. No fix yet.

### v1.1.7 — diagnostic pre-release — never published

Built, then withdrawn in favour of v1.1.8, which carries the same work plus a test
battery. **There is no v1.1.7 release, tag, or CIA** — it exists only as a step in the
changelog's record of the investigation.

### v1.1.8 — diagnostic: GPU pre-flight + validator — released

A GPU self-test at boot and a per-frame command-list validator that flags bad
addresses and NaNs. Diagnostic pre-release, not a fix.

### v1.2.0 — freeze "fixed" (cache flushes) — released

Adds missing `GSPGPU_FlushDataCache` calls project-wide and bounds the GPU wait so a
wedge no longer hangs the console outright. Believed at the time to be the fix for the
hardware freeze. **It was not** — see v1.2.1.

### v1.2.1 — freeze actually fixed (CPU/GPU race) — released

Real cause found: the CPU was rewriting chunk vertex buffers while the GPU was still
drawing from them. Adds a wait that serialises the two. Also fixes two latent
use-after-free crashes in shader teardown. v1.2.0's cache-flush theory is explicitly
retracted here as not the cause — it stays in, but not for that reason.

### v1.2.2 — diagnostic: boot fencing, real reporting — released

Fixes the console mixing up which boot a diagnostic file belongs to, restores full
crash reporting, and fixes a checker that was misreading GPU uniforms as the wrong
float format — which had been manufacturing false "NaN" reports.

### v1.2.3 — diagnostic: draw bisect, round two — released

Re-enables a finer bisect that cuts inside the chunk-draw call itself, now
version-stamped so it cannot misread a stale probe file left by a different build.

### v1.2.4 — experiment: vertex shader indexing — released

Replaces the world shader's one indexed uniform fetch with a fixed one, as a test —
not a confirmed fix — deliberately flattening lighting so the build is visibly the
experimental one.

### v1.2.5 — freeze fixed for real (vertex misalignment) — released

The actual cause, thirteen builds in: a 4-byte vertex attribute was being fetched from
an unaligned byte offset. Real GPU hardware cannot do that; an emulator tolerates it
silently, which is why nothing here ever reproduced off a real console. Fixes the
vertex layout and adds a compile-time assert so it cannot regress unnoticed.

### v1.2.6 — remote edits now visible — released

Fixes multiplayer: another player's block edits changed the world but not what you saw
on screen, because only local edits marked their chunk for a rebuild. Adds a crosshair.

### v1.2.7 — pause menu — released

SELECT opens a real pause menu — render distance, a memory readout — instead of
requiring a trip back to the title screen. Adds wooden planks. Rewrites how a joining
client receives world edits so the amount is bounded by render distance instead of by
the server's whole history. Fixes 289 columns that stayed allocated for the whole
session after a boot-time memory check.

### v1.3.0 — server-owned inventory — released

On a server, your inventory now lives on the server instead of in each console's own
RAM — closes a duplication exploit and stops carried items vanishing on sleep.

### v1.3.1 — server inventory: actually works — released

Fixes two ordering bugs that meant v1.3.0's server inventory did nothing at all — every
rejoin came back empty.

### v1.4.0 — remapping, sleep, minimap, debug menu — released

Full control remapping. A sleep/lid handler that pauses the game properly instead of
running on into a suspended console. A live minimap with fog of war. A debug menu with
render distance, live stats and placeholder rows for later systems. Fixes a broken
protocol pin that stopped a fresh clone from building.

### v1.5.0 — saved players, adaptive lighting — released

Client half of server-side saved player state (position, meters — held dark until the
server side exists to fill them). Adds a real lighting engine: sky light baked per
vertex, computed dynamically on a New 3DS and identical to before on an Old 3DS, which
runs neither the new engine nor its shader.

### v1.5.1 — repair release — released

Removes the minimap (it crashed a real console). Fixes the debug menu stepping render
distance on open, an unreachable controls screen, mis-clocked timing after sleep on a
New 3DS, lost building on lid-close, and a multiplayer session left as a zombie after
sleep.

### v1.6.0 — server-defined blocks, render rebuild — released

Finishes the block registry: a server-defined block can now actually be seen, mined
and synced correctly. Adds non-cube block shapes and greedy face merging (15–21% fewer
quads). Rebuilds the texture atlas as a one-tile-wide strip to make that merging
possible. Fixes several defects this exposed, including a registry sync that verified
nothing and a server block that deleted itself the moment it was mined.

### v1.7.0 — new terrain generator — released

**If you had v1.6.0, it did not start — this release fixes that first.** Adds a
Beta-1.7.3-style density-field generator with real caves and overhangs, water (still,
non-flowing), and tall grass. Worlds record which generator made them, so existing
worlds keep their old terrain.

### v1.7.1 — repair and optimization — released

Fixes spawning inside your own blocks after a reload, players returning to the wrong
position, unbreakable tall grass, and repeated region-file rereads. Four measured
performance fixes to the mesher, column install, horizon culling and light-fill. One
of the four originally claimed here (region compaction) was later found to still be
unwired — corrected under v1.8.2 below.

### v1.8.0 — water moves — released

Water is simulated rather than static: it spreads, falls, finds its level, and drains
away when its source is removed.

### v1.8.1 — breaking takes time — released

Per-block break times (hardness) replace instant breaking, with a crack overlay and an
interruptible hold. The core block registry's checksum changes as a result — the
matching server release is required.

### v1.8.2 — water made to look and feel right — released

Water now spreads gradually instead of instantly, sits below the block rim, is
see-through, and settles instead of bobbing at the surface. Texture atlas expands from
15 to 64 usable slots. Also documents, after the fact, that region compaction (the
write-amplification fix v1.7.1 claimed) was actually wired up in this release — not
that one — correcting three releases' worth of changelog entries.

### v1.8.3 — biomes — released

Six biomes — tundra, taiga, plains, forest, desert, jungle — each with its own ground,
plants and tree shape. Worlds get their own terrain seed instead of sharing one. A
client that joins a server running a different generator is now refused rather than
silently desyncing.

### v1.8.4 — New 3DS treated as a New 3DS — released

*This is a genuine Old/New 3DS split.* The 804 MHz clock, L2 cache, larger memory mode
and third CPU core are actually requested and used now — they were written in an
earlier version but gated behind a flag that defaulted off. Background world work
moves onto the New 3DS's extra core; an Old 3DS is unchanged. Also: you now press jump
to climb out of water, and failed downloads get a direct retry button.

### v1.8.5 — render distance — released, CURRENT VERSION

**The version installed today.** *Another genuine Old/New 3DS split:* a New 3DS can now
be set to render distance 5, an Old 3DS stays at 3. Fixes the real ceiling on how far
you could see — the old fog closed the world in at about 14 blocks at any render
distance; it is now a proper depth-based fade that scales with the setting (28.5 blocks
half-faded at distance 3, 47.5 at distance 5). The chunk mesh pool is now sized at
runtime per console instead of fixed at compile time for the smaller one. Fixes chunks
that could go permanently missing at a wide render distance.

---

## Planned

Everything from here down has not been built. These are plans, not shipped features,
and their scope, order, and even whether a given version ships at all can still change.
Full reasoning for each lives in `docs/ROADMAP.md`.

### v1.8.6 — Speed — planned

Plans to cut the biggest confirmed worldgen waste: about 75% of trees considered during
generation are thrown away because reachability is not checked until after the
expensive part of the work. Also plans to look at whether New 3DS work budgets are
worth raising — measurement so far says probably not, since a millisecond is a
millisecond on both consoles. Several originally-planned optimisations for this
version were re-measured and dropped as no-ops before any code was written (chunk
queue ordering, edit-relight coalescing, per-file compiler flags, recipe matching,
camera math) — see the "Removed from this version" section in `docs/ROADMAP.md`.

### v1.8.7 — Terrain — planned

Despite the name, not "the Beta terrain" — that already shipped, across v1.7.0 and
v1.8.3. This version is about tuning the amplitude table that already exists (currently
tuned by eye against two seeds) across a real seed sweep, and adding a real ARM11
timing reading for column generation, which does not exist yet.

### v1.8.8 — Biome identity — planned

Plans to colour terrain by tint rather than by separate blocks wherever possible, with
real per-biome blocks (wood, leaves, planks, ground cover) kept only where the material
genuinely differs. Plant variety — short and tall grass, ferns, flowers — apples from
leaves, redone dead bushes, and a fix for currently-unbreakable cactus. Also plans to
lift the block ID ceiling, which most later versions on this list depend on.

### v1.8.9 — Sky and weather — planned

A day/night cycle matched to Minecraft's own timings and light curve. Weather decided
by biome temperature and altitude — rain above a threshold, snow below it, deserts and
savannas get neither. Snow settles up to exactly one block deep and no further.

### v1.8.10 — Light — planned

Torches with smooth per-corner lighting. Missing particle effects, starting with water
splashes. Water's gentle surface bob returns, calmer than before. A cheap fake-lighting
and fake-reflection shader option — plans to drop it rather than ship it badly if it
cannot hold frame rate on an Old 3DS, and it may end up a New 3DS-only option even if
it ships.

### v1.8.11 — Caves — planned

Legacy-console-style cave generation: long connected tunnels and ravines carved by a
damped random walk, not isolated pockets. Lava pools and underground water.

### v1.8.12 — Ores — planned

Ore generation on the legacy distribution — coal, iron, gold, redstone, lapis, diamond
— in veins rather than singles, plus tool tiers to make reaching them mean something.

### v1.8.13 — Survival — planned

Health, hunger, fall damage, eating, death and respawn.

### v1.8.14 — Animals — planned

The entity system and its first inhabitants — pigs, cows, chickens, sheep. They
wander, can be killed, and drop meat.

### v1.8.15 — Furnace — planned

A furnace block with fuel and a smelting timer: raw meat becomes cooked meat, ore
becomes ingots.

### v1.8.16 — Monsters — planned

Zombies and skeletons, spawning in the dark and in caves, on light and space rules that
make placing a torch matter.

### v1.8.17 — Sound — planned

A real audio system — footsteps by surface material, block break/place sounds per
material, animal and water sounds, ambience — sourced under licences that permit any
use, credited in the repository.

### v1.8.18 — Storage and quality of life — planned

Chests. Stack splitting and merging, shift-move, and the smaller inventory conveniences
a game gets tiring without.

### v1.8.19 — The new interface — planned

Menus and inventory redrawn around a long horizontal bar of options, referenced from
the shape of the legacy console's own crafting menu and the PS3 system menu — no
assets, layout metrics or icons taken from either.

### v1.9.0 — Redstone — planned

Wire, power, levers, buttons, pressure plates, doors, pistons.

### v1.9.1 — The other worlds — planned

Two more dimensions, each with its own generator, blocks and way in — a fire-themed one
and an end-themed one, named but not copied from anything. A shattered-sky dimension, a
frozen deep, and a mirrored always-night overworld are candidates for later, not
committed.
