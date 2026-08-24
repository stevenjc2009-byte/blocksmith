// Guards the atlas layout: the numbers a .pica file cannot get from C, the geometry
// atlasRect() produces, and the sheet the generator actually wrote.
//
// THE SHADER HALF. Both source/shaders/world.v.pica and source/shaders/world_dynamic.v.pica
// carry a `uvScale` constant that must equal (1/ATLAS_W_PX, 1/ATLAS_H_PX), with both defined
// in world/atlas_uv.h. world/block_tiles_check.c's trick for the same kind of problem
// (duplicate a value on both sides, then _Static_assert the two copies agree) is not
// available here: the picasso shader compiler has no #include and no _Static_assert, so there
// is no way to make a .pica file read ATLAS_W_PX at all. This test parses the shader source
// text instead, at host-test time, and checks the literals against the one C definition.
//
// The bug this guards against already happened once. Step 9.3c shrank the atlas sheet from
// 256x256 to 64x64 and updated the header, but the shader's uvScale constant was left at
// 1/256. A wrong scale still produces valid UVs, so nothing crashed or errored - it just
// rendered every tile as the same ~14x14px corner of the sheet (the wood tile), so the whole
// world drew flat brown with no grass green and no stone grey anywhere.
//
// Two things widened this in v1.6.0, both because the sheet became a 16x256 one-tile-wide
// strip and stopped being square:
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
// where the layout has to be proven. Every addressable tile must return a 16x16 rect that
// lies wholly inside the sheet and shares no TEXEL with any other tile; the rect is half-open
// (it covers rows v0..v1-1), so adjacent slots legitimately share the coordinate v1==v0 while
// sharing no pixel, and a test written on coordinate overlap instead of texel overlap would
// fail on a correct sheet. The U-repeat property greedy meshing depends on is proven
// arithmetically here as well, since no host test can sample a texture.
//
// THE SHEET HALF (new in v1.6.0). world/atlas_uv.h's own comment used to say the duplication
// with tools/make_atlas.py was unguarded because "the PNG carries no dimensions the C side
// reads". It does - a PNG's IHDR chunk is at a fixed offset - so this test reads gfx/atlas.png
// directly and checks its width and height against ATLAS_W_PX/ATLAS_H_PX. That closes the
// last unguarded copy of the layout.
//
// THE PIXEL HALF (new in v1.6.0 F7). Everything above is geometry, and geometry cannot tell
// you whether a slot has any ART in it. Two slots' worth of that was exactly the F7 defect:
// tex 10..14 addressed real, addressable slots that tools/make_atlas.py never painted, so they
// drew the sheet's background fill as an opaque near-black solid, and an out-of-range tex
// clamped to slot 0 and drew grass. Neither is an error at any level a geometry test can see -
// both are a block with a texture on it. So this file now DECODES build/atlas.t3x, the actual
// object the GPU is handed (LZ11, then the PICA200's 8x8-tile Morton swizzle), and asserts on
// texel values: that every slot the TILES list does not fill is the magenta/black missing-
// texture marker, that no slot anywhere is still a solid block of background fill, and that
// each of the ten painted slots still fingerprints to what it did before F7 touched the
// generator. This is as close to looking at the sheet as a host test can get.
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
#define NEEDLE ".constf uvScale("

// The number of slots tools/make_atlas.py's TILES list actually paints art into. Duplicated
// from gfx/atlas.h's enum rather than included, because that header pulls in <3ds.h>; the
// coverage loop further down is what stops the two drifting, since every block's face tile
// has to land below this and the marker check owns everything at or above it.
#define ATLAS_PAINTED_SLOTS 12

// Slots 10 and 11 within that: water and tall grass (roadmap tasks 17 and 19). Named here
// because the two texel-content checks further down are about what these two tiles ARE, not
// merely that they are stable - one must have cutout texels and the other must not.
#define ATLAS_SLOT_WATER      10
#define ATLAS_SLOT_TALL_GRASS 11

// The shader literals are written to 8 decimal digits, so the tolerance has to be looser than
// that literal's own rounding while staying far tighter than the gap to any wrong value: the
// historical bug value 1/256 = 0.00390625 is ~0.0117 away from 1/64, over 100000x this.
#define UV_TOLERANCE 1e-7

static double absd(double v) { return v < 0.0 ? -v : v; }

// Reads `path` looking for the ".constf uvScale(a, b, c, d)" line and extracts components a
// and b - the U and V divisors. Returns true and fills *outU/*outV only when every step
// succeeds (file opens, the line is found, it has exactly four comma-separated components,
// and the first two parse cleanly as numbers with nothing left over). Otherwise fills errbuf
// with which step failed and returns false, so the caller can turn that into a real failure
// instead of silently passing.
static bool readShaderUvScale(const char* path, double* outU, double* outV,
                              char* errbuf, size_t errbufsz)
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
static void checkShader(const char* path, double* outU, double* outV)
{
	double u = -1.0, v = -1.0;
	char err[256] = {0};

	const bool ok = readShaderUvScale(path, &u, &v, err, sizeof(err));
	CHECK(ok, "%s", err);
	if (!ok) { *outU = *outV = -1.0; return; }

	const double want_u = 1.0 / (double)ATLAS_W_PX;
	const double want_v = 1.0 / (double)ATLAS_H_PX;

	CHECK(absd(u - want_u) < UV_TOLERANCE,
	      "%s uvScale.x=%.8f but %s ATLAS_W_PX=%d means 1/ATLAS_W_PX=%.8f (diff %.8f)",
	      path, u, ATLAS_HEADER_PATH, ATLAS_W_PX, want_u, absd(u - want_u));
	CHECK(absd(v - want_v) < UV_TOLERANCE,
	      "%s uvScale.y=%.8f but %s ATLAS_H_PX=%d means 1/ATLAS_H_PX=%.8f (diff %.8f)",
	      path, v, ATLAS_HEADER_PATH, ATLAS_H_PX, want_v, absd(v - want_v));

	*outU = u;
	*outV = v;
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
//      of texture-space v. Verified against gfx/atlas.png with an independent decoder: all
//      4096 texels matched with no flip, and all 4096 mismatched with one, so the orientation
//      below is measured rather than assumed.
//
// s_texel is indexed [png_row][x], row 0 being the TOP of the sheet.
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
		         "- the sheet and the code disagree about the layout", ATLAS_T3X_PATH, t.w, t.h,
		         ATLAS_W_PX, ATLAS_H_PX);
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

	// The two shaders against each other, not only against the header. An Old 3DS binds one
	// and a New 3DS the other, so they have to agree or the same world looks different on the
	// two consoles - and that is a difference nobody would attribute to a texture constant.
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
	// The top slot is unaddressable because its v1 would be ATLAS_H_PX, which is 256 and does
	// not fit in MeshVertex's uint8_t v. Both halves are checked: the last addressable slot
	// must fit, and the slot after it must not - otherwise ATLAS_TILE_COUNT is simply wrong.
	CHECK((ATLAS_TILE_COUNT) * TILE_PX <= 255,
	      "the last addressable slot's top edge is v=%d, which does not fit in uint8_t",
	      ATLAS_TILE_COUNT * TILE_PX);
	CHECK((ATLAS_TILE_COUNT + 1) * TILE_PX > 255,
	      "slot %d's top edge is v=%d and DOES fit in uint8_t, so ATLAS_TILE_COUNT=%d is leaving an addressable slot unused",
	      ATLAS_TILE_COUNT, (ATLAS_TILE_COUNT + 1) * TILE_PX, ATLAS_TILE_COUNT);

	// ── Every tile's rect, and the no-shared-pixel property ─────────────────────────────
	//
	// A per-texel ownership map, not a rect-overlap comparison: the rects are half-open, so
	// adjacent slots share the coordinate v1==v0 while sharing no pixel, and a coordinate
	// test would go red on a perfectly correct sheet.
	static unsigned char owner[ATLAS_H_PX][ATLAS_W_PX];   // 0 = unclaimed, tile+1 otherwise
	memset(owner, 0, sizeof(owner));
	int claimed = 0;

	for (int t = 0; t < ATLAS_TILE_COUNT; t++) {
		const AtlasRect r = atlasRect(t);

		CHECK(r.u1 - r.u0 == TILE_PX, "tile %d spans %d px in u, not %d", t, r.u1 - r.u0, TILE_PX);
		CHECK(r.v1 - r.v0 == TILE_PX, "tile %d spans %d px in v, not %d", t, r.v1 - r.v0, TILE_PX);
		CHECK(r.u1 <= ATLAS_W_PX, "tile %d's u1=%d runs past the %d px sheet width", t, r.u1, ATLAS_W_PX);
		CHECK(r.v1 <= ATLAS_H_PX, "tile %d's v1=%d runs past the %d px sheet height", t, r.v1, ATLAS_H_PX);
		// v0/v1 are uint8_t, so a v1 that should have been >255 comes back wrapped and small
		// rather than out of range. Comparing against the arithmetic in int catches that.
		CHECK((int)r.v0 == t * TILE_PX, "tile %d's v0=%d, expected %d (a wrapped uint8_t looks like this)",
		      t, r.v0, t * TILE_PX);

		for (int y = r.v0; y < r.v1; y++) {
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
	// The unaddressable top slot must genuinely be untouched, or ATLAS_TILE_COUNT is a lie.
	{
		int stray = 0;
		for (int y = ATLAS_TILE_COUNT * TILE_PX; y < ATLAS_H_PX; y++)
			for (int x = 0; x < ATLAS_W_PX; x++)
				if (owner[y][x] != 0) stray++;
		CHECK(stray == 0, "%d texels above the last addressable slot were claimed by a tile", stray);
	}

	// ── The U-repeat property greedy meshing depends on ─────────────────────────────────
	//
	// No host test can sample a texture, so this is arithmetic on the same thing the GPU
	// does: with GPU_REPEAT and a sheet TILE_PX wide, texture coordinate u maps to texel
	// u mod TILE_PX. A merged quad `k+1` blocks wide emits u1 = u0 + TILE_PX*k, and the
	// property that makes the merge legal is that every one of those lands back on the
	// same column of the same tile.
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
	// world/mesher.c builds its rect table for all 256 registry ids, and a dynamic block
	// registered over the wire carries an arbitrary tile byte, so this is reachable in
	// production, not a theoretical edge.
	//
	// It must land on the MISSING-TEXTURE MARKER, not on tile 0 (v1.6.0 F7). Clamping to 0
	// drew grass: a block declared with a wrong tex byte came out as a perfectly plausible
	// grass block, and the player - and the server author - had nothing to see. Checked as
	// "identical to atlasRect(ATLAS_TILE_MISSING)", not merely as "in range", because without
	// the clamp the arithmetic wraps mod 256 and for some out-of-range indices the wrap lands
	// on a perfectly in-range-looking rect: 200*16 mod 256 = 128, i.e. the leaves slot. An
	// in-range test alone therefore CANNOT go red for those, and a case that cannot go red is
	// not cover.
	//
	// ATLAS_TILE_SLOTS (16) used to be the one index no test here could discriminate on:
	// 16 * 16 = 256 wraps to exactly 0, so with a clamp-to-0 the wrapped rect IS the clamped
	// rect. Clamping to slot 14 instead gives it a distinct answer (v0 224, not 0), so it now
	// goes red with the rest of them.
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
		CHECK((int)missing.v0 == ATLAS_TILE_MISSING * TILE_PX,
		      "atlasRect(ATLAS_TILE_MISSING) gave v0=%d, expected %d", missing.v0,
		      ATLAS_TILE_MISSING * TILE_PX);
		// If the marker rect were tile 0's rect, every check below would pass for the OLD
		// clamp as well and none of them would be cover at all.
		CHECK(missing.v0 != zero.v0,
		      "the marker slot's rect is tile 0's rect, so no check here can tell the F7 clamp "
		      "from the one it replaced");

		const int bad[] = { -1, ATLAS_TILE_COUNT, ATLAS_TILE_SLOTS, 200, 255, 1000 };
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			const AtlasRect r = atlasRect(bad[i]);
			CHECK(r.u0 == missing.u0 && r.u1 == missing.u1 && r.v0 == missing.v0 && r.v1 == missing.v1,
			      "atlasRect(%d) gave u %d..%d v %d..%d; an unaddressable tile must clamp to the "
			      "missing-texture marker at slot %d (u %d..%d v %d..%d) - clamping to tile 0's "
			      "u %d..%d v %d..%d draws GRASS for a wrong tex byte, and wrapping the uint8_t "
			      "draws whatever slot the wrap lands on",
			      bad[i], r.u0, r.u1, r.v0, r.v1, ATLAS_TILE_MISSING,
			      missing.u0, missing.u1, missing.v0, missing.v1,
			      zero.u0, zero.u1, zero.v0, zero.v1);
		}
	}

	// ── Every real block, every face ────────────────────────────────────────────────────
	//
	// The REAL block.c and registry.c are linked, so this is a claim about what blockFaceTex()
	// returns rather than about a table copied into this file. Every id the mesher can ask
	// about - all 256 raw byte values, exactly as world/mesher.c's planBuild does - must map
	// to an addressable slot, or the atlas silently renders that block as tile 0.
	registryInitCore();
	for (int id = 0; id < 256; id++) {
		for (int face = 0; face < BLOCK_FACES; face++) {
			const int tile = blockFaceTex((BlockId)id, face);
			CHECK(tile >= 0 && tile < ATLAS_TILE_COUNT,
			      "block %d face %d maps to tile %d, outside the %d addressable slots",
			      id, face, tile, ATLAS_TILE_COUNT);
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
	// 0..16 - so v is the only discriminator left, and any test that still separates tiles by
	// u has been neutralised by the strip layout.
	{
		const AtlasRect top  = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_TOP));
		const AtlasRect side = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_EAST));
		const AtlasRect bot  = atlasRect(blockFaceTex(BLOCK_GRASS, FACE_BOTTOM));
		CHECK(top.u0 == side.u0 && side.u0 == bot.u0,
		      "every tile shares one u span on a one-tile-wide sheet, but grass's are %d/%d/%d",
		      top.u0, side.u0, bot.u0);
		CHECK(top.v0 != side.v0 && side.v0 != bot.v0 && top.v0 != bot.v0,
		      "grass's three faces must land on three different slots, got v0 %d/%d/%d",
		      top.v0, side.v0, bot.v0);
	}

	// ── The V flip, which the generator has to implement in reverse ─────────────────────
	//
	// Texture v grows upwards and the PNG's rows run downwards, so slot t - which owns
	// texture rows [t*16, t*16+16) - is written to PNG rows ATLAS_H_PX-v1 .. ATLAS_H_PX-v0-1.
	// Slot 0 is therefore the LAST 16 rows of the image, not the first. That reversal is what
	// tools/make_atlas.py's slot_png_y() implements, and getting it backwards would put every
	// tile on the wrong slot with no error at all - the sheet would still be a valid sheet.
	for (int t = 0; t < ATLAS_TILE_COUNT; t++) {
		const AtlasRect r = atlasRect(t);
		const int png_top = ATLAS_H_PX - (int)r.v1;
		CHECK(png_top == ATLAS_H_PX - (t + 1) * TILE_PX,
		      "slot %d's PNG top row is %d, expected %d", t, png_top, ATLAS_H_PX - (t + 1) * TILE_PX);
		CHECK(png_top >= 0 && png_top + TILE_PX <= ATLAS_H_PX,
		      "slot %d maps to PNG rows %d..%d, outside the image", t, png_top, png_top + TILE_PX - 1);
	}
	CHECK(ATLAS_H_PX - (int)atlasRect(0).v1 == ATLAS_H_PX - TILE_PX,
	      "slot 0 must be the LAST %d PNG rows - the v flip is backwards", TILE_PX);

	// ── The built sheet's actual texels ─────────────────────────────────────────────────
	//
	// Everything above this line is arithmetic, and the F7 defect was not arithmetic: slots
	// 10..14 produced perfectly valid rects over pixels nobody had painted. So this decodes
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

			// The twelve painted slots, pinned. Slots 0..9's fingerprints were taken from the
			// t3x built BEFORE F7 touched tools/make_atlas.py, so they are what says the art did
			// not move when the generator learned to paint the spares. They are UNCHANGED by
			// tasks 17/19: water and tall grass were appended to TILES, and the generator draws
			// every tile from one seeded stream in TILES order precisely so that appending
			// cannot disturb art already painted. These ten values not moving IS that proof.
			//
			// Slots 10 and 11 are the two new tiles. Their values were computed independently
			// from gfx/atlas.png (RGBA5551-packed and FNV'd in Python) and match what this
			// decoder gets out of build/atlas.t3x, so the t3x really carries what was painted.
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
			};
			for (int slot = 0; slot < ATLAS_PAINTED_SLOTS; slot++) {
				const int      png_top = ATLAS_H_PX - (slot + 1) * TILE_PX;
				const uint64_t got     = slotFingerprint(png_top);
				CHECK(got == kPaintedFingerprint[slot],
				      "slot %d's art changed: fingerprint 0x%016" PRIX64 ", pinned 0x%016" PRIX64
				      ". tools/make_atlas.py draws from one seeded stream in TILES order, so any "
				      "change to the order, the seed, or a painter moves this",
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
				const int png_top = ATLAS_H_PX - (ATLAS_SLOT_TALL_GRASS + 1) * TILE_PX;
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
				const int png_top = ATLAS_H_PX - (ATLAS_SLOT_WATER + 1) * TILE_PX;
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

			// And the two new tiles are not each other, nor a copy of a tile that was already
			// there. The fingerprint loop would catch a duplicate only by accident - it compares
			// each slot against its own pin, never against its neighbours - so a painter wired
			// to the wrong function would pin cleanly and ship two identical tiles.
			for (int a = 0; a < ATLAS_PAINTED_SLOTS; a++) {
				for (int b = a + 1; b < ATLAS_PAINTED_SLOTS; b++) {
					const int ta = ATLAS_H_PX - (a + 1) * TILE_PX;
					const int tb = ATLAS_H_PX - (b + 1) * TILE_PX;
					CHECK(slotFingerprint(ta) != slotFingerprint(tb),
					      "slots %d and %d are texel-identical; two painted tiles must not be "
					      "the same art under two names", a, b);
				}
			}

			// Every slot the TILES list does not fill must be the marker, texel for texel. This
			// is the check the F7 defect exists in: before it, slots 10..14 were a flat
			// near-black fill and every geometry test in this file was green over them.
			for (int slot = ATLAS_PAINTED_SLOTS; slot < ATLAS_TILE_SLOTS; slot++) {
				const int png_top = ATLAS_H_PX - (slot + 1) * TILE_PX;
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

			// And nothing anywhere on the sheet is still a solid block of background fill. The
			// check above already covers slots 12..15 by value; this one covers the twelve
			// painted ones too, and it is the one that would catch a future slot painted by a
			// painter that silently returned nothing.
			for (int slot = 0; slot < ATLAS_TILE_SLOTS; slot++) {
				const int png_top = ATLAS_H_PX - (slot + 1) * TILE_PX;
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
