/* bsnet_transport.c — the encrypted link. Client-side mirror of
 * server/gateway/bsgate.c; read that file's header comment for the protocol
 * diagram this implements.
 *
 * Wire sequence (client drives, server answers — see bs_proto.h for exact
 * byte layouts):
 *
 *   1. hydro_kx_xx_1()                          -> packet1 (ephemeral)
 *   2. send HELLO carrying packet1's first 32 bytes as the cookie nonce
 *   3. recv COOKIE
 *   4. send KX1 = cookie + the SAME packet1 from step 1 (never regenerated —
 *      a fresh ephemeral here would silently invalidate the cookie)
 *   5. recv KX2 (session id + Noise packet2)
 *   6. hydro_kx_xx_3() -> session keys, packet3, the server's revealed static
 *      key — checked against the configured server key here, because Noise
 *      XX authenticates whoever answers, not specifically "the real server"
 *   7. send KX3 = sid + packet3
 *   8. from here the session exists server-side, but the gateway never sends
 *      an explicit accept — a peer whose key is not on the allowlist gets
 *      total silence. So: keep resending KX3 plus an empty liveness DATA
 *      packet until either something authenticated arrives on this sid
 *      (-> ESTABLISHED) or the attempt times out (-> FAILED, allowlist
 *      wording)
 *   9. AEAD DATA both ways, BS_CTX_C2S / BS_CTX_S2C, sliding replay window
 *      on receive (bsnet_replay.c)
 *
 * The only platform-specific code (bringing SOCU up/down, a monotonic clock)
 * is behind bsnet_sock.h; every socket call here is plain POSIX and compiles
 * unchanged for both the 3DS and the Linux host test in
 * source/net/hosttest/.
 */

#include "bsnet_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <hydrogen.h>

#include "bsnet_replay.h"
#include "bsnet_sock.h"
#include "proto/bs_proto.h"

/* The wire literals in bs_proto.h are duplicated so it stays dependency-free;
 * bsgate.c static-asserts them against libhydrogen on the server side, so the
 * client checks the same thing here rather than trusting the two builds
 * agree. */
_Static_assert(BS_KX_PUBLICKEYBYTES == hydro_kx_PUBLICKEYBYTES, "kx pk size drift");
_Static_assert(BS_KX_PACKET1BYTES   == hydro_kx_XX_PACKET1BYTES, "kx p1 size drift");
_Static_assert(BS_KX_PACKET2BYTES   == hydro_kx_XX_PACKET2BYTES, "kx p2 size drift");
_Static_assert(BS_KX_PACKET3BYTES   == hydro_kx_XX_PACKET3BYTES, "kx p3 size drift");
_Static_assert(BS_AEAD_HEADERBYTES  == hydro_secretbox_HEADERBYTES, "aead hdr drift");
_Static_assert(sizeof BS_CTX_S2C - 1 == hydro_secretbox_CONTEXTBYTES, "ctx len");
_Static_assert(sizeof BS_CTX_C2S - 1 == hydro_secretbox_CONTEXTBYTES, "ctx len");
_Static_assert(NET_MAX_PAYLOAD == BS_MAX_PAYLOAD, "payload size drift");

/* ---------------------------------------------------------------- config --
 * Same convention as bsnet.c's NET_ADDR_FILE: sdmc:/blocksmith/ holds
 * whatever a build needs that should not require a new CIA to change.
 * Overridable at compile time (BS_NET_DIR) so the host test harness in
 * source/net/hosttest/ can point this whole file at a real, throwaway
 * directory instead of the 3DS-only "sdmc:/" prefix. */
#ifndef BS_NET_DIR
#define BS_NET_DIR "sdmc:/blocksmith"
#endif

/* Raw 32-byte hydro_kx seed. Generated once and persisted: this is the
 * player's identity, and it is what the server's allowlist actually knows —
 * losing it means losing your spot on every friend's allowlist. */
#define BS_CLIENT_SEED_FILE  BS_NET_DIR "/client.seed"

/* Hex mirror of this device's public key, rewritten every netTransportInit().
 * Lets a player read their own key off the SD card instead of copying it off
 * a 240px screen; format matches the first field of a bsgate-keys allowlist
 * line so it is paste-ready there. */
#define BS_CLIENT_PUB_FILE   BS_NET_DIR "/client.pub"

/* Hex text, one line, 64 characters — copy-pasted straight out of
 * `bsgate --print-identity` / `bsgate-keys identity`, which print in the same
 * format. Both are required before Connect will do anything. */
#define BS_SERVER_PK_FILE    BS_NET_DIR "/server.pub"
#define BS_NETWORK_PSK_FILE  BS_NET_DIR "/network.psk"

/* Values baked into the build, so a copy of the game handshakes with the right
 * server straight out of the box the way BS_SERVER_ADDR points it at the right
 * host. The SD files above still win when present, which is what makes a second
 * or a test server possible without a rebuild.
 *
 * Empty means "not configured" and Connect refuses, which is the honest state
 * for a build nobody has pointed at a server.
 *
 * The server's public key is committed. A public key is meant to be public:
 * publishing it lets anyone verify they are talking to this gateway and lets
 * nobody impersonate it. It is steve's gateway, read out of `bsgate-keys
 * identity` inside the container on 2026-08-19, and it pairs with
 * BS_SERVER_ADDR in bsnet.c — change the server and this changes with it.
 *
 * The network PSK is deliberately NOT committed. It is a shared secret: holding
 * it gets you as far as completing a handshake, at which point the allowlist is
 * what actually decides whether you are let in. That makes it "reached the
 * gate", not "got in" — but this repository is public, and a secret sitting in
 * public source is one grep away rather than one reverse-engineer away.
 *
 * Supply it at build time instead:
 *
 *     make cia BS_PSK=<64 hex characters>
 *
 * The Makefile turns that into -DBS_NETWORK_PSK_HEX and the `cia` target
 * refuses to package without it, so a release build cannot silently ship
 * unable to connect. A clone without the PSK still compiles and runs; it just
 * says so at the Connect screen instead of failing obscurely. */
#ifndef BS_SERVER_PK_HEX
#define BS_SERVER_PK_HEX "2347a5d714537b8d97515e8a62dff87bbdde9921a5468b446507afa2512a401b"
#endif
#ifndef BS_NETWORK_PSK_HEX
#define BS_NETWORK_PSK_HEX ""
#endif

#ifdef BSNET_TRANSPORT_DEBUG_LOG
#define BSLOG(...) do { fprintf(stderr, "[bsnet] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
#else
#define BSLOG(...) do { } while (0)
#endif

/* ------------------------------------------------------------- constants --
 * Retry pacing assumes a playit relay adding 10-50ms and occasional loss or
 * reordering, not a broken link: several quick retries within the first
 * second or two catch almost everything, and giving up after ~3s per phase
 * keeps "Connecting..." from hanging on a genuinely dead server. */
#define BS_HS_RETRY_MS        400u
#define BS_HS_MAX_TRIES         8u
#define BS_WELCOME_RETRY_MS   400u
#define BS_WELCOME_MAX_TRIES     8u
#define BS_KEEPALIVE_MS      4000u
#define BS_RX_QUEUE_LEN         16u

/* How long an enrolment attempt waits for BS_PKT_ENROL_OK. The server gives a
 * probation session BS_ENROL_WINDOW_MS (10s) from the moment it finishes the
 * handshake; this clock starts when *we* accept KX2, which is strictly earlier
 * — KX3 still has to cross the wire — so waiting exactly 10s would sometimes
 * give up while the server was still listening. The margin covers that skew
 * plus one relay round trip. Failing slightly late costs a couple of seconds;
 * failing early costs the invite, since a retry burns a fresh handshake and
 * three wrong-looking attempts kill the code for everyone. */
#define BS_ENROL_WINDOW_MS  10000u
#define BS_ENROL_MARGIN_MS   2000u
#define BS_ENROL_WAIT_MS    (BS_ENROL_WINDOW_MS + BS_ENROL_MARGIN_MS)

static const uint8_t EMPTY_PAYLOAD[1] = { 0 };

/* --------------------------------------------------------------- state ---- */

typedef enum {
    CSTATE_NONE = 0,
    CSTATE_SENT_HELLO,
    CSTATE_SENT_KX1,
    CSTATE_AWAIT_WELCOME, /* KX3 sent; waiting for the first proof the server admitted us */
    CSTATE_RUNNING
} ClientSubstate;

struct rx_item {
    uint8_t data[NET_MAX_PAYLOAD];
    size_t  len;
};

static struct {
    bool inited;
    int  fd;

    hydro_kx_keypair static_kp;   /* this device's persistent identity */
    bool             have_client_id;

    uint8_t server_pk[BS_KX_PUBLICKEYBYTES];
    bool    have_server_pk;
    uint8_t psk[hydro_kx_PSKBYTES];
    bool    have_psk;

    struct sockaddr_in server_addr;

    NetTransportState  state;
    ClientSubstate      substate;
    char                error[160];

    hydro_kx_state kx_state;
    uint8_t packet1[BS_KX_PACKET1BYTES];
    uint8_t cookie[BS_COOKIE_BYTES];
    uint8_t kx1_pkt[BS_KX1_BYTES];
    uint8_t kx3_pkt[BS_KX3_BYTES];

    uint32_t sid;
    hydro_kx_session_keypair keys;
    uint64_t tx_msg_id;
    struct bsnet_replay rx_replay;

    unsigned phase_tries;
    uint64_t phase_deadline_ms;

    /* Enrolment. `armed` survives across a connect so the code typed on the
     * menu is still there when the handshake reaches probation; `sent` and
     * `ok` are per-attempt. The code itself is never written to the SD card
     * and is wiped the moment it has been sent. */
    bool     enrol_armed;
    bool     enrol_sent;
    bool     enrol_ok;
    uint8_t  enrol_code[BS_INVITE_CODE_MAX];
    size_t   enrol_len;

    uint64_t last_send_ms;
    bool     probe_outstanding;
    uint64_t probe_sent_ms;
    int      ping_ms;

    struct rx_item rxq[BS_RX_QUEUE_LEN];
    unsigned rxq_head;
    unsigned rxq_count;
} s;

/* ---------------------------------------------------------------- errors -- */

static void set_error(const char *msg)
{
    snprintf(s.error, sizeof s.error, "%s", msg);
}

/* ----------------------------------------------------------- config load -- */

static void trim_tail(char *str)
{
    size_t n = strlen(str);
    while (n > 0 && (str[n - 1] == '\n' || str[n - 1] == '\r'
                     || str[n - 1] == ' ' || str[n - 1] == '\t')) {
        str[--n] = '\0';
    }
}

/* Reads one line of exactly 2*out_len hex characters. Comments/blanks are
 * skipped, same as bsnet.c's server.txt reader and the gateway's allowlist. */
static bool load_hex_key(const char *path, uint8_t *out, size_t out_len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return false;

    char line[256];
    bool got = false;
    while (!got && fgets(line, sizeof line, f) != NULL) {
        trim_tail(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strlen(line) != out_len * 2) { fclose(f); return false; }
        if (hydro_hex2bin(out, out_len, line, out_len * 2, NULL, NULL) < 0) {
            fclose(f);
            return false;
        }
        got = true;
    }
    fclose(f);
    return got;
}

/* Decodes one of the baked-in hex constants. Same length and alphabet rules as
 * load_hex_key(), because a value that would be rejected off the SD card must
 * not sneak in through the compile-time door. */
static bool load_hex_literal(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || strlen(hex) != out_len * 2) return false;
    return hydro_hex2bin(out, out_len, hex, out_len * 2, NULL, NULL) >= 0;
}

/* Loads the persisted identity seed, or mints one and saves it. Failure to
 * persist is not fatal to this run — only to surviving a relaunch — because a
 * freshly written SD card or a read-only blocksmith/ folder should not block
 * booting into single-player. */
static bool load_or_create_client_seed(uint8_t seed[hydro_kx_SEEDBYTES])
{
    FILE *f = fopen(BS_CLIENT_SEED_FILE, "rb");
    if (f != NULL) {
        size_t n = fread(seed, 1, hydro_kx_SEEDBYTES, f);
        fclose(f);
        if (n == hydro_kx_SEEDBYTES) {
            BSLOG("loaded client identity from %s", BS_CLIENT_SEED_FILE);
            return true;
        }
        BSLOG("%s is not %d bytes, regenerating", BS_CLIENT_SEED_FILE, (int)hydro_kx_SEEDBYTES);
    }

    hydro_random_buf(seed, hydro_kx_SEEDBYTES);

    f = fopen(BS_CLIENT_SEED_FILE, "wb");
    if (f == NULL) {
        BSLOG("could not persist %s (identity will not survive a restart)", BS_CLIENT_SEED_FILE);
        return true;
    }
    size_t n = fwrite(seed, 1, hydro_kx_SEEDBYTES, f);
    fclose(f);
    if (n != hydro_kx_SEEDBYTES) BSLOG("short write on %s", BS_CLIENT_SEED_FILE);
    else BSLOG("generated and saved a new client identity at %s", BS_CLIENT_SEED_FILE);
    return true;
}

/* Writes this device's public key as lowercase hex to BS_CLIENT_PUB_FILE.
 * Purely informational — failure here must not fail netTransportInit(), same
 * posture as the seed-persist failure path above. */
static void write_client_pub_file(const hydro_kx_keypair *kp)
{
    char hex[BS_KX_PUBLICKEYBYTES * 2 + 1];
    if (hydro_bin2hex(hex, sizeof hex, kp->pk, sizeof kp->pk) == NULL) {
        BSLOG("could not hex-encode the client public key");
        return;
    }

    FILE *f = fopen(BS_CLIENT_PUB_FILE, "wb");
    if (f == NULL) {
        BSLOG("could not write %s (key will not be readable off the SD card)", BS_CLIENT_PUB_FILE);
        return;
    }
    size_t hex_len = strlen(hex);
    size_t n = fwrite(hex, 1, hex_len, f);
    if (n == hex_len) n += fwrite("\n", 1, 1, f);
    fclose(f);
    if (n != hex_len + 1) BSLOG("short write on %s", BS_CLIENT_PUB_FILE);
    else BSLOG("wrote client public key to %s", BS_CLIENT_PUB_FILE);
}

/* ---------------------------------------------------------------- socket -- */

static bool resolve_host(const char *host, uint16_t port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return true;

    struct hostent *he = gethostbyname(host);
    if (he == NULL || he->h_addrtype != AF_INET || he->h_length != 4
        || he->h_addr_list[0] == NULL) {
        return false;
    }
    memcpy(&out->sin_addr, he->h_addr_list[0], 4);
    return true;
}

static bool open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    s.fd = fd;
    return true;
}

static void close_socket(void)
{
    if (s.fd >= 0) { close(s.fd); s.fd = -1; }
}

static void udp_send(const uint8_t *buf, size_t len)
{
    if (s.fd < 0) return;
    (void)sendto(s.fd, buf, len, 0, (struct sockaddr *)&s.server_addr, sizeof s.server_addr);
    s.last_send_ms = bsSockNowMs();
}

/* ------------------------------------------------------- handshake frames -- */

static void send_hello(void)
{
    uint8_t pkt[BS_HELLO_BYTES];
    memset(pkt, 0, sizeof pkt);
    bs_put_hdr(pkt, BS_PKT_HELLO);
    memcpy(pkt + BS_HDR_BYTES, s.packet1, BS_NONCE_BYTES);
    udp_send(pkt, sizeof pkt);
    BSLOG("-> HELLO");
}

static void build_and_send_kx1(void)
{
    bs_put_hdr(s.kx1_pkt, BS_PKT_KX1);
    memcpy(s.kx1_pkt + BS_HDR_BYTES, s.cookie, BS_COOKIE_BYTES);
    memcpy(s.kx1_pkt + BS_HDR_BYTES + BS_COOKIE_BYTES, s.packet1, sizeof s.packet1);
    udp_send(s.kx1_pkt, sizeof s.kx1_pkt);
    BSLOG("-> KX1");
}

static void build_and_send_kx3(const uint8_t packet3[BS_KX_PACKET3BYTES])
{
    bs_put_hdr(s.kx3_pkt, BS_PKT_KX3);
    bs_put_u32(s.kx3_pkt + BS_HDR_BYTES, s.sid);
    memcpy(s.kx3_pkt + BS_HDR_BYTES + BS_SID_BYTES, packet3, BS_KX_PACKET3BYTES);
    udp_send(s.kx3_pkt, sizeof s.kx3_pkt);
    BSLOG("-> KX3 sid=%08x", s.sid);
}

/* Encrypts and sends one C2S packet (DATA or DISCONNECT). Used both for real
 * application payloads and for the empty-payload liveness probes the
 * handshake tail and idle keepalive send — mirrors bsgate.c's
 * send_encrypted(), context BS_CTX_C2S. */
static bool send_app_packet(uint8_t type, const uint8_t *payload, size_t len)
{
    if (len > NET_MAX_PAYLOAD) return false;
    if (s.fd < 0) return false;

    uint8_t out[BS_MAX_PACKET];
    bs_put_hdr(out, type);
    bs_put_u32(out + BS_HDR_BYTES, s.sid);

    uint64_t msg_id = s.tx_msg_id++;
    bs_put_u64(out + BS_HDR_BYTES + BS_SID_BYTES, msg_id);

    uint8_t *ct = out + BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES;
    if (hydro_secretbox_encrypt(ct, payload, len, msg_id, BS_CTX_C2S, s.keys.tx) != 0) {
        return false;
    }

    size_t total = BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES + BS_AEAD_HEADERBYTES + len;
    ssize_t n = sendto(s.fd, out, total, 0,
                       (struct sockaddr *)&s.server_addr, sizeof s.server_addr);
    s.last_send_ms = bsSockNowMs();
    return n == (ssize_t)total;
}

/* ------------------------------------------------------------- rx queue --- */

static void push_rx(const uint8_t *data, size_t len)
{
    if (s.rxq_count >= BS_RX_QUEUE_LEN) {
        /* Drop the oldest, not the newest — a player who stalled briefly
         * should catch up on recent state, not miss it. */
        s.rxq_head = (s.rxq_head + 1) % BS_RX_QUEUE_LEN;
        s.rxq_count--;
    }
    unsigned idx = (s.rxq_head + s.rxq_count) % BS_RX_QUEUE_LEN;
    memcpy(s.rxq[idx].data, data, len);
    s.rxq[idx].len = len;
    s.rxq_count++;
}

/* ------------------------------------------------------------- failure ---- */

static void fail(const char *msg)
{
    set_error(msg);
    close_socket();
    hydro_memzero(&s.keys, sizeof s.keys);
    s.substate = CSTATE_NONE;
    s.state    = NET_TRANSPORT_FAILED;
    BSLOG("FAILED: %s", msg);
}

/* --------------------------------------------------------- packet intake -- */

static void handle_app_packet(uint8_t type, const uint8_t *pkt, size_t len, uint64_t now)
{
    size_t min_len = BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES + BS_AEAD_HEADERBYTES;
    if (len < min_len || len > BS_MAX_PACKET) return;

    uint32_t sid = bs_get_u32(pkt + BS_HDR_BYTES);
    if (sid != s.sid) return; /* stale/foreign packet, ignore */

    uint64_t msg_id = bs_get_u64(pkt + BS_HDR_BYTES + BS_SID_BYTES);
    const uint8_t *ct = pkt + BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES;
    size_t ct_len = len - (BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES);

    uint8_t plain[NET_MAX_PAYLOAD];
    if (hydro_secretbox_decrypt(plain, ct, ct_len, msg_id, BS_CTX_S2C, s.keys.rx) != 0) {
        BSLOG("<- packet failed to authenticate, dropped");
        return;
    }
    size_t plain_len = ct_len - BS_AEAD_HEADERBYTES;

    /* Only now, with the tag verified, may the window be touched — a forged
     * id must never be allowed to lock out the real server. */
    if (!bsnet_replay_check(&s.rx_replay, msg_id)) {
        hydro_memzero(plain, sizeof plain);
        BSLOG("<- replay dropped (msg_id %llu)", (unsigned long long)msg_id);
        return;
    }

    /* Any authenticated packet on our sid is the only proof the wire
     * protocol ever gives that the server admitted this session — there is
     * no explicit accept, by design (see bsgate.c's drop()). */
    if (s.substate == CSTATE_AWAIT_WELCOME) {
        BSLOG("<- first authenticated packet from server: session established");
        s.substate = CSTATE_RUNNING;
        s.state    = NET_TRANSPORT_ESTABLISHED;
    }

    if (s.probe_outstanding) {
        uint64_t rtt = now - s.probe_sent_ms;
        s.ping_ms = (s.ping_ms < 0) ? (int)rtt : (s.ping_ms * 4 + (int)rtt) / 5;
        s.probe_outstanding = false;
    }

    if (type == BS_PKT_DISCONNECT) {
        hydro_memzero(plain, sizeof plain);
        fail("Disconnected by the server");
        return;
    }

    if (type == BS_PKT_ENROL_OK) {
        /* The payload is empty; the packet arriving at all is the message.
         * This console is on the allowlist from here on, so drop out of
         * enrolment mode — a reconnect must behave like any ordinary join. */
        s.enrol_ok    = true;
        s.enrol_armed = false;
        BSLOG("<- ENROL_OK: enrolled");
        hydro_memzero(plain, sizeof plain);
        return;
    }

    if (plain_len > 0) push_rx(plain, plain_len);

    hydro_memzero(plain, sizeof plain);
}

static void process_datagram(const uint8_t *pkt, size_t len, uint64_t now)
{
    if (len < BS_HDR_BYTES) return;
    if (pkt[1] != BS_PROTO_VERSION) return;
    if (pkt[2] != 0 || pkt[3] != 0) return;

    uint8_t type = pkt[0];

    switch (s.substate) {
    case CSTATE_SENT_HELLO:
        if (type != BS_PKT_COOKIE || len != BS_COOKIE_PKT_BYTES) return;
        memcpy(s.cookie, pkt + BS_HDR_BYTES, BS_COOKIE_BYTES);
        BSLOG("<- COOKIE");

        build_and_send_kx1();
        s.substate          = CSTATE_SENT_KX1;
        s.phase_tries        = 1;
        s.phase_deadline_ms  = now + BS_HS_RETRY_MS;
        break;

    case CSTATE_SENT_KX1: {
        if (type != BS_PKT_KX2 || len != BS_KX2_BYTES) return;

        uint32_t sid = bs_get_u32(pkt + BS_HDR_BYTES);
        const uint8_t *packet2 = pkt + BS_HDR_BYTES + BS_SID_BYTES;

        uint8_t packet3[BS_KX_PACKET3BYTES];
        uint8_t peer_pk[BS_KX_PUBLICKEYBYTES];
        if (hydro_kx_xx_3(&s.kx_state, &s.keys, packet3, peer_pk, packet2,
                          s.psk, &s.static_kp) != 0) {
            BSLOG("<- KX2 rejected locally (bad PSK or corrupt packet), ignoring");
            return; /* keep waiting; a later, undamaged KX2 may still arrive */
        }

        /* Noise XX authenticates whoever answered, not specifically "the
         * real gateway" — that check is this comparison, exactly like
         * bsgate_test.c's client_connect() does against g_server_pk. */
        if (!hydro_equal(peer_pk, s.server_pk, sizeof peer_pk)) {
            hydro_memzero(&s.keys, sizeof s.keys);
            hydro_memzero(packet3, sizeof packet3);
            fail("Server identity does not match this device's configured key");
            return;
        }

        s.sid       = sid;
        s.tx_msg_id = 0;
        bsnet_replay_init(&s.rx_replay);
        BSLOG("<- KX2 sid=%08x, server identity verified", sid);

        build_and_send_kx3(packet3);
        hydro_memzero(packet3, sizeof packet3);

        s.substate           = CSTATE_AWAIT_WELCOME;
        s.phase_tries         = 1;
        s.phase_deadline_ms   = now + BS_WELCOME_RETRY_MS;
        s.probe_outstanding   = false;

        /* This is the earliest moment a probation session can exist on the
         * server, and the last one before the ordinary retry path would send
         * the empty-DATA probe that ends it. */
        if (s.enrol_armed && !s.enrol_sent) {
            s.enrol_sent = netTransportSendEnrol(s.enrol_code, s.enrol_len);
            hydro_memzero(s.enrol_code, sizeof s.enrol_code);
            s.enrol_len = 0;
            s.phase_deadline_ms = now + BS_ENROL_WAIT_MS;
            if (!s.enrol_sent) {
                fail("Could not send the invite code");
                return;
            }
        }
        break;
    }

    case CSTATE_AWAIT_WELCOME:
    case CSTATE_RUNNING:
        if (type != BS_PKT_DATA && type != BS_PKT_DISCONNECT &&
            type != BS_PKT_ENROL_OK) return;
        handle_app_packet(type, pkt, len, now);
        break;

    default:
        break;
    }
}

static void drain_socket(uint64_t now)
{
    /* Bounded: a flood of garbage aimed at our ephemeral port must not turn
     * one frame's Update() into unbounded work. */
    enum { BS_MAX_RX_PER_UPDATE = 32 };
    uint8_t buf[BS_MAX_PACKET + 64];

    for (int i = 0; i < BS_MAX_RX_PER_UPDATE; i++) {
        if (s.fd < 0) return;
        ssize_t n = recvfrom(s.fd, buf, sizeof buf, 0, NULL, NULL);
        if (n <= 0) return; /* EAGAIN/EWOULDBLOCK or a genuine error: nothing more to read */
        process_datagram(buf, (size_t)n, now);
        if (s.substate == CSTATE_NONE) return; /* fail() fired mid-drain */
    }
}

/* ------------------------------------------------------------- retries ---- */

static void tick_retries(uint64_t now)
{
    switch (s.substate) {
    case CSTATE_SENT_HELLO:
        if (now < s.phase_deadline_ms) break;
        if (s.phase_tries >= BS_HS_MAX_TRIES) { fail("Could not reach the server"); break; }
        s.phase_tries++;
        send_hello();
        s.phase_deadline_ms = now + BS_HS_RETRY_MS;
        break;

    case CSTATE_SENT_KX1:
        if (now < s.phase_deadline_ms) break;
        if (s.phase_tries >= BS_HS_MAX_TRIES) { fail("Could not reach the server"); break; }
        s.phase_tries++;
        udp_send(s.kx1_pkt, sizeof s.kx1_pkt); /* same bytes — never a fresh ephemeral here */
        BSLOG("-> KX1 (retry)");
        s.phase_deadline_ms = now + BS_HS_RETRY_MS;
        break;

    case CSTATE_AWAIT_WELCOME:
        if (now < s.phase_deadline_ms) break;

        /* An enrolment attempt sends nothing at all while it waits. Not the
         * DATA probe below, which ends the probation session on the server;
         * not another ENROL, because a repeat of the same code is
         * indistinguishable on the wire from a second guess and only three are
         * allowed before the invite dies for everyone; and not a KX3 retry,
         * because a duplicate handshake against a session already on probation
         * is not something the server promises anything about. One shot, then
         * the player retries from the menu with a fresh handshake. */
        if (s.enrol_armed) {
            /* Kept to 37 characters because the multiplayer screen draws this
             * on one 320px line from x=8 at 6px per glyph — 52 characters
             * before it runs off the right edge, with no wrapping and no
             * clipping, so anything longer is simply not there. The spec
             * suggests a fuller sentence about codes expiring after fifteen
             * minutes and working once; that detail belongs to whoever armed
             * the invite and can see its TTL, while the only move available to
             * the player holding the console is to go and ask for another.
             *
             * Says nothing about WHY on purpose. Wrong, expired, already
             * burnt, never armed, and (per the server's own notes) an
             * allowlist that refused the append all look identical from here,
             * so any more specific wording would be a guess. */
            fail("Code didn't work - ask for a new one.");
            break;
        }

        if (s.phase_tries >= BS_WELCOME_MAX_TRIES) {
            fail("Handshake completed, but the server never admitted the session "
                 "- check that this device's key is on the server's allowlist");
            break;
        }
        s.phase_tries++;
        udp_send(s.kx3_pkt, sizeof s.kx3_pkt);
        BSLOG("-> KX3 (retry)");
        /* Also worth a shot even if KX3 already landed and it is only the
         * first reply that is slow: harmless either way, the gateway simply
         * drops it if no session exists yet for this sid. */
        send_app_packet(BS_PKT_DATA, EMPTY_PAYLOAD, 0);
        s.phase_deadline_ms = now + BS_WELCOME_RETRY_MS;
        break;

    case CSTATE_RUNNING:
        if (!s.probe_outstanding && now - s.last_send_ms >= BS_KEEPALIVE_MS) {
            s.probe_outstanding = true;
            s.probe_sent_ms     = now;
            send_app_packet(BS_PKT_DATA, EMPTY_PAYLOAD, 0);
        }
        break;

    default:
        break;
    }
}

/* ============================================================== API ===== */

bool netTransportInit(void)
{
    memset(&s, 0, sizeof s);
    s.fd       = -1;
    s.state    = NET_TRANSPORT_IDLE;
    s.substate = CSTATE_NONE;
    s.ping_ms  = -1;
    set_error("");

    if (!bsSockPlatformInit()) {
        set_error("Could not start the network service");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }

    if (hydro_init() != 0) {
        set_error("Could not start the crypto library");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }

    uint8_t seed[hydro_kx_SEEDBYTES];
    if (load_or_create_client_seed(seed)) {
        hydro_kx_keygen_deterministic(&s.static_kp, seed);
        s.have_client_id = true;
        write_client_pub_file(&s.static_kp);
    }
    hydro_memzero(seed, sizeof seed);

    /* SD first, baked-in second: a card that carries a server.pub/network.psk
     * is deliberately pointing this console somewhere else, and that has to
     * beat whatever the build shipped with. */
    s.have_server_pk = load_hex_key(BS_SERVER_PK_FILE, s.server_pk, sizeof s.server_pk)
                    || load_hex_literal(BS_SERVER_PK_HEX, s.server_pk, sizeof s.server_pk);
    s.have_psk       = load_hex_key(BS_NETWORK_PSK_FILE, s.psk, sizeof s.psk)
                    || load_hex_literal(BS_NETWORK_PSK_HEX, s.psk, sizeof s.psk);

    s.inited = true;
    return true;
}

void netTransportExit(void)
{
    if (!s.inited) return;
    netTransportDisconnect();
    bsSockPlatformExit();
    memset(&s, 0, sizeof s);
    s.fd    = -1;
    s.state = NET_TRANSPORT_IDLE;
}

bool netTransportConnect(const char *host, uint16_t port)
{
    if (!s.inited) {
        set_error("Network is not initialized");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }
    if (s.substate != CSTATE_NONE) {
        set_error("Already connecting or connected");
        return false;
    }
    if (!s.have_client_id) {
        set_error("No client identity available");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }
    if (!s.have_server_pk || !s.have_psk) {
        set_error("This build has no server key / network PSK configured");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }
    if (host == NULL || host[0] == '\0') {
        set_error("No server address configured");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }

    struct sockaddr_in addr;
    if (!resolve_host(host, port, &addr)) {
        set_error("Could not resolve the server address");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }

    if (!open_socket()) {
        set_error("Could not open a network socket");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }
    s.server_addr = addr;

    if (hydro_kx_xx_1(&s.kx_state, s.packet1, s.psk) != 0) {
        close_socket();
        set_error("Internal error starting the handshake");
        s.state = NET_TRANSPORT_FAILED;
        return false;
    }

    set_error("");
    s.ping_ms           = -1;
    s.probe_outstanding = false;
    s.rxq_head = s.rxq_count = 0;

    /* Per-attempt, unlike enrol_armed: the code was armed before this call and
     * has to survive into the handshake. */
    s.enrol_sent = false;
    s.enrol_ok   = false;

    send_hello();
    s.substate           = CSTATE_SENT_HELLO;
    s.phase_tries         = 1;
    s.phase_deadline_ms   = bsSockNowMs() + BS_HS_RETRY_MS;
    s.state               = NET_TRANSPORT_HANDSHAKING;
    return true;
}

void netTransportDisconnect(void)
{
    if (!s.inited) return;

    if (s.fd >= 0 && (s.substate == CSTATE_AWAIT_WELCOME || s.substate == CSTATE_RUNNING)) {
        send_app_packet(BS_PKT_DISCONNECT, EMPTY_PAYLOAD, 0);
    }
    close_socket();
    hydro_memzero(&s.keys, sizeof s.keys);
    s.substate = CSTATE_NONE;
    s.ping_ms  = -1;
    s.state    = NET_TRANSPORT_IDLE;
}

void netTransportUpdate(void)
{
    if (!s.inited) return;
    if (s.substate == CSTATE_NONE) return;

    uint64_t now = bsSockNowMs();
    drain_socket(now);
    if (s.substate != CSTATE_NONE) tick_retries(now);
}

NetTransportState netTransportState(void)
{
    return s.state;
}

const char *netTransportError(void)
{
    return s.error;
}

int netTransportPingMs(void)
{
    if (s.state != NET_TRANSPORT_ESTABLISHED) return -1;
    return s.ping_ms;
}

bool netTransportLocalPublicKey(uint8_t out[32])
{
    if (!s.have_client_id) return false;
    memcpy(out, s.static_kp.pk, BS_KX_PUBLICKEYBYTES);
    return true;
}

bool netTransportSend(const uint8_t *payload, size_t len)
{
    if (!s.inited) return false;
    if (s.substate != CSTATE_RUNNING) return false;
    if (len > NET_MAX_PAYLOAD) return false;
    if (payload == NULL && len > 0) return false;
    return send_app_packet(BS_PKT_DATA, payload != NULL ? payload : EMPTY_PAYLOAD, len);
}

void netTransportArmEnrol(const uint8_t *code, size_t len)
{
    hydro_memzero(s.enrol_code, sizeof s.enrol_code);
    s.enrol_len   = 0;
    s.enrol_sent  = false;
    s.enrol_ok    = false;
    s.enrol_armed = false;

    if (code == NULL || len == 0) return; /* disarms */

    if (len > sizeof s.enrol_code) len = sizeof s.enrol_code;
    memcpy(s.enrol_code, code, len);
    s.enrol_len   = len;
    s.enrol_armed = true;
}

bool netTransportSendEnrol(const uint8_t *code, size_t len)
{
    if (!s.inited) return false;
    if (s.fd < 0) return false;
    if (code == NULL || len == 0) return false;
    if (len > BS_INVITE_CODE_MAX) return false;
    BSLOG("-> ENROL (%u bytes)", (unsigned)len);
    return send_app_packet(BS_PKT_ENROL, code, len);
}

bool netTransportEnrolled(void)
{
    return s.enrol_ok;
}

int netTransportRecv(uint8_t *out, size_t cap)
{
    if (!s.inited || s.rxq_count == 0) return 0;

    struct rx_item *it = &s.rxq[s.rxq_head];
    if (it->len > cap) return -1;

    memcpy(out, it->data, it->len);
    int n = (int)it->len;

    s.rxq_head = (s.rxq_head + 1) % BS_RX_QUEUE_LEN;
    s.rxq_count--;
    return n;
}
