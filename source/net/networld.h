// networld.h — glue between the encrypted transport and the live World.
//
// bsnet_transport.c moves opaque, already-decrypted bytes; bsnet.c is the UI-facing status
// API for the other session's menus. Neither of them knows a block from a byte. This module
// is the missing piece: it decodes BS_APP_BLOCK_EDIT / BS_APP_WORLD_SYNC (bs_proto.h, which
// `make deps` fetches pinned into deps/blocksmith-server — it is included as
// "proto/bs_proto.h", and was once "../../server/proto/bs_proto.h", a path that only
// resolved on a machine with the server repo checked out next door; see bsnet_transport.h
// for the whole story) into world writes, and it is the only thing in the client that calls
// net/blockdiff.h's store — see that header for why one exists at all: worldSet()
// (world/world.c) refuses a non-air block into a column that is not currently loaded, and a
// remote edit aimed at one would otherwise simply vanish.
//
// It also decodes BS_APP_POS_UPDATE into a small remote-player table (NetworldRemote, below).
// That table is deliberately independent of the World this file otherwise guards: pose sync is
// not a world-write concern, so it is tracked and aged out regardless of whether
// networldSetWorld() has ever been called. The server sends no join/leave notification for a
// player (see below), so a pose-silence timeout is the only despawn signal that exists.
//
// Threading note, because it is the one thing that makes the column-load hook safe rather
// than a latent crash: app/worker.c's worker thread builds its own private staging World
// (never the live one) and calls the very same world.c primitives this module hooks into.
// networldOnColumnLoad() is therefore given the World* the call happened on and compares it
// against the one registered with networldSetWorld() — a mismatch is the worker thread
// touching its staging copy, and is a silent no-op, not a bug. See world.c's worldSet() and
// main.c's genInstallOne() for the two call sites that matter.
//
// Bounded work per frame throughout: this runs on an Old 3DS at 60 FPS. No heap allocation
// anywhere in this file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world/world.h"

// Registers the World that networldUpdate() writes into and networldOnColumnLoad() drains
// into. Call once, main thread only, after the live World exists — main.c does this right
// after worldReportBuild() returns. NULL is a valid value (clears the registration), which is
// what makes every other call in this header a harmless no-op before boot finishes wiring
// things up, and is also what keeps the worker thread's staging World from ever matching.
void networldSetWorld(World* w);

// Clears the pending block-diff store and the remote-player table (see networldResetRemotes()
// below), and resets networldSendPose()'s own send-interval timer. Call once at boot alongside
// netInit(); safe to call again later (e.g. leaving a session) to drop anything still queued.
void networldInit(void);

// Pumps the transport once: drains up to NETWORLD_MAX_MSGS_PER_FRAME decrypted application
// payloads (net/bsnet_transport.h's netTransportRecv), applying each BS_APP_BLOCK_EDIT /
// BS_APP_WORLD_SYNC to the registered World or queueing it via net/blockdiff.h when its
// column is not currently loaded. Call once per frame, after netUpdate(). Also ages out any
// NetworldRemote whose last pose is older than NETWORLD_REMOTE_TIMEOUT_MS, every call,
// regardless of whether a World is registered. The message pump itself is still a no-op before
// networldSetWorld() — see the header comment above for why. Bounded regardless of how many
// messages are actually queued.
void networldUpdate(void);

// Drains every diff pending for column (cx, cz) into `w`, the moment that column becomes part
// of the *live* world. `w` must be the same pointer passed to networldSetWorld() — see the
// threading note above for why a mismatch is deliberately ignored rather than treated as an
// error.
void networldOnColumnLoad(World* w, int cx, int cz);

// Encodes and sends one local edit to the server. Fire-and-forget: the caller's own worldSet()
// already applied it locally, so this never blocks on or waits for a server round trip. False
// if there is no session or the transport refused the send — both the ordinary "the packet
// did not make it" UDP outcome, not a caller error.
bool networldSendBlockEdit(int x, int y, int z, uint8_t block);

// The seed of the world the server put us in, from BS_APP_WORLD_INFO. False until that packet
// arrives — which is immediately after JOIN, so in practice it is true well before the player
// has finished with the title screen, but a caller must handle false rather than assume: an
// older server never sends one, and in single player there is no session at all. False means
// "use this client's own seed", not "the seed is 0" — 0 is a value a server can legitimately
// own, which is why the seed comes back through an out-parameter.
bool networldWorldSeed(uint32_t* out);

// Diagnostics, mirroring net/blockdiff.h's own counters — this module owns the store
// privately, so a HUD or log line reaches these instead of the store directly.
int networldPendingCount(void);
int networldPendingRefusals(void);

// Lifetime traffic counters, since the last networldInit(). These exist because the failure
// they were added to diagnose is completely silent: a client can be Connected, dig a hole, and
// have the edit never reach anyone, with nothing on screen different from the working case.
// Each one splits the pipeline at a different point, so one HUD line says which half is broken:
//   sent     — BS_APP_BLOCK_EDIT payloads this client handed to the transport
//   recvd    — application payloads decoded off the transport, of any type
//   synced   — BS_APP_WORLD_SYNC *entries* seen (the server's backlog of everyone's edits)
//   applied  — remote edits that actually reached worldSet(), rather than being queued
// "sent > 0 but the other three are 0" is a send that never comes back; "synced > 0 but
// applied 0" is a delivery the world never took.
int networldSentEdits(void);
int networldRecvMsgs(void);
int networldSyncEntries(void);
int networldAppliedEdits(void);

// ---- remote players and pose sync ---------------------------------------------------------
//
// Mirrors BS_GAME_MAX_PLAYERS (16) minus the local player.
#define NETWORLD_MAX_REMOTE 15

// How often the local pose goes out. Matches the server's own 10 Hz tick, so resending an
// unchanged pose keeps this player from ageing out on peers (bsgame marks pos_dirty on every
// POS_UPDATE it receives, not only a changed one).
#define NETWORLD_POSE_INTERVAL_MS 100

// A remote player is forgotten after this long without a pose. The server sends no leave
// notification, so this is the only despawn signal there is.
#define NETWORLD_REMOTE_TIMEOUT_MS 3000

typedef struct {
	uint32_t sid;
	float    x, y, z;
	float    yaw, pitch;
} NetworldRemote;

// How many remote players are currently tracked (0..NETWORLD_MAX_REMOTE).
int networldRemoteCount(void);

// Fetches the remote at `index` (0..networldRemoteCount()-1, table order — not sorted, not
// stable across upserts/aging). False, `*out` untouched, for an out-of-range index.
bool networldRemoteGet(int index, NetworldRemote* out);

// Encodes and sends this client's own pose as BS_APP_POS_UPDATE. Internally rate-limited to
// NETWORLD_POSE_INTERVAL_MS, so calling this every frame is the intended usage and stays cheap
// — a call inside an interval that has already sent is a plain false and never touches the
// transport. True only when a packet was actually handed to net/bsnet_transport.h.
bool networldSendPose(float x, float y, float z, float yaw, float pitch);

// Empties the remote-player table. Call on disconnect (or a failed connection) so a stale
// roster from a previous session never lingers into the next one.
void networldResetRemotes(void);

// ---- exposed for the host test only ------------------------------------------------------
//
// networldUpdate() cannot be driven from a host test without a live Noise XX/UDP session
// (bsnet_transport.c is not host-portable — see its own header). This is the decode-and-apply
// step it delegates to for one already-decrypted application payload, so a test can hand it
// hand-built byte buffers directly, the same way server/game/bsgame_test.c exercises
// handle_block_edit() without a live socket. Not "internal" in the sense of being unstable —
// networldUpdate() is a two-line loop around exactly this call.
void networldApplyPayload(const uint8_t* payload, size_t len);
