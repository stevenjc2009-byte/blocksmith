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
#include "world/genrefuse.h"
#include "world/genversion.h"
#include "world/mesher.h"
#include "world/registry.h"
#include "world/scratch.h"
#include "world/water.h"
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

/* v1.8.7. The fourth link-time double, and the only one of the four that is not merely a
 * stand-in for something this suite ignores: net/networld.c's registry verdict ends a session
 * through netTransportRefuse(), so this IS the observable the refusal scenarios below check.
 * There is no transport in this binary to put into a failed state, so the call is recorded
 * rather than acted on — that it happened, how many times, and with which line. */
static int     fake_refuse_calls;
static char    fake_refuse_why[128];

static void fakeTransportReset(void)
{
    fake_recv_head = fake_recv_tail = fake_recv_count = 0;
    fake_sent_len   = 0;
    fake_sent_calls = 0;
    fake_send_fail  = false;
    fake_refuse_calls  = 0;
    fake_refuse_why[0] = '\0';
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

/* v1.8.7. networld.c gained networldSessionActive(), which reads this — so it is a third
 * extern the link has to satisfy, doubled here for exactly the reason the two above are:
 * bsnet_transport.c is not in this binary. ESTABLISHED is the honest default for a suite whose
 * every case is about a client that already has a session; fake_send_fail above is how a case
 * says "this packet did not go", which is a different question from "is there a server". */
NetTransportState netTransportState(void)
{
    return NET_TRANSPORT_ESTABLISHED;
}

void netTransportRefuse(const char *why)
{
    fake_refuse_calls++;
    snprintf(fake_refuse_why, sizeof fake_refuse_why, "%s", why ? why : "");
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

/* v1.8.5. networld.c's pending store used to be a `static BlockDiffStore` — 0x104010 =
 * 1064976 bytes of .bss, committed from boot whether or not the player ever opened
 * Multiplayer, and invisible to both mallinfo() and linearSpaceFree(). It is now a pointer,
 * NULL for the whole of a single-player session and malloc'd on the first server packet that
 * actually needs somewhere to queue an edit.
 *
 * Every scenario above this one reaches the store by queueing into it first, so all of them
 * run against a store that exists. This one is the other half: the state single player is in
 * for its entire run, where every path into the store has to answer without one. */
static void test_pending_store_is_absent_in_single_player(void)
{
    puts("with no store allocated, every pending-store path is safe and answers 0");

    World w;
    worldInit(&w);
    networldInit();               /* frees whatever store the scenario before this allocated */
    networldSetWorld(&w);

    check(!networldTestPendingStoreAllocated(),
          "no store exists at all after init — not an empty one");
    check(networldPendingCount() == 0, "count reads 0 with no store");
    check(networldPendingRefusals() == 0, "refusals reads 0 with no store");

    /* Draining while absent. This is the single-player hot path, not an edge case: main.c
     * calls networldOnColumnLoad() for every column the streamer installs, so in a solo world
     * this runs hundreds of times and must never touch — or create — a store. */
    worldColumnCreate(&w, 0, 0);
    worldSet(&w, 3, 12, 3, BLOCK_GRASS);
    networldOnColumnLoad(&w, 0, 0);
    check(worldGet(&w, 3, 12, 3) == BLOCK_GRASS,
          "a column load with no store leaves the world exactly as it was");
    check(networldPendingCount() == 0, "and still reports nothing queued");
    /* The count above cannot carry this on its own — an allocated empty store answers 0 too,
     * and a drain that called pendingStore() passed the count check unchanged when it was
     * injected. This is the check that actually holds the reclaim. */
    check(!networldTestPendingStoreAllocated(),
          "and did not allocate a store on the way past just to find it empty");

    /* Recording while absent: the first edit that genuinely needs the store is what creates
     * it, which is the only trigger there is — nothing tells this module a session began. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 40, 10, 5, BLOCK_STONE);   /* column (2,0), never created */
    networldApplyPayload(msg, sizeof msg);
    check(networldTestPendingStoreAllocated(), "the store exists once an edit has needed it");
    check(networldPendingCount() == 1,
          "the first edit that needs a store allocates one and queues into it");
    check(networldPendingRefusals() == 0, "and nothing was refused doing it");

    /* And networldInit() hands the megabyte back — the disconnect path, net/bsnet.c's
     * netDisconnect(). */
    networldInit();
    check(networldPendingCount() == 0, "networldInit() drops everything in the store");
    check(!networldTestPendingStoreAllocated(),
          "and gives the megabyte back rather than merely emptying it — which is the whole "
          "reclaim on the disconnect path");

    /* Freed and re-creatable, not freed and abandoned. Without this check a pendingDestroy()
     * that failed to NULL the pointer, or a pendingStore() that refused to allocate twice,
     * would leave a rejoin in the same boot silently queueing nothing — which looks exactly
     * like a working client right up until the player's building is missing. */
    networldSetWorld(&w);
    networldApplyPayload(msg, sizeof msg);
    check(networldPendingCount() == 1,
          "a fresh store is created for the next session's first edit");
}

/* The out-of-memory branch. A 1.02 MB malloc on a 3DS with the mesh pool already up is not a
 * theoretical failure, and the wrong answer to it is the one that costs nothing to write:
 * return quietly, having neither queued the edit nor said so. That is the "my house is gone"
 * bug reached from a different direction, and it is why blockdiff.h argues at length that a
 * refused diff has to be countable. So an allocation failure is reported through the same
 * counter a full store is — networldPendingRefusals(), which main.c already renders as "X". */
static void test_pending_store_allocation_failure_is_counted_not_swallowed(void)
{
    puts("an edit that cannot be queued because the store will not allocate is counted, not swallowed");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);

    networldTestForcePendingAllocFail(true);

    uint8_t m1[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(m1, 40, 10, 5, BLOCK_STONE);   /* column (2,0), never created */
    networldApplyPayload(m1, sizeof m1);

    check(networldPendingCount() == 0, "nothing is queued when the store cannot be allocated");
    check(worldGet(&w, 40, 10, 5) == BLOCK_AIR,
          "and the edit was not written into the world as a consolation instead");
    check(worldColumn(&w, 2, 0) == NULL, "nor was a phantom column created to hold it");
    check(networldPendingRefusals() == 1,
          "the lost edit is counted as a refusal, which is what lights main.c's \"X\" marker");

    /* Every failed attempt counts, not only the first. A join sync that spent its whole length
     * failing must not report one lost edit. */
    uint8_t m2[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(m2, 41, 11, 6, BLOCK_SAND);    /* also column (2,0) */
    networldApplyPayload(m2, sizeof m2);
    check(networldPendingRefusals() == 2, "a second failed edit is counted too");

    /* Transient, not latched. The allocation is retried on the next edit, so a console that
     * could not spare a megabyte during one frame is not desynced for the rest of the session. */
    networldTestForcePendingAllocFail(false);
    networldApplyPayload(m2, sizeof m2);
    check(networldPendingCount() == 1,
          "once allocation succeeds again the store is created and used");
    check(networldPendingRefusals() == 2,
          "and the two edits already lost stay on the record rather than being reset");

    /* A store created late still drains normally — the failure path must not leave it in a
     * state that only looks initialised. */
    worldColumnCreate(&w, 2, 0);
    networldOnColumnLoad(&w, 2, 0);
    check(worldGet(&w, 41, 11, 6) == BLOCK_SAND, "the edit that did get queued lands on load");
    check(networldPendingCount() == 0, "leaving nothing pending");

    /* Disarm the seam for every scenario after this one — it is a process-wide static, in the
     * same way the registry table the scenarios at the bottom of main() are grouped for is. */
    networldTestForcePendingAllocFail(false);
    networldInit();
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

/* ------------------------------------------- the server's generator (v1.8.3 Phase 4) ------
 *
 * BS_APP_WORLD_GEN, sent by the server on the line immediately after BS_APP_WORLD_INFO. The
 * seed says WHICH world; this says WHAT SHAPE it is. Until v1.8.3 the shape was assumed —
 * both ends generated legacy terrain and neither ever said so — and the assumption was
 * correct only because no server had ever run anything else. The moment one does, a client
 * that assumes legacy does not fail: it generates a whole different world from the same seed,
 * silently, and then builds in it.
 *
 * BS_APP_WORLD_INFO could not carry the extra bytes. net/networld.c's applyWorldInfo() and the
 * server's own decoder both compare length with STRICT EQUALITY, so a widened WORLD_INFO is
 * dropped whole by every client already in the field — no seed, no world, no diagnostic. Hence
 * a new type at the next free id rather than two more bytes on an old one; see proto/bs_proto.h
 * on BS_APP_WORLD_GEN for the same reasoning stated where the id is allocated.
 */
static void test_world_gen_carries_the_servers_generator(void)
{
    puts("BS_APP_WORLD_GEN hands the server's generator declaration to the client");

    networldInit();

    /* Poisoned exactly as the seed case above poisons it, and for the same reason: 0 and 1 are
     * both values a server can legitimately declare, so "nothing said" cannot be a sentinel. */
    uint32_t gen = 0xDEADBEEFu;
    check(!networldServerGenVersion(&gen), "no generator is claimed before the server sends one");
    check(gen == 0xDEADBEEFu, "a false return does not write the out-parameter");

    uint8_t wg[BS_WORLD_GEN_BYTES];
    wg[0] = BS_APP_WORLD_GEN;
    bs_put_u16(wg + 1, 2u);
    networldApplyPayload(wg, sizeof wg);
    check(networldServerGenVersion(&gen), "the declaration is available once WORLD_GEN arrives");
    check(gen == 2u, "and it is the exact value the server sent");

    /* Byte order, asserted against literal bytes rather than through bs_put_u16. Written this
     * way on purpose: a round trip through the matching putter and getter agrees with itself
     * whichever order it uses, so it cannot see an endianness bug — it can only see one that
     * disagrees with ITSELF. The server writes these two bytes with bs_put_u16 and the value
     * has to survive the wire, so the wire bytes are what this pins. 0x0102 rather than a
     * palindrome, so low-byte-first and high-byte-first give different answers. */
    networldInit();
    wg[0] = BS_APP_WORLD_GEN;
    wg[1] = 0x02;
    wg[2] = 0x01;
    networldApplyPayload(wg, sizeof wg);
    check(networldServerGenVersion(&gen) && gen == 0x0102u,
          "the two payload bytes are read little-endian, low byte first");

    /* 0 is not a generator this build can make — world/genversion.h refuses it — but it has to
     * arrive AS 0 and be refused there, not be mistaken here for silence and resolved to
     * legacy. The distinction is the whole point of the bool/out-parameter pair. */
    networldInit();
    bs_put_u16(wg + 1, 0u);
    networldApplyPayload(wg, sizeof wg);
    gen = 0xDEADBEEFu;
    check(networldServerGenVersion(&gen) && gen == 0u,
          "a declaration of 0 is a declaration, not silence");
    check(networldServerGenVersion(NULL), "and the out-parameter is optional");

    /* Short: malformed, dropped whole, same posture as every other decoder in this module. */
    networldInit();
    uint8_t truncated_gen[BS_WORLD_GEN_BYTES - 1];
    truncated_gen[0] = BS_APP_WORLD_GEN;
    memset(truncated_gen + 1, 0xFF, sizeof truncated_gen - 1);
    networldApplyPayload(truncated_gen, sizeof truncated_gen);
    check(!networldServerGenVersion(NULL), "a short WORLD_GEN is dropped, not misparsed");

    /* Long, which is the case that actually protects the NEXT protocol change. If a later
     * server widens this message, this client must refuse it outright rather than read the
     * first two bytes of a record whose meaning it does not know. Reading them is worse than
     * dropping them: dropping resolves to legacy and refuses on mismatch, half-parsing enters
     * a world on a number it invented. */
    networldInit();
    uint8_t widened[BS_WORLD_GEN_BYTES + 1];
    widened[0] = BS_APP_WORLD_GEN;
    bs_put_u16(widened + 1, 2u);
    widened[BS_WORLD_GEN_BYTES] = 0x7F;
    networldApplyPayload(widened, sizeof widened);
    check(!networldServerGenVersion(NULL),
          "a LONGER WORLD_GEN is dropped whole rather than half-parsed for its first two bytes");

    /* Per-session, like the seed beside it. A second join in the same boot — quit to title,
     * join a different server — must not inherit the first server's declaration; that is a
     * mismatch nobody would ever be told about. */
    networldInit();
    bs_put_u16(wg + 1, 2u);
    networldApplyPayload(wg, sizeof wg);
    check(networldServerGenVersion(NULL), "declared in this session");
    networldInit();
    check(!networldServerGenVersion(NULL),
          "and gone in the next: a second join does not inherit the first server's generator");
}

/* ------------------------------------------- the server's clock (v1.8.20) -----------------
 *
 * BS_APP_TIME_SYNC, 0x10, one uint64 LE, sent on join and then once a second. The design was
 * written down in world/daynight.h:338-365 long before it was built, and this suite pins the
 * parts of it that live in this module: the length posture, the byte order, the full width,
 * and the one behaviour that is unlike everything else in this file — the accessor CONSUMES.
 *
 * Why consuming is the property worth a test rather than a comment: the client advances its
 * own clock every tick and the server corrects it once a second. A latching accessor would let
 * main.c re-pin the clock every frame to a value up to a second old, and time would stop
 * moving between packets. That bug cannot be seen in a screenshot, it cannot be seen in a
 * frame time, and it looks exactly like a working feature right up until somebody waits for a
 * sunrise that never arrives. So the difference between "told us, ever" and "told us again" is
 * asserted here, both ways round, rather than trusted to the name.
 */
static void test_time_sync_carries_the_servers_clock(void)
{
    puts("BS_APP_TIME_SYNC hands the server's day/night counter to the client, once per packet");

    networldInit();

    /* Poisoned with a value no counter under test will produce, for the same reason the seed
     * and generator cases above poison theirs: 0 is a legitimate counter (it is midnight of day
     * zero), so "nothing said" cannot be encoded as a value. */
    uint64_t t = 0xDEADBEEFCAFEF00DULL;
    check(!networldTakeTimeSync(&t), "no time is claimed before the server sends one");
    check(t == 0xDEADBEEFCAFEF00DULL, "a false return does not write the out-parameter");

    uint8_t ts[BS_TIME_SYNC_BYTES];
    ts[0] = BS_APP_TIME_SYNC;
    bs_put_u64(ts + 1, 123456u);
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(&t), "the counter is available once TIME_SYNC arrives");
    check(t == 123456u, "and it is the exact value the server sent");

    /* THE DEFINING PROPERTY. One packet, one delivery. Everything else in this module latches;
     * this must not, and a reader who assumed it did would write a clock that never moves. */
    t = 0xDEADBEEFCAFEF00DULL;
    check(!networldTakeTimeSync(&t),
          "a second take with no new packet answers false: the pending flag was CONSUMED");
    check(t == 0xDEADBEEFCAFEF00DULL, "and that false leaves the out-parameter alone too");

    /* ...and consuming must not latch it OFF either, which is the mirror-image bug and just as
     * silent: the clock would sync exactly once, on join, and then drift for the rest of the
     * session. A second packet has to re-arm it. */
    bs_put_u64(ts + 1, 999u);
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(&t) && t == 999u,
          "a later packet re-arms it: consuming does not disable the feature for the session");

    /* Full width, and this is not ceremony. The counter is uint64 in world/daynight.h, uint64
     * on the wire and uint64 here, and at 20 ticks a second a 32-bit counter would wrap after
     * about six and a half years of server uptime -- far enough away to never be found by
     * playing, close enough to be real. A value with bits set ABOVE bit 31 and a distinct low
     * half goes red the moment anyone narrows this to uint32 anywhere along the path. */
    networldInit();
    bs_put_u64(ts + 1, 0x0000BEEF12345678ULL);
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(&t) && t == 0x0000BEEF12345678ULL,
          "the counter survives at full 64-bit width, high half intact");

    /* Byte order, asserted against literal bytes rather than through bs_put_u64 -- a round trip
     * through the matching putter and getter agrees with itself whichever order it uses, so it
     * can only catch a decoder that disagrees with ITSELF, never one that disagrees with the
     * server. All eight bytes distinct, so any permutation gives a different answer. */
    networldInit();
    ts[0] = BS_APP_TIME_SYNC;
    ts[1] = 0x08; ts[2] = 0x07; ts[3] = 0x06; ts[4] = 0x05;
    ts[5] = 0x04; ts[6] = 0x03; ts[7] = 0x02; ts[8] = 0x01;
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(&t) && t == 0x0102030405060708ULL,
          "the eight payload bytes are read little-endian, low byte first");

    /* 0 is a real counter -- midnight of day zero, which every fresh world starts at -- and has
     * to arrive AS 0 rather than being mistaken here for silence. The bool/out-parameter pair
     * exists precisely so those two are not the same thing. */
    networldInit();
    bs_put_u64(ts + 1, 0u);
    networldApplyPayload(ts, sizeof ts);
    t = 0xDEADBEEFCAFEF00DULL;
    check(networldTakeTimeSync(&t) && t == 0u, "a counter of 0 is a time, not silence");

    networldInit();
    bs_put_u64(ts + 1, 7u);
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(NULL), "and the out-parameter is optional");
    check(!networldTakeTimeSync(NULL), "a NULL take consumes exactly like a non-NULL one");

    /* Short: malformed, dropped whole, same posture as every other decoder in this module. */
    networldInit();
    uint8_t truncated_ts[BS_TIME_SYNC_BYTES - 1];
    truncated_ts[0] = BS_APP_TIME_SYNC;
    memset(truncated_ts + 1, 0xFF, sizeof truncated_ts - 1);
    networldApplyPayload(truncated_ts, sizeof truncated_ts);
    check(!networldTakeTimeSync(NULL), "a short TIME_SYNC is dropped, not misparsed");

    /* Long, which is the case that protects the NEXT protocol change. Half-parsing a widened
     * record would set the world's clock from the first eight bytes of a message whose meaning
     * this build does not know -- worse than ignoring it, because ignoring it leaves the local
     * clock running correctly and half-parsing moves it somewhere invented. */
    networldInit();
    uint8_t widened_ts[BS_TIME_SYNC_BYTES + 1];
    widened_ts[0] = BS_APP_TIME_SYNC;
    bs_put_u64(widened_ts + 1, 4242u);
    widened_ts[BS_TIME_SYNC_BYTES] = 0x7F;
    networldApplyPayload(widened_ts, sizeof widened_ts);
    check(!networldTakeTimeSync(NULL),
          "a LONGER TIME_SYNC is dropped whole rather than half-parsed for its first eight bytes");

    /* Per-session, and this arm guards single player specifically. networldInit() is reached by
     * netDisconnect(), so a counter left pending from a server would otherwise be handed to the
     * next session -- including the one with no server in it, where it would drag the player's
     * own saved world to whatever o'clock that server happened to be at. */
    networldInit();
    bs_put_u64(ts + 1, 55555u);
    networldApplyPayload(ts, sizeof ts);
    networldInit();
    check(!networldTakeTimeSync(NULL),
          "a pending counter does not survive into the next session, or into single player");

    /* An unsolicited TIME_SYNC with no join ahead of it is still just data: recorded, not
     * judged, exactly like WORLD_GEN beside it. This module's job ends at "the server said N";
     * nothing here is a gate and nothing here refuses a world. */
    networldInit();
    bs_put_u64(ts + 1, 61u);
    networldApplyPayload(ts, sizeof ts);
    check(networldTakeTimeSync(&t) && t == 61u,
          "an unsolicited TIME_SYNC is recorded rather than treated as an error");

    /* Control: the dispatch really is keyed on the type byte, and 0x10 really is the one that
     * reaches this decoder. Without this, every check above would pass just as happily if
     * applyTimeSync were wired to the wrong case -- or to none, with some other handler
     * incidentally setting the flag. */
    networldInit();
    ts[0] = (uint8_t)(BS_APP_TIME_SYNC + 1u);
    bs_put_u64(ts + 1, 77u);
    networldApplyPayload(ts, sizeof ts);
    check(!networldTakeTimeSync(NULL),
          "a neighbouring type byte does not reach the clock decoder");
}

/* The entry gate's fourth term, against the clock. scene/title_nav_test.c checks that
 * titleMpNav() honours the bool; this checks that the bool is TRUE when it should be and,
 * more importantly, that every arm of it is BOUNDED — a gate on a packet a pre-v1.8.3 server
 * never sends is a title screen with no way into the world at all. Same shape and same
 * argument as test_registry_gate_holds_entry_until_the_table_settles() below. */
static void test_gen_gate_holds_entry_until_the_generator_is_known(void)
{
    puts("the generator gate holds world entry for one grace, and every arm of it is bounded");

    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);
    check(!networldGenWaiting(), "a client that has not joined anything is never held");

    /* A WORLD_GEN with no WORLD_INFO ahead of it. Nothing arms, so nothing waits — this is
     * what keeps a single-player boot from being delayed by a stray datagram. */
    uint8_t wg[BS_WORLD_GEN_BYTES];
    wg[0] = BS_APP_WORLD_GEN;
    bs_put_u16(wg + 1, 1u);
    networldApplyPayload(wg, sizeof wg);
    check(!networldGenWaiting(),
          "an unsolicited WORLD_GEN does not put a boot that never joined anything on hold");

    /* Joined: the seed is in, the declaration has not landed yet. This is the one-frame window
     * between the two packets, and it is the whole reason the gate exists. */
    networldInit();
    fakeNowSet(1000);
    /* Built by hand rather than through buildWorldInfoMsg(): that helper is defined with
     * the registry scenarios further down this file and is not in scope up here. */
    uint8_t wi[BS_WORLD_INFO_BYTES];
    wi[0] = BS_APP_WORLD_INFO;
    bs_put_u32(wi + 1, 99u);
    networldApplyPayload(wi, sizeof wi);
    check(networldGenWaiting(), "the seed alone holds entry while WORLD_GEN could still land");

    networldApplyPayload(wg, sizeof wg);
    check(!networldGenWaiting(), "the declaration releases entry on the frame it lands");
    check(networldServerGenVersion(NULL), "with the declaration recorded, not merely awaited");

    /* A pre-v1.8.3 server never sends one at all. Released at the grace, having heard nothing
     * — which world/genversion.h's genVersionForSessionResolve() resolves to legacy, the
     * generator such a server's clients have always run. */
    networldInit();
    fakeNowSet(1000);
    networldApplyPayload(wi, sizeof wi);
    fakeNowAdvance(NETWORLD_GEN_GRACE_MS - 1);
    check(networldGenWaiting(), "still held one millisecond before the grace expires");
    fakeNowAdvance(1);
    check(!networldGenWaiting(), "a pre-v1.8.3 server stops holding entry at the grace");
    check(!networldServerGenVersion(NULL),
          "and the client enters having heard nothing, which resolves to legacy rather than "
          "to a refusal");

    /* Backwards, too. bsSockNowMs() is osGetTime() on hardware — a wall clock that can step
     * back over a lid-close or a clock change. The bound is `now - base` on uint64_t, so a
     * backwards step underflows to a huge value and releases rather than traps. */
    networldInit();
    fakeNowSet(10000);
    networldApplyPayload(wi, sizeof wi);
    check(networldGenWaiting(), "held while the clock is behaving");
    fakeNowSet(9999);
    check(!networldGenWaiting(), "a clock that steps backwards releases, never traps");

    /* And a chatty server cannot push the origin out. registryGateArm() latches, which is what
     * makes this true for both gates riding it; without the latch a server that resent
     * WORLD_INFO every 200 ms would hold this console on the title screen indefinitely. */
    networldInit();
    fakeNowSet(1000);
    networldApplyPayload(wi, sizeof wi);
    fakeNowAdvance(NETWORLD_GEN_GRACE_MS - 10);
    networldApplyPayload(wi, sizeof wi);
    fakeNowAdvance(10);
    check(!networldGenWaiting(), "a repeated WORLD_INFO does not extend the grace");
}

/* The compatibility claim this whole phase rests on, checked rather than argued.
 *
 * BS_APP_WORLD_GEN is server-to-client only, and the argument for shipping it is that a client
 * older than v1.8.3 has no case for the id and skips it. That argument is about code that is
 * not in this build and cannot be linked here — but the property it depends on IS in this
 * build and is the same one line of code: networldApplyPayload()'s default arm. So what is
 * checked here is that arm, driven with an id this build genuinely has no case for.
 *
 * 0x10 is chosen because it is the next id after BS_APP_WORLD_GEN. If a later phase allocates
 * it, this check starts exercising a handled type and stops meaning what it says — so it will
 * need moving to whatever the next free id is then, not deleting.
 *
 * Note what is NOT claimed: this does not prove a v1.8.2 binary is unaffected. It proves the
 * mechanism that makes it so, in the file that implements it. The real cross-version evidence
 * is ordering — the server ships first — and that is a release process, not a test. */
static void test_an_unhandled_app_type_changes_nothing(void)
{
    puts("an application type this build has no case for is skipped, not acted on");

    networldInit();
    fakeTransportReset();
    fakeNowSet(1000);

    /* Built by hand rather than through buildWorldInfoMsg(): that helper is defined with
     * the registry scenarios further down this file and is not in scope up here. */
    uint8_t wi[BS_WORLD_INFO_BYTES];
    wi[0] = BS_APP_WORLD_INFO;
    bs_put_u32(wi + 1, 4242u);
    networldApplyPayload(wi, sizeof wi);
    uint8_t wg[BS_WORLD_GEN_BYTES];
    wg[0] = BS_APP_WORLD_GEN;
    bs_put_u16(wg + 1, 1u);
    networldApplyPayload(wg, sizeof wg);

    /* Past both graces, so the two gates are already released and a re-arm would be visible as
     * a gate going back up rather than being masked by one that was still up anyway. */
    fakeNowAdvance(NETWORLD_REG_INFO_GRACE_MS);

    const int msgs_before  = networldRecvMsgs();
    const int edits_before = networldAppliedEdits();
    fake_sent_calls = 0;

    uint8_t future[8];
    future[0] = 0x10;
    memset(future + 1, 0xA5, sizeof future - 1);
    networldApplyPayload(future, sizeof future);

    check(networldRecvMsgs() == msgs_before + 1,
          "it is counted as received, so the packet was really delivered to the dispatcher");
    check(fake_sent_calls == 0, "and answered with nothing: no kick, no complaint, no retry");
    check(networldAppliedEdits() == edits_before, "no world edit came out of it");

    uint32_t seed = 0u;
    uint32_t gen  = 0u;
    check(networldWorldSeed(&seed) && seed == 4242u, "the seed already learned is untouched");
    check(networldServerGenVersion(&gen) && gen == 1u,
          "so is the generator already declared");
    check(!networldGenWaiting() && !networldRegistryWaiting(),
          "and neither gate was pushed back up by it");
}

/* world/genversion.h's genVersionForSessionResolve() and world/genrefuse.h's refusal sentence:
 * the decision main.c acts on, exercised here rather than in world_test.c because the inputs
 * come off the wire and this is the suite that has the wire in it.
 *
 * Three rows, and the third is the one this phase exists for. The first two are what keep the
 * third from being satisfiable by a function that simply refuses everything. */
static void test_the_session_generator_resolves_and_refuses(void)
{
    puts("the declared generator resolves to a session generator, or to a refusal");

    uint32_t out = 0xDEADBEEFu;

    /* Row 1: the server said nothing — every server older than v1.8.3, forever. Not a fault
     * and not a refusal, because it has exactly one correct answer. */
    check(genVersionForSessionResolve(false, 0u, &out) == GENVER_OK,
          "a server that said nothing is not a refusal");
    check(out == genVersionForSession(),
          "and the session generates what a joined session has always generated");
    check(out == GEN_VERSION_LEGACY,
          "which is legacy: the generator every pre-v1.8.3 server's clients ran");
    check(genVersionRefusalText(GENVER_OK) == NULL, "GENVER_OK has nothing to say to the player");

    /* Row 2: a declaration this build knows. Taken verbatim — the wire value is the answer,
     * not a hint that gets re-derived locally. */
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, GEN_VERSION_LEGACY, &out) == GENVER_OK
              && out == GEN_VERSION_LEGACY,
          "a declared LEGACY is accepted and used verbatim");
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, GEN_VERSION_DENSITY, &out) == GENVER_OK
              && out == GEN_VERSION_DENSITY,
          "and so is a declared DENSITY: the test is what this build KNOWS, not which "
          "generator Phase 4's server happens to declare");

    /* Row 2b: the rest of what this build knows, and the TOP of the range in particular.
     *
     * Row 2 stops at DENSITY, which was the whole of the range when Phase 4 shipped. The
     * generator has grown three versions since — BIOME, CAVES, ORES (world/genversion.h) —
     * and until this row nothing asserted that any of them survives the trip. The gap is not
     * theoretical: a server declaring ORES is the reason this row was written, and if
     * genVersionKnown()'s ceiling ever regressed to DENSITY, every check above would stay
     * green while every client refused that server at the join screen. Row 3 would not catch
     * it either — it probes NEWEST + 1, which is refused correctly in both worlds.
     *
     * The NEWEST identity check at the end is what keeps this row honest as the enum grows.
     * It is not decoration: append a sixth generator and it goes red, which forces whoever
     * appended it to extend this row rather than leave the newest generator — the one a
     * server will actually declare — as the only one nothing here tests. */
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, GEN_VERSION_BIOME, &out) == GENVER_OK
              && out == GEN_VERSION_BIOME,
          "a declared BIOME is accepted and used verbatim");
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, GEN_VERSION_CAVES, &out) == GENVER_OK
              && out == GEN_VERSION_CAVES,
          "and so is a declared CAVES");
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, GEN_VERSION_ORES, &out) == GENVER_OK
              && out == GEN_VERSION_ORES,
          "and so is a declared ORES, which is the generator a current server declares");
    check(GEN_VERSION_ORES == GEN_VERSION_NEWEST,
          "and ORES is still the newest this build has, so the check above is testing the top "
          "of the range and not a value the enum has since overtaken");

    /* Row 3: a generator this build cannot produce. */
    out = 0xDEADBEEFu;
    const GenVersionStatus st =
        genVersionForSessionResolve(true, GEN_VERSION_NEWEST + 1u, &out);
    check(st == GENVER_SESSION_MISMATCH,
          "a generator newer than this build knows is a session mismatch");
    check(genVersionRefusalText(st) != NULL,
          "which is a refusal, so main.c stops instead of entering the wrong world");
    check(strcmp(genVersionRefusalText(st), "server world needs newer game") == 0,
          "with the sentence that points at the game, which is the thing the player can fix");
    check(strcmp(genVersionRefusalText(st), genVersionRefusalText(GENVER_TOO_NEW)) != 0,
          "and not TOO_NEW's, which would send them hunting a local world that is not the "
          "problem");
    check(out == GEN_VERSION_LEGACY,
          "the out-parameter is left at a safe value rather than at the number it refused");

    /* 0 travels the same road. It is a value the wire can carry and the encoding cannot
     * reject, so the refusal has to be the thing that catches it. */
    out = 0xDEADBEEFu;
    check(genVersionForSessionResolve(true, 0u, &out) == GENVER_SESSION_MISMATCH,
          "a declaration of 0 is an unknown generator, not 'nothing said'");

    check(genVersionForSessionResolve(true, GEN_VERSION_LEGACY, NULL) == GENVER_OK,
          "the out-parameter is optional");
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

/* ---- a remote break must clear the broken block's per-position state -------------------- */
/* v1.8.17. main.c's LOCAL break path removes the blockstate record for the cell it has just
 * emptied (source/main.c, the `if (it.broke_valid)` block). The REMOTE path had no such step:
 * a furnace another player broke vanished from the world here while its record stayed in the
 * table. Two costs, both real. BLOCKSTATE_SLOTS is 64, so orphaned records fill a fixed pool
 * and legitimate new furnaces silently get no state; and a furnace later placed on the same
 * coordinate reads the DEAD one's contents back out of blockStateGet -- which, now that
 * breaking a furnace pays its contents into the inventory, is an item duplication route
 * rather than merely stale data.
 *
 * The fix cannot live in net/networld.c, and this binary could not link it if it did. See
 * net/networld.h's own comment on NetworldEditFn: this module never reaches into world/'s
 * state tables, which is exactly what keeps it host-testable, and the table itself is
 * main.c's `s_blockstate` file static. So the behavioural half below proves what networld.c
 * really owns -- the break lands, and the hook carries the cell's own coordinate out to the
 * table's owner -- and the source-text half proves that owner acts on it. That second
 * technique is app/session_test.c's, which reads source/main.c the same way to check the
 * session_start lap really calls sessionBegin(); a behavioural test of the hook alone stays
 * green against a game in which nothing ever clears the record. */
static char *readWholeSourceFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void test_remote_break_of_a_stateful_block_clears_its_state(void)
{
    puts("a remote break of a stateful block drops it AND clears its blockstate record");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    hookReset();
    networldSetEditHook(hookRecord, &hook_userdata_marker);

    worldColumnCreate(&w, 0, 0);              /* column (0,0): x,z in 0..15 */
    worldSet(&w, 6, 12, 9, BLOCK_FURNACE);    /* the stateful block, as placed */
    check(worldGet(&w, 6, 12, 9) == BLOCK_FURNACE, "fixture: a furnace stands at (6,12,9)");

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 6, 12, 9, BLOCK_AIR);
    networldApplyPayload(msg, sizeof msg);

    check(worldGet(&w, 6, 12, 9) == BLOCK_AIR, "the remote break emptied the cell");
    check(hook_count == 1 && hook_log[0].x == 6 && hook_log[0].y == 12 && hook_log[0].z == 9,
          "the edit hook carried the broken cell's own coordinate to the table's owner");

    networldSetEditHook(NULL, NULL);

    /* Run from the repository root, the same working directory app/session_test.c reads
     * source/main.c out of. */
    char *src = readWholeSourceFile("source/main.c");
    check(src != NULL, "source/main.c could be read");
    if (src == NULL) {
        printf("  (source/main.c could not be read from this working directory)\n");
        return;
    }

    check(strstr(src, "#include \"world/blockstate.h\"") != NULL,
          "control: main.c owns the blockstate table at all");
    check(strstr(src, "blockStateRemove(&s_blockstate, it.broke_x, it.broke_y, it.broke_z)") != NULL,
          "control: the LOCAL break path still removes a record, so this search shape finds hits");

    const char *fn = strstr(src, "static void onRemoteEdit(");
    check(fn != NULL, "main.c still has the remote-edit hook this test is about");
    if (fn != NULL) {
        /* Body only. Every statement in onRemoteEdit is a call or an unbraced if, so the
         * first line-initial closing brace after its signature is its own. */
        const char *end = strstr(fn, "\n}");
        check(end != NULL, "the hook's body is delimited");
        if (end != NULL) {
            const size_t body_len = (size_t)(end - fn);
            char *body = (char *)malloc(body_len + 1);
            check(body != NULL, "the body could be copied out for searching");
            if (body != NULL) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';
                check(strstr(body, "blockStateRemove(") != NULL,
                      "the REMOTE break path removes the record too");
                check(strstr(body, "blockStateRemove(&s_blockstate, x, y, z)") != NULL,
                      "and removes it for the cell the remote edit actually named");
                free(body);
            }
        }
    }

    free(src);
}

/* ---- a remote PLACE of a stateful block must create its per-position state --------------- */
/* The other half of test_remote_break_of_a_stateful_block_clears_its_state above. main.c's
 * LOCAL place path (the `if (it.placed_valid && it.placed_id == BLOCK_FURNACE)` block) claims
 * a blockstate slot the moment a furnace lands, so the block is usable the instant it exists.
 * The REMOTE path had no equivalent: onRemoteEdit() unconditionally removes whatever record
 * stood at the cell (the fix above), but nothing ever recreates one for a furnace another
 * player just placed. The furnace exists as a block -- worldGet() answers BLOCK_FURNACE -- but
 * blockStateGet() on it answers false forever, because no blockStateCreate() call is reachable
 * from this path. Interacting with such a furnace has nothing to read or write.
 *
 * This is not a missing capability in net/networld.c: net/networld.h's own header comment on
 * NetworldEditFn already promises "the block is already readable with worldGet() by the time
 * the hook runs" -- true today, unchanged by this test -- so onRemoteEdit() already has every
 * fact it needs (s_world, x, y, z) to look the new block up and act on it. The gap is entirely
 * that main.c's onRemoteEdit() never does. Same split as the test above: the behavioural half
 * proves networld.c hands the hook a coordinate whose block is already readable; the
 * source-text half proves the table's owner (main.c) acts on it for a furnace. */
static void test_remote_place_of_a_furnace_creates_its_state(void)
{
    puts("a remote place of a furnace makes the block AND its blockstate record");

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    hookReset();
    networldSetEditHook(hookRecord, &hook_userdata_marker);

    worldColumnCreate(&w, 0, 0);   /* column (0,0): x,z in 0..15 */
    check(worldGet(&w, 4, 20, 8) == BLOCK_AIR, "fixture: nothing stands at (4,20,8) yet");

    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 4, 20, 8, BLOCK_FURNACE);
    networldApplyPayload(msg, sizeof msg);

    check(worldGet(&w, 4, 20, 8) == BLOCK_FURNACE, "the remote place landed the furnace block");
    check(hook_count == 1 && hook_log[0].x == 4 && hook_log[0].y == 20 && hook_log[0].z == 8,
          "the edit hook carried the placed cell's own coordinate to the table's owner, with "
          "the block already readable via worldGet() -- net/networld.h's own promise");

    networldSetEditHook(NULL, NULL);

    /* Run from the repository root, the same working directory app/session_test.c and the
     * break-side test above both read source/main.c out of. */
    char *src = readWholeSourceFile("source/main.c");
    check(src != NULL, "source/main.c could be read");
    if (src == NULL) {
        printf("  (source/main.c could not be read from this working directory)\n");
        return;
    }

    check(strstr(src, "it.placed_id == BLOCK_FURNACE") != NULL,
          "control: the LOCAL place path still gates on BLOCK_FURNACE, so this search shape "
          "finds hits");
    check(strstr(src,
                 "blockStateCreate(&s_blockstate, it.placed_x, it.placed_y, it.placed_z") != NULL,
          "control: the LOCAL place path still creates a record, confirming the control string "
          "above is not stale");

    const char *fn = strstr(src, "static void onRemoteEdit(");
    check(fn != NULL, "main.c still has the remote-edit hook this test is about");
    if (fn != NULL) {
        /* Body only, same delimiting technique as the break-side test above: every statement in
         * onRemoteEdit is a call or an unbraced if, so the first line-initial closing brace
         * after its signature is its own. */
        const char *end = strstr(fn, "\n}");
        check(end != NULL, "the hook's body is delimited");
        if (end != NULL) {
            const size_t body_len = (size_t)(end - fn);
            char *body = (char *)malloc(body_len + 1);
            check(body != NULL, "the body could be copied out for searching");
            if (body != NULL) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';
                check(strstr(body, "BLOCK_FURNACE") != NULL,
                      "THE DEFECT: the REMOTE place path never even names BLOCK_FURNACE, so it "
                      "cannot be gating a create on it the way the local path does");
                check(strstr(body, "blockStateCreate(") != NULL,
                      "THE DEFECT: the REMOTE place path never calls blockStateCreate() at all "
                      "-- a remotely placed furnace gets no state, ever");
                free(body);
            }
        }
    }

    free(src);
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
 * The shape of it: the server replays every diff it holds — it is not the one forgetting —
 * the instant the handshake completes, which is on the title screen,
 * before a world exists. With no world, every entry goes to the pending store, which REFUSES
 * rather than evicts once full. BLOCKDIFF_MAX_PENDING was 256, and the server sends
 * oldest-first, so the first 256 diffs survived and the newest were dropped. A house is the
 * newest thing in the world. Players who never left still have theirs in RAM, which is why
 * it stays visible to them — the asymmetry is the tell.
 *
 * The fix raised BLOCKDIFF_MAX_PENDING from 256 to 65536, far past the 300 replayed below, so
 * the case reproduced here cannot overflow it. That is NOT the same as "a join sync can no longer
 * overflow it", which an earlier version of this comment claimed on the strength of the two
 * caps matching at 65536. They do not match: the server's BS_DIFF_MAX is 131072u
 * (deps/blocksmith-server/game/diffstore.h:39), raised there deliberately in server commit
 * b95f980 on 2026-08-21, and a legacy full-dump sync that big still gets half of it refused —
 * blockdiff_test.c's test_server_replay_overflow measures exactly that. The gap is the design
 * and is not a defect to close: with per-column delivery the console holds edits for loaded
 * columns only, so what it can hold is bounded by a 3DS's memory rather than by the server's,
 * and the two ceilings are meant to come apart.
 *
 * The count below stays at 300 — over the OLD 256 cap, so this test still goes red against
 * the code that had the bug, which is the only reason to keep it.
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
    check(registryCount() == 45, "count moved from 43 to 45 defined rows");
    check(blockIsSolid(REG_ID_DYN_LO), "after sync the same id resolves to the real def");
    check(worldGet(&w, 5, 10, 7) == REG_ID_DYN_LO,
          "the stored raw byte needed no rewrite — tables agree around it");
    /* A `!=` pin, and it needed reading rather than bumping. It fails when the table
     * carrying two extra dynamic rows hashes IDENTICALLY to the core-only table — i.e.
     * when registryCrc16() has stopped seeing the dynamic range at all. Leaving the old
     * 0x4066 here would not have failed anything; it would have compared against a number
     * that is no longer any table's fingerprint, so the check could never go red for the
     * reason it exists. Updating the literal to the measured core-only value is what KEEPS
     * it armed; that is the opposite of neutralising it.
     *
     * 0x4066 -> 0x189B on 2026-08-30 by v1.8.3 Phase 3, same move as registry_test.c:423
     * and for the same measured reason: five more 28-byte records now feed the hash.
     *
     * 0x189B -> 0xBDC5 on 2026-09-02 by v1.8.8, and this one is a DIFFERENT shape again: no
     * record was added or removed (registryCount() stays 15), one byte inside the cactus
     * record changed value — .hardness 8 -> 9. Leaving 0x189B here would have been the
     * quiet failure this comment already warns about: the `!=` would still pass, against a
     * fingerprint no table has any more.
     *
     * 0xBDC5 -> 0xD236 on 2026-09-02 by v1.8.8's per-biome blocks and plants: twelve records
     * appended (registryCount() moves 15 -> 27), same shape of move as Phase 3's above. Left at
     * 0xBDC5 this `!=` would still have passed — the converged table (core + 2 dynamic rows)
     * was never going to collide with either golden by chance — but it would have been proving
     * inequality against a fingerprint that is nobody's core-only table any more, which is
     * exactly the silently-disarmed case this comment exists to name.
     *
     * 0xD236 -> 0x165E on 2026-09-02 by v1.8.10's floor-standing torch (id 27, hardness 1,
     * luminance 14, BLOCK_SHAPE_CROSS, REG_FLAG_TRANSPARENT|REG_FLAG_LUMINOUS, tex0=TILE_TORCH):
     * one more 28-byte record appended (registryCount() moves 27 -> 28), same shape of move as
     * the two above it. Measured, not guessed: a scratchpad probe linking the real
     * world/registry.c + world/block.c calls registryInitCore() and prints registryCrc16(),
     * printing `count=28 crc=0x165E`; a second probe linking only the vendored
     * deps/blocksmith-server/game/world/registry.c printed the identical `count=28 crc16=0x165E
     * rev=1`. Left at 0xD236 this `!=` would still have passed for the same reason as every prior
     * move — the converged table was never going to collide with either golden by chance — but it
     * would again be proving inequality against a fingerprint that is nobody's core-only table
     * any more.
     *
     * 0x165E -> 0xE15E on 2026-09-02/03 by v1.8.12 "Ores"'s six ore rows (ids 28..33). Same
     * shape of move as the torch move directly above: one more record range appended
     * (registryCount() moves 28 -> 34), same reasoning about the `!=` check. Left at 0x165E
     * this check would have kept passing — the converged table (34 core + 2 dynamic rows)
     * was never going to collide with either golden by chance — but it would have been the
     * exact silently-disarmed case this whole comment exists to name: 0x165E is not any
     * table's core-only fingerprint any more, 0xE15E is.
     *
     * 0xE15E -> 0x9610 on 2026-09-03 by v1.8.14 "Animals"'s four raw-meat rows (ids 34..37,
     * lane ANIMAL-B). Same shape of move as the ore move directly above: one more record range
     * appended (registryCount() moves 34 -> 38), same reasoning about the `!=` check. Left at
     * 0xE15E it would have kept passing — the converged table (38 core + 2 dynamic rows) was
     * never going to collide with either golden by chance — and that pass would have been
     * worthless, because 0xE15E stopped being any table's core-only fingerprint the moment the
     * meat rows landed.
     *
     * MEASURED, not fitted. A scratchpad probe (animb_meat_crc_probe.c) that links the real
     * world/registry.c and world/block.c and NO test file — so no golden constant is reachable
     * from the binary — calls registryInitCore() and prints registryCrc16(): `count=38
     * crc=0x9610 rev=1`. A second probe built the same way against the vendored
     * deps/blocksmith-server/game/world/registry.c printed the identical `count=38 crc16=0x9610
     * rev=1`, which is the client/server lockstep networld.c:322-328 rejects a join over.
     *
     * 0x9610 -> 0xE486 on 2026-09-03 by v1.8.15 "Furnace"'s five rows (four cooked meats and the
     * furnace, ids 38..42, lane FURNACE-C). Same shape of move as the meat move directly above:
     * a record range appended, registryCount() moving 38 -> 43.
     *
     * READ THE FAILURE LIST BEFORE BELIEVING THIS ONE WAS FOUND THE EASY WAY. It was not. The
     * full suite run that caught this file reported TWENTY failed checks and this check was NOT
     * among them -- it passed, exactly as every paragraph above predicts a stale `!=` golden
     * will. The twenty that went red were all registryCount() pins, which state a number and so
     * fail loudly when the number moves. This one states an INEQUALITY against a number that
     * stopped being any table's fingerprint, and an inequality against a meaningless constant is
     * satisfied by everything. It was moved because the count pins beside it moved, not because
     * anything complained -- which is the entire reason this comment keeps being extended
     * instead of being trimmed down to the current value.
     *
     * MEASURED, not fitted, and not taken on the word of the lane that added the rows: a probe
     * linking world/registry.c and world/crc32.c alone -- no test file in the binary, so no
     * golden constant is even reachable from what was measured -- printed `count=43 crc=0xE486
     * rev=1`. The same probe built against the vendored deps/blocksmith-server copy printed the
     * identical numbers, and a third arm built from `git show HEAD:` (pre-sync) printed
     * `count=38 crc=0x9610 rev=1`, reproducing the OLD golden and proving the probe can tell the
     * two tables apart rather than printing whatever it was pointed at. */
    check(registryCrc16() != 0xE486u,
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
    check(registryCount() == 43 && registryFind("bad_a") == 0,
          "a truncated DEFS applies nothing");

    /* Claiming more records than the sender's own cap can carry. */
    uint8_t greedy[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(greedy, batch, sizeof greedy);
    greedy[2] = BS_APP_REGISTRY_DEFS_MAX_N + 1u;
    networldApplyPayload(greedy, sizeof greedy);
    check(registryCount() == 43, "an over-cap count is refused outright");

    /* first below the dyn range would overwrite compiled-in core rows. */
    uint8_t hostile[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(hostile, batch, sizeof hostile);
    hostile[1] = BLOCK_STONE;
    networldApplyPayload(hostile, sizeof hostile);
    check(registryCount() == 43 && strcmp(blockInfo(BLOCK_STONE)->name, "stone") == 0,
          "a batch aimed at the core range changes nothing");

    /* A record whose embedded id disagrees with its slot position: all-or-nothing. */
    uint8_t shuffled[BS_APP_REGISTRY_DEFS_BYTES(2)];
    memcpy(shuffled, batch, sizeof shuffled);
    shuffled[4] = REG_ID_DYN_LO + 1u;   /* record 0 claims id 0x81 */
    networldApplyPayload(shuffled, sizeof shuffled);
    check(registryCount() == 43 && registryFind("bad_a") == 0 && registryFind("bad_b") == 0,
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

/* The forty-three core rows as world/block.h's own constants spell them, written out here so
 * the core-only fingerprint below can be DERIVED rather than recorded. This is deliberately not
 * read out of world/registry.c's kCoreDefs (it is static there anyway): a reference built from
 * the table under test would move whenever that table moved, and a pasted hash literal — which
 * is what this replaced — would pin whatever the table happened to hash to on the day it was
 * recorded, wrong answer included. Two independent spellings of the same forty-three rows can
 * disagree;
 * a number copied out of a run cannot.
 *
 * The fields the rows never vary are not listed: fluid_class is REG_FLUID_NONE on every core row
 * (water included — REG_FLAG_LIQUID is what makes it a liquid, and Phase B is what will make
 * fluid_class mean anything), and variant_of is the row's own id, because a core row is its own
 * variant base. registryInitCore() sets both itself rather than taking them from the definition;
 * coreOnlyCrc16() below says so in bytes.
 *
 * luminance is 0 on every core row EXCEPT the torch (id 27, luminance 14 — its own dedicated
 * block below, added 2026-09-02 by v1.8.10 "Light"). Before the torch this field never varied
 * either and CoreRowSpec had no member for it; the struct grew a luminance field the day the
 * first row needed one, rather than reserving it speculatively ahead of time. */
typedef struct {
    uint8_t     id;
    const char *name;
    uint8_t     tex[BLOCK_FACES];
    uint8_t     flags;
    uint8_t     hardness;
    uint8_t     luminance;   /* 0 for every row here except the torch's own entry below */
} CoreRowSpec;

static const CoreRowSpec kCoreRowSpecs[] = {
    { 0, "air",   { 0, 0, 0, 0, 0, 0 }, REG_FLAG_TRANSPARENT, 0, 0 },
    { 1, "grass", { BTEX_GRASS_SIDE, BTEX_GRASS_SIDE, BTEX_GRASS_TOP,
                    BTEX_DIRT,       BTEX_GRASS_SIDE, BTEX_GRASS_SIDE }, REG_FLAG_SOLID, 12, 0 },
    { 2, "dirt",  { BTEX_DIRT, BTEX_DIRT, BTEX_DIRT,
                    BTEX_DIRT, BTEX_DIRT, BTEX_DIRT }, REG_FLAG_SOLID, 12, 0 },
    { 3, "stone", { BTEX_STONE, BTEX_STONE, BTEX_STONE,
                    BTEX_STONE, BTEX_STONE, BTEX_STONE }, REG_FLAG_SOLID, 45, 0 },
    { 4, "sand",  { BTEX_SAND, BTEX_SAND, BTEX_SAND,
                    BTEX_SAND, BTEX_SAND, BTEX_SAND }, REG_FLAG_SOLID, 10, 0 },
    { 5, "wood",  { BTEX_WOOD_SIDE, BTEX_WOOD_SIDE, BTEX_WOOD_TOP,
                    BTEX_WOOD_TOP,  BTEX_WOOD_SIDE, BTEX_WOOD_SIDE }, REG_FLAG_SOLID, 40, 0 },
    { 6, "leaves", { BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES,
                     BTEX_LEAVES, BTEX_LEAVES, BTEX_LEAVES },
      REG_FLAG_SOLID | REG_FLAG_TRANSPARENT, 4, 0 },
    { 7, "planks", { BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS,
                     BTEX_PLANKS, BTEX_PLANKS, BTEX_PLANKS }, REG_FLAG_SOLID, 40, 0 },
    { 8, "water",  { BTEX_WATER, BTEX_WATER, BTEX_WATER,
                     BTEX_WATER, BTEX_WATER, BTEX_WATER },
      REG_FLAG_TRANSPARENT | REG_FLAG_LIQUID, 0, 0 },
    { 9, "tall_grass", { BTEX_TALL_GRASS, BTEX_TALL_GRASS, BTEX_TALL_GRASS,
                         BTEX_TALL_GRASS, BTEX_TALL_GRASS, BTEX_TALL_GRASS },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },

    /* v1.8.3 Phase 3's five, ids 10..14. Transcribed from world/block.h's constants the
     * same way every row above was — deliberately NOT copied out of world/registry.c's
     * kCoreDefs, and deliberately not reconciled against it by eye afterwards. If a field
     * below disagrees with the shipped row, coreOnlyCrc16() disagrees with registryCrc16()
     * and the check further down goes red, which is the entire reason this list exists.
     *
     * The three cubes carry REG_FLAG_SOLID alone (ice included: its art is opaque, see
     * world/registry.c's note on the row), the two plants carry TRANSPARENT|SHAPE(CROSS)
     * exactly as tall grass does. Hardness: snow 8, ice 10, cactus 9, plants 1.
     *
     * Cactus moved 8 -> 9 on 2026-09-02 by v1.8.8, which is the first release in which that
     * byte is ever READ: until the item ceiling was widened the block could not be broken at
     * all, so its hardness was a number nothing consumed. 9 sits deliberately between snow's
     * 8 and ice's 10 so the three desert/tundra cubes have three distinct break times.
     * Transcribed here from that intent, NOT read back out of world/registry.c — which is the
     * whole point of this list. */
    { 10, "snow",   { BTEX_SNOW, BTEX_SNOW, BTEX_SNOW,
                      BTEX_SNOW, BTEX_SNOW, BTEX_SNOW }, REG_FLAG_SOLID, 8, 0 },
    { 11, "ice",    { BTEX_ICE, BTEX_ICE, BTEX_ICE,
                      BTEX_ICE, BTEX_ICE, BTEX_ICE }, REG_FLAG_SOLID, 10, 0 },
    { 12, "cactus", { BTEX_CACTUS, BTEX_CACTUS, BTEX_CACTUS,
                      BTEX_CACTUS, BTEX_CACTUS, BTEX_CACTUS }, REG_FLAG_SOLID, 9, 0 },
    { 13, "dead_bush", { BTEX_DEAD_BUSH, BTEX_DEAD_BUSH, BTEX_DEAD_BUSH,
                         BTEX_DEAD_BUSH, BTEX_DEAD_BUSH, BTEX_DEAD_BUSH },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 14, "fern",   { BTEX_FERN, BTEX_FERN, BTEX_FERN,
                      BTEX_FERN, BTEX_FERN, BTEX_FERN },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },

    /* v1.8.8's twelve, ids 15..26: per-biome timber and flora. Transcribed from
     * world/registry.c's kCoreDefs the same way every row above was — independently, from the
     * block.h constants and the row comments' own stated intent, not copied out of the table
     * this function exists to check. Hardness: birch log/planks 36, spruce log/planks 44,
     * birch leaves 3, spruce leaves 5, apple 2, and 1 for every CROSS plant (tall grass top,
     * the four flowers) — the floor mining.c holds every CROSS plant to already. */
    { 15, "birch_log", { BTEX_BIRCH_LOG_SIDE, BTEX_BIRCH_LOG_SIDE, BTEX_BIRCH_LOG_TOP,
                         BTEX_BIRCH_LOG_TOP,  BTEX_BIRCH_LOG_SIDE, BTEX_BIRCH_LOG_SIDE },
      REG_FLAG_SOLID, 36, 0 },
    { 16, "birch_planks", { BTEX_BIRCH_PLANKS, BTEX_BIRCH_PLANKS, BTEX_BIRCH_PLANKS,
                            BTEX_BIRCH_PLANKS, BTEX_BIRCH_PLANKS, BTEX_BIRCH_PLANKS },
      REG_FLAG_SOLID, 36, 0 },
    { 17, "birch_leaves", { BTEX_BIRCH_LEAVES, BTEX_BIRCH_LEAVES, BTEX_BIRCH_LEAVES,
                            BTEX_BIRCH_LEAVES, BTEX_BIRCH_LEAVES, BTEX_BIRCH_LEAVES },
      REG_FLAG_SOLID | REG_FLAG_TRANSPARENT, 3, 0 },
    { 18, "spruce_log", { BTEX_SPRUCE_LOG_SIDE, BTEX_SPRUCE_LOG_SIDE, BTEX_SPRUCE_LOG_TOP,
                          BTEX_SPRUCE_LOG_TOP,  BTEX_SPRUCE_LOG_SIDE, BTEX_SPRUCE_LOG_SIDE },
      REG_FLAG_SOLID, 44, 0 },
    { 19, "spruce_planks", { BTEX_SPRUCE_PLANKS, BTEX_SPRUCE_PLANKS, BTEX_SPRUCE_PLANKS,
                             BTEX_SPRUCE_PLANKS, BTEX_SPRUCE_PLANKS, BTEX_SPRUCE_PLANKS },
      REG_FLAG_SOLID, 44, 0 },
    { 20, "spruce_leaves", { BTEX_SPRUCE_LEAVES, BTEX_SPRUCE_LEAVES, BTEX_SPRUCE_LEAVES,
                             BTEX_SPRUCE_LEAVES, BTEX_SPRUCE_LEAVES, BTEX_SPRUCE_LEAVES },
      REG_FLAG_SOLID | REG_FLAG_TRANSPARENT, 5, 0 },
    { 21, "tall_grass_top", { BTEX_TALL_GRASS_TOP, BTEX_TALL_GRASS_TOP, BTEX_TALL_GRASS_TOP,
                              BTEX_TALL_GRASS_TOP, BTEX_TALL_GRASS_TOP, BTEX_TALL_GRASS_TOP },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 22, "poppy", { BTEX_POPPY, BTEX_POPPY, BTEX_POPPY,
                    BTEX_POPPY, BTEX_POPPY, BTEX_POPPY },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 23, "daisy", { BTEX_DAISY, BTEX_DAISY, BTEX_DAISY,
                    BTEX_DAISY, BTEX_DAISY, BTEX_DAISY },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 24, "bluebell", { BTEX_BLUEBELL, BTEX_BLUEBELL, BTEX_BLUEBELL,
                        BTEX_BLUEBELL, BTEX_BLUEBELL, BTEX_BLUEBELL },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 25, "orchid", { BTEX_ORCHID, BTEX_ORCHID, BTEX_ORCHID,
                     BTEX_ORCHID, BTEX_ORCHID, BTEX_ORCHID },
      REG_FLAG_TRANSPARENT | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 0 },
    { 26, "apple", { BTEX_APPLE, BTEX_APPLE, BTEX_APPLE,
                    BTEX_APPLE, BTEX_APPLE, BTEX_APPLE },
      REG_FLAG_SOLID, 2, 0 },

    /* v1.8.10 "Light"'s torch, id 27: the first light source, and the first core row this list
     * has ever needed a nonzero luminance field for — see the struct comment above. Transcribed
     * from world/registry.c's own row comment (registry.c:517-543), not copied from its
     * .luminance/.hardness fields: TRANSPARENT|LUMINOUS|SHAPE(CROSS), same as every other
     * transparent CROSS plant above plus LUMINOUS; hardness 1, the shared floor every CROSS row
     * in this table already carries (tall grass, dead bush, fern, tall_grass_top, the four
     * flowers) so the torch reads as "instant, like every other plant" and not a block that was
     * never given a break time; luminance 14, Minecraft's own torch light level and one step
     * below this schema's 4-bit ceiling of 15. */
    { 27, "torch", { BTEX_TORCH, BTEX_TORCH, BTEX_TORCH,
                     BTEX_TORCH, BTEX_TORCH, BTEX_TORCH },
      REG_FLAG_TRANSPARENT | REG_FLAG_LUMINOUS | REG_FLAG_SHAPE(BLOCK_SHAPE_CROSS), 1, 14 },

    /* v1.8.12 "Ores"'s six, ids 28..33 (ORE-BLOCKS lane). Transcribed from world/registry.c's
     * own row comments (registry.c:583-624) and world/block.h's BTEX_* constants, the same way
     * every row above was — independently, not copied out of the table this function exists to
     * check. All six are opaque solid cubes, REG_FLAG_SOLID alone, luminance 0 — no light
     * source among them, unlike the torch just above. Hardness is the six-step depth ladder
     * registry.c pins: coal 60, iron 70, gold 85, redstone 90, lapis 80, diamond 100. */
    { 28, "coal_ore", { BTEX_COAL_ORE, BTEX_COAL_ORE, BTEX_COAL_ORE,
                        BTEX_COAL_ORE, BTEX_COAL_ORE, BTEX_COAL_ORE },
      REG_FLAG_SOLID, 60, 0 },
    { 29, "iron_ore", { BTEX_IRON_ORE, BTEX_IRON_ORE, BTEX_IRON_ORE,
                        BTEX_IRON_ORE, BTEX_IRON_ORE, BTEX_IRON_ORE },
      REG_FLAG_SOLID, 70, 0 },
    { 30, "gold_ore", { BTEX_GOLD_ORE, BTEX_GOLD_ORE, BTEX_GOLD_ORE,
                        BTEX_GOLD_ORE, BTEX_GOLD_ORE, BTEX_GOLD_ORE },
      REG_FLAG_SOLID, 85, 0 },
    { 31, "redstone_ore", { BTEX_REDSTONE_ORE, BTEX_REDSTONE_ORE, BTEX_REDSTONE_ORE,
                            BTEX_REDSTONE_ORE, BTEX_REDSTONE_ORE, BTEX_REDSTONE_ORE },
      REG_FLAG_SOLID, 90, 0 },
    { 32, "lapis_ore", { BTEX_LAPIS_ORE, BTEX_LAPIS_ORE, BTEX_LAPIS_ORE,
                         BTEX_LAPIS_ORE, BTEX_LAPIS_ORE, BTEX_LAPIS_ORE },
      REG_FLAG_SOLID, 80, 0 },
    { 33, "diamond_ore", { BTEX_DIAMOND_ORE, BTEX_DIAMOND_ORE, BTEX_DIAMOND_ORE,
                           BTEX_DIAMOND_ORE, BTEX_DIAMOND_ORE, BTEX_DIAMOND_ORE },
      REG_FLAG_SOLID, 100, 0 },

    /* v1.8.14 "Animals"'s four raw meats, ids 34..37 (ANIMAL-B lane). Transcribed from
     * world/block.h's BLOCK_RAW_* / BTEX_RAW_* constants and the stated intent of the rows,
     * the same way every row above was — independently, not copied out of world/registry.c's
     * kCoreDefs, which is the table this whole list exists to disagree with when it is wrong.
     *
     * THE IDS ARE 34..37, NOT 27..30. docs/plan-1.8.14-animals.md still says 27..30; that plan
     * predates v1.8.10's torch (which took 27) and v1.8.12's six ores (28..33). 34 was read off
     * BLOCK_DIAMOND_ORE == 33 in world/block.h, not off any plan document.
     *
     * All four are opaque solid cubes — REG_FLAG_SOLID alone, luminance 0 — for the same reason
     * BLOCK_APPLE (id 26) is: an inventory item in this engine is a placeable block, there is no
     * separate item registry (world/block.h typedefs ItemId to BlockId), and a CROSS shape would
     * make dropped meat a flat billboard. Hardness is a four-step ladder, chicken 3 < porkchop 4
     * < mutton 5 < beef 6: above the 1-tick floor every CROSS plant sits at, and far below
     * stone's 45, so meat reads as soft. The ladder is transcribed from that intent here, NOT
     * read back out of registry.c. */
    { 34, "raw_porkchop", { BTEX_RAW_PORKCHOP, BTEX_RAW_PORKCHOP, BTEX_RAW_PORKCHOP,
                            BTEX_RAW_PORKCHOP, BTEX_RAW_PORKCHOP, BTEX_RAW_PORKCHOP },
      REG_FLAG_SOLID, 4, 0 },
    { 35, "raw_beef", { BTEX_RAW_BEEF, BTEX_RAW_BEEF, BTEX_RAW_BEEF,
                        BTEX_RAW_BEEF, BTEX_RAW_BEEF, BTEX_RAW_BEEF },
      REG_FLAG_SOLID, 6, 0 },
    { 36, "raw_chicken", { BTEX_RAW_CHICKEN, BTEX_RAW_CHICKEN, BTEX_RAW_CHICKEN,
                           BTEX_RAW_CHICKEN, BTEX_RAW_CHICKEN, BTEX_RAW_CHICKEN },
      REG_FLAG_SOLID, 3, 0 },
    { 37, "raw_mutton", { BTEX_RAW_MUTTON, BTEX_RAW_MUTTON, BTEX_RAW_MUTTON,
                          BTEX_RAW_MUTTON, BTEX_RAW_MUTTON, BTEX_RAW_MUTTON },
      REG_FLAG_SOLID, 5, 0 },

    /* v1.8.15 "Furnace"'s five, ids 38..42 (FURNACE-C lane): four cooked meats and the furnace
     * itself. Transcribed the same way every row above was -- from world/block.h's BLOCK_* and
     * BTEX_* constants and from world/registry.c's own ROW COMMENTS stating the intent, never
     * from its .tex/.flags/.hardness fields, which are the thing this list exists to disagree
     * with when they are wrong.
     *
     * The four cooked cuts carry their RAW counterpart's hardness UNCHANGED -- chicken 3 <
     * porkchop 4 < mutton 5 < beef 6, the identical four-step ladder as ids 34..37 directly
     * above. That is the stated intent, not a coincidence to be tidied away: cooking changes
     * what the meat feeds you, not how big or how tough the block is. Four identical bytes here
     * would be the failure mode registry_test.c's hardness ladder exists to catch, so the
     * repetition of the raw numbers is deliberate and must survive anyone "simplifying" it.
     * REG_FLAG_SOLID alone, luminance 0, for the raw meats' and the apple's reason: an item in
     * this engine IS a placeable block, and claiming TRANSPARENT would push four more opaque
     * cubes into the mesher's deferred pass for nothing.
     *
     * The furnace is the first row in this entire table whose six faces are NOT all the same
     * tile. Five faces are BTEX_STONE and only FACE_SOUTH carries BTEX_FURNACE_FRONT --
     * FACE_SOUTH being index 4 in world/block.h's face order (EAST, WEST, TOP, BOTTOM, SOUTH,
     * NORTH), and being, per registry.c's own comment, an ARBITRARY BUT FIXED choice: nothing
     * in this build carries a facing, so any one face is as correct as any other and south
     * means nothing. Anyone reading a direction into it is reading something that is not there.
     * Hardness 45, BLOCK_STONE's own number, because a furnace is worked stone.
     *
     * BTEX_FURNACE_FRONT_LIT (atlas slot 47) is deliberately absent from this row. The registry
     * is one static row per BLOCK, not one per STATE, so it cannot swap a face when a furnace
     * lights; the lit tile is painted ahead of the renderer that will select it. That is why
     * BLOCK_FURNACE is one id owning TWO atlas slots and why the id-to-slot offset that held
     * for every append before this one no longer exists. */
    { 38, "cooked_porkchop", { BTEX_COOKED_PORKCHOP, BTEX_COOKED_PORKCHOP, BTEX_COOKED_PORKCHOP,
                               BTEX_COOKED_PORKCHOP, BTEX_COOKED_PORKCHOP, BTEX_COOKED_PORKCHOP },
      REG_FLAG_SOLID, 4, 0 },
    { 39, "cooked_beef", { BTEX_COOKED_BEEF, BTEX_COOKED_BEEF, BTEX_COOKED_BEEF,
                           BTEX_COOKED_BEEF, BTEX_COOKED_BEEF, BTEX_COOKED_BEEF },
      REG_FLAG_SOLID, 6, 0 },
    { 40, "cooked_chicken", { BTEX_COOKED_CHICKEN, BTEX_COOKED_CHICKEN, BTEX_COOKED_CHICKEN,
                              BTEX_COOKED_CHICKEN, BTEX_COOKED_CHICKEN, BTEX_COOKED_CHICKEN },
      REG_FLAG_SOLID, 3, 0 },
    { 41, "cooked_mutton", { BTEX_COOKED_MUTTON, BTEX_COOKED_MUTTON, BTEX_COOKED_MUTTON,
                             BTEX_COOKED_MUTTON, BTEX_COOKED_MUTTON, BTEX_COOKED_MUTTON },
      REG_FLAG_SOLID, 5, 0 },
    { 42, "furnace", { BTEX_STONE, BTEX_STONE, BTEX_STONE,
                       BTEX_STONE, BTEX_FURNACE_FRONT, BTEX_STONE },
      REG_FLAG_SOLID, 45, 0 },
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
        rec[24] = row->luminance;                    /* 0 on every core row except the torch */
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
    check(registryCount() == 45, "the partial batch landed two rows");
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
    check(registryCount() == 45 && registryFind("srv1_a") == REG_ID_DYN_LO,
          "the first server's two rows are in the table");

    /* netDisconnect() runs networldInit() (net/bsnet.c), which is the whole of leaving a
     * session as far as this module is concerned. */
    networldInit();
    check(registryCount() == 43, "after leaving, only the forty-three core rows remain");
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
    check(registryCount() == 44, "and it is the only dynamic row in the table");
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
    check(registryFrozen() && registryCount() == 44,
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
    check(registryCount() == 44 && registryFind("sp_sidecar") == 0,
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

    check(registryCount() == 43,
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
    check(registryCount() == 43, "the frozen table refused the batch, per registry.h's contract");
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
    check(registryCount() == 45, "the unsolicited batch's rows did land in the table");
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
    check(registryCount() == 46, "all three declared rows landed");
    check(networldRegistrySynced(), "and the table reproduces the server's whole fingerprint");
    check(!networldRegistryWaiting(), "which releases world entry immediately");

    /* A SHORT delivery that still carries the LAST flag: two of the three rows, then "that is
     * all". The count alone already disagrees, and the retry must stay armed for the third. */
    registryJoinDeclaring(20000, declared, 3);
    uint8_t two[BS_APP_REGISTRY_DEFS_BYTES(2)];
    const size_t twolen = buildRegistryDefs(two, REG_ID_DYN_LO, declared, 2, true);
    networldApplyPayload(two, twolen);
    check(registryCount() == 45, "two of the three rows landed");
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
    check(registryCount() == 46, "three rows landed, so rev and count both agree");
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
    check(registryCount() == 46, "control: the rows landed exactly as in the passing case");
    check(!networldRegistrySynced(), "but a foreign table revision is never verifiable");
}

/* ---- the verdict (v1.8.7) -------------------------------------------------------------------
 *
 * Everything above this line establishes what networldRegistrySynced() MEANS. None of it says
 * what happens when the answer is no, because until v1.8.7 the answer was "nothing happens":
 * the bounded wait expired, scene/title.c's entry gate opened, and the player walked into a
 * world where every id the two builds disagreed about was an air hole. On a CORE-row
 * disagreement that state is permanent — a DEFS batch cannot carry a core row — and the only
 * lasting indicator is a "!" on a debug overlay that is off unless the player turned it on.
 * v1.8.1 shipped exactly that, which is why the comment above registryMatchesInfo() in
 * networld.c is as long as it is.
 *
 * These two scenarios are the enforcement and its blast radius. The first is what must now be
 * refused; the second is the much longer list of things that must still NOT be, because a
 * refusal that fires one case too wide takes offline play or every pre-v1.6.0 server with it.
 *
 * The observable is netTransportRefuse(): net/networld.c really calls it, and the double at the
 * top of this file really records it. Nothing here re-implements the decision — registryVerdict-
 * Tick() is reached the only way it is ever reached in production, through networldUpdate(). */

/* One frame of the real pump, `n` times, for the cases that are about a decision NOT being
 * taken: a single call cannot tell "never fires" from "has not fired yet". */
static void pumpFrames(int n)
{
    for (int i = 0; i < n; i++) networldUpdate();
}

static void test_registry_mismatch_refuses_instead_of_degrading(void)
{
    puts("registry: a table that never reproduces the server's fingerprint refuses the session");

    /* Direction one: the server names FEWER rows than this build's core table has. That is what
     * an older server looks like to a newer client — v1.8.3 Phase 3 took the core table from 10
     * rows to 15 — and it is unreachable by construction, not merely unreached: a DEFS batch can
     * only ADD rows, so no delivery can ever bring this table back down to the count named. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(30000);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    info[2] = (uint8_t)(registryCount() - 1u);   /* one core row fewer than this build has */
    info[3] ^= 0xA5u;                            /* and, necessarily, a different fingerprint */
    networldApplyPayload(info, sizeof info);

    check(fake_refuse_calls == 0,
          "nothing is refused while the bounded retry still has time and tries left");
    check(networldRegistryWaiting(),
          "control: the gate really is still holding, so the check above is not vacuous");

    /* The deadline. This is the exact edge scene/title.c would have entered the world on. */
    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS);
    networldUpdate();

    check(fake_refuse_calls == 1,
          "at the instant the gate would have released, the session is refused instead");
    printf("    [measured] refusal text: \"%s\"\n", fake_refuse_why);
    check(strstr(fake_refuse_why, "version") != NULL,
          "and the reason names a version disagreement, which is the thing the player can act on");
    /* The emptiness clause is not padding. Neutralising the refusal leaves fake_refuse_why
     * an empty string, and an empty string contains no "0x" — so without it this check
     * passed in the red arm, against a session that had refused nothing at all. */
    check(fake_refuse_why[0] != '\0' && strstr(fake_refuse_why, "0x") == NULL,
          "there is a real line, and it is not a hex dump of two crc16s (true, useless)");
    check(!networldWorldSeed(NULL),
          "the world seed is gone with the session, which is what closes title_nav.h's entry "
          "gate on this same frame rather than one frame late");
    check(!networldRegistrySynced(),
          "and nothing about the refusal pretends the table was ever agreed");
    check(!networldRegistryWaiting(),
          "the bound networld.h promises is still kept: this does not become a gate that hangs");

    /* Once. The tick runs every frame forever after; a second refusal would mean a second
     * netTransportRefuse() at a transport that has already been torn down. */
    fakeNowAdvance(10u * NETWORLD_REG_SYNC_DEADLINE_MS);
    pumpFrames(5);
    check(fake_refuse_calls == 1, "and it is taken exactly once, however long the pump runs on");

    /* Direction two: the COUNT agrees and the CONTENT does not. This is what a core-row change
     * looks like from the other side — the server has a row this build does not, the fetch
     * delivers a dynamic row that makes the totals line up, and only the crc16 knows. The
     * fingerprint check already caught this (test_registry_sync_is_proved_by_the_fingerprint
     * above); what is new is that catching it now ends the session. */
    BlockDef declared[3] = { wireDef("verdict_a"), wireDef("verdict_b"), wireDef("verdict_c") };
    BlockDef impostor[3] = { wireDef("verdict_a"), wireDef("verdict_b"), wireDef("verdict_X") };

    registryJoinDeclaring(40000, declared, 3);
    uint8_t wrong[BS_APP_REGISTRY_DEFS_BYTES(3)];
    const size_t wlen = buildRegistryDefs(wrong, REG_ID_DYN_LO, impostor, 3, true);
    networldApplyPayload(wrong, wlen);

    check(registryCount() == 46,
          "control: three rows landed, so the server's count is reproduced exactly");
    check(!networldRegistrySynced(),
          "control: and only the crc16 disagrees — this is the same-count, wrong-content case");
    check(fake_refuse_calls == 0, "still nothing refused inside the wait");

    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS);
    networldUpdate();
    check(fake_refuse_calls == 1,
          "a table with the right number of the wrong rows is refused too, not entered");
    check(!networldWorldSeed(NULL), "with the same gate-closing teardown as the first direction");
}

static void test_registry_agreement_and_silence_are_never_refused(void)
{
    puts("registry: agreement, an old server's silence and single player are never refused");

    /* (a) The compiled-in table already IS the server's. The commonest join there is. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(50000);

    uint8_t wi[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi, 4242);
    networldApplyPayload(wi, sizeof wi);

    uint8_t info[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(info);
    networldApplyPayload(info, sizeof info);

    check(networldRegistrySynced(), "control: a matching fingerprint syncs on the INFO alone");
    fakeNowAdvance(10u * NETWORLD_REG_SYNC_DEADLINE_MS);
    pumpFrames(5);
    check(fake_refuse_calls == 0, "and no amount of pumping afterwards refuses an agreed table");
    check(networldWorldSeed(NULL), "the session is untouched: the seed is still there to enter on");

    /* (b) A fetch that really converges. The rows arrive late, which is the case the deadline
     * exists for, and the verdict must read the settled table rather than the timer. */
    BlockDef declared[2] = { wireDef("agree_a"), wireDef("agree_b") };
    registryJoinDeclaring(60000, declared, 2);
    uint8_t batch[BS_APP_REGISTRY_DEFS_BYTES(2)];
    const size_t blen = buildRegistryDefs(batch, REG_ID_DYN_LO, declared, 2, true);
    networldApplyPayload(batch, blen);

    check(networldRegistrySynced(), "control: the delivered rows reproduce the fingerprint");
    fakeNowAdvance(10u * NETWORLD_REG_SYNC_DEADLINE_MS);
    pumpFrames(5);
    check(fake_refuse_calls == 0, "so the deadline passing afterwards refuses nothing");

    /* (c) A server older than v1.6.0: WORLD_INFO, and no BS_APP_REGISTRY_INFO, ever. It named
     * no fingerprint, so nothing about its table has been disproved and there is nothing to
     * refuse ON. Refusing here would make every one of those servers unjoinable. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(70000);
    networldApplyPayload(wi, sizeof wi);

    fakeNowAdvance(10u * NETWORLD_REG_SYNC_DEADLINE_MS);
    pumpFrames(5);
    check(!networldRegistrySynced(),
          "control: with no INFO there is no fingerprint, so nothing is verified");
    check(fake_refuse_calls == 0, "and an unverifiable table is not a disproved one: no refusal");
    check(networldWorldSeed(NULL),
          "the old server is still joinable, degraded, exactly as it always was");
    check(!networldRegistryWaiting(), "with the short grace still bounding the wait");

    /* (d) Single player. The trap this guard is written against: netTransportState() is doubled
     * ESTABLISHED at the top of this file, so anything gating on networldSessionActive() would
     * treat an offline session as a live one and refuse it. networld.c gates on the registry
     * gate flag instead, which only a decoded server packet can arm — and offline decodes none. */
    registryInitCore();
    networldInit();
    fakeTransportReset();
    fakeNowSet(80000);

    check(networldSessionActive(),
          "control: the transport double claims ESTABLISHED, so a session-state guard would "
          "have believed this offline pump was a join");
    fakeNowAdvance(100u * NETWORLD_REG_SYNC_DEADLINE_MS);
    pumpFrames(10);
    check(fake_refuse_calls == 0, "single player is never refused, however far the clock runs");
    check(!networldRegistryWaiting(), "and was never held on the registry in the first place");
    check(!networldWorldSeed(NULL), "no server named a world, so there was never one to lose");

    /* And the part that costs something if the guard above is the wrong one. net/bsnet.c's
     * netConnect() does NOT call networldInit() — only netDisconnect() does — so a join can
     * begin on exactly the module state the offline pump above just left behind. A guard that
     * let those frames take the session's one verdict would latch it against a session that
     * had not started, and this join would then be unrefusable however wrong its table was.
     * Deliberately no networldInit() here: that call is what the scenario is testing the
     * absence of. */
    uint8_t wi2[BS_WORLD_INFO_BYTES];
    buildWorldInfoMsg(wi2, 4242);
    networldApplyPayload(wi2, sizeof wi2);

    uint8_t bad[BS_APP_REGISTRY_INFO_BYTES];
    buildRegistryInfo(bad);
    bad[2] = (uint8_t)(registryCount() - 1u);
    bad[3] ^= 0xA5u;
    networldApplyPayload(bad, sizeof bad);
    check(networldRegistryWaiting(),
          "control: the join really did arm the gate on top of the offline frames");

    fakeNowAdvance(NETWORLD_REG_SYNC_DEADLINE_MS);
    networldUpdate();
    check(fake_refuse_calls == 1,
          "and the verdict was still there to be taken: single player spent none of it");
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

/* ------------------------------------------- rejoin replay vs. the water simulation ------- */
/* v1.8.19. The three scenarios below are the only place in the repository where net/blockdiff.c
 * and world/water.c are linked into the same binary, which is what this file's link line in
 * tools/run_host_tests.sh was widened for. Nothing else can reproduce the defect: the drain that
 * causes it lives in networld.c and the ring it overruns lives in water.c, and until now no test
 * could see both ends of that at once.
 *
 * What is being reproduced, in one sentence: a rejoining client's stored diffs all land inside
 * the frame their column installs, every one of them fires world.c's edit hook, and the seven
 * cells each firing offers to water.c's 1024-entry ring evict whatever the player put there
 * first. Measured 2026-08-30 against a model of main.c's frame order: 47 of 60 player placements
 * lost, 78.3%.
 *
 * The three arms are deliberately in tension, because the obvious fix passes one and fails
 * another. Blanket suppression during the drain passes the first arm perfectly (0 evicted) and
 * destroys the second (replayed water spreads to 1 block instead of 113). A per-column drain
 * budget passes the second and fails the first. Only a filter that asks whether there is water
 * nearby passes both, which is why the production code is one. */

#define REPLAY_COL_X0    2     /* replay columns cx 2..8, cz 0..6: 7 x 7 = the measured 49 */
#define REPLAY_COL_Z0    0
#define REPLAY_COL_SPAN  7
#define REPLAY_PER_COL   200   /* the measured per-column backlog */
#define REPLAY_DIFFS     (REPLAY_COL_SPAN * REPLAY_COL_SPAN * REPLAY_PER_COL)   /* 9800 */

static WaterSim    g_replay_sim;
static const World *g_replay_world;

/* The stand-in for source/main.c's onWorldEdit (main.c:3941), and it is a copy of it down to the
 * order of the two guards. That copy is not taken on trust:
 * test_mainc_edit_hook_filters_only_replayed_writes() below reads main.c's own source text and
 * fails if the composition here stops matching the game's. Without that pin this hook would be
 * hand-written logic proving itself, which is the defect water_test.c's wiredEditHook header
 * describes at length. */
static void replayEditHook(void *ud, const World *w, int x, int y, int z, BlockId prev, BlockId now)
{
    (void)ud; (void)prev; (void)now;
    if (w != g_replay_world) return;
    if (networldReplayingDiffs() && waterReplaySkippable(&g_replay_sim, w, x, y, z)) return;
    waterNotify(&g_replay_sim, x, y, z);
}

/* water_test.c's buildFloor, in this file's idiom: a 27x27 stone slab at y 60..63 centred on the
 * origin, air above it. Deliberately the same fixture, so the outcome numbers below (113 blocks,
 * 112 flow cells) are the same literals testFlatPour() pins, and a disagreement between the two
 * files is a real disagreement and not two different experiments. */
static void replayBuildFloor(World *w)
{
    for (int y = 60; y <= 63; y++)
        for (int z = -13; z <= 13; z++)
            for (int x = -13; x <= 13; x++)
                (void)worldSet(w, x, y, z, BLOCK_STONE);
}

/* Wide enough for both pours: arm one's is at the origin and reaches x,z -7..7, arm two's is at
 * (8,64,8) inside column (0,0) and reaches 1..15. A box that clipped either would report a low
 * count as a pass-shaped number, so it covers both with room to spare. */
static int replayCountWater(const World *w)
{
    int n = 0;
    for (int y = 60; y <= 70; y++)
        for (int z = -13; z <= 17; z++)
            for (int x = -13; x <= 17; x++)
                if (worldGet(w, x, y, z) == BLOCK_WATER) n++;
    return n;
}

/* Arms the hook, lays the floor, then settles so the ring is empty. Every fixture cell fires the
 * hook exactly as it does on the console, so the slab alone offers 2916 candidates to a ring of
 * 1024 -- that saturation is the fixture's, not the burst's, and it has to be drained away
 * before the burst starts or the measurement below is of the wrong thing. */
static void replayArmAndFloor(World *w)
{
    worldInit(w);
    networldInit();
    networldSetWorld(w);

    waterInit(&g_replay_sim);
    g_replay_world = w;
    worldSetEditHook(replayEditHook, NULL);

    replayBuildFloor(w);
    (void)waterSettle(&g_replay_sim, w, 20000, NULL, NULL);
}

/* world.c's s_edit_fn is a file static that outlives every World (world.c:217), so a hook left
 * armed here fires during the NEXT scenario's fixture, against a sim that scenario never meant to
 * disturb. Every arm below ends with this. */
static void replayDisarm(World *w)
{
    worldSetEditHook(NULL, NULL);
    g_replay_world = NULL;
    worldExit(w);
    networldInit();
}

static void test_a_rejoin_replay_keeps_the_players_own_water(void)
{
    puts("a rejoin's diff replay no longer evicts the water the player placed himself");

    World w;
    replayArmAndFloor(&w);
    check(waterPending(&g_replay_sim) == 0 && waterFlowCells(&g_replay_sim) == 0,
          "fixture: the floor is down and the ring is drained, so nothing below is the floor's");

    /* 49 unloaded columns, 200 diffs each. Every one of them is queued rather than applied --
     * networld.c refuses to create a phantom column -- which is precisely the rejoin case. */
    for (int cz = REPLAY_COL_Z0; cz < REPLAY_COL_Z0 + REPLAY_COL_SPAN; cz++)
        for (int cx = REPLAY_COL_X0; cx < REPLAY_COL_X0 + REPLAY_COL_SPAN; cx++)
            for (int i = 0; i < REPLAY_PER_COL; i++) {
                uint8_t msg[BS_BLOCK_EDIT_BYTES];
                buildBlockEdit(msg, cx * 16 + (i % 16), 64, cz * 16 + (i / 16), BLOCK_STONE);
                networldApplyPayload(msg, sizeof msg);
            }
    check(networldPendingCount() == REPLAY_DIFFS, "all 9800 stored diffs are queued, not applied");
    check(worldGet(&w, REPLAY_COL_X0 * 16, 64, REPLAY_COL_Z0 * 16) == BLOCK_AIR,
          "control: none of them has reached the world yet");

    /* The player builds while his columns are still streaming in. This is a LIVE edit, so the
     * flag is false and it is never filtered. */
    check(worldSet(&w, 0, 64, 0, BLOCK_WATER), "the player places a water source at (0,64,0)");
    check(waterPending(&g_replay_sim) == 7,
          "the edit hook queued his source and its six neighbours, exactly as testFlatPour pins");

    const uint32_t lost_before = waterQueueFull(&g_replay_sim);

    /* The install burst. worldColumnCreate is the streamer's path (it does not go through
     * worldSet and so fires no hook of its own); networldOnColumnLoad is what main.c calls
     * immediately after it, at main.c:1940. */
    for (int cz = REPLAY_COL_Z0; cz < REPLAY_COL_Z0 + REPLAY_COL_SPAN; cz++)
        for (int cx = REPLAY_COL_X0; cx < REPLAY_COL_X0 + REPLAY_COL_SPAN; cx++) {
            worldColumnCreate(&w, cx, cz);
            networldOnColumnLoad(&w, cx, cz);
        }

    const uint32_t lost = waterQueueFull(&g_replay_sim) - lost_before;
    printf("         replay of %d diffs: %u ring evictions, %d pending of %d\n",
           REPLAY_DIFFS, lost, waterPending(&g_replay_sim), (int)WATERQ_CAP);

    check(networldPendingCount() == 0, "the burst drained every one of them");
    check(worldGet(&w, REPLAY_COL_X0 * 16, 64, REPLAY_COL_Z0 * 16) == BLOCK_STONE,
          "control: the replayed diffs really did land, so the run had work to filter");

    /* The measurement, stated as the thing that actually goes wrong. Before the filter this was
     * 68600 pushes against a ring of 1024 and the player's seven were the oldest entries in it. */
    check(lost == 0, "the replay evicted nothing from the water ring");

    (void)waterSettle(&g_replay_sim, &w, 20000, NULL, NULL);
    check(waterFlowCells(&g_replay_sim) == 112,
          "his source still spreads to 112 flow cells, not the 0 an eviction leaves");
    check(replayCountWater(&w) == 113, "113 water blocks in the world: the source and its 112");

    replayDisarm(&w);
}

static void test_a_replayed_water_diff_still_spreads(void)
{
    puts("water that arrives AS a replayed diff still spreads -- the filter is not a blanket");

    /* This arm is the one blanket suppression fails, and it fails it silently: suppressing every
     * notify during the drain leaves a remote player's pour as a single wet cell with no flow at
     * all, in a build where the first arm above reports a perfect 0 evicted. Measured 2026-08-30:
     * 117 blocks and 608 flow cells with the notify, 1 block and 0 flow without it. */

    World w;
    worldInit(&w);
    networldInit();
    networldSetWorld(&w);
    waterInit(&g_replay_sim);
    g_replay_world = &w;
    worldSetEditHook(replayEditHook, NULL);

    /* Queued FIRST, while column (0,0) does not exist yet -- that is what makes it a replay
     * rather than a live remote edit. Everything about this column is then installed the way the
     * streamer installs one, and NOT with worldSet: world.c:245-250 auto-drains the pending store
     * the moment a worldSet creates a column, so a floor laid with worldSet would land this diff
     * part-way through its own fixture and then let the remaining 2900-odd fixture cells evict the
     * notify it queued. That cost a red arm on 2026-09-05 which read as the production filter
     * suppressing a replayed pour; it was the fixture draining itself early. The bulk path fires
     * no edit hook at all (world.h:101-104) and does not drain, so the drain below is the only
     * one in this scenario -- which is also exactly main.c's order at main.c:1890-1940. */
    uint8_t msg[BS_BLOCK_EDIT_BYTES];
    buildBlockEdit(msg, 8, 64, 8, BLOCK_WATER);
    networldApplyPayload(msg, sizeof msg);
    check(networldPendingCount() == 1, "the remote pour is queued against an unloaded column");
    check(worldGet(&w, 8, 64, 8) == BLOCK_AIR, "control: it has not been applied yet");

    /* Chunk (0,3,0) is world y 48..63; stone in its top four layers is the same y 60..63 slab
     * arm one pours onto. The pour is at (8,64,8), so its radius of 7 stays inside x,z 1..15 and
     * never needs a neighbouring column. */
    {
        static BlockId in[CHUNK_BLOCKS];
        for (int i = 0; i < CHUNK_BLOCKS; i++) in[i] = BLOCK_AIR;
        for (int y = 12; y < 16; y++)
            for (int z = 0; z < 16; z++)
                for (int x = 0; x < 16; x++)
                    in[chunkIndex(x, y, z)] = BLOCK_STONE;
        check(worldSetChunkAll(&w, 0, 3, 0, in), "the streamer installs the column's floor chunk");
    }
    check(networldPendingCount() == 1 && waterPending(&g_replay_sim) == 0,
          "fixture: the bulk install fired no edit hook and drained nothing");

    networldOnColumnLoad(&w, 0, 0);

    check(worldGet(&w, 8, 64, 8) == BLOCK_WATER, "the replayed source landed in the world");
    check(waterPending(&g_replay_sim) == 7,
          "and was NOT filtered: it queued its own seven cells, because it is itself the water");

    (void)waterSettle(&g_replay_sim, &w, 20000, NULL, NULL);
    check(waterFlowCells(&g_replay_sim) == 112, "it spreads to the same 112 flow cells");
    check(replayCountWater(&w) == 113, "and the same 113 water blocks");

    replayDisarm(&w);
}

static void test_mainc_edit_hook_filters_only_replayed_writes(void)
{
    puts("source/main.c's onWorldEdit composes the two guards the hook above copies");

    /* The behavioural arms above run replayEditHook, not main.c's onWorldEdit -- main.c is the
     * console binary and has no host build. So they prove the FILTER works and prove nothing
     * about whether the game installs it. This is the half that does, using app/session_test.c's
     * technique of reading main.c's own source text. Run from the repository root. */
    char *src = readWholeSourceFile("source/main.c");
    check(src != NULL, "source/main.c could be read");
    if (src == NULL) {
        printf("  (source/main.c could not be read from this working directory)\n");
        return;
    }

    check(strstr(src, "#include \"world/water.h\"") != NULL,
          "control: main.c owns the water simulation at all");

    const char *fn = strstr(src, "static void onWorldEdit(");
    check(fn != NULL, "main.c still has the world edit hook this test is about");
    if (fn != NULL) {
        /* Body only. The function's statements are tab-indented, so the first line-initial
         * closing brace after the signature is its own. */
        const char *end = strstr(fn, "\n}");
        check(end != NULL, "the hook's body is delimited");
        if (end != NULL) {
            const size_t body_len = (size_t)(end - fn);
            char *body = (char *)malloc(body_len + 1);
            check(body != NULL, "the body could be copied out for searching");
            if (body != NULL) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';

                check(strstr(body, "waterNotify(&s_water, x, y, z);") != NULL,
                      "control: the live path still reaches waterNotify at all");
                check(strstr(body, "if (w != &s_world) return;") != NULL,
                      "control: the staging-world guard is still the first filter");

                /* One condition, not two statements. Two separate early returns would be a
                 * blanket suppression wearing the same identifiers, and would pass a pair of
                 * strstr checks that only asked whether both names appeared. */
                check(strstr(body,
                             "if (networldReplayingDiffs() && waterReplaySkippable("
                             "&s_water, &s_world, x, y, z)) return;") != NULL,
                      "the replay filter is the two guards ANDed, applied to the edited cell");

                free(body);
            }
        }
    }
    free(src);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    puts("== networld test ==");

    test_loaded_column_applies_directly();
    test_unloaded_column_queues_not_drops();
    test_explicit_column_load_drains_pending();
    test_pending_store_is_absent_in_single_player();
    test_pending_store_allocation_failure_is_counted_not_swallowed();
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
    test_world_gen_carries_the_servers_generator();
    test_time_sync_carries_the_servers_clock();
    test_gen_gate_holds_entry_until_the_generator_is_known();
    test_an_unhandled_app_type_changes_nothing();
    test_the_session_generator_resolves_and_refuses();
    test_edit_hook_fires_for_an_applied_edit();
    test_remote_break_of_a_stateful_block_clears_its_state();
    test_remote_place_of_a_furnace_creates_its_state();
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
    test_registry_mismatch_refuses_instead_of_degrading();
    test_registry_agreement_and_silence_are_never_refused();
    test_mesher_tables_after_register();

    test_a_rejoin_replay_keeps_the_players_own_water();
    test_a_replayed_water_diff_still_spreads();
    test_mainc_edit_hook_filters_only_replayed_writes();

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
     *   more than it (368 here, 369 on the PASS line). That off-by-one is deliberate: it means
     *   blind-pasting the number off the PASS line lands you a red, not a false green.
     *
     * This guard is deliberately scoped to THIS suite. The same hole exists in every other
     * host suite under source/ and is a separate, fleet-wide job. */
    /* 368 -> 389. Delta computed from what was added, per the instructions above, not pasted
     * off a failing run: test_pending_store_is_absent_in_single_player adds 12 and
     * test_pending_store_allocation_failure_is_counted_not_swallowed adds 9. Nothing removed. */
    /* 389 -> 416, same rule. test_registry_mismatch_refuses_instead_of_degrading adds 14 (9 for
     * the short-count direction, 5 for the wrong-content one) and
     * test_registry_agreement_and_silence_are_never_refused adds 15 (3 + 2 + 4 + 6 for its four
     * must-not-refuse cases; the last one grew by 2 when a red arm showed its single-player
     * checks could not tell the gate guard from a transport-state guard). 14 + 15 = 29.
     * Nothing removed. */
    /* 416 -> 429, same rule. test_remote_break_of_a_stateful_block_clears_its_state adds
     * 11: 3 behavioural (the fixture furnace, the emptied cell, the hook's coordinate) and
     * 8 source-text (main.c readable, the blockstate.h include, the local break path's own
     * removal as a control that the search shape finds hits at all, onRemoteEdit found, its
     * body delimited, the body copied out, and the two that are the point of the file: the
     * remote path removes a record, and removes it for the coordinate it was handed).
     * Nothing removed. */
    /* 429 -> 440, same rule. test_remote_place_of_a_furnace_creates_its_state adds 11: 3
     * behavioural (the air fixture, the landed furnace block, the hook's coordinate) and 8
     * source-text (main.c readable, the local place path's own BLOCK_FURNACE gate as a control,
     * the local path's own blockStateCreate call as a second control, onRemoteEdit found, its
     * body delimited, the body copied out, and the two that are the point of the file: the
     * remote path never names BLOCK_FURNACE and never calls blockStateCreate at all).
     * Nothing removed. */
    /* 440 -> 444, same rule. Row 2b inside
     * test_the_session_generator_resolves_and_refuses adds 4: three that a declared BIOME,
     * CAVES and ORES each resolve to themselves, and one that ORES is still
     * GEN_VERSION_NEWEST, so the ORES check is known to be sitting at the top of the range
     * rather than somewhere in the middle of it. Nothing removed. */
    /* 444 -> 470, same rule. The three rejoin-replay arms add 26:
     * test_a_rejoin_replay_keeps_the_players_own_water adds 10 (the drained fixture, the 9800
     * queued, the control that none had landed, the player's placement, its seven queued cells,
     * the drain emptying the store, the control that the diffs really landed, the eviction count
     * itself, and the two outcome numbers 112 and 113);
     * test_a_replayed_water_diff_still_spreads adds 8 (queued not applied, the control that it
     * had not landed, the streamer's bulk floor install, the control that that install neither
     * fired a hook nor drained, the landed source, its seven queued cells, and the same two
     * outcome numbers); and test_mainc_edit_hook_filters_only_replayed_writes adds 8 (main.c
     * readable, the water.h include as a control, onWorldEdit found, its body delimited, the body
     * copied out, the two controls that the live path and the staging-world guard are still
     * there, and the one that is the point of the file: the two guards ANDed into one condition).
     * Nothing removed. */
    /* 470 -> 487, same rule. test_time_sync_carries_the_servers_clock adds 17: nothing claimed
     * before a packet and the out-parameter left alone, the value arriving and being exact, the
     * two that are the point of the test (a second take answers false because the flag was
     * consumed, and that false leaves the out-parameter alone), the re-arm proving consuming
     * does not disable the feature for the session, the full 64-bit width with its high half
     * intact, the little-endian byte order against literal bytes, a counter of 0 being a time
     * rather than silence, the optional out-parameter and that a NULL take consumes too, short
     * and long both dropped whole, the pending counter not surviving into the next session or
     * into single player, an unsolicited packet being recorded rather than refused, and the
     * control that a neighbouring type byte does not reach the clock decoder. Nothing
     * removed. */
    check(g_checks == 487,
          "check-count guard: every check in this suite actually ran (487 before this line)");

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
