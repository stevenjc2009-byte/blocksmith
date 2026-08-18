// Screen and GPU bring-up.
//
// Top screen  : citro3d render target (the game).
// Bottom screen: libctru text console (the Phase 0 debug overlay).
//
// Phase 8 replaces the console with our own sprite batch + bitmap font when the
// bottom screen becomes real game UI.
#pragma once

#include <stddef.h>

#include <citro3d.h>

// Brings up gfx, the console and citro3d, and creates the top render target.
void screenInit(void);

// Tears everything down in reverse order.
void screenExit(void);

// The top-screen render target to draw the world into. This one is the left eye, which is
// also the only eye a 2D frame has.
C3D_RenderTarget* screenTop(void);

// Step 7.6's right eye. Created at init whether or not 3D is ever switched on, because a
// render target is 240x400 of colour plus depth out of VRAM and claiming that in the middle
// of a frame — the first time the player hits SELECT — is the kind of allocation that fails
// somewhere the player is standing rather than at boot. screenVramFree() is what says
// whether that was affordable; it is printed in the report.
C3D_RenderTarget* screenTopRight(void);

// Bytes of VRAM still free. Reported rather than assumed: the right eye's cost is a real
// number and this is where it shows up.
size_t screenVramFree(void);

// What the right eye's render target actually took out of VRAM, measured across the
// allocation at init rather than worked out from 240*400*4 twice. Constant for the life of
// the run. Nothing branches on it — it exists so the cost of step 7.6 is a number somebody
// can read off the console instead of a claim in a log entry.
size_t screenRightEyeBytes(void);
