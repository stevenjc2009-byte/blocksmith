#pragma once

// world/placeable.h — may this item be put down in the world as a block? (v1.8.16)
//
// steve's ask, verbatim: "make the apple an item, not a block, similar to Minecraft."
//
// THE DEFECT. There is no item registry distinct from the block registry in this game —
// world/inventory.h:33-55 says so in as many words, and `typedef BlockId ItemId` is the whole
// of it. Every id in world/registry.c is therefore BOTH a block and an inventory item, so an
// apple sitting in the hotbar could be PLACED as a solid one-metre cube of apple. Nine ids are
// wrong that way: the apple and the eight meats.
//
// WHY THIS IS A LIST HERE AND NOT A FLAG IN THE REGISTRY. Both of the obvious moves are closed:
//
//  * A REGISTRY FLAG does not fit. registry.h:37-43 spends bits 0-4 on REG_FLAG_*, and bits 5-7
//    are REG_SHAPE_MASK — a sixth flag at 1u<<5 collides with the shape field and trips the
//    _Static_assert at registry.h:67-69. Escaping that means widening BlockDef past 27 bytes,
//    which changes the DEFS packet AND the on-disk registry.bin format, and so drags in a
//    server release for what is a client-side refusal.
//
//  * CHANGING THE APPLE'S ROW is worse. registry.c:495-516 already spells out why the row must
//    stay SOLID/FULL_CUBE: world/block.h's blockDropsNothing() answers from the SHAPE, so a
//    CROSS apple would break and yield NOTHING — killing the v1.8.8 supply of apples from
//    leaves, which is the exact opposite of what was asked for one version earlier. Worldgen
//    also grows real apple cubes into oak and birch canopies (world/worldgen.c's treePut), so
//    the apple genuinely IS a block in the world; what it must stop being is a block the
//    PLAYER can put down.
//
// So: a small explicit list living outside the registry, which is not a new pattern invented
// here — world/survival.c's survivalFoodValue() already keeps per-id hunger values exactly this
// way, for exactly this reason. It moves the registry crc not at all and vendors nothing: this
// pair is NOT among the eleven files deps/blocksmith-server/tools/sync-world-sources.sh mirrors
// to the server, so no server release follows from it.
//
// SCOPE, stated plainly so a reader does not over-trust this. This is the CLIENT'S OWN place
// path and nothing else. The server does not validate which id a client asks to place
// (deps/blocksmith-server/game/validate.c's bsEditValid checks coordinates and `block >
// REG_ID_DYN_HI` and nothing more), and this client's inbound remote-edit path mirrors that
// same non-check (net/networld.c's editValid). A modified remote client can therefore still put
// an apple cube in a shared world, and this client will render it. Closing that needs a server
// change and was deliberately not made here.

#include <stdbool.h>

#include "world/inventory.h"   // ItemId

// True if `item` may be written into the world by the player's place button.
//
// The rule is "everything, except the short list of food", not the other way round, and that
// direction is load-bearing: a server-registered dynamic block (REG_ID_DYN_LO..HI) arrives with
// no local row and must stay placeable, so an allowlist would break every modded server the
// moment it shipped. BLOCK_AIR answers true here and is refused one step earlier by
// scene/interact.c's own empty-hand check, which is left exactly as it was.
bool itemIsPlaceable(ItemId item);
