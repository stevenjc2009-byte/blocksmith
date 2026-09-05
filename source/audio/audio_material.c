#include "audio/audio_material.h"

// One switch, grouped by material rather than by id order — the grouping is what makes "is
// X really wood" a one-glance answer instead of a table scan against world/block.h. Follows
// the task brief's suggested mapping exactly; the comments below record the handful of ids
// whose material is not obvious from the block's name alone, not a departure from it.
//
// `id` is a BlockId (uint8_t), not the anonymous enum world/block.h defines its constants
// in, so this switch is never checked for exhaustiveness against that enum and needs no
// case for every core id nor for the dynamic 0x80..0xFD range — the `default:` below is the
// only thing standing between an untaught id and SFX_MAT_GENERIC, which is exactly the
// silent-safe answer wanted for it.
SfxMaterial sfxMaterialOfBlock(BlockId id)
{
	switch (id) {
	// ── Stone: rock, ore-bearing rock, and the two things built out of rock ────────────
	case BLOCK_STONE:
	case BLOCK_COAL_ORE:
	case BLOCK_IRON_ORE:
	case BLOCK_GOLD_ORE:
	case BLOCK_REDSTONE_ORE:
	case BLOCK_LAPIS_ORE:
	case BLOCK_DIAMOND_ORE:
	case BLOCK_FURNACE:  // built from stone — world/registry.c's row textures five of its
	                      // six faces BTEX_STONE, the same tile plain stone wears
	case BLOCK_ICE:       // not rock, but a hard, dense underfoot the same way stone is,
	                       // and the task brief names it here rather than under dirt
		return SFX_MAT_STONE;

	// ── Wood: logs, planks, and the one thing whittled from a plank ───────────────────
	case BLOCK_WOOD:
	case BLOCK_PLANKS:
	case BLOCK_BIRCH_LOG:
	case BLOCK_BIRCH_PLANKS:
	case BLOCK_SPRUCE_LOG:
	case BLOCK_SPRUCE_PLANKS:
	case BLOCK_TORCH:  // a stick, not a plank, but "stick" has no material bucket of its
	                    // own here and the task brief groups it with wood
		return SFX_MAT_WOOD;

	// ── Dirt: loose, walkable ground with no living thing in it ───────────────────────
	case BLOCK_DIRT:
	case BLOCK_SAND:
	case BLOCK_SNOW:
		return SFX_MAT_DIRT;

	// ── Grass: grass itself, and every plant that grows out of it ─────────────────────
	case BLOCK_GRASS:
	case BLOCK_LEAVES:
	case BLOCK_BIRCH_LEAVES:
	case BLOCK_SPRUCE_LEAVES:
	case BLOCK_TALL_GRASS:
	case BLOCK_TALL_GRASS_TOP:
	case BLOCK_FERN:
	case BLOCK_DEAD_BUSH:
	case BLOCK_CACTUS:
	case BLOCK_POPPY:
	case BLOCK_DAISY:
	case BLOCK_BLUEBELL:
	case BLOCK_ORCHID:
	case BLOCK_APPLE:
		return SFX_MAT_GRASS;

	// ── Everything else: GENERIC ───────────────────────────────────────────────────────
	// BLOCK_AIR (never targetable, never walked "on") and BLOCK_WATER (not solid, and the
	// one liquid in the game) fall through here explicitly rather than by omission, so a
	// reader does not have to go check they were not simply forgotten. The eight food
	// blocks — BLOCK_RAW_PORKCHOP/BEEF/CHICKEN/MUTTON and their four BLOCK_COOKED_*
	// counterparts (world/block.h's v1.8.14/v1.8.15 spans) — are items wearing a block id
	// rather than terrain a player ever stands on, so they fall through too. And so does
	// every id this switch does not name, present or future, including the whole dynamic
	// range 0x80..0xFD a server can register that this client build has never heard of.
	case BLOCK_AIR:
	case BLOCK_WATER:
	default:
		return SFX_MAT_GENERIC;
	}
}
