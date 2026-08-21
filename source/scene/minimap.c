#include "scene/minimap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "world/block.h"

// Duplicate of gfx/sprite.h's SPRITE_RGBA — that header pulls in <citro3d.h> which
// is unavailable on the host; this macro is pure integer arithmetic and identical.
#ifndef SPRITE_RGBA
#define SPRITE_RGBA(r, g, b, a) \
	((uint32_t)(uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | \
	 ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(a) << 24))
#endif

// ── Block colour table ─────────────────────────────────────────────────────────────────
//
// Flat RGB packed as SPRITE_RGBA. One entry per BLOCK_* id; air is black, unknown is
// magenta (the standard "you forgot to add this" signal). Kept here rather than in block.h
// because the 3DS atlas textures own the real rendering colours — this is a lookup for a
// 64-pixel minimap only.
#define COL_AIR     SPRITE_RGBA(0,   0,   0,   0)
#define COL_GRASS   SPRITE_RGBA(76,  140, 48,  255)
#define COL_DIRT    SPRITE_RGBA(134, 96,  67,  255)
#define COL_STONE   SPRITE_RGBA(128, 128, 128, 255)
#define COL_SAND    SPRITE_RGBA(218, 200, 152, 255)
#define COL_WOOD    SPRITE_RGBA(101, 72,  46,  255)
#define COL_LEAVES  SPRITE_RGBA(44,  120, 32,  255)
#define COL_PLANKS  SPRITE_RGBA(160, 120, 72,  255)
#define COL_UNKNOWN SPRITE_RGBA(255, 0,   255, 255)

static uint32_t s_block_colors[BLOCK_COUNT] = {
	COL_AIR, COL_GRASS, COL_DIRT, COL_STONE, COL_SAND,
	COL_WOOD, COL_LEAVES, COL_PLANKS,
};

uint32_t minimapBlockColor(BlockId id)
{
	if (id < BLOCK_COUNT) return s_block_colors[id];
	return COL_UNKNOWN;
}

// ── Fog bitmap ─────────────────────────────────────────────────────────────────────────

static bool s_visible;

void minimapSetVisible(bool v) { s_visible = v; }

bool minimapIsVisible(void) { return s_visible; }

void minimapFogClear(MinimapFog* f)
{
	if (!f) return;
	memset(f->bits, 0, sizeof(f->bits));
}

bool minimapFogGet(const MinimapFog* f, int idx)
{
	if (!f || idx < 0 || idx >= MINIMAP_SIZE * MINIMAP_SIZE) return false;
	return (f->bits[idx / 64] >> (idx % 64)) & 1;
}

void minimapFogSet(MinimapFog* f, int idx)
{
	if (!f || idx < 0 || idx >= MINIMAP_SIZE * MINIMAP_SIZE) return;
	f->bits[idx / 64] |= (uint64_t)1 << (idx % 64);
}

void minimapFogUpdate(MinimapFog* f, int playerX, int playerZ, int offsetX, int offsetZ)
{
	if (!f) return;
	const int cx = playerX - offsetX;
	const int cz = playerZ - offsetZ;
	const int r2 = MINIMAP_EXPLORED * MINIMAP_EXPLORED;
	for (int z = 0; z < MINIMAP_SIZE; z++) {
		for (int x = 0; x < MINIMAP_SIZE; x++) {
			const int dx = x - cx;
			const int dz = z - cz;
			if (dx * dx + dz * dz <= r2)
				minimapFogSet(f, z * MINIMAP_SIZE + x);
		}
	}
}

// ── Save / load ────────────────────────────────────────────────────────────────────────

static void fogTmpPath(const char* path, char* out, size_t cap)
{
	snprintf(out, cap, "%s.tmp", path);
}

static void fogRecover(const char* path)
{
	char tmp[512];
	fogTmpPath(path, tmp, sizeof(tmp));
	FILE* t = fopen(tmp, "rb");
	if (!t) return;
	fclose(t);
	FILE* real = fopen(path, "rb");
	if (real) { fclose(real); remove(tmp); return; }
	rename(tmp, path);
}

bool minimapSave(const MinimapFog* f, const char* path)
{
	if (!f || !path) return false;

	char tmp[512];
	fogTmpPath(path, tmp, sizeof(tmp));

	FILE* fp = fopen(tmp, "wb");
	if (!fp) return false;

	const uint32_t magic = MINIMAP_SAVE_MAGIC;
	const uint32_t ver   = MINIMAP_SAVE_VER;
	const uint32_t sz    = MINIMAP_FOG_BYTES;

	bool ok = fwrite(&magic, 4, 1, fp) == 1
	       && fwrite(&ver,   4, 1, fp) == 1
	       && fwrite(&sz,    4, 1, fp) == 1
	       && fwrite(f->bits, MINIMAP_FOG_BYTES, 1, fp) == 1;

	if (fclose(fp) != 0 || !ok) { remove(tmp); return false; }
	remove(path);
	if (rename(tmp, path) != 0) return false;
	return true;
}

bool minimapLoad(MinimapFog* f, const char* path)
{
	if (!f || !path) { if (f) minimapFogClear(f); return false; }

	minimapFogClear(f);
	fogRecover(path);

	FILE* fp = fopen(path, "rb");
	if (!fp) return true;  // missing file = defaults, same as optionsLoad

	uint32_t magic, ver, sz;
	bool ok = fread(&magic, 4, 1, fp) == 1
	       && fread(&ver,   4, 1, fp) == 1
	       && fread(&sz,    4, 1, fp) == 1;

	if (!ok || magic != MINIMAP_SAVE_MAGIC || ver != MINIMAP_SAVE_VER || sz != MINIMAP_FOG_BYTES) {
		fclose(fp);
		minimapFogClear(f);
		return false;
	}

	ok = fread(f->bits, MINIMAP_FOG_BYTES, 1, fp) == 1;
	fclose(fp);
	if (!ok) minimapFogClear(f);
	return ok;
}

// ── 3DS rendering ──────────────────────────────────────────────────────────────────────
#ifdef __3DS__

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include "gfx/font.h"
#include "gfx/sprite.h"

#define MINIMAP_TEX_DIM 64

static C3D_Tex s_minimap_tex;
static bool    s_minimap_ready;

bool minimapGfxInit(void)
{
	if (s_minimap_ready) return true;
	if (!C3D_TexInit(&s_minimap_tex, (u16)MINIMAP_TEX_DIM, (u16)MINIMAP_TEX_DIM,
	                 GPU_RGBA8))
		return false;
	C3D_TexSetFilter(&s_minimap_tex, GPU_NEAREST, GPU_NEAREST);
	s_minimap_ready = true;
	return true;
}

void minimapGfxExit(void)
{
	if (!s_minimap_ready) return;
	C3D_TexDelete(&s_minimap_tex);
	s_minimap_ready = false;
}

// Writes one pixel into the texture's CPU-side buffer.
static inline void setPixel(int x, int y, uint32_t rgba)
{
	u32* dst = (u32*)s_minimap_tex.data + (u32)y * MINIMAP_TEX_DIM + (u32)x;
	*dst = rgba;
}

// Draws a small right-facing triangle (5 px wide, 7 px tall) centred at (cx, cy)
// rotated by `yaw` radians. The triangle points along +X at yaw=0.
static void drawArrow(float cx, float cy, float yaw)
{
	const float ca = cosf(yaw), sa = sinf(yaw);
	const float verts[3][2] = {
		{ 3.0f,  0.0f},
		{-2.0f, -3.0f},
		{-2.0f,  3.0f},
	};
	const uint32_t arrowCol = SPRITE_RGBA(255, 255, 0, 255);

	for (int t = 0; t < 3; t++) {
		const int next = (t + 1) % 3;
		const float x0 = cx + verts[t][0]  * ca - verts[t][1]  * sa;
		const float y0 = cy + verts[t][0]  * sa + verts[t][1]  * ca;
		const float x1 = cx + verts[next][0] * ca - verts[next][1] * sa;
		const float y1 = cy + verts[next][0] * sa + verts[next][1] * ca;

		const int px0 = (int)x0, py0 = (int)y0;
		const int px1 = (int)x1, py1 = (int)y1;
		const int dx = abs(px1 - px0), dy = -abs(py1 - py0);
		const int sx = px0 < px1 ? 1 : -1, sy = py0 < py1 ? 1 : -1;
		int err = dx + dy;
		int bx = px0, by = py0;
		for (;;) {
			if (bx >= 0 && bx < MINIMAP_TEX_DIM && by >= 0 && by < MINIMAP_TEX_DIM)
				setPixel(bx, by, arrowCol);
			if (bx == px1 && by == py1) break;
			const int e2 = 2 * err;
			if (e2 >= dy) { err += dy; bx += sx; }
			if (e2 <= dx) { err += dx; by += sy; }
		}
	}
}

void minimapDraw(const MinimapFog* f, int centerX, int centerZ, int offsetX, int offsetZ,
                 float yaw)
{
	(void)offsetX;
	(void)offsetZ;
	if (!s_minimap_ready || !f) return;

	const int ox = centerX - MINIMAP_SIZE / 2;
	const int oz = centerZ - MINIMAP_SIZE / 2;

	for (int z = 0; z < MINIMAP_SIZE; z++) {
		for (int x = 0; x < MINIMAP_SIZE; x++) {
			const int idx = z * MINIMAP_SIZE + x;
			if (!minimapFogGet(f, idx)) {
				setPixel(x, z, SPRITE_RGBA(80, 80, 80, 255));
				continue;
			}
			const int wx = ox + x;
			const int wz = oz + z;
			const BlockId id = worldGet(NULL, wx, 0, wz);
			setPixel(x, z, minimapBlockColor(id));
		}
	}

	drawArrow((float)(MINIMAP_SIZE / 2), (float)(MINIMAP_SIZE / 2), yaw);

	GSPGPU_FlushDataCache(s_minimap_tex.data, MINIMAP_TEX_DIM * MINIMAP_TEX_DIM * 4);

	spriteTexture(fontTexture());
	spriteRect(95.0f, 175.0f, 66.0f, 66.0f, SPRITE_RGBA(30, 30, 30, 200));
	spriteTexture(&s_minimap_tex);
	spriteQuad(96.0f, 176.0f, 64.0f, 64.0f,
	           0.0f, 1.0f, 1.0f, 0.0f,
	           SPRITE_WHITE);
}

#endif  // __3DS__
