// Host stubs for tests/ui_chest_test.c — see ui_chest_stub.h. Same philosophy as
// tests/player_water_stub/audio_stub.c beside it: these are stubs, not reimplementations of
// anything under test. The REAL source/scene/ui.c is linked and exercised; only the things it
// calls out to that cannot exist on a host are replaced, and each one does the least it can
// while still being the "did nothing" answer the real API documents.
//
// What is stubbed, and why each is safe to stub for a test about TRANSFERS:
//
//   gfx/sprite.h    spriteBegin/Texture/Quad/Rect/End — the GPU batch. ui.c draws every frame
//                   unconditionally (its first move is spriteBegin(320, 240)); none of it reads
//                   or writes a slot. RECORDED rather than discarded — geometry and a quad
//                   count — so the test can also prove what the panel drew and how much of
//                   sprite.c's 1024-quad frame budget it used. Nothing here decides anything.
//   gfx/font.h      fontTexture/fontDraw/fontDrawf/fontTextWidth — the glyph sheet. fontDraw
//                   counts the quads the real one would have emitted (one per non-space glyph
//                   in FONT_FIRST..FONT_LAST, font.c:120-140) and draws nothing. fontTextWidth
//                   returns the same cell-width arithmetic the header documents (advance x
//                   length x scale) because ui.c uses it to centre labels, and a width of 0
//                   would be a stub with an opinion.
//   gfx/atlas.h     atlasTile — delegates to world/atlas_uv.h's atlasRect(), the host-clean tile
//                   arithmetic gfx/atlas.c's own atlasTile() is a one-line wrapper around, so
//                   this is the real function minus the GPU upload rather than a fake.
//   audio/audio.h   audioSfxId/audioPlay — the tap click. audio.h's own header says play
//                   "returns AUDIO_VOICE_NONE, and nothing crashes, blocks or logs per frame"
//                   when audio is unavailable; that is what these return.
//   debug/biomeinfo.h  debugBiomeRow — the HUD's biome line. Only reached with a non-NULL
//                   UiStats; an empty string is what an unwired lookup draws.
//   scene/chunk_render.h  chunkRenderCamera — the position that biome line is looked up at.
//   net/networld.h  networldSendInvAction — the send net/inv_bridge.c makes for every bag
//                   mutation. Recorded, not dropped: ui_chest_stub.h says why the test reads it.
//                   Returns false, which is what the real one answers with no session.
//
// Nothing in source/net, source/gfx or source/audio is edited, read into, or depended on by
// this file beyond the headers it includes for the signatures.

#include "ui_chest_stub.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/audio_sfx.h"
#include "debug/biomeinfo.h"
#include "gfx/atlas.h"
#include "gfx/font.h"
#include "gfx/sprite.h"
#include "net/networld.h"
#include "scene/chunk_render.h"

// ── audio ──────────────────────────────────────────────────────────────────────────────

AudioSoundId audioSfxId(SfxSlot slot)
{
	(void)slot;
	return AUDIO_SOUND_NONE;
}

AudioVoice audioPlay(AudioSoundId sound, AudioPriority prio, float gain)
{
	(void)sound;
	(void)prio;
	(void)gain;
	return AUDIO_VOICE_NONE;
}

// ── sprite batch (recorded) ────────────────────────────────────────────────────────────

static UiChestStubDraw s_draw;

const UiChestStubDraw* uiChestStubDraw(void)
{
	return &s_draw;
}

void spriteBegin(int w, int h)
{
	(void)w;
	(void)h;
	// A frame's record starts here, the way sprite.c's own counter restarts on spriteBegin.
	memset(&s_draw, 0, sizeof(s_draw));
	s_draw.begins = 1;
}

void spriteTexture(C3D_Tex* tex)
{
	(void)tex;
	s_draw.tex_changes++;
}

void spriteQuad(float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, uint32_t colour)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)u0;
	(void)v0;
	(void)u1;
	(void)v1;
	(void)colour;
	s_draw.icon_quads++;
	s_draw.quads++;
}

void spriteRect(float x, float y, float w, float h, uint32_t colour)
{
	if (s_draw.rects < UI_CHEST_STUB_RECT_MAX) {
		UiChestStubRect* r = &s_draw.rect[s_draw.rects];
		r->x = x; r->y = y; r->w = w; r->h = h; r->colour = colour;
	}
	s_draw.rects++;
	s_draw.quads++;
}

void spriteEnd(void)
{
}

// ── font ───────────────────────────────────────────────────────────────────────────────

static C3D_Tex s_font_tex;   // never read; a real, distinct address is all fontTexture promises

C3D_Tex* fontTexture(void)
{
	return &s_font_tex;
}

// The real font.c's glyph range, restated (FONT_FIRST 32, FONT_LAST 126 at font.c:22-23) so
// the quad count below matches what the console batch would hold. Space is inside the range
// but the real fontDraw skips it explicitly, so it is skipped here too.
#define STUB_FONT_FIRST 32
#define STUB_FONT_LAST  126

void fontDraw(float x, float y, int scale, uint32_t colour, const char* text)
{
	(void)x;
	(void)y;
	(void)colour;
	if (!text || scale < 1) return;
	for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
		if (*p != ' ' && *p >= STUB_FONT_FIRST && *p <= STUB_FONT_LAST) {
			s_draw.glyph_quads++;
			s_draw.quads++;
		}
	}
}

// Formats into the same 128-byte buffer the real fontDrawf uses (font.c:146-155), then
// counts through fontDraw above, so a long HUD line truncates to the same glyph count.
void fontDrawf(float x, float y, int scale, uint32_t colour, const char* fmt, ...)
{
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fontDraw(x, y, scale, colour, buf);
}

int fontTextWidth(const char* text, int scale)
{
	return (int)strlen(text) * FONT_ADVANCE * scale;
}

// ── atlas ──────────────────────────────────────────────────────────────────────────────

AtlasRect atlasTile(int tile)
{
	return atlasRect(tile);
}

// ── HUD readouts ───────────────────────────────────────────────────────────────────────

const char* debugBiomeRow(int32_t x, int32_t z, char* buf, size_t cap)
{
	(void)x;
	(void)z;
	if (cap) buf[0] = '\0';
	return buf;
}

void chunkRenderCamera(float* x, float* y, float* z)
{
	*x = 0.0f;
	*y = 0.0f;
	*z = 0.0f;
}

// ── net ────────────────────────────────────────────────────────────────────────────────

static UiChestStubNet s_net;

bool networldSendInvAction(uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
	s_net.calls++;
	s_net.op = op;
	s_net.a  = a;
	s_net.b  = b;
	s_net.c  = c;
	return false;
}

const UiChestStubNet* uiChestStubNet(void)
{
	return &s_net;
}

void uiChestStubResetNet(void)
{
	memset(&s_net, 0, sizeof(s_net));
}
