#include "debug/biomeborder.h"

#include <stddef.h>

// ── The neon table ───────────────────────────────────────────────────────────────────
//
// Indexed by BiomeId, so the order here IS world/worldgen.h's enum order and a shuffle is a
// wrong colour on every seam rather than a compile error. source/debug/biomeborder_test.c
// pins each colour to its enumerator by hand for that reason, and the _Static_assert below
// catches the other half of it — a biome added to the enum without a colour added here.
//
// Chosen to be NEON: every one of them has at least one channel pinned at 255 and at least
// one at or near 0, so the fence reads as an emitted light against terrain that is, by the
// time it reaches the screen, a shaded and fogged texture multiply. There is no such limit
// here as the one world/mesher.h documents for the tint palette — that palette MULTIPLIES
// finished art and so can only darken, while this colour REPLACES the fragment outright
// through one GPU_CONSTANT/GPU_REPLACE TEV stage. A fence may therefore be brighter than
// anything in the world, which is the entire point of it.
static const uint32_t BIOME_NEON[] = {
	BB_RGBA(  0, 200, 255, 255),   // BIOME_TUNDRA — electric ice blue
	BB_RGBA(  0, 255, 160, 255),   // BIOME_TAIGA  — cold spring green
	BB_RGBA(200, 255,   0, 255),   // BIOME_PLAINS — acid yellow-green
	BB_RGBA(  0, 255,  64, 255),   // BIOME_FOREST — pure green
	BB_RGBA(255, 160,   0, 255),   // BIOME_DESERT — hot orange
	BB_RGBA(255,   0, 220, 255),   // BIOME_JUNGLE — magenta
};

_Static_assert(sizeof(BIOME_NEON) / sizeof(BIOME_NEON[0]) == BIOME_COUNT,
               "BIOME_NEON is out of step with BiomeId in world/worldgen.h — a biome was "
               "added to the enum without a neon colour here");

uint32_t biomeBorderBiomeColour(BiomeId b)
{
	// See the note in the header: ONE unsigned comparison. The obvious signed pair fails the
	// CONSOLE build under -fshort-enums + -Werror=type-limits while compiling clean here.
	if ((unsigned)b >= (unsigned)BIOME_COUNT) return BB_RGBA(255, 255, 255, 255);
	return BIOME_NEON[(unsigned)b];
}

int biomeBorderColourKey(const BiomeBorderSeg* s)
{
	if (!s) return -1;
	return s->lo < s->hi ? (int)s->lo : (int)s->hi;
}

uint32_t biomeBorderColour(const BiomeBorderSeg* s)
{
	const int key = biomeBorderColourKey(s);
	if (key < 0) return BB_RGBA(255, 255, 255, 255);
	return biomeBorderBiomeColour((BiomeId)key);
}

int biomeBorderCountFor(const BiomeBorderSeg* segs, int n, int key)
{
	if (!segs || n <= 0) return 0;
	if (key < 0) return n;
	int c = 0;
	for (int i = 0; i < n; i++)
		if (biomeBorderColourKey(&segs[i]) == key) c++;
	return c;
}

// ── State ────────────────────────────────────────────────────────────────────────────
//
// s_enabled is a plain bool with no initialiser, so it is in .bss and starts false. That IS
// the "defaults OFF" guarantee and it is worth being explicit about: there is no options.c
// key for this, nothing reads a saved value into it, and the only writer in the whole tree is
// biomeBorderSetEnabled(). biomeborder_test.c asserts the default rather than trusting it.
static bool            s_enabled;
static bool            s_truncated;
static const WorldGen* s_gen;

void biomeBorderSetEnabled(bool on) { s_enabled = on; }
bool biomeBorderEnabled(void)       { return s_enabled; }

void biomeBorderSetWorldGen(const WorldGen* g) { s_gen = g; }
const WorldGen* biomeBorderWorldGen(void)      { return s_gen; }

void biomeBorderReset(void)
{
	s_enabled   = false;
	s_truncated = false;
	s_gen       = NULL;
}

bool biomeBorderTruncated(void) { return s_truncated; }

// ── The search ───────────────────────────────────────────────────────────────────────

int biomeBorderBuild(const WorldGen* g, int32_t px, int32_t pz, int radius,
                     BiomeBorderSeg* out, int cap)
{
	s_truncated = false;
	if (!g || !out || cap < 1) return 0;

	if (radius < 0)             radius = 0;
	if (radius > BB_MAX_RADIUS) radius = BB_MAX_RADIUS;

	const int32_t x0 = px - radius;
	const int32_t z0 = pz - radius, z1 = pz + radius;
	const int      w  = radius * 2 + 1;

	// Two rows of classifications kept at a time, so each column inside the square is
	// classified ONCE instead of the three times the obvious
	//   biomeAt(x,z) != biomeAt(x+1,z) || biomeAt(x,z) != biomeAt(x,z+1)
	// would cost. One extra column and one extra row are sampled past the far edge so that
	// the seams on the boundary of the square are found too — without them the fence would
	// stop one column short of the search area and read as the seam ending there.
	//
	// +2 for exactly that overhang. BB_MAX_RADIUS bounds it, which is why the clamp above is
	// not optional: an unclamped radius would walk off these arrays.
	static uint8_t row_a[BB_MAX_RADIUS * 2 + 2];
	static uint8_t row_b[BB_MAX_RADIUS * 2 + 2];
	uint8_t* cur  = row_a;
	uint8_t* next = row_b;

	for (int i = 0; i <= w; i++)
		cur[i] = (uint8_t)worldgenBiomeAt(g, x0 + i, z0);

	int n = 0;
	for (int32_t z = z0; z <= z1; z++) {
		// The row one step further along z, needed for every axis-1 test on this row.
		for (int i = 0; i <= w; i++)
			next[i] = (uint8_t)worldgenBiomeAt(g, x0 + i, z + 1);

		for (int i = 0; i < w; i++) {
			const int32_t x  = x0 + i;
			const uint8_t me = cur[i];

			if (me != cur[i + 1]) {
				if (n >= cap) { s_truncated = true; return n; }
				out[n].x = x; out[n].z = z; out[n].axis = 0;
				out[n].lo = me; out[n].hi = cur[i + 1];
				n++;
			}
			if (me != next[i]) {
				if (n >= cap) { s_truncated = true; return n; }
				out[n].x = x; out[n].z = z; out[n].axis = 1;
				out[n].lo = me; out[n].hi = next[i];
				n++;
			}
		}
		// Column x1+1 is sampled but never emitted for. It exists only so that the axis-0
		// test at i == w-1 has a neighbour to compare against; emitting for it as well would
		// make the covered area one column wider in x for along-z seams than for along-x
		// ones, and the contract in the header — both axes, exactly the columns
		// [px-radius, px+radius] x [pz-radius, pz+radius] — would stop being statable in one
		// line. biomeborder_test.c sweeps that exact rectangle for missed edges, so an
		// asymmetry here would have to be encoded in the test as a special case, which is
		// how a wrong contract gets frozen into a passing check.

		uint8_t* t = cur; cur = next; next = t;
	}

	return n;
}

// ── The vertices ─────────────────────────────────────────────────────────────────────

int biomeBorderVerts(const WorldGen* g, const BiomeBorderSeg* segs, int n, int key,
                     float* out, int cap_floats)
{
	if (!g || !segs || !out || n <= 0) return 0;

	const int sel = biomeBorderCountFor(segs, n, key);
	if (sel == 0) return 0;
	// All or nothing. A wall missing its second triangle is a HOLE in the overlay, and a hole
	// reads as "no seam here" — the one lie this feature must not tell.
	if (cap_floats < sel * BB_FLOATS_PER_SEG) return 0;

	int o = 0;
	for (int i = 0; i < n; i++) {
		const BiomeBorderSeg* s = &segs[i];
		if (key >= 0 && biomeBorderColourKey(s) != key) continue;

		// worldgenHeight() returns the first AIR y, so the top solid block is at height-1
		// (world/worldgen.h). Both sides are asked, and the wall spans both, so a seam that
		// runs along a cliff face is still planted in the ground at its foot and still clears
		// the terrain at its head.
		const int hl = worldgenHeight(g, s->x, s->z);
		const int hr = s->axis == 0 ? worldgenHeight(g, s->x + 1, s->z)
		                            : worldgenHeight(g, s->x, s->z + 1);

		const float y0 = (float)((hl < hr ? hl : hr) - BB_WALL_DOWN);
		const float y1 = (float)((hl > hr ? hl : hr) + BB_WALL_UP);

		float ax, az, bx, bz;
		if (s->axis == 0) {
			// The plane BETWEEN (x,z) and (x+1,z): world x+1, running the width of the column
			// in z. Not x+0.5 — positions here are block corners, the same whole-block space
			// world.v.pica's v0 is in, and the face of column x is at x+1 exactly.
			ax = (float)(s->x + 1); az = (float)s->z;
			bx = (float)(s->x + 1); bz = (float)(s->z + 1);
		} else {
			ax = (float)s->x;       az = (float)(s->z + 1);
			bx = (float)(s->x + 1); bz = (float)(s->z + 1);
		}

		// Two triangles, unindexed, matching the GPU_TRIANGLES C3D_DrawArrays the draw half
		// issues. Winding is not load-bearing: the draw half sets GPU_CULL_NONE, because a
		// one-quad-thick fence seen from its back side must still be there.
		const float quad[4][3] = {
			{ ax, y0, az }, { bx, y0, bz }, { bx, y1, bz }, { ax, y1, az },
		};
		static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k = 0; k < 6; k++) {
			out[o++] = quad[tri[k]][0];
			out[o++] = quad[tri[k]][1];
			out[o++] = quad[tri[k]][2];
		}
	}
	return o;
}
