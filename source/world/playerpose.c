// The single-player pose sidecar. See world/playerpose.h for the contract and the layout.
//
// Written against world/inventory.c line by line rather than against world/genversion.c,
// and the choice is not cosmetic. genversion.c states its own reason for having no
// tmp+rename dance: "the file is written once when the world is first opened and never
// again, so the torn-write window is one moment in a world's whole life rather than every
// save". A pose is written on every world close and on every lid-close, which is the exact
// opposite access pattern, so genversion's deliberate omission does not carry over.
// inventory.c already solves this problem — a small fixed-size record in the world
// directory, rewritten on every quit — and this is its protocol with a different payload.

#include "world/playerpose.h"

#include "world/crc32.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PLAYERPOSE_FILE_NAME "player.dat"

// "BSP1" — Blocksmith Player, format 1 — read as bytes little-endian. Same naming shape as
// world/inventory.c's "BSI1" and world/region.c's "BSR1", and a different letter from both
// for the reason inventory.c gives about its own: so the files can never be mistaken for
// one another even if a path ever got crossed.
#define PLAYERPOSE_MAGIC   0x31505342u
#define PLAYERPOSE_VERSION 1u

#define POSE_FIELDS      5
#define POSE_HDR_BYTES   12
#define POSE_FILE_BYTES  (POSE_HDR_BYTES + POSE_FIELDS * 4)   // 32

// Little-endian on both the ARM11 and the host, but written and read a byte at a time
// anyway — copied from world/inventory.c's put32/get32 rather than re-derived, since this
// is solving the exact same "a save file must load on whichever machine reads it" problem.
static void put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

// A float goes to and from its four bytes through memcpy into a uint32_t, never through a
// pointer cast: the cast is a strict-aliasing violation and this tree builds -Werror. Both
// machines are IEEE-754 little-endian, which is the same assumption net/bswire.h's
// bs_get_f32/bs_put_f32 already make on the wire — nothing new is being invented here, only
// written down.
static void putF32(uint8_t* p, float v)
{
	uint32_t u;
	memcpy(&u, &v, sizeof u);
	put32(p, u);
}

static float getF32(const uint8_t* p)
{
	const uint32_t u = get32(p);
	float v;
	memcpy(&v, &u, sizeof v);
	return v;
}

static bool posePath(char* out, size_t cap, const char* world_dir)
{
	return snprintf(out, cap, "%s/%s", world_dir, PLAYERPOSE_FILE_NAME) < (int)cap;
}

static bool tmpPath(const char* path, char* out, size_t cap)
{
	return snprintf(out, cap, "%s.tmp", path) < (int)cap;
}

// A NULL directory is the server session (main.c gates on the same variable inventorySave
// does, so a joined world writes nothing to this card) and an empty one is a caller that
// has lost track of which world it is in — both refuse rather than resolving to "./".
static bool dirUsable(const char* world_dir)
{
	return world_dir != NULL && world_dir[0] != '\0';
}

bool playerPoseSave(const PlayerPose* pose, const char* world_dir)
{
	if (!pose || !dirUsable(world_dir)) return false;

	char path[512], tmp[512];
	if (!posePath(path, sizeof(path), world_dir)) return false;
	if (!tmpPath(path, tmp, sizeof(tmp))) return false;

	uint8_t buf[POSE_FILE_BYTES];
	putF32(buf + 0x0C, pose->x);
	putF32(buf + 0x10, pose->y);
	putF32(buf + 0x14, pose->z);
	putF32(buf + 0x18, pose->yaw);
	putF32(buf + 0x1C, pose->pitch);

	// The crc covers the payload only and is stored above it, so computing it never has to
	// exclude itself — world/inventory.c's rule.
	put32(buf + 0x00, PLAYERPOSE_MAGIC);
	put32(buf + 0x04, PLAYERPOSE_VERSION);
	put32(buf + 0x08, crc32(buf + POSE_HDR_BYTES, sizeof(buf) - POSE_HDR_BYTES));

	FILE* f = fopen(tmp, "wb");
	if (!f) return false;

	const bool wrote_ok = fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);

	// fclose is the flush: this is what makes the bytes actually reach the card rather than
	// sitting in stdio's buffer when the rename below runs — same reasoning inventory.c and
	// app/options.c both give.
	if (fclose(f) != 0 || !wrote_ok) { remove(tmp); return false; }

	// Windows' rename() refuses to replace an existing destination, so the old file has to
	// go first — the same reason inventory.c, options.c and region.c all remove before they
	// rename. remove() failing because `path` does not exist yet (the very first save) is
	// expected and is not a failure of this function.
	remove(path);

	if (rename(tmp, path) != 0) return false;

	// The window this leaves — a power cut between the remove and the rename — is closed by
	// poseRecover below the next time anything tries to load this path.
	return true;
}

// Mirrors inventory.c's inventoryRecover / region.c's regionRecover: if the last save was
// cut between removing the old file and renaming the new one into place, `path` is gone and
// `path.tmp` is a complete, unopened replacement. Promoting it here means playerPoseLoad
// never has to tell "never saved" apart from "saved, then interrupted right after".
static void poseRecover(const char* path)
{
	char tmp[512];
	if (!tmpPath(path, tmp, sizeof(tmp))) return;

	FILE* t = fopen(tmp, "rb");
	if (!t) return;             // no interrupted save to recover
	fclose(t);

	FILE* real = fopen(path, "rb");
	if (real) { fclose(real); remove(tmp); return; }   // real file is fine; drop the leftover

	rename(tmp, path);
}

bool playerPoseLoad(PlayerPose* out, const char* world_dir)
{
	if (!out || !dirUsable(world_dir)) return false;

	char path[512];
	if (!posePath(path, sizeof(path), world_dir)) return false;

	poseRecover(path);

	FILE* f = fopen(path, "rb");
	if (!f) return false;   // the normal case for every world that existed before v1.7.1

	// One byte more than the record, so a file that is LONGER than 32 bytes is refused too.
	// inventory.c reads exactly its own length and so cannot tell a 68-byte file from a
	// 6800-byte one; world/genversion.c does check, and states why: "a file longer than the
	// record is as wrong as a short one". A pose file with anything appended to it was not
	// written by this code, and guessing what it means is how a format stops being one.
	uint8_t buf[POSE_FILE_BYTES + 1];
	const size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n != POSE_FILE_BYTES) return false;   // short, or trailing bytes

	if (get32(buf + 0x00) != PLAYERPOSE_MAGIC)   return false;
	if (get32(buf + 0x04) != PLAYERPOSE_VERSION) return false;

	const uint32_t stored   = get32(buf + 0x08);
	const uint32_t computed = crc32(buf + POSE_HDR_BYTES, POSE_FILE_BYTES - POSE_HDR_BYTES);
	if (stored != computed) return false;

	PlayerPose p;
	p.x     = getF32(buf + 0x0C);
	p.y     = getF32(buf + 0x10);
	p.z     = getF32(buf + 0x14);
	p.yaw   = getF32(buf + 0x18);
	p.pitch = getF32(buf + 0x1C);

	// The check net/networld.c deliberately does NOT do on the wire, and this is where the
	// difference is paid. networld.h's boundary is "decodes bytes" and it says so; this file
	// is this game's own, so this game validates it. A NaN reaching bodyInit() propagates
	// through every physics comparison as false — the player falls forever, with no error
	// anywhere and nothing on screen to say why. A checksum cannot catch it: a NaN is a
	// perfectly well-formed float and a torn write is not the only way to get one.
	const float f5[POSE_FIELDS] = { p.x, p.y, p.z, p.yaw, p.pitch };
	for (int i = 0; i < POSE_FIELDS; i++)
		if (!isfinite(f5[i])) return false;

	// And the one range the world actually has. world/world.h bounds y to 0..WORLD_HEIGHT-1
	// and says nothing equivalent about x or z — "world block coordinates are signed and
	// unbounded in x and z" — so no horizontal bound is invented here. The finiteness check
	// above plus playerPoseUnstick()'s pass over real blocks is what covers those.
	if (!(p.y >= 0.0f && p.y <= (float)(WORLD_HEIGHT - 1))) return false;

	// Armed last, after every field has been validated, so *out is never left half-written
	// on a refusal — the same discipline net/networld.c's applyPlayerState() states about
	// its own s_have_player_state.
	*out = p;
	return true;
}

void playerPoseUnstick(const World* w, PlayerPose* pose)
{
	if (!w || !pose) return;

	// floorf, not a cast. A cast truncates towards zero, so (int)-0.5f is 0 where the block
	// at -0.5 is block -1 — main.c's genColumnOf() carries the same two-step for the same
	// reason, and getting it wrong reads the wrong column on the negative side only, which
	// looks like "the world is fine until you walk west".
	const int bx = (int)floorf(pose->x);
	const int bz = (int)floorf(pose->z);
	const int by = (int)floorf(pose->y);

	// Only y moves, and it moves through worldStandingY() — the same function task 46 put on
	// the spawn path, for the same reason. x and z keep their fractional parts: a player
	// restored to the block centre when they logged out at its edge is a visible, pointless
	// teleport. Sub-block height is the one thing discarded, and correctly — a player
	// standing on a block has an integer feet y by construction.
	pose->y = (float)worldStandingY(w, bx, bz, by);
}
