# Changelog

All notable changes to Blocksmith. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/).

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
