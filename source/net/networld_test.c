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

static size_t buildChunkDiffs(uint8_t *out, int32_t cx, int32_t cz, uint8_t flags,
                               const struct sync_entry *e, uint16_t n)
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

static void buildInvState(uint8_t *out, uint8_t selected_hotbar,
                           const uint8_t *items, const uint8_t *counts)
{
    out[0] = BS_APP_INV_STATE;
    out[1] = selected_hotbar;
    uint8_t *p = out + 2;
    for (uint32_t i = 0; i < BS_INV_SLOT_COUNT; i++) {
        p[0] = items[i];
        p[1] = counts[i];
        p += 2;
    }
}

/* A well-formed inventory: every 4th slot empty (item 0, count 0 — bs_proto.h's own rule),
 * every other slot a distinct non-zero item with a distinct non-zero count, all <=
 * BS_INV_STACK_MAX. Used as the base for both "this round-trips exactly" checks and, mutated in
 * one slot at a time, the malformed-packet checks below. */
static void fillValidInv(uint8_t *items, uint8_t *counts)
{
    for (uint32_t i = 0; i < BS_INV_SLOT_COUNT; i++) {
        if (i % 4 == 0) {
            items[i]  = 0;
            counts[i] = 0;
        } else {
            items[i]  = (uint8_t)(i + 1);
            counts[i] = (uint8_t)((i % BS_INV_STACK_MAX) + 1);
        }
    }
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

/* ------------------------------------------------ per-column diff subscription (V127-A) --- */
/* networldSubscribeColumn()/networldUnsubscribeColumn() are plain encode-and-send, tested the
 * same way test_send_block_edit_encodes_and_sends() covers BS_APP_BLOCK_EDIT above. The
 * receive side, BS_APP_CHUNK_DIFFS, is deliberately tested against the SAME assertions as
 * test_world_sync_batch() above it, entry for entry: the whole point of applyChunkDiffs()
 * (networld.c) is that it is applyOrQueue() wearing a different header, not a second
 * application path, so a test that could tell the two apart would mean that promise had been
 * broken. */

static void test_subscribe_column_encodes_and_sends(void)
{
    puts("networldSubscribeColumn() encodes BS_APP_CHUNK_SUB and hands it to the transport");

    fakeTransportReset();

    networldSubscribeColumn(-3, 12);
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_CHUNK_SUB_BYTES, "encoded to exactly BS_CHUNK_SUB_BYTES");
    check(fake_sent_buf[0] == BS_APP_CHUNK_SUB, "type byte is BS_APP_CHUNK_SUB");
    check(bs_get_i32(fake_sent_buf + 1) == -3, "col_x round-trips, including negative");
    check(bs_get_i32(fake_sent_buf + 5) == 12, "col_z round-trips");

    /* Fire-and-forget, same posture as networldSendBlockEdit(): a transport-level refusal (no
     * session) must not crash, and there is no return value for the caller to check either way
     * — this only proves the attempt still happens and nothing blows up when it fails. */
    fake_send_fail = true;
    networldSubscribeColumn(1, 1);
    check(fake_sent_calls == 2, "still attempted the send even when the transport refuses it");
}

static void test_unsubscribe_column_encodes_and_sends(void)
{
    puts("networldUnsubscribeColumn() encodes BS_APP_CHUNK_UNSUB and hands it to the transport");

    fakeTransportReset();

    networldUnsubscribeColumn(7, -9);
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_CHUNK_UNSUB_BYTES, "encoded to exactly BS_CHUNK_UNSUB_BYTES");
    check(fake_sent_buf[0] == BS_APP_CHUNK_UNSUB, "type byte is BS_APP_CHUNK_UNSUB");
    check(bs_get_i32(fake_sent_buf + 1) == 7,  "col_x round-trips");
    check(bs_get_i32(fake_sent_buf + 5) == -9, "col_z round-trips, including negative");

    fake_send_fail = true;
    networldUnsubscribeColumn(1, 1);
    check(fake_sent_calls == 2, "still attempted the send even when the transport refuses it");
}

static void test_chunk_diffs_batch(void)
{
    puts("BS_APP_CHUNK_DIFFS applies a batch through the same rules as WORLD_SYNC, per entry");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);   /* column (0,0) loaded; column (5,0) is not */

    struct sync_entry e[3] = {
        { 4, 10, 4, BLOCK_STONE },          /* column (0,0): loaded, should apply directly */
        { 84, 10, 4, BLOCK_SAND },          /* column (5,0): not loaded, should queue      */
        { 6, 10, 6, (uint8_t)BLOCK_COUNT }, /* invalid block id: should be skipped         */
    };
    uint8_t buf[BS_CHUNK_DIFFS_BYTES(3)];
    size_t len = buildChunkDiffs(buf, 0, 0, BS_CHUNK_DIFFS_LAST, e, 3);
    networldApplyPayload(buf, len);

    check(worldGet(&w, 4, 10, 4) == BLOCK_STONE, "the loaded-column entry applied");
    check(networldPendingCount() == 1, "the unloaded-column entry was queued, and only it");
    check(worldColumn(&w, 5, 0) == NULL, "queuing did not create a phantom column for it");
    check(worldGet(&w, 6, 10, 6) == BLOCK_AIR, "the invalid entry was skipped, not applied");
}

static void test_chunk_diffs_unloaded_column_drains_on_load(void)
{
    puts("CHUNK_DIFFS queued for a column not yet loaded drains via networldOnColumnLoad(), "
         "exactly like BLOCK_EDIT/WORLD_SYNC");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    struct sync_entry e[2] = {
        { 40, 10, 5, BLOCK_STONE },   /* column (2,0) */
        { 35, 20, 3, BLOCK_SAND },    /* column (2,0), same column */
    };
    uint8_t buf[BS_CHUNK_DIFFS_BYTES(2)];
    size_t len = buildChunkDiffs(buf, 2, 0, BS_CHUNK_DIFFS_LAST, e, 2);
    networldApplyPayload(buf, len);
    check(networldPendingCount() == 2, "both queued for the not-yet-loaded column");

    worldColumnCreate(&w, 2, 0);
    networldOnColumnLoad(&w, 2, 0);

    check(worldGet(&w, 40, 10, 5) == BLOCK_STONE, "first diff landed");
    check(worldGet(&w, 35, 20, 3) == BLOCK_SAND, "second diff landed");
    check(networldPendingCount() == 0, "store drained — the same blockdiff.h store, not a second one");
}

static void test_chunk_diffs_empty_last_batch_is_a_noop(void)
{
    puts("a CHUNK_DIFFS with zero entries and LAST set parses cleanly and applies nothing");

    /* This is bs_proto.h's "a column with no edits still gets one packet" case. Nothing in this
     * client tracks a per-column synced flag (see applyChunkDiffs()'s own comment in networld.c
     * for why), so the only thing to prove here is that the empty/LAST shape does not crash or
     * misparse — not that some synced state flips. */
    networldInit();

    uint8_t buf[BS_CHUNK_DIFFS_BYTES(0)];
    size_t len = buildChunkDiffs(buf, 0, 0, BS_CHUNK_DIFFS_LAST, NULL, 0);
    networldApplyPayload(buf, len);

    check(networldPendingCount() == 0, "nothing queued for an empty batch");
    check(networldRecvMsgs() == 1, "the packet still counted as received");
}

static void test_chunk_diffs_short_header_dropped(void)
{
    puts("a CHUNK_DIFFS shorter than its own header is dropped, not misparsed");

    networldInit();

    uint8_t short_msg[BS_CHUNK_DIFFS_HDR_BYTES - 1] = {0};
    short_msg[0] = BS_APP_CHUNK_DIFFS;
    networldApplyPayload(short_msg, sizeof short_msg);

    check(networldPendingCount() == 0, "nothing queued from a truncated header");
}

static void test_chunk_diffs_malformed_length_dropped_whole(void)
{
    puts("a CHUNK_DIFFS whose declared count disagrees with its own length is dropped whole");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);

    struct sync_entry e[2] = {
        { 1, 10, 1, BLOCK_STONE },
        { 2, 10, 2, BLOCK_STONE },
    };
    uint8_t buf[BS_CHUNK_DIFFS_BYTES(2)];
    buildChunkDiffs(buf, 0, 0, BS_CHUNK_DIFFS_LAST, e, 2);
    bs_put_u16(buf + 10, 5);   /* claims 5 entries, packet only carries room/bytes for 2 */
    networldApplyPayload(buf, sizeof buf);

    check(networldPendingCount() == 0, "nothing was queued from the malformed batch");
    check(worldGet(&w, 1, 10, 1) == BLOCK_AIR, "and nothing applied either — dropped whole");
}

static void test_chunk_diffs_count_over_cap_dropped(void)
{
    puts("a CHUNK_DIFFS claiming more than BS_CHUNK_DIFFS_MAX_ENTRIES is rejected outright");

    networldInit();

    uint8_t over[BS_CHUNK_DIFFS_HDR_BYTES];
    over[0] = BS_APP_CHUNK_DIFFS;
    bs_put_i32(over + 1, 0);
    bs_put_i32(over + 5, 0);
    over[9] = 0;
    bs_put_u16(over + 10, (uint16_t)(BS_CHUNK_DIFFS_MAX_ENTRIES + 1));
    networldApplyPayload(over, sizeof over);

    check(networldPendingCount() == 0,
          "a CHUNK_DIFFS claiming more than BS_CHUNK_DIFFS_MAX_ENTRIES is dropped");
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

static void test_edits_before_a_world_exists_are_kept(void)
{
    puts("an edit arriving before any World is registered is queued, not dropped");

    /* This is the join case, and it used to be the bug. The server sends BS_APP_WORLD_SYNC —
     * every edit every other player has made — immediately after JOIN, and JOIN completes on
     * the title screen, tens of seconds before the player has picked a world for
     * networldSetWorld() to register. Dropping here put the joining player in untouched
     * terrain instead of the world everyone else had been building in. */
    networldInit();
    networldSetWorld(NULL);

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 5, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);   /* must not crash */
    check(networldPendingCount() == 1, "the edit is held for a World that does not exist yet");

    World w;
    worldInit(&w);
    /* Still a no-op: the World this is called on is not the registered one (it is still NULL),
     * which is the same guard that keeps the worker thread's staging World out. */
    networldOnColumnLoad(&w, 0, 0);
    check(networldPendingCount() == 1, "a foreign World cannot drain it either");
}

static void test_setworld_keeps_diffs_until_terrain_exists(void)
{
    puts("a diff that arrives before the World survives the terrain being generated over it");

    /* The join case, end to end, in the order main.c actually runs it. This replaced a test
     * that asserted the opposite — that networldSetWorld() flushes the store into whatever
     * columns already exist — on the strength of a comment claiming the starting ring is built
     * by then. It is not: worldReportBuild() allocates a 17x17 grid of *empty* columns as a
     * memory-budget proof, and genStart() only afterwards starts the worker that fills them.
     * Flushing there put the whole batch into air that generated terrain then overwrote, which
     * on the emulator read as `net y8 a0 q0` with the dug trench visibly gone after a rejoin. */
    networldInit();
    networldSetWorld(NULL);

    uint8_t early[BS_BLOCK_EDIT_BYTES], later[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(early, 5, 10, 5, BLOCK_AIR);     /* column (0,0), in the report grid */
    buildBlockEdit(later, 40, 12, 5, BLOCK_SAND);   /* column (2,0), streams in later */
    networldApplyPayload(early, sizeof early);
    networldApplyPayload(later, sizeof later);
    check(networldPendingCount() == 2, "both held while there is no World");

    World w;
    worldInit(&w);
    worldColumnCreate(&w, 0, 0);   /* exactly what worldReportBuild() leaves behind: empty */
    networldSetWorld(&w);

    check(networldPendingCount() == 2,
          "an allocated but ungenerated column does not consume the diff aimed at it");

    /* Generation, as workerInstall() does it: real blocks written straight over that column,
     * with no idea a diff was ever aimed at it. A flush at registration time would already have
     * been erased by this line. */
    worldSet(&w, 5, 10, 5, BLOCK_STONE);
    networldOnColumnLoad(&w, 0, 0);

    check(worldGet(&w, 5, 10, 5) == BLOCK_AIR,
          "the diff lands on top of the generated terrain, not under it");
    check(networldPendingCount() == 1, "the diff for a column that is not built yet stays queued");

    /* And that survivor still drains the ordinary way once its column streams in. */
    worldColumnCreate(&w, 2, 0);
    networldOnColumnLoad(&w, 2, 0);
    check(worldGet(&w, 40, 12, 5) == BLOCK_SAND, "the still-queued diff lands on its column load");
    check(networldPendingCount() == 0, "store is empty once both have landed");
}

static void test_world_info_carries_the_servers_seed(void)
{
    puts("BS_APP_WORLD_INFO hands the server's world seed to the client");

    networldInit();

    uint32_t seed = 0xDEADBEEFu;   /* poisoned: a false return must leave it untouched */
    check(!networldWorldSeed(&seed), "no seed is claimed before the server sends one");
    check(seed == 0xDEADBEEFu, "a false return does not write the out-parameter");

    uint8_t info[BS_WORLD_INFO_BYTES];
    info[0] = BS_APP_WORLD_INFO;
    bs_put_u32(info + 1, 1495017430u);
    networldApplyPayload(info, sizeof info);

    check(networldWorldSeed(&seed), "the seed is available once WORLD_INFO arrives");
    check(seed == 1495017430u, "and it is the exact value the server sent");

    /* 0 is a seed a server can legitimately own, which is the whole reason the value comes
     * back through an out-parameter instead of a sentinel return. */
    networldInit();
    bs_put_u32(info + 1, 0u);
    networldApplyPayload(info, sizeof info);
    seed = 0xDEADBEEFu;
    check(networldWorldSeed(&seed) && seed == 0u, "a seed of 0 is a real seed, not 'no seed'");

    /* Wrong length is malformed, not a seed of whatever happened to be in the first bytes —
     * same drop-whole posture as every other decoder in this module. */
    networldInit();
    uint8_t truncated[BS_WORLD_INFO_BYTES - 1];
    truncated[0] = BS_APP_WORLD_INFO;
    memset(truncated + 1, 0xFF, sizeof truncated - 1);
    networldApplyPayload(truncated, sizeof truncated);
    check(!networldWorldSeed(NULL), "a short WORLD_INFO is dropped, not misparsed");
}

/* ---------------------------------------------------- the remesh notification ------------- */
/* Written after the bug it catches was reported from a live two-player session: a remote
 * player's edits changed the world but nothing on screen changed. Collision reads world
 * blocks directly, so the reporting player fell into a trench his friend had dug while still
 * looking at solid ground. Every check below is about the hook that closes that gap — this
 * module writes the block, and something outside it has to be told which block, or the chunk
 * mesh describing that block is never rebuilt. */

#define HOOK_LOG_MAX 16

static struct { int x, y, z; } hook_log[HOOK_LOG_MAX];
static int  hook_count;
static int  hook_userdata_ok;
static int  hook_userdata_marker;

static void hookRecord(void *userdata, int x, int y, int z)
{
    if (userdata == &hook_userdata_marker) hook_userdata_ok++;
    if (hook_count < HOOK_LOG_MAX) {
        hook_log[hook_count].x = x;
        hook_log[hook_count].y = y;
        hook_log[hook_count].z = z;
    }
    hook_count++;
}

static void hookReset(void)
{
    memset(hook_log, 0, sizeof hook_log);
    hook_count = 0;
    hook_userdata_ok = 0;
}

static void test_edit_hook_fires_for_an_applied_edit(void)
{
    puts("an applied remote edit notifies the hook with its own coordinate");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    hookReset();
    networldSetEditHook(hookRecord, &hook_userdata_marker);

    worldColumnCreate(&w, 0, 0);   /* column (0,0): x,z in 0..15 */

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 5, 10, 7, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);

    check(worldGet(&w, 5, 10, 7) == BLOCK_STONE, "the block landed in the world");
    check(hook_count == 1, "the hook fired exactly once");
    check(hook_log[0].x == 5 && hook_log[0].y == 10 && hook_log[0].z == 7,
          "it was handed the edited block's own coordinate, not the chunk's");
    check(hook_userdata_ok == 1, "userdata was passed through unchanged");

    networldSetEditHook(NULL, NULL);
}

static void test_edit_hook_silent_while_the_edit_is_only_queued(void)
{
    puts("a queued remote edit does not notify until it actually reaches the world");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    hookReset();
    networldSetEditHook(hookRecord, &hook_userdata_marker);

    /* Column (2,0) never created: the edit is parked in net/blockdiff.h. Nothing has changed
     * in the world, so notifying here would dirty a chunk for no reason — and worse, would
     * teach a caller that a notification means the block is readable, which it is not yet. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 40, 10, 5, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);

    check(networldPendingCount() == 1, "the edit is queued, as before");
    check(hook_count == 0, "no notification for a block that is not in the world yet");

    /* ...and it does fire when the column arrives and the diff is finally written. */
    worldColumnCreate(&w, 2, 0);
    networldOnColumnLoad(&w, 2, 0);

    check(worldGet(&w, 40, 10, 5) == BLOCK_STONE, "the drained diff landed");
    check(hook_count == 1, "the drain notified once, at the moment the block became real");
    check(hook_log[0].x == 40 && hook_log[0].y == 10 && hook_log[0].z == 5,
          "the drained diff's coordinate came through intact");

    networldSetEditHook(NULL, NULL);
}

static void test_edit_hook_fires_per_entry_of_a_sync_batch(void)
{
    puts("a WORLD_SYNC batch notifies once per entry it applies");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    hookReset();
    networldSetEditHook(hookRecord, &hook_userdata_marker);

    worldColumnCreate(&w, 0, 0);

    /* Three entries, all inside the one loaded column, so all three apply immediately. */
    const int count = 3;
    uint8_t msg[BS_APP_HDR_BYTES + 2 + 3 * BS_SYNC_ENTRY_BYTES];
    msg[0] = BS_APP_WORLD_SYNC;
    bs_put_u16(msg + 1, (uint16_t)count);
    uint8_t *p = msg + BS_APP_HDR_BYTES + 2;
    for (int i = 0; i < count; i++) {
        bs_put_i32(p,     1 + i);
        bs_put_i32(p + 4, 30);
        bs_put_i32(p + 8, 2);
        p[12] = BLOCK_STONE;
        p += BS_SYNC_ENTRY_BYTES;
    }
    networldApplyPayload(msg, sizeof msg);

    check(hook_count == 3, "one notification per applied entry, not one per packet");
    check(hook_log[2].x == 3 && hook_log[2].y == 30 && hook_log[2].z == 2,
          "the last entry's coordinate came through");

    networldSetEditHook(NULL, NULL);
}

static void test_edit_hook_is_cleared_by_init_and_optional(void)
{
    puts("the hook is optional, and networldInit() clears it");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);
    hookReset();

    /* No hook registered at all: applying an edit must not crash on a NULL function pointer.
     * This is the single-player path — main.c has no reason to register one there. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 1, 12, 1, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 1, 12, 1) == BLOCK_STONE, "the edit still applies with no hook set");
    check(hook_count == 0, "and nothing was notified");

    /* Registered, then cleared by a re-init: leaving a session and joining another must not
     * leave a hook pointing at the previous session's state. */
    networldSetEditHook(hookRecord, &hook_userdata_marker);
    networldInit();
    networldSetWorld(&w);
    buildBlockEdit(msg, 2, 12, 1, BLOCK_STONE);
    networldApplyPayload(msg, sizeof msg);
    check(hook_count == 0, "networldInit() cleared the hook");
}

/* ---------------------------------------------------------- inventory sync (BS_APP_INV_*) - */
/* BS_APP_INV_STATE / BS_APP_INV_ACTION, proto/bs_proto.h's newest pair. See that header's long
 * comment on why the server always speaks first: an INV_ACTION networldSendInvAction() lets out
 * before any INV_STATE has arrived would be exactly the CHUNK_SUB mistake v1.2.7 made, kicked
 * outright by any server that predates this feature. The checks below cover the decode side
 * (applyInvState(), networld.c) and the capability probe itself, the same two halves the CHUNK_
 * SUB/CHUNK_DIFFS pair above gets. */

static NetworldInvState inv_hook_state;
static int              inv_hook_count;
static int              inv_hook_userdata_ok;
static int              inv_hook_userdata_marker;

static void invHookRecord(void *userdata, const NetworldInvState *state)
{
    if (userdata == &inv_hook_userdata_marker) inv_hook_userdata_ok++;
    inv_hook_state = *state;
    inv_hook_count++;
}

static void invHookReset(void)
{
    memset(&inv_hook_state, 0, sizeof inv_hook_state);
    inv_hook_count        = 0;
    inv_hook_userdata_ok  = 0;
}

static void test_inv_state_valid_fires_hook_with_exact_values(void)
{
    puts("a valid INV_STATE fires the hook with the exact values encoded");

    networldInit();
    invHookReset();
    networldSetInvHook(invHookRecord, &inv_hook_userdata_marker);

    uint8_t items[BS_INV_SLOT_COUNT], counts[BS_INV_SLOT_COUNT];
    fillValidInv(items, counts);
    uint8_t msg[BS_INV_STATE_BYTES];
    buildInvState(msg, 3, items, counts);
    networldApplyPayload(msg, sizeof msg);

    check(inv_hook_count == 1, "the hook fired exactly once");
    check(inv_hook_userdata_ok == 1, "userdata was passed through unchanged");
    check(inv_hook_state.selected_hotbar == 3, "selected_hotbar round-trips");

    bool all_slots_ok = true;
    for (uint32_t i = 0; i < BS_INV_SLOT_COUNT; i++) {
        if (inv_hook_state.slots[i].item != items[i] || inv_hook_state.slots[i].count != counts[i])
            all_slots_ok = false;
    }
    check(all_slots_ok, "every one of the 24 (item, count) pairs round-trips exactly");

    networldSetInvHook(NULL, NULL);
}

static void test_inv_state_wrong_length_dropped(void)
{
    puts("an INV_STATE one byte short or one byte long is dropped, and the hook does not fire");

    networldInit();
    invHookReset();
    networldSetInvHook(invHookRecord, &inv_hook_userdata_marker);

    uint8_t short_msg[BS_INV_STATE_BYTES - 1] = {0};
    short_msg[0] = BS_APP_INV_STATE;
    networldApplyPayload(short_msg, sizeof short_msg);
    check(inv_hook_count == 0, "one byte short: the hook did not fire");

    uint8_t long_msg[BS_INV_STATE_BYTES + 1] = {0};
    long_msg[0] = BS_APP_INV_STATE;
    networldApplyPayload(long_msg, sizeof long_msg);
    check(inv_hook_count == 0, "one byte long: the hook did not fire either");

    networldSetInvHook(NULL, NULL);
}

static void test_inv_state_malformed_slot_drops_whole_packet(void)
{
    puts("a malformed slot (count 0 / item non-zero, or the reverse) drops the whole packet");

    networldInit();
    invHookReset();
    networldSetInvHook(invHookRecord, &inv_hook_userdata_marker);

    uint8_t items[BS_INV_SLOT_COUNT], counts[BS_INV_SLOT_COUNT];
    uint8_t msg[BS_INV_STATE_BYTES];

    fillValidInv(items, counts);
    items[5] = 7; counts[5] = 0;   /* item non-zero, count zero: malformed */
    buildInvState(msg, 0, items, counts);
    networldApplyPayload(msg, sizeof msg);
    check(inv_hook_count == 0, "item non-zero with count zero drops the whole packet, not just that slot");

    fillValidInv(items, counts);
    items[5] = 0; counts[5] = 7;   /* item zero, count non-zero: malformed the other way */
    buildInvState(msg, 0, items, counts);
    networldApplyPayload(msg, sizeof msg);
    check(inv_hook_count == 0, "item zero with count non-zero drops the whole packet too");

    networldSetInvHook(NULL, NULL);
}

static void test_inv_state_count_over_cap_dropped(void)
{
    puts("a slot claiming count > BS_INV_STACK_MAX drops the whole packet");

    networldInit();
    invHookReset();
    networldSetInvHook(invHookRecord, &inv_hook_userdata_marker);

    uint8_t items[BS_INV_SLOT_COUNT], counts[BS_INV_SLOT_COUNT];
    fillValidInv(items, counts);
    counts[3] = (uint8_t)(BS_INV_STACK_MAX + 1);   /* items[3] is already non-zero */
    uint8_t msg[BS_INV_STATE_BYTES];
    buildInvState(msg, 0, items, counts);
    networldApplyPayload(msg, sizeof msg);

    check(inv_hook_count == 0, "count over BS_INV_STACK_MAX is dropped, not clamped or partially applied");

    networldSetInvHook(NULL, NULL);
}

static void test_send_inv_action_gated_on_inv_state(void)
{
    puts("networldSendInvAction() sends nothing before any INV_STATE has arrived, and exactly "
         "BS_INV_ACTION_BYTES with the right bytes once one has");

    networldInit();
    fakeTransportReset();

    check(networldSendInvAction(BS_INV_OP_MOVE, 1, 2, 3) == false,
          "no INV_STATE received yet: the send is refused");
    check(fake_sent_calls == 0,
          "the transport was never even touched — this is the capability probe itself, "
          "not a defensive nicety in front of it");

    uint8_t items[BS_INV_SLOT_COUNT], counts[BS_INV_SLOT_COUNT];
    fillValidInv(items, counts);
    uint8_t msg[BS_INV_STATE_BYTES];
    buildInvState(msg, 0, items, counts);
    networldApplyPayload(msg, sizeof msg);

    check(networldSendInvAction(BS_INV_OP_SWAP, 4, 5, 0) == true,
          "an INV_STATE has now arrived, so the send goes through");
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_INV_ACTION_BYTES, "encoded to exactly BS_INV_ACTION_BYTES");
    check(fake_sent_buf[0] == BS_APP_INV_ACTION, "type byte is BS_APP_INV_ACTION");
    check(fake_sent_buf[1] == BS_INV_OP_SWAP, "op round-trips");
    check(fake_sent_buf[2] == 4, "a round-trips");
    check(fake_sent_buf[3] == 5, "b round-trips");
    check(fake_sent_buf[4] == 0, "c round-trips, including zero");

    /* A malformed INV_STATE must not have armed the probe either. */
    networldInit();
    fakeTransportReset();
    uint8_t bad[BS_INV_STATE_BYTES - 1] = {0};
    bad[0] = BS_APP_INV_STATE;
    networldApplyPayload(bad, sizeof bad);
    check(networldSendInvAction(BS_INV_OP_SELECT, 0, 0, 0) == false,
          "a dropped, malformed INV_STATE does not arm the capability probe");
    check(fake_sent_calls == 0, "and the transport was never touched for it");
}

static void test_inv_state_flag_does_not_survive_fresh_session(void)
{
    puts("the \"server has spoken\" flag does not survive into a fresh session");

    networldInit();
    fakeTransportReset();

    uint8_t items[BS_INV_SLOT_COUNT], counts[BS_INV_SLOT_COUNT];
    fillValidInv(items, counts);
    uint8_t msg[BS_INV_STATE_BYTES];
    buildInvState(msg, 0, items, counts);
    networldApplyPayload(msg, sizeof msg);

    check(networldSendInvAction(BS_INV_OP_SELECT, 1, 0, 0) == true,
          "the send works within this session, once INV_STATE has arrived");

    /* Leaving and rejoining a server — bsnet.c's netDisconnect() calls exactly this. */
    networldInit();
    fakeTransportReset();

    check(networldSendInvAction(BS_INV_OP_SELECT, 1, 0, 0) == false,
          "a fresh session has heard nothing from a server yet, even though the previous one did");
    check(fake_sent_calls == 0, "and never touched the transport for it");
}

/* ------------------------------------------- the rejoin sync capacity --------------------- */
/* Written from a live report: steve and a friend built a house, steve rejoined, and the house
 * was gone from HIS screen while still standing on everyone else's. This reproduces that
 * exactly, with no server and no second console.
 *
 * The shape of it: the server replays every diff it holds (BS_DIFF_MAX is 65536, so it is not
 * the one forgetting) the instant the handshake completes — which is on the title screen,
 * before a world exists. With no world, every entry goes to the pending store, which REFUSES
 * rather than evicts once full. BLOCKDIFF_MAX_PENDING was 256, and the server sends
 * oldest-first, so the first 256 diffs survived and the newest were dropped. A house is the
 * newest thing in the world. Players who never left still have theirs in RAM, which is why
 * it stays visible to them — the asymmetry is the tell.
 *
 * The fix raised BLOCKDIFF_MAX_PENDING to match the server's 65536 exactly, so a join sync
 * can no longer overflow it. The count below stays at 300 — over the OLD cap, so this test
 * still goes red against the code that had the bug, which is the only reason to keep it.
 * Measured against that code: "queued 256 of 300, refused 44", 3 checks failed. */

#define REJOIN_EDITS 300

static void test_rejoin_sync_larger_than_the_store_keeps_every_edit(void)
{
    puts("a join sync bigger than the pending store must not silently lose the newest edits");

    World w;
    worldInit(&w);
    networldInit();

    /* No world registered: this is the title screen, which is where a join sync actually
     * arrives. networldSetWorld() is deliberately NOT called yet — explicitly reset to NULL
     * rather than just relying on networldInit() above, which does not touch s_world at all
     * (see networld.c). Without this, s_world can still hold whatever a PRECEDING test last
     * registered via networldSetWorld(&some_local_World) and never cleared, and that local has
     * since gone out of scope: found live via AddressSanitizer as a stack-use-after-return into
     * test_edit_hook_is_cleared_by_init_and_optional()'s `w`, the test that happens to run
     * immediately before this one. Not a bug this feature introduced, but this test's own stated
     * precondition was never actually enforced, so it is enforced here explicitly. */
    networldSetWorld(NULL);

    /* REJOIN_EDITS distinct coordinates, all inside column (0,0) so one drain collects them.
     * Oldest first, exactly as send_world_sync() walks the server's diff store. y varies so
     * every coordinate is genuinely distinct rather than overwriting in place. */
    for (int i = 0; i < REJOIN_EDITS; i++) {
        uint8_t msg[BS_BLOCK_EDIT_BYTES];
        buildBlockEdit(msg, i % 16, 40 + i / 16, (i / 4) % 16, BLOCK_STONE);
        networldApplyPayload(msg, sizeof msg);
    }

    printf("    [measured] queued %d of %d, refused %d\n",
           networldPendingCount(), REJOIN_EDITS, networldPendingRefusals());

    check(networldPendingRefusals() == 0,
          "no edit from the join sync was refused for want of room");
    check(networldPendingCount() == REJOIN_EDITS,
          "every edit the server replayed is still held, waiting for its column");

    /* Now the player picks a world and the column streams in — the house should reappear. */
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);
    networldOnColumnLoad(&w, 0, 0);

    int landed = 0;
    for (int i = 0; i < REJOIN_EDITS; i++)
        if (worldGet(&w, i % 16, 40 + i / 16, (i / 4) % 16) == BLOCK_STONE) landed++;

    printf("    [measured] %d of %d edits reached the world\n", landed, REJOIN_EDITS);
    check(landed == REJOIN_EDITS, "the whole house is standing after the rejoin");
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
    test_subscribe_column_encodes_and_sends();
    test_unsubscribe_column_encodes_and_sends();
    test_chunk_diffs_batch();
    test_chunk_diffs_unloaded_column_drains_on_load();
    test_chunk_diffs_empty_last_batch_is_a_noop();
    test_chunk_diffs_short_header_dropped();
    test_chunk_diffs_malformed_length_dropped_whole();
    test_chunk_diffs_count_over_cap_dropped();
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
    test_edits_before_a_world_exists_are_kept();
    test_setworld_keeps_diffs_until_terrain_exists();
    test_world_info_carries_the_servers_seed();
    test_edit_hook_fires_for_an_applied_edit();
    test_edit_hook_silent_while_the_edit_is_only_queued();
    test_edit_hook_fires_per_entry_of_a_sync_batch();
    test_edit_hook_is_cleared_by_init_and_optional();
    test_inv_state_valid_fires_hook_with_exact_values();
    test_inv_state_wrong_length_dropped();
    test_inv_state_malformed_slot_drops_whole_packet();
    test_inv_state_count_over_cap_dropped();
    test_send_inv_action_gated_on_inv_state();
    test_inv_state_flag_does_not_survive_fresh_session();
    test_rejoin_sync_larger_than_the_store_keeps_every_edit();

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
