# Audio sourcing research — v1.8.17 "Sound"

Scope: this document is a sourcing plan and licence audit only. No audio file was
downloaded, no `assets/sfx_src/*.ogg` was added, no `source/` file was touched, and
`tools/make_sounds.py` was read but not edited during this pass — that file belongs to
another lane. It answers three questions: what audio system already exists (§1), how
much room is left in it (§2), and which specific, licence-clean sounds should fill that
room (§3–§5).

Provenance is labelled on every claim: **measured** (a number read directly off a file,
or computed arithmetic on one), **read-from-source** (quoted from a file in this repo,
file:line), **verified-this-session** (a licence page fetched and read during this pass,
independent of anything an earlier document claimed), and **reasoned** (a conclusion
drawn from the above).

---

## §0 — Correction: the previous version of this document was wrong

The version of this file this pass replaced opened with: *"Blocksmith's own source tree
has no audio subsystem at all today"* and built its entire memory analysis, format
recommendation and shortlist on that premise. **That premise is false as of this
session.** A working ndsp audio backend is checked in, sounds already ship, and the
format, sample rate and pool-sizing decisions that earlier draft treated as open
questions have already been made and implemented. Likely explanation: a separate
research pass (`docs/research/audio.md`, not owned by this lane) was written before
the audio backend existed and correctly found nothing; the backend was built afterward;
this file was never updated to match. It is updated now. Every §1 and §2 claim below was
re-verified against the actual files this session, not carried over from either prior
draft.

---

## §1 — What already exists (verified this session)

**read-from-source.** The audio subsystem lives in `source/audio/`: `audio.c` (364
lines), `audio.h` (207 lines), `audio_ndsp.c` (177 lines, the real libctru backend),
`audio_bsnd.c`/`audio_bsnd.h` (the sound-container format), `audio_mixer.c`/`.h` (voice
allocation), `audio_pan.c`/`.h` (positional panning), plus `audio_bsnd_test.c` (311
lines) and `audio_mixer_test.c` (692 lines). 2,564 lines total, not zero.

- `source/audio/audio.c:126` calls `mixerInit(&s_mixer, backend, AUDIO_VOICE_COUNT)`,
  which drives `ndspInit()` through `audio_ndsp.c`'s real backend
  (`extern const AudioBackend* audioNdspBackend(void);` at `audio.c:22`) on `__3DS__`
  builds.
- `source/audio/audio.h:9-17` states the contract: if `ndspInit()` fails (no dumped
  `dspfirm.cdc`), `audioInit()` returns `false` and every play call becomes a safe no-op
  — never a crash, never a per-frame check the caller has to remember. This is exercised
  by test, not just described (`audio.h:19-20`).
- `source/main.c:3851-3853` calls `audioLoad("romfs:/sfx/block_break.bsnd")`,
  `block_place.bsnd`, and `footstep.bsnd` at boot. `source/main.c:4884-4885` calls
  `audioSetListener(...)` and `audioUpdate()` once a frame.
- **Gap found this session, worth flagging explicitly:** boot-loading and the per-frame
  listener update are wired in, but no gameplay call site actually triggers a sound yet.
  `grep -rn "audioPlayAt(\|audioPlay(\|audioPlayEx(" source/ --include=*.c` outside
  `source/audio/` itself returns **zero hits**. Block break/place, footsteps, and
  everything else in §3 below have no trigger call in the tree today. That wiring is
  gameplay-code work (`source/main.c` and/or the world/scene files other lanes currently
  own) — out of scope for this document and this lane, noted here only so it isn't
  mistaken for already being done.

**read-from-source — the shipped format.** `tools/make_sounds.py:1-24` documents the
`BSND` container: the 3DS DSP plays raw PCM only, so `ffmpeg` decodes each source file
once on a desktop machine into the exact format the console loads with a bulk read and
zero per-sound decode work. `tools/make_sounds.py:44` (`TARGET_RATE = 22050`) and `:49`
(`TARGET_CHANNELS = 1`) fix the format as **16-bit mono PCM at 22,050 Hz** — this is
already decided, not a choice this document gets to make. `tools/make_sounds.py:58-60`
gives the reasoning: 22,050 Hz is a clean 2:1 decimation from the 44.1 kHz sources are
mastered at, and it is well above what the 3DS's speakers reproduce.

**measured — the three shipped sounds.**

```
romfs/sfx/block_break.bsnd   41,368 bytes
romfs/sfx/block_place.bsnd   34,380 bytes
romfs/sfx/footstep.bsnd      11,992 bytes
                             -------
                             87,740 bytes on disk
```

Each `.bsnd` carries a 24-byte header (`tools/make_sounds.py:96`, `HEADER_BYTES = 24`),
so pool bytes actually spent = 87,740 − (3 × 24) = **87,668 bytes**. This is not a figure
computed for this document — it is asserted directly by the test suite:
`source/audio/audio_mixer_test.c:653-654`, `CHECK(87668u < AUDIO_POOL_BYTES_OLD3DS)` and
`CHECK(87668u < AUDIO_POOL_BYTES_NEW3DS)`. `assets/sfx_src/ATTRIBUTION.md:14-20`
confirms provenance: all three are Kenney "Impact Sounds" (CC0), byte-verified by SHA-256
against the pack zip (`ATTRIBUTION.md:51-56`).

**read-from-source — the pool ceiling.** `source/audio/audio.h:68-69`:

```c
#define AUDIO_POOL_BYTES_NEW3DS 1048576u
#define AUDIO_POOL_BYTES_OLD3DS  393216u
```

`source/audio/audio.c:127` picks between them with `hwIsNew3ds() ? ... : ...`, and
`audio.c:129-134` takes the pool from the **linear** heap (`poolAlloc`), after `ndspInit`
and before any sound loads — `audio.h:22-28` explains why linear and not the application
heap: the DSP reads sample data by physical address, and linear is "the same pool the GPU
draws vertices out of," i.e. it is in direct competition with render distance
(`audio.h:46-67` gives the full measured-linear-heap context: 67,108,864 B total,
~18,285,568 B free on a New 3DS at boot, chunk mesh arenas as the other big consumer).
This is already documented in `audio.h` itself and was not re-derived here.

**Arithmetic check, done independently this session, not copied from a comment:**
393,216 ÷ (22,050 × 2) = 393,216 ÷ 44,100 = **8.9147… seconds** of 22,050 Hz 16-bit mono
audio, total, for the entire Old 3DS pool. That confirms the task's stated figure. The
three shipped sounds (87,668 B) already spend 87,668 ÷ 44,100 = **1.988 s** of that.

**Remaining pool room for new sounds in v1.8.17:**

| Console | Pool ceiling | Already spent | Remaining |
|---|---:|---:|---:|
| Old 3DS | 393,216 B | 87,668 B | **305,548 B** (6.928 s) |
| New 3DS | 1,048,576 B | 87,668 B | **960,908 B** (21.789 s) |

**The Old 3DS figure is the binding constraint for everything below** — the task's own
framing, confirmed by measurement, not assumption.

---

## §2 — Ranked sound list and Old-3DS fit

Ranked by gameplay value (how often it's heard × how much it's missed when absent), not
by how easy each was to source. Durations are *target trimmed lengths for the game*, not
the source recording's full length — `tools/make_sounds.py`'s pipeline decodes and
resamples but does not itself trim, so trimming happens either during import prep or
needs a small addition to that step; that decision belongs to whoever owns
`tools/make_sounds.py`, flagged here rather than assumed. Bytes = duration × 22,050 × 2,
i.e. 44,100 B/s — the format already fixed by §1, not a recommendation.

### Tier 1 — Per-material break / place / footstep (the core verb of the game)

| # | Sound | Status | Duration | Bytes | Running total |
|---|---|---|---:|---:|---:|
| — | Footstep — wood | **existing**, no new cost | 0.27s* | 0 | 0 |
| — | Block break — stone | **existing**, no new cost | 0.94s* | 0 | 0 |
| — | Block place — wood | **existing**, no new cost | 0.78s* | 0 | 0 |
| 1 | Footstep — dirt/grass | new | 0.25s | 11,025 | 11,025 |
| 2 | Footstep — stone | new | 0.25s | 11,025 | 22,050 |
| 3 | Footstep — sand | new | 0.25s | 11,025 | 33,075 |
| 4 | Block break — dirt/grass | new | 0.40s | 17,640 | 50,715 |
| 5 | Block break — wood | new | 0.45s | 19,845 | 70,560 |
| 6 | Block break — sand | new | 0.40s | 17,640 | 88,200 |
| 7 | Block break — glass | new | 0.45s | 19,845 | 108,045 |
| 8 | Block place — stone | new | 0.40s | 17,640 | 125,685 |
| 9 | Block place — dirt/grass | new | 0.35s | 15,435 | 141,120 |
| 10 | Block place — sand | new | 0.35s | 15,435 | 156,555 |

*Existing durations computed from shipped pool bytes ÷ 44,100 B/s, for reference only —
already paid for.

### Tier 2 — Player feedback and UI

| # | Sound | Duration | Bytes | Running total |
|---|---|---:|---:|---:|
| 11 | Player — hurt | 0.55s | 24,255 | 180,810 |
| 12 | Player — eat | 0.18s† | 7,938 | 188,748 |
| 13 | UI — click/move | 0.12s | 5,292 | 194,040 |
| 14 | UI — confirm/select | 0.15s | 6,615 | 200,655 |

†Source clip is itself only ~0.18s (see §4) — trimming further isn't possible, this row
uses the source near-full.

### Tier 3 — World interaction

| # | Sound | Duration | Bytes | Running total |
|---|---|---:|---:|---:|
| 15 | Door/chest — open clunk | 0.40s | 17,640 | 218,295 |
| 16 | Water — enter/splash | 0.55s | 24,255 | 242,550 |
| 17 | Water — exit | 0.50s | 22,050 | 264,600 |

**Total new bytes through Tier 3: 264,600 B.**
**Old 3DS remaining budget: 305,548 B.**
**Fits, with 40,948 B (10.4%) to spare** — a real margin, not a shave-to-the-byte fit.
New 3DS remaining budget is 960,908 B; Tier 1–3 uses 27.5% of it, comfortable room to
grow later.

### Tier 4 — Environmental ambience (does NOT fit Old 3DS — see §3)

| # | Sound | Duration | Bytes |
|---|---|---:|---:|
| 18 | Swimming stroke (one-shot, retriggered) | 0.40s | 17,640 |
| 19 | Lava — sizzle tick (one-shot, retriggered) | 0.50s | 22,050 |
| 20 | Ambient — cave drone (short seamless loop) | 2.50s | 110,250 |

Tier 4 total: 149,940 B. Added to the Tier 1–3 total, that's 414,540 B — **114,992 B over
the Old 3DS's entire remaining budget of 305,548 B**, before even considering that a
usable ambient drone loop realistically wants to be longer than 2.5s to avoid sounding
like a metronome, which would only widen the gap. There is no trim of Tier 4 that makes
it fit next to Tier 1–3 on Old 3DS. It fits trivially on New 3DS (415K of 961K available,
43%), which is exactly the trap: it would be easy to ship Tier 4 New-3DS-only and call
v1.8.17 done.

### Cut from this version — real gaps, not oversights

- **Footstep — glass.** Walking on top of a placed glass block is a legal but rare
  interaction. Dropped to save 11,025 B rather than forced in at the bottom of the
  ranking.
- **Block place — glass.** Same reasoning the previous draft of this document reached
  independently (§7 of the prior version): placing glass doesn't shatter anything, so
  the natural "glass" sound (breaking) doesn't fit the place action, and no dedicated
  "soft glass clink" CC0/CC-BY recording was found this session either. See §4 for the
  synthesis recommendation instead of leaving this empty.

---

## §3 — Should Tier 4 ship New-3DS-only? No — recommend deferring all of it instead

`docs/ROADMAP.md:7-9` states the project's own standing rule in these words: **"Both
consoles, always. Every version must work on an Old 3DS and a New 3DS. Where the two
genuinely differ ... it is said explicitly. Where they do not, it is not mentioned,
because 'works on both' is the baseline, not a feature."** Render distance is the one
place that rule already accepts a console split (`source/scene/render_dist.h:167-168`,
`RENDER_DIST_MAX_OLD 3` vs `RENDER_DIST_MAX_NEW 5`), and that split is a hardware
constraint stated openly in the render-distance system itself, not something bolted on
per-feature.

Ambience is not in that category. Swimming, lava hazard cues and a cave drone are not
edge-case content an Old 3DS player would expect to be missing — they're regular
environmental feedback, and a player who never sees a change log would simply experience
"the game plays quieter on Old 3DS," which is a worse outcome than "the game doesn't have
this yet on either console." Recommend: **cut Tier 4 from v1.8.17 entirely, on both
consoles**, and revisit it in a later version, either when a render-distance or
linear-heap change (owned by the lanes currently in `chunk_render.c`/`worldgen*.c`)
frees more Old 3DS headroom, or by redesigning cave ambience as a much shorter one-shot
"stinger" triggered occasionally rather than a continuously resident loop, which is a
design change outside this document's scope.

---

## §4 — Sourcing decisions, one row per sound

Every licence below was fetched and read on the sound's own original page this session —
not an aggregator's summary, and not carried over from the prior draft of this document
without re-checking. Durations are the **source recording's own length**; the trimmed
in-game length is the Tier table above.

### Source (real recordings — verified this session)

| Sound | Original page | Uploader | Licence | Source duration | Attribution needed |
|---|---|---|---|---:|---|
| Footstep — dirt/grass, stone, sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | Original recordist **Fantozzi**; submitted to OpenGameArt by **qubodup** | CC0 | pack of 12 single steps (per-file length not itemised — see gap below) | No |
| Block break — glass | https://freesound.org/people/Ruben_Uitenweerde/sounds/486166/ | Ruben_Uitenweerde | **CC BY 3.0** | 2.147s | **Yes** — "Glass breaking" by Ruben_Uitenweerde (Freesound.org, CC BY 3.0) |
| Player — hurt | https://freesound.org/people/MAJ061785/sounds/85553/ | MAJ061785 | **CC BY 3.0** | 1.800s | **Yes** — "male pain grunt" by MAJ061785 (Freesound.org, CC BY 3.0) |
| Player — eat | https://opengameart.org/content/apple-bite | AntumDeluge | CC0 | ~0.18s (est. from 16.1 KB file at 44,100 Hz mono; page does not state duration directly) | No |
| Door/chest — open clunk | https://freesound.org/people/spookymodem/sounds/202092/ | spookymodem | CC0 | 4.936s | No |
| Water — enter/splash | https://freesound.org/people/qubodup/sounds/210428/ | qubodup | CC0 | 2.445s | No |
| Water — exit | https://freesound.org/people/speedygonzo/sounds/235725/ | speedygonzo | CC0 | 35.424s (a long field recording; a single splash moment within it is the trim target) | No |
| Block break/place — dirt/grass, sand, stone; break — wood | https://kenney.nl/assets/impact-sounds | Kenney (Kenney Vleugels) | CC0 | pack of 130 files (per-file length not itemised — see gap below) | No |

**Rejected candidates, and why** (recorded so the same dead end isn't re-walked later):

- "Open Treasure Chest 8 Bit.wav" (Mrthenoronha, Freesound) — Attribution-NonCommercial.
  **Rejected**: non-commercial-only is on the not-acceptable list.
- Audionautics "Lava loop.wav" (Freesound) — CC BY, tempting for Tier 4's lava sizzle,
  but Tier 4 is deferred per §3, so no licence decision was needed on it this pass.
- ZapSplat's lava sizzle — ZapSplat's terms require a free account and attribution
  *to ZapSplat*, not a standard CC licence; provenance to an original recordist is
  unclear from the aggregator page. Not pursued, consistent with the rule that an
  aggregator's licence claim is not proof.

### Pack-level gap, stated plainly

Kenney's own asset pages (`impact-sounds`, confirmed CC0 again this session by re-fetching
the page — see §1's provenance note that this is the *same pack* the three shipped sounds
already come from) do not list individual filenames; that only becomes visible by
downloading the zip, which this pass does not do. The same is true of the Fantozzi
footsteps pack (OpenGameArt states "12 single steps," not which is which). **This means
rows 4, 6, 8, and 9 of the Tier 1 table (dirt/grass and sand break/place, wood break) and
rows 1–3 (dirt/grass/stone/sand footsteps) are licence-verified at the pack level but not
yet matched to a specific filename inside the archive.** That match is a five-minute task
once download is authorised, exactly as the prior draft of this document also found for
its own pack-level rows — flagged rather than guessed at either time.

### Synthesise instead (recommend building, not sourcing)

| Sound | Why synthesis beats sourcing here |
|---|---|
| UI — click/move, confirm/select | A single short tone or two-tone blip is trivial to generate and needs zero licence research, zero attribution bookkeeping, and hits an exact byte target (5,292 B / 6,615 B) instead of whatever a downloaded sample happens to be. This project already has working precedent for exactly this: the Home Menu banner chime is synthesised from sine partials and a noise burst by `tools/make_banner_audio.py` rather than sourced (per `README.md:96-98`, cited in the prior draft of this document and independently confirmed against `tools/make_banner_audio.py`'s own header comment this session). UI blips are the same category of sound. |
| Block place — glass (the gap noted in §2) | No natural recording fits "set a pane of glass down gently" — the only glass-adjacent CC0/CC-BY sound found either session is the *breaking* sound, which is wrong for this action. A synthesised soft high-pitched clink is a better fit than force-reusing the break sound quietly, and costs nothing to licence. |

Implementing either of these means a new small synthesis script (not a change to
`tools/make_sounds.py`'s decode pipeline, which is for pre-existing source files) or an
extension someone with ownership of that file adds — not built in this pass, per the
file-ownership boundary for this task.

---

## §5 — What could not be established with confidence

- **Exact filenames inside the Kenney Impact Sounds zip and the Fantozzi footsteps
  archive**, for the rows noted in §4's pack-level gap. The pack/author-level licence is
  solid (CC0, verified on the pack's own page this session); the specific file is not
  chosen yet because choosing it means downloading, which this pass does not do.
- **Player — eat's exact duration.** OpenGameArt's page for "Apple Bite" states file
  size (16.1 KB) and format (44,100 Hz mono) but not a duration figure; 0.18s is this
  document's arithmetic from those two numbers, not a value read directly off the page.
- **A specific splash moment inside the 35-second "Water splash.wav" exit recording.**
  The licence (CC0) and uploader (speedygonzo) are solid; which few hundred milliseconds
  of the 35s file to trim is a listening decision that needs the actual audio, not
  something to guess from a licence page.
- **Whether `tools/make_sounds.py`'s pipeline needs a trim step added**, or whether
  trimming happens by hand-editing the source file before it enters `assets/sfx_src/`.
  Both are workable; the choice belongs to whoever owns that script, not this document.
