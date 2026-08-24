// Bottom-screen debug menu UI, reachable from the pause menu's OPTIONS page.
//
// Follows the same pattern as app/remap_ui: owns its own drawing pass
// (spriteBegin/spriteEnd), D-pad navigation with A to activate and B to
// close, plus touch taps on visible rows. Returns true while the menu is
// open; false means the player pressed B and wants to go back.
//
// The caller provides a DebugContext with live pointers to the systems the
// menu reads. The context is read every frame the menu is drawn, so the
// numbers never go stale.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/debugmenu.h"

// Live data the debug menu reads each frame. All pointers are owned by the
// caller and must outlive the menu session.
typedef struct {
	int         render_dist;        // current render distance
	int         render_dist_min;
	int         render_dist_max;
	void (*set_render_dist)(int);   // apply render distance change

	// Frame stats
	float       frame_ms;
	int         meshes;
	uint32_t    tris;
	int         culled;

	// Simulation clock (v1.8.0 task 21). `tps` is measured against the system tick counter, not
	// asserted from the constant, so a console that cannot keep up shows a number below 20 here
	// instead of lying. `ticks_dropped` is the catch-up clamp's count: nonzero means simulation
	// time has fallen behind wall-clock time and stayed there.
	float       tps;
	uint64_t    ticks_dropped;

	// World stats
	int         columns;
	int         chunks;
	uint32_t    blocks_bytes;
	uint32_t    blocks_budget;

	// Player position
	float       player_x, player_y, player_z;
} DebugContext;

// Initialise the debug-menu UI state. Call once before the first update.
void debugMenuUiInit(void);

// Open the debug menu. The caller (pausemenu integration) calls this when
// the player selects "Debug" from the options page.
void debugMenuUiOpen(void);

// Is the debug menu currently open? The pause menu uses this to decide
// whether to draw/take input from the debug menu or its own options page.
bool debugMenuUiIsOpen(void);

// One frame of input and drawing. Returns true while the debug menu is
// open; false means the player pressed B to close it.
//
// `ctx` is read every frame the menu is drawn. `enabled` is toggled by
// the menu's own enable/disable state; the caller persists it through
// options.ini.
bool debugMenuUiUpdate(const DebugContext* ctx, bool* enabled,
                       uint32_t down,
                       bool touch_down, int touch_x, int touch_y);
