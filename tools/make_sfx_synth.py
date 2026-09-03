#!/usr/bin/env python3
"""Synthesises source WAVs for the six new v1.8.17 gameplay cues (lane SOUND-A).

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
    python tools/make_sfx_synth.py            # writes all six WAVs into assets/sfx_src/
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
