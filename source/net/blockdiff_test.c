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
 * inspect what was actually applied rather than just trusting the returned count. */
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

    BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffCount(&s) == 0, "fresh store is empty");
    check(blockdiffRecord(&s, 10, 5, 20, BLOCK_STONE), "record accepted");
    check(blockdiffCount(&s) == 1, "count reflects the one pending diff");

    struct applied_log log = {0};
    const int n = blockdiffDrain(&s, 10 >> 4, 20 >> 4, logApply, &log);
    check(n == 1, "drain reports one diff");
    check(log.n == 1, "apply callback invoked once");
    check(loggedContains(&log, 10, 5, 20, BLOCK_STONE),
          "applied diff carries the right coordinate and block");
    check(blockdiffCount(&s) == 0, "store empty after drain");

    /* Draining an empty or unrelated column must be a harmless no-op. */
    struct applied_log log2 = {0};
    check(blockdiffDrain(&s, 10 >> 4, 20 >> 4, logApply, &log2) == 0,
          "draining an already-empty column reports zero");
    check(log2.n == 0, "and never calls apply");

    blockdiffFree(&s);
}

static void test_latest_write_wins(void)
{
    puts("latest write wins");

    BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRecord(&s, 3, 8, 3, BLOCK_STONE), "first write accepted");
    check(blockdiffRecord(&s, 3, 8, 3, BLOCK_GRASS), "second write to same coordinate accepted");
    check(blockdiffCount(&s) == 1, "same coordinate collapses to one entry, not two");

    struct applied_log log = {0};
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

    struct applied_log log2 = {0};
    blockdiffDrain(&s, 7 >> 4, 7 >> 4, logApply, &log2);
    check(log2.n == 1 && log2.id[0] == BLOCK_AIR,
          "place-then-break resolves to air, not the placed block");

    blockdiffFree(&s);
}

static void test_columns_stay_separate(void)
{
    puts("diffs for different columns stay separate");

    BlockDiffStore s;
    blockdiffInit(&s);

    /* Column (0,0) covers x,z in 0..15; column (1,0) covers x in 16..31. */
    check(blockdiffRecord(&s, 4, 10, 4, BLOCK_STONE), "diff for column (0,0)");
    check(blockdiffRecord(&s, 20, 10, 4, BLOCK_SAND), "diff for column (1,0)");
    check(blockdiffCount(&s) == 2, "two distinct columns, two entries");

    struct applied_log log = {0};
    int n = blockdiffDrain(&s, 0, 0, logApply, &log);
    check(n == 1, "draining column (0,0) only touches its own diff");
    check(log.n == 1 && log.x[0] == 4, "and it's the (0,0) diff, not the other one");
    check(blockdiffCount(&s) == 1, "the other column's diff is untouched and still pending");

    struct applied_log log2 = {0};
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

    struct applied_log logz = {0};
    check(blockdiffDrain(&s, 0, 0, logApply, &logz) == 1,
          "draining column (0,0) by z alone finds only its own diff");
    check(logz.n == 1 && logz.z[0] == 4, "and it's the z=4 diff, not the z=20 one");
    check(blockdiffCount(&s) == 1, "the z=20 diff is untouched by a same-x, different-z drain");

    struct applied_log logz2 = {0};
    check(blockdiffDrain(&s, 0, 1, logApply, &logz2) == 1, "draining column (0,1) now finds it");
    check(logz2.n == 1 && logz2.z[0] == 20, "and it's the right one");

    /* Negative coordinates: column coordinate is x >> 4 / z >> 4, an arithmetic shift, the
     * same convention world/world.c uses. Block -1 belongs to column -1, not column 0. */
    blockdiffClear(&s);
    check(blockdiffRecord(&s, -1, 5, -1, BLOCK_STONE), "diff at a negative coordinate");
    check(blockdiffDrain(&s, 0, 0, logApply, &(struct applied_log){0}) == 0,
          "column (0,0) does not see a block at x=-1");
    struct applied_log log3 = {0};
    check(blockdiffDrain(&s, -1, -1, logApply, &log3) == 1,
          "column (-1,-1) is where the negative-coordinate diff actually lives");

    blockdiffFree(&s);
}

static void test_y_bounds(void)
{
    puts("out-of-range y rejected");

    BlockDiffStore s;
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

    BlockDiffStore s;
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
    struct applied_log log = {0};
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

static void test_drain_empties_store(void)
{
    puts("drain leaves the store empty");

    BlockDiffStore s;
    blockdiffInit(&s);

    check(blockdiffRecord(&s, 1, 1, 1, BLOCK_STONE), "diff A recorded");
    check(blockdiffRecord(&s, 2, 2, 2, BLOCK_SAND), "diff B recorded, same column as A");
    check(blockdiffRecord(&s, 40, 1, 1, BLOCK_WOOD), "diff C recorded, a different column");
    check(blockdiffCount(&s) == 3, "three diffs pending across two columns");

    struct applied_log log = {0};
    const int n = blockdiffDrain(&s, 0, 0, logApply, &log);
    check(n == 2, "draining column (0,0) picks up both A and B");
    check(blockdiffCount(&s) == 1, "only C, in the other column, is left pending");

    struct applied_log log2 = {0};
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
    test_drain_empties_store();

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
