/* networld_test — host unit test for the transport <-> World glue.
 *
 * In the style of net/blockdiff_test.c: "ok"/"FAIL" lines per check, a final "PASS/FAIL N
 * checks, M failed" summary, non-zero exit on any failure. Unlike blockdiff_test.c this links
 * the REAL world/world.c (+ chunk.c, budget.c) as well as net/blockdiff.c, so every check here
 * exercises the actual wiring networld.c and world.c now share — including world.c's own
 * worldSet() hook (source/world/world.c, the "new_column" branch) — not a stand-in for it.
 *
 * What is NOT linked: net/bsnet_transport.c. It is Noise XX over a real UDP socket via
 * libhydrogen, none of it host-portable (see bsnet_transport.c's own header), and networld.c
 * only ever calls two functions from it — netTransportRecv() and netTransportSend(). Both are
 * faked below, the same link-seam technique server/game/bsgame_test.c uses to exercise
 * handle_block_edit() without a live socket. See networld.h's own comment on
 * networldApplyPayload() for why that is the primary entry point most scenarios below drive
 * directly, with the fake transport reserved for the two scenarios — the per-frame pump and
 * the outbound send — that are actually about the transport boundary itself.
 *
 * The __3DS__ guard is load-bearing, not tidy — the same one blockdiff_test.c and
 * app/options_test.c carry: mc/Makefile globs every .c under source/net into the console
 * build, so without it this file's main() collides with source/main.c's.
 */
#ifndef __3DS__

#include "net/networld.h"

#include <stdio.h>
#include <string.h>

#include "net/blockdiff.h"
#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/block.h"
#include "world/world.h"

#include "proto/bs_proto.h"

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

/* ------------------------------------------------------- fake transport ------------------ */
/* Link-time test double for net/bsnet_transport.h. networld.o references netTransportRecv()
 * and netTransportSend() as extern symbols; these definitions satisfy the linker instead of
 * the real bsnet_transport.o, which this Makefile never builds. */

#define FAKE_RECV_QUEUE_MAX 64

static uint8_t fake_recv_buf[FAKE_RECV_QUEUE_MAX][NET_MAX_PAYLOAD];
static int     fake_recv_len[FAKE_RECV_QUEUE_MAX];
static int     fake_recv_head, fake_recv_tail, fake_recv_count;

static uint8_t fake_sent_buf[NET_MAX_PAYLOAD];
static size_t  fake_sent_len;
static int     fake_sent_calls;
static bool    fake_send_fail;

static void fakeTransportReset(void)
{
    fake_recv_head = fake_recv_tail = fake_recv_count = 0;
    fake_sent_len   = 0;
    fake_sent_calls = 0;
    fake_send_fail  = false;
}

static void fakeRecvPush(const uint8_t *data, size_t len)
{
    if (fake_recv_count >= FAKE_RECV_QUEUE_MAX) return;   /* test bug, not a networld bug */
    memcpy(fake_recv_buf[fake_recv_tail], data, len);
    fake_recv_len[fake_recv_tail] = (int)len;
    fake_recv_tail = (fake_recv_tail + 1) % FAKE_RECV_QUEUE_MAX;
    fake_recv_count++;
}

int netTransportRecv(uint8_t *out, size_t cap)
{
    if (fake_recv_count == 0) return 0;

    int len = fake_recv_len[fake_recv_head];
    if ((size_t)len > cap) len = (int)cap;
    memcpy(out, fake_recv_buf[fake_recv_head], (size_t)len);

    fake_recv_head = (fake_recv_head + 1) % FAKE_RECV_QUEUE_MAX;
    fake_recv_count--;
    return len;
}

bool netTransportSend(const uint8_t *payload, size_t len)
{
    fake_sent_calls++;
    if (fake_send_fail) return false;
    if (len > sizeof fake_sent_buf) return false;
    memcpy(fake_sent_buf, payload, len);
    fake_sent_len = len;
    return true;
}

/* ------------------------------------------------------- fake clock ----------------------- */
/* Link-time test double for net/bsnet_sock.h's bsSockNowMs(). Makefile.networld-test does not
 * link bsnet_sock.c (its real clock_gettime()/osGetTime() implementation) any more than it
 * links bsnet_transport.c — see that Makefile's own comment for why. This is the same
 * link-seam technique as the fake transport above, and it is what lets the remote-aging and
 * pose-rate-limit scenarios below move time by hand instead of sleeping in a test. */

static uint64_t fake_now_ms;

static void fakeNowSet(uint64_t ms)     { fake_now_ms = ms; }
static void fakeNowAdvance(uint64_t ms) { fake_now_ms += ms; }

uint64_t bsSockNowMs(void) { return fake_now_ms; }

/* ------------------------------------------------------- payload builders ----------------- */

static void buildBlockEdit(uint8_t *out, int32_t x, int32_t y, int32_t z, uint8_t block)
{
    out[0] = BS_APP_BLOCK_EDIT;
    bs_put_i32(out + 1, x);
    bs_put_i32(out + 5, y);
    bs_put_i32(out + 9, z);
    out[13] = block;
}

struct sync_entry { int32_t x, y, z; uint8_t block; };

static size_t buildWorldSync(uint8_t *out, const struct sync_entry *e, uint16_t n)
{
    out[0] = BS_APP_WORLD_SYNC;
    bs_put_u16(out + 1, n);
    uint8_t *p = out + 3;
    for (uint16_t i = 0; i < n; i++) {
        bs_put_i32(p,     e[i].x);
        bs_put_i32(p + 4, e[i].y);
        bs_put_i32(p + 8, e[i].z);
        p[12] = e[i].block;
        p += BS_SYNC_ENTRY_BYTES;
    }
    return BS_WORLD_SYNC_BYTES(n);
}

static void buildPosUpdateS(uint8_t *out, uint32_t sid, float x, float y, float z,
                             float yaw, float pitch)
{
    out[0] = BS_APP_POS_UPDATE;
    bs_put_u32(out + 1, sid);
    bs_put_f32(out + 5,  x);
    bs_put_f32(out + 9,  y);
    bs_put_f32(out + 13, z);
    bs_put_f32(out + 17, yaw);
    bs_put_f32(out + 21, pitch);
}

/* ------------------------------------------------------------------ scenarios ------------- */

static void test_loaded_column_applies_directly(void)
{
    puts("a remote edit into an already-loaded column applies immediately");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    worldColumnCreate(&w, 0, 0);   /* column (0,0): x,z in 0..15 */

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 5, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);

    check(worldGet(&w, 5, 10, 5) == BLOCK_STONE, "the block landed in the world");
    check(networldPendingCount() == 0, "nothing was queued for a column that was already loaded");
}

static void test_unloaded_column_queues_not_drops(void)
{
    puts("a remote edit into a column we have not streamed in is queued, not dropped");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    /* Column (2,0): x in 32..47, z in 0..15. Never created. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 40, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);

    check(worldColumn(&w, 2, 0) == NULL,
          "the edit must not have created a phantom column outside the streaming ring");
    check(worldGet(&w, 40, 10, 5) == BLOCK_AIR, "nothing was written anywhere for it yet");
    check(networldPendingCount() == 1, "it was queued instead");
    check(networldPendingRefusals() == 0, "the store had room");
}

static void test_explicit_column_load_drains_pending(void)
{
    puts("networldOnColumnLoad() drains everything pending for that column");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    uint8_t m1[BS_BLOCK_EDIT_BYTES], m2[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(m1, 40, 10, 5, BLOCK_STONE);   /* column (2,0) */
    buildBlockEdit(m2, 35, 20, 3, BLOCK_SAND);    /* column (2,0), same column */
    networldApplyPayload(m1, sizeof m1);
    networldApplyPayload(m2, sizeof m2);
    check(networldPendingCount() == 2, "both queued");

    /* The column becomes real without going through worldSet() at all — e.g. the streaming
     * install path (app/worker.c -> world/world.c worldChunkCreate), which is why
     * networldOnColumnLoad() has to be its own callable entry point and not only a side
     * effect of worldSet(). */
    worldColumnCreate(&w, 2, 0);
    networldOnColumnLoad(&w, 2, 0);

    check(worldGet(&w, 40, 10, 5) == BLOCK_STONE, "first pending diff landed");
    check(worldGet(&w, 35, 20, 3) == BLOCK_SAND, "second pending diff landed");
    check(networldPendingCount() == 0, "store is empty after the drain");
}

static void test_column_load_ignores_foreign_world(void)
{
    puts("networldOnColumnLoad() for a World other than the registered live one is a no-op");

    World live;
    worldInit(&live);
    networldInit();
    networldSetWorld(&live);

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 40, 10, 5, BLOCK_STONE);   /* column (2,0) */
    networldApplyPayload(msg, sizeof msg);
    check(networldPendingCount() == 1, "queued against the live world");

    /* Stands in for app/worker.c's worker thread, which builds its own private staging World
     * and calls the very same world/world.c primitives world.c's hook lives inside. This is
     * the scenario that hook has to get right: a column finishing in the STAGING world must
     * never drain into, or even touch, the live one. */
    World staging;
    worldInit(&staging);
    worldColumnCreate(&staging, 2, 0);
    networldOnColumnLoad(&staging, 2, 0);

    check(networldPendingCount() == 1,
          "a call for a different World than networldSetWorld() registered must not drain anything");
    check(worldGet(&staging, 40, 10, 5) == BLOCK_AIR,
          "and must not have written into that foreign world either");

    /* The real drain, against the world that was actually registered, still works. */
    worldColumnCreate(&live, 2, 0);
    networldOnColumnLoad(&live, 2, 0);
    check(networldPendingCount() == 0, "draining against the correct world still works");
    check(worldGet(&live, 40, 10, 5) == BLOCK_STONE, "and the diff landed there");

    worldExit(&staging);
    worldExit(&live);
}

static void test_worldset_hook_autodrains(void)
{
    puts("world/world.c's own worldSet() hook drains pending diffs the instant it creates a column");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    /* Column (1,0): x in 16..31, z in 0..15. Queue a remote diff for it while it does not
     * exist yet. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 20, 10, 5, BLOCK_SAND);
    networldApplyPayload(msg, sizeof msg);
    check(networldPendingCount() == 1, "queued while column (1,0) does not exist");

    /* A plain, ordinary worldSet() call — the same one scene/interact.c makes for a local
     * player's own edit, or worldgen.c makes while generating terrain — is what actually
     * creates the column here, with no call to networldOnColumnLoad() in this test at all.
     * The drain has to come from world.c's own hook, wired in production, not from the test
     * doing the module's job for it. */
    check(worldSet(&w, 16, 5, 2, BLOCK_STONE), "the local write that brings column (1,0) into being");

    check(worldGet(&w, 16, 5, 2) == BLOCK_STONE, "the direct local write happened");
    check(worldGet(&w, 20, 10, 5) == BLOCK_SAND,
          "the earlier queued remote diff landed automatically via world.c's own hook");
    check(networldPendingCount() == 0, "store drained by the hook, not by the test");
}

static void test_validation_rejects_hostile_input(void)
{
    puts("out-of-range coordinates and unknown block ids are rejected, not applied or queued");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);   /* loaded, so an accepted edit would be visible immediately */

    uint8_t msg[BS_BLOCK_EDIT_BYTES];

    buildBlockEdit(msg, 5, -1, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 0, 5) == BLOCK_AIR, "y below 0 rejected");

    buildBlockEdit(msg, 5, WORLD_HEIGHT, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, WORLD_HEIGHT - 1, 5) == BLOCK_AIR, "y == WORLD_HEIGHT rejected");

    buildBlockEdit(msg, 5, 10, 5, (uint8_t)BLOCK_COUNT);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == BLOCK_AIR, "block id == BLOCK_COUNT (one past the last real id) rejected");

    buildBlockEdit(msg, 5, 10, 5, (uint8_t)0xFF);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == BLOCK_AIR, "block id 0xFF rejected");

    buildBlockEdit(msg, 500000, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(networldPendingCount() == 0, "an x far outside any legitimate play area is rejected outright, not queued");

    check(networldPendingCount() == 0, "none of the rejected edits were queued either");

    /* A well-formed edit in between the rejected ones still works, proving the rejections
     * above are really rejections and not e.g. a world that stopped accepting writes. */
    buildBlockEdit(msg, 5, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == BLOCK_STONE, "a valid edit in between still applies");

    /* Malformed length: right type byte, wrong size. */
    uint8_t short_msg[BS_BLOCK_EDIT_BYTES - 1] = {0};
    short_msg[0] = BS_APP_BLOCK_EDIT;
    networldApplyPayload(short_msg, sizeof short_msg);
    check(networldPendingCount() == 0, "a truncated BLOCK_EDIT payload is dropped, not misparsed");
}

static void test_world_sync_batch(void)
{
    puts("BS_APP_WORLD_SYNC applies a batch, per-entry, same rules as a single edit");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);   /* column (0,0) loaded; column (5,0) is not */

    struct sync_entry e[3] = {
        { 4, 10, 4, BLOCK_STONE },    /* column (0,0): loaded, should apply directly    */
        { 84, 10, 4, BLOCK_SAND },    /* column (5,0): not loaded, should queue         */
        { 6, 10, 6, (uint8_t)BLOCK_COUNT }, /* invalid block id: should be skipped       */
    };
    uint8_t buf[BS_WORLD_SYNC_BYTES(3)];
    size_t len = buildWorldSync(buf, e, 3);
    networldApplyPayload(buf, len);

    check(worldGet(&w, 4, 10, 4) == BLOCK_STONE, "the loaded-column entry applied");
    check(networldPendingCount() == 1, "the unloaded-column entry was queued, and only it");
    check(worldColumn(&w, 5, 0) == NULL, "queuing did not create a phantom column for it");
    check(worldGet(&w, 6, 10, 6) == BLOCK_AIR, "the invalid entry was skipped, not applied");

    /* A sync packet whose declared count disagrees with its own length is malformed as a
     * whole and must not partially apply. */
    networldInit();   /* fresh pending store for a clean count check below */
    uint8_t bad[BS_WORLD_SYNC_BYTES(3)];
    memcpy(bad, buf, sizeof bad);
    bs_put_u16(bad + 1, 5);   /* claims 5 entries, packet only carries room/bytes for 3 */
    networldApplyPayload(bad, sizeof bad);
    check(networldPendingCount() == 0, "a WORLD_SYNC whose count disagrees with its length is dropped whole");

    /* A count beyond the sender's own cap is rejected outright too. */
    uint8_t over[BS_APP_HDR_BYTES + 2];
    over[0] = BS_APP_WORLD_SYNC;
    bs_put_u16(over + 1, (uint16_t)(BS_SYNC_MAX_ENTRIES + 1));
    networldApplyPayload(over, sizeof over);
    check(networldPendingCount() == 0, "a WORLD_SYNC claiming more than BS_SYNC_MAX_ENTRIES is dropped");
}

static void test_pump_is_bounded_per_frame(void)
{
    puts("networldUpdate() drains at most a bounded number of messages per call");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);   /* column (0,0): x,z in 0..15 — every entry below lands here */

    fakeTransportReset();

    /* 37 distinct, individually valid edits queued in the fake transport — comfortably past
     * networld.c's own NETWORLD_MAX_MSGS_PER_FRAME (32 at the time of writing). If that
     * constant changes this test's numbers need to move with it. Each message targets a
     * DISTINCT (x, z) inside column (0,0) — x = i % 16, z = i / 16, injective for i < 256 —
     * so counting "how many of these positions are now stone" actually counts how many
     * messages were processed instead of just whether the column got touched at all; an
     * earlier version of this test aliased every i onto only 16 positions and could not tell
     * 32 processed apart from 37. */
    const int total = 37;
    for (int i = 0; i < total; i++) {
        uint8_t msg[BS_BLOCK_EDIT_BYTES];
        buildBlockEdit(msg, i % 16, 10, i / 16, BLOCK_STONE);   /* still column (0,0) */
        fakeRecvPush(msg, sizeof msg);
    }

    networldUpdate();

    int applied_after_first = 0;
    for (int i = 0; i < total; i++)
        if (worldGet(&w, i % 16, 10, i / 16) == BLOCK_STONE) applied_after_first++;

    check(applied_after_first == 32,
          "exactly the per-frame cap was processed on the first networldUpdate() call");
    check(fake_recv_count == total - 32, "the rest are still queued in the transport, not lost");

    networldUpdate();

    int applied_after_second = 0;
    for (int i = 0; i < total; i++)
        if (worldGet(&w, i % 16, 10, i / 16) == BLOCK_STONE) applied_after_second++;

    check(applied_after_second == total, "a second call drains the remainder");
    check(fake_recv_count == 0, "transport queue now empty");
}

static void test_send_block_edit_encodes_and_sends(void)
{
    puts("networldSendBlockEdit() encodes BS_APP_BLOCK_EDIT and hands it to the transport");

    fakeTransportReset();

    check(networldSendBlockEdit(-7, 12, 300, BLOCK_WOOD), "send reports success");
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_BLOCK_EDIT_BYTES, "encoded to exactly BS_BLOCK_EDIT_BYTES");
    check(fake_sent_buf[0] == BS_APP_BLOCK_EDIT, "type byte is BS_APP_BLOCK_EDIT");
    check(bs_get_i32(fake_sent_buf + 1) == -7,  "x round-trips, including negative");
    check(bs_get_i32(fake_sent_buf + 5) == 12,  "y round-trips");
    check(bs_get_i32(fake_sent_buf + 9) == 300, "z round-trips");
    check(fake_sent_buf[13] == BLOCK_WOOD, "block id round-trips");

    /* Fire-and-forget: a transport-level failure (no session, socket would block) surfaces as
     * a plain false, and must not be retried or queued anywhere in this module — the caller's
     * local worldSet() has already happened and is authoritative for this client either way. */
    fake_send_fail = true;
    check(networldSendBlockEdit(1, 1, 1, BLOCK_STONE) == false, "a transport-level send failure propagates as false");
}

static void test_pos_update_registers_remote(void)
{
    puts("a 25-byte POS_UPDATE registers a remote player with the right sid and floats");

    networldInit();

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 42, 1.5f, 2.5f, -3.5f, 0.25f, -0.75f);
    networldApplyPayload(msg, sizeof msg);

    check(networldRemoteCount() == 1, "one remote registered");

    NetworldRemote r;
    check(networldRemoteGet(0, &r), "index 0 is populated");
    check(r.sid == 42, "sid matches");
    check(r.x == 1.5f, "x matches");
    check(r.y == 2.5f, "y matches");
    check(r.z == -3.5f, "z matches");
    check(r.yaw == 0.25f, "yaw matches");
    check(r.pitch == -0.75f, "pitch matches");
}

static void test_pos_update_wrong_length_dropped(void)
{
    puts("a wrong-length POS_UPDATE (24 and 26 bytes) is dropped, table unchanged");

    networldInit();

    uint8_t short_msg[BS_POS_UPDATE_S_BYTES - 1] = {0};
    short_msg[0] = BS_APP_POS_UPDATE;
    networldApplyPayload(short_msg, sizeof short_msg);
    check(networldRemoteCount() == 0, "24-byte POS_UPDATE dropped");

    uint8_t long_msg[BS_POS_UPDATE_S_BYTES + 1] = {0};
    long_msg[0] = BS_APP_POS_UPDATE;
    networldApplyPayload(long_msg, sizeof long_msg);
    check(networldRemoteCount() == 0, "26-byte POS_UPDATE dropped");
}

static void test_pos_update_same_sid_updates_in_place(void)
{
    puts("a second pose for the same sid updates in place, count stays 1");

    networldInit();

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 7, 1, 2, 3, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    buildPosUpdateS(msg, 7, 10, 20, 30, 1, 1);
    networldApplyPayload(msg, sizeof msg);

    check(networldRemoteCount() == 1, "still just one remote");

    NetworldRemote r;
    check(networldRemoteGet(0, &r), "index 0 populated");
    check(r.x == 10 && r.y == 20 && r.z == 30, "position updated to the newer pose");
}

static void test_pos_update_two_sids_count_two(void)
{
    puts("two distinct sids give count 2");

    networldInit();

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 1, 0, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    buildPosUpdateS(msg, 2, 0, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);

    check(networldRemoteCount() == 2, "two remotes registered");
}

static void test_remote_ages_out_after_timeout(void)
{
    puts("a player ages out after NETWORLD_REMOTE_TIMEOUT_MS of no poses");

    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 9, 0, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    check(networldRemoteCount() == 1, "registered");

    fakeNowAdvance(NETWORLD_REMOTE_TIMEOUT_MS - 1);
    networldUpdate();
    check(networldRemoteCount() == 1, "not yet aged out one ms before the deadline");

    fakeNowAdvance(2);
    networldUpdate();
    check(networldRemoteCount() == 0, "aged out once the timeout has elapsed");
}

static void test_remote_survives_on_repeated_unchanged_pose(void)
{
    puts("a player that keeps sending unchanged poses never ages out");

    networldInit();
    fakeTransportReset();
    fakeNowSet(0);

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 3, 5, 5, 5, 0, 0);
    networldApplyPayload(msg, sizeof msg);

    for (int i = 0; i < 5; i++) {
        fakeNowAdvance(NETWORLD_REMOTE_TIMEOUT_MS - 1);
        networldUpdate();
        check(networldRemoteCount() == 1, "still present just before the deadline");

        /* Refresh with the SAME pose values — this is the whole point: bsgame marks
         * pos_dirty on every POS_UPDATE it receives regardless of whether anything moved
         * (see this module's own header), so an unmoving remote player must keep resetting
         * its own despawn clock here too, or it would vanish for peers despite being live. */
        buildPosUpdateS(msg, 3, 5, 5, 5, 0, 0);
        networldApplyPayload(msg, sizeof msg);
    }

    check(networldRemoteCount() == 1, "never aged out across repeated unchanged refreshes");
}

static void test_16th_remote_dropped_existing_survive(void)
{
    puts("a 16th remote sid is dropped, not evicted, and the existing 15 survive");

    networldInit();

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    for (uint32_t sid = 1; sid <= NETWORLD_MAX_REMOTE; sid++) {
        buildPosUpdateS(msg, sid, 0, 0, 0, 0, 0);
        networldApplyPayload(msg, sizeof msg);
    }
    check(networldRemoteCount() == NETWORLD_MAX_REMOTE, "table filled to capacity");

    buildPosUpdateS(msg, 999, 1, 1, 1, 1, 1);
    networldApplyPayload(msg, sizeof msg);
    check(networldRemoteCount() == NETWORLD_MAX_REMOTE, "the 16th sid was dropped, not evicted");

    bool found_extra = false;
    for (int i = 0; i < networldRemoteCount(); i++) {
        NetworldRemote r;
        check(networldRemoteGet(i, &r), "each of the original slots is still readable");
        if (r.sid == 999) found_extra = true;
    }
    check(!found_extra, "the dropped sid never made it into the table");
}

static void test_send_pose_encodes_and_sends(void)
{
    puts("networldSendPose() emits exactly 21 bytes with type 0x02 and the five floats "
         "round-tripping, including a negative coordinate");

    networldInit();
    fakeTransportReset();
    fakeNowSet(0);

    check(networldSendPose(-1.5f, 2.0f, 3.0f, 0.5f, -0.25f), "send reports success");
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_POS_UPDATE_C_BYTES, "encoded to exactly BS_POS_UPDATE_C_BYTES");
    check(fake_sent_buf[0] == BS_APP_POS_UPDATE, "type byte is BS_APP_POS_UPDATE");
    check(bs_get_f32(fake_sent_buf + 1)  == -1.5f, "x round-trips, including negative");
    check(bs_get_f32(fake_sent_buf + 5)  == 2.0f,  "y round-trips");
    check(bs_get_f32(fake_sent_buf + 9)  == 3.0f,  "z round-trips");
    check(bs_get_f32(fake_sent_buf + 13) == 0.5f,  "yaw round-trips");
    check(bs_get_f32(fake_sent_buf + 17) == -0.25f, "pitch round-trips");
}

static void test_send_pose_rate_limited(void)
{
    puts("calling networldSendPose() twice inside one interval sends only once");

    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);

    check(networldSendPose(0, 0, 0, 0, 0), "first call in a fresh interval sends");
    check(fake_sent_calls == 1, "one send so far");

    fakeNowAdvance(NETWORLD_POSE_INTERVAL_MS - 1);
    check(networldSendPose(1, 1, 1, 1, 1) == false,
          "second call inside the same interval is suppressed");
    check(fake_sent_calls == 1, "transport still only touched once");

    fakeNowAdvance(2);
    check(networldSendPose(2, 2, 2, 2, 2), "call after the interval elapses sends again");
    check(fake_sent_calls == 2, "transport touched a second time");
}

static void test_reset_remotes_empties_table(void)
{
    puts("networldResetRemotes() empties the table");

    networldInit();

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(msg, 1, 0, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    buildPosUpdateS(msg, 2, 0, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    check(networldRemoteCount() == 2, "two remotes before reset");

    networldResetRemotes();
    check(networldRemoteCount() == 0, "table empty after reset");
}

static void test_inert_before_world_is_registered(void)
{
    puts("nothing here touches memory before networldSetWorld() has been called");

    networldInit();
    networldSetWorld(NULL);

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 5, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);   /* must not crash */
    check(networldPendingCount() == 0, "an edit that arrives before any World is registered is simply dropped");

    World w;
    worldInit(&w);
    networldOnColumnLoad(&w, 0, 0);   /* must not crash even though no World is registered */
    check(networldPendingCount() == 0, "still nothing pending");
}

/* ------------------------------------------------------------------------- main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    puts("== networld test ==");

    test_loaded_column_applies_directly();
    test_unloaded_column_queues_not_drops();
    test_explicit_column_load_drains_pending();
    test_column_load_ignores_foreign_world();
    test_worldset_hook_autodrains();
    test_validation_rejects_hostile_input();
    test_world_sync_batch();
    test_pump_is_bounded_per_frame();
    test_send_block_edit_encodes_and_sends();
    test_pos_update_registers_remote();
    test_pos_update_wrong_length_dropped();
    test_pos_update_same_sid_updates_in_place();
    test_pos_update_two_sids_count_two();
    test_remote_ages_out_after_timeout();
    test_remote_survives_on_repeated_unchanged_pose();
    test_16th_remote_dropped_existing_survive();
    test_send_pose_encodes_and_sends();
    test_send_pose_rate_limited();
    test_reset_remotes_empties_table();
    test_inert_before_world_is_registered();

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
