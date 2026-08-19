#include "gfx/screen.h"

#include <3ds.h>
#include <stdio.h>

#define DISPLAY_TRANSFER_FLAGS                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                                  \
	 GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                                  \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget* s_top;
static C3D_RenderTarget* s_top_right;
static C3D_RenderTarget* s_bottom;
static size_t            s_right_eye_bytes;

void screenInit(void)
{
	gfxInitDefault();

#if !BS_BOTTOM_UI
	// Screen-presentation gotcha, paid for once already on the model-kit project:
	// C3D_FrameEnd only swaps buffers for screens that have a citro3d render
	// target bound. Our only target is the top screen, so the bottom screen would
	// never present and would sit black. Turning double buffering off for the
	// bottom screen makes console writes land in the single visible framebuffer,
	// so no per-frame swap is needed for it.
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	consoleInit(GFX_BOTTOM, NULL);

	// Blue background so a live-but-idle bottom screen is visibly ours rather
	// than indistinguishable from a dead screen.
	printf("\x1b[44;37m");
	consoleClear();
#endif

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	s_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// Step 7.6's right eye, created up front rather than on the first SELECT press. See
	// screen.h. gfxSet3D stays off here: 3D is opt-in and main.c owns the switch.
	//
	// Bracketed by vramSpaceFree so what the second eye actually costs is a measured number
	// rather than 240*400*4 twice on the back of an envelope — VRAM allocations are aligned
	// and the depth buffer's real footprint is the allocator's business, not arithmetic's.
	const size_t vram_before = vramSpaceFree();
	s_top_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	s_right_eye_bytes = vram_before - vramSpaceFree();
	C3D_RenderTargetSetOutput(s_top_right, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	gfxSet3D(false);

#if BS_BOTTOM_UI
	// 240x320, not 320x240: every 3DS render target is created in the framebuffer's own
	// rotated orientation, height first. The UI is laid out 320 wide and 240 tall, and the
	// rotation between the two is carried by Mtx_OrthoTilt in gfx/sprite.c, which is the
	// only place in the project that has to know about it.
	//
	// No depth buffer. A UI pass draws in submission order with depth testing off (see
	// gfx/sprite.h), so a depth attachment here would be ~150 KB of VRAM allocated to be
	// cleared every frame and never read.
	s_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
	C3D_RenderTargetSetOutput(s_bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
#endif
}

void screenExit(void)
{
	C3D_Fini();
	gfxExit();
}

C3D_RenderTarget* screenTop(void)
{
	return s_top;
}

C3D_RenderTarget* screenTopRight(void)
{
	return s_top_right;
}

C3D_RenderTarget* screenBottom(void)
{
	return s_bottom;   // NULL in a console build — see screen.h
}

size_t screenVramFree(void)
{
	return vramSpaceFree();
}

size_t screenRightEyeBytes(void)
{
	return s_right_eye_bytes;
}
