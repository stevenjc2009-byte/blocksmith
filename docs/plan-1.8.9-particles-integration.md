# Particle system integration — exact hooks for whoever wires this in

This is the handoff `source/gfx/particles.h`/`.c` (v1.8.9) promise in place of editing
`source/main.c`, `source/scene/player.c` or `source/scene/interact.c` directly. Those three
files were explicitly off-limits to the lane that wrote the particle system — two other lanes
are live on `main.c` right now, and a whole-file write to a shared file silently reverts
whatever either of them has in flight. Nothing below has been applied. Every line number was
read directly out of the current files (not out of `docs/plan-particles.md`, whose citations
had already drifted) — re-check them before pasting, since `main.c` in particular is being
edited by other lanes concurrently and these numbers are a snapshot, not a guarantee.

None of this has been compiled together, linked, or run. `source/gfx/particles.c` compiles
clean under a real devkitARM single-translation-unit pass (see the parent task's report), but
that pass does not touch `main.c`/`player.c`/`interact.c` at all — the splices below are
unverified until someone applies them and runs a real `make`.

## 1. Lifecycle: `particlesInit()` / `particlesExit()` — `source/main.c`

Same pattern as `crackOverlayInit()`/`crackOverlayExit()`, which this ties to directly so the
two modules' lifetimes are visibly paired for the next person reading this file.

`source/main.c:3984` currently reads:

```c
	(void)crackOverlayInit();
```

Add immediately after it:

```c
	(void)particlesInit();
```

`source/main.c:5566` currently reads:

```c
	crackOverlayExit();
```

Add immediately after it:

```c
	particlesExit();
```

`particlesInit()`'s return value can be discarded the same way `crackOverlayInit()`'s already
is (line 3984) — a failed init degrades to "no particles drawn," never a crash (see
`particles.h`'s own comment on `particlesInit`).

## 2. Include — `source/main.c`

`source/main.c:47-50` currently reads:

```c
#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/screen.h"
#include "gfx/sprite.h"
```

Insert `#include "gfx/particles.h"` between `gfx/font.h` and `gfx/screen.h` (line 48/49),
keeping the block's existing alphabetical order.

## 3. Per-frame draw — `source/main.c`

`source/main.c:2393-2394` currently reads:

```c
	chunkRenderDraw(view);
	for (int i = 0; i < BS_GPU_STRESS; i++) chunkRenderDraw(view);
```

Add immediately after (still before `highlightDraw(view, hit)` at line 2421):

```c
	particlesDraw(view);
```

This satisfies `particlesDraw`'s own ordering requirement (see `particles.h`): after the world
and after `chunkRenderDraw` specifically, so the depth buffer it tests against already holds
real terrain. Nothing about its position relative to `highlightDraw`/`crackOverlayDraw`
matters — it does not touch the block-highlight or crack-overlay draw state, and they do not
touch its three (documented, restored) pieces of GPU state either.

## 4. Per-frame tick — `source/scene/player.c`, NOT `main.c`

`playerUpdate` (`source/scene/player.c:30-33`) already computes a correctly clamped per-frame
`dt` in seconds before anything else in the function runs:

```c
void playerUpdate(Player* p, const World* w, float dt_ms)
{
	float dt = dt_ms * 0.001f;
	if (dt > MAX_TICK) dt = MAX_TICK;
```

`source/scene/player.c:96` currently reads:

```c
	bodyStep(&p->body, w, dt);
```

Add immediately after it:

```c
	particlesTick(dt);
```

Deliberately NOT a second call site in `main.c` with its own `metricsFrameMs() * 0.001f`
conversion (main.c:4673 has that pattern, but only inside the `#if BS_WALK_STRESS` debug
path) — reusing playerUpdate's own already-clamped `dt` avoids a second, possibly
disagreeing, clamp. The tradeoff, disclosed rather than silent: particles only tick while
`playerUpdate` runs, i.e. while `!paused` and not under `#if BS_FLY` (main.c:4651-4655) — under
the BS_FLY spectator/debug camera, `playerUpdate` is never called at all, so particles freeze
rather than animate. That is almost certainly fine for a debug-only camera mode, but it is
this lane's judgment call, not a fact — worth a second opinion before applying.

## 5. Splash trigger — `source/scene/player.c` — the one plan section 3a calls REQUIRED

`source/scene/player.c:59-63` currently reads:

```c
	// bodyWetUpdate, not bodyWetState: the submerged boundary is hysteretic and the band
	// is carried on the body, so the driven answer is the one the vertical input must see.
	// bodyStep calls it again below and gets the same answer at the same position.
	const BodyWet wet = bodyWetUpdate(w, &p->body);
	const bool submerged = (wet != BODY_DRY);
```

Replace with:

```c
	// bodyWetUpdate, not bodyWetState: the submerged boundary is hysteretic and the band
	// is carried on the body, so the driven answer is the one the vertical input must see.
	// bodyStep calls it again below and gets the same answer at the same position.
	const BodyWet prev_wet = p->body.wet;   // bodyWetUpdate has not overwritten it yet -- see physics.c:412-417
	const BodyWet wet = bodyWetUpdate(w, &p->body);
	const bool submerged = (wet != BODY_DRY);

	if (prev_wet == BODY_DRY && wet != BODY_DRY)
		particlesSpawnSplash(p->body.x, p->body.y, p->body.z, p->body.vy);
```

Why `p->body.wet` read BEFORE the `bodyWetUpdate` call is safely the PREVIOUS frame's value,
not this frame's: `bodyWetUpdate` (`source/world/physics.c:412-417`) reads `b->wet` for its own
hysteresis bias on the line before it overwrites it —

```c
BodyWet bodyWetUpdate(const World* w, Body* b)
{
	const float bias = (b->wet == BODY_SUBMERGED) ? -PLAYER_WET_HYSTERESIS : 0.0f;
	b->wet = wetAt(w, b, bias);
	return b->wet;
}
```

— so capturing `p->body.wet` into `prev_wet` on the line immediately before calling it is the
last point at which that field still holds last frame's answer. `impact_speed` is
`p->body.vy` at that same instant — the fall speed the body arrives at the water with, before
`bodyStep` (line 96) changes it — matching `docs/plan-particles.md` section 3a's "pass Body.vy
at the moment of entry" and `particles.h`'s own comment on `particlesSpawnSplash`.

`BodyWet`'s three states are `BODY_DRY = 0`, `BODY_SURFACE = 1`, `BODY_SUBMERGED = 2`
(`source/world/physics.h:193-195`) — `wet != BODY_DRY` catches entry into either the surface
or a fully submerged cell in one comparison, which is what "hit the water" means here; it does
not fire again on a SURFACE<->SUBMERGED transition (diving under after already floating),
which was this lane's read of "a splash is an entry event, not a state" but is exactly the
kind of design call the top-level brief said needed asking, not assuming — flag before
applying if a diving-under splash is also wanted.

## 6. Include — `source/scene/player.c`

`source/scene/player.c:1-5` currently reads:

```c
#include "scene/player.h"

#include <math.h>

#include "app/input_map.h"
```

Add `#include "gfx/particles.h"` after `#include "app/input_map.h"` (line 5).

## 7. Block-break debris — OPTIONAL, `source/scene/interact.c` — NOT required, NOT written

`docs/plan-particles.md` section 3b calls this a "strong candidate," not a requirement — the
brief's only REQUIRED case was water splash (sections 1-6 above). This lane did not build a
block-break spawn call and is not recommending one be added without being asked; it is left
here only as a pointer for whoever picks this up next, since the hook site was already
located while researching section 5 above.

`source/scene/interact.c:209` is `breakComplete(...)`; the block is actually removed at
`source/scene/interact.c:219` (`worldSet(w, x, y, z, BLOCK_AIR)`). A debris burst would go
right after that line, reusing the general `particlesSpawn` entry point (not
`particlesSpawnSplash`, which is water-specific) — but this needs a colour/count decision
`particles.h`'s general API deliberately leaves open, and per this project's rule on
adjacent-but-unasked work, that decision belongs to whoever picks this up, not to this lane.
