// Guards the atlas layout: the numbers a .pica file cannot get from C, the geometry
// atlasRect() produces, and the sheet the generator actually wrote.
//
// THE SHADER HALF. Both source/shaders/world.v.pica and source/shaders/world_dynamic.v.pica
// carry a `uvScale` constant that must equal (1/ATLAS_W_PX, TILE_PX/ATLAS_H_PX), with all
// three of those defined in world/atlas_uv.h. world/block_tiles_check.c's trick for the same
// kind of problem (duplicate a value on both sides, then _Static_assert the two copies agree)
// is not available here: the picasso shader compiler has no #include and no _Static_assert, so
// there is no way to make a .pica file read ATLAS_W_PX at all. This test parses the shader
// source text instead, at host-test time, and checks the literals against the one C definition.
//
// The .y component is NOT 1/ATLAS_H_PX, and that asymmetry is the whole of task 13b. u is still
// an atlas PIXEL column, so it is divided by the sheet width; v is a SLOT-EDGE INDEX, so it is
// multiplied by TILE_PX/ATLAS_H_PX — the tile height as a fraction of the sheet. The shader
// carries the TILE_PX factor that used to live in the vertex byte.
//
// The bug this guards against already happened once. Step 9.3c shrank the atlas sheet from
// 256x256 to 64x64 and updated the header, but the shader's uvScale constant was left at
// 1/256. A wrong scale still produces valid UVs, so nothing crashed or errored - it just
// rendered every tile as the same ~14x14px corner of the sheet (the wood tile), so the whole
// world drew flat brown with no grass green and no stone grey anywhere.
//
// THE WIRING HALF (new in v1.8.3), and the reason everything above it was not enough. Each
// check described so far is about what uvScale DECLARES. None of them is about whether any
// instruction READS it, and picasso does not care either way — a .constf nobody consumes is
// uploaded and ignored, silently. So the exact defect this file was written to prevent was
// still reachable, just through the other door.
//
// MEASURED, on this tree: `mul r2, uvScale, inpack` in source/shaders/world.v.pica changed to
// `mul r2, consts, inpack` (consts is (0, 1, 0, 1/3), so every u is multiplied by zero and
// every v by one), with the uvScale declaration left perfectly correct. Every UV on screen is
// garbage. This suite printed "atlas uv shader self-test: PASS 4280 checks" — byte-identical
// to a healthy tree, with not one check moved.
//
// What replaced token presence: ORDERED needles naming whole instructions, operand included.
// The multiply must exist, must be the only write to r2, and the write that outputs r2 must
// come after it. Both files, the BOUND one first. That arm now goes red 2/4293.
//
// And which file IS bound is asserted rather than narrated — see
// checkBoundShaderIsTheDynamicOne(). The prose in this header saying "a New 3DS binds
// world_dynamic.v.pica instead" went stale at v1.8.0 task 24, when scene/chunk_render.c
// stopped binding world.v.pica for ANY model, and nothing noticed because nothing checked.
// world.v.pica is compiled, checked here, and never on screen.
//
// Two things widened this in v1.6.0, both because the sheet became a one-tile-wide strip and
// stopped being square:
//
//   * uvScale is two numbers now, not one broadcast scalar, so BOTH components are checked.
//     Getting u right and v wrong would slide every tile vertically into its neighbour -
//     again with no error, again as bad art.
//   * world_dynamic.v.pica is checked too. It was never checked before, and it carries its
//     own copy of the same constant. A New 3DS binds THAT program instead of world.v.pica
//     (scene/chunk_render.c asks APT_CheckNew3DS once at init), so a drift there would be
//     invisible on every Old 3DS the game happened to be tested on.
//
// THE GEOMETRY HALF (new in v1.6.0). atlasRect() is now the only place the strip layout is
// written down, which is what lets the mesher stay unmodified across the change - so it is
// where the layout has to be proven. Every addressable tile must return a rect covering one
// 16x16 block of texels that lies wholly inside the sheet and shares no TEXEL with any other
// tile; the rect is half-open (slot t covers slot rows vslot0..vslot1-1, i.e. exactly one
// slot), so adjacent slots legitimately share the edge index vslot1==vslot0 while sharing no
// pixel, and a test written on coordinate overlap instead of texel overlap would fail on a
// correct sheet. The U-repeat property greedy meshing depends on is proven arithmetically here
// as well, since no host test can sample a texture.
//
// THE UNITS, which task 13b (v1.8.2) changed and this file has to state exactly once:
//
//   AtlasRect.u0, u1          atlas PIXEL columns (0 and 16; widened to TILE_PX*width by a
//                             greedy merge, which is why they stayed in pixels)
//   AtlasRect.vslot0, vslot1  SLOT-EDGE INDICES (tile and tile+1). NOT pixels. Slot t's texels
//                             are texture rows t*TILE_PX .. (t+1)*TILE_PX-1, and this file has
//                             to do that multiply itself before it can talk about texels.
//
// Until v1.8.1 the v fields were named v0/v1 and held raw pixel offsets, which is what capped
// the sheet at 16 slots (slot 15's top edge needed v=256 and MeshVertex.v is a uint8_t, so 15
// slots were addressable at ANY sheet height). Task 13b changed the field's UNITS, not its
// width: the byte now holds 0..64 instead of 0..1024, the sheet grew to 16x1024 with all 64
// slots addressable, and MeshVertex is still 8 bytes. Every "v" in this file means a slot edge
// unless it says PIXEL.
//
// THE SHEET HALF (new in v1.6.0). world/atlas_uv.h's own comment used to say the duplication
// with tools/make_atlas.py was unguarded because "the PNG carries no dimensions the C side
// reads". It does - a PNG's IHDR chunk is at a fixed offset - so this test reads gfx/atlas.png
// directly and checks its width and height against ATLAS_W_PX/ATLAS_H_PX. That closes the
// last unguarded copy of the layout.
//
// THE PIXEL HALF (new in v1.6.0 F7). Everything above is geometry, and geometry cannot tell
// you whether a slot has any ART in it. Several slots' worth of that was exactly the F7 defect:
// tex bytes addressed real, addressable slots that tools/make_atlas.py never painted, so they
// drew the sheet's background fill as an opaque near-black solid, and an out-of-range tex
// clamped to slot 0 and drew grass. Neither is an error at any level a geometry test can see -
// both are a block with a texture on it. So this file DECODES build/atlas.t3x, the actual
// object the GPU is handed (LZ11, then the PICA200's 8x8-tile Morton swizzle), and asserts on
// texel values: that every slot the TILES list does not fill is the magenta/black missing-
// texture marker, that no slot anywhere is still a solid block of background fill, and that
// each painted slot still fingerprints to what it did before F7 touched the
// generator. This is as close to looking at the sheet as a host test can get. Since task 13b
// that sweep covers 64 slots rather than 16.
//
// A parse that finds nothing must not read as "nothing wrong": a test that silently passed
// whenever it failed to even locate the constant would stay green forever regardless of what
// the shader actually says, which is worse than having no test. So every failure path below
// (file missing, line missing, malformed number) is itself a loud failure, never a skip.
//
// Self-contained (its own main()), same shape and same reason as world/worldlist_test.c:
// this has nothing to do with the world data tools/run_host_tests.sh already links into
// build-host/world_test, and folding it in would mean one broken parse here could stop the
// whole world suite from running. It links the REAL world/block.c and world/registry.c,
// because the block-to-tile half of the coverage check is a claim about what blockFaceTex()
// actually returns, and a hand-copied tile table would only be checking itself.
//
// The __3DS__ guard around the whole file is load-bearing, not tidy - copied from the same
// guard in world/worldlist_test.c and source/app/options_test.c: the Makefile globs every
// .c under source/world into the console build, so without it this file's main() would link
// against source/main.c's and the build would die with "multiple definition of `main'".
#ifndef __3DS__

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/atlas_uv.h"
#include "world/block.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[512];

// Reports every failure, not just the first - a red run has to be readable case by case, so
// that a case which stayed green under sabotage can be spotted as neutralised.
#define CHECK(cond, ...) do {                                            \
		s_checks++;                                                       \
		if (!(cond)) {                                                    \
			s_fails++;                                                    \
			char _m[400];                                                 \
			snprintf(_m, sizeof(_m), __VA_ARGS__);                        \
			printf("  FAIL L%d  %s\n", __LINE__, _m);                     \
			if (!s_first[0]) snprintf(s_first, sizeof(s_first), "L%d %s", __LINE__, _m); \
		}                                                                 \
	} while (0)

#define WORLD_SHADER_PATH   "source/shaders/world.v.pica"
#define DYNAMIC_SHADER_PATH "source/shaders/world_dynamic.v.pica"
#define ATLAS_HEADER_PATH   "source/world/atlas_uv.h"
#define ATLAS_PNG_PATH      "gfx/atlas.png"
#define ATLAS_T3X_PATH      "build/atlas.t3x"
#define RENDERER_PATH       "source/scene/chunk_render.c"
#define NEEDLE ".constf uvScale("

// THE WIRING NEEDLES (v1.8.3). Everything the NEEDLE above leads to is about what uvScale
// DECLARES. None of it is about whether any instruction reads it, and the two are completely
// independent: a `.constf` with nobody consuming it is not an error in picasso, it is a
// register that gets uploaded and ignored.
//
// This is not hypothetical and it is not a new class of bug — it is verbatim the step 9.3c
// defect this whole file was written to prevent, reached by the other door. MEASURED: with
// source/shaders/world.v.pica's `mul r2, uvScale, inpack` changed to `mul r2, consts, inpack`
// (consts is (0, 1, 0, 1/3), so every u is multiplied by zero and every v by one), the uvScale
// declaration left correct and untouched, this suite printed "atlas uv shader self-test: PASS
// 4280 checks" — byte-identical to a healthy tree, with not one check moved, while every UV on
// screen was garbage.
//
// So the needles below name the whole instruction, operand included, and are checked in ORDER:
// the multiply must exist and must be the only write to r2, and the output write that consumes
// r2 must come after it. Token presence is what failed; an ordered pair of whole instructions
// is what replaces it.
#define UVSCALE_MUL_NEEDLE "mul r2, uvScale, inpack"
#define UVSCALE_OUT_NEEDLE "mov outtc0, r2.xyxy"
#define ANY_R2_MUL_NEEDLE  "mul r2,"

// The number of slots tools/make_atlas.py's TILES list actually paints art into.
//
// This used to be described as a copy of gfx/atlas.h's TILE_* enum count. From v1.8.3 phase 3
// it is NOT that any more, and the difference is deliberate: the sheet carries 17 painted
// slots while the enum still names 12. The five extra - snow, ice, cactus, dead bush and fern
// in slots 12..16 - are ART THAT LANDED AHEAD OF THE BLOCK IDS that will use them, which is
// the safe order to land the two halves in. An atlas slot no tex byte addresses is never
// sampled and costs nothing; the reverse order is the failure this whole file exists for - a
// registered block whose tile is still an unpainted slot draws the magenta missing-texture
// marker on a real block face.
//
// So this number belongs to the GENERATOR, and the two checks it feeds still say exactly what
// they said before: everything below it is real art with a pinned fingerprint, and everything
// at or above it must be the missing-texture marker texel for texel. Neither claim ever needed
// the enum. What the enum's count governs is TILE_USED_COUNT in gfx/atlas_tiles.h, which
// world/block_tiles_check.c asserts against the BTEX_* mirror - a separate guard over a
// separate pair of lists, untouched by this and still reading 12 on both sides.
#define ATLAS_PAINTED_SLOTS 17

// Slots 10 and 11 within that: water and tall grass (roadmap tasks 17 and 19). Named here
// because the two texel-content checks further down are about what these two tiles ARE, not
// merely that they are stable - one must have cutout texels and the other must not.
#define ATLAS_SLOT_WATER      10
#define ATLAS_SLOT_TALL_GRASS 11

// The shader literals are written to 8 decimal digits, so the tolerance has to be looser than
// that literal's own rounding while staying far tighter than the gap to any wrong value. The
// near-miss to guard against is the pre-13b form of the SAME constant: uvScale.y used to be
// 1/ATLAS_H_PX = 0.00097656 and is now TILE_PX/ATLAS_H_PX = 0.01562500, a difference of
// 0.01464844 - over 140000x this tolerance. That wrong value is not a typo anybody would
// invent; it is what the file said one commit ago, which is exactly why it has to be excluded
// by a margin rather than by hoping nobody reverts it.
#define UV_TOLERANCE 1e-7

static double absd(double v) { return v < 0.0 ? -v : v; }

// ── Reading a source file as lines ───────────────────────────────────────────
//
// Lifted verbatim from world/water_alpha_test.c rather than reinvented, so the two suites
// that parse these same two .pica files agree on what a "line" is: whitespace collapsed to
// single spaces and the ends trimmed, because both shaders column-align their operands and a
// needle matched against raw text would be asserting the indentation.
//
// loadLines() is one fgets() per line and joins NO continuations. That is a real limit and it
// has bitten this project once already (water_alpha_test.c's BLEND_ON_TAIL); it does not bite
// here, because a .pica instruction is always exactly one line.
//
// COMMENTS (v1.9.0). Every needle below is a fragment of picasso assembly, and picasso's
// comment character is ';' — so a line of PROSE that names an instruction is indistinguishable
// from the instruction itself under a plain strstr(). That is not hypothetical: the fog work
// added a comment to both .pica files explaining why the fog coordinate could NOT ride on the
// spare z/w lanes of outtc0, and naming `mov outtc0, r2.xyxy` to say which instruction it was
// talking about. This file promptly reported 4 failures — two per shader — claiming the
// texcoord output was written twice and that the multiply came after the output, the second
// only because the "output" it had found was a comment 50 lines above the real one.
//
// Both shaders were correct. The guard was wrong, and wrong in a way that punishes documenting
// an instruction by name, which this project does constantly. So a .pica line is now truncated
// at its first ';' BEFORE matching, and every needle is asserted against code only.
//
// The flag is a parameter rather than always-on because loadLines() is also pointed at
// scene/chunk_render.c, where ';' ends a statement and cutting at it would silently discard
// most of every line. Callers say which language they are reading.
#define MAX_LINES 4096
#define MAX_LINE  512

#define PICA_COMMENTS true   // ';' starts a comment (picasso assembly)
#define C_COMMENTS    false  // ';' ends a statement (C)

static char s_lines[MAX_LINES][MAX_LINE];
static int  s_nlines;

static void squash(const char* in, char* out, size_t cap)
{
	size_t o = 0;
	bool   sp = false;
	for (const char* p = in; *p && o + 1 < cap; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { sp = (o > 0); continue; }
		if (sp) { out[o++] = ' '; sp = false; }
		if (o + 1 < cap) out[o++] = *p;
	}
	out[o] = '\0';
}

// Returns the number of lines read, or -1 if the file could not be opened. The caller turns
// -1 into a failure; it is never treated as "no lines, therefore nothing to complain about" —
// the same rule the header of this file states for every other parse in it.
//
// `strip_semicolon` truncates each line at its first ';' — pass PICA_COMMENTS for a .pica file
// and C_COMMENTS for a .c one. See the comment block above for what it is protecting against.
// Note the line NUMBERS are unaffected: a comment-only line becomes empty and matches nothing,
// but it still occupies its slot, so an L%d in a failure message still points at the real line
// of the real file.
static int loadLines(const char* path, bool strip_semicolon)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	s_nlines = 0;
	char raw[4096];
	while (s_nlines < MAX_LINES && fgets(raw, sizeof raw, f)) {
		if (strip_semicolon) {
			char* c = strchr(raw, ';');
			if (c) *c = '\0';
		}
		squash(raw, s_lines[s_nlines++], MAX_LINE);
	}

	fclose(f);
	return s_nlines;
}

// How many loaded lines contain `needle`, and the line number (1-based) of the first.
static int countLines(const char* needle, int* first_line)
{
	int n = 0;
	if (first_line) *first_line = -1;
	for (int i = 0; i < s_nlines; i++) {
		if (!strstr(s_lines[i], needle)) continue;
		if (first_line && *first_line < 0) *first_line = i + 1;
		n++;
	}
	return n;
}

// Reads `path` looking for the ".constf uvScale(a, b, c, d)" line and extracts components a
// and b - the U divisor and the V scale. Returns true and fills *outU/*outV only when every
// step succeeds (file opens, the line is found, it has exactly four comma-separated components,
// and the first two parse cleanly as numbers with nothing left over). Otherwise fills errbuf
// with which step failed and returns false, so the caller can turn that into a real failure
// instead of silently passing.
//
// COMMENTS (v1.9.0). This used to run its own fopen/fgets loop and match NEEDLE against the raw
// text, which made it blind in exactly the way checkShaderUsesUvScale() was — a line of PROSE
// naming `.constf uvScale(` would be picked up as the declaration itself, and then parsed. The
// two failure modes are worse here than they were there, because this function does not merely
// count: a comment that happens to contain a parenthesis parses into NUMBERS, so the wrong
// values could be checked against the header and reported as a wrong constant in a file whose
// constant is right. Left alone it was a trap waiting on the day someone documents uvScale by
// name, so it now shares loadLines(path, PICA_COMMENTS) with every other parse in this file and
// there is one definition of "a line" for the whole suite.
static bool readShaderUvScale(const char* path, double* outU, double* outV,
                              char* errbuf, size_t errbufsz)
{
	const int nlines = loadLines(path, PICA_COMMENTS);
	if (nlines < 0) {
		snprintf(errbuf, errbufsz, "cannot open %s", path);
		return false;
	}

	// A pointer INTO s_lines rather than a copy of it. Copying needed a bound gcc could not
	// see through the flattened [MAX_LINES][MAX_LINE] index, and -Werror=format-truncation
	// rejected it; nothing reloads s_lines between here and the parse below, so borrowing the
	// line is both correct and one less buffer to size.
	const char* found = NULL;
	for (int i = 0; i < nlines; i++) {
		if (!strstr(s_lines[i], NEEDLE)) continue;
		found = s_lines[i];
		break;
	}

	if (!found) {
		snprintf(errbuf, errbufsz,
		         "no '%s' line found in %s. Note comments are stripped before matching, so a "
		         "commented-out declaration reads as no declaration — which is what it is",
		         NEEDLE, path);
		return false;
	}

	const char* open_paren = strstr(found, NEEDLE) + strlen(NEEDLE);
	const char* close_paren = strchr(open_paren, ')');
	if (!close_paren) {
		snprintf(errbuf, errbufsz, "malformed uvScale(...) line in %s: %.150s", path, found);
		return false;
	}

	char args[256];
	const size_t len = (size_t)(close_paren - open_paren);
	if (len >= sizeof(args)) {
		snprintf(errbuf, errbufsz, "uvScale(...) line too long in %s", path);
		return false;
	}
	memcpy(args, open_paren, len);
	args[len] = '\0';

	char* fields[4] = {0};
	int nfields = 0;
	char* tok = strtok(args, ",");
	while (tok && nfields < 4) {
		fields[nfields++] = tok;
		tok = strtok(NULL, ",");
	}

	if (nfields != 4) {
		snprintf(errbuf, errbufsz, "expected 4 components in uvScale(...), found %d in %s",
		         nfields, path);
		return false;
	}

	double parsed[2];
	for (int i = 0; i < 2; i++) {
		char* end = NULL;
		const double v = strtod(fields[i], &end);
		// end must have advanced past at least one character, and nothing but whitespace may
		// follow: a clean "0.0625" parses fine, but a corrupted "0.06xyz" would leave "xyz"
		// trailing and must not be accepted as a number.
		if (end == fields[i]) {
			snprintf(errbuf, errbufsz, "component %d of uvScale(...) is not a number in %s: '%s'",
			         i, path, fields[i]);
			return false;
		}
		while (*end == ' ' || *end == '\t') end++;
		if (*end != '\0') {
			snprintf(errbuf, errbufsz, "trailing garbage after component %d in %s: '%s'",
			         i, path, fields[i]);
			return false;
		}
		parsed[i] = v;
	}

	*outU = parsed[0];
	*outV = parsed[1];
	return true;
}

// Both components of one shader's uvScale, against the header. Also hands the values back so
// the two shaders can be compared with each other.
//
// The two components are NOT the same formula, and that is the point of task 13b:
//   .x = 1/ATLAS_W_PX        because u is an atlas PIXEL column
//   .y = TILE_PX/ATLAS_H_PX  because v is a SLOT-EDGE INDEX, so the shader supplies the
//                            TILE_PX factor the vertex byte no longer carries
static void checkShader(const char* path, double* outU, double* outV)
{
	double u = -1.0, v = -1.0;
	char err[256] = {0};

	const bool ok = readShaderUvScale(path, &u, &v, err, sizeof(err));
	CHECK(ok, "%s", err);
	if (!ok) { *outU = *outV = -1.0; return; }

	const double want_u = 1.0 / (double)ATLAS_W_PX;
	const double want_v = (double)TILE_PX / (double)ATLAS_H_PX;

	CHECK(absd(u - want_u) < UV_TOLERANCE,
	      "%s uvScale.x=%.8f but %s ATLAS_W_PX=%d means 1/ATLAS_W_PX=%.8f (diff %.8f)",
	      path, u, ATLAS_HEADER_PATH, ATLAS_W_PX, want_u, absd(u - want_u));
	CHECK(absd(v - want_v) < UV_TOLERANCE,
	      "%s uvScale.y=%.8f but %s TILE_PX=%d / ATLAS_H_PX=%d means TILE_PX/ATLAS_H_PX=%.8f "
	      "(diff %.8f). v is a SLOT-EDGE INDEX since task 13b, so the shader multiplies by a "
	      "whole tile's height; the pre-13b 1/ATLAS_H_PX=%.8f would draw every face as the "
	      "bottom 1/16th of its own tile, stretched",
	      path, v, ATLAS_HEADER_PATH, TILE_PX, ATLAS_H_PX, want_v, absd(v - want_v),
	      1.0 / (double)ATLAS_H_PX);

	*outU = u;
	*outV = v;
}

// Is uvScale actually CONSUMED, and does what it produces reach the texture coordinate output?
//
// checkShader() above proves the constant holds the right two numbers. This proves an
// instruction reads it and that the result is written out. They are separate failures with the
// same symptom — a world drawn with wrong UVs and no error anywhere — and until v1.8.3 only
// the first was covered. See the UVSCALE_MUL_NEEDLE comment for the measurement.
//
// `bound` is printed, not branched on: both files are checked identically. It exists because
// world.v.pica has been compiled-but-never-bound since v1.8.0 task 24 and world_dynamic.v.pica
// is what both console models run, so a red run has to say which of the two names is the one
// that is on screen. checkBoundShaderIsTheDynamicOne() below is what keeps that label honest.
static void checkShaderUsesUvScale(const char* path, bool bound)
{
	const char* role = bound ? "BOUND on both console models" : "compiled, never bound";

	const int n = loadLines(path, PICA_COMMENTS);
	CHECK(n >= 0, "cannot open %s to check that uvScale is used (%s)", path, role);
	if (n < 0) return;

	int mul_line = -1, out_line = -1;
	const int any_muls = countLines(ANY_R2_MUL_NEEDLE, NULL);
	const int scaled   = countLines(UVSCALE_MUL_NEEDLE, &mul_line);
	const int outs     = countLines(UVSCALE_OUT_NEEDLE, &out_line);

	// Exactly one write to r2, so the pair below is the whole story of how a texture
	// coordinate is computed in this file and there is no second multiply to argue about.
	CHECK(any_muls == 1,
	      "%s (%s) has %d `%s` instructions, not 1 — the uvScale wiring check below only "
	      "describes one of them", path, role, any_muls, ANY_R2_MUL_NEEDLE);

	// The consumption itself. This is the check that goes red when uvScale is left declared,
	// correct, and unread.
	CHECK(scaled == 1,
	      "%s (%s) does not multiply the packed attribute BY uvScale: expected exactly one "
	      "`%s`, found %d. The declaration being right proves nothing on its own — a .constf "
	      "nothing reads is uploaded and ignored, and every UV on screen comes out of whatever "
	      "operand replaced it", path, role, UVSCALE_MUL_NEEDLE, scaled);

	// ...and that the scaled result leaves the shader. A correct multiply into a register
	// nothing outputs is the same picture as no multiply at all.
	CHECK(outs == 1,
	      "%s (%s) does not write the scaled coordinate to the texcoord output: expected "
	      "exactly one `%s`, found %d", path, role, UVSCALE_OUT_NEEDLE, outs);

	// ORDER, which is what makes the two needles a wiring claim rather than two presences.
	// r2 must be written before it is read; the other way round outputs the previous vertex's
	// leftovers, which draws a whole world one vertex out of step and errors nowhere.
	CHECK(mul_line > 0 && out_line > 0 && mul_line < out_line,
	      "%s (%s) scales into r2 at L%d but outputs r2 at L%d — the multiply must come FIRST",
	      path, role, mul_line, out_line);
}

// ── The v1.9.0 fog wiring, in BOTH shaders ──────────────────────────────────────────────────
//
// The same duplication problem uvScale has, for the same reason. world.v.pica is compiled and
// never bound (see checkBoundShaderIsTheDynamicOne below), and it is kept precisely so this
// file can hold the two copies to each other — so an edit that lands in one and not the other
// is caught by a test rather than by a console. The fog coordinate is now a second thing
// duplicated across them, so it is checked the same way.
//
// The wiring, and what each needle is actually protecting:
//
//   .fvec fogParams[1]   the uniform chunk_render.c uploads (inv_range, bias). The whole fade
//                        is those two numbers; without the declaration nothing uploads.
//   .out outtc1 texcoord1  the varying. NOT outtc0's spare z/w lanes — texcoord0 is a
//                        two-component semantic and those lanes are discarded, and not vertex
//                        colour either, because greedy merging requires flat AO so no merged
//                        quad in this game has ever interpolated colour across a long run.
//   dp4 r7.w, projection[3], r1   LINEAR eye depth, kept in a register instead of being thrown
//                        straight at outpos.w. Row 3 of Mtx_PerspTilt is (0,0,-1,0) — measured
//                        with objdump on the shipped libcitro3d.a, quoted in both .pica files —
//                        so this dp4 is -z_eye, i.e. positive distance in blocks. It must be
//                        eye-space Z and not a Euclidean length: mesher.c merges only coplanar
//                        cells, Z is linear across a plane and interpolates exactly, and a
//                        per-vertex sqrt() would flatten the fade toward the middle of every
//                        big merged run.
//   mov outpos.w, r7.wwww   the clip w that dp4 used to write directly. If this is ever lost
//                        the world does not fog wrongly, it does not draw at all.
//   mul / add / mov      u = depth * inv_range + bias, then out. The ORDER of these is the
//                        wiring claim; presence alone would pass with them shuffled.
//
// Comment stripping matters here more than anywhere: the .pica files explain this design in
// prose that names these very instructions. See the loadLines() comment block.
#define FOG_UNIFORM_NEEDLE ".fvec fogParams[1]"
#define FOG_OUTDECL_NEEDLE ".out outtc1 texcoord1"
#define FOG_DEPTH_NEEDLE   "dp4 r7.w, projection[3], r1"
#define FOG_CLIPW_NEEDLE   "mov outpos.w, r7.wwww"
#define FOG_MUL_NEEDLE     "mul r7.x, fogParams.xxxx, r7.wwww"
#define FOG_ADD_NEEDLE     "add r7.x, fogParams.yyyy, r7.xxxx"
#define FOG_OUT_NEEDLE     "mov outtc1, r7.xxxx"

// The old hardware path, which must be GONE from the shaders' point of view. Nothing in a
// .pica ever mentioned the fog LUT — this is here for the OTHER half of the double-fog risk:
// `dp4 outpos.w, projection[3], r1` is the instruction the depth capture replaced, and if it
// comes back alongside the new one the shader writes clip w twice.
#define FOG_OLD_CLIPW_NEEDLE "dp4 outpos.w, projection[3], r1"

static void checkShaderFogWiring(const char* path, bool bound)
{
	const char* role = bound ? "BOUND on both console models" : "compiled, never bound";

	const int n = loadLines(path, PICA_COMMENTS);
	CHECK(n >= 0, "cannot open %s to check the fog wiring (%s)", path, role);
	if (n < 0) return;

	int depth_line = -1, mul_line = -1, add_line = -1, out_line = -1;

	CHECK(countLines(FOG_UNIFORM_NEEDLE, NULL) == 1,
	      "%s (%s) does not declare `%s`. scene/chunk_render.c looks that uniform up by name "
	      "with shaderInstanceGetUniformLocation and uploads (inv_range, bias) into it every "
	      "frame; with no declaration the lookup returns -1 and the entire fade is silently "
	      "whatever register 0 happens to hold", path, role, FOG_UNIFORM_NEEDLE);

	CHECK(countLines(FOG_OUTDECL_NEEDLE, NULL) == 1,
	      "%s (%s) does not declare `%s`. The fog factor is a VARYING — it has to be "
	      "interpolated per fragment for the fade to be smooth across a face, and TEV stage 1 "
	      "reads it as texture unit 1's coordinate", path, role, FOG_OUTDECL_NEEDLE);

	CHECK(countLines(FOG_DEPTH_NEEDLE, &depth_line) == 1,
	      "%s (%s) does not compute linear eye depth with `%s`. This is the only source of the "
	      "fog coordinate; there is no vertex attribute carrying distance and there cannot be "
	      "one — world/mesh_vertex.h pins MeshVertex at 8 bytes with a _Static_assert and its "
	      "one spare-looking byte carries the smooth-light nibbles",
	      path, role, FOG_DEPTH_NEEDLE);

	CHECK(countLines(FOG_CLIPW_NEEDLE, NULL) == 1,
	      "%s (%s) has no `%s`, so clip w is never written. Capturing the depth into r7.w is "
	      "only half the edit — the perspective divide still needs that value in outpos.w and "
	      "without it nothing rasterises at all", path, role, FOG_CLIPW_NEEDLE);

	CHECK(countLines(FOG_OLD_CLIPW_NEEDLE, NULL) == 0,
	      "%s (%s) still contains `%s`. That is the instruction the depth capture REPLACED; "
	      "having both writes clip w twice", path, role, FOG_OLD_CLIPW_NEEDLE);

	CHECK(countLines(FOG_MUL_NEEDLE, &mul_line) == 1,
	      "%s (%s) does not scale the depth by fogParams.x: expected exactly one `%s`. Without "
	      "it the ramp is sampled with raw block distances, so every fragment past 1 block "
	      "clamps to fully fogged", path, role, FOG_MUL_NEEDLE);

	CHECK(countLines(FOG_ADD_NEEDLE, &add_line) == 1,
	      "%s (%s) does not add the fogParams.y bias: expected exactly one `%s`. The bias is "
	      "what holds the near end of the fade at `start` blocks; without it the fog begins at "
	      "the camera", path, role, FOG_ADD_NEEDLE);

	CHECK(countLines(FOG_OUT_NEEDLE, &out_line) == 1,
	      "%s (%s) does not write the fog coordinate out: expected exactly one `%s`. A correct "
	      "computation left in a register nothing outputs looks exactly like no fog",
	      path, role, FOG_OUT_NEEDLE);

	// ORDER. Four instructions each of which is individually correct still produce nothing if
	// they run in the wrong sequence, and a PICA200 register holds the PREVIOUS vertex's value,
	// so a mis-ordered chain draws a world one vertex out of step and errors nowhere. Same
	// reasoning as the uvScale order check above.
	CHECK(depth_line > 0 && mul_line > 0 && depth_line < mul_line,
	      "%s (%s) captures the depth at L%d but scales it at L%d — the depth must come FIRST, "
	      "or the multiply reads whatever r7.w held from the previous vertex",
	      path, role, depth_line, mul_line);
	CHECK(mul_line > 0 && add_line > 0 && mul_line < add_line,
	      "%s (%s) scales at L%d but biases at L%d — the multiply must come FIRST",
	      path, role, mul_line, add_line);
	CHECK(add_line > 0 && out_line > 0 && add_line < out_line,
	      "%s (%s) biases at L%d but outputs at L%d — the bias must come FIRST",
	      path, role, add_line, out_line);
}

// WHICH of the two programs the renderer actually binds.
//
// This file has said "a New 3DS binds world_dynamic.v.pica instead" in prose since v1.6.0 and
// that prose went stale at v1.8.0 task 24, when scene/chunk_render.c stopped binding
// world.v.pica for ANY model. Nothing noticed, because nothing checked. The label is worth
// having only if it cannot silently invert, so the fact is asserted here instead of narrated:
// chunkRenderInit parses world_dynamic_shbin and parses world_shbin nowhere.
//
// If this pair ever goes red, the `bound` arguments in main() are what has to change — and the
// same argument in world/water_alpha_test.c's checkOneShader calls with them.
static void checkBoundShaderIsTheDynamicOne(void)
{
	const int n = loadLines(RENDERER_PATH, C_COMMENTS);
	CHECK(n >= 0, "cannot open %s to find out which shader program is bound", RENDERER_PATH);
	if (n < 0) return;

	const int dyn  = countLines("DVLB_ParseFile((u32*)world_dynamic_shbin", NULL);
	const int baked = countLines("DVLB_ParseFile((u32*)world_shbin", NULL);

	CHECK(dyn == 1,
	      "%s parses world_dynamic_shbin %d times, not once — world_dynamic.v.pica is the file "
	      "every check in this suite labels BOUND", RENDERER_PATH, dyn);
	CHECK(baked == 0,
	      "%s parses world_shbin %d times. Since v1.8.0 task 24 it parses it never, and every "
	      "'compiled, never bound' label in this file and in world/water_alpha_test.c rests on "
	      "that. If world.v.pica is being bound again, both files' labels are now backwards",
	      RENDERER_PATH, baked);
}

// gfx/atlas.png's IHDR: 8-byte signature, then a 4-byte length and the 4-byte type "IHDR",
// then width and height as big-endian u32 at offsets 16 and 20. Fixed by the PNG spec, so no
// decoder is needed to learn what tools/make_atlas.py actually wrote.
static bool readPngSize(const char* path, unsigned* w, unsigned* h, char* errbuf, size_t errbufsz)
{
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(errbuf, errbufsz, "cannot open %s", path);
		return false;
	}
	unsigned char hdr[24];
	const size_t got = fread(hdr, 1, sizeof(hdr), f);
	fclose(f);
	if (got != sizeof(hdr)) {
		snprintf(errbuf, errbufsz, "%s is only %zu bytes, too short for a PNG header", path, got);
		return false;
	}
	static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(hdr, sig, 8) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) {
		snprintf(errbuf, errbufsz, "%s is not a PNG with an IHDR first chunk", path);
		return false;
	}
	*w = ((unsigned)hdr[16] << 24) | ((unsigned)hdr[17] << 16) | ((unsigned)hdr[18] << 8) | hdr[19];
	*h = ((unsigned)hdr[20] << 24) | ((unsigned)hdr[21] << 16) | ((unsigned)hdr[22] << 8) | hdr[23];
	return true;
}

static bool isPowerOfTwo(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }

// ── build/atlas.t3x, decoded down to texels ─────────────────────────────────────────────
//
// Three layers, all of them fixed formats rather than guesses:
//
//   1. The t3x header tex3ds writes: u16 subtexture count, one packed byte holding
//      log2(width)-3 in bits 0..2 and log2(height)-3 in bits 3..5, then the GPU format byte
//      and the mipmap level count. Each subtexture that follows is u16 width, u16 height and
//      four u16 texture coordinates in 1/1024ths.
//   2. The pixel data, which gfx/atlas.t3s asks tex3ds to compress ("-z auto"). What that has
//      been choosing is LZ11, and the four-byte compression header says so: low byte 0x11,
//      high three bytes the decompressed size. Anything else is a loud failure here rather
//      than a wrong decode - see lz11Inflate().
//   3. The PICA200's texture swizzle: 8x8 tiles in raster order, and inside a tile the texels
//      are Morton (Z-order) interleaved. Undoing it gives back rows in the PNG's own top-down
//      order, which is the order slot_png_y() in tools/make_atlas.py writes and the opposite
//      of texture-space v. Verified against gfx/atlas.png with an independent decoder: every
//      texel matched with no flip, and every texel mismatched with one, so the orientation
//      below is measured rather than assumed.
//
// s_texel is indexed [png_row][x], row 0 being the TOP of the sheet. It sizes itself from
// ATLAS_H_PX, so it grew from 8 KB to 32 KB with the sheet in task 13b with no edit here -
// static, so that is .bss and not 32 KB of stack.
static uint16_t s_texel[ATLAS_H_PX][ATLAS_W_PX];

// RGBA5551 exactly as the PICA200 packs it: red in the top five bits, then green, then blue,
// with the single alpha bit last.
#define RGBA5551(r, g, b, a) \
	((uint16_t)((((r) >> 3) << 11) | (((g) >> 3) << 6) | (((b) >> 3) << 1) | ((a) >= 128 ? 1 : 0)))

#define TEXEL_MAGENTA  RGBA5551(255, 0, 255, 255)   // 0xF83F
#define TEXEL_BLACK    RGBA5551(0, 0, 0, 255)       // 0x0001
// The colour tools/make_atlas.py fills the sheet with before painting anything. Nothing may be
// left showing it: an opaque near-black solid on a block face reads as a dark block, which is
// the entire F7 defect.
#define TEXEL_OLD_FILL RGBA5551(24, 20, 28, 255)    // 0x1887

// PNG row of the TOP of slot `slot`, i.e. where slot_png_y() in tools/make_atlas.py puts it.
// Texture v grows upwards and PNG rows run downwards, so slot 0 is the LAST TILE_PX rows of
// the image and the top slot is the first. Written once, here, because every texel loop below
// needs it and the flip is the thing most worth not re-deriving by hand each time.
#define SLOT_PNG_TOP(slot) (ATLAS_H_PX - ((slot) + 1) * TILE_PX)

typedef struct {
	bool     ok;
	unsigned w, h, format, mipmaps, subtex_count, sub_w, sub_h;
} T3xInfo;

static bool lz11Inflate(const unsigned char* in, size_t in_len, size_t in_off,
                        unsigned char* out, size_t out_cap, size_t* out_len,
                        char* err, size_t errsz)
{
	if (in_off + 4 > in_len) {
		snprintf(err, errsz, "%s ends before its compression header", ATLAS_T3X_PATH);
		return false;
	}
	const uint32_t hdr = (uint32_t)in[in_off] | ((uint32_t)in[in_off + 1] << 8)
	                   | ((uint32_t)in[in_off + 2] << 16) | ((uint32_t)in[in_off + 3] << 24);
	in_off += 4;

	const unsigned type = hdr & 0xFFu;
	const size_t   size = (size_t)(hdr >> 8);
	if (type != 0x11u) {
		snprintf(err, errsz,
		         "%s is compressed with type 0x%02x; this decoder implements only LZ11 (0x11), "
		         "which is what gfx/atlas.t3s's '-z auto' has been choosing. Decoding it as the "
		         "wrong format would give wrong texels, not an error, so this refuses instead",
		         ATLAS_T3X_PATH, type);
		return false;
	}
	if (size > out_cap) {
		snprintf(err, errsz, "%s decompresses to %zu bytes, past this decoder's %zu byte buffer",
		         ATLAS_T3X_PATH, size, out_cap);
		return false;
	}

	size_t o = 0;
	while (o < size) {
		if (in_off >= in_len) {
			snprintf(err, errsz, "%s is truncated: %zu of %zu bytes decoded", ATLAS_T3X_PATH, o, size);
			return false;
		}
		const unsigned flags = in[in_off++];
		for (int bit = 0; bit < 8 && o < size; bit++) {
			if (!(flags & (0x80u >> bit))) {
				if (in_off >= in_len) {
					snprintf(err, errsz, "%s is truncated mid-literal at output byte %zu", ATLAS_T3X_PATH, o);
					return false;
				}
				out[o++] = in[in_off++];
				continue;
			}

			if (in_off + 1 >= in_len) {
				snprintf(err, errsz, "%s is truncated mid-reference at output byte %zu", ATLAS_T3X_PATH, o);
				return false;
			}
			const unsigned b1  = in[in_off];
			const unsigned ind = b1 >> 4;
			size_t len, disp;
			if (ind == 0) {
				if (in_off + 2 >= in_len) {
					snprintf(err, errsz, "%s is truncated mid-reference at output byte %zu", ATLAS_T3X_PATH, o);
					return false;
				}
				len  = (size_t)((((b1 & 0xFu) << 4) | (in[in_off + 1] >> 4)) + 0x11);
				disp = (size_t)((((in[in_off + 1] & 0xFu) << 8) | in[in_off + 2]) + 1);
				in_off += 3;
			} else if (ind == 1) {
				if (in_off + 3 >= in_len) {
					snprintf(err, errsz, "%s is truncated mid-reference at output byte %zu", ATLAS_T3X_PATH, o);
					return false;
				}
				len  = (size_t)((((b1 & 0xFu) << 12) | ((unsigned)in[in_off + 1] << 4)
				                | (in[in_off + 2] >> 4)) + 0x111);
				disp = (size_t)((((in[in_off + 2] & 0xFu) << 8) | in[in_off + 3]) + 1);
				in_off += 4;
			} else {
				len  = (size_t)(ind + 1);
				disp = (size_t)((((b1 & 0xFu) << 8) | in[in_off + 1]) + 1);
				in_off += 2;
			}

			if (disp > o) {
				snprintf(err, errsz, "%s has a back-reference %zu bytes before the start of the "
				         "output at byte %zu - the stream is corrupt", ATLAS_T3X_PATH, disp - o, o);
				return false;
			}
			for (size_t i = 0; i < len && o < size; i++, o++)
				out[o] = out[o - disp];
		}
	}

	*out_len = size;
	return true;
}

// The PICA200's in-tile Morton interleave: x and y bits alternate, x first.
static unsigned picaMorton(unsigned x, unsigned y)
{
	return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2)
	     | ((x & 4u) << 2) | ((y & 4u) << 3);
}

static T3xInfo readT3x(char* err, size_t errsz)
{
	T3xInfo t;
	memset(&t, 0, sizeof(t));

	static unsigned char raw[256 * 1024];
	// Sized from the macros, never from a literal, so a sheet resize cannot leave this reader
	// silently short: at 16x1024 RGBA5551 this is 32 KB, four times what it was before 13b.
	static unsigned char pixels[ATLAS_W_PX * ATLAS_H_PX * 2];

	FILE* f = fopen(ATLAS_T3X_PATH, "rb");
	if (!f) {
		snprintf(err, errsz,
		         "cannot open %s - a build product (tex3ds, from gfx/atlas.t3s), not in git. Run "
		         "\"make\" from devkitPro MSYS2 and re-run. Skipping would leave the atlas's ART "
		         "unchecked, which is where the F7 defect lived",
		         ATLAS_T3X_PATH);
		return t;
	}
	const size_t len = fread(raw, 1, sizeof(raw), f);
	const bool   eof = feof(f) != 0;
	fclose(f);
	if (!eof) {
		snprintf(err, errsz, "%s is larger than this reader's %zu byte buffer", ATLAS_T3X_PATH, sizeof(raw));
		return t;
	}
	if (len < 17) {
		snprintf(err, errsz, "%s is only %zu bytes, too short for a t3x header", ATLAS_T3X_PATH, len);
		return t;
	}

	t.subtex_count = (unsigned)raw[0] | ((unsigned)raw[1] << 8);
	const unsigned packed = raw[2];
	t.w       = 8u << (packed & 7u);
	t.h       = 8u << ((packed >> 3) & 7u);
	t.format  = raw[3];
	t.mipmaps = raw[4];
	t.sub_w   = (unsigned)raw[5] | ((unsigned)raw[6] << 8);
	t.sub_h   = (unsigned)raw[7] | ((unsigned)raw[8] << 8);

	if (t.w != ATLAS_W_PX || t.h != ATLAS_H_PX) {
		// Bail before decoding: the untile loop below indexes s_texel by this size, so a
		// disagreement has to stop here rather than run off the end of the array.
		snprintf(err, errsz, "%s says the texture is %ux%u, but ATLAS_W_PX/ATLAS_H_PX say %dx%d "
		         "- the sheet and the code disagree about the layout. A t3x built before the "
		         "sheet grew is STALE, not merely different: rebuild it from gfx/atlas.png",
		         ATLAS_T3X_PATH, t.w, t.h, ATLAS_W_PX, ATLAS_H_PX);
		return t;
	}

	// 5 header bytes, then one 12-byte subtexture record, then the compressed pixels.
	size_t got = 0;
	if (!lz11Inflate(raw, len, 5 + 12 * (size_t)t.subtex_count, pixels, sizeof(pixels), &got, err, errsz))
		return t;
	if (got != sizeof(pixels)) {
		snprintf(err, errsz, "%s decompressed to %zu bytes, expected %zu for a %dx%d RGBA5551 sheet",
		         ATLAS_T3X_PATH, got, sizeof(pixels), ATLAS_W_PX, ATLAS_H_PX);
		return t;
	}

	const unsigned tiles_across = (unsigned)ATLAS_W_PX / 8u;
	for (unsigned y = 0; y < (unsigned)ATLAS_H_PX; y++) {
		for (unsigned x = 0; x < (unsigned)ATLAS_W_PX; x++) {
			const unsigned tile_index = (y / 8u) * tiles_across + (x / 8u);
			const unsigned texel      = tile_index * 64u + picaMorton(x & 7u, y & 7u);
			s_texel[y][x] = (uint16_t)((unsigned)pixels[texel * 2u] | ((unsigned)pixels[texel * 2u + 1] << 8));
		}
	}

	t.ok = true;
	return t;
}

// A regression fingerprint over one slot's decoded texels - FNV-1a 64 over the little-endian
// RGBA5551 words, in PNG row order. Not a security hash and not pretending to be one: its only
// job is to say "these 256 texels are the ones that were there before", and the pinned values
// below were computed from the t3x built BEFORE F7 changed tools/make_atlas.py.
static uint64_t slotFingerprint(int png_top)
{
	uint64_t h = 0xcbf29ce484222325ull;            // FNV-1a 64 offset basis
	for (int y = 0; y < TILE_PX; y++) {
		for (int x = 0; x < ATLAS_W_PX; x++) {
			const uint16_t v = s_texel[png_top + y][x];
			h ^= (uint64_t)(v & 0xFFu);          h *= 0x100000001b3ull;
			h ^= (uint64_t)((v >> 8) & 0xFFu);   h *= 0x100000001b3ull;
		}
	}
	return h;
}

// The missing-texture marker's texel at (x, y) INSIDE a tile, y counted downwards from the
// tile's top PNG row - the orientation tools/make_atlas.py paints in.
//
// Magenta and black in half-tile quadrants. Deliberately NOT the 2px checker TILE_SENTINEL
// uses: that one is a bleed alarm and is meant to be sampled by accident, so sharing its
// appearance would make one magenta face mean either "the UVs bled" or "this texture does not
// exist". At 16x16 on a 240px screen a 2px checker blurs to flat pink while four half-tile
// quadrants stay four blocks, so the two are told apart on sight.
//
// Note this is not symmetric under a vertical flip - mirroring a tile swaps magenta for black
// - so checking against it also catches slot_png_y()'s v flip being reversed.
static uint16_t markerTexel(int x, int y)
{
	const int half = TILE_PX / 2;
	return ((x / half + y / half) % 2 == 0) ? TEXEL_MAGENTA : TEXEL_BLACK;
}

int main(void)
{
	// ── The shader constants ────────────────────────────────────────────────────────────
	double wu = -1.0, wv = -1.0, du = -1.0, dv = -1.0;
	checkShader(WORLD_SHADER_PATH, &wu, &wv);
	checkShader(DYNAMIC_SHADER_PATH, &du, &dv);

	// ── The shader WIRING (v1.8.3) ──────────────────────────────────────────────────────
	// The constant being right and the constant being used are two different claims. The
	// BOUND file goes first, so the head of a red run names the program that is on screen.
	checkBoundShaderIsTheDynamicOne();
	checkShaderUsesUvScale(DYNAMIC_SHADER_PATH, true);
	checkShaderUsesUvScale(WORLD_SHADER_PATH, false);

	// ── The fog wiring (v1.9.0) ─────────────────────────────────────────────────────────
	// Second thing duplicated across the two .pica files, checked for the same reason as
	// uvScale: world.v.pica exists to be the copy that goes stale, and this is what notices.
	// The fade itself — its shape, and the half_vis it produces — is tests/fogramp_test.c;
	// this pair only asserts that the shader computes a fog coordinate at all and in the
	// right order.
	checkShaderFogWiring(DYNAMIC_SHADER_PATH, true);
	checkShaderFogWiring(WORLD_SHADER_PATH, false);

	// H2. The two shaders against each other, not only against the header. An Old 3DS binds
	// one and a New 3DS the other, so they have to agree or the same world looks different on
	// the two consoles - and that is a difference nobody would attribute to a texture constant.
	// Still worth its own pair of checks after task 13b: uvScale.y changed in BOTH files, and
	// updating one and not the other is precisely the edit this catches.
	CHECK(absd(wu - du) < UV_TOLERANCE, "the two shaders disagree on uvScale.x: %.8f vs %.8f", wu, du);
	CHECK(absd(wv - dv) < UV_TOLERANCE, "the two shaders disagree on uvScale.y: %.8f vs %.8f", wv, dv);

	// ── The sheet the generator wrote ───────────────────────────────────────────────────
	unsigned png_w = 0, png_h = 0;
	char png_err[256] = {0};
	const bool png_ok = readPngSize(ATLAS_PNG_PATH, &png_w, &png_h, png_err, sizeof(png_err));
	CHECK(png_ok, "%s", png_err);
	if (png_ok) {
		CHECK(png_w == (unsigned)ATLAS_W_PX, "%s is %u px wide but ATLAS_W_PX is %d",
		      ATLAS_PNG_PATH, png_w, ATLAS_W_PX);
		CHECK(png_h == (unsigned)ATLAS_H_PX, "%s is %u px tall but ATLAS_H_PX is %d",
		      ATLAS_PNG_PATH, png_h, ATLAS_H_PX);
	}

	// ── Sheet invariants ────────────────────────────────────────────────────────────────
	CHECK(isPowerOfTwo((unsigned)ATLAS_W_PX), "ATLAS_W_PX=%d is not a power of two; the PICA200 requires one", ATLAS_W_PX);
	CHECK(isPowerOfTwo((unsigned)ATLAS_H_PX), "ATLAS_H_PX=%d is not a power of two; the PICA200 requires one", ATLAS_H_PX);
	// The premise of the whole strip: one tile wide, so GPU_REPEAT's U period is one tile.
	CHECK(ATLAS_W_PX == TILE_PX, "the sheet is %d px wide, not one %d px tile - GPU_REPEAT would wrap over %d tiles and greedy meshing in U is impossible",
	      ATLAS_W_PX, TILE_PX, ATLAS_W_PX / TILE_PX);
	CHECK(ATLAS_TILE_SLOTS * TILE_PX == ATLAS_H_PX, "ATLAS_TILE_SLOTS=%d x %d != ATLAS_H_PX=%d",
	      ATLAS_TILE_SLOTS, TILE_PX, ATLAS_H_PX);
	// The PICA200's own limit on a texture dimension, which is what the slot count is now
	// bounded BY rather than by the vertex byte. citro3d's C3D_TexInitWithParams refuses
	// anything outside 8..1024 (see world/atlas_uv.h), so a sheet taller than this would not
	// fail to look right, it would fail to load at all.
	CHECK(ATLAS_H_PX <= 1024, "ATLAS_H_PX=%d is past the PICA200's 1024 px maximum texture "
	      "dimension; C3D_TexInitWithParams would refuse the atlas outright", ATLAS_H_PX);

	// H5. Every slot on the sheet is addressable. This check used to assert the OPPOSITE - it
	// read `ATLAS_TILE_COUNT * TILE_PX <= 255` together with `(ATLAS_TILE_COUNT+1) * TILE_PX
	// > 255`, i.e. "the last slot's top edge fits in the vertex byte AS PIXELS and the next
	// one's does not", which pinned ATLAS_TILE_COUNT at SLOTS-1 and left the top slot
	// permanently unreachable. Task 13b changed vslot0/vslot1 from pixels to slot-edge indices,
	// so the pixel arithmetic those two checks did no longer describes anything the code does,
	// and the unaddressable slot they justified no longer exists. Asserting the new fact rather
	// than deleting them, because SLOTS-1 is exactly what a careless revert would restore.
	CHECK(ATLAS_TILE_COUNT == ATLAS_TILE_SLOTS,
	      "ATLAS_TILE_COUNT=%d but the sheet holds ATLAS_TILE_SLOTS=%d. Since task 13b every "
	      "slot is addressable; a count below the slot count means %d slots of painted sheet "
	      "that no tex byte can reach",
	      ATLAS_TILE_COUNT, ATLAS_TILE_SLOTS, ATLAS_TILE_SLOTS - ATLAS_TILE_COUNT);

	// H6. The vertex byte still bounds the sheet, just far higher than it did. MeshVertex.v is
	// a uint8_t holding a slot EDGE - `tile + 1` for a rect's top - so the real requirement is
	// ATLAS_TILE_SLOTS + 1 <= 256, i.e. ATLAS_TILE_SLOTS <= 255.
	//
	// mesh_vertex.h's _Static_assert(sizeof(MeshVertex) == 8) is BLIND to this. Task 13b
	// changed the field's UNITS and not its width, so the struct is still exactly 8 bytes and
	// that assert stays green through any sheet height at all - which is precisely why this
	// check has to exist here, in the file that knows what the byte is counting.
	CHECK(ATLAS_TILE_SLOTS <= 255,
	      "the sheet holds %d slots, so a rect's top edge index reaches %d - past what "
	      "MeshVertex.v's uint8_t can hold. sizeof(MeshVertex)==8 cannot see this: a units "
	      "change keeps the struct 8 bytes",
	      ATLAS_TILE_SLOTS, ATLAS_TILE_SLOTS);

	// ── Every tile's rect, and the no-shared-texel property ─────────────────────────────
	//
	// H3/H4. Two separate claims, and they are separate on purpose:
	//
	//   H3 is about the SLOT UNITS: vslot0 is the tile index itself and vslot1 is tile+1. That
	//      is the whole of what task 13b changed, and the checks it replaced (v1-v0 == TILE_PX,
	//      v0 == t*TILE_PX) now assert the opposite of the truth - a rect whose vslot1-vslot0
	//      were 16 would be sixteen slots tall.
	//   H4 is about TEXELS, which needs the slot units converted back to pixels first. Slot t
	//      owns texture rows t*TILE_PX .. (t+1)*TILE_PX-1; texture v runs upwards and PNG rows
	//      run downwards, so those are PNG rows ATLAS_H_PX-(t+1)*TILE_PX .. ATLAS_H_PX-t*TILE_PX-1.
	//      A per-texel ownership map, not a rect-overlap comparison: the rect is half-open in
	//      slots, so adjacent slots share the edge index vslot1==vslot0 while sharing no pixel,
	//      and a coordinate test would go red on a perfectly correct sheet.
	//
	// 16 KB of .bss, not stack - it is static deliberately, and it grew with ATLAS_H_PX.
	static unsigned char owner[ATLAS_H_PX][ATLAS_W_PX];   // 0 = unclaimed, tile+1 otherwise
	memset(owner, 0, sizeof(owner));
	int claimed = 0;

	for (int t = 0; t < ATLAS_TILE_COUNT; t++) {
		const AtlasRect r = atlasRect(t);

		CHECK(r.u1 - r.u0 == TILE_PX, "tile %d spans %d px in u, not %d", t, r.u1 - r.u0, TILE_PX);
		CHECK(r.u1 <= ATLAS_W_PX, "tile %d's u1=%d runs past the %d px sheet width", t, r.u1, ATLAS_W_PX);

		// H3, the slot-unit claims.
		CHECK((int)r.vslot0 == t,
		      "tile %d's vslot0=%d, expected %d. vslot0 is the SLOT INDEX itself since task 13b, "
		      "not t*TILE_PX - if this reads like a pixel offset the units have been reverted",
		      t, r.vslot0, t);
		CHECK((int)r.vslot1 == t + 1,
		      "tile %d's vslot1=%d, expected %d (the slot edge ABOVE it)", t, r.vslot1, t + 1);
		CHECK((int)r.vslot1 - (int)r.vslot0 == 1,
		      "tile %d spans %d slots, not 1 - a rect covers exactly one slot",
		      t, (int)r.vslot1 - (int)r.vslot0);
		CHECK((int)r.vslot1 <= ATLAS_TILE_SLOTS,
		      "tile %d's top edge index vslot1=%d runs past the sheet's %d slots",
		      t, r.vslot1, ATLAS_TILE_SLOTS);

		// H4, the texel claims. The multiply back to pixels happens HERE and nowhere else in
		// this loop, so a wrong conversion shows up as overlap or as unclaimed rows rather than
		// as a comment nobody reads.
		const int png_top = ATLAS_H_PX - (int)r.vslot1 * TILE_PX;
		const int ok_rows = (png_top >= 0 && png_top + TILE_PX <= ATLAS_H_PX);
		CHECK(ok_rows, "slot %d maps to PNG rows %d..%d, outside the %d row image - the slot "
		      "-> pixel conversion is wrong", t, png_top, png_top + TILE_PX - 1, ATLAS_H_PX);
		if (!ok_rows) continue;   // indexing owner[] with it would run off the array

		CHECK(png_top == ATLAS_H_PX - (t + 1) * TILE_PX,
		      "slot %d's PNG top row is %d, expected %d - the v flip tools/make_atlas.py's "
		      "slot_png_y() implements is reversed, which puts every tile on the wrong slot "
		      "with no error at all",
		      t, png_top, ATLAS_H_PX - (t + 1) * TILE_PX);

		for (int y = png_top; y < png_top + TILE_PX; y++) {
			for (int x = r.u0; x < r.u1; x++) {
				if (owner[y][x] != 0) {
					CHECK(false, "texel (%d,%d) is claimed by both tile %d and tile %d",
					      x, y, owner[y][x] - 1, t);
				} else {
					owner[y][x] = (unsigned char)(t + 1);
					claimed++;
				}
			}
		}
	}
	CHECK(claimed == ATLAS_TILE_COUNT * TILE_PX * TILE_PX,
	      "%d texels claimed, expected %d - some tile overlapped another",
	      claimed, ATLAS_TILE_COUNT * TILE_PX * TILE_PX);
	// And the slots together must claim the WHOLE sheet, which is the same number said the
	// other way round. Stated separately because the two can only agree when the sheet's area
	// and the slot count's area are the same thing.
	CHECK(ATLAS_TILE_COUNT * TILE_PX * TILE_PX == ATLAS_W_PX * ATLAS_H_PX,
	      "%d addressable slots of %dx%d cover %d texels, but the sheet is %d - the slots do "
	      "not tile the sheet", ATLAS_TILE_COUNT, TILE_PX, TILE_PX,
	      ATLAS_TILE_COUNT * TILE_PX * TILE_PX, ATLAS_W_PX * ATLAS_H_PX);
	// This block INVERTED in task 13b. It used to assert that the rows above the last
	// addressable slot were UNCLAIMED, because the top slot could not be reached by a uint8_t
	// pixel offset and had to stay empty. There is no unaddressable slot any more, so the same
	// sweep now asserts the opposite: not one texel anywhere on the sheet is left unowned. The
	// old form would pass trivially today (there are no rows past the last slot to sweep), so
	// leaving it renamed rather than rewritten would have been a check that cannot go red.
	{
		int stray = 0, fx = -1, fy = -1;
		for (int y = 0; y < ATLAS_H_PX; y++) {
			for (int x = 0; x < ATLAS_W_PX; x++) {
				if (owner[y][x] == 0) {
					if (stray == 0) { fx = x; fy = y; }
					stray++;
				}
			}
		}
		CHECK(stray == 0,
		      "%d texels of the %d px sheet are owned by no slot, first at PNG (%d,%d). Every "
		      "one of the %d slots is addressable now, so an unowned texel is sheet the game "
		      "paid for and cannot draw",
		      stray, ATLAS_W_PX * ATLAS_H_PX, fx, fy, ATLAS_TILE_COUNT);
	}

	// ── The U-repeat property greedy meshing depends on ─────────────────────────────────
	//
	// No host test can sample a texture, so this is arithmetic on the same thing the GPU
	// does: with GPU_REPEAT and a sheet TILE_PX wide, texture coordinate u maps to texel
	// u mod TILE_PX. A merged quad `k+1` blocks wide emits u1 = u0 + TILE_PX*k, and the
	// property that makes the merge legal is that every one of those lands back on the
	// same column of the same tile. u is the one field task 13b left in PIXELS, deliberately -
	// see ATLAS_MAX_MERGE_BLOCKS in world/atlas_uv.h - so this half is unchanged.
	for (int t = 0; t < ATLAS_TILE_COUNT; t++) {
		const AtlasRect r = atlasRect(t);
		for (int k = 1; k <= ATLAS_MAX_MERGE_BLOCKS; k++) {
			const int u_extended = r.u0 + TILE_PX * k;
			CHECK(u_extended % ATLAS_W_PX == r.u0 % ATLAS_W_PX,
			      "tile %d extended by %d blocks gives u=%d, which repeats to column %d, not %d",
			      t, k, u_extended, u_extended % ATLAS_W_PX, r.u0 % ATLAS_W_PX);
			CHECK(u_extended <= 255,
			      "tile %d extended by %d blocks gives u=%d, which does not fit in MeshVertex's uint8_t u",
			      t, k, u_extended);
		}
	}
	// The ceiling itself, stated as a check rather than only as a comment: 15 blocks fit,
	// 16 do not. If TILE_PX or the vertex format ever changes, this is what says so.
	CHECK(TILE_PX * ATLAS_MAX_MERGE_BLOCKS <= 255,
	      "a %d-block merge emits u1=%d, past uint8_t", ATLAS_MAX_MERGE_BLOCKS, TILE_PX * ATLAS_MAX_MERGE_BLOCKS);
	CHECK(TILE_PX * (ATLAS_MAX_MERGE_BLOCKS + 1) > 255,
	      "a %d-block merge emits u1=%d, which still fits - ATLAS_MAX_MERGE_BLOCKS=%d is too low",
	      ATLAS_MAX_MERGE_BLOCKS + 1, TILE_PX * (ATLAS_MAX_MERGE_BLOCKS + 1), ATLAS_MAX_MERGE_BLOCKS);

	// ── Out-of-range tiles land somewhere defined ───────────────────────────────────────
	//
	// H7. world/mesher.c builds its rect table for all 256 registry ids, and a dynamic block
	// registered over the wire carries an arbitrary tile byte, so this is reachable in
	// production, not a theoretical edge.
	//
	// It must land on the MISSING-TEXTURE MARKER, not on tile 0 (v1.6.0 F7). Clamping to 0
	// drew grass: a block declared with a wrong tex byte came out as a perfectly plausible
	// grass block, and the player - and the server author - had nothing to see. Checked as
	// "identical to atlasRect(ATLAS_TILE_MISSING)", not merely as "in range".
	//
	// THE ALIASING HAZARD MOVED IN TASK 13b, and the old reasoning here has to be discarded
	// rather than re-read. It used to be about a PIXEL wrap: vslot0 was tile*TILE_PX, so index
	// 16 gave 256 which wrapped to exactly 0, and a clamp-to-0 was indistinguishable from no
	// clamp at all for that one index. There is no *TILE_PX left in atlasRect(), so no index
	// below 256 wraps any more. What replaces it:
	//
	//   * 64..255 are now all perfectly VALID uint8_t values that are out of range for the
	//     sheet. Nothing about the byte rejects them; only the explicit clamp does. That is a
	//     far wider hole than the single index 16 used to be - 192 values instead of one.
	//   * 256 and up are what a raw `(uint8_t)tile` would alias, and 256 aliases to 0, i.e.
	//     to GRASS. So 256 is in the list below specifically: it is the one value for which a
	//     dropped clamp still produces a plausible-looking block rather than anything odd.
	//
	// ATLAS_TILE_SLOTS is NOT in the list any more. It equals ATLAS_TILE_COUNT since task 13b,
	// so it would be a duplicate entry proving nothing separate.
	{
		const AtlasRect zero    = atlasRect(0);
		const AtlasRect missing = atlasRect(ATLAS_TILE_MISSING);

		CHECK(ATLAS_TILE_MISSING > 0 && ATLAS_TILE_MISSING < ATLAS_TILE_COUNT,
		      "ATLAS_TILE_MISSING=%d is not an addressable slot (0..%d), so out-of-range ids "
		      "would clamp to something the sheet does not contain",
		      ATLAS_TILE_MISSING, ATLAS_TILE_COUNT - 1);
		CHECK(ATLAS_TILE_MISSING == ATLAS_TILE_COUNT - 1,
		      "ATLAS_TILE_MISSING=%d is not the TOP addressable slot (%d). It is reserved there "
		      "on purpose: that is the last slot a growing TILES list would reach, so reserving "
		      "any lower one would collide with the next block added",
		      ATLAS_TILE_MISSING, ATLAS_TILE_COUNT - 1);
		CHECK((int)missing.vslot0 == ATLAS_TILE_MISSING,
		      "atlasRect(ATLAS_TILE_MISSING) gave vslot0=%d, expected the slot index %d itself",
		      missing.vslot0, ATLAS_TILE_MISSING);
		// If the marker rect were tile 0's rect, every check below would pass for the OLD
		// clamp as well and none of them would be cover at all.
		CHECK(missing.vslot0 != zero.vslot0,
		      "the marker slot's rect is tile 0's rect, so no check here can tell the F7 clamp "
		      "from the one it replaced");

		// -1                    negative, which atlasRect() sees as a huge unsigned
		// ATLAS_TILE_COUNT      the first index past the sheet
		// ATLAS_TILE_COUNT + 1  and the one after it
		// 100, 200, 255         valid bytes, invalid slots - the 192-value hole described above
		// 256                   the one value a raw (uint8_t) cast aliases onto slot 0, i.e. grass
		// 1000                  well past anything a byte could hold
		const int bad[] = { -1, ATLAS_TILE_COUNT, ATLAS_TILE_COUNT + 1, 100, 200, 255, 256, 1000 };
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			const AtlasRect r = atlasRect(bad[i]);
			CHECK(r.u0 == missing.u0 && r.u1 == missing.u1
			      && r.vslot0 == missing.vslot0 && r.vslot1 == missing.vslot1,
			      "atlasRect(%d) gave u %d..%d vslot %d..%d; an unaddressable tile must clamp to "
			      "the missing-texture marker at slot %d (u %d..%d vslot %d..%d) - clamping to "
			      "tile 0's u %d..%d vslot %d..%d draws GRASS for a wrong tex byte, and a raw "
			      "uint8_t cast draws whatever slot the value happens to name",
			      bad[i], r.u0, r.u1, r.vslot0, r.vslot1, ATLAS_TILE_MISSING,
			      missing.u0, missing.u1, missing.vslot0, missing.vslot1,
			      zero.u0, zero.u1, zero.vslot0, zero.vslot1);
		}
	}

	// ── Every real block, every face ────────────────────────────────────────────────────
	//
	// H8. The REAL block.c and registry.c are linked, so this is a claim about what
	// blockFaceTex() returns rather than about a table copied into this file. Every id the
	// mesher can ask about - all 256 raw byte values, exactly as world/mesher.c's planBuild
	// does - must map into 0..ATLAS_TILE_COUNT-1, which is 0..63 since task 13b, or the atlas
	// silently renders that block as the missing-texture marker.
	registryInitCore();
	for (int id = 0; id < 256; id++) {
		for (int face = 0; face < BLOCK_FACES; face++) {
			const int tile = blockFaceTex((BlockId)id, face);
			CHECK(tile >= 0 && tile < ATLAS_TILE_COUNT,
			      "block %d face %d maps to tile %d, outside the %d addressable slots (0..%d)",
			      id, face, tile, ATLAS_TILE_COUNT, ATLAS_TILE_COUNT - 1);
		}
	}
	// ...and none of them may land on the slot reserved for the missing-texture marker. That
	// slot's art IS the error indicator, so a real block pointing at it would render as the
	// thing the game shows when a texture does not exist. Counted into one check rather than
	// 1536 more: what matters is that the answer is zero, and the first offender names itself.
	{
		int marker_uses = 0, first_id = -1, first_face = -1;
		for (int id = 0; id < 256; id++) {
			for (int face = 0; face < BLOCK_FACES; face++) {
				if (blockFaceTex((BlockId)id, face) == ATLAS_TILE_MISSING) {
					if (marker_uses == 0) { first_id = id; first_face = face; }
					marker_uses++;
				}
			}
		}
		CHECK(marker_uses == 0,
		      "%d block faces map to tile %d, which is ATLAS_TILE_MISSING - first block %d face "
		      "%d. That slot is reserved for the missing-texture marker and tools/make_atlas.py "
		      "paints a magenta checker into it, so those faces would draw as an error",
		      marker_uses, ATLAS_TILE_MISSING, first_id, first_face);
	}
	// And the named blocks specifically, whose tiles must be distinct where the art is: grass
	// is the block that proves per-face tiles work at all, with three different tiles on one
	// cube. On a one-tile-wide sheet u can no longer tell two tiles apart - every tile's u is
	// 0..16 - so the slot index is the only discriminator left, and any test that still
	// separates tiles by u has been neutralised by the strip layout.
	{
		const AtlasRect top  = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_TOP));
		const AtlasRect side = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_EAST));
		const AtlasRect bot  = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_BOTTOM));
		CHECK(top.u0 == side.u0 && side.u0 == bot.u0,
		      "every tile shares one u span on a one-tile-wide sheet, but grass's are %d/%d/%d",
		      top.u0, side.u0, bot.u0);
		CHECK(top.vslot0 != side.vslot0 && side.vslot0 != bot.vslot0 && top.vslot0 != bot.vslot0,
		      "grass's three faces must land on three different slots, got vslot0 %d/%d/%d",
		      top.vslot0, side.vslot0, bot.vslot0);
	}

	// ── The V flip, at the two ends of the sheet ────────────────────────────────────────
	//
	// The per-slot form of this check lives in the ownership loop above, where the conversion
	// from slot units to PNG rows is actually performed. What is left here is the two ends,
	// because those are the ones a reversed flip swaps and they are worth naming: slot 0 is the
	// LAST TILE_PX rows of the image and the top slot is the FIRST. Getting that backwards puts
	// every tile on the wrong slot with no error at all - the sheet is still a valid sheet.
	CHECK(ATLAS_H_PX - (int)atlasRect(0).vslot1 * TILE_PX == ATLAS_H_PX - TILE_PX,
	      "slot 0 must be the LAST %d PNG rows - the v flip is backwards", TILE_PX);
	CHECK(ATLAS_H_PX - (int)atlasRect(ATLAS_TILE_COUNT - 1).vslot1 * TILE_PX == 0,
	      "the top slot (%d) must be the FIRST %d PNG rows, got row %d",
	      ATLAS_TILE_COUNT - 1, TILE_PX,
	      ATLAS_H_PX - (int)atlasRect(ATLAS_TILE_COUNT - 1).vslot1 * TILE_PX);

	// ── The built sheet's actual texels ─────────────────────────────────────────────────
	//
	// Everything above this line is arithmetic, and the F7 defect was not arithmetic: unpainted
	// slots produced perfectly valid rects over pixels nobody had painted. So this decodes
	// build/atlas.t3x - the object atlasInit() hands the GPU - and looks at what is in them.
	{
		char    t3x_err[300] = {0};
		T3xInfo t = readT3x(t3x_err, sizeof(t3x_err));
		CHECK(t.ok, "%s", t3x_err);

		if (t.ok) {
			CHECK(t.w == (unsigned)ATLAS_W_PX && t.h == (unsigned)ATLAS_H_PX,
			      "%s is %ux%u, not %dx%d", ATLAS_T3X_PATH, t.w, t.h, ATLAS_W_PX, ATLAS_H_PX);
			// 2 is GPU_RGBA5551. gfx/atlas.t3s asks for it and gfx/atlas.c's comment depends
			// on it (the leaves tile's binary alpha is what makes the 1-bit alpha lossless).
			CHECK(t.format == 2u, "%s is texture format %u, not 2 (GPU_RGBA5551) - gfx/atlas.t3s "
			      "asks tex3ds for rgba5551", ATLAS_T3X_PATH, t.format);
			// Load-bearing since the 2px inter-tile border went away: the slots are directly
			// adjacent in V now, and a mip level would average across the seam and bleed one
			// tile into the next at distance. See gfx/atlas.c.
			CHECK(t.mipmaps == 0u, "%s carries mipmapLevels=%u; with the strip layout's adjacent "
			      "slots a mip chain averages across the seam and bleeds one tile into the next",
			      ATLAS_T3X_PATH, t.mipmaps);
			CHECK(t.subtex_count == 1u, "%s declares %u subtextures, expected 1",
			      ATLAS_T3X_PATH, t.subtex_count);
			CHECK(t.sub_w == (unsigned)ATLAS_W_PX && t.sub_h == (unsigned)ATLAS_H_PX,
			      "%s's subtexture is %ux%u, not the whole %dx%d sheet",
			      ATLAS_T3X_PATH, t.sub_w, t.sub_h, ATLAS_W_PX, ATLAS_H_PX);

			// H11. The painted slots, pinned. Slots 0..9's fingerprints were taken from
			// the t3x built BEFORE F7 touched tools/make_atlas.py; slots 10 and 11 were added by
			// tasks 17/19 and computed independently from gfx/atlas.png, and 12..16 by v1.8.3
			// phase 3 the same way. "Independently" is load-bearing and is not a figure of
			// speech: the pins below were produced by a separate decoder walking the PNG's own
			// RGBA8 pixels through the RGBA5551 packing, NOT by running this suite and copying
			// what it printed. The two routes to a texel share nothing - this one goes through
			// LZ11 and the Morton swizzle and the other does not - so agreeing is evidence,
			// where a pin lifted out of the suite's own failure message would only ever have
			// been a record of what the code did. Slots 0..11's pins were left untouched by that
			// exercise and still hold, which is what says the seeded stream did not move.
			// The generator draws
			// every tile from ONE seeded stream in TILES order precisely so that appending to
			// TILES cannot disturb art already painted.
			//
			// THESE VALUES MUST NOT MOVE, AND TASK 13b IS NOT A REASON TO RE-PIN THEM. The sheet
			// went from 16 slots to 64 and every one of these tiles is drawn from the same
			// stream, in the same order, at the same size, and lands - via slot_png_y()'s flip -
			// at a DIFFERENT absolute PNG row than it did before. That is exactly why the pins
			// are per-slot and taken through SLOT_PNG_TOP(): the art is addressed by slot, so a
			// taller sheet moves where a slot IS without changing what is in it.
			//
			// So if one of these goes red after a sheet resize, the correct response is NOT to
			// re-pin it. It means the generator's slot addressing is wrong - the art has been
			// painted into a different slot than before, or the flip has been re-derived
			// incorrectly for the new height - and the sheet is shipping tiles under the wrong
			// tex bytes. Re-pinning would make that permanent and green.
			static const uint64_t kPaintedFingerprint[ATLAS_PAINTED_SLOTS] = {
				0xC8C34C2212D92C86ull,   //  0 grass_top
				0x1D3D28DFB5E9B829ull,   //  1 grass_side
				0xA40A6FBCE9741558ull,   //  2 dirt
				0x4289458043C29F17ull,   //  3 stone
				0x768D5E349E5A1EFFull,   //  4 sand
				0xC61D51839F0C7F25ull,   //  5 sentinel
				0xEA69BAC48A287A9Cull,   //  6 wood_side
				0xDDF9C5205F2EA5A1ull,   //  7 wood_top
				0x1D3D3D0320D56934ull,   //  8 leaves
				0x3E149457CC8AF270ull,   //  9 planks
				0x55BDFF89F02B7891ull,   // 10 water
				0x8CD474E09FA7C22Cull,   // 11 tall_grass
				0x655A7DA542F30C5Eull,   // 12 snow
				0x8A6AD29F1C304552ull,   // 13 ice
				0x27AD124F58810F53ull,   // 14 cactus
				0x2DF2ECEF5259A5A2ull,   // 15 dead_bush
				0x003F0B7EA35C1195ull,   // 16 fern
			};
			for (int slot = 0; slot < ATLAS_PAINTED_SLOTS; slot++) {
				const uint64_t got = slotFingerprint(SLOT_PNG_TOP(slot));
				CHECK(got == kPaintedFingerprint[slot],
				      "slot %d's art changed: fingerprint 0x%016" PRIX64 ", pinned 0x%016" PRIX64
				      ". tools/make_atlas.py draws from one seeded stream in TILES order, so any "
				      "change to the order, the seed, or a painter moves this. A SHEET RESIZE "
				      "must not: if this moved because ATLAS_H_PX changed, the generator's slot "
				      "addressing is wrong and re-pinning would ship it",
				      slot, got, kPaintedFingerprint[slot]);
			}

			// ── What the two new tiles ARE, not merely that they are stable ──────────────
			//
			// A fingerprint says "these 256 texels are the ones from last time". It cannot say
			// they are the RIGHT texels, and for a cross block one specific property decides
			// whether the block renders at all: the engine's RGBA5551 alpha is a single bit and
			// the transparent pass is an alpha TEST, not blending. A tall-grass tile whose
			// "gaps" came out alpha=1 does not render as grass with gaps - it renders as a solid
			// card, twice, at right angles. So the gaps are counted, by value, off the t3x.
			{
				const int png_top = SLOT_PNG_TOP(ATLAS_SLOT_TALL_GRASS);
				int cutout = 0, opaque = 0;
				for (int y = 0; y < TILE_PX; y++) {
					for (int x = 0; x < ATLAS_W_PX; x++) {
						((s_texel[png_top + y][x] & 1u) == 0u) ? cutout++ : opaque++;
					}
				}
				// Both bounds matter. Zero cutout texels is the solid card above; all-cutout is
				// an invisible block, which would pass a "has transparency" check and draw
				// nothing. The real tile measures 152 cutout / 104 opaque - the numbers are not
				// pinned, only the fact that the tile is genuinely a mix.
				CHECK(cutout > 0,
				      "the tall grass tile (slot %d) has NO alpha-0 texels: %d of %d are opaque. "
				      "RGBA5551 alpha is one bit and the transparent pass is an alpha test, so a "
				      "cross block with no cutout renders as two solid crossed cards",
				      ATLAS_SLOT_TALL_GRASS, opaque, TILE_PX * ATLAS_W_PX);
				CHECK(opaque > 0,
				      "the tall grass tile (slot %d) is entirely alpha-0: %d of %d texels are "
				      "cutout, so the block would be invisible rather than grass",
				      ATLAS_SLOT_TALL_GRASS, cutout, TILE_PX * ATLAS_W_PX);
			}

			// Water is the opposite claim and needs saying separately, because "transparent" in
			// the registry means "does not occlude, goes in the deferred pass" - it does not mean
			// the texture has holes. With one bit of alpha there is no partial coverage: an alpha
			// -0 texel in the water tile would be a hole you could see the seabed through, not a
			// see-through sea. Every texel must be opaque.
			{
				const int png_top = SLOT_PNG_TOP(ATLAS_SLOT_WATER);
				int cutout = 0, fx = -1, fy = -1;
				for (int y = 0; y < TILE_PX; y++) {
					for (int x = 0; x < ATLAS_W_PX; x++) {
						if ((s_texel[png_top + y][x] & 1u) == 0u) {
							if (cutout == 0) { fx = x; fy = y; }
							cutout++;
						}
					}
				}
				CHECK(cutout == 0,
				      "the water tile (slot %d) has %d alpha-0 texels, first at tile-local "
				      "(%d,%d). The engine's alpha is one bit tested, not blended, so a cutout "
				      "texel here is a hole through the sea rather than translucency",
				      ATLAS_SLOT_WATER, cutout, fx, fy);
			}

			// H12. And the painted tiles are not each other. The fingerprint loop would catch a
			// duplicate only by accident - it compares each slot against its own pin, never
			// against its neighbours - so a painter wired to the wrong function would pin
			// cleanly and ship two identical tiles. Bounded by ATLAS_PAINTED_SLOTS, not by the
			// slot count: the 47 unpainted slots are all the marker and ARE deliberately
			// identical to each other, which the marker sweep below owns instead.
			for (int a = 0; a < ATLAS_PAINTED_SLOTS; a++) {
				for (int b = a + 1; b < ATLAS_PAINTED_SLOTS; b++) {
					CHECK(slotFingerprint(SLOT_PNG_TOP(a)) != slotFingerprint(SLOT_PNG_TOP(b)),
					      "slots %d and %d are texel-identical; two painted tiles must not be "
					      "the same art under two names", a, b);
				}
			}

			// H9. Every slot the TILES list does not fill must be the marker, texel for texel.
			// This is the check the F7 defect exists in: before it, the spare slots were a flat
			// near-black fill and every geometry test in this file was green over them. Since
			// v1.8.3 phase 3 claimed 12..16 that is slots 17..63 - 47 of them - and the bound is
			// ATLAS_TILE_SLOTS so it follows the sheet rather than a number written here.
			for (int slot = ATLAS_PAINTED_SLOTS; slot < ATLAS_TILE_SLOTS; slot++) {
				const int png_top = SLOT_PNG_TOP(slot);
				int       wrong = 0, fx = -1, fy = -1;
				uint16_t  got = 0, want = 0;
				for (int y = 0; y < TILE_PX; y++) {
					for (int x = 0; x < ATLAS_W_PX; x++) {
						const uint16_t expect = markerTexel(x, y);
						if (s_texel[png_top + y][x] != expect) {
							if (wrong == 0) {
								fx = x; fy = y;
								got = s_texel[png_top + y][x]; want = expect;
							}
							wrong++;
						}
					}
				}
				CHECK(wrong == 0,
				      "slot %d is not the missing-texture marker: %d of %d texels differ, first "
				      "at tile-local (%d,%d) which is 0x%04X where the magenta/black quadrant "
				      "checker wants 0x%04X. An unpainted addressable slot draws as art, never "
				      "as an error - that is the whole F7 defect",
				      slot, wrong, TILE_PX * ATLAS_W_PX, fx, fy, got, want);
			}

			// H10. And no slot ANYWHERE on the sheet is a solid block of background fill. The
			// check above already covers slots 17..63 by value; this one covers the seventeen
			// painted ones too, and it is the one that would catch a future slot painted by a
			// painter that silently returned nothing.
			for (int slot = 0; slot < ATLAS_TILE_SLOTS; slot++) {
				const int png_top = SLOT_PNG_TOP(slot);
				int       fill = 0;
				for (int y = 0; y < TILE_PX; y++)
					for (int x = 0; x < ATLAS_W_PX; x++)
						if (s_texel[png_top + y][x] == TEXEL_OLD_FILL) fill++;
				CHECK(fill != TILE_PX * ATLAS_W_PX,
				      "slot %d is a solid block of the sheet's background fill (0x%04X). On a "
				      "block face that is an opaque near-black cube, which reads as a dark "
				      "block and never as a missing texture",
				      slot, (unsigned)TEXEL_OLD_FILL);
			}

			// The marker must be two high-contrast colours, not one. A single flat colour would
			// satisfy the texel loop above only if markerTexel() were also flat, so this is the
			// control that says the pattern being compared against is a pattern at all.
			{
				int magenta = 0, black = 0;
				for (int y = 0; y < TILE_PX; y++)
					for (int x = 0; x < ATLAS_W_PX; x++)
						(markerTexel(x, y) == TEXEL_MAGENTA) ? magenta++ : black++;
				CHECK(magenta == black && magenta == (TILE_PX * ATLAS_W_PX) / 2,
				      "the marker pattern is %d magenta and %d black texels, not an even split - "
				      "it is meant to be four half-tile quadrants", magenta, black);
				CHECK(TEXEL_MAGENTA != TEXEL_BLACK && TEXEL_MAGENTA != TEXEL_OLD_FILL,
				      "the marker's colours are not distinguishable from each other or from the "
				      "background fill");
			}
		}
	}

	// ── The ceiling task 13b actually moved, kept as a regression check ─────────────────
	//
	// This block used to prove the OPPOSITE, and the measurement it recorded is worth keeping
	// because it is what justified the change. Task 13a asked whether bumping ATLAS_H_PX to 512
	// would buy "31 addressable" slots, as world/atlas_uv.h's comment then claimed. It did not:
	// atlasRect() computed v0/v1 as `(uint8_t)(tile * TILE_PX [+ TILE_PX])`, a raw PIXEL offset
	// never scaled to the sheet height, so nothing about a taller sheet changed how far that
	// cast could count. Looping tiles 0..30 and checking each for an intact, distinct v0/v1 pair
	// went RED starting at tile 15, not 31 - measured verbatim from this file at the time:
	//
	//   FAIL L979  at a hypothetical ATLAS_H_PX=512, tile 15's rect (v0=240 v1=256, full
	//   pixel values) does not fit uint8_t and truncates
	//
	// Tiles 0..14 stayed green; every one of 15..30 failed the same way. Tile 30 truncated to
	// v1=240, which is tile 14's own v1 - it would have rendered as already-painted geometry
	// rather than failing loudly.
	//
	// Task 13b fixed the UNITS instead of the width, which is what the refutation pointed at:
	// vslot0/vslot1 hold `tile` and `tile + 1`, the shader's uvScale.y carries the TILE_PX
	// factor, and MeshVertex is still 8 bytes. The ceiling is now the PICA200's 1024 px texture
	// limit, i.e. 64 slots, and all 64 are addressable.
	//
	// What ships below is the permanent, GREEN form of that: the new bound asserted directly,
	// plus the old formula stated as arithmetic so the check can still tell the two units apart.
	{
		for (int t = 0; t < ATLAS_TILE_COUNT; t++) {
			const AtlasRect r = atlasRect(t);
			// The whole point of the change: both edge indices survive the uint8_t intact, for
			// every slot on a sheet four times taller than the one that used to overflow at 15.
			CHECK((int)r.vslot0 == t && (int)r.vslot1 == t + 1,
			      "slot %d's edge indices came back %d/%d, not %d/%d - a value that does not "
			      "survive MeshVertex.v's uint8_t looks exactly like this",
			      t, r.vslot0, r.vslot1, t, t + 1);
		}

		// The old pixel formula, computed here rather than by atlasRect(), showing that it
		// would still overflow at slot 15 on this very sheet. This is what says the two units
		// are genuinely different and that the ceiling moved because of the UNITS change and
		// not because the sheet grew: the sheet growing is what the refuted claim proposed, and
		// it would not have helped.
		const int     old_v1_at_15 = 15 * TILE_PX + TILE_PX;      // 256, the pre-13b top edge
		const uint8_t old_v1_u8    = (uint8_t)old_v1_at_15;
		CHECK(old_v1_at_15 != (int)old_v1_u8,
		      "the pre-task-13b pixel formula for slot 15's top edge (%d) now fits in a uint8_t "
		      "(%d). If that is true, TILE_PX has changed and this file's account of why the "
		      "units had to change needs re-deriving rather than re-reading",
		      old_v1_at_15, (int)old_v1_u8);
		// And the new formula for the SAME slot fits with room to spare - 16 against 256.
		CHECK((int)atlasRect(15).vslot1 == 16,
		      "slot 15's top edge index is %d, not 16 - the slot-unit form is what lifted the "
		      "ceiling past the pixel form's overflow at exactly this slot",
		      atlasRect(15).vslot1);
		// The top slot of the sheet, which under the old units was unreachable at ANY height.
		CHECK((int)atlasRect(ATLAS_TILE_COUNT - 1).vslot1 == ATLAS_TILE_COUNT,
		      "the top slot's edge index is %d, not %d - the slot the old pixel units could "
		      "never address is the one reserved for the missing-texture marker, so losing it "
		      "loses the error indicator itself",
		      atlasRect(ATLAS_TILE_COUNT - 1).vslot1, ATLAS_TILE_COUNT);
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
