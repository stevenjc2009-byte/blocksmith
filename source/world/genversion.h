// v1.7.0 task 15 prerequisite. Which terrain generator a world was made with.
//
// ── Why this file exists, before anything else in the rung ────────────────────────────
//
// Terrain is not stored. It is generated on the console from the world's seed, and only
// *edits* are saved (world/region.h). So the generator is not an implementation detail of
// the build — it is part of the world's identity. Replace it and every world that already
// exists comes back a different shape underneath the player's buildings: floating, buried,
// or cut in half, silently, on an update nobody opted into.
//
// The decision (roadmap, "Answered 2026-08-24", option b) is to **version the generator per
// world**. A world stamps which generator made it, and that stamp — not the build — decides
// what gets generated for it, forever.
//
// **This had to land before the new generator could produce a saved world**, or the first
// world made with it would be unversionable: there would be no way afterwards to tell it
// apart from a legacy one, and no way to get its terrain back.
//
// ── The rule, in one place ────────────────────────────────────────────────────────────
//
//   * A world directory with **no stamp** is a world that predates versioning — which is
//     every world in existence on 2026-08-24 — and loads as GEN_VERSION_LEGACY. It keeps
//     exactly the terrain it has today.
//   * A world directory with a **valid stamp** loads as that generator.
//   * A world stamped with a version **this build does not know** is REFUSED, not guessed
//     at. Generating a world at the wrong version is indistinguishable from the corruption
//     it would cause, so a save written by a newer build must stop the load rather than
//     quietly come back wrong.
//   * A stamp that is **present but unreadable** — bad magic, bad CRC, truncated — is also
//     refused, for the same reason: a stamp was intended here, and the one thing we must not
//     do is fall back to a *different* generator than the one that shaped this world.
//   * A world with **no stamp, that cannot be given one** — the card refused the write — is
//     refused as well, but **only when it is a brand new world**. That is the one case where
//     the derived answer does not survive: the world is about to save its first region file,
//     and from the next boot on the absent stamp beside that file derives LEGACY for a world
//     that is not legacy. A world that already has a region file derives LEGACY either way,
//     so its stamp is best effort and a refused write there changes nothing.
//
// The asymmetry between "absent" and "damaged" is the whole design. Absent is a fact about
// history (versioning did not exist yet) and has exactly one correct answer. Damaged is a
// fact about this card, and has no correct answer, so it stops.
//
// ── The residual risk, stated rather than hidden ──────────────────────────────────────
//
// If a stamp file is **deleted** (not corrupted) from a world made by a new generator, that
// world is indistinguishable from a legacy one and will come back with legacy terrain. There
// is no redundancy that fixes this without storing the stamp somewhere the legacy format
// also has, which by definition it does not. A player would have to delete one file out of a
// world directory by hand over USB to reach it.
//
// Nothing here includes <3ds.h>. Same reasoning as scene/worldlist.c and world/region.c: the
// SD card is a devoptab under "sdmc:/", so fopen/fread/opendir reach it on console exactly
// as they reach a host temp directory in the test build — which is what lets the host suite
// link this real file rather than a copy of its logic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── The versions themselves ───────────────────────────────────────────────────────────
//
// Append only, and never renumber: these values are written to players' SD cards. A value
// that has been shipped means one specific terrain shape for the rest of the game's life.

// v1.6.0 and earlier: the 2D value-noise heightmap with a separate 3D cave carve.
// **Every world that existed before 2026-08-24 is this one**, and reaches it by having no
// stamp at all rather than by carrying the number.
#define GEN_VERSION_LEGACY   1u

// v1.7.0: the Beta 1.7.3-style 5x5x17 density field, trilinearly interpolated.
#define GEN_VERSION_DENSITY  2u

// The newest generator this build can produce. A stamp above this is a world from the
// future and is refused — see genVersionResolve.
#define GEN_VERSION_NEWEST   GEN_VERSION_DENSITY

// What a new world made by this build gets stamped with.
#define GEN_VERSION_FOR_NEW_WORLDS GEN_VERSION_NEWEST

// ── The file ──────────────────────────────────────────────────────────────────────────

// Sits beside registry.bin in the world directory, and is written the same way: a tiny
// fixed-size sidecar rather than a new field in the region format, because the region format
// is per-region and this is per-world, and because touching a format that survives power
// cuts to add a field that never changes would be the larger and riskier change.
#define GEN_VERSION_FILE  "genver.bin"

// 4 magic + 2 version + 2 reserved + 4 CRC-32 over the first 8. Fixed, so a short read is
// itself a detectable fault rather than something to parse around.
#define GEN_VERSION_BYTES 12

typedef enum {
	// A generator this build can run. *out holds it.
	GENVER_OK = 0,

	// The world is stamped with a generator newer than this build knows. The world must NOT
	// be entered: its terrain cannot be reproduced here. *out holds the stamped value so the
	// caller can say what it saw.
	GENVER_TOO_NEW,

	// A stamp is present and cannot be trusted — wrong magic, failed CRC, or short. Refuse
	// for the same reason as TOO_NEW: something stamped this world and we cannot honour it.
	GENVER_DAMAGED,

	// No world directory at all (a server session, or an SD card that would not take one).
	// Not an error and not a refusal: there is nothing on this console to be consistent with.
	// *out is left at GEN_VERSION_LEGACY, which is what genVersionForSession() explains.
	GENVER_NO_WORLD_DIR,

	// A world that has no stamp and could not be given one: the card would not take the
	// write. Only ever returned for a world that has never been saved, because that is the
	// only case where the stamp has to land — see genVersionResolve for why the other one
	// stays best effort. *out still holds the generator this session would have used.
	//
	// A separate code rather than reusing GENVER_DAMAGED because the sentence the player
	// gets is a different sentence: a card that would not take the stamp is something they
	// can do something about — free some space, take the write-lock off, reseat it — and an
	// unreadable stamp is not. Folding the two together would cost them that.
	GENVER_STAMP_FAILED,

	// v1.8.3 Phase 4. A joined SERVER declared a generator this build cannot produce. The
	// session must NOT be entered: this client would generate a different landscape from the
	// one every other player in that world is standing on, and every block edit it made would
	// land in the wrong hillside. *out is left at GEN_VERSION_LEGACY — the wire value is
	// deliberately not adopted, because the whole point is that it is unusable here.
	//
	// A separate code rather than reusing GENVER_TOO_NEW, which is the same shape of fault one
	// module over. TOO_NEW's sentence is about a save file on the player's own SD card
	// ("world made by a newer version"), and pointing that at a server would send them looking
	// for a local world to delete. The fault is the server's world, and the fix is a newer
	// build of the game, so it gets its own sentence in world/genrefuse.h.
	GENVER_SESSION_MISMATCH,
} GenVersionStatus;

// Decides which generator `world_dir` gets, stamping it if it does not have one yet.
//
// The stamp for an unstamped directory is chosen by whether the world has ever been saved:
// a directory holding a region file (`*.bsr`) is a world that was played before versioning
// existed and is stamped GEN_VERSION_LEGACY; an empty one is brand new and is stamped
// GEN_VERSION_FOR_NEW_WORLDS. That single rule covers both ways a world directory comes into
// existence — the title screen's New World button and main.c's saveWorldDir() — without
// either of them having to know this file exists.
//
// The write is best-effort **for the legacy derivation only**: that answer was read off
// evidence that is still on the card next boot, so a refused write costs nothing and the
// status stays GENVER_OK. For a brand-new world it is not optional — its first save creates
// the very region file that would make the next boot answer LEGACY — so a refused write
// there returns GENVER_STAMP_FAILED. `*out` is always written, either way.
GenVersionStatus genVersionResolve(const char* world_dir, uint32_t* out);

// Reads the stamp without creating one. Exposed for the tests and for any caller that wants
// to know what a world says about itself without changing it.
GenVersionStatus genVersionRead(const char* world_dir, uint32_t* out);

// Writes `version` into `world_dir`, overwriting any existing stamp. False on any IO failure.
bool genVersionWrite(const char* world_dir, uint32_t version);

// Whether `world_dir` holds a saved region file — i.e. whether this world has ever been
// played. False for a directory that cannot be listed at all, which is the same answer an
// empty one gives and is deliberate: both mean "no evidence of a previous save here".
//
// v1.8.3 Phase 1. Exposed for world/worldseed.c, which has to make the identical
// absent-sidecar decision about the identical directory — see genVersionResolve's comment on
// the derivation, and worldseed.h's file comment for the mirrored rule. Two copies of this
// scan that were free to drift would let the generator stamp and the seed sidecar disagree
// about whether a world is old, which rewrites terrain under a world that has buildings in it.
bool genVersionWorldHasRegionFile(const char* world_dir);

// True if this build can generate `version` at all.
static inline bool genVersionKnown(uint32_t version)
{
	return version >= GEN_VERSION_LEGACY && version <= GEN_VERSION_NEWEST;
}

// Which generator a **joined server session** uses.
//
// A server session has no world directory and no stamp: the server sends only a seed
// (net/networld.h's BS_APP_WORLD_INFO) and the terrain is never transmitted, so every client
// generates the landscape for itself. Two clients on different generators standing in the
// same place would be standing in two different worlds — one player's floor is another
// player's sky, and every block edit lands in the wrong hillside.
//
// **So a session generates LEGACY**, and will keep doing so until the protocol can carry the
// server's generator version. That is deliberately the conservative direction: it is the
// generator every existing client and every existing server world already agrees on, so a
// v1.7.0 client joining steve's live server sees the terrain that is actually there.
//
// The alternative — generating the new terrain in a session — would silently split the world
// between old and new clients with no error anywhere, which is the exact harm the whole of
// this file exists to prevent, applied to the one world that has other people's buildings in
// it. Making it a named function rather than a bare constant at the call site is so that
// whoever adds the wire field has one place to change and this comment to read first.
static inline uint32_t genVersionForSession(void) { return GEN_VERSION_LEGACY; }

// v1.8.3 Phase 4. The wire field the comment above was waiting for now exists — the server
// declares its world's generator as BS_APP_WORLD_GEN (proto/bs_proto.h, 0x0F, one uint16 LE),
// and this is the decision that turns that number into an answer.
//
// genVersionForSession() above is KEPT, not replaced, and is still the whole answer for the
// `have_wire == false` row below. It is what a pre-Phase-4 server produces — it sends no
// WORLD_GEN at all — and legacy is exactly right for one: every client that has ever joined
// such a server generated legacy, so that is the terrain its diff store's coordinates point
// into. "Heard nothing" therefore means LEGACY and NOT "refuse", and that is a commitment,
// not an implementation detail: it is compiled into every client this phase ships, so a later
// phase that wanted silence to mean refusal could not tell the fielded ones.
//
//   have_wire == false                      -> GENVER_OK,               *out = LEGACY
//   have_wire, genVersionKnown(wire)        -> GENVER_OK,               *out = wire
//   have_wire, version unknown to this build-> GENVER_SESSION_MISMATCH, *out = LEGACY
//
// PURE, and deliberately so. It takes the two impure facts (did a WORLD_GEN arrive, and what
// did it say) as arguments instead of calling net/networld.h itself, following
// world/worldseed.h's worldSeedMintFrom(). world/genversion.c must not learn about net/: it is
// linked into host suites that do not link networld.c at all, and a call in here would drag the
// whole transport in behind it. Passing the facts in is also what lets a test drive every row
// above, refusals included, with no socket and no clock.
//
// A static inline in the header rather than a function in genversion.c, matching
// genVersionKnown() just above it: there is no state and no I/O here, only a total function
// over two arguments, and a header-only resolver can be exercised from any suite that includes
// this file rather than only from the one that links genversion.c.
//
// `*out` is always written, on every row, so a caller cannot read an uninitialised generator by
// forgetting to check the status. On the refusal row it is LEGACY rather than the wire value:
// the caller has no use for a version it cannot generate, and leaving the unusable number in an
// out-parameter named for the generator to use is how it eventually gets used.
static inline GenVersionStatus genVersionForSessionResolve(bool have_wire, uint32_t wire_version,
                                                           uint32_t* out)
{
	uint32_t dummy;
	if (!out) out = &dummy;

	if (!have_wire) {
		*out = genVersionForSession();
		return GENVER_OK;
	}

	if (!genVersionKnown(wire_version)) {
		*out = GEN_VERSION_LEGACY;
		return GENVER_SESSION_MISMATCH;
	}

	*out = wire_version;
	return GENVER_OK;
}
