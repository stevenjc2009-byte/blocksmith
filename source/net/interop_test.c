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
 * Duplicated here exactly as server/game/bsgame_test.c already duplicates it, for the identical
 * reason: this is the one other place in the tree that has to stand in for bsgate without
 * including gateway-private code. Canonical values: deps/blocksmith-server/game/bsgame.c. */
enum bs_game_msg {
    BS_GAME_JOIN  = 1,   /* gate -> game: sid(4) + pubkey(32) + label(32) */
    BS_GAME_DATA  = 2,   /* both ways:    sid(4) + payload                */
    BS_GAME_LEAVE = 3,   /* gate -> game: sid(4)                          */
    BS_GAME_KICK  = 4    /* game -> gate: sid(4)                          */
};

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

    reap_daemon();

    printf("%s: %d checks, %d failed\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif /* __3DS__ */
