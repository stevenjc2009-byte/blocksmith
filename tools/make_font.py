#!/usr/bin/env python3
"""Draws Blocksmith's GUI bitmap font from scratch (step 8.3).

Every glyph below is drawn here, by hand, as a 5x7 grid of '#' and '.'. Nothing is
rendered from a system typeface, traced, or lifted from another game — same rule as
tools/make_atlas.py, and for the same reason: a typeface is somebody's design work
even when it is eight pixels tall.

Layout: a 128x64 RGBA texture, 16 columns by 8 rows of 8x8 cells, holding ASCII 32
to 126 in order. A glyph occupies the top-left 5x7 of its cell, which leaves one
column and one row of guaranteed-empty pixels on the right and bottom — that gap is
what lets the C side draw text with GPU_NEAREST and no padding without a
neighbouring glyph bleeding in at the seam.

White with a hard alpha, so the C side tints it per draw rather than the texture
carrying a colour it can never change.

Re-run after editing:
    python tools/make_font.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

CELL_PX = 8            # cell pitch in the texture
GLYPH_W = 5            # drawn width inside the cell
GLYPH_H = 7            # drawn height inside the cell
COLS = 16
ROWS = 8
TEX_W = COLS * CELL_PX  # 128
TEX_H = ROWS * CELL_PX  # 64

FIRST_CHAR = 32
LAST_CHAR = 126

# Cell index of a solid white 8x8 block, sitting just past the last glyph.
#
# It is here rather than in a texture of its own so that the GUI's two primitives — a
# glyph and a filled rectangle — come from the same C3D_Tex. A panel behind a line of
# text is then the same draw call as the text, and the whole bottom screen can be one
# submission. A second texture would mean a bind, a flush and a second draw for every
# rectangle, which is most of what a UI is.
SOLID_CELL = LAST_CHAR - FIRST_CHAR + 1   # 95

# ── The glyphs ────────────────────────────────────────────────────────────────────
#
# Seven rows of five. Read them as pixels; that is exactly what they are.
#
# House rules, so the set looks like one typeface rather than ninety-five decisions:
#   * capitals and digits fill all seven rows,
#   * lowercase x-height is rows 2..6, with descenders running into row 7,
#   * punctuation sits on the same baseline as everything else,
#   * strokes are one pixel, because at 8px anything thicker closes its own holes.
GLYPHS: dict[str, list[str]] = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    '"': [".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."],
    "#": [".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."],
    "$": ["..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."],
    "%": ["##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"],
    "&": [".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"],
    "'": ["..#..", "..#..", ".....", ".....", ".....", ".....", "....."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    # Six-point rather than the filled star the obvious "#####" middle row produces:
    # at five pixels wide a solid centre closes up and reads as a blob, not an asterisk.
    "*": [".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    ",": [".....", ".....", ".....", ".....", ".....", "..##.", "..#.."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ".": [".....", ".....", ".....", ".....", ".....", "..##.", "..##."],
    "/": ["....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    ":": [".....", "..##.", "..##.", ".....", "..##.", "..##.", "....."],
    ";": [".....", "..##.", "..##.", ".....", "..##.", "..#..", ".#..."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "@": [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "[": [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."],
    "\\": ["#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#"],
    "]": [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."],
    "^": ["..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
    "`": [".#...", "..#..", ".....", ".....", ".....", ".....", "....."],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "b": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "####."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#....", ".###."],
    "d": ["....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "f": ["..##.", ".#...", "####.", ".#...", ".#...", ".#...", ".#..."],
    "g": [".....", ".####", "#...#", "#...#", ".####", "....#", ".###."],
    "h": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "j": ["...#.", ".....", "..##.", "...#.", "...#.", "#..#.", ".##.."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"],
    "n": [".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "p": [".....", "####.", "#...#", "#...#", "####.", "#....", "#...."],
    "q": [".....", ".####", "#...#", "#...#", ".####", "....#", "....#"],
    "r": [".....", ".....", "#.##.", "##...", "#....", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####"],
    "v": [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."],
    "x": [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"],
    "y": [".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."],
    "z": [".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"],
    "{": ["...##", "..#..", "..#..", ".#...", "..#..", "..#..", "...##"],
    "|": ["..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "}": ["##...", "..#..", "..#..", "...#.", "..#..", "..#..", "##..."],
    "~": [".....", ".....", ".#..#", "#.#.#", "#..#.", ".....", "....."],
}


def check() -> None:
    """Every printable character present, every glyph exactly 5x7.

    A missing glyph would draw as whatever happened to be in that cell of the
    texture, which reads as a corrupt font rather than a missing character — so
    this is a hard failure, not a warning.
    """
    missing = [chr(c) for c in range(FIRST_CHAR, LAST_CHAR + 1) if chr(c) not in GLYPHS]
    if missing:
        raise SystemExit(f"missing glyphs: {missing!r}")

    for ch, rows in GLYPHS.items():
        if len(rows) != GLYPH_H:
            raise SystemExit(f"glyph {ch!r} has {len(rows)} rows, want {GLYPH_H}")
        for i, row in enumerate(rows):
            if len(row) != GLYPH_W:
                raise SystemExit(f"glyph {ch!r} row {i} is {len(row)} wide, want {GLYPH_W}")
            bad = set(row) - {"#", "."}
            if bad:
                raise SystemExit(f"glyph {ch!r} row {i} has {bad!r}; only '#' and '.' allowed")

    # The bottom row and right column of every cell must stay empty, because that gap
    # is the only thing stopping a neighbouring glyph bleeding in at the seam. It is
    # structural rather than stylistic, so it is checked rather than trusted.
    assert GLYPH_W < CELL_PX and GLYPH_H < CELL_PX


def build() -> Image.Image:
    img = Image.new("RGBA", (TEX_W, TEX_H), (255, 255, 255, 0))
    px = img.load()

    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        idx = code - FIRST_CHAR
        cx = (idx % COLS) * CELL_PX
        cy = (idx // COLS) * CELL_PX

        for y, row in enumerate(GLYPHS[chr(code)]):
            for x, c in enumerate(row):
                if c == "#":
                    px[cx + x, cy + y] = (255, 255, 255, 255)

    # The solid cell. Filled edge to edge, unlike a glyph — a rectangle is sampled from
    # the middle of it (see spriteRect in source/gfx/sprite.c), so it has nothing to
    # bleed into and needs no gap.
    sx = (SOLID_CELL % COLS) * CELL_PX
    sy = (SOLID_CELL // COLS) * CELL_PX
    for y in range(CELL_PX):
        for x in range(CELL_PX):
            px[sx + x, sy + y] = (255, 255, 255, 255)
    return img


def main() -> None:
    check()
    img = build()

    out = Path(__file__).resolve().parent.parent / "gfx" / "font.png"
    img.save(out)
    print(f"wrote {out} ({TEX_W}x{TEX_H}, {LAST_CHAR - FIRST_CHAR + 1} glyphs)")

    # A human-readable proof sheet at 4x, because the only way to know a hand-drawn
    # font is right is to look at it. Not consumed by the build.
    preview = Image.new("RGBA", (TEX_W, TEX_H), (20, 24, 40, 255))
    preview.alpha_composite(img)
    preview = preview.resize((TEX_W * 4, TEX_H * 4), Image.NEAREST)
    prev_path = Path(__file__).resolve().parent.parent / "gfx" / "font_preview.png"
    preview.save(prev_path)
    print(f"wrote {prev_path} (4x proof sheet)")


if __name__ == "__main__":
    main()
