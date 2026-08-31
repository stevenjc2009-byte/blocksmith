// v1.8.3 Phase 1. Which world a single-player world IS.
//
// ── Why this file exists ──────────────────────────────────────────────────────────────
//
// Terrain is not stored (world/region.h) — it is generated from a seed, and only edits are
// saved. Until this file, that seed was one compiled-in constant, `BS_WORLD_SEED 1337` in
// source/main.c, for every single-player world ever created on every console. So "New World"
// produced the same landscape every time, and the only way to see different terrain was to
// rebuild the game with -DBS_WORLD_SEED=something.
//
// The multiplayer half of this problem was already solved and is NOT touched here. A server
// mints its own seed per world, persists it to <state-dir>/world_seed.txt, and sends it after
// join in BS_APP_WORLD_INFO; net/networld.h's networldWorldSeed() hands it to genStart(). A
// joined session has no world directory and nothing in this file runs for it.
//
// ── The rule, in one place, and it is genversion.h's rule ─────────────────────────────
//
// This file is written deliberately as a mirror of world/genversion.h, because it is the same
// question about the same directory: a per-world fact that is not recoverable from anything
// else on the card, so the absent case has to be decided once and never revisited.
//
//   * A world directory with a **valid seed sidecar** uses that seed.
//   * A sidecar that is **present but unreadable** — bad magic, bad CRC, short, long — is
//     REFUSED. It is the same reason genversion.h refuses a damaged stamp: a seed was
//     intended here, and generating from a *different* seed than the one that shaped this
//     world rewrites the landscape under the player's buildings with nothing to recover
//     from. There is no "probably 1337" answer that is safe to guess.
//   * A world with **no sidecar that has already been played** — a `.bsr` region file is
//     present — is a world made before per-world seeds existed, and every one of those was
//     generated from 1337. It gets WORLD_SEED_LEGACY, which is that constant, and the
//     sidecar is written best-effort. **This is the case that keeps every existing save
//     byte-identical**, and it is why the value below can never change.
//   * A world with **no sidecar that has never been played** is brand new. It gets a freshly
//     minted seed, and the write is NOT optional: the world is about to save its first
//     region file, and from the next boot on the absent sidecar beside that file derives
//     LEGACY for a world that is not legacy — different terrain under a base that is already
//     built, one boot later, silently. So a refused write there is a refusal.
//   * A mint that could not happen at all is a refusal too, for the same reason: falling
//     back to 1337 there would hand the player the one world this rung exists to stop
//     handing them, and would do it without saying so.
//
// The asymmetry between "absent-and-played" and "damaged" is genversion.h's asymmetry
// verbatim. Absent is a fact about history and has exactly one correct answer; damaged is a
// fact about this card and has none, so it stops.
//
// ── The residual risk, stated rather than hidden ──────────────────────────────────────
//
// A world created after this change but never saved — the player made it, walked around, and
// quit without a single column being written — has a sidecar but no `.bsr`. That is fine: the
// sidecar exists, so it decides. The exposed case is a world created between 82d4a1e and this
// change that has a genver.bin and no region file: it has no sidecar, looks brand new, and
// gets a minted seed rather than 1337. Its terrain changes. genversion.c accepted exactly this
// exposure for exactly this reason and named it in the same words — "the only thing given up
// is a world that has never been played" — and this file takes the same trade rather than
// inventing a second, different rule for the same directory.
//
// Nothing here includes <3ds.h>. Same reasoning as world/genversion.c: the SD card is a
// devoptab under "sdmc:/", so fopen/fread reach it on console exactly as they reach a host
// temp directory in the test build, which is what lets the host suite link this real file
// rather than a copy of its logic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── The legacy seed ───────────────────────────────────────────────────────────────────

// The seed every single-player world made before v1.8.3 was generated from, and therefore the
// seed every one of them must keep being generated from. This is source/main.c's
// BS_WORLD_SEED default written down a second time on purpose: BS_WORLD_SEED is overridable
// with -D for one-off instrumented builds (see the Makefile's EXTRA_CFLAGS note), and a value
// that has already been baked into players' saves must not move when somebody does that.
//
// **Never change this number.** It is not a tuning knob; it is a fact about worlds that
// already exist on cards.
#define WORLD_SEED_LEGACY 1337u

// ── The file ──────────────────────────────────────────────────────────────────────────

// Sits beside genver.bin in the world directory and is written the same way, for the same
// reason: per-world rather than per-region, so it is a tiny fixed-size sidecar rather than a
// new field in a format that survives power cuts.
#define WORLD_SEED_FILE  "seed.bin"

// 4 magic + 4 seed + 4 CRC-32 over the first 8. Fixed, so a short read is itself a detectable
// fault rather than something to parse around. Twelve bytes, the same as genver.bin, and that
// is arithmetic rather than imitation: genver spends its middle four on 2 version + 2 reserved
// and a seed needs all four.
#define WORLD_SEED_BYTES 12

typedef enum {
	// A usable seed. *out holds it.
	WSEED_OK = 0,

	// A sidecar is present and cannot be trusted — wrong magic, failed CRC, short or long.
	// The world must NOT be entered. *out is left at WORLD_SEED_LEGACY, which the caller
	// must not use: the status is the answer, not the value.
	//
	// Also returned when `world_dir` is so long that "<world_dir>/seed.bin" does not fit the
	// path buffer, because then this module cannot tell whether a sidecar is present at all.
	// It is the same answer for the same reason — a seed was intended here and cannot be
	// read, so it stops — and it is deliberately NOT the "absent" answer: reporting a path
	// that could not be built as "no sidecar" is a fault answered as a fact about the world's
	// history, and it resolves to WORLD_SEED_LEGACY for a world that may have a minted seed.
	// See world/worldseed.c's pathFor() for the measurement. Not a separate enumerator only
	// because adding one is a world/genrefuse.h change, which that commit did not own.
	WSEED_DAMAGED,

	// No world directory at all (a joined server session). Not an error and not a refusal:
	// there is no sidecar on this console to be consistent with, and the seed comes off the
	// wire instead. *out is left at WORLD_SEED_LEGACY and the caller keeps whatever it had.
	WSEED_NO_WORLD_DIR,

	// A brand-new world, and no seed could be minted for it — every entropy source the
	// console offers refused. Refused rather than defaulted, because the default is 1337 and
	// silently handing the player the one world this rung exists to replace is the failure
	// mode, not the fallback.
	WSEED_MINT_FAILED,

	// A brand-new world with a minted seed the card would not take. Only ever returned for a
	// world that has never been saved — see the file comment for why the played case stays
	// best effort. *out holds the seed that would have been used.
	//
	// A separate code from WSEED_DAMAGED because the sentence the player gets is a different
	// sentence, exactly as genversion.h argues for GENVER_STAMP_FAILED: a card that would not
	// take the write is something they can act on, an unreadable file is not.
	WSEED_STAMP_FAILED,
} WorldSeedStatus;

// ── Minting ───────────────────────────────────────────────────────────────────────────

// The whole mint except where the entropy comes from, so that the entropy-refused branch is
// reachable from a host test instead of being three lines nothing can exercise.
//
// `have_wall` / `wall` is a wall-clock reading (seconds since the epoch on both platforms);
// `have_ticks` / `ticks` is a within-process monotonic reading. Either alone is enough. With
// NEITHER there is no entropy at all and this returns false — a counter on its own would give
// the first world of every boot the same seed on every console, which is the compiled-in
// constant again wearing a different hat.
//
// **Two calls with identical readings return different seeds**, and that is the load-bearing
// property, not a nicety: wall clock is whole seconds and two worlds created from the title
// screen thirty seconds apart can still land in the same second on a console with no
// sub-second clock. A per-process mint counter is folded in so that "New World" twice in a
// row cannot produce the same landscape twice.
bool worldSeedMintFrom(bool have_wall, uint32_t wall, bool have_ticks, uint32_t ticks,
                       uint32_t* out);

// worldSeedMintFrom() wired to this platform's clocks. time() and clock() only — no <3ds.h>,
// per the file comment.
bool worldSeedMint(uint32_t* out);

// ── The sidecar ───────────────────────────────────────────────────────────────────────

// Writes `seed` into `world_dir`, overwriting any existing sidecar. False on any IO failure.
bool worldSeedWrite(const char* world_dir, uint32_t seed);

// Reads the sidecar without creating one. WSEED_OK with *out at WORLD_SEED_LEGACY when the
// file is simply absent — "no sidecar" is not a fault, it is what every pre-v1.8.3 world looks
// like. Exposed for the tests and for any caller that wants to know what a world says about
// itself without changing it.
WorldSeedStatus worldSeedRead(const char* world_dir, uint32_t* out);

// Decides which seed `world_dir` gets, writing the sidecar if it does not have one.
//
// `mint_ok` / `mint` are the caller's freshly minted candidate, used ONLY when the world turns
// out to be brand new. It is passed in rather than minted here so that this function is a pure
// decision over the filesystem with no clock in it: the host suite can drive every branch
// deterministically, including the refusals, which is the whole reason the mint lives behind
// worldSeedMintFrom() above.
//
// `*out` is always written.
WorldSeedStatus worldSeedResolve(const char* world_dir, bool mint_ok, uint32_t mint,
                                 uint32_t* out);
