#!/usr/bin/env python3
"""Draws gfx/animals.png — the animal texture sheet — from scratch.

Original art for this project. Nothing here is borrowed, traced, sampled or ported: every
texel is the output of a function in this file. Minecraft is a visual REFERENCE for what a
blocky farm animal reads like at 16 texels to the block, in the same sense that a photograph
of a pig is a reference; no asset of its is used or derived from.

── why the art is generated in 3D and not painted per face ────────────────────────────────

The obvious way to write this script is to fill each face's rectangle with a pattern. That
produces seams: a cow's patch runs off the edge of the flank rectangle and restarts somewhere
unrelated on the top rectangle, because two rectangles that are adjacent on the ANIMAL are not
adjacent on the SHEET.

So nothing here paints in sheet space. For every texel of every face this script inverts the
unwrap in source/scene/entitymodel.h — sheet pixel -> which box, which face, which point in
MODEL space — and then evaluates the colour as a function of that 3D point. A cow's patches
are 3D Voronoi cells; a sheep's wool clumps are 3D Voronoi cells at a smaller scale. Both
therefore continue across every seam automatically, including the seam between the body and a
leg, because the leg genuinely is somewhere else in the same 3D field.

── the single source of truth ─────────────────────────────────────────────────────────────

The box table is NOT restated here. It is PARSED out of source/scene/entitymodel.h, which is
also what the console's vertex buffer is built from. A model and its texture layout are two
halves of one fact, and this project has repeatedly paid for one fact written down twice —
the atlas slot count, the diff-store capacity, the chunk dimension, each of which now needs a
Makefile guard to stay honest. There is nothing to keep in step here because there is only
one copy, and if the header's shape changes so this parse stops matching, this script stops
rather than quietly painting the old layout.

── the V flip ─────────────────────────────────────────────────────────────────────────────

There is no flip in this file. This script paints in PNG rows, top-left origin, exactly the
coordinates the header's `tox/toy` are in. The one conversion to texture v (which grows
upwards) lives in emBoxFaceQuad() in that header and nowhere else. If a flip is ever added
here, that one must go — two flips is no flip, and the symptom is not an error, it is an
animal wearing its belly on its back. tools/make_crack_atlas.py has the same warning for the
same reason; the difference is that the crack sheet puts the flip in the PNG and this one puts
it in the UVs, because this sheet's layout has no natural row order to flip.

── verification ───────────────────────────────────────────────────────────────────────────

verify_sheet() re-reads the finished PNG and checks it rather than trusting the code above it.
Every face rectangle of every box must be fully painted (the sheet starts as loud magenta, so
"unpainted" is a colour and not an absence), no two different boxes may overlap on the sheet
unless they deliberately share a net, every net must stay inside its animal's quadrant, and
every painted texel must be fully opaque. Those are the failures that render as *a* texture
rather than as an error, which is the class this GPU specialises in.

Usage:
    python3 tools/make_animals.py            # writes gfx/animals.png and verifies it
    python3 tools/make_animals.py --check     # verifies the existing PNG, writes nothing
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

from PIL import Image

# ---------------------------------------------------------------------------------------
# Layout, read out of the header rather than restated. See the module docstring.
# ---------------------------------------------------------------------------------------

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "source" / "scene" / "entitymodel.h"
OUT_PNG = REPO / "gfx" / "animals.png"

KIND_NAMES = ["pig", "cow", "chicken", "sheep", "zombie", "skeleton"]

# Loud, deliberately hideous, and never sampled if the UVs are right. An unpainted texel that
# reached the console as black or as transparent would look like shading; magenta cannot.
UNPAINTED = (255, 0, 255, 255)

FACE_TOP, FACE_BOTTOM, FACE_RIGHT, FACE_FRONT, FACE_LEFT, FACE_BACK = range(6)
FACE_NAMES = ["top", "bottom", "right", "front", "left", "back"]


def read_int_define(text: str, name: str) -> int:
    m = re.search(r"^#define\s+" + name + r"\s+(\d+)\b", text, re.M)
    if not m:
        raise SystemExit(
            f"make_animals.py: cannot read #define {name} out of {HEADER}.\n"
            f"  that macro is part of the sheet layout this script paints into. Without it\n"
            f"  there is nothing to paint against, so this stops rather than guess."
        )
    return int(m.group(1))


class Box:
    __slots__ = ("kind", "index", "px", "py", "pz", "sx", "sy", "sz", "tox", "toy")

    def __init__(self, kind, index, vals):
        self.kind = kind
        self.index = index
        (self.px, self.py, self.pz, self.sx, self.sy, self.sz, self.tox, self.toy) = vals

    def net_size(self) -> tuple[int, int]:
        """Mirrors emBoxNetSize() in the header: 2*(w+d) by (h+d)."""
        return 2 * (self.sx + self.sz), self.sy + self.sz

    def lo(self) -> tuple[int, int, int]:
        return self.px, self.py, self.pz

    def hi(self) -> tuple[int, int, int]:
        return self.px + self.sx, self.py + self.sy, self.pz + self.sz


def parse_header() -> tuple[dict, list[list[Box]]]:
    text = HEADER.read_text(encoding="utf-8", errors="replace")

    cfg = {
        name: read_int_define(text, name)
        for name in (
            "EM_SHEET_W",
            "EM_SHEET_H",
            "EM_QUADRANT_W",
            "EM_QUADRANT_H",
            "EM_QUADRANT_COLS",
            "EM_TEXELS_PER_BLOCK",
            "EM_KINDS",
            "EM_BOXES_PER_KIND",
        )
    }

    start = text.find("static const EmBox kEmBoxes")
    if start < 0:
        raise SystemExit(
            f"make_animals.py: cannot find `static const EmBox kEmBoxes` in {HEADER}.\n"
            f"  that table is the single source of truth for both the console's vertex\n"
            f"  buffer and this sheet's layout. If it moved, re-point this parse at it —\n"
            f"  do NOT restate the numbers here, which is the whole point of parsing."
        )
    end = text.find("\n};", start)
    if end < 0:
        raise SystemExit("make_animals.py: kEmBoxes is not terminated by a `\\n};` line.")
    body = text[start:end]

    rows = re.findall(
        r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
        r"\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
        r"\s*(\d+)\s*,\s*(\d+)\s*,?\s*\}",
        body,
    )

    want = cfg["EM_KINDS"] * cfg["EM_BOXES_PER_KIND"]
    if len(rows) != want:
        raise SystemExit(
            f"make_animals.py: parsed {len(rows)} boxes out of kEmBoxes, expected {want}\n"
            f"  (EM_KINDS {cfg['EM_KINDS']} x EM_BOXES_PER_KIND {cfg['EM_BOXES_PER_KIND']}).\n"
            f"  Painting a sheet against a layout this script only half understood is how a\n"
            f"  texture bug becomes an art bug, so it stops here."
        )

    kinds = []
    for k in range(cfg["EM_KINDS"]):
        boxes = []
        for b in range(cfg["EM_BOXES_PER_KIND"]):
            vals = [int(v) for v in rows[k * cfg["EM_BOXES_PER_KIND"] + b]]
            boxes.append(Box(k, b, vals))
        kinds.append(boxes)
    return cfg, kinds


def face_rect(box: Box, face: int) -> tuple[int, int, int, int, int, int, int, int, int, int]:
    """The exact inverse of emBoxFaceQuad()'s switch, in the same order and with the same
    names: rect x/y/w/h on the sheet, then the fixed axis and side, then the model axis and
    direction the rect's x and y run along.

    Kept structurally identical to the C rather than simplified, so the two can be read side
    by side. This is the one piece of duplication in the file and it is duplication of a
    FORMULA, not of data — the numbers it operates on all come from the parsed table.
    """
    w, h, d = box.sx, box.sy, box.sz
    if face == FACE_TOP:
        return (box.tox + d, box.toy, w, d, 1, 1, 0, 1, 2, 1)
    if face == FACE_BOTTOM:
        return (box.tox + d + w, box.toy, w, d, 1, 0, 0, 1, 2, 0)
    if face == FACE_RIGHT:
        return (box.tox, box.toy + d, d, h, 0, 1, 2, 1, 1, 1)
    if face == FACE_FRONT:
        return (box.tox + d, box.toy + d, w, h, 2, 0, 0, 1, 1, 1)
    if face == FACE_LEFT:
        return (box.tox + d + w, box.toy + d, d, h, 0, 0, 2, 0, 1, 1)
    return (box.tox + 2 * d + w, box.toy + d, w, h, 2, 1, 0, 0, 1, 1)


def texel_to_model(box: Box, face: int, s: float, t: float) -> tuple[float, float, float]:
    """(s, t) inside a face's rectangle -> a point in MODEL space, in texels.

    s and t are measured from the rectangle's top-left, and are passed in at texel CENTRES
    (i + 0.5) so a point never lands exactly on a box edge where the inversion is ambiguous.
    """
    _rx, _ry, _rw, _rh, fixed_axis, fixed_hi, s_axis, s_inv, t_axis, t_inv = face_rect(box, face)
    lo, hi = box.lo(), box.hi()

    p = [0.0, 0.0, 0.0]
    p[fixed_axis] = float(hi[fixed_axis] if fixed_hi else lo[fixed_axis])
    p[s_axis] = (hi[s_axis] - s) if s_inv else (lo[s_axis] + s)
    p[t_axis] = (hi[t_axis] - t) if t_inv else (lo[t_axis] + t)
    return p[0], p[1], p[2]


# ---------------------------------------------------------------------------------------
# Procedural primitives. All deterministic, all pure functions of position — no RNG state,
# so the sheet is byte-reproducible and a re-run after an unrelated edit produces no diff.
# ---------------------------------------------------------------------------------------


def _hash3(x: int, y: int, z: int, seed: int) -> int:
    n = (x * 0x27D4EB2D) ^ (y * 0x165667B1) ^ (z * 0x9E3779B1) ^ (seed * 0x85EBCA6B)
    n &= 0xFFFFFFFF
    n ^= n >> 15
    n = (n * 0x2C1B3C6D) & 0xFFFFFFFF
    n ^= n >> 12
    n = (n * 0x297A2D39) & 0xFFFFFFFF
    n ^= n >> 15
    return n


def hash01(x: int, y: int, z: int, seed: int) -> float:
    return _hash3(x, y, z, seed) / 4294967295.0


def voronoi3(p, cell: float, seed: int, jitter: float = 0.85):
    """Nearest-seed cell id and the gap to the runner-up.

    Returns (cell_hash, edge) where cell_hash is a stable 0..1 value per cell — what a
    patch's colour is chosen with — and edge is (d2 - d1), small near a cell boundary. The
    edge term is what makes a sheep's wool read as clumps rather than as flat blotches: it
    darkens the crease between clumps.
    """
    gx, gy, gz = p[0] / cell, p[1] / cell, p[2] / cell
    ix, iy, iz = math.floor(gx), math.floor(gy), math.floor(gz)

    best = (1e9, 0.0)
    second = 1e9
    for dz in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                cx, cy, cz = ix + dx, iy + dy, iz + dz
                fx = cx + jitter * (hash01(cx, cy, cz, seed) - 0.5) + 0.5
                fy = cy + jitter * (hash01(cx, cy, cz, seed + 1) - 0.5) + 0.5
                fz = cz + jitter * (hash01(cx, cy, cz, seed + 2) - 0.5) + 0.5
                d = (gx - fx) ** 2 + (gy - fy) ** 2 + (gz - fz) ** 2
                if d < best[0]:
                    second = best[0]
                    best = (d, hash01(cx, cy, cz, seed + 3))
                elif d < second:
                    second = d
    return best[1], math.sqrt(max(second, 0.0)) - math.sqrt(best[0])


def mottle(p, seed: int, scale: float = 2.0) -> float:
    """A small -1..1 grain, so no surface is a dead flat fill. Cheap on purpose: one hash
    per texel at one frequency is all this resolution can show."""
    return 2.0 * hash01(
        int(math.floor(p[0] / scale)),
        int(math.floor(p[1] / scale)),
        int(math.floor(p[2] / scale)),
        seed,
    ) - 1.0


def shade(rgb, amount: float):
    return (
        max(0, min(255, int(round(rgb[0] + amount)))),
        max(0, min(255, int(round(rgb[1] + amount)))),
        max(0, min(255, int(round(rgb[2] + amount)))),
    )


def mix(a, b, t: float):
    t = max(0.0, min(1.0, t))
    return (
        int(round(a[0] + (b[0] - a[0]) * t)),
        int(round(a[1] + (b[1] - a[1]) * t)),
        int(round(a[2] + (b[2] - a[2]) * t)),
    )


# ---------------------------------------------------------------------------------------
# The four animals.
#
# Each shader is called once per texel with:
#   box   the box being painted, face the face index,
#   p     the texel centre in MODEL space, in texels (x right, y up, z back; the animal
#         faces -z, so p[2] < 0 is towards its nose),
#   s, t  the texel's integer position inside its face rectangle, top-left origin — used
#         ONLY for features that belong to a face rather than to a place, i.e. eyes and
#         snout panels, which want to sit squarely on a face and not drift across a seam.
# ---------------------------------------------------------------------------------------

BODY, HEAD = 0, 1


def pig_shader(box, face, p, s, t, rw, rh):
    base = (223, 143, 156)
    c = shade(base, 5.0 * mottle(p, 101))

    # Underside a shade cooler and darker, on the same 3D test for every box, so the belly
    # and the inside of the legs agree with each other.
    if face == FACE_BOTTOM:
        c = shade(c, -22.0)

    # Rosy flank patches. Two 3D ellipsoids sitting on the body's sides, which is why they
    # continue onto the top of the back rather than stopping at the flank rectangle's edge.
    if box.index == BODY:
        for cx in (-4.0, 4.0):
            dx = (p[0] - cx) / 5.0
            dy = (p[1] - 10.5) / 4.0
            dz = (p[2] - 1.0) / 6.0
            r = dx * dx + dy * dy + dz * dz
            if r < 1.0:
                c = mix(c, (243, 179, 190), 1.0 - r * 0.55)

    if box.index == HEAD:
        # Ears: two darker wedges on the top of the head, nearest the front edge. On the TOP
        # rect, t = 0 is the BACK of the box, so "near the front" is large t.
        if face == FACE_TOP and t >= rh - 3:
            if s <= 1 or s >= rw - 2:
                c = shade(base, -52.0)
        # The snout panel the design asks for: a distinct raised-looking plate on the head's
        # front face, with two nostrils. Face-local, because a snout belongs to the face the
        # animal looks out of and nowhere else.
        if face == FACE_FRONT:
            if 2 <= s <= 5 and 3 <= t <= 6:
                c = (201, 119, 136)
                if t == 3:
                    c = (215, 133, 149)          # lit top lip of the plate
                if t in (4, 5) and s in (3, 5):
                    c = (137, 74, 90)            # nostrils
            if t == 1 and s in (1, rw - 2):
                c = (46, 32, 34)                 # eyes
            if t == 2 and s in (1, rw - 2):
                c = (232, 196, 200)              # a single lit texel under each eye

    # Trotters.
    if box.index >= 2 and p[1] < 1.6:
        c = mix(c, (92, 64, 70), 0.85)
    return c


def cow_shader(box, face, p, s, t, rw, rh):
    hide = (236, 232, 222)
    umber = (84, 57, 38)

    cell, edge = voronoi3(p, 6.5, 4400)
    c = shade(hide, 4.0 * mottle(p, 202))
    # Underside a shade darker, matching every other kind on this sheet: pig, chicken, sheep,
    # zombie and skeleton all ground themselves with a darkened FACE_BOTTOM and the cow was
    # the one kind missing it, which broke "consistent light direction across all kinds" —
    # a cow lit from above with a belly the same tone as its back reads as flat, not round.
    if face == FACE_BOTTOM:
        c = shade(c, -20.0)
    if cell > 0.55:
        # A patch. Eroded near its own boundary so the blobs come out irregular instead of
        # tiling as visible cells: `edge` is small next to a neighbouring cell, and requiring
        # a little margin nibbles the corners off.
        if edge > 0.06:
            c = shade(umber, 6.0 * mottle(p, 303))

    if box.index == HEAD:
        # The head is deliberately taken back off the Voronoi over the muzzle and eyes: a
        # patch across a cow's nose reads as damage, not as a marking.
        if face == FACE_FRONT:
            if 1 <= s <= rw - 2 and t >= 3:
                # Dark, not pink. A pale muzzle on a pale hide left the front of the head
                # reading as one flat panel in the 3/4 render; dark is also what the brief
                # asked for, and it is what separates this face from the pig's at a glance.
                c = (92, 76, 71)
                if t == 3:
                    c = (116, 98, 92)            # lit top lip
                if t in (5, 6) and s in (2, rw - 3):
                    c = (46, 37, 35)             # nostrils
            if t == 1 and s in (1, rw - 2):
                c = (38, 28, 26)                 # eyes
        # Horn stubs, on the top of the head at its front corners.
        if face == FACE_TOP and t >= rh - 3 and (s == 0 or s == rw - 1):
            c = (228, 221, 196)

    # Hooves. Cows are heavier than pigs, so two texels rather than one and a harder colour.
    if box.index >= 2 and p[1] < 2.1:
        c = mix(c, (56, 45, 42), 0.9)
    return c


def chicken_shader(box, face, p, s, t, rw, rh):
    cream = (241, 237, 221)
    # v1.8.18 ANIMAL-ART: 4.0 left the body reading as a near-flat pale blob at gameplay size
    # (the wing and tail cues below sat on top of it but were themselves too low-contrast to
    # separate from it) — bumped to 6.0, matching the grain the other kinds already carry.
    c = shade(cream, 6.0 * mottle(p, 505))

    if box.index == BODY:
        # Wing shading, as a 3D ellipsoid slab hugging each flank so the shoulder wraps onto
        # the back by exactly as much as the slab overlaps it and it stops dead at the belly.
        # This started life as an axis-aligned box with a hard border and a 2-texel bar
        # pattern, and the 3/4 render showed exactly what that is: a grey panel with rungs
        # riveted to the side of a crate, not a wing. The ellipse plus the soft edge below is
        # what turns it back into a folded wing, and the contrast against the cream is cut to
        # roughly a third of what it was so the shape reads without the outline reading.
        if abs(p[0]) > 1.9:
            dy = (p[1] - 5.8) / 2.05
            dz = (p[2] - 0.1) / 3.5
            d = math.sqrt(dy * dy + dz * dz)
            # v1.8.19 ANIMAL-ART: the wing's face rect is only 8x5 texels (checked against
            # face_rect() for the chicken's body-right/-left faces), and a 0.30-wide fade band
            # in `d` covers most of an ellipse that small -- there was no texel left for an
            # actual EDGE, only a gradient from body to wing and back, which is why a render
            # crop of this face at 24x still read as a soft blob rather than a folded shape.
            # Narrowed to 0.16 so the boundary resolves to 1-2 texels instead of 3-4: still a
            # soft antialiased edge, not the hard rectangle the v1.8.18 note above rejected,
            # just one that a 5-texel-tall face can actually render as an edge.
            w = min(1.0, max(0.0, (1.0 - d) / 0.16))
            if w > 0.0:
                # v1.8.18 ANIMAL-ART: the wing base was (214,210,205) against a (241,237,221)
                # body — an 11-point gap that vanished into the body's own mottle grain at
                # gameplay size, so the wing read as a smudge rather than a folded shape.
                # Dropped to a clearly darker, more saturated taupe and the rim shadow deepened
                # to match, so the wing separates from the body before any detail is read.
                #
                # v1.8.19 ANIMAL-ART: -34.0*d*d on its own was a smooth vignette from the
                # wing's centre to its rim, and on an 8x5-texel face that quadratic curve IS
                # the whole wing -- there was no flat interior for it to read as a filled
                # shape against, only gradient, which is the second half of why it looked like
                # a smudge rather than a folded shape even after the edge above was sharpened.
                # Split into a small flat interior term (-8.0, so the body of the wing reads
                # as one shade) plus a distinct rim band in the outer ring (d > 0.72) that
                # steps down hard rather than curving down -- a folded wing's trailing edge is
                # where the primaries overlap and shadow, which is a LINE, not a gradient.
                rim = -22.0 if d > 0.72 else 0.0
                wing = shade((182, 173, 158), 5.0 * mottle(p, 606) - 8.0 * d * d + rim)
                # Primaries: quills stacked up the trailing half of the wing, running fore-aft
                # in 3D so both flanks get the same feathers rather than a mirrored rectangle,
                # and only in the outer half so they fade out toward the shoulder. The period
                # is deliberately ~2 texels: the first attempt used 1.75 and a narrow
                # threshold, which put a 0.66-texel band on a 5-texel-tall body — below the
                # texel grid, so it rendered as nothing at all.
                # v1.8.19 ANIMAL-ART: at 8x5 texels the old q>0.2 threshold left at most one
                # faint stripe inside the sharpened edge above, which read as a single scratch
                # rather than stacked feathers. Widened to q>0.05 (more of the outer half gets
                # a quill) and the contrast term raised 26.0->36.0 so the stripe(s) that do fit
                # read as dark bars instead of a shade barely off the wing's own base colour.
                if p[2] > -0.6 and d > 0.32:
                    q = math.sin(p[1] * 3.14 + 1.1)
                    if q > 0.05:
                        wing = shade(wing, -36.0 * (q - 0.05) / 0.95)
                c = mix(c, wing, w)
        # Tail: the back-top corner lifts to a warmer, darker cream — deepened alongside the
        # wing so the two shape cues read at the same strength instead of the tail vanishing
        # once the wing became the more visible feature.
        if p[2] > 2.6 and p[1] > 6.6:
            c = shade((198, 186, 150), 6.0 * mottle(p, 707))
        if face == FACE_BOTTOM:
            c = shade(c, -20.0)

    if box.index == HEAD:
        # Comb: a red ridge down the centre of the top of the head.
        if face == FACE_TOP and 1 <= s <= rw - 2:
            c = (201, 54, 54) if s == 1 else (176, 42, 42)
        # Eyes on the sides, which is where a bird's are.
        if face in (FACE_LEFT, FACE_RIGHT) and t == 1 and s == 1:
            c = (32, 26, 24)

    if box.index == 4:                            # beak
        c = (238, 165, 52)
        if face == FACE_TOP:
            c = (250, 188, 84)
        if face == FACE_BOTTOM:
            c = (196, 124, 30)
    if box.index == 5:                            # wattle
        c = (197, 47, 47) if face != FACE_BOTTOM else (156, 32, 32)
    if box.index in (2, 3):                       # legs
        c = (217, 141, 57)
        if p[1] < 1.1:
            c = (176, 104, 36)
    return c


def sheep_shader(box, face, p, s, t, rw, rh):
    wool = (233, 229, 220)
    # Grey-taupe, NOT pink. The first pass used a dusty pink barely a shade off the pig's,
    # and head-on in the render the two animals were the same face; the wool is on the body,
    # so the head is the only thing the player has to tell them apart at range.
    bare = (176, 158, 155)

    # The head and the legs are BARE — that is the whole silhouette of a sheep: a cloud with
    # a small dark face and four thin legs poking out of it.
    woolly = box.index == BODY
    if box.index == HEAD and face == FACE_TOP and t < rh * 0.55:
        woolly = True                             # a tuft over the back half of the skull

    if woolly:
        # TWO scales, and the coarse one is why this reads as fleece rather than as noise.
        # A single cell size at 16 texels to the block gives clumps about three texels
        # across, which at any distance the player will actually see a sheep from averages
        # back out to flat grey — the first pass looked exactly like film grain. The coarse
        # layer groups those clumps into fleece-sized regions that survive the averaging;
        # the fine layer is what makes each region look lumpy up close.
        coarse, _ = voronoi3(p, 7.5, 9100)
        cell, edge = voronoi3(p, 3.6, 8800)
        c = shade(wool, 9.0 * (coarse - 0.5) + 24.0 * (cell - 0.5))
        # The crease between clumps. Without this the cells read as flat blotches; with it
        # they read as separate lumps of fleece, which is the whole difference between
        # "noisy white box" and "wool".
        if edge < 0.34:
            c = shade(c, -30.0 * (1.0 - edge / 0.34))
        if face == FACE_BOTTOM:
            c = shade(c, -18.0)
        return c

    c = shade(bare, 4.0 * mottle(p, 909))
    if box.index == HEAD:
        if face == FACE_FRONT:
            if 2 <= s <= rw - 3 and t >= rh - 3:
                c = (150, 133, 130)               # muzzle
                if t == rh - 3:
                    c = (166, 149, 146)           # lit top lip
                if t == rh - 2 and s in (2, rw - 3):
                    c = (78, 68, 67)              # nostrils
            if t == 1 and s in (1, rw - 2):
                c = (34, 27, 26)                  # eyes
        if face in (FACE_LEFT, FACE_RIGHT):
            c = shade(c, -8.0)
    if box.index >= 2 and p[1] < 2.1:
        c = mix(c, (88, 71, 70), 0.9)
    return c


def zombie_shader(box, face, p, s, t, rw, rh):
    """v1.8.18 MON-BUILD. Box order for both monsters is body(0), head(1), arm L/R(2,3),
    leg L/R(4,5) -- see the kEmBoxes rows in entitymodel.h. The zombie's arms are pure
    GEOMETRY reaching forward (py/pz place them stuck out in front of the body); nothing in
    this shader moves or rotates anything, it only colours the boxes the header already
    positioned that way.
    """
    # v1.8.18 ANIMAL-ART: the original three — skin (107,142,96), rot (58,74,49), shirt
    # (72,69,56) — measured at near-identical luminance (~130, ~68, ~69) with rot and shirt
    # eight points apart in the SAME dark olive-brown family, so the torn-shirt-over-flesh
    # read collapsed into one undifferentiated muddy patch, exactly the "muddy mid-grey"
    # this sheet is meant to avoid. Skin brightened toward a clearly sickly pale green so it
    # separates from cloth at a glance; rot pushed more saturated and green rather than just
    # darker, so decay reads as diseased flesh and not as a second shade of dirt; shirt pulled
    # toward a neutral brown-grey with the green removed, so cloth and flesh are never the
    # same hue family even before value is read.
    skin = (150, 173, 122)
    rot = (72, 100, 48)                         # a saturated, sickly green for decay patches
    shirt = (58, 52, 42)                        # neutral, desaturated brown-grey cloth

    cell, edge = voronoi3(p, 5.5, 5500)
    c = shade(skin, 5.0 * mottle(p, 1010))

    # Decay patches: the same eroded-Voronoi trick as the cow's hide markings, at a slightly
    # coarser cell size than skin's original 4.5 so patches read as legible wounds rather than
    # camouflage speckle at gameplay distance.
    if cell > 0.58 and edge > 0.05:
        c = shade(rot, 5.0 * mottle(p, 1011))

    if box.index == BODY:
        # A torn shirt over the front and sides; bare rotten flesh shows through the back and
        # through a rip down the centre of the chest. The geometry has no separate cloth
        # mesh, so the tear is carried entirely by colour.
        torn = abs(p[0]) < 1.6 and p[2] < -0.5
        if p[2] < 1.5 and not torn:
            c = shade(shirt, 5.0 * mottle(p, 1012))
        elif face == FACE_FRONT and abs(p[0]) < 2.5:
            # Ribs, visible only through the tear on the front face -- a rib belongs to the
            # face the wound opens onto, not to the whole body volume.
            c = shade(rot, -20.0 if t % 2 == 0 else -4.0)
        if face == FACE_BOTTOM:
            c = shade(c, -20.0)

    if box.index == HEAD:
        if face == FACE_FRONT:
            # Sunken eye sockets: wide black hollows rather than the animals' single bright
            # eye texel -- a zombie's face reads by what is missing.
            if t in (2, 3) and (1 <= s <= 2 or rw - 3 <= s <= rw - 2):
                c = (16, 20, 13)
            # A slack, torn-open jaw low on the face, with the odd broken tooth still in it.
            if 5 <= t <= 6 and 2 <= s <= rw - 3:
                c = (28, 24, 18)
                if t == 5 and s % 2 == 0:
                    c = (176, 168, 140)
        if face == FACE_TOP:
            # Hair, patchy rather than an even cap -- torn away in a few spots.
            if (s * 3 + t * 5) % 7 != 0:
                c = shade((37, 33, 25), 4.0 * mottle(p, 1013))

    if box.index in (2, 3):
        # Arms: grubby at the reaching hand -- the end farthest from the shoulder, i.e. the
        # most negative z, since these boxes are positioned pointing forward out of the body.
        if p[2] < -8.0:
            c = mix(c, (54, 50, 40), 0.7)

    if box.index in (4, 5) and p[1] < 1.6:
        c = mix(c, (46, 43, 34), 0.75)          # grime at the ankle

    return c


def skeleton_shader(box, face, p, s, t, rw, rh):
    """See zombie_shader's docstring for the shared box order. The skeleton's arms hang at
    its sides (py/pz keep them in line with the body, not reaching forward like the
    zombie's) purely as a geometry choice, so the two silhouettes read apart at a glance --
    nothing here animates either one.
    """
    bone = (214, 209, 190)
    shadow = (147, 140, 122)                    # gap-between-bone shadow, not a patch colour

    c = shade(bone, 4.0 * mottle(p, 2020))

    if box.index == BODY:
        # Ribs: horizontal bone bars with a dark gap between them, wrapped around the whole
        # ribcage rather than painted only on the front, because a ribcage is a cage.
        if face in (FACE_FRONT, FACE_LEFT, FACE_RIGHT, FACE_BACK):
            band = int(math.floor((p[1] - box.py) / 1.6)) % 2
            if band == 0:
                c = shade(shadow, 4.0 * mottle(p, 2021))
        # The spine: a single darker column down the centre of the back.
        if face == FACE_BACK and abs(p[0]) < 0.6:
            c = shade(shadow, -6.0)
        if face == FACE_BOTTOM:
            c = shade(c, -16.0)

    if box.index == HEAD:
        if face == FACE_FRONT:
            # Eye sockets: deep hollows, wider and darker than any living animal's eye texel.
            if t in (2, 3) and (1 <= s <= 2 or rw - 3 <= s <= rw - 2):
                c = (20, 18, 16)
            # The nasal cavity: a small dark hollow between the sockets.
            if t in (3, 4) and rw // 2 - 1 <= s <= rw // 2:
                c = (34, 31, 27)
            # A fixed, exposed row of teeth low on the face.
            if t == rh - 2 and 1 <= s <= rw - 2:
                c = shade(bone, -4.0) if s % 2 == 0 else shadow
        if face == FACE_TOP:
            # The cranial suture: a faint darker seam down the centre of the skull.
            if s == rw // 2:
                c = shade(c, -10.0)

    if box.index in (2, 3, 4, 5):
        # Long bones: a joint shadow at each end (the socket) and a slightly lighter shaft
        # between them, so a bare limb reads as a bone and not a uniform grey rod. Both the
        # arms and the legs are vertical bones on this kind, so one test in y covers both.
        local_y = p[1] - box.py
        if local_y < 1.2 or local_y > box.sy - 1.2:
            c = shade(shadow, 3.0 * mottle(p, 2022))
        else:
            c = shade(c, 3.0)

    return c


SHADERS = [pig_shader, cow_shader, chicken_shader, sheep_shader, zombie_shader, skeleton_shader]


# ---------------------------------------------------------------------------------------
# Painting
# ---------------------------------------------------------------------------------------


def paint(cfg, kinds) -> Image.Image:
    img = Image.new("RGBA", (cfg["EM_SHEET_W"], cfg["EM_SHEET_H"]), UNPAINTED)
    px = img.load()

    for k, boxes in enumerate(kinds):
        shader = SHADERS[k]
        for box in boxes:
            for face in range(6):
                rx, ry, rw, rh = face_rect(box, face)[:4]
                for tt in range(rh):
                    for ss in range(rw):
                        p = texel_to_model(box, face, ss + 0.5, tt + 0.5)
                        r, g, b = shader(box, face, p, ss, tt, rw, rh)
                        px[rx + ss, ry + tt] = (r, g, b, 255)
    return img


# ---------------------------------------------------------------------------------------
# Verification — run against the finished FILE, not against the code that wrote it.
# ---------------------------------------------------------------------------------------


def verify_sheet(path: Path, cfg, kinds) -> dict:
    img = Image.open(path)
    if img.mode != "RGBA":
        raise SystemExit(f"make_animals.py: {path} is mode {img.mode}, expected RGBA.")
    if img.size != (cfg["EM_SHEET_W"], cfg["EM_SHEET_H"]):
        raise SystemExit(
            f"make_animals.py: {path} is {img.size}, expected "
            f"({cfg['EM_SHEET_W']}, {cfg['EM_SHEET_H']}) from the header's EM_SHEET_W/H."
        )
    px = img.load()

    # owner[x][y] = (kind, tox, toy) of the box that painted it. Two boxes sharing a net is
    # legitimate and intended (the four legs of a quadruped); two boxes with DIFFERENT net
    # origins overlapping is a packing bug that would put one animal's leg on another's face.
    owner: dict[tuple[int, int], tuple[int, int, int]] = {}
    painted = [0] * cfg["EM_KINDS"]
    problems: list[str] = []

    for k, boxes in enumerate(kinds):
        qx = (k % cfg["EM_QUADRANT_COLS"]) * cfg["EM_QUADRANT_W"]
        qy = (k // cfg["EM_QUADRANT_COLS"]) * cfg["EM_QUADRANT_H"]
        for box in boxes:
            nw, nh = box.net_size()
            if not (qx <= box.tox and box.tox + nw <= qx + cfg["EM_QUADRANT_W"]
                    and qy <= box.toy and box.toy + nh <= qy + cfg["EM_QUADRANT_H"]):
                problems.append(
                    f"{KIND_NAMES[k]} box {box.index}: net {nw}x{nh} at "
                    f"({box.tox},{box.toy}) leaves the quadrant "
                    f"({qx},{qy})..({qx + cfg['EM_QUADRANT_W']},{qy + cfg['EM_QUADRANT_H']})"
                )
            for face in range(6):
                rx, ry, rw, rh = face_rect(box, face)[:4]
                for tt in range(rh):
                    for ss in range(rw):
                        x, y = rx + ss, ry + tt
                        if not (0 <= x < cfg["EM_SHEET_W"] and 0 <= y < cfg["EM_SHEET_H"]):
                            problems.append(
                                f"{KIND_NAMES[k]} box {box.index} face {FACE_NAMES[face]}: "
                                f"texel ({x},{y}) is off the sheet"
                            )
                            continue
                        r, g, b, a = px[x, y]
                        if (r, g, b, a) == UNPAINTED:
                            problems.append(
                                f"{KIND_NAMES[k]} box {box.index} face {FACE_NAMES[face]}: "
                                f"texel ({x},{y}) was never painted"
                            )
                        if a != 255:
                            problems.append(
                                f"{KIND_NAMES[k]} box {box.index} face {FACE_NAMES[face]}: "
                                f"texel ({x},{y}) alpha {a}, expected 255 — the sheet is "
                                f"opaque and RGBA5551 carries one alpha bit"
                            )
                        prev = owner.get((x, y))
                        mine = (k, box.tox, box.toy)
                        if prev is not None and prev != mine:
                            problems.append(
                                f"net collision at ({x},{y}): {KIND_NAMES[prev[0]]} net "
                                f"({prev[1]},{prev[2]}) and {KIND_NAMES[k]} net "
                                f"({box.tox},{box.toy}) both claim it"
                            )
                        owner[(x, y)] = mine
                        painted[k] += 1

    if problems:
        for line in problems[:24]:
            print("  " + line, file=sys.stderr)
        if len(problems) > 24:
            print(f"  ... and {len(problems) - 24} more", file=sys.stderr)
        raise SystemExit(f"make_animals.py: {len(problems)} problem(s) in {path}")

    unique = len(owner)
    total = cfg["EM_SHEET_W"] * cfg["EM_SHEET_H"]
    return {"painted": painted, "unique": unique, "total": total}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="verify the existing gfx/animals.png and write nothing")
    args = ap.parse_args()

    cfg, kinds = parse_header()

    if not args.check:
        img = paint(cfg, kinds)
        OUT_PNG.parent.mkdir(parents=True, exist_ok=True)
        img.save(OUT_PNG)

    if not OUT_PNG.exists():
        raise SystemExit(f"make_animals.py: {OUT_PNG} does not exist.")

    stats = verify_sheet(OUT_PNG, cfg, kinds)

    print(f"{'checked' if args.check else 'wrote'} {OUT_PNG.relative_to(REPO)}  "
          f"{cfg['EM_SHEET_W']}x{cfg['EM_SHEET_H']} RGBA  "
          f"{OUT_PNG.stat().st_size} bytes on disk")
    for k, name in enumerate(KIND_NAMES):
        boxes = kinds[k]
        nets = sorted({(b.tox, b.toy, *b.net_size()) for b in boxes})
        print(f"  {name:8s} {len(boxes)} boxes, {len(nets)} nets, "
              f"{stats['painted'][k]:5d} face texels")
    print(f"verified: every face rect painted and opaque, no net collisions, "
          f"every net inside its quadrant; {stats['unique']} of {stats['total']} sheet "
          f"texels are reachable")


if __name__ == "__main__":
    main()
