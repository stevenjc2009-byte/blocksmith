#include "net/networld.h"

#include <string.h>

#include "net/blockdiff.h"
#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/block.h"

#include "proto/bs_proto.h"

// The World networldUpdate()/networldOnColumnLoad() are allowed to touch. NULL until
// networldSetWorld() is called, and also the reason a worker-thread call into
// networldOnColumnLoad() (see the header's threading note) is safely ignored: the worker
// thread never has this pointer, only its own private staging World.
static World* s_world;

// The whole point of this module: remote edits aimed at a column we have not streamed in yet,
// held here instead of being dropped, until networldOnColumnLoad() says that column exists.
static BlockDiffStore s_pending;

// netTransportRecv() (bsnet_transport.h) has no queue-depth contract of its own, so a very
// busy server — or a hostile one — handing back messages faster than one frame can drain them
// must not turn networldUpdate() into an unbounded loop on hardware with a 16.71 ms frame
// budget. 32 is comfortably above what the server's own 10 Hz tick (server/game/bsgame.c)
// produces for a full 16-player room in one frame.
#define NETWORLD_MAX_MSGS_PER_FRAME 32

// One packet's worth. Mirrors NET_MAX_PAYLOAD (bsnet_transport.h), which itself mirrors
// BS_MAX_PAYLOAD (bs_proto.h) — both already have to agree for the transport to work at all.
#define NETWORLD_RECV_BUF NET_MAX_PAYLOAD

// Mirrors server/game/validate.h's BS_WORLD_XZ_LIMIT: generous relative to any legitimate
// play area, tight enough to catch a coordinate near the edge of int32 range from a buggy or
// hostile peer before it ever reaches worldSet(), which only bounds y itself. This is defence
// in depth, not a shared implementation with the server's own check — server/ is not on the
// 3DS include path (see bsnet_transport.c's own header comment on the client/server split),
// so the two limits are two literals that happen to agree rather than one.
#define NETWORLD_XZ_LIMIT 60000

// One slot per tracked remote player. Deliberately not a World concern — see this file's own
// header comment — so this table lives and is aged independent of s_world above.
typedef struct {
	bool     occupied;
	uint32_t sid;
	float    x, y, z;
	float    yaw, pitch;
	uint64_t last_seen_ms;
} RemoteSlot;

static RemoteSlot s_remotes[NETWORLD_MAX_REMOTE];

// networldSendPose()'s own rate limiter. "sent_once" separates "never sent" from "sent at
// bsSockNowMs() == 0" — that clock's epoch is arbitrary (osGetTime() on the 3DS,
// CLOCK_MONOTONIC on the host test build), so 0 is a value it can legitimately return, not a
// sentinel for "unset".
static uint64_t s_last_pose_send_ms;
static bool     s_pose_sent_once;

// Every value here came off the wire, so every one of it is treated as hostile — same posture
// as server/game/validate.c's bsEditValid(), which this mirrors. y and block are checked
// against the client's own real world model (world/world.h's WORLD_HEIGHT, world/block.h's
// BLOCK_COUNT) rather than a second copy of either number.
static bool editValid(int32_t x, int32_t y, int32_t z, uint8_t block)
{
	if (x < -NETWORLD_XZ_LIMIT || x > NETWORLD_XZ_LIMIT) return false;
	if (z < -NETWORLD_XZ_LIMIT || z > NETWORLD_XZ_LIMIT) return false;
	if (y < 0 || y >= WORLD_HEIGHT)                       return false;
	if (block >= BLOCK_COUNT)                             return false;
	return true;
}

// The one place that decides "apply now or queue it": straight into the world when its column
// is already loaded, or handed to net/blockdiff.h when it is not. Deliberately not delegated
// to worldSet()'s own return value — worldSet() actually *will* allocate a fresh column for a
// non-air block (worldSet -> worldChunkCreate -> worldColumnCreate, world/world.c), which for
// a remote edit into territory we have not streamed in yet would create a column outside the
// streaming ring that app/worker.c's own install path (workerInstall -> worldChunkCreate)
// would then find already-there and never overwrite with real generated terrain — a hole
// dressed up as a bug report from a completely different session. Checking worldColumn()
// first avoids ever taking that path for a column we do not already own.
static void applyOrQueue(int32_t x, int32_t y, int32_t z, uint8_t block)
{
	if (!s_world) return;

	if (worldColumn(s_world, x >> 4, z >> 4) != NULL) {
		worldSet(s_world, x, y, z, block);
	} else {
		blockdiffRecord(&s_pending, x, y, z, block);
	}
}

static void applyBlockEdit(const uint8_t* msg, size_t len)
{
	if (len != BS_BLOCK_EDIT_BYTES) return;

	const int32_t x     = bs_get_i32(msg + 1);
	const int32_t y     = bs_get_i32(msg + 5);
	const int32_t z     = bs_get_i32(msg + 9);
	const uint8_t block = msg[13];

	if (!editValid(x, y, z, block)) return;
	applyOrQueue(x, y, z, block);
}

static void applyWorldSync(const uint8_t* msg, size_t len)
{
	if (len < BS_APP_HDR_BYTES + 2u) return;

	const uint16_t count = bs_get_u16(msg + 1);

	// The declared count must exactly explain the packet's own length, and independently
	// must not exceed the sender's own cap (bs_proto.h) — a payload that lies about either
	// is malformed rather than merely large, so it is dropped whole rather than parsed as
	// far as it happens to still make sense.
	if ((uint32_t)count > BS_SYNC_MAX_ENTRIES) return;
	if ((size_t)BS_WORLD_SYNC_BYTES(count) != len) return;

	const uint8_t* p = msg + BS_APP_HDR_BYTES + 2u;
	for (uint16_t i = 0; i < count; i++) {
		const int32_t x     = bs_get_i32(p);
		const int32_t y     = bs_get_i32(p + 4);
		const int32_t z     = bs_get_i32(p + 8);
		const uint8_t block = p[12];

		if (editValid(x, y, z, block)) applyOrQueue(x, y, z, block);
		p += BS_SYNC_ENTRY_BYTES;
	}
}

// The one place that decides "new player or existing one": a known sid updates in place; an
// unknown one takes the first free slot. A full table drops the newcomer rather than evicting
// someone — the server caps a room at BS_GAME_MAX_PLAYERS (16) anyway, so a full table here
// means something upstream is wrong, and silently replacing a real player would be worse.
static void upsertRemote(uint32_t sid, float x, float y, float z, float yaw, float pitch)
{
	const uint64_t now = bsSockNowMs();
	int free_slot = -1;

	for (int i = 0; i < NETWORLD_MAX_REMOTE; i++) {
		if (s_remotes[i].occupied && s_remotes[i].sid == sid) {
			s_remotes[i].x            = x;
			s_remotes[i].y            = y;
			s_remotes[i].z            = z;
			s_remotes[i].yaw          = yaw;
			s_remotes[i].pitch        = pitch;
			s_remotes[i].last_seen_ms = now;
			return;
		}
		if (!s_remotes[i].occupied && free_slot < 0) free_slot = i;
	}

	if (free_slot < 0) return;   // table full: drop, don't evict — see the comment above

	s_remotes[free_slot].occupied     = true;
	s_remotes[free_slot].sid          = sid;
	s_remotes[free_slot].x            = x;
	s_remotes[free_slot].y            = y;
	s_remotes[free_slot].z            = z;
	s_remotes[free_slot].yaw          = yaw;
	s_remotes[free_slot].pitch        = pitch;
	s_remotes[free_slot].last_seen_ms = now;
}

static void applyPosUpdate(const uint8_t* msg, size_t len)
{
	// Wrong length gets the player kicked server-side (bs_proto.h); here it is simply dropped
	// — same posture as applyBlockEdit()/applyWorldSync() above, never crash, never partially
	// apply. bsgame never sends the client its own sid (broadcast_except excludes the sender),
	// so every S->C POS_UPDATE this parses is, by definition, another player.
	if (len != BS_POS_UPDATE_S_BYTES) return;

	const uint32_t sid   = bs_get_u32(msg + 1);
	const float    x     = bs_get_f32(msg + 5);
	const float    y     = bs_get_f32(msg + 9);
	const float    z     = bs_get_f32(msg + 13);
	const float    yaw   = bs_get_f32(msg + 17);
	const float    pitch = bs_get_f32(msg + 21);

	upsertRemote(sid, x, y, z, yaw, pitch);
}

void networldApplyPayload(const uint8_t* payload, size_t len)
{
	if (len < BS_APP_HDR_BYTES) return;

	switch (payload[0]) {
	case BS_APP_BLOCK_EDIT: applyBlockEdit(payload, len); break;
	case BS_APP_WORLD_SYNC: applyWorldSync(payload, len); break;
	case BS_APP_POS_UPDATE: applyPosUpdate(payload, len); break;
	// Every other type byte is something this client build has no use for. Silently skipped
	// rather than treated as an error: the server is trusted to only ever send well-formed
	// application types (server/game/bsgame.c kicks a *client* for sending one it does not
	// recognise, which is a very different situation from this client receiving a type it
	// simply ignores).
	default: break;
	}
}

void networldSetWorld(World* w) { s_world = w; }

void networldInit(void)
{
	blockdiffInit(&s_pending);
	networldResetRemotes();
	s_last_pose_send_ms = 0;
	s_pose_sent_once    = false;
}

// Ages out any remote whose last pose is at or past NETWORLD_REMOTE_TIMEOUT_MS old. The server
// sends no leave notification (see this file's own header), so pose silence is the only
// despawn signal that exists at all. Unconditional — no s_world check — on purpose: remote-
// player presence is not a World concept, so a client between menu and world must still not
// accumulate players it can never despawn.
static void ageOutRemotes(void)
{
	const uint64_t now = bsSockNowMs();
	for (int i = 0; i < NETWORLD_MAX_REMOTE; i++) {
		if (!s_remotes[i].occupied) continue;
		if (now - s_remotes[i].last_seen_ms >= (uint64_t)NETWORLD_REMOTE_TIMEOUT_MS)
			s_remotes[i].occupied = false;
	}
}

void networldUpdate(void)
{
	ageOutRemotes();

	if (!s_world) return;

	uint8_t buf[NETWORLD_RECV_BUF];
	for (int i = 0; i < NETWORLD_MAX_MSGS_PER_FRAME; i++) {
		const int n = netTransportRecv(buf, sizeof buf);
		if (n <= 0) break;
		networldApplyPayload(buf, (size_t)n);
	}
}

// blockdiffDrain()'s apply callback (net/blockdiff.h): one already-recorded, already-valid
// diff, handed straight to worldSet(). Not re-validated — it was validated once, in
// applyBlockEdit()/applyWorldSync(), before it was ever recorded.
static void applyDrained(void* userdata, int x, int y, int z, BlockId id)
{
	World* w = (World*)userdata;
	worldSet(w, x, y, z, id);
}

void networldOnColumnLoad(World* w, int cx, int cz)
{
	if (!s_world || w != s_world) return;
	blockdiffDrain(&s_pending, cx, cz, applyDrained, s_world);
}

bool networldSendBlockEdit(int x, int y, int z, uint8_t block)
{
	uint8_t out[BS_BLOCK_EDIT_BYTES];
	out[0] = BS_APP_BLOCK_EDIT;
	bs_put_i32(out + 1, (int32_t)x);
	bs_put_i32(out + 5, (int32_t)y);
	bs_put_i32(out + 9, (int32_t)z);
	out[13] = block;
	return netTransportSend(out, sizeof out);
}

int networldPendingCount(void)     { return blockdiffCount(&s_pending); }
int networldPendingRefusals(void)  { return blockdiffRefusals(&s_pending); }

int networldRemoteCount(void)
{
	int n = 0;
	for (int i = 0; i < NETWORLD_MAX_REMOTE; i++)
		if (s_remotes[i].occupied) n++;
	return n;
}

bool networldRemoteGet(int index, NetworldRemote* out)
{
	if (index < 0 || !out) return false;

	int n = 0;
	for (int i = 0; i < NETWORLD_MAX_REMOTE; i++) {
		if (!s_remotes[i].occupied) continue;
		if (n == index) {
			out->sid   = s_remotes[i].sid;
			out->x     = s_remotes[i].x;
			out->y     = s_remotes[i].y;
			out->z     = s_remotes[i].z;
			out->yaw   = s_remotes[i].yaw;
			out->pitch = s_remotes[i].pitch;
			return true;
		}
		n++;
	}
	return false;
}

bool networldSendPose(float x, float y, float z, float yaw, float pitch)
{
	const uint64_t now = bsSockNowMs();

	// Gate before touching the transport at all — a suppressed call must count as zero sends,
	// not a send the caller happens to ignore the result of.
	if (s_pose_sent_once && now - s_last_pose_send_ms < (uint64_t)NETWORLD_POSE_INTERVAL_MS)
		return false;

	uint8_t out[BS_POS_UPDATE_C_BYTES];
	out[0] = BS_APP_POS_UPDATE;
	bs_put_f32(out + 1,  x);
	bs_put_f32(out + 5,  y);
	bs_put_f32(out + 9,  z);
	bs_put_f32(out + 13, yaw);
	bs_put_f32(out + 17, pitch);

	s_last_pose_send_ms = now;
	s_pose_sent_once    = true;
	return netTransportSend(out, sizeof out);
}

void networldResetRemotes(void)
{
	memset(s_remotes, 0, sizeof s_remotes);
}
