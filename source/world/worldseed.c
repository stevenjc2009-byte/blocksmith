#include "world/worldseed.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "world/crc32.h"
#include "world/genversion.h"
#include "world/rng.h"

// Nothing here includes <3ds.h> — see worldseed.h's file comment for why that matters.

// 'B','S','S','D'. Byte-wise rather than a packed uint32 so the file is endian-independent by
// construction, exactly as world/genversion.c argues for its own magic.
static const uint8_t WSEED_MAGIC[4] = {'B', 'S', 'S', 'D'};

static void pathFor(char* out, size_t cap, const char* world_dir)
{
	snprintf(out, cap, "%s/%s", world_dir, WORLD_SEED_FILE);
}

// ── Minting ───────────────────────────────────────────────────────────────────────────

bool worldSeedMintFrom(bool have_wall, uint32_t wall, bool have_ticks, uint32_t ticks,
                       uint32_t* out)
{
	// How many seeds this process has minted. The reason it exists is in worldseed.h: the
	// wall clock is whole seconds on both platforms, so two "New World" presses can share a
	// reading, and two worlds created in one sitting sharing a landscape is the exact defect
	// this rung is here to remove. rngMix is a bijection and multiplication by an odd
	// constant is a bijection, so distinct counter values give distinct seeds outright —
	// this is not a probabilistic decorrelation, it is a guaranteed one.
	static uint32_t s_nth = 0u;

	if (!out) return false;

	// No clock of any kind answered. Refused rather than fabricated: a counter on its own
	// restarts at the same place every boot, so the first world made after every power-on
	// would share a seed across every console in existence — which is BS_WORLD_SEED again
	// with extra steps, and silently.
	if (!have_wall && !have_ticks) return false;

	s_nth++;

	uint32_t h = rngMix(s_nth * 0x9E3779B1u);
	if (have_wall)  h = rngMix(h ^ (wall  * 0x85EBCA77u));
	if (have_ticks) h = rngMix(h ^ (ticks * 0xC2B2AE3Du));

	*out = h;
	return true;
}

bool worldSeedMint(uint32_t* out)
{
	// time() is the only wall clock reachable without <3ds.h>, and clock() is the only
	// monotonic one. Both are checked against their documented failure value rather than
	// assumed to work: on a console with a dead or unset RTC, time() answering -1 is a real
	// state, and the whole point of worldSeedMintFrom's first branch is to have an honest
	// answer for it.
	const time_t  t = time(NULL);
	const clock_t c = clock();
	return worldSeedMintFrom(t != (time_t)-1,  (uint32_t)t,
	                         c != (clock_t)-1, (uint32_t)c, out);
}

// ── The sidecar ───────────────────────────────────────────────────────────────────────

bool worldSeedWrite(const char* world_dir, uint32_t seed)
{
	if (!world_dir || !*world_dir) return false;

	uint8_t buf[WORLD_SEED_BYTES];
	memcpy(buf, WSEED_MAGIC, 4);
	buf[4] = (uint8_t)(seed & 0xFFu);
	buf[5] = (uint8_t)((seed >> 8) & 0xFFu);
	buf[6] = (uint8_t)((seed >> 16) & 0xFFu);
	buf[7] = (uint8_t)((seed >> 24) & 0xFFu);

	const uint32_t crc = crc32(buf, 8);
	buf[8]  = (uint8_t)(crc & 0xFFu);
	buf[9]  = (uint8_t)((crc >> 8) & 0xFFu);
	buf[10] = (uint8_t)((crc >> 16) & 0xFFu);
	buf[11] = (uint8_t)((crc >> 24) & 0xFFu);

	char path[160];
	pathFor(path, sizeof path, world_dir);

	// One fwrite of a fixed 12 bytes, no double-buffering, for world/genversion.c's stated
	// reason: the file is written once in a world's whole life, so the torn-write window is
	// one moment rather than every save, and a torn write reads back as WSEED_DAMAGED and
	// refuses — the safe direction.
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
	// fclose can fail on a full card with the bytes still in the buffer, so its result is
	// part of the answer, not ignored.
	return (fclose(f) == 0) && ok;
}

WorldSeedStatus worldSeedRead(const char* world_dir, uint32_t* out)
{
	uint32_t dummy;
	if (!out) out = &dummy;
	*out = WORLD_SEED_LEGACY;

	if (!world_dir || !*world_dir) return WSEED_NO_WORLD_DIR;

	char path[160];
	pathFor(path, sizeof path, world_dir);

	FILE* f = fopen(path, "rb");
	if (!f) {
		// Missing. Reported as OK/LEGACY rather than as a fault because "no sidecar" is not
		// a fault — it is what every world made before v1.8.3 looks like, and it has exactly
		// one correct answer. Deciding whether that answer is the right one for THIS
		// directory is worldSeedResolve's job, not this one's.
		return WSEED_OK;
	}

	uint8_t buf[WORLD_SEED_BYTES];
	const size_t got = fread(buf, 1, sizeof buf, f);
	// A file longer than the record is as wrong as a short one — nothing this build writes
	// produces it, so something else wrote here and the contents cannot be trusted.
	const bool trailing = (fgetc(f) != EOF);
	fclose(f);

	if (got != sizeof buf || trailing) return WSEED_DAMAGED;
	if (memcmp(buf, WSEED_MAGIC, 4) != 0) return WSEED_DAMAGED;

	const uint32_t want = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
	                      ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
	if (crc32(buf, 8) != want) return WSEED_DAMAGED;

	// Every 32-bit value is a legal seed, so there is no equivalent of genversion.h's
	// GENVER_TOO_NEW here and there must not be one: a seed carries no capability, so a
	// value this build has never seen is not a world it cannot generate.
	*out = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
	       ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
	return WSEED_OK;
}

WorldSeedStatus worldSeedResolve(const char* world_dir, bool mint_ok, uint32_t mint,
                                 uint32_t* out)
{
	uint32_t dummy;
	if (!out) out = &dummy;
	*out = WORLD_SEED_LEGACY;

	if (!world_dir || !*world_dir) return WSEED_NO_WORLD_DIR;

	// A sidecar that exists decides, whatever it says — including deciding to refuse. The
	// probe is a bare fopen rather than worldSeedRead's return value because "absent" and
	// "unreadable" are the same WSEED_DAMAGED-or-OK pair to a reader and completely
	// different questions here.
	{
		char path[160];
		pathFor(path, sizeof path, world_dir);
		FILE* f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return worldSeedRead(world_dir, out);
		}
	}

	// No sidecar. Whether this world already HAS a seed is a question about its history, and
	// the only evidence on the card is whether anything was ever saved for it.
	//
	// genVersionWorldHasRegionFile() rather than a copy of that scan, and the sharing is the
	// point: world/genversion.c asks the identical question about the identical directory two
	// lines earlier in main.c's genStart(). Two implementations that must agree and are free
	// to drift is how an old world ends up stamped LEGACY by one and minted a fresh seed by
	// the other — terrain rewritten under a build the player already has.
	if (genVersionWorldHasRegionFile(world_dir)) {
		*out = WORLD_SEED_LEGACY;

		// Best effort, and that is right — the same argument world/genversion.c makes above
		// its own return. The evidence that produced this answer is permanent: the next boot
		// opens the same directory, finds the same .bsr beside the same absent sidecar, and
		// derives LEGACY again. A card that refuses the write costs this world nothing. It is
		// still attempted, so the value stops being derived at all once it lands.
		(void)worldSeedWrite(world_dir, WORLD_SEED_LEGACY);
		return WSEED_OK;
	}

	// Brand new. Both of the failures below are refusals rather than fallbacks, and it is the
	// same reason twice: the fallback is 1337, and quietly generating the one landscape this
	// rung exists to stop generating — while telling the player nothing — is worse than not
	// opening the world at all. Nothing has ever been saved here, so refusing costs a world
	// that does not exist yet.
	if (!mint_ok) return WSEED_MINT_FAILED;

	*out = mint;

	// Not optional here, and this is the line the sidecar turns on. This session generates
	// from `mint`, the player's first saved column creates a .bsr, and from the next boot on
	// the branch above reads that .bsr and answers LEGACY — a different landscape under a
	// base that is already built, one boot later, with no error and nothing to recover from.
	//
	// Nothing in this file fsyncs; world/region.c states that convention for the whole save
	// path and world/genversion.c keeps it, so this does too. A torn write reads back as
	// WSEED_DAMAGED and refuses, which is the safe direction.
	if (!worldSeedWrite(world_dir, mint)) return WSEED_STAMP_FAILED;
	return WSEED_OK;
}
