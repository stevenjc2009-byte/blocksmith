// Screen and GPU bring-up.
//
// Top screen  : citro3d render target (the game).
// Bottom screen: libctru text console (the Phase 0 debug overlay).
//
// Phase 8 replaces the console with our own sprite batch + bitmap font when the
// bottom screen becomes real game UI.
#pragma once

#include <citro3d.h>

// Brings up gfx, the console and citro3d, and creates the top render target.
void screenInit(void);

// Tears everything down in reverse order.
void screenExit(void);

// The top-screen render target to draw the world into.
C3D_RenderTarget* screenTop(void);
