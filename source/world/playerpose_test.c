/* playerpose_test — host unit test for the single-player pose sidecar (task 46b).
 *
 * Own binary rather than more checks in world/world_test.c, for the reason
 * source/net/Makefile.blockdiff-test gives about blockdiff_test: this is a self-contained
 * contract with its own failure modes, and a suite that is one file per contract says
 * which contract broke without anybody reading a line number.
 *
 * Three kinds of check live here and the split matters:
 *
 *   1. THE FILE FORMAT. Round trip, and every degradation path returning the same clean
 *      "no pose saved". These run the real world/playerpose.c against a real directory on
 *      a real filesystem — no doubles, because there is nothing here a double could stand
 *      in for that would not also stand in for the bug.
 *
 *   2. THE UN-STICK. A pose saved where a later edit filled in solid must come back
 *      standing, not buried. This is the whole point of the task — roadmap task 46's bug
 *      in a new costume is the thing 46b is most likely to reintroduce — so it is checked
 *      against a real World with real blocks in it, not against arithmetic.
 *
 *   3. THAT main.c ACTUALLY CALLS ANY OF IT. main.c carries main() and includes <3ds.h>,
 *      so it cannot be linked here and has no host test at all; roadmap tasks 12 and 20c
 *      both shipped real bugs for exactly that reason. So its text is read, the way
 *      app/session_test.c:298-336 reads it. Every behavioural check in 1 and 2 passes
 *      against a game that never calls playerPoseSave or playerPoseLoad even once.
 *
 * The __3DS__ guard is load-bearing, not tidy — the same one net/blockdiff_test.c carries.
 * mc/Makefile globs every .c under source/world into the console build, so without it this
 * file's main() links against source/main.c's and the build dies with "multiple definition
 * of `main'".
 */
#ifndef __3DS__

/* mkdir(2) and the S_IRWXU macros are POSIX, and -std=c11 (not gnu11) makes glibc hide
 * them behind this. Same reason the tree builds strict everywhere else. */
#define _POSIX_C_SOURCE 200809L

#include "world/playerpose.h"

#include "world/block.h"
#include "world/crc32.h"
#include "world/registry.h"
#include "world/world.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) checkAt((cond), #cond, __LINE__)

static void checkAt(bool cond, const char* what, int line)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("  FAIL  L%d %s\n", line, what);
	}
}

/* ── the scratch world directory ─────────────────────────────────────────────────────
 *
 * Under build-host/ rather than /tmp, because tools/run_host_tests.sh already owns that
 * directory and cleans it, and because a test that writes outside the tree is a test that
 * behaves differently on the one machine where it matters. Both files are removed before
 * every scenario so no check can pass on a leftover from the one above it.
 */
#define POSE_DIR "build-host/t46b-posedir"

static const char* poseFile(void) { return POSE_DIR "/player.dat"; }
static const char* poseTmp(void)  { return POSE_DIR "/player.dat.tmp"; }

static void freshDir(void)
{
	mkdir("build-host", 0777);
	mkdir(POSE_DIR, 0777);
	remove(poseFile());
	remove(poseTmp());
}

static bool writeBytes(const char* path, const uint8_t* buf, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static size_t readBytes(const char* path, uint8_t* buf, size_t cap)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	const size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return n;
}

static bool fileExists(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

/* Two floats are the same pose only if they are the same BITS. Comparing with == would
 * call 0.0f and -0.0f equal and would call two different NaNs unequal, and this suite has
 * checks about both. */
static uint32_t bits(float v)
{
	uint32_t u;
	memcpy(&u, &v, sizeof u);
	return u;
}

static bool poseBitsEqual(const PlayerPose* a, const PlayerPose* b)
{
	return bits(a->x)   == bits(b->x)   && bits(a->y)     == bits(b->y) &&
	       bits(a->z)   == bits(b->z)   && bits(a->yaw)   == bits(b->yaw) &&
	       bits(a->pitch) == bits(b->pitch);
}

/* ── a hand-built 32-byte file, so the corruption cases can be exact ─────────────────
 *
 * Deliberately NOT built by calling playerPoseSave and then poking the result: the point
 * of several checks below is that a specific byte is wrong, and a helper that shares the
 * writer's idea of where that byte is would move with it. This encodes the layout from
 * playerpose.h's comment independently, so a load that starts reading the wrong offset is
 * caught rather than tracked.
 */
#define POSE_BYTES 32

static void put32le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void putF32(uint8_t* p, float v) { put32le(p, bits(v)); }

static void buildFile(uint8_t* buf, uint32_t magic, uint32_t version, const PlayerPose* p)
{
	putF32(buf + 0x0C, p->x);
	putF32(buf + 0x10, p->y);
	putF32(buf + 0x14, p->z);
	putF32(buf + 0x18, p->yaw);
	putF32(buf + 0x1C, p->pitch);
	put32le(buf + 0x00, magic);
	put32le(buf + 0x04, version);
	put32le(buf + 0x08, crc32(buf + 0x0C, POSE_BYTES - 0x0C));
}

/* "BSP1" little-endian, spelled out here rather than taken from the module, so a magic
 * changed in playerpose.c without meaning to is caught here instead of agreeing with
 * itself. */
#define EXPECT_MAGIC 0x31505342u

static const PlayerPose kRef = { 8.5f, 40.0f, 8.5f, 0.0f, 0.0f };

/* ── 1. round trip ─────────────────────────────────────────────────────────────────── */

static void testRoundTripIsBitExact(void)
{
	freshDir();

	const PlayerPose in = { -123.375f, 71.0f, 4096.125f, 2.3561945f, -0.6108652f };
	CHECK(playerPoseSave(&in, POSE_DIR));

	PlayerPose out = { 0, 0, 0, 0, 0 };
	CHECK(playerPoseLoad(&out, POSE_DIR));
	CHECK(bits(out.x)     == bits(in.x));
	CHECK(bits(out.y)     == bits(in.y));
	CHECK(bits(out.z)     == bits(in.z));
	CHECK(bits(out.yaw)   == bits(in.yaw));
	CHECK(bits(out.pitch) == bits(in.pitch));

	/* The file the writer actually produced is the layout the header documents. Without
	 * this, a writer and a reader that agree with each other on a wrong offset round-trip
	 * perfectly and nothing notices until a save from a different build is read. */
	uint8_t got[POSE_BYTES + 8];
	const size_t n = readBytes(poseFile(), got, sizeof got);
	CHECK(n == POSE_BYTES);
	if (n == POSE_BYTES) {
		uint8_t want[POSE_BYTES];
		buildFile(want, EXPECT_MAGIC, 1u, &in);
		CHECK(memcmp(got, want, POSE_BYTES) == 0);
	}

	/* The tmp file is gone once the save has landed — a leftover would be promoted by the
	 * recover pass the next time a real file went missing. */
	CHECK(!fileExists(poseTmp()));
}

/* The control for the DEGRADATION arms: a clean save and load of an ordinary pose, which
 * must stay green while any of the refusal checks below is being sabotaged. It is not a
 * control for the writer or the checksum — measured, and stated rather than implied: with
 * the crc computed over the wrong range this goes red along with everything else, because
 * a broken writer breaks every valid case there is.
 *
 * The suite's true cross-arm control is testMainCIsWired() at the bottom. It shares no code
 * with world/playerpose.c at all, and across twelve sabotages of that file it stayed green
 * in every one — which is what says a red run here is about the sabotage and not about the
 * binary having fallen over. */
static void testControlCleanSaveLoad(void)
{
	freshDir();
	CHECK(playerPoseSave(&kRef, POSE_DIR));

	PlayerPose out = { 0, 0, 0, 0, 0 };
	CHECK(playerPoseLoad(&out, POSE_DIR));
	CHECK(poseBitsEqual(&out, &kRef));
}

/* ── 2. every degradation path is the same clean "no pose saved" ────────────────────── */

/* The sentinel the out-parameter is filled with before every refusal case. "false leaves
 * *out untouched" is half the contract — a caller that keeps its own spawn choice on false
 * is reading a struct the load may have half-written otherwise. */
static const PlayerPose kUntouched = { -999.0f, -999.0f, -999.0f, -999.0f, -999.0f };

static void expectRefused(const char* what)
{
	PlayerPose out = kUntouched;
	const bool ok = playerPoseLoad(&out, POSE_DIR);
	g_checks++;
	if (ok) { g_fails++; printf("  FAIL  refused: %s (load returned true)\n", what); }
	g_checks++;
	if (!poseBitsEqual(&out, &kUntouched)) {
		g_fails++;
		printf("  FAIL  untouched: %s (out was written)\n", what);
	}
}

static void testMissingFileIsRefused(void)
{
	freshDir();
	expectRefused("no player.dat at all");
}

/* Measured, and the two halves of the length check are not equally load-bearing. Dropping
 * the length check alone leaves the 33-byte case red and the 31-byte case GREEN — a
 * truncated file loses payload the crc covers, so the checksum catches it one line later.
 * The 31-byte case only goes red when the length check AND the crc comparison are both
 * removed (arm S18: FAIL 6/108, first "refused: 31 bytes"). So this is defence in depth
 * behind the checksum, and the trailing-bytes case below is the half that stands alone. */
static void testShortFileIsRefused(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, &kRef);
	CHECK(writeBytes(poseFile(), buf, POSE_BYTES - 1));
	expectRefused("31 bytes");
}

static void testTrailingBytesAreRefused(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES + 1];
	buildFile(buf, EXPECT_MAGIC, 1u, &kRef);
	buf[POSE_BYTES] = 0x00;
	CHECK(writeBytes(poseFile(), buf, POSE_BYTES + 1));
	expectRefused("33 bytes");
}

static void testBadMagicIsRefused(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES];
	buildFile(buf, 0x31495342u /* "BSI1", the inventory's */, 1u, &kRef);
	CHECK(writeBytes(poseFile(), buf, POSE_BYTES));
	expectRefused("magic BSI1");
}

static void testBadVersionIsRefused(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES];
	buildFile(buf, EXPECT_MAGIC, 2u, &kRef);
	CHECK(writeBytes(poseFile(), buf, POSE_BYTES));
	expectRefused("version 2");
}

static void testBadCrcIsRefused(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, &kRef);
	buf[0x11] ^= 0x01u;   /* one bit inside y, after the crc was computed */
	CHECK(writeBytes(poseFile(), buf, POSE_BYTES));
	expectRefused("one payload bit flipped");
}

/* The check that stands between a corrupt file and the void. A NaN reaching bodyInit()
 * propagates through every physics comparison as false, so the player falls forever with
 * no error printed anywhere — the worst failure this file can produce. */
static void testNonFiniteIsRefused(void)
{
	static const struct { const char* what; PlayerPose p; } cases[] = {
		{ "x NaN",       { NAN,   40.0f, 8.5f,  0.0f, 0.0f } },
		{ "y +inf",      { 8.5f,  INFINITY, 8.5f, 0.0f, 0.0f } },
		{ "z -inf",      { 8.5f,  40.0f, -INFINITY, 0.0f, 0.0f } },
		{ "yaw NaN",     { 8.5f,  40.0f, 8.5f,  NAN,  0.0f } },
		{ "pitch NaN",   { 8.5f,  40.0f, 8.5f,  0.0f, NAN  } },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		freshDir();
		uint8_t buf[POSE_BYTES];
		buildFile(buf, EXPECT_MAGIC, 1u, &cases[i].p);
		CHECK(writeBytes(poseFile(), buf, POSE_BYTES));
		expectRefused(cases[i].what);
	}
}

/* The y bound, both ends, with both legal neighbours kept as controls in the same test —
 * a bound written as < instead of <= flips exactly one of the four. */
static void testYRangeEnds(void)
{
	static const struct { float y; bool valid; } cases[] = {
		{ -1.0f,                     false },
		{ 0.0f,                      true  },
		{ (float)(WORLD_HEIGHT - 1), true  },
		{ (float)WORLD_HEIGHT,       false },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		freshDir();
		PlayerPose p = kRef;
		p.y = cases[i].y;
		uint8_t buf[POSE_BYTES];
		buildFile(buf, EXPECT_MAGIC, 1u, &p);
		CHECK(writeBytes(poseFile(), buf, POSE_BYTES));

		PlayerPose out = kUntouched;
		const bool ok = playerPoseLoad(&out, POSE_DIR);
		g_checks++;
		if (ok != cases[i].valid) {
			g_fails++;
			printf("  FAIL  y=%.1f expected %s\n", (double)cases[i].y,
			       cases[i].valid ? "valid" : "refused");
		}
	}
}

/* ⚠ Half of this scenario is belt-and-braces and is labelled so rather than left to look
 * like proof. The two NULL-POINTER checks are real: with the `!out` / `!pose` guards
 * removed, playerPoseLoad(NULL, dir) and playerPoseSave(NULL, dir) segfault, and a crashed
 * binary is as red as a failed check. The NULL and EMPTY DIRECTORY checks are not: with
 * dirUsable() removed they still pass, because "%s/player.dat" against NULL or "" resolves
 * to a path this process cannot open anyway and the save fails one layer down. Measured —
 * the arm was run and stayed green at 107/107. They document the intent (a server session
 * passes NULL here precisely so this console gains no file from a world it does not own)
 * and they would catch a future rewrite that made an empty directory mean "here", but they
 * are not evidence that the guard is doing the work today. */
static void testNullAndEmptyDirs(void)
{
	freshDir();

	CHECK(!playerPoseSave(&kRef, NULL));
	CHECK(!playerPoseSave(&kRef, ""));
	CHECK(!playerPoseSave(NULL, POSE_DIR));
	/* Nothing was written by any of the three — the server-session path passes NULL here
	 * precisely so this console gains no file from a world it does not own. */
	CHECK(!fileExists(poseFile()));
	CHECK(!fileExists(poseTmp()));

	PlayerPose out = kUntouched;
	CHECK(!playerPoseLoad(&out, NULL));
	CHECK(!playerPoseLoad(&out, ""));
	CHECK(poseBitsEqual(&out, &kUntouched));

	// The NULL-out check goes LAST and against a directory that really does hold a valid
	// pose file, which is the whole difference between a check and a decoration. Called
	// against an empty directory it returns false at the fopen, long before it would have
	// written through the pointer — measured: with the `!out` guard removed that version of
	// this check stayed green at 107/107. With a loadable file in place the guard is the only
	// thing standing between here and the store, so removing it segfaults.
	CHECK(playerPoseSave(&kRef, POSE_DIR));
	CHECK(!playerPoseLoad(NULL, POSE_DIR));
}

/* ── 3. the interrupted-save window ─────────────────────────────────────────────────── */

static void testTmpIsPromotedWhenTheRealFileIsGone(void)
{
	freshDir();
	uint8_t buf[POSE_BYTES];
	buildFile(buf, EXPECT_MAGIC, 1u, &kRef);
	CHECK(writeBytes(poseTmp(), buf, POSE_BYTES));   /* power cut between remove and rename */

	PlayerPose out = { 0, 0, 0, 0, 0 };
	CHECK(playerPoseLoad(&out, POSE_DIR));
	CHECK(poseBitsEqual(&out, &kRef));
	CHECK(fileExists(poseFile()));
	CHECK(!fileExists(poseTmp()));
}

static void testAGoodRealFileBeatsALeftoverTmp(void)
{
	freshDir();

	const PlayerPose real = { 1.0f, 5.0f, 2.0f, 0.25f, 0.5f };
	CHECK(playerPoseSave(&real, POSE_DIR));

	uint8_t stale[POSE_BYTES];
	buildFile(stale, EXPECT_MAGIC, 1u, &kRef);
	CHECK(writeBytes(poseTmp(), stale, POSE_BYTES));

	PlayerPose out = { 0, 0, 0, 0, 0 };
	CHECK(playerPoseLoad(&out, POSE_DIR));
	CHECK(poseBitsEqual(&out, &real));
	CHECK(!fileExists(poseTmp()));
}

/* ── 4. the 46b/46 interaction: a restored pose must not come back buried ────────────── */

static World s_world;

static void fillSolid(int x, int y, int z)
{
	if (!worldSet(&s_world, x, y, z, BLOCK_STONE))
		printf("  (worldSet refused %d,%d,%d — the fixture is broken)\n", x, y, z);
}

static void testUnstickStepsUpOutOfLaterBlocks(void)
{
	worldInit(&s_world);

	/* The pose was written standing on the ground at y=40, at the edge of the block rather
	 * than its centre — the fractions are what the second half of this test is about. */
	PlayerPose pose = { 8.9f, 40.0f, 8.1f, 1.25f, -0.5f };

	/* Then the world changed under it. A block landed on their feet and another on their
	 * head: the exact shape a player describes as "the chunk I'm in doesn't get loaded in". */
	fillSolid(8, 40, 8);
	fillSolid(8, 41, 8);

	playerPoseUnstick(&s_world, &pose);

	/* 40 and 41 are solid, 42 and 43 are air, and a 1.8-tall body needs both — so 42. */
	CHECK(bits(pose.y) == bits(42.0f));

	/* Only y moved. */
	CHECK(bits(pose.x)     == bits(8.9f));
	CHECK(bits(pose.z)     == bits(8.1f));
	CHECK(bits(pose.yaw)   == bits(1.25f));
	CHECK(bits(pose.pitch) == bits(-0.5f));

	worldExit(&s_world);
}

/* The other half, and the one that says the un-stick is not simply "add 2 to y": on a pose
 * whose column is clear it must return the pose unchanged, or every reload of an ordinary
 * world would drift the player upward. */
static void testUnstickLeavesAClearPoseAlone(void)
{
	worldInit(&s_world);

	PlayerPose pose = { 8.9f, 40.0f, 8.1f, 1.25f, -0.5f };
	fillSolid(8, 39, 8);   /* the ground they are standing ON, not in */

	playerPoseUnstick(&s_world, &pose);

	CHECK(bits(pose.y) == bits(40.0f));
	CHECK(bits(pose.x) == bits(8.9f));
	CHECK(bits(pose.z) == bits(8.1f));

	worldExit(&s_world);
}

/* Negative coordinates, because the block a float feet position sits in is floor(v), not
 * (int)v — a truncating cast puts -0.5 in block 0 where it belongs in block -1, and the
 * un-stick would then read a different column from the one the player is in. That failure
 * is invisible on the positive side, which is where every other check here lives. */
static void testUnstickFloorsNegativeCoordinates(void)
{
	worldInit(&s_world);

	PlayerPose pose = { -0.5f, 40.0f, -0.5f, 0.0f, 0.0f };
	fillSolid(-1, 40, -1);
	fillSolid(-1, 41, -1);
	/* Block 0 deliberately left clear: a truncating cast reads (0,0) and finds air, so the
	 * un-stick would no-op and this check would be the only thing that says so. */

	playerPoseUnstick(&s_world, &pose);
	CHECK(bits(pose.y) == bits(42.0f));

	worldExit(&s_world);
}

/* ── 5. main.c really calls it, and in the right order ──────────────────────────────── */

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

static int countOf(const char* hay, const char* needle)
{
	int n = 0;
	for (const char* p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) n++;
	return n;
}

static void testMainCIsWired(void)
{
	/* Run from the repository root, the same working directory
	 * world/atlas_uv_shader_test.c and app/session_test.c read source/ out of. */
	char* src = readWholeFile("source/main.c");
	CHECK(src != NULL);
	if (src == NULL) {
		printf("  (source/main.c could not be read from this working directory)\n");
		return;
	}

	CHECK(strstr(src, "#include \"world/playerpose.h\"") != NULL);

	const char* load    = strstr(src, "playerPoseLoad(");
	/* The CALL, not the definition: `static bool genStart(int32_t cx, int32_t cz)` appears
	 * a couple of thousand lines above the world-entry sequence, so a bare "genStart("
	 * search finds the wrong one and the ordering check below would pass for free. */
	const char* gen     = strstr(src, "= genStart(");
	const char* unstick = strstr(src, "playerPoseUnstick(");
	const char* pinit   = strstr(src, "playerInit(&player,");
	const char* iinit   = strstr(src, "interactInit(");

	CHECK(load != NULL);
	CHECK(gen != NULL);
	CHECK(unstick != NULL);
	CHECK(pinit != NULL);
	CHECK(iinit != NULL);

	/* THE RESIDENCY FIX, asserted as text.
	 *
	 * worldGet() answers BLOCK_AIR for an absent chunk, so worldStandingY() over a column
	 * that is not resident returns its input and does nothing. genStart() used to be called
	 * as genStart(0, 0) — the ring centred on the spawn column — and runLoadingScreen()
	 * fills only that ring, so a pose restored a hundred columns away would have been
	 * un-stuck against air and come back buried: task 46's bug, reintroduced by task 46b.
	 *
	 * The fix is that the pose is loaded BEFORE genStart and the ring is centred on the
	 * restored column, so the loading screen has already filled it by the time the un-stick
	 * runs. Both halves are checked: the load happens first, and the literal spawn centre
	 * is gone. */
	CHECK(load != NULL && gen != NULL && load < gen);
	CHECK(strstr(src, "= genStart(0, 0)") == NULL);
	CHECK(strstr(src, "genColumnOf(saved_pose.x)") != NULL);
	CHECK(strstr(src, "genColumnOf(saved_pose.z)") != NULL);
	/* And the pose column is what genStart is actually CALLED with — not merely computed
	 * somewhere else in the file and dropped. Bounded by the statement's own semicolon so
	 * this cannot be satisfied by a genColumnOf() call further down. */
	if (gen != NULL) {
		const char* semi = strstr(gen, ";");
		const char* arg  = strstr(gen, "genColumnOf(saved_pose.x)");
		CHECK(semi != NULL);
		CHECK(arg != NULL);
		CHECK(arg != NULL && semi != NULL && arg < semi);
	}

	/* The un-stick sits between the spawn playerInit and interactInit — after the spawn so
	 * a restore is the last word, before interact so it is not competing with anything that
	 * has already read the player. */
	CHECK(pinit != NULL && unstick != NULL && pinit < unstick);
	CHECK(unstick != NULL && iinit != NULL && unstick < iinit);

	/* worldStandingY is still in main.c: the no-pose spawn is the fallback every world on
	 * the card takes today and task 46 is what put it there. */
	CHECK(strstr(src, "worldStandingY(") != NULL);

	/* Two saves — the quit path and the lid-close flush. */
	CHECK(countOf(src, "playerPoseSave(") >= 2);

	/* The quit-path save is inside the proven-idle window: after saveDirtyColumns(), whose
	 * last act is workerFlushSaves(), and before workerStop(). app/worker.h:92-97 allows one
	 * thread in the SD's FS session at a time, and this is the argument that the main thread
	 * is the only one in it here. Searched forward from saveDirtyColumns rather than from
	 * the top of the file, because the lid-close save appears earlier in the text. */
	const char* teardown = strstr(src, "saveDirtyColumns();");
	CHECK(teardown != NULL);
	if (teardown != NULL) {
		const char* save = strstr(teardown, "playerPoseSave(");
		const char* stop = strstr(teardown, "workerStop();");
		CHECK(save != NULL);
		CHECK(stop != NULL);
		CHECK(save != NULL && stop != NULL && save < stop);
	}

	/* And the lid-close save is inside sleepFlushOneColumn, which is the hook app/sleep.c
	 * drives on ONSLEEP — the only thing standing between a battery that dies with the lid
	 * shut and a lost pose. */
	const char* hook = strstr(src, "static bool sleepFlushOneColumn(void)");
	CHECK(hook != NULL);
	if (hook != NULL) {
		/* "\n}" and not "\n}\n": source/main.c has CRLF line endings, so the byte after the
		 * closing brace is \r. Measured — the three-byte form matched nowhere in the file. */
		const char* end  = strstr(hook, "\n}");
		const char* save = strstr(hook, "playerPoseSave(");
		CHECK(end != NULL);
		CHECK(save != NULL);
		CHECK(save != NULL && end != NULL && save < end);
	}

	free(src);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== playerpose test (task 46b) ==");

	testControlCleanSaveLoad();
	testRoundTripIsBitExact();

	testMissingFileIsRefused();
	testShortFileIsRefused();
	testTrailingBytesAreRefused();
	testBadMagicIsRefused();
	testBadVersionIsRefused();
	testBadCrcIsRefused();
	testNonFiniteIsRefused();
	testYRangeEnds();
	testNullAndEmptyDirs();

	testTmpIsPromotedWhenTheRealFileIsGone();
	testAGoodRealFileBeatsALeftoverTmp();

	testUnstickStepsUpOutOfLaterBlocks();
	testUnstickLeavesAClearPoseAlone();
	testUnstickFloorsNegativeCoordinates();

	testMainCIsWired();

	printf("\nplayerpose self-test: %s %d checks, %d failed\n",
	       g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
