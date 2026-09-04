/* bsnet_transport_hosttest — end-to-end proof that bsnet_transport.c's client
 * state machine really speaks the protocol bsgate.c expects, against a real,
 * forked bsgate process. Modelled on server/gateway/bsgate_test.c's approach
 * (launch the real daemon, do not mock it), but driving the actual client
 * transport module rather than a second hand-rolled protocol implementation —
 * this is the SAME bsnet_transport.c that ships to the 3DS, compiled here
 * with BS_NET_DIR pointed at a real filesystem path instead of "sdmc:/", and
 * bsnet_sock.c's host branch standing in for SOCU.
 *
 * We also stand in for the game logic on the gateway's Unix socket, exactly
 * like bsgate_test.c's `g_game`: a real game server would send a welcome
 * payload on JOIN and echo DATA back, so that is what this harness does too.
 * That is what lets the client's transport ever leave CSTATE_AWAIT_WELCOME —
 * the wire protocol has no explicit "you're in" packet, only silence for a
 * rejected peer, so the only way to prove the ESTABLISHED transition (and,
 * separately, the allowlist-rejection timeout) is against something that
 * behaves like the real downstream game would.
 *
 * Two phases:
 *   A. an allowlisted client's identity handshakes, gets a welcome payload,
 *      round-trips one application packet.
 *   B. the client's on-disk identity is deleted and regenerated (proving
 *      persistence AND rotation), left off the allowlist, and must time out
 *      to NET_TRANSPORT_FAILED with an allowlist-shaped error rather than
 *      hanging or silently succeeding.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <hydrogen.h>

#include "../bsnet_transport.h"
#include "../../../server/proto/bs_proto.h"

enum { BS_GAME_JOIN = 1, BS_GAME_DATA = 2, BS_GAME_LEAVE = 3, BS_GAME_KICK = 4 };

/* Must match BS_NET_DIR passed on the compiler command line (see Makefile). */
#ifndef BS_NET_DIR
#error "BS_NET_DIR must be defined to match bsnet_transport.c's build"
#endif

static int  g_checks = 0;
static int  g_fails  = 0;
static pid_t g_daemon = -1;
static char g_state_dir[128];
static int  g_game_fd = -1;
static char g_gate_sock_path[160];

static void check(bool cond, const char *what)
{
    g_checks++;
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_fails++;
}

static void die(const char *what)
{
    fprintf(stderr, "hosttest: %s: %s\n", what, strerror(errno));
    if (g_daemon > 0) kill(g_daemon, SIGKILL);
    exit(1);
}

static void msleep(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void reap_daemon(void)
{
    if (g_daemon > 0) {
        kill(g_daemon, SIGKILL);
        waitpid(g_daemon, NULL, 0);
        g_daemon = -1;
    }
}

/* ------------------------------------------------------- client identity -- */

/* Reads BS_NET_DIR/client.seed and derives the same public key
 * bsnet_transport.c would have derived from it, purely by re-running the
 * documented, public algorithm (hydro_kx_keygen_deterministic) on the same
 * bytes — no private state is reached into. This is how the harness gets the
 * key to put on the server's allowlist. */
static bool read_client_pubkey(uint8_t pk_out[BS_KX_PUBLICKEYBYTES])
{
    FILE *f = fopen(BS_NET_DIR "/client.seed", "rb");
    if (f == NULL) return false;
    uint8_t seed[hydro_kx_SEEDBYTES];
    size_t n = fread(seed, 1, sizeof seed, f);
    fclose(f);
    if (n != sizeof seed) return false;

    hydro_kx_keypair kp;
    hydro_kx_keygen_deterministic(&kp, seed);
    memcpy(pk_out, kp.pk, BS_KX_PUBLICKEYBYTES);
    hydro_memzero(&kp, sizeof kp);
    hydro_memzero(seed, sizeof seed);
    return true;
}

static void write_text_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) die("write config file");
    fputs(contents, f);
    fclose(f);
}

/* ------------------------------------------------------------ gate setup -- */

static uint16_t g_port;

static uint16_t pick_free_port(void)
{
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(probe, (struct sockaddr *)&a, sizeof a) != 0) die("probe bind");
    socklen_t al = sizeof a;
    getsockname(probe, (struct sockaddr *)&a, &al);
    uint16_t port = ntohs(a.sin_port);
    close(probe);
    return port;
}

static void start_daemon(void)
{
    g_port = pick_free_port();

    char listen_arg[64], game_arg[192];
    snprintf(listen_arg, sizeof listen_arg, "127.0.0.1:%u", g_port);
    snprintf(game_arg, sizeof game_arg, "%s/game.sock", g_state_dir);
    snprintf(g_gate_sock_path, sizeof g_gate_sock_path, "%s/gate.sock", g_state_dir);

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execl("./bsgate", "bsgate",
              "--listen", listen_arg,
              "--state-dir", g_state_dir,
              "--game-socket", game_arg,
              (char *)NULL);
        _exit(127);
    }
    g_daemon = pid;
}

static void read_identity(char *pk_hex, size_t pk_cap, char *psk_hex, size_t psk_cap)
{
    char cmd[320];
    snprintf(cmd, sizeof cmd, "./bsgate --state-dir %s --print-identity", g_state_dir);
    FILE *p = popen(cmd, "r");
    if (p == NULL) die("popen print-identity");

    char line[256];
    bool got_pk = false, got_psk = false;
    while (fgets(line, sizeof line, p)) {
        char hex[256];
        if (sscanf(line, "server_public_key %255s", hex) == 1) {
            snprintf(pk_hex, pk_cap, "%s", hex);
            got_pk = true;
        } else if (sscanf(line, "network_psk %255s", hex) == 1) {
            snprintf(psk_hex, psk_cap, "%s", hex);
            got_psk = true;
        }
    }
    pclose(p);
    if (!got_pk || !got_psk) { fprintf(stderr, "hosttest: --print-identity failed\n"); exit(1); }
}

static void write_allowlist_one(const uint8_t pk[BS_KX_PUBLICKEYBYTES], const char *label)
{
    char path[192];
    snprintf(path, sizeof path, "%s/allowlist", g_state_dir);
    char hex[2 * BS_KX_PUBLICKEYBYTES + 1];
    hydro_bin2hex(hex, sizeof hex, pk, BS_KX_PUBLICKEYBYTES);

    FILE *f = fopen(path, "w");
    if (f == NULL) die("write allowlist");
    fprintf(f, "%s %s\n", hex, label);
    fclose(f);
}

/* ------------------------------------------------- game-logic stand-in --- */

static void game_send(uint8_t msg_type, uint32_t sid, const uint8_t *payload, size_t len)
{
    uint8_t buf[5 + BS_MAX_PAYLOAD];
    buf[0] = msg_type;
    bs_put_u32(buf + 1, sid);
    if (len > 0) memcpy(buf + 5, payload, len);

    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", g_gate_sock_path);
    if (sendto(g_game_fd, buf, 5 + len, 0, (struct sockaddr *)&un, sizeof un) < 0) {
        fprintf(stderr, "hosttest: game_send: %s\n", strerror(errno));
    }
}

static void open_game_socket(void)
{
    g_game_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_game_fd < 0) die("game socket");

    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s/game.sock", g_state_dir);
    unlink(un.sun_path);
    if (bind(g_game_fd, (struct sockaddr *)&un, sizeof un) != 0) die("bind game.sock");

    int flags = fcntl(g_game_fd, F_GETFL, 0);
    fcntl(g_game_fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t g_established_sid = 0;
static unsigned  g_tick_accum_ms  = 0;

/* Services one pending message, if any. Real game logic would obviously be
 * more than an echo, but the point here is only to prove the transport's
 * plumbing: a JOIN must produce a welcome the client can recognise as
 * "admitted", and a DATA payload must round-trip. */
static void service_game_socket(void)
{
    uint8_t buf[5 + BS_MAX_PAYLOAD];
    ssize_t n = recv(g_game_fd, buf, sizeof buf, 0);
    if (n >= 5) {
        uint32_t sid = bs_get_u32(buf + 1);
        if (buf[0] == BS_GAME_JOIN) {
            g_established_sid = sid;
            const char *welcome = "welcome";
            printf("        [stand-in game] JOIN sid=%08x -> sending welcome\n", sid);
            game_send(BS_GAME_DATA, sid, (const uint8_t *)welcome, strlen(welcome));
        } else if (buf[0] == BS_GAME_LEAVE) {
            if (sid == g_established_sid) g_established_sid = 0;
        } else if (buf[0] == BS_GAME_DATA) {
            size_t plen = (size_t)n - 5;
            uint8_t reply[5 + BS_MAX_PAYLOAD];
            size_t rlen = snprintf((char *)reply, sizeof reply, "echo:");
            if (plen > 0) { memcpy(reply + rlen, buf + 5, plen); rlen += plen; }
            printf("        [stand-in game] DATA sid=%08x (%zu B) -> echoing\n", sid, plen);
            game_send(BS_GAME_DATA, sid, reply, rlen);
        }
    }

    /* Stand-in for ordinary downstream gameplay traffic (world/entity sync,
     * etc.), which real game logic sends continuously regardless of whether
     * the client pinged it. This matters for netTransportPingMs(): it only
     * credits an RTT sample off *some* authenticated packet arriving while a
     * probe is outstanding, and bsgate.c's handle_data() never acks a bare
     * empty keepalive DATA packet on its own (a zero-length payload is never
     * handed to game_send, so nothing comes back) -- the sample has always
     * been meant to ride on real traffic, not on the keepalive round-tripping
     * by itself. pump_until()/the wait loop below call this every ~10ms, so a
     * fixed 10ms step is accumulated here rather than reading a clock. */
    if (g_established_sid != 0) {
        g_tick_accum_ms += 10;
        if (g_tick_accum_ms >= 250) {
            g_tick_accum_ms = 0;
            const char *tick = "tick";
            game_send(BS_GAME_DATA, g_established_sid, (const uint8_t *)tick, strlen(tick));
        }
    }
}

/* -------------------------------------------------------- transport pump - */

/* Drives netTransportUpdate() and the game stand-in together until `pred`
 * is true or `timeout_ms` elapses. Returns true if `pred` became true. */
static bool pump_until(bool (*pred)(void), unsigned timeout_ms)
{
    for (unsigned waited = 0; waited < timeout_ms; waited += 10) {
        netTransportUpdate();
        service_game_socket();
        if (pred()) return true;
        msleep(10);
    }
    return false;
}

static bool pred_established_or_failed(void)
{
    NetTransportState st = netTransportState();
    return st == NET_TRANSPORT_ESTABLISHED || st == NET_TRANSPORT_FAILED;
}

static bool g_have_reply = false;
static uint8_t g_reply[NET_MAX_PAYLOAD];
static int g_reply_len = 0;

static bool pred_got_reply(void)
{
    uint8_t buf[NET_MAX_PAYLOAD];
    int n = netTransportRecv(buf, sizeof buf);
    if (n > 0) {
        memcpy(g_reply, buf, (size_t)n);
        g_reply_len = n;
        g_have_reply = true;
    }
    return g_have_reply;
}

/* ------------------------------------------------------------------- main - */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    atexit(reap_daemon);

    if (hydro_init() != 0) { fprintf(stderr, "hydro_init failed\n"); return 1; }

    snprintf(g_state_dir, sizeof g_state_dir, "/tmp/bsnet_hosttest_gate_%d", (int)getpid());
    if (mkdir(g_state_dir, 0700) != 0) die("mkdir state dir");

    /* bs_allowlist_load() (server/gateway/allowlist.c) refuses to start the
     * gateway at all if this file does not exist -- an absent allowlist is
     * "cannot open %s", not "zero peers", and bsgate's main() returns 1
     * before ever binding its UDP socket. An empty-but-present file loads
     * fine as zero entries, so it has to exist before start_daemon() forks
     * the real gateway; write_allowlist_one() + SIGHUP below then populates
     * it for phase A. */
    {
        char allow_path[192];
        snprintf(allow_path, sizeof allow_path, "%s/allowlist", g_state_dir);
        FILE *f = fopen(allow_path, "w");
        if (f == NULL) die("create empty allowlist");
        fclose(f);
    }

    /* BS_NET_DIR is a fixed compile-time path (see Makefile) because
     * bsnet_transport.c resolves it at compile time, not at runtime, exactly
     * like the real 3DS build resolves "sdmc:/blocksmith". Clean it so two
     * consecutive runs of this harness cannot see each other's identity. */
    system("rm -rf " BS_NET_DIR " && mkdir -p " BS_NET_DIR);

    puts("== bsnet_transport hosttest ==");

    char server_pk_hex[256], psk_hex[256];
    read_identity(server_pk_hex, sizeof server_pk_hex, psk_hex, sizeof psk_hex);
    write_text_file(BS_NET_DIR "/server.pub", server_pk_hex);
    write_text_file(BS_NET_DIR "/network.psk", psk_hex);
    printf("server identity: %.16s...\n", server_pk_hex);

    start_daemon();
    open_game_socket();
    msleep(300); /* let the daemon finish binding before the first HELLO */

    /* ---------------------------------------------------- phase A: init -- */
    puts("\nphase A: allowlisted client");

    check(netTransportInit(), "netTransportInit succeeds");
    check(netTransportState() == NET_TRANSPORT_IDLE, "state is IDLE after init");

    uint8_t client_pk_a[BS_KX_PUBLICKEYBYTES];
    check(read_client_pubkey(client_pk_a), "client.seed was generated and can be read back");

    uint8_t client_pk_a_again[BS_KX_PUBLICKEYBYTES];
    read_client_pubkey(client_pk_a_again);
    check(hydro_equal(client_pk_a, client_pk_a_again, sizeof client_pk_a),
          "re-reading client.seed derives the same identity (persistence, not just generation)");

    write_allowlist_one(client_pk_a, "hosttest-alice");
    kill(g_daemon, SIGHUP); /* pick up the allowlist we just wrote */
    msleep(200);

    check(netTransportConnect("127.0.0.1", g_port), "netTransportConnect accepts a valid target");
    check(netTransportState() == NET_TRANSPORT_HANDSHAKING, "state moves to HANDSHAKING");

    bool reached_established = pump_until(pred_established_or_failed, 8000);
    check(reached_established && netTransportState() == NET_TRANSPORT_ESTABLISHED,
          "cookie exchange + Noise XX handshake completes -> ESTABLISHED");
    if (netTransportState() != NET_TRANSPORT_ESTABLISHED) {
        printf("        error: %s\n", netTransportError());
    }

    if (netTransportState() == NET_TRANSPORT_ESTABLISHED) {
        /* The first authenticated packet that proved ESTABLISHED *is* the
         * "welcome" DATA the stand-in game sent on JOIN (see
         * handle_app_packet() in bsnet_transport.c: any authenticated packet
         * on our sid is what flips the substate, and a non-empty one is also
         * queued for the caller). Drain it here so the round-trip check below
         * waits for the actual echo of what we are about to send, rather than
         * grabbing this leftover message and returning immediately. */
        uint8_t drain[NET_MAX_PAYLOAD];
        int dn;
        while ((dn = netTransportRecv(drain, sizeof drain)) > 0) {
            drain[dn < (int)sizeof drain ? dn : (int)sizeof drain - 1] = '\0';
            printf("        (drained pre-queued message: \"%s\")\n", drain);
        }

        const char *msg = "hello from 3ds-hosttest";
        check(netTransportSend((const uint8_t *)msg, strlen(msg)), "netTransportSend accepts an app payload");

        g_have_reply = false;
        bool got = pump_until(pred_got_reply, 3000);
        check(got, "an encrypted application payload round-trips through the gateway");
        if (got) {
            char expect[64];
            snprintf(expect, sizeof expect, "echo:%s", msg);
            g_reply[g_reply_len < (int)sizeof g_reply ? g_reply_len : (int)sizeof g_reply - 1] = '\0';
            check(strcmp((char *)g_reply, expect) == 0,
                  "round-tripped payload content is correct and undamaged");
            printf("        received: \"%s\"\n", g_reply);
        }

        /* netTransportPingMs() is documented (bsnet_transport.h) to read -1
         * until "known" -- the transport only samples RTT off the idle
         * keepalive probe (BS_KEEPALIVE_MS = 4000ms in bsnet_transport.c), not
         * off ordinary application traffic. Checking it right after the round
         * trip above would just prove it correctly reads -1 early; waiting
         * past one keepalive interval is what actually exercises the RTT
         * sampling path instead of merely asserting the documented default. */
        check(netTransportPingMs() == -1,
              "ping reads -1 before any keepalive round trip has happened yet");

        puts("        waiting past one keepalive interval for the first RTT sample...");
        for (unsigned waited = 0; waited < 4600; waited += 10) {
            netTransportUpdate();
            service_game_socket();
            msleep(10);
        }

        int ping = netTransportPingMs();
        check(ping >= 0, "netTransportPingMs reports a non-negative RTT after a keepalive round trip");
        printf("        ping: %d ms\n", ping);
    }

    netTransportDisconnect();
    check(netTransportState() == NET_TRANSPORT_IDLE, "netTransportDisconnect returns to IDLE");
    netTransportExit();

    /* ------------------------------------------- phase B: not allowlisted - */
    puts("\nphase B: client key not on the allowlist");

    /* Force a fresh identity: delete the persisted seed and reinitialise.
     * This is the same "if the seed file does not exist, generate one and
     * save it" path phase A already exercised, run a second time to prove it
     * is not a one-shot fluke, and to produce a key that was never added to
     * the allowlist. */
    unlink(BS_NET_DIR "/client.seed");

    check(netTransportInit(), "netTransportInit succeeds again after deleting client.seed");

    uint8_t client_pk_b[BS_KX_PUBLICKEYBYTES];
    check(read_client_pubkey(client_pk_b), "a new client.seed was generated");
    check(!hydro_equal(client_pk_a, client_pk_b, sizeof client_pk_a),
          "the regenerated identity is different from phase A's (not silently reused)");

    check(netTransportConnect("127.0.0.1", g_port), "netTransportConnect accepts a valid target");

    bool phase_b_done = pump_until(pred_established_or_failed, 12000);
    check(phase_b_done && netTransportState() == NET_TRANSPORT_FAILED,
          "an unlisted key is refused (times out to FAILED, never ESTABLISHED)");

    const char *err = netTransportError();
    printf("        error text: \"%s\"\n", err);
    check(strstr(err, "allowlist") != NULL,
          "the failure reason clearly names the allowlist, not a generic timeout");

    netTransportDisconnect();
    netTransportExit();

    /* ------------------------------------------------------------- done -- */
    reap_daemon();
    if (g_fails == 0) {
        char rm[256];
        snprintf(rm, sizeof rm, "rm -rf %s", g_state_dir);
        if (system(rm) != 0) { /* best effort */ }
    } else {
        printf("\nstate dir kept at %s\n", g_state_dir);
    }

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
