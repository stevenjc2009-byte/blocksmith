# Changelog

All notable changes to Blocksmith. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/).

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
