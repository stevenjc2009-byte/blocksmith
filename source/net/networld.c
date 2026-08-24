#include "net/networld.h"

#include <string.h>

#include "net/blockdiff.h"
#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/block.h"
#include "world/mesher.h"
#include "world/registry.h"
#include "world/visgraph.h"

#include "proto/bs_proto.h"

// The World networldUpdate()/networldOnColumnLoad() are allowed to touch. NULL until
// networldSetWorld() is called, and also the reason a worker-thread call into
// networldOnColumnLoad() (see the header's threading note) is safely ignored: the worker
// thread never has this pointer, only its own private staging World.
static World* s_world;

// The whole point of this module: remote edits aimed at a column we have not streamed in yet,
// held here instead of being dropped, until networldOnColumnLoad() says that column exists.
static BlockDiffStore s_pending;

// Who to tell when a remote edit actually lands in the world. See networld.h's own comment for
// why this is a hook rather than a direct chunkRenderTouch() call.
static NetworldEditFn s_edit_fn;
static void*          s_edit_ud;

// networld.h defines its own NETWORLD_INV_SLOT_COUNT rather than including this header for one
// integer (see that comment); this keeps the duplicate honest the same way validate.h's
// static-asserts do for BS_BLOCK_COUNT.
_Static_assert(NETWORLD_INV_SLOT_COUNT == BS_INV_SLOT_COUNT,
               "networld.h's slot count must match proto/bs_proto.h's");
_Static_assert(NETWORLD_ARMOR_SLOTS == BS_ARMOR_SLOTS,
               "networld.h's armour slot count must match proto/bs_proto.h's");

// Who to tell when a valid BS_APP_INV_STATE arrives. See networld.h's own comment on
// NetworldInvFn for why this is a hook rather than a direct call into world/inventory.h.
static NetworldInvFn s_inv_fn;
static void*         s_inv_ud;

// True once at least one valid BS_APP_INV_STATE has been decoded this session. This is the
// capability probe networld.h's own comment on networldSendInvAction() describes — the server
// speaks first, and a client that has not heard it must never originate BS_APP_INV_ACTION, or
// an old server would kick it for a message type it does not recognise. Reset by networldInit()
// alongside every other per-session static, so a stale "yes" never survives into a fresh
// session that has not actually heard from this server yet.
static bool s_have_inv_state;

// The most recent well-formed INV_STATE, kept so that registering a hook after one has already
// arrived still delivers it rather than losing it.
//
// The server volunteers the join-time snapshot immediately after JOIN, and JOIN completes on the
// title screen — main.c's runTitleScreen() pumps networldUpdate() itself, for the same reason it
// pumps netUpdate(). That is long before main.c has entered a world and has an inventory to put
// a snapshot in. Without this retention the join snapshot was decoded, found s_inv_fn still NULL,
// and was dropped: every join started with an empty inventory no matter what the server held, and
// only the *next* snapshot — which the server sends in reply to an action — could ever fill it.
//
// Written only from applyInvState(), and only once the whole packet has validated, so it can
// never hold a half-parsed state. Meaningful only while s_have_inv_state is true; networldInit()
// clearing that flag is what makes a stale snapshot unreachable in a fresh session, so this does
// not need clearing separately.
static NetworldInvState s_last_inv_state;

// ---- saved player state (v1.5.0) ---------------------------------------------------------
// The PLAYER_STATE counterparts of the inventory statics above, same reasoning throughout.

// True once at least one valid BS_APP_PLAYER_STATE has been decoded this session — the
// capability probe networldSendPlayerReport() checks, for the identical reason
// s_have_inv_state gates networldSendInvAction(): the server speaks first, and a client that
// has not heard PLAYER_STATE must never originate PLAYER_REPORT, or an old server would kick
// it for a message type it does not recognise. Reset by networldInit() with every other
// per-session static.
static bool s_have_player_state;

// Which parts of the retained snapshot the server flagged as meaningful
// (BS_PLAYER_STATE_FLAG_POSE / BS_PLAYER_STATE_FLAG_EXT). Kept alongside the values because
// "the server spoke but had nothing saved" (flags 0) is a real state this client must be able
// to tell from "never heard anything" — bs_proto.h's fresh-spawn marker.
static uint8_t s_player_flags;

// The pose those flags cover. Written only once the whole packet has validated, so it can
// never hold a half-parsed state; meaningful only while s_have_player_state is true and the
// POSE flag is set. main.c reads it through networldSavedPose() right after playerInit().
static float s_saved_pose_x, s_saved_pose_y, s_saved_pose_z;
static float s_saved_pose_yaw, s_saved_pose_pitch;

// The armour+meters block, retained raw (networld.h: consumed by survival systems when they
// land). Same write discipline as the pose above.
static NetworldPlayerMeters s_player_meters;

// ---- block registry sync (v1.6.0 Phase A) -------------------------------------------------
// The REGISTRY_INFO/DEFS counterparts of the statics above, same capability-probe reasoning:
// the server speaks first (INFO right after WORLD_INFO at join), and FETCH is only ever sent
// once an INFO has been seen, so an old server never receives a message type it would kick.

// True once a valid BS_APP_REGISTRY_INFO has been decoded this session. Gates nothing on its
// own — DEFS are accepted whenever they arrive, since UDP gives no ordering guarantee between
// them and the INFO — but it is what makes sending FETCH legitimate at all.
static bool s_have_registry_info;

// True when this client's table provably matches the fingerprint the server sent — INFO
// rev + count + crc16 all reproduced against the table as it stands right now. That is the
// WHOLE meaning of this flag and the only thing that may set it; see registryMatchesInfo()
// below for the one function that decides it.
//
// It used to also be set by the LAST flag of a BS_APP_REGISTRY_DEFS batch, i.e. by "the
// server says that was all" rather than by any property of the table. The server sends such
// a batch unconditionally — deps/blocksmith-server/game/bsgame.c emits an empty final batch
// even with nothing to say — so four bytes carrying no records could set it. A client whose
// data batch was lost in the UDP then declared itself synced with a core-only table, which
// disarmed the retry below (registryFetchTick() gives up the moment this is true), released
// the entry gate, and left every server-defined block an air hole for the rest of the
// session with no degraded marker anywhere on screen. Reset by networldInit() like every
// other per-session static.
//
// Since v1.6.0 task 8 it is load-bearing rather than diagnostic: networldRegistryWaiting()
// below is what holds world entry open until it is settled, which is the only thing that
// stops main.c's registryFreeze() from slamming shut before the server's DEFS have had a
// round trip to arrive in.
//
// When it stays false the client is DEGRADED, deliberately and documentedly: an id the table
// has no row for resolves through registryView()'s never-NULL contract to the air row, so an
// unsynced dyn block is a hole in the world rather than a crash or a wrong block. That is a
// designed fallback, not an accident — see networld.h's own comment on
// networldRegistrySynced() for what a caller is expected to do about it.
static bool s_reg_synced;

// True once this session has sent its first BS_APP_REGISTRY_FETCH. Doubles as the retry's arming
// flag: it can only ever be set from applyRegistryInfo(), i.e. only after a well-formed INFO,
// which is what keeps the old-server invariant (a server that never speaks registry never
// receives a FETCH) true for the retries as well as for the first send.
static bool s_sent_fetch;

// The server's own fingerprint for its table, retained from the last well-formed
// BS_APP_REGISTRY_INFO. applyRegistryInfo() used to compare these three against the local
// table and then drop them on the floor, which is why nothing could re-check the table once
// a fetch had actually delivered something: "synced" degenerated into "the server said that
// was all". Meaningful only while s_have_registry_info is true — that flag is what
// networldInit() clears to make a previous session's fingerprint unreachable, the same way
// s_have_inv_state does for s_last_inv_state.
static uint8_t  s_reg_info_rev;
static uint8_t  s_reg_info_count;
static uint16_t s_reg_info_crc;

// The single question s_reg_synced is an answer to: does the table, as it stands at this
// instant, reproduce the fingerprint the server named?
//
// Recomputed rather than remembered, because everything it reads can move under it — a DEFS
// batch commits rows, and networldInit() puts the table back to the core rows. Cheap enough
// to be called from the two decoders that can change either side of the comparison
// (applyRegistryInfo, registryApplyRemote) and deliberately NOT from the frame pump, which
// would run registryCrc16() over the whole table sixty times a second for no new information.
//
// No INFO means no fingerprint, so nothing can be proved and the answer is no. That is also
// what keeps an unsolicited DEFS batch from a server that never sent an INFO — the one case
// the old LAST-flag test accepted outright — from being able to claim a verified table.
static bool registryMatchesInfo(void)
{
	if (!s_have_registry_info) return false;
	return s_reg_info_rev   == REGISTRY_REV
	    && s_reg_info_count == registryCount()
	    && s_reg_info_crc   == registryCrc16();
}

// ---- FETCH retry (v1.6.0 task 8) ----------------------------------------------------------
// One FETCH was not enough. The reply is UDP: a single lost DEFS batch left the client with a
// permanently degraded table for the whole session, and nothing ever asked again. The retry is
// bounded in BOTH dimensions on purpose — a client that keeps asking forever is a client that
// hammers a server which has already decided not to answer.
//
//   attempts: NETWORLD_REG_FETCH_MAX_SENDS total sends, first one included
//   time:     nothing is sent past NETWORLD_REG_SYNC_DEADLINE_MS after the join
//
// It is driven from networldUpdate(), the frame pump, and never from inside a decoder — see
// sendRegistryFetch()'s own comment further down for why a decoder is the wrong place for it.
//
// The four numbers themselves live in networld.h next to NETWORLD_POSE_INTERVAL_MS and
// NETWORLD_REMOTE_TIMEOUT_MS, for the reason those two are there: a timing bound that no test
// can name is a timing bound no test can prove, and networld_test.c drives all of these
// against its own fake clock.

// How many FETCHes have gone out this session (the first one counts), and when the last one did.
static uint8_t  s_reg_fetch_sends;
static uint64_t s_reg_fetch_last_ms;

// When BS_APP_WORLD_INFO landed — the instant the registry question becomes askable, and the
// origin both bounds above are measured from. "armed" is separate from the value for the same
// reason s_pose_sent_once is separate from s_last_pose_send_ms: bsSockNowMs()'s epoch is
// arbitrary, so 0 is a time it can legitimately return rather than a sentinel for "unset".
static uint64_t s_reg_gate_ms;
static bool     s_reg_gate_armed;

// Forward declaration: applyRegistryInfo() answers an INFO mismatch with a FETCH, and the
// senders live further down this file with every other encoder.
static void sendRegistryFetch(uint8_t first_index);

// Starts the registry clock, at whichever of BS_APP_WORLD_INFO / BS_APP_REGISTRY_INFO this
// session hears first. Armed once: a retransmitted packet must not silently extend either
// bound, or a chatty server could hold the client on the title screen indefinitely.
static void registryGateArm(void)
{
	if (s_reg_gate_armed) return;
	s_reg_gate_armed = true;
	s_reg_gate_ms    = bsSockNowMs();
}

// The single place an applied edit is announced. Called only after the block is really in the
// world — never for one that is merely queued, which would tell the renderer to rebuild a mesh
// that is still correct and, worse, imply a block is readable when it is not.
static void notifyEdit(int32_t x, int32_t y, int32_t z)
{
	if (s_edit_fn) s_edit_fn(s_edit_ud, (int)x, (int)y, (int)z);
}

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

// Which world the server put us in. The server owns the seed (server/game/bsgame.c persists it
// in world_seed.txt) and sends it as BS_APP_WORLD_INFO, the first packet after JOIN, precisely
// so this is known before any terrain is generated. "have" is separate from the value because
// 0 is a legal seed, and because a single-player world must keep using its own seed rather
// than silently generating from a server's.
static uint32_t s_world_seed;
static bool     s_have_world_seed;

// Traffic counters — see networld.h for what each one distinguishes. Plain ints, main thread
// only, reset by networldInit(): they are a diagnostic readout, not part of any protocol.
static int s_sent_edits;
static int s_recv_msgs;
static int s_sync_entries;
static int s_applied_edits;

// Every value here came off the wire, so every one of it is treated as hostile — same posture
// as server/game/validate.c's bsEditValid(), which this mirrors. y and block are checked
// against the client's own real world model (world/world.h's WORLD_HEIGHT, world/registry.h's
// id space) rather than a second copy of either number.
static bool editValid(int32_t x, int32_t y, int32_t z, uint8_t block)
{
	if (x < -NETWORLD_XZ_LIMIT || x > NETWORLD_XZ_LIMIT) return false;
	if (z < -NETWORLD_XZ_LIMIT || z > NETWORLD_XZ_LIMIT) return false;
	if (y < 0 || y >= WORLD_HEIGHT)                      return false;
	// v1.6.0: the whole defined id space is legal, not just the core rows — a server can
	// register dynamic blocks (0x80..0xFD) this client has not synced yet, and such an edit
	// must still be applied (it renders as air until the registry catches up, then resolves).
	// Only the two reserved ids above the dyn range are refused.
	if (block > REG_ID_DYN_HI)                           return false;
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
	// No world yet is the *normal* case for the batch that matters most. The server sends
	// BS_APP_WORLD_SYNC — every edit every other player has ever made — immediately after JOIN
	// (proto/bs_proto.h), and JOIN happens the moment the handshake completes, which is on the
	// title screen: the player is still choosing a world, and s_world stays NULL for however
	// long that takes. Dropping here meant a joining player never saw a single one of those
	// edits, so "connect to the server" put them in an untouched copy of the terrain instead of
	// the world everyone else had been building in. Queueing is exactly what this store is for
	// — a column that does not exist yet is not different in kind from one that has not
	// streamed in yet — and networldSetWorld() flushes whatever landed before the world did.
	if (!s_world) {
		blockdiffRecord(&s_pending, x, y, z, block);
		return;
	}

	if (worldColumn(s_world, x >> 4, z >> 4) != NULL) {
		worldSet(s_world, x, y, z, block);
		s_applied_edits++;
		notifyEdit(x, y, z);
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

		s_sync_entries++;
		if (editValid(x, y, z, block)) applyOrQueue(x, y, z, block);
		p += BS_SYNC_ENTRY_BYTES;
	}
}

static void applyWorldInfo(const uint8_t* msg, size_t len)
{
	if (len != BS_WORLD_INFO_BYTES) return;

	s_world_seed      = bs_get_u32(msg + 1);
	s_have_world_seed = true;

	// v1.6.0 task 8: WORLD_INFO is the join's own timestamp as far as the registry is
	// concerned — the server sends REGISTRY_INFO immediately behind it, so this is the
	// instant from which "how long have we waited for the table" is measured.
	registryGateArm();
}

// BS_APP_INV_STATE: the whole inventory, authoritative, exactly BS_INV_STATE_BYTES or dropped
// — same "reject on length, never half-parse" posture applyWorldInfo() above takes, for the
// same reason bs_proto.h states there: a longer INV_STATE from a newer server must be
// rejectable outright rather than silently misread.
static void applyInvState(const uint8_t* msg, size_t len)
{
	if (len != BS_INV_STATE_BYTES) return;

	NetworldInvState state;
	state.selected_hotbar = msg[1];

	const uint8_t* p = msg + BS_APP_HDR_BYTES + 1u;
	for (uint32_t i = 0; i < BS_INV_SLOT_COUNT; i++) {
		const uint8_t item  = p[0];
		const uint8_t count = p[1];

		// bs_proto.h: count == 0 iff item == 0 (BLOCK_AIR / ITEM_NONE); any other pairing is a
		// packet the client is entitled to treat as malformed, and the whole packet is dropped
		// — not the one bad slot — same posture as a bad length or an over-cap count below.
		if ((count == 0) != (item == 0)) return;
		if (count > BS_INV_STACK_MAX)    return;

		state.slots[i].item  = item;
		state.slots[i].count = count;
		p += 2;
	}

	// Only set once the whole packet has proven well-formed above — a malformed INV_STATE must
	// not be the thing that first arms the capability probe networldSendInvAction() checks.
	s_have_inv_state = true;
	s_last_inv_state = state;
	if (s_inv_fn) s_inv_fn(s_inv_ud, &state);
}

// BS_APP_PLAYER_STATE: exactly BS_PLAYER_STATE_BYTES or dropped — the same reject-on-length,
// never-half-parse posture every other snapshot decoder in this file takes (applyWorldInfo(),
// applyInvState() above), for the reason bs_proto.h states there: a longer PLAYER_STATE from a
// newer server must be rejectable outright rather than silently misread.
//
// Float policy: the five pose floats and three meter floats are taken off the wire verbatim,
// with no NaN/range sanitisation — deliberately the same posture applyPosUpdate() above takes
// for POS_UPDATE's untrusted floats. The transport is Noise XX: every payload here already
// came from an authenticated peer, and this packet's contents are that peer's own persisted
// record of this player. The one consumer difference from a remote pose — these values reach
// main.c's live physics body rather than a rendered remote — is answered by where they are
// *applied* (main.c, at playerInit(), before the first frame), not by inventing a second
// validation posture for one message type.
static void applyPlayerState(const uint8_t* msg, size_t len)
{
	if (len != BS_PLAYER_STATE_BYTES) return;

	const uint8_t flags = msg[1];

	if (flags & BS_PLAYER_STATE_FLAG_POSE) {
		s_saved_pose_x    = bs_get_f32(msg + 2);
		s_saved_pose_y    = bs_get_f32(msg + 6);
		s_saved_pose_z    = bs_get_f32(msg + 10);
		s_saved_pose_yaw   = bs_get_f32(msg + 14);
		s_saved_pose_pitch = bs_get_f32(msg + 18);
	}

	if (flags & BS_PLAYER_STATE_FLAG_EXT) {
		uint8_t* armor = &s_player_meters.armor[0][0];
		for (uint32_t i = 0; i < BS_ARMOR_SLOTS * 2u; i++)
			armor[i] = msg[BS_APP_HDR_BYTES + 1u + 4u * 5u + i];

		s_player_meters.xp_level    = bs_get_u32(msg + 30);
		s_player_meters.xp_progress = bs_get_f32(msg + 34);
		s_player_meters.health      = bs_get_f32(msg + 38);
		s_player_meters.hunger      = bs_get_f32(msg + 42);
	}

	// Armed only after everything above has been stored — same discipline as applyInvState().
	// Note flags == 0 (the fresh-spawn marker) still arms it: the server spoke, which is
	// precisely what the capability probe is asking.
	s_have_player_state = true;
	s_player_flags      = flags;
}

// BS_APP_REGISTRY_INFO: the server's whole-table fingerprint {rev u8, count u8, crc16 u16 LE},
// exactly BS_APP_REGISTRY_INFO_BYTES or dropped — the same reject-on-length, never-half-parse
// posture every other snapshot decoder in this file takes.
//
// Matching rev + count + crc16 means this build's compiled-in table already agrees with the
// server's and the join costs zero extra traffic. Anything else asks for the dynamic rows with
// one FETCH — core rows are compiled into every binary of this protocol era, so only 0x80..0xFD
// ever travel. The FETCH goes out here, immediately: at join time this lands right after
// WORLD_INFO, long before any server column can generate or mesh, which is the ordering slot
// that makes "apply before generating" free (bs_proto.h's REGISTRY_INFO comment).
// Where a FETCH must ask from: the lowest dyn id this table still has no row for.
//
// Derived from the registry every time rather than remembered here, because a RETRY after a
// partially delivered fetch has to ask for what is actually still missing. registryRemoteApply()
// refuses any batch whose first id is not the table's own next free slot (world/registry.c: a
// batch that starts anywhere else is a tampered or reordered stream), so re-asking from
// REG_ID_DYN_LO once two rows had already landed would be refused for the rest of the session
// and the retry would achieve nothing at all.
static uint8_t registryFetchIndex(void)
{
	for (int id = REG_ID_DYN_LO; id <= REG_ID_DYN_HI; id++)
		if (!registryIsDefined((BlockId)id)) return (uint8_t)id;
	return REG_ID_DYN_LO;   // dyn space full: nothing is missing, the value is moot
}

static void applyRegistryInfo(const uint8_t* msg, size_t len)
{
	if (len != BS_APP_REGISTRY_INFO_BYTES) return;

	// Armed before any content decision, exactly like applyInvState(): a malformed INFO is
	// dropped without arming, but a well-formed one proves the server speaks registry at all.
	s_have_registry_info = true;
	registryGateArm();

	// Retained, not just compared: registryApplyRemote() below re-checks the table against
	// these three once a batch has landed, which is the only way "synced" can mean anything
	// after a fetch rather than only before one.
	s_reg_info_rev   = msg[1];
	s_reg_info_count = msg[2];
	s_reg_info_crc   = bs_get_u16(msg + 3);

	if (registryMatchesInfo()) {
		s_reg_synced = true;
		return;
	}

	if (!s_sent_fetch) {
		s_sent_fetch        = true;
		s_reg_fetch_sends   = 1;
		s_reg_fetch_last_ms = bsSockNowMs();
		sendRegistryFetch(registryFetchIndex());
	}
}

// The bounded retry. Called from networldUpdate() once per frame, AFTER the receive drain, so
// a batch that arrived this frame has already been applied and can cancel the next attempt.
//
// Deliberately not called from a decoder. The senders in this file are reached from decoders
// only where the decode itself is the trigger (applyRegistryInfo()'s first FETCH), and a retry
// is the opposite of that: it fires precisely because nothing arrived, so hanging it off an
// arriving packet would make it fire on the traffic of unrelated message types and never fire
// at all on a session that has gone quiet — which is the only session that needs it.
static void registryFetchTick(void)
{
	// The old-server invariant, restated at the one new place that could break it: s_sent_fetch
	// is set only by applyRegistryInfo(), only after a well-formed BS_APP_REGISTRY_INFO. A
	// server that never speaks registry never sets it and therefore never receives a retry,
	// exactly as it never receives the first send.
	if (!s_sent_fetch) return;
	if (s_reg_synced)  return;   // the table is settled; nothing left to ask for

	if (s_reg_fetch_sends >= NETWORLD_REG_FETCH_MAX_SENDS) return;   // attempt bound

	const uint64_t now = bsSockNowMs();
	if (now - s_reg_gate_ms >= (uint64_t)NETWORLD_REG_SYNC_DEADLINE_MS) return;  // time bound
	if (now - s_reg_fetch_last_ms < (uint64_t)NETWORLD_REG_FETCH_RETRY_MS) return;

	s_reg_fetch_sends++;
	s_reg_fetch_last_ms = now;
	sendRegistryFetch(registryFetchIndex());
}

// BS_APP_REGISTRY_DEFS: one batch of consecutive dynamic defs {first u8, n u8, last u8,
// n x 28B}. Exactly BS_APP_REGISTRY_DEFS_BYTES(n) or dropped; n over the sender's own cap is
// dropped rather than trusted. The records themselves are validated twice over — once for
// shape here, once for id/name sanity inside registryRemoteApply(), which commits all-or-
// nothing so a bad batch can never leave a half-applied table behind.
//
// After a successful application the mesher's derived tables are dropped so the next
// meshChunk() rebuilds them against the new rows; edits that arrived before sync were stored
// as their raw ids and resolve now instead of rendering as air forever.
//
// A refused batch (duplicate, gap, out-of-range) is dropped whole and s_reg_synced stays
// false — degraded but safe: unknown ids keep answering air through blockInfo()'s contract.
//
// The wire's LAST flag (msg[3]) is deliberately not read at all. It says the SERVER has
// finished talking, which is a fact about the server and not about this table, and treating
// it as the end of the sync is exactly what made an empty terminating batch — which bsgame
// sends unconditionally — able to fake a completed one. What settles the question instead is
// registryMatchesInfo(): re-run the fingerprint after every batch that changed the table, and
// let a mismatch leave s_reg_synced false so the bounded retry above stays armed and asks
// again from registryFetchIndex(), the first id the table is actually still missing.
//
// Re-evaluated on every batch rather than only on the last one, which also buys the reverse
// case for free: a delivery whose terminator was the packet that got lost still verifies the
// instant its final rows land, instead of leaving a complete, correct table marked degraded.
//
// It is an assignment rather than an |=, so a batch that arrives after the table has been
// frozen (main.c's genStart(), after which world/registry.c refuses every row for the rest of
// the session) cannot leave the degraded indicator reading healthy at the one moment recovery
// has become impossible.
static void registryApplyRemote(const uint8_t* msg, size_t len)
{
	if (len < BS_APP_HDR_BYTES + 3u) return;

	const uint8_t first = msg[1];
	const uint8_t n     = msg[2];

	if ((size_t)BS_APP_REGISTRY_DEFS_BYTES(n) != len) return;
	if (n > BS_APP_REGISTRY_DEFS_MAX_N)               return;

	if (n > 0) {
		if (registryRemoteApply(first, msg + BS_APP_HDR_BYTES + 3u, n) != n) return;
		mesherInvalidateTables();

		// v1.7.1: world/visgraph.c caches an openness table off the same registry the mesher
		// does, so it has to be dropped here for the same reason and at the same moment. Its
		// own contents-stamp already catches this batch on its own — this call is the belt to
		// that stamp's braces, and it earns its place because the two failures are not
		// comparable: a stale mesher table draws a face wrong, while a stale visgraph table
		// culls a chunk that should have been drawn and puts a hole in the world.
		visgraphInvalidateTables();
	}

	s_reg_synced = registryMatchesInfo();
}

// BS_APP_CHUNK_DIFFS: one column's worth of diffs, batched. Same per-entry shape as
// applyWorldSync() above and deliberately handled the same way — apply now if the column is
// already loaded, queue it via net/blockdiff.h otherwise — because a diff scoped to a column is
// not different in kind from one that arrived as part of the old whole-world replay; only how
// the server chose to batch it changed. Reusing applyOrQueue() rather than a second path is the
// whole point: there is exactly one way a remote edit reaches this client's World, regardless of
// which message type carried it in.
//
// cx/cz (the column this batch is for) and the LAST flag are read only far enough to validate
// the packet's own shape. Both exist on the wire for reasons that are about the SERVER's side of
// this protocol (bs_proto.h: cx/cz because UDP has no ordering guarantee between two columns
// subscribed in the same tick, LAST because a column with zero diffs still has to be told
// "empty", not "still coming") and neither is consumed here beyond that: every entry carries its
// own absolute (x, y, z), which is what applyOrQueue() actually keys on to find its column — the
// same way a WORLD_SYNC entry does, with no column header at all. This client tracks no
// per-column "am I fully synced yet" state, because nothing in main.c reads such a thing: a
// column meshes the moment it installs (genInstallOne() -> genQueueReadyColumns(), before any
// CHUNK_DIFFS for it can have arrived) and a diff that lands afterwards — whether it was queued
// from before the column existed or arrives live once it does — already reaches the screen via
// the edit hook's remesh notification (see this header's own comment on NetworldEditFn). Adding a
// synced/not-synced flag nobody reads would be state for its own sake.
static void applyChunkDiffs(const uint8_t* msg, size_t len)
{
	if (len < BS_CHUNK_DIFFS_HDR_BYTES) return;

	const uint16_t count = bs_get_u16(msg + 10);

	// Same two-part malformed-packet posture as applyWorldSync(): the declared count must
	// exactly explain the packet's own length, and independently must not exceed the sender's
	// own per-batch cap — a payload lying about either is dropped whole, not parsed as far as
	// it happens to still make sense.
	if ((uint32_t)count > BS_CHUNK_DIFFS_MAX_ENTRIES) return;
	if ((size_t)BS_CHUNK_DIFFS_BYTES(count) != len) return;

	const uint8_t* p = msg + BS_CHUNK_DIFFS_HDR_BYTES;
	for (uint16_t i = 0; i < count; i++) {
		const int32_t x     = bs_get_i32(p);
		const int32_t y     = bs_get_i32(p + 4);
		const int32_t z     = bs_get_i32(p + 8);
		const uint8_t block = p[12];

		// Not counted in s_sync_entries: that counter is documented (networld.h) as
		// specifically BS_APP_WORLD_SYNC entries, and s_recv_msgs already went up once for
		// this packet in networldApplyPayload() below — enough to tell "chunk diffs are
		// arriving" apart from "nothing is arriving" without redefining an existing counter's
		// meaning out from under whatever already reads it.
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

	s_recv_msgs++;

	switch (payload[0]) {
	case BS_APP_BLOCK_EDIT:  applyBlockEdit(payload, len);  break;
	case BS_APP_WORLD_SYNC:  applyWorldSync(payload, len);  break;
	case BS_APP_POS_UPDATE:  applyPosUpdate(payload, len);  break;
	case BS_APP_WORLD_INFO:  applyWorldInfo(payload, len);  break;
	case BS_APP_CHUNK_DIFFS: applyChunkDiffs(payload, len); break;
	case BS_APP_INV_STATE:   applyInvState(payload, len);   break;
	case BS_APP_PLAYER_STATE: applyPlayerState(payload, len); break;
	case BS_APP_REGISTRY_INFO: applyRegistryInfo(payload, len); break;
	case BS_APP_REGISTRY_DEFS: registryApplyRemote(payload, len); break;
	// Every other type byte is something this client build has no use for. Silently skipped
	// rather than treated as an error: the server is trusted to only ever send well-formed
	// application types (server/game/bsgame.c kicks a *client* for sending one it does not
	// recognise, which is a very different situation from this client receiving a type it
	// simply ignores).
	default: break;
	}
}

void networldSetWorld(World* w)
{
	// Registering the World is all this does. It used to also flush the pending store into any
	// column that already existed, on the stated assumption that "main.c calls this straight
	// after worldReportBuild(), with the starting ring built" — and that assumption is false.
	// worldReportBuild() (main.c) *allocates* a 17x17 grid of columns as a memory-budget proof
	// before a single block of terrain is generated; the worker only starts afterwards, in
	// genStart(). So every column in that grid answered worldColumn() != NULL while holding
	// nothing but air, the flush emptied the store into it, and the generated terrain then
	// landed on top and wiped all of it.
	//
	// Measured, not argued: dig 8 blocks in a joined session, leave, rejoin. The HUD read
	// `net s8 ... y8 a0 q0` — eight edits sent, eight sync entries back from the server, none
	// still queued — and the ground was visibly whole, with the frame's triangle count
	// (71532) identical to the pre-dig frame's. The trench had been applied to an empty column
	// and generated over.
	//
	// Nothing is stranded by dropping the flush: every column that ever holds real terrain
	// arrives through workerInstall(), and main.c calls networldOnColumnLoad() for each one the
	// moment it is installed (genInstallOne, immediately before genQueueReadyColumns) — which
	// is also the only point at which applying a diff is meaningful, since it has to go on top
	// of the generated blocks rather than under them.
	s_world = w;
}

void networldSetEditHook(NetworldEditFn fn, void* userdata)
{
	s_edit_fn = fn;
	s_edit_ud = userdata;
}

void networldSetInvHook(NetworldInvFn fn, void* userdata)
{
	s_inv_fn = fn;
	s_inv_ud = userdata;

	// Deliver the snapshot that already arrived, if one has. Registration order is not something
	// the caller can win by rearranging: the server's join-time INV_STATE lands during JOIN, on
	// the title screen, and the only place a hook can usefully point is at an inventory that does
	// not exist until a world has been entered. So the hook comes late by construction, and it is
	// this replay — not the arrival — that puts the server's inventory on the console.
	//
	// Guarded on fn so that unregistering — passing NULL to deliberately stop receiving these —
	// stays a plain assignment and never calls through a null pointer.
	if (fn && s_have_inv_state) fn(userdata, &s_last_inv_state);
}

void networldInit(void)
{
	blockdiffInit(&s_pending);
	networldResetRemotes();

	// Cleared with everything else: a hook left over from a previous session would point at
	// renderer state that session has already torn down.
	s_edit_fn = NULL;
	s_edit_ud = NULL;

	// Same reasoning as the edit hook above, plus the capability probe itself: a fresh session
	// has heard nothing from this server yet, regardless of what the previous session heard.
	s_inv_fn          = NULL;
	s_inv_ud          = NULL;
	s_have_inv_state  = false;

	// The PLAYER_STATE snapshot and its capability flag, for the same reason as the inventory
	// pair above: a stale "yes" must never survive into a session whose server has not spoken.
	// Like s_last_inv_state, the retained values are left in place — unreachable while the
	// flags say no, and overwritten before they can ever be read again.
	s_have_player_state = false;
	s_player_flags      = 0;

	// Registry sync state, same per-session discipline: a fresh session has heard no INFO,
	// synced nothing, and owes no FETCH.
	s_have_registry_info = false;
	s_reg_synced         = false;
	s_sent_fetch         = false;
	s_reg_info_rev       = 0;
	s_reg_info_count     = 0;
	s_reg_info_crc       = 0;
	s_reg_fetch_sends    = 0;
	s_reg_fetch_last_ms  = 0;
	s_reg_gate_armed     = false;
	s_reg_gate_ms        = 0;

	// v1.6.0 task 8: and the TABLE itself, not just the flags that describe it. The dynamic
	// rows above REG_ID_DYN_LO were installed by one server's REGISTRY_DEFS; leaving them
	// standing meant the next server's batch was refused outright (registryRemoteApply()
	// requires a batch to start at the table's next free slot, and the first server had
	// already moved it), so the second session of a boot ran with the FIRST server's blocks
	// under the second server's ids. Everything else per-session in this function is cleared
	// here, and the table is per-session in exactly the same way.
	//
	// Ordering, which is what makes this safe: main.c calls netDisconnect() — the one thing
	// that reaches networldInit() mid-run — only after workerStop() has joined the worker and
	// worldExit() has run, so no thread is reading the table when it is rebuilt. The one
	// exception is app/sleep.c's lid-close leave hook, which disconnects with the worker
	// still alive; that path tears the world down on the next frame anyway, and a worker that
	// meshes against a core-only table in the meantime draws air, which is the same degraded
	// answer an unsynced table gives everywhere else in this file.
	registryInitCore();

	s_last_pose_send_ms = 0;
	s_pose_sent_once    = false;
	s_world_seed        = 0;
	s_have_world_seed   = false;
	s_sent_edits        = 0;
	s_recv_msgs         = 0;
	s_sync_entries      = 0;
	s_applied_edits     = 0;
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

	// Draining is deliberately *not* gated on a registered World any more. The transport's rx
	// ring is 16 slots deep and evicts oldest-first (bsnet_transport.c's push_rx), so a client
	// sitting on the title screen with the session already up — which is where connecting
	// happens — had the server's post-JOIN WORLD_SYNC pushed out of that ring by the next
	// sixteen packets, about 1.6 s of one other player's 10 Hz pose updates, long before it
	// ever reached a world. applyOrQueue() now parks anything that arrives early in the pending
	// store, so the only requirement here is that the ring keeps being emptied.
	uint8_t buf[NETWORLD_RECV_BUF];
	for (int i = 0; i < NETWORLD_MAX_MSGS_PER_FRAME; i++) {
		const int n = netTransportRecv(buf, sizeof buf);
		if (n <= 0) break;
		networldApplyPayload(buf, (size_t)n);
	}

	// After the drain, never before it: a DEFS batch that arrived this frame has already been
	// applied by the loop above, so a fetch that has just completed cancels the retry instead
	// of racing it out onto the wire one last time.
	registryFetchTick();
}

// blockdiffDrain()'s apply callback (net/blockdiff.h): one already-recorded, already-valid
// diff, handed straight to worldSet(). Not re-validated — it was validated once, in
// applyBlockEdit()/applyWorldSync(), before it was ever recorded.
static void applyDrained(void* userdata, int x, int y, int z, BlockId id)
{
	World* w = (World*)userdata;
	worldSet(w, x, y, z, id);

	// Notified here and not when it was recorded: this is the first instant the block is real.
	// Mostly a no-op in practice — the column this diff belongs to has only just been installed
	// and its own chunks have no mesh to dirty yet (main.c calls this immediately before
	// genQueueReadyColumns, which meshes them from the finished blocks) — but a diff on a column
	// edge changes the faces of an *already meshed* neighbour, and that one does need saying.
	notifyEdit(x, y, z);
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
	const bool ok = netTransportSend(out, sizeof out);
	if (ok) s_sent_edits++;
	return ok;
}

// See networld.h's own comment on this for why the capability-probe check comes first and
// unconditionally: sending nothing before an INV_STATE has arrived is not a defensive nicety,
// it is the entire reason a new client->server message could be introduced at all without a
// hand-sequenced release (proto/bs_proto.h's long comment on BS_APP_INV_STATE/BS_APP_INV_ACTION
// spells out why). Encoded by hand, little-endian, never memcpy of a struct — the same posture
// networldSendBlockEdit() takes just above, for the same struct-padding reason bs_proto.h's own
// byte-order note gives.
bool networldSendInvAction(uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
	if (!s_have_inv_state) return false;

	uint8_t out[BS_INV_ACTION_BYTES];
	out[0] = BS_APP_INV_ACTION;
	out[1] = op;
	out[2] = a;
	out[3] = b;
	out[4] = c;
	return netTransportSend(out, sizeof out);
}

// The registry counterpart of the capability probe above, and the invariant this whole feature
// hangs on: the ONLY two callers are applyRegistryInfo() and registryFetchTick(), and both are
// reachable only once a well-formed BS_APP_REGISTRY_INFO has arrived — the first directly, the
// second through s_sent_fetch, which nothing else sets. So an old server — one that never
// speaks registry — never receives this C->S type and never has reason to kick us for it. Do
// not add a third caller without carrying that check with it.
//
// Fire-and-forget on the wire like every sender in this file. What changed in v1.6.0 task 8 is
// that a lost reply is no longer permanent: registryFetchTick() re-asks up to
// NETWORLD_REG_FETCH_MAX_SENDS times within NETWORLD_REG_SYNC_DEADLINE_MS, driven by the frame
// pump rather than from inside a decoder (see that function for why the distinction matters).
static void sendRegistryFetch(uint8_t first_index)
{
	uint8_t out[BS_APP_REGISTRY_FETCH_BYTES];
	out[0] = BS_APP_REGISTRY_FETCH;
	out[1] = first_index;
	netTransportSend(out, sizeof out);
}

// See networld.h's own comment on this pair for why both are void, fire-and-forget, and
// unguarded by any local session check — netTransportSend() already reports "no session" as a
// plain false, which is exactly the same "the packet did not make it" outcome UDP always carries,
// and there is nothing more useful to do with it than what networldSendBlockEdit() already does:
// nothing, silently.
void networldSubscribeColumn(int col_x, int col_z)
{
	uint8_t out[BS_CHUNK_SUB_BYTES];
	out[0] = BS_APP_CHUNK_SUB;
	bs_put_i32(out + 1, (int32_t)col_x);
	bs_put_i32(out + 5, (int32_t)col_z);
	netTransportSend(out, sizeof out);
}

void networldUnsubscribeColumn(int col_x, int col_z)
{
	uint8_t out[BS_CHUNK_UNSUB_BYTES];
	out[0] = BS_APP_CHUNK_UNSUB;
	bs_put_i32(out + 1, (int32_t)col_x);
	bs_put_i32(out + 5, (int32_t)col_z);
	netTransportSend(out, sizeof out);
}

bool networldWorldSeed(uint32_t* out)
{
	if (!s_have_world_seed) return false;
	if (out) *out = s_world_seed;
	return true;
}

bool networldRegistrySynced(void)
{
	return s_reg_synced;
}

// See networld.h for the contract. The whole point of this function is that world entry —
// and therefore main.c's registryFreeze(), which is what makes a dynamic row uncommittable
// forever after — has something to wait on. Both arms are bounded, so a caller that spins on
// this is guaranteed to be let through.
bool networldRegistryWaiting(void)
{
	// Not joined at all: single player, or a session that has not reached WORLD_INFO. Nothing
	// to wait for, and in particular a single-player boot must never be delayed by this.
	if (!s_reg_gate_armed) return false;

	// Settled: either the compiled-in table already matched the server's fingerprint, or the
	// last DEFS batch of a fetch has been applied.
	if (s_reg_synced) return false;

	const uint64_t waited = bsSockNowMs() - s_reg_gate_ms;

	// No INFO heard yet gets only the short grace, not the full deadline: a pre-v1.6.0 server
	// never sends one at all, and stalling every join against an old server for two seconds to
	// wait for a packet that does not exist would be a regression the player can feel.
	if (!s_have_registry_info) return waited < (uint64_t)NETWORLD_REG_INFO_GRACE_MS;

	// INFO heard and still not synced: the fetch is in flight (or being retried). Wait out the
	// deadline, then give up and let the caller in degraded — a hole in the world beats a
	// title screen that never ends.
	return waited < (uint64_t)NETWORLD_REG_SYNC_DEADLINE_MS;
}

// See networld.h's own comment on this pair: the pose accessors hand the retained join
// snapshot to main.c at the one moment it is applicable — right after playerInit(), which is
// this client's join-time position of the local player. The flag checks are what keep a
// fresh-spawn snapshot (server spoke, nothing saved) from being read as a real pose.
bool networldSavedPose(float* out_x, float* out_y, float* out_z,
                       float* out_yaw, float* out_pitch)
{
	if (!s_have_player_state || !(s_player_flags & BS_PLAYER_STATE_FLAG_POSE)) return false;
	if (out_x)    *out_x    = s_saved_pose_x;
	if (out_y)    *out_y    = s_saved_pose_y;
	if (out_z)    *out_z    = s_saved_pose_z;
	if (out_yaw)  *out_yaw  = s_saved_pose_yaw;
	if (out_pitch)*out_pitch= s_saved_pose_pitch;
	return true;
}

bool networldPlayerMetersValid(void)
{
	return s_have_player_state && (s_player_flags & BS_PLAYER_STATE_FLAG_EXT) != 0;
}

const NetworldPlayerMeters* networldPlayerMeters(void)
{
	return networldPlayerMetersValid() ? &s_player_meters : NULL;
}

// Capability probe first and unconditionally, exactly as networldSendInvAction() above does:
// no PLAYER_STATE heard this session means the server may predate PLAYER_REPORT entirely, and
// bsgame kicks an unknown C->S type rather than ignoring it. Then the compile-honest meter
// gate — see BS_CLIENT_HAS_METERS in networld.h for why sending before real meters exist
// would wipe a returning player's saved state rather than update it.
//
// Encoded by hand, little-endian, never memcpy of a struct — same posture as every other
// sender in this file. The five reserved tail bytes go out as zeros because bs_proto.h says
// they MUST, the same way INV_ACTION's unused parameters do.
bool networldSendPlayerReport(const NetworldPlayerMeters* m)
{
#if !BS_CLIENT_HAS_METERS
	(void)m;
	return false;
#else
	if (!s_have_player_state || !m) return false;

	uint8_t out[BS_PLAYER_REPORT_BYTES];
	out[0] = BS_APP_PLAYER_REPORT;
	const uint8_t* armor = &m->armor[0][0];
	for (uint32_t i = 0; i < BS_ARMOR_SLOTS * 2u; i++)
		out[1 + i] = armor[i];

	bs_put_u32(out + 9,  m->xp_level);
	bs_put_f32(out + 13, m->xp_progress);
	bs_put_f32(out + 17, m->health);
	bs_put_f32(out + 21, m->hunger);

	memset(out + 25, 0, 5u);
	return netTransportSend(out, sizeof out);
#endif
}

int networldPendingCount(void)     { return blockdiffCount(&s_pending); }
int networldPendingRefusals(void)  { return blockdiffRefusals(&s_pending); }

int networldSentEdits(void)    { return s_sent_edits; }
int networldRecvMsgs(void)     { return s_recv_msgs; }
int networldSyncEntries(void)  { return s_sync_entries; }
int networldAppliedEdits(void) { return s_applied_edits; }

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
