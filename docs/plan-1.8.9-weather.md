# Weather model — v1.8.9, weather half

Scope note: this document covers the **weather half** of v1.8.9 only —
`source/world/weather.h`, `source/world/weather.c`, `tests/weather_test.c`. The
day/night half (the clock, the light curve, sky/fog colour, sun/moon/stars) is a
sibling lane living in `source/world/daynight.h` / `daynight.c` and is not this
document's concern except where the two hand off (see "Determinism" below).

Provenance is marked on every number, the same convention
`docs/research/sky-and-weather.md` uses:

- **[MEASURED]** — a real command was run and this is its real output.
- **[ESTIMATE]** — arithmetic over documented per-call costs, not instrumented.
- **[BY CONSTRUCTION]** — true because of how the code is shaped, not a bound
  that has to be checked at runtime.
- **[CITED]** — a fact read out of another file in this tree, with its path.

---

## 1. What this model is

`weatherAt(g, tick, x, z)` is a pure function: seed, tick, and position in;
`WEATHER_CLEAR` / `WEATHER_RAIN` / `WEATHER_SNOW` out. It owns no state of its
own. `weatherStepCell` and `weatherTickColumn` are the only things that touch
the world, and they touch it through the one `BlockId` byte every surface cell
already has (`BLOCK_AIR` ↔ `BLOCK_SNOW`) — no new field, no new byte, anywhere.

This is deliberately a narrower slice than the full research brief in
`docs/research/sky-and-weather.md` (Part B). That brief also covers rain/snow
*rendering* (billboard strips, E8), storm/thunder timers and strength ramps
(B4/E9), and lightning (B7, then explicitly cut). None of that is built here —
see §6, "What was scoped out."

## 2. Determinism — what is achieved and what is not

`weatherAt` takes `tick` as an explicit argument rather than reading a clock it
owns. `weatherStepCell` and `weatherTickColumn` do the same. This is
deliberate: two callers that pass the same `(seed, tick, x, z)` get the same
answer, with no synchronization of any weather-owned state required, because
there is no weather-owned state to synchronize.

This claim is not just asserted — it is tested directly.
`testWeatherAtDeterministic` in `tests/weather_test.c` builds two independent
`WorldGen` instances from the same raw seed and checks `weatherAt` agrees at
every one of 200 pseudo-random `(x, z, tick)` triples, plus a falsifiability
check that the 200 triples actually produced more than one kind of answer.
**[MEASURED]**: this check passed in every real run (see §5).

What this does **not** solve: **what `tick` value two clients agree on in the
first place.** That is entirely the day/night lane's problem, not weather's.
`source/world/daynight.h`'s own "Multiplayer, and why nothing here has to be
torn up to add it" section (lines 338–359) already designs for this: time of
day is meant to be server-authoritative via a new opcode `BS_APP_TIME_SYNC`
(0x10, the next free slot after `BS_APP_WORLD_GEN = 0x0F` in
`deps/blocksmith-server/proto/bs_proto.h`'s `enum bs_app_msg`), sent once on
join and then on a `tickDue(t, TICK_HZ, 0)` cadence. **[CITED]**

**Recommendation, not built here:** feed weather's `tick` parameter from
whatever `dayNightTicks()`-equivalent accessor the day/night lane exposes once
that sync lands. Weather needs **no packet of its own** — it needs no state at
all, so there is nothing for a weather-specific packet to carry. The moment two
clients agree on a tick (day/night's job), they automatically agree on weather
too, for free, because `weatherAt` is a pure function of that same tick. This
is a deliberately smaller ask than the alternative of giving weather its own
opcode and its own periodic broadcast — there is nothing weather-side worth
broadcasting, since the client can always recompute it locally once it has the
tick.

**Honest gap:** until the day/night sync above is actually wired up, two
clients running independently (e.g. singleplayer only, no server) simply agree
by definition (one `TickClock`), and a hypothetical multiplayer session today
would already be exposed to whatever tick drift `daynight.c`'s own module
already documents as unsolved. This document does not paper over that — it is
inherited, not introduced.

## 3. Biome-awareness — what is achieved and what is not

Weather is decided at two independent granularities, matching the research
brief's two-field model in Part B1 (`precipitation(biome)` × `effectiveTemp` →
snow/rain), adapted to what this scope can actually touch:

- **Where a front is** — one Bernoulli roll per **weather cell** (128×128
  blocks, `WEATHER_CELL_SHIFT = GEN_BIOME_SHIFT = 7`) per **episode** (8192
  ticks, `WEATHER_EPISODE_SHIFT = 13`). `WEATHER_CHANCE_PRECIP = 64/256` (25%)
  ordinarily, `WEATHER_CHANCE_DESERT = 8/256` (~3%) if the roll's own cell
  centre biome is `BIOME_DESERT`.
- **What it looks like** — decided per **exact block**, via
  `worldgenBiomeAt(g, x, z)` at the specific `(x, z)` asked about, not the
  cell's dominant biome. `BIOME_TUNDRA` / `BIOME_TAIGA` → snow; everything
  else that precipitates → rain.

This differs from the brief's E5 recommendation in one respect: E5 proposes an
altitude-cooling term (`effectiveTemp = baseTemp - ((y-64) << 7)` for y > 64)
so that plains/forest columns can turn snowy at height. **That term is not
implemented here.** `weatherAt` only ever asks `worldgenBiomeAt` for a
*horizontal* `(x, z)` — there is no `y` in its signature at all. Adding
altitude-dependent species would mean either passing `y` through the whole
call chain or re-deriving it via `worldgenHeight` inside `weatherAt` itself
(a second noise evaluation on every call, paid whether or not the front is a
snow front). This is exactly the kind of design fork the operating rules call
out as his call, not mine to default on quietly — flagged here rather than
guessed at.

Real-terrain evidence, not just design: `testWeatherAtBiomeSpecies` searches a
4096-block, 32-stride square for a real column of every one of the 6 biomes
(seed 20260901) and checks the species rule holds across 256 sampled episodes
per biome, plus a falsifiability check that both `WEATHER_SNOW` and
`WEATHER_RAIN` were actually observed in the sweep — not vacuously true of a
sweep that only ever saw one kind. `testWeatherAtDesertRarerThanPlains`
independently confirms `WEATHER_CHANCE_DESERT` (not `WEATHER_CHANCE_PRECIP`) is
actually the constant applied at a real desert location, by sampling 512
episodes and checking desert precipitates less than half as often as plains at
the same seed. **[MEASURED]**: both passed in every real run (§5), and both
were the direct target of a sabotage arm that failed exactly the expected
check and nothing else (§5, arms `biome_species_swap` and
`desert_chance_ignored`).

## 4. Snow accumulation and melt

`weatherNextSurfaceBlock(kind, ticks_into_episode, current)` is the only place
a transition happens, and it only ever considers the *current episode's own*
elapsed ticks (`tick & (WEATHER_EPISODE_TICKS - 1)`) — no stored timer, no
per-cell state beyond the one `BlockId` byte already there.

- Snow accumulates: `WEATHER_SNOW == kind && current == BLOCK_AIR &&
  ticks_into_episode >= WEATHER_SNOW_ACCUM_TICKS (2400, 120 real seconds at
  TICK_HZ 20)` → `BLOCK_SNOW`.
- Snow melts: `kind != WEATHER_SNOW && current == BLOCK_SNOW &&
  ticks_into_episode >= WEATHER_MELT_TICKS (1200, 60 real seconds)` →
  `BLOCK_AIR`.
- Anything else: unchanged.

**The one-block cap is [BY CONSTRUCTION], not a checked bound.** There is no
"add a layer" operation anywhere in this file — `weatherNextSurfaceBlock` only
ever writes one of two fixed values to one fixed cell. A second layer is not
possible to represent, because the storage backing it is the same single
`BlockId` byte every other surface cell already has
(`source/world/chunk.h`'s one-byte-per-cell layout — **[CITED]**, confirmed by
Grep/Read earlier in this project and re-affirmed by the `_Static_assert(
CHUNK_DIM == 16, ...)` in `weather.c` which would fail loudly if that layout
assumption ever changed). This is a deliberately different design than the
research brief's E6 recommendation (a new `BLOCK_SNOW_LAYER` block id meshed
with a partial-height "drop" bit, per D7's already-shipping 8-step
`MeshVertex.nrm` mechanism) — see §6 for why that path is out of scope here.

`testWeatherNextSurfaceBlockTable` pins this against a 14-row hand-computed
table (both boundary and off-by-one-tick cases on both directions), plus
explicit boundary checks and a huge-`ticks_into_episode` check that an
already-snow cell asked again after a huge tick count stays exactly one block.
`testWeatherHugeTickArithmetic` separately confirms the mask arithmetic itself
is correct at `tick ≈ 5×10¹²` (roughly 7,900 real years of uptime at
`TICK_HZ 20`), hand-verified by long division that `5×10¹² mod 8192 = 4096`.
**[MEASURED]**: all passed (§5), and the accumulation boundary was the direct
target of the `accum_threshold` sabotage arm, which failed exactly the 5
checks structurally downstream of it and nothing else.

### Where this is safe against an edited world

`weatherStepCell` refuses to act unless: the block directly below the surface
cell is solid (`blockIsSolid`, guards against snow floating over ground a
player mined away since worldgen ran), and the surface cell itself is
currently exactly `BLOCK_AIR` or `BLOCK_SNOW` (guards against overwriting a
player placement, a plant, or anything else). Both guards are real-World
integration-tested in `testWeatherStepCellOnRealWorld`
(`budgetReset()` + `worldInit`, seed 42424242): a "mined ground" case using a
real search-found cold-biome/snow-episode location with the block below
force-cleared to air, and an "occupied cell" case using `BLOCK_STONE` as a
stand-in for a player placement. **[MEASURED]**: both passed, and the
mined-ground guard was the direct target of the `ground_safety_removed`
sabotage arm, which failed exactly that one check and nothing else.

**What could not be verified:** the defensive `air_y < 0 || air_y >=
WORLD_HEIGHT` branch at the top of `weatherStepCell` is not reachable under
any test in this suite, because `worldgenHeight` is documented
(`worldgen.h`) to always return a value inside `GEN_SURFACE_MIN ..
GEN_SURFACE_MIN + GEN_SURFACE_RANGE`, which is itself inside `0 ..
WORLD_HEIGHT`. The branch exists as a defensive boundary against a caller
passing coordinates `worldgenHeight` was never meant to answer for — it is
honestly untested dead code under the real generator's own guarantees, not a
gap in this suite's coverage of anything reachable in practice.

## 5. Real measured evidence

### 5a. Host test suite

`tests/weather_test.c`, compiled and run under WSL with
`gcc -std=c11 -Wall -Wextra -Werror -O1 -g` against the real (unsabotaged)
`source/world/weather.c` and the project's other host-buildable world sources:

```
PASS 46 checks, 0 failed
```

**[MEASURED]**, 0 compiler warnings under `-Werror`.

### 5b. Sabotage / red-arm proof — all four planned arms run

Each arm copies `weather.c` to a scratchpad file, patches exactly one thing
wrong, compiles the real test suite against the sabotaged copy, runs it, and
deletes the scratchpad copy and its build directory unconditionally on exit
(never touches the live repo file). Verified after every run: no
`weather_sabotage_*.c` left in scratchpad, no `weather-sabotage-*` directory
left under `build-host/`.

| Arm | What was broken | Real result | Localized as expected? |
|---|---|---|---|
| `accum_threshold` | `>=` → `>` on the snow-accumulation boundary | **[MEASURED]** `FAIL 46 checks, 5 failed` | Yes — the 14-row table check, the exact-boundary check, the integration "places snow at threshold" check, and both `weatherTickColumn` coverage checks (which depend on snow actually appearing) failed; all 41 other checks, including the control and the melt-boundary checks, stayed green. |
| `biome_species_swap` | swapped which biome group returns `WEATHER_SNOW` vs `WEATHER_RAIN` | **[MEASURED]** `FAIL 36 checks, 4 failed` | Yes — the species-rule check failed directly; the two real-terrain searches that specifically require a cold-biome location classifying `WEATHER_SNOW` (sections 9 and 10) correctly failed to find one (since swapped tundra/taiga now emit rain) and their functions returned early, which the test harness itself reports as "10 check(s) WENT MISSING" via `checkCountPin()` rather than silently passing; accumulation, melt, desert-rarity and determinism all stayed green. |
| `desert_chance_ignored` | desert lost its own lower precipitation chance, falling back to `WEATHER_CHANCE_PRECIP` | **[MEASURED]** `FAIL 46 checks, 1 failed` | Yes — exactly the desert-rarer-than-plains ratio check failed; even that section's own falsifiability check (neither location is always-clear/always-precipitating) stayed green, and every other section was unaffected. |
| `ground_safety_removed` | deleted the "ground below must be solid" guard | **[MEASURED]** `FAIL 46 checks, 1 failed` | Yes — exactly the mined-ground-safety check failed; the sibling occupied-cell-safety check (a different guard) stayed green, as did everything else. |

Every check category this suite claims to guard has now been demonstrated
able to go red, and every red result was traced to the specific line
sabotaged rather than a broad collapse.

### 5c. Real ARM compile

Compiled `source/world/weather.c` alone as a single translation unit, for the
real target, via `devkitARM`'s own `arm-none-eabi-gcc.exe`, with the exact
flags specified for this task and no others invented:

```
arm-none-eabi-gcc.exe -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
    -O3 -mword-relocations -ffunction-sections -Werror -D__3DS__ -std=c11 \
    -I source -c source/world/weather.c -o weather_arm.o
```

Result: **[MEASURED]** compiled clean, 0 warnings/errors under `-Werror`.

```
   text    data     bss     dec     hex filename
   1064       0       0    1064     428 weather_arm.o
```

`.data = 0`, `.bss = 0` — **[MEASURED]**, directly confirms the "zero
persistent bytes, stateless by construction" claim at the object-code level,
not just by source inspection. `1064` bytes of `.text` is this file's own
code only (unlinked — it excludes the callee code in `worldGet`/`worldSet`/
`rngMix`/`rngHash3`/`worldgenHeight`/`worldgenBiomeAt`, which live in other
translation units and are not weather's own footprint). `arm-none-eabi-nm`
confirmed all four public symbols (`weatherAt`, `weatherNextSurfaceBlock`,
`weatherStepCell`, `weatherTickColumn`) are present and non-stripped, ruling
out an accidentally-empty compilation.

**Not applicable here:** the ARM EABI `-fshort-enums` struct-size hazard this
project has been bitten by before (`sizeof()` lying about a struct's packed
size when compiled for host vs ARM) does not apply to this file — `weather.c`
defines no struct that stores a `WeatherKind` or `BiomeId` and gets compared
byte-for-byte across host and ARM; `WeatherKind` is used only as a plain
parameter/return type. There is nothing here for that hazard to bite.

### 5d. Per-tick cost — estimate, explicitly not a measurement

`weather.c`'s own header comment works through the arithmetic: at the Old
3DS's documented render radius of 5 (121 loaded columns), a worst-case single
tick where every decimated-tick column happens to land on the same tick is
121 columns × `WEATHER_CELLS_PER_VISIT` (4) = **484 calls to
`weatherStepCell` [ESTIMATE]** in that tick. Each call costs one
`worldgenHeight` (the same noise-sample cost class worldgen already pays once
per column at generation time), one `weatherAt` (one `worldgenBiomeAt` plus
one `rngMix` plus one `rngHash3`, all O(1) integer arithmetic), one or two
`worldGet` calls, and at most one `worldSet`. `worldSet` only actually writes
on the ticks where a cell crosses the accumulation or melt threshold — at
most twice per `WEATHER_EPISODE_TICKS` (8192 ticks, ~409 seconds) per cell —
so the overwhelming majority of calls read four values and write nothing.
This figure is an upper bound on **work**, not on memory (memory is 0, §5c),
and it was arrived at by counting operations, not by instrumenting a real
tick loop — because wiring a call into the real tick loop is a `main.c` edit
outside this task's scope (see §7).

## 6. What was scoped out, and why

- **Rendering** (rain/snow billboard strips, per the research brief's B6/E8) —
  not built. Weather here is a world-state model only; nothing in
  `weather.c`/`weather.h` issues a draw call or touches `chunk_render.c`.
  The brief's own facts about *why* this would be cheap on this hardware
  remain valid and are worth re-reading when that phase starts: no particle
  system exists or is scheduled before v1.8.10 (D8), so vanilla's own
  non-particle billboard-strip approach (B6) is already the cheap path, not
  an approximation forced by this hardware.
- **True multi-layer snow** (vanilla's 1–8 layer `layers` blockstate, or the
  brief's E6 `BLOCK_SNOW_LAYER` + partial-height mesh drop) — not built.
  `source/world/chunk.h` gives exactly one `BlockId` byte per cell with no
  metadata channel, so a depth state cannot be represented without either a
  new per-cell metadata channel or several new discrete block ids — both are
  registry changes (`source/world/registry.c`, `kCoreDefs`), and any new
  block id is a cross-repo registry CRC lockstep with
  `deps/blocksmith-server`. Both are explicitly out of this task's scope and
  were not attempted. This project's own requirement (a hard one-block cap)
  happens to make the simpler model — the one actually built — correct, not
  just convenient: see §4's "by construction" argument.
- **Altitude-dependent snow line** (brief's E5) — not built; see §3.
- **Storm/thunder timers, strength ramps, lightning** (brief's B4/B7/E9) —
  not built. This model has no notion of a storm "starting" or "ending" with
  a ramp; each weather cell's kind is decided fresh per episode by a single
  roll, with no lerp between states. A caller wanting vanilla's gradual
  strength ramp would need to add it on top of (not inside) this model.
- **A weather network packet** — deliberately not designed, let alone built.
  See §2: because this model is stateless, there is nothing weather-specific
  to broadcast. It only needs an agreed `tick`, which is entirely the
  day/night lane's problem, and that lane already documents its own wire
  design (`daynight.h` lines 338–359, `BS_APP_TIME_SYNC = 0x10`).

## 7. Lines someone else must add

I do not own `source/main.c` and did not edit it. For this model to actually
run in the game, the following needs to be added by whoever owns that file:

- A call to `weatherTickColumn(g, w, tick, cx, cz)` inside `main.c`'s per-tick
  world-update loop, once per loaded column, decimated using the existing
  `source/world/tick.h` scheme (`tickPeriodForDistSq` / `tickDue`) the same
  way the project's other per-tick systems (e.g. the water tick) already are
  — so that weather does not visit every column every tick at close range.
- The `tick` value passed in should come from whatever accessor the
  day/night lane (`daynight.c`) exposes once it lands (its own
  `dayNightTicks()`-equivalent), not a separately invented counter — see §2.
  Until that lane lands, any monotonic tick counter already available in
  `main.c`'s loop is an acceptable placeholder, but should be swapped for the
  real one when it exists rather than kept as a permanent second clock.

Neither of these lines exists yet anywhere in the tree; both are additions,
not modifications of existing lines.
