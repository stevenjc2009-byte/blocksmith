// The in-game pause menu: SELECT opens RESUME / OPTIONS / QUIT.
//
// Until this existed the only way to change a setting was to leave the world entirely and
// use the title screen's options page (scene/title.c), which meant unloading the world to
// adjust the render distance and loading it again to see what the change did — the one
// comparison the setting exists to let a player make. The options page here is deliberately
// a *subset* of that one: the settings that are worth changing while looking at the world,
// plus the memory readout that says what changing them costs.
//
// State lives here rather than in main.c because main.c's play loop is already long and
// this is self-contained: it owns nothing but its own cursor and a bool, reads the world's
// numbers through the PauseStats struct the caller fills in, and never touches the world
// itself. Applying a chosen render distance is the caller's job (main.c owns genSetRadius
// and the Options struct that has to be written back to the card), so this module cannot
// change the world by accident — it can only report what the player asked for.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// What the player chose this frame. NONE is the overwhelmingly common answer.
typedef enum {
	PAUSE_ACTION_NONE = 0,
	PAUSE_ACTION_RESUME,   // close the menu and carry on
	PAUSE_ACTION_QUIT,     // leave the world; the caller decides what that means
	PAUSE_ACTION_REMAP,    // open the control-remapping screen (v1.4.0)
	PAUSE_ACTION_DEBUG,    // open the debug menu (v1.4.0)
} PauseAction;

// Everything the menu displays, filled in by the caller each frame it draws.
//
// Bytes rather than KB throughout, converted for display here, so there is one rounding
// site and the caller passes the numbers its own APIs already return (linearSpaceFree and
// vramSpaceFree are bytes; world/budget.h's budgetUsed is bytes).
typedef struct {
	uint32_t linear_free;    // linear heap free — where the chunk mesh pool is claimed from
	uint32_t vram_free;      // VRAM free — the atlas and both framebuffers live here
	uint32_t world_used;     // block storage in use, world/budget.h budgetUsed()
	uint32_t world_budget;   // its ceiling, budget.h WORLD_BUDGET_BYTES

	int render_dist;         // the setting's current value, in columns
	int dist_min, dist_max;  // render_dist.h's RENDER_DIST_MIN / MAX, so the stepper knows
	                         // when to stop and the row can grey out at the ends

	bool stereo;             // is the top screen currently in 3D? main.c's s_stereo
} PauseStats;

// Is the menu currently up? The play loop asks this to decide whether to run the world
// this frame — a paused world must not tick, or a player who opened the menu to read the
// memory figures would find the numbers moving under them as columns streamed in.
bool pauseMenuOpen(void);

// SELECT. Opens the menu if it is closed, closes it if it is open, and always returns to
// the top page so reopening never lands mid-way down the options list.
void pauseMenuToggle(void);

// Closes the menu without choosing anything. For the caller's own paths — leaving a server
// session, say — so the menu can never be left up over a world that is no longer there.
void pauseMenuClose(void);

// One frame of input. `down` is hidKeysDown(). Returns what the player chose, and reports
// the two settings changes the options page can produce:
//   *out_dist_step        -1, 0 or +1 — a render-distance change to apply
//   *out_stereo_toggle    true once on the frame the player flipped 3D
//
// Both are reported rather than applied because the caller owns them: genSetRadius() in
// main.c re-meshes the ring and the Options struct has to be written back to the card;
// gfxSet3D() is a libctru call against the screen main.c set up. Doing either from in here
// would put world and hardware state behind a menu module, which is what this avoids.
//
// Either out pointer may be NULL. Safe to call when the menu is closed: it consumes nothing
// and returns NONE.
PauseAction pauseMenuInput(uint32_t down, int* out_dist_step, bool* out_stereo_toggle);

// Draws the menu over the bottom screen. Call inside the sprite batch, after the ordinary
// bottom-screen UI, so it lands on top of it. A no-op when the menu is closed.
void pauseMenuDraw(const PauseStats* st);
