// Step 8.4 (console-side wiring). The one place the game asks "which button is this verb on"
// and "how fast does the stick look", so app/options.h's settings actually reach the code
// that reads input.
//
// Why a module and not a parameter. The three readers — scene/player.c, scene/interact.c and
// scene/camera.c — each call hidKeysHeld()/hidKeysDown()/hidCircleRead() themselves rather
// than being handed a frame's input, and their update functions take exactly the arguments
// they need for their own job. Threading an Options* through playerUpdate, interactUpdate and
// cameraLook would change three public signatures and every call site in main.c, to deliver a
// value that never changes during play, to code that has no decision to make about it. A
// settings snapshot with accessors is smaller, and it puts the "these hex bits are libctru
// KEY_* bits" fact in one file instead of three.
//
// Pure C, no <3ds.h>. It deals in uint32_t key bits, which is what app/options.h already
// stores and what main.c static-asserts against the real KEY_* constants at boot. That keeps
// this file host-compilable for the same reason options.c is.
//
// Before inputMapSet() is called, every accessor answers exactly what optionsDefaults() would
// give — the bindings that were hardcoded in scene/player.h and scene/interact.h before this
// step, and a look scale of 1.0 with no invert. So a build that never loads options (or one
// where the load failed outright) plays precisely as it did before step 8.4 rather than with
// no controls at all, which is the failure mode a zeroed binding table would produce.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/options.h"

// Copies `o` into the module's snapshot. Call once at boot, after optionsLoad and after the
// title screen has finished editing it — the settings are read every frame from then on and
// nothing re-reads the file. NULL restores the defaults described above.
//
// A snapshot, not a stored pointer: main.c's Options lives on main()'s stack, and a pointer
// to it read from scene/ code would be a dangling read the moment anything in this program
// grows a second entry point.
void inputMapSet(const Options* o);

// v1.9.1. While a modal menu owns the pad, every action answers 0 — see inputKey below.
//
// This is the guard, rather than a playerSetInputEnabled/cameraSetInputEnabled pair, because
// the three readers named at the top of this file each ask for their own verbs by calling
// inputKey. Zeroing the answer here disables move, jump, break, place and eat in one place;
// adding an enable flag to each reader would be three new APIs, three call sites in main.c,
// and three chances for one of them to be missed and leave the player walking around behind
// an open menu.
//
// Deliberately NOT sticky across a mode change: main.c sets it every frame from the current
// screen, so a menu that closes without an explicit false still returns control on the very
// next frame. A latched flag would leave the pad dead until something remembered to clear it.
void inputMapSetMenuOwnsPad(bool owns);

// The raw key bit bound to `a`, ready to be tested against hidKeysHeld()/hidKeysDown(). An
// action outside the enum answers 0, which tests false against every key state — a verb that
// does nothing rather than a verb that fires on every button.
//
// Answers 0 for every action while inputMapSetMenuOwnsPad(true) is in force. Note this gates
// the *bound verbs* only: main.c reads raw KEY_ bits directly in a few places (the pause
// button, the render-distance L/R lines) and those are unaffected, by design — a menu that
// swallowed the pause button could not be escaped.
uint32_t inputKey(OptionsAction a);

// Multiplier on scene/camera.c's LOOK_SPEED. 1.0 is the pre-8.4 feel exactly.
//
// v1.9.1: answers 0 while inputMapSetMenuOwnsPad(true) is in force, which freezes the camera
// for as long as a modal menu is up. That is deliberate and is the ONLY thing stopping the
// circle pad from turning the player while it also walks a menu cursor: cameraLook reads the
// pad directly rather than through inputKey, so the action gate above cannot reach it. See the
// long note at the definition in input_map.c for what was rejected in its place.
float inputLookScale(void);

// True when the player has asked for inverted pitch: stick up looks down.
bool inputInvertLook(void);
