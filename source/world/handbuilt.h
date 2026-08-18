// The hand-built area Phase 3 is judged against.
//
// Not terrain generation — that is a later phase. This is a deliberately shaped test
// piece: flat ground to spot seams on, an 8x8 checker of plateaus so every chunk
// border has a step across it, a stepped pyramid whose corners are where baked AO
// will show up, and a sunken pit for inside corners. All of it deterministic, so two
// runs are comparable.
//
// <3ds.h>-free like the rest of world/, so the tests can mesh it on the PC.
#pragma once

#include "world/world.h"

#define HANDBUILT_CHUNKS_X  4    // 64 blocks
#define HANDBUILT_CHUNKS_Z  4
#define HANDBUILT_CHUNKS_Y  2    // 32 blocks tall, which covers every surface here

#define HANDBUILT_BLOCKS_X  (HANDBUILT_CHUNKS_X * CHUNK_DIM)
#define HANDBUILT_BLOCKS_Z  (HANDBUILT_CHUNKS_Z * CHUNK_DIM)

// Surface height at (x, z): the y of the first air block above the ground.
int handbuiltHeight(int x, int z);

// Writes the area into the world. False if any block write was refused, which means
// the budget ran out — the caller should report that rather than draw a hole.
bool handbuiltFill(World* w);
