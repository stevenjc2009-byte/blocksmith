#!/usr/bin/env python3
"""Synthesises source WAVs for the six new v1.8.17 gameplay cues (lane SOUND-A), plus the
nine per-material footstep/break/place cues added for v1.8.19 ("footsteps that know what
you are walking on, block breaking and placing per material" — STONE/WOOD/DIRT/GRASS).
The v1.8.19 additions are Sounds 7-15 below, after the original six; everything in this
docstring about no downloads, no numpy, and byte-identical re-runs applies to them equally.

Nothing here is sampled, recorded, downloaded or lifted from another game — the same rule
tools/make_banner_audio.py and tools/make_atlas.py already follow, and for the same reason:
"CC0 or CC-BY only" (assets/sfx_src/ATTRIBUTION.md) is satisfied trivially by a sound that
was never anyone else's in the first place, at the cost of writing the waveform by hand.
Every sample below comes from sine partials, one-pole filtered noise and hand-shaped
envelopes, using nothing but the Python standard library (math, struct, wave). No numpy,
no Pillow, no sound library — same constraint make_banner_audio.py works under, so this
file borrows its noise generator and its report() shape verbatim rather than reinventing
either.

These are SOURCE files, not what the game loads: each WAV lands in assets/sfx_src/ next to
the three Kenney recordings already there, and tools/make_sounds.py's MANIFEST is extended
to decode them through the same ffmpeg pass every other sound gets — so the shipped .bsnd
is byte-for-byte whatever that pipeline produces, not whatever this script's own PCM was,
even though the two happen to agree here because the source is already 22050 Hz mono s16.

Deterministic by construction: every noise burst comes from the integer LCG below, seeded
per sound from a fixed constant, never from Python's `random`. A re-run byte-diffs identical.

Usage:
    python tools/make_sfx_synth.py            # writes all fifteen WAVs into assets/sfx_src/
"""

from __future__ import annotations

import math
import struct
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "assets" / "sfx_src"

RATE = 22050          # matches tools/make_sounds.py's TARGET_RATE exactly — see file comment
SAMPLE_BYTES = 2       # 16-bit signed PCM; ffmpeg re-encodes this on the way through anyway
PEAK_DBFS = -3.0        # headroom, same margin make_banner_audio.py leaves
END_FADE_S = 0.006      # linear ramp to a guaranteed zero last sample — no click at the tail


def lcg_noise(seed: int):
    """Deterministic white noise in [-1, 1). Numerical Recipes LCG constants.

    Copied from tools/make_banner_audio.py rather than re-derived: the point of an LCG
    here is only that the interpreter's own random module can never move the output out
    from under a byte-identical re-run, and any fixed-constant LCG does that equally well.
    """
    state = seed & 0xFFFFFFFF
    while True:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state / 2147483648.0 - 1.0


def lowpass(samples: list[float], cutoff_hz: float) -> list[float]:
    """One-pole leaky-integrator lowpass. No scipy on this machine, and a single pole is
    all six of these sounds need — it is the same "shape the noise" tool a synth patch
    would reach for first, just spelled out by hand."""
    alpha = 1.0 - math.exp(-2.0 * math.pi * cutoff_hz / RATE)
    y = 0.0
    out = []
    for x in samples:
        y += alpha * (x - y)
        out.append(y)
    return out


def highpass(samples: list[float], cutoff_hz: float) -> list[float]:
    """Complement of lowpass: input minus its own lowpassed self."""
    lp = lowpass(samples, cutoff_hz)
    return [x - y for x, y in zip(samples, lp)]


def env_exp_decay(n: int, tau_s: float, attack_s: float = 0.0) -> list[float]:
    """A soft-attack, exponential-decay envelope of length n samples."""
    out = []
    attack_n = max(1, int(round(attack_s * RATE)))
    for i in range(n):
        t = i / RATE
        if i < attack_n:
            out.append(i / attack_n)
        else:
            out.append(math.exp(-(t - attack_s) / tau_s))
    return out


def mix_into(buf: list[float], start: int, part: list[float]) -> None:
    for i, v in enumerate(part):
        n = start + i
        if 0 <= n < len(buf):
            buf[n] += v


# ── Sound 1: player hurt ─────────────────────────────────────────────────────────────
#
# A short "oof": a descending sine (a grunt's pitch drop) layered under a burst of
# lowpassed noise (the breath/impact transient). 0.28s — long enough to read as a
# reaction, short enough that a run of consecutive fall-damage ticks does not smear.
def make_hurt() -> list[float]:
    dur_s = 0.28
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x48555254)  # 'HURT'

    env = env_exp_decay(n, tau_s=0.09, attack_s=0.004)
    for i in range(n):
        t = i / RATE
        freq = 260.0 - 140.0 * (t / dur_s)     # 260 Hz falling to 120 Hz
        buf[i] += 0.55 * env[i] * math.sin(2.0 * math.pi * freq * t)

    burst_n = int(round(0.10 * RATE))
    raw = [next(noise) for _ in range(burst_n)]
    shaped = lowpass(raw, 900.0)
    benv = env_exp_decay(burst_n, tau_s=0.035, attack_s=0.002)
    mix_into(buf, 0, [0.6 * s * e for s, e in zip(shaped, benv)])

    return buf


# ── Sound 2: player death ────────────────────────────────────────────────────────────
#
# The same falling-pitch idea as hurt, stretched and deepened, plus a second sweep an
# octave down for weight, and a longer noise tail so it reads as final rather than as a
# second hurt grunt. 0.55s, matching the research doc's Tier-2 duration target.
def make_death() -> list[float]:
    dur_s = 0.55
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x44454154)  # 'DEAT'

    env = env_exp_decay(n, tau_s=0.24, attack_s=0.006)
    for i in range(n):
        t = i / RATE
        f1 = 220.0 - 160.0 * (t / dur_s)   # 220 -> 60 Hz
        f2 = 110.0 - 80.0 * (t / dur_s)    # an octave under f1, same fall
        tone = 0.5 * math.sin(2.0 * math.pi * f1 * t) + 0.35 * math.sin(2.0 * math.pi * f2 * t)
        buf[i] += env[i] * tone

    tail_n = n
    raw = [next(noise) for _ in range(tail_n)]
    shaped = lowpass(raw, 500.0)
    tenv = env_exp_decay(tail_n, tau_s=0.18, attack_s=0.01)
    mix_into(buf, 0, [0.30 * s * e for s, e in zip(shaped, tenv)])

    return buf


# ── Sound 3: eat ──────────────────────────────────────────────────────────────────────
#
# Two short bandpassed noise clicks back to back — "bite, bite" — rather than one longer
# burst, because a single crunch reads as a footstep at this length and two short ones is
# what makes it read as chewing. 0.20s total.
def make_eat() -> list[float]:
    dur_s = 0.20
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x45415431)  # 'EAT1'

    bite_len_s = 0.07
    bite_n = int(round(bite_len_s * RATE))
    starts = [0, int(round(0.10 * RATE))]
    for k, start in enumerate(starts):
        raw = [next(noise) for _ in range(bite_n)]
        band = highpass(lowpass(raw, 3500.0), 700.0)
        benv = env_exp_decay(bite_n, tau_s=0.018, attack_s=0.002)
        gain = 0.85 if k == 0 else 0.65   # second bite a little quieter than the first
        mix_into(buf, start, [gain * s * e for s, e in zip(band, benv)])

    return buf


# ── Sound 4: craft success ───────────────────────────────────────────────────────────
#
# A bright two-note "ding" — root then a fifth above, the same additive-plus-transient
# idea make_banner_audio.py's chime uses, just two notes instead of four and a fraction
# of the length, so it reads as a UI confirm rather than a fanfare. 0.22s.
def make_craft() -> list[float]:
    dur_s = 0.22
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x43524654)  # 'CRFT'

    root_hz = 660.0
    notes = ((0, 0.00), (7, 0.09))   # root, then +7 semitones (a fifth), 90ms later
    harmonics = ((1, 1.00), (2, 0.30))

    for degree, onset_s in notes:
        start = int(round(onset_s * RATE))
        length_n = n - start
        freq = root_hz * (2.0 ** (degree / 12.0))
        env = env_exp_decay(length_n, tau_s=0.05, attack_s=0.003)
        tone = []
        for i in range(length_n):
            t = i / RATE
            s = 0.0
            for mult, weight in harmonics:
                s += weight * math.sin(2.0 * math.pi * freq * mult * t)
            tone.append(0.35 * env[i] * s)
        mix_into(buf, start, tone)

        # A short noise tick at each onset, the same "strike" role the banner chime's
        # transient plays, so the note reads as struck rather than switched on.
        tick_n = int(round(0.012 * RATE))
        raw = [next(noise) for _ in range(tick_n)]
        tenv = env_exp_decay(tick_n, tau_s=0.006, attack_s=0.001)
        mix_into(buf, start, [0.25 * s * e for s, e in zip(raw, tenv)])

    return buf


# ── Sound 5: water splash ────────────────────────────────────────────────────────────
#
# Filtered noise with a lowpass cutoff that SWEEPS downward across the burst — a bright
# "plosh" at the strike settling into a duller "gloop" — which is what a one-pole filter
# with a moving cutoff gives for free without needing a second filter stage. 0.30s.
def make_splash() -> list[float]:
    dur_s = 0.30
    n = int(round(dur_s * RATE))
    noise = lcg_noise(0x53504c53)  # 'SPLS'
    raw = [next(noise) for _ in range(n)]

    # A time-varying lowpass: cutoff falls from 4000 Hz to 400 Hz across the burst. Not a
    # single lowpass() call because that function's alpha is fixed for the whole buffer —
    # this loop is the same one-pole update with alpha recomputed every sample instead.
    out = []
    y = 0.0
    for i in range(n):
        t = i / RATE
        cutoff = 4000.0 - (4000.0 - 400.0) * (t / dur_s)
        alpha = 1.0 - math.exp(-2.0 * math.pi * cutoff / RATE)
        y += alpha * (raw[i] - y)
        out.append(y)

    env = env_exp_decay(n, tau_s=0.11, attack_s=0.003)
    buf = [0.9 * s * e for s, e in zip(out, env)]
    return buf


# ── Sound 6: UI tap ───────────────────────────────────────────────────────────────────
#
# The shortest of the six by a wide margin — a single high tick, noise plus one sine
# partial, both gone inside 90 ms. Meant to be heard, not noticed, on every inventory tap.
def make_ui_tap() -> list[float]:
    dur_s = 0.09
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x55495431)  # 'UIT1'

    env = env_exp_decay(n, tau_s=0.018, attack_s=0.001)
    for i in range(n):
        t = i / RATE
        buf[i] += 0.4 * env[i] * math.sin(2.0 * math.pi * 1800.0 * t)

    raw = [next(noise) for _ in range(n)]
    shaped = highpass(raw, 2500.0)
    mix_into(buf, 0, [0.35 * s * e for s, e in zip(shaped, env)])

    return buf


# ── v1.8.19 lane: per-material footstep / break / place ─────────────────────────────
#
# Four materials — STONE, WOOD, DIRT, GRASS — three actions each. Three of the twelve
# outputs (footstep_wood, break_stone, place_wood) reuse the existing Kenney recordings
# unchanged rather than being synthesised here, because a real recording beats a synthetic
# one when it already is the right material; see tools/make_sounds.py's MANIFEST for those
# three. The nine functions below cover the rest. Each material gets one consistent
# spectral idea reused across its footstep/break/place variants (a lowpass cutoff and a
# tonal register for stone/wood/dirt, a highpass-only noise-burst pattern for grass), so
# that e.g. every dirt sound is recognisably dirt regardless of which action triggered it.


# ── Sound 7: footstep — stone ────────────────────────────────────────────────────────
#
# Stone gives almost nothing back to a footfall to absorb it, so the noise burst keeps
# nearly its whole spectrum (lowpass at 7000 Hz, barely a filter at all) and the tonal
# partial sits high (1800 Hz) and dies fast (tau 0.02s) — a "tick", not a "thud". 0.14s:
# the shortest of the twelve new sounds, because stone returns its energy immediately
# instead of ringing or absorbing it.
def make_footstep_stone() -> list[float]:
    dur_s = 0.14
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x46535354)  # 'FSST'

    env = env_exp_decay(n, tau_s=0.020, attack_s=0.001)
    for i in range(n):
        t = i / RATE
        buf[i] += 0.30 * env[i] * math.sin(2.0 * math.pi * 1800.0 * t)

    raw = [next(noise) for _ in range(n)]
    shaped = lowpass(raw, 7000.0)
    mix_into(buf, 0, [0.55 * s * e for s, e in zip(shaped, env)])

    return buf


# ── Sound 8: footstep — dirt ─────────────────────────────────────────────────────────
#
# The opposite of stone: dirt swallows almost everything above its lowest partials, so the
# lowpass sits at 500 Hz (nearly everything above that is gone) and the "tone" is a soft
# 90 Hz thump rather than a click — there is no bright transient to speak of. Slightly
# longer than stone's click (0.16s, tau 0.035s) because a muffled sound that is also
# instantaneous just reads as quiet, not dull; it needs a beat more time to read as soft.
def make_footstep_dirt() -> list[float]:
    dur_s = 0.16
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x46534454)  # 'FSDT'

    env = env_exp_decay(n, tau_s=0.035, attack_s=0.003)
    for i in range(n):
        t = i / RATE
        buf[i] += 0.30 * env[i] * math.sin(2.0 * math.pi * 90.0 * t)

    raw = [next(noise) for _ in range(n)]
    shaped = lowpass(raw, 500.0)
    mix_into(buf, 0, [0.75 * s * e for s, e in zip(shaped, env)])

    return buf


# ── Sound 9: footstep — grass ────────────────────────────────────────────────────────
#
# No tonal partial at all — grass has no resonant body to ring, only blades brushing past
# each other — so this is built the way make_eat() builds "bite, bite": two short
# highpassed noise bursts rather than one continuous one, because the gap between them is
# what reads as separate blades rather than a single soft impact. 0.15s, and quieter than
# the other two footsteps (gain 0.5/0.35) since grass is the softest surface underfoot.
def make_footstep_grass() -> list[float]:
    dur_s = 0.15
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x46534752)  # 'FSGR'

    burst_n = int(round(0.05 * RATE))
    starts = [0, int(round(0.07 * RATE))]
    gains = [0.5, 0.35]
    for start, gain in zip(starts, gains):
        raw = [next(noise) for _ in range(burst_n)]
        shaped = highpass(raw, 3000.0)
        benv = env_exp_decay(burst_n, tau_s=0.015, attack_s=0.002)
        mix_into(buf, start, [gain * s * e for s, e in zip(shaped, benv)])

    return buf


# ── Sound 10: break — wood ───────────────────────────────────────────────────────────
#
# A hollow knock — 350 Hz fundamental plus a weaker 700 Hz overtone, the "bock" a wooden
# plank gives back rather than the pure sine a struck string would — followed by three
# short lowpassed noise ticks standing in for splinters coming apart, spaced 60ms apart so
# they read as separate pieces breaking rather than one crack. Busiest and longest of the
# wood/dirt/grass break trio (0.28s): breaking should read as more eventful than placing
# or walking.
def make_break_wood() -> list[float]:
    dur_s = 0.28
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x42525744)  # 'BRWD'

    env = env_exp_decay(n, tau_s=0.09, attack_s=0.003)
    for i in range(n):
        t = i / RATE
        tone = (0.6 * math.sin(2.0 * math.pi * 350.0 * t)
                + 0.25 * math.sin(2.0 * math.pi * 700.0 * t))
        buf[i] += 0.35 * env[i] * tone

    tick_n = int(round(0.035 * RATE))
    starts = [0, int(round(0.06 * RATE)), int(round(0.12 * RATE))]
    gains = [0.6, 0.45, 0.3]
    for start, gain in zip(starts, gains):
        raw = [next(noise) for _ in range(tick_n)]
        shaped = lowpass(raw, 2500.0)
        tenv = env_exp_decay(tick_n, tau_s=0.02, attack_s=0.001)
        mix_into(buf, start, [gain * s * e for s, e in zip(shaped, tenv)])

    return buf


# ── Sound 11: break — dirt ───────────────────────────────────────────────────────────
#
# Two heavily lowpassed noise clods (450 Hz cutoff — the same "nothing above the low end
# survives" idea as footstep_dirt) landing 70ms apart under a soft 80 Hz thump, rather than
# one continuous burst, so a dirt block reads as crumbling into clods instead of thudding
# once. 0.26s: busier than the footstep but with the same dull, cutoff-limited character
# throughout, so it is still unmistakably dirt.
def make_break_dirt() -> list[float]:
    dur_s = 0.26
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x42524454)  # 'BRDT'

    env = env_exp_decay(n, tau_s=0.09, attack_s=0.004)
    for i in range(n):
        t = i / RATE
        buf[i] += 0.30 * env[i] * math.sin(2.0 * math.pi * 80.0 * t)

    clod_n = int(round(0.10 * RATE))
    starts = [0, int(round(0.07 * RATE))]
    gains = [0.8, 0.55]
    for start, gain in zip(starts, gains):
        raw = [next(noise) for _ in range(clod_n)]
        shaped = lowpass(raw, 450.0)
        cenv = env_exp_decay(clod_n, tau_s=0.05, attack_s=0.003)
        mix_into(buf, start, [gain * s * e for s, e in zip(shaped, cenv)])

    return buf


# ── Sound 12: break — grass ──────────────────────────────────────────────────────────
#
# Four short highpassed bursts rather than make_footstep_grass()'s two — breaking should
# read as busier than walking, and grass has no tonal body to add weight with, so the only
# way to make it read as more eventful than a footstep is more bursts, not louder or lower
# ones. No sine partial anywhere in this sound, same as the footstep. 0.24s: the shortest
# of the three synthesised breaks, because grass is the lightest material in the set.
def make_break_grass() -> list[float]:
    dur_s = 0.24
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x42524752)  # 'BRGR'

    burst_n = int(round(0.05 * RATE))
    starts = [0, int(round(0.045 * RATE)), int(round(0.10 * RATE)), int(round(0.16 * RATE))]
    gains = [0.65, 0.55, 0.45, 0.35]
    for start, gain in zip(starts, gains):
        raw = [next(noise) for _ in range(burst_n)]
        shaped = highpass(raw, 2800.0)
        benv = env_exp_decay(burst_n, tau_s=0.015, attack_s=0.002)
        mix_into(buf, start, [gain * s * e for s, e in zip(shaped, benv)])

    return buf


# ── Sound 13: place — stone ──────────────────────────────────────────────────────────
#
# The same bright click as footstep_stone (1800 Hz partial, 7000 Hz noise lowpass) struck
# twice rather than once — the second, quieter strike 50ms later stands in for the block
# seating into the grid, the same "note, then a second note" shape make_craft() uses for
# its two-note ding. Footsteps only strike once; placing strikes and then settles. 0.20s,
# between the footstep (0.14s) and the reused stone break in duration.
def make_place_stone() -> list[float]:
    dur_s = 0.20
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x504c5354)  # 'PLST'

    # (onset_s, tone_gain, noise_gain) — the second strike is the first at 60% level.
    strikes = ((0.00, 0.30, 0.55), (0.05, 0.18, 0.33))
    tick_n = int(round(0.055 * RATE))
    for onset_s, tone_gain, noise_gain in strikes:
        start = int(round(onset_s * RATE))
        length_n = min(tick_n, n - start)
        env = env_exp_decay(length_n, tau_s=0.02, attack_s=0.001)
        tone = [tone_gain * env[i] * math.sin(2.0 * math.pi * 1800.0 * (i / RATE))
                for i in range(length_n)]
        mix_into(buf, start, tone)

        raw = [next(noise) for _ in range(length_n)]
        shaped = lowpass(raw, 7000.0)
        mix_into(buf, start, [noise_gain * s * e for s, e in zip(shaped, env)])

    return buf


# ── Sound 14: place — dirt ───────────────────────────────────────────────────────────
#
# A single muffled thud, the same ~450-500 Hz-cutoff family as footstep_dirt and
# break_dirt, but one strike rather than two or three: dirt does not "seat" the way
# stone's flat faces do, it just sits where it lands. 0.20s, matching place_stone's
# duration so the two read as the same gameplay event at a different material rather than
# a different-length event.
def make_place_dirt() -> list[float]:
    dur_s = 0.20
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x504c4454)  # 'PLDT'

    env = env_exp_decay(n, tau_s=0.06, attack_s=0.004)
    for i in range(n):
        t = i / RATE
        buf[i] += 0.30 * env[i] * math.sin(2.0 * math.pi * 85.0 * t)

    raw = [next(noise) for _ in range(n)]
    shaped = lowpass(raw, 480.0)
    mix_into(buf, 0, [0.70 * s * e for s, e in zip(shaped, env)])

    return buf


# ── Sound 15: place — grass ──────────────────────────────────────────────────────────
#
# Two highpassed brush bursts, the same no-tonal-content family as the other two grass
# sounds, spaced a little further apart than footstep_grass's (80ms vs 70ms) and a touch
# louder, so placing reads as slightly more deliberate than a single step without adding
# anything grass does not actually have — no resonant body, no bass thump. 0.18s.
def make_place_grass() -> list[float]:
    dur_s = 0.18
    n = int(round(dur_s * RATE))
    buf = [0.0] * n
    noise = lcg_noise(0x504c4752)  # 'PLGR'

    burst_n = int(round(0.055 * RATE))
    starts = [0, int(round(0.08 * RATE))]
    gains = [0.6, 0.45]
    for start, gain in zip(starts, gains):
        raw = [next(noise) for _ in range(burst_n)]
        shaped = highpass(raw, 3000.0)
        benv = env_exp_decay(burst_n, tau_s=0.018, attack_s=0.002)
        mix_into(buf, start, [gain * s * e for s, e in zip(shaped, benv)])

    return buf


# ── Master / write / report ──────────────────────────────────────────────────────────

def master(buf: list[float]) -> list[float]:
    """Peak-normalise to PEAK_DBFS, then fade the tail to a hard zero."""
    peak = max(abs(v) for v in buf)
    if peak <= 0.0:
        raise ValueError("synthesis produced silence - refusing to write a dead track")

    gain = (10.0 ** (PEAK_DBFS / 20.0)) / peak
    out = [v * gain for v in buf]

    fade = max(1, int(round(END_FADE_S * RATE)))
    total = len(out)
    for i in range(min(fade, total)):
        idx = total - fade + i
        if 0 <= idx < total:
            out[idx] *= (fade - 1 - i) / fade

    return out


def to_pcm16(buf: list[float]) -> bytes:
    frames = bytearray()
    for v in buf:
        s = int(round(v * 32767.0))
        if s > 32767:
            s = 32767
        elif s < -32768:
            s = -32768
        frames += struct.pack("<h", s)
    return bytes(frames)


def write_wav(path: Path, pcm: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(SAMPLE_BYTES)
        w.setframerate(RATE)
        w.writeframes(pcm)


def report(path: Path) -> None:
    """Read the file back and print what its OWN header says — not the constants above."""
    with wave.open(str(path), "rb") as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        width = w.getsampwidth()
        frames = w.getnframes()
        data = w.readframes(frames)

    first = struct.unpack_from("<h", data, 0)[0]
    last = struct.unpack_from("<h", data, len(data) - 2)[0]
    peak = max(abs(v) for v in struct.unpack("<%dh" % frames, data))

    print("  wrote %s" % path.name)
    print("    bytes=%d  rate=%dHz  ch=%d  bits=%d  frames=%d  dur=%.4fs" %
          (path.stat().st_size, rate, ch, width * 8, frames, frames / rate))
    print("    first/last=%d/%d (both should be 0)  peak=%d/32767 (%.2f dBFS)" %
          (first, last, peak, 20.0 * math.log10(peak / 32767.0)))


GENERATORS = [
    ("hurt_synth.wav", make_hurt),
    ("death_synth.wav", make_death),
    ("eat_synth.wav", make_eat),
    ("craft_synth.wav", make_craft),
    ("splash_synth.wav", make_splash),
    ("ui_tap_synth.wav", make_ui_tap),

    # v1.8.19 lane: per-material footstep / break / place. footstep_wood, break_stone and
    # place_wood are NOT here — they reuse existing Kenney recordings verbatim, see
    # tools/make_sounds.py's MANIFEST.
    ("footstep_stone_synth.wav", make_footstep_stone),
    ("footstep_dirt_synth.wav", make_footstep_dirt),
    ("footstep_grass_synth.wav", make_footstep_grass),
    ("break_wood_synth.wav", make_break_wood),
    ("break_dirt_synth.wav", make_break_dirt),
    ("break_grass_synth.wav", make_break_grass),
    ("place_stone_synth.wav", make_place_stone),
    ("place_dirt_synth.wav", make_place_dirt),
    ("place_grass_synth.wav", make_place_grass),
]


def main(argv: list[str]) -> int:
    print("make_sfx_synth.py: writing %d source WAVs to %s" % (len(GENERATORS), OUT_DIR))
    for name, gen in GENERATORS:
        pcm = to_pcm16(master(gen()))
        out_path = OUT_DIR / name
        write_wav(out_path, pcm)
        report(out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
