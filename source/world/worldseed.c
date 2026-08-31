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

// Builds "<world_dir>/seed.bin". Answers false, and leaves `out` empty, when it does not fit.
//
// The truncation is REFUSED rather than performed, and that is the whole point: a truncated
// path is not a broken path, it is a perfectly openable name for a DIFFERENT file. This was
// measured on the unfixed code rather than argued, at cap 160:
//
//   dirlen  write  created-file  read-status  seed
//      150  true   seed.bin      OK           0xFEEDFACE     <- the last length that works
//      151  true   seed.bi       OK           0xFEEDFACE
//      155  true   see           OK           0xFEEDFACE
//
// Every one of those reported success. The writer and the reader truncate identically, so they
// agree with each other about the wrong name and a round-trip reads back perfectly — which is
// exactly why nothing caught it. Worse, past 159 the DIRECTORY half is cut too, and two world
// directories sharing their first 159 bytes become one world: writing 0xAAAAAAAA into A and
// then resolving B, which has never been stamped, answered WSEED_OK with 0xAAAAAAAA and B's
// own mint discarded. That is the landscape moving under a base the player has already built,
// which is the single harm this file exists to prevent, arriving as a success code.
//
// Widening `out` is not the fix and was rejected. It moves the cliff without removing it;
// there is no width at which "it fits" is a property of this code rather than of today's
// callers. It also gives up the diagnostic: -Wformat-truncation at level 1 — the level -Wall
// turns on — fires precisely when snprintf's result is UNUSED, i.e. when the code cannot tell
// whether it truncated, so USING the result is the fix the warning asks for. Same shape and
// same reasoning as 1a3c7e4's genverStampPath() one module over.
//
// An empty `out` is the refusal because it is the one name no fopen can mistake for a
// neighbour. It is a belt-and-braces second line only: all three callers below check the bool
// and stop before they ever open it. At cap 0 there is nowhere to put even the NUL, so nothing
// is written at all.
static bool pathFor(char* out, size_t cap, const char* world_dir)
{
	const int n = snprintf(out, cap, "%s/%s", world_dir, WORLD_SEED_FILE);
	if (n < 0 || (size_t)n >= cap) {
		if (cap) out[0] = '\0';
		return false;
	}
	return true;
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

	// A path that does not fit is a refused write, not a write somewhere else. Reported through
	// the return value the header already documents as "false on any IO failure", so
	// worldSeedResolve's brand-new branch turns it into WSEED_STAMP_FAILED and the world does
	// not open — rather than stamping a neighbouring file and reporting success.
	char path[160];
	if (!pathFor(path, sizeof path, world_dir)) return false;

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

	// THE SILENT FAILURE THIS FUNCTION USED TO HAVE, and the reason the refusal cannot simply
	// fall through to the fopen below. A truncated path that does not happen to open would
	// reach the `if (!f)` branch and be reported as WSEED_OK / "no sidecar" — a fault answered
	// as a fact about history, which main.c then turns into WORLD_SEED_LEGACY for a world that
	// may have a minted seed. A truncated path that DOES open is worse still: it reports some
	// other file's seed as this world's. Neither is a question this function can answer, so it
	// stops instead.
	//
	// WSEED_DAMAGED rather than a status of its own: the enum's refusals are routed by
	// world/genrefuse.h's switch, that file is not this commit's to edit, and DAMAGED is the
	// one that already means "a seed was intended here and cannot be trusted, do not enter the
	// world" — which is precisely the situation. *out stays at WORLD_SEED_LEGACY, as the header
	// documents for DAMAGED: the status is the answer, not the value.
	char path[160];
	if (!pathFor(path, sizeof path, world_dir)) return WSEED_DAMAGED;

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
		// Refused here too, and not left to the two branches below. A path that does not fit
		// makes the probe unable to say whether a sidecar exists, and BOTH of the answers it
		// would otherwise fall through to are wrong for a world that has one: the played
		// branch stamps it LEGACY and the brand-new branch mints over it. Same status and
		// same reasoning as worldSeedRead's refusal above.
		char path[160];
		if (!pathFor(path, sizeof path, world_dir)) return WSEED_DAMAGED;
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
