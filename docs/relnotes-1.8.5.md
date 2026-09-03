# Blocksmith v1.8.5 — Render distance

There were two ceilings on how far you could see, and only one of them was the one
everybody talks about.

Raising the number of chunks the game loads was the easy half. The half that actually
decided what you saw was the fog — and it had been quietly closing the world in at
about fourteen blocks no matter what the render-distance slider said.

Both are gone. **This release makes the view further on both consoles, and further
still on a New 3DS.** Those are two separate changes, and the notes below keep them
separate.

## The fog was the real limit

The fade to sky used to be drawn by a piece of fixed hardware on the PICA200: a
128-entry table indexed by 1/w, whose steps bunch up close to the camera. With the
near plane where this game pins it, **the first step past the far plane landed at
20.68 blocks**, and everything beyond that was one unshapeable segment. The result
was a world half-faded by about 14 blocks at *every* render distance — measured at
14.3358, 14.3651 and 14.4065 blocks for radius 3, 4 and 5. Turning the distance up
loaded more chunks and showed you none of them.

The fade is now drawn by the game itself, from a ramp texture sampled off true eye
depth, and it scales with the setting:

| Render distance | World half-faded at |
| --- | --- |
| 3 | 28.5 blocks |
| 4 | 38.0 blocks |
| 5 | 47.5 blocks |

A clean 9.5 blocks per step, against 0.03 before.

**If you have an Old 3DS, most of this release is right here.** Your limit is
unchanged at distance 3 — but what you can actually see at distance 3 roughly
doubles.

## A New 3DS goes up to render distance 5

The slider, the title-screen stepper and the saved settings file all go to 5 on a
New 3DS. An Old 3DS still stops at 3, which is what its memory holds. *This is the
only setting in the release where the two consoles differ.*

It is a **ceiling, not a default**. A New 3DS still starts at 2, exactly as before;
4 and 5 are yours to opt into. Nothing about an Old 3DS's limits changed.

> **⚠ CORRECTION [2026-09-03]** — "A New 3DS still starts at 2" was true when this release
> shipped and is false as of commit `bc6ddbd`: `RENDER_DIST_DEFAULT_NEW` in
> `source/scene/render_dist.h` moved from 2 to 3. The Old 3DS default is unchanged at 1.

The chunk mesh pool is what sets that ceiling, and it is now claimed at startup for
the widest distance the console will ever be offered rather than at a size fixed when
the game was compiled — 9,199,616 bytes at distance 3 against 46,948,352 at distance
5, out of a linear heap of 33,554,432 bytes on an Old 3DS and 67,108,864 on a New one.
That is why the ceilings are where they are and not one step higher.

## Move your SD card between consoles safely

A settings file written by a New 3DS can carry a render distance of 5. Put that card
in an Old 3DS and the value is now quietly brought down to 3 on load, instead of being
handed to a console that cannot hold it.

## Other players fade like the world does

A distant player used to be drawn at full brightness against a hazed-out world and
stood out like a sticker. They now fade into the distance the same way the blocks
around them do.

## Fixed: chunks could go permanently missing

The queue holding not-yet-built chunk meshes was sized for the old limit and could
refuse work during the first big load of a world. A refused chunk was never asked for
again — it left a hole in the terrain that stayed there, with nothing on screen and
nothing in any log to say why.

The queue is now sized *from* the limit rather than set beside it, and the build fails
outright if the two ever drift apart again. The relight queue was undersized in the
same way; it was caught by a test the mesh queue did not have, and now has one too.

## Compatibility

Worlds, saves and multiplayer are untouched. Nothing in the world format, the
generator or the network protocol changed, so a 1.8.5 client and a 1.8.4 client see
the same world and can play together.

Your existing settings file is read as-is. The only value that can change is the
render distance, and only on an Old 3DS reading a file a New 3DS wrote.

## Not verified

Nothing in this release has run on real 3DS hardware. Distance 5 draws 2.47× the
columns distance 3 does, and whether a New 3DS holds frame rate at the top of its own
ceiling is unknown — every memory and timing figure quoted above is arithmetic off the
built binary or a reading from the emulator, not from a console.
