// FIX-LOADPROF lane, 2026-09. The host harness for debug/loadprof.{c,h}'s CSV OUTPUT — not the
// stage-timing behaviour loadprof_test.c already drives (source/debug/loadprof_test.c owns
// that; this file does not duplicate it and links only loadprof.c, not the world stack).
//
// ── What this exists to prove ──────────────────────────────────────────────────────────
//
// steve's SD card carries a nine-row sdmc:/blocksmith/load.csv written by every Blocksmith
// build back to v1.7.1: no version column, no timestamp, and a header that only ever named
// eleven stage pairs plus `other_ms`. That file is live evidence in an open freeze
// investigation (see FREEZE-EVIDENCE-1817.md) and must not be silently corrupted by a build
// that starts writing a wider row.
//
// loadprofWrite() now emits `version` and `unix_time_s` ahead of the row, and renames the old
// `other_ms` column to `unaccounted_ms` (see loadprof.h for why negative is correct there).
// The mechanism that keeps this from corrupting steve's existing file is NOT inside
// loadprofWrite — it still just appends blindly to whatever path it is given, exactly as
// before. It is that LOADPROF_PATH itself now names "load2.csv" instead of "load.csv", so the
// production write path can never open the file the old rows live in. Test C below is the
// check that actually gates that guarantee; Tests A/B check the new file's shape is
// self-consistent; Test D demonstrates, deliberately, what would happen if something ever
// pointed loadprofWrite at the legacy file, which is the reason Test C has to hold.
#ifndef __3DS__

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug/loadprof.h"
#include "version.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                        \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			fprintf(stderr, "FAIL L%d %s\n", __LINE__, #cond);                  \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// Same pid-scoped directory rule as loadprof_test.c and world_test.c: two suite runs sharing a
// path is a measured cause of unrelated failures in this project, not a hypothetical one.
static const char* testDir(void)
{
	static char dir[64];
	if (dir[0] == '\0') snprintf(dir, sizeof dir, "build-host/loadprofcsv-%ld", (long)getpid());
	return dir;
}

static void testMkdir(const char* path)
{
#if defined(_WIN32)
	mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

// Reads the whole file into a NUL-terminated malloc'd buffer. Returns NULL (and sets *out_len
// to 0) if the file cannot be opened, which a CHECK below turns into a visible failure rather
// than a null-deref.
static char* readWholeFile(const char* path, size_t* out_len)
{
	*out_len = 0;
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	const long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); return NULL; }
	char* buf = (char*)malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }
	const size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[n] = '\0';
	*out_len = n;
	return buf;
}

// Number of comma-separated fields on one line, not counting a trailing '\n'. A line with no
// commas at all is 1 field; an empty line is 0.
static int countFields(const char* line, size_t len)
{
	if (len == 0) return 0;
	if (line[len - 1] == '\n') len--;
	if (len == 0) return 0;
	int fields = 1;
	for (size_t i = 0; i < len; i++)
		if (line[i] == ',') fields++;
	return fields;
}

// Splits `text` into an array of line pointers/lengths (each including its '\n' if present).
// Returns the number of lines found, capped at `cap`.
static int splitLines(char* text, size_t len, char** out_lines, size_t* out_lens, int cap)
{
	int n = 0;
	size_t start = 0;
	for (size_t i = 0; i < len && n < cap; i++) {
		if (text[i] == '\n') {
			out_lines[n] = text + start;
			out_lens[n]  = i + 1 - start;
			n++;
			start = i + 1;
		}
	}
	// A final line with no trailing newline (should not happen here -- loadprofWrite always
	// terminates a row with \n -- but a partial write on a full card is exactly the kind of
	// thing this instrument has to survive without corrupting the check).
	if (start < len && n < cap) {
		out_lines[n] = text + start;
		out_lens[n]  = len - start;
		n++;
	}
	return n;
}

// Drives one synthetic world entry through the public API -- no real world, no real generator,
// just enough to give wall_ticks and one stage a nonzero reading so the row this produces looks
// like a real one rather than an all-zero degenerate case.
static void driveOneLoad(const char* world, int reloaded)
{
	loadprofReset();
	loadprofBegin(world, reloaded);
	const uint64_t mark = loadprofMark();
	static volatile int sink;
	for (int i = 0; i < 50000; i++) sink = sink + 1;
	loadprofSince(LOAD_STAGE_GENERATE, mark);
	loadprofFrame();
	loadprofEnd();
}

// ── Test A: a fresh file's header and row agree with each other and with the new schema ──
static void testFreshFileShape(const char* path)
{
	remove(path);
	driveOneLoad("csvtest-a", 0);
	loadprofWrite(path);

	size_t len = 0;
	char* text = readWholeFile(path, &len);
	CHECK(text != NULL);
	if (!text) return;

	char*  lines[8];
	size_t lens[8];
	const int n = splitLines(text, len, lines, lens, 8);
	CHECK(n == 2);   // exactly one header line, one data row
	if (n < 2) { free(text); return; }

	// The header names the two new columns FIRST, exactly where loadprof.c writes them, and
	// carries unaccounted_ms rather than other_ms.
	CHECK(lens[0] >= 20 && strncmp(lines[0], "version,unix_time_s,", 20) == 0);
	{
		char header[512];
		size_t hl = lens[0] < sizeof(header) - 1 ? lens[0] : sizeof(header) - 1;
		memcpy(header, lines[0], hl);
		header[hl] = '\0';
		CHECK(strstr(header, "unaccounted_ms") != NULL);
		CHECK(strstr(header, "other_ms") == NULL);
	}

	const int header_fields = countFields(lines[0], lens[0]);
	const int row_fields    = countFields(lines[1], lens[1]);
	CHECK(header_fields == 29);
	CHECK(row_fields == header_fields);

	// First field of the row is the version this binary was built with -- same rule
	// app/watchdog.c's hang report already uses for the same macro.
	{
		char row[512];
		size_t rl = lens[1] < sizeof(row) - 1 ? lens[1] : sizeof(row) - 1;
		memcpy(row, lines[1], rl);
		row[rl] = '\0';
		char* comma1 = strchr(row, ',');
		CHECK(comma1 != NULL);
		if (comma1) {
			*comma1 = '\0';
			const char* expect_version = BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "(unset)";
			CHECK(strcmp(row, expect_version) == 0);

			// Second field is unix_time_s. A host always has a working clock, so this must be a
			// real, recent-looking epoch second count -- 1700000000 is 2023-11-14, comfortably
			// before this task, so any value at or above it is a real wall-clock reading rather
			// than the 0 loadprofWallSeconds reports for a dead/unset RTC.
			char* comma2 = strchr(comma1 + 1, ',');
			CHECK(comma2 != NULL);
			if (comma2) {
				*comma2 = '\0';
				const unsigned long ts = strtoul(comma1 + 1, NULL, 10);
				CHECK(ts >= 1700000000UL);
			}
		}
	}

	free(text);
}

// ── Test B: repeated writes stay one header plus one row per load, all self-consistent ──
static void testAppendContract(const char* path)
{
	driveOneLoad("csvtest-b1", 1);
	loadprofWrite(path);
	driveOneLoad("csvtest-b2", 1);
	loadprofWrite(path);

	size_t len = 0;
	char* text = readWholeFile(path, &len);
	CHECK(text != NULL);
	if (!text) return;

	char*  lines[8];
	size_t lens[8];
	const int n = splitLines(text, len, lines, lens, 8);
	// Test A already wrote one header + one row to this same path; two more driveOneLoad/Write
	// calls must add exactly two more rows and NOT a second header.
	CHECK(n == 4);
	if (n == 4) {
		const int header_fields = countFields(lines[0], lens[0]);
		for (int i = 1; i < 4; i++)
			CHECK(countFields(lines[i], lens[i]) == header_fields);
		// Only line 0 may start with "version,unix_time_s," as a literal header word -- a real
		// version string never starts with the literal word "version".
		for (int i = 1; i < 4; i++)
			CHECK(strncmp(lines[i], "version,", 8) != 0);
	}

	free(text);
}

// ── Test C: the guarantee steve's existing load.csv depends on ──────────────────────────
static void testPathIsNotLegacy(void)
{
	// This is the actual regression gate. If LOADPROF_PATH is ever pointed back at the
	// pre-version/timestamp filename while loadprofWrite still emits the wider row, this is
	// the check that catches it -- see Test D below for what that mismatch looks like.
	CHECK(strcmp(LOADPROF_PATH, "sdmc:/blocksmith/load.csv") != 0);
	CHECK(strcmp(LOADPROF_PATH, "sdmc:/blocksmith/load2.csv") == 0);
}

// ── Test D: demonstrates the hazard Test C exists to prevent ────────────────────────────
//
// Writes a file with the EXACT legacy header (transcribed from FREEZE-EVIDENCE-1817.md's
// load.csv column list) and one legacy-shaped row, standing in for steve's real card file.
// Then calls the CURRENT loadprofWrite() pointed directly at that path -- not at LOADPROF_PATH
// -- and confirms the appended row's field count no longer matches the file's own header. This
// is not a defect in loadprofWrite (it was never made to inspect an existing header; see
// loadprof.h for why) -- it is proof that the hazard described there is real, which is what
// makes Test C's guarantee load-bearing rather than decorative.
static void testLegacyFileWouldMismatch(const char* path)
{
	FILE* f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f) return;

	static const char* const kStages[] = {
		"worlddir", "played", "setup", "region_io", "decode", "generate",
		"light", "install", "queue", "mesh", "present",
	};
	fputs("world,reloaded,wall_ms,frames", f);
	for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++)
		fprintf(f, ",%s_ms,%s_n", kStages[i], kStages[i]);
	fputs(",other_ms\n", f);
	// One legacy-shaped row, matching the header field-for-field (27 fields), standing in for
	// one of steve's nine real rows -- the exact values do not matter to this check.
	fputs("hgg,0,683.000,27", f);
	for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++)
		fputs(",0.000,0", f);
	fputs(",0.000\n", f);
	fclose(f);

	size_t before_len = 0;
	char* before = readWholeFile(path, &before_len);
	CHECK(before != NULL);
	int header_fields_before = 0;
	if (before) {
		char* nl = memchr(before, '\n', before_len);
		header_fields_before = countFields(before, nl ? (size_t)(nl - before + 1) : before_len);
		CHECK(header_fields_before == 27);
		free(before);
	}

	driveOneLoad("csvtest-legacy", 0);
	loadprofWrite(path);   // deliberately the legacy-shaped path, not LOADPROF_PATH

	size_t after_len = 0;
	char* after = readWholeFile(path, &after_len);
	CHECK(after != NULL);
	if (after) {
		char*  lines[8];
		size_t lens[8];
		const int n = splitLines(after, after_len, lines, lens, 8);
		// The pre-existing legacy header, the pre-existing legacy row, and the ONE new row
		// loadprofWrite just appended underneath both of them (it never inspects an existing
		// file's header -- see loadprof.h -- so it has no way to know this file's shape changed).
		CHECK(n == 3);
		if (n == 3) {
			const int header_fields    = countFields(lines[0], lens[0]);
			const int legacy_row_fields = countFields(lines[1], lens[1]);
			const int new_row_fields    = countFields(lines[2], lens[2]);
			CHECK(header_fields == 27);              // the legacy header, untouched
			CHECK(legacy_row_fields == 27);          // the pre-existing legacy row, still fine
			CHECK(new_row_fields == 29);             // the row THIS build just appended
			CHECK(new_row_fields != header_fields);  // the mismatch Test C's guarantee prevents
		}
		free(after);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("== loadprof csv test ==");

	testMkdir("build-host");
	testMkdir(testDir());

	char pathA[128], pathD[128];
	snprintf(pathA, sizeof pathA, "%s/loadA.csv", testDir());
	snprintf(pathD, sizeof pathD, "%s/legacy.csv", testDir());

	testFreshFileShape(pathA);
	testAppendContract(pathA);
	testPathIsNotLegacy();
	testLegacyFileWouldMismatch(pathD);

	remove(pathA);
	remove(pathD);
	rmdir(testDir());

	printf("\n%s %d checks, %d failed", s_fails == 0 ? "PASS" : "FAIL", s_checks, s_fails);
	if (s_fails) printf("  first %s", s_first);
	printf("\n");
	if (s_fails) printf("FAILED - %d of %d checks\n", s_fails, s_checks);
	return s_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
