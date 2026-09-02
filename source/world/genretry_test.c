// Host probe and regression test for world/genretry.c — the ledger that turns a budget-refused
// column into a recoverable hole instead of a permanent one.
//
// ── what this reproduces ──────────────────────────────────────────────────────────────────
//
// main.c's genInstallOne() marks a column installed whether or not world.c's
// worldColumnCreate()/worldChunkCreate() got every one of its 8 chunks in — it has to, or the
// column would stall its neighbours' ring-complete check forever (see genretry.h's header).
// What the pre-fix code never did is look at that column again: genRequestArea() only asks for
// a column that is not already marked installed, and a column that failed IS marked installed,
// permanently. The only thing that ever clears the mark is the player walking far enough for
// the streaming ring to drop and later re-request the column — "I am one chunk away and it is
// unloaded, only when I get close does it load in."
//
// main.c cannot be linked here — it includes <3ds.h> and carries main() — so this probe cannot
// call genInstallOne() itself. What it CAN do, and does, is call the exact real functions the
// bug lives in: world.c's worldColumnCreate()/worldChunkCreate() against world/budget.c's real
// budgetClaim(), through genColumnAttempt() below, which models worldgenColumn()'s documented
// allocation contract (app/worker.h: "false in *ok means the budget or column table refused
// part of the column") without needing a seed, a WorldGenScratch, or any terrain math. Two
// scenarios are then driven through that same real mechanism: one that never calls
// genretry.c's ledger (the pre-fix shape — proves the hole is real and permanent), and one that
// does (the fix — proves it heals once budget allows, without spinning or double-claiming).
//
// No <3ds.h>, own main(), same pattern as tests/world_budget_bytes_test.c.
//
// The __3DS__ guard below is load-bearing rather than tidy, and the world_budget_bytes_test.c
// this file copied does NOT need it: that one lives in tests/, which the Makefile never globs.
// This one lives in source/world/, which IS in SOURCES, so every .c here is compiled into the
// console build. Without the guard this file's main() meets source/main.c's and the link dies
// with "multiple definition of `main'" — measured, exactly that way, on the first console build
// attempted after this file was added. Every other _test.c under source/ carries the same guard
// for the same reason; see the note at the top of app/options_test.c.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "world/budget.h"
#include "world/chunk.h"
#include "world/genretry.h"
#include "world/world.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                 \
		if (!(cond)) {                                                              \
			s_fails++;                                                              \
			printf("  FAIL L%d: %s\n", __LINE__, #cond);                           \
			if (!s_first[0])                                                        \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond); \
		}                                                                            \
	} while (0)

// A MODEL of worldgenColumn's allocation contract (world/worldgen.c, world/worldgen_density.c):
// create the column, then create every one of its COLUMN_CHUNKS chunks, continuing past a
// refusal rather than bailing out (both real generators do the same — a chunk that fits is not
// skipped just because an earlier one did not). Chunks that already exist (a retry landing on a
// column an earlier attempt partly filled) are left alone, which is worldChunkCreate's own
// short-circuit (world.c: "if (col->chunks[cy]) return col->chunks[cy];") doing the "not a
// double-claim" work — this function does nothing extra to get that property.
static bool genColumnAttempt(World* w, int32_t cx, int32_t cz)
{
	if (!worldColumnCreate(w, cx, cz)) return false;

	bool ok = true;
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		if (worldChunk(w, cx, cy, cz)) continue;
		if (!worldChunkCreate(w, cx, cy, cz)) ok = false;
	}
	return ok;
}

static int columnChunkCount(const World* w, int32_t cx, int32_t cz)
{
	int n = 0;
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++)
		if (worldChunk(w, cx, cy, cz)) n++;
	return n;
}

// Claims bytes with no column behind them, purely to occupy budget the way "every other loaded
// column in the ring" does in the real game — the mechanism genColumnAttempt's refusal has to
// come from is budgetClaim() saying no, exactly as it would with 168 real neighbours loaded.
static void hogBudget(size_t bytes)
{
	CHECK(budgetClaim(bytes));
}

// ── 1. The mechanism: a real budget refusal leaves a real partial column ──────────────────
static void testPartialColumnIsReal(void)
{
	World w;
	worldInit(&w);
	budgetReset();

	const size_t col_bytes   = sizeof(Column);
	const size_t chunk_bytes = chunkFormBytes(CHUNK_FORM_UNIFORM);

	// Room for the column and exactly 3 of its 8 chunks.
	hogBudget(WORLD_BUDGET_BYTES - (col_bytes + 3 * chunk_bytes));

	const bool ok = genColumnAttempt(&w, 0, 0);
	CHECK(!ok);
	CHECK(worldColumn(&w, 0, 0) != NULL);
	CHECK(columnChunkCount(&w, 0, 0) == 3);

	worldExit(&w);
	budgetReset();
}

// ── 2. Pre-fix reproduction: nothing ever revisits a failed column ────────────────────────
//
// This is genInstallOne()'s ACTUAL pre-fix shape: on failure it marks the column installed
// (modelled here by simply not undoing the partial worldColumnCreate/worldChunkCreate above)
// and calls nothing else. No genretry.c function is called anywhere in this function — that
// omission IS the bug, not a stand-in for it. The budget is then freed exactly the way it would
// be in the real game (another column leaving the streaming ring), and the probe waits a
// thousand simulated frames doing nothing, because that is what main.c did before this fix:
// nothing. If this ever prints "healed" without genretry.c wired in, the bug this file exists
// to reproduce is not what the comments upstream (main.c:1603-1606) describe, and the fix below
// would be solving the wrong problem.
static bool demoOldBehaviorPermanentHole(void)
{
	World w;
	worldInit(&w);
	budgetReset();

	const size_t col_bytes   = sizeof(Column);
	const size_t chunk_bytes = chunkFormBytes(CHUNK_FORM_UNIFORM);
	hogBudget(WORLD_BUDGET_BYTES - (col_bytes + 3 * chunk_bytes));

	const bool ok = genColumnAttempt(&w, 7, -3);
	const int before = columnChunkCount(&w, 7, -3);
	printf("  old: first attempt ok=%d, %d/%d chunks in\n", ok, before, COLUMN_CHUNKS);

	// The ring moving on: some other column's unload frees the budget this one needed.
	budgetRelease(WORLD_BUDGET_BYTES - (col_bytes + 3 * chunk_bytes));
	printf("  old: budget freed (used %zu of %u)\n", budgetUsed(), WORLD_BUDGET_BYTES);

	// A thousand frames, genuinely nothing called — the pre-fix main.c has no code path that
	// would touch this column again short of the player unloading and re-requesting it.
	for (int frame = 0; frame < 1000; frame++) { /* nothing: this is the bug */ }

	const int after = columnChunkCount(&w, 7, -3);
	const bool healed = (after == COLUMN_CHUNKS);
	printf("  old: after budget freed + 1000 frames, %d/%d chunks in -> %s\n",
	       after, COLUMN_CHUNKS, healed ? "HEALED" : "STILL A HOLE");

	worldExit(&w);
	budgetReset();
	return healed;
}

// ── 3. The fix: genretry.c wired the way main.c's genInstallOne/genRetryTick now are ──────

// File-scope so submitOnly/installPending (plain function pointer targets, not closures) can
// reach it. The real caller, main.c, does not need a global for this — it has exactly one
// ledger and one genInstallOne, and passes its own state through genRetryTick's `ud` instead;
// this probe runs several independent scenarios through the same callback, so its ledger is the
// one thing each test resets via genRetryInit() rather than passing through `ud` for no reason.
static GenRetryLedger g_fix_ledger;

typedef struct {
	World*  w;
	int     attempts;      // how many times submitOnly actually fired — the number the
	                        // "does it spin" check reads
	bool    has_pending;
	int32_t pending_cx, pending_cz;
} FixHarness;

// The two halves of what one real frame pair does, kept as two separate, non-reentrant calls
// instead of one — see below for why that separation is load-bearing, not cosmetic.
//
// submitOnly models workerSubmitColumn: genRetryTick calls this synchronously and it only
// enqueues (h->has_pending), exactly like the real submit — it must NOT call genRetryMark or
// genRetryClear itself. The job ring is never full in this probe, so it always accepts.
static bool submitOnly(void* ud, int32_t cx, int32_t cz)
{
	FixHarness* h = ud;
	h->attempts++;
	h->has_pending = true;
	h->pending_cx = cx;
	h->pending_cz = cz;
	return true;
}

// installPending models genInstallOne() draining a completed worker job on a later frame: runs
// the actual generation attempt and reports the outcome to the ledger via genRetryMark/
// genRetryClear — the two calls main.c's genInstallOne makes after workerInstall() reports ok.
//
// This must be called from OUTSIDE genRetryTick's own submit() call, never from inside it. An
// earlier version of this harness called genRetryMark/genRetryClear synchronously from inside
// the submit callback itself (collapsing "submit" and "install completes" into one call) and it
// produced a real, reproducible failure: genRetryTick's own `if (submit(...)) s->inflight =
// true;` runs AFTER submit() returns, so it clobbered the inflight=false that genRetryMark had
// just set moments earlier inside that same call, permanently wedging the slot inflight with
// nothing left to ever clear it — h.attempts stuck at 1 instead of climbing to 2, then the
// healing loop timing out at the 301-frame cap with the column still 3/8 chunks in. That is not
// a bug in genRetryTick: production's submit callback (workerSubmitColumn) never calls
// genRetryMark/genRetryClear reentrantly like that — only genInstallOne does, on a separate,
// later frame — so the fix belongs here, in the harness, not in genretry.c. Keeping submitOnly
// and installPending as two calls is what makes that true in the test too.
static void installPending(FixHarness* h)
{
	if (!h->has_pending) return;
	h->has_pending = false;

	const bool ok = genColumnAttempt(h->w, h->pending_cx, h->pending_cz);
	if (ok) genRetryClear(&g_fix_ledger, h->pending_cx, h->pending_cz);
	else    genRetryMark(&g_fix_ledger, h->pending_cx, h->pending_cz);
}

// One simulated frame: genRetryTick (may call submitOnly), then the worker "reporting back" —
// in production these can land frames apart; collapsing them to adjacent calls only changes how
// many simulated frames healing takes, not whether it spins or double-claims, which is what this
// test is checking.
static void tickAndInstall(int radius, FixHarness* h)
{
	genRetryTick(&g_fix_ledger, 0, 0, radius, submitOnly, h);
	installPending(h);
}

static void testFixHeals(void)
{
	World w;
	worldInit(&w);
	budgetReset();
	CHECK(genRetryInit(&g_fix_ledger, 13));   // GEN_AREA_SPAN at RENDER_DIST_MAX 5 (13x13)

	const size_t col_bytes   = sizeof(Column);
	const size_t chunk_bytes = chunkFormBytes(CHUNK_FORM_UNIFORM);
	const size_t hog_bytes   = WORLD_BUDGET_BYTES - (col_bytes + 3 * chunk_bytes);
	hogBudget(hog_bytes);

	FixHarness h = {&w, 0, false, 0, 0};

	// The first attempt — genInstallOne's normal, non-retry path.
	const bool ok0 = genColumnAttempt(&w, 7, -3);
	CHECK(!ok0);
	CHECK(columnChunkCount(&w, 7, -3) == 3);
	genRetryMark(&g_fix_ledger, 7, -3);
	CHECK(genRetryOutstanding(&g_fix_ledger) == 1);

	// ── not-spin: while the budget stays tight, genRetryTick must not resubmit every frame.
	// GEN_RETRY_BASE_FRAMES is 30, so across 29 frames of backoff it must submit zero times.
	for (int frame = 0; frame < GEN_RETRY_BASE_FRAMES - 1; frame++)
		tickAndInstall(12, &h);
	CHECK(h.attempts == 0);
	CHECK(genRetryOutstanding(&g_fix_ledger) == 1);   // still a hole, still budget-starved

	// One more frame reaches the backoff and resubmits — but budget is STILL tight, so it
	// fails again, and genRetryMark (called from installPending, the frame after) must back
	// off FURTHER rather than resubmit next frame.
	tickAndInstall(12, &h);
	CHECK(h.attempts == 1);
	CHECK(genRetryOutstanding(&g_fix_ledger) == 1);   // same hole, not a second one

	for (int frame = 0; frame < 2 * GEN_RETRY_BASE_FRAMES - 1; frame++)
		tickAndInstall(12, &h);
	CHECK(h.attempts == 1);   // the doubled backoff (60 frames) has not elapsed yet

	tickAndInstall(12, &h);
	CHECK(h.attempts == 2);   // it just elapsed

	// ── now free the budget, the way another column unloading would ───────────────────────
	budgetRelease(hog_bytes);
	printf("  new: budget freed (used %zu of %u)\n", budgetUsed(), WORLD_BUDGET_BYTES);

	// Run frames until the next scheduled retry fires (backoff is now 120 frames after two
	// failures) — well under the seconds a player spends walking one column, and nowhere near
	// "never", which is what the old arm measured.
	int frames_to_heal = 0;
	while (genRetryOutstanding(&g_fix_ledger) > 0 && frames_to_heal < GEN_RETRY_MAX_FRAMES + 1) {
		tickAndInstall(12, &h);
		frames_to_heal++;
	}

	CHECK(genRetryOutstanding(&g_fix_ledger) == 0);
	CHECK(columnChunkCount(&w, 7, -3) == COLUMN_CHUNKS);
	printf("  new: healed after %d more frames (%d generation attempts total), %d/%d chunks in\n",
	       frames_to_heal, h.attempts, columnChunkCount(&w, 7, -3), COLUMN_CHUNKS);

	// ── not-double-claim: budget used for one fully-installed column matches a clean
	// one-shot install of an unrelated column from an empty budget, exactly.
	const size_t used_after_retry = budgetUsed();
	worldColumnRemove(&w, 7, -3);
	budgetReset();
	CHECK(genColumnAttempt(&w, 99, 99));
	CHECK(columnChunkCount(&w, 99, 99) == COLUMN_CHUNKS);
	CHECK(budgetUsed() == used_after_retry);
	printf("  new: retried column cost %zu B, a clean install costs %zu B (match)\n",
	       used_after_retry, budgetUsed());

	worldExit(&w);
	budgetReset();
}

// ── 4. Ledger unit tests, independent of world.c/budget.c ─────────────────────────────────
static bool submitAlwaysAccept(void* ud, int32_t cx, int32_t cz)
{
	(void)cx; (void)cz;
	int* n = ud;
	(*n)++;
	return true;
}

static bool submitAlwaysRefuse(void* ud, int32_t cx, int32_t cz)
{
	(void)cx; (void)cz;
	int* n = ud;
	(*n)++;
	return false;
}

static void testLedgerUnit(void)
{
	GenRetryLedger led;

	CHECK(!genRetryInit(&led, 0));           // span must be positive
	CHECK(!genRetryInit(&led, 1000));        // span*span must fit GEN_RETRY_SLOTS
	CHECK(genRetryInit(&led, 13));

	// clearing nothing is a documented no-op, not a crash
	CHECK(!genRetryClear(&led, 1, 1));
	CHECK(genRetryOutstanding(&led) == 0);

	// a new failure is one outstanding hole; a repeat of the SAME column is not a second one
	genRetryMark(&led, 1, 1);
	CHECK(genRetryOutstanding(&led) == 1);
	genRetryMark(&led, 1, 1);
	CHECK(genRetryOutstanding(&led) == 1);

	// a different column IS a second hole
	genRetryMark(&led, 2, 2);
	CHECK(genRetryOutstanding(&led) == 2);

	CHECK(genRetryClear(&led, 1, 1));
	CHECK(genRetryOutstanding(&led) == 1);
	CHECK(genRetryClear(&led, 2, 2));
	CHECK(genRetryOutstanding(&led) == 0);

	// geometry drop: a pending entry outside `radius` of the tick's center is forgotten, not
	// retried, and the outstanding count follows it down
	int n = 0;
	CHECK(genRetryInit(&led, 13));
	genRetryMark(&led, 100, 100);
	CHECK(genRetryOutstanding(&led) == 1);
	genRetryTick(&led, 0, 0, 6, submitAlwaysAccept, &n);
	CHECK(n == 0);                             // never even offered — out of radius
	CHECK(genRetryOutstanding(&led) == 0);

	// job-ring-full path: submit returning false leaves the entry pending at cooldown 0, so
	// the very next tick offers it again rather than waiting out a whole backoff
	CHECK(genRetryInit(&led, 13));
	genRetryMark(&led, 3, 3);
	for (int i = 0; i < GEN_RETRY_BASE_FRAMES; i++)
		genRetryTick(&led, 0, 0, 6, submitAlwaysRefuse, &n);
	n = 0;
	genRetryTick(&led, 0, 0, 6, submitAlwaysRefuse, &n);
	CHECK(n == 1);
	genRetryTick(&led, 0, 0, 6, submitAlwaysRefuse, &n);
	CHECK(n == 2);   // still offered every frame — a ring-full refusal is not backed off
	CHECK(genRetryOutstanding(&led) == 1);

	// inflight: once submit accepts, the entry is not offered again until genRetryMark or
	// genRetryClear says what happened to it
	CHECK(genRetryInit(&led, 13));
	genRetryMark(&led, 4, 4);
	for (int i = 0; i < GEN_RETRY_BASE_FRAMES; i++)
		genRetryTick(&led, 0, 0, 6, submitAlwaysRefuse, &n);
	n = 0;
	genRetryTick(&led, 0, 0, 6, submitAlwaysAccept, &n);
	CHECK(n == 1);
	CHECK(genRetryOutstanding(&led) == 1);   // still a hole — nobody has resolved it yet
	for (int i = 0; i < 1000; i++)
		genRetryTick(&led, 0, 0, 6, submitAlwaysAccept, &n);
	CHECK(n == 1);   // inflight — never offered again on its own
	CHECK(genRetryClear(&led, 4, 4));
	CHECK(genRetryOutstanding(&led) == 0);

	printf("  ledger unit tests done\n");
}

int main(void)
{
	printf("gen retry self-test\n");

	testPartialColumnIsReal();

	printf("-- reproducing the pre-fix bug (main.c before this change) --\n");
	const bool healed_without_fix = demoOldBehaviorPermanentHole();
	CHECK(!healed_without_fix);   // the bug must actually reproduce, or the fix below proves nothing
	printf("%s\n", healed_without_fix
	       ? "  UNEXPECTED: healed with no retry code — the reproduction above is not modelling the real bug"
	       : "  bug reproduced: hole is permanent with no retry code, exactly as main.c pre-fix behaves");

	printf("-- proving the fix (genretry.c, wired the way main.c now is) --\n");
	testFixHeals();

	printf("-- genretry.c ledger unit tests --\n");
	testLedgerUnit();

	printf("gen retry self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}

#endif   // !__3DS__
