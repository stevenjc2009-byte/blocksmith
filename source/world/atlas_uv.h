// Atlas geometry: where a tile sits on the sheet, in texture space.
//
// This is the one definition of that arithmetic. It lives here, free of <3ds.h>, so
// the host-testable mesher and the console's gfx/atlas.c can share it — duplicating
// it would be values drifting silently, which no static assert can catch.
#pragma once

#include <stdint.h>

// The sheet is a ONE-TILE-WIDE STRIP: 16 px across, 256 px tall, 16 slots of 16x16
// stacked vertically with no padding between them. Up to v1.5.1 it was a 128x128 sheet
// holding a 6x6 grid of 20x20 cells (16px of art inside a 2px edge-extended border).
//
// The strip exists for greedy meshing (v1.6.0 task 11). Merging N co-planar faces into
// one quad needs the tile to REPEAT N times across it, and on a packed grid it cannot:
// stone's tile sat at u 62..78, so a two-wide merge reached u 86, which was sand.
// GPU_REPEAT did not fix it either — the repeat period was the whole 128px sheet, eight
// tiles wide. With the sheet exactly one tile across, GPU_REPEAT in U has a period of
// exactly one tile, so u may run as far as a merge needs and every block of the run
// shows a complete, correct tile. gfx/atlas.c therefore sets GPU_REPEAT in U and
// GPU_CLAMP_TO_EDGE in V. Merging stays U-ONLY: extending v walks into the next slot,
// and that limit is accepted.
//
// The 2px border went with the grid. In U there is no neighbour left to bleed in —
// REPEAT wraps the tile onto itself. In V the slots are directly adjacent, which would
// matter if the texture had a mip chain, since a mip level averages across the boundary.
// It has none: gfx/atlas.t3s passes tex3ds only "-f rgba5551 -z auto" with no -m, the
// built build/atlas.t3x carries mipmapLevels = 0 in its header (measured), and
// gfx/atlas.c asks for GPU_NEAREST on both min and mag, which is not a mipmapped filter.
//
// These two numbers, ATLAS_W_PX/ATLAS_H_PX in tools/make_atlas.py, and the uvScale
// constant in BOTH source/shaders/world.v.pica and source/shaders/world_dynamic.v.pica
// are one layout in four places, and nothing the compiler can see guards it — the PNG
// carries no dimensions the C side reads. If they disagree, atlasRect() addresses the
// wrong rows and every face in the game is textured with a slice of its neighbours.
// A wrong constant here still renders *a* texture, so it presents as bad art and never
// as an error. source/world/atlas_uv_shader_test.c is what makes the duplication safe.
// Both must stay powers of two: the PICA200 requires it.
#define ATLAS_W_PX  16
#define ATLAS_H_PX  256
#define TILE_PX     16

// Slots that physically fit, and slots that can actually be addressed.
//
// MeshVertex.u/v are uint8_t and that format is locked (source/world/mesh_vertex.h), so
// a tile's TOP edge v1 has to fit in 255. The topmost slot in texture space would need
// v1 = 256, which does not, so slot 15 is unaddressable and atlasRect() clamps into
// range rather than returning a wrapped rect. TILE_* in gfx/atlas.h enumerates 10, so 5
// addressable slots are spare — and since v1.6.0 F7 every one of them, slot 15 included,
// is painted with the missing-texture marker instead of left as background fill. See
// ATLAS_TILE_MISSING below. The next step up is ATLAS_H_PX 512: 32 slots, 31
// addressable, 16 KB of VRAM instead of 8 KB.
#define ATLAS_TILE_SLOTS   (ATLAS_H_PX / TILE_PX)   // 16 slots exist on the sheet
#define ATLAS_TILE_COUNT   (ATLAS_TILE_SLOTS - 1)   // 15 of them addressable: indices 0..14

// The missing-texture marker, and the slot permanently reserved for it (v1.6.0 F7).
//
// A texture that does not exist did not LOOK like it did not exist. Two ways in:
//
//   * an out-of-range tile id clamped to 0 and drew grass, so a block declared with a wrong
//     tex byte rendered as a perfectly plausible grass block;
//   * slots 10..14 are addressable but were never painted, so a tex in that range drew the
//     sheet's background fill — an opaque near-black solid that reads as "a dark block",
//     not as an error.
//
// Both matter because the REGISTRY lets a server define block types over the wire
// (world/registry.c), so a wrong or unsupported tex byte is a SERVER misconfiguration that
// presented as a Blocksmith rendering bug. This project has lost days to exactly that
// confusion before: a wrong texture constant still renders *a* texture, so the failure never
// arrives as an error.
//
// tools/make_atlas.py now paints EVERY slot the TILES list does not fill — 10..14, plus the
// unaddressable 15 — with a magenta/black quadrant checker, so no addressable slot is ever
// background fill again. Those spares are not wasted: the marker is the DEFAULT content of an
// unclaimed slot and each one is overwritten by real art as TILES grows.
//
// ATLAS_TILE_MISSING is the one slot that stays a marker forever, and it is the TOP of the
// addressable range on purpose: it is the last slot a growing TILES list would ever reach, so
// reserving it costs nothing until the sheet is full anyway. tools/make_atlas.py refuses to
// paint art into it, which is what keeps this constant true rather than merely intended.
//
// Deliberately NOT TILE_SENTINEL (slot 5). That tile is a 2px magenta/black checker and it is
// a BLEED alarm — it exists to be sampled by accident. Clamping here would have made one
// appearance mean either "UVs bled" or "this texture does not exist", which is two bugs
// wearing one face. The marker is the same two colours at 8px quadrants instead: unmistakable
// as an error at 16x16 on a 240px screen, and unmistakably not the sentinel, because a 2px
// checker blurs to flat pink at any distance while four half-tile quadrants stay four blocks.
#define ATLAS_TILE_MISSING (ATLAS_TILE_COUNT - 1)   // slot 14, reserved for the marker

// The ceiling greedy meshing has to respect, stated here because this is where the u8
// comes from. A merged quad `width` blocks long emits u1 = TILE_PX * width, and that has
// to fit in MeshVertex.u's uint8_t, so:
//
//     16 * 15 = 240   fits
//     16 * 16 = 256   does NOT — it wraps to 0 and the quad samples a zero-width tile
//
// So a U-direction merge run may be at most ATLAS_MAX_MERGE_BLOCKS blocks long. The
// mesher must split a longer run into several quads; it cannot rely on the wrap being
// harmless, because u=0 is a valid coordinate and the face would draw *a* texture.
#define ATLAS_MAX_MERGE_BLOCKS  15

// A tile's extent in atlas pixels, in **texture space** — v grows upwards, the
// opposite of the PNG's rows, because that is how the hardware samples. So u0/v0
// is the art's bottom-left corner and u1/v1 its top-right. The rect is half-open in
// texels: it covers texel rows v0 .. v1-1, so adjacent slots share the coordinate v1/v0
// without sharing a pixel. All four fit in a u8: the largest value any addressable tile
// can produce is 240.
typedef struct {
	uint8_t u0, v0, u1, v1;
} AtlasRect;

static inline AtlasRect atlasRect(int tile)
{
	// world/mesher.c builds its rect table for all 256 registry ids, and a dynamic block
	// registered over the wire can carry any tile byte at all, so out-of-range must land
	// somewhere defined rather than wrapping the u8 into a nonsense rect.
	//
	// It lands on the MISSING-TEXTURE MARKER, not on tile 0 (v1.6.0 F7). Clamping to 0 drew
	// grass, so a server shipping a wrong or unsupported tex byte produced a plausible grass
	// block and read as a Blocksmith rendering bug rather than a server misconfiguration.
	// Still one comparison and one assignment: no divide and no modulo, which is the whole
	// reason this function is shaped the way it is.
	if ((unsigned)tile >= (unsigned)ATLAS_TILE_COUNT)
		tile = ATLAS_TILE_MISSING;

	// One tile wide, so every tile shares the same u span. GPU_REPEAT makes that span
	// the repeat period: u0 + TILE_PX*k lands back on the same tile for every k.
	// Note there is no divide or modulo left in here — the grid layout needed both, and
	// the ARM11 has no divide instruction (see the note in world/mesher.c).
	AtlasRect r;
	r.u0 = 0;
	r.u1 = (uint8_t)TILE_PX;
	r.v0 = (uint8_t)(tile * TILE_PX);
	r.v1 = (uint8_t)(tile * TILE_PX + TILE_PX);
	return r;
}
