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

#include "app/session.h"
#include "net/blockdiff.h"
#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/atlas_uv.h"
#include "world/block.h"
#include "world/mesher.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/world.h"

#include <stdlib.h>

#include "proto/bs_proto.h"

/* game/players.h is the server-side home of BS_GAME_MAX_PLAYERS, the room cap that
 * networld.h:355 claims NETWORLD_MAX_REMOTE mirrors "minus the local player". It is pulled in
 * HERE, in the test, rather than in net/networld.c alongside the two mirror asserts already at
 * networld.c:33-36, because BS_GAME_MAX_PLAYERS is not reachable from that translation unit:
 * networld.c includes only proto/bs_proto.h, and bs_proto.h does not define it. Measured with
 * `gcc -E -dM` over networld.c's TU on 2026-08-25 — 0 hits for BS_GAME_MAX_PLAYERS, 2 for
 * BS_INV_SLOT_COUNT, which is exactly why those two asserts can live in networld.c and this one
 * cannot without dragging a server header into production client code.
 *
 * The gap that leaves, stated plainly rather than buried: this guard fires when the HOST test
 * build compiles (every tools/run_host_tests.sh run), not in the 3DS console build. Console
 * builds never compile this file (see the __3DS__ guard in this file's header comment). */
#include "game/players.h"

/* The mirror guard networld.h:355 always implied and never had, in the idiom of the two at
 * net/networld.c:33-36. Its absence was measurable: shrinking NETWORLD_MAX_REMOTE from 15 to 7
 * built clean and this suite printed "PASS 318 checks, 0 failed" against a baseline of 326 —
 * green, with eight checks silently deleted rather than failed, because the fill loop below was
 * bounded by the very constant its expectation was compared against. A client whose table is
 * smaller than the server's room cap silently drops players in a full room. */
_Static_assert(NETWORLD_MAX_REMOTE == BS_GAME_MAX_PLAYERS - 1,
               "networld.h's NETWORLD_MAX_REMOTE must be game/players.h's BS_GAME_MAX_PLAYERS "
               "minus the local player");

/* And the naked-literal pin, because the assert above is satisfied by both constants moving
 * TOGETHER — a room-cap change would otherwise take the client's table with it unnoticed. This
 * is the line that must be changed consciously, and only after deciding that 16-player rooms
 * really have become something else. Never edit it to silence a build. */
_Static_assert(NETWORLD_MAX_REMOTE == 15,
               "NETWORLD_MAX_REMOTE is 15 (16 players minus the local one)");

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

static void buildPlayerState(uint8_t *out, uint8_t flags,
                             float x, float y, float z, float yaw, float pitch,
                             const uint8_t *armor /* 8 bytes: 4 slots x {item,count} */,
                             uint32_t xp_level, float xp_progress, float health, float hunger)
{
    out[0] = BS_APP_PLAYER_STATE;
    out[1] = flags;
    bs_put_f32(out + 2,  x);
    bs_put_f32(out + 6,  y);
    bs_put_f32(out + 10, z);
    bs_put_f32(out + 14, yaw);
    bs_put_f32(out + 18, pitch);
    uint8_t *p = out + 22;
    for (uint32_t i = 0; i < BS_ARMOR_SLOTS * 2u; i++)
        p[i] = armor ? armor[i] : 0;
    bs_put_u32(out + 30, xp_level);
    bs_put_f32(out + 34, xp_progress);
    bs_put_f32(out + 38, health);
    bs_put_f32(out + 42, hunger);
}

/* Bit-exact float compare: the point of several checks below is that a value came off the wire
 * and back verbatim — including NaN, whose every ordinary comparison is false. */
static bool f32bits_eq(float a, float b)
{
    uint32_t ua, ub;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    return ua == ub;
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

    /* v1.6.0: the legal id space is the registry's, not BLOCK_COUNT's. A dyn id
     * is accepted even when this table does not define it yet — it stores raw
     * and reads back as air through blockInfo()'s contract until DEFS arrive. */
    buildBlockEdit(msg, 5, 10, 5, REG_ID_DYN_LO);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == REG_ID_DYN_LO,
          "an undefined-but-legal dyn id is stored raw, not rejected");

    buildBlockEdit(msg, 5, 10, 5, (uint8_t)REG_ID_DYN_HI + 1u /* 0xFE, reserved */);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == REG_ID_DYN_LO,
          "block id 0xFE (reserved) rejected");

    buildBlockEdit(msg, 5, 10, 5, (uint8_t)0xFF);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 5) == REG_ID_DYN_LO, "block id 0xFF rejected");

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
        { 6, 10, 6, 0xFF },           /* invalid block id: should be skipped            */
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
        { 6, 10, 6, 0xFF },                 /* invalid block id: should be skipped         */
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

    /* Every bound and every expectation below is the naked literal 15, never
     * NETWORLD_MAX_REMOTE. That is deliberate, and it is the whole shape of this function:
     * before 2026-08-25 the fill loop was bounded by NETWORLD_MAX_REMOTE *and* the resulting
     * count was asserted against NETWORLD_MAX_REMOTE, so both sides moved together and neither
     * could ever disagree. A check parameterised by the value under test cannot detect that
     * value changing.
     *
     * Bounding the loops by the literal too — not just the comparisons — is the part that
     * matters most: it keeps the NUMBER of checks this function emits (19) independent of the
     * constant, so drift arrives as named FAILURES rather than as a shorter, still-green run.
     * The two _Static_asserts at the top of this file are the primary, compile-time defence;
     * these are the runtime one that survives if somebody ever deletes those. */
    networldInit();

    check(NETWORLD_MAX_REMOTE == 15, "NETWORLD_MAX_REMOTE is still 15");

    uint8_t msg[BS_POS_UPDATE_S_BYTES];
    for (uint32_t sid = 1; sid <= 15; sid++) {
        buildPosUpdateS(msg, sid, 0, 0, 0, 0, 0);
        networldApplyPayload(msg, sizeof msg);
    }
    check(networldRemoteCount() == 15, "table filled to capacity, all 15 slots taken");

    buildPosUpdateS(msg, 999, 1, 1, 1, 1, 1);
    networldApplyPayload(msg, sizeof msg);
    check(networldRemoteCount() == 15, "the 16th sid was dropped, not evicted");

    bool found_extra = false;
    for (int i = 0; i < 15; i++) {
        /* networldRemoteGet() leaves *out untouched on an out-of-range index (see
         * networld.h), so r must be initialised before the read of r.sid below. */
        NetworldRemote r;
        memset(&r, 0, sizeof r);
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

/* ------------------------------------------------ saved player state (BS_APP_PLAYER_*) --- */
/* BS_APP_PLAYER_STATE / BS_APP_PLAYER_REPORT, bs_proto.h's v1.5.0 pair — the third
 * server-speaks-first set after WORLD_INFO and INV_STATE. The decode side mirrors
 * applyInvState() one for one: length-exact, capability armed only once the whole packet has
 * validated, fresh-spawn (flags 0) still counts as the server having spoken. The float policy
 * deliberately mirrors applyPosUpdate(): wire floats verbatim, no NaN/huge sanitisation, and
 * test_player_state_float_policy_verbatim() holds that choice in place. */

static void test_player_state_pose_and_meters_round_trip(void)
{
    puts("a valid PLAYER_STATE with both flags makes pose and meters available, values exact");

    networldInit();

    const uint8_t armor[BS_ARMOR_SLOTS * 2] = { 7, 1, 8, 1, 9, 1, 10, 3 };
    uint8_t msg[BS_PLAYER_STATE_BYTES];
    buildPlayerState(msg, BS_PLAYER_STATE_FLAG_POSE | BS_PLAYER_STATE_FLAG_EXT,
                     12.5f, 33.0f, -7.25f, 1.5f, -0.5f, armor, 42, 0.75f, 17.5f, 9.5f);
    networldApplyPayload(msg, sizeof msg);

    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    check(networldSavedPose(&x, &y, &z, &yaw, &pitch), "the pose is available");
    check(f32bits_eq(x, 12.5f) && f32bits_eq(y, 33.0f) && f32bits_eq(z, -7.25f),
          "position round-trips bit-exactly");
    check(f32bits_eq(yaw, 1.5f) && f32bits_eq(pitch, -0.5f), "facing round-trips bit-exactly");

    check(networldPlayerMetersValid(), "the meters are flagged meaningful");
    const NetworldPlayerMeters *m = networldPlayerMeters();
    check(m != NULL, "and reachable through the accessor");
    check(memcmp(m->armor, armor, sizeof armor) == 0,
          "all four armour slots round-trip exactly");
    check(m->xp_level == 42, "xp level round-trips");
    check(f32bits_eq(m->xp_progress, 0.75f), "xp progress round-trips");
    check(f32bits_eq(m->health, 17.5f) && f32bits_eq(m->hunger, 9.5f),
          "health and hunger round-trip");
}

static void test_player_state_flag_combinations(void)
{
    puts("flag combinations: fresh spawn is distinct from pose-only and ext-only");

    networldInit();

    /* flags == 0: bs_proto.h's fresh-spawn marker. The server spoke — which is what the
     * capability probe asks — but claims nothing meaningful in this packet. */
    uint8_t msg[BS_PLAYER_STATE_BYTES] = {0};
    msg[0] = BS_APP_PLAYER_STATE;
    networldApplyPayload(msg, sizeof msg);
    check(!networldSavedPose(NULL, NULL, NULL, NULL, NULL),
          "flags 0 claims no pose (and NULL out-parameters are legal)");
    check(!networldPlayerMetersValid() && networldPlayerMeters() == NULL,
          "flags 0 claims no meters");

    /* POSE only. */
    networldInit();
    buildPlayerState(msg, BS_PLAYER_STATE_FLAG_POSE, 1, 2, 3, 4, 5, NULL, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);
    float x, y, z, yaw, pitch;
    check(networldSavedPose(&x, &y, &z, &yaw, &pitch), "pose-only: the pose is available");
    check(x == 1 && y == 2 && z == 3 && yaw == 4 && pitch == 5, "pose-only: values exact");
    check(!networldPlayerMetersValid(), "pose-only: no meters are claimed");
    check(networldPlayerMeters() == NULL, "pose-only: the accessor stays NULL");

    /* EXT only. */
    networldInit();
    buildPlayerState(msg, BS_PLAYER_STATE_FLAG_EXT, 0, 0, 0, 0, 0, NULL, 3, 0.5f, 20.0f, 20.0f);
    networldApplyPayload(msg, sizeof msg);
    check(!networldSavedPose(&x, &y, &z, &yaw, &pitch), "ext-only: no pose is claimed");
    check(networldPlayerMetersValid() && networldPlayerMeters()->xp_level == 3,
          "ext-only: the meters are available");
}

static void test_player_state_wrong_length_dropped(void)
{
    puts("a wrong-length PLAYER_STATE (45 and 47 bytes) is dropped whole");

    networldInit();
    fakeTransportReset();

    NetworldPlayerMeters m;
    memset(&m, 0, sizeof m);

    uint8_t short_msg[BS_PLAYER_STATE_BYTES - 1] = {0};
    short_msg[0] = BS_APP_PLAYER_STATE;
    networldApplyPayload(short_msg, sizeof short_msg);

    uint8_t long_msg[BS_PLAYER_STATE_BYTES + 1] = {0};
    long_msg[0] = BS_APP_PLAYER_STATE;
    networldApplyPayload(long_msg, sizeof long_msg);

    float x, y, z, yaw, pitch;
    check(!networldSavedPose(&x, &y, &z, &yaw, &pitch), "neither wrong length applied a pose");
    check(!networldPlayerMetersValid(), "nor meters");
    check(networldSendPlayerReport(&m) == false,
          "a malformed PLAYER_STATE does not arm the capability probe");
}

static void test_player_state_float_policy_verbatim(void)
{
    puts("NaN and huge pose floats are kept verbatim — applyPosUpdate's posture, not a new one");

    /* Policy mirror, asserted so any future change to it shows up here: inbound POS_UPDATE
     * (applyPosUpdate, networld.c) does not range- or finiteness-check untrusted floats,
     * because the Noise XX transport has already authenticated the peer — and neither does
     * this decoder. The one consumer difference (main.c feeds these to playerInit() rather
     * than a remote-render table) lives at the application point, before frame one; if that
     * ever proves insufficient it changes BOTH decoders, not one of them quietly. */
    networldInit();

    const uint32_t nan_bits = 0x7FC00000u;
    float nan_f;
    memcpy(&nan_f, &nan_bits, 4);
    const float huge = 1e30f;

    uint8_t msg[BS_PLAYER_STATE_BYTES];
    buildPlayerState(msg, BS_PLAYER_STATE_FLAG_POSE, nan_f, huge, -huge, nan_f, huge,
                     NULL, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);

    float x, y, z, yaw, pitch;
    check(networldSavedPose(&x, &y, &z, &yaw, &pitch),
          "the packet is accepted — hostile-looking floats are not a malformed packet here");
    check(f32bits_eq(x, nan_f), "NaN in, NaN out, bit for bit");
    check(f32bits_eq(y, huge) && f32bits_eq(z, -huge) && f32bits_eq(pitch, huge),
          "huge-but-finite magnitudes survive untouched");
}

static void test_player_state_capability_transitions(void)
{
    puts("the PLAYER_STATE capability flag: valid arms it, init clears it");

    networldInit();
    fakeTransportReset();

    NetworldPlayerMeters m;
    memset(&m, 0, sizeof m);

    uint8_t msg[BS_PLAYER_STATE_BYTES];
    buildPlayerState(msg, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, 0);
    networldApplyPayload(msg, sizeof msg);

    check(networldSendPlayerReport(&m) == true,
          "even a flags-0 snapshot arms the probe — the server has spoken");
    check(fake_sent_calls == 1, "and the report went out (this build flips BS_CLIENT_HAS_METERS)");

    /* Leaving and rejoining — bsnet.c's netDisconnect() calls exactly this. */
    networldInit();
    fakeTransportReset();
    check(networldSendPlayerReport(&m) == false,
          "a fresh session has heard nothing from a server yet, even though the previous one did");
    check(fake_sent_calls == 0, "and never touched the transport for it");
}

static void test_send_player_report_encodes(void)
{
    puts("networldSendPlayerReport() encodes exactly 30 bytes with an all-zero reserved tail");

    networldInit();
    fakeTransportReset();

    const uint8_t armor[BS_ARMOR_SLOTS * 2] = { 7, 1, 0, 0, 0, 0, 10, 3 };
    NetworldPlayerMeters m;
    memcpy(m.armor, armor, sizeof armor);
    m.xp_level    = 12;
    m.xp_progress = 0.25f;
    m.health      = 13.5f;
    m.hunger      = 6.5f;

    /* Arm first — the gating itself is covered by the two tests above this one. */
    uint8_t state[BS_PLAYER_STATE_BYTES];
    buildPlayerState(state, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0, 0);
    networldApplyPayload(state, sizeof state);

    check(networldSendPlayerReport(&m), "send reports success");
    check(fake_sent_calls == 1, "the transport was asked to send exactly once");
    check(fake_sent_len == BS_PLAYER_REPORT_BYTES, "encoded to exactly BS_PLAYER_REPORT_BYTES");
    check(fake_sent_buf[0] == BS_APP_PLAYER_REPORT, "type byte is BS_APP_PLAYER_REPORT");
    check(memcmp(fake_sent_buf + 1, armor, sizeof armor) == 0, "armour block round-trips");
    check(bs_get_u32(fake_sent_buf + 9) == 12, "xp level round-trips");
    check(bs_get_f32(fake_sent_buf + 13) == 0.25f, "xp progress round-trips");
    check(bs_get_f32(fake_sent_buf + 17) == 13.5f, "health round-trips");
    check(bs_get_f32(fake_sent_buf + 21) == 6.5f, "hunger round-trips");

    bool tail_zero = true;
    for (size_t i = 25; i < BS_PLAYER_REPORT_BYTES; i++)
        if (fake_sent_buf[i] != 0) tail_zero = false;
    check(tail_zero, "all five reserved tail bytes went out zero — bs_proto.h's MUST");

    check(networldSendPlayerReport(NULL) == false, "NULL meters refused");
    check(fake_sent_calls == 1, "and that refusal never reached the transport");
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

/* ------------------------------------------------- registry sync (v1.6.0 Phase A) ---
 *
 * The client half of the master block registry: INFO/FETCH/DEFS. The table itself is
 * probed by world/registry_test.c; everything here is about the WIRE behaviour —
 * when a FETCH goes out, how batches converge, and what an edit carrying a not-yet-
 * -defined id does before and after sync. These run last in main() because they
 * mutate the process-wide registry the earlier tests expect pristine. */

static void buildRegistryInfo(uint8_t *out /* BS_APP_REGISTRY_INFO_BYTES */)
{
    out[0] = BS_APP_REGISTRY_INFO;
    out[1] = REGISTRY_REV;
    out[2] = registryCount();
    bs_put_u16(out + 3, registryCrc16());
}

static size_t buildRegistryDefs(uint8_t *out, uint8_t first,
                                const BlockDef *defs, unsigned n, bool last)
{
    out[0] = BS_APP_REGISTRY_DEFS;
    out[1] = first;
    out[2] = (uint8_t)n;
    out[3] = last ? 1 : 0;
    for (unsigned i = 0; i < n; i++) {
        registryDefPack(out + 4 + i * REGISTRY_WIRE_RECORD_BYTES,
                        (BlockId)(first + i), &defs[i]);
    }
    return BS_APP_REGISTRY_DEFS_BYTES(n);
}

static BlockDef wireDef(const char *name)
{
    BlockDef d;
    memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "%s", name);
    for (int f = 0; f < BLOCK_FACES; f++) d.tex[f] = BTEX_STONE;
    d.flags    = REG_FLAG_SOLID;
    d.hardness = 1;
    return d;
}

static void test_registry_info_match_sends_nothing(void)
{
    puts("registry: a matching INFO costs zero traffic, a short one arms nothing");

    registryInitCore();
    networldInit();
    fakeTransportReset();

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 0, "a matching rev/count/crc16 sends nothing back");

    /* A wrong-length INFO is dropped without arming the capability probe: the
     * FETCH that must only ever follow a WELL-FORMED INFO stays unsent. */
    networldInit();
    fakeTransportReset();
    uint8_t short_info[BS_APP_REGISTRY_INFO_BYTES - 1];
    memcpy(short_info, info, sizeof short_info);
    networldApplyPayload(short_info, sizeof short_info);
    check(fake_sent_calls == 0, "a malformed INFO neither matches nor provokes a FETCH");
}

static void test_registry_info_mismatch_sends_fetch_once(void)
{
    puts("registry: a mismatched INFO answers with exactly one FETCH per session");

    registryInitCore();
    networldInit();
    fakeTransportReset();

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[3] ^= 0xFFu;   /* corrupt the crc low byte: guaranteed mismatch */
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 1 && fake_sent_len == BS_APP_REGISTRY_FETCH_BYTES
          && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH
          && fake_sent_buf[1] == REG_ID_DYN_LO,
          "the mismatch draws one FETCH asking from the dyn base 0x80");

    /* The server re-transmits INFO (it volunteers it at join; a retry would too).
     * The FETCH must not go out twice — one outstanding request is the whole design. */
    fakeTransportReset();
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 0, "a second mismatched INFO does not re-send the FETCH");

    /* A revision mismatch alone is also a mismatch, on a fresh session. */
    networldInit();
    fakeTransportReset();
    buildRegistryInfo(info);
    info[1] = (uint8_t)(REGISTRY_REV + 1u);
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH,
          "a future-table revision draws the same single FETCH");
}

static void test_registry_defs_converge(void)
{
    puts("registry: DEFS batches converge the table; pre-sync unknown ids resolve");

    registryInitCore();
    networldInit();
    fakeTransportReset();

    World w;
    worldInit(&w);
    networldSetWorld(&w);
    worldColumnCreate(&w, 0, 0);

    /* An edit carrying a dyn id the local table does not know YET: accepted (it is
     * inside the legal id space), stored raw, and read back as air through
     * blockInfo()'s never-NULL contract until definitions arrive. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 5, 10, 7, REG_ID_DYN_LO);
    networldApplyPayload(msg, sizeof msg);
    check(worldGet(&w, 5, 10, 7) == REG_ID_DYN_LO,
          "an unknown-but-legal dyn id is stored raw, not rejected like 0xFF");
    check(!blockIsSolid(REG_ID_DYN_LO) && strcmp(blockInfo(REG_ID_DYN_LO)->name, "air") == 0,
          "before sync the id renders as air");

    /* Mismatched INFO -> FETCH -> DEFS. After the batch lands, the SAME id in the
     * SAME voxel resolves to the real definition — the doc's "render as air then
     * resolve" contract, end to end. */
    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[3] ^= 0xFFu;
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 1, "the mismatch drew the FETCH");

    BlockDef defs[2] = { wireDef("wire_a"), wireDef("wire_b") };
    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, defs, 2, true);
    networldApplyPayload(batch, len);

    check(registryFind("wire_a") == REG_ID_DYN_LO && registryFind("wire_b") == REG_ID_DYN_LO + 1,
          "both defs landed under their consecutive ids");
    check(registryCount() == 12, "count moved from 10 to 12 defined rows");
    check(blockIsSolid(REG_ID_DYN_LO), "after sync the same id resolves to the real def");
    check(worldGet(&w, 5, 10, 7) == REG_ID_DYN_LO,
          "the stored raw byte needed no rewrite — tables agree around it");
    check(registryCrc16() != 0x4066u,
          "the converged table no longer hashes like the core-only one");
}

static void test_registry_defs_malformed_dropped_whole(void)
{
    puts("registry: malformed DEFS batches are dropped whole, table untouched");

    registryInitCore();
    networldInit();
    fakeTransportReset();

    BlockDef defs[2] = { wireDef("bad_a"), wireDef("bad_b") };
    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, defs, 2, true);

    /* Short by one record byte: length-exact posture, drop without parsing. */
    networldApplyPayload(batch, len - 1);
    check(registryCount() == 10 && registryFind("bad_a") == 0,
          "a truncated DEFS applies nothing");

    /* Claiming more records than the sender's own cap can carry. */
    uint8_t greedy[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(greedy, batch, sizeof greedy);
    greedy[2] = BS_APP_REGISTRY_DEFS_MAX_N + 1u;
    networldApplyPayload(greedy, sizeof greedy);
    check(registryCount() == 10, "an over-cap count is refused outright");

    /* first below the dyn range would overwrite compiled-in core rows. */
    uint8_t hostile[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(hostile, batch, sizeof hostile);
    hostile[1] = BLOCK_STONE;
    networldApplyPayload(hostile, sizeof hostile);
    check(registryCount() == 10 && strcmp(blockInfo(BLOCK_STONE)->name, "stone") == 0,
          "a batch aimed at the core range changes nothing");

    /* A record whose embedded id disagrees with its slot position: all-or-nothing. */
    uint8_t shuffled[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(shuffled, batch, sizeof shuffled);
    shuffled[4] = REG_ID_DYN_LO + 1u;   /* record 0 claims id 0x81 */
    networldApplyPayload(shuffled, sizeof shuffled);
    check(registryCount() == 10 && registryFind("bad_a") == 0 && registryFind("bad_b") == 0,
          "one inconsistent record refuses the WHOLE batch, not just itself");
}

/* ------------------------------------------------- registry wiring (v1.6.0 task 8) ---
 *
 * The four scenarios below are the wiring the Phase A audit found inert: a FETCH that was
 * never retried, a synced flag nothing read, and a table nothing reset. They link the REAL
 * net/networld.c and the REAL world/registry.c — the only fakes in this binary are the
 * transport and the clock, both link-time doubles for code that cannot exist on a host at
 * all (see this file's header). Nothing here re-implements a decision networld.c makes. */

static void buildWorldInfoMsg(uint8_t *out /* BS_WORLD_INFO_BYTES */, uint32_t seed)
{
    out[0] = BS_APP_WORLD_INFO;
    bs_put_u32(out + 1, seed);
}

/* Puts the session exactly where a real join does: seed known, server's fingerprint heard
 * and disagreeing, first FETCH already on the wire. Leaves the fake clock at `t0`. */
static void registryJoinWithMismatch(uint64_t t0)
{
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(t0);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[3] ^= 0xFFu;   /* corrupt the crc low byte: guaranteed mismatch */
    networldApplyPayload(info, sizeof info);
}

/* The fingerprint a server whose table is the core rows plus `defs` would put in its
 * BS_APP_REGISTRY_INFO. Built by making that exact table locally and reading world/registry.c's
 * own registryCount()/registryCrc16() off it, then putting the table back — never by copying a
 * hash literal into this file, which would only prove the test agrees with itself.
 *
 * registryJoinWithMismatch() above corrupts the crc byte instead, which is the right shape for
 * "does a mismatch draw a FETCH" but the wrong shape for anything about COMPLETING one: no
 * delivery can ever reproduce a fingerprint that was never any real table's. */
static void buildRegistryInfoFor(uint8_t *out /* BS_APP_REGISTRY_INFO_BYTES */,
                                 const BlockDef *defs, unsigned n)
{
    registryInitCore();
    for (unsigned i = 0; i < n; i++) registryRegister(&defs[i]);

    out[0] = BS_APP_REGISTRY_INFO;
    out[1] = REGISTRY_REV;
    out[2] = registryCount();
    bs_put_u16(out + 3, registryCrc16());

    registryInitCore();
}

/* The ten core rows as world/block.h's own constants spell them, written out here so the
 * core-only fingerprint below can be DERIVED rather than recorded. This is deliberately not
 * read out of world/registry.c's kCoreDefs (it is static there anyway): a reference built from
 * the table under test would move whenever that table moved, and a pasted hash literal — which
 * is what this replaced — would pin whatever the table happened to hash to on the day it was
 * recorded, wrong answer included. Two independent spellings of the same ten rows can disagree;
 * a number copied out of a run cannot.
 *
 * The fields the rows never vary are not listed: luminance is 0 on every core row, fluid_class
 * is REG_FLUID_NONE on every core row (water included — REG_FLAG_LIQUID is what makes it a
 * liquid, and Phase B is what will make fluid_class mean anything), and variant_of is the row's
 * own id, because a core row is its own variant base. registryInitCore() sets the last two
 * itself rather than taking them from the definition; coreOnlyCrc16() below says so in bytes. */
typedef struct {
    uint8_t     id;
    const char *name;
    uint8_t     tex[BLOCK_FACES];
    uint8_t     flags;
    uint8_t     hardness;
} CoreRowSpec;

static const CoreRowSpec kCoreRowSpecs[] = {
    { 0, "air",   { 0, 0, 0, 0, 0, 0 }, REG_FLAG_TRANSPARENT, 0 },
    { 1, "grass", { BTEX_GRASS_SIDE, BTEX_GRASS_SIDE, BTEX_GRASS_TOP,
                    BTEX_DIRT,       BTEX_GRASS_SIDE, BTEX_GRASS_SIDE }, REG_FLAG_SOLID, 12 },
    { 2, "dirt",  { BTEX_DIRT, BTEX_DIRT, BTEX_DIRT,
                    BTEX_DIRT, BTEX_DIRT, BTEX_DIRT }, REG_FLAG_SOLID, 12 },
    { 3, "stone", { BTEX_STONE, BTEX_STONE, BTEX_STONE,
                    BTEX_STONE, BTEX_STONE, BTEX_STONE }, REG_FLAG_SOLID, 45 },
    { 4, "sand",  { BTEX_SAND, BTEX_SAND, BTEX_SAND,
                    BTEX_SAND, BTEX_SAND, BTEX_SAND }, REG_FLAG_SOLID, 10 },
    { 5, "wood",  { BTEX_WOOD_SIDE, BTEX_WOOD_SIDE, BTEX_WOOD_TOP,
                    BTEX_WOOD_TOP,  BTEX_WOOD_SIDE, BTEX_WOOD_SIDE }, REG_FLAG_SOLID, 40 },
    { 6, "leaves", { BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES,
                     BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES },
      REG_FLAG_SOLID | REG_FLAG_TRANSPARENT, 4 },
    { 7, "planks", { BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS,
                     BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS }, REG_FLAG_SOLID, 40 },
    { 8, "water",  { BTEX_WATER, BTEX_WATER, BTEX_WATER,
                     BTEX_WATER, BTEX_WATER, BTEX_WATER },
      REG_FLAG_TRANSPARENT | REG_FLAG_LIQUID, 0 },
    { 9, "tall_grass", { BTEX_TALL_GRASS, BTEX_TALL_GRASS, BTEX_TALL_GRASS,
                         BTEX_TALL_GRASS, BTEX_TALL_GRASS, BTEX_TALL_GRASS },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1 },
};

/* CRC-16/CCITT-FALSE, spelled out here rather than reached for in world/registry.c, so that the
 * comparison below is between two whole derivations and not between one derivation and itself.
 * The algorithm and the 28-byte record layout are both written down in world/registry.h's
 * contract for registryCrc16()/registryDefPack(); this is that contract, implemented a second
 * time from the header rather than from the .c file it is checking. */
static uint16_t coreCrcFeed(uint16_t crc, const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)bytes[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* What registryCrc16() must answer on a table holding the core rows and nothing else. */
static uint16_t coreOnlyCrc16(void)
{
    uint16_t crc = 0xFFFF;
    for (unsigned i = 0; i < sizeof kCoreRowSpecs / sizeof kCoreRowSpecs[0]; i++) {
        const CoreRowSpec *row = &kCoreRowSpecs[i];
        uint8_t rec[REGISTRY_WIRE_RECORD_BYTES];

        memset(rec, 0, sizeof rec);                 /* name tail is NUL padding */
        rec[0] = row->id;
        memcpy(&rec[1], row->name, strlen(row->name));
        memcpy(&rec[17], row->tex, BLOCK_FACES);
        rec[23] = row->flags;
        rec[24] = 0;                                /* luminance: 0 on every core row */
        rec[25] = row->hardness;
        rec[26] = row->id;                          /* variant_of: a base is its own root */
        rec[27] = REG_FLUID_NONE;

        crc = coreCrcFeed(crc, rec, sizeof rec);
    }
    return crc;
}

/* registryJoinWithMismatch()'s honest sibling: a real join against a server whose table is the
 * core rows plus `defs`. It still mismatches — this client is core-only until the DEFS land, so
 * the same single FETCH goes out — but this fingerprint is one a correct delivery can actually
 * reproduce, which is the whole thing the checks below are about. Leaves the fake clock at t0. */
static void registryJoinDeclaring(uint64_t t0, const BlockDef *defs, unsigned n)
{
    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfoFor(info, defs, n);

    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(t0);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);
    networldApplyPayload(info, sizeof info);
}

static void test_registry_fetch_retries_a_lost_reply(void)
{
    puts("registry: a lost DEFS reply is re-asked for, bounded in tries and in time");

    registryJoinWithMismatch(10000);
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH,
          "the mismatched INFO drew the first FETCH");

    /* The server's reply is lost. Pumping inside the retry interval must not re-ask: a
     * retry every frame would be 60 FETCHes a second at a server already struggling. */
    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS - 1);
    networldUpdate();
    check(fake_sent_calls == 0, "a pump inside the retry interval sends nothing");

    /* Past the interval, one more goes out — and only one, however many frames run. */
    fakeNowAdvance(2);
    networldUpdate();
    networldUpdate();
    networldUpdate();
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH
          && fake_sent_buf[1] == REG_ID_DYN_LO,
          "past the interval exactly one retry goes out, still asking from 0x80");

    /* The attempt bound. Keep the clock inside the deadline and pump for a long time: the
     * session total must stop at NETWORLD_REG_FETCH_MAX_SENDS, first send included — so
     * only MAX_SENDS - 2 remain after the first send and the retry just counted. */
    fake_sent_calls = 0;
    for (int i = 0; i < 40; i++) {
        fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS / 8);
        networldUpdate();
    }
    check(fake_sent_calls == NETWORLD_REG_FETCH_MAX_SENDS - 2,
          "the retries stop at MAX_SENDS total, not once per interval forever");

    /* And the time bound, proven independently of the attempt bound: a fresh session whose
     * clock has jumped past the deadline gets no retry at all, even though it has only ever
     * sent one FETCH and has three attempts left. */
    registryJoinWithMismatch(10000);
    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS);
    networldUpdate();
    check(fake_sent_calls == 0, "nothing is re-asked past the sync deadline");

    /* A reply that DOES arrive — and that reproduces the fingerprint the server named —
     * cancels the rest of the retries. The INFO here is the honest one a real server sends
     * (registryJoinDeclaring), not the corrupted one above, because a delivery that cannot
     * reproduce the fingerprint is deliberately NOT a completed sync any more and so must
     * not disarm the retry. */
    BlockDef defs[1] = { wireDef("retry_done") };
    registryJoinDeclaring(10000, defs, 1);
    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(1)];
    size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, defs, 1, true);
    networldApplyPayload(batch, len);
    fake_sent_calls = 0;
    for (int i = 0; i < 20; i++) {
        fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
        networldUpdate();
    }
    check(fake_sent_calls == 0, "once the last batch lands the retry stops immediately");

    /* A retry after a PARTIAL delivery must resume where the table actually got to.
     * registryRemoteApply() refuses any batch that does not start at the next free slot,
     * so re-asking from 0x80 here would be refused for the rest of the session. */
    registryJoinWithMismatch(10000);
    BlockDef two[2] = { wireDef("part_a"), wireDef("part_b") };
    uint8_t partial[BS_APP_REGISTRY_DEFS_BYTES(2)];
    len = buildRegistryDefs(partial, REG_ID_DYN_LO, two, 2, false);   /* not the last */
    networldApplyPayload(partial, len);
    check(registryCount() == 12, "the partial batch landed two rows");
    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
    networldUpdate();
    check(fake_sent_calls == 1 && fake_sent_buf[1] == REG_ID_DYN_LO + 2u,
          "the retry resumes from the first id the table is still missing");
}

/* The invariant the whole feature is allowed to exist under: a server that never speaks
 * registry must never receive BS_APP_REGISTRY_FETCH, or it kicks this client for an
 * unknown C->S type. The retry above is a NEW way to reach that sender, so it gets its own
 * scenario rather than being assumed safe. */
static void test_registry_fetch_never_reaches_a_silent_server(void)
{
    puts("registry: a server that never sent INFO never receives a FETCH, retries included");

    /* An old server: JOIN, WORLD_INFO, a normal session's traffic, and a long stretch of
     * frames. Not one byte of registry may go back. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(500);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 7);
    networldApplyPayload(wi, sizeof wi);

    uint8_t pos[BS_POS_UPDATE_S_BYTES];
    buildPosUpdateS(pos, 3, 1.0f, 2.0f, 3.0f, 0.0f, 0.0f);
    networldApplyPayload(pos, sizeof pos);

    for (int i = 0; i < 200; i++) {
        fakeNowAdvance(50);          /* 10 s of frames, five times the sync deadline */
        networldUpdate();
    }
    check(fake_sent_calls == 0,
          "ten seconds of pumping a pre-registry server sends nothing at all");
    check(!networldRegistrySynced(), "and the client knows its table is unverified");

    /* A MALFORMED INFO is not a well-formed one: it arms nothing, so it must not arm the
     * retry either. This is the case a bare length check would let through. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(500);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    uint8_t short_info[BS_APP_REGISTRY_INFO_BYTES - 1];
    memcpy(short_info, info, sizeof short_info);
    networldApplyPayload(short_info, sizeof short_info);

    for (int i = 0; i < 60; i++) {
        fakeNowAdvance(100);
        networldUpdate();
    }
    check(fake_sent_calls == 0, "a malformed INFO arms no retry either");

    /* Control arm: the same pump against a server that DID send a well-formed mismatching
     * INFO does send. Without this the two checks above would pass just as well against a
     * retry that had been wired to nothing at all. */
    registryJoinWithMismatch(500);
    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
    networldUpdate();
    check(fake_sent_calls == 1, "control: the same pump DOES retry after a real INFO");
}

/* s_reg_synced used to be set and never read. This is what now reads it: the gate that
 * holds world entry — and therefore main.c's registryFreeze() — open until the table is
 * settled or provably never going to be. Every arm is bounded, which is the property that
 * keeps a stalled join from becoming a title screen with no way out. */
static void test_registry_gate_holds_entry_until_the_table_settles(void)
{
    puts("registry: the sync gate holds world entry, and every arm of it is bounded");

    /* Single player / pre-join: never armed, so nothing is ever held. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);
    check(!networldRegistryWaiting(), "a client that has not joined anything is never held");
    check(!networldRegistrySynced(), "and has nothing to be synced with");

    /* Joined. The seed is in, the INFO has not arrived yet — held, inside the grace. */
    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 99);
    networldApplyPayload(wi, sizeof wi);
    check(networldRegistryWaiting(), "the seed alone holds entry while INFO could still land");

    /* An old server never sends one. The grace expires and entry proceeds DEGRADED — not
     * synced, but not stuck either. */
    fakeNowAdvance(NETWORLD_REG_INFO_GRACE_MS);
    check(!networldRegistryWaiting(), "a silent server stops holding entry at the grace");
    check(!networldRegistrySynced(), "and the client enters knowing the table is unverified");

    /* A matching INFO settles it outright: zero traffic, zero waiting. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);
    networldApplyPayload(wi, sizeof wi);
    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    networldApplyPayload(info, sizeof info);
    check(networldRegistrySynced(), "a matching fingerprint is a synced table");
    check(!networldRegistryWaiting(), "which holds entry for no frames at all");

    /* A mismatch holds entry across the whole fetch, then releases the instant the last
     * batch lands — the case the Phase A code could never reach, because entry happened
     * one frame after the seed and froze the table before this point. */
    BlockDef defs[2] = { wireDef("gate_a"), wireDef("gate_b") };
    registryJoinDeclaring(1000, defs, 2);
    check(networldRegistryWaiting(), "a mismatched INFO holds entry while the fetch runs");
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
    networldUpdate();
    check(networldRegistryWaiting(), "and keeps holding while a retry is still outstanding");

    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, defs, 2, true);
    networldApplyPayload(batch, len);
    check(networldRegistrySynced(), "the completed batch reproduces the server's fingerprint");
    check(!networldRegistryWaiting(), "and releases entry immediately");
    check(registryFind("gate_a") == REG_ID_DYN_LO,
          "the row the old freeze point made uncommittable is committed");

    /* And the give-up arm: a server that answers the INFO but never the FETCH. Held for
     * the deadline, released after it, still not synced. */
    registryJoinWithMismatch(1000);
    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS - 1);
    networldUpdate();
    check(networldRegistryWaiting(), "still held one millisecond before the deadline");
    fakeNowAdvance(1);
    check(!networldRegistryWaiting(), "released at the deadline");
    check(!networldRegistrySynced(), "entering degraded rather than never entering");

    /* A retransmitted WORLD_INFO must not push the deadline out. A chatty or hostile
     * server could otherwise hold this console on the title screen forever. */
    registryJoinWithMismatch(1000);
    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS - 10);
    networldApplyPayload(wi, sizeof wi);
    fakeNowAdvance(10);
    check(!networldRegistryWaiting(), "a repeated WORLD_INFO does not extend the deadline");
}

/* The table is per-session state and was the one piece of it networldInit() did not clear,
 * so a second join reused the first server's rows — and, worse, could not accept the second
 * server's, since registryRemoteApply() requires a batch to start at the table's own next
 * free slot and the first server had already moved it. */
static void test_registry_table_resets_between_sessions(void)
{
    puts("registry: leaving a session puts the block table back to the core rows");

    registryJoinWithMismatch(1000);
    BlockDef defs[2] = { wireDef("srv1_a"), wireDef("srv1_b") };
    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, defs, 2, true);
    networldApplyPayload(batch, len);
    check(registryCount() == 12 && registryFind("srv1_a") == REG_ID_DYN_LO,
          "the first server's two rows are in the table");

    /* netDisconnect() runs networldInit() (net/bsnet.c), which is the whole of leaving a
     * session as far as this module is concerned. */
    networldInit();
    check(registryCount() == 10, "after leaving, only the ten core rows remain");
    check(registryFind("srv1_a") == 0 && registryFind("srv1_b") == 0,
          "the first server's names are gone, not merely hidden");
    check(registryCrc16() == coreOnlyCrc16(),
          "and the table hashes as the independently derived core-only fingerprint again");
    check(!networldRegistrySynced() && !networldRegistryWaiting(),
          "with the sync state cleared alongside it");

    /* The second server's batch is the proof that matters: it starts at 0x80 too, and would
     * have been refused outright against a table still holding server one's rows. */
    fakeTransportReset();
    fakeNowSet(50000);
    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 777);
    networldApplyPayload(wi, sizeof wi);
    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[3] ^= 0xFFu;
    networldApplyPayload(info, sizeof info);

    BlockDef defs2[1] = { wireDef("srv2_only") };
    uint8_t batch2[BS_APP_REGISTRY_DEFS_BYTES(1)];
    len = buildRegistryDefs(batch2, REG_ID_DYN_LO, defs2, 1, true);
    networldApplyPayload(batch2, len);
    check(registryFind("srv2_only") == REG_ID_DYN_LO,
          "the second server's row takes 0x80, which server one had been holding");
    check(registryCount() == 11, "and it is the only dynamic row in the table");
}

/* v1.6.0 F2, and the scenario the test directly above could not be: it calls networldInit()
 * itself, so it proves "leaving a session clears the table" and nothing about the path where
 * NOTHING calls networldInit(). That path is single player. registryFreeze() runs in genStart()
 * for a single-player world exactly as it does for a joined one — the worker's no-locks
 * contract needs it there, and app/worker.c's workerSubmitColumn() refuses to generate a column
 * against an unfrozen table — but `if (quit_to_title) goto session_start;` (source/main.c) went
 * back to the menu without unfreezing it. So the player's very next act, joining a server, met
 * registryRemoteApply()'s `if (s_frozen) return 0;` on every batch, and every server-defined
 * block was an invisible hole for the whole session.
 *
 * The lap below is app/session.c's sessionBegin() and nothing else, because sessionBegin() and
 * nothing else is what main.c runs between the two worlds. The registry-level version of this
 * lives in app/session_test.c; this one is here because it goes through net/networld.c's real
 * BS_APP_REGISTRY_DEFS handler and so can also check the flag the HUD's degraded marker reads,
 * which was the other half of what the player saw. */
static void test_registry_join_after_a_single_player_quit_to_title(void)
{
    puts("registry: a server join right after a single-player quit-to-title still syncs");

    /* The server's fingerprint, built first because buildRegistryInfoFor() rebuilds the local
     * table to derive it and would otherwise wipe the single-player session set up below. */
    BlockDef srv[1] = { wireDef("srv_blk") };
    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfoFor(info, srv, 1);

    /* Boot: main.c:2519's networldInit(), next to netInit(). */
    registryInitCore();
    networldInit();

    /* A single-player session, in genStart()'s real order: the world's own registry.bin rows
     * go in first, then the freeze, before workerStart(). */
    BlockDef sp = wireDef("sp_sidecar");
    check(registryRegister(&sp) == REG_ID_DYN_LO,
          "control: the single-player world's sidecar row is really in the table");
    registryFreeze();
    check(registryFrozen() && registryCount() == 11,
          "control: and genStart() has frozen it, in single player exactly as in a session");

    /* Quit to title. This is the whole of it. */
    sessionBegin();

    /* The join, from the multiplayer screen, one lap later and without a reboot. */
    fakeTransportReset();
    fakeNowSet(90000);
    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 555);
    networldApplyPayload(wi, sizeof wi);
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH,
          "the server's INFO still draws a FETCH, so the table is the only variable");

    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(1)];
    const size_t len = buildRegistryDefs(batch, REG_ID_DYN_LO, srv, 1, true);
    networldApplyPayload(batch, len);

    check(registryFind("srv_blk") == REG_ID_DYN_LO,
          "the server's block is committed at 0x80 rather than refused by the stale freeze");
    check(registryCount() == 11 && registryFind("sp_sidecar") == 0,
          "and it is the only dynamic row: the last world's is gone");
    /* The NAME is checked, not just "a defined solid row exists at 0x80". Against the stale
     * table the single-player world's own row was sitting in that slot, defined and solid, so
     * an id-only check would have passed while the player was still colliding with the wrong
     * block — which is the failure, not the absence of one. */
    check(registryIsDefined(REG_ID_DYN_LO) && registryView(REG_ID_DYN_LO)->solid
              && strcmp(registryView(REG_ID_DYN_LO)->name, "srv_blk") == 0,
          "it is the SERVER's solid block at that id, so the player collides with the right "
          "thing instead of falling through a hole");
    check(networldRegistrySynced(),
          "and the table reproduces the server's fingerprint, so no degraded marker is shown");
    check(!networldRegistryWaiting(), "world entry is not held open waiting for it");
}

/* ------------------------------------------------- registry fingerprint (v1.6.0 task 8b) ---
 *
 * s_reg_synced used to mean "a BS_APP_REGISTRY_DEFS packet with the LAST bit arrived". The
 * server sends exactly such a packet unconditionally — bsgame.c's fetch loop emits an empty
 * final batch even when it has nothing to say — so the flag could be set by four bytes that
 * carried no rows, checked no first index, and were not refused by a frozen table. A client
 * whose 1 KB data batch was lost in the UDP then declared itself synced with a core-only
 * table, disarmed its own bounded retry, and spent the whole session rendering every
 * server-defined block as an air hole while the HUD showed no degraded marker at all.
 *
 * It now means "this table demonstrably reproduces the fingerprint the server sent" — the
 * INFO's rev/count/crc16, retained and re-checked against registryCount()/registryCrc16()
 * after every batch. The two scenarios below are what holds that meaning in place. */
static void test_registry_terminator_alone_is_not_a_synced_table(void)
{
    puts("registry: an empty terminating batch is not a synced table, frozen or not");

    BlockDef declared[2] = { wireDef("term_a"), wireDef("term_b") };

    /* Trigger (a): the server's reply spans more than one packet, the batch carrying the
     * rows is lost, and only the four-byte terminator arrives. */
    registryJoinDeclaring(10000, declared, 2);
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH,
          "the rows the server declared but this table lacks drew a FETCH");

    uint8_t term[BS_APP_REGISTRY_DEFS_BYTES(0)];
    const size_t tlen = buildRegistryDefs(term, REG_ID_DYN_LO, declared, 0, true);
    networldApplyPayload(term, tlen);

    check(registryCount() == 10,
          "the terminator carried no records, so the table is still core-only");
    check(!networldRegistrySynced(),
          "and a table that cannot reproduce the server's crc16 is not a synced table");
    check(networldRegistryWaiting(), "so the entry gate is still holding for the retry");

    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
    networldUpdate();
    check(fake_sent_calls == 1 && fake_sent_buf[0] == BS_APP_REGISTRY_FETCH
          && fake_sent_buf[1] == REG_ID_DYN_LO,
          "the retry is still armed and re-asks from the first id the table is missing");

    /* Trigger (b): the INFO disagrees on the table REVISION while the server holds no dynamic
     * rows at all, so its fetch loop finds nothing to send and terminates empty on the first
     * packet. The client used to answer that by declaring itself synced with a table it had
     * just been told disagrees. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(10000);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[1] = (uint8_t)(REGISTRY_REV + 1u);   /* a future table revision, dyn rows: none */
    networldApplyPayload(info, sizeof info);
    check(fake_sent_calls == 1, "the revision disagreement drew the FETCH");

    networldApplyPayload(term, tlen);
    check(!networldRegistrySynced(),
          "an empty terminator does not settle a table the server called another revision");
    check(networldRegistryWaiting(), "and the gate keeps holding rather than releasing early");

    /* Still bounded in both directions, which is the property that keeps this from becoming a
     * title screen with no way out. Forwards first — the deadline (or a lid-close sleep that
     * jumps the wall clock past it) releases the player DEGRADED. */
    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS);
    check(!networldRegistryWaiting(), "the deadline releases entry, degraded rather than never");
    check(!networldRegistrySynced(), "with the indicator still honestly reporting unverified");

    /* And backwards: bsSockNowMs() is osGetTime() on hardware, a wall clock that can step
     * back. Every bound here is `now - base` on uint64_t, so a backwards step underflows to a
     * huge value and both bounds fire — released, never trapped, and nothing more is sent. */
    registryJoinDeclaring(10000, declared, 2);
    networldApplyPayload(term, tlen);
    check(networldRegistryWaiting(), "held while the clock is behaving");
    fakeNowSet(9999);                          /* one millisecond backwards */
    check(!networldRegistryWaiting(), "a clock that steps backwards releases, never traps");
    fake_sent_calls = 0;
    networldUpdate();
    check(fake_sent_calls == 0, "and re-asks nothing of a server it can no longer time");

    /* F4: after registryFreeze() — which is where genStart() leaves the table for the rest of
     * the session — no row can ever be committed again. The degraded indicator has to read
     * degraded at exactly that point; it used to read healthy. */
    registryJoinDeclaring(10000, declared, 2);
    registryFreeze();
    check(registryFrozen(), "the table is frozen, as it is for every frame after genStart()");

    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    const size_t blen = buildRegistryDefs(batch, REG_ID_DYN_LO, declared, 2, true);
    networldApplyPayload(batch, blen);
    check(registryCount() == 10, "the frozen table refused the batch, per registry.h's contract");
    check(!networldRegistrySynced(),
          "and a refused batch's LAST flag does not turn the indicator healthy");

    networldApplyPayload(term, tlen);
    check(!networldRegistrySynced(), "nor does a bare terminator after the freeze");

    registryInitCore();   /* unfreeze: the scenarios after this one register rows of their own */

    /* No INFO at all: DEFS that nobody asked for, from a server that never sent a fingerprint.
     * There is nothing to check the table against, so nothing can be proved and the answer has
     * to be no — which is also the case the old LAST-flag test accepted outright. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(10000);
    networldApplyPayload(wi, sizeof wi);

    networldApplyPayload(batch, blen);
    check(registryCount() == 12, "the unsolicited batch's rows did land in the table");
    check(!networldRegistrySynced(),
          "but with no INFO there is no fingerprint, so nothing is verified");

    networldApplyPayload(term, tlen);
    check(!networldRegistrySynced(), "and its terminator settles nothing either");
}

/* The gap F1 was the exploit of: nothing ever re-ran registryCrc16() once a fetch finished, so
 * networldRegistrySynced()'s documented "provably agrees with the server's" reduced to "the
 * server said that was all". These are the checks that make the word "provably" true. */
static void test_registry_sync_is_proved_by_the_fingerprint(void)
{
    puts("registry: a completed fetch is checked against the INFO's fingerprint, not trusted");

    BlockDef declared[3] = { wireDef("fp_a"), wireDef("fp_b"), wireDef("fp_c") };

    /* The whole declared set arrives: rev, count and crc16 all reproduce. This is the only
     * thing that is a synced table, and it must still be one — a fingerprint check that never
     * says yes would pass every negative below for the wrong reason. */
    registryJoinDeclaring(20000, declared, 3);
    uint8_t all[BS_APP_REGISTRY_DEFS_BYTES(3)];
    size_t alen = buildRegistryDefs(all, REG_ID_DYN_LO, declared, 3, true);
    networldApplyPayload(all, alen);
    check(registryCount() == 13, "all three declared rows landed");
    check(networldRegistrySynced(), "and the table reproduces the server's whole fingerprint");
    check(!networldRegistryWaiting(), "which releases world entry immediately");

    /* A SHORT delivery that still carries the LAST flag: two of the three rows, then "that is
     * all". The count alone already disagrees, and the retry must stay armed for the third. */
    registryJoinDeclaring(20000, declared, 3);
    uint8_t two[BS_APP_REGISTRY_DEFS_BYTES(2)];
    const size_t twolen = buildRegistryDefs(two, REG_ID_DYN_LO, declared, 2, true);
    networldApplyPayload(two, twolen);
    check(registryCount() == 12, "two of the three rows landed");
    check(!networldRegistrySynced(),
          "a short delivery is not a synced table however the LAST flag is set");
    fake_sent_calls = 0;
    fakeNowAdvance(NETWORLD_REG_FETCH_RETRY_MS);
    networldUpdate();
    check(fake_sent_calls == 1 && fake_sent_buf[1] == REG_ID_DYN_LO + 2u,
          "and the retry asks for exactly the row still missing");

    /* Same COUNT, different CONTENT. Only the crc16 catches this one — the number
     * applyRegistryInfo() used to compute for its own comparison and then throw away. */
    registryJoinDeclaring(20000, declared, 3);
    BlockDef impostor[3] = { wireDef("fp_a"), wireDef("fp_b"), wireDef("fp_X") };
    uint8_t wrong[BS_APP_REGISTRY_DEFS_BYTES(3)];
    const size_t wlen = buildRegistryDefs(wrong, REG_ID_DYN_LO, impostor, 3, true);
    networldApplyPayload(wrong, wlen);
    check(registryCount() == 13, "three rows landed, so rev and count both agree");
    check(registryFind("fp_X") == REG_ID_DYN_LO + 2u, "with the third row under its own name");
    check(!networldRegistrySynced(),
          "but the crc16 disagrees, so this is not the server's table and not synced");

    /* And the flip side of no longer trusting the flag: a delivery that completes the table
     * with its terminator LOST is synced anyway. A dropped terminator now costs nothing. */
    registryJoinDeclaring(20000, declared, 3);
    alen = buildRegistryDefs(all, REG_ID_DYN_LO, declared, 3, false);
    networldApplyPayload(all, alen);
    check(networldRegistrySynced(),
          "a complete table whose terminator was lost is still provably the server's");

    /* Control that cannot pass by luck: the same complete, correct rows against a server that
     * named a different table REVISION never verify, because this build cannot know what that
     * revision's rows would have meant. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(20000);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfoFor(info, declared, 3);
    info[1] = (uint8_t)(REGISTRY_REV + 1u);
    networldApplyPayload(info, sizeof info);

    alen = buildRegistryDefs(all, REG_ID_DYN_LO, declared, 3, true);
    networldApplyPayload(all, alen);
    check(registryCount() == 13, "control: the rows landed exactly as in the passing case");
    check(!networldRegistrySynced(), "but a foreign table revision is never verifiable");
}

/* How many block faces one emitted quad covers. Since v1.6.0 task 11 meshChunk()
 * merges runs of identical co-planar faces along u into one wide quad, so out.faces
 * counts QUADS, not block faces: a merged quad w blocks wide spans u0 .. u0+TILE_PX*w.
 * Block faces are the number that never moves — merging regroups them, it never
 * creates or destroys one — so the counts below are stated in those. Quad q owns
 * verts q*4..q*4+3, the four-per-quad-in-order property chunk_render.c's shared
 * index buffer also rests on. (world/world_test.c owns the merge algorithm itself;
 * this file only cares that the registry's rect table feeds it the right tiles.) */
static uint32_t meshFaceCells(const MeshOut *o)
{
    uint32_t cells = 0;
    for (uint32_t q = 0; q < o->faces; q++) {
        const MeshVertex *v = &o->verts[q * 4];
        uint8_t lo = v[0].u, hi = v[0].u;
        for (int i = 1; i < 4; i++) {
            if (v[i].u < lo) lo = v[i].u;
            if (v[i].u > hi) hi = v[i].u;
        }
        cells += (uint32_t)((hi - lo) / TILE_PX);
    }
    return cells;
}

/* Proves planBuild ordering plus the s_rect resize in one scenario: a solid block
 * registered ABOVE the old [BLOCK_COUNT] rect table meshes correctly. Before the
 * v1.6.0 fix this indexed s_rect[0x80] out of bounds of an 8-row array; today the
 * table is [REGISTRY_MAX][BLOCK_FACES] and the face emits with the right atlas tile. */
static void test_mesher_tables_after_register(void)
{
    puts("mesher: a freshly registered dyn block meshes with its own atlas rect");

    registryInitCore();

    BlockDef d = wireDef("mesh_probe");
    d.tex[FACE_TOP] = BTEX_GRASS_TOP;   // distinguishable from the sides on purpose
    BlockId id = registryRegister(&d);
    check(id == REG_ID_DYN_LO, "the probe block took the first dyn id");

    World w;
    worldInit(&w);
    /* Chunk (1,1,1), not (0,0,0): at the world's bottom layer the mesher culls
     * the underside on purpose (scratchFill treats below-world as solid), which
     * would make the expected face count 1280 instead of the full 1536. */
    Chunk *c = worldChunkCreate(&w, 1, 1, 1);
    check(c != NULL, "fixture chunk allocated");
    if (!c) return;
    chunkClear(c, id);   // whole chunk = the new block

    static MeshScratch scratch;
    scratchFill(&scratch, &w, 1, 1, 1);

    MeshOut out = {0};
    out.vert_cap  = MESH_MAX_VERTS;
    out.index_cap = MESH_MAX_INDICES;
    out.verts   = (MeshVertex *)malloc(sizeof(MeshVertex) * out.vert_cap);
    out.indices = (uint16_t *)malloc(sizeof(uint16_t) * out.index_cap);
    check(out.verts != NULL && out.indices != NULL, "mesh buffers allocated");
    if (!out.verts || !out.indices) { free(out.verts); free(out.indices); return; }

    meshChunk(&out, &scratch);
    check(!out.overflow, "the mesh completed without overflow");
    check(meshFaceCells(&out) == 6 * CHUNK_DIM * CHUNK_DIM,
          "a full chunk of the new solid block emits every face");
    check(out.opaque_faces == out.faces && out.opaque_index_count == out.index_count,
          "and all of it is opaque, since the def carries SOLID without TRANSPARENT");

    unsigned wrong_tile = 0;
    for (uint32_t i = 0; i < out.vert_count; i++) {
        const MeshVertex *v = &out.verts[i];
        const AtlasRect r = atlasRect(d.tex[v->nrm]);
        /* v pins the tile and is the strong half of this claim: the strip atlas
         * stacks tiles in v, so a wrong rect row shows up here. Since task 13b v
         * is a slot-EDGE index (tile or tile+1), not a pixel row, which is why it
         * compares against vslot0/vslot1 with no TILE_PX factor. u is a merge run —
         * it starts at the tile's u0 and steps a whole tile at a time, up to
         * ATLAS_MAX_MERGE_BLOCKS of them, deliberately past this tile's own u1
         * because u is sampled GPU_REPEAT (world/atlas_uv.h). */
        const int du = (int)v->u - (int)r.u0;
        const bool u_ok = du >= 0 && (du % TILE_PX) == 0
                          && du <= TILE_PX * ATLAS_MAX_MERGE_BLOCKS;
        if (!u_ok || (v->v != r.vslot0 && v->v != r.vslot1)) {
            wrong_tile++;
        }
    }
    check(wrong_tile == 0, "every vertex UV corner comes from the def's own tiles");

    /* mesherInvalidateTables() drops planBuild's readiness; the next meshChunk
     * rebuilds lazily and reaches the identical answer. This is the exact call
     * registryApplyRemote() makes after each applied batch. */
    mesherInvalidateTables();
    memset(out.verts, 0xAB, sizeof(MeshVertex) * out.vert_cap);
    meshChunk(&out, &scratch);
    check(meshFaceCells(&out) == 6 * CHUNK_DIM * CHUNK_DIM && out.vert_count > 0,
          "after invalidation the next meshChunk rebuilds the same geometry");

    free(out.verts);
    free(out.indices);
}

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
    test_player_state_pose_and_meters_round_trip();
    test_player_state_flag_combinations();
    test_player_state_wrong_length_dropped();
    test_player_state_float_policy_verbatim();
    test_player_state_capability_transitions();
    test_send_player_report_encodes();
    test_rejoin_sync_larger_than_the_store_keeps_every_edit();

    /* Registry probes last: they mutate the process-wide table. */
    test_registry_info_match_sends_nothing();
    test_registry_info_mismatch_sends_fetch_once();
    test_registry_defs_converge();
    test_registry_defs_malformed_dropped_whole();
    test_registry_fetch_retries_a_lost_reply();
    test_registry_fetch_never_reaches_a_silent_server();
    test_registry_gate_holds_entry_until_the_table_settles();
    test_registry_table_resets_between_sessions();
    test_registry_join_after_a_single_player_quit_to_title();
    test_registry_terminator_alone_is_not_a_synced_table();
    test_registry_sync_is_proved_by_the_fingerprint();
    test_mesher_tables_after_register();

    /* ---- check-count guard -----------------------------------------------------------------
     * This suite counts failures, and until 2026-08-25 that was ALL it counted. A suite that
     * only counts failures cannot notice checks that never ran. Any sabotage which shortens a
     * loop bounded by a production constant therefore DELETES checks instead of failing them,
     * and the run stays green: NETWORLD_MAX_REMOTE 15 -> 7 took this file from
     * "PASS 326 checks, 0 failed" to "PASS 318 checks, 0 failed", both green, eight checks gone.
     *
     * So: the number below is the count of checks that must already have run by the time
     * control reaches this line. It is a naked literal on purpose — it is the one number in
     * this file that is not derived from anything the tests themselves compute, which is
     * precisely what lets it notice them vanishing.
     *
     * HOW TO UPDATE IT WHEN YOU ADD OR REMOVE CHECKS — read this before changing the number:
     *   Work out the delta from what you actually changed (checks added minus checks removed)
     *   and ADD THAT DELTA to the number below. Do NOT paste whatever the failing run printed.
     *   Pasting the observed count is the single failure mode this guard exists to catch: if a
     *   constant shrank and silently deleted eight checks, the printed count is the SYMPTOM,
     *   and copying it in here re-arms the trap and throws away the only evidence you had.
     *   If your recomputed delta and the observed count disagree, that disagreement is a bug
     *   report — go and find out which checks stopped running, and why.
     *
     *   Note the number is the count BEFORE this guard itself, so the summary line prints one
     *   more than it (327 here, 328 on the PASS line). That off-by-one is deliberate: it means
     *   blind-pasting the number off the PASS line lands you a red, not a false green.
     *
     * This guard is deliberately scoped to THIS suite. The same hole exists in every other
     * host suite under source/ and is a separate, fleet-wide job. */
    check(g_checks == 327,
          "check-count guard: every check in this suite actually ran (327 before this line)");

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
