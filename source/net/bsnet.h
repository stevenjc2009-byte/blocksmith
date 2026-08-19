/* bsnet.h — Blocksmith multiplayer client, UI-agnostic interface.
 *
 * This is the whole surface the menu code needs. Nothing in here draws,
 * touches the screen, reads input, or blocks. Call netUpdate() once a frame
 * and read the status back; every call below returns immediately.
 *
 * Typical multiplayer screen:
 *
 *     netInit();                                  // once, at boot
 *     ...
 *     printf("Server: %s\n", netServerAddress()); // what to display
 *     if (pressed_connect) netConnect();
 *
 *     netUpdate();                                // every frame
 *     printf("%s\n", netStatusText());
 *     if (netStatus() == NET_CONNECTED) enter_the_world();
 *
 * The address shown comes from the build/SD config, not from the user: playit
 * assigns it, and typing a hostname on a 3DS keyboard is a bad time. See
 * netServerAddress() for where it is read from.
 */

#ifndef BSNET_H
#define BSNET_H

#include <stdbool.h>
#include <stdint.h>

/* Longest server address string, including the port and the terminator.
 * playit hostnames look like "something-words.at.ply.gg:41234". */
#define NET_ADDR_MAX      96

/* Mirrors the gateway's BS_MAX_SESSIONS. The server refuses the 17th player,
 * so the client never needs room for more. */
#define NET_MAX_PLAYERS   16

/* Matches BS_ALLOW_LABEL_MAX on the server: the label the server knows a
 * player by, which is what should be shown in a player list. */
#define NET_NAME_MAX      32

/* ---------------------------------------------------------------- status */

typedef enum {
    NET_IDLE = 0,     /* not connected, nothing in flight — Connect is valid */
    NET_CONNECTING,   /* socket up, handshake in progress                    */
    NET_CONNECTED,    /* encrypted session established, safe to enter world  */
    NET_FAILED        /* last attempt failed; netErrorText() says why        */
} NetStatus;

/* ------------------------------------------------------------- lifecycle */

/* Brings up the 3DS socket service and loads the server address and keys.
 * Safe to call once at boot even if the player never opens Multiplayer —
 * it does not touch the network. Returns false if SOCU could not start, in
 * which case every other call here is a harmless no-op and netStatus() stays
 * NET_IDLE with netErrorText() explaining it. */
bool netInit(void);

/* Tears down the session and the socket service. Safe to call when never
 * connected. */
void netExit(void);

/* --------------------------------------------------------------- session */

/* Begins connecting. Returns false immediately if a connection is already up
 * or in progress, or if netInit() failed. Never blocks: the handshake runs
 * across subsequent netUpdate() calls, so the menu keeps rendering while it
 * happens. */
bool netConnect(void);

/* Longest invite code this build will send, plus the terminator. Mirrors
 * BS_INVITE_CODE_MAX in the server's proto/bs_proto.h. Codes are ten symbols
 * shown as XXXXX-XXXXX, so the extra room is only there to let a player type
 * spaces or lowercase without the console silently truncating what it sends —
 * the server does the normalising. */
#define NET_INVITE_MAX    33

/* Begins connecting with a one-time invite code, for a console that is not on
 * the server's allowlist yet. Same handshake, same states, same netUpdate()
 * loop as netConnect(); the difference is that once the handshake lands, the
 * code is offered and the client waits out the server's ten-second enrolment
 * window instead of the ordinary handshake timeout.
 *
 * Success is an ordinary NET_CONNECTED — the console is on the allowlist from
 * that moment and every later join is a plain netConnect(). Failure is an
 * ordinary NET_FAILED with netErrorText() explaining that the code did not
 * work; the server deliberately does not say whether it was wrong, expired,
 * already used or never armed, so neither does this.
 *
 * `code` is sent exactly as given. Do not trim, upper-case or validate it
 * first: the server normalises, so a check here could only reject something
 * the server would have accepted. Anything past NET_INVITE_MAX-1 bytes is
 * dropped. Returns false on the same conditions as netConnect(), plus a NULL
 * or empty code.
 *
 * A retry after failure means calling this again from scratch — the server
 * discards the session on a wrong code, so nothing can be re-sent on it. */
bool netConnectWithInvite(const char *code);

/* True if the connection currently up was established by enrolling, i.e. this
 * console just joined the allowlist. Only useful for saying so once on the
 * screen; nothing else should branch on it, because from the next connect
 * onwards an enrolled console is indistinguishable from any other. */
bool netJustEnrolled(void);

/* Ends the session, telling the server so the player slot frees at once
 * rather than after the 30-second idle timeout. Returns to NET_IDLE. Safe to
 * call in any state. */
void netDisconnect(void);

/* Pumps the network. Call once per frame from the main loop, in every scene,
 * including the menu — the handshake and the keepalives both live here.
 * Bounded work per call; it will not stall a frame waiting on a packet. */
void netUpdate(void);

/* ---------------------------------------------------------------- status */

NetStatus netStatus(void);

/* One short line suitable for printing straight onto the screen, already
 * phrased for a player rather than a developer. Never NULL, never longer than
 * 48 characters, so it fits the bottom screen without wrapping.
 *
 *   NET_IDLE        "Not connected"
 *   NET_CONNECTING  "Connecting..."
 *   NET_CONNECTED   "Connected - 3 players"
 *   NET_FAILED      "Could not reach the server"
 */
const char *netStatusText(void);

/* Why the last attempt failed, in plain words. Empty string if nothing has
 * failed. Never NULL. Worth showing under netStatusText() when the status is
 * NET_FAILED, because "server is full" and "your key is not on the allowlist"
 * need very different responses from the player. */
const char *netErrorText(void);

/* The server address this build talks to, as "host:port", for display.
 * Never NULL; reads "(not configured)" if no address could be found.
 *
 * Resolved once during netInit(), in this order:
 *   1. /blocksmith/server.txt on the SD card, if present — one line, "host:port".
 *      This exists so a new playit address does not need a new CIA.
 *   2. the address compiled into the build.
 */
const char *netServerAddress(void);

/* Round-trip time to the server in milliseconds, or -1 if not connected or
 * not yet measured. For a HUD readout. */
int netPingMs(void);

/* ---------------------------------------------------------- player list */

/* Number of players currently in the session, including the local player.
 * 0 when not connected. */
int netPlayerCount(void);

/* Name of player `index`, where index is 0 .. netPlayerCount()-1. The server
 * sends no roster and no labels to clients — it only ever tells a peer's sid
 * in a pose update — so this is a placeholder tag derived from that sid
 * ("Player xxxx", low 16 bits, hex), not the label from the allowlist.
 * Returns NULL if the index is out of range.
 *
 * The list is only stable within a single frame — a player can leave between
 * frames, so re-read netPlayerCount() each time rather than caching it. */
const char *netPlayerName(int index);

/* True if player `index` is the local player, for highlighting them in a
 * list. False if the index is out of range. */
bool netPlayerIsLocal(int index);

/* --------------------------------------------------------------- identity */

/* This console's public key as 64 lowercase hex characters plus a
 * terminator: exactly the first field of a line in the server's allowlist,
 * ready to paste into `bsgate-keys add <key> <label>`. Reads "(unavailable)"
 * if no identity exists. Never NULL. */
const char *netLocalKeyHex(void);

#endif /* BSNET_H */
