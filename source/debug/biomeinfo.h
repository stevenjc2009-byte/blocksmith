// v1.8.8 debug feature: the biome the player is standing in, on the BOTTOM-screen debug
// readout that scene/ui.c's drawHudFont draws.
//
// ── Why this module exists at all ────────────────────────────────────────────────────
//
// Two things that did not exist anywhere in the tree before it:
//
//   1. A NAME for a biome. world/worldgen.h declares BiomeId and six enumerators and that
//      is the whole of it — no table, no string, nothing that turns BIOME_TAIGA into
//      "taiga". A readout needs one, and the only sane place for it is beside the enum's
//      consumers rather than inside the generator.
//   2. A way for DRAWING code to ask "which biome is this". worldgenBiomeAt() wants a
//      const WorldGen*, and the only one that exists is `static WorldGen s_gen` in
//      source/main.c:695. scene/ui.c cannot see it.
//
// ── THE PLACEHOLDER, AND EXACTLY WHAT IS UNWIRED ─────────────────────────────────────
//
// Point 2 is NOT SOLVED HERE and deliberately so: source/main.c is owned by another lane
// and this change may not touch it. So this module holds a pointer that main.c is expected
// to hand it, and until something does, debugBiomeRow() reports BIOME_ROW_UNWIRED rather
// than guessing. The row, its wording, its colour and its position on the bottom screen are
// all finished and tested; only the one line that supplies the world is missing.
//
// THE ROUTED CHANGE THIS NEEDS — one line in source/main.c, immediately after the
// worldgenInit(&s_gen, ...) call that sets the world up:
//
//     debugBiomeSetWorldGen(&s_gen);
//
// and, wherever the world is torn down on quit-to-title, the matching:
//
//     debugBiomeSetWorldGen(NULL);
//
// A POINTER, not a seed, and that is load-bearing rather than a style choice.
// worldgenInit does NOT store the seed it is given — it stores rngMix(seed ^ 'BLKS')
// (world/worldgen.c, in worldgenInit). So an accessor handing back `s_gen.seed` and a
// worldgenInit() call on this side would mix an already-mixed seed and classify against a
// completely different noise field. That failure is silent: every lookup still returns a
// valid BiomeId, the row still reads "biome desert", and it is simply the WRONG biome, with
// nothing on screen to say so. Passing the live WorldGen cannot express that bug.
//
// Storing a POINTER also means the row follows the world across a re-generation without
// anything being re-pushed. The pointer's target must outlive the debug readout, which
// `static WorldGen s_gen` does by construction.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world/worldgen.h"   // BiomeId, WorldGen, worldgenBiomeAt

// What the row says while the world is not wired up. Deliberately not an empty string and
// not a plausible-looking biome: an unwired readout has to be visibly unwired on the screen,
// or the first person to read it will believe the answer.
#define BIOME_ROW_UNWIRED "biome ?  unwired"

// "tundra", "taiga", "plains", "forest", "desert", "jungle". Anything outside
// 0..BIOME_COUNT-1 gives "?" rather than reading off the end of the table.
const char* debugBiomeName(BiomeId b);

// The live world, or NULL to forget it. See the routed-change note above.
void debugBiomeSetWorldGen(const WorldGen* g);

// Whether a world has been handed over — i.e. whether the row will say anything real.
bool debugBiomeKnown(void);

// Formats the readout row for block column (x, z) into `buf` and returns `buf`, so it can
// be used directly as a fontDraw argument. Always NUL-terminates. With no world wired up it
// writes BIOME_ROW_UNWIRED.
//
// Takes the position rather than reading a camera itself: scene/chunk_render.h's
// chunkRenderCamera() is behind <3ds.h> and this file is host-tested. The caller in
// scene/ui.c does that read.
const char* debugBiomeRow(int32_t x, int32_t z, char* buf, size_t cap);
