#include "world/genversion.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "world/crc32.h"

// Nothing here includes <3ds.h> — see genversion.h's file comment for why that matters.

// 'B','S','G','V'. Byte-wise rather than a packed uint32 so the file is endian-independent
// by construction; the two machines this runs on happen to agree, but a save format that
// only works because of that is a trap for whoever ports it.
static const uint8_t GENVER_MAGIC[4] = {'B', 'S', 'G', 'V'};

static void pathFor(char* out, size_t cap, const char* world_dir)
{
	snprintf(out, cap, "%s/%s", world_dir, GEN_VERSION_FILE);
}

bool genVersionWrite(const char* world_dir, uint32_t version)
{
	if (!world_dir || !*world_dir) return false;

	uint8_t buf[GEN_VERSION_BYTES];
	memcpy(buf, GENVER_MAGIC, 4);
	buf[4] = (uint8_t)(version & 0xFFu);
	buf[5] = (uint8_t)((version >> 8) & 0xFFu);
	buf[6] = 0;   // reserved, must be written zero and is not checked on read: a future
	buf[7] = 0;   // build may give these meaning without invalidating today's files.

	const uint32_t crc = crc32(buf, 8);
	buf[8]  = (uint8_t)(crc & 0xFFu);
	buf[9]  = (uint8_t)((crc >> 8) & 0xFFu);
	buf[10] = (uint8_t)((crc >> 16) & 0xFFu);
	buf[11] = (uint8_t)((crc >> 24) & 0xFFu);

	char path[160];
	pathFor(path, sizeof path, world_dir);

	// Written whole in one fwrite of a fixed 12 bytes. There is no double-buffering here the
	// way region.c has it, and deliberately not: the file is written once when the world is
	// first opened and never again, so the torn-write window is one moment in a world's whole
	// life rather than every save. A torn write leaves a short or bad-CRC file, which
	// genVersionRead reports as GENVER_DAMAGED and the caller refuses — the safe direction.
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
	// fclose can fail on a full card with the bytes still in the buffer, so its result is
	// part of the answer, not ignored.
	return (fclose(f) == 0) && ok;
}

GenVersionStatus genVersionRead(const char* world_dir, uint32_t* out)
{
	uint32_t dummy;
	if (!out) out = &dummy;
	*out = GEN_VERSION_LEGACY;

	if (!world_dir || !*world_dir) return GENVER_NO_WORLD_DIR;

	char path[160];
	pathFor(path, sizeof path, world_dir);

	FILE* f = fopen(path, "rb");
	if (!f) {
		// Missing, and on this console that is the only realistic reason fopen fails for a
		// read in a directory the game just created. Reported as OK/LEGACY rather than as a
		// fault because "no stamp" is not a fault — it is what every world made before
		// versioning existed looks like, and it has exactly one correct answer.
		return GENVER_OK;
	}

	uint8_t buf[GEN_VERSION_BYTES];
	const size_t got = fread(buf, 1, sizeof buf, f);
	// A file longer than the record is as wrong as a short one — nothing this build writes
	// produces it, so something else wrote here and the contents cannot be trusted.
	const bool trailing = (fgetc(f) != EOF);
	fclose(f);

	if (got != sizeof buf || trailing) return GENVER_DAMAGED;
	if (memcmp(buf, GENVER_MAGIC, 4) != 0) return GENVER_DAMAGED;

	const uint32_t want = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
	                      ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
	if (crc32(buf, 8) != want) return GENVER_DAMAGED;

	const uint32_t v = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8);
	*out = v;

	// Checked after the CRC on purpose: a garbled file must be reported as garbled, not as a
	// world from the future, because the two mean different things to the player.
	return genVersionKnown(v) ? GENVER_OK : GENVER_TOO_NEW;
}

// Whether this directory holds a saved region file, i.e. whether the world has ever been
// played. The same test main.c's worldHasBeenPlayed() makes, for a different reason: there it
// picks the loading screen's heading, here it decides whether an unstamped world is an old
// one that must keep its terrain or a new one that may have the new generator.
//
// region.c names its files r.<rx>.<rz>.bsr and nothing else in a world directory carries that
// extension, so the extension alone is the whole test.
//
// v1.8.3 Phase 1: no longer static. world/worldseed.c has to ask the identical question about
// the identical directory — is this world old enough that its seed must stay 1337 — and it is
// asked two lines apart from this one inside main.c's genStart(). A second copy of the scan
// would be free to drift, and the two disagreeing is precisely how a world gets stamped LEGACY
// by one and given a freshly minted seed by the other: terrain rewritten under a base the
// player has already built. Declared in genversion.h; there is no behaviour change here.
bool genVersionWorldHasRegionFile(const char* world_dir)
{
	DIR* d = opendir(world_dir);
	if (!d) return false;

	bool found = false;
	const struct dirent* e;
	while (!found && (e = readdir(d)) != NULL) {
		const char* dot = strrchr(e->d_name, '.');
		found = dot && strcmp(dot, ".bsr") == 0;
	}
	closedir(d);
	return found;
}

GenVersionStatus genVersionResolve(const char* world_dir, uint32_t* out)
{
	uint32_t dummy;
	if (!out) out = &dummy;

	if (!world_dir || !*world_dir) {
		*out = genVersionForSession();
		return GENVER_NO_WORLD_DIR;
	}

	// A stamp that exists decides, whatever it says — including deciding to refuse.
	uint32_t stamped = GEN_VERSION_LEGACY;
	{
		char path[160];
		pathFor(path, sizeof path, world_dir);
		FILE* f = fopen(path, "rb");
		if (f) {
			fclose(f);
			const GenVersionStatus st = genVersionRead(world_dir, &stamped);
			*out = stamped;
			return st;
		}
	}

	// No stamp. Which generator this world *already* has is a question about its history, and
	// the only evidence on the card is whether anything was ever saved for it.
	const bool played = genVersionWorldHasRegionFile(world_dir);
	const uint32_t chosen = played ? GEN_VERSION_LEGACY : GEN_VERSION_FOR_NEW_WORLDS;
	*out = chosen;

	const bool stamp_ok = genVersionWrite(world_dir, chosen);

	// **This is the line the whole prerequisite turns on**, and the two derivations want
	// opposite things from it, which is why the result is not thrown away for both:
	//
	//   * Derived LEGACY — a region file is present. Best effort, and that is right. The
	//     evidence that produced the answer is permanent: the next boot opens the same
	//     directory, finds the same .bsr beside the same absent stamp, and derives LEGACY
	//     again. A card that refuses the write costs this world nothing.
	//
	//   * Derived FOR_NEW_WORLDS — nothing has ever been saved here. The exact opposite,
	//     because the evidence is about to change. This session generates the new terrain,
	//     the player's first saved column creates a .bsr, and from the next boot on that
	//     same derivation reads that .bsr and answers LEGACY — different terrain under a
	//     base that is already built, silently, one boot later, with no error and nothing
	//     to recover from. Carrying on here is what arms that, so it is refused instead,
	//     and the only thing given up is a world that has never been played.
	//
	// Nothing in this file fsyncs — region.c:279 states that convention for the whole save
	// path ("Nothing in this file fsyncs — everything flushes with fflush()") and this file
	// keeps it. So `stamp_ok` means the C library and the card accepted the bytes, which is
	// weaker than the stamp having survived a power cut in the moment after. That gap is
	// left as it is on purpose: a torn stamp reads back as GENVER_DAMAGED and refuses,
	// which is the safe direction, and changing the house convention for one 12-byte file
	// is not this function's call to make. What is fixed here is only the silent case.
	if (!stamp_ok && !played) return GENVER_STAMP_FAILED;
	return GENVER_OK;
}
