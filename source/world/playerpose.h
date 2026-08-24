// The player's pose, persisted per world, for single player.
//
// v1.7.1 task 46b. A server session already gets this for free: the server volunteers the
// pose in its join-time PLAYER_STATE and net/networld.h's networldSavedPose() replays it
// once, at world entry, into the Player main.c has just built. Single player had nothing,
// so every reload put the player back at the spawn column whatever they had walked to.
//
// This is the same shape with the card in place of the server, and deliberately the same
// contract:
//
//   * a pose is exactly five floats — x, y, z (feet position in blocks) and yaw, pitch
//     (radians), the same five networldSavedPose() carries and nothing else. Velocity is
//     not one of them, and the selected hotbar slot already lives in inventory.dat;
//   * "nothing saved" is a first-class answer, not an error. playerPoseLoad() returns false
//     and leaves *out untouched, and the caller keeps its own spawn choice — exactly what
//     networldSavedPose() does, and the reason both can be consulted from one block;
//   * every way of failing degrades to that same false. Missing file, short file, trailing
//     bytes, wrong magic, wrong version, bad CRC, a non-finite float, a y off the ends of
//     the world: one answer, because the rest of the game would otherwise have to know the
//     difference between eight things it would treat identically. world/inventory.c makes
//     the same argument about its own file and this is copied from it.
//
// The missing-file case is the normal one, not the exceptional one: every world already on
// steve's card was written before this existed and has no player.dat, and opening one of
// those has to produce byte-for-byte today's behaviour — worldStandingY() over
// worldgenHeight() at the spawn column.
//
// ── On-disk layout, <world_dir>/player.dat, 32 bytes, fixed ────────────────────────────
//
//   0x00  u32  magic     PLAYERPOSE_MAGIC ("BSP1")
//   0x04  u32  version   PLAYERPOSE_VERSION
//   0x08  u32  crc       crc32 over 0x0C to EOF (the twenty payload bytes)
//   0x0C  f32  x         feet position, blocks
//   0x10  f32  y         feet position, blocks
//   0x14  f32  z         feet position, blocks
//   0x18  f32  yaw       radians, 0 looks along -Z
//   0x1C  f32  pitch     radians, positive looks down
//
// The crc sits *before* the range it covers so computing it never has to exclude itself —
// world/inventory.c's rule, and world/region.c's before that. There is no shape field:
// inventory.dat needs one because its payload length is a compile-time constant that can
// change, and a pose is five floats forever. If a sixth field is ever added, `version`
// becomes 2 and a version-1 file is refused by the same check that refuses a corrupt one,
// which is correct, because the fallback is safe.
//
// Written and read a byte at a time through put32/get32 the way every other save file in
// this tree is, and the floats go through memcpy into a uint32_t rather than a pointer
// cast — the cast is a strict-aliasing violation and this tree builds -Werror.
#pragma once

#include <stdbool.h>

// For World (playerPoseUnstick reads it) and WORLD_HEIGHT (the y range the load refuses
// outside). world.h is types and macros only — net/blockdiff.c already includes it and
// links nothing from world/*.c — so this costs no dependency the console build did not
// already have.
#include "world/world.h"

typedef struct {
	float x, y, z;      // feet position, blocks
	float yaw, pitch;   // radians
} PlayerPose;

// Writes `pose` into <world_dir>/player.dat, via player.dat.tmp and a rename, the way
// world/inventory.c writes inventory.dat. False on a NULL/empty directory, a path that does
// not fit, or any stdio failure; a failed pose save is not retried and not reported,
// because the cost of losing one is landing at spawn.
bool playerPoseSave(const PlayerPose* pose, const char* world_dir);

// Reads it back. True only when the file was present, whole, this format, this version,
// checksum-clean, all five floats finite and y inside the world. False on every other
// answer and *out is left untouched, so the caller's own spawn choice stands.
bool playerPoseLoad(PlayerPose* out, const char* world_dir);

// Steps a restored pose up out of anything solid, and this is not optional — it is the
// whole of roadmap task 46 applied to task 46b's new input.
//
// A player who logs out standing at y=40 and comes back to find y=40 filled with solid is
// buried: physics.c has no un-stick, so every move snaps back, and back-face culling draws
// nothing around them — which is exactly what "the chunk I'm in doesn't get loaded in"
// looked like the first time. The routes are real: a generator-version change, a region
// file that failed its CRC and regenerated from the seed, or a falling block that landed
// after the pose was written.
//
// Only y moves. x and z keep their fractional parts — restoring a player to the block
// centre when they logged out at its edge is a visible, pointless teleport — and yaw and
// pitch are not touched at all.
//
// ⚠ The world must actually have the pose's column resident when this runs. worldGet()
// answers BLOCK_AIR for an absent chunk, so against unloaded terrain this returns the y it
// was given and silently does nothing — the failure would be invisible and would be task
// 46's bug back again. main.c handles that by centring the streaming ring on the restored
// column at genStart() and doing this after runLoadingScreen() has filled it.
void playerPoseUnstick(const World* w, PlayerPose* pose);
