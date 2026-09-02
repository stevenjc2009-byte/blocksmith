// Host self-test for debug/blocklist.c — the debug menu's BLOCK LIST screen.
//
// Own main() and a CHECK macro, the same shape as app/debugmenu_test.c and
// source/scene/ui_layout_test.c.
//
// What this binary is actually for. The screen's one hard requirement is that it is
// REGISTRY-DRIVEN: a block added to world/registry.c later, or one a server registers over
// the wire at join, must appear with no edit to debug/blocklist.c. That claim cannot be
// proved by reading the source — a hardcoded list and a registry walk look identical from
// the outside when the two happen to agree today. So testRegistryDriven() below registers a
// brand-new block at runtime and asserts it shows up, which is the same thing another
// developer adding a block does, done in a second instead of a release.
//
// The rest of the file works on the op list debug/blocklist.h emits, NOT on a re-derivation
// of what the screen ought to look like. app/debugmenu_ui.c's console half draws exactly
// these ops and decides nothing, so a check here is a check on the real program.
#ifndef __3DS__

#include <stdio.h>
#include <string.h>

#include "debug/blocklist.h"
#include "world/registry.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                           \
	} while (0)

// ── Pinned literals ───────────────────────────────────────────────────────────────────
//
// Written as bare numbers, never derived from the BL_* macros they are checking. Same rule
// as app/debugmenu_test.c's DEBUG_MENU_MAX_ENTRIES_PIN and world/registry_test.c's
// REGISTRY_DYN_*_PIN, and for the reason that file spells out at length: a bound computed
// from the constant under test shrinks with it, so the checks that would have caught the
// change are never run rather than failed.
#define PIN_COLS      2
#define PIN_ROWS      8
#define PIN_PER_PAGE  16
#define PIN_SCR_W     320
#define PIN_SCR_H     240
#define PIN_ICON_PX   16

// The ABSOLUTE ceiling on one page's quads, over every registry this screen could ever be
// shown with — not today's measurement.
//
// Today's number is measured and printed by testQuadBudget, but it must NOT be pinned:
// blocks are being added to world/registry.c continuously, and the label is "XX " plus the
// block's name, so the real figure moves every time somebody registers a block with a
// longer name than the current longest. A pin on it would go red on an unrelated change
// and would be deleted by whoever tripped over it, which is worse than no pin at all.
//
// The ceiling is stable because it does not depend on any name:
//   background                                                                   1
//   header, "BLOCK LIST  256 blocks  page 16/16" at its very longest, 30 drawn   30
//   footer, BL_FOOT_TEXT, 21 drawn                                               21
//   16 cells x (plate 1 + icon 1 + label at most "FF " + 15 name chars = 17)     304
//                                                                              ----
//                                                                               356
#define PIN_WORST_PAGE_QUADS 356

static void testLayoutConstants(void)
{
	CHECK(BL_COLS == PIN_COLS);
	CHECK(BL_ROWS == PIN_ROWS);
	CHECK(BL_PER_PAGE == PIN_PER_PAGE);
	CHECK(BL_ICON_PX == PIN_ICON_PX);

	// The bottom screen, not the top. The owner asked for this screen specifically on the
	// bottom, and 320x240 IS the bottom screen — the top is 400x240. A build that started
	// drawing this at 400 wide would be on the wrong screen, so the width is the check.
	CHECK(BL_SCR_W == PIN_SCR_W);
	CHECK(BL_SCR_H == PIN_SCR_H);

	// The two columns and the margins have to tile the screen exactly, or the right-hand
	// column runs off the edge and its labels are silently clipped.
	CHECK(BL_MARGIN + BL_CELL_W + BL_MARGIN + BL_CELL_W + BL_MARGIN == PIN_SCR_W);

	// The grid must finish above the footer.
	CHECK(BL_GRID_Y + BL_ROWS * BL_CELL_H <= BL_FOOT_Y);

	// The label column must clear the icon, or the name is drawn over the art.
	CHECK(BL_TEXT_DX >= BL_ICON_DX + BL_ICON_PX);
}

static void testPaging(void)
{
	registryInitCore();

	const int n     = blockListCount();
	const int pages = blockListPages();

	CHECK(n > 0);
	CHECK(pages >= 1);
	CHECK(pages == (n + PIN_PER_PAGE - 1) / PIN_PER_PAGE);

	// Wrapping, in both directions. C's % keeps the dividend's sign, so the negative case
	// is the one that indexes out of the table if it is written naively.
	CHECK(blockListClampPage(0) == 0);
	CHECK(blockListClampPage(pages) == 0);
	CHECK(blockListClampPage(pages * 3) == 0);
	CHECK(blockListClampPage(-1) == pages - 1);
	CHECK(blockListClampPage(-pages - 1) == pages - 1);
}

// Every defined id appears on exactly one page, exactly once, in ascending order, and
// nothing that is NOT defined appears at all. This is the enumeration contract the whole
// screen rests on.
static void testEveryBlockAppearsExactlyOnce(void)
{
	registryInitCore();

	int seen[256];
	memset(seen, 0, sizeof(seen));

	const int pages = blockListPages();
	int drawn = 0;
	int last  = -1;

	for (int p = 0; p < pages; p++) {
		BlockListOp ops[BL_MAX_OPS];
		const int n = blockListBuild(p, ops, BL_MAX_OPS);
		CHECK(n > 0);

		// Keyed on the CELL plates, not on the icons: air is listed without an icon, so an
		// icon-keyed count would report one block short and call it correct.
		for (int i = 0; i < n; i++) {
			if (ops[i].kind != BL_OP_CELL) continue;
			const int id = (int)ops[i].id;
			CHECK(id >= 0 && id <= 0xFF);
			seen[id]++;
			drawn++;
			CHECK(id > last);   // ascending across the whole run, pages included
			last = id;
		}
	}

	CHECK(drawn == blockListCount());

	// Air is on the listing and has no icon; every other listed block has exactly one.
	{
		BlockListOp ops[BL_MAX_OPS];
		const int n = blockListBuild(0, ops, BL_MAX_OPS);
		int cells = 0, icons = 0;
		bool air_listed = false, air_iconed = false;
		for (int i = 0; i < n; i++) {
			if (ops[i].kind == BL_OP_CELL) {
				cells++;
				if (ops[i].id == REG_ID_AIR) air_listed = true;
			}
			if (ops[i].kind == BL_OP_ICON) {
				icons++;
				if (ops[i].id == REG_ID_AIR) air_iconed = true;
			}
		}
		CHECK(air_listed);
		CHECK(!air_iconed);
		CHECK(icons == cells - 1);   // exactly one un-illustrated row: air
	}

	for (int id = 0; id <= 0xFF; id++) {
		if (registryIsDefined((BlockId)id)) CHECK(seen[id] == 1);
		else                                CHECK(seen[id] == 0);
	}
}

// ── The registry-driven proof ─────────────────────────────────────────────────────────
//
// A new block registered at runtime must appear, with its own name and its own art, with
// no edit to debug/blocklist.c. This is exactly what happens when someone adds a row to
// world/registry.c or when a server sends a DEFS batch at join.
static void testRegistryDriven(void)
{
	registryInitCore();

	const int before       = blockListCount();
	const int pages_before = blockListPages();

	BlockDef def;
	memset(&def, 0, sizeof(def));
	snprintf(def.name, sizeof(def.name), "testrock");
	// A tile no core row uses, so "the new block drew the new art" is distinguishable from
	// "the new block drew grass", which is what a broken icon lookup would do.
	for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = 40;
	def.flags     = REG_FLAG_SOLID;
	def.hardness  = 3;

	const BlockId id = registryRegister(&def);
	CHECK(id != 0);
	CHECK(id >= REG_ID_DYN_LO);

	CHECK(blockListCount() == before + 1);
	CHECK(blockListPages() >= pages_before);

	// Find it in the op list, without knowing which page it landed on.
	bool found_icon = false;
	bool found_name = false;
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0;

	for (int p = 0; p < blockListPages(); p++) {
		BlockListOp ops[BL_MAX_OPS];
		const int n = blockListBuild(p, ops, BL_MAX_OPS);
		for (int i = 0; i < n; i++) {
			if (ops[i].kind == BL_OP_ICON && ops[i].id == id) {
				found_icon = true;
				u0 = ops[i].u0; v0 = ops[i].v0; u1 = ops[i].u1; v1 = ops[i].v1;
			}
			if (ops[i].kind == BL_OP_TEXT && ops[i].text &&
			    strstr(ops[i].text, "testrock") != NULL)
				found_name = true;
		}
	}

	CHECK(found_icon);
	CHECK(found_name);

	// And its icon must be its OWN tile, not slot 0's. On this hardware a wrong texture
	// constant still renders *a* texture, so this is the difference between a visible bug
	// and a silent one.
	const AtlasRect want = atlasRect(40);
	CHECK(v0 == (float)(want.vslot1 * TILE_PX) / (float)ATLAS_H_PX);
	CHECK(v1 == (float)(want.vslot0 * TILE_PX) / (float)ATLAS_H_PX);
	CHECK(u0 == (float)want.u0 / (float)ATLAS_W_PX);
	CHECK(u1 == (float)want.u1 / (float)ATLAS_W_PX);

	registryInitCore();   // leave the table as the other tests expect it
}

// Every icon op must carry the UVs of ITS OWN block's FACE_TOP tile, and blocks whose top
// tiles differ must get different UVs. The second half is what fails when an icon lookup
// is accidentally constant — the exact bug that reads as "all the art is wrong" rather
// than as an error.
static void testIconUvsArePerBlock(void)
{
	registryInitCore();

	int distinct_v = 0;
	float seen_v[256];
	int   seen_n = 0;

	for (int p = 0; p < blockListPages(); p++) {
		BlockListOp ops[BL_MAX_OPS];
		const int n = blockListBuild(p, ops, BL_MAX_OPS);
		for (int i = 0; i < n; i++) {
			if (ops[i].kind != BL_OP_ICON) continue;

			const AtlasRect want = atlasRect(blockFaceTex(ops[i].id, FACE_TOP));
			CHECK(ops[i].v0 == (float)(want.vslot1 * TILE_PX) / (float)ATLAS_H_PX);
			CHECK(ops[i].v1 == (float)(want.vslot0 * TILE_PX) / (float)ATLAS_H_PX);

			bool dup = false;
			for (int k = 0; k < seen_n; k++)
				if (seen_v[k] == ops[i].v0) { dup = true; break; }
			if (!dup && seen_n < 256) {
				seen_v[seen_n++] = ops[i].v0;
				distinct_v++;
			}
		}
	}

	// The core registry has plainly more than three distinct top textures. A constant
	// lookup would collapse this to 1.
	CHECK(distinct_v > 3);
}

// Nothing may be drawn off the bottom screen. A layout that overflowed would not error —
// the quad is simply clipped — so the screen would just quietly lose its last row.
static void testEverythingIsOnScreen(void)
{
	registryInitCore();

	for (int p = 0; p < blockListPages(); p++) {
		BlockListOp ops[BL_MAX_OPS];
		const int n = blockListBuild(p, ops, BL_MAX_OPS);
		for (int i = 0; i < n; i++) {
			CHECK(ops[i].x >= 0.0f);
			CHECK(ops[i].y >= 0.0f);
			CHECK(ops[i].x + ops[i].w <= (float)PIN_SCR_W);
			CHECK(ops[i].y + ops[i].h <= (float)PIN_SCR_H);

			if (ops[i].kind == BL_OP_TEXT) {
				CHECK(ops[i].text != NULL);
				// 6 px an advance (gfx/font.h FONT_ADVANCE), 7 px tall.
				const int w = (int)strlen(ops[i].text) * 6;
				CHECK((int)ops[i].x + w <= PIN_SCR_W);
				CHECK((int)ops[i].y + 7 <= PIN_SCR_H);
			}
		}
	}
}

// Cells must not overlap each other, which is the failure a wrong column stride produces
// and which no bounds check above would catch.
static void testCellsDoNotOverlap(void)
{
	registryInitCore();

	BlockListOp ops[BL_MAX_OPS];
	const int n = blockListBuild(0, ops, BL_MAX_OPS);

	for (int i = 0; i < n; i++) {
		if (ops[i].kind != BL_OP_CELL) continue;
		for (int j = i + 1; j < n; j++) {
			if (ops[j].kind != BL_OP_CELL) continue;
			const bool apart =
				ops[i].x + ops[i].w <= ops[j].x || ops[j].x + ops[j].w <= ops[i].x ||
				ops[i].y + ops[i].h <= ops[j].y || ops[j].y + ops[j].h <= ops[i].y;
			CHECK(apart);
		}
	}
}

// ── The quad budget ───────────────────────────────────────────────────────────────────
//
// gfx/sprite.c's SPRITE_MAX_QUADS is 1024 and the batch flushes when it fills, so an
// overrun is not a crash — it is a second draw call, and the screen still looks right.
// That makes the number something to MEASURE rather than to assume, and this is where it
// is measured: blockListQuadsFor walks the same op list the console half draws.
static void testQuadBudget(void)
{
	registryInitCore();

	// A full page is the worst case, so make sure there is one to measure. The core table
	// has 10 rows, which is less than a page, so fill the dynamic range until one page is
	// full and the number below is the real ceiling rather than today's short list.
	// BOUNDED, and the bound is not cosmetic. Written as a bare `while (blockListCount() <
	// PIN_PER_PAGE)` this loop HUNG THE TEST BINARY under a red arm that made
	// blockListCount() return a constant 10: the condition could never be satisfied, so it
	// registered blocks forever. A check that hangs is worse than one that fails — a failure
	// prints a line, a hang looks exactly like a slow machine, and the two stuck processes
	// had to be found with ps and killed by PID.
	//
	// REGISTRY_MAX is 256, so 300 iterations cannot be reached by any honest run; the guard
	// fires only when something upstream is lying or the table is full, and then it reports
	// and goes red instead of spinning.
	int guard = 0;
	while (blockListCount() < PIN_PER_PAGE) {
		if (++guard > 300) {
			printf("  (registry never reached a full page: count stuck at %d after %d "
			       "registrations)\n", blockListCount(), guard);
			CHECK(guard <= 300);
			break;
		}
		BlockDef def;
		memset(&def, 0, sizeof(def));
		// The longest name the registry allows, so the label quad count is the worst case
		// too — REGISTRY_NAME_MAX is 16 including the terminator, so 15 characters.
		snprintf(def.name, sizeof(def.name), "fill%011d", blockListCount());
		for (int f = 0; f < BLOCK_FACES; f++) def.tex[f] = 5;
		def.flags = REG_FLAG_SOLID;
		if (registryRegister(&def) == 0) {
			CHECK(registryRegister(&def) != 0);   // report the refusal, once
			break;                                // table full — do not spin on it
		}
	}

	BlockListOp ops[BL_MAX_OPS];
	const int n = blockListBuild(0, ops, BL_MAX_OPS);
	CHECK(n > 0);
	CHECK(n <= BL_MAX_OPS);

	// A full page: background + header + footer + one plate and one label per block, plus
	// one icon per block that has art. Derived from the ops themselves rather than pinned
	// at 51, because air is listed without an icon and page 0 always contains air.
	int cells = 0, icons = 0, texts = 0;
	for (int i = 0; i < n; i++) {
		if (ops[i].kind == BL_OP_CELL) cells++;
		if (ops[i].kind == BL_OP_ICON) icons++;
		if (ops[i].kind == BL_OP_TEXT) texts++;
	}
	CHECK(cells == PIN_PER_PAGE);            // the page really is full
	CHECK(texts == PIN_PER_PAGE + 2);        // one label each, plus header and footer
	CHECK(n == 1 + texts + cells + icons);   // and nothing else is in the stream

	const int quads = blockListQuadsFor(ops, n);
	CHECK(quads <= PIN_WORST_PAGE_QUADS);
	CHECK(PIN_WORST_PAGE_QUADS < 1024);   // SPRITE_MAX_QUADS: one batch, one draw call
	CHECK(quads < 1024);

	// The measured figure for the registry this build actually has, and the ceiling it is
	// being read against. Printed rather than only asserted because the number is the
	// deliverable here — "it fits in the batch" is a claim about a quantity, and a check
	// that passes silently does not tell anyone what the quantity was.
	printf("blocklist: %d blocks, %d pages; full page = %d ops, %d quads"
	       " (ceiling %d, SPRITE_MAX_QUADS 1024)\n",
	       blockListCount(), blockListPages(), n, quads, PIN_WORST_PAGE_QUADS);

	registryInitCore();
}

static void testTextQuadRule(void)
{
	// Mirrors gfx/font.c:96-98: space draws nothing, printable ASCII draws one quad.
	CHECK(blockListTextQuads("") == 0);
	CHECK(blockListTextQuads(NULL) == 0);
	CHECK(blockListTextQuads("abc") == 3);
	CHECK(blockListTextQuads("a b c") == 3);
	CHECK(blockListTextQuads("   ") == 0);
	CHECK(blockListTextQuads("a\nb") == 2);   // '\n' is outside 32..126
}

// A short buffer must refuse rather than emit half a screen.
static void testShortBufferRefuses(void)
{
	registryInitCore();

	BlockListOp ops[BL_MAX_OPS];
	CHECK(blockListBuild(0, ops, 0) == 0);
	CHECK(blockListBuild(0, ops, 2) == 0);
	CHECK(blockListBuild(0, NULL, BL_MAX_OPS) == 0);
	CHECK(blockListBuild(0, ops, BL_MAX_OPS) > 0);
}

// The screen's open/close/paging state machine. It lives in blocklist.c rather than in
// app/debugmenu_ui.c precisely so it can be driven from here — see blocklist.h's note on
// what app/debugmenu_test.c links.
static void testUiStateMachine(void)
{
	registryInitCore();
	blockListUiReset();

	CHECK(!blockListUiIsOpen());
	// Input to a closed screen must do nothing and report closed, not open it by accident.
	CHECK(blockListUiInput(BL_KEY_NEXT) == false);
	CHECK(!blockListUiIsOpen());

	blockListUiOpen();
	CHECK(blockListUiIsOpen());
	CHECK(blockListUiPage() == 0);

	// Paging wraps in both directions, over however many pages the registry currently has.
	const int pages = blockListPages();
	CHECK(blockListUiInput(BL_KEY_NEXT) == true);
	CHECK(blockListUiPage() == (pages > 1 ? 1 : 0));
	CHECK(blockListUiInput(BL_KEY_PREV) == true);
	CHECK(blockListUiPage() == 0);
	CHECK(blockListUiInput(BL_KEY_PREV) == true);
	CHECK(blockListUiPage() == pages - 1);

	// Both directions on one frame cancel rather than racing.
	blockListUiOpen();
	CHECK(blockListUiPage() == 0);
	CHECK(blockListUiInput(BL_KEY_PREV | BL_KEY_NEXT) == true);
	CHECK(blockListUiPage() == 0);

	// Close reports false ONCE and leaves the screen closed.
	CHECK(blockListUiInput(BL_KEY_CLOSE) == false);
	CHECK(!blockListUiIsOpen());
	CHECK(blockListUiInput(BL_KEY_CLOSE) == false);

	// Reopening starts at the top of the list, not where the last visit ended.
	blockListUiOpen();
	blockListUiInput(BL_KEY_NEXT);
	blockListUiInput(BL_KEY_CLOSE);
	blockListUiOpen();
	CHECK(blockListUiPage() == 0);

	// A page index that was valid can stop being valid while the screen is open: a server
	// registering blocks at join grows the table under it. The page must clamp, not index
	// off the end.
	blockListUiOpen();
	for (int i = 0; i < 40; i++) blockListUiInput(BL_KEY_NEXT);
	CHECK(blockListUiPage() >= 0);
	CHECK(blockListUiPage() < blockListPages());

	blockListUiReset();
	registryInitCore();
}

int main(void)
{
	testLayoutConstants();
	testPaging();
	testEveryBlockAppearsExactlyOnce();
	testRegistryDriven();
	testIconUvsArePerBlock();
	testEverythingIsOnScreen();
	testCellsDoNotOverlap();
	testQuadBudget();
	testTextQuadRule();
	testShortBufferRefuses();
	testUiStateMachine();

	if (s_fails) {
		printf("blocklist self-test: FAIL %d/%d  first: %s\n",
		       s_fails, s_checks, s_first);
		return 1;
	}
	printf("blocklist self-test: PASS  %d checks\n", s_checks);
	return 0;
}

#endif   // !__3DS__
