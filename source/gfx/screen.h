// Screen and GPU bring-up.
//
// Top screen  : citro3d render target (the game).
// Bottom screen: libctru text console (the Phase 0 debug overlay), or — with BS_BOTTOM_UI —
//                a second citro3d render target for the real game UI.
//
// Step 8.3 is where the second of those becomes possible.
#pragma once

#include <stddef.h>

#include <citro3d.h>

// Step 8.3. 0 = the bottom screen is libctru's text console; 1 = it is a citro3d render
// target driven by gfx/sprite.c and gfx/font.c.
//
// The two are mutually exclusive rather than layered: consoleInit claims the bottom
// framebuffer, and gfxSetDoubleBuffering(GFX_BOTTOM, false) turns off the very swap a
// citro3d render target needs to present. So this is a switch, not a layering.
//
// **Step 8.2 flipped the default from 0 to 1.** It was 0 through step 8.3 because every probe
// this project has — BS_SAVE_CHECK, BS_REMESH_STRESS, BS_DIG_OUT, BS_WALK_STRESS and the
// per-frame status line — reports by printf into that console, and the console is also the
// only way to read a number back off the emulator; retiring it in the same step that
// introduced its replacement would have blinded every existing check at the exact moment the
// replacement was least trusted. Step 8.2 is where the replacement earns it: the bottom
// screen now carries the hotbar, the inventory grid and the crafting panel (scene/ui.c), and
// a shipped build showing a text console instead of those is not the game.
//
// ⚠ **Every measurement and probe build must now pass `-DBS_BOTTOM_UI=0` explicitly**, or it
// loses the console it reports through and comes back with no output at all. Nothing about
// what those builds measure changed — the world, the mesher and the frame loop are identical
// — only which surface the answer is printed on. A probe build gets the console back and, by
// main.c's BS_TITLE (which follows this switch), also keeps booting straight into the world
// with no menu waiting for a tap that a scripted run will never deliver.
#ifndef BS_BOTTOM_UI
#define BS_BOTTOM_UI 1
#endif

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

// Step 8.3's bottom screen, 320x240. NULL unless BS_BOTTOM_UI is 1 — callers must check,
// because in a console build there is genuinely no target to draw on and a UI pass that
// assumed one would draw the game's HUD over the debug text of whichever probe is running.
C3D_RenderTarget* screenBottom(void);

// Bytes of VRAM still free. Reported rather than assumed: the right eye's cost is a real
// number and this is where it shows up.
size_t screenVramFree(void);

// What the right eye's render target actually took out of VRAM, measured across the
// allocation at init rather than worked out from 240*400*4 twice. Constant for the life of
// the run. Nothing branches on it — it exists so the cost of step 7.6 is a number somebody
// can read off the console instead of a claim in a log entry.
size_t screenRightEyeBytes(void);
