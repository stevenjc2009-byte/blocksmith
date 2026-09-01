#!/usr/bin/env python3
"""Draws Blocksmith's distance-fog ramp texture from scratch.

Like every other sheet in gfx/, this is GENERATED — nothing is traced, sampled
or copied from anywhere. There is no art in it at all: it is a 128x8 alpha ramp
that turns a fog coordinate into a fog factor, and the only thing it encodes is
the curve.

    fog factor f(t) = t*t*(3 - 2*t)     smoothstep, t in 0..1

Why a texture rather than arithmetic. The PICA200 has no fragment shader, so a
per-fragment value can only reach the TEV combiner as a texture sample or as an
interpolated vertex colour. The vertex shader emits the fog COORDINATE on
texcoord1 (an affine function of eye-space depth — see source/gfx/fogramp.h for
why it must be depth and not a radial distance), and this sheet is what shapes
it. Putting the shape in a table also means the curve costs nothing per vertex
and can be any function at all, including one with zero derivative at both ends
— which is what stops a linear ramp's visible switch-on ring.

Layout. 128 texels across the fade, 8 rows tall because 8 is the PICA200's
minimum texture dimension. Every row is IDENTICAL, which is what lets the vertex
shader write the same value into both components of texcoord1 and not care what
v means. The ramp is in the ALPHA channel and gfx/fogramp.t3s asks tex3ds for
-f a8, one byte a texel, 1 KB for the whole sheet; RGB is left white and is
never sampled.

    -z none, not -z auto. The sheet is 1 KB, so compression saves nothing worth
    having, and tests/fogramp_test.c decodes the built build/fogramp.t3x and
    asserts every texel against source/gfx/fogramp.c's fogRampTexel() — the
    check that keeps this script and the C reference from drifting apart. An
    uncompressed t3x makes that decoder ten lines instead of an LZ11 inflater.

THE CURVE IS DUPLICATED, deliberately and with a guard. fogRampCurve() and
fogRampTexel() in source/gfx/fogramp.c are the reference the host suite and the
whole half_vis measurement run on; the two lines below are the copy that reaches
the GPU. tests/fogramp_test.c fails loudly if they disagree by a single texel.
That is the same arrangement world/atlas_uv_shader_test.c uses for the uvScale
constant duplicated across the two .pica files, and it is the only reason a
second copy of a number is allowed in this project.

Run from the repository root:

    python3 tools/make_fog_ramp.py

then rebuild from devkitPro MSYS2 so tex3ds picks the new .png up. The Makefile
carries an explicit `fogramp.t3x: fogramp.png` prerequisite for that — without
it the .d-file collision documented next to the atlas/font/crackatlas rules
would leave the console sampling the previous ramp.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

WIDTH = 128
HEIGHT = 8

OUT = Path(__file__).resolve().parent.parent / "gfx" / "fogramp.png"


def curve(t: float) -> float:
    """The fog factor at t. Must match fogRampCurve() in source/gfx/fogramp.c."""
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def texel(i: int) -> int:
    """Texel i's 8-bit value. Must match fogRampTexel() in source/gfx/fogramp.c.

    Sampled at the texel CENTRE because GPU_LINEAR interpolates between centres,
    and rounded with int(x + 0.5) rather than round(), because Python's round()
    is banker's rounding and C's is not — at 0.5 exactly the two disagree, and
    the cross-check in tests/fogramp_test.c is texel-exact.
    """
    return int(curve((i + 0.5) / WIDTH) * 255.0 + 0.5)


def main() -> None:
    img = Image.new("RGBA", (WIDTH, HEIGHT), (255, 255, 255, 0))
    px = img.load()
    for x in range(WIDTH):
        a = texel(x)
        for y in range(HEIGHT):
            px[x, y] = (255, 255, 255, a)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT)
    print(f"wrote {OUT} ({WIDTH}x{HEIGHT}, alpha ramp, first={texel(0)} last={texel(WIDTH - 1)})")


if __name__ == "__main__":
    main()
