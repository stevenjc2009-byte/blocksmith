# v1.8.9 weather rendering — integration hand-off

Written by the rendering lane, for the lane that owns `source/main.c` and
`source/scene/chunk_render.c`. Those two files were explicitly off-limits to
the rendering lane (a third, concurrent lane also touches `main.c`), so
nothing below has been applied — these are the exact lines to add, and the
exact reasoning for where each one goes, verified against the real, current
`main.c` (not guessed) as of this writing.

The four files this lane owns outright and does not expect anyone else to
edit: `source/gfx/weatherdraw.c`, `source/gfx/weatherdraw.h`,
`tests/weatherdraw_test.c`, plus a stanza appended to
`tools/run_host_tests.sh`. Supporting assets also created and owned by this
lane: `tools/make_weathertex.py`, `gfx/weathertex.t3s` (+ the `.png` it
generates), `source/shaders/weather.v.pica`.

## 1. What weatherdraw.c/.h actually expose

```c
void  weatherDrawStateInit(WeatherDrawState* st);
void  weatherDrawSetState(WeatherDrawState* st, WeatherKind kind);
void  weatherDrawUpdate(WeatherDrawState* st, float dt,
                         float cam_x, float cam_y, float cam_z);
bool  weatherDrawShouldDraw(const WeatherDrawState* st);

#ifdef __3DS__
bool  weatherDrawInit(void);
void  weatherDrawExit(void);
void  weatherDrawDraw(const C3D_Mtx* projection, const C3D_Mtx* view,
                       const WeatherDrawState* st);
#endif
```

`WeatherDrawState` is a small POD struct (kind, scroll/drift phase, snapped
grid position) — own one instance as file-static state in `main.c`, the same
shape as the other file-static state (`s_daynight`, `s_gen`, `s_world`, …)
already living there.

This module does **not** call `weatherAt()` itself — it only includes
`world/weather.h` for the `WeatherKind` enum type. That call, and its
recommended polling cadence, belongs to `main.c` (§3 below); weatherdraw.c
stays decoupled from `WorldGen`/`World` entirely.

## 2. One-time init / exit

Add near the other one-time GPU-object `*_Init()` calls in `main.c` (grep for
`chunkRenderInit(` around line 3438 — same phase, same shape):

```c
if (!weatherDrawInit()) {
    // same failure handling shape as any other *_Init() that can fail here —
    // whatever that project convention already is (log + continue with the
    // feature silently absent is consistent with weatherDrawShouldDraw()
    // returning false / weatherDrawDraw() no-op'ing on a not-ready module).
}
```

And at the `app_shutdown:` label, alongside `chunkRenderExit()` (~line 5597):

```c
weatherDrawExit();
```

Also declare the file-static state once, near `s_daynight`/`s_gen`/`s_world`:

```c
static WeatherDrawState s_weatherdraw;
```

and call `weatherDrawStateInit(&s_weatherdraw);` once, at the same point the
other file-static world state gets initialized for a new/loaded world (not
at process boot — it needs a real world to poll `weatherAt()` against).

## 3. Per-frame: poll the model, update the animation state

`main.c`'s existing per-tick weather loop (already wired by the integration
lane, not by this one) is at ~line 4789–4798:

```c
const uint64_t wx_tick = dayNightTicks(&s_daynight);
...
if (!tickDue(wx_tick, period, (uint32_t)i)) continue;
(void)weatherTickColumn(&s_gen, &s_world, wx_tick, col->cx, col->cz);
```

Add a **render-side** poll near that same block, using the **same
`wx_tick`** so the draw and the simulation can never disagree about what the
weather currently is. `world/weather.h`'s own guidance (its final comment
block) is explicit: query `weatherAt()` at the player's position "a few times
a second — not per particle, not per frame". `world/tick.h` defines
`TICK_HZ 20`, so `tickDue(wx_tick, TICK_HZ / 4, 0)` fires at 4 Hz — a few
times a second, using the same staggering primitive the column loop right
above it already uses (id `0` here rather than a per-column `i`, since this
is one global query, not a decimated set):

```c
if (tickDue(wx_tick, TICK_HZ / 4, 0)) {
    WeatherKind wk = weatherAt(&s_gen, wx_tick,
                               (int32_t)floorf(player.cam.x),
                               (int32_t)floorf(player.cam.z));
    weatherDrawSetState(&s_weatherdraw, wk);
}
```

Then, once per frame (every frame, not throttled — this just advances scroll
and drift phase, it is cheap arithmetic with no GPU or model-query cost),
using the frame delta already available via `metricsFrameMs()`
(`debug/metrics.h:114`, returns milliseconds):

```c
weatherDrawUpdate(&s_weatherdraw, metricsFrameMs() / 1000.0f,
                   player.cam.x, player.cam.y, player.cam.z);
```

Place both of the above somewhere in the simulation portion of the frame,
before the draw phase begins (`watchdogPhase(WD_PHASE_DRAW)` at ~line 5179)
— anywhere after `player.cam`/`s_world` are current for this frame is fine;
it does not need to sit next to the column-tick loop, it only needs the same
`wx_tick` value that loop already computed.

## 4. Per-frame: the draw call — exact placement, and why

**Call `weatherDrawDraw()` inside `drawEye()`, once per invocation, using
that call's own `view` parameter — not once per frame outside it.**

Why: `drawEye(C3D_RenderTarget* target, const C3D_Mtx* view, ...)`
(`main.c:2377`) is called once per eye in stereo (twice, at ~5272–5274, only
the second call gated by `s_stereo`) and `chunkRenderDraw(view)` inside it
(`main.c:2393`) already redraws the whole world per eye with that eye's own
view matrix. Rain/snow strips are real world-space geometry (a
camera-centred grid, translated into view space the same way chunk geometry
is) — for stereo to show them at the correct depth/parallax they need the
same per-eye treatment as the terrain, not a single shared draw. Calling
`weatherDrawDraw()` once outside `drawEye()` would either render it for only
one eye (broken stereo separation) or need its own duplicated per-eye call
site — strictly worse than just putting the one call where `chunkRenderDraw`
already lives.

**Exact ordering: after `chunkRenderDraw(view)` (and after the
`BS_GPU_STRESS` repeat loop right below it), not before.**

`main.c:2393-2394`:
```c
chunkRenderDraw(view);
for (int i = 0; i < BS_GPU_STRESS; i++) chunkRenderDraw(view);
```
Add immediately after:
```c
weatherDrawDraw(chunkRenderProjection(), view, &s_weatherdraw);
```

Why this order, concretely, verified against `weatherDrawDraw`'s own real
code (`source/gfx/weatherdraw.c:321-413`):

- **Depth is not the reason, because weatherDrawDraw does not use it.** It
  draws with `C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR)`
  (line 384) — test disabled, so it never reads the depth buffer, and
  `GPU_WRITE_COLOR` (not `GPU_WRITE_ALL`) means it never writes to it either.
  This is a **named, deliberate trade-off already in the code's own
  comments** (lines 373–383): a 12-block-tall camera-centred strip
  necessarily reaches through walls/hills by construction, and giving it a
  real depth test would mean running it inside the world's own depth-write
  pass rather than as a separately-ownable one — a scope decision this lane
  was told was not its call to make unasked. Concretely this means **rain
  will currently draw in front of geometry it should be behind** (e.g.
  visible through a roof). Flagged here explicitly for you to accept as-is
  or revisit — it is not a silent gap.
- **Color compositing is the real reason for the ordering, and it is not
  optional.** `weatherDrawDraw` blends with
  `C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
  GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA)`
  (lines 369–371) — ordinary alpha-over blending against whatever is
  *already in the color buffer*. If it drew **before**
  `chunkRenderDraw(view)`, the terrain draw immediately after would be an
  opaque write over the same pixels and would completely erase the
  rain/snow that had just blended there (opaque draws do not blend, they
  overwrite). It must run after **every** opaque draw for that eye that
  could cover the same screen pixels, which in practice means after
  `chunkRenderDraw` and its `BS_GPU_STRESS` repeats, and before nothing else
  opaque follows for that eye. It does not need to be the very last thing in
  `drawEye` (a debug overlay drawn after it, translucent or not, would sit
  visually on top of the rain, which is a separate and smaller judgement
  call than the terrain-erasure failure mode above) — placing it directly
  after the terrain draw is the safe, minimal choice.
- **Projection**: pass `chunkRenderProjection()` (`chunk_render.h:238`,
  returns `const C3D_Mtx*`) — "the projection the world is drawn with, so a
  second pass can match it exactly" per that function's own header comment.
  This already reflects whatever `chunkRenderSetEye(iod)` (called at
  `main.c:2382`, before `chunkRenderDraw`) did for stereo skew, so
  weatherDrawDraw does not need any separate per-eye projection handling.
- **State restore**: `weatherDrawDraw` already restores depth test, alpha
  blend, and cull face to `chunk_render.c`'s own established world-pass
  defaults at the end of itself (`weatherdraw.c:403-412`, explicitly modeled
  on `chunk_render.c`'s transparent-pass reset) — whatever draws next this
  frame (a debug overlay, the other eye's pass) sees the same GPU state it
  would have if this call had not run. No cleanup is needed at the call
  site.

## 5. Shader/texture changes — scope confirmation

The **only** shader file this lane created or edited is
`source/shaders/weather.v.pica` (new file). The only edit made to it after
its first draft was an operand-order fix on one line — picasso rejected
`add outtc0.y, inuv.y, params[0].x` with `invalid source2 register: params`
(a real PICA200 vertex-shader ISA restriction: the constant/uniform bank
cannot be addressed from the source2 slot of an arithmetic instruction).
Fixed by swapping to `add outtc0.y, params[0].x, inuv.y`, matching the same
operand order already used in `world.v.pica`'s
`add r7.x, fogParams.yyyy, r7.xxxx`. This lane did **not** touch
`source/shaders/world_dynamic.v.pica` — confirmed by re-reading its own
edit history this session (one file touched: `weather.v.pica`) and by not
having opened `world_dynamic.v.pica` for writing at any point.

## 6. Build wiring — reasoned, not yet verified by a real `make`

Both new build inputs use a texture/shader stem (`weathertex`,
`weather.v.pica`) deliberately different from the `.c` stem
(`weatherdraw.c`), to avoid the exact `.t3s`/`.c` same-stem collision this
Makefile's own comments warn about elsewhere (see `Makefile:892` area, the
`gfx/atlas.t3s` example). `source/gfx`, `source/shaders`, and `gfx/` are all
already covered by the Makefile's wildcarded `SOURCES`/`GRAPHICS`
directories, so **no Makefile edit is believed necessary** — but this is
REASONED, not measured: this lane was explicitly told never to run
`make`/`make clean` against the console target, so no real devkitPro build
of the full project has exercised this path. What *was* verified for real:
`picasso` compiles `weather.v.pica` cleanly, `tex3ds` compiles
`gfx/weathertex.t3s`/`.png` cleanly, and `arm-none-eabi-gcc` compiles
`weatherdraw.c` cleanly against both generated headers, all with the
project's exact real flags (§ below). Whether the full project `make`
picks up both new files automatically, with the right dependency ordering,
is the one build-integration fact only a real full build can confirm.

## 7. Sequencing relative to the other two concurrent lanes

This doc only proposes edits to `main.c` and (implicitly)
`chunk_render.c`-adjacent call sites; it makes no claim about how those
lines interact with the OTHER weather lane's `weatherTickColumn` wiring or
the third (particle-system) lane's own hooks, beyond sharing `wx_tick` as
described in §3. Apply in whatever order avoids stepping on either.
