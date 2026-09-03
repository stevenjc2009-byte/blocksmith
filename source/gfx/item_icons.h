// Item icons: the atlas slots that hold ITEM art rather than block faces.
//
// v1.8.16 IMP-ICONS. Until this file existed, an item's inventory icon was its block's TOP
// FACE — scene/ui.c's iconUv called blockFaceTex(id, FACE_TOP) and nothing else. For a cube
// that is right: dirt's icon should look like dirt. For a DROP it is wrong, and it is the
// defect steve reported: a raw porkchop drew as a full opaque 16x16 square of pink, because a
// block face is a square by definition and has no silhouette to have. Nine drops now have
// real item art on the sheet — item-shaped, alpha 0 around the edge — and this header is the
// only way to reach it.
//
// ── Why the ids live HERE and not in world/block.h's BTEX_* enum ─────────────────────────
//
// Two reasons, and the first one is a hard constraint rather than a preference.
//
// 1. SAFETY. These nine tiles carry real alpha-0 texels. The opaque terrain pass runs with
//    the alpha TEST off and the blend func at ONE/ZERO (scene/chunk_render.c), so a
//    transparent texel on a cube face does not vanish — it writes its RGB at full strength
//    over whatever was behind it. Putting these ids in BTEX_* would make them nameable by
//    world/registry.c, i.e. one table row away from being a block face. They are not in that
//    enum, so that row cannot be written. The UI is the only consumer, and it draws through
//    gfx/sprite.c, which arms SRC_ALPHA/ONE_MINUS_SRC_ALPHA per batch and runs after
//    chunk_render.c has already disabled the alpha test — no state change was needed there.
//
// 2. THE DRIFT GATE. world/block.h is one of the eleven files
//    deps/blocksmith-server/tools/sync-world-sources.sh mirrors into the server tree (see
//    its FILES array), and Makefile's check-world-drift target fails the console build the
//    moment the two copies differ. Adding a BTEX_* name would therefore have broken every
//    build until the server was re-synced AND re-released — for art the server has no opinion
//    about at all. source/gfx/ is not mirrored, so this header costs no server release.
//
// The same reasoning is why gfx/atlas_tiles.h's TILE_* enum does not name them either: that
// enum is checked one-for-one against BTEX_* by world/block_tiles_check.c
// (BS_BTEX_TILE_PAIR_COUNT == TILE_USED_COUNT), so a TILE_* name with no BTEX_* twin is a
// build failure by construction. TILE_USED_COUNT stays at 48 and still means exactly what it
// says: the number of BLOCK-FACE tiles. The nine icons are painted by tools/make_atlas.py,
// which drives the sheet from its own TILES list and needs no C enum to do it.
//
// ── Adding an icon ───────────────────────────────────────────────────────────────────────
//
// Append a painter and a TILES entry in tools/make_atlas.py (append only — that list is one
// seeded random stream consumed in order, so inserting re-rolls the art of every tile after
// the insertion point), add its slot constant below, add its case to itemIconTile(), and move
// ATLAS_PAINTED_SLOTS in world/atlas_uv_shader_test.c with its new fingerprint. Slots 57..62
// are free; 63 is ATLAS_TILE_MISSING and cannot be claimed.

#pragma once

#include "world/block.h"

// The nine item-art slots, 48..56, in tools/make_atlas.py's TILES order. Raw cuts first, then
// their cooked counterparts, so a pair is a fixed +4 apart exactly as the block ids are.
#define ITEM_ICON_APPLE           48
#define ITEM_ICON_RAW_PORKCHOP    49
#define ITEM_ICON_RAW_BEEF        50
#define ITEM_ICON_RAW_CHICKEN     51
#define ITEM_ICON_RAW_MUTTON      52
#define ITEM_ICON_COOKED_PORKCHOP 53
#define ITEM_ICON_COOKED_BEEF     54
#define ITEM_ICON_COOKED_CHICKEN  55
#define ITEM_ICON_COOKED_MUTTON   56

// "This item has no icon art" — the caller must fall back to the block face.
#define ITEM_ICON_NONE (-1)

// The atlas slot holding `item`'s icon art, or ITEM_ICON_NONE if it has none.
//
// ITEM_ICON_NONE is the answer for every block in the game and must stay that way: a cube's
// icon is its own top face, which is both correct and free. Only drops — things that exist in
// the bag but are not really cubes — get art here.
//
// Header-only and static inline rather than a gfx/item_icons.c. It is one switch with nine
// arms called once per visible slot, there is exactly one caller (scene/ui.c's iconUv), and
// keeping it out of a translation unit means the console link is unchanged and no build file
// had to move. A `default:` arm rather than an exhaustive list of BlockId is deliberate — the
// enum has ~43 rows and all but nine of them want the same answer, so listing them would be a
// second table to keep in step with world/block.h for no gain.
static inline int itemIconTile(BlockId item)
{
	switch (item) {
	case BLOCK_APPLE:           return ITEM_ICON_APPLE;
	case BLOCK_RAW_PORKCHOP:    return ITEM_ICON_RAW_PORKCHOP;
	case BLOCK_RAW_BEEF:        return ITEM_ICON_RAW_BEEF;
	case BLOCK_RAW_CHICKEN:     return ITEM_ICON_RAW_CHICKEN;
	case BLOCK_RAW_MUTTON:      return ITEM_ICON_RAW_MUTTON;
	case BLOCK_COOKED_PORKCHOP: return ITEM_ICON_COOKED_PORKCHOP;
	case BLOCK_COOKED_BEEF:     return ITEM_ICON_COOKED_BEEF;
	case BLOCK_COOKED_CHICKEN:  return ITEM_ICON_COOKED_CHICKEN;
	case BLOCK_COOKED_MUTTON:   return ITEM_ICON_COOKED_MUTTON;
	default:                    return ITEM_ICON_NONE;
	}
}
