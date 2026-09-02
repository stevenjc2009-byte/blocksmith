#include "debug/blocklist.h"

#include <stdio.h>

#include "world/registry.h"

// Colours, in gfx/sprite.h's SPRITE_RGBA byte order (r | g<<8 | b<<16 | a<<24). Taken from
// app/debugmenu_ui.c's palette so the block list reads as the same screen the player just
// came from rather than as a different program: COL_PANEL, COL_EDGE, COL_TEXT, COL_HEAD.
#define BLC_PANEL 0xF0201A14u
#define BLC_CELL  0xFF2E241Cu
#define BLC_TEXT  0xFFE8DCC8u
#define BLC_HEAD  0xFFB4E0A0u
#define BLC_DIM   0xFF9A8B76u

// The header line's text. Static rather than a caller-provided buffer because
// BlockListOp.text is documented as safe to hold: the console half draws the op list after
// blockListBuild has returned, so a stack buffer here would be a dangling pointer that
// happened to work.
// 64, not 40. gcc's -Wformat-truncation reasons about the widest an int can print (11
// characters), not about the real bounds here — three of them plus the 28-character
// literal is 61 — so a 40-byte buffer failed the build under -Werror even though the
// actual worst case is "BLOCK LIST  256 blocks  page 16/16", 34 characters.
static char s_head[64];

// The footer is a literal today, but it goes through the same op list as everything else
// so that blockListQuadsFor counts it without a special case.
#define BL_FOOT_TEXT "B back   L/R or D-pad: page"

int blockListCount(void)
{
	int n = 0;
	for (int id = 0; id <= 0xFF; id++)
		if (registryIsDefined((BlockId)id)) n++;
	return n;
}

int blockListPages(void)
{
	const int n = blockListCount();
	if (n <= 0) return 1;
	return (n + BL_PER_PAGE - 1) / BL_PER_PAGE;
}

int blockListClampPage(int page)
{
	const int pages = blockListPages();
	// Positive modulo: C's % keeps the sign of the dividend, so -1 % 3 is -1, not 2, and
	// paging left off the first page would index out of the table instead of wrapping to
	// the last one. Written as two steps rather than one clever expression because this is
	// the kind of arithmetic that is easier to test than to read.
	int p = page % pages;
	if (p < 0) p += pages;
	return p;
}

// The `slot`-th defined id on `page`, or 0 when the page runs out. `found` distinguishes
// "cell is empty" from "cell holds air", which blockListAt's own return value cannot.
static BlockId nthDefined(int index, bool* found)
{
	int seen = 0;
	for (int id = 0; id <= 0xFF; id++) {
		if (!registryIsDefined((BlockId)id)) continue;
		if (seen == index) {
			if (found) *found = true;
			return (BlockId)id;
		}
		seen++;
	}
	if (found) *found = false;
	return 0;
}

BlockId blockListAt(int page, int slot)
{
	if (slot < 0 || slot >= BL_PER_PAGE) return 0;
	return nthDefined(blockListClampPage(page) * BL_PER_PAGE + slot, NULL);
}

int blockListTextQuads(const char* s)
{
	int n = 0;
	if (!s) return 0;
	// Mirrors gfx/font.c:96-98 exactly: a quad is emitted only for a character that is not
	// a space and lies inside 32..126. '\n' is outside that range and so is skipped here
	// too, which is correct — it moves the pen and draws nothing.
	for (const unsigned char* p = (const unsigned char*)s; *p; p++)
		if (*p != ' ' && *p >= 32 && *p <= 126) n++;
	return n;
}

int blockListQuadsFor(const BlockListOp* ops, int n)
{
	int q = 0;
	if (!ops) return 0;
	for (int i = 0; i < n; i++) {
		switch (ops[i].kind) {
		case BL_OP_RECT:
		case BL_OP_CELL:
		case BL_OP_ICON:
			q += 1;                                   // one quad each
			break;
		case BL_OP_TEXT:
			q += blockListTextQuads(ops[i].text);     // one per drawn glyph
			break;
		default:
			break;
		}
	}
	return q;
}

// The icon's UVs, computed the same way scene/ui.c's iconUv does (scene/ui.c:182-189) and
// for the same reasons documented there: FACE_TOP for every block so the choice is
// consistent, and v0/v1 taken from vslot1/vslot0 so the quad's TOP edge is the art's top
// row. atlasRect() is the pure static inline in world/atlas_uv.h that gfx/atlas.c's
// atlasTile() simply forwards to (gfx/atlas.c:68-71), so this is the same lookup the
// inventory's icons already go through — not a second implementation of it.
static void iconUvFor(BlockId id, float* u0, float* v0, float* u1, float* v1)
{
	const AtlasRect r = atlasRect(blockFaceTex(id, FACE_TOP));
	*u0 = (float)r.u0 / (float)ATLAS_W_PX;
	*v0 = (float)(r.vslot1 * TILE_PX) / (float)ATLAS_H_PX;
	*u1 = (float)r.u1 / (float)ATLAS_W_PX;
	*v1 = (float)(r.vslot0 * TILE_PX) / (float)ATLAS_H_PX;
}

static void pushRect(BlockListOp* o, float x, float y, float w, float h, uint32_t c)
{
	o->kind = BL_OP_RECT;
	o->id = 0;
	o->x = x; o->y = y; o->w = w; o->h = h;
	o->u0 = o->v0 = o->u1 = o->v1 = 0.0f;
	o->colour = c;
	o->text = NULL;
}

static void pushText(BlockListOp* o, float x, float y, uint32_t c, const char* t)
{
	o->kind = BL_OP_TEXT;
	o->id = 0;
	o->x = x; o->y = y; o->w = 0.0f; o->h = 0.0f;
	o->u0 = o->v0 = o->u1 = o->v1 = 0.0f;
	o->colour = c;
	o->text = t;
}

static void pushIcon(BlockListOp* o, float x, float y, BlockId id)
{
	o->kind = BL_OP_ICON;
	o->id = id;
	o->x = x; o->y = y; o->w = (float)BL_ICON_PX; o->h = (float)BL_ICON_PX;
	iconUvFor(id, &o->u0, &o->v0, &o->u1, &o->v1);
	o->colour = 0xFFFFFFFFu;   // SPRITE_WHITE: the atlas art supplies the colour
	o->text = NULL;
}

int blockListBuild(int page, BlockListOp* out, int cap)
{
	if (!out || cap < 3) return 0;

	const int total = blockListCount();
	const int pages = blockListPages();
	const int p     = blockListClampPage(page);

	// Worst case for this page, checked BEFORE anything is written so a short buffer
	// returns 0 rather than a partially built screen.
	int on_page = total - p * BL_PER_PAGE;
	if (on_page < 0)            on_page = 0;
	if (on_page > BL_PER_PAGE)  on_page = BL_PER_PAGE;
	if (cap < 3 + on_page * 3)  return 0;

	int n = 0;

	pushRect(&out[n++], 0.0f, 0.0f, (float)BL_SCR_W, (float)BL_SCR_H, BLC_PANEL);

	snprintf(s_head, sizeof(s_head), "BLOCK LIST  %d blocks  page %d/%d",
	         total, p + 1, pages);
	pushText(&out[n++], (float)BL_MARGIN, (float)BL_HEAD_Y, BLC_HEAD, s_head);

	for (int slot = 0; slot < BL_PER_PAGE; slot++) {
		bool found = false;
		const BlockId id = nthDefined(p * BL_PER_PAGE + slot, &found);
		if (!found) break;   // the last page is short; the rest of the grid stays empty

		const int col = slot % BL_COLS;
		const int row = slot / BL_COLS;
		const float cx = (float)(BL_MARGIN + col * (BL_CELL_W + BL_MARGIN));
		const float cy = (float)(BL_GRID_Y + row * BL_CELL_H);

		pushRect(&out[n], cx, cy, (float)BL_CELL_W, (float)(BL_CELL_H - 2), BLC_CELL);
		out[n].kind = BL_OP_CELL;   // same rect, but tagged with the block it is listing
		out[n].id   = id;
		n++;

		// Air is listed but not illustrated — see the BL_OP_CELL comment in blocklist.h.
		if (id != REG_ID_AIR)
			pushIcon(&out[n++], cx + (float)BL_ICON_DX, cy + (float)BL_ICON_DY, id);

		// The id is on the row as well as the name. It is the number that appears in save
		// files, wire packets and every other debug readout in this game, and a screen that
		// showed only names would leave the reader converting between the two by hand.
		// Two hex digits, so the core rows (0x01..0x09) and the dynamic ones (0x80 up) are
		// visibly different ranges rather than arbitrary decimals.
		//
		// One static line buffer per cell, not one shared one: BlockListOp.text is held
		// until the caller has drawn the whole program, so sixteen labels have to coexist.
		// The precision on %s is load-bearing, not defensive style. BlockDef.name is a
		// fixed char[REGISTRY_NAME_MAX] and registry.h documents it as NUL-terminated, but a
		// name that used all sixteen bytes would leave no terminator in the array and %s
		// would run off the end of the packed struct into the next block's tex bytes.
		static char labels[BL_PER_PAGE][8 + REGISTRY_NAME_MAX];
		snprintf(labels[slot], sizeof(labels[slot]), "%02X %.*s",
		         (unsigned)id, REGISTRY_NAME_MAX, registryGet(id)->name);
		pushText(&out[n++], cx + (float)BL_TEXT_DX, cy + (float)BL_TEXT_DY,
		         BLC_TEXT, labels[slot]);
	}

	pushText(&out[n++], (float)BL_MARGIN, (float)BL_FOOT_Y, BLC_DIM, BL_FOOT_TEXT);

	return n;
}

// ── Screen state ──────────────────────────────────────────────────────────────────────

static bool s_ui_open;
static int  s_ui_page;

void blockListUiReset(void) { s_ui_open = false; s_ui_page = 0; }

void blockListUiOpen(void)
{
	s_ui_open = true;
	// Always page 0, not wherever the last visit left off. The list is short and the
	// player has just arrived from a different screen; resuming mid-list would look like
	// the top of the table is missing.
	s_ui_page = 0;
}

bool blockListUiIsOpen(void) { return s_ui_open; }

int blockListUiPage(void)
{
	// Clamped on the way OUT as well as on the way in, because the registry can grow while
	// this screen is open — a server's DEFS batch at join is exactly that — and a page
	// index that was valid when it was set can stop being valid without anyone pressing a
	// key. Answering with a stale index would index past the end of the table.
	return blockListClampPage(s_ui_page);
}

bool blockListUiInput(uint32_t keys)
{
	if (!s_ui_open) return false;

	if (keys & BL_KEY_CLOSE) {
		s_ui_open = false;
		return false;
	}

	// Both directions before the clamp, so pressing left and right on the same frame is a
	// no-op rather than whichever branch happens to be written second.
	int p = blockListUiPage();
	if (keys & BL_KEY_NEXT) p++;
	if (keys & BL_KEY_PREV) p--;
	s_ui_page = blockListClampPage(p);

	return true;
}
