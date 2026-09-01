# Sky and weather — research brief for Blocksmith v1.8.9

Scope note: this is research only. **Nothing in the Blocksmith project tree was changed.**
`source/` was read, never written.

Provenance is marked throughout. Unmarked claims carry an inline source link. Otherwise:

- **[COMPUTED]** — I ran the cited formula myself and this is the output. The scripts are in
  the session scratchpad (`sky_curve.py`, `sky_blend_ablation.py`, `sky_tables.py`,
  `sky_altitude.py`, `sky_snowrate.py`); they are throwaway, not part of the repo.
- **[DERIVED]** — arithmetic on top of cited numbers, shown so you can check it.
- **[INFERENCE]** — my engineering judgement. Not sourced.
- **[NO SOURCE]** — I looked and could not find one. Named explicitly so it is never
  implemented as if it were a fact.

The single most important framing point, up front: **this version is far cheaper than it
looks, because the day/night hook was already built.** `source/shaders/world_dynamic.v.pica`
has declared a `dayLevel` uniform since v1.5.0, the mesher already packs sky and block light
separately into every vertex, and `scene/chunk_render.c:862` pins the uniform to 1.0 with a
comment that says it is waiting for exactly this version. See Part D.

---

# Part A — The day/night cycle

## A1. Day length and the phase boundaries

| Quantity | Value | Source |
|---|---|---|
| Full day | **24,000 ticks** | [Daylight cycle](https://minecraft.wiki/w/Daylight_cycle) |
| Real time | **20 minutes** | same |
| Tick rate | **20 tps** | same |
| Day (bright) | tick **0 – 12000** (10 min) | same |
| Sunset / dusk | tick **12000 – 13000** (50 s) | same |
| Night | tick **13000 – 23000** (8 min 20 s) | same |
| Sunrise / dawn | tick **23000 – 24000** (50 s) | same |

Tick 0 is **06:00** in the in-game clock, so noon is 6000 and midnight is 18000.

Blocksmith's tick rate is already exactly 20 Hz — `TICK_HZ 20`, `TICK_PERIOD_US 50000` in
`source/world/tick.h`. **A vanilla-length day needs no rescaling at all**: 24000 of the
project's existing ticks is 20 minutes on the nose.

Gameplay boundaries that fall out of the light curve (all from
[Daylight cycle](https://minecraft.wiki/w/Daylight_cycle)):

- Hostile mobs can first spawn outdoors at tick **13188** (clear) / **12969** (rain); last
  spawn tick **22812** (Java).
- Beds usable **12542 – 23459** clear, from **12010** in rain.
- Undead stop burning at **23460**, resume at **12542**.

These are not independent constants. They are the ticks at which the light curve in A3
crosses particular integers — see the cross-check at the end of A3, which is the strongest
piece of evidence in this brief.

## A2. The celestial angle — and why it is not linear

Minecraft does **not** move the sun linearly with time. The decompiled function, from a
Beta 1.7.3-derived writeup and from a 1.8.9 decompile independently:

```
f  = (timeOfDay / 24000) - 0.25      ; wrap into [0,1)
f1 = f                                ; keep the linear value
f  = 1 - (cos(f * PI) + 1) / 2        ; the cosine easing
f  = f1 + (f - f1) / 3                ; blend ONE THIRD of the way toward it
```

Sources: [TrueCraft wiki "Sky"](https://github.com/ddevault/TrueCraft/wiki/Sky) (explicitly
based on a decompiled **Beta 1.7.3** client) and
[MCP-919 `WorldProvider.calculateCelestialAngle`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java)
(1.8.9).

**A warning about that second source.** MCP-919's line 131 literally reads
`f = f + (f - f) / 3.0F;` — algebraically a no-op. That is a decompiler artefact: MCP's
namer collapsed the `f1` local onto `f`, destroying the blend. Anyone reading that file and
implementing what it says will ship the pure-cosine curve. The modern Mojang-mapped form of
the same function is written differently and is not ambiguous:

```
d0 = frac(time / 24000 - 0.25)
d1 = 0.5 - cos(d0 * PI) / 2
return (d0 * 2 + d1) / 3
```

`(2*d0 + d1)/3` is algebraically identical to `d0 + (d1 - d0)/3`. **[DERIVED]** I confirmed
that numerically: the maximum difference between the two forms across all 24000 ticks is
**1.1e-16** — float noise. **[COMPUTED]**

**Proof the blend is real, not a decompiler ghost.** I ran the sky-light curve of A3 through
four candidate angle functions and compared the resulting integer transitions against the
tick boundaries the wiki documents independently. **[COMPUTED]**

| Angle function | first fall (wiki 12041) | floor reached (wiki 13670) | rise begins (wiki 22331) | back to full (wiki 23960) |
|---|---|---|---|---|
| blended `f1 + (f-f1)/3` | **12041** | **13670** | **22331** | **23960** |
| Mojang `(2*d0+d1)/3` | **12041** | **13670** | **22331** | **23960** |
| pure cosine, no blend | 13408 | 14693 | 21308 | 22593 |
| pure linear | 611 | 12966 | 23035 | never |

The check goes red for both wrong variants and green for both right ones. Implement the
blend.

**What the blend does to the feel.** Pure cosine easing has zero derivative at the horizons
and maximum derivative at zenith — the sun would crawl at dawn and dusk and race overhead.
Blending only one third of that in leaves the motion **mostly linear with a mild lingering
near the horizon and a mild speed-up overhead**. The dusk and dawn *look* longer than the
50 ticks of nominal "sunset" because the sun is moving slowly there, which is a large part
of why Minecraft's sunsets read as unhurried.

## A3. The sky light curve — the part clones get wrong

This is the heart of the version. Verbatim from
[MCP-919 `World.calculateSkylightSubtracted`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/World.java):

```java
float f  = this.getCelestialAngle(partialTicks);
float f1 = 1.0F - (MathHelper.cos(f * (float)Math.PI * 2.0F) * 2.0F + 0.5F);
f1 = MathHelper.clamp_float(f1, 0.0F, 1.0F);
f1 = 1.0F - f1;
f1 = (float)((double)f1 * (1.0D - (double)(this.getRainStrength(partialTicks)    * 5.0F) / 16.0D));
f1 = (float)((double)f1 * (1.0D - (double)(this.getThunderStrength(partialTicks) * 5.0F) / 16.0D));
f1 = 1.0F - f1;
return (int)(f1 * 11.0F);
```

Read it carefully — three things matter:

1. The result is a **subtracted** value in **0..11**, not a light level. Effective sky light
   is `15 - subtracted`, so full night is **4**, never 0. That floor is the whole reason
   Minecraft nights are navigable.
2. The shape is `cos(angle * 2*PI) * 2 + 0.5`, **clamped to 0..1**. The `* 2` is what makes
   the fall steep; the clamp is what makes the curve flat across midday and flat across the
   whole of night, with the entire transition packed into a short window. **It is not a
   linear fade and it is not a smoothstep.**
3. The weather terms are `1 - (strength * 5) / 16`, i.e. only `strength*5` is divided by 16,
   and rain and thunder **compound multiplicatively**. The operator precedence here is the
   thing a paraphrase gets wrong; see A3's cross-check for why I trust this reading.

### The integer transitions, tick by tick **[COMPUTED]**

| Sky light | falls at tick | rises at tick |
|---|---|---|
| 15 → 14 | 12041 | (from 14) 23960 |
| 14 → 13 | 12210 | 23791 |
| 13 → 12 | 12377 | 23624 |
| 12 → 11 | 12541 | 23460 |
| 11 → 10 | 12704 | 23297 |
| 10 → 9 | 12866 | 23135 |
| 9 → 8 | 13027 | 22974 |
| 8 → 7 | 13188 | 22813 |
| 7 → 6 | 13348 | 22653 |
| 6 → 5 | 13509 | 22492 |
| 5 → 4 | 13670 | 22331 |

**The fall from 15 to 4 takes 1629 ticks — 81.5 real seconds.** The rise is symmetric. That
is the number to hold onto: **the light goes from full day to full night in about a minute
and a quarter, out of a twenty-minute day.** Everything either side of that window is flat.

### The cross-check that makes this brief trustworthy

Every one of the gameplay boundaries in A1 falls out of this table without being fitted to
it: **[COMPUTED]**

- First hostile spawn at 13188 = the tick sky light reaches **7**, and hostiles need sky
  light ≤ 7 ([Light](https://minecraft.wiki/w/Light)). Exact match.
- Last spawn at 22812/22813 = the tick sky light returns to **8**. Exact match.
- Beds usable 12542–23459 = exactly the window in which sky light is **≤ 11**. Both edges
  match to the tick.
- Undead stop burning at 23460 = the tick sky light returns to **12**. Exact match.
- Wiki's own quoted "starts decreasing at 12040 / minimum at 13670 / increasing at 22331 /
  maximum at 23961" — all four reproduced.

Eight independently-documented tick numbers, from a formula fitted to none of them. That is
why I trust this formula over any prose description of the curve, including the wiki's own.

### Sky light under weather **[COMPUTED]**, cross-checked against the wiki

| Condition | subtracted at noon | sky light at noon | at midnight |
|---|---|---|---|
| Clear | 0 | **15** | 4 |
| Rain (strength 1.0) | 3 | **12** | 4 |
| Thunderstorm (rain 1.0 + thunder 1.0) | 5 | **10** | 4 |

The wiki states rain independently: *"the light from the sun to decrease by 3, bringing it to
light level 12 in full daylight. Moonlight, however, is not reduced, and remains at light
level 4."* ([Rain](https://minecraft.wiki/w/Rain)). Thunderstorm noon = 10 is stated on
[Thunderstorm](https://minecraft.wiki/w/Thunderstorm). Both reproduced exactly, which
independently confirms the `* 5 / 16` precedence reading above.

**Correction to a figure that circulates widely.** "A thunderstorm drops sky light to 5" is
**wrong**. The light value is **10**. The number 5 comes from a *separate* rule: for hostile
mob-spawning checks only, a thunderstorm treats sky light as reduced by a further 10, which
is what allows daytime spawns. Two different mechanisms, one of which is not a light level
at all. If v1.8.9 ever grows mob spawning, keep them separate.

### Rain and thunder ramp

Rain strength is lerped **±0.01 per tick** toward its target, so a storm fades fully in or
out over **100 ticks / 5 seconds** —
[Technical Minecraft Wiki: Weather](https://techmcdocs.github.io/pages/GameTick/Weather/).
The thunder-strength lerp rate is **[NO SOURCE]**; it shares the same code path so ±0.01 is
architecturally likely, but nothing states it.

## A4. Light level → screen brightness — the second thing clones get wrong

A light level is not a brightness. Minecraft converts it through a table, verbatim from
[MCP-919 `WorldProvider.generateLightBrightnessTable`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java):

```java
float f = 0.0F;                              // 0 for the Overworld
for (int i = 0; i <= 15; ++i) {
    float f1 = 1.0F - (float)i / 15.0F;
    this.lightBrightnessTable[i] = (1.0F - f1) / (f1 * 3.0F + 1.0F) * (1.0F - f) + f;
}
```

With `f = 0` this simplifies exactly to, for `l = level/15`:

```
brightness(l) = l / (4 - 3*l)
```

**[DERIVED]** — `(1-f1)/(f1*3+1)` with `f1 = 1-l` gives `l / (3-3l+1)` = `l / (4-3l)`.

| Level | linear `l` | vanilla `l/(4-3l)` | vanilla ÷ linear |
|---|---|---|---|
| 0 | 0.000 | 0.000 | — |
| 4 | 0.267 | **0.083** | 0.31 |
| 7 | 0.467 | 0.180 | 0.39 |
| 8 | 0.533 | 0.222 | 0.42 |
| 11 | 0.733 | 0.407 | 0.56 |
| 15 | 1.000 | 1.000 | 1.00 |

**[COMPUTED]**

This matters enormously. Blocksmith's shader currently normalises light linearly
(`inv15`, i.e. `level/15`). At full night, sky light 4, vanilla renders the world at **0.083**
of full brightness; a linear ramp renders it at **0.267** — **3.2x too bright**. A linear
night does not look like a Minecraft night; it looks like an overcast afternoon. This is the
single most likely way for v1.8.9 to ship a day/night cycle that is technically correct and
still feels wrong.

The curve costs **three vertex-shader instructions** on the PICA200 — see Part E.

## A5. The sun's and moon's path — and a Beta 1.7.3 correction

**In Beta 1.7.3 the sun rose in the NORTH and set in the SOUTH.** It was not changed to
east/west until **Beta 1.9 Prerelease 4**. From [Sun](https://minecraft.wiki/w/Sun):
*"Historically, this was reversed—the sun originally rose in the north and set in the south
until Beta 1.9 Prerelease 4, when developers corrected this bug."*

This is a genuine design fork for Blocksmith, not a detail:

- Blocksmith's stated reference is **Beta 1.7.3 / the legacy console editions**. Beta 1.7.3
  is north/south. The legacy console editions all descend from Beta 1.8+ and are
  **east/west**.
- **[INFERENCE]** I would ship **east/west**. It is what every player alive expects, it is
  what the console editions the project actually names as its second reference did, and
  Mojang themselves called the other behaviour a bug. But it is a call the project owner
  should make explicitly rather than have made for him, because "accurate to Beta 1.7.3" and
  "east/west" are not the same answer here.

Path shape: a plane rotated `360° × celestialAngle` about a single axis, passing directly
through the zenith at noon. There is only one rotation axis in the renderer, so **no tilt and
no seasonal variation is possible** — the sun goes straight overhead, always.
([Sun](https://minecraft.wiki/w/Sun), [TrueCraft "Sky"](https://github.com/ddevault/TrueCraft/wiki/Sky))

Quad geometry, from the Beta 1.7.3 decompile
([TrueCraft "Sky"](https://github.com/ddevault/TrueCraft/wiki/Sky)): the **sun** is a
60×60 plane (extents −30..30) drawn at (0, 100, 0); the **moon** is drawn at (0, −100, 0)
with extents given as −20..20. That source describes the moon as "60×60 with [-20,20]
extents", which is internally inconsistent (−20..20 spans 40). **[NO SOURCE]** for which half
is the typo. Treat the *ratio* as reliable — the moon is visibly smaller than the sun — and
pick the absolute size by eye on a 400×240 top screen, where 100 units out is far past the
fog anyway.

## A6. Sky colour and fog colour — a different curve from light level

Sky colour and light level are driven by the same celestial angle but by **different
formulas**, and neither is the other. Three separate things:

**(a) The base sky hue, from biome temperature.** Verbatim from a decompiled
`BiomeGenBase.getSkyColorByTemp`
([Spoutcraft mirror](https://github.com/Spoutcraft/Spoutcraft/blob/master/src/main/java/net/minecraft/src/BiomeGenBase.java)):

```java
par1 /= 3.0F;
if (par1 < -1.0F) par1 = -1.0F;
if (par1 >  1.0F) par1 =  1.0F;
return Color.getHSBColor(0.62222224F - par1 * 0.05F, 0.5F + par1 * 0.1F, 1.0F).getRGB();
```

Hue `0.6222 − (T/3)*0.05`, saturation `0.5 + (T/3)*0.1`, value fixed at 1.0. Hot biomes get
a slightly greener-blue, more saturated sky; cold biomes a slightly purpler, paler one. The
variation is small by design.

**(b) The day→night dimming term.** From
[MCP-919 `WorldProvider.getFogColor`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java):

```java
float f = MathHelper.cos(celestialAngle * (float)Math.PI * 2.0F) * 2.0F + 0.5F;
f = MathHelper.clamp_float(f, 0.0F, 1.0F);
return new Vec3(0.7529412F * (f * 0.94F + 0.06F),
                0.84705883F * (f * 0.94F + 0.06F),
                1.0F        * (f * 0.91F + 0.09F));
```

Note `f` here is **the same clamped cosine expression as the sky-light curve**, before the
`1 - x` inversion. One evaluation serves both. Note also the per-channel floors differ —
`0.06` on red and green, `0.09` on blue — so **the night sky keeps a blue cast rather than
going to grey**. That asymmetry is small and is exactly the kind of thing a hand-rolled fade
loses.

Daytime fog colour is `(192, 216, 255)`; the midnight floor is `(11.5, 13.0, 22.9)`.
**[COMPUTED]** from the formula above.

**(c) The sunrise/sunset band.** Verbatim from
[MCP-919 `WorldProvider.calcSunriseSunsetColors`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java)
— my brief originally guessed at a ±0.4 radian window; the real gate is on the **cosine**,
not the angle:

```java
float f  = 0.4F;
float f1 = MathHelper.cos(celestialAngle * (float)Math.PI * 2.0F);
if (f1 >= -f && f1 <= f) {
    float f3 = f1 / f * 0.5F + 0.5F;
    float f4 = 1.0F - (1.0F - MathHelper.sin(f3 * (float)Math.PI)) * 0.99F;
    f4 = f4 * f4;
    colorsSunriseSunset[0] = f3 * 0.3F + 0.7F;   // red
    colorsSunriseSunset[1] = f3 * f3 * 0.7F + 0.2F; // green
    colorsSunriseSunset[2] = 0.2F;                // blue, constant
    colorsSunriseSunset[3] = f4;                  // alpha
    return colorsSunriseSunset;
}
return null;   // no band at all outside the window
```

**[COMPUTED]** the resulting windows and peaks:

| Band | active ticks | peak alpha | peak RGB |
|---|---|---|---|
| Dusk | 11271 – 14212 | 1.000 at t=12785 | (217, 96, 51) |
| Dawn | 21788 – 729 (wraps) | 1.000 at t=23215 | (217, 96, 51) |

So the warm band is up for roughly **2940 ticks — about 2.5 real minutes — each side**,
which is far longer than the 50-tick nominal "sunset" phase and much longer than the 1629-tick
light fall. **The orange is on screen well before the world starts getting dark and well
after it stops.** That is the sunset feel, and it is why the sunset reads as slow while the
light drop reads as fast.

Blue is a **constant 0.2** through the whole band — the colour never goes purple or pink, it
runs white-orange → deep orange → out. In the renderer the band is blended into the fog by
`dot(cameraLook, sunDirection)` and only applied where that dot is positive, i.e. **only when
you are looking toward the sun** ([MCP-919 `EntityRenderer`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/client/renderer/EntityRenderer.java)).

## A7. Moon phases and stars

**Beta 1.7.3 had no moon phases.** Phases arrived in **Beta 1.9 Prerelease 4**, when the moon
also stopped being square ([Moon](https://minecraft.wiki/w/Moon)). If the target is strictly
Beta 1.7.3, the moon is one unchanging square.

If phases are wanted anyway: **8 phases**, one per day, so a full cycle is 8 days =
**192,000 ticks = 2 h 40 min** real time ([Moon](https://minecraft.wiki/w/Moon)). The index
is trivial —
`moonPhase = (int)(worldTime / 24000 % 8 + 8) % 8`
([MCP-919 `WorldProvider.getMoonPhase`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/WorldProvider.java)).

**Moon phase does not affect light.** *"Moon phase does not affect the base illumination. The
nighttime light level remains constant at a minimum of four regardless of which phase is
occurring"* ([Moon](https://minecraft.wiki/w/Moon)). It affects slime spawning, regional
difficulty and black-cat spawns — none of which exist in Blocksmith. **A phase cycle is
therefore pure art with zero mechanical consequence**, which makes it a good thing to cut if
the atlas budget is tight.

**Stars.** Verbatim from
[MCP-919 `World.getStarBrightness`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/world/World.java):

```java
float f  = this.getCelestialAngle(partialTicks);
float f1 = 1.0F - (MathHelper.cos(f * (float)Math.PI * 2.0F) * 2.0F + 0.25F);
f1 = MathHelper.clamp_float(f1, 0.0F, 1.0F);
return f1 * f1 * 0.5F;
```

Same cosine expression again, different constant (`0.25` rather than `0.5`), squared, and
capped at 0.5 — **stars never reach full brightness**. Visible from tick **11576**, maximum
at **13219**, fading from **22782**, minimum by **425**
([Star](https://minecraft.wiki/w/Star)). Note stars begin to appear at 11576, i.e. **465
ticks before the light starts dropping at 12041** — the sky tells you night is coming before
the ground does.

Rendering: **780 stars** drawn from 1500 attempts with fixed seed **10842** (so every world
has the same sky), plain white squares with **no texture**, side length randomised 0.3–0.5
([Star](https://minecraft.wiki/w/Star)). Untextured white quads is about the cheapest thing
that can be drawn on a PICA200 and this is one place vanilla's own approach is already the
cheap one.

---

# Part B — Weather that knows where it is

## B1. Rain vs snow: the threshold, and the mechanism that is *not* a threshold

**The rain/snow line is temperature `0.15`.** Below it snows, at or above it rains.
[Rain](https://minecraft.wiki/w/Rain), and stated again on
[Biome](https://minecraft.wiki/w/Biome): *"a location is rainable when its temperature value
is equal or greater than 0.15, and snowable otherwise."*

**"No precipitation at all" is NOT a second temperature cutoff.** It is a separate per-biome
flag. From [Biome](https://minecraft.wiki/w/Biome): *"if it is 0.0, no rain or snow occurs."*
In the decompiled source the desert calls `setDisableRain()` alongside its temperature — the
flag suppresses weather, not the 2.0 temperature
([mc-dev `BiomeBase.java`](https://github.com/Bukkit/mc-dev/blob/master/net/minecraft/server/BiomeBase.java)).

The proof the two are decoupled: **Mountains has temperature 1.0 and still gets weather** —
it snows at altitude. If temperature ≥ 1.0 were the "no rain" rule, it could not.

So the real model is **two independent fields**:

```
precipitation(biome) = NONE, or RAIN-OR-SNOW
if RAIN-OR-SNOW:  effectiveTemp < 0.15  ->  snow
                  otherwise             ->  rain
```

This is the right shape for Blocksmith, because it means **desert and savanna are dry by
declaration, not by being hot**, and you never have to pick a hot-side threshold that also
has to not break mountains.

## B2. The vanilla biome temperature table

All from [Biome](https://minecraft.wiki/w/Biome).

| Biome | Temperature | Side of 0.15 |
|---|---|---|
| Snowy Taiga | −0.5 | snow |
| Snowy Tundra / Ice Plains | 0.0 | snow |
| Frozen Ocean | 0.0 | snow |
| Taiga | 0.25 | rain (snows high up) |
| Ocean | 0.25 | rain |
| River | 0.5 | rain |
| Forest | 0.7 | rain |
| Plains | 0.8 | rain |
| Beach | 0.8 | rain |
| Swamp | 0.9 | rain |
| Mushroom Fields | 0.9 | rain |
| Jungle | 0.95 | rain |
| Mountains / Stony Peaks | 1.0 | rain (snows high up) |
| Desert | 2.0 | **none** (downfall 0) |
| Savanna | 2.0 | **none** (downfall 0) |
| Badlands / Mesa | 2.0 | **none** (downfall 0) |

Note how tightly clustered the wet biomes are — 0.25 to 1.0 covers everything that is not
frozen or desert. **The snow line at 0.15 sits *below* every non-snowy biome, not among
them.** Snow is not "the cold end of a continuum"; it is a separate club, and the only way a
normal biome joins it is altitude.

## B3. Altitude cooling — the formula, and why it does nothing in a 128-tall world

There are **two different eras** of this rule and they disagree. Both are cited; you need to
know which you are copying.

**Rule A — pre-1.18 (introduced Java 1.7.2 / snapshot 13w36a, ran to ~1.17).** Verbatim from
a decompiled `BiomeBase.a(int, int, int)`
([mc-dev](https://github.com/Bukkit/mc-dev/blob/master/net/minecraft/server/BiomeBase.java)):

```java
if (j > 64) {
    float f = (float) noise.a((double) i * 1.0D / 8.0D, (double) k * 1.0D / 8.0D) * 4.0F;
    return this.temperature - (f + (float) j - 64.0F) * 0.05F / 30.0F;
}
return this.temperature;
```

Base **y = 64**, coefficient **0.05/30 = 0.00166667 per block**, plus a Perlin jitter term
sampled at `(x/8, z/8)` scaled by 4.0 — which is worth **±4 blocks of equivalent height**,
i.e. the snow line is *not* a flat contour, it wobbles. That wobble is doing real work: it is
what stops the snow line looking like a laser-cut plane across a mountain.

**Rule B — modern (1.18+).** From [Biome](https://minecraft.wiki/w/Biome): *"Locations with
Y≤80 use the base temperature as actual temperature. At Y=81, the actual temperature value
randomly fluctuates up and down by −0.00875 — +0.01125 from the base temperature based on a
noise on the XZ plane, and at Y≥81 the actual temperature decreases by 0.00125 (1⁄800) every
block up."* The same page states the snow line is *"randomized per block ... with a margin of
up to 8 blocks."*

**Which do I trust?** Both, for their own eras — they are not in conflict, they are different
versions. Rule B is self-consistent with the wiki's own per-biome snow-line figures: Taiga
at base 0.25 gives `81 + (0.25 − 0.15)/0.00125 = 161`, against the wiki's stated "160±8".
**[DERIVED]** That reproduction is why I am confident Rule B is transcribed correctly.

**Neither existed in Beta 1.7.3.** Altitude snow was added in 1.7.2 (2013), two years after
Beta 1.7.3 ([Weather](https://minecraft.wiki/w/Weather)). Beta 1.7.3 predates the Beta 1.8
biome overhaul entirely; its biome placement came from noise temperature/rainfall on a biome
graph, and snow was a property of being a Tundra/Taiga biome rather than a continuous
threshold test ([Biome/Before Beta 1.8](https://minecraft.wiki/w/Biome/Before_Beta_1.8)).
**There is [NO SOURCE] giving a numeric Beta 1.7.3 rain/snow threshold.** The 0.15 figure is
a later-Java number and should be treated as "the rule the project wants", not "the rule
Beta 1.7.3 had".

### The problem: both rules are inert at 128 blocks tall

Blocksmith's world is **128 tall with sea level at 64** (`WORLD_HEIGHT` in
`source/world/world.h`, `GEN_SEA_LEVEL` in `source/world/worldgen.h`). Vanilla's coefficients
were calibrated for a 256-tall world (Rule A) and a 384-tall one (Rule B). Applied unchanged:

**[COMPUTED]** — snow-line altitude for each vanilla biome, and whether it is inside 0..128:

| Biome | T | Rule A snow line | Rule B snow line |
|---|---|---|---|
| Taiga | 0.25 | y = **124** (just reachable) | y = 161 (unreachable) |
| River | 0.5 | y = 274 | y = 361 |
| Forest | 0.7 | y = 394 | y = 521 |
| Plains | 0.8 | y = 454 | y = 601 |
| Jungle | 0.95 | y = 544 | y = 721 |
| Mountains | 1.0 | y = 574 | y = 761 |

Total cooling available across the whole world:

- Rule A, y 64→128: **0.1067**. Only base temperatures below **0.2567** can ever cross.
- Rule B, y 81→128: **0.0588**. Only base temperatures below **0.2087** can ever cross.

**So under Rule B nothing at all snows from altitude in a 128-tall world, and under Rule A
exactly one biome does, in the top four blocks.** Copying vanilla's constant here would ship
a feature that is verifiably dead. This is the single most important finding in Part B and
Part E gives the fix.

## B4. Storm and thunder timing

Decompiled form
([Technical Minecraft Wiki: Weather](https://techmcdocs.github.io/pages/GameTick/Weather/)):

```java
rainTime    = raining    ? random.nextInt(12000) + 12000 : random.nextInt(168000) + 12000;
thunderTime = thundering ? random.nextInt(12000) +  3600 : random.nextInt(168000) + 12000;
```

| Timer | Range (ticks) | Real time | Source |
|---|---|---|---|
| Rain ON | 12000 – 24000 | 10 – 20 min | [Weather](https://minecraft.wiki/w/Weather) |
| Rain OFF (clear) | 12000 – 180000 | 10 min – 2.5 h | same |
| Thunder ON | 3600 – 15600 | 3 – 13 min | same |
| Thunder OFF | 12000 – 180000 | 10 min – 2.5 h | same |

The two flags cycle **independently**; a thunderstorm is the state where both happen to be on
at once. The wiki states the same thing in day units on
[Rain](https://minecraft.wiki/w/Rain): *"The average rainfall lasts 0.5–1 Minecraft day, and
there is a 0.5–7.5 day delay between rains."* — consistent with the tick ranges above
(12000/24000 = 0.5/1.0 days; 12000/180000 = 0.5/7.5 days). **[DERIVED]** Two sources agreeing
in different units is a good sign the numbers are right.

## B5. What rain does, mechanically

All from [Rain](https://minecraft.wiki/w/Rain) unless noted.

- **Only where there is sky access.** The check chain is: is it raining → can this position
  see the sky → is the heightmap at or below this position → is this biome's precipitation
  rain ([MC-265045](https://bugs-legacy.mojang.com/browse/MC-265045)). The heightmap test is
  the cheap one and does most of the work.
- Extinguishes fires and flaming arrows; **does not** extinguish campfires, netherrack fire
  or magma fire.
- Fills cauldrons, slowly.
- Rain sound is audible within **16 blocks**.
- Rain particles fall only in the two middle lines of a block column.

## B6. How rain is actually drawn — not particles

This matters for the 3DS more than any other single point in Part B.

Vanilla does **not** run rain through the particle system. It is drawn by a dedicated weather
pass (`EntityRenderer.renderRainSnow`, later `WeatherEffectRenderer`) as **camera-facing
textured billboard strips, one per world column**, over a square region of columns around the
camera, with the texture V offset animated from the tick counter plus a per-column
pseudo-random offset. Ground splashes are separate real particles spawned by the rain tick.

The strongest evidence that weather is not particles: Sodium Extra's issue tracker records
that **unchecking "Rain" under the Particles option does not stop rain from rendering**
([sodium-extra #431](https://github.com/FlashyReese/sodium-extra/issues/431)), and a whole
mod exists whose purpose is to convert vanilla weather *into* particles
([particle-rain](https://github.com/PigCart/particle-rain)). Both only make sense if weather
is a separate rendering path.

**[NO SOURCE]** for the exact column radius (the widely-repeated figures are 10 columns
Fancy / 5 Fast; I could not pin either to an authoritative page). Do not implement a specific
radius on my say-so — pick it from the frame budget, per Part E.

This is a very good outcome for Blocksmith: **the vanilla approach is already the cheap one.**
A fixed grid of per-column billboard strips with a scrolling V is a small, bounded, static
vertex count that does not grow, does not need a particle allocator, does not need per-particle
physics, and can share one texture. A real particle system — which Blocksmith does not have,
and which the roadmap does not schedule until v1.8.10 — would be both more work and slower.

## B7. Lightning

From [Thunderstorm](https://minecraft.wiki/w/Thunderstorm):

- **1 in 100,000 chance per loaded chunk per tick** during a thunderstorm.
- At ~201 loaded chunks that is *"90% of the time up to 5 strikes/minute, average ≈2.4
  strikes/minute"*.
- Target: random X/Z in the chunk; Y is the block above the highest liquid-or-motion-blocking
  block. 5 HP damage and sets the target alight.

**[DERIVED]** Blocksmith at render radius 3 loads 49 columns × 8 chunks = 392 chunks, so the
raw vanilla rate would give `392/100000 × 20 = 0.078` strikes/second ≈ **4.7 per minute** —
already at the top of vanilla's own range, because Blocksmith's chunks are 16³ sections while
vanilla's are full columns. **If the rate is ported per-section it will be ~8x too frequent.**
Roll it **per column footprint**, not per 16³ chunk.

---

# Part C — Snow that settles in layers, capped at one

## C1. The one-layer cap is confirmed, and it is vanilla's own default

The project owner's requirement is exactly vanilla's behaviour, which the wiki states
directly ([Snowfall](https://minecraft.wiki/w/Snowfall)):

> In Java Edition, snowfall creates one layer of snow by default, and the number of layers
> that can accumulate can be altered by the `max_snow_accumulation_height` game rule.

Game rule specifics ([Game rule](https://minecraft.wiki/w/Game_rule)):

- Default **1**. Minimum **0** (no snow forms at all). Maximum **8** (full block height).
- Introduced Java **1.19.3** (snapshot 22w44a) as `snowAccumulationHeight`.
- Renamed to `max_snow_accumulation_height` in Java **1.21.11** (snapshot 25w44a).

**Bedrock differs** — it accumulates natively per biome up to a full block, no game rule
involved ([Snowfall](https://minecraft.wiki/w/Snowfall)). The wiki's own text on that page is
internally inconsistent about the Bedrock ceiling (it says "maximum of 8" in one place and
"1–12 depending on biome" in another). **Flagged, unresolved, and irrelevant** — Blocksmith
is following the Java rule.

**Beta 1.7.3 already behaved this way**, for a different reason. Stacked layers were added in
Beta 1.5 but ([Snow](https://minecraft.wiki/w/Snow)): *"Stacked snow layers have been added.
They do not generate naturally and cannot be placed."* So in the reference era, natural
snowfall produced **exactly one layer** because the deeper states were unreachable at all.
The requirement and the reference era agree.

## C2. The snow layer block

From [Snow](https://minecraft.wiki/w/Snow):

- Blockstate `layers` **1–8**.
- *"Each layer adds two pixels to the block height"* → **2/16 = 0.125 blocks per layer**.
- Corroborated: a player fits through a 1-block gap with a floor up to **4 layers** high
  (4 × 0.125 = 0.5). And *"a player can jump up 1 block and 3 snow layers"*.
- Silk Touch on 8 layers drops a **snow block** rather than 8 layer items.
- **Collision history**: Beta 1.5 — *"Layers 1-3 have no collision, while 4-8 all have a half
  block collision."* Java 1.13 — *"Layer 1 snow now has empty collision instead of solid
  collision."* So in modern vanilla, **a single layer has no hitbox at all**: you walk
  through it and it does not slow you or step you up.
- *"Snow blocks vision if it is 8 layers tall, but it never suffocates"* — implying layers 1–7
  are non-opaque.

**[NO SOURCE]** for a numeric light opacity (0–15) for the layer block. The "blocks vision
only at 8 layers" statement is consistent with opacity 0 below that, but no page says so in
those words. **[INFERENCE]** For Blocksmith, a one-layer cap makes this moot: **give the snow
layer opacity 0 and no collision.** That is one less thing to relight and one less thing to
collide against, and at one layer deep it is also what vanilla does.

## C3. Where a snow layer is allowed to form

Four conditions, all of which must hold ([Snow](https://minecraft.wiki/w/Snow)):

> snow generates on random blocks with a complete solid top surface at integer y-values, with
> a **block light level of 9 or less**, with the exception of ice and packed ice

plus:

> Only blocks with direct access to the sky can generate snow layers naturally.

So:

1. **Sky access** — the position must be the top of its column (heightmap test).
2. **Block light ≤ 9.** Note: **block light only**, not sky light. This is what makes a torch
   clear a patch of ground around it during a blizzard, which is a strong readable signal and
   worth keeping.
3. **Solid full top face** on the supporting block, and it must not be ice.
4. **The biome must currently be snowing** — effective temperature < 0.15 and the biome has
   precipitation at all.

### The accumulation rate

From [Tick](https://minecraft.wiki/w/Tick): during each chunk tick there is a **1/16 chance
that one column is chosen for weather checks on the topmost block** — the same single check
decides freezing, snow layer formation and cauldron filling. This is *separate* from
`random_tick_speed` (default 3), which drives ordinary block random ticks.

**[COMPUTED]** what that rate actually produces, for a 16×16 chunk footprint (256 positions):

| Quantity | Value |
|---|---|
| Expected ticks per weather check, per chunk | 16 |
| First ~10 layers appear after | 160 ticks ≈ **8 s** |
| Half the positions checked after | 2831 ticks ≈ **2.4 min** |
| 94.7% of positions checked after | 12000 ticks (the shortest storm) |
| 99.7% of positions checked after | 24000 ticks (the longest storm) |
| Every position (coupon-collector) | ~25,085 ticks ≈ **21 min ≈ 1.05 MC days** |

That is the shape of the feel: **snow appears within seconds, covers half a chunk in a couple
of minutes, and is essentially complete by the end of a single storm** — but never quite
finishes, so there are always a few bare patches. That last property is emergent from the
random selection and is worth not "fixing".

**[NO SOURCE]** for any published wall-clock measurement of snow coverage; the table above is
my arithmetic on the cited 1/16 rate, not a measurement of the real game.

## C4. What snow can and cannot sit on

Modern Java uses block tags ([Snow](https://minecraft.wiki/w/Snow)):

> snow can be placed on a block with the `snow_layer_can_survive_on` tag, or a block with a
> full top face and **not** with the `cannot_support_snow_layer` tag.

The **blacklist** is short — `minecraft:ice`, `minecraft:packed_ice`, `minecraft:blue_ice`
([Block tag](https://minecraft.wiki/w/Block_tag_(Java_Edition))). Ice takes priority over
having a full top face. Whether `barrier` is also in it could not be confirmed —
**[NO SOURCE]**, conflicting reports.

Historically permissive: Alpha v1.0.5 — *"Snow can now fall onto many non solid blocks
including glass, stairs, slabs, doors, signs, and pressure plates."*
([Snow](https://minecraft.wiki/w/Snow)). No source says this was later reversed, but the
complete modern `snow_layer_can_survive_on` whitelist **could not be retrieved** —
**[NO SOURCE]** for leaves, fences, slab orientation, and stacking on other snow.

Support removal: *"In Java Edition, snow breaks if its support block is removed."* (Bedrock
makes it fall instead.) Mined with a shovel it drops **1 snowball per layer**.

**[INFERENCE]** For Blocksmith this reduces to a rule you can state in one line and test:
**snow forms on a block whose top face is full and solid, and never on ice.** With one layer
of depth and no partial blocks in the game yet, nothing else in the current registry is an
edge case.

## C5. Melting

From [Snow](https://minecraft.wiki/w/Snow): *"Snow melts if there is a heat block
(Bedrock/Education only) or **block light level of 12 or more**."*

- **Block light only.** Sky light does not melt snow — snow survives full daylight. This is
  stated explicitly for the parallel ice rule (*"Sky light level is ignored, therefore ice
  does not melt from sunlight"*, [Ice](https://minecraft.wiki/w/Ice)) and the snow rule is
  written the same way.
- **Java snow does not melt from being in a warm biome.** Place a snow layer in a desert and
  it stays. Bedrock does melt it. This is a real behavioural fork and Java's answer is the
  simpler one to implement. **[Moderate confidence]** — this negative claim comes from a
  synthesis of the Snow page rather than one verbatim sentence.
- Multiple layers melt **all at once** in Java, gradually in Bedrock. Moot at a cap of 1.
- Melting drops **nothing**. Mining drops snowballs.

**Note the asymmetry:** snow forms at block light **≤ 9** and melts at block light **≥ 12**.
There is a **dead band at 10 and 11** where it neither forms nor melts. That hysteresis is
deliberate and load-bearing — without it, a block sitting exactly at the threshold would
flicker on and off forever, and on this hardware every flicker is a chunk remesh. **Do not
collapse the two thresholds into one.**

Ice is the same idea with different numbers: freezes at block light **< 10**, melts at block
light **> 11** ([Ice](https://minecraft.wiki/w/Ice)) — a dead band at 10–11 again.

## C6. Ice, since it shares the whole mechanism

From [Ice](https://minecraft.wiki/w/Ice): water freezes when it is a **source block**, in a
snowy biome (base temperature ≤ 0.05), **exposed to sky from directly above**, **block light
< 10**, and has at least one horizontally adjacent non-water block. Flowing water does not
freeze — **[moderate confidence]**, this came through a fetch synthesis rather than a
verbatim quote.

That "adjacent non-water block" clause is what stops the middle of a lake freezing before its
edges, and it is cheap: four neighbour reads.

---

# Part D — Read-only survey of Blocksmith

Everything in this part was read out of the tree at the time of writing. It is here because
several v1.8.9 recommendations are much cheaper than a generic description would suggest, and
one of them is nearly free.

## D1. The day/night hook already exists

`source/shaders/world_dynamic.v.pica` declares, at line 36:

```
.fvec dayLevel[1]
```

with the header comment (lines 13–18):

> `brightness = faceShade[nrm] x aoCurve(ao) x max(sky x dayLevel, block) / 15`
>
> With dayLevel 1.0 and full sky light the multiplier is exactly 1 ... **dayLevel is a
> uniform so a future day/night cycle becomes one register write per frame, not a re-mesh.**

The light math itself, lines 166–178, unpacks `MeshVertex.pad` (`sky*16 + block`) into two
channels and combines them as:

```
mul r5.z, dayLevel.xxxx, r5.xxxx     ; day-scaled sky
max r5.w, r5.zzzz, r5.yyyy           ; lum = max(daySky, block)
```

`source/scene/chunk_render.c:862` currently writes:

```c
C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_daylevel, 1.0f, 1.0f, 1.0f, 1.0f);
```

inside the per-bind uniform block, with a comment that says it is pinned because no cycle
exists yet.

**Consequence: the entire "world gets dark at night, torches do not" behaviour is one float,
written once per frame, with no remesh and no vertex format change.** Block light is already
excluded from the dimming by the `max()`. This is the largest single saving available in
v1.8.9 and it was designed in three versions ago.

## D2. How light is stored

`source/world/light.c` / `light.h`:

- Two nibble channels per column, `sky[]` and `blk[]`, each `LIGHT_COL_BYTES = 16384`, packed
  two cells per byte. `LightColumn` is **32 KiB per column**, hung off `Column.light`.
- Max level **15**. Sky light is stored **raw at 15** — there is no time-of-day dimming baked
  into the array, which is exactly right: the array is the gameplay truth, the uniform is the
  render dimming.
- The light-to-vertex path is **baked at mesh time**: `mesher.c` `cornerLight()` averages four
  cells per corner and writes `sky<<4 | block` into `MeshVertex.pad`. So anything that changes
  a *stored* light value costs a remesh; anything that changes the *time of day* does not.
- Public surface: `lightGetSky`, `lightGetBlock`, `lightPropagateColumn`, `lightRelightColumn`.
  Edit-path relight was measured at **0.141 ms per block** (down from 4.064 ms) in v1.8.0.

**There is no 16-entry brightness LUT.** The shader normalises linearly with `inv15`. Per A4,
that is the wrong curve, and Part E has the three-instruction fix.

## D3. The TexEnv budget: 2 of 6 stages used

`source/scene/chunk_render.c`:

- **Stage 0** (lines 738–741): `GPU_MODULATE(GPU_TEXTURE0, GPU_PRIMARY_COLOR)` — atlas texel
  times the baked light/AO vertex colour.
- **Stage 1** (lines 814–822): `GPU_INTERPOLATE(GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1)` —
  fog, blending the terrain colour toward the constant sky colour by the fog ramp's alpha.
  Alpha passes through.
- **Stages 2–5: unused.** citro3d leaves them at the default passthrough.

The PICA200 has **6 combiner stages and 3 texture units**, no programmable fragment shader
([3dbrew GPU/Internal Registers](https://www.3dbrew.org/wiki/GPU/Internal_Registers),
[GBATEK TexEnv registers](http://problemkaputt.de/gbatek-3ds-gpu-internal-registers-texturing-registers-environment.htm)).
So **4 stages and 1 texture unit are free**. Two constraints on using them, both from the
same sources: only the **first four** stages can write the TEV buffer, and a TEV buffer write
in one stage is **not readable until two stages later**.

The hardware fog unit is **deliberately disabled** (`C3D_FogGasMode(GPU_NO_FOG, ...)` around
`chunk_render.c:770`) and replaced by the ramp texture — `source/gfx/fogramp.h` explains why
at length: the PICA fog LUT is indexed by 1/w, its knots bunch against the camera, and the
half-visibility distance saturated at ~14.4 blocks no matter the render distance. **Do not
try to reintroduce the hardware fog unit for weather.** The note that fog does not consume a
TexEnv stage on PICA200 is true of the *hardware* unit; Blocksmith's replacement does consume
one, and that is a price already paid for a measured reason.

## D4. The sky is a compile-time constant, in two byte orders

`source/scene/chunk_render.h:44–51`:

```c
#define SKY_CLEAR_RGBA8  0x102A33FF
#define SKY_FOG_BGR      0x00332A10
```

with the comment: *"the sky colour, in one place because step 6.4's fog has to be the same
colour as the background it fades into. Fog that does not match the clear colour reads as a
grey sheet hung in front of the sky instead of as distance."*

`main.c:84` aliases it to `CLEAR_COLOR` and clears the top screen with it at three call sites
(1631, 2235, 2779). `chunk_render.c:822` feeds the same three bytes to `C3D_TexEnvColor` for
the fog stage.

So today the sky is **(16, 42, 51) — a dark teal**, fixed, and there is **no sky geometry, no
sun, no moon and no stars**. Making the sky change colour with time means turning one macro
into one variable and writing it to two places per frame. The invariant the comment protects —
clear colour and fog colour are the same number — must be preserved, and is trivially
preserved if both read the same variable.

## D5. Biome and temperature: a different scale, and no cache

`source/world/worldgen.h`:

```c
typedef enum { BIOME_TUNDRA=0, BIOME_TAIGA, BIOME_PLAINS,
               BIOME_FOREST, BIOME_DESERT, BIOME_JUNGLE, BIOME_COUNT } BiomeId;
```

A 3×2 grid — three temperature bands crossed with two humidity bands:

|  | DRY | WET |
|---|---|---|
| **COLD** | TUNDRA | TAIGA |
| **MILD** | PLAINS | FOREST |
| **HOT** | DESERT | JUNGLE |

Temperature is **not a stored per-biome field**. It is a continuous noise field, thresholded
at lookup:

```c
temp = FX_ONE - worldgenBiome(g, x, z);       // worldgen.h:298
#define GEN_TEMP_HOT   (FX_ONE - GEN_SAND_BELOW)   // 0x8800 = 0.5312
#define GEN_TEMP_COLD  0x00005000                  // 0.3125
#define GEN_HUMID_WET  0x00008000                  // 0.5
```

`FX_ONE = 1<<16` (`source/world/noise.h`). So Blocksmith's temperature runs **0.0 to 1.0**,
not Minecraft's −0.5 to 2.0. Band widths **[COMPUTED]**: COLD is 20480 fx (31.25% of range),
MILD is 14336 fx (21.88%), HOT is 30720 fx (46.88%).

**Two things about this that change the design:**

**(a) `worldgenBiomeAt` is not cached.** Every call runs a fresh `noiseFbm2` at
`GEN_BIOME_OCTAVES = 2`, plus a second for humidity. Nothing memoises it per column. A
per-tick weather check that calls it is paying two fBm evaluations every time.

**(b) Temperature is the *inverted terrain* field, so cold is already high ground.** From the
header comment at worldgen.h:298:

> **Temperature is the existing biome field INVERTED**: `temp = FX_ONE - worldgenBiome()`.
> ... The direction matters: the biome field's high end is the tall broken ground of the
> density table's upper control points, so reading it as "hot" would put deserts on mountain
> tops and tundra on beaches. Its low end is the flat lowland.

**[INFERENCE — this is my reading of that comment, not a measurement]** If high biome field
means tall ground, and temperature is its inverse, then **Blocksmith's mountains are already
cold and its lowlands already warm, structurally, before any altitude rule is added.** That
is the effect Minecraft's altitude cooling exists to produce, arriving for free from a
decision made for a different reason. It also means an altitude term added on top would be
**double-counting**. This should be measured before it is either relied on or added to — see
Open Questions.

## D6. Ticks: no world clock, no persistence, no random tick

`source/world/tick.h`: `TICK_HZ 20`, `TICK_PERIOD_US 50000`. `TickClock` carries
`accum_us, count, dropped, max_catchup`; `tickClockCount()` is a **monotonic run-count since
process init**, not a saved game time. Distance decimation exists (`TICK_NEAR_BLOCKS 24`,
half-rate beyond) but it was built for mob AI, not for blocks.

Three gaps, all of which v1.8.9 must fill:

1. **No world time variable exists** and nothing persists one. `region.c` and
   `chunk_codec.c` have no time field.
2. **No random-tick mechanism exists at all.** Nothing walks a chunk picking random cells.
   Snow accumulation needs this built from scratch.
3. **A strong hint about where the clock should live.** `source/world/region.c:81` refers to
   *"a defect that travelled by clone through world/worldseed.c, world/daytime.c and
   world/genversion.c"* — and **`world/daytime.c` does not exist in the tree.** Both named
   siblings (`worldseed.c`, `genversion.c`) persist a small **sidecar file** in the world
   directory rather than a region/chunk field. **[INFERENCE]** That comment reads as a
   design already decided and not yet written: day-time belongs in `world/daytime.c` as a
   sidecar, matching the two files it is named alongside. Worth confirming with the author,
   but it is the least surprising place to put it and it avoids touching the region format.

## D7. The mesher already has an 8-step partial-height block

This is the second big saving, and it is not obvious.

`source/world/mesher.h:104–120` — `MeshVertex.nrm` packs a face index in bits 0–2 and a
**"water height drop" in bits 3–5**, giving 8 drop levels. `chunk_render.c` fills a 64-row
`faceShade` uniform table where row `face + 8*d` carries a y offset of `-d/8`, and the vertex
shader adds it. `mesh_vertex.h` confirms the whole thing is already shipping.

**A snow layer is a block with a drop of 7 (7/8 of the way down).** The vertex format, the
uniform table, the shader instruction and the greedy-mesher path for partial-height top faces
all already exist and are already tested on hardware by the water surface.

One thing to watch: `meshNrmAlpha` (mesher.h:144) returns `WATER_ALPHA = 0.70f` for any drop
`>= WATER_SURFACE_DROP`, and `WATER_SURFACE_DROP` is **1** (`scratch.h:51`). So **every**
non-zero drop is currently semi-transparent. Snow needs to be opaque, so the alpha rule has to
become "water is transparent" rather than "any drop is transparent" — a real change, to a line
that `world/water_alpha_test.c` parses out of both `.pica` files. It is a small change in a
carefully-guarded place; budget for the test churn, not for the code.

## D8. What "snow" means in the tree today

`BLOCK_SNOW = 10` exists (`source/world/block.h:115`) but it is a **full solid cube** —
`REG_FLAG_SOLID`, hardness 8, texture `BTEX_SNOW` (`registry.c:215–221`) — placed once at
generation time as the tundra surface cap by `worldgen_density.c`. **It is not a layer, has
no depth state, and does not accumulate.** `BLOCK_ICE = 11` exists similarly as a water
surface cap.

There is also already a greyed-out **"Weather"** entry in the debug menu (`main.c:1140`,
`e->available = false`) with a comment saying a later version flips the flag rather than
redesigning the menu.

And no particle system exists anywhere — `grep particle` over `source/` returns nothing.
Per the roadmap, particles are **v1.8.10**, i.e. *after* this version. **v1.8.9 must not
assume a particle system.** Per B6, it does not need one.

---

# Part E — The verdict: what to build, what to approximate, in what order

## E1. The clock, and how much of it to keep

**Recommendation: keep vanilla's numbers exactly.** 24000 ticks, 20 tps, phase boundaries at
0/12000/13000/23000. Blocksmith's tick rate is already 20 Hz, so this is a `uint32_t` counter
incremented in the existing tick path and wrapped at 24000. There is no cost argument for
changing any of it and every reason not to: the whole rest of this brief's numbers are
indexed by that tick.

Persist it per world as a sidecar in `world/daytime.c`, per D6. **[INFERENCE]** A world that
reloads at whatever time it was saved at is worth more than the four bytes.

## E2. Day/night light — exact, and it costs one float per frame

Compute `skylightSubtracted` per A3 and drive the shader from it. Two separate consumers,
which must not be confused:

**The renderer** uses the multiplicative sky scale, because that is what vanilla's client
does. The lightmap builder is explicit
([MCP-919 `EntityRenderer.updateLightmap`](https://github.com/Marcelektro/MCP-919/blob/main/src/minecraft/net/minecraft/client/renderer/EntityRenderer.java)):

```java
float f  = world.getSunBrightness(1.0F);
float f1 = f * 0.95F + 0.05F;
float f2 = lightBrightnessTable[skyLevel]   * f1;             // sky column, SCALED
float f3 = lightBrightnessTable[blockLevel] * (flicker * 0.1F + 1.5F);  // block, BOOSTED
```

`getSunBrightness` is the same cosine expression with `+0.2F` instead of `+0.5F`, and returns
`f1 * 0.8F + 0.2F` — **range 0.2 to 1.0**. So:

> **`dayLevel = getSunBrightness(tick)`**, clamped to 0.2..1.0, written to the existing
> uniform in `pipelineBind`. One `C3D_FVUnifSet`, once per frame. **Nothing else changes.**

`max(sky * dayLevel, block)` already leaves torches undimmed. This is vanilla's own model,
not an approximation.

**[COMPUTED]** `dayLevel` at key ticks: 1.000 through midday; 0.704 at 12000; 0.487 at 12500;
0.263 at 13000; **0.200 floor from 13188 to 22974**; 0.470 at 23460; 0.687 at 23960.

**The gameplay side** uses the integer `skylightSubtracted` (0..11) and the *subtractive*
rule, because that is what vanilla's server does and it is what snow, ice and any future mob
spawning must read. Keep both. They are cheap and they are not the same function.

Two deliberate deviations from vanilla, both worth stating in the changelog:

- Vanilla combines sky and block **additively** (`f4 + f3`) with block light boosted 1.5x;
  Blocksmith uses `max()`. The `max()` is one instruction and the `add` needs a clamp.
  **Consequence: a torch at night will read slightly dimmer and less warm than vanilla's.**
  **[INFERENCE]** Accept it for v1.8.9; revisit in v1.8.10 when torches land, which is the
  version where it will actually be visible.
- Blocksmith's `dayLevel` is a continuous float, so there is **no stepping** as the light
  falls. Vanilla's integer `skylightSubtracted` produces 11 visible steps over 81 seconds.
  Smoother is better here and costs nothing.

## E3. The brightness curve — three instructions, and skipping it is the biggest risk

Per A4, a linear ramp renders full night **3.2x too bright**. The fix is exact and small:

```
b = l / (4 - 3*l)
```

On the PICA200 vertex shader that is a `mad` (compute `4 - 3l`), an `rcp`, and a `mul` —
**three instructions, once per vertex**, inserted after the existing
`max r5.w, r5.zzzz, r5.yyyy`. No new uniform, no new attribute, no remesh, no TexEnv stage.

**[INFERENCE]** I would treat this as non-optional. It is the difference between "the world
gets darker" and "it is night". If it must be cut for instruction budget, the fallback is to
fold an approximation of the curve into `dayLevel` itself on the CPU — but that is wrong for
block light (which vanilla curves too) and will make torch-lit caves look flat, so it is a
real degradation rather than an equivalent.

## E4. Sky colour, fog colour, and the sunset band

**Sky and fog.** Turn `SKY_CLEAR_RGBA8` / `SKY_FOG_BGR` from macros into one runtime colour
computed per frame, feeding both `C3D_RenderTargetClear` and the stage-1
`C3D_TexEnvColor`. The invariant the header comment protects — clear colour equals fog colour —
is preserved by construction if both read the same variable. **Cost: one colour computation
and one extra register write per frame. Effectively free.**

Use vanilla's *curve*, not vanilla's *palette*. The shape is:

```
f = clamp(cos(celestialAngle * 2*PI) * 2 + 0.5, 0, 1)     // same expression as the light curve
r = base_r * (f * 0.94 + 0.06)
g = base_g * (f * 0.94 + 0.06)
b = base_b * (f * 0.91 + 0.09)
```

with `base` being Blocksmith's own `(16, 42, 51)` rather than vanilla's `(192, 216, 255)`.
**Keep the asymmetric floors** (0.06 red/green, 0.09 blue) — they are what stops the night sky
going grey, and they are one constant each.

**[INFERENCE]** Blocksmith's daytime sky is already very dark (16, 42, 51 against vanilla's
192, 216, 255). Multiplying it down to 6% at midnight may put it below what a 3DS top screen
can resolve against black. **Measure it on hardware before committing**; if it disappears,
raise the floors rather than raising the base, because the base is also the fog colour and
raising it washes out the distance fade.

**The sunset band.** Per A6 it is a warm orange (peaking at 217, 96, 51) that is up for ~2940
ticks each side and is blended only where the camera looks toward the sun.

**[INFERENCE] Recommendation: implement it, but as a change to the sky/fog colour, not as
geometry.** Blend the band colour into the same runtime sky colour by `alpha × max(0, dot(
cameraForward, sunDirection))`, computed **once per frame on the CPU**. That is a handful of
floats per frame and **zero** additional GPU work, because it lands in a register that is
already being written. Drawing an actual gradient band as geometry would need a stage and a
texture unit and would look worse, because the fog already fades everything to the sky colour
at distance — so tinting the sky colour tints the horizon automatically. **This is the cheap
approximation that preserves the feel, and I think it is genuinely better than the real thing
on this hardware.**

The cost of getting this wrong is only that the sunset is uniform across the sky rather than
concentrated toward the sun. The `dot` term recovers most of that for free.

## E5. Temperature, the snow line, and the altitude constant

**Do not port 0.15.** Blocksmith's temperature is a 0..1 fx field with completely different
band edges; 0.15 on that scale means "the coldest 15%", which is not what it means in vanilla.

**[INFERENCE] Recommendation: the snow line is `GEN_TEMP_COLD` (0x5000).** It already selects
TUNDRA and TAIGA — the two biomes that *are* the snowy biomes. Reusing the existing constant
rather than inventing a new one is the same discipline `GEN_TEMP_HOT` already follows
(worldgen.h:305 explains why at length), and it makes the snow line and the biome border the
same line by construction rather than by two numbers agreeing.

The precipitation table then falls out with no new fields:

| Biome | fx temperature band | Precipitation |
|---|---|---|
| TUNDRA | cold, dry | **snow** |
| TAIGA | cold, wet | **snow** |
| PLAINS | mild, dry | **rain** |
| FOREST | mild, wet | **rain** |
| DESERT | hot, dry | **none** |
| JUNGLE | hot, wet | **rain** |

Desert gets nothing by declaration, matching vanilla's `downfall = 0` mechanism (B1) rather
than a hot-side threshold. Jungle rains, which is both correct and the one place a hot-side
threshold would have got it wrong.

### The altitude constant — vanilla's is dead, here is a live one

Per B3, vanilla's coefficient produces **zero** altitude snow in a 128-tall world under the
modern rule and one biome's top four blocks under the old one. **[COMPUTED]** candidates on
Blocksmith's fx scale, cooling `k` fx per block above y=64:

| k (fx/block) | drop over y 64→128 | % of the MILD band crossed | mild columns colder than … snow somewhere |
|---|---|---|---|
| 44 (vanilla Rule A, rescaled) | 2796 | 19.5% | 0x5AEC |
| 33 (vanilla Rule B, rescaled) | 2097 | 14.6% | 0x5831 |
| 64 | 4096 | 28.6% | 0x6000 |
| **96** | **6144** | **42.9%** | **0x6800** |
| **128** | **8192** | **57.1%** | **0x7000** |
| 192 | 12288 | 85.7% | 0x8000 |

The middle two rows are the useful band. **Recommendation: `k = 128`, i.e.**

```
effectiveTemp = baseTemp - ((y - 64) << 7)      // for y > 64; unchanged at or below 64
```

Two reasons for 128 specifically: it is a **pure left shift**, which matters on a 268 MHz
ARM11 with no hardware divide and in a function that may run tens of times a second; and at
57% of the mild band it means **roughly the cooler half of plains and forest columns turn
snowy at some reachable height**, which reads as "mountains are white, valleys are not"
rather than as a feature nobody ever sees. `k = 96` is the conservative alternative if 128
makes too much of the world snowy in playtesting — it is the same shift plus a halved
addend.

**Add the jitter.** Vanilla's Rule A jitter is worth ±4 blocks of equivalent height and
Rule B's is stated as up to ±8 blocks; both exist to stop the snow line being a flat contour.
**[INFERENCE]** A cheap equivalent: perturb the threshold by a hash of `(x >> 3, z >> 3)`
scaled to about ±4 blocks' worth of `k`, i.e. ±512 fx at `k = 128`. That is one hash and one
add, reuses the coarse-cell hashing idiom the project already uses elsewhere, and needs no
noise evaluation.

**Before implementing any of this, measure the D5 correlation.** If Blocksmith's temperature
field is already strongly anti-correlated with terrain height — which the worldgen.h comment
says it is by construction — then an altitude term is double-counting and `k = 128` may
produce far more snow than the table above suggests. The measurement is cheap: sample
`(worldgenBiome(x,z), surfaceHeight(x,z))` over a few thousand columns across several seeds
and correlate. **[INFERENCE]** I would do this measurement *first* and pick `k` from it,
rather than picking `k` and discovering it in playtest.

## E6. Snow layers — reuse the drop bits

**Recommendation: a new block id `BLOCK_SNOW_LAYER`, meshed with `nrm` drop = 7.**

Per D7 the machinery exists and ships. What is needed:

1. A registry entry: **not solid**, **opacity 0**, **no collision** (matching vanilla's
   layer-1 empty collision, C2), texture `BTEX_SNOW` on top.
2. The `meshNrmAlpha` change in D7 — transparency must key on "is water", not "has a drop".
   `world/water_alpha_test.c` parses that line out of both `.pica` files, so expect that
   test to need updating alongside.
3. The greedy mesher must be checked for merging snow layer top faces with water top faces —
   both now carry a non-zero drop and they must not merge into one run. **[INFERENCE]** The
   merge predicate almost certainly already compares block id, but this is the one place
   where reusing the drop bits could bite, and it is worth an explicit test.

**Cap: exactly one layer, hard.** No depth state, no `layers` field, no game rule. That is
both the project owner's requirement and vanilla's default (C1) and Beta 1.7.3's actual
behaviour (C1). It also removes the whole "stacking" branch from the placement code.

**Placement rule** (C3), all four conditions:

```
canSnowAt(x, y, z):
    y == topSolid(x, z) + 1            // sky access, from the heightmap
    AND blockLight(x,y,z) <= 9
    AND blockBelow has a full solid top face AND is not BLOCK_ICE
    AND biome is currently snowing at this (x, z, y)
```

**Melting** (C5): remove when `blockLight >= 12`. **Keep the 10–11 dead band.** Do not melt
from sky light and do not melt from biome temperature — Java's rule, and the cheaper one.
Melting drops nothing.

## E7. The accumulation tick — and the remesh trap

Per D6 there is **no random-tick mechanism**. Build the narrow one, not the general one:
vanilla's weather check is not a random block tick, it is *"a 1/16 chance that one column is
chosen for weather checks on the topmost block"* ([Tick](https://minecraft.wiki/w/Tick)). One
position per chunk per 16 ticks, at the surface only.

**[COMPUTED]** the CPU cost at Blocksmith's radii, rolling **per loaded column** (16×16
footprint), not per 16³ chunk:

| Render distance | Loaded columns | Checks/tick | Checks/second |
|---|---|---|---|
| radius 3 (O3DS) | 49 | 3.06 | 61 |
| radius 4 | 81 | 5.06 | 101 |
| radius 5 (N3DS) | 121 | 7.56 | 151 |

Each check is a heightmap read, a block-light nibble read, a temperature read and a compare.
**That is nothing.** The rate is not the problem.

**The remesh is the problem, and it is the one number that could sink this version.** Every
snow layer placed dirties a chunk. At 151 checks/second on a New 3DS, in a snowy biome where
most checks succeed, that is **up to ~151 chunk remeshes per second** landing in a system that
was budgeted for player edits at a few per second. **[DERIVED]** This will not hold a frame
rate.

**[INFERENCE] Two mitigations, both needed:**

- **Coalesce per chunk per frame.** The project already has `dirtyq` / `meshq` / `remesh`
  for exactly this shape of problem. Snow placements should mark dirty and let the existing
  drain decide, never remesh inline. Several placements in one chunk in one frame must cost
  one remesh.
- **Cap snow remeshes per frame separately from edit remeshes**, so a blizzard cannot starve
  the player's own block placements of the mesh budget. A snow layer appearing a second late
  is invisible; a placed block appearing a second late is not.

**Measure this before believing it works.** The success criterion is concrete: *stand in an
open tundra field on an Old 3DS, start a snowstorm, and hold the frame rate the game holds
with weather off.* A host-side test can count remesh requests per simulated second and go red
above a threshold; that check can be built before the feature and will actually distinguish
fixed from not-fixed.

## E8. Drawing the rain and the snow

Per B6, vanilla does **not** use particles, and per D8 Blocksmith has no particle system and
is not scheduled to get one until v1.8.10. This lines up perfectly.

**[INFERENCE] Recommendation: a fixed grid of vertical billboard strips around the camera,
one draw call, one texture, V offset scrolled from the tick counter.**

- **The vertex count is fixed and known at compile time.** An 11×11 column grid is 121 quads
  = 484 vertices — smaller than a single terrain mesh slot. It does not grow with weather
  intensity, it does not allocate, and it cannot spike.
- **One texture unit.** Unit 2 is free (D3). One extra TexEnv stage for the alpha blend
  leaves two spare.
- The per-column pseudo-random V offset (vanilla's trick) is a hash of the column coordinate,
  computed once when the grid is rebuilt, not per frame.
- **Rain and snow are the same geometry with a different texture and a different scroll
  speed.** Vanilla slants and stretches rain and drifts snow gently; both are texture and
  scroll-rate changes, not geometry changes.
- Skip columns whose surface is not sky-exposed — same heightmap read the accumulation check
  already does.

**Pick the grid radius from the frame budget, not from vanilla.** The widely-repeated 10/5
figures are **[NO SOURCE]** and, more importantly, vanilla's radius was chosen against a
render distance far beyond Blocksmith's. **[INFERENCE]** Blocksmith's fog reaches half
visibility at roughly 14 blocks; a weather grid wider than the fog is invisible work. Start at
a radius that covers the visible distance and no more — likely 5 to 8 columns — and measure.

**What to cut if it does not fit:** the ground splash particles (which vanilla *does* spawn as
real particles) are the first thing to drop — they need the particle system that does not
exist yet, and their absence is far less noticeable than the falling streaks'. Cut them and
revisit in v1.8.10.

## E9. Weather state, timing, and light

Keep vanilla's timers verbatim (B4) — they are two `random.nextInt` calls and a countdown, and
the ranges are what produce "rain is uncommon but not rare". Keep the **±0.01 per tick
strength ramp**; a storm that snaps on is the single most obvious tell of a cheap
implementation, and 100 ticks of lerp costs one add and one clamp.

Feed `rainStrength` and `thunderStrength` straight into the A3 formula. Both the render
`dayLevel` and the gameplay `skylightSubtracted` then darken during weather automatically,
with no separate weather-darkening code path — which is also why it will be consistent.

Flip `main.c:1140`'s `"Weather"` debug entry to `available = true` (D8) — the comment there
says that is exactly the intended migration.

**Lightning: [INFERENCE] cut it from v1.8.9.** It needs a light source that does not exist, a
fire mechanic that does not exist, and per B7 the vanilla rate ported naively to Blocksmith's
16³ sections would be ~8x too frequent. The roadmap entry for v1.8.9 does not mention it. A
thunderstorm that only darkens the sky and intensifies the rain is a complete feature.

## E10. Recommended implementation order

Ordered so that each step is independently verifiable and each one is playable before the
next starts, per the project's phase-boundary rule.

**Phase 1 — the clock.** World time counter in the tick path; sidecar persistence in
`world/daytime.c`. Verify on the host: 24000 ticks wraps, a save/load round-trips the time.
Nothing visible yet.

**Phase 2 — day/night light.** `getSunBrightness` curve driving the existing `dayLevel`
uniform, plus the three-instruction brightness curve (E3). **This is the phase that delivers
most of the version's felt value and it is the smallest.** Verify: a host table of `dayLevel`
against the A3 tick boundaries, then look at it on hardware. Playtest ask: *stand outside and
watch a full sunset — the light should hold steady, drop over about eighty seconds, then hold
steady again; a torch should not dim with it.*

**Phase 3 — sky and fog colour.** Runtime sky colour, the day/night curve, the sunset band
blended by `dot(look, sun)`. Verify the clear colour and TexEnv constant never disagree.
Playtest ask: *look east and west at sunset — the horizon you are facing the sun across should
warm up, and the sky should never go grey.* This is also where the E4 "is 6% of a dark teal
visible on a 3DS screen" measurement happens.

**Phase 4 — sun, moon, stars.** Untextured white star quads (cheapest, per A7), then the two
celestial quads. **[INFERENCE]** Take the north/south vs east/west decision (A5) before
writing this, not during.

**Phase 5 — the temperature model.** Per-column temperature cache (D5a), the altitude term
with `k` chosen from the D5b correlation measurement, the per-biome precipitation table (E5).
Verify entirely on the host: sweep a few thousand columns per seed and report what fraction of
the world is snow / rain / dry, and at what heights the snow line lands. **No rendering, all
measurement.**

**Phase 6 — weather state and rain rendering.** Timers, strength ramps, the billboard grid.
Verify the frame rate on an Old 3DS in a storm before anything else.

**Phase 7 — snow layers.** The block, the drop-bit meshing, the weather-check tick, the
remesh coalescing and cap. Verify with the E7 red-armed remesh-rate check *before* claiming
it holds frame rate. Playtest ask: *stand in a tundra field through one full storm — snow
should appear within seconds, cover most of the field over a few minutes, cap at one layer
everywhere, and a torch should hold a bare circle around itself.*

If the version has to be cut short, **Phases 1–3 alone are a shippable release** and they are
the cheapest three. Phases 5 and 7 are where the real cost is.

---

# Open questions and gaps

1. **North/south vs east/west sun.** Beta 1.7.3 rose north and set south; the legacy console
   editions rose east and set west (A5). Both are "accurate" to something the project names as
   a reference. This is the project owner's call and it should be made before Phase 4.
   Consequence of ignoring it: whichever is implemented will be defended as correct
   afterwards, and it is far cheaper to decide now.

2. **Is Blocksmith's temperature already anti-correlated with height?** `worldgen.h:298`
   states by construction that the temperature field is the inverted terrain field, so
   mountains should already be cold (D5b). **This is my reading of a comment, not a
   measurement**, and it decides whether the altitude term in E5 is a feature or a
   double-count. The measurement is a few thousand `(temp, surfaceHeight)` samples over
   several seeds. Consequence of not doing it: `k = 128` may bury half the world in snow, and
   the cause will not be obvious from playtest.

3. **The `world/daytime.c` sidecar.** `source/world/region.c:81` names a file that does not
   exist, alongside two that do and that both use sidecar persistence (D6). I read that as a
   design already decided by whoever wrote the comment. If it is not, the alternative is a
   field in the region header, which is a save-format change. Worth a one-line confirmation
   before Phase 1 rather than after.

4. **`meshNrmAlpha` and the greedy merge.** Reusing the drop bits for snow (E6) requires
   changing a line that `world/water_alpha_test.c` parses out of two `.pica` files, and
   requires confirming the mesher will not merge a snow top face with a water top face. I did
   not read the merge predicate. If it does not already discriminate by block id, this is the
   one place where the otherwise-free reuse becomes real work.

5. **Beta 1.7.3's own numbers do not exist in any source I could find.** No page gives Beta
   1.7.3's rain/snow temperature threshold, its snow-formation light level, or its weather
   check rate. Everything numeric in Parts B and C is a **modern Java figure**. What *is*
   well-supported for the reference era: weather was added in Beta 1.5, altitude snow did not
   exist until 2013, moon phases did not exist until Beta 1.9pre4, and natural snowfall
   produced exactly one layer. **[INFERENCE]** Use the modern numbers — they are the only ones
   that exist, they produce the behaviour the roadmap describes, and the Beta-era alternative
   is not documented enough to implement even if it were wanted.

6. **The complete `snow_layer_can_survive_on` whitelist** could not be retrieved (C4), so
   leaves, fences, slab orientation and snow-on-snow are unsourced. **[INFERENCE]** At a
   one-layer cap with no partial blocks currently in the registry, "full solid top face and
   not ice" covers every case Blocksmith actually has. Revisit only when slabs or stairs land.

7. **The thunder-strength lerp rate** is unsourced (A3). ±0.01/tick matching rain is
   architecturally likely — they share the code path — but nothing states it. Consequence of
   guessing wrong: a thunderstorm's darkening ramps at a slightly different speed than
   vanilla's, which nobody will ever notice.

8. **Snow layer light opacity** has no numeric source (C2). Recommending 0 is an inference
   from "blocks vision only at 8 layers", and at a one-layer cap it is also the choice that
   avoids relighting a column every time a flake lands — which is a performance argument as
   much as a fidelity one.
