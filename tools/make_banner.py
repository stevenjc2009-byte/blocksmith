#!/usr/bin/env python3
"""Draws Blocksmith's CIA icon and Home Menu banner from scratch.

Step 10.1 (CIA packaging) needs two pieces of art that bannertool turns into
the SMDH icon and the .bnr banner. Both are generated here, deterministically,
the same house rule as tools/make_atlas.py: nothing traced, sampled or copied
from another project, and a re-run must be byte-identical.

The motif is a single isometric block — top, left and right faces of a cube —
in the grass/dirt palette tools/make_atlas.py already established for
Blocksmith. Reusing that palette is not "borrowing art": it is this game's own
colours, picked so the Home Menu icon reads as the same game as the block
texture on screen. Deliberately plain per the brief: one clear cube, no text,
no clutter — legible at Home Menu thumbnail size.

Outputs:
    gfx/icon.png    48x48   - bannertool `makesmdh` requires exactly this size
                              (verified: a 32x32 input is rejected with
                              "[ERROR] Image must be exactly 48 x 48 in size.")
    gfx/banner.png  256x128 - bannertool `makebanner` requires exactly this
                              size (verified the same way: a 100x100 input is
                              rejected with "...exactly 256 x 128...").

Re-run after editing:
    python tools/make_banner.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

# Same family of greens as tools/make_atlas.py's grass tiles, split into three
# flat shades for a cube's top/left/right faces (top brightest - lit from
# above; right darkest - in shadow).
TOP = (88, 158, 104)
LEFT = (58, 112, 74)
RIGHT = (40, 82, 54)
OUTLINE = (24, 20, 28)

ICON_BG = (30, 26, 34)
BANNER_BG_TOP = (46, 40, 54)
BANNER_BG_BOTTOM = (24, 20, 28)


def draw_cube(draw: ImageDraw.ImageDraw, cx: int, cy: int, s: int) -> None:
    """One isometric cube, centred at (cx, cy), each face a rhombus of
    half-width s and half-height s/2 — the standard 2:1 isometric block."""
    h = s // 2
    top = [(cx, cy - s), (cx + s, cy - h), (cx, cy), (cx - s, cy - h)]
    left = [(cx - s, cy - h), (cx, cy), (cx, cy + s), (cx - s, cy + h)]
    right = [(cx + s, cy - h), (cx, cy), (cx, cy + s), (cx + s, cy + h)]
    draw.polygon(top, fill=TOP, outline=OUTLINE)
    draw.polygon(left, fill=LEFT, outline=OUTLINE)
    draw.polygon(right, fill=RIGHT, outline=OUTLINE)


def make_icon(out_path: Path) -> None:
    size = 48
    img = Image.new("RGBA", (size, size), ICON_BG + (255,))
    draw = ImageDraw.Draw(img)
    # One block filling most of the frame, leaving a margin so the outline
    # doesn't touch the edge at Home Menu thumbnail scale.
    draw_cube(draw, size // 2, size // 2 + 8, 16)
    img.save(out_path)
    print(f"{out_path}  {size}x{size}")


def make_banner(out_path: Path) -> None:
    w, h = 256, 128
    img = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        row = tuple(
            int(round(BANNER_BG_TOP[i] + (BANNER_BG_BOTTOM[i] - BANNER_BG_TOP[i]) * t))
            for i in range(3)
        )
        for x in range(w):
            px[x, y] = row + (255,)
    draw = ImageDraw.Draw(img)
    # Three blocks in a small row, alternating height, mirroring the game's
    # own "stack of blocks" identity without spelling out any text.
    draw_cube(draw, w // 2 - 64, h // 2 + 20, 30)
    draw_cube(draw, w // 2, h // 2 - 6, 34)
    draw_cube(draw, w // 2 + 64, h // 2 + 20, 30)
    img.save(out_path)
    print(f"{out_path}  {w}x{h}")


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    gfx = root / "gfx"
    gfx.mkdir(parents=True, exist_ok=True)
    make_icon(gfx / "icon.png")
    make_banner(gfx / "banner.png")


if __name__ == "__main__":
    main()
