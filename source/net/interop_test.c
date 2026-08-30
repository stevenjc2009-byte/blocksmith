/* interop_test — proves the CLIENT's per-chunk diff subscription code (net/networld.c) and the
 * SERVER's (deps/blocksmith-server/game/bsgame.c) agree on the bytes of BS_APP_CHUNK_SUB,
 * BS_APP_CHUNK_DIFFS and BS_APP_CHUNK_UNSUB (proto/bs_proto.h).
 *
 * Why this file exists and networld_test.c / bsgame_test.c are not enough: both of those already
 * pass, and neither one can tell you anything about the OTHER side. networld_test.c hand-builds
 * CHUNK_DIFFS bytes and feeds them to the client; bsgame_test.c drives the real daemon and reads
 * its raw reply bytes back with its own hand-rolled parser. If the two implementations made the
 * SAME mistake about a field's offset — which is exactly what happened once already; see the
 * comment above BS_APP_CHUNK_SUB in bs_proto.h — both of those suites would stay green while a
 * real client talking to a real server silently lost every diff.
 *
 * So this test does not trust either implementation's idea of the wire format, and it does not
 * take either implementation's word for what the other one sent. It:
 *
 *   1. Spawns the REAL bsgame binary (built by deps/blocksmith-server/game/Makefile, invoked by
 *      tools/run_host_tests.sh before this binary compiles) as a child process and speaks the
 *      real gate<->game Unix-datagram protocol at it, standing in for bsgate — the same technique
 *      bsgame_test.c already uses, and the reason this is possible at all without linking bsgame.c
 *      (its CHUNK_DIFFS/CHUNK_SUB/CHUNK_UNSUB handlers are static; there is no other way to invoke
 *      them from outside that process at all).
 *   2. Links the REAL net/networld.c (client decoder + SUB/UNSUB encoders) and REAL
 *      world/world.c, exactly as networld_test.c does, and drives it with a "fake transport" that
 *      performs NO protocol logic of its own — netTransportSend() below only wraps a payload in
 *      the local gate<->game envelope and hands it to the real socket; netTransportRecv() only
 *      strips that envelope back off. Neither function inspects or reshapes the application
 *      payload, so they cannot hide an interop bug: whatever networld.c actually builds is
 *      word-for-word what reaches bsgame, and whatever bsgame actually sends is word-for-word what
 *      networld.c actually parses.
 *   3. Independently, for every packet on the wire, decodes/encodes a "canonical" copy using
 *      nothing but bs_proto.h's own macros and its bs_put_ / bs_get_ helpers (buildCanonical
 *      below) — never copied from bsgame.c's send_chunk_diffs() or networld.c's applyChunkDiffs —
 *      and
 *      memcmp()s the real bytes against it. This is the check that actually answers "byte-
 *      identical, per the spec, or not" rather than "do the two ends merely agree with each
 *      other" (two ends can agree with each other and both disagree with bs_proto.h).
 *
 * What is NOT proven here: bsgame_test.c and networld_test.c already cover malformed/oversized/
 * truncated packets and rate limiting exhaustively from each side; this file only adds the seam
 * between them and does not re-litigate those cases.
 *
 * Same __3DS__ guard as networld_test.c and blockdiff_test.c, and for the identical reason: the
 * mc Makefile globs every .c under source/net into the console build, and this file carries its
 * own main().
 */
#ifndef __3DS__

/* Same as bsgame.c and bsgame_test.c (deps/blocksmith-server/game/): clock_gettime(),
 * nanosleep() and the rest of the POSIX process/socket calls this file spawns the real daemon
 * with are not visible under plain -std=c11 without a feature-test macro. */
#define _GNU_SOURCE

#include "net/networld.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/block.h"
#include "world/world.h"

#include "proto/bs_proto.h"

/* ---------------------------------------------------------------- gate<->game IPC framing ---
 * Not part of bs_proto.h — it is the LOCAL convention between bsgate and bsgame on one box, not
 * anything a 3DS ever sees on the wire (see bs_proto.h's own header comment on that boundary).
 *
 * enum bs_game_msg was hand-copied into this file until now: one of SIX copies of the same four
 * values, in two repos, with nothing whatsoever holding them equal. A value drifting between them
 * would not have been a compile error anywhere — it would have been the gate sending one kind byte
 * for what the game read as another, at runtime, with no diagnostic. deps/blocksmith-server's
 * 65d40f8 collapsed the four server-side copies into proto/bs_gamelink.h and named this file as
 * one of the two it could not reach from inside that repo. This is that edit; six copies become
 * two.
 *
 * Reached by exactly the path bs_proto.h above is reached by — `-I deps/blocksmith-server`, in
 * tools/run_host_tests.sh's interop_test stanza — so no build change is needed for it. Note that
 * nothing in bs_gamelink.h is covered by this repo's PROTO_COMMIT pin: check-proto-drift compares
 * proto/bs_proto.h and nothing else, by design, because bs_gamelink.h never travels on the wire
 * and no 3DS build ever includes it (this whole file is inside #ifndef __3DS__). See that
 * header's own comment for why it is a separate file from bs_proto.h.
 *
 * BS_GAME_ENVELOPE_BYTES stays local. bsgame.c owns that constant and bs_gamelink.h deliberately
 * does not define it — it only points at it. */
#include "proto/bs_gamelink.h"

#define BS_GAME_ENVELOPE_BYTES 5u

/* ---------------------------------------------------------------------------- check() plumbing */

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* How many check() calls this suite makes against a healthy tree and a healthy daemon. A LITERAL
 * on purpose, and hand-recomputed rather than pasted from a run.
 *
 * Every suite in this project used to end at "0 failed" and nothing else, which cannot tell a
 * check that PASSED from a check that never RAN. That distinction is unusually sharp here: almost
 * every test below is a conversation with a child process over a socket, so an early `return`
 * after a timeout, a helper that gives up quietly, a loop over a packet count that comes back
 * shorter than it should, or an rc-based bail in one of the four inventory tests all remove
 * checks rather than fail them. The suite would then print PASS with a smaller number, and the
 * number is the only place that shows.
 *
 * 56 was the count on 2026-08-25, the first time this binary was ever run in this environment
 * (it is skipped on Windows/MSYS2 - see tools/run_host_tests.sh - and this repo's host is
 * Windows, so it had never executed here at all until it was run from WSL).
 *
 * 66 on 2026-08-30: v1.8.3 Phase 4 added scenario 9 (BS_APP_WORLD_GEN across the real process
 * boundary), which makes 10 checks. Recomputed as 56 + 10 by counting the check() calls added,
 * not read off the run - the paragraph below is the reason that distinction matters.
 *
 * Legitimately adding or removing a check means editing this by hand. The suite going red until
 * you do is deliberate friction, not an accident. */
#define INTEROP_TEST_EXPECTED_CHECKS 66

/* Deliberately NOT routed through check(): it must not perturb the number it is testing, so it
 * bumps g_fails only and leaves g_checks alone. Reporting shape is check()'s, so a failure here
 * reads the way every other failure in this file does.
 *
 * `ran` is latched from g_checks on entry, so the pin means "checks completed before this line"
 * no matter how the counter is maintained. That is not pedantry: this project's suites are split
 * between a CHECK macro that increments before it evaluates its condition and a check() function
 * whose argument is evaluated at the call site before the increment, so the same pin expression
 * is off by one in half of them. Latching first makes the reading identical everywhere. */
static void check_count_pin(void)
{
    const int ran = g_checks;
    if (ran == INTEROP_TEST_EXPECTED_CHECKS) return;

    g_fails++;
    if (ran < INTEROP_TEST_EXPECTED_CHECKS)
        printf("  FAIL  CHECK COUNT: %d check(s) WENT MISSING - expected %d, ran %d.\n"
               "        They did not fail. They never ran: a packet loop came back short, a\n"
               "        timeout took an early return, a helper bailed, or a check was deleted.\n"
               "        The checks that did run passing tells you nothing about the ones that\n"
               "        did not. Find them. Do NOT re-pin INTEROP_TEST_EXPECTED_CHECKS to go\n"
               "        green.\n",
               INTEROP_TEST_EXPECTED_CHECKS - ran, INTEROP_TEST_EXPECTED_CHECKS, ran);
    else
        printf("  FAIL  CHECK COUNT: %d check(s) were ADDED - expected %d, ran %d.\n"
               "        If you added them on purpose, set INTEROP_TEST_EXPECTED_CHECKS in\n"
               "        source/net/interop_test.c to %d. If you did not, something is running\n"
               "        checks more times than it should.\n",
               ran - INTEROP_TEST_EXPECTED_CHECKS, INTEROP_TEST_EXPECTED_CHECKS, ran, ran);
}

/* ------------------------------------------------------------------------------- time helpers */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void msleep(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* net/bsnet_sock.h's clock, faked the same way networld_test.c fakes it (bsnet_sock.c is not
 * linked here — it is the real UDP/Noise clock, not host-portable). Real wall time is fine: none
 * of the scenarios below exercise remote-pose ageing, so nothing needs to fast-forward it. */
uint64_t bsSockNowMs(void) { return now_ms(); }

/* ---------------------------------------------------------------------------- daemon plumbing */

static pid_t g_daemon = -1;
static char  g_dir[64];
static char  g_game_sock[108];
static char  g_gate_sock[108];
static int   g_gate = -1;

static void die(const char *what)
{
    fprintf(stderr, "interop_test: %s: %s\n", what, strerror(errno));
    if (g_daemon > 0) kill(g_daemon, SIGKILL);
    exit(1);
}

static void reap_daemon(void)
{
    if (g_daemon <= 0) return;
    kill(g_daemon, SIGTERM);
    waitpid(g_daemon, NULL, 0);
    g_daemon = -1;
}

/* Same crash posture as bsgame_test.c: if this process dies unexpectedly, the daemon it spawned
 * must not be left running with our now-defunct gate.sock as its only way to talk to anyone. */
static void crash_handler(int sig)
{
    reap_daemon();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void set_sock_paths(void)
{
    snprintf(g_game_sock, sizeof g_game_sock, "%s/game.sock", g_dir);
    snprintf(g_gate_sock, sizeof g_gate_sock, "%s/gate.sock", g_dir);
}

static void open_sockets(void)
{
    g_gate = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_gate < 0) die("gate socket");

    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", g_gate_sock);
    unlink(un.sun_path);
    if (bind(g_gate, (struct sockaddr *)&un, sizeof un) != 0) die("bind gate.sock");
}

/* Relative to the repo root, not to this binary's own build-host/run-$$ directory: run_host_
 * tests.sh always runs every compiled test with the repo root as cwd (see its own comment on
 * why — concurrent sessions must not collide on build-host/), and it builds this exact bsgame
 * binary via `make -C deps/blocksmith-server/game bsgame` immediately before compiling this
 * file, so the path below is guaranteed to exist and be current, not whatever was last built by
 * hand. */
#define BSGAME_BIN "deps/blocksmith-server/game/bsgame"

static void start_daemon(void)
{
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execl(BSGAME_BIN, "bsgame",
              "--game-socket", g_game_sock,
              "--gate-socket", g_gate_sock,
              "--state-dir",   g_dir,
              (char *)NULL);
        _exit(127);
    }
    g_daemon = pid;
}

static bool wait_ready(unsigned timeout_ms)
{
    for (unsigned waited = 0; waited < timeout_ms; waited += 50) {
        struct stat st;
        if (stat(g_game_sock, &st) == 0) return true;
        msleep(50);
    }
    return false;
}

/* ------------------------------------------------------------------------- raw IPC send helpers
 * Everything in this section is scaffolding: it JOINs a simulated player and seeds diffs into the
 * server's diffstore so a scenario below has something to subscribe to. None of it is the thing
 * under test — the seeding shape (BS_APP_BLOCK_EDIT) is exercised exhaustively by bsgame_test.c
 * and blockdiff_test.c already. It is still built strictly from bs_proto.h's macros, never copied
 * from bsgame.c's own encoder, for the same reason the canonical builders further down are. */

static void gate_sendto(const uint8_t *buf, size_t len)
{
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", g_game_sock);
    if (sendto(g_gate, buf, len, 0, (struct sockaddr *)&un, sizeof un) < 0) die("gate sendto");
}

static void send_join(uint32_t sid, const char *label)
{
    uint8_t buf[BS_GAME_ENVELOPE_BYTES + 32 + 32];
    memset(buf, 0, sizeof buf);
    buf[0] = BS_GAME_JOIN;
    bs_put_u32(buf + 1, sid);
    /* pubkey left zeroed — bsgame does not look at it, see handle_join()'s own comment. */
    snprintf((char *)buf + BS_GAME_ENVELOPE_BYTES + 32, 32, "%s", label);
    gate_sendto(buf, sizeof buf);
}

static void seed_block_edit(uint32_t sid, int32_t x, int32_t y, int32_t z, uint8_t block)
{
    uint8_t app[BS_BLOCK_EDIT_BYTES];
    app[0] = BS_APP_BLOCK_EDIT;
    bs_put_i32(app + 1, x);
    bs_put_i32(app + 5, y);
    bs_put_i32(app + 9, z);
    app[13] = block;

    uint8_t buf[BS_GAME_ENVELOPE_BYTES + BS_BLOCK_EDIT_BYTES];
    buf[0] = BS_GAME_DATA;
    bs_put_u32(buf + 1, sid);
    memcpy(buf + BS_GAME_ENVELOPE_BYTES, app, sizeof app);
    gate_sendto(buf, sizeof buf);
}

/* ------------------------------------------------------------------------- canonical encoders
 * The yardstick every real byte in this file is measured against. Written directly from
 * bs_proto.h's macros and put/get helpers — BS_APP_HDR_BYTES, the cx/cz/flags/count offsets
 * documented above BS_CHUNK_DIFFS_HDR_BYTES, BS_SYNC_ENTRY_BYTES — and deliberately NOT looked up
 * by reading bsgame.c's send_chunk_diffs() or networld.c's applyChunkDiffs() first. If it agreed
 * with whichever implementation it was copied from, a mistake shared by both of those (the exact
 * failure mode this whole file exists to catch) would sail through unnoticed. */

struct diff_entry { int32_t x, y, z; uint8_t block; };

static size_t buildCanonicalChunkDiffs(uint8_t *out, int32_t cx, int32_t cz, uint8_t flags,
                                        const struct diff_entry *e, uint16_t n)
{
    out[0] = BS_APP_CHUNK_DIFFS;
    bs_put_i32(out + 1, cx);
    bs_put_i32(out + 5, cz);
    out[9] = flags;
    bs_put_u16(out + 10, n);

    uint8_t *p = out + BS_CHUNK_DIFFS_HDR_BYTES;
    for (uint16_t i = 0; i < n; i++) {
        bs_put_i32(p,     e[i].x);
        bs_put_i32(p + 4, e[i].y);
        bs_put_i32(p + 8, e[i].z);
        p[12] = e[i].block;
        p += BS_SYNC_ENTRY_BYTES;
    }
    return BS_CHUNK_DIFFS_BYTES(n);
}

static void buildCanonicalChunkSub(uint8_t *out, int32_t cx, int32_t cz)
{
    out[0] = BS_APP_CHUNK_SUB;
    bs_put_i32(out + 1, cx);
    bs_put_i32(out + 5, cz);
}

static void buildCanonicalChunkUnsub(uint8_t *out, int32_t cx, int32_t cz)
{
    out[0] = BS_APP_CHUNK_UNSUB;
    bs_put_i32(out + 1, cx);
    bs_put_i32(out + 5, cz);
}

/* Same "never copied from the implementation it is checking" posture as the three builders
 * above, written straight from bs_proto.h's own BS_APP_INV_STATE/BS_INV_STATE_BYTES comments
 * (selected_hotbar at offset 1, then BS_INV_SLOT_COUNT (item, count) pairs), not from
 * bsgame.c's send_inv_state() or networld.c's applyInvState(). `item`/`count` are
 * BS_INV_SLOT_COUNT-long arrays, one entry per slot. */
static void buildCanonicalInvState(uint8_t *out, uint8_t selected_hotbar,
                                    const uint8_t *item, const uint8_t *count)
{
    out[0] = BS_APP_INV_STATE;
    out[1] = selected_hotbar;

    uint8_t *p = out + BS_APP_HDR_BYTES + 1u;
    for (uint32_t i = 0; i < BS_INV_SLOT_COUNT; i++) {
        p[0] = item[i];
        p[1] = count[i];
        p += 2;
    }
}

/* v1.8.3 Phase 4. BS_APP_WORLD_GEN, built from proto/bs_proto.h's macros and nothing else — not
 * copied from bsgame.c's send_world_gen() and not from networld.c's applyWorldGen(), for the
 * reason the other canonical builders in this section give: a byte layout copied from one of the
 * two sides it is meant to arbitrate agrees with that side by construction and can only catch
 * the other one. This is the third, independent statement of the layout, and it is the one the
 * header actually specifies. */
static void buildCanonicalWorldGen(uint8_t *out /* BS_WORLD_GEN_BYTES */, uint16_t gen)
{
    out[0] = BS_APP_WORLD_GEN;
    bs_put_u16(out + BS_APP_HDR_BYTES, gen);
}

/* ------------------------------------------------------------------------------ fake transport
 * Link-time test double for net/bsnet_transport.h, same seam networld_test.c uses (bsnet_
 * transport.c is real Noise XX over a real UDP socket, none of it host-portable). UNLIKE
 * networld_test.c's fake, this one performs no protocol logic at all: it only wraps/unwraps the
 * local gate<->game envelope around whatever bytes networld.c already built, and relays them to
 * the REAL bsgame process over the REAL Unix socket above. See this file's header comment for why
 * that split — real logic, dumb pipe — is what makes this an interop test rather than one more
 * self-consistency check. */

static uint32_t g_client_sid;   /* the sid this process's networld.c instance is JOINed as */

/* Every BS_APP_CHUNK_SUB / BS_APP_CHUNK_UNSUB networld.c hands to the transport, captured
 * verbatim — this is what a scenario below memcmp()s against buildCanonicalChunkSub/Unsub(). */
static uint8_t g_last_sent[BS_MAX_PAYLOAD];
static size_t  g_last_sent_len;

/* Every BS_APP_CHUNK_DIFFS packet networld.c receives, captured verbatim before it is handed to
 * networldApplyPayload() — the client's real parse still runs on every one of these via the
 * networldUpdate() pump in the scenarios below; capturing it here additionally is what lets a
 * scenario also check the exact bytes, not just the parse's visible effect. */
#define CAPTURE_MAX 4
static uint8_t g_captured[CAPTURE_MAX][BS_MAX_PAYLOAD];
static size_t  g_captured_len[CAPTURE_MAX];
static int     g_captured_count;

static void captureReset(void)
{
    g_captured_count = 0;
    g_last_sent_len  = 0;
}

/* Same idea as g_captured/g_captured_len/g_captured_count above, one capture point earlier
 * (netTransportRecv, below), but for BS_APP_INV_STATE instead of BS_APP_CHUNK_DIFFS. Kept as a
 * separate array/counter rather than reusing the CHUNK_DIFFS one: a scenario below needs to
 * assert on "how many INV_STATE packets arrived" independent of how many CHUNK_DIFFS packets
 * also happened to be in flight, and a shared counter would conflate the two message types. */
#define INV_CAPTURE_MAX 4
static uint8_t g_captured_inv[INV_CAPTURE_MAX][BS_INV_STATE_BYTES];
static size_t  g_captured_inv_len[INV_CAPTURE_MAX];
static int     g_captured_inv_count;

static void captureInvReset(void)
{
    g_captured_inv_count = 0;
}

/* v1.8.3 Phase 4. The same again for BS_APP_WORLD_GEN, plus a running log of the app ids the
 * join burst delivered, in arrival order.
 *
 * The order log is not decoration. The client's entry gate (net/networld.c's
 * networldGenWaiting(), and scene/title_nav.c's fourth term) is built on WORLD_GEN arriving in
 * the same burst as WORLD_INFO and immediately behind it — that is the whole reason its grace is
 * 250 ms and not two seconds. A check that only asked "did a WORLD_GEN turn up eventually" would
 * stay green against a server that sent it a second later, by which time every client in the
 * field would already have entered the world assuming legacy. The ORDER is the property, so it
 * is recorded here rather than inferred. */
#define GEN_CAPTURE_MAX 4
static uint8_t g_captured_gen[GEN_CAPTURE_MAX][BS_MAX_PAYLOAD];
static size_t  g_captured_gen_len[GEN_CAPTURE_MAX];
static int     g_captured_gen_count;

#define JOIN_ORDER_MAX 8
static uint8_t g_join_order[JOIN_ORDER_MAX];
static int     g_join_order_count;

static void captureGenReset(void)
{
    g_captured_gen_count = 0;
    g_join_order_count   = 0;
}

/* Registered once per scenario via networldSetInvHook() (networldInit() clears any previous
 * registration, so this must be re-armed after every networldInit() call, same as the World
 * pointer would need to be if a scenario used one). Captures the REAL decoded state — the
 * struct networld.c's applyInvState() actually builds and hands to the hook — as a second,
 * independent witness alongside g_captured_inv[]'s raw bytes: one proves the wire bytes are
 * right, the other proves networld.c's own parse of those bytes is right. A scenario that only
 * checked one of the two could not tell "the bytes are correct but the decode is buggy" apart
 * from "the decode is fine but the raw capture is wrong". */
static NetworldInvState g_last_inv_state;
static int              g_inv_hook_calls;

static void invHook(void *userdata, const NetworldInvState *state)
{
    (void)userdata;
    g_last_inv_state = *state;
    g_inv_hook_calls++;
}

bool netTransportSend(const uint8_t *payload, size_t len)
{
    if (len > BS_MAX_PAYLOAD) return false;

    memcpy(g_last_sent, payload, len);
    g_last_sent_len = len;

    uint8_t buf[BS_GAME_ENVELOPE_BYTES + BS_MAX_PAYLOAD];
    buf[0] = BS_GAME_DATA;
    bs_put_u32(buf + 1, g_client_sid);
    memcpy(buf + BS_GAME_ENVELOPE_BYTES, payload, len);
    gate_sendto(buf, BS_GAME_ENVELOPE_BYTES + len);
    return true;
}

int netTransportRecv(uint8_t *out, size_t cap)
{
    struct pollfd pfd = { .fd = g_gate, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0) return 0;   /* non-blocking, same as the real transport's drain */

    uint8_t buf[BS_GAME_ENVELOPE_BYTES + BS_MAX_PAYLOAD];
    ssize_t n = recv(g_gate, buf, sizeof buf, 0);
    if (n < (ssize_t)BS_GAME_ENVELOPE_BYTES) return 0;
    if (buf[0] != BS_GAME_DATA) return 0;                 /* e.g. a KICK; nothing here expects one */
    if (bs_get_u32(buf + 1) != g_client_sid) return 0;     /* another sid's packet on the shared gate socket */

    size_t plen = (size_t)n - BS_GAME_ENVELOPE_BYTES;
    if (plen > cap) return 0;
    memcpy(out, buf + BS_GAME_ENVELOPE_BYTES, plen);

    if (plen > 0 && out[0] == BS_APP_CHUNK_DIFFS && g_captured_count < CAPTURE_MAX) {
        memcpy(g_captured[g_captured_count], out, plen);
        g_captured_len[g_captured_count] = plen;
        g_captured_count++;
    }
    if (plen > 0 && out[0] == BS_APP_INV_STATE && g_captured_inv_count < INV_CAPTURE_MAX) {
        memcpy(g_captured_inv[g_captured_inv_count], out, plen);
        g_captured_inv_len[g_captured_inv_count] = plen;
        g_captured_inv_count++;
    }
    if (plen > 0 && out[0] == BS_APP_WORLD_GEN && g_captured_gen_count < GEN_CAPTURE_MAX) {
        memcpy(g_captured_gen[g_captured_gen_count], out, plen);
        g_captured_gen_len[g_captured_gen_count] = plen;
        g_captured_gen_count++;
    }
    /* Unconditional and type-blind: what the Phase 4 scenario asks of it is the SEQUENCE, so
     * filtering to the types that scenario cares about would let a packet slipped in between
     * them go unrecorded and the sequence still read as adjacent. */
    if (plen > 0 && g_join_order_count < JOIN_ORDER_MAX)
        g_join_order[g_join_order_count++] = out[0];
    return (int)plen;
}

/* Drives the REAL networldUpdate() pump (which calls the two functions above, then the real
 * networldApplyPayload() for whatever it received) until `want` CHUNK_DIFFS packets have been
 * captured or `timeout_ms` has passed. */
static void pumpUntilCaptured(int want, unsigned timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    while (g_captured_count < want && now_ms() < deadline) {
        networldUpdate();
        msleep(5);
    }
    networldUpdate();   /* one last drain in case something landed right at the deadline */
}

static void pumpFor(unsigned ms)
{
    uint64_t deadline = now_ms() + ms;
    do {
        networldUpdate();
        msleep(5);
    } while (now_ms() < deadline);
}

/* Same idea as pumpUntilCaptured() above, watching g_captured_inv_count instead of
 * g_captured_count. A separate function rather than a parameter on the existing one: the two
 * counters are reset independently (captureReset() vs captureInvReset()) by design, so a
 * shared "which counter" flag would just be this function anyway with extra branching. */
static void pumpUntilInvCaptured(int want, unsigned timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    while (g_captured_inv_count < want && now_ms() < deadline) {
        networldUpdate();
        msleep(5);
    }
    networldUpdate();   /* one last drain in case something landed right at the deadline */
}

/* v1.8.3 Phase 4, and the same again for BS_APP_WORLD_GEN. */
static void pumpUntilGenCaptured(int want, unsigned timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    while (g_captured_gen_count < want && now_ms() < deadline) {
        networldUpdate();
        msleep(5);
    }
    networldUpdate();   /* one last drain in case something landed right at the deadline */
}

/* ------------------------------------------------------------------------------- scenario 1 --
 * A normal multi-entry CHUNK_DIFFS batch: several diffs, one column, one packet, LAST set. */

static void test_normal_multi_entry_batch(void)
{
    puts("real interop: CHUNK_SUB -> a normal multi-entry CHUNK_DIFFS batch, byte-identical to "
         "the spec, and every block lands in the client's World");

    const uint32_t seeder = 0x51EEED01u;
    const uint32_t client = 0xC1E00001u;
    const int32_t  cx = 2, cz = -1;   /* column (2,-1): x in 32..47, z in -16..-1 */

    const struct diff_entry e[5] = {
        { 32, 10, -16, BLOCK_DIRT  },
        { 33, 20, -15, BLOCK_STONE },
        { 40, 30, -10, BLOCK_SAND  },
        { 47,  5,  -1, BLOCK_DIRT  },
        { 35, 64,  -8, BLOCK_STONE },
    };

    send_join(seeder, "seeder1");
    for (int i = 0; i < 5; i++) seed_block_edit(seeder, e[i].x, e[i].y, e[i].z, e[i].block);

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    check(worldColumnCreate(&w, cx, cz) != NULL, "test column (2,-1) loaded ahead of the sub");

    send_join(client, "client1");
    g_client_sid = client;
    captureReset();

    networldSubscribeColumn(cx, cz);   /* REAL client encoder */

    uint8_t canon_sub[BS_CHUNK_SUB_BYTES];
    buildCanonicalChunkSub(canon_sub, cx, cz);
    check(g_last_sent_len == BS_CHUNK_SUB_BYTES, "CHUNK_SUB encoded to exactly BS_CHUNK_SUB_BYTES");
    check(memcmp(g_last_sent, canon_sub, BS_CHUNK_SUB_BYTES) == 0,
          "CHUNK_SUB bytes are byte-identical to the canonical (cx,cz) encoding");

    pumpUntilCaptured(1, 1000);
    check(g_captured_count == 1, "exactly one CHUNK_DIFFS packet came back for a 5-entry column");
    if (g_captured_count == 1) {
        uint8_t canon[BS_CHUNK_DIFFS_BYTES(5)];
        size_t canon_len = buildCanonicalChunkDiffs(canon, cx, cz, BS_CHUNK_DIFFS_LAST, e, 5);
        check(g_captured_len[0] == canon_len, "CHUNK_DIFFS packet length matches BS_CHUNK_DIFFS_BYTES(5)");
        check(memcmp(g_captured[0], canon, canon_len) == 0,
              "the REAL server's CHUNK_DIFFS bytes are byte-identical to the canonical encoding "
              "(header AND all 5 entries, in insertion order)");
    }

    for (int i = 0; i < 5; i++) {
        char msg[96];
        snprintf(msg, sizeof msg, "block (%d,%d,%d) landed in the client's World",
                 e[i].x, e[i].y, e[i].z);
        check(worldGet(&w, e[i].x, e[i].y, e[i].z) == e[i].block, msg);
    }

    worldExit(&w);
}

/* ------------------------------------------------------------------------------- scenario 2 --
 * A fresh, untouched column: zero entries, one packet, LAST still set (bs_proto.h: "a column
 * with no edits still gets one packet ... so the client can tell 'none' from 'still coming'"). */

static void test_empty_last_batch(void)
{
    puts("real interop: CHUNK_SUB on an untouched column gets a zero-entry CHUNK_DIFFS, LAST set, "
         "byte-identical to the spec");

    const uint32_t client = 0xC1E00002u;
    const int32_t  cx = 42, cz = -42;   /* never seeded by any scenario */

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, cx, cz);

    send_join(client, "client2");
    g_client_sid = client;
    captureReset();

    networldSubscribeColumn(cx, cz);

    pumpUntilCaptured(1, 1000);
    check(g_captured_count == 1, "exactly one CHUNK_DIFFS packet came back for an untouched column");
    if (g_captured_count == 1) {
        uint8_t canon[BS_CHUNK_DIFFS_BYTES(0)];
        size_t canon_len = buildCanonicalChunkDiffs(canon, cx, cz, BS_CHUNK_DIFFS_LAST, NULL, 0);
        check(g_captured_len[0] == canon_len && g_captured_len[0] == BS_CHUNK_DIFFS_HDR_BYTES,
              "zero-entry CHUNK_DIFFS is exactly BS_CHUNK_DIFFS_HDR_BYTES (12) long");
        check(memcmp(g_captured[0], canon, canon_len) == 0,
              "zero-entry CHUNK_DIFFS is byte-identical to the canonical encoding "
              "(type, cx, cz, flags=LAST, count=0)");
    }

    worldExit(&w);
}

/* ------------------------------------------------------------------------------- scenario 3 --
 * A column with exactly BS_CHUNK_DIFFS_MAX_ENTRIES diffs: bsgame.c's send_chunk_diffs() flushes
 * a batch the instant it fills, so 64 diffs produce a FULL packet (count 64, flags 0 — "more
 * still to come") followed immediately by an empty LAST packet, never one 64-entry LAST packet.
 * That split is itself part of what this scenario proves both ends agree on. */

static void test_full_batch_max_entries(void)
{
    puts("real interop: exactly BS_CHUNK_DIFFS_MAX_ENTRIES diffs split into a full non-LAST "
         "batch plus an empty LAST batch, both byte-identical to the spec");

    const uint32_t seederA = 0x51EEED02u, seederB = 0x51EEED03u;
    const uint32_t client  = 0xC1E00003u;
    const int32_t  cx = 5, cz = 5;   /* column (5,5): x in 80..95, z in 80..95 */

    struct diff_entry e[BS_CHUNK_DIFFS_MAX_ENTRIES];
    for (int i = 0; i < (int)BS_CHUNK_DIFFS_MAX_ENTRIES; i++) {
        e[i].x     = 80 + (i % 16);
        e[i].y     = 10 + i;
        e[i].z     = 80 + (i / 16);
        e[i].block = (uint8_t)(BLOCK_DIRT + (i % 3));   /* DIRT, STONE, SAND, repeating */
    }

    send_join(seederA, "seederA");
    send_join(seederB, "seederB");
    for (int i = 0; i < 32; i++) seed_block_edit(seederA, e[i].x, e[i].y, e[i].z, e[i].block);
    for (int i = 32; i < 64; i++) seed_block_edit(seederB, e[i].x, e[i].y, e[i].z, e[i].block);

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    check(worldColumnCreate(&w, cx, cz) != NULL, "test column (5,5) loaded ahead of the sub");

    send_join(client, "client3");
    g_client_sid = client;
    captureReset();

    networldSubscribeColumn(cx, cz);

    pumpUntilCaptured(2, 1500);
    check(g_captured_count == 2,
          "64 diffs produced exactly two CHUNK_DIFFS packets (one full, one empty LAST)");
    if (g_captured_count == 2) {
        uint8_t canon_full[BS_CHUNK_DIFFS_BYTES(BS_CHUNK_DIFFS_MAX_ENTRIES)];
        size_t canon_full_len = buildCanonicalChunkDiffs(canon_full, cx, cz, 0, e,
                                                          (uint16_t)BS_CHUNK_DIFFS_MAX_ENTRIES);
        check(g_captured_len[0] == canon_full_len,
              "first packet's length matches BS_CHUNK_DIFFS_BYTES(BS_CHUNK_DIFFS_MAX_ENTRIES)");
        check(memcmp(g_captured[0], canon_full, canon_full_len) == 0,
              "first packet (64 entries, flags 0) is byte-identical to the canonical encoding, "
              "including all 64 entries in insertion order");

        uint8_t canon_empty[BS_CHUNK_DIFFS_BYTES(0)];
        size_t canon_empty_len = buildCanonicalChunkDiffs(canon_empty, cx, cz,
                                                           BS_CHUNK_DIFFS_LAST, NULL, 0);
        check(g_captured_len[1] == canon_empty_len,
              "second packet's length matches BS_CHUNK_DIFFS_BYTES(0)");
        check(memcmp(g_captured[1], canon_empty, canon_empty_len) == 0,
              "second packet (0 entries, flags LAST) is byte-identical to the canonical encoding");
    }

    check(worldGet(&w, e[0].x, e[0].y, e[0].z) == e[0].block, "first seeded block landed");
    check(worldGet(&w, e[31].x, e[31].y, e[31].z) == e[31].block,
          "last block from the first seeder landed");
    check(worldGet(&w, e[32].x, e[32].y, e[32].z) == e[32].block,
          "first block from the second seeder landed");
    check(worldGet(&w, e[63].x, e[63].y, e[63].z) == e[63].block, "64th (last) seeded block landed");

    worldExit(&w);
}

/* ------------------------------------------------------------------------------- scenario 4 --
 * SUB and UNSUB, proven not just by their own bytes but by what they actually DO on a real
 * server: a live edit in the subscribed column must reach this client while subscribed, and must
 * NOT reach it once unsubscribed. That is only possible if the server parsed cx/cz from the exact
 * offsets the client's real encoder wrote them at — a length-only check (which is all bsgame_
 * test.c's own malformed-CHUNK_SUB case exercises) cannot tell a correct cx/cz apart from ones
 * read one byte off. */

static void test_sub_then_unsub_real_roundtrip(void)
{
    puts("real interop: SUB/UNSUB bytes are byte-identical to the spec, AND the real server "
         "actually gates live broadcasts on them");

    const uint32_t seeder = 0x51EEED04u;
    const uint32_t client  = 0xC1E00004u;
    const int32_t  cx = 9, cz = 9;   /* column (9,9): x in 144..159, z in 144..159 */

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    check(worldColumnCreate(&w, cx, cz) != NULL, "test column (9,9) loaded ahead of the sub");

    send_join(seeder, "seeder4");
    send_join(client, "client4");
    g_client_sid = client;
    captureReset();

    /* ---- SUB: bytes, then the zero-diff CHUNK_DIFFS every fresh CHUNK_SUB gets. ---- */
    networldSubscribeColumn(cx, cz);

    uint8_t canon_sub[BS_CHUNK_SUB_BYTES];
    buildCanonicalChunkSub(canon_sub, cx, cz);
    check(g_last_sent_len == BS_CHUNK_SUB_BYTES, "CHUNK_SUB encoded to exactly BS_CHUNK_SUB_BYTES");
    check(memcmp(g_last_sent, canon_sub, BS_CHUNK_SUB_BYTES) == 0,
          "CHUNK_SUB bytes are byte-identical to the canonical (cx,cz) encoding");

    pumpUntilCaptured(1, 1000);
    check(g_captured_count == 1, "CHUNK_SUB got its (empty) CHUNK_DIFFS reply");

    /* ---- while subscribed: a live edit from someone else must reach this client. ---- */
    seed_block_edit(seeder, 150, 10, 150, BLOCK_STONE);
    pumpFor(400);
    check(worldGet(&w, 150, 10, 150) == BLOCK_STONE,
          "a live edit in the SUBSCRIBED column reached the client and landed "
          "(proves the server parsed CHUNK_SUB's cx/cz correctly, not just its length)");

    /* ---- UNSUB: bytes, then confirm the live edit no longer arrives. ---- */
    captureReset();
    networldUnsubscribeColumn(cx, cz);

    uint8_t canon_unsub[BS_CHUNK_UNSUB_BYTES];
    buildCanonicalChunkUnsub(canon_unsub, cx, cz);
    check(g_last_sent_len == BS_CHUNK_UNSUB_BYTES,
          "CHUNK_UNSUB encoded to exactly BS_CHUNK_UNSUB_BYTES");
    check(memcmp(g_last_sent, canon_unsub, BS_CHUNK_UNSUB_BYTES) == 0,
          "CHUNK_UNSUB bytes are byte-identical to the canonical (cx,cz) encoding");

    seed_block_edit(seeder, 151, 11, 151, BLOCK_SAND);
    pumpFor(400);
    check(worldGet(&w, 151, 11, 151) == BLOCK_AIR,
          "a live edit in the now-UNSUBSCRIBED column did NOT reach the client "
          "(proves the server parsed CHUNK_UNSUB's cx/cz correctly, not just its length)");

    worldExit(&w);
}

/* ------------------------------------------------------------------------------- scenario 5 --
 * Inventory sync, v1.3.0/v1.4.0. This is the single most important check in this file for the
 * feature: proto/bs_proto.h's whole deployment-order safety argument (an old client never sends
 * INV_ACTION, because it never received an INV_STATE to arm it) rests on the server actually
 * volunteering INV_STATE at JOIN without being asked — see bs_proto.h's long comment above
 * enum bs_inv_op and BS_APP_INV_STATE's own entry in enum bs_app_msg. bsgame_test.c already
 * proves the server sends one (grep its own "JOIN sends an INV_STATE" case); this proves the
 * REAL client — the same net/networld.c the console runs — actually receives and decodes it
 * over the real socket, unprompted: nothing in this scenario ever calls networldSendInvAction()
 * or anything else that could be mistaken for a request. */

static void test_inv_state_volunteered_at_join(void)
{
    puts("real interop: JOIN alone gets the real client a BS_APP_INV_STATE from the real server, "
         "WITHOUT the client ever asking for one, byte-identical to a fresh empty inventory");

    const uint32_t client = 0xC1E00010u;

    networldInit();
    networldSetInvHook(invHook, NULL);
    g_inv_hook_calls = 0;
    g_client_sid = client;
    captureReset();
    captureInvReset();

    send_join(client, "invclient1");   /* the ONLY thing this scenario does before pumping */

    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1,
          "exactly one BS_APP_INV_STATE arrived after JOIN, with no CHUNK_SUB, no INV_ACTION, "
          "nothing else sent by this client");
    if (g_captured_inv_count == 1) {
        check(g_captured_inv_len[0] == BS_INV_STATE_BYTES,
              "the unprompted INV_STATE is exactly BS_INV_STATE_BYTES (50) long, accepted by "
              "networld.c's length check rather than dropped");

        uint8_t zero_item[BS_INV_SLOT_COUNT]  = { 0 };
        uint8_t zero_count[BS_INV_SLOT_COUNT] = { 0 };
        uint8_t canon[BS_INV_STATE_BYTES];
        buildCanonicalInvState(canon, 0, zero_item, zero_count);
        check(memcmp(g_captured_inv[0], canon, BS_INV_STATE_BYTES) == 0,
              "a brand-new player's INV_STATE is byte-identical to the canonical empty-inventory "
              "encoding (selected_hotbar=0, all 24 slots {item=0,count=0})");
    }

    check(g_inv_hook_calls == 1,
          "the real client's own decode (networld.c's applyInvState) parsed it and fired the "
          "registered inv hook exactly once");
    check(g_last_inv_state.selected_hotbar == 0,
          "the client's decoded state agrees with the raw bytes: selected_hotbar 0");
}

/* ------------------------------------------------------------------------------- scenario 6 --
 * networldSendInvAction()'s capability-probe gate (networld.c, and networld.h's own comment on
 * it): refuses before any INV_STATE has been heard this session, and actually sends once one
 * has. This is the other half of the deployment-order argument scenario 5 covers — that argument
 * is only as good as this gate actually holding in the real client linked against the real
 * server, not merely documented. */

static void test_inv_action_gated_on_inv_state(void)
{
    puts("real interop: networldSendInvAction() refuses to send before any BS_APP_INV_STATE has "
         "arrived this session, and actually sends once the real server's JOIN reply has landed");

    const uint32_t client = 0xC1E00011u;

    networldInit();
    networldSetInvHook(invHook, NULL);
    g_inv_hook_calls = 0;
    g_client_sid = client;
    captureReset();
    captureInvReset();

    check(networldSendInvAction(BS_INV_OP_SELECT, 0, 0, 0) == false,
          "networldSendInvAction() refuses before any BS_APP_INV_STATE has been received this "
          "session (fresh networldInit(), no packets exchanged yet)");
    check(g_last_sent_len == 0, "the refused call touched the transport for nothing at all");

    send_join(client, "invclient2");
    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1, "the join's unprompted INV_STATE arrived");

    captureReset();
    check(networldSendInvAction(BS_INV_OP_SELECT, 0, 0, 0) == true,
          "networldSendInvAction() now sends, having heard an INV_STATE from the real server");
    check(g_last_sent_len == BS_INV_ACTION_BYTES,
          "the now-permitted INV_ACTION is exactly BS_INV_ACTION_BYTES (5) long");
}

/* ------------------------------------------------------------------------------- scenario 7 --
 * A real round trip that changes server state: BS_INV_OP_SELECT, the easiest op to prove with
 * (bs_proto.h: "SELECT is the only idempotent op ... applying it twice is applying it once"), so
 * this sends it and then reads the server's NEXT INV_STATE snapshot back, checking both the raw
 * bytes on the wire (g_captured_inv) and the real client's own decode of them (g_last_inv_state,
 * via the registered hook) agree that selected_hotbar changed and nothing else did. */

static void test_inv_select_roundtrip_reflected_in_snapshot(void)
{
    puts("real interop: BS_INV_OP_SELECT sent to the real server changes selected_hotbar, and the "
         "server's NEXT INV_STATE snapshot reflects it, both on the wire and in the client's own "
         "decode");

    const uint32_t client = 0xC1E00012u;
    const uint8_t  target_slot = 3;

    networldInit();
    networldSetInvHook(invHook, NULL);
    g_inv_hook_calls = 0;
    g_client_sid = client;
    captureReset();
    captureInvReset();

    send_join(client, "invclient3");
    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1, "join's INV_STATE arrived");
    check(g_last_inv_state.selected_hotbar == 0, "a fresh player starts on hotbar slot 0");

    captureInvReset();
    g_inv_hook_calls = 0;
    check(networldSendInvAction(BS_INV_OP_SELECT, target_slot, 0, 0) == true,
          "SELECT(3) sent to the real server");

    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1, "the server answered the SELECT with exactly one new INV_STATE");
    if (g_captured_inv_count == 1) {
        check(g_captured_inv_len[0] == BS_INV_STATE_BYTES,
              "the reply is exactly BS_INV_STATE_BYTES long");
        check(g_captured_inv[0][1] == target_slot,
              "the raw reply's selected_hotbar byte (offset 1) reflects the SELECT this client "
              "just sent");
    }

    check(g_inv_hook_calls == 1, "the client's own decode parsed the reply and fired the hook once");
    check(g_last_inv_state.selected_hotbar == target_slot,
          "the client's own decode agrees with the raw bytes: selected_hotbar == 3 after the "
          "round trip");

    bool slots_unchanged = true;
    for (int i = 0; i < NETWORLD_INV_SLOT_COUNT; i++) {
        if (g_last_inv_state.slots[i].item != 0 || g_last_inv_state.slots[i].count != 0) {
            slots_unchanged = false;
        }
    }
    check(slots_unchanged,
          "SELECT changed selected_hotbar and nothing else — all 24 slots are still empty");
}

/* ------------------------------------------------------------------------------- scenario 8 --
 * BS_INV_OP_PICKUP, taken on trust by the real server by design (bs_proto.h's comment above
 * BS_INV_OP_PICKUP/BS_INV_OP_CONSUME: bsgame has no terrain generator to check a pickup report
 * against, so it never will). "Taken on trust" only matters if it actually always lands — this
 * proves a real PICKUP sent by the real client always shows up in the real server's next
 * snapshot, the same round-trip shape as scenario 7 but for the op the feature depends on most
 * for ordinary play (every block break goes through this, not through SELECT). */

static void test_inv_pickup_roundtrip_reflected_in_snapshot(void)
{
    puts("real interop: BS_INV_OP_PICKUP sent to the real server always lands in the NEXT "
         "INV_STATE snapshot — taken on trust, by design, and always credited");

    const uint32_t client = 0xC1E00013u;
    const uint8_t  item   = BLOCK_DIRT;
    const uint8_t  count  = 5;

    networldInit();
    networldSetInvHook(invHook, NULL);
    g_inv_hook_calls = 0;
    g_client_sid = client;
    captureReset();
    captureInvReset();

    send_join(client, "invclient4");
    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1, "join's INV_STATE arrived");

    captureInvReset();
    g_inv_hook_calls = 0;
    check(networldSendInvAction(BS_INV_OP_PICKUP, item, count, 0) == true,
          "PICKUP(BLOCK_DIRT, 5) sent to the real server");

    pumpUntilInvCaptured(1, 1000);
    check(g_captured_inv_count == 1, "the server answered the PICKUP with a new INV_STATE");
    check(g_inv_hook_calls == 1, "the client's own decode parsed the reply and fired the hook once");

    bool found = false;
    for (int i = 0; i < NETWORLD_INV_SLOT_COUNT; i++) {
        if (g_last_inv_state.slots[i].item == item && g_last_inv_state.slots[i].count == count) {
            found = true;
            break;
        }
    }
    check(found,
          "the picked-up item (BLOCK_DIRT x5) landed somewhere in the real server's snapshot, "
          "credited without server-side verification, exactly as bs_proto.h documents PICKUP "
          "must be");
}

/* ------------------------------------------------------------------------------- scenario 9 --
 * v1.8.3 Phase 4, and the check this phase actually rests on: BS_APP_WORLD_GEN, from the real
 * bsgame daemon, decoded by the real net/networld.c, in one measurement.
 *
 * Every other Phase 4 check is one side talking to itself. net/networld_test.c builds the packet
 * with the same macros it decodes with; game/bsgame_test.c drives the real daemon but reads the
 * reply with a decoder written beside the encoder. Both are worth having and neither can see a
 * disagreement BETWEEN the two programs, which is the only failure mode that matters for a wire
 * message — and the one that ends with two builds generating different terrain from the same
 * seed and never noticing.
 *
 * Three independent things are asserted, and it is worth being explicit about why none of them
 * subsumes the others:
 *
 *   * The BYTES on the wire equal buildCanonicalWorldGen()'s, which is written from bs_proto.h
 *     alone. Neither side's encoder is the reference.
 *   * The VALUE is INTEROP_SEEDED_WORLD_GEN, written into the daemon's --state-dir before it
 *     booted (see main()). Deliberately not 1: 1 is BSGAME_WORLD_GEN_DEFAULT, so a server that
 *     ignored its state dir entirely — or a send_world_gen() with the number baked into it —
 *     would answer 1 and a check expecting 1 would pass. 258 can only have come from the file.
 *   * The ORDER is WORLD_INFO then WORLD_GEN, adjacent. The client's entry gate assumes it.
 *
 * 258 is also 0x0102, which is not a palindrome, so the low-byte-first check below fails against
 * a big-endian encoder even though both sides use bs_put_u16/bs_get_u16 and would therefore
 * agree with each other whichever order those macros used. */
#define INTEROP_SEEDED_WORLD_GEN 258u

static void test_world_gen_declared_at_join(void)
{
    puts("real interop: JOIN alone gets the real client a BS_APP_WORLD_GEN from the real server, "
         "carrying the generator its state dir declares, immediately behind WORLD_INFO");

    const uint32_t client = 0xC1E00014u;

    networldInit();
    g_client_sid = client;
    captureReset();
    captureInvReset();
    captureGenReset();

    send_join(client, "genclient1");   /* the ONLY thing this scenario does before pumping */

    pumpUntilGenCaptured(1, 1000);
    check(g_captured_gen_count == 1,
          "exactly one BS_APP_WORLD_GEN arrived after JOIN, unprompted — the client asked for "
          "nothing and there is no request message it could have asked with");

    if (g_captured_gen_count == 1) {
        check(g_captured_gen_len[0] == BS_WORLD_GEN_BYTES,
              "it is exactly BS_WORLD_GEN_BYTES (3) long, so networld.c's strict-equality length "
              "check accepts it rather than dropping it whole");

        uint8_t canon[BS_WORLD_GEN_BYTES];
        buildCanonicalWorldGen(canon, (uint16_t)INTEROP_SEEDED_WORLD_GEN);
        check(memcmp(g_captured_gen[0], canon, BS_WORLD_GEN_BYTES) == 0,
              "and byte-identical to the canonical encoding built from bs_proto.h's macros "
              "alone, neither side's encoder used as the reference");

        check(g_captured_gen[0][BS_APP_HDR_BYTES]     == 0x02
              && g_captured_gen[0][BS_APP_HDR_BYTES + 1] == 0x01,
              "the two payload bytes are 02 01 on the wire — little-endian, low byte first, "
              "across a real process boundary rather than through a matching put/get pair");
    }

    /* The client's OWN decode, which is the half a raw byte capture cannot speak for. */
    uint32_t gen = 0xDEADBEEFu;
    check(networldServerGenVersion(&gen),
          "the real networld.c accepted it: this session now has a declared generator");
    check(gen == INTEROP_SEEDED_WORLD_GEN,
          "and the value is the one written into the daemon's --state-dir before it booted — "
          "not BSGAME_WORLD_GEN_DEFAULT, so it was really read from the file it is persisted in");

    uint32_t seed = 0xDEADBEEFu;
    check(networldWorldSeed(&seed),
          "control: the seed landed too, so this is a real join burst and not a lone packet");

    check(g_join_order_count >= 2,
          "at least two packets arrived in the burst");
    if (g_join_order_count >= 2) {
        check(g_join_order[0] == BS_APP_WORLD_INFO && g_join_order[1] == BS_APP_WORLD_GEN,
              "and WORLD_GEN is the packet immediately behind WORLD_INFO, which is the ordering "
              "networldGenWaiting()'s 250 ms grace is sized for");
    }

    check(!networldGenWaiting(),
          "so the entry gate is open by the end of the burst rather than held for the grace");
}

/* --------------------------------------------------------------------------------------- main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    atexit(reap_daemon);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS,  crash_handler);

    snprintf(g_dir, sizeof g_dir, "/tmp/bsgame_interop_%d", (int)getpid());
    if (mkdir(g_dir, 0700) != 0) die("mkdir state dir");

    /* v1.8.3 Phase 4. Written BEFORE the daemon starts, so bsgame's world_gen_load() finds it on
     * first boot and adopts it instead of minting BSGAME_WORLD_GEN_DEFAULT. This is what makes
     * scenario 9's value check mean something: 258 is not the default and is not a number
     * anywhere in bsgame.c, so it can only have reached the client by being read out of this
     * file and put on the wire.
     *
     * It is emphatically NOT a statement about which generator a real deployment should declare.
     * This is a throwaway /tmp state dir that is created and torn down inside one test process.
     * See world/water.h and the Phase 4 commit message for why a real server declares LEGACY. */
    {
        char gp[128];
        snprintf(gp, sizeof gp, "%s/world_gen.txt", g_dir);
        FILE *gf = fopen(gp, "w");
        if (gf == NULL) die("write world_gen.txt");
        fprintf(gf, "%u\n", INTEROP_SEEDED_WORLD_GEN);
        fclose(gf);
    }

    puts("== interop_test: client (networld.c) <-> real bsgame daemon ==");

    set_sock_paths();
    open_sockets();
    start_daemon();
    if (!wait_ready(5000)) {
        fprintf(stderr, "interop_test: daemon never became ready (is " BSGAME_BIN " built?)\n");
        reap_daemon();
        return 1;
    }

    test_normal_multi_entry_batch();
    test_empty_last_batch();
    test_full_batch_max_entries();
    test_sub_then_unsub_real_roundtrip();

    test_inv_state_volunteered_at_join();
    test_inv_action_gated_on_inv_state();
    test_inv_select_roundtrip_reflected_in_snapshot();
    test_inv_pickup_roundtrip_reflected_in_snapshot();

    test_world_gen_declared_at_join();

    reap_daemon();

    /* Last, so it sees every check the eight tests above managed to run. It is not reached on the
     * wait_ready() path that returns 1 further up, and does not need to be: that path already
     * fails loudly with its own message and a non-zero exit. */
    check_count_pin();

    printf("%s: %d checks, %d failed\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif /* __3DS__ */
