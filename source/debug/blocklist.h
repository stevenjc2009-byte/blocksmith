// v1.8.8 debug feature: the debug menu's BLOCK LIST — one small icon and one name for
// every block that actually exists, on the BOTTOM screen.
//
// ── Registry-driven, never a hand-maintained list ─────────────────────────────────────
//
// The whole point of this screen is that it cannot rot. It enumerates world/registry.h's
// table directly — registryCount() for how many rows are defined, registryIsDefined() for
// which ids are real, registryGet()->name for the label and blockFaceTex() for the art —
// so a block added to world/registry.c, or one registered over the wire at join time by a
// server, appears here on the next open with no edit to this file. There is deliberately
// no BlockId list, no name table and no count constant anywhere in this module: every one
// of those would be a second source of truth that a later block would silently fall out
// of, which is exactly the failure this screen exists to make impossible.
//
// That includes the DYNAMIC id space (REG_ID_DYN_LO..REG_ID_DYN_HI, registry.h:30-31).
// The enumeration walks defined ids ascending across the whole 0..255 range rather than
// stopping at the core rows, so a server's blocks list beside the compiled-in ones.
//
// ── Why this file is pure, and what that buys ────────────────────────────────────────
//
// No <3ds.h>, no <citro3d.h>, no gfx/*.h. Same split as scene/ui_layout.h against
// scene/ui.c and as the input half of app/debugmenu_ui.c against its draw half, and for
// the same reason those exist: the part with the bugs in it is the layout and the paging,
// and neither needs a GPU to be wrong.
//
// It goes further than ui_layout.h, though, and the difference is the point. This module
// does not merely compute rectangles for someone else to draw — it emits the COMPLETE
// DRAW PROGRAM for a page as a list of BlockListOp, and app/debugmenu_ui.c's console half
// is a dumb loop that turns each op into one spriteRect / spriteQuad / fontDraw call and
// decides nothing.
//
// That shape was chosen over "return the rects, let the drawing code lay out the rest"
// because of a failure this project has already shipped once: a compiling, test-passing
// diff that rendered garbage. A test that re-derives what it thinks the screen draws
// proves only that two pieces of arithmetic agree. Here the host test consumes the very
// ops the console submits, so counting quads, checking that every registered block is on
// exactly one page, and rasterising the screen to a bitmap to LOOK at it are all done
// against the real program and not a mirror of it.
//
// The atlas UVs are part of that program and are computed HERE, from world/atlas_uv.h's
// atlasRect() — the same pure static inline gfx/atlas.c's atlasTile() is a one-line
// wrapper around (gfx/atlas.c:68-71). On this hardware a wrong texture constant still
// renders *a* texture, so it reads as bad art rather than as an error; computing the UVs
// somewhere a host test can assert them is what turns that silent class of mistake into a
// failing check.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/atlas_uv.h"   // AtlasRect, atlasRect(), ATLAS_*_PX, TILE_PX — all pure
#include "world/block.h"      // BlockId, blockFaceTex(), FACE_TOP

// ── Layout, in bottom-screen pixels ──────────────────────────────────────────────────
//
// The bottom screen is 320x240 (scene/ui_layout.h SCR_W/SCR_H) and this screen owns all
// of it while it is open. Two columns rather than a denser icon grid because the ask was
// icons AND names: REGISTRY_NAME_MAX is 16, so a name is up to 15 characters at
// FONT_ADVANCE 6 = 90 px, and a 3-column split (100 px a cell) could not hold one.
#define BL_SCR_W   320
#define BL_SCR_H   240

#define BL_COLS    2
#define BL_ROWS    8
#define BL_PER_PAGE (BL_COLS * BL_ROWS)   // 16

#define BL_MARGIN  4
#define BL_CELL_W  154                     // 4 + 154 + 4 + 154 + 4 = 320 exactly
#define BL_CELL_H  24                      // 8 rows = 192 px of grid
#define BL_GRID_Y  26
#define BL_HEAD_Y  8
#define BL_FOOT_Y  224

#define BL_ICON_PX  16                     // the "small icon"
#define BL_ICON_DX  3                      // inset from the cell's left edge
#define BL_ICON_DY  4                      // inset from the cell's top edge
#define BL_TEXT_DX  (BL_ICON_DX + BL_ICON_PX + 4)   // 23 — clear of the icon
#define BL_TEXT_DY  8                      // (24 - 7) / 2 rounded down: FONT_GLYPH_H is 7

// Ops for one page: one background rect, a header line, a footer line, and three ops per
// cell (cell plate, icon, label). 3 + 16*3 = 51.
#define BL_MAX_OPS 64

typedef enum {
	BL_OP_RECT = 0,   // spriteRect: a solid fill in the bound font texture's white texel
	BL_OP_CELL,       // spriteRect too, but it is one listed block's plate and carries `id`
	BL_OP_ICON,       // spriteQuad from the BLOCK ATLAS texture — needs the atlas bound
	BL_OP_TEXT,       // fontDraw at (x, y), scale 1
} BlockListOpKind;

// BL_OP_CELL is drawn exactly like BL_OP_RECT and exists only so that "which blocks does
// this page list" is answerable from the op stream itself.
//
// It cannot be answered from the icon ops, because NOT EVERY LISTED BLOCK HAS AN ICON.
// Air is id 0 and is a defined registry row, so it belongs on a debug listing of the
// table — but its tex[] bytes are all 0, and tile 0 is grass, so drawing air's icon draws
// a grass block. On this hardware that is invisible as an error: the quad renders, the
// screen looks populated, and the only symptom is one row of art that is quietly a lie.
// So air gets its plate and its label and no icon quad, and the enumeration key moved off
// the icon onto the plate rather than the listing losing a row.

// One drawing instruction. Deliberately flat and POD: app/debugmenu_ui.c switches on
// `kind` and passes the fields straight through, so there is no place for a decision to
// hide between this module and the screen.
typedef struct {
	uint8_t  kind;                 // a BlockListOpKind
	BlockId  id;                   // BL_OP_ICON only: which block this icon is
	float    x, y, w, h;           // RECT/ICON: the rect. TEXT: x,y top-left; w,h are 0.
	float    u0, v0, u1, v1;       // BL_OP_ICON only; 0 otherwise
	uint32_t colour;               // SPRITE_RGBA byte order: r | g<<8 | b<<16 | a<<24
	const char* text;              // BL_OP_TEXT only. Static storage — safe to hold.
} BlockListOp;

// How many blocks the screen will show: every id in 0..255 that registryIsDefined().
// Counted by walking the table, NOT read from registryCount(), because the two answer
// subtly different questions once the dynamic range is in use and this screen must show
// what is actually there.
int blockListCount(void);

// Pages needed to show them all. Never less than 1, so an empty registry still draws a
// page that says so rather than drawing nothing at all.
int blockListPages(void);

// Wraps `page` into 0..blockListPages()-1. Negative wraps to the last page, so paging
// backwards off the front lands on the end.
int blockListClampPage(int page);

// The block on `page` at `slot` (0..BL_PER_PAGE-1), or 0 when that cell is empty. Air is
// id 0 and IS a defined row, so callers must use the returned op list rather than testing
// this against 0 to decide whether a cell exists; it is exposed for the tests.
BlockId blockListAt(int page, int slot);

// Builds the complete draw program for `page` into `out`, returning the number of ops
// written, or 0 if `cap` is smaller than the program needs (never a partial program — a
// half-drawn screen is worse than a caller that can see it failed).
int blockListBuild(int page, BlockListOp* out, int cap);

// Printable, non-space characters in `s` — i.e. exactly how many quads gfx/font.c's
// fontDraw emits for it. font.c skips ' ' and anything outside 32..126 and still advances
// the pen (gfx/font.c:89-123), so this is a count of drawn glyphs, not of bytes.
int blockListTextQuads(const char* s);

// Exact number of sprite quads `ops` costs when drawn. Walks the same op list the console
// half walks, so it is a measurement of the real program rather than an estimate of it —
// which is what makes the number safe to quote against gfx/sprite.h's SPRITE_MAX_QUADS.
int blockListQuadsFor(const BlockListOp* ops, int n);

// ── The screen's own state machine ────────────────────────────────────────────────────
//
// Open/closed and which page, kept HERE rather than in app/debugmenu_ui.c, for one
// concrete reason: app/debugmenu_test.c compiles debugmenu_ui.c as SOURCE on the host
// (see its `#include "app/debugmenu_ui.c"`), and tools/run_host_tests.sh builds that
// binary from options.c + hw.c + render_dist.c + debugmenu.c only. Putting this state in
// debugmenu_ui.c would drag the registry and the block table into that link, changing a
// stanza this change does not own. Behind this seam, debugmenu_ui.c's use of the block
// list sits entirely inside its `#ifdef __3DS__` half and that binary links exactly what
// it linked before — while the logic itself is still host-tested, right here.
//
// Abstract key bits rather than libctru's KEY_*: this file has no <3ds.h> and must not
// acquire one. app/debugmenu_ui.c does the mapping on the console side.
enum {
	BL_KEY_CLOSE = 1u << 0,   // leave the block list, back to the debug menu
	BL_KEY_PREV  = 1u << 1,   // previous page (wraps)
	BL_KEY_NEXT  = 1u << 2,   // next page (wraps)
};

void blockListUiReset(void);      // closed, on page 0
void blockListUiOpen(void);       // open, on page 0
bool blockListUiIsOpen(void);
int  blockListUiPage(void);

// One frame of input. Returns true while the screen should still be drawn, false when the
// player asked to leave — at which point it is already closed.
bool blockListUiInput(uint32_t keys);
