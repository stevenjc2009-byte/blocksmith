#pragma once

// The block-to-material mapping used to pick which footstep/break/place clip plays.
//
// This is the ONE file in the audio module allowed to know both "what block is this" and
// "what does it sound like underfoot" — everywhere else, audio_sfx.h/.c work in SfxMaterial
// only and never see a BlockId, and world/block.h itself carries no audio field at all.
//
// That split is not stylistic, it is forced. world/block.h, world/registry.h and
// world/registry.c are byte-mirrored into deps/blocksmith-server by
// deps/blocksmith-server/tools/sync-world-sources.sh and guarded by a drift check — any
// change to one of those files forces a coordinated client+server release, because the
// server compiles and runs them too. Putting a "material" byte on BlockInfo (world/block.h)
// or on a registry row (world/registry.h/.c) would have meant exactly that coordinated
// release for a fact the server has no use for whatsoever: which noise a client's own
// speaker makes when a foot lands on a block. So the mapping lives here instead, in a file
// that is client-only and reachable from nowhere the server tree touches, and it READS the
// frozen ids world/block.h already exposes rather than adding anything to that header.
//
// v1.8.19 "per-material sound". Four materials plus a fallback — this is deliberately a
// coarse bucket, not one row per block: the goal is "does this sound like rock, wood, loose
// ground or foliage", not a distinct clip per block type, and four clips per event (twelve
// total — see audio_sfx.h) is what tools/make_sounds.py's asset lane was asked to produce.
#include "world/block.h"

// SFX_MAT_GENERIC is both a real material (the fallback every unmapped id resolves to) and
// the name of the pre-1.8.19 sound: audio_sfx.h's SFX_FOOTSTEP/SFX_BLOCK_BREAK/
// SFX_BLOCK_PLACE slots are what a GENERIC material plays, and what every other material
// falls back to when its own clip was never registered (see audio_sfx.h's three resolvers).
//
// Order is arbitrary. Nothing indexes this enum by arithmetic the way SfxSlot in
// audio_sfx.h has to (that one IS a table index into a fixed array); this one is only ever
// switched on.
typedef enum {
	SFX_MAT_GENERIC = 0,
	SFX_MAT_STONE,
	SFX_MAT_WOOD,
	SFX_MAT_DIRT,
	SFX_MAT_GRASS,
	SFX_MAT_COUNT,
} SfxMaterial;

// Pure: no state, no I/O, the same answer for the same id every call. Never returns
// anything outside the enum above, and a `default:` case in the .c file answers
// SFX_MAT_GENERIC for every id the switch does not name — which is what makes an
// unrecognised id (a dynamic 0x80..0xFD row a server registered that this client build has
// never heard of, or a future core id nobody has taught this switch about yet) silent-safe
// instead of undefined: it falls back to the generic sound instead of reading past a table
// or needing its own bug report.
SfxMaterial sfxMaterialOfBlock(BlockId id);
