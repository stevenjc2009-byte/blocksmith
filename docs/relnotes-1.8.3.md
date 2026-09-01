## [1.8.3] - 2026-09-01

Every world was the same world. "New World" generated the same landscape on every console
every time, because the seed was compiled into the game and the only way to see different
terrain was to rebuild it. And every part of that one landscape was the same place: one
surface material, one plant, one tree drawn at six different densities. 1.8.3 is the release
where a world becomes *a* world — its own seed, six biomes with their own ground, their own
plants and their own tree shapes — and where the underwater trap that made a lake a prison
gets its way out.

**Worlds made in 1.8.2 and earlier open unchanged, and that is measured rather than argued.**
Twelve pinned columns across three seeds come back byte-identical. See Compatibility.

### Added

- **Six biomes.** Every column is classified on a temperature/humidity rectangle into tundra,
  taiga, plains, forest, desert or jungle, and the biome decides the surface material, how
  much grass grows, which plants appear and what shape the trees are. Temperature is the
  existing terrain field read the other way up; humidity is a new one on its own salt, so the
  two vary independently and a hot place is not automatically a dry one.
- **Five new kinds of ground and plant** — snow, ice, cactus, dead bush and fern. Snow caps
  tundra ground as a single top layer, so a tundra cliff shows the dirt band under a white
  lid rather than being snow all the way down. Ice replaces the top water cell of any cold
  sea — tundra *and* taiga, deliberately, because a taiga shore of open water beside a frozen
  tundra shore would read as a bug rather than as a boundary. Cactus and dead bush scatter on
  desert sand; ferns grow in taiga and jungle. All five are drawn with new hand-generated
  16x16 tiles in atlas slots 12 to 16.
- **Every biome grows its own tree.** Until now the only thing a biome could change was how
  *often* a tree appeared: one hard-coded 4-6 block trunk under a fixed 2-block crown, so a
  taiga, a jungle and a plains grew identical geometry at different densities. There are now
  three crown silhouettes — a round crown, a tiered conifer rising to a tip, and a broad flat
  jungle canopy on a long trunk — and each tree also varies within its biome: its own canopy
  width, its own tier count, and in the biomes that allow it a 2x2 trunk, taken only when all
  four ground columns are level so a wide trunk never stands on a step.
- **Per-world terrain seeds.** A brand new world mints its own seed and stores it beside the
  save. Two new worlds on the same console are now two different landscapes.
- **A joined server that generates a different world is refused at the door.** Terrain is
  never transmitted — every client generates it from the server's seed — so a client and a
  server that disagreed about the generator were standing in two different worlds over the
  same numbers: one player's floor was another's sky, and every block edit landed in the
  wrong hillside, with nothing logged and nothing errored because both sides believed they
  agreed. The server now declares its generator on the wire and a client that cannot produce
  it says so and returns to the title screen. A server that says nothing at all is treated as
  the old generator, not as a refusal — that is a compatibility promise compiled into every
  client from this release on.

### Changed

- **The relight queue now has a per-frame budget.** Lighting work after an edit used to be
  drained to empty inside a single frame under a comment claiming it was multiplayer-only and
  bounded to 49 columns. Both claims were false — the queue holds 64, and single player feeds
  it on every water movement, so a water cascade could drain an arbitrary number of columns in
  one frame. Timed directly rather than quoted: 240 samples over six column profiles gives a
  median of 0.156 ms and a max of 0.287 ms per column, so 64 unbudgeted columns is 9.98-18.4
  ms against a 16.71 ms frame — a dropped frame on the host PC before any 3DS penalty. The
  cap is four columns or 1.0 ms, whichever binds first. **Nothing is dropped**: a column not
  reached this frame is relit on a later one.
- **A lighting engine that fails to allocate now says so.** Its 65,544-byte allocation was
  requested and the result thrown away; a refusal left the engine enabled and queueless, and
  every relight for the rest of the session silently took the 26x slower path with no flag,
  no counter and no log line. The slow path is still the fallback — sweeps beat darkness —
  but it now reports itself.
- **The console build treats warnings as errors.** Every warning the compiler can raise about
  this game now stops the build instead of scrolling past.
- **The HUD's aim and network lines are sized to what they can actually hold**, rather than to
  a width someone once eyeballed, and the network line now shows block edits the server
  refused instead of dropping them silently.

### Fixed

- **You can get out of the water.** Swim to a shore and press into it and you climb out. This
  was reported as "as of right now, I am unable to leave the water", and it was exactly true:
  sea level puts a bank flush with the water surface, a floating body's feet sit 1.135 blocks
  below that surface, the swim button holds you at the waterline rather than lifting you past
  it, and the one-block auto-climb that carries a walker over a kerb refuses to run unless
  you are standing on the ground — which a floating body never is. Pressed into the bank you
  simply stopped, every frame, forever. Climbing out now finds the water plane rather than
  assuming a fixed step up, because the gap between your feet and the surface is not a whole
  number and is not even a constant — it measures 11.865 at 60 fps against 11.894 at 30, so a
  flat one-block lift lands inside the bank and would have failed at every frame rate. A bank
  standing a block proud of the water is still too high to climb, and a fully submerged diver
  cannot use this to walk up a cliff.
- **The water surface has a gentle swell again.** 1.8.2 removed the bobbing because it was a
  physics feedback loop — 0.223 blocks at 4.4 Hz, which strobed the water plane through the
  camera. This one is a quarter of that amplitude at a twelfth of the frequency, 0.055 blocks
  at 0.35 Hz, and it moves **the camera only**: it cannot move your body, cannot affect what
  you collide with, and cannot form a loop, so the failure 1.8.2 fixed cannot come back
  through it. Floating leaves your eyes 0.485 blocks clear of the water, nearly nine times the
  swell, so the view never dips under the surface.
- **The battery gauge blinks when the battery is critical.** One second a cycle, lit for the
  first 600 ms. At zero bars the outline pulses too, since there is no bar left to hide. It
  never blinks while charging, and it never blinks on a console whose battery service failed
  to open — a gauge that cannot read the battery is drawn as switched-off, not as a warning.
- **Truncated save paths are refused instead of acted on.** A world whose name pushed a file
  path past the buffer used to have that path silently cut short — which names a *different*
  file. Four places did this: region files, the generator stamp, the seed sidecar and a test
  fixture. All four now refuse the operation and say so.
- **A refused world returns to the title screen** rather than stalling on a black screen.
- **The world-edit hook is told which world was written to**, so an edit can no longer be
  attributed to the wrong one.
- **Byte counts are counted, not requested.** Two places recorded how many bytes were *wanted*
  rather than how many were written, which over-reports on truncation.
- **Water evicts the oldest pending candidate on a full queue, not the newest**, so a busy
  cascade no longer discards the change that just happened in favour of one from ten ticks ago.
- **A negative squared distance is treated as far away, not as full rate.**
- **The CIA build lets the release script name its own Python**, so building the banner assets
  no longer depends on which interpreter happens to be first on the path.
- **The release tooling refuses an unedited what's-new template**, which is the one gate that
  used to be possible to pass by forgetting.

### Compatibility

- **No save-format break, and no block id moved.** `BLOCK_COUNT` is unchanged at 8. The five
  new blocks are ids 10 to 14, deliberately outside that count, exactly as water and tall grass
  already are — so a 1.8.3 client and a 1.8.2 server still agree about the block registry, and
  no existing region file re-encodes.
- **The new blocks are scenery, not items.** Snow, ice and cactus cannot be broken; dead bush
  and fern break and yield nothing, identical to tall grass. That is a consequence of the id
  ceiling and it is stated here rather than left to be discovered. Making them collectable
  later is a change to the *rule* about which ids fit in a bag, not a change to the ceiling —
  sliding the ceiling would drag water and tall grass off the rows every saved region encodes.
- **Existing worlds keep their exact terrain, measured.** The generator is versioned per world:
  a world stamps which generator made it, and that stamp — not the build — decides what gets
  generated for it, forever. A new generator version, 3, was minted for the biome work, and no
  released build has ever written it, so every world that exists is version 1 or 2 and never
  consults any of the new tables. A probe built twice, once against a detached 1.8.2 tree and
  once against this one, hashed twelve columns across three seeds: **IDENTICAL, all twelve.**
- **A world stamped by a build newer than yours is refused, not guessed at**, and so is a stamp
  that is present but damaged. Generating a world at the wrong version is indistinguishable
  from corrupting it.
- **A world made before 1.8.3 keeps seed 1337**, which is what every world was generated from,
  and gets a seed file written beside it recording that. Only brand new worlds mint fresh seeds.
- **Servers ship first.** A 1.8.3 server with a 1.8.2 client is a visible, degraded mismatch.
  The reverse must never ship: an older server accepts the join and every id 10-14 in the world
  resolves to air — an invisible hole indistinguishable from a cave.

### Corrected

The 1.8.3 cycle also went back over claims this project had already made and found some of
them false. Recorded because a wrong number that was measured once is worse than no number:

- The atlas free-slot count, the relight cost figure, the render-distance ceiling comment, the
  worker stack margin, the New 3DS lighting note, the water eviction justification, the claim
  that the server verifies the block-registry checksum, the biome field's stated distribution,
  and a statement about what the console build treats as an error — all were stale or wrong
  and are corrected in place.
- Six user-facing claims in earlier documentation were false and are fixed; two figures that
  had no stated origin are now labelled with theirs.
- The README no longer says worlds share one fixed seed.

### Testing

- A green test suite over a broken feature is this project's signature failure, and 1.8.3
  spent a large share of its commits closing that hole rather than adding features: check
  counts are now pinned in fourteen more suites, so a deleted check fails the run instead of
  passing it quietly; the atlas test fails loudly when its built sheet is missing or stale;
  the block-registry and remote-player capacities are asserted against their wire twins rather
  than against themselves; the shader test asserts the uniforms are wired rather than merely
  declared; and the interop suite — the real network code against a real server daemon over a
  real socket — was run for the first time.
- A red-arm sabotage left behind in an inherited working tree was found and removed. It read
  like deliberate code and compiled clean, and it meant cactus and dead bush were never placed
  at all while ferns grew in every grass biome.

### Not verified

- How the surface swell and the battery blink **look**. 0.055 blocks at 0.35 Hz is a number,
  not a feel, and no automated check here can tell a gentle swell from an imperceptible one.
- Nothing in this release has run on real hardware. Every measurement above is from the host
  test suite or from Azahar.

