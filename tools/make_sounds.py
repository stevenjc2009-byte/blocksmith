#!/usr/bin/env python3
"""Packs source audio files into the BSND v1 containers the game loads at boot.

Why a custom container at all: the 3DS DSP plays raw PCM and nothing else. Every
alternative means shipping a decoder — Ogg Vorbis is ~2 MB of code and needs a
working buffer per stream, and the ARM11 has better things to do at 60 fps than
decode a footstep. So the decode happens HERE, once, on a desktop, and the console
does a bulk read into linear memory with no per-sound work at all. The cost of that
choice is disk size in the CIA, which is the cheapest of the three resources
(RomFS is not linear memory and is not the app heap).

The container is documented in source/audio/audio_bsnd.h. This file and that header
are the two halves of one format and must be changed together.

Usage:
    python tools/make_sounds.py            # pack the manifest into romfs/sfx/
    python tools/make_sounds.py --check    # report sizes, write nothing

Requires ffmpeg on PATH for the decode. Nothing else.
"""

import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

MAGIC = b"BSND"
VERSION = 1
# These two values are not free to choose: they are libctru's NDSP_ENCODING_PCM8 and
# NDSP_ENCODING_PCM16, so the console backend can pass the header field straight to
# ndspChnSetFormat without a translation table that could drift out of step with this
# file. source/audio/audio_bsnd.h states the same pair and audio_ndsp.c asserts it.
ENC_PCM8 = 0
ENC_PCM16 = 1
HEADER_BYTES = 24

# The rate every sound is resampled to. 22050 Hz is half of the 44.1 kHz the sources
# are mastered at, so the resample is a clean 2:1 decimation rather than an arbitrary
# ratio, and it halves the linear-memory cost of every sound against 44.1 kHz for a
# difference that a 3DS's speakers cannot reproduce anyway (they roll off well before
# the 11 kHz this still carries). The rate is stored per-sound in the header, so a
# future sound that needs more can have it without a format change.
TARGET_RATE = 22050

# Mono for the same reason: the DSP mixes a mono source into a stereo field for us via
# ndspChnSetMix, so storing two channels would double the memory to store a pan that
# gets overwritten by the listener's position on every play.
TARGET_CHANNELS = 1


# (output name, source zip-relative name, encoding). Kept explicit rather than globbed
# so that adding a sound is a visible, reviewable line, and so the shipped set cannot
# quietly grow when someone drops a file into a directory.
MANIFEST = [
    ("block_break", "impactMining_000.ogg", ENC_PCM16),
    ("block_place", "impactPlank_medium_000.ogg", ENC_PCM16),
    ("footstep", "footstep_wood_000.ogg", ENC_PCM16),
]


def decode(src: Path, encoding: int) -> bytes:
    """Runs ffmpeg to produce raw interleaved samples at TARGET_RATE, mono."""
    fmt = "s16le" if encoding == ENC_PCM16 else "u8"
    cmd = [
        "ffmpeg", "-v", "error", "-i", str(src),
        "-f", fmt, "-acodec", "pcm_" + fmt,
        "-ar", str(TARGET_RATE), "-ac", str(TARGET_CHANNELS),
        "-",
    ]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise SystemExit("ffmpeg failed for %s:\n%s" % (src, out.stderr.decode("utf-8", "replace")))
    data = out.stdout
    if encoding == ENC_PCM8:
        # ndsp's PCM8 is SIGNED; ffmpeg's u8 is unsigned with a 128 bias. Flipping the
        # top bit is the whole conversion. Doing it here rather than on the console is
        # the point of this tool: the console memcpys and plays.
        data = bytes((b ^ 0x80) for b in data)
    return data


def pack(name: str, data: bytes, encoding: int) -> bytes:
    bytes_per_frame = 2 if encoding == ENC_PCM16 else 1
    if len(data) % bytes_per_frame:
        raise SystemExit("%s: %d bytes is not a whole number of frames" % (name, len(data)))
    frames = len(data) // bytes_per_frame

    # 4-byte alignment of the payload matters on the console: the DSP reads the sample
    # data directly out of linear memory and the loader hands it out of a bump arena.
    # Padding here means the arena never has to insert a gap it would then have to
    # account for, so the pool cost of a sound is exactly what this file reports.
    pad = (-len(data)) % 4
    payload = data + b"\0" * pad

    head = struct.pack(
        "<4sHHIIII",
        MAGIC, VERSION, encoding, TARGET_RATE, frames, len(payload),
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    assert len(head) == HEADER_BYTES, len(head)
    return head + payload


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(REPO / "assets" / "sfx_src"),
                    help="directory holding the decoded-from source files")
    ap.add_argument("--out", default=str(REPO / "romfs" / "sfx"))
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    args = ap.parse_args()

    src_dir = Path(args.src)
    out_dir = Path(args.out)
    if not args.check:
        out_dir.mkdir(parents=True, exist_ok=True)

    total_pool = 0
    total_disk = 0
    print("%-14s %10s %10s %8s %s" % ("name", "frames", "poolbytes", "ms", "source"))
    for name, src_name, encoding in MANIFEST:
        src = src_dir / src_name
        if not src.is_file():
            raise SystemExit("missing source: %s" % src)
        raw = decode(src, encoding)
        blob = pack(name, raw, encoding)
        pool = len(blob) - HEADER_BYTES
        frames = struct.unpack_from("<I", blob, 12)[0]
        total_pool += pool
        total_disk += len(blob)
        print("%-14s %10d %10d %8.1f %s"
              % (name, frames, pool, 1000.0 * frames / TARGET_RATE, src_name))
        if not args.check:
            (out_dir / (name + ".bsnd")).write_bytes(blob)

    print("---")
    print("linear pool bytes required: %d" % total_pool)
    print("romfs bytes on disk:        %d" % total_disk)
    return 0


if __name__ == "__main__":
    sys.exit(main())
