# Audio sourcing research — v1.8.17

Scope: this document is a plan and a shortlist only. No audio files were downloaded, no
`source/` file was touched, and no `make` was run during this pass. It answers three
questions: what the 3DS audio hardware actually requires, what the real memory ceiling
is, and where to get sound effects that are genuinely free to use.

Provenance is labelled on every claim: **measured** (read off a real console tonight, or
computed arithmetic on a measured number), **read-from-header** (taken directly from a
libctru header or bundled devkitPro example source file, quoted with file:line),
**sourced-with-URL** (a web page fetched and read this session), **reasoned** (a
conclusion drawn from the above, not itself measured or read), or **assumed** (stated
without independent verification, flagged as such).

---

## §1 — What the 3DS audio hardware requires

**read-from-header.** Blocksmith's own source tree has no audio subsystem at all today.
`grep -ri "ndsp|DSP|audio|sound"` across `source/` returns 17 hits, and every one of
them is a false positive — English prose using the words "sound" (as in "a sound
argument") or "unsound", never the DSP service. There is no `ndspInit`, no `ndsp/`
include, no `.cdc` reference, nothing. Confirmed by reading the matched lines directly,
not just the file list (`source/app/gputest.c:848`, `source/world/chunk.h:110`,
`source/world/cavewalk_test.c:548-658`, `source/world/water.c:204`, etc. — all comment
prose). This matches `docs/ROADMAP.md:371-375`, which schedules "a real audio system" for
v1.8.17 and has not been touched yet.

**read-from-header.** The audio hardware is driven through libctru's `ndsp` module,
headers at `C:\devkitPro\libctru\include\3ds\ndsp\ndsp.h` and `...\ndsp\channel.h`:

- `ndsp.h:9` — `#define NDSP_SAMPLE_RATE (SYSCLOCK_SOC / 512.0)`. This is the DSP
  coprocessor's own fixed internal mixing rate (works out to ~32,728 Hz), not a rate the
  game must encode its samples at.
- `ndsp.h:95` — `Result ndspInit(void);`. It returns a `Result`, i.e. it is designed to
  be checked and can fail. (Consequence for what happens on failure: §1 continued below.)
- `channel.h:10-15` — three sample encodings: `NDSP_ENCODING_PCM8` (0), `PCM16` (1),
  `ADPCM` (2, "DSPADPCM (GameCube format)").
- `channel.h:25-33` — format flags combine channel count and encoding:
  `NDSP_FORMAT_MONO_PCM8/PCM16/ADPCM` and `NDSP_FORMAT_STEREO_PCM8/PCM16`. There is no
  stereo ADPCM flag defined — ADPCM is mono-only in this header.
  `NDSP_CHANNELS(n)` masks to 2 bits, but only 1 and 2 have defined format constants.
- `channel.h:56` — `void ndspChnReset(int id);` and every channel function's doc comment
  says `id (0..23)` — 24 hardware mixer channels.
  `channel.h:137` — `void ndspChnSetRate(int id, float rate);` — **each channel has its
  own arbitrary sample rate**, resampled by the DSP's own interpolator
  (`channel.h:41-46`: polyphase / linear / none) up or down to the fixed internal rate.
  So source material does **not** need to be pre-resampled to one canonical rate; the
  hardware does that per-channel.
- `ndsp.h:57-75` — the wave buffer struct (`ndspWaveBuf`) holds a union of
  `data_pcm8` / `data_pcm16` / `data_adpcm` / `data_vaddr` plus `nsamples`. The header
  itself says nothing about *which* memory region the pointer must point into.

**measured (read the actual bundled example source, not guessed).** That memory
requirement is answered by devkitPro's own shipped example,
`C:\devkitPro\examples\3ds\audio\streaming\source\main.c`:
- Line 44: `u32 *audioBuffer = (u32*)linearAlloc(SAMPLESPERBUF*BYTESPERSAMPLE*2);` — the
  sample buffer is allocated with `linearAlloc`, not `malloc`.
- Line 25: `DSP_FlushDataCache(audioBuffer,size);` — called after writing samples and
  before the buffer is handed to a channel.
- Line 123: `linearFree(audioBuffer);` on teardown.

**reasoned**, from the above: the DSP coprocessor reads wave buffer sample data by
physical address (it is a separate chip from the ARM11, not something that shares the
CPU's virtual-memory view), so the buffer has to be physically contiguous and
cache-coherent — which is exactly what `linearAlloc`/`linearFree`/`DSP_FlushDataCache`
exist to guarantee and `malloc` does not. **ndsp sample data must live in the linear
heap, not the application heap.** This is the single most important constraint for
sizing a sound set on this console, and it is why §2 below uses the linear-heap reading
and not the much larger application-heap one.

### The DSP firmware requirement — a mandatory error-handling case, not a hard blocker

**sourced-with-URL**, cross-checked across `C:\devkitPro\examples\3ds\audio\README.md:3`
(bundled locally, read directly) and community documentation (3dbrew/GBAtemp/GameBrew):

> "Homebrew requires a copy of the DSP firmware to be present at sdmc:/3ds/dspfirm.cdc."

Nintendo's DSP firmware binary is not distributable — it is copyrighted, dumped by each
user from their own console (Luma3DS Rosalina menu → "Miscellaneous options" → "Dump DSP
firmware", or historically the DSP1 homebrew title). Blocksmith's CIA cannot legally
ship this file, and the devkitPro README says so.

**On real hardware**, if `dspfirm.cdc` is absent, `ndspInit()` fails — consistent with it
returning a checkable `Result` (`ndsp.h:95`). Community sources describe the practical
symptom as "no sound", not a hard crash, provided the caller actually checks the
`Result` and skips the rest of audio setup rather than assuming success and calling into
an unready DSP service. **On Citra/Azahar** (HLE DSP), a bundled bug report
(lovebrew/lovepotion#102) confirms a *zero-byte* placeholder file at that path is enough
to satisfy the check — the emulator's HLE path doesn't parse the firmware, it only
checks the file exists.

**Consequence for Blocksmith, stated plainly since this is exactly the kind of thing
that becomes a shipping blocker if mishandled:** this is not something to work around —
every other 3DS homebrew game with sound has the same requirement, and the fix is
entirely in how the code is written, not in anything that needs sourcing. `ndspInit()`'s
`Result` must be checked at boot; on failure the game must set an `audioAvailable = false`
flag and skip every subsequent `ndsp*` call for the rest of the session, rather than
assume init succeeded. Handled that way, a player without a dumped `dspfirm.cdc` gets a
silent game, not a crash. This belongs in whatever `audio_init()` gets written for
v1.8.17 — flagged here, not built here, per this task's file-ownership boundary.

---

## §2 — The memory ceiling

**measured**, exactly as given — a live New 3DS reading taken from a running v1.8.6
build:

| Reading | Bytes |
|---|---|
| Linear heap, total | 67,108,864 |
| Linear heap, free after boot at render distance 2 | 18,285,568 |
| Chunk mesh pool alone, claimed from that linear heap | 46,948,352 |
| Application heap, total | 60,977,152 |
| Application heap, in use at boot | 253,032 |

Since §1 established ndsp sample data must be **linear**, the application heap's 60.7 MB
(60,724,120 B free) is irrelevant to audio sizing — that memory is not reachable by the
DSP coprocessor. The number that matters is the **18,285,568 B of free linear heap.**

**reasoned — an arithmetic check that surfaced an inconsistency worth flagging rather
than silently resolving:** `67,108,864 − 46,948,352 = 20,160,512`. The measured free
figure is 18,285,568, a further 1,874,944 B short of that — plausible, since GPU command
buffers, framebuffers and other `linearAlloc` users also compete for this heap and
weren't itemised in the reading handed to this pass. But `docs/ROADMAP.md:45-46` states
the mesh pool costs **9,199,616 bytes at radius 3** and **46,948,352 at radius 5** — the
same 46,948,352 figure given here as "at render distance 2." Both citations can't be
correct about which radius produced that pool size. This document uses the reading
exactly as supplied rather than guessing which label is right; see §8.

**assumed, not measured — Old 3DS.** No Old 3DS linear-heap reading was taken tonight.
`docs/ROADMAP.md:46` gives the Old 3DS linear heap as 33,554,432 B total with a
9,199,616 B pool at its own radius-3 ceiling. By the same subtraction logic and assuming
similar ~1.87 MB of other linear overhead, Old 3DS free linear would be roughly
22.5 MB — *more* headroom than the New 3DS reading above, because its smaller render
distance more than offsets its smaller heap. This is arithmetic on an unmeasured
assumption, not a hardware reading, and is flagged as such in §8.

**The real ceiling for audio, stated plainly:** roughly **18.3 MB of free linear memory
on a New 3DS at the measured boot state**, shared with everything else that wants
physically-contiguous memory — not a dedicated audio budget, and one that shrinks as
render distance or other linear-heap consumers grow. Audio needs to be a small, bounded
slice of that, not treated as if 18 MB were available to spend.

---

## §3 — Recommended format, sample rate, and reasoning

**reasoned.** Recommendation: **16-bit mono PCM (`NDSP_FORMAT_MONO_PCM16`), 16,000 Hz**
for the whole v1.8.17 set, decoded once at load and kept resident in linear memory (no
ADPCM, no streaming) — for these reasons:

- Per the task's own worked example, 16-bit mono at 22,050 Hz costs 44,100 B/s. At
  16,000 Hz that's `16,000 × 2 = 32,000 B/s` — a 27% reduction with no perceptible loss
  for short, percussive game SFX (footsteps, breaks, UI blips, mob calls) played through
  a handheld's small stereo speakers or headphones. An 8 kHz Nyquist ceiling is well
  above what any of these sounds need.
- `channel.h:137` (`ndspChnSetRate`) means the hardware resamples per-channel anyway, so
  16,000 Hz source material is not a compatibility risk — it plays back correctly
  regardless of the DSP's fixed internal ~32,728 Hz mixing rate.
- ADPCM would shrink the footprint further (roughly 4:1 over PCM16), but it is mono-only
  (`channel.h:25-33` has no stereo ADPCM flag), needs per-sound coefficients set via
  `ndspChnSetAdpcmCoefs` (`channel.h:171`), and needs an ADPCM encode step added to the
  build pipeline that doesn't exist yet. Given the total set below comes in at under 10%
  of measured headroom even as plain PCM16, that complexity buys nothing this version
  and is not recommended for v1.8.17. Worth revisiting only if a future version's
  render-distance increase eats further into the linear heap and a later ambience/mob
  set no longer fits comfortably.
- Mono, not stereo: half the bytes for effects that have no directional stereo image to
  begin with (impacts, UI, mono field recordings as sourced below). `NDSP_FORMAT_STEREO_PCM16`
  stays available for a later pass if positional audio becomes a design goal.

**Total byte cost of the shortlist in §4, computed at this format:** see the table's
running total — **1,762,016 B (≈1.68 MiB)**, against 18,285,568 B measured free linear
(§2). That's **≈9.6% of measured headroom**, all sounds resident simultaneously,
worst-case (no streaming, nothing ever unloaded). Even against the more conservative
20,160,512 B pre-overhead figure from the same reading, it's still under 9%. There is
substantial margin for the eating/hurt/mob variety a full survival game eventually wants,
without touching ADPCM or streaming.

---

## §4 — Sound shortlist

Every entry below was checked on its own asset page this session (not the site's
general licensing blurb) unless marked "pack-level only" — see the note under the table.
Durations marked *(trim)* are the game's target clip length; the source recording is
longer and would be cut down during import, not shipped whole. Bytes are computed at the
§3 recommendation, 16-bit mono PCM @ 16,000 Hz = 32,000 B/s.

| # | Sound | Source URL | Licence | Attribution required | Duration used | Bytes |
|---|---|---|---|---|---|---|
| 1 | Block break — stone | https://kenney.nl/assets/impact-sounds | CC0 (pack-level, 130 files) | No | 0.40s (trim) | 12,800 |
| 2 | Block break — wood | https://kenney.nl/assets/impact-sounds | CC0 (pack-level, 130 files) | No | 0.40s (trim) | 12,800 |
| 3 | Block break — dirt/grass | https://freesound.org/people/dr19/sounds/353907/ | CC0 | No | 0.40s (trim from 9.05s) | 12,800 |
| 4 | Block break — sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | CC0 | No | 0.40s (trim; author notes sand sounds like grass) | 12,800 |
| 5 | Block break — glass | https://freesound.org/people/Ruben_Uitenweerde/sounds/486166/ | CC BY 3.0 | Yes — "Glass breaking" by Ruben_Uitenweerde (Freesound.org, CC BY 3.0) | 0.40s (trim from 2.15s) | 12,800 |
| 6 | Block break — leaves | https://freesound.org/people/giddster/sounds/437356/ | CC0 | No | 0.40s (trim from 7.96s) | 12,800 |
| 7 | Block place — stone | https://kenney.nl/assets/impact-sounds | CC0 (pack-level) | No | 0.30s (trim) | 9,600 |
| 8 | Block place — wood | https://kenney.nl/assets/impact-sounds | CC0 (pack-level) | No | 0.30s (trim) | 9,600 |
| 9 | Block place — dirt/grass | https://freesound.org/people/dr19/sounds/353907/ | CC0 | No | 0.30s (softer trim, same source) | 9,600 |
| 10 | Block place — sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | CC0 | No | 0.30s (trim) | 9,600 |
| 11 | Block place — glass | *(gap — see §7)* | — | — | — | — |
| 12 | Block place — leaves | https://freesound.org/people/giddster/sounds/437356/ | CC0 | No | 0.30s (trim) | 9,600 |
| 13 | Footstep — stone | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | CC0 | No | 0.25s (trim) | 8,000 |
| 14 | Footstep — wood | https://kenney.nl/assets/rpg-audio | CC0 (pack-level, tagged "footstep") | No | 0.25s (trim) | 8,000 |
| 15 | Footstep — dirt/grass | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | CC0 | No | 0.25s (trim) | 8,000 |
| 16 | Footstep — sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | CC0 | No | 0.25s (trim) | 8,000 |
| 17 | Footstep — glass | *(gap — see §7)* | — | — | — | — |
| 18 | Footstep — leaves | https://freesound.org/people/giddster/sounds/437356/ | CC0 | No | 0.25s (trim, lighter than the break clip) | 8,000 |
| 19 | Water — enter/splash (also the splash-particle match) | https://freesound.org/people/qubodup/sounds/210428/ | CC0 | No | 1.00s (trim from 2.45s) | 32,000 |
| 20 | Water — exit | https://freesound.org/people/speedygonzo/sounds/235725/ | CC0 | No | 1.00s (trim from 35.42s) | 32,000 |
| 21 | Water — swimming/underwater loop | https://freesound.org/s/366159/ | CC0 (public domain) | No (crediting DCSFX optional) | 10.00s (trim from 4:00 loop) | 320,000 |
| 22 | Player — hurt | https://freesound.org/people/MAJ061785/sounds/85553/ | CC BY 3.0 | Yes — "male pain grunt" by MAJ061785 (Freesound.org, CC BY 3.0) | 1.00s (trim from 1.80s) | 32,000 |
| 23 | Player — fall damage | https://freesound.org/people/leonelmail/sounds/504626/ | CC0 | No | 1.63s (near-full, already short) | 52,256 |
| 24 | Player — eat | https://opengameart.org/content/apple-bite | CC0 | No | 1.00s (already a short extracted clip) | 32,000 |
| 25 | Ambient — cave/underground drone | https://freesound.org/people/Kinoton/sounds/421826/ | CC0 | No | 10.00s (trim from 4:16.85) | 320,000 |
| 26 | Ambient — outdoor wind | https://freesound.org/people/felix.blume/sounds/217506/ | CC0 | No | 10.00s (trim from 3:32.32) | 320,000 |
| 27 | UI — menu move | https://kenney.nl/assets/interface-sounds | CC0 (pack-level, 100 files) | No | 0.15s (trim) | 4,800 |
| 28 | UI — menu select | https://kenney.nl/assets/interface-sounds | CC0 (pack-level) | No | 0.15s (trim) | 4,800 |
| 29 | UI — inventory open | https://kenney.nl/assets/interface-sounds | CC0 (pack-level) | No | 0.20s (trim) | 6,400 |
| 30 | UI — inventory close | https://kenney.nl/assets/interface-sounds | CC0 (pack-level) | No | 0.20s (trim) | 6,400 |
| 31 | Mob — pig | https://freesound.org/people/felix.blume/sounds/158746/ | CC0 | No | 1.20s (trim from 2:02.81) | 38,400 |
| 32 | Mob — cow | https://freesound.org/people/Bird_man/sounds/275154/ | CC0 | No | 1.20s (trim from 1.65s) | 38,400 |
| 33 | Mob — chicken | https://freesound.org/people/Breviceps/sounds/456803/ | CC0 | No | 1.20s (trim from 14.95s; source is already 16kHz mono) | 38,400 |
| 34 | Mob — sheep | https://freesound.org/people/zachrau/sounds/383144/ | CC0 | No | 1.20s (trim from 45.26s) | 38,400 |
| 35 | Mob — zombie | https://freesound.org/people/robert18productions/sounds/634699/ | CC0 | No | 1.20s (trim one groan from 1:08.71 compilation) | 38,400 |
| 36 | Mob — skeleton | https://freesound.org/people/spookymodem/sounds/202102/ | CC0 | No | 1.20s (trim from 5.46s "Rattling Bones") | 38,400 |
| 37 | Furnace crackle | https://freesound.org/people/soundofsong/sounds/650574/ | CC0 | No | 5.03s (full file — already built as a loop) | 160,960 |
| 38 | Craft / success chime | https://freesound.org/people/grunz/sounds/109662/ | CC BY 3.0 | Yes — "success.wav" by grunz (Freesound.org, CC BY 3.0) | 1.30s (full file, resampled from original 22,050Hz) | 41,600 |

**Running total: 1,762,016 B (≈1.68 MiB), 36 of 38 rows filled** (row 11 and row 17 are
open gaps, tracked in §7, and are not counted since nothing was sourced for them).

**Pack-level note (rows 1, 2, 7, 8, 14, 27–30):** the Kenney packs (Impact Sounds,
Interface Sounds, RPG Audio) were verified at the **pack** level — CC0 licence, file
count, and download URL confirmed by fetching each asset page directly. The individual
filename for "the wood impact" or "the menu-select click" inside each zip was **not**
itemised, because doing that means downloading the archive, which this pass explicitly
does not do. Picking the exact file per row is a five-minute task once download is
authorised — flagged here rather than guessed.

---

## §5 — Attribution block

If this shortlist is adopted, the following would go in the repository (README or a
dedicated `CREDITS.md`), covering every entry that legally requires it — CC0 entries are
included too since the task said "he will credit" even where not legally required:

```
Sound effects

CC0 / Public domain (no attribution required, credited anyway):
- Kenney (kenney.nl) — Impact Sounds, Interface Sounds, RPG Audio packs
- dr19, "Shovel_dirt.wav" (Freesound.org)
- Fantozzi, "Fantozzi's Footsteps (Grass/Sand & Stone)" via qubodup (OpenGameArt.org,
  originally Freesound.org)
- giddster, "Rustling leaves" (Freesound.org)
- qubodup, "Water Splash 1" (Freesound.org)
- speedygonzo, "Water splash.wav" (Freesound.org)
- DCSFX, "Underwater [Loop] AMB.wav" (Freesound.org)
- leonelmail, "BODY FALL - V HVY - DIRT" (Freesound.org)
- AntumDeluge, "Apple Bite" (OpenGameArt.org, extracted from Freesound.org)
- Kinoton, "Dark Cave Drone" (Freesound.org)
- felix.blume, "Wind blowing in a field in Texas, USA" and "A pig grunting, grumbling
  and falling asleep (France, Limousin)" (Freesound.org)
- Bird_man, "Moo.wav" (Freesound.org)
- Breviceps, "Chicken clucking" (Freesound.org)
- zachrau, "Sheep bleating" (Freesound.org)
- robert18productions, "Zombie Sound Effects (Raw).wav" (Freesound.org)
- spookymodem, "Rattling Bones.wav" (Freesound.org)
- soundofsong, "fire crackling loop.wav" (Freesound.org)

CC BY 3.0 (attribution required):
- "Glass breaking" by Ruben_Uitenweerde (Freesound.org), CC BY 3.0
- "male pain grunt" by MAJ061785 (Freesound.org), CC BY 3.0
- "success.wav" by grunz (Freesound.org), CC BY 3.0

Full licence text: https://creativecommons.org/publicdomain/zero/1.0/ and
https://creativecommons.org/licenses/by/3.0/
```

---

## §6 — What the game would need built to play any of this

**reasoned**, from §1's API survey — none of this exists in `source/` today:

1. **`audio_init()` / `audio_shutdown()`** — calls `ndspInit()`, checks its `Result`,
   sets a global `audioAvailable` flag on failure per §1, calls `ndspExit()` on shutdown.
2. **A sample-loading path** — decode each shipped source file (likely OGG or WAV in
   `romfs`, since the shortlist above arrives as WAV/FLAC/OGG from various sources) down
   to raw 16-bit mono PCM @ 16,000 Hz at build or first-load time, into a `linearAlloc`'d
   buffer, with `DSP_FlushDataCache` called once after decode. A decode step is needed
   somewhere in the pipeline — devkitPro ships `libopus`/`libvorbisidec` examples
   (`C:\devkitPro\examples\3ds\audio\ogg-vorbis-decoding\`,
   `...\opus-decoding\`) that are the obvious starting point rather than writing a
   decoder from scratch.
3. **A channel/voice allocator** — 24 hardware channels (`channel.h`) is not infinite;
   the game needs a small pool manager that picks a free channel per triggered sound,
   sets format/rate/mix via `ndspChnSetFormat`/`ndspChnSetRate`/`ndspChnSetMix`, and
   queues the wave buffer with `ndspChnWaveBufAdd`.
4. **A trigger layer wired into existing systems** — block break/place needs to read the
   material at the broken/placed block (the block ID already carries material identity
   per `docs/ROADMAP.md`'s biome-block work) and pick the matching sample; footsteps need
   a periodic trigger keyed to player movement and the block underfoot; water enter/exit
   needs hooking into `world/water.c`'s existing state transitions; the splash-particle
   match needs to sit next to whatever already spawns the v1.8.10 splash particles so the
   two fire together.
5. **A simple mixer/volume policy** — master volume, and probably a settings-menu volume
   slider, since v1.8.19's UI rework is still two versions out and this needs *a* home
   before then.
6. **Looping ambience playback** — cave drone and wind need `ndspWaveBuf.looping = true`
   (`ndsp.h:70`) and a trigger for when to start/stop them (underground vs. surface,
   presumably keyed off the existing light/biome detection already used for v1.8.16's
   monster spawn rules).
7. **Build-side asset conversion tooling** — whatever produces the trimmed, resampled,
   16kHz mono PCM files that ship in `romfs` from the longer source recordings in §4.
   Nothing in `tools/` does this today; it would be a new script.

None of this was written or touched in this pass — file ownership for this task is
`docs/research/audio-sourcing.md` only.

---

## §7 — Gaps

- **Block place — glass** (table row 11). Shattering is the natural sound for glass
  *breaking*, but placing a glass block doesn't shatter anything, and no dedicated
  "gentle glass clink/set down" CC0 or CC-BY sound turned up in this pass's searches.
  Real gap — recommend either sourcing a dedicated soft-glass-clink sample in a later
  pass, or deliberately reusing a quieter trim of the same Ruben_Uitenweerde recording
  (with attribution) as a placeholder, which is a design call for steve, not this pass.
- **Footstep — glass** (table row 17). Same reasoning — walking on glass blocks isn't a
  common Minecraft-family mechanic to begin with, so this may not be a real requirement;
  flagged rather than assumed away.
- **Footstep — wood**, and every **UI/Impact-pack row**, are pack-level CC0 verifications
  only (§4's note) — the exact file within each Kenney zip was not picked out.
- **Sand**, throughout, reuses the Fantozzi grass/sand pack on the original author's own
  statement that "sand sounds like grass too" rather than a dedicated sand recording.
  Acceptable as a first pass; a true sand-specific foley recording would be a nicer fit
  later.

No category was left completely empty — every one of the eight groups the task listed
(breaks/places, footsteps, water, player, ambient, UI, mobs, furnace/craft) has at least
a partial, licence-clean source. The two true gaps above are both single missing rows,
not missing categories.

---

## §8 — What could not be verified

- **The render-distance/mesh-pool number mismatch flagged in §2.** The reading supplied
  for this pass states 46,948,352 B claimed by the mesh pool "at render distance 2," but
  `docs/ROADMAP.md:45-46` attributes that exact figure to **radius 5**, with radius 3
  costing 9,199,616 B. Both cannot be describing the same boot. This document used the
  reading exactly as supplied without resolving the discrepancy — that resolution needs
  whoever took tonight's reading, not a guess from this pass.
- **No Old 3DS linear-heap reading exists.** §2's Old 3DS figure (~22.5 MB free) is
  arithmetic on ROADMAP.md's own paper numbers, not a console reading — labelled
  "assumed" there, not "measured."
- **Kenney pack contents were not itemised file-by-file** (§4's pack-level note) —
  confirmed the licence and file count on each pack's own page, not which of the 130/100/
  50 individual files best fits "wood impact" vs. "stone impact," etc.
- **`ndspInit()`'s exact real-hardware failure mode** — every source agrees it fails
  without `dspfirm.cdc`, but none of the sources fetched this session include the actual
  `Result` error code it returns, because libctru's `ndsp` implementation is a
  precompiled `.a` in this devkitPro install with no bundled source (`C:\devkitPro\libctru\source`
  does not exist locally) — only the header and community reports were available, not
  the implementation.
- **No sound in this shortlist was downloaded, decoded, or listened to.** Every duration,
  format and sample-rate figure in §4 comes from the asset page's own metadata, not from
  opening the file. A pass that does download would be the point to confirm none of
  these turn out to be mislabelled, silent, or clipped.
