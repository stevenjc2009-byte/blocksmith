/* blockdiff_test — host unit test for the pending block-diff store.
 *
 * In the style of server/gateway/bsgate_test.c: prints "ok"/"FAIL" lines per check and a
 * final "PASS/FAIL N checks, M failed" summary, exiting non-zero on any failure. Unlike
 * bsgate_test this needs no daemon and no network — blockdiff.c is a plain data structure,
 * so every check runs in-process against a BlockDiffStore on the stack.
 *
 * The __3DS__ guard below is load-bearing, not tidy — the same one app/options_test.c
 * carries, for the same reason. mc/Makefile globs every .c under source/net into the
 * console build, so without it this file's main() links against source/main.c's and the
 * build dies with "multiple definition of `main'". Found exactly that way.
 */
#ifndef __3DS__

#include "net/blockdiff.h"

#include <stdio.h>
#include <string.h>

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

/* ---------------------------------------------------- apply-callback plumbing */

/* Records every (x, y, z, id) the store hands back through blockdiffDrain(), so a test can
 * inspect what was actually applied rather than just trusting the returned count.
 *
 * Every BlockDiffStore and every applied_log below has static storage duration rather than
 * being a plain local. Not style: BLOCKDIFF_MAX_PENDING is 65536, so a BlockDiffStore is a
 * little over 1 MB and an applied_log is roughly 850 KB, while the mingw default thread
 * stack is exactly 1 MB. Declared as locals, this binary died before printing a single
 * check — the run showed only its "== blockdiff test ==" banner, 0 ok lines and 0 FAIL
 * lines. Static storage puts them in BSS, which has no such limit and costs nothing on
 * disk. The console build is unaffected either way: its one store (networld.c's s_pending)
 * has always been a static. */
#define APPLIED_MAX (BLOCKDIFF_MAX_PENDING + 16)

struct applied_log {
    int     n;
    int     x[APPLIED_MAX];
    int     y[APPLIED_MAX];
    int     z[APPLIED_MAX];
    BlockId id[APPLIED_MAX];
};

static void logApply(void *userdata, int x, int y, int z, BlockId id)
{
    struct applied_log *log = (struct applied_log *)userdata;
    if (log->n >= APPLIED_MAX) return;   /* test bug, not a store bug, if this ever trips */
    log->x[log->n]  = x;
    log->y[log->n]  = y;
    log->z[log->n]  = z;
    log->id[log->n] = id;
    log->n++;
}

static bool loggedContains(const struct applied_log *log, int x, int y, int z, BlockId id)
{
    for (int i = 0; i < log->n; i++) {
        if (log->x[i] == x && log->y[i] == y && log->z[i] == z && log->id[i] == id) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ scenarios */

static void test_record_and_drain(void)
{
    puts("record and drain");

    static BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffCount(&s) == 0, "fresh store is empty");
    check(blockdiffRecord(&s, 10, 5, 20, BLOCK_STONE), "record accepted");
    check(blockdiffCount(&s) == 1, "count reflects the one pending diff");

    static struct applied_log log; log.n = 0;
    const int n = blockdiffDrain(&s, 10 >> 4, 20 >> 4, logApply, &log);
    check(n == 1, "drain reports one diff");
    check(log.n == 1, "apply callback invoked once");
    check(loggedContains(&log, 10, 5, 20, BLOCK_STONE),
          "applied diff carries the right coordinate and block");
    check(blockdiffCount(&s) == 0, "store empty after drain");

    /* Draining an empty or unrelated column must be a harmless no-op. */
    static struct applied_log log2; log2.n = 0;
    check(blockdiffDrain(&s, 10 >> 4, 20 >> 4, logApply, &log2) == 0,
          "draining an already-empty column reports zero");
    check(log2.n == 0, "and never calls apply");

    blockdiffFree(&s);
}

static void test_latest_write_wins(void)
{
    puts("latest write wins");

    static BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRecord(&s, 3, 8, 3, BLOCK_STONE), "first write accepted");
    check(blockdiffRecord(&s, 3, 8, 3, BLOCK_GRASS), "second write to same coordinate accepted");
    check(blockdiffCount(&s) == 1, "same coordinate collapses to one entry, not two");

    static struct applied_log log; log.n = 0;
    const int n = blockdiffDrain(&s, 3 >> 4, 3 >> 4, logApply, &log);
    check(n == 1, "drain still reports exactly one diff");
    check(log.n == 1 && log.id[0] == BLOCK_GRASS, "the later write is the one that survives");

    /* Place-then-break: the case called out in the brief. A block placed and then broken by
     * the same peer must resolve to air, not silently keep the placed block queued alongside
     * (or instead of) the break. */
    blockdiffClear(&s);
    check(blockdiffRecord(&s, 7, 9, 7, BLOCK_WOOD), "place recorded");
    check(blockdiffRecord(&s, 7, 9, 7, BLOCK_AIR), "break recorded over the same coordinate");
    check(blockdiffCount(&s) == 1, "place-then-break is still one pending entry, not two");

    static struct applied_log log2; log2.n = 0;
    blockdiffDrain(&s, 7 >> 4, 7 >> 4, logApply, &log2);
    check(log2.n == 1 && log2.id[0] == BLOCK_AIR,
          "place-then-break resolves to air, not the placed block");

    blockdiffFree(&s);
}

static void test_columns_stay_separate(void)
{
    puts("diffs for different columns stay separate");

    static BlockDiffStore s;
    blockdiffInit(&s);

    /* Column (0,0) covers x,z in 0..15; column (1,0) covers x in 16..31. */
    check(blockdiffRecord(&s, 4, 10, 4, BLOCK_STONE), "diff for column (0,0)");
    check(blockdiffRecord(&s, 20, 10, 4, BLOCK_SAND), "diff for column (1,0)");
    check(blockdiffCount(&s) == 2, "two distinct columns, two entries");

    static struct applied_log log; log.n = 0;
    int n = blockdiffDrain(&s, 0, 0, logApply, &log);
    check(n == 1, "draining column (0,0) only touches its own diff");
    check(log.n == 1 && log.x[0] == 4, "and it's the (0,0) diff, not the other one");
    check(blockdiffCount(&s) == 1, "the other column's diff is untouched and still pending");

    static struct applied_log log2; log2.n = 0;
    n = blockdiffDrain(&s, 1, 0, logApply, &log2);
    check(n == 1, "draining column (1,0) now finds its own diff");
    check(log2.n == 1 && log2.x[0] == 20, "and it's the right one");
    check(blockdiffCount(&s) == 0, "both columns empty once both are drained");

    /* Same x, different z: column (0,0) covers z in 0..15, column (0,1) covers z in 16..31.
     * A drain that matched on x alone and ignored z entirely would wrongly hand both of
     * these to whichever column asked first — this is the case that isolates z from x. */
    blockdiffClear(&s);
    check(blockdiffRecord(&s, 4, 10, 4, BLOCK_STONE), "diff for column (0,0), z=4");
    check(blockdiffRecord(&s, 4, 10, 20, BLOCK_SAND), "diff for column (0,1), same x, z=20");
    check(blockdiffCount(&s) == 2, "two distinct columns sharing an x, two entries");

    static struct applied_log logz; logz.n = 0;
    check(blockdiffDrain(&s, 0, 0, logApply, &logz) == 1,
          "draining column (0,0) by z alone finds only its own diff");
    check(logz.n == 1 && logz.z[0] == 4, "and it's the z=4 diff, not the z=20 one");
    check(blockdiffCount(&s) == 1, "the z=20 diff is untouched by a same-x, different-z drain");

    static struct applied_log logz2; logz2.n = 0;
    check(blockdiffDrain(&s, 0, 1, logApply, &logz2) == 1, "draining column (0,1) now finds it");
    check(logz2.n == 1 && logz2.z[0] == 20, "and it's the right one");

    /* Negative coordinates: column coordinate is x >> 4 / z >> 4, an arithmetic shift, the
     * same convention world/world.c uses. Block -1 belongs to column -1, not column 0. */
    blockdiffClear(&s);
    check(blockdiffRecord(&s, -1, 5, -1, BLOCK_STONE), "diff at a negative coordinate");
    static struct applied_log discard; discard.n = 0;
    check(blockdiffDrain(&s, 0, 0, logApply, &discard) == 0,
          "column (0,0) does not see a block at x=-1");
    static struct applied_log log3; log3.n = 0;
    check(blockdiffDrain(&s, -1, -1, logApply, &log3) == 1,
          "column (-1,-1) is where the negative-coordinate diff actually lives");

    blockdiffFree(&s);
}

static void test_y_bounds(void)
{
    puts("out-of-range y rejected");

    static BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRecord(&s, 0, -1, 0, BLOCK_STONE) == false, "y below 0 rejected");
    check(blockdiffRecord(&s, 0, WORLD_HEIGHT, 0, BLOCK_STONE) == false,
          "y == WORLD_HEIGHT rejected (one past the top)");
    check(blockdiffRecord(&s, 0, WORLD_HEIGHT + 1000, 0, BLOCK_STONE) == false,
          "y far above WORLD_HEIGHT rejected");
    check(blockdiffCount(&s) == 0, "none of the rejected writes were stored");

    check(blockdiffRecord(&s, 0, 0, 0, BLOCK_STONE), "y == 0 is in range");
    check(blockdiffRecord(&s, 0, WORLD_HEIGHT - 1, 0, BLOCK_STONE), "y == WORLD_HEIGHT-1 is in range");
    check(blockdiffCount(&s) == 2, "both valid writes were stored");

    blockdiffFree(&s);
}

static void test_capacity(void)
{
    puts("behaviour at the memory cap");

    static BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRefusals(&s) == 0, "fresh store has no refusals");

    /* Fill to exactly capacity with distinct coordinates spread across many columns, so
     * nothing here collapses via latest-write-wins. */
    bool all_accepted = true;
    for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
        if (!blockdiffRecord(&s, i * 16, 10, 0, BLOCK_STONE)) all_accepted = false;
    }
    check(all_accepted, "store accepts exactly its capacity worth of distinct diffs");
    check(blockdiffCount(&s) == BLOCKDIFF_MAX_PENDING, "count matches capacity");
    check(blockdiffRefusals(&s) == 0, "no refusals yet — full is not over capacity");

    /* One more, at a brand-new coordinate: must be refused outright. */
    const bool accepted = blockdiffRecord(&s, BLOCKDIFF_MAX_PENDING * 16, 10, 0, BLOCK_GRASS);
    check(accepted == false, "recording one more past capacity is refused");
    check(blockdiffCount(&s) == BLOCKDIFF_MAX_PENDING, "count unchanged by the refusal");
    check(blockdiffRefusals(&s) == 1, "the refusal was counted");

    /* The eviction policy itself: the refusal must not have silently dropped anything that
     * was already accepted. Drain every column the fill loop used and confirm all
     * BLOCKDIFF_MAX_PENDING original diffs are still there, untouched. */
    static struct applied_log log; log.n = 0;
    int drained_total = 0;
    for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
        drained_total += blockdiffDrain(&s, i, 0, logApply, &log);
    }
    check(drained_total == BLOCKDIFF_MAX_PENDING,
          "every one of the original diffs survived the refusal of the (capacity+1)th");
    check(log.n == BLOCKDIFF_MAX_PENDING, "and every one of them was actually applied");
    check(blockdiffCount(&s) == 0, "store empty after draining all of them");

    /* An overwrite of an already-held coordinate must still collapse (latest write wins)
     * even sitting exactly at the cap — that path must never be confused with, or blocked
     * by, the capacity-refusal path. */
    blockdiffClear(&s);
    bool refill_ok = true;
    for (int i = 0; i < BLOCKDIFF_MAX_PENDING; i++) {
        if (!blockdiffRecord(&s, i * 16, 10, 0, BLOCK_STONE)) refill_ok = false;
    }
    check(refill_ok, "store fills back up to capacity");
    check(blockdiffRecord(&s, 0, 10, 0, BLOCK_SAND),
          "overwriting a coordinate already held succeeds even while the store is full");
    check(blockdiffCount(&s) == BLOCKDIFF_MAX_PENDING, "count unchanged by the overwrite");
    check(blockdiffRefusals(&s) == 0, "an overwrite at capacity is not counted as a refusal");

    blockdiffFree(&s);
}

/* ------------------------------------- the client/server capacity gap ---------------------- */
/* What a legacy full-dump WORLD_SYNC of the SERVER's whole diff store actually does to this
 * store, measured rather than reasoned about.
 *
 * Why this is not already covered by test_capacity() above: that test drives the store to
 * BLOCKDIFF_MAX_PENDING and one past it, which proves the refusal POLICY (refuse, do not
 * evict, count it). It says nothing about the number that matters operationally, which is how
 * far past the cap a real server can push this store in one burst. The server's BS_DIFF_MAX is
 * 131072 — twice BLOCKDIFF_MAX_PENDING — and send_world_sync() (bsgame.c) replays every diff it
 * holds, oldest first, in back-to-back BS_APP_WORLD_SYNC batches, with no flow control and no
 * way for this side to say stop.
 *
 * That burst is reachable in ordinary play, not only in some contrived race. The server falls
 * back to it for any player who has not sent a CHUNK_SUB within BS_CHUNK_LEGACY_GRACE_MS (500
 * ms) of joining (bsgame.c tick()), and this client's JOIN completes on the TITLE SCREEN
 * (networld.h's PLAYER_STATE comment) while its first CHUNK_SUB cannot be sent until a column
 * is live in the World (main.c genInstallOne()) — which is after the player has finished with
 * the title screen and worldgen has produced a column. See this suite's report for why the 500
 * ms figure itself is the one thing here that cannot be measured off hardware.
 *
 * The loss is recoverable — handle_chunk_sub() answers every subscription with that column's
 * diffs unconditionally, so anything refused here comes back when the player walks to it — but
 * "recoverable" is a claim about the SERVER's behaviour, not this store's. What this store owes
 * the caller is an honest count of what it turned away, which is what the checks below pin. */
static void test_server_replay_overflow(void)
{
    puts("a legacy full-dump WORLD_SYNC of the server's whole store overflows this one");

    /* Structural facts first. These are the CONTROL for this file's red arms: they are
     * properties of the entry layout, not of the capacity or refusal logic, so a sabotage of
     * blockdiffRecord()'s cap handling must leave them green. Printed as real numbers because
     * blockdiff.h's own cost comment asserts them. */
    printf("    [measured] sizeof(BlockDiffEntry) = %zu bytes\n", sizeof(BlockDiffEntry));
    printf("    [measured] entry[] at BLOCKDIFF_MAX_PENDING (%d) = %zu bytes (%.2f MB)\n",
           BLOCKDIFF_MAX_PENDING,
           sizeof(BlockDiffEntry) * (size_t)BLOCKDIFF_MAX_PENDING,
           (double)(sizeof(BlockDiffEntry) * (size_t)BLOCKDIFF_MAX_PENDING) / (1024.0 * 1024.0));
    printf("    [measured] entry[] at BLOCKDIFF_SERVER_DIFF_MAX (%u) would be %zu bytes (%.2f MB)\n",
           (unsigned)BLOCKDIFF_SERVER_DIFF_MAX,
           sizeof(BlockDiffEntry) * (size_t)BLOCKDIFF_SERVER_DIFF_MAX,
           (double)(sizeof(BlockDiffEntry) * (size_t)BLOCKDIFF_SERVER_DIFF_MAX) / (1024.0 * 1024.0));
    printf("    [measured] whole BlockDiffStore = %zu bytes (%.2f MB)\n",
           sizeof(BlockDiffStore), (double)sizeof(BlockDiffStore) / (1024.0 * 1024.0));

    check(sizeof(BlockDiffEntry) == 16,
          "CONTROL: BlockDiffEntry is exactly 16 bytes, as blockdiff.h's cost figure assumes");
    check(sizeof(BlockDiffEntry) * (size_t)BLOCKDIFF_MAX_PENDING == 1048576u,
          "CONTROL: the entry array is exactly the 1 MB blockdiff.h claims");

    check(BLOCKDIFF_SERVER_DIFF_MAX > (unsigned)BLOCKDIFF_MAX_PENDING,
          "the server can hold strictly more diffs than this store can — the gap is real");

    static BlockDiffStore s;
    blockdiffInit(&s);

    /* Replay BLOCKDIFF_SERVER_DIFF_MAX diffs oldest-first, exactly as send_world_sync() walks
     * the server's store. One per column so nothing collapses via latest-write-wins, which is
     * the worst case and also the realistic one for a world built out over many sessions. */
    int accepted = 0;
    for (unsigned i = 0; i < BLOCKDIFF_SERVER_DIFF_MAX; i++) {
        if (blockdiffRecord(&s, (int)i * 16, 10, 0, BLOCK_STONE)) accepted++;
    }

    const unsigned expect_refused = BLOCKDIFF_SERVER_DIFF_MAX - (unsigned)BLOCKDIFF_MAX_PENDING;
    printf("    [measured] replayed %u, accepted %d, refused %d\n",
           (unsigned)BLOCKDIFF_SERVER_DIFF_MAX, accepted, blockdiffRefusals(&s));

    check(accepted == BLOCKDIFF_MAX_PENDING,
          "exactly BLOCKDIFF_MAX_PENDING of the server's replay were accepted");
    check((unsigned)blockdiffRefusals(&s) == expect_refused,
          "every diff past the cap was REFUSED and COUNTED — the store does not lose them quietly");
    check(blockdiffCount(&s) == BLOCKDIFF_MAX_PENDING,
          "CONTROL: the store holds exactly its capacity, nothing was evicted to make room");

    /* Now name the blocks. The oldest edit in the replay must have survived; a late one must
     * be provably gone. Reading back a specific coordinate is the point — a refusal count on
     * its own does not tell a player WHICH of their blocks is missing. */
    const int oldest_x = 0;
    const int newest_x = (int)(BLOCKDIFF_SERVER_DIFF_MAX - 1u) * 16;

    static struct applied_log oldest_log; oldest_log.n = 0;
    const int oldest_drained = blockdiffDrain(&s, oldest_x >> 4, 0, logApply, &oldest_log);
    check(oldest_drained == 1 && loggedContains(&oldest_log, oldest_x, 10, 0, BLOCK_STONE),
          "CONTROL: the OLDEST edit of the replay survived and drains back as the block it was");

    static struct applied_log newest_log; newest_log.n = 0;
    const int newest_drained = blockdiffDrain(&s, newest_x >> 4, 0, logApply, &newest_log);
    printf("    [measured] block at x=%d (column %d): drained %d, applied %d\n",
           newest_x, newest_x >> 4, newest_drained, newest_log.n);

    check(newest_drained == 0,
          "the NEWEST edit of the replay is GONE — nothing pending for its column at all");
    check(newest_log.n == 0,
          "and the apply callback was never invoked for it, so no world would ever receive it");
    check(!loggedContains(&newest_log, newest_x, 10, 0, BLOCK_STONE),
          "read-back confirms it: the block the server replayed last is not in this store");

    blockdiffFree(&s);
}

static void test_drain_empties_store(void)
{
    puts("drain leaves the store empty");

    static BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRecord(&s, 1, 1, 1, BLOCK_STONE), "diff A recorded");
    check(blockdiffRecord(&s, 2, 2, 2, BLOCK_SAND), "diff B recorded, same column as A");
    check(blockdiffRecord(&s, 40, 1, 1, BLOCK_WOOD), "diff C recorded, a different column");
    check(blockdiffCount(&s) == 3, "three diffs pending across two columns");

    static struct applied_log log; log.n = 0;
    const int n = blockdiffDrain(&s, 0, 0, logApply, &log);
    check(n == 2, "draining column (0,0) picks up both A and B");
    check(blockdiffCount(&s) == 1, "only C, in the other column, is left pending");

    static struct applied_log log2; log2.n = 0;
    check(blockdiffDrain(&s, 2, 0, logApply, &log2) == 1, "draining C's column picks it up");
    check(blockdiffCount(&s) == 0, "store is now completely empty");

    /* clear() on an already-empty store, and on a populated one, both leave it empty and
     * with a zeroed refusal count. */
    blockdiffClear(&s);
    check(blockdiffCount(&s) == 0 && blockdiffRefusals(&s) == 0, "clear() on an empty store is a no-op");

    check(blockdiffRecord(&s, 5, 5, 5, BLOCK_STONE), "repopulate before the next clear");
    blockdiffClear(&s);
    check(blockdiffCount(&s) == 0, "clear() drops pending diffs without applying them");

    blockdiffFree(&s);
}

/* v1.8.5. The store the console actually runs stopped being a static and became a malloc
 * (net/networld.c's pendingStore()), so blockdiffInit() is now handed memory that has never
 * been zeroed. Everything else in this file inits a `static BlockDiffStore`, which .bss hands
 * over as zeros — so every check above would stay green even if blockdiffClear() forgot a
 * scalar entirely, and the console would be the thing that found out.
 *
 * 0xAA rather than 0 or 0xFF on purpose: 0xFF is BLOCKDIFF_NIL in every uint32_t field, which
 * is a legal end-of-chain marker and would accidentally look correct, and 0 is what the old
 * static already gave. 0xAAAAAAAA is a large in-range-looking index that is not NIL, not zero,
 * and past BLOCKDIFF_MAX_PENDING — the shape of value that turns into a wild read if a chain
 * head or the free list survives init.
 *
 * This drives the store afterwards rather than only reading its counters: a count that reads 0
 * proves one field was written, and the bug this guards against is any of five not being. */
static void test_uninitialised_memory_is_safe(void)
{
    puts("init over uninitialised memory (the malloc'd store the console now runs)");

    static BlockDiffStore garbage;
    memset(&garbage, 0xAA, sizeof garbage);

    blockdiffInit(&garbage);

    check(blockdiffCount(&garbage) == 0, "count reads 0 after init over 0xAA memory");
    check(blockdiffRefusals(&garbage) == 0, "refusals reads 0 after init over 0xAA memory");

    static struct applied_log log; log.n = 0;
    check(blockdiffDrain(&garbage, 0, 0, logApply, &log) == 0,
          "drain finds nothing rather than walking a chain head left as garbage");
    check(log.n == 0, "and applies nothing");

    check(blockdiffRecord(&garbage, 33, 7, 33, BLOCK_STONE),
          "a record into it is accepted, so the free list and high water mark are usable");
    check(blockdiffCount(&garbage) == 1, "and is the store's only entry");

    static struct applied_log log2; log2.n = 0;
    check(blockdiffDrain(&garbage, 33 >> 4, 33 >> 4, logApply, &log2) == 1,
          "it drains back out of the bucket it was filed in");
    check(loggedContains(&log2, 33, 7, 33, BLOCK_STONE),
          "carrying the coordinate and block it went in with, not garbage from around it");
    check(blockdiffCount(&garbage) == 0, "leaving the store empty");

    blockdiffFree(&garbage);
}

/* ------------------------------------------------------------------------- main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    puts("== blockdiff test ==");

    test_record_and_drain();
    test_latest_write_wins();
    test_columns_stay_separate();
    test_y_bounds();
    test_capacity();
    test_server_replay_overflow();
    test_drain_empties_store();
    test_uninitialised_memory_is_safe();

    /* Latched BEFORE the pin's own check() runs, because check() increments g_checks: read
     * after, the pin would be comparing against a total that includes itself and would have to
     * be written one higher than the number of real checks, which is exactly the off-by-one
     * that makes a pin useless to reason about. `ran` is the count of everything above.
     *
     * The pin exists because this project's signature failure is a green suite over a broken
     * feature: a check silently DELETED still leaves "PASS", just with a smaller total, and
     * nobody reads a total they were not given something to compare against. */
    const int ran = g_checks;
    /* 80 -> 89: test_uninitialised_memory_is_safe adds nine, and nothing was removed. Computed
     * from the delta, not pasted off the failing run — see networld_test.c's own guard for why
     * pasting the observed number is the failure this pin exists to catch. */
    check(ran == 89, "check-count pin: 89 checks ran (update deliberately, never to go green)");

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
