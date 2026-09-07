// The in-game pause menu: SELECT opens a three-tab panel.
//
//     GAME     Resume, Quit to title
//     OPTIONS  Render dist ±, 3D on/off, Sound ±
//     SYSTEM   Controls >, Debug >, and the memory readout under the rows
//
// Until this existed the only way to change a setting was to leave the world entirely and
// use the title screen's options page (scene/title.c), which meant unloading the world to
// adjust the render distance and loading it again to see what the change did — the one
// comparison the setting exists to let a player make. The OPTIONS tab here is deliberately
// a *subset* of that one: the settings that are worth changing while looking at the world,
// plus the memory readout that says what changing them costs.
//
// v1.9.1 INTERFACE (docs/blueprint-1.9.1-interface.md D9) replaced the two stacked pages
// this shipped with — main: Resume/Options/Quit, then an options page behind it — with the
// three tabs above, inside the same scrim and the same panel. L/R switch tab; the d-pad and
// the stylus both work; every function below keeps the signature it shipped with and every
// PauseAction keeps its value, because main.c switches on them.
//
// The MODEL — which tab and row the cursor is on after any key or any stylus press, which
// action that produces, and where every rect is — lives in scene/pausebar.h / .c, which has
// no <3ds.h> in it and is gated on the host by tests/pausebar_test.c. This pair keeps the
// drawing, the one audio call the volume row makes, and the three memory figures: the parts
// no host test can reach and the parts that were never the ones that could be subtly wrong.
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
// the GAME tab's first row so reopening never lands mid-way down a tab. The per-tab row
// memory goes with it: it is a memory of THIS visit, not of the last one.
void pauseMenuToggle(void);

// Closes the menu without choosing anything. For the caller's own paths — leaving a server
// session, say — so the menu can never be left up over a world that is no longer there.
void pauseMenuClose(void);

// One frame of input. `down` is hidKeysDown() — the PRESS EDGE word, not the level: nothing
// in here repeats, and feeding the level would step a value once per frame while a direction
// is held. It also consumes whatever pauseMenuTouch recorded this frame, and the tap wins.
// Returns what the player chose, and reports the two settings changes the OPTIONS tab can
// produce:
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

// True once if the player moved the sound volume since the last call, and clears the
// flag. The volume itself is NOT reported: unlike render distance and 3D there is
// nothing for the caller to apply — audioSetMasterVolume() is the whole action and this
// module already made it. What the caller still owns is the card, so this exists purely
// so main.c knows to copy audioGetMasterVolume() into its Options and optionsSave().
//
// A separate poll rather than a third out-param on pauseMenuInput above, because that
// signature is called from main.c and changing it would break a file this change does
// not own. Ignoring this function entirely is a supported outcome: the volume still
// works for the session, it just does not survive a reboot.
bool pauseMenuTakeVolumeChanged(void);

// One stylus press on the pause panel. `press` is the RISING EDGE — main.c's `touch_press`,
// not `touch.touch_down` — because a stylus held still on a stepper arrow must step once,
// not once a frame.
//
// Call it IMMEDIATELY BEFORE pauseMenuInput each frame, unconditionally, exactly as
// hidKeysDown() is read unconditionally. It records at most one action and OVERWRITES what
// the previous frame recorded, so a frame with no tap clears the slot; pauseMenuInput
// consumes it and reports it through its own return value and out-params. A tap and a button
// press on the same frame therefore produce ONE action, the tap's, and a tap on a frame whose
// pauseMenuInput is skipped (the remap/debug gate in main.c) is dropped rather than fired
// late against a row the player has since moved off.
//
// Safe when the menu is closed and on a press outside the panel: it records nothing. A tap
// outside the panel does NOT close it — the panel is modal and a mis-aimed stylus must not
// drop the player back into the world.
//
// A separate entry point rather than three more parameters on pauseMenuInput because that
// signature is called from main.c and the blueprint pins it unchanged (§6.3).
void pauseMenuTouch(bool press, int x, int y);

// Draws the menu over the bottom screen. Call inside the sprite batch, after the ordinary
// bottom-screen UI, so it lands on top of it. A no-op when the menu is closed.
void pauseMenuDraw(const PauseStats* st);
