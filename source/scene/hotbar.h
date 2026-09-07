// v1.9.0 HOTBAR-LR. The L and R shoulder buttons cycle the hotbar selection: R one slot on,
// L one slot back, wrapping at both ends, once on the press and then repeating while held.
//
// This is the PURE half. It decides WHICH slot the buttons are asking for and WHEN; it never
// writes the inventory and never reads HID. main.c feeds it the frame's `down`/`held` words
// and hands the answer to net/inv_bridge.h's invBridgeSelectHotbar — the one call the
// touchscreen hotbar tap already makes (scene/ui.c's handleHotbarSelect) — so both controls
// share a single write to selected_hotbar, a single clamp and a single BS_INV_OP_SELECT to
// the server. No <3ds.h>: the two key bits are mirrored below so tests/hotbar_test.c links
// this module on the host, the carve-out scene/ringorder.c and scene/aimtext.c already made.
//
// Until this change L and R stepped the render distance live (main.c step 7.7). That control
// now lives ONLY on the pause menu's OPT_ROW_DIST row (scene/pausemenu.c) — a settled
// decision, not one to re-open here.
#ifndef SCENE_HOTBAR_H
#define SCENE_HOTBAR_H

#include <stdbool.h>
#include <stdint.h>

// libctru's KEY_R / KEY_L (3ds/services/hid.h: BIT(8), BIT(9)), mirrored so this header
// compiles without <3ds.h>. hotbar.c static-asserts the two agree when built for the console.
#define HOTBAR_KEY_R 0x100u
#define HOTBAR_KEY_L 0x200u

// Held-to-repeat timing, in frames at the project's measured 59.83 fps. Same SHAPE as
// app/whatsnew.h's WHATSNEW_REPEAT_DELAY/PERIOD (one step on the press, a pause, then a
// steady cadence) and the same delay; the PERIOD is deliberately NOT whatsnew's 4. Four frames
// is ~15 steps a second, fine for scrolling text and hopeless for an 8-slot bar it would lap
// twice a second — 12 frames (~5 slots/s, a full lap in ~1.6 s) is slow enough to land on a
// slot and fast enough that holding still beats tapping seven times.
#define HOTBAR_REPEAT_DELAY  20
#define HOTBAR_REPEAT_PERIOD 12

// One per hotbar. Zero-initialise ({0}) and never touch the fields directly.
//   active       HOTBAR_KEY_L, HOTBAR_KEY_R or 0 — the button whose repeat is running
//   held_frames  frames `active` has been down, counting from the press
typedef struct {
	uint32_t active;
	int      held_frames;
} HotbarNav;

// Moves *sel one slot in direction `dir`, wrapping within [0, count-1]. `dir` is a DIRECTION,
// not a distance: any negative value is one back, any positive value one on, 0 is a no-op —
// the same rule world/inventory.h's inventoryHotbarStep states, so no caller comes to rely
// on +2 meaning two. A *sel outside the range is clamped into it before the step, so the
// answer is a real slot whatever was passed. count < 1 leaves *sel alone; count == 1 always
// answers 0. Pure apart from the write to *sel.
void hotbarNavStep(int* sel, int dir, int count);

// The frame's decision: -1 (L wants a step back), +1 (R wants a step on) or 0. `down` is the
// frame's press edges, `held` its level, both in libctru's key bits (HOTBAR_KEY_* above).
//
//   * A press fires on the frame it lands (held_frames 0), then nothing for
//     HOTBAR_REPEAT_DELAY frames, then once every HOTBAR_REPEAT_PERIOD frames while held.
//   * Releasing the active button re-arms it: the next press fires at once with a fresh delay.
//   * Both buttons: LAST PRESSED WINS. Pressing R while L is held switches to R immediately
//     (fires, delay restarts); L's repeat stops. Both pressed on the SAME frame is a tie and
//     moves nothing. Releasing the active button while the other is still down does NOT hand
//     over to it — that button was pressed earlier and already had its say; it has to be
//     pressed again. Deliberate: a hand-over would fire a step the player did not just ask for.
int hotbarNavPoll(HotbarNav* nav, uint32_t down, uint32_t held);

// hotbarNavPoll followed by hotbarNavStep on its answer. Returns true if *sel changed, which
// is what main.c gates its invBridgeSelectHotbar call on — a SELECT sent for an unchanged slot
// would be wire traffic for nothing.
bool hotbarNavFrame(HotbarNav* nav, uint32_t down, uint32_t held, int* sel, int count);

#endif
