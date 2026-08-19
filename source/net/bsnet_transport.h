/* bsnet_transport.h — the encrypted link, below the menu-facing API.
 *
 * Internal to source/net/. Menu and scene code should use net/bsnet.h; nothing
 * outside this folder should include this header.
 *
 * This layer owns the UDP socket and the Noise XX handshake described in
 * bs_proto.h. It deals in opaque payload bytes and knows nothing about blocks,
 * players or chunks — the game layer above gives it a buffer and gets buffers
 * back.
 *
 * Where bs_proto.h lives: it is defined in the blocksmith-server repo, as
 * proto/bs_proto.h there, and this repo deliberately keeps no copy of it — one
 * editable copy of a wire format is the point. `make deps` clones that repo
 * pinned to a commit into deps/blocksmith-server, which is on the include path,
 * so every include of it here reads "proto/bs_proto.h".
 *
 * It used to read "../../server/proto/bs_proto.h": a reach up out of this repo
 * and into a sibling checkout that happened to sit beside it. That built on the
 * one machine that had both and nowhere else, which is how a released binary
 * came to contain networking code that no clone of this repo could compile.
 */

#ifndef BSNET_TRANSPORT_H
#define BSNET_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest payload one call can carry. Mirrors BS_MAX_PAYLOAD in bs_proto.h;
 * the gateway silently drops anything larger, so the game layer must split. */
#define NET_MAX_PAYLOAD 1024

typedef enum {
    NET_TRANSPORT_IDLE = 0,
    NET_TRANSPORT_HANDSHAKING,
    NET_TRANSPORT_ESTABLISHED,
    NET_TRANSPORT_FAILED
} NetTransportState;

/* Brings up SOCU and the crypto library. No network traffic. */
bool netTransportInit(void);
void netTransportExit(void);

/* Starts a handshake. Returns false only for an immediate local failure (bad
 * host, no socket); a server that never answers surfaces later as
 * NET_TRANSPORT_FAILED from netTransportState(). */
bool netTransportConnect(const char *host, uint16_t port);

/* Sends a disconnect if a session is up, then tears down. Safe in any state. */
void netTransportDisconnect(void);

/* Drives handshake retries, keepalives and timeouts. Bounded work; never
 * blocks. Call once per frame. */
void netTransportUpdate(void);

NetTransportState netTransportState(void);

/* Why the last failure happened, in words a player can act on. Never NULL. */
const char *netTransportError(void);

/* Smoothed round-trip time in ms, or -1 if not established or not yet known. */
int netTransportPingMs(void);

/* Copies this console's 32-byte public identity key into `out`. False if no
 * identity exists (netTransportInit() has not run, or the seed could not be
 * minted), in which case `out` is untouched. */
bool netTransportLocalPublicKey(uint8_t out[32]);

/* Encrypts and sends one payload. False if there is no session, the payload is
 * too large, or the socket would block — UDP, so callers must treat every send
 * as best-effort and never assume delivery. */
bool netTransportSend(const uint8_t *payload, size_t len);

/* Pops one decrypted, replay-checked payload into `out`.
 * Returns the byte count, 0 if nothing is queued, or -1 on error. */
int netTransportRecv(uint8_t *out, size_t cap);

#endif /* BSNET_TRANSPORT_H */
