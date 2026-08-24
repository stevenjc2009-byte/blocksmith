// Atlas geometry: where a tile sits on the sheet, in texture space.
//
// This is the one definition of that arithmetic. It lives here, free of <3ds.h>, so
// the host-testable mesher and the console's gfx/atlas.c can share it — duplicating
// it would be values drifting silently, which no static assert can catch.
#pragma once

#include <stdint.h>

// The sheet is a ONE-TILE-WIDE STRIP: 16 px across, 1024 px tall, 64 slots of 16x16
// stacked vertically with no padding between them. It was 16x256 (16 slots) from v1.6.0
// to v1.8.1, and before that, up to v1.5.1, a 128x128 sheet holding a 6x6 grid of 20x20
// cells (16px of art inside a 2px edge-extended border).
//
// 1024 is the hardware ceiling, not a round number picked for comfort. citro3d's
// C3D_TexInitWithParams rejects any dimension outside 8..1024 inclusive and any dimension
// that is not a power of two — checkTexSize, disassembled out of the installed
// citro3d 1.7.1: `sub r3, width, #8 / cmp r3, #1016 / bhi fail`, then `sub r2, w, #1 /
// tst r2, w / bne fail`, and the same two checks again for height. The two dimensions are
// checked INDEPENDENTLY: there is no square requirement and no aspect-ratio check, which
// is what makes a 64:1 strip legal.
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
#define ATLAS_H_PX  1024
#define TILE_PX     16

// Slots that physically fit, and slots that can actually be addressed. Since task 13b
// (v1.8.2) those are the SAME NUMBER: every slot on the sheet is addressable.
//
// The history, because it is the whole reason this file is shaped the way it is. Until
// v1.8.1, vslot0/vslot1 below held a raw atlas PIXEL offset (tile * TILE_PX). MeshVertex.v
// is a uint8_t, so a tile's top edge had to fit in 255, and 16 slots' worth of pixels is
// where that byte ran out — slot 15 needed v1 = 256 and was unaddressable, giving a hard
// ceiling of 15 slots at ANY sheet height. Widening ATLAS_H_PX did not help: at 512 the
// same overflow simply arrived at the same slot, and every tile above it silently aliased
// an earlier one's rect (tile 30, for instance, truncated onto tile 14's own v1) — which
// renders *a* texture and so presents as bad art, never as an error.
//
// Task 13b fixed the field's UNITS rather than its width. vslot0/vslot1 now hold a
// SLOT-EDGE INDEX — `tile` and `tile + 1` — and the shader's uvScale.y carries the
// TILE_PX factor that used to be baked into the byte. The byte was never too narrow: it
// holds 0..255 and the largest edge index the biggest legal sheet can produce is 64. So
// the vertex struct did not change, the stride is still 8 bytes, the attribute
// configuration is untouched, and the whole change cost zero bytes per vertex.
//
// The limiter is now the HARDWARE's 1024 px maximum texture dimension (see the note at
// the top of this file), which at 16 px a slot is 64 slots. u0/u1 are still PIXELS —
// that is deliberate, see ATLAS_MAX_MERGE_BLOCKS below — so this struct carries two units
// at once, which is why the v fields are named vslot* and not v*: a caller that still
// assumes pixels fails to COMPILE rather than failing to look right.
#define ATLAS_TILE_SLOTS   (ATLAS_H_PX / TILE_PX)   // 64 slots exist on the sheet
#define ATLAS_TILE_COUNT   (ATLAS_TILE_SLOTS)       // and all 64 are addressable: 0..63

// The vertex byte still has to hold every slot EDGE, which is one past the last slot.
// mesh_vertex.h's _Static_assert(sizeof(MeshVertex) == 8) is blind to this — a units
// change keeps the struct exactly 8 bytes — so this is the only machine check that the
// ceiling above is real. It fires the moment ATLAS_H_PX grows past what the byte can
// address, instead of letting the wrap come back as bad art the way it did before v1.8.2.
_Static_assert(ATLAS_TILE_SLOTS <= 255,
               "MeshVertex.v is a uint8_t holding a slot-edge index (tile+1), so the sheet "
               "may hold at most 255 slots; ATLAS_H_PX is too tall");

// The missing-texture marker, and the slot permanently reserved for it (v1.6.0 F7).
//
// A texture that does not exist did not LOOK like it did not exist. Two ways in:
//
//   * an out-of-range tile id clamped to 0 and drew grass, so a block declared with a wrong
//     tex byte rendered as a perfectly plausible grass block;
//   * the slots above the painted ones are addressable but were never painted, so a tex in
//     that range drew the sheet's background fill — an opaque near-black solid that reads as
//     "a dark block", not as an error.
//
// Both matter because the REGISTRY lets a server define block types over the wire
// (world/registry.c), so a wrong or unsupported tex byte is a SERVER misconfiguration that
// presented as a Blocksmith rendering bug. This project has lost days to exactly that
// confusion before: a wrong texture constant still renders *a* texture, so the failure never
// arrives as an error.
//
// tools/make_atlas.py paints EVERY slot the TILES list does not fill with a magenta/black
// quadrant checker, so no slot is ever background fill again. Since task 13b that is 52 of
// the 64 slots rather than 6 of 16. Those spares are not wasted: the marker is the DEFAULT
// content of an unclaimed slot and each one is overwritten by real art as TILES grows.
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
#define ATLAS_TILE_MISSING (ATLAS_TILE_COUNT - 1)   // slot 63, reserved for the marker

// The ceiling greedy meshing has to respect, stated here because this is where the u8
// comes from. This one did NOT move in task 13b: u is still the only pixel-unit field in
// AtlasRect, so its byte still runs out where it always did. Putting u into tile units too
// would raise this cap to at least CHUNK_DIM, and was considered and rejected as a
// performance change riding on a correctness change — it is not part of task 13b.
//
// A merged quad `width` blocks long emits u1 = TILE_PX * width, and that has to fit in
// MeshVertex.u's uint8_t, so:
//
//     16 * 15 = 240   fits
//     16 * 16 = 256   does NOT — it wraps to 0 and the quad samples a zero-width tile
//
// So a U-direction merge run may be at most ATLAS_MAX_MERGE_BLOCKS blocks long. The
// mesher must split a longer run into several quads; it cannot rely on the wrap being
// harmless, because u=0 is a valid coordinate and the face would draw *a* texture.
#define ATLAS_MAX_MERGE_BLOCKS  15

// A tile's extent in **texture space** — v grows upwards, the opposite of the PNG's rows,
// because that is how the hardware samples. So u0/vslot0 is the art's bottom-left corner
// and u1/vslot1 its top-right.
//
// THE TWO FIELDS ARE IN DIFFERENT UNITS AND THAT IS DELIBERATE:
//
//   u0, u1          atlas PIXELS across  (0 and 16, widened to TILE_PX*width by a merge)
//   vslot0, vslot1  SLOT-EDGE INDICES up (tile and tile+1 — NOT pixels)
//
// The v fields carry the awkward name precisely so this cannot be forgotten. Before task
// 13b they were `v0`/`v1` and held pixels; renaming them means every caller that still
// scales by ATLAS_H_PX alone fails to COMPILE. That mattered: scene/ui.c's iconUv() was
// exactly such a caller, and left unfixed it would have drawn every hotbar icon as grass
// with no error anywhere — the failure mode this project keeps paying for.
//
// The shader carries the missing TILE_PX: uvScale.y is TILE_PX/ATLAS_H_PX where it used
// to be 1/ATLAS_H_PX, so `mul r2, uvScale, inpack` produces the same texture coordinate
// from a smaller number. Both .v.pica files and this header are one layout in three
// places; world/atlas_uv_shader_test.c is what keeps them agreeing.
//
// The rect is half-open in slots: it covers texel rows vslot0*TILE_PX .. vslot1*TILE_PX-1,
// so adjacent slots share the edge index vslot1/vslot0 without sharing a pixel. All four
// fields fit in a u8 — u tops out at 240 and a slot edge at ATLAS_TILE_SLOTS, asserted
// above.
typedef struct {
	uint8_t u0, vslot0, u1, vslot1;
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
	//
	// v is a SLOT-EDGE INDEX since task 13b, not a pixel offset: the TILE_PX factor moved
	// into the shader's uvScale.y. That is what lifted the ceiling from 15 slots to 64
	// without touching the vertex struct, the stride or the attribute configuration.
	AtlasRect r;
	r.u0     = 0;
	r.u1     = (uint8_t)TILE_PX;
	r.vslot0 = (uint8_t)tile;
	r.vslot1 = (uint8_t)(tile + 1);
	return r;
}
