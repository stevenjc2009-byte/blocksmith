/* enrol_hosttest.c — invite-code enrolment, proven on the wire.
 *
 * Builds the shipping source/net C files (SOCU swapped for bsnet_sock.c's
 * host branch) and runs them against two servers, because the two answer
 * different questions:
 *
 *   Part A, a stub gateway written here. It speaks the server side of the
 *   Noise XX handshake with libhydrogen directly, which means every byte the
 *   client sends can be decrypted and looked at, and scenarios the real
 *   gateway will not perform on request — say nothing at all for twelve
 *   seconds — can be staged exactly. This is where the type byte, the
 *   plaintext and the timing are measured.
 *
 *   Part B, a real bsgate forked from deps/blocksmith-server with a real
 *   invite armed through its own --arm-invite CLI. A stub written by the same
 *   hand as the client only ever proves the two agree with each other; this is
 *   the arm that can actually fail.
 *
 * Every check here is written so it can go red. Where that is not obvious from
 * reading it — the timing one, the "no other packet" one — the comment says
 * what breaking the code would do to it.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <hydrogen.h>

#include "net/bsnet.h"
#include "net/bsnet_transport.h"
#include "proto/bs_proto.h"

/* ------------------------------------------------------------ reporting --- */

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    g_checks++;
    if (!ok) g_fails++;
    fputs(ok ? "  ok   " : "  FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void section(const char *name)
{
    printf("\n== %s\n", name);
    fflush(stdout);
}

static void die(const char *what)
{
    fprintf(stderr, "enroltest: %s: %s\n", what, strerror(errno));
    exit(1);
}

/* ----------------------------------------------------------------- time --- */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------ workspace --- */

#define CLIENT_DIR   ENROL_WORKDIR "/client"

static void run_quiet(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd) != 0) { /* best-effort cleanup; a missing directory is fine */ }
}

static void write_text_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) die(path);
    fputs(contents, f);
    fclose(f);
}

/* bsnet.c reads its server address from a path that is hardcoded for the
 * console ("sdmc:/blocksmith/server.txt") rather than compiled in the way
 * BS_NET_DIR is. On Linux "sdmc:" is an ordinary directory name, so the
 * harness makes one in the working directory and the same code path runs
 * unmodified — much better than adding a #ifdef to shipping code purely so a
 * test can reach it. */
static void set_server_address(uint16_t port)
{
    char line[64];
    snprintf(line, sizeof line, "127.0.0.1:%u\n", port);
    run_quiet("mkdir -p '%s/sdmc:/blocksmith'", ENROL_WORKDIR);

    char path[256];
    snprintf(path, sizeof path, "%s/sdmc:/blocksmith/server.txt", ENROL_WORKDIR);
    write_text_file(path, line);
}

/* Wipes the client's identity as well as its config, so each scenario starts
 * as a console the server has never seen. Without this, Part B's second
 * scenario would arrive with a key the first scenario had already enrolled and
 * would sail past the check it exists to make. */
static void reset_client_dir(void)
{
    run_quiet("rm -rf '%s'", CLIENT_DIR);
    run_quiet("mkdir -p '%s'", CLIENT_DIR);
}

static void write_client_keys(const char *server_pk_hex, const char *psk_hex)
{
    char path[256], line[160];

    snprintf(path, sizeof path, "%s/server.pub", CLIENT_DIR);
    snprintf(line, sizeof line, "%s\n", server_pk_hex);
    write_text_file(path, line);

    snprintf(path, sizeof path, "%s/network.psk", CLIENT_DIR);
    snprintf(line, sizeof line, "%s\n", psk_hex);
    write_text_file(path, line);
}

static uint16_t pick_free_port(void)
{
    struct sockaddr_in a;
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0) die("probe socket");

    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (bind(probe, (struct sockaddr *)&a, sizeof a) != 0) die("probe bind");

    socklen_t al = sizeof a;
    if (getsockname(probe, (struct sockaddr *)&a, &al) != 0) die("probe getsockname");
    uint16_t port = ntohs(a.sin_port);
    close(probe);
    return port;
}

/* ========================================================== stub gateway === */

/* Every C->S packet the stub sees once the session keys exist, kept with the
 * time it arrived. The tests assert on the contents of this log rather than on
 * a flag set by a callback, because "which packets did NOT arrive" is half of
 * what needs proving and a flag cannot say that. */
typedef struct {
    uint8_t  type;
    uint8_t  plain[BS_MAX_PAYLOAD];
    size_t   plain_len;
    bool     decrypted;
    uint64_t at_ms;
} AppPkt;

#define STUB_LOG_MAX 64

typedef struct {
    int fd;
    uint16_t port;

    hydro_kx_keypair kp;
    uint8_t          psk[hydro_kx_PSKBYTES];
    char             pk_hex[2 * 32 + 1];
    char             psk_hex[2 * 32 + 1];

    struct sockaddr_in peer;
    bool peer_known;

    hydro_kx_state           kx;
    hydro_kx_session_keypair keys;
    uint32_t                 sid;
    bool                     saw_kx1;
    bool                     established;   /* keys derived; app packets readable */
    uint64_t                 established_ms;
    uint64_t                 tx_msg_id;

    /* Scenario knob: reply to BS_PKT_ENROL with BS_PKT_ENROL_OK. False stages
     * the server that heard a wrong code — silence, per the spec. */
    bool answer_enrol;

    AppPkt   log[STUB_LOG_MAX];
    unsigned log_count;
} Stub;

static void stub_start(Stub *st, bool answer_enrol)
{
    memset(st, 0, sizeof *st);
    st->answer_enrol = answer_enrol;
    st->port = pick_free_port();

    st->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (st->fd < 0) die("stub socket");

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(st->port);
    if (bind(st->fd, (struct sockaddr *)&a, sizeof a) != 0) die("stub bind");

    int flags = fcntl(st->fd, F_GETFL, 0);
    fcntl(st->fd, F_SETFL, flags | O_NONBLOCK);

    hydro_kx_keygen(&st->kp);
    hydro_random_buf(st->psk, sizeof st->psk);
    hydro_bin2hex(st->pk_hex,  sizeof st->pk_hex,  st->kp.pk, 32);
    hydro_bin2hex(st->psk_hex, sizeof st->psk_hex, st->psk,   32);
}

static void stub_stop(Stub *st)
{
    if (st->fd >= 0) close(st->fd);
    st->fd = -1;
}

static void stub_send(Stub *st, const uint8_t *buf, size_t len)
{
    if (!st->peer_known) return;
    sendto(st->fd, buf, len, 0, (struct sockaddr *)&st->peer, sizeof st->peer);
}

/* Encrypts and sends one S->C packet, framed exactly as bsgate.c frames its
 * own: header, session id, message id, then the AEAD box under BS_CTX_S2C. */
static void stub_send_app(Stub *st, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t out[BS_MAX_PACKET];
    bs_put_hdr(out, type);
    bs_put_u32(out + BS_HDR_BYTES, st->sid);

    uint64_t msg_id = st->tx_msg_id++;
    bs_put_u64(out + BS_HDR_BYTES + BS_SID_BYTES, msg_id);

    uint8_t *ct = out + BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES;
    if (hydro_secretbox_encrypt(ct, payload, len, msg_id, BS_CTX_S2C, st->keys.tx) != 0) {
        fprintf(stderr, "enroltest: stub encrypt failed\n");
        return;
    }
    stub_send(st, out, BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES
                       + BS_AEAD_HEADERBYTES + len);
}

static void stub_log(Stub *st, uint8_t type, const uint8_t *plain, size_t len, bool ok)
{
    if (st->log_count >= STUB_LOG_MAX) return;
    AppPkt *p = &st->log[st->log_count++];
    p->type      = type;
    p->plain_len = len;
    p->decrypted = ok;
    p->at_ms     = now_ms();
    if (ok && len > 0) memcpy(p->plain, plain, len);
}

static void stub_handle_app(Stub *st, const uint8_t *pkt, size_t len)
{
    size_t min_len = BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES + BS_AEAD_HEADERBYTES;
    if (len < min_len) return;

    uint64_t       msg_id = bs_get_u64(pkt + BS_HDR_BYTES + BS_SID_BYTES);
    const uint8_t *ct     = pkt + BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES;
    size_t         ct_len = len - (BS_HDR_BYTES + BS_SID_BYTES + BS_MSGID_BYTES);

    uint8_t plain[BS_MAX_PAYLOAD];
    bool ok = hydro_secretbox_decrypt(plain, ct, ct_len, msg_id,
                                      BS_CTX_C2S, st->keys.rx) == 0;
    stub_log(st, pkt[0], plain, ok ? ct_len - BS_AEAD_HEADERBYTES : 0, ok);

    if (ok && pkt[0] == BS_PKT_ENROL && st->answer_enrol) {
        stub_send_app(st, BS_PKT_ENROL_OK, NULL, 0);
    }
}

static void stub_pump(Stub *st)
{
    uint8_t buf[BS_MAX_PACKET + 64];

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(st->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        if ((size_t)n < BS_HDR_BYTES) continue;

        st->peer       = from;
        st->peer_known = true;

        switch (buf[0]) {
        case BS_PKT_HELLO: {
            /* The cookie is opaque to the client — it echoes back whatever 32
             * bytes it was handed — so any value proves the round trip. */
            uint8_t out[BS_COOKIE_PKT_BYTES];
            memset(out, 0, sizeof out);
            bs_put_hdr(out, BS_PKT_COOKIE);
            memset(out + BS_HDR_BYTES, 0xA5, BS_COOKIE_BYTES);
            stub_send(st, out, sizeof out);
            break;
        }

        case BS_PKT_KX1: {
            if (st->saw_kx1) break;   /* a retransmit must not re-derive keys */
            if ((size_t)n != BS_KX1_BYTES) break;

            const uint8_t *packet1 = buf + BS_HDR_BYTES + BS_COOKIE_BYTES;
            uint8_t packet2[BS_KX_PACKET2BYTES];
            if (hydro_kx_xx_2(&st->kx, packet2, packet1, st->psk, &st->kp) != 0) {
                fprintf(stderr, "enroltest: stub hydro_kx_xx_2 failed\n");
                break;
            }
            st->saw_kx1 = true;
            st->sid     = 0x1234abcdu;

            uint8_t out[BS_KX2_BYTES];
            bs_put_hdr(out, BS_PKT_KX2);
            bs_put_u32(out + BS_HDR_BYTES, st->sid);
            memcpy(out + BS_HDR_BYTES + BS_SID_BYTES, packet2, sizeof packet2);
            stub_send(st, out, sizeof out);
            break;
        }

        case BS_PKT_KX3: {
            if (st->established) break;
            if ((size_t)n != BS_KX3_BYTES) break;

            const uint8_t *packet3 = buf + BS_HDR_BYTES + BS_SID_BYTES;
            if (hydro_kx_xx_4(&st->kx, &st->keys, NULL, packet3, st->psk) != 0) {
                fprintf(stderr, "enroltest: stub hydro_kx_xx_4 failed\n");
                break;
            }
            st->established    = true;
            st->established_ms = now_ms();
            break;
        }

        default:
            if (st->established) stub_handle_app(st, buf, (size_t)n);
            break;
        }
    }
}

/* ---------------------------------------------------------- client drive -- */

/* One frame of the console's main loop: pump the client, then let whichever
 * server is running answer. The 2ms is the harness being polite to the CPU,
 * not a timing dependency — every assertion below is on wall-clock elapsed or
 * on packet contents, never on a frame count. */
static void pump(Stub *st, unsigned ms)
{
    uint64_t until = now_ms() + ms;
    do {
        netUpdate();
        if (st != NULL) stub_pump(st);
        sleep_ms(2);
    } while (now_ms() < until);
}

/* Pumps until the status leaves NET_CONNECTING or `timeout_ms` passes.
 * Returns the elapsed time so a test can assert on how long it took. */
static uint64_t pump_until_settled(Stub *st, unsigned timeout_ms)
{
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        netUpdate();
        if (st != NULL) stub_pump(st);
        NetStatus s = netStatus();
        if (s == NET_CONNECTED || s == NET_FAILED) break;
        sleep_ms(2);
    }
    return now_ms() - start;
}

static unsigned count_of_type(const Stub *st, uint8_t type)
{
    unsigned n = 0;
    for (unsigned i = 0; i < st->log_count; i++) {
        if (st->log[i].type == type) n++;
    }
    return n;
}

static void describe_log(const Stub *st)
{
    printf("       stub saw %u C->S packet(s) after the handshake:", st->log_count);
    for (unsigned i = 0; i < st->log_count; i++) {
        printf(" [0x%02x len=%zu%s]", st->log[i].type, st->log[i].plain_len,
               st->log[i].decrypted ? "" : " UNDECRYPTABLE");
    }
    putchar('\n');
}

/* ============================================================== Part A ==== */

static const char INVITE_TYPED[] = "9k4b2-hmq7x";   /* lowercase and hyphenated on purpose */

/* The server's probation window, from CLIENT-ENROLMENT-SPEC.md and from
 * BS_ENROL_WINDOW_MS in the gateway. Written out here rather than read from
 * the client's own constant on purpose: the thing being tested is that the
 * client outlasts the *server's* window, and a check that imports the client's
 * number would follow that number down if somebody shortened it. */
#define SERVER_WINDOW_MS 10000u

/* 1. The code goes out as BS_PKT_ENROL and the plaintext is byte-for-byte what
 *    was typed.
 * 2. Nothing else goes out while the client waits — in particular not the
 *    empty-DATA liveness probe, which ends the probation session on the
 *    server. Restoring that probe turns this red.
 * 3. BS_PKT_ENROL_OK is not dropped by the receive path and reaches the UI
 *    layer as NET_CONNECTED + netJustEnrolled(). Narrowing the type gate in
 *    process_datagram() back to DATA/DISCONNECT turns this red. */
static void test_enrol_send_and_ok(void)
{
    section("A1  code is sent as ENROL, verbatim, alone — and ENROL_OK lands");

    Stub st;
    stub_start(&st, true);
    reset_client_dir();
    write_client_keys(st.pk_hex, st.psk_hex);
    set_server_address(st.port);

    check(netInit(), "netInit()");
    check(netConnectWithInvite(INVITE_TYPED), "netConnectWithInvite(\"%s\")", INVITE_TYPED);

    uint64_t elapsed = pump_until_settled(&st, 8000);
    describe_log(&st);

    check(st.established, "stub completed the Noise XX handshake");
    check(st.log_count == 1, "exactly one C->S packet after the handshake (got %u)",
          st.log_count);
    check(count_of_type(&st, BS_PKT_DATA) == 0,
          "no BS_PKT_DATA sent while enrolling (would end probation server-side)");

    bool typed_ok = false, type_ok = false;
    if (st.log_count >= 1) {
        const AppPkt *p = &st.log[0];
        type_ok  = (p->type == BS_PKT_ENROL);
        typed_ok = p->decrypted
                && p->plain_len == strlen(INVITE_TYPED)
                && memcmp(p->plain, INVITE_TYPED, p->plain_len) == 0;
        printf("       first packet type byte = 0x%02x, plaintext = \"%.*s\"\n",
               p->type, (int)p->plain_len, p->plain);
    }
    check(type_ok, "first packet's type byte is 0x%02x (BS_PKT_ENROL)", BS_PKT_ENROL);
    check(typed_ok, "decrypted plaintext is exactly the %zu bytes typed, unnormalised",
          strlen(INVITE_TYPED));

    check(netStatus() == NET_CONNECTED, "UI layer reports NET_CONNECTED (status \"%s\")",
          netStatusText());
    check(netJustEnrolled(), "UI layer reports netJustEnrolled()");
    check(netTransportEnrolled(), "transport reports netTransportEnrolled()");
    printf("       settled in %llums\n", (unsigned long long)elapsed);

    netExit();
    stub_stop(&st);
}

/* The wait is the whole safety margin: the server's probation window is 10s,
 * and a client that gives up early throws away an invite that was still live.
 * Before this work the AWAIT_WELCOME phase gave up after 8 tries x 400ms =
 * 3.2s, so this check goes red on exactly the regression it is here to catch.
 * The upper bound catches the opposite mistake — a wait so long the player
 * thinks the console has hung. */
static void test_enrol_waits_ten_seconds(void)
{
    section("A2  a silent server is given at least the full 10s window");

    Stub st;
    stub_start(&st, false);          /* hears the code, says nothing */
    reset_client_dir();
    write_client_keys(st.pk_hex, st.psk_hex);
    set_server_address(st.port);

    check(netInit(), "netInit()");
    check(netConnectWithInvite(INVITE_TYPED), "netConnectWithInvite()");

    uint64_t elapsed = pump_until_settled(&st, 20000);
    describe_log(&st);

    uint64_t since_code = 0;
    if (st.log_count >= 1) since_code = now_ms() - st.log[0].at_ms;

    printf("       gave up %llums after connecting, %llums after the code went out\n",
           (unsigned long long)elapsed, (unsigned long long)since_code);

    check(netStatus() == NET_FAILED, "settles on NET_FAILED, not a hang");
    check(since_code >= SERVER_WINDOW_MS,
          "waited >= %ums after sending the code (measured %llums)",
          (unsigned)SERVER_WINDOW_MS, (unsigned long long)since_code);
    check(elapsed < 20000, "gave up rather than waiting forever");

    /* The spec's ban on auto-retry is about the invite's three strikes: a
     * resend of the same code is indistinguishable on the wire from a second
     * guess. One packet, ever. */
    check(st.log_count == 1, "the code was sent once and never repeated (%u packets)",
          st.log_count);
    check(count_of_type(&st, BS_PKT_DATA) == 0, "still no BS_PKT_DATA during the wait");

    check(!netJustEnrolled(), "not reported as enrolled after a failure");
    printf("       error text: \"%s\"\n", netErrorText());
    check(netErrorText()[0] != '\0', "netErrorText() explains the failure to the player");

    netExit();
    stub_stop(&st);
}

/* An ordinary join must be untouched by any of this: same packets, same
 * timing, and emphatically not reported as an enrolment. */
static void test_plain_connect_unchanged(void)
{
    section("A3  an ordinary connect still behaves like an ordinary connect");

    Stub st;
    stub_start(&st, false);
    reset_client_dir();
    write_client_keys(st.pk_hex, st.psk_hex);
    set_server_address(st.port);

    check(netInit(), "netInit()");
    check(netConnect(), "netConnect() with no invite");

    pump(&st, 2500);
    describe_log(&st);

    check(count_of_type(&st, BS_PKT_ENROL) == 0, "no BS_PKT_ENROL on a plain connect");
    check(count_of_type(&st, BS_PKT_DATA) >= 1,
          "the ordinary liveness probe is still sent when not enrolling (%u seen)",
          count_of_type(&st, BS_PKT_DATA));
    check(!netJustEnrolled(), "netJustEnrolled() is false on a plain connect");

    netExit();
    stub_stop(&st);
}

/* ============================================================== Part B ==== */

typedef struct {
    /* Sized down from anything roomier on purpose: game_sock is copied into a
     * sockaddr_un, whose sun_path is 108 bytes, and a silently truncated
     * socket path fails as "the daemon never answered" rather than as a path
     * error. Keeping the source buffer provably smaller makes that a compile
     * error's worth of impossible instead of a runtime mystery. */
    char   state_dir[80];
    char   game_sock[96];
    char   gate_sock[96];
    pid_t  pid;
    int    game_fd;
    uint16_t port;
    char   pk_hex[128];
    char   psk_hex[128];
    char   code[64];
} Gate;

/* Runs bsgate as a one-shot CLI and returns its stdout. The daemon is not
 * running at that point, or is running against the same state dir — both are
 * how bsgate-keys drives it on the real box. */
static bool gate_cli(const Gate *g, const char *args, char *out, size_t cap)
{
    char cmd[768];
    snprintf(cmd, sizeof cmd, "%s --state-dir %s %s 2>&1", BSGATE_PATH, g->state_dir, args);

    FILE *p = popen(cmd, "r");
    if (p == NULL) return false;

    size_t n = 0;
    out[0] = '\0';
    char line[512];
    while (fgets(line, sizeof line, p) != NULL) {
        size_t l = strlen(line);
        if (n + l + 1 < cap) { memcpy(out + n, line, l); n += l; out[n] = '\0'; }
    }
    pclose(p);
    return true;
}

static bool scan_field(const char *text, const char *key, char *out, size_t cap)
{
    const char *p = strstr(text, key);
    if (p == NULL) return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;

    size_t n = 0;
    while (*p != '\0' && *p != '\n' && *p != '\r' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

/* bsgate refuses to hand its socket to nobody: the game process is the other
 * half of the pipe. Nothing here plays the game, it only has to exist and
 * drain, so the daemon's sends do not pile up. */
static void gate_open_game_socket(Gate *g)
{
    g->game_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g->game_fd < 0) die("game socket");

    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", g->game_sock);
    unlink(un.sun_path);
    if (bind(g->game_fd, (struct sockaddr *)&un, sizeof un) != 0) die("bind game.sock");

    int flags = fcntl(g->game_fd, F_GETFL, 0);
    fcntl(g->game_fd, F_SETFL, flags | O_NONBLOCK);
}

static void gate_drain_game_socket(Gate *g)
{
    uint8_t buf[BS_MAX_PACKET];
    while (recv(g->game_fd, buf, sizeof buf, 0) > 0) { }
}

/* Arms an invite BEFORE the daemon starts, so it is in the file the daemon
 * reads at boot and no SIGHUP race is involved. Each scenario gets its own
 * state directory for the same reason: the three strikes belong to the invite,
 * so a wrong-code test must not be able to spend a strike belonging to the
 * good-code test. */
static bool gate_start(Gate *g, const char *tag, const char *label)
{
    char out[4096];

    memset(g, 0, sizeof *g);
    g->game_fd = -1;
    snprintf(g->state_dir, sizeof g->state_dir, "%s/gate-%s", ENROL_WORKDIR, tag);
    snprintf(g->game_sock, sizeof g->game_sock, "%s/game.sock", g->state_dir);
    snprintf(g->gate_sock, sizeof g->gate_sock, "%s/gate.sock", g->state_dir);

    run_quiet("rm -rf '%s' && mkdir -p '%s'", g->state_dir, g->state_dir);

    if (!gate_cli(g, "--print-identity", out, sizeof out)) return false;
    if (!scan_field(out, "server_public_key", g->pk_hex, sizeof g->pk_hex) ||
        !scan_field(out, "network_psk", g->psk_hex, sizeof g->psk_hex)) {
        fprintf(stderr, "enroltest: --print-identity gave:\n%s\n", out);
        return false;
    }

    /* A non-empty allowlist that does not contain this console: proves the
     * enrolment appended a line rather than the daemon admitting everyone. */
    char path[256];
    snprintf(path, sizeof path, "%s/allowlist", g->state_dir);
    write_text_file(path,
        "0000000000000000000000000000000000000000000000000000000000000001 somebody-else\n");

    char args[128];
    snprintf(args, sizeof args, "--arm-invite %s", label);
    if (!gate_cli(g, args, out, sizeof out)) return false;
    if (!scan_field(out, "invite_code", g->code, sizeof g->code)) {
        fprintf(stderr, "enroltest: --arm-invite gave:\n%s\n", out);
        return false;
    }

    gate_open_game_socket(g);
    g->port = pick_free_port();

    char listen_arg[64];
    snprintf(listen_arg, sizeof listen_arg, "127.0.0.1:%u", g->port);

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execl(BSGATE_PATH, "bsgate",
              "--listen", listen_arg,
              "--state-dir", g->state_dir,
              "--game-socket", g->game_sock,
              (char *)NULL);
        _exit(127);
    }
    g->pid = pid;
    sleep_ms(400);                       /* let it bind before the client knocks */
    return true;
}

static void gate_stop(Gate *g)
{
    if (g->pid > 0) {
        kill(g->pid, SIGTERM);
        int st = 0;
        waitpid(g->pid, &st, 0);
        g->pid = 0;
    }
    if (g->game_fd >= 0) { close(g->game_fd); g->game_fd = -1; }
}

static bool allowlist_contains_local_key(const Gate *g)
{
    uint8_t pk[32];
    if (!netTransportLocalPublicKey(pk)) return false;

    char hex[2 * 32 + 1];
    hydro_bin2hex(hex, sizeof hex, pk, sizeof pk);

    char path[256];
    snprintf(path, sizeof path, "%s/allowlist", g->state_dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) return false;

    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, hex) != NULL) found = true;
    }
    fclose(f);
    return found;
}

/* Pumps the client against the real daemon. There is no stub to service, but
 * the game socket has to be drained or the daemon's writes block. */
static uint64_t pump_real(Gate *g, unsigned timeout_ms)
{
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        netUpdate();
        gate_drain_game_socket(g);
        NetStatus s = netStatus();
        if (s == NET_CONNECTED || s == NET_FAILED) break;
        sleep_ms(2);
    }
    return now_ms() - start;
}

static void test_real_gate_good_code(void)
{
    section("B1  a real bsgate, a real armed invite, the real code");

    Gate g;
    if (!gate_start(&g, "good", "enroltest-friend")) {
        check(false, "could not start a real bsgate with an armed invite");
        return;
    }
    printf("       bsgate on 127.0.0.1:%u, invite code \"%s\"\n", g.port, g.code);

    reset_client_dir();
    write_client_keys(g.pk_hex, g.psk_hex);
    set_server_address(g.port);

    check(netInit(), "netInit()");
    check(!allowlist_contains_local_key(&g),
          "this console's key is NOT on the allowlist before enrolling");

    check(netConnectWithInvite(g.code), "netConnectWithInvite(\"%s\")", g.code);
    uint64_t elapsed = pump_real(&g, 20000);

    printf("       settled in %llums as %s (\"%s\")\n", (unsigned long long)elapsed,
           netStatus() == NET_CONNECTED ? "CONNECTED" : "FAILED", netStatusText());
    if (netStatus() == NET_FAILED) printf("       error: \"%s\"\n", netErrorText());

    check(netStatus() == NET_CONNECTED, "the real gateway admitted the session");
    check(netJustEnrolled(), "netJustEnrolled() — a real BS_PKT_ENROL_OK arrived");
    check(allowlist_contains_local_key(&g),
          "the daemon wrote this console's key into its allowlist file");

    netExit();
    gate_stop(&g);
}

/* The control arm, and the one that would catch a client that reports success
 * on silence. A wrong code produces no reply at all — the same silence as an
 * expired one, a burnt one, or none armed — so "did not connect" here is the
 * only correct outcome, and it must arrive as a failure rather than a hang. */
static void test_real_gate_wrong_code(void)
{
    section("B2  the same daemon, a wrong code — must fail, must not enrol");

    Gate g;
    if (!gate_start(&g, "wrong", "enroltest-nobody")) {
        check(false, "could not start a real bsgate with an armed invite");
        return;
    }

    /* Same alphabet and shape as a real code, so the server rejects it for
     * being wrong rather than for being malformed. */
    const char *wrong = "23456-789AB";
    printf("       bsgate on 127.0.0.1:%u, armed \"%s\", sending \"%s\"\n",
           g.port, g.code, wrong);
    check(strcmp(wrong, g.code) != 0, "the wrong code really is not the armed one");

    reset_client_dir();
    write_client_keys(g.pk_hex, g.psk_hex);
    set_server_address(g.port);

    check(netInit(), "netInit()");
    check(netConnectWithInvite(wrong), "netConnectWithInvite(<wrong>)");

    uint64_t elapsed = pump_real(&g, 20000);
    printf("       settled in %llums, error \"%s\"\n",
           (unsigned long long)elapsed, netErrorText());

    check(netStatus() == NET_FAILED, "reports failure rather than claiming a join");
    check(!netJustEnrolled(), "netJustEnrolled() stays false");
    check(!allowlist_contains_local_key(&g), "nothing was written to the allowlist");
    check(elapsed >= SERVER_WINDOW_MS,
          "still waited out the full window before giving up (%llums)",
          (unsigned long long)elapsed);

    netExit();
    gate_stop(&g);
}

/* ================================================================ main ==== */

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    run_quiet("rm -rf '%s'", ENROL_WORKDIR);
    run_quiet("mkdir -p '%s'", ENROL_WORKDIR);
    if (chdir(ENROL_WORKDIR) != 0) die("chdir workdir");

    if (hydro_init() != 0) { fprintf(stderr, "enroltest: hydro_init failed\n"); return 1; }

    printf("enrol_hosttest — invite-code enrolment, client side\n");
    printf("  client config dir : %s\n", CLIENT_DIR);
    printf("  bsgate binary     : %s\n", BSGATE_PATH);

    test_enrol_send_and_ok();
    test_enrol_waits_ten_seconds();
    test_plain_connect_unchanged();

    if (access(BSGATE_PATH, X_OK) == 0) {
        test_real_gate_good_code();
        test_real_gate_wrong_code();
    } else {
        section("B  real-gateway arm SKIPPED");
        check(false, "bsgate binary not built at %s — the arm that can fail did not run",
              BSGATE_PATH);
    }

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
