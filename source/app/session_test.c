// Host self-test for app/session.c — the per-session reset main.c runs on every lap of its
// `session_start:` label. Self-contained (its own main()), same shape and same reason as
// scene/title_nav_test.c and scene/ui_layout_test.c.
//
// The __3DS__ guard around the *whole file* is load-bearing, not tidy — the same one
// app/options_test.c, app/debugmenu_test.c and app/remap_test.c carry: the Makefile globs
// every .c under source/app into the console build, so without it this file's main() links
// against source/main.c's and the build dies with "multiple definition of `main'".
//
// ── Why this file is shaped the way it is ────────────────────────────────────────────────
//
// net/networld_test.c already had test_registry_table_resets_between_sessions(), and it
// PASSED against the broken tree, because it calls networldInit() itself. That proves
// "networldInit() clears the table", which was never in doubt. It cannot prove anything
// about the path where NOTHING calls networldInit() — which is the single-player
// quit-to-title path, and which is the whole defect. So the checks below do two things
// that test does not:
//
//   1. They walk the caller's REAL sequence. Boot, then a single-player session exactly as
//      genStart() builds one (the world's own dynamic rows, then registryFreeze()), then
//      the lap — sessionBegin() and nothing else, because sessionBegin() and nothing else
//      is what main.c runs between the two worlds — then the join. No convenient extra
//      reset is inserted anywhere, because inserting one is exactly the bug.
//
//   2. They read source/main.c and check the call is actually wired into the lap. A
//      behavioural test of sessionBegin() alone would stay green if main.c never called
//      it, which is the same flaw again one level up. Reading a source file to check a
//      constant that cannot be linked is established here — world/atlas_uv_shader_test.c
//      parses the .pica shader text for the same reason.
//
// Every registry call below is the REAL world/registry.c. registryRemoteApply() in
// particular is the exact function net/networld.c's BS_APP_REGISTRY_DEFS handler ends in
// and the exact function registrySidecarLoad() ends in, so both the join path and the
// single-player sidecar path are being exercised through their own committing call rather
// than through a stand-in for it.
#ifndef __3DS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/session.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

// Printed per failure, not just the first: this project's own test standard is that every
// case of a red run gets read, and a summary naming one of nine hides the other eight.
#define CHECK(cond) do {                                                      \
		s_checks++;                                                            \
		if (!(cond)) {                                                         \
			s_fails++;                                                         \
			printf("  FAIL  L%d %s\n", __LINE__, #cond);                       \
			if (!s_first[0])                                                   \
				snprintf(s_first, sizeof(s_first), "L%d %.140s",               \
				         __LINE__, #cond);                                     \
		}                                                                      \
	} while (0)

// ── the pieces of a session, each one the real call main.c makes ─────────────────────────

// A wire record for one dynamic block, packed by world/registry.c's own packer so the bytes
// are the bytes a server would send and a registry.bin would hold — never a literal.
static void packOne(uint8_t out[REGISTRY_WIRE_RECORD_BYTES], uint8_t id, const char* name)
{
	BlockDef d;
	memset(&d, 0, sizeof d);
	snprintf(d.name, sizeof d.name, "%s", name);
	for (int f = 0; f < BLOCK_FACES; f++) d.tex[f] = BTEX_STONE;
	d.flags      = REG_FLAG_SOLID;
	d.hardness   = 1;
	d.variant_of = id;
	registryDefPack(out, (BlockId)id, &d);
}

// main.c:2519 — networldInit() at boot, whose registry half is this call. Everything below
// starts here because this is where a cold boot leaves the table.
static void boot(void)
{
	registryInitCore();
}

// genStart() (main.c:1008..1086), registry half, in its real order: the world's dynamic rows
// are installed first — from registrySidecarLoad() in single player, from the server's
// BS_APP_REGISTRY_DEFS during the join in a session — and only then is the table frozen,
// before workerStart(). Returns what the row installation returned.
static size_t enterWorldWithRows(const char* const* names, size_t n)
{
	size_t applied = 0;
	if (n > 0) {
		uint8_t recs[4][REGISTRY_WIRE_RECORD_BYTES];
		for (size_t i = 0; i < n && i < 4; i++)
			packOne(recs[i], (uint8_t)(REG_ID_DYN_LO + i), names[i]);
		applied = registryRemoteApply(REG_ID_DYN_LO, &recs[0][0], n);
	}
	registryFreeze();
	return applied;
}

// The teardown at main.c:3763..3842 for a SINGLE-PLAYER world left through the pause menu's
// "Quit to title" row: saves, stops the worker, frees the world, and then
// `if (quit_to_title) goto session_start;`. Nothing in any of that touches the registry —
// which is the defect — so the lap below is the whole of it.
static void quitToTitleFromSinglePlayer(void)
{
	sessionBegin();
}

// The same teardown for a SERVER session (main.c:3833..3836): netDisconnect() first, then
// the same jump to the same label. netDisconnect() (net/bsnet.c:337) calls networldInit(),
// whose registry half is registryInitCore() — so this exit has always been covered, and it
// is here to be COMPARED against the single-player one rather than trusted.
static void quitToTitleFromServerSession(void)
{
	registryInitCore();   // the registry half of netDisconnect() -> networldInit()
	sessionBegin();
}

// What a table looks like from outside, as three numbers that a stale table cannot fake:
// how many rows, what they hash to, and whether it still refuses writes.
typedef struct {
	unsigned count;
	unsigned crc16;
	bool     frozen;
} TableState;

static TableState tableState(void)
{
	TableState s;
	s.count  = registryCount();
	s.crc16  = registryCrc16();
	s.frozen = registryFrozen();
	return s;
}

static bool tableStateEq(TableState a, TableState b)
{
	return a.count == b.count && a.crc16 == b.crc16 && a.frozen == b.frozen;
}

// ── 1. the defect itself, as the player hits it ──────────────────────────────────────────
//
// Boot, play single player, back out to the title without rebooting, join a server. The
// measured symptom before the fix was `frozen=1 / count=8 / find(srv_blk)=0x00 / applied=0`
// against a control of `count=9 / find(srv_blk)=0x80`.
static void testSinglePlayerQuitToTitleLeavesTheTableJoinable(void)
{
	static const char* const sp_rows[] = { "sp_sidecar" };

	boot();
	// Control, green in both arms: the single-player session really did build the table
	// this test is about. Without this a "the table is clean afterwards" pass could mean
	// nothing ever dirtied it.
	CHECK(enterWorldWithRows(sp_rows, 1) == 1);
	CHECK(registryFrozen());
	// Thirty-four core rows since v1.8.12 added six ore rows on top of v1.8.10's torch,
	// v1.8.8's twelve per-biome timber and flora rows, v1.8.3 Phase 3's snow, ice, cactus,
	// dead bush and fern, and tasks 17/19's water and tall grass, plus the one dynamic row
	// this world registered. (The measured symptom quoted above was taken when there were
	// eight core rows; the +1/+0 shape of it is what matters, not the base.)
	CHECK(registryCount() == 35);

	quitToTitleFromSinglePlayer();

	CHECK(!registryFrozen());
	CHECK(registryCount() == 34);
	CHECK(registryFind("sp_sidecar") == 0);

	// The join. This is the batch the server sends and the client refused for the whole
	// session: it starts at REG_ID_DYN_LO, which the single-player world had been holding.
	uint8_t rec[REGISTRY_WIRE_RECORD_BYTES];
	packOne(rec, REG_ID_DYN_LO, "srv_blk");
	CHECK(registryRemoteApply(REG_ID_DYN_LO, rec, 1) == 1);
	CHECK(registryFind("srv_blk") == REG_ID_DYN_LO);
	CHECK(registryCount() == 35);
	// The row at that id is the SERVER's row and usable, not merely a row. Checking
	// registryIsDefined(REG_ID_DYN_LO) alone would have passed against the broken tree —
	// the single-player world's own row was sitting in that slot, defined and solid — and
	// a wrong definition under the right id is precisely the failure the player saw.
	CHECK(registryIsDefined(REG_ID_DYN_LO));
	CHECK(strcmp(registryView(REG_ID_DYN_LO)->name, "srv_blk") == 0);
	CHECK(registryView(REG_ID_DYN_LO)->solid);
}

// ── 2. the same defect one world earlier ─────────────────────────────────────────────────
//
// Single player into single player. The second world's registry.bin is its own, and against
// a frozen table still holding the first world's rows it was refused in silence — so world
// B rendered and collided with world A's block definitions under the same ids.
static void testSinglePlayerIntoSinglePlayerGetsItsOwnTable(void)
{
	static const char* const world_a[] = { "a_marble", "a_basalt" };
	static const char* const world_b[] = { "b_thatch" };

	boot();
	CHECK(enterWorldWithRows(world_a, 2) == 2);
	CHECK(registryFind("a_marble") == REG_ID_DYN_LO);

	quitToTitleFromSinglePlayer();

	CHECK(enterWorldWithRows(world_b, 1) == 1);
	CHECK(registryFind("b_thatch") == REG_ID_DYN_LO);
	CHECK(registryFind("a_marble") == 0);
	CHECK(registryFind("a_basalt") == 0);
	CHECK(registryCount() == 35);
}

// ── 3. every exit from a world lands in the same state ───────────────────────────────────
//
// Demonstrated by comparing them, not asserted. A fix that closes one `goto` and leaves the
// other open makes the bug rarer and therefore harder to believe, so the two exits are put
// side by side against a cold boot and required to be indistinguishable.
static void testEveryExitFromAWorldLandsInTheSameState(void)
{
	static const char* const rows[] = { "leftover" };

	boot();
	const TableState cold = tableState();

	// Exit A: single player, pause menu -> Quit to title.
	boot();
	(void)enterWorldWithRows(rows, 1);
	quitToTitleFromSinglePlayer();
	const TableState after_sp = tableState();

	// Exit B: a server session ending — Quit, a kick, a lost link or HOME all reach the
	// same teardown and the same netDisconnect().
	boot();
	(void)enterWorldWithRows(rows, 1);
	quitToTitleFromServerSession();
	const TableState after_mp = tableState();

	CHECK(tableStateEq(after_sp, cold));
	CHECK(tableStateEq(after_mp, cold));
	CHECK(tableStateEq(after_sp, after_mp));

	// And the number itself, so a "they agree" pass cannot be two identically wrong tables.
	//
	// 10 -> 15 on 2026-08-30: v1.8.3 Phase 3 added snow, ice, cactus, dead bush and fern as
	// core rows 10..14. A cold table is exactly the core rows, so this literal tracks that
	// count and nothing else. It is spelled `cold.count` rather than `registryCount()`, which
	// is why the sweep that moved the five registryCount() pins in this file missed it and
	// the suite caught it instead -- worth recording, because the next core row will hit the
	// same blind spot.
	//
	// 15 -> 27 on 2026-09-02: v1.8.8 appends twelve more core rows, ids 15..26. Same blind
	// spot, same fix.
	//
	// 27 -> 28 on 2026-09-02: v1.8.10 appends the torch, core row 27. Same blind spot again.
	//
	// 28 -> 34 on 2026-09-02/03: v1.8.12 "Ores" appends six ore rows, ids 28..33. Same blind
	// spot again — this literal is `cold.count`, not `registryCount()`, so it was missed by
	// the same kind of sweep that would only grep for the latter.
	CHECK(cold.count == 34 && !cold.frozen);

	// Both exits leave the NEXT FREE SLOT at REG_ID_DYN_LO too, which the three numbers
	// above do not cover: registryRemoteApply() refuses any batch that does not start
	// exactly there, and that refusal is half of what the player experienced.
	uint8_t rec[REGISTRY_WIRE_RECORD_BYTES];

	boot();
	(void)enterWorldWithRows(rows, 1);
	quitToTitleFromSinglePlayer();
	packOne(rec, REG_ID_DYN_LO, "probe_sp");
	CHECK(registryRemoteApply(REG_ID_DYN_LO, rec, 1) == 1);

	boot();
	(void)enterWorldWithRows(rows, 1);
	quitToTitleFromServerSession();
	packOne(rec, REG_ID_DYN_LO, "probe_mp");
	CHECK(registryRemoteApply(REG_ID_DYN_LO, rec, 1) == 1);
}

// ── 4. the first lap, and laps that had nothing to undo ──────────────────────────────────
//
// sessionBegin() runs on every lap including the first, before any world has existed. It
// must be a no-op there rather than something that has to be guarded at the call site.
// Green in both arms by design — the control that says the checks above are not simply
// passing because the table is always empty.
static void testTheResetIsHarmlessWhenThereIsNothingToReset(void)
{
	boot();
	const TableState before = tableState();
	sessionBegin();
	CHECK(tableStateEq(tableState(), before));

	sessionBegin();
	sessionBegin();
	CHECK(tableStateEq(tableState(), before));
	CHECK(registryCount() == 34);
}

// ── 5. main.c really calls it ────────────────────────────────────────────────────────────
//
// The check net/networld_test.c's version could not have: that the reset is wired into the
// lap at all. main.c cannot be linked into a host binary, so its text is read. Deliberately
// three separate facts rather than one grep, so a red run says which one broke.
static char* readWholeFile(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (f == NULL) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	const long n = ftell(f);
	if (n <= 0) { fclose(f); return NULL; }
	rewind(f);
	char* buf = (char*)malloc((size_t)n + 1);
	if (buf == NULL) { fclose(f); return NULL; }
	const size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void testMainCRunsTheResetOnEverySessionLap(void)
{
	// Run from the repository root, the same working directory
	// world/atlas_uv_shader_test.c reads gfx/ and source/ out of.
	char* src = readWholeFile("source/main.c");
	CHECK(src != NULL);
	if (src == NULL) {
		printf("  (source/main.c could not be read from this working directory)\n");
		return;
	}

	CHECK(strstr(src, "#include \"app/session.h\"") != NULL);

	// One label, so "after the label" below means "on every lap" and not "on one of them".
	const char* label = strstr(src, "\nsession_start:");
	CHECK(label != NULL);
	if (label != NULL) {
		CHECK(strstr(label + 1, "\nsession_start:") == NULL);

		const char* call  = strstr(label, "sessionBegin();");
		const char* title = strstr(label, "runTitleScreen(");
		CHECK(call != NULL);
		// Before the menu, not after it: a joining client's BS_APP_REGISTRY_DEFS arrive
		// while the multiplayer screen is still up, so a reset after runTitleScreen() would
		// wipe the rows it was supposed to make room for.
		CHECK(title != NULL);
		CHECK(call != NULL && title != NULL && call < title);
	}

	// Both backward jumps still aim at that one label — the property that makes resetting
	// at the label equivalent to resetting on every exit.
	int gotos = 0;
	for (const char* p = strstr(src, "goto session_start;"); p != NULL;
	     p = strstr(p + 1, "goto session_start;"))
		gotos++;
	CHECK(gotos >= 2);

	free(src);
}

int main(void)
{
	puts("session: the per-session reset on main.c's session_start lap");

	testSinglePlayerQuitToTitleLeavesTheTableJoinable();
	testSinglePlayerIntoSinglePlayerGetsItsOwnTable();
	testEveryExitFromAWorldLandsInTheSameState();
	testTheResetIsHarmlessWhenThereIsNothingToReset();
	testMainCRunsTheResetOnEverySessionLap();

	// ---- check-count guard ---------------------------------------------------------------
	//
	// This suite counts failures, and until 2026-08-25 that was ALL it counted. A suite that
	// only counts failures cannot notice checks that never ran. Measured on net/networld_test.c
	// the same day: shrinking one production constant took it from "PASS 326 checks, 0 failed"
	// to "PASS 318 checks, 0 failed" — both green, exit 0, eight checks silently DELETED rather
	// than failed.
	//
	// This file's own deletion shape is the pair of guarded blocks in
	// testMainCRunsTheResetOnEverySessionLap(): every source-text check about main.c sits behind
	// `if (src == NULL) return;` or `if (label != NULL)`. Rename or remove the session_start
	// label in source/main.c and four checks stop being emitted at all — the one check that goes
	// red says the label is gone, and the four that would have said WHERE the reset sits vanish
	// with it. That is the exact class of failure this file exists to catch, so it must not be
	// able to shrink out from under itself.
	//
	// So: the number below is the count of checks that must already have run by the time control
	// reaches this line. It is a naked literal on purpose — it is the one number in this file
	// that is not derived from anything the tests themselves compute, which is precisely what
	// lets it notice them vanishing. Deriving it from the text of main.c, or from any registry
	// constant, would move it with the very thing it is supposed to be watching; that
	// self-reference is the bug that let networld_test.c's 326 -> 318 hide.
	//
	// HOW TO UPDATE IT WHEN YOU ADD OR REMOVE CHECKS — read this before changing the number:
	//   Work out the delta from what you actually changed (checks added minus checks removed)
	//   and ADD THAT DELTA to the number below. Do NOT paste whatever the failing run printed.
	//   Pasting the observed count is the single failure mode this guard exists to catch: if a
	//   production change silently deleted checks, the printed count is the SYMPTOM, and copying
	//   it in here re-arms the trap and throws away the only evidence you had. If your
	//   recomputed delta and the observed count disagree, that disagreement is a bug report — go
	//   and find out which checks stopped running, and why.
	//
	//   Note the number is the count BEFORE this guard itself, so the summary line prints one
	//   more than it (36 here, 37 on the PASS line). That off-by-one is deliberate: it means
	//   blind-pasting the number off the PASS line lands you a red, not a false green.
	//   Latched into `ran` first, and compared through that, because this file's CHECK is a
	//   MACRO that does s_checks++ BEFORE it evaluates its condition — testing s_checks directly
	//   inside CHECK would be testing a counter the guard had already bumped, and would quietly
	//   want 37. The latch keeps the pin meaning "checks before this line" in the same way it
	//   does in the function-based suites, and so keeps the paste-trap above honest.
	const int ran = s_checks;
	if (ran != 36)
		printf("  CHECK-COUNT GUARD: %d checks ran, %d expected.\n"
		       "    %s\n"
		       "    This is NOT an ordinary assertion failure.\n"
		       "    Read the comment above this guard in app/session_test.c before"
		       " touching the pinned number.\n",
		       ran, 36,
		       ran < 36
		           ? "Checks went MISSING: checks that should have run never ran at all."
		           : "Extra checks appeared: either you added checks and did not update the"
		             " pin, or something is emitting checks it should not.");
	CHECK(ran == 36);

	if (s_fails == 0)
		printf("session self-test: PASS  %d checks\n", s_checks);
	else
		printf("session self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int session_test_host_only_t;

#endif   // !__3DS__
