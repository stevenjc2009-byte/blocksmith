#!/usr/bin/env python3
"""Draws Blocksmith's block-breaking CRACK overlay sheet from scratch.

Roadmap task 50 part 3. Like tools/make_atlas.py, every pixel here is generated
by this script — nothing is traced, sampled or copied from another game.

Why this is a SEPARATE sheet from gfx/atlas.png
-----------------------------------------------
The world atlas is full for this purpose. source/world/atlas_uv.h makes it a
16x256 one-tile-wide strip: ATLAS_TILE_SLOTS is 16, only 15 are addressable
(MeshVertex.v is a uint8_t, so the top slot's v1 = 256 does not fit), and
ATLAS_TILE_MISSING (slot 14) is permanently reserved for the missing-texture
marker. source/gfx/atlas.h's TILE_* enum already names twelve painted tiles
(0..11), which leaves exactly two free slots. Eight crack stages cannot fit,
and growing the shared strip to 16x512 would NOT help — the uint8_t v ceiling
is 15 addressable slots at any sheet height (see atlas_uv.h's long note).

So the crack stages get their own small texture: 16 px wide by 128 px tall,
eight 16x16 stages stacked vertically, no padding. At RGBA5551 that is 2 bytes
a texel, 4,096 bytes of VRAM — trivial beside the 8 KB the world atlas already
costs, and it keeps the shared strip's slot budget untouched.

Layout, and the V flip
----------------------
Same convention as tools/make_atlas.py, deliberately: texture v grows UPWARDS
while PNG rows run downwards, so stage `s` owns texture rows [s*16, s*16+16)
and is written to PNG rows [128 - (s+1)*16, 128 - s*16). Stage 0 is therefore
the LAST 16 rows of the PNG and stage 7 the FIRST 16.

This project has already been bitten by getting a V flip wrong — every glyph in
the font resolved to another glyph, and it presented as bad art rather than as
an error, because a wrong texture coordinate still samples *a* texture. So the
flip is asserted, not assumed: stage 0 is the faintest stage and stage 7 the
heaviest, so the lit-pixel count read back out of the PNG must INCREASE towards
the top of the image. If the flip were dropped the counts would decrease and
verify_sheet() below goes red.

Colour and alpha
----------------
Pure black cracks on a FULLY TRANSPARENT background. The overlay is drawn on
top of the block's own textured face with an alpha TEST (GPU_GREATER, 127 —
the same idiom source/scene/chunk_render.c uses for leaves and tall grass), not
with blending, so alpha is 0 or 255 and never anything between. RGBA5551 stores
exactly that losslessly in its one alpha bit; a middling value would land on one
side of the threshold and look like a mistake either way.

Monotonic growth
----------------
Every pixel lit at stage N is still lit at stage N+1. That is not a property
this script checks after the fact — it is how the art is built. One ordered
list of crack pixels is generated, and stage N is a PREFIX of that list. A
prefix of a list is a subset of every longer prefix by construction, so the
progression can only ever read as spreading damage and never as a flicker.
verify_sheet() re-reads the finished PNG and asserts it anyway, because the
thing that would break it is a bug in the placement, not in the ordering.

Determinism
-----------
One fixed seed, one stream, drawn in one order. Re-running must produce a
byte-identical PNG, or a rebuild silently changes the art and any comparison
against a previous build becomes meaningless.

Re-run after editing:
    python tools/make_crack_atlas.py
"""

from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image

TILE_PX = 16
CRACK_STAGES = 8                       # stage 0 = faintest, stage 7 = nearly shattered
CRACK_W_PX = TILE_PX                   # one tile wide
CRACK_H_PX = TILE_PX * CRACK_STAGES    # 128; both dimensions are powers of two, as the
                                       # PICA200 requires

# Fixed seed. See the module docstring — the sheet must be byte-identical on every re-run.
SEED = 20260824

CRACK_RGBA = (0, 0, 0, 255)      # opaque black
CLEAR_RGBA = (0, 0, 0, 0)        # alpha 0: the block's own texture shows through

# How far through the ordered pixel list each stage reaches, as a fraction of the whole.
# Stage 7 is 1.0 by definition — it is the whole list. The early steps are small so that
# stage 0 is a handful of pixels rather than an obvious fracture: the first tick of a
# break should be barely visible.
STAGE_FRACTIONS = (0.05, 0.12, 0.21, 0.32, 0.45, 0.60, 0.79, 1.00)

# Random walk shape. MAX_BRANCHES caps how many crack arms are ever alive at once; a
# 16x16 tile saturates into a black square well before a dozen.
START_ARMS = 3
MAX_BRANCHES = 7
MAX_ROUNDS = 40
BRANCH_CHANCE = 0.13
TURN_JITTER = 0.38               # radians a branch may turn per step


def crack_pixel_order(rng: random.Random) -> list[tuple[int, int]]:
    """The ordered list of crack pixels. Index 0 appears first, the last index last.

    Grown round-robin rather than one branch at a time. If each arm were walked to
    completion before the next one started, an early prefix of this list would be a
    single long line reaching the tile edge, and stage 1 would already look like a
    finished crack. Advancing every live arm one step per round instead makes the
    prefix a small radial star that spreads outwards, which is what damage looks like.
    """
    pixels: list[tuple[int, int]] = []
    seen: set[tuple[int, int]] = set()

    def emit(px: int, py: int) -> None:
        if 0 <= px < TILE_PX and 0 <= py < TILE_PX and (px, py) not in seen:
            seen.add((px, py))
            pixels.append((px, py))

    # The impact point, off-centre so the eight stages do not read as a symmetric
    # snowflake stamped in the middle of every block.
    cx = TILE_PX / 2.0 + rng.uniform(-1.5, 1.5)
    cy = TILE_PX / 2.0 + rng.uniform(-1.5, 1.5)
    emit(int(cx), int(cy))

    base_angle = rng.uniform(0.0, 2.0 * math.pi)
    branches = [
        {
            "x": cx,
            "y": cy,
            "a": base_angle + arm * (2.0 * math.pi / START_ARMS) + rng.uniform(-0.4, 0.4),
            "alive": True,
        }
        for arm in range(START_ARMS)
    ]

    for _ in range(MAX_ROUNDS):
        if not any(b["alive"] for b in branches):
            break
        # Snapshot: a branch spawned this round starts walking on the NEXT round, so a
        # burst of spawns cannot race ahead of the arms that produced them.
        for b in list(branches):
            if not b["alive"]:
                continue
            b["a"] += rng.uniform(-TURN_JITTER, TURN_JITTER)
            b["x"] += math.cos(b["a"])
            b["y"] += math.sin(b["a"])
            px, py = int(round(b["x"])), int(round(b["y"]))
            if not (0 <= px < TILE_PX and 0 <= py < TILE_PX):
                b["alive"] = False
                continue
            emit(px, py)
            if len(branches) < MAX_BRANCHES and rng.random() < BRANCH_CHANCE:
                fork = rng.choice((-1.0, 1.0)) * rng.uniform(0.7, 1.4)
                branches.append({"x": b["x"], "y": b["y"], "a": b["a"] + fork, "alive": True})

    # Thickening, appended AFTER the whole walk so it lands in the late stages. The first
    # pass widens the cracks to two pixels here and there; the second chips at the widened
    # edges. Both iterate over a frozen snapshot of what came before, so a pass can only
    # thicken what already existed and never chase its own output out to the tile edge.
    for chance in (0.34, 0.42):
        for px, py in list(pixels):
            if rng.random() >= chance:
                continue
            dx, dy = rng.choice(((1, 0), (-1, 0), (0, 1), (0, -1)))
            emit(px + dx, py + dy)

    return pixels


def stage_cut(order_len: int, stage: int) -> int:
    """How many pixels of the ordered list stage `stage` lights.

    At least stage+1, so no two early stages can come out identical after rounding —
    on a 16x16 tile the first three fractions are only a few pixels apart, and two
    consecutive stages that draw the same pixels read as the break having stalled.
    """
    cut = int(round(STAGE_FRACTIONS[stage] * order_len))
    return max(stage + 1, min(order_len, cut))


def stage_tile(order: list[tuple[int, int]], stage: int) -> Image.Image:
    """One 16x16 RGBA stage: a PREFIX of `order` drawn black on transparent."""
    img = Image.new("RGBA", (TILE_PX, TILE_PX), CLEAR_RGBA)
    px = img.load()
    for x, y in order[: stage_cut(len(order), stage)]:
        px[x, y] = CRACK_RGBA
    return img


def slot_png_y(stage: int) -> int:
    """The PNG row a stage's top edge sits on.

    Stage `stage` owns texture rows [stage*16, stage*16+16), and texture v grows upwards
    while PNG rows run downwards, so its PNG top row is CRACK_H_PX - (stage+1)*TILE_PX.
    Stage 0 is the LAST 16 rows of the image. This is the same flip make_atlas.py's
    slot_png_y applies to the world sheet, and it must agree with crackStageV() in
    source/scene/crackoverlay.c or the wrong stage is sampled — silently, as bad art.
    """
    return CRACK_H_PX - (stage + 1) * TILE_PX


def place(sheet: Image.Image, tile: Image.Image, stage: int) -> None:
    """Blits a stage into its row.

    The size check is not defensive padding: Image.paste places whatever it is handed at
    the offset and leaves the rest of the row as whatever was underneath, so a 15x15 tile
    would produce a valid-looking sheet with a one-pixel seam of the neighbouring stage.
    That is bad art, never an error — the exact failure mode this project keeps paying for.
    """
    if not 0 <= stage < CRACK_STAGES:
        raise ValueError(f"stage {stage} is outside 0..{CRACK_STAGES - 1}")
    if tile.size != (TILE_PX, TILE_PX):
        raise ValueError(
            f"stage {stage} is {tile.size[0]}x{tile.size[1]}, not {TILE_PX}x{TILE_PX}"
        )
    sheet.paste(tile, (0, slot_png_y(stage)))


def verify_sheet(path: Path) -> list[int]:
    """Re-reads the finished PNG and asserts what the C side depends on.

    Read back off disk rather than checked on the in-memory image on purpose: the thing
    being verified includes the save itself (mode, channel order, that the alpha channel
    survived), not only the drawing.

    Four properties, all of which fail as BAD ART rather than as an error if they break:

      1. The sheet is exactly CRACK_W_PX x CRACK_H_PX RGBA.
      2. Every pixel is either fully transparent or opaque black. A middling alpha lands
         on one side of the alpha test and looks like a mistake either way.
      3. Every stage is a strict superset of the one below it — monotonic growth.
      4. Lit-pixel counts INCREASE towards the top of the PNG, which is what proves the
         V flip in slot_png_y is applied: stage 0 is the faintest stage and it must be at
         the BOTTOM of the image. Without the flip this goes red.

    Returns the per-stage lit-pixel counts, stage 0 first.
    """
    img = Image.open(path)
    if img.mode != "RGBA":
        raise AssertionError(f"{path} is mode {img.mode}, not RGBA")
    if img.size != (CRACK_W_PX, CRACK_H_PX):
        raise AssertionError(
            f"{path} is {img.size[0]}x{img.size[1]}, not {CRACK_W_PX}x{CRACK_H_PX}"
        )
    px = img.load()

    lit_sets: list[set[tuple[int, int]]] = []
    for stage in range(CRACK_STAGES):
        top = slot_png_y(stage)
        lit: set[tuple[int, int]] = set()
        for y in range(top, top + TILE_PX):
            for x in range(TILE_PX):
                r, g, b, a = px[x, y]
                if a == 0:
                    if (r, g, b) != (0, 0, 0):
                        # Harmless to the GPU, but it means the background is not the
                        # clear colour this script thinks it wrote.
                        raise AssertionError(
                            f"stage {stage} pixel ({x},{y - top}) is transparent but not "
                            f"black: {(r, g, b)}"
                        )
                elif a == 255:
                    if (r, g, b) != (0, 0, 0):
                        raise AssertionError(
                            f"stage {stage} pixel ({x},{y - top}) is opaque but not black: "
                            f"{(r, g, b)}"
                        )
                    lit.add((x, y - top))
                else:
                    raise AssertionError(
                        f"stage {stage} pixel ({x},{y - top}) has alpha {a}: the overlay is "
                        f"alpha-TESTED, so alpha must be 0 or 255 and never between"
                    )
        lit_sets.append(lit)

    for stage in range(1, CRACK_STAGES):
        missing = lit_sets[stage - 1] - lit_sets[stage]
        if missing:
            raise AssertionError(
                f"stage {stage} lost {len(missing)} pixel(s) that stage {stage - 1} lit, "
                f"e.g. {sorted(missing)[0]} - the progression would flicker"
            )
        if len(lit_sets[stage]) <= len(lit_sets[stage - 1]):
            raise AssertionError(
                f"stage {stage} lights {len(lit_sets[stage])} pixels, no more than stage "
                f"{stage - 1}'s {len(lit_sets[stage - 1])} - the break would appear to stall"
            )

    # The V-flip check, stated in PNG-row terms so it is about the image and not about the
    # stage indices that were just used to read it: the faintest 16 rows must be the
    # BOTTOM 16 rows of the file.
    rows_lit = [
        sum(1 for y in range(band * TILE_PX, band * TILE_PX + TILE_PX)
            for x in range(TILE_PX) if px[x, y][3] == 255)
        for band in range(CRACK_STAGES)
    ]
    if rows_lit != sorted(rows_lit, reverse=True):
        raise AssertionError(
            f"PNG bands top-to-bottom light {rows_lit} pixels: they must DECREASE downwards, "
            f"because texture v grows upwards and stage 0 is the faintest. The V flip in "
            f"slot_png_y() is not being applied."
        )

    return [len(s) for s in lit_sets]


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    out_png = root / "gfx" / "crackatlas.png"
    out_png.parent.mkdir(parents=True, exist_ok=True)

    rng = random.Random(SEED)
    order = crack_pixel_order(rng)

    sheet = Image.new("RGBA", (CRACK_W_PX, CRACK_H_PX), CLEAR_RGBA)
    for stage in range(CRACK_STAGES):
        place(sheet, stage_tile(order, stage), stage)
    sheet.save(out_png)

    counts = verify_sheet(out_png)

    size = out_png.stat().st_size
    print(f"{out_png}  {CRACK_W_PX}x{CRACK_H_PX}  {CRACK_STAGES} stages  "
          f"no padding  seed {SEED}  {size} bytes on disk")
    print(f"  {len(order)} crack pixels in the ordered list; "
          f"{CRACK_W_PX * CRACK_H_PX * 2} bytes of VRAM at RGBA5551")
    for stage in range(CRACK_STAGES):
        y = slot_png_y(stage)
        print(f"  stage {stage}  png row {y:3d}..{y + TILE_PX - 1:3d}   "
              f"tex v {stage * TILE_PX:3d}..{stage * TILE_PX + TILE_PX:3d}   "
              f"{counts[stage]:3d} lit px "
              f"({100.0 * counts[stage] / (TILE_PX * TILE_PX):5.1f}% of the tile)")
    print("  verified off disk: RGBA, binary alpha, every stage a superset of the last, "
          "V flip applied")


if __name__ == "__main__":
    main()
