#pragma once

// Draws the loading screen scene/loading.c drives. Split from loading.c for the same reason
// scene/ui_layout.c is split from scene/ui.c: this half reaches for gfx/sprite.h and
// gfx/font.h, and the half that decides what to say must stay linkable on the host.
//
// Same contract as scene/title.c — the caller has already bound the target and cleared it,
// this calls spriteBegin/spriteEnd itself, and it never touches HID. Which key does what is
// main.c's business; this only paints, and answers where the two stall-panel buttons are so
// the same rectangles cannot drift between the drawing and the hit test.

#include "scene/loading.h"

// What the player has to be told, and the two rectangles that offer it. Only drawn when the
// phase is LOADING_STALLED — a boot that is merely slow is offered nothing to press, because
// there is nothing useful to decide yet.
typedef enum {
	LOADING_ACTION_NONE = 0,
	LOADING_ACTION_PLAY,   // go in with whatever loaded; the world may have holes in it
	LOADING_ACTION_QUIT,   // give up and shut the game down cleanly
} LoadingAction;

// `heading` is the line at the top — "CREATING WORLD" for a world with nothing on the card
// yet, "LOADING WORLD" otherwise — and `world_name` is drawn under it.
void loadingDraw(const LoadingState* st, const char* heading, const char* world_name);

// Which stall-panel button a touch at (tx, ty) landed on, or LOADING_ACTION_NONE. Returns
// NONE for any phase other than LOADING_STALLED, so a stray tap during a healthy boot cannot
// skip the wait.
LoadingAction loadingHitAction(const LoadingState* st, int tx, int ty);
