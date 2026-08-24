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
// The write is best-effort: a card that refuses it still gets the right answer this session,
// and the next boot re-derives the same one from the same evidence. `*out` is always written.
GenVersionStatus genVersionResolve(const char* world_dir, uint32_t* out);

// Reads the stamp without creating one. Exposed for the tests and for any caller that wants
// to know what a world says about itself without changing it.
GenVersionStatus genVersionRead(const char* world_dir, uint32_t* out);

// Writes `version` into `world_dir`, overwriting any existing stamp. False on any IO failure.
bool genVersionWrite(const char* world_dir, uint32_t version);

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
