// Host tests for the master block registry (v1.6.0 Phase A).
//
// Own main(), like every other suite binary: a broken registry must not stop the
// world suite from running, and two mains cannot share one link. The registry is
// plain C with no <3ds.h>, so all of it is provable here in seconds.
//
// What each probe guards:
//   testRegistryRoundTrip    - register/find/duplicate/full-range/unknown-id contract
//   testRegistryCoreIdsStable- ids 1..9 byte-identical after InitCore; every saved
//                              world and replay depends on these never moving
//   testRegistryCrcStability - same defs -> same crc16, different defs -> different
//                              crc16, plus a pinned golden for the core-only table
//   testRegistrySidecar      - registry.bin round-trip restores dynamic rows exactly
//
// The __3DS__ guard is load-bearing, not tidy — the console Makefile globs every .c
// under source/world, and this file's main() would collide with source/main.c's.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/registry.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL   %s\n", what);
	} else {
		printf("  ok     %s\n", what);
	}
}

// A dyn def with a distinct name and stone faces everywhere, solid. Returns the
// packed def rather than registering it, so tests control when/whether that happens.
static BlockDef makeDef(const char *name)
{
	BlockDef d;
	memset(&d, 0, sizeof d);
	snprintf(d.name, sizeof d.name, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) d.tex[f] = BTEX_STONE;
	d.flags      = REG_FLAG_SOLID;
	d.hardness   = 1;
	d.variant_of = 0;
	return d;
}

static void testRegistryRoundTrip(void)
{
	puts("registry: register / find / duplicate / full range / unknown id");

	registryInitCore();
	// Nine core rows since roadmap tasks 17 and 19: grass..planks are the seven that
	// also carry ITEM ids, plus water (0x08) and tall grass (0x09), which are core
	// blocks and deliberately NOT items — world/block.h records why BLOCK_COUNT
	// stayed at 8 while the registry's row count moved to 10.
	check(registryCount() == 10, "a fresh table defines exactly air + the nine core blocks");
	check(registryFind("grass") == BLOCK_GRASS, "core rows are findable by name");

	// Five dynamic registrations land on consecutive ids from REG_ID_DYN_LO up.
	BlockId ids[5];
	for (int i = 0; i < 5; i++) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "probe_%d", i);
		BlockDef d = makeDef(name);
		ids[i] = registryRegister(&d);
	}
	bool consecutive = true;
	for (int i = 0; i < 5; i++)
		if (ids[i] != (BlockId)(REG_ID_DYN_LO + i)) consecutive = false;
	check(consecutive, "registrations take the lowest free dyn ids in order");

	for (int i = 0; i < 5; i++) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "probe_%d", i);
		check(registryFind(name) == ids[i], "find-by-name returns the registered id");
	}

	BlockDef dup = makeDef("probe_2");
	check(registryRegister(&dup) == 0, "a duplicate name is refused with 0");

	const BlockDef *got = registryGet(ids[3]);
	check(got != NULL && strcmp(got->name, "probe_3") == 0,
	      "registryGet hands back the def that was registered");
	check(got->flags & REG_FLAG_SOLID, "the registered flags survive the round trip");

	// The never-NULL contract blockInfo() has always had: an unknown id answers
	// air instead of crashing whoever reads it.
	const BlockDef *unknown = registryGet((BlockId)0xC7);
	check(unknown != NULL && strcmp(unknown->name, "air") == 0,
	      "an unregistered dyn id answers the air def, never NULL");
	check(!blockIsSolid(0xC7), "and it is not solid, so meshing/collision treat it as a hole");
	check(strcmp(blockInfo((BlockId)0xFE)->name, "air") == 0,
	      "the reserved 0xFE answers air too");

	// Fill the whole dyn range: exactly 126 more registrations fit, then 0.
	int placed = 0;
	for (;;) {
		char name[REGISTRY_NAME_MAX];
		snprintf(name, sizeof name, "full_%03d", placed);
		BlockDef d = makeDef(name);
		BlockId id = registryRegister(&d);
		if (id == 0) break;
		placed++;
	}
	check(placed == REG_ID_DYN_HI - REG_ID_DYN_LO + 1 - 5,
	      "the dyn range accepts exactly its remaining capacity, then refuses");
	check(registryCount() == 1 + 9 + (REG_ID_DYN_HI - REG_ID_DYN_LO + 1),
	      "count reflects every defined row once the range is full");
}

static void testRegistryCoreIdsStable(void)
{
	puts("registry: core ids 1..9 are identical after InitCore");

	registryInitCore();

	// These ten checks ARE the save-format guarantee: a region file written by
	// any earlier build decodes by raw id, so if grass ever stops being 1 every
	// old world silently re-textures. Names first, then the texture rows.
	static const char *const want_names[] = {
		"air", "grass", "dirt", "stone", "sand", "wood", "leaves", "planks",
		"water", "tall_grass",
	};
	for (BlockId id = 0; id < 10; id++) {
		char what[64];
		snprintf(what, sizeof what, "id %u is still \"%s\"", (unsigned)id, want_names[id]);
		check(strcmp(registryGet(id)->name, want_names[id]) == 0, what);
	}

	const BlockDef *grass = registryGet(BLOCK_GRASS);
	check(grass->tex[FACE_TOP] == BTEX_GRASS_TOP && grass->tex[FACE_BOTTOM] == BTEX_DIRT
	      && grass->tex[FACE_EAST] == BTEX_GRASS_SIDE,
	      "grass keeps top/side/bottom tiles exactly as block.c defined them");
	const BlockDef *wood = registryGet(BLOCK_WOOD);
	check(wood->tex[FACE_TOP] == BTEX_WOOD_TOP && wood->tex[FACE_EAST] == BTEX_WOOD_SIDE,
	      "wood keeps end/side tiles");
	check((registryGet(BLOCK_LEAVES)->flags & REG_FLAG_TRANSPARENT) != 0,
	      "leaves stay transparent");
	check((registryGet(BLOCK_STONE)->flags & REG_FLAG_SOLID) != 0,
	      "stone stays solid");
	check(!(registryGet(BLOCK_AIR)->flags & REG_FLAG_SOLID),
	      "air stays non-solid");

	// Water (0x08) and tall grass (0x09), roadmap tasks 17 and 19. Pinned here for
	// the same reason as the rows above: they are core ids now, so a saved world
	// stores them as raw 8 and 9 and they can never move.
	const BlockDef *water = registryGet((BlockId)BLOCK_WATER);
	check(!(water->flags & REG_FLAG_SOLID),
	      "water is not solid: the player walks through it and it occludes nothing");
	check((water->flags & REG_FLAG_TRANSPARENT) != 0,
	      "water is transparent, which is what puts it in the mesher's deferred pass");
	check((water->flags & REG_FLAG_LIQUID) != 0,
	      "water is a liquid, which is what makes the crosshair refuse to target it");
	check(regShapeOf(water->flags) == BLOCK_SHAPE_FULL_CUBE,
	      "water is a full cube, so it culls against its own kind face-for-face");
	check(water->tex[FACE_TOP] == BTEX_WATER && water->tex[FACE_EAST] == BTEX_WATER
	      && water->tex[FACE_BOTTOM] == BTEX_WATER,
	      "water wears the same tile on every face");

	const BlockDef *tg = registryGet((BlockId)BLOCK_TALL_GRASS);
	check(!(tg->flags & REG_FLAG_SOLID),
	      "tall grass is not solid: walked through, and it casts no AO");
	check((tg->flags & REG_FLAG_TRANSPARENT) != 0,
	      "tall grass is transparent, so its cutout texels are alpha-tested away");
	check(!(tg->flags & REG_FLAG_LIQUID),
	      "tall grass is not a liquid, so the crosshair CAN target it");
	check(regShapeOf(tg->flags) == BLOCK_SHAPE_CROSS,
	      "tall grass is the first BLOCK_SHAPE_CROSS block in the game");
	check(tg->tex[FACE_TOP] == BTEX_TALL_GRASS && tg->tex[FACE_EAST] == BTEX_TALL_GRASS,
	      "tall grass wears its own tile; emitCross reads the east rect");
}

static void testRegistryCrcStability(void)
{
	puts("registry: crc16 is deterministic, content-sensitive, and pinned for core");

	registryInitCore();
	uint16_t base = registryCrc16();
	check(base != 0, "the core table produces a non-zero crc");
	check(base == registryCrc16() && base == registryCrc16(),
	      "repeated calls over an unchanged table agree");

	// Pinned golden for the core-only table. If this ever moves WITHOUT a
	// deliberate core-def edit, something redefined history behind the saved
	// worlds' backs. (Computed from this exact kCoreDefs layout.)
	//
	// Moved 0x7E5B -> 0x72A8 on 2026-08-23 by roadmap tasks 17 and 19, which append
	// water (0x08) and tall grass (0x09) to kCoreDefs. Deliberate core-def edit, so
	// the pin moves with it. The cost of moving it is real and is recorded here: a
	// client on this build joining a server still built with eight core rows sees
	// registryMatchesInfo() fail on both count and crc, falls into the bounded
	// REGISTRY_FETCH retry, and finishes the session with s_reg_synced false.
	// Degraded, not fatal — and it clears the moment the server ships the same rows.
	check(base == 0x72A8u,
	      "core-only crc matches the pinned golden 0x72A8");

	// Content sensitivity: one extra def must move the crc, and re-init must
	// put it back - proving the crc covers table content, not process state.
	BlockDef d = makeDef("crc_probe");
	BlockId id = registryRegister(&d);
	check(id != 0 && registryCrc16() != base, "registering a def changes the crc");

	registryInitCore();
	check(registryCrc16() == base, "re-init restores the core-only crc exactly");
}

static void testRegistrySidecar(void)
{
	puts("registry: sidecar round-trip restores dynamic rows exactly");

	static const char *const path = "build-host/registry_sidecar_probe.bin";
	registryInitCore();

	BlockDef a = makeDef("side_a");
	a.luminance = 5;
	BlockId id_a = registryRegister(&a);
	BlockDef b = makeDef("side_b");
	b.flags |= REG_FLAG_LUMINOUS;
	BlockId id_b = registryRegister(&b);
	check(id_a == REG_ID_DYN_LO && id_b == REG_ID_DYN_LO + 1,
	      "two defs registered before saving");

	uint16_t before_crc = registryCrc16();
	check(registrySidecarSave(path), "sidecarSave writes the file");

	registryInitCore();
	check(registryFind("side_a") == 0, "after re-init the dyn rows are gone");
	check(registrySidecarLoad(path), "sidecarLoad reads it back");
	check(registryFind("side_a") == id_a && registryFind("side_b") == id_b,
	      "both rows come back under their original ids");
	check(registryGet(id_a)->luminance == 5
	      && (registryGet(id_b)->flags & REG_FLAG_LUMINOUS) != 0,
	      "field-level content survives the round trip");
	check(registryCrc16() == before_crc, "the restored table hashes identically");

	remove(path);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	puts("== registry test ==");

	testRegistryRoundTrip();
	testRegistryCoreIdsStable();
	testRegistryCrcStability();
	testRegistrySidecar();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
