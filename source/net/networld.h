// networld.h — glue between the encrypted transport and the live World.
//
// bsnet_transport.c moves opaque, already-decrypted bytes; bsnet.c is the UI-facing status
// API for the other session's menus. Neither of them knows a block from a byte. This module
// is the missing piece: it decodes BS_APP_BLOCK_EDIT / BS_APP_WORLD_SYNC / BS_APP_CHUNK_DIFFS
// (bs_proto.h, which `make deps` fetches pinned into deps/blocksmith-server — it is included as
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
//
// v1.8.5: "clears the pending block-diff store" now also means FREES it. The store is a
// 1064976-byte heap allocation this module makes the first time a server packet actually needs
// somewhere to queue an edit, not a static that exists from boot — see networld.c's comment on
// s_pending for the whole lifecycle and why the allocation could not be hung off this call.
// Nothing about the contract above changes: after this returns, nothing is queued.
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

// ---- remesh notification -----------------------------------------------------------------
//
// Writing a block into the World is only half of applying a remote edit. Nothing on screen is
// read from the World directly: the terrain the player sees is a set of cached chunk meshes
// (scene/chunk_render.h), each built once and reused until something marks it dirty. A local
// break or place marks its own chunk — scene/interact.c calls chunkRenderTouch() right after
// worldSet(). A remote edit had no equivalent, so from the local player's point of view
// another player's edits did not exist visually while being fully real physically: collision
// and the block raycast both read World blocks, so the block outline snapped to a block that
// was not drawn, and a trench someone else dug was invisible right up until you fell into it.
// Reported from a live two-player session; the world eventually caught up only because
// streaming a column out and back in rebuilds its meshes from scratch.
//
// The fix cannot be a call to chunkRenderTouch() from this file. scene/ is 3DS-only — it pulls
// in citro3d — and net/networld.c is compiled on the host by net/networld_test.c, which is
// what makes any of this testable at all. So this module reports *which block changed* and the
// owner of the renderer decides what that means.
//
// Called on the thread that called networldUpdate()/networldOnColumnLoad(), from inside that
// call, once per edit, immediately after the block reaches the World — so the block is already
// readable with worldGet() by the time the hook runs. Edits still parked in net/blockdiff.h
// do not notify; they notify when they drain, which is the first moment there is anything to
// redraw. Optional: with no hook set (single player) nothing is called.
typedef void (*NetworldEditFn)(void* userdata, int x, int y, int z);

// Registers (or with fn == NULL, clears) the hook above. networldInit() clears it too, so a
// hook never survives from one session into the next.
void networldSetEditHook(NetworldEditFn fn, void* userdata);

// Encodes and sends one local edit to the server. Fire-and-forget: the caller's own worldSet()
// already applied it locally, so this never blocks on or waits for a server round trip. False
// if there is no session or the transport refused the send — both the ordinary "the packet
// did not make it" UDP outcome, not a caller error.
bool networldSendBlockEdit(int x, int y, int z, uint8_t block);

// Is there a live server session at all? (v1.8.7.)
//
// This exists because networldSendBlockEdit() above returns false for two situations that
// look identical to a caller and are not remotely the same thing:
//
//   * SINGLE PLAYER. There is no transport, so every send "fails". Nothing was supposed to
//     go out, nothing is out of step, and the local write is the whole truth.
//   * A LIVE SESSION whose packet did not leave this console — no socket, the AEAD encrypt
//     refused, or a short/failed sendto (net/bsnet_transport.c's send_app_packet). THAT is a
//     divergence: the client has applied an edit the server has never heard of.
//
// A caller that wants to undo or suppress something on a failed send must ask this first, or
// it will undo every edit in single player. scene/interact.c is the caller this was added for.
//
// True only in NET_TRANSPORT_ESTABLISHED. Deliberately NOT "the send would have succeeded":
// this answers "is there a server on the other end", and the send's own false answers the
// rest. Cheap enough to call per edit — it is one comparison against a cached enum.
bool networldSessionActive(void);

// ---- inventory sync -----------------------------------------------------------------------
//
// See proto/bs_proto.h's own long comment on BS_APP_INV_STATE/BS_APP_INV_ACTION for why the
// server always speaks first: an old client that has never heard an INV_STATE must never send
// an INV_ACTION, because an old server would kick it for a message type it does not recognise
// (server/game/bsgame.c's handle_app_payload default case) — the exact same asymmetry that
// header explains for BS_APP_CHUNK_SUB. That rule is enforced here, not left to the caller: see
// networldSendInvAction() below.

// Mirrors BS_INV_SLOT_COUNT (proto/bs_proto.h), restated as its own constant rather than
// pulling that header's include path into this one for a single integer — the same choice
// NETWORLD_MAX_REMOTE makes against BS_GAME_MAX_PLAYERS above.
#define NETWORLD_INV_SLOT_COUNT 24

// One inventory slot, decoded straight off the wire: (item id, stack count). Plain uint8 on
// purpose — this mirrors the wire shape exactly, not world/inventory.h's own item type, which
// this header and networld.c must not depend on (see this file's own header comment: the
// boundary this module holds is "decodes bytes", not "knows what an item is"). Whoever owns
// world/inventory.h converts these into its own types after the hook fires.
typedef struct {
	uint8_t item;
	uint8_t count;
} NetworldInvSlot;

// The whole decoded BS_APP_INV_STATE payload, handed to the hook below. A plain struct holding
// a fixed-size array, rather than a pointer + separate count: NETWORLD_INV_SLOT_COUNT is not a
// run-time quantity here — INV_STATE is always exactly that many slots, rejected on length
// otherwise (applyInvState(), networld.c) — so a count parameter would only ever carry one
// value, and a fixed-size array already says that in the type instead of in a comment.
typedef struct {
	uint8_t         selected_hotbar;
	NetworldInvSlot slots[NETWORLD_INV_SLOT_COUNT];
} NetworldInvState;

// Called once per valid BS_APP_INV_STATE, mirroring NetworldEditFn's own reasoning exactly:
// this module stays host-testable (net/networld_test.c) precisely because it never reaches
// into world/inventory.h itself, so it reports the decoded state and the owner of that header
// decides what to do with it. `state` is only valid for the duration of the call.
typedef void (*NetworldInvFn)(void* userdata, const NetworldInvState* state);

// Registers (or with fn == NULL, clears) the hook above. networldInit() clears it too, for the
// same reason networldSetEditHook()'s does — a hook left over from a previous session would
// point at UI state that session has already torn down.
void networldSetInvHook(NetworldInvFn fn, void* userdata);

// Encodes and sends one inventory/crafting op as BS_APP_INV_ACTION. Fire-and-forget, the same
// posture as networldSendBlockEdit() above — the caller's own local UI is authoritative either
// way, so this never blocks on or waits for a server round trip.
//
// Sends NOTHING, and returns false, until at least one BS_APP_INV_STATE has been received this
// session. This is not a defensive nicety: proto/bs_proto.h's whole reason INV_STATE is sent
// unprompted is so that a client which has not heard it — an old build, or one not yet far
// enough into a session — never originates INV_ACTION, which an old server would refuse
// outright. Checking "have we heard INV_STATE" here, rather than trusting every caller to check
// first, is what makes that guarantee actually hold rather than merely being documented. This
// is the capability probe itself, not a guard in front of it.
bool networldSendInvAction(uint8_t op, uint8_t a, uint8_t b, uint8_t c);

// ---- saved player state (v1.5.0) -----------------------------------------------------------
//
// BS_APP_PLAYER_STATE (S->C) / BS_APP_PLAYER_REPORT (C->S), the pair bs_proto.h added after
// the inventory one for everything worth persisting about a player *beyond* the inventory:
// where they stood, what they were wearing, their meters. The same deployment-order rule
// applies and is enforced here the same way: the server volunteers PLAYER_STATE at JOIN, so
// this module knows the server speaks it, and nothing is ever SENT back until it has — see
// networldSendPlayerReport() below.
//
// The join-time snapshot has exactly the arrival-order problem INV_STATE had, resolved the
// same way: JOIN completes on the title screen, main.c pumps networldUpdate() there, and the
// server sends PLAYER_STATE immediately after its INV_STATE — long before main.c has entered
// a world and built the Player the saved pose is meant to land in. So the decoded state is
// retained here (mirroring s_last_inv_state's reasoning in networld.c) and read back through
// the accessors below at the moment it becomes applicable: main.c consults
// networldSavedPose() right after playerInit(), which is this client's own equivalent of the
// inv hook's register-and-replay.

// Mirrors BS_ARMOR_SLOTS (proto/bs_proto.h), restated rather than included for one integer —
// the same choice NETWORLD_INV_SLOT_COUNT makes above, kept honest by networld.c's
// _Static_assert against the real constant.
#define NETWORLD_ARMOR_SLOTS 4

// The armour+meters block PLAYER_STATE carries after its pose, decoded raw and retained as-is.
// Deliberately dumb: this client has no armour, XP, health or hunger systems yet, so there is
// nothing here to convert into — the boundary this module holds is "decodes bytes", not
// "knows what health means". Consumed by survival systems when they land (roadmap §7); until
// then the block is held only so those systems can hook in without a wire redesign.
typedef struct {
	uint8_t  armor[NETWORLD_ARMOR_SLOTS][2];   // head/chest/legs/feet, {item, count}
	uint32_t xp_level;
	float    xp_progress;                      // 0..1 through the current level
	float    health;                           // 0..20
	float    hunger;                           // 0..20
} NetworldPlayerMeters;

// The pose from the most recent valid BS_APP_PLAYER_STATE with BS_PLAYER_STATE_FLAG_POSE set,
// written to the five out-parameters. True only when such a state arrived this session; false
// leaves every out-parameter untouched — the same contract networldWorldSeed() uses, and for
// the same reason: false means "the server had nothing saved" (or never spoke PLAYER_STATE at
// all), which the caller answers by keeping its own spawn choice. Values are the wire floats
// verbatim — see applyPlayerState() in networld.c for why they are not sanitised here.
bool networldSavedPose(float* out_x, float* out_y, float* out_z,
                       float* out_yaw, float* out_pitch);

// Whether the most recent valid PLAYER_STATE carried BS_PLAYER_STATE_FLAG_EXT (meaningful
// armour/meters), in which case networldPlayerMeters() returns the retained block. NULL
// otherwise — including when a state arrived with the flag clear ("fresh spawn": the server
// spoke, but had nothing saved), which is distinguishable from never hearing one at all by
// design (bs_proto.h).
bool networldPlayerMetersValid(void);
const NetworldPlayerMeters* networldPlayerMeters(void);

// Compile-honest meter gate. This client cannot fill in a single field of a PLAYER_REPORT
// today — no armour, no XP, no health, no hunger — and sending zeros would be worse than not
// sending: the server treats a report as authoritative and overwrites the state it persisted
// for this player, so an empty report WIPES a returning player's saved pose-and-meters on
// the very join that was supposed to restore them. Until survival systems exist and can hand
// real values to networldSendPlayerReport(), this stays 0 and the send path does not exist
// in the compiled binary at all. A future milestone flips it to 1 the day real meters do;
// the capability probe below keeps the wire side safe either way.
// v1.8.13 SURV-WIRE: flipped to 1. world/survival.c now simulates real health and hunger, and
// main.c builds each report by copying the RETAINED inbound meters block and overwriting only
// those two fields — so the armour and XP this client still has no system for round-trip back
// to the server untouched instead of going out as the zeros that the paragraph above warns
// would wipe them. The zero-wipe hazard was never about the wire format; it was about sending
// values this client could not source, and health and hunger it can now source.
//
// Nothing about the wire changed with this flip. BS_PLAYER_REPORT_BYTES is a fixed 30 built
// from BS_APP_HDR_BYTES + BS_PLAYER_METERS_BYTES + 5 in the shared proto header, with no
// dependency on this flag — the flag appears nowhere outside this file, networld.c and
// networld_test.c. All it does is decide whether the send path below is compiled in at all.
#ifndef BS_CLIENT_HAS_METERS
#define BS_CLIENT_HAS_METERS 1
#endif

// Encodes and sends this client's own armour+meters as BS_APP_PLAYER_REPORT, from `m`.
// Sends NOTHING, and returns false, until a valid PLAYER_STATE has been received this session
// — the capability probe again, identical in kind to networldSendInvAction()'s: an old server
// would kick this client for a message type it does not recognise. Independently gated on
// BS_CLIENT_HAS_METERS above, for the zero-wipe reason stated there.
bool networldSendPlayerReport(const NetworldPlayerMeters* m);


// ---- per-column diff subscription (V127-A) ---------------------------------------------------
//
// Why this pair exists at all, given BS_APP_WORLD_SYNC already replays every diff at JOIN: that
// replay is sized to the SERVER's whole diff store (BS_DIFF_MAX, deps/blocksmith-server's
// diffstore.h), and this client's own pending store (net/blockdiff.h's BLOCKDIFF_MAX_PENDING)
// had to be raised to match it just to stop losing a joining player's own house — see that
// header's comment and networld_test.c's test_rejoin_sync_larger_than_the_store_keeps_every_edit
// for the exact, previously-real bug. Scoping delivery to columns the player has actually
// streamed in is what lets the server's capacity grow past that number without this client's
// memory growing with it: the console only ever holds diffs for columns it currently has loaded
// (or has just asked for), never the server's entire history.
//
// Both calls are void and fire-and-forget, the same posture as networldSendBlockEdit() above:
// there is no session-status check here because netTransportSend() already reports "no session"
// as a plain false, and there is nothing a caller could usefully do with that beyond what already
// happens on its own — a lost SUB just means the column streams in with whatever WORLD_SYNC or a
// later resubscribe still recovers, not silence forever. Callers do not need the return value, so
// neither function has one.

// Tells the server this client now has column (col_x, col_z) loaded and wants its diffs — sends
// BS_APP_CHUNK_SUB. Call the instant a column becomes part of the live World, alongside
// networldOnColumnLoad(): main.c's genInstallOne() is that one place today (see this module's
// own .c file for exactly where). Calling this before the column is actually live would only
// mean any CHUNK_DIFFS batch that beat the install back gets queued the ordinary way, via the
// same pending store BLOCK_EDIT and WORLD_SYNC already share — not lost, just delayed to the
// next column load, so there is no strict ordering requirement between the two calls.
void networldSubscribeColumn(int col_x, int col_z);

// The other half: tells the server this client no longer wants diffs for column (col_x, col_z)
// — sends BS_APP_CHUNK_UNSUB. Call the instant a column leaves the live World: main.c's
// genUnloadColumn() is that one place today. A column dropped before ever being subscribed (see
// genInstallOne()'s stale-ring-position branch, which removes a column without ever having
// called networldSubscribeColumn() for it) does not need this call either — there is nothing to
// unsubscribe from the server did not already fail to hear about.
void networldUnsubscribeColumn(int col_x, int col_z);

// The seed of the world the server put us in, from BS_APP_WORLD_INFO. False until that packet
// arrives — which is immediately after JOIN, so in practice it is true well before the player
// has finished with the title screen, but a caller must handle false rather than assume: an
// older server never sends one, and in single player there is no session at all. False means
// "use this client's own seed", not "the seed is 0" — 0 is a value a server can legitimately
// own, which is why the seed comes back through an out-parameter.
bool networldWorldSeed(uint32_t* out);

// v1.8.3 Phase 4. WHICH GENERATOR the server says shaped that world, from BS_APP_WORLD_GEN
// (proto/bs_proto.h, 0x0F, one uint16 LE sent immediately behind WORLD_INFO).
//
// Same contract as networldWorldSeed() above and for the same reasons. False leaves *out
// untouched, and false is not an error: a pre-v1.8.3 server never sends this message at all,
// and in single player there is no session. False means "this server did not say", which
// world/genversion.h's genVersionForSessionResolve() answers with GEN_VERSION_LEGACY — the
// generator every client that ever joined such a server actually used. It does NOT mean "the
// version is 0"; 0 is simply not a generator, and the value travels through an out-parameter so
// a caller cannot read the two as the same thing.
//
// The value is the server's claim, verbatim, not something this client has agreed to. Deciding
// whether it can be honoured is genVersionForSessionResolve()'s job, and refusing is main.c's.
bool networldServerGenVersion(uint32_t* out);

// v1.8.20. The server's authoritative day/night counter, if one has arrived SINCE THE LAST
// CALL (BS_APP_TIME_SYNC, proto/bs_proto.h, 0x10, one uint64 LE, sent on join and then once a
// second). True means *out now holds it and the pending flag has been cleared.
//
// This is the one accessor in this header that is NOT a question you may ask repeatedly and
// get the same answer to. networldWorldSeed() and networldServerGenVersion() above latch: they
// mean "the server has told us, ever". This one consumes: it means "the server has told us
// again". Hence Take, not Get.
//
// That difference is load-bearing, not stylistic. The client advances its own clock every tick
// (world/daynight.h's dayNightAdvance) and the server corrects it once a second. If this
// latched, a caller polling every frame would re-pin the clock ~60 times a second to a value up
// to a second old, and time would visibly stop between packets — a worse failure than no sync
// at all, because a stopped clock still looks like a working feature until somebody waits for
// sunrise and it never comes.
//
// False is not an error, and is the normal answer on the great majority of frames: no packet
// has landed since the last call. It is also the permanent answer in single player, where there
// is no session, and after netDisconnect(), which clears the flag — so a caller that simply
// does nothing on false is correct everywhere, including in the world the player owns and which
// no server is entitled to move.
//
// The WHOLE counter travels, not a time of day, so day number and moon phase agree across the
// room as well as the light level. Apply it with dayNightSet(); no interpolation is needed at
// one second, because a correction of a few ticks moves dayLevel by less than a thousandth.
bool networldTakeTimeSync(uint64_t* out);

// ---- block registry sync (v1.6.0) ---------------------------------------------------------
//
// True when this client's block table provably agrees with the server's, and "provably" is meant
// literally: the join-time BS_APP_REGISTRY_INFO fingerprint (table revision + defined-row count
// + crc16 over the canonical table stream) is retained, and this is true exactly while the table
// as it stands reproduces all three. It is established by recomputing registryCount() and
// registryCrc16() — when the INFO arrives, and again after every BS_APP_REGISTRY_DEFS batch.
//
// It is deliberately NOT established by the LAST flag on a DEFS batch. The server sends an empty
// terminating batch unconditionally, so that flag can arrive with nothing applied at all — a
// data batch lost in the UDP, or a fingerprint mismatch the server has no dynamic rows to answer
// with — and taking it as proof left this client insisting the sync had succeeded while every
// server-defined block was an air hole. The flag says the server has finished talking; only the
// fingerprint says the table is right. One consequence worth knowing: a delivery whose
// terminator is the packet that gets lost still comes out synced, because the table is what is
// checked and by then the table is correct.
//
// False is a real, supported, DEGRADED state, not an error: a block id with no row in the table
// resolves through registryView()'s never-NULL contract to the air row, so a dynamic block this
// client never learned about is a hole in the world rather than a crash or a wrong block. Ids
// are stored raw either way (see editValid() in networld.c), so the hole fills itself in the
// instant the definitions do arrive. Always false in single player and against a pre-v1.6.0
// server, neither of which has a server table to agree with — so a caller must read this as
// "is the multiplayer table trustworthy", never as "may I draw the world".
bool networldRegistrySynced(void);

// True while world entry should be held open for the registry to settle, and the reason this
// pair exists at all. main.c's registryFreeze() (in genStart) makes the table permanently
// read-only, and BS_APP_REGISTRY_DEFS arrive a round trip behind the BS_APP_WORLD_INFO seed
// that scene/title.c enters the world on — so entering the instant the seed landed froze the
// table before a single dynamic row could ever be committed, in every session, forever.
//
// Bounded in every arm, so a caller that spins on this is guaranteed to be let through:
//   * never armed (single player, or pre-WORLD_INFO)  -> false immediately
//   * synced                                          -> false immediately
//   * joined but no INFO heard (old server)           -> false after a short grace
//   * INFO heard, fetch still outstanding             -> false at the sync deadline
//
// v1.8.7 changed what falling through the LAST of those four means, and only that one. Dropping
// out of it used to enter the world with networldRegistrySynced() false — the degraded state
// documented above, which on a CORE-row disagreement is permanent, unrepairable and effectively
// invisible to the player (registryVerdictTick() in networld.c carries the whole argument). It
// now ends the session instead: networldUpdate() takes the verdict on that exact edge, refuses
// the server through netTransportRefuse() with a line saying the two builds are different
// versions, and clears this module the way netDisconnect() does — so networldWorldSeed() goes
// false on the same frame and scene/title_nav.h's entry gate closes rather than opening.
//
// The no-INFO arm is untouched and must stay that way: a server that never sent one named no
// fingerprint, so nothing about its table has been disproved, and it joins degraded exactly as
// it always has. Both remaining fall-throughs still leave networldRegistrySynced() false, which
// is still the documented degraded state above rather than a failure to report.
bool networldRegistryWaiting(void);

// v1.8.3 Phase 4. True while this session is still waiting to hear BS_APP_WORLD_GEN, in exactly
// the sense networldRegistryWaiting()'s no-INFO arm is: a caller that gates world entry on this
// will not enter until the server has either answered or run out of grace.
//
// It exists to close a one-frame race, and the race is the whole feature failing quietly rather
// than loudly. WORLD_GEN is sent one line behind WORLD_INFO, so in practice it has always
// arrived by the time scene/title_nav.c's entry condition is next evaluated — but "in practice"
// is a timing assumption about two datagrams, not a guarantee. A client that entered on the
// frame between them would see networldServerGenVersion() answer false, conclude "old server,
// generate legacy", and walk into a world the server was one packet away from saying was
// something else. That is precisely the harm this phase exists to stop, delivered by a race
// instead of by a version disagreement, and with both sides believing they agree.
//
// Bounded in every arm, so a caller that spins on it is guaranteed to be let through:
//   * never armed (single player, or pre-WORLD_INFO)  -> false immediately
//   * WORLD_GEN heard                                 -> false immediately
//   * joined, nothing heard (a pre-v1.8.3 server)     -> false after NETWORLD_GEN_GRACE_MS
// Falling through the last arm leaves networldServerGenVersion() false, which is the documented
// "server did not say" answer and not a failure to report.
bool networldGenWaiting(void);

// The bounds behind both functions above, driven off bsSockNowMs(). They are here rather than
// private to networld.c for the same reason NETWORLD_POSE_INTERVAL_MS is: a timing bound no
// test can name is a timing bound no test can prove, and net/networld_test.c exercises every
// one of these against a fake clock.
//
//   RETRY_MS       gap between one BS_APP_REGISTRY_FETCH and the next
//   MAX_SENDS      total FETCHes per session, the first one included (the attempt bound)
//   SYNC_DEADLINE  how long after the join the registry question stays open (the time bound)
//   INFO_GRACE     the much shorter wait for BS_APP_REGISTRY_INFO itself. A pre-v1.6.0 server
//                  never sends one, and stalling every join against one for the full deadline
//                  to wait for a packet that does not exist is a regression the player feels.
#define NETWORLD_REG_FETCH_RETRY_MS    250
#define NETWORLD_REG_FETCH_MAX_SENDS   4
#define NETWORLD_REG_SYNC_DEADLINE_MS  2000
#define NETWORLD_REG_INFO_GRACE_MS     250

// v1.8.3 Phase 4. How long networldGenWaiting() holds entry for BS_APP_WORLD_GEN when nothing
// has arrived. Here rather than private to networld.c for the reason the four above are: a
// timing bound no test can name is a timing bound no test can prove.
//
// The same 250 ms as REG_INFO_GRACE, and for an identical reason rather than a copied number:
// both are the wait for a single packet that a server one release older simply does not send,
// and both are measured from the same instant (the arrival of WORLD_INFO). Stalling every join
// against a pre-v1.8.3 server for any longer would be a regression the player can feel, for a
// packet that is never coming.
#define NETWORLD_GEN_GRACE_MS          250

// Diagnostics, mirroring net/blockdiff.h's own counters — this module owns the store
// privately, so a HUD or log line reaches these instead of the store directly.
//
// Both are safe, and both are honest, when there is no store at all — which is the whole of a
// single-player session, since v1.8.5 made the store a lazy allocation (networld.c, s_pending).
// Count reads 0 because nothing has been queued. Refusals reads the number of edits dropped,
// which since v1.8.5 includes any dropped because the store could not be ALLOCATED as well as
// the ones blockdiff.c turned away for being full: to the player both mean "this session lost
// remote edits", which is the one question main.c's "X" marker asks of this.
int networldPendingCount(void);
int networldPendingRefusals(void);

// True only while a column's queued diffs are being replayed into the world — that is, only
// inside networldOnColumnLoad()'s blockdiffDrain(), for the duration of the writes it makes.
// False everywhere else, including during a live remote edit, during the bulk world sync, and
// at every instant no column is installing.
//
// What false does NOT mean: it does not mean the write is the local player's. A remote player's
// live edit arrives with this false, because it is not a replay — it is one write, in the frame
// it happened, and it costs the simulation nothing to hear about.
//
// It exists for exactly one caller. main.c's world edit hook uses it to decide whether a write
// may be run past world/water.h's waterReplaySkippable() before it notifies the water
// simulation. A replay lands a whole column's backlog in one uncapped burst — 9800 diffs
// measured — which is enough to evict the player's own placement out of the water queue before
// it is ever examined; 47 of 60 lost, measured 2026-08-30. A live edit is one write and is never
// filtered. See water.h for the full measurement and for why blanket suppression is wrong.
bool networldReplayingDiffs(void);

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
// Mirrors BS_GAME_MAX_PLAYERS (16, deps/blocksmith-server/game/players.h) minus the local
// player. That claim went unguarded until 2026-08-25, while the other two mirror claims in this
// header were not; it is guarded now, by the pair of _Static_asserts at the top of
// net/networld_test.c. They sit there rather than beside the mirror asserts at
// net/networld.c:33-36 — the natural home — because BS_GAME_MAX_PLAYERS is not
// reachable from that translation unit: networld.c includes only proto/bs_proto.h, which does
// not define it, and pulling a server header into production client code for one integer is
// the very thing this restated constant exists to avoid. The gap that leaves, said plainly:
// drift is caught when the HOST suite compiles, not by the 3DS build.
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

#ifndef __3DS__
// Forces the next pending-store allocation (networld.c, pendingStore()) to fail, so the host
// suite can drive the out-of-memory branch that decides whether a dropped remote edit is
// counted or silently swallowed.
//
// Compiled out of every console build, and gated on __3DS__ rather than a -D switch for a
// concrete reason: tools/run_host_tests.sh builds networld.c with no extra defines, so a
// switch nothing turns on would leave the branch exactly as unreachable as it is without this.
// A 1 MB malloc does not fail on demand on a host with gigabytes free, and an unreachable
// branch is an unchecked one — which is how a silent-loss path gets shipped.
//
// Latching: while `on` is true EVERY attempt fails, so a test can prove the retry path counts
// each dropped edit rather than only the first. Turn it back off to let the store be created.
void networldTestForcePendingAllocFail(bool on);

// True while the pending store exists.
//
// This is here because the reclaim's actual claim — single player never allocates the
// megabyte — turned out to be unobservable from outside without it. networldPendingCount()
// answers 0 for an absent store AND for an allocated empty one, so a check written on the
// count alone stays green against a networldOnColumnLoad() that allocates one per column.
// MEASURED, not argued: that regression was injected (the drain guard replaced with a
// pendingStore() call) and the count-only check did not move — "PASS 386 checks, 0 failed",
// identical to the healthy run. This probe is what makes it go red.
bool networldTestPendingStoreAllocated(void);
#endif
