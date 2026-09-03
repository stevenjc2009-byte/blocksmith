# v1.8.19 — Sound

This document does not implement anything. It is a sourcing and wiring plan
for `docs/ROADMAP.md`'s v1.8.19 entry, written by lane SOUND-PLAN, which owns
no `source/` file. Every finding below was checked against the tree as it
stood on 2026-09-03; other lanes are live on `source/world/*`, `source/net/*`,
`source/main.c`, `source/gfx/particles.*`, `source/scene/player.*`,
`source/app/*` and `docs/ROADMAP.md` during this pass, so none of those were
touched or assumed to still read the same by the time this lands.

## Provenance legend

**MEASURED** — a number read directly off a file, or arithmetic computed on
one, this session. **REASONED** — a conclusion drawn from measured facts.
**ASSUMED** — a judgement call with no citation behind it, flagged as such
rather than dressed up as either of the other two.

---

## §1 — Audit: what the SFX player has versus what it does

**MEASURED.** The slot table is `SfxSlot` in `source/audio/audio_sfx.h:47-84`,
backed by a fixed array in `source/audio/audio_sfx.c:11` (`static AudioSoundId
s_slots[SFX_SLOT_COUNT]`). `SFX_SLOT_COUNT = 9`. A slot only plays a sound if
`audioSfxRegister()` was called for it at boot **and** a call site somewhere
in gameplay code invokes it; both are required, and both fail silently if
missing (`audio_sfx.h:10-30`'s own header comment explains why the id is
captured rather than assumed).

| Slot | `.bsnd` on disk | Bytes | Registered at boot | Called from gameplay |
|---|---|---:|---|---|
| `SFX_BLOCK_BREAK` (0) | `block_break.bsnd` | 41,368 | `main.c:4295` | `interact.c:288` |
| `SFX_BLOCK_PLACE` (1) | `block_place.bsnd` | 34,380 | `main.c:4296` | `interact.c:610` |
| `SFX_FOOTSTEP` (2) | `footstep.bsnd` | 11,992 | `main.c:4297` | `audio_sfx.c:116` (distance accumulator) |
| `SFX_HURT` (3) | `hurt.bsnd` | 12,372 | **no** | **no** |
| `SFX_DEATH` (4) | `death.bsnd` | 24,280 | **no** | **no** |
| `SFX_EAT` (5) | `eat.bsnd` | 8,844 | **no** | **no** |
| `SFX_CRAFT` (6) | `craft.bsnd` | 9,728 | **no** | **no** |
| `SFX_SPLASH` (7) | `splash.bsnd` | 13,256 | **no** | **no** |
| `SFX_UI_TAP` (8) | `ui_tap.bsnd` | 3,992 | **no** | **no** |

**MEASURED**, `grep -n "audioSfxRegister\|audioLoad(" source/main.c` returns
exactly three hits, all at `main.c:4295-4297`, all for slots 0-2.
`grep -rn "audioSfxPlayAtBlock\|SFX_" source/scene source/world source/main.c`
(excluding `source/audio/` itself) returns hits only for `SFX_BLOCK_BREAK`
(`interact.c:288`) and `SFX_BLOCK_PLACE` (`interact.c:610`); `SFX_FOOTSTEP`'s
only caller is `audio_sfx.c:116` inside `audioFootstepsUpdate()`, which is
itself never called from `source/main.c` in this tree either (control: the
same grep for the unrelated string `"audio"` in `main.c` returns real hits,
so the empty result for the other six slots is not a broken grep — see
Verification below).

The gap is not "6 sounds are missing." **All 9 files already exist in
`romfs/sfx/`, 6 of them shipped by v1.8.17 lane SOUND-A specifically to fill
these slots** (`audio_sfx.h:40-46`'s own comment says so by name). The gap is
that nobody has written the `audioSfxRegister()` call for those six at boot,
nor the call site in `world/survival.h` (hurt/death/eat), `world/crafting.h`
(craft), `world/physics.h` (splash) or `scene/ui.c` (UI tap) that
`audio_sfx.h:52-81`'s own per-slot comments already specify exactly where to
put. **This is a zero-new-audio-bytes wiring task**, not a sourcing task — see
§8.

---

## §2 — Corrected budget arithmetic (fixes `docs/research/audio-sourcing.md:114`)

**MEASURED**, `ls -la romfs/sfx/` and `sha256sum`/byte-count of every file:

```
block_break.bsnd   41,368
block_place.bsnd   34,380
craft.bsnd          9,728
death.bsnd         24,280
eat.bsnd            8,844
footstep.bsnd      11,992
hurt.bsnd          12,372
splash.bsnd        13,256
ui_tap.bsnd         3,992
                  -------
                  160,212  bytes on disk, 9 files
```

Each `.bsnd` carries a 24-byte header (`tools/make_sounds.py`'s
`HEADER_BYTES`, cited by the existing research doc, not re-read by this
lane). Pool bytes actually spent = 160,212 − (9 × 24) = 160,212 − 216 =
**159,996 bytes.**

`AUDIO_POOL_BYTES_OLD3DS = 393216u` (`source/audio/audio.h:69`). Remaining =
393,216 − 159,996 = **233,220 bytes.**

This is not just my own arithmetic — `source/audio/audio_sfx_test.c:203-213`
asserts it directly as a running host-test check: `CHECK(total_pool ==
159996u)` and `CHECK(AUDIO_POOL_BYTES_OLD3DS - total_pool == 233220u)`, with
the file's own comment calling this "the number this whole task turns on."
My independent measurement and the shipped test agree exactly.

**This confirms the task's own stated figures (~159,996 spent / ~233,220
free) and finds no disagreement** — the number in the task brief is correct;
the number in the file is what's stale.

**The correction, to be applied to `docs/research/audio-sourcing.md:114`:**

```diff
- | Old 3DS | 393,216 B | 87,668 B | **305,548 B** (6.928 s) |
+ | Old 3DS | 393,216 B | 159,996 B | **233,220 B** (5.289 s) |
```

> **⚠ CORRECTION [2026-09-03, v1.8.17→v1.8.19] — this row is stale by
> 72,328 B.** It was correct when written (three shipped sounds, 87,668 B
> spent after header subtraction — `audio-sourcing.md:71-87`), but v1.8.17
> lane SOUND-A shipped six more `.bsnd` files since (`hurt`, `death`, `eat`,
> `craft`, `splash`, `ui_tap` — see `assets/sfx_src/ATTRIBUTION.md:62-80`),
> bringing spend to **159,996 B** and remaining Old-3DS headroom down to
> **233,220 B**. Re-measured directly from `romfs/sfx/` and cross-checked
> against `source/audio/audio_sfx_test.c:203-213`'s own `CHECK` on
> 2026-09-03 by lane SOUND-PLAN. The New-3DS row two lines below
> (`87,668 B` / `960,908 B`) is stale by the same 72,328 B and needs the same
> fix; not touched here since only line 114 was named in this lane's brief —
> flagged so it isn't missed.

*(This lane may edit only this one line of `audio-sourcing.md` per its
brief; the New-3DS row and the rest of that document's now-superseded Tier
tables in §2-§4 are for whoever owns that file to reconcile — see §5 below
for what still holds and what doesn't.)*

---

## §3 — Sound set, derived from what the game actually has

**MEASURED**, from a read-only survey of `source/world/registry.c` (the
43-row `kCoreDefs` block table), `source/world/mining.c`/`mining.h`,
`source/entity/animal.h`/`.c`, `source/world/furnace.c`, `world/physics.h`'s
`bodyWetUpdate()`, and `world/weather.h`. Full findings, not re-derived from
memory of Minecraft:

- **No "sound material" tag exists in the block registry.** The only
  classification bits are `REG_FLAG_SOLID/TRANSPARENT/LIQUID/LUMINOUS/
  FLAMMABLE` and a 3-bit shape (`registry.h:38-76`). Any break/place/footstep
  material grouping below is **REASONED** from block names and row comments,
  not read off a code enum — flagged because a future lane adding a real
  material tag would make this grouping obsolete in a good way.
- **No tool system exists.** `mining.h:24-36` states directly that `ItemId`
  is a typedef of `BlockId` and there is no separate item registry; bare
  hands are the only "tool," at a flat 1x speed multiplier
  (`mining.c:16-26`). Break time is `ceil(hardness / speed)`
  (`breakTicksRequired()`, `mining.c:28-45`), driven entirely by the
  broken block's own hardness — **there is no tool-tier sound distinction
  to design for**, because there is no tool tier.
- **4 animal kinds are implemented**, no more: pig, cow, chicken, sheep
  (`entity/animal.h:43-46`, ids 1-4). Ids 5-7 (zombie/skeleton/arrow) are
  reserved but have zero implementation anywhere in `source/`. **Zero audio
  hooks exist on any animal path today** — `animalHurt()` and the
  combat/flee/death logic (`animal.h:239-263`) call no audio function.
- **Furnace exists** (`world/furnace.c`), with ignite/burn-down/smelt-complete
  states (`furnaceTick()`, `furnace.c:129-171`), and **no audio hook of any
  kind**. Furnace sound is **not named in the ROADMAP's own v1.8.19 scope
  line** (quoted below) — kept out of MUST-HAVE on that basis, not an
  oversight.
- **Water** is confirmed: `bodyWetUpdate()` (`world/physics.h:426`) crosses
  `BODY_DRY` against `BODY_SURFACE`/`BODY_SUBMERGED` with hysteresis, and
  `SFX_SPLASH` already names this exact crossing as its trigger
  (`audio_sfx.h:71-74`) — unwired, per §1. No drowning system exists
  (`survival.h:78` names it as hypothetical future work only). No discrete
  "swim stroke" event exists; swimming is continuous physics, not a
  triggerable instant.
- **Weather** is `CLEAR`/`RAIN`/`SNOW` only — **no thunder or lightning
  exists anywhere in `source/`** (confirmed by grep; only `docs/` mentions
  it). Weather state changes on a per-cell roll (`weatherAt()`,
  `weather.h:243`) with no per-frame discrete "it started raining here"
  callback a one-shot sound could hang cleanly off — an ambient loop tied to
  `WeatherKind` is the natural shape, and ambient loops are exactly the
  category §3's budget work below still can't afford (see Tier 4 finding,
  carried forward from the existing research doc and re-confirmed at the
  corrected budget in §7).

**MEASURED, quoted verbatim**, `docs/ROADMAP.md`'s own v1.8.19 scope line:
*"A real audio system — footsteps that know what you are walking on, block
breaking and placing per material, animals, water, ambience. Effects sourced
under licences that permit any use, credited in the repository."* Furnace and
weather/thunder are not named; ambience is named but, per the budget below,
does not fit the Old-3DS pool this version — carried forward unchanged from
the prior research pass's finding, re-checked against the corrected §2
numbers, still true.

*(Note: the ROADMAP line says "credited in the repository." §6 below is
where this document flags that the credits requirement the owner stated
directly to this lane is stricter than that line — an in-game-visible
credit, not only a repo file, for anything CC-BY. Not a contradiction to
resolve here; just surfaced so whoever next touches `ROADMAP.md` sees it.)*

### Sound list, split MUST-HAVE / LATER

| Sound | Trigger | Target length | Existing slot? |
|---|---|---:|---|
| **MUST-HAVE — wiring only, 0 new bytes** | | | |
| Hurt | health-loss tick, `world/survival.h` | 0.28s (existing file) | `SFX_HURT`, unwired |
| Death | health reaches 0, `world/survival.h` | 0.55s (existing file) | `SFX_DEATH`, unwired |
| Eat | `survivalEat()` returns true | 0.20s (existing file) | `SFX_EAT`, unwired |
| Craft | `craftMake()` returns true | 0.22s (existing file) | `SFX_CRAFT`, unwired |
| Water enter/exit | `bodyWetUpdate()` crosses `BODY_DRY` | 0.30s (existing file) | `SFX_SPLASH`, unwired |
| UI tap | rising-edge tap, `scene/ui.c` | 0.09s (existing file) | `SFX_UI_TAP`, unwired |
| **MUST-HAVE — new sourcing, new slots** | | | |
| Footstep — dirt/grass | `audioFootstepsUpdate()`, block under feet = dirt/grass family | 0.25s | **no** — needs a new slot or a material-keyed lookup, see §4 |
| Footstep — stone | same, stone family | 0.25s | **no** |
| Footstep — sand | same, sand family | 0.25s | **no** |
| Break — dirt/grass | `breakComplete()`, `interact.c:237`, block = dirt/grass family | 0.40s | **no** |
| Break — wood | same, wood family | 0.45s | **no** |
| Break — sand | same, sand family | 0.40s | **no** |
| Place — stone | placement, `interact.c:610`, block = stone family | 0.40s | **no** |
| Place — dirt/grass | same, dirt/grass family | 0.35s | **no** |
| Place — sand | same, sand family | 0.35s | **no** |
| Animal — pig | `animalHurt()`, `ENT_KIND_PIG` | 0.35s | **no** |
| Animal — cow | `animalHurt()`, `ENT_KIND_COW` | 0.35s | **no** |
| Animal — chicken | `animalHurt()`, `ENT_KIND_CHICKEN` | 0.30s | **no** |
| Animal — sheep | `animalHurt()`, `ENT_KIND_SHEEP` | 0.35s | **no** |
| **LATER — real gaps, not oversights** | | | |
| Footstep/place — glass | rare interaction (walking on / placing a pane) | — | cut for byte cost, same call the prior research doc made |
| Break — glass | `breakComplete()`, glass block | 0.45s | sourced (§5), CC-BY — deferred with the credits-screen prerequisite (§6), not for budget reasons |
| Player — hurt (real recording upgrade) | replaces the existing synth `hurt.bsnd` | 0.55s | sourced (§5, carried forward), CC-BY — same §6 gate |
| Water enter/exit (real recording upgrade) | replaces the existing synth `splash.bsnd` | 0.55s+0.50s | sourced (§5, carried forward), CC0 but doesn't fit this version's margin (§7) |
| Door/chest open | no door or chest block exists in the registry today | — | genuinely not implementable yet, not a sourcing gap |
| Furnace ignite/burn/complete | not named in ROADMAP's v1.8.19 line | — | out of this version's scope |
| Weather — rain/snow ambience, cave drone | no discrete trigger; would be a resident loop | — | doesn't fit Old-3DS pool, carried forward from prior research, re-confirmed (§7) |

---

## §4 — The slot table doesn't have room for this shape of sound list, and that's a design question for the coding lane

**REASONED**, flagged rather than decided — this lane owns no `source/`
file. `SfxSlot` (`audio_sfx.h:47-84`) is a flat enum, one slot per exact
sound, currently 9 entries. The MUST-HAVE new-sourcing list above needs 13
more *distinct* sounds (3 footsteps + 3 breaks + 3 places + 4 animals) that
each depend on which block or animal kind triggered them — something no
existing slot or call site expresses. Two shapes are available to whoever
picks this up:

- **A — grow the enum linearly.** `SFX_SLOT_COUNT` goes from 9 to 22 (or
  more, later). Simplest change, but every future per-material or per-animal
  sound repeats it, and every call site needs its own `if`/`switch` on block
  or animal kind to pick the right slot.
- **B — a material/kind-keyed lookup.** A small array indexed by (a coarse
  material tag derived from `BlockId`, once one exists per §3's own finding
  that none does yet) or `AnimalKind`, each entry an `AudioSoundId` with a
  sane fallback (untagged material → today's single generic break/place/
  footstep, matching current behavior exactly rather than regressing it).
  More code once, scales without enum growth after.

Not decided here — **HIS CALL / for the coding lane**, noted so it isn't
discovered mid-implementation as a surprise.

---

## §5 — Sourced candidates, full provenance

Licence bar applied: CC0 or CC-BY only, attribution shown to the licence's
own words and quoted directly by this lane where marked
**verified-this-session**; nothing non-commercial-only, share-alike, or
requiring per-title purchase; nothing from Minecraft or Nintendo in any form.

### New this session (verified-this-session, downloaded to scratchpad)

Genuinely new sourcing work — animal sounds have no prior candidate anywhere
in this repo's research. All four fetched and licence-quoted directly from
the sound's own Freesound page, not an aggregator, this session. A preview
MP3 (not the full-quality original `.wav`, which Freesound gates behind a
login this lane does not have and will not create) was downloaded for each
into the scratchpad, and its SHA-256 recorded, so the exact bytes checked
against the licence claim are reproducible.

| Sound | Source page | Author | Licence (quoted) | Source format/length | Preview downloaded | SHA-256 (preview file) |
|---|---|---|---|---|---|---|
| Animal — pig | https://freesound.org/people/Jofae/sounds/352698/ | Jofae | CC0 — *"You can copy, modify, distribute and perform the sound, even for commercial purposes, all without the need of asking permission"* | WAV, 44,100 Hz, 16-bit mono, 0:08.079, 695.9 KB | `soundplan_preview_pig.mp3`, 186,409 B | `f212c4b3f7604ca2c522262535b16070ef51a22615809c267a1144b9ef476ef2` |
| Animal — cow | https://freesound.org/people/JarredGibb/sounds/233127/ | JarredGibb | CC0, same text | WAV, 96,000 Hz, 24-bit mono, 0:03.009, 874.0 KB | `soundplan_preview_cow.mp3`, 71,760 B | `922083352f5bc34b92d25bada6d4550aa3aaabab8596452d8776df4f6383970c` |
| Animal — chicken | https://freesound.org/people/Breviceps/sounds/456803/ | Breviceps | CC0, same text | WAV, 16,000 Hz, 16-bit mono, 0:14.953, 467.3 KB | `soundplan_preview_chicken.mp3`, 199,656 B | `4d4bbffa4297df2c4a61afc9acb348567e59a3b32ac819a1494a3897097cfff0` |
| Animal — sheep | https://freesound.org/people/Gitanki/sounds/172712/ | Gitanki | CC0, same text | WAV, 48,000 Hz, 32-bit stereo, 0:03.317, 1.2 MB | `soundplan_preview_sheep.mp3`, 62,880 B | `9d314c3b22d6021dce3077e9ba5d1cfe8fac36d987c9cbbad02391e4ae3ccbb2` |

All four are single, short, unmodified recordings (not multi-animal field
recordings, which would need trimming to isolate one animal — deliberately
avoided; e.g. "Farmyard Sounds — Pig, Sheep, Donkey, Ducks, Chickens" by
nebulousflynn turned up in the pig search and was **not** picked, exactly
for that reason).

### Carried forward from `docs/research/audio-sourcing.md`, re-verified this session

The prior sourcing pass (v1.8.17, not this lane) already licence-traced these
against original pages. Re-fetched and re-quoted independently this session
rather than trusted on the prior document's word:

| Sound | Source page | Author | Licence (quoted, this session) | Notes |
|---|---|---|---|---|
| Footstep — dirt/grass, stone, sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | Fantozzi (recorded), submitted by qubodup | CC0, page states "CC0" directly. 12-file pack, FLAC+OGG, 16-bit/44,100 Hz | Per-file mapping inside the 12-file pack still not chosen — same gap the prior document flagged, unresolved by this pass either, since resolving it means downloading the archive |
| Break/place — dirt/grass, sand, stone; break — wood | https://kenney.nl/assets/impact-sounds | Kenney (Kenney Vleugels) | CC0, page states "Creative Commons CC0" with a link to the CC0 1.0 deed. Same pack the three already-shipped sounds come from | Same pack-level-not-file-level gap as above |
| Water — enter/splash | https://freesound.org/people/qubodup/sounds/210428/ | qubodup | CC0 — *"You can copy, modify, distribute and perform the sound, even for commercial purposes, all without the need of asking permission to the author."* Page notes it was originally CC-BY and changed to CC0 on 2024-11-23 | LATER, per §3/§7 — doesn't fit this version's margin |
| Player — eat | https://opengameart.org/content/apple-bite | AntumDeluge | CC0 — page states "Creative Commons Zero (CC0)" | Already superseded — `eat.bsnd` ships as a synth today, no download needed |
| Break — glass | https://freesound.org/people/Ruben_Uitenweerde/sounds/486166/ | Ruben_Uitenweerde | **CC BY 3.0** — *"You are free to share (to copy, distribute and transmit) and to remix (to adapt and modify) as long as you credit the author of the sound."* WAV, 48,000 Hz, 24-bit mono, 0:02.147, 302.7 KB | Downloaded (preview): `soundplan_preview_glass.mp3`, 51,504 B, SHA-256 `4a1a4a164b1ee60d50f290407fe45bd7371ea6fa4468a7358b87548a4f7b1ad6`. **LATER — gated on the credits-screen prerequisite, §6**, not on budget |

### Not re-verified this session (carried forward on the prior document's word only)

- Player — hurt, https://freesound.org/people/MAJ061785/sounds/85553/, CC BY 3.0 —
  **already superseded in practice**: `hurt.bsnd` ships as a synth today
  (§1), so this candidate is only relevant as a future quality upgrade, and
  is gated on §6 exactly like the glass break candidate if it's ever used.
  Not fetched again this session; treat the prior document's quote as
  unconfirmed by this lane until someone re-checks it.
- Door/chest open, https://freesound.org/people/spookymodem/sounds/202092/,
  CC0 — not fetched again. Moot regardless: no door or chest block exists in
  the registry (§3), so there is no trigger to hang it on yet.
- Water — exit, https://freesound.org/people/speedygonzo/sounds/235725/,
  CC0 — not fetched again. LATER per §7, so left unconfirmed rather than
  spending a fetch on a candidate that doesn't fit this version's budget
  regardless of its licence.

### Rejected

- "Open Treasure Chest 8 Bit.wav" (Mrthenoronha, Freesound) —
  Attribution-NonCommercial. **Rejected outright**, non-commercial-only is
  never acceptable.
- "Farmyard Sounds — Pig, Sheep, Donkey, Ducks, Chickens" (nebulousflynn,
  Freesound) — turned up in the pig search; not picked because it is a
  multi-animal field recording, not a clean single-animal source (see
  above).
- ZapSplat's lava/cow assets — ZapSplat's terms require crediting ZapSplat
  itself rather than a standard CC deed and a free account to download;
  not pursued, same reasoning the prior research pass already applied to
  ZapSplat.

---

## §6 — Credits screen: does not exist today, and is a real prerequisite

**MEASURED.** `grep -rli "credits" source/ docs/ README.md` returns two
files: `source/net/hosttest/bsnet_transport_hosttest.c:273` (an unrelated
comment — *"credits an RTT sample off..."*, nothing to do with attribution)
and `docs/research/audio.md`. **No in-game credits or about screen exists.**
`source/scene/` has `title.c`/`title_nav.c` and `pausemenu.c` — no
`credits.c`, no `about.c`, nothing adjacent. (Control: the same grep style
for `"audio"` in `source/main.c` returns real hits, confirming the empty
credits result isn't a broken search.)

`docs/research/audio.md:246-252` (a different research pass, not owned by
this lane) argues a `CREDITS.md` in the repository satisfies CC-BY's
attribution requirement on its own, since nothing in the licence text
requires runtime display, and quotes Freesound's own forum guidance to that
effect. That may be correct as a floor on the legal requirement. **It is not
what was asked of this lane.** The owner's instruction to this lane states
plainly that credit "must appear where a USER of the game can see it, not
just in a repo file" — a stricter bar than the legal minimum, and this
document follows the stricter bar rather than resolving the disagreement
between the two.

**Consequence:** both CC-BY candidates on record (glass break,
Ruben_Uitenweerde; player hurt, MAJ061785) are blocked on an in-game credits
screen existing, not on their licence terms — their licence terms are
already satisfied by CC-BY itself; it's the owner's stricter bar that isn't
met yet. Recommendation: keep v1.8.19's MUST-HAVE list **entirely CC0** (the
table in §3 already is — no CC-BY sound is in the new-sourcing MUST-HAVE
rows), and treat "build a minimal in-game credits screen" as its own small
prerequisite feature for whoever picks up the two CC-BY LATER items, rather
than a blocker on this version. Not decided further here — which screen it
hangs off (`title.c`'s menu, `pausemenu.c`) is **HIS CALL / for the coding
lane**.

---

## §7 — Does the MUST-HAVE new-sourcing list fit the Old-3DS pool?

**REASONED**, target durations are this lane's own proposal (not dictated by
any source recording), chosen short enough to read as a stinger rather than
a clip, and consistent with the existing shipped sounds' own lengths (e.g.
`hurt.bsnd` today is 12,372 B ÷ 44,100 B/s ≈ 0.28s). Bytes = duration ×
44,100 B/s (16-bit mono 22,050 Hz, the format §1/§2's existing pool already
fixes, not a new choice).

| # | Sound | Duration | Bytes | Running total |
|---|---|---:|---:|---:|
| 1 | Footstep — dirt/grass | 0.25s | 11,025 | 11,025 |
| 2 | Footstep — stone | 0.25s | 11,025 | 22,050 |
| 3 | Footstep — sand | 0.25s | 11,025 | 33,075 |
| 4 | Break — dirt/grass | 0.40s | 17,640 | 50,715 |
| 5 | Break — wood | 0.45s | 19,845 | 70,560 |
| 6 | Break — sand | 0.40s | 17,640 | 88,200 |
| 7 | Place — stone | 0.40s | 17,640 | 105,840 |
| 8 | Place — dirt/grass | 0.35s | 15,435 | 121,275 |
| 9 | Place — sand | 0.35s | 15,435 | 136,710 |
| 10 | Animal — pig | 0.35s | 15,435 | 152,145 |
| 11 | Animal — cow | 0.35s | 15,435 | 167,580 |
| 12 | Animal — chicken | 0.30s | 13,230 | 180,810 |
| 13 | Animal — sheep | 0.35s | 15,435 | 196,245 |

**Total new bytes: 196,245.** Old-3DS remaining budget (§2, corrected):
**233,220 B.** **Fits, with 36,975 B (15.9%) to spare.** New-3DS remaining
budget is 960,908 − 72,328 (the same stale-figure correction applied, §2) =
888,580 B; this list uses 22.1% of it, comfortable.

**This margin is why glass break (19,845 B) and the water/hurt upgrades are
LATER rather than squeezed in** — they're gated on the credits screen (§6)
regardless of budget for the CC-BY pair, and the CC0 water-recording upgrade
alone (24,255 + 22,050 = 46,305 B) would eat more than the entire remaining
margin above, so it stays deferred on budget grounds independent of §6.

---

## §8 — Recommended build order

This lane cannot touch any of the files below; this is a sequencing
recommendation for whoever does.

- **P0 — wire the six already-shipped, already-synthesized slots
  (§1).** Zero new audio bytes, zero new sourcing, zero licence work — the
  `audioSfxRegister()` calls belong in `source/main.c` next to the existing
  three (`main.c:4295-4297`); the six call sites are individually specified
  by `audio_sfx.h:52-81`'s own comments, in `world/survival.h` (hurt, death,
  eat), `world/crafting.h`/`scene/ui.c` (craft), `world/physics.h`
  (splash), and `scene/ui.c` (UI tap). This alone gives the roadmap's
  "water" bullet a real sound (via the existing splash synth) and is the
  cheapest, lowest-risk win available — recommended first regardless of
  what else in this plan gets picked up.
- **P1 — resolve §4's slot-architecture question** before writing any new
  per-material or per-animal call site, so the 13 MUST-HAVE new sounds in §3
  aren't wired against a shape that has to be redone.
- **P2 — download and licence-finalize** the sourcing rows in §5 that feed
  the MUST-HAVE list: the Fantozzi footstep pack and Kenney Impact Sounds
  pack (both already CC0-verified at the pack level, per-file mapping still
  open — a five-minute task once download is authorised) and the four
  animal previews already pulled into the scratchpad this session (full
  originals need a Freesound login this lane doesn't have — a human
  download, not an agent one, per the standing rule against entering
  credentials).
- **P3 — run `tools/make_sounds.py`** against the finalized set; it prints
  the exact byte cost of the whole manifest, which is the number to check
  against §7's arithmetic before merging, not this document's estimate.
- **P4 — the 13 new call sites** (per-material footstep/break/place,
  per-animal hurt) once P1's architecture is settled.
- **LATER, separately sequenced:** a minimal in-game credits screen (§6),
  then the two CC-BY upgrades it unblocks; the CC0 water-recording upgrade,
  budget-gated per §7; furnace and weather ambience, both out of this
  version's named scope per §3.

---

## §9 — What could not be verified

- **The exact filenames inside the Kenney Impact Sounds zip and the
  Fantozzi footsteps archive** that map to each per-material row in §3/§7.
  The pack/author-level licence is solid (CC0, re-verified this session);
  which specific file is which requires downloading the archive, which this
  pass does not do.
- **Player — hurt (MAJ061785) and Water — exit (speedygonzo)'s licence
  pages were not re-fetched this session** — carried forward on the prior
  research document's word only, flagged in §5, and both are LATER items so
  nothing in this plan depends on them being re-confirmed yet.
- **The full-quality original `.wav` files for the four animal sounds** —
  only the lossy preview MP3 was downloaded and hashed (§5); Freesound gates
  the original behind a login, which this lane does not have and will not
  create per the standing rule against entering credentials on the user's
  behalf. Downloading the originals is a human task or an authenticated
  step for whoever owns `tools/make_sounds.py`.
- **Whether `tools/make_sounds.py`'s pipeline needs a trim step added**, or
  whether trimming happens by hand before a source file enters
  `assets/sfx_src/`. Same open question the prior research pass already
  flagged; not resolved by this pass either — it belongs to whoever owns
  that script.

---

## Verification note (this document's own claims)

Before any grep-for-absence claim above was trusted, a literal `( exit 5 )`
control was run first and printed `5`, confirming the shell's exit-code
reporting in this environment before it was relied on anywhere else. Every
"empty grep" claim in §1 and §6 above was paired with a control grep for a
string known to exist in the same file, to rule out a broken search
reading as evidence of absence.
