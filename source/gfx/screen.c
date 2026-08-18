#include "gfx/screen.h"

#include <3ds.h>
#include <stdio.h>

#define DISPLAY_TRANSFER_FLAGS                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                                  \
	 GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                                  \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget* s_top;

void screenInit(void)
{
	gfxInitDefault();

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

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	s_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
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
