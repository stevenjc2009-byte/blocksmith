// Guards the one number a .pica file cannot get from C: the vertex shader's UV scale
// constant in source/shaders/world.v.pica must equal 1/ATLAS_PX, with ATLAS_PX defined in
// world/atlas_uv.h. world/block_tiles_check.c's trick for the same kind of problem
// (duplicate a value on both sides, then _Static_assert the two copies agree) is not
// available here: the picasso shader compiler has no #include and no _Static_assert, so
// there is no way to make world.v.pica read ATLAS_PX at all. This test parses the shader
// source text instead, at host-test time, and checks the literal against the one C
// definition directly.
//
// The bug this guards against already happened once. Step 9.3c shrank the atlas sheet from
// 256x256 to 64x64 and updated ATLAS_PX, but the shader's uvScale constant was left at
// 1/256. A wrong scale still produces valid UVs, so nothing crashed or errored - it just
// rendered every tile as the same ~14x14px corner of the sheet (the wood tile), so the whole
// world drew flat brown with no grass green and no stone grey anywhere. See the comment
// block at source/shaders/world.v.pica lines 20-27 for the full story.
//
// A parse that finds nothing must not read as "nothing wrong": a test that silently passed
// whenever it failed to even locate the constant would stay green forever regardless of what
// the shader actually says, which is worse than having no test. So every failure path below
// (file missing, line missing, malformed number) is itself a loud failure, never a skip.
//
// Self-contained (its own main()), same shape and same reason as world/worldlist_test.c:
// this has nothing to do with the world data tools/run_host_tests.sh already links into
// build-host/world_test, and folding it in would mean one broken parse here could stop the
// whole world suite from running.
//
// The __3DS__ guard around the whole file is load-bearing, not tidy - copied from the same
// guard in world/worldlist_test.c and source/app/options_test.c: the Makefile globs every
// .c under source/world into the console build, so without it this file's main() would link
// against source/main.c's and the build would die with "multiple definition of `main'".
#ifndef __3DS__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/atlas_uv.h"

static int  s_checks;
static int  s_fails;
static char s_first[256];

#define SHADER_PATH "source/shaders/world.v.pica"
#define ATLAS_HEADER_PATH "source/world/atlas_uv.h"
#define NEEDLE ".constf consts("

// Reads `path` looking for the ".constf consts(a, b, c, d)" line and extracts component c -
// the one aliased to uvScale two lines below it in world.v.pica. Returns true and fills
// *outScale only when every step succeeds (file opens, the line is found, it has exactly
// four comma-separated components, and the third parses cleanly as a number with nothing
// left over). Otherwise fills errbuf with which step failed and returns false, so the caller
// can turn that into a real failure instead of silently passing.
static bool readShaderUvScale(const char* path, double* outScale, char* errbuf, size_t errbufsz)
{
	FILE* f = fopen(path, "r");
	if (!f) {
		snprintf(errbuf, errbufsz, "cannot open %s", path);
		return false;
	}

	char line[512];
	char found[512];
	found[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, NEEDLE)) {
			snprintf(found, sizeof(found), "%s", line);
			break;
		}
	}
	fclose(f);

	if (!found[0]) {
		snprintf(errbuf, errbufsz, "no '%s' line found in %s", NEEDLE, path);
		return false;
	}

	const char* open_paren = strstr(found, NEEDLE) + strlen(NEEDLE);
	const char* close_paren = strchr(open_paren, ')');
	if (!close_paren) {
		snprintf(errbuf, errbufsz, "malformed consts(...) line in %s: %.150s", path, found);
		return false;
	}

	char args[256];
	const size_t len = (size_t)(close_paren - open_paren);
	if (len >= sizeof(args)) {
		snprintf(errbuf, errbufsz, "consts(...) line too long in %s", path);
		return false;
	}
	memcpy(args, open_paren, len);
	args[len] = '\0';

	// Split on commas; index 2 (the third field) is uvScale.
	char* fields[4] = {0};
	int nfields = 0;
	char* tok = strtok(args, ",");
	while (tok && nfields < 4) {
		fields[nfields++] = tok;
		tok = strtok(NULL, ",");
	}

	if (nfields != 4) {
		snprintf(errbuf, errbufsz, "expected 4 components in consts(...), found %d in %s", nfields, path);
		return false;
	}

	char* end = NULL;
	const double v = strtod(fields[2], &end);
	// end must have advanced past at least one character, and nothing but whitespace may
	// follow: a clean "0.015625" parses fine, but a corrupted "0.01xyz" would leave "xyz"
	// trailing and must not be accepted as a number.
	if (end == fields[2]) {
		snprintf(errbuf, errbufsz, "third component of consts(...) is not a number in %s: '%s'", path, fields[2]);
		return false;
	}
	while (*end == ' ' || *end == '\t') end++;
	if (*end != '\0') {
		snprintf(errbuf, errbufsz, "trailing garbage after third component in %s: '%s'", path, fields[2]);
		return false;
	}

	*outScale = v;
	return true;
}

int main(void)
{
	double scale = -1.0;
	char err[256] = {0};

	const bool ok = readShaderUvScale(SHADER_PATH, &scale, err, sizeof(err));
	s_checks++;
	if (!ok) {
		s_fails++;
		snprintf(s_first, sizeof(s_first), "%s", err);
	} else {
		const double expected = 1.0 / (double)ATLAS_PX;
		const double diff = scale - expected;
		const double adiff = diff < 0.0 ? -diff : diff;
		// 0.015625 is written to 8 decimal digits in the shader source (world.v.pica line
		// 28), so the tolerance has to be looser than that literal's own rounding, while
		// still being far tighter than the gap to the historical bug value: 1/256 =
		// 0.00390625 is ~0.0117 away from 1/64, over 100000x this tolerance.
		const double tolerance = 1e-7;
		if (adiff >= tolerance) {
			s_fails++;
			snprintf(s_first, sizeof(s_first),
				"%s uvScale=%.8f but %s ATLAS_PX=%d means 1/ATLAS_PX=%.8f (diff %.8f >= tolerance %.8f)",
				SHADER_PATH, scale, ATLAS_HEADER_PATH, ATLAS_PX, expected, adiff, tolerance);
		}
	}

	if (s_fails == 0)
		printf("atlas uv shader self-test: PASS  %d checks\n", s_checks);
	else
		printf("atlas uv shader self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int atlas_uv_shader_test_host_only_t;

#endif   // !__3DS__
