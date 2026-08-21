// Console-only UI for the input remapping screen.
//
// Reachable from the pause menu's OPTIONS page. Lists all mappable actions with
// their current physical button; tapping an action enters "press a button" capture
// mode; a "Reset to defaults" row at the bottom. D-pad navigates, A selects,
// B goes back.
//
// This file includes <3ds.h> for KEY_* constants and gfx/sprite/font for drawing.
// The pure logic it drives lives in app/remap.h and stays host-testable.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/remap.h"

// Initialise the remap UI state. Call once before the first remapUiUpdate.
void remapUiInit(void);

// One frame of input and drawing. `down` is hidKeysDown(), `touch_down/touch_x/y`
// are the bottom-screen touch state. Returns true while the remap screen is active;
// false means the player pressed B and wants to go back to the pause menu.
//
// `rs` is modified in place: captures, swaps, and resets all happen here.
bool remapUiUpdate(RemapState* rs, uint32_t down,
                   bool touch_down, int touch_x, int touch_y);
