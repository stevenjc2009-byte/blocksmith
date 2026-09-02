#!/usr/bin/env python3
"""Draws Blocksmith's rain/snow billboard texture from scratch — v1.8.9 weather rendering.

Like every other sheet in gfx/, this is GENERATED — nothing is traced, sampled or copied
from another game or from Minecraft's own art. Two soft alpha shapes, drawn procedurally:
a rain streak and a snow fleck.

── Layout, and WHY the tiles sit side by side, not stacked ────────────────────────────────

16 px wide, 8 px tall: two 8x8 tiles side by side along X, rain (tile 0) on the LEFT and
snow (tile 1) on the RIGHT. See source/gfx/weatherdraw.h's WEATHERDRAW_TILE_* constants,
which this layout must match, and weatherdraw.c's weatherTileU() for the arithmetic.

The sheet is exactly ONE TILE TALL for the same reason the block atlas is exactly one tile
wide on its own repeating axis (tools/make_atlas.py's own header): source/shaders/weather.v.pica
scrolls a billboard strip's fall animation by translating the texture V coordinate — the axis
that runs along a strip's WORLD-VERTICAL extent — and relying on GPU_REPEAT to wrap it. The
wrapped axis must therefore contain nothing but the ONE tile being repeated, or a scrolling
streak would wrap straight into whatever sits next to it along that axis. Putting rain and
snow side by side along U instead — the CLAMPED axis, sampled at a fixed value per strip and
never animated — means the vertical scroll can never leave the tile it started in, while U
stays exactly the tool's-eye "which of the two tiles" switch it needs to be.

(An earlier draft of this script stacked the tiles top-to-bottom along V instead, on the
mistaken assumption that repeat should run horizontally. That is backwards: repeats belong
on the AXIS THE STRIP IS TALL ALONG, which is V, so a single-tile-tall sheet is what
GPU_REPEAT on V actually needs. Left here because the failure mode is worth naming: it does
not throw, it draws a plausible-looking texture, and it wraps two tiles' worth of art into
each other the moment the scroll offset crosses 0.5 — the exact "a wrong texture coordinate
still renders a texture and never an error" trap tools/make_atlas.py's own header warns
about.)

── Shapes ──────────────────────────────────────────────────────────────────────────────────

Both shapes are soft (anti-aliased, not binary alpha): rain and snow both want blended edges
so a moving streak does not flicker between "textured" and "not" as it crosses a texel
boundary — the opposite requirement from the crack overlay's binary art, which is read by an
alpha TEST rather than blended (see tools/make_crack_atlas.py's own note on why that one is
binary). weatherdraw.c draws this sheet through C3D_AlphaBlend, not C3D_AlphaTest, exactly
because these shapes are not binary.

  * Rain (tile 0): elongated along the tile's LOCAL V axis (top-to-bottom within the tile) --
    the axis weather.v.pica maps to the world-vertical falling direction -- so the streak
    reads as a near-vertical falling line once it is on a world-space strip. Getting this
    backwards (elongating along U instead) would draw horizontal rain, a shape bug that would
    look like a rotated texture rather than an error -- see the note above about the axis
    swap this script itself first got wrong.
  * Snow (tile 1): a soft round fleck, axis-symmetric, so the slow horizontal drift
    weatherdraw.c applies to snow strips does not read as a shape rotating in place.

Determinism: no RNG at all. Both shapes are closed-form (an ellipse falloff for rain, a radial
falloff for snow), so re-running this script byte-for-byte reproduces the same PNG.

Re-run after editing:
    python tools/make_weathertex.py

then rebuild from devkitPro MSYS2 so tex3ds picks the new .png up. gfx/weathertex.t3s names
gfx/weathertex.png directly, and — per the .d-file stem-collision trap every other texture in
this project's Makefile already documents (atlas.t3x: atlas.png, font.t3x: font.png,
crackatlas.t3x: crackatlas.png, fogramp.t3x: fogramp.png) — needs its OWN
`weathertex.t3x: weathertex.png` prerequisite line added to the Makefile before a regenerated
PNG will actually reach a build. That line is NOT added by this script or by anything in this
task's scope; see docs/plan-1.8.9-weather-integration.md for the exact line and why it is left
for the integration lane rather than added here.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

TILE_PX = 8               # PICA200's minimum texture dimension -- both these shapes are tiny
TEX_W_PX = TILE_PX * 2    # two tiles side by side along U: rain, then snow
TEX_H_PX = TILE_PX        # exactly one tile tall, so GPU_REPEAT's wrapped V axis holds one tile

TILE_RAIN = 0
TILE_SNOW = 1

OUT = Path(__file__).resolve().parent.parent / "gfx" / "weathertex.png"


def rain_alpha(lx: float, ly: float) -> float:
    """Alpha at local tile coordinates lx, ly in 0..1 (tile-relative, not pixel).

    An ellipse falloff, long along ly (the axis weather.v.pica maps to world-vertical -- see
    the module docstring), narrow along lx. Centred in the tile so the streak does not touch
    either u edge, which would let GPU_CLAMP_TO_EDGE (weatherdraw.c's wrap mode on this axis)
    smear a partial streak across the tile boundary.
    """
    dx = (lx - 0.5) / 0.16          # short axis: a thin streak
    dy = (ly - 0.5) / 0.46          # long axis: nearly fills the tile
    d2 = dx * dx + dy * dy
    if d2 >= 1.0:
        return 0.0
    # Soft shoulder rather than a hard disc edge: (1 - d2) rather than a step, so the streak
    # fades out instead of stopping — the same smoothstep-family reasoning
    # source/gfx/fogramp.c's fogRampCurve uses for its own soft edges, at lower degree because
    # this is 8 texels across and a cubic would not resolve against a quadratic here.
    return max(0.0, min(1.0, 1.0 - d2))


def snow_alpha(lx: float, ly: float) -> float:
    """Alpha at local tile coordinates lx, ly in 0..1. A round, axis-symmetric fleck."""
    dx = (lx - 0.5) / 0.34
    dy = (ly - 0.5) / 0.34
    d2 = dx * dx + dy * dy
    if d2 >= 1.0:
        return 0.0
    return max(0.0, min(1.0, 1.0 - d2))


def tile_pixels(shape) -> list[list[int]]:
    """8x8 alpha values 0..255, sampling `shape` at each texel's CENTRE — the same "texel
    centre, not texel edge" rule source/gfx/fogramp.c's fogRampTexel() states, so a texel's
    stored value is what GPU_LINEAR will actually reconstruct at that texel's own coordinate.

    Returned as rows[py][px], py=0 is the tile's own top (matches PIL/PNG row order).
    """
    rows = []
    for py in range(TILE_PX):
        row = []
        for px in range(TILE_PX):
            lx = (px + 0.5) / TILE_PX
            ly = (py + 0.5) / TILE_PX
            a = shape(lx, ly)
            row.append(int(a * 255.0 + 0.5))
        rows.append(row)
    return rows


def main() -> None:
    img = Image.new("RGBA", (TEX_W_PX, TEX_H_PX), (255, 255, 255, 0))
    px = img.load()

    rain_rows = tile_pixels(rain_alpha)
    snow_rows = tile_pixels(snow_alpha)

    # Side-by-side placement along X: tile N's columns are [N*TILE_PX, (N+1)*TILE_PX). Rows
    # copy straight across -- PNG row order and texture-V-after-the-flip agree with each
    # other for a single-tile-tall sheet in the sense that there is exactly one flip to keep
    # track of (which weatherdraw.c's weatherTileU()/quad builder performs when it computes
    # v), not two interacting ones the way the old stacked layout needed.
    for tile, rows in ((TILE_RAIN, rain_rows), (TILE_SNOW, snow_rows)):
        col0 = tile * TILE_PX
        for ty in range(TILE_PX):
            for tx in range(TILE_PX):
                a = rows[ty][tx]
                px[col0 + tx, ty] = (255, 255, 255, a)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT)

    # ── verify what was just written, the way make_crack_atlas.py's verify_sheet() does ────
    #
    # Re-open the file (not the in-memory `img`) so this checks the bytes actually on disk,
    # not whatever PIL happens to still be holding.
    check = Image.open(OUT).convert("RGBA")
    cpx = check.load()

    def tile_alpha_sum(tile: int) -> int:
        col0 = tile * TILE_PX
        total = 0
        for ty in range(TILE_PX):
            for tx in range(TILE_PX):
                total += cpx[col0 + tx, ty][3]
        return total

    rain_sum = tile_alpha_sum(TILE_RAIN)
    snow_sum = tile_alpha_sum(TILE_SNOW)
    assert rain_sum > 0, "rain tile is fully transparent -- the streak did not draw"
    assert snow_sum > 0, "snow tile is fully transparent -- the fleck did not draw"

    # The two shapes must actually be DIFFERENT shapes, not two copies of the same alpha
    # pattern in different columns -- otherwise a caller could never tell the difference
    # between binding "rain" and binding "snow" and a texture-selection bug would render
    # correctly by accident.
    assert rain_sum != snow_sum, (
        f"rain (sum={rain_sum}) and snow (sum={snow_sum}) tiles are numerically "
        "identical -- they must be visibly different shapes"
    )

    # Centre texel of each tile must be near-opaque -- both falloffs are 1.0 at (0.5, 0.5) by
    # construction, so a centre that reads dim means the wrong tile was sampled or the column
    # placement above is backwards, exactly the class of bug tools/make_atlas.py's own header
    # describes ("a wrong texture coordinate still renders a texture and never an error").
    rain_col0 = TILE_RAIN * TILE_PX
    snow_col0 = TILE_SNOW * TILE_PX
    rain_centre = cpx[rain_col0 + TILE_PX // 2, TILE_PX // 2][3]
    snow_centre = cpx[snow_col0 + TILE_PX // 2, TILE_PX // 2][3]
    assert rain_centre >= 200, f"rain tile centre alpha {rain_centre} is not near-opaque"
    assert snow_centre >= 200, f"snow tile centre alpha {snow_centre} is not near-opaque"

    # The rain streak must actually be TALLER than it is wide (elongated along V, the
    # falling axis) -- otherwise the axis-swap mistake documented at the top of this file has
    # crept back in. Measured as: alpha-weighted column spread (variance along x) versus
    # alpha-weighted row spread (variance along y) within the rain tile only.
    def spread(tile: int, axis: str) -> float:
        col0 = tile * TILE_PX
        total_a = 0.0
        total_ad2 = 0.0
        for ty in range(TILE_PX):
            for tx in range(TILE_PX):
                a = cpx[col0 + tx, ty][3]
                pos = tx if axis == "x" else ty
                total_a += a
                total_ad2 += a * (pos - (TILE_PX - 1) / 2.0) ** 2
        return total_ad2 / total_a if total_a > 0 else 0.0

    rain_spread_x = spread(TILE_RAIN, "x")
    rain_spread_y = spread(TILE_RAIN, "y")
    assert rain_spread_y > rain_spread_x, (
        f"rain tile is not elongated along V (spread_x={rain_spread_x:.2f}, "
        f"spread_y={rain_spread_y:.2f}) -- the falling streak must run top-to-bottom "
        "within its own tile, not side to side"
    )

    print(
        f"wrote {OUT} ({TEX_W_PX}x{TEX_H_PX}, A8, rain tile 0 alpha-sum={rain_sum}, "
        f"snow tile 1 alpha-sum={snow_sum}, rain centre={rain_centre}, snow centre={snow_centre}, "
        f"rain spread x={rain_spread_x:.2f} y={rain_spread_y:.2f})"
    )


if __name__ == "__main__":
    main()
