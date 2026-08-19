// Atlas geometry: where a tile sits on the sheet, in texture space.
//
// This is the one definition of that arithmetic. It lives here, free of <3ds.h>, so
// the host-testable mesher and the console's gfx/atlas.c can share it — duplicating
// it would be values drifting silently, which no static assert can catch.
#pragma once

#include <stdint.h>

// Step 9.3c: sized to exactly the 9 tiles atlas.h enumerates (3x3 = 9 cells, zero
// spare). Was 256 (12x12 = 144 cells, 135 of them unused). The next appended tile
// needs the next power-of-two sheet — 128, a 6x6 = 36-cell grid — there is no
// room left here. Must stay a power of two: the PICA200 requires it.
#define ATLAS_PX    64
#define TILE_PX     16
#define ATLAS_PAD   2
#define ATLAS_CELL  (TILE_PX + ATLAS_PAD * 2)   // 20
#define ATLAS_GRID  (ATLAS_PX / ATLAS_CELL)     // 3

// A tile's extent in atlas pixels, in **texture space** — v grows upwards, the
// opposite of the PNG's rows, because that is how the hardware samples. So u0/v0
// is the art's bottom-left corner and u1/v1 its top-right. All four fit in a u8:
// the largest value any tile can produce is 254.
typedef struct {
	uint8_t u0, v0, u1, v1;
} AtlasRect;

static inline AtlasRect atlasRect(int tile)
{
	const int col = tile % ATLAS_GRID;
	const int row = tile / ATLAS_GRID;
	const int x0  = col * ATLAS_CELL + ATLAS_PAD;
	const int y0  = row * ATLAS_CELL + ATLAS_PAD;

	// Texture space runs bottom-up while the PNG runs top-down, so the tile's row
	// has to be flipped: v0 is the art's *bottom* edge, v1 its top.
	AtlasRect r;
	r.u0 = (uint8_t)x0;
	r.u1 = (uint8_t)(x0 + TILE_PX);
	r.v0 = (uint8_t)(ATLAS_PX - (y0 + TILE_PX));
	r.v1 = (uint8_t)(ATLAS_PX - y0);
	return r;
}
