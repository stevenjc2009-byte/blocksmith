# Changelog

All notable changes to Blocksmith. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/).

## [1.8.2] - 2026-08-25

Water was asked for again — four things in one go — and this is the release that answers all
four. 1.8.0 made water move, but it moved *instantly*, filled its block edge to edge, was as
opaque as stone, and juddered under you when you swam up to the top of it. The first three
were wrong for the same reason: water that behaves like a solid reads like a solid. The
fourth was a feedback loop at the waterline. From 1.8.2 water spreads at a pace you can
watch, sits a little below the rim of its block, can be seen through, and holds you still
when you surface in it.

Worlds made in 1.8.1 open unchanged and no block id moved — see Compatibility.

### Changed

- **Water now spreads gradually instead of arriving all at once.** A flat pour that used to
  finish in 9 ticks takes 41; a 7-block spread reaches its far edge at tick 34 rather than
  tick 7. **The settled result is identical** — the same 112 flow cells in the same final
  layout — so nothing about where water ends up has moved, only how long it takes to get
  there. That distinction is the whole point of the change: a pour you cannot watch is a pour
  that looks like a texture swap, and the equivalence of the end state is what makes it safe
  to slow down.
- **The water surface sits 7/8 of a block high rather than filling the block.** There is now a
  visible lip where water meets land instead of the two meeting flush, which is what made a
  shoreline read as a wall of blue blocks. Only the full and near-full levels move; the
  shallower flow levels are unchanged, because they were already drawn below the rim and
  lowering them again would have opened a gap under the surface.
- **Water is see-through.** Alpha 0.70, drawn in a transparent pass rather than as an opaque
  block, so a lake bottom, a submerged wall and anything you have built underwater are all
  visible from above.
- **The upward swim speed is now exactly 3.0 blocks/second, about 4.6% faster than 1.8.0's
  2.867.** Written down rather than left to be noticed, because it is a real change to how the
  game feels — but it is not a tuning decision. 3.0 was always the intended constant; the old
  figure was gravity leaking into the clamped value every frame, so the fix is that the
  intended constant is now the actual one and the speed change is its consequence. In the same
  vein, dropping into water from a height now leaves you floating higher than it used to
  rather than bouncing you back out of it.
- **The texture atlas holds 64 block textures instead of 15**, which is room for many more
  kinds of block than the game has ever had — twelve slots are in use today, one is
  permanently reserved as the missing-texture marker, and 51 are free. The old ceiling was
  never the sheet: `MeshVertex.v` held an atlas pixel row, and a `uint8_t` capped the sheet at
  256 px, which is 16 slots with the top one's upper edge falling off the end. That byte now
  counts slot edges rather than pixels, the `TILE_PX` factor moved into the shaders'
  `uvScale.y`, and the sheet grew to 16x1024 — the vertex is still 8 bytes. **1024 is where
  this stops**: it is the PICA200's maximum texture dimension, measured from citro3d 1.7.1's
  `checkTexSize` rather than picked, so 64 slots is the most this one-tile-wide design can
  ever have and not a step towards a larger sheet later.

### Fixed

- **Swimming up to the surface settles instead of bobbing.** Hold the swim button at the top
  of a lake and you rise, stop, and float there with your head clearly out of the water —
  0.485 blocks of it — rather than the camera juddering up and down through the waterline.
  Vertical drift while floating is 0.000018 blocks, against 0.223333 before. Nothing about
  swimming speed underwater or the rate you sink is different to the eye. The old behaviour
  was not one broken rule but three correct ones meeting badly: a hard clamp on the upward
  swim speed, a step change in gravity at the surface, and a submersion test with no middle
  state, which together formed a relay. The first attempt at fixing it pulled the vertical
  speed towards zero and looked better on the obvious measurement — travel down to 0.022027
  blocks — while crossing the waterline **140 times in 399 frames** where the shipped fix
  crosses it **zero** times, which is a strobing water plane rather than a bob and is worse.
  What ships instead suspends gravity while the button is actively pulling, so the resting
  point is a real fixed point with nothing re-establishing the error. Zero crossings holds at
  60, 30 and 20 fps.
- **Standing exactly on the seam between two chunks could blink a hole in the world.** Walk
  across the boundary and a chunk that should have been in front of you was culled for a
  moment — the terrain simply was not drawn, then came back. It is easiest to catch
  underground, where a cave wall vanishes and you are looking into nothing. Only the frames
  spent precisely on the seam were affected; nothing was wrong with the world itself or with
  what was saved.
- **The CIA has a banner chime again.** The Home Menu icon played nothing at all; it now plays
  its own sound like every other title.

### Compatibility

- No save-format break, and no block id moved. `BLOCK_COUNT` is unchanged at 8, so a 1.8.2
  client and a 1.8.1 server still agree on the block registry — none of the water changes
  above is a block, a level or a wire field, only how the existing ones are stepped, drawn
  and swum in.
- **The water and tall-grass block ids are now pinned to literals** rather than derived from
  their position in the registry. Both sit deliberately outside `BLOCK_COUNT`, and the old
  form meant that adding one core block would have shifted them silently — a save-format break
  with no compile error and no failing test in front of it.

### Corrected

- Several source comments describing water's old instant spread and its full-height surface
  now say what the code actually does. They were accurate when they were written and stopped
  being accurate in this release, which is exactly the kind of comment that gets believed
  later.

### Testing

- The host suite is now about 5,100 world checks, and gained 90 checks covering the chunk-cull
  boundary case above so the seam cannot regress unnoticed. The server suite is 236 checks.

### Known limitations

- **Tapping the swim button repeatedly at the surface still bobs you**, because letting go of
  it restores gravity; holding it is what settles you. That is the intended behaviour rather
  than a remnant of the bug above — the button holding you still is exactly what the button
  is doing.
- Every swimming figure quoted above is measured on the host test suite. None of it, and
  nothing else in this release, has run on a real console.

## [1.8.1] - 2026-08-24

Breaking a block takes time. Until now every block came out on the frame the button went down,
which made stone and dirt and wood indistinguishable and made a mis-aimed press instantly
destructive. From 1.8.1 the break button is HELD, each block has its own hardness, and a crack
spreads across the face while you work at it.

Worlds made in 1.8.0 open unchanged and no block id moved. One thing DID move and it matters for
multiplayer — see Compatibility.

### Added

- **Per-block break times.** Hardness in ticks at 20 TPS, so the same block takes the same real
  time on a console holding 60 fps and one at 30 — a frame-counted timer would have made stone
  quicker to mine on an emptier screen, which is the exact defect the tick clock exists to
  prevent. Tall grass 1 tick, leaves 4, sand 10, grass and dirt 12, wood and planks 40, stone 45.
  Tuned for a game that has no tools in it yet; `world/mining.c` carries the multiplier seam they
  will slot into, deliberately empty until then.
- **A crack overlay.** Eight progressive stages drawn on the block being mined, on their own
  small texture (16x128, RGBA5551, about 4 KB) and their own draw pass — never through the chunk
  mesher, because break progress advances twenty times a second and a chunk rebuild costs about
  2.1 ms, which would spend the whole Old 3DS frame budget rebuilding geometry that is already
  correct.
- **Breaking is now interruptible.** Let go, look away, or have the block change under your
  crosshair mid-hold, and the progress is thrown away rather than carried over to whatever you
  look at next. The block's id is part of the progress identity, not just its coordinates, so a
  90%-mined stone cannot finish instantly as the dirt a remote player put in its place.

### Changed

- **Break is level-triggered, place is still edge-triggered.** One press still puts down exactly
  one block however long the button is held; only the break reads the button's state rather than
  its edge.
- **The simulation clock now advances before the edit rather than after it.** A held break
  consumes those same ticks, and advancing afterwards would hand every frame the previous frame's
  tick count. Water is indifferent to which side of the edit it steps on, so the whole block moved
  rather than being split in two.
- **A block the bag cannot hold is refused at the START of the hold**, not at the end. Watching a
  full crack animation play out on a block that was never going to move reads as the game being
  broken rather than as the block being unbreakable.

### Compatibility

- **The core block registry's CRC moved, 0x72A8 to 0x4066.** The wire layout did NOT change —
  `hardness` has been byte 25 of the 28-byte record since the field was added, and there are still
  ten core rows — but the values in that byte went from ten zeroes to real numbers, and the
  registry hash covers content. A 1.8.1 client joining a server older than 1.8.1 fails the
  registry match, falls into the bounded fetch retry and finishes the session unsynced: degraded,
  not fatal, and it clears the moment the server is updated. **The matching server release is
  1.8.1 — update both.**
- No save-format break. `BLOCK_COUNT` is unchanged at 8 and no block id moved.

## [1.8.0] - 2026-08-24

Water moves. 1.7.0 put water in the world and 1.7.1 taught the player to swim in it, but until
now it was scenery: a still block that sat wherever the generator left it. From 1.8.0 it is
simulated — it spreads, it falls, it finds its level, and it drains away when its source is
taken.

This is the second half of the simulation core. The 20 TPS clock that drives it, the lighting
repair and the buoyancy that lets you swim all shipped in 1.7.1; water is what that clock was
built to run.

Worlds made in 1.7.x open in 1.8.0 unchanged, and no block id moved — see Compatibility.

### Added

- **Water physics.** Sources, flow levels 1..7, spread, and drainage when the source is removed.
  A cell holding water is a source when the flow map has no entry for it and a flow cell when it
  does, so an ocean occupies no map slots at all — absence-means-source is what makes the sea
  free. Water falls a block wide, pools where it lands, and a falling column does not weaken.
- **Water is drawn at its real depth.** Eight flow levels render as eight surface heights, with a
  wall between any two unequal neighbours and no seam at all between equal ones. This cost no
  texture-atlas slot: the height drop packs into three spare bits of the mesher's normal field.
- **A remesh on a level change alone.** A cell whose water merely got shallower moves no block,
  so nothing in the old edit path would have noticed it. Water now asks for its own chunk rebuild
  when only the level changed — without a relight, deliberately, since no block moved and the
  light would recompute an identical answer at the most expensive moment in the frame.

### Compatibility

- No save-format break, and no block id moved. `BLOCK_COUNT` is unchanged at 8, which matters
  because the server uses it as its item ceiling and because the shared block registry's CRC is
  part of the join handshake — water levels deliberately live in a side map rather than as new
  block ids, since seven new rows would have desynced every joined session permanently.

  **Correction (2026-08-25).** This entry originally said the server "compares a CRC of the
  shared block registry on connect". It does not, and never has — verified by reading the code
  on 2026-08-25. The server only *announces* its fingerprint, once: `send_registry_info()`
  (`deps/blocksmith-server/game/bsgame.c:569-576`) sends `{rev, count, crc16}` from
  `handle_join()` (`bsgame.c:915`). `BS_APP_REGISTRY_INFO` is server-to-client only
  (`proto/bs_proto.h:291`) and the only registry message going the other way is
  `BS_APP_REGISTRY_FETCH`, two bytes of type plus `first_index` (`bs_proto.h:292`). No CRC ever
  travels client-to-server, so the server has nothing to compare and performs no comparison.
  The whole gate is client-side, in `registryMatchesInfo()` at `source/net/networld.c:210-216`.

  A mismatch also cannot repair itself. The client retries `REGISTRY_FETCH` four times at 250 ms,
  but a `REGISTRY_DEFS` batch can only carry *dynamic* rows — `registryDefUnpack()`
  (`source/world/registry.c:363`) rejects any id below `REG_ID_DYN_LO` at `:367`, so core rows
  are compiled in and untransmittable by construction. After the 2000 ms deadline the client
  enters the world degraded, permanently for that session. That degraded state is effectively
  invisible to a player: the "Joined - syncing block table..." row vanishes on the deadline
  exactly as it would on success, and the only lasting indicator is a `!` on the debug overlay,
  which is off unless the player turned it on.

  This is precisely why a **core** registry addition forces a matching server release, shipped
  first or simultaneously and never client-first: nothing at runtime detects it for the player,
  nothing at runtime repairs it, and the server accepts the join either way. `BS_PROTO_VERSION`
  (`bs_proto.h:56`, enforced at `bsgate.c:867` and `bsnet_transport.c:570`) is a completely
  independent gate and is not involved.

  The same false claim is carried by blocksmith-server's published `v1.8.1` tag body and its
  commit `c77db54`. Both are published history and are deliberately left unrewritten.
- Water still cannot be carried or placed by the player: it sits outside the hotbar's id range by
  design, and 1.8.0 does not change that.

## [1.7.1] - 2026-08-24

A repair release for the four things steve reported playing 1.7.0, plus the optimisation pass
he asked for alongside them. No new terrain, no save-format break: worlds made in 1.7.0 open
in 1.7.1 unchanged.

One of the four did not survive contact with a measurement, and the entry below says so rather
than claiming a fix that was never made.

### Fixed

- **Reloading a world dropped you inside your own blocks.** Reported as "the chunk I'm in
  doesn't get loaded in… the other chunks do", and it looked exactly like that, but nothing was
  failing to load. `worldgenHeight()` predicts terrain from the seed and the generator version
  alone and never reads the live world, so a player who had built anything at the spawn cell was
  placed back at the *original* ground height — inside their own blocks, camera included, with
  back-face culling drawing nothing for the geometry immediately around them while columns
  further out drew normally. `worldStandingY()` now steps the spawn up until it clears solid
  blocks, capped at `WORLD_HEIGHT - 2`. A second, independent defect found in the same place:
  `chunkRenderReleaseAll()` now clears the per-world mesh-slot table on world exit, which was
  being carried into the next world.
- **You now return to where you actually were, facing the way you were facing.** A 32-byte pose
  sidecar per world (magic, version, CRC32, x/y/z/yaw/pitch), written on exit and read on entry.
  It is loaded *before* the streaming ring starts, and the ring is centred on the pose's column —
  the obvious ordering, with the ring started first, silently does nothing, because `worldGet()`
  returns air for a chunk that is not resident yet and the un-stick step then has nothing to
  stand on.
- **Tall grass could not be broken.** Swinging at it did nothing at all, silently. The break was
  gated on `inventoryCanHold()`, and `BLOCK_TALL_GRASS` is deliberately outside `BLOCK_COUNT` so
  it can never become a hotbar item. Breaking and dropping are now two separate questions:
  the plant breaks to air and drops nothing until the survival rung gives it a drop. No change to
  `BLOCK_COUNT`, none to the server's block ceiling, and no protocol change.
- **Worlds with saved changes reread the same region file once per column.** A two-entry region
  and directory cache. Per-column read 39.2/43.5/56.0 µs to 3.34/3.34/3.64 µs; one 81-column area
  3.2-4.5 ms to 0.27-0.29 ms; `fopen` calls 32,400 to 2, `dirRead` 32,400 to 2, across 81 columns
  with zero mismatches. Costs 12,656 bytes of `.bss`. A host page cache flatters this — a real SD
  card will show more.
- **Frame-rate drops while walking.** Four separate costs, each measured before and after,
  paired and interleaved in one process rather than as two runs:
  - The mesher tested each cell's six neighbours once per pass and made seven passes. Exposure
    is now computed once per cell into a face mask. Mean per-chunk mesh cost 69.14/68.42/75.64 µs
    to 36.67/32.70/32.36 µs; worst chunk 145-157 µs to 74-92 µs; geometry hash `5bf6000fd3e99fab`
    identical across 719 chunks, 245,820 faces, 983,280 vertices.
  - Installing a generated column cost ~4.36 ms and had been hiding inside the mesh timer, so
    every previous investigation attributed it to meshing. Since `Chunk` became opaque, install
    walked the 4,096-cell buffer three times, the last with a linear palette search per cell. One
    walk now builds the palette and a 256-entry lookup together. Whole install path 104.5 to
    29.8 µs/column; `worldSetChunkAll` 4.2x. Contents hash `a8ed2826ea80538c` identical in both
    arms across eight runs.
  - `horizonHidden()` called `atan2f` roughly 5,800 times a frame, on a console with no hardware
    transcendentals, and was timed by nothing. Every one of those results fed a comparison, and
    an angle comparison is a cross-multiply, so all four are gone. 4.6x on x86 — a floor, not the
    console figure, because x86 has hardware `atan2f` and an ARM11 at 268 MHz runs newlib's
    software one.
  - `scratchFillLight()` made 11,664 out-of-line light-channel calls per chunk build, 11,658 of
    which returned the previous value. 2.9x, output byte-identical (hash `B7506C73` both arms).

  Measured end to end on the emulator over an identical world snapshot restored before each arm,
  same starting chunk: main-thread `cpu_ms` median 2.230 to 1.799, p95 3.330 to 2.476, p99 3.634
  to 2.678, max 3.951 to 2.905, with median triangles drawn unchanged at 16,642 — the work was
  removed, not the geometry. Over-budget mesh frames 26.2% to 11.3% against 1.7.0.
- **A culling bug nobody had noticed**, found while proving the horizon rewrite equivalent. Over
  697,968 enumerated configurations the old and new predicates disagree six times, and in all six
  the *old* one is wrong: it culled a chunk that is genuinely inside the cone, because subtracting
  two ~2.76 radian `atan2f` results loses the 1.5e-6 that decides it. Six holes in 139,514 culls.
- **A save-corruption path in region compaction.** `regionCompact()` read only the live directory
  copy, so a column whose newest payload had been torn by a power cut was dropped from the repack
  *and* its recoverable older copy deleted. It now falls back to the older entry exactly as the
  reader does. Compaction remains deliberately unwired — see below.
- **`visgraphInvalidateTables()` shipped with no caller.** Added beside `mesherInvalidateTables()`
  where a remote registry batch lands. A stale mesher table draws a face wrong; a stale visgraph
  table culls a chunk that should have been drawn, which is a hole in the world.
- **Every visibility timing this project has ever printed was wrong.** `chunkRenderProfileReset()`
  reset the build count but not the tick accumulator, so `chunkRenderVisUs()` reported
  ticks-since-boot as though they belonged to one build.
- **Water no longer holds you under.** Reduced gravity and terminal velocity while submerged, a
  swim-up impulse on jump, and half movement speed in water. `bodyStep()` asks whether it is in
  water itself rather than taking a flag, because a flag every caller must remember to compute is
  a flag one of them will not.

### Added

- **A 20 TPS simulation clock**, shared verbatim by the client and the dedicated server from one
  file, so fluid spread, growth and smelting can be defined in ticks rather than in frames.
  Nothing is on it yet. It also fixed a real server bug on the way: that server's tick rate rose
  with network traffic, because it ticked once per `poll()` return.
- **A coalescing relight worklist.** Rejoining a multiplayer world drained every stored diff for
  a column the instant it installed, and every one fired a full 32,768-cell column relight of the
  same column. Each column is now relit once per frame at most.
- **A world-load profiler**, because the number everyone had been quoting was an artefact. The
  emulator CSV's `frame_ms` is wall clock between two *in-world* frame boundaries, so across a
  reload it spans the pause menu, the title screen, the world list and the test harness's own
  sleeps and screenshots; the row reporting a 20,481 ms "frame" also reports 3.2 ms of CPU and
  4 µs of meshing. `debug/loadprof.c` now times eleven named stages of world entry to
  `sdmc:/blocksmith/load.csv`.

### Measured and not fixed

- **World loading does not double when a world has saved changes.** With the profiler in, a
  reloaded edited world is *faster* than a fresh one — 104.0 ms against 135.4 ms on the host,
  because 25 of its columns come off the card at 92 µs each instead of being generated at
  1,388 µs each. The uncached region path fixed above is the only thing that fits the reported
  symptom, and it is fixed.
- **What world loading actually costs is terrain generation.** On the emulator, entering a world
  takes 8,719 ms, of which 6,906 ms is generating 75 columns at 92 ms each — 79% of the load,
  against 71.6 ms of region I/O for all 81. No fix here: the cheap approaches were tried and
  measured worse. Hand-factoring the noise lattice's eight corner hashes is bit-correct and
  slower (ARM11 codegen 508 to 535 instructions, 94 to 101 multiplies, host -10.5%), so it was
  reverted with the numbers left at the call site. What did land is one integer division removed
  from the fBm inner loop, taking `__aeabi_ldivmod` from 2 calls to 0 in the shipped ARM object —
  8.1 million library calls per load, though what that is worth in cycles is unmeasured.
- **Region compaction is still not wired to anything**, and that is deliberate rather than
  forgotten. It is proven correct and crash-safe on the host (9,289 to 7,134 bytes, arena 3,113 to
  958, 69% reclaimed), but its `remove()` then `rename()` window depends on `.tmp` durability on
  an SD card and on `rename()` behaving on libctru's sdmc devoptab. Nothing in the region writer
  fsyncs, and neither property can be tested anywhere except on the console.
- **Whether the main thread stalls waiting for a save slot is now observable, not answered.**
  `workerSubmitSave()` sleeps the main thread in 1 ms steps waiting for one of two slots, on a
  worker pinned to the same core. The geometry that justified two slots is wrong — one
  axis-aligned ring step unloads nine columns, not the three a diagonal crossing was said to need
  — but the wait duration is worker-thread time plus an SD write and cannot be measured off the
  console. Rather than change an untestable save path, the wait is now counted and shown on the
  debug overlay. Building across three or more columns and walking sideways out of them will
  settle it in one capture.

## [1.7.0] - 2026-08-24

**If you installed 1.6.0, it did not start. This release fixes that, and you should install it.**

1.6.0 shipped with a renderer that refused to initialise. `DIRTYQ_MAX` in `world/dirtyq.h` is a
hand-maintained copy of the mesh-pool size `MESH_SLOTS`, and it was left at 150 when 1.6.0 took
the pool to 294. `chunkRenderInit()` opens by asking the remesh backlog for one flag per mesh
slot, that request was refused, and the game quit before drawing a frame — with no message on
screen, because the text console is not up that early in boot. A black screen that answers only
START. Every release check 1.6.0 passed tested the packaging: the branch, the tag, the `.cia`,
the asset URLs, the `/releases/latest` redirect. **None of them started the program.** Booting
the build to a title screen is now part of cutting a release.

The rest of this release is the terrain rework: a Beta 1.7.3-style density-field generator with
real caves and overhangs, water, tall grass, and a resurfaced world.

### Fixed

- **The game boots.** `DIRTYQ_MAX` raised to match the pool, and a
  `_Static_assert(DIRTYQ_MAX >= MESH_SLOTS)` added in the one translation unit that can see both
  numbers, so the two can never silently drift apart again — they now fail the build instead of
  the console.
- **Chunks that would never have been drawn at all.** The mesh pool is sized against a measured
  worst case, and the new generator's caves push more chunks over the "not empty" line than the
  old terrain did — measured worst case 298 chunks against a pool of 294, and 7 chunks in a
  single column against a limit of 6. Over that limit a chunk is not merely slow to appear, it is
  never built: a hole in the world that walking away and coming back does not repair. The pool is
  now sized structurally — every chunk of every column in the widest view distance — so no future
  change to terrain generation can put it back under. 7.24 MB to 8.77 MB.

### Added

- **A new terrain generator.** Density-field terrain in the Beta 1.7.3 style: instead of picking
  a height for each column, the world is carved out of a 3D field, which is what makes real caves,
  overhangs and cliff faces possible rather than approximated. Sampled on a coarse lattice and
  interpolated, entirely in fixed-point arithmetic so the console and the test host agree to the
  bit.
- **Water.** A sea level and a water block, filling below the waterline. Still and static in this
  release — flow comes later. It is see-through, you cannot stand on it, and it does not hide the
  seabed.
- **Tall grass.** The first plant, drawn as crossed quads rather than a cube, scattered across
  grass by the surface pass.
- **A resurfaced world.** Grass, dirt and sand chosen by depth and slope over the new terrain,
  and trees that no longer try to grow at or below the waterline.
- **Per-world generator versions.** Every world records which generator built it, in a small
  `genver.bin` file beside its region data. **Worlds you already have keep the terrain they were
  made with** and are not reshaped under you; only new worlds get the new generator. A world saved
  by a newer version of the game than you are running is refused rather than loaded and quietly
  damaged.

### Testing

- 7,751 host checks across 21 binaries, up from 7,507.
- The new generator is pinned by 12 terrain hashes, so a change to its output is a test failure
  rather than something you notice in-game months later.
- The step-up physics gained 57 checks. Its first sabotage test was found to be inert — the body
  it moved never reached the step it was supposed to climb, so the test "passed" a run in which
  nothing happened. Fixed, then re-armed.

### Known limitations

- Nothing in this release has run on real hardware. The boot failure above was found and fixed on
  the host and confirmed in an emulator; a console has not seen it.
- The larger mesh pool needs 8.77 MB of linear heap. If it is not there, the game will say so at
  startup rather than misbehave later.
- Water does not flow, has no current, and does not slow you down. That is the next release.

## [1.6.0] - 2026-08-24

Finishes the master block registry and rebuilds the rendering foundation underneath it.

1.5.1 shipped the registry module dormant — it spoke only to a server that volunteers
`REGISTRY_INFO`, and no released server did. **blocksmith-server 1.6.0 does.** So this is the
release where a server-defined block actually becomes a thing you can see, stand on and build
with, and consequently the release where every place that quietly assumed there are exactly
eight block types had to be found and fixed. Several of those were live defects rather than
hypotheticals; each is listed below by what it did, not by what it was.

### Added

- **Server-defined blocks work end to end.** `bsEditValid()` on the server now admits the whole
  dynamic range `REG_ID_DYN_LO..REG_ID_DYN_HI` (0x80..0xFD) instead of stopping at the eight
  compiled-in core ids, matching the ceiling the client has always used. Placement, replication
  and the persisted diff store all carry dynamic ids now.
- **A bounded registry sync on join.** The client holds world entry open behind a
  "Joined - syncing block table..." row while `REGISTRY_DEFS` land, retrying a `FETCH` up to four
  times at 250 ms with a 2000 ms deadline, and resuming from the first id it is actually missing.
  Both clock-failure directions release the player rather than trapping them — `osGetTime()` is a
  wall clock that can step backwards, and a lid-close sleep can jump it forwards.
- **Non-cube block geometry.** Shape is carried in the top three bits of the existing
  `BlockDef.flags` byte, so `sizeof(BlockDef)` stays 27, the wire record stays 28 bytes, and the
  DEFS packet, `registry.bin` and the pinned core CRC-16 `0x7E5B` are all unchanged — nothing
  under `source/net/` needed editing. `BLOCK_SHAPE_FULL_CUBE` is 0, so every existing definition
  keeps its meaning. Cross-shaped blocks neither cast ambient occlusion nor cull their
  neighbours' faces, and the raycast treats them as targetable without treating them as walls.
- **Greedy face merging along the texture's U axis.** Co-planar faces of the same block merge
  into one quad when all four corners of every face share identical AO *and* identical light —
  the strictly lossless condition, since four equal corners interpolate to a constant. Measured
  **15.8% / 17.4% / 21.3%** fewer quads on seeds 1337 / 4242 / 90210 at render radius 3. Runs cap
  at 15 blocks and stop at chunk edges.
- **Render distance can reach radius 3.** The mesh pool is re-cut to 158 small / 120 medium /
  16 large slots (294 total, 7.24 MB, +3.00 MB), sized against 9,000 measured columns and 37,682
  chunk meshes rather than against a formula. **The New 3DS default deliberately stays at 2** —
  raising a ceiling is opt-in and one shoulder-press reversible, while raising a default would
  make every console's first boot the experiment, and radius 3 has never run on hardware.
- **A "what's new" screen in the updater.** When Options → Check for Update finds a newer
  release, the top screen shows a short plain-English changelog for the version about to be
  downloaded — features added, then bugs fixed — scrollable with D-pad up/down and carrying a
  proportional scrollbar so it is visibly scrollable. The text comes from a
  `whatsnew<version>.txt` asset on the release, fetched from the same predictable
  `releases/download/v<ver>/…` path the `.cia` uses. Never from `api.github.com`, which is 60
  unauthenticated requests per hour per IP and shared, and never from the release body, which the
  redirect cannot see. Every failure mode — 404, timeout, DNS failure, empty file, garbage —
  falls through to "No change notes for this version." and the download proceeds.

### Fixed

- **A completed registry sync was never actually verified.** `s_reg_synced` was set by any DEFS
  packet carrying the LAST bit, outside the branch that applies rows — so the empty terminating
  batch the server always sends declared the sync finished without applying anything, without
  checking that the batch started where it should, and without checking whether the table had
  already been frozen. A dropped data packet or a revision mismatch therefore produced a
  core-only table that reported itself healthy, with the retry disarmed. It now means what it
  claims: `rev`, `count` and a recomputed CRC-16 must all match the fingerprint the server sent.
  A complete delivery whose terminator is lost now verifies anyway, because verification no
  longer depends on any single packet arriving.
- **Playing single-player, backing out to the title and joining a server broke that server's
  blocks.** `registryFreeze()` runs in single player too, and only `registryInitCore()` clears
  it, which the quit-to-title path never called — so the table stayed frozen and refused every
  batch the server sent, leaving its blocks as invisible holes for the whole session. Rebooting
  fixed it, which made it look intermittent. The reset now lives at the top of the session loop,
  so any future exit path gets it for free. **The same bug had a second, unreported half:**
  loading a different single-player world after backing out ran it on the first world's block
  definitions.
- **Mining a server-defined block deleted it.** The cell was cleared before the inventory add was
  attempted, and the add then refused on the same eight-id ceiling — so the block vanished from
  the world for every player, with nothing to show for it. A block that cannot be carried now
  cannot be broken, guarded before anything is mutated and before the edit is sent.
- **A chunk whose mesh job was refused was never re-queued.** The column was marked queued
  *before* the push, so a full job queue left a permanent hole that no amount of walking would
  fill. Latent at radius 2 because the ring was exactly the queue's capacity; raising the radius
  would have made it routine. Now push-then-mark, with a retry flag drained on the next pass.
  `JOBQ_CAP` also goes 128 → 512 and the horizon-culling column table 40 → 65, both of which the
  radius-3 ring overran.
- **Server-registered solid blocks read back see-through.** `visgraph.c`'s open-cell table was
  filled only for the eight core ids and left every dynamic id at its air prefill.
- **Pressing B while the block table was still syncing could throw you into the world instead of
  back to the menu.** Leaving and entering were decided by two independent branches in the same
  frame; the decision is now a single ordered one in which BACK wins outright.
- **A glowing server-defined block would never have lit anything** — the lighting pass scanned
  only the first eight entries of a 256-wide luminance table. Dormant until a server declares
  one; measured at 0.011% of the relight call it gates.
- **Persisted edits carrying dynamic ids were dropped again on every server restart**, because
  the diff store replays each record through the same validator that was rejecting them.

### Changed

- **The texture atlas is now a one-tile-wide vertical strip** — 16 × 256 px, sixteen stacked
  16×16 slots with no padding, replacing the 128 × 128 sheet of 6 × 6 twenty-pixel cells. This is
  what makes greedy merging possible at all: `GPU_REPEAT`'s period is the whole texture, so on a
  packed grid a merged quad samples its neighbour's tile, while on a one-tile strip it repeats
  correctly. Tile pixels are unchanged, proved per tile by SHA-256. Atlas VRAM drops 32 KB → 8 KB,
  and `atlasRect()` became divide-free and modulo-free, neither of which the ARM11 has an
  instruction for.
- **An unpainted or out-of-range texture id now draws an unmistakable missing-texture marker**
  rather than silently drawing grass or an opaque near-black square. A wrong texture constant
  still renders *a* texture, so this class of bug otherwise presents as bad art and never as an
  error — which has cost this project real time before.
- `PROTO_COMMIT` re-pinned to blocksmith-server 1.6.0. `git diff` over `proto/` between the old
  and new pins is empty, so `bs_proto.h` is byte-identical and **this is not a wire change**; the
  pin moved only so it names a released server rather than an unreleased mid-branch commit.

### Known limitations

- **A server-defined block still cannot be picked up.** The eight-id ceiling remains on *item*
  ids, which index client-side tables that are genuinely that wide on the console, and the server
  mirrors it for pickup and consume. Registry-aware inventory is a later, cross-repo change; until
  then such a block is placeable and unbreakable rather than placeable and destructible.
- **Only 15 of the atlas's 16 slots are addressable and 10 are painted**, because a mesh vertex
  stores its V coordinate in a byte. Not a constraint on anything today — the registry protocol
  has no texture upload, so a server-defined block can only reference a tile the client already
  ships.
- **Whether an Old 3DS holds 60 fps at radius 3 is unknown.** It submits roughly 1.9× the
  triangles of radius 2 before culling. This machine has no GPU and the emulator does not model
  one, so nothing measured here makes radius 3 safe — it makes it available. That is why the
  default did not move.

## [1.5.1] - 2026-08-23

A repair release. Everything here fixes a defect that was live in 1.5.0; there are no
new features. The block-registry module added in the unreleased "Phase A" work is present
but dormant — it speaks only to a server that volunteers `REGISTRY_INFO`, which no released
server does, so against the current server it costs nothing and changes nothing.

### Removed

- **The minimap.** `scene/minimap.c` called `worldGet(NULL, …)`, and `worldGet` only
  short-circuits on `y < 0`, so at `y == 0` it reached `slotIndexFor` and dereferenced a null
  world — opening Options → Debug → Minimap data-aborted a real console. Its fog was never
  updated (nothing outside the test called `minimapFogUpdate`) and its writes into a tiled
  `C3D_Tex` were linear, so the feature was three independent repairs deep. Removed rather
  than patched; the touch-screen map in the spec is a larger, different feature to be built
  fresh.

### Fixed

- **The debug menu stepped render distance every time it opened.** Opening it consumed the
  same `hidKeysDown()` word that opened it, so each open incremented the distance and
  re-meshed the ring. It then saved the *old* value, because `opts.render_dist` was never
  assigned from `s_mesh_radius`.
- **The controls screen was unreachable.** The same frame-input reuse dropped the player
  straight into binding capture instead of the binding list.
- **Held-stylus input toggled at 60 Hz** in both new menus — `hidKeysHeld() & KEY_TOUCH` is a
  level, and it was being treated as an edge. Both are edge-triggered now.
- **Sleep left the CPU clocked up.** The wake hook called `osSetSpeedupEnable(true)` — the only
  such call in the tree — so after the first lid cycle a New 3DS ran at 804 MHz and
  `battery.c`'s `tickMs()`, which divides by `SYSCLOCK_ARM11`, silently mis-measured. Both
  arms removed.
- **Closing the lid lost recent building.** There is now a time-capped flush (budget
  `SLEEP_FLUSH_BUDGET_MS`, 500 ms) that saves dirty columns one at a time and keeps a cursor,
  so a truncated flush resumes at the next lid-close instead of restarting.
- **A lidded multiplayer session became a zombie.** The old `sleepNetHeartbeat()` claimed the
  socket survived sleep; it cannot — the process sits in libctru's `LightEvent_Wait` for the
  whole sleep, so nothing can be sent. It is replaced by a leave hook that disconnects on
  lid-close, which drops the player back to the Multiplayer screen instead of leaving them in
  a convincing forgery of a shared world. A short lid-close now also costs a rejoin; that is
  deliberate.
- **A dead frame-skip guard** was removed. Both APT hooks complete inside a single
  `aptMainLoop()` call, so the flag was always false again by the time the loop body read it.
- **The probe build configurations did not compile.** `-DBS_WORLD_GEN=0` (and with it
  `BS_EDIT_STRESS`, `BS_DIG_OUT`, `BS_REMESH_STRESS`, `BS_CMDBUF_PROBE`) failed with 12
  errors: the debug-menu state, the remap-screen state and the boot/draw-probe instrumentation
  were all written inside the `#if BS_WORLD_GEN` block while their callers sat outside it.
  Pre-existing, not new to this release.

### Testing

- `tests/battery_test.c` and `tests/sleep_test.c` were compiled as lone translation units
  carrying hand-copied duplicates of the logic they claimed to check, so neither linked the
  module it was named after. Measured: with `battery.c` sabotaged to return a wrong bar count,
  the old test still passed all 16 checks. Both now link the real module behind an
  `#ifdef __3DS__` split, with the sabotage arms recorded in `tools/run_host_tests.sh`.
  Battery 16 → 25 real checks, sleep 13 → 34.
- Suite total is now 17 binaries and 4012 checks.

## [1.5.0] - 2026-08-22

### Added

- **Server-side saved players.** A server running v1.5.0 now remembers each
  player between sessions — position, rotation, armor, XP, health and hunger —
  and hands it all back when you rejoin. This release carries the client half
  of that contract: the new `BS_APP_PLAYER_STATE` packet is decoded the moment
  it arrives (even while the title screen is still pumping network updates)
  and retained in `source/net/networld.c` alongside the inventory snapshot it
  already kept. Armor, XP, health and hunger have no gameplay on the client
  yet — they are held verbatim behind a deliberately dumb boundary
  (`NetworldPlayerMeters`, `source/net/networld.h`) so the wire format is
  frozen before those systems land. The report half of the pair exists too,
  but stays dark twice over: the client sends nothing until the server has
  proven it speaks player state, and the meter-filling code is compiled out
  entirely (`BS_CLIENT_HAS_METERS=0`), so this build can never overwrite a
  returning player's saved progress with zeros. The server side is server
  v1.5.0, released separately.

- **An adaptive lighting engine** (`source/world/light.{c,h}`, new). Sky light
  is computed per world column by two engines the host suite asserts produce
  byte-identical output — BFS flood fill at generation time on the worker
  thread, fixed-order relaxation sweeps when blocks change on the main thread —
  and baked into each vertex through the pad byte the mesher has carried since
  the v1.2.5 alignment fix. On a New 3DS a new dynamic shader
  (`source/shaders/world_dynamic.v.pica`) unpacks those values per corner, so
  overhangs, caves and tree canopies finally shade by what is actually
  overhead; on an Old 3DS nothing runs, nothing allocates, and every vertex is
  byte-for-byte what it always was. The console model picks the path once at
  startup via `APT_CheckNew3DS`; day-night modulation already rides in the
  shader but is pinned to full daylight until a cycle exists.

### Changed

- **Protocol pin updated to server v1.5.0** (`e8e094c`), the commit that added
  the player-state/report pair above, replacing the v1.3.0-era pin left from
  the v1.4.0 repair. Fresh clones now build against the wire format this
  release actually speaks; the Makefile documents the deployment order — state
  first, reports only ever after the server has spoken.

### Fixed

- **Rejoining a server restores your last position and facing** instead of
  dropping you at the world spawn point. The saved pose replays once, right
  after player init in `source/main.c`, using the same arrival-order trick as
  the inventory snapshot: the packet lands before the player object exists, is
  kept by the network layer, then applied. Against a server that does not send
  player state nothing changes — no packet, no restore, the usual spawn choice.

## [1.4.0] - 2026-08-21

### Added

- **Universal control remapping.** Options → Controls lists every mappable action
  with its current button. Tap an action (or select it and press A) and press the
  button you want: if that button already drives something else, the two actions
  swap rather than one of them silently losing its binding. B during capture
  cancels without changing anything, a row at the bottom restores the shipped
  defaults, and every change is written straight back to `options.ini`, so it
  survives a reboot like the rest of the settings.

- **A sleep preserver.** Closing the lid no longer leaves the game running into
  a suspended console: an APT hook marks the app asleep, the frame loop skips
  simulation, networking and drawing until wake-up, and the New 3DS clock-speed
  request is dropped for the duration so the battery is not drained fast on
  purpose. Opening the lid resumes exactly where the frame left off.

- **A battery gauge.** A four-bar indicator with a charging state, read from the
  console's own power service at most once a second, sits in the corner of the
  touch screen.

- **A live minimap with fog of war.** The bottom-right of the touch screen can
  show a 64×64 top-down map centred on the player: terrain colours where you
  have walked, flat grey where you have not, and a small arrow for which way you
  are facing. Explored area is saved per world seed and reloaded when you come
  back — including on a server, since the fog belongs to this console's
  exploration of that seed, not to any local save file. Until crafting exists to
  make an equip-able map item, the toggle lives in the debug menu; the moment a
  crafted map is possible this moves to equipping it, as intended.

- **A debug menu.** Options → Debug opens a registry-driven menu: the render
  distance stepper, the minimap toggle, live readouts (frame time, meshes and
  triangles, culled chunks, world size, memory against budget, player position),
  and greyed-out placeholder rows for Weather and Dimension so the systems the
  roadmap adds later have somewhere to land without redesigning the menu.

### Fixed

- **A fresh clone could not build.** The Makefile pins the multiplayer protocol
  by commit, and the pin still named the v1.3.0 server while v1.3.1's inventory
  code already spoke v1.4.0's wire format — anyone running `make deps && make`
  got a compile error through no fault of their own. The pin now names the
  v1.4.0 server commit.

## [1.3.1] - 2026-08-21

### Fixed

- **Server inventories actually work now.** In v1.3.0 they did nothing at all: you would rejoin
  with an empty inventory every time, exactly as before the feature existed. Two mistakes,
  stacked, both on the console side — the server half was correct and has not changed.

  The first is a matter of ordering. Joining a server finishes on the *title screen*, not on
  entry to the world, and the server sends your inventory the instant you join. The client did
  not start listening for it until much later, so the one message that carried everything you
  owned arrived, was decoded, found nobody waiting for it, and was discarded. The client now
  keeps the most recent snapshot and hands it over the moment something is listening, which
  makes the order it arrives in stop mattering.

  The second is that a server session deliberately starts from an empty inventory — the server
  owns what you are carrying, so the console has nothing of its own to load. That emptying ran
  *after* the snapshot would have been applied, wiping it. The listener is now registered after
  it instead of before, so the server's answer is the last word rather than the first.

  Both are covered by tests that walk the real sequence, including the title-screen arrival, and
  both were confirmed to fail against the old code before the fix went in.

## [1.3.0] - 2026-08-21

### Changed

- **On a server, your inventory now belongs to the server.** Until now every console kept
  its own inventory in RAM and told nobody, so two players could mine the same block and
  both bank it, and anything you were carrying vanished the moment the console slept. The
  server now holds all 24 slots and the selected hotbar index for each player, writes
  them to disk, and hands them back on the next join.

- **Changes are applied on the console first and reported afterwards, not asked for and
  waited on.** The link is unreliable UDP with no acknowledgements and no sequence numbers,
  so asking the server for permission would make the touch panel appear to ignore taps for
  as long as the round trip takes. Instead the tap happens instantly and a one-line report
  follows it. If that report is lost, the server's next full snapshot puts the inventory
  back the way the server sees it — the move visibly un-happens, which is legible to a
  player, and it corrects itself without the client needing to detect the loss at all.

- **Every report carries the amount that actually landed, never the amount that was asked
  for.** Moving a stack of 40 into a slot that only has room for 4 reports 4. Picking up a
  block into a full inventory reports nothing. Sending the requested figure instead would
  let a server whose copy had already drifted apply *more* than this console did, and the
  two would diverge further with every action rather than resynchronising at the next
  snapshot.

- **This is a protocol addition, and it is safe in both directions.** The server volunteers
  the first inventory snapshot unprompted at join, which makes that snapshot the capability
  probe as well as the payload: a console talking to a v1.3.0 server never receives one, so
  it never sends an action, and its inventory behaves exactly as it did on v1.2.7. A
  v1.4.0 server talking to a v1.2.7 console is safe for the opposite reason — this client
  ignores app message types it does not recognise rather than dropping the link. **Update
  in whichever order suits you.** Single-player is untouched either way; nothing is sent
  when there is no session.

- **Terrain generation and chunk streaming deliberately stay on the console.** Both were
  weighed against moving them to the server and both were worse there — the terrain is
  already deterministic from its seed, so sending it over the link would spend bandwidth
  reproducing something the console can compute for free.

## [1.2.7] - 2026-08-20

### Added

- **A pause menu.** SELECT now opens Resume / Options / Quit over the bottom screen.
  Until now the only way to change a setting was to leave the world entirely and use
  the title screen's options page, which meant unloading the world to change the render
  distance and loading it again to see what the change did — the one comparison the
  setting exists to let a player make.

- **Render distance is changeable from that menu, and it takes effect immediately.**
  The ring re-meshes around the player as the value steps, and the new value is written
  to `sdmc:/blocksmith/options.ini` in the same breath, so it survives a reboot. The
  arrows grey out at each end of the range rather than disappearing, so the row does not
  change width when it hits a limit.

- **A memory readout on that page.** Three figures, not one, because they come from three
  different pools and answer three different questions: the world's block storage against
  its budget, free space in the linear heap the chunk mesh pool is claimed from, and free
  VRAM. A single "free RAM" number would be a fiction on this console.

- **Wooden planks.** A new block, crafted four at a time from one wood log — the first
  recipe in the game whose output is not also something you can dig up, and the first
  thing wood is good for. The texture atlas grew from 64x64 to 128x128 to hold its tile,
  which cost 24 KB of VRAM, measured on device as free VRAM moving 4636 KB to 4612 KB.

### Changed

- **A server session now asks for the block edits it can actually see.** Joining used to
  pull one `WORLD_SYNC` carrying every edit anyone had ever made anywhere in the world,
  and the client held all of them forever. The client now subscribes per column as the
  ring loads it (`CHUNK_SUB`), receives that column's diffs in 64-edit batches
  (`CHUNK_DIFFS`), and unsubscribes as the column falls out of the ring (`CHUNK_UNSUB`).
  What arrives at a join is bounded by render distance instead of by the age of the
  server. **This is a protocol change and it is not backwards compatible in one
  direction:** the server must be on v1.3.0 or newer before this client connects, because
  bsgame kicks any client that sends a message type it does not know and this client sends
  `CHUNK_SUB` on its very first loaded column. The reverse is safe — a v1.3.0 server that
  hears no subscription within 500 ms falls back to serving the old full `WORLD_SYNC`, so
  older clients keep working against it.

- **The server's edit ceiling went from 65,536 to 131,072 blocks**, its hash table doubled
  with it to 262,144 slots to hold the same ~0.5 load factor and the same short probes.
  65,536 was under the 100,000-block figure that was asked for; 131,072 is the next power
  of two above it. Memory on the server, which has it: 131,072 entries plus a 1 MB index.

- **The client's store of edits waiting for their column stopped losing the newest ones.**
  It held 256 entries and *refused* everything past that rather than evicting, while the
  server replays its whole diff set to a joining client in one oldest-first burst — so on
  a well-built world a join dropped whatever had most recently been built. Measured on a
  300-edit sync against the old cap: `queued 256 of 300, refused 44`. The cap is now
  65,536 entries, 256x what it held before. That is *half* the server's own 131,072
  ceiling, not a match for it: a world that has been edited in more than 65,536 distinct
  places can still overflow this store on a join, and the newest edits are still the ones
  that lose. Closing that gap is a separate change. That size
  was only affordable after the store was hash-chained by column into 4096 buckets: the
  old design scanned the whole array on every record and drain, which at the new size is
  about 4.3 billion comparisons to absorb one sync — a hardware freeze, not a slow frame.

- **3D moved from SELECT onto the pause menu's options page.** SELECT was the only button
  that toggled it, and SELECT is now the button that opens the menu. Putting the toggle on
  a settings page is where it belongs, but the honest reason it moved rather than being
  dropped is that reusing its button would otherwise have deleted the feature outright.

- **A paused world does not tick.** The player does not move, terrain does not stream in,
  and the L/R render-distance keys are ignored, so the memory figures on screen hold still
  while they are being read. Networking deliberately keeps running: a session that stopped
  answering while its player read a menu would be dropped by the server.

### Fixed

- **The world stopped carrying 289 columns nobody was standing in.** At boot the game
  allocates a 17x17 grid of columns to prove the console can claim the worst case it will
  ever be asked for — and then never gave the grid back, so every session generated the
  real world *around* it. On the same world at the same render distance the overlay now
  reads `cols 25 chunks 124` where it read `cols 305 chunks 2392`, and peak block storage
  halved from 515,181 to 258,320 bytes. The bytes were the smaller half of it: those 289
  columns also occupied 289 of the world table's 1024 slots for the whole session, so
  every lookup the streaming ring made for its own columns had to probe past them.

### Notes

- The touch panel underneath the menu is fed a blank input while the menu is up, so a
  finger landing on the QUIT row cannot also swap the hotbar slot sitting beneath it.

## [1.2.6] - 2026-08-20

### Fixed

- **Another player's block edits are now visible.** In a server session, breaking or
  placing a block changed the world for everyone but only changed the *picture* for
  the player who did it. From anyone else's screen the edit was invisible: the terrain
  kept drawing as it was before. It was not, however, imaginary — the block outline
  snapped onto the edited block, and a player could walk into and fall down a trench
  someone else had dug while still looking at solid, unbroken ground.

  Nothing on screen is read from the world directly. The terrain is a set of cached
  chunk meshes, each built once and rebuilt only when something marks it dirty. A local
  break or place marks its own chunk immediately (`scene/interact.c`), but a remote edit
  wrote the block and marked nothing, so the stale mesh survived until the chunk happened
  to stream out of the ring and back in — which rebuilds it from scratch and is why the
  world eventually, unpredictably, caught up. Collision and the block raycast both read
  world blocks directly rather than the mesh, which is exactly why physics and the
  outline stayed correct throughout and made the bug look like a rendering ghost rather
  than a missing update.

  `net/networld.c` now reports every remote edit the moment it reaches the world, and
  `main.c` marks the affected chunk — plus any neighbour whose faces the change exposed —
  through the same `chunkRenderTouch()` call a local edit has always made.

### Added

- A crosshair. A white reticle with a dark outline at the centre of the top screen,
  drawn once per eye at identical screen coordinates so it carries no parallax and sits
  at screen depth rather than floating in front of or behind the block being aimed at.
  The outline is not decoration: a plain white reticle disappears against snow, sand and
  the bright top faces of grass, and a plain black one disappears into cave mouths.

### Notes

- The fix is entirely client-side. No protocol change, no new message types, no altered
  byte layouts — the pinned `proto/bs_proto.h` commit is untouched. **A server running
  v1.2.5 needs no update to work with this client.**

## [1.2.5] - 2026-08-20

### Fixed

- **The freeze entering a world, present since v1.1.0, is fixed: the chunk vertex
  attributes were misaligned.** Thirteen builds tried to find this and it never once
  reproduced in an emulator, which was itself the clue.

  citro3d has no per-attribute byte offset. The PICA200 works out where each attribute
  starts inside a vertex by adding up the sizes of the attributes declared before it, so
  `AttrInfo_AddLoader(attr, 0, GPU_BYTE, 3)` followed by
  `AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4)` placed a four-byte attribute fetch on
  **byte offset 3** of an eight-byte vertex. Every vertex, every chunk, every frame. An
  emulator runs that on an x86 host, which performs unaligned loads without complaint; the
  real GPU took the command and never reported finishing it.

  `MeshVertex.pad` was at the end of the struct "to keep the stride a power of two" — but the
  stride was eight bytes either way. The thing that actually needed padding was the gap
  between the two attributes. `pad` now sits at offset 3, attribute 0 is declared as four
  components, and attribute 1 starts at offset 4. Both attributes are 4-byte aligned, the
  vertex is still exactly 8 bytes, and the 4.24 MB chunk vertex pool is unchanged.

  Evidence this was the right target, rather than argument:
  - The hardware capture of the hung frame (v1.2.2) recorded
    `GPUREG_ATTRIBBUFFERS_FORMAT_LOW = 0x000000d8` — the GPU being told, in its own words,
    to fetch four bytes from offset 3 of an 8-byte stride.
  - `source/gfx/sprite.c` uses the same `C3D_DrawElements`, the same shared index buffer and
    the same offset-into-it draw, and renders correctly on the console in the very frames the
    chunk draw wedges. Its vertex is 3 floats + 2 floats + 4 unsigned bytes: aligned
    throughout. That was the control, and it ruled out the draw call itself.
  - The round-2 draw bisect (v1.2.3, run on hardware) had already eliminated everything else
    inside the draw: arm 2 (all the CPU work, not one GPU command) survived, so it was not a
    loop; arm 5 (full GPU state, no draw calls) survived, so it was not the program bind, the
    attribute *config*, the texture bind, the depth/blend/fog state or any uniform upload;
    arms 3 and 4 (each pass alone) both hung, so it was nothing unique to either pass.

  A `_Static_assert(offsetof(MeshVertex, u) == 4, ...)` now fails the build if anyone moves
  the field back. It was tested against the old layout and does fail.

### Changed

- The world vertex shader's relative addressing (`mova a0.x, inpack.zzzz` /
  `mov r3, faceShade[a0.x]`) is **restored**. v1.2.4 replaced it with a constant index to test
  it as the cause; that build still froze, so it is cleared by measurement and per-face
  lighting is correct again. Nothing was ever wrong with those two instructions.
- Attribute 0 is now declared with four components rather than three, so 4 + 4 accounts for
  every byte of the 8-byte stride. Previously one byte of each vertex was unaccounted for.

## [1.2.4] - 2026-08-20

### The round-2 bisect came back and it narrowed the freeze to one thing

Results, one arm per boot on real hardware: arm 0 "everything" HUNG (control, correct). Arm 1 "world draw skipped entirely" SURVIVED (control, correct). Arm 2 "cull only - all the CPU work, not one GPU command" SURVIVED. Arm 3 "opaque pass only" HUNG. Arm 4 "transparent pass only" HUNG. Arm 5 "GPU state set up, but no draw calls at all" SURVIVED.

### What that eliminates

Arm 2 surviving rules out a CPU loop that never ends - all the culling, the visibility walk and the horizon test ran to completion. Arm 5 surviving rules out the whole GPU state block: the shader program bind, the attribute configuration, the atlas texture bind, depth/blend/cull/fog state and every uniform upload. Arms 3 and 4 both hanging rules out anything unique to either pass, including the alpha test that only the transparent pass turns on and the face-mask bucket merging that only the opaque pass does. What is left is what happens only once a chunk draw is actually issued.

### The experiment in this build

The world vertex shader is the biggest thing in that remaining set, and it contains the only relative addressing in the entire project - `mova a0.x, inpack.zzzz` followed by `mov r3, faceShade[a0.x]`, in source/shaders/world.v.pica. That reads a byte off the vertex, loads it into the PICA200's address register, and fetches a uniform at an offset the CPU never sees. It is exactly the kind of construct an emulator implements as a bounds-safe array lookup while real silicon does a raw fetch out of the constant bank, which fits "hardware only, never reproduces in Azahar". This build replaces the indexed fetch with a fixed `mov r3, faceShade[0]`. Nothing else changed.

### Why it is written that way rather than deleting the array

faceShade stays declared and still read, so picasso keeps emitting it into the shader's uniform table and `shaderInstanceGetUniformLocation` still resolves it. If the array had become unused that lookup could return -1 and the upload loop in source/scene/chunk_render.c would write to whatever sits below it. Only the indexing changed - one variable.

### What it looks like on screen

faceShade[0] is FACE_EAST at 0.78, so every face is now lit identically at 0.78 instead of 1.00 on top and 0.48 underneath. Textures are unaffected. The flatness is deliberate and doubles as proof the new shader is the one running.

### Verified in the emulator

by screenshotting the same world position with the experiment and with the original two lines restored, then measuring mean luminance of the same pixels in both. Top faces came out at 0.781 and 0.783 of their control brightness against a predicted 0.78/1.00 = 0.780. A west-facing wall went UP by 1.143 against a predicted 0.78/0.66 = 1.18, and a north-facing wall by 1.068 against a predicted 0.78/0.72 = 1.08. Had the build not taken effect every ratio would have been 1.000. This build carries no diagnostics and writes no report files; the result is meant to be judged by eye.

### This is an experiment, not a known fix

If the freeze persists, the shader is eliminated and the next cut is inside the draw itself.

## [1.2.3] - 2026-08-20

### Why this build exists

v1.2.2's hardware report was the first from a provably single boot (the boot fence worked). It said the freeze happens on the FIRST world frame, not after minutes of play: `frame captures: 1`, `list check: 1 clean frame(s)`, `columns in: 49` (world still streaming). The `frames drawn: 341` is almost entirely title screen. The frame's own ProcessCommandList was submitted and never completed — `gx queue: cap 32 queued 5 submitted 5 completed 2`, with both buffer clears completing and the draw never doing so. Vblanks kept arriving, the command list was byte-identical to what was submitted, it validated clean, and the draw guard passed every chunk.

This clears v1.2.1's fix as the cause. That race needed a previous in-flight draw for genFollow and the mesh drains to overwrite. On the first world frame there is no previous draw.

### The draw bisect is switched back on

Version-stamped so it cannot repeat v1.1.2's failure. Round 1 (already done, on hardware) removed whole draws from the frame and left exactly one survivor — "no world" — so chunkRenderDraw() is the call that hangs. Round 2's six arms cut INSIDE it: 0 everything (control, must hang), 1 world draw skipped entirely (control, must survive), 2 cull only - all the CPU work and not one GPU command, 3 opaque pass only, 4 transparent pass only, 5 GPU state set up but no draw calls at all. Arm 2 is the pivot: if it hangs the fault is a CPU loop that never ends; if it survives the fault is something handed to the GPU.

### Version stamping

drawprobe.txt now carries a `# blocksmith <version> round 2 bisect` first line. Anything not written by exactly this version is deleted rather than interpreted. This matters because arms are RENUMBERED between rounds — round 1's arm 2 was "no highlight cage", round 2's arm 2 is "cull only". v1.1.2 read v1.1.1's file, concluded every arm had had its turn, and parked on the arm that draws nothing; a build meant to freeze came back alive and was reported as passing.

### Two report lines that lied, fixed

(a) The R1 command-list replay printed "REPLAY IS BROKEN" on timeout. replaySubmit uses GX_ProcessCommandList, which appends to the SAME GX queue the game's frame was submitted on — so when that frame is already wedged the replay can never run. It now samples the queue before submitting and says "the queue was ALREADY wedged; the replay never ran", with the queued/completed counts. On steve's console the old wording blamed the instrument for the bug it had just caught. (b) The `gsp vblank` explanation claimed ALIVE proves the stall is a GPU command that never finished. VBlank and command-completion are different GSP events, so ALIVE does not prove completion interrupts are being delivered. Reworded.

### Verified

(emulator, both directions): planted a foreign 1799-byte round-1 drawprobe.txt — it was discarded, not inherited, and boot 1 started at arm 0. Across 7 boots the arm advanced exactly one step per boot, 0 through 5, then reported "all arms done; parked on arm 5", with no false HUNG lines. Red control with a stall planted: boot 1 wrote `arm 0 START` and hung with no survival line; boot 2 correctly wrote `arm 0 HUNG - no survival line, the console froze on this arm` and advanced to arm 1. No hardware testing was performed; this build does not attempt to fix the freeze.

## [1.2.2] - 2026-08-20

### Instrument, not a guess

This release can tell us where the console actually stops. Everything v1.2.1 fixed is still in it. What is new is the visibility into the exact moment a freeze happens, and which boot froze.

The reason it was needed: the user booted v1.2.0 twice, once into multiplayer and once into single player, and both froze. The SD card came back with `postmortem.txt` saying "frame 2, GPU wedged" and `hang.txt` saying "340 frames drawn, phase DRAW". Both stamped 1.2.0, and they cannot describe the same freeze. Nothing on the card said which boot either came from — so two separate failures were read as one, and the fix that followed was aimed at an average of them. That is the real reason five builds in a row have missed.

### Boot fencing

At startup, after `sdmc:/blocksmith` is created and before anything can write into it, the app deletes `hang.txt`, `postmortem.txt`, `gxprobe.txt`, `selftest.txt`, `cmdhang.bin`, `cmdprev.bin`, `bisect.bin`, `boot_timing.txt` and the numbered `cmd*.bin` capture ring, then writes `bootid.txt` naming the version and whether it is an Old or New 3DS. A boot that freezes cannot delete its own files, and the next boot deletes them before writing its own — so from now on, any diagnostic file sitting on the card was written by the boot that just froze. `drawprobe.txt` is deliberately exempt because its entire job is to survive a boot: a boot that hangs never writes its own survival line, and that silence is what the next boot reads.

### Full reporting instrumentation

Switched back on. It existed in v1.1.8 and was compiled out of v1.2.0 and v1.2.1, which is why their `hang.txt` could only say a phase name and could not say whether the GPU or the CPU was stuck. Now `hang.txt` carries: the GX command queue with which command is stuck, citro3d's two vblank counters sampled twice a second apart (which separates "the GPU never finished" from "event delivery died"), the draw-stage breadcrumb naming which pass and which chunk index was being drawn, and the draw guard's verdict on whether any malformed draw was ever issued.

### Nothing is removed from the frame

The draw bisect's arm selection is forced to arm 0, "everything drawn", so this build looks and plays like the game. This matters because an earlier diagnostic build silently parked itself on a "draw nothing" arm, came back alive, and was reported as passing when it had actually been skipped.

### The command-list checker was manufacturing false evidence

`hang.txt`'s `list check` section reads back the exact command list the GPU was executing and
looks for values a rasteriser cannot use. It was decoding every vertex-shader float uniform as
PICA **float24**, three packed words per vec4.

citro3d does not upload in that format. It sets bit 31 of `GPUREG_VSH_FLOATUNIFORM_CONFIG`
(register `0x2C0`), which selects plain IEEE **float32**, four words per vec4. Reading those
words as float24 slices 24-bit fields straight across the float boundaries, and the garbage that
falls out routinely has an exponent of all ones - which the checker then reported as "float
uniform is Inf or NaN".

Measured on a real captured list: all sixteen `0x2C0` writes in the frame are `80000000` /
`80000004`, bit 31 set. All sixteen `0x2C1` uniform bursts have parameter counts divisible by
four and none divisible by three. Decoded as float32 the values are an ordinary view matrix -
`bf800000` (-1.0), `3f3504f3` (0.70710678), `c19069e8` (-18.05).

The checker now reads the `0x2C0` mode bit and decodes accordingly, and a burst reached without
a preceding config write is left unchecked rather than decoded on a guess.

The effect, same captured list, same build otherwise:

| | before | after |
|---|---|---|
| frames flagged | 597 of 795 | 0 of 199 |

Its self-test was passing throughout, because it only ever fed itself synthetic float24 data -
it agreed with its own wrong assumption. There is now a float32 red/green pair as well, and the
green word (`3f35047f`) is chosen so it can only pass if the mode bit is honoured: it is an
ordinary float32 whose low byte is `0x7f`, exactly where float24 slicing puts an exponent. With
only the mode bit cleared, that same word flags. Both pairs report under test 9.

**Any conclusion drawn from a `list check` line in a hang report before this release has to be
thrown away.**

### The GX queue heading could blame the wrong processor

With a stall planted in the CPU-side draw loop, `hang.txt` printed `queued 1 submitted 0
completed 0` under a heading reading `STUCK ON : entry 0 type 2 MemoryFill`. The numbers were
right and the heading was a lie - nothing had been handed to the GPU at all.

When `submitted` has not moved past `completed`, nothing is in flight; the entry is labelled
`NOT SENT` and the report says in words that the stall is CPU-side. When it has, the label is
still `STUCK ON`. This is the single most consequential line in the file, because it decides
which half of the machine the next build looks at.

### Verified

Both emulator arms, on the shipping validator:

- **Red** (stall planted at opaque-pass chunk index 3): `hang.txt` named `phase DRAW`,
  `draw stage opaque pass`, `loop index 3`, `mesh slot 57` - an exact hit on the planted
  location. `gsp vblank ALIVE (+59/+59)`, `gx queue ... NOT SENT`, `list check: 199 clean
  frame(s), nothing flagged`, `draw guard clean`.
- **Green** (shipping build, nothing planted): world renders normally at 114 meshes / 88084
  tris, no `hang.txt`, no `postmortem.txt`, `drawprobe.txt` reads `arm 0 SURVIVED 600 frames`.
- Validator self-test reports `9 validator red/green pair PASS - flags Inf in both float
  formats, flags a bad address`.

Red arm ran on an Old 3DS emulator profile, green arm on a New 3DS one.

**No hardware testing was performed.** The freeze this build exists to locate has never
reproduced in an emulator, so nothing here says the freeze is fixed - only that the instrument
works and reports honestly.

## [1.2.1] - 2026-08-20

### Fixed - the hardware freeze, for real this time

**The CPU was rewriting chunk geometry while the GPU was still drawing it.**

`C3D_FrameEnd(0)` hands the frame's command list to GX and returns immediately. The GPU keeps
reading the chunk vertex buffers out of physical RAM, on its own bus, for as long as that draw
takes. The frame loop then went straight on to the next iteration and did three things that
write to exactly those buffers:

- `genFollow()` (`source/main.c`) recentres the column ring, which reaches
  `chunkRenderReleaseColumn` and hands a live mesh slot to a different chunk.
- `chunkRenderDrainDirty()` rebuilds edited chunks - `memcpy` straight over a slot's vertices.
- `genDrainMesh()` meshes newly generated columns - the same `memcpy`.

The only wait for the GPU came *after* all three, at `C3D_FrameBegin`. So from the second world
frame onward the vertex fetch unit was following indices into memory the CPU had already
overwritten or reassigned, and it stopped part-way through the command list without ever
reporting completion. That is the wedge every hardware report since v1.1.6 has shown:
`ProcessCommandList` submitted, never completed, the two display transfers behind it stuck
forever, vblanks still arriving, main thread alive.

Frame 2 is not a coincidence. The loading loop drains meshes but never calls `chunkRenderDraw`,
so frame 1 is the first frame that ever submits a chunk draw, which makes frame 2 the first
iteration where an in-flight draw exists to race. steve's v1.2.0 post-mortem says `frame : 2`.

It never reproduced in an emulator because an emulator executes each GX command inline at
submission. There is no in-flight window there to race at all - which is also why nine builds
of emulator testing could not find it.

**The fix:** `gpuWaitPrevFrame()` in `source/main.c`, called once per frame immediately after
`cameraView()` and before `genFollow()` - ahead of every path that can free or overwrite a mesh
slot. It is `gpuTestFrameWait()` (bounded, and it writes `postmortem.txt` rather than hanging
the console) in a build with the safety net compiled in, and `C3D_FrameSync()` otherwise. On a
frame that works it costs nothing: the queue has already drained and it returns at once.

### Corrected

The v1.2.0 entry below claims the missing `GSPGPU_FlushDataCache` calls were the freeze. **They
were not.** steve installed v1.2.0 on his own New 3DS and it froze exactly as before, in single
player and in multiplayer. The flushes were a real defect on hardware with no coherency between
the ARM11 data cache and the GPU's bus, and they stay for that reason alone - but they were
never the cause. If anything, making the CPU's writes reach RAM sooner made the real race land
harder. The comment in `source/scene/chunk_render.c` that asserted the cache theory as proven
has been rewritten to say what actually happened.

### Fixed - two latent crashes found in the same audit

- **`source/scene/highlight.c`, `source/scene/playermodel.c`**: both created a `DVLB_s` and a
  `shaderProgram_s` in `*Init()` and freed them in `*Exit()`, and both run per session - join a
  world, leave, join again. `shaderProgramFree` does not null `program->vertexShader`, and
  citro3d separately caches a pointer to the last program it bound, so rebuilding these statics
  across a rejoin is the same use-after-free that hard-crashes the console and that this project
  has already paid for once. Both now build their shader once for the life of the process and
  never tear it down; the vertex buffers are still freed per session.
- **`source/scene/chunk_render.c`**: `chunkRenderBuild` published a slot's `vert_count` /
  `index_count` / `face_start` *before* the `memcpy` that fills the vertices those counts
  describe. Not reachable today - both halves run on the main thread with no draw between them -
  but it is one refactor away from being real, and it is easy to mistake for the actual race.
  Payload now lands first, metadata second.

### Verified

- Azahar, shipping configuration: world renders correctly, highlight cage draws, counters
  identical to v1.2.0 - cols 322, chunks 2473, meshes 114, tris 88084, cull 81. No
  `postmortem.txt` written, so the bounded wait never fired.
- `arm-none-eabi-nm blocksmith.elf` shows `gpuTestFrameWait` and `gpuTestPostMortem` present, so
  the safety net is genuinely in the shipped binary rather than dropped by `--gc-sections`.

### Not verified

- **The console.** This is a hardware-only bug and no emulator can confirm the fix; only booting
  it on a real 3DS can.
- **The frame-rate cost.** Serialising the CPU behind the GPU gives up their overlap. Azahar's
  timing does not predict hardware and the metrics CSV was empty, so the real cost is unmeasured.
  If the game feels slower than v1.1.0, this is why, and the fix for that is double-buffering the
  mesh slots rather than removing the wait.

### Also worth checking, not a code change

Luma3DS has a **New 3DS CPU** option (Rosalina / boot config) that forces the 804 MHz clock
and/or the extra L2 cache onto titles that never asked for either. `cia/blocksmith.rsf` declares
`CpuSpeed: 268MHz` and `EnableL2Cache: false` - this title explicitly opts out. Luma's own wiki
notes that a few titles are unstable with it forced on. If the freeze somehow survives this
release, set that option to **Off** before anything else.

## [1.2.0] — 2026-08-20

The freeze that killed real 3DS consoles from v1.1.0 through v1.1.8 is fixed.

v1.1.0 froze a real Old 3DS and never reproduced in an emulator. Seven diagnostic builds narrowed it down to this: the GPU was handed a command list and never reported finishing it, so the next frame's `C3D_FrameBegin` blocked forever with the HOME button dead.

The root cause is linearAlloc's memory layout on the 3DS. The CPU writes into a cached region, the GPU is a separate bus master reading physical RAM, and this codebase had zero GSPGPU_FlushDataCache calls anywhere. CPU-written vertex and index buffers sat in the ARM11 data cache, the GPU read stale RAM, followed garbage indices out of the vertex buffer, and stopped partway through the command list without reporting completion. Emulators do not model the CPU data cache, which is why it never reproduced in Azahar.

The proof came from the console itself: the 9264-byte command list the GPU choked on (`cmdhang.bin`) is byte-for-byte identical to the same list replayed successfully in an emulator (`bisect.bin`) — 2316 words, 555 commands, identical register sequence, identical buffer offsets. The list was innocent; the memory it pointed at was stale.

### Added

The frame loop's wait on the GPU command queue is now bounded at two seconds instead of unbounded. On every working frame it costs nothing. If the GPU ever does wedge again the console writes `sdmc:/blocksmith/postmortem.txt` and keeps running instead of locking up with the HOME button dead.

### Fixed

- **The real freeze.** `GSPGPU_FlushDataCache` calls are now issued at every point where the CPU writes a buffer the GPU then reads: chunk vertex buffers and the shared index buffer in `source/scene/chunk_render.c`, the static index buffer at init and each sprite batch's vertex range at flush in `source/gfx/sprite.c`, the block-highlight cage in `source/scene/highlight.c`, and the other-player body mesh in `source/scene/playermodel.c`.
- **A pre-existing compile break in `source/app/watchdog.c`.** The file declared `s_guard_line` inside `#if BS_DRAW_PROBE` but used it unconditionally, so any build without the probe flags failed to compile outright — `error: 's_guard_line' undeclared`. Unnoticed from v1.1.2 through v1.1.8 because every one of those builds set them.

## [1.1.8] — 2026-08-20 — diagnostic pre-release

A GPU pre-flight battery and command-list validator to narrow the real-hardware freeze.

### Added

- Added a GPU pre-flight battery that runs at boot and writes `sdmc:/blocksmith/selftest.txt`: memory fills at three sizes, two display transfer geometries, a texture copy, 200 fills back to back, a timeout control, and a self-check of the new command-list validator.
- Added a per-frame command-list validator that walks every PICA200 command the GPU is given and flags infinities and NaNs in shader uniforms, out-of-range vertex and index buffer addresses, and impossible vertex counts.
- The frame loop now waits two seconds for the GPU instead of forever, so a wedged GPU is reported rather than freezing the console.
- Added a post-mortem that runs while the GPU is still stuck: it proves whether the GPU is alive, replays the stuck command list, and binary-searches the list to name the exact command the GPU cannot execute.
- The command-list capture ring widened from 2 frames to 8.

### Note

This is a diagnostic pre-release, not a fix.

## [1.1.7] — 2026-08-20 — diagnostic pre-release, NEVER PUBLISHED

Built and then withdrawn in favour of 1.1.8, which carries everything below plus
the test battery, so one console boot answers everything instead of two. There is
no v1.1.7 release, tag or CIA. Kept here because the reasoning below is what 1.1.8
was built on.

v1.1.6 named the command the GPU never finished. This build carries that command
off the console so its contents can be read.

### What v1.1.6 said

```
gx queue     : cap 32  queued 5  submitted 5  completed 2
  STUCK ON   : entry 2  type 1  ProcessCommandList - OUR draw commands
    args     : 14189c00 00002430 00000000 00000000
```

Both screen-clearing commands completed. The GPU then accepted the frame's draw
command list — 0x2430 bytes at 0x14189c00 — and never reported finishing it, so
the two display transfers queued behind it never ran and the picture stopped on
the last good frame.

### Why the length is the useful part

`gxprobe.txt` from the same boot measured a healthy frame's command list at
0x2430 bytes as well. A citro3d command list's length is decided purely by the
sequence of calls that produced it, so identical lengths mean the frame that hung
issued *the same commands* as the 334 frames before it and differs only in the
values inside them.

Measured in the emulator this round: two consecutive frames of a stationary scene
produce byte-identical command lists. The noise floor for a frame-to-frame diff is
zero, which makes any difference at all significant.

### Added

- The last two frames' command lists are copied after every `C3D_FrameEnd`. When
  the watchdog fires it writes `sdmc:/blocksmith/cmdhang.bin` (the list the GPU
  never finished) and `sdmc:/blocksmith/cmdprev.bin` (the one it finished
  immediately before). They are decoded off the device, so the decoder can be
  corrected without another boot.
- `hang.txt` gains a `cmd list` section reporting the list's address and length,
  and — the part that can only be established on the console — whether the live
  memory still matches what was handed to the GPU. If it differs, the command
  buffer was overwritten after submission, and that is the bug regardless of what
  either file decodes to.
- `BS_FRAME_HANG_AFTER`, a test-only knob letting a parked arm run normally for N
  frames first. Defaults to 0, so every previously built arm behaves identically.

### Verified before shipping

Both branches of the new comparison were made to fire in the emulator, because a
comparison that has only ever been seen agreeing with itself proves nothing:

- clean arm → `live vs submitted : IDENTICAL`, 120 captures, both files 9264 bytes
- an arm that deliberately flips one byte of the list after the copy is taken →
  `DIFFERS in 1 bytes, first at offset 16 (submitted 00, now ff)`

The first attempt at both arms reported `NOT CAPTURED` and was thrown away: the
park fired on the first frame of the game loop, before any `C3D_FrameEnd`, so the
capture had never run. That is what `BS_FRAME_HANG_AFTER` exists to fix.

### Ruled out this round

Corrupt vertex data cannot be the cause. Chunk positions are `GPU_BYTE, 3` —
three signed bytes in chunk-local space — so wrong vertex data can only ever
produce a wrong-looking block inside a 256-unit cube. It cannot make a NaN, a
huge triangle, or an out-of-range fetch, and therefore cannot wedge the GPU.

That also retires the leading suspicion from the previous round. citro3d flushes
the data cache for exactly two things, measured by disassembling the shipped
binary: its own command buffer in `C3D_FrameEnd`, and decompressed texture data
at load. It never flushes user vertex or index buffers, and this codebase never
flushes anything. That is a real latent defect and it is written down, but with
the vertex format being signed bytes it is not this bug.

### Unchanged

The game itself. Only the reporting is finer. Published as a pre-release so
**Options → Check for Update** never offers it to anyone running v1.1.0.

## [1.1.6] — 2026-08-20 — diagnostic pre-release

v1.1.5 froze a real Old 3DS and returned an unambiguous answer. This build asks
the one question that answer left open.

### What v1.1.5 said

```
draw stage   : C3D_FrameBegin(0) - waiting for the GX QUEUE to drain
gsp vblank   : ALIVE  (+60 / +60 in 1000 ms)
  counters   : 1397 / 1397  then  1457 / 1457
```

Both halves matter, and they point the same way.

`GX QUEUE` rules out frame pacing. The two waits v1.1.5 separated are a wait for
a VBlank tick and a wait for the GPU's command queue to empty; it stopped in the
second. `ALIVE (+60 / +60)` then rules out the obvious explanation for that:
VBlank interrupts were still being delivered at exactly 60 Hz while the console
sat frozen, and the thread that counts them — libctru's GSP event thread — is
the *same* thread that advances the GX queue when the GPU signals a command
finished. It was alive, it was being scheduled, and the queue still never
drained.

So the console is not deadlocked, has not lost an interrupt thread, and is not
starved. The GPU was handed a command and never reported completing it.

### What v1.1.6 adds

`hang.txt` now carries the queue itself:

```
gx queue     : cap 32  queued 5  submitted 5  completed 5
  STUCK ON   : entry 2  type 1  ProcessCommandList - OUR draw commands
    args     : 30189c00 00002430 00000000 00000000
```

`queued` / `submitted` / `completed` are citro3d's own counters, and the entry
named is the oldest command GX has not reported back — which is the one it is
stuck on. Its type splits the remaining possibilities three ways, and they have
three different fixes:

* **type 1, ProcessCommandList** — the world's own drawing. The bug is in what
  the game submits.
* **type 2, MemoryFill** — a colour or depth buffer clear. The bug is in the
  render-target plumbing, not in any geometry.
* **type 3, DisplayTransfer** — copying the finished picture to the screen. The
  bug is in presentation, and the drawing is a bystander.

There is a fourth outcome. If the report says `STUCK ON : NOTHING — every queued
command reported complete`, then the GPU finished everything and the wait still
did not return, and the fault is in libctru's own `isRunning` flag rather than
in the hardware. `gxCmdQueueWait` polls that flag and not these counters, so
that case is real and had to be distinguishable.

### How the new reporting was proved before shipping

A report that cannot be trusted is worse than no report, and neither place this
code normally runs can establish that it works — on hardware the queue state is
the unknown being measured, and an artificially parked build never submits
anything, so it would print an empty queue whether the reader worked or not.

So the same function is also run once from the main thread immediately after
`C3D_FrameEnd(0)`, where the queue is guaranteed to be holding commands, and
writes `sdmc:/blocksmith/gxprobe.txt`. Measured in the emulator:

```
gx queue     : cap 32  queued 5  submitted 4  completed 1
  in flight  : entry 1  type 2  MemoryFill - a colour or depth buffer clear
    args     : 1f0bb800 1a1424ff 1f106800 00000000
  behind it  : entry 2  type 1  ProcessCommandList - OUR draw commands
    args     : 30189c00 00002430 00000000 00000000
  behind it  : entry 3  type 3  DisplayTransfer - copying a framebuffer to the screen
    args     : 1f0bb800 30119400 014000f0 014000f0
  behind it  : entry 4  type 3  DisplayTransfer - copying a framebuffer to the screen
    args     : 1f000000 30000000 019000f0 019000f0
```

Every field decodes to something real: the fill targets VRAM, the command list
sits in the linear heap, and the two transfers are `014000f0` and `019000f0` —
240×320 and 240×400, the bottom and top screens, sideways, exactly as the 3DS
draws them. A build parked deliberately on the queue marker was run separately
to exercise the other branch, and produced `STUCK ON : NOTHING` with the last
completed entry beside it.

### Also

* The hang report buffer went from 1280 to 4096 bytes. v1.1.5's report was
  truncated mid-word at the old size; nothing decisive was lost that time
  because the two lines that mattered came out above the cut, but the queue
  section must not be the thing that falls off the end.
* `gpuCmdBuf` / `gpuCmdBufSize` / `gpuCmdBufOffset` were briefly printed
  alongside the queue and then removed: the self-test measured them as
  `00000000 / 0 / 0`, because citro3d does not route through libctru's `GPUCMD_*`
  layer and those globals are never set in this process. Three zeroes pretending
  to be evidence is worse than no line at all.

### Unchanged

Behaviour. The game does exactly what v1.1.0 does; only the reporting is finer.
Still settled by earlier hardware reports and not revisited here: it is not a
CPU spin, not memory pressure (`linear free 27081216 B`, `vram free 4440064 B`,
identical to the emulator), the world draw completes, and every chunk draw was
validated well formed on the way out (`draw guard : clean`).

## [1.1.5] — 2026-08-20 — diagnostic pre-release

v1.1.4 froze a real Old 3DS and named the stage it froze in. That stage turns
out to be two different bugs wearing one name.

### What v1.1.4 actually said

```
version      : 1.1.4        phase        : DRAW
frames drawn : 336          stalled for  : 10000 ms
columns in   : 322          meshes       : 114
worker       : IDLE         probe arm    : 0
linear free  : 27081216 B   vram free    : 4440064 B
draw stage   : C3D_FrameBegin SYNCDRAW (waiting on the GPU)
draw guard   : clean - every chunk draw issued this session validated
```

Two more things are settled by it and are not being revisited:

- **The world draw completes.** The breadcrumb went past every stage from
  entering `chunkRenderDraw` through `highlightDraw`, the player models, the
  name tags, the bottom screen and `C3D_FrameEnd`. The freeze is in the wait at
  the top of the *next* frame.
- **The chunk draws were well formed.** The guard added in v1.1.4 validated
  every `C3D_DrawElements` issued that session and reported `clean`, so a
  malformed draw is not the cause.

### What that stage name got wrong

`waiting on the GPU` is prose this project wrote into its own report, and it is
only half right. Disassembling citro3d's `renderqueue.o` shows
`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` is **two unrelated waits back to back**,
with v1.1.4's single marker sitting before both:

1. an inlined `C3D_FrameSync()` —
   `do { gspWaitForAnyEvent(); } while (frameCounter[0]==a && frameCounter[1]==b)`.
   That is frame **pacing**. It waits for a VBlank tick and never looks at the
   GPU's progress at all.
2. `gxCmdQueueWait(&ctx->gxQueue, -1)` — the real one: the command list, the
   memory fills and the display transfers all completing.

Stuck in 1 means GSP stopped delivering events, or the pacing counter stopped
advancing, and the world draw is irrelevant. Stuck in 2 means a GPU command
never finished. Different bugs, different fixes, and v1.1.4 could not tell them
apart.

### Added

- **The two waits, called separately.** `main.c` now calls `C3D_FrameSync()`
  and then `C3D_FrameBegin(0)`, which is exactly what
  `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` does internally, with its own marker
  before each. Behaviour is unchanged; only the reporting is finer.
- **A live GSP vblank verdict.** Once a stall is confirmed, the watchdog thread
  samples citro3d's two frame counters, sleeps one second and samples again —
  both from inside the stall, because the question is whether ticks are still
  arriving *now*. `hang.txt` gains a line reading, for example,
  `gsp vblank : ALIVE (+60 / +60 in 1000 ms)`. ALIVE with the console stuck
  means the GX queue never drained; STOPPED means event delivery itself died.
  `C3D_FrameCounter` is three instructions reading a plain `.bss` array — no
  lock, no allocation, no service call — which is the only reason the watchdog
  thread is allowed to call it.

### Verified

Three emulator arms, each driven through world creation into the same world:

| arm | armed at | `draw stage` | `gsp vblank` |
| --- | --- | --- | --- |
| RED_V | `C3D_FrameRate(1.0e-30f)` after 120 frames | `C3D_FrameSync - waiting for a VBLANK TICK, not for the GPU` | `STOPPED (+0 / +0 in 1000 ms)` |
| RED_Q | `-DBS_FRAME_HANG_TEST=WD_DRAW_FRAME_QUEUE` | `C3D_FrameBegin(0) - waiting for the GX QUEUE to drain` | `ALIVE (+60 / +60 in 1000 ms)` |
| control | nothing armed | no `hang.txt` written at all | — |

The verdict came out opposite ways on the two red arms, which is what makes the
line worth reading at all. RED_Q's counters moved `3691 / 3691` to
`3751 / 3751` across the one-second gap — exactly 60 Hz, read live from inside
a stalled console. The control played normally with the world on screen and the
stats line reading `cols 322  chunks 2473  meshes 114  tris 88084  cull 81`.

The first attempt at RED_V used `C3D_FrameRate(0.0f)` and wrote no report at
all. That is not a passing test, it is a skipped one — and the reason is worth
writing down: `C3D_FrameRate` opens with `vcmpe.f32 s15, #0.0` / `bxle lr` and
rejects anything at or below zero, so the arm was a no-op that could never have
fired. The accepted range is `0 < fps <= 60`. `1.0e-30` sits inside it and
still stalls the counter, because after the first tick `framerateCounter` holds
`60.0f` and `60.0f - 1.0e-30f` rounds back to `60.0f`, so it can never reach
zero again.

**The freeze itself is still not fixed, and this build does not claim to fix
it.** It is a pre-release for one reason: so the in-app updater never offers it.

## [1.1.4] — 2026-08-20 — diagnostic pre-release

v1.1.3 worked. It froze a real Old 3DS and wrote the report it was built to
write. This release exists because that report answered less than it looked
like it did.

### What v1.1.3 actually said

```
version      : 1.1.3        phase        : DRAW
frames drawn : 336          stalled for  : 10000 ms
columns in   : 322          meshes       : 114
worker       : IDLE         probe arm    : 0
linear free  : 27081216 B   vram free    : 4440064 B
draw stage   : finished, back in main.c
```

Two things are settled by it and are not being revisited:

- **It is not a CPU spin.** The breadcrumb passed `CULL`, the one stage that
  issues no GPU commands at all, and went past every draw after it.
- **It is not memory.** `linear free` and `vram free` are byte-for-byte what the
  emulator reports on a scene that runs fine.

The mistake was in reading `finished, back in main.c` as "the CPU finished the
whole frame". It does not say that. It says `chunkRenderDraw` returned — and six
more GPU submissions happen after it inside the same `DRAW` window, none of
which had a marker of its own, so all six looked identical in the report:
`highlightDraw`, `playerModelDraw`, `playerModelDrawTags`, the bottom screen's
clear and sprite batch, `C3D_FrameEnd`, and the next frame's `C3D_FrameBegin`.

### Added

- **The rest of the frame's breadcrumb.** Seven new `WD_DRAW_*` stages covering
  every one of those six submissions plus the per-eye setup, so the next report
  names the submission the console died on instead of the neighbourhood it died
  in. One 32-bit store each, in a diagnostic build only.
- **A draw guard.** Every chunk draw is checked before it is issued, against four
  ways a `C3D_DrawElements` can be malformed: a vertex pointer that is not the
  start of a real mesh slot, an index count that is not whole triangles, an index
  range that runs off the shared index buffer, and a maximum index that reaches
  past the bound slot's vertex capacity. A bad draw is refused rather than
  issued, and `hang.txt` gains a `draw guard` line either naming it or saying
  `clean`. `clean` is the more useful half of the answer: it would mean the
  commands were well formed and the GPU stopped for some other reason.

### Ruled out by measurement, not by argument

- **Command-buffer overflow.** citro3d does not bounds-check its command buffer,
  and a frame that overran it would hand the GPU whatever follows it in the
  linear heap. Measured with `C3D_GetCmdBufUsage` on the same 322-column scene:
  `cmd 3.5% max 3.5%` of `C3D_DEFAULT_CMDBUF_SIZE` (0x40000 bytes). Not close.
- **Missing GPU cache flushes.** Every vertex buffer here is written by the CPU
  into cached linear memory and read by the GPU, and nothing in this repo calls
  `GSPGPU_FlushDataCache`. It does not need to: disassembling citro3d's
  `C3D_FrameEnd` shows it calls `GSPGPU_FlushDataCache(__ctru_linear_heap,
  __ctru_linear_heap_size)` on every frame that is not `C3D_FRAME_NONBLOCK`.

### Verified

The instrument was proven able to fail before being shipped, on three emulator
arms driven through world creation into the same world:

| arm | armed at | `draw stage` in `hang.txt` |
| --- | --- | --- |
| RED_B | `-DBS_FRAME_HANG_TEST=WD_DRAW_BOTTOM` | `drawBottomUi (bottom screen + UI sprites)` |
| RED_E | `-DBS_FRAME_HANG_TEST=WD_DRAW_FRAME_END` | `C3D_FrameEnd (submitting to the GPU)` |
| control | nothing armed | no `hang.txt` written at all |

The control arm played normally and reported `meshes 114  tris 88084  cull 81`,
the same figures the console reports, so the markers cost nothing and cannot
fire on their own. The draw guard was proven separately: a build that corrupts
its 300th chunk draw produced `BAD DRAW code 4 hits 1 slot 112 first 0 count
12288 maxidx 8191 cap 4096`, and the unmodified build produced `clean`.

**The freeze itself is still not fixed, and this build does not claim to fix
it.** It is a pre-release for one reason: so the in-app updater never offers it.

## [1.1.3] — 2026-08-20 — diagnostic pre-release

The same hunt, and the same instrument as v1.1.2 — but v1.1.2 could not carry it
out, because of a bug in the harness rather than in the game.

### What went wrong with v1.1.2

Installed on real hardware it booted, loaded a world, and drew nothing but the
highlight cage: "I can just see the outline of the blocks, I can't actually see
the world." It did not freeze either, which is the part that gives it away — a
build meant to freeze that comes back alive has not been tested, it has been
skipped.

The cause is v1.1.1's `sdmc:/blocksmith/drawprobe.txt`, still on the SD card.
v1.1.2 read it to decide which arm of the bisect to run next, saw `arm 5 START`
as the last line, concluded every arm had already had its turn, and took the
park branch — `s_probe_arm = PROBE_ARMS - 1`, arm 5, "GPU state set up, but no
draw calls at all". So the console was obeying instructions left behind by a
version that was no longer installed. Multiplayer worked, worlds loaded, the
stats line counted meshes and triangles; they were simply never drawn.

That is also why the breadcrumb v1.1.2 exists to collect came back empty. An arm
that issues no draw calls cannot reach the stage the breadcrumb is there to
name.

### Fixed

- **`BS_DRAW_BISECT`, defaulting to off.** The shipped diagnostic now runs one
  arm on every boot — arm 0, everything drawn — regardless of what any previous
  version left on the card. Nothing is removed from the frame, so it looks like
  the game. The draw-stage breadcrumb added in v1.1.2 is what does the work now,
  and it needs the whole frame present to be worth reading. The bisect code is
  kept and can still be switched on with `-DBS_DRAW_BISECT=1`; it is simply no
  longer the instrument.
- The `HUNG` epitaph for a previous frozen boot is still written, because "the
  last boot froze" is exactly what a build that is meant to freeze must record.

### Verified

Red and green, against the exact situation that produced the report. The real
`drawprobe.txt` off the console was planted on a private emulator SD card, and
both builds then booted into the same world from it.

With v1.1.2 as shipped: the log gained `-- all arms done; parked on arm 5 --`
and the screenshot is a dark screen with a floating wireframe cube outline and
nothing else — the report reproduced exactly.

With this build, same planted file: the log gained `arm 0 START  everything`
then `arm 0 SURVIVED 600 frames`, and the screenshot shows grass, dirt, a tree
trunk and the highlight cage. The bottom-screen stats line reads identically in
both (`meshes 114  tris 88084  cull 81`), which is the control — the world was
loaded and meshed either way, and only the drawing differed.

**The freeze itself is still not fixed, and this build does not claim to fix
it.** It is meant to freeze. That is now possible again, and when it does,
`hang.txt` names the draw stage.

## [1.1.2] — 2026-08-20 — diagnostic pre-release

Still not a normal release, and still the same hunt. v1.1.1 asked the console
six questions and cost six boots to get six answers; v1.1.2 asks the one
question that is left and answers it on the first boot that freezes.

### What v1.1.1 came back with

`drawprobe.txt` off the real console: arm 0 `HUNG`, arm 1 (no world)
`SURVIVED 600 frames`, and arms 2, 3 and 4 all `HUNG`. Only the arm that skipped
the world survived — and it survived while still drawing the highlight cage,
which matches what was on the screen ("I could see the outline of a block, but
not the actual world"). So the freeze is inside `chunkRenderDraw()` in
`source/scene/chunk_render.c`, and nothing else in the frame is implicated.

`hang.txt` from the same session ruled out memory pressure by measurement rather
than by argument: `linear free 27081216 B` and `vram free 4440064 B` on hardware
are byte-for-byte what the emulator reports, so nothing is running out.

One correction, because it is in that log file and it is wrong: an `arm 1 HUNG`
line printed after `arm 1 SURVIVED`. v1.1.1's bookkeeping compared the total
number of START lines against the total number of SURVIVED lines, so one genuine
hang left those totals permanently one apart and every later boot invented a
`HUNG` for an arm that had already survived. Fixed here — it now tracks the last
arm to start and whether that same arm's own survival line followed.

### Added

- **A draw-stage breadcrumb.** `chunkRenderDraw` now records where it is —
  entered, culling, binding, opaque pass, transparent pass, done — plus the loop
  index and mesh slot when it is inside a pass, and the watchdog prints all
  three into `hang.txt`. The distinction it is built around is `CULL` versus
  everything after it: `cullFrame` issues no GPU commands at all, so a freeze
  recorded there is the CPU spinning, and a freeze recorded later is the CPU
  waiting on a GPU that never finished. From outside those two look identical,
  which is why no amount of watching from `main.c` could separate them.
- **A second bisect round**, kept as corroboration rather than as the primary
  instrument: six arms that cut *inside* `chunkRenderDraw` — everything, the
  whole call skipped, cull only, opaque only, transparent only, and state setup
  with no draws.
- **`BS_DRAW_HANG_TEST`**, the red arm for the breadcrumb. It stalls the main
  thread deliberately partway through the opaque pass, so the report can be
  checked for saying the right thing rather than assumed to.

### Verified

Every claim above was run, not reasoned about. In Azahar, with the stall planted
at opaque-pass loop index 3, `hang.txt` came back `draw stage : opaque pass`,
`loop index : 3`, `mesh slot : 57`. With the stall planted outside the draw
instead, the same field read `finished, back in main.c`, `-1`, `-1` — so it
tracks position rather than being a constant. Four consecutive emulator boots
produced `arm 0 SURVIVED`, a deliberately killed `arm 1 HUNG`, `arm 2 SURVIVED`
and `arm 3 SURVIVED` with **no** spurious `arm 2 HUNG`, which is the bookkeeping
fix going green on the exact case that was red. Two more boots covered arms 4
and 5, and screenshots confirm each cut is real: arm 2 draws no world at all,
arm 4 draws no ground.

**The freeze itself is not fixed, and this build does not claim to fix it.**

## [1.1.1] — 2026-08-20 — diagnostic pre-release

Not a normal release. v1.1.0 froze a real Old 3DS a moment after a world
finished loading, HOME included, and the failure does not reproduce in an
emulator. This build is the instrument that finds it. Nothing about the game
changed.

### The evidence it was built from

The watchdog wrote `sdmc:/blocksmith/hang.txt` on the frozen console: phase
`DRAW`, 310 frames drawn, `columns in 322`, `meshes 99`, mesh queue empty,
worker idle. Those two counters only exist after a `C3D_FrameEnd`, so a whole
world frame completed and the GPU never signalled it — and the next
`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` then blocked forever, which is also why
HOME stopped answering. Ruled out with evidence rather than intuition:
non-linear GPU buffers (every one is `linearAlloc`'d), cache coherency
(`C3D_FrameEnd` flushes the whole linear heap), NULL vertex buffers (checked at
init, fatal), zero-count draws (guarded in `drawIndices`/`drawRun`), mesh-slot
exhaustion (99 of 150), stereo (never enabled), and the loading→game handoff
(the phase name proves it was survived).

### Added

- **`BS_DRAW_PROBE`, a self-advancing six-arm draw bisect.** Each boot removes
  one thing from the frame — arm 0 nothing removed, then the world, the
  highlight cage, the player models and tags, the bottom-screen UI, and finally
  everything — and appends the result to `sdmc:/blocksmith/drawprobe.txt`. Arms
  0 and 5 are controls: 0 must hang and 5 must survive, and the bisect means
  nothing without both. A started arm with no survival line is written up as
  `HUNG` on the *next* boot, because a frozen console cannot write its own
  epitaph.
- **A finer watchdog phase would not have worked**, which is why this exists
  instead: every draw call only appends to a command list and returns, and the
  block happens at the next `C3D_FrameBegin`. There is no moment in between to
  put a marker in. Removing one draw and seeing whether the console lives is the
  only instrument that separates them.
- **Three extra `hang.txt` lines** — `probe arm`, `linear free`, `vram free` —
  present only in this build. Sampled on the main thread and merely read by the
  monitor: `linearSpaceFree()` takes libctru's heap lock, and a watchdog that
  can block on a lock is a watchdog that writes no report at all.

### Verified

The harness was proven both ways in Azahar before being cut, because a check
that cannot go red proves nothing: three boots gave `arm 0 SURVIVED`, a
deliberately killed run gave `arm 1 HUNG` on the boot after it, and arm 2's
screenshot shows the same world with the highlight cage genuinely gone. The new
`hang.txt` lines were proven by a throwaway `BS_HANG_TEST` build that stalls the
main thread on purpose, which wrote `probe arm : 0`, `linear free : 27081216 B`,
`vram free : 4440064 B`.

**The freeze itself is not fixed, and this build does not claim to fix it.**

## [1.1.0] — 2026-08-20

Multiplayer that actually works end to end: joining a server puts you in the
server's world, your edits reach it, everyone else's edits reach you, and a
session that ends says so instead of pretending.

**Still not tested on real hardware.** Every figure below was measured in
Azahar against a real gateway — the live server for the final check, a local
`bsgate`/`bsgame` pair for the fault injection — and Azahar does not emulate
GPU cost.

### Added

- **Connect enters the server's world directly.** There is no name prompt, no
  NEW WORLD and no trip through world select on the multiplayer path, because
  none of those describe a world this console owns. The join waits one round
  trip for `BS_APP_WORLD_INFO` — the seed the server generates from — and only
  then enters, showing "Joined - waiting for the world..." while it waits.
  Entering on the handshake alone would have generated from this client's own
  seed: a private landscape that looks exactly like a successful join.
- **A server session writes nothing to the SD card.** The worker's save
  directory and the inventory directory are both NULL on that path, and
  `saveWorldDir()` is not called at all rather than called and ignored, because
  calling it is what creates the directory. Measured: the world list is
  byte-identical before and after a full join → dig → leave → rejoin cycle.
- **Leaving a server session returns to the title screen** instead of quitting
  the app, landing on the Multiplayer screen so the reason it ended is on
  screen. START in a joined world now hangs up cleanly, frees the slot on the
  gateway, and comes back ready to connect again.
- **A lost server is detected.** The keepalive probe is now armed off *receive*
  silence rather than send silence, retried on a deadline, and eventually
  fatal — five retries at 2 s after 4 s of quiet, so a dead server is called at
  roughly 14 s with "Lost the connection to the server". Before this the client
  sat in Connected forever with a full HUD and edits going nowhere.
- **A multiplayer HUD line**: `net s… r… y… a… q… p…` — edits sent, payloads
  received, WORLD_SYNC entries seen, remote edits applied, edits still queued,
  remote players. A session that is connected but silently moving nothing used
  to look identical on screen to one that works.

### Fixed

- **Joining no longer discards the world's history.** The server's post-JOIN
  `WORLD_SYNC` batch was being flushed into the 17×17 grid of *empty* columns
  that `worldReportBuild()` allocates as a memory-budget proof, before the
  generator had run — so every edit landed in air and the generated terrain
  buried it. Diffs are now held until the column each one belongs to is
  generated and installed. Measured against the live server: dig 8 blocks,
  leave, rejoin — `y15` sync entries back (7 already stored there plus the 8
  new), the player standing inside the shaft at `aim 8 48 8`, and 88064
  triangles, identical to the frame at the end of the dig. Before the fix the
  same run gave `y8 a0` with the ground visibly whole.
- **The multiplayer menu could not complete a handshake.** `netUpdate()` is
  where every retry and every arriving packet is processed, and the title
  screen's loop never called it — so Connect sat on "Connecting..." forever,
  the HELLO was never retried and the server's COOKIE was never read off the
  socket. Both `netUpdate()` and `networldUpdate()` are now pumped there.
- **The gateway now reports a dead game daemon.** `bsgate` answers keepalive
  probes itself, so a client stayed Connected with `bsgame` dead behind it and
  no explanation anywhere. It now logs "game daemon unreachable", counts the
  messages lost, logs the recovery, and exposes `game link` in its status —
  measured as `game_link down` / `game_down_s 11`, then "back after 13 s, 130
  message(s) lost" on restart. **This half ships in the server repo**; the
  console side of the release does not depend on it.

### Verified

- **Two clients, one server.** Two emulator copies joined at once — server
  reported `sessions 2 / 16`, `joins total 2`. One dug 8 blocks; the other,
  never touched between joining and the reading, showed `a8` applied remote
  edits with its own break counter still at `b0`, and could aim into the bottom
  of the shaft the first one made.
- **Against the live server**, not just a local rig: connect, break, sync, and
  the edits still there after leaving and rejoining.

## [0.2.0] — 2026-08-19

Creating a world no longer looks like a freeze, the game can update itself, and
a hang now leaves evidence behind.

**Still not tested on real hardware.** Every figure below was measured in
Azahar, which does not emulate GPU cost.

### Added

- **In-app updater** — Options → Check for Update. Asks GitHub for the newest
  release, and if it is newer, streams the `.cia` straight into the AM service
  and relaunches into it. The version check reads the tag out of the
  `releases/latest` redirect rather than the API, because `api.github.com`
  allows only 60 unauthenticated requests an hour per IP and a check that fails
  at random is worse than no check. The certificate bundle needed to verify
  github.com ships in the RomFs (`romfs/cacert.pem`) — the console's own root
  store predates every CA in use today.
- **A loading screen for world creation.** Heading, world name, a progress bar,
  the stage in words, and live counters: columns in, meshes, queued, holes,
  refusals, worker state, elapsed seconds. If generation stops making progress
  it says so and offers **A — play anyway** or **START — quit**, rather than
  leaving you looking at a frame that never changes.
- **A watchdog thread** (`source/app/watchdog.c`). If the main thread stops
  completing frames for ten seconds anywhere other than the HOME-menu wait, it
  writes `sdmc:/blocksmith/hang.txt` naming the phase it stopped in, the frames
  drawn, and what the world was doing. It writes through the raw FS service
  rather than stdio, because a main thread stuck inside newlib holds newlib's
  lock and an ordinary write would deadlock with it.

### Changed

- **Creating a world is 4.7× faster: 30.44 s → 6.50 s**, and the loading screen
  now appears within **0.60 s** instead of 23.95 s. The cause was measured, not
  guessed — an instrumented boot attributed **23,855.6 ms of 24,133.8 ms
  (98.9 %)** to the on-console world self-test, which builds and tears down
  whole worlds across 818 assertions on a 268 MHz ARM11. That self-test is now
  off in release builds (`BS_SELFTEST`, default 0). The identical code still
  runs on the host on every build — 2,694 checks of it — and can be switched
  back on with `make EXTRA_CFLAGS="-DBS_SELFTEST=1"`.

### Fixed

- **The loading screen drew nothing at all** in its first build: every quad was
  batched and never submitted, because `loadingDraw()` returned without calling
  `spriteEnd()`. On screen that is indistinguishable from the frozen boot the
  screen was written to replace.

### Known issues

- A freeze reported on real hardware after creating a world — no terrain on the
  top screen, the HUD still showing its last numbers, and the HOME button dead —
  **has not been reproduced** in Azahar on either console profile, and nothing
  in this release is proven to fix it. The watchdog above exists so that the
  next occurrence names itself.

## [0.1.0] — 2026-08-19

First public release. Playable single-player: generate a world, walk around it,
break and place blocks, and come back to it later.

**Not tested on real hardware.** Every figure below was measured in Azahar,
which does not emulate GPU cost.

### World and rendering

- Infinite chunked world from seeded value noise, with a biome pass, trees and
  3D-noise caves. Generation runs on a worker thread with a per-frame job
  budget, so it never blocks a frame.
- Greedy-ish chunk mesher with per-face-direction draw buckets and a draw mask
  that merges adjacent runs.
- Cave culling and horizon culling. Cave culling alone removes 25 chunks of 45
  for 8.25 µs a frame.
- Fixed-function fog matched to the render distance, so the load boundary is
  never visible.
- Render distance is a setting, and the near plane, far plane and fog curve are
  all derived from it rather than tuned separately.

### Playing

- DDA voxel raycast for targeting, with a highlight cage on the face you are
  aimed at. Break with X, place with Y.
- Player physics with collision, gravity and jumping.
- Touchscreen inventory, hotbar and a three-recipe crafting table
  (dirt → grass, stone → sand, leaves → dirt).
- Title screen and world select: create as many worlds as you like, each with
  its own directory on the SD card.
- Options screen — render distance, look sensitivity, inverted pitch — and full
  button rebinding, saved to `sdmc:/blocksmith/options.ini`.

### Saving

- Region save format with a CRC32 per column and torn-write recovery: a power
  cut mid-write costs that one region, never the world.
- Inventory saved alongside, checksummed the same way.

### Packaging

- Installable `.cia` built with makerom, with a Home Menu icon and banner
  generated by `tools/make_banner.py`.
- `blocksmith.3dsx` for the Homebrew Launcher.

### Performance work

- Palette-compressed chunk storage: a chunk holding 16 or fewer distinct blocks
  stores one nibble per cell instead of a byte. Encode 7,435 → 4,085 ns,
  decode 14,040 → 3,165 ns.
- One shared index buffer across every chunk, and a three-tier vertex slab pool
  (512 / 1024 / 2048 face slots) so a small chunk stops claiming a large slab:
  **9.40 MB → 4.24 MB**.
- `chunkCopyRun`'s palette path unpacks two cells per byte rather than calling a
  nibble accessor per cell. Measured on the console against a control build:
  **292 → 241 µs** per chunk of neighbourhood gather, with the mesher's own
  column unmoved (918 → 917 µs) as the control and identical geometry hashes on
  both sides.
- Atlas trimmed and its filtering fixed; column height ranges and uniform-chunk
  flags let whole chunks be skipped without inspection.

### Fixed

- The entire world rendered flat brown. `source/shaders/world.v.pica`'s
  `uvScale` was still 1/256 after the atlas shrank to 64×64, so every tile's UVs
  collapsed into one cell. A host test now parses the shader source and asserts
  the constant against `ATLAS_PX`, because a `.pica` file cannot include a C
  header and so cannot be static-asserted.
- Creating a world could fail without saying anything: the flow returned
  silently on every outcome except success, which looks exactly like a button
  that does nothing. It now reports the numeric keyboard result, an invalid
  name, or an SD write failure. Cancel is still silent, as it should be.
- The host test script shared one output directory across concurrent runs, so a
  stray binary from another session could be executed instead of the one just
  built. Each run now gets its own directory.

### Known limitations

- Never run on real hardware.
- Mipmaps are not enabled; the distance shimmer that implies is unaddressed.
- Meshing is single-core.
- Multiplayer is in progress and not part of this release. Building from a
  clean clone therefore fails — see the note in the README.
