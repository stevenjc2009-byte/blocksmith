// v1.8.8 DEBUG-ONLY feature: NEON BIOME BORDERS — a glowing fence standing on the ground
// wherever the world stops being one biome and starts being another.
//
// ── This is a debug option and nothing else ──────────────────────────────────────────
//
// It is registered as one DEBUG_TOGGLE row in source/main.c's bsDebugRegister(), it starts
// OFF, and the only way to reach it is the pause menu's Options page -> Debug -> the row.
// No hotkey, no options.c persistence, no title-screen setting, nothing on the bottom-screen
// HUD. Turning the game on and playing it normally can never switch this on by accident, and
// a build that never opens the debug menu never executes a line of the drawing half.
//
// OFF HAS TO BE FREE, not merely quiet, and that shaped the whole module. With the toggle
// off the frame is byte-for-byte the frame this program drew before the feature existed:
// nothing is built, no GPU state is touched, and no draw call is issued. The entire cost is
// one byte loaded and one branch taken, once per eye per frame, at the call site in drawEye.
//
// ── Why the geometry lives in a pure file and the GPU calls live in another ──────────
//
// Same split as debug/blocklist.h against app/debugmenu_ui.c's console half, and for the
// reason that file states outright: the part with the bugs in it is the arithmetic, and the
// arithmetic does not need a GPU to be wrong. This header and biomeborder.c have no <3ds.h>
// and no citro3d, so source/debug/biomeborder_test.c links them for real and checks the
// segments against the REAL worldgenBiomeAt(). debug/biomeborder_draw.c is the dumb half: it
// memcpy's the vertices this module emits and issues one C3D_DrawArrays per colour.
//
// It goes as far as blocklist.h does, deliberately: this module emits the FINISHED VERTICES,
// not "a description of a fence for someone else to turn into triangles". A test that
// re-derives what it thinks the screen draws proves only that two pieces of arithmetic agree.
// This project has already shipped a compiling, test-passing diff that rendered garbage.
//
// ── A POINTER to the live WorldGen, never a seed ─────────────────────────────────────
//
// biomeBorderSetWorldGen() takes a const WorldGen*, and that is load-bearing rather than a
// style choice — it is the same trap debug/biomeinfo.h documents. worldgenInit() does NOT
// store the seed it is handed; it stores rngMix(seed ^ 'BLKS'). So an accessor handing back
// g->seed and a worldgenInit() call on this side would mix an already-mixed seed and classify
// against a completely different noise field. That failure is SILENT: every lookup still
// returns a valid BiomeId, a fence still appears, and it is simply standing in the wrong
// place with nothing on screen to say so. Passing the live pointer cannot express that bug.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/worldgen.h"   // BiomeId, WorldGen, worldgenBiomeAt, worldgenHeight

// ── Sizes ────────────────────────────────────────────────────────────────────────────

// How far out from the player the search runs, in block columns. The searched square is
// (2R+1)^2 columns and each column costs two worldgenBiomeAt() lookups, so this number is
// the module's whole cost model — see the measured figure in biomeborder.c.
//
// 24 rather than the render distance: the fence is a debug aid for "where is the seam I am
// standing near", not a map. At 24 the seam is visible well before the player walks into it
// and the rebuild stays cheap enough to run on a column change.
#define BB_RADIUS 24

// Hard ceiling, so the row buffer and every caller's array are sized by a constant.
#define BB_MAX_RADIUS 32

// Segments one build may emit. A biome seam crossing a 49x49 square runs a few hundred
// column edges at its raggedest; 512 leaves headroom. A build that fills this stops early
// and says so through biomeBorderTruncated() rather than drawing a fence that silently ends
// in mid-air.
#define BB_MAX_SEGS 512

// Vertices per segment: one quad as two triangles, unindexed, matching the GPU_TRIANGLES
// C3D_DrawArrays that scene/highlight.c uses with the same shader.
#define BB_VERTS_PER_SEG 6
#define BB_FLOATS_PER_SEG (BB_VERTS_PER_SEG * 3)

// How far the fence stands above the taller of the two columns it divides, in blocks, and
// how far it is sunk below the shorter one. Sinking it matters: on a cliff the two sides can
// differ by many blocks, and a fence planted only at the top edge would hang in the air.
#define BB_WALL_UP   8
#define BB_WALL_DOWN 1

// ── One edge of the biome map ────────────────────────────────────────────────────────

// The edge between column (x, z) and its neighbour one step along `axis`. The wall stands in
// the plane BETWEEN them — at world x+1 for axis 0, at world z+1 for axis 1 — so a segment
// names the low-side column and the axis, and never a "position" that could be off by half a
// block without anything noticing.
typedef struct {
	int32_t x, z;    // the low-side column
	uint8_t axis;    // 0: neighbour is (x+1, z).  1: neighbour is (x, z+1).
	uint8_t lo;      // BiomeId of (x, z)
	uint8_t hi;      // BiomeId of the neighbour
} BiomeBorderSeg;

// ── The neon ─────────────────────────────────────────────────────────────────────────

// citro3d's C3D_TexEnvColor takes the PICA TEV constant register's own byte order, which is
// R in the LOW byte: 0xAABBGGRR. Writing 0xFF00FF00 expecting green would get you green here
// by luck and blue somewhere else, so the order is spelled out once, in a macro, and the host
// test pulls the channels back out of it. gfx/sprite.h's SPRITE_RGBA is the same order; this
// macro is separate only because this file must not acquire a gfx/ include.
#define BB_RGBA(r, g, b, a) \
	((uint32_t)((uint8_t)(r)) | ((uint32_t)((uint8_t)(g)) << 8) | \
	 ((uint32_t)((uint8_t)(b)) << 16) | ((uint32_t)((uint8_t)(a)) << 24))

// The colour a segment is drawn in: the neon of the LOWER of its two BiomeIds.
//
// A quad has exactly one constant colour on this hardware — the fragment side is
// GPU_CONSTANT/GPU_REPLACE through one TEV stage, as scene/highlight.c does it, because the
// PICA200 has no programmable fragment shader to blend two. So an edge must pick ONE of the
// two biomes it divides. min(lo, hi) is arbitrary but it is TOTAL and STABLE: the same seam
// is the same colour from either side and from either direction of travel, which a "colour it
// by whichever side the player is on" rule would not be. Picking by the pair instead would
// need 15 colours nobody can tell apart.
//
// Six colours means at most six C3D_DrawArrays calls for the whole overlay, which is why the
// draw half emits one contiguous run of vertices per colour and never one call per segment.
uint32_t biomeBorderColour(const BiomeBorderSeg* s);

// Which of those runs a segment belongs to: min(lo, hi), i.e. a BiomeId used as a bucket
// index. -1 for a NULL segment. biomeBorderVerts() filters on this.
int biomeBorderColourKey(const BiomeBorderSeg* s);

// How many of `segs` fall in bucket `key`. `key` < 0 counts them all.
int biomeBorderCountFor(const BiomeBorderSeg* segs, int n, int key);

// The neon for one BiomeId, in BB_RGBA order. Out-of-range gives white, so a biome added to
// world/worldgen.h without a colour added here draws visibly wrong rather than reading off
// the end of the table.
//
// ONE unsigned comparison and not `(int)b < 0 || (int)b >= BIOME_COUNT`, for the reason
// debug/biomeinfo.c spells out at length: devkitARM's EABI defaults to -fshort-enums, BiomeId
// is a single byte on the console, and the signed form compiles clean on the host then FAILS
// the console build under -Werror=type-limits. Measured there, not reasoned about here.
uint32_t biomeBorderBiomeColour(BiomeId b);

// ── Building ─────────────────────────────────────────────────────────────────────────

// Walks the (2*radius+1)^2 columns centred on (px, pz) and writes one BiomeBorderSeg for
// every edge where worldgenBiomeAt() differs across it. Returns the count written.
//
// THE CONTRACT, exactly, because biomeborder_test.c sweeps against it: for every column
// (x, z) with x in [px-radius, px+radius] and z in [pz-radius, pz+radius],
//   * an axis-0 segment iff worldgenBiomeAt(x, z) != worldgenBiomeAt(x+1, z), and
//   * an axis-1 segment iff worldgenBiomeAt(x, z) != worldgenBiomeAt(x, z+1),
// and nothing else. Both axes cover the same rectangle, so a neighbour one step outside it
// is CLASSIFIED but never emitted for. Order is z-major then x then axis-0 before axis-1.
//
// `radius` is clamped to 0..BB_MAX_RADIUS. A NULL generator, a NULL out or a cap below 1
// writes nothing and returns 0 — an unwired overlay draws no fence, and never a fence in a
// guessed-at place.
//
// Stops at `cap`. biomeBorderTruncated() reports whether the last build hit it.
int biomeBorderBuild(const WorldGen* g, int32_t px, int32_t pz, int radius,
                     BiomeBorderSeg* out, int cap);

// Whether the last biomeBorderBuild() ran out of room. Sticky per build, not per process.
bool biomeBorderTruncated(void);

// Turns the segments in bucket `key` into the vertices the GPU draws, in world block units —
// 6 vertices of 3 floats each per segment, so 18 floats per segment, in `segs` order. `key`
// < 0 takes them all. Returns the number of FLOATS written, or 0 if `cap_floats` cannot hold
// all of the ones it selected (never a partial fence: a wall missing its second triangle is a
// hole in the overlay that reads as a gap in the biome map, which is exactly the lie this
// feature must not tell).
//
// Filtering here rather than sorting the segment list is what lets the draw half issue one
// C3D_DrawArrays per colour off a single vertex buffer without a second BB_MAX_SEGS array to
// sort into: it calls this once per bucket and the runs come out already contiguous. Six
// passes over at most 512 one-byte comparisons is not a cost worth 6 KB of .bss to avoid.
//
// The heights come from worldgenHeight() on BOTH columns: the wall spans from
// min(hLo, hHi) - BB_WALL_DOWN up to max(hLo, hHi) + BB_WALL_UP, so it is planted in the
// ground on the low side and clears the terrain on the high side even across a cliff.
int biomeBorderVerts(const WorldGen* g, const BiomeBorderSeg* segs, int n, int key,
                     float* out, int cap_floats);

// ── The toggle ───────────────────────────────────────────────────────────────────────
//
// Here rather than in the draw half so that "does it default to off" is a question the host
// suite can answer. main.c's DebugEntry getBool/setBool point straight at these two.

void biomeBorderSetEnabled(bool on);
bool biomeBorderEnabled(void);      // false until something sets it; see the default note

// Forget everything, including the toggle. Called on world teardown so a fence built for the
// last world cannot survive into the next one.
void biomeBorderReset(void);

// The live world, or NULL to forget it. A POINTER — see the header comment above.
void biomeBorderSetWorldGen(const WorldGen* g);
const WorldGen* biomeBorderWorldGen(void);
